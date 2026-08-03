// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_input.h"

#include "sak/win32mcp/win32_mcp_capture.h"
#include "sak/win32mcp/win32_mcp_input_plan.h"
#include "sak/win32mcp/win32_mcp_key_chord.h"
#include "sak/win32mcp/win32_mcp_ocr.h"

#include <QJsonDocument>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace sak::win32mcp {

namespace {

// A model-supplied string cannot be allowed to inject an unbounded keystroke stream.
constexpr int kMaxTypeChars = 10'000;

// -- result + schema helpers (module-local, mirroring win32_mcp_tools) -------

ToolResult jsonResult(const QJsonObject& object) {
    return {QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)), false};
}

ToolResult errorResult(const QString& message) {
    return {QJsonDocument(QJsonObject{{QStringLiteral("error"), message}})
                .toJson(QJsonDocument::Compact),
            true};
}

QJsonObject stringProperty(const QString& description) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                       {QStringLiteral("description"), description}};
}

QJsonObject intProperty(const QString& description) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                       {QStringLiteral("description"), description}};
}

QJsonObject boolProperty(const QString& description) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")},
                       {QStringLiteral("description"), description}};
}

QJsonObject toolSchema(const QJsonObject& properties, const QJsonArray& required) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"), required},
                       {QStringLiteral("additionalProperties"), false}};
}

QJsonObject toolEntry(const QString& name, const QString& description, const QJsonObject& schema) {
    return QJsonObject{{QStringLiteral("name"), name},
                       {QStringLiteral("description"), description},
                       {QStringLiteral("inputSchema"), schema}};
}

// -- SendInput primitives ----------------------------------------------------

ScreenBox virtualScreen() {
    return ScreenBox{GetSystemMetrics(SM_XVIRTUALSCREEN),
                     GetSystemMetrics(SM_YVIRTUALSCREEN),
                     GetSystemMetrics(SM_CXVIRTUALSCREEN),
                     GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

INPUT mouseInput(DWORD flags, LONG nx, LONG ny) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.dx = nx;
    in.mi.dy = ny;
    return in;
}

// Map a mouse button-down flag to its matching button-up flag; 0 for a non-button event (move).
DWORD mouseDownToUp(DWORD down_flags) {
    if ((down_flags & MOUSEEVENTF_LEFTDOWN) != 0u) {
        return MOUSEEVENTF_LEFTUP;
    }
    if ((down_flags & MOUSEEVENTF_RIGHTDOWN) != 0u) {
        return MOUSEEVENTF_RIGHTUP;
    }
    if ((down_flags & MOUSEEVENTF_MIDDLEDOWN) != 0u) {
        return MOUSEEVENTF_MIDDLEUP;
    }
    if ((down_flags & MOUSEEVENTF_XDOWN) != 0u) {
        return MOUSEEVENTF_XUP;
    }
    return 0u;
}

// Build the key-up / button-up events (in reverse order) that release whatever the first `count`
// events of `seq` pressed down. A redundant up for an already-released key/button is a harmless
// no-op, so this over-releases rather than track exact pairing.
QVector<INPUT> releaseSequenceForPartial(const QVector<INPUT>& seq, UINT count) {
    QVector<INPUT> ups;
    const int n = std::min(static_cast<int>(count), static_cast<int>(seq.size()));
    for (int i = n - 1; i >= 0; --i) {
        const INPUT& in = seq[i];
        if (in.type == INPUT_KEYBOARD && (in.ki.dwFlags & KEYEVENTF_KEYUP) == 0u) {
            INPUT up = in;
            up.ki.dwFlags |= KEYEVENTF_KEYUP;
            ups.append(up);
        } else if (in.type == INPUT_MOUSE) {
            const DWORD up_flag = mouseDownToUp(in.mi.dwFlags);
            if (up_flag != 0u) {
                ups.append(mouseInput(up_flag, 0, 0));
            }
        }
    }
    return ups;
}

// Send an input sequence; true only when EVERY event was accepted. SendInput returns the count it
// actually inserted -- 0 or a short count when a higher-integrity foreground window (UIPI), the
// secure desktop, or BlockInput refuses the injection -- so a mismatch means the input did not land
// and the tool must report failure instead of a false success. On a partial delivery we first
// release any modifier/button the accepted prefix pressed, so a failed chord cannot strand a held
// Ctrl/Shift or mouse button on the desktop.
bool sendInputAll(QVector<INPUT>& seq) {
    if (seq.isEmpty()) {
        return true;
    }
    const UINT total = static_cast<UINT>(seq.size());
    const UINT sent = SendInput(total, seq.data(), sizeof(INPUT));
    if (sent == total) {
        return true;
    }
    QVector<INPUT> ups = releaseSequenceForPartial(seq, sent);
    if (!ups.isEmpty()) {
        SendInput(static_cast<UINT>(ups.size()), ups.data(), sizeof(INPUT));
    }
    return false;
}

INPUT keyUnicode(wchar_t ch, bool key_up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = ch;
    in.ki.dwFlags = KEYEVENTF_UNICODE | (key_up ? KEYEVENTF_KEYUP : 0u);
    return in;
}

INPUT keyVk(WORD vk, bool key_up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = key_up ? KEYEVENTF_KEYUP : 0u;
    return in;
}

// Move the pointer to (x, y) then press+release `clicks` times with the given button flags. Returns
// false when SendInput could not deliver the whole sequence (blocked / elevated target).
bool clickAt(int x, int y, DWORD down, DWORD up, int clicks) {
    const ScreenBox vs = virtualScreen();
    const AbsPoint p = toAbsCoord(x, y, vs);
    QVector<INPUT> seq;
    seq.append(
        mouseInput(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, p.nx, p.ny));
    for (int i = 0; i < clicks; ++i) {
        seq.append(mouseInput(down, 0, 0));
        seq.append(mouseInput(up, 0, 0));
    }
    return sendInputAll(seq);
}

// Standard message when an injection is refused (usually a higher-integrity/elevated foreground
// window, the secure desktop, or an active BlockInput), so the model does not proceed as if the
// input landed.
QString injectionBlockedError() {
    return QStringLiteral(
        "Input was not delivered; the target window may be elevated (higher integrity), on the "
        "secure desktop, or blocking input.");
}

// Bring a top-level window to the foreground so the next input actuates its control instead of
// only raising it. Works around SetForegroundWindow's focus-steal guard by briefly attaching this
// thread's input queue to the current foreground thread. Restores a minimized window. Best-effort.
void activateWindow(HWND hwnd) {
    if (hwnd == nullptr) {
        return;
    }
    if (IsIconic(hwnd) != FALSE) {
        ShowWindow(hwnd, SW_RESTORE);
    }
    const HWND fg = GetForegroundWindow();
    const DWORD this_tid = GetCurrentThreadId();
    const DWORD fg_tid = (fg != nullptr) ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const bool attach = fg_tid != 0 && fg_tid != this_tid;
    if (attach) {
        AttachThreadInput(fg_tid, this_tid, TRUE);
    }
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    if (attach) {
        AttachThreadInput(fg_tid, this_tid, FALSE);
    }
}

// Activate whichever window an input tool is aimed at: foreground=true confirms the active pop-up,
// window_title raises that titled window; neither leaves focus untouched. Gives the OS a moment to
// repaint/focus before the caller clicks or types. Returns an empty string on success (or when no
// target was requested), else a non-empty error: when a target IS named but cannot be raised we
// surface it (like focus_window) so the caller aborts instead of injecting into the wrong window.
QString activateTarget(const QJsonObject& args) {
    WindowRect wr;
    QString err;
    bool ok = false;
    bool requested = false;
    if (args.value(QStringLiteral("foreground")).toBool()) {
        requested = true;
        ok = foregroundWindowRect(wr, err);
    } else {
        const QString title = args.value(QStringLiteral("window_title")).toString().trimmed();
        if (!title.isEmpty()) {
            requested = true;
            ok = windowRectByTitle(title.toLower(), wr, err);
        }
    }
    if (!requested) {
        return {};  // no target named -> leave focus untouched, not an error
    }
    if (!ok) {
        return err.isEmpty() ? QStringLiteral("Could not raise the requested window.") : err;
    }
    activateWindow(static_cast<HWND>(wr.hwnd));
    Sleep(120);
    return {};
}

// Extract a screen coordinate as an EXACT integer: a JSON number with a fractional part or beyond
// int range is rejected (returns false) instead of being truncated / collapsed to a real
// coordinate by QJsonValue::toInt().
bool exactScreenInt(const QJsonValue& value, int& out) {
    const double d = value.toDouble();
    if (std::floor(d) != d) {
        return false;
    }
    if (d < static_cast<double>(std::numeric_limits<int>::min()) ||
        d > static_cast<double>(std::numeric_limits<int>::max())) {
        return false;
    }
    out = static_cast<int>(d);
    return true;
}

// Validate and extract an in-bounds virtual-screen click point. Fails closed on a missing,
// non-numeric, fractional, out-of-range, or off-screen coordinate: a fractional/out-of-range
// number must NOT be truncated or collapsed to 0 (a coerced 0 could pass the bounds check and
// fire at the desktop corner), and an off-screen point would be OS-clamped to a wrong control.
QString resolveClickPoint(const QJsonObject& args, int& x, int& y) {
    if (!args.contains(QStringLiteral("x")) || !args.contains(QStringLiteral("y"))) {
        return QStringLiteral("x and y are required (virtual-screen coordinates)");
    }
    const QJsonValue xv = args.value(QStringLiteral("x"));
    const QJsonValue yv = args.value(QStringLiteral("y"));
    if (!xv.isDouble() || !yv.isDouble()) {
        return QStringLiteral("x and y must be numbers (virtual-screen coordinates)");
    }
    if (!exactScreenInt(xv, x) || !exactScreenInt(yv, y)) {
        return QStringLiteral("x and y must be whole numbers within the coordinate range.");
    }
    const ScreenBox vs = virtualScreen();
    if (!pointInVirtualScreen(x, y, vs)) {
        return QStringLiteral("(%1, %2) is outside the virtual screen [%3,%4)+%5x%6.")
            .arg(x)
            .arg(y)
            .arg(vs.x)
            .arg(vs.y)
            .arg(vs.w)
            .arg(vs.h);
    }
    return {};
}

// -- tools -------------------------------------------------------------------

ToolResult toolMouseClick(const QJsonObject& args) {
    int x = 0;
    int y = 0;
    const QString point_err = resolveClickPoint(args, x, y);
    if (!point_err.isEmpty()) {
        return errorResult(point_err);
    }
    unsigned long down_flags = 0;
    unsigned long up_flags = 0;
    if (!mouseButtonFlags(args.value(QStringLiteral("button")).toString(), down_flags, up_flags)) {
        return errorResult(QStringLiteral("button must be left, right, or middle"));
    }
    // optional window_title / foreground raises the target first; abort if a named target could
    // not be raised rather than clicking whatever happens to be focused.
    const QString activate_err = activateTarget(args);
    if (!activate_err.isEmpty()) {
        return errorResult(activate_err);
    }
    const int clicks = args.value(QStringLiteral("double")).toBool() ? 2 : 1;
    if (!clickAt(x, y, static_cast<DWORD>(down_flags), static_cast<DWORD>(up_flags), clicks)) {
        return errorResult(injectionBlockedError());
    }
    return jsonResult(QJsonObject{{QStringLiteral("ok"), true},
                                  {QStringLiteral("x"), x},
                                  {QStringLiteral("y"), y},
                                  {QStringLiteral("clicks"), clicks}});
}

ToolResult toolClickText(const QJsonObject& args) {
    const QString text = args.value(QStringLiteral("text")).toString().trimmed();
    if (text.isEmpty()) {
        return errorResult(QStringLiteral("text is required"));
    }
    // raise the target window so the click actuates, not just activates; abort if a named target
    // could not be raised rather than OCR-clicking the wrong window.
    const QString activate_err = activateTarget(args);
    if (!activate_err.isEmpty()) {
        return errorResult(activate_err);
    }
    int cx = 0;
    int cy = 0;
    const QString err = ocrLocateText(args, cx, cy);
    if (!err.isEmpty()) {
        return errorResult(err);
    }
    const int clicks = args.value(QStringLiteral("double")).toBool() ? 2 : 1;
    if (!clickAt(cx, cy, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, clicks)) {
        return errorResult(injectionBlockedError());
    }
    return jsonResult(QJsonObject{{QStringLiteral("ok"), true},
                                  {QStringLiteral("text"), text},
                                  {QStringLiteral("x"), cx},
                                  {QStringLiteral("y"), cy},
                                  {QStringLiteral("clicks"), clicks}});
}

ToolResult toolTypeText(const QJsonObject& args) {
    const QJsonValue text_value = args.value(QStringLiteral("text"));
    if (!args.contains(QStringLiteral("text"))) {
        return errorResult(QStringLiteral("text is required"));
    }
    // Reject a non-string text rather than coercing it to "" (which sendInputAll would treat as a
    // no-op success reporting typed:0); a wrong-typed argument is caller error, not "typed
    // nothing".
    if (!text_value.isString()) {
        return errorResult(QStringLiteral("text must be a string"));
    }
    const QString text = text_value.toString();
    if (text.size() > kMaxTypeChars) {
        return errorResult(QStringLiteral("text exceeds the type limit"));
    }
    const QVector<KeyStroke> strokes = planTypeText(text);
    QVector<INPUT> seq;
    seq.reserve(strokes.size());
    for (const KeyStroke& stroke : strokes) {
        seq.append(stroke.is_vk ? keyVk(static_cast<WORD>(stroke.code), stroke.key_up)
                                : keyUnicode(static_cast<wchar_t>(stroke.code), stroke.key_up));
    }
    if (!sendInputAll(seq)) {
        return errorResult(injectionBlockedError());
    }
    // Each typed unit is a down+up pair; report units actually sent (excludes dropped CRs), not the
    // raw text length, so the count is honest.
    return jsonResult(
        QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("typed"), strokes.size() / 2}});
}

ToolResult toolSendKeys(const QJsonObject& args) {
    const QString keys = args.value(QStringLiteral("keys")).toString().trimmed();
    if (keys.isEmpty()) {
        return errorResult(QStringLiteral("keys is required, e.g. 'Enter' or 'Ctrl+S'"));
    }
    QVector<WORD> modifiers;
    WORD main_key = 0;
    if (!parseKeyChord(keys, modifiers, main_key)) {
        return errorResult(QStringLiteral("Unrecognized key combination: %1").arg(keys));
    }
    QVector<INPUT> seq;
    for (const WORD mod : modifiers) {
        seq.append(keyVk(mod, false));
    }
    seq.append(keyVk(main_key, false));
    seq.append(keyVk(main_key, true));
    for (int i = modifiers.size() - 1; i >= 0; --i) {
        seq.append(keyVk(modifiers[i], true));
    }
    if (!sendInputAll(seq)) {
        return errorResult(injectionBlockedError());
    }
    return jsonResult(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("keys"), keys}});
}

ToolResult toolFocusWindow(const QJsonObject& args) {
    const bool fg = args.value(QStringLiteral("foreground")).toBool();
    const QString title = args.value(QStringLiteral("window_title")).toString().trimmed();
    if (!fg && title.isEmpty()) {
        return errorResult(QStringLiteral("window_title or foreground is required"));
    }
    WindowRect wr;
    QString err;
    const bool ok = fg ? foregroundWindowRect(wr, err)
                       : windowRectByTitle(title.toLower(), wr, err);
    if (!ok) {
        return errorResult(err);
    }
    activateWindow(static_cast<HWND>(wr.hwnd));
    Sleep(120);
    return jsonResult(
        QJsonObject{{QStringLiteral("ok"), true},
                    {QStringLiteral("window"), fg ? QStringLiteral("<foreground>") : title}});
}

// -- catalog + dispatch ------------------------------------------------------

void appendInputTools(QJsonArray& tools) {
    tools.append(toolEntry(
        QStringLiteral("mouse_click"),
        QStringLiteral("Move the physical mouse to virtual-screen (x, y) and click. button = "
                       "left (default) / right / middle; double = true for a double-click. Pass "
                       "window_title or foreground=true to raise that window before clicking."),
        toolSchema(QJsonObject{{QStringLiteral("x"), intProperty(QStringLiteral("Screen X."))},
                               {QStringLiteral("y"), intProperty(QStringLiteral("Screen Y."))},
                               {QStringLiteral("button"),
                                stringProperty(QStringLiteral("left / right / middle."))},
                               {QStringLiteral("window_title"),
                                stringProperty(QStringLiteral("Optional window to raise first."))},
                               {QStringLiteral("foreground"),
                                boolProperty(QStringLiteral("Raise the active window first."))},
                               {QStringLiteral("double"),
                                boolProperty(QStringLiteral("Double-click when true."))}},
                   QJsonArray{QStringLiteral("x"), QStringLiteral("y")})));
    tools.append(toolEntry(
        QStringLiteral("click_text"),
        QStringLiteral(
            "Find on-screen text with OCR and click its center with the physical mouse. "
            "Scope with window_title, or foreground=true to hit the active pop-up dialog "
            "(which often has no title); the target window is raised first so the click "
            "actuates. Matches whole words and prefers a standalone label (a button) "
            "over the same word inside a sentence. Prefer uia_click_control when the "
            "target has a ref."),
        toolSchema(QJsonObject{{QStringLiteral("text"),
                                stringProperty(QStringLiteral("Visible label to click (whole "
                                                              "words, case-insensitive)."))},
                               {QStringLiteral("window_title"),
                                stringProperty(QStringLiteral("Optional window to scope to."))},
                               {QStringLiteral("foreground"),
                                boolProperty(QStringLiteral("Target the active window (title-less "
                                                            "pop-ups). Overrides window_title."))},
                               {QStringLiteral("contains"),
                                boolProperty(QStringLiteral("Allow substring matches too. Default "
                                                            "false (whole-word only)."))},
                               {QStringLiteral("double"),
                                boolProperty(QStringLiteral("Double-click when true."))}},
                   QJsonArray{QStringLiteral("text")})));
    tools.append(toolEntry(
        QStringLiteral("type_text"),
        QStringLiteral("Type Unicode text into the focused control via the keyboard (newlines "
                       "become Enter). Focus the target first (click it or uia_click_control)."),
        toolSchema(QJsonObject{{QStringLiteral("text"),
                                stringProperty(QStringLiteral("Text to type."))}},
                   QJsonArray{QStringLiteral("text")})));
    tools.append(toolEntry(
        QStringLiteral("send_keys"),
        QStringLiteral("Press a key or chord, e.g. 'Enter', 'Tab', 'F5', 'Ctrl+S', "
                       "'Ctrl+Shift+Escape'. Modifiers: Ctrl/Alt/Shift/Win. Goes to the focused "
                       "window -- focus_window first to aim it."),
        toolSchema(QJsonObject{{QStringLiteral("keys"),
                                stringProperty(QStringLiteral("A key or modifier+key chord."))}},
                   QJsonArray{QStringLiteral("keys")})));
    tools.append(toolEntry(
        QStringLiteral("focus_window"),
        QStringLiteral("Bring a window to the foreground so keyboard input (type_text / send_keys) "
                       "lands in it. Give window_title, or foreground=true to confirm the active "
                       "pop-up. Use before typing when no click precedes it."),
        toolSchema(QJsonObject{{QStringLiteral("window_title"),
                                stringProperty(QStringLiteral("Title substring to raise."))},
                               {QStringLiteral("foreground"),
                                boolProperty(QStringLiteral("Raise the active window instead."))}},
                   QJsonArray{})));
}

struct InputHandler {
    QLatin1String name;
    ToolResult (*fn)(const QJsonObject&);
};

const InputHandler kInputHandlers[] = {
    {QLatin1String("mouse_click"), toolMouseClick},
    {QLatin1String("click_text"), toolClickText},
    {QLatin1String("type_text"), toolTypeText},
    {QLatin1String("send_keys"), toolSendKeys},
    {QLatin1String("focus_window"), toolFocusWindow},
};

}  // namespace

QJsonArray inputToolCatalog() {
    QJsonArray tools;
    appendInputTools(tools);
    return tools;
}

bool inputHandles(const QString& name) {
    for (const auto& entry : kInputHandlers) {
        if (name == entry.name) {
            return true;
        }
    }
    return false;
}

ToolResult invokeInputTool(const QString& name, const QJsonObject& args) {
    for (const auto& entry : kInputHandlers) {
        if (name == entry.name) {
            return entry.fn(args);
        }
    }
    return errorResult(QStringLiteral("Unknown input tool: %1").arg(name));
}

}  // namespace sak::win32mcp

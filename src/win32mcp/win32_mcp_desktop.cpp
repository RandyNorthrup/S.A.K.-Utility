// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_desktop.h"

#include "sak/win32mcp/win32_mcp_capture.h"
#include "sak/win32mcp/win32_mcp_dialog_choice.h"
#include "sak/win32mcp/win32_mcp_uia_ref.h"

#include <QBuffer>
#include <QByteArray>
#include <QImage>
#include <QJsonDocument>
#include <QStringList>
#include <QVector>

#include <algorithm>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// WIN32_LEAN_AND_MEAN drops the core COM/OLE headers windows.h would otherwise pull, but
// <uiautomation.h> needs the `interface` keyword + COM base types (objbase) and BSTR/VARIANT
// helpers (oleauto) to declare its provider interfaces; include them explicitly first.
#include <objbase.h>
#include <oleauto.h>
#include <uiautomation.h>
#include <wrl/client.h>

namespace sak::win32mcp {

namespace {

// -- result + schema helpers (module-local, mirroring win32_mcp_tools) -------

ToolResult jsonResult(const QJsonObject& object) {
    return {QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)), false};
}

ToolResult errorResult(const QString& message) {
    return {QJsonDocument(QJsonObject{{QStringLiteral("error"), message}})
                .toJson(QJsonDocument::Compact),
            true};
}

ToolResult imageResult(const QString& base64, const QString& summary) {
    ToolResult result;
    result.text = summary;
    result.is_error = false;
    result.image_base64 = base64;
    result.image_mime = QStringLiteral("image/png");
    return result;
}

QJsonObject stringProperty(const QString& description) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                       {QStringLiteral("description"), description}};
}

QJsonObject typedProperty(const QString& type, const QString& description) {
    return QJsonObject{{QStringLiteral("type"), type},
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

// -- window lookup -----------------------------------------------------------

QString windowTitleOf(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) {
        return {};
    }
    QVector<wchar_t> buffer(length + 1, L'\0');
    const int copied = GetWindowTextW(hwnd, buffer.data(), length + 1);
    return QString::fromWCharArray(buffer.data(), copied);
}

struct WindowMatch {
    HWND hwnd;
    QString title_lower;
};

struct FindState {
    QString needle_lower;
    QVector<WindowMatch> matches;
};

BOOL CALLBACK collectMatchProc(HWND hwnd, LPARAM param) {
    auto* find = reinterpret_cast<FindState*>(param);
    if (IsWindowVisible(hwnd) == FALSE) {
        return TRUE;
    }
    const QString title = windowTitleOf(hwnd);
    const QString lower = title.toLower();
    if (title.isEmpty() || !lower.contains(find->needle_lower)) {
        return TRUE;
    }
    find->matches.append(WindowMatch{hwnd, lower});
    return TRUE;  // collect every match, then disambiguate rather than taking the first
}

// Pick the single window a title substring unambiguously names. One match wins; when several
// match, a lone EXACT (case-insensitive) title match wins; otherwise fail closed rather than
// silently inspecting/driving whichever window enumerated first.
HWND pickUniqueWindow(const QVector<WindowMatch>& matches,
                      const QString& needle_lower,
                      QString& err) {
    if (matches.isEmpty()) {
        err = QStringLiteral("No visible window matching '%1'").arg(needle_lower);
        return nullptr;
    }
    if (matches.size() == 1) {
        return matches.first().hwnd;
    }
    HWND exact = nullptr;
    int exact_count = 0;
    for (const WindowMatch& m : matches) {
        if (m.title_lower == needle_lower) {
            exact = m.hwnd;
            ++exact_count;
        }
    }
    if (exact_count == 1) {
        return exact;
    }
    err = QStringLiteral("'%1' matches %2 visible windows; use a more specific title.")
              .arg(needle_lower)
              .arg(matches.size());
    return nullptr;
}

HWND findVisibleWindowByTitle(const QString& needle_lower, QString& err) {
    FindState state{needle_lower, {}};
    EnumWindows(collectMatchProc, reinterpret_cast<LPARAM>(&state));
    return pickUniqueWindow(state.matches, needle_lower, err);
}

// Resolve the window an observation tool targets: foreground=true -> the active window (the only
// way to reach a title-less pop-up dialog), otherwise the first visible window whose title
// contains window_title. Sets err and returns null on failure.
HWND resolveTargetHwnd(const QJsonObject& args, QString& err) {
    if (args.value(QStringLiteral("foreground")).toBool()) {
        HWND fg = GetForegroundWindow();
        if (!fg) {
            err = QStringLiteral("There is no foreground window.");
        }
        return fg;
    }
    const QString title = args.value(QStringLiteral("window_title")).toString().trimmed();
    if (title.isEmpty()) {
        err = QStringLiteral("window_title or foreground is required");
        return nullptr;
    }
    return findVisibleWindowByTitle(title.toLower(), err);  // sets err on not-found/ambiguous
}

// -- screen capture ----------------------------------------------------------

// A capture scales with the surface; cap the base64 so a multi-monitor grab cannot flood the
// transport (16 MiB matches the browser screenshot cap). kCaptureFullEdge asks captureBgra for
// full-resolution pixels (no downscale) up to its own hard bound.
constexpr int kMaxCaptureBase64 = 16 * 1024 * 1024;
constexpr int kCaptureFullEdge = 16'384;
constexpr int kRgb32BytesPerPixel = 4;  // Format_RGB32 stores 4 bytes per pixel

// Encode a top-down 32bpp buffer as base64 PNG. Format_RGB32 reads the B,G,R,X bytes a screen
// DIB holds (little-endian) and ignores the unused 4th byte, so an opaque capture round-trips.
QString encodePng(const uchar* bits, int width, int height) {
    const QImage image(bits, width, height, width * kRgb32BytesPerPixel, QImage::Format_RGB32);
    if (image.isNull()) {
        return {};
    }
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) {
        return {};
    }
    return QString::fromLatin1(png.toBase64());
}

// Capture a window (hwnd non-null -> PrintWindow) or screen rect to a base64 PNG via the shared
// grab primitive. Returns empty and sets *err on failure or an over-cap result.
QString captureToBase64(void* hwnd, const RECT& rect, QString* err) {
    CaptureBits bits;
    const CaptureRequest req{hwnd, rect.left, rect.top, rect.right, rect.bottom, kCaptureFullEdge};
    if (!captureBgra(req, bits, *err)) {
        return {};
    }
    const QString base64 =
        encodePng(reinterpret_cast<const uchar*>(bits.bgra.constData()), bits.width, bits.height);
    if (base64.isEmpty()) {
        *err = QStringLiteral("Failed to encode the capture as PNG.");
        return {};
    }
    if (base64.size() > kMaxCaptureBase64) {
        *err = QStringLiteral("The capture is too large to return; target one window/monitor.");
        return {};
    }
    return base64;
}

ToolResult toolCaptureWindow(const QJsonObject& args) {
    QString err_target;
    HWND hwnd = resolveTargetHwnd(args, err_target);
    if (!hwnd) {
        return errorResult(err_target);
    }
    if (IsIconic(hwnd)) {
        return errorResult(QStringLiteral("The window is minimized; restore it before capturing."));
    }
    RECT rc{};
    if (GetWindowRect(hwnd, &rc) == FALSE) {
        return errorResult(QStringLiteral("Could not read the window bounds."));
    }
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    QString err;
    const QString b64 = captureToBase64(hwnd, rc, &err);
    if (b64.isEmpty()) {
        return errorResult(err);
    }
    return imageResult(b64,
                       QStringLiteral("Captured a %1x%2 PNG of window '%3'.")
                           .arg(w)
                           .arg(h)
                           .arg(windowTitleOf(hwnd)));
}

ToolResult toolCaptureScreen(const QJsonObject&) {
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    QString err;
    const RECT rc{x, y, x + w, y + h};
    const QString b64 = captureToBase64(nullptr, rc, &err);
    if (b64.isEmpty()) {
        return errorResult(err);
    }
    return imageResult(
        b64, QStringLiteral("Captured a %1x%2 PNG of the full virtual screen.").arg(w).arg(h));
}

BOOL CALLBACK collectMonitorRectProc(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    auto* rects = reinterpret_cast<QVector<RECT>*>(param);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) != FALSE) {
        rects->append(info.rcMonitor);
    }
    return TRUE;
}

ToolResult toolCaptureMonitor(const QJsonObject& args) {
    if (!args.contains(QStringLiteral("index"))) {
        return errorResult(QStringLiteral("index is required (see list_monitors)"));
    }
    const int index = args.value(QStringLiteral("index")).toInt(-1);
    QVector<RECT> rects;
    EnumDisplayMonitors(nullptr, nullptr, collectMonitorRectProc, reinterpret_cast<LPARAM>(&rects));
    if (index < 0 || index >= rects.size()) {
        return errorResult(
            QStringLiteral("No monitor at index %1 (there are %2).").arg(index).arg(rects.size()));
    }
    const RECT rc = rects[index];
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    QString err;
    const QString b64 = captureToBase64(nullptr, rc, &err);
    if (b64.isEmpty()) {
        return errorResult(err);
    }
    return imageResult(
        b64, QStringLiteral("Captured a %1x%2 PNG of monitor %3.").arg(w).arg(h).arg(index));
}

// -- UI Automation (read) ----------------------------------------------------
//
// A read-only view of a window's live control tree via IUIAutomation: an outline the model
// reads (mirroring browser_snapshot's role/name/value + ref model) plus targeted lookups.
// Refs are the depth-first index into the last inspected tree and are valid only against THAT
// window; a ref action re-walks the same window and refuses if the tree drifted, so a stale
// ref can never read the wrong control. No input is injected here -- clicking/invoking a
// control is a separate hard-gated input tool in a later batch.

using Microsoft::WRL::ComPtr;

// One control-tree node, flattened depth-first. `role` is a friendly control-type name. left/top
// are the element's screen-space bounding-rectangle origin, kept as part of the ref-drift identity
// so two same-role, same-(or empty-)name controls that swap position cannot pass the guard.
struct UiaNode {
    QString role;
    QString name;
    QString value;
    int depth{0};
    bool enabled{true};
    bool offscreen{false};
    long left{0};
    long top{0};
};

// Bound the walk so a pathological tree (thousands of list items) cannot flood the transport
// or spin unbounded; kDefaultUiaDepth also caps a caller-supplied max_depth.
constexpr int kMaxUiaNodes = 400;
constexpr int kDefaultUiaDepth = 40;
constexpr int kMaxUiaNameChars = 200;
constexpr int kMaxUiaValueChars = 400;
constexpr int kMaxIndentDepth = 20;

// COM apartment scoped to one tool call. UIA lives in the MTA; tolerate a thread that is
// already an STA (CoInitializeEx returns RPC_E_CHANGED_MODE) by not un-initializing what we
// did not initialize -- the existing apartment still services CoCreateInstance.
class ComApartment {
public:
    ComApartment() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owned_ = (hr == S_OK || hr == S_FALSE);
    }
    ~ComApartment() {
        if (owned_) {
            CoUninitialize();
        }
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    bool owned_{false};
};

QString bstrToQString(BSTR text) {
    if (text == nullptr) {
        return {};
    }
    return QString::fromWCharArray(text, static_cast<int>(SysStringLen(text)));
}

// Map the common UIA control-type ids to short, stable, lower-case names. Anything unlisted
// reports "control" so the outline is still well-formed.
const char* controlTypeName(CONTROLTYPEID id) {
    struct Row {
        int id;
        const char* name;
    };
    static const Row kRows[] = {
        {UIA_ButtonControlTypeId, "button"},
        {UIA_CheckBoxControlTypeId, "checkbox"},
        {UIA_ComboBoxControlTypeId, "combobox"},
        {UIA_EditControlTypeId, "edit"},
        {UIA_HyperlinkControlTypeId, "link"},
        {UIA_ImageControlTypeId, "image"},
        {UIA_ListItemControlTypeId, "listitem"},
        {UIA_ListControlTypeId, "list"},
        {UIA_MenuControlTypeId, "menu"},
        {UIA_MenuBarControlTypeId, "menubar"},
        {UIA_MenuItemControlTypeId, "menuitem"},
        {UIA_ProgressBarControlTypeId, "progressbar"},
        {UIA_RadioButtonControlTypeId, "radio"},
        {UIA_ScrollBarControlTypeId, "scrollbar"},
        {UIA_SliderControlTypeId, "slider"},
        {UIA_SpinnerControlTypeId, "spinner"},
        {UIA_StatusBarControlTypeId, "statusbar"},
        {UIA_TabControlTypeId, "tab"},
        {UIA_TabItemControlTypeId, "tabitem"},
        {UIA_TextControlTypeId, "text"},
        {UIA_ToolBarControlTypeId, "toolbar"},
        {UIA_ToolTipControlTypeId, "tooltip"},
        {UIA_TreeControlTypeId, "tree"},
        {UIA_TreeItemControlTypeId, "treeitem"},
        {UIA_GroupControlTypeId, "group"},
        {UIA_DataGridControlTypeId, "datagrid"},
        {UIA_DataItemControlTypeId, "dataitem"},
        {UIA_DocumentControlTypeId, "document"},
        {UIA_SplitButtonControlTypeId, "splitbutton"},
        {UIA_WindowControlTypeId, "window"},
        {UIA_PaneControlTypeId, "pane"},
        {UIA_HeaderControlTypeId, "header"},
        {UIA_HeaderItemControlTypeId, "headeritem"},
        {UIA_TableControlTypeId, "table"},
        {UIA_TitleBarControlTypeId, "titlebar"},
        {UIA_SeparatorControlTypeId, "separator"},
    };
    for (const auto& row : kRows) {
        if (row.id == id) {
            return row.name;
        }
    }
    return "control";
}

// Read a control's ValuePattern text, if it exposes one and it is non-empty.
QString readValuePattern(IUIAutomationElement* element) {
    ComPtr<IUnknown> unknown;
    if (FAILED(element->GetCurrentPattern(UIA_ValuePatternId, &unknown)) || !unknown) {
        return {};
    }
    ComPtr<IUIAutomationValuePattern> pattern;
    if (FAILED(unknown.As(&pattern)) || !pattern) {
        return {};
    }
    BSTR value = nullptr;
    if (FAILED(pattern->get_CurrentValue(&value)) || value == nullptr) {
        return {};
    }
    const QString out = bstrToQString(value);
    SysFreeString(value);
    return out;
}

// Read a control's TogglePattern state as on/off/indeterminate, if it exposes one.
QString readToggleState(IUIAutomationElement* element) {
    ComPtr<IUnknown> unknown;
    if (FAILED(element->GetCurrentPattern(UIA_TogglePatternId, &unknown)) || !unknown) {
        return {};
    }
    ComPtr<IUIAutomationTogglePattern> pattern;
    if (FAILED(unknown.As(&pattern)) || !pattern) {
        return {};
    }
    ToggleState state = ToggleState_Off;
    if (FAILED(pattern->get_CurrentToggleState(&state))) {
        return {};
    }
    if (state == ToggleState_On) {
        return QStringLiteral("on");
    }
    if (state == ToggleState_Indeterminate) {
        return QStringLiteral("indeterminate");
    }
    return QStringLiteral("off");
}

// A control's most useful single value: its editable/selected text if any, else its toggle
// state. Empty when the control carries neither (a plain button, label, or group).
QString readElementValue(IUIAutomationElement* element) {
    const QString value = readValuePattern(element);
    if (!value.isEmpty()) {
        return value;
    }
    return readToggleState(element);
}

UiaNode describeElement(IUIAutomationElement* element, int depth) {
    UiaNode node;
    node.depth = depth;
    CONTROLTYPEID control_type = 0;
    element->get_CurrentControlType(&control_type);
    node.role = QString::fromLatin1(controlTypeName(control_type));
    BSTR name = nullptr;
    if (SUCCEEDED(element->get_CurrentName(&name)) && name != nullptr) {
        node.name = bstrToQString(name).trimmed().left(kMaxUiaNameChars);
        SysFreeString(name);
    }
    BOOL enabled = TRUE;
    if (FAILED(element->get_CurrentIsEnabled(&enabled))) {
        enabled = FALSE;  // fail closed: an unreadable enabled state must not read as enabled --
                          // it feeds collectButtons/dismiss_dialog, which must not invoke it.
    }
    node.enabled = enabled != FALSE;
    BOOL offscreen = FALSE;
    if (FAILED(element->get_CurrentIsOffscreen(&offscreen))) {
        offscreen = TRUE;  // fail closed: an unreadable visibility must not read as on-screen.
    }
    node.offscreen = offscreen != FALSE;
    RECT bounds{};
    if (SUCCEEDED(element->get_CurrentBoundingRectangle(&bounds))) {
        node.left = bounds.left;
        node.top = bounds.top;
    }
    node.value = readElementValue(element).left(kMaxUiaValueChars);
    return node;
}

struct WalkState {
    QVector<UiaNode>* nodes;
    IUIAutomationTreeWalker* walker;
    int max_depth;
    bool truncated;
    // Optional parallel list of the live elements (same index as nodes), retained so a ref can be
    // invoked. Null when only the read-only outline is needed (inspect/find/get_value).
    QVector<ComPtr<IUIAutomationElement>>* elements;
};

// Depth-first pre-order walk of the control view, appending each element as a flat node. The
// node index becomes its ref. Stops (and flags truncated) once kMaxUiaNodes is reached.
void walkElement(WalkState& state, IUIAutomationElement* element, int depth) {
    if (state.nodes->size() >= kMaxUiaNodes) {
        state.truncated = true;
        return;
    }
    state.nodes->append(describeElement(element, depth));
    if (state.elements != nullptr) {
        state.elements->append(ComPtr<IUIAutomationElement>(element));
    }
    if (depth >= state.max_depth) {
        return;
    }
    ComPtr<IUIAutomationElement> child;
    if (FAILED(state.walker->GetFirstChildElement(element, &child))) {
        state.truncated = true;  // a walk failure leaves the tree incomplete -- flag, don't hide it
        return;
    }
    if (!child) {
        return;  // genuinely no children
    }
    while (child) {
        walkElement(state, child.Get(), depth + 1);
        if (state.nodes->size() >= kMaxUiaNodes) {
            state.truncated = true;
            return;
        }
        ComPtr<IUIAutomationElement> next;
        if (FAILED(state.walker->GetNextSiblingElement(child.Get(), &next))) {
            state.truncated = true;  // sibling read failed -> remaining peers unseen; flag it
            return;
        }
        child = next;
    }
}

// Walk `hwnd`'s control tree into `out` (and, if `elements` is non-null, the live element behind
// each node). Precondition: a ComApartment is live on this thread. Returns an error string on any
// COM failure, empty on success.
QString inspectHwnd(HWND hwnd,
                    int max_depth,
                    QVector<UiaNode>& out,
                    bool& truncated,
                    QVector<ComPtr<IUIAutomationElement>>* elements = nullptr) {
    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(
            __uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))) ||
        !automation) {
        return QStringLiteral("UI Automation is unavailable on this system.");
    }
    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation->ElementFromHandle(hwnd, &root)) || !root) {
        return QStringLiteral("Could not read the window's UI Automation tree.");
    }
    ComPtr<IUIAutomationTreeWalker> walker;
    if (FAILED(automation->get_ControlViewWalker(&walker)) || !walker) {
        return QStringLiteral("The UI Automation control view is unavailable.");
    }
    WalkState state{&out, walker.Get(), max_depth, false, elements};
    walkElement(state, root.Get(), 0);
    truncated = state.truncated;
    return {};
}

// Escape a control's name/value so an embedded quote or newline (which can be attacker-influenced
// window text) cannot break the outline's one-node-per-line, quoted-field shape.
QString escapeOutlineText(const QString& text) {
    QString out = text;
    out.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    out.replace(QLatin1Char('"'), QLatin1String("\\\""));
    out.replace(QLatin1Char('\n'), QLatin1String("\\n"));
    out.replace(QLatin1Char('\r'), QLatin1String("\\r"));
    out.replace(QLatin1Char('\t'), QLatin1String("\\t"));
    return out;
}

QString buildOutline(const QVector<UiaNode>& nodes) {
    QStringList lines;
    lines.reserve(nodes.size());
    for (int i = 0; i < nodes.size(); ++i) {
        const UiaNode& node = nodes[i];
        const QString indent(std::min(node.depth, kMaxIndentDepth) * 2, QLatin1Char(' '));
        QString line = QStringLiteral("%1[%2] %3").arg(indent).arg(i).arg(node.role);
        if (!node.name.isEmpty()) {
            line += QStringLiteral(" \"%1\"").arg(escapeOutlineText(node.name));
        }
        if (!node.value.isEmpty()) {
            line += QStringLiteral(" = \"%1\"").arg(escapeOutlineText(node.value));
        }
        if (!node.enabled) {
            line += QStringLiteral(" (disabled)");
        }
        if (node.offscreen) {
            line += QStringLiteral(" (offscreen)");
        }
        lines << line;
    }
    return lines.join(QLatin1Char('\n'));
}

int clampUiaDepth(const QJsonObject& args) {
    const int requested = args.value(QStringLiteral("max_depth")).toInt(kDefaultUiaDepth);
    if (requested <= 0 || requested > kDefaultUiaDepth) {
        return kDefaultUiaDepth;
    }
    return requested;
}

// The last inspected tree, kept so ref-based lookups can re-walk the same window and verify a
// ref still points at the same control. The native server processes one request at a time, so
// a single module-local snapshot is safe.
struct UiaSnapshot {
    HWND hwnd{nullptr};
    QString title;
    QVector<UiaNode> nodes;
    int depth{kDefaultUiaDepth};  // walk depth used, so a ref re-walk matches the stored indices
};
UiaSnapshot g_uia_snapshot;

// The stable-identity view (role/name/bounds) of a walked tree, for the ref-drift guard.
QVector<UiaRefNode> toRefNodes(const QVector<UiaNode>& nodes) {
    QVector<UiaRefNode> out;
    out.reserve(nodes.size());
    for (const UiaNode& node : nodes) {
        out.append(UiaRefNode{node.role, node.name, node.left, node.top});
    }
    return out;
}

ToolResult toolUiaInspectWindow(const QJsonObject& args) {
    QString err_target;
    HWND hwnd = resolveTargetHwnd(args, err_target);
    if (!hwnd) {
        return errorResult(err_target);
    }
    const ComApartment com;
    QVector<UiaNode> nodes;
    bool truncated = false;
    const int depth = clampUiaDepth(args);
    const QString err = inspectHwnd(hwnd, depth, nodes, truncated);
    if (!err.isEmpty()) {
        return errorResult(err);
    }
    g_uia_snapshot = UiaSnapshot{hwnd, windowTitleOf(hwnd), nodes, depth};
    return jsonResult(
        QJsonObject{{QStringLiteral("window"), windowTitleOf(hwnd)},
                    {QStringLiteral("hwnd"), QString::number(reinterpret_cast<quintptr>(hwnd))},
                    {QStringLiteral("count"), nodes.size()},
                    {QStringLiteral("truncated"), truncated},
                    {QStringLiteral("outline"), buildOutline(nodes)}});
}

ToolResult toolUiaFindControl(const QJsonObject& args) {
    const QString query = args.value(QStringLiteral("name")).toString().trimmed();
    if (query.isEmpty()) {
        return errorResult(QStringLiteral("name is required"));
    }
    QString err_target;
    HWND hwnd = resolveTargetHwnd(args, err_target);
    if (!hwnd) {
        return errorResult(err_target);
    }
    const QString type_filter =
        args.value(QStringLiteral("control_type")).toString().trimmed().toLower();
    const ComApartment com;
    QVector<UiaNode> nodes;
    bool truncated = false;
    const QString err = inspectHwnd(hwnd, kDefaultUiaDepth, nodes, truncated);
    if (!err.isEmpty()) {
        return errorResult(err);
    }
    g_uia_snapshot = UiaSnapshot{hwnd, windowTitleOf(hwnd), nodes};
    const QString needle = query.toLower();
    QJsonArray matches;
    for (int i = 0; i < nodes.size(); ++i) {
        const UiaNode& node = nodes[i];
        if (!node.name.toLower().contains(needle)) {
            continue;
        }
        if (!type_filter.isEmpty() && node.role != type_filter) {
            continue;
        }
        matches.append(QJsonObject{{QStringLiteral("ref"), i},
                                   {QStringLiteral("role"), node.role},
                                   {QStringLiteral("name"), node.name},
                                   {QStringLiteral("value"), node.value},
                                   {QStringLiteral("enabled"), node.enabled}});
    }
    return jsonResult(QJsonObject{{QStringLiteral("window"), windowTitleOf(hwnd)},
                                  {QStringLiteral("count"), matches.size()},
                                  {QStringLiteral("matches"), matches},
                                  {QStringLiteral("tree_truncated"), truncated}});
}

// Guard the stored snapshot before a ref lookup: it must exist and still describe a live
// window with the same title. Returns an error string when stale, empty when usable.
QString uiaSnapshotPrecheck() {
    if (g_uia_snapshot.hwnd == nullptr) {
        return QStringLiteral("No inspection yet; call uia_inspect_window first.");
    }
    if (IsWindow(g_uia_snapshot.hwnd) == FALSE) {
        return QStringLiteral("The inspected window is gone; call uia_inspect_window again.");
    }
    if (windowTitleOf(g_uia_snapshot.hwnd) != g_uia_snapshot.title) {
        return QStringLiteral(
            "The window changed since inspection; call uia_inspect_window again.");
    }
    return {};
}

// True when the ref no longer points at the same control it did at inspection time (the tree
// shifted between calls), so reading it would silently target the wrong element. Delegates the
// comparison to the pure, unit-tested uiaRefDrifted seam over the stored snapshot.
bool snapshotRefDrifted(const QVector<UiaNode>& live, int ref) {
    return uiaRefDrifted(toRefNodes(g_uia_snapshot.nodes), toRefNodes(live), ref);
}

ToolResult toolUiaGetControlValue(const QJsonObject& args) {
    if (!args.contains(QStringLiteral("ref"))) {
        return errorResult(QStringLiteral("ref is required (from uia_inspect_window)"));
    }
    const int ref = args.value(QStringLiteral("ref")).toInt(-1);
    const QString stale = uiaSnapshotPrecheck();
    if (!stale.isEmpty()) {
        return errorResult(stale);
    }
    const ComApartment com;
    QVector<UiaNode> nodes;
    bool truncated = false;
    const QString err = inspectHwnd(g_uia_snapshot.hwnd, g_uia_snapshot.depth, nodes, truncated);
    if (!err.isEmpty()) {
        return errorResult(err);
    }
    if (snapshotRefDrifted(nodes, ref)) {
        return errorResult(
            QStringLiteral("That ref no longer points at the same control; call "
                           "uia_inspect_window again."));
    }
    const UiaNode& node = nodes[ref];
    g_uia_snapshot.nodes = nodes;  // keep the stored tree aligned with what we just read
    return jsonResult(QJsonObject{{QStringLiteral("ref"), ref},
                                  {QStringLiteral("role"), node.role},
                                  {QStringLiteral("name"), node.name},
                                  {QStringLiteral("value"), node.value},
                                  {QStringLiteral("enabled"), node.enabled},
                                  {QStringLiteral("offscreen"), node.offscreen}});
}

// Programmatic activation via UI Automation patterns (no cursor movement): Invoke a button/menu
// item, else Toggle a checkbox, else Select a list/tab item. Each returns true only if the
// control exposes that pattern and the call succeeds.
bool tryInvoke(IUIAutomationElement* element) {
    ComPtr<IUnknown> unknown;
    if (FAILED(element->GetCurrentPattern(UIA_InvokePatternId, &unknown)) || !unknown) {
        return false;
    }
    ComPtr<IUIAutomationInvokePattern> pattern;
    return SUCCEEDED(unknown.As(&pattern)) && pattern && SUCCEEDED(pattern->Invoke());
}

bool tryToggle(IUIAutomationElement* element) {
    ComPtr<IUnknown> unknown;
    if (FAILED(element->GetCurrentPattern(UIA_TogglePatternId, &unknown)) || !unknown) {
        return false;
    }
    ComPtr<IUIAutomationTogglePattern> pattern;
    return SUCCEEDED(unknown.As(&pattern)) && pattern && SUCCEEDED(pattern->Toggle());
}

bool trySelect(IUIAutomationElement* element) {
    ComPtr<IUnknown> unknown;
    if (FAILED(element->GetCurrentPattern(UIA_SelectionItemPatternId, &unknown)) || !unknown) {
        return false;
    }
    ComPtr<IUIAutomationSelectionItemPattern> pattern;
    return SUCCEEDED(unknown.As(&pattern)) && pattern && SUCCEEDED(pattern->Select());
}

QString invokeElement(IUIAutomationElement* element, QString& action) {
    if (tryInvoke(element)) {
        action = QStringLiteral("invoke");
        return {};
    }
    if (tryToggle(element)) {
        action = QStringLiteral("toggle");
        return {};
    }
    if (trySelect(element)) {
        action = QStringLiteral("select");
        return {};
    }
    return QStringLiteral("This control cannot be invoked (no Invoke/Toggle/Select pattern).");
}

ToolResult toolUiaClickControl(const QJsonObject& args) {
    if (!args.contains(QStringLiteral("ref"))) {
        return errorResult(QStringLiteral("ref is required (from uia_inspect_window)"));
    }
    const int ref = args.value(QStringLiteral("ref")).toInt(-1);
    const QString stale = uiaSnapshotPrecheck();
    if (!stale.isEmpty()) {
        return errorResult(stale);
    }
    const ComApartment com;
    QVector<UiaNode> nodes;
    bool truncated = false;
    QVector<ComPtr<IUIAutomationElement>> elements;
    const QString err =
        inspectHwnd(g_uia_snapshot.hwnd, g_uia_snapshot.depth, nodes, truncated, &elements);
    if (!err.isEmpty()) {
        return errorResult(err);
    }
    if (snapshotRefDrifted(nodes, ref) || ref >= elements.size()) {
        return errorResult(
            QStringLiteral("That ref no longer points at the same control; call "
                           "uia_inspect_window again."));
    }
    QString action;
    const QString invoke_err = invokeElement(elements[ref].Get(), action);
    if (!invoke_err.isEmpty()) {
        return errorResult(invoke_err);
    }
    g_uia_snapshot.nodes = nodes;
    const UiaNode& node = nodes[ref];
    return jsonResult(QJsonObject{{QStringLiteral("ok"), true},
                                  {QStringLiteral("ref"), ref},
                                  {QStringLiteral("action"), action},
                                  {QStringLiteral("role"), node.role},
                                  {QStringLiteral("name"), node.name}});
}

QString focusedWindowTitle(IUIAutomationElement* element) {
    UIA_HWND native = nullptr;
    if (FAILED(element->get_CurrentNativeWindowHandle(&native)) || native == nullptr) {
        return {};
    }
    HWND root = GetAncestor(static_cast<HWND>(native), GA_ROOT);
    return windowTitleOf(root != nullptr ? root : static_cast<HWND>(native));
}

ToolResult toolUiaGetFocused(const QJsonObject&) {
    const ComApartment com;
    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(
            __uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))) ||
        !automation) {
        return errorResult(QStringLiteral("UI Automation is unavailable on this system."));
    }
    ComPtr<IUIAutomationElement> element;
    if (FAILED(automation->GetFocusedElement(&element)) || !element) {
        return errorResult(QStringLiteral("No UI element currently has focus."));
    }
    const UiaNode node = describeElement(element.Get(), 0);
    return jsonResult(QJsonObject{{QStringLiteral("role"), node.role},
                                  {QStringLiteral("name"), node.name},
                                  {QStringLiteral("value"), node.value},
                                  {QStringLiteral("enabled"), node.enabled},
                                  {QStringLiteral("window"), focusedWindowTitle(element.Get())}});
}

// -- dialog dismissal --------------------------------------------------------
//
// Close an acknowledgement pop-up by invoking its affirmative button WITHOUT a stored ref -- the
// robust exit for a completion dialog whose OK/Continue button is custom-drawn and nameless (so
// uia_click_control has no stable ref to target). Resolves the window (foreground pop-up or
// title), collects its enabled buttons, and invokes the best affirmative match; a single-button
// dialog is unambiguous and is invoked even when its button carries no accessible name. The
// safety-critical selection (which button wins, or refuse) lives in the pure, unit-tested
// chooseDialogButton (win32_mcp_dialog_choice); here we only gather candidates and invoke.

// Map the walked tree to the button-candidate view and let the pure, unit-tested collectButtons
// seam pick the enabled, on-screen buttons (preserving each node's index as element_index).
QVector<DialogButton> buttonsFromNodes(const QVector<UiaNode>& nodes) {
    QVector<ButtonNode> views;
    views.reserve(nodes.size());
    for (const UiaNode& node : nodes) {
        views.append(ButtonNode{node.role, node.name, node.enabled, node.offscreen});
    }
    return collectButtons(views);
}

ToolResult toolDismissDialog(const QJsonObject& args) {
    QString err_target;
    HWND hwnd = resolveTargetHwnd(args, err_target);
    if (!hwnd) {
        return errorResult(err_target);
    }
    const QString explicit_button = args.value(QStringLiteral("button")).toString().trimmed();
    const ComApartment com;
    QVector<UiaNode> nodes;
    bool truncated = false;
    QVector<ComPtr<IUIAutomationElement>> elements;
    const QString err = inspectHwnd(hwnd, kDefaultUiaDepth, nodes, truncated, &elements);
    if (!err.isEmpty()) {
        return errorResult(err);
    }
    if (truncated && explicit_button.isEmpty()) {
        // A truncated tree means the button set may be incomplete, so "lone button" / "best
        // affirmative" auto-selection is no longer trustworthy; require an explicit caption.
        return errorResult(QStringLiteral(
            "The control tree is too large to auto-select a button safely; pass 'button'."));
    }
    QString why;
    const int index = chooseDialogButton(buttonsFromNodes(nodes), explicit_button, why);
    if (index < 0) {
        return errorResult(why);
    }
    if (index >= elements.size()) {
        return errorResult(
            QStringLiteral("The chosen button is no longer available; re-inspect the window."));
    }
    QString action;
    const QString invoke_err = invokeElement(elements[index].Get(), action);
    if (!invoke_err.isEmpty()) {
        return errorResult(invoke_err);
    }
    return jsonResult(QJsonObject{{QStringLiteral("ok"), true},
                                  {QStringLiteral("action"), action},
                                  {QStringLiteral("button"), nodes[index].name},
                                  {QStringLiteral("window"), windowTitleOf(hwnd)}});
}

void appendUiaTools(QJsonArray& tools) {
    tools.append(toolEntry(
        QStringLiteral("uia_inspect_window"),
        QStringLiteral(
            "Read a window's live UI Automation control tree as an indented outline of "
            "[ref] role \"name\" = \"value\" lines (the desktop analog of "
            "browser_snapshot). Each ref is usable with uia_get_control_value. Match the "
            "window by title substring, or set foreground=true to inspect the active pop-up."),
        toolSchema(
            QJsonObject{
                {QStringLiteral("window_title"),
                 stringProperty(QStringLiteral("Title substring of the target window."))},
                {QStringLiteral("foreground"),
                 boolProperty(QStringLiteral("Inspect the active window (title-less pop-ups)."))},
                {QStringLiteral("max_depth"),
                 typedProperty(QStringLiteral("integer"),
                               QStringLiteral("Optional tree depth cap (default/max 40)."))}},
            QJsonArray{})));
    tools.append(toolEntry(
        QStringLiteral("uia_find_control"),
        QStringLiteral(
            "Find controls in a window whose accessible name contains the query "
            "(case-insensitive), optionally filtered by control_type (button, checkbox, "
            "edit, menuitem, tabitem, ...). Returns matching refs with role/name/value."),
        toolSchema(
            QJsonObject{{QStringLiteral("window_title"),
                         stringProperty(QStringLiteral("Title substring of the target window."))},
                        {QStringLiteral("foreground"),
                         boolProperty(QStringLiteral("Search the active window instead."))},
                        {QStringLiteral("name"),
                         stringProperty(QStringLiteral("Accessible-name substring to match."))},
                        {QStringLiteral("control_type"),
                         stringProperty(QStringLiteral("Optional control-type name to require."))}},
            QJsonArray{QStringLiteral("name")})));
    tools.append(toolEntry(
        QStringLiteral("uia_get_control_value"),
        QStringLiteral("Read one control's current value/name/enabled/toggle state by its ref from "
                       "the most recent uia_inspect_window. Re-reads the live control and refuses "
                       "if the tree changed since inspection."),
        toolSchema(
            QJsonObject{{QStringLiteral("ref"),
                         typedProperty(QStringLiteral("integer"),
                                       QStringLiteral("Ref index from uia_inspect_window."))}},
            QJsonArray{QStringLiteral("ref")})));
    tools.append(toolEntry(
        QStringLiteral("uia_get_focused"),
        QStringLiteral("Report the control that currently has keyboard focus (role, name, value, "
                       "and its top-level window), useful for confirming where typed input lands."),
        toolSchema(QJsonObject{}, QJsonArray{})));
    tools.append(toolEntry(
        QStringLiteral("uia_click_control"),
        QStringLiteral("Activate a control by its ref from the most recent uia_inspect_window: "
                       "Invoke a button/menu item, else Toggle a checkbox, else Select a list/tab "
                       "item -- programmatically via UI Automation, WITHOUT moving the mouse. "
                       "Re-checks the tree and refuses if the ref drifted. Preferred over "
                       "click_text/mouse_click when the target has a ref."),
        toolSchema(
            QJsonObject{{QStringLiteral("ref"),
                         typedProperty(QStringLiteral("integer"),
                                       QStringLiteral("Ref index from uia_inspect_window."))}},
            QJsonArray{QStringLiteral("ref")})));
}

void appendDialogTools(QJsonArray& tools) {
    tools.append(toolEntry(
        QStringLiteral("dismiss_dialog"),
        QStringLiteral(
            "Close an acknowledgement pop-up by invoking its affirmative button "
            "(OK/Close/Continue/Finish/...) via UI Automation, WITHOUT the mouse and WITHOUT a "
            "stored ref -- the robust exit for a completion dialog whose button is custom-drawn "
            "and nameless. Target the active pop-up with foreground=true or match by window_title. "
            "A single-button dialog is invoked even when unnamed; pass 'button' to force a "
            "specific caption. Never presses Cancel/No/Quit unless you name it in 'button'."),
        toolSchema(
            QJsonObject{
                {QStringLiteral("foreground"),
                 boolProperty(QStringLiteral("Target the active window (title-less pop-ups)."))},
                {QStringLiteral("window_title"),
                 stringProperty(QStringLiteral("Title substring of the target window."))},
                {QStringLiteral("button"),
                 stringProperty(QStringLiteral(
                     "Optional caption substring to invoke instead of the auto affirmative."))}},
            QJsonArray{})));
}

// -- catalog + dispatch ------------------------------------------------------

void appendCaptureTools(QJsonArray& tools) {
    tools.append(toolEntry(
        QStringLiteral("capture_window"),
        QStringLiteral("Capture a PNG screenshot of a window (uses PrintWindow, so occluded or "
                       "DWM-composited windows still render). Match by title substring, or set "
                       "foreground=true to grab the active pop-up (which often has no title). "
                       "Fails if the window is minimized."),
        toolSchema(
            QJsonObject{{QStringLiteral("window_title"),
                         stringProperty(QStringLiteral("Title substring to match."))},
                        {QStringLiteral("foreground"),
                         boolProperty(QStringLiteral("Capture the active window instead."))}},
            QJsonArray{})));
    tools.append(toolEntry(
        QStringLiteral("capture_screen"),
        QStringLiteral("Capture a PNG screenshot of the full virtual screen (all monitors)."),
        toolSchema(QJsonObject{}, QJsonArray{})));
    tools.append(toolEntry(
        QStringLiteral("capture_monitor"),
        QStringLiteral("Capture a PNG screenshot of one monitor by its index from list_monitors."),
        toolSchema(
            QJsonObject{{QStringLiteral("index"),
                         typedProperty(QStringLiteral("integer"),
                                       QStringLiteral("Monitor index from list_monitors."))}},
            QJsonArray{QStringLiteral("index")})));
}

struct DesktopHandler {
    QLatin1String name;
    ToolResult (*fn)(const QJsonObject&);
};

const DesktopHandler kDesktopHandlers[] = {
    {QLatin1String("capture_window"), toolCaptureWindow},
    {QLatin1String("capture_screen"), toolCaptureScreen},
    {QLatin1String("capture_monitor"), toolCaptureMonitor},
    {QLatin1String("uia_inspect_window"), toolUiaInspectWindow},
    {QLatin1String("uia_find_control"), toolUiaFindControl},
    {QLatin1String("uia_get_control_value"), toolUiaGetControlValue},
    {QLatin1String("uia_get_focused"), toolUiaGetFocused},
    {QLatin1String("uia_click_control"), toolUiaClickControl},
    {QLatin1String("dismiss_dialog"), toolDismissDialog},
};

}  // namespace

QJsonArray desktopToolCatalog() {
    QJsonArray tools;
    appendCaptureTools(tools);
    appendUiaTools(tools);
    appendDialogTools(tools);
    return tools;
}

bool desktopHandles(const QString& name) {
    for (const auto& entry : kDesktopHandlers) {
        if (name == entry.name) {
            return true;
        }
    }
    return false;
}

ToolResult invokeDesktopTool(const QString& name, const QJsonObject& args) {
    for (const auto& entry : kDesktopHandlers) {
        if (name == entry.name) {
            return entry.fn(args);
        }
    }
    return errorResult(QStringLiteral("Unknown desktop tool: %1").arg(name));
}

}  // namespace sak::win32mcp

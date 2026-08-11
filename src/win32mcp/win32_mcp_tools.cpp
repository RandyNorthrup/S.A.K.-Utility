// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_tools.h"

#include "sak/win32mcp/browser_extension_installer.h"
#include "sak/win32mcp/win32_mcp_desktop.h"
#include "sak/win32mcp/win32_mcp_input.h"
#include "sak/win32mcp/win32_mcp_ocr.h"
#include "sak/win32mcp/win32_mcp_watch.h"

#include <QJsonDocument>
#include <QJsonValue>
#include <QLatin1String>
#include <QVector>

#include <algorithm>
#include <cwchar>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shellscalingapi.h>

namespace sak::win32mcp {

namespace {

// Native server version. Independent of the app version so the two can move apart;
// bump when the tool surface changes.
constexpr char kServerName[] = "sak-win32-mcp";
constexpr char kServerVersion[] = "1.0.0";

ToolResult jsonResult(const QJsonObject& object) {
    return {.text = QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)),
            .is_error = false};
}

ToolResult errorResult(const QString& message) {
    return {.text = QJsonDocument(QJsonObject{{QStringLiteral("error"), message}})
                        .toJson(QJsonDocument::Compact),
            .is_error = true};
}

QString wideToQString(const wchar_t* text, int length) {
    if ((text == nullptr) || length <= 0) {
        return {};
    }
    return QString::fromWCharArray(text, length);
}

QString windowTitle(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) {
        return {};
    }
    QVector<wchar_t> buffer(length + 1, L'\0');
    const int copied = GetWindowTextW(hwnd, buffer.data(), length + 1);
    return wideToQString(buffer.data(), copied);
}

QString windowClassName(HWND hwnd) {
    constexpr int kWindowClassNameBufferChars = 256;
    wchar_t buffer[kWindowClassNameBufferChars] = {0};
    const int copied = GetClassNameW(hwnd, buffer, kWindowClassNameBufferChars);
    return wideToQString(buffer, copied);
}

qint64 windowProcessId(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return static_cast<qint64>(pid);
}

QJsonObject rectObject(const RECT& rect) {
    return QJsonObject{{QStringLiteral("x"), static_cast<int>(rect.left)},
                       {QStringLiteral("y"), static_cast<int>(rect.top)},
                       {QStringLiteral("width"), static_cast<int>(rect.right - rect.left)},
                       {QStringLiteral("height"), static_cast<int>(rect.bottom - rect.top)}};
}

QJsonObject describeWindow(HWND hwnd) {
    RECT rect = {.left = 0, .top = 0, .right = 0, .bottom = 0};
    GetWindowRect(hwnd, &rect);
    return QJsonObject{{QStringLiteral("title"), windowTitle(hwnd)},
                       {QStringLiteral("class"), windowClassName(hwnd)},
                       {QStringLiteral("hwnd"), QString::number(reinterpret_cast<quintptr>(hwnd))},
                       {QStringLiteral("pid"), windowProcessId(hwnd)},
                       {QStringLiteral("visible"), IsWindowVisible(hwnd) != FALSE},
                       {QStringLiteral("minimized"), IsIconic(hwnd) != FALSE},
                       {QStringLiteral("maximized"), IsZoomed(hwnd) != FALSE},
                       {QStringLiteral("bounds"), rectObject(rect)}};
}

struct WindowCollectState {
    QString m_filter_lower;
    QJsonArray m_windows;
};

BOOL CALLBACK collectWindowProc(HWND hwnd, LPARAM param) {
    auto* state = reinterpret_cast<WindowCollectState*>(param);
    if (IsWindowVisible(hwnd) == FALSE) {
        return TRUE;
    }
    const QString title = windowTitle(hwnd);
    if (title.isEmpty()) {
        return TRUE;
    }
    if (!state->m_filter_lower.isEmpty() && !title.toLower().contains(state->m_filter_lower)) {
        return TRUE;
    }
    state->m_windows.append(describeWindow(hwnd));
    return TRUE;
}

struct WindowMatch {
    HWND m_hwnd;
    QString m_title_lower;
};

struct FindState {
    QString m_needle_lower;
    QVector<WindowMatch> m_matches;
};

BOOL CALLBACK collectMatchProc(HWND hwnd, LPARAM param) {
    auto* find = reinterpret_cast<FindState*>(param);
    if (IsWindowVisible(hwnd) == FALSE) {
        return TRUE;
    }
    const QString title = windowTitle(hwnd);
    const QString lower = title.toLower();
    if (title.isEmpty() || !lower.contains(find->m_needle_lower)) {
        return TRUE;
    }
    find->m_matches.append(WindowMatch{.m_hwnd = hwnd, .m_title_lower = lower});
    return TRUE;  // collect every match, then disambiguate rather than taking the first
}

// Pick the single window a title substring unambiguously names. One match wins; when several
// match, a lone EXACT (case-insensitive) title match wins; otherwise fail closed rather than
// silently acting on whichever window enumerated first (which could close/drive the wrong app).
HWND pickUniqueWindow(const QVector<WindowMatch>& matches,
                      const QString& needle_lower,
                      QString& err) {
    if (matches.isEmpty()) {
        err = QStringLiteral("No visible window matching '%1'").arg(needle_lower);
        return nullptr;
    }
    if (matches.size() == 1) {
        return matches.first().m_hwnd;
    }
    HWND exact = nullptr;
    int exact_count = 0;
    for (const WindowMatch& m : matches) {
        if (m.m_title_lower == needle_lower) {
            exact = m.m_hwnd;
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

HWND findWindowByTitle(const QString& needle_lower, QString& err) {
    FindState state{.m_needle_lower = needle_lower, .m_matches = {}};
    EnumWindows(collectMatchProc, reinterpret_cast<LPARAM>(&state));
    return pickUniqueWindow(state.m_matches, needle_lower, err);
}

ToolResult toolHealthCheck(const QJsonObject&) {
    return jsonResult(
        QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")},
                    {QStringLiteral("server"), QString::fromLatin1(kServerName)},
                    {QStringLiteral("version"), QString::fromLatin1(kServerVersion)},
                    {QStringLiteral("os"), QStringLiteral("Windows")},
                    {QStringLiteral("pid"), static_cast<qint64>(GetCurrentProcessId())},
                    {QStringLiteral("native"), true}});
}

ToolResult toolListWindows(const QJsonObject& args) {
    WindowCollectState state;
    state.m_filter_lower = args.value(QStringLiteral("filter")).toString().trimmed().toLower();
    EnumWindows(collectWindowProc, reinterpret_cast<LPARAM>(&state));
    return jsonResult(QJsonObject{{QStringLiteral("count"), state.m_windows.size()},
                                  {QStringLiteral("windows"), state.m_windows}});
}

ToolResult toolGetWindowInfo(const QJsonObject& args) {
    const QString title = args.value(QStringLiteral("window_title")).toString().trimmed();
    if (title.isEmpty()) {
        return errorResult(QStringLiteral("window_title is required"));
    }
    QString find_err;
    HWND hwnd = findWindowByTitle(title.toLower(), find_err);
    if (hwnd == nullptr) {
        return errorResult(find_err);
    }
    return jsonResult(describeWindow(hwnd));
}

ToolResult toolCloseWindow(const QJsonObject& args) {
    const QString title = args.value(QStringLiteral("window_title")).toString().trimmed();
    if (title.isEmpty()) {
        return errorResult(QStringLiteral("window_title is required"));
    }
    QString find_err;
    HWND hwnd = findWindowByTitle(title.toLower(), find_err);
    if (hwnd == nullptr) {
        return errorResult(find_err);
    }
    // WM_CLOSE is the graceful request a window's own close button sends: the app can still run
    // its shutdown path and prompt to save. We never TerminateProcess, so no unsaved-work loss
    // beyond what a user clicking [X] would cause.
    const QString actual = windowTitle(hwnd);
    const bool posted = PostMessageW(hwnd, WM_CLOSE, 0, 0) != FALSE;
    if (!posted) {
        return errorResult(QStringLiteral("Could not deliver the close request to the window."));
    }
    return jsonResult(
        QJsonObject{{QStringLiteral("ok"), true},
                    {QStringLiteral("window"), actual},
                    {QStringLiteral("note"),
                     QStringLiteral("Sent WM_CLOSE; the app may still prompt to save.")}});
}

BOOL CALLBACK collectMonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    auto* monitors = reinterpret_cast<QJsonArray*>(param);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) == FALSE) {
        // Abort the whole enumeration so a read failure surfaces as an error below, rather than
        // silently omitting a monitor and returning a short list as if it were complete.
        return FALSE;
    }
    constexpr UINT kDefaultDpi = 96;
    UINT dpi_x = kDefaultDpi;
    UINT dpi_y = kDefaultDpi;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);
    monitors->append(
        QJsonObject{{QStringLiteral("index"), monitors->size()},
                    {QStringLiteral("primary"), (info.dwFlags & MONITORINFOF_PRIMARY) != 0},
                    {QStringLiteral("dpi"), static_cast<int>(dpi_x)},
                    {QStringLiteral("bounds"), rectObject(info.rcMonitor)},
                    {QStringLiteral("work_area"), rectObject(info.rcWork)}});
    return TRUE;
}

ToolResult toolListMonitors(const QJsonObject&) {
    QJsonArray monitors;
    // EnumDisplayMonitors returns FALSE on a genuine enumeration failure OR when collectMonitorProc
    // aborted on a GetMonitorInfo failure; either way we cannot vouch for a complete list, so fail
    // closed instead of returning a partial/empty set as success.
    if (EnumDisplayMonitors(
            nullptr, nullptr, collectMonitorProc, reinterpret_cast<LPARAM>(&monitors)) == FALSE) {
        return errorResult(QStringLiteral("Could not enumerate the monitors."));
    }
    return jsonResult(QJsonObject{{QStringLiteral("count"), monitors.size()},
                                  {QStringLiteral("monitors"), monitors}});
}

ToolResult toolMousePosition(const QJsonObject&) {
    POINT point = {.x = 0, .y = 0};
    if (GetCursorPos(&point) == FALSE) {
        return errorResult(QStringLiteral("GetCursorPos failed"));
    }
    return jsonResult(QJsonObject{{QStringLiteral("x"), static_cast<int>(point.x)},
                                  {QStringLiteral("y"), static_cast<int>(point.y)}});
}

// Bound clipboard text both ways: never dump an unbounded clipboard into the model, and
// never allocate an unbounded write buffer from a model-supplied string.
constexpr int kMaxClipboardChars = 200'000;

// The clipboard is a shared, single-owner OS resource; another process may hold it for a
// moment. Retry a few times before giving up rather than failing on transient contention.
bool openClipboardWithRetry() {
    constexpr int kOpenClipboardAttempts = 5;
    constexpr DWORD kClipboardRetryDelayMs = 10;
    for (int attempt = 0; attempt < kOpenClipboardAttempts; ++attempt) {
        if (OpenClipboard(nullptr) != FALSE) {
            return true;
        }
        Sleep(kClipboardRetryDelayMs);
    }
    return false;
}

ToolResult toolClipboardRead(const QJsonObject&) {
    if (!openClipboardWithRetry()) {
        return errorResult(
            QStringLiteral("Could not open the clipboard (held by another process)."));
    }
    const HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (handle == nullptr) {
        // No Unicode text on the clipboard (empty, or an image/other format). Report an
        // honest empty-with-no-text rather than an error, so the model can tell "nothing to
        // paste" from "the read failed".
        CloseClipboard();
        return jsonResult(QJsonObject{{QStringLiteral("text"), QString()},
                                      {QStringLiteral("has_text"), false},
                                      {QStringLiteral("truncated"), false}});
    }
    const auto* data = static_cast<const wchar_t*>(GlobalLock(handle));
    if (data == nullptr) {
        // A handle exists but could not be locked: a real failure, not empty success.
        CloseClipboard();
        return errorResult(QStringLiteral("Clipboard text could not be read."));
    }
    const size_t max_chars = GlobalSize(handle) / sizeof(wchar_t);
    const size_t length = wcsnlen(data, max_chars);  // bounded even if not null-terminated
    const size_t capped = std::min(length, static_cast<size_t>(kMaxClipboardChars));
    const QString text = QString::fromWCharArray(data, static_cast<int>(capped));
    GlobalUnlock(handle);
    CloseClipboard();
    return jsonResult(QJsonObject{{QStringLiteral("text"), text},
                                  {QStringLiteral("has_text"), true},
                                  {QStringLiteral("truncated"), capped < length}});
}

// Copy the model's text into a moveable global the clipboard can own. Returns nullptr on
// allocation/lock failure; on success the caller passes ownership to SetClipboardData.
HGLOBAL allocClipboardText(const QString& text) {
    const size_t bytes = (static_cast<size_t>(text.size()) + 1) * sizeof(wchar_t);
    const HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem == nullptr) {
        return nullptr;
    }
    auto* dst = static_cast<wchar_t*>(GlobalLock(mem));
    if (dst == nullptr) {
        GlobalFree(mem);
        return nullptr;
    }
    const qsizetype copied = text.toWCharArray(dst);
    dst[copied] = L'\0';
    GlobalUnlock(mem);
    return mem;
}

ToolResult toolClipboardWrite(const QJsonObject& arguments) {
    if (!arguments.contains(QStringLiteral("text"))) {
        return errorResult(QStringLiteral("clipboard_write requires 'text'"));
    }
    // Reject a non-string text rather than coercing it to "" -- an empty coercion would clear the
    // clipboard (destroying prior contents) and report success for what was actually caller error.
    const QJsonValue text_value = arguments.value(QStringLiteral("text"));
    if (!text_value.isString()) {
        return errorResult(QStringLiteral("text must be a string"));
    }
    const QString text = text_value.toString();
    if (text.size() > kMaxClipboardChars) {
        return errorResult(QStringLiteral("Text exceeds the clipboard write limit."));
    }
    const HGLOBAL mem = allocClipboardText(text);
    if (mem == nullptr) {
        return errorResult(QStringLiteral("Could not allocate clipboard memory."));
    }
    if (!openClipboardWithRetry()) {
        GlobalFree(mem);
        return errorResult(
            QStringLiteral("Could not open the clipboard (held by another process)."));
    }
    // EmptyClipboard must precede SetClipboardData to take ownership; it clears any prior
    // contents. This is the mandated Win32 order (SetClipboardData with a valid moveable
    // handle after a successful open+empty does not realistically fail).
    EmptyClipboard();
    if (SetClipboardData(CF_UNICODETEXT, mem) == nullptr) {
        // Ownership is transferred to the system only on success; free it ourselves here.
        GlobalFree(mem);
        CloseClipboard();
        return errorResult(QStringLiteral("SetClipboardData failed."));
    }
    CloseClipboard();  // on success the system owns `mem`; do not free it
    return jsonResult(QJsonObject{{QStringLiteral("ok"), true},
                                  {QStringLiteral("written"), static_cast<int>(text.size())}});
}

// Combine the installer's summary + detail into one error string.
QString installerError(const ExtensionInstallResult& r) {
    return r.detail.isEmpty() ? r.summary : (r.summary + QStringLiteral(": ") + r.detail);
}

ToolResult toolBrowserExtensionInstall(const QJsonObject&) {
    BrowserExtensionInstaller installer;
    const ExtensionInstallResult r = installer.install();
    if (!r.ok) {
        return errorResult(installerError(r));
    }
    return jsonResult(QJsonObject{{QStringLiteral("ok"), true},
                                  {QStringLiteral("summary"), r.summary},
                                  {QStringLiteral("detail"), r.detail},
                                  {QStringLiteral("state"), installer.stateString()}});
}

ToolResult toolBrowserExtensionUninstall(const QJsonObject&) {
    BrowserExtensionInstaller installer;
    const ExtensionInstallResult r = installer.uninstall();
    if (!r.ok) {
        return errorResult(installerError(r));
    }
    return jsonResult(QJsonObject{{QStringLiteral("ok"), true},
                                  {QStringLiteral("summary"), r.summary},
                                  {QStringLiteral("detail"), r.detail},
                                  {QStringLiteral("state"), installer.stateString()}});
}

ToolResult toolBrowserExtensionStatus(const QJsonObject&) {
    const BrowserExtensionInstaller installer;
    return jsonResult(
        QJsonObject{{QStringLiteral("state"), installer.stateString()},
                    {QStringLiteral("crx_present"), installer.crxPresent()},
                    {QStringLiteral("extension_id"), QString::fromLatin1(kBrowserExtensionId)}});
}

QJsonObject stringProperty(const QString& description) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
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

// Desktop-control tools (capture + UIA read, and -- later -- input) and OCR tools live in their
// own modules; merge their advertised schemas so they appear in the one catalog.
void appendModuleCatalogs(QJsonArray& tools) {
    for (const QJsonValue& entry : desktopToolCatalog()) {
        tools.append(entry);
    }
    for (const QJsonValue& entry : ocrToolCatalog()) {
        tools.append(entry);
    }
    for (const QJsonValue& entry : watchToolCatalog()) {
        tools.append(entry);
    }
    for (const QJsonValue& entry : inputToolCatalog()) {
        tools.append(entry);
    }
}

}  // namespace

QJsonArray toolCatalog() {
    QJsonArray tools;
    tools.append(toolEntry(QStringLiteral("health_check"),
                           QStringLiteral("Report the native Win32 control engine's health and "
                                          "identity."),
                           toolSchema({}, {})));
    tools.append(toolEntry(
        QStringLiteral("list_windows"),
        QStringLiteral("List visible top-level windows (title, class, pid, bounds, state). "
                       "Optional case-insensitive title substring filter."),
        toolSchema(
            QJsonObject{{QStringLiteral("filter"),
                         stringProperty(QStringLiteral("Case-insensitive title substring."))}},
            {})));
    tools.append(toolEntry(
        QStringLiteral("get_window_info"),
        QStringLiteral("Get details for the first visible window whose title contains the query."),
        toolSchema(QJsonObject{{QStringLiteral("window_title"),
                                stringProperty(QStringLiteral("Title substring to match."))}},
                   QJsonArray{QStringLiteral("window_title")})));
    tools.append(toolEntry(
        QStringLiteral("close_window"),
        QStringLiteral("Ask the first visible window matching the title to close (sends WM_CLOSE, "
                       "like clicking its X -- the app may prompt to save). High-risk: confirms in "
                       "Assisted and offers a restore point in Unattended."),
        toolSchema(QJsonObject{{QStringLiteral("window_title"),
                                stringProperty(QStringLiteral("Title substring to match."))}},
                   QJsonArray{QStringLiteral("window_title")})));
    tools.append(toolEntry(QStringLiteral("list_monitors"),
                           QStringLiteral("List monitors with bounds, work area, DPI, and which "
                                          "is primary."),
                           toolSchema({}, {})));
    tools.append(toolEntry(QStringLiteral("mouse_position"),
                           QStringLiteral("Return the current mouse cursor position in virtual "
                                          "screen coordinates."),
                           toolSchema({}, {})));
    tools.append(toolEntry(
        QStringLiteral("clipboard_read"),
        QStringLiteral("Read the system clipboard's text. May expose data the user copied from "
                       "other applications, so it requires confirmation."),
        toolSchema({}, {})));
    tools.append(toolEntry(
        QStringLiteral("clipboard_write"),
        QStringLiteral("Replace the system clipboard's text with the given string."),
        toolSchema(QJsonObject{{QStringLiteral("text"),
                                stringProperty(QStringLiteral("Text to place on the clipboard."))}},
                   QJsonArray{QStringLiteral("text")})));
    tools.append(toolEntry(
        QStringLiteral("browser_extension_install"),
        QStringLiteral("Set up the S.A.K. browser-control extension in Chrome so the assistant can "
                       "drive the browser: force-installs the signed extension via user Chrome "
                       "policy and registers the native messaging host. Restart Chrome to finish. "
                       "Changes user (HKCU) Chrome policy, so it requires confirmation."),
        toolSchema({}, {})));
    tools.append(toolEntry(
        QStringLiteral("browser_extension_uninstall"),
        QStringLiteral("Remove the S.A.K. browser-control extension from Chrome: deletes our "
                       "force-install policy entry (leaving any other policies intact) and the "
                       "native messaging host. Chrome removes the extension on its next launch."),
        toolSchema({}, {})));
    tools.append(toolEntry(
        QStringLiteral("browser_extension_status"),
        QStringLiteral(
            "Report whether the S.A.K. browser-control extension is set up: force-install "
            "policy and native host presence, whether the extension package is available, "
            "and the extension id."),
        toolSchema({}, {})));
    appendModuleCatalogs(tools);
    return tools;
}

ToolResult invokeTool(const QString& name, const QJsonObject& arguments) {
    using Handler = ToolResult (*)(const QJsonObject&);
    struct Entry {
        QLatin1String m_name;
        Handler m_fn;
    };
    static const Entry kHandlers[] = {
        {.m_name = QLatin1String("health_check"), .m_fn = toolHealthCheck},
        {.m_name = QLatin1String("list_windows"), .m_fn = toolListWindows},
        {.m_name = QLatin1String("get_window_info"), .m_fn = toolGetWindowInfo},
        {.m_name = QLatin1String("close_window"), .m_fn = toolCloseWindow},
        {.m_name = QLatin1String("list_monitors"), .m_fn = toolListMonitors},
        {.m_name = QLatin1String("mouse_position"), .m_fn = toolMousePosition},
        {.m_name = QLatin1String("clipboard_read"), .m_fn = toolClipboardRead},
        {.m_name = QLatin1String("clipboard_write"), .m_fn = toolClipboardWrite},
        {.m_name = QLatin1String("browser_extension_install"), .m_fn = toolBrowserExtensionInstall},
        {.m_name = QLatin1String("browser_extension_uninstall"),
         .m_fn = toolBrowserExtensionUninstall},
        {.m_name = QLatin1String("browser_extension_status"), .m_fn = toolBrowserExtensionStatus},
    };
    for (const auto& entry : kHandlers) {
        if (name == entry.m_name) {
            return entry.m_fn(arguments);
        }
    }
    if (desktopHandles(name)) {
        return invokeDesktopTool(name, arguments);
    }
    if (ocrHandles(name)) {
        return invokeOcrTool(name, arguments);
    }
    if (watchHandles(name)) {
        return invokeWatchTool(name, arguments);
    }
    if (inputHandles(name)) {
        return invokeInputTool(name, arguments);
    }
    return errorResult(QStringLiteral("Unknown tool: %1").arg(name));
}

QString serverName() {
    return QString::fromLatin1(kServerName);
}

QString serverVersion() {
    return QString::fromLatin1(kServerVersion);
}

}  // namespace sak::win32mcp

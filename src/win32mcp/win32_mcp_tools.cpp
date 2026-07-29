// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_tools.h"

#include <QJsonDocument>
#include <QLatin1String>
#include <QVector>

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
    return {QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)), false};
}

ToolResult errorResult(const QString& message) {
    return {QJsonDocument(QJsonObject{{QStringLiteral("error"), message}})
                .toJson(QJsonDocument::Compact),
            true};
}

QString wideToQString(const wchar_t* text, int length) {
    if (!text || length <= 0) {
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
    wchar_t buffer[256] = {0};
    const int copied = GetClassNameW(hwnd, buffer, 256);
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
    RECT rect = {0, 0, 0, 0};
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
    QString filter_lower;
    QJsonArray windows;
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
    if (!state->filter_lower.isEmpty() && !title.toLower().contains(state->filter_lower)) {
        return TRUE;
    }
    state->windows.append(describeWindow(hwnd));
    return TRUE;
}

HWND findWindowByTitle(const QString& needle_lower) {
    struct FindState {
        QString needle_lower;
        HWND match{nullptr};
    } state{needle_lower, nullptr};
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* find = reinterpret_cast<FindState*>(param);
            if (IsWindowVisible(hwnd) == FALSE) {
                return TRUE;
            }
            const QString title = windowTitle(hwnd);
            if (title.isEmpty() || !title.toLower().contains(find->needle_lower)) {
                return TRUE;
            }
            find->match = hwnd;
            return FALSE;  // stop at the first match
        },
        reinterpret_cast<LPARAM>(&state));
    return state.match;
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
    state.filter_lower = args.value(QStringLiteral("filter")).toString().trimmed().toLower();
    EnumWindows(collectWindowProc, reinterpret_cast<LPARAM>(&state));
    return jsonResult(QJsonObject{{QStringLiteral("count"), state.windows.size()},
                                  {QStringLiteral("windows"), state.windows}});
}

ToolResult toolGetWindowInfo(const QJsonObject& args) {
    const QString title = args.value(QStringLiteral("window_title")).toString().trimmed();
    if (title.isEmpty()) {
        return errorResult(QStringLiteral("window_title is required"));
    }
    HWND hwnd = findWindowByTitle(title.toLower());
    if (!hwnd) {
        return errorResult(QStringLiteral("No visible window matching '%1'").arg(title));
    }
    return jsonResult(describeWindow(hwnd));
}

BOOL CALLBACK collectMonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    auto* monitors = reinterpret_cast<QJsonArray*>(param);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) == FALSE) {
        return TRUE;
    }
    UINT dpi_x = 96;
    UINT dpi_y = 96;
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
    EnumDisplayMonitors(nullptr, nullptr, collectMonitorProc, reinterpret_cast<LPARAM>(&monitors));
    return jsonResult(QJsonObject{{QStringLiteral("count"), monitors.size()},
                                  {QStringLiteral("monitors"), monitors}});
}

ToolResult toolMousePosition(const QJsonObject&) {
    POINT point = {0, 0};
    if (GetCursorPos(&point) == FALSE) {
        return errorResult(QStringLiteral("GetCursorPos failed"));
    }
    return jsonResult(QJsonObject{{QStringLiteral("x"), static_cast<int>(point.x)},
                                  {QStringLiteral("y"), static_cast<int>(point.y)}});
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

}  // namespace

QJsonArray toolCatalog() {
    QJsonArray tools;
    tools.append(toolEntry(QStringLiteral("health_check"),
                           QStringLiteral("Report native win32 MCP server health and identity."),
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
    tools.append(toolEntry(QStringLiteral("list_monitors"),
                           QStringLiteral("List monitors with bounds, work area, DPI, and which "
                                          "is primary."),
                           toolSchema({}, {})));
    tools.append(toolEntry(QStringLiteral("mouse_position"),
                           QStringLiteral("Return the current mouse cursor position in virtual "
                                          "screen coordinates."),
                           toolSchema({}, {})));
    return tools;
}

ToolResult invokeTool(const QString& name, const QJsonObject& arguments) {
    if (name == QLatin1String("health_check")) {
        return toolHealthCheck(arguments);
    }
    if (name == QLatin1String("list_windows")) {
        return toolListWindows(arguments);
    }
    if (name == QLatin1String("get_window_info")) {
        return toolGetWindowInfo(arguments);
    }
    if (name == QLatin1String("list_monitors")) {
        return toolListMonitors(arguments);
    }
    if (name == QLatin1String("mouse_position")) {
        return toolMousePosition(arguments);
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

// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_desktop.h"

#include <QBuffer>
#include <QByteArray>
#include <QImage>
#include <QJsonDocument>
#include <QVector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00'00'00'02
#endif

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

HWND findVisibleWindowByTitle(const QString& needle_lower) {
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
            const QString title = windowTitleOf(hwnd);
            if (title.isEmpty() || !title.toLower().contains(find->needle_lower)) {
                return TRUE;
            }
            find->match = hwnd;
            return FALSE;  // stop at the first match
        },
        reinterpret_cast<LPARAM>(&state));
    return state.match;
}

// -- screen capture ----------------------------------------------------------

// A capture scales with the surface; cap the base64 so a multi-monitor grab cannot flood the
// transport (16 MiB matches the browser screenshot cap). Bound raw dimensions too so an absurd
// window rect cannot allocate gigabytes before encoding.
constexpr int kMaxCaptureBase64 = 16 * 1024 * 1024;
constexpr int kMaxCaptureEdge = 16'384;

// Encode a top-down 32bpp buffer as base64 PNG. Format_RGB32 reads the B,G,R,X bytes a screen
// DIB holds (little-endian) and ignores the unused 4th byte, so an opaque capture round-trips.
QString encodePng(const uchar* bits, int width, int height) {
    const QImage image(bits, width, height, width * 4, QImage::Format_RGB32);
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

// What + where to capture. When hwnd is non-null PrintWindow renders it first (so an occluded
// or DWM-composited window like Chrome still captures); otherwise a BitBlt from srcDC of `rect`
// is used.
struct CaptureSpec {
    HWND hwnd;
    HDC srcDC;
    RECT rect;
};

QString validateCaptureDims(int width, int height) {
    if (width <= 0 || height <= 0) {
        return QStringLiteral("The capture region is empty.");
    }
    if (width > kMaxCaptureEdge || height > kMaxCaptureEdge) {
        return QStringLiteral("The capture region is too large; target a single window.");
    }
    return {};
}

// Render the spec into memDC (which already has the target DIB selected). PrintWindow first for
// a window; BitBlt otherwise (also the fallback when PrintWindow fails).
bool blitCapture(const CaptureSpec& spec, HDC memDC, int width, int height) {
    if (spec.hwnd != nullptr && PrintWindow(spec.hwnd, memDC, PW_RENDERFULLCONTENT) != FALSE) {
        return true;
    }
    return BitBlt(memDC,
                  0,
                  0,
                  width,
                  height,
                  spec.srcDC,
                  spec.rect.left,
                  spec.rect.top,
                  SRCCOPY | CAPTUREBLT) != FALSE;
}

QString encodeCapped(const uchar* bits, int width, int height, QString* err) {
    const QString base64 = encodePng(bits, width, height);
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

// Capture the spec to base64 PNG, releasing every GDI object on every path. Returns empty and
// sets *err on any failure (bad dims, GDI failure, blit failure, or an over-cap result).
QString captureToBase64(const CaptureSpec& spec, QString* err) {
    const int width = spec.rect.right - spec.rect.left;
    const int height = spec.rect.bottom - spec.rect.top;
    *err = validateCaptureDims(width, height);
    if (!err->isEmpty()) {
        return {};
    }
    HDC memDC = CreateCompatibleDC(spec.srcDC);
    if (memDC == nullptr) {
        *err = QStringLiteral("CreateCompatibleDC failed.");
        return {};
    }
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;  // top-down so scanlines match QImage's order
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib == nullptr || bits == nullptr) {
        if (dib != nullptr) {
            DeleteObject(dib);
        }
        DeleteDC(memDC);
        *err = QStringLiteral("CreateDIBSection failed.");
        return {};
    }
    HGDIOBJ old = SelectObject(memDC, dib);
    QString result;
    if (blitCapture(spec, memDC, width, height)) {
        result = encodeCapped(static_cast<const uchar*>(bits), width, height, err);
    } else {
        *err = QStringLiteral("The capture failed (the window may be minimized or protected).");
    }
    SelectObject(memDC, old);
    DeleteObject(dib);
    DeleteDC(memDC);
    return result;
}

ToolResult toolCaptureWindow(const QJsonObject& args) {
    const QString title = args.value(QStringLiteral("window_title")).toString().trimmed();
    if (title.isEmpty()) {
        return errorResult(QStringLiteral("window_title is required"));
    }
    HWND hwnd = findVisibleWindowByTitle(title.toLower());
    if (!hwnd) {
        return errorResult(QStringLiteral("No visible window matching '%1'").arg(title));
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
    HDC screen = GetDC(nullptr);
    QString err;
    const QString b64 = captureToBase64(CaptureSpec{hwnd, screen, rc}, &err);
    ReleaseDC(nullptr, screen);
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
    HDC screen = GetDC(nullptr);
    QString err;
    const RECT rc{x, y, x + w, y + h};
    const QString b64 = captureToBase64(CaptureSpec{nullptr, screen, rc}, &err);
    ReleaseDC(nullptr, screen);
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
    HDC screen = GetDC(nullptr);
    QString err;
    const QString b64 = captureToBase64(CaptureSpec{nullptr, screen, rc}, &err);
    ReleaseDC(nullptr, screen);
    if (b64.isEmpty()) {
        return errorResult(err);
    }
    return imageResult(
        b64, QStringLiteral("Captured a %1x%2 PNG of monitor %3.").arg(w).arg(h).arg(index));
}

// -- catalog + dispatch ------------------------------------------------------

void appendCaptureTools(QJsonArray& tools) {
    tools.append(toolEntry(
        QStringLiteral("capture_window"),
        QStringLiteral("Capture a PNG screenshot of the first visible window whose title contains "
                       "the query (uses PrintWindow, so occluded or DWM-composited windows still "
                       "render). Fails if the window is minimized."),
        toolSchema(QJsonObject{{QStringLiteral("window_title"),
                                stringProperty(QStringLiteral("Title substring to match."))}},
                   QJsonArray{QStringLiteral("window_title")})));
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
};

}  // namespace

QJsonArray desktopToolCatalog() {
    QJsonArray tools;
    appendCaptureTools(tools);
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

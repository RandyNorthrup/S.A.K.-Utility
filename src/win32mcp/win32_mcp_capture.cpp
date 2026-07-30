// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_capture.h"

#include <QVector>

#include <algorithm>
#include <utility>

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

// An absurd raw rect cannot be allowed to allocate gigabytes before downscaling.
constexpr int kAbsMaxEdge = 16'384;

struct Size {
    int w;
    int h;
};

struct Scaled {
    int w;
    int h;
    double scale;  // dest/source ratio; < 1 when downscaled
};

// The GDI objects backing one capture, torn down together in freeSurface.
struct Surface {
    HDC screen;
    HDC memDC;
    HBITMAP dib;
    void* bits;
};

// Create a top-down 32bpp (BGRX) DIB section selected into memDC. Top-down (negative height)
// so scanlines are in the row order both PNG and SoftwareBitmap expect.
HBITMAP makeTopDownDib(HDC memDC, int width, int height, void** bits) {
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, bits, nullptr, 0);
}

// Render the source into memDC (which owns the target DIB). PrintWindow first for a window (so
// an occluded or DWM-composited window still renders); BitBlt otherwise, and as the fallback
// when PrintWindow fails.
bool blitSource(void* hwnd, HDC memDC, HDC screen, const RECT& rect) {
    if (hwnd != nullptr &&
        PrintWindow(static_cast<HWND>(hwnd), memDC, PW_RENDERFULLCONTENT) != FALSE) {
        return true;
    }
    return BitBlt(memDC,
                  0,
                  0,
                  rect.right - rect.left,
                  rect.bottom - rect.top,
                  screen,
                  rect.left,
                  rect.top,
                  SRCCOPY | CAPTUREBLT) != FALSE;
}

// Compute the downscaled destination size so max(width,height) <= max_edge; scale is dest/src.
Scaled scaledSize(Size src, int max_edge) {
    const int longest = std::max(src.w, src.h);
    if (max_edge <= 0 || longest <= max_edge) {
        return Scaled{src.w, src.h, 1.0};
    }
    const double scale = static_cast<double>(max_edge) / static_cast<double>(longest);
    return Scaled{std::max(1, static_cast<int>(src.w * scale)),
                  std::max(1, static_cast<int>(src.h * scale)),
                  scale};
}

// Copy a DIB's pixels out to a byte array (width*height*4, top-down BGRA).
QByteArray copyDibBytes(const void* bits, int width, int height) {
    return QByteArray(static_cast<const char*>(bits), static_cast<qsizetype>(width) * height * 4);
}

// StretchBlt the source DC into a fresh destination DIB and return its bytes. HALFTONE keeps
// downscaled text legible for OCR. Releases the destination DIB/DC on every path.
QByteArray stretchToBytes(HDC screen, HDC src_dc, Size src, Size dst) {
    HDC dst_dc = CreateCompatibleDC(screen);
    if (dst_dc == nullptr) {
        return {};
    }
    void* bits = nullptr;
    HBITMAP dib = makeTopDownDib(dst_dc, dst.w, dst.h, &bits);
    QByteArray out;
    if (dib != nullptr && bits != nullptr) {
        HGDIOBJ old = SelectObject(dst_dc, dib);
        SetStretchBltMode(dst_dc, HALFTONE);
        SetBrushOrgEx(dst_dc, 0, 0, nullptr);
        if (StretchBlt(dst_dc, 0, 0, dst.w, dst.h, src_dc, 0, 0, src.w, src.h, SRCCOPY) != FALSE) {
            out = copyDibBytes(bits, dst.w, dst.h);
        }
        SelectObject(dst_dc, old);
    }
    if (dib != nullptr) {
        DeleteObject(dib);
    }
    DeleteDC(dst_dc);
    return out;
}

QString validateRect(int width, int height) {
    if (width <= 0 || height <= 0) {
        return QStringLiteral("The capture region is empty.");
    }
    if (width > kAbsMaxEdge || height > kAbsMaxEdge) {
        return QStringLiteral("The capture region is too large; target a single window.");
    }
    return {};
}

// Allocate the screen DC + a memory DC + a top-down DIB of (w x h). Returns false (and frees
// whatever was created) if any step fails; on success s.dib is selected by the caller.
bool allocSurface(int width, int height, Surface& s) {
    s.screen = GetDC(nullptr);
    s.memDC = CreateCompatibleDC(s.screen);
    s.bits = nullptr;
    s.dib = (s.memDC != nullptr) ? makeTopDownDib(s.memDC, width, height, &s.bits) : nullptr;
    return s.memDC != nullptr && s.dib != nullptr && s.bits != nullptr;
}

void freeSurface(const Surface& s) {
    if (s.dib != nullptr) {
        DeleteObject(s.dib);
    }
    if (s.memDC != nullptr) {
        DeleteDC(s.memDC);
    }
    ReleaseDC(nullptr, s.screen);
}

// Blit the source into the surface, downscale if needed, and copy pixels into `out`. Returns an
// error string on blit/read failure, empty on success.
QString renderToBytes(const Surface& s,
                      const CaptureRequest& req,
                      const RECT& rect,
                      CaptureBits& out) {
    if (!blitSource(req.hwnd, s.memDC, s.screen, rect)) {
        return QStringLiteral("The capture failed (the window may be minimized or protected).");
    }
    const Size src{static_cast<int>(rect.right - rect.left),
                   static_cast<int>(rect.bottom - rect.top)};
    const Scaled sc = scaledSize(src, req.max_edge);
    QByteArray bytes = (sc.scale < 1.0) ? stretchToBytes(s.screen, s.memDC, src, Size{sc.w, sc.h})
                                        : copyDibBytes(s.bits, src.w, src.h);
    if (bytes.isEmpty()) {
        return QStringLiteral("The capture could not be read.");
    }
    out = CaptureBits{std::move(bytes), sc.w, sc.h, sc.scale};
    return {};
}

}  // namespace

bool captureBgra(const CaptureRequest& req, CaptureBits& out, QString& err) {
    const RECT rect{req.left, req.top, req.right, req.bottom};
    err = validateRect(static_cast<int>(rect.right - rect.left),
                       static_cast<int>(rect.bottom - rect.top));
    if (!err.isEmpty()) {
        return false;
    }
    Surface s{};
    if (!allocSurface(static_cast<int>(rect.right - rect.left),
                      static_cast<int>(rect.bottom - rect.top),
                      s)) {
        freeSurface(s);
        err = QStringLiteral("Could not allocate a capture surface.");
        return false;
    }
    HGDIOBJ old = SelectObject(s.memDC, s.dib);
    err = renderToBytes(s, req, rect, out);
    SelectObject(s.memDC, old);
    freeSurface(s);
    return err.isEmpty();
}

namespace {

struct FindWindowState {
    QString needle_lower;
    HWND match;
};

QString titleOf(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) {
        return {};
    }
    QVector<wchar_t> buffer(length + 1, L'\0');
    const int copied = GetWindowTextW(hwnd, buffer.data(), length + 1);
    return QString::fromWCharArray(buffer.data(), copied);
}

BOOL CALLBACK findWindowProc(HWND hwnd, LPARAM param) {
    auto* find = reinterpret_cast<FindWindowState*>(param);
    if (IsWindowVisible(hwnd) == FALSE) {
        return TRUE;
    }
    const QString title = titleOf(hwnd);
    if (title.isEmpty() || !title.toLower().contains(find->needle_lower)) {
        return TRUE;
    }
    find->match = hwnd;
    return FALSE;  // first match wins
}

}  // namespace

bool windowRectByTitle(const QString& needle_lower, WindowRect& out, QString& err) {
    FindWindowState state{needle_lower, nullptr};
    EnumWindows(findWindowProc, reinterpret_cast<LPARAM>(&state));
    if (state.match == nullptr) {
        err = QStringLiteral("No visible window matching '%1'").arg(needle_lower);
        return false;
    }
    if (IsIconic(state.match) != FALSE) {
        err = QStringLiteral("The window is minimized; restore it first.");
        return false;
    }
    RECT rc{};
    if (GetWindowRect(state.match, &rc) == FALSE) {
        err = QStringLiteral("Could not read the window bounds.");
        return false;
    }
    out.hwnd = state.match;
    out.left = rc.left;
    out.top = rc.top;
    out.right = rc.right;
    out.bottom = rc.bottom;
    return true;
}

bool foregroundWindowRect(WindowRect& out, QString& err) {
    const HWND fg = GetForegroundWindow();
    if (fg == nullptr) {
        err = QStringLiteral("There is no foreground window.");
        return false;
    }
    if (IsIconic(fg) != FALSE) {
        err = QStringLiteral("The foreground window is minimized; restore it first.");
        return false;
    }
    RECT rc{};
    if (GetWindowRect(fg, &rc) == FALSE) {
        err = QStringLiteral("Could not read the foreground window bounds.");
        return false;
    }
    out.hwnd = fg;
    out.left = rc.left;
    out.top = rc.top;
    out.right = rc.right;
    out.bottom = rc.bottom;
    return true;
}

}  // namespace sak::win32mcp

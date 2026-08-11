// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_capture.h"

#include "sak/win32mcp/win32_mcp_geometry.h"

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

struct Size {
    int w;
    int h;
};

// The GDI objects backing one capture, torn down together in freeSurface.
struct Surface {
    HDC screen;
    HDC memDC;
    HBITMAP dib;
    void* bits;
};

// A 32bpp top-down DIB: each pixel occupies 4 bytes (BGRX/BGRA).
constexpr int kDibBitsPerPixel = 32;
constexpr int kDibBytesPerPixel = 4;

// Create a top-down 32bpp (BGRX) DIB section selected into memDC. Top-down (negative height)
// so scanlines are in the row order both PNG and SoftwareBitmap expect.
HBITMAP makeTopDownDib(HDC memDC, int width, int height, void** bits) {
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = kDibBitsPerPixel;
    bmi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, bits, nullptr, 0);
}

// Render the source into memDC (which owns the target DIB). For a specific window we use
// PrintWindow ONLY: it renders even an occluded/DWM-composited window from its own surface, and
// on failure we fail closed instead of BitBlt-ing the screen rect -- a desktop BitBlt of an
// occluded window would capture whatever OTHER window sits on top, feeding OCR/clicks the wrong
// pixels. BitBlt is used solely for a null-hwnd full-screen / region grab, where the screen rect
// IS the intended source.
bool blitSource(void* hwnd, HDC memDC, HDC screen, const RECT& rect) {
    if (hwnd != nullptr) {
        return PrintWindow(static_cast<HWND>(hwnd), memDC, PW_RENDERFULLCONTENT) != FALSE;
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

// Copy a DIB's pixels out to a byte array (width*height*4, top-down BGRA).
QByteArray copyDibBytes(const void* bits, int width, int height) {
    return QByteArray(static_cast<const char*>(bits),
                      static_cast<qsizetype>(width) * height * kDibBytesPerPixel);
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
    const Size src{.w = static_cast<int>(rect.right - rect.left),
                   .h = static_cast<int>(rect.bottom - rect.top)};
    const Scaled sc = scaledSize(src.w, src.h, req.max_edge);
    QByteArray bytes = (sc.scale < 1.0)
                           ? stretchToBytes(s.screen, s.memDC, src, Size{.w = sc.w, .h = sc.h})
                           : copyDibBytes(s.bits, src.w, src.h);
    if (bytes.isEmpty()) {
        return QStringLiteral("The capture could not be read.");
    }
    out = CaptureBits{.bgra = std::move(bytes), .width = sc.w, .height = sc.h, .scale = sc.scale};
    return {};
}

}  // namespace

bool captureBgra(const CaptureRequest& req, CaptureBits& out, QString& err) {
    const RECT rect{.left = req.left, .top = req.top, .right = req.right, .bottom = req.bottom};
    err = validateCaptureRect(static_cast<int>(rect.right - rect.left),
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
    if (old == nullptr || old == HGDI_ERROR) {
        // Without the DIB selected the blit renders into the DC's default 1x1 bitmap and we would
        // return the DIB's zero-filled memory as a "successful" black capture; fail closed instead.
        freeSurface(s);
        err = QStringLiteral("Could not prepare the capture surface.");
        return false;
    }
    err = renderToBytes(s, req, rect, out);
    SelectObject(s.memDC, old);
    freeSurface(s);
    return err.isEmpty();
}

namespace {

struct WindowMatch {
    HWND hwnd;
    QString title_lower;
};

struct FindWindowState {
    QString needle_lower;
    QVector<WindowMatch> matches;
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
    const QString lower = title.toLower();
    if (title.isEmpty() || !lower.contains(find->needle_lower)) {
        return TRUE;
    }
    find->matches.append(WindowMatch{.hwnd = hwnd, .title_lower = lower});
    return TRUE;  // collect every match, then disambiguate rather than blindly taking the first
}

// Pick the single window a title substring unambiguously names. Exactly one match wins; when
// several visible windows match, a lone EXACT (case-insensitive) title match wins; otherwise fail
// closed rather than silently acting on whichever window happened to enumerate first, which could
// close/capture/drive the wrong application.
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

}  // namespace

bool windowRectByTitle(const QString& needle_lower, WindowRect& out, QString& err) {
    if (needle_lower.isEmpty()) {
        // An empty needle substring-matches every visible window; refuse rather than let an
        // absent/defaulted title select a guessed target. Fail closed.
        err = QStringLiteral("No window title was provided.");
        return false;
    }
    FindWindowState state{.needle_lower = needle_lower, .matches = {}};
    EnumWindows(findWindowProc, reinterpret_cast<LPARAM>(&state));
    HWND match = pickUniqueWindow(state.matches, needle_lower, err);
    if (match == nullptr) {
        return false;
    }
    if (IsIconic(match) != FALSE) {
        err = QStringLiteral("The window is minimized; restore it first.");
        return false;
    }
    RECT rc{};
    if (GetWindowRect(match, &rc) == FALSE) {
        err = QStringLiteral("Could not read the window bounds.");
        return false;
    }
    out.hwnd = match;
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

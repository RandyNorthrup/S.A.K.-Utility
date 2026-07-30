// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QString>

namespace sak::win32mcp {

// Shared screen/window capture primitive: grabs raw top-down BGRA8 pixels the higher-level
// desktop tools build on (PNG for capture_*, SoftwareBitmap for the OCR module). Kept in its
// own translation unit so the single GDI implementation is not duplicated and so the OCR TU's
// C++/WinRT projection stays isolated from the GDI/UIA code.
//
// The interface is deliberately windows.h-free (HWND passed as void*, RECT as four longs) so
// callers that already include windows.h and those that must not can both use it.

struct CaptureRequest {
    void* hwnd;  // non-null: PrintWindow that window; null: BitBlt the screen rect
    long left;   // capture rect in virtual-screen coordinates
    long top;
    long right;
    long bottom;
    int max_edge;  // downscale so max(width,height) <= max_edge (engine/transport limit)
};

struct CaptureBits {
    QByteArray bgra;  // top-down BGRA8, exactly width*height*4 bytes
    int width = 0;
    int height = 0;
    double scale = 1.0;  // dest/source ratio; < 1 when downscaled to fit max_edge
};

// Capture per `req` into `out`. Returns false and sets `err` on any failure (empty/oversized
// region, GDI failure, protected/minimized window). Releases every GDI object on every path.
[[nodiscard]] bool captureBgra(const CaptureRequest& req, CaptureBits& out, QString& err);

struct WindowRect {
    void* hwnd = nullptr;  // the matched top-level window (HWND)
    long left = 0;         // window bounds in virtual-screen coordinates
    long top = 0;
    long right = 0;
    long bottom = 0;
};

// Find the first visible top-level window whose lower-cased title contains `needle_lower` and
// report its handle + bounds. Returns false and sets `err` when none match or the match is
// minimized (its pixels are not renderable for capture/OCR).
[[nodiscard]] bool windowRectByTitle(const QString& needle_lower, WindowRect& out, QString& err);

// Report the current foreground (active) top-level window's handle + bounds. This is how the
// active pop-up dialog -- which often has no title for windowRectByTitle to match -- is reached.
// Returns false and sets `err` when there is no foreground window or it is minimized.
[[nodiscard]] bool foregroundWindowRect(WindowRect& out, QString& err);

}  // namespace sak::win32mcp

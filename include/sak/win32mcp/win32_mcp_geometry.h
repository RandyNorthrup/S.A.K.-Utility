// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef SAK_WIN32MCP_WIN32_MCP_GEOMETRY_H
#define SAK_WIN32MCP_WIN32_MCP_GEOMETRY_H

#include <QString>

#include <cstdint>

namespace sak::win32mcp {

// An absurd raw capture rect cannot be allowed to allocate gigabytes before downscaling.
constexpr int kCaptureMaxEdge = 16'384;

// The per-edge cap alone still permits a 16384x16384 square = 256 megapixels x 4 bytes ~ 1 GiB
// allocated at full resolution BEFORE downscaling. Bound the total area so the pre-downscale
// surface stays a few hundred MiB at most; 64 megapixels x 4 = 256 MiB covers any real capture
// (a 4K monitor is ~8 megapixels) while rejecting the pathological maximal square.
constexpr int64_t kCaptureMaxPixels = 64LL * 1024 * 1024;

// A downscaled destination size plus the applied dest/source ratio (< 1 when downscaled, 1.0 when
// the source already fits).
struct Scaled {
    int w{0};
    int h{0};
    double scale{1.0};
};

// Downscale so max(w, h) <= max_edge, preserving aspect; each dest edge is floored at 1 so a very
// wide/short source never yields a zero dimension. max_edge <= 0 or an already-small source is a
// no-op (scale 1.0).
Scaled scaledSize(int src_w, int src_h, int max_edge);

// Empty string when a capture rect is usable; otherwise the reason it is rejected (non-positive, or
// an edge over kCaptureMaxEdge -- the ~1 GiB-alloc guard).
QString validateCaptureRect(int width, int height);

// The multiplier that maps a downscaled-capture coordinate back to source pixels: 1/scale, or 1.0
// when scale is non-positive (an un-scaled capture).
double inverseScale(double scale);

// A recognized word's bounding rect in downscaled-capture pixels (the shape an OCR engine reports).
struct WordRectF {
    double x{0.0};
    double y{0.0};
    double w{0.0};
    double h{0.0};
};

// A word box mapped to absolute virtual-screen pixels.
struct AbsBox {
    int x{0};
    int y{0};
    int w{0};
    int h{0};
};

// Map a recognized word's bounding rect back to absolute virtual-screen coordinates -- divide out
// the capture scale (inv = inverseScale(scale)) then add the capture origin. Rounds to nearest.
AbsBox mapWordBox(const WordRectF& rect, long origin_x, long origin_y, double inv);

}  // namespace sak::win32mcp

#endif  // SAK_WIN32MCP_WIN32_MCP_GEOMETRY_H

// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_geometry.h"

#include <algorithm>
#include <cmath>

namespace sak::win32mcp {

Scaled scaledSize(int src_w, int src_h, int max_edge) {
    const int longest = std::max(src_w, src_h);
    if (max_edge <= 0 || longest <= max_edge) {
        return Scaled{src_w, src_h, 1.0};
    }
    const double scale = static_cast<double>(max_edge) / static_cast<double>(longest);
    return Scaled{std::max(1, static_cast<int>(src_w * scale)),
                  std::max(1, static_cast<int>(src_h * scale)),
                  scale};
}

QString validateCaptureRect(int width, int height) {
    if (width <= 0 || height <= 0) {
        return QStringLiteral("The capture region is empty.");
    }
    if (width > kCaptureMaxEdge || height > kCaptureMaxEdge) {
        return QStringLiteral("The capture region is too large; target a single window.");
    }
    // Bound the TOTAL area too: width*height*4 bytes are allocated at full resolution before any
    // downscale, so a maximal square within the per-edge cap would still allocate ~1 GiB.
    if (static_cast<int64_t>(width) * height > kCaptureMaxPixels) {
        return QStringLiteral("The capture region is too large; target a single window.");
    }
    return {};
}

double inverseScale(double scale) {
    return (scale > 0.0) ? (1.0 / scale) : 1.0;
}

AbsBox mapWordBox(const WordRectF& rect, long origin_x, long origin_y, double inv) {
    return AbsBox{static_cast<int>(std::llround(origin_x + rect.x * inv)),
                  static_cast<int>(std::llround(origin_y + rect.y * inv)),
                  static_cast<int>(std::llround(rect.w * inv)),
                  static_cast<int>(std::llround(rect.h * inv))};
}

}  // namespace sak::win32mcp

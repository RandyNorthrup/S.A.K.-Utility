// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_fingerprint.h"

#include <algorithm>
#include <cstdlib>

namespace sak::win32mcp {

QByteArray fingerprint(const QByteArray& bgra, int width, int height) {
    if (width < 1 || height < 1) {
        return {};
    }
    const qsizetype needed = static_cast<qsizetype>(width) * height * 4;
    if (bgra.size() < needed) {
        return {};  // short/degenerate capture: treat as no fingerprint (reads as changed)
    }
    QByteArray fp(kFingerprintGrid * kFingerprintGrid, 0);
    const auto* pixels = reinterpret_cast<const unsigned char*>(bgra.constData());
    for (int gy = 0; gy < kFingerprintGrid; ++gy) {
        for (int gx = 0; gx < kFingerprintGrid; ++gx) {
            const int sx = std::min(width - 1, gx * width / kFingerprintGrid);
            const int sy = std::min(height - 1, gy * height / kFingerprintGrid);
            const qsizetype idx = ((static_cast<qsizetype>(sy) * width) + sx) * 4;
            const int gray = (pixels[idx] + pixels[idx + 1] + pixels[idx + 2]) / 3;
            fp[(gy * kFingerprintGrid) + gx] = static_cast<char>(gray);
        }
    }
    return fp;
}

double fingerprintDiff(const QByteArray& a, const QByteArray& b) {
    if (a.isEmpty() || a.size() != b.size()) {
        return 1.0;
    }
    int changed = 0;
    for (int i = 0; i < a.size(); ++i) {
        if (std::abs(static_cast<int>(static_cast<unsigned char>(a[i])) -
                     static_cast<int>(static_cast<unsigned char>(b[i]))) > kFingerprintTolerance) {
            ++changed;
        }
    }
    return static_cast<double>(changed) / static_cast<double>(a.size());
}

bool fingerprintChanged(const QByteArray& a, const QByteArray& b) {
    return fingerprintDiff(a, b) > kFingerprintChangedRatio;
}

}  // namespace sak::win32mcp

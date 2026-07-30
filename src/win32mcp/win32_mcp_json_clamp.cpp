// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_json_clamp.h"

#include <algorithm>

namespace sak::win32mcp {

qint64 clampMs(const QJsonObject& args, const QString& key, qint64 def, qint64 lo, qint64 hi) {
    // Clamp in the double domain first: a JSON number above INT64_MAX is a valid double, and
    // narrowing an out-of-range double to qint64 is undefined (MSVC yields INT64_MIN), which would
    // wrongly collapse a huge timeout to the floor. Pinning to [lo, hi] as doubles keeps the value
    // in qint64 range before the cast.
    const double value = args.value(key).toDouble(static_cast<double>(def));
    const double clamped = std::min(std::max(value, static_cast<double>(lo)),
                                    static_cast<double>(hi));
    return static_cast<qint64>(clamped);
}

}  // namespace sak::win32mcp

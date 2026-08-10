// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file format_utils.h
/// @brief Centralized formatting utilities for consistent display across the application

#pragma once

#include "sak/layout_constants.h"

#include <QString>

#include <cstdint>

namespace sak {

/// @brief Format a byte count into a human-readable string (B, KB, MB, GB, TB)
/// @param bytes Number of bytes to format
/// @return Formatted string (e.g., "1.50 GB", "256 KB", "0 bytes")
[[nodiscard]] inline QString formatBytes(qint64 bytes) {
    if (bytes <= 0) {
        return QStringLiteral("0 bytes");
    }

    constexpr qint64 kKB = 1024LL;
    constexpr qint64 kMB = 1024LL * 1024;
    constexpr qint64 kGB = 1024LL * 1024 * 1024;
    constexpr qint64 kTB = 1024LL * 1024 * 1024 * 1024;
    constexpr int kLargeUnitPrecision = 2;
    constexpr int kMediumUnitPrecision = 1;

    if (bytes >= kTB) {
        return QString("%1 TB").arg(static_cast<double>(bytes) / kTB, 0, 'f', kLargeUnitPrecision);
    }
    if (bytes >= kGB) {
        return QString("%1 GB").arg(static_cast<double>(bytes) / kGB, 0, 'f', kLargeUnitPrecision);
    }
    if (bytes >= kMB) {
        return QString("%1 MB").arg(static_cast<double>(bytes) / kMB, 0, 'f', kMediumUnitPrecision);
    }
    if (bytes >= kKB) {
        return QString("%1 KB").arg(static_cast<double>(bytes) / kKB, 0, 'f', kMediumUnitPrecision);
    }
    return QString("%1 bytes").arg(bytes);
}

/// @brief Format a byte count (unsigned) into a human-readable string
/// @param bytes Number of bytes to format
/// @return Formatted string (e.g., "1.50 TB", "256.0 MB")
/// @note Computed in unsigned arithmetic so a value above INT64_MAX is not
///       narrowed to a negative qint64 (which the signed overload would then
///       falsify as "0 bytes"); a hostile oversized size stays honest.
[[nodiscard]] inline QString formatBytes(uint64_t bytes) {
    if (bytes == 0) {
        return QStringLiteral("0 bytes");
    }

    constexpr uint64_t kKB = 1024ULL;
    constexpr uint64_t kMB = 1024ULL * 1024;
    constexpr uint64_t kGB = 1024ULL * 1024 * 1024;
    constexpr uint64_t kTB = 1024ULL * 1024 * 1024 * 1024;
    constexpr int kLargeUnitPrecision = 2;
    constexpr int kMediumUnitPrecision = 1;

    if (bytes >= kTB) {
        return QString("%1 TB").arg(static_cast<double>(bytes) / kTB, 0, 'f', kLargeUnitPrecision);
    }
    if (bytes >= kGB) {
        return QString("%1 GB").arg(static_cast<double>(bytes) / kGB, 0, 'f', kLargeUnitPrecision);
    }
    if (bytes >= kMB) {
        return QString("%1 MB").arg(static_cast<double>(bytes) / kMB, 0, 'f', kMediumUnitPrecision);
    }
    if (bytes >= kKB) {
        return QString("%1 KB").arg(static_cast<double>(bytes) / kKB, 0, 'f', kMediumUnitPrecision);
    }
    return QString("%1 bytes").arg(bytes);
}

}  // namespace sak

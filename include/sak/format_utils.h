// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file format_utils.h
/// @brief Centralized formatting utilities for consistent display across the application

#pragma once

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

    if (bytes >= kTB) {
        return QString("%1 TB").arg(static_cast<double>(bytes) / kTB, 0, 'f', 2);
    }
    if (bytes >= kGB) {
        return QString("%1 GB").arg(static_cast<double>(bytes) / kGB, 0, 'f', 2);
    }
    if (bytes >= kMB) {
        return QString("%1 MB").arg(static_cast<double>(bytes) / kMB, 0, 'f', 1);
    }
    if (bytes >= kKB) {
        return QString("%1 KB").arg(static_cast<double>(bytes) / kKB, 0, 'f', 1);
    }
    return QString("%1 bytes").arg(bytes);
}

/// @brief Format a byte count (unsigned) into a human-readable string
/// @param bytes Number of bytes to format
/// @return Formatted string (e.g., "1.50 TB", "256.0 MB")
[[nodiscard]] inline QString formatBytes(uint64_t bytes) {
    return formatBytes(static_cast<qint64>(bytes));
}

}  // namespace sak

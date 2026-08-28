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

/// @brief Format a byte count in the COMPACT style: "512 B", "1.5 KB", "2.0 MB", "1.50 GB".
/// @param bytes Number of bytes to format
/// @return Formatted string; a value below 1 KB (including zero and any negative) renders as "B".
///
/// This is the second of exactly two byte styles, and it exists because five call sites had
/// independently written it: the email inspector, the OST converter widget, the email attachments
/// browser, the email report generator and the conversion report generator. They differed only in
/// trivia -- one printed KB with integer division ("1 KB" for 1536 bytes), another printed GB to
/// one decimal instead of two -- which is what a duplicated formatter looks like after a few
/// years: the same intent, five spellings, none of them wrong enough to notice.
///
/// It is NOT merged into formatBytes() above because the difference is deliberate rather than
/// accidental. formatBytes() spells the smallest unit out as "bytes" and floors at "0 bytes",
/// which reads correctly in a sentence or a wide report column. This one stays short enough for a
/// table cell and a list row. Two named styles, one implementation each; a caller picks by name
/// instead of writing a sixth.
[[nodiscard]] inline QString formatBytesCompact(qint64 bytes) {
    constexpr qint64 kKB = 1024LL;
    constexpr qint64 kMB = 1024LL * 1024;
    constexpr qint64 kGB = 1024LL * 1024 * 1024;
    constexpr int kLargeUnitPrecision = 2;
    constexpr int kMediumUnitPrecision = 1;

    if (bytes < kKB) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (bytes < kMB) {
        return QStringLiteral("%1 KB").arg(
            static_cast<double>(bytes) / static_cast<double>(kKB), 0, 'f', kMediumUnitPrecision);
    }
    if (bytes < kGB) {
        return QStringLiteral("%1 MB").arg(
            static_cast<double>(bytes) / static_cast<double>(kMB), 0, 'f', kMediumUnitPrecision);
    }
    return QStringLiteral("%1 GB").arg(
        static_cast<double>(bytes) / static_cast<double>(kGB), 0, 'f', kLargeUnitPrecision);
}

}  // namespace sak

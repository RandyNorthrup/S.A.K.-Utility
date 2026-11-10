// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file format_utils.h
/// @brief Centralized formatting utilities for consistent display across the application

#pragma once

#include "sak/layout_constants.h"

#include <QString>

#include <cstdint>

namespace sak {

/// @brief "1 hop", "2 hops": @p count with the noun that agrees with it. Every count the app shows
///        is spelled this way, never "hop(s)" (UI review S24); core messages reach the window too.
[[nodiscard]] inline QString countOf(qint64 count, const QString& singular, const QString& plural) {
    return QStringLiteral("%1 %2").arg(count).arg(count == 1 ? singular : plural);
}

/// @brief A temperature as every view shows one: "43 degrees C", "71.5 degrees C" (UI review X17:
/// the SMART
///        table said "40 degC" beside the Thermal row's degree sign).
[[nodiscard]] inline QString formatCelsius(double celsius, int decimals) {
    return QStringLiteral("%1 \u00B0C").arg(celsius, 0, 'f', decimals);
}

inline constexpr char16_t kPathBreakChance = 0x200B;  ///< ZERO WIDTH SPACE
inline constexpr char16_t kPathNoBreak = 0x2060;      ///< WORD JOINER

/// @brief @p text as a wrapped view shows it: a line may break after each backslash, and not at a
///        hyphen. Qt breaks a path only at a hyphen or a space, so "C:\SAK-Samples\Docs" wrapped as
///        "C:\SAK-" and "Samples\Docs", two pieces that read as two names (review 4, Y28). A zero
///        width space follows each backslash and a word joiner each hyphen; withoutPathBreaks
///        takes them out again, so what is copied is the path as it is.
[[nodiscard]] inline QString withPathBreaks(const QString& text) {
    QString shown;
    for (const QChar ch : text) {
        shown.append(ch);
        if (ch == QLatin1Char('\\')) {
            shown.append(QChar(kPathBreakChance));
        } else if (ch == QLatin1Char('-')) {
            shown.append(QChar(kPathNoBreak));
        }
    }
    return shown;
}

/// @brief @p shown without the marks withPathBreaks put in.
[[nodiscard]] inline QString withoutPathBreaks(QString shown) {
    shown.remove(QChar(kPathBreakChance));
    shown.remove(QChar(kPathNoBreak));
    return shown;
}

/// @brief A byte count as the application shows every size: "512 B", "1.5 KB", "2.0 MB",
///        "1.50 GB", "7.28 TB".
///
/// One style, one implementation (review 4, Y10 and X7). There were two named styles here,
/// "242 bytes" and "242 B", and seven more written beside them in panels and dialogs: the AI page
/// printed "(3 KB)" by integer division beside "(527 B)", the backup wizard "4.9 GB" and "0.0 GB"
/// for profiles the restore showed as "4.88 GB" and "252.1 KB", Advanced Search's metadata "242
/// bytes" beside its list's "242 B". Every size the application shows comes from here;
/// test_one_byte_format fails on a size formatted anywhere else.
/// @note Computed in unsigned arithmetic so a value above INT64_MAX is not narrowed to a
///       negative qint64; a hostile oversized size stays honest.
[[nodiscard]] inline QString formatBytes(uint64_t bytes) {
    constexpr uint64_t kKB = 1024ULL;
    constexpr uint64_t kMB = 1024ULL * 1024;
    constexpr uint64_t kGB = 1024ULL * 1024 * 1024;
    constexpr uint64_t kTB = 1024ULL * 1024 * 1024 * 1024;
    constexpr int kLargeUnitPrecision = 2;
    constexpr int kMediumUnitPrecision = 1;

    if (bytes >= kTB) {
        return QStringLiteral("%1 TB").arg(
            static_cast<double>(bytes) / static_cast<double>(kTB), 0, 'f', kLargeUnitPrecision);
    }
    if (bytes >= kGB) {
        return QStringLiteral("%1 GB").arg(
            static_cast<double>(bytes) / static_cast<double>(kGB), 0, 'f', kLargeUnitPrecision);
    }
    if (bytes >= kMB) {
        return QStringLiteral("%1 MB").arg(
            static_cast<double>(bytes) / static_cast<double>(kMB), 0, 'f', kMediumUnitPrecision);
    }
    if (bytes >= kKB) {
        return QStringLiteral("%1 KB").arg(
            static_cast<double>(bytes) / static_cast<double>(kKB), 0, 'f', kMediumUnitPrecision);
    }
    return QStringLiteral("%1 B").arg(bytes);
}

/// @brief formatBytes for a signed count. A negative count is shown as it is ("-5 B"), not as
///        zero: it is a fault to see, not a size.
[[nodiscard]] inline QString formatBytes(qint64 bytes) {
    if (bytes < 0) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    return formatBytes(static_cast<uint64_t>(bytes));
}

}  // namespace sak

// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_grouping.cpp
/// @brief Group-key computation cloned from the Files app
///        (Utils/Storage/Collection/GroupingHelper.cs and
///        Services/DateTimeFormatter/AbstractDateTimeFormatter.cs).

#include "sak/file_explorer_grouping.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QLocale>

#include <algorithm>
#include <iterator>
#include <optional>

namespace sak {

namespace {

QString trGroup(const char* text) {
    return QCoreApplication::translate("FileExplorerGrouping", text);
}

// File timestamps on a foreign volume are attacker-controlled, and QDate::year()
// spans roughly +/-2.1 billion, so raw (year * 12) or (2 + daysTo) sort-index
// arithmetic would overflow a signed int (UB) or narrow a qint64 wrongly. Compute
// in the 64-bit domain and clamp every sort index into a bounded, monotonic range.
constexpr int kMaxDateSortIndex = 1'000'000'000;

// Day-difference cutoffs for the relative week buckets: a date is "earlier this
// week" only within one week, and "last week" only within two weeks, of today.
constexpr qint64 kDaysPerWeek = 7;
constexpr qint64 kDaysPerTwoWeeks = 2 * kDaysPerWeek;

// Fold a whole-year delta into months when ranking month/year buckets.
constexpr qint64 kMonthsPerYear = 12;

// Group sort indices (FileExplorerGroupInfo::sort_index), ranking sections
// newest-first. Future=-1, Today=0, and Yesterday=1 (bare identity literals)
// precede these. The relative buckets take fixed ranks; the open-ended
// per-day / per-month / per-year buckets take a base rank plus the (clamped)
// elapsed delta so they stay monotonic behind the relative labels.
constexpr int kSortEarlierThisWeek = 2;
constexpr int kSortLastWeek = 3;
constexpr int kSortEarlierThisMonth = 4;
constexpr int kSortLastMonth = 5;
constexpr int kSortEarlierThisYear = 6;
constexpr int kSortLastYear = 7;
constexpr int kSortDayBucketBase = 2;
constexpr qint64 kSortMonthBucketBase = 6;
constexpr qint64 kSortYearBucketBase = 8;

// Invalid / undated items sort to the very bottom.
constexpr int kSortUnknown = 9999;

// Files GroupingHelper sizeGroups table: strict greater-than thresholds
// evaluated largest-first; the header shows the bucket name and byte range.
struct SizeBucket {
    quint64 threshold;
    const char* name;
    const char* range;
};
// The thresholds are decimal (SI) byte counts, so the range labels use decimal
// units (GB/MB/KB) to match -- previously they read GiB/MiB/KiB, which name the
// binary (1024-based) sizes and so misdescribed every bucket boundary (B8-24).
constexpr SizeBucket kSizeBuckets[] = {
    {5'000'000'000ULL, "Huge", "5 GB +"},
    {1'000'000'000ULL, "Very large", "1 GB - 5 GB"},
    {128'000'000ULL, "Large", "128 MB - 1 GB"},
    {1'000'000ULL, "Medium", "1 MB - 128 MB"},
    {16'000ULL, "Small", "16 KB - 1 MB"},
};

FileExplorerGroupInfo sizeGroupInfo(const FileExplorerGroupSource& source) {
    if (source.directory) {
        // Files keys folders by their (usually empty) FileSizeDisplay, which
        // collapses them into one unlabeled group; a named group reads better.
        return {trGroup("Folders"), -1};
    }
    int index = static_cast<int>(std::size(kSizeBuckets));
    for (const SizeBucket& bucket : kSizeBuckets) {
        if (source.size_bytes > bucket.threshold) {
            return {QStringLiteral("%1 (%2)").arg(trGroup(bucket.name), trGroup(bucket.range)),
                    index};
        }
        --index;
    }
    return {QStringLiteral("%1 (%2)").arg(trGroup("Tiny"), trGroup("0 B - 16 KB")), 0};
}

// Files AbstractDateTimeFormatter.ToTimeSpanLabel ladder, split by rung. The
// buckets are checked most-recent-first; the sort index ranks sections
// newest-first and spreads per-day / per-month / per-year buckets behind the
// relative labels.
std::optional<FileExplorerGroupInfo> weekDateGroup(const QDate& date,
                                                   const QDate& today,
                                                   const qint64 day_diff) {
    int week_year = 0;
    int now_week_year = 0;
    const int week = date.weekNumber(&week_year);
    const int now_week = today.weekNumber(&now_week_year);
    if (day_diff <= kDaysPerWeek && week == now_week && week_year == now_week_year) {
        return FileExplorerGroupInfo{trGroup("Earlier this week"), kSortEarlierThisWeek};
    }
    int last_week_year = 0;
    const int last_week = today.addDays(-kDaysPerWeek).weekNumber(&last_week_year);
    if (day_diff <= kDaysPerTwoWeeks && week == last_week && week_year == last_week_year) {
        return FileExplorerGroupInfo{trGroup("Last week"), kSortLastWeek};
    }
    return std::nullopt;
}

FileExplorerGroupInfo monthAndYearDateGroup(const QDate& date,
                                            const QDate& today,
                                            const FileExplorerGroupDateUnit unit) {
    const qint64 year_delta = static_cast<qint64>(today.year()) - date.year();
    const qint64 month_diff = std::clamp<qint64>(
        year_delta * kMonthsPerYear + (today.month() - date.month()), 0, kMaxDateSortIndex);
    if (month_diff == 0) {
        return {trGroup("Earlier this month"), kSortEarlierThisMonth};
    }
    if (month_diff == 1) {
        return {trGroup("Last month"), kSortLastMonth};
    }
    if (unit == FileExplorerGroupDateUnit::Month) {
        return {QLocale().toString(date, QStringLiteral("MMMM yyyy")),
                static_cast<int>(
                    std::min<qint64>(kSortMonthBucketBase + month_diff, kMaxDateSortIndex))};
    }
    if (date.year() == today.year()) {
        return {trGroup("Earlier this year"), kSortEarlierThisYear};
    }
    if (date.year() == today.year() - 1) {
        return {trGroup("Last year"), kSortLastYear};
    }
    return {QString::number(date.year()),
            static_cast<int>(
                std::clamp<qint64>(kSortYearBucketBase + year_delta, 0, kMaxDateSortIndex))};
}

FileExplorerGroupInfo dateGroupInfo(const QDateTime& time,
                                    const FileExplorerGroupDateUnit unit,
                                    const QDateTime& now) {
    if (!time.isValid()) {
        return {trGroup("Unknown"), kSortUnknown};
    }
    const QDate date = time.date();
    const QDate today = now.date();
    const qint64 day_diff = date.daysTo(today);
    if (day_diff < 0) {
        return {trGroup("Future"), -1};
    }
    if (day_diff == 0) {
        return {trGroup("Today"), 0};
    }
    if (day_diff == 1) {
        return {trGroup("Yesterday"), 1};
    }
    if (unit == FileExplorerGroupDateUnit::Day) {
        return {QLocale().toString(date, QLocale::LongFormat),
                kSortDayBucketBase +
                    static_cast<int>(std::min<qint64>(day_diff, kMaxDateSortIndex))};
    }
    if (const auto week_group = weekDateGroup(date, today, day_diff)) {
        return *week_group;
    }
    return monthAndYearDateGroup(date, today, unit);
}

FileExplorerGroupInfo typeGroupInfo(const FileExplorerGroupSource& source) {
    // Files GroupingHelper: folders group by ItemType and sort above files
    // (SortIndexOverride); files group by lower-cased extension.
    if (source.directory) {
        return {source.type, 0};
    }
    const QString extension = QFileInfo(source.name).suffix().toLower();
    return {extension.isEmpty() ? source.type : extension, 1};
}

}  // namespace

FileExplorerGroupInfo fileExplorerGroupInfo(const FileExplorerGroupSource& source,
                                            const FileExplorerGroupOption option,
                                            const FileExplorerGroupDateUnit date_unit,
                                            const QDateTime& now) {
    switch (option) {
    case FileExplorerGroupOption::Name:
        // Files: the first character uppercased, digits and symbols as-is.
        return {source.name.isEmpty() ? QStringLiteral("?") : QString(source.name.at(0)).toUpper(),
                0};
    case FileExplorerGroupOption::DateModified:
        return dateGroupInfo(source.modified_time, date_unit, now);
    case FileExplorerGroupOption::DateCreated:
        return dateGroupInfo(source.created_time, date_unit, now);
    case FileExplorerGroupOption::Size:
        return sizeGroupInfo(source);
    case FileExplorerGroupOption::FileType:
        return typeGroupInfo(source);
    case FileExplorerGroupOption::FileTag:
        // Files: the first tag, or the literal Untagged bucket below tagged.
        return source.tags.isEmpty() ? FileExplorerGroupInfo{trGroup("Untagged"), 1}
                                     : FileExplorerGroupInfo{source.tags.first(), 0};
    case FileExplorerGroupOption::None:
        break;
    }
    return {};
}

QString fileExplorerGroupOptionName(const FileExplorerGroupOption option) {
    switch (option) {
    case FileExplorerGroupOption::Name:
        return QStringLiteral("name");
    case FileExplorerGroupOption::DateModified:
        return QStringLiteral("dateModified");
    case FileExplorerGroupOption::DateCreated:
        return QStringLiteral("dateCreated");
    case FileExplorerGroupOption::Size:
        return QStringLiteral("size");
    case FileExplorerGroupOption::FileType:
        return QStringLiteral("fileType");
    case FileExplorerGroupOption::FileTag:
        return QStringLiteral("fileTag");
    case FileExplorerGroupOption::None:
        break;
    }
    return QStringLiteral("none");
}

FileExplorerGroupOption fileExplorerGroupOptionFromName(const QString& name) {
    const QString clean = name.trimmed();
    if (clean == QStringLiteral("name")) {
        return FileExplorerGroupOption::Name;
    }
    if (clean == QStringLiteral("dateModified")) {
        return FileExplorerGroupOption::DateModified;
    }
    if (clean == QStringLiteral("dateCreated")) {
        return FileExplorerGroupOption::DateCreated;
    }
    if (clean == QStringLiteral("size")) {
        return FileExplorerGroupOption::Size;
    }
    if (clean == QStringLiteral("fileType")) {
        return FileExplorerGroupOption::FileType;
    }
    if (clean == QStringLiteral("fileTag")) {
        return FileExplorerGroupOption::FileTag;
    }
    return FileExplorerGroupOption::None;
}

}  // namespace sak

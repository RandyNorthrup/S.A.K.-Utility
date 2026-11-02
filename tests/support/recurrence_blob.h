// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file recurrence_blob.h
/// @brief Writes the MS-OXOCAL binary forms of a repeating appointment, for tests and the sample.
///
/// Laid out from the specification's field tables (2.2.1.44), not from the reader, so that a shared
/// wrong assumption shows up against real stores, where the same reader is counted by a separate
/// script that parses these fields itself.

#pragma once

#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QString>
#include <QtEndian>
#include <QTimeZone>
#include <QVector>

#include <algorithm>
#include <cstdint>

namespace sak::pst_fixture {

inline constexpr uint16_t kRecurDaily = 0x200A;
inline constexpr uint16_t kRecurWeekly = 0x200B;
inline constexpr uint16_t kRecurMonthly = 0x200C;
inline constexpr uint16_t kRecurYearly = 0x200D;
inline constexpr uint16_t kPatternTypeDay = 0;
inline constexpr uint16_t kPatternTypeWeek = 1;
inline constexpr uint16_t kPatternTypeMonth = 2;
inline constexpr uint16_t kPatternTypeMonthNth = 3;
inline constexpr uint16_t kPatternTypeMonthEnd = 4;
inline constexpr uint16_t kPatternTypeHijriMonth = 0x000A;
inline constexpr uint16_t kCalendarHijri = 6;
inline constexpr uint32_t kRecurEndByDate = 0x2021;
inline constexpr uint32_t kRecurEndAfterCount = 0x2022;
inline constexpr uint32_t kRecurEndNever = 0x2023;
inline constexpr uint32_t kRecurNoEndDate = 0x5A'E9'80'DF;
inline constexpr int kMaskSunday = 0x01;
inline constexpr int kMaskMonday = 0x02;
inline constexpr int kMaskTuesday = 0x04;
inline constexpr int kMaskWednesday = 0x08;
inline constexpr int kMaskThursday = 0x10;
inline constexpr int kMaskFriday = 0x20;
inline constexpr int kMaskSaturday = 0x40;
inline constexpr int kMaskWeekdays = 0x3E;

/// One occurrence moved or edited. Times are wall-clock.
struct RecurChangedOccurrence {
    QDateTime original_start;
    QDateTime start;
    QDateTime end;
    QString subject;   ///< Empty: not overridden
    QString location;  ///< Empty: not overridden
    bool body_changed = false;
};

struct RecurrenceSpec {
    uint16_t frequency = kRecurWeekly;
    uint16_t pattern_type = kPatternTypeWeek;
    uint16_t calendar_type = 0;
    uint32_t period = 1;
    uint32_t sliding = 0;
    uint32_t day_or_mask = kMaskMonday;
    uint32_t nth = 0;
    uint32_t end_type = kRecurEndNever;
    uint32_t occurrence_count = 0;
    uint32_t first_day_of_week = 0;
    QVector<QDate> deleted;
    QVector<RecurChangedOccurrence> changed;
    QDate start_date;
    QDate end_date;  ///< Invalid: Outlook's "no end" value
    int start_minutes = 540;
    int end_minutes = 600;
    uint16_t reader_version = 0x3004;
    uint32_t reader_version2 = 0x3006;
    uint32_t writer_version2 = 0x3009;
    /// Write fewer modified dates than there are changed occurrences, as a damaged blob might.
    bool drop_modified_dates = false;
};

inline uint32_t recurMinutes(const QDateTime& wall) {
    const QDateTime epoch(QDate(1601, 1, 1), QTime(0, 0), QTimeZone::UTC);
    return static_cast<uint32_t>(epoch.secsTo(wall) / 60);
}

inline uint32_t recurMinutes(const QDate& date) {
    return recurMinutes(QDateTime(date, QTime(0, 0), QTimeZone::UTC));
}

inline void recurPut16(QByteArray& out, uint16_t value) {
    const uint16_t le = qToLittleEndian(value);
    out.append(reinterpret_cast<const char*>(&le), sizeof(le));
}

inline void recurPut32(QByteArray& out, uint32_t value) {
    const uint32_t le = qToLittleEndian(value);
    out.append(reinterpret_cast<const char*>(&le), sizeof(le));
}

/// The fixed head of a RecurrencePattern, through its pattern-specific fields.
inline void recurPutPatternHead(QByteArray& out, const RecurrenceSpec& spec) {
    recurPut16(out, spec.reader_version);
    recurPut16(out, spec.reader_version);
    recurPut16(out, spec.frequency);
    recurPut16(out, spec.pattern_type);
    recurPut16(out, spec.calendar_type);
    recurPut32(out, 0);  // FirstDateTime, which only anchors an expansion
    recurPut32(out, spec.period);
    recurPut32(out, spec.sliding);
    if (spec.pattern_type == kPatternTypeWeek || spec.pattern_type == kPatternTypeMonth ||
        spec.pattern_type == kPatternTypeMonthEnd || spec.pattern_type == kPatternTypeHijriMonth) {
        recurPut32(out, spec.day_or_mask);
    } else if (spec.pattern_type == kPatternTypeMonthNth) {
        recurPut32(out, spec.day_or_mask);
        recurPut32(out, spec.nth);
    }
    recurPut32(out, spec.end_type);
    recurPut32(out, spec.occurrence_count);
    recurPut32(out, spec.first_day_of_week);
}

/// The deleted and modified date lists, then the pattern's own start and end.
inline void recurPutOccurrenceLists(QByteArray& out, const RecurrenceSpec& spec) {
    // Deleted dates also hold the original date of every changed occurrence, sorted.
    QVector<uint32_t> deleted;
    for (const QDate& date : spec.deleted) {
        deleted.append(recurMinutes(date));
    }
    for (const RecurChangedOccurrence& changed : spec.changed) {
        deleted.append(recurMinutes(changed.original_start.date()));
    }
    std::sort(deleted.begin(), deleted.end());
    recurPut32(out, static_cast<uint32_t>(deleted.size()));
    for (const uint32_t date : deleted) {
        recurPut32(out, date);
    }
    if (spec.drop_modified_dates) {
        recurPut32(out, 0);
    } else {
        recurPut32(out, static_cast<uint32_t>(spec.changed.size()));
        for (const RecurChangedOccurrence& changed : spec.changed) {
            recurPut32(out, recurMinutes(changed.start.date()));
        }
    }
    recurPut32(out, recurMinutes(spec.start_date));
    recurPut32(out, spec.end_date.isValid() ? recurMinutes(spec.end_date) : kRecurNoEndDate);
}

inline void recurPutAnsi(QByteArray& out, const QString& text) {
    const QByteArray ansi = text.toLatin1();
    recurPut16(out, static_cast<uint16_t>(ansi.size() + 1));
    recurPut16(out, static_cast<uint16_t>(ansi.size()));
    out.append(ansi);
}

/// One ExceptionInfo. A reminder override sits between the subject and the location, so a reader
/// that miscounts fields loses the location here.
inline void recurPutExceptionInfo(QByteArray& out, const RecurChangedOccurrence& changed) {
    recurPut32(out, recurMinutes(changed.start));
    recurPut32(out, recurMinutes(changed.end));
    recurPut32(out, recurMinutes(changed.original_start));
    uint16_t flags = 0x0008;
    flags |= changed.subject.isEmpty() ? 0 : 0x0001;
    flags |= changed.location.isEmpty() ? 0 : 0x0010;
    flags |= changed.body_changed ? 0x0200 : 0;
    recurPut16(out, flags);
    if (!changed.subject.isEmpty()) {
        recurPutAnsi(out, changed.subject);
    }
    recurPut32(out, 1);  // ReminderSet
    if (!changed.location.isEmpty()) {
        recurPutAnsi(out, changed.location);
    }
}

/// One ExtendedException: the Unicode copies of what the ExceptionInfo overrode.
inline void recurPutExtendedException(QByteArray& out,
                                      const RecurChangedOccurrence& changed,
                                      uint32_t writer_version2) {
    if (writer_version2 >= 0x3009) {
        recurPut32(out, 4);  // ChangeHighlightSize
        recurPut32(out, 0);  // ChangeHighlightValue
    }
    recurPut32(out, 0);      // ReservedBlockEE1Size
    if (changed.subject.isEmpty() && changed.location.isEmpty()) {
        return;
    }
    recurPut32(out, recurMinutes(changed.start));
    recurPut32(out, recurMinutes(changed.end));
    recurPut32(out, recurMinutes(changed.original_start));
    for (const QString& text : {changed.subject, changed.location}) {
        if (!text.isEmpty()) {
            recurPut16(out, static_cast<uint16_t>(text.size()));
            out.append(reinterpret_cast<const char*>(text.utf16()), text.size() * 2);
        }
    }
    recurPut32(out, 0);  // ReservedBlockEE2Size
}

/// An AppointmentRecur blob (MS-OXOCAL 2.2.1.44.5).
inline QByteArray buildRecurrenceBlob(const RecurrenceSpec& spec) {
    QByteArray out;
    recurPutPatternHead(out, spec);
    recurPutOccurrenceLists(out, spec);
    recurPut32(out, spec.reader_version2);
    recurPut32(out, spec.writer_version2);
    recurPut32(out, static_cast<uint32_t>(spec.start_minutes));
    recurPut32(out, static_cast<uint32_t>(spec.end_minutes));
    recurPut16(out, static_cast<uint16_t>(spec.changed.size()));
    for (const RecurChangedOccurrence& changed : spec.changed) {
        recurPutExceptionInfo(out, changed);
    }
    recurPut32(out, 0);  // ReservedBlock1Size
    for (const RecurChangedOccurrence& changed : spec.changed) {
        recurPutExtendedException(out, changed, spec.writer_version2);
    }
    recurPut32(out, 0);  // ReservedBlock2Size
    return out;
}

/// A rule for one daylight change, for the zone builders below.
struct ZoneChangeSpec {
    int month = 0;
    int day_of_week = 0;
    int week = 0;
    int hour = 0;
};

inline void recurPutSystemTime(QByteArray& out, const ZoneChangeSpec& change) {
    recurPut16(out, 0);  // wYear: 0 means "every year, by the rule"
    recurPut16(out, static_cast<uint16_t>(change.month));
    recurPut16(out, static_cast<uint16_t>(change.day_of_week));
    recurPut16(out, static_cast<uint16_t>(change.week));
    recurPut16(out, static_cast<uint16_t>(change.hour));
    recurPut16(out, 0);
    recurPut16(out, 0);
    recurPut16(out, 0);
}

/// PidLidTimeZoneStruct. @p bias is minutes behind UTC: US Pacific is 480, Paris is -60.
inline QByteArray buildTimeZoneStruct(int bias,
                                      int daylight_bias,
                                      const ZoneChangeSpec& to_standard,
                                      const ZoneChangeSpec& to_daylight) {
    QByteArray out;
    recurPut32(out, static_cast<uint32_t>(bias));
    recurPut32(out, 0);  // lStandardBias
    recurPut32(out, static_cast<uint32_t>(daylight_bias));
    recurPut16(out, 0);  // wStandardYear
    recurPutSystemTime(out, to_standard);
    recurPut16(out, 0);  // wDaylightYear
    recurPutSystemTime(out, to_daylight);
    return out;
}

/// A TZDEFINITION with one rule (MS-OXOCAL 2.2.1.41).
inline QByteArray buildTimeZoneDefinition(const QString& key,
                                          int bias,
                                          int daylight_bias,
                                          const ZoneChangeSpec& to_standard,
                                          const ZoneChangeSpec& to_daylight) {
    QByteArray out;
    out.append(char{2});                                           // bMajorVersion
    out.append(char{1});                                           // bMinorVersion
    recurPut16(out, static_cast<uint16_t>(6 + (key.size() * 2)));  // cbHeader
    recurPut16(out, 0);                                            // wReserved
    recurPut16(out, static_cast<uint16_t>(key.size()));
    out.append(reinterpret_cast<const char*>(key.utf16()), key.size() * 2);
    recurPut16(out, 1);       // cRules
    out.append(char{2});      // TZRULE bMajorVersion
    out.append(char{1});
    recurPut16(out, 0);       // wReserved
    recurPut16(out, 0x0003);  // Recurring and effective
    recurPut16(out, 2026);    // wYear
    out.append(QByteArray(14, '\0'));
    recurPut32(out, static_cast<uint32_t>(bias));
    recurPut32(out, 0);
    recurPut32(out, static_cast<uint32_t>(daylight_bias));
    recurPutSystemTime(out, to_standard);
    recurPutSystemTime(out, to_daylight);
    return out;
}

/// United States Pacific time: back on the first Sunday of November, on the second of March.
inline QByteArray pacificZoneStruct() {
    return buildTimeZoneStruct(480, -60, {11, 0, 1, 2}, {3, 0, 2, 2});
}

}  // namespace sak::pst_fixture

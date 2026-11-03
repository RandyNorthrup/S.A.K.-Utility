// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_pst_recurrence.cpp
/// @brief Reading a repeating appointment's pattern and time zone, and failing clearly when it
///        cannot be read exactly. A wrong rule puts meetings on wrong days without anyone noticing.

#include "sak/pst_recurrence.h"

#include "../support/recurrence_blob.h"

#include <QtTest/QtTest>

#include <array>
#include <cstring>
#include <vector>

#include <windows.h>

using namespace sak::pst_fixture;
using sak::pst::parseAppointmentRecurrence;
using sak::pst::parseTimeZoneDefinition;
using sak::pst::parseTimeZoneStruct;
using sak::pst::zoneUtcToWallClock;
using sak::pst::zoneWallClockToUtc;

namespace {

RecurrenceSpec weeklyMondays() {
    RecurrenceSpec spec;
    spec.start_date = QDate(2026, 11, 2);  // A Monday
    return spec;
}

sak::PstRecurrence read(const RecurrenceSpec& spec, bool all_day = false) {
    return parseAppointmentRecurrence(buildRecurrenceBlob(spec), {}, all_day);
}

QDateTime utcAt(int year, int month, int day, int hour, int minute = 0) {
    return QDateTime(QDate(year, month, day), QTime(hour, minute), QTimeZone::UTC);
}

}  // namespace

namespace {
// The registry's value for a zone (REG_TZI_FORMAT).
struct RegTziFormat {
    LONG bias;
    LONG standard_bias;
    LONG daylight_bias;
    SYSTEMTIME standard_date;
    SYSTEMTIME daylight_date;
};

// PidLidTimeZoneStruct is the same fields with a year in front of each date.
QByteArray zoneStructFrom(const RegTziFormat& tzi) {
    QByteArray raw;
    const auto append = [&raw](const auto& value) {
        raw.append(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    append(tzi.bias);
    append(tzi.standard_bias);
    append(tzi.daylight_bias);
    append(WORD{0});
    append(tzi.standard_date);
    append(WORD{0});
    append(tzi.daylight_date);
    return raw;
}

SYSTEMTIME systemTimeOf(const QDateTime& utc) {
    SYSTEMTIME time{};
    time.wYear = static_cast<WORD>(utc.date().year());
    time.wMonth = static_cast<WORD>(utc.date().month());
    time.wDay = static_cast<WORD>(utc.date().day());
    time.wHour = static_cast<WORD>(utc.time().hour());
    time.wMinute = static_cast<WORD>(utc.time().minute());
    return time;
}

QDateTime dateTimeOf(const SYSTEMTIME& time) {
    return QDateTime(QDate(time.wYear, time.wMonth, time.wDay),
                     QTime(time.wHour, time.wMinute),
                     QTimeZone::UTC);
}

// What this code and Windows disagree on, or empty. A wall-clock time in the hour a change skips or
// repeats names no one instant, so only the direction with one answer, UTC to wall clock, is held
// to every hour.
QString firstDisagreementWithWindows(const sak::PstTimeZone& zone, const RegTziFormat& tzi) {
    TIME_ZONE_INFORMATION windows{};
    windows.Bias = tzi.bias;
    windows.StandardBias = tzi.standard_bias;
    windows.DaylightBias = tzi.daylight_bias;
    windows.StandardDate = tzi.standard_date;
    windows.DaylightDate = tzi.daylight_date;
    const QDateTime first(QDate(2026, 1, 1), QTime(0, 0), QTimeZone::UTC);
    constexpr int kHoursInAYear = 365 * 24;
    for (int hour = 0; hour < kHoursInAYear; ++hour) {
        const QDateTime utc = first.addSecs(static_cast<qint64>(hour) * 3600);
        const SYSTEMTIME utc_time = systemTimeOf(utc);
        SYSTEMTIME local{};
        if (SystemTimeToTzSpecificLocalTime(&windows, &utc_time, &local) == 0) {
            return QStringLiteral("Windows refused %1").arg(utc.toString(Qt::ISODate));
        }
        const QDateTime ours = sak::pst::zoneUtcToWallClock(zone, utc);
        if (ours != dateTimeOf(local)) {
            return QStringLiteral("%1 UTC: ours %2, Windows %3")
                .arg(utc.toString(Qt::ISODate),
                     ours.toString(Qt::ISODate),
                     dateTimeOf(local).toString(Qt::ISODate));
        }
    }
    return {};
}
}  // namespace

class PstRecurrenceTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void daily() {
        RecurrenceSpec spec = weeklyMondays();
        spec.frequency = kRecurDaily;
        spec.pattern_type = kPatternTypeDay;
        spec.period = 1440;
        const auto every_day = read(spec);
        QCOMPARE(every_day.state, sak::RecurrenceState::Readable);
        QCOMPARE(every_day.rrule, QStringLiteral("FREQ=DAILY;INTERVAL=1"));
        QCOMPARE(every_day.description, QStringLiteral("Every day, no end date"));

        spec.period = 1440 * 3;
        const auto every_third = read(spec);
        QCOMPARE(every_third.rrule, QStringLiteral("FREQ=DAILY;INTERVAL=3"));
        QCOMPARE(every_third.description, QStringLiteral("Every 3 days, no end date"));
    }

    void anIntervalOfPartOfADayIsNotGuessedAt() {
        RecurrenceSpec spec = weeklyMondays();
        spec.frequency = kRecurDaily;
        spec.pattern_type = kPatternTypeDay;
        spec.period = 1000;
        QCOMPARE(read(spec).state, sak::RecurrenceState::Unreadable);
    }

    void everyWeekdayIsStoredAsDailyOverAWeek() {
        RecurrenceSpec spec = weeklyMondays();
        spec.frequency = kRecurDaily;
        spec.pattern_type = kPatternTypeWeek;
        spec.day_or_mask = kMaskWeekdays;
        const auto result = read(spec);
        QCOMPARE(result.rrule,
                 QStringLiteral("FREQ=WEEKLY;INTERVAL=1;BYDAY=MO,TU,WE,TH,FR;WKST=SU"));
        QCOMPARE(result.description, QStringLiteral("Every weekday, no end date"));
    }

    void weeklyOnSeveralDaysEveryOtherWeek() {
        RecurrenceSpec spec = weeklyMondays();
        spec.period = 2;
        spec.day_or_mask = kMaskMonday | kMaskWednesday;
        spec.first_day_of_week = 1;
        const auto result = read(spec);
        QCOMPARE(result.rrule, QStringLiteral("FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,WE;WKST=MO"));
        QCOMPARE(result.description,
                 QStringLiteral("Every 2 weeks on Monday and Wednesday, no end date"));
    }

    void aWeeklyRuleThatMissesItsOwnFirstDateIsRefused() {
        RecurrenceSpec spec = weeklyMondays();
        spec.day_or_mask = kMaskTuesday;
        QCOMPARE(read(spec).state, sak::RecurrenceState::Unreadable);
    }

    void monthlyOnADay() {
        RecurrenceSpec spec;
        spec.frequency = kRecurMonthly;
        spec.pattern_type = kPatternTypeMonth;
        spec.period = 1;
        spec.day_or_mask = 15;
        spec.start_date = QDate(2026, 11, 15);
        const auto result = read(spec);
        QCOMPARE(result.rrule, QStringLiteral("FREQ=MONTHLY;INTERVAL=1;BYMONTHDAY=15"));
        QCOMPARE(result.description, QStringLiteral("Every month on day 15, no end date"));
    }

    // Outlook moves day 31 to the last day of a short month; RFC 5545 would skip the month.
    void theLastDaysOfAMonthKeepOutlooksMeaning() {
        RecurrenceSpec spec;
        spec.frequency = kRecurMonthly;
        spec.pattern_type = kPatternTypeMonth;
        spec.period = 1;
        spec.start_date = QDate(2026, 10, 31);
        spec.day_or_mask = 31;
        QCOMPARE(read(spec).rrule, QStringLiteral("FREQ=MONTHLY;INTERVAL=1;BYMONTHDAY=-1"));

        spec.start_date = QDate(2026, 11, 30);
        spec.day_or_mask = 30;
        QCOMPARE(read(spec).rrule,
                 QStringLiteral("FREQ=MONTHLY;INTERVAL=1;BYMONTHDAY=28,29,30;BYSETPOS=-1"));

        spec.start_date = QDate(2026, 12, 29);
        spec.day_or_mask = 29;
        QCOMPARE(read(spec).rrule,
                 QStringLiteral("FREQ=MONTHLY;INTERVAL=1;BYMONTHDAY=28,29;BYSETPOS=-1"));

        // Starting in a short month, on that month's last day, is still day 31.
        spec.start_date = QDate(2026, 11, 30);
        spec.day_or_mask = 31;
        QCOMPARE(read(spec).state, sak::RecurrenceState::Readable);
    }

    void aMonthlyRuleThatMissesItsOwnFirstDateIsRefused() {
        RecurrenceSpec spec;
        spec.frequency = kRecurMonthly;
        spec.pattern_type = kPatternTypeMonth;
        spec.period = 1;
        spec.day_or_mask = 15;
        spec.start_date = QDate(2026, 11, 16);
        QCOMPARE(read(spec).state, sak::RecurrenceState::Unreadable);
    }

    void monthlyOnAnNthWeekday() {
        RecurrenceSpec spec;
        spec.frequency = kRecurMonthly;
        spec.pattern_type = kPatternTypeMonthNth;
        spec.period = 2;
        spec.day_or_mask = kMaskTuesday;
        spec.nth = 2;
        spec.start_date = QDate(2026, 11, 10);
        const auto second = read(spec);
        QCOMPARE(second.rrule, QStringLiteral("FREQ=MONTHLY;INTERVAL=2;BYDAY=TU;BYSETPOS=2"));
        QCOMPARE(second.description,
                 QStringLiteral("Every 2 months on the second Tuesday, no end date"));

        spec.nth = 5;  // MS-OXOCAL: 5 is the last
        spec.period = 1;
        spec.start_date = QDate(2026, 11, 24);
        const auto last = read(spec);
        QCOMPARE(last.rrule, QStringLiteral("FREQ=MONTHLY;INTERVAL=1;BYDAY=TU;BYSETPOS=-1"));
        QCOMPARE(last.description, QStringLiteral("Every month on the last Tuesday, no end date"));
    }

    // The start date is the first occurrence. One real yearly series' was not on its own rule, and
    // its other dates named another month: nothing says which is right, so it says so.
    void anNthRuleWhoseFirstDateIsOffItCannotBeRead() {
        RecurrenceSpec spec;
        spec.frequency = kRecurYearly;
        spec.pattern_type = kPatternTypeMonthNth;
        spec.period = 12;
        spec.day_or_mask = kMaskThursday;
        spec.nth = 5;
        spec.start_date = QDate(2026, 11, 5);  // The first Thursday, of a "last Thursday" rule
        const auto off_by_position = read(spec);
        QCOMPARE(off_by_position.state, sak::RecurrenceState::Unreadable);
        QVERIFY(off_by_position.rrule.isEmpty());
        QCOMPARE(off_by_position.problem,
                 QStringLiteral("its first date is not on the weekday its pattern names"));

        spec.start_date = QDate(2026, 11, 27);  // The right week, the wrong day
        QCOMPARE(read(spec).state, sak::RecurrenceState::Unreadable);

        spec.start_date = QDate(2026, 11, 26);
        const auto on_it = read(spec);
        QCOMPARE(on_it.state, sak::RecurrenceState::Readable);
        QCOMPARE(on_it.wall_start, utcAt(2026, 11, 26, 9));
        QCOMPARE(on_it.wall_end, utcAt(2026, 11, 26, 10));

        spec.frequency = kRecurMonthly;
        spec.period = 1;
        spec.day_or_mask = kMaskTuesday;
        spec.nth = 2;
        spec.start_date = QDate(2026, 11, 3);  // The first Tuesday, of a "second Tuesday" rule
        QCOMPARE(read(spec).state, sak::RecurrenceState::Unreadable);
    }

    // Every weekday, Nth of them: the set is counted, not the weeks.
    void anNthWeekdayCountsTheDaysInItsSet() {
        RecurrenceSpec spec;
        spec.frequency = kRecurMonthly;
        spec.pattern_type = kPatternTypeMonthNth;
        spec.period = 1;
        spec.day_or_mask = kMaskWeekdays;
        spec.nth = 2;
        spec.start_date = QDate(2026, 11, 3);  // The 1st is a Sunday: the second weekday is the 3rd
        QCOMPARE(read(spec).state, sak::RecurrenceState::Readable);
        spec.start_date = QDate(2026, 11, 2);
        QCOMPARE(read(spec).state, sak::RecurrenceState::Unreadable);
    }

    void monthlyOnTheLastDay() {
        RecurrenceSpec spec;
        spec.frequency = kRecurMonthly;
        spec.pattern_type = kPatternTypeMonthEnd;
        spec.period = 1;
        spec.day_or_mask = 31;
        spec.start_date = QDate(2026, 11, 30);
        QCOMPARE(read(spec).rrule, QStringLiteral("FREQ=MONTHLY;INTERVAL=1;BYMONTHDAY=-1"));
    }

    void yearlyOnADay() {
        RecurrenceSpec spec;
        spec.frequency = kRecurYearly;
        spec.pattern_type = kPatternTypeMonth;
        spec.period = 12;
        spec.day_or_mask = 4;
        spec.start_date = QDate(2027, 3, 4);
        const auto result = read(spec);
        QCOMPARE(result.rrule, QStringLiteral("FREQ=YEARLY;INTERVAL=1;BYMONTH=3;BYMONTHDAY=4"));
        QCOMPARE(result.description, QStringLiteral("Every year on March 4, no end date"));
    }

    void yearlyOnAnNthWeekday() {
        RecurrenceSpec spec;
        spec.frequency = kRecurYearly;
        spec.pattern_type = kPatternTypeMonthNth;
        spec.period = 24;
        spec.day_or_mask = kMaskMonday;
        spec.nth = 1;
        spec.start_date = QDate(2026, 10, 5);
        const auto result = read(spec);
        QCOMPARE(result.rrule,
                 QStringLiteral("FREQ=YEARLY;INTERVAL=2;BYMONTH=10;BYDAY=MO;BYSETPOS=1"));
        QCOMPARE(result.description,
                 QStringLiteral("Every 2 years on the first Monday of October, no end date"));
    }

    void anEndAfterACount() {
        RecurrenceSpec spec = weeklyMondays();
        spec.end_type = kRecurEndAfterCount;
        spec.occurrence_count = 10;
        const auto result = read(spec);
        QVERIFY2(result.rrule.endsWith(QStringLiteral(";COUNT=10")), qPrintable(result.rrule));
        QVERIFY(result.description.endsWith(QStringLiteral(", 10 times")));
    }

    void theEndDateTakesTheFormOfTheStart() {
        RecurrenceSpec spec = weeklyMondays();
        spec.end_type = kRecurEndByDate;
        spec.end_date = QDate(2027, 1, 25);

        // With a zone, UNTIL is UTC, and the last occurrence's own offset is what converts it:
        // 09:00 on 25 January in Pacific standard time is 17:00 UTC.
        const sak::PstTimeZone pacific = parseTimeZoneStruct(pacificZoneStruct(),
                                                             QStringLiteral("Pacific"));
        const auto zoned = parseAppointmentRecurrence(buildRecurrenceBlob(spec), pacific, false);
        QVERIFY2(zoned.rrule.endsWith(QStringLiteral(";UNTIL=20270125T170000Z")),
                 qPrintable(zoned.rrule));
        QVERIFY(zoned.description.endsWith(QStringLiteral(", until January 25, 2027")));

        // With no zone the times float, and so does UNTIL.
        QVERIFY2(read(spec).rrule.endsWith(QStringLiteral(";UNTIL=20270125T090000")),
                 qPrintable(read(spec).rrule));

        // An all-day event is dates throughout.
        QVERIFY2(read(spec, true).rrule.endsWith(QStringLiteral(";UNTIL=20270125")),
                 qPrintable(read(spec, true).rrule));
    }

    void outlooksFarFutureEndDateMeansNoEnd() {
        RecurrenceSpec spec = weeklyMondays();
        spec.end_type = kRecurEndByDate;
        const auto result = read(spec);
        QCOMPARE(result.state, sak::RecurrenceState::Readable);
        QVERIFY2(!result.rrule.contains(QStringLiteral("UNTIL")), qPrintable(result.rrule));
    }

    void theSeriesTimesComeFromThePattern() {
        RecurrenceSpec spec = weeklyMondays();
        spec.start_minutes = 9 * 60 + 30;
        spec.end_minutes = 25 * 60;  // Runs past midnight
        const auto result = read(spec);
        QCOMPARE(result.wall_start, utcAt(2026, 11, 2, 9, 30));
        QCOMPARE(result.wall_end, utcAt(2026, 11, 3, 1));
    }

    // The deleted list also names every changed occurrence's original date.
    void deletedAndChangedOccurrencesAreToldApart() {
        RecurrenceSpec spec = weeklyMondays();
        spec.deleted = {QDate(2026, 11, 9), QDate(2026, 11, 16)};
        RecurChangedOccurrence moved;
        moved.original_start = utcAt(2026, 11, 23, 9);
        moved.start = utcAt(2026, 11, 24, 14);
        moved.end = utcAt(2026, 11, 24, 15);
        moved.subject = QStringLiteral("Moved review");
        moved.location = QStringLiteral("Back room");
        spec.changed = {moved};

        const auto result = read(spec);
        QCOMPARE(result.state, sak::RecurrenceState::Readable);
        QCOMPARE(result.deleted,
                 QVector<QDateTime>({utcAt(2026, 11, 9, 9), utcAt(2026, 11, 16, 9)}));
        QCOMPARE(result.exceptions.size(), 1);
        const auto& exception = result.exceptions.first();
        QCOMPARE(exception.original_start, utcAt(2026, 11, 23, 9));
        QCOMPARE(exception.start, utcAt(2026, 11, 24, 14));
        QCOMPARE(exception.end, utcAt(2026, 11, 24, 15));
        QCOMPARE(exception.subject, QStringLiteral("Moved review"));
        QCOMPARE(exception.location, QStringLiteral("Back room"));
        QVERIFY(result.description.endsWith(QStringLiteral(". 2 deleted, 1 changed")));
    }

    // Past Latin-1 only the Unicode copy has the text.
    void aChangedOccurrenceKeepsItsUnicodeText() {
        RecurrenceSpec spec = weeklyMondays();
        RecurChangedOccurrence moved;
        moved.original_start = utcAt(2026, 11, 23, 9);
        moved.start = utcAt(2026, 11, 24, 14);
        moved.end = utcAt(2026, 11, 24, 15);
        moved.subject = QStringLiteral("R\u00E9union \u4F1A\u8BAE");
        spec.changed = {moved};
        const auto result = read(spec);
        QCOMPARE(result.exceptions.first().subject, QStringLiteral("R\u00E9union \u4F1A\u8BAE"));
    }

    void anOlderWriterHasNoChangeHighlight() {
        RecurrenceSpec spec = weeklyMondays();
        spec.writer_version2 = 0x3008;
        RecurChangedOccurrence moved;
        moved.original_start = utcAt(2026, 11, 23, 9);
        moved.start = utcAt(2026, 11, 24, 14);
        moved.end = utcAt(2026, 11, 24, 15);
        // Past Latin-1, so the text can only have come from the Unicode copy, read at the right
        // place.
        moved.location = QStringLiteral("Annex \u4F1A");
        spec.changed = {moved};
        QCOMPARE(read(spec).exceptions.first().location, QStringLiteral("Annex \u4F1A"));
    }

    // The count comes from the file. No real series has this many, and reserving room for whatever
    // a damaged one says is how a count becomes an allocation of many gigabytes.
    void aListLongerThanAnyRealSeriesIsRefused() {
        constexpr int kMostDates = 100'000;
        RecurrenceSpec spec = weeklyMondays();
        spec.deleted = QVector<QDate>(kMostDates, QDate(2026, 11, 9));
        const auto at_the_limit = read(spec);
        QCOMPARE(at_the_limit.state, sak::RecurrenceState::Readable);
        QCOMPARE(at_the_limit.deleted.size(), kMostDates);

        spec.deleted.append(QDate(2026, 11, 16));
        const auto past_it = read(spec);
        QCOMPARE(past_it.state, sak::RecurrenceState::Unreadable);
        QCOMPARE(past_it.problem, QStringLiteral("its list of changed occurrences is damaged"));
    }

    void whatCannotBeReadIsSaidAndNeverGuessed() {
        using Damage = void (*)(RecurrenceSpec&);
        const QVector<QPair<const char*, Damage>> damages = {
            {"reader version", [](RecurrenceSpec& spec) { spec.reader_version = 0x3005; }},
            {"appointment reader version",
             [](RecurrenceSpec& spec) { spec.reader_version2 = 0x3007; }},
            {"hijri pattern",
             [](RecurrenceSpec& spec) {
                 spec.frequency = kRecurMonthly;
                 spec.pattern_type = kPatternTypeHijriMonth;
                 spec.day_or_mask = 2;
             }},
            {"monthly in another calendar",
             [](RecurrenceSpec& spec) {
                 spec.frequency = kRecurMonthly;
                 spec.pattern_type = kPatternTypeMonth;
                 spec.calendar_type = kCalendarHijri;
                 spec.day_or_mask = 2;
             }},
            {"sliding", [](RecurrenceSpec& spec) { spec.sliding = 1; }},
            {"unknown frequency", [](RecurrenceSpec& spec) { spec.frequency = 0x2099; }},
            {"unknown end type", [](RecurrenceSpec& spec) { spec.end_type = 0x2099; }},
            {"empty count", [](RecurrenceSpec& spec) { spec.end_type = kRecurEndAfterCount; }},
            {"changed list disagrees",
             [](RecurrenceSpec& spec) {
                 RecurChangedOccurrence moved;
                 moved.original_start = utcAt(2026, 11, 23, 9);
                 moved.start = utcAt(2026, 11, 24, 14);
                 moved.end = utcAt(2026, 11, 24, 15);
                 spec.changed = {moved};
                 spec.drop_modified_dates = true;
             }},
            {"nth out of range",
             [](RecurrenceSpec& spec) {
                 spec.frequency = kRecurMonthly;
                 spec.pattern_type = kPatternTypeMonthNth;
                 spec.day_or_mask = kMaskMonday;
                 spec.nth = 6;
             }},
            {"yearly in part of a year",
             [](RecurrenceSpec& spec) {
                 spec.frequency = kRecurYearly;
                 spec.pattern_type = kPatternTypeMonth;
                 spec.period = 18;
                 spec.day_or_mask = 2;
             }},
        };
        for (const auto& [name, damage] : damages) {
            RecurrenceSpec spec = weeklyMondays();
            damage(spec);
            const auto result = read(spec);
            QVERIFY2(result.state == sak::RecurrenceState::Unreadable, name);
            QVERIFY2(result.rrule.isEmpty(), name);
            QVERIFY2(!result.problem.isEmpty(), name);
            QVERIFY2(result.description.startsWith(
                         QStringLiteral("Repeats, but the pattern could not be read")),
                     name);
        }
    }

    // Every length of a damaged blob, read to its end: nothing past the buffer, nothing half-read.
    void aTruncatedPatternIsNeverReadAsAWholeOne() {
        RecurrenceSpec spec = weeklyMondays();
        spec.deleted = {QDate(2026, 11, 9)};
        RecurChangedOccurrence moved;
        moved.original_start = utcAt(2026, 11, 23, 9);
        moved.start = utcAt(2026, 11, 24, 14);
        moved.end = utcAt(2026, 11, 24, 15);
        moved.subject = QStringLiteral("Moved");
        spec.changed = {moved};
        const QByteArray whole = buildRecurrenceBlob(spec);
        int readable = 0;
        // Everything through the changed list itself must be there; only the Unicode tail may go.
        const int essential = whole.size() - 50;
        for (int length = 0; length < essential; ++length) {
            const auto result = parseAppointmentRecurrence(whole.left(length), {}, false);
            readable += result.state == sak::RecurrenceState::Readable ? 1 : 0;
        }
        QCOMPARE(readable, 0);
        QCOMPARE(parseAppointmentRecurrence(whole, {}, false).state,
                 sak::RecurrenceState::Readable);
    }

    void aHugeDeletedCountIsRefusedBeforeItIsAllocated() {
        RecurrenceSpec spec = weeklyMondays();
        QByteArray blob = buildRecurrenceBlob(spec);
        // The deleted count sits after the 22-byte head, the mask, and three end fields.
        const int count_at = 22 + 4 + 12;
        qToLittleEndian<uint32_t>(0xFF'FF'FF'FF, blob.data() + count_at);
        QCOMPARE(parseAppointmentRecurrence(blob, {}, false).state,
                 sak::RecurrenceState::Unreadable);
    }

    void aTimeZoneStructReadsAndConverts() {
        const sak::PstTimeZone zone =
            parseTimeZoneStruct(pacificZoneStruct(), QStringLiteral("(UTC-08:00) Pacific; Time"));
        QVERIFY(zone.known);
        QCOMPARE(zone.standard_offset_minutes, -480);
        QCOMPARE(zone.daylight_offset_minutes, -420);
        QCOMPARE(zone.to_daylight.month, 3);
        QCOMPARE(zone.to_standard.month, 11);
        // What a TZID cannot hold is gone, so it needs no quoting anywhere it is written.
        QVERIFY2(!zone.name.contains(QLatin1Char(':')) && !zone.name.contains(QLatin1Char(';')),
                 qPrintable(zone.name));

        // Summer and winter, on both sides of the change.
        QCOMPARE(zoneWallClockToUtc(zone, utcAt(2026, 7, 1, 9)), utcAt(2026, 7, 1, 16));
        QCOMPARE(zoneWallClockToUtc(zone, utcAt(2026, 12, 1, 9)), utcAt(2026, 12, 1, 17));
        QCOMPARE(zoneWallClockToUtc(zone, utcAt(2026, 3, 8, 1)), utcAt(2026, 3, 8, 9));
        QCOMPARE(zoneWallClockToUtc(zone, utcAt(2026, 3, 8, 3)), utcAt(2026, 3, 8, 10));
        QCOMPARE(zoneUtcToWallClock(zone, utcAt(2026, 7, 1, 16)), utcAt(2026, 7, 1, 9));
        QCOMPARE(zoneUtcToWallClock(zone, utcAt(2026, 12, 1, 17)), utcAt(2026, 12, 1, 9));
    }

    void aSouthernZonesDaylightRunsAcrossTheNewYear() {
        // Sydney: back on the first Sunday of April, forward on the first Sunday of October.
        const sak::PstTimeZone zone = parseTimeZoneStruct(
            buildTimeZoneStruct(-600, -60, {4, 0, 1, 3}, {10, 0, 1, 2}), QStringLiteral("Sydney"));
        QVERIFY(zone.known);
        QCOMPARE(zoneWallClockToUtc(zone, utcAt(2026, 1, 15, 12)), utcAt(2026, 1, 15, 1));
        QCOMPARE(zoneWallClockToUtc(zone, utcAt(2026, 7, 15, 12)), utcAt(2026, 7, 15, 2));
        QCOMPARE(zoneWallClockToUtc(zone, utcAt(2026, 12, 15, 12)), utcAt(2026, 12, 15, 1));
    }

    void aZoneWithNoDaylightStaysPut() {
        const sak::PstTimeZone zone = parseTimeZoneStruct(buildTimeZoneStruct(-540, 0, {}, {}),
                                                          QString());
        QVERIFY(zone.known);
        QCOMPARE(zone.name, QStringLiteral("UTC+0900"));
        QCOMPARE(zoneWallClockToUtc(zone, utcAt(2026, 7, 1, 9)), utcAt(2026, 7, 1, 0));
        QCOMPARE(zoneWallClockToUtc(zone, utcAt(2026, 1, 1, 9)), utcAt(2026, 1, 1, 0));
    }

    void aTimeZoneDefinitionReads() {
        const sak::PstTimeZone zone = parseTimeZoneDefinition(buildTimeZoneDefinition(
            QStringLiteral("Pacific Standard Time"), 480, -60, {11, 0, 1, 2}, {3, 0, 2, 2}));
        QVERIFY(zone.known);
        QCOMPARE(zone.name, QStringLiteral("Pacific Standard Time"));
        QCOMPARE(zone.standard_offset_minutes, -480);
        QCOMPARE(zone.daylight_offset_minutes, -420);
        QCOMPARE(zone.to_standard.week, 1);
        QCOMPARE(zone.to_daylight.week, 2);
    }

    void allDayDateRoundsToTheNearestMidnight() {
        const sak::PstTimeZone no_zone;
        QCOMPARE(sak::pst::allDayDate(no_zone, utcAt(2026, 3, 14, 18, 30)), QDate(2026, 3, 15));
        QCOMPARE(sak::pst::allDayDate(no_zone, utcAt(2026, 3, 15, 0)), QDate(2026, 3, 15));
        QCOMPARE(sak::pst::allDayDate(no_zone, utcAt(2026, 3, 15, 11, 59)), QDate(2026, 3, 15));
        QCOMPARE(sak::pst::allDayDate(no_zone, utcAt(2026, 3, 15, 12)), QDate(2026, 3, 16));
        // The same instant, held as local time, is the same date.
        QCOMPARE(sak::pst::allDayDate(no_zone, utcAt(2026, 3, 14, 18, 30).toLocalTime()),
                 QDate(2026, 3, 15));
    }

    // A zone settles what no-zone rounding cannot: 11:00 UTC is the day before in New Zealand's
    // summer, 13 hours ahead, and the same day in American Samoa.
    void allDayDateUsesTheEventsOwnZone() {
        sak::PstTimeZone new_zealand_summer;
        new_zealand_summer.known = true;
        new_zealand_summer.standard_offset_minutes = 13 * 60;
        new_zealand_summer.daylight_offset_minutes = 13 * 60;
        QCOMPARE(sak::pst::allDayDate(new_zealand_summer, utcAt(2026, 1, 14, 11)),
                 QDate(2026, 1, 15));
        QCOMPARE(sak::pst::allDayDate({}, utcAt(2026, 1, 14, 11)), QDate(2026, 1, 14));
    }

    // Windows' own zone database, read from the registry, and Windows' own conversion. Every zone
    // on this PC, every hour of a year, compared with what a stored zone of the same bytes gives.
    // Reads the registry and changes nothing.
    void everyWindowsZoneConvertsAsWindowsDoes() {
        HKEY zones = nullptr;
        const wchar_t* const path = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones";
        QCOMPARE(RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &zones), ERROR_SUCCESS);
        int compared = 0;
        std::array<wchar_t, 256> name{};
        for (DWORD index = 0;; ++index) {
            DWORD length = static_cast<DWORD>(name.size());
            if (RegEnumKeyExW(
                    zones, index, name.data(), &length, nullptr, nullptr, nullptr, nullptr) !=
                ERROR_SUCCESS) {
                break;
            }
            RegTziFormat tzi{};
            DWORD size = sizeof(tzi);
            if (RegGetValueW(zones, name.data(), L"TZI", RRF_RT_REG_BINARY, nullptr, &tzi, &size) !=
                ERROR_SUCCESS) {
                continue;
            }
            const QString zone_name = QString::fromWCharArray(name.data());
            const sak::PstTimeZone zone = sak::pst::parseTimeZoneStruct(zoneStructFrom(tzi),
                                                                        zone_name);
            QVERIFY2(zone.known, qPrintable(zone_name));
            const QString wrong = firstDisagreementWithWindows(zone, tzi);
            QVERIFY2(wrong.isEmpty(), qPrintable(zone_name + QStringLiteral(": ") + wrong));
            ++compared;
        }
        RegCloseKey(zones);
        QVERIFY2(compared > 50, qPrintable(QString::number(compared)));
    }

    void aDamagedZoneIsNotKnown() {
        QVERIFY(!parseTimeZoneStruct(QByteArray(47, '\0'), QString()).known);
        QVERIFY(!parseTimeZoneStruct({}, QString()).known);
        QVERIFY(!parseTimeZoneDefinition({}).known);
        QVERIFY(!parseTimeZoneDefinition(QByteArray(3, char{2})).known);
        const QByteArray whole = buildTimeZoneDefinition(QStringLiteral("Zone"), 0, 0, {}, {});
        QVERIFY(parseTimeZoneDefinition(whole).known);
        for (int length = 0; length < whole.size(); ++length) {
            QVERIFY(!parseTimeZoneDefinition(whole.left(length)).known);
        }
    }
};

QTEST_GUILESS_MAIN(PstRecurrenceTests)
#include "test_pst_recurrence.moc"

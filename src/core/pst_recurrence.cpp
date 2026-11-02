// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file pst_recurrence.cpp
/// @brief AppointmentRecur (MS-OXOCAL 2.2.1.44), TimeZoneStruct and TZDEFINITION readers.

#include "sak/pst_recurrence.h"

#include <QHash>
#include <QLocale>
#include <QtEndian>
#include <QTimeZone>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>

namespace sak::pst {

namespace {

constexpr uint16_t kPatternVersion = 0x3004;
constexpr uint32_t kAppointmentReaderVersion = 0x3006;
constexpr uint16_t kFreqDaily = 0x200A;
constexpr uint16_t kFreqWeekly = 0x200B;
constexpr uint16_t kFreqMonthly = 0x200C;
constexpr uint16_t kFreqYearly = 0x200D;
constexpr uint16_t kPatternDay = 0;
constexpr uint16_t kPatternWeek = 1;
constexpr uint16_t kPatternMonth = 2;
constexpr uint16_t kPatternMonthNth = 3;
constexpr uint16_t kPatternMonthEnd = 4;
constexpr uint32_t kEndByDate = 0x2021;
constexpr uint32_t kEndAfterCount = 0x2022;
constexpr uint32_t kEndNever = 0x2023;
constexpr uint32_t kEndNeverAlternate = 0xFF'FF'FF'FF;
constexpr uint32_t kNeverEndingDate = 0x5A'E9'80'DF;  // 31 December 4500, Outlook's "no end"
constexpr uint32_t kMaxListedDates = 100'000;
constexpr int kNthIsLast = 5;
constexpr int kMinutesPerDay = 1440;
constexpr int kMinutesPerHour = 60;
constexpr int kSecondsPerMinute = 60;
constexpr int kTwoDigits = 2;
constexpr int kDecimal = 10;
constexpr qsizetype kExceptionTimesBytes = 12;  // Start, end and original start
constexpr int kMonthsPerYear = 12;
constexpr int kDaysPerWeek = 7;
constexpr int kFirstAmbiguousMonthDay = 29;
constexpr int kLastMonthDay = 31;

// Gregorian calendars MS-OXOCAL names: default, Gregorian, US English, and the localized variants.
constexpr std::array<uint16_t, 7> kGregorianCalendarTypes = {0x0, 0x1, 0x2, 0x9, 0xA, 0xB, 0xC};

constexpr uint16_t kOverrideSubject = 0x0001;
constexpr uint16_t kOverrideMeetingType = 0x0002;
constexpr uint16_t kOverrideReminderDelta = 0x0004;
constexpr uint16_t kOverrideReminder = 0x0008;
constexpr uint16_t kOverrideLocation = 0x0010;
constexpr uint16_t kOverrideBusyStatus = 0x0020;
constexpr uint16_t kOverrideAttachment = 0x0040;
constexpr uint16_t kOverrideSubType = 0x0080;
constexpr uint16_t kOverrideColor = 0x0100;
constexpr uint16_t kOverrideBody = 0x0200;
constexpr uint32_t kWriterVersionWithChangeHighlight = 0x3009;

constexpr int kTzStructBytes = 48;
constexpr int kTzStructStandardBiasOffset = 4;
constexpr int kTzStructDaylightBiasOffset = 8;
constexpr int kTzStructStandardDateOffset = 14;
constexpr int kTzStructDaylightDateOffset = 32;

constexpr int kSystemTimeMonthOffset = 2;
constexpr int kSystemTimeDayOfWeekOffset = 4;
constexpr int kSystemTimeDayOffset = 6;
constexpr int kSystemTimeHourOffset = 8;
constexpr int kSystemTimeMinuteOffset = 10;

constexpr int kTzDefHeaderSizeOffset = 2;
constexpr int kTzDefKeyLengthOffset = 6;
constexpr int kTzDefKeyNameOffset = 8;
constexpr int kTzDefMajorVersion = 2;
constexpr int kTzRuleBytes = 66;
constexpr int kTzRuleFlagsOffset = 4;
constexpr int kTzRuleBiasOffset = 22;
constexpr int kTzRuleStandardBiasOffset = 26;
constexpr int kTzRuleDaylightBiasOffset = 30;
constexpr int kTzRuleStandardDateOffset = 34;
constexpr int kTzRuleDaylightDateOffset = 50;
constexpr uint16_t kTzRuleEffective = 0x0002;

/// Bounds-checked little-endian reader. Once a read runs past the end every later read yields zero
/// and failed() stays true, so a caller checks once after a group of reads.
class BlobReader {
public:
    explicit BlobReader(const QByteArray& data) : m_data(data) {}

    template <typename T>
    [[nodiscard]] T read() {
        if (m_failed || m_pos + static_cast<qsizetype>(sizeof(T)) > m_data.size()) {
            m_failed = true;
            return T{0};
        }
        const T value = qFromLittleEndian<T>(m_data.constData() + m_pos);
        m_pos += static_cast<qsizetype>(sizeof(T));
        return value;
    }

    [[nodiscard]] QByteArray bytes(qsizetype count) {
        if (m_failed || count < 0 || m_pos + count > m_data.size()) {
            m_failed = true;
            return {};
        }
        QByteArray out = m_data.mid(m_pos, count);
        m_pos += count;
        return out;
    }

    void skip(qsizetype count) { (void)bytes(count); }

    [[nodiscard]] bool failed() const { return m_failed; }
    [[nodiscard]] qsizetype position() const { return m_pos; }
    void seek(qsizetype pos) {
        m_pos = pos;
        m_failed = false;
    }

private:
    const QByteArray& m_data;
    qsizetype m_pos = 0;
    bool m_failed = false;
};

/// Minutes since 1 January 1601 as a wall-clock time, held in a UTC QDateTime.
QDateTime fromOutlookMinutes(uint32_t minutes) {
    static const QDateTime kEpoch(QDate(1601, 1, 1), QTime(0, 0), QTimeZone::UTC);
    return kEpoch.addSecs(static_cast<qint64>(minutes) * kSecondsPerMinute);
}

template <typename T>
T fieldAt(const QByteArray& raw, int offset) {
    if (offset < 0 || offset + static_cast<qsizetype>(sizeof(T)) > raw.size()) {
        return T{0};
    }
    return qFromLittleEndian<T>(raw.constData() + offset);
}

PstTimeZoneRule ruleFromSystemTime(const QByteArray& raw, int offset) {
    PstTimeZoneRule rule;
    rule.month = fieldAt<uint16_t>(raw, offset + kSystemTimeMonthOffset);
    rule.day_of_week = fieldAt<uint16_t>(raw, offset + kSystemTimeDayOfWeekOffset);
    rule.week = fieldAt<uint16_t>(raw, offset + kSystemTimeDayOffset);
    rule.hour = fieldAt<uint16_t>(raw, offset + kSystemTimeHourOffset);
    rule.minute = fieldAt<uint16_t>(raw, offset + kSystemTimeMinuteOffset);
    return rule;
}

QString safeZoneName(const QString& raw) {
    // A TZID sits in both a property value and a parameter: keep only what needs no escaping there.
    QString name;
    name.reserve(raw.size());
    for (const QChar ch : raw) {
        const bool unsafe = ch.unicode() < 0x20 || ch == QLatin1Char('"') ||
                            ch == QLatin1Char(',') || ch == QLatin1Char(';') ||
                            ch == QLatin1Char(':') || ch == QLatin1Char('\\');
        name.append(unsafe ? QLatin1Char(' ') : ch);
    }
    return name.simplified();
}

QString zoneNameFromOffset(int offset_minutes) {
    const int magnitude = std::abs(offset_minutes);
    return QStringLiteral("UTC%1%2%3")
        .arg(offset_minutes < 0 ? QLatin1Char('-') : QLatin1Char('+'))
        .arg(magnitude / kMinutesPerHour, kTwoDigits, kDecimal, QLatin1Char('0'))
        .arg(magnitude % kMinutesPerHour, kTwoDigits, kDecimal, QLatin1Char('0'));
}

/// The raw fields both zone formats share.
struct ZoneFields {
    int bias = 0;
    int standard_bias = 0;
    int daylight_bias = 0;
    PstTimeZoneRule to_standard;
    PstTimeZoneRule to_daylight;
};

PstTimeZone finishZone(const ZoneFields& fields, const QString& name) {
    PstTimeZone zone;
    zone.known = true;
    zone.standard_offset_minutes = -(fields.bias + fields.standard_bias);
    zone.daylight_offset_minutes = -(fields.bias + fields.daylight_bias);
    zone.to_standard = fields.to_standard;
    zone.to_daylight = fields.to_daylight;
    const QString safe = safeZoneName(name);
    zone.name = safe.isEmpty() ? zoneNameFromOffset(zone.standard_offset_minutes) : safe;
    return zone;
}

/// The wall-clock moment a yearly rule fires in @p year: its Nth (5 = last) weekday of its month.
QDateTime transitionIn(int year, const PstTimeZoneRule& rule) {
    const QDate first(year, rule.month, 1);
    if (!first.isValid()) {
        return {};
    }
    // Qt counts Monday as 1 and Sunday as 7; SYSTEMTIME counts Sunday as 0.
    const int first_weekday = first.dayOfWeek() % kDaysPerWeek;
    const int to_first_match = (rule.day_of_week - first_weekday + kDaysPerWeek) % kDaysPerWeek;
    QDate date =
        first.addDays(to_first_match + (static_cast<qint64>(rule.week - 1) * kDaysPerWeek));
    while (date.month() != rule.month) {
        date = date.addDays(-kDaysPerWeek);
    }
    return QDateTime(date, QTime(rule.hour, rule.minute), QTimeZone::UTC);
}

/// Whether @p wall falls in daylight time. Southern-hemisphere zones change back early in the year,
/// so the span runs across the new year.
bool isDaylightAt(const PstTimeZone& zone, const QDateTime& wall) {
    if (zone.to_daylight.month == 0 || zone.to_standard.month == 0) {
        return false;
    }
    const int year = wall.date().year();
    const QDateTime begins = transitionIn(year, zone.to_daylight);
    const QDateTime ends = transitionIn(year, zone.to_standard);
    if (!begins.isValid() || !ends.isValid()) {
        return false;
    }
    if (begins < ends) {
        return wall >= begins && wall < ends;
    }
    return wall >= begins || wall < ends;
}

QString weekdayToken(int bit) {
    static const std::array<const char*, kDaysPerWeek> kTokens = {
        "SU", "MO", "TU", "WE", "TH", "FR", "SA"};
    return QString::fromLatin1(kTokens[static_cast<size_t>(bit)]);
}

QString weekdayName(int bit) {
    static const std::array<const char*, kDaysPerWeek> kNames = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    return QString::fromLatin1(kNames[static_cast<size_t>(bit)]);
}

QStringList weekdayTokens(uint32_t mask) {
    QStringList tokens;
    for (int bit = 0; bit < kDaysPerWeek; ++bit) {
        if ((mask & (1U << bit)) != 0) {
            tokens.append(weekdayToken(bit));
        }
    }
    return tokens;
}

QString joinWords(const QStringList& words) {
    if (words.size() <= 1) {
        return words.join(QString());
    }
    return QStringList(words.mid(0, words.size() - 1)).join(QStringLiteral(", ")) +
           QStringLiteral(" and ") + words.last();
}

QString weekdayWords(uint32_t mask) {
    constexpr uint32_t kAllDays = 0x7F;
    constexpr uint32_t kWeekdays = 0x3E;
    constexpr uint32_t kWeekend = 0x41;
    if (mask == kAllDays) {
        return QStringLiteral("day");
    }
    if (mask == kWeekdays) {
        return QStringLiteral("weekday");
    }
    if (mask == kWeekend) {
        return QStringLiteral("weekend day");
    }
    QStringList names;
    for (int bit = 0; bit < kDaysPerWeek; ++bit) {
        if ((mask & (1U << bit)) != 0) {
            names.append(weekdayName(bit));
        }
    }
    return joinWords(names);
}

QString nthWord(int nth) {
    static const std::array<const char*, 5> kWords = {"first", "second", "third", "fourth", "last"};
    return QString::fromLatin1(kWords[static_cast<size_t>(std::clamp(nth, 1, kNthIsLast) - 1)]);
}

QString everyWords(int interval, const QString& singular) {
    return interval == 1 ? QStringLiteral("Every %1").arg(singular)
                         : QStringLiteral("Every %1 %2s").arg(interval).arg(singular);
}

/// What the fixed part of a pattern said.
struct PatternFields {
    uint16_t frequency = 0;
    uint16_t pattern_type = 0;
    uint16_t calendar_type = 0;
    uint32_t period = 0;
    uint32_t sliding = 0;
    uint32_t day_or_mask = 0;  ///< Day of month, or weekday mask
    uint32_t nth = 0;
    uint32_t end_type = 0;
    uint32_t occurrence_count = 0;
    uint32_t first_day_of_week = 0;
    QVector<uint32_t> deleted_dates;
    QVector<uint32_t> modified_dates;
    uint32_t start_date = 0;
    uint32_t end_date = 0;
    uint32_t start_offset = 0;
    uint32_t end_offset = 0;
    uint32_t writer_version2 = 0;
    QVector<PstRecurrenceException> exceptions;
    QVector<uint32_t> exception_original_minutes;
};

std::optional<QVector<uint32_t>> readDateList(BlobReader& reader) {
    const uint32_t count = reader.read<uint32_t>();
    if (reader.failed() || count > kMaxListedDates) {
        return std::nullopt;
    }
    QVector<uint32_t> dates;
    dates.reserve(static_cast<qsizetype>(count));
    for (uint32_t i = 0; i < count; ++i) {
        dates.append(reader.read<uint32_t>());
    }
    if (reader.failed()) {
        return std::nullopt;
    }
    return dates;
}

bool readPatternTypeFields(BlobReader& reader, PatternFields& fields) {
    switch (fields.pattern_type) {
    case kPatternDay:
        return true;
    case kPatternWeek:
    case kPatternMonth:
    case kPatternMonthEnd:
        fields.day_or_mask = reader.read<uint32_t>();
        return true;
    case kPatternMonthNth:
        fields.day_or_mask = reader.read<uint32_t>();
        fields.nth = reader.read<uint32_t>();
        return true;
    default:
        return false;  // The Hijri patterns, or one this reader has never seen
    }
}

/// Read one ExceptionInfo: the wall-clock times, and whichever fields it overrides.
bool readExceptionInfo(BlobReader& reader, PstRecurrenceException& exception, uint32_t& original) {
    const uint32_t start = reader.read<uint32_t>();
    const uint32_t end = reader.read<uint32_t>();
    original = reader.read<uint32_t>();
    const uint16_t overrides = reader.read<uint16_t>();
    exception.start = fromOutlookMinutes(start);
    exception.end = fromOutlookMinutes(end);
    exception.original_start = fromOutlookMinutes(original);
    exception.body_changed = (overrides & kOverrideBody) != 0;
    if ((overrides & kOverrideSubject) != 0) {
        (void)reader.read<uint16_t>();
        const uint16_t length = reader.read<uint16_t>();
        exception.subject = QString::fromLocal8Bit(reader.bytes(length));
        exception.has_subject = true;
    }
    const auto skip_dword_if = [&reader, overrides](uint16_t flag) {
        if ((overrides & flag) != 0) {
            reader.skip(sizeof(uint32_t));
        }
    };
    skip_dword_if(kOverrideMeetingType);
    skip_dword_if(kOverrideReminderDelta);
    skip_dword_if(kOverrideReminder);
    if ((overrides & kOverrideLocation) != 0) {
        (void)reader.read<uint16_t>();
        const uint16_t length = reader.read<uint16_t>();
        exception.location = QString::fromLocal8Bit(reader.bytes(length));
        exception.has_location = true;
    }
    skip_dword_if(kOverrideBusyStatus);
    skip_dword_if(kOverrideAttachment);
    skip_dword_if(kOverrideSubType);
    skip_dword_if(kOverrideColor);
    return !reader.failed();
}

QString readWideText(BlobReader& reader) {
    const uint16_t chars = reader.read<uint16_t>();
    const QByteArray raw = reader.bytes(static_cast<qsizetype>(chars) * 2);
    if (reader.failed()) {
        return {};
    }
    return QString::fromUtf16(reinterpret_cast<const char16_t*>(raw.constData()), chars);
}

/// The Unicode copies of the overridden subject and location, which follow the ANSI list. When
/// this tail does not parse, the ANSI values stand: the pattern itself is already read.
void readExtendedExceptions(BlobReader& reader, PatternFields& fields) {
    for (PstRecurrenceException& exception : fields.exceptions) {
        if (fields.writer_version2 >= kWriterVersionWithChangeHighlight) {
            const uint32_t highlight_size = reader.read<uint32_t>();
            reader.skip(highlight_size);
        }
        const uint32_t reserved = reader.read<uint32_t>();
        reader.skip(reserved);
        if (exception.has_subject || exception.has_location) {
            reader.skip(kExceptionTimesBytes);
            if (exception.has_subject) {
                const QString wide = readWideText(reader);
                if (!reader.failed()) {
                    exception.subject = wide;
                }
            }
            if (exception.has_location) {
                const QString wide = readWideText(reader);
                if (!reader.failed()) {
                    exception.location = wide;
                }
            }
            const uint32_t tail = reader.read<uint32_t>();
            reader.skip(tail);
        }
        if (reader.failed()) {
            return;
        }
    }
}

/// Everything up to the occurrence lists: what kind of rule it is, and how it ends.
QString readPatternHead(BlobReader& reader, PatternFields& fields) {
    if (reader.read<uint16_t>() != kPatternVersion || reader.read<uint16_t>() != kPatternVersion) {
        return QStringLiteral("it uses a recurrence format this version does not read");
    }
    fields.frequency = reader.read<uint16_t>();
    fields.pattern_type = reader.read<uint16_t>();
    fields.calendar_type = reader.read<uint16_t>();
    (void)reader.read<uint32_t>();  // FirstDateTime: the start date below says the same thing
    fields.period = reader.read<uint32_t>();
    fields.sliding = reader.read<uint32_t>();
    if (reader.failed() || !readPatternTypeFields(reader, fields)) {
        return QStringLiteral("its pattern type is not one this version reads");
    }
    fields.end_type = reader.read<uint32_t>();
    fields.occurrence_count = reader.read<uint32_t>();
    fields.first_day_of_week = reader.read<uint32_t>();
    return {};
}

/// The appointment's times, then its changed occurrences, which must match the list before them.
QString readAppointmentPart(BlobReader& reader, PatternFields& fields) {
    if (reader.read<uint32_t>() != kAppointmentReaderVersion) {
        return QStringLiteral("it uses an appointment format this version does not read");
    }
    fields.writer_version2 = reader.read<uint32_t>();
    fields.start_offset = reader.read<uint32_t>();
    fields.end_offset = reader.read<uint32_t>();
    const uint16_t exception_count = reader.read<uint16_t>();
    if (reader.failed()) {
        return QStringLiteral("the data ends too early");
    }
    for (int i = 0; i < exception_count; ++i) {
        PstRecurrenceException exception;
        uint32_t original = 0;
        if (!readExceptionInfo(reader, exception, original)) {
            return QStringLiteral("one of its changed occurrences is damaged");
        }
        fields.exceptions.append(exception);
        fields.exception_original_minutes.append(original);
    }
    if (fields.exceptions.size() != fields.modified_dates.size()) {
        return QStringLiteral("its changed occurrences do not match its own list of them");
    }
    return {};
}

/// Parse the blob into fields, or say what stopped it.
QString readPattern(const QByteArray& blob, PatternFields& fields) {
    BlobReader reader(blob);
    QString problem = readPatternHead(reader, fields);
    if (!problem.isEmpty()) {
        return problem;
    }
    const auto deleted = readDateList(reader);
    const auto modified = readDateList(reader);
    if (!deleted || !modified) {
        return QStringLiteral("its list of changed occurrences is damaged");
    }
    fields.deleted_dates = *deleted;
    fields.modified_dates = *modified;
    fields.start_date = reader.read<uint32_t>();
    fields.end_date = reader.read<uint32_t>();
    problem = readAppointmentPart(reader, fields);
    if (!problem.isEmpty()) {
        return problem;
    }
    const uint32_t reserved = reader.read<uint32_t>();
    reader.skip(reserved);
    if (!reader.failed()) {
        readExtendedExceptions(reader, fields);
    }
    return {};
}

bool isGregorian(uint16_t calendar_type) {
    return std::ranges::find(kGregorianCalendarTypes, calendar_type) !=
           kGregorianCalendarTypes.end();
}

/// BYMONTHDAY for a day of the month. Outlook moves a day a short month does not have to that
/// month's last day; RFC 5545 would skip the month, so those days need a set of candidates.
QString monthDayRule(int day) {
    if (day >= kLastMonthDay) {
        return QStringLiteral("BYMONTHDAY=-1");
    }
    if (day >= kFirstAmbiguousMonthDay) {
        QStringList days;
        for (int candidate = kFirstAmbiguousMonthDay - 1; candidate <= day; ++candidate) {
            days.append(QString::number(candidate));
        }
        return QStringLiteral("BYMONTHDAY=%1;BYSETPOS=-1").arg(days.join(QLatin1Char(',')));
    }
    return QStringLiteral("BYMONTHDAY=%1").arg(day);
}

QString nthDayRule(const PatternFields& fields) {
    const int position = fields.nth >= kNthIsLast ? -1 : static_cast<int>(fields.nth);
    return QStringLiteral("BYDAY=%1;BYSETPOS=%2")
        .arg(weekdayTokens(fields.day_or_mask).join(QLatin1Char(',')))
        .arg(position);
}

struct RuleWords {
    QString rule;
    QString words;
    QString problem;
};

RuleWords weeklyRule(const PatternFields& fields, const QDate& start) {
    if (fields.period == 0 || fields.day_or_mask == 0 || fields.first_day_of_week >= kDaysPerWeek) {
        return {.rule = {}, .words = {}, .problem = QStringLiteral("its weekly pattern is empty")};
    }
    if ((fields.day_or_mask & (1U << (start.dayOfWeek() % kDaysPerWeek))) == 0) {
        return {.rule = {},
                .words = {},
                .problem = QStringLiteral("its first date is not on one of its own weekdays")};
    }
    const QString rule = QStringLiteral("FREQ=WEEKLY;INTERVAL=%1;BYDAY=%2;WKST=%3")
                             .arg(fields.period)
                             .arg(weekdayTokens(fields.day_or_mask).join(QLatin1Char(',')))
                             .arg(weekdayToken(static_cast<int>(fields.first_day_of_week)));
    return {.rule = rule,
            .words = everyWords(static_cast<int>(fields.period), QStringLiteral("week")) +
                     QStringLiteral(" on ") + weekdayWords(fields.day_or_mask),
            .problem = {}};
}

RuleWords dailyRule(const PatternFields& fields, const QDate& start) {
    if (fields.pattern_type == kPatternWeek) {
        // "Every weekday" is stored as a daily frequency over a weekly pattern.
        RuleWords weekly = weeklyRule(fields, start);
        if (weekly.problem.isEmpty()) {
            weekly.words = QStringLiteral("Every ") + weekdayWords(fields.day_or_mask);
        }
        return weekly;
    }
    if (fields.period == 0 || fields.period % kMinutesPerDay != 0) {
        return {.rule = {},
                .words = {},
                .problem = QStringLiteral("its daily interval is not a whole number of days")};
    }
    const int days = static_cast<int>(fields.period / kMinutesPerDay);
    return {.rule = QStringLiteral("FREQ=DAILY;INTERVAL=%1").arg(days),
            .words = everyWords(days, QStringLiteral("day")),
            .problem = {}};
}

RuleWords dayOutOfRange() {
    return {.rule = {},
            .words = {},
            .problem = QStringLiteral("its day of the month is out of range")};
}

/// The Nth weekday of a month.
/// The day an Nth-weekday rule lands on in the month of @p date, or an invalid date if it has none.
QDate nthDayOfMonth(const QDate& date, uint32_t mask, uint32_t nth) {
    QVector<QDate> matching;
    for (QDate day(date.year(), date.month(), 1); day.isValid() && day.month() == date.month();
         day = day.addDays(1)) {
        if ((mask & (1U << (day.dayOfWeek() % kDaysPerWeek))) != 0) {
            matching.append(day);
        }
    }
    if (matching.isEmpty()) {
        return {};
    }
    if (nth >= kNthIsLast) {
        return matching.last();
    }
    return matching.value(static_cast<qsizetype>(nth) - 1);
}

RuleWords nthDayPart(const PatternFields& fields, const QDate& start) {
    if (fields.day_or_mask == 0 || fields.nth < 1 || fields.nth > kNthIsLast) {
        return dayOutOfRange();
    }
    // MS-OXOCAL has the start date be the first occurrence, so it lands on the rule. A real yearly
    // series' did not, and its stored start, clip start and Outlook's wording all named another
    // month. Nothing in the store says which is right, so the series says so instead of picking.
    if (nthDayOfMonth(start, fields.day_or_mask, fields.nth) != start) {
        return {.rule = {},
                .words = {},
                .problem =
                    QStringLiteral("its first date is not on the weekday its pattern names")};
    }
    return {.rule = nthDayRule(fields),
            .words = QStringLiteral("the %1 %2")
                         .arg(nthWord(static_cast<int>(fields.nth)),
                              weekdayWords(fields.day_or_mask)),
            .problem = {}};
}

/// The day part shared by monthly and yearly rules, in rule and in words.
RuleWords monthDayPart(const PatternFields& fields, const QDate& start) {
    if (fields.pattern_type == kPatternMonthNth) {
        return nthDayPart(fields, start);
    }
    if (fields.pattern_type == kPatternMonthEnd) {
        return {.rule = QStringLiteral("BYMONTHDAY=-1"),
                .words = QStringLiteral("the last day"),
                .problem = {}};
    }
    if (fields.pattern_type != kPatternMonth || fields.day_or_mask < 1 ||
        fields.day_or_mask > kLastMonthDay) {
        return dayOutOfRange();
    }
    const int day = static_cast<int>(fields.day_or_mask);
    // The first date is the day itself, or the last day of a month too short to have it.
    if (start.day() != day && start.day() != start.daysInMonth()) {
        return {.rule = {},
                .words = {},
                .problem = QStringLiteral("its first date is not on its own day of the month")};
    }
    return {.rule = monthDayRule(day), .words = QStringLiteral("day %1").arg(day), .problem = {}};
}

RuleWords monthlyRule(const PatternFields& fields, const QDate& start) {
    RuleWords part = monthDayPart(fields, start);
    if (!part.problem.isEmpty()) {
        return part;
    }
    if (fields.period == 0) {
        return {.rule = {},
                .words = {},
                .problem = QStringLiteral("its monthly interval is empty")};
    }
    return {.rule = QStringLiteral("FREQ=MONTHLY;INTERVAL=%1;%2").arg(fields.period).arg(part.rule),
            .words = everyWords(static_cast<int>(fields.period), QStringLiteral("month")) +
                     QStringLiteral(" on ") + part.words,
            .problem = {}};
}

RuleWords yearlyRule(const PatternFields& fields, const QDate& start) {
    RuleWords part = monthDayPart(fields, start);
    if (!part.problem.isEmpty()) {
        return part;
    }
    if (fields.period == 0 || fields.period % kMonthsPerYear != 0) {
        return {.rule = {},
                .words = {},
                .problem = QStringLiteral("its yearly interval is not a whole number of years")};
    }
    const int years = static_cast<int>(fields.period / kMonthsPerYear);
    const QString month = QLocale::c().monthName(start.month());
    const QString on_words = fields.pattern_type == kPatternMonth
                                 ? QStringLiteral("%1 %2").arg(month).arg(fields.day_or_mask)
                                 : QStringLiteral("%1 of %2").arg(part.words, month);
    return {.rule = QStringLiteral("FREQ=YEARLY;INTERVAL=%1;BYMONTH=%2;%3")
                        .arg(years)
                        .arg(start.month())
                        .arg(part.rule),
            .words = everyWords(years, QStringLiteral("year")) + QStringLiteral(" on ") + on_words,
            .problem = {}};
}

RuleWords ruleForFrequency(const PatternFields& fields, const QDate& start) {
    switch (fields.frequency) {
    case kFreqDaily:
        return dailyRule(fields, start);
    case kFreqWeekly:
        return fields.pattern_type == kPatternWeek
                   ? weeklyRule(fields, start)
                   : RuleWords{.rule = {},
                               .words = {},
                               .problem = QStringLiteral("its weekly pattern is malformed")};
    case kFreqMonthly:
        return monthlyRule(fields, start);
    case kFreqYearly:
        return yearlyRule(fields, start);
    default:
        return {.rule = {},
                .words = {},
                .problem = QStringLiteral("its frequency is not one this version reads")};
    }
}

/// UNTIL takes the form of DTSTART: a date, a UTC time when there is a zone, else a floating time.
QString untilValue(const PatternFields& fields, const PstTimeZone& zone, bool all_day) {
    const QDateTime last = fromOutlookMinutes(fields.end_date + fields.start_offset);
    if (all_day) {
        return last.toString(QStringLiteral("yyyyMMdd"));
    }
    if (zone.known) {
        return zoneWallClockToUtc(zone, last).toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
    }
    return last.toString(QStringLiteral("yyyyMMdd'T'HHmmss"));
}

/// The end of the rule, appended to @p words. Empty problem means it was understood.
QString appendEnd(const PatternFields& fields,
                  const PstTimeZone& zone,
                  bool all_day,
                  QString& rule,
                  QString& words) {
    const bool no_end = fields.end_type == kEndNever || fields.end_type == kEndNeverAlternate ||
                        (fields.end_type == kEndByDate && fields.end_date == kNeverEndingDate);
    if (no_end) {
        words += QStringLiteral(", no end date");
        return {};
    }
    if (fields.end_type == kEndAfterCount) {
        if (fields.occurrence_count == 0) {
            return QStringLiteral("its occurrence count is empty");
        }
        rule += QStringLiteral(";COUNT=%1").arg(fields.occurrence_count);
        words += QStringLiteral(", %1 times").arg(fields.occurrence_count);
        return {};
    }
    if (fields.end_type == kEndByDate) {
        rule += QStringLiteral(";UNTIL=") + untilValue(fields, zone, all_day);
        words += QStringLiteral(", until %1")
                     .arg(QLocale::c().toString(fromOutlookMinutes(fields.end_date).date(),
                                                QStringLiteral("MMMM d, yyyy")));
        return {};
    }
    return QStringLiteral("its end type is not one this version reads");
}

/// Sort the changed list into plain deleted starts and exceptions, and put the counts into words.
void applyOccurrenceLists(const PatternFields& fields, PstRecurrence& recurrence) {
    // DeletedInstanceDates also holds the original date of every changed occurrence.
    QVector<uint32_t> changed_dates;
    changed_dates.reserve(fields.exception_original_minutes.size());
    for (const uint32_t original : fields.exception_original_minutes) {
        changed_dates.append(original - (original % kMinutesPerDay));
    }
    for (const uint32_t date : fields.deleted_dates) {
        if (!changed_dates.contains(date)) {
            recurrence.deleted.append(fromOutlookMinutes(date + fields.start_offset));
        }
    }
    recurrence.exceptions = fields.exceptions;
    if (!recurrence.deleted.isEmpty() || !recurrence.exceptions.isEmpty()) {
        recurrence.description += QStringLiteral(". %1 deleted, %2 changed")
                                      .arg(recurrence.deleted.size())
                                      .arg(recurrence.exceptions.size());
    }
}

/// Where the rule Outlook marks as in effect starts, else the first rule; -1 when the rules run
/// past the data or there are none.
int effectiveRuleOffset(const QByteArray& raw, qsizetype rules_at) {
    const int rule_count = fieldAt<uint16_t>(raw, static_cast<int>(rules_at) - 2);
    int chosen = -1;
    for (int i = 0; i < rule_count; ++i) {
        const qsizetype at = rules_at + (static_cast<qsizetype>(i) * kTzRuleBytes);
        if (at + kTzRuleBytes > raw.size()) {
            return -1;
        }
        const uint16_t flags = fieldAt<uint16_t>(raw, static_cast<int>(at) + kTzRuleFlagsOffset);
        if ((flags & kTzRuleEffective) != 0 || chosen < 0) {
            chosen = static_cast<int>(at);
        }
    }
    return chosen;
}

PstRecurrence unreadable(const QString& reason) {
    PstRecurrence recurrence;
    recurrence.state = RecurrenceState::Unreadable;
    recurrence.problem = reason;
    recurrence.description =
        QStringLiteral("Repeats, but the pattern could not be read: %1").arg(reason);
    return recurrence;
}

}  // namespace

PstTimeZone parseTimeZoneStruct(const QByteArray& raw, const QString& description) {
    if (raw.size() != kTzStructBytes) {
        return {};
    }
    return finishZone({.bias = fieldAt<int32_t>(raw, 0),
                       .standard_bias = fieldAt<int32_t>(raw, kTzStructStandardBiasOffset),
                       .daylight_bias = fieldAt<int32_t>(raw, kTzStructDaylightBiasOffset),
                       .to_standard = ruleFromSystemTime(raw, kTzStructStandardDateOffset),
                       .to_daylight = ruleFromSystemTime(raw, kTzStructDaylightDateOffset)},
                      description);
}

PstTimeZone parseTimeZoneDefinition(const QByteArray& raw) {
    if (raw.size() < kTzDefKeyNameOffset || static_cast<uint8_t>(raw[0]) != kTzDefMajorVersion) {
        return {};
    }
    const int header_size = fieldAt<uint16_t>(raw, kTzDefHeaderSizeOffset);
    const int key_chars = fieldAt<uint16_t>(raw, kTzDefKeyLengthOffset);
    const qsizetype key_end = kTzDefKeyNameOffset + (static_cast<qsizetype>(key_chars) * 2);
    // cbHeader counts from just past itself, and the rule count closes the header.
    const qsizetype rules_at = kTzDefHeaderSizeOffset + static_cast<qsizetype>(sizeof(uint16_t)) +
                               header_size;
    if (key_end > raw.size() || rules_at > raw.size() || rules_at < key_end) {
        return {};
    }
    const int chosen = effectiveRuleOffset(raw, rules_at);
    if (chosen < 0) {
        return {};
    }
    const QString key = QString::fromUtf16(
        reinterpret_cast<const char16_t*>(raw.constData() + kTzDefKeyNameOffset), key_chars);
    return finishZone({.bias = fieldAt<int32_t>(raw, chosen + kTzRuleBiasOffset),
                       .standard_bias = fieldAt<int32_t>(raw, chosen + kTzRuleStandardBiasOffset),
                       .daylight_bias = fieldAt<int32_t>(raw, chosen + kTzRuleDaylightBiasOffset),
                       .to_standard = ruleFromSystemTime(raw, chosen + kTzRuleStandardDateOffset),
                       .to_daylight = ruleFromSystemTime(raw, chosen + kTzRuleDaylightDateOffset)},
                      key);
}

QDateTime zoneWallClockToUtc(const PstTimeZone& zone, const QDateTime& wall) {
    if (!zone.known) {
        return wall;
    }
    const int offset = isDaylightAt(zone, wall) ? zone.daylight_offset_minutes
                                                : zone.standard_offset_minutes;
    return wall.addSecs(-static_cast<qint64>(offset) * kSecondsPerMinute);
}

QDateTime zoneUtcToWallClock(const PstTimeZone& zone, const QDateTime& utc) {
    if (!zone.known) {
        return utc;
    }
    // Read as standard time first: that lands in the right season except within the hour of a
    // change, which the daylight check below settles.
    const QDateTime as_standard =
        utc.toUTC().addSecs(static_cast<qint64>(zone.standard_offset_minutes) * kSecondsPerMinute);
    const QDateTime as_daylight =
        utc.toUTC().addSecs(static_cast<qint64>(zone.daylight_offset_minutes) * kSecondsPerMinute);
    return isDaylightAt(zone, as_daylight) ? as_daylight : as_standard;
}

PstRecurrence parseAppointmentRecurrence(const QByteArray& blob,
                                         const PstTimeZone& zone,
                                         bool all_day) {
    PatternFields fields;
    const QString read_problem = readPattern(blob, fields);
    if (!read_problem.isEmpty()) {
        return unreadable(read_problem);
    }
    if (fields.sliding != 0) {
        return unreadable(QStringLiteral("it repeats relative to completion, like a task"));
    }
    const bool calendar_matters = fields.frequency == kFreqMonthly ||
                                  fields.frequency == kFreqYearly;
    if (calendar_matters && !isGregorian(fields.calendar_type)) {
        return unreadable(QStringLiteral("it follows a calendar other than the Gregorian one"));
    }
    const QDateTime start = fromOutlookMinutes(fields.start_date + fields.start_offset);
    const RuleWords rule = ruleForFrequency(fields, start.date());
    if (!rule.problem.isEmpty()) {
        return unreadable(rule.problem);
    }

    PstRecurrence recurrence;
    recurrence.rrule = rule.rule;
    recurrence.description = rule.words;
    const QString end_problem =
        appendEnd(fields, zone, all_day, recurrence.rrule, recurrence.description);
    if (!end_problem.isEmpty()) {
        return unreadable(end_problem);
    }
    recurrence.state = RecurrenceState::Readable;
    recurrence.wall_start = start;
    recurrence.wall_end = fromOutlookMinutes(fields.start_date + fields.end_offset);
    applyOccurrenceLists(fields, recurrence);
    return recurrence;
}

}  // namespace sak::pst

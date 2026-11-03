// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_ics_writer.cpp
/// @brief RFC 5545 iCalendar output for calendar items.

#include "sak/email_ics_writer.h"

#include "sak/pst_recurrence.h"

#include <QHash>
#include <QStringList>

#include <algorithm>
#include <cstdlib>

namespace sak {

namespace {

constexpr int kMaxLineOctets = 75;
constexpr int kUtf8ContinuationMask = 0xC0;
constexpr int kUtf8ContinuationByte = 0x80;
constexpr ushort kFirstPrintable = 0x20;
constexpr int kTrackTentative = 2;
constexpr int kTrackAccepted = 3;
constexpr int kTrackDeclined = 4;
constexpr int kTrackNotResponded = 5;
constexpr int kRoleOptional = 2;
constexpr int kRoleResource = 3;
constexpr int kMinutesPerHour = 60;
constexpr int kTwoDigits = 2;
constexpr int kDecimal = 10;
constexpr int kFirstZoneSuffix = 2;

const QString kDateFormat = QStringLiteral("yyyyMMdd");
const QString kWallClockFormat = QStringLiteral("yyyyMMdd'T'HHmmss");
const QString kUtcFormat = QStringLiteral("yyyyMMdd'T'HHmmss'Z'");

/// Escape a TEXT value (RFC 5545 3.3.11) so it cannot end its line or add a forged property.
QString escapeText(const QString& value) {
    QString out = value;
    out.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    out.replace(QLatin1Char(';'), QStringLiteral("\\;"));
    out.replace(QLatin1Char(','), QStringLiteral("\\,"));
    out.replace(QStringLiteral("\r\n"), QStringLiteral("\\n"));
    out.replace(QLatin1Char('\r'), QStringLiteral("\\n"));
    out.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    return out;
}

/// Control characters out, so a value cannot end a line or a parameter.
QString stripControls(const QString& value) {
    QString out;
    out.reserve(value.size());
    for (const QChar ch : value) {
        if (ch.unicode() >= kFirstPrintable) {
            out.append(ch);
        }
    }
    return out;
}

/// A parameter value, DQUOTE-wrapped. It cannot hold a DQUOTE, so those are dropped.
QString paramValue(const QString& value) {
    QString clean = stripControls(value);
    clean.remove(QLatin1Char('"'));
    return QLatin1Char('"') + clean + QLatin1Char('"');
}

/// Append one content line, folded at 75 octets on a character boundary.
void appendLine(QByteArray& out, const QString& line) {
    const QByteArray utf8 = line.toUtf8();
    qsizetype pos = 0;
    bool first = true;
    while (pos < utf8.size()) {
        // A continuation line's leading space counts toward its 75.
        const qsizetype room = first ? kMaxLineOctets : kMaxLineOctets - 1;
        qsizetype take = std::min(room, utf8.size() - pos);
        while (pos + take < utf8.size() && take > 1 &&
               (utf8.at(pos + take) & kUtf8ContinuationMask) == kUtf8ContinuationByte) {
            --take;
        }
        if (!first) {
            out += ' ';
        }
        out += utf8.mid(pos, take);
        out += "\r\n";
        pos += take;
        first = false;
    }
}

void appendText(QByteArray& out, const QString& name, const QString& value) {
    appendLine(out, name + QLatin1Char(':') + escapeText(value));
}

QString offsetText(int minutes) {
    const int magnitude = std::abs(minutes);
    return QStringLiteral("%1%2%3")
        .arg(minutes < 0 ? QLatin1Char('-') : QLatin1Char('+'))
        .arg(magnitude / kMinutesPerHour, kTwoDigits, kDecimal, QLatin1Char('0'))
        .arg(magnitude % kMinutesPerHour, kTwoDigits, kDecimal, QLatin1Char('0'));
}

/// RRULE for one daylight change: the Nth weekday of a month, every year.
QString changeRule(const PstTimeZoneRule& rule) {
    static const QStringList kDays = {QStringLiteral("SU"),
                                      QStringLiteral("MO"),
                                      QStringLiteral("TU"),
                                      QStringLiteral("WE"),
                                      QStringLiteral("TH"),
                                      QStringLiteral("FR"),
                                      QStringLiteral("SA")};
    const int week = rule.week >= 5 ? -1 : rule.week;
    return QStringLiteral("FREQ=YEARLY;BYMONTH=%1;BYDAY=%2%3")
        .arg(rule.month)
        .arg(week)
        .arg(kDays.value(rule.day_of_week, kDays.first()));
}

void appendZoneChange(QByteArray& out,
                      const char* kind,
                      const PstTimeZoneRule& rule,
                      int offset_from,
                      int offset_to) {
    appendLine(out, QStringLiteral("BEGIN:%1").arg(QLatin1String(kind)));
    appendLine(out,
               QStringLiteral("DTSTART:16010101T%1%2")
                       .arg(rule.hour, kTwoDigits, kDecimal, QLatin1Char('0'))
                       .arg(rule.minute, kTwoDigits, kDecimal, QLatin1Char('0')) +
                   QStringLiteral("00"));
    appendLine(out, QStringLiteral("TZOFFSETFROM:") + offsetText(offset_from));
    appendLine(out, QStringLiteral("TZOFFSETTO:") + offsetText(offset_to));
    if (rule.month != 0) {
        appendLine(out, QStringLiteral("RRULE:") + changeRule(rule));
    }
    appendLine(out, QStringLiteral("END:%1").arg(QLatin1String(kind)));
}

void appendZone(QByteArray& out, const PstTimeZone& zone) {
    appendLine(out, QStringLiteral("BEGIN:VTIMEZONE"));
    appendLine(out, QStringLiteral("TZID:") + zone.name);
    const bool has_daylight = zone.to_daylight.month != 0 && zone.to_standard.month != 0;
    if (has_daylight) {
        appendZoneChange(out,
                         "STANDARD",
                         zone.to_standard,
                         zone.daylight_offset_minutes,
                         zone.standard_offset_minutes);
        appendZoneChange(out,
                         "DAYLIGHT",
                         zone.to_daylight,
                         zone.standard_offset_minutes,
                         zone.daylight_offset_minutes);
    } else {
        appendZoneChange(
            out, "STANDARD", {}, zone.standard_offset_minutes, zone.standard_offset_minutes);
    }
    appendLine(out, QStringLiteral("END:VTIMEZONE"));
}

bool sameChange(const PstTimeZoneRule& a, const PstTimeZoneRule& b) {
    return a.month == b.month && a.week == b.week && a.day_of_week == b.day_of_week;
}

bool sameRules(const PstTimeZone& a, const PstTimeZone& b) {
    return a.standard_offset_minutes == b.standard_offset_minutes &&
           a.daylight_offset_minutes == b.daylight_offset_minutes &&
           sameChange(a.to_standard, b.to_standard) && sameChange(a.to_daylight, b.to_daylight);
}

/// Add @p zone to @p zones unless it is there. A zone name can be shared by zones with different
/// rules, so such a zone gets a distinguishing suffix: its times must not be read in the other.
void placeZone(PstTimeZone& zone, QVector<PstTimeZone>& zones) {
    const QString base = zone.name;
    for (int attempt = kFirstZoneSuffix;; ++attempt) {
        const auto same_name = std::ranges::find_if(zones, [&zone](const PstTimeZone& other) {
            return other.name == zone.name;
        });
        if (same_name == zones.end()) {
            zones.append(zone);
            return;
        }
        if (sameRules(*same_name, zone)) {
            return;
        }
        zone.name = QStringLiteral("%1 %2").arg(base).arg(attempt);
    }
}

/// The zones the events' times are written in, once each.
QVector<PstTimeZone> zonesInUse(QVector<PstItemDetail>& events) {
    QVector<PstTimeZone> zones;
    for (PstItemDetail& event : events) {
        if (event.recurrence.state == RecurrenceState::Readable && !event.is_all_day &&
            event.time_zone.known) {
            placeZone(event.time_zone, zones);
        }
    }
    return zones;
}

/// How an event's series times are written: as dates, in its zone, or with no zone at all.
struct SeriesTimes {
    bool date_only = false;
    QString zone_name;

    [[nodiscard]] QString line(const QString& name, const QDateTime& wall) const {
        if (date_only) {
            return QStringLiteral("%1;VALUE=DATE:%2").arg(name, wall.toString(kDateFormat));
        }
        if (!zone_name.isEmpty()) {
            return QStringLiteral("%1;TZID=%2:%3")
                .arg(name, zone_name, wall.toString(kWallClockFormat));
        }
        return QStringLiteral("%1:%2").arg(name, wall.toString(kWallClockFormat));
    }
};

QString attendeeAddressValue(const PstAttendee& attendee) {
    return attendee.email.isEmpty() ? QStringLiteral("invalid:nomail")
                                    : QStringLiteral("mailto:") + stripControls(attendee.email);
}

QString attendeeRoleValue(int role) {
    if (role == kRoleOptional) {
        return QStringLiteral("OPT-PARTICIPANT");
    }
    return role == kRoleResource ? QStringLiteral("NON-PARTICIPANT")
                                 : QStringLiteral("REQ-PARTICIPANT");
}

QString attendeeStatusValue(int response) {
    if (response == kTrackTentative) {
        return QStringLiteral("TENTATIVE");
    }
    if (response == kTrackAccepted) {
        return QStringLiteral("ACCEPTED");
    }
    return response == kTrackDeclined ? QStringLiteral("DECLINED") : QStringLiteral("NEEDS-ACTION");
}

void appendAttendees(QByteArray& out, const PstItemDetail& event) {
    bool wrote_organizer = false;
    for (const PstAttendee& attendee : event.attendees) {
        if (attendee.organizer) {
            appendLine(out,
                       QStringLiteral("ORGANIZER;CN=%1:%2")
                           .arg(paramValue(attendee.name), attendeeAddressValue(attendee)));
            wrote_organizer = true;
            continue;
        }
        appendLine(out,
                   QStringLiteral("ATTENDEE;CN=%1;ROLE=%2;PARTSTAT=%3:%4")
                       .arg(paramValue(attendee.name),
                            attendeeRoleValue(attendee.role),
                            attendeeStatusValue(attendee.response),
                            attendeeAddressValue(attendee)));
    }
    if (!wrote_organizer && !event.attendees.isEmpty() &&
        event.sender_email.contains(QLatin1Char('@'))) {
        appendLine(out,
                   QStringLiteral("ORGANIZER;CN=%1:mailto:%2")
                       .arg(paramValue(event.sender_name), stripControls(event.sender_email)));
    }
}

void appendBody(QByteArray& out, const PstItemDetail& event) {
    if (!event.subject.isEmpty()) {
        appendText(out, QStringLiteral("SUMMARY"), event.subject);
    }
    if (!event.location.isEmpty()) {
        appendText(out, QStringLiteral("LOCATION"), event.location);
    }
    if (!event.body_plain.isEmpty()) {
        appendText(out, QStringLiteral("DESCRIPTION"), event.body_plain);
    }
}

QString eventUid(const PstItemDetail& event) {
    // A series and its changed occurrences share one UID: that is what ties them together.
    return event.message_id.isEmpty() ? QStringLiteral("%1@sak-utility").arg(event.node_id)
                                      : event.message_id;
}

void beginEvent(QByteArray& out, const PstItemDetail& event, const QByteArray& stamp) {
    appendLine(out, QStringLiteral("BEGIN:VEVENT"));
    appendText(out, QStringLiteral("UID"), eventUid(event));
    appendLine(out, QStringLiteral("DTSTAMP:") + QString::fromLatin1(stamp));
}

void appendSeriesException(QByteArray& out,
                           const PstItemDetail& event,
                           const PstRecurrenceException& exception,
                           const SeriesTimes& times,
                           const QByteArray& stamp) {
    beginEvent(out, event, stamp);
    appendLine(out, times.line(QStringLiteral("RECURRENCE-ID"), exception.original_start));
    appendLine(out, times.line(QStringLiteral("DTSTART"), exception.start));
    appendLine(out, times.line(QStringLiteral("DTEND"), exception.end));
    PstItemDetail changed = event;
    if (exception.has_subject) {
        changed.subject = exception.subject;
    }
    if (exception.has_location) {
        changed.location = exception.location;
    }
    if (exception.body_changed) {
        // Its own text lives in a hidden attachment. Leaving it blank beats showing the series'.
        changed.body_plain.clear();
    }
    appendBody(out, changed);
    appendAttendees(out, event);
    appendLine(out, QStringLiteral("END:VEVENT"));
}

void appendSeries(QByteArray& out, const PstItemDetail& event, const QByteArray& stamp) {
    const PstRecurrence& series = event.recurrence;
    SeriesTimes times;
    times.date_only = event.is_all_day;
    if (!event.is_all_day && event.time_zone.known) {
        times.zone_name = event.time_zone.name;
    }
    beginEvent(out, event, stamp);
    appendLine(out, times.line(QStringLiteral("DTSTART"), series.wall_start));
    appendLine(out, times.line(QStringLiteral("DTEND"), series.wall_end));
    appendLine(out, QStringLiteral("RRULE:") + series.rrule);
    for (const QDateTime& deleted : series.deleted) {
        appendLine(out, times.line(QStringLiteral("EXDATE"), deleted));
    }
    appendBody(out, event);
    appendAttendees(out, event);
    appendLine(out, QStringLiteral("END:VEVENT"));
    for (const PstRecurrenceException& exception : series.exceptions) {
        appendSeriesException(out, event, exception, times, stamp);
    }
}

void appendSingleEvent(QByteArray& out, const PstItemDetail& event, const QByteArray& stamp) {
    beginEvent(out, event, stamp);
    // An all-day event is a date, not a moment. See allDayDate for how a stored time becomes one.
    if (event.is_all_day && event.start_time.isValid()) {
        const QDate start = pst::allDayDate(event.time_zone, event.start_time);
        appendLine(out, QStringLiteral("DTSTART;VALUE=DATE:") + start.toString(kDateFormat));
        // DTEND is exclusive and must be later. With none, a date event is that one day.
        const QDate end =
            event.end_time.isValid() ? pst::allDayDate(event.time_zone, event.end_time) : QDate();
        if (end > start) {
            appendLine(out, QStringLiteral("DTEND;VALUE=DATE:") + end.toString(kDateFormat));
        }
    } else {
        if (event.start_time.isValid()) {
            appendLine(out,
                       QStringLiteral("DTSTART:") + event.start_time.toUTC().toString(kUtcFormat));
        }
        if (event.end_time.isValid()) {
            appendLine(out, QStringLiteral("DTEND:") + event.end_time.toUTC().toString(kUtcFormat));
        }
    }
    if (event.recurrence.state == RecurrenceState::Unreadable) {
        appendText(out,
                   QStringLiteral("COMMENT"),
                   QStringLiteral("In Outlook this event repeats. %1, so only one occurrence is "
                                  "written here.")
                       .arg(event.recurrence.problem.isEmpty()
                                ? QStringLiteral("The pattern could not be read")
                                : QStringLiteral("The pattern could not be read because ") +
                                      event.recurrence.problem));
    }
    appendBody(out, event);
    appendAttendees(out, event);
    appendLine(out, QStringLiteral("END:VEVENT"));
}

}  // namespace

QByteArray buildIcsCalendar(const QVector<PstItemDetail>& events, const QDateTime& stamp) {
    QVector<PstItemDetail> named = events;
    const QVector<PstTimeZone> zones = zonesInUse(named);

    QByteArray ics;
    appendLine(ics, QStringLiteral("BEGIN:VCALENDAR"));
    appendLine(ics, QStringLiteral("VERSION:2.0"));
    appendLine(ics, QStringLiteral("PRODID:-//SAK Utility//Email Export//EN"));
    appendLine(ics, QStringLiteral("CALSCALE:GREGORIAN"));
    for (const PstTimeZone& zone : zones) {
        appendZone(ics, zone);
    }
    const QByteArray stamp_text = stamp.toUTC().toString(kUtcFormat).toLatin1();
    for (const PstItemDetail& event : named) {
        if (event.recurrence.state == RecurrenceState::Readable) {
            appendSeries(ics, event, stamp_text);
        } else {
            appendSingleEvent(ics, event, stamp_text);
        }
    }
    appendLine(ics, QStringLiteral("END:VCALENDAR"));
    return ics;
}

QStringList icsEventNotes(const PstItemDetail& event) {
    QStringList notes;
    if (event.recurrence.state == RecurrenceState::Unreadable) {
        notes.append(QStringLiteral("\"%1\" repeats, but its pattern could not be read (%2). It "
                                    "was exported as one event.")
                         .arg(event.subject, event.recurrence.problem));
    }
    if (event.attendees_unreadable) {
        notes.append(QStringLiteral("\"%1\": its attendees could not be read, so none were "
                                    "exported.")
                         .arg(event.subject));
    }
    return notes;
}

QString attendeeResponseWord(int response) {
    switch (response) {
    case kTrackTentative:
        return QStringLiteral("Tentative");
    case kTrackAccepted:
        return QStringLiteral("Accepted");
    case kTrackDeclined:
        return QStringLiteral("Declined");
    case kTrackNotResponded:
        return QStringLiteral("No response");
    default:
        return {};
    }
}

}  // namespace sak

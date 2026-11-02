// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_email_ics_writer.cpp
/// @brief What an exported calendar says: attendees, repeating rules, and what it could not carry.

#include "sak/email_ics_writer.h"
#include "sak/pst_recurrence.h"

#include "../support/recurrence_blob.h"

#include <QtTest/QtTest>

using namespace sak::pst_fixture;

namespace {

const QDateTime kStamp(QDate(2026, 11, 2), QTime(12, 0), QTimeZone::UTC);

QDateTime utcAt(int year, int month, int day, int hour, int minute = 0) {
    return QDateTime(QDate(year, month, day), QTime(hour, minute), QTimeZone::UTC);
}

/// Content lines with folding undone, the way a calendar program reads them.
QStringList unfolded(const QByteArray& ics) {
    QString text = QString::fromUtf8(ics);
    text.replace(QStringLiteral("\r\n "), QString());
    QStringList lines = text.split(QStringLiteral("\r\n"));
    if (lines.last().isEmpty()) {
        lines.removeLast();
    }
    return lines;
}

QStringList linesOf(const QByteArray& ics, const QString& prefix) {
    QStringList found;
    for (const QString& line : unfolded(ics)) {
        if (line.startsWith(prefix)) {
            found.append(line);
        }
    }
    return found;
}

sak::PstItemDetail plainEvent() {
    sak::PstItemDetail event;
    event.node_id = 42;
    event.item_type = sak::EmailItemType::Calendar;
    event.subject = QStringLiteral("Staff meeting");
    event.location = QStringLiteral("Front kitchen");
    event.start_time = utcAt(2026, 11, 2, 17);
    event.end_time = utcAt(2026, 11, 2, 18);
    return event;
}

sak::PstItemDetail seriesEvent(RecurrenceSpec spec) {
    sak::PstItemDetail event = plainEvent();
    spec.start_date = QDate(2026, 11, 2);
    event.time_zone = sak::pst::parseTimeZoneStruct(pacificZoneStruct(), QStringLiteral("Pacific"));
    event.recurrence =
        sak::pst::parseAppointmentRecurrence(buildRecurrenceBlob(spec), event.time_zone, false);
    return event;
}

}  // namespace

class EmailIcsWriterTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void aPlainEventIsInUtcWithItsRequiredFields() {
        const QByteArray ics = sak::buildIcsCalendar({plainEvent()}, kStamp);
        QCOMPARE(linesOf(ics, QStringLiteral("DTSTART")),
                 QStringList{QStringLiteral("DTSTART:20261102T170000Z")});
        QCOMPARE(linesOf(ics, QStringLiteral("DTEND")),
                 QStringList{QStringLiteral("DTEND:20261102T180000Z")});
        QCOMPARE(linesOf(ics, QStringLiteral("UID")),
                 QStringList{QStringLiteral("UID:42@sak-utility")});
        QCOMPARE(linesOf(ics, QStringLiteral("DTSTAMP")),
                 QStringList{QStringLiteral("DTSTAMP:20261102T120000Z")});
        QVERIFY(linesOf(ics, QStringLiteral("RRULE")).isEmpty());
        QVERIFY(linesOf(ics, QStringLiteral("BEGIN:VTIMEZONE")).isEmpty());
    }

    void attendeesCarryTheirRoleReplyAndAddress() {
        sak::PstItemDetail event = plainEvent();
        sak::PstAttendee owner;
        owner.name = QStringLiteral("Dana Whitlock");
        owner.email = QStringLiteral("dana@harborlane.example");
        owner.organizer = true;
        sak::PstAttendee required;
        required.name = QStringLiteral("Marisol Quint");
        required.email = QStringLiteral("marisol@harborlane.example");
        required.response = 3;
        sak::PstAttendee optional;
        optional.name = QStringLiteral("Owen Lark");
        optional.email = QStringLiteral("owen@harborlane.example");
        optional.role = 2;
        optional.response = 4;
        sak::PstAttendee resource;
        resource.name = QStringLiteral("Back room");
        resource.role = 3;
        resource.response = 2;
        event.attendees = {owner, required, optional, resource};

        const QByteArray ics = sak::buildIcsCalendar({event}, kStamp);
        QCOMPARE(linesOf(ics, QStringLiteral("ORGANIZER")),
                 QStringList{QStringLiteral(
                     "ORGANIZER;CN=\"Dana Whitlock\":mailto:dana@harborlane.example")});
        QCOMPARE(linesOf(ics, QStringLiteral("ATTENDEE")),
                 QStringList({QStringLiteral("ATTENDEE;CN=\"Marisol Quint\";ROLE=REQ-PARTICIPANT;"
                                             "PARTSTAT=ACCEPTED:mailto:marisol@harborlane.example"),
                              QStringLiteral("ATTENDEE;CN=\"Owen Lark\";ROLE=OPT-PARTICIPANT;"
                                             "PARTSTAT=DECLINED:mailto:owen@harborlane.example"),
                              QStringLiteral("ATTENDEE;CN=\"Back room\";ROLE=NON-PARTICIPANT;"
                                             "PARTSTAT=TENTATIVE:invalid:nomail")}));
    }

    // With no organizer row, the sender is who sent the invitation.
    void theSenderOrganizesWhenNoRowSaysSo() {
        sak::PstItemDetail event = plainEvent();
        event.sender_name = QStringLiteral("Dana Whitlock");
        event.sender_email = QStringLiteral("dana@harborlane.example");
        sak::PstAttendee attendee;
        attendee.name = QStringLiteral("Marisol Quint");
        event.attendees = {attendee};
        QCOMPARE(linesOf(sak::buildIcsCalendar({event}, kStamp), QStringLiteral("ORGANIZER")),
                 QStringList{QStringLiteral(
                     "ORGANIZER;CN=\"Dana Whitlock\":mailto:dana@harborlane.example")});

        event.attendees.clear();
        QVERIFY(
            linesOf(sak::buildIcsCalendar({event}, kStamp), QStringLiteral("ORGANIZER")).isEmpty());
    }

    // A stored name is whatever someone typed. It must not end a line or add a parameter.
    void anAttendeeNameCannotForgeAProperty() {
        sak::PstItemDetail event = plainEvent();
        sak::PstAttendee hostile;
        hostile.name =
            QStringLiteral("Eve\"; ROLE=CHAIR\r\nATTENDEE:mailto:forged@example.example");
        hostile.email = QStringLiteral("eve@example.example\r\nX-FORGED:1");
        event.attendees = {hostile};
        const QByteArray ics = sak::buildIcsCalendar({event}, kStamp);
        QCOMPARE(linesOf(ics, QStringLiteral("ATTENDEE")).size(), 1);
        QVERIFY(linesOf(ics, QStringLiteral("X-FORGED")).isEmpty());
        const QString line = linesOf(ics, QStringLiteral("ATTENDEE")).first();
        QVERIFY2(line.contains(QStringLiteral("CN=\"Eve; ROLE=CHAIRATTENDEE")), qPrintable(line));
    }

    void aSeriesIsWrittenInItsOwnZone() {
        RecurrenceSpec spec;
        spec.end_type = kRecurEndByDate;
        spec.end_date = QDate(2027, 1, 25);
        const QByteArray ics = sak::buildIcsCalendar({seriesEvent(spec)}, kStamp);

        QCOMPARE(linesOf(ics, QStringLiteral("DTSTART;")),
                 QStringList{QStringLiteral("DTSTART;TZID=Pacific:20261102T090000")});
        QCOMPARE(linesOf(ics, QStringLiteral("DTEND;")),
                 QStringList{QStringLiteral("DTEND;TZID=Pacific:20261102T100000")});
        QCOMPARE(linesOf(ics, QStringLiteral("RRULE:FREQ=WEEKLY")),
                 QStringList{QStringLiteral(
                     "RRULE:FREQ=WEEKLY;INTERVAL=1;BYDAY=MO;WKST=SU;UNTIL=20270125T170000Z")});

        // The zone it names is defined, with both of its changes.
        QCOMPARE(linesOf(ics, QStringLiteral("TZID:")),
                 QStringList{QStringLiteral("TZID:Pacific")});
        QCOMPARE(linesOf(ics, QStringLiteral("RRULE:FREQ=YEARLY")),
                 QStringList({QStringLiteral("RRULE:FREQ=YEARLY;BYMONTH=11;BYDAY=1SU"),
                              QStringLiteral("RRULE:FREQ=YEARLY;BYMONTH=3;BYDAY=2SU")}));
        QCOMPARE(linesOf(ics, QStringLiteral("TZOFFSETTO")),
                 QStringList(
                     {QStringLiteral("TZOFFSETTO:-0800"), QStringLiteral("TZOFFSETTO:-0700")}));
        QCOMPARE(linesOf(ics, QStringLiteral("DTSTART:1601")),
                 QStringList({QStringLiteral("DTSTART:16010101T020000"),
                              QStringLiteral("DTSTART:16010101T020000")}));
    }

    void aZoneIsDefinedOnceHoweverManyEventsUseIt() {
        RecurrenceSpec spec;
        const QByteArray ics = sak::buildIcsCalendar({seriesEvent(spec), seriesEvent(spec)},
                                                     kStamp);
        QCOMPARE(linesOf(ics, QStringLiteral("BEGIN:VTIMEZONE")).size(), 1);
    }

    // Two zones can share a name and differ in their rules. Times must not read in the wrong one.
    void zonesOfOneNameWithDifferentRulesStayApart() {
        RecurrenceSpec spec;
        sak::PstItemDetail other = seriesEvent(spec);
        other.time_zone.standard_offset_minutes = 60;
        other.time_zone.daylight_offset_minutes = 120;
        const QByteArray ics = sak::buildIcsCalendar({seriesEvent(spec), other}, kStamp);
        QCOMPARE(linesOf(ics, QStringLiteral("TZID:")),
                 QStringList({QStringLiteral("TZID:Pacific"), QStringLiteral("TZID:Pacific 2")}));
        QCOMPARE(linesOf(ics, QStringLiteral("DTSTART;")),
                 QStringList({QStringLiteral("DTSTART;TZID=Pacific:20261102T090000"),
                              QStringLiteral("DTSTART;TZID=Pacific 2:20261102T090000")}));
    }

    void aDeletedOccurrenceIsAnExdateAndAChangedOneIsItsOwnEvent() {
        RecurrenceSpec spec;
        spec.deleted = {QDate(2026, 11, 9)};
        RecurChangedOccurrence moved;
        moved.original_start = utcAt(2026, 11, 23, 9);
        moved.start = utcAt(2026, 11, 24, 14);
        moved.end = utcAt(2026, 11, 24, 15);
        moved.subject = QStringLiteral("Moved review");
        spec.changed = {moved};
        const QByteArray ics = sak::buildIcsCalendar({seriesEvent(spec)}, kStamp);

        QCOMPARE(linesOf(ics, QStringLiteral("EXDATE")),
                 QStringList{QStringLiteral("EXDATE;TZID=Pacific:20261109T090000")});
        QCOMPARE(linesOf(ics, QStringLiteral("RECURRENCE-ID")),
                 QStringList{QStringLiteral("RECURRENCE-ID;TZID=Pacific:20261123T090000")});
        QCOMPARE(linesOf(ics, QStringLiteral("BEGIN:VEVENT")).size(), 2);
        // One UID: that is what makes the second event part of the first.
        QCOMPARE(linesOf(ics, QStringLiteral("UID")),
                 QStringList(
                     {QStringLiteral("UID:42@sak-utility"), QStringLiteral("UID:42@sak-utility")}));
        QCOMPARE(linesOf(ics, QStringLiteral("DTSTART;")),
                 QStringList({QStringLiteral("DTSTART;TZID=Pacific:20261102T090000"),
                              QStringLiteral("DTSTART;TZID=Pacific:20261124T140000")}));
        QCOMPARE(linesOf(ics, QStringLiteral("SUMMARY")),
                 QStringList({QStringLiteral("SUMMARY:Staff meeting"),
                              QStringLiteral("SUMMARY:Moved review")}));
        // What the change left alone comes from the series.
        QCOMPARE(linesOf(ics, QStringLiteral("LOCATION")),
                 QStringList({QStringLiteral("LOCATION:Front kitchen"),
                              QStringLiteral("LOCATION:Front kitchen")}));
    }

    // A changed occurrence's own text is in a hidden attachment. The series' is not it.
    void aChangedBodyIsLeftOutRatherThanShownWrong() {
        RecurrenceSpec spec;
        RecurChangedOccurrence moved;
        moved.original_start = utcAt(2026, 11, 23, 9);
        moved.start = utcAt(2026, 11, 24, 14);
        moved.end = utcAt(2026, 11, 24, 15);
        moved.body_changed = true;
        spec.changed = {moved};
        sak::PstItemDetail event = seriesEvent(spec);
        event.body_plain = QStringLiteral("The usual agenda");
        QCOMPARE(linesOf(sak::buildIcsCalendar({event}, kStamp), QStringLiteral("DESCRIPTION")),
                 QStringList{QStringLiteral("DESCRIPTION:The usual agenda")});
    }

    void anAllDaySeriesIsDates() {
        RecurrenceSpec spec;
        spec.frequency = kRecurYearly;
        spec.pattern_type = kPatternTypeMonth;
        spec.period = 12;
        spec.day_or_mask = 4;
        spec.start_date = QDate(2027, 3, 4);
        spec.start_minutes = 0;
        spec.end_minutes = 1440;
        spec.deleted = {QDate(2028, 3, 4)};
        sak::PstItemDetail event = plainEvent();
        event.is_all_day = true;
        event.time_zone = sak::pst::parseTimeZoneStruct(pacificZoneStruct(),
                                                        QStringLiteral("Pacific"));
        event.recurrence =
            sak::pst::parseAppointmentRecurrence(buildRecurrenceBlob(spec), event.time_zone, true);
        const QByteArray ics = sak::buildIcsCalendar({event}, kStamp);
        QCOMPARE(linesOf(ics, QStringLiteral("DTSTART")),
                 QStringList{QStringLiteral("DTSTART;VALUE=DATE:20270304")});
        QCOMPARE(linesOf(ics, QStringLiteral("DTEND")),
                 QStringList{QStringLiteral("DTEND;VALUE=DATE:20270305")});
        QCOMPARE(linesOf(ics, QStringLiteral("EXDATE")),
                 QStringList{QStringLiteral("EXDATE;VALUE=DATE:20280304")});
        // Dates name no zone, so none is defined.
        QVERIFY(linesOf(ics, QStringLiteral("BEGIN:VTIMEZONE")).isEmpty());
    }

    // Plenty of real events carry no zone at all. Their wall clock is what is known.
    void aSeriesWithNoZoneFloats() {
        RecurrenceSpec spec;
        spec.start_date = QDate(2026, 11, 2);
        spec.end_type = kRecurEndByDate;
        spec.end_date = QDate(2027, 1, 25);
        sak::PstItemDetail event = plainEvent();
        event.recurrence =
            sak::pst::parseAppointmentRecurrence(buildRecurrenceBlob(spec), {}, false);
        const QByteArray ics = sak::buildIcsCalendar({event}, kStamp);
        QCOMPARE(linesOf(ics, QStringLiteral("DTSTART")),
                 QStringList{QStringLiteral("DTSTART:20261102T090000")});
        QCOMPARE(linesOf(ics, QStringLiteral("RRULE")),
                 QStringList{QStringLiteral(
                     "RRULE:FREQ=WEEKLY;INTERVAL=1;BYDAY=MO;WKST=SU;UNTIL=20270125T090000")});
        QVERIFY(linesOf(ics, QStringLiteral("BEGIN:VTIMEZONE")).isEmpty());
    }

    void aSingleAllDayEventInAKnownZoneIsADate() {
        sak::PstItemDetail event = plainEvent();
        event.is_all_day = true;
        event.time_zone = sak::pst::parseTimeZoneStruct(pacificZoneStruct(),
                                                        QStringLiteral("Pacific"));
        event.start_time = utcAt(2026, 12, 25, 8);  // Local midnight in Pacific standard time
        event.end_time = utcAt(2026, 12, 26, 8);
        const QByteArray ics = sak::buildIcsCalendar({event}, kStamp);
        QCOMPARE(linesOf(ics, QStringLiteral("DTSTART")),
                 QStringList{QStringLiteral("DTSTART;VALUE=DATE:20261225")});
        QCOMPARE(linesOf(ics, QStringLiteral("DTEND")),
                 QStringList{QStringLiteral("DTEND;VALUE=DATE:20261226")});
    }

    void anUnreadablePatternIsSaidOnTheEventAndInTheLog() {
        RecurrenceSpec spec;
        spec.frequency = 0x2099;
        sak::PstItemDetail event = seriesEvent(spec);
        QCOMPARE(event.recurrence.state, sak::RecurrenceState::Unreadable);
        const QByteArray ics = sak::buildIcsCalendar({event}, kStamp);
        QVERIFY(linesOf(ics, QStringLiteral("RRULE")).isEmpty());
        QCOMPARE(linesOf(ics, QStringLiteral("DTSTART")),
                 QStringList{QStringLiteral("DTSTART:20261102T170000Z")});
        QCOMPARE(linesOf(ics, QStringLiteral("COMMENT")).size(), 1);
        QVERIFY(linesOf(ics, QStringLiteral("COMMENT"))
                    .first()
                    .contains(QStringLiteral("could not be read")));

        const QStringList notes = sak::icsEventNotes(event);
        QCOMPARE(notes.size(), 1);
        QVERIFY2(notes.first().contains(QStringLiteral("Staff meeting")) &&
                     notes.first().contains(QStringLiteral("exported as one event")),
                 qPrintable(notes.first()));
        QVERIFY(sak::icsEventNotes(plainEvent()).isEmpty());
    }

    void unreadableAttendeesAreSaidInTheLog() {
        sak::PstItemDetail event = plainEvent();
        event.attendees_unreadable = true;
        QCOMPARE(sak::icsEventNotes(event).size(), 1);
        QVERIFY(sak::icsEventNotes(event).first().contains(QStringLiteral("attendees")));
    }

    // RFC 5545 3.1: no line past 75 octets, and folding never splits a character.
    void longLinesFoldOnACharacterBoundary() {
        sak::PstItemDetail event = plainEvent();
        event.subject = QString(200, QChar(0x4F1A));
        event.body_plain = QString(400, QLatin1Char('x'));
        const QByteArray ics = sak::buildIcsCalendar({event}, kStamp);
        for (const QByteArray& line : ics.split('\n')) {
            QVERIFY2(line.endsWith('\r') || line.isEmpty(), line.constData());
            QVERIFY2(line.size() <= 76, line.constData());  // 75 and the CR
            QVERIFY(!QString::fromUtf8(line).contains(QChar::ReplacementCharacter));
        }
        QCOMPARE(linesOf(ics, QStringLiteral("SUMMARY")),
                 QStringList{QStringLiteral("SUMMARY:") + QString(200, QChar(0x4F1A))});
    }

    void textCannotEndItsLine() {
        sak::PstItemDetail event = plainEvent();
        event.subject = QStringLiteral("a;b,c\\d\r\nX-FORGED:1");
        const QByteArray ics = sak::buildIcsCalendar({event}, kStamp);
        QVERIFY(linesOf(ics, QStringLiteral("X-FORGED")).isEmpty());
        QCOMPARE(linesOf(ics, QStringLiteral("SUMMARY")),
                 QStringList{QStringLiteral("SUMMARY:a\\;b\\,c\\\\d\\nX-FORGED:1")});
    }

    void responseWords() {
        QCOMPARE(sak::attendeeResponseWord(3), QStringLiteral("Accepted"));
        QCOMPARE(sak::attendeeResponseWord(4), QStringLiteral("Declined"));
        QCOMPARE(sak::attendeeResponseWord(2), QStringLiteral("Tentative"));
        QVERIFY(sak::attendeeResponseWord(0).isEmpty());
    }
};

QTEST_GUILESS_MAIN(EmailIcsWriterTests)
#include "test_email_ics_writer.moc"

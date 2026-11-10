// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_email_search_worker.cpp
/// @brief Unit tests for the email search worker

#include "sak/email_constants.h"
#include "sak/email_search_worker.h"
#include "sak/email_types.h"
#include "sak/mbox_parser.h"
#include "sak/pst_named_properties.h"
#include "sak/pst_parser.h"

#include "../support/pst_store_builder.h"

#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTimeZone>
#include <QtTest/QtTest>

#include <algorithm>

class TestEmailSearchWorker : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- Criteria Defaults -----------------------------------------------
    void criteriaDefaults();
    void criteriaFieldFlags();
    void criteriaDateRange();
    void criteriaFolderScope();
    void criteriaMapiProperty();

    // -- Cancel ----------------------------------------------------------
    void cancelBeforeSearchDoesNotPoisonNextSearch();
    void cancelMidSearchEmitsSearchCancelledNotComplete();

    // -- Search With Null Parser -----------------------------------------
    void searchWithNullPstParserFailsClosed();
    void searchWithNullMboxParserFailsClosed();

    // -- Search Hit Structure --------------------------------------------
    void searchHitDefaults();
    void searchHitFields();

    // -- B7-28: body search maps a hit to the correct message index ------
    void mboxBodySearchUsesMessageIndex();

    // -- B7-34: MBOX honors recipient / attachment-name criteria ---------
    void mboxRecipientSearchMatchesToField();
    void mboxAttachmentNameSearchMatches();

    // -- Filters ---------------------------------------------------------
    void plainSearchIsUnchangedWhenNoFilterIsSet();
    void itemTypeFilterGroupsAsTheWindowsDo();
    void hasAttachmentFilterNarrows();
    void dateRangeCoversBothEndsAndLeavesOutTheUndated();
    void anEventIsInARangeByWhenItHappens();
    void filtersAloneListWhatTheyMatch();
    void aFilteredSearchIsASubsetOfThePlainOne();
    void anMboxHasOnlyEmailToFind();
    void mboxDateAndAttachmentFilters();
    void theResultLimitCountsWhatIsLeftAfterFiltering();
    void theResultLimitCountsWhatIsLeftInAPstToo();
    void aSearchFindsTheRealTextOfAnEncodedHeader();
    void aDateRangeFindsAnAllDayEventOnItsOwnDate_data();
    void aDateRangeFindsAnAllDayEventOnItsOwnDate();
};

namespace {

using namespace sak::pst_fixture;

BuilderMessage filterMail(const QString& subject, const QDateTime& date, bool attached = false) {
    BuilderMessage msg;
    msg.subject = subject;
    msg.sender_name = QStringLiteral("Owen Lark");
    msg.sender_email = QStringLiteral("owen@harborlane.example");
    msg.date = date;
    msg.body_plain = QStringLiteral("needle in ") + subject;
    if (attached) {
        msg.attachments = {{QStringLiteral("f.bin"), {}, QByteArray("data"), {}}};
    }
    return msg;
}

QDateTime dayOf(int month, int day) {
    return QDateTime(QDate(2026, month, day), QTime(12, 0), QTimeZone::UTC);
}

BuilderMessage ofClass(const QString& cls, const QString& subject, const QDateTime& date) {
    BuilderMessage msg;
    msg.message_class = cls;
    msg.subject = subject;
    msg.date = date;
    msg.body_plain = QStringLiteral("needle in ") + subject;
    return msg;
}

/// One store holding one of everything a filter can ask for.
QByteArray filterStore() {
    BuilderFolder inbox;
    inbox.name = QStringLiteral("Inbox");
    BuilderMessage undated = filterMail(QStringLiteral("Undated draft"), {});
    inbox.messages = {
        filterMail(QStringLiteral("March mail"), dayOf(3, 10)),
        filterMail(QStringLiteral("April mail with file"), dayOf(4, 10), true),
        filterMail(QStringLiteral("May mail"), dayOf(5, 10)),
        undated,
        ofClass(
            QStringLiteral("IPM.Schedule.Meeting.Request"), QStringLiteral("Invite"), dayOf(1, 5)),
    };
    BuilderMessage event =
        ofClass(QStringLiteral("IPM.Appointment"), QStringLiteral("Event"), dayOf(1, 5));
    event.named_props = {{0x02,
                          sak::pst::kLidAppointmentStartWhole,
                          sak::email::kPropTypeSysTime,
                          builderFiletime(dayOf(4, 20))}};
    BuilderFolder calendar;
    calendar.name = QStringLiteral("Calendar");
    calendar.container_class = QStringLiteral("IPF.Appointment");
    calendar.messages = {event};
    BuilderFolder contacts;
    contacts.name = QStringLiteral("Contacts");
    contacts.container_class = QStringLiteral("IPF.Contact");
    contacts.messages = {
        ofClass(QStringLiteral("IPM.Contact"), QStringLiteral("Person"), dayOf(2, 2)),
        ofClass(QStringLiteral("IPM.DistList"), QStringLiteral("Group"), dayOf(2, 3)),
    };
    BuilderFolder tasks;
    tasks.name = QStringLiteral("Tasks");
    tasks.container_class = QStringLiteral("IPF.Task");
    tasks.messages = {ofClass(QStringLiteral("IPM.Task"), QStringLiteral("Job"), dayOf(2, 4))};
    BuilderFolder notes;
    notes.name = QStringLiteral("Notes");
    notes.container_class = QStringLiteral("IPF.StickyNote");
    notes.messages = {
        ofClass(QStringLiteral("IPM.StickyNote"), QStringLiteral("Note"), dayOf(2, 5))};
    BuilderFolder journal;
    journal.name = QStringLiteral("Journal");
    journal.container_class = QStringLiteral("IPF.Journal");
    journal.messages = {
        ofClass(QStringLiteral("IPM.Activity"), QStringLiteral("Log"), dayOf(2, 6))};
    return PstStoreBuilder{}.build({inbox, calendar, contacts, tasks, notes, journal});
}

QString writeTemp(const QByteArray& bytes, const QString& suffix, QTemporaryDir& dir) {
    static int counter = 0;
    const QString path = dir.filePath(QStringLiteral("search%1%2").arg(++counter).arg(suffix));
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(bytes);
    }
    return path;
}

QStringList hitSubjects(PstParser& parser, const sak::EmailSearchCriteria& criteria) {
    EmailSearchWorker worker;
    QStringList subjects;
    QObject::connect(&worker,
                     &EmailSearchWorker::searchHit,
                     &worker,
                     [&subjects](const sak::EmailSearchHit& hit) { subjects.append(hit.subject); });
    worker.search(&parser, criteria);
    subjects.sort();
    return subjects;
}

sak::EmailSearchCriteria needle() {
    sak::EmailSearchCriteria criteria;
    criteria.query_text = QStringLiteral("needle");
    return criteria;
}

QByteArray dateAttachedMailbox() {
    QByteArray box;
    const auto add = [&box](const char* subject, const char* date, bool attached) {
        box += "From a@example.test Mon Jan  1 00:00:00 2024\n";
        box += "From: a@example.test\n";
        box += QByteArray("Subject: ") + subject + "\n";
        if (date[0] != '\0') {
            box += QByteArray("Date: ") + date + "\n";
        }
        if (attached) {
            box += "MIME-Version: 1.0\nContent-Type: multipart/mixed; boundary=\"B\"\n\n";
            box += "--B\nContent-Type: text/plain\n\nneedle\n";
            box += "--B\nContent-Type: application/pdf; name=\"f.pdf\"\n";
            box += "Content-Disposition: attachment; filename=\"f.pdf\"\n\nX\n--B--\n\n";
        } else {
            box += "\nneedle\n\n";
        }
    };
    // A text file that came with a Content-ID: inline, and still an attachment.
    box += "From a@example.test Mon Jan  1 00:00:00 2024\n";
    box += "From: a@example.test\nSubject: Inline file\n";
    box += "Date: Sun, 15 Mar 2026 12:00:00 +0000\n";
    box += "MIME-Version: 1.0\nContent-Type: multipart/mixed; boundary=\"I\"\n\n";
    box += "--I\nContent-Type: text/plain\n\nneedle\n";
    box += "--I\nContent-Type: text/plain\n";
    box += "Content-Disposition: inline; filename=\"notes.txt\"\n\nneedle\n--I--\n\n";
    // Multipart with no attachment: its headers look just like a message with one.
    box += "From a@example.test Mon Jan  1 00:00:00 2024\n";
    box += "From: a@example.test\nSubject: Alternative\n";
    box += "MIME-Version: 1.0\nContent-Type: multipart/alternative; boundary=\"A\"\n\n";
    box += "--A\nContent-Type: text/plain\n\nneedle\n";
    box += "--A\nContent-Type: text/html\n\n<p>needle</p>\n--A--\n\n";
    add("March", "Tue, 10 Mar 2026 12:00:00 +0000", false);
    add("April", "Fri, 10 Apr 2026 12:00:00 +0000", true);
    add("May", "Sun, 10 May 2026 12:00:00 +0000", false);
    return box;
}

QStringList mboxHitSubjects(MboxParser& parser, const sak::EmailSearchCriteria& criteria) {
    EmailSearchWorker worker;
    QStringList subjects;
    QObject::connect(&worker,
                     &EmailSearchWorker::searchHit,
                     &worker,
                     [&subjects](const sak::EmailSearchHit& hit) { subjects.append(hit.subject); });
    worker.searchMbox(&parser, criteria);
    subjects.sort();
    return subjects;
}

}  // namespace

// ============================================================================
// Criteria Defaults
// ============================================================================

void TestEmailSearchWorker::criteriaDefaults() {
    sak::EmailSearchCriteria criteria;
    QVERIFY(criteria.query_text.isEmpty());
    QVERIFY(criteria.search_subject);
    QVERIFY(criteria.search_body);
    QVERIFY(criteria.search_sender);
    QVERIFY(!criteria.search_recipients);
    QVERIFY(!criteria.search_attachment_names);
    QVERIFY(!criteria.case_sensitive);
    QVERIFY(!criteria.has_attachment_only);
    QCOMPARE(criteria.item_type_filter, sak::EmailItemType::Unknown);
    QCOMPARE(criteria.folder_scope_id, static_cast<uint64_t>(0));
    QCOMPARE(criteria.mapi_property_id, static_cast<uint16_t>(0));
    QVERIFY(criteria.mapi_property_value.isEmpty());
}

void TestEmailSearchWorker::criteriaFieldFlags() {
    sak::EmailSearchCriteria criteria;
    criteria.search_subject = false;
    criteria.search_body = false;
    criteria.search_sender = false;
    criteria.search_recipients = true;
    criteria.search_attachment_names = true;
    criteria.case_sensitive = true;

    QVERIFY(!criteria.search_subject);
    QVERIFY(!criteria.search_body);
    QVERIFY(!criteria.search_sender);
    QVERIFY(criteria.search_recipients);
    QVERIFY(criteria.search_attachment_names);
    QVERIFY(criteria.case_sensitive);
}

void TestEmailSearchWorker::criteriaDateRange() {
    sak::EmailSearchCriteria criteria;
    QVERIFY(!criteria.date_from.isValid());
    QVERIFY(!criteria.date_to.isValid());

    criteria.date_from = QDateTime(QDate(2024, 1, 1), QTime(0, 0, 0), QTimeZone::UTC);
    criteria.date_to = QDateTime(QDate(2024, 12, 31), QTime(23, 59, 59), QTimeZone::UTC);

    QVERIFY(criteria.date_from.isValid());
    QVERIFY(criteria.date_to.isValid());
    QVERIFY(criteria.date_from < criteria.date_to);

    // Vacuous as written: every assertion above compares values this test just assigned. Prove
    // instead that the window actually FILTERS -- one message before date_from and one after
    // date_to must be rejected, leaving only the in-window message even though all three match
    // the query.
    QTemporaryFile mbox;
    QVERIFY(mbox.open());
    QByteArray content;
    content += "From a@example.com Sun Dec 31 12:00:00 2023\r\n";
    content += "From: A <a@example.com>\r\n";
    content += "Subject: WINDOWKEY too early\r\n";
    content += "Date: Sun, 31 Dec 2023 12:00:00 +0000\r\n";
    content += "\r\n";
    content += "Body.\r\n";
    content += "\r\n";
    content += "From b@example.com Sat Jun  1 12:00:00 2024\r\n";
    content += "From: B <b@example.com>\r\n";
    content += "Subject: WINDOWKEY inside\r\n";
    content += "Date: Sat, 01 Jun 2024 12:00:00 +0000\r\n";
    content += "\r\n";
    content += "Body.\r\n";
    content += "\r\n";
    content += "From c@example.com Wed Jan  1 12:00:00 2025\r\n";
    content += "From: C <c@example.com>\r\n";
    content += "Subject: WINDOWKEY too late\r\n";
    content += "Date: Wed, 01 Jan 2025 12:00:00 +0000\r\n";
    content += "\r\n";
    content += "Body.\r\n";
    mbox.write(content);
    mbox.close();

    MboxParser parser;
    parser.open(mbox.fileName());
    QVERIFY(parser.isOpen());
    parser.indexMessages();
    QCOMPARE(parser.messageCount(), 3);

    criteria.query_text = QStringLiteral("WINDOWKEY");
    criteria.search_body = false;
    criteria.search_sender = false;

    EmailSearchWorker worker;
    QSignalSpy hit_spy(&worker, &EmailSearchWorker::searchHit);
    QSignalSpy done_spy(&worker, &EmailSearchWorker::searchComplete);
    worker.searchMbox(&parser, criteria);

    QCOMPARE(done_spy.count(), 1);
    QCOMPARE(done_spy.first().at(0).toInt(), 1);
    QCOMPARE(hit_spy.count(), 1);  // ONLY the in-window message survives both bounds
    const auto hit = hit_spy.first().first().value<sak::EmailSearchHit>();
    QCOMPARE(hit.item_node_id, static_cast<uint64_t>(1));
    QCOMPARE(hit.subject, QStringLiteral("WINDOWKEY inside"));
    parser.close();
}

void TestEmailSearchWorker::criteriaFolderScope() {
    sak::EmailSearchCriteria criteria;
    criteria.folder_scope_id = 12'345;
    QCOMPARE(criteria.folder_scope_id, static_cast<uint64_t>(12'345));
}

void TestEmailSearchWorker::criteriaMapiProperty() {
    sak::EmailSearchCriteria criteria;
    criteria.mapi_property_id = sak::email::kPropIdSubject;
    criteria.mapi_property_value = QStringLiteral("Important");

    // Pin the MS-OXPROPS wire tag itself: kPropIdSubject == kPropIdSubject holds no matter what
    // value the constant takes, so it proves nothing about the tag actually searched for.
    QCOMPARE(criteria.mapi_property_id, static_cast<uint16_t>(0x0037));
    QCOMPARE(criteria.mapi_property_value, QStringLiteral("Important"));
}

// ============================================================================
// Cancel
// ============================================================================

// A cancel raised while nothing is running must be a harmless no-op that does NOT
// poison the NEXT search: searchMbox() clears the flag on entry. Without that reset
// the search below would break out on the first message and report zero hits (the
// same stale-cancel bug MboxParser had in B7-24).
void TestEmailSearchWorker::cancelBeforeSearchDoesNotPoisonNextSearch() {
    QTemporaryFile mbox;
    QVERIFY(mbox.open());
    QByteArray content;
    content += "From a@example.com Mon Jan  1 00:00:00 2024\r\n";
    content += "From: A <a@example.com>\r\n";
    content += "Subject: ZEBRASUBJECT\r\n";
    content += "Date: Mon, 01 Jan 2024 00:00:00 +0000\r\n";
    content += "\r\n";
    content += "Ordinary body text.\r\n";
    mbox.write(content);
    mbox.close();

    MboxParser parser;
    parser.open(mbox.fileName());
    QVERIFY(parser.isOpen());
    parser.indexMessages();

    sak::EmailSearchCriteria criteria;
    criteria.query_text = QStringLiteral("ZEBRASUBJECT");
    criteria.search_body = false;
    criteria.search_sender = false;

    EmailSearchWorker worker;
    worker.cancel();
    worker.cancel();  // idempotent: a repeated cancel must not wedge the worker either

    QSignalSpy hit_spy(&worker, &EmailSearchWorker::searchHit);
    QSignalSpy done_spy(&worker, &EmailSearchWorker::searchComplete);
    QSignalSpy cancel_spy(&worker, &EmailSearchWorker::searchCancelled);
    worker.searchMbox(&parser, criteria);

    QCOMPARE(done_spy.count(), 1);
    QCOMPARE(hit_spy.count(), 1);
    QCOMPARE(cancel_spy.count(), 0);  // a finished run is COMPLETE, never also cancelled
    QCOMPARE(done_spy.first().at(0).toInt(), 1);  // the run was not cut short
    // The hit must carry the matching message verbatim: subject/sender/date/type/container are
    // filled by searchMessagePage (email_search_worker.cpp:290-298) and are read by nothing
    // else in this file, so a transposed or hard-coded field ships green without these.
    const auto hit = hit_spy.first().first().value<sak::EmailSearchHit>();
    QCOMPARE(hit.item_node_id, static_cast<uint64_t>(0));
    QCOMPARE(hit.item_type, sak::EmailItemType::Email);
    QCOMPARE(hit.subject, QStringLiteral("ZEBRASUBJECT"));
    QCOMPARE(hit.sender, QStringLiteral("A <a@example.com>"));
    QCOMPARE(hit.match_field, QStringLiteral("subject"));
    QCOMPARE(hit.context_snippet, QStringLiteral("ZEBRASUBJECT"));
    QCOMPARE(hit.folder_path, QStringLiteral("MBOX"));
    // Explicit +0000 in the fixture plus toUTC() keeps this host- and locale-independent.
    QCOMPARE(hit.date.toUTC().toString(Qt::ISODate), QStringLiteral("2024-01-01T00:00:00Z"));
    parser.close();
}

// A cancel raised WHILE the search is running must terminate on searchCancelled -- carrying
// the partial hit count -- and must NOT emit searchComplete. Otherwise a truncated scan is
// announced to the UI as a finished one (R5-P6-18). The cancel is injected from the searchHit
// slot on the first match; searchMbox runs synchronously here, so the slot fires inline and the
// second message's top-of-loop cancel check stops the run before it completes.
void TestEmailSearchWorker::cancelMidSearchEmitsSearchCancelledNotComplete() {
    QTemporaryFile mbox;
    QVERIFY(mbox.open());
    QByteArray content;
    content += "From a@example.com Mon Jan  1 00:00:00 2024\r\n";
    content += "From: A <a@example.com>\r\n";
    content += "Subject: MATCHME first\r\n";
    content += "Date: Mon, 01 Jan 2024 00:00:00 +0000\r\n";
    content += "\r\n";
    content += "Body one.\r\n";
    content += "\r\n";
    content += "From b@example.com Mon Jan  1 00:00:00 2024\r\n";
    content += "From: B <b@example.com>\r\n";
    content += "Subject: MATCHME second\r\n";
    content += "Date: Mon, 01 Jan 2024 00:00:01 +0000\r\n";
    content += "\r\n";
    content += "Body two.\r\n";
    mbox.write(content);
    mbox.close();

    MboxParser parser;
    parser.open(mbox.fileName());
    QVERIFY(parser.isOpen());
    parser.indexMessages();

    sak::EmailSearchCriteria criteria;
    criteria.query_text = QStringLiteral("MATCHME");
    criteria.search_body = false;
    criteria.search_sender = false;

    EmailSearchWorker worker;
    QSignalSpy hit_spy(&worker, &EmailSearchWorker::searchHit);
    QSignalSpy done_spy(&worker, &EmailSearchWorker::searchComplete);
    QSignalSpy cancel_spy(&worker, &EmailSearchWorker::searchCancelled);

    // Cancel the moment the first hit is seen, so the second message is never searched.
    QObject::connect(&worker,
                     &EmailSearchWorker::searchHit,
                     &worker,
                     [&worker](sak::EmailSearchHit) { worker.cancel(); });

    worker.searchMbox(&parser, criteria);

    QCOMPARE(hit_spy.count(), 1);                   // only the first match, before the cancel
    QCOMPARE(done_spy.count(), 0);                  // NOT announced as a completed search
    QCOMPARE(cancel_spy.count(), 1);                // the single terminal event is the cancel
    QCOMPARE(cancel_spy.first().at(0).toInt(), 1);  // partial_hits carries the one hit found
    parser.close();
}

// ============================================================================
// Search With Null Parser
// ============================================================================

// A null parser is a caller error that must fail CLOSED in both configurations:
// search() asserts nothing, it surfaces the reason and still completes the run with
// zero hits so a caller waiting on searchComplete is never left hanging.
void TestEmailSearchWorker::searchWithNullPstParserFailsClosed() {
    EmailSearchWorker worker;
    QSignalSpy error_spy(&worker, &EmailSearchWorker::errorOccurred);
    QSignalSpy done_spy(&worker, &EmailSearchWorker::searchComplete);

    sak::EmailSearchCriteria criteria;
    criteria.query_text = QStringLiteral("test");

    worker.search(nullptr, criteria);

    QCOMPARE(error_spy.count(), 1);
    QCOMPARE(error_spy.first().at(0).toString(), QStringLiteral("No PST/OST file open for search"));
    QCOMPARE(done_spy.count(), 1);
    QCOMPARE(done_spy.first().at(0).toInt(), 0);
}

void TestEmailSearchWorker::searchWithNullMboxParserFailsClosed() {
    EmailSearchWorker worker;
    QSignalSpy error_spy(&worker, &EmailSearchWorker::errorOccurred);
    QSignalSpy done_spy(&worker, &EmailSearchWorker::searchComplete);

    sak::EmailSearchCriteria criteria;
    criteria.query_text = QStringLiteral("test");

    worker.searchMbox(nullptr, criteria);

    QCOMPARE(error_spy.count(), 1);
    QCOMPARE(error_spy.first().at(0).toString(), QStringLiteral("No MBOX file open for search"));
    QCOMPARE(done_spy.count(), 1);
    QCOMPARE(done_spy.first().at(0).toInt(), 0);

    // Second arm of the SAME guard (email_search_worker.cpp:200): a non-null parser that was
    // never opened. Only the nullptr arm was exercised, so narrowing the guard to
    // `parser == nullptr` stayed green while a closed parser fell through to readMessages(),
    // which fails invalid_operation and reports the WRONG reason ("Failed to read MBOX
    // messages") instead of the fail-closed "no file open".
    MboxParser closed_parser;
    QVERIFY(!closed_parser.isOpen());
    EmailSearchWorker closed_worker;
    QSignalSpy closed_error_spy(&closed_worker, &EmailSearchWorker::errorOccurred);
    QSignalSpy closed_done_spy(&closed_worker, &EmailSearchWorker::searchComplete);
    closed_worker.searchMbox(&closed_parser, criteria);
    QCOMPARE(closed_error_spy.count(), 1);
    QCOMPARE(closed_error_spy.first().at(0).toString(),
             QStringLiteral("No MBOX file open for search"));
    QCOMPARE(closed_done_spy.count(), 1);
    QCOMPARE(closed_done_spy.first().at(0).toInt(), 0);

    QCOMPARE(error_spy.count(), 1);
    QCOMPARE(error_spy.first().at(0).toString(), QStringLiteral("No MBOX file open for search"));
    QCOMPARE(done_spy.count(), 1);
    QCOMPARE(done_spy.first().at(0).toInt(), 0);
}

// ============================================================================
// Search Hit Structure
// ============================================================================

void TestEmailSearchWorker::searchHitDefaults() {
    sak::EmailSearchHit hit;
    QCOMPARE(hit.item_node_id, static_cast<uint64_t>(0));
    QCOMPARE(hit.item_type, sak::EmailItemType::Unknown);
    QVERIFY(hit.subject.isEmpty());
    QVERIFY(hit.sender.isEmpty());
    QVERIFY(!hit.date.isValid());
    QVERIFY(hit.context_snippet.isEmpty());
    QVERIFY(hit.match_field.isEmpty());
    QVERIFY(hit.folder_path.isEmpty());
}

void TestEmailSearchWorker::searchHitFields() {
    sak::EmailSearchHit hit;
    hit.item_node_id = 42;
    hit.item_type = sak::EmailItemType::Email;
    hit.subject = QStringLiteral("Budget Report");
    hit.sender = QStringLiteral("boss@example.com");
    hit.date = QDateTime::currentDateTime();
    hit.context_snippet = QStringLiteral("...the budget for Q1...");
    hit.match_field = QStringLiteral("body");
    hit.folder_path = QStringLiteral("Inbox/Work");

    QCOMPARE(hit.item_node_id, static_cast<uint64_t>(42));
    QCOMPARE(hit.item_type, sak::EmailItemType::Email);
    QCOMPARE(hit.subject, QStringLiteral("Budget Report"));
    // sender/date/context_snippet were assigned and never read back (FINDING N8). They are the
    // three columns the search results list renders beside the subject, so a cross-wired
    // assignment would show the wrong correspondent or the wrong excerpt for a real hit.
    QCOMPARE(hit.sender, QStringLiteral("boss@example.com"));
    QVERIFY(hit.date.isValid());
    QCOMPARE(hit.context_snippet, QStringLiteral("...the budget for Q1..."));
    QCOMPARE(hit.match_field, QStringLiteral("body"));
    QCOMPARE(hit.folder_path, QStringLiteral("Inbox/Work"));
}

// A body-only search must map its hit to the message whose body actually matched,
// keyed on msg.message_index (the same value reported as item_node_id), not the
// loop position (B7-28).
void TestEmailSearchWorker::mboxBodySearchUsesMessageIndex() {
    QTemporaryFile mbox;
    QVERIFY(mbox.open());
    QByteArray content;
    // Message 0: keyword NOT present in body.
    content += "From a@example.com Mon Jan  1 00:00:00 2024\r\n";
    content += "From: A <a@example.com>\r\n";
    content += "Subject: First\r\n";
    content += "\r\n";
    content += "Ordinary body text.\r\n";
    content += "\r\n";
    // Message 1: the unique keyword lives only in THIS body.
    content += "From b@example.com Tue Jan  2 00:00:00 2024\r\n";
    content += "From: B <b@example.com>\r\n";
    content += "Subject: Second\r\n";
    content += "\r\n";
    content += "This body contains ZEBRACODE only.\r\n";
    mbox.write(content);
    mbox.close();

    MboxParser parser;
    parser.open(mbox.fileName());
    QVERIFY(parser.isOpen());
    parser.indexMessages();
    QCOMPARE(parser.messageCount(), 2);

    sak::EmailSearchCriteria criteria;
    criteria.query_text = QStringLiteral("ZEBRACODE");
    criteria.search_subject = false;
    criteria.search_sender = false;
    criteria.search_body = true;

    EmailSearchWorker worker;
    QSignalSpy hit_spy(&worker, &EmailSearchWorker::searchHit);
    QSignalSpy done_spy(&worker, &EmailSearchWorker::searchComplete);
    worker.searchMbox(&parser, criteria);

    QCOMPARE(done_spy.count(), 1);
    QCOMPARE(hit_spy.count(), 1);  // exactly the one message whose body matched
    const auto hit = hit_spy.first().first().value<sak::EmailSearchHit>();
    QCOMPARE(hit.item_node_id, static_cast<uint64_t>(1));  // the SECOND message
    QCOMPARE(hit.match_field, QStringLiteral("body"));
    // The snippet is cut from the SECOND message's body, so it also proves WHICH body was
    // loaded -- "reports body" alone says nothing about the text shown to the user. The body is
    // 36 chars, well inside the 120-char window, so extractContextSnippet returns it whole with
    // no ellipsis on either end (email_search_worker.cpp:354-368).
    QCOMPARE(hit.context_snippet, QStringLiteral("This body contains ZEBRACODE only.\r\n"));

    // search_body must GATE the matcher (:586-588): re-run with it off while another
    // detail-based criterion keeps the detail load alive, so matchMboxBody IS reached with the
    // flag clear and ZEBRACODE still in the body. It must refuse (the fixture has no recipient
    // header, so matchMboxRecipients finds nothing either).
    criteria.search_body = false;
    criteria.search_recipients = true;
    EmailSearchWorker gated_worker;
    QSignalSpy gated_hit_spy(&gated_worker, &EmailSearchWorker::searchHit);
    QSignalSpy gated_done_spy(&gated_worker, &EmailSearchWorker::searchComplete);
    gated_worker.searchMbox(&parser, criteria);
    QCOMPARE(gated_done_spy.count(), 1);
    QCOMPARE(gated_hit_spy.count(), 0);
    parser.close();
}

// A recipient-only search must evaluate the To/Cc/Bcc fields on the MBOX path (these
// were never checked before -- B7-34), reporting a "recipient" hit.
void TestEmailSearchWorker::mboxRecipientSearchMatchesToField() {
    QTemporaryFile mbox;
    QVERIFY(mbox.open());
    QByteArray content;
    content += "From a@example.com Mon Jan  1 00:00:00 2024\r\n";
    content += "From: A <a@example.com>\r\n";
    content += "To: Zebra Recipient <zebracontact@example.com>\r\n";
    content += "Subject: Ordinary\r\n";
    content += "\r\n";
    content += "Plain body without the keyword.\r\n";
    mbox.write(content);
    mbox.close();

    MboxParser parser;
    parser.open(mbox.fileName());
    QVERIFY(parser.isOpen());
    parser.indexMessages();

    sak::EmailSearchCriteria criteria;
    criteria.query_text = QStringLiteral("zebracontact");
    criteria.search_subject = false;
    criteria.search_sender = false;
    criteria.search_body = false;
    criteria.search_recipients = true;

    EmailSearchWorker worker;
    QSignalSpy hit_spy(&worker, &EmailSearchWorker::searchHit);
    QSignalSpy done_spy(&worker, &EmailSearchWorker::searchComplete);
    worker.searchMbox(&parser, criteria);

    QCOMPARE(done_spy.count(), 1);
    QCOMPARE(hit_spy.count(), 1);
    const auto hit = hit_spy.first().first().value<sak::EmailSearchHit>();
    QCOMPARE(hit.match_field, QStringLiteral("recipient"));
    // The context IS the To value (email_search_worker.cpp:606 -> detail.to). "recipient"
    // alone says nothing about which string the technician is actually shown.
    QCOMPARE(hit.context_snippet, QStringLiteral("Zebra Recipient <zebracontact@example.com>"));

    // search_recipients must GATE the matcher (:600-602), not merely select it: re-run with it
    // off while search_body keeps the detail load alive, so matchMboxRecipients IS reached with
    // the flag clear and the To header still matching. It must refuse (the body lacks the query).
    criteria.search_recipients = false;
    criteria.search_body = true;
    EmailSearchWorker gated_worker;
    QSignalSpy gated_hit_spy(&gated_worker, &EmailSearchWorker::searchHit);
    QSignalSpy gated_done_spy(&gated_worker, &EmailSearchWorker::searchComplete);
    gated_worker.searchMbox(&parser, criteria);
    QCOMPARE(gated_done_spy.count(), 1);
    QCOMPARE(gated_hit_spy.count(), 0);
    parser.close();
}

// An attachment-name-only search must evaluate attachment filenames on the MBOX path
// (never checked before -- B7-34), reporting an "attachment" hit.
void TestEmailSearchWorker::mboxAttachmentNameSearchMatches() {
    QTemporaryFile mbox;
    QVERIFY(mbox.open());
    QByteArray content;
    content += "From a@example.com Mon Jan  1 00:00:00 2024\r\n";
    content += "From: A <a@example.com>\r\n";
    content += "To: B <b@example.com>\r\n";
    content += "Subject: Ordinary\r\n";
    content += "MIME-Version: 1.0\r\n";
    content += "Content-Type: multipart/mixed; boundary=\"BOUND\"\r\n";
    content += "\r\n";
    content += "--BOUND\r\n";
    content += "Content-Type: text/plain; charset=UTF-8\r\n";
    content += "\r\n";
    content += "Body text.\r\n";
    content += "--BOUND\r\n";
    content += "Content-Type: application/octet-stream; name=\"zebrafile.bin\"\r\n";
    content += "Content-Transfer-Encoding: base64\r\n";
    content += "Content-Disposition: attachment; filename=\"zebrafile.bin\"\r\n";
    content += "\r\n";
    content += "SGVsbG8gQXR0YWNo\r\n";
    content += "--BOUND--\r\n";
    mbox.write(content);
    mbox.close();

    MboxParser parser;
    parser.open(mbox.fileName());
    QVERIFY(parser.isOpen());
    parser.indexMessages();

    sak::EmailSearchCriteria criteria;
    criteria.query_text = QStringLiteral("zebrafile");
    criteria.search_subject = false;
    criteria.search_sender = false;
    criteria.search_body = false;
    criteria.search_attachment_names = true;

    EmailSearchWorker worker;
    QSignalSpy hit_spy(&worker, &EmailSearchWorker::searchHit);
    QSignalSpy done_spy(&worker, &EmailSearchWorker::searchComplete);
    worker.searchMbox(&parser, criteria);

    QCOMPARE(done_spy.count(), 1);
    QCOMPARE(hit_spy.count(), 1);
    const auto hit = hit_spy.first().first().value<sak::EmailSearchHit>();
    QCOMPARE(hit.match_field, QStringLiteral("attachment"));
    // The context is the decoded FILENAME (email_search_worker.cpp:618-620), not the part's
    // mime type: an "attachment" label alone says nothing about which string is shown.
    QCOMPARE(hit.context_snippet, QStringLiteral("zebrafile.bin"));

    // search_attachment_names must GATE the matcher (:614): re-run with it off while search_body
    // keeps the detail load alive, so matchMboxAttachments IS reached with the flag clear and the
    // attachment still named zebrafile.bin. It must refuse (the body lacks the keyword).
    criteria.search_attachment_names = false;
    criteria.search_body = true;
    EmailSearchWorker gated_worker;
    QSignalSpy gated_hit_spy(&gated_worker, &EmailSearchWorker::searchHit);
    QSignalSpy gated_done_spy(&gated_worker, &EmailSearchWorker::searchComplete);
    gated_worker.searchMbox(&parser, criteria);
    QCOMPARE(gated_done_spy.count(), 1);
    QCOMPARE(gated_hit_spy.count(), 0);
    parser.close();
}

void TestEmailSearchWorker::plainSearchIsUnchangedWhenNoFilterIsSet() {
    QVERIFY(!sak::EmailSearchCriteria{}.hasFilter());
    QTemporaryDir dir;
    PstParser parser;
    parser.open(writeTemp(filterStore(), QStringLiteral(".pst"), dir));
    QVERIFY(parser.isOpen());
    // Everything with the word in it, dated or not, whatever it is.
    QCOMPARE(hitSubjects(parser, needle()).size(), 11);
    // No text and no filter still finds nothing.
    QVERIFY(hitSubjects(parser, {}).isEmpty());
}

void TestEmailSearchWorker::itemTypeFilterGroupsAsTheWindowsDo() {
    QTemporaryDir dir;
    PstParser parser;
    parser.open(writeTemp(filterStore(), QStringLiteral(".pst"), dir));
    const auto of_type = [&parser](sak::EmailItemType type) {
        sak::EmailSearchCriteria criteria = needle();
        criteria.item_type_filter = type;
        return hitSubjects(parser, criteria);
    };
    QCOMPARE(of_type(sak::EmailItemType::Email),
             QStringList({QStringLiteral("April mail with file"),
                          QStringLiteral("March mail"),
                          QStringLiteral("May mail"),
                          QStringLiteral("Undated draft")}));
    QCOMPARE(of_type(sak::EmailItemType::Calendar),
             QStringList({QStringLiteral("Event"), QStringLiteral("Invite")}));
    QCOMPARE(of_type(sak::EmailItemType::Contact),
             QStringList({QStringLiteral("Group"), QStringLiteral("Person")}));
    QCOMPARE(of_type(sak::EmailItemType::Task), QStringList{QStringLiteral("Job")});
    QCOMPARE(of_type(sak::EmailItemType::StickyNote), QStringList{QStringLiteral("Note")});
    QCOMPARE(of_type(sak::EmailItemType::JournalEntry), QStringList{QStringLiteral("Log")});
}

void TestEmailSearchWorker::hasAttachmentFilterNarrows() {
    QTemporaryDir dir;
    PstParser parser;
    parser.open(writeTemp(filterStore(), QStringLiteral(".pst"), dir));
    sak::EmailSearchCriteria criteria = needle();
    criteria.has_attachment_only = true;
    QCOMPARE(hitSubjects(parser, criteria), QStringList{QStringLiteral("April mail with file")});
}

void TestEmailSearchWorker::dateRangeCoversBothEndsAndLeavesOutTheUndated() {
    QTemporaryDir dir;
    PstParser parser;
    parser.open(writeTemp(filterStore(), QStringLiteral(".pst"), dir));
    sak::EmailSearchCriteria criteria = needle();
    criteria.item_type_filter = sak::EmailItemType::Email;
    criteria.date_from = dayOf(3, 10);
    criteria.date_to = dayOf(4, 10);
    QCOMPARE(hitSubjects(parser, criteria),
             QStringList({QStringLiteral("April mail with file"), QStringLiteral("March mail")}));

    // One end alone: an item with no date cannot be shown to be in it, whichever end it is.
    criteria.date_from = {};
    criteria.date_to = dayOf(4, 10);
    QVERIFY(!hitSubjects(parser, criteria).contains(QStringLiteral("Undated draft")));
    criteria.date_to = {};
    criteria.date_from = dayOf(4, 10);
    QVERIFY(!hitSubjects(parser, criteria).contains(QStringLiteral("Undated draft")));
}

// A meeting is found by the day it happens, not by the day its invitation landed.
void TestEmailSearchWorker::anEventIsInARangeByWhenItHappens() {
    QTemporaryDir dir;
    PstParser parser;
    parser.open(writeTemp(filterStore(), QStringLiteral(".pst"), dir));
    sak::EmailSearchCriteria criteria = needle();
    criteria.item_type_filter = sak::EmailItemType::Calendar;
    criteria.date_from = dayOf(4, 1);
    criteria.date_to = dayOf(4, 30);
    QCOMPARE(hitSubjects(parser, criteria), QStringList{QStringLiteral("Event")});

    // And the hit shows the date it was found by.
    EmailSearchWorker worker;
    QVector<sak::EmailSearchHit> hits;
    QObject::connect(&worker,
                     &EmailSearchWorker::searchHit,
                     &worker,
                     [&hits](const sak::EmailSearchHit& hit) { hits.append(hit); });
    worker.search(&parser, criteria);
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.first().date, dayOf(4, 20));
}

void TestEmailSearchWorker::filtersAloneListWhatTheyMatch() {
    QTemporaryDir dir;
    PstParser parser;
    parser.open(writeTemp(filterStore(), QStringLiteral(".pst"), dir));
    sak::EmailSearchCriteria criteria;
    criteria.item_type_filter = sak::EmailItemType::Task;
    QCOMPARE(hitSubjects(parser, criteria), QStringList{QStringLiteral("Job")});
}

void TestEmailSearchWorker::aFilteredSearchIsASubsetOfThePlainOne() {
    QTemporaryDir dir;
    PstParser parser;
    parser.open(writeTemp(filterStore(), QStringLiteral(".pst"), dir));
    const QStringList plain = hitSubjects(parser, needle());
    for (const auto type : {sak::EmailItemType::Email,
                            sak::EmailItemType::Calendar,
                            sak::EmailItemType::Contact,
                            sak::EmailItemType::Task,
                            sak::EmailItemType::StickyNote,
                            sak::EmailItemType::JournalEntry}) {
        sak::EmailSearchCriteria criteria = needle();
        criteria.item_type_filter = type;
        for (const QString& subject : hitSubjects(parser, criteria)) {
            QVERIFY2(plain.contains(subject), qPrintable(subject));
        }
    }
}

void TestEmailSearchWorker::anMboxHasOnlyEmailToFind() {
    QTemporaryDir dir;
    MboxParser parser;
    parser.open(writeTemp(dateAttachedMailbox(), QStringLiteral(".mbox"), dir));
    QVERIFY(parser.isOpen());
    sak::EmailSearchCriteria criteria = needle();
    criteria.item_type_filter = sak::EmailItemType::Email;
    QCOMPARE(mboxHitSubjects(parser, criteria).size(), 5);
    criteria.item_type_filter = sak::EmailItemType::Calendar;
    QVERIFY(mboxHitSubjects(parser, criteria).isEmpty());
}

void TestEmailSearchWorker::mboxDateAndAttachmentFilters() {
    QTemporaryDir dir;
    MboxParser parser;
    parser.open(writeTemp(dateAttachedMailbox(), QStringLiteral(".mbox"), dir));
    sak::EmailSearchCriteria attached;
    attached.has_attachment_only = true;
    QCOMPARE(mboxHitSubjects(parser, attached),
             QStringList({QStringLiteral("April"), QStringLiteral("Inline file")}));

    sak::EmailSearchCriteria ranged = needle();
    ranged.date_from = dayOf(3, 1);
    ranged.date_to = dayOf(4, 30);
    QCOMPARE(mboxHitSubjects(parser, ranged),
             QStringList({QStringLiteral("April"),
                          QStringLiteral("Inline file"),
                          QStringLiteral("March")}));
}

// The filter narrows first and the limit counts what is left, so a filter reaches an item that a
// plain search would have stopped short of.
void TestEmailSearchWorker::theResultLimitCountsWhatIsLeftAfterFiltering() {
    QByteArray box;
    const int plain_messages = sak::email::kMaxSearchResults + 5;
    box.reserve(static_cast<qsizetype>(plain_messages) * 80);
    for (int i = 0; i < plain_messages; ++i) {
        box += "From a@example.test Mon Jan  1 00:00:00 2024\n";
        box += "From: a@example.test\nSubject: needle plain\n\nx\n\n";
    }
    box += "From a@example.test Mon Jan  1 00:00:00 2024\n";
    box += "From: a@example.test\nSubject: needle last\n";
    box += "MIME-Version: 1.0\nContent-Type: multipart/mixed; boundary=\"B\"\n\n";
    box += "--B\nContent-Type: text/plain\n\nx\n";
    box += "--B\nContent-Type: application/pdf; name=\"f.pdf\"\n";
    box += "Content-Disposition: attachment; filename=\"f.pdf\"\n\nX\n--B--\n\n";

    QTemporaryDir dir;
    MboxParser parser;
    parser.open(writeTemp(box, QStringLiteral(".mbox"), dir));
    QVERIFY(parser.isOpen());
    sak::EmailSearchCriteria plain;
    plain.query_text = QStringLiteral("needle");
    plain.search_body = false;
    const QStringList capped = mboxHitSubjects(parser, plain);
    QCOMPARE(capped.size(), sak::email::kMaxSearchResults);
    QVERIFY(!capped.contains(QStringLiteral("needle last")));

    sak::EmailSearchCriteria filtered = plain;
    filtered.has_attachment_only = true;
    QCOMPARE(mboxHitSubjects(parser, filtered), QStringList{QStringLiteral("needle last")});
}

// The same on a PST, whose search walks folders and not a file: the limit stops a plain search
// short of the last item, and a filter reaches it.
void TestEmailSearchWorker::theResultLimitCountsWhatIsLeftInAPstToo() {
    constexpr int kMessagesPerFolder = 250;
    QVector<BuilderFolder> folders;
    for (int left = sak::email::kMaxSearchResults + 5; left > 0; left -= kMessagesPerFolder) {
        BuilderFolder folder;
        folder.name = QStringLiteral("Folder %1").arg(folders.size());
        for (int i = 0; i < std::min(left, kMessagesPerFolder); ++i) {
            folder.messages.append(filterMail(QStringLiteral("needle plain"), dayOf(3, 1)));
        }
        folders.append(folder);
    }
    folders.last().messages.append(filterMail(QStringLiteral("needle last"), dayOf(3, 2), true));

    QTemporaryDir dir;
    PstParser parser;
    parser.open(writeTemp(PstStoreBuilder{}.build(folders), QStringLiteral(".pst"), dir));
    QVERIFY(parser.isOpen());
    sak::EmailSearchCriteria plain;
    plain.query_text = QStringLiteral("needle");
    plain.search_body = false;
    const QStringList capped = hitSubjects(parser, plain);
    QCOMPARE(capped.size(), sak::email::kMaxSearchResults);
    QVERIFY(!capped.contains(QStringLiteral("needle last")));

    sak::EmailSearchCriteria filtered = plain;
    filtered.has_attachment_only = true;
    QCOMPARE(hitSubjects(parser, filtered), QStringList{QStringLiteral("needle last")});
}

void TestEmailSearchWorker::aDateRangeFindsAnAllDayEventOnItsOwnDate_data() {
    QTest::addColumn<QDateTime>("stored");
    QTest::newRow("written in India")
        << QDateTime(QDate(2026, 3, 14), QTime(18, 30), QTimeZone::UTC);
    QTest::newRow("written in Pacific time")
        << QDateTime(QDate(2026, 3, 15), QTime(8, 0), QTimeZone::UTC);
}

// The range is one day on this PC's calendar, and the event is on its own date.
void TestEmailSearchWorker::aDateRangeFindsAnAllDayEventOnItsOwnDate() {
    QFETCH(QDateTime, stored);
    BuilderMessage event =
        ofClass(QStringLiteral("IPM.Appointment"), QStringLiteral("Holiday"), {});
    event.named_props = {{0x02,
                          sak::pst::kLidAppointmentStartWhole,
                          sak::email::kPropTypeSysTime,
                          builderFiletime(stored)},
                         {0x02,
                          sak::pst::kLidAppointmentSubType,
                          sak::email::kPropTypeBoolean,
                          QByteArray(1, char{1})}};
    BuilderFolder calendar;
    calendar.name = QStringLiteral("Calendar");
    calendar.container_class = QStringLiteral("IPF.Appointment");
    calendar.messages = {event};

    QTemporaryDir dir;
    PstParser parser;
    parser.open(writeTemp(PstStoreBuilder{}.build({calendar}), QStringLiteral(".pst"), dir));
    QVERIFY(parser.isOpen());
    sak::EmailSearchCriteria on_the_day;
    on_the_day.date_from = QDate(2026, 3, 15).startOfDay();
    on_the_day.date_to = QDate(2026, 3, 15).endOfDay();
    QCOMPARE(hitSubjects(parser, on_the_day), QStringList{QStringLiteral("Holiday")});
    sak::EmailSearchCriteria the_day_before;
    the_day_before.date_from = QDate(2026, 3, 14).startOfDay();
    the_day_before.date_to = QDate(2026, 3, 14).endOfDay();
    QVERIFY(hitSubjects(parser, the_day_before).isEmpty());
}

void TestEmailSearchWorker::aSearchFindsTheRealTextOfAnEncodedHeader() {
    QByteArray box;
    box += "From a@example.test Mon Jan  1 00:00:00 2024\n";
    box += "From: =?UTF-8?B?Q2Fmw6k=?= <a@example.test>\n";
    box += "Subject: =?UTF-8?Q?Caf?=\n =?UTF-8?Q?=C3=A9_menu?=\n\nx\n\n";
    box += "From b@example.test Mon Jan  1 00:00:00 2024\n";
    box += "From: b@example.test\nSubject: Something else\n\nx\n\n";
    QTemporaryDir dir;
    MboxParser parser;
    parser.open(writeTemp(box, QStringLiteral(".mbox"), dir));
    QVERIFY(parser.isOpen());
    sak::EmailSearchCriteria by_subject;
    by_subject.query_text = QStringLiteral("Caf\u00E9 menu");
    by_subject.search_body = false;
    QCOMPARE(mboxHitSubjects(parser, by_subject), QStringList{QStringLiteral("Caf\u00E9 menu")});
    sak::EmailSearchCriteria by_sender;
    by_sender.query_text = QStringLiteral("Caf\u00E9 <a@");
    by_sender.search_body = false;
    QCOMPARE(mboxHitSubjects(parser, by_sender), QStringList{QStringLiteral("Caf\u00E9 menu")});
    // What the file really says is no longer what is searched.
    sak::EmailSearchCriteria raw;
    raw.query_text = QStringLiteral("=?UTF-8?");
    raw.search_body = false;
    QVERIFY(mboxHitSubjects(parser, raw).isEmpty());
}

QTEST_MAIN(TestEmailSearchWorker)
#include "test_email_search_worker.moc"

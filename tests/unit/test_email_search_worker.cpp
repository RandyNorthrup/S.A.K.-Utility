// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_email_search_worker.cpp
/// @brief Unit tests for the email search worker

#include "sak/email_constants.h"
#include "sak/email_search_worker.h"
#include "sak/email_types.h"
#include "sak/mbox_parser.h"

#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTimeZone>
#include <QtTest/QtTest>

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
};

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

    QCOMPARE(criteria.mapi_property_id, sak::email::kPropIdSubject);
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
    worker.searchMbox(&parser, criteria);

    QCOMPARE(done_spy.count(), 1);
    QCOMPARE(hit_spy.count(), 1);
    QCOMPARE(done_spy.first().at(0).toInt(), 1);  // the run was not cut short
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
    QVERIFY(error_spy.first().at(0).toString().contains(QStringLiteral("No PST/OST file open")));
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
    QVERIFY(error_spy.first().at(0).toString().contains(QStringLiteral("No MBOX file open")));
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
    parser.close();
}

QTEST_MAIN(TestEmailSearchWorker)
#include "test_email_search_worker.moc"

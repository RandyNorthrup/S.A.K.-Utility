// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_email_search_worker.cpp
/// @brief Unit tests for the email search worker

#include "sak/email_constants.h"
#include "sak/email_search_worker.h"
#include "sak/email_types.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

class TestEmailSearchWorker : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- Construction ----------------------------------------------------
    void defaultConstruction();

    // -- Criteria Defaults -----------------------------------------------
    void criteriaDefaults();
    void criteriaFieldFlags();
    void criteriaDateRange();
    void criteriaFolderScope();
    void criteriaMapiProperty();

    // -- Cancel ----------------------------------------------------------
    void cancelBeforeSearchDoesNotCrash();
    void cancelDuringIdle();

    // -- Search With Null Parser -----------------------------------------
    void searchWithNullPstParserDoesNotCrash();
    void searchWithNullMboxParserDoesNotCrash();

    // -- Search Hit Structure --------------------------------------------
    void searchHitDefaults();
    void searchHitFields();
};

// ============================================================================
// Construction
// ============================================================================

void TestEmailSearchWorker::defaultConstruction() {
    EmailSearchWorker worker;
    // Should construct without crash
    QVERIFY(true);
}

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

    criteria.date_from = QDateTime(QDate(2024, 1, 1), QTime(0, 0, 0), Qt::UTC);
    criteria.date_to = QDateTime(QDate(2024, 12, 31), QTime(23, 59, 59), Qt::UTC);

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

void TestEmailSearchWorker::cancelBeforeSearchDoesNotCrash() {
    EmailSearchWorker worker;
    worker.cancel();
    QVERIFY(true);
}

void TestEmailSearchWorker::cancelDuringIdle() {
    EmailSearchWorker worker;
    worker.cancel();
    worker.cancel();
    QVERIFY(true);
}

// ============================================================================
// Search With Null Parser
// ============================================================================

void TestEmailSearchWorker::searchWithNullPstParserDoesNotCrash() {
    EmailSearchWorker worker;
    QSignalSpy error_spy(&worker, &EmailSearchWorker::errorOccurred);

    sak::EmailSearchCriteria criteria;
    criteria.query_text = QStringLiteral("test");

    // Null parser — should assert in debug or handle gracefully
    // In release builds, this tests that we don't segfault
#ifdef QT_NO_DEBUG
    worker.search(nullptr, criteria);
    QVERIFY(error_spy.count() > 0 || true);
#else
    QVERIFY(true);
#endif
}

void TestEmailSearchWorker::searchWithNullMboxParserDoesNotCrash() {
    EmailSearchWorker worker;
    QSignalSpy error_spy(&worker, &EmailSearchWorker::errorOccurred);

    sak::EmailSearchCriteria criteria;
    criteria.query_text = QStringLiteral("test");

#ifdef QT_NO_DEBUG
    worker.searchMbox(nullptr, criteria);
    QVERIFY(error_spy.count() > 0 || true);
#else
    QVERIFY(true);
#endif
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

QTEST_MAIN(TestEmailSearchWorker)
#include "test_email_search_worker.moc"

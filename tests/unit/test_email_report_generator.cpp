// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_email_report_generator.cpp
/// @brief Unit tests for the email report generator

#include "sak/email_report_generator.h"
#include "sak/email_types.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest/QtTest>

class TestEmailReportGenerator : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- Construction ----------------------------------------------------
    void defaultConstruction();

    // -- ReportData Defaults ---------------------------------------------
    void reportDataDefaults();
    void reportDataPopulation();

    // -- HTML Report -----------------------------------------------------
    void htmlReportContainsDoctype();
    void htmlReportContainsTitle();
    void htmlReportIncludesMetadata();
    void htmlReportIncludesFileInfo();
    void htmlReportIncludesStatistics();
    void htmlReportIncludesFolderTree();
    void htmlReportIncludesExportResults();
    void htmlReportIncludesFooter();
    void htmlReportEmptyDataProducesValidHtml();

    // -- JSON Report -----------------------------------------------------
    void jsonReportIsValidJson();
    void jsonReportContainsMetadata();
    void jsonReportContainsStatistics();
    void jsonReportContainsFileInfo();
    void jsonReportContainsFolderTree();
    void jsonReportEmptyDataProducesValidJson();

    // -- CSV Report ------------------------------------------------------
    void csvReportContainsHeader();
    void csvReportContainsMetrics();
    void csvReportEmptyDataProducesValidCsv();

private:
    EmailReportGenerator::ReportData createSampleData();
};

// ============================================================================
// Construction
// ============================================================================

void TestEmailReportGenerator::defaultConstruction() {
    EmailReportGenerator generator;
    QVERIFY(true);
}

// ============================================================================
// ReportData Defaults
// ============================================================================

void TestEmailReportGenerator::reportDataDefaults() {
    EmailReportGenerator::ReportData data;
    QVERIFY(data.technician_name.isEmpty());
    QVERIFY(data.ticket_number.isEmpty());
    QVERIFY(data.customer_name.isEmpty());
    QVERIFY(!data.report_date.isValid());
    QCOMPARE(data.total_emails, 0);
    QCOMPARE(data.total_contacts, 0);
    QCOMPARE(data.total_calendar_items, 0);
    QCOMPARE(data.total_tasks, 0);
    QCOMPARE(data.total_notes, 0);
    QCOMPARE(data.total_attachments, 0);
    QCOMPARE(data.total_attachment_bytes, static_cast<qint64>(0));
    QVERIFY(data.export_results.isEmpty());
    QCOMPARE(data.searches_performed, 0);
    QCOMPARE(data.total_search_hits, 0);
    QVERIFY(data.discovered_profiles.isEmpty());
}

void TestEmailReportGenerator::reportDataPopulation() {
    auto data = createSampleData();
    QCOMPARE(data.technician_name, QStringLiteral("John Doe"));
    QCOMPARE(data.total_emails, 5000);
    QCOMPARE(data.total_contacts, 150);
    QVERIFY(!data.folder_tree.isEmpty());
}

// ============================================================================
// HTML Report
// ============================================================================

void TestEmailReportGenerator::htmlReportContainsDoctype() {
    EmailReportGenerator generator;
    auto data = createSampleData();
    QString html = generator.generateHtml(data);

    QVERIFY(html.contains(QStringLiteral("<!DOCTYPE html>")));
    QVERIFY(html.contains(QStringLiteral("<html")));
    QVERIFY(html.contains(QStringLiteral("</html>")));
}

void TestEmailReportGenerator::htmlReportContainsTitle() {
    EmailReportGenerator generator;
    auto data = createSampleData();
    QString html = generator.generateHtml(data);

    QVERIFY(html.contains(QStringLiteral("Email Inspection Report")));
}

void TestEmailReportGenerator::htmlReportIncludesMetadata() {
    EmailReportGenerator generator;
    auto data = createSampleData();
    QString html = generator.generateHtml(data);

    QVERIFY(html.contains(data.technician_name.toHtmlEscaped()));
    QVERIFY(html.contains(data.customer_name.toHtmlEscaped()));
    QVERIFY(html.contains(data.ticket_number.toHtmlEscaped()));
}

void TestEmailReportGenerator::htmlReportIncludesFileInfo() {
    EmailReportGenerator generator;
    auto data = createSampleData();
    QString html = generator.generateHtml(data);

    QVERIFY(html.contains(data.file_info.file_path.toHtmlEscaped()));
    QVERIFY(html.contains(QStringLiteral("Unicode")));
}

void TestEmailReportGenerator::htmlReportIncludesStatistics() {
    EmailReportGenerator generator;
    auto data = createSampleData();
    QString html = generator.generateHtml(data);

    QVERIFY(html.contains(QStringLiteral("5000")));
    QVERIFY(html.contains(QStringLiteral("150")));
    QVERIFY(html.contains(QStringLiteral("Emails")));
    QVERIFY(html.contains(QStringLiteral("Contacts")));
}

void TestEmailReportGenerator::htmlReportIncludesFolderTree() {
    EmailReportGenerator generator;
    auto data = createSampleData();
    QString html = generator.generateHtml(data);

    QVERIFY(html.contains(QStringLiteral("Inbox")));
    QVERIFY(html.contains(QStringLiteral("Sent Items")));
}

void TestEmailReportGenerator::htmlReportIncludesExportResults() {
    EmailReportGenerator generator;
    auto data = createSampleData();
    QString html = generator.generateHtml(data);

    QVERIFY(html.contains(QStringLiteral("EML")));
}

void TestEmailReportGenerator::htmlReportIncludesFooter() {
    EmailReportGenerator generator;
    auto data = createSampleData();
    QString html = generator.generateHtml(data);

    QVERIFY(html.contains(QStringLiteral("S.A.K. Utility")));
}

void TestEmailReportGenerator::htmlReportEmptyDataProducesValidHtml() {
    EmailReportGenerator generator;
    EmailReportGenerator::ReportData empty_data;
    QString html = generator.generateHtml(empty_data);

    QVERIFY(!html.isEmpty());
    QVERIFY(html.contains(QStringLiteral("<!DOCTYPE html>")));
    QVERIFY(html.contains(QStringLiteral("</html>")));
}

// ============================================================================
// JSON Report
// ============================================================================

void TestEmailReportGenerator::jsonReportIsValidJson() {
    EmailReportGenerator generator;
    auto data = createSampleData();
    QByteArray json = generator.generateJson(data);

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(json, &error);
    QCOMPARE(error.error, QJsonParseError::NoError);
    QVERIFY(doc.isObject());
}

void TestEmailReportGenerator::jsonReportContainsMetadata() {
    EmailReportGenerator generator;
    auto data = createSampleData();
    QByteArray json = generator.generateJson(data);

    QJsonDocument doc = QJsonDocument::fromJson(json);
    QJsonObject root = doc.object();

    QVERIFY(root.contains(QStringLiteral("metadata")));
    QJsonObject metadata = root[QStringLiteral("metadata")].toObject();
    QCOMPARE(metadata[QStringLiteral("technician")].toString(), QStringLiteral("John Doe"));
    QCOMPARE(metadata[QStringLiteral("customer")].toString(), QStringLiteral("Acme Corp"));
    QCOMPARE(metadata[QStringLiteral("tool")].toString(), QStringLiteral("SAK Utility"));
}

void TestEmailReportGenerator::jsonReportContainsStatistics() {
    EmailReportGenerator generator;
    auto data = createSampleData();
    QByteArray json = generator.generateJson(data);

    QJsonDocument doc = QJsonDocument::fromJson(json);
    QJsonObject root = doc.object();

    QVERIFY(root.contains(QStringLiteral("statistics")));
    QJsonObject stats = root[QStringLiteral("statistics")].toObject();
    QCOMPARE(stats[QStringLiteral("emails")].toInt(), 5000);
    QCOMPARE(stats[QStringLiteral("contacts")].toInt(), 150);
    QCOMPARE(stats[QStringLiteral("calendar_items")].toInt(), 75);
    QCOMPARE(stats[QStringLiteral("tasks")].toInt(), 30);
    QCOMPARE(stats[QStringLiteral("notes")].toInt(), 10);
}

void TestEmailReportGenerator::jsonReportContainsFileInfo() {
    EmailReportGenerator generator;
    auto data = createSampleData();
    QByteArray json = generator.generateJson(data);

    QJsonDocument doc = QJsonDocument::fromJson(json);
    QJsonObject root = doc.object();

    QVERIFY(root.contains(QStringLiteral("file_info")));
    QJsonObject file_info = root[QStringLiteral("file_info")].toObject();
    QCOMPARE(file_info[QStringLiteral("is_unicode")].toBool(), true);
    QCOMPARE(file_info[QStringLiteral("is_ost")].toBool(), false);
}

void TestEmailReportGenerator::jsonReportContainsFolderTree() {
    EmailReportGenerator generator;
    auto data = createSampleData();
    QByteArray json = generator.generateJson(data);

    QJsonDocument doc = QJsonDocument::fromJson(json);
    QJsonObject root = doc.object();

    QVERIFY(root.contains(QStringLiteral("folder_tree")));
    QJsonArray folders = root[QStringLiteral("folder_tree")].toArray();
    QVERIFY(folders.size() >= 2);
}

void TestEmailReportGenerator::jsonReportEmptyDataProducesValidJson() {
    EmailReportGenerator generator;
    EmailReportGenerator::ReportData empty_data;
    QByteArray json = generator.generateJson(empty_data);

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(json, &error);
    QCOMPARE(error.error, QJsonParseError::NoError);
    QVERIFY(doc.isObject());
}

// ============================================================================
// CSV Report
// ============================================================================

void TestEmailReportGenerator::csvReportContainsHeader() {
    EmailReportGenerator generator;
    auto data = createSampleData();
    QString csv = generator.generateCsv(data);

    QVERIFY(csv.contains(QStringLiteral("Email Inspection Report Summary")));
    QVERIFY(csv.contains(QStringLiteral("\"Metric\",\"Value\"")));
}

void TestEmailReportGenerator::csvReportContainsMetrics() {
    EmailReportGenerator generator;
    auto data = createSampleData();
    QString csv = generator.generateCsv(data);

    QVERIFY(csv.contains(QStringLiteral("Total Emails")));
    QVERIFY(csv.contains(QStringLiteral("5000")));
    QVERIFY(csv.contains(QStringLiteral("Total Contacts")));
    QVERIFY(csv.contains(QStringLiteral("150")));
}

void TestEmailReportGenerator::csvReportEmptyDataProducesValidCsv() {
    EmailReportGenerator generator;
    EmailReportGenerator::ReportData empty_data;
    QString csv = generator.generateCsv(empty_data);

    QVERIFY(!csv.isEmpty());
    QVERIFY(csv.contains(QStringLiteral("Metric")));
}

// ============================================================================
// Helpers
// ============================================================================

EmailReportGenerator::ReportData TestEmailReportGenerator::createSampleData() {
    EmailReportGenerator::ReportData data;

    // Metadata
    data.technician_name = QStringLiteral("John Doe");
    data.ticket_number = QStringLiteral("TKT-2024-0042");
    data.customer_name = QStringLiteral("Acme Corp");
    data.report_date = QDateTime::currentDateTime();

    // File info
    data.file_info.file_path = QStringLiteral("C:/Users/Test/Documents/Outlook.pst");
    data.file_info.display_name = QStringLiteral("Personal Folders");
    data.file_info.file_size_bytes = 1024LL * 1024 * 500;
    data.file_info.is_unicode = true;
    data.file_info.is_ost = false;
    data.file_info.encryption_type = 0;
    data.file_info.total_folders = 25;
    data.file_info.total_items = 5265;

    // Statistics
    data.total_emails = 5000;
    data.total_contacts = 150;
    data.total_calendar_items = 75;
    data.total_tasks = 30;
    data.total_notes = 10;
    data.total_attachments = 1200;
    data.total_attachment_bytes = 1024LL * 1024 * 350;

    // Folder tree
    sak::PstFolder inbox;
    inbox.node_id = 1;
    inbox.display_name = QStringLiteral("Inbox");
    inbox.content_count = 3500;
    inbox.container_class = QStringLiteral("IPF.Note");

    sak::PstFolder sent;
    sent.node_id = 2;
    sent.display_name = QStringLiteral("Sent Items");
    sent.content_count = 1200;
    sent.container_class = QStringLiteral("IPF.Note");

    sak::PstFolder contacts;
    contacts.node_id = 3;
    contacts.display_name = QStringLiteral("Contacts");
    contacts.content_count = 150;
    contacts.container_class = QStringLiteral("IPF.Contact");

    data.folder_tree = {inbox, sent, contacts};

    // Export results
    sak::EmailExportResult export_result;
    export_result.export_format = QStringLiteral("EML");
    export_result.export_path = QStringLiteral("C:/output/eml_export");
    export_result.items_exported = 500;
    export_result.items_failed = 2;
    export_result.total_bytes = 50 * 1024 * 1024;
    export_result.started = QDateTime::currentDateTime().addSecs(-120);
    export_result.finished = QDateTime::currentDateTime();
    data.export_results = {export_result};

    // Search stats
    data.searches_performed = 5;
    data.total_search_hits = 42;

    return data;
}

QTEST_MAIN(TestEmailReportGenerator)
#include "test_email_report_generator.moc"

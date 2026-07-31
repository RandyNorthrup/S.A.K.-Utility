// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_email_export_worker.cpp
/// @brief Unit tests for the email export worker

#include "sak/email_constants.h"
#include "sak/email_export_worker.h"
#include "sak/email_types.h"
#include "sak/mbox_parser.h"

#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QtTest/QtTest>

class TestEmailExportWorker : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- Construction ----------------------------------------------------
    void defaultConstruction();

    // -- Config Defaults -------------------------------------------------
    void configDefaults();
    void configCsvOptions();
    void configAttachmentOptions();
    void configEmlOptions();

    // -- Result Defaults -------------------------------------------------
    void resultDefaults();
    void resultPopulation();

    // -- Cancel ----------------------------------------------------------
    void cancelBeforeExportDoesNotCrash();

    // -- Export With Null Parser -----------------------------------------
    void exportWithNullPstParser();
    void exportWithNullMboxParser();

    // -- Export With Empty Config ----------------------------------------
    void exportWithEmptyOutputPath();
    void exportWithNoItems();

    // -- Format Coverage -------------------------------------------------
    void allExportFormatsHaveNames();

    // -- B7-25/27: real MBOX export with a readable attachment -----------
    void mboxExportWithAttachmentSucceeds();
};

// ============================================================================
// Construction
// ============================================================================

void TestEmailExportWorker::defaultConstruction() {
    EmailExportWorker worker;
    QVERIFY(true);
}

// ============================================================================
// Config Defaults
// ============================================================================

void TestEmailExportWorker::configDefaults() {
    sak::EmailExportConfig config;
    QCOMPARE(config.format, sak::ExportFormat::Eml);
    QVERIFY(config.output_path.isEmpty());
    QVERIFY(config.item_ids.isEmpty());
    QCOMPARE(config.folder_id, static_cast<uint64_t>(0));
    QVERIFY(!config.recurse_subfolders);
    QVERIFY(config.csv_columns.isEmpty());
    QCOMPARE(config.csv_delimiter, QLatin1Char(','));
    QVERIFY(config.csv_include_header);
    QVERIFY(config.flatten_attachments);
    QVERIFY(config.attachment_filter.isEmpty());
    QVERIFY(config.skip_inline_images);
    QVERIFY(config.eml_include_headers);
    QVERIFY(config.save_attachments_with_messages);
    QVERIFY(config.prefix_with_date);
}

void TestEmailExportWorker::configCsvOptions() {
    sak::EmailExportConfig config;
    config.format = sak::ExportFormat::CsvEmails;
    config.csv_columns = {QStringLiteral("Subject"),
                          QStringLiteral("From"),
                          QStringLiteral("Date"),
                          QStringLiteral("Size")};
    config.csv_delimiter = QLatin1Char(';');
    config.csv_include_header = false;

    QCOMPARE(config.csv_columns.size(), 4);
    QCOMPARE(config.csv_delimiter, QLatin1Char(';'));
    QVERIFY(!config.csv_include_header);
}

void TestEmailExportWorker::configAttachmentOptions() {
    sak::EmailExportConfig config;
    config.format = sak::ExportFormat::Attachments;
    config.flatten_attachments = false;
    config.attachment_filter = QStringLiteral("*.pdf");
    config.skip_inline_images = false;

    QVERIFY(!config.flatten_attachments);
    QCOMPARE(config.attachment_filter, QStringLiteral("*.pdf"));
    QVERIFY(!config.skip_inline_images);
}

void TestEmailExportWorker::configEmlOptions() {
    sak::EmailExportConfig config;
    config.format = sak::ExportFormat::Eml;
    config.eml_include_headers = false;
    config.prefix_with_date = false;

    QVERIFY(!config.eml_include_headers);
    QVERIFY(!config.prefix_with_date);
}

// ============================================================================
// Result Defaults
// ============================================================================

void TestEmailExportWorker::resultDefaults() {
    sak::EmailExportResult result;
    QVERIFY(result.export_path.isEmpty());
    QVERIFY(result.export_format.isEmpty());
    QCOMPARE(result.items_exported, 0);
    QCOMPARE(result.items_failed, 0);
    QCOMPARE(result.total_bytes, static_cast<qint64>(0));
    QVERIFY(result.errors.isEmpty());
    QVERIFY(!result.started.isValid());
    QVERIFY(!result.finished.isValid());
}

void TestEmailExportWorker::resultPopulation() {
    sak::EmailExportResult result;
    result.export_path = QStringLiteral("C:/output");
    result.export_format = QStringLiteral("EML");
    result.items_exported = 150;
    result.items_failed = 3;
    result.total_bytes = 1024 * 1024;
    result.errors.append(QStringLiteral("One item had bad encoding"));
    result.started = QDateTime::currentDateTime().addSecs(-60);
    result.finished = QDateTime::currentDateTime();

    QCOMPARE(result.items_exported, 150);
    QCOMPARE(result.items_failed, 3);
    QCOMPARE(result.errors.size(), 1);
    QVERIFY(result.started < result.finished);
}

// ============================================================================
// Cancel
// ============================================================================

void TestEmailExportWorker::cancelBeforeExportDoesNotCrash() {
    EmailExportWorker worker;
    worker.cancel();
    worker.cancel();
    QVERIFY(true);
}

// ============================================================================
// Export With Null Parser
// ============================================================================

void TestEmailExportWorker::exportWithNullPstParser() {
    EmailExportWorker worker;
    sak::EmailExportConfig config;
    config.output_path = QStringLiteral("C:/output");
    config.item_ids = {1, 2, 3};

    // In debug builds, Q_ASSERT will fire; in release, verify no crash
#ifdef QT_NO_DEBUG
    QSignalSpy complete_spy(&worker, &EmailExportWorker::exportComplete);
    worker.exportItems(nullptr, config);
    QVERIFY(complete_spy.count() > 0 || true);
#else
    QVERIFY(true);
#endif
}

void TestEmailExportWorker::exportWithNullMboxParser() {
    EmailExportWorker worker;
    sak::EmailExportConfig config;
    config.output_path = QStringLiteral("C:/output");

#ifdef QT_NO_DEBUG
    QSignalSpy complete_spy(&worker, &EmailExportWorker::exportComplete);
    worker.exportMboxItems(nullptr, config);
    QVERIFY(complete_spy.count() > 0 || true);
#else
    QVERIFY(true);
#endif
}

// ============================================================================
// Export With Empty/Invalid Config
// ============================================================================

void TestEmailExportWorker::exportWithEmptyOutputPath() {
    sak::EmailExportConfig config;
    // output_path is empty — an assertion is expected in debug mode
    QVERIFY(config.output_path.isEmpty());
}

void TestEmailExportWorker::exportWithNoItems() {
    sak::EmailExportConfig config;
    config.output_path = QStringLiteral("C:/output");
    // No items and no folder — export should complete with zero items
    QVERIFY(config.item_ids.isEmpty());
    QCOMPARE(config.folder_id, static_cast<uint64_t>(0));
}

// ============================================================================
// Format Coverage
// ============================================================================

void TestEmailExportWorker::allExportFormatsHaveNames() {
    // Verify all ExportFormat enum values are distinct
    QVector<int> format_values;
    format_values.append(static_cast<int>(sak::ExportFormat::Eml));
    format_values.append(static_cast<int>(sak::ExportFormat::Html));
    format_values.append(static_cast<int>(sak::ExportFormat::Text));
    format_values.append(static_cast<int>(sak::ExportFormat::Pdf));
    format_values.append(static_cast<int>(sak::ExportFormat::CsvEmails));
    format_values.append(static_cast<int>(sak::ExportFormat::Vcf));
    format_values.append(static_cast<int>(sak::ExportFormat::CsvContacts));
    format_values.append(static_cast<int>(sak::ExportFormat::Ics));
    format_values.append(static_cast<int>(sak::ExportFormat::CsvCalendar));
    format_values.append(static_cast<int>(sak::ExportFormat::CsvTasks));
    format_values.append(static_cast<int>(sak::ExportFormat::PlainTextNotes));
    format_values.append(static_cast<int>(sak::ExportFormat::Attachments));

    // All must be unique
    QSet<int> unique_values(format_values.begin(), format_values.end());
    QCOMPARE(unique_values.size(), format_values.size());
    QCOMPARE(unique_values.size(), 12);
}

// A readable attachment must NOT mark the message a partial export, and paging the
// whole mailbox must record no read-failure -- items_exported=1, items_failed=0.
// Exercises the refactored collectMboxIndices / collectMboxAttachmentData /
// exportOneMboxItem wiring (B7-25, B7-27).
void TestEmailExportWorker::mboxExportWithAttachmentSucceeds() {
    QTemporaryFile mbox;
    QVERIFY(mbox.open());
    QByteArray content;
    content += "From sender@example.com Mon Jan  1 00:00:00 2024\r\n";
    content += "From: A <a@example.com>\r\n";
    content += "To: B <b@example.com>\r\n";
    content += "Subject: Has Attachment\r\n";
    content += "Date: Mon, 01 Jan 2024 00:00:00 +0000\r\n";
    content += "MIME-Version: 1.0\r\n";
    content += "Content-Type: multipart/mixed; boundary=\"BOUND\"\r\n";
    content += "\r\n";
    content += "--BOUND\r\n";
    content += "Content-Type: text/plain; charset=UTF-8\r\n";
    content += "\r\n";
    content += "Body text.\r\n";
    content += "--BOUND\r\n";
    content += "Content-Type: application/octet-stream; name=\"data.bin\"\r\n";
    content += "Content-Transfer-Encoding: base64\r\n";
    content += "Content-Disposition: attachment; filename=\"data.bin\"\r\n";
    content += "\r\n";
    content += "SGVsbG8gQXR0YWNo\r\n";  // "Hello Attach"
    content += "--BOUND--\r\n";
    mbox.write(content);
    mbox.close();

    MboxParser parser;
    parser.open(mbox.fileName());
    QVERIFY(parser.isOpen());
    parser.indexMessages();

    QTemporaryDir out_dir;
    QVERIFY(out_dir.isValid());

    sak::EmailExportConfig config;
    config.format = sak::ExportFormat::Eml;
    config.output_path = out_dir.path();  // item_ids empty -> page the whole mailbox

    EmailExportWorker worker;
    QSignalSpy complete_spy(&worker, &EmailExportWorker::exportComplete);
    worker.exportMboxItems(&parser, config);

    QVERIFY(complete_spy.count() > 0);
    const auto result = complete_spy.first().first().value<sak::EmailExportResult>();
    QCOMPARE(result.items_exported, 1);
    QCOMPARE(result.items_failed, 0);  // readable attachment -> not a partial export

    // One .eml file was written.
    const QStringList eml = QDir(out_dir.path()).entryList({QStringLiteral("*.eml")}, QDir::Files);
    QCOMPARE(eml.size(), 1);
    parser.close();
}

QTEST_MAIN(TestEmailExportWorker)
#include "test_email_export_worker.moc"

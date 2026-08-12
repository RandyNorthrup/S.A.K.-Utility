// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_email_export_worker.cpp
/// @brief Unit tests for the email export worker

#include "sak/email_constants.h"
#include "sak/email_export_worker.h"
#include "sak/email_types.h"
#include "sak/mbox_parser.h"
#include "sak/pst_parser.h"

#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QtTest/QtTest>

class TestEmailExportWorker : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- Config Defaults -------------------------------------------------
    void configDefaults();
    void configCsvOptions();
    void configAttachmentOptions();
    void configEmlOptions();

    // -- Result Defaults -------------------------------------------------
    void resultDefaults();
    void resultPopulation();

    // -- Cancel ----------------------------------------------------------
    void cancelBeforeExportDoesNotPoisonNextExport();

    // -- Export With Null Parser -----------------------------------------
    void exportWithNullPstParserFailsClosed();
    void exportWithNullMboxParserFailsClosed();

    // -- Export With Empty Config ----------------------------------------
    void exportWithEmptyOutputPathFailsClosed();

    // -- Format Coverage -------------------------------------------------
    void allExportFormatValuesAreDistinct();

    // -- B7-25/27: real MBOX export with a readable attachment -----------
    void mboxExportWithAttachmentSucceeds();

    // -- eml_include_headers wiring --------------------------------------
    void emlExportRespectsIncludeHeaders();

    // -- B7: MBOX must reject non-per-message formats, not coerce to EML --
    void mboxRejectsNonMessageFormat();
};

// ============================================================================
// Config Defaults
// ============================================================================

void TestEmailExportWorker::configDefaults() {
    sak::EmailExportConfig config;
    QCOMPARE(config.format, sak::ExportFormat::Eml);
    QVERIFY(config.output_path.isEmpty());
    QVERIFY(config.item_ids.isEmpty());
    QCOMPARE(config.folder_id, static_cast<uint64_t>(0));
    QVERIFY(!config.has_folder);
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

// A cancel raised while nothing is running must be a harmless no-op that does NOT
// poison the NEXT export: exportMboxItems() clears the flag on entry. Without that
// reset the loop would break before writing anything and noteIfCancelled would stamp
// the result with the "cancelled ... output is partial" error.
void TestEmailExportWorker::cancelBeforeExportDoesNotPoisonNextExport() {
    QTemporaryFile mbox;
    QVERIFY(mbox.open());
    QByteArray content;
    content += "From a@example.com Mon Jan  1 00:00:00 2024\r\n";
    content += "From: A <a@example.com>\r\n";
    content += "Subject: Plain\r\n";
    content += "Date: Mon, 01 Jan 2024 00:00:00 +0000\r\n";
    content += "\r\n";
    content += "Body text.\r\n";
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
    config.output_path = out_dir.path();

    EmailExportWorker worker;
    worker.cancel();
    worker.cancel();  // idempotent: a repeated cancel must not wedge the worker either

    QSignalSpy complete_spy(&worker, &EmailExportWorker::exportComplete);
    worker.exportMboxItems(&parser, config);

    QCOMPARE(complete_spy.count(), 1);
    const auto result = complete_spy.first().first().value<sak::EmailExportResult>();
    QCOMPARE(result.items_exported, 1);
    const QString error_text = result.errors.join(QLatin1Char('|'));
    QVERIFY2(result.errors.isEmpty(), qPrintable(error_text));
    parser.close();
}

// ============================================================================
// Export With Null Parser
// ============================================================================

// A null parser is a caller error that must fail CLOSED in both configurations: the
// worker asserts nothing, it emits ONE exportComplete carrying the reason so a caller
// waiting on that signal is never left hanging, and exports nothing.
void TestEmailExportWorker::exportWithNullPstParserFailsClosed() {
    EmailExportWorker worker;
    sak::EmailExportConfig config;
    config.output_path = QStringLiteral("C:/output");
    config.item_ids = {1, 2, 3};

    QSignalSpy complete_spy(&worker, &EmailExportWorker::exportComplete);
    worker.exportItems(nullptr, config);

    QCOMPARE(complete_spy.count(), 1);
    const auto result = complete_spy.first().first().value<sak::EmailExportResult>();
    QCOMPARE(result.items_exported, 0);
    QCOMPARE(result.errors.size(), 1);
    QVERIFY(result.errors.first().contains(QStringLiteral("No PST/OST file open")));
}

void TestEmailExportWorker::exportWithNullMboxParserFailsClosed() {
    EmailExportWorker worker;
    sak::EmailExportConfig config;
    config.output_path = QStringLiteral("C:/output");

    QSignalSpy complete_spy(&worker, &EmailExportWorker::exportComplete);
    worker.exportMboxItems(nullptr, config);

    QCOMPARE(complete_spy.count(), 1);
    const auto result = complete_spy.first().first().value<sak::EmailExportResult>();
    QCOMPARE(result.items_exported, 0);
    QCOMPARE(result.errors.size(), 1);
    QVERIFY(result.errors.first().contains(QStringLiteral("No MBOX file open")));
}

// ============================================================================
// Export With Empty/Invalid Config
// ============================================================================

// An empty output path is refused before any parser work: the guard runs on both the
// PST and the MBOX entry point, and each surfaces the reason through exportComplete
// rather than writing into the process working directory.
void TestEmailExportWorker::exportWithEmptyOutputPathFailsClosed() {
    sak::EmailExportConfig config;  // default output_path is empty
    QVERIFY(config.output_path.isEmpty());

    PstParser pst_parser;  // never touched: the path check runs before any parser use
    EmailExportWorker pst_worker;
    QSignalSpy pst_spy(&pst_worker, &EmailExportWorker::exportComplete);
    pst_worker.exportItems(&pst_parser, config);

    QCOMPARE(pst_spy.count(), 1);
    const auto pst_result = pst_spy.first().first().value<sak::EmailExportResult>();
    QCOMPARE(pst_result.items_exported, 0);
    QCOMPARE(pst_result.errors.size(), 1);
    QVERIFY(pst_result.errors.first().contains(QStringLiteral("output path is empty")));

    MboxParser mbox_parser;
    EmailExportWorker mbox_worker;
    QSignalSpy mbox_spy(&mbox_worker, &EmailExportWorker::exportComplete);
    mbox_worker.exportMboxItems(&mbox_parser, config);

    QCOMPARE(mbox_spy.count(), 1);
    const auto mbox_result = mbox_spy.first().first().value<sak::EmailExportResult>();
    QCOMPARE(mbox_result.items_exported, 0);
    QCOMPARE(mbox_result.errors.size(), 1);
    QVERIFY(mbox_result.errors.first().contains(QStringLiteral("output path is empty")));
}

// ============================================================================
// Format Coverage
// ============================================================================

void TestEmailExportWorker::allExportFormatValuesAreDistinct() {
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

// eml_include_headers must take effect: with it disabled the exported .eml is
// body-only (no From/Subject addressing headers); with it enabled those headers
// are present. Exercises the worker's writeEml header-strip wiring end to end via
// the MBOX->EML path.
void TestEmailExportWorker::emlExportRespectsIncludeHeaders() {
    QTemporaryFile mbox;
    QVERIFY(mbox.open());
    QByteArray content;
    content += "From sender@example.com Mon Jan  1 00:00:00 2024\r\n";
    content += "From: A <a@example.com>\r\n";
    content += "To: B <b@example.com>\r\n";
    content += "Subject: SecretSubjectLine\r\n";
    content += "Date: Mon, 01 Jan 2024 00:00:00 +0000\r\n";
    content += "\r\n";
    content += "Plain body content.\r\n";
    mbox.write(content);
    mbox.close();

    MboxParser parser;
    parser.open(mbox.fileName());
    QVERIFY(parser.isOpen());
    parser.indexMessages();

    const auto read_single_eml = [](const QString& dir) -> QByteArray {
        const QStringList eml = QDir(dir).entryList({QStringLiteral("*.eml")}, QDir::Files);
        if (eml.size() != 1) {
            return {};
        }
        QFile file(dir + QLatin1Char('/') + eml.first());
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return file.readAll();
    };

    // Headers omitted.
    QTemporaryDir off_dir;
    QVERIFY(off_dir.isValid());
    sak::EmailExportConfig off_config;
    off_config.format = sak::ExportFormat::Eml;
    off_config.output_path = off_dir.path();
    off_config.eml_include_headers = false;
    EmailExportWorker off_worker;
    off_worker.exportMboxItems(&parser, off_config);
    const QByteArray off_eml = read_single_eml(off_dir.path());
    QVERIFY(!off_eml.isEmpty());
    QVERIFY(!off_eml.contains("Subject: SecretSubjectLine"));
    QVERIFY(!off_eml.contains("From: "));
    QVERIFY(off_eml.contains("Plain body content."));

    // Headers included (default).
    QTemporaryDir on_dir;
    QVERIFY(on_dir.isValid());
    sak::EmailExportConfig on_config;
    on_config.format = sak::ExportFormat::Eml;
    on_config.output_path = on_dir.path();
    on_config.eml_include_headers = true;
    EmailExportWorker on_worker;
    on_worker.exportMboxItems(&parser, on_config);
    const QByteArray on_eml = read_single_eml(on_dir.path());
    QVERIFY(!on_eml.isEmpty());
    QVERIFY(on_eml.contains("Subject: SecretSubjectLine"));

    parser.close();
}

// An MBOX export requested in a non-per-message format (e.g. CSV) must fail closed
// with a surfaced error rather than silently coerce the request to EML (B7-format).
void TestEmailExportWorker::mboxRejectsNonMessageFormat() {
    QTemporaryFile mbox;
    QVERIFY(mbox.open());
    mbox.write("From s@example.com Mon Jan  1 00:00:00 2024\r\nSubject: x\r\n\r\nbody\r\n");
    mbox.close();

    MboxParser parser;
    parser.open(mbox.fileName());
    QVERIFY(parser.isOpen());
    parser.indexMessages();

    QTemporaryDir out_dir;
    QVERIFY(out_dir.isValid());

    sak::EmailExportConfig config;
    config.format = sak::ExportFormat::CsvEmails;  // not a per-message format
    config.output_path = out_dir.path();

    EmailExportWorker worker;
    QSignalSpy complete_spy(&worker, &EmailExportWorker::exportComplete);
    worker.exportMboxItems(&parser, config);

    QVERIFY(complete_spy.count() > 0);
    const auto result = complete_spy.first().first().value<sak::EmailExportResult>();
    QCOMPARE(result.items_exported, 0);
    QVERIFY(!result.errors.isEmpty());  // the unsupported-format error is surfaced
    // No stray .eml was written from a coerced format.
    QVERIFY(QDir(out_dir.path()).entryList({QStringLiteral("*.eml")}, QDir::Files).isEmpty());
    parser.close();
}

QTEST_MAIN(TestEmailExportWorker)
#include "test_email_export_worker.moc"

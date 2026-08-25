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
    void cancelDuringMboxExportStampsPartialOutput();

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

namespace {

// One MBOX message carrying a base64 attachment ("Hello Attach") in a multipart/mixed body,
// plus an INLINE image part with a Content-ID -- the shape the skip_inline_images option
// exists to filter (a signature logo or a tracking pixel), and which no fixture anywhere in
// the suite produced before, leaving that filter measured-dead.
QByteArray attachmentMboxFixture() {
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
    // image/* is non-text, so MboxParser routes this to appendAttachment despite the
    // "inline" disposition, and records the Content-ID that marks it inline.
    content += "--BOUND\r\n";
    content += "Content-Type: image/png; name=\"logo.png\"\r\n";
    content += "Content-Transfer-Encoding: base64\r\n";
    content += "Content-Disposition: inline; filename=\"logo.png\"\r\n";
    content += "Content-ID: <logo@example.com>\r\n";
    content += "\r\n";
    content += "SW5saW5lTG9nbw==\r\n";  // "InlineLogo"
    content += "--BOUND\r\n";
    content += "Content-Type: application/octet-stream; name=\"data.bin\"\r\n";
    content += "Content-Transfer-Encoding: base64\r\n";
    content += "Content-Disposition: attachment; filename=\"data.bin\"\r\n";
    content += "\r\n";
    content += "SGVsbG8gQXR0YWNo\r\n";  // "Hello Attach"
    content += "--BOUND--\r\n";
    return content;
}

// One plain-text MBOX message whose Subject is the value the include-headers switch must hide.
// It carries Cc and Message-ID as well: clearEmlHeaderFields clears eight fields, but
// EmlWriter::appendHeader returns early on an EMPTY value, so a field this fixture leaves
// empty makes its clear() a provable no-op and the leak assertions below vacuous.
QByteArray plainHeaderMboxFixture() {
    QByteArray content;
    content += "From sender@example.com Mon Jan  1 00:00:00 2024\r\n";
    content += "From: A <a@example.com>\r\n";
    content += "To: B <b@example.com>\r\n";
    content += "Cc: C <c@example.com>\r\n";
    content += "Subject: SecretSubjectLine\r\n";
    content += "Date: Mon, 01 Jan 2024 00:00:00 +0000\r\n";
    content += "Message-ID: <SecretMsgId42@example.com>\r\n";
    content += "\r\n";
    content += "Plain body content.\r\n";
    return content;
}

// The inline-skip assertions in the caller are one-sided: dropping the inline part somewhere
// EARLIER -- a parser that never records it, or a filter keyed on the wrong field --
// satisfies them just as well as the skip does, and leaves the guard measured-dead exactly
// as it was. Re-exporting with skip_inline_images=false must EMBED it, which is only
// possible if the part reached the filter and the filter read the flag. The HTML re-export
// then pins the SECOND row of the ordered format-name table, whose first row is all any
// other assertion in this file has ever matched.
void verifyInlineKeepArmAndHtmlFormatRow(MboxParser& parser) {
    // The OTHER arm, and the reason the three assertions above are not vacuous. Dropping the
    // inline part somewhere EARLIER -- a parser that never records it, or a filter keyed on
    // the wrong field -- satisfies them just as well as the skip does, and leaves the guard
    // measured-dead exactly as it was. Re-exporting with skip_inline_images=false must EMBED
    // it, which is only possible if the part reached the filter and the filter read the flag.
    QTemporaryDir keep_dir;
    QVERIFY(keep_dir.isValid());
    sak::EmailExportConfig keep_config;
    keep_config.format = sak::ExportFormat::Eml;
    keep_config.output_path = keep_dir.path();
    keep_config.skip_inline_images = false;

    EmailExportWorker keep_worker;
    QSignalSpy keep_spy(&keep_worker, &EmailExportWorker::exportComplete);
    keep_worker.exportMboxItems(&parser, keep_config);
    QCOMPARE(keep_spy.count(), 1);
    const auto keep_result = keep_spy.first().first().value<sak::EmailExportResult>();
    QVERIFY(keep_result.errors.isEmpty());
    QCOMPARE(keep_result.items_exported, 1);

    const QStringList keep_eml =
        QDir(keep_dir.path()).entryList({QStringLiteral("*.eml")}, QDir::Files);
    QCOMPARE(keep_eml.size(), 1);
    QFile keep_written(keep_dir.path() + QLatin1Char('/') + keep_eml.first());
    QVERIFY(keep_written.open(QIODevice::ReadOnly));
    const QByteArray keep_bytes = keep_written.readAll();
    keep_written.close();
    QVERIFY(keep_bytes.contains("logo.png"));
    QVERIFY(keep_bytes.contains("SW5saW5lTG9nbw"));
    QCOMPARE(keep_bytes.count("Content-Disposition: attachment;"), 2);

    // The label comes from kExportFormatNames, an ORDERED table, and every assertion above
    // matches its FIRST row -- coverage shows the row comparison never once returned false, so
    // the loop has never advanced past Eml and the other eleven rows are unverified text.
    // Re-exporting the SAME fixture as HTML pins the second row and drives the Html arms that
    // the coverage run reports dead: isMessageFileFormat's Html clause, the HtmlEmailWriter
    // construction in prepareMessageWriters, and exportOneMboxItem's Html case.
    QTemporaryDir html_dir;
    QVERIFY(html_dir.isValid());
    sak::EmailExportConfig html_config;
    html_config.format = sak::ExportFormat::Html;
    html_config.output_path = html_dir.path();

    EmailExportWorker html_worker;
    QSignalSpy html_spy(&html_worker, &EmailExportWorker::exportComplete);
    html_worker.exportMboxItems(&parser, html_config);
    QCOMPARE(html_spy.count(), 1);
    const auto html_result = html_spy.first().first().value<sak::EmailExportResult>();
    // A renamed or deleted Html row reports "EML (from MBOX)" or " (from MBOX)" here, and
    // dropping Html from isMessageFileFormat refuses the export outright.
    QCOMPARE(html_result.export_format, QStringLiteral("HTML (from MBOX)"));
    QVERIFY(html_result.errors.isEmpty());
    QCOMPARE(html_result.items_exported, 1);
    QCOMPARE(html_result.items_failed, 0);
    QCOMPARE(QDir(html_dir.path()).entryList({QStringLiteral("*.html")}, QDir::Files).size(), 1);
}

// The headers-ON half: the recipient header must survive that path too, and the two fields
// the leak checks depend on must really reach the writer -- appendHeader returns early on an
// empty value, so an absent Cc or Message-ID cannot leak either way and would make those
// checks vacuous again, which is the exact failure being fixed.
template <typename ReadSingleEml>
void verifyHeadersOnPathCarriesEveryHeader(MboxParser& parser,
                                           const ReadSingleEml& read_single_eml) {
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
    QVERIFY(on_eml.contains("Subject: SecretSubjectLine\r\n"));
    // The recipient header must survive the include-headers path too: nothing else pins
    // display_to, so losing the worker's msg.to -> display_to mapping is invisible today.
    QVERIFY(on_eml.contains("To: B <b@example.com>\r\n"));
    // And the two fields the leak checks above depend on really do reach the writer. Without
    // this, a fixture typo would silently make those checks vacuous again -- appendHeader
    // returns early on an empty value, so an absent Cc/Message-ID cannot leak either way.
    QVERIFY(on_eml.contains("Cc: C <c@example.com>\r\n"));
    QVERIFY(on_eml.contains("Message-ID: <SecretMsgId42@example.com>\r\n"));
}

// The attachment BYTES must actually reach the written file: with an empty attachment vector
// EmlWriter takes the simple text/plain branch and every count assertion in the caller still
// passes. The inline part (Content-ID set) must equally be EXCLUDED -- skip_inline_images
// defaults true, and without that skip every signature logo and tracking pixel is re-embedded.
void verifyEmbeddedAttachmentBytesAndInlineSkip(const QString& out_dir,
                                                const QString& eml_name,
                                                const sak::EmailExportResult& result) {
    // ...and the attachment BYTES actually reached that file. With an empty attachment
    // vector (the pre-B7 behaviour) EmlWriter takes the simple text/plain branch and
    // every assertion above -- exported=1, failed=0, one .eml -- still passes.
    QFile written(out_dir + QLatin1Char('/') + eml_name);
    QVERIFY(written.open(QIODevice::ReadOnly));
    const QByteArray eml_bytes = written.readAll();
    written.close();
    QVERIFY(eml_bytes.contains("Content-Type: multipart/mixed; boundary=\"----=_SAK_Part_"));
    QVERIFY(eml_bytes.contains("Content-Type: application/octet-stream; name=\"data.bin\"\r\n"));
    QVERIFY(eml_bytes.contains(
        "Content-Disposition: attachment; filename=\"data.bin\"\r\n\r\nSGVsbG8gQXR0YWNo\r\n"));
    // total_bytes is the writer's own byte counter; cross-check it against the file.
    QCOMPARE(result.total_bytes, static_cast<qint64>(eml_bytes.size()));

    // ...and the INLINE part (Content-ID set) was EXCLUDED. skip_inline_images defaults true,
    // so collectMboxAttachmentData must drop it before EmlWriter ever sees it; without that
    // skip, every signature logo and tracking pixel is re-embedded as a full attachment part.
    QVERIFY(!eml_bytes.contains("logo.png"));
    QVERIFY(!eml_bytes.contains("SW5saW5lTG9nbw"));
    QCOMPARE(eml_bytes.count("Content-Disposition: attachment;"), 1);
}

}  // namespace

// ============================================================================
// Config Defaults
// ============================================================================

void TestEmailExportWorker::configDefaults() {
    sak::EmailExportConfig config;
    QCOMPARE(config.format, sak::ExportFormat::Eml);
    QVERIFY(config.output_path.isEmpty());
    QVERIFY(config.item_ids.isEmpty());
    QCOMPARE(config.folder_id, static_cast<uint64_t>(0));
    QVERIFY(config.folder_ids.isEmpty());
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

    // `format` is what selects the writer, so an option block that never reads it back cannot
    // tell a CSV config from any other. cppcheck flagged all three of these as unreadVariable
    // once the gate stopped being blind (FINDING N8).
    QCOMPARE(config.format, sak::ExportFormat::CsvEmails);
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
    QCOMPARE(config.format, sak::ExportFormat::Attachments);
    QVERIFY(!config.skip_inline_images);
}

void TestEmailExportWorker::configEmlOptions() {
    sak::EmailExportConfig config;
    config.format = sak::ExportFormat::Eml;
    config.eml_include_headers = false;
    config.prefix_with_date = false;

    QCOMPARE(config.format, sak::ExportFormat::Eml);
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

    // The three fields below were assigned and never read back, so the round trip proved only
    // the counters. export_path/export_format are what the completion report shows the operator,
    // and total_bytes is the exported volume.
    QCOMPARE(result.export_path, QStringLiteral("C:/output"));
    QCOMPARE(result.export_format, QStringLiteral("EML"));
    QCOMPARE(result.total_bytes, static_cast<qint64>(1024 * 1024));
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

// The test above claims something about cancel(), but every assertion in it describes a
// CLEAN export -- empty out the body of EmailExportWorker::cancel() and it still passes with
// identical values, because the cancel it raises is REQUIRED to have no effect. Nothing else
// in the suite covers for that: the coverage run reports both cancellation checks -- the MBOX
// item loop's and noteIfCancelled's -- as never once taken. So a cancel raised WHILE an
// export is running must be shown to stop the run AND mark the result partial: the loop
// breaks before the only message is written, and noteIfCancelled stamps "output is partial"
// so a caller can tell a cancelled export from a clean one.
void TestEmailExportWorker::cancelDuringMboxExportStampsPartialOutput() {
    QTemporaryFile mbox;
    QVERIFY(mbox.open());
    mbox.write(plainHeaderMboxFixture());
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
    QSignalSpy started_spy(&worker, &EmailExportWorker::exportStarted);
    QSignalSpy complete_spy(&worker, &EmailExportWorker::exportComplete);

    // exportStarted is emitted AFTER the entry reset of the cancelled flag and BEFORE the
    // item loop, and the worker lives on this thread, so this handler runs synchronously and
    // the cancel it raises is the one the loop sees. Raising it before the call instead would
    // be erased by that entry reset -- which is exactly what the previous test pins.
    QObject::connect(&worker, &EmailExportWorker::exportStarted, &worker, [&worker](int) {
        worker.cancel();
    });

    worker.exportMboxItems(&parser, config);

    QCOMPARE(started_spy.count(), 1);
    QCOMPARE(complete_spy.count(), 1);
    const auto result = complete_spy.first().first().value<sak::EmailExportResult>();
    QCOMPARE(result.items_exported, 0);  // the loop broke before the one message
    QCOMPARE(result.items_failed, 0);    // a cancel is not an item failure
    QCOMPARE(result.errors.size(), 1);
    QVERIFY2(result.errors.first().contains(QStringLiteral("output is partial")),
             qPrintable(result.errors.join(QLatin1Char('|'))));
    QVERIFY(QDir(out_dir.path()).entryList(QDir::Files).isEmpty());  // nothing written
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
    mbox.write(attachmentMboxFixture());
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
    QSignalSpy started_spy(&worker, &EmailExportWorker::exportStarted);
    QSignalSpy complete_spy(&worker, &EmailExportWorker::exportComplete);
    worker.exportMboxItems(&parser, config);

    // The total handed to the caller's progress UI is the RESOLVED index count from
    // paging the mailbox, not the (empty) requested id list. Nothing else in this file
    // asserts exportStarted at all.
    QCOMPARE(started_spy.count(), 1);
    QCOMPARE(started_spy.first().first().toInt(), 1);

    QCOMPARE(complete_spy.count(), 1);
    const auto result = complete_spy.first().first().value<sak::EmailExportResult>();
    QCOMPARE(result.items_exported, 1);
    QCOMPARE(result.items_failed, 0);  // readable attachment -> not a partial export
    QVERIFY(result.errors.isEmpty());
    QCOMPARE(result.export_path, out_dir.path());
    // The result's format label carries the MBOX provenance; a bare "EML" (or an empty
    // name from a kExportFormatNames table that lost its Eml row) is a different value.
    QCOMPARE(result.export_format, QStringLiteral("EML (from MBOX)"));

    // One .eml file was written.
    const QStringList eml = QDir(out_dir.path()).entryList({QStringLiteral("*.eml")}, QDir::Files);
    QCOMPARE(eml.size(), 1);
    // The date prefix is the ONLY place config.prefix_with_date (default true) is observable
    // anywhere in this file -- it is set and read back as struct storage, never as behaviour.
    // It is also the only check that the item's subject and its parsed Date reached the writer
    // at all. Hard-coding EmlWriter's prefix argument to false, or losing the msg.date ->
    // item.date mapping, drops the prefix silently while every other assertion here holds.
    // The fixture Date carries +0000, so the printed wall clock is machine-independent.
    QCOMPARE(eml.first(), QStringLiteral("2024-01-01_000000_Has Attachment.eml"));

    verifyEmbeddedAttachmentBytesAndInlineSkip(out_dir.path(), eml.first(), result);

    verifyInlineKeepArmAndHtmlFormatRow(parser);

    parser.close();
}

// eml_include_headers must take effect: with it disabled the exported .eml is
// body-only (no From/Subject addressing headers); with it enabled those headers
// are present. Exercises the worker's writeEml header-strip wiring end to end via
// the MBOX->EML path.
void TestEmailExportWorker::emlExportRespectsIncludeHeaders() {
    QTemporaryFile mbox;
    QVERIFY(mbox.open());
    mbox.write(plainHeaderMboxFixture());
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
    // From/To/Cc/Subject/Message-ID/Date are 6 of the 8 fields clearEmlHeaderFields must
    // clear, and appendStandardHeaders runs before the first MIME byte -- so a body-only .eml
    // must begin AT the MIME block. Forgetting any one of them leaks a recipient, a timestamp
    // or a thread id while the coarser checks stay green. (in_reply_to is unreachable from
    // here: MboxMessageDetail has no such field, so that clear needs a PST-path test.)
    QVERIFY(off_eml.startsWith("MIME-Version: 1.0\r\n"));
    QVERIFY(!off_eml.contains("b@example.com"));  // To: recipient must not leak
    QVERIFY(!off_eml.contains("a@example.com"));  // From: addr-spec must not leak
    QVERIFY(!off_eml.contains("c@example.com"));  // Cc: recipient must not leak
    QVERIFY(!off_eml.contains("SecretMsgId42"));  // originating Message-ID must not leak
    QVERIFY(off_eml.contains("Plain body content."));

    verifyHeadersOnPathCarriesEveryHeader(parser, read_single_eml);

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

    QCOMPARE(complete_spy.count(), 1);
    const auto result = complete_spy.first().first().value<sak::EmailExportResult>();
    QCOMPARE(result.items_exported, 0);
    QCOMPARE(result.errors.size(), 1);
    QVERIFY(result.errors.first().contains(
        QStringLiteral("MBOX export supports only per-message formats")));
    // No stray .eml was written from a coerced format.
    QVERIFY(QDir(out_dir.path()).entryList({QStringLiteral("*.eml")}, QDir::Files).isEmpty());

    // The guard delegates the whole decision to isMessageFileFormat(), a four-arm alternation,
    // and CsvEmails above is the ONLY value the entire suite has ever fed it -- the coverage
    // run reports the Html/Text/Pdf clauses never once evaluated. One rejected value leaves
    // the acceptance set free to widen: a mutation such as `return !isCsvFormat(format);`
    // keeps the probe above green while letting Vcf / Ics / PlainTextNotes / Attachments past,
    // where messageFormatOrEml() coerces them to EML and the export silently succeeds in the
    // wrong format -- precisely the coercion this test is named for. Probe every one of them,
    // and check the whole directory rather than *.eml alone, since a coerced Text format
    // would write .txt and slip past a glob that only looks for .eml.
    const QVector<sak::ExportFormat> refused_formats = {sak::ExportFormat::Vcf,
                                                        sak::ExportFormat::Ics,
                                                        sak::ExportFormat::CsvContacts,
                                                        sak::ExportFormat::CsvCalendar,
                                                        sak::ExportFormat::CsvTasks,
                                                        sak::ExportFormat::PlainTextNotes,
                                                        sak::ExportFormat::Attachments};
    for (const auto refused : refused_formats) {
        const QByteArray label = QByteArray::number(static_cast<int>(refused));
        QTemporaryDir each_dir;
        QVERIFY2(each_dir.isValid(), label.constData());
        sak::EmailExportConfig each_config;
        each_config.format = refused;
        each_config.output_path = each_dir.path();

        EmailExportWorker each_worker;
        QSignalSpy each_spy(&each_worker, &EmailExportWorker::exportComplete);
        each_worker.exportMboxItems(&parser, each_config);

        QVERIFY2(each_spy.count() == 1, label.constData());
        const auto each_result = each_spy.first().first().value<sak::EmailExportResult>();
        QVERIFY2(each_result.items_exported == 0, label.constData());
        QVERIFY2(each_result.errors.size() == 1, label.constData());
        QVERIFY2(each_result.errors.first().contains(
                     QStringLiteral("MBOX export supports only per-message formats")),
                 qPrintable(each_result.errors.first()));
        QVERIFY2(QDir(each_dir.path()).entryList(QDir::Files).isEmpty(), label.constData());
    }
    parser.close();
}

QTEST_MAIN(TestEmailExportWorker)
#include "test_email_export_worker.moc"

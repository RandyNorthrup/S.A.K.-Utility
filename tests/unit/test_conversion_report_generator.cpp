// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_conversion_report_generator.cpp
/// @brief Unit tests for ConversionReportGenerator HTML/CSV output

#include "sak/conversion_report_generator.h"
#include "sak/email_types.h"
#include "sak/ost_converter_types.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class TestConversionReportGenerator : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // ====================================================================
    // Happy Path -- Generate HTML report
    // ====================================================================

    void testGenerateHtmlReport() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::OstConversionBatchResult batch;
        batch.files_total = 2;
        batch.files_succeeded = 2;
        batch.files_failed = 0;
        batch.total_items_converted = 150;
        batch.total_bytes_written = 1024 * 1024 * 50;

        sak::OstConversionResult r1;
        r1.source_path = QStringLiteral("archive1.ost");
        r1.items_converted = 100;
        r1.items_failed = 2;
        r1.bytes_written = 1024 * 1024 * 30;
        batch.file_results.append(r1);

        sak::OstConversionResult r2;
        r2.source_path = QStringLiteral("archive2.pst");
        r2.items_converted = 50;
        r2.items_failed = 1;
        r2.bytes_written = 1024 * 1024 * 20;
        batch.file_results.append(r2);

        QString report_path = sak::ConversionReportGenerator::generateHtmlReport(batch,
                                                                                 temp_dir.path());

        QCOMPARE(report_path, temp_dir.path() + QStringLiteral("/conversion_report.html"));
        QFile file(report_path);
        QVERIFY(file.exists());
        QVERIFY(file.open(QIODevice::ReadOnly));

        QByteArray content = file.readAll();
        // Pin the exact deterministic document open (kEnterpriseReportDocumentOpen). The `||`
        // passed for any doc carrying either token; this catches a broken/reordered head or wrong
        // title.
        QVERIFY(
            content.startsWith("<!DOCTYPE html><html><head><meta charset='utf-8'><title>S.A.K. "
                               "Utility - Conversion Report</title><style>"));
        // Pin the whole file-results row (source/status-class/counts/size/duration/hash-preview);
        // contains("archive1.ost") would pass on a column swap or wrong status class. The empty
        // hash renders as an em-dash (U+2014, UTF-8 E2 80 94), written here as ASCII escapes.
        QVERIFY(content.contains(QByteArray(
            "<tr><td>archive1.ost</td><td class='warn'>100</td><td class='error'>2</td><td>0</td>"
            "<td>30.0 MB</td><td>0 ms</td><td class='hash-preview'>\xE2\x80\x94</td></tr>")));
        QVERIFY(content.contains("archive2.pst"));
    }

    // ====================================================================
    // Empty batch result
    // ====================================================================

    void testEmptyBatch() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::OstConversionBatchResult batch;
        batch.files_total = 0;
        batch.files_succeeded = 0;

        QString report_path = sak::ConversionReportGenerator::generateHtmlReport(batch,
                                                                                 temp_dir.path());

        QCOMPARE(report_path, temp_dir.path() + QStringLiteral("/conversion_report.html"));
        QFile file(report_path);
        QVERIFY(file.exists());
    }

    // ====================================================================
    // CSV manifest generation
    // ====================================================================

    void testGenerateCsvManifest() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        QVector<sak::PstItemDetail> items;
        sak::PstItemDetail item;
        item.subject = QStringLiteral("Test Subject");
        item.sender_email = QStringLiteral("test@example.com");
        item.date = QDateTime(QDate(2025, 6, 15), QTime(10, 0, 0));
        items.append(item);

        QVector<QVector<sak::MapiProperty>> all_props;
        QVector<sak::MapiProperty> props;
        sak::MapiProperty prop;
        prop.property_name = QStringLiteral("PidTagSubject");
        prop.display_value = QStringLiteral("Test Subject");
        props.append(prop);
        all_props.append(props);

        QString csv_path =
            sak::ConversionReportGenerator::generateCsvManifest(items, all_props, temp_dir.path());

        QCOMPARE(csv_path, temp_dir.path() + QStringLiteral("/properties_manifest.csv"));
        QFile file(csv_path);
        QVERIFY(file.exists());
        QVERIFY(file.open(QIODevice::ReadOnly));

        QByteArray content = file.readAll();
        // Pin the exact header (with the single pivot column) and the full data row; a bare
        // contains("Test Subject") would pass even if columns were reordered or shifted.
        QVERIFY(
            content.contains("NodeId,Subject,SenderName,SenderEmail,Date,MessageId,PidTagSubject"));
        QVERIFY(
            content.contains("0,Test Subject,,test@example.com,2025-06-15T10:00:00,,Test Subject"));
    }

    // ====================================================================
    // Report with failures
    // ====================================================================

    void testReportWithFailures() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::OstConversionBatchResult batch;
        batch.files_total = 2;
        batch.files_succeeded = 1;
        batch.files_failed = 1;
        batch.total_items_converted = 50;

        sak::OstConversionResult r1;
        r1.source_path = QStringLiteral("good.ost");
        r1.items_converted = 50;
        batch.file_results.append(r1);

        sak::OstConversionResult r2;
        r2.source_path = QStringLiteral("bad.ost");
        r2.items_converted = 0;
        r2.items_failed = 100;
        r2.errors.append(QStringLiteral("Corrupted file header"));
        batch.file_results.append(r2);

        QString report_path = sak::ConversionReportGenerator::generateHtmlReport(batch,
                                                                                 temp_dir.path());

        QFile file(report_path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray content = file.readAll();
        // Pin the whole error-log row (name<->error pairing plus the 'error' class), unique to the
        // error log; contains("bad.ost") also matches the file-results table row for the same name.
        QVERIFY(content.contains(
            "<tr><td>bad.ost</td><td class='error'>Corrupted file header</td></tr>"));
        QVERIFY(content.contains("Corrupted file header"));
    }

    // ====================================================================
    // B7-17: CSV cells are attacker-controlled -- neutralize spreadsheet
    // formula injection and quote per RFC 4180.
    // ====================================================================

    void csvSafeCell_neutralizesFormulaInjection() {
        using sak::ConversionReportGenerator;
        // Leading =/+/-/@ (and tab/CR) get a single-quote prefix so a spreadsheet treats them as
        // text; a comma also forces quoting around the now-prefixed value.
        QCOMPARE(ConversionReportGenerator::csvSafeCell(QStringLiteral("=cmd|'/c calc'!A1")),
                 QStringLiteral("'=cmd|'/c calc'!A1"));
        QCOMPARE(ConversionReportGenerator::csvSafeCell(QStringLiteral("+1+2")),
                 QStringLiteral("'+1+2"));
        QCOMPARE(ConversionReportGenerator::csvSafeCell(QStringLiteral("-2+3")),
                 QStringLiteral("'-2+3"));
        QCOMPARE(ConversionReportGenerator::csvSafeCell(QStringLiteral("@SUM(A1)")),
                 QStringLiteral("'@SUM(A1)"));
    }

    void csvSafeCell_quotesButDoesNotPrefixSafeText() {
        using sak::ConversionReportGenerator;
        // Ordinary text passes through unchanged.
        QCOMPARE(ConversionReportGenerator::csvSafeCell(QStringLiteral("Hello World")),
                 QStringLiteral("Hello World"));
        // A comma forces RFC 4180 quoting; an internal quote is doubled.
        QCOMPARE(ConversionReportGenerator::csvSafeCell(QStringLiteral("a,b")),
                 QStringLiteral("\"a,b\""));
        QCOMPARE(ConversionReportGenerator::csvSafeCell(QStringLiteral("say \"hi\"")),
                 QStringLiteral("\"say \"\"hi\"\"\""));
        // Empty stays empty (no prefix).
        QCOMPARE(ConversionReportGenerator::csvSafeCell(QString()), QString());
    }

    void csvManifest_neutralizesFormulaInSubject() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::PstItemDetail item;
        item.node_id = 1;
        item.subject = QStringLiteral("=HYPERLINK(\"http://evil\")");
        item.sender_email = QStringLiteral("a@b.com");
        QVector<sak::PstItemDetail> items = {item};
        QVector<QVector<sak::MapiProperty>> props = {{}};

        const QString path =
            sak::ConversionReportGenerator::generateCsvManifest(items, props, temp_dir.path());
        QCOMPARE(path, temp_dir.path() + QStringLiteral("/properties_manifest.csv"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray csv = file.readAll();
        // The dangerous subject must appear single-quote-prefixed and quoted, never as a raw
        // leading '=' cell.
        // Pin the fully escaped cell: the leading '=' gets a single-quote prefix and the internal
        // quotes are doubled and the cell wrapped (RFC 4180). The old prefix-only check missed a
        // quote-doubling regression in this manifest-writer path.
        QVERIFY(csv.contains("\"'=HYPERLINK(\"\"http://evil\"\")\""));
        QVERIFY(!csv.contains(",=HYPERLINK"));
    }
};

QTEST_MAIN(TestConversionReportGenerator)

#include "test_conversion_report_generator.moc"

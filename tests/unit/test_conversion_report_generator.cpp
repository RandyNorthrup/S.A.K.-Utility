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
    // Happy Path â€” Generate HTML report
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

        QVERIFY(!report_path.isEmpty());
        QFile file(report_path);
        QVERIFY(file.exists());
        QVERIFY(file.open(QIODevice::ReadOnly));

        QByteArray content = file.readAll();
        QVERIFY(content.contains("<!DOCTYPE html>") || content.contains("<html"));
        QVERIFY(content.contains("archive1.ost"));
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

        QVERIFY(!report_path.isEmpty());
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

        QVERIFY(!csv_path.isEmpty());
        QFile file(csv_path);
        QVERIFY(file.exists());
        QVERIFY(file.open(QIODevice::ReadOnly));

        QByteArray content = file.readAll();
        QVERIFY(content.contains("Test Subject"));
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
        QVERIFY(content.contains("bad.ost"));
        QVERIFY(content.contains("Corrupted") || content.contains("fail"));
    }
};

QTEST_MAIN(TestConversionReportGenerator)

#include "test_conversion_report_generator.moc"

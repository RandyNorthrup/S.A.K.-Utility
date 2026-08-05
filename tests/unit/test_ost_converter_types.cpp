// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_ost_converter_types.cpp
/// @brief Unit tests for OST Converter type invariants and defaults

#include "sak/ost_converter_constants.h"
#include "sak/ost_converter_types.h"

#include <QTest>

class TestOstConverterTypes : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // ====================================================================
    // OstConversionJob Defaults
    // ====================================================================

    void testJobDefaultStatus() {
        sak::OstConversionJob job;
        QCOMPARE(job.status, sak::OstConversionJob::Status::Queued);
        QCOMPARE(job.items_processed, 0);
        QCOMPARE(job.items_total, 0);
        QCOMPARE(job.items_recovered, 0);
        QCOMPARE(job.items_failed, 0);
        QCOMPARE(job.bytes_written, static_cast<qint64>(0));
        QVERIFY(job.source_path.isEmpty());
        QVERIFY(job.display_name.isEmpty());
        QVERIFY(job.error_message.isEmpty());
    }

    void testJobFileMetadata() {
        sak::OstConversionJob job;
        job.source_path = QStringLiteral("C:/test/mailbox.ost");
        job.display_name = QStringLiteral("mailbox.ost");
        job.file_size_bytes = 1024 * 1024;
        job.is_ost = true;
        job.estimated_items = 500;
        job.estimated_folders = 10;

        QCOMPARE(job.source_path, QStringLiteral("C:/test/mailbox.ost"));
        QCOMPARE(job.display_name, QStringLiteral("mailbox.ost"));
        QCOMPARE(job.file_size_bytes, static_cast<qint64>(1024 * 1024));
        QVERIFY(job.is_ost);
        QCOMPARE(job.estimated_items, 500);
        QCOMPARE(job.estimated_folders, 10);
    }

    // ====================================================================
    // OstConversionConfig Defaults
    // ====================================================================

    void testConfigDefaults() {
        sak::OstConversionConfig config;
        // There is no output format: the converter produces MBOX and nothing else. Per-message
        // output belongs to the Email Inspector, so an untouched config already describes a
        // complete, runnable conversion.
        QVERIFY(config.output_directory.isEmpty());
        QCOMPARE(config.max_threads, 2);
        QVERIFY(config.date_from.isNull());
        QVERIFY(config.date_to.isNull());
        QVERIFY(config.folder_include.isEmpty());
        QVERIFY(config.folder_exclude.isEmpty());
        QCOMPARE(config.recovery_mode, sak::RecoveryMode::Normal);
        QVERIFY(!config.recover_deleted_items);
        // One .mbox per source folder by default, so the folder tree survives into the
        // importing client rather than collapsing into a single mailbox.
        QVERIFY(config.one_mbox_per_folder);
        QVERIFY(config.generate_html_report);
        QVERIFY(config.include_source_checksums);
    }

    void testConfigThreadBounds() {
        QVERIFY(sak::ost::kMinThreads >= 1);
        QVERIFY(sak::ost::kMaxThreads <= 16);
        QVERIFY(sak::ost::kDefaultThreads >= sak::ost::kMinThreads);
        QVERIFY(sak::ost::kDefaultThreads <= sak::ost::kMaxThreads);
    }

    // ====================================================================
    // OstConversionResult Defaults
    // ====================================================================

    // There is deliberately no output-format support table any more. It existed to record
    // which formats did not work (PST, MSG, DBX, IMAP upload) -- a list of switched-off
    // features living in the source. Every one was either removed or is the single format the
    // converter produces, so the table, its "not supported" GUI labelling and the tests that
    // pinned it are all gone rather than kept as a home for half-finished formats.

    void testResultDefaults() {
        sak::OstConversionResult result;
        QCOMPARE(result.items_converted, 0);
        QCOMPARE(result.items_failed, 0);
        QCOMPARE(result.items_recovered, 0);
        QCOMPARE(result.folders_processed, 0);
        QCOMPARE(result.bytes_written, static_cast<qint64>(0));
        QVERIFY(result.errors.isEmpty());
        QVERIFY(result.source_sha256.isEmpty());
    }

    // ====================================================================
    // OstConversionBatchResult Defaults
    // ====================================================================

    void testBatchResultDefaults() {
        sak::OstConversionBatchResult batch;
        QCOMPARE(batch.files_total, 0);
        QCOMPARE(batch.files_succeeded, 0);
        QCOMPARE(batch.files_failed, 0);
        QCOMPARE(batch.total_items_converted, 0);
        QCOMPARE(batch.total_items_recovered, 0);
        QCOMPARE(batch.total_bytes_written, static_cast<qint64>(0));
        QVERIFY(batch.file_results.isEmpty());
    }

    // ====================================================================
    // Constants
    // ====================================================================

    void testQueueColumnEnum() {
        QCOMPARE(sak::ost::ColFile, 0);
        QCOMPARE(sak::ost::ColSize, 1);
        QCOMPARE(sak::ost::ColItems, 2);
        QCOMPARE(sak::ost::ColStatus, 3);
        QCOMPARE(sak::ost::ColProgress, 4);
        QCOMPARE(sak::ost::ColCount, 5);
    }

    // ====================================================================
    // Enum Coverage
    // ====================================================================

    void testRecoveryModeValues() {
        QVERIFY(static_cast<int>(sak::RecoveryMode::Normal) !=
                static_cast<int>(sak::RecoveryMode::SkipCorrupt));
        QVERIFY(static_cast<int>(sak::RecoveryMode::SkipCorrupt) !=
                static_cast<int>(sak::RecoveryMode::DeepRecovery));
    }

    void testJobStatusValues() {
        QVector<int> values;
        values << static_cast<int>(sak::OstConversionJob::Status::Queued)
               << static_cast<int>(sak::OstConversionJob::Status::Parsing)
               << static_cast<int>(sak::OstConversionJob::Status::Converting)
               << static_cast<int>(sak::OstConversionJob::Status::Complete)
               << static_cast<int>(sak::OstConversionJob::Status::Failed)
               << static_cast<int>(sak::OstConversionJob::Status::Cancelled);

        QSet<int> unique(values.begin(), values.end());
        QCOMPARE(unique.size(), values.size());
    }
};

QTEST_MAIN(TestOstConverterTypes)
#include "test_ost_converter_types.moc"

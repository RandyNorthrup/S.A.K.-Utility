// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_ost_converter_controller.cpp
/// @brief Unit tests for OstConverterController queue management

#include "sak/ost_conversion_worker.h"
#include "sak/ost_converter_constants.h"
#include "sak/ost_converter_controller.h"
#include "sak/ost_converter_types.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>

/// Helper: create a dummy file in a temp directory and return its path
static QString createTempFile(QTemporaryDir& dir, const QString& name) {
    QString path = dir.path() + "/" + name;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        qFatal("Failed to create temporary OST test file");
    }
    file.write("dummy ost content");
    file.close();
    return path;
}

class TestOstConverterController : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // ====================================================================
    // Queue Management -- Add Files
    // ====================================================================

    void testAddSingleFile() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path = createTempFile(temp, "email.ost");

        sak::OstConverterController ctrl;
        QSignalSpy spy(&ctrl, &sak::OstConverterController::fileAdded);

        ctrl.addFile(path);

        QCOMPARE(ctrl.queue().size(), 1);
        QVERIFY(spy.count() >= 1);
    }

    void testAddMultipleFiles() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path1 = createTempFile(temp, "one.ost");
        QString path2 = createTempFile(temp, "two.pst");

        sak::OstConverterController ctrl;

        ctrl.addFile(path1);
        ctrl.addFile(path2);

        QCOMPARE(ctrl.queue().size(), 2);
    }

    void testAddDuplicateFileRejected() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path = createTempFile(temp, "email.ost");

        sak::OstConverterController ctrl;

        ctrl.addFile(path);
        ctrl.addFile(path);

        QCOMPARE(ctrl.queue().size(), 1);
    }

    // ====================================================================
    // Queue Management -- Remove Files
    // ====================================================================

    void testRemoveFile() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path1 = createTempFile(temp, "one.ost");
        QString path2 = createTempFile(temp, "two.ost");

        sak::OstConverterController ctrl;

        ctrl.addFile(path1);
        ctrl.addFile(path2);
        ctrl.removeFile(0);

        QCOMPARE(ctrl.queue().size(), 1);
    }

    void testRemoveInvalidIndex() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path = createTempFile(temp, "one.ost");

        sak::OstConverterController ctrl;

        ctrl.addFile(path);
        ctrl.removeFile(5);                // Out of bounds

        QCOMPARE(ctrl.queue().size(), 1);  // Unchanged
    }

    // ====================================================================
    // Queue Management -- Clear
    // ====================================================================

    void testClearQueue() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path1 = createTempFile(temp, "one.ost");
        QString path2 = createTempFile(temp, "two.ost");
        QString path3 = createTempFile(temp, "three.ost");

        sak::OstConverterController ctrl;

        ctrl.addFile(path1);
        ctrl.addFile(path2);
        ctrl.addFile(path3);
        ctrl.clearQueue();

        QCOMPARE(ctrl.queue().size(), 0);
    }

    void testClearEmptyQueue() {
        sak::OstConverterController ctrl;
        ctrl.clearQueue();  // Should not crash
        QCOMPARE(ctrl.queue().size(), 0);
    }

    // ====================================================================
    // Queue Access
    // ====================================================================

    void testQueueJobAccess() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path = createTempFile(temp, "email.ost");

        sak::OstConverterController ctrl;

        ctrl.addFile(path);

        const auto& jobs = ctrl.queue();
        QCOMPARE(jobs.size(), 1);
        QVERIFY(jobs[0].source_path.endsWith("email.ost"));
        QCOMPARE(jobs[0].status, sak::OstConversionJob::Status::Queued);
    }

    // ====================================================================
    // Conversion State
    // ====================================================================

    void testIsRunningInitiallyFalse() {
        sak::OstConverterController ctrl;
        QVERIFY(!ctrl.isRunning());
    }

    void testCannotStartWithEmptyQueue() {
        sak::OstConverterController ctrl;

        sak::OstConversionConfig config;
        config.output_directory = QStringLiteral("C:/output");

        ctrl.startConversion(config);
        QVERIFY(!ctrl.isRunning());
    }

    // ====================================================================
    // Signal Emission
    // ====================================================================

    void testFileAddedSignalOnAdd() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path = createTempFile(temp, "a.ost");

        sak::OstConverterController ctrl;
        QSignalSpy spy(&ctrl, &sak::OstConverterController::fileAdded);

        ctrl.addFile(path);
        QVERIFY(spy.count() >= 1);
    }

    void testFileRemovedSignalOnRemove() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path = createTempFile(temp, "a.ost");

        sak::OstConverterController ctrl;
        ctrl.addFile(path);

        QSignalSpy spy(&ctrl, &sak::OstConverterController::fileRemoved);
        ctrl.removeFile(0);

        QVERIFY(spy.count() >= 1);
    }

    void testQueueClearedSignalOnClear() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path1 = createTempFile(temp, "a.ost");
        QString path2 = createTempFile(temp, "b.ost");

        sak::OstConverterController ctrl;
        ctrl.addFile(path1);
        ctrl.addFile(path2);

        QSignalSpy spy(&ctrl, &sak::OstConverterController::queueCleared);
        ctrl.clearQueue();

        QVERIFY(spy.count() >= 1);
    }

    // ====================================================================
    // Cancel When Not Running
    // ====================================================================

    void testCancelWhenNotRunning() {
        sak::OstConverterController ctrl;
        ctrl.cancelAll();  // Should not crash
        QVERIFY(!ctrl.isRunning());
    }

    // ====================================================================
    // B7-03: cancelling a running batch must stop workers safely -- graceful
    // quit()+wait() (the worker honors the cancel flag), NEVER terminate() --
    // finalize the batch, and leave the controller destructible without abort.
    // ====================================================================

    void testCancelDuringConversionStopsSafely() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());

        sak::OstConverterController ctrl;
        for (int i = 0; i < 4; ++i) {
            ctrl.addFile(createTempFile(temp, QStringLiteral("f%1.ost").arg(i)));
        }

        QSignalSpy complete_spy(&ctrl, &sak::OstConverterController::allConversionsComplete);

        sak::OstConversionConfig config;
        config.output_directory = temp.path();

        config.max_threads = 4;  // spin up several worker threads at once
        ctrl.startConversion(config);

        // Cancel immediately. cancelAll() cancels each worker, joins its thread with a graceful
        // quit()+wait() (no terminate()), then finalizes the batch synchronously.
        ctrl.cancelAll();

        QVERIFY(!ctrl.isRunning());
        QVERIFY2(complete_spy.count() >= 1,
                 "cancelAll must finalize the batch (completion signal)");
        // ctrl destructs at end of scope -> cancelAll() again on no active workers: must not abort.
    }

    // ====================================================================
    // P05-40: a zero/negative max_threads must clamp to one worker, not
    // leave the batch permanently stuck (no worker, no completion signal).
    // ====================================================================

    // ====================================================================
    // FB (review 2): a run with any failed item OR recorded error is not a
    // clean conversion -- even when some items converted, it must NOT be
    // classified Complete. classifyOutcome centralizes that fail-closed rule.
    // ====================================================================

    void testClassifyCleanRunIsComplete() {
        sak::OstConversionResult result;
        result.items_converted = 10;
        result.items_failed = 0;
        QCOMPARE(sak::OstConverterController::classifyOutcome(result),
                 sak::OstConversionJob::Status::Complete);
    }

    void testClassifyPartialRunIsFailed() {
        // 9 converted but 1 failed -> the whole job is a failure, not Complete.
        sak::OstConversionResult result;
        result.items_converted = 9;
        result.items_failed = 1;
        QCOMPARE(sak::OstConverterController::classifyOutcome(result),
                 sak::OstConversionJob::Status::Failed);
    }

    void testClassifyErroredRunIsFailed() {
        // Items converted but an error was recorded (e.g. a dropped attachment).
        sak::OstConversionResult result;
        result.items_converted = 5;
        result.items_failed = 0;
        result.errors.append(QStringLiteral("Attachment 'a.pdf' dropped (read failed)"));
        QCOMPARE(sak::OstConverterController::classifyOutcome(result),
                 sak::OstConversionJob::Status::Failed);
    }

    void testClassifySourceOpenFailureIsFailed() {
        // Nothing converted, no per-item failures, but a fatal error was recorded.
        sak::OstConversionResult result;
        result.errors.append(QStringLiteral("Failed to open file"));
        QCOMPARE(sak::OstConverterController::classifyOutcome(result),
                 sak::OstConversionJob::Status::Failed);
    }

    void testClassifyEmptyCleanRunIsComplete() {
        // A validly empty mailbox (no items, no failures, no errors) is Complete.
        sak::OstConversionResult result;
        QCOMPARE(sak::OstConverterController::classifyOutcome(result),
                 sak::OstConversionJob::Status::Complete);
    }

    void testZeroThreadsDoesNotWedgeBatch() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path = createTempFile(temp, "email.ost");

        sak::OstConverterController ctrl;
        ctrl.addFile(path);

        QSignalSpy complete_spy(&ctrl, &sak::OstConverterController::allConversionsComplete);

        sak::OstConversionConfig config;
        config.output_directory = temp.path();

        config.max_threads = 0;  // would launch zero workers without the clamp

        ctrl.startConversion(config);

        constexpr int kBatchWaitMs = 15'000;
        // Assert the absolute count rather than spy.wait(). The batch is finalized from a
        // worker thread's completion signal, so the emission can be recorded before the main
        // thread reaches this line, and wait() only reports emissions that arrive after it is
        // entered -- it would sit out the full 15s waiting for a second one.
        QTRY_VERIFY2_WITH_TIMEOUT(complete_spy.count() >= 1,
                                  "batch wedged: allConversionsComplete never fired",
                                  kBatchWaitMs);
        QVERIFY(!ctrl.isRunning());
    }

    // R5-G20-2: re-running a queue that already finished used to report a bogus empty
    // batch. startConversion reset the index, the batch result and the worker list, but not
    // the per-job statuses -- every job was still Complete/Failed from the previous run, so
    // startNextFile found nothing Queued, saw no active workers, and finalized immediately.
    // The controller emitted conversionStarted(N) and then "0/N files succeeded" for files
    // it had converted perfectly moments earlier.
    void testRerunningFinishedQueueConvertsAgain() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = createTempFile(temp, "email.ost");

        sak::OstConverterController ctrl;
        ctrl.addFile(path);

        QSignalSpy complete_spy(&ctrl, &sak::OstConverterController::allConversionsComplete);

        sak::OstConversionConfig config;
        config.output_directory = temp.path();

        constexpr int kBatchWaitMs = 15'000;

        ctrl.startConversion(config);
        QTRY_COMPARE_WITH_TIMEOUT(complete_spy.count(), 1, kBatchWaitMs);
        const auto first = complete_spy.at(0).at(0).value<sak::OstConversionBatchResult>();
        QCOMPARE(first.files_total, 1);

        // Second run over the same queue must attempt the file again, not report an empty
        // batch. Every job is back to Queued, so the totals must match the first run.
        ctrl.startConversion(config);
        QTRY_COMPARE_WITH_TIMEOUT(complete_spy.count(), 2, kBatchWaitMs);
        const auto second = complete_spy.at(1).at(0).value<sak::OstConversionBatchResult>();

        QCOMPARE(second.files_total, first.files_total);
        QCOMPARE(second.files_succeeded, first.files_succeeded);
        QCOMPARE(second.files_failed, first.files_failed);
        QVERIFY(!ctrl.isRunning());

        // Every file is accounted for exactly once, whichever way it went. This is the
        // invariant that a cancelled run used to break: cancelled jobs were flipped to
        // Cancelled and counted nowhere, so the sum came out short of the total.
        QCOMPARE(second.files_succeeded + second.files_failed + second.files_cancelled,
                 second.files_total);
    }

    // ====================================================================
    // Deleted-item recovery honesty
    // ====================================================================

    void testRecoveryReliabilityDefaultsToComplete() {
        // A fresh result claims a complete recovery; only a scan that reported trouble may
        // clear it. Reliable flags must therefore leave the result untouched.
        sak::OstConversionResult result;
        QVERIFY(result.recovery_complete);

        sak::OstConversionWorker::recordRecoveryReliability(
            /*recoverable_reliable=*/true,
            /*orphan_reliable=*/true,
            /*orphans_scanned=*/true,
            result);
        QVERIFY(result.recovery_complete);
        QVERIFY(result.errors.isEmpty());
        QCOMPARE(sak::OstConverterController::classifyOutcome(result),
                 sak::OstConversionJob::Status::Complete);
    }

    void testTruncatedRecoverableScanIsNotACleanConversion() {
        // A read error inside the Recoverable Items hierarchy leaves recovered items OUT of the
        // output. items_recovered alone cannot say that, so the run must not be reported clean:
        // recovery_complete drops and an error is recorded, which demotes the job to Failed.
        sak::OstConversionResult result;
        result.items_converted = 12;
        result.items_recovered = 3;

        sak::OstConversionWorker::recordRecoveryReliability(
            /*recoverable_reliable=*/false,
            /*orphan_reliable=*/true,
            /*orphans_scanned=*/true,
            result);

        QVERIFY(!result.recovery_complete);
        QCOMPARE(result.errors.size(), qsizetype(1));
        QVERIFY(result.errors.first().contains(QStringLiteral("INCOMPLETE")));
        QCOMPARE(sak::OstConverterController::classifyOutcome(result),
                 sak::OstConversionJob::Status::Failed);
    }

    void testUnreliableOrphanScanOnlyCountsWhenOrphansWereScanned() {
        // Shallow recovery never runs the orphan pass, so the scanner's orphan flag says nothing
        // about that run and must not manufacture a failure.
        sak::OstConversionResult shallow;
        sak::OstConversionWorker::recordRecoveryReliability(
            /*recoverable_reliable=*/true,
            /*orphan_reliable=*/false,
            /*orphans_scanned=*/false,
            shallow);
        QVERIFY(shallow.recovery_complete);
        QVERIFY(shallow.errors.isEmpty());

        // Deep recovery DID run it, so the same flag is now a real truncation.
        sak::OstConversionResult deep;
        sak::OstConversionWorker::recordRecoveryReliability(
            /*recoverable_reliable=*/true,
            /*orphan_reliable=*/false,
            /*orphans_scanned=*/true,
            deep);
        QVERIFY(!deep.recovery_complete);
        QCOMPARE(deep.errors.size(), qsizetype(1));
        QVERIFY(deep.errors.first().contains(QStringLiteral("orphaned-node")));
        QCOMPARE(sak::OstConverterController::classifyOutcome(deep),
                 sak::OstConversionJob::Status::Failed);
    }
};

QTEST_MAIN(TestOstConverterController)
#include "test_ost_converter_controller.moc"

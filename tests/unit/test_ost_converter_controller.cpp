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
        QCOMPARE(spy.count(), 1);  // one add -> exactly one fileAdded (not >= 1, which hides a dup)
    }

    void testAddMultipleFiles() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path1 = createTempFile(temp, "one.ost");
        QString path2 = createTempFile(temp, "two.pst");

        sak::OstConverterController ctrl;

        ctrl.addFile(path1);
        ctrl.addFile(path2);

        // Size alone is blind to ORDER, and order is load-bearing: the queue table appends one
        // row per fileAdded and then hands that ROW back as a queue index -- removeFile(row)
        // and queue().at(file_index) for the "Converting: %1" line -- so a queue that is not
        // append-ordered removes and labels the wrong file.
        QCOMPARE(ctrl.queue().size(), 2);
        QCOMPARE(ctrl.queue().at(0).source_path, path1);
        QCOMPARE(ctrl.queue().at(1).source_path, path2);

        QCOMPARE(ctrl.queue().size(), 2);
    }

    void testAddDuplicateFileRejected() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path = createTempFile(temp, "email.ost");

        sak::OstConverterController ctrl;
        QSignalSpy added_spy(&ctrl, &sak::OstConverterController::fileAdded);
        QSignalSpy status_spy(&ctrl, &sak::OstConverterController::statusMessage);

        ctrl.addFile(path);
        ctrl.addFile(path);

        // A queue still holding one entry does not say the second add was recognised as a
        // DUPLICATE and reported as one -- addFile's other refuse path (a path that does not
        // exist / is not a file) leaves the size at 1 too, and so does a dup branch that silently
        // returns or reuses the "Added: %1" text. The status line is the only feedback a re-added
        // file gets, so pin the dedup guard's own outcome: exactly one fileAdded (a second would
        // paint a phantom row in the queue table) and the duplicate reported by name.
        QCOMPARE(ctrl.queue().size(), 1);
        QCOMPARE(added_spy.count(), 1);
        QCOMPARE(status_spy.count(), 2);
        QCOMPARE(status_spy.at(0).at(0).toString(), QStringLiteral("Added: email.ost"));
        QCOMPARE(status_spy.at(1).at(0).toString(),
                 QStringLiteral("File already in queue: email.ost"));
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

        // Size alone cannot tell removeAt(index) from removeLast() or removeAt(size-1-index):
        // every one of them leaves a single entry. Pin WHICH entry survived -- index 0 was
        // dropped, so the remaining job is the second file.
        QCOMPARE(ctrl.queue().size(), 1);
        QCOMPARE(ctrl.queue().at(0).source_path, path2);
    }

    void testRemoveInvalidIndex() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path = createTempFile(temp, "one.ost");

        sak::OstConverterController ctrl;
        QSignalSpy removed_spy(&ctrl, &sak::OstConverterController::fileRemoved);

        ctrl.addFile(path);
        ctrl.removeFile(5);   // Past the end
        ctrl.removeFile(1);   // First index past the last valid one
        ctrl.removeFile(-1);  // Before the start -- the OTHER arm of the same bounds guard

        // Only the high arm was exercised before, so an implementation that dropped the
        // `index < 0` half stayed green while removeFile(-1) reached QVector::removeAt(-1).
        // A refused remove must also stay silent: telling the view a row went away while the
        // queue is untouched desynchronises the table.
        QCOMPARE(ctrl.queue().size(), 1);  // Unchanged
        QCOMPARE(ctrl.queue().at(0).source_path, path);
        QCOMPARE(removed_spy.count(), 0);
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
        QCOMPARE(jobs[0].source_path, path);  // stored verbatim; endsWith only pinned the suffix
        QCOMPARE(jobs[0].status, sak::OstConversionJob::Status::Queued);
        // The queue row and the per-file status line are built from the SIBLING fields, not
        // from source_path: the table shows display_name and formatBytes(file_size_bytes), and
        // "Converting: %1" reads display_name straight off the stored job. Leaving them default
        // (empty name, 0 bytes) passes a source_path/status-only check. createTempFile writes
        // exactly the 17 bytes of "dummy ost content".
        QCOMPARE(jobs[0].display_name, QStringLiteral("email.ost"));
        QCOMPARE(jobs[0].file_size_bytes, qint64(17));
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
        QSignalSpy started_spy(&ctrl, &sak::OstConverterController::conversionStarted);
        QSignalSpy complete_spy(&ctrl, &sak::OstConverterController::allConversionsComplete);

        sak::OstConversionConfig config;
        config.output_directory = QStringLiteral("C:/output");

        ctrl.startConversion(config);

        // !isRunning() is a one-flag report on a two-condition refuser, and it is just as true of
        // a start that announced itself before refusing (the panel switches to converting state on
        // conversionStarted and never switches back) or of one that "finalized" the empty batch (a
        // bogus "0/0 files succeeded" plus an HTML report for a run that never happened). A
        // refused start must be completely silent.
        QVERIFY(!ctrl.isRunning());
        QCOMPARE(started_spy.count(), 0);
        QCOMPARE(complete_spy.count(), 0);
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
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), 0);  // first append -> index 0
        // The signal's SECOND argument is what the queue table renders -- onFileAdded ignores
        // the index and calls updateQueueRow with the PAYLOAD, never with controller.queue() --
        // so emitting the job before its fields are filled blanks the row while the index
        // assertion above stays green.
        const auto emitted = spy.at(0).at(1).value<sak::OstConversionJob>();
        QCOMPARE(emitted.source_path, path);
        QCOMPARE(emitted.display_name, QStringLiteral("a.ost"));
        QCOMPARE(emitted.file_size_bytes, qint64(17));
        QCOMPARE(emitted.status, sak::OstConversionJob::Status::Queued);
    }

    void testFileRemovedSignalOnRemove() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path = createTempFile(temp, "a.ost");

        sak::OstConverterController ctrl;
        ctrl.addFile(path);

        QSignalSpy spy(&ctrl, &sak::OstConverterController::fileRemoved);
        ctrl.removeFile(0);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), 0);  // removed index 0
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

        QCOMPARE(spy.count(), 1);
    }

    // ====================================================================
    // Cancel When Not Running
    // ====================================================================

    void testCancelWhenNotRunning() {
        sak::OstConverterController ctrl;
        QSignalSpy complete_spy(&ctrl, &sak::OstConverterController::allConversionsComplete);
        QSignalSpy status_spy(&ctrl, &sak::OstConverterController::statusMessage);

        ctrl.cancelAll();  // Should not crash

        // isRunning() was ALREADY false before the call, so it holds no matter what cancelAll()
        // did. The contract that can actually break is the `if (m_running)` gate: with no batch
        // in flight nothing may be finalized -- no allConversionsComplete carrying an empty
        // batch result, no "Conversion complete: 0/0 files succeeded" status line, and (after a
        // finished run, via the destructor's own cancelAll) no duplicate HTML report.
        QVERIFY(!ctrl.isRunning());
        QCOMPARE(complete_spy.count(), 0);
        QCOMPARE(status_spy.count(), 0);
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
        QCOMPARE(complete_spy.count(), 1);  // cancelAll finalizes the batch exactly once

        // Stopping is not enough: the cancel must be ACCOUNTED. cancelAll() flips every job
        // still Queued/Converting to Cancelled and COUNTS it; dropping the counter (the bug
        // files_cancelled exists for) leaves succeeded + failed + cancelled short of
        // files_total and the completion line unexplained. Nothing can have succeeded here:
        // conversionFinished is a QueuedConnection and the main thread runs no event loop
        // between startConversion() and this point, so no worker result was delivered.
        const auto batch = complete_spy.at(0).at(0).value<sak::OstConversionBatchResult>();
        QCOMPARE(batch.files_total, 4);
        QCOMPARE(batch.files_succeeded, 0);
        QCOMPARE(batch.files_succeeded + batch.files_failed + batch.files_cancelled,
                 batch.files_total);
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

        // The same clean counts with the cancel flag set are NOT a clean run: convert()
        // returned early, so the counts are partial. No other case in this file sets
        // `cancelled`, so deleting that first branch of classifyOutcome -- which would count an
        // abandoned file into files_succeeded -- otherwise stays green.
        result.cancelled = true;
        QCOMPARE(sak::OstConverterController::classifyOutcome(result),
                 sak::OstConversionJob::Status::Cancelled);
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

        // ...but a NEGATIVE counter cannot occur on a real run and must fail closed. Each is
        // its own arm of the guard and none is exercised anywhere else in this file, so
        // reverting to `items_failed > 0` -- or dropping the three negative-counter arms --
        // leaves a corrupt result reported as a clean conversion.
        sak::OstConversionResult negative_failed;
        negative_failed.items_failed = -1;
        QCOMPARE(sak::OstConverterController::classifyOutcome(negative_failed),
                 sak::OstConversionJob::Status::Failed);

        sak::OstConversionResult negative_converted;
        negative_converted.items_converted = -1;
        QCOMPARE(sak::OstConverterController::classifyOutcome(negative_converted),
                 sak::OstConversionJob::Status::Failed);

        sak::OstConversionResult negative_recovered;
        negative_recovered.items_recovered = -1;
        QCOMPARE(sak::OstConverterController::classifyOutcome(negative_recovered),
                 sak::OstConversionJob::Status::Failed);

        sak::OstConversionResult negative_bytes;
        negative_bytes.bytes_written = -1;
        QCOMPARE(sak::OstConverterController::classifyOutcome(negative_bytes),
                 sak::OstConversionJob::Status::Failed);
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
        QTRY_COMPARE_WITH_TIMEOUT(complete_spy.count(), 1, kBatchWaitMs);
        QVERIFY(!ctrl.isRunning());

        // A completion signal alone does not prove a worker ran: "if threads_to_launch == 0,
        // finalizeBatch()" un-wedges the batch just as well and converts nothing. The clamp
        // must launch one real worker, so the queued file has to be ATTEMPTED -- it is a
        // 17-byte dummy, so PstParser::open() fails and the attempt is recorded as one failed
        // file carrying the opener's own reason (never silently "succeeded").
        const auto batch = complete_spy.at(0).at(0).value<sak::OstConversionBatchResult>();
        QCOMPARE(batch.files_total, 1);
        QCOMPARE(batch.files_failed, 1);
        QCOMPARE(batch.files_succeeded, 0);
        QCOMPARE(batch.files_cancelled, 0);
        QCOMPARE(batch.file_results.size(), qsizetype(1));
        QCOMPARE(batch.file_results.at(0).errors.size(), qsizetype(1));
        QCOMPARE(batch.file_results.at(0).errors.at(0),
                 QStringLiteral("Failed to open file: ") + path);
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

        // Those three are symmetric: two runs that were BOTH wrong the same way satisfy all of
        // them. Anchor the symmetry with run 1's ABSOLUTE outcome. The queued file is a 17-byte
        // dummy, so PstParser::open() fails and each run must record it as one ATTEMPTED and
        // failed file -- never skipped, never silently "succeeded" -- with the opener's own
        // reason, and must write that verdict back onto the queued job.
        QCOMPARE(first.files_failed, 1);
        QCOMPARE(first.files_succeeded, 0);
        QCOMPARE(first.files_cancelled, 0);
        QCOMPARE(second.file_results.size(), qsizetype(1));
        QCOMPARE(second.file_results.at(0).errors.size(), qsizetype(1));
        QCOMPARE(second.file_results.at(0).errors.at(0),
                 QStringLiteral("Failed to open file: ") + path);
        QCOMPARE(ctrl.queue().at(0).status, sak::OstConversionJob::Status::Failed);
        QVERIFY(!ctrl.isRunning());
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
        // contains("INCOMPLETE") cannot tell the two messages apart -- BOTH branches say
        // INCOMPLETE -- so a recoverable arm that appended the ORPHAN text stays green while
        // the user is told the wrong scan came up short.
        QCOMPARE(result.errors.first(),
                 QStringLiteral("Deleted-item recovery is INCOMPLETE: a read error truncated "
                                "the Recoverable Items scan, so recoverable items are missing "
                                "from the output. The recovered count is a floor, not a total."));
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
        const QString kRecoverableTruncated = QStringLiteral(
            "Deleted-item recovery is INCOMPLETE: a read error truncated the Recoverable Items "
            "scan, so recoverable items are missing from the output. The recovered count is a "
            "floor, not a total.");
        const QString kOrphanTruncated = QStringLiteral(
            "Deleted-item recovery is INCOMPLETE: the orphaned-node scan could not enumerate "
            "every candidate, so orphaned items are missing from the output. The recovered "
            "count is a floor, not a total.");

        QVERIFY(!deep.recovery_complete);
        QCOMPARE(deep.errors.size(), qsizetype(1));
        QCOMPARE(deep.errors.first(), kOrphanTruncated);
        QCOMPARE(sak::OstConverterController::classifyOutcome(deep),
                 sak::OstConversionJob::Status::Failed);

        // The two reliability checks are independent ifs, not an if/else: when BOTH scans were
        // truncated the user must be told about both, recoverable first. An else-if still reports
        // one INCOMPLETE error and hides half of what is missing.
        sak::OstConversionResult both;
        sak::OstConversionWorker::recordRecoveryReliability(
            /*recoverable_reliable=*/false,
            /*orphan_reliable=*/false,
            /*orphans_scanned=*/true,
            both);
        QVERIFY(!both.recovery_complete);
        QCOMPARE(both.errors.size(), qsizetype(2));
        QCOMPARE(both.errors.at(0), kRecoverableTruncated);
        QCOMPARE(both.errors.at(1), kOrphanTruncated);
    }
};

QTEST_MAIN(TestOstConverterController)
#include "test_ost_converter_controller.moc"

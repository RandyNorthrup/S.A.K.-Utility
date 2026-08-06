// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_app_installation_worker.cpp
/// @brief Unit tests for AppInstallationWorker job filtering, stats, and state management

#include "sak/app_installation_worker.h"
#include "sak/chocolatey_manager.h"
#include "sak/migration_report.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

class AppInstallationWorkerTests : public QObject {
    Q_OBJECT

private:
    /// Create a MigrationReport with configurable entries for testing
    static std::shared_ptr<sak::MigrationReport> createTestReport(int selectedAvailable,
                                                                  int selectedUnavailable,
                                                                  int unselected,
                                                                  int noPackage) {
        auto report = std::make_shared<sak::MigrationReport>();

        // Selected + available + has choco package -> should become jobs
        for (int i = 0; i < selectedAvailable; ++i) {
            sak::MigrationReport::MigrationEntry entry;
            entry.app_name = QString("AvailableApp%1").arg(i);
            entry.choco_package = QString("available-app%1").arg(i);
            entry.selected = true;
            entry.available = true;
            entry.status = "pending";
            report->addEntry(entry);
        }

        // Selected but NOT available -> should be skipped
        for (int i = 0; i < selectedUnavailable; ++i) {
            sak::MigrationReport::MigrationEntry entry;
            entry.app_name = QString("UnavailableApp%1").arg(i);
            entry.choco_package = QString("unavailable-app%1").arg(i);
            entry.selected = true;
            entry.available = false;
            entry.status = "pending";
            report->addEntry(entry);
        }

        // Not selected -> should be skipped
        for (int i = 0; i < unselected; ++i) {
            sak::MigrationReport::MigrationEntry entry;
            entry.app_name = QString("UnselectedApp%1").arg(i);
            entry.choco_package = QString("unselected-app%1").arg(i);
            entry.selected = false;
            entry.available = true;
            entry.status = "pending";
            report->addEntry(entry);
        }

        // No choco package -> should be skipped
        for (int i = 0; i < noPackage; ++i) {
            sak::MigrationReport::MigrationEntry entry;
            entry.app_name = QString("NoPackageApp%1").arg(i);
            entry.choco_package = "";  // Empty -> skip
            entry.selected = true;
            entry.available = true;
            entry.status = "pending";
            report->addEntry(entry);
        }

        return report;
    }

private Q_SLOTS:
    /// startMigration should only create jobs for selected+available entries with a choco package
    void jobFilteringOnlySelectedAvailable() {
        auto chocoMgr = std::make_shared<sak::ChocolateyManager>();
        sak::AppInstallationWorker worker(chocoMgr);

        auto report = createTestReport(3, 2, 4, 1);  // 3 valid, 7 invalid
        QCOMPARE(static_cast<int>(report->getEntries().size()), 10);

        // startMigration returns the number of jobs created
        int jobCount = worker.startMigration(report, 0);
        // Only 3 selected+available entries with a choco package
        QCOMPARE(jobCount, 3);

        // Cancel immediately so processQueue exits (maxConcurrent=0 prevents actual installs)
        worker.cancel();

        // Verify job list
        auto jobs = worker.getJobs();
        QCOMPARE(jobs.size(), 3);
        for (const auto& job : jobs) {
            QVERIFY(job.appName.startsWith("AvailableApp"));
            QVERIFY(!job.packageId.isEmpty());
        }
    }

    /// startMigration with no valid entries should return 0
    void noValidEntriesReturnsZero() {
        auto chocoMgr = std::make_shared<sak::ChocolateyManager>();
        sak::AppInstallationWorker worker(chocoMgr);

        auto report = createTestReport(0, 3, 2, 1);
        int jobCount = worker.startMigration(report, 0);
        QCOMPARE(jobCount, 0);
    }

    /// Initial state should be not-running, not-paused
    void initialStateCorrect() {
        auto chocoMgr = std::make_shared<sak::ChocolateyManager>();
        sak::AppInstallationWorker worker(chocoMgr);

        QVERIFY(!worker.isRunning());
        QVERIFY(!worker.isPaused());

        auto stats = worker.getStats();
        QCOMPARE(stats.total, 0);
        QCOMPARE(stats.pending, 0);
        QCOMPARE(stats.success, 0);
        QCOMPARE(stats.failed, 0);
    }

    /// getStats should reflect job counts correctly after startMigration
    void statsReflectJobCounts() {
        auto chocoMgr = std::make_shared<sak::ChocolateyManager>();
        sak::AppInstallationWorker worker(chocoMgr);

        auto report = createTestReport(5, 0, 0, 0);
        worker.startMigration(report, 0);

        auto stats = worker.getStats();
        QCOMPARE(stats.total, 5);
        // Was QVERIFY(pending + queued + cancelled >= 0), a tautology: all three are
        // non-negative counters, so the sum can never be negative and the assertion could
        // not fail. This asserts a real invariant instead - the worker can never report
        // more finished jobs than it was given - and it holds at any point in the run, so
        // it does not depend on how far this fast-failing migration has got.
        QVERIFY2(stats.success + stats.failed <= stats.total,
                 "more jobs reported finished than were ever queued");

        worker.cancel();
    }

    /// cancel should emit migrationCancelled and eventually stop the worker
    void cancelMarksJobsCancelled() {
        auto chocoMgr = std::make_shared<sak::ChocolateyManager>();
        sak::AppInstallationWorker worker(chocoMgr);

        QSignalSpy cancelSpy(&worker, &sak::AppInstallationWorker::migrationCancelled);

        auto report = createTestReport(4, 0, 0, 0);
        worker.startMigration(report, 1);

        // Cancel immediately -- some jobs may already be in-flight
        worker.cancel();

        QCOMPARE(cancelSpy.count(), 1);

        // Worker destructor will wait for processQueue thread to finish
    }

    /// pause/resume toggle state correctly
    void pauseResumeToggles() {
        auto chocoMgr = std::make_shared<sak::ChocolateyManager>();
        sak::AppInstallationWorker worker(chocoMgr);

        QSignalSpy pauseSpy(&worker, &sak::AppInstallationWorker::migrationPaused);
        QSignalSpy resumeSpy(&worker, &sak::AppInstallationWorker::migrationResumed);

        // Enough queued work that processQueue cannot drain it before pause() is
        // reached. With the previous two jobs this test RACED: the
        // ChocolateyManager is not initialised, so every job fails instantly, and
        // the worker frequently finished before pause() took the mutex. pause()
        // then correctly no-opped on a stopped worker and isPaused() was false.
        // It passed in Release and failed about half the time in Debug, which
        // means it had been passing on timing rather than on behaviour.
        // Concurrency 1 keeps the queue serial so the window stays open. 200 jobs
        // is roughly a 20ms window against the microseconds it takes to reach
        // pause() - a margin of about a thousand - while keeping the test fast.
        // An earlier attempt used 2000, which bought no extra safety and made the
        // binary noticeably slower for nothing.
        auto report = createTestReport(200, 0, 0, 0);
        worker.startMigration(report, 1);

        // The precondition this test depends on, stated explicitly rather than
        // assumed: pause() only means anything while the worker is running. If
        // this ever fails the test is racing again and says so, instead of
        // quietly asserting a state that no longer exists.
        QVERIFY2(worker.isRunning(),
                 "worker finished before pause() was called; the queued job set is no longer "
                 "large enough to hold the window open, so this test would be checking the "
                 "wrong state");

        worker.pause();
        QVERIFY(worker.isPaused());
        QCOMPARE(pauseSpy.count(), 1);

        worker.resume();
        QVERIFY(!worker.isPaused());
        QCOMPARE(resumeSpy.count(), 1);

        worker.cancel();
    }

    /// Version lock fields should be captured in jobs
    void versionLockCaptured() {
        auto chocoMgr = std::make_shared<sak::ChocolateyManager>();
        sak::AppInstallationWorker worker(chocoMgr);

        auto report = std::make_shared<sak::MigrationReport>();
        sak::MigrationReport::MigrationEntry entry;
        entry.app_name = "LockedApp";
        entry.choco_package = "locked-app";
        entry.selected = true;
        entry.available = true;
        entry.version_lock = true;
        entry.locked_version = "1.2.3";
        report->addEntry(entry);

        int jobCount = worker.startMigration(report, 0);
        QCOMPARE(jobCount, 1);

        auto jobs = worker.getJobs();
        QCOMPARE(jobs.size(), 1);
        QCOMPARE(jobs[0].packageId, "locked-app");
        QCOMPARE(jobs[0].version, "1.2.3");

        worker.cancel();
    }

    /// migrationStarted signal should be emitted with correct count
    void migrationStartedSignalEmitted() {
        auto chocoMgr = std::make_shared<sak::ChocolateyManager>();
        sak::AppInstallationWorker worker(chocoMgr);

        QSignalSpy startSpy(&worker, &sak::AppInstallationWorker::migrationStarted);

        auto report = createTestReport(3, 1, 2, 0);
        worker.startMigration(report, 0);

        // QSignalSpy connects with Qt::DirectConnection, so a signal emitted before
        // the main thread reaches wait() is already recorded and wait() then blocks
        // for a SECOND emission that never comes. Check-then-wait does not close
        // that hole: the emission can land between isEmpty() and wait(). Today
        // migrationStarted is emitted synchronously inside startMigration(), which
        // makes the old branch dead code that could only ever burn the timeout and
        // fail. QTRY_COMPARE succeeds at once on an already-recorded signal and
        // still polls the original 1000ms if the emit ever moves off this thread.
        QTRY_COMPARE_WITH_TIMEOUT(startSpy.count(), 1, 1000);
        QCOMPARE(startSpy.at(0).at(0).toInt(), 3);  // 3 valid jobs

        worker.cancel();
    }

    /// boundConcurrency clamps to [0, cap] (B10-26): 0 stays the dry-run mode,
    /// negatives clamp to 0, absurd values clamp to the cap so scheduling never
    /// stalls on an out-of-range request.
    void boundConcurrencyClampsRange() {
        using W = sak::AppInstallationWorker;
        QCOMPARE(W::boundConcurrency(0), 0);
        QCOMPARE(W::boundConcurrency(-5), 0);
        QCOMPARE(W::boundConcurrency(1), 1);
        QCOMPARE(W::boundConcurrency(3), 3);
        QCOMPARE(W::boundConcurrency(1000), W::kMaxInstallConcurrency);
        QCOMPARE(W::boundConcurrency(W::kMaxInstallConcurrency), W::kMaxInstallConcurrency);
    }

    /// A dry-run (maxConcurrent==0) must finish on its own without a cancel --
    /// previously it busy-polled forever (B10-26 stall).
    void dryRunFinishesWithoutCancel() {
        auto chocoMgr = std::make_shared<sak::ChocolateyManager>();
        sak::AppInstallationWorker worker(chocoMgr);

        QSignalSpy doneSpy(&worker, &sak::AppInstallationWorker::migrationCompleted);
        auto report = createTestReport(3, 1, 2, 0);
        worker.startMigration(report, 0);  // dry-run: queue jobs, launch none

        // No cancel() here: the loop must reach migrationCompleted on its own.
        //
        // QTRY_COMPARE, not doneSpy.wait(). QSignalSpy connects with
        // Qt::DirectConnection, so the WORKER thread records the signal the moment
        // it is emitted, and wait() returns size() > origCount - it waits for a
        // signal it has not already seen. A dry run finishes almost instantly, so
        // whenever the worker beat the main thread to wait(), the spy already held
        // the signal and wait() blocked for a second one that never comes. That
        // made this test fail about 2 times in 300 while the code under test was
        // working perfectly. QTRY_COMPARE succeeds immediately if the signal has
        // already arrived, and polls with an event loop if it has not.
        QTRY_COMPARE(doneSpy.count(), 1);
    }

    /// migrationSkipReason (CODEX REVIEW 3 #6): a SELECTED entry that cannot be
    /// migrated must yield an explicit reason instead of being silently dropped;
    /// an installable entry yields empty.
    void migrationSkipReasonClassifies() {
        using W = sak::AppInstallationWorker;
        sak::MigrationReport::MigrationEntry ok;
        ok.choco_package = "googlechrome";
        ok.available = true;
        QVERIFY(W::migrationSkipReason(ok).isEmpty());

        sak::MigrationReport::MigrationEntry noPkg = ok;
        noPkg.choco_package = "";
        QVERIFY(!W::migrationSkipReason(noPkg).isEmpty());

        sak::MigrationReport::MigrationEntry unavail = ok;
        unavail.available = false;
        QVERIFY(!W::migrationSkipReason(unavail).isEmpty());

        // Version lock with no locked version would silently install latest.
        sak::MigrationReport::MigrationEntry lockNoVer = ok;
        lockNoVer.version_lock = true;
        lockNoVer.locked_version = "";
        QVERIFY(!W::migrationSkipReason(lockNoVer).isEmpty());

        sak::MigrationReport::MigrationEntry lockVer = lockNoVer;
        lockVer.locked_version = "1.2.3";
        QVERIFY(W::migrationSkipReason(lockVer).isEmpty());
    }

    /// Skipped SELECTED entries must be recorded on the report (CODEX REVIEW 3 #6),
    /// not silently dropped.
    void skippedSelectedEntriesRecordedOnReport() {
        auto chocoMgr = std::make_shared<sak::ChocolateyManager>();
        sak::AppInstallationWorker worker(chocoMgr);

        auto report = createTestReport(1, 1, 0, 1);  // 1 valid, 1 unavailable, 1 no-package
        worker.startMigration(report, 0);

        int skipped = 0;
        const auto& entries = report->getEntries();
        for (const auto& e : entries) {
            if (e.status == "skipped") {
                ++skipped;
                QVERIFY(!e.error_message.isEmpty());
            }
        }
        QCOMPARE(skipped, 2);  // the unavailable + the no-package selected entries

        worker.cancel();
    }

    /// nameIndicatesApp (CODEX REVIEW 3 #7): whole-word match so an unrelated app
    /// whose name merely embeds the target token cannot certify an install.
    void nameIndicatesAppWholeWord() {
        using W = sak::AppInstallationWorker;
        QVERIFY(W::nameIndicatesApp("Google Chrome (64-bit)", "Google Chrome"));
        QVERIFY(W::nameIndicatesApp("notepad", "Notepad"));
        // Punctuation-tailed ids must still match (alnum lookarounds, not \\b).
        QVERIFY(W::nameIndicatesApp("Notepad++ (64-bit)", "Notepad++"));
        // Embedded-token lookalikes must NOT match.
        QVERIFY(!W::nameIndicatesApp("Notepadster Deluxe", "Notepad"));
        QVERIFY(!W::nameIndicatesApp("SuperGitHubTool", "Git"));
        // Empty operands fail closed.
        QVERIFY(!W::nameIndicatesApp("", "Notepad"));
        QVERIFY(!W::nameIndicatesApp("Notepad", ""));
    }

    /// A null ChocolateyManager must never be dereferenced (CODEX REVIEW 3 #11):
    /// construction and startMigration fail closed instead of crashing.
    void nullChocoManagerFailsClosed() {
        std::shared_ptr<sak::ChocolateyManager> nullMgr;
        sak::AppInstallationWorker worker(nullMgr);  // must not crash on connect()

        auto report = createTestReport(2, 0, 0, 0);
        const int jobCount = worker.startMigration(report, 0);
        QCOMPARE(jobCount, 0);  // refuses to start with a null manager
        QVERIFY(!worker.isRunning());
    }
};

QTEST_GUILESS_MAIN(AppInstallationWorkerTests)
#include "test_app_installation_worker.moc"

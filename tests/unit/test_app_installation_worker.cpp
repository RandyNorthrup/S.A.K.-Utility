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
        for (int i = 0; i < jobs.size(); ++i) {
            QCOMPARE(jobs[i].appName, QString("AvailableApp%1").arg(i));
            QCOMPARE(jobs[i].packageId, QString("available-app%1").arg(i));
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

        // The not-running arm of the pause/resume/cancel guards
        // (app_installation_worker.cpp:326, 337, 353) has no test anywhere, yet the
        // cancel() one fires in production on a real path every time: the destructor
        // calls cancel() unconditionally (cpp:198) and the panel cancels a worker that
        // may already have finished (app_installation_panel.cpp:102). All three must be
        // silent no-ops on a worker that is not running -- no state flip, no stray
        // signal, in particular no migrationCancelled emitted out of a destructor.
        // Fully deterministic: no worker thread exists yet, so nothing can race these.
        QSignalSpy pausedSpy(&worker, &sak::AppInstallationWorker::migrationPaused);
        QSignalSpy resumedSpy(&worker, &sak::AppInstallationWorker::migrationResumed);
        QSignalSpy cancelledSpy(&worker, &sak::AppInstallationWorker::migrationCancelled);
        worker.pause();
        QVERIFY(!worker.isPaused());
        QCOMPARE(pausedSpy.count(), 0);
        worker.resume();
        QVERIFY(!worker.isPaused());
        QCOMPARE(resumedSpy.count(), 0);
        worker.cancel();
        QVERIFY(!worker.isRunning());
        QCOMPARE(cancelledSpy.count(), 0);

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
        // maxConcurrent=0 makes checkQueueState Finish before any job runs, so nothing
        // transitions to Success/Failed -- both counters are deterministically 0.
        QCOMPARE(stats.success, 0);
        QCOMPARE(stats.failed, 0);

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

        // The signal was the ONLY thing this test checked, despite its name: a cancel()
        // that emitted without draining the queue and marking those jobs Cancelled
        // (app_installation_worker.cpp:360-368) shipped green. That drain is what the
        // panel's progress readout depends on -- it counts finished work as
        // success+failed+skipped+cancelled (app_installation_panel.cpp:466) -- and what
        // stamps "cancelled" onto the report entries (cpp:365). At concurrency 1 at most
        // one job can have left the queue before cancel() took the mutex: a job cycle
        // costs a full registry+AppX scan (cpp:588) and then a 5s backoff sleep that
        // blocks processQueue (cpp:509). So at least three of the four are provably
        // marked -- a bound that holds however the threads interleave, and one a
        // signal-only cancel() cannot satisfy: it leaves all four Queued, cancelled == 0.
        const auto stats = worker.getStats();
        QCOMPARE(stats.total, 4);
        QVERIFY2(stats.cancelled >= 3,
                 qPrintable(QStringLiteral("cancel() marked only %1 of 4 jobs cancelled")
                                .arg(stats.cancelled)));

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
        sak::MigrationReport::MigrationEntry locked;
        locked.app_name = "LockedApp";
        locked.choco_package = "locked-app";
        locked.selected = true;
        locked.available = true;
        locked.version_lock = true;
        locked.locked_version = "1.2.3";
        report->addEntry(locked);

        // A rejected SELECTED entry sits BETWEEN the two installable ones so a job's
        // entryIndex can no longer be confused with its ordinal in m_jobs: buildJobQueue
        // passes the ENTRY index (app_installation_worker.cpp:287) and cancel() writes
        // report status through it (cpp:365).
        sak::MigrationReport::MigrationEntry rejected;
        rejected.app_name = "RejectedApp";
        rejected.choco_package = "rejected-app";
        rejected.selected = true;
        rejected.available = false;
        report->addEntry(rejected);

        // version_lock false with a stale locked_version: the OTHER arm of makeJob's
        // ternary (app_installation_worker.cpp:318), which no fixture reached because
        // every unlocked entry in this file also has an empty locked_version.
        sak::MigrationReport::MigrationEntry unlocked;
        unlocked.app_name = "UnlockedApp";
        unlocked.choco_package = "unlocked-app";
        unlocked.selected = true;
        unlocked.available = true;
        unlocked.version_lock = false;
        unlocked.locked_version = "9.9.9";
        report->addEntry(unlocked);

        int jobCount = worker.startMigration(report, 0);
        QCOMPARE(jobCount, 2);

        auto jobs = worker.getJobs();
        QCOMPARE(jobs.size(), 2);
        // maxConcurrent==0 never launches a job, so Queued is deterministic here.
        const int queued_status = static_cast<int>(sak::MigrationStatus::Queued);
        QCOMPARE(jobs[0].appName, QStringLiteral("LockedApp"));
        QCOMPARE(jobs[0].packageId, QStringLiteral("locked-app"));
        QCOMPARE(jobs[0].version, QStringLiteral("1.2.3"));
        QCOMPARE(jobs[0].entryIndex, 0);
        QCOMPARE(static_cast<int>(jobs[0].status), queued_status);
        QVERIFY(jobs[0].errorMessage.isEmpty());
        QCOMPARE(jobs[0].retryCount, 0);

        QCOMPARE(jobs[1].appName, QStringLiteral("UnlockedApp"));
        QCOMPARE(jobs[1].packageId, QStringLiteral("unlocked-app"));
        QVERIFY2(jobs[1].version.isEmpty(),
                 "an unlocked entry must not inherit a stale locked_version");
        QCOMPARE(jobs[1].entryIndex, 2);
        QCOMPARE(static_cast<int>(jobs[1].status), queued_status);

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

        // The payload is the whole point of the signal and nothing asserted it. A dry
        // run must report the 3 jobs it queued AND fold back the one
        // selected-but-unavailable entry it declined to queue
        // (app_installation_worker.cpp:424-425), so a completed migration accounts for
        // all 4 requested items rather than only the installable subset. total and
        // skipped are what the panel renders (app_installation_panel.cpp:408-418,
        // 465-469), so dropping the fold misreports the run to the user as 3-of-3.
        const auto done = doneSpy.at(0).at(0).value<sak::AppInstallationWorker::Stats>();
        QCOMPARE(done.total, 4);
        QCOMPARE(done.queued, 3);
        QCOMPARE(done.skipped, 1);
        QCOMPARE(done.pending, 0);
        QCOMPARE(done.installing, 0);
        QCOMPARE(done.success, 0);
        QCOMPARE(done.failed, 0);
        QCOMPARE(done.cancelled, 0);
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
        QCOMPARE(W::migrationSkipReason(noPkg), QStringLiteral("No matched Chocolatey package"));

        sak::MigrationReport::MigrationEntry unavail = ok;
        unavail.available = false;
        QCOMPARE(W::migrationSkipReason(unavail),
                 QStringLiteral("Package not available in the configured feed"));

        // Version lock with no locked version would silently install latest.
        sak::MigrationReport::MigrationEntry lockNoVer = ok;
        lockNoVer.version_lock = true;
        lockNoVer.locked_version = "";
        QCOMPARE(W::migrationSkipReason(lockNoVer),
                 QStringLiteral("Version lock requested but no locked version specified"));

        sak::MigrationReport::MigrationEntry lockVer = lockNoVer;
        lockVer.locked_version = "1.2.3";
        QVERIFY(W::migrationSkipReason(lockVer).isEmpty());
    }

    /// Skipped SELECTED entries must be recorded on the report (CODEX REVIEW 3 #6),
    /// not silently dropped.
    void skippedSelectedEntriesRecordedOnReport() {
        auto chocoMgr = std::make_shared<sak::ChocolateyManager>();
        sak::AppInstallationWorker worker(chocoMgr);

        QSignalSpy progressSpy(&worker, &sak::AppInstallationWorker::jobProgress);

        auto report = createTestReport(1, 1, 0, 1);  // 1 valid, 1 unavailable, 1 no-package
        worker.startMigration(report, 0);

        // Each rejected SELECTED entry is also NOTIFIED, in entry order, with the "Skipped: "
        // prefix and the reason -- a path nothing asserted, so the notify loop could be deleted
        // with the suite green and the user's log would never mention the dropped apps.
        // maxConcurrent==0 launches no install, so these are the ONLY jobProgress emissions and
        // the catalog is exact and ordered, not a membership floor.
        QCOMPARE(progressSpy.count(), 2);
        QCOMPARE(progressSpy.at(0).at(0).toInt(), 1);
        QCOMPARE(progressSpy.at(0).at(1).toString(),
                 QStringLiteral("Skipped: Package not available in the configured feed"));
        QCOMPARE(progressSpy.at(1).at(0).toInt(), 2);
        QCOMPARE(progressSpy.at(1).at(1).toString(),
                 QStringLiteral("Skipped: No matched Chocolatey package"));

        int skipped = 0;
        const auto& entries = report->getEntries();
        for (const auto& e : entries) {
            if (e.status == "skipped") {
                ++skipped;
                if (e.app_name == QLatin1String("UnavailableApp0")) {
                    QCOMPARE(e.error_message,
                             QStringLiteral("Package not available in the configured feed"));
                } else {
                    QCOMPARE(e.error_message, QStringLiteral("No matched Chocolatey package"));
                }
            }
        }
        QCOMPARE(skipped, 2);  // the unavailable + the no-package selected entries

        // The report side was checked but the WORKER side was not: entries rejected at
        // queue construction never become jobs, so getStats() has to fold them back into
        // total/skipped (app_installation_worker.cpp:424-425) or a finished migration
        // silently under-reports the work it was asked to do -- total and skipped are
        // exactly what the panel renders (app_installation_panel.cpp:408-418, 465-469).
        // maxConcurrent==0 dequeues nothing, so 1 queued + 2 rejected is exact, not a
        // floor.
        const auto stats = worker.getStats();
        QCOMPARE(stats.total, 3);
        QCOMPARE(stats.skipped, 2);
        QCOMPARE(stats.queued, 1);
        QCOMPARE(stats.success, 0);
        QCOMPARE(stats.failed, 0);
        QCOMPARE(stats.cancelled, 0);

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
        // Every negative above is rejected by the TRAILING lookahead, so the leading
        // (?<![A-Za-z0-9]) half of the whole-word guard (app_installation_worker.cpp:719)
        // was never the reason any of them failed and could be deleted with the suite
        // still green. These put the token at the END of the candidate, where only the
        // lookbehind can reject it -- one for the letter half of the class, one for the
        // digit half, so narrowing it to [A-Za-z] is caught too.
        QVERIFY(!W::nameIndicatesApp("MyNotepad", "Notepad"));
        QVERIFY(!W::nameIndicatesApp("Java8Update", "Update"));
        // Empty operands fail closed.
        QVERIFY(!W::nameIndicatesApp("", "Notepad"));
        QVERIFY(!W::nameIndicatesApp("Notepad", ""));
    }

    /// systemStateCheckEligible (R5-G22-12): the snapshot-based system-state
    /// fallback may certify an install ONLY when the pre-install snapshot was
    /// reliable AND choco did not report a definitive "0 installed". An unreliable
    /// snapshot can omit a pre-existing matching app, which the post-install check
    /// would misread as a NEW install and falsely certify; a "0 installed" line is
    /// authoritative failure. Both must fail closed.
    void systemStateCheckEligibleFailsClosed() {
        using W = sak::AppInstallationWorker;
        // The only eligible combination: reliable snapshot, choco not zero.
        QVERIFY(W::systemStateCheckEligible(/*reliable=*/true, /*chocoZero=*/false));
        // An unreliable pre-install snapshot fails closed even without a zero line
        // -- this is the R5-G22-12 fix; dropping the snapshot-reliable guard would
        // let this certify falsely.
        QVERIFY(!W::systemStateCheckEligible(/*reliable=*/false, /*chocoZero=*/false));
        // A definitive "0 installed" line is authoritative failure regardless of
        // the snapshot.
        QVERIFY(!W::systemStateCheckEligible(/*reliable=*/true, /*chocoZero=*/true));
        QVERIFY(!W::systemStateCheckEligible(/*reliable=*/false, /*chocoZero=*/true));
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
        // The refusal must land BEFORE any queue is built (the guard at
        // app_installation_worker.cpp:227-230 sits ahead of buildJobQueue at cpp:249):
        // returning 0 while having already populated m_jobs leaves phantom jobs that
        // getStats().total reports (cpp:393) straight into the panel's progress label,
        // and that a later start would inherit.
        QVERIFY(worker.getJobs().isEmpty());
        QCOMPARE(worker.getStats().total, 0);

        // The OTHER fail-closed arm of startMigration -- a null report
        // (app_installation_worker.cpp:223-226) -- had no test at all; delete it and
        // buildJobQueue null-derefs m_report at cpp:269. It is checked on a FRESH worker
        // holding a LIVE manager so neither the null-manager guard nor the
        // already-running guard can be what returns 0 here.
        auto liveMgr = std::make_shared<sak::ChocolateyManager>();
        sak::AppInstallationWorker liveWorker(liveMgr);
        QCOMPARE(liveWorker.startMigration(nullptr, 0), 0);
        QVERIFY(!liveWorker.isRunning());
        QVERIFY(liveWorker.getJobs().isEmpty());
    }
};

QTEST_GUILESS_MAIN(AppInstallationWorkerTests)
#include "test_app_installation_worker.moc"

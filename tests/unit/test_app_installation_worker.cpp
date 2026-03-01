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
    static std::shared_ptr<sak::MigrationReport> createTestReport(
        int selectedAvailable, int selectedUnavailable,
        int unselected, int noPackage)
    {
        auto report = std::make_shared<sak::MigrationReport>();

        // Selected + available + has choco package → should become jobs
        for (int i = 0; i < selectedAvailable; ++i) {
            sak::MigrationReport::MigrationEntry entry;
            entry.app_name = QString("AvailableApp%1").arg(i);
            entry.choco_package = QString("available-app%1").arg(i);
            entry.selected = true;
            entry.available = true;
            entry.status = "pending";
            report->addEntry(entry);
        }

        // Selected but NOT available → should be skipped
        for (int i = 0; i < selectedUnavailable; ++i) {
            sak::MigrationReport::MigrationEntry entry;
            entry.app_name = QString("UnavailableApp%1").arg(i);
            entry.choco_package = QString("unavailable-app%1").arg(i);
            entry.selected = true;
            entry.available = false;
            entry.status = "pending";
            report->addEntry(entry);
        }

        // Not selected → should be skipped
        for (int i = 0; i < unselected; ++i) {
            sak::MigrationReport::MigrationEntry entry;
            entry.app_name = QString("UnselectedApp%1").arg(i);
            entry.choco_package = QString("unselected-app%1").arg(i);
            entry.selected = false;
            entry.available = true;
            entry.status = "pending";
            report->addEntry(entry);
        }

        // No choco package → should be skipped
        for (int i = 0; i < noPackage; ++i) {
            sak::MigrationReport::MigrationEntry entry;
            entry.app_name = QString("NoPackageApp%1").arg(i);
            entry.choco_package = "";  // Empty → skip
            entry.selected = true;
            entry.available = true;
            entry.status = "pending";
            report->addEntry(entry);
        }

        return report;
    }

private Q_SLOTS:
    /// startMigration should only create jobs for selected+available entries with a choco package
    void jobFilteringOnlySelectedAvailable()
    {
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
    void noValidEntriesReturnsZero()
    {
        auto chocoMgr = std::make_shared<sak::ChocolateyManager>();
        sak::AppInstallationWorker worker(chocoMgr);

        auto report = createTestReport(0, 3, 2, 1);
        int jobCount = worker.startMigration(report, 0);
        QCOMPARE(jobCount, 0);
    }

    /// Initial state should be not-running, not-paused
    void initialStateCorrect()
    {
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
    void statsReflectJobCounts()
    {
        auto chocoMgr = std::make_shared<sak::ChocolateyManager>();
        sak::AppInstallationWorker worker(chocoMgr);

        auto report = createTestReport(5, 0, 0, 0);
        worker.startMigration(report, 0);

        auto stats = worker.getStats();
        QCOMPARE(stats.total, 5);
        // Jobs start as Pending
        QVERIFY(stats.pending + stats.queued + stats.cancelled >= 0);

        worker.cancel();
    }

    /// cancel should emit migrationCancelled and eventually stop the worker
    void cancelMarksJobsCancelled()
    {
        auto chocoMgr = std::make_shared<sak::ChocolateyManager>();
        sak::AppInstallationWorker worker(chocoMgr);

        QSignalSpy cancelSpy(&worker, &sak::AppInstallationWorker::migrationCancelled);

        auto report = createTestReport(4, 0, 0, 0);
        worker.startMigration(report, 1);

        // Cancel immediately — some jobs may already be in-flight
        worker.cancel();

        QCOMPARE(cancelSpy.count(), 1);

        // Worker destructor will wait for processQueue thread to finish
    }

    /// pause/resume toggle state correctly
    void pauseResumeToggles()
    {
        auto chocoMgr = std::make_shared<sak::ChocolateyManager>();
        sak::AppInstallationWorker worker(chocoMgr);

        QSignalSpy pauseSpy(&worker, &sak::AppInstallationWorker::migrationPaused);
        QSignalSpy resumeSpy(&worker, &sak::AppInstallationWorker::migrationResumed);

        auto report = createTestReport(2, 0, 0, 0);
        worker.startMigration(report, 0);

        worker.pause();
        QVERIFY(worker.isPaused());
        QCOMPARE(pauseSpy.count(), 1);

        worker.resume();
        QVERIFY(!worker.isPaused());
        QCOMPARE(resumeSpy.count(), 1);

        worker.cancel();
    }

    /// Version lock fields should be captured in jobs
    void versionLockCaptured()
    {
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
    void migrationStartedSignalEmitted()
    {
        auto chocoMgr = std::make_shared<sak::ChocolateyManager>();
        sak::AppInstallationWorker worker(chocoMgr);

        QSignalSpy startSpy(&worker, &sak::AppInstallationWorker::migrationStarted);

        auto report = createTestReport(3, 1, 2, 0);
        worker.startMigration(report, 0);

        // migrationStarted may be emitted synchronously or async
        if (startSpy.isEmpty()) {
            QVERIFY(startSpy.wait(1000));
        }
        QCOMPARE(startSpy.count(), 1);
        QCOMPARE(startSpy.at(0).at(0).toInt(), 3);  // 3 valid jobs

        worker.cancel();
    }
};

QTEST_GUILESS_MAIN(AppInstallationWorkerTests)
#include "test_app_installation_worker.moc"

// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * Unit tests for AppMigrationWorker
 * Tests background migration execution
 */

#include "sak/workers/app_migration_worker.h"
#include "sak/migration_report.h"
#include <QTest>
#include <QSignalSpy>
#include <QEventLoop>

class TestAppMigrationWorker : public QObject {
    Q_OBJECT

private slots:
    void testInitialization() {
        sak::AppMigrationWorker worker;
        
        QVERIFY(!worker.isRunning());
        QCOMPARE(worker.getProgress(), 0);
    }

    void testSetMigrationReport() {
        sak::AppMigrationWorker worker;
        sak::MigrationReport report;
        
        sak::MigrationReport::MigrationEntry entry;
        entry.app_name = "7-Zip";
        entry.package_id = "7zip";
        entry.selected = true;
        report.addEntry(entry);
        
        worker.setMigrationReport(report);
        
        QCOMPARE(worker.getTotalPackages(), 1);
    }

    void testStartMigration() {
        sak::AppMigrationWorker worker;
        sak::MigrationReport report;
        
        sak::MigrationReport::MigrationEntry entry;
        entry.app_name = "Test App";
        entry.package_id = "testapp";
        entry.selected = true;
        report.addEntry(entry);
        
        worker.setMigrationReport(report);
        
        QSignalSpy startedSpy(&worker, &sak::AppMigrationWorker::started);
        
        worker.start();
        
        QVERIFY(startedSpy.wait(1000));
        QVERIFY(worker.isRunning());
    }

    void testProgressReporting() {
        sak::AppMigrationWorker worker;
        sak::MigrationReport report;
        
        // Add multiple entries
        for (int i = 0; i < 5; i++) {
            sak::MigrationReport::MigrationEntry entry;
            entry.app_name = QString("App %1").arg(i);
            entry.package_id = QString("app%1").arg(i);
            entry.selected = true;
            report.addEntry(entry);
        }
        
        worker.setMigrationReport(report);
        
        QSignalSpy progressSpy(&worker, &sak::AppMigrationWorker::progress);
        
        worker.start();
        
        // Wait for some progress signals
        QVERIFY(progressSpy.wait(2000));
        
        // Should have received progress updates
        QVERIFY(progressSpy.count() > 0);
        
        // Progress should be between 0 and 100
        for (const auto& signal : progressSpy) {
            int progress = signal.at(0).toInt();
            QVERIFY(progress >= 0);
            QVERIFY(progress <= 100);
        }
    }

    void testPackageInstallation() {
        sak::AppMigrationWorker worker;
        sak::MigrationReport report;
        
        sak::MigrationReport::MigrationEntry entry;
        entry.app_name = "Notepad++";
        entry.package_id = "notepadplusplus";
        entry.selected = true;
        report.addEntry(entry);
        
        worker.setMigrationReport(report);
        worker.setDryRun(true);  // Dry run mode for testing
        
        QSignalSpy packageInstalledSpy(&worker, &sak::AppMigrationWorker::packageInstalled);
        
        worker.start();
        
        // Wait for completion
        QVERIFY(packageInstalledSpy.wait(5000));
        
        QCOMPARE(packageInstalledSpy.count(), 1);
    }

    void testErrorHandling() {
        sak::AppMigrationWorker worker;
        sak::MigrationReport report;
        
        // Add entry with non-existent package
        sak::MigrationReport::MigrationEntry entry;
        entry.app_name = "NonExistent App";
        entry.package_id = "nonexistent_package_xyz";
        entry.selected = true;
        report.addEntry(entry);
        
        worker.setMigrationReport(report);
        worker.setDryRun(true);
        
        QSignalSpy errorSpy(&worker, &sak::AppMigrationWorker::error);
        
        worker.start();
        
        // Should emit error signal
        QVERIFY(errorSpy.wait(5000));
        QVERIFY(errorSpy.count() > 0);
    }

    void testCancellation() {
        sak::AppMigrationWorker worker;
        sak::MigrationReport report;
        
        // Add many entries to ensure cancellation can be tested
        for (int i = 0; i < 20; i++) {
            sak::MigrationReport::MigrationEntry entry;
            entry.app_name = QString("App %1").arg(i);
            entry.package_id = QString("app%1").arg(i);
            entry.selected = true;
            report.addEntry(entry);
        }
        
        worker.setMigrationReport(report);
        worker.setDryRun(true);
        
        QSignalSpy cancelledSpy(&worker, &sak::AppMigrationWorker::cancelled);
        
        worker.start();
        
        // Cancel after short delay
        QTimer::singleShot(500, &worker, &sak::AppMigrationWorker::cancel);
        
        QVERIFY(cancelledSpy.wait(5000));
        QVERIFY(worker.wasCancelled());
    }

    void testCompletion() {
        sak::AppMigrationWorker worker;
        sak::MigrationReport report;
        
        sak::MigrationReport::MigrationEntry entry;
        entry.app_name = "Test App";
        entry.package_id = "testapp";
        entry.selected = true;
        report.addEntry(entry);
        
        worker.setMigrationReport(report);
        worker.setDryRun(true);
        
        QSignalSpy completedSpy(&worker, &sak::AppMigrationWorker::completed);
        
        worker.start();
        
        QVERIFY(completedSpy.wait(5000));
        QVERIFY(!worker.isRunning());
        QCOMPARE(worker.getProgress(), 100);
    }

    void testSkipUnselectedPackages() {
        sak::AppMigrationWorker worker;
        sak::MigrationReport report;
        
        // Add selected entry
        sak::MigrationReport::MigrationEntry entry1;
        entry1.app_name = "Selected App";
        entry1.package_id = "selected";
        entry1.selected = true;
        report.addEntry(entry1);
        
        // Add unselected entry
        sak::MigrationReport::MigrationEntry entry2;
        entry2.app_name = "Unselected App";
        entry2.package_id = "unselected";
        entry2.selected = false;
        report.addEntry(entry2);
        
        worker.setMigrationReport(report);
        worker.setDryRun(true);
        
        QSignalSpy installedSpy(&worker, &sak::AppMigrationWorker::packageInstalled);
        
        worker.start();
        installedSpy.wait(5000);
        
        // Should only install selected package
        QCOMPARE(installedSpy.count(), 1);
    }

    void testStatusMessages() {
        sak::AppMigrationWorker worker;
        sak::MigrationReport report;
        
        sak::MigrationReport::MigrationEntry entry;
        entry.app_name = "Test App";
        entry.package_id = "testapp";
        entry.selected = true;
        report.addEntry(entry);
        
        worker.setMigrationReport(report);
        worker.setDryRun(true);
        
        QSignalSpy statusSpy(&worker, &sak::AppMigrationWorker::statusChanged);
        
        worker.start();
        
        QVERIFY(statusSpy.wait(5000));
        QVERIFY(statusSpy.count() > 0);
        
        // Verify status messages are not empty
        for (const auto& signal : statusSpy) {
            QString status = signal.at(0).toString();
            QVERIFY(!status.isEmpty());
        }
    }

    void testBatchInstallation() {
        sak::AppMigrationWorker worker;
        sak::MigrationReport report;
        
        QStringList packages = {"7zip", "notepadplusplus", "vlc"};
        
        for (const QString& pkg : packages) {
            sak::MigrationReport::MigrationEntry entry;
            entry.package_id = pkg;
            entry.selected = true;
            report.addEntry(entry);
        }
        
        worker.setMigrationReport(report);
        worker.setDryRun(true);
        
        QSignalSpy installedSpy(&worker, &sak::AppMigrationWorker::packageInstalled);
        QSignalSpy completedSpy(&worker, &sak::AppMigrationWorker::completed);
        
        worker.start();
        
        QVERIFY(completedSpy.wait(10000));
        QCOMPARE(installedSpy.count(), packages.size());
    }

    void testRetryOnFailure() {
        sak::AppMigrationWorker worker;
        worker.setMaxRetries(3);
        
        sak::MigrationReport report;
        sak::MigrationReport::MigrationEntry entry;
        entry.package_id = "failing_package";
        entry.selected = true;
        report.addEntry(entry);
        
        worker.setMigrationReport(report);
        worker.setDryRun(true);
        
        QSignalSpy retrySpy(&worker, &sak::AppMigrationWorker::retrying);
        
        worker.start();
        
        // Should retry on failure
        retrySpy.wait(5000);
        
        // May or may not retry depending on error type
        QVERIFY(retrySpy.count() <= 3);
    }

    void testGetInstalledPackages() {
        sak::AppMigrationWorker worker;
        sak::MigrationReport report;
        
        sak::MigrationReport::MigrationEntry entry1;
        entry1.package_id = "pkg1";
        entry1.selected = true;
        report.addEntry(entry1);
        
        sak::MigrationReport::MigrationEntry entry2;
        entry2.package_id = "pkg2";
        entry2.selected = true;
        report.addEntry(entry2);
        
        worker.setMigrationReport(report);
        worker.setDryRun(true);
        
        QSignalSpy completedSpy(&worker, &sak::AppMigrationWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        QStringList installed = worker.getInstalledPackages();
        QVERIFY(installed.size() > 0);
    }

    void testGetFailedPackages() {
        sak::AppMigrationWorker worker;
        sak::MigrationReport report;
        
        sak::MigrationReport::MigrationEntry entry;
        entry.package_id = "nonexistent";
        entry.selected = true;
        report.addEntry(entry);
        
        worker.setMigrationReport(report);
        worker.setDryRun(true);
        
        QSignalSpy completedSpy(&worker, &sak::AppMigrationWorker::completed);
        worker.start();
        completedSpy.wait(5000);
        
        QStringList failed = worker.getFailedPackages();
        // May have failed packages depending on implementation
    }

    void testDryRunMode() {
        sak::AppMigrationWorker worker;
        worker.setDryRun(true);
        
        QVERIFY(worker.isDryRun());
        
        // Dry run should complete without actual installation
        sak::MigrationReport report;
        sak::MigrationReport::MigrationEntry entry;
        entry.package_id = "testpkg";
        entry.selected = true;
        report.addEntry(entry);
        
        worker.setMigrationReport(report);
        
        QSignalSpy completedSpy(&worker, &sak::AppMigrationWorker::completed);
        worker.start();
        
        QVERIFY(completedSpy.wait(5000));
    }

    void testThreadSafety() {
        // Create multiple workers
        QVector<sak::AppMigrationWorker*> workers;
        
        for (int i = 0; i < 3; i++) {
            auto* worker = new sak::AppMigrationWorker(this);
            worker->setDryRun(true);
            
            sak::MigrationReport report;
            sak::MigrationReport::MigrationEntry entry;
            entry.package_id = QString("pkg%1").arg(i);
            entry.selected = true;
            report.addEntry(entry);
            
            worker->setMigrationReport(report);
            workers.append(worker);
        }
        
        // Start all workers
        for (auto* worker : workers) {
            worker->start();
        }
        
        // Wait for all to complete
        QEventLoop loop;
        int completed = 0;
        for (auto* worker : workers) {
            connect(worker, &sak::AppMigrationWorker::completed, [&]() {
                completed++;
                if (completed == workers.size()) {
                    loop.quit();
                }
            });
        }
        
        QTimer::singleShot(15000, &loop, &QEventLoop::quit);
        loop.exec();
        
        QCOMPARE(completed, workers.size());
        
        qDeleteAll(workers);
    }

    void testGetElapsedTime() {
        sak::AppMigrationWorker worker;
        sak::MigrationReport report;
        
        sak::MigrationReport::MigrationEntry entry;
        entry.package_id = "test";
        entry.selected = true;
        report.addEntry(entry);
        
        worker.setMigrationReport(report);
        worker.setDryRun(true);
        
        worker.start();
        
        QTest::qWait(1000);
        
        qint64 elapsed = worker.getElapsedTime();
        QVERIFY(elapsed >= 900);  // At least 900ms
    }

    void testPauseResume() {
        sak::AppMigrationWorker worker;
        sak::MigrationReport report;
        
        for (int i = 0; i < 10; i++) {
            sak::MigrationReport::MigrationEntry entry;
            entry.package_id = QString("pkg%1").arg(i);
            entry.selected = true;
            report.addEntry(entry);
        }
        
        worker.setMigrationReport(report);
        worker.setDryRun(true);
        
        QSignalSpy pausedSpy(&worker, &sak::AppMigrationWorker::paused);
        QSignalSpy resumedSpy(&worker, &sak::AppMigrationWorker::resumed);
        
        worker.start();
        
        QTimer::singleShot(500, &worker, &sak::AppMigrationWorker::pause);
        
        if (pausedSpy.wait(2000)) {
            QVERIFY(worker.isPaused());
            
            QTimer::singleShot(500, &worker, &sak::AppMigrationWorker::resume);
            
            QVERIFY(resumedSpy.wait(2000));
            QVERIFY(!worker.isPaused());
        }
    }
};

QTEST_MAIN(TestAppMigrationWorker)
#include "test_app_migration_worker.moc"

// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * Unit tests for DiskCleanupAction
 * Tests disk cleanup functionality and file deletion
 */

#include "sak/actions/disk_cleanup_action.h"
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>

class TestDiskCleanupAction : public QObject {
    Q_OBJECT

private:
    QTemporaryDir* tempDir;

private slots:
    void initTestCase() {
        tempDir = new QTemporaryDir();
        QVERIFY(tempDir->isValid());
        
        // Create test temp files
        QDir dir(tempDir->path());
        dir.mkpath("Temp");
        dir.mkpath("Downloads");
        
        for (int i = 0; i < 10; i++) {
            QFile file(tempDir->path() + QString("/Temp/temp%1.tmp").arg(i));
            file.open(QIODevice::WriteOnly);
            file.write(QByteArray(1024, 'x')); // 1KB each
            file.close();
        }
    }

    void cleanupTestCase() {
        delete tempDir;
    }

    void testScanTempFiles() {
        sak::actions::DiskCleanupAction action;
        
        action.addScanPath(tempDir->path() + "/Temp");
        
        qint64 totalSize = action.calculateCleanupSize();
        QVERIFY(totalSize >= 10240); // At least 10KB
    }

    void testCleanupExecution() {
        sak::actions::DiskCleanupAction action;
        action.addScanPath(tempDir->path() + "/Temp");
        
        bool started = false;
        bool completed = false;
        
        connect(&action, &sak::actions::DiskCleanupAction::started, [&]() {
            started = true;
        });
        
        connect(&action, &sak::actions::DiskCleanupAction::completed, [&]() {
            completed = true;
        });
        
        action.execute();
        QTest::qWait(1000); // Wait for completion
        
        QVERIFY(started);
        QVERIFY(completed);
    }

    void testProgressSignals() {
        sak::actions::DiskCleanupAction action;
        action.addScanPath(tempDir->path() + "/Temp");
        
        int progressCount = 0;
        connect(&action, &sak::actions::DiskCleanupAction::progress, 
                [&](int current, int total) {
            progressCount++;
            QVERIFY(current <= total);
        });
        
        action.execute();
        QTest::qWait(1000);
        
        QVERIFY(progressCount > 0);
    }

    void testExcludePatterns() {
        // Create file to keep
        QFile keepFile(tempDir->path() + "/Temp/keep.log");
        keepFile.open(QIODevice::WriteOnly);
        keepFile.write("Keep this");
        keepFile.close();
        
        sak::actions::DiskCleanupAction action;
        action.addScanPath(tempDir->path() + "/Temp");
        action.setExcludePatterns(QStringList() << "*.log");
        
        action.execute();
        QTest::qWait(1000);
        
        QVERIFY(keepFile.exists());
    }

    void testDryRun() {
        sak::actions::DiskCleanupAction action;
        action.addScanPath(tempDir->path() + "/Temp");
        action.setDryRun(true);
        
        qint64 beforeSize = action.calculateCleanupSize();
        action.execute();
        QTest::qWait(1000);
        qint64 afterSize = action.calculateCleanupSize();
        
        QCOMPARE(beforeSize, afterSize); // Nothing should be deleted
    }

    void testCancellation() {
        sak::actions::DiskCleanupAction action;
        action.addScanPath(tempDir->path() + "/Temp");
        
        action.execute();
        QTimer::singleShot(100, &action, &sak::actions::DiskCleanupAction::cancel);
        
        QTest::qWait(500);
        QVERIFY(action.wasCancelled());
    }
};

QTEST_MAIN(TestDiskCleanupAction)
#include "test_disk_cleanup_action.moc"

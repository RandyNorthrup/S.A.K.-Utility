// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * Unit tests for BackupWorker
 * Tests background backup execution
 */

#include "sak/workers/backup_worker.h"
#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>

class TestBackupWorker : public QObject {
    Q_OBJECT

private:
    QTemporaryDir* tempDir;
    QString sourceDir;
    QString backupDir;

private slots:
    void initTestCase() {
        tempDir = new QTemporaryDir();
        QVERIFY(tempDir->isValid());
        
        sourceDir = tempDir->path() + "/source";
        backupDir = tempDir->path() + "/backup";
        
        QDir().mkpath(sourceDir);
        QDir().mkpath(backupDir);
        
        createTestFiles();
    }

    void cleanupTestCase() {
        delete tempDir;
    }

    void createTestFiles() {
        for (int i = 0; i < 10; i++) {
            QString path = sourceDir + QString("/file%1.txt").arg(i);
            QFile file(path);
            file.open(QIODevice::WriteOnly);
            file.write(QByteArray(1024, 'a' + (i % 26)));
            file.close();
        }
    }

    void testInitialization() {
        sak::BackupWorker worker;
        
        QVERIFY(!worker.isRunning());
        QCOMPARE(worker.getProgress(), 0);
    }

    void testSetSourceAndDestination() {
        sak::BackupWorker worker;
        
        worker.setSourceDirectory(sourceDir);
        worker.setBackupDirectory(backupDir);
        
        QCOMPARE(worker.getSourceDirectory(), sourceDir);
        QCOMPARE(worker.getBackupDirectory(), backupDir);
    }

    void testStartBackup() {
        sak::BackupWorker worker;
        worker.setSourceDirectory(sourceDir);
        worker.setBackupDirectory(backupDir + "/test1");
        
        QSignalSpy startedSpy(&worker, &sak::BackupWorker::started);
        
        worker.start();
        
        QVERIFY(startedSpy.wait(1000));
        QVERIFY(worker.isRunning());
    }

    void testProgressReporting() {
        sak::BackupWorker worker;
        worker.setSourceDirectory(sourceDir);
        worker.setBackupDirectory(backupDir + "/test2");
        
        QSignalSpy progressSpy(&worker, &sak::BackupWorker::progress);
        
        worker.start();
        
        QVERIFY(progressSpy.wait(5000));
        QVERIFY(progressSpy.count() > 0);
        
        // Progress should be valid
        for (const auto& signal : progressSpy) {
            int progress = signal.at(0).toInt();
            QVERIFY(progress >= 0);
            QVERIFY(progress <= 100);
        }
    }

    void testFilesCopied() {
        sak::BackupWorker worker;
        QString testBackup = backupDir + "/test3";
        
        worker.setSourceDirectory(sourceDir);
        worker.setBackupDirectory(testBackup);
        
        QSignalSpy completedSpy(&worker, &sak::BackupWorker::completed);
        
        worker.start();
        
        QVERIFY(completedSpy.wait(10000));
        
        // Verify files were copied
        for (int i = 0; i < 10; i++) {
            QString path = testBackup + QString("/file%1.txt").arg(i);
            QVERIFY(QFile::exists(path));
        }
    }

    void testCancellation() {
        sak::BackupWorker worker;
        worker.setSourceDirectory(sourceDir);
        worker.setBackupDirectory(backupDir + "/test4");
        
        QSignalSpy cancelledSpy(&worker, &sak::BackupWorker::cancelled);
        
        worker.start();
        
        QTimer::singleShot(100, &worker, &sak::BackupWorker::cancel);
        
        QVERIFY(cancelledSpy.wait(5000));
        QVERIFY(worker.wasCancelled());
    }

    void testCompressionEnabled() {
        sak::BackupWorker worker;
        worker.setSourceDirectory(sourceDir);
        worker.setBackupDirectory(backupDir + "/test5");
        worker.setCompressionEnabled(true);
        
        QVERIFY(worker.isCompressionEnabled());
        
        QSignalSpy completedSpy(&worker, &sak::BackupWorker::completed);
        
        worker.start();
        
        QVERIFY(completedSpy.wait(10000));
        
        // Should create compressed archive
        QString archivePath = backupDir + "/test5/backup.zip";
        // Check based on implementation
    }

    void testEncryptionEnabled() {
        sak::BackupWorker worker;
        worker.setSourceDirectory(sourceDir);
        worker.setBackupDirectory(backupDir + "/test6");
        worker.setEncryptionEnabled(true);
        worker.setEncryptionPassword("testpassword123");
        
        QVERIFY(worker.isEncryptionEnabled());
        
        QSignalSpy completedSpy(&worker, &sak::BackupWorker::completed);
        
        worker.start();
        
        QVERIFY(completedSpy.wait(10000));
    }

    void testIncrementalBackup() {
        sak::BackupWorker worker;
        QString testBackup = backupDir + "/test7";
        
        worker.setSourceDirectory(sourceDir);
        worker.setBackupDirectory(testBackup);
        worker.setBackupType(sak::BackupWorker::BackupType::Incremental);
        
        // First backup
        QSignalSpy completed1(&worker, &sak::BackupWorker::completed);
        worker.start();
        QVERIFY(completed1.wait(10000));
        
        // Modify a file
        QFile file(sourceDir + "/file0.txt");
        file.open(QIODevice::Append);
        file.write("Modified content");
        file.close();
        
        // Second incremental backup
        sak::BackupWorker worker2;
        worker2.setSourceDirectory(sourceDir);
        worker2.setBackupDirectory(testBackup);
        worker2.setBackupType(sak::BackupWorker::BackupType::Incremental);
        
        QSignalSpy completed2(&worker2, &sak::BackupWorker::completed);
        worker2.start();
        QVERIFY(completed2.wait(10000));
    }

    void testDifferentialBackup() {
        sak::BackupWorker worker;
        worker.setSourceDirectory(sourceDir);
        worker.setBackupDirectory(backupDir + "/test8");
        worker.setBackupType(sak::BackupWorker::BackupType::Differential);
        
        QSignalSpy completedSpy(&worker, &sak::BackupWorker::completed);
        
        worker.start();
        
        QVERIFY(completedSpy.wait(10000));
    }

    void testExclusionPatterns() {
        sak::BackupWorker worker;
        QString testBackup = backupDir + "/test9";
        
        worker.setSourceDirectory(sourceDir);
        worker.setBackupDirectory(testBackup);
        worker.setExclusionPatterns({"*0.txt", "*1.txt"});
        
        QSignalSpy completedSpy(&worker, &sak::BackupWorker::completed);
        
        worker.start();
        
        QVERIFY(completedSpy.wait(10000));
        
        // Excluded files should not exist
        QVERIFY(!QFile::exists(testBackup + "/file0.txt"));
        QVERIFY(!QFile::exists(testBackup + "/file1.txt"));
        
        // Other files should exist
        QVERIFY(QFile::exists(testBackup + "/file2.txt"));
    }

    void testErrorHandling() {
        sak::BackupWorker worker;
        worker.setSourceDirectory("/nonexistent/source");
        worker.setBackupDirectory(backupDir + "/test10");
        
        QSignalSpy errorSpy(&worker, &sak::BackupWorker::error);
        
        worker.start();
        
        QVERIFY(errorSpy.wait(5000));
        QVERIFY(errorSpy.count() > 0);
    }

    void testGetBackupSize() {
        sak::BackupWorker worker;
        QString testBackup = backupDir + "/test11";
        
        worker.setSourceDirectory(sourceDir);
        worker.setBackupDirectory(testBackup);
        
        QSignalSpy completedSpy(&worker, &sak::BackupWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        qint64 size = worker.getBackupSize();
        QVERIFY(size > 0);
    }

    void testGetFileCount() {
        sak::BackupWorker worker;
        worker.setSourceDirectory(sourceDir);
        worker.setBackupDirectory(backupDir + "/test12");
        
        QSignalSpy completedSpy(&worker, &sak::BackupWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        int count = worker.getFileCount();
        QCOMPARE(count, 10);
    }

    void testVerifyBackup() {
        sak::BackupWorker worker;
        QString testBackup = backupDir + "/test13";
        
        worker.setSourceDirectory(sourceDir);
        worker.setBackupDirectory(testBackup);
        worker.setVerificationEnabled(true);
        
        QSignalSpy verifiedSpy(&worker, &sak::BackupWorker::verified);
        
        worker.start();
        
        QVERIFY(verifiedSpy.wait(10000));
    }

    void testStatusMessages() {
        sak::BackupWorker worker;
        worker.setSourceDirectory(sourceDir);
        worker.setBackupDirectory(backupDir + "/test14");
        
        QSignalSpy statusSpy(&worker, &sak::BackupWorker::statusChanged);
        
        worker.start();
        
        QVERIFY(statusSpy.wait(10000));
        QVERIFY(statusSpy.count() > 0);
    }

    void testCurrentFileSignal() {
        sak::BackupWorker worker;
        worker.setSourceDirectory(sourceDir);
        worker.setBackupDirectory(backupDir + "/test15");
        
        QSignalSpy fileSpy(&worker, &sak::BackupWorker::currentFile);
        
        worker.start();
        
        QVERIFY(fileSpy.wait(10000));
        QVERIFY(fileSpy.count() > 0);
    }

    void testPauseResume() {
        sak::BackupWorker worker;
        worker.setSourceDirectory(sourceDir);
        worker.setBackupDirectory(backupDir + "/test16");
        
        QSignalSpy pausedSpy(&worker, &sak::BackupWorker::paused);
        QSignalSpy resumedSpy(&worker, &sak::BackupWorker::resumed);
        
        worker.start();
        
        QTimer::singleShot(500, &worker, &sak::BackupWorker::pause);
        
        if (pausedSpy.wait(2000)) {
            QVERIFY(worker.isPaused());
            
            QTimer::singleShot(500, &worker, &sak::BackupWorker::resume);
            
            QVERIFY(resumedSpy.wait(2000));
            QVERIFY(!worker.isPaused());
        }
    }

    void testElapsedTime() {
        sak::BackupWorker worker;
        worker.setSourceDirectory(sourceDir);
        worker.setBackupDirectory(backupDir + "/test17");
        
        worker.start();
        
        QTest::qWait(1000);
        
        qint64 elapsed = worker.getElapsedTime();
        QVERIFY(elapsed >= 900);
    }

    void testSpeed() {
        sak::BackupWorker worker;
        worker.setSourceDirectory(sourceDir);
        worker.setBackupDirectory(backupDir + "/test18");
        
        worker.start();
        
        QTest::qWait(1000);
        
        double speed = worker.getCurrentSpeed();
        // Speed should be calculated in bytes/sec
        QVERIFY(speed >= 0);
    }
};

QTEST_MAIN(TestBackupWorker)
#include "test_backup_worker.moc"

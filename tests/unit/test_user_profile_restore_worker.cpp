// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * Unit tests for UserProfileRestoreWorker
 * Tests user profile restore execution
 */

#include "sak/workers/user_profile_restore_worker.h"
#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>

class TestUserProfileRestoreWorker : public QObject {
    Q_OBJECT

private:
    QTemporaryDir* tempDir;
    QString backupDir;
    QString restoreDir;

private slots:
    void initTestCase() {
        tempDir = new QTemporaryDir();
        QVERIFY(tempDir->isValid());
        
        backupDir = tempDir->path() + "/backup";
        restoreDir = tempDir->path() + "/restore";
        
        QDir().mkpath(backupDir);
        QDir().mkpath(restoreDir);
        
        createTestBackup();
    }

    void cleanupTestCase() {
        delete tempDir;
    }

    void createTestBackup() {
        QDir(backupDir).mkpath("Documents");
        QDir(backupDir).mkpath("Pictures");
        QDir(backupDir).mkpath("Desktop");
        
        createFile(backupDir + "/Documents/doc1.txt", "Document 1");
        createFile(backupDir + "/Documents/doc2.txt", "Document 2");
        createFile(backupDir + "/Pictures/pic1.jpg", "Image data");
        createFile(backupDir + "/Desktop/readme.txt", "Desktop file");
    }

    void createFile(const QString& path, const QString& content) {
        QFile file(path);
        file.open(QIODevice::WriteOnly);
        file.write(content.toUtf8());
        file.close();
    }

    void testInitialization() {
        sak::UserProfileRestoreWorker worker;
        
        QVERIFY(!worker.isRunning());
        QCOMPARE(worker.getProgress(), 0);
    }

    void testSetBackupDirectory() {
        sak::UserProfileRestoreWorker worker;
        
        worker.setBackupDirectory(backupDir);
        
        QCOMPARE(worker.getBackupDirectory(), backupDir);
    }

    void testSetRestoreDirectory() {
        sak::UserProfileRestoreWorker worker;
        
        worker.setRestoreDirectory(restoreDir);
        
        QCOMPARE(worker.getRestoreDirectory(), restoreDir);
    }

    void testStartRestore() {
        sak::UserProfileRestoreWorker worker;
        worker.setBackupDirectory(backupDir);
        worker.setRestoreDirectory(restoreDir + "/test1");
        
        QSignalSpy startedSpy(&worker, &sak::UserProfileRestoreWorker::started);
        
        worker.start();
        
        QVERIFY(startedSpy.wait(1000));
        QVERIFY(worker.isRunning());
    }

    void testProgressReporting() {
        sak::UserProfileRestoreWorker worker;
        worker.setBackupDirectory(backupDir);
        worker.setRestoreDirectory(restoreDir + "/test2");
        
        QSignalSpy progressSpy(&worker, &sak::UserProfileRestoreWorker::progress);
        
        worker.start();
        
        QVERIFY(progressSpy.wait(5000));
        QVERIFY(progressSpy.count() > 0);
    }

    void testRestoreDocuments() {
        sak::UserProfileRestoreWorker worker;
        QString testRestore = restoreDir + "/test3";
        
        worker.setBackupDirectory(backupDir);
        worker.setRestoreDirectory(testRestore);
        worker.setFolderSelection({"Documents"});
        
        QSignalSpy completedSpy(&worker, &sak::UserProfileRestoreWorker::completed);
        
        worker.start();
        
        QVERIFY(completedSpy.wait(10000));
        
        QVERIFY(QFile::exists(testRestore + "/Documents/doc1.txt"));
        QVERIFY(QFile::exists(testRestore + "/Documents/doc2.txt"));
    }

    void testRestoreMultipleFolders() {
        sak::UserProfileRestoreWorker worker;
        QString testRestore = restoreDir + "/test4";
        
        worker.setBackupDirectory(backupDir);
        worker.setRestoreDirectory(testRestore);
        worker.setFolderSelection({"Documents", "Pictures", "Desktop"});
        
        QSignalSpy completedSpy(&worker, &sak::UserProfileRestoreWorker::completed);
        
        worker.start();
        
        QVERIFY(completedSpy.wait(10000));
        
        QVERIFY(QFile::exists(testRestore + "/Documents/doc1.txt"));
        QVERIFY(QFile::exists(testRestore + "/Pictures/pic1.jpg"));
        QVERIFY(QFile::exists(testRestore + "/Desktop/readme.txt"));
    }

    void testConflictResolutionSkip() {
        // Create existing file
        QString testRestore = restoreDir + "/test5";
        QDir().mkpath(testRestore + "/Documents");
        createFile(testRestore + "/Documents/doc1.txt", "Existing content");
        
        sak::UserProfileRestoreWorker worker;
        worker.setBackupDirectory(backupDir);
        worker.setRestoreDirectory(testRestore);
        worker.setFolderSelection({"Documents"});
        worker.setConflictResolution(sak::UserProfileRestoreWorker::ConflictResolution::Skip);
        
        QSignalSpy completedSpy(&worker, &sak::UserProfileRestoreWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        // File should not be overwritten
        QFile file(testRestore + "/Documents/doc1.txt");
        file.open(QIODevice::ReadOnly);
        QString content = QString::fromUtf8(file.readAll());
        file.close();
        
        QCOMPARE(content, QString("Existing content"));
    }

    void testConflictResolutionOverwrite() {
        // Create existing file
        QString testRestore = restoreDir + "/test6";
        QDir().mkpath(testRestore + "/Documents");
        createFile(testRestore + "/Documents/doc1.txt", "Existing content");
        
        sak::UserProfileRestoreWorker worker;
        worker.setBackupDirectory(backupDir);
        worker.setRestoreDirectory(testRestore);
        worker.setFolderSelection({"Documents"});
        worker.setConflictResolution(sak::UserProfileRestoreWorker::ConflictResolution::Overwrite);
        
        QSignalSpy completedSpy(&worker, &sak::UserProfileRestoreWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        // File should be overwritten
        QFile file(testRestore + "/Documents/doc1.txt");
        file.open(QIODevice::ReadOnly);
        QString content = QString::fromUtf8(file.readAll());
        file.close();
        
        QCOMPARE(content, QString("Document 1"));
    }

    void testConflictResolutionKeepNewer() {
        QString testRestore = restoreDir + "/test7";
        QDir().mkpath(testRestore + "/Documents");
        
        // Create older file
        createFile(testRestore + "/Documents/doc1.txt", "Old content");
        
        // Wait to ensure timestamp difference
        QTest::qWait(100);
        
        sak::UserProfileRestoreWorker worker;
        worker.setBackupDirectory(backupDir);
        worker.setRestoreDirectory(testRestore);
        worker.setFolderSelection({"Documents"});
        worker.setConflictResolution(sak::UserProfileRestoreWorker::ConflictResolution::KeepNewer);
        
        QSignalSpy completedSpy(&worker, &sak::UserProfileRestoreWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        // Newer version should be kept
        QVERIFY(QFile::exists(testRestore + "/Documents/doc1.txt"));
    }

    void testCancellation() {
        sak::UserProfileRestoreWorker worker;
        worker.setBackupDirectory(backupDir);
        worker.setRestoreDirectory(restoreDir + "/test8");
        worker.setFolderSelection({"Documents", "Pictures", "Desktop"});
        
        QSignalSpy cancelledSpy(&worker, &sak::UserProfileRestoreWorker::cancelled);
        
        worker.start();
        
        QTimer::singleShot(200, &worker, &sak::UserProfileRestoreWorker::cancel);
        
        QVERIFY(cancelledSpy.wait(5000));
        QVERIFY(worker.wasCancelled());
    }

    void testErrorHandling() {
        sak::UserProfileRestoreWorker worker;
        worker.setBackupDirectory("/nonexistent/backup");
        worker.setRestoreDirectory(restoreDir + "/test9");
        
        QSignalSpy errorSpy(&worker, &sak::UserProfileRestoreWorker::error);
        
        worker.start();
        
        QVERIFY(errorSpy.wait(5000));
        QVERIFY(errorSpy.count() > 0);
    }

    void testGetRestoredFileCount() {
        sak::UserProfileRestoreWorker worker;
        worker.setBackupDirectory(backupDir);
        worker.setRestoreDirectory(restoreDir + "/test10");
        worker.setFolderSelection({"Documents"});
        
        QSignalSpy completedSpy(&worker, &sak::UserProfileRestoreWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        int count = worker.getRestoredFileCount();
        QVERIFY(count >= 2);
    }

    void testGetRestoredSize() {
        sak::UserProfileRestoreWorker worker;
        worker.setBackupDirectory(backupDir);
        worker.setRestoreDirectory(restoreDir + "/test11");
        worker.setFolderSelection({"Documents"});
        
        QSignalSpy completedSpy(&worker, &sak::UserProfileRestoreWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        qint64 size = worker.getRestoredSize();
        QVERIFY(size > 0);
    }

    void testVerifyRestore() {
        sak::UserProfileRestoreWorker worker;
        worker.setBackupDirectory(backupDir);
        worker.setRestoreDirectory(restoreDir + "/test12");
        worker.setFolderSelection({"Documents"});
        worker.setVerificationEnabled(true);
        
        QSignalSpy verifiedSpy(&worker, &sak::UserProfileRestoreWorker::verified);
        
        worker.start();
        
        QVERIFY(verifiedSpy.wait(10000));
    }

    void testStatusMessages() {
        sak::UserProfileRestoreWorker worker;
        worker.setBackupDirectory(backupDir);
        worker.setRestoreDirectory(restoreDir + "/test13");
        worker.setFolderSelection({"Documents"});
        
        QSignalSpy statusSpy(&worker, &sak::UserProfileRestoreWorker::statusChanged);
        
        worker.start();
        
        QVERIFY(statusSpy.wait(10000));
        QVERIFY(statusSpy.count() > 0);
    }

    void testCurrentFolderSignal() {
        sak::UserProfileRestoreWorker worker;
        worker.setBackupDirectory(backupDir);
        worker.setRestoreDirectory(restoreDir + "/test14");
        worker.setFolderSelection({"Documents", "Pictures"});
        
        QSignalSpy folderSpy(&worker, &sak::UserProfileRestoreWorker::currentFolder);
        
        worker.start();
        
        QVERIFY(folderSpy.wait(10000));
        QVERIFY(folderSpy.count() >= 2);
    }

    void testPauseResume() {
        sak::UserProfileRestoreWorker worker;
        worker.setBackupDirectory(backupDir);
        worker.setRestoreDirectory(restoreDir + "/test15");
        worker.setFolderSelection({"Documents", "Pictures", "Desktop"});
        
        QSignalSpy pausedSpy(&worker, &sak::UserProfileRestoreWorker::paused);
        QSignalSpy resumedSpy(&worker, &sak::UserProfileRestoreWorker::resumed);
        
        worker.start();
        
        QTimer::singleShot(500, &worker, &sak::UserProfileRestoreWorker::pause);
        
        if (pausedSpy.wait(2000)) {
            QVERIFY(worker.isPaused());
            
            QTimer::singleShot(500, &worker, &sak::UserProfileRestoreWorker::resume);
            
            QVERIFY(resumedSpy.wait(2000));
            QVERIFY(!worker.isPaused());
        }
    }

    void testElapsedTime() {
        sak::UserProfileRestoreWorker worker;
        worker.setBackupDirectory(backupDir);
        worker.setRestoreDirectory(restoreDir + "/test16");
        worker.setFolderSelection({"Documents"});
        
        worker.start();
        
        QTest::qWait(1000);
        
        qint64 elapsed = worker.getElapsedTime();
        QVERIFY(elapsed >= 900);
    }

    void testFixPermissions() {
        sak::UserProfileRestoreWorker worker;
        worker.setBackupDirectory(backupDir);
        worker.setRestoreDirectory(restoreDir + "/test17");
        worker.setFolderSelection({"Documents"});
        worker.setFixPermissions(true);
        
        QSignalSpy completedSpy(&worker, &sak::UserProfileRestoreWorker::completed);
        
        worker.start();
        
        QVERIFY(completedSpy.wait(10000));
    }

    void testDryRun() {
        sak::UserProfileRestoreWorker worker;
        worker.setBackupDirectory(backupDir);
        worker.setRestoreDirectory(restoreDir + "/test18");
        worker.setFolderSelection({"Documents"});
        worker.setDryRun(true);
        
        QVERIFY(worker.isDryRun());
        
        QSignalSpy completedSpy(&worker, &sak::UserProfileRestoreWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        // Files should not actually be restored in dry run
        QVERIFY(!QFile::exists(restoreDir + "/test18/Documents/doc1.txt"));
    }
};

QTEST_MAIN(TestUserProfileRestoreWorker)
#include "test_user_profile_restore_worker.moc"

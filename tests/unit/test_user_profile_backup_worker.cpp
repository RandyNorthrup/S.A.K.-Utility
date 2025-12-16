// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * Unit tests for UserProfileBackupWorker
 * Tests user profile backup execution
 */

#include "sak/workers/user_profile_backup_worker.h"
#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>

class TestUserProfileBackupWorker : public QObject {
    Q_OBJECT

private:
    QTemporaryDir* tempDir;
    QString profileDir;
    QString backupDir;

private slots:
    void initTestCase() {
        tempDir = new QTemporaryDir();
        QVERIFY(tempDir->isValid());
        
        profileDir = tempDir->path() + "/profile";
        backupDir = tempDir->path() + "/backup";
        
        QDir().mkpath(profileDir);
        QDir().mkpath(backupDir);
        
        createTestProfile();
    }

    void cleanupTestCase() {
        delete tempDir;
    }

    void createTestProfile() {
        QDir(profileDir).mkpath("Documents");
        QDir(profileDir).mkpath("Pictures");
        QDir(profileDir).mkpath("AppData/Roaming");
        QDir(profileDir).mkpath("Desktop");
        
        createFile(profileDir + "/Documents/doc1.txt", "Document 1");
        createFile(profileDir + "/Pictures/pic1.jpg", "Image data");
        createFile(profileDir + "/AppData/Roaming/config.ini", "[Settings]");
        createFile(profileDir + "/Desktop/readme.txt", "Desktop file");
    }

    void createFile(const QString& path, const QString& content) {
        QFile file(path);
        file.open(QIODevice::WriteOnly);
        file.write(content.toUtf8());
        file.close();
    }

    void testInitialization() {
        sak::UserProfileBackupWorker worker;
        
        QVERIFY(!worker.isRunning());
        QCOMPARE(worker.getProgress(), 0);
    }

    void testSetProfileDirectory() {
        sak::UserProfileBackupWorker worker;
        
        worker.setProfileDirectory(profileDir);
        
        QCOMPARE(worker.getProfileDirectory(), profileDir);
    }

    void testSetBackupDirectory() {
        sak::UserProfileBackupWorker worker;
        
        worker.setBackupDirectory(backupDir);
        
        QCOMPARE(worker.getBackupDirectory(), backupDir);
    }

    void testStartBackup() {
        sak::UserProfileBackupWorker worker;
        worker.setProfileDirectory(profileDir);
        worker.setBackupDirectory(backupDir + "/test1");
        
        QSignalSpy startedSpy(&worker, &sak::UserProfileBackupWorker::started);
        
        worker.start();
        
        QVERIFY(startedSpy.wait(1000));
        QVERIFY(worker.isRunning());
    }

    void testProgressReporting() {
        sak::UserProfileBackupWorker worker;
        worker.setProfileDirectory(profileDir);
        worker.setBackupDirectory(backupDir + "/test2");
        
        QSignalSpy progressSpy(&worker, &sak::UserProfileBackupWorker::progress);
        
        worker.start();
        
        QVERIFY(progressSpy.wait(5000));
        QVERIFY(progressSpy.count() > 0);
    }

    void testBackupDocuments() {
        sak::UserProfileBackupWorker worker;
        QString testBackup = backupDir + "/test3";
        
        worker.setProfileDirectory(profileDir);
        worker.setBackupDirectory(testBackup);
        worker.setFolderSelection({"Documents"});
        
        QSignalSpy completedSpy(&worker, &sak::UserProfileBackupWorker::completed);
        
        worker.start();
        
        QVERIFY(completedSpy.wait(10000));
        QVERIFY(QFile::exists(testBackup + "/Documents/doc1.txt"));
    }

    void testBackupMultipleFolders() {
        sak::UserProfileBackupWorker worker;
        QString testBackup = backupDir + "/test4";
        
        worker.setProfileDirectory(profileDir);
        worker.setBackupDirectory(testBackup);
        worker.setFolderSelection({"Documents", "Pictures", "Desktop"});
        
        QSignalSpy completedSpy(&worker, &sak::UserProfileBackupWorker::completed);
        
        worker.start();
        
        QVERIFY(completedSpy.wait(10000));
        
        QVERIFY(QFile::exists(testBackup + "/Documents/doc1.txt"));
        QVERIFY(QFile::exists(testBackup + "/Pictures/pic1.jpg"));
        QVERIFY(QFile::exists(testBackup + "/Desktop/readme.txt"));
    }

    void testBackupAppData() {
        sak::UserProfileBackupWorker worker;
        QString testBackup = backupDir + "/test5";
        
        worker.setProfileDirectory(profileDir);
        worker.setBackupDirectory(testBackup);
        worker.setFolderSelection({"AppData"});
        
        QSignalSpy completedSpy(&worker, &sak::UserProfileBackupWorker::completed);
        
        worker.start();
        
        QVERIFY(completedSpy.wait(10000));
        QVERIFY(QFile::exists(testBackup + "/AppData/Roaming/config.ini"));
    }

    void testCancellation() {
        sak::UserProfileBackupWorker worker;
        worker.setProfileDirectory(profileDir);
        worker.setBackupDirectory(backupDir + "/test6");
        worker.setFolderSelection({"Documents", "Pictures", "Desktop", "AppData"});
        
        QSignalSpy cancelledSpy(&worker, &sak::UserProfileBackupWorker::cancelled);
        
        worker.start();
        
        QTimer::singleShot(200, &worker, &sak::UserProfileBackupWorker::cancel);
        
        QVERIFY(cancelledSpy.wait(5000));
        QVERIFY(worker.wasCancelled());
    }

    void testExclusionPatterns() {
        sak::UserProfileBackupWorker worker;
        QString testBackup = backupDir + "/test7";
        
        worker.setProfileDirectory(profileDir);
        worker.setBackupDirectory(testBackup);
        worker.setFolderSelection({"Documents"});
        worker.setExclusionPatterns({"*.txt"});
        
        QSignalSpy completedSpy(&worker, &sak::UserProfileBackupWorker::completed);
        
        worker.start();
        
        QVERIFY(completedSpy.wait(10000));
        QVERIFY(!QFile::exists(testBackup + "/Documents/doc1.txt"));
    }

    void testInclusionPatterns() {
        sak::UserProfileBackupWorker worker;
        QString testBackup = backupDir + "/test8";
        
        worker.setProfileDirectory(profileDir);
        worker.setBackupDirectory(testBackup);
        worker.setFolderSelection({"Pictures", "Documents"});
        worker.setInclusionPatterns({"*.jpg"});
        
        QSignalSpy completedSpy(&worker, &sak::UserProfileBackupWorker::completed);
        
        worker.start();
        
        QVERIFY(completedSpy.wait(10000));
        QVERIFY(QFile::exists(testBackup + "/Pictures/pic1.jpg"));
        QVERIFY(!QFile::exists(testBackup + "/Documents/doc1.txt"));
    }

    void testBackupManifest() {
        sak::UserProfileBackupWorker worker;
        QString testBackup = backupDir + "/test9";
        
        worker.setProfileDirectory(profileDir);
        worker.setBackupDirectory(testBackup);
        worker.setFolderSelection({"Documents"});
        worker.setCreateManifest(true);
        
        QSignalSpy completedSpy(&worker, &sak::UserProfileBackupWorker::completed);
        
        worker.start();
        
        QVERIFY(completedSpy.wait(10000));
        QVERIFY(QFile::exists(testBackup + "/manifest.json"));
    }

    void testCompressionEnabled() {
        sak::UserProfileBackupWorker worker;
        worker.setProfileDirectory(profileDir);
        worker.setBackupDirectory(backupDir + "/test10");
        worker.setFolderSelection({"Documents"});
        worker.setCompressionEnabled(true);
        
        QVERIFY(worker.isCompressionEnabled());
        
        QSignalSpy completedSpy(&worker, &sak::UserProfileBackupWorker::completed);
        
        worker.start();
        
        QVERIFY(completedSpy.wait(10000));
    }

    void testErrorHandling() {
        sak::UserProfileBackupWorker worker;
        worker.setProfileDirectory("/nonexistent/profile");
        worker.setBackupDirectory(backupDir + "/test11");
        
        QSignalSpy errorSpy(&worker, &sak::UserProfileBackupWorker::error);
        
        worker.start();
        
        QVERIFY(errorSpy.wait(5000));
        QVERIFY(errorSpy.count() > 0);
    }

    void testGetBackedUpFileCount() {
        sak::UserProfileBackupWorker worker;
        worker.setProfileDirectory(profileDir);
        worker.setBackupDirectory(backupDir + "/test12");
        worker.setFolderSelection({"Documents", "Pictures", "Desktop"});
        
        QSignalSpy completedSpy(&worker, &sak::UserProfileBackupWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        int count = worker.getBackedUpFileCount();
        QVERIFY(count >= 3);
    }

    void testGetBackedUpSize() {
        sak::UserProfileBackupWorker worker;
        worker.setProfileDirectory(profileDir);
        worker.setBackupDirectory(backupDir + "/test13");
        worker.setFolderSelection({"Documents"});
        
        QSignalSpy completedSpy(&worker, &sak::UserProfileBackupWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        qint64 size = worker.getBackedUpSize();
        QVERIFY(size > 0);
    }

    void testCurrentFolderSignal() {
        sak::UserProfileBackupWorker worker;
        worker.setProfileDirectory(profileDir);
        worker.setBackupDirectory(backupDir + "/test14");
        worker.setFolderSelection({"Documents", "Pictures"});
        
        QSignalSpy folderSpy(&worker, &sak::UserProfileBackupWorker::currentFolder);
        
        worker.start();
        
        QVERIFY(folderSpy.wait(10000));
        QVERIFY(folderSpy.count() >= 2);
    }

    void testStatusMessages() {
        sak::UserProfileBackupWorker worker;
        worker.setProfileDirectory(profileDir);
        worker.setBackupDirectory(backupDir + "/test15");
        worker.setFolderSelection({"Documents"});
        
        QSignalSpy statusSpy(&worker, &sak::UserProfileBackupWorker::statusChanged);
        
        worker.start();
        
        QVERIFY(statusSpy.wait(10000));
        QVERIFY(statusSpy.count() > 0);
    }

    void testSkipHiddenFiles() {
        // Create hidden file
        QString hiddenFile = profileDir + "/Documents/.hidden";
        createFile(hiddenFile, "Hidden content");
        
        sak::UserProfileBackupWorker worker;
        QString testBackup = backupDir + "/test16";
        
        worker.setProfileDirectory(profileDir);
        worker.setBackupDirectory(testBackup);
        worker.setFolderSelection({"Documents"});
        worker.setSkipHiddenFiles(true);
        
        QSignalSpy completedSpy(&worker, &sak::UserProfileBackupWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        QVERIFY(!QFile::exists(testBackup + "/Documents/.hidden"));
    }

    void testPauseResume() {
        sak::UserProfileBackupWorker worker;
        worker.setProfileDirectory(profileDir);
        worker.setBackupDirectory(backupDir + "/test17");
        worker.setFolderSelection({"Documents", "Pictures", "Desktop", "AppData"});
        
        QSignalSpy pausedSpy(&worker, &sak::UserProfileBackupWorker::paused);
        QSignalSpy resumedSpy(&worker, &sak::UserProfileBackupWorker::resumed);
        
        worker.start();
        
        QTimer::singleShot(500, &worker, &sak::UserProfileBackupWorker::pause);
        
        if (pausedSpy.wait(2000)) {
            QVERIFY(worker.isPaused());
            
            QTimer::singleShot(500, &worker, &sak::UserProfileBackupWorker::resume);
            
            QVERIFY(resumedSpy.wait(2000));
            QVERIFY(!worker.isPaused());
        }
    }

    void testElapsedTime() {
        sak::UserProfileBackupWorker worker;
        worker.setProfileDirectory(profileDir);
        worker.setBackupDirectory(backupDir + "/test18");
        worker.setFolderSelection({"Documents"});
        
        worker.start();
        
        QTest::qWait(1000);
        
        qint64 elapsed = worker.getElapsedTime();
        QVERIFY(elapsed >= 900);
    }

    void testVerifyBackup() {
        sak::UserProfileBackupWorker worker;
        QString testBackup = backupDir + "/test19";
        
        worker.setProfileDirectory(profileDir);
        worker.setBackupDirectory(testBackup);
        worker.setFolderSelection({"Documents"});
        worker.setVerificationEnabled(true);
        
        QSignalSpy verifiedSpy(&worker, &sak::UserProfileBackupWorker::verified);
        
        worker.start();
        
        QVERIFY(verifiedSpy.wait(10000));
    }
};

QTEST_MAIN(TestUserProfileBackupWorker)
#include "test_user_profile_backup_worker.moc"

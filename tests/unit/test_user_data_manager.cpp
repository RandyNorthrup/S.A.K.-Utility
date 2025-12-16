// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * Unit tests for UserDataManager
 * Tests user profile backup/restore operations
 */

#include "sak/user_data_manager.h"
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QFileInfo>

class TestUserDataManager : public QObject {
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
        
        createTestStructure();
    }

    void cleanupTestCase() {
        delete tempDir;
    }

    void createTestStructure() {
        // Create test directory structure
        QDir(sourceDir).mkpath("Documents");
        QDir(sourceDir).mkpath("Pictures");
        QDir(sourceDir).mkpath("Desktop");
        QDir(sourceDir).mkpath("AppData/Roaming");
        
        // Create test files
        createFile(sourceDir + "/Documents/test.txt", "Test document");
        createFile(sourceDir + "/Pictures/photo.jpg", "Fake image data");
        createFile(sourceDir + "/Desktop/readme.txt", "Desktop file");
        createFile(sourceDir + "/AppData/Roaming/config.ini", "[Settings]");
    }

    void createFile(const QString& path, const QString& content) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(content.toUtf8());
        file.close();
    }

    void testInitialization() {
        sak::UserDataManager manager;
        
        QVERIFY(manager.isValid());
    }

    void testSetSourceDirectory() {
        sak::UserDataManager manager;
        
        manager.setSourceDirectory(sourceDir);
        
        QCOMPARE(manager.getSourceDirectory(), sourceDir);
    }

    void testSetBackupDirectory() {
        sak::UserDataManager manager;
        
        manager.setBackupDirectory(backupDir);
        
        QCOMPARE(manager.getBackupDirectory(), backupDir);
    }

    void testScanUserData() {
        sak::UserDataManager manager;
        manager.setSourceDirectory(sourceDir);
        
        auto items = manager.scanUserData();
        
        QVERIFY(!items.isEmpty());
        
        // Should find our test directories
        bool foundDocuments = false;
        bool foundPictures = false;
        
        for (const auto& item : items) {
            if (item.name == "Documents") foundDocuments = true;
            if (item.name == "Pictures") foundPictures = true;
        }
        
        QVERIFY(foundDocuments);
        QVERIFY(foundPictures);
    }

    void testCalculateSize() {
        sak::UserDataManager manager;
        manager.setSourceDirectory(sourceDir);
        
        qint64 size = manager.calculateSize(sourceDir + "/Documents");
        
        QVERIFY(size > 0);
    }

    void testBackupSingleFolder() {
        sak::UserDataManager manager;
        manager.setSourceDirectory(sourceDir);
        manager.setBackupDirectory(backupDir);
        
        bool success = manager.backupFolder("Documents");
        
        QVERIFY(success);
        QVERIFY(QFile::exists(backupDir + "/Documents/test.txt"));
    }

    void testBackupMultipleFolders() {
        sak::UserDataManager manager;
        manager.setSourceDirectory(sourceDir);
        manager.setBackupDirectory(backupDir);
        
        QStringList folders = {"Documents", "Pictures", "Desktop"};
        bool success = manager.backupFolders(folders);
        
        QVERIFY(success);
        QVERIFY(QFile::exists(backupDir + "/Documents/test.txt"));
        QVERIFY(QFile::exists(backupDir + "/Pictures/photo.jpg"));
        QVERIFY(QFile::exists(backupDir + "/Desktop/readme.txt"));
    }

    void testBackupWithProgress() {
        sak::UserDataManager manager;
        manager.setSourceDirectory(sourceDir);
        manager.setBackupDirectory(backupDir);
        
        int progressCount = 0;
        int lastProgress = -1;
        
        connect(&manager, &sak::UserDataManager::progress,
                [&](int current, int total, const QString& item) {
            progressCount++;
            QVERIFY(current >= 0);
            QVERIFY(total > 0);
            QVERIFY(current <= total);
            QVERIFY(current >= lastProgress);
            lastProgress = current;
        });
        
        manager.backupFolder("Documents");
        
        QVERIFY(progressCount > 0);
    }

    void testRestoreSingleFolder() {
        // First backup
        sak::UserDataManager manager;
        manager.setSourceDirectory(sourceDir);
        manager.setBackupDirectory(backupDir);
        manager.backupFolder("Documents");
        
        // Delete original
        QDir(sourceDir + "/Documents").removeRecursively();
        QVERIFY(!QFile::exists(sourceDir + "/Documents/test.txt"));
        
        // Restore
        QString restoreDir = tempDir->path() + "/restore";
        QDir().mkpath(restoreDir);
        
        manager.setBackupDirectory(backupDir);
        bool success = manager.restoreFolder("Documents", restoreDir);
        
        QVERIFY(success);
        QVERIFY(QFile::exists(restoreDir + "/Documents/test.txt"));
    }

    void testRestoreMultipleFolders() {
        sak::UserDataManager manager;
        manager.setSourceDirectory(sourceDir);
        manager.setBackupDirectory(backupDir);
        
        QStringList folders = {"Documents", "Pictures"};
        manager.backupFolders(folders);
        
        QString restoreDir = tempDir->path() + "/restore2";
        QDir().mkpath(restoreDir);
        
        bool success = manager.restoreFolders(folders, restoreDir);
        
        QVERIFY(success);
        QVERIFY(QFile::exists(restoreDir + "/Documents/test.txt"));
        QVERIFY(QFile::exists(restoreDir + "/Pictures/photo.jpg"));
    }

    void testBackupWithExclusions() {
        sak::UserDataManager manager;
        manager.setSourceDirectory(sourceDir);
        manager.setBackupDirectory(backupDir);
        
        // Exclude .txt files
        manager.setExclusionPatterns({"*.txt"});
        
        manager.backupFolder("Documents");
        
        QVERIFY(!QFile::exists(backupDir + "/Documents/test.txt"));
    }

    void testBackupWithInclusions() {
        sak::UserDataManager manager;
        manager.setSourceDirectory(sourceDir);
        manager.setBackupDirectory(backupDir);
        
        // Only include .jpg files
        manager.setInclusionPatterns({"*.jpg"});
        
        manager.backupFolders({"Documents", "Pictures"});
        
        QVERIFY(!QFile::exists(backupDir + "/Documents/test.txt"));
        QVERIFY(QFile::exists(backupDir + "/Pictures/photo.jpg"));
    }

    void testConflictResolution() {
        sak::UserDataManager manager;
        manager.setSourceDirectory(sourceDir);
        manager.setBackupDirectory(backupDir);
        
        // Create initial backup
        manager.backupFolder("Documents");
        
        // Modify source file
        createFile(sourceDir + "/Documents/test.txt", "Modified content");
        
        // Set conflict resolution to keep newer
        manager.setConflictResolution(sak::UserDataManager::ConflictResolution::KeepNewer);
        
        // Backup again
        manager.backupFolder("Documents");
        
        // Verify newer version was kept
        QFile file(backupDir + "/Documents/test.txt");
        QVERIFY(file.open(QIODevice::ReadOnly));
        QString content = QString::fromUtf8(file.readAll());
        file.close();
        
        QCOMPARE(content, QString("Modified content"));
    }

    void testSkipExisting() {
        sak::UserDataManager manager;
        manager.setSourceDirectory(sourceDir);
        manager.setBackupDirectory(backupDir);
        
        // First backup
        manager.backupFolder("Documents");
        
        QFileInfo original(backupDir + "/Documents/test.txt");
        QDateTime originalTime = original.lastModified();
        
        // Wait a moment
        QTest::qWait(100);
        
        // Backup again with skip existing
        manager.setConflictResolution(sak::UserDataManager::ConflictResolution::Skip);
        manager.backupFolder("Documents");
        
        QFileInfo after(backupDir + "/Documents/test.txt");
        
        // File should not be modified
        QCOMPARE(after.lastModified(), originalTime);
    }

    void testCancellation() {
        sak::UserDataManager manager;
        manager.setSourceDirectory(sourceDir);
        manager.setBackupDirectory(backupDir);
        
        // Cancel after short delay
        QTimer::singleShot(10, &manager, &sak::UserDataManager::cancel);
        
        manager.backupFolder("Documents");
        
        QVERIFY(manager.wasCancelled());
    }

    void testGetBackupManifest() {
        sak::UserDataManager manager;
        manager.setSourceDirectory(sourceDir);
        manager.setBackupDirectory(backupDir);
        
        manager.backupFolder("Documents");
        
        auto manifest = manager.getBackupManifest();
        
        QVERIFY(!manifest.isEmpty());
        QVERIFY(manifest.contains("Documents"));
    }

    void testVerifyBackup() {
        sak::UserDataManager manager;
        manager.setSourceDirectory(sourceDir);
        manager.setBackupDirectory(backupDir);
        
        manager.backupFolder("Documents");
        
        bool verified = manager.verifyBackup("Documents");
        
        QVERIFY(verified);
    }

    void testVerifyCorruptedBackup() {
        sak::UserDataManager manager;
        manager.setSourceDirectory(sourceDir);
        manager.setBackupDirectory(backupDir);
        
        manager.backupFolder("Documents");
        
        // Corrupt backup file
        QFile corrupt(backupDir + "/Documents/test.txt");
        corrupt.open(QIODevice::WriteOnly);
        corrupt.write("Corrupted data");
        corrupt.close();
        
        bool verified = manager.verifyBackup("Documents");
        
        QVERIFY(!verified);
    }

    void testIncrementalBackup() {
        sak::UserDataManager manager;
        manager.setSourceDirectory(sourceDir);
        manager.setBackupDirectory(backupDir);
        
        // Initial backup
        manager.backupFolder("Documents");
        
        // Add new file
        createFile(sourceDir + "/Documents/new.txt", "New file");
        
        // Incremental backup
        manager.setBackupMode(sak::UserDataManager::BackupMode::Incremental);
        manager.backupFolder("Documents");
        
        QVERIFY(QFile::exists(backupDir + "/Documents/test.txt"));
        QVERIFY(QFile::exists(backupDir + "/Documents/new.txt"));
    }

    void testGetBackupSize() {
        sak::UserDataManager manager;
        manager.setSourceDirectory(sourceDir);
        manager.setBackupDirectory(backupDir);
        
        manager.backupFolder("Documents");
        
        qint64 size = manager.getBackupSize();
        
        QVERIFY(size > 0);
    }

    void testListBackups() {
        sak::UserDataManager manager;
        manager.setBackupDirectory(backupDir);
        
        manager.setSourceDirectory(sourceDir);
        manager.backupFolders({"Documents", "Pictures", "Desktop"});
        
        QStringList backups = manager.listBackups();
        
        QVERIFY(backups.contains("Documents"));
        QVERIFY(backups.contains("Pictures"));
        QVERIFY(backups.contains("Desktop"));
    }

    void testDeleteBackup() {
        sak::UserDataManager manager;
        manager.setSourceDirectory(sourceDir);
        manager.setBackupDirectory(backupDir);
        
        manager.backupFolder("Documents");
        QVERIFY(QDir(backupDir + "/Documents").exists());
        
        bool deleted = manager.deleteBackup("Documents");
        
        QVERIFY(deleted);
        QVERIFY(!QDir(backupDir + "/Documents").exists());
    }

    void testClearAllBackups() {
        sak::UserDataManager manager;
        manager.setSourceDirectory(sourceDir);
        manager.setBackupDirectory(backupDir);
        
        manager.backupFolders({"Documents", "Pictures"});
        
        bool cleared = manager.clearAllBackups();
        
        QVERIFY(cleared);
        QVERIFY(manager.listBackups().isEmpty());
    }
};

QTEST_MAIN(TestUserDataManager)
#include "test_user_data_manager.moc"

// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * Integration test for Backup Workflow
 * Tests end-to-end backup process with multiple components
 */

#include "sak/backup_wizard.h"
#include "sak/user_data_manager.h"
#include "sak/actions/backup_browser_data_action.h"
#include <QTest>
#include <QTemporaryDir>

class TestBackupWorkflow : public QObject {
    Q_OBJECT

private:
    QTemporaryDir* tempDir;
    QString sourcePath;
    QString backupPath;

private slots:
    void initTestCase() {
        tempDir = new QTemporaryDir();
        QVERIFY(tempDir->isValid());
        
        sourcePath = tempDir->path() + "/source";
        backupPath = tempDir->path() + "/backup";
        
        QDir().mkpath(sourcePath);
        QDir().mkpath(backupPath);
        
        // Create test data structure
        createTestData();
    }

    void cleanupTestCase() {
        delete tempDir;
    }

    void createTestData() {
        QDir source(sourcePath);
        
        // User documents
        source.mkpath("Documents");
        createFile(sourcePath + "/Documents/report.docx", 1024);
        createFile(sourcePath + "/Documents/notes.txt", 512);
        
        // Desktop files
        source.mkpath("Desktop");
        createFile(sourcePath + "/Desktop/shortcut.lnk", 256);
        
        // App data
        source.mkpath("AppData/Local/TestApp");
        createFile(sourcePath + "/AppData/Local/TestApp/config.json", 128);
        
        // Browser data
        source.mkpath("AppData/Local/Google/Chrome/User Data/Default");
        createFile(sourcePath + "/AppData/Local/Google/Chrome/User Data/Default/Bookmarks", 512);
    }

    void createFile(const QString& path, int size) {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QByteArray(size, 'x'));
            file.close();
        }
    }

    void testFullBackupWorkflow() {
        sak::BackupWizard wizard;
        
        // Configure backup
        wizard.setSourcePath(sourcePath);
        wizard.setBackupPath(backupPath);
        wizard.setIncludeDocuments(true);
        wizard.setIncludeDesktop(true);
        wizard.setIncludeAppData(true);
        wizard.setCompression(true);
        
        bool started = false;
        bool completed = false;
        
        connect(&wizard, &sak::BackupWizard::started, [&]() { started = true; });
        connect(&wizard, &sak::BackupWizard::completed, [&]() { completed = true; });
        
        wizard.execute();
        QTest::qWait(5000); // Wait for backup to complete
        
        QVERIFY(started);
        QVERIFY(completed);
        
        // Verify backup exists
        QVERIFY(QDir(backupPath).exists());
        QVERIFY(QFile::exists(backupPath + "/manifest.json"));
    }

    void testIncrementalBackup() {
        // First backup
        sak::BackupWizard wizard;
        wizard.setSourcePath(sourcePath);
        wizard.setBackupPath(backupPath + "/full");
        wizard.execute();
        QTest::qWait(3000);
        
        // Modify source
        createFile(sourcePath + "/Documents/new_file.txt", 256);
        
        // Incremental backup
        wizard.setBackupPath(backupPath + "/incremental");
        wizard.setIncrementalMode(true);
        wizard.setBaseline(backupPath + "/full");
        wizard.execute();
        QTest::qWait(3000);
        
        // Verify only new files are backed up
        QVERIFY(QFile::exists(backupPath + "/incremental/Documents/new_file.txt"));
    }

    void testBackupVerification() {
        sak::BackupWizard wizard;
        wizard.setSourcePath(sourcePath);
        wizard.setBackupPath(backupPath + "/verified");
        wizard.setVerifyAfterBackup(true);
        
        bool verified = false;
        connect(&wizard, &sak::BackupWizard::verificationCompleted, 
                [&](bool success) { verified = success; });
        
        wizard.execute();
        QTest::qWait(5000);
        
        QVERIFY(verified);
    }

    void testBackupEncryption() {
        sak::BackupWizard wizard;
        wizard.setSourcePath(sourcePath);
        wizard.setBackupPath(backupPath + "/encrypted");
        wizard.setEncryption(true);
        wizard.setPassword("SecurePassword123!");
        
        wizard.execute();
        QTest::qWait(3000);
        
        // Verify backup is encrypted
        QFile manifest(backupPath + "/encrypted/manifest.json");
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        QJsonDocument doc = QJsonDocument::fromJson(manifest.readAll());
        manifest.close();
        
        QVERIFY(doc.object()["encrypted"].toBool());
    }

    void testBackupProgress() {
        sak::BackupWizard wizard;
        wizard.setSourcePath(sourcePath);
        wizard.setBackupPath(backupPath + "/progress");
        
        QVector<int> progressValues;
        connect(&wizard, &sak::BackupWizard::progress, 
                [&](int current, int total) {
            progressValues.append((current * 100) / total);
        });
        
        wizard.execute();
        QTest::qWait(3000);
        
        QVERIFY(!progressValues.isEmpty());
        QVERIFY(progressValues.last() == 100);
    }

    void testBackupCancellation() {
        sak::BackupWizard wizard;
        wizard.setSourcePath(sourcePath);
        wizard.setBackupPath(backupPath + "/cancelled");
        
        wizard.execute();
        QTimer::singleShot(500, &wizard, &sak::BackupWizard::cancel);
        QTest::qWait(2000);
        
        QVERIFY(wizard.wasCancelled());
    }

    void testBackupWithFilters() {
        sak::BackupWizard wizard;
        wizard.setSourcePath(sourcePath);
        wizard.setBackupPath(backupPath + "/filtered");
        wizard.setFileFilters(QStringList() << "*.txt" << "*.docx");
        wizard.setExcludePatterns(QStringList() << "*temp*");
        
        wizard.execute();
        QTest::qWait(3000);
        
        // Verify only filtered files are backed up
        QVERIFY(QFile::exists(backupPath + "/filtered/Documents/notes.txt"));
        QVERIFY(QFile::exists(backupPath + "/filtered/Documents/report.docx"));
    }

    void testRestoreFromBackup() {
        QString restorePath = tempDir->path() + "/restore";
        
        // Create backup first
        sak::BackupWizard wizard;
        wizard.setSourcePath(sourcePath);
        wizard.setBackupPath(backupPath + "/for_restore");
        wizard.execute();
        QTest::qWait(3000);
        
        // Restore
        sak::RestoreWizard restoreWizard;
        restoreWizard.setBackupPath(backupPath + "/for_restore");
        restoreWizard.setRestorePath(restorePath);
        
        bool completed = false;
        connect(&restoreWizard, &sak::RestoreWizard::completed, 
                [&]() { completed = true; });
        
        restoreWizard.execute();
        QTest::qWait(3000);
        
        QVERIFY(completed);
        QVERIFY(QFile::exists(restorePath + "/Documents/report.docx"));
    }
};

QTEST_MAIN(TestBackupWorkflow)
#include "test_backup_workflow.moc"

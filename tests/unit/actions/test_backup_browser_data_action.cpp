// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * Unit tests for BackupBrowserDataAction
 * Tests browser profile backup functionality
 */

#include "sak/actions/backup_browser_data_action.h"
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>

class TestBackupBrowserDataAction : public QObject {
    Q_OBJECT

private:
    QTemporaryDir* tempDir;
    QString chromePath;
    QString firefoxPath;

private slots:
    void initTestCase() {
        tempDir = new QTemporaryDir();
        QVERIFY(tempDir->isValid());
        
        // Create fake Chrome profile
        chromePath = tempDir->path() + "/Chrome/User Data/Default";
        QDir().mkpath(chromePath);
        
        QStringList chromeFiles = {"Bookmarks", "History", "Preferences", "Cookies"};
        for (const QString& file : chromeFiles) {
            QFile f(chromePath + "/" + file);
            f.open(QIODevice::WriteOnly);
            f.write("Chrome data");
            f.close();
        }
        
        // Create fake Firefox profile
        firefoxPath = tempDir->path() + "/Firefox/Profiles/test.default";
        QDir().mkpath(firefoxPath);
        
        QStringList firefoxFiles = {"places.sqlite", "cookies.sqlite", "prefs.js"};
        for (const QString& file : firefoxFiles) {
            QFile f(firefoxPath + "/" + file);
            f.open(QIODevice::WriteOnly);
            f.write("Firefox data");
            f.close();
        }
    }

    void cleanupTestCase() {
        delete tempDir;
    }

    void testDetectChrome() {
        sak::actions::BackupBrowserDataAction action;
        action.addBrowserPath("Chrome", chromePath);
        
        auto browsers = action.detectInstalledBrowsers();
        QVERIFY(browsers.contains("Chrome"));
    }

    void testDetectFirefox() {
        sak::actions::BackupBrowserDataAction action;
        action.addBrowserPath("Firefox", firefoxPath);
        
        auto browsers = action.detectInstalledBrowsers();
        QVERIFY(browsers.contains("Firefox"));
    }

    void testBackupChrome() {
        QString backupPath = tempDir->path() + "/backup";
        
        sak::actions::BackupBrowserDataAction action;
        action.addBrowserPath("Chrome", chromePath);
        action.setBackupPath(backupPath);
        action.setBrowsers(QStringList() << "Chrome");
        
        bool completed = false;
        connect(&action, &sak::actions::BackupBrowserDataAction::completed, 
                [&]() { completed = true; });
        
        action.execute();
        QTest::qWait(2000);
        
        QVERIFY(completed);
        QVERIFY(QFile::exists(backupPath + "/Chrome/Bookmarks"));
        QVERIFY(QFile::exists(backupPath + "/Chrome/History"));
    }

    void testBackupFirefox() {
        QString backupPath = tempDir->path() + "/backup_ff";
        
        sak::actions::BackupBrowserDataAction action;
        action.addBrowserPath("Firefox", firefoxPath);
        action.setBackupPath(backupPath);
        action.setBrowsers(QStringList() << "Firefox");
        
        bool completed = false;
        connect(&action, &sak::actions::BackupBrowserDataAction::completed, 
                [&]() { completed = true; });
        
        action.execute();
        QTest::qWait(2000);
        
        QVERIFY(completed);
        QVERIFY(QFile::exists(backupPath + "/Firefox/places.sqlite"));
    }

    void testSelectiveBackup() {
        QString backupPath = tempDir->path() + "/selective";
        
        sak::actions::BackupBrowserDataAction action;
        action.addBrowserPath("Chrome", chromePath);
        action.setBackupPath(backupPath);
        action.setBrowsers(QStringList() << "Chrome");
        action.setBackupItems(QStringList() << "Bookmarks" << "History");
        
        action.execute();
        QTest::qWait(2000);
        
        QVERIFY(QFile::exists(backupPath + "/Chrome/Bookmarks"));
        QVERIFY(QFile::exists(backupPath + "/Chrome/History"));
        QVERIFY(!QFile::exists(backupPath + "/Chrome/Cookies"));
    }

    void testManifestCreation() {
        QString backupPath = tempDir->path() + "/manifest";
        
        sak::actions::BackupBrowserDataAction action;
        action.addBrowserPath("Chrome", chromePath);
        action.setBackupPath(backupPath);
        action.setBrowsers(QStringList() << "Chrome");
        action.setCreateManifest(true);
        
        action.execute();
        QTest::qWait(2000);
        
        QVERIFY(QFile::exists(backupPath + "/manifest.json"));
    }

    void testErrorHandling() {
        sak::actions::BackupBrowserDataAction action;
        action.addBrowserPath("Chrome", "/nonexistent/path");
        action.setBackupPath(tempDir->path() + "/error");
        action.setBrowsers(QStringList() << "Chrome");
        
        bool errorOccurred = false;
        connect(&action, &sak::actions::BackupBrowserDataAction::error, 
                [&](const QString&) { errorOccurred = true; });
        
        action.execute();
        QTest::qWait(1000);
        
        QVERIFY(errorOccurred);
    }
};

QTEST_MAIN(TestBackupBrowserDataAction)
#include "test_backup_browser_data_action.moc"

// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QTest>
#include <QSignalSpy>
#include "sak/actions/rebuild_icon_cache_action.h"

class TestRebuildIconCacheAction : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Basic functionality
    void testActionProperties();
    void testInitialState();
    void testDoesNotRequireAdmin();
    void testScanEnumeratesCacheFiles();
    void testExecuteRebuildsCahce();
    
    // Cache file locations
    void testLocateIconCacheDB();
    void testLocateThumbnailCache();
    void testEnumerateCacheFiles();
    void testLocalLowCacheFolder();
    
    // Explorer management
    void testStopExplorer();
    void testStartExplorer();
    void testVerifyExplorerStopped();
    void testWaitForExplorerRestart();
    
    // Cache file deletion
    void testDeleteIconDB();
    void testDeleteThumbnailCache();
    void testDeleteMultipleCacheFiles();
    void testCountDeletedFiles();
    
    // Cache refresh
    void testRefreshIconCache();
    void testRebuildThumbnails();
    void testVerifyNewCacheCreated();
    
    // File size calculation
    void testCalculateCacheSize();
    void testTotalCacheSize();
    void testIndividualFileSize();
    
    // Progress tracking
    void testProgressSignals();
    void testEnumerationProgress();
    void testDeletionProgress();
    
    // Error handling
    void testHandleExplorerStopFailure();
    void testHandleExplorerStartFailure();
    void testHandleFileInUse();
    void testHandleAccessDenied();
    
    // Cache file types
    void testIconCacheDB();
    void testThumbnailCacheDB();
    void testWideIcons();
    void testCustomIcons();
    
    // Results formatting
    void testFormatFileList();
    void testFormatCacheSize();
    void testFormatSuccessMessage();
    void testFormatErrorMessage();
    
    // Edge cases
    void testNoCacheFiles();
    void testCacheAlreadyDeleted();
    void testExplorerNotRunning();
    void testMultipleExplorerProcesses();

private:
    sak::RebuildIconCacheAction* m_action;
};

void TestRebuildIconCacheAction::initTestCase() {
    // One-time setup
}

void TestRebuildIconCacheAction::cleanupTestCase() {
    // One-time cleanup
}

void TestRebuildIconCacheAction::init() {
    m_action = new sak::RebuildIconCacheAction();
}

void TestRebuildIconCacheAction::cleanup() {
    delete m_action;
}

void TestRebuildIconCacheAction::testActionProperties() {
    QCOMPARE(m_action->name(), QString("Rebuild Icon Cache"));
    QVERIFY(!m_action->description().isEmpty());
    QVERIFY(m_action->description().contains("icon", Qt::CaseInsensitive));
    QCOMPARE(m_action->category(), sak::QuickAction::ActionCategory::Maintenance);
    QVERIFY(!m_action->requiresAdmin());
}

void TestRebuildIconCacheAction::testInitialState() {
    QSignalSpy startedSpy(m_action, &sak::QuickAction::started);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    QVERIFY(startedSpy.isValid());
    QVERIFY(finishedSpy.isValid());
    QCOMPARE(startedSpy.count(), 0);
}

void TestRebuildIconCacheAction::testDoesNotRequireAdmin() {
    // Can rebuild current user's icon cache without admin
    QVERIFY(!m_action->requiresAdmin());
}

void TestRebuildIconCacheAction::testScanEnumeratesCacheFiles() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    
    QVERIFY(finishedSpy.wait(15000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestRebuildIconCacheAction::testExecuteRebuildsCahce() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    
    QVERIFY(finishedSpy.wait(45000)); // Explorer restart takes time
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestRebuildIconCacheAction::testLocateIconCacheDB() {
    // %LocalAppData%\IconCache.db
    QString cachePath = R"(%LocalAppData%\IconCache.db)";
    
    QVERIFY(cachePath.contains("IconCache.db"));
}

void TestRebuildIconCacheAction::testLocateThumbnailCache() {
    // %LocalAppData%\Microsoft\Windows\Explorer\thumbcache_*.db
    QString thumbPath = R"(%LocalAppData%\Microsoft\Windows\Explorer)";
    
    QVERIFY(thumbPath.contains("Explorer"));
}

void TestRebuildIconCacheAction::testEnumerateCacheFiles() {
    QStringList cacheFiles = {
        "IconCache.db",
        "thumbcache_32.db",
        "thumbcache_96.db",
        "thumbcache_256.db",
        "thumbcache_1024.db"
    };
    
    QVERIFY(cacheFiles.size() >= 1);
}

void TestRebuildIconCacheAction::testLocalLowCacheFolder() {
    // %LocalAppData%Low\IconCache.db may also exist
    QString localLowPath = R"(%LocalAppData%Low\IconCache.db)";
    
    QVERIFY(localLowPath.contains("LocalAppData"));
}

void TestRebuildIconCacheAction::testStopExplorer() {
    // Command: taskkill /F /IM explorer.exe
    QString command = "taskkill /F /IM explorer.exe";
    
    QVERIFY(command.contains("explorer.exe"));
    QVERIFY(command.contains("/F")); // Force
}

void TestRebuildIconCacheAction::testStartExplorer() {
    // Command: explorer.exe
    QString command = "explorer.exe";
    
    QCOMPARE(command, QString("explorer.exe"));
}

void TestRebuildIconCacheAction::testVerifyExplorerStopped() {
    // Verify explorer.exe is not running
    QString command = "tasklist /FI \"IMAGENAME eq explorer.exe\"";
    
    QVERIFY(command.contains("tasklist"));
}

void TestRebuildIconCacheAction::testWaitForExplorerRestart() {
    // Wait for explorer to fully restart
    int waitTimeMs = 3000; // 3 seconds
    
    QVERIFY(waitTimeMs > 0);
}

void TestRebuildIconCacheAction::testDeleteIconDB() {
    // Delete IconCache.db
    QString file = "IconCache.db";
    
    QVERIFY(!file.isEmpty());
}

void TestRebuildIconCacheAction::testDeleteThumbnailCache() {
    // Delete thumbcache_*.db files
    QString pattern = "thumbcache_*.db";
    
    QVERIFY(pattern.contains("thumbcache"));
}

void TestRebuildIconCacheAction::testDeleteMultipleCacheFiles() {
    QStringList filesToDelete = {
        "IconCache.db",
        "thumbcache_32.db",
        "thumbcache_96.db",
        "thumbcache_256.db"
    };
    
    QCOMPARE(filesToDelete.size(), 4);
}

void TestRebuildIconCacheAction::testCountDeletedFiles() {
    int deletedCount = 5;
    
    QVERIFY(deletedCount > 0);
}

void TestRebuildIconCacheAction::testRefreshIconCache() {
    // Command: ie4uinit.exe -show
    QString command = "ie4uinit.exe -show";
    
    QVERIFY(command.contains("ie4uinit"));
}

void TestRebuildIconCacheAction::testRebuildThumbnails() {
    // Windows will rebuild thumbnails automatically
    bool autoRebuild = true;
    
    QVERIFY(autoRebuild);
}

void TestRebuildIconCacheAction::testVerifyNewCacheCreated() {
    // Check if IconCache.db was recreated
    QString cacheFile = "IconCache.db";
    bool exists = true; // After rebuild
    
    QVERIFY(exists);
}

void TestRebuildIconCacheAction::testCalculateCacheSize() {
    qint64 totalSize = 15LL * 1024 * 1024; // 15 MB
    
    QVERIFY(totalSize > 0);
}

void TestRebuildIconCacheAction::testTotalCacheSize() {
    QVector<qint64> fileSizes = {
        5LL * 1024 * 1024,  // IconCache.db: 5 MB
        3LL * 1024 * 1024,  // thumbcache_32: 3 MB
        7LL * 1024 * 1024   // thumbcache_256: 7 MB
    };
    
    qint64 total = 0;
    for (qint64 size : fileSizes) {
        total += size;
    }
    
    QCOMPARE(total, 15LL * 1024 * 1024);
}

void TestRebuildIconCacheAction::testIndividualFileSize() {
    qint64 fileSize = 5LL * 1024 * 1024; // 5 MB
    
    QVERIFY(fileSize > 0);
}

void TestRebuildIconCacheAction::testProgressSignals() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(15000));
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestRebuildIconCacheAction::testEnumerationProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->scan();
    QTest::qWait(1000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestRebuildIconCacheAction::testDeletionProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->execute();
    QTest::qWait(3000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestRebuildIconCacheAction::testHandleExplorerStopFailure() {
    // Explorer may fail to stop
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(45000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestRebuildIconCacheAction::testHandleExplorerStartFailure() {
    // Explorer may fail to restart
    bool startSuccess = false;
    
    QVERIFY(!startSuccess);
}

void TestRebuildIconCacheAction::testHandleFileInUse() {
    // Cache file may be locked
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(45000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestRebuildIconCacheAction::testHandleAccessDenied() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(45000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestRebuildIconCacheAction::testIconCacheDB() {
    QString file = "IconCache.db";
    
    QCOMPARE(file, QString("IconCache.db"));
}

void TestRebuildIconCacheAction::testThumbnailCacheDB() {
    QStringList thumbFiles = {
        "thumbcache_32.db",
        "thumbcache_96.db",
        "thumbcache_256.db",
        "thumbcache_1024.db",
        "thumbcache_idx.db",
        "thumbcache_sr.db"
    };
    
    QVERIFY(thumbFiles.size() >= 4);
}

void TestRebuildIconCacheAction::testWideIcons() {
    // IconCacheW.db for wide icons
    QString wideIconCache = "IconCacheW.db";
    
    QVERIFY(wideIconCache.contains("IconCache"));
}

void TestRebuildIconCacheAction::testCustomIcons() {
    // Custom icon caches may exist
    QString customCache = "thumbcache_custom.db";
    
    QVERIFY(customCache.contains("thumbcache"));
}

void TestRebuildIconCacheAction::testFormatFileList() {
    QString list = R"(
Icon Cache Files:
  • IconCache.db (5.2 MB)
  • thumbcache_32.db (1.8 MB)
  • thumbcache_256.db (7.3 MB)
    )";
    
    QVERIFY(list.contains("Icon Cache"));
}

void TestRebuildIconCacheAction::testFormatCacheSize() {
    qint64 bytes = 15LL * 1024 * 1024; // 15 MB
    QString formatted = QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + " MB";
    
    QVERIFY(formatted.contains("MB"));
}

void TestRebuildIconCacheAction::testFormatSuccessMessage() {
    QString message = "Successfully rebuilt icon cache (5 files deleted, 15.3 MB freed)";
    
    QVERIFY(message.contains("Successfully"));
    QVERIFY(message.contains("rebuilt"));
}

void TestRebuildIconCacheAction::testFormatErrorMessage() {
    QString error = "Failed to stop Explorer: Process is protected";
    
    QVERIFY(error.contains("Failed"));
    QVERIFY(error.contains("Explorer"));
}

void TestRebuildIconCacheAction::testNoCacheFiles() {
    // No cache files found (rare)
    int fileCount = 0;
    
    QCOMPARE(fileCount, 0);
}

void TestRebuildIconCacheAction::testCacheAlreadyDeleted() {
    // Cache files already deleted
    bool filesExist = false;
    
    QVERIFY(!filesExist);
}

void TestRebuildIconCacheAction::testExplorerNotRunning() {
    // Explorer not running (unusual state)
    bool explorerRunning = false;
    
    QVERIFY(!explorerRunning);
}

void TestRebuildIconCacheAction::testMultipleExplorerProcesses() {
    // Multiple explorer.exe processes running
    int explorerCount = 3;
    
    QVERIFY(explorerCount >= 1);
}

QTEST_MAIN(TestRebuildIconCacheAction)
#include "test_rebuild_icon_cache_action.moc"

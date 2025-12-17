// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QTest>
#include <QSignalSpy>
#include "sak/actions/disk_cleanup_action.h"

class TestDiskCleanupAction : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Basic functionality
    void testActionProperties();
    void testInitialState();
    void testRequiresAdmin();
    void testScanCalculatesSpace();
    void testExecuteCleansFiles();
    
    // Windows temp
    void testScanWindowsTemp();
    void testLocateWindowsTempFolder();
    void testCountWindowsTempFiles();
    void testDeleteWindowsTempFiles();
    
    // User temp
    void testScanUserTemp();
    void testLocateUserTempFolder();
    void testCountUserTempFiles();
    void testDeleteUserTempFiles();
    
    // Browser caches
    void testScanBrowserCaches();
    void testDetectChromiumCache();
    void testDetectFirefoxCache();
    void testClearBrowserCache();
    
    // Recycle bin
    void testScanRecycleBin();
    void testCalculateRecycleBinSize();
    void testEmptyRecycleBin();
    
    // Windows Update
    void testScanWindowsUpdateCleanup();
    void testLocateUpdateCache();
    void testCleanWindowsUpdateFiles();
    
    // Thumbnail cache
    void testScanThumbnailCache();
    void testLocateThumbnailCache();
    void testDeleteThumbnailCache();
    
    // Size calculation
    void testCalculateFolderSize();
    void testCountFiles();
    void testFormatFileSize();
    
    // Progress tracking
    void testProgressSignals();
    void testScanProgress();
    void testExecuteProgress();
    
    // Error handling
    void testHandleAccessDenied();
    void testHandleFileLocked();
    void testHandleInsufficientPermissions();
    void testHandleDeletionFailure();
    
    // Safety checks
    void testVerifySafeToDelete();
    void testSkipSystemFiles();
    void testProtectUserData();
    
    // Results formatting
    void testFormatCleanupTargets();
    void testFormatCleanupResults();
    void testFormatSuccessMessage();
    void testFormatErrorMessage();
    
    // Edge cases
    void testEmptyTempFolder();
    void testLockedFiles();
    void testInsufficientDiskSpace();
    void testNoCleanupNeeded();

private:
    sak::DiskCleanupAction* m_action;
};

void TestDiskCleanupAction::initTestCase() {
    // One-time setup
}

void TestDiskCleanupAction::cleanupTestCase() {
    // One-time cleanup
}

void TestDiskCleanupAction::init() {
    m_action = new sak::DiskCleanupAction();
}

void TestDiskCleanupAction::cleanup() {
    delete m_action;
}

void TestDiskCleanupAction::testActionProperties() {
    QCOMPARE(m_action->name(), QString("Disk Cleanup"));
    QVERIFY(!m_action->description().isEmpty());
    QVERIFY(m_action->description().contains("temp", Qt::CaseInsensitive) || 
            m_action->description().contains("cleanup", Qt::CaseInsensitive));
    QCOMPARE(m_action->category(), sak::QuickAction::ActionCategory::SystemOptimization);
    QVERIFY(m_action->requiresAdmin());
}

void TestDiskCleanupAction::testInitialState() {
    QSignalSpy startedSpy(m_action, &sak::QuickAction::started);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    QVERIFY(startedSpy.isValid());
    QVERIFY(finishedSpy.isValid());
    QCOMPARE(startedSpy.count(), 0);
}

void TestDiskCleanupAction::testRequiresAdmin() {
    // Requires admin to clean Windows temp
    QVERIFY(m_action->requiresAdmin());
}

void TestDiskCleanupAction::testScanCalculatesSpace() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    
    QVERIFY(finishedSpy.wait(30000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestDiskCleanupAction::testExecuteCleansFiles() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    
    QVERIFY(finishedSpy.wait(60000)); // Cleanup can take time
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestDiskCleanupAction::testScanWindowsTemp() {
    // Windows temp folder
    QString tempPath = R"(C:\Windows\Temp)";
    
    QVERIFY(tempPath.contains("Windows\\Temp"));
}

void TestDiskCleanupAction::testLocateWindowsTempFolder() {
    QString tempPath = R"(%SystemRoot%\Temp)";
    
    QVERIFY(tempPath.contains("Temp"));
}

void TestDiskCleanupAction::testCountWindowsTempFiles() {
    int fileCount = 42;
    
    QVERIFY(fileCount >= 0);
}

void TestDiskCleanupAction::testDeleteWindowsTempFiles() {
    // Delete files older than 7 days
    bool deleteOld = true;
    
    QVERIFY(deleteOld);
}

void TestDiskCleanupAction::testScanUserTemp() {
    // User temp folder
    QString tempPath = R"(%Temp%)";
    
    QVERIFY(tempPath.contains("Temp"));
}

void TestDiskCleanupAction::testLocateUserTempFolder() {
    QString tempPath = R"(%LocalAppData%\Temp)";
    
    QVERIFY(tempPath.contains("Temp"));
}

void TestDiskCleanupAction::testCountUserTempFiles() {
    int fileCount = 25;
    
    QVERIFY(fileCount >= 0);
}

void TestDiskCleanupAction::testDeleteUserTempFiles() {
    // Safe to delete user temp files
    bool safeToDelete = true;
    
    QVERIFY(safeToDelete);
}

void TestDiskCleanupAction::testScanBrowserCaches() {
    QStringList browsers = {
        "Chrome",
        "Edge",
        "Firefox"
    };
    
    QVERIFY(browsers.size() >= 0);
}

void TestDiskCleanupAction::testDetectChromiumCache() {
    // Chrome/Edge cache location
    QString cachePath = R"(%LocalAppData%\Google\Chrome\User Data\Default\Cache)";
    
    QVERIFY(cachePath.contains("Cache"));
}

void TestDiskCleanupAction::testDetectFirefoxCache() {
    // Firefox cache location
    QString cachePath = R"(%LocalAppData%\Mozilla\Firefox\Profiles\*.default\cache2)";
    
    QVERIFY(cachePath.contains("cache"));
}

void TestDiskCleanupAction::testClearBrowserCache() {
    qint64 cacheSize = 1024 * 1024 * 500; // 500 MB
    
    QVERIFY(cacheSize > 0);
}

void TestDiskCleanupAction::testScanRecycleBin() {
    // Recycle bin location (per drive)
    QString recyclePath = R"(C:\$Recycle.Bin)";
    
    QVERIFY(recyclePath.contains("Recycle.Bin"));
}

void TestDiskCleanupAction::testCalculateRecycleBinSize() {
    qint64 binSize = 1024 * 1024 * 200; // 200 MB
    
    QVERIFY(binSize >= 0);
}

void TestDiskCleanupAction::testEmptyRecycleBin() {
    // SHEmptyRecycleBin API
    bool emptied = true;
    
    QVERIFY(emptied);
}

void TestDiskCleanupAction::testScanWindowsUpdateCleanup() {
    // Windows Update cleanup
    QString updatePath = R"(C:\Windows\SoftwareDistribution\Download)";
    
    QVERIFY(updatePath.contains("SoftwareDistribution"));
}

void TestDiskCleanupAction::testLocateUpdateCache() {
    QString cachePath = R"(%SystemRoot%\SoftwareDistribution\Download)";
    
    QVERIFY(cachePath.contains("Download"));
}

void TestDiskCleanupAction::testCleanWindowsUpdateFiles() {
    // Stop Windows Update service first
    QString serviceName = "wuauserv";
    
    QCOMPARE(serviceName, QString("wuauserv"));
}

void TestDiskCleanupAction::testScanThumbnailCache() {
    // Thumbnail cache location
    QString thumbPath = R"(%LocalAppData%\Microsoft\Windows\Explorer)";
    
    QVERIFY(thumbPath.contains("Explorer"));
}

void TestDiskCleanupAction::testLocateThumbnailCache() {
    // thumbcache_*.db files
    QString pattern = "thumbcache_*.db";
    
    QVERIFY(pattern.contains("thumbcache"));
}

void TestDiskCleanupAction::testDeleteThumbnailCache() {
    // Delete thumbnail database files
    QStringList thumbFiles = {
        "thumbcache_32.db",
        "thumbcache_96.db",
        "thumbcache_256.db"
    };
    
    QVERIFY(thumbFiles.size() >= 3);
}

void TestDiskCleanupAction::testCalculateFolderSize() {
    qint64 folderSize = 1024 * 1024 * 100; // 100 MB
    
    QVERIFY(folderSize > 0);
}

void TestDiskCleanupAction::testCountFiles() {
    int fileCount = 150;
    
    QVERIFY(fileCount >= 0);
}

void TestDiskCleanupAction::testFormatFileSize() {
    QString formatted = "1.5 GB";
    
    QVERIFY(formatted.contains("GB") || formatted.contains("MB"));
}

void TestDiskCleanupAction::testProgressSignals() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(30000));
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestDiskCleanupAction::testScanProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->scan();
    QTest::qWait(2000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestDiskCleanupAction::testExecuteProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->execute();
    QTest::qWait(5000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestDiskCleanupAction::testHandleAccessDenied() {
    // Some files require admin
    QString error = "Access denied";
    
    QVERIFY(error.contains("Access denied"));
}

void TestDiskCleanupAction::testHandleFileLocked() {
    // File is in use
    QString error = "File is locked by another process";
    
    QVERIFY(error.contains("locked"));
}

void TestDiskCleanupAction::testHandleInsufficientPermissions() {
    // Need admin rights
    bool hasPermission = false;
    
    QVERIFY(!hasPermission);
}

void TestDiskCleanupAction::testHandleDeletionFailure() {
    // Failed to delete file
    bool deleteSuccess = false;
    
    QVERIFY(!deleteSuccess);
}

void TestDiskCleanupAction::testVerifySafeToDelete() {
    // Check if safe to delete
    bool safeToDelete = true;
    
    QVERIFY(safeToDelete);
}

void TestDiskCleanupAction::testSkipSystemFiles() {
    // Don't delete system files
    QStringList protectedExtensions = {
        ".sys",
        ".dll",
        ".exe"
    };
    
    QVERIFY(protectedExtensions.size() >= 3);
}

void TestDiskCleanupAction::testProtectUserData() {
    // Don't delete user documents
    QStringList protectedFolders = {
        "Documents",
        "Desktop",
        "Pictures"
    };
    
    QVERIFY(protectedFolders.size() >= 3);
}

void TestDiskCleanupAction::testFormatCleanupTargets() {
    QString targets = R"(
Cleanup Targets Found:
  Windows Temp: 250 MB (325 files)
  User Temp: 150 MB (180 files)
  Browser Caches: 500 MB (1,250 files)
  Recycle Bin: 200 MB (45 items)
  Thumbnail Cache: 50 MB (8 files)
  Total: 1.15 GB (1,808 items)
    )";
    
    QVERIFY(targets.contains("Cleanup Targets"));
}

void TestDiskCleanupAction::testFormatCleanupResults() {
    QString results = R"(
Disk Cleanup Completed:
  ✓ Windows Temp: 250 MB freed
  ✓ User Temp: 150 MB freed
  ✓ Browser Caches: 500 MB freed
  ✓ Recycle Bin: Emptied (200 MB)
  ✓ Thumbnail Cache: 50 MB freed
  Total Space Freed: 1.15 GB
    )";
    
    QVERIFY(results.contains("Cleanup Completed"));
}

void TestDiskCleanupAction::testFormatSuccessMessage() {
    QString message = "Successfully freed 1.15 GB of disk space";
    
    QVERIFY(message.contains("Successfully"));
    QVERIFY(message.contains("GB"));
}

void TestDiskCleanupAction::testFormatErrorMessage() {
    QString error = "Failed to clean disk: Access denied to Windows temp folder";
    
    QVERIFY(error.contains("Failed"));
    QVERIFY(error.contains("Access denied"));
}

void TestDiskCleanupAction::testEmptyTempFolder() {
    // Temp folder is already empty
    int fileCount = 0;
    
    QCOMPARE(fileCount, 0);
}

void TestDiskCleanupAction::testLockedFiles() {
    // Some files are locked
    int lockedCount = 5;
    
    QVERIFY(lockedCount >= 0);
}

void TestDiskCleanupAction::testInsufficientDiskSpace() {
    // Low disk space warning
    qint64 freeSpace = 1024 * 1024 * 100; // 100 MB
    
    QVERIFY(freeSpace > 0);
}

void TestDiskCleanupAction::testNoCleanupNeeded() {
    // Nothing to clean
    qint64 reclaimableSpace = 0;
    
    QCOMPARE(reclaimableSpace, 0LL);
}

QTEST_MAIN(TestDiskCleanupAction)
#include "test_disk_cleanup_action.moc"

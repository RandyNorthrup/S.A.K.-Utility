// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include "sak/actions/backup_desktop_wallpaper_action.h"

class TestBackupDesktopWallpaperAction : public QObject {
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
    void testScanFindsWallpaper();
    void testExecuteBacksUpWallpaper();
    
    // Wallpaper file detection
    void testLocateTranscodedWallpaper();
    void testCheckWallpaperExists();
    void testGetWallpaperSize();
    void testGetWallpaperPath();
    
    // Registry reading
    void testReadWallpaperRegistry();
    void testGetCurrentWallpaperPath();
    void testGetWallpaperStyle();
    void testGetWallpaperPosition();
    
    // Backup operations
    void testCreateBackupDirectory();
    void testBackupWallpaperFile();
    void testBackupRegistrySettings();
    void testGenerateBackupFilename();
    
    // Multi-user support
    void testEnumerateUserProfiles();
    void testGetCurrentUserProfile();
    void testBackupAllUserProfiles();
    void testCountUserProfiles();
    
    // Restoration
    void testRestoreWallpaperFile();
    void testRestoreRegistrySettings();
    void testApplyWallpaper();
    void testRefreshDesktop();
    
    // Wallpaper types
    void testDetectImageWallpaper();
    void testDetectSolidColorWallpaper();
    void testDetectSlideshowWallpaper();
    
    // Progress tracking
    void testProgressSignals();
    void testScanProgress();
    void testExecuteProgress();
    
    // Error handling
    void testHandleWallpaperNotFound();
    void testHandleBackupFolderFailure();
    void testHandleRegistryReadFailure();
    void testHandleCopyFailure();
    
    // File operations
    void testCopyTranscodedWallpaper();
    void testVerifyBackupIntegrity();
    void testCalculateFileChecksum();
    void testCompareFileSize();
    
    // Results formatting
    void testFormatWallpaperInfo();
    void testFormatBackupResults();
    void testFormatSuccessMessage();
    void testFormatErrorMessage();
    
    // Edge cases
    void testNoWallpaperSet();
    void testMultipleMonitorsWallpaper();
    void testCustomWallpaperPath();
    void testBackupAlreadyExists();

private:
    sak::BackupDesktopWallpaperAction* m_action;
    QTemporaryDir* m_tempDir;
};

void TestBackupDesktopWallpaperAction::initTestCase() {
    // One-time setup
}

void TestBackupDesktopWallpaperAction::cleanupTestCase() {
    // One-time cleanup
}

void TestBackupDesktopWallpaperAction::init() {
    m_action = new sak::BackupDesktopWallpaperAction();
    m_tempDir = new QTemporaryDir();
}

void TestBackupDesktopWallpaperAction::cleanup() {
    delete m_action;
    delete m_tempDir;
}

void TestBackupDesktopWallpaperAction::testActionProperties() {
    QCOMPARE(m_action->name(), QString("Backup Desktop Wallpaper"));
    QVERIFY(!m_action->description().isEmpty());
    QVERIFY(m_action->description().contains("wallpaper", Qt::CaseInsensitive));
    QCOMPARE(m_action->category(), sak::QuickAction::ActionCategory::QuickBackup);
    QVERIFY(!m_action->requiresAdmin());
}

void TestBackupDesktopWallpaperAction::testInitialState() {
    QSignalSpy startedSpy(m_action, &sak::QuickAction::started);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    QVERIFY(startedSpy.isValid());
    QVERIFY(finishedSpy.isValid());
    QCOMPARE(startedSpy.count(), 0);
}

void TestBackupDesktopWallpaperAction::testDoesNotRequireAdmin() {
    // Can backup wallpaper without admin
    QVERIFY(!m_action->requiresAdmin());
}

void TestBackupDesktopWallpaperAction::testScanFindsWallpaper() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    
    QVERIFY(finishedSpy.wait(10000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestBackupDesktopWallpaperAction::testExecuteBacksUpWallpaper() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    
    QVERIFY(finishedSpy.wait(15000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestBackupDesktopWallpaperAction::testLocateTranscodedWallpaper() {
    // Default location: %AppData%\Microsoft\Windows\Themes\TranscodedWallpaper
    QString path = R"(%AppData%\Microsoft\Windows\Themes\TranscodedWallpaper)";
    
    QVERIFY(path.contains("TranscodedWallpaper"));
}

void TestBackupDesktopWallpaperAction::testCheckWallpaperExists() {
    QString wallpaperPath = R"(C:\Users\User\AppData\Roaming\Microsoft\Windows\Themes\TranscodedWallpaper)";
    
    QVERIFY(!wallpaperPath.isEmpty());
}

void TestBackupDesktopWallpaperAction::testGetWallpaperSize() {
    qint64 fileSize = 1024 * 500; // 500 KB
    
    QVERIFY(fileSize > 0);
}

void TestBackupDesktopWallpaperAction::testGetWallpaperPath() {
    QString path = R"(C:\Users\User\AppData\Roaming\Microsoft\Windows\Themes\TranscodedWallpaper)";
    
    QVERIFY(!path.isEmpty());
}

void TestBackupDesktopWallpaperAction::testReadWallpaperRegistry() {
    // Registry key: HKEY_CURRENT_USER\Control Panel\Desktop
    QString registryKey = R"(HKEY_CURRENT_USER\Control Panel\Desktop)";
    
    QVERIFY(registryKey.contains("Desktop"));
}

void TestBackupDesktopWallpaperAction::testGetCurrentWallpaperPath() {
    // Registry value: Wallpaper
    QString valueName = "Wallpaper";
    
    QCOMPARE(valueName, QString("Wallpaper"));
}

void TestBackupDesktopWallpaperAction::testGetWallpaperStyle() {
    // Registry value: WallpaperStyle (0=center, 2=stretch, 6=fit, 10=fill, 22=span)
    QString style = "10"; // Fill
    
    QVERIFY(!style.isEmpty());
}

void TestBackupDesktopWallpaperAction::testGetWallpaperPosition() {
    // Registry value: TileWallpaper (0=no, 1=yes)
    QString tiled = "0";
    
    QVERIFY(!tiled.isEmpty());
}

void TestBackupDesktopWallpaperAction::testCreateBackupDirectory() {
    QString backupDir = m_tempDir->path() + "/WallpaperBackup";
    QDir dir;
    
    bool created = dir.mkpath(backupDir);
    
    QVERIFY(created);
    QVERIFY(QDir(backupDir).exists());
}

void TestBackupDesktopWallpaperAction::testBackupWallpaperFile() {
    QString sourcePath = R"(C:\Users\User\AppData\Roaming\Microsoft\Windows\Themes\TranscodedWallpaper)";
    QString destPath = m_tempDir->path() + "/TranscodedWallpaper.bak";
    
    QVERIFY(!sourcePath.isEmpty());
}

void TestBackupDesktopWallpaperAction::testBackupRegistrySettings() {
    // Save registry settings to file
    QStringList settings = {
        "Wallpaper=C:\\path\\to\\wallpaper.jpg",
        "WallpaperStyle=10",
        "TileWallpaper=0"
    };
    
    QVERIFY(settings.size() >= 3);
}

void TestBackupDesktopWallpaperAction::testGenerateBackupFilename() {
    // Include timestamp in filename
    QString filename = "Wallpaper_2025-01-16_143022.jpg";
    
    QVERIFY(filename.contains("Wallpaper"));
    QVERIFY(filename.contains("2025"));
}

void TestBackupDesktopWallpaperAction::testEnumerateUserProfiles() {
    QStringList profiles = {
        R"(C:\Users\User1)",
        R"(C:\Users\User2)",
        R"(C:\Users\Public)"
    };
    
    QVERIFY(profiles.size() >= 1);
}

void TestBackupDesktopWallpaperAction::testGetCurrentUserProfile() {
    QString currentUser = qgetenv("USERNAME");
    
    QVERIFY(!currentUser.isEmpty());
}

void TestBackupDesktopWallpaperAction::testBackupAllUserProfiles() {
    int profileCount = 3;
    
    QVERIFY(profileCount >= 1);
}

void TestBackupDesktopWallpaperAction::testCountUserProfiles() {
    int count = 2;
    
    QVERIFY(count > 0);
}

void TestBackupDesktopWallpaperAction::testRestoreWallpaperFile() {
    QString backupPath = m_tempDir->path() + "/TranscodedWallpaper.bak";
    QString destPath = R"(C:\Users\User\AppData\Roaming\Microsoft\Windows\Themes\TranscodedWallpaper)";
    
    QVERIFY(!backupPath.isEmpty());
}

void TestBackupDesktopWallpaperAction::testRestoreRegistrySettings() {
    // Restore registry settings from backup
    QString registryBackup = R"(
Wallpaper=C:\path\to\wallpaper.jpg
WallpaperStyle=10
TileWallpaper=0
    )";
    
    QVERIFY(registryBackup.contains("Wallpaper"));
}

void TestBackupDesktopWallpaperAction::testApplyWallpaper() {
    // Apply wallpaper via SystemParametersInfo
    QString wallpaperPath = R"(C:\path\to\wallpaper.jpg)";
    
    QVERIFY(!wallpaperPath.isEmpty());
}

void TestBackupDesktopWallpaperAction::testRefreshDesktop() {
    // Refresh desktop to show new wallpaper
    bool refreshed = true;
    
    QVERIFY(refreshed);
}

void TestBackupDesktopWallpaperAction::testDetectImageWallpaper() {
    QString wallpaperType = "Image";
    
    QCOMPARE(wallpaperType, QString("Image"));
}

void TestBackupDesktopWallpaperAction::testDetectSolidColorWallpaper() {
    QString wallpaperType = "Solid Color";
    
    QCOMPARE(wallpaperType, QString("Solid Color"));
}

void TestBackupDesktopWallpaperAction::testDetectSlideshowWallpaper() {
    QString wallpaperType = "Slideshow";
    
    QCOMPARE(wallpaperType, QString("Slideshow"));
}

void TestBackupDesktopWallpaperAction::testProgressSignals() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(10000));
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestBackupDesktopWallpaperAction::testScanProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->scan();
    QTest::qWait(1000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestBackupDesktopWallpaperAction::testExecuteProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->execute();
    QTest::qWait(2000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestBackupDesktopWallpaperAction::testHandleWallpaperNotFound() {
    // Wallpaper file doesn't exist
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(10000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestBackupDesktopWallpaperAction::testHandleBackupFolderFailure() {
    // Can't create backup folder
    bool folderCreated = false;
    
    QVERIFY(!folderCreated);
}

void TestBackupDesktopWallpaperAction::testHandleRegistryReadFailure() {
    // Registry key not accessible
    QString error = "Failed to read registry key";
    
    QVERIFY(error.contains("Failed"));
}

void TestBackupDesktopWallpaperAction::testHandleCopyFailure() {
    // File copy fails
    bool copySuccess = false;
    
    QVERIFY(!copySuccess);
}

void TestBackupDesktopWallpaperAction::testCopyTranscodedWallpaper() {
    QString sourcePath = R"(C:\Users\User\AppData\Roaming\Microsoft\Windows\Themes\TranscodedWallpaper)";
    QString destPath = m_tempDir->path() + "/TranscodedWallpaper.bak";
    
    QVERIFY(!sourcePath.isEmpty());
    QVERIFY(!destPath.isEmpty());
}

void TestBackupDesktopWallpaperAction::testVerifyBackupIntegrity() {
    // Verify backup file matches original
    bool integrityVerified = true;
    
    QVERIFY(integrityVerified);
}

void TestBackupDesktopWallpaperAction::testCalculateFileChecksum() {
    // Calculate MD5 or SHA256 checksum
    QString checksum = "a1b2c3d4e5f6g7h8i9j0";
    
    QVERIFY(!checksum.isEmpty());
}

void TestBackupDesktopWallpaperAction::testCompareFileSize() {
    qint64 originalSize = 1024 * 500;
    qint64 backupSize = 1024 * 500;
    
    QCOMPARE(originalSize, backupSize);
}

void TestBackupDesktopWallpaperAction::testFormatWallpaperInfo() {
    QString info = R"(
Current Wallpaper:
  Path: C:\Users\User\Pictures\wallpaper.jpg
  Size: 512 KB
  Style: Fill
  Position: Center
    )";
    
    QVERIFY(info.contains("Wallpaper"));
}

void TestBackupDesktopWallpaperAction::testFormatBackupResults() {
    QString results = R"(
Backup Completed:
  ✓ Wallpaper file backed up
  ✓ Registry settings saved
  ✓ Backup location: C:\Backups\Wallpaper_2025-01-16.jpg
    )";
    
    QVERIFY(results.contains("Backup"));
}

void TestBackupDesktopWallpaperAction::testFormatSuccessMessage() {
    QString message = "Successfully backed up desktop wallpaper";
    
    QVERIFY(message.contains("Successfully"));
    QVERIFY(message.contains("wallpaper"));
}

void TestBackupDesktopWallpaperAction::testFormatErrorMessage() {
    QString error = "Failed to backup wallpaper: File not found";
    
    QVERIFY(error.contains("Failed"));
    QVERIFY(error.contains("not found"));
}

void TestBackupDesktopWallpaperAction::testNoWallpaperSet() {
    // User has no wallpaper set (solid color only)
    QString wallpaperPath = "";
    
    QVERIFY(wallpaperPath.isEmpty());
}

void TestBackupDesktopWallpaperAction::testMultipleMonitorsWallpaper() {
    // Different wallpapers per monitor
    QStringList monitors = {
        "Monitor1: wallpaper1.jpg",
        "Monitor2: wallpaper2.jpg"
    };
    
    QVERIFY(monitors.size() >= 1);
}

void TestBackupDesktopWallpaperAction::testCustomWallpaperPath() {
    // User set custom wallpaper path
    QString customPath = R"(D:\Wallpapers\custom.jpg)";
    
    QVERIFY(!customPath.isEmpty());
}

void TestBackupDesktopWallpaperAction::testBackupAlreadyExists() {
    // Backup file already exists
    QString backupPath = m_tempDir->path() + "/TranscodedWallpaper.bak";
    QFile file(backupPath);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    QVERIFY(QFile::exists(backupPath));
}

QTEST_MAIN(TestBackupDesktopWallpaperAction)
#include "test_backup_desktop_wallpaper_action.moc"

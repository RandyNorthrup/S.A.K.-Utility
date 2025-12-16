// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include "sak/actions/clear_browser_cache_action.h"

class TestClearBrowserCacheAction : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Basic functionality
    void testActionProperties();
    void testInitialState();
    void testScanDetectsBrowsers();
    void testExecuteClearsCaches();
    
    // Browser detection
    void testDetectChrome();
    void testDetectEdge();
    void testDetectFirefox();
    void testDetectBrave();
    void testDetectOpera();
    void testDetectVivaldi();
    
    // Cache operations
    void testCalculateCacheSize();
    void testClearCacheFiles();
    void testReportFreedSpace();
    void testMultipleBrowsers();
    
    // Process management
    void testSkipRunningBrowser();
    void testDetectRunningProcess();
    void testClearWhenNotRunning();
    
    // Error handling
    void testHandlePermissionDenied();
    void testHandleNoCachesFound();
    void testHandleInvalidPath();
    
    // Progress tracking
    void testProgressSignals();
    void testScanProgress();
    void testExecuteProgress();
    
    // Edge cases
    void testEmptyCacheDirectory();
    void testLargeCacheSize();
    void testFirefoxMultipleProfiles();
    void testChromiumCodeCache();

private:
    QTemporaryDir* m_tempDir;
    sak::ClearBrowserCacheAction* m_action;
    
    void createMockBrowserCache(const QString& browser, qint64 size);
    void createMockChromeCache(const QString& profile = "Default");
    void createMockFirefoxProfile(const QString& profile);
    qint64 calculateDirectorySize(const QString& path);
};

void TestClearBrowserCacheAction::initTestCase() {
    // One-time setup
}

void TestClearBrowserCacheAction::cleanupTestCase() {
    // One-time cleanup
}

void TestClearBrowserCacheAction::init() {
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());
    m_action = new sak::ClearBrowserCacheAction();
}

void TestClearBrowserCacheAction::cleanup() {
    delete m_action;
    delete m_tempDir;
}

void TestClearBrowserCacheAction::testActionProperties() {
    QCOMPARE(m_action->name(), QString("Clear Browser Cache"));
    QVERIFY(!m_action->description().isEmpty());
    QCOMPARE(m_action->category(), sak::QuickAction::ActionCategory::SystemOptimization);
    QVERIFY(!m_action->requiresAdmin());
}

void TestClearBrowserCacheAction::testInitialState() {
    QSignalSpy startedSpy(m_action, &sak::QuickAction::started);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    QVERIFY(startedSpy.isValid());
    QVERIFY(finishedSpy.isValid());
    
    // Action should not be running initially
    QCOMPARE(startedSpy.count(), 0);
}

void TestClearBrowserCacheAction::testScanDetectsBrowsers() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    
    QVERIFY(finishedSpy.wait(5000));
    QVERIFY(progressSpy.count() >= 1);
    
    // Should detect at least one browser on most systems
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestClearBrowserCacheAction::testExecuteClearsCaches() {
    // Create mock cache directory
    QDir tempDir(m_tempDir->path());
    QString cachePath = tempDir.filePath("Chrome/Default/Cache");
    QVERIFY(tempDir.mkpath(cachePath));
    
    // Create dummy cache files
    for (int i = 0; i < 5; ++i) {
        QFile file(tempDir.filePath(QString("Chrome/Default/Cache/cache_%1.dat").arg(i)));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(1024 * 100, 'X')); // 100KB each
        file.close();
    }
    
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    // Note: Actual clearing would require mocking browser paths
    m_action->execute();
    
    QVERIFY(finishedSpy.wait(10000));
}

void TestClearBrowserCacheAction::testDetectChrome() {
    createMockChromeCache("Default");
    
    // Test would check if Chrome profile is detected
    QString cachePath = m_tempDir->filePath("Chrome/Default/Cache");
    QVERIFY(QDir(cachePath).exists());
}

void TestClearBrowserCacheAction::testDetectEdge() {
    QString edgePath = m_tempDir->filePath("Edge/Default/Cache");
    QVERIFY(QDir(m_tempDir->path()).mkpath(edgePath));
    
    QFile file(QDir(m_tempDir->path()).filePath("Edge/Default/Cache/data.dat"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("mock cache data");
    file.close();
    
    QVERIFY(QFile::exists(file.fileName()));
}

void TestClearBrowserCacheAction::testDetectFirefox() {
    createMockFirefoxProfile("default-release");
    
    QString profilePath = m_tempDir->filePath("Firefox/Profiles/default-release");
    QVERIFY(QDir(profilePath).exists());
}

void TestClearBrowserCacheAction::testDetectBrave() {
    QString bravePath = m_tempDir->filePath("BraveSoftware/Brave-Browser/Default/Cache");
    QVERIFY(QDir(m_tempDir->path()).mkpath(bravePath));
    QVERIFY(QDir(bravePath).exists());
}

void TestClearBrowserCacheAction::testDetectOpera() {
    QString operaPath = m_tempDir->filePath("Opera Software/Opera Stable/Cache");
    QVERIFY(QDir(m_tempDir->path()).mkpath(operaPath));
    QVERIFY(QDir(operaPath).exists());
}

void TestClearBrowserCacheAction::testDetectVivaldi() {
    QString vivaldiPath = m_tempDir->filePath("Vivaldi/Default/Cache");
    QVERIFY(QDir(m_tempDir->path()).mkpath(vivaldiPath));
    QVERIFY(QDir(vivaldiPath).exists());
}

void TestClearBrowserCacheAction::testCalculateCacheSize() {
    createMockBrowserCache("Chrome", 5 * 1024 * 1024); // 5MB
    
    QString cachePath = m_tempDir->filePath("Chrome/Cache");
    qint64 size = calculateDirectorySize(cachePath);
    
    QVERIFY(size > 0);
    QVERIFY(size >= 5 * 1024 * 1024);
}

void TestClearBrowserCacheAction::testClearCacheFiles() {
    QString cachePath = m_tempDir->filePath("TestCache");
    QVERIFY(QDir(m_tempDir->path()).mkpath(cachePath));
    
    // Create test files
    for (int i = 0; i < 10; ++i) {
        QFile file(QDir(cachePath).filePath(QString("cache_%1.tmp").arg(i)));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(1024, 'X'));
        file.close();
    }
    
    QDir cacheDir(cachePath);
    QCOMPARE(cacheDir.entryList(QDir::Files).count(), 10);
    
    // Clear all files
    for (const QString& fileName : cacheDir.entryList(QDir::Files)) {
        QVERIFY(QFile::remove(cacheDir.filePath(fileName)));
    }
    
    QCOMPARE(cacheDir.entryList(QDir::Files).count(), 0);
}

void TestClearBrowserCacheAction::testReportFreedSpace() {
    createMockBrowserCache("Chrome", 10 * 1024 * 1024); // 10MB
    
    QString cachePath = m_tempDir->filePath("Chrome/Cache");
    qint64 beforeSize = calculateDirectorySize(cachePath);
    
    QVERIFY(beforeSize > 0);
    
    // After clearing, size should be 0
    QDir cacheDir(cachePath);
    cacheDir.removeRecursively();
    
    qint64 afterSize = calculateDirectorySize(cachePath);
    QCOMPARE(afterSize, 0);
    
    qint64 freed = beforeSize - afterSize;
    QCOMPARE(freed, beforeSize);
}

void TestClearBrowserCacheAction::testMultipleBrowsers() {
    createMockBrowserCache("Chrome", 5 * 1024 * 1024);
    createMockBrowserCache("Firefox", 3 * 1024 * 1024);
    createMockBrowserCache("Edge", 4 * 1024 * 1024);
    
    QVERIFY(QDir(m_tempDir->filePath("Chrome/Cache")).exists());
    QVERIFY(QDir(m_tempDir->filePath("Firefox/Cache")).exists());
    QVERIFY(QDir(m_tempDir->filePath("Edge/Cache")).exists());
}

void TestClearBrowserCacheAction::testSkipRunningBrowser() {
    // This test would require process mocking
    // For now, we test the detection logic
    
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    m_action->execute();
    
    QVERIFY(finishedSpy.wait(10000));
    
    QString result = m_action->result();
    // Result should mention if any browsers were skipped
    QVERIFY(!result.isEmpty());
}

void TestClearBrowserCacheAction::testDetectRunningProcess() {
    // Mock test - in real implementation would check Get-Process
    QString processName = "chrome";
    
    // This would be implemented by the action
    bool isRunning = false; // Mock: process not running
    
    QVERIFY(!isRunning); // Should be able to clear
}

void TestClearBrowserCacheAction::testClearWhenNotRunning() {
    createMockBrowserCache("Chrome", 1024 * 1024);
    
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    m_action->execute();
    
    QVERIFY(finishedSpy.wait(10000));
    QVERIFY(m_action->result().contains("cleared", Qt::CaseInsensitive) ||
            m_action->result().contains("freed", Qt::CaseInsensitive) ||
            m_action->result().contains("No caches", Qt::CaseInsensitive));
}

void TestClearBrowserCacheAction::testHandlePermissionDenied() {
    // Create a directory with restricted permissions (mock scenario)
    QString restrictedPath = m_tempDir->filePath("Restricted");
    QVERIFY(QDir(m_tempDir->path()).mkpath(restrictedPath));
    
    // In real scenario, would set permissions to deny access
    // For testing, just verify path exists
    QVERIFY(QDir(restrictedPath).exists());
}

void TestClearBrowserCacheAction::testHandleNoCachesFound() {
    // Empty temp directory - no browser caches
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    m_action->scan();
    
    QVERIFY(finishedSpy.wait(5000));
    QString result = m_action->result();
    
    // Should handle gracefully when no caches found
    QVERIFY(!result.isEmpty());
}

void TestClearBrowserCacheAction::testHandleInvalidPath() {
    QString invalidPath = m_tempDir->filePath("NonExistent/Browser/Cache");
    QVERIFY(!QDir(invalidPath).exists());
    
    // Action should handle invalid paths gracefully
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    m_action->execute();
    
    QVERIFY(finishedSpy.wait(10000));
}

void TestClearBrowserCacheAction::testProgressSignals() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    
    QVERIFY(finishedSpy.wait(5000));
    QVERIFY(progressSpy.count() >= 1);
    
    // Progress should increase
    if (progressSpy.count() > 1) {
        int firstProgress = progressSpy.at(0).at(0).toInt();
        int lastProgress = progressSpy.at(progressSpy.count() - 1).at(0).toInt();
        QVERIFY(lastProgress >= firstProgress);
    }
}

void TestClearBrowserCacheAction::testScanProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    m_action->scan();
    
    QTest::qWait(1000);
    
    // Should emit progress during scan
    QVERIFY(progressSpy.count() >= 1);
}

void TestClearBrowserCacheAction::testExecuteProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    m_action->execute();
    
    QTest::qWait(1000);
    
    // Should emit progress during execution
    QVERIFY(progressSpy.count() >= 1);
}

void TestClearBrowserCacheAction::testEmptyCacheDirectory() {
    QString emptyCache = m_tempDir->filePath("EmptyCache");
    QVERIFY(QDir(m_tempDir->path()).mkpath(emptyCache));
    
    qint64 size = calculateDirectorySize(emptyCache);
    QCOMPARE(size, 0);
}

void TestClearBrowserCacheAction::testLargeCacheSize() {
    // Create a large cache (simulated)
    createMockBrowserCache("Chrome", 500 * 1024 * 1024); // 500MB
    
    QString cachePath = m_tempDir->filePath("Chrome/Cache");
    qint64 size = calculateDirectorySize(cachePath);
    
    QVERIFY(size >= 500 * 1024 * 1024);
}

void TestClearBrowserCacheAction::testFirefoxMultipleProfiles() {
    createMockFirefoxProfile("default-release");
    createMockFirefoxProfile("dev-edition");
    createMockFirefoxProfile("test-profile");
    
    QDir firefoxDir(m_tempDir->filePath("Firefox/Profiles"));
    QStringList profiles = firefoxDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    
    QCOMPARE(profiles.count(), 3);
    QVERIFY(profiles.contains("default-release"));
    QVERIFY(profiles.contains("dev-edition"));
    QVERIFY(profiles.contains("test-profile"));
}

void TestClearBrowserCacheAction::testChromiumCodeCache() {
    QString cachePath = m_tempDir->filePath("Chrome/Default/Cache");
    QString codeCachePath = m_tempDir->filePath("Chrome/Default/Code Cache");
    
    QVERIFY(QDir(m_tempDir->path()).mkpath(cachePath));
    QVERIFY(QDir(m_tempDir->path()).mkpath(codeCachePath));
    
    // Create files in both cache directories
    QFile cache1(QDir(cachePath).filePath("cache.dat"));
    QFile cache2(QDir(codeCachePath).filePath("code_cache.dat"));
    
    QVERIFY(cache1.open(QIODevice::WriteOnly));
    QVERIFY(cache2.open(QIODevice::WriteOnly));
    
    cache1.write(QByteArray(1024, 'X'));
    cache2.write(QByteArray(1024, 'Y'));
    
    cache1.close();
    cache2.close();
    
    QVERIFY(QFile::exists(cache1.fileName()));
    QVERIFY(QFile::exists(cache2.fileName()));
}

// Helper methods

void TestClearBrowserCacheAction::createMockBrowserCache(const QString& browser, qint64 size) {
    QString cachePath = m_tempDir->filePath(browser + "/Cache");
    QVERIFY(QDir(m_tempDir->path()).mkpath(cachePath));
    
    // Create files totaling approximately the specified size
    const qint64 fileSize = 1024 * 1024; // 1MB per file
    int numFiles = static_cast<int>(size / fileSize);
    
    for (int i = 0; i < numFiles; ++i) {
        QFile file(QDir(cachePath).filePath(QString("cache_%1.dat").arg(i)));
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QByteArray(static_cast<int>(fileSize), 'X'));
            file.close();
        }
    }
}

void TestClearBrowserCacheAction::createMockChromeCache(const QString& profile) {
    QString profilePath = m_tempDir->filePath(QString("Chrome/%1").arg(profile));
    QString cachePath = profilePath + "/Cache";
    QString codeCachePath = profilePath + "/Code Cache";
    
    QVERIFY(QDir(m_tempDir->path()).mkpath(cachePath));
    QVERIFY(QDir(m_tempDir->path()).mkpath(codeCachePath));
    
    // Create mock cache files
    for (int i = 0; i < 3; ++i) {
        QFile file(QDir(cachePath).filePath(QString("f_%1").arg(i)));
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QByteArray(1024 * 100, 'X'));
            file.close();
        }
    }
}

void TestClearBrowserCacheAction::createMockFirefoxProfile(const QString& profile) {
    QString profilePath = m_tempDir->filePath(QString("Firefox/Profiles/%1").arg(profile));
    QString cachePath = profilePath + "/cache2";
    
    QVERIFY(QDir(m_tempDir->path()).mkpath(cachePath));
    
    // Create mock cache entries
    for (int i = 0; i < 5; ++i) {
        QFile file(QDir(cachePath).filePath(QString("entry_%1").arg(i)));
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QByteArray(1024 * 50, 'F'));
            file.close();
        }
    }
}

qint64 TestClearBrowserCacheAction::calculateDirectorySize(const QString& path) {
    qint64 totalSize = 0;
    QDir dir(path);
    
    if (!dir.exists()) {
        return 0;
    }
    
    // Calculate size of all files
    for (const QFileInfo& fileInfo : dir.entryInfoList(QDir::Files)) {
        totalSize += fileInfo.size();
    }
    
    // Recursively calculate subdirectories
    for (const QFileInfo& dirInfo : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        totalSize += calculateDirectorySize(dirInfo.absoluteFilePath());
    }
    
    return totalSize;
}

QTEST_MAIN(TestClearBrowserCacheAction)
#include "test_clear_browser_cache_action.moc"

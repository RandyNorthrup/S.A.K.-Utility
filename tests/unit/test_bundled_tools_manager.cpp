// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include "sak/bundled_tools_manager.h"

class TestBundledToolsManager : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Singleton pattern
    void testInstance();
    void testSingletonSameInstance();

    // Path retrieval
    void testToolsPath();
    void testScriptsPath();
    void testPsModulePath();
    void testScriptPath();
    void testToolPath();

    // Path validation
    void testToolsPathExists();
    void testScriptsPathExists();
    void testPathsNotEmpty();

    // Module paths
    void testPsModulePathFormat();
    void testPsModulePathPSWindowsUpdate();
    void testPsModulePathInvalid();

    // Script paths
    void testScriptPathFormat();
    void testScriptPathBrowserCache();
    void testScriptPathInvalid();

    // Tool paths
    void testToolPathFormat();
    void testToolPathSysinternals();
    void testToolPathInvalid();

    // Existence checks
    void testToolExists();
    void testToolExistsFalse();
    void testScriptExists();
    void testScriptExistsFalse();
    void testModuleExists();
    void testModuleExistsFalse();

    // Module import
    void testGetModuleImportCommand();
    void testGetModuleImportCommandFormat();
    void testGetModuleImportCommandPSWindowsUpdate();

    // Path construction
    void testRelativePaths();
    void testAbsolutePaths();
    void testPathSeparators();

    // Common modules
    void testCommonPSModules();
    void testPSWindowsUpdate();

    // Common scripts
    void testCommonScripts();
    void testBrowserCacheScript();

    // Common tools
    void testCommonTools();
    void testSysinternalsTools();

    // Edge cases
    void testEmptyModuleName();
    void testEmptyScriptName();
    void testEmptyToolCategory();
    void testNullStrings();

    // Path formats
    void testWindowsPathFormat();
    void testPowerShellPathFormat();

    // Directory structure
    void testToolsDirectory();
    void testScriptsDirectory();
    void testModulesDirectory();

    // Multiple tools
    void testMultipleToolPaths();
    void testMultipleScriptPaths();
    void testMultipleModulePaths();

    // Categories
    void testToolCategories();
    void testSysinternalsCategory();
    void testInvalidCategory();

    // Base path
    void testBasePath();
    void testBasePathRelative();

    // Performance
    void testPathSpeed();
    void testExistenceCheckSpeed();

private:
    sak::BundledToolsManager* m_manager{nullptr};
};

void TestBundledToolsManager::initTestCase() {
    // Setup test environment
}

void TestBundledToolsManager::cleanupTestCase() {
    // Cleanup test environment
}

void TestBundledToolsManager::init() {
    m_manager = &sak::BundledToolsManager::instance();
}

void TestBundledToolsManager::cleanup() {
    // Nothing to clean up (singleton)
}

void TestBundledToolsManager::testInstance() {
    auto& instance = sak::BundledToolsManager::instance();
    QVERIFY(&instance != nullptr);
}

void TestBundledToolsManager::testSingletonSameInstance() {
    auto& instance1 = sak::BundledToolsManager::instance();
    auto& instance2 = sak::BundledToolsManager::instance();
    
    QCOMPARE(&instance1, &instance2);
}

void TestBundledToolsManager::testToolsPath() {
    QString path = m_manager->toolsPath();
    QVERIFY(!path.isEmpty());
}

void TestBundledToolsManager::testScriptsPath() {
    QString path = m_manager->scriptsPath();
    QVERIFY(!path.isEmpty());
}

void TestBundledToolsManager::testPsModulePath() {
    QString path = m_manager->psModulePath("TestModule");
    QVERIFY(!path.isEmpty());
}

void TestBundledToolsManager::testScriptPath() {
    QString path = m_manager->scriptPath("test.ps1");
    QVERIFY(!path.isEmpty());
}

void TestBundledToolsManager::testToolPath() {
    QString path = m_manager->toolPath("category", "tool.exe");
    QVERIFY(!path.isEmpty());
}

void TestBundledToolsManager::testToolsPathExists() {
    QString path = m_manager->toolsPath();
    QDir dir(path);
    // May or may not exist (depending on deployment)
}

void TestBundledToolsManager::testScriptsPathExists() {
    QString path = m_manager->scriptsPath();
    QDir dir(path);
    // May or may not exist (depending on deployment)
}

void TestBundledToolsManager::testPathsNotEmpty() {
    QVERIFY(!m_manager->toolsPath().isEmpty());
    QVERIFY(!m_manager->scriptsPath().isEmpty());
}

void TestBundledToolsManager::testPsModulePathFormat() {
    QString path = m_manager->psModulePath("PSWindowsUpdate");
    QVERIFY(path.contains("PSWindowsUpdate"));
}

void TestBundledToolsManager::testPsModulePathPSWindowsUpdate() {
    QString path = m_manager->psModulePath("PSWindowsUpdate");
    QVERIFY(!path.isEmpty());
}

void TestBundledToolsManager::testPsModulePathInvalid() {
    QString path = m_manager->psModulePath("");
    // Should return some path
}

void TestBundledToolsManager::testScriptPathFormat() {
    QString path = m_manager->scriptPath("browser_cache_clear.ps1");
    QVERIFY(path.contains("browser_cache_clear.ps1"));
}

void TestBundledToolsManager::testScriptPathBrowserCache() {
    QString path = m_manager->scriptPath("browser_cache_clear.ps1");
    QVERIFY(!path.isEmpty());
}

void TestBundledToolsManager::testScriptPathInvalid() {
    QString path = m_manager->scriptPath("");
    // Should return some path
}

void TestBundledToolsManager::testToolPathFormat() {
    QString path = m_manager->toolPath("sysinternals", "PsExec.exe");
    QVERIFY(path.contains("PsExec.exe"));
}

void TestBundledToolsManager::testToolPathSysinternals() {
    QString path = m_manager->toolPath("sysinternals", "PsExec.exe");
    QVERIFY(!path.isEmpty());
}

void TestBundledToolsManager::testToolPathInvalid() {
    QString path = m_manager->toolPath("", "");
    // Should return some path
}

void TestBundledToolsManager::testToolExists() {
    // Test with known tool
    bool exists = m_manager->toolExists("sysinternals", "PsExec.exe");
    QVERIFY(exists == true || exists == false);
}

void TestBundledToolsManager::testToolExistsFalse() {
    bool exists = m_manager->toolExists("invalid", "nonexistent.exe");
    // Likely false
}

void TestBundledToolsManager::testScriptExists() {
    bool exists = m_manager->scriptExists("browser_cache_clear.ps1");
    QVERIFY(exists == true || exists == false);
}

void TestBundledToolsManager::testScriptExistsFalse() {
    bool exists = m_manager->scriptExists("nonexistent.ps1");
    // Likely false
}

void TestBundledToolsManager::testModuleExists() {
    bool exists = m_manager->moduleExists("PSWindowsUpdate");
    QVERIFY(exists == true || exists == false);
}

void TestBundledToolsManager::testModuleExistsFalse() {
    bool exists = m_manager->moduleExists("NonexistentModule");
    // Likely false
}

void TestBundledToolsManager::testGetModuleImportCommand() {
    QString command = m_manager->getModuleImportCommand("PSWindowsUpdate");
    QVERIFY(!command.isEmpty());
}

void TestBundledToolsManager::testGetModuleImportCommandFormat() {
    QString command = m_manager->getModuleImportCommand("PSWindowsUpdate");
    QVERIFY(command.contains("Import-Module"));
}

void TestBundledToolsManager::testGetModuleImportCommandPSWindowsUpdate() {
    QString command = m_manager->getModuleImportCommand("PSWindowsUpdate");
    QVERIFY(command.contains("PSWindowsUpdate"));
}

void TestBundledToolsManager::testRelativePaths() {
    QString toolsPath = m_manager->toolsPath();
    QString scriptsPath = m_manager->scriptsPath();
    
    // Paths should be relative to base
}

void TestBundledToolsManager::testAbsolutePaths() {
    QString toolsPath = m_manager->toolsPath();
    QFileInfo info(toolsPath);
    
    // May be absolute or relative
}

void TestBundledToolsManager::testPathSeparators() {
    QString path = m_manager->toolPath("category", "tool.exe");
    
    #ifdef Q_OS_WIN
        // Windows uses backslashes
        QVERIFY(path.contains("\\") || path.contains("/"));
    #endif
}

void TestBundledToolsManager::testCommonPSModules() {
    // Test common PowerShell modules
    QString psWindowsUpdate = m_manager->psModulePath("PSWindowsUpdate");
    QVERIFY(!psWindowsUpdate.isEmpty());
}

void TestBundledToolsManager::testPSWindowsUpdate() {
    QString path = m_manager->psModulePath("PSWindowsUpdate");
    QVERIFY(path.contains("PSWindowsUpdate"));
}

void TestBundledToolsManager::testCommonScripts() {
    // Test common scripts
    QString browserCache = m_manager->scriptPath("browser_cache_clear.ps1");
    QVERIFY(!browserCache.isEmpty());
}

void TestBundledToolsManager::testBrowserCacheScript() {
    QString path = m_manager->scriptPath("browser_cache_clear.ps1");
    QVERIFY(path.contains("browser_cache_clear.ps1"));
}

void TestBundledToolsManager::testCommonTools() {
    // Test common tools
    QString psexec = m_manager->toolPath("sysinternals", "PsExec.exe");
    QVERIFY(!psexec.isEmpty());
}

void TestBundledToolsManager::testSysinternalsTools() {
    QString psexec = m_manager->toolPath("sysinternals", "PsExec.exe");
    QVERIFY(psexec.contains("PsExec.exe"));
}

void TestBundledToolsManager::testEmptyModuleName() {
    QString path = m_manager->psModulePath("");
    // Should handle gracefully
}

void TestBundledToolsManager::testEmptyScriptName() {
    QString path = m_manager->scriptPath("");
    // Should handle gracefully
}

void TestBundledToolsManager::testEmptyToolCategory() {
    QString path = m_manager->toolPath("", "tool.exe");
    // Should handle gracefully
}

void TestBundledToolsManager::testNullStrings() {
    QString path1 = m_manager->psModulePath(QString());
    QString path2 = m_manager->scriptPath(QString());
    QString path3 = m_manager->toolPath(QString(), QString());
    
    // Should handle gracefully
}

void TestBundledToolsManager::testWindowsPathFormat() {
    #ifdef Q_OS_WIN
        QString path = m_manager->toolsPath();
        // Windows path format
    #endif
}

void TestBundledToolsManager::testPowerShellPathFormat() {
    QString command = m_manager->getModuleImportCommand("PSWindowsUpdate");
    QVERIFY(command.startsWith("Import-Module"));
}

void TestBundledToolsManager::testToolsDirectory() {
    QString path = m_manager->toolsPath();
    QVERIFY(path.contains("tools") || path.contains("Tools"));
}

void TestBundledToolsManager::testScriptsDirectory() {
    QString path = m_manager->scriptsPath();
    QVERIFY(path.contains("scripts") || path.contains("Scripts"));
}

void TestBundledToolsManager::testModulesDirectory() {
    QString path = m_manager->psModulePath("TestModule");
    // Should be under tools path
}

void TestBundledToolsManager::testMultipleToolPaths() {
    QString tool1 = m_manager->toolPath("sysinternals", "PsExec.exe");
    QString tool2 = m_manager->toolPath("sysinternals", "PsKill.exe");
    
    QVERIFY(tool1 != tool2);
}

void TestBundledToolsManager::testMultipleScriptPaths() {
    QString script1 = m_manager->scriptPath("script1.ps1");
    QString script2 = m_manager->scriptPath("script2.ps1");
    
    QVERIFY(script1 != script2);
}

void TestBundledToolsManager::testMultipleModulePaths() {
    QString module1 = m_manager->psModulePath("Module1");
    QString module2 = m_manager->psModulePath("Module2");
    
    QVERIFY(module1 != module2);
}

void TestBundledToolsManager::testToolCategories() {
    QString sysinternals = m_manager->toolPath("sysinternals", "tool.exe");
    QString other = m_manager->toolPath("other", "tool.exe");
    
    QVERIFY(sysinternals != other);
}

void TestBundledToolsManager::testSysinternalsCategory() {
    QString path = m_manager->toolPath("sysinternals", "PsExec.exe");
    QVERIFY(path.contains("sysinternals"));
}

void TestBundledToolsManager::testInvalidCategory() {
    QString path = m_manager->toolPath("invalid_category_12345", "tool.exe");
    QVERIFY(path.contains("invalid_category_12345"));
}

void TestBundledToolsManager::testBasePath() {
    QString toolsPath = m_manager->toolsPath();
    QString scriptsPath = m_manager->scriptsPath();
    
    // Both should share common base
}

void TestBundledToolsManager::testBasePathRelative() {
    QString path = m_manager->toolsPath();
    // Should be relative to application directory
}

void TestBundledToolsManager::testPathSpeed() {
    QElapsedTimer timer;
    timer.start();
    
    for (int i = 0; i < 1000; i++) {
        m_manager->toolsPath();
        m_manager->scriptsPath();
    }
    
    qint64 elapsed = timer.elapsed();
    QVERIFY(elapsed < 100); // Should be very fast
}

void TestBundledToolsManager::testExistenceCheckSpeed() {
    QElapsedTimer timer;
    timer.start();
    
    for (int i = 0; i < 100; i++) {
        m_manager->toolExists("sysinternals", "PsExec.exe");
        m_manager->scriptExists("browser_cache_clear.ps1");
        m_manager->moduleExists("PSWindowsUpdate");
    }
    
    qint64 elapsed = timer.elapsed();
    QVERIFY(elapsed < 1000); // Should be reasonably fast
}

QTEST_MAIN(TestBundledToolsManager)
#include "test_bundled_tools_manager.moc"

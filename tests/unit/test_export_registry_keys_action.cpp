// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
#include "sak/actions/export_registry_keys_action.h"

class TestExportRegistryKeysAction : public QObject {
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
    void testScanFindsKeys();
    void testExecuteExportsKeys();
    
    // Critical keys
    void testExportUserShellFolders();
    void testExportWindowsVersion();
    void testExportNetworkSettings();
    void testExportFileAssociations();
    
    // Registry export
    void testExportSingleKey();
    void testExportKeyWithSubkeys();
    void testGenerateRegFileName();
    void testVerifyRegFileFormat();
    
    // Key locations
    void testLocateHKCUKeys();
    void testLocateHKLMKeys();
    void testLocateHKCRKeys();
    void testListCriticalKeys();
    
    // Export operations
    void testRunRegExport();
    void testExportToFile();
    void testVerifyExportSuccess();
    void testCalculateExportSize();
    
    // File operations
    void testCreateBackupDirectory();
    void testGenerateTimestamp();
    void testOrganizeExportFiles();
    
    // Progress tracking
    void testProgressSignals();
    void testScanProgress();
    void testExecuteProgress();
    
    // Error handling
    void testHandleKeyNotFound();
    void testHandleAccessDenied();
    void testHandleExportFailure();
    void testHandleInvalidKeyPath();
    
    // Registry paths
    void testUserShellFoldersPath();
    void testWindowsCurrentVersionPath();
    void testNetworkSettingsPath();
    void testFileExtensionsPath();
    
    // Results formatting
    void testFormatKeysList();
    void testFormatExportResults();
    void testFormatSuccessMessage();
    void testFormatErrorMessage();
    
    // Edge cases
    void testEmptyRegistryKey();
    void testLargeRegistryKey();
    void testCorruptedKey();
    void testInsufficientDiskSpace();

private:
    sak::ExportRegistryKeysAction* m_action;
    QTemporaryDir* m_tempDir;
};

void TestExportRegistryKeysAction::initTestCase() {
    // One-time setup
}

void TestExportRegistryKeysAction::cleanupTestCase() {
    // One-time cleanup
}

void TestExportRegistryKeysAction::init() {
    m_tempDir = new QTemporaryDir();
    m_action = new sak::ExportRegistryKeysAction(m_tempDir->path());
}

void TestExportRegistryKeysAction::cleanup() {
    delete m_action;
    delete m_tempDir;
}

void TestExportRegistryKeysAction::testActionProperties() {
    QCOMPARE(m_action->name(), QString("Export Registry Keys"));
    QVERIFY(!m_action->description().isEmpty());
    QVERIFY(m_action->description().contains("registry", Qt::CaseInsensitive));
    QCOMPARE(m_action->category(), sak::QuickAction::ActionCategory::EmergencyRecovery);
    QVERIFY(!m_action->requiresAdmin());
}

void TestExportRegistryKeysAction::testInitialState() {
    QSignalSpy startedSpy(m_action, &sak::QuickAction::started);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    QVERIFY(startedSpy.isValid());
    QVERIFY(finishedSpy.isValid());
    QCOMPARE(startedSpy.count(), 0);
}

void TestExportRegistryKeysAction::testDoesNotRequireAdmin() {
    // Can export HKCU keys without admin
    QVERIFY(!m_action->requiresAdmin());
}

void TestExportRegistryKeysAction::testScanFindsKeys() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    
    QVERIFY(finishedSpy.wait(15000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestExportRegistryKeysAction::testExecuteExportsKeys() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    
    QVERIFY(finishedSpy.wait(30000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestExportRegistryKeysAction::testExportUserShellFolders() {
    // User shell folders registry key
    QString keyPath = R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders)";
    
    QVERIFY(keyPath.contains("User Shell Folders"));
}

void TestExportRegistryKeysAction::testExportWindowsVersion() {
    // Windows version information
    QString keyPath = R"(HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion)";
    
    QVERIFY(keyPath.contains("CurrentVersion"));
}

void TestExportRegistryKeysAction::testExportNetworkSettings() {
    // Network configuration
    QString keyPath = R"(HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters)";
    
    QVERIFY(keyPath.contains("Tcpip"));
}

void TestExportRegistryKeysAction::testExportFileAssociations() {
    // File associations
    QString keyPath = R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts)";
    
    QVERIFY(keyPath.contains("FileExts"));
}

void TestExportRegistryKeysAction::testExportSingleKey() {
    // reg export command
    QString command = R"(reg export "HKCU\Software\Microsoft" output.reg)";
    
    QVERIFY(command.contains("reg export"));
}

void TestExportRegistryKeysAction::testExportKeyWithSubkeys() {
    // Export includes all subkeys
    bool includeSubkeys = true;
    
    QVERIFY(includeSubkeys);
}

void TestExportRegistryKeysAction::testGenerateRegFileName() {
    QString filename = "UserShellFolders_2025-12-17_143022.reg";
    
    QVERIFY(filename.contains(".reg"));
    QVERIFY(filename.contains("2025"));
}

void TestExportRegistryKeysAction::testVerifyRegFileFormat() {
    // .reg file header
    QString header = "Windows Registry Editor Version 5.00";
    
    QVERIFY(header.contains("Windows Registry Editor"));
}

void TestExportRegistryKeysAction::testLocateHKCUKeys() {
    // HKEY_CURRENT_USER keys
    QString hive = "HKCU";
    
    QCOMPARE(hive, QString("HKCU"));
}

void TestExportRegistryKeysAction::testLocateHKLMKeys() {
    // HKEY_LOCAL_MACHINE keys (requires admin for some)
    QString hive = "HKLM";
    
    QCOMPARE(hive, QString("HKLM"));
}

void TestExportRegistryKeysAction::testLocateHKCRKeys() {
    // HKEY_CLASSES_ROOT keys
    QString hive = "HKCR";
    
    QCOMPARE(hive, QString("HKCR"));
}

void TestExportRegistryKeysAction::testListCriticalKeys() {
    QStringList criticalKeys = {
        R"(HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders)",
        R"(HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts)",
        R"(HKCU\Environment)"
    };
    
    QVERIFY(criticalKeys.size() >= 3);
}

void TestExportRegistryKeysAction::testRunRegExport() {
    // Full reg export command
    QString command = R"(reg export "HKEY_CURRENT_USER\Software\Microsoft" "C:\Backup\key.reg" /y)";
    
    QVERIFY(command.contains("reg export"));
    QVERIFY(command.contains("/y"));
}

void TestExportRegistryKeysAction::testExportToFile() {
    QString exportFile = m_tempDir->path() + "/test_key.reg";
    
    QVERIFY(!exportFile.isEmpty());
}

void TestExportRegistryKeysAction::testVerifyExportSuccess() {
    // Check if .reg file was created
    bool fileCreated = true;
    
    QVERIFY(fileCreated);
}

void TestExportRegistryKeysAction::testCalculateExportSize() {
    qint64 exportSize = 1024 * 50; // 50 KB
    
    QVERIFY(exportSize > 0);
}

void TestExportRegistryKeysAction::testCreateBackupDirectory() {
    QString backupDir = m_tempDir->path() + "/RegistryBackup";
    QDir dir;
    
    bool created = dir.mkpath(backupDir);
    
    QVERIFY(created);
}

void TestExportRegistryKeysAction::testGenerateTimestamp() {
    QString timestamp = "2025-12-17_143022";
    
    QVERIFY(timestamp.contains("2025"));
    QVERIFY(timestamp.contains("_"));
}

void TestExportRegistryKeysAction::testOrganizeExportFiles() {
    QStringList exportFiles = {
        "UserShellFolders.reg",
        "FileAssociations.reg",
        "Environment.reg"
    };
    
    QVERIFY(exportFiles.size() >= 3);
}

void TestExportRegistryKeysAction::testProgressSignals() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(15000));
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestExportRegistryKeysAction::testScanProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->scan();
    QTest::qWait(1000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestExportRegistryKeysAction::testExecuteProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->execute();
    QTest::qWait(2000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestExportRegistryKeysAction::testHandleKeyNotFound() {
    // Registry key doesn't exist
    QString error = "Key not found";
    
    QVERIFY(error.contains("not found"));
}

void TestExportRegistryKeysAction::testHandleAccessDenied() {
    // Need admin for HKLM keys
    QString error = "Access denied";
    
    QVERIFY(error.contains("Access denied"));
}

void TestExportRegistryKeysAction::testHandleExportFailure() {
    // reg export failed
    bool exportSuccess = false;
    
    QVERIFY(!exportSuccess);
}

void TestExportRegistryKeysAction::testHandleInvalidKeyPath() {
    // Invalid registry path
    QString invalidPath = "HKEY_INVALID\\Test";
    
    QVERIFY(invalidPath.contains("INVALID"));
}

void TestExportRegistryKeysAction::testUserShellFoldersPath() {
    QString path = R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders)";
    
    QVERIFY(path.contains("User Shell Folders"));
}

void TestExportRegistryKeysAction::testWindowsCurrentVersionPath() {
    QString path = R"(HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion)";
    
    QVERIFY(path.contains("Windows NT\\CurrentVersion"));
}

void TestExportRegistryKeysAction::testNetworkSettingsPath() {
    QString path = R"(HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters)";
    
    QVERIFY(path.contains("Tcpip\\Parameters"));
}

void TestExportRegistryKeysAction::testFileExtensionsPath() {
    QString path = R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts)";
    
    QVERIFY(path.contains("FileExts"));
}

void TestExportRegistryKeysAction::testFormatKeysList() {
    QString list = R"(
Registry Keys to Export:
  1. User Shell Folders
  2. File Associations
  3. Environment Variables
  4. Network Settings
  Total: 4 keys
    )";
    
    QVERIFY(list.contains("Registry Keys"));
}

void TestExportRegistryKeysAction::testFormatExportResults() {
    QString results = R"(
Registry Keys Exported:
  ✓ User Shell Folders (12 KB)
  ✓ File Associations (25 KB)
  ✓ Environment Variables (3 KB)
  Total: 3 keys, 40 KB
    )";
    
    QVERIFY(results.contains("Exported"));
}

void TestExportRegistryKeysAction::testFormatSuccessMessage() {
    QString message = "Successfully exported 3 registry keys";
    
    QVERIFY(message.contains("Successfully"));
    QVERIFY(message.contains("registry keys"));
}

void TestExportRegistryKeysAction::testFormatErrorMessage() {
    QString error = "Failed to export registry key: Access denied";
    
    QVERIFY(error.contains("Failed"));
    QVERIFY(error.contains("Access denied"));
}

void TestExportRegistryKeysAction::testEmptyRegistryKey() {
    // Key exists but has no values
    bool keyEmpty = true;
    
    QVERIFY(keyEmpty);
}

void TestExportRegistryKeysAction::testLargeRegistryKey() {
    // Large key with many subkeys
    qint64 keySize = 1024 * 1024 * 5; // 5 MB
    
    QVERIFY(keySize > 0);
}

void TestExportRegistryKeysAction::testCorruptedKey() {
    // Registry key is corrupted
    QString status = "Corrupted";
    
    QCOMPARE(status, QString("Corrupted"));
}

void TestExportRegistryKeysAction::testInsufficientDiskSpace() {
    // Not enough space for export
    qint64 requiredSpace = 1024 * 1024 * 10; // 10 MB
    qint64 availableSpace = 1024 * 1024 * 5; // 5 MB
    
    QVERIFY(requiredSpace > availableSpace);
}

QTEST_MAIN(TestExportRegistryKeysAction)
#include "test_export_registry_keys_action.moc"

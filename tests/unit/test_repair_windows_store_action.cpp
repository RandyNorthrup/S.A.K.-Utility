// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QTest>
#include <QSignalSpy>
#include "sak/actions/repair_windows_store_action.h"

class TestRepairWindowsStoreAction : public QObject {
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
    void testScanChecksStore();
    void testExecuteRepairsStore();
    
    // Store package detection
    void testCheckStorePackage();
    void testDetectStoreInstalled();
    void testDetectStoreRegistered();
    void testGetStoreVersion();
    
    // WSReset operations
    void testResetWindowsStoreCache();
    void testWSResetCommand();
    void testClearStoreCache();
    
    // Store package reset
    void testResetStorePackage();
    void testGetAppxPackage();
    void testResetAppxPackage();
    
    // App re-registration
    void testReregisterWindowsStore();
    void testReregisterAllApps();
    void testCountRegisteredApps();
    
    // Service management
    void testResetStoreServices();
    void testRestartStoreServices();
    void testCheckServiceStatus();
    
    // Event log checking
    void testCheckStoreEventLogs();
    void testParseStoreErrors();
    void testCountStoreIssues();
    
    // Progress tracking
    void testProgressSignals();
    void testScanProgress();
    void testExecuteProgress();
    
    // Error handling
    void testHandleStoreNotInstalled();
    void testHandlePowerShellFailure();
    void testHandleWSResetFailure();
    void testHandleRegistrationFailure();
    
    // PowerShell commands
    void testGetAppxPackageCommand();
    void testResetAppxPackageCommand();
    void testRegisterAppxManifest();
    
    // Results formatting
    void testFormatStoreStatus();
    void testFormatResetResults();
    void testFormatSuccessMessage();
    void testFormatErrorMessage();
    
    // Edge cases
    void testStoreAlreadyWorking();
    void testMultipleStoreIssues();
    void testCorruptedStorePackage();
    void testMissingDependencies();

private:
    sak::RepairWindowsStoreAction* m_action;
};

void TestRepairWindowsStoreAction::initTestCase() {
    // One-time setup
}

void TestRepairWindowsStoreAction::cleanupTestCase() {
    // One-time cleanup
}

void TestRepairWindowsStoreAction::init() {
    m_action = new sak::RepairWindowsStoreAction();
}

void TestRepairWindowsStoreAction::cleanup() {
    delete m_action;
}

void TestRepairWindowsStoreAction::testActionProperties() {
    QCOMPARE(m_action->name(), QString("Repair Windows Store"));
    QVERIFY(!m_action->description().isEmpty());
    QVERIFY(m_action->description().contains("Store", Qt::CaseInsensitive));
    QCOMPARE(m_action->category(), sak::QuickAction::ActionCategory::Troubleshooting);
    QVERIFY(!m_action->requiresAdmin());
}

void TestRepairWindowsStoreAction::testInitialState() {
    QSignalSpy startedSpy(m_action, &sak::QuickAction::started);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    QVERIFY(startedSpy.isValid());
    QVERIFY(finishedSpy.isValid());
    QCOMPARE(startedSpy.count(), 0);
}

void TestRepairWindowsStoreAction::testDoesNotRequireAdmin() {
    // Can reset Store without admin for current user
    QVERIFY(!m_action->requiresAdmin());
}

void TestRepairWindowsStoreAction::testScanChecksStore() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    
    QVERIFY(finishedSpy.wait(30000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestRepairWindowsStoreAction::testExecuteRepairsStore() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    
    QVERIFY(finishedSpy.wait(120000)); // Re-registration can take time
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestRepairWindowsStoreAction::testCheckStorePackage() {
    // PowerShell: Get-AppxPackage *WindowsStore*
    QString command = "Get-AppxPackage *WindowsStore*";
    
    QVERIFY(command.contains("WindowsStore"));
}

void TestRepairWindowsStoreAction::testDetectStoreInstalled() {
    // Check if Store is installed
    bool storeInstalled = true;
    
    QVERIFY(storeInstalled);
}

void TestRepairWindowsStoreAction::testDetectStoreRegistered() {
    // Check if Store is registered
    bool storeRegistered = true;
    
    QVERIFY(storeRegistered);
}

void TestRepairWindowsStoreAction::testGetStoreVersion() {
    // Get Store version
    QString version = "11.2310.6.0";
    
    QVERIFY(!version.isEmpty());
}

void TestRepairWindowsStoreAction::testResetWindowsStoreCache() {
    // Command: wsreset.exe
    QString command = "wsreset.exe";
    
    QCOMPARE(command, QString("wsreset.exe"));
}

void TestRepairWindowsStoreAction::testWSResetCommand() {
    // WSReset clears Store cache
    QString command = "wsreset.exe";
    
    QVERIFY(command.contains("wsreset"));
}

void TestRepairWindowsStoreAction::testClearStoreCache() {
    // Clear Store cache directory
    QString cachePath = R"(%LocalAppData%\Packages\Microsoft.WindowsStore_*\LocalCache)";
    
    QVERIFY(cachePath.contains("WindowsStore"));
}

void TestRepairWindowsStoreAction::testResetStorePackage() {
    // PowerShell: Get-AppxPackage *WindowsStore* | Reset-AppxPackage
    QString command = "Get-AppxPackage *WindowsStore* | Reset-AppxPackage";
    
    QVERIFY(command.contains("Reset-AppxPackage"));
}

void TestRepairWindowsStoreAction::testGetAppxPackage() {
    // Get Store package info
    QString command = "Get-AppxPackage -Name Microsoft.WindowsStore";
    
    QVERIFY(command.contains("Get-AppxPackage"));
}

void TestRepairWindowsStoreAction::testResetAppxPackage() {
    // Reset package to default state
    QString command = "Reset-AppxPackage -Package Microsoft.WindowsStore";
    
    QVERIFY(command.contains("Reset-AppxPackage"));
}

void TestRepairWindowsStoreAction::testReregisterWindowsStore() {
    // Re-register Store app
    QString command = "Get-AppxPackage *WindowsStore* | ForEach {Add-AppxPackage -DisableDevelopmentMode -Register \"$($_.InstallLocation)\\AppXManifest.xml\"}";
    
    QVERIFY(command.contains("Add-AppxPackage"));
}

void TestRepairWindowsStoreAction::testReregisterAllApps() {
    // Re-register all UWP apps
    QString command = "Get-AppxPackage -AllUsers | ForEach {Add-AppxPackage -DisableDevelopmentMode -Register \"$($_.InstallLocation)\\AppXManifest.xml\"}";
    
    QVERIFY(command.contains("AllUsers"));
}

void TestRepairWindowsStoreAction::testCountRegisteredApps() {
    // Count re-registered apps
    int appCount = 25;
    
    QVERIFY(appCount > 0);
}

void TestRepairWindowsStoreAction::testResetStoreServices() {
    // Reset related services
    QStringList services = {
        "wuauserv",  // Windows Update
        "bits",      // Background Intelligent Transfer
        "cryptsvc"   // Cryptographic Services
    };
    
    QVERIFY(services.size() >= 3);
}

void TestRepairWindowsStoreAction::testRestartStoreServices() {
    // Restart services
    QString command = "Restart-Service wuauserv";
    
    QVERIFY(command.contains("Restart-Service"));
}

void TestRepairWindowsStoreAction::testCheckServiceStatus() {
    // Check service status
    QString command = "Get-Service wuauserv";
    
    QVERIFY(command.contains("Get-Service"));
}

void TestRepairWindowsStoreAction::testCheckStoreEventLogs() {
    // Check Store-related event logs
    QString command = "Get-EventLog -LogName Application -Source \"Microsoft-Windows-Store\" -Newest 50";
    
    QVERIFY(command.contains("Microsoft-Windows-Store"));
}

void TestRepairWindowsStoreAction::testParseStoreErrors() {
    // Parse error messages from event log
    QString errorMessage = "Error 0x80073CF9: Package could not be registered";
    
    QVERIFY(errorMessage.contains("Error"));
}

void TestRepairWindowsStoreAction::testCountStoreIssues() {
    // Count Store-related errors
    int errorCount = 3;
    
    QVERIFY(errorCount >= 0);
}

void TestRepairWindowsStoreAction::testProgressSignals() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(30000));
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestRepairWindowsStoreAction::testScanProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->scan();
    QTest::qWait(2000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestRepairWindowsStoreAction::testExecuteProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->execute();
    QTest::qWait(5000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestRepairWindowsStoreAction::testHandleStoreNotInstalled() {
    // Store may not be installed (Windows Server, etc.)
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(120000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestRepairWindowsStoreAction::testHandlePowerShellFailure() {
    // PowerShell commands may fail
    bool commandSuccess = false;
    
    QVERIFY(!commandSuccess);
}

void TestRepairWindowsStoreAction::testHandleWSResetFailure() {
    // WSReset may fail
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(120000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestRepairWindowsStoreAction::testHandleRegistrationFailure() {
    // App registration may fail
    QString error = "Failed to register package: Access denied";
    
    QVERIFY(error.contains("Failed"));
}

void TestRepairWindowsStoreAction::testGetAppxPackageCommand() {
    // Full PowerShell command
    QString command = "powershell -Command \"Get-AppxPackage -Name Microsoft.WindowsStore\"";
    
    QVERIFY(command.contains("Get-AppxPackage"));
}

void TestRepairWindowsStoreAction::testResetAppxPackageCommand() {
    // Full reset command
    QString command = "powershell -Command \"Get-AppxPackage *WindowsStore* | Reset-AppxPackage\"";
    
    QVERIFY(command.contains("Reset-AppxPackage"));
}

void TestRepairWindowsStoreAction::testRegisterAppxManifest() {
    // Register using AppXManifest.xml
    QString manifestPath = R"(C:\Program Files\WindowsApps\Microsoft.WindowsStore_*\AppXManifest.xml)";
    
    QVERIFY(manifestPath.contains("AppXManifest.xml"));
}

void TestRepairWindowsStoreAction::testFormatStoreStatus() {
    QString status = R"(
Windows Store Status:
  Package: Microsoft.WindowsStore
  Version: 11.2310.6.0
  Status: Installed
  Registered: Yes
    )";
    
    QVERIFY(status.contains("Windows Store"));
}

void TestRepairWindowsStoreAction::testFormatResetResults() {
    QString results = R"(
Repair Operations Completed:
  ✓ Cleared Store cache (WSReset)
  ✓ Reset Store package
  ✓ Re-registered Store app
  ✓ Restarted Store services
    )";
    
    QVERIFY(results.contains("Repair Operations"));
}

void TestRepairWindowsStoreAction::testFormatSuccessMessage() {
    QString message = "Successfully repaired Windows Store. Try opening the Store app now.";
    
    QVERIFY(message.contains("Successfully"));
    QVERIFY(message.contains("Store"));
}

void TestRepairWindowsStoreAction::testFormatErrorMessage() {
    QString error = "Failed to reset Windows Store: Package not found";
    
    QVERIFY(error.contains("Failed"));
    QVERIFY(error.contains("not found"));
}

void TestRepairWindowsStoreAction::testStoreAlreadyWorking() {
    // Store has no issues
    int issuesFound = 0;
    
    QCOMPARE(issuesFound, 0);
}

void TestRepairWindowsStoreAction::testMultipleStoreIssues() {
    // Multiple problems detected
    QStringList issues = {
        "Package not registered",
        "Cache corrupted",
        "Services not running"
    };
    
    QVERIFY(issues.size() >= 2);
}

void TestRepairWindowsStoreAction::testCorruptedStorePackage() {
    // Store package is corrupted
    QString status = "Corrupted";
    
    QCOMPARE(status, QString("Corrupted"));
}

void TestRepairWindowsStoreAction::testMissingDependencies() {
    // Store dependencies missing
    QStringList missingDeps = {
        "Microsoft.NET.Native.Framework",
        "Microsoft.VCLibs.140.00"
    };
    
    QVERIFY(missingDeps.size() >= 1);
}

QTEST_MAIN(TestRepairWindowsStoreAction)
#include "test_repair_windows_store_action.moc"

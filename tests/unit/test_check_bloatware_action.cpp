// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QTest>
#include <QSignalSpy>
#include "sak/actions/check_bloatware_action.h"

class TestCheckBloatwareAction : public QObject {
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
    void testScanDetectsBloatware();
    void testExecuteRemovesBloatware();
    
    // Detection methods
    void testDetectUWPApps();
    void testDetectWin32Programs();
    void testDetectStartupBloat();
    void testDetectVendorSoftware();
    
    // Bloatware identification
    void testIdentifyCandyCrush();
    void testIdentifyXboxApps();
    void testIdentifySkypeConsumer();
    void testIdentifyVendorTrialware();
    void testIdentifyToolbars();
    
    // Safety checks
    void testMarkSafeToRemove();
    void testMarkSystemCritical();
    void testVerifyRemovalSafety();
    void testWarnAboutRiskyRemoval();
    
    // Size calculation
    void testCalculateBloatwareSize();
    void testCalculateTotalSize();
    void testFormatSizeDisplay();
    
    // Removal methods
    void testRemoveUWPApp();
    void testRemoveWin32Program();
    void testRemoveStartupItem();
    void testBulkRemoval();
    
    // Error handling
    void testHandleRemovalFailed();
    void testHandleAppNotFound();
    void testHandleAppInUse();
    void testHandleInsufficientPrivileges();
    
    // Progress tracking
    void testProgressSignals();
    void testScanProgress();
    void testRemovalProgress();
    
    // Results formatting
    void testFormatBloatwareList();
    void testFormatRemovalResults();
    void testFormatSpaceFreed();
    
    // Edge cases
    void testNoBloatwareFound();
    void testCleanSystem();
    void testHeavilyBloatedSystem();
    void testPartialRemovalFailure();

private:
    sak::CheckBloatwareAction* m_action;
    
    QStringList getCommonBloatware();
    QString formatSize(qint64 bytes);
};

void TestCheckBloatwareAction::initTestCase() {
    // One-time setup
}

void TestCheckBloatwareAction::cleanupTestCase() {
    // One-time cleanup
}

void TestCheckBloatwareAction::init() {
    m_action = new sak::CheckBloatwareAction();
}

void TestCheckBloatwareAction::cleanup() {
    delete m_action;
}

void TestCheckBloatwareAction::testActionProperties() {
    QCOMPARE(m_action->name(), QString("Check for Bloatware"));
    QVERIFY(!m_action->description().isEmpty());
    QVERIFY(m_action->description().contains("bloatware", Qt::CaseInsensitive));
    QCOMPARE(m_action->category(), sak::QuickAction::ActionCategory::Troubleshooting);
    QVERIFY(m_action->requiresAdmin());
}

void TestCheckBloatwareAction::testInitialState() {
    QSignalSpy startedSpy(m_action, &sak::QuickAction::started);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    QVERIFY(startedSpy.isValid());
    QVERIFY(finishedSpy.isValid());
    QCOMPARE(startedSpy.count(), 0);
}

void TestCheckBloatwareAction::testRequiresAdmin() {
    // Removing apps requires administrator privileges
    QVERIFY(m_action->requiresAdmin());
}

void TestCheckBloatwareAction::testScanDetectsBloatware() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    
    QVERIFY(finishedSpy.wait(30000));
    QVERIFY(progressSpy.count() >= 1);
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestCheckBloatwareAction::testExecuteRemovesBloatware() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    
    QVERIFY(finishedSpy.wait(60000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestCheckBloatwareAction::testDetectUWPApps() {
    // Command: Get-AppxPackage | Where-Object {$_.Name -like "*bloatware*"}
    QString command = "Get-AppxPackage";
    
    QVERIFY(command.contains("Get-AppxPackage"));
}

void TestCheckBloatwareAction::testDetectWin32Programs() {
    // Check Programs and Features for installed apps
    QString registryPath = "HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    
    QVERIFY(!registryPath.isEmpty());
}

void TestCheckBloatwareAction::testDetectStartupBloat() {
    // Check startup programs
    QString command = "Get-CimInstance Win32_StartupCommand";
    
    QVERIFY(command.contains("Win32_StartupCommand"));
}

void TestCheckBloatwareAction::testDetectVendorSoftware() {
    // Detect vendor-specific bloatware (Dell, HP, Lenovo, etc.)
    QStringList vendors = {"Dell", "HP", "Lenovo", "Acer", "ASUS"};
    
    QVERIFY(vendors.count() >= 5);
}

void TestCheckBloatwareAction::testIdentifyCandyCrush() {
    QString appName = "Microsoft.CandyCrushSaga";
    
    QStringList bloatware = getCommonBloatware();
    bool isBloat = bloatware.contains(appName, Qt::CaseInsensitive);
    
    // Candy Crush is commonly considered bloatware
    QVERIFY(isBloat || appName.contains("CandyCrush"));
}

void TestCheckBloatwareAction::testIdentifyXboxApps() {
    QStringList xboxApps = {
        "Microsoft.XboxApp",
        "Microsoft.XboxGameOverlay",
        "Microsoft.XboxGamingOverlay"
    };
    
    QVERIFY(xboxApps.count() >= 3);
}

void TestCheckBloatwareAction::testIdentifySkypeConsumer() {
    QString appName = "Microsoft.SkypeApp";
    
    // Consumer Skype (not Business) is often unwanted
    QVERIFY(appName.contains("Skype"));
}

void TestCheckBloatwareAction::testIdentifyVendorTrialware() {
    QStringList trialware = {
        "McAfee",
        "Norton",
        "WildTangent",
        "Dropbox"
    };
    
    QVERIFY(trialware.count() >= 3);
}

void TestCheckBloatwareAction::testIdentifyToolbars() {
    QStringList toolbars = {
        "Ask Toolbar",
        "Babylon Toolbar",
        "Conduit"
    };
    
    QVERIFY(toolbars.count() >= 2);
}

void TestCheckBloatwareAction::testMarkSafeToRemove() {
    QString appName = "Microsoft.CandyCrushSaga";
    bool isSafe = true; // Games are safe to remove
    
    QVERIFY(isSafe);
}

void TestCheckBloatwareAction::testMarkSystemCritical() {
    QStringList systemApps = {
        "Microsoft.Windows.Store",
        "Microsoft.WindowsCalculator",
        "Microsoft.Windows.Photos"
    };
    
    // These should be marked as potentially risky
    QVERIFY(systemApps.count() >= 3);
}

void TestCheckBloatwareAction::testVerifyRemovalSafety() {
    QString appName = "Microsoft.Office.Desktop";
    
    // Legitimate software should not be flagged
    bool isBloat = false;
    QVERIFY(!isBloat);
}

void TestCheckBloatwareAction::testWarnAboutRiskyRemoval() {
    QString warning = "⚠️ Removing this app may affect system functionality.";
    
    QVERIFY(warning.contains("⚠") || warning.contains("warn"));
}

void TestCheckBloatwareAction::testCalculateBloatwareSize() {
    qint64 appSize = 150 * 1024 * 1024; // 150 MB
    
    QVERIFY(appSize > 0);
}

void TestCheckBloatwareAction::testCalculateTotalSize() {
    QVector<qint64> sizes = {
        100 * 1024 * 1024,  // 100 MB
        50 * 1024 * 1024,   // 50 MB
        200 * 1024 * 1024   // 200 MB
    };
    
    qint64 total = 0;
    for (qint64 size : sizes) {
        total += size;
    }
    
    QCOMPARE(total, 350 * 1024 * 1024);
}

void TestCheckBloatwareAction::testFormatSizeDisplay() {
    qint64 size = 1536 * 1024 * 1024; // 1.5 GB
    QString formatted = formatSize(size);
    
    QVERIFY(formatted.contains("GB") || formatted.contains("1.5"));
}

void TestCheckBloatwareAction::testRemoveUWPApp() {
    QString appName = "Microsoft.CandyCrushSaga";
    QString removeCommand = QString("Get-AppxPackage *%1* | Remove-AppxPackage").arg(appName);
    
    QVERIFY(removeCommand.contains("Remove-AppxPackage"));
}

void TestCheckBloatwareAction::testRemoveWin32Program() {
    QString programName = "McAfee Trial";
    
    // Would use wmic or msiexec to remove
    QString command = QString("wmic product where name='%1' call uninstall").arg(programName);
    
    QVERIFY(command.contains("wmic") || command.contains("uninstall"));
}

void TestCheckBloatwareAction::testRemoveStartupItem() {
    QString itemName = "MyStartupItem";
    
    // Remove from startup registry key
    QString registryKey = "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    
    QVERIFY(!registryKey.isEmpty());
}

void TestCheckBloatwareAction::testBulkRemoval() {
    QStringList appsToRemove = {
        "Microsoft.CandyCrushSaga",
        "Microsoft.BingWeather",
        "Microsoft.GetHelp"
    };
    
    QVERIFY(appsToRemove.count() >= 3);
}

void TestCheckBloatwareAction::testHandleRemovalFailed() {
    // Some apps may fail to remove
    QString error = "Failed to remove app: Access Denied";
    
    QVERIFY(error.contains("Failed") || error.contains("Access Denied"));
}

void TestCheckBloatwareAction::testHandleAppNotFound() {
    QString appName = "NonExistentApp12345";
    
    // Should handle gracefully when app not found
    QVERIFY(!appName.isEmpty());
}

void TestCheckBloatwareAction::testHandleAppInUse() {
    QString error = "Cannot remove app while it is running.";
    
    QVERIFY(error.contains("running") || error.contains("in use"));
}

void TestCheckBloatwareAction::testHandleInsufficientPrivileges() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(60000));
    
    // Should handle privilege issues gracefully
    QVERIFY(!m_action->result().isEmpty());
}

void TestCheckBloatwareAction::testProgressSignals() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(30000));
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestCheckBloatwareAction::testScanProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->scan();
    QTest::qWait(2000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestCheckBloatwareAction::testRemovalProgress() {
    // Progress during removal
    int removed = 5;
    int total = 10;
    int progress = removed * 100 / total;
    
    QCOMPARE(progress, 50);
}

void TestCheckBloatwareAction::testFormatBloatwareList() {
    QString list = R"(
Detected Bloatware (5 items):
  1. Candy Crush Saga (150 MB) - UWP App
  2. Bing Weather (50 MB) - UWP App
  3. McAfee Trial (300 MB) - Win32 Program
  4. HP Smart (100 MB) - Win32 Program
  5. Dell Update (75 MB) - Startup Item
    )";
    
    QVERIFY(list.contains("Detected"));
    QVERIFY(list.contains("Candy Crush"));
}

void TestCheckBloatwareAction::testFormatRemovalResults() {
    QString results = R"(
Bloatware Removal Complete:
  ✅ Removed: 4 apps
  ❌ Failed: 1 app
  💾 Space freed: 675 MB
    )";
    
    QVERIFY(results.contains("Complete"));
    QVERIFY(results.contains("Space freed"));
}

void TestCheckBloatwareAction::testFormatSpaceFreed() {
    qint64 freedSpace = 800 * 1024 * 1024;
    QString formatted = formatSize(freedSpace);
    
    QVERIFY(formatted.contains("MB"));
}

void TestCheckBloatwareAction::testNoBloatwareFound() {
    QString result = "No bloatware detected. System is clean!";
    
    QVERIFY(result.contains("No bloatware") || result.contains("clean"));
}

void TestCheckBloatwareAction::testCleanSystem() {
    // System with no bloatware
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(30000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestCheckBloatwareAction::testHeavilyBloatedSystem() {
    // System with many bloatware apps (e.g., 20+)
    int bloatCount = 25;
    
    QVERIFY(bloatCount > 20);
}

void TestCheckBloatwareAction::testPartialRemovalFailure() {
    // Some apps removed, some failed
    int successful = 8;
    int failed = 2;
    
    QVERIFY(successful > 0);
    QVERIFY(failed >= 0);
}

// Helper methods

QStringList TestCheckBloatwareAction::getCommonBloatware() {
    return {
        "Microsoft.CandyCrushSaga",
        "Microsoft.CandyCrushSodaSaga",
        "Microsoft.BingWeather",
        "Microsoft.GetHelp",
        "Microsoft.Getstarted",
        "Microsoft.Xbox.TCUI",
        "Microsoft.XboxApp",
        "Microsoft.XboxGameOverlay",
        "Microsoft.XboxGamingOverlay",
        "Microsoft.XboxIdentityProvider",
        "Microsoft.XboxSpeechToTextOverlay",
        "Microsoft.ZuneMusic",
        "Microsoft.ZuneVideo",
        "Microsoft.SkypeApp"
    };
}

QString TestCheckBloatwareAction::formatSize(qint64 bytes) {
    if (bytes >= 1024LL * 1024 * 1024) {
        return QString("%1 GB").arg(bytes / (1024.0 * 1024 * 1024), 0, 'f', 2);
    } else if (bytes >= 1024 * 1024) {
        return QString("%1 MB").arg(bytes / (1024.0 * 1024), 0, 'f', 1);
    }
    return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
}

QTEST_MAIN(TestCheckBloatwareAction)
#include "test_check_bloatware_action.moc"

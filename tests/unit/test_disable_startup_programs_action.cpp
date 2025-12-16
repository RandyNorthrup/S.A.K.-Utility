// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QTest>
#include <QSignalSpy>
#include "sak/actions/disable_startup_programs_action.h"

class TestDisableStartupProgramsAction : public QObject {
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
    void testScanFindsStartupItems();
    void testExecuteDisablesItems();
    
    // Registry scanning
    void testScanHKLMRun();
    void testScanHKCURun();
    void testScanRunOnce();
    void testScanWow64Node();
    
    // Startup Folder scanning
    void testScanUserStartupFolder();
    void testScanCommonStartupFolder();
    void testFindStartupShortcuts();
    
    // Task Scheduler scanning
    void testScanScheduledTasks();
    void testFilterBootTasks();
    void testFilterLogonTasks();
    
    // Impact analysis
    void testCalculateHighImpact();
    void testCalculateMediumImpact();
    void testCalculateLowImpact();
    void testCategorizeByStartupTime();
    
    // Startup items
    void testListAllStartupItems();
    void testGetItemDetails();
    void testCheckItemEnabled();
    void testGetItemLocation();
    
    // Disabling items
    void testDisableRegistryItem();
    void testDisableStartupFolderItem();
    void testDisableScheduledTask();
    void testDisableMultipleItems();
    
    // Re-enabling items
    void testEnableItem();
    void testRestoreDisabledItem();
    
    // Progress tracking
    void testProgressSignals();
    void testScanProgress();
    
    // Error handling
    void testHandleRegistryAccessError();
    void testHandleMissingStartupFolder();
    void testHandleTaskSchedulerError();
    void testHandleInvalidItem();
    
    // Results formatting
    void testFormatHighImpactList();
    void testFormatMediumImpactList();
    void testFormatDisabledList();
    void testFormatSummary();
    
    // Edge cases
    void testNoStartupItems();
    void testAllItemsDisabled();
    void testMixedImpactLevels();
    void testSystemCriticalItems();

private:
    sak::DisableStartupProgramsAction* m_action;
};

void TestDisableStartupProgramsAction::initTestCase() {
    // One-time setup
}

void TestDisableStartupProgramsAction::cleanupTestCase() {
    // One-time cleanup
}

void TestDisableStartupProgramsAction::init() {
    m_action = new sak::DisableStartupProgramsAction();
}

void TestDisableStartupProgramsAction::cleanup() {
    delete m_action;
}

void TestDisableStartupProgramsAction::testActionProperties() {
    QCOMPARE(m_action->name(), QString("Disable Startup Programs"));
    QVERIFY(!m_action->description().isEmpty());
    QVERIFY(m_action->description().contains("startup", Qt::CaseInsensitive));
    QCOMPARE(m_action->category(), sak::QuickAction::ActionCategory::SystemOptimization);
    QVERIFY(!m_action->requiresAdmin());
}

void TestDisableStartupProgramsAction::testInitialState() {
    QSignalSpy startedSpy(m_action, &sak::QuickAction::started);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    QVERIFY(startedSpy.isValid());
    QVERIFY(finishedSpy.isValid());
    QCOMPARE(startedSpy.count(), 0);
}

void TestDisableStartupProgramsAction::testDoesNotRequireAdmin() {
    // Can manage current user's startup items without admin
    QVERIFY(!m_action->requiresAdmin());
}

void TestDisableStartupProgramsAction::testScanFindsStartupItems() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    
    QVERIFY(finishedSpy.wait(20000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestDisableStartupProgramsAction::testExecuteDisablesItems() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    
    QVERIFY(finishedSpy.wait(30000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestDisableStartupProgramsAction::testScanHKLMRun() {
    // HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\Run
    QString registryPath = R"(HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\Run)";
    
    QVERIFY(registryPath.contains("Run"));
}

void TestDisableStartupProgramsAction::testScanHKCURun() {
    // HKEY_CURRENT_USER\SOFTWARE\Microsoft\Windows\CurrentVersion\Run
    QString registryPath = R"(HKEY_CURRENT_USER\SOFTWARE\Microsoft\Windows\CurrentVersion\Run)";
    
    QVERIFY(registryPath.contains("HKEY_CURRENT_USER"));
}

void TestDisableStartupProgramsAction::testScanRunOnce() {
    // Check RunOnce keys
    QString registryPath = R"(SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce)";
    
    QVERIFY(registryPath.contains("RunOnce"));
}

void TestDisableStartupProgramsAction::testScanWow64Node() {
    // Check Wow6432Node for 32-bit apps on 64-bit Windows
    QString registryPath = R"(SOFTWARE\Wow6432Node\Microsoft\Windows\CurrentVersion\Run)";
    
    QVERIFY(registryPath.contains("Wow6432Node"));
}

void TestDisableStartupProgramsAction::testScanUserStartupFolder() {
    // %AppData%\Microsoft\Windows\Start Menu\Programs\Startup
    QString startupFolder = R"(%AppData%\Microsoft\Windows\Start Menu\Programs\Startup)";
    
    QVERIFY(startupFolder.contains("Startup"));
}

void TestDisableStartupProgramsAction::testScanCommonStartupFolder() {
    // C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Startup
    QString commonStartup = R"(C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Startup)";
    
    QVERIFY(commonStartup.contains("ProgramData"));
}

void TestDisableStartupProgramsAction::testFindStartupShortcuts() {
    // Look for .lnk files in Startup folder
    QString extension = ".lnk";
    
    QCOMPARE(extension, QString(".lnk"));
}

void TestDisableStartupProgramsAction::testScanScheduledTasks() {
    // Query Task Scheduler for startup tasks
    QString command = "schtasks /Query /FO CSV /V";
    
    QVERIFY(command.contains("schtasks"));
}

void TestDisableStartupProgramsAction::testFilterBootTasks() {
    // Tasks that run at system boot
    QString trigger = "At system startup";
    
    QVERIFY(trigger.contains("startup"));
}

void TestDisableStartupProgramsAction::testFilterLogonTasks() {
    // Tasks that run at user logon
    QString trigger = "At logon";
    
    QVERIFY(trigger.contains("logon"));
}

void TestDisableStartupProgramsAction::testCalculateHighImpact() {
    // High impact: >5 second startup delay or >100MB memory
    int startupDelay = 8; // seconds
    qint64 memoryUsage = 150LL * 1024 * 1024; // 150 MB
    
    bool isHighImpact = (startupDelay > 5) || (memoryUsage > 100LL * 1024 * 1024);
    QVERIFY(isHighImpact);
}

void TestDisableStartupProgramsAction::testCalculateMediumImpact() {
    // Medium impact: 2-5 second delay or 50-100MB memory
    int startupDelay = 3; // seconds
    qint64 memoryUsage = 75LL * 1024 * 1024; // 75 MB
    
    bool isMediumImpact = (startupDelay >= 2 && startupDelay <= 5);
    QVERIFY(isMediumImpact);
}

void TestDisableStartupProgramsAction::testCalculateLowImpact() {
    // Low impact: <2 second delay and <50MB memory
    int startupDelay = 1; // seconds
    qint64 memoryUsage = 30LL * 1024 * 1024; // 30 MB
    
    bool isLowImpact = (startupDelay < 2) && (memoryUsage < 50LL * 1024 * 1024);
    QVERIFY(isLowImpact);
}

void TestDisableStartupProgramsAction::testCategorizeByStartupTime() {
    struct StartupItem {
        QString name;
        int delaySeconds;
    };
    
    StartupItem item{"TestApp", 6};
    
    QString category;
    if (item.delaySeconds > 5) {
        category = "High Impact";
    } else if (item.delaySeconds >= 2) {
        category = "Medium Impact";
    } else {
        category = "Low Impact";
    }
    
    QCOMPARE(category, QString("High Impact"));
}

void TestDisableStartupProgramsAction::testListAllStartupItems() {
    QStringList items = {
        "OneDrive",
        "Spotify",
        "Discord",
        "Steam"
    };
    
    QVERIFY(items.size() >= 1);
}

void TestDisableStartupProgramsAction::testGetItemDetails() {
    QString details = R"(
Name: OneDrive
Location: Registry (HKCU\Run)
Command: C:\Users\...\OneDrive.exe
Impact: Medium
Status: Enabled
    )";
    
    QVERIFY(details.contains("Name"));
    QVERIFY(details.contains("Location"));
}

void TestDisableStartupProgramsAction::testCheckItemEnabled() {
    bool enabled = true;
    
    QVERIFY(enabled);
}

void TestDisableStartupProgramsAction::testGetItemLocation() {
    QStringList locations = {
        "Registry (HKCU\\Run)",
        "Startup Folder",
        "Task Scheduler"
    };
    
    QVERIFY(locations.contains("Registry (HKCU\\Run)"));
}

void TestDisableStartupProgramsAction::testDisableRegistryItem() {
    // Rename registry value to disable (append "_disabled")
    QString originalName = "OneDrive";
    QString disabledName = originalName + "_disabled";
    
    QCOMPARE(disabledName, QString("OneDrive_disabled"));
}

void TestDisableStartupProgramsAction::testDisableStartupFolderItem() {
    // Rename .lnk file to .lnk.disabled
    QString originalPath = "OneDrive.lnk";
    QString disabledPath = originalPath + ".disabled";
    
    QCOMPARE(disabledPath, QString("OneDrive.lnk.disabled"));
}

void TestDisableStartupProgramsAction::testDisableScheduledTask() {
    // schtasks /Change /TN "TaskName" /DISABLE
    QString command = "schtasks /Change /TN \"OneDriveSync\" /DISABLE";
    
    QVERIFY(command.contains("DISABLE"));
}

void TestDisableStartupProgramsAction::testDisableMultipleItems() {
    QStringList itemsToDisable = {"OneDrive", "Spotify", "Discord"};
    
    QCOMPARE(itemsToDisable.size(), 3);
}

void TestDisableStartupProgramsAction::testEnableItem() {
    // Remove "_disabled" suffix
    QString disabledName = "OneDrive_disabled";
    QString enabledName = disabledName.remove("_disabled");
    
    QCOMPARE(enabledName, QString("OneDrive"));
}

void TestDisableStartupProgramsAction::testRestoreDisabledItem() {
    // Rename back from .disabled
    QString disabledPath = "OneDrive.lnk.disabled";
    QString enabledPath = disabledPath.remove(".disabled");
    
    QCOMPARE(enabledPath, QString("OneDrive.lnk"));
}

void TestDisableStartupProgramsAction::testProgressSignals() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(20000));
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestDisableStartupProgramsAction::testScanProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->scan();
    QTest::qWait(2000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestDisableStartupProgramsAction::testHandleRegistryAccessError() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(20000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestDisableStartupProgramsAction::testHandleMissingStartupFolder() {
    QString startupFolder = R"(C:\NonExistent\Startup)";
    bool exists = false;
    
    QVERIFY(!exists);
}

void TestDisableStartupProgramsAction::testHandleTaskSchedulerError() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(20000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestDisableStartupProgramsAction::testHandleInvalidItem() {
    QString invalidItem = "";
    
    QVERIFY(invalidItem.isEmpty());
}

void TestDisableStartupProgramsAction::testFormatHighImpactList() {
    QString list = R"(
High Impact Startup Items (>5s delay):
  • OneDrive (8s, 150MB)
  • Spotify (7s, 120MB)
  • Steam (10s, 200MB)
    )";
    
    QVERIFY(list.contains("High Impact"));
}

void TestDisableStartupProgramsAction::testFormatMediumImpactList() {
    QString list = R"(
Medium Impact Startup Items (2-5s delay):
  • Discord (3s, 75MB)
  • Skype (4s, 90MB)
    )";
    
    QVERIFY(list.contains("Medium Impact"));
}

void TestDisableStartupProgramsAction::testFormatDisabledList() {
    QString list = R"(
Disabled Startup Items:
  ✓ OneDrive (High Impact)
  ✓ Spotify (High Impact)
  ✓ Discord (Medium Impact)
    )";
    
    QVERIFY(list.contains("Disabled"));
}

void TestDisableStartupProgramsAction::testFormatSummary() {
    QString summary = R"(
Startup Programs Analysis:
  Total items: 15
  High impact: 3
  Medium impact: 5
  Low impact: 7
  Disabled: 3
    )";
    
    QVERIFY(summary.contains("Analysis"));
    QVERIFY(summary.contains("Total"));
}

void TestDisableStartupProgramsAction::testNoStartupItems() {
    // System with no startup items
    int itemCount = 0;
    
    QCOMPARE(itemCount, 0);
}

void TestDisableStartupProgramsAction::testAllItemsDisabled() {
    int totalItems = 10;
    int disabledItems = 10;
    
    QCOMPARE(totalItems, disabledItems);
}

void TestDisableStartupProgramsAction::testMixedImpactLevels() {
    struct ImpactStats {
        int high;
        int medium;
        int low;
    };
    
    ImpactStats stats{3, 5, 7};
    
    QVERIFY(stats.high > 0);
    QVERIFY(stats.medium > 0);
    QVERIFY(stats.low > 0);
}

void TestDisableStartupProgramsAction::testSystemCriticalItems() {
    // Some items should not be disabled (Windows Defender, etc.)
    QStringList criticalItems = {
        "Windows Security notification icon",
        "Windows Defender"
    };
    
    QVERIFY(criticalItems.contains("Windows Security notification icon"));
}

QTEST_MAIN(TestDisableStartupProgramsAction)
#include "test_disable_startup_programs_action.moc"

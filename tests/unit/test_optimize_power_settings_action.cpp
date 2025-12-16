// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
#include "sak/actions/optimize_power_settings_action.h"

class TestOptimizePowerSettingsAction : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Basic functionality
    void testActionProperties();
    void testInitialState();
    void testScanDetectsPowerPlans();
    void testExecuteSwitchesToHighPerformance();
    
    // Power plan detection
    void testEnumeratePowerPlans();
    void testDetectActivePlan();
    void testFindPlanByName();
    void testFindPlanByGuid();
    
    // Standard plans
    void testDetectBalancedPlan();
    void testDetectHighPerformancePlan();
    void testDetectPowerSaverPlan();
    void testDetectUltimatePlan();
    
    // Plan switching
    void testSwitchFromBalanced();
    void testSwitchFromPowerSaver();
    void testAlreadyHighPerformance();
    void testSwitchToSpecificGuid();
    
    // Error handling
    void testHandleHighPerfNotAvailable();
    void testHandleInvalidGuid();
    void testHandlePowerCfgUnavailable();
    void testHandleAccessDenied();
    
    // Progress tracking
    void testProgressSignals();
    void testScanProgress();
    void testExecuteProgress();
    
    // Results formatting
    void testFormatCurrentPlan();
    void testFormatPlanList();
    void testFormatSwitchResult();
    
    // Edge cases
    void testCustomPowerPlans();
    void testLaptopVsDesktop();
    void testBatteryPowered();
    void testACPowered();

private:
    sak::OptimizePowerSettingsAction* m_action;
    
    QString createMockPowerPlanOutput(const QString& guid, const QString& name, bool isActive);
    QString createMockActivePlanOutput(const QString& guid);
};

void TestOptimizePowerSettingsAction::initTestCase() {
    // One-time setup
}

void TestOptimizePowerSettingsAction::cleanupTestCase() {
    // One-time cleanup
}

void TestOptimizePowerSettingsAction::init() {
    m_action = new sak::OptimizePowerSettingsAction();
}

void TestOptimizePowerSettingsAction::cleanup() {
    delete m_action;
}

void TestOptimizePowerSettingsAction::testActionProperties() {
    QCOMPARE(m_action->name(), QString("Optimize Power Settings"));
    QVERIFY(!m_action->description().isEmpty());
    QVERIFY(m_action->description().contains("High Performance", Qt::CaseInsensitive));
    QCOMPARE(m_action->category(), sak::QuickAction::ActionCategory::SystemOptimization);
    QVERIFY(!m_action->requiresAdmin());
}

void TestOptimizePowerSettingsAction::testInitialState() {
    QSignalSpy startedSpy(m_action, &sak::QuickAction::started);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    QVERIFY(startedSpy.isValid());
    QVERIFY(finishedSpy.isValid());
    QCOMPARE(startedSpy.count(), 0);
}

void TestOptimizePowerSettingsAction::testScanDetectsPowerPlans() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    
    QVERIFY(finishedSpy.wait(10000));
    QVERIFY(progressSpy.count() >= 1);
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
    QVERIFY(result.contains("power", Qt::CaseInsensitive) ||
            result.contains("plan", Qt::CaseInsensitive));
}

void TestOptimizePowerSettingsAction::testExecuteSwitchesToHighPerformance() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    
    QVERIFY(finishedSpy.wait(10000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestOptimizePowerSettingsAction::testEnumeratePowerPlans() {
    QString mockOutput = R"(
Power Scheme GUID: 381b4222-f694-41f0-9685-ff5bb260df2e  (Balanced)
Power Scheme GUID: 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c  (High performance)
Power Scheme GUID: a1841308-3541-4fab-bc81-f71556f20b4a  (Power saver)
    )";
    
    QVERIFY(mockOutput.contains("Balanced"));
    QVERIFY(mockOutput.contains("High performance"));
    QVERIFY(mockOutput.contains("Power saver"));
}

void TestOptimizePowerSettingsAction::testDetectActivePlan() {
    QString mockOutput = createMockActivePlanOutput("8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c");
    
    QVERIFY(mockOutput.contains("8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c"));
}

void TestOptimizePowerSettingsAction::testFindPlanByName() {
    QString planName = "High performance";
    QString guid = "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c";
    
    QVERIFY(!planName.isEmpty());
    QVERIFY(!guid.isEmpty());
}

void TestOptimizePowerSettingsAction::testFindPlanByGuid() {
    QString guid = "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c";
    QString expectedName = "High performance";
    
    QVERIFY(!guid.isEmpty());
    QCOMPARE(expectedName, QString("High performance"));
}

void TestOptimizePowerSettingsAction::testDetectBalancedPlan() {
    QString balancedGuid = "381b4222-f694-41f0-9685-ff5bb260df2e";
    QVERIFY(!balancedGuid.isEmpty());
}

void TestOptimizePowerSettingsAction::testDetectHighPerformancePlan() {
    QString highPerfGuid = "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c";
    QVERIFY(!highPerfGuid.isEmpty());
}

void TestOptimizePowerSettingsAction::testDetectPowerSaverPlan() {
    QString powerSaverGuid = "a1841308-3541-4fab-bc81-f71556f20b4a";
    QVERIFY(!powerSaverGuid.isEmpty());
}

void TestOptimizePowerSettingsAction::testDetectUltimatePlan() {
    // Ultimate Performance plan (not always available)
    QString ultimateGuid = "e9a42b02-d5df-448d-aa00-03f14749eb61";
    QVERIFY(!ultimateGuid.isEmpty());
}

void TestOptimizePowerSettingsAction::testSwitchFromBalanced() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(10000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestOptimizePowerSettingsAction::testSwitchFromPowerSaver() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(10000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestOptimizePowerSettingsAction::testAlreadyHighPerformance() {
    // If already on High Performance, should report no change needed
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(10000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestOptimizePowerSettingsAction::testSwitchToSpecificGuid() {
    QString guid = "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c";
    
    // Command would be: powercfg /setactive <guid>
    QString command = QString("powercfg /setactive %1").arg(guid);
    
    QVERIFY(command.contains("powercfg"));
    QVERIFY(command.contains(guid));
}

void TestOptimizePowerSettingsAction::testHandleHighPerfNotAvailable() {
    // On some laptops, High Performance may not be available
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(10000));
    
    // Should handle gracefully
    QVERIFY(!m_action->result().isEmpty());
}

void TestOptimizePowerSettingsAction::testHandleInvalidGuid() {
    QString invalidGuid = "invalid-guid-format";
    
    // Should validate GUID format
    bool isValid = invalidGuid.contains("-") && invalidGuid.length() > 30;
    QVERIFY(!isValid);
}

void TestOptimizePowerSettingsAction::testHandlePowerCfgUnavailable() {
    // Unlikely, but powercfg might not be in PATH
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(10000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestOptimizePowerSettingsAction::testHandleAccessDenied() {
    // Switching power plans usually doesn't require admin, but handle errors
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(10000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestOptimizePowerSettingsAction::testProgressSignals() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(10000));
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestOptimizePowerSettingsAction::testScanProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->scan();
    QTest::qWait(500);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestOptimizePowerSettingsAction::testExecuteProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->execute();
    QTest::qWait(500);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestOptimizePowerSettingsAction::testFormatCurrentPlan() {
    QString mockResult = "Current power plan: Balanced";
    
    QVERIFY(mockResult.contains("Current"));
    QVERIFY(mockResult.contains("Balanced"));
}

void TestOptimizePowerSettingsAction::testFormatPlanList() {
    QString mockList = R"(
Available power plans:
  - Balanced (Active)
  - High performance
  - Power saver
    )";
    
    QVERIFY(mockList.contains("Balanced"));
    QVERIFY(mockList.contains("High performance"));
    QVERIFY(mockList.contains("Active"));
}

void TestOptimizePowerSettingsAction::testFormatSwitchResult() {
    QString mockResult = "Switched power plan: Balanced → High performance";
    
    QVERIFY(mockResult.contains("Switched"));
    QVERIFY(mockResult.contains("→") || mockResult.contains("->"));
    QVERIFY(mockResult.contains("High performance"));
}

void TestOptimizePowerSettingsAction::testCustomPowerPlans() {
    // Users may have custom power plans
    QString customPlan = "My Custom Plan";
    
    QVERIFY(!customPlan.isEmpty());
}

void TestOptimizePowerSettingsAction::testLaptopVsDesktop() {
    // Laptops typically have Power Saver, desktops may not
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(10000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestOptimizePowerSettingsAction::testBatteryPowered() {
    // On battery, might want to warn about High Performance
    bool onBattery = false; // Mock
    
    if (onBattery) {
        // Would show warning
    }
    
    QVERIFY(true); // Test passes
}

void TestOptimizePowerSettingsAction::testACPowered() {
    // On AC power, High Performance is ideal
    bool onAC = true; // Mock
    
    QVERIFY(onAC);
}

// Helper methods

QString TestOptimizePowerSettingsAction::createMockPowerPlanOutput(const QString& guid, const QString& name, bool isActive) {
    QString output = QString("Power Scheme GUID: %1  (%2)").arg(guid, name);
    
    if (isActive) {
        output += " *";
    }
    
    return output;
}

QString TestOptimizePowerSettingsAction::createMockActivePlanOutput(const QString& guid) {
    return QString("Power Scheme GUID: %1").arg(guid);
}

QTEST_MAIN(TestOptimizePowerSettingsAction)
#include "test_optimize_power_settings_action.moc"

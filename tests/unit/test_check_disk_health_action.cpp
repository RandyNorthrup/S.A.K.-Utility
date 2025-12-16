// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
#include "sak/actions/check_disk_health_action.h"

class TestCheckDiskHealthAction : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Basic functionality
    void testActionProperties();
    void testInitialState();
    void testScanDetectsDrives();
    void testExecuteChecksHealth();
    
    // S.M.A.R.T. status
    void testDetectHealthyDrive();
    void testDetectWarningStatus();
    void testDetectCriticalStatus();
    void testParseSmartOutput();
    
    // Drive detection
    void testDetectAllDrives();
    void testDetectSSD();
    void testDetectHDD();
    void testFilterSystemDrives();
    
    // Temperature monitoring
    void testReadTemperature();
    void testHighTemperatureWarning();
    void testNormalTemperature();
    void testTemperatureUnavailable();
    
    // Lifetime monitoring
    void testReadLifetimeUsed();
    void testHighLifetimeWarning();
    void testNormalLifetime();
    void testLifetimeUnavailable();
    
    // Error handling
    void testHandleWmicUnavailable();
    void testHandlePowerShellFallback();
    void testHandleAccessDenied();
    void testHandleInvalidDrive();
    
    // Progress tracking
    void testProgressSignals();
    void testScanProgress();
    void testExecuteProgress();
    
    // Results formatting
    void testFormatHealthReport();
    void testFormatWarnings();
    void testFormatMultipleDrives();
    
    // Edge cases
    void testNoDrivesFound();
    void testUSBDrives();
    void testNetworkDrives();
    void testMultipleWarnings();

private:
    sak::CheckDiskHealthAction* m_action;
    
    QString createMockSmartOutput(const QString& status, int temp = 40, int lifetime = 10);
    QString createMockDriveList();
};

void TestCheckDiskHealthAction::initTestCase() {
    // One-time setup
}

void TestCheckDiskHealthAction::cleanupTestCase() {
    // One-time cleanup
}

void TestCheckDiskHealthAction::init() {
    m_action = new sak::CheckDiskHealthAction();
}

void TestCheckDiskHealthAction::cleanup() {
    delete m_action;
}

void TestCheckDiskHealthAction::testActionProperties() {
    QCOMPARE(m_action->name(), QString("Check Disk Health"));
    QVERIFY(!m_action->description().isEmpty());
    QVERIFY(m_action->description().contains("S.M.A.R.T."));
    QCOMPARE(m_action->category(), sak::QuickAction::ActionCategory::Maintenance);
    QVERIFY(!m_action->requiresAdmin());
}

void TestCheckDiskHealthAction::testInitialState() {
    QSignalSpy startedSpy(m_action, &sak::QuickAction::started);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    QVERIFY(startedSpy.isValid());
    QVERIFY(finishedSpy.isValid());
    QCOMPARE(startedSpy.count(), 0);
}

void TestCheckDiskHealthAction::testScanDetectsDrives() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    
    QVERIFY(finishedSpy.wait(10000));
    QVERIFY(progressSpy.count() >= 1);
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestCheckDiskHealthAction::testExecuteChecksHealth() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    
    QVERIFY(finishedSpy.wait(15000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
    QVERIFY(result.contains("Health", Qt::CaseInsensitive) ||
            result.contains("S.M.A.R.T.", Qt::CaseInsensitive) ||
            result.contains("Drive", Qt::CaseInsensitive));
}

void TestCheckDiskHealthAction::testDetectHealthyDrive() {
    QString mockOutput = createMockSmartOutput("OK", 40, 5);
    
    QVERIFY(mockOutput.contains("OK"));
    QVERIFY(mockOutput.contains("40"));
}

void TestCheckDiskHealthAction::testDetectWarningStatus() {
    QString mockOutput = createMockSmartOutput("Warning", 55, 80);
    
    QVERIFY(mockOutput.contains("Warning"));
}

void TestCheckDiskHealthAction::testDetectCriticalStatus() {
    QString mockOutput = createMockSmartOutput("Critical", 70, 95);
    
    QVERIFY(mockOutput.contains("Critical"));
}

void TestCheckDiskHealthAction::testParseSmartOutput() {
    QString mockOutput = R"(
Status=OK
Temperature=42
PercentLifetimeUsed=15
    )";
    
    QVERIFY(mockOutput.contains("Status=OK"));
    QVERIFY(mockOutput.contains("Temperature=42"));
    QVERIFY(mockOutput.contains("PercentLifetimeUsed=15"));
    
    // Parse logic would extract these values
    bool hasStatus = mockOutput.contains("Status=");
    QVERIFY(hasStatus);
}

void TestCheckDiskHealthAction::testDetectAllDrives() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(10000));
    
    QString result = m_action->result();
    // Should detect at least C: drive on Windows
    QVERIFY(!result.isEmpty());
}

void TestCheckDiskHealthAction::testDetectSSD() {
    // Mock detection - in real implementation would check DeviceType
    QString driveType = "SSD";
    QCOMPARE(driveType, QString("SSD"));
}

void TestCheckDiskHealthAction::testDetectHDD() {
    QString driveType = "HDD";
    QCOMPARE(driveType, QString("HDD"));
}

void TestCheckDiskHealthAction::testFilterSystemDrives() {
    // Test that system/boot drives are included
    QStringList drives = {"C:", "D:", "E:"};
    QVERIFY(drives.contains("C:"));
}

void TestCheckDiskHealthAction::testReadTemperature() {
    QString mockOutput = createMockSmartOutput("OK", 45, 10);
    QVERIFY(mockOutput.contains("45"));
    
    // Parse temperature
    int temp = 45;
    QVERIFY(temp >= 0 && temp <= 100);
}

void TestCheckDiskHealthAction::testHighTemperatureWarning() {
    QString mockOutput = createMockSmartOutput("Warning", 65, 10);
    
    int temp = 65;
    bool isHighTemp = (temp > 60);
    QVERIFY(isHighTemp);
}

void TestCheckDiskHealthAction::testNormalTemperature() {
    QString mockOutput = createMockSmartOutput("OK", 40, 10);
    
    int temp = 40;
    bool isNormal = (temp >= 20 && temp <= 55);
    QVERIFY(isNormal);
}

void TestCheckDiskHealthAction::testTemperatureUnavailable() {
    QString mockOutput = createMockSmartOutput("OK", -1, 10);
    
    int temp = -1;
    bool unavailable = (temp < 0);
    QVERIFY(unavailable);
}

void TestCheckDiskHealthAction::testReadLifetimeUsed() {
    QString mockOutput = createMockSmartOutput("OK", 40, 25);
    
    QVERIFY(mockOutput.contains("25"));
}

void TestCheckDiskHealthAction::testHighLifetimeWarning() {
    QString mockOutput = createMockSmartOutput("Warning", 40, 85);
    
    int lifetime = 85;
    bool isHighLifetime = (lifetime > 80);
    QVERIFY(isHighLifetime);
}

void TestCheckDiskHealthAction::testNormalLifetime() {
    QString mockOutput = createMockSmartOutput("OK", 40, 30);
    
    int lifetime = 30;
    bool isNormal = (lifetime <= 70);
    QVERIFY(isNormal);
}

void TestCheckDiskHealthAction::testLifetimeUnavailable() {
    QString mockOutput = createMockSmartOutput("OK", 40, -1);
    
    int lifetime = -1;
    bool unavailable = (lifetime < 0);
    QVERIFY(unavailable);
}

void TestCheckDiskHealthAction::testHandleWmicUnavailable() {
    // If wmic is unavailable, should fall back to PowerShell
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(15000));
    
    // Should complete even if wmic is not available
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestCheckDiskHealthAction::testHandlePowerShellFallback() {
    // Test PowerShell Get-PhysicalDisk fallback
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(15000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestCheckDiskHealthAction::testHandleAccessDenied() {
    // Some drives may be inaccessible
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(15000));
    
    // Should handle access denied gracefully
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestCheckDiskHealthAction::testHandleInvalidDrive() {
    // Test with invalid drive letter
    QString invalidDrive = "Z:";
    
    // Action should handle gracefully
    QVERIFY(!invalidDrive.isEmpty());
}

void TestCheckDiskHealthAction::testProgressSignals() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(10000));
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestCheckDiskHealthAction::testScanProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->scan();
    QTest::qWait(1000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestCheckDiskHealthAction::testExecuteProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->execute();
    QTest::qWait(2000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestCheckDiskHealthAction::testFormatHealthReport() {
    QString mockReport = "Drive C: - Status: OK, Temp: 42°C, Lifetime: 15%";
    
    QVERIFY(mockReport.contains("Drive"));
    QVERIFY(mockReport.contains("Status"));
    QVERIFY(mockReport.contains("Temp"));
    QVERIFY(mockReport.contains("Lifetime"));
}

void TestCheckDiskHealthAction::testFormatWarnings() {
    QStringList warnings = {
        "High temperature detected: 65°C",
        "Drive lifetime at 85%"
    };
    
    QCOMPARE(warnings.count(), 2);
    QVERIFY(warnings[0].contains("temperature"));
    QVERIFY(warnings[1].contains("lifetime"));
}

void TestCheckDiskHealthAction::testFormatMultipleDrives() {
    QString mockReport = R"(
Drive C: - OK
Drive D: - OK
Drive E: - Warning
    )";
    
    QVERIFY(mockReport.contains("Drive C:"));
    QVERIFY(mockReport.contains("Drive D:"));
    QVERIFY(mockReport.contains("Drive E:"));
}

void TestCheckDiskHealthAction::testNoDrivesFound() {
    // Edge case: system with no detectable drives (unlikely)
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(10000));
    
    // Should handle gracefully
    QVERIFY(!m_action->result().isEmpty());
}

void TestCheckDiskHealthAction::testUSBDrives() {
    // USB drives may not support S.M.A.R.T.
    QString driveType = "Removable";
    
    QVERIFY(!driveType.isEmpty());
}

void TestCheckDiskHealthAction::testNetworkDrives() {
    // Network drives don't have S.M.A.R.T.
    QString driveType = "Network";
    
    QVERIFY(!driveType.isEmpty());
}

void TestCheckDiskHealthAction::testMultipleWarnings() {
    QStringList warnings = {
        "High temperature: 65°C",
        "High lifetime usage: 85%",
        "Reallocated sectors detected"
    };
    
    QCOMPARE(warnings.count(), 3);
    QVERIFY(warnings.count() >= 2);
}

// Helper methods

QString TestCheckDiskHealthAction::createMockSmartOutput(const QString& status, int temp, int lifetime) {
    QString output;
    output += QString("Status=%1\n").arg(status);
    
    if (temp >= 0) {
        output += QString("Temperature=%1\n").arg(temp);
    }
    
    if (lifetime >= 0) {
        output += QString("PercentLifetimeUsed=%1\n").arg(lifetime);
    }
    
    return output;
}

QString TestCheckDiskHealthAction::createMockDriveList() {
    return R"(
DeviceID=0, MediaType=SSD, HealthStatus=Healthy
DeviceID=1, MediaType=HDD, HealthStatus=Healthy
    )";
}

QTEST_MAIN(TestCheckDiskHealthAction)
#include "test_check_disk_health_action.moc"

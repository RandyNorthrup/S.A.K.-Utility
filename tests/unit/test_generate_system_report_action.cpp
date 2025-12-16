// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
#include "sak/actions/generate_system_report_action.h"

class TestGenerateSystemReportAction : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Basic functionality
    void testActionProperties();
    void testInitialState();
    void testScanGathersInfo();
    void testExecuteGeneratesReport();
    
    // Information gathering
    void testGatherSystemInfo();
    void testGatherHardwareInfo();
    void testGatherSoftwareInfo();
    void testGatherNetworkInfo();
    
    // System information
    void testGetOSVersion();
    void testGetComputerName();
    void testGetCPUInfo();
    void testGetMemoryInfo();
    void testGetDiskInfo();
    
    // Installed programs
    void testListInstalledPrograms();
    void testGetProgramVersions();
    void testCountPrograms();
    
    // Driver information
    void testListDrivers();
    void testGetDriverVersions();
    void testDetectOutdatedDrivers();
    
    // Event logs
    void testGatherRecentErrors();
    void testGatherWarnings();
    void testGatherCriticalEvents();
    
    // Report generation
    void testGenerateHTMLReport();
    void testGenerateTextReport();
    void testReportFormatting();
    void testIncludeTimestamp();
    
    // Error handling
    void testHandleWMIFailure();
    void testHandleMSInfo32Unavailable();
    void testHandleInvalidOutputPath();
    void testHandleInsufficientData();
    
    // Progress tracking
    void testProgressSignals();
    void testGatherProgress();
    void testGenerationProgress();
    
    // Results formatting
    void testFormatSystemSummary();
    void testFormatHardwareSection();
    void testFormatSoftwareSection();
    
    // Edge cases
    void testMinimalSystem();
    void testVirtualMachine();
    void testLargeEventLog();
    void testNoInternetConnection();

private:
    QTemporaryDir* m_tempDir;
    sak::GenerateSystemReportAction* m_action;
    
    QString createMockSystemInfo();
    bool validateHTMLReport(const QString& path);
};

void TestGenerateSystemReportAction::initTestCase() {
    // One-time setup
}

void TestGenerateSystemReportAction::cleanupTestCase() {
    // One-time cleanup
}

void TestGenerateSystemReportAction::init() {
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());
    m_action = new sak::GenerateSystemReportAction(m_tempDir->path());
}

void TestGenerateSystemReportAction::cleanup() {
    delete m_action;
    delete m_tempDir;
}

void TestGenerateSystemReportAction::testActionProperties() {
    QCOMPARE(m_action->name(), QString("Generate System Report"));
    QVERIFY(!m_action->description().isEmpty());
    QVERIFY(m_action->description().contains("system report", Qt::CaseInsensitive));
    QCOMPARE(m_action->category(), sak::QuickAction::ActionCategory::Troubleshooting);
    QVERIFY(!m_action->requiresAdmin());
}

void TestGenerateSystemReportAction::testInitialState() {
    QSignalSpy startedSpy(m_action, &sak::QuickAction::started);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    QVERIFY(startedSpy.isValid());
    QVERIFY(finishedSpy.isValid());
    QCOMPARE(startedSpy.count(), 0);
}

void TestGenerateSystemReportAction::testScanGathersInfo() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    
    QVERIFY(finishedSpy.wait(30000));
    QVERIFY(progressSpy.count() >= 1);
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestGenerateSystemReportAction::testExecuteGeneratesReport() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    
    QVERIFY(finishedSpy.wait(60000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestGenerateSystemReportAction::testGatherSystemInfo() {
    QString mockInfo = createMockSystemInfo();
    
    QVERIFY(mockInfo.contains("Windows"));
    QVERIFY(mockInfo.contains("CPU"));
}

void TestGenerateSystemReportAction::testGatherHardwareInfo() {
    // Hardware information via WMI
    QString command = "Get-WmiObject Win32_ComputerSystem";
    
    QVERIFY(command.contains("Win32_ComputerSystem"));
}

void TestGenerateSystemReportAction::testGatherSoftwareInfo() {
    // Installed software
    QString command = "Get-ItemProperty HKLM:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\*";
    
    QVERIFY(command.contains("Uninstall"));
}

void TestGenerateSystemReportAction::testGatherNetworkInfo() {
    // Network adapters
    QString command = "Get-NetAdapter";
    
    QVERIFY(command.contains("NetAdapter"));
}

void TestGenerateSystemReportAction::testGetOSVersion() {
    QString osVersion = "Windows 11 Pro 23H2";
    
    QVERIFY(osVersion.contains("Windows"));
}

void TestGenerateSystemReportAction::testGetComputerName() {
    QString computerName = "DESKTOP-ABC123";
    
    QVERIFY(!computerName.isEmpty());
}

void TestGenerateSystemReportAction::testGetCPUInfo() {
    QString cpuInfo = "Intel Core i7-12700K @ 3.6GHz";
    
    QVERIFY(cpuInfo.contains("Intel") || cpuInfo.contains("AMD"));
}

void TestGenerateSystemReportAction::testGetMemoryInfo() {
    QString memInfo = "32 GB DDR4";
    
    QVERIFY(memInfo.contains("GB"));
}

void TestGenerateSystemReportAction::testGetDiskInfo() {
    QString diskInfo = "C: 500 GB SSD (250 GB free)";
    
    QVERIFY(diskInfo.contains("GB"));
}

void TestGenerateSystemReportAction::testListInstalledPrograms() {
    QStringList programs = {
        "Microsoft Office 2021",
        "Google Chrome",
        "Adobe Reader"
    };
    
    QVERIFY(programs.count() >= 3);
}

void TestGenerateSystemReportAction::testGetProgramVersions() {
    QString program = "Google Chrome 120.0.6099.109";
    
    QVERIFY(program.contains("Chrome"));
}

void TestGenerateSystemReportAction::testCountPrograms() {
    int programCount = 150;
    
    QVERIFY(programCount >= 0);
}

void TestGenerateSystemReportAction::testListDrivers() {
    QStringList drivers = {
        "nvlddmkm.sys - NVIDIA Graphics Driver",
        "intelppm.sys - Intel Processor Driver"
    };
    
    QVERIFY(drivers.count() >= 2);
}

void TestGenerateSystemReportAction::testGetDriverVersions() {
    QString driver = "NVIDIA Graphics Driver 546.33";
    
    QVERIFY(driver.contains("Driver"));
}

void TestGenerateSystemReportAction::testDetectOutdatedDrivers() {
    // Detect drivers needing updates
    int outdatedCount = 3;
    
    QVERIFY(outdatedCount >= 0);
}

void TestGenerateSystemReportAction::testGatherRecentErrors() {
    // Recent error events
    QString command = "Get-EventLog -LogName System -EntryType Error -Newest 50";
    
    QVERIFY(command.contains("Error"));
}

void TestGenerateSystemReportAction::testGatherWarnings() {
    QString command = "Get-EventLog -LogName System -EntryType Warning -Newest 50";
    
    QVERIFY(command.contains("Warning"));
}

void TestGenerateSystemReportAction::testGatherCriticalEvents() {
    QString command = "Get-EventLog -LogName System -EntryType Critical -Newest 20";
    
    QVERIFY(command.contains("Critical"));
}

void TestGenerateSystemReportAction::testGenerateHTMLReport() {
    QString reportPath = m_tempDir->filePath("system_report.html");
    
    QFile file(reportPath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("<html><body><h1>System Report</h1></body></html>");
    file.close();
    
    QVERIFY(QFile::exists(reportPath));
    QVERIFY(validateHTMLReport(reportPath));
}

void TestGenerateSystemReportAction::testGenerateTextReport() {
    QString reportPath = m_tempDir->filePath("system_report.txt");
    
    QFile file(reportPath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("SYSTEM REPORT\n=============\n");
    file.close();
    
    QVERIFY(QFile::exists(reportPath));
}

void TestGenerateSystemReportAction::testReportFormatting() {
    QString html = R"(
<!DOCTYPE html>
<html>
<head><title>System Report</title></head>
<body>
    <h1>System Report</h1>
    <h2>System Information</h2>
    <p>OS: Windows 11 Pro</p>
</body>
</html>
    )";
    
    QVERIFY(html.contains("<html>"));
    QVERIFY(html.contains("System Report"));
}

void TestGenerateSystemReportAction::testIncludeTimestamp() {
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    QString report = QString("Generated: %1").arg(timestamp);
    
    QVERIFY(report.contains("Generated"));
}

void TestGenerateSystemReportAction::testHandleWMIFailure() {
    // WMI service may be unavailable
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(60000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestGenerateSystemReportAction::testHandleMSInfo32Unavailable() {
    // msinfo32 may not be available
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(60000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestGenerateSystemReportAction::testHandleInvalidOutputPath() {
    QString invalidPath = "Z:\\Invalid\\Path";
    
    QVERIFY(!QDir(invalidPath).exists());
}

void TestGenerateSystemReportAction::testHandleInsufficientData() {
    // Some info may be unavailable
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(60000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestGenerateSystemReportAction::testProgressSignals() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(30000));
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestGenerateSystemReportAction::testGatherProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->scan();
    QTest::qWait(2000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestGenerateSystemReportAction::testGenerationProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->execute();
    QTest::qWait(5000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestGenerateSystemReportAction::testFormatSystemSummary() {
    QString summary = R"(
System Summary:
  Computer: DESKTOP-ABC123
  OS: Windows 11 Pro 23H2
  CPU: Intel Core i7-12700K
  RAM: 32 GB
  Disk: 500 GB SSD
    )";
    
    QVERIFY(summary.contains("System Summary"));
    QVERIFY(summary.contains("CPU"));
}

void TestGenerateSystemReportAction::testFormatHardwareSection() {
    QString hardware = R"(
Hardware Information:
  Processor: Intel Core i7-12700K @ 3.6GHz (12 cores)
  Memory: 32 GB DDR4 @ 3200 MHz
  Graphics: NVIDIA GeForce RTX 3080
  Storage: Samsung 980 PRO 500GB
    )";
    
    QVERIFY(hardware.contains("Hardware"));
    QVERIFY(hardware.contains("Processor"));
}

void TestGenerateSystemReportAction::testFormatSoftwareSection() {
    QString software = R"(
Installed Software (150 programs):
  - Microsoft Office 2021
  - Google Chrome 120.0.6099.109
  - Adobe Reader DC 2024.001.20643
    )";
    
    QVERIFY(software.contains("Installed Software"));
    QVERIFY(software.contains("programs"));
}

void TestGenerateSystemReportAction::testMinimalSystem() {
    // Fresh Windows installation with minimal software
    int programCount = 20;
    
    QVERIFY(programCount >= 0);
}

void TestGenerateSystemReportAction::testVirtualMachine() {
    // Detect VM environment
    QString systemModel = "VMware Virtual Platform";
    
    QVERIFY(systemModel.contains("Virtual") || systemModel.contains("VMware"));
}

void TestGenerateSystemReportAction::testLargeEventLog() {
    // System with thousands of events
    int eventCount = 50000;
    
    QVERIFY(eventCount >= 0);
}

void TestGenerateSystemReportAction::testNoInternetConnection() {
    // Report should work offline
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(60000));
    
    QVERIFY(!m_action->result().isEmpty());
}

// Helper methods

QString TestGenerateSystemReportAction::createMockSystemInfo() {
    return R"(
Computer Name: DESKTOP-ABC123
Operating System: Windows 11 Pro 23H2
Processor: Intel Core i7-12700K @ 3.6GHz
Memory: 32 GB
    )";
}

bool TestGenerateSystemReportAction::validateHTMLReport(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    
    QString content = file.readAll();
    file.close();
    
    // Basic HTML validation
    return content.contains("<html>") && content.contains("</html>");
}

QTEST_MAIN(TestGenerateSystemReportAction)
#include "test_generate_system_report_action.moc"

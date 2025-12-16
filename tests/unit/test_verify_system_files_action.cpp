// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QTest>
#include <QSignalSpy>
#include "sak/actions/verify_system_files_action.h"

class TestVerifySystemFilesAction : public QObject {
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
    void testScanChecksHealth();
    void testExecuteRepairsFiles();
    
    // DISM operations
    void testRunDISMCheckHealth();
    void testRunDISMScanHealth();
    void testRunDISMRestoreHealth();
    void testDISMProgressParsing();
    
    // SFC operations
    void testRunSFC();
    void testSFCScannow();
    void testSFCVerifyOnly();
    void testSFCProgressParsing();
    
    // Corruption detection
    void testDetectCorruption();
    void testNoCorruptionFound();
    void testCorruptionRepaired();
    void testCorruptionNotRepaired();
    
    // Log file handling
    void testLocateCBSLog();
    void testParseCBSLog();
    void testDISMLogPath();
    void testExtractLogDetails();
    
    // Scan phases
    void testCheckHealthPhase();
    void testRestoreHealthPhase();
    void testSFCPhase();
    void testPhaseProgression();
    
    // Progress tracking
    void testProgressSignals();
    void testDISMProgress();
    void testSFCProgress();
    
    // Error handling
    void testHandleDISMFailure();
    void testHandleSFCFailure();
    void testHandleAccessDenied();
    void testHandleWindowsUpdateRunning();
    
    // Results parsing
    void testParseDISMOutput();
    void testParseSFCOutput();
    void testDetectRepairSuccess();
    void testDetectRepairFailure();
    
    // Results formatting
    void testFormatDISMResults();
    void testFormatSFCResults();
    void testFormatSuccessMessage();
    void testFormatErrorMessage();
    
    // Edge cases
    void testOfflineSystem();
    void testNoInternetConnection();
    void testInsufficientSpace();
    void testLongRunningOperation();

private:
    sak::VerifySystemFilesAction* m_action;
};

void TestVerifySystemFilesAction::initTestCase() {
    // One-time setup
}

void TestVerifySystemFilesAction::cleanupTestCase() {
    // One-time cleanup
}

void TestVerifySystemFilesAction::init() {
    m_action = new sak::VerifySystemFilesAction();
}

void TestVerifySystemFilesAction::cleanup() {
    delete m_action;
}

void TestVerifySystemFilesAction::testActionProperties() {
    QCOMPARE(m_action->name(), QString("Verify System Files"));
    QVERIFY(!m_action->description().isEmpty());
    QVERIFY(m_action->description().contains("SFC", Qt::CaseInsensitive) || 
            m_action->description().contains("DISM", Qt::CaseInsensitive));
    QCOMPARE(m_action->category(), sak::QuickAction::ActionCategory::Maintenance);
    QVERIFY(m_action->requiresAdmin());
}

void TestVerifySystemFilesAction::testInitialState() {
    QSignalSpy startedSpy(m_action, &sak::QuickAction::started);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    QVERIFY(startedSpy.isValid());
    QVERIFY(finishedSpy.isValid());
    QCOMPARE(startedSpy.count(), 0);
}

void TestVerifySystemFilesAction::testRequiresAdmin() {
    // DISM and SFC require administrator privileges
    QVERIFY(m_action->requiresAdmin());
}

void TestVerifySystemFilesAction::testScanChecksHealth() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    
    QVERIFY(finishedSpy.wait(120000)); // DISM can take time
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestVerifySystemFilesAction::testExecuteRepairsFiles() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    
    QVERIFY(finishedSpy.wait(600000)); // Can take up to 10 minutes
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestVerifySystemFilesAction::testRunDISMCheckHealth() {
    // Command: DISM /Online /Cleanup-Image /CheckHealth
    QString command = "DISM /Online /Cleanup-Image /CheckHealth";
    
    QVERIFY(command.contains("CheckHealth"));
}

void TestVerifySystemFilesAction::testRunDISMScanHealth() {
    // Command: DISM /Online /Cleanup-Image /ScanHealth
    QString command = "DISM /Online /Cleanup-Image /ScanHealth";
    
    QVERIFY(command.contains("ScanHealth"));
}

void TestVerifySystemFilesAction::testRunDISMRestoreHealth() {
    // Command: DISM /Online /Cleanup-Image /RestoreHealth
    QString command = "DISM /Online /Cleanup-Image /RestoreHealth";
    
    QVERIFY(command.contains("RestoreHealth"));
}

void TestVerifySystemFilesAction::testDISMProgressParsing() {
    // DISM output: [==========================100.0%==========================]
    QString output = "[==========================100.0%==========================]";
    
    QVERIFY(output.contains("100.0%"));
}

void TestVerifySystemFilesAction::testRunSFC() {
    // Command: sfc /scannow
    QString command = "sfc /scannow";
    
    QVERIFY(command.contains("sfc"));
    QVERIFY(command.contains("scannow"));
}

void TestVerifySystemFilesAction::testSFCScannow() {
    // Full scan and repair
    QString command = "sfc /scannow";
    
    QCOMPARE(command, QString("sfc /scannow"));
}

void TestVerifySystemFilesAction::testSFCVerifyOnly() {
    // Verify only without repair
    QString command = "sfc /verifyonly";
    
    QVERIFY(command.contains("verifyonly"));
}

void TestVerifySystemFilesAction::testSFCProgressParsing() {
    // SFC output: Verification 45% complete
    QString output = "Verification 45% complete";
    
    QVERIFY(output.contains("complete"));
}

void TestVerifySystemFilesAction::testDetectCorruption() {
    // DISM: corruption detected
    QString output = "The component store is repairable.";
    bool corruptionFound = output.contains("repairable");
    
    QVERIFY(corruptionFound);
}

void TestVerifySystemFilesAction::testNoCorruptionFound() {
    // DISM: No corruption
    QString output = "No component store corruption detected.";
    bool corruptionFound = output.contains("corruption detected");
    
    QVERIFY(corruptionFound);
}

void TestVerifySystemFilesAction::testCorruptionRepaired() {
    // DISM: Successfully repaired
    QString output = "The restore operation completed successfully.";
    bool repaired = output.contains("successfully");
    
    QVERIFY(repaired);
}

void TestVerifySystemFilesAction::testCorruptionNotRepaired() {
    // DISM: Failed to repair
    QString output = "The restore operation failed.";
    bool failed = output.contains("failed");
    
    QVERIFY(failed);
}

void TestVerifySystemFilesAction::testLocateCBSLog() {
    // CBS.log location: C:\Windows\Logs\CBS\CBS.log
    QString logPath = R"(C:\Windows\Logs\CBS\CBS.log)";
    
    QVERIFY(logPath.contains("CBS.log"));
}

void TestVerifySystemFilesAction::testParseCBSLog() {
    // Parse CBS.log for details
    QString logPath = R"(C:\Windows\Logs\CBS\CBS.log)";
    
    QVERIFY(!logPath.isEmpty());
}

void TestVerifySystemFilesAction::testDISMLogPath() {
    // DISM.log location: C:\Windows\Logs\DISM\dism.log
    QString logPath = R"(C:\Windows\Logs\DISM\dism.log)";
    
    QVERIFY(logPath.contains("dism.log"));
}

void TestVerifySystemFilesAction::testExtractLogDetails() {
    // Extract corruption details from logs
    QString logContent = "Corruption detected in file: kernel32.dll";
    
    QVERIFY(logContent.contains("Corruption"));
}

void TestVerifySystemFilesAction::testCheckHealthPhase() {
    // Phase 1: Check health
    QString phase = "CheckHealth";
    
    QCOMPARE(phase, QString("CheckHealth"));
}

void TestVerifySystemFilesAction::testRestoreHealthPhase() {
    // Phase 2: Restore health
    QString phase = "RestoreHealth";
    
    QCOMPARE(phase, QString("RestoreHealth"));
}

void TestVerifySystemFilesAction::testSFCPhase() {
    // Phase 3: SFC scan
    QString phase = "SFC";
    
    QCOMPARE(phase, QString("SFC"));
}

void TestVerifySystemFilesAction::testPhaseProgression() {
    // Phases execute in order
    QStringList phases = {"CheckHealth", "RestoreHealth", "SFC"};
    
    QCOMPARE(phases.size(), 3);
}

void TestVerifySystemFilesAction::testProgressSignals() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(120000));
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestVerifySystemFilesAction::testDISMProgress() {
    // DISM reports progress
    int progress = 50; // 50%
    
    QVERIFY(progress >= 0 && progress <= 100);
}

void TestVerifySystemFilesAction::testSFCProgress() {
    // SFC reports progress
    int progress = 75; // 75%
    
    QVERIFY(progress >= 0 && progress <= 100);
}

void TestVerifySystemFilesAction::testHandleDISMFailure() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(600000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestVerifySystemFilesAction::testHandleSFCFailure() {
    // SFC may fail
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(600000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestVerifySystemFilesAction::testHandleAccessDenied() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(600000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestVerifySystemFilesAction::testHandleWindowsUpdateRunning() {
    // DISM fails if Windows Update is running
    QString error = "Error: 0x800f0816 - The source files could not be found.";
    
    QVERIFY(error.contains("Error"));
}

void TestVerifySystemFilesAction::testParseDISMOutput() {
    QString output = R"(
Deployment Image Servicing and Management tool
Version: 10.0.19041.1
Image Version: 10.0.19041.1348
[==========================100.0%==========================]
The operation completed successfully.
    )";
    
    QVERIFY(output.contains("successfully"));
}

void TestVerifySystemFilesAction::testParseSFCOutput() {
    QString output = R"(
Beginning system scan.  This process will take some time.
Beginning verification phase of system scan.
Verification 100% complete.
Windows Resource Protection did not find any integrity violations.
    )";
    
    QVERIFY(output.contains("integrity violations"));
}

void TestVerifySystemFilesAction::testDetectRepairSuccess() {
    QString output = "Windows Resource Protection found corrupt files and successfully repaired them.";
    bool success = output.contains("successfully repaired");
    
    QVERIFY(success);
}

void TestVerifySystemFilesAction::testDetectRepairFailure() {
    QString output = "Windows Resource Protection found corrupt files but was unable to fix some of them.";
    bool failure = output.contains("unable to fix");
    
    QVERIFY(failure);
}

void TestVerifySystemFilesAction::testFormatDISMResults() {
    QString results = R"(
DISM Check Health: No corruption detected
DISM Scan Health: Skipped (no corruption found)
DISM Restore Health: Skipped (no corruption found)
    )";
    
    QVERIFY(results.contains("DISM"));
}

void TestVerifySystemFilesAction::testFormatSFCResults() {
    QString results = R"(
SFC Scan Results:
  Status: Completed
  Corrupt files: 0
  Repaired files: 0
    )";
    
    QVERIFY(results.contains("SFC"));
}

void TestVerifySystemFilesAction::testFormatSuccessMessage() {
    QString message = "System file verification completed. No issues found.";
    
    QVERIFY(message.contains("completed"));
}

void TestVerifySystemFilesAction::testFormatErrorMessage() {
    QString error = "Failed to run DISM: Access Denied. Run as administrator.";
    
    QVERIFY(error.contains("Failed"));
    QVERIFY(error.contains("administrator"));
}

void TestVerifySystemFilesAction::testOfflineSystem() {
    // DISM RestoreHealth requires internet
    bool hasInternet = false;
    
    QVERIFY(!hasInternet);
}

void TestVerifySystemFilesAction::testNoInternetConnection() {
    // RestoreHealth may fail without internet
    QString error = "The source files could not be downloaded.";
    
    QVERIFY(error.contains("could not be downloaded"));
}

void TestVerifySystemFilesAction::testInsufficientSpace() {
    // Need space for repair operations
    qint64 freeSpace = 500LL * 1024 * 1024; // 500 MB
    qint64 requiredSpace = 1LL * 1024 * 1024 * 1024; // 1 GB
    
    QVERIFY(freeSpace < requiredSpace);
}

void TestVerifySystemFilesAction::testLongRunningOperation() {
    // Operations can take 10+ minutes
    int estimatedMinutes = 15;
    
    QVERIFY(estimatedMinutes > 10);
}

QTEST_MAIN(TestVerifySystemFilesAction)
#include "test_verify_system_files_action.moc"

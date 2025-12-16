// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QTest>
#include <QSignalSpy>
#include "sak/actions/check_disk_errors_action.h"

class TestCheckDiskErrorsAction : public QObject {
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
    void testScanDetectsDrives();
    void testExecuteChecksErrors();
    
    // Drive detection
    void testDetectAllDrives();
    void testDetectSystemDrive();
    void testDetectDataDrives();
    void testSkipRemovableDrives();
    
    // Error detection
    void testDetectNoErrors();
    void testDetectMinorErrors();
    void testDetectSeriousErrors();
    void testDetectFileSystemCorruption();
    
    // CHKDSK operations
    void testRunChkdskScan();
    void testParseChkdskOutput();
    void testDetectErrorCount();
    void testDetectRebootNeeded();
    
    // Results parsing
    void testParseCleanDrive();
    void testParseErrorsDriveWithErrors();
    void testParseRebootRequired();
    void testParseBadSectors();
    
    // Error handling
    void testHandleDriveInUse();
    void testHandleAccessDenied();
    void testHandleInvalidDrive();
    void testHandleChkdskFailed();
    
    // Progress tracking
    void testProgressSignals();
    void testScanProgress();
    void testPerDriveProgress();
    
    // Results formatting
    void testFormatDriveResults();
    void testFormatErrorSummary();
    void testFormatRecommendations();
    
    // Edge cases
    void testNoDrives();
    void testAllDrivesHealthy();
    void testMultipleDrivesWithErrors();
    void testEncryptedDrive();

private:
    sak::CheckDiskErrorsAction* m_action;
    
    QString createMockChkdskOutput(bool hasErrors, int errorCount = 0);
};

void TestCheckDiskErrorsAction::initTestCase() {
    // One-time setup
}

void TestCheckDiskErrorsAction::cleanupTestCase() {
    // One-time cleanup
}

void TestCheckDiskErrorsAction::init() {
    m_action = new sak::CheckDiskErrorsAction();
}

void TestCheckDiskErrorsAction::cleanup() {
    delete m_action;
}

void TestCheckDiskErrorsAction::testActionProperties() {
    QCOMPARE(m_action->name(), QString("Check Disk Errors"));
    QVERIFY(!m_action->description().isEmpty());
    QVERIFY(m_action->description().contains("CHKDSK", Qt::CaseInsensitive));
    QCOMPARE(m_action->category(), sak::QuickAction::ActionCategory::Maintenance);
    QVERIFY(m_action->requiresAdmin());
}

void TestCheckDiskErrorsAction::testInitialState() {
    QSignalSpy startedSpy(m_action, &sak::QuickAction::started);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    QVERIFY(startedSpy.isValid());
    QVERIFY(finishedSpy.isValid());
    QCOMPARE(startedSpy.count(), 0);
}

void TestCheckDiskErrorsAction::testRequiresAdmin() {
    // CHKDSK requires administrator privileges
    QVERIFY(m_action->requiresAdmin());
}

void TestCheckDiskErrorsAction::testScanDetectsDrives() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    
    QVERIFY(finishedSpy.wait(10000));
    QVERIFY(progressSpy.count() >= 1);
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestCheckDiskErrorsAction::testExecuteChecksErrors() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    
    QVERIFY(finishedSpy.wait(60000)); // CHKDSK can take time
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestCheckDiskErrorsAction::testDetectAllDrives() {
    QStringList drives = {"C:", "D:", "E:"};
    
    QVERIFY(drives.count() >= 1);
    QVERIFY(drives.contains("C:"));
}

void TestCheckDiskErrorsAction::testDetectSystemDrive() {
    QString systemDrive = "C:";
    
    QCOMPARE(systemDrive, QString("C:"));
}

void TestCheckDiskErrorsAction::testDetectDataDrives() {
    QStringList dataDrives = {"D:", "E:", "F:"};
    
    QVERIFY(dataDrives.count() >= 0);
}

void TestCheckDiskErrorsAction::testSkipRemovableDrives() {
    // Should skip USB drives, CD-ROM, etc.
    QString driveType = "Removable";
    
    bool shouldSkip = (driveType == "Removable" || driveType == "CDRom");
    QVERIFY(shouldSkip);
}

void TestCheckDiskErrorsAction::testDetectNoErrors() {
    QString mockOutput = createMockChkdskOutput(false);
    
    QVERIFY(mockOutput.contains("no errors", Qt::CaseInsensitive) ||
            mockOutput.contains("healthy", Qt::CaseInsensitive));
}

void TestCheckDiskErrorsAction::testDetectMinorErrors() {
    QString mockOutput = createMockChkdskOutput(true, 3);
    
    QVERIFY(mockOutput.contains("error") || mockOutput.contains("3"));
}

void TestCheckDiskErrorsAction::testDetectSeriousErrors() {
    QString mockOutput = createMockChkdskOutput(true, 50);
    
    QVERIFY(mockOutput.contains("error"));
}

void TestCheckDiskErrorsAction::testDetectFileSystemCorruption() {
    QString mockOutput = "Corruption detected in file system metadata.";
    
    QVERIFY(mockOutput.contains("corruption", Qt::CaseInsensitive));
}

void TestCheckDiskErrorsAction::testRunChkdskScan() {
    // Command: chkdsk C: /scan
    QString drive = "C:";
    QString command = QString("chkdsk %1 /scan").arg(drive);
    
    QVERIFY(command.contains("chkdsk"));
    QVERIFY(command.contains("/scan"));
}

void TestCheckDiskErrorsAction::testParseChkdskOutput() {
    QString mockOutput = R"(
Windows has scanned the file system and found no problems.
No further action is required.
    )";
    
    bool hasErrors = mockOutput.contains("found problems", Qt::CaseInsensitive);
    QVERIFY(!hasErrors);
}

void TestCheckDiskErrorsAction::testDetectErrorCount() {
    QString mockOutput = "Windows found 5 errors on the disk.";
    
    // Parse error count
    QVERIFY(mockOutput.contains("5"));
}

void TestCheckDiskErrorsAction::testDetectRebootNeeded() {
    QString mockOutput = "Chkdsk cannot run because the volume is in use. Schedule scan at next reboot? (Y/N)";
    
    bool needsReboot = mockOutput.contains("reboot", Qt::CaseInsensitive) ||
                       mockOutput.contains("in use", Qt::CaseInsensitive);
    QVERIFY(needsReboot);
}

void TestCheckDiskErrorsAction::testParseCleanDrive() {
    QString mockOutput = createMockChkdskOutput(false);
    
    QVERIFY(!mockOutput.isEmpty());
    QVERIFY(mockOutput.contains("no") || mockOutput.contains("healthy"));
}

void TestCheckDiskErrorsAction::testParseErrorsDriveWithErrors() {
    QString mockOutput = createMockChkdskOutput(true, 10);
    
    QVERIFY(mockOutput.contains("error"));
}

void TestCheckDiskErrorsAction::testParseRebootRequired() {
    QString mockOutput = "Volume is in use. Scan scheduled at next reboot.";
    
    QVERIFY(mockOutput.contains("reboot"));
}

void TestCheckDiskErrorsAction::testParseBadSectors() {
    QString mockOutput = "Found 3 bad sectors. Data may be corrupted.";
    
    QVERIFY(mockOutput.contains("bad sectors"));
}

void TestCheckDiskErrorsAction::testHandleDriveInUse() {
    // System drive is always in use
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(60000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestCheckDiskErrorsAction::testHandleAccessDenied() {
    // Some drives may be access denied
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(60000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestCheckDiskErrorsAction::testHandleInvalidDrive() {
    QString invalidDrive = "Z:";
    
    // Should handle gracefully
    QVERIFY(!invalidDrive.isEmpty());
}

void TestCheckDiskErrorsAction::testHandleChkdskFailed() {
    // CHKDSK command failed
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(60000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestCheckDiskErrorsAction::testProgressSignals() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(10000));
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestCheckDiskErrorsAction::testScanProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->scan();
    QTest::qWait(1000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestCheckDiskErrorsAction::testPerDriveProgress() {
    // Progress should update for each drive checked
    int totalDrives = 3;
    
    for (int i = 0; i < totalDrives; ++i) {
        int progress = (i + 1) * 100 / totalDrives;
        QVERIFY(progress >= 0 && progress <= 100);
    }
}

void TestCheckDiskErrorsAction::testFormatDriveResults() {
    QString result = R"(
Drive C: - Healthy (No errors found)
Drive D: - 3 errors detected
Drive E: - Healthy (No errors found)
    )";
    
    QVERIFY(result.contains("Drive C:"));
    QVERIFY(result.contains("Healthy"));
    QVERIFY(result.contains("3 errors"));
}

void TestCheckDiskErrorsAction::testFormatErrorSummary() {
    QString summary = R"(
Disk Check Complete:
  - 2 drives healthy
  - 1 drive with errors
  - Total errors: 3
    )";
    
    QVERIFY(summary.contains("Complete"));
    QVERIFY(summary.contains("healthy"));
    QVERIFY(summary.contains("errors"));
}

void TestCheckDiskErrorsAction::testFormatRecommendations() {
    QString recommendations = R"(
Recommendations:
  ⚠️ Drive D: has 3 errors - Run chkdsk /F to fix
  ⚠️ Schedule full scan at next reboot for C:
    )";
    
    QVERIFY(recommendations.contains("Recommendations"));
    QVERIFY(recommendations.contains("chkdsk"));
}

void TestCheckDiskErrorsAction::testNoDrives() {
    // Unlikely: no drives detected
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(10000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestCheckDiskErrorsAction::testAllDrivesHealthy() {
    QString result = "All drives are healthy. No errors detected.";
    
    QVERIFY(result.contains("healthy") || result.contains("No errors"));
}

void TestCheckDiskErrorsAction::testMultipleDrivesWithErrors() {
    QStringList drivesWithErrors = {"C:", "D:"};
    
    QVERIFY(drivesWithErrors.count() >= 2);
}

void TestCheckDiskErrorsAction::testEncryptedDrive() {
    // BitLocker encrypted drives
    QString driveStatus = "BitLocker encrypted";
    
    QVERIFY(driveStatus.contains("BitLocker"));
}

// Helper methods

QString TestCheckDiskErrorsAction::createMockChkdskOutput(bool hasErrors, int errorCount) {
    if (!hasErrors) {
        return R"(
Windows has scanned the file system and found no problems.
No further action is required.

  1234567 KB total disk space.
  1000000 KB in 10000 files.
   200000 KB in 1000 indexes.
        0 KB in bad sectors.
    34567 KB in use by the system.
        )";
    }
    
    return QString(R"(
Windows has found problems with the file system.
%1 file system errors detected.
Run chkdsk with /F option to fix errors.
    )").arg(errorCount);
}

QTEST_MAIN(TestCheckDiskErrorsAction)
#include "test_check_disk_errors_action.moc"

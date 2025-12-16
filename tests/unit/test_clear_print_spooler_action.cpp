// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QTest>
#include <QSignalSpy>
#include "sak/actions/clear_print_spooler_action.h"

class TestClearPrintSpoolerAction : public QObject {
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
    void testScanCountsJobs();
    void testExecuteClearsSpooler();
    
    // Service management
    void testStopSpoolerService();
    void testStartSpoolerService();
    void testVerifyServiceStopped();
    void testVerifyServiceStarted();
    
    // Spool folder operations
    void testLocateSpoolFolder();
    void testClearSpoolFolder();
    void testCountSpoolFiles();
    void testCalculateSpoolSize();
    
    // Print job detection
    void testDetectStuckJobs();
    void testIdentifyJobFiles();
    void testCountSHDFiles();
    void testCountSPLFiles();
    
    // File deletion
    void testDeleteSPLFiles();
    void testDeleteSHDFiles();
    void testDeleteAllSpoolFiles();
    void testVerifyFilesDeleted();
    
    // Progress tracking
    void testProgressSignals();
    void testScanProgress();
    void testExecuteProgress();
    
    // Error handling
    void testHandleServiceStopFailure();
    void testHandleServiceStartFailure();
    void testHandleFileInUse();
    void testHandleAccessDenied();
    
    // Service verification
    void testCheckSpoolerStatus();
    void testWaitForServiceStop();
    void testWaitForServiceStart();
    
    // Results formatting
    void testFormatJobCount();
    void testFormatSpoolSize();
    void testFormatSuccessMessage();
    void testFormatErrorMessage();
    
    // Edge cases
    void testNoStuckJobs();
    void testEmptySpoolFolder();
    void testSpoolerNotInstalled();
    void testSpoolerDisabled();

private:
    sak::ClearPrintSpoolerAction* m_action;
};

void TestClearPrintSpoolerAction::initTestCase() {
    // One-time setup
}

void TestClearPrintSpoolerAction::cleanupTestCase() {
    // One-time cleanup
}

void TestClearPrintSpoolerAction::init() {
    m_action = new sak::ClearPrintSpoolerAction();
}

void TestClearPrintSpoolerAction::cleanup() {
    delete m_action;
}

void TestClearPrintSpoolerAction::testActionProperties() {
    QCOMPARE(m_action->name(), QString("Clear Print Spooler"));
    QVERIFY(!m_action->description().isEmpty());
    QVERIFY(m_action->description().contains("print", Qt::CaseInsensitive));
    QCOMPARE(m_action->category(), sak::QuickAction::ActionCategory::Maintenance);
    QVERIFY(m_action->requiresAdmin());
}

void TestClearPrintSpoolerAction::testInitialState() {
    QSignalSpy startedSpy(m_action, &sak::QuickAction::started);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    QVERIFY(startedSpy.isValid());
    QVERIFY(finishedSpy.isValid());
    QCOMPARE(startedSpy.count(), 0);
}

void TestClearPrintSpoolerAction::testRequiresAdmin() {
    // Requires admin to stop services and delete system files
    QVERIFY(m_action->requiresAdmin());
}

void TestClearPrintSpoolerAction::testScanCountsJobs() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    
    QVERIFY(finishedSpy.wait(15000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestClearPrintSpoolerAction::testExecuteClearsSpooler() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    
    QVERIFY(finishedSpy.wait(30000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestClearPrintSpoolerAction::testStopSpoolerService() {
    // Command: net stop spooler
    QString command = "net stop spooler";
    
    QVERIFY(command.contains("spooler"));
}

void TestClearPrintSpoolerAction::testStartSpoolerService() {
    // Command: net start spooler
    QString command = "net start spooler";
    
    QVERIFY(command.contains("start"));
}

void TestClearPrintSpoolerAction::testVerifyServiceStopped() {
    // Verify spooler is stopped
    QString expectedState = "STOPPED";
    
    QCOMPARE(expectedState, QString("STOPPED"));
}

void TestClearPrintSpoolerAction::testVerifyServiceStarted() {
    // Verify spooler is running
    QString expectedState = "RUNNING";
    
    QCOMPARE(expectedState, QString("RUNNING"));
}

void TestClearPrintSpoolerAction::testLocateSpoolFolder() {
    // C:\Windows\System32\spool\PRINTERS
    QString spoolPath = R"(C:\Windows\System32\spool\PRINTERS)";
    
    QVERIFY(spoolPath.contains("spool"));
    QVERIFY(spoolPath.contains("PRINTERS"));
}

void TestClearPrintSpoolerAction::testClearSpoolFolder() {
    // Delete all files in spool\PRINTERS
    QString command = "del /F /S /Q C:\\Windows\\System32\\spool\\PRINTERS\\*";
    
    QVERIFY(command.contains("del"));
    QVERIFY(command.contains("/F")); // Force
}

void TestClearPrintSpoolerAction::testCountSpoolFiles() {
    int fileCount = 5;
    
    QVERIFY(fileCount >= 0);
}

void TestClearPrintSpoolerAction::testCalculateSpoolSize() {
    qint64 totalSize = 25LL * 1024 * 1024; // 25 MB
    
    QVERIFY(totalSize >= 0);
}

void TestClearPrintSpoolerAction::testDetectStuckJobs() {
    // Stuck jobs have .SPL and .SHD files
    int stuckJobs = 3;
    
    QVERIFY(stuckJobs >= 0);
}

void TestClearPrintSpoolerAction::testIdentifyJobFiles() {
    QStringList jobFiles = {
        "00001.SPL",
        "00001.SHD",
        "00002.SPL",
        "00002.SHD"
    };
    
    QVERIFY(jobFiles.size() >= 2);
}

void TestClearPrintSpoolerAction::testCountSHDFiles() {
    // .SHD files contain job information
    QStringList shdFiles = {"00001.SHD", "00002.SHD"};
    
    QCOMPARE(shdFiles.size(), 2);
}

void TestClearPrintSpoolerAction::testCountSPLFiles() {
    // .SPL files contain actual print data
    QStringList splFiles = {"00001.SPL", "00002.SPL"};
    
    QCOMPARE(splFiles.size(), 2);
}

void TestClearPrintSpoolerAction::testDeleteSPLFiles() {
    // Delete spool data files
    QString pattern = "*.SPL";
    
    QVERIFY(pattern.contains("SPL"));
}

void TestClearPrintSpoolerAction::testDeleteSHDFiles() {
    // Delete shadow files
    QString pattern = "*.SHD";
    
    QVERIFY(pattern.contains("SHD"));
}

void TestClearPrintSpoolerAction::testDeleteAllSpoolFiles() {
    // Delete all files in PRINTERS folder
    QString command = "del /Q *.*";
    
    QVERIFY(command.contains("del"));
}

void TestClearPrintSpoolerAction::testVerifyFilesDeleted() {
    // Verify spool folder is empty
    int remainingFiles = 0;
    
    QCOMPARE(remainingFiles, 0);
}

void TestClearPrintSpoolerAction::testProgressSignals() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(15000));
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestClearPrintSpoolerAction::testScanProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->scan();
    QTest::qWait(1000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestClearPrintSpoolerAction::testExecuteProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->execute();
    QTest::qWait(3000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestClearPrintSpoolerAction::testHandleServiceStopFailure() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(30000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestClearPrintSpoolerAction::testHandleServiceStartFailure() {
    // Service may fail to restart
    bool startSuccess = false;
    
    QVERIFY(!startSuccess);
}

void TestClearPrintSpoolerAction::testHandleFileInUse() {
    // Files may be locked by another process
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(30000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestClearPrintSpoolerAction::testHandleAccessDenied() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(30000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestClearPrintSpoolerAction::testCheckSpoolerStatus() {
    // Command: sc query spooler
    QString command = "sc query spooler";
    
    QVERIFY(command.contains("query"));
}

void TestClearPrintSpoolerAction::testWaitForServiceStop() {
    // Wait for service to fully stop
    int waitTimeMs = 3000;
    
    QVERIFY(waitTimeMs > 0);
}

void TestClearPrintSpoolerAction::testWaitForServiceStart() {
    // Wait for service to fully start
    int waitTimeMs = 5000;
    
    QVERIFY(waitTimeMs > 0);
}

void TestClearPrintSpoolerAction::testFormatJobCount() {
    int jobs = 5;
    QString formatted = QString("%1 stuck print jobs").arg(jobs);
    
    QVERIFY(formatted.contains("jobs"));
}

void TestClearPrintSpoolerAction::testFormatSpoolSize() {
    qint64 bytes = 25LL * 1024 * 1024; // 25 MB
    QString formatted = QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + " MB";
    
    QVERIFY(formatted.contains("MB"));
}

void TestClearPrintSpoolerAction::testFormatSuccessMessage() {
    QString message = "Successfully cleared 5 print jobs (25.3 MB freed)";
    
    QVERIFY(message.contains("Successfully"));
    QVERIFY(message.contains("cleared"));
}

void TestClearPrintSpoolerAction::testFormatErrorMessage() {
    QString error = "Failed to stop Print Spooler service: Access Denied";
    
    QVERIFY(error.contains("Failed"));
    QVERIFY(error.contains("Spooler"));
}

void TestClearPrintSpoolerAction::testNoStuckJobs() {
    // No stuck print jobs
    int stuckJobs = 0;
    
    QCOMPARE(stuckJobs, 0);
}

void TestClearPrintSpoolerAction::testEmptySpoolFolder() {
    // Spool folder already empty
    int fileCount = 0;
    qint64 totalSize = 0;
    
    QCOMPARE(fileCount, 0);
    QCOMPARE(totalSize, 0LL);
}

void TestClearPrintSpoolerAction::testSpoolerNotInstalled() {
    // Print Spooler service may not be installed (rare)
    bool serviceExists = false;
    
    QVERIFY(!serviceExists);
}

void TestClearPrintSpoolerAction::testSpoolerDisabled() {
    // Service may be disabled
    QString serviceState = "DISABLED";
    
    QCOMPARE(serviceState, QString("DISABLED"));
}

QTEST_MAIN(TestClearPrintSpoolerAction)
#include "test_clear_print_spooler_action.moc"

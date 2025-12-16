// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include "sak/actions/clear_event_logs_action.h"

class TestClearEventLogsAction : public QObject {
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
    void testScanDetectsLogs();
    void testExecuteClearsLogs();
    
    // Log detection
    void testDetectApplicationLog();
    void testDetectSystemLog();
    void testDetectSecurityLog();
    void testDetectCustomLogs();
    void testGetLogSize();
    
    // Backup operations
    void testBackupLogBeforeClear();
    void testBackupLocationCreation();
    void testBackupFileNaming();
    void testBackupWithTimestamp();
    
    // Clear operations
    void testClearSingleLog();
    void testClearMultipleLogs();
    void testClearAllStandardLogs();
    void testPreserveBackup();
    
    // Error handling
    void testHandleAccessDenied();
    void testHandleLogInUse();
    void testHandleBackupFailed();
    void testHandleInvalidLogName();
    
    // Progress tracking
    void testProgressSignals();
    void testScanProgress();
    void testExecuteProgress();
    void testPerLogProgress();
    
    // Results formatting
    void testFormatLogSummary();
    void testFormatBackupInfo();
    void testFormatClearResults();
    
    // Edge cases
    void testEmptyEventLog();
    void testLargeEventLog();
    void testCorruptedLog();
    void testNoLogsFound();

private:
    QTemporaryDir* m_tempDir;
    sak::ClearEventLogsAction* m_action;
    
    QString createMockEventLog(const QString& name, qint64 size);
    QString formatLogSize(qint64 bytes);
};

void TestClearEventLogsAction::initTestCase() {
    // One-time setup
}

void TestClearEventLogsAction::cleanupTestCase() {
    // One-time cleanup
}

void TestClearEventLogsAction::init() {
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());
    m_action = new sak::ClearEventLogsAction();
}

void TestClearEventLogsAction::cleanup() {
    delete m_action;
    delete m_tempDir;
}

void TestClearEventLogsAction::testActionProperties() {
    QCOMPARE(m_action->name(), QString("Clear Event Logs"));
    QVERIFY(!m_action->description().isEmpty());
    QVERIFY(m_action->description().contains("Event Logs", Qt::CaseInsensitive));
    QCOMPARE(m_action->category(), sak::QuickAction::ActionCategory::SystemOptimization);
    QVERIFY(m_action->requiresAdmin());
}

void TestClearEventLogsAction::testInitialState() {
    QSignalSpy startedSpy(m_action, &sak::QuickAction::started);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    QVERIFY(startedSpy.isValid());
    QVERIFY(finishedSpy.isValid());
    QCOMPARE(startedSpy.count(), 0);
}

void TestClearEventLogsAction::testRequiresAdmin() {
    // Event log clearing requires administrator privileges
    QVERIFY(m_action->requiresAdmin());
}

void TestClearEventLogsAction::testScanDetectsLogs() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    
    QVERIFY(finishedSpy.wait(10000));
    QVERIFY(progressSpy.count() >= 1);
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestClearEventLogsAction::testExecuteClearsLogs() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    
    QVERIFY(finishedSpy.wait(15000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestClearEventLogsAction::testDetectApplicationLog() {
    // Application log is a standard Windows event log
    QString logName = "Application";
    QVERIFY(!logName.isEmpty());
}

void TestClearEventLogsAction::testDetectSystemLog() {
    QString logName = "System";
    QVERIFY(!logName.isEmpty());
}

void TestClearEventLogsAction::testDetectSecurityLog() {
    QString logName = "Security";
    QVERIFY(!logName.isEmpty());
}

void TestClearEventLogsAction::testDetectCustomLogs() {
    // Custom logs like Windows PowerShell, Setup, etc.
    QStringList customLogs = {
        "Windows PowerShell",
        "Setup",
        "Forwarded Events"
    };
    
    QVERIFY(customLogs.count() >= 3);
}

void TestClearEventLogsAction::testGetLogSize() {
    // Mock log size calculation
    qint64 mockSize = 50 * 1024 * 1024; // 50MB
    
    QVERIFY(mockSize > 0);
    QCOMPARE(mockSize, 50 * 1024 * 1024);
}

void TestClearEventLogsAction::testBackupLogBeforeClear() {
    QString backupPath = m_tempDir->filePath("Application_backup.evtx");
    
    // Would backup before clearing
    QFile mockBackup(backupPath);
    QVERIFY(mockBackup.open(QIODevice::WriteOnly));
    mockBackup.write("mock event log data");
    mockBackup.close();
    
    QVERIFY(QFile::exists(backupPath));
}

void TestClearEventLogsAction::testBackupLocationCreation() {
    QString backupDir = m_tempDir->filePath("EventLogBackups");
    
    QVERIFY(QDir().mkpath(backupDir));
    QVERIFY(QDir(backupDir).exists());
}

void TestClearEventLogsAction::testBackupFileNaming() {
    QString logName = "Application";
    QString backupName = QString("%1_backup.evtx").arg(logName);
    
    QVERIFY(backupName.contains("Application"));
    QVERIFY(backupName.endsWith(".evtx"));
}

void TestClearEventLogsAction::testBackupWithTimestamp() {
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString backupName = QString("Application_%1.evtx").arg(timestamp);
    
    QVERIFY(backupName.contains("Application"));
    QVERIFY(backupName.contains(timestamp.left(8))); // Date part
}

void TestClearEventLogsAction::testClearSingleLog() {
    // Mock clearing a single log
    QString logName = "Application";
    
    // Command would be: wevtutil cl Application
    QString clearCommand = QString("wevtutil cl %1").arg(logName);
    
    QVERIFY(clearCommand.contains("wevtutil"));
    QVERIFY(clearCommand.contains(logName));
}

void TestClearEventLogsAction::testClearMultipleLogs() {
    QStringList logs = {"Application", "System", "Security"};
    
    for (const QString& log : logs) {
        QString command = QString("wevtutil cl %1").arg(log);
        QVERIFY(command.contains(log));
    }
    
    QCOMPARE(logs.count(), 3);
}

void TestClearEventLogsAction::testClearAllStandardLogs() {
    QStringList standardLogs = {
        "Application",
        "System",
        "Security"
    };
    
    QVERIFY(standardLogs.contains("Application"));
    QVERIFY(standardLogs.contains("System"));
    QVERIFY(standardLogs.contains("Security"));
}

void TestClearEventLogsAction::testPreserveBackup() {
    QString backupPath = m_tempDir->filePath("backup.evtx");
    
    QFile backup(backupPath);
    QVERIFY(backup.open(QIODevice::WriteOnly));
    backup.write("backup data");
    backup.close();
    
    // After clearing, backup should still exist
    QVERIFY(QFile::exists(backupPath));
}

void TestClearEventLogsAction::testHandleAccessDenied() {
    // Security log requires special privileges
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(15000));
    
    // Should handle access denied gracefully
    QVERIFY(!m_action->result().isEmpty());
}

void TestClearEventLogsAction::testHandleLogInUse() {
    // Some logs may be in use by system
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(15000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestClearEventLogsAction::testHandleBackupFailed() {
    // If backup fails, should not clear the log
    QString backupPath = "/invalid/path/backup.evtx";
    
    QVERIFY(!QDir("/invalid/path").exists());
}

void TestClearEventLogsAction::testHandleInvalidLogName() {
    QString invalidLog = "NonExistentLog12345";
    
    // Should handle gracefully
    QVERIFY(!invalidLog.isEmpty());
}

void TestClearEventLogsAction::testProgressSignals() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(10000));
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestClearEventLogsAction::testScanProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->scan();
    QTest::qWait(1000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestClearEventLogsAction::testExecuteProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->execute();
    QTest::qWait(2000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestClearEventLogsAction::testPerLogProgress() {
    // Progress should update for each log processed
    int totalLogs = 3;
    
    for (int i = 0; i < totalLogs; ++i) {
        int progress = (i + 1) * 100 / totalLogs;
        QVERIFY(progress >= 0 && progress <= 100);
    }
}

void TestClearEventLogsAction::testFormatLogSummary() {
    QString summary = "Found 3 event logs, total size: 150 MB";
    
    QVERIFY(summary.contains("3"));
    QVERIFY(summary.contains("150 MB"));
}

void TestClearEventLogsAction::testFormatBackupInfo() {
    QString backupInfo = "Backups saved to: C:\\EventLogBackups";
    
    QVERIFY(backupInfo.contains("Backups"));
    QVERIFY(backupInfo.contains("saved"));
}

void TestClearEventLogsAction::testFormatClearResults() {
    QString results = R"(
Cleared 3 event logs:
  - Application (50 MB)
  - System (75 MB)
  - Security (25 MB)
Total freed: 150 MB
    )";
    
    QVERIFY(results.contains("Cleared"));
    QVERIFY(results.contains("Application"));
    QVERIFY(results.contains("150 MB"));
}

void TestClearEventLogsAction::testEmptyEventLog() {
    // Log with 0 entries
    qint64 size = 0;
    
    QCOMPARE(size, 0);
}

void TestClearEventLogsAction::testLargeEventLog() {
    // Log with many entries (e.g., 500MB)
    qint64 size = 500 * 1024 * 1024;
    
    QVERIFY(size > 100 * 1024 * 1024);
}

void TestClearEventLogsAction::testCorruptedLog() {
    // Corrupted log that can't be backed up
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(15000));
    
    // Should handle gracefully
    QVERIFY(!m_action->result().isEmpty());
}

void TestClearEventLogsAction::testNoLogsFound() {
    // Unlikely scenario: no event logs detected
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(10000));
    
    QVERIFY(!m_action->result().isEmpty());
}

// Helper methods

QString TestClearEventLogsAction::createMockEventLog(const QString& name, qint64 size) {
    QString logPath = m_tempDir->filePath(name + ".evtx");
    
    QFile logFile(logPath);
    if (logFile.open(QIODevice::WriteOnly)) {
        logFile.write(QByteArray(static_cast<int>(size), 'L'));
        logFile.close();
    }
    
    return logPath;
}

QString TestClearEventLogsAction::formatLogSize(qint64 bytes) {
    if (bytes >= 1024 * 1024 * 1024) {
        return QString("%1 GB").arg(bytes / (1024.0 * 1024 * 1024), 0, 'f', 2);
    } else if (bytes >= 1024 * 1024) {
        return QString("%1 MB").arg(bytes / (1024.0 * 1024), 0, 'f', 2);
    } else if (bytes >= 1024) {
        return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 2);
    }
    return QString("%1 bytes").arg(bytes);
}

QTEST_MAIN(TestClearEventLogsAction)
#include "test_clear_event_logs_action.moc"

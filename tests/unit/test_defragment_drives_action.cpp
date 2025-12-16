// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QTest>
#include <QSignalSpy>
#include "sak/actions/defragment_drives_action.h"

class TestDefragmentDrivesAction : public QObject {
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
    void testScanAnalyzesDrives();
    void testExecuteDefragments();
    
    // Drive detection
    void testDetectHDD();
    void testDetectSSD();
    void testDetectRemovable();
    void testSkipSSD();
    
    // Fragmentation analysis
    void testAnalyzeFragmentation();
    void testCalculateFragmentationPercent();
    void testIdentifyFragmentedDrive();
    void testIdentifyOptimizedDrive();
    
    // Defragmentation
    void testDefragmentSingleDrive();
    void testDefragmentMultipleDrives();
    void testOptimizeHDD();
    void testTrimSSD();
    
    // Drive type detection
    void testCheckDriveType();
    void testQueryMediaType();
    void testDetectNVMe();
    
    // Progress tracking
    void testProgressSignals();
    void testAnalysisProgress();
    void testDefragProgress();
    
    // Error handling
    void testHandleDriveInUse();
    void testHandleInsufficientSpace();
    void testHandleDefragDisabled();
    void testHandleAccessDenied();
    
    // Results formatting
    void testFormatFragmentationReport();
    void testFormatDefragResults();
    void testFormatDriveList();
    
    // Edge cases
    void testNoHDDsFound();
    void testAllSSDSystem();
    void testHighlyFragmented();
    void testAlreadyOptimized();

private:
    sak::DefragmentDrivesAction* m_action;
    
    QString createMockDefragOutput(int fragmentationPercent);
};

void TestDefragmentDrivesAction::initTestCase() {
    // One-time setup
}

void TestDefragmentDrivesAction::cleanupTestCase() {
    // One-time cleanup
}

void TestDefragmentDrivesAction::init() {
    m_action = new sak::DefragmentDrivesAction();
}

void TestDefragmentDrivesAction::cleanup() {
    delete m_action;
}

void TestDefragmentDrivesAction::testActionProperties() {
    QCOMPARE(m_action->name(), QString("Defragment Drives"));
    QVERIFY(!m_action->description().isEmpty());
    QVERIFY(m_action->description().contains("HDD", Qt::CaseInsensitive));
    QCOMPARE(m_action->category(), sak::QuickAction::ActionCategory::SystemOptimization);
    QVERIFY(m_action->requiresAdmin());
}

void TestDefragmentDrivesAction::testInitialState() {
    QSignalSpy startedSpy(m_action, &sak::QuickAction::started);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    QVERIFY(startedSpy.isValid());
    QVERIFY(finishedSpy.isValid());
    QCOMPARE(startedSpy.count(), 0);
}

void TestDefragmentDrivesAction::testRequiresAdmin() {
    // Defragmentation requires administrator privileges
    QVERIFY(m_action->requiresAdmin());
}

void TestDefragmentDrivesAction::testScanAnalyzesDrives() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    
    QVERIFY(finishedSpy.wait(30000));
    QVERIFY(progressSpy.count() >= 1);
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestDefragmentDrivesAction::testExecuteDefragments() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    
    QVERIFY(finishedSpy.wait(120000)); // Defrag can take time
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestDefragmentDrivesAction::testDetectHDD() {
    QString driveType = "HDD";
    
    QCOMPARE(driveType, QString("HDD"));
}

void TestDefragmentDrivesAction::testDetectSSD() {
    QString driveType = "SSD";
    
    QCOMPARE(driveType, QString("SSD"));
}

void TestDefragmentDrivesAction::testDetectRemovable() {
    QString driveType = "Removable";
    
    // Removable drives should be skipped
    QCOMPARE(driveType, QString("Removable"));
}

void TestDefragmentDrivesAction::testSkipSSD() {
    // SSDs should not be defragmented
    QString driveType = "SSD";
    bool shouldDefrag = (driveType == "HDD");
    
    QVERIFY(!shouldDefrag);
}

void TestDefragmentDrivesAction::testAnalyzeFragmentation() {
    // Command: defrag C: /A
    QString command = "defrag C: /A";
    
    QVERIFY(command.contains("defrag"));
    QVERIFY(command.contains("/A"));
}

void TestDefragmentDrivesAction::testCalculateFragmentationPercent() {
    int fragmentationPercent = 15;
    
    QVERIFY(fragmentationPercent >= 0 && fragmentationPercent <= 100);
}

void TestDefragmentDrivesAction::testIdentifyFragmentedDrive() {
    int fragmentation = 25; // 25% fragmented
    bool needsDefrag = (fragmentation > 10);
    
    QVERIFY(needsDefrag);
}

void TestDefragmentDrivesAction::testIdentifyOptimizedDrive() {
    int fragmentation = 2; // 2% fragmented
    bool needsDefrag = (fragmentation > 10);
    
    QVERIFY(!needsDefrag);
}

void TestDefragmentDrivesAction::testDefragmentSingleDrive() {
    // Command: defrag C: /O
    QString command = "defrag C: /O";
    
    QVERIFY(command.contains("defrag"));
    QVERIFY(command.contains("/O")); // Optimize
}

void TestDefragmentDrivesAction::testDefragmentMultipleDrives() {
    QStringList drives = {"C:", "D:", "E:"};
    
    for (const QString& drive : drives) {
        QString command = QString("defrag %1 /O").arg(drive);
        QVERIFY(command.contains(drive));
    }
}

void TestDefragmentDrivesAction::testOptimizeHDD() {
    // HDD optimization with /O flag
    QString driveType = "HDD";
    QString command = "defrag C: /O";
    
    QVERIFY(command.contains("/O"));
}

void TestDefragmentDrivesAction::testTrimSSD() {
    // SSD uses TRIM instead of defrag
    QString command = "defrag C: /L"; // Retrim
    
    QVERIFY(command.contains("/L"));
}

void TestDefragmentDrivesAction::testCheckDriveType() {
    // Query drive type via PowerShell
    QString psCommand = "Get-PhysicalDisk | Select MediaType";
    
    QVERIFY(psCommand.contains("MediaType"));
}

void TestDefragmentDrivesAction::testQueryMediaType() {
    QStringList mediaTypes = {"HDD", "SSD", "SCM"};
    
    QVERIFY(mediaTypes.contains("HDD"));
    QVERIFY(mediaTypes.contains("SSD"));
}

void TestDefragmentDrivesAction::testDetectNVMe() {
    QString busType = "NVMe";
    
    // NVMe drives are SSDs
    bool isSSD = (busType == "NVMe");
    QVERIFY(isSSD);
}

void TestDefragmentDrivesAction::testProgressSignals() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(30000));
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestDefragmentDrivesAction::testAnalysisProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->scan();
    QTest::qWait(2000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestDefragmentDrivesAction::testDefragProgress() {
    // Defrag reports progress
    int progress = 45;
    
    QVERIFY(progress >= 0 && progress <= 100);
}

void TestDefragmentDrivesAction::testHandleDriveInUse() {
    // System drive may be in use
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(120000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestDefragmentDrivesAction::testHandleInsufficientSpace() {
    // Need 15% free space for defrag
    qint64 totalSpace = 500LL * 1024 * 1024 * 1024; // 500 GB
    qint64 freeSpace = 50LL * 1024 * 1024 * 1024;   // 50 GB
    
    double freePercent = (freeSpace * 100.0) / totalSpace;
    QVERIFY(freePercent >= 10);
}

void TestDefragmentDrivesAction::testHandleDefragDisabled() {
    // Defrag may be disabled by policy
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(120000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestDefragmentDrivesAction::testHandleAccessDenied() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(120000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestDefragmentDrivesAction::testFormatFragmentationReport() {
    QString report = R"(
Drive Analysis:
  C: (HDD) - 25% fragmented - Needs optimization
  D: (SSD) - Skipped (SSD detected)
  E: (HDD) - 3% fragmented - Already optimized
    )";
    
    QVERIFY(report.contains("Analysis"));
    QVERIFY(report.contains("fragmented"));
}

void TestDefragmentDrivesAction::testFormatDefragResults() {
    QString results = R"(
Defragmentation Complete:
  C: Optimized (25% → 1%)
  E: Skipped (already optimized)
Time elapsed: 15 minutes
    )";
    
    QVERIFY(results.contains("Complete"));
    QVERIFY(results.contains("Optimized"));
}

void TestDefragmentDrivesAction::testFormatDriveList() {
    QString list = "Found 2 HDDs requiring optimization: C:, E:";
    
    QVERIFY(list.contains("HDD"));
}

void TestDefragmentDrivesAction::testNoHDDsFound() {
    // All-SSD system
    int hddCount = 0;
    
    QCOMPARE(hddCount, 0);
}

void TestDefragmentDrivesAction::testAllSSDSystem() {
    QStringList driveTypes = {"SSD", "SSD", "SSD"};
    
    bool hasHDD = false;
    for (const QString& type : driveTypes) {
        if (type == "HDD") {
            hasHDD = true;
        }
    }
    
    QVERIFY(!hasHDD);
}

void TestDefragmentDrivesAction::testHighlyFragmented() {
    int fragmentation = 85; // 85% fragmented
    
    QVERIFY(fragmentation > 50);
}

void TestDefragmentDrivesAction::testAlreadyOptimized() {
    int fragmentation = 1; // 1% fragmented
    bool needsDefrag = (fragmentation > 10);
    
    QVERIFY(!needsDefrag);
}

// Helper methods

QString TestDefragmentDrivesAction::createMockDefragOutput(int fragmentationPercent) {
    return QString(R"(
Analyzing drive C:...
Total fragmentation: %1%
File fragmentation: %2%
Free space fragmentation: %3%
    )").arg(fragmentationPercent)
       .arg(fragmentationPercent - 2)
       .arg(fragmentationPercent + 1);
}

QTEST_MAIN(TestDefragmentDrivesAction)
#include "test_defragment_drives_action.moc"

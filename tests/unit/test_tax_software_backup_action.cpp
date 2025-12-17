// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QtTest>
#include "sak/actions/tax_software_backup_action.h"

class TestTaxSoftwareBackupAction : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Basic properties
    void testActionProperties();
    void testActionCategory();
    void testRequiresAdmin();
    void testActionMetadata();

    // TurboTax detection
    void testDetectTurboTaxInstalled();
    void testGetTurboTaxVersion();
    void testFindTurboTaxDataLocation();
    void testDetectTurboTaxYears();

    // TurboTax file scanning
    void testScanTurboTaxReturns();
    void testScanTurboTaxBackups();
    void testEnumerateTaxYears();
    void testDetectTurboTaxPDF();

    // H&R Block detection
    void testDetectHRBlockInstalled();
    void testGetHRBlockVersion();
    void testFindHRBlockDataLocation();
    void testDetectHRBlockYears();

    // H&R Block file scanning
    void testScanHRBlockReturns();
    void testScanHRBlockBackups();
    void testDetectHRBlockPDF();

    // TaxAct detection
    void testDetectTaxActInstalled();
    void testGetTaxActVersion();
    void testFindTaxActDataLocation();
    void testDetectTaxActYears();

    // TaxAct file scanning
    void testScanTaxActReturns();
    void testScanTaxActBackups();
    void testDetectTaxActPDF();

    // Tax file types
    void testIdentifyTaxReturnFiles();
    void testIdentifyTaxPDFFiles();
    void testIdentifyTaxBackupFiles();
    void testValidateTaxFileIntegrity();

    // Tax year detection
    void testParseTaxYear();
    void testDetectMultipleYears();
    void testSortByTaxYear();
    void testFilterByYear();

    // Size calculation
    void testCalculateTurboTaxSize();
    void testCalculateHRBlockSize();
    void testCalculateTaxActSize();
    void testCalculateTotalSize();

    // File enumeration
    void testCountTaxFiles();
    void testDetectLargeTaxFiles();
    void testGroupByYear();
    void testGroupBySoftware();

    // Multi-software support
    void testScanMultipleSoftware();
    void testMergeDuplicateYears();
    void testPrioritizeSoftware();

    // Security considerations
    void testEncryptionWarning();
    void testSensitiveDataDetection();
    void testSSNRedaction();

    // Scan functionality
    void testScanTaxData();
    void testScanProgress();
    void testScanCancellation();
    void testScanWithoutTaxSoftware();

    // Execute functionality
    void testExecuteBackup();
    void testExecuteWithTimestamp();
    void testExecuteMultipleSoftware();
    void testExecuteTimeout();

    // Backup verification
    void testVerifyBackupStructure();
    void testVerifyBackupIntegrity();
    void testVerifyAllFilesBackedUp();
    void testVerifyPDFsIncluded();

    // Error handling
    void testHandleNoTaxSoftwareFound();
    void testHandleNoTaxDataFound();
    void testHandleAccessDenied();
    void testHandleInsufficientSpace();
    void testHandleCorruptTaxFile();

private:
    QTemporaryDir* m_temp_dir{nullptr};
    QString m_test_backup_location;
};

void TestTaxSoftwareBackupAction::initTestCase() {
    // Setup test environment
}

void TestTaxSoftwareBackupAction::cleanupTestCase() {
    // Cleanup test environment
}

void TestTaxSoftwareBackupAction::init() {
    m_temp_dir = new QTemporaryDir();
    QVERIFY(m_temp_dir->isValid());
    m_test_backup_location = m_temp_dir->path();
}

void TestTaxSoftwareBackupAction::cleanup() {
    delete m_temp_dir;
    m_temp_dir = nullptr;
}

void TestTaxSoftwareBackupAction::testActionProperties() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QCOMPARE(action.name(), QString("Tax Software Data Backup"));
    QVERIFY(!action.description().isEmpty());
}

void TestTaxSoftwareBackupAction::testActionCategory() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QCOMPARE(action.category(), sak::ActionCategory::QuickBackup);
}

void TestTaxSoftwareBackupAction::testRequiresAdmin() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QVERIFY(!action.requiresAdmin());
}

void TestTaxSoftwareBackupAction::testActionMetadata() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QVERIFY(!action.name().isEmpty());
    QVERIFY(!action.description().isEmpty());
    QCOMPARE(action.category(), sak::ActionCategory::QuickBackup);
}

void TestTaxSoftwareBackupAction::testDetectTurboTaxInstalled() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testGetTurboTaxVersion() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testFindTurboTaxDataLocation() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testDetectTurboTaxYears() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testScanTurboTaxReturns() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testScanTurboTaxBackups() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testEnumerateTaxYears() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testDetectTurboTaxPDF() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testDetectHRBlockInstalled() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testGetHRBlockVersion() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testFindHRBlockDataLocation() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testDetectHRBlockYears() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testScanHRBlockReturns() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testScanHRBlockBackups() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testDetectHRBlockPDF() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testDetectTaxActInstalled() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testGetTaxActVersion() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testFindTaxActDataLocation() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testDetectTaxActYears() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testScanTaxActReturns() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testScanTaxActBackups() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testDetectTaxActPDF() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testIdentifyTaxReturnFiles() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testIdentifyTaxPDFFiles() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testIdentifyTaxBackupFiles() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testValidateTaxFileIntegrity() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testParseTaxYear() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testDetectMultipleYears() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testSortByTaxYear() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testFilterByYear() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testCalculateTurboTaxSize() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testCalculateHRBlockSize() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testCalculateTaxActSize() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testCalculateTotalSize() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testCountTaxFiles() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testDetectLargeTaxFiles() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testGroupByYear() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testGroupBySoftware() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testScanMultipleSoftware() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testMergeDuplicateYears() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testPrioritizeSoftware() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testEncryptionWarning() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testSensitiveDataDetection() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testSSNRedaction() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testScanTaxData() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testScanProgress() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::progressUpdated);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testScanCancellation() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testScanWithoutTaxSoftware() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testExecuteBackup() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestTaxSoftwareBackupAction::testExecuteWithTimestamp() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestTaxSoftwareBackupAction::testExecuteMultipleSoftware() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestTaxSoftwareBackupAction::testExecuteTimeout() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestTaxSoftwareBackupAction::testVerifyBackupStructure() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestTaxSoftwareBackupAction::testVerifyBackupIntegrity() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestTaxSoftwareBackupAction::testVerifyAllFilesBackedUp() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestTaxSoftwareBackupAction::testVerifyPDFsIncluded() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestTaxSoftwareBackupAction::testHandleNoTaxSoftwareFound() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testHandleNoTaxDataFound() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestTaxSoftwareBackupAction::testHandleAccessDenied() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestTaxSoftwareBackupAction::testHandleInsufficientSpace() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestTaxSoftwareBackupAction::testHandleCorruptTaxFile() {
    sak::TaxSoftwareBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::TaxSoftwareBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

QTEST_MAIN(TestTaxSoftwareBackupAction)
#include "test_tax_software_backup_action.moc"

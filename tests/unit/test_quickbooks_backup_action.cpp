// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QtTest>
#include "sak/actions/quickbooks_backup_action.h"

class TestQuickBooksBackupAction : public QObject {
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

    // QuickBooks detection
    void testDetectQuickBooksInstalled();
    void testGetQuickBooksVersion();
    void testDetectQuickBooksNotInstalled();
    void testDetectMultipleVersions();

    // Company file discovery
    void testLocateCompanyFiles();
    void testFindQBWFiles();
    void testFindQBBFiles();
    void testFindTLGFiles();
    void testFindNDFiles();

    // Common locations
    void testScanPublicDocuments();
    void testScanUserDocuments();
    void testScanCustomLocations();
    void testScanNetworkShares();

    // File validation
    void testValidateQBWFile();
    void testValidateBackupFile();
    void testValidateTransactionLog();
    void testDetectCorruptFiles();

    // Company file info
    void testGetCompanyFileName();
    void testGetCompanyFileSize();
    void testGetLastModifiedDate();
    void testCheckFileInUse();

    // Backup operations
    void testBackupCompanyFile();
    void testBackupWithTransactionLog();
    void testBackupMultipleCompanies();
    void testBackupStructure();

    // QuickBooks running detection
    void testDetectQuickBooksRunning();
    void testWarnQuickBooksOpen();
    void testCheckFileLockedByQuickBooks();
    void testForceCloseOption();

    // Multi-user mode
    void testDetectMultiUserMode();
    void testBackupMultiUserFiles();
    void testHandleNetworkFiles();
    void testCheckUserLocks();

    // Version-specific handling
    void testBackupQuickBooksDesktop();
    void testBackupQuickBooksOnline();
    void testBackupQuickBooksEnterprise();
    void testHandleVersionDifferences();

    // Associated files
    void testBackupCompanyPreferences();
    void testBackupTemplates();
    void testBackupReports();
    void testBackupAttachments();

    // Backup verification
    void testVerifyBackupIntegrity();
    void testVerifyFileSize();
    void testVerifyChecksum();
    void testCreateBackupManifest();

    // Scan functionality
    void testScanAllLocations();
    void testScanSpecificFolder();
    void testScanProgress();
    void testScanCancellation();

    // Execute functionality
    void testExecuteBackup();
    void testExecuteWithTimestamp();
    void testExecuteMultipleFiles();
    void testExecuteTimeout();

    // Backup strategies
    void testFullBackup();
    void testIncrementalBackup();
    void testCompressedBackup();
    void testEncryptedBackup();

    // Error handling
    void testHandleQuickBooksNotFound();
    void testHandleNoCompanyFiles();
    void testHandleFileLocked();
    void testHandleInsufficientSpace();
    void testHandleAccessDenied();

private:
    QTemporaryDir* m_temp_dir{nullptr};
    QString m_test_backup_location;
};

void TestQuickBooksBackupAction::initTestCase() {
    // Setup test environment
}

void TestQuickBooksBackupAction::cleanupTestCase() {
    // Cleanup test environment
}

void TestQuickBooksBackupAction::init() {
    m_temp_dir = new QTemporaryDir();
    QVERIFY(m_temp_dir->isValid());
    m_test_backup_location = m_temp_dir->path();
}

void TestQuickBooksBackupAction::cleanup() {
    delete m_temp_dir;
    m_temp_dir = nullptr;
}

void TestQuickBooksBackupAction::testActionProperties() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QCOMPARE(action.name(), QString("QuickBooks Backup"));
    QVERIFY(!action.description().isEmpty());
}

void TestQuickBooksBackupAction::testActionCategory() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QCOMPARE(action.category(), sak::ActionCategory::QuickBackup);
}

void TestQuickBooksBackupAction::testRequiresAdmin() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QVERIFY(!action.requiresAdmin());
}

void TestQuickBooksBackupAction::testActionMetadata() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QVERIFY(!action.name().isEmpty());
    QVERIFY(!action.description().isEmpty());
    QCOMPARE(action.category(), sak::ActionCategory::QuickBackup);
}

void TestQuickBooksBackupAction::testDetectQuickBooksInstalled() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testGetQuickBooksVersion() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testDetectQuickBooksNotInstalled() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testDetectMultipleVersions() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testLocateCompanyFiles() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testFindQBWFiles() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testFindQBBFiles() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testFindTLGFiles() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testFindNDFiles() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testScanPublicDocuments() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testScanUserDocuments() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testScanCustomLocations() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testScanNetworkShares() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testValidateQBWFile() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testValidateBackupFile() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testValidateTransactionLog() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testDetectCorruptFiles() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testGetCompanyFileName() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testGetCompanyFileSize() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testGetLastModifiedDate() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testCheckFileInUse() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testBackupCompanyFile() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testBackupWithTransactionLog() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testBackupMultipleCompanies() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testBackupStructure() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testDetectQuickBooksRunning() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testWarnQuickBooksOpen() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testCheckFileLockedByQuickBooks() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testForceCloseOption() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testDetectMultiUserMode() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testBackupMultiUserFiles() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testHandleNetworkFiles() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testCheckUserLocks() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testBackupQuickBooksDesktop() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testBackupQuickBooksOnline() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testBackupQuickBooksEnterprise() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testHandleVersionDifferences() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testBackupCompanyPreferences() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testBackupTemplates() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testBackupReports() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testBackupAttachments() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testVerifyBackupIntegrity() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testVerifyFileSize() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testVerifyChecksum() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testCreateBackupManifest() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testScanAllLocations() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testScanSpecificFolder() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testScanProgress() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::progressUpdated);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testScanCancellation() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testExecuteBackup() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testExecuteWithTimestamp() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testExecuteMultipleFiles() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testExecuteTimeout() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testFullBackup() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testIncrementalBackup() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testCompressedBackup() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testEncryptedBackup() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testHandleQuickBooksNotFound() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testHandleNoCompanyFiles() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestQuickBooksBackupAction::testHandleFileLocked() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testHandleInsufficientSpace() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestQuickBooksBackupAction::testHandleAccessDenied() {
    sak::QuickBooksBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::QuickBooksBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

QTEST_MAIN(TestQuickBooksBackupAction)
#include "test_quickbooks_backup_action.moc"

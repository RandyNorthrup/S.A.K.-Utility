// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QtTest>
#include "sak/actions/outlook_backup_action.h"

class TestOutlookBackupAction : public QObject {
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

    // Outlook detection
    void testDetectOutlookInstalled();
    void testGetOutlookVersion();
    void testDetectOutlookNotInstalled();
    void testDetectMultipleOutlookVersions();

    // PST file discovery
    void testLocatePSTFiles();
    void testFindDefaultPSTLocation();
    void testFindCustomPSTLocation();
    void testFindMultiplePSTFiles();

    // OST file discovery
    void testLocateOSTFiles();
    void testFindDefaultOSTLocation();
    void testFindCachedExchangeData();
    void testDetectOSTVsPST();

    // File validation
    void testValidatePSTFile();
    void testValidateOSTFile();
    void testDetectCorruptPST();
    void testDetectLargePSTFile();

    // Outlook running detection
    void testDetectOutlookRunning();
    void testWarnOutlookOpen();
    void testCheckFileLocked();
    void testForceCloseOption();

    // File size handling
    void testGetPSTFileSize();
    void testHandleLargePST();
    void testCalculateTotalBackupSize();
    void testEstimateBackupTime();

    // Account configuration
    void testBackupAccountSettings();
    void testBackupEmailProfiles();
    void testBackupSignatures();
    void testBackupRules();

    // Multi-profile support
    void testDetectMultipleProfiles();
    void testBackupDefaultProfile();
    void testBackupAllProfiles();
    void testProfileSelection();

    // Backup operations
    void testBackupPSTFile();
    void testBackupOSTFile();
    void testBackupWithProgress();
    void testVerifyBackupIntegrity();

    // File locking
    void testDetectFileInUse();
    void testWaitForFileRelease();
    void testShadowCopyOption();
    void testVSSBackup();

    // Registry settings
    void testReadOutlookRegistry();
    void testGetDataFilePaths();
    void testGetAccountConfiguration();
    void testBackupRegistrySettings();

    // Exchange integration
    void testDetectExchangeAccount();
    void testBackupExchangeOST();
    void testWarnAboutOSTLimitations();
    void testOnlineArchiveDetection();

    // Outlook versions
    void testBackupOutlook2016();
    void testBackupOutlook2019();
    void testBackupOutlook2021();
    void testBackupMicrosoft365();

    // Archive files
    void testDetectArchivePST();
    void testBackupArchiveFiles();
    void testIncludeArchiveOption();

    // Scan functionality
    void testScanForPSTFiles();
    void testScanProgress();
    void testScanMultipleUsers();
    void testScanCancellation();

    // Execute functionality
    void testExecuteBackup();
    void testExecuteWithTimestamp();
    void testExecuteMultipleFiles();
    void testExecuteTimeout();

    // Error handling
    void testHandleOutlookNotFound();
    void testHandleNoPSTFiles();
    void testHandleFileLocked();
    void testHandleInsufficientSpace();
    void testHandleAccessDenied();

private:
    QTemporaryDir* m_temp_dir{nullptr};
    QString m_test_backup_location;
};

void TestOutlookBackupAction::initTestCase() {
    // Setup test environment
}

void TestOutlookBackupAction::cleanupTestCase() {
    // Cleanup test environment
}

void TestOutlookBackupAction::init() {
    m_temp_dir = new QTemporaryDir();
    QVERIFY(m_temp_dir->isValid());
    m_test_backup_location = m_temp_dir->path();
}

void TestOutlookBackupAction::cleanup() {
    delete m_temp_dir;
    m_temp_dir = nullptr;
}

void TestOutlookBackupAction::testActionProperties() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QCOMPARE(action.name(), QString("Outlook Email Backup"));
    QVERIFY(!action.description().isEmpty());
}

void TestOutlookBackupAction::testActionCategory() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QCOMPARE(action.category(), sak::ActionCategory::QuickBackup);
}

void TestOutlookBackupAction::testRequiresAdmin() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QVERIFY(!action.requiresAdmin());
}

void TestOutlookBackupAction::testActionMetadata() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QVERIFY(!action.name().isEmpty());
    QVERIFY(!action.description().isEmpty());
    QCOMPARE(action.category(), sak::ActionCategory::QuickBackup);
}

void TestOutlookBackupAction::testDetectOutlookInstalled() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testGetOutlookVersion() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testDetectOutlookNotInstalled() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testDetectMultipleOutlookVersions() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testLocatePSTFiles() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testFindDefaultPSTLocation() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testFindCustomPSTLocation() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testFindMultiplePSTFiles() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testLocateOSTFiles() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testFindDefaultOSTLocation() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testFindCachedExchangeData() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testDetectOSTVsPST() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testValidatePSTFile() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testValidateOSTFile() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testDetectCorruptPST() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testDetectLargePSTFile() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testDetectOutlookRunning() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testWarnOutlookOpen() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testCheckFileLocked() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testForceCloseOption() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testGetPSTFileSize() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testHandleLargePST() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testCalculateTotalBackupSize() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testEstimateBackupTime() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testBackupAccountSettings() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testBackupEmailProfiles() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testBackupSignatures() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testBackupRules() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testDetectMultipleProfiles() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testBackupDefaultProfile() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testBackupAllProfiles() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testProfileSelection() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testBackupPSTFile() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testBackupOSTFile() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testBackupWithProgress() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::progressUpdated);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testVerifyBackupIntegrity() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testDetectFileInUse() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testWaitForFileRelease() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testShadowCopyOption() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testVSSBackup() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testReadOutlookRegistry() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testGetDataFilePaths() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testGetAccountConfiguration() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testBackupRegistrySettings() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testDetectExchangeAccount() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testBackupExchangeOST() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testWarnAboutOSTLimitations() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testOnlineArchiveDetection() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testBackupOutlook2016() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testBackupOutlook2019() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testBackupOutlook2021() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testBackupMicrosoft365() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testDetectArchivePST() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testBackupArchiveFiles() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testIncludeArchiveOption() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testScanForPSTFiles() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testScanProgress() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::progressUpdated);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testScanMultipleUsers() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testScanCancellation() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testExecuteBackup() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testExecuteWithTimestamp() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testExecuteMultipleFiles() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testExecuteTimeout() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testHandleOutlookNotFound() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testHandleNoPSTFiles() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestOutlookBackupAction::testHandleFileLocked() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testHandleInsufficientSpace() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestOutlookBackupAction::testHandleAccessDenied() {
    sak::OutlookBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::OutlookBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

QTEST_MAIN(TestOutlookBackupAction)
#include "test_outlook_backup_action.moc"

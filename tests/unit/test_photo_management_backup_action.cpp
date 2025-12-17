// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QtTest>
#include "sak/actions/photo_management_backup_action.h"

class TestPhotoManagementBackupAction : public QObject {
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

    // Lightroom detection
    void testDetectLightroomInstalled();
    void testGetLightroomVersion();
    void testDetectLightroomClassic();
    void testDetectLightroomCC();

    // Lightroom catalog discovery
    void testLocateLightroomCatalogs();
    void testFindDefaultCatalogLocation();
    void testFindCustomCatalogLocation();
    void testDetectMultipleCatalogs();

    // Lightroom catalog backup
    void testBackupCatalogFile();
    void testBackupCatalogPreviews();
    void testBackupCatalogSettings();
    void testBackupSmartPreviews();

    // Lightroom presets
    void testBackupDevelopPresets();
    void testBackupExportPresets();
    void testBackupPrintPresets();
    void testBackupUserTemplates();

    // Photoshop detection
    void testDetectPhotoshopInstalled();
    void testGetPhotoshopVersion();
    void testDetectPhotoshopCC();
    void testDetectPhotoshopElements();

    // Photoshop settings
    void testBackupPhotoshopPreferences();
    void testBackupCustomShapes();
    void testBackupBrushes();
    void testBackupPatterns();

    // Photoshop actions
    void testBackupActions();
    void testBackupActionSets();
    void testBackupScripts();

    // Photoshop workspaces
    void testBackupWorkspaces();
    void testBackupKeyboardShortcuts();
    void testBackupMenuCustomization();

    // Capture One detection
    void testDetectCaptureOneInstalled();
    void testGetCaptureOneVersion();
    void testLocateCaptureOneCatalogs();

    // Capture One backup
    void testBackupCaptureOneCatalog();
    void testBackupCaptureOneSettings();
    void testBackupCaptureOneStyles();

    // Other photo software
    void testDetectAdobeBridge();
    void testDetectONCapture();
    void testDetectDxOPhotoLab();

    // File size handling
    void testCalculateCatalogSize();
    void testCalculateLargeCatalog();
    void testEstimateBackupSize();

    // Scan functionality
    void testScanPhotoSoftware();
    void testScanMultipleSoftware();
    void testScanProgress();
    void testScanCancellation();

    // Execute functionality
    void testExecuteBackup();
    void testExecuteWithTimestamp();
    void testExecuteMultipleSoftware();
    void testExecuteTimeout();

    // Error handling
    void testHandleNoSoftwareFound();
    void testHandleNoCatalogs();
    void testHandleAccessDenied();
    void testHandleInsufficientSpace();
    void testHandleCorruptCatalog();

private:
    QTemporaryDir* m_temp_dir{nullptr};
    QString m_test_backup_location;
};

void TestPhotoManagementBackupAction::initTestCase() {
    // Setup test environment
}

void TestPhotoManagementBackupAction::cleanupTestCase() {
    // Cleanup test environment
}

void TestPhotoManagementBackupAction::init() {
    m_temp_dir = new QTemporaryDir();
    QVERIFY(m_temp_dir->isValid());
    m_test_backup_location = m_temp_dir->path();
}

void TestPhotoManagementBackupAction::cleanup() {
    delete m_temp_dir;
    m_temp_dir = nullptr;
}

void TestPhotoManagementBackupAction::testActionProperties() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QCOMPARE(action.name(), QString("Photo Management Backup"));
    QVERIFY(!action.description().isEmpty());
}

void TestPhotoManagementBackupAction::testActionCategory() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QCOMPARE(action.category(), sak::ActionCategory::QuickBackup);
}

void TestPhotoManagementBackupAction::testRequiresAdmin() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QVERIFY(!action.requiresAdmin());
}

void TestPhotoManagementBackupAction::testActionMetadata() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QVERIFY(!action.name().isEmpty());
    QVERIFY(!action.description().isEmpty());
    QCOMPARE(action.category(), sak::ActionCategory::QuickBackup);
}

void TestPhotoManagementBackupAction::testDetectLightroomInstalled() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testGetLightroomVersion() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testDetectLightroomClassic() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testDetectLightroomCC() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testLocateLightroomCatalogs() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testFindDefaultCatalogLocation() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testFindCustomCatalogLocation() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testDetectMultipleCatalogs() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testBackupCatalogFile() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testBackupCatalogPreviews() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testBackupCatalogSettings() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testBackupSmartPreviews() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testBackupDevelopPresets() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testBackupExportPresets() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testBackupPrintPresets() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testBackupUserTemplates() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testDetectPhotoshopInstalled() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testGetPhotoshopVersion() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testDetectPhotoshopCC() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testDetectPhotoshopElements() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testBackupPhotoshopPreferences() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testBackupCustomShapes() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testBackupBrushes() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testBackupPatterns() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testBackupActions() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testBackupActionSets() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testBackupScripts() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testBackupWorkspaces() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testBackupKeyboardShortcuts() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testBackupMenuCustomization() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testDetectCaptureOneInstalled() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testGetCaptureOneVersion() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testLocateCaptureOneCatalogs() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testBackupCaptureOneCatalog() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testBackupCaptureOneSettings() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testBackupCaptureOneStyles() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testDetectAdobeBridge() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testDetectONCapture() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testDetectDxOPhotoLab() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testCalculateCatalogSize() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testCalculateLargeCatalog() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testEstimateBackupSize() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testScanPhotoSoftware() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testScanMultipleSoftware() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testScanProgress() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::progressUpdated);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testScanCancellation() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testExecuteBackup() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testExecuteWithTimestamp() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testExecuteMultipleSoftware() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testExecuteTimeout() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testHandleNoSoftwareFound() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testHandleNoCatalogs() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(10000));
}

void TestPhotoManagementBackupAction::testHandleAccessDenied() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testHandleInsufficientSpace() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

void TestPhotoManagementBackupAction::testHandleCorruptCatalog() {
    sak::PhotoManagementBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::PhotoManagementBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(30000));
}

QTEST_MAIN(TestPhotoManagementBackupAction)
#include "test_photo_management_backup_action.moc"

// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QtTest>
#include "sak/actions/saved_game_data_backup_action.h"

class TestSavedGameDataBackupAction : public QObject {
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

    // Steam detection
    void testDetectSteamInstalled();
    void testGetSteamLocation();
    void testFindSteamUserData();
    void testDetectMultipleSteamAccounts();

    // Steam save scanning
    void testScanSteamCloudSaves();
    void testScanSteamLocalSaves();
    void testEnumerateSteamGames();
    void testDetectSteamAppData();

    // Epic Games detection
    void testDetectEpicGamesInstalled();
    void testGetEpicGamesLocation();
    void testFindEpicManifests();
    void testDetectEpicGamesSaves();

    // Epic Games save scanning
    void testScanEpicSavedGames();
    void testScanEpicLocalSaves();
    void testEnumerateEpicGames();

    // GOG detection
    void testDetectGOGInstalled();
    void testGetGOGLocation();
    void testFindGOGGamesSaves();
    void testDetectGOGGalaxyData();

    // GOG save scanning
    void testScanGOGSavedGames();
    void testScanGOGCloudSaves();
    void testEnumerateGOGGames();

    // Documents folder scanning
    void testScanMyGamesFolder();
    void testScanDocumentsSaves();
    void testDetectCommonSaveLocations();
    void testDetectCustomSaveLocations();

    // Save location identification
    void testIdentifySavesByGameName();
    void testIdentifySavesByPattern();
    void testDetectSaveFileTypes();
    void testValidateSaveIntegrity();

    // Size calculation
    void testCalculateSteamSavesSize();
    void testCalculateEpicSavesSize();
    void testCalculateGOGSavesSize();
    void testCalculateTotalSaveSize();

    // File enumeration
    void testCountSaveFiles();
    void testDetectLargeSaves();
    void testFilterBySaveType();

    // Multi-platform support
    void testScanMultiplePlatforms();
    void testMergeDuplicates();
    void testPrioritizePlatform();

    // Scan functionality
    void testScanGameSaves();
    void testScanProgress();
    void testScanCancellation();
    void testScanWithoutGames();

    // Execute functionality
    void testExecuteBackup();
    void testExecuteWithTimestamp();
    void testExecuteMultiplePlatforms();
    void testExecuteTimeout();

    // Backup verification
    void testVerifyBackupStructure();
    void testVerifyBackupIntegrity();
    void testVerifyAllFilesBackedUp();

    // Error handling
    void testHandleNoGamesFound();
    void testHandleNoSavesFound();
    void testHandleAccessDenied();
    void testHandleInsufficientSpace();
    void testHandleCorruptSave();

private:
    QTemporaryDir* m_temp_dir{nullptr};
    QString m_test_backup_location;
};

void TestSavedGameDataBackupAction::initTestCase() {
    // Setup test environment
}

void TestSavedGameDataBackupAction::cleanupTestCase() {
    // Cleanup test environment
}

void TestSavedGameDataBackupAction::init() {
    m_temp_dir = new QTemporaryDir();
    QVERIFY(m_temp_dir->isValid());
    m_test_backup_location = m_temp_dir->path();
}

void TestSavedGameDataBackupAction::cleanup() {
    delete m_temp_dir;
    m_temp_dir = nullptr;
}

void TestSavedGameDataBackupAction::testActionProperties() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QCOMPARE(action.name(), QString("Saved Game Data Backup"));
    QVERIFY(!action.description().isEmpty());
}

void TestSavedGameDataBackupAction::testActionCategory() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QCOMPARE(action.category(), sak::ActionCategory::QuickBackup);
}

void TestSavedGameDataBackupAction::testRequiresAdmin() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QVERIFY(!action.requiresAdmin());
}

void TestSavedGameDataBackupAction::testActionMetadata() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QVERIFY(!action.name().isEmpty());
    QVERIFY(!action.description().isEmpty());
    QCOMPARE(action.category(), sak::ActionCategory::QuickBackup);
}

void TestSavedGameDataBackupAction::testDetectSteamInstalled() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testGetSteamLocation() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testFindSteamUserData() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testDetectMultipleSteamAccounts() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testScanSteamCloudSaves() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testScanSteamLocalSaves() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testEnumerateSteamGames() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testDetectSteamAppData() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testDetectEpicGamesInstalled() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testGetEpicGamesLocation() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testFindEpicManifests() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testDetectEpicGamesSaves() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testScanEpicSavedGames() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testScanEpicLocalSaves() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testEnumerateEpicGames() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testDetectGOGInstalled() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testGetGOGLocation() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testFindGOGGamesSaves() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testDetectGOGGalaxyData() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testScanGOGSavedGames() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testScanGOGCloudSaves() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testEnumerateGOGGames() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testScanMyGamesFolder() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testScanDocumentsSaves() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testDetectCommonSaveLocations() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testDetectCustomSaveLocations() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testIdentifySavesByGameName() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testIdentifySavesByPattern() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testDetectSaveFileTypes() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testValidateSaveIntegrity() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testCalculateSteamSavesSize() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testCalculateEpicSavesSize() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testCalculateGOGSavesSize() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testCalculateTotalSaveSize() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testCountSaveFiles() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testDetectLargeSaves() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testFilterBySaveType() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testScanMultiplePlatforms() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testMergeDuplicates() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testPrioritizePlatform() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testScanGameSaves() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testScanProgress() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::progressUpdated);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testScanCancellation() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testScanWithoutGames() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testExecuteBackup() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(45000));
}

void TestSavedGameDataBackupAction::testExecuteWithTimestamp() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(45000));
}

void TestSavedGameDataBackupAction::testExecuteMultiplePlatforms() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(45000));
}

void TestSavedGameDataBackupAction::testExecuteTimeout() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(45000));
}

void TestSavedGameDataBackupAction::testVerifyBackupStructure() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(45000));
}

void TestSavedGameDataBackupAction::testVerifyBackupIntegrity() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(45000));
}

void TestSavedGameDataBackupAction::testVerifyAllFilesBackedUp() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(45000));
}

void TestSavedGameDataBackupAction::testHandleNoGamesFound() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testHandleNoSavesFound() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::scanCompleted);
    
    action.scan();
    QVERIFY(spy.wait(15000));
}

void TestSavedGameDataBackupAction::testHandleAccessDenied() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(45000));
}

void TestSavedGameDataBackupAction::testHandleInsufficientSpace() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(45000));
}

void TestSavedGameDataBackupAction::testHandleCorruptSave() {
    sak::SavedGameDataBackupAction action(m_test_backup_location);
    QSignalSpy spy(&action, &sak::SavedGameDataBackupAction::executionCompleted);
    
    action.execute();
    QVERIFY(spy.wait(45000));
}

QTEST_MAIN(TestSavedGameDataBackupAction)
#include "test_saved_game_data_backup_action.moc"

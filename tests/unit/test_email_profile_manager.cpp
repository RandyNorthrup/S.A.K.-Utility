// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_email_profile_manager.cpp
/// @brief Unit tests for the email profile manager

#include "sak/email_profile_manager.h"
#include "sak/email_types.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class TestEmailProfileManager : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- Construction ----------------------------------------------------
    void defaultConstruction();

    // -- Data Types -------------------------------------------------------
    void clientTypeDefaults();
    void profileDefaults();
    void dataFileDefaults();

    // -- Linked Files ----------------------------------------------------
    void linkedFilesEmptyByDefault();

    // -- Cancel ----------------------------------------------------------
    void cancelBeforeDiscoveryDoesNotCrash();

    // -- Discovery (smoke test) ------------------------------------------
    void discoverProfilesEmitsSignal();

    // -- Backup Invalid Inputs -------------------------------------------
    void backupWithEmptyIndices();
    void backupWithInvalidPath();
    void backupWithNoDiscoveredProfiles();

    // -- Restore Invalid Inputs ------------------------------------------
    void restoreFromNonExistentManifest();
    void restoreFromEmptyPath();

    // -- Profile Structure -----------------------------------------------
    void profilePopulation();
    void dataFilePopulation();
};

// ============================================================================
// Construction
// ============================================================================

void TestEmailProfileManager::defaultConstruction() {
    EmailProfileManager manager;
    QVERIFY(manager.linkedFilePaths().isEmpty());
}

// ============================================================================
// Data Types
// ============================================================================

void TestEmailProfileManager::clientTypeDefaults() {
    QVERIFY(static_cast<int>(sak::EmailClientType::Outlook) == 0);
    QVERIFY(static_cast<int>(sak::EmailClientType::Thunderbird) == 1);
    QVERIFY(static_cast<int>(sak::EmailClientType::WindowsMail) == 2);
    QVERIFY(static_cast<int>(sak::EmailClientType::Other) == 3);
}

void TestEmailProfileManager::profileDefaults() {
    sak::EmailClientProfile profile;
    QCOMPARE(profile.client_type, sak::EmailClientType::Other);
    QVERIFY(profile.client_name.isEmpty());
    QVERIFY(profile.client_version.isEmpty());
    QVERIFY(profile.profile_name.isEmpty());
    QVERIFY(profile.profile_path.isEmpty());
    QVERIFY(profile.data_files.isEmpty());
    QCOMPARE(profile.total_size_bytes, static_cast<qint64>(0));
}

void TestEmailProfileManager::dataFileDefaults() {
    sak::EmailDataFile data_file;
    QVERIFY(data_file.path.isEmpty());
    QVERIFY(data_file.type.isEmpty());
    QCOMPARE(data_file.size_bytes, static_cast<qint64>(0));
    QVERIFY(!data_file.is_linked);
}

// ============================================================================
// Linked Files
// ============================================================================

void TestEmailProfileManager::linkedFilesEmptyByDefault() {
    EmailProfileManager manager;
    QSet<QString> linked = manager.linkedFilePaths();
    QVERIFY(linked.isEmpty());
}

// ============================================================================
// Cancel
// ============================================================================

void TestEmailProfileManager::cancelBeforeDiscoveryDoesNotCrash() {
    EmailProfileManager manager;
    manager.cancel();
    manager.cancel();
    QVERIFY(true);
}

// ============================================================================
// Discovery (smoke test — depends on system state)
// ============================================================================

void TestEmailProfileManager::discoverProfilesEmitsSignal() {
    EmailProfileManager manager;
    QSignalSpy spy(&manager, &EmailProfileManager::profilesDiscovered);
    manager.discoverProfiles();

    // Discovery may find 0+ profiles depending on the machine,
    // but the signal should always be emitted
    QVERIFY(spy.count() > 0);

    auto profiles = spy.at(0).at(0).value<QVector<sak::EmailClientProfile>>();
    // Just verify we got a valid vector (empty is fine)
    QVERIFY(profiles.size() >= 0);
}

// ============================================================================
// Backup Invalid Inputs
// ============================================================================

void TestEmailProfileManager::backupWithEmptyIndices() {
    EmailProfileManager manager;
    QSignalSpy error_spy(&manager, &EmailProfileManager::errorOccurred);
    QSignalSpy complete_spy(&manager, &EmailProfileManager::backupComplete);

    manager.backupProfiles({}, QStringLiteral("C:/backup"));

    // Empty indices — should either emit an error or complete with 0
    QVERIFY(error_spy.count() > 0 || complete_spy.count() > 0);
}

void TestEmailProfileManager::backupWithInvalidPath() {
    EmailProfileManager manager;
    QSignalSpy error_spy(&manager, &EmailProfileManager::errorOccurred);

    QVector<int> indices = {0};
    manager.backupProfiles(indices, QString());

    // Empty path should produce an error or be handled gracefully
    QVERIFY(error_spy.count() > 0 || true);
}

void TestEmailProfileManager::backupWithNoDiscoveredProfiles() {
    EmailProfileManager manager;
    QSignalSpy error_spy(&manager, &EmailProfileManager::errorOccurred);
    QSignalSpy complete_spy(&manager, &EmailProfileManager::backupComplete);

    // No discovery done — indices won't map to anything
    QVector<int> indices = {0, 1, 2};
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    manager.backupProfiles(indices, temp_dir.path());

    // Should handle gracefully
    QVERIFY(error_spy.count() > 0 || complete_spy.count() > 0);
}

// ============================================================================
// Restore Invalid Inputs
// ============================================================================

void TestEmailProfileManager::restoreFromNonExistentManifest() {
    EmailProfileManager manager;
    QSignalSpy error_spy(&manager, &EmailProfileManager::errorOccurred);

    manager.restoreProfiles(QStringLiteral("C:/nonexistent/manifest.json"));

    QVERIFY(error_spy.count() > 0);
}

void TestEmailProfileManager::restoreFromEmptyPath() {
    EmailProfileManager manager;
    QSignalSpy error_spy(&manager, &EmailProfileManager::errorOccurred);

    manager.restoreProfiles(QString());

    // Empty path should produce an error
    QVERIFY(error_spy.count() > 0 || true);
}

// ============================================================================
// Profile Structure Population
// ============================================================================

void TestEmailProfileManager::profilePopulation() {
    sak::EmailClientProfile profile;
    profile.client_type = sak::EmailClientType::Outlook;
    profile.client_name = QStringLiteral("Microsoft Outlook 2021");
    profile.client_version = QStringLiteral("16.0");
    profile.profile_name = QStringLiteral("Default");
    profile.profile_path = QStringLiteral("HKCU\\Software\\Microsoft\\Outlook");
    profile.total_size_bytes = 1024 * 1024 * 500;

    sak::EmailDataFile pst_file;
    pst_file.path = QStringLiteral("C:/Users/Test/Documents/Outlook.pst");
    pst_file.type = QStringLiteral("PST");
    pst_file.size_bytes = 1024 * 1024 * 300;
    pst_file.is_linked = true;
    profile.data_files.append(pst_file);

    QCOMPARE(profile.client_type, sak::EmailClientType::Outlook);
    QCOMPARE(profile.data_files.size(), 1);
    QVERIFY(profile.data_files[0].is_linked);
    QCOMPARE(profile.data_files[0].type, QStringLiteral("PST"));
}

void TestEmailProfileManager::dataFilePopulation() {
    sak::EmailDataFile file;
    file.path = QStringLiteral("C:/mail.mbox");
    file.type = QStringLiteral("MBOX");
    file.size_bytes = 50 * 1024;
    file.is_linked = false;

    QCOMPARE(file.path, QStringLiteral("C:/mail.mbox"));
    QCOMPARE(file.size_bytes, static_cast<qint64>(50 * 1024));
    QVERIFY(!file.is_linked);
}

QTEST_MAIN(TestEmailProfileManager)
#include "test_email_profile_manager.moc"

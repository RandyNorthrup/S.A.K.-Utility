// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_config_manager.cpp
/// @brief Unit tests for ConfigManager singleton â€” load, save, defaults, signals

#include "sak/config_manager.h"

#include <QSignalSpy>
#include <QtTest/QtTest>
#include <QVariant>

class ConfigManagerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void cleanup();

    // Singleton
    void instance_returnsSameObject();

    // Basic get/set
    void setValue_getValue();
    void getValue_defaultValue();
    void contains_existingKey();
    void contains_nonExistentKey();
    void remove_key();

    // Defaults
    void resetToDefaults_restoresValues();

    // Signal emission
    void setValue_emitsSignal();

    // Typed accessors â€” backup
    void backupThreadCount_setGet();
    void backupVerifyMD5_setGet();
    void lastBackupLocation_setGet();

    // Typed accessors â€” organizer
    void organizerPreviewMode_setGet();

    // Typed accessors â€” duplicate finder
    void duplicateMinFileSize_setGet();
    void duplicateKeepStrategy_setGet();

    // Typed accessors â€” image flasher
    void imageFlasherValidationMode_setGet();
    void imageFlasherBufferSize_setGet();


    // Clear
    void clear_removesAllKeys();
};

void ConfigManagerTests::cleanup() {
    // Reset to defaults after each test to avoid cross-contamination
    sak::ConfigManager::instance().resetToDefaults();
}

// ============================================================================
// Singleton
// ============================================================================

void ConfigManagerTests::instance_returnsSameObject() {
    auto& a = sak::ConfigManager::instance();
    auto& b = sak::ConfigManager::instance();
    QCOMPARE(&a, &b);
}

// ============================================================================
// Basic Key-Value Operations
// ============================================================================

void ConfigManagerTests::setValue_getValue() {
    auto& mgr = sak::ConfigManager::instance();
    mgr.setValue("test/custom_key", 42);
    QCOMPARE(mgr.getValue("test/custom_key").toInt(), 42);
}

void ConfigManagerTests::getValue_defaultValue() {
    auto& mgr = sak::ConfigManager::instance();
    auto val = mgr.getValue("test/nonexistent_key_xyz", QVariant(999));
    QCOMPARE(val.toInt(), 999);
}

void ConfigManagerTests::contains_existingKey() {
    auto& mgr = sak::ConfigManager::instance();
    mgr.setValue("test/exists", true);
    QVERIFY(mgr.contains("test/exists"));
}

void ConfigManagerTests::contains_nonExistentKey() {
    auto& mgr = sak::ConfigManager::instance();
    QVERIFY(!mgr.contains("test/definitely_not_here"));
}

void ConfigManagerTests::remove_key() {
    auto& mgr = sak::ConfigManager::instance();
    mgr.setValue("test/to_remove", "value");
    QVERIFY(mgr.contains("test/to_remove"));
    mgr.remove("test/to_remove");
    QVERIFY(!mgr.contains("test/to_remove"));
}

// ============================================================================
// Defaults
// ============================================================================

void ConfigManagerTests::resetToDefaults_restoresValues() {
    auto& mgr = sak::ConfigManager::instance();

    // Override a known default
    mgr.setBackupThreadCount(99);
    QCOMPARE(mgr.getBackupThreadCount(), 99);

    // Reset
    mgr.resetToDefaults();

    // Should be back to default (typically 4 or similar)
    int defaultCount = mgr.getBackupThreadCount();
    QVERIFY(defaultCount != 99);
    QVERIFY(defaultCount > 0);
}

// ============================================================================
// Signal
// ============================================================================

void ConfigManagerTests::setValue_emitsSignal() {
    auto& mgr = sak::ConfigManager::instance();
    QSignalSpy spy(&mgr, &sak::ConfigManager::settingChanged);
    QVERIFY(spy.isValid());

    mgr.setValue("test/signal_key", "signal_value");
    QCOMPARE(spy.count(), 1);

    auto args = spy.takeFirst();
    QCOMPARE(args.at(0).toString(), QString("test/signal_key"));
    QCOMPARE(args.at(1).toString(), QString("signal_value"));
}

// ============================================================================
// Typed Accessors â€” Backup
// ============================================================================

void ConfigManagerTests::backupThreadCount_setGet() {
    auto& mgr = sak::ConfigManager::instance();
    mgr.setBackupThreadCount(8);
    QCOMPARE(mgr.getBackupThreadCount(), 8);
}

void ConfigManagerTests::backupVerifyMD5_setGet() {
    auto& mgr = sak::ConfigManager::instance();
    mgr.setBackupVerifyMD5(true);
    QVERIFY(mgr.getBackupVerifyMD5());
    mgr.setBackupVerifyMD5(false);
    QVERIFY(!mgr.getBackupVerifyMD5());
}

void ConfigManagerTests::lastBackupLocation_setGet() {
    auto& mgr = sak::ConfigManager::instance();
    mgr.setLastBackupLocation("C:/Backups/test");
    QCOMPARE(mgr.getLastBackupLocation(), QString("C:/Backups/test"));
}

// ============================================================================
// Typed Accessors â€” Organizer
// ============================================================================

void ConfigManagerTests::organizerPreviewMode_setGet() {
    auto& mgr = sak::ConfigManager::instance();
    mgr.setOrganizerPreviewMode(true);
    QVERIFY(mgr.getOrganizerPreviewMode());
}

// ============================================================================
// Typed Accessors â€” Duplicate Finder
// ============================================================================

void ConfigManagerTests::duplicateMinFileSize_setGet() {
    auto& mgr = sak::ConfigManager::instance();
    mgr.setDuplicateMinimumFileSize(1024);
    QCOMPARE(mgr.getDuplicateMinimumFileSize(), qint64{1024});
}

void ConfigManagerTests::duplicateKeepStrategy_setGet() {
    auto& mgr = sak::ConfigManager::instance();
    mgr.setDuplicateKeepStrategy("newest");
    QCOMPARE(mgr.getDuplicateKeepStrategy(), QString("newest"));
}

// ============================================================================
// Typed Accessors â€” Image Flasher
// ============================================================================

void ConfigManagerTests::imageFlasherValidationMode_setGet() {
    auto& mgr = sak::ConfigManager::instance();
    mgr.setImageFlasherValidationMode("full");
    QCOMPARE(mgr.getImageFlasherValidationMode(), QString("full"));
}

void ConfigManagerTests::imageFlasherBufferSize_setGet() {
    auto& mgr = sak::ConfigManager::instance();
    mgr.setImageFlasherBufferSize(2048);
    QCOMPARE(mgr.getImageFlasherBufferSize(), 2048);
}

// ============================================================================
// Clear
// ============================================================================

void ConfigManagerTests::clear_removesAllKeys() {
    auto& mgr = sak::ConfigManager::instance();
    mgr.setValue("test/clear_me", "value");
    QVERIFY(mgr.contains("test/clear_me"));

    mgr.clear();
    QVERIFY(!mgr.contains("test/clear_me"));
}

QTEST_GUILESS_MAIN(ConfigManagerTests)
#include "test_config_manager.moc"

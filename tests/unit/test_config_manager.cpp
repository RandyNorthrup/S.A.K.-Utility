// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_config_manager.cpp
/// @brief Unit tests for ConfigManager singleton -- load, save, defaults, signals

#include "sak/app_paths.h"
#include "sak/config_manager.h"

#include <QCoreApplication>
#include <QDir>
#include <QSignalSpy>
#include <QtTest/QtTest>
#include <QVariant>

class ConfigManagerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void cleanup();

    // Singleton
    void instance_returnsSameObject();
    void configPath_isPortableAppLocal();

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

    // Typed accessors -- backup
    void backupThreadCount_setGet();
    void backupVerifyMD5_setGet();
    void lastBackupLocation_setGet();

    // Typed accessors -- organizer
    void organizerPreviewMode_setGet();

    // Typed accessors -- duplicate finder
    void duplicateMinFileSize_setGet();
    void duplicateKeepStrategy_setGet();

    // Typed accessors -- image flasher
    void imageFlasherValidationMode_setGet();
    void imageFlasherBufferSize_setGet();
    void imageFlasherDefaults_doNotRecreateRetiredSystemDriveWarning();

    // Getter-side invariant enforcement (mirror setter invariants on read)
    void typedGetters_reclampOutOfRangeStoredValues();


    // Clear
    void clear_removesAllKeys();

    // B5 tail: settings-store health is checked, not silently ignored.
    void sync_reportsHealthy();
    void describeSettingsStatus_mapsErrors();
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

void ConfigManagerTests::configPath_isPortableAppLocal() {
    const QString appDir = QDir::cleanPath(QCoreApplication::applicationDirPath());
    const QString configPath = QDir::cleanPath(sak::app_paths::configFilePath());
    // Portable branch: the whole path is fixed (appDir/data/config/Utility.ini). The old
    // startsWith+endsWith pair left the middle segment unconstrained.
    QCOMPARE(configPath, QDir::cleanPath(appDir + QStringLiteral("/data/config/Utility.ini")));
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

    // resetToDefaults restores the exact kDefaultBackupThreadCount (mirrors the reclamp
    // sibling's QCOMPARE at getBackupThreadCount()==4).
    int defaultCount = mgr.getBackupThreadCount();
    QCOMPARE(defaultCount, 4);
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
// Typed Accessors -- Backup
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
// Typed Accessors -- Organizer
// ============================================================================

void ConfigManagerTests::organizerPreviewMode_setGet() {
    auto& mgr = sak::ConfigManager::instance();
    mgr.setOrganizerPreviewMode(true);
    QVERIFY(mgr.getOrganizerPreviewMode());
}

// ============================================================================
// Typed Accessors -- Duplicate Finder
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
// Typed Accessors -- Image Flasher
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

// image_flasher/show_system_drive_warning was retired: the system-drive block is
// unconditional, so a switch that claimed to control it described protection the
// user could not actually turn off. Nothing may put it back -- a default for it
// would resurrect the control in every config file.
void ConfigManagerTests::imageFlasherDefaults_doNotRecreateRetiredSystemDriveWarning() {
    auto& mgr = sak::ConfigManager::instance();
    mgr.resetToDefaults();

    // The flasher defaults really did run (otherwise the absence below proves
    // nothing).
    QVERIFY(mgr.contains("image_flasher/show_large_drive_warning"));
    QVERIFY(mgr.contains("image_flasher/large_drive_threshold"));
    QVERIFY(mgr.contains("image_flasher/max_concurrent_writes"));
    QVERIFY(mgr.contains("image_flasher/unmount_on_completion"));

    QVERIFY(!mgr.contains("image_flasher/show_system_drive_warning"));
}

// ============================================================================
// Getter-side invariant enforcement
// ============================================================================

// A stored value can be out of range (older config, hand-edited INI, foreign
// writer) even though the setters reject bad input. The typed getters must
// re-clamp on read and hand back the default rather than the invalid value.
void ConfigManagerTests::typedGetters_reclampOutOfRangeStoredValues() {
    auto& mgr = sak::ConfigManager::instance();

    // Poke invalid values straight into the store, bypassing the setter guards.
    mgr.setValue("backup/thread_count", -5);
    QCOMPARE(mgr.getBackupThreadCount(), 4);

    mgr.setValue("duplicate/minimum_file_size", qint64{-1});
    QCOMPARE(mgr.getDuplicateMinimumFileSize(), qint64{0});

    mgr.setValue("duplicate/keep_strategy", QString());
    QCOMPARE(mgr.getDuplicateKeepStrategy(), QString("oldest"));

    mgr.setValue("image_flasher/validation_mode", QString());
    QCOMPARE(mgr.getImageFlasherValidationMode(), QString("full"));

    mgr.setValue("image_flasher/buffer_size", 0);
    // A non-positive buffer reclamps to the default kBufferAlignment (4096); literal because
    // config_manager.h does not include network_constants.h.
    QCOMPARE(mgr.getImageFlasherBufferSize(), 4096);

    mgr.setValue("image_flasher/large_drive_threshold", -10);
    QCOMPARE(mgr.getImageFlasherLargeDriveThreshold(), 128);

    mgr.setValue("image_flasher/max_concurrent_writes", 0);
    QCOMPARE(mgr.getImageFlasherMaxConcurrentWrites(), 1);
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

// ============================================================================
// B5 tail: settings-store health
// ============================================================================

// A normal writable config syncs successfully and reports healthy; previously
// sync() ignored QSettings::status() entirely (a failed disk write looked ok).
void ConfigManagerTests::sync_reportsHealthy() {
    auto& mgr = sak::ConfigManager::instance();
    mgr.setValue("test/sync_probe", 7);
    QVERIFY(mgr.sync());
    QVERIFY(mgr.isHealthy());
}

// The status->message mapping used by sync()/init is well-formed: NoError is
// empty (no error to report); real errors carry a non-empty message.
void ConfigManagerTests::describeSettingsStatus_mapsErrors() {
    QVERIFY(sak::ConfigManager::describeSettingsStatus(QSettings::NoError).isEmpty());
    QCOMPARE(sak::ConfigManager::describeSettingsStatus(QSettings::AccessError),
             QStringLiteral("settings access error (permission denied or file locked)"));
    QCOMPARE(sak::ConfigManager::describeSettingsStatus(QSettings::FormatError),
             QStringLiteral("settings format error (config file is corrupt)"));
}

QTEST_GUILESS_MAIN(ConfigManagerTests)
#include "test_config_manager.moc"

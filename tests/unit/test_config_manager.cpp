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

    // Second arm: the setter REFUSES non-positive input outright (early return, no
    // write, hence no settingChanged -- config_manager.cpp:287-293). That is a
    // different contract from "stored, then re-clamped on read", so the previously
    // stored 8 must survive untouched. Read the RAW key: the typed getter's own
    // re-clamp would mask an overwritten value with the same 4 either way.
    QSignalSpy spy(&mgr, &sak::ConfigManager::settingChanged);
    QVERIFY(spy.isValid());
    mgr.setBackupThreadCount(0);
    mgr.setBackupThreadCount(-5);
    QCOMPARE(spy.count(), 0);
    QCOMPARE(mgr.getValue("backup/thread_count").toInt(), 8);
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
    // Absent-key contract first: the getter's default is an EMPTY string
    // (config_manager.cpp:304), never a guessed path.
    mgr.remove("backup/last_location");
    QCOMPARE(mgr.getLastBackupLocation(), QString());
    mgr.setLastBackupLocation("C:/Backups/test");
    QCOMPARE(mgr.getLastBackupLocation(), QString("C:/Backups/test"));
    // Pin the persisted key name too: setter and getter agree by construction, so a
    // symmetric rename round-trips green while orphaning every already-stored
    // last-backup location on upgrade.
    QCOMPARE(mgr.getValue("backup/last_location").toString(), QString("C:/Backups/test"));
}

// ============================================================================
// Typed Accessors -- Organizer
// ============================================================================

void ConfigManagerTests::organizerPreviewMode_setGet() {
    auto& mgr = sak::ConfigManager::instance();
    // `true` is BOTH the initialized default (config_manager.cpp:127-129) and the
    // getter's own fallback (config_manager.cpp:313), and cleanup() re-runs
    // resetToDefaults() before every case -- so writing true and reading true is
    // green with the setter entirely gutted. Drive `false`, the value only a real
    // write can produce, and pin the raw key so a symmetric key rename cannot hide
    // inside the setter/getter pair.
    mgr.setOrganizerPreviewMode(false);
    QVERIFY(!mgr.getOrganizerPreviewMode());
    QVERIFY(mgr.contains("organizer/preview_mode"));
    QCOMPARE(mgr.getValue("organizer/preview_mode").toBool(), false);
    mgr.setOrganizerPreviewMode(true);
    QVERIFY(mgr.getOrganizerPreviewMode());
    QCOMPARE(mgr.getValue("organizer/preview_mode").toBool(), true);
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
    // "full" is BOTH the initialized default AND the value every unrecognized
    // stored string re-clamps to, so a "full" round trip is green against a getter
    // hard-wired to return "full" and against a no-op setter alike. Drive the other
    // two whitelisted modes, which survive only if the write and the whitelist
    // pass-through are both real, and pin the raw key.
    mgr.setImageFlasherValidationMode("quick");
    QCOMPARE(mgr.getImageFlasherValidationMode(), QString("quick"));
    QCOMPARE(mgr.getValue("image_flasher/validation_mode").toString(), QString("quick"));
    mgr.setImageFlasherValidationMode("none");
    QCOMPARE(mgr.getImageFlasherValidationMode(), QString("none"));
    QCOMPARE(mgr.getValue("image_flasher/validation_mode").toString(), QString("none"));
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
    // Existence alone lets the flasher defaults be written with the WRONG values:
    // every typed getter carries the same fallback, so a wrong stored value is
    // invisible through the getters, and the reclamp sibling only ever pins those
    // fallbacks. Pin the raw stored values (read with no getter-side default).
    QVERIFY(mgr.contains("image_flasher/show_large_drive_warning"));
    QCOMPARE(mgr.getValue("image_flasher/show_large_drive_warning").toBool(), true);
    QVERIFY(mgr.contains("image_flasher/large_drive_threshold"));
    QCOMPARE(mgr.getValue("image_flasher/large_drive_threshold").toInt(), 128);
    QVERIFY(mgr.contains("image_flasher/max_concurrent_writes"));
    QCOMPARE(mgr.getValue("image_flasher/max_concurrent_writes").toInt(), 1);
    QVERIFY(mgr.contains("image_flasher/unmount_on_completion"));
    QCOMPARE(mgr.getValue("image_flasher/unmount_on_completion").toBool(), true);
    QVERIFY(mgr.contains("image_flasher/validation_mode"));
    QCOMPARE(mgr.getValue("image_flasher/validation_mode").toString(), QString("full"));
    QVERIFY(mgr.contains("image_flasher/buffer_size"));
    // kBufferAlignment (4096) as a literal: config_manager.h does not include
    // network_constants.h.
    QCOMPARE(mgr.getValue("image_flasher/buffer_size").toInt(), 4096);
    QVERIFY(mgr.contains("image_flasher/enable_notifications"));
    QCOMPARE(mgr.getValue("image_flasher/enable_notifications").toBool(), true);

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
    // The mode guard is a WHITELIST (config_manager.cpp:49-55), not an emptiness
    // check: an unrecognized NON-EMPTY mode must also re-clamp, while a whitelisted
    // one passes through untouched. Empty-only coverage is green against a plain
    // nonEmptyOrDefault, which would let "bogus" reach the flasher.
    mgr.setValue("image_flasher/validation_mode", QStringLiteral("bogus"));
    QCOMPARE(mgr.getImageFlasherValidationMode(), QString("full"));
    mgr.setValue("image_flasher/validation_mode", QStringLiteral("quick"));
    QCOMPARE(mgr.getImageFlasherValidationMode(), QString("quick"));

    mgr.setValue("image_flasher/buffer_size", 0);
    // A non-positive buffer reclamps to the default kBufferAlignment (4096); literal because
    // config_manager.h does not include network_constants.h.
    QCOMPARE(mgr.getImageFlasherBufferSize(), 4096);
    // The buffer guard has TWO arms: the magnitude cap (kMaxImageFlasherBufferSizeMb
    // == 4096 MB, config_manager.cpp:26) is what stops an absurd allocation request
    // from a hand-edited INI. Bracket it -- 4095 is in range and passes through,
    // 4097 is over and reclamps. (4096 itself would prove nothing: it equals the
    // default the clamp hands back.)
    mgr.setValue("image_flasher/buffer_size", 4095);
    QCOMPARE(mgr.getImageFlasherBufferSize(), 4095);
    mgr.setValue("image_flasher/buffer_size", 4097);
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
    mgr.resetToDefaults();
    mgr.setValue("test/clear_me", "value");
    // A key in no group this build knows about. "Removes ALL keys" is otherwise
    // unfalsifiable: a clear() that walks the groups it recognizes passes a probe
    // that lives inside one of them.
    mgr.setValue("foreign/unknown_feature", 9001);
    QVERIFY(mgr.contains("test/clear_me"));
    QVERIFY(mgr.contains("backup/thread_count"));
    QCOMPARE(mgr.storedSchemaVersion(), sak::ConfigManager::kCurrentSchemaVersion);

    mgr.clear();
    QVERIFY(!mgr.contains("test/clear_me"));
    QVERIFY(!mgr.contains("foreign/unknown_feature"));
    QVERIFY(!mgr.contains("backup/thread_count"));
    // The meta schema stamp goes too: resetToDefaults() re-stamps it right after
    // clear() (config_manager.cpp:214-220), and a stamp that survives leaves a
    // newer-than-current store permanently unhealthy across a "reset to defaults".
    QCOMPARE(mgr.storedSchemaVersion(), sak::ConfigManager::kNoSchemaVersion);
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
    // The post-switch arm (config_manager.cpp:256) is the one no other test reaches,
    // and its NON-emptiness is a fail-closed signal: the constructor treats an empty
    // describe() as "loaded clean" (:88-98) and then writes defaults over the store,
    // so a status this build does not enumerate must never read as healthy there.
    // QSettings::Status has enumerators 0/1/2 and no fixed underlying type, so 3 is
    // inside the enumeration's value range and the cast is well-defined.
    const QString unknown_status =
        sak::ConfigManager::describeSettingsStatus(static_cast<QSettings::Status>(3));
    QVERIFY(!unknown_status.isEmpty());
    QCOMPARE(unknown_status, QStringLiteral("unknown settings error"));
}

QTEST_GUILESS_MAIN(ConfigManagerTests)
#include "test_config_manager.moc"

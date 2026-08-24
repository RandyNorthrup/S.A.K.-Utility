// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_config_schema_versioning.cpp
/// @brief R5-G23-5: config schema versioning -- current round-trip, older-version
///        read migrates forward (no data loss), newer-version read preserves
///        unknown keys and reports unhealthy.

#include "sak/config_manager.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <QVariant>

using sak::ConfigManager;

namespace {

// A value written by a hypothetical future build; used only to fabricate a
// "rolled back to this build" store. Named so the intent is not a bare literal.
constexpr int kFutureSchemaVersion = ConfigManager::kCurrentSchemaVersion + 1;

// Distinctive probe values so a preserved key is unmistakable in a failure diff.
constexpr int kBackupThreadProbe = 7;
constexpr int kPreciseProbe = 4242;
constexpr int kFutureFeatureProbe = 9001;

QByteArray readAllBytes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

QString schemaKey() {
    return QLatin1String(ConfigManager::kSchemaVersionKey);
}

}  // namespace

class ConfigSchemaVersioningTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void cleanup();

    // Pure seam: reconcileSchemaVersion(QSettings&)
    void reconcile_currentVersion_isByteIdenticalNoWrite();
    void reconcile_olderVersion_migratesAndPreservesValues();
    void reconcile_legacyNoVersionKey_migratesAndPreservesValues();
    void reconcile_newerVersion_preservesUnknownKeysAndVersion();

    // Singleton integration
    void singleton_freshStore_isCurrentVersionAndHealthy();
    void singleton_currentVersionRoundTrip_valueSurvives();
    void singleton_newerVersion_isUnhealthyAndDataPreserved();
};

void ConfigSchemaVersioningTests::cleanup() {
    // Restore a clean, current-version store after each test so a poked future
    // version cannot leak into the next case.
    ConfigManager::instance().resetToDefaults();
}

// ============================================================================
// Pure seam -- reconcileSchemaVersion
// ============================================================================

// Current-version round-trip: a store already at the current schema is not
// rewritten at all -- the file is byte-identical before and after reconcile.
void ConfigSchemaVersioningTests::reconcile_currentVersion_isByteIdenticalNoWrite() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("cfg.ini"));

    {
        QSettings seed(path, QSettings::IniFormat);
        seed.setValue(schemaKey(), ConfigManager::kCurrentSchemaVersion);
        seed.setValue(QStringLiteral("backup/thread_count"), kBackupThreadProbe);
        // A key outside this build's recognized set. Without one, "no write" is
        // unfalsifiable: a Current branch that prunes unrecognized keys has nothing to
        // prune here, never dirties the store, and leaves the file byte-identical.
        seed.setValue(QStringLiteral("future/unknown_feature"), kFutureFeatureProbe);
        seed.sync();
    }
    // QSettings regenerates the whole INI from its parsed map -- same groups, same values,
    // same bytes -- so a redundant re-stamp of the version the store ALREADY holds
    // reproduces the file EXACTLY, and "no write" is unfalsifiable through the bytes alone.
    // A hand-added comment line is the one thing that rewriter drops, so ANY write is now
    // visible. This is what kills the natural merge of the Current and Migrated arms into
    // one unconditional setValue(kSchemaVersionKey, ...), which would dirty and rewrite the
    // user's config on every launch and flip a read-only or locked store to AccessError,
    // making isHealthy() refuse.
    const QByteArray marker = QByteArrayLiteral("; sak-no-rewrite-marker\n");
    {
        QFile raw(path);
        QVERIFY(raw.open(QIODevice::Append));
        QCOMPARE(raw.write(marker), static_cast<qint64>(marker.size()));
    }
    const QByteArray before = readAllBytes(path);
    QVERIFY(before.endsWith(marker));

    QSettings store(path, QSettings::IniFormat);
    const auto state = ConfigManager::reconcileSchemaVersion(store);
    store.sync();

    QCOMPARE(static_cast<int>(state), static_cast<int>(ConfigManager::SchemaVersionState::Current));
    QCOMPARE(store.value(QStringLiteral("backup/thread_count")).toInt(), kBackupThreadProbe);
    QCOMPARE(store.value(QStringLiteral("future/unknown_feature")).toInt(), kFutureFeatureProbe);
    QCOMPARE(store.value(schemaKey()).toInt(), ConfigManager::kCurrentSchemaVersion);
    // Pin the PERSISTED name, not only schemaKey(): every seed and every read-back in this
    // file resolves the stamp through the same constant, so renaming kSchemaVersionKey round
    // trips green everywhere. On upgrade the rename orphans an installed store's stamp -- and
    // worse, a config a NEWER build stamped under the OLD name then reads as ABSENT, so
    // reconcileSchemaVersion returns Migrated instead of FromFuture and isHealthy() reports
    // true on a schema this build does not understand (config_manager.h:122,
    // config_manager.cpp:225/239/244).
    QCOMPARE(schemaKey(), QStringLiteral("meta/schema_version"));
    // Exact key set: an at-current store is neither tidied nor added to.
    QStringList keys_after = store.allKeys();
    keys_after.sort();
    const QStringList expected_keys{QStringLiteral("backup/thread_count"),
                                    QStringLiteral("future/unknown_feature"),
                                    schemaKey()};
    QCOMPARE(keys_after, expected_keys);
    QCOMPARE(readAllBytes(path), before);
}

// Older-version read: the version is stamped forward to current and every
// pre-existing value is preserved (no silent data loss on upgrade).
void ConfigSchemaVersioningTests::reconcile_olderVersion_migratesAndPreservesValues() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("cfg.ini"));

    {
        QSettings seed(path, QSettings::IniFormat);
        // One version behind whatever the current build is (future-proof as the
        // constant grows). With current == 1 this is kNoSchemaVersion.
        seed.setValue(schemaKey(), ConfigManager::kCurrentSchemaVersion - 1);
        seed.setValue(QStringLiteral("backup/thread_count"), kBackupThreadProbe);
        seed.setValue(QStringLiteral("duplicate/keep_strategy"), QStringLiteral("newest"));
        // Both keys above are ones this build writes itself, so "no data loss" was only ever
        // probed against the allowlist. A migrate step that drops unrecognized keys -- the
        // exact line a future author edits when chaining a field migration in -- ships green.
        seed.setValue(QStringLiteral("future/unknown_feature"), kFutureFeatureProbe);
        seed.sync();
    }

    QSettings store(path, QSettings::IniFormat);
    const auto state = ConfigManager::reconcileSchemaVersion(store);

    QCOMPARE(static_cast<int>(state),
             static_cast<int>(ConfigManager::SchemaVersionState::Migrated));
    QCOMPARE(store.value(schemaKey()).toInt(), ConfigManager::kCurrentSchemaVersion);
    QCOMPARE(store.value(QStringLiteral("backup/thread_count")).toInt(), kBackupThreadProbe);
    QCOMPARE(store.value(QStringLiteral("duplicate/keep_strategy")).toString(),
             QStringLiteral("newest"));
    QCOMPARE(store.value(QStringLiteral("future/unknown_feature")).toInt(), kFutureFeatureProbe);
    // Stamping the version is the migration's ONLY write: nothing dropped, nothing invented.
    QStringList keys_after = store.allKeys();
    keys_after.sort();
    const QStringList expected_keys{QStringLiteral("backup/thread_count"),
                                    QStringLiteral("duplicate/keep_strategy"),
                                    QStringLiteral("future/unknown_feature"),
                                    schemaKey()};
    QCOMPARE(keys_after, expected_keys);
}

// A store predating schema versioning has no version key at all. It is treated
// as the oldest version, migrated forward, and its values preserved.
void ConfigSchemaVersioningTests::reconcile_legacyNoVersionKey_migratesAndPreservesValues() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("cfg.ini"));

    {
        QSettings seed(path, QSettings::IniFormat);
        seed.setValue(QStringLiteral("backup/thread_count"), kBackupThreadProbe);
        // A pre-versioning store is the one MOST likely to carry keys this build has never
        // heard of, so it is the worst case for an allowlist prune -- and the only seeded key
        // above is one the build defaults itself.
        seed.setValue(QStringLiteral("future/unknown_feature"), kFutureFeatureProbe);
        seed.sync();
        QVERIFY(!seed.contains(schemaKey()));
    }

    QSettings store(path, QSettings::IniFormat);
    const auto state = ConfigManager::reconcileSchemaVersion(store);

    QCOMPARE(static_cast<int>(state),
             static_cast<int>(ConfigManager::SchemaVersionState::Migrated));
    QCOMPARE(store.value(schemaKey()).toInt(), ConfigManager::kCurrentSchemaVersion);
    QCOMPARE(store.value(QStringLiteral("backup/thread_count")).toInt(), kBackupThreadProbe);
    QCOMPARE(store.value(QStringLiteral("future/unknown_feature")).toInt(), kFutureFeatureProbe);
    // Adding the version key is the migration's ONLY write.
    QStringList keys_after = store.allKeys();
    keys_after.sort();
    const QStringList expected_keys{QStringLiteral("backup/thread_count"),
                                    QStringLiteral("future/unknown_feature"),
                                    schemaKey()};
    QCOMPARE(keys_after, expected_keys);
}

// Newer-version read (a rollback opened a config a newer build wrote): the
// stored version is left untouched and every key -- including one this build
// does not recognize -- survives. No data is wiped or downgraded.
void ConfigSchemaVersioningTests::reconcile_newerVersion_preservesUnknownKeysAndVersion() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("cfg.ini"));

    {
        QSettings seed(path, QSettings::IniFormat);
        seed.setValue(schemaKey(), kFutureSchemaVersion);
        seed.setValue(QStringLiteral("backup/thread_count"), kBackupThreadProbe);
        seed.setValue(QStringLiteral("future/unknown_feature"), kFutureFeatureProbe);
        seed.sync();
    }
    // "FromFuture takes no write at all" is only falsifiable against the bytes on disk. The
    // prune direction was covered; the ADDITIVE direction was wide open -- stamping a
    // breadcrumb key here dirties and rewrites a config a NEWER build owns and this build has
    // just admitted it does not understand.
    // The bytes alone only catch a write that CHANGES the content: QSettings regenerates an
    // INI from its parsed map, so re-writing a key with the value it already holds reproduces
    // the file exactly (verified against Qt 6.10.3). A hand-added comment line is dropped by
    // that rewriter, so a same-value "normalize the stamp" touch -- the one write most likely
    // to be added blindly to the top of reconcileSchemaVersion, and the one this build must
    // NEVER perform on a newer build's store -- now shows up too.
    const QByteArray marker = QByteArrayLiteral("; sak-no-rewrite-marker\n");
    {
        QFile raw(path);
        QVERIFY(raw.open(QIODevice::Append));
        QCOMPARE(raw.write(marker), static_cast<qint64>(marker.size()));
    }
    const QByteArray before = readAllBytes(path);
    QVERIFY(before.endsWith(marker));

    QSettings store(path, QSettings::IniFormat);
    const auto state = ConfigManager::reconcileSchemaVersion(store);
    store.sync();

    QCOMPARE(static_cast<int>(state),
             static_cast<int>(ConfigManager::SchemaVersionState::FromFuture));
    // Version left intact so the newer build still recognizes its own store.
    QCOMPARE(store.value(schemaKey()).toInt(), kFutureSchemaVersion);
    // Known and unknown keys both preserved.
    QCOMPARE(store.value(QStringLiteral("backup/thread_count")).toInt(), kBackupThreadProbe);
    QCOMPARE(store.value(QStringLiteral("future/unknown_feature")).toInt(), kFutureFeatureProbe);
    QStringList keys_after = store.allKeys();
    keys_after.sort();
    const QStringList expected_keys{QStringLiteral("backup/thread_count"),
                                    QStringLiteral("future/unknown_feature"),
                                    schemaKey()};
    QCOMPARE(keys_after, expected_keys);
    QCOMPARE(readAllBytes(path), before);
}

// ============================================================================
// Singleton integration
// ============================================================================

// A freshly reset store is stamped at the current version and reports healthy.
void ConfigSchemaVersioningTests::singleton_freshStore_isCurrentVersionAndHealthy() {
    auto& mgr = ConfigManager::instance();
    mgr.resetToDefaults();

    QCOMPARE(mgr.storedSchemaVersion(), ConfigManager::kCurrentSchemaVersion);
    QVERIFY(mgr.isHealthy());
    // resetToDefaults() is clear -> reconcile -> initializeDefaults. Delete the last step and
    // BOTH assertions above still pass against a completely EMPTY store (reconcile stamped the
    // version, and isHealthy only checks status plus the version bound) -- and every other case
    // in this file leans on cleanup()'s resetToDefaults() to hand back a well-formed store, so
    // the whole file would stay green. Read with no getter-side default so a dropped key cannot
    // be masked by the getter's own fallback.
    QCOMPARE(mgr.getValue(QStringLiteral("backup/thread_count")).toInt(), 4);
    // The REST of initializeBackupAndOrganizerDefaults() (config_manager.cpp:119-136). Only
    // thread_count and keep_strategy were pinned, so deleting -- or flipping -- the other three
    // default writes ships green here, and in test_config_manager.cpp too, whose
    // backup/organizer/duplicate cases each SET a value first and never read a freshly reset
    // store.
    QVERIFY(mgr.contains(QStringLiteral("backup/verify_md5")));
    QCOMPARE(mgr.getValue(QStringLiteral("backup/verify_md5")).toBool(), true);
    QVERIFY(mgr.contains(QStringLiteral("organizer/preview_mode")));
    QCOMPARE(mgr.getValue(QStringLiteral("organizer/preview_mode")).toBool(), true);
    // 0 is also what an ABSENT key converts to, so contains() is what carries this one.
    QVERIFY(mgr.contains(QStringLiteral("duplicate/minimum_file_size")));
    QCOMPARE(mgr.getValue(QStringLiteral("duplicate/minimum_file_size")).toLongLong(), qint64{0});
    QCOMPARE(mgr.getValue(QStringLiteral("duplicate/keep_strategy")).toString(),
             QStringLiteral("oldest"));
    QCOMPARE(mgr.getValue(QStringLiteral("duplicate/keep_strategy")).toString(),
             QStringLiteral("oldest"));
    QCOMPARE(mgr.getValue(QStringLiteral("image_flasher/validation_mode")).toString(),
             QStringLiteral("full"));
    QCOMPARE(mgr.getValue(QStringLiteral("image_flasher/max_concurrent_writes")).toInt(), 1);
    QCOMPARE(mgr.getValue(QStringLiteral("ui/restore_window_geometry")).toBool(), true);
    // The retired key must not come back.
    QVERIFY(!mgr.contains(QStringLiteral("image_flasher/show_system_drive_warning")));
}

// Same-version write/read round-trip: a value persists and the schema version is
// unchanged, matching the pre-existing config behavior for current stores.
void ConfigSchemaVersioningTests::singleton_currentVersionRoundTrip_valueSurvives() {
    auto& mgr = ConfigManager::instance();
    mgr.setValue(QStringLiteral("test/roundtrip"), QStringLiteral("keep-me"));
    QVERIFY(mgr.sync());

    QCOMPARE(mgr.getValue(QStringLiteral("test/roundtrip")).toString(), QStringLiteral("keep-me"));
    QCOMPARE(mgr.storedSchemaVersion(), ConfigManager::kCurrentSchemaVersion);
    QVERIFY(mgr.isHealthy());
}

// A store that carries a newer schema version is reported unhealthy (surfaced
// via isHealthy()) yet loses no data -- the newer version and the surrounding
// values both remain readable.
void ConfigSchemaVersioningTests::singleton_newerVersion_isUnhealthyAndDataPreserved() {
    auto& mgr = ConfigManager::instance();
    mgr.resetToDefaults();
    mgr.setValue(QStringLiteral("test/precious"), kPreciseProbe);

    // Simulate a newer build having stamped this store.
    mgr.setValue(schemaKey(), kFutureSchemaVersion);

    QVERIFY(!mgr.isHealthy());
    QCOMPARE(mgr.storedSchemaVersion(), kFutureSchemaVersion);
    QCOMPARE(mgr.getValue(QStringLiteral("test/precious")).toInt(), kPreciseProbe);

    // Positive control. isHealthy() refuses through TWO independent guards -- a QSettings
    // status error OR a newer-than-current schema -- so !isHealthy() alone does not prove which
    // fired. The bound is deliberately ONE-SIDED (<=), so an older stamp must read healthy
    // again on the very same store. Without this leg, tightening the bound to == passes every
    // case in the file while a fail-closed health flag refuses every legacy/pre-versioning
    // store -- exactly the stores the header says must be accepted and migrated forward.
    mgr.setValue(schemaKey(), ConfigManager::kNoSchemaVersion);
    QCOMPARE(mgr.storedSchemaVersion(), ConfigManager::kNoSchemaVersion);
    QVERIFY(mgr.isHealthy());
    QCOMPARE(mgr.getValue(QStringLiteral("test/precious")).toInt(), kPreciseProbe);
}

QTEST_GUILESS_MAIN(ConfigSchemaVersioningTests)
#include "test_config_schema_versioning.moc"

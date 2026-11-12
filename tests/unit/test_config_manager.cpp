// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_config_manager.cpp
/// @brief Unit tests for ConfigManager singleton -- load, save, defaults, retired settings

#include "sak/app_paths.h"
#include "sak/config_manager.h"
#include "sak/portable_settings.h"

#include "../support/junction_fixture.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <QVariant>

#include <windows.h>

#include <aclapi.h>

class ConfigManagerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void portableSettingsLiveInTheDataFolder();
    void noSourceWritesSettingsToTheRegistry();
    void dataRootHasNoSilentFallback();
    void runTempIsProcessWideAndProtected();
    void staleRunFoldersNeverFollowAJunction();
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
    void resetToDefaultsForTesting_restoresValues();
    void removeRetiredKeys_dropsOnlyGoneSettings();
    void removeRetiredKeys_writesNothingWithoutThem();

    // Typed accessors -- image flasher
    void imageFlasherValidationMode_setGet();
    void imageFlasherBufferSize_setGet();
    void imageFlasherDefaults_doNotRecreateRetiredSystemDriveWarning();
    void themePreference_setGetAndUnknownReadsAsSystem();

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
    sak::ConfigManager::instance().resetToDefaultsForTesting();
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

void ConfigManagerTests::resetToDefaultsForTesting_restoresValues() {
    auto& mgr = sak::ConfigManager::instance();

    // Override a known default. Read the raw key: the getter's own fallback is also "system".
    mgr.setThemePreference(QStringLiteral("dark"));
    QCOMPARE(mgr.getValue("ui/theme").toString(), QStringLiteral("dark"));

    mgr.resetToDefaultsForTesting();

    QVERIFY(mgr.contains("ui/theme"));
    QCOMPARE(mgr.getValue("ui/theme").toString(), QStringLiteral("system"));
}

// Settings that are gone: the SettingsDialog nothing opened, User Migration's popup whose values
// no backup read, defaults nothing read, and a flag nothing set.
namespace {
const QStringList& retiredKeysForTest() {
    static const QStringList keys{QStringLiteral("image_flasher/show_system_drive_warning"),
                                  QStringLiteral("backup/thread_count"),
                                  QStringLiteral("backup/verify_md5"),
                                  QStringLiteral("backup/last_location"),
                                  QStringLiteral("organizer/preview_mode"),
                                  QStringLiteral("duplicate/minimum_file_size"),
                                  QStringLiteral("duplicate/keep_strategy"),
                                  QStringLiteral("ui/restore_window_geometry"),
                                  QStringLiteral("quick_actions/backup_location"),
                                  QStringLiteral("quick_actions/confirm_before_execute"),
                                  QStringLiteral("quick_actions/show_notifications"),
                                  QStringLiteral("quick_actions/enable_logging"),
                                  QStringLiteral("quick_actions/compress_backups")};
    return keys;
}
}  // namespace

// The real load path: a Utility.ini written by an older build still carries the retired keys;
// constructing the store (the app's first instance() call) drops them and keeps the rest. Runs
// before any test function, because cleanup() constructs the singleton after the first one.
void ConfigManagerTests::initTestCase() {
    {
        QSettings seed(sak::app_paths::configFilePath(), QSettings::IniFormat);
        for (const QString& key : retiredKeysForTest()) {
            seed.setValue(key, QStringLiteral("old"));
        }
        seed.setValue(QStringLiteral("ui/theme"), QStringLiteral("dark"));
        seed.sync();
        QCOMPARE(seed.status(), QSettings::NoError);
    }
    auto& mgr = sak::ConfigManager::instance();
    for (const QString& key : retiredKeysForTest()) {
        QVERIFY2(!mgr.contains(key), qPrintable(key));
    }
    QCOMPARE(mgr.getValue("ui/theme").toString(), QStringLiteral("dark"));
}

void ConfigManagerTests::removeRetiredKeys_dropsOnlyGoneSettings() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings store(dir.filePath(QStringLiteral("cfg.ini")), QSettings::IniFormat);
    for (const QString& key : retiredKeysForTest()) {
        store.setValue(key, 1);
    }
    store.setValue(QStringLiteral("image_flasher/buffer_size"), 2048);
    store.setValue(QStringLiteral("advsearch/max_results"), 50);
    store.setValue(QStringLiteral("future/unknown_feature"), 9001);

    sak::ConfigManager::removeRetiredKeys(store);

    QStringList left = store.allKeys();
    left.sort();
    QCOMPARE(left,
             (QStringList{QStringLiteral("advsearch/max_results"),
                          QStringLiteral("future/unknown_feature"),
                          QStringLiteral("image_flasher/buffer_size")}));
}

// A store with no retired keys is not rewritten: a comment line, which QSettings drops whenever
// it rewrites the file, survives.
void ConfigManagerTests::removeRetiredKeys_writesNothingWithoutThem() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("cfg.ini"));
    {
        QSettings seed(path, QSettings::IniFormat);
        seed.setValue(QStringLiteral("ui/theme"), QStringLiteral("light"));
        seed.sync();
    }
    const QByteArray marker = QByteArrayLiteral("; sak-no-rewrite-marker\n");
    {
        QFile raw(path);
        QVERIFY(raw.open(QIODevice::Append));
        QCOMPARE(raw.write(marker), static_cast<qint64>(marker.size()));
    }
    QFile before_file(path);
    QVERIFY(before_file.open(QIODevice::ReadOnly));
    const QByteArray before = before_file.readAll();
    before_file.close();

    {
        QSettings store(path, QSettings::IniFormat);
        sak::ConfigManager::removeRetiredKeys(store);
        store.sync();
    }

    QFile after_file(path);
    QVERIFY(after_file.open(QIODevice::ReadOnly));
    QCOMPARE(after_file.readAll(), before);
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

// The one theme setting: "system" (follow Windows), "light" or "dark". An unknown stored value
// reads as the documented default and the setter refuses one instead of storing it.
void ConfigManagerTests::themePreference_setGetAndUnknownReadsAsSystem() {
    auto& mgr = sak::ConfigManager::instance();
    mgr.setThemePreference(QStringLiteral("dark"));
    QCOMPARE(mgr.getThemePreference(), QStringLiteral("dark"));
    QCOMPARE(mgr.getValue("ui/theme").toString(), QStringLiteral("dark"));
    mgr.setThemePreference(QStringLiteral("purple"));
    QCOMPARE(mgr.getValue("ui/theme").toString(), QStringLiteral("dark"));
    mgr.setValue("ui/theme", QStringLiteral("purple"));
    QCOMPARE(mgr.getThemePreference(), QStringLiteral("system"));
    mgr.remove("ui/theme");
    QCOMPARE(mgr.getThemePreference(), QStringLiteral("system"));
    mgr.setThemePreference(QStringLiteral("light"));
    QCOMPARE(mgr.getThemePreference(), QStringLiteral("light"));
    mgr.setThemePreference(QStringLiteral("system"));
}

void ConfigManagerTests::imageFlasherBufferSize_setGet() {
    auto& mgr = sak::ConfigManager::instance();
    mgr.setImageFlasherBufferSize(256);
    QCOMPARE(mgr.getImageFlasherBufferSize(), 256);
    // 4096 was the old default, written to every install's settings as if it were megabytes. The
    // dialog cannot show it and the flasher refuses it, so it reads back as the real default.
    mgr.setValue("image_flasher/buffer_size", 4096);
    QCOMPARE(mgr.getImageFlasherBufferSize(), sak::kImageFlasherDefaultBufferMb);
}

// image_flasher/show_system_drive_warning was retired: the system-drive block is
// unconditional, so a switch that claimed to control it described protection the
// user could not actually turn off. Nothing may put it back -- a default for it
// would resurrect the control in every config file.
void ConfigManagerTests::imageFlasherDefaults_doNotRecreateRetiredSystemDriveWarning() {
    auto& mgr = sak::ConfigManager::instance();
    mgr.resetToDefaultsForTesting();

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
    // The default is a size in megabytes the settings dialog can show and the flasher accepts.
    // network_constants.h.
    QCOMPARE(mgr.getValue("image_flasher/buffer_size").toInt(), sak::kImageFlasherDefaultBufferMb);
    QCOMPARE(sak::kImageFlasherDefaultBufferMb, 64);
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
    // A non-positive buffer reclamps to the default.
    QCOMPARE(mgr.getImageFlasherBufferSize(), sak::kImageFlasherDefaultBufferMb);
    // The buffer guard has TWO arms: the magnitude cap (kMaxImageFlasherBufferSizeMb, the 512 MB
    // the settings dialog offers) is what stops an absurd allocation request from a hand-edited
    // INI. Bracket it: 512 is in range and passes through, 513 is over and reclamps.
    mgr.setValue("image_flasher/buffer_size", 512);
    QCOMPARE(mgr.getImageFlasherBufferSize(), 512);
    mgr.setValue("image_flasher/buffer_size", 513);
    QCOMPARE(mgr.getImageFlasherBufferSize(), sak::kImageFlasherDefaultBufferMb);

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
    mgr.resetToDefaultsForTesting();
    mgr.setValue("test/clear_me", "value");
    // A key in no group this build knows about. "Removes ALL keys" is otherwise
    // unfalsifiable: a clear that walks the groups it recognizes passes a probe
    // that lives inside one of them.
    mgr.setValue("foreign/unknown_feature", 9001);
    QVERIFY(mgr.contains("test/clear_me"));
    QVERIFY(mgr.contains("ui/theme"));
    QCOMPARE(mgr.storedSchemaVersion(), sak::ConfigManager::kCurrentSchemaVersion);

    mgr.clearForTesting();
    QVERIFY(!mgr.contains("test/clear_me"));
    QVERIFY(!mgr.contains("foreign/unknown_feature"));
    QVERIFY(!mgr.contains("ui/theme"));
    // The meta schema stamp goes too: resetToDefaultsForTesting() re-stamps it right after
    // the clear, and a stamp that survives leaves a newer-than-current store permanently
    // unhealthy across a reset.
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

// A default-constructed QSettings writes HKEY_CURRENT_USER. UI state now goes to an INI file in
// the portable data folder, beside (not inside) the ConfigManager store.
void ConfigManagerTests::portableSettingsLiveInTheDataFolder() {
    const sak::PortableSettings settings;
    QCOMPARE(settings.format(), QSettings::IniFormat);
    QCOMPARE(QDir::cleanPath(settings.fileName()),
             QDir::cleanPath(sak::app_paths::uiStateFilePath()));
    QVERIFY(QDir::cleanPath(settings.fileName())
                .startsWith(QDir::cleanPath(sak::app_paths::dataRoot()) + QLatin1Char('/')));
    QVERIFY(QDir::cleanPath(settings.fileName()) !=
            QDir::cleanPath(sak::app_paths::configFilePath()));
}

void ConfigManagerTests::noSourceWritesSettingsToTheRegistry() {
    // "QSettings name;" (default format = registry) and QSettings("Org", "App") (always native,
    // i.e. registry). Reading another program's registry key by explicit path stays allowed.
    const QRegularExpression bare(QStringLiteral("\\bQSettings\\s+\\w+\\s*;"));
    const QRegularExpression org_app(
        QStringLiteral("\\bQSettings\\s+\\w+\\s*\\(\\s*\"[^\"]*\"\\s*,"));
    QStringList offenders;
    QDirIterator it(QStringLiteral(SAK_REPO_ROOT "/src"),
                    {QStringLiteral("*.cpp"), QStringLiteral("*.h")},
                    QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(file.readAll());
        if (bare.match(text).hasMatch() || org_app.match(text).hasMatch()) {
            offenders.append(path);
        }
    }
    QVERIFY2(offenders.isEmpty(), qPrintable(offenders.join(QLatin1Char('\n'))));
}

// The data root used to fall back silently to %LOCALAPPDATA% when data/ beside the exe was not
// writable, so a "portable" tool left its settings and logs on the host without saying so.
void ConfigManagerTests::dataRootHasNoSilentFallback() {
    const auto writable = [](const QString&) {
        return true;
    };
    const auto unwritable = [](const QString&) {
        return false;
    };
    const QString app = QStringLiteral("D:/Tools/SAK");
    const QString per_user = QStringLiteral("E:/PerUserData/SAK");
    const auto portable = sak::app_paths::decideDataRoot(false, app, per_user, writable);
    QVERIFY(portable.has_value());
    QCOMPARE(QDir::cleanPath(*portable), QStringLiteral("D:/Tools/SAK/data"));
    const auto refused = sak::app_paths::decideDataRoot(false, app, per_user, unwritable);
    QVERIFY2(!refused.has_value(), "an unwritable portable folder must not fall back");
    QVERIFY(refused.error().contains(QStringLiteral("not writable")));
    const auto packaged = sak::app_paths::decideDataRoot(true, app, per_user, unwritable);
    QVERIFY(packaged.has_value());
    QCOMPARE(*packaged, per_user);
    // Decided once: the same answer every time.
    QCOMPARE(sak::app_paths::dataRoot(), sak::app_paths::dataRoot());
}

// TMP and TEMP point at this run's own folder under data\temp for the whole process (owner,
// 2026-11-11), so QDir::tempPath() and every child land in the program's folder.
void ConfigManagerTests::runTempIsProcessWideAndProtected() {
    // The cases after this one make their own temporary folders, so the variables go back after.
    const QByteArray original_tmp = qgetenv("TMP");
    const QByteArray original_temp = qgetenv("TEMP");
    const auto restore = qScopeGuard([&] {
        qputenv("TMP", original_tmp);
        qputenv("TEMP", original_temp);
    });
    const auto established = sak::app_paths::establishSessionTempDirectory();
    QVERIFY2(established, qPrintable(established ? QString() : established.error()));
    const QString run = QDir::cleanPath(QDir::fromNativeSeparators(*established));
    QVERIFY(QRegularExpression(
                QStringLiteral("^run-%1-[0-9a-f]{32}$").arg(QCoreApplication::applicationPid()))
                .match(QFileInfo(run).fileName())
                .hasMatch());
    QVERIFY(run.startsWith(QDir::cleanPath(sak::app_paths::tempDirectory()) + QLatin1Char('/'),
                           Qt::CaseInsensitive));
    QCOMPARE(QDir::cleanPath(QDir::fromNativeSeparators(qEnvironmentVariable("TEMP"))), run);
    QCOMPARE(QDir::cleanPath(QDir::fromNativeSeparators(qEnvironmentVariable("TMP"))), run);
    QCOMPARE(QDir::cleanPath(QDir::tempPath()), run);
    const QProcessEnvironment child =
        sak::app_paths::withSessionTemp(QProcessEnvironment::systemEnvironment());
    QCOMPARE(QDir::cleanPath(QDir::fromNativeSeparators(child.value(QStringLiteral("TEMP")))), run);
    // A second call keeps the same folder.
    QCOMPARE(QDir::cleanPath(QDir::fromNativeSeparators(
                 sak::app_paths::establishSessionTempDirectory().value_or(QString()))),
             run);

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    QCOMPARE(GetNamedSecurityInfoW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(run).utf16()),
                                   SE_FILE_OBJECT,
                                   DACL_SECURITY_INFORMATION,
                                   nullptr,
                                   nullptr,
                                   &dacl,
                                   nullptr,
                                   &descriptor),
             static_cast<DWORD>(ERROR_SUCCESS));
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    QVERIFY(GetSecurityDescriptorControl(descriptor, &control, &revision) != 0);
    LocalFree(descriptor);
    QVERIFY2((control & SE_DACL_PROTECTED) != 0, "the run folder inherits other accounts' access");

    QFile scratch(QDir(run).filePath(QStringLiteral("scratch.txt")));
    QVERIFY(scratch.open(QIODevice::WriteOnly));
    scratch.close();
    sak::app_paths::removeSessionTempDirectory();
    QVERIFY(!QDir(run).exists());

    const auto alive = [](qint64 pid) {
        return pid == 200;
    };
    const QString hex(32, QLatin1Char('a'));
    QCOMPARE(sak::app_paths::staleSessionTempDirectories({QStringLiteral("session-100"),
                                                          QStringLiteral("session-200"),
                                                          QStringLiteral("session-300"),
                                                          QStringLiteral("run-300-") + hex,
                                                          QStringLiteral("run-200-") + hex,
                                                          QStringLiteral("run-300-short"),
                                                          QStringLiteral("uup-build"),
                                                          QStringLiteral("session-x")},
                                                         100,
                                                         alive),
             (QStringList{QStringLiteral("session-300"), QStringLiteral("run-300-") + hex}));
}

// A run folder left by a process that is gone is removed at the next start. One that is a junction
// (or holds one) is removed as the junction itself: what it points at is never touched, even when
// this runs as administrator (security brief 4a.1).
void ConfigManagerTests::staleRunFoldersNeverFollowAJunction() {
    QTemporaryDir outside;
    QVERIFY(outside.isValid());
    const QString canary = outside.filePath(QStringLiteral("canary"));
    const QByteArray canary_bytes = QByteArrayLiteral("must survive the cleanup");
    QVERIFY(sak::test::createCanary(canary, QStringLiteral("keep.txt"), canary_bytes));

    const QString temp = sak::app_paths::tempDirectory();
    QVERIFY(QDir().mkpath(temp));
    // Process ids are multiples of four, so these never name a running process.
    const QString hex(32, QLatin1Char('b'));
    const QString junction_session = QDir(temp).filePath(QStringLiteral("session-999999"));
    const QString junction_run = QDir(temp).filePath(QStringLiteral("run-999998-") + hex);
    const QString real_session = QDir(temp).filePath(QStringLiteral("session-999997"));
    QVERIFY(sak::test::createJunction(junction_session, canary));
    QVERIFY(sak::test::createJunction(junction_run, canary));
    QVERIFY(QDir().mkpath(real_session));
    QVERIFY(
        sak::test::createJunction(QDir(real_session).filePath(QStringLiteral("inner")), canary));

    QVERIFY(sak::app_paths::removeStaleSessionTempDirectories() >= 3);

    QVERIFY(sak::test::canaryIntact(canary, QStringLiteral("keep.txt"), canary_bytes));
    QVERIFY(!QFileInfo::exists(junction_session) && !QFileInfo(junction_session).isJunction());
    QVERIFY(!QFileInfo(junction_run).isJunction());
    QVERIFY(!QFileInfo::exists(real_session));
}

QTEST_GUILESS_MAIN(ConfigManagerTests)
#include "test_config_manager.moc"

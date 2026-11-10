// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/error_codes.h"

#include <QObject>
#include <QSettings>
#include <QString>
#include <QVariant>

#include <expected>
#include <memory>

namespace sak {

/**
 * @brief Configuration manager for application settings
 *
 * Provides:
 * - QSettings-based persistence
 * - Default values
 * - Type-safe access
 * - Settings validation
 * - Reset to defaults
 */
/// Default Image Flasher read/write buffer, in MB. Large enough for USB and SD throughput without
/// holding a lot of memory per drive.
inline constexpr int kImageFlasherDefaultBufferMb = 64;

class ConfigManager : public QObject {
    Q_OBJECT

public:
    /**
     * @brief Get singleton instance
     * @return ConfigManager instance
     */
    static ConfigManager& instance();

    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;
    ConfigManager(ConfigManager&&) = delete;
    ConfigManager& operator=(ConfigManager&&) = delete;

    /**
     * @brief Get configuration value
     * @param key Configuration key
     * @param default_value Default value if key not found
     * @return Configuration value
     */
    [[nodiscard]] QVariant getValue(const QString& key,
                                    const QVariant& default_value = QVariant()) const;

    /**
     * @brief Set configuration value
     * @param key Configuration key
     * @param value Configuration value
     */
    void setValue(const QString& key, const QVariant& value);

    /**
     * @brief Check if key exists
     * @param key Configuration key
     * @return True if key exists
     */
    [[nodiscard]] bool contains(const QString& key) const;

    /**
     * @brief Remove configuration key
     * @param key Configuration key
     */
    void remove(const QString& key);

#ifdef SAK_CORE_TEST_HOOKS
    /**
     * @brief Test-only: remove every key, the schema stamp included.
     */
    void clearForTesting();

    /**
     * @brief Test-only: clear the store, re-stamp the schema version and write the defaults,
     *        so a suite hands the next case a well-formed store.
     *
     * The product has no reset or clear (the settings dialog that offered them is gone), so both
     * are compiled only into the config suites (tests/CMakeLists.txt,
     * SAK_CORE_CONFIG_TEST_HOOK_SOURCES), and test_release_binary_test_hooks keeps them out of
     * sak_core.
     */
    void resetToDefaultsForTesting();
#endif

    /**
     * @brief Sync settings to disk.
     * @return true if the store is healthy after the flush; false if QSettings
     *         reported an access/format error (the persist did not fully succeed).
     * @note Previously this ignored QSettings::status(), so a failed write to
     *       disk (permission/full-disk) looked like a successful save.
     */
    bool sync();

    /**
     * @brief Whether the settings store is currently healthy.
     * @return true iff QSettings::status() == NoError (no access/format error)
     *         AND the persisted schema version is not newer than this build's
     *         (see kCurrentSchemaVersion). A config stamped by a newer build is
     *         reported unhealthy so callers fail closed rather than risk
     *         operating on a schema they do not understand; its data is never
     *         altered or wiped (see reconcileSchemaVersion).
     */
    [[nodiscard]] bool isHealthy() const;

    /**
     * @brief Human-readable message for a QSettings status (empty for NoError).
     * @note Pure + static for unit testing.
     */
    [[nodiscard]] static QString describeSettingsStatus(QSettings::Status status);

    // ------------------------------------------------------------------
    // Schema versioning
    // ------------------------------------------------------------------

    /// Schema version this build writes. Bump when a persisted field's meaning
    /// changes so an older config can be migrated forward and a newer config
    /// (a rollback read this store) can be detected instead of corrupted.
    static constexpr int kCurrentSchemaVersion = 1;

    /// Sentinel for a store that predates schema versioning (key absent). Such a
    /// store is treated as the oldest version and migrated forward on load.
    static constexpr int kNoSchemaVersion = 0;

    /// Key under which the persisted schema version lives.
    static constexpr char kSchemaVersionKey[] = "meta/schema_version";

    /// Outcome of reconciling a persisted schema version against this build.
    enum class SchemaVersionState {
        Current,    ///< Stored == current: no change, byte-identical read.
        Migrated,   ///< Stored < current: stamped forward, all values preserved.
        FromFuture  ///< Stored > current: newer build wrote it; preserved, unhealthy.
    };

    /// Reconcile the persisted schema version against kCurrentSchemaVersion.
    /// - Stored == current: no write (Current).
    /// - Stored < current (or absent): every existing value is left untouched and
    ///   the current version is stamped (Migrated) -- never any data loss.
    /// - Stored > current: every key is preserved (including ones this build does
    ///   not recognize) and the stored version is left intact so the newer build
    ///   still reads it (FromFuture); isHealthy() then reports the mismatch.
    /// Pure + static for unit testing: it touches only `settings`, never globals.
    [[nodiscard]] static SchemaVersionState reconcileSchemaVersion(QSettings& settings);

    /// Persisted schema version, or kNoSchemaVersion if the key is absent.
    [[nodiscard]] int storedSchemaVersion() const;

    /// Remove the keys of settings this build no longer has (each listed with its reason in
    /// config_manager.cpp), so a stored value cannot outlive the control it described. Writes
    /// only when a retired key is present. Runs with the defaults on every load and reset.
    /// Pure + static for unit testing: it touches only `settings`.
    static void removeRetiredKeys(QSettings& settings);

    // Image Flasher settings
    [[nodiscard]] QString getImageFlasherValidationMode() const;
    void setImageFlasherValidationMode(const QString& mode);

    [[nodiscard]] int getImageFlasherBufferSize() const;
    void setImageFlasherBufferSize(int size);

    [[nodiscard]] bool getImageFlasherUnmountOnCompletion() const;
    void setImageFlasherUnmountOnCompletion(bool unmount);

    [[nodiscard]] bool getImageFlasherShowLargeDriveWarning() const;
    void setImageFlasherShowLargeDriveWarning(bool show);

    [[nodiscard]] int getImageFlasherLargeDriveThreshold() const;
    void setImageFlasherLargeDriveThreshold(int threshold);

    [[nodiscard]] int getImageFlasherMaxConcurrentWrites() const;
    void setImageFlasherMaxConcurrentWrites(int max);

    [[nodiscard]] bool getImageFlasherEnableNotifications() const;
    void setImageFlasherEnableNotifications(bool enable);

    // UI settings
    [[nodiscard]] QByteArray getWindowGeometry() const;
    void setWindowGeometry(const QByteArray& geometry);

    [[nodiscard]] QByteArray getWindowState() const;
    void setWindowState(const QByteArray& state);

    /// The app theme: "system" (follow Windows), "light" or "dark". A stored value outside that
    /// set reads as "system"; the setter refuses one.
    [[nodiscard]] QString getThemePreference() const;
    void setThemePreference(const QString& preference);

private:
    explicit ConfigManager(QObject* parent = nullptr);
    ~ConfigManager() override = default;

    void initializeDefaults();
    void initializeFlasherDefaults();
    void initializeUiDefaults();

    std::unique_ptr<QSettings> m_settings;
};

}  // namespace sak

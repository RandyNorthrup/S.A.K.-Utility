#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include <QVariant>
#include <memory>
#include <expected>
#include "sak/error_codes.h"

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
    [[nodiscard]] QVariant getValue(const QString& key, const QVariant& default_value = QVariant()) const;

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

    /**
     * @brief Clear all settings
     */
    void clear();

    /**
     * @brief Reset to default values
     */
    void resetToDefaults();

    /**
     * @brief Sync settings to disk
     */
    void sync();

    // Backup settings
    [[nodiscard]] int getBackupThreadCount() const;
    void setBackupThreadCount(int count);

    [[nodiscard]] bool getBackupVerifyMD5() const;
    void setBackupVerifyMD5(bool verify);

    [[nodiscard]] QString getLastBackupLocation() const;
    void setLastBackupLocation(const QString& path);

    // Organizer settings
    [[nodiscard]] bool getOrganizerPreviewMode() const;
    void setOrganizerPreviewMode(bool preview);

    // Duplicate finder settings
    [[nodiscard]] qint64 getDuplicateMinimumFileSize() const;
    void setDuplicateMinimumFileSize(qint64 size);

    [[nodiscard]] QString getDuplicateKeepStrategy() const;
    void setDuplicateKeepStrategy(const QString& strategy);

    // License scanner settings
    [[nodiscard]] bool getLicenseScanRegistry() const;
    void setLicenseScanRegistry(bool scan);

    [[nodiscard]] bool getLicenseScanFilesystem() const;
    void setLicenseScanFilesystem(bool scan);

    // UI settings
    [[nodiscard]] bool getRestoreWindowGeometry() const;
    void setRestoreWindowGeometry(bool restore);

    [[nodiscard]] QByteArray getWindowGeometry() const;
    void setWindowGeometry(const QByteArray& geometry);

    [[nodiscard]] QByteArray getWindowState() const;
    void setWindowState(const QByteArray& state);

Q_SIGNALS:
    /**
     * @brief Emitted when a setting changes
     * @param key Configuration key
     * @param value New value
     */
    void settingChanged(const QString& key, const QVariant& value);

private:
    explicit ConfigManager(QObject* parent = nullptr);
    ~ConfigManager() override = default;

    void initializeDefaults();

    std::unique_ptr<QSettings> m_settings;
};

} // namespace sak

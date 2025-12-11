// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/config_manager.h"
#include "sak/logger.h"
#include <QCoreApplication>

namespace sak {

ConfigManager& ConfigManager::instance()
{
    static ConfigManager instance;
    return instance;
}

ConfigManager::ConfigManager(QObject* parent)
    : QObject(parent)
    , m_settings(std::make_unique<QSettings>(
        QSettings::IniFormat,
        QSettings::UserScope,
        "SAK",
        "Utility"
    ))
{
    log_info("ConfigManager initialized: {}", m_settings->fileName().toStdString());
    initializeDefaults();
}

void ConfigManager::initializeDefaults()
{
    // Only set defaults if keys don't exist
    if (!contains("backup/thread_count")) {
        setValue("backup/thread_count", 4);
    }
    if (!contains("backup/verify_md5")) {
        setValue("backup/verify_md5", true);
    }
    if (!contains("organizer/preview_mode")) {
        setValue("organizer/preview_mode", true);
    }
    if (!contains("duplicate/minimum_file_size")) {
        setValue("duplicate/minimum_file_size", 0);
    }
    if (!contains("duplicate/keep_strategy")) {
        setValue("duplicate/keep_strategy", "oldest");
    }
    if (!contains("license/scan_registry")) {
        setValue("license/scan_registry", true);
    }
    if (!contains("license/scan_filesystem")) {
        setValue("license/scan_filesystem", true);
    }
    if (!contains("ui/restore_window_geometry")) {
        setValue("ui/restore_window_geometry", true);
    }
}

QVariant ConfigManager::getValue(const QString& key, const QVariant& default_value) const
{
    return m_settings->value(key, default_value);
}

void ConfigManager::setValue(const QString& key, const QVariant& value)
{
    m_settings->setValue(key, value);
    Q_EMIT settingChanged(key, value);
}

bool ConfigManager::contains(const QString& key) const
{
    return m_settings->contains(key);
}

void ConfigManager::remove(const QString& key)
{
    m_settings->remove(key);
}

void ConfigManager::clear()
{
    m_settings->clear();
    log_info("All settings cleared");
}

void ConfigManager::resetToDefaults()
{
    clear();
    initializeDefaults();
    log_info("Settings reset to defaults");
}

void ConfigManager::sync()
{
    m_settings->sync();
}

// Backup settings
int ConfigManager::getBackupThreadCount() const
{
    return getValue("backup/thread_count", 4).toInt();
}

void ConfigManager::setBackupThreadCount(int count)
{
    setValue("backup/thread_count", count);
}

bool ConfigManager::getBackupVerifyMD5() const
{
    return getValue("backup/verify_md5", true).toBool();
}

void ConfigManager::setBackupVerifyMD5(bool verify)
{
    setValue("backup/verify_md5", verify);
}

QString ConfigManager::getLastBackupLocation() const
{
    return getValue("backup/last_location", QString()).toString();
}

void ConfigManager::setLastBackupLocation(const QString& path)
{
    setValue("backup/last_location", path);
}

// Organizer settings
bool ConfigManager::getOrganizerPreviewMode() const
{
    return getValue("organizer/preview_mode", true).toBool();
}

void ConfigManager::setOrganizerPreviewMode(bool preview)
{
    setValue("organizer/preview_mode", preview);
}

// Duplicate finder settings
qint64 ConfigManager::getDuplicateMinimumFileSize() const
{
    return getValue("duplicate/minimum_file_size", 0).toLongLong();
}

void ConfigManager::setDuplicateMinimumFileSize(qint64 size)
{
    setValue("duplicate/minimum_file_size", size);
}

QString ConfigManager::getDuplicateKeepStrategy() const
{
    return getValue("duplicate/keep_strategy", "oldest").toString();
}

void ConfigManager::setDuplicateKeepStrategy(const QString& strategy)
{
    setValue("duplicate/keep_strategy", strategy);
}

// License scanner settings
bool ConfigManager::getLicenseScanRegistry() const
{
    return getValue("license/scan_registry", true).toBool();
}

void ConfigManager::setLicenseScanRegistry(bool scan)
{
    setValue("license/scan_registry", scan);
}

bool ConfigManager::getLicenseScanFilesystem() const
{
    return getValue("license/scan_filesystem", true).toBool();
}

void ConfigManager::setLicenseScanFilesystem(bool scan)
{
    setValue("license/scan_filesystem", scan);
}

// UI settings
bool ConfigManager::getRestoreWindowGeometry() const
{
    return getValue("ui/restore_window_geometry", true).toBool();
}

void ConfigManager::setRestoreWindowGeometry(bool restore)
{
    setValue("ui/restore_window_geometry", restore);
}

QByteArray ConfigManager::getWindowGeometry() const
{
    return getValue("ui/window_geometry", QByteArray()).toByteArray();
}

void ConfigManager::setWindowGeometry(const QByteArray& geometry)
{
    setValue("ui/window_geometry", geometry);
}

QByteArray ConfigManager::getWindowState() const
{
    return getValue("ui/window_state", QByteArray()).toByteArray();
}

void ConfigManager::setWindowState(const QByteArray& state)
{
    setValue("ui/window_state", state);
}

} // namespace sak

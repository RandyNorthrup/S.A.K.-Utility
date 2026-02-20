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
    logInfo("ConfigManager initialized: {}", m_settings->fileName().toStdString());
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
    if (!contains("image_flasher/validation_mode")) {
        setValue("image_flasher/validation_mode", "full");
    }
    if (!contains("image_flasher/buffer_size")) {
        setValue("image_flasher/buffer_size", 4096);
    }
    if (!contains("image_flasher/unmount_on_completion")) {
        setValue("image_flasher/unmount_on_completion", true);
    }
    if (!contains("image_flasher/show_system_drive_warning")) {
        setValue("image_flasher/show_system_drive_warning", true);
    }
    if (!contains("image_flasher/show_large_drive_warning")) {
        setValue("image_flasher/show_large_drive_warning", true);
    }
    if (!contains("image_flasher/large_drive_threshold")) {
        setValue("image_flasher/large_drive_threshold", 128);
    }
    if (!contains("image_flasher/max_concurrent_writes")) {
        setValue("image_flasher/max_concurrent_writes", 1);
    }
    if (!contains("image_flasher/enable_notifications")) {
        setValue("image_flasher/enable_notifications", true);
    }
    if (!contains("network_transfer/enabled")) {
        setValue("network_transfer/enabled", true);
    }
    if (!contains("network_transfer/discovery_port")) {
        setValue("network_transfer/discovery_port", 54321);
    }
    if (!contains("network_transfer/control_port")) {
        setValue("network_transfer/control_port", 54322);
    }
    if (!contains("network_transfer/data_port")) {
        setValue("network_transfer/data_port", 54323);
    }
    if (!contains("network_transfer/encryption")) {
        setValue("network_transfer/encryption", true);
    }
    if (!contains("network_transfer/compression")) {
        setValue("network_transfer/compression", true);
    }
    if (!contains("network_transfer/resume")) {
        setValue("network_transfer/resume", true);
    }
    if (!contains("network_transfer/max_bandwidth")) {
        setValue("network_transfer/max_bandwidth", 0);
    }
    if (!contains("network_transfer/auto_discovery")) {
        setValue("network_transfer/auto_discovery", true);
    }
    if (!contains("network_transfer/chunk_size")) {
        setValue("network_transfer/chunk_size", 65536);
    }
    if (!contains("network_transfer/relay_server")) {
        setValue("network_transfer/relay_server", QString());
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
    logInfo("All settings cleared");
}

void ConfigManager::resetToDefaults()
{
    clear();
    initializeDefaults();
    logInfo("Settings reset to defaults");
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

// Image Flasher settings
QString ConfigManager::getImageFlasherValidationMode() const
{
    return getValue("image_flasher/validation_mode", "full").toString();
}

void ConfigManager::setImageFlasherValidationMode(const QString& mode)
{
    setValue("image_flasher/validation_mode", mode);
}

int ConfigManager::getImageFlasherBufferSize() const
{
    return getValue("image_flasher/buffer_size", 4096).toInt();
}

void ConfigManager::setImageFlasherBufferSize(int size)
{
    setValue("image_flasher/buffer_size", size);
}

bool ConfigManager::getImageFlasherUnmountOnCompletion() const
{
    return getValue("image_flasher/unmount_on_completion", true).toBool();
}

void ConfigManager::setImageFlasherUnmountOnCompletion(bool unmount)
{
    setValue("image_flasher/unmount_on_completion", unmount);
}

bool ConfigManager::getImageFlasherShowSystemDriveWarning() const
{
    return getValue("image_flasher/show_system_drive_warning", true).toBool();
}

void ConfigManager::setImageFlasherShowSystemDriveWarning(bool show)
{
    setValue("image_flasher/show_system_drive_warning", show);
}

bool ConfigManager::getImageFlasherShowLargeDriveWarning() const
{
    return getValue("image_flasher/show_large_drive_warning", true).toBool();
}

void ConfigManager::setImageFlasherShowLargeDriveWarning(bool show)
{
    setValue("image_flasher/show_large_drive_warning", show);
}

int ConfigManager::getImageFlasherLargeDriveThreshold() const
{
    return getValue("image_flasher/large_drive_threshold", 128).toInt();
}

void ConfigManager::setImageFlasherLargeDriveThreshold(int threshold)
{
    setValue("image_flasher/large_drive_threshold", threshold);
}

int ConfigManager::getImageFlasherMaxConcurrentWrites() const
{
    return getValue("image_flasher/max_concurrent_writes", 1).toInt();
}

void ConfigManager::setImageFlasherMaxConcurrentWrites(int max)
{
    setValue("image_flasher/max_concurrent_writes", max);
}

bool ConfigManager::getImageFlasherEnableNotifications() const
{
    return getValue("image_flasher/enable_notifications", true).toBool();
}

void ConfigManager::setImageFlasherEnableNotifications(bool enable)
{
    setValue("image_flasher/enable_notifications", enable);
}

bool ConfigManager::getNetworkTransferEnabled() const
{
    return getValue("network_transfer/enabled", true).toBool();
}

void ConfigManager::setNetworkTransferEnabled(bool enabled)
{
    setValue("network_transfer/enabled", enabled);
}

int ConfigManager::getNetworkTransferDiscoveryPort() const
{
    return getValue("network_transfer/discovery_port", 54321).toInt();
}

void ConfigManager::setNetworkTransferDiscoveryPort(int port)
{
    setValue("network_transfer/discovery_port", port);
}

int ConfigManager::getNetworkTransferControlPort() const
{
    return getValue("network_transfer/control_port", 54322).toInt();
}

void ConfigManager::setNetworkTransferControlPort(int port)
{
    setValue("network_transfer/control_port", port);
}

int ConfigManager::getNetworkTransferDataPort() const
{
    return getValue("network_transfer/data_port", 54323).toInt();
}

void ConfigManager::setNetworkTransferDataPort(int port)
{
    setValue("network_transfer/data_port", port);
}

bool ConfigManager::getNetworkTransferEncryptionEnabled() const
{
    return getValue("network_transfer/encryption", true).toBool();
}

void ConfigManager::setNetworkTransferEncryptionEnabled(bool enabled)
{
    setValue("network_transfer/encryption", enabled);
}

bool ConfigManager::getNetworkTransferCompressionEnabled() const
{
    return getValue("network_transfer/compression", true).toBool();
}

void ConfigManager::setNetworkTransferCompressionEnabled(bool enabled)
{
    setValue("network_transfer/compression", enabled);
}

bool ConfigManager::getNetworkTransferResumeEnabled() const
{
    return getValue("network_transfer/resume", true).toBool();
}

void ConfigManager::setNetworkTransferResumeEnabled(bool enabled)
{
    setValue("network_transfer/resume", enabled);
}

int ConfigManager::getNetworkTransferMaxBandwidth() const
{
    return getValue("network_transfer/max_bandwidth", 0).toInt();
}

void ConfigManager::setNetworkTransferMaxBandwidth(int bandwidth)
{
    setValue("network_transfer/max_bandwidth", bandwidth);
}

bool ConfigManager::getNetworkTransferAutoDiscoveryEnabled() const
{
    return getValue("network_transfer/auto_discovery", true).toBool();
}

void ConfigManager::setNetworkTransferAutoDiscoveryEnabled(bool enabled)
{
    setValue("network_transfer/auto_discovery", enabled);
}

int ConfigManager::getNetworkTransferChunkSize() const
{
    return getValue("network_transfer/chunk_size", 65536).toInt();
}

void ConfigManager::setNetworkTransferChunkSize(int size)
{
    setValue("network_transfer/chunk_size", size);
}

QString ConfigManager::getNetworkTransferRelayServer() const
{
    return getValue("network_transfer/relay_server", QString()).toString();
}

void ConfigManager::setNetworkTransferRelayServer(const QString& server)
{
    setValue("network_transfer/relay_server", server);
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

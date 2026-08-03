// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file config_manager.cpp
/// @brief Implements application configuration persistence and settings management

#include "sak/config_manager.h"

#include "sak/app_paths.h"
#include "sak/logger.h"
#include "sak/network_constants.h"

#include <QCoreApplication>
#include <QtGlobal>

#include <stdexcept>

namespace sak {

namespace {
constexpr int kDefaultBackupThreadCount = 4;
constexpr int kDefaultLargeDriveThresholdGb = 128;

// Getter-side invariant enforcement: a stored value can be out of range (older
// config, hand-edited INI, or a foreign writer) even though the setters reject
// bad input. Re-clamp on read so callers never receive a value the setter would
// have refused; fall back to the default when the stored value is invalid.
int positiveOrDefault(int value, int default_value) {
    return value > 0 ? value : default_value;
}

qint64 nonNegativeOrDefault(qint64 value, qint64 default_value) {
    return value >= 0 ? value : default_value;
}

QString nonEmptyOrDefault(const QString& value, const QString& default_value) {
    return value.isEmpty() ? default_value : value;
}
}  // namespace

ConfigManager& ConfigManager::instance() {
    static ConfigManager instance;
    return instance;
}

ConfigManager::ConfigManager(QObject* parent) : QObject(parent) {
    const QString config_dir = sak::app_paths::configDirectory();
    if (!sak::app_paths::ensureDirectory(config_dir)) {
        logError("ConfigManager failed to create portable config directory: {}",
                 config_dir.toStdString());
        throw std::runtime_error("Failed to create portable config directory");
    }

    m_settings = std::make_unique<QSettings>(sak::app_paths::configFilePath(),
                                             QSettings::IniFormat);
    // make_unique throws on allocation failure, so m_settings is always valid here.
    // Surface a corrupt/unreadable config at load rather than silently proceeding
    // on an empty store: getValue() would then hand back defaults as if the file
    // were simply new.
    const QString load_error = describeSettingsStatus(m_settings->status());
    if (!load_error.isEmpty()) {
        logError("ConfigManager: {} at '{}'",
                 load_error.toStdString(),
                 m_settings->fileName().toStdString());
    }
    logInfo("ConfigManager initialized: {}", m_settings->fileName().toStdString());
    initializeDefaults();
}

void ConfigManager::initializeDefaults() {
    initializeBackupAndOrganizerDefaults();
    initializeFlasherDefaults();
    initializeUiDefaults();
}

void ConfigManager::initializeBackupAndOrganizerDefaults() {
    // Only set defaults if keys don't exist
    if (!contains("backup/thread_count")) {
        setValue("backup/thread_count", kDefaultBackupThreadCount);
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
}

void ConfigManager::initializeFlasherDefaults() {
    if (!contains("image_flasher/validation_mode")) {
        setValue("image_flasher/validation_mode", "full");
    }
    if (!contains("image_flasher/buffer_size")) {
        setValue("image_flasher/buffer_size", static_cast<int>(sak::kBufferAlignment));
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
        setValue("image_flasher/large_drive_threshold", kDefaultLargeDriveThresholdGb);
    }
    if (!contains("image_flasher/max_concurrent_writes")) {
        setValue("image_flasher/max_concurrent_writes", 1);
    }
    if (!contains("image_flasher/enable_notifications")) {
        setValue("image_flasher/enable_notifications", true);
    }
}

void ConfigManager::initializeUiDefaults() {
    if (!contains("ui/restore_window_geometry")) {
        setValue("ui/restore_window_geometry", true);
    }
}

QVariant ConfigManager::getValue(const QString& key, const QVariant& default_value) const {
    if (key.isEmpty()) {
        logWarning("ConfigManager::getValue called with empty key");
        return default_value;
    }
    return m_settings->value(key, default_value);
}

void ConfigManager::setValue(const QString& key, const QVariant& value) {
    if (key.isEmpty()) {
        logWarning("ConfigManager::setValue called with empty key");
        return;
    }
    m_settings->setValue(key, value);
    Q_EMIT settingChanged(key, value);
}

bool ConfigManager::contains(const QString& key) const {
    if (key.isEmpty()) {
        logWarning("ConfigManager::contains called with empty key");
        return false;
    }
    return m_settings->contains(key);
}

void ConfigManager::remove(const QString& key) {
    if (key.isEmpty()) {
        logWarning("ConfigManager::remove called with empty key");
        return;
    }
    m_settings->remove(key);
}

void ConfigManager::clear() {
    m_settings->clear();
    logInfo("All settings cleared");
}

void ConfigManager::resetToDefaults() {
    clear();
    initializeDefaults();
    logInfo("Settings reset to defaults");
}

QString ConfigManager::describeSettingsStatus(QSettings::Status status) {
    switch (status) {
    case QSettings::NoError:
        return {};
    case QSettings::AccessError:
        return QStringLiteral("settings access error (permission denied or file locked)");
    case QSettings::FormatError:
        return QStringLiteral("settings format error (config file is corrupt)");
    }
    return QStringLiteral("unknown settings error");
}

bool ConfigManager::isHealthy() const {
    return m_settings->status() == QSettings::NoError;
}

bool ConfigManager::sync() {
    m_settings->sync();
    // Check status: sync() itself is void in Qt, so a failed disk write is only
    // observable here. Report it instead of pretending the save succeeded.
    const QSettings::Status status = m_settings->status();
    if (status != QSettings::NoError) {
        logError("ConfigManager::sync failed: {}", describeSettingsStatus(status).toStdString());
        return false;
    }
    return true;
}

// Backup settings
int ConfigManager::getBackupThreadCount() const {
    return positiveOrDefault(getValue("backup/thread_count", kDefaultBackupThreadCount).toInt(),
                             kDefaultBackupThreadCount);
}

void ConfigManager::setBackupThreadCount(int count) {
    if (count <= 0) {
        logWarning("setBackupThreadCount: ignoring non-positive count {}", count);
        return;
    }
    setValue("backup/thread_count", count);
}

bool ConfigManager::getBackupVerifyMD5() const {
    return getValue("backup/verify_md5", true).toBool();
}

void ConfigManager::setBackupVerifyMD5(bool verify) {
    setValue("backup/verify_md5", verify);
}

QString ConfigManager::getLastBackupLocation() const {
    return getValue("backup/last_location", QString()).toString();
}

void ConfigManager::setLastBackupLocation(const QString& path) {
    setValue("backup/last_location", path);
}

// Organizer settings
bool ConfigManager::getOrganizerPreviewMode() const {
    return getValue("organizer/preview_mode", true).toBool();
}

void ConfigManager::setOrganizerPreviewMode(bool preview) {
    setValue("organizer/preview_mode", preview);
}

// Duplicate finder settings
qint64 ConfigManager::getDuplicateMinimumFileSize() const {
    return nonNegativeOrDefault(getValue("duplicate/minimum_file_size", 0).toLongLong(), 0);
}

void ConfigManager::setDuplicateMinimumFileSize(qint64 size) {
    if (size < 0) {
        logWarning("setDuplicateMinimumFileSize: ignoring negative size {}", size);
        return;
    }
    setValue("duplicate/minimum_file_size", size);
}

QString ConfigManager::getDuplicateKeepStrategy() const {
    return nonEmptyOrDefault(getValue("duplicate/keep_strategy", "oldest").toString(),
                             QStringLiteral("oldest"));
}

void ConfigManager::setDuplicateKeepStrategy(const QString& strategy) {
    if (strategy.isEmpty()) {
        logWarning("setDuplicateKeepStrategy: ignoring empty strategy");
        return;
    }
    setValue("duplicate/keep_strategy", strategy);
}

// Image Flasher settings
QString ConfigManager::getImageFlasherValidationMode() const {
    return nonEmptyOrDefault(getValue("image_flasher/validation_mode", "full").toString(),
                             QStringLiteral("full"));
}

void ConfigManager::setImageFlasherValidationMode(const QString& mode) {
    if (mode.isEmpty()) {
        logWarning("setImageFlasherValidationMode: ignoring empty mode");
        return;
    }
    setValue("image_flasher/validation_mode", mode);
}

int ConfigManager::getImageFlasherBufferSize() const {
    return positiveOrDefault(
        getValue("image_flasher/buffer_size", static_cast<int>(sak::kBufferAlignment)).toInt(),
        static_cast<int>(sak::kBufferAlignment));
}

void ConfigManager::setImageFlasherBufferSize(int size) {
    if (size <= 0) {
        logWarning("setImageFlasherBufferSize: ignoring non-positive size {}", size);
        return;
    }
    setValue("image_flasher/buffer_size", size);
}

bool ConfigManager::getImageFlasherUnmountOnCompletion() const {
    return getValue("image_flasher/unmount_on_completion", true).toBool();
}

void ConfigManager::setImageFlasherUnmountOnCompletion(bool unmount) {
    setValue("image_flasher/unmount_on_completion", unmount);
}

bool ConfigManager::getImageFlasherShowSystemDriveWarning() const {
    return getValue("image_flasher/show_system_drive_warning", true).toBool();
}

void ConfigManager::setImageFlasherShowSystemDriveWarning(bool show) {
    setValue("image_flasher/show_system_drive_warning", show);
}

bool ConfigManager::getImageFlasherShowLargeDriveWarning() const {
    return getValue("image_flasher/show_large_drive_warning", true).toBool();
}

void ConfigManager::setImageFlasherShowLargeDriveWarning(bool show) {
    setValue("image_flasher/show_large_drive_warning", show);
}

int ConfigManager::getImageFlasherLargeDriveThreshold() const {
    return positiveOrDefault(
        getValue("image_flasher/large_drive_threshold", kDefaultLargeDriveThresholdGb).toInt(),
        kDefaultLargeDriveThresholdGb);
}

void ConfigManager::setImageFlasherLargeDriveThreshold(int threshold) {
    if (threshold <= 0) {
        logWarning("setImageFlasherLargeDriveThreshold: ignoring non-positive threshold {}",
                   threshold);
        return;
    }
    setValue("image_flasher/large_drive_threshold", threshold);
}

int ConfigManager::getImageFlasherMaxConcurrentWrites() const {
    return positiveOrDefault(getValue("image_flasher/max_concurrent_writes", 1).toInt(), 1);
}

void ConfigManager::setImageFlasherMaxConcurrentWrites(int max) {
    if (max <= 0) {
        logWarning("setImageFlasherMaxConcurrentWrites: ignoring non-positive max {}", max);
        return;
    }
    setValue("image_flasher/max_concurrent_writes", max);
}

bool ConfigManager::getImageFlasherEnableNotifications() const {
    return getValue("image_flasher/enable_notifications", true).toBool();
}

void ConfigManager::setImageFlasherEnableNotifications(bool enable) {
    setValue("image_flasher/enable_notifications", enable);
}

// UI settings
bool ConfigManager::getRestoreWindowGeometry() const {
    return getValue("ui/restore_window_geometry", true).toBool();
}

void ConfigManager::setRestoreWindowGeometry(bool restore) {
    setValue("ui/restore_window_geometry", restore);
}

QByteArray ConfigManager::getWindowGeometry() const {
    return getValue("ui/window_geometry", QByteArray()).toByteArray();
}

void ConfigManager::setWindowGeometry(const QByteArray& geometry) {
    setValue("ui/window_geometry", geometry);
}

QByteArray ConfigManager::getWindowState() const {
    return getValue("ui/window_state", QByteArray()).toByteArray();
}

void ConfigManager::setWindowState(const QByteArray& state) {
    setValue("ui/window_state", state);
}

}  // namespace sak

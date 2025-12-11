// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QTimer>
#include <memory>
#include <windows.h>

namespace sak {

/**
 * @brief Information about a physical drive
 */
struct DriveInfo {
    QString devicePath;        // e.g., "\\.\PhysicalDrive1"
    QString name;              // e.g., "Generic USB Flash Disk"
    QString description;       // Additional info
    qint64 size;              // Size in bytes
    quint32 blockSize;        // Block size in bytes (usually 512 or 4096)
    bool isSystem;            // True if contains Windows installation
    bool isRemovable;         // True if removable media
    bool isReadOnly;          // True if write-protected
    QString busType;          // USB, SATA, NVMe, SD, etc.
    
    // Volume information (if mounted)
    QStringList mountPoints;  // e.g., ["E:\\", "F:\\"]
    QString volumeLabel;      // Volume label if any
    
    bool isValid() const { return !devicePath.isEmpty() && size > 0; }
};

} // namespace sak

/**
 * @brief Drive Scanner - Detects physical drives with hot-plug support
 * 
 * Monitors system for physical drives (USB, SD cards, etc.) and provides
 * real-time notifications when drives are attached or removed.
 * Based on Etcher SDK's Scanner/BlockDeviceAdapter pattern.
 * 
 * Features:
 * - Enumerate physical drives via WMI
 * - Filter system/removable drives
 * - Hot-plug detection via Windows device notifications
 * - Drive property queries (size, block size, bus type)
 * - Volume mount point detection
 * - Read-only/write-protection detection
 * 
 * Thread-Safety: All methods are thread-safe. Signals emitted on main thread.
 * 
 * Example:
 * @code
 * DriveScanner scanner;
 * connect(&scanner, &DriveScanner::driveAttached, [](const DriveInfo& info) {
 *     qDebug() << "Drive attached:" << info.name << info.size;
 * });
 * scanner.start();
 * @endcode
 */
class DriveScanner : public QObject {
    Q_OBJECT

public:
    explicit DriveScanner(QObject* parent = nullptr);
    ~DriveScanner() override;

    // Disable copy and move
    DriveScanner(const DriveScanner&) = delete;
    DriveScanner& operator=(const DriveScanner&) = delete;
    DriveScanner(DriveScanner&&) = delete;
    DriveScanner& operator=(DriveScanner&&) = delete;

    /**
     * @brief Start drive monitoring
     * Begins scanning for drives and enables hot-plug detection
     */
    void start();

    /**
     * @brief Stop drive monitoring
     * Disables hot-plug detection
     */
    void stop();

    /**
     * @brief Get list of all detected drives
     * @return List of DriveInfo structures
     */
    QList<sak::DriveInfo> getDrives() const;

    /**
     * @brief Get list of removable drives only
     * @return List of removable DriveInfo structures
     */
    QList<sak::DriveInfo> getRemovableDrives() const;

    /**
     * @brief Get drive info by device path
     * @param devicePath Device path (e.g., "\\.\PhysicalDrive1")
     * @return DriveInfo if found, invalid DriveInfo otherwise
     */
    sak::DriveInfo getDriveInfo(const QString& devicePath) const;

    /**
     * @brief Check if a drive is the system drive
     * @param devicePath Device path to check
     * @return true if drive contains Windows installation
     */
    bool isSystemDrive(const QString& devicePath) const;

    /**
     * @brief Refresh drive list immediately
     * Forces a rescan of all drives
     */
    void refresh();

Q_SIGNALS:
    /**
     * @brief Emitted when a new drive is attached
     * @param info Information about the attached drive
     */
    void driveAttached(const sak::DriveInfo& info);

    /**
     * @brief Emitted when a drive is detached
     * @param devicePath Device path of the detached drive
     */
    void driveDetached(const QString& devicePath);

    /**
     * @brief Emitted when drive list is updated
     * @param drives Current list of all drives
     */
    void drivesUpdated(const QList<sak::DriveInfo>& drives);

    /**
     * @brief Emitted on error during scanning
     * @param error Error message
     */
    void scanError(const QString& error);

private Q_SLOTS:
    void onRefreshTimer();

private:
    void scanDrives();
    void registerDeviceNotification();
    void unregisterDeviceNotification();
    
    sak::DriveInfo queryDriveInfo(int driveNumber);
    QString getDriveName(int driveNumber);
    qint64 getDriveSize(HANDLE hDrive);
    quint32 getBlockSize(HANDLE hDrive);
    QString getBusType(HANDLE hDrive);
    bool isDriveRemovable(int driveNumber);
    bool isDriveReadOnly(HANDLE hDrive);
    QStringList getMountPoints(int driveNumber);
    QString getVolumeLabel(const QString& mountPoint);
    bool containsWindowsInstallation(int driveNumber);

    static LRESULT CALLBACK deviceNotificationProc(HWND hwnd, UINT message, 
                                                   WPARAM wParam, LPARAM lParam);

    QList<sak::DriveInfo> m_drives;
    QTimer* m_refreshTimer;
    HWND m_notificationWindow;
    HDEVNOTIFY m_deviceNotify;
    bool m_isScanning;
    
    static DriveScanner* s_instance; // For static callback
};

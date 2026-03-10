// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/action_constants.h"

#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

#include <windows.h>

/**
 * @brief Drive Unmounter - Windows-specific drive preparation
 *
 * Handles safe unmounting and preparation of drives for raw writing.
 * Based on Etcher SDK patterns for drive preparation.
 *
 * Operations:
 * 1. Enumerate all volumes on the physical drive
 * 2. Lock each volume for exclusive access
 * 3. Dismount all volumes
 * 4. Close all file handles
 * 5. Prevent Windows auto-mount during operation
 *
 * Retry Strategy:
 * - Exponential backoff: 100ms, 200ms, 400ms, 800ms, 1600ms
 * - Maximum retry attempts: 5
 * - Fail gracefully if locked by another process
 *
 * Thread-Safety: Methods are NOT thread-safe. Use from single thread.
 *
 * Example:
 * @code
 * DriveUnmounter unmounter;
 * if (!unmounter.unmountDrive(0)) {
 *     qWarning() << "Failed:" << unmounter.lastError();
 * }
 * @endcode
 */
class DriveUnmounter : public QObject {
    Q_OBJECT

public:
    explicit DriveUnmounter(QObject* parent = nullptr);
    ~DriveUnmounter() override;

    /**
     * @brief Unmount all volumes on a physical drive
     * @param driveNumber Physical drive number (0 = first drive)
     * @return true if successful, false on failure
     */
    bool unmountDrive(int driveNumber);

    /**
     * @brief Get list of volume mount points on a drive
     * @param driveNumber Physical drive number
     * @return List of mount points (e.g., "C:\", "D:\")
     */
    QStringList getVolumesOnDrive(int driveNumber) const;

    /**
     * @brief Lock a volume for exclusive access
     * @param volumePath Volume path (e.g., "\\.\C:")
     * @return Volume handle, or INVALID_HANDLE_VALUE on failure
     */
    HANDLE lockVolume(const QString& volumePath);

    /**
     * @brief Dismount a volume
     * @param volumeHandle Handle to locked volume
     * @return true if successful
     */
    bool dismountVolume(HANDLE volumeHandle);

    /**
     * @brief Delete all mount points for a volume
     * @param volumePath Volume GUID path (e.g., "\\?\Volume{...}\")
     * @return true if successful
     */
    bool deleteMountPoints(const QString& volumePath);

    /**
     * @brief Prevent Windows from auto-mounting volumes
     * @param driveNumber Physical drive number
     * @return true if successful
     */
    bool preventAutoMount(int driveNumber);

    /**
     * @brief Get last error message
     * @return Human-readable error description
     */
    QString lastError() const { return m_lastError; }

Q_SIGNALS:
    /**
     * @brief Emitted during unmount operations
     * @param message Status message
     */
    void statusMessage(const QString& message);

private:
    /**
     * @brief Retry an operation with exponential backoff
     * @param operation Lambda function to retry
     * @param maxAttempts Maximum retry attempts (default 5)
     * @return true if operation succeeded within retries
     */
    bool retryWithBackoff(std::function<bool()> operation, int maxAttempts = sak::kRetryCountHigh);

    /**
     * @brief Lock and dismount a single volume with retry
     * @param volumePath Volume path to lock and dismount
     * @return true if successful
     */
    bool lockAndDismountVolume(const QString& volumePath);

    /**
     * @brief Get physical drive number for a volume
     * @param volumePath Volume path
     * @return Drive number, or -1 on failure
     */
    int getDriveNumberForVolume(const QString& volumePath) const;

    /**
     * @brief Close all handles to a drive
     * @param driveNumber Physical drive number
     * @return true if successful
     */
    bool closeAllHandles(int driveNumber);

    /**
     * @brief Find all volume GUID paths belonging to a physical drive
     * @param driveNumber Physical drive number
     * @return List of volume GUID paths (without trailing backslash)
     */
    QStringList findVolumesForDrive(int driveNumber) const;

    /**
     * @brief Use Restart Manager to force-close open handles on volumes
     * @param dwSession Active Restart Manager session handle
     * @param mountPoints Volume GUID paths to process
     */
    void shutdownHandlesViaRestartManager(DWORD dwSession, const QStringList& mountPoints);

    QString m_lastError;                    // Last error message
    QMap<QString, HANDLE> m_lockedVolumes;  // Volume path -> handle mapping
};

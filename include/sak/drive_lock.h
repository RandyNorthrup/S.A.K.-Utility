// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>
#include <windows.h>

/**
 * @brief RAII Drive Lock - Exclusive access to physical drive
 * 
 * Provides exception-safe exclusive locking of a physical drive or volume.
 * Based on RAII pattern - lock is acquired in constructor, released in destructor.
 * 
 * Features:
 * - Automatic lock acquisition and release
 * - Exception-safe (destructor always runs)
 * - Prevents other processes from accessing drive
 * - Prevents Windows from auto-mounting volumes
 * 
 * Usage Pattern:
 * @code
 * {
 *     DriveLock lock(0);  // Lock PhysicalDrive0
 *     if (lock.isLocked()) {
 *         // Safe to write to drive
 *         writeData();
 *     }
 *     // Lock automatically released when scope ends
 * }
 * @endcode
 * 
 * Thread-Safety: NOT thread-safe. Use one instance per thread.
 */
class DriveLock {
public:
    /**
     * @brief Constructor - Acquire exclusive lock on physical drive
     * @param driveNumber Physical drive number (0 = first drive)
     * @param readOnly If true, open for read-only access (default: false)
     */
    explicit DriveLock(int driveNumber, bool readOnly = false);

    /**
     * @brief Constructor - Acquire exclusive lock on volume
     * @param volumePath Volume path (e.g., "\\.\C:" or "\\?\Volume{...}")
     * @param readOnly If true, open for read-only access (default: false)
     */
    explicit DriveLock(const QString& volumePath, bool readOnly = false);

    /**
     * @brief Destructor - Release lock and close handle
     */
    ~DriveLock();

    // Disable copy (RAII pattern)
    DriveLock(const DriveLock&) = delete;
    DriveLock& operator=(const DriveLock&) = delete;

    // Enable move semantics
    DriveLock(DriveLock&& other) noexcept;
    DriveLock& operator=(DriveLock&& other) noexcept;

    /**
     * @brief Check if lock was acquired successfully
     * @return true if locked, false on failure
     */
    bool isLocked() const { return m_handle != INVALID_HANDLE_VALUE; }

    /**
     * @brief Get the drive handle
     * @return Windows handle, or INVALID_HANDLE_VALUE if not locked
     */
    HANDLE handle() const { return m_handle; }

    /**
     * @brief Get last error message
     * @return Human-readable error description
     */
    QString lastError() const { return m_lastError; }

    /**
     * @brief Get drive path
     * @return Drive or volume path
     */
    QString path() const { return m_path; }

    /**
     * @brief Release lock early (before destructor)
     * Called automatically by destructor.
     */
    void unlock();

private:
    /**
     * @brief Internal lock acquisition
     * @param path Drive or volume path
     * @param readOnly Read-only access flag
     */
    void acquireLock(const QString& path, bool readOnly);

    HANDLE m_handle;        // Windows handle to drive/volume
    QString m_path;         // Drive or volume path
    QString m_lastError;    // Last error message
    bool m_isLocked;        // Lock status flag
};

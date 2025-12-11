// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/drive_lock.h"
#include "sak/logger.h"
#include <windows.h>
#include <winioctl.h>

DriveLock::DriveLock(int driveNumber, bool readOnly)
    : m_handle(INVALID_HANDLE_VALUE)
    , m_isLocked(false)
{
    QString path = QString("\\\\.\\PhysicalDrive%1").arg(driveNumber);
    acquireLock(path, readOnly);
}

DriveLock::DriveLock(const QString& volumePath, bool readOnly)
    : m_handle(INVALID_HANDLE_VALUE)
    , m_isLocked(false)
{
    acquireLock(volumePath, readOnly);
}

DriveLock::~DriveLock() {
    unlock();
}

DriveLock::DriveLock(DriveLock&& other) noexcept
    : m_handle(other.m_handle)
    , m_path(std::move(other.m_path))
    , m_lastError(std::move(other.m_lastError))
    , m_isLocked(other.m_isLocked)
{
    other.m_handle = INVALID_HANDLE_VALUE;
    other.m_isLocked = false;
}

DriveLock& DriveLock::operator=(DriveLock&& other) noexcept {
    if (this != &other) {
        unlock(); // Release current lock

        m_handle = other.m_handle;
        m_path = std::move(other.m_path);
        m_lastError = std::move(other.m_lastError);
        m_isLocked = other.m_isLocked;

        other.m_handle = INVALID_HANDLE_VALUE;
        other.m_isLocked = false;
    }
    return *this;
}

void DriveLock::unlock() {
    if (m_handle != INVALID_HANDLE_VALUE) {
        // Unlock the volume if it was locked
        if (m_isLocked) {
            DWORD bytesReturned = 0;
            DeviceIoControl(
                m_handle,
                FSCTL_UNLOCK_VOLUME,
                nullptr,
                0,
                nullptr,
                0,
                &bytesReturned,
                nullptr
            );
            m_isLocked = false;
        }

        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;

        sak::log_info(QString("Released lock on %1").arg(m_path).toStdString());
    }
}

void DriveLock::acquireLock(const QString& path, bool readOnly) {
    m_path = path;
    std::wstring wPath = path.toStdWString();

    sak::log_info(QString("Acquiring lock on %1").arg(path).toStdString());

    // Determine access flags
    DWORD accessFlags = readOnly ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
    DWORD shareFlags = FILE_SHARE_READ | FILE_SHARE_WRITE;

    // Open the drive/volume
    m_handle = CreateFileW(
        wPath.c_str(),
        accessFlags,
        shareFlags,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
        nullptr
    );

    if (m_handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        m_lastError = QString("Failed to open %1: error %2").arg(path).arg(error);
        sak::log_error(m_lastError.toStdString());
        return;
    }

    // Try to lock the volume for exclusive access
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(
        m_handle,
        FSCTL_LOCK_VOLUME,
        nullptr,
        0,
        nullptr,
        0,
        &bytesReturned,
        nullptr
    )) {
        DWORD error = GetLastError();
        m_lastError = QString("Failed to lock %1: error %2").arg(path).arg(error);
        sak::log_warning(m_lastError.toStdString());
        
        // Lock failure is not fatal for physical drives - we can still write
        // but for volumes, we should fail
        if (path.contains("Volume") || path.contains(":")) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
            return;
        }
    } else {
        m_isLocked = true;
        sak::log_info(QString("Successfully locked %1").arg(path).toStdString());
    }

    // For physical drives, try to bring drive offline to prevent auto-mount
    if (path.contains("PhysicalDrive")) {
        SET_DISK_ATTRIBUTES attributes = {};
        attributes.Version = sizeof(SET_DISK_ATTRIBUTES);
        attributes.Persist = FALSE; // Temporary offline state
        attributes.Attributes = DISK_ATTRIBUTE_OFFLINE;
        attributes.AttributesMask = DISK_ATTRIBUTE_OFFLINE;

        if (DeviceIoControl(
            m_handle,
            IOCTL_DISK_SET_DISK_ATTRIBUTES,
            &attributes,
            sizeof(attributes),
            nullptr,
            0,
            &bytesReturned,
            nullptr
        )) {
            sak::log_info(QString("Drive set to offline mode").toStdString());
        } else {
            sak::log_warning(QString("Failed to set drive offline, continuing anyway").toStdString());
        }
    }

    sak::log_info(QString("Lock acquired on %1 (handle: %2)")
        .arg(path).arg(reinterpret_cast<quintptr>(m_handle)).toStdString());
}

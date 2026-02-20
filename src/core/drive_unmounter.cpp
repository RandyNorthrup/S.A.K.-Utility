// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/drive_unmounter.h"
#include "sak/logger.h"
#include <QThread>
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <RestartManager.h>
#include <functional>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "Rstrtmgr.lib")

DriveUnmounter::DriveUnmounter(QObject* parent)
    : QObject(parent)
{
}

DriveUnmounter::~DriveUnmounter() {
    // Unlock all locked volumes
    for (auto it = m_lockedVolumes.constBegin(); it != m_lockedVolumes.constEnd(); ++it) {
        if (it.value() != INVALID_HANDLE_VALUE) {
            CloseHandle(it.value());
        }
    }
    m_lockedVolumes.clear();
}

bool DriveUnmounter::unmountDrive(int driveNumber) {
    sak::logInfo(QString("Unmounting drive %1").arg(driveNumber).toStdString());
    Q_EMIT statusMessage(QString("Preparing drive %1...").arg(driveNumber));

    // Step 1: Get all volumes on this drive
    QStringList volumes = getVolumesOnDrive(driveNumber);
    if (volumes.isEmpty()) {
        sak::logInfo(QString("No volumes found on drive, proceeding").toStdString());
        return true;
    }

    // Step 2: Prevent auto-mount
    if (!preventAutoMount(driveNumber)) {
        sak::logWarning(QString("Failed to prevent auto-mount, continuing anyway").toStdString());
    }

    // Step 3: Lock and dismount each volume
    bool allSucceeded = true;
    for (const QString& volumePath : volumes) {
        Q_EMIT statusMessage(QString("Unmounting volume %1...").arg(volumePath));

        // Delete mount points first
        if (!deleteMountPoints(volumePath)) {
            sak::logWarning(QString("Failed to delete mount points for %1")
                .arg(volumePath).toStdString());
        }

        // Lock the volume with retry
        HANDLE volumeHandle = INVALID_HANDLE_VALUE;
        bool locked = retryWithBackoff([&]() {
            volumeHandle = lockVolume(volumePath);
            return volumeHandle != INVALID_HANDLE_VALUE;
        });

        if (!locked) {
            m_lastError = QString("Failed to lock volume %1: %2")
                .arg(volumePath).arg(m_lastError);
            sak::logError(m_lastError.toStdString());
            allSucceeded = false;
            continue;
        }

        // Dismount the volume with retry
        bool dismounted = retryWithBackoff([&]() {
            return dismountVolume(volumeHandle);
        });

        if (!dismounted) {
            m_lastError = QString("Failed to dismount volume %1: %2")
                .arg(volumePath).arg(m_lastError);
            sak::logError(m_lastError.toStdString());
            CloseHandle(volumeHandle);
            allSucceeded = false;
            continue;
        }

        // Keep handle open until we're done with the drive
        m_lockedVolumes.insert(volumePath, volumeHandle);
        sak::logInfo(QString("Successfully unmounted %1").arg(volumePath).toStdString());
    }

    // Step 4: Close all remaining handles
    closeAllHandles(driveNumber);

    if (allSucceeded) {
        Q_EMIT statusMessage("Drive prepared successfully");
        sak::logInfo(QString("Drive unmount completed successfully").toStdString());
    } else {
        Q_EMIT statusMessage("Drive preparation completed with warnings");
        sak::logWarning(QString("Drive unmount completed with some failures").toStdString());
    }

    return allSucceeded;
}

QStringList DriveUnmounter::getVolumesOnDrive(int driveNumber) const {
    QStringList volumes;

    // Enumerate all volumes using FindFirstVolume/FindNextVolume
    WCHAR volumeName[MAX_PATH];
    HANDLE hFind = FindFirstVolumeW(volumeName, ARRAYSIZE(volumeName));
    if (hFind == INVALID_HANDLE_VALUE) {
        return volumes;
    }

    do {
        // Remove trailing backslash for CreateFile
        size_t length = wcslen(volumeName);
        if (length > 0 && volumeName[length - 1] == L'\\') {
            volumeName[length - 1] = L'\0';
        }

        // Open the volume
        HANDLE hVolume = CreateFileW(
            volumeName,
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );

        if (hVolume != INVALID_HANDLE_VALUE) {
            // Get the drive number for this volume
            STORAGE_DEVICE_NUMBER deviceNumber = {};
            DWORD bytesReturned = 0;

            if (DeviceIoControl(
                hVolume,
                IOCTL_STORAGE_GET_DEVICE_NUMBER,
                nullptr,
                0,
                &deviceNumber,
                sizeof(deviceNumber),
                &bytesReturned,
                nullptr
            )) {
                if (static_cast<int>(deviceNumber.DeviceNumber) == driveNumber) {
                    volumes.append(QString::fromWCharArray(volumeName));
                }
            }

            CloseHandle(hVolume);
        }

        // Restore trailing backslash for FindNextVolume
        volumeName[length - 1] = L'\\';

    } while (FindNextVolumeW(hFind, volumeName, ARRAYSIZE(volumeName)));

    FindVolumeClose(hFind);
    return volumes;
}

HANDLE DriveUnmounter::lockVolume(const QString& volumePath) {
    std::wstring wVolumePath = volumePath.toStdWString();

    // Open the volume
    HANDLE hVolume = CreateFileW(
        wVolumePath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );

    if (hVolume == INVALID_HANDLE_VALUE) {
        m_lastError = QString("CreateFile failed: error %1").arg(GetLastError());
        return INVALID_HANDLE_VALUE;
    }

    // Lock the volume
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(
        hVolume,
        FSCTL_LOCK_VOLUME,
        nullptr,
        0,
        nullptr,
        0,
        &bytesReturned,
        nullptr
    )) {
        m_lastError = QString("FSCTL_LOCK_VOLUME failed: error %1").arg(GetLastError());
        CloseHandle(hVolume);
        return INVALID_HANDLE_VALUE;
    }

    return hVolume;
}

bool DriveUnmounter::dismountVolume(HANDLE volumeHandle) {
    if (volumeHandle == INVALID_HANDLE_VALUE) {
        m_lastError = "Invalid volume handle";
        return false;
    }

    // Dismount the volume
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(
        volumeHandle,
        FSCTL_DISMOUNT_VOLUME,
        nullptr,
        0,
        nullptr,
        0,
        &bytesReturned,
        nullptr
    )) {
        m_lastError = QString("FSCTL_DISMOUNT_VOLUME failed: error %1").arg(GetLastError());
        return false;
    }

    return true;
}

bool DriveUnmounter::deleteMountPoints(const QString& volumePath) {
    // Get all mount points for this volume
    WCHAR volumePathBuf[MAX_PATH];
    wcscpy_s(volumePathBuf, volumePath.toStdWString().c_str());

    // Ensure trailing backslash
    size_t length = wcslen(volumePathBuf);
    if (length > 0 && volumePathBuf[length - 1] != L'\\') {
        wcscat_s(volumePathBuf, L"\\");
    }

    WCHAR mountPoint[MAX_PATH];
    HANDLE hFindMP = FindFirstVolumeMountPointW(volumePathBuf, mountPoint, ARRAYSIZE(mountPoint));

    if (hFindMP == INVALID_HANDLE_VALUE) {
        // No mount points is not an error
        return true;
    }

    bool allSucceeded = true;
    do {
        // Build full mount point path
        WCHAR fullPath[MAX_PATH];
        wcscpy_s(fullPath, volumePathBuf);
        wcscat_s(fullPath, mountPoint);

        // Delete the mount point
        if (!DeleteVolumeMountPointW(fullPath)) {
            sak::logWarning(QString("Failed to delete mount point: %1")
                .arg(QString::fromWCharArray(fullPath)).toStdString());
            allSucceeded = false;
        }

    } while (FindNextVolumeMountPointW(hFindMP, mountPoint, ARRAYSIZE(mountPoint)));

    FindVolumeMountPointClose(hFindMP);
    return allSucceeded;
}

bool DriveUnmounter::preventAutoMount(int driveNumber) {
    // Open the physical drive
    QString drivePath = QString("\\\\.\\PhysicalDrive%1").arg(driveNumber);
    std::wstring wDrivePath = drivePath.toStdWString();

    HANDLE hDrive = CreateFileW(
        wDrivePath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );

    if (hDrive == INVALID_HANDLE_VALUE) {
        m_lastError = QString("Failed to open drive: error %1").arg(GetLastError());
        return false;
    }

    // Set persistent volume state to prevent auto-mount
    SET_DISK_ATTRIBUTES attributes = {};
    attributes.Version = sizeof(SET_DISK_ATTRIBUTES);
    attributes.Persist = TRUE;
    attributes.Attributes = DISK_ATTRIBUTE_OFFLINE;
    attributes.AttributesMask = DISK_ATTRIBUTE_OFFLINE;

    DWORD bytesReturned = 0;
    bool success = DeviceIoControl(
        hDrive,
        IOCTL_DISK_SET_DISK_ATTRIBUTES,
        &attributes,
        sizeof(attributes),
        nullptr,
        0,
        &bytesReturned,
        nullptr
    );

    if (!success) {
        m_lastError = QString("IOCTL_DISK_SET_DISK_ATTRIBUTES failed: error %1")
            .arg(GetLastError());
    }

    CloseHandle(hDrive);
    return success;
}

bool DriveUnmounter::retryWithBackoff(std::function<bool()> operation, int maxAttempts) {
    int delay = 100; // Start with 100ms

    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        if (operation()) {
            return true;
        }

        if (attempt < maxAttempts) {
            sak::logInfo(QString("Retry attempt %1/%2, waiting %3ms")
                .arg(attempt).arg(maxAttempts).arg(delay).toStdString());
            QThread::msleep(delay);
            delay *= 2; // Exponential backoff
        }
    }

    return false;
}

int DriveUnmounter::getDriveNumberForVolume(const QString& volumePath) const {
    std::wstring wVolumePath = volumePath.toStdWString();

    // Remove trailing backslash
    if (!wVolumePath.empty() && wVolumePath.back() == L'\\') {
        wVolumePath.pop_back();
    }

    HANDLE hVolume = CreateFileW(
        wVolumePath.c_str(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );

    if (hVolume == INVALID_HANDLE_VALUE) {
        return -1;
    }

    STORAGE_DEVICE_NUMBER deviceNumber = {};
    DWORD bytesReturned = 0;

    if (!DeviceIoControl(
        hVolume,
        IOCTL_STORAGE_GET_DEVICE_NUMBER,
        nullptr,
        0,
        &deviceNumber,
        sizeof(deviceNumber),
        &bytesReturned,
        nullptr
    )) {
        CloseHandle(hVolume);
        return -1;
    }

    CloseHandle(hVolume);
    return static_cast<int>(deviceNumber.DeviceNumber);
}

bool DriveUnmounter::closeAllHandles(int driveNumber) {
    for (auto it = m_lockedVolumes.constBegin(); it != m_lockedVolumes.constEnd(); ++it) {
        if (it.value() != INVALID_HANDLE_VALUE) {
            CloseHandle(it.value());
        }
    }
    m_lockedVolumes.clear();
    
    DWORD dwSession;
    WCHAR szSessionKey[CCH_RM_SESSION_KEY + 1] = {0};
    DWORD dwError = RmStartSession(&dwSession, 0, szSessionKey);
    
    if (dwError != ERROR_SUCCESS) {
        sak::logWarning(QString("Failed to start Restart Manager session: %1").arg(dwError).toStdString());
        return true;
    }
    
    QStringList mountPoints;
    wchar_t volumeName[MAX_PATH];
    HANDLE hFind = FindFirstVolumeW(volumeName, MAX_PATH);
    
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            HANDLE hVolume = CreateFileW(
                volumeName,
                0,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                0,
                nullptr
            );
            
            if (hVolume != INVALID_HANDLE_VALUE) {
                STORAGE_DEVICE_NUMBER deviceNumber = {};
                DWORD bytesReturned = 0;
                
                if (DeviceIoControl(
                    hVolume,
                    IOCTL_STORAGE_GET_DEVICE_NUMBER,
                    nullptr, 0,
                    &deviceNumber, sizeof(deviceNumber),
                    &bytesReturned,
                    nullptr)
                    && static_cast<int>(deviceNumber.DeviceNumber) == driveNumber)
                {
                    size_t len = wcslen(volumeName);
                    if (len > 0 && volumeName[len - 1] == L'\\') {
                        volumeName[len - 1] = L'\0';
                    }
                    mountPoints.append(QString::fromWCharArray(volumeName));
                }
                
                CloseHandle(hVolume);
            }
        } while (FindNextVolumeW(hFind, volumeName, MAX_PATH));
        
        FindVolumeClose(hFind);
    }
    
    QVector<LPCWSTR> files;
    for (const QString& mountPoint : mountPoints) {
        files.append(reinterpret_cast<LPCWSTR>(mountPoint.utf16()));
    }
    
    if (!files.isEmpty()) {
        dwError = RmRegisterResources(dwSession, files.size(), const_cast<LPCWSTR*>(files.data()), 0, nullptr, 0, nullptr);
        
        if (dwError == ERROR_SUCCESS) {
            DWORD dwReason;
            UINT nProcInfoNeeded = 0;
            UINT nProcInfo = 0;
            RM_PROCESS_INFO rgpi[10];
            
            dwError = RmGetList(dwSession, &nProcInfoNeeded, &nProcInfo, rgpi, &dwReason);
            
            if (dwError == ERROR_SUCCESS || dwError == ERROR_MORE_DATA) {
                if (nProcInfoNeeded > 0) {
                    sak::logInfo(QString("Found %1 processes with open handles").arg(nProcInfoNeeded).toStdString());
                    
                    dwError = RmShutdown(dwSession, RmForceShutdown, nullptr);
                    if (dwError == ERROR_SUCCESS) {
                        sak::logInfo("Successfully closed all file handles");
                    } else {
                        sak::logWarning(QString("Failed to close handles: %1").arg(dwError).toStdString());
                    }
                }
            }
        }
    }
    
    RmEndSession(dwSession);
    return true;
}

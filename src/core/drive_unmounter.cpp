// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/drive_unmounter.h"

#include "sak/logger.h"

#include <QThread>

#include <functional>
#include <vector>

#include <windows.h>

#include <cfgmgr32.h>
#include <RestartManager.h>
#include <setupapi.h>
#include <winioctl.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "Rstrtmgr.lib")

namespace {
constexpr int kBackoffInitialDelayMs = 100;
constexpr int kBackoffMultiplier = 2;
constexpr int kRestartManagerProcessCapacity = 10;
constexpr int kEjectRetryAttempts = 3;

/// @brief Query the physical drive number for a volume path
// Sentinels distinguishing the two failure modes so the caller can fail closed on
// the dangerous one without over-blocking on the benign one:
constexpr int kVolumeUnopenable = -1;   // could not open the volume (e.g. empty
                                        // card-reader slot / no media) -- such a
                                        // volume is never the mounted target.
constexpr int kVolumeProbeFailed = -2;  // opened but the device-number IOCTL
                                        // failed -- identity unknown; treat as
                                        // possibly-on-target and fail closed.

/// @param volumePath Volume GUID path (with or without trailing backslash)
/// @return Drive number (>=0), kVolumeUnopenable, or kVolumeProbeFailed
int queryVolumeDriveNumber(const wchar_t* volumePath) {
    // Both callers pass the stack volume-name buffer FindFirstVolumeW/FindNextVolumeW filled in.
    Q_ASSERT(volumePath);
    HANDLE hVolume = CreateFileW(
        volumePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);

    if (hVolume == INVALID_HANDLE_VALUE) {
        return kVolumeUnopenable;
    }

    STORAGE_DEVICE_NUMBER deviceNumber = {};
    DWORD bytesReturned = 0;
    int result = kVolumeProbeFailed;

    if (DeviceIoControl(hVolume,
                        IOCTL_STORAGE_GET_DEVICE_NUMBER,
                        nullptr,
                        0,
                        &deviceNumber,
                        sizeof(deviceNumber),
                        &bytesReturned,
                        nullptr) != 0) {
        result = static_cast<int>(deviceNumber.DeviceNumber);
    }

    CloseHandle(hVolume);
    return result;
}

// Classify one enumerated volume against the target drive. Returns true to keep
// scanning; on an openable-but-unprobeable volume returns false and sets *fatal
// (fail closed). Appends the volume (without trailing backslash) to @p volumes on
// a match, then restores the trailing backslash for the next FindNextVolumeW.
bool classifyEnumeratedVolume(wchar_t* volumeName,
                              int driveNumber,
                              QStringList& volumes,
                              bool* fatal) {
    const size_t length = wcslen(volumeName);
    const bool hadSlash = (length > 0 && volumeName[length - 1] == L'\\');
    if (hadSlash) {
        volumeName[length - 1] = L'\0';
    }
    const int volumeDrive = queryVolumeDriveNumber(volumeName);
    bool keepScanning = true;
    if (volumeDrive == kVolumeProbeFailed) {
        // Opened but its physical-disk identity could not be read -- it could be a
        // mounted volume on the target we are about to raw-write, so fail closed.
        // (An unopenable volume -- e.g. an empty card-reader slot -- is never the
        // mounted target and is simply not matched.)
        *fatal = true;
        keepScanning = false;
    } else if (volumeDrive == driveNumber) {
        volumes.append(QString::fromWCharArray(volumeName));
    }
    if (hadSlash) {
        volumeName[length - 1] = L'\\';
    }
    return keepScanning;
}

/// @brief Write the optional enumeration-authority out-param (no-op when the caller did not
/// ask for it). Keeps getVolumesOnDrive within the complexity gate.
void setEnumerationOk(bool* enumerationOk, bool value) {
    if (enumerationOk != nullptr) {
        *enumerationOk = value;
    }
}

/// @brief Log the processes the Restart Manager is about to FORCE-TERMINATE.
///
/// RmShutdown(RmForceShutdown) kills every holder without giving it a chance to save, so
/// the technician log must at least name them. A list shorter than the reported total is
/// declared partial rather than passed off as the complete set.
void logRestartManagerHolders(const RM_PROCESS_INFO* processes, UINT listed, UINT needed) {
    for (UINT i = 0; i < listed; ++i) {
        sak::logWarning(QString("Restart Manager will force-close '%1' (PID %2), which holds a "
                                "handle on the target drive")
                            .arg(QString::fromWCharArray(processes[i].strAppName))
                            .arg(processes[i].Process.dwProcessId)
                            .toStdString());
    }
    if (needed > listed) {
        sak::logWarning(QString("Restart Manager reported %1 holder process(es); only %2 could be "
                                "listed before the force-close")
                            .arg(needed)
                            .arg(listed)
                            .toStdString());
    }
}

}  // anonymous namespace

DriveUnmounter::DriveUnmounter(QObject* parent) : QObject(parent) {}

DriveUnmounter::~DriveUnmounter() {
    closeLockedVolumeHandles();
}

void DriveUnmounter::closeLockedVolumeHandles() {
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

    // Step 1: Get all volumes on this drive. Fail closed if enumeration itself
    // failed: an empty list from a failed enumeration must not be treated as
    // "no volumes, safe to proceed" -- that would raw-write a still-mounted disk.
    bool enumerationOk = true;
    const QStringList volumes = getVolumesOnDrive(driveNumber, &enumerationOk);
    if (!enumerationOk) {
        m_lastError = QString("Failed to enumerate volumes on drive %1").arg(driveNumber);
        Q_EMIT statusMessage("Failed to enumerate volumes; aborting");
        sak::logError(m_lastError.toStdString());
        return false;
    }
    // Step 2: Set the persistent offline attribute BEFORE releasing any volume
    // locks -- and even when the disk currently has no volumes -- so Windows cannot
    // auto-mount the partitions we write mid-flash. Fail closed if it can't be set.
    if (!preventAutoMount(driveNumber)) {
        m_lastError = QString("Failed to prevent auto-mount on drive %1: %2")
                          .arg(driveNumber)
                          .arg(m_lastError);
        Q_EMIT statusMessage("Failed to prevent auto-mount; aborting");
        sak::logError(m_lastError.toStdString());
        return false;
    }

    if (volumes.isEmpty()) {
        sak::logInfo(QString("No volumes on drive %1; offline attribute set")
                         .arg(driveNumber)
                         .toStdString());
        return true;
    }

    // Step 3: Lock and dismount each volume
    bool allSucceeded = true;
    for (const QString& volumePath : volumes) {
        Q_EMIT statusMessage(QString("Unmounting volume %1...").arg(volumePath));
        if (!lockAndDismountVolume(volumePath)) {
            allSucceeded = false;
        }
    }

    // Step 4: advisory Restart Manager pass (non-fatal -- offline attribute + locks
    // are authoritative).
    advisoryCloseHandles(driveNumber);

    if (allSucceeded) {
        Q_EMIT statusMessage("Drive prepared successfully");
        sak::logInfo(QString("Drive unmount completed successfully").toStdString());
        return true;
    }

    // Partial failure: roll back the persistent offline state so the drive is not
    // left offline with mount points removed and no protection in place.
    if (!allowAutoMount(driveNumber)) {
        sak::logWarning(QString("Rollback: failed to bring drive %1 back online")
                            .arg(driveNumber)
                            .toStdString());
    }
    Q_EMIT statusMessage("Drive preparation failed; drive restored online");
    sak::logWarning(QString("Drive unmount failed; rolled back offline state").toStdString());
    return false;
}

void DriveUnmounter::advisoryCloseHandles(int driveNumber) {
    // Best-effort Restart Manager pass to close third-party handles still open on
    // the volumes. ADVISORY ONLY -- the authoritative guard against concurrent
    // access during the raw write is the persistent OFFLINE attribute plus the
    // exclusive volume locks we still hold, so an incomplete pass is not fatal.
    if (!closeAllHandles(driveNumber)) {
        sak::logWarning(QString("Restart Manager handle-close incomplete on drive %1 "
                                "(advisory; offline attribute + volume locks remain "
                                "authoritative)")
                            .arg(driveNumber)
                            .toStdString());
    }
}

bool DriveUnmounter::lockAndDismountVolume(const QString& volumePath) {
    // Delete mount points first
    if (!deleteMountPoints(volumePath)) {
        sak::logWarning(
            QString("Failed to delete mount points for %1").arg(volumePath).toStdString());
    }

    // Lock the volume with retry
    HANDLE volumeHandle = INVALID_HANDLE_VALUE;
    const bool locked = retryWithBackoff([&]() {
        volumeHandle = lockVolume(volumePath);
        return volumeHandle != INVALID_HANDLE_VALUE;
    });

    if (!locked) {
        m_lastError = QString("Failed to lock volume %1: %2").arg(volumePath).arg(m_lastError);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    // Dismount the volume with retry
    const bool dismounted = retryWithBackoff([&]() { return dismountVolume(volumeHandle); });

    if (!dismounted) {
        m_lastError = QString("Failed to dismount volume %1: %2").arg(volumePath).arg(m_lastError);
        sak::logError(m_lastError.toStdString());
        CloseHandle(volumeHandle);
        return false;
    }

    // Keep handle open until we're done with the drive
    m_lockedVolumes.insert(volumePath, volumeHandle);
    sak::logInfo(QString("Successfully unmounted %1").arg(volumePath).toStdString());
    return true;
}

QStringList DriveUnmounter::getVolumesOnDrive(int driveNumber, bool* enumerationOk) const {
    QStringList volumes;
    setEnumerationOk(enumerationOk, true);

    if (driveNumber < 0) {
        // Not a physical-drive number. Refuse instead of reporting an authoritative empty
        // list, which a caller reads as "nothing mounted here, safe to raw-write"; a negative
        // value would also alias the kVolumeUnopenable/kVolumeProbeFailed sentinels.
        sak::logError(QString("Refusing volume enumeration for invalid drive number %1")
                          .arg(driveNumber)
                          .toStdString());
        setEnumerationOk(enumerationOk, false);
        return volumes;
    }

    // Enumerate all volumes using FindFirstVolume/FindNextVolume
    WCHAR volumeName[MAX_PATH];
    HANDLE hFind = FindFirstVolumeW(volumeName, ARRAYSIZE(volumeName));
    if (hFind == INVALID_HANDLE_VALUE) {
        // Distinguish an enumeration failure from a genuinely empty result so the
        // caller can fail closed instead of proceeding to raw-write a mounted disk.
        setEnumerationOk(enumerationOk, false);
        return volumes;
    }

    do {
        bool fatal = false;
        if (!classifyEnumeratedVolume(volumeName, driveNumber, volumes, &fatal)) {
            if (fatal) {
                setEnumerationOk(enumerationOk, false);
            }
            FindVolumeClose(hFind);
            return volumes;
        }
    } while (FindNextVolumeW(hFind, volumeName, ARRAYSIZE(volumeName)) != 0);

    // FindNextVolumeW returns FALSE at the end of enumeration; only ERROR_NO_MORE_FILES
    // is a clean end. Any other error means the list may be incomplete -> fail closed.
    if (GetLastError() != ERROR_NO_MORE_FILES) {
        setEnumerationOk(enumerationOk, false);
    }

    FindVolumeClose(hFind);
    return volumes;
}

HANDLE DriveUnmounter::lockVolume(const QString& volumePath) {
    const std::wstring wVolumePath = volumePath.toStdWString();

    // Open the volume
    HANDLE hVolume = CreateFileW(wVolumePath.c_str(),
                                 GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr,
                                 OPEN_EXISTING,
                                 0,
                                 nullptr);

    if (hVolume == INVALID_HANDLE_VALUE) {
        m_lastError = QString("CreateFile failed: error %1").arg(GetLastError());
        return INVALID_HANDLE_VALUE;
    }

    // Lock the volume
    DWORD bytesReturned = 0;
    if (DeviceIoControl(
            hVolume, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &bytesReturned, nullptr) == 0) {
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
    if (DeviceIoControl(
            volumeHandle, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &bytesReturned, nullptr) ==
        0) {
        m_lastError = QString("FSCTL_DISMOUNT_VOLUME failed: error %1").arg(GetLastError());
        return false;
    }

    return true;
}

bool DriveUnmounter::deleteMountPoints(const QString& volumePath) {
    // Remove the TARGET volume's OWN mount points (drive letters / mounted
    // folders). GetVolumePathNamesForVolumeNameW returns the paths that resolve TO
    // this volume; FindFirstVolumeMountPointW would instead enumerate volumes
    // mounted INSIDE this volume, which are not what must be detached before a
    // raw write. The volume GUID name must carry a trailing backslash.
    std::wstring volumeName = volumePath.toStdWString();
    if (volumeName.empty() || volumeName.back() != L'\\') {
        volumeName.push_back(L'\\');
    }

    DWORD charCount = 0;
    if (GetVolumePathNamesForVolumeNameW(volumeName.c_str(), nullptr, 0, &charCount) != 0) {
        // Success with a zero-length buffer means there are no mount points.
        return true;
    }
    const DWORD firstError = GetLastError();
    if (firstError != ERROR_MORE_DATA) {
        // Do NOT treat access-denied / invalid-parameter as "no mount points" (a
        // fail-open coercion that would drop this volume's mount points silently).
        // Fail closed: report the real failure. ERROR_MORE_DATA is the ONLY expected
        // outcome here (the zero-length probe could not hold the path-name list).
        sak::logWarning(QString("GetVolumePathNamesForVolumeNameW probe failed for %1: error %2")
                            .arg(volumePath)
                            .arg(firstError)
                            .toStdString());
        return false;
    }
    if (charCount == 0) {
        return true;  // MORE_DATA with a zero required length -> genuinely no mount points.
    }

    std::vector<wchar_t> names(charCount, L'\0');
    if (GetVolumePathNamesForVolumeNameW(volumeName.c_str(), names.data(), charCount, &charCount) ==
        0) {
        sak::logWarning(QString("GetVolumePathNamesForVolumeNameW failed for %1: error %2")
                            .arg(volumePath)
                            .arg(GetLastError())
                            .toStdString());
        return false;
    }
    return deleteVolumePathNames(names.data(), names.size());
}

bool DriveUnmounter::deleteVolumePathNames(const wchar_t* multiSz, size_t count) {
    // deleteMountPoints is the only caller; it passes names.data() of a vector sized to the
    // charCount GetVolumePathNamesForVolumeNameW reported, having already rejected charCount 0.
    Q_ASSERT(multiSz != nullptr);
    // multiSz is a double-null-terminated list of path names, each already ending
    // in a trailing backslash as required by DeleteVolumeMountPointW.
    const wchar_t* const end = multiSz + count;
    bool allSucceeded = true;
    for (const wchar_t* p = multiSz; p < end && *p != L'\0';) {
        if (DeleteVolumeMountPointW(p) == 0) {
            sak::logWarning(QString("Failed to delete mount point: %1")
                                .arg(QString::fromWCharArray(p))
                                .toStdString());
            allSucceeded = false;
        }
        p += wcslen(p) + 1;
    }
    return allSucceeded;
}

bool DriveUnmounter::preventAutoMount(int driveNumber) {
    // Open the physical drive
    const QString drivePath = QString("\\\\.\\PhysicalDrive%1").arg(driveNumber);
    const std::wstring wDrivePath = drivePath.toStdWString();

    HANDLE hDrive = CreateFileW(wDrivePath.c_str(),
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                OPEN_EXISTING,
                                0,
                                nullptr);

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
    const bool success = DeviceIoControl(hDrive,
                                         IOCTL_DISK_SET_DISK_ATTRIBUTES,
                                         &attributes,
                                         sizeof(attributes),
                                         nullptr,
                                         0,
                                         &bytesReturned,
                                         nullptr) != 0;

    if (!success) {
        m_lastError =
            QString("IOCTL_DISK_SET_DISK_ATTRIBUTES failed: error %1").arg(GetLastError());
    }

    CloseHandle(hDrive);
    return success;
}

bool DriveUnmounter::allowAutoMount(int driveNumber) {
    // Inverse of preventAutoMount: clear the persistent OFFLINE attribute so a
    // drive taken offline during a failed unmount is brought back online.
    const QString drivePath = QString("\\\\.\\PhysicalDrive%1").arg(driveNumber);
    const std::wstring wDrivePath = drivePath.toStdWString();

    HANDLE hDrive = CreateFileW(wDrivePath.c_str(),
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                OPEN_EXISTING,
                                0,
                                nullptr);

    if (hDrive == INVALID_HANDLE_VALUE) {
        m_lastError = QString("Failed to open drive: error %1").arg(GetLastError());
        return false;
    }

    SET_DISK_ATTRIBUTES attributes = {};
    attributes.Version = sizeof(SET_DISK_ATTRIBUTES);
    attributes.Persist = TRUE;
    attributes.Attributes = 0;
    attributes.AttributesMask = DISK_ATTRIBUTE_OFFLINE;

    DWORD bytesReturned = 0;
    const bool success = DeviceIoControl(hDrive,
                                         IOCTL_DISK_SET_DISK_ATTRIBUTES,
                                         &attributes,
                                         sizeof(attributes),
                                         nullptr,
                                         0,
                                         &bytesReturned,
                                         nullptr) != 0;

    if (!success) {
        m_lastError =
            QString("IOCTL_DISK_SET_DISK_ATTRIBUTES (online) failed: error %1").arg(GetLastError());
    }

    CloseHandle(hDrive);
    return success;
}

bool DriveUnmounter::ejectDrive(int driveNumber) {
    if (driveNumber < 0) {
        m_lastError = QString("Refusing to eject invalid drive number %1").arg(driveNumber);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    // Fail closed on a failed enumeration exactly as unmountDrive does: an empty
    // list from a failed enumeration would mean "no volumes to dismount, eject
    // away", which is how a mounted disk gets reported as safe to unplug.
    bool enumerationOk = true;
    const QStringList volumes = getVolumesOnDrive(driveNumber, &enumerationOk);
    if (!enumerationOk) {
        m_lastError =
            QString("Failed to enumerate volumes on drive %1 before eject").arg(driveNumber);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    Q_EMIT statusMessage(QString("Ejecting drive %1...").arg(driveNumber));
    for (const QString& volumePath : volumes) {
        if (!lockAndDismountForEject(volumePath)) {
            closeLockedVolumeHandles();
            return false;
        }
    }

    const bool ejected = issueEjectIoctls(driveNumber);
    // Release the locks either way. Holding them after a failed eject would leave
    // the drive unusable for the very retry the caller is about to report.
    closeLockedVolumeHandles();
    if (ejected) {
        sak::logInfo(QString("Ejected drive %1").arg(driveNumber).toStdString());
    }
    return ejected;
}

bool DriveUnmounter::lockAndDismountForEject(const QString& volumePath) {
    // Fewer attempts than the pre-write unmount deliberately. This runs after the
    // write, on the UI thread, and its failure costs the user a manual "Safely
    // Remove Hardware" -- not a corrupted disk. The full five-attempt backoff would
    // block the UI for seconds per volume to save an outcome that is already safe.
    HANDLE volumeHandle = INVALID_HANDLE_VALUE;
    const bool locked = retryWithBackoff(
        [&]() {
            volumeHandle = lockVolume(volumePath);
            return volumeHandle != INVALID_HANDLE_VALUE;
        },
        kEjectRetryAttempts);
    if (!locked) {
        m_lastError =
            QString("Failed to lock volume %1 for eject: %2").arg(volumePath, m_lastError);
        sak::logWarning(m_lastError.toStdString());
        return false;
    }

    if (!retryWithBackoff([&]() { return dismountVolume(volumeHandle); }, kEjectRetryAttempts)) {
        m_lastError =
            QString("Failed to dismount volume %1 for eject: %2").arg(volumePath, m_lastError);
        sak::logWarning(m_lastError.toStdString());
        CloseHandle(volumeHandle);
        return false;
    }

    // Hold the handle until the eject has been issued: releasing the lock here
    // lets Windows remount the volume in the gap and the eject then fails.
    m_lockedVolumes.insert(volumePath, volumeHandle);
    return true;
}

bool DriveUnmounter::issueEjectIoctls(int driveNumber) {
    const QString drivePath = QString("\\\\.\\PhysicalDrive%1").arg(driveNumber);
    const std::wstring wDrivePath = drivePath.toStdWString();

    HANDLE hDrive = CreateFileW(wDrivePath.c_str(),
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                OPEN_EXISTING,
                                0,
                                nullptr);
    if (hDrive == INVALID_HANDLE_VALUE) {
        m_lastError = QString("Failed to open drive %1 for eject: error %2")
                          .arg(driveNumber)
                          .arg(GetLastError());
        return false;
    }

    // Clear any software removal lock first. Not every device has one, so this is
    // informational rather than fatal -- the eject below is the operation whose
    // result is reported.
    DWORD bytesReturned = 0;
    PREVENT_MEDIA_REMOVAL allowRemoval = {};
    allowRemoval.PreventMediaRemoval = FALSE;
    if (DeviceIoControl(hDrive,
                        IOCTL_STORAGE_MEDIA_REMOVAL,
                        &allowRemoval,
                        sizeof(allowRemoval),
                        nullptr,
                        0,
                        &bytesReturned,
                        nullptr) == 0) {
        sak::logInfo(QString("IOCTL_STORAGE_MEDIA_REMOVAL not honoured on drive %1: error %2")
                         .arg(driveNumber)
                         .arg(GetLastError())
                         .toStdString());
    }

    const bool ejected =
        DeviceIoControl(
            hDrive, IOCTL_STORAGE_EJECT_MEDIA, nullptr, 0, nullptr, 0, &bytesReturned, nullptr) !=
        FALSE;
    if (!ejected) {
        m_lastError = QString("IOCTL_STORAGE_EJECT_MEDIA failed on drive %1: error %2")
                          .arg(driveNumber)
                          .arg(GetLastError());
    }

    CloseHandle(hDrive);
    return ejected;
}

bool DriveUnmounter::retryWithBackoff(std::function<bool()> operation, int maxAttempts) {
    int delay_ms = kBackoffInitialDelayMs;

    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        if (operation()) {
            return true;
        }

        if (attempt < maxAttempts) {
            sak::logInfo(QString("Retry attempt %1/%2, waiting %3ms")
                             .arg(attempt)
                             .arg(maxAttempts)
                             .arg(delay_ms)
                             .toStdString());
            QThread::msleep(delay_ms);
            delay_ms *= kBackoffMultiplier;
        }
    }

    return false;
}

bool DriveUnmounter::closeAllHandles(int driveNumber) {
    // Reached only from unmountDrive (via advisoryCloseHandles), which has already returned
    // false for a negative drive number -- getVolumesOnDrive rejects one.
    Q_ASSERT(driveNumber >= 0);
    // Releasing the exclusive volume locks here (before the raw write) is BY DESIGN:
    // the authoritative, persistent concurrent-access guard is the DISK_ATTRIBUTE_OFFLINE
    // set by preventAutoMount(), which survives the flash and blocks Windows auto-mount.
    // The volume locks were belt-and-suspenders; they cannot be held across the separate
    // FlashWorker write handle anyway. Fail-closed protection does not depend on them.
    for (auto it = m_lockedVolumes.constBegin(); it != m_lockedVolumes.constEnd(); ++it) {
        if (it.value() != INVALID_HANDLE_VALUE) {
            CloseHandle(it.value());
        }
    }
    m_lockedVolumes.clear();

    DWORD dwSession;
    WCHAR szSessionKey[CCH_RM_SESSION_KEY + 1] = {0};
    DWORD const dwError = RmStartSession(&dwSession, 0, szSessionKey);

    if (dwError != ERROR_SUCCESS) {
        // Honest outcome: the Restart Manager pass could not run at all.
        sak::logWarning(
            QString("Failed to start Restart Manager session: %1").arg(dwError).toStdString());
        return false;
    }

    bool enumerationOk = true;
    const QStringList mountPoints = findVolumesForDrive(driveNumber, &enumerationOk);
    if (!enumerationOk) {
        // An incomplete list would leave real holders unregistered and the pass would
        // still report "nothing left holding a handle". Report the truth instead.
        sak::logWarning(QString("Volume enumeration for the Restart Manager pass on drive %1 was "
                                "incomplete")
                            .arg(driveNumber)
                            .toStdString());
        RmEndSession(dwSession);
        return false;
    }
    const bool rmOk = shutdownHandlesViaRestartManager(dwSession, mountPoints);

    RmEndSession(dwSession);
    return rmOk;
}

QStringList DriveUnmounter::findVolumesForDrive(int driveNumber, bool* enumerationOk) const {
    // closeAllHandles is the only caller and passes the number unmountDrive validated. The
    // comparison below must never see a negative one: it would alias kVolumeUnopenable (-1).
    Q_ASSERT(driveNumber >= 0);
    QStringList mountPoints;
    setEnumerationOk(enumerationOk, true);
    wchar_t volumeName[MAX_PATH];
    HANDLE hFind = FindFirstVolumeW(volumeName, MAX_PATH);

    if (hFind == INVALID_HANDLE_VALUE) {
        setEnumerationOk(enumerationOk, false);
        return mountPoints;
    }

    do {
        // Strip the trailing backslash BEFORE probing: CreateFileW on a volume
        // GUID path fails when the path carries one, so probing with the slash
        // would never match and the RM pass would register no resources.
        const size_t len = wcslen(volumeName);
        if (len > 0 && volumeName[len - 1] == L'\\') {
            volumeName[len - 1] = L'\0';
        }
        const int volumeDrive = queryVolumeDriveNumber(volumeName);
        if (volumeDrive == kVolumeProbeFailed) {
            // Opened, but its physical-disk identity could not be read: it may well sit on
            // the target, so the resource list handed to the Restart Manager is short.
            setEnumerationOk(enumerationOk, false);
            continue;
        }
        if (volumeDrive != driveNumber) {
            continue;
        }
        mountPoints.append(QString::fromWCharArray(volumeName));
    } while (FindNextVolumeW(hFind, volumeName, MAX_PATH) != 0);

    // Only ERROR_NO_MORE_FILES is a clean end of enumeration; any other error means the
    // list may be short (same fail-closed rule as getVolumesOnDrive).
    if (GetLastError() != ERROR_NO_MORE_FILES) {
        setEnumerationOk(enumerationOk, false);
    }

    FindVolumeClose(hFind);
    return mountPoints;
}

bool DriveUnmounter::shutdownHandlesViaRestartManager(DWORD dwSession,
                                                      const QStringList& mountPoints) {
    QVector<LPCWSTR> files;
    for (const QString& mountPoint : mountPoints) {
        files.append(reinterpret_cast<LPCWSTR>(mountPoint.utf16()));
    }

    if (files.isEmpty()) {
        return true;  // Nothing to register -- nothing left holding a handle.
    }

    DWORD dwError = RmRegisterResources(
        dwSession, files.size(), const_cast<LPCWSTR*>(files.data()), 0, nullptr, 0, nullptr);

    if (dwError != ERROR_SUCCESS) {
        sak::logWarning(QString("RmRegisterResources failed: %1").arg(dwError).toStdString());
        return false;
    }

    DWORD dwReason = 0;
    UINT nProcInfoNeeded = 0;
    // In/out: on input the CAPACITY of rgpi, on output how many entries were filled.
    // Passing 0 here (as this did) leaves the array untouched, so the holder processes
    // the force-shutdown below is about to kill could never be named.
    UINT nProcInfo = static_cast<UINT>(kRestartManagerProcessCapacity);
    RM_PROCESS_INFO rgpi[kRestartManagerProcessCapacity] = {};

    dwError = RmGetList(dwSession, &nProcInfoNeeded, &nProcInfo, rgpi, &dwReason);

    if ((dwError != ERROR_SUCCESS && dwError != ERROR_MORE_DATA) || nProcInfoNeeded == 0) {
        // Could not list holders (or none held a handle): report the real outcome.
        return dwError == ERROR_SUCCESS || dwError == ERROR_MORE_DATA;
    }

    sak::logInfo(
        QString("Found %1 processes with open handles").arg(nProcInfoNeeded).toStdString());
    // rgpi is populated only when the whole list fit; ERROR_MORE_DATA means there were
    // more holders than the fixed capacity, and then nothing was copied.
    logRestartManagerHolders(rgpi, dwError == ERROR_SUCCESS ? nProcInfo : 0, nProcInfoNeeded);
    dwError = RmShutdown(dwSession, RmForceShutdown, nullptr);
    if (dwError == ERROR_SUCCESS) {
        sak::logInfo("Successfully closed all file handles");
        return true;
    }
    sak::logWarning(QString("Failed to close handles: %1").arg(dwError).toStdString());
    return false;
}

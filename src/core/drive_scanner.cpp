// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/drive_scanner.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QtConcurrentRun>
#include <QtGlobal>
#include <QVector>

#include <algorithm>
#include <cwchar>
#include <vector>

#include <cfgmgr32.h>
#include <dbt.h>
#include <ntddscsi.h>
#include <setupapi.h>
#include <winioctl.h>

#pragma comment(lib, "setupapi.lib")

namespace {
constexpr int kDosDeviceInitialBufferChars = 32'768;
constexpr int kDosDeviceQueryAttempts = 3;
constexpr int kDosDeviceBufferGrowthFactor = 2;
constexpr DWORD kStorageDescriptorBufferBytes = 1024;
constexpr int kVolumePathBufferMultiplier = 4;
// One initial query plus retries after ERROR_MORE_DATA (the mount table can change between
// attempts); bounded so a volume whose path list keeps growing cannot spin forever.
constexpr int kVolumePathQueryAttempts = 3;

/// @brief Check if any drive in the list has the given devicePath
bool containsDevicePath(const QList<sak::DriveInfo>& drives, const QString& devicePath) {
    return std::any_of(drives.begin(), drives.end(), [&devicePath](const auto& drive) {
        return drive.devicePath == devicePath;
    });
}

// @param query_ok Set true when QueryDosDeviceW returned the real device-name table (the drive
// list is authoritative); false when every attempt failed and the returned {0} fallback is a
// best-effort guess. Lets callers report an incomplete enumeration honestly.
QVector<int> enumeratePhysicalDriveNumbers(bool& query_ok) {
    query_ok = false;
    QVector<int> drive_numbers;
    std::vector<wchar_t> buffer(kDosDeviceInitialBufferChars);

    for (int attempt = 0; attempt < kDosDeviceQueryAttempts; ++attempt) {
        const DWORD chars =
            QueryDosDeviceW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (chars != 0) {
            query_ok = true;
            const wchar_t* current = buffer.data();
            while (*current != L'\0') {
                const QString name = QString::fromWCharArray(current);
                const QString prefix = QStringLiteral("PhysicalDrive");
                if (name.startsWith(prefix, Qt::CaseInsensitive)) {
                    bool ok = false;
                    const int number = name.mid(prefix.size()).toInt(&ok);
                    if (ok && number >= 0) {
                        drive_numbers.append(number);
                    }
                }
                current += std::wcslen(current) + 1;
            }
            break;
        }

        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            break;
        }
        buffer.resize(buffer.size() * kDosDeviceBufferGrowthFactor);
    }

    std::sort(drive_numbers.begin(), drive_numbers.end());
    drive_numbers.erase(std::unique(drive_numbers.begin(), drive_numbers.end()),
                        drive_numbers.end());

    if (drive_numbers.isEmpty()) {
        // No authoritative PhysicalDrive entry (the query failed, or the table had none). Do
        // NOT coerce to a probe of drive 0: guessing a device number is a fail-open default.
        // Report the enumeration as non-authoritative and return an empty set so callers keep
        // their cached drives instead of dropping them (mergeDriveList / drivesDetached).
        query_ok = false;
    }

    return drive_numbers;
}
}  // anonymous namespace

DriveScanner* DriveScanner::s_instance = nullptr;

DriveScanner::DriveScanner(QObject* parent)
    : QObject(parent)
    , m_refreshTimer(new QTimer(this))
    , m_notificationWindow(nullptr)
    , m_deviceNotify(nullptr)
    , m_isScanning(false) {
    Q_ASSERT_X(s_instance == nullptr, "DriveScanner", "Only one DriveScanner instance is allowed");
    s_instance = this;

    // Refresh every 5 seconds as fallback
    m_refreshTimer->setInterval(sak::kTimerRefreshMs);
    connect(m_refreshTimer, &QTimer::timeout, this, &DriveScanner::onRefreshTimer);
    connect(&m_scanWatcher,
            &QFutureWatcher<sak::DriveScanResult>::finished,
            this,
            &DriveScanner::onScanFinished);
}

DriveScanner::~DriveScanner() {
    stop();
    s_instance = nullptr;
}

void DriveScanner::start() {
    // The constructor creates m_refreshTimer (new QTimer(this)) and nothing ever reassigns it.
    Q_ASSERT(m_refreshTimer);
    sak::logInfo("Starting drive scanner");

    // Initial scan
    scanDrives();

    // Register for device notifications
    registerDeviceNotification();

    // Start refresh timer
    m_refreshTimer->start();
}

void DriveScanner::stop() {
    sak::logInfo("Stopping drive scanner");

    m_refreshTimer->stop();
    // Block until any in-flight worker scan finishes so its lambda does not
    // touch this object after destruction.
    if (m_scanWatcher.isRunning()) {
        m_scanWatcher.waitForFinished();
    }
    unregisterDeviceNotification();
    m_drives.clear();
}

void DriveScanner::refresh() {
    scanDrives();
}

QList<sak::DriveInfo> DriveScanner::getDrives() const {
    return m_drives;
}

QList<sak::DriveInfo> DriveScanner::getRemovableDrives() const {
    QList<sak::DriveInfo> removable;
    for (const auto& drive : m_drives) {
        if (drive.isRemovable) {
            removable.append(drive);
        }
    }
    return removable;
}

sak::DriveInfo DriveScanner::getDriveInfo(const QString& devicePath) const {
    auto it = std::find_if(m_drives.begin(), m_drives.end(), [&devicePath](const auto& d) {
        return d.devicePath == devicePath;
    });
    return it != m_drives.end() ? *it : sak::DriveInfo{};
}

bool DriveScanner::isSystemDrive(const QString& devicePath) const {
    sak::DriveInfo info = getDriveInfo(devicePath);
    return info.isValid() && info.isSystem;
}

void DriveScanner::onRefreshTimer() {
    scanDrives();
}

void DriveScanner::scanDrives() {
    bool expected = false;
    if (!m_isScanning.compare_exchange_strong(expected, true)) {
        return;  // A scan is already in flight.
    }
    // Enumerate/query drives off the UI thread; each drive query issues blocking
    // Win32 IOCTLs, so doing this inline would freeze the interface. Shares the exact
    // one-shot enumeration used by the headless imaging.list_drives path.
    m_scanWatcher.setFuture(QtConcurrent::run([]() {
        sak::DriveScanResult result;
        result.drives = DriveScanner::enumerateDrivesOnce(result.enumeration_ok);
        return result;
    }));
}

QList<sak::DriveInfo> DriveScanner::enumerateDrivesOnce(bool& enumeration_ok) {
    QList<sak::DriveInfo> newDrives;
    const QVector<int> drive_numbers = enumeratePhysicalDriveNumbers(enumeration_ok);
    for (const int driveNumber : drive_numbers) {
        sak::DriveInfo info = queryDriveInfo(driveNumber);
        if (info.isValid()) {
            newDrives.append(info);
            continue;
        }
        // The device table listed this drive but its per-disk query (CreateFile/IOCTL)
        // failed THIS pass. That is not proof the drive is gone -- treat the scan as
        // non-authoritative so a transient failure does not spuriously detach a cached
        // drive (a genuinely-removed drive simply drops out of the table on a later pass).
        enumeration_ok = false;
    }
    return newDrives;
}

void DriveScanner::onScanFinished() {
    const sak::DriveScanResult result = m_scanWatcher.result();
    applyDriveScan(result.drives, result.enumeration_ok);
    m_isScanning = false;
}

QList<sak::DriveInfo> DriveScanner::drivesDetached(const QList<sak::DriveInfo>& current,
                                                   const QList<sak::DriveInfo>& scanned,
                                                   bool enumeration_ok) {
    QList<sak::DriveInfo> detached;
    if (!enumeration_ok) {
        // The scan was partial/probe-only. Treating a drive missing from it as "detached" would
        // spuriously drop every real drive on a transient enumeration failure -- fail closed.
        return detached;
    }
    for (const auto& drive : current) {
        if (!containsDevicePath(scanned, drive.devicePath)) {
            detached.append(drive);
        }
    }
    return detached;
}

QList<sak::DriveInfo> DriveScanner::mergeDriveList(const QList<sak::DriveInfo>& current,
                                                   const QList<sak::DriveInfo>& scanned,
                                                   bool enumeration_ok) {
    if (enumeration_ok) {
        return scanned;  // Authoritative: the scan IS the drive list.
    }
    // Non-authoritative: never drop cached drives; only add ones the partial scan newly saw.
    QList<sak::DriveInfo> merged = current;
    for (const auto& drive : scanned) {
        if (!containsDevicePath(merged, drive.devicePath)) {
            merged.append(drive);
        }
    }
    return merged;
}

void DriveScanner::applyDriveScan(const QList<sak::DriveInfo>& newDrives, bool enumeration_ok) {
    bool hasChanges = false;

    // Removed drives -- only when the scan was authoritative (see drivesDetached).
    for (const auto& oldDrive : drivesDetached(m_drives, newDrives, enumeration_ok)) {
        sak::logInfo(QString("Drive detached: %1").arg(oldDrive.devicePath).toStdString());
        Q_EMIT driveDetached(oldDrive.devicePath);
        hasChanges = true;
    }

    // New drives (a freshly-seen drive is real even under a partial scan -- attach is additive),
    // and in-place property changes on an already-known drive (size/read-only/system/mount-point
    // changes on the SAME devicePath) -- both must refresh subscribers via drivesUpdated.
    for (const auto& newDrive : newDrives) {
        const auto it = std::find_if(m_drives.begin(), m_drives.end(), [&newDrive](const auto& d) {
            return d.devicePath == newDrive.devicePath;
        });
        if (it == m_drives.end()) {
            sak::logInfo(QString("Drive attached: %1 (%2)")
                             .arg(newDrive.devicePath, newDrive.name)
                             .toStdString());
            Q_EMIT driveAttached(newDrive);
            hasChanges = true;
        } else if (driveInfoChanged(*it, newDrive)) {
            sak::logInfo(
                QString("Drive properties changed: %1").arg(newDrive.devicePath).toStdString());
            hasChanges = true;
        }
    }

    m_drives = mergeDriveList(m_drives, newDrives, enumeration_ok);

    if (hasChanges) {
        Q_EMIT drivesUpdated(m_drives);
    }
}

bool DriveScanner::driveInfoChanged(const sak::DriveInfo& a, const sak::DriveInfo& b) {
    return a.size != b.size || a.blockSize != b.blockSize || a.name != b.name ||
           a.isReadOnly != b.isReadOnly || a.isSystem != b.isSystem ||
           a.isRemovable != b.isRemovable || a.mountPoints != b.mountPoints ||
           a.volumeLabel != b.volumeLabel || a.busType != b.busType;
}

sak::DriveInfo DriveScanner::queryDriveInfo(int driveNumber) {
    sak::DriveInfo info;

    // getDriveName, isDriveRemovable, getMountPoints and containsWindowsInstallation all
    // assert a non-negative drive number and name this function as what guarantees it, so
    // make that true here rather than relying on CreateFileW happening to fail on a device
    // path like "\\.\PhysicalDrive-1". A default DriveInfo is what every other failure
    // path in this function returns.
    if (driveNumber < 0) {
        sak::logError("queryDriveInfo refused a negative drive number: {}", driveNumber);
        return info;
    }

    info.devicePath = QString("\\\\.\\PhysicalDrive%1").arg(driveNumber);

    // Try to open drive
    HANDLE hDrive = CreateFileW(reinterpret_cast<LPCWSTR>(info.devicePath.utf16()),
                                0,  // No access needed for query
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                OPEN_EXISTING,
                                0,
                                nullptr);

    if (hDrive == INVALID_HANDLE_VALUE) {
        return sak::DriveInfo{};  // Drive doesn't exist or can't be accessed
    }

    // Get drive geometry and size
    info.size = getDriveSize(hDrive);
    info.blockSize = getBlockSize(hDrive);
    info.name = getDriveName(driveNumber);
    info.busType = getBusType(hDrive);
    info.isRemovable = isDriveRemovable(driveNumber);
    info.isReadOnly = isDriveReadOnly(hDrive);
    info.isSystem = containsWindowsInstallation(driveNumber);
    // The completeness flag travels with the record: a failed/partial volume enumeration is
    // reported honestly instead of surfacing as a confident "no mount points".
    info.mountPoints = getMountPoints(driveNumber, &info.mountPointsComplete);

    if (!info.mountPoints.isEmpty()) {
        info.volumeLabel = getVolumeLabel(info.mountPoints.first());
    }

    CloseHandle(hDrive);

    return info;
}

QString DriveScanner::descriptorString(const BYTE* buffer,
                                       DWORD buffer_size,
                                       DWORD bytes_returned,
                                       DWORD field_offset) {
    if (field_offset == 0) {
        return QString();  // Field absent.
    }
    // Never trust the driver's byte count past our own buffer.
    const DWORD limit = std::min(bytes_returned, buffer_size);
    if (field_offset >= limit) {
        return QString();  // Offset points outside the returned data -- would be an OOB read.
    }
    const DWORD max_len = limit - field_offset;
    const char* start = reinterpret_cast<const char*>(buffer + field_offset);
    DWORD len = 0;
    while (len < max_len && start[len] != '\0') {
        ++len;
    }
    return QString::fromLatin1(start, static_cast<int>(len));
}

QString DriveScanner::getDriveName(int driveNumber) {
    // Only queryDriveInfo calls this, with a number enumeratePhysicalDriveNumbers accepted (>= 0).
    Q_ASSERT_X(driveNumber >= 0, "getDriveName", "driveNumber must be non-negative");
    QString devicePath = QString("\\\\.\\PhysicalDrive%1").arg(driveNumber);
    HANDLE hDrive = CreateFileW(reinterpret_cast<LPCWSTR>(devicePath.utf16()),
                                0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                OPEN_EXISTING,
                                0,
                                nullptr);

    if (hDrive == INVALID_HANDLE_VALUE) {
        return QString("Physical Drive %1").arg(driveNumber);
    }

    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    BYTE buffer[kStorageDescriptorBufferBytes] = {};
    DWORD bytesReturned = 0;

    if (DeviceIoControl(hDrive,
                        IOCTL_STORAGE_QUERY_PROPERTY,
                        &query,
                        sizeof(query),
                        buffer,
                        sizeof(buffer),
                        &bytesReturned,
                        nullptr) &&
        bytesReturned >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        const auto* desc = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer);

        // Vendor/Product are byte offsets into the returned buffer supplied by the driver; a buggy
        // or hostile driver could point them past the data, so read them bounds-checked, not raw.
        const QString vendor =
            descriptorString(buffer, sizeof(buffer), bytesReturned, desc->VendorIdOffset).trimmed();
        const QString product =
            descriptorString(buffer, sizeof(buffer), bytesReturned, desc->ProductIdOffset)
                .trimmed();

        CloseHandle(hDrive);

        if (!vendor.isEmpty() && !product.isEmpty()) {
            return QString("%1 %2").arg(vendor, product);
        } else if (!product.isEmpty()) {
            return product;
        } else if (!vendor.isEmpty()) {
            return vendor;
        }
    } else {
        CloseHandle(hDrive);
    }

    return QString("Physical Drive %1").arg(driveNumber);
}

qint64 DriveScanner::getDriveSize(HANDLE hDrive) {
    // queryDriveInfo, the only caller, returns early when CreateFileW fails -- hDrive is open here.
    Q_ASSERT_X(hDrive != INVALID_HANDLE_VALUE, "getDriveSize", "hDrive must be a valid handle");
    DISK_GEOMETRY_EX geometry = {};
    DWORD bytesReturned = 0;

    if (DeviceIoControl(hDrive,
                        IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                        nullptr,
                        0,
                        &geometry,
                        sizeof(geometry),
                        &bytesReturned,
                        nullptr)) {
        return geometry.DiskSize.QuadPart;
    }

    return 0;
}

quint32 DriveScanner::getBlockSize(HANDLE hDrive) {
    // queryDriveInfo, the only caller, returns early when CreateFileW fails -- hDrive is open here.
    Q_ASSERT_X(hDrive != INVALID_HANDLE_VALUE, "getBlockSize", "hDrive must be a valid handle");
    DISK_GEOMETRY geometry = {};
    DWORD bytesReturned = 0;

    if (DeviceIoControl(hDrive,
                        IOCTL_DISK_GET_DRIVE_GEOMETRY,
                        nullptr,
                        0,
                        &geometry,
                        sizeof(geometry),
                        &bytesReturned,
                        nullptr)) {
        return geometry.BytesPerSector;
    }

    // No default coercion: a failed geometry query returns 0 (unknown) rather than a
    // guessed 512. This field is display-only; the flash path queries the real logical
    // sector size itself, fail-closed, in FlashWorker::queryDeviceSectorSize().
    return 0;
}

QString DriveScanner::getBusType(HANDLE hDrive) {
    // queryDriveInfo, the only caller, returns early when CreateFileW fails -- hDrive is open here.
    Q_ASSERT_X(hDrive != INVALID_HANDLE_VALUE, "getBusType", "hDrive must be a valid handle");
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    BYTE buffer[kStorageDescriptorBufferBytes] = {};
    DWORD bytesReturned = 0;

    if (DeviceIoControl(hDrive,
                        IOCTL_STORAGE_QUERY_PROPERTY,
                        &query,
                        sizeof(query),
                        buffer,
                        sizeof(buffer),
                        &bytesReturned,
                        nullptr)) {
        const auto* desc = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer);

        switch (desc->BusType) {
        case BusTypeUsb:
            return "USB";
        case BusTypeAta:
            return "ATA";
        case BusTypeSata:
            return "SATA";
        case BusTypeNvme:
            return "NVMe";
        case BusTypeSd:
            return "SD";
        case BusTypeMmc:
            return "MMC";
        default:
            return "Unknown";
        }
    }

    return "Unknown";
}

bool DriveScanner::isDriveRemovable(int driveNumber) {
    // Callers pass a number enumeratePhysicalDriveNumbers accepted (>= 0); physicalDriveBootProbe
    // rejects a negative one before it can reach here.
    Q_ASSERT_X(driveNumber >= 0, "isDriveRemovable", "driveNumber must be non-negative");
    // Use IOCTL_STORAGE_QUERY_PROPERTY to check both RemovableMedia flag and BusType
    QString devicePath = QString("\\\\.\\PhysicalDrive%1").arg(driveNumber);
    HANDLE hDrive = CreateFileW(reinterpret_cast<LPCWSTR>(devicePath.utf16()),
                                0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                OPEN_EXISTING,
                                0,
                                nullptr);

    if (hDrive == INVALID_HANDLE_VALUE) {
        return false;
    }

    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    BYTE buffer[kStorageDescriptorBufferBytes] = {};
    DWORD bytesReturned = 0;

    bool removable = false;

    if (DeviceIoControl(hDrive,
                        IOCTL_STORAGE_QUERY_PROPERTY,
                        &query,
                        sizeof(query),
                        buffer,
                        sizeof(buffer),
                        &bytesReturned,
                        nullptr)) {
        const auto* desc = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer);

        // Primary: use the RemovableMedia flag from the device descriptor
        if (desc->RemovableMedia) {
            removable = true;
        }

        // Secondary: certain bus types are inherently removable
        switch (desc->BusType) {
        case BusTypeUsb:
        case BusTypeSd:
        case BusTypeMmc:
        case BusType1394:  // FireWire
            removable = true;
            break;
        default:
            break;
        }
    }

    CloseHandle(hDrive);
    return removable;
}

bool DriveScanner::driveReadOnlyFromProbe(bool is_writable_ioctl_ok,
                                          DWORD is_writable_error,
                                          bool length_ok,
                                          qint64 length_bytes) {
    if (is_writable_ioctl_ok) {
        return false;  // Only path that AFFIRMATIVELY confirms the drive is writable.
    }
    if (is_writable_error == ERROR_WRITE_PROTECT) {
        return true;  // Hardware write-protect.
    }
    if (length_ok && length_bytes == 0) {
        return true;  // No media / zero-length volume.
    }
    // Any other failure (access denied, not-ready, unsupported IOCTL) is undeterminable. Report
    // read-only rather than fail open: IOCTL_DISK_IS_WRITABLE is FILE_ANY_ACCESS, so a genuinely
    // writable drive succeeds above -- reaching here means we could not confirm writability.
    return true;
}

bool DriveScanner::isDriveReadOnly(HANDLE hDrive) {
    // queryDriveInfo, the only caller, returns early when CreateFileW fails -- hDrive is open here.
    Q_ASSERT_X(hDrive != INVALID_HANDLE_VALUE, "isDriveReadOnly", "hDrive must be a valid handle");
    DWORD bytesReturned = 0;

    const bool writable_ok =
        DeviceIoControl(
            hDrive, IOCTL_DISK_IS_WRITABLE, nullptr, 0, nullptr, 0, &bytesReturned, nullptr) != 0;
    // Capture the error immediately -- the length IOCTL below would overwrite GetLastError().
    const DWORD writable_error = writable_ok ? ERROR_SUCCESS : GetLastError();

    GET_LENGTH_INFORMATION lengthInfo = {};
    const bool length_ok = DeviceIoControl(hDrive,
                                           IOCTL_DISK_GET_LENGTH_INFO,
                                           nullptr,
                                           0,
                                           &lengthInfo,
                                           sizeof(lengthInfo),
                                           &bytesReturned,
                                           nullptr) != 0;

    return driveReadOnlyFromProbe(
        writable_ok, writable_error, length_ok, lengthInfo.Length.QuadPart);
}

QStringList DriveScanner::getMountPoints(int driveNumber, bool* enumerationOk) {
    // queryDriveInfo passes a number enumeratePhysicalDriveNumbers accepted (>= 0).
    Q_ASSERT_X(driveNumber >= 0, "getMountPoints", "driveNumber must be non-negative");
    QStringList mountPoints;
    bool ok = true;

    wchar_t volumeName[MAX_PATH];
    HANDLE hFind = FindFirstVolumeW(volumeName, MAX_PATH);

    if (hFind == INVALID_HANDLE_VALUE) {
        // Enumeration itself failed: signal it so a caller does not read the empty result as
        // "this drive has no mounted volumes".
        ok = false;
    } else {
        do {
            size_t len = wcslen(volumeName);
            if (len > 0 && volumeName[len - 1] == L'\\') {
                volumeName[len - 1] = L'\0';
            }
            if (volumeMatchesDrive(volumeName, driveNumber) &&
                !collectMountPaths(volumeName, len, mountPoints)) {
                ok = false;
            }
        } while (FindNextVolumeW(hFind, volumeName, MAX_PATH));
        // Only ERROR_NO_MORE_FILES is a clean end of enumeration; any other error means the
        // list may be incomplete -> report non-authoritative so the caller fails closed.
        if (GetLastError() != ERROR_NO_MORE_FILES) {
            ok = false;
        }
        FindVolumeClose(hFind);
    }

    if (enumerationOk != nullptr) {
        *enumerationOk = ok;
    }
    return mountPoints;
}

bool DriveScanner::volumeMatchesDrive(const wchar_t* volumeName, int driveNumber) {
    HANDLE hVolume = CreateFileW(
        volumeName, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hVolume == INVALID_HANDLE_VALUE) {
        return false;
    }

    STORAGE_DEVICE_NUMBER deviceNumber = {};
    DWORD bytesReturned = 0;
    const bool match = DeviceIoControl(hVolume,
                                       IOCTL_STORAGE_GET_DEVICE_NUMBER,
                                       nullptr,
                                       0,
                                       &deviceNumber,
                                       sizeof(deviceNumber),
                                       &bytesReturned,
                                       nullptr) &&
                       static_cast<int>(deviceNumber.DeviceNumber) == driveNumber;
    CloseHandle(hVolume);
    return match;
}

bool DriveScanner::appendVolumeRoot(wchar_t* volumeName, int driveNumber, QStringList& roots) {
    const QString guidRoot = QString::fromWCharArray(volumeName);  // keeps trailing backslash
    size_t len = wcslen(volumeName);
    if (len > 0 && volumeName[len - 1] == L'\\') {
        volumeName[len - 1] = L'\0';
    }
    if (!volumeMatchesDrive(volumeName, driveNumber)) {
        return true;
    }
    QStringList mounts;
    const bool mountsOk = collectMountPaths(volumeName, len, mounts);
    if (mounts.isEmpty()) {
        // Unmounted volume: it has no drive letter, but its \\?\Volume{GUID}\ path is still
        // inspectable -- so an unmounted Windows partition is not missed by the system check.
        // The same GUID path is appended when the mount-path query FAILED (mounts is empty but
        // not authoritative), so the volume is still inspected; mountsOk tells the caller the
        // root list is not authoritative.
        roots.append(guidRoot);
    } else {
        roots += mounts;
    }
    return mountsOk;
}

QStringList DriveScanner::getVolumeRootsForDrive(int driveNumber, bool* enumerationOk) {
    // containsWindowsInstallation passes an enumerated number; physicalDriveBootProbe returns
    // Undetermined for a negative one before reaching here.
    Q_ASSERT_X(driveNumber >= 0, "getVolumeRootsForDrive", "driveNumber must be non-negative");
    QStringList roots;
    bool ok = true;

    wchar_t volumeName[MAX_PATH];
    HANDLE hFind = FindFirstVolumeW(volumeName, MAX_PATH);
    if (hFind == INVALID_HANDLE_VALUE) {
        // Enumeration itself failed: signal it so a fail-closed caller does not read the
        // empty result as "this drive holds no system/boot volume".
        ok = false;
    } else {
        do {
            if (!appendVolumeRoot(volumeName, driveNumber, roots)) {
                ok = false;
            }
        } while (FindNextVolumeW(hFind, volumeName, MAX_PATH));
        // Only ERROR_NO_MORE_FILES is a clean end of enumeration; any other error means the
        // list may be incomplete -> report non-authoritative so the caller fails closed.
        if (GetLastError() != ERROR_NO_MORE_FILES) {
            ok = false;
        }
        FindVolumeClose(hFind);
    }

    if (enumerationOk != nullptr) {
        *enumerationOk = ok;
    }
    return roots;
}

sak::VolumePathQuery DriveScanner::volumePathQueryOutcome(bool query_ok,
                                                          DWORD query_error,
                                                          DWORD required_chars,
                                                          size_t buffer_chars) {
    if (query_ok) {
        return sak::VolumePathQuery::Complete;
    }
    // ERROR_MORE_DATA is the only recoverable failure: the API reports how many characters it
    // needs, so grow to that size and ask again instead of dropping the volume's mount paths.
    // A required size that would not grow the buffer is treated as a failure -- retrying with
    // the same buffer would spin forever.
    if (query_error == ERROR_MORE_DATA && required_chars > buffer_chars) {
        return sak::VolumePathQuery::Retry;
    }
    return sak::VolumePathQuery::Failed;
}

bool DriveScanner::collectMountPaths(wchar_t* volumeName,
                                     size_t nameLen,
                                     QStringList& mountPoints) {
    volumeName[nameLen - 1] = L'\\';

    // Grow the buffer to the size the API asks for (ERROR_MORE_DATA) rather than dropping this
    // volume's mount paths; any other failure is surfaced so the caller never presents an
    // unreadable volume as unmounted.
    std::vector<wchar_t> pathNames(MAX_PATH * kVolumePathBufferMultiplier, L'\0');
    bool query_ok = false;
    for (int attempt = 0; attempt < kVolumePathQueryAttempts; ++attempt) {
        DWORD required = 0;
        const bool call_ok = GetVolumePathNamesForVolumeNameW(volumeName,
                                                              pathNames.data(),
                                                              static_cast<DWORD>(pathNames.size()),
                                                              &required) != 0;
        const auto outcome =
            volumePathQueryOutcome(call_ok, GetLastError(), required, pathNames.size());
        if (outcome != sak::VolumePathQuery::Retry) {
            query_ok = outcome == sak::VolumePathQuery::Complete;
            break;
        }
        pathNames.assign(required, L'\0');
    }
    volumeName[nameLen - 1] = L'\0';
    if (!query_ok) {
        return false;
    }

    const wchar_t* ptr = pathNames.data();
    while (*ptr) {
        mountPoints.append(QString::fromWCharArray(ptr));
        ptr += wcslen(ptr) + 1;
    }
    return true;
}

QString DriveScanner::getVolumeLabel(const QString& mountPoint) {
    // queryDriveInfo calls this only with mountPoints.first(), and collectMountPaths appends
    // only non-empty NUL-terminated mount paths.
    Q_ASSERT_X(!mountPoint.isEmpty(), "getVolumeLabel", "mountPoint must not be empty");
    wchar_t volumeLabel[MAX_PATH + 1] = {};

    if (GetVolumeInformationW(reinterpret_cast<LPCWSTR>(mountPoint.utf16()),
                              volumeLabel,
                              MAX_PATH,
                              nullptr,
                              nullptr,
                              nullptr,
                              nullptr,
                              0)) {
        return QString::fromWCharArray(volumeLabel);
    }

    return QString();
}

namespace {

/// @brief True if @p rel exists under volume root @p root. Uses GetFileAttributesW on a native
/// backslash path so it works for BOTH drive-letter roots (E:\) and unmounted volumes addressed by
/// their \\?\Volume{GUID}\ path (QDir mangles the \\?\ prefix, so it cannot be used here).
bool nativePathExists(const QString& root, const char* rel) {
    QString full = root;
    if (!full.endsWith(QLatin1Char('\\'))) {
        full += QLatin1Char('\\');
    }
    full += QString::fromLatin1(rel);
    full.replace(QLatin1Char('/'), QLatin1Char('\\'));
    const DWORD attrs = GetFileAttributesW(reinterpret_cast<LPCWSTR>(full.utf16()));
    return attrs != INVALID_FILE_ATTRIBUTES;
}

}  // namespace

bool DriveScanner::hasWindowsIndicators(const QString& root) {
    if (nativePathExists(root, "Windows/System32") &&
        nativePathExists(root, "Windows/System32/ntoskrnl.exe")) {
        return true;
    }
    if (nativePathExists(root, "Windows/explorer.exe") && nativePathExists(root, "Program Files")) {
        return true;
    }
    // Boot files alone don't mean it's a system drive
    if ((nativePathExists(root, "bootmgr") || nativePathExists(root, "BOOTNXT")) &&
        nativePathExists(root, "Windows")) {
        return true;
    }
    if (nativePathExists(root, "EFI/Microsoft/Boot/bootmgfw.efi") &&
        nativePathExists(root, "Windows")) {
        return true;
    }
    return false;
}

// Windows boot-loader files. On a split-boot system the ESP that boots the OS
// lives on a SEPARATE disk from the \Windows tree, so these appear with no Windows
// dir; a disk carrying them is boot-critical and must not be erased.
bool DriveScanner::hasBootManagerIndicators(const QString& root) {
    return nativePathExists(root, "EFI/Microsoft/Boot/bootmgfw.efi") ||
           nativePathExists(root, "bootmgr") || nativePathExists(root, "BOOTNXT");
}

bool DriveScanner::containsWindowsInstallation(int driveNumber) {
    // queryDriveInfo passes a number enumeratePhysicalDriveNumbers accepted (>= 0).
    Q_ASSERT_X(driveNumber >= 0, "containsWindowsInstallation", "driveNumber must be non-negative");
    // Inspect EVERY volume on the drive -- including unmounted ones (GUID path) -- so a Windows
    // partition without a drive letter is still classified as a system drive.
    bool enumerationOk = true;
    const QStringList roots = getVolumeRootsForDrive(driveNumber, &enumerationOk);
    if (!enumerationOk) {
        // Volume enumeration failed: we cannot prove this disk is NOT a system disk, so fail
        // closed and classify it as system rather than exposing an unknown state as "safe".
        return true;
    }

    if (std::any_of(roots.begin(), roots.end(), [](const QString& root) {
            return hasWindowsIndicators(root);
        })) {
        return true;
    }
    // A FIXED disk carrying the Windows boot loader (e.g. a split-boot ESP whose
    // \Windows lives on another disk) IS the boot disk -- protect it from erasure.
    // Removable media carrying boot files (a bootable USB the user wants to
    // re-flash) is NOT treated as system, so it stays a valid flash target.
    if (isDriveRemovable(driveNumber)) {
        return false;
    }
    return std::any_of(roots.begin(), roots.end(), [](const QString& root) {
        return hasBootManagerIndicators(root);
    });
}

sak::DiskProbe DriveScanner::physicalDriveBootProbe(int driveNumber) {
    if (driveNumber < 0) {
        return sak::DiskProbe::Undetermined;
    }
    bool enumerationOk = false;
    const QStringList roots = getVolumeRootsForDrive(driveNumber, &enumerationOk);
    if (!enumerationOk) {
        // Could not enumerate the drive's volumes -> cannot prove it is not boot-critical.
        return sak::DiskProbe::Undetermined;
    }
    // Removable media carrying boot files is a legitimate re-flash target, not a boot disk.
    if (isDriveRemovable(driveNumber)) {
        return sak::DiskProbe::No;
    }
    const bool boot = std::any_of(roots.begin(), roots.end(), [](const QString& root) {
        return hasBootManagerIndicators(root);
    });
    return boot ? sak::DiskProbe::Yes : sak::DiskProbe::No;
}

LRESULT CALLBACK DriveScanner::deviceChangeWndProc(HWND hwnd,
                                                   UINT msg,
                                                   WPARAM wParam,
                                                   LPARAM lParam) {
    if (msg != WM_DEVICECHANGE) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (wParam != DBT_DEVICEARRIVAL && wParam != DBT_DEVICEREMOVECOMPLETE) {
        return TRUE;
    }
    const auto* pHdr = reinterpret_cast<const DEV_BROADCAST_HDR*>(lParam);
    if (pHdr && pHdr->dbch_devicetype == DBT_DEVTYP_VOLUME && DriveScanner::s_instance) {
        QMetaObject::invokeMethod(DriveScanner::s_instance, "scanDrives", Qt::QueuedConnection);
    }
    return TRUE;
}

void DriveScanner::registerDeviceNotification() {
    const wchar_t* className = L"DriveScannerWindowClass";

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = deviceChangeWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        sak::logError("Failed to register window class");
        return;
    }

    m_notificationWindow = CreateWindowExW(0,
                                           className,
                                           L"DriveScanner",
                                           0,
                                           0,
                                           0,
                                           0,
                                           0,
                                           HWND_MESSAGE,
                                           nullptr,
                                           GetModuleHandleW(nullptr),
                                           nullptr);

    if (!m_notificationWindow) {
        sak::logError("Failed to create notification window");
        return;
    }

    DEV_BROADCAST_DEVICEINTERFACE_W notificationFilter = {};
    notificationFilter.dbcc_size = sizeof(notificationFilter);
    notificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;

    m_deviceNotify = RegisterDeviceNotificationW(m_notificationWindow,
                                                 &notificationFilter,
                                                 DEVICE_NOTIFY_WINDOW_HANDLE |
                                                     DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);

    if (!m_deviceNotify) {
        sak::logError("Failed to register device notification");
        DestroyWindow(m_notificationWindow);
        m_notificationWindow = nullptr;
    } else {
        sak::logDebug("DriveScanner", "Device notification registered successfully");
    }
}

void DriveScanner::unregisterDeviceNotification() {
    if (m_deviceNotify != nullptr) {
        UnregisterDeviceNotification(m_deviceNotify);
        m_deviceNotify = nullptr;
    }

    if (m_notificationWindow != nullptr) {
        DestroyWindow(m_notificationWindow);
        m_notificationWindow = nullptr;
    }
}

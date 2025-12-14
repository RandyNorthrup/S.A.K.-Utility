// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/drive_scanner.h"
#include "sak/logger.h"
#include <QDir>
#include <dbt.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <winioctl.h>
#include <ntddscsi.h>

#pragma comment(lib, "setupapi.lib")

DriveScanner* DriveScanner::s_instance = nullptr;

DriveScanner::DriveScanner(QObject* parent)
    : QObject(parent)
    , m_refreshTimer(new QTimer(this))
    , m_notificationWindow(nullptr)
    , m_deviceNotify(nullptr)
    , m_isScanning(false)
{
    s_instance = this;
    
    // Refresh every 5 seconds as fallback
    m_refreshTimer->setInterval(5000);
    connect(m_refreshTimer, &QTimer::timeout, this, &DriveScanner::onRefreshTimer);
}

DriveScanner::~DriveScanner() {
    stop();
    s_instance = nullptr;
}

void DriveScanner::start() {
    sak::log_info("Starting drive scanner");
    
    // Initial scan
    scanDrives();
    
    // Register for device notifications
    registerDeviceNotification();
    
    // Start refresh timer
    m_refreshTimer->start();
}

void DriveScanner::stop() {
    sak::log_info("Stopping drive scanner");
    
    m_refreshTimer->stop();
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
    for (const auto& drive : m_drives) {
        if (drive.devicePath == devicePath) {
            return drive;
        }
    }
    return sak::DriveInfo{}; // Return invalid info
}

bool DriveScanner::isSystemDrive(const QString& devicePath) const {
    sak::DriveInfo info = getDriveInfo(devicePath);
    return info.isValid() && info.isSystem;
}

void DriveScanner::onRefreshTimer() {
    scanDrives();
}

void DriveScanner::scanDrives() {
    if (m_isScanning) {
        return;
    }
    
    m_isScanning = true;
    QList<sak::DriveInfo> newDrives;
    
    // Enumerate physical drives (0-99 should be more than enough)
    for (int driveNumber = 0; driveNumber < 100; ++driveNumber) {
        sak::DriveInfo info = queryDriveInfo(driveNumber);
        if (info.isValid()) {
            newDrives.append(info);
        }
    }
    
    // Check for changes
    bool hasChanges = false;
    
    // Find removed drives
    for (const auto& oldDrive : m_drives) {
        bool found = false;
        for (const auto& newDrive : newDrives) {
            if (newDrive.devicePath == oldDrive.devicePath) {
                found = true;
                break;
            }
        }
        if (!found) {
            sak::log_info(QString("Drive detached: %1").arg(oldDrive.devicePath).toStdString());
            Q_EMIT driveDetached(oldDrive.devicePath);
            hasChanges = true;
        }
    }
    
    // Find new drives
    for (const auto& newDrive : newDrives) {
        bool found = false;
        for (const auto& oldDrive : m_drives) {
            if (newDrive.devicePath == oldDrive.devicePath) {
                found = true;
                break;
            }
        }
        if (!found) {
            sak::log_info(QString("Drive attached: %1 (%2)").arg(newDrive.devicePath, newDrive.name).toStdString());
            Q_EMIT driveAttached(newDrive);
            hasChanges = true;
        }
    }
    
    m_drives = newDrives;
    
    if (hasChanges) {
        Q_EMIT drivesUpdated(m_drives);
    }
    
    m_isScanning = false;
}

sak::DriveInfo DriveScanner::queryDriveInfo(int driveNumber) {
    sak::DriveInfo info;
    info.devicePath = QString("\\\\.\\PhysicalDrive%1").arg(driveNumber);
    
    // Try to open drive
    HANDLE hDrive = CreateFileW(
        reinterpret_cast<LPCWSTR>(info.devicePath.utf16()),
        0, // No access needed for query
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );
    
    if (hDrive == INVALID_HANDLE_VALUE) {
        return sak::DriveInfo{}; // Drive doesn't exist or can't be accessed
    }
    
    // Get drive geometry and size
    info.size = getDriveSize(hDrive);
    info.blockSize = getBlockSize(hDrive);
    info.name = getDriveName(driveNumber);
    info.busType = getBusType(hDrive);
    info.isRemovable = isDriveRemovable(driveNumber);
    info.isReadOnly = isDriveReadOnly(hDrive);
    info.isSystem = containsWindowsInstallation(driveNumber);
    info.mountPoints = getMountPoints(driveNumber);
    
    if (!info.mountPoints.isEmpty()) {
        info.volumeLabel = getVolumeLabel(info.mountPoints.first());
    }
    
    CloseHandle(hDrive);
    
    return info;
}

QString DriveScanner::getDriveName(int driveNumber) {
    QString devicePath = QString("\\\\.\\PhysicalDrive%1").arg(driveNumber);
    HANDLE hDrive = CreateFileW(
        reinterpret_cast<LPCWSTR>(devicePath.utf16()),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );
    
    if (hDrive == INVALID_HANDLE_VALUE) {
        return QString("Physical Drive %1").arg(driveNumber);
    }
    
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    
    BYTE buffer[1024] = {};
    DWORD bytesReturned = 0;
    
    if (DeviceIoControl(
        hDrive,
        IOCTL_STORAGE_QUERY_PROPERTY,
        &query, sizeof(query),
        buffer, sizeof(buffer),
        &bytesReturned,
        nullptr))
    {
        auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buffer);
        
        QString vendor, product;
        if (desc->VendorIdOffset != 0) {
            vendor = QString::fromLatin1(reinterpret_cast<const char*>(buffer + desc->VendorIdOffset)).trimmed();
        }
        if (desc->ProductIdOffset != 0) {
            product = QString::fromLatin1(reinterpret_cast<const char*>(buffer + desc->ProductIdOffset)).trimmed();
        }
        
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
    DISK_GEOMETRY_EX geometry = {};
    DWORD bytesReturned = 0;
    
    if (DeviceIoControl(
        hDrive,
        IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
        nullptr, 0,
        &geometry, sizeof(geometry),
        &bytesReturned,
        nullptr))
    {
        return geometry.DiskSize.QuadPart;
    }
    
    return 0;
}

quint32 DriveScanner::getBlockSize(HANDLE hDrive) {
    DISK_GEOMETRY geometry = {};
    DWORD bytesReturned = 0;
    
    if (DeviceIoControl(
        hDrive,
        IOCTL_DISK_GET_DRIVE_GEOMETRY,
        nullptr, 0,
        &geometry, sizeof(geometry),
        &bytesReturned,
        nullptr))
    {
        return geometry.BytesPerSector;
    }
    
    return 512; // Default
}

QString DriveScanner::getBusType(HANDLE hDrive) {
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    
    BYTE buffer[1024] = {};
    DWORD bytesReturned = 0;
    
    if (DeviceIoControl(
        hDrive,
        IOCTL_STORAGE_QUERY_PROPERTY,
        &query, sizeof(query),
        buffer, sizeof(buffer),
        &bytesReturned,
        nullptr))
    {
        auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buffer);
        
        switch (desc->BusType) {
            case BusTypeUsb: return "USB";
            case BusTypeAta: return "ATA";
            case BusTypeSata: return "SATA";
            case BusTypeNvme: return "NVMe";
            case BusTypeSd: return "SD";
            case BusTypeMmc: return "MMC";
            default: return "Unknown";
        }
    }
    
    return "Unknown";
}

bool DriveScanner::isDriveRemovable(int driveNumber) {
    // Query using WMI or DeviceIoControl
    // For now, simple heuristic: USB and SD are removable
    QString devicePath = QString("\\\\.\\PhysicalDrive%1").arg(driveNumber);
    HANDLE hDrive = CreateFileW(
        reinterpret_cast<LPCWSTR>(devicePath.utf16()),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );
    
    if (hDrive == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    QString busType = getBusType(hDrive);
    CloseHandle(hDrive);
    
    return busType == "USB" || busType == "SD" || busType == "MMC";
}

bool DriveScanner::isDriveReadOnly(HANDLE hDrive) {
    DISK_GEOMETRY geometry = {};
    DWORD bytesReturned = 0;
    
    if (DeviceIoControl(
        hDrive,
        IOCTL_DISK_IS_WRITABLE,
        nullptr, 0,
        nullptr, 0,
        &bytesReturned,
        nullptr))
    {
        return false;
    }
    
    DWORD error = GetLastError();
    if (error == ERROR_WRITE_PROTECT) {
        return true;
    }
    
    GET_LENGTH_INFORMATION lengthInfo = {};
    if (DeviceIoControl(
        hDrive,
        IOCTL_DISK_GET_LENGTH_INFO,
        nullptr, 0,
        &lengthInfo, sizeof(lengthInfo),
        &bytesReturned,
        nullptr))
    {
        if (lengthInfo.Length.QuadPart == 0) {
            return true;
        }
    }
    
    return false;
}

QStringList DriveScanner::getMountPoints(int driveNumber) {
    QStringList mountPoints;
    
    wchar_t volumeName[MAX_PATH];
    HANDLE hFind = FindFirstVolumeW(volumeName, MAX_PATH);
    
    if (hFind == INVALID_HANDLE_VALUE) {
        return mountPoints;
    }
    
    do {
        size_t len = wcslen(volumeName);
        if (len > 0 && volumeName[len - 1] == L'\\') {
            volumeName[len - 1] = L'\0';
        }
        
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
                wchar_t pathNames[MAX_PATH * 4];
                DWORD pathLen = 0;
                
                volumeName[len - 1] = L'\\';
                if (GetVolumePathNamesForVolumeNameW(volumeName, pathNames, sizeof(pathNames) / sizeof(wchar_t), &pathLen)) {
                    wchar_t* ptr = pathNames;
                    while (*ptr) {
                        mountPoints.append(QString::fromWCharArray(ptr));
                        ptr += wcslen(ptr) + 1;
                    }
                }
                volumeName[len - 1] = L'\0';
            }
            
            CloseHandle(hVolume);
        }
    } while (FindNextVolumeW(hFind, volumeName, MAX_PATH));
    
    FindVolumeClose(hFind);
    return mountPoints;
}

QString DriveScanner::getVolumeLabel(const QString& mountPoint) {
    wchar_t volumeLabel[MAX_PATH + 1] = {};
    
    if (GetVolumeInformationW(
        reinterpret_cast<LPCWSTR>(mountPoint.utf16()),
        volumeLabel, MAX_PATH,
        nullptr, nullptr, nullptr,
        nullptr, 0))
    {
        return QString::fromWCharArray(volumeLabel);
    }
    
    return QString();
}

bool DriveScanner::containsWindowsInstallation(int driveNumber) {
    QStringList mountPoints = getMountPoints(driveNumber);
    
    for (const QString& mountPoint : mountPoints) {
        QDir mountDir(mountPoint);
        
        // Check for Windows system directory
        if (mountDir.exists("Windows/System32") && mountDir.exists("Windows/System32/ntoskrnl.exe")) {
            return true;
        }
        
        // Check for Windows installation with explorer
        if (mountDir.exists("Windows/explorer.exe") && mountDir.exists("Program Files")) {
            return true;
        }
        
        // Check for boot files (but only if other Windows indicators present)
        // Boot files alone don't mean it's a system drive - recovery/install media has these too
        if ((mountDir.exists("bootmgr") || mountDir.exists("BOOTNXT")) && 
            mountDir.exists("Windows")) {
            return true;
        }
        
        // Check for EFI boot files with Windows
        if (mountDir.exists("EFI/Microsoft/Boot/bootmgfw.efi") && 
            mountDir.exists("Windows")) {
            return true;
        }
    }
    
    return false;
}

void DriveScanner::registerDeviceNotification() {
    const wchar_t* className = L"DriveScannerWindowClass";
    
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        if (msg == WM_DEVICECHANGE) {
            if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) {
                auto* pHdr = reinterpret_cast<DEV_BROADCAST_HDR*>(lParam);
                if (pHdr && pHdr->dbch_devicetype == DBT_DEVTYP_VOLUME) {
                    if (DriveScanner::s_instance) {
                        QMetaObject::invokeMethod(DriveScanner::s_instance, "scanDrives", Qt::QueuedConnection);
                    }
                }
            }
            return TRUE;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    };
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        sak::log_error("Failed to register window class");
        return;
    }
    
    m_notificationWindow = CreateWindowExW(
        0,
        className,
        L"DriveScanner",
        0, 0, 0, 0, 0,
        HWND_MESSAGE,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr
    );
    
    if (!m_notificationWindow) {
        sak::log_error("Failed to create notification window");
        return;
    }
    
    DEV_BROADCAST_DEVICEINTERFACE_W notificationFilter = {};
    notificationFilter.dbcc_size = sizeof(notificationFilter);
    notificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    
    m_deviceNotify = RegisterDeviceNotificationW(
        m_notificationWindow,
        &notificationFilter,
        DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES
    );
    
    if (!m_deviceNotify) {
        sak::log_error("Failed to register device notification");
        DestroyWindow(m_notificationWindow);
        m_notificationWindow = nullptr;
    } else {
        sak::log_debug("DriveScanner", "Device notification registered successfully");
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

LRESULT CALLBACK DriveScanner::deviceNotificationProc(HWND hwnd, UINT message, 
                                                     WPARAM wParam, LPARAM lParam)
{
    if (message == WM_DEVICECHANGE) {
        if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) {
            if (s_instance) {
                s_instance->refresh();
            }
        }
    }
    
    return DefWindowProc(hwnd, message, wParam, lParam);
}







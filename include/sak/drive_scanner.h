// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QFutureWatcher>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <memory>

#include <windows.h>

namespace sak {

/**
 * @brief Information about a physical drive
 */
struct DriveInfo {
    QString devicePath;       // e.g., "\\.\PhysicalDrive1"
    QString name;             // e.g., "Generic USB Flash Disk"
    QString description;      // Additional info
    qint64 size{0};           // Size in bytes
    quint32 blockSize{0};     // Block size in bytes (usually 512 or 4096)
    bool isSystem{false};     // True if contains Windows installation
    bool isRemovable{false};  // True if removable media
    bool isReadOnly{false};   // True if write-protected
    QString busType;          // USB, SATA, NVMe, SD, etc.

    // Volume information (if mounted)
    QStringList mountPoints;  // e.g., ["E:\\", "F:\\"]
    QString volumeLabel;      // Volume label if any

    bool isValid() const { return !devicePath.isEmpty() && size > 0; }
};

/**
 * @brief Result of one physical-drive enumeration pass.
 *
 * Carries the authoritative-ness of the scan alongside the drives so a partial/failed
 * enumeration (enumeration_ok == false) is never mistaken for "these are all the drives"
 * and does not spuriously detach the cached ones.
 */
struct DriveScanResult {
    QList<sak::DriveInfo> drives;
    bool enumeration_ok{false};
};

/**
 * @brief Tri-state result of a fail-closed physical-disk probe.
 *
 * Undetermined MUST be treated as unsafe by safety-critical callers (fail closed):
 * an inability to inspect the disk is never reported as "safe".
 */
enum class DiskProbe {
    No,
    Yes,
    Undetermined
};

}  // namespace sak

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
 * Thread-Safety: NOT thread-safe. Instance state (m_drives, the timer, the
 * notification window/handle, and the s_instance singleton) is mutated only on
 * the owning (GUI) thread -- construct, start/stop, and query from that thread.
 * The one exception is the static enumerateDrivesOnce(), which touches no instance
 * state and is safe to call from a worker thread. Signals are emitted on the
 * owning thread.
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
class DriveScannerTests;  // grants the unit test access to the pure decision seams below

class DriveScanner : public QObject {
    Q_OBJECT

    friend class ::DriveScannerTests;

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

    /**
     * @brief Enumerate all physical drives once, synchronously, with no monitor.
     *
     * A self-contained one-shot enumeration for headless/read-only callers (e.g. the
     * AI assistant's imaging.list_drives). Constructs NO DriveScanner instance, so it
     * never touches the s_instance singleton, the hot-plug notification window, or the
     * refresh timer -- it is safe to call while a GUI DriveScanner is live and from a
     * worker thread. Shares the exact per-drive query path used by the GUI scan.
     * @param enumeration_ok Set true when the OS physical-drive device list was read
     *        successfully; false when that query failed and the result is a best-effort
     *        probe of drive 0 only (so the list may be incomplete). Lets a caller report
     *        an empty/partial result honestly instead of as a definitive "no drives".
     * @return All valid physical drives detected.
     */
    [[nodiscard]] static QList<sak::DriveInfo> enumerateDrivesOnce(bool& enumeration_ok);

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

private Q_SLOTS:
    void onRefreshTimer();
    void onScanFinished();

private:
    void scanDrives();
    /// @brief Diff a fresh scan against the cached list and emit changes (UI thread).
    /// @param enumeration_ok False when the scan was not authoritative (partial/probe-only); a
    ///        non-authoritative scan never detaches cached drives -- see drivesDetached().
    void applyDriveScan(const QList<sak::DriveInfo>& newDrives, bool enumeration_ok);
    void registerDeviceNotification();
    void unregisterDeviceNotification();

    // --- Pure decision seams (no Win32/instance state; unit-tested offscreen) ---
    /// @brief Cached drives that are genuinely gone (in current, absent from scanned). Returns
    ///        EMPTY when !enumeration_ok, so a partial scan is never read as "these detached".
    static QList<sak::DriveInfo> drivesDetached(const QList<sak::DriveInfo>& current,
                                                const QList<sak::DriveInfo>& scanned,
                                                bool enumeration_ok);
    /// @brief The drive list to cache after a scan. Authoritative -> the scanned list;
    ///        non-authoritative -> keep every current drive and union in any newly-seen ones.
    static QList<sak::DriveInfo> mergeDriveList(const QList<sak::DriveInfo>& current,
                                                const QList<sak::DriveInfo>& scanned,
                                                bool enumeration_ok);
    /// @brief OOB-safe read of a NUL-terminated field from a STORAGE_DEVICE_DESCRIPTOR buffer.
    ///        Empty when offset is 0 (absent) or outside [0, bytes_returned); never reads past
    ///        the returned data even if the field is not NUL-terminated.
    static QString descriptorString(const BYTE* buffer,
                                    DWORD buffer_size,
                                    DWORD bytes_returned,
                                    DWORD field_offset);
    /// @brief Fail-closed writability decision. True (read-only) unless the drive AFFIRMATIVELY
    ///        reports writable; an undeterminable probe is never announced as writable.
    static bool driveReadOnlyFromProbe(bool is_writable_ioctl_ok,
                                       DWORD is_writable_error,
                                       bool length_ok,
                                       qint64 length_bytes);
    /// @brief True if the given volume root (drive letter OR \\?\Volume{GUID}\ path) holds a
    ///        Windows installation. Uses GetFileAttributesW so it works for unmounted volumes.
    static bool hasWindowsIndicators(const QString& root);
    /// @brief True if the given volume root carries Windows boot-loader files
    ///        (bootmgfw.efi / bootmgr / BOOTNXT). On split-boot hardware the ESP that
    ///        boots the OS can live on a SEPARATE physical disk from \Windows, so this
    ///        catches a boot/system disk the \Windows-volume check alone would miss.
    static bool hasBootManagerIndicators(const QString& root);

public:
    /// @brief Fail-closed engine guard: is physical drive @p driveNumber boot-critical?
    ///        DiskProbe::Yes when a NON-removable volume on it carries boot-loader files;
    ///        No when it provably does not (or is removable -- a bootable USB re-flash
    ///        target); Undetermined when volume enumeration failed. Independent of the
    ///        \Windows disk, so it also protects a split-boot ESP on another disk. Public so
    ///        FlashCoordinator's engine-level OS-disk guard can consult it.
    [[nodiscard]] static sak::DiskProbe physicalDriveBootProbe(int driveNumber);

private:
    // These per-drive queries are pure (no instance state); they are static so the
    // headless enumerateDrivesOnce() path can share them without constructing a scanner.
    static sak::DriveInfo queryDriveInfo(int driveNumber);
    static QString getDriveName(int driveNumber);
    static qint64 getDriveSize(HANDLE hDrive);
    static quint32 getBlockSize(HANDLE hDrive);
    static QString getBusType(HANDLE hDrive);
    static bool isDriveRemovable(int driveNumber);
    static bool isDriveReadOnly(HANDLE hDrive);
    static QStringList getMountPoints(int driveNumber);
    static QString getVolumeLabel(const QString& mountPoint);
    static bool containsWindowsInstallation(int driveNumber);
    static void collectMountPaths(wchar_t* volumeName, size_t nameLen, QStringList& mountPoints);
    /// @brief True if the volume (opened by its \\?\Volume{GUID} path, no trailing backslash)
    ///        lives on physical drive @p driveNumber.
    static bool volumeMatchesDrive(const wchar_t* volumeName, int driveNumber);
    /// @brief Inspectable roots for every volume on @p driveNumber: the mount path(s) when the
    ///        volume is mounted, otherwise the \\?\Volume{GUID}\ path so an UNMOUNTED Windows
    ///        partition is still inspected (closes the "system drive missed" hazard).
    /// @param enumerationOk Optional out-param set to false when the volume enumeration itself
    ///        failed (FindFirstVolume/FindNextVolume error); lets a fail-closed caller refuse
    ///        rather than treat an empty result as "no system/boot volumes".
    static QStringList getVolumeRootsForDrive(int driveNumber, bool* enumerationOk = nullptr);
    /// @brief Append the inspectable root(s) of one enumerated volume to @p roots when it
    ///        lives on @p driveNumber (mount path(s) if mounted, else its GUID path).
    static void appendVolumeRoot(wchar_t* volumeName, int driveNumber, QStringList& roots);

    /// @brief True when two DriveInfo records for the same device differ in a user-visible
    ///        property (size, block size, name, read-only, system, removable, mount points,
    ///        volume label), so an in-place property change still emits drivesUpdated.
    static bool driveInfoChanged(const sak::DriveInfo& a, const sak::DriveInfo& b);

    static LRESULT CALLBACK deviceChangeWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    QList<sak::DriveInfo> m_drives;
    QTimer* m_refreshTimer;
    HWND m_notificationWindow;
    HDEVNOTIFY m_deviceNotify;
    std::atomic<bool> m_isScanning;
    QFutureWatcher<sak::DriveScanResult> m_scanWatcher;

    static DriveScanner* s_instance;  // For static callback
};

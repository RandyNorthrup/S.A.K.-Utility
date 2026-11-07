// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file hardware_inventory_scanner.h
/// @brief System hardware enumeration via WMI and native Windows APIs

#pragma once

#include "sak/diagnostic_types.h"
#include "sak/layout_constants.h"

#include <QDateTime>
#include <QHash>
#include <QJsonDocument>
#include <QObject>
#include <QStorageInfo>
#include <QVariant>
#include <QVariantMap>

#include <atomic>

namespace sak {

inline constexpr int kDefaultWmiQueryTimeoutMs = kTimeoutProcessLongMs;

/**
 * @brief Enumerates all system hardware components
 *
 * Uses WMI (via PowerShell Get-CimInstance) and native Windows APIs
 * (DXGI, GetSystemPowerStatus) to build a complete hardware inventory.
 *
 * Thread-Safety: Designed to run on a worker thread. All results are
 * delivered via queued signal connections.
 */
class HardwareInventoryScanner : public QObject {
    Q_OBJECT

public:
    explicit HardwareInventoryScanner(QObject* parent = nullptr);
    ~HardwareInventoryScanner() override = default;

    // Disable copy and move
    HardwareInventoryScanner(const HardwareInventoryScanner&) = delete;
    HardwareInventoryScanner& operator=(const HardwareInventoryScanner&) = delete;
    HardwareInventoryScanner(HardwareInventoryScanner&&) = delete;
    HardwareInventoryScanner& operator=(HardwareInventoryScanner&&) = delete;

    /// @brief Run full hardware scan (all components)
    void scan();

    /// @brief Scan CPU only
    void scanCpu();

    /// @brief Scan memory only
    void scanMemory();

    /// @brief Scan storage devices only
    void scanStorage();

    /// @brief Scan GPU(s) only
    void scanGpu();

    /// @brief Scan battery only
    void scanBattery();

    /// @brief Cancel a running scan
    void cancel();

    /// @brief Parse the volume-to-disk JSON emitted by the internal WMI script
    /// @param json Compact JSON: array of {Letter, DiskIndex} (or a lone object)
    /// @return Hash of upper-case "X:" drive letter to disk index
    /// @note Public + static for unit testing; null/non-numeric DiskIndex dropped
    [[nodiscard]] static QHash<QString, uint32_t> parseVolumeDiskMap(const QByteArray& json);

    /// @brief Whether a finished WMI query should count as a reportable failure.
    /// @param timedOut the query process timed out.
    /// @param cancelled the query was cancelled by the user.
    /// @param exitCode the process exit code (0 == success).
    /// @param jsonParseFailed the (non-empty) output failed to parse as JSON.
    /// @return true for a genuine failure; false for success or user cancel.
    /// @note Pure + static for unit testing. A legitimately empty-but-successful
    ///       result is NOT a failure; a user cancellation is NOT a failure. Lets
    ///       scan() surface "inventory may be incomplete" instead of silently
    ///       reporting success over drives it never actually read (fail-open).
    [[nodiscard]] static bool wmiQueryIsReportableFailure(bool timedOut,
                                                          bool cancelled,
                                                          int exitCode,
                                                          bool jsonParseFailed);

    /// @brief Convert a parsed WMI JSON document into per-instance property maps.
    /// @param doc A JSON array (multiple instances) or object (single instance).
    /// @return One QVariantMap per JSON object; non-object array entries skipped.
    /// @note Pure + static for unit testing.
    [[nodiscard]] static QVector<QVariantMap> jsonDocToVariantMaps(const QJsonDocument& doc);

    /// @brief Seconds since @p boot_time, as Win32_OperatingSystem.LastBootUpTime arrives: CIM
    ///        ("yyyyMMddHHmmss.ffffff+UUU"), or Windows PowerShell's JSON form "/Date(ms)/".
    ///        0 (unknown) when it is neither. Pure + static for unit testing.
    [[nodiscard]] static uint64_t uptimeSecondsSinceBoot(const QString& boot_time,
                                                         const QDateTime& now);

    /// @brief The bus a disk is on, from MSFT_PhysicalDisk.BusType (the STORAGE_BUS_TYPE values):
    ///        "NVMe", "SATA", "USB", "SAS", "RAID", "SCSI" and the rest; empty for 0 or a value
    ///        Windows does not define. Pure + static for unit testing.
    [[nodiscard]] static QString storageBusName(int bus_type);

    /// @brief A disk's interface as the views show it: @p bus_name when Windows' storage stack gave
    ///        one, else Win32_DiskDrive's own InterfaceType as it is. Win32_DiskDrive reports an
    ///        NVMe disk as "SCSI", and a USB enclosure's model can contain "NVMe": read as "SATA"
    ///        and "NVMe", testpc1's internal NVMe and its USB enclosure were typed the wrong way
    ///        round (review 3). Pure + static for unit testing.
    [[nodiscard]] static QString storageInterfaceName(const QString& bus_name,
                                                      const QString& wmi_interface);

    /// @brief Every physical disk Windows lists, with its number, model, serial, size, bus and
    /// media
    ///        type, without the volumes on it. The SMART run reads its drives' names from this when
    ///        no hardware scan has run. Runs PowerShell queries; call it off the GUI thread.
    [[nodiscard]] QVector<StorageDeviceInfo> queryPhysicalDisks();

Q_SIGNALS:
    void scanStarted();
    void scanProgress(int percent, const QString& component);
    void scanComplete(sak::HardwareInventory inventory);
    void errorOccurred(const QString& error);

private:
    std::atomic<bool> m_cancelled{false};
    HardwareInventory m_inventory;

    /// @brief Set true when any WMI query in the current scan genuinely failed
    ///        (timeout, non-zero exit, or malformed JSON) -- distinct from a
    ///        legitimately empty result. Reset at the start of every scan entry;
    ///        only touched from the scan thread.
    bool m_wmiQueryFailed{false};

    /// @brief Emit the terminal signals for a scan: an errorOccurred (only when a
    ///        WMI query failed) followed by scanComplete with whatever was read.
    void finishScan();

    /// @brief Query CPU via WMI Win32_Processor
    CpuInfo queryCpu();

    /// @brief Query memory via WMI Win32_PhysicalMemory + GlobalMemoryStatusEx
    MemorySummary queryMemory();

    /// @brief Parse a single WMI memory module result into MemoryModuleInfo
    static MemoryModuleInfo parseMemoryModule(const QVariantMap& mod, uint32_t slotIndex);

    /// @brief Query storage via WMI Win32_DiskDrive + volumes
    QVector<StorageDeviceInfo> queryStorage();

    /// @brief Enrich storage devices with partition/volume information
    void enrichStorageWithVolumeInfo(QVector<StorageDeviceInfo>& devices);

    /// @brief Map each mounted drive letter ("C:") to its physical disk number
    /// @return Hash of upper-case "X:" drive letter to Win32_DiskDrive index
    /// @note Chains Win32_LogicalDiskToPartition -> Win32_DiskPartition.DiskIndex
    ///       via PowerShell/WMI. Empty on query failure (partitions stay unassigned).
    QHash<QString, uint32_t> queryVolumeDiskMap();

    /// @brief Enrich a single storage device with volume info from mounted volumes
    /// @param volumeDiskMap Drive-letter -> disk-number map from queryVolumeDiskMap
    void enrichDeviceWithVolumes(StorageDeviceInfo& dev,
                                 const QList<QStorageInfo>& volumes,
                                 const QHash<QString, uint32_t>& volumeDiskMap);

    /// @brief Enumerate GPU adapters using DXGI (Windows only)
    void enumerateDxgiAdapters(QVector<GpuInfo>& gpus);

    /// @brief Query GPU via DXGI adapter enumeration + WMI fallback
    QVector<GpuInfo> queryGpu();

    /// @brief Query motherboard via WMI Win32_BaseBoard + Win32_BIOS
    MotherboardInfo queryMotherboard();

    /// @brief Query battery via WMI Win32_Battery + GetSystemPowerStatus
    BatteryInfo queryBattery();

    /// @return false if no battery present (caller should return early)
    bool queryBatteryPowerStatus(BatteryInfo& info);

    /// @brief Query OS info via WMI Win32_OperatingSystem
    void queryOsInfo();

    // -- WMI helpers ----------------------------------------------

    /// @brief Execute a PowerShell WMI query and return parsed JSON results
    /// @param wmi_class WMI class name (e.g., "Win32_Processor")
    /// @param properties List of properties to select
    /// @param timeout_ms Maximum wait time in milliseconds
    /// @return Vector of property maps, one per WMI instance
    QVector<QVariantMap> wmiQuery(const QString& wmi_class,
                                  const QStringList& properties,
                                  int timeout_ms = kDefaultWmiQueryTimeoutMs,
                                  const QString& cim_namespace = {});
};

}  // namespace sak

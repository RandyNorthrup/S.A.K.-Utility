// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file hardware_inventory_scanner.h
/// @brief System hardware enumeration via WMI and native Windows APIs

#pragma once

#include "sak/diagnostic_types.h"

#include <QObject>
#include <QStorageInfo>
#include <QVariant>
#include <QVariantMap>

#include <atomic>

namespace sak {

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

Q_SIGNALS:
    void scanStarted();
    void scanProgress(int percent, const QString& component);
    void scanComplete(sak::HardwareInventory inventory);
    void errorOccurred(const QString& error);

private:
    std::atomic<bool> m_cancelled{false};
    HardwareInventory m_inventory;

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

    /// @brief Enrich a single storage device with volume info from mounted volumes
    void enrichDeviceWithVolumes(StorageDeviceInfo& dev, const QList<QStorageInfo>& volumes);

    /// @brief Enumerate GPU adapters using DXGI (Windows only)
    void enumerateDxgiAdapters(QVector<GpuInfo>& gpus);

    /// @brief Query GPU via DXGI adapter enumeration + WMI fallback
    QVector<GpuInfo> queryGpu();

    /// @brief Query motherboard via WMI Win32_BaseBoard + Win32_BIOS
    MotherboardInfo queryMotherboard();

    /// @brief Query battery via WMI Win32_Battery + GetSystemPowerStatus
    BatteryInfo queryBattery();

    /// @brief Query OS info via WMI Win32_OperatingSystem
    void queryOsInfo();

    // ── WMI helpers ──────────────────────────────────────────────

    /// @brief Execute a PowerShell WMI query and return parsed JSON results
    /// @param wmi_class WMI class name (e.g., "Win32_Processor")
    /// @param properties List of properties to select
    /// @param timeout_ms Maximum wait time in milliseconds
    /// @return Vector of property maps, one per WMI instance
    QVector<QVariantMap> wmiQuery(const QString& wmi_class,
                                  const QStringList& properties,
                                  int timeout_ms = 30000);
};

} // namespace sak

// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file hardware_inventory_scanner.cpp
/// @brief System hardware enumeration implementation

#include "sak/hardware_inventory_scanner.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStorageInfo>

#ifdef SAK_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <dxgi.h>
#include <wrl/client.h>
#pragma comment(lib, "dxgi.lib")
#endif

namespace {

QString classifyInterfaceType(const QString& iface, const QString& model) {
    if (iface.contains("NVMe", Qt::CaseInsensitive) ||
        model.contains("NVMe", Qt::CaseInsensitive)) {
        return QStringLiteral("NVMe");
    }
    if (iface.contains("SCSI", Qt::CaseInsensitive) || iface.contains("IDE", Qt::CaseInsensitive)) {
        return QStringLiteral("SATA");
    }
    if (iface.contains("USB", Qt::CaseInsensitive)) {
        return QStringLiteral("USB");
    }
    return iface;
}

QString classifyMediaType(const QString& interface_type,
                          const QString& model,
                          const QString& media) {
    if (interface_type == "NVMe" || model.contains("SSD", Qt::CaseInsensitive) ||
        media.contains("SSD", Qt::CaseInsensitive) ||
        media.contains("Solid", Qt::CaseInsensitive)) {
        return QStringLiteral("SSD");
    }
    if (media.contains("Fixed", Qt::CaseInsensitive) ||
        media.contains("HDD", Qt::CaseInsensitive)) {
        return QStringLiteral("HDD");
    }
    return QStringLiteral("Unknown");
}

}  // namespace

namespace sak {

// ============================================================================
// Construction
// ============================================================================

HardwareInventoryScanner::HardwareInventoryScanner(QObject* parent) : QObject(parent) {}

// ============================================================================
// Public API
// ============================================================================

void HardwareInventoryScanner::scan() {
    m_cancelled.store(false, std::memory_order_relaxed);
    Q_EMIT scanStarted();

    m_inventory = HardwareInventory{};

    // CPU ---------------------------------------------------------
    if (m_cancelled.load(std::memory_order_relaxed)) {
        return;
    }
    Q_EMIT scanProgress(0, "Scanning CPU...");
    m_inventory.cpu = queryCpu();

    // Memory ------------------------------------------------------
    if (m_cancelled.load(std::memory_order_relaxed)) {
        return;
    }
    Q_EMIT scanProgress(15, "Scanning Memory...");
    m_inventory.memory = queryMemory();

    // Storage -----------------------------------------------------
    if (m_cancelled.load(std::memory_order_relaxed)) {
        return;
    }
    Q_EMIT scanProgress(30, "Scanning Storage...");
    m_inventory.storage = queryStorage();

    // GPU ---------------------------------------------------------
    if (m_cancelled.load(std::memory_order_relaxed)) {
        return;
    }
    Q_EMIT scanProgress(50, "Scanning GPU...");
    m_inventory.gpus = queryGpu();

    // Motherboard -------------------------------------------------
    if (m_cancelled.load(std::memory_order_relaxed)) {
        return;
    }
    Q_EMIT scanProgress(65, "Scanning Motherboard...");
    m_inventory.motherboard = queryMotherboard();

    // Battery -----------------------------------------------------
    if (m_cancelled.load(std::memory_order_relaxed)) {
        return;
    }
    Q_EMIT scanProgress(80, "Scanning Battery...");
    m_inventory.battery = queryBattery();

    // OS info -----------------------------------------------------
    if (m_cancelled.load(std::memory_order_relaxed)) {
        return;
    }
    Q_EMIT scanProgress(90, "Querying OS information...");
    queryOsInfo();

    m_inventory.scan_timestamp = QDateTime::currentDateTime();

    Q_EMIT scanProgress(100, "Scan complete");
    Q_EMIT scanComplete(m_inventory);
    logInfo("Hardware inventory scan completed successfully");
}

void HardwareInventoryScanner::scanCpu() {
    Q_EMIT scanStarted();
    m_inventory.cpu = queryCpu();
    m_inventory.scan_timestamp = QDateTime::currentDateTime();
    Q_EMIT scanComplete(m_inventory);
}

void HardwareInventoryScanner::scanMemory() {
    Q_EMIT scanStarted();
    m_inventory.memory = queryMemory();
    m_inventory.scan_timestamp = QDateTime::currentDateTime();
    Q_EMIT scanComplete(m_inventory);
}

void HardwareInventoryScanner::scanStorage() {
    Q_EMIT scanStarted();
    m_inventory.storage = queryStorage();
    m_inventory.scan_timestamp = QDateTime::currentDateTime();
    Q_EMIT scanComplete(m_inventory);
}

void HardwareInventoryScanner::scanGpu() {
    Q_EMIT scanStarted();
    m_inventory.gpus = queryGpu();
    m_inventory.scan_timestamp = QDateTime::currentDateTime();
    Q_EMIT scanComplete(m_inventory);
}

void HardwareInventoryScanner::scanBattery() {
    Q_EMIT scanStarted();
    m_inventory.battery = queryBattery();
    m_inventory.scan_timestamp = QDateTime::currentDateTime();
    Q_EMIT scanComplete(m_inventory);
}

void HardwareInventoryScanner::cancel() {
    m_cancelled.store(true, std::memory_order_relaxed);
    logInfo("Hardware inventory scan cancelled");
}

// ============================================================================
// WMI Helper
// ============================================================================

QVector<QVariantMap> HardwareInventoryScanner::wmiQuery(const QString& wmi_class,
                                                        const QStringList& properties,
                                                        int timeout_ms) {
    const QString prop_list = properties.join(", ");
    const QString ps_command = QString(
                                   "Get-CimInstance -ClassName %1 | "
                                   "Select-Object %2 | "
                                   "ConvertTo-Json -Compress")
                                   .arg(wmi_class, prop_list);

    QProcess ps;
    ps.setProcessChannelMode(QProcess::MergedChannels);
    ps.start("powershell.exe", {"-NoProfile", "-NoLogo", "-Command", ps_command});

    if (!ps.waitForStarted(sak::kTimeoutProcessStartMs)) {
        logError("PowerShell failed to start for WMI query on class {}", wmi_class.toStdString());
        return {};
    }
    if (!ps.waitForFinished(timeout_ms)) {
        logError("WMI query timed out for class {}", wmi_class.toStdString());
        ps.kill();
        return {};
    }

    if (ps.exitCode() != 0) {
        logError("WMI query failed for class {} (exit code {})",
                 wmi_class.toStdString(),
                 ps.exitCode());
        return {};
    }

    const QByteArray output = ps.readAllStandardOutput().trimmed();
    if (output.isEmpty()) {
        return {};
    }

    QJsonParseError parse_error{};
    const QJsonDocument doc = QJsonDocument::fromJson(output, &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        logError("WMI JSON parse error for class {}: {}",
                 wmi_class.toStdString(),
                 parse_error.errorString().toStdString());
        return {};
    }

    QVector<QVariantMap> results;

    if (doc.isArray()) {
        const QJsonArray arr = doc.array();
        results.reserve(arr.size());
        for (const auto& val : arr) {
            if (val.isObject()) {
                results.append(val.toObject().toVariantMap());
            }
        }
    } else if (doc.isObject()) {
        // Single result comes back as an object, not an array
        results.append(doc.object().toVariantMap());
    }

    return results;
}

// ============================================================================
// Component Queries
// ============================================================================

CpuInfo HardwareInventoryScanner::queryCpu() {
    CpuInfo info;

    const auto results = wmiQuery("Win32_Processor",
                                  {"Name",
                                   "Manufacturer",
                                   "NumberOfCores",
                                   "NumberOfLogicalProcessors",
                                   "MaxClockSpeed",
                                   "L2CacheSize",
                                   "L3CacheSize",
                                   "SocketDesignation",
                                   "Architecture",
                                   "LoadPercentage"});

    if (results.isEmpty()) {
        logWarning("No CPU information returned from WMI");
        return info;
    }

    const auto& cpu = results.first();
    info.name = cpu.value("Name").toString().trimmed();
    info.manufacturer = cpu.value("Manufacturer").toString().trimmed();
    info.cores = cpu.value("NumberOfCores").toUInt();
    info.threads = cpu.value("NumberOfLogicalProcessors").toUInt();
    info.max_clock_mhz = cpu.value("MaxClockSpeed").toUInt();
    info.l2_cache_kb = cpu.value("L2CacheSize").toUInt();
    info.l3_cache_kb = cpu.value("L3CacheSize").toUInt();
    info.socket = cpu.value("SocketDesignation").toString().trimmed();
    info.cpu_usage_percent = cpu.value("LoadPercentage").toDouble();

    // Architecture mapping
    const uint32_t arch = cpu.value("Architecture").toUInt();
    switch (arch) {
    case 0:
        info.architecture = "x86";
        break;
    case 9:
        info.architecture = "x64";
        break;
    case 12:
        info.architecture = "ARM64";
        break;
    default:
        info.architecture = "Unknown";
        break;
    }

    // Base clock: WMI doesn't distinguish base vs boost reliably;
    // MaxClockSpeed is the advertised max. For base, query CurrentClockSpeed
    // fallback but it changes with power states.
    info.base_clock_mhz = info.max_clock_mhz;

    logInfo("CPU detected: {} ({} cores / {} threads)",
            info.name.toStdString(),
            info.cores,
            info.threads);

    return info;
}

MemorySummary HardwareInventoryScanner::queryMemory() {
    MemorySummary summary;

    // Global memory status from Windows API
#ifdef SAK_PLATFORM_WINDOWS
    MEMORYSTATUSEX mem_status{};
    mem_status.dwLength = sizeof(mem_status);
    if (GlobalMemoryStatusEx(&mem_status)) {
        summary.total_bytes = mem_status.ullTotalPhys;
        summary.available_bytes = mem_status.ullAvailPhys;
    }
#endif

    // Per-module info from WMI
    const auto results = wmiQuery("Win32_PhysicalMemory",
                                  {"Manufacturer",
                                   "PartNumber",
                                   "Capacity",
                                   "Speed",
                                   "SMBIOSMemoryType",
                                   "FormFactor",
                                   "DeviceLocator",
                                   "SerialNumber"});

    summary.modules.reserve(results.size());
    uint32_t slot_index = 0;

    for (const auto& mod : results) {
        summary.modules.append(parseMemoryModule(mod, slot_index++));
    }

    summary.slots_used = static_cast<uint32_t>(summary.modules.size());

    // Total memory slots from Win32_PhysicalMemoryArray
    const auto array_results = wmiQuery("Win32_PhysicalMemoryArray", {"MemoryDevices"});
    if (!array_results.isEmpty()) {
        summary.slots_total = array_results.first().value("MemoryDevices").toUInt();
    } else {
        summary.slots_total = summary.slots_used;
    }

    logInfo("Memory: {} GB total, {}/{} slots used",
            summary.total_bytes / static_cast<uint64_t>(sak::kBytesPerGB),
            summary.slots_used,
            summary.slots_total);

    return summary;
}

MemoryModuleInfo HardwareInventoryScanner::parseMemoryModule(const QVariantMap& mod,
                                                             uint32_t slotIndex) {
    MemoryModuleInfo module;
    module.manufacturer = mod.value("Manufacturer").toString().trimmed();
    module.part_number = mod.value("PartNumber").toString().trimmed();
    module.capacity_bytes = mod.value("Capacity").toULongLong();
    module.speed_mhz = mod.value("Speed").toUInt();
    module.serial_number = mod.value("SerialNumber").toString().trimmed();
    module.slot = slotIndex;

    // SMBIOSMemoryType mapping (key values)
    const uint32_t smbios_type = mod.value("SMBIOSMemoryType").toUInt();
    switch (smbios_type) {
    case 20:
        module.memory_type = "DDR";
        break;
    case 21:
        module.memory_type = "DDR2";
        break;
    case 22:  // intentional fallthrough
    case 24:
        module.memory_type = "DDR3";
        break;
    case 26:
        module.memory_type = "DDR4";
        break;
    case 30:  // intentional fallthrough
    case 34:
        module.memory_type = "DDR5";
        break;
    default:
        module.memory_type = "Unknown";
        break;
    }

    // FormFactor mapping
    const uint32_t form = mod.value("FormFactor").toUInt();
    switch (form) {
    case 8:
        module.form_factor = "DIMM";
        break;
    case 12:
        module.form_factor = "SODIMM";
        break;
    default:
        module.form_factor = "Unknown";
        break;
    }

    return module;
}

QVector<StorageDeviceInfo> HardwareInventoryScanner::queryStorage() {
    const auto results = wmiQuery("Win32_DiskDrive",
                                  {"Model",
                                   "SerialNumber",
                                   "Size",
                                   "InterfaceType",
                                   "MediaType",
                                   "FirmwareRevision",
                                   "Index"});

    QVector<StorageDeviceInfo> devices;
    devices.reserve(results.size());

    for (const auto& disk : results) {
        StorageDeviceInfo dev;
        dev.model = disk.value("Model").toString().trimmed();
        dev.serial_number = disk.value("SerialNumber").toString().trimmed();
        dev.size_bytes = disk.value("Size").toULongLong();
        dev.firmware_version = disk.value("FirmwareRevision").toString().trimmed();
        dev.disk_number = disk.value("Index").toUInt();

        const QString iface = disk.value("InterfaceType").toString().trimmed();
        dev.interface_type = classifyInterfaceType(iface, dev.model);

        const QString media = disk.value("MediaType").toString();
        dev.media_type = classifyMediaType(dev.interface_type, dev.model, media);

        devices.append(dev);
    }

    // Enrich with partition/volume info from Qt
    enrichStorageWithVolumeInfo(devices);

    logInfo("Storage: {} device(s) detected", devices.size());
    return devices;
}

void HardwareInventoryScanner::enrichStorageWithVolumeInfo(QVector<StorageDeviceInfo>& devices) {
    const auto volumes = QStorageInfo::mountedVolumes();
    for (auto& dev : devices) {
        enrichDeviceWithVolumes(dev, volumes);
    }
}

void HardwareInventoryScanner::enrichDeviceWithVolumes(StorageDeviceInfo& dev,
                                                       const QList<QStorageInfo>& volumes) {
    for (const auto& vol : volumes) {
        // Match by checking if the volume's device contains the disk number
        // This is a best-effort match; exact mapping requires more APIs
        if (!vol.isValid() || !vol.isReady()) {
            continue;
        }

        const QString root = vol.rootPath();
        if (root.isEmpty()) {
            continue;
        }

        PartitionInfo part;
        part.drive_letter = root.left(2);  // "C:"
        part.label = vol.name();
        part.file_system = QString::fromUtf8(vol.fileSystemType());
        part.total_bytes = static_cast<uint64_t>(vol.bytesTotal());
        part.free_bytes = static_cast<uint64_t>(vol.bytesFree());
        part.is_boot = vol.isRoot();

        // Assign volumes to the first device that has capacity
        // (simplified; exact matching requires Win32_LogicalDiskToPartition)
        if (dev.partitions.isEmpty() || dev.size_bytes > part.total_bytes) {
            dev.partitions.append(part);
        }
    }
}

QVector<GpuInfo> HardwareInventoryScanner::queryGpu() {
    QVector<GpuInfo> gpus;

#ifdef SAK_PLATFORM_WINDOWS
    // Primary method: DXGI adapter enumeration (most accurate VRAM reporting)
    enumerateDxgiAdapters(gpus);
#endif

    // Enrich with driver info from WMI
    const auto wmi_gpus = wmiQuery("Win32_VideoController",
                                   {"Name",
                                    "DriverVersion",
                                    "DriverDate",
                                    "CurrentHorizontalResolution",
                                    "CurrentVerticalResolution",
                                    "CurrentRefreshRate"});

    for (int i = 0; i < gpus.size() && i < wmi_gpus.size(); ++i) {
        gpus[i].driver_version = wmi_gpus[i].value("DriverVersion").toString().trimmed();
        gpus[i].driver_date = wmi_gpus[i].value("DriverDate").toString().left(10);
        gpus[i].current_res_x = wmi_gpus[i].value("CurrentHorizontalResolution").toUInt();
        gpus[i].current_res_y = wmi_gpus[i].value("CurrentVerticalResolution").toUInt();
        gpus[i].refresh_rate = wmi_gpus[i].value("CurrentRefreshRate").toUInt();
    }

    // Fallback: if DXGI returned nothing, use WMI only
    if (gpus.isEmpty()) {
        for (const auto& wmi_gpu : wmi_gpus) {
            GpuInfo gpu;
            gpu.name = wmi_gpu.value("Name").toString().trimmed();
            gpu.driver_version = wmi_gpu.value("DriverVersion").toString().trimmed();
            gpu.driver_date = wmi_gpu.value("DriverDate").toString().left(10);
            gpu.current_res_x = wmi_gpu.value("CurrentHorizontalResolution").toUInt();
            gpu.current_res_y = wmi_gpu.value("CurrentVerticalResolution").toUInt();
            gpu.refresh_rate = wmi_gpu.value("CurrentRefreshRate").toUInt();
            gpus.append(gpu);
        }
    }

    logInfo("GPU: {} adapter(s) detected", gpus.size());
    return gpus;
}

void HardwareInventoryScanner::enumerateDxgiAdapters(QVector<GpuInfo>& gpus) {
#ifdef SAK_PLATFORM_WINDOWS
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (!SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (!SUCCEEDED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            adapter.Reset();
            continue;
        }

        GpuInfo gpu;
        gpu.name = QString::fromWCharArray(desc.Description);
        gpu.vram_bytes = desc.DedicatedVideoMemory;

        // Determine manufacturer from vendor ID
        switch (desc.VendorId) {
        case 0x10DE:
            gpu.manufacturer = "NVIDIA";
            break;
        case 0x1002:
            gpu.manufacturer = "AMD";
            break;
        case 0x8086:
            gpu.manufacturer = "Intel";
            break;
        default:
            gpu.manufacturer = "Unknown";
            break;
        }

        gpus.append(gpu);
        adapter.Reset();
    }
#endif
}

MotherboardInfo HardwareInventoryScanner::queryMotherboard() {
    MotherboardInfo info;

    // BaseBoard
    const auto boards = wmiQuery("Win32_BaseBoard", {"Manufacturer", "Product", "SerialNumber"});
    if (!boards.isEmpty()) {
        const auto& board = boards.first();
        info.manufacturer = board.value("Manufacturer").toString().trimmed();
        info.product = board.value("Product").toString().trimmed();
        info.serial_number = board.value("SerialNumber").toString().trimmed();
    }

    // BIOS
    const auto bioses = wmiQuery("Win32_BIOS",
                                 {"SMBIOSBIOSVersion", "ReleaseDate", "Manufacturer"});
    if (!bioses.isEmpty()) {
        const auto& bios = bioses.first();
        info.bios_version = bios.value("SMBIOSBIOSVersion").toString().trimmed();
        info.bios_date = bios.value("ReleaseDate").toString().left(10);
        info.bios_manufacturer = bios.value("Manufacturer").toString().trimmed();
    }

    logInfo("Motherboard: {} {}", info.manufacturer.toStdString(), info.product.toStdString());
    return info;
}

#ifdef SAK_PLATFORM_WINDOWS
bool HardwareInventoryScanner::queryBatteryPowerStatus(BatteryInfo& info) {
    SYSTEM_POWER_STATUS power_status{};
    if (!GetSystemPowerStatus(&power_status)) {
        return true;
    }

    constexpr BYTE kNoBatteryFlag = 128;
    info.present = (power_status.BatteryFlag != kNoBatteryFlag);

    if (!info.present) {
        logInfo("No battery detected");
        return false;
    }

    info.estimated_minutes = (power_status.BatteryLifeTime == static_cast<DWORD>(-1))
                                 ? 0
                                 : power_status.BatteryLifeTime / 60;

    switch (power_status.ACLineStatus) {
    case 0:
        info.status = "Discharging";
        break;
    case 1:
        info.status = "Charging";
        break;
    default:
        info.status = "Unknown";
        break;
    }

    return true;
}
#endif

BatteryInfo HardwareInventoryScanner::queryBattery() {
    BatteryInfo info;

#ifdef SAK_PLATFORM_WINDOWS
    if (!queryBatteryPowerStatus(info)) {
        return info;
    }
#endif

    const auto batteries = wmiQuery("Win32_Battery",
                                    {"Name",
                                     "DesignCapacity",
                                     "FullChargeCapacity",
                                     "EstimatedChargeRemaining",
                                     "BatteryStatus"});

    if (!batteries.isEmpty()) {
        const auto& bat = batteries.first();
        info.name = bat.value("Name").toString().trimmed();
        info.design_capacity = bat.value("DesignCapacity").toUInt();
        info.full_charge_capacity = bat.value("FullChargeCapacity").toUInt();

        if (info.design_capacity > 0 && info.full_charge_capacity > 0) {
            info.health_percent = (static_cast<double>(info.full_charge_capacity) /
                                   static_cast<double>(info.design_capacity)) *
                                  100.0;
        }
    }

    if (info.present) {
        logInfo("Battery: {} -- {:.1f}% health", info.name.toStdString(), info.health_percent);
    }

    return info;
}

void HardwareInventoryScanner::queryOsInfo() {
    const auto results =
        wmiQuery("Win32_OperatingSystem",
                 {"Caption", "Version", "BuildNumber", "OSArchitecture", "LastBootUpTime"});

    if (results.isEmpty()) {
        logWarning("No OS information returned from WMI");
        return;
    }

    const auto& os = results.first();
    m_inventory.os_name = os.value("Caption").toString().trimmed();
    m_inventory.os_build = os.value("BuildNumber").toString().trimmed();
    m_inventory.os_architecture = os.value("OSArchitecture").toString().trimmed();

    // Version string includes build; extract the display version separately
    const QString version_string = os.value("Version").toString().trimmed();
    m_inventory.os_version = version_string;

    // Uptime: parse LastBootUpTime (CIM_DATETIME format) or use GetTickCount64
#ifdef SAK_PLATFORM_WINDOWS
    m_inventory.uptime_seconds = GetTickCount64() / 1000ULL;
#endif

    logInfo("OS: {} (Build {})",
            m_inventory.os_name.toStdString(),
            m_inventory.os_build.toStdString());
}

}  // namespace sak

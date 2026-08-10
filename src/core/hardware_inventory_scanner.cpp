// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file hardware_inventory_scanner.cpp
/// @brief System hardware enumeration implementation

#include "sak/hardware_inventory_scanner.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/process_runner.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
constexpr int kScanProgressCpu = 0;
constexpr int kScanProgressMemory = 15;
constexpr int kScanProgressStorage = 30;
constexpr int kScanProgressGpu = 50;
constexpr int kScanProgressMotherboard = 65;
constexpr int kScanProgressBattery = 80;
constexpr int kScanProgressOs = 90;
constexpr int kScanProgressComplete = sak::kPercentMax;
constexpr uint32_t kCpuArchitectureX64 = 9;
constexpr uint32_t kCpuArchitectureArm64 = 12;
constexpr int kWmiDatePrefixLength = 10;
constexpr uint32_t kMemoryTypeDdr = 20;
constexpr uint32_t kMemoryTypeDdr2 = 21;
constexpr uint32_t kMemoryTypeDdr3 = 22;
constexpr uint32_t kMemoryTypeDdr3Synchronous = 24;
constexpr uint32_t kMemoryTypeDdr4 = 26;
constexpr uint32_t kMemoryTypeDdr5 = 30;
constexpr uint32_t kMemoryTypeLpDdr5 = 34;
constexpr uint32_t kMemoryFormFactorDimm = 8;
constexpr uint32_t kMemoryFormFactorSoDimm = 12;
constexpr int kDriveRootPrefixLength = 2;
constexpr unsigned int kVendorIdNvidia = 0x10DE;
constexpr unsigned int kVendorIdAmd = 0x1002;
constexpr unsigned int kVendorIdIntel = 0x8086;

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

/// @brief Copy driver/display metadata from a WMI Win32_VideoController row.
void enrichGpuFromWmi(sak::GpuInfo& gpu, const QVariantMap& wmi) {
    gpu.driver_version = wmi.value("DriverVersion").toString().trimmed();
    gpu.driver_date = wmi.value("DriverDate").toString().left(kWmiDatePrefixLength);
    gpu.current_res_x = wmi.value("CurrentHorizontalResolution").toUInt();
    gpu.current_res_y = wmi.value("CurrentVerticalResolution").toUInt();
    gpu.refresh_rate = wmi.value("CurrentRefreshRate").toUInt();
}

/// @brief Find the WMI video-controller row whose Name matches @p name.
///
/// DXGI enumeration filters out software (WARP) adapters while
/// Win32_VideoController lists every controller, so zipping the two lists by
/// index can attach one GPU's driver metadata to a different GPU on multi-GPU
/// systems. Matching by name attaches it to the right adapter; a null return
/// leaves the driver fields empty rather than mis-attributing them.
const QVariantMap* findWmiGpuByName(const QVector<QVariantMap>& wmiGpus, const QString& name) {
    for (const auto& wmi : wmiGpus) {
        if (wmi.value("Name").toString().trimmed().compare(name, Qt::CaseInsensitive) == 0) {
            return &wmi;
        }
    }
    return nullptr;
}

/// @brief Uptime in seconds parsed from a CIM_DATETIME LastBootUpTime string.
///
/// Uses the queried boot time ("yyyyMMddHHmmss.ffffff+UUU") so the value reflects
/// wall-clock uptime including sleep/hibernate, unlike GetTickCount64. Returns 0
/// (unknown) when the string is missing or unparseable -- no fabricated uptime.
uint64_t uptimeSecondsFromCimDateTime(const QString& cimDateTime) {
    constexpr int kCimDateTimeSecondsWidth = 14;  // yyyyMMddHHmmss
    if (cimDateTime.size() < kCimDateTimeSecondsWidth) {
        return 0;
    }
    const QDateTime boot = QDateTime::fromString(cimDateTime.left(kCimDateTimeSecondsWidth),
                                                 QStringLiteral("yyyyMMddHHmmss"));
    if (!boot.isValid()) {
        return 0;
    }
    const qint64 secs = boot.secsTo(QDateTime::currentDateTime());
    return secs > 0 ? static_cast<uint64_t>(secs) : 0;
}

// Insert one {Letter, DiskIndex} mapping, dropping entries that cannot be
// resolved to a real disk. DiskIndex is null when the partition had no matching
// disk (never attribute an unresolved volume to disk 0), and a negative index is
// out-of-range garbage (never wrap it into a huge uint32_t disk number).
void insertVolumeDiskEntry(QHash<QString, uint32_t>& map, const QJsonObject& obj) {
    const QString letter = obj.value("Letter").toString().trimmed().toUpper();
    const QJsonValue disk = obj.value("DiskIndex");
    if (letter.isEmpty() || disk.isNull() || !disk.isDouble() || disk.toInt() < 0) {
        return;
    }
    map.insert(letter, static_cast<uint32_t>(disk.toInt()));
}

// Parse trimmed WMI JSON @p output into @p results. Returns false on a genuine
// failure -- a JSON parse error, OR a syntactically valid but unexpected top-level
// shape (a scalar rather than the object/array that
// Get-CimInstance|ConvertTo-Json emits) -- so the caller flags an incomplete
// inventory instead of silently treating an empty parse as a clean success. An
// empty output is a valid "no instances" result and returns true.
bool parseWmiJsonOutput(const QByteArray& output,
                        const QString& wmiClass,
                        QVector<QVariantMap>& results) {
    if (output.isEmpty()) {
        return true;
    }
    QJsonParseError parse_error{};
    const QJsonDocument doc = QJsonDocument::fromJson(output, &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        sak::logError("WMI JSON parse error for class {}: {}",
                      wmiClass.toStdString(),
                      parse_error.errorString().toStdString());
        return false;
    }
    if (!doc.isObject() && !doc.isArray()) {
        sak::logError("WMI query for class {} returned an unexpected JSON shape",
                      wmiClass.toStdString());
        return false;
    }
    results = sak::HardwareInventoryScanner::jsonDocToVariantMaps(doc);
    return true;
}

#ifdef SAK_PLATFORM_WINDOWS
/// @brief Map a DXGI vendor ID to a human-readable manufacturer name.
QString dxgiVendorName(unsigned int vendor_id) {
    switch (vendor_id) {
    case kVendorIdNvidia:
        return QStringLiteral("NVIDIA");
    case kVendorIdAmd:
        return QStringLiteral("AMD");
    case kVendorIdIntel:
        return QStringLiteral("Intel");
    default:
        return QStringLiteral("Unknown");
    }
}

/// @brief Fill a GpuInfo from a (non-null) DXGI adapter.
/// @return false if the adapter description is unreadable or is a software
///         (WARP) adapter that should be skipped.
bool tryBuildGpuFromAdapter(IDXGIAdapter1* adapter, sak::GpuInfo& out) {
    DXGI_ADAPTER_DESC1 desc{};
    if (!SUCCEEDED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
        return false;
    }
    out.name = QString::fromWCharArray(desc.Description);
    out.vram_bytes = desc.DedicatedVideoMemory;
    out.manufacturer = dxgiVendorName(desc.VendorId);
    return true;
}
#endif

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
    m_wmiQueryFailed = false;
    Q_EMIT scanStarted();

    m_inventory = HardwareInventory{};

    // A cancelled checkpoint must still emit a TERMINAL signal or a UI awaiting
    // completion hangs forever: surface the cancel via errorOccurred (never a
    // clean scanComplete-as-success) then the terminal scanComplete with partials.
    const auto cancelled = [this]() -> bool {
        if (!m_cancelled.load(std::memory_order_relaxed)) {
            return false;
        }
        m_inventory.scan_timestamp = QDateTime::currentDateTime();
        Q_EMIT errorOccurred(QStringLiteral("Hardware inventory scan cancelled"));
        Q_EMIT scanComplete(m_inventory);
        return true;
    };

    if (cancelled()) {
        return;
    }
    Q_EMIT scanProgress(kScanProgressCpu, "Scanning CPU...");
    m_inventory.cpu = queryCpu();

    if (cancelled()) {
        return;
    }
    Q_EMIT scanProgress(kScanProgressMemory, "Scanning Memory...");
    m_inventory.memory = queryMemory();

    if (cancelled()) {
        return;
    }
    Q_EMIT scanProgress(kScanProgressStorage, "Scanning Storage...");
    m_inventory.storage = queryStorage();

    if (cancelled()) {
        return;
    }
    Q_EMIT scanProgress(kScanProgressGpu, "Scanning GPU...");
    m_inventory.gpus = queryGpu();

    if (cancelled()) {
        return;
    }
    Q_EMIT scanProgress(kScanProgressMotherboard, "Scanning Motherboard...");
    m_inventory.motherboard = queryMotherboard();

    if (cancelled()) {
        return;
    }
    Q_EMIT scanProgress(kScanProgressBattery, "Scanning Battery...");
    m_inventory.battery = queryBattery();

    if (cancelled()) {
        return;
    }
    Q_EMIT scanProgress(kScanProgressOs, "Querying OS information...");
    queryOsInfo();

    Q_EMIT scanProgress(kScanProgressComplete, "Scan complete");
    if (m_wmiQueryFailed) {
        logWarning("Hardware inventory scan completed with WMI query failure(s)");
    } else {
        logInfo("Hardware inventory scan completed successfully");
    }
    finishScan();
}

void HardwareInventoryScanner::scanCpu() {
    // Clear any stale cancel from a prior aborted scan: otherwise m_cancelled
    // stays true, wmiQuery's process is torn down as "cancelled", and finishScan
    // reports an empty inventory as a clean success (fail-open).
    m_cancelled.store(false, std::memory_order_relaxed);
    m_wmiQueryFailed = false;
    Q_EMIT scanStarted();
    m_inventory.cpu = queryCpu();
    finishScan();
}

void HardwareInventoryScanner::scanMemory() {
    m_cancelled.store(false, std::memory_order_relaxed);
    m_wmiQueryFailed = false;
    Q_EMIT scanStarted();
    m_inventory.memory = queryMemory();
    finishScan();
}

void HardwareInventoryScanner::scanStorage() {
    m_cancelled.store(false, std::memory_order_relaxed);
    m_wmiQueryFailed = false;
    Q_EMIT scanStarted();
    m_inventory.storage = queryStorage();
    finishScan();
}

void HardwareInventoryScanner::scanGpu() {
    m_cancelled.store(false, std::memory_order_relaxed);
    m_wmiQueryFailed = false;
    Q_EMIT scanStarted();
    m_inventory.gpus = queryGpu();
    finishScan();
}

void HardwareInventoryScanner::scanBattery() {
    m_cancelled.store(false, std::memory_order_relaxed);
    m_wmiQueryFailed = false;
    Q_EMIT scanStarted();
    m_inventory.battery = queryBattery();
    finishScan();
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

    // Shared resolver: the System32-qualified interpreter, empty when unresolvable so
    // this query aborts instead of letting CreateProcess search PATH/CWD.
    const QString ps_exe = sak::systemPowerShellPath();
    if (ps_exe.isEmpty()) {
        logError("WMI query for class {} aborted: system powershell.exe not resolvable",
                 wmi_class.toStdString());
        m_wmiQueryFailed = true;
        return {};
    }

    const auto result =
        sak::runProcess(ps_exe,
                        {QStringLiteral("-NoProfile"),
                         QStringLiteral("-NoLogo"),
                         QStringLiteral("-Command"),
                         ps_command},
                        timeout_ms,
                        [this]() { return m_cancelled.load(std::memory_order_relaxed); });

    bool parse_failed = false;
    QVector<QVariantMap> results;

    if (result.timed_out) {
        logError("WMI query timed out for class {}", wmi_class.toStdString());
    } else if (result.exit_code != 0 || result.cancelled) {
        logError("WMI query failed for class {} (exit code {})",
                 wmi_class.toStdString(),
                 result.exit_code);
    } else {
        parse_failed = !parseWmiJsonOutput(result.std_out.toUtf8().trimmed(), wmi_class, results);
    }

    // Record a genuine failure so scan() can report an incomplete inventory
    // instead of a silent success. An empty-but-successful result is fine.
    if (wmiQueryIsReportableFailure(
            result.timed_out, result.cancelled, result.exit_code, parse_failed)) {
        m_wmiQueryFailed = true;
    }

    return results;
}

bool HardwareInventoryScanner::wmiQueryIsReportableFailure(bool timedOut,
                                                           bool cancelled,
                                                           int exitCode,
                                                           bool jsonParseFailed) {
    if (cancelled) {
        return false;  // user aborted the scan -- not a data-integrity failure
    }
    return timedOut || exitCode != 0 || jsonParseFailed;
}

QVector<QVariantMap> HardwareInventoryScanner::jsonDocToVariantMaps(const QJsonDocument& doc) {
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
        // A single result comes back as an object, not an array.
        results.append(doc.object().toVariantMap());
    }
    return results;
}

void HardwareInventoryScanner::finishScan() {
    m_inventory.scan_timestamp = QDateTime::currentDateTime();
    if (m_cancelled.load(std::memory_order_relaxed)) {
        // A cancel observed during a single-component scan (the full scan() runs
        // its own cancel checkpoints and returns before here) must surface as a
        // terminal error, never a clean scanComplete-as-success over data the
        // query was torn down mid-read.
        Q_EMIT errorOccurred(QStringLiteral("Hardware inventory scan cancelled"));
    } else if (m_wmiQueryFailed) {
        Q_EMIT errorOccurred(
            "One or more hardware queries failed; the inventory may be incomplete.");
    }
    Q_EMIT scanComplete(m_inventory);
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
        // Every system has a processor: an empty Win32_Processor result with a
        // clean exit is a failed query, not a legitimately empty class. Flag it so
        // the scan reports an incomplete inventory instead of a silent success.
        logWarning("No CPU information returned from WMI");
        m_wmiQueryFailed = true;
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
    case kCpuArchitectureX64:
        info.architecture = "x64";
        break;
    case kCpuArchitectureArm64:
        info.architecture = "ARM64";
        break;
    default:
        info.architecture = "Unknown";
        break;
    }

    // Base clock is not exposed reliably via WMI: MaxClockSpeed is the advertised
    // max and CurrentClockSpeed swings with power state. Leave base_clock_mhz at 0
    // (unknown) rather than fabricate it from the max, which would misreport the
    // max frequency as the base.

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
    } else {
        logWarning("GlobalMemoryStatusEx failed ({}); total/available memory unknown",
                   GetLastError());
        m_wmiQueryFailed = true;
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

    // Total memory slots from Win32_PhysicalMemoryArray. If the query returns
    // nothing, leave slots_total at 0 (unknown) rather than fabricating it from
    // slots_used -- wmiQuery already flags a genuine query failure so an
    // incomplete inventory is surfaced instead of a fake "all slots populated".
    const auto array_results = wmiQuery("Win32_PhysicalMemoryArray", {"MemoryDevices"});
    if (!array_results.isEmpty()) {
        summary.slots_total = array_results.first().value("MemoryDevices").toUInt();
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
    case kMemoryTypeDdr:
        module.memory_type = "DDR";
        break;
    case kMemoryTypeDdr2:
        module.memory_type = "DDR2";
        break;
    case kMemoryTypeDdr3:
    case kMemoryTypeDdr3Synchronous:
        module.memory_type = "DDR3";
        break;
    case kMemoryTypeDdr4:
        module.memory_type = "DDR4";
        break;
    case kMemoryTypeDdr5:
    case kMemoryTypeLpDdr5:
        module.memory_type = "DDR5";
        break;
    default:
        module.memory_type = "Unknown";
        break;
    }

    // FormFactor mapping
    const uint32_t form = mod.value("FormFactor").toUInt();
    switch (form) {
    case kMemoryFormFactorDimm:
        module.form_factor = "DIMM";
        break;
    case kMemoryFormFactorSoDimm:
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
    const auto volume_disk_map = queryVolumeDiskMap();
    if (volume_disk_map.isEmpty() && !devices.isEmpty()) {
        logWarning("No volume-to-disk mapping available; partitions left unassigned");
    }
    for (auto& dev : devices) {
        enrichDeviceWithVolumes(dev, volumes, volume_disk_map);
    }
}

QHash<QString, uint32_t> HardwareInventoryScanner::queryVolumeDiskMap() {
    // Build "C:" -> physical disk index by chaining the two WMI association
    // classes: Win32_LogicalDiskToPartition (letter <-> partition) and
    // Win32_DiskPartition.DiskIndex (partition -> disk). Association-class
    // references carry only key properties, so join by partition DeviceID.
    const QString script = QStringLiteral(
        "$p=@{};"
        "Get-CimInstance Win32_DiskPartition|"
        "ForEach-Object{$p[$_.DeviceID]=$_.DiskIndex};"
        "Get-CimInstance Win32_LogicalDiskToPartition|"
        "ForEach-Object{[PSCustomObject]@{"
        "Letter=$_.Dependent.DeviceID;"
        "DiskIndex=$p[$_.Antecedent.DeviceID]}}|"
        "ConvertTo-Json -Compress");

    const QString ps_exe = sak::systemPowerShellPath();
    if (ps_exe.isEmpty()) {
        logWarning("Volume-to-disk mapping query aborted: system powershell.exe not resolvable");
        m_wmiQueryFailed = true;
        return {};
    }

    const auto result =
        sak::runProcess(ps_exe,
                        {QStringLiteral("-NoProfile"),
                         QStringLiteral("-NoLogo"),
                         QStringLiteral("-Command"),
                         script},
                        kDefaultWmiQueryTimeoutMs,
                        [this]() { return m_cancelled.load(std::memory_order_relaxed); });

    if (!result.succeeded()) {
        // Flag a genuine failure (not a user cancel) so the inventory is reported
        // incomplete instead of silently leaving partitions unassigned as if clean,
        // matching wmiQuery's failure convention.
        if (!result.cancelled) {
            m_wmiQueryFailed = true;
        }
        logWarning("Volume-to-disk mapping query failed (exit code {})", result.exit_code);
        return {};
    }

    return parseVolumeDiskMap(result.std_out.toUtf8().trimmed());
}

QHash<QString, uint32_t> HardwareInventoryScanner::parseVolumeDiskMap(const QByteArray& json) {
    QHash<QString, uint32_t> map;
    if (json.isEmpty()) {
        return map;
    }

    QJsonParseError parse_error{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        return map;
    }

    if (doc.isArray()) {
        const QJsonArray arr = doc.array();
        for (const auto& val : arr) {
            if (val.isObject()) {
                insertVolumeDiskEntry(map, val.toObject());
            }
        }
    } else if (doc.isObject()) {
        // A single mapping comes back as an object, not an array.
        insertVolumeDiskEntry(map, doc.object());
    }

    return map;
}

void HardwareInventoryScanner::enrichDeviceWithVolumes(
    StorageDeviceInfo& dev,
    const QList<QStorageInfo>& volumes,
    const QHash<QString, uint32_t>& volumeDiskMap) {
    for (const auto& vol : volumes) {
        if (!vol.isValid() || !vol.isReady()) {
            continue;
        }

        const QString root = vol.rootPath();
        if (root.isEmpty()) {
            continue;
        }

        // Exact match: attach a volume only when its drive letter resolves to
        // THIS physical disk. Volumes with no known mapping are skipped (fail
        // closed) rather than attributed to an arbitrary device by size.
        const QString letter = root.left(kDriveRootPrefixLength).toUpper();
        const auto it = volumeDiskMap.constFind(letter);
        if (it == volumeDiskMap.constEnd() || *it != dev.disk_number) {
            continue;
        }

        PartitionInfo part;
        part.drive_letter = root.left(kDriveRootPrefixLength);
        part.label = vol.name();
        part.file_system = QString::fromUtf8(vol.fileSystemType());
        part.total_bytes = static_cast<uint64_t>(vol.bytesTotal());
        part.free_bytes = static_cast<uint64_t>(vol.bytesFree());
        part.is_boot = vol.isRoot();
        dev.partitions.append(part);
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

    // Attach WMI driver metadata to the matching adapter BY NAME, not by array
    // index: DXGI skips software adapters while Win32_VideoController lists all,
    // so a positional zip can bind one GPU's driver info to a different GPU on
    // multi-GPU systems. No name match -> driver fields stay empty (fail closed).
    for (auto& gpu : gpus) {
        const QVariantMap* match = findWmiGpuByName(wmi_gpus, gpu.name);
        if (match != nullptr) {
            enrichGpuFromWmi(gpu, *match);
        }
    }

    // If DXGI enumerated no hardware adapter, WMI is the only remaining source
    // (not an error-masking fallback -- it is a distinct enumeration path).
    if (gpus.isEmpty()) {
        for (const auto& wmi_gpu : wmi_gpus) {
            GpuInfo gpu;
            gpu.name = wmi_gpu.value("Name").toString().trimmed();
            gpu.driver_version = wmi_gpu.value("DriverVersion").toString().trimmed();
            gpu.driver_date = wmi_gpu.value("DriverDate").toString().left(kWmiDatePrefixLength);
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
    for (UINT i = 0;; ++i) {
        const HRESULT hr = factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;  // enumerated every adapter
        }
        // Any other failure leaves `adapter` unpopulated -- stop rather than
        // dereference a null ComPtr (GetDesc1 would crash). The old loop only
        // broke on NOT_FOUND, so an error HRESULT fell through to the deref.
        if (FAILED(hr) || !adapter) {
            logWarning("DXGI EnumAdapters1 failed at index {} (hr {:#x})",
                       i,
                       static_cast<unsigned long>(hr));
            break;
        }

        GpuInfo gpu;
        if (tryBuildGpuFromAdapter(adapter.Get(), gpu)) {
            gpus.append(gpu);
        }
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
        info.bios_date = bios.value("ReleaseDate").toString().left(kWmiDatePrefixLength);
        info.bios_manufacturer = bios.value("Manufacturer").toString().trimmed();
    }

    logInfo("Motherboard: {} {}", info.manufacturer.toStdString(), info.product.toStdString());
    return info;
}

#ifdef SAK_PLATFORM_WINDOWS
bool HardwareInventoryScanner::queryBatteryPowerStatus(BatteryInfo& info) {
    SYSTEM_POWER_STATUS power_status{};
    if (!GetSystemPowerStatus(&power_status)) {
        // Do not swallow the failure silently: log it, then continue to the WMI
        // Win32_Battery query (the authoritative presence source) rather than
        // asserting a battery state we could not read.
        logWarning("GetSystemPowerStatus failed ({}); AC/battery state from API unavailable",
                   GetLastError());
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
                                 : power_status.BatteryLifeTime / sak::kSecondsPerMinute;

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
                                  sak::kPercentMaxF;
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
        // The host always runs an OS: an empty Win32_OperatingSystem result with a
        // clean exit is a failed query. Flag it so the scan reports an incomplete
        // inventory rather than a silent success with blank OS fields.
        logWarning("No OS information returned from WMI");
        m_wmiQueryFailed = true;
        return;
    }

    const auto& os = results.first();
    m_inventory.os_name = os.value("Caption").toString().trimmed();
    m_inventory.os_build = os.value("BuildNumber").toString().trimmed();
    m_inventory.os_architecture = os.value("OSArchitecture").toString().trimmed();

    // Version string includes build; extract the display version separately
    const QString version_string = os.value("Version").toString().trimmed();
    m_inventory.os_version = version_string;

    // Uptime from the queried LastBootUpTime (CIM_DATETIME) so it reflects
    // wall-clock uptime including sleep/hibernate, unlike GetTickCount64. Fails
    // closed to 0 (unknown) when the boot time is missing/unparseable.
    m_inventory.uptime_seconds =
        uptimeSecondsFromCimDateTime(os.value("LastBootUpTime").toString().trimmed());

    logInfo("OS: {} (Build {})",
            m_inventory.os_name.toStdString(),
            m_inventory.os_build.toStdString());
}

}  // namespace sak

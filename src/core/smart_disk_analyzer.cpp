// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file smart_disk_analyzer.cpp
/// @brief SMART disk health analysis implementation via bundled smartctl

#include "sak/smart_disk_analyzer.h"

#include "sak/bundled_tools_manager.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/process_runner.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#ifdef SAK_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>

#include <Windows.h>
#endif

namespace sak {

// ============================================================================
// SMART Attribute Thresholds for Health Assessment
// ============================================================================

namespace {

/// @brief Critical SATA SMART attribute IDs and their warning/critical thresholds
struct SmartThreshold {
    uint8_t id;
    int64_t warning_raw;   ///< Raw value triggering a warning
    int64_t critical_raw;  ///< Raw value triggering critical status
};

/// Key SATA attributes monitored for health assessment
constexpr SmartThreshold kSataThresholds[] = {
    {.id = 5, .warning_raw = 1, .critical_raw = 50},      // Reallocated_Sector_Ct
    {.id = 10, .warning_raw = 1, .critical_raw = 10},     // Spin_Retry_Count
    {.id = 187, .warning_raw = 1, .critical_raw = 10},    // Reported_Uncorrect
    {.id = 188, .warning_raw = 5, .critical_raw = 50},    // Command_Timeout
    {.id = 196, .warning_raw = 1, .critical_raw = 50},    // Reallocated_Event_Count
    {.id = 197, .warning_raw = 1, .critical_raw = 10},    // Current_Pending_Sector
    {.id = 198, .warning_raw = 1, .critical_raw = 10},    // Offline_Uncorrectable
    {.id = 199, .warning_raw = 10, .critical_raw = 100},  // UDMA_CRC_Error_Count
};

/// @brief NVMe percentage_used threshold values
constexpr uint8_t kNvmeWearWarningPercent = 80;
constexpr uint8_t kNvmeWearCriticalPercent = 95;
constexpr uint32_t kNvmeMediaErrorWarning = 1;

constexpr int kSmartctlTimeoutMs = 30'000;  // 30 seconds per drive
constexpr int kSmartctlFatalExitMask = 0x07;
constexpr uint8_t kSataAttrReallocatedSectorCount = 5;
constexpr uint8_t kSataAttrCurrentPendingSector = 197;
constexpr uint8_t kSataAttrUdmaCrcErrorCount = 199;
constexpr int64_t kReallocatedSectorsCriticalAdviceThreshold = 50;
constexpr uint32_t kUnsafeShutdownWarningThreshold = 100;
constexpr double kElevatedTemperatureCelsius = 55.0;
constexpr int64_t kHighPowerOnHoursThreshold = 50'000;
constexpr int64_t kHoursPerYear = 8760;
// Drive numbering is not guaranteed contiguous and can exceed 15 (many
// controllers, removable media). Probe past absent indices and stop only after
// a run of consecutive misses, bounded by an absolute cap.
constexpr uint32_t kPhysicalDriveProbeMax = 256;
constexpr uint32_t kPhysicalDriveMissRun = 8;

}  // anonymous namespace

// ============================================================================
// Construction
// ============================================================================

SmartDiskAnalyzer::SmartDiskAnalyzer(QObject* parent) : QObject(parent) {}

// ============================================================================
// Public API
// ============================================================================

void SmartDiskAnalyzer::analyzeAll() {
    // NOTE: m_reports is (re)built by this call and is legitimately empty on a fresh
    // analyzer, so it must NOT be asserted non-empty here -- the prior asserts were
    // an inverted precondition that crashed debug builds the first time a
    // standalone analyzer ran (e.g. the headless diagnostics.smart_scan op).
    m_cancelled.store(false, std::memory_order_relaxed);
    m_reports.clear();

    Q_EMIT analysisStarted();

    if (!isSmartctlAvailable()) {
        Q_EMIT errorOccurred("smartctl.exe not found -- cannot analyze SMART data");
        Q_EMIT analysisComplete(m_reports);
        return;
    }

    const auto drives = enumerateDrives();
    if (drives.isEmpty()) {
        Q_EMIT errorOccurred("No physical drives detected");
        Q_EMIT analysisComplete(m_reports);
        return;
    }

    logInfo("Starting SMART analysis for {} drive(s)", drives.size());

    for (int i = 0; i < drives.size(); ++i) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            logInfo("SMART analysis cancelled");
            break;
        }

        const int percent = static_cast<int>((i * kPercentMax) / drives.size());
        Q_EMIT analysisProgress(percent,
                                QString("Analyzing drive %1 (%2 of %3)...")
                                    .arg(drives[i])
                                    .arg(i + 1)
                                    .arg(drives.size()));

        analyzeDrive(drives[i]);
    }

    Q_EMIT analysisProgress(kPercentMax, "SMART analysis complete");
    Q_EMIT analysisComplete(m_reports);
    logInfo("SMART analysis complete -- {} report(s) generated", m_reports.size());
}

void SmartDiskAnalyzer::analyzeDrive(uint32_t disk_number) {
    const QByteArray json_data = runSmartctl(disk_number);
    if (json_data.isEmpty()) {
        Q_EMIT errorOccurred(
            QString("Failed to read SMART data for PhysicalDrive%1").arg(disk_number));
        return;
    }

    SmartReport report = parseSmartctlOutput(json_data, disk_number);

    assessHealth(report);
    generateRecommendations(report);

    // Fail-closed surfacing: a malformed or content-free smartctl payload leaves
    // the drive indeterminate. Report that as an error rather than silently
    // appending a report that reads as "fine".
    if (report.overall_health == SmartHealthStatus::Unknown) {
        Q_EMIT errorOccurred(QString("SMART data for PhysicalDrive%1 is incomplete or unreadable "
                                     "-- health could not be determined")
                                 .arg(disk_number));
    }

    report.scan_timestamp = QDateTime::currentDateTime();
    m_reports.append(report);

    Q_EMIT driveAnalyzed(report);
}

SmartReport SmartDiskAnalyzer::parseAndAssessForTesting(const QByteArray& json_data,
                                                        uint32_t disk_number) {
    SmartReport report = parseSmartctlOutput(json_data, disk_number);
    assessHealth(report);
    generateRecommendations(report);
    return report;
}

void SmartDiskAnalyzer::cancel() {
    m_cancelled.store(true, std::memory_order_relaxed);
    logInfo("SMART analysis cancellation requested");
}

bool SmartDiskAnalyzer::isSmartctlAvailable() const {
    const QString path = resolveSmartctlPath();
    if (path.isEmpty()) {
        return false;
    }

    const auto result =
        sak::runProcess(path, {QStringLiteral("--version")}, sak::kTimeoutSmartQueryMs, [this]() {
            return m_cancelled.load(std::memory_order_relaxed);
        });
    return result.succeeded();
}

// ============================================================================
// Private Implementation
// ============================================================================

QString SmartDiskAnalyzer::resolveSmartctlPath() {
    auto& tools = BundledToolsManager::instance();

    // Try the dedicated smartmontools category first
    if (tools.toolExists("smartmontools", "smartctl.exe")) {
        return tools.toolPath("smartmontools", "smartctl.exe");
    }

    // Fallback: look in generic tools directory
    if (tools.toolExists("generic", "smartctl.exe")) {
        return tools.toolPath("generic", "smartctl.exe");
    }

    logWarning("smartctl.exe not found in bundled tools");
    return {};
}

QByteArray SmartDiskAnalyzer::runSmartctl(uint32_t disk_number) {
    const QString smartctl_path = resolveSmartctlPath();
    if (smartctl_path.isEmpty()) {
        return {};
    }

    const QString device_path = QString("/dev/pd%1").arg(disk_number);

    const auto result =
        sak::runProcess(smartctl_path,
                        {QStringLiteral("-a"),        // All SMART info
                         QStringLiteral("--json=c"),  // Compact JSON output
                         device_path},
                        kSmartctlTimeoutMs,
                        [this]() { return m_cancelled.load(std::memory_order_relaxed); });

    if (result.timed_out) {
        logError("smartctl timed out for drive {}", disk_number);
        return {};
    }

    // Fail closed on a cancelled run, a crash-exit, or a truncated capture: any of
    // these can leave std_out holding a partial JSON document whose fatal-exit bits
    // (0-2) happen to be clear. Handing a truncated/aborted payload to the parser as
    // if it were a complete reading is exactly the fail-open this must not do.
    if (result.cancelled) {
        logInfo("smartctl cancelled for drive {}", disk_number);
        return {};
    }
    if (result.output_truncated) {
        logError("smartctl output truncated for drive {} -- refusing partial SMART data",
                 disk_number);
        return {};
    }
    if (result.exit_status != 0) {
        logError("smartctl crash-exited for drive {} (exit_status {})",
                 disk_number,
                 result.exit_status);
        return {};
    }

    // smartctl exit codes are bitmasks:
    //   Bit 0: Command line parse error
    //   Bit 1: Device open failed
    //   Bit 2: SMART command failed
    //   Bit 3: SMART status: DISK FAILING
    //   Bit 4: SMART prefail attributes <= threshold
    //   Bit 5: SMART OK but attributes below threshold
    //   Bit 6: Error log has errors
    //   Bit 7: Self-test log has errors
    // Bits 3-7 are informational; only bits 0-2 are true failures
    const int exit_code = result.exit_code;
    if ((exit_code & kSmartctlFatalExitMask) != 0) {
        const QString stderr_text = result.std_err;
        logError("smartctl failed for drive {} (exit {}): {}",
                 disk_number,
                 exit_code,
                 stderr_text.toStdString());
        return {};
    }

    return result.std_out.toUtf8();
}

void SmartDiskAnalyzer::parseSmartctlDeviceFields(const QJsonObject& root, SmartReport& report) {
    if (root.contains("device")) {
        const auto device = root["device"].toObject();
        report.interface_type = device.value("type").toString().toUpper();
        if (report.interface_type == "SAT") {
            report.interface_type = "SATA";
        }
    }

    if (root.contains("model_name")) {
        report.model = root.value("model_name").toString();
    }
    if (root.contains("serial_number")) {
        report.serial_number = root.value("serial_number").toString();
    }
    if (root.contains("firmware_version")) {
        report.firmware_version = root.value("firmware_version").toString();
    }
    if (root.contains("user_capacity")) {
        const auto cap = root["user_capacity"].toObject();
        report.size_bytes = static_cast<uint64_t>(cap.value("bytes").toInteger());
    }
}

void SmartDiskAnalyzer::parseSmartctlHealthFields(const QJsonObject& root, SmartReport& report) {
    if (root.contains("smart_status")) {
        const auto status = root["smart_status"].toObject();
        report.smart_status = status.value("passed").toBool() ? "PASSED" : "FAILED";
    }
    if (root.contains("temperature")) {
        const auto temp = root["temperature"].toObject();
        report.temperature_celsius = temp.value("current").toDouble();
    }
    if (root.contains("power_on_time")) {
        const auto pot = root["power_on_time"].toObject();
        report.power_on_hours = pot.value("hours").toInteger();
    }
}

SmartReport SmartDiskAnalyzer::parseSmartctlOutput(const QByteArray& json_data,
                                                   uint32_t disk_number) {
    SmartReport report;
    report.device_path = QString("\\\\.\\PhysicalDrive%1").arg(disk_number);

    QJsonParseError parse_error{};
    const QJsonDocument doc = QJsonDocument::fromJson(json_data, &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        logError("Failed to parse smartctl JSON for drive {}: {}",
                 disk_number,
                 parse_error.errorString().toStdString());
        return report;
    }

    const QJsonObject root = doc.object();
    parseSmartctlDeviceFields(root, report);
    parseSmartctlHealthFields(root, report);

    if (root.contains("ata_smart_attributes")) {
        parseSataAttributes(root["ata_smart_attributes"].toObject(), report);
    }
    if (root.contains("nvme_smart_health_information_log")) {
        parseNvmeHealth(root["nvme_smart_health_information_log"].toObject(), report);
    }

    return report;
}

void SmartDiskAnalyzer::parseSataAttributes(const QJsonObject& ata_smart_obj, SmartReport& report) {
    // An empty/absent object is valid (a drive may expose no SATA table): the loop
    // below simply produces no attributes. Do not assert non-empty -- malformed or
    // minimal smartctl output must degrade, not crash a debug build.
    const QJsonArray table = ata_smart_obj.value("table").toArray();

    report.attributes.reserve(table.size());

    for (const auto& entry : table) {
        const QJsonObject attr_obj = entry.toObject();

        SmartAttribute attr;
        attr.id = static_cast<uint8_t>(attr_obj.value("id").toInt());
        // A null or empty table entry (`table:[null]` / `table:[{}]`) parses to id 0.
        // No real ATA SMART attribute uses id 0, and appending such a phantom would
        // give assessHealth() an in-threshold attribute -- enough to satisfy
        // reportHasAssessableData() and let a data-less payload read as Healthy. Skip
        // it so the report stays fail-closed (no assessable data -> Unknown).
        if (attr.id == 0) {
            continue;
        }
        attr.name = attr_obj.value("name").toString();
        attr.current_value = static_cast<uint8_t>(attr_obj.value("value").toInt());
        attr.worst_value = static_cast<uint8_t>(attr_obj.value("worst").toInt());
        attr.threshold = static_cast<uint8_t>(attr_obj.value("thresh").toInt());

        // Flags
        const auto flags = attr_obj.value("flags").toObject();
        attr.flags = flags.value("string").toString();

        // Raw value
        const auto raw = attr_obj.value("raw").toObject();
        attr.raw_value = raw.value("value").toInteger();

        // Check if attribute is failing
        if (attr.threshold > 0 && attr.current_value <= attr.threshold) {
            attr.failing = true;
        }

        report.attributes.append(attr);

        // Extract key metrics
        switch (attr.id) {
        case kSataAttrReallocatedSectorCount:
            report.reallocated_sectors = attr.raw_value;
            break;
        case kSataAttrCurrentPendingSector:
            report.pending_sectors = attr.raw_value;
            break;
        default:
            break;
        }
    }
}

void SmartDiskAnalyzer::parseNvmeHealth(const QJsonObject& nvme_obj, SmartReport& report) {
    // Fail closed on a null/empty log. `nvme_smart_health_information_log: null`
    // (or `{}`) parses to an empty object here; building an all-zero record from it
    // would set report.nvme_health, satisfy reportHasAssessableData(), and let a
    // data-less payload read as Healthy -- and it would clobber a good top-level
    // temperature/power-on reading with zero. A real NVMe log is never empty; an
    // empty one is malformed input, not a minimal drive, so degrade to no NVMe data
    // (Unknown) rather than a phantom clean record. Never crashes on malformed input.
    if (nvme_obj.isEmpty()) {
        return;
    }
    NvmeHealthInfo nvme;

    nvme.percentage_used = static_cast<uint8_t>(nvme_obj.value("percentage_used").toInt());
    nvme.data_units_read = static_cast<uint64_t>(nvme_obj.value("data_units_read").toInteger());
    nvme.data_units_written =
        static_cast<uint64_t>(nvme_obj.value("data_units_written").toInteger());
    nvme.power_on_hours = static_cast<uint64_t>(nvme_obj.value("power_on_hours").toInteger());
    nvme.unsafe_shutdowns = static_cast<uint32_t>(nvme_obj.value("unsafe_shutdowns").toInt());
    nvme.media_errors = static_cast<uint32_t>(nvme_obj.value("media_errors").toInt());
    nvme.error_log_entries = static_cast<uint32_t>(nvme_obj.value("num_err_log_entries").toInt());
    nvme.temperature = static_cast<int16_t>(nvme_obj.value("temperature").toInt());
    nvme.available_spare = static_cast<uint16_t>(nvme_obj.value("available_spare").toInt());
    nvme.available_spare_threshold =
        static_cast<uint16_t>(nvme_obj.value("available_spare_threshold").toInt());

    report.nvme_health = nvme;
    report.wear_level_percent = static_cast<double>(nvme.percentage_used);
    // Only override the top-level power-on/temperature when the log actually carried
    // them. A partial log missing one of these members must not overwrite a valid
    // reading (parsed from "power_on_time"/"temperature") with a default zero, which
    // would suppress the age/temperature warnings.
    if (nvme_obj.contains("power_on_hours")) {
        report.power_on_hours = static_cast<int64_t>(nvme.power_on_hours);
    }
    if (nvme_obj.contains("temperature")) {
        report.temperature_celsius = static_cast<double>(nvme.temperature);
    }
}

SmartHealthStatus SmartDiskAnalyzer::checkAttributeAgainstThresholds(
    const SmartAttribute& attr) const {
    for (const auto& thresh : kSataThresholds) {
        if (attr.id != thresh.id) {
            continue;
        }
        if (attr.raw_value >= thresh.critical_raw) {
            return SmartHealthStatus::Critical;
        }
        if (attr.raw_value >= thresh.warning_raw) {
            return SmartHealthStatus::Warning;
        }
    }
    return SmartHealthStatus::Healthy;
}

bool SmartDiskAnalyzer::reportHasAssessableData(const SmartReport& report) {
    // Fail-closed signal set: an overall SMART status, at least one SATA
    // attribute, or an NVMe health log. With none of these present the drive
    // gave us nothing to judge, so health must stay Unknown rather than
    // defaulting to Healthy (which would hide a failing/unreadable disk).
    return !report.smart_status.isEmpty() || !report.attributes.isEmpty() ||
           report.nvme_health.has_value();
}

void SmartDiskAnalyzer::assessHealth(SmartReport& report) {
    // Fail closed: a malformed/empty smartctl payload parses into a data-less
    // report. Without any SMART signal we cannot claim the drive is Healthy --
    // leave it Unknown so the caller surfaces "could not determine", not a
    // green result over a disk we never actually read.
    if (!reportHasAssessableData(report)) {
        report.overall_health = SmartHealthStatus::Unknown;
        return;
    }

    report.overall_health = SmartHealthStatus::Healthy;

    if (report.smart_status == "FAILED") {
        report.overall_health = SmartHealthStatus::Critical;
        return;
    }

    if (std::ranges::any_of(report.attributes, [](const auto& attr) { return attr.failing; })) {
        report.overall_health = SmartHealthStatus::Critical;
        return;
    }

    assessSataAttributeHealth(report);
    assessNvmeHealth(report);
}

void SmartDiskAnalyzer::assessSataAttributeHealth(SmartReport& report) const {
    for (const auto& attr : report.attributes) {
        auto status = checkAttributeAgainstThresholds(attr);
        if (status == SmartHealthStatus::Critical) {
            report.overall_health = SmartHealthStatus::Critical;
            return;
        }
        if (status == SmartHealthStatus::Warning) {
            report.overall_health = SmartHealthStatus::Warning;
        }
    }
}

void SmartDiskAnalyzer::assessNvmeHealth(SmartReport& report) const {
    if (!report.nvme_health.has_value()) {
        return;
    }
    const auto& nvme = report.nvme_health.value();

    if (nvme.percentage_used >= kNvmeWearCriticalPercent || nvme.media_errors > 0 ||
        nvme.available_spare < nvme.available_spare_threshold) {
        report.overall_health = SmartHealthStatus::Critical;
        return;
    }

    if (nvme.percentage_used >= kNvmeWearWarningPercent) {
        report.overall_health = SmartHealthStatus::Warning;
    }
}

void SmartDiskAnalyzer::generateSataRecommendations(SmartReport& report) {
    if (report.reallocated_sectors > 0) {
        report.warnings.append(
            QString("Reallocated sectors detected: %1").arg(report.reallocated_sectors));

        if (report.reallocated_sectors >= kReallocatedSectorsCriticalAdviceThreshold) {
            report.recommendations.append(
                "CRITICAL: Drive has significant sector damage -- back up data immediately and "
                "replace drive");
        } else {
            report.recommendations.append(
                "Monitor reallocated sector count -- back up important data as a precaution");
        }
    }

    if (report.pending_sectors > 0) {
        report.warnings.append(
            QString("Pending sectors awaiting reallocation: %1").arg(report.pending_sectors));
        report.recommendations.append(
            "Run a full drive surface scan -- pending sectors may indicate developing issues");
    }

    // Check for CRC errors (attribute 199)
    for (const auto& attr : report.attributes) {
        if (attr.id == kSataAttrUdmaCrcErrorCount && attr.raw_value > 0) {
            report.warnings.append(QString("UDMA CRC errors detected: %1").arg(attr.raw_value));
            report.recommendations.append("Check SATA cable connections or replace SATA cable");
        }
    }
}

void SmartDiskAnalyzer::generateNvmeRecommendations(SmartReport& report) {
    if (!report.nvme_health.has_value()) {
        return;
    }

    const auto& nvme = report.nvme_health.value();

    if (nvme.percentage_used >= kNvmeWearCriticalPercent) {
        report.warnings.append(QString("NVMe drive endurance at %1% -- nearing end of life")
                                   .arg(nvme.percentage_used));
        report.recommendations.append(
            "CRITICAL: Plan drive replacement -- SSD endurance nearly exhausted");
    } else if (nvme.percentage_used >= kNvmeWearWarningPercent) {
        report.warnings.append(QString("NVMe drive endurance at %1%").arg(nvme.percentage_used));
        report.recommendations.append("Consider planning drive replacement in the near future");
    }

    if (nvme.media_errors > 0) {
        report.warnings.append(QString("NVMe media errors detected: %1").arg(nvme.media_errors));
        report.recommendations.append(
            "Media errors indicate flash cell failure -- back up data and monitor closely");
    }

    if (nvme.unsafe_shutdowns > kUnsafeShutdownWarningThreshold) {
        report.warnings.append(
            QString("High number of unsafe shutdowns: %1").arg(nvme.unsafe_shutdowns));
        report.recommendations.append(
            "Investigate power supply or shutdown procedures to reduce unsafe shutdowns");
    }

    if (nvme.available_spare < nvme.available_spare_threshold) {
        report.warnings.append("Available spare NVM below threshold");
        report.recommendations.append("Drive spare capacity is low -- plan for replacement");
    }
}

void SmartDiskAnalyzer::generateRecommendations(SmartReport& report) {
    report.warnings.clear();
    report.recommendations.clear();

    // -- Temperature warnings ------------------------------------
    if (report.temperature_celsius > kElevatedTemperatureCelsius) {
        report.warnings.append(QString("Drive temperature is elevated (%1 degC)")
                                   .arg(report.temperature_celsius, 0, 'f', 0));
        report.recommendations.append("Check case airflow and ensure drive has adequate cooling");
    }

    // -- SATA-specific recommendations ---------------------------
    generateSataRecommendations(report);

    // -- NVMe-specific recommendations ---------------------------
    generateNvmeRecommendations(report);

    // -- Power-on hours advisory ---------------------------------
    if (report.power_on_hours > kHighPowerOnHoursThreshold) {
        report.warnings.append(QString("Drive has %1 power-on hours (~%2 years)")
                                   .arg(report.power_on_hours)
                                   .arg(report.power_on_hours / kHoursPerYear));
        report.recommendations.append(
            "High usage drive -- consider proactive replacement for critical workloads");
    }

    // -- Overall SMART failure ------------------------------------
    if (report.smart_status == "FAILED") {
        report.warnings.prepend("SMART overall health assessment: FAILED");
        report.recommendations.prepend(
            "CRITICAL: Drive is reporting imminent failure -- back up all data immediately and "
            "replace drive");
    }

    // Fail-closed: an indeterminate report must NOT read as "health is good".
    // Say plainly that health is unknown so a data-less scan is never mistaken
    // for a clean bill.
    if (report.overall_health == SmartHealthStatus::Unknown) {
        report.warnings.append("SMART health could not be determined for this drive");
        report.recommendations.append(
            "SMART data was unavailable or unreadable -- verify the drive connection and "
            "re-run with administrator privileges");
        return;
    }

    // If no issues found, add a positive note
    if (report.warnings.isEmpty()) {
        report.recommendations.append("Drive health is good -- no action required");
    }
}

#ifdef SAK_PLATFORM_WINDOWS
namespace {

// True when \\.\PhysicalDriveN exists. Opened with zero access -- an existence
// probe only, never read/write. A genuine not-found is the only "absent" answer;
// any other open failure (in use, access denied) means the device IS present, so
// enumeration neither drops a real drive nor stops the scan run prematurely.
bool physicalDriveExists(uint32_t index) {
    const QString dev_path = QString("\\\\.\\PhysicalDrive%1").arg(index);
    HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(dev_path.utf16()),
                           0,  // No read/write access needed -- just checking existence
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr,
                           OPEN_EXISTING,
                           0,
                           nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return true;
    }
    const DWORD err = GetLastError();
    return err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND;
}

}  // namespace
#endif

QVector<uint32_t> SmartDiskAnalyzer::enumerateDrives() {
    QVector<uint32_t> drives;

#ifdef SAK_PLATFORM_WINDOWS
    // Probe PhysicalDriveN tolerating gaps in numbering; stop after a run of
    // consecutive misses rather than a fixed 0..15 window that hides higher or
    // sparsely numbered drives.
    uint32_t consecutive_misses = 0;
    for (uint32_t i = 0; i < kPhysicalDriveProbeMax && consecutive_misses < kPhysicalDriveMissRun;
         ++i) {
        if (physicalDriveExists(i)) {
            drives.append(i);
            consecutive_misses = 0;
        } else {
            ++consecutive_misses;
        }
    }
#endif

    logInfo("Enumerated {} physical drive(s)", drives.size());
    return drives;
}

}  // namespace sak

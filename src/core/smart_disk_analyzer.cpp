// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file smart_disk_analyzer.cpp
/// @brief SMART disk health analysis implementation via bundled smartctl

#include "sak/smart_disk_analyzer.h"

#include "sak/bundled_tools_manager.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

#ifdef SAK_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
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
    const char* name;
    int64_t warning_raw;   ///< Raw value triggering a warning
    int64_t critical_raw;  ///< Raw value triggering critical status
};

/// Key SATA attributes monitored for health assessment
constexpr SmartThreshold kSataThresholds[] = {
    {5, "Reallocated_Sector_Ct", 1, 50},
    {10, "Spin_Retry_Count", 1, 10},
    {187, "Reported_Uncorrect", 1, 10},
    {188, "Command_Timeout", 5, 50},
    {196, "Reallocated_Event_Count", 1, 50},
    {197, "Current_Pending_Sector", 1, 10},
    {198, "Offline_Uncorrectable", 1, 10},
    {199, "UDMA_CRC_Error_Count", 10, 100},
};

/// @brief NVMe percentage_used threshold values
constexpr uint8_t kNvmeWearWarningPercent = 80;
constexpr uint8_t kNvmeWearCriticalPercent = 95;
constexpr uint32_t kNvmeMediaErrorWarning = 1;

constexpr int kSmartctlTimeoutMs = 30'000;  // 30 seconds per drive

}  // anonymous namespace

// ============================================================================
// Construction
// ============================================================================

SmartDiskAnalyzer::SmartDiskAnalyzer(QObject* parent) : QObject(parent) {}

// ============================================================================
// Public API
// ============================================================================

void SmartDiskAnalyzer::analyzeAll() {
    Q_ASSERT(!m_reports.empty());
    Q_ASSERT(!m_reports.isEmpty());
    m_cancelled.store(false, std::memory_order_relaxed);
    m_reports.clear();

    Q_EMIT analysisStarted();

    if (!isSmartctlAvailable()) {
        Q_EMIT errorOccurred("smartctl.exe not found — cannot analyze SMART data");
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

        const int percent = static_cast<int>((i * 100) / drives.size());
        Q_EMIT analysisProgress(percent,
                                QString("Analyzing drive %1 (%2 of %3)...")
                                    .arg(drives[i])
                                    .arg(i + 1)
                                    .arg(drives.size()));

        analyzeDrive(drives[i]);
    }

    Q_EMIT analysisProgress(100, "SMART analysis complete");
    Q_EMIT analysisComplete(m_reports);
    logInfo("SMART analysis complete — {} report(s) generated", m_reports.size());
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

    report.scan_timestamp = QDateTime::currentDateTime();
    m_reports.append(report);

    Q_EMIT driveAnalyzed(report);
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

    QProcess proc;
    proc.start(path, {"--version"});
    if (!proc.waitForStarted(sak::kTimeoutProcessStartMs)) {
        return false;
    }
    return proc.waitForFinished(sak::kTimeoutSmartQueryMs) && proc.exitCode() == 0;
}

// ============================================================================
// Private Implementation
// ============================================================================

QString SmartDiskAnalyzer::resolveSmartctlPath() const {
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

    // Request all SMART info in JSON format
    QProcess proc;
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.start(smartctl_path,
               {"-a",        // All SMART info
                "--json=c",  // Compact JSON output
                device_path});

    if (!proc.waitForStarted(sak::kTimeoutProcessStartMs)) {
        logError("smartctl failed to start for drive {}", disk_number);
        return {};
    }
    if (!proc.waitForFinished(kSmartctlTimeoutMs)) {
        logError("smartctl timed out for drive {}", disk_number);
        proc.kill();
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
    const int exit_code = proc.exitCode();
    if (exit_code & 0x07) {
        const QString stderr_text = QString::fromUtf8(proc.readAllStandardError());
        logError("smartctl failed for drive {} (exit {}): {}",
                 disk_number,
                 exit_code,
                 stderr_text.toStdString());
        return {};
    }

    return proc.readAllStandardOutput();
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
    Q_ASSERT(!ata_smart_obj.isEmpty());
    const QJsonArray table = ata_smart_obj.value("table").toArray();

    report.attributes.reserve(table.size());

    for (const auto& entry : table) {
        const QJsonObject attr_obj = entry.toObject();

        SmartAttribute attr;
        attr.id = static_cast<uint8_t>(attr_obj.value("id").toInt());
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
        case 5:
            report.reallocated_sectors = attr.raw_value;
            break;
        case 197:
            report.pending_sectors = attr.raw_value;
            break;
        default:
            break;
        }
    }
}

void SmartDiskAnalyzer::parseNvmeHealth(const QJsonObject& nvme_obj, SmartReport& report) {
    Q_ASSERT(!nvme_obj.isEmpty());
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
    report.power_on_hours = static_cast<int64_t>(nvme.power_on_hours);
    report.temperature_celsius = static_cast<double>(nvme.temperature);
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

void SmartDiskAnalyzer::assessHealth(SmartReport& report) {
    report.overall_health = SmartHealthStatus::Healthy;

    if (report.smart_status == "FAILED") {
        report.overall_health = SmartHealthStatus::Critical;
        return;
    }

    for (const auto& attr : report.attributes) {
        if (attr.failing) {
            report.overall_health = SmartHealthStatus::Critical;
            return;
        }
    }

    assessSataAttributeHealth(report);
    assessNvmeHealth(report);
}

void SmartDiskAnalyzer::assessSataAttributeHealth(SmartReport& report) {
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

void SmartDiskAnalyzer::assessNvmeHealth(SmartReport& report) {
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

        if (report.reallocated_sectors >= 50) {
            report.recommendations.append(
                "CRITICAL: Drive has significant sector damage — back up data immediately and "
                "replace drive");
        } else {
            report.recommendations.append(
                "Monitor reallocated sector count — back up important data as a precaution");
        }
    }

    if (report.pending_sectors > 0) {
        report.warnings.append(
            QString("Pending sectors awaiting reallocation: %1").arg(report.pending_sectors));
        report.recommendations.append(
            "Run a full drive surface scan — pending sectors may indicate developing issues");
    }

    // Check for CRC errors (attribute 199)
    for (const auto& attr : report.attributes) {
        if (attr.id == 199 && attr.raw_value > 0) {
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
        report.warnings.append(
            QString("NVMe drive endurance at %1% — nearing end of life").arg(nvme.percentage_used));
        report.recommendations.append(
            "CRITICAL: Plan drive replacement — SSD endurance nearly exhausted");
    } else if (nvme.percentage_used >= kNvmeWearWarningPercent) {
        report.warnings.append(QString("NVMe drive endurance at %1%").arg(nvme.percentage_used));
        report.recommendations.append("Consider planning drive replacement in the near future");
    }

    if (nvme.media_errors > 0) {
        report.warnings.append(QString("NVMe media errors detected: %1").arg(nvme.media_errors));
        report.recommendations.append(
            "Media errors indicate flash cell failure — back up data and monitor closely");
    }

    if (nvme.unsafe_shutdowns > 100) {
        report.warnings.append(
            QString("High number of unsafe shutdowns: %1").arg(nvme.unsafe_shutdowns));
        report.recommendations.append(
            "Investigate power supply or shutdown procedures to reduce unsafe shutdowns");
    }

    if (nvme.available_spare < nvme.available_spare_threshold) {
        report.warnings.append("Available spare NVM below threshold");
        report.recommendations.append("Drive spare capacity is low — plan for replacement");
    }
}

void SmartDiskAnalyzer::generateRecommendations(SmartReport& report) {
    report.warnings.clear();
    report.recommendations.clear();

    // ── Temperature warnings ────────────────────────────────────
    if (report.temperature_celsius > 55.0) {
        report.warnings.append(QString("Drive temperature is elevated (%1°C)")
                                   .arg(report.temperature_celsius, 0, 'f', 0));
        report.recommendations.append("Check case airflow and ensure drive has adequate cooling");
    }

    // ── SATA-specific recommendations ───────────────────────────
    generateSataRecommendations(report);

    // ── NVMe-specific recommendations ───────────────────────────
    generateNvmeRecommendations(report);

    // ── Power-on hours advisory ─────────────────────────────────
    if (report.power_on_hours > 50'000) {
        report.warnings.append(QString("Drive has %1 power-on hours (~%2 years)")
                                   .arg(report.power_on_hours)
                                   .arg(report.power_on_hours / 8760));
        report.recommendations.append(
            "High usage drive — consider proactive replacement for critical workloads");
    }

    // ── Overall SMART failure ────────────────────────────────────
    if (report.smart_status == "FAILED") {
        report.warnings.prepend("SMART overall health assessment: FAILED");
        report.recommendations.prepend(
            "CRITICAL: Drive is reporting imminent failure — back up all data immediately and "
            "replace drive");
    }

    // If no issues found, add a positive note
    if (report.warnings.isEmpty()) {
        report.recommendations.append("Drive health is good — no action required");
    }
}

QVector<uint32_t> SmartDiskAnalyzer::enumerateDrives() {
    QVector<uint32_t> drives;

#ifdef SAK_PLATFORM_WINDOWS
    // Probe PhysicalDrive0..15
    for (uint32_t i = 0; i < 16; ++i) {
        const QString dev_path = QString("\\\\.\\PhysicalDrive%1").arg(i);
        HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(dev_path.utf16()),
                               0,  // No read/write access needed — just checking existence
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr,
                               OPEN_EXISTING,
                               0,
                               nullptr);

        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            drives.append(i);
        }
    }
#endif

    logInfo("Enumerated {} physical drive(s)", drives.size());
    return drives;
}

}  // namespace sak

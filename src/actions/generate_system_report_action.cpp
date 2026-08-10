// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file generate_system_report_action.cpp
/// @brief Implements system information report generation

#include "sak/actions/generate_system_report_action.h"

#include "sak/action_constants.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/process_runner.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QStorageInfo>
#include <QSysInfo>

namespace sak {

namespace {

constexpr int kTextReportRuleWidth = 78;
constexpr int kDurationDisplayPrecision = 1;
constexpr int kReportSizeDisplayPrecision = 1;
constexpr int kStorageDisplayPrecision = 2;
constexpr int kPercentDisplayPrecision = 1;

// Millisecond resolution (_zzz): two reports generated in the same second would
// otherwise collide and the atomic save would truncate the earlier one.
QString buildReportFilePath(const QDir& output_dir) {
    const QString filename = QString("SystemReport_%1.txt")
                                 .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz"));
    return output_dir.filePath(filename);
}

}  // namespace

GenerateSystemReportAction::GenerateSystemReportAction(const QString& output_location,
                                                       QObject* parent)
    : QuickAction(parent), m_output_location(output_location) {}

bool GenerateSystemReportAction::collectorFailed(bool timed_out,
                                                 int exit_code,
                                                 bool output_incomplete) {
    return timed_out || exit_code != 0 || output_incomplete;
}

bool GenerateSystemReportAction::reportGenerationSucceeded(bool save_ok, bool all_collectors_ok) {
    return save_ok && all_collectors_ok;
}

void GenerateSystemReportAction::scan() {
    setStatus(ActionStatus::Scanning);

    ScanResult result;
    result.applicable = true;
    result.summary = "System report will gather OS, hardware, storage, and drivers";
    result.details = "Output saved to reports folder";

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

// Fail closed on an empty or relative output location: a blank path resolves
// against the process working directory and a relative one escapes the intended
// reports folder, so the report must never be written to a caller-unintended place.
bool GenerateSystemReportAction::rejectInvalidOutputLocation() {
    if (m_output_location.trimmed().isEmpty() || !QDir::isAbsolutePath(m_output_location)) {
        ExecutionResult result;
        result.success = false;
        result.message = QStringLiteral("System report output location is invalid");
        result.log =
            QStringLiteral("Refusing to write a system report to a blank or relative path");
        finishWithResult(result, ActionStatus::Failed);
        return true;
    }
    return false;
}

// Polled between collectors so a cancel aborts promptly; settles the cancelled
// outcome (stamped with start_time) and reports whether execute() should return.
bool GenerateSystemReportAction::finishIfCancelled(const QDateTime& start_time) {
    if (isCancelled()) {
        emitCancelledResult(QStringLiteral("System report generation cancelled"), start_time);
        return true;
    }
    return false;
}

void GenerateSystemReportAction::execute() {
    if (isCancelled()) {
        emitCancelledResult(QStringLiteral("System report generation cancelled"));
        return;
    }

    if (rejectInvalidOutputLocation()) {
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    m_collector_errors.clear();
    Q_EMIT executionProgress("Gathering comprehensive system information...", progress::kStep5);

    // Phase 1: Report header
    QString report = buildReportHeader();

    // Phase 2: OS and hardware
    Q_EMIT executionProgress("Collecting OS and hardware information...", progress::kStep15);
    report += gatherOsAndHardwareInfo();
    if (finishIfCancelled(start_time)) {
        return;
    }

    // Phase 3: Storage
    Q_EMIT executionProgress("Collecting storage information...", progress::kStep40);
    report += gatherStorageInfo();
    if (finishIfCancelled(start_time)) {
        return;
    }

    // Phase 4: Network
    Q_EMIT executionProgress("Collecting network configuration...", progress::kStep60);
    report += gatherNetworkInfo();
    if (finishIfCancelled(start_time)) {
        return;
    }

    // Phase 5: Qt/Volume info
    Q_EMIT executionProgress("Adding supplemental system data...", progress::kStep80);
    report += gatherQtAndVolumeInfo();
    if (finishIfCancelled(start_time)) {
        return;
    }

    // Phase 6: Save
    Q_EMIT executionProgress("Saving report...", progress::kStep95);

    QDir output_dir(m_output_location);
    if (!output_dir.exists()) {
        if (!output_dir.mkpath(".")) {
            sak::logWarning("Failed to create system report directory: {}",
                            m_output_location.toStdString());
        }
    }

    QString filepath = buildReportFilePath(output_dir);

    report += QString("-").repeated(kTextReportRuleWidth) + "\n";
    report += QString("Report completed in %1 seconds\n")
                  .arg(start_time.msecsTo(QDateTime::currentDateTime()) / kMillisecondsPerSecondF,
                       0,
                       'f',
                       kDurationDisplayPrecision);

    saveReportAndFinish(report, filepath, start_time);
}

void GenerateSystemReportAction::saveReportAndFinish(const QString& report,
                                                     const QString& filepath,
                                                     const QDateTime& start_time) {
    const bool save_success = saveReport(report, filepath);
    const bool all_collectors_ok = m_collector_errors.isEmpty();
    const bool overall_success = reportGenerationSucceeded(save_success, all_collectors_ok);

    Q_EMIT executionProgress("Report complete", progress::kComplete);

    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.bytes_processed = report.size();
    result.success = overall_success;

    if (overall_success) {
        result.message = QString("Comprehensive system report generated: %1")
                             .arg(QFileInfo(filepath).fileName());
        result.output_path = filepath;
        result.log =
            QString("Report saved to: %1\nSize: %2 KB\nDuration: %3 seconds")
                .arg(filepath)
                .arg(report.size() / sak::kBytesPerKBf, 0, 'f', kReportSizeDisplayPrecision)
                .arg(duration_ms / kMillisecondsPerSecondF, 0, 'f', kDurationDisplayPrecision);
    } else if (!save_success) {
        result.message = "Failed to save system report";
        result.log = QString("Could not write to: %1").arg(filepath);
    } else {
        // Saved, but one or more collectors failed to run: surface it rather than
        // pass off an empty/partial report as a complete success.
        result.message = QString("System report incomplete: %1 data could not be collected")
                             .arg(m_collector_errors.join(", "));
        result.output_path = filepath;
        result.log = QString("Partial report saved to: %1\nFailed collectors: %2")
                         .arg(filepath, m_collector_errors.join(", "));
    }

    finishWithResult(result, overall_success ? ActionStatus::Success : ActionStatus::Failed);
}

// ============================================================================
// Private Helpers
// ============================================================================

QString GenerateSystemReportAction::buildReportHeader() const {
    QString header;
    header += "+" + QString("=").repeated(kTextReportRuleWidth) + "+\n";
    header +=
        "|" +
        QString(" COMPREHENSIVE SYSTEM DIAGNOSTIC REPORT").leftJustified(kTextReportRuleWidth) +
        "|\n";
    header += "+" + QString("=").repeated(kTextReportRuleWidth) + "+\n\n";
    header += QString("Generated: %1\n\n")
                  .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd "
                                                             "HH:mm:ss"));
    return header;
}

QString GenerateSystemReportAction::buildOsInfoScript() {
    // $ErrorActionPreference='Stop' promotes a collector cmdlet failure to a
    // terminating error so the process exits non-zero and collectorFailed()
    // rejects the partial output instead of saving it as a complete section.
    return "$ErrorActionPreference = 'Stop'\n"
           "$info = Get-ComputerInfo\n"
           "\n"
           "Write-Output \"=== OPERATING SYSTEM ===\"\n"
           "Write-Output \"OS Name: $($info.OsName)\"\n"
           "Write-Output \"OS Version: $($info.OsVersion)\"\n"
           "Write-Output \"OS Build: $($info.OsBuildNumber)\"\n"
           "Write-Output \"OS Architecture: $($info.OsArchitecture)\"\n"
           "Write-Output \"OS Install Date: $($info.OsInstallDate)\"\n"
           "Write-Output \"OS Last Boot Time: $($info.OsLastBootUpTime)\"\n"
           "Write-Output \"OS Uptime: $($info.OsUptime)\"\n"
           "Write-Output \"Windows Directory: $($info.WindowsDirectory)\"\n"
           "Write-Output \"System Drive: $($info.WindowsSystemRoot)\"\n"
           "Write-Output \"\"\n"
           "\n"
           "Write-Output \"=== COMPUTER SYSTEM ===\"\n"
           "Write-Output \"Computer Name: $($info.CsName)\"\n"
           "Write-Output \"Domain: $($info.CsDomain)\"\n"
           "Write-Output \"Workgroup: $($info.CsWorkgroup)\"\n"
           "Write-Output \"Part of Domain: $($info.CsPartOfDomain)\"\n"
           "Write-Output \"System Type: $($info.CsSystemType)\"\n"
           "Write-Output \"PC System Type: $($info.CsPCSystemType)\"\n"
           "Write-Output \"Manufacturer: $($info.CsManufacturer)\"\n"
           "Write-Output \"Model: $($info.CsModel)\"\n"
           "Write-Output \"System Family: $($info.CsSystemFamily)\"\n"
           "Write-Output \"System SKU: $($info.CsSystemSKUNumber)\"\n"
           "Write-Output \"\"\n"
           "\n"
           "Write-Output \"=== PROCESSOR ===\"\n"
           "Write-Output \"Number of Processors: $($info.CsNumberOfProcessors)\"\n"
           "Write-Output \"Number of Logical Processors: $($info.CsNumberOfLogicalProcessors)\"\n"
           "Write-Output \"Processor Name: $($info.CsProcessors[0].Name)\"\n"
           "Write-Output \"Processor Description: $($info.CsProcessors[0].Description)\"\n"
           "Write-Output \"Max Clock Speed: $($info.CsProcessors[0].MaxClockSpeed) MHz\"\n"
           "Write-Output \"Current Clock Speed: $($info.CsProcessors[0].CurrentClockSpeed) MHz\"\n"
           "Write-Output \"Address Width: $($info.CsProcessors[0].AddressWidth) bit\"\n"
           "Write-Output \"\"\n";
}

QString GenerateSystemReportAction::buildHardwareInfoScript() {
    return "\n"
           "Write-Output \"=== MEMORY ===\"\n"
           "Write-Output \"Total Physical Memory: "
           "$([math]::Round($info.CsTotalPhysicalMemory / 1GB, 2)) GB\"\n"
           "Write-Output \"Free Physical Memory: "
           "$([math]::Round($info.OsFreePhysicalMemory / 1MB, 2)) MB\"\n"
           "Write-Output \"Total Virtual Memory: "
           "$([math]::Round($info.OsTotalVirtualMemorySize / 1MB, 2)) MB\"\n"
           "Write-Output \"Free Virtual Memory: $([math]::Round($info.OsFreeVirtualMemory / 1MB, "
           "2)) "
           "MB\"\n"
           "Write-Output \"Page File Size: $([math]::Round($info.OsSizeStoredInPagingFiles / 1MB, "
           "2)) "
           "MB\"\n"
           "Write-Output \"\"\n"
           "\n"
           "Write-Output \"=== BIOS ===\"\n"
           "Write-Output \"BIOS Version: $($info.BiosVersion)\"\n"
           "Write-Output \"BIOS Manufacturer: $($info.BiosManufacturer)\"\n"
           "Write-Output \"BIOS Release Date: $($info.BiosReleaseDate)\"\n"
           // NB: "BiosSeralNumber" is intentionally misspelled -- Get-ComputerInfo
           // itself exposes the property with the missing 'i' (a documented
           // Microsoft typo). Correcting it to BiosSerialNumber returns $null.
           "Write-Output \"BIOS Serial Number: $($info.BiosSeralNumber)\"\n"
           "Write-Output \"BIOS UEFI: $($info.BiosFirmwareType)\"\n"
           "Write-Output \"\"\n"
           "\n"
           "Write-Output \"=== TIME ZONE & LOCALE ===\"\n"
           "Write-Output \"Time Zone: $($info.TimeZone)\"\n"
           "Write-Output \"Locale: $($info.OsLocale)\"\n"
           "Write-Output \"UI Language: $($info.OsMuiLanguages -join ', ')\"\n"
           "Write-Output \"Keyboard Layout: $($info.KeyboardLayout)\"\n"
           "Write-Output \"\"\n"
           "\n"
           "Write-Output \"=== NETWORK ===\"\n"
           "Write-Output \"Network Adapters: $($info.CsNetworkAdapters.Count)\"\n"
           "Write-Output \"DNS Host Name: $($info.CsDNSHostName)\"\n"
           "Write-Output \"Primary Owner Name: $($info.CsPrimaryOwnerName)\"\n"
           "Write-Output \"\"\n"
           "\n"
           "Write-Output \"=== WINDOWS ACTIVATION ===\"\n"
           "Write-Output \"Product Name: $($info.WindowsProductName)\"\n"
           "Write-Output \"Product ID: $($info.WindowsProductId)\"\n"
           "Write-Output \"Edition ID: $($info.WindowsEditionId)\"\n"
           "Write-Output \"Registered Owner: $($info.WindowsRegisteredOwner)\"\n"
           "Write-Output \"Registered Organization: $($info.WindowsRegisteredOrganization)\"";
}

QString GenerateSystemReportAction::gatherOsAndHardwareInfo() {
    QString ps_cmd_info = buildOsInfoScript() + buildHardwareInfoScript();

    ProcessResult proc_info = runPowerShell(ps_cmd_info, sak::kTimeoutChocoListMs);
    if (!proc_info.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("System report OS warning: " + proc_info.std_err.trimmed());
    }

    const bool info_incomplete = proc_info.output_truncated ||
                                 proc_info.std_out.trimmed().isEmpty();
    if (collectorFailed(proc_info.timed_out, proc_info.exit_code, info_incomplete)) {
        m_collector_errors << QStringLiteral("OS/hardware");
        return "=== OPERATING SYSTEM ===\n[collection failed: OS/hardware data unavailable]\n\n";
    }
    return proc_info.std_out + "\n";
}

QString GenerateSystemReportAction::gatherStorageInfo() {
    QString ps_cmd_storage =
        "$ErrorActionPreference = 'Stop'\n"
        "Write-Output \"=== STORAGE DEVICES ===\"\n"
        "$disks = Get-PhysicalDisk\n"
        "foreach ($disk in $disks) {\n"
        "    Write-Output \"\"\n"
        "    Write-Output \"Physical Disk $($disk.DeviceID):\"\n"
        "    Write-Output \"  Friendly Name: $($disk.FriendlyName)\"\n"
        "    Write-Output \"  Model: $($disk.Model)\"\n"
        "    Write-Output \"  Media Type: $($disk.MediaType)\"\n"
        "    Write-Output \"  Bus Type: $($disk.BusType)\"\n"
        "    Write-Output \"  Size: $([math]::Round($disk.Size / 1GB, 2)) GB\"\n"
        "    Write-Output \"  Health Status: $($disk.HealthStatus)\"\n"
        "    Write-Output \"  Operational Status: $($disk.OperationalStatus)\"\n"
        "    \n"
        "    try {\n"
        "        $smart = $disk | Get-StorageReliabilityCounter -ErrorAction SilentlyContinue\n"
        "        if ($smart) {\n"
        "            Write-Output \"  Temperature: $($smart.Temperature) degC\"\n"
        "            Write-Output \"  Power On Hours: $($smart.PowerOnHours)\"\n"
        "            Write-Output \"  Wear: $($smart.Wear)%\"\n"
        "        }\n"
        "    } catch {}\n"
        "}\n"
        "Write-Output \"\"";

    ProcessResult proc_storage = runPowerShell(ps_cmd_storage, sak::kTimeoutProcessMediumMs);
    if (!proc_storage.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("System report storage warning: " + proc_storage.std_err.trimmed());
    }

    const bool storage_incomplete = proc_storage.output_truncated ||
                                    proc_storage.std_out.trimmed().isEmpty();
    if (collectorFailed(proc_storage.timed_out, proc_storage.exit_code, storage_incomplete)) {
        m_collector_errors << QStringLiteral("storage");
        return "=== STORAGE DEVICES ===\n[collection failed: storage data unavailable]\n\n";
    }
    return proc_storage.std_out + "\n";
}

QString GenerateSystemReportAction::gatherNetworkInfo() {
    QString ps_cmd_network =
        "$ErrorActionPreference = 'Stop'\n"
        "Write-Output \"=== NETWORK ADAPTERS ===\"\n"
        "$adapters = Get-NetAdapter | Where-Object {$_.Status -eq 'Up'}\n"
        "foreach ($adapter in $adapters) {\n"
        "    Write-Output \"\"\n"
        "    Write-Output \"$($adapter.Name):\"\n"
        "    Write-Output \"  Interface Description: $($adapter.InterfaceDescription)\"\n"
        "    Write-Output \"  MAC Address: $($adapter.MacAddress)\"\n"
        "    Write-Output \"  Link Speed: $($adapter.LinkSpeed)\"\n"
        "    Write-Output \"  Status: $($adapter.Status)\"\n"
        "    \n"
        "    $ipconfig = Get-NetIPAddress -InterfaceIndex $adapter.ifIndex -ErrorAction "
        "SilentlyContinue\n"
        "    foreach ($ip in $ipconfig) {\n"
        "        if ($ip.AddressFamily -eq 'IPv4') {\n"
        "            Write-Output \"  IPv4 Address: $($ip.IPAddress)\"\n"
        "            Write-Output \"  Subnet Prefix: $($ip.PrefixLength)\"\n"
        "        }\n"
        "    }\n"
        "}\n"
        "Write-Output \"\"";

    ProcessResult proc_network = runPowerShell(ps_cmd_network, sak::kTimeoutProcessMediumMs);
    if (!proc_network.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("System report network warning: " + proc_network.std_err.trimmed());
    }

    const bool network_incomplete = proc_network.output_truncated ||
                                    proc_network.std_out.trimmed().isEmpty();
    if (collectorFailed(proc_network.timed_out, proc_network.exit_code, network_incomplete)) {
        m_collector_errors << QStringLiteral("network");
        return "=== NETWORK ADAPTERS ===\n[collection failed: network data unavailable]\n\n";
    }
    return proc_network.std_out + "\n";
}

QString GenerateSystemReportAction::gatherQtAndVolumeInfo() const {
    QString section;

    section += "=== QT SYSTEM INFORMATION ===\n\n";
    section += QString("Machine Host Name: %1\n").arg(QSysInfo::machineHostName());
    section += QString("Pretty Product Name: %1\n").arg(QSysInfo::prettyProductName());
    section += QString("Kernel Type: %1\n").arg(QSysInfo::kernelType());
    section += QString("Kernel Version: %1\n").arg(QSysInfo::kernelVersion());
    section += QString("CPU Architecture: %1\n").arg(QSysInfo::currentCpuArchitecture());
    section += QString("Build CPU Architecture: %1\n").arg(QSysInfo::buildCpuArchitecture());
    section += QString("Build ABI: %1\n\n").arg(QSysInfo::buildAbi());

    section += "=== VOLUME INFORMATION ===\n\n";
    for (const QStorageInfo& storage : QStorageInfo::mountedVolumes()) {
        if (!storage.isValid() || !storage.isReady()) {
            continue;
        }

        section += QString("Volume: %1\n").arg(storage.rootPath());
        section += QString("  Name: %1\n").arg(storage.name());
        section += QString("  File System: %1\n").arg(QString::fromUtf8(storage.fileSystemType()));
        section += QString("  Device: %1\n").arg(QString::fromUtf8(storage.device()));
        section +=
            QString("  Total: %1 GB\n")
                .arg(storage.bytesTotal() / sak::kBytesPerGBf, 0, 'f', kStorageDisplayPrecision);
        section +=
            QString("  Free: %1 GB\n")
                .arg(storage.bytesFree() / sak::kBytesPerGBf, 0, 'f', kStorageDisplayPrecision);
        section += QString("  Available: %1 GB\n")
                       .arg(storage.bytesAvailable() / sak::kBytesPerGBf,
                            0,
                            'f',
                            kStorageDisplayPrecision);
        const qint64 totalBytes = storage.bytesTotal();
        const double usedPercent =
            totalBytes > 0
                ? kPercentMaxF * (1.0 - static_cast<double>(storage.bytesFree()) /
                                            static_cast<double>(totalBytes))
                : 0.0;  // guard divide-by-zero: an unknown-capacity volume reports 0% used
        section += QString("  Used: %1%%\n\n").arg(usedPercent, 0, 'f', kPercentDisplayPrecision);
    }

    return section;
}

bool GenerateSystemReportAction::saveReport(const QString& report, const QString& filepath) {
    // QSaveFile: the report is written to a temp file and atomically renamed on
    // commit(), so a crash mid-write never leaves a truncated report in place,
    // and commit() surfaces a close/flush error instead of it being ignored.
    QSaveFile file(filepath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    const QByteArray data = report.toUtf8();
    if (file.write(data) != data.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

}  // namespace sak

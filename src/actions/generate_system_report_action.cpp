// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file generate_system_report_action.cpp
/// @brief Implements system information report generation

#include "sak/actions/generate_system_report_action.h"
#include "sak/process_runner.h"
#include <QDir>
#include <QDateTime>
#include <QFileInfo>
#include <QSysInfo>
#include <QStorageInfo>

namespace sak {

GenerateSystemReportAction::GenerateSystemReportAction(const QString& output_location, QObject* parent)
    : QuickAction(parent)
    , m_output_location(output_location)
{
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

void GenerateSystemReportAction::execute() {
    if (isCancelled()) {
        emitCancelledResult(QStringLiteral("System report generation cancelled"));
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    Q_EMIT executionProgress("Gathering comprehensive system information...", 5);

    // Phase 1: Report header
    QString report = buildReportHeader();
    
    // Phase 2: OS and hardware
    Q_EMIT executionProgress("Collecting OS and hardware information...", 15);
    report += gatherOsAndHardwareInfo();
    if (isCancelled()) { emitCancelledResult(QStringLiteral("System report generation cancelled"), start_time); return; }
    
    // Phase 3: Storage
    Q_EMIT executionProgress("Collecting storage information...", 40);
    report += gatherStorageInfo();
    if (isCancelled()) { emitCancelledResult(QStringLiteral("System report generation cancelled"), start_time); return; }
    
    // Phase 4: Network
    Q_EMIT executionProgress("Collecting network configuration...", 60);
    report += gatherNetworkInfo();
    if (isCancelled()) { emitCancelledResult(QStringLiteral("System report generation cancelled"), start_time); return; }
    
    // Phase 5: Qt/Volume info
    Q_EMIT executionProgress("Adding supplemental system data...", 80);
    report += gatherQtAndVolumeInfo();
    if (isCancelled()) { emitCancelledResult(QStringLiteral("System report generation cancelled"), start_time); return; }
    
    // Phase 6: Save
    Q_EMIT executionProgress("Saving report...", 95);
    
    QDir output_dir(m_output_location);
    if (!output_dir.exists()) {
        output_dir.mkpath(".");
    }
    
    QString filename = QString("SystemReport_%1.txt")
        .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    QString filepath = output_dir.filePath(filename);
    
    report += QString("─").repeated(78) + "\n";
    report += QString("Report completed in %1 seconds\n")
        .arg(start_time.msecsTo(QDateTime::currentDateTime()) / 1000.0, 0, 'f', 1);
    
    saveReportAndFinish(report, filepath, start_time);
}

void GenerateSystemReportAction::saveReportAndFinish(
        const QString& report, const QString& filepath, const QDateTime& start_time)
{
    bool save_success = saveReport(report, filepath);
    
    Q_EMIT executionProgress("Report complete", 100);
    
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    
    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.bytes_processed = report.size();
    
    if (save_success) {
        result.success = true;
        result.message = QString("Comprehensive system report generated: %1")
            .arg(QFileInfo(filepath).fileName());
        result.output_path = filepath;
        result.log = QString("Report saved to: %1\nSize: %2 KB\nDuration: %3 seconds")
            .arg(filepath)
            .arg(report.size() / 1024.0, 0, 'f', 1)
            .arg(duration_ms / 1000.0, 0, 'f', 1);
    } else {
        result.success = false;
        result.message = "Failed to save system report";
        result.log = QString("Could not write to: %1").arg(filepath);
    }
    
    finishWithResult(result, save_success ? ActionStatus::Success : ActionStatus::Failed);
}

// ============================================================================
// Private Helpers
// ============================================================================

QString GenerateSystemReportAction::buildReportHeader() const
{
    QString header;
    header += "╔" + QString("═").repeated(78) + "╗\n";
    header += "║" + QString(" COMPREHENSIVE SYSTEM DIAGNOSTIC REPORT").leftJustified(78) + "║\n";
    header += "╚" + QString("═").repeated(78) + "╝\n\n";
    header += QString("Generated: %1\n\n").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
    return header;
}

QString GenerateSystemReportAction::buildOsInfoScript() const
{
    return
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

QString GenerateSystemReportAction::buildHardwareInfoScript() const
{
    return
        "\n"
        "Write-Output \"=== MEMORY ===\"\n"
        "Write-Output \"Total Physical Memory: $([math]::Round($info.CsTotalPhysicalMemory / 1GB, 2)) GB\"\n"
        "Write-Output \"Free Physical Memory: $([math]::Round($info.OsFreePhysicalMemory / 1MB, 2)) MB\"\n"
        "Write-Output \"Total Virtual Memory: $([math]::Round($info.OsTotalVirtualMemorySize / 1MB, 2)) MB\"\n"
        "Write-Output \"Free Virtual Memory: $([math]::Round($info.OsFreeVirtualMemory / 1MB, 2)) MB\"\n"
        "Write-Output \"Page File Size: $([math]::Round($info.OsSizeStoredInPagingFiles / 1MB, 2)) MB\"\n"
        "Write-Output \"\"\n"
        "\n"
        "Write-Output \"=== BIOS ===\"\n"
        "Write-Output \"BIOS Version: $($info.BiosVersion)\"\n"
        "Write-Output \"BIOS Manufacturer: $($info.BiosManufacturer)\"\n"
        "Write-Output \"BIOS Release Date: $($info.BiosReleaseDate)\"\n"
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

QString GenerateSystemReportAction::gatherOsAndHardwareInfo()
{
    QString ps_cmd_info = buildOsInfoScript() + buildHardwareInfoScript();
    
    ProcessResult proc_info = runPowerShell(ps_cmd_info, 15000);
    if (!proc_info.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("System report OS warning: " + proc_info.std_err.trimmed());
    }
    
    if (!proc_info.timed_out) {
        return proc_info.std_out + "\n";
    }
    return "=== OPERATING SYSTEM ===\nTimeout gathering system info\n\n";
}

QString GenerateSystemReportAction::gatherStorageInfo()
{
    QString ps_cmd_storage = 
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
        "            Write-Output \"  Temperature: $($smart.Temperature)°C\"\n"
        "            Write-Output \"  Power On Hours: $($smart.PowerOnHours)\"\n"
        "            Write-Output \"  Wear: $($smart.Wear)%\"\n"
        "        }\n"
        "    } catch {}\n"
        "}\n"
        "Write-Output \"\"";
    
    ProcessResult proc_storage = runPowerShell(ps_cmd_storage, 10000);
    if (!proc_storage.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("System report storage warning: " + proc_storage.std_err.trimmed());
    }
    
    if (!proc_storage.timed_out) {
        return proc_storage.std_out + "\n";
    }
    return QString();
}

QString GenerateSystemReportAction::gatherNetworkInfo()
{
    QString ps_cmd_network = 
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
        "    $ipconfig = Get-NetIPAddress -InterfaceIndex $adapter.ifIndex -ErrorAction SilentlyContinue\n"
        "    foreach ($ip in $ipconfig) {\n"
        "        if ($ip.AddressFamily -eq 'IPv4') {\n"
        "            Write-Output \"  IPv4 Address: $($ip.IPAddress)\"\n"
        "            Write-Output \"  Subnet Prefix: $($ip.PrefixLength)\"\n"
        "        }\n"
        "    }\n"
        "}\n"
        "Write-Output \"\"";
    
    ProcessResult proc_network = runPowerShell(ps_cmd_network, 10000);
    if (!proc_network.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("System report network warning: " + proc_network.std_err.trimmed());
    }
    
    if (!proc_network.timed_out) {
        return proc_network.std_out + "\n";
    }
    return QString();
}

QString GenerateSystemReportAction::gatherQtAndVolumeInfo() const
{
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
        if (!storage.isValid() || !storage.isReady()) continue;
        
        section += QString("Volume: %1\n").arg(storage.rootPath());
        section += QString("  Name: %1\n").arg(storage.name());
        section += QString("  File System: %1\n").arg(QString::fromUtf8(storage.fileSystemType()));
        section += QString("  Device: %1\n").arg(QString::fromUtf8(storage.device()));
        section += QString("  Total: %1 GB\n").arg(storage.bytesTotal() / (1024.0 * 1024 * 1024), 0, 'f', 2);
        section += QString("  Free: %1 GB\n").arg(storage.bytesFree() / (1024.0 * 1024 * 1024), 0, 'f', 2);
        section += QString("  Available: %1 GB\n").arg(storage.bytesAvailable() / (1024.0 * 1024 * 1024), 0, 'f', 2);
        section += QString("  Used: %1%%\n\n").arg(100.0 * (1.0 - static_cast<double>(storage.bytesFree()) / storage.bytesTotal()), 0, 'f', 1);
    }
    
    return section;
}

bool GenerateSystemReportAction::saveReport(const QString& report, const QString& filepath)
{
    QFile file(filepath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    file.write(report.toUtf8());
    file.close();
    return true;
}

} // namespace sak

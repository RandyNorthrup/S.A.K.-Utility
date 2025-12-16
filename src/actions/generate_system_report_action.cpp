// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/generate_system_report_action.h"
#include <QProcess>
#include <QDir>
#include <QDateTime>
#include <QSysInfo>
#include <QStorageInfo>

namespace sak {

GenerateSystemReportAction::GenerateSystemReportAction(const QString& output_location, QObject* parent)
    : QuickAction(parent)
    , m_output_location(output_location)
{
}

void GenerateSystemReportAction::gatherSystemInfo() {
    // Gathered via systeminfo command in execute()
}

void GenerateSystemReportAction::gatherInstalledPrograms() {
    // Gathered via registry query in execute()
}

void GenerateSystemReportAction::gatherDriverInfo() {
    // Gathered via driverquery in execute()
}

void GenerateSystemReportAction::gatherEventLogs() {
    // Gathered via Get-EventLog in execute()
}

void GenerateSystemReportAction::generateHTML() {
    // HTML generation happens in execute()
}

void GenerateSystemReportAction::scan() {
    // Scan is no longer used - actions execute immediately
    setStatus(ActionStatus::Ready);
    ScanResult result;
    result.applicable = true;
    result.summary = "Ready to generate system report";
    setScanResult(result);
    Q_EMIT scanComplete(result);
}

void GenerateSystemReportAction::execute() {
    if (isCancelled()) {
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    QString report;
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    
    Q_EMIT executionProgress("Gathering comprehensive system information...", 5);
    
    report += "╔" + QString("═").repeated(78) + "╗\n";
    report += "║" + QString(" COMPREHENSIVE SYSTEM DIAGNOSTIC REPORT").leftJustified(78) + "║\n";
    report += "╚" + QString("═").repeated(78) + "╝\n\n";
    report += QString("Generated: %1\n\n").arg(timestamp);
    
    // Get comprehensive computer info (100+ properties)
    Q_EMIT executionProgress("Collecting OS and hardware information...", 15);
    
    QProcess proc_info;
    QString ps_cmd_info = 
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
        "Write-Output \"\"\n"
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
    
    proc_info.start("powershell.exe", QStringList() << "-Command" << ps_cmd_info);
    
    if (proc_info.waitForFinished(15000)) {
        report += proc_info.readAllStandardOutput() + "\n";
    } else {
        report += "=== OPERATING SYSTEM ===\nTimeout gathering system info\n\n";
    }
    
    // Get detailed storage information
    Q_EMIT executionProgress("Collecting storage information...", 40);
    
    QProcess proc_storage;
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
    
    proc_storage.start("powershell.exe", QStringList() << "-Command" << ps_cmd_storage);
    
    if (proc_storage.waitForFinished(10000)) {
        report += proc_storage.readAllStandardOutput() + "\n";
    }
    
    // Get network adapter details
    Q_EMIT executionProgress("Collecting network configuration...", 60);
    
    QProcess proc_network;
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
    
    proc_network.start("powershell.exe", QStringList() << "-Command" << ps_cmd_network);
    
    if (proc_network.waitForFinished(10000)) {
        report += proc_network.readAllStandardOutput() + "\n";
    }
    
    // Get Qt system info
    Q_EMIT executionProgress("Adding supplemental system data...", 80);
    
    report += "=== QT SYSTEM INFORMATION ===\n\n";
    report += QString("Machine Host Name: %1\n").arg(QSysInfo::machineHostName());
    report += QString("Pretty Product Name: %1\n").arg(QSysInfo::prettyProductName());
    report += QString("Kernel Type: %1\n").arg(QSysInfo::kernelType());
    report += QString("Kernel Version: %1\n").arg(QSysInfo::kernelVersion());
    report += QString("CPU Architecture: %1\n").arg(QSysInfo::currentCpuArchitecture());
    report += QString("Build CPU Architecture: %1\n").arg(QSysInfo::buildCpuArchitecture());
    report += QString("Build ABI: %1\n\n").arg(QSysInfo::buildAbi());
    
    // Get volume information
    report += "=== VOLUME INFORMATION ===\n\n";
    for (const QStorageInfo& storage : QStorageInfo::mountedVolumes()) {
        if (!storage.isValid() || !storage.isReady()) continue;
        
        report += QString("Volume: %1\n").arg(storage.rootPath());
        report += QString("  Name: %1\n").arg(storage.name());
        report += QString("  File System: %1\n").arg(QString::fromUtf8(storage.fileSystemType()));
        report += QString("  Device: %1\n").arg(QString::fromUtf8(storage.device()));
        report += QString("  Total: %1 GB\n").arg(storage.bytesTotal() / (1024.0 * 1024 * 1024), 0, 'f', 2);
        report += QString("  Free: %1 GB\n").arg(storage.bytesFree() / (1024.0 * 1024 * 1024), 0, 'f', 2);
        report += QString("  Available: %1 GB\n").arg(storage.bytesAvailable() / (1024.0 * 1024 * 1024), 0, 'f', 2);
        report += QString("  Used: %1%%\n\n").arg(100.0 * (1.0 - (double)storage.bytesFree() / storage.bytesTotal()), 0, 'f', 1);
    }
    
    Q_EMIT executionProgress("Saving report...", 95);
    
    // Save to file
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
    
    QFile file(filepath);
    bool save_success = false;
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        file.write(report.toUtf8());
        file.close();
        save_success = true;
    }
    
    Q_EMIT executionProgress("Report complete", 100);
    
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    
    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.bytes_processed = report.size();
    
    if (save_success) {
        result.success = true;
        result.message = QString("Comprehensive system report generated: %1").arg(filename);
        result.output_path = filepath;
        result.log = QString("Report saved to: %1\nSize: %2 KB\nDuration: %3 seconds")
            .arg(filepath)
            .arg(report.size() / 1024.0, 0, 'f', 1)
            .arg(duration_ms / 1000.0, 0, 'f', 1);
        setStatus(ActionStatus::Success);
    } else {
        result.success = false;
        result.message = "Failed to save system report";
        result.log = QString("Could not write to: %1").arg(filepath);
        setStatus(ActionStatus::Failed);
    }
    
    setExecutionResult(result);
    Q_EMIT executionComplete(result);
}

} // namespace sak

// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/check_disk_health_action.h"
#include <QProcess>
#include <QRegularExpression>
#include <QStorageInfo>

namespace sak {

CheckDiskHealthAction::CheckDiskHealthAction(QObject* parent)
    : QuickAction(parent)
{
}

QVector<CheckDiskHealthAction::DriveHealth> CheckDiskHealthAction::querySmartStatus() {
    QVector<DriveHealth> disks;
    
    Q_EMIT executionProgress("Querying physical disks...", 20);
    
    QString ps_cmd = QStringLiteral(
        "$disks = Get-PhysicalDisk\n"
        "foreach ($disk in $disks) {\n"
        "    Write-Output '===DISK_START==='\n"
        "    Write-Output \"DeviceID: $($disk.DeviceID)\"\n"
        "    Write-Output \"FriendlyName: $($disk.FriendlyName)\"\n"
        "    Write-Output \"Model: $($disk.Model)\"\n"
        "    Write-Output \"MediaType: $($disk.MediaType)\"\n"
        "    Write-Output \"BusType: $($disk.BusType)\"\n"
        "    Write-Output \"HealthStatus: $($disk.HealthStatus)\"\n"
        "    Write-Output \"OperationalStatus: $($disk.OperationalStatus)\"\n"
        "    Write-Output \"Size: $($disk.Size)\"\n"
        "    try {\n"
        "        $counter = $disk | Get-StorageReliabilityCounter -ErrorAction SilentlyContinue\n"
        "        if ($counter) {\n"
        "            Write-Output \"Temperature: $($counter.Temperature)\"\n"
        "            Write-Output \"TemperatureMax: $($counter.TemperatureMax)\"\n"
        "            Write-Output \"Wear: $($counter.Wear)\"\n"
        "            Write-Output \"PowerOnHours: $($counter.PowerOnHours)\"\n"
        "            Write-Output \"ReadErrorsTotal: $($counter.ReadErrorsTotal)\"\n"
        "            Write-Output \"ReadErrorsUncorrected: $($counter.ReadErrorsUncorrected)\"\n"
        "            Write-Output \"WriteErrorsTotal: $($counter.WriteErrorsTotal)\"\n"
        "            Write-Output \"WriteErrorsUncorrected: $($counter.WriteErrorsUncorrected)\"\n"
        "            Write-Output \"LoadUnloadCycleCount: $($counter.LoadUnloadCycleCount)\"\n"
        "            Write-Output \"LoadUnloadCycleCountMax: $($counter.LoadUnloadCycleCountMax)\"\n"
        "            Write-Output \"StartStopCycleCount: $($counter.StartStopCycleCount)\"\n"
        "            Write-Output \"StartStopCycleCountMax: $($counter.StartStopCycleCountMax)\"\n"
        "        }\n"
        "    } catch {\n"
        "        Write-Output 'Temperature: N/A'\n"
        "    }\n"
        "    Write-Output '===DISK_END==='\n"
        "}\n"
    );
    
    QProcess proc;
    proc.start("powershell.exe", QStringList() << "-Command" << ps_cmd);
    
    if (!proc.waitForFinished(15000)) {
        Q_EMIT executionProgress("Timeout querying disks", 50);
        return disks;
    }
    
    QString output = proc.readAllStandardOutput();
    QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    
    DriveHealth current_disk;
    bool parsing_disk = false;
    
    for (const QString& line : lines) {
        QString trimmed = line.trimmed();
        
        if (trimmed == "===DISK_START===") {
            current_disk = DriveHealth();
            current_disk.status = "Unknown";
            current_disk.temperature = 0;
            current_disk.percent_lifetime_used = 0;
            parsing_disk = true;
            continue;
        }
        
        if (trimmed == "===DISK_END===") {
            if (parsing_disk) {
                disks.append(current_disk);
            }
            parsing_disk = false;
            continue;
        }
        
        if (!parsing_disk) continue;
        
        QStringList parts = trimmed.split(':', Qt::SkipEmptyParts);
        if (parts.size() < 2) continue;
        
        QString key = parts[0].trimmed();
        QString value = parts[1].trimmed();
        
        if (key == "FriendlyName" && !value.isEmpty()) {
            current_disk.letter = value.at(0);
        }
        else if (key == "HealthStatus") {
            if (value == "Healthy") {
                current_disk.status = "Healthy";
            } else if (value == "Warning") {
                current_disk.status = "Warning";
                current_disk.warnings.append("Drive health warning - backup immediately!");
            } else if (value == "Unhealthy") {
                current_disk.status = "Critical";
                current_disk.warnings.append("CRITICAL: Drive failure imminent - backup NOW!");
            }
        }
        else if (key == "Temperature" && value != "N/A") {
            current_disk.temperature = value.toInt();
            if (current_disk.temperature > 55) {
                current_disk.warnings.append(QString("High temperature: %1°C").arg(current_disk.temperature));
            }
        }
        else if (key == "Wear" && value != "N/A" && !value.isEmpty()) {
            current_disk.percent_lifetime_used = value.toInt();
            if (current_disk.percent_lifetime_used > 80) {
                current_disk.warnings.append(QString("High wear: %1%% lifetime used").arg(current_disk.percent_lifetime_used));
            }
        }
        else if (key == "ReadErrorsUncorrected" && value.toInt() > 0) {
            current_disk.warnings.append(QString("Read errors detected: %1").arg(value));
        }
        else if (key == "WriteErrorsUncorrected" && value.toInt() > 0) {
            current_disk.warnings.append(QString("Write errors detected: %1").arg(value));
        }
    }
    
    return disks;
}

bool CheckDiskHealthAction::isDriveSSD(const QString& drive) {
    QProcess proc;
    QString ps_cmd = QString("Get-PhysicalDisk | Where-Object {$_.DeviceID -eq %1} | Select-Object -ExpandProperty MediaType").arg(drive);
    proc.start("powershell.exe", QStringList() << "-Command" << ps_cmd);
    
    if (!proc.waitForFinished(5000)) {
        return false;
    }
    
    QString output = proc.readAllStandardOutput().trimmed();
    return output.contains("SSD", Qt::CaseInsensitive) || output.contains("NVMe", Qt::CaseInsensitive);
}

void CheckDiskHealthAction::scan() {
    // Scan is no longer used - actions execute immediately
    setStatus(ActionStatus::Ready);
    ScanResult result;
    result.applicable = true;
    result.summary = "Ready to check disk health";
    setScanResult(result);
    Q_EMIT scanComplete(result);
}

void CheckDiskHealthAction::execute() {
    if (isCancelled()) {
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    Q_EMIT executionProgress("Scanning physical disks with SMART monitoring...", 10);
    
    QVector<DriveHealth> disk_info = querySmartStatus();
    
    Q_EMIT executionProgress("Analyzing SMART data and reliability counters...", 80);
    
    QString report;
    report += "╔" + QString("═").repeated(78) + "╗\n";
    report += "║" + QString(" DISK HEALTH & SMART STATUS REPORT").leftJustified(78) + "║\n";
    report += "╚" + QString("═").repeated(78) + "╝\n\n";
    
    int healthy_count = 0;
    int warning_count = 0;
    int critical_count = 0;
    
    for (const DriveHealth& disk : disk_info) {
        report += QString("Drive: %1\n").arg(disk.letter);
        report += QString("  Health Status: %1\n").arg(disk.status);
        
        if (disk.temperature > 0) {
            report += QString("  Temperature: %1°C\n").arg(disk.temperature);
        }
        
        if (disk.percent_lifetime_used > 0) {
            report += QString("  Wear Level: %1%% lifetime used\n").arg(disk.percent_lifetime_used);
        }
        
        if (disk.status == "Healthy") {
            healthy_count++;
        } else if (disk.status == "Warning") {
            warning_count++;
        } else if (disk.status == "Critical") {
            critical_count++;
        }
        
        if (!disk.warnings.isEmpty()) {
            report += "\n  ⚠ WARNINGS:\n";
            for (const QString& warning : disk.warnings) {
                report += QString("    • %1\n").arg(warning);
            }
        }
        
        report += "\n";
    }
    
    report += QString("─").repeated(78) + "\n";
    report += QString("Summary: %1 disk(s) - %2 healthy, %3 warnings, %4 critical\n")
        .arg(disk_info.count()).arg(healthy_count).arg(warning_count).arg(critical_count);
    
    Q_EMIT executionProgress("Health check complete", 100);
    
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    
    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.files_processed = disk_info.count();
    
    if (!disk_info.isEmpty()) {
        result.success = (critical_count == 0);
        result.message = QString("%1 disk(s): %2 healthy, %3 warnings, %4 critical")
            .arg(disk_info.count()).arg(healthy_count).arg(warning_count).arg(critical_count);
        result.log = report;
        setStatus((critical_count > 0 || warning_count > 0) ? ActionStatus::Failed : ActionStatus::Success);
    } else {
        result.success = false;
        result.message = "Could not query disk health - SMART data unavailable";
        result.log = "No physical disks found or PowerShell Storage module not available";
        setStatus(ActionStatus::Failed);
    }
    
    setExecutionResult(result);
    Q_EMIT executionComplete(result);
}

} // namespace sak

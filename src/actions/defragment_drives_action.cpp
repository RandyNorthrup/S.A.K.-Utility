// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/defragment_drives_action.h"
#include "sak/process_runner.h"
#include <QProcess>
#include <QStorageInfo>
#include <QRegularExpression>

namespace sak {

DefragmentDrivesAction::DefragmentDrivesAction(QObject* parent)
    : QuickAction(parent)
{
}

bool DefragmentDrivesAction::isDriveSSD(const QString& drive_letter) {
    QString cmd = QString("Get-PhysicalDisk | Where-Object {$_.DeviceID -eq (Get-Partition -DriveLetter %1).DiskNumber} | Select-Object -ExpandProperty MediaType")
                     .arg(drive_letter.left(1));
    ProcessResult proc = runPowerShell(cmd, 5000);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Drive media type warning: " + proc.std_err.trimmed());
    }
    QString output = proc.std_out.trimmed();
    return output.contains("SSD", Qt::CaseInsensitive);
}

int DefragmentDrivesAction::analyzeFragmentation(const QString& drive_letter) {
    QString cmd = QString("defrag %1: /A").arg(drive_letter);
    ProcessResult proc = runProcess("cmd.exe", QStringList() << "/c" << cmd, 30000);
    QString output = proc.std_out;
    
    // Parse fragmentation percentage from output
    QRegularExpression re("(\\d+)%.*fragmented");
    QRegularExpressionMatch match = re.match(output);
    
    if (match.hasMatch()) {
        return match.captured(1).toInt();
    }
    return 0;
}

void DefragmentDrivesAction::scan() {
    setStatus(ActionStatus::Scanning);

    Q_EMIT scanProgress("Enumerating fixed drives...");

    int fixed_drives = 0;
    for (const QStorageInfo& storage : QStorageInfo::mountedVolumes()) {
        if (storage.isValid() && storage.isReady() && !storage.isReadOnly()) {
            if (!storage.rootPath().isEmpty() && storage.rootPath().length() >= 2) {
                fixed_drives++;
            }
        }
    }

    ScanResult result;
    result.applicable = fixed_drives > 0;
    result.summary = fixed_drives > 0
        ? QString("Fixed drives detected: %1").arg(fixed_drives)
        : "No fixed drives detected";
    result.details = "Optimization uses Optimize-Volume (defrag/TRIM based on media type)";

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void DefragmentDrivesAction::execute() {
    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();

    auto finish_cancelled = [this, &start_time]() {
        ExecutionResult result;
        result.success = false;
        result.message = "Drive optimization cancelled";
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        setExecutionResult(result);
        setStatus(ActionStatus::Cancelled);
        Q_EMIT executionComplete(result);
    };
    
    Q_EMIT executionProgress("Analyzing drives for optimization...", 5);
    
    // Enterprise approach: Use Optimize-Volume PowerShell cmdlet
    // Per Microsoft docs: Automatically selects correct optimization per drive type:
    // - HDD: Defragmentation
    // - SSD with TRIM: TRIM/Retrim operation
    // - Tiered Storage: TierOptimize
    
    QString ps_script = 
        "# Enterprise Drive Optimization using Optimize-Volume\n"
        "$ErrorActionPreference = 'Continue'; \n"
        "\n"
        "# Get all fixed volumes\n"
        "$volumes = Get-Volume | Where-Object { \n"
        "    $_.DriveType -eq 'Fixed' -and \n"
        "    $_.DriveLetter -and \n"
        "    $_.FileSystem -eq 'NTFS' \n"
        "}; \n"
        "\n"
        "if ($volumes.Count -eq 0) { \n"
        "    Write-Output 'NO_DRIVES_FOUND'; \n"
        "    exit 0; \n"
        "} \n"
        "\n"
        "Write-Output \"Found $($volumes.Count) drive(s) to optimize\"; \n"
        "\n"
        "$optimized = 0; \n"
        "$skipped = 0; \n"
        "\n"
        "foreach ($volume in $volumes) { \n"
        "    $driveLetter = $volume.DriveLetter; \n"
        "    Write-Output \"OPTIMIZING:$driveLetter\"; \n"
        "    \n"
        "    try { \n"
        "        # Get drive type using Get-PhysicalDisk\n"
        "        $partition = Get-Partition -DriveLetter $driveLetter -ErrorAction SilentlyContinue; \n"
        "        $disk = Get-PhysicalDisk -ErrorAction SilentlyContinue | Where-Object { \n"
        "            $_.DeviceID -eq $partition.DiskNumber \n"
        "        } | Select-Object -First 1; \n"
        "        \n"
        "        $driveType = if ($disk) { $disk.MediaType } else { 'Unknown' }; \n"
        "        Write-Output \"DRIVE_TYPE:$driveLetter=$driveType\"; \n"
        "        \n"
        "        # Optimize-Volume automatically selects correct operation:\n"
        "        # HDD -> Defrag, SSD -> Retrim, Tiered -> TierOptimize\n"
        "        Write-Output \"Starting optimization for $driveLetter`:...\"; \n"
        "        \n"
        "        # Run optimization with verbose output\n"
        "        Optimize-Volume -DriveLetter $driveLetter -Verbose -ErrorAction Stop; \n"
        "        \n"
        "        Write-Output \"SUCCESS:$driveLetter\"; \n"
        "        $optimized++; \n"
        "    } catch { \n"
        "        if ($_.Exception.Message -match 'not supported') { \n"
        "            Write-Output \"SKIPPED:$driveLetter (optimization not needed)\"; \n"
        "            $skipped++; \n"
        "        } else { \n"
        "            Write-Warning \"ERROR:$driveLetter - $($_.Exception.Message)\"; \n"
        "        } \n"
        "    } \n"
        "} \n"
        "\n"
        "Write-Output \"TOTAL_OPTIMIZED:$optimized\"; \n"
        "Write-Output \"TOTAL_SKIPPED:$skipped\"; \n"
        "Write-Output 'COMPLETE'";
    
    Q_EMIT executionProgress("Optimizing drives...", 15);

    ProcessResult ps_result = runPowerShell(ps_script, 3600000);
    if (ps_result.timed_out || isCancelled()) {
        finish_cancelled();
        return;
    }

    QString accumulated_output = ps_result.std_out;

    int optimized = 0;
    int total_drives = 0;
    QStringList drive_types;

    QRegularExpression optRe("OPTIMIZING:([A-Z])");
    auto optIt = optRe.globalMatch(accumulated_output);
    while (optIt.hasNext()) {
        optIt.next();
        total_drives++;
    }

    QRegularExpression typeRe("DRIVE_TYPE:([A-Z])=(.+)");
    auto typeIt = typeRe.globalMatch(accumulated_output);
    while (typeIt.hasNext()) {
        auto match = typeIt.next();
        drive_types.append(QString("%1: = %2").arg(match.captured(1), match.captured(2).trimmed()));
    }

    optimized = accumulated_output.count("SUCCESS:", Qt::CaseInsensitive);
    
    if (accumulated_output.contains("NO_DRIVES_FOUND")) {
        ExecutionResult result;
        result.success = true;
        result.message = "No fixed NTFS drives found to optimize";
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        result.log = accumulated_output + (ps_result.std_err.isEmpty() ? "" : "\nErrors:\n" + ps_result.std_err);
        setExecutionResult(result);
        setStatus(ActionStatus::Success);
        Q_EMIT executionComplete(result);
        return;
    }
    
    // Parse final results
    QRegularExpression optimizedRe("TOTAL_OPTIMIZED:(\\d+)");
    QRegularExpression skippedRe("TOTAL_SKIPPED:(\\d+)");
    
    QRegularExpressionMatch optMatch = optimizedRe.match(accumulated_output);
    QRegularExpressionMatch skipMatch = skippedRe.match(accumulated_output);
    
    int total_optimized = optMatch.hasMatch() ? optMatch.captured(1).toInt() : optimized;
    int total_skipped = skipMatch.hasMatch() ? skipMatch.captured(1).toInt() : 0;
    
    Q_EMIT executionProgress("Optimization complete", 100);
    
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    
    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.success = true;
    
    QString drive_info = drive_types.join("\n");
    
    if (total_optimized > 0) {
        result.message = QString("Optimized %1 drive(s)").arg(total_optimized);
        if (total_skipped > 0) {
            result.message += QString(" (%1 skipped)").arg(total_skipped);
        }
    } else if (total_skipped > 0) {
        result.message = QString("All %1 drive(s) skipped (no optimization needed)").arg(total_skipped);
    } else {
        result.message = "Drive optimization completed";
    }
    
    result.log = QString("Drive Types:\n%1\n\nOptimization Details:\n%2")
                    .arg(drive_info)
                    .arg(accumulated_output);
    if (!ps_result.std_err.trimmed().isEmpty()) {
        result.log += "\nErrors:\n" + ps_result.std_err.trimmed();
    }
    
    setExecutionResult(result);
    setStatus(ActionStatus::Success);
    Q_EMIT executionComplete(result);
}

} // namespace sak

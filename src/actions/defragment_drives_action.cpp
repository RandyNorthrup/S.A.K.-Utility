// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/defragment_drives_action.h"
#include <QProcess>
#include <QStorageInfo>
#include <QRegularExpression>

namespace sak {

DefragmentDrivesAction::DefragmentDrivesAction(QObject* parent)
    : QuickAction(parent)
{
}

bool DefragmentDrivesAction::isDriveSSD(const QString& drive_letter) {
    QProcess proc;
    QString cmd = QString("Get-PhysicalDisk | Where-Object {$_.DeviceID -eq (Get-Partition -DriveLetter %1).DiskNumber} | Select-Object -ExpandProperty MediaType")
                     .arg(drive_letter.left(1));
    
    proc.start("powershell.exe", QStringList() << "-NoProfile" << "-Command" << cmd);
    proc.waitForFinished(5000);
    
    QString output = proc.readAllStandardOutput().trimmed();
    return output.contains("SSD", Qt::CaseInsensitive);
}

int DefragmentDrivesAction::analyzeFragmentation(const QString& drive_letter) {
    QProcess proc;
    QString cmd = QString("defrag %1: /A").arg(drive_letter);
    
    proc.start("cmd.exe", QStringList() << "/c" << cmd);
    proc.waitForFinished(30000);
    
    QString output = proc.readAllStandardOutput();
    
    // Parse fragmentation percentage from output
    QRegularExpression re("(\\d+)%.*fragmented");
    QRegularExpressionMatch match = re.match(output);
    
    if (match.hasMatch()) {
        return match.captured(1).toInt();
    }
    return 0;
}

void DefragmentDrivesAction::scan() {
    setStatus(ActionStatus::Ready);
    ScanResult result;
    result.applicable = true;
    result.summary = "Ready to defragment drives";
    setScanResult(result);
    Q_EMIT scanComplete(result);
}

void DefragmentDrivesAction::execute() {
    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
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
    
    QProcess ps;
    ps.start("powershell.exe", QStringList() << "-NoProfile" << "-ExecutionPolicy" << "Bypass" << "-Command" << ps_script);
    
    // Monitor progress
    QString accumulated_output;
    int optimized = 0;
    int total_drives = 0;
    QStringList drive_types;
    
    while (ps.state() == QProcess::Running) {
        if (ps.waitForReadyRead(10000)) {
            QString chunk = ps.readAllStandardOutput();
            accumulated_output += chunk;
            
            // Track drive being optimized
            if (chunk.contains("OPTIMIZING:", Qt::CaseInsensitive)) {
                QRegularExpression re("OPTIMIZING:([A-Z])");
                QRegularExpressionMatch match = re.match(chunk);
                if (match.hasMatch()) {
                    QString letter = match.captured(1);
                    total_drives++;
                    int progress = 10 + (total_drives * 80 / qMax(1, total_drives));
                    Q_EMIT executionProgress(QString("Optimizing drive %1:...").arg(letter), progress);
                }
            }
            
            // Track drive type
            if (chunk.contains("DRIVE_TYPE:", Qt::CaseInsensitive)) {
                QRegularExpression re("DRIVE_TYPE:([A-Z])=(.+)");
                QRegularExpressionMatch match = re.match(chunk);
                if (match.hasMatch()) {
                    QString letter = match.captured(1);
                    QString type = match.captured(2).trimmed();
                    drive_types.append(QString("%1: = %2").arg(letter, type));
                }
            }
            
            // Count successes
            if (chunk.contains("SUCCESS:", Qt::CaseInsensitive)) {
                optimized++;
            }
        }
        
        if (isCancelled()) {
            ps.kill();
            setStatus(ActionStatus::Cancelled);
            return;
        }
    }
    
    ps.waitForFinished(3600000); // 1 hour timeout for large drives
    accumulated_output += ps.readAll();
    
    if (accumulated_output.contains("NO_DRIVES_FOUND")) {
        ExecutionResult result;
        result.success = true;
        result.message = "No fixed NTFS drives found to optimize";
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        result.log = accumulated_output;
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
    
    setExecutionResult(result);
    setStatus(ActionStatus::Success);
    Q_EMIT executionComplete(result);
}

} // namespace sak

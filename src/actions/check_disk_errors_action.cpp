// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/check_disk_errors_action.h"
#include <QProcess>
#include <QStorageInfo>

namespace sak {

CheckDiskErrorsAction::CheckDiskErrorsAction(QObject* parent)
    : QuickAction(parent)
{
}

void CheckDiskErrorsAction::scan() {
    // Scan is no longer used - actions execute immediately
    setStatus(ActionStatus::Ready);
    ScanResult result;
    result.applicable = true;
    result.summary = "Ready to schedule disk error check";
    setScanResult(result);
    Q_EMIT scanComplete(result);
}

void CheckDiskErrorsAction::execute() {
    if (isCancelled()) {
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    Q_EMIT executionProgress("Detecting disk drives...", 5);
    
    // Get all drives
    QVector<QChar> drives;
    for (const QStorageInfo& storage : QStorageInfo::mountedVolumes()) {
        if (storage.isValid() && !storage.isReadOnly() && storage.rootPath().length() >= 2) {
            QChar drive = storage.rootPath().at(0);
            if (drive.isLetter()) {
                drives.append(drive);
            }
        }
    }
    
    if (drives.isEmpty()) {
        ExecutionResult result;
        result.success = false;
        result.message = "No valid drives found for scanning";
        result.log = "Unable to detect any readable, writable volumes";
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        setExecutionResult(result);
        setStatus(ActionStatus::Failed);
        Q_EMIT executionComplete(result);
        return;
    }
    
    QString report;
    report += "╔" + QString("═").repeated(78) + "╗\n";
    report += "║" + QString(" DISK ERROR CHECK & REPAIR REPORT").leftJustified(78) + "║\n";
    report += "╚" + QString("═").repeated(78) + "╝\n\n";
    
    int drives_scanned = 0;
    int errors_found = 0;
    int errors_fixed = 0;
    
    for (int i = 0; i < drives.count(); ++i) {
        const QChar& drive = drives[i];
        
        int progress = 10 + ((i * 80) / drives.count());
        Q_EMIT executionProgress(QString("Scanning drive %1: with Repair-Volume...").arg(drive), progress);
        
        // Use modern Repair-Volume cmdlet instead of deprecated chkdsk
        // -Scan: Performs an online scan (like chkdsk /scan)
        // -OfflineScanAndFix: Comprehensive offline repair (like chkdsk /f /r)
        // -SpotFix: Quick targeted fixes for corrupt system file issues
        QString ps_cmd = QString(
            "$drive = \"%1:\"\\n"
            "Write-Output '===SCAN_START==='\\n"
            "Write-Output \"Drive: $drive\"\\n"
            "\\n"
            "try {\\n"
            "    Write-Output 'Running online scan...'\\n"
            "    Repair-Volume -DriveLetter %1 -Scan -ErrorAction Stop\\n"
            "    Write-Output 'OnlineScan: Success'\\n"
            "    \\n"
            "    if (Test-Path \"$drive\\\\`$corrupt\") {\\n"
            "        Write-Output 'CorruptFile: Detected'\\n"
            "        Write-Output 'Status: Corruption detected - offline repair needed'\\n"
            "        Write-Output 'Scheduling offline repair...'\\n"
            "        Repair-Volume -DriveLetter %1 -OfflineScanAndFix -ErrorAction Stop\\n"
            "        Write-Output 'OfflineRepair: Scheduled'\\n"
            "        Write-Output 'RebootRequired: Yes'\\n"
            "    } else {\\n"
            "        Write-Output 'CorruptFile: NotFound'\\n"
            "        Write-Output 'Status: No corruption detected'\\n"
            "        Write-Output 'RebootRequired: No'\\n"
            "    }\\n"
            "} catch {\\n"
            "    Write-Output \"Error: $($_.Exception.Message)\"\\n"
            "    Write-Output 'Status: Scan failed'\\n"
            "}\\n"
            "\\n"
            "Write-Output '===SCAN_END==='\\n"
        ).arg(drive);
        
        QProcess proc;
        proc.start("powershell.exe", QStringList() << "-Command" << ps_cmd);
        
        if (!proc.waitForFinished(30000)) {
            report += QString("Drive %1: - TIMEOUT (scan took too long)\n\n").arg(drive);
            continue;
        }
        
        QString output = proc.readAllStandardOutput();
        QStringList lines = output.split('\n', Qt::SkipEmptyParts);
        
        bool parsing = false;
        QString drive_letter;
        QString status;
        bool has_corrupt = false;
        bool reboot_needed = false;
        bool scan_success = false;
        
        for (const QString& line : lines) {
            QString trimmed = line.trimmed();
            
            if (trimmed == "===SCAN_START===") {
                parsing = true;
                continue;
            }
            
            if (trimmed == "===SCAN_END===") {
                parsing = false;
                
                if (scan_success) {
                    drives_scanned++;
                    
                    report += QString("Drive %1:\n").arg(drive_letter);
                    report += QString("  Status: %1\n").arg(status);
                    
                    if (has_corrupt) {
                        errors_found++;
                        report += "  ⚠ Corruption detected: $corrupt file found\n";
                        report += "  ℹ Offline repair scheduled at next reboot\n";
                        reboot_needed = true;
                    } else {
                        report += "  ✓ No corruption detected\n";
                    }
                    
                    if (reboot_needed) {
                        report += "  ⚠ REBOOT REQUIRED to complete repair\n";
                    }
                } else {
                    report += QString("Drive %1: - %2\n").arg(drive_letter).arg(status);
                }
                
                report += "\n";
                continue;
            }
            
            if (!parsing) continue;
            
            QStringList parts = trimmed.split(':', Qt::SkipEmptyParts);
            if (parts.size() < 2) continue;
            
            QString key = parts[0].trimmed();
            QString value = parts[1].trimmed();
            
            if (key == "Drive") {
                drive_letter = value;
            } else if (key == "OnlineScan" && value == "Success") {
                scan_success = true;
            } else if (key == "CorruptFile" && value == "Detected") {
                has_corrupt = true;
            } else if (key == "Status") {
                status = value;
            } else if (key == "RebootRequired" && value == "Yes") {
                reboot_needed = true;
            } else if (key == "OfflineRepair" && value == "Scheduled") {
                errors_fixed++;
            }
        }
    }
    
    Q_EMIT executionProgress("Disk error check complete", 100);
    
    report += QString("─").repeated(78) + "\n";
    report += QString("Summary: %1 drive(s) scanned, %2 error(s) found, %3 repair(s) scheduled\n")
        .arg(drives_scanned).arg(errors_found).arg(errors_fixed);
    
    if (errors_fixed > 0) {
        report += "\n⚠ REBOOT REQUIRED to complete offline disk repair\n";
    }
    
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    
    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.files_processed = drives_scanned;
    
    if (drives_scanned > 0) {
        result.success = true;
        result.message = QString("Scanned %1 drive(s): %2 error(s), %3 repair(s) scheduled")
            .arg(drives_scanned).arg(errors_found).arg(errors_fixed);
        result.log = report;
        setStatus((errors_found > 0) ? ActionStatus::Failed : ActionStatus::Success);
    } else {
        result.success = false;
        result.message = "Could not scan any drives";
        result.log = "No drives scanned or PowerShell Storage module unavailable (requires admin privileges)";
        setStatus(ActionStatus::Failed);
    }
    
    setExecutionResult(result);
    Q_EMIT executionComplete(result);
}

} // namespace sak

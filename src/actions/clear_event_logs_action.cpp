// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/clear_event_logs_action.h"
#include <QProcess>
#include <QDir>
#include <QDateTime>
#include <QRegularExpression>

namespace sak {

ClearEventLogsAction::ClearEventLogsAction(QObject* parent)
    : QuickAction(parent)
{
}

bool ClearEventLogsAction::backupEventLog(const QString& log_name) {
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss");
    QString backup_path = QString("C:/SAK_Backups/EventLogs/%1_%2.evtx").arg(log_name, timestamp);
    
    QDir().mkpath("C:/SAK_Backups/EventLogs");
    
    QProcess proc;
    QString cmd = QString("wevtutil epl %1 \"%2\"").arg(log_name, backup_path);
    proc.start("cmd.exe", QStringList() << "/c" << cmd);
    if (!proc.waitForFinished(10000)) {
        return false;
    }
    return proc.exitCode() == 0;
}

bool ClearEventLogsAction::clearEventLog(const QString& log_name) {
    QProcess proc;
    QString cmd = QString("wevtutil cl %1").arg(log_name);
    proc.start("cmd.exe", QStringList() << "/c" << cmd);
    if (!proc.waitForFinished(5000)) {
        return false;
    }
    return proc.exitCode() == 0;
}

void ClearEventLogsAction::scan() {
    // Scan is no longer used - actions execute immediately
    setStatus(ActionStatus::Ready);
    ScanResult result;
    result.applicable = true;
    result.summary = "Ready to clear event logs";
    setScanResult(result);
    Q_EMIT scanComplete(result);
}

void ClearEventLogsAction::execute() {
    if (isCancelled()) {
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    Q_EMIT executionProgress("╔════════════════════════════════════════════════════════════════╗", 0);
    Q_EMIT executionProgress("║        EVENT LOG CLEARING - ENTERPRISE MODE                   ║", 0);
    Q_EMIT executionProgress("╠════════════════════════════════════════════════════════════════╣", 0);
    
    // Comprehensive PowerShell script for enterprise-grade event log management
    QString ps_script = QString(
        "$ErrorActionPreference = 'Continue'\n"
        "$results = @()\n"
        "$totalLogs = 0\n"
        "$clearedLogs = 0\n"
        "$totalEntries = 0\n"
        "$backedUp = 0\n"
        "\n"
        "# Ensure backup directory exists\n"
        "$backupPath = 'C:\\SAK_Backups\\EventLogs'\n"
        "if (-not (Test-Path $backupPath)) {\n"
        "    New-Item -Path $backupPath -ItemType Directory -Force | Out-Null\n"
        "}\n"
        "$timestamp = Get-Date -Format 'yyyyMMdd_HHmmss'\n"
        "\n"
        "# Get all event logs using Get-EventLog -List\n"
        "$allLogs = Get-EventLog -List | Where-Object { $_.Entries.Count -gt 0 } | Sort-Object Log\n"
        "\n"
        "foreach ($log in $allLogs) {\n"
        "    $totalLogs++\n"
        "    $logName = $log.Log\n"
        "    $entryCount = $log.Entries.Count\n"
        "    $totalEntries += $entryCount\n"
        "    \n"
        "    # Attempt to backup using wevtutil\n"
        "    $backupFile = Join-Path $backupPath (\"$($logName)_$timestamp.evtx\")\n"
        "    try {\n"
        "        $backup = Start-Process -FilePath 'wevtutil.exe' -ArgumentList \"epl\", $logName, \"\\\"$backupFile\\\"\" -NoNewWindow -Wait -PassThru\n"
        "        if ($backup.ExitCode -eq 0) {\n"
        "            $backedUp++\n"
        "        }\n"
        "    } catch {\n"
        "        # Backup failed, continue anyway\n"
        "    }\n"
        "    \n"
        "    # Clear the log using wevtutil\n"
        "    try {\n"
        "        $clear = Start-Process -FilePath 'wevtutil.exe' -ArgumentList 'cl', $logName -NoNewWindow -Wait -PassThru\n"
        "        if ($clear.ExitCode -eq 0) {\n"
        "            $clearedLogs++\n"
        "            $results += \"$($logName): Cleared $entryCount entries\"\n"
        "        } else {\n"
        "            $results += \"$($logName): Failed to clear\"\n"
        "        }\n"
        "    } catch {\n"
        "        $results += \"$($logName): Error - $_\"\n"
        "    }\n"
        "}\n"
        "\n"
        "# Output structured results\n"
        "Write-Output \"TOTAL_LOGS:$totalLogs\"\n"
        "Write-Output \"CLEARED_LOGS:$clearedLogs\"\n"
        "Write-Output \"TOTAL_ENTRIES:$totalEntries\"\n"
        "Write-Output \"BACKED_UP:$backedUp\"\n"
        "Write-Output \"BACKUP_PATH:$backupPath\"\n"
        "foreach ($result in $results) {\n"
        "    Write-Output \"DETAIL:$result\"\n"
        "}\n"
    );
    
    Q_EMIT executionProgress("║ Enumerating all event logs with Get-EventLog...              ║", 20);
    
    QProcess ps;
    ps.start("powershell.exe", QStringList() << "-NoProfile" << "-ExecutionPolicy" << "Bypass" << "-Command" << ps_script);
    
    Q_EMIT executionProgress("║ Backing up logs with wevtutil...                             ║", 40);
    Q_EMIT executionProgress("║ Clearing event log entries...                                ║", 60);
    
    bool finished = ps.waitForFinished(300000); // 5 minute timeout for large logs
    
    if (!finished || isCancelled()) {
        ps.kill();
        ExecutionResult result;
        result.success = false;
        result.message = isCancelled() ? "Event log clearing cancelled" : "Operation timed out after 5 minutes";
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        setExecutionResult(result);
        setStatus(ActionStatus::Failed);
        Q_EMIT executionComplete(result);
        return;
    }
    
    Q_EMIT executionProgress("║ Processing results and generating report...                   ║", 80);
    
    QString output = ps.readAllStandardOutput();
    
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    
    // Parse structured output
    QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    int total_logs = 0;
    int cleared_logs = 0;
    int total_entries = 0;
    int backed_up = 0;
    QString backup_path;
    QStringList details;
    
    for (const QString& line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.startsWith("TOTAL_LOGS:")) {
            total_logs = trimmed.mid(11).toInt();
        } else if (trimmed.startsWith("CLEARED_LOGS:")) {
            cleared_logs = trimmed.mid(13).toInt();
        } else if (trimmed.startsWith("TOTAL_ENTRIES:")) {
            total_entries = trimmed.mid(14).toInt();
        } else if (trimmed.startsWith("BACKED_UP:")) {
            backed_up = trimmed.mid(10).toInt();
        } else if (trimmed.startsWith("BACKUP_PATH:")) {
            backup_path = trimmed.mid(12);
        } else if (trimmed.startsWith("DETAIL:")) {
            details.append(trimmed.mid(7));
        }
    }
    
    Q_EMIT executionProgress("╠════════════════════════════════════════════════════════════════╣", 90);
    
    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.files_processed = cleared_logs;
    result.output_path = backup_path;
    
    QString message;
    QString log_output = "╔════════════════════════════════════════════════════════════════╗\n";
    log_output += "║        EVENT LOG CLEARING - RESULTS                           ║\n";
    log_output += "╠════════════════════════════════════════════════════════════════╣\n";
    
    if (cleared_logs > 0) {
        result.success = true;
        
        message = QString("Successfully cleared %1 event log(s)").arg(cleared_logs);
        log_output += QString("║ Logs Processed: %1/%2\n").arg(cleared_logs).arg(total_logs).leftJustified(66) + "║\n";
        log_output += QString("║ Total Entries Cleared: %1\n").arg(total_entries).leftJustified(66) + "║\n";
        log_output += QString("║ Logs Backed Up: %1\n").arg(backed_up).leftJustified(66) + "║\n";
        
        if (!backup_path.isEmpty()) {
            log_output += QString("║ Backup Location: %1\n").arg(backup_path).leftJustified(66) + "║\n";
        }
        
        log_output += "╠════════════════════════════════════════════════════════════════╣\n";
        log_output += QString("║ CLEARED LOGS:                                                  ║\n");
        
        // Show first 10 cleared logs
        int shown = 0;
        for (const QString& detail : details) {
            if (detail.contains("Cleared") && shown < 10) {
                log_output += QString("║ • %1\n").arg(detail).leftJustified(66) + "║\n";
                shown++;
            }
        }
        
        if (details.size() > 10) {
            log_output += QString("║ ... and %1 more\n").arg(details.size() - 10).leftJustified(66) + "║\n";
        }
        
        log_output += "╠════════════════════════════════════════════════════════════════╣\n";
        log_output += QString("║ Completed in: %1 seconds\n").arg(duration_ms / 1000.0, 0, 'f', 2).leftJustified(66) + "║\n";
        log_output += "╚════════════════════════════════════════════════════════════════╝\n";
        
        result.message = message;
        result.log = log_output;
        setStatus(ActionStatus::Success);
    } else {
        result.success = false;
        
        message = "No event logs were cleared";
        log_output += QString("║ Status: No logs processed                                      ║\n");
        log_output += "╠════════════════════════════════════════════════════════════════╣\n";
        log_output += QString("║ Reason: Administrator privileges may be required              ║\n");
        log_output += QString("║ or all event logs are already empty                           ║\n");
        log_output += "╠════════════════════════════════════════════════════════════════╣\n";
        
        if (details.size() > 0) {
            log_output += QString("║ ERROR DETAILS:                                                 ║\n");
            for (int i = 0; i < qMin(5, details.size()); i++) {
                log_output += QString("║ %1\n").arg(details[i]).leftJustified(66) + "║\n";
            }
        }
        
        log_output += "╚════════════════════════════════════════════════════════════════╝\n";
        
        result.message = message;
        result.log = log_output;
        setStatus(ActionStatus::Failed);
    }
    
    setExecutionResult(result);
    Q_EMIT executionComplete(result);
}

} // namespace sak

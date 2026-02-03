// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/clear_event_logs_action.h"
#include "sak/process_runner.h"
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
    
    QString cmd = QString("wevtutil epl %1 \"%2\"").arg(log_name, backup_path);
    ProcessResult proc = runProcess("cmd.exe", QStringList() << "/c" << cmd, 10000);
    if (proc.timed_out) {
        return false;
    }
    return proc.exit_code == 0;
}

bool ClearEventLogsAction::clearEventLog(const QString& log_name) {
    QString cmd = QString("wevtutil cl %1").arg(log_name);
    ProcessResult proc = runProcess("cmd.exe", QStringList() << "/c" << cmd, 5000);
    if (proc.timed_out) {
        return false;
    }
    return proc.exit_code == 0;
}

void ClearEventLogsAction::scan() {
    setStatus(ActionStatus::Scanning);

    Q_EMIT scanProgress("Enumerating event logs...");

    QString ps_cmd =
        "try { "
        "  $logs = Get-EventLog -List | Where-Object { $_.Entries.Count -gt 0 }; "
        "  $totalLogs = $logs.Count; "
        "  $totalEntries = ($logs | Measure-Object -Property Entries.Count -Sum).Sum; "
        "  Write-Output \"LOGS:$totalLogs\"; "
        "  Write-Output \"ENTRIES:$totalEntries\"; "
        "} catch { Write-Output \"LOGS:0\"; Write-Output \"ENTRIES:0\" }";
    ProcessResult proc = runPowerShell(ps_cmd, 8000);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Event log scan warning: " + proc.std_err.trimmed());
    }
    QString output = proc.std_out;
    QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    int total_logs = 0;
    qint64 total_entries = 0;
    for (const auto& line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.startsWith("LOGS:")) {
            total_logs = trimmed.mid(5).toInt();
        } else if (trimmed.startsWith("ENTRIES:")) {
            total_entries = trimmed.mid(8).toLongLong();
        }
    }

    ScanResult result;
    result.applicable = total_logs > 0;
    result.files_count = total_logs;
    result.summary = total_logs > 0
        ? QString("Event logs: %1, entries: %2").arg(total_logs).arg(total_entries)
        : "No event log entries detected";
    result.details = "Full run will backup and clear all event logs";

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void ClearEventLogsAction::execute() {
    if (isCancelled()) {
        ExecutionResult result;
        result.success = false;
        result.message = "Event log clearing cancelled";
        setExecutionResult(result);
        setStatus(ActionStatus::Cancelled);
        Q_EMIT executionComplete(result);
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
    
    ProcessResult ps = runPowerShell(ps_script, 300000);

    Q_EMIT executionProgress("║ Backing up logs with wevtutil...                             ║", 40);
    Q_EMIT executionProgress("║ Clearing event log entries...                                ║", 60);

    if (ps.timed_out || isCancelled()) {
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
    
    if (!ps.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Event log clear warning: " + ps.std_err.trimmed());
    }
    QString output = ps.std_out;
    
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

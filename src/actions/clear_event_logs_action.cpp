// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file clear_event_logs_action.cpp
/// @brief Implements Windows event log clearing and backup

#include "sak/actions/clear_event_logs_action.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/process_runner.h"

#include <QDateTime>
#include <QDir>
#include <QRegularExpression>

namespace sak {

namespace {
/// Box-drawing border helpers (avoid 400+ char \uXXXX lines)
constexpr int kBoxWidth = 63;
inline QString boxTop() {
    return QChar(0x2554) + QString(kBoxWidth, QChar(0x2550)) + QChar(0x2557) + "\n";
}
inline QString boxMid() {
    return QChar(0x2560) + QString(kBoxWidth, QChar(0x2550)) + QChar(0x2563) + "\n";
}
inline QString boxBot() {
    return QChar(0x255A) + QString(kBoxWidth, QChar(0x2550)) + QChar(0x255D) + "\n";
}
}  // namespace

ClearEventLogsAction::ClearEventLogsAction(QObject* parent) : QuickAction(parent) {}

bool ClearEventLogsAction::backupEventLog(const QString& log_name) {
    Q_ASSERT(!log_name.isEmpty());
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss");
    const QString base = m_backup_location.isEmpty() ? QStringLiteral("C:/SAK_Backups")
                                                     : m_backup_location;
    const QString backup_dir = base + QStringLiteral("/EventLogs");
    QString backup_path = QString("%1/%2_%3.evtx").arg(backup_dir, log_name, timestamp);

    if (!QDir().mkpath(backup_dir)) {
        sak::logWarning("Failed to create event log backup directory: {}",
                        backup_dir.toStdString());
    }

    ProcessResult proc = runProcess("wevtutil",
                                    QStringList() << "epl" << log_name << backup_path,
                                    sak::kTimeoutProcessMediumMs);
    return proc.succeeded();
}

bool ClearEventLogsAction::clearEventLog(const QString& log_name) {
    ProcessResult proc =
        runProcess("wevtutil", QStringList() << "cl" << log_name, sak::kTimeoutProcessShortMs);
    return proc.succeeded();
}

void ClearEventLogsAction::scan() {
    setStatus(ActionStatus::Scanning);
    Q_ASSERT(status() == ActionStatus::Scanning);

    Q_EMIT scanProgress("Enumerating event logs...");

    QString ps_cmd =
        "try { "
        "  $logs = Get-EventLog -List | Where-Object { $_.Entries.Count -gt 0 }; "
        "  $totalLogs = $logs.Count; "
        "  $totalEntries = ($logs | Measure-Object -Property Entries.Count -Sum).Sum; "
        "  Write-Output \"LOGS:$totalLogs\"; "
        "  Write-Output \"ENTRIES:$totalEntries\"; "
        "} catch { Write-Output \"LOGS:0\"; Write-Output \"ENTRIES:0\" }";
    ProcessResult proc = runPowerShell(ps_cmd, sak::kTimerNetshWaitMs);
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

    Q_ASSERT(!result.summary.isEmpty());

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void ClearEventLogsAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("Event log clearing cancelled");
        return;
    }

    setStatus(ActionStatus::Running);
    Q_ASSERT(status() == ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    Q_ASSERT(start_time.isValid());

    QString ps_script;
    if (!executeEnumerateLogs(start_time, ps_script)) {
        return;
    }

    int total_logs = 0;
    int cleared_logs = 0;
    int total_entries = 0;
    int backed_up = 0;
    QString backup_path;
    QStringList details;
    if (!executeClearLogs(start_time,
                          ps_script,
                          total_logs,
                          cleared_logs,
                          total_entries,
                          backed_up,
                          backup_path,
                          details)) {
        return;
    }

    executeBuildReport(
        start_time, total_logs, cleared_logs, total_entries, backed_up, backup_path, details);
}

bool ClearEventLogsAction::executeEnumerateLogs(const QDateTime& start_time, QString& ps_script) {
    Q_ASSERT(!ps_script.isEmpty());
    Q_UNUSED(start_time)
    Q_EMIT executionProgress("╔════════════════════════════════════════════════════════════════╗",
                             0);
    Q_EMIT executionProgress("║        EVENT LOG CLEARING - ENTERPRISE MODE                   ║",
                             0);
    Q_EMIT executionProgress("╠════════════════════════════════════════════════════════════════╣",
                             0);

    ps_script = buildLogScriptInit() + buildLogScriptLoop();
    return true;
}

QString ClearEventLogsAction::buildLogScriptInit() const {
    return QString(
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
        "$allLogs = Get-EventLog -List | Where-Object { $_.Entries.Count -gt 0 } | Sort-Object "
        "Log\n");
}

QString ClearEventLogsAction::buildLogScriptLoop() const {
    return QString(
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
        "        $backup = Start-Process -FilePath 'wevtutil.exe' -ArgumentList \"epl\", $logName, "
        "\"\\\"$backupFile\\\"\" -NoNewWindow -Wait -PassThru\n"
        "        if ($backup.ExitCode -eq 0) {\n"
        "            $backedUp++\n"
        "        }\n"
        "    } catch {\n"
        "        # Backup failed, continue anyway\n"
        "    }\n"
        "    \n"
        "    # Clear the log using wevtutil\n"
        "    try {\n"
        "        $clear = Start-Process -FilePath 'wevtutil.exe' -ArgumentList 'cl', $logName "
        "-NoNewWindow -Wait -PassThru\n"
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
        "}\n");
}

bool ClearEventLogsAction::executeClearLogs(const QDateTime& start_time,
                                            const QString& ps_script,
                                            int& total_logs,
                                            int& cleared_logs,
                                            int& total_entries,
                                            int& backed_up,
                                            QString& backup_path,
                                            QStringList& details) {
    Q_EMIT executionProgress("║ Enumerating all event logs with Get-EventLog...              ║",
                             20);

    ProcessResult ps = runPowerShell(ps_script, sak::kTimeoutArchiveMs);

    Q_EMIT executionProgress("║ Backing up logs with wevtutil...                             ║",
                             40);
    Q_EMIT executionProgress("║ Clearing event log entries...                                ║",
                             60);

    if (isCancelled()) {
        emitCancelledResult("Event log clearing cancelled", start_time);
        return false;
    }
    if (ps.timed_out) {
        emitFailedResult("Operation timed out after 5 minutes", {}, start_time);
        return false;
    }

    Q_EMIT executionProgress("║ Processing results and generating report...                   ║",
                             80);

    if (!ps.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Event log clear warning: " + ps.std_err.trimmed());
    }
    QString output = ps.std_out;

    // Parse structured output
    QStringList lines = output.split('\n', Qt::SkipEmptyParts);

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

    return true;
}

void ClearEventLogsAction::executeBuildReport(const QDateTime& start_time,
                                              int total_logs,
                                              int cleared_logs,
                                              int total_entries,
                                              int backed_up,
                                              const QString& backup_path,
                                              const QStringList& details) {
    Q_EMIT executionProgress(boxMid().trimmed(), 90);

    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    Q_ASSERT(!result.success);  // verify default init
    result.duration_ms = duration_ms;
    result.files_processed = cleared_logs;
    result.output_path = backup_path;

    QString log_output = boxTop();
    log_output += "\u2551        EVENT LOG CLEARING - RESULTS                           \u2551\n";
    log_output += boxMid();

    if (cleared_logs > 0) {
        result.success = true;
        result.message = QString("Successfully cleared %1 event log(s)").arg(cleared_logs);
        appendSuccessReport(log_output,
                            total_logs,
                            cleared_logs,
                            total_entries,
                            backed_up,
                            backup_path,
                            details,
                            duration_ms);
        result.log = log_output;
        Q_ASSERT(result.duration_ms >= 0);
        finishWithResult(result, ActionStatus::Success);
    } else {
        result.success = false;
        result.message = "No event logs were cleared";
        appendFailureReport(log_output, details);
        result.log = log_output;
        finishWithResult(result, ActionStatus::Failed);
    }
}

void ClearEventLogsAction::appendSuccessReport(QString& log_output,
                                               int total_logs,
                                               int cleared_logs,
                                               int total_entries,
                                               int backed_up,
                                               const QString& backup_path,
                                               const QStringList& details,
                                               qint64 duration_ms) {
    log_output += QString("\u2551 Logs Processed: %1/%2\n")
                      .arg(cleared_logs)
                      .arg(total_logs)
                      .leftJustified(66) +
                  "\u2551\n";
    log_output +=
        QString("\u2551 Total Entries Cleared: %1\n").arg(total_entries).leftJustified(66) +
        "\u2551\n";
    log_output += QString("\u2551 Logs Backed Up: %1\n").arg(backed_up).leftJustified(66) +
                  "\u2551\n";

    if (!backup_path.isEmpty()) {
        log_output += QString("\u2551 Backup Location: %1\n").arg(backup_path).leftJustified(66) +
                      "\u2551\n";
    }

    log_output += boxMid();
    log_output += QString(
        "\u2551 CLEARED LOGS:                                                  "
        "\u2551\n");

    // Show first 10 cleared logs
    int shown = 0;
    for (const QString& detail : details) {
        if (detail.contains("Cleared") && shown < 10) {
            log_output += QString("\u2551 \u2022 %1\n").arg(detail).leftJustified(66) + "\u2551\n";
            shown++;
        }
    }

    if (details.size() > 10) {
        log_output +=
            QString("\u2551 ... and %1 more\n").arg(details.size() - 10).leftJustified(66) +
            "\u2551\n";
    }

    log_output += boxMid();
    log_output += QString("\u2551 Completed in: %1 seconds\n")
                      .arg(duration_ms / 1000.0, 0, 'f', 2)
                      .leftJustified(66) +
                  "\u2551\n";
    log_output += boxBot();
}

void ClearEventLogsAction::appendFailureReport(QString& log_output, const QStringList& details) {
    Q_ASSERT(!log_output.isEmpty());
    Q_ASSERT(!details.isEmpty());
    log_output += QString(
        "\u2551 Status: No logs processed                                      "
        "\u2551\n");
    log_output += boxMid();
    log_output += QString(
        "\u2551 Reason: Administrator privileges may be required              "
        "\u2551\n");
    log_output += QString(
        "\u2551 or all event logs are already empty                           "
        "\u2551\n");
    log_output += boxMid();

    if (details.size() > 0) {
        log_output += QString(
            "\u2551 ERROR DETAILS:                                               "
            "  \u2551\n");
        for (int i = 0; i < qMin(5, details.size()); i++) {
            log_output += QString("\u2551 %1\n").arg(details[i]).leftJustified(66) + "\u2551\n";
        }
    }

    log_output += boxBot();
}

}  // namespace sak

// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file clear_print_spooler_action.cpp
/// @brief Implements print spooler queue clearing and service restart

#include "sak/actions/clear_print_spooler_action.h"
#include "sak/process_runner.h"
#include "sak/layout_constants.h"
#include <QThread>
#include <QDir>
#include <QDirIterator>

namespace sak {

ClearPrintSpoolerAction::ClearPrintSpoolerAction(QObject* parent)
    : QuickAction(parent)
{
}

int ClearPrintSpoolerAction::countSpoolFiles() {
    const QString spool_path =
        qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"))
        + QStringLiteral("/System32/spool/PRINTERS");
    QDir spool_dir(spool_path);

    if (!spool_dir.exists()) {
        return 0;
    }

    return spool_dir.entryList(QDir::Files).count();
}

void ClearPrintSpoolerAction::stopSpooler() {
    Q_EMIT executionProgress("Stopping print spooler service...", 20);
    ProcessResult proc = runProcess("net", QStringList() << "stop" << "spooler",
        sak::kTimeoutNetworkReadMs);
    if (!proc.succeeded()) {
        Q_EMIT logMessage("Stop spooler warning: " + proc.std_err.trimmed());
    }
    QThread::msleep(sak::kTimerServiceDelayMs);
}

void ClearPrintSpoolerAction::clearSpoolFolder() {
    Q_EMIT executionProgress("Clearing spool folder...", 50);

    const QString spool_path =
        qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"))
        + QStringLiteral("/System32/spool/PRINTERS");
    QDir spool_dir(spool_path);

    if (spool_dir.exists()) {
        for (const QString& file : spool_dir.entryList(QDir::Files)) {
            spool_dir.remove(file);
        }
    }
}

void ClearPrintSpoolerAction::startSpooler() {
    Q_EMIT executionProgress("Starting print spooler service...", 80);
    ProcessResult proc = runProcess("net", QStringList() << "start" << "spooler",
        sak::kTimeoutNetworkReadMs);
    if (!proc.succeeded()) {
        Q_EMIT logMessage("Start spooler warning: " + proc.std_err.trimmed());
    }
}

void ClearPrintSpoolerAction::scan() {
    setStatus(ActionStatus::Scanning);
    Q_ASSERT(status() == ActionStatus::Scanning);

    int files = countSpoolFiles();

    ScanResult result;
    result.applicable = files > 0;
    result.files_count = files;
    result.summary = files > 0
        ? QString("Spool files queued: %1").arg(files)
        : "No spool files detected";
    result.details = "Clearing spooler will restart Print Spooler service";

    Q_ASSERT(!result.summary.isEmpty());

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void ClearPrintSpoolerAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("Print spooler clear cancelled");
        return;
    }
    setStatus(ActionStatus::Running);
    Q_ASSERT(status() == ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    Q_ASSERT(start_time.isValid());

    Q_EMIT executionProgress("╔════════════════════════════════════════════════════════════════╗",
        0);
    Q_EMIT executionProgress("║     PRINT SPOOLER CLEARING - ENTERPRISE MODE                  ║",
        0);
    Q_EMIT executionProgress("╠════════════════════════════════════════════════════════════════╣",
        0);

    Q_EMIT executionProgress("║ Checking Print Spooler service status...                     ║",
        20);
    ProcessResult ps = runPowerShell(buildSpoolerScript(), sak::kTimeoutProcessVeryLongMs);
    Q_EMIT executionProgress("║ Stopping service with Stop-Service...                        ║",
        40);

    if (isCancelled()) {
        emitCancelledResult("Spooler clearing cancelled", start_time);
        return;
    }
    if (ps.timed_out) {
        emitFailedResult("Operation timed out", {}, start_time);
        return;
    }

    Q_EMIT executionProgress("║ Clearing spool files and restarting...                       ║",
        60);

    if (!ps.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Spooler clear warning: " + ps.std_err.trimmed());
    }

    SpoolerResult spooler = parseSpoolerOutput(ps.std_out);
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    Q_EMIT executionProgress("╠════════════════════════════════════════════════════════════════╣",
        80);

    ExecutionResult result;
    Q_ASSERT(!result.success);  // verify default init
    result.duration_ms = duration_ms;
    result.files_processed = spooler.cleared;
    result.bytes_processed = spooler.size_before;

    if (spooler.start_success && spooler.stop_success) {
        result.success = true;
        result.message = spooler.files_before > 0
            ? QString("Cleared %1 stuck print job(s)").arg(spooler.cleared)
            : "Print spooler refreshed (no stuck jobs)";
        result.log = buildSuccessLog(spooler, duration_ms);
        Q_ASSERT(result.duration_ms >= 0);
        finishWithResult(result, ActionStatus::Success);
    } else {
        result.success = false;
        result.message = "Failed to manage Print Spooler service";
        result.log = buildFailureLog(spooler, duration_ms);
        finishWithResult(result, ActionStatus::Failed);
    }
}

// ─── Private Helpers ────────────────────────────────────────────────────────────

QString ClearPrintSpoolerAction::buildSpoolerScript() const {
    return buildSpoolerScriptPreamble() + buildSpoolerScriptRestart();
}

QString ClearPrintSpoolerAction::buildSpoolerScriptPreamble() const {
    return QString(
        "$ErrorActionPreference = 'Continue'\n"
        "$spoolPath = 'C:\\Windows\\System32\\spool\\PRINTERS'\n"
        "$results = @{}\n"
        "\n"
        "# Get current service status\n"
        "$service = Get-Service -Name 'Spooler' -ErrorAction SilentlyContinue\n"
        "if (-not $service) {\n"
        "    Write-Output 'SERVICE_ERROR:Print Spooler service not found'\n"
        "    exit 1\n"
        "}\n"
        "$results['InitialStatus'] = $service.Status\n"
        "$results['StartType'] = $service.StartType\n"
        "\n"
        "# Count files before clearing\n"
        "$filesBefore = 0\n"
        "$sizeBefore = 0\n"
        "if (Test-Path $spoolPath) {\n"
        "    $files = Get-ChildItem -Path $spoolPath -File -ErrorAction SilentlyContinue\n"
        "    $filesBefore = $files.Count\n"
        "    $sizeBefore = ($files | Measure-Object -Property Length -Sum).Sum\n"
        "    if ($null -eq $sizeBefore) { $sizeBefore = 0 }\n"
        "}\n"
        "$results['FilesBefore'] = $filesBefore\n"
        "$results['SizeBefore'] = $sizeBefore\n"
        "\n"
        "# Stop spooler service\n"
        "if ($service.Status -eq 'Running') {\n"
        "    try {\n"
        "        Stop-Service -Name 'Spooler' -Force -ErrorAction Stop\n"
        "        Start-Sleep -Milliseconds 500\n"
        "        \n"
        "        # Verify stopped\n"
        "        $service = Get-Service -Name 'Spooler'\n"
        "        if ($service.Status -eq 'Stopped') {\n"
        "            $results['StopSuccess'] = $true\n"
        "        } else {\n"
        "            $results['StopSuccess'] = $false\n"
        "            Write-Output 'STOP_ERROR:Service did not stop properly'\n"
        "        }\n"
        "    } catch {\n"
        "        $results['StopSuccess'] = $false\n"
        "        Write-Output \"STOP_ERROR:$($_.Exception.Message)\"\n"
        "    }\n"
        "} else {\n"
        "    $results['StopSuccess'] = $true\n"
        "}\n"
        "\n"
        "# Clear spool folder\n"
        "$cleared = 0\n"
        "if ($results['StopSuccess']) {\n"
        "    try {\n"
        "        if (Test-Path $spoolPath) {\n"
        "            $files = Get-ChildItem -Path $spoolPath -File -ErrorAction SilentlyContinue\n"
        "            foreach ($file in $files) {\n"
        "                Remove-Item -Path $file.FullName -Force -ErrorAction Stop\n"
        "                $cleared++\n"
        "            }\n"
        "        }\n"
        "        $results['Cleared'] = $cleared\n"
        "    } catch {\n"
        "        $results['ClearError'] = $_.Exception.Message\n"
        "    }\n"
        "}\n"
    );
}

QString ClearPrintSpoolerAction::buildSpoolerScriptRestart() const {
    return QString(
        "# Start spooler service\n"
        "try {\n"
        "    Start-Service -Name 'Spooler' -ErrorAction Stop\n"
        "    Start-Sleep -Milliseconds 1000\n"
        "    \n"
        "    # Verify started\n"
        "    $service = Get-Service -Name 'Spooler'\n"
        "    if ($service.Status -eq 'Running') {\n"
        "        $results['StartSuccess'] = $true\n"
        "        $results['FinalStatus'] = 'Running'\n"
        "    } else {\n"
        "        $results['StartSuccess'] = $false\n"
        "        $results['FinalStatus'] = $service.Status\n"
        "        Write-Output \"START_ERROR:Service status is $($service.Status)\"\n"
        "    }\n"
        "} catch {\n"
        "    $results['StartSuccess'] = $false\n"
        "    $results['StartError'] = $_.Exception.Message\n"
        "    Write-Output \"START_ERROR:$($_.Exception.Message)\"\n"
        "}\n"
        "\n"
        "# Count files after\n"
        "$filesAfter = 0\n"
        "if (Test-Path $spoolPath) {\n"
        "    $filesAfter = (Get-ChildItem -Path $spoolPath -File -ErrorAction "
        "SilentlyContinue).Count\n"
        "}\n"
        "$results['FilesAfter'] = $filesAfter\n"
        "\n"
        "# Output structured results\n"
        "Write-Output \"INITIAL_STATUS:$($results['InitialStatus'])\"\n"
        "Write-Output \"FILES_BEFORE:$($results['FilesBefore'])\"\n"
        "Write-Output \"SIZE_BEFORE:$($results['SizeBefore'])\"\n"
        "Write-Output \"STOP_SUCCESS:$($results['StopSuccess'])\"\n"
        "Write-Output \"CLEARED:$($results['Cleared'])\"\n"
        "Write-Output \"START_SUCCESS:$($results['StartSuccess'])\"\n"
        "Write-Output \"FINAL_STATUS:$($results['FinalStatus'])\"\n"
        "Write-Output \"FILES_AFTER:$($results['FilesAfter'])\"\n"
    );
}

ClearPrintSpoolerAction::SpoolerResult ClearPrintSpoolerAction::parseSpoolerOutput(
    const QString& output) const {
    SpoolerResult spooler;
    const QStringList lines = output.split('\n', Qt::SkipEmptyParts);

    for (const QString& line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.startsWith("INITIAL_STATUS:")) {
            spooler.initial_status = trimmed.mid(15);
        } else if (trimmed.startsWith("FILES_BEFORE:")) {
            spooler.files_before = trimmed.mid(13).toInt();
        } else if (trimmed.startsWith("SIZE_BEFORE:")) {
            spooler.size_before = trimmed.mid(12).toLongLong();
        } else if (trimmed.startsWith("STOP_SUCCESS:")) {
            spooler.stop_success = (trimmed.mid(13) == "True");
        } else if (trimmed.startsWith("CLEARED:")) {
            spooler.cleared = trimmed.mid(8).toInt();
        } else if (trimmed.startsWith("START_SUCCESS:")) {
            spooler.start_success = (trimmed.mid(14) == "True");
        } else if (trimmed.startsWith("FINAL_STATUS:")) {
            spooler.final_status = trimmed.mid(13);
        } else if (trimmed.startsWith("FILES_AFTER:")) {
            spooler.files_after = trimmed.mid(12).toInt();
        } else if (trimmed.contains("_ERROR:")) {
            spooler.errors.append(trimmed);
        }
    }

    return spooler;
}

QString ClearPrintSpoolerAction::buildSuccessLog(const SpoolerResult& spooler,
    qint64 duration_ms) const {
    QString log;
    log += "╔════════════════════════════════════════════════════════════════╗\n";
    log += "║     PRINT SPOOLER CLEARING - RESULTS                          ║\n";
    log += "╠════════════════════════════════════════════════════════════════╣\n";

    if (spooler.files_before > 0) {
        log += QString("║ Print Jobs Cleared: %1\n").arg(spooler.cleared).leftJustified(66) + "║\n";
        log += QString("║ Space Freed: %1\n")
            .arg(formatFileSize(spooler.size_before)).leftJustified(66) + "║\n";
    } else {
        log += QString("║ Status: No stuck jobs found                                    ║\n");
    }

    log += "╠════════════════════════════════════════════════════════════════╣\n";
    log += QString("║ Service Status: %1 → %2\n").arg(spooler.initial_status,
        spooler.final_status).leftJustified(66) + "║\n";
    log += QString("║ Service Stopped: Successfully\n").leftJustified(66) + "║\n";
    log += QString("║ Service Started: Successfully\n").leftJustified(66) + "║\n";
    log += "╠════════════════════════════════════════════════════════════════╣\n";
    log += QString("║ Completed in: %1 seconds\n").arg(duration_ms / 1000.0, 0, 'f',
        2).leftJustified(66) + "║\n";
    log += "╚════════════════════════════════════════════════════════════════╝\n";

    return log;
}

QString ClearPrintSpoolerAction::buildFailureLog(const SpoolerResult& spooler,
    qint64 duration_ms) const {
    Q_UNUSED(duration_ms)
    QString log;
    log += "╔════════════════════════════════════════════════════════════════╗\n";
    log += "║     PRINT SPOOLER CLEARING - RESULTS                          ║\n";
    log += "╠════════════════════════════════════════════════════════════════╣\n";
    log += QString("║ Status: Operation Failed                                       ║\n");
    log += "╠════════════════════════════════════════════════════════════════╣\n";

    if (!spooler.stop_success) {
        log += QString("║ Service Stop: FAILED\n").leftJustified(66) + "║\n";
    } else {
        log += QString("║ Service Stop: SUCCESS\n").leftJustified(66) + "║\n";
    }

    if (!spooler.start_success) {
        log += QString("║ Service Start: FAILED\n").leftJustified(66) + "║\n";
    }

    log += QString("║ Final Service Status: %1\n")
        .arg(spooler.final_status.isEmpty() ? "Unknown" : spooler.final_status)
            .leftJustified(66) + "║\n";

    if (!spooler.errors.isEmpty()) {
        log += "╠════════════════════════════════════════════════════════════════╣\n";
        log += QString("║ ERRORS:                                                        ║\n");
        for (const QString& error : spooler.errors) {
            log += QString("║ %1\n").arg(error).leftJustified(66) + "║\n";
        }
    }

    log += "╠════════════════════════════════════════════════════════════════╣\n";
    log += QString("║ Action Required: Run as Administrator or restart manually      ║\n");
    log += "╚════════════════════════════════════════════════════════════════╝\n";

    return log;
}

} // namespace sak

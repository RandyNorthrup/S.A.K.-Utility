// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/clear_print_spooler_action.h"
#include "sak/process_runner.h"
#include <QThread>
#include <QDir>
#include <QDirIterator>

namespace sak {

ClearPrintSpoolerAction::ClearPrintSpoolerAction(QObject* parent)
    : QuickAction(parent)
{
}

int ClearPrintSpoolerAction::countSpoolFiles() {
    QString spool_path = "C:/Windows/System32/spool/PRINTERS";
    QDir spool_dir(spool_path);
    
    if (!spool_dir.exists()) {
        return 0;
    }
    
    return spool_dir.entryList(QDir::Files).count();
}

void ClearPrintSpoolerAction::stopSpooler() {
    Q_EMIT executionProgress("Stopping print spooler service...", 20);
    ProcessResult proc = runProcess("net", QStringList() << "stop" << "spooler", 15000);
    if (proc.timed_out || proc.exit_code != 0) {
        Q_EMIT logMessage("Stop spooler warning: " + proc.std_err.trimmed());
    }
    QThread::msleep(2000);
}

void ClearPrintSpoolerAction::clearSpoolFolder() {
    Q_EMIT executionProgress("Clearing spool folder...", 50);
    
    QString spool_path = "C:/Windows/System32/spool/PRINTERS";
    QDir spool_dir(spool_path);
    
    if (spool_dir.exists()) {
        for (const QString& file : spool_dir.entryList(QDir::Files)) {
            spool_dir.remove(file);
        }
    }
}

void ClearPrintSpoolerAction::startSpooler() {
    Q_EMIT executionProgress("Starting print spooler service...", 80);
    ProcessResult proc = runProcess("net", QStringList() << "start" << "spooler", 15000);
    if (proc.timed_out || proc.exit_code != 0) {
        Q_EMIT logMessage("Start spooler warning: " + proc.std_err.trimmed());
    }
}

void ClearPrintSpoolerAction::scan() {
    setStatus(ActionStatus::Scanning);

    int files = countSpoolFiles();

    ScanResult result;
    result.applicable = files > 0;
    result.files_count = files;
    result.summary = files > 0
        ? QString("Spool files queued: %1").arg(files)
        : "No spool files detected";
    result.details = "Clearing spooler will restart Print Spooler service";

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void ClearPrintSpoolerAction::execute() {
    if (isCancelled()) {
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    Q_EMIT executionProgress("╔════════════════════════════════════════════════════════════════╗", 0);
    Q_EMIT executionProgress("║     PRINT SPOOLER CLEARING - ENTERPRISE MODE                  ║", 0);
    Q_EMIT executionProgress("╠════════════════════════════════════════════════════════════════╣", 0);
    
    // Enterprise PowerShell script with Get-Service verification
    QString ps_script = QString(
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
        "\n"
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
        "    $filesAfter = (Get-ChildItem -Path $spoolPath -File -ErrorAction SilentlyContinue).Count\n"
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
    
    Q_EMIT executionProgress("║ Checking Print Spooler service status...                     ║", 20);
    
    ProcessResult ps = runPowerShell(ps_script, 60000);

    Q_EMIT executionProgress("║ Stopping service with Stop-Service...                        ║", 40);

    if (ps.timed_out || isCancelled()) {
        ExecutionResult result;
        result.success = false;
        result.message = isCancelled() ? "Spooler clearing cancelled" : "Operation timed out";
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        setExecutionResult(result);
        setStatus(ActionStatus::Failed);
        Q_EMIT executionComplete(result);
        return;
    }
    
    Q_EMIT executionProgress("║ Clearing spool files and restarting...                       ║", 60);
    
    if (!ps.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Spooler clear warning: " + ps.std_err.trimmed());
    }
    QString output = ps.std_out;
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    
    // Parse results
    QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    QString initial_status, final_status;
    int files_before = 0, files_after = 0, cleared = 0;
    qint64 size_before = 0;
    bool stop_success = false, start_success = false;
    QStringList errors;
    
    for (const QString& line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.startsWith("INITIAL_STATUS:")) {
            initial_status = trimmed.mid(15);
        } else if (trimmed.startsWith("FILES_BEFORE:")) {
            files_before = trimmed.mid(13).toInt();
        } else if (trimmed.startsWith("SIZE_BEFORE:")) {
            size_before = trimmed.mid(12).toLongLong();
        } else if (trimmed.startsWith("STOP_SUCCESS:")) {
            stop_success = (trimmed.mid(13) == "True");
        } else if (trimmed.startsWith("CLEARED:")) {
            cleared = trimmed.mid(8).toInt();
        } else if (trimmed.startsWith("START_SUCCESS:")) {
            start_success = (trimmed.mid(14) == "True");
        } else if (trimmed.startsWith("FINAL_STATUS:")) {
            final_status = trimmed.mid(13);
        } else if (trimmed.startsWith("FILES_AFTER:")) {
            files_after = trimmed.mid(12).toInt();
        } else if (trimmed.contains("_ERROR:")) {
            errors.append(trimmed);
        }
    }
    
    Q_EMIT executionProgress("╠════════════════════════════════════════════════════════════════╣", 80);
    
    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.files_processed = cleared;
    result.bytes_processed = size_before;
    
    QString message;
    QString log_output = "╔════════════════════════════════════════════════════════════════╗\n";
    log_output += "║     PRINT SPOOLER CLEARING - RESULTS                          ║\n";
    log_output += "╠════════════════════════════════════════════════════════════════╣\n";
    
    if (start_success && stop_success) {
        result.success = true;
        
        QString size_str;
        if (size_before >= 1048576LL) {
            size_str = QString::number(size_before / 1048576.0, 'f', 2) + " MB";
        } else if (size_before >= 1024LL) {
            size_str = QString::number(size_before / 1024.0, 'f', 2) + " KB";
        } else {
            size_str = QString::number(size_before) + " bytes";
        }
        
        if (files_before > 0) {
            message = QString("Cleared %1 stuck print job(s)").arg(cleared);
            log_output += QString("║ Print Jobs Cleared: %1\n").arg(cleared).leftJustified(66) + "║\n";
            log_output += QString("║ Space Freed: %1\n").arg(size_str).leftJustified(66) + "║\n";
        } else {
            message = "Print spooler refreshed (no stuck jobs)";
            log_output += QString("║ Status: No stuck jobs found                                    ║\n");
        }
        
        log_output += "╠════════════════════════════════════════════════════════════════╣\n";
        log_output += QString("║ Service Status: %1 → %2\n").arg(initial_status, final_status).leftJustified(66) + "║\n";
        log_output += QString("║ Service Stopped: Successfully\n").leftJustified(66) + "║\n";
        log_output += QString("║ Service Started: Successfully\n").leftJustified(66) + "║\n";
        log_output += "╠════════════════════════════════════════════════════════════════╣\n";
        log_output += QString("║ Completed in: %1 seconds\n").arg(duration_ms / 1000.0, 0, 'f', 2).leftJustified(66) + "║\n";
        log_output += "╚════════════════════════════════════════════════════════════════╝\n";
        
        result.message = message;
        result.log = log_output;
        setStatus(ActionStatus::Success);
    } else {
        result.success = false;
        
        message = "Failed to manage Print Spooler service";
        log_output += QString("║ Status: Operation Failed                                       ║\n");
        log_output += "╠════════════════════════════════════════════════════════════════╣\n";
        
        if (!stop_success) {
            log_output += QString("║ Service Stop: FAILED\n").leftJustified(66) + "║\n";
        } else {
            log_output += QString("║ Service Stop: SUCCESS\n").leftJustified(66) + "║\n";
        }
        
        if (!start_success) {
            log_output += QString("║ Service Start: FAILED\n").leftJustified(66) + "║\n";
        }
        
        log_output += QString("║ Final Service Status: %1\n").arg(final_status.isEmpty() ? "Unknown" : final_status).leftJustified(66) + "║\n";
        
        if (!errors.isEmpty()) {
            log_output += "╠════════════════════════════════════════════════════════════════╣\n";
            log_output += QString("║ ERRORS:                                                        ║\n");
            for (const QString& error : errors) {
                log_output += QString("║ %1\n").arg(error).leftJustified(66) + "║\n";
            }
        }
        
        log_output += "╠════════════════════════════════════════════════════════════════╣\n";
        log_output += QString("║ Action Required: Run as Administrator or restart manually      ║\n");
        log_output += "╚════════════════════════════════════════════════════════════════╝\n";
        
        result.message = message;
        result.log = log_output;
        setStatus(ActionStatus::Failed);
    }
    
    setExecutionResult(result);
    Q_EMIT executionComplete(result);
}

} // namespace sak

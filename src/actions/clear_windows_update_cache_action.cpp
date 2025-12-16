// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/clear_windows_update_cache_action.h"
#include <QProcess>
#include <QDir>
#include <QDirIterator>
#include <QThread>

namespace sak {

ClearWindowsUpdateCacheAction::ClearWindowsUpdateCacheAction(QObject* parent)
    : QuickAction(parent)
{
}

bool ClearWindowsUpdateCacheAction::stopWindowsUpdateService() {
    Q_EMIT executionProgress("Stopping Windows Update service...", 20);
    int result = QProcess::execute("net", QStringList() << "stop" << "wuauserv");
    QThread::msleep(2000);
    return (result == 0);
}

bool ClearWindowsUpdateCacheAction::startWindowsUpdateService() {
    Q_EMIT executionProgress("Starting Windows Update service...", 80);
    int result = QProcess::execute("net", QStringList() << "start" << "wuauserv");
    return (result == 0);
}

qint64 ClearWindowsUpdateCacheAction::calculateDirectorySize(const QString& path, int& file_count) {
    qint64 total_size = 0;
    file_count = 0;
    
    QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total_size += it.fileInfo().size();
        file_count++;
    }
    
    return total_size;
}

void ClearWindowsUpdateCacheAction::scan() {
    setStatus(ActionStatus::Ready);
    ScanResult result;
    result.applicable = true;
    result.summary = "Ready to clear Windows Update cache";
    setScanResult(result);
    Q_EMIT scanComplete(result);
}

void ClearWindowsUpdateCacheAction::execute() {
    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    Q_EMIT executionProgress("╔════════════════════════════════════════════════════════════════╗", 0);
    Q_EMIT executionProgress("║   WINDOWS UPDATE CACHE CLEARING - ENTERPRISE MODE            ║", 0);
    Q_EMIT executionProgress("╠════════════════════════════════════════════════════════════════╣", 0);
    
    // Enterprise PowerShell script with multi-service management
    QString ps_script = QString(
        "$ErrorActionPreference = 'Continue'\n"
        "$results = @{}\n"
        "$services = @('wuauserv', 'bits', 'cryptsvc')\n"
        "$paths = @(\n"
        "    'C:\\Windows\\SoftwareDistribution\\Download',\n"
        "    'C:\\Windows\\SoftwareDistribution\\DataStore',\n"
        "    'C:\\Windows\\System32\\catroot2'\n"
        ")\n"
        "\n"
        "# Function to format bytes\n"
        "function Format-Bytes {\n"
        "    param([long]$Bytes)\n"
        "    if ($Bytes -ge 1GB) { return '{0:N2} GB' -f ($Bytes / 1GB) }\n"
        "    if ($Bytes -ge 1MB) { return '{0:N2} MB' -f ($Bytes / 1MB) }\n"
        "    if ($Bytes -ge 1KB) { return '{0:N2} KB' -f ($Bytes / 1KB) }\n"
        "    return '{0} bytes' -f $Bytes\n"
        "}\n"
        "\n"
        "# Function to get directory size\n"
        "function Get-DirectorySize {\n"
        "    param([string]$Path)\n"
        "    if (-not (Test-Path $Path)) { return 0 }\n"
        "    $size = (Get-ChildItem -Path $Path -Recurse -File -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum\n"
        "    if ($null -eq $size) { return 0 }\n"
        "    return $size\n"
        "}\n"
        "\n"
        "# Get initial service status\n"
        "foreach ($svc in $services) {\n"
        "    $service = Get-Service -Name $svc -ErrorAction SilentlyContinue\n"
        "    if ($service) {\n"
        "        $results[\"${svc}_InitialStatus\"] = $service.Status\n"
        "    } else {\n"
        "        Write-Output \"SERVICE_ERROR:$svc not found\"\n"
        "        $results[\"${svc}_InitialStatus\"] = 'NotFound'\n"
        "    }\n"
        "}\n"
        "\n"
        "# Calculate sizes before\n"
        "$totalBefore = 0\n"
        "foreach ($path in $paths) {\n"
        "    $size = Get-DirectorySize -Path $path\n"
        "    $pathName = Split-Path $path -Leaf\n"
        "    $results[\"${pathName}_Before\"] = $size\n"
        "    $totalBefore += $size\n"
        "}\n"
        "$results['TotalBefore'] = $totalBefore\n"
        "\n"
        "# Stop services\n"
        "$stopSuccess = 0\n"
        "foreach ($svc in $services) {\n"
        "    try {\n"
        "        $service = Get-Service -Name $svc -ErrorAction Stop\n"
        "        if ($service.Status -eq 'Running') {\n"
        "            Stop-Service -Name $svc -Force -ErrorAction Stop\n"
        "            Start-Sleep -Milliseconds 500\n"
        "            $stopSuccess++\n"
        "        } else {\n"
        "            $stopSuccess++\n"
        "        }\n"
        "        $results[\"${svc}_Stopped\"] = $true\n"
        "    } catch {\n"
        "        $results[\"${svc}_Stopped\"] = $false\n"
        "        $results[\"${svc}_StopError\"] = $_.Exception.Message\n"
        "        Write-Output \"STOP_ERROR:$svc - $($_.Exception.Message)\"\n"
        "    }\n"
        "}\n"
        "\n"
        "# Clear cache directories\n"
        "$clearedPaths = 0\n"
        "if ($stopSuccess -eq $services.Count) {\n"
        "    foreach ($path in $paths) {\n"
        "        $pathName = Split-Path $path -Leaf\n"
        "        try {\n"
        "            if (Test-Path $path) {\n"
        "                # For catroot2, rename instead of delete\n"
        "                if ($pathName -eq 'catroot2') {\n"
        "                    $timestamp = Get-Date -Format 'yyyyMMdd_HHmmss'\n"
        "                    $backupPath = \"$path.bak_$timestamp\"\n"
        "                    Rename-Item -Path $path -NewName \"catroot2.bak_$timestamp\" -Force -ErrorAction Stop\n"
        "                    New-Item -Path $path -ItemType Directory -Force | Out-Null\n"
        "                } else {\n"
        "                    Remove-Item -Path \"$path\\*\" -Recurse -Force -ErrorAction Stop\n"
        "                }\n"
        "                $clearedPaths++\n"
        "                $results[\"${pathName}_Cleared\"] = $true\n"
        "            }\n"
        "        } catch {\n"
        "            $results[\"${pathName}_Cleared\"] = $false\n"
        "            $results[\"${pathName}_Error\"] = $_.Exception.Message\n"
        "            Write-Output \"CLEAR_ERROR:$pathName - $($_.Exception.Message)\"\n"
        "        }\n"
        "    }\n"
        "}\n"
        "$results['ClearedPaths'] = $clearedPaths\n"
        "\n"
        "# Start services\n"
        "$startSuccess = 0\n"
        "foreach ($svc in $services) {\n"
        "    try {\n"
        "        Start-Service -Name $svc -ErrorAction Stop\n"
        "        Start-Sleep -Milliseconds 500\n"
        "        $service = Get-Service -Name $svc\n"
        "        if ($service.Status -eq 'Running') {\n"
        "            $startSuccess++\n"
        "            $results[\"${svc}_Started\"] = $true\n"
        "        } else {\n"
        "            $results[\"${svc}_Started\"] = $false\n"
        "        }\n"
        "    } catch {\n"
        "        $results[\"${svc}_Started\"] = $false\n"
        "        $results[\"${svc}_StartError\"] = $_.Exception.Message\n"
        "        Write-Output \"START_ERROR:$svc - $($_.Exception.Message)\"\n"
        "    }\n"
        "}\n"
        "\n"
        "# Calculate sizes after\n"
        "$totalAfter = 0\n"
        "foreach ($path in $paths) {\n"
        "    $size = Get-DirectorySize -Path $path\n"
        "    $pathName = Split-Path $path -Leaf\n"
        "    $results[\"${pathName}_After\"] = $size\n"
        "    $totalAfter += $size\n"
        "}\n"
        "$results['TotalAfter'] = $totalAfter\n"
        "$results['TotalCleared'] = $totalBefore - $totalAfter\n"
        "\n"
        "# Output structured results\n"
        "Write-Output \"TOTAL_BEFORE:$totalBefore\"\n"
        "Write-Output \"TOTAL_CLEARED:$($results['TotalCleared'])\"\n"
        "Write-Output \"PATHS_CLEARED:$clearedPaths\"\n"
        "Write-Output \"SERVICES_STOPPED:$stopSuccess\"\n"
        "Write-Output \"SERVICES_STARTED:$startSuccess\"\n"
        "\n"
        "foreach ($svc in $services) {\n"
        "    Write-Output \"SERVICE:$svc|$($results[\"${svc}_InitialStatus\"])|$($results[\"${svc}_Stopped\"])|$($results[\"${svc}_Started\"])\"\n"
        "}\n"
        "\n"
        "foreach ($path in $paths) {\n"
        "    $pathName = Split-Path $path -Leaf\n"
        "    $before = $results[\"${pathName}_Before\"]\n"
        "    $after = $results[\"${pathName}_After\"]\n"
        "    $cleared = $before - $after\n"
        "    Write-Output \"PATH:$pathName|$(Format-Bytes $before)|$(Format-Bytes $cleared)|$($results[\"${pathName}_Cleared\"])\"\n"
        "}\n"
    );
    
    Q_EMIT executionProgress("║ Checking Windows Update service status...                    ║", 20);
    
    QProcess ps;
    ps.start("powershell.exe", QStringList() << "-NoProfile" << "-ExecutionPolicy" << "Bypass" << "-Command" << ps_script);
    
    Q_EMIT executionProgress("║ Stopping wuauserv, bits, and cryptsvc services...           ║", 40);
    
    bool finished = ps.waitForFinished(120000); // 2 minute timeout
    
    if (!finished || isCancelled()) {
        ps.kill();
        ExecutionResult result;
        result.success = false;
        result.message = isCancelled() ? "Cache clearing cancelled" : "Operation timed out";
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        setExecutionResult(result);
        setStatus(ActionStatus::Failed);
        Q_EMIT executionComplete(result);
        return;
    }
    
    Q_EMIT executionProgress("║ Clearing SoftwareDistribution and catroot2...                ║", 60);
    
    QString output = ps.readAllStandardOutput();
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    
    // Parse results
    QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    qint64 total_before = 0, total_cleared = 0;
    int paths_cleared = 0, services_stopped = 0, services_started = 0;
    QStringList service_details, path_details, errors;
    
    for (const QString& line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.startsWith("TOTAL_BEFORE:")) {
            total_before = trimmed.mid(13).toLongLong();
        } else if (trimmed.startsWith("TOTAL_CLEARED:")) {
            total_cleared = trimmed.mid(14).toLongLong();
        } else if (trimmed.startsWith("PATHS_CLEARED:")) {
            paths_cleared = trimmed.mid(14).toInt();
        } else if (trimmed.startsWith("SERVICES_STOPPED:")) {
            services_stopped = trimmed.mid(17).toInt();
        } else if (trimmed.startsWith("SERVICES_STARTED:")) {
            services_started = trimmed.mid(17).toInt();
        } else if (trimmed.startsWith("SERVICE:")) {
            service_details.append(trimmed.mid(8));
        } else if (trimmed.startsWith("PATH:")) {
            path_details.append(trimmed.mid(5));
        } else if (trimmed.contains("_ERROR:")) {
            errors.append(trimmed);
        }
    }
    
    Q_EMIT executionProgress("╠════════════════════════════════════════════════════════════════╣", 80);
    
    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.bytes_processed = total_cleared;
    result.files_processed = paths_cleared;
    
    QString message;
    QString log_output = "╔════════════════════════════════════════════════════════════════╗\n";
    log_output += "║   WINDOWS UPDATE CACHE CLEARING - RESULTS                    ║\n";
    log_output += "╠════════════════════════════════════════════════════════════════╣\n";
    
    if (services_stopped == 3 && services_started == 3 && paths_cleared > 0) {
        result.success = true;
        
        QString size_str;
        if (total_cleared >= 1073741824LL) {
            size_str = QString::number(total_cleared / 1073741824.0, 'f', 2) + " GB";
        } else if (total_cleared >= 1048576LL) {
            size_str = QString::number(total_cleared / 1048576.0, 'f', 2) + " MB";
        } else if (total_cleared >= 1024LL) {
            size_str = QString::number(total_cleared / 1024.0, 'f', 2) + " KB";
        } else {
            size_str = QString::number(total_cleared) + " bytes";
        }
        
        message = QString("Cleared %1 from Windows Update cache").arg(size_str);
        log_output += QString("║ Total Space Freed: %1\n").arg(size_str).leftJustified(66) + "║\n";
        log_output += QString("║ Cache Paths Cleared: %1/3\n").arg(paths_cleared).leftJustified(66) + "║\n";
        log_output += "╠════════════════════════════════════════════════════════════════╣\n";
        log_output += QString("║ SERVICES MANAGED:                                              ║\n");
        
        for (const QString& svc_detail : service_details) {
            QStringList parts = svc_detail.split('|');
            if (parts.size() >= 4) {
                log_output += QString("║ • %1: %2 → Stopped → Restarted\n").arg(parts[0], parts[1]).leftJustified(66) + "║\n";
            }
        }
        
        log_output += "╠════════════════════════════════════════════════════════════════╣\n";
        log_output += QString("║ CACHE DIRECTORIES:                                             ║\n");
        
        for (const QString& path_detail : path_details) {
            QStringList parts = path_detail.split('|');
            if (parts.size() >= 4 && parts[3] == "True") {
                log_output += QString("║ • %1: %2 cleared\n").arg(parts[0], parts[2]).leftJustified(66) + "║\n";
            }
        }
        
        log_output += "╠════════════════════════════════════════════════════════════════╣\n";
        log_output += QString("║ Completed in: %1 seconds\n").arg(duration_ms / 1000.0, 0, 'f', 2).leftJustified(66) + "║\n";
        log_output += "╚════════════════════════════════════════════════════════════════╝\n";
        
        result.message = message;
        result.log = log_output;
        setStatus(ActionStatus::Success);
    } else {
        result.success = false;
        
        message = "Failed to clear Windows Update cache";
        log_output += QString("║ Status: Operation Failed                                       ║\n");
        log_output += "╠════════════════════════════════════════════════════════════════╣\n";
        log_output += QString("║ Services Stopped: %1/3\n").arg(services_stopped).leftJustified(66) + "║\n";
        log_output += QString("║ Services Started: %1/3\n").arg(services_started).leftJustified(66) + "║\n";
        log_output += QString("║ Paths Cleared: %1/3\n").arg(paths_cleared).leftJustified(66) + "║\n";
        
        if (!errors.isEmpty()) {
            log_output += "╠════════════════════════════════════════════════════════════════╣\n";
            log_output += QString("║ ERRORS:                                                        ║\n");
            for (const QString& error : errors) {
                log_output += QString("║ %1\n").arg(error).leftJustified(66) + "║\n";
            }
        }
        
        log_output += "╠════════════════════════════════════════════════════════════════╣\n";
        log_output += QString("║ Action Required: Run as Administrator                          ║\n");
        log_output += "╚════════════════════════════════════════════════════════════════╝\n";
        
        result.message = message;
        result.log = log_output;
        setStatus(ActionStatus::Failed);
    }
    
    setExecutionResult(result);
    Q_EMIT executionComplete(result);
}

} // namespace sak

// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file clear_windows_update_cache_action.cpp
/// @brief Implements Windows Update cache cleanup to free disk space

#include "sak/actions/clear_windows_update_cache_action.h"
#include "sak/path_utils.h"
#include "sak/process_runner.h"
#include "sak/layout_constants.h"
#include <QDir>
#include <QThread>

namespace sak {

ClearWindowsUpdateCacheAction::ClearWindowsUpdateCacheAction(QObject* parent)
    : QuickAction(parent)
{
}

bool ClearWindowsUpdateCacheAction::stopWindowsUpdateService() {
    Q_EMIT executionProgress("Stopping Windows Update service...", 20);
    ProcessResult result = runProcess("net", QStringList() << "stop" << "wuauserv",
        sak::kTimeoutNetworkReadMs);
    QThread::msleep(sak::kTimerServiceDelayMs);
    return result.succeeded();
}

bool ClearWindowsUpdateCacheAction::startWindowsUpdateService() {
    Q_EMIT executionProgress("Starting Windows Update service...", 80);
    ProcessResult result = runProcess("net", QStringList() << "start" << "wuauserv",
        sak::kTimeoutNetworkReadMs);
    return result.succeeded();
}

qint64 ClearWindowsUpdateCacheAction::calculateDirectorySize(const QString& path, int& file_count) {
    file_count = 0;
    auto result = path_utils::getDirectorySizeAndCount(path.toStdWString());
    if (!result) {
        return 0;
    }
    file_count = static_cast<int>(result->file_count);
    return static_cast<qint64>(result->total_bytes);
}

void ClearWindowsUpdateCacheAction::scan() {
    setStatus(ActionStatus::Scanning);
    Q_ASSERT(status() == ActionStatus::Scanning);

    Q_EMIT scanProgress("Calculating Windows Update cache size...");

    const QString sysRoot =
        qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"));
    QStringList paths = {
        sysRoot + QStringLiteral("/SoftwareDistribution/Download"),
        sysRoot + QStringLiteral("/SoftwareDistribution/DataStore"),
        sysRoot + QStringLiteral("/System32/catroot2")
    };

    qint64 total_size = 0;
    int total_files = 0;

    for (const QString& path : paths) {
        if (isCancelled()) {
            return;
        }
        int count = 0;
        total_size += calculateDirectorySize(path, count);
        total_files += count;
    }

    ScanResult result;
    result.applicable = total_size > 0;
    result.bytes_affected = total_size;
    result.files_count = total_files;
    result.summary = total_size > 0
        ? QString("Cache size: %1 MB").arg(total_size / sak::kBytesPerMBf, 0, 'f', 1)
        : "Windows Update cache is already minimal";
    result.details = "Clearing cache stops update services briefly";

    Q_ASSERT(!result.summary.isEmpty());

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void ClearWindowsUpdateCacheAction::execute() {
    setStatus(ActionStatus::Running);
    Q_ASSERT(status() == ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    Q_ASSERT(start_time.isValid());

    Q_EMIT executionProgress("╔════════════════════════════════════════════════════════════════╗",
        0);
    Q_EMIT executionProgress("║   WINDOWS UPDATE CACHE CLEARING - ENTERPRISE MODE            ║", 0);
    Q_EMIT executionProgress("╠════════════════════════════════════════════════════════════════╣",
        0);

    QString ps_script = buildCacheCleanupScript();

    Q_EMIT executionProgress("║ Checking Windows Update service status...                    ║",
        20);
    Q_EMIT executionProgress("║ Stopping wuauserv, bits, and cryptsvc services...           ║", 40);

    ProcessResult ps_result = runPowerShell(ps_script, sak::kTimeoutDismCheckMs);

    if (ps_result.timed_out || isCancelled()) {
        if (isCancelled()) {
            emitCancelledResult("Cache clearing cancelled", start_time);
        } else {
            emitFailedResult("Operation timed out", ps_result.std_err, start_time);
        }
        return;
    }

    Q_EMIT executionProgress("║ Clearing SoftwareDistribution and catroot2...                ║",
        60);

    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    CacheCleanupResult parsed = parseCacheCleanupOutput(ps_result.std_out, ps_result.std_err);

    Q_EMIT executionProgress("╠════════════════════════════════════════════════════════════════╣",
        80);

    ExecutionResult result;
    Q_ASSERT(!result.success);  // verify default init
    result.duration_ms = duration_ms;
    result.bytes_processed = parsed.total_cleared;
    result.files_processed = parsed.paths_cleared;

    if (parsed.services_stopped == 3 && parsed.services_started == 3 && parsed.paths_cleared > 0) {
        result.success = true;
        result.message = QString("Cleared %1 from Windows Update cache")
            .arg(formatFileSize(parsed.total_cleared));
        result.log = buildSuccessLog(parsed, duration_ms);
        Q_ASSERT(result.duration_ms >= 0);
        finishWithResult(result, ActionStatus::Success);
    } else {
        result.success = false;
        result.message = "Failed to clear Windows Update cache";
        result.log = buildFailureLog(parsed);
        finishWithResult(result, ActionStatus::Failed);
    }
}

// ============================================================================
// Private Helpers
// ============================================================================

QString ClearWindowsUpdateCacheAction::buildCacheCleanupScript() const
{
    return buildServiceStopScript() + buildCachePurgeScript() + buildServiceStartScript();
}

QString ClearWindowsUpdateCacheAction::buildServiceStopScript() const
{
    return QString(
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
        "    $size = (Get-ChildItem -Path $Path -Recurse -File -ErrorAction SilentlyContinue | "
        "Measure-Object -Property Length -Sum).Sum\n"
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
    );
}

QString ClearWindowsUpdateCacheAction::buildCachePurgeScript() const
{
    return QString(
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
        "                    Rename-Item -Path $path -NewName \"catroot2.bak_$timestamp\" -Force "
        "-ErrorAction Stop\n"
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
    );
}

QString ClearWindowsUpdateCacheAction::buildServiceStartScript() const
{
    return QString(
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
        "    Write-Output "
        "\"SERVICE:$svc|$($results[\"${svc}_InitialStatus\"])|$($results[\"${svc}_Stopped\"])|$($results[\"${svc}_Started\"])\"\n"  // NOLINT(line-length)
        "}\n"
        "\n"
        "foreach ($path in $paths) {\n"
        "    $pathName = Split-Path $path -Leaf\n"
        "    $before = $results[\"${pathName}_Before\"]\n"
        "    $after = $results[\"${pathName}_After\"]\n"
        "    $cleared = $before - $after\n"
        "    Write-Output "
        "\"PATH:$pathName|$(Format-Bytes $before)|$(Format-Bytes $cleared)|$($results[\"${pathName}_Cleared\"])\"\n"  // NOLINT(line-length)
        "}\n"
    );
}

ClearWindowsUpdateCacheAction::CacheCleanupResult
ClearWindowsUpdateCacheAction::parseCacheCleanupOutput(const QString& output,
    const QString& std_err) const
{
    CacheCleanupResult parsed;
    parsed.total_before = 0;
    parsed.total_cleared = 0;
    parsed.paths_cleared = 0;
    parsed.services_stopped = 0;
    parsed.services_started = 0;

    if (!std_err.trimmed().isEmpty()) {
        parsed.errors.append(std_err.trimmed());
    }

    const QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith("TOTAL_BEFORE:")) {
            parsed.total_before = trimmed.mid(13).toLongLong();
        } else if (trimmed.startsWith("TOTAL_CLEARED:")) {
            parsed.total_cleared = trimmed.mid(14).toLongLong();
        } else if (trimmed.startsWith("PATHS_CLEARED:")) {
            parsed.paths_cleared = trimmed.mid(14).toInt();
        } else if (trimmed.startsWith("SERVICES_STOPPED:")) {
            parsed.services_stopped = trimmed.mid(17).toInt();
        } else if (trimmed.startsWith("SERVICES_STARTED:")) {
            parsed.services_started = trimmed.mid(17).toInt();
        } else if (trimmed.startsWith("SERVICE:")) {
            parsed.service_details.append(trimmed.mid(8));
        } else if (trimmed.startsWith("PATH:")) {
            parsed.path_details.append(trimmed.mid(5));
        } else if (trimmed.contains("_ERROR:")) {
            parsed.errors.append(trimmed);
        }
    }

    return parsed;
}

QString ClearWindowsUpdateCacheAction::buildSuccessLog(const CacheCleanupResult& parsed,
    qint64 duration_ms) const
{
    const QString size_str = formatFileSize(parsed.total_cleared);

    QString log = "╔════════════════════════════════════════════════════════════════╗\n"
                  "║   WINDOWS UPDATE CACHE CLEARING - RESULTS                    ║\n"
                  "╠════════════════════════════════════════════════════════════════╣\n";

    log += QString("║ Total Space Freed: %1\n").arg(size_str).leftJustified(66) + "║\n";
    log += QString("║ Cache Paths Cleared: %1/3\n").arg(parsed.paths_cleared).leftJustified(66) +
        "║\n";
    log += "╠════════════════════════════════════════════════════════════════╣\n";
    log += "║ SERVICES MANAGED:                                              ║\n";

    for (const QString& svc_detail : parsed.service_details) {
        const QStringList parts = svc_detail.split('|');
        if (parts.size() >= 4) {
            log += QString("║ • %1: %2 → Stopped → Restarted\n").arg(parts[0],
                parts[1]).leftJustified(66) + "║\n";
        }
    }

    log += "╠════════════════════════════════════════════════════════════════╣\n";
    log += "║ CACHE DIRECTORIES:                                             ║\n";

    for (const QString& path_detail : parsed.path_details) {
        const QStringList parts = path_detail.split('|');
        if (parts.size() >= 4 && parts[3] == "True") {
            log += QString("║ • %1: %2 cleared\n").arg(parts[0],
                parts[2]).leftJustified(66) + "║\n";
        }
    }

    log += "╠════════════════════════════════════════════════════════════════╣\n";
    log += QString("║ Completed in: %1 seconds\n").arg(duration_ms / 1000.0, 0, 'f',
        2).leftJustified(66) + "║\n";
    log += "╚════════════════════════════════════════════════════════════════╝\n";

    return log;
}

QString ClearWindowsUpdateCacheAction::buildFailureLog(const CacheCleanupResult& parsed) const
{
    QString log = "╔════════════════════════════════════════════════════════════════╗\n"
                  "║   WINDOWS UPDATE CACHE CLEARING - RESULTS                    ║\n"
                  "╠════════════════════════════════════════════════════════════════╣\n";

    log += "║ Status: Operation Failed                                       ║\n";
    log += "╠════════════════════════════════════════════════════════════════╣\n";
    log += QString("║ Services Stopped: %1/3\n").arg(parsed.services_stopped).leftJustified(66) +
        "║\n";
    log += QString("║ Services Started: %1/3\n").arg(parsed.services_started).leftJustified(66) +
        "║\n";
    log += QString("║ Paths Cleared: %1/3\n").arg(parsed.paths_cleared).leftJustified(66) + "║\n";

    if (!parsed.errors.isEmpty()) {
        log += "╠════════════════════════════════════════════════════════════════╣\n";
        log += "║ ERRORS:                                                        ║\n";
        for (const QString& error : parsed.errors) {
            log += QString("║ %1\n").arg(error).leftJustified(66) + "║\n";
        }
    }

    log += "╠════════════════════════════════════════════════════════════════╣\n";
    log += "║ Action Required: Run as Administrator                          ║\n";
    log += "╚════════════════════════════════════════════════════════════════╝\n";

    return log;
}

} // namespace sak

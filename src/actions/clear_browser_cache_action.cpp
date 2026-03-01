// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file clear_browser_cache_action.cpp
/// @brief Implements browser cache cleanup for major web browsers

#include "sak/actions/clear_browser_cache_action.h"
#include "sak/process_runner.h"

#include <QDir>
#include <QDirIterator>
#include <QStandardPaths>
#include <QDateTime>

namespace sak {

ClearBrowserCacheAction::ClearBrowserCacheAction()
    : QuickAction(nullptr) {
}

void ClearBrowserCacheAction::scan() {
    setStatus(ActionStatus::Scanning);

    auto dirSize = [this](const QString& path, qint64& files) -> qint64 {
        qint64 total = 0;
        QDir dir(path);
        if (!dir.exists()) {
            return 0;
        }

        QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            if (isCancelled()) {
                break;
            }
            it.next();
            total += it.fileInfo().size();
            files++;
        }
        return total;
    };

    struct CachePath {
        QString name;
        QString path;
    };

    QVector<CachePath> cache_paths = {
        {"Chrome", QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/Google/Chrome/User Data/Default/Cache"},
        {"Chrome Code Cache", QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/Google/Chrome/User Data/Default/Code Cache"},
        {"Edge", QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/Microsoft/Edge/User Data/Default/Cache"},
        {"Edge Code Cache", QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/Microsoft/Edge/User Data/Default/Code Cache"},
        {"Brave", QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/BraveSoftware/Brave-Browser/User Data/Default/Cache"},
        {"Brave Code Cache", QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/BraveSoftware/Brave-Browser/User Data/Default/Code Cache"},
        {"Vivaldi", QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/Vivaldi/User Data/Default/Cache"},
        {"Vivaldi Code Cache", QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/Vivaldi/User Data/Default/Code Cache"},
        {"Opera", QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/Opera Software/Opera Stable/Cache"},
        {"Opera Code Cache", QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/Opera Software/Opera Stable/Code Cache"}
    };

    qint64 total_bytes = 0;
    qint64 total_files = 0;
    int locations = 0;

    Q_EMIT scanProgress("Scanning browser cache locations...");

    for (const auto& cache : cache_paths) {
        qint64 files = 0;
        qint64 bytes = dirSize(cache.path, files);
        if (bytes > 0) {
            total_bytes += bytes;
            total_files += files;
            locations++;
        }
    }

    // Firefox profiles
    QString ff_profiles_path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/Mozilla/Firefox/Profiles";
    QDir profiles_dir(ff_profiles_path);
    if (profiles_dir.exists()) {
        const auto profiles = profiles_dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const auto& profile : profiles) {
            qint64 files = 0;
            qint64 bytes = dirSize(profile.absoluteFilePath() + "/cache2", files);
            if (bytes > 0) {
                total_bytes += bytes;
                total_files += files;
                locations++;
            }
        }
    }

    ScanResult result;
    result.applicable = total_bytes > 0;
    result.bytes_affected = total_bytes;
    result.files_count = total_files;
    result.estimated_duration_ms = std::max<qint64>(3000, total_files * 3);

    if (result.applicable) {
        double mb = total_bytes / (1024.0 * 1024.0);
        result.summary = QString("Cache size: %1 MB").arg(mb, 0, 'f', 1);
        result.details = QString("Locations: %1").arg(locations);
    } else {
        result.summary = "No browser caches found";
        result.details = "Caches are already minimal or browsers not installed";
    }

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void ClearBrowserCacheAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("Browser cache clearing cancelled");
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    Q_EMIT executionProgress("╔════════════════════════════════════════════════════════════════╗", 0);
    Q_EMIT executionProgress("║          BROWSER CACHE CLEARING - ENTERPRISE MODE             ║", 0);
    Q_EMIT executionProgress("╠════════════════════════════════════════════════════════════════╣", 0);
    
    QString ps_script = buildCacheClearingScript();
    
    Q_EMIT executionProgress("║ Detecting browser processes and cache locations...           ║", 20);
    
    ProcessResult ps = runPowerShell(ps_script, 180000);

    Q_EMIT executionProgress("║ Calculating cache sizes before clearing...                   ║", 40);

    if (ps.timed_out || isCancelled()) {
        if (isCancelled()) {
            emitCancelledResult("Cache clearing cancelled", start_time);
        } else {
            emitFailedResult("Operation timed out after 3 minutes", {}, start_time);
        }
        return;
    }
    
    Q_EMIT executionProgress("║ Processing results and generating report...                   ║", 80);
    
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    BrowserCacheResult parsed = parseCacheOutput(ps.std_out);
    
    Q_EMIT executionProgress("╠════════════════════════════════════════════════════════════════╣", 90);
    
    ExecutionResult result;
    result.duration_ms = duration_ms;
    
    if (parsed.cleared_count > 0) {
        result.success = true;
        result.message = QString("Successfully cleared %1 browser(s)").arg(parsed.cleared_count);
        result.log = buildSuccessLog(parsed, ps.std_err, duration_ms);
        finishWithResult(result, ActionStatus::Success);
    } else {
        result.success = false;
        result.message = parsed.blocked_count > 0
            ? QString("All %1 detected browser(s) are currently running").arg(parsed.blocked_count)
            : "No browser caches found on this system";
        result.log = buildFailureLog(parsed);
        finishWithResult(result, ActionStatus::Failed);
    }
}

// ============================================================================
// Private Helpers
// ============================================================================

QString ClearBrowserCacheAction::buildCacheClearingScript() const
{
    return QString(
        "$ErrorActionPreference = 'SilentlyContinue'\n"
        "$results = @()\n"
        "$totalBefore = 0\n"
        "$totalAfter = 0\n"
        "$clearedBrowsers = @()\n"
        "$blockedBrowsers = @()\n"
        "\n"
        "# Helper function to calculate directory size\n"
        "function Get-DirectorySize {\n"
        "    param([string]$Path)\n"
        "    if (Test-Path $Path) {\n"
        "        $size = (Get-ChildItem -Path $Path -Recurse -File -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum\n"
        "        if ($null -eq $size) { return 0 }\n"
        "        return $size\n"
        "    }\n"
        "    return 0\n"
        "}\n"
        "\n"
        "# Helper function to format bytes\n"
        "function Format-Bytes {\n"
        "    param([long]$Bytes)\n"
        "    if ($Bytes -ge 1GB) { return '{0:N2} GB' -f ($Bytes / 1GB) }\n"
        "    if ($Bytes -ge 1MB) { return '{0:N2} MB' -f ($Bytes / 1MB) }\n"
        "    if ($Bytes -ge 1KB) { return '{0:N2} KB' -f ($Bytes / 1KB) }\n"
        "    return '{0} bytes' -f $Bytes\n"
        "}\n"
        "\n"
        "# Browser configurations (Chromium-based and Firefox)\n"
        "$browsers = @(\n"
        "    @{Name='Chrome'; Process='chrome'; Paths=@(\"$env:LOCALAPPDATA\\Google\\Chrome\\User Data\\Default\\Cache\", \"$env:LOCALAPPDATA\\Google\\Chrome\\User Data\\Default\\Code Cache\")},\n"
        "    @{Name='Edge'; Process='msedge'; Paths=@(\"$env:LOCALAPPDATA\\Microsoft\\Edge\\User Data\\Default\\Cache\", \"$env:LOCALAPPDATA\\Microsoft\\Edge\\User Data\\Default\\Code Cache\")},\n"
        "    @{Name='Brave'; Process='brave'; Paths=@(\"$env:LOCALAPPDATA\\BraveSoftware\\Brave-Browser\\User Data\\Default\\Cache\", \"$env:LOCALAPPDATA\\BraveSoftware\\Brave-Browser\\User Data\\Default\\Code Cache\")},\n"
        "    @{Name='Opera'; Process='opera'; Paths=@(\"$env:APPDATA\\Opera Software\\Opera Stable\\Cache\", \"$env:APPDATA\\Opera Software\\Opera Stable\\Code Cache\")},\n"
        "    @{Name='Vivaldi'; Process='vivaldi'; Paths=@(\"$env:LOCALAPPDATA\\Vivaldi\\User Data\\Default\\Cache\", \"$env:LOCALAPPDATA\\Vivaldi\\User Data\\Default\\Code Cache\")}\n"
        ")\n"
        "\n"
        "foreach ($browser in $browsers) {\n"
        "    $running = Get-Process -Name $browser.Process -ErrorAction SilentlyContinue\n"
        "    $browserSizeBefore = 0\n"
        "    $browserSizeAfter = 0\n"
        "    \n"
        "    if ($running) {\n"
        "        $blockedBrowsers += $browser.Name\n"
        "        continue\n"
        "    }\n"
        "    \n"
        "    $foundCache = $false\n"
        "    foreach ($path in $browser.Paths) {\n"
        "        if (Test-Path $path) {\n"
        "            $foundCache = $true\n"
        "            $sizeBefore = Get-DirectorySize -Path $path\n"
        "            $browserSizeBefore += $sizeBefore\n"
        "            \n"
        "            Remove-Item -Path \"$path\\*\" -Recurse -Force -ErrorAction SilentlyContinue\n"
        "            Start-Sleep -Milliseconds 100\n"
        "            \n"
        "            $sizeAfter = Get-DirectorySize -Path $path\n"
        "            $browserSizeAfter += $sizeAfter\n"
        "        }\n"
        "    }\n"
        "    \n"
        "    if ($foundCache) {\n"
        "        $cleared = $browserSizeBefore - $browserSizeAfter\n"
        "        $totalBefore += $browserSizeBefore\n"
        "        $totalAfter += $browserSizeAfter\n"
        "        $clearedBrowsers += $browser.Name\n"
        "        $results += \"$($browser.Name): Cleared $(Format-Bytes $cleared)\"\n"
        "    }\n"
        "}\n"
        "\n"
        "# Firefox special handling (profiles-based)\n"
        "$ffProfilesPath = \"$env:APPDATA\\Mozilla\\Firefox\\Profiles\"\n"
        "if (Test-Path $ffProfilesPath) {\n"
        "    $ffRunning = Get-Process -Name 'firefox' -ErrorAction SilentlyContinue\n"
        "    if ($ffRunning) {\n"
        "        $blockedBrowsers += 'Firefox'\n"
        "    } else {\n"
        "        $ffSizeBefore = 0\n"
        "        $ffSizeAfter = 0\n"
        "        $profiles = Get-ChildItem -Path $ffProfilesPath -Directory\n"
        "        foreach ($profile in $profiles) {\n"
        "            $cachePath = Join-Path $profile.FullName 'cache2'\n"
        "            if (Test-Path $cachePath) {\n"
        "                $sizeBefore = Get-DirectorySize -Path $cachePath\n"
        "                $ffSizeBefore += $sizeBefore\n"
        "                \n"
        "                Remove-Item -Path \"$cachePath\\*\" -Recurse -Force -ErrorAction SilentlyContinue\n"
        "                Start-Sleep -Milliseconds 100\n"
        "                \n"
        "                $sizeAfter = Get-DirectorySize -Path $cachePath\n"
        "                $ffSizeAfter += $sizeAfter\n"
        "            }\n"
        "        }\n"
        "        if ($ffSizeBefore -gt 0) {\n"
        "            $cleared = $ffSizeBefore - $ffSizeAfter\n"
        "            $totalBefore += $ffSizeBefore\n"
        "            $totalAfter += $ffSizeAfter\n"
        "            $clearedBrowsers += 'Firefox'\n"
        "            $results += \"Firefox: Cleared $(Format-Bytes $cleared) across $($profiles.Count) profile(s)\"\n"
        "        }\n"
        "    }\n"
        "}\n"
        "\n"
        "# Output results\n"
        "Write-Output \"CLEARED:$($clearedBrowsers.Count)\"\n"
        "Write-Output \"BLOCKED:$($blockedBrowsers.Count)\"\n"
        "Write-Output \"TOTAL_BEFORE:$totalBefore\"\n"
        "Write-Output \"TOTAL_CLEARED:$($totalBefore - $totalAfter)\"\n"
        "if ($clearedBrowsers.Count -gt 0) {\n"
        "    Write-Output \"BROWSERS:$($clearedBrowsers -join ',')\"\n"
        "}\n"
        "if ($blockedBrowsers.Count -gt 0) {\n"
        "    Write-Output \"BLOCKED_LIST:$($blockedBrowsers -join ',')\"\n"
        "}\n"
        "foreach ($result in $results) {\n"
        "    Write-Output \"DETAIL:$result\"\n"
        "}\n"
    );
}

ClearBrowserCacheAction::BrowserCacheResult
ClearBrowserCacheAction::parseCacheOutput(const QString& output) const
{
    BrowserCacheResult parsed;
    const QStringList lines = output.split('\n', Qt::SkipEmptyParts);

    for (const QString& line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith("CLEARED:")) {
            parsed.cleared_count = trimmed.mid(8).toInt();
        } else if (trimmed.startsWith("BLOCKED:")) {
            parsed.blocked_count = trimmed.mid(8).toInt();
        } else if (trimmed.startsWith("TOTAL_BEFORE:")) {
            parsed.size_before = trimmed.mid(13).toLongLong();
        } else if (trimmed.startsWith("TOTAL_CLEARED:")) {
            parsed.size_cleared = trimmed.mid(14).toLongLong();
        } else if (trimmed.startsWith("BROWSERS:")) {
            parsed.cleared_browsers = trimmed.mid(9).split(',', Qt::SkipEmptyParts);
        } else if (trimmed.startsWith("BLOCKED_LIST:")) {
            parsed.blocked_browsers = trimmed.mid(13).split(',', Qt::SkipEmptyParts);
        } else if (trimmed.startsWith("DETAIL:")) {
            parsed.details.append(trimmed.mid(7));
        }
    }

    return parsed;
}

QString ClearBrowserCacheAction::buildSuccessLog(const BrowserCacheResult& parsed,
                                                  const QString& stderr_output,
                                                  qint64 duration_ms) const
{
    const QString size_str = formatFileSize(parsed.size_cleared);

    QString log = "╔════════════════════════════════════════════════════════════════╗\n"
                  "║          BROWSER CACHE CLEARING - RESULTS                     ║\n"
                  "╠════════════════════════════════════════════════════════════════╣\n";

    log += QString("║ Total Cleared: %1\n").arg(size_str).leftJustified(66) + "║\n";
    log += QString("║ Browsers Processed: %1/%2\n").arg(parsed.cleared_count).arg(parsed.cleared_count + parsed.blocked_count).leftJustified(66) + "║\n";
    log += "╠════════════════════════════════════════════════════════════════╣\n";

    for (const QString& detail : parsed.details) {
        log += QString("║ %1\n").arg(detail).leftJustified(66) + "║\n";
    }

    if (parsed.blocked_count > 0) {
        log += "╠════════════════════════════════════════════════════════════════╣\n";
        log += QString("║ Skipped (%1 running): %2\n").arg(parsed.blocked_count).arg(parsed.blocked_browsers.join(", ")).leftJustified(66) + "║\n";
    }

    if (!stderr_output.trimmed().isEmpty()) {
        log += "╠════════════════════════════════════════════════════════════════╣\n";
        log += "║ Warnings:                                                       ║\n";
        const QStringList warn_lines = stderr_output.split('\n', Qt::SkipEmptyParts);
        const int warn_max = std::min(3, static_cast<int>(warn_lines.size()));
        for (int i = 0; i < warn_max; ++i) {
            log += QString("║ %1\n").arg(warn_lines[i].left(66)).leftJustified(66) + "║\n";
        }
    }

    log += "╠════════════════════════════════════════════════════════════════╣\n";
    log += QString("║ Completed in: %1 seconds\n").arg(duration_ms / 1000.0, 0, 'f', 2).leftJustified(66) + "║\n";
    log += "╚════════════════════════════════════════════════════════════════╝\n";

    return log;
}

QString ClearBrowserCacheAction::buildFailureLog(const BrowserCacheResult& parsed) const
{
    QString log = "╔════════════════════════════════════════════════════════════════╗\n"
                  "║          BROWSER CACHE CLEARING - RESULTS                     ║\n"
                  "╠════════════════════════════════════════════════════════════════╣\n";

    if (parsed.blocked_count > 0) {
        log += "║ Cannot clear cache - browsers running:                       ║\n";
        log += QString("║ %1\n").arg(parsed.blocked_browsers.join(", ")).leftJustified(66) + "║\n";
        log += "╠════════════════════════════════════════════════════════════════╣\n";
        log += "║ Action Required: Close all browsers and retry                ║\n";
    } else {
        log += "║ No cache directories detected                                  ║\n";
        log += "╠════════════════════════════════════════════════════════════════╣\n";
        log += "║ Checked browsers: Chrome, Edge, Firefox, Brave, Opera, Vivaldi ║\n";
    }

    log += "╚════════════════════════════════════════════════════════════════╝\n";

    return log;
}

} // namespace sak

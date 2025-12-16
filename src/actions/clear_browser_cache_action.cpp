// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/clear_browser_cache_action.h"

#include <QDir>
#include <QDirIterator>
#include <QStandardPaths>
#include <QProcess>
#include <QDateTime>

namespace sak {

ClearBrowserCacheAction::ClearBrowserCacheAction()
    : QuickAction(nullptr) {
}

void ClearBrowserCacheAction::scan() {
    // Scan is no longer used - actions execute immediately
    setStatus(ActionStatus::Ready);
    ScanResult result;
    result.applicable = true;
    result.summary = "Ready to clear browser caches";
    setScanResult(result);
    Q_EMIT scanComplete(result);
}

void ClearBrowserCacheAction::execute() {
    if (isCancelled()) {
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    Q_EMIT executionProgress("╔════════════════════════════════════════════════════════════════╗", 0);
    Q_EMIT executionProgress("║          BROWSER CACHE CLEARING - ENTERPRISE MODE             ║", 0);
    Q_EMIT executionProgress("╠════════════════════════════════════════════════════════════════╣", 0);
    
    // Comprehensive PowerShell script for enterprise-grade cache clearing
    QString ps_script = QString(
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
    
    Q_EMIT executionProgress("║ Detecting browser processes and cache locations...           ║", 20);
    
    QProcess ps;
    ps.start("powershell.exe", QStringList() << "-NoProfile" << "-ExecutionPolicy" << "Bypass" << "-Command" << ps_script);
    
    Q_EMIT executionProgress("║ Calculating cache sizes before clearing...                   ║", 40);
    
    bool finished = ps.waitForFinished(180000); // 3 minute timeout for size calculation
    
    if (!finished || isCancelled()) {
        ps.kill();
        ExecutionResult result;
        result.success = false;
        result.message = isCancelled() ? "Cache clearing cancelled" : "Operation timed out after 3 minutes";
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
    int cleared_count = 0;
    int blocked_count = 0;
    qint64 size_before = 0;
    qint64 size_cleared = 0;
    QStringList cleared_browsers;
    QStringList blocked_browsers;
    QStringList details;
    
    for (const QString& line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.startsWith("CLEARED:")) {
            cleared_count = trimmed.mid(8).toInt();
        } else if (trimmed.startsWith("BLOCKED:")) {
            blocked_count = trimmed.mid(8).toInt();
        } else if (trimmed.startsWith("TOTAL_BEFORE:")) {
            size_before = trimmed.mid(13).toLongLong();
        } else if (trimmed.startsWith("TOTAL_CLEARED:")) {
            size_cleared = trimmed.mid(14).toLongLong();
        } else if (trimmed.startsWith("BROWSERS:")) {
            cleared_browsers = trimmed.mid(9).split(',', Qt::SkipEmptyParts);
        } else if (trimmed.startsWith("BLOCKED_LIST:")) {
            blocked_browsers = trimmed.mid(13).split(',', Qt::SkipEmptyParts);
        } else if (trimmed.startsWith("DETAIL:")) {
            details.append(trimmed.mid(7));
        }
    }
    
    Q_EMIT executionProgress("╠════════════════════════════════════════════════════════════════╣", 90);
    
    ExecutionResult result;
    result.duration_ms = duration_ms;
    
    QString message;
    QString log_output = "╔════════════════════════════════════════════════════════════════╗\n";
    log_output += "║          BROWSER CACHE CLEARING - RESULTS                     ║\n";
    log_output += "╠════════════════════════════════════════════════════════════════╣\n";
    
    if (cleared_count > 0) {
        result.success = true;
        
        // Format size for display
        QString size_str;
        if (size_cleared >= 1073741824LL) { // 1GB
            size_str = QString::number(size_cleared / 1073741824.0, 'f', 2) + " GB";
        } else if (size_cleared >= 1048576LL) { // 1MB
            size_str = QString::number(size_cleared / 1048576.0, 'f', 2) + " MB";
        } else if (size_cleared >= 1024LL) { // 1KB
            size_str = QString::number(size_cleared / 1024.0, 'f', 2) + " KB";
        } else {
            size_str = QString::number(size_cleared) + " bytes";
        }
        
        message = QString("Successfully cleared %1 browser(s)").arg(cleared_count);
        log_output += QString("║ Total Cleared: %1\n").arg(size_str).leftJustified(66) + "║\n";
        log_output += QString("║ Browsers Processed: %1/%2\n").arg(cleared_count).arg(cleared_count + blocked_count).leftJustified(66) + "║\n";
        log_output += "╠════════════════════════════════════════════════════════════════╣\n";
        
        for (const QString& detail : details) {
            log_output += QString("║ %1\n").arg(detail).leftJustified(66) + "║\n";
        }
        
        if (blocked_count > 0) {
            log_output += "╠════════════════════════════════════════════════════════════════╣\n";
            log_output += QString("║ Skipped (%1 running): %2\n").arg(blocked_count).arg(blocked_browsers.join(", ")).leftJustified(66) + "║\n";
        }
        
        log_output += "╠════════════════════════════════════════════════════════════════╣\n";
        log_output += QString("║ Completed in: %1 seconds\n").arg(duration_ms / 1000.0, 0, 'f', 2).leftJustified(66) + "║\n";
        log_output += "╚════════════════════════════════════════════════════════════════╝\n";
        
        result.message = message;
        result.log = log_output;
        setStatus(ActionStatus::Success);
    } else {
        result.success = false;
        
        if (blocked_count > 0) {
            message = QString("All %1 detected browser(s) are currently running").arg(blocked_count);
            log_output += QString("║ Cannot clear cache - browsers running:                       ║\n");
            log_output += QString("║ %1\n").arg(blocked_browsers.join(", ")).leftJustified(66) + "║\n";
            log_output += "╠════════════════════════════════════════════════════════════════╣\n";
            log_output += QString("║ Action Required: Close all browsers and retry                ║\n");
        } else {
            message = "No browser caches found on this system";
            log_output += QString("║ No cache directories detected                                  ║\n");
            log_output += "╠════════════════════════════════════════════════════════════════╣\n";
            log_output += QString("║ Checked browsers: Chrome, Edge, Firefox, Brave, Opera, Vivaldi ║\n");
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

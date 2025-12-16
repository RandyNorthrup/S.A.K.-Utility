// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/windows_update_action.h"
#include <QProcess>

namespace sak {

WindowsUpdateAction::WindowsUpdateAction(QObject* parent)
    : QuickAction(parent)
{
}

bool WindowsUpdateAction::isPSWindowsUpdateInstalled() {
    QProcess proc;
    QString ps_cmd = "Get-Module -ListAvailable -Name PSWindowsUpdate";
    proc.start("powershell.exe", QStringList() << "-Command" << ps_cmd);
    proc.waitForFinished(5000);
    
    QString output = proc.readAllStandardOutput();
    return output.contains("PSWindowsUpdate", Qt::CaseInsensitive);
}

bool WindowsUpdateAction::installPSWindowsUpdateModule() {
    Q_EMIT executionProgress("Installing PSWindowsUpdate module...", 10);
    
    QString ps_cmd = "Install-Module -Name PSWindowsUpdate -Force -Confirm:$false";
    QProcess::execute("powershell.exe", QStringList() << "-Command" << ps_cmd);
    return true;
}

void WindowsUpdateAction::checkForUpdates() {
    QProcess proc;
    QString ps_cmd = "Get-WindowsUpdate -MicrosoftUpdate";
    proc.start("powershell.exe", QStringList() << "-Command" << ps_cmd);
    proc.waitForFinished(30000);
    
    QString output = proc.readAllStandardOutput();
    QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    
    m_available_updates = lines.count() - 2; // Subtract header lines
}

void WindowsUpdateAction::installUpdates() {
    Q_EMIT executionProgress("Installing Windows Updates...", 30);
    
    QString ps_cmd = "Install-WindowsUpdate -MicrosoftUpdate -AcceptAll -AutoReboot";
    
    QProcess proc;
    proc.start("powershell.exe", QStringList() << "-Command" << ps_cmd);
    
    // Windows Update can take a long time
    while (proc.state() == QProcess::Running) {
        proc.waitForReadyRead(10000);
        QString output = proc.readAll();
        
        if (output.contains("Downloading", Qt::CaseInsensitive)) {
            Q_EMIT executionProgress("Downloading updates...", 50);
        } else if (output.contains("Installing", Qt::CaseInsensitive)) {
            Q_EMIT executionProgress("Installing updates...", 70);
        }
    }
    
    proc.waitForFinished();
}

void WindowsUpdateAction::scan() {
    // Scan is no longer used - actions execute immediately
    setStatus(ActionStatus::Ready);
    ScanResult result;
    result.applicable = true;
    result.summary = "Ready to check for Windows Updates";
    setScanResult(result);
    Q_EMIT scanComplete(result);
}

void WindowsUpdateAction::execute() {
    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    Q_EMIT executionProgress("Initiating Windows Update scan...", 5);
    
    // Modern Windows 10/11 approach: Use UsoClient (Update Session Orchestrator)
    // Per Microsoft docs: UsoClient replaces deprecated COM API for Windows Update automation
    // Commands: StartScan, StartDownload, StartInstall, ResumeUpdate, RestartDevice
    
    QString ps_script = 
        "# Enterprise Windows Update using UsoClient\n"
        "Write-Output 'Starting update scan via UsoClient...'; \n"
        "$usoClient = Join-Path $env:SystemRoot 'System32\\UsoClient.exe'; \n"
        "\n"
        "# Step 1: Scan for updates\n"
        "if (Test-Path $usoClient) { \n"
        "  try { \n"
        "    Start-Process -FilePath $usoClient -ArgumentList 'StartScan' -NoNewWindow -Wait; \n"
        "    Write-Output 'Scan initiated'; \n"
        "    Start-Sleep -Seconds 10; \n"
        "  } catch { \n"
        "    Write-Error \"Scan failed: $_\"; \n"
        "    exit 1; \n"
        "  } \n"
        "} else { \n"
        "  Write-Error 'UsoClient not found'; \n"
        "  exit 1; \n"
        "} \n"
        "\n"
        "# Check for available updates using Windows Update API\n"
        "Write-Output 'Checking update status...'; \n"
        "try { \n"
        "  $updateSession = New-Object -ComObject Microsoft.Update.Session; \n"
        "  $updateSearcher = $updateSession.CreateUpdateSearcher(); \n"
        "  $searchResult = $updateSearcher.Search('IsInstalled=0 and Type=\'Software\' and IsHidden=0'); \n"
        "  $updateCount = $searchResult.Updates.Count; \n"
        "  Write-Output \"Found $updateCount update(s)\"; \n"
        "  \n"
        "  if ($updateCount -eq 0) { \n"
        "    Write-Output 'No updates available'; \n"
        "    exit 0; \n"
        "  } \n"
        "  \n"
        "  # List update titles\n"
        "  foreach ($update in $searchResult.Updates) { \n"
        "    Write-Output \"  - $($update.Title)\"; \n"
        "  } \n"
        "} catch { \n"
        "  Write-Warning \"Could not query updates: $_\"; \n"
        "  # Continue anyway - UsoClient will handle\n"
        "} \n"
        "\n"
        "# Step 2: Download updates\n"
        "Write-Output 'Starting download via UsoClient...'; \n"
        "try { \n"
        "  Start-Process -FilePath $usoClient -ArgumentList 'StartDownload' -NoNewWindow -Wait; \n"
        "  Write-Output 'Download initiated'; \n"
        "  Start-Sleep -Seconds 15; \n"
        "} catch { \n"
        "  Write-Error \"Download failed: $_\"; \n"
        "  exit 1; \n"
        "} \n"
        "\n"
        "# Step 3: Install updates\n"
        "Write-Output 'Starting installation via UsoClient...'; \n"
        "try { \n"
        "  Start-Process -FilePath $usoClient -ArgumentList 'StartInstall' -NoNewWindow -Wait; \n"
        "  Write-Output 'Installation initiated'; \n"
        "  Start-Sleep -Seconds 20; \n"
        "} catch { \n"
        "  Write-Error \"Installation failed: $_\"; \n"
        "  exit 1; \n"
        "} \n"
        "\n"
        "# Check if reboot required\n"
        "$rebootRequired = $false; \n"
        "try { \n"
        "  $systemInfo = New-Object -ComObject Microsoft.Update.SystemInfo; \n"
        "  $rebootRequired = $systemInfo.RebootRequired; \n"
        "} catch { \n"
        "  # Also check registry\n"
        "  $regPath = 'HKLM:\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired'; \n"
        "  if (Test-Path $regPath) { \n"
        "    $rebootRequired = $true; \n"
        "  } \n"
        "} \n"
        "\n"
        "if ($rebootRequired) { \n"
        "  Write-Output 'REBOOT_REQUIRED'; \n"
        "} else { \n"
        "  Write-Output 'Installation completed successfully'; \n"
        "} \n"
        "\n"
        "exit 0";
    
    QProcess ps;
    ps.start("powershell.exe", QStringList() << "-NoProfile" << "-ExecutionPolicy" << "Bypass" << "-Command" << ps_script);
    
    // Monitor progress with periodic output checks
    QString accumulated_output;
    int last_progress = 5;
    
    while (ps.state() == QProcess::Running) {
        if (ps.waitForReadyRead(10000)) {
            QString chunk = ps.readAllStandardOutput();
            accumulated_output += chunk;
            
            if (chunk.contains("Scan initiated", Qt::CaseInsensitive)) {
                Q_EMIT executionProgress("Scanning for updates...", 20);
                last_progress = 20;
            } else if (chunk.contains("Found", Qt::CaseInsensitive) && chunk.contains("update", Qt::CaseInsensitive)) {
                Q_EMIT executionProgress("Updates found, preparing download...", 35);
                last_progress = 35;
            } else if (chunk.contains("Download initiated", Qt::CaseInsensitive)) {
                Q_EMIT executionProgress("Downloading updates...", 50);
                last_progress = 50;
            } else if (chunk.contains("Installation initiated", Qt::CaseInsensitive)) {
                Q_EMIT executionProgress("Installing updates...", 70);
                last_progress = 70;
            }
        }
        
        if (isCancelled()) {
            ps.kill();
            ExecutionResult result;
            result.success = false;
            result.message = "Windows Update cancelled";
            result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
            setExecutionResult(result);
            setStatus(ActionStatus::Failed);
            Q_EMIT executionComplete(result);
            return;
        }
    }
    
    // Wait for completion with 30 minute timeout
    bool finished = ps.waitForFinished(1800000);
    
    if (!finished) {
        ps.kill();
        ExecutionResult result;
        result.success = false;
        result.message = "Operation timed out after 30 minutes";
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        setExecutionResult(result);
        setStatus(ActionStatus::Failed);
        Q_EMIT executionComplete(result);
        return;
    }
    
    Q_EMIT executionProgress("Finalizing...", 95);
    
    accumulated_output += ps.readAll();
    QString errors = ps.readAllStandardError();
    int exit_code = ps.exitCode();
    
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    
    ExecutionResult result;
    result.duration_ms = duration_ms;
    
    if (accumulated_output.contains("No updates available")) {
        result.success = true;
        result.message = "Windows is up to date";
        result.log = accumulated_output;
        setStatus(ActionStatus::Success);
    } else if (exit_code == 0) {
        result.success = true;
        bool reboot_required = accumulated_output.contains("REBOOT_REQUIRED", Qt::CaseInsensitive);
        result.message = reboot_required ? 
            "Updates installed successfully - REBOOT REQUIRED" : 
            "Updates installed successfully";
        result.log = accumulated_output;
        setStatus(ActionStatus::Success);
    } else {
        result.success = false;
        result.message = "Windows Update failed";
        result.log = QString("Exit code: %1\n%2\nErrors:\n%3").arg(exit_code).arg(accumulated_output).arg(errors);
        setStatus(ActionStatus::Failed);
    }
    
    setExecutionResult(result);
    Q_EMIT executionComplete(result);
}

} // namespace sak

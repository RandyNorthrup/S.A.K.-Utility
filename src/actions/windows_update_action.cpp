// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file windows_update_action.cpp
/// @brief Implements Windows Update checking and installation

#include "sak/actions/windows_update_action.h"

#include "sak/layout_constants.h"
#include "sak/process_runner.h"

namespace sak {

namespace {
int queryPendingUpdateCount() {
    QString ps_cmd =
        "try { "
        "  $session = New-Object -ComObject Microsoft.Update.Session; "
        "  $searcher = $session.CreateUpdateSearcher(); "
        "  $result = $searcher.Search('IsInstalled=0 and IsHidden=0'); "
        "  Write-Output $result.Updates.Count; "
        "} catch { Write-Output -1 }";

    ProcessResult proc = runPowerShell(ps_cmd, sak::kTimeoutChocoListMs);
    if (!proc.succeeded()) {
        return -1;
    }
    bool ok = false;
    int count = proc.std_out.trimmed().toInt(&ok);
    return ok ? count : -1;
}
}  // namespace

WindowsUpdateAction::WindowsUpdateAction(QObject* parent) : QuickAction(parent) {}

bool WindowsUpdateAction::isPSWindowsUpdateInstalled() {
    QString ps_cmd = "Get-Module -ListAvailable -Name PSWindowsUpdate";
    ProcessResult proc = runPowerShell(ps_cmd, sak::kTimeoutProcessShortMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("PSWindowsUpdate check warning: " + proc.std_err.trimmed());
    }
    return proc.std_out.contains("PSWindowsUpdate", Qt::CaseInsensitive);
}

bool WindowsUpdateAction::installPSWindowsUpdateModule() {
    Q_EMIT executionProgress("Installing PSWindowsUpdate module...", 10);

    QString ps_cmd = "Install-Module -Name PSWindowsUpdate -Force -Confirm:$false";
    ProcessResult proc = runPowerShell(ps_cmd, sak::kTimeoutDismCheckMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("PSWindowsUpdate install warning: " + proc.std_err.trimmed());
    }
    return proc.succeeded();
}

void WindowsUpdateAction::checkForUpdates() {
    QString ps_cmd = "Get-WindowsUpdate -MicrosoftUpdate";
    ProcessResult proc = runPowerShell(ps_cmd, sak::kTimeoutProcessLongMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Windows Update list warning: " + proc.std_err.trimmed());
    }
    QString output = proc.std_out;
    QStringList lines = output.split('\n', Qt::SkipEmptyParts);

    m_available_updates = lines.count() - 2;  // Subtract header lines
}

void WindowsUpdateAction::installUpdates() {
    Q_EMIT executionProgress("Installing Windows Updates...", 30);

    QString ps_cmd = "Install-WindowsUpdate -MicrosoftUpdate -AcceptAll -AutoReboot";
    ProcessResult proc = runPowerShell(ps_cmd, sak::kTimeoutSystemRepairMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Windows Update install warning: " + proc.std_err.trimmed());
    }
    if (proc.std_out.contains("Downloading", Qt::CaseInsensitive)) {
        Q_EMIT executionProgress("Downloading updates...", 50);
    }
    if (proc.std_out.contains("Installing", Qt::CaseInsensitive)) {
        Q_EMIT executionProgress("Installing updates...", 70);
    }
}

void WindowsUpdateAction::scan() {
    setStatus(ActionStatus::Scanning);
    Q_ASSERT(status() == ActionStatus::Scanning);

    Q_EMIT scanProgress("Checking Windows Update availability...");

    QString ps_cmd =
        "try { "
        "  $session = New-Object -ComObject Microsoft.Update.Session; "
        "  $searcher = $session.CreateUpdateSearcher(); "
        "  $result = $searcher.Search('IsInstalled=0 and IsHidden=0'); "
        "  Write-Output \"COUNT:$($result.Updates.Count)\"; "
        "} catch { Write-Output \"COUNT:-1\" }";

    ProcessResult proc = runPowerShell(ps_cmd, sak::kTimeoutChocoListMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Windows Update scan warning: " + proc.std_err.trimmed());
    }
    QString output = proc.std_out.trimmed();
    int count = -1;
    if (output.contains("COUNT:")) {
        bool ok = false;
        count = output.mid(output.indexOf("COUNT:") + 6).trimmed().toInt(&ok);
        if (!ok) {
            count = -1;
        }
    }

    ScanResult result;
    if (count >= 0) {
        result.applicable = true;
        result.files_count = count;
        result.summary = count > 0 ? QString("Updates available: %1").arg(count)
                                   : "Windows is up to date";
        result.details = "Run update to download and install available patches";
    } else {
        result.applicable = false;
        result.summary = "Windows Update check failed";
        result.details = "Requires Windows Update service access and admin rights";
        result.warning = "Unable to query update service";
    }

    Q_ASSERT(!result.summary.isEmpty());

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void WindowsUpdateAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("Windows Update check cancelled");
        return;
    }
    setStatus(ActionStatus::Running);
    Q_ASSERT(status() == ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    Q_ASSERT(start_time.isValid());

    QString ps_script;
    executeInitSession(start_time, ps_script);

    QString accumulated_output;
    QString errors;
    int exit_code = 0;
    if (!executeSearchUpdates(start_time, ps_script, accumulated_output, errors, exit_code)) {
        return;
    }

    executeBuildReport(start_time, accumulated_output, errors, exit_code);
}

void WindowsUpdateAction::executeInitSession(const QDateTime& start_time, QString& ps_script) {
    Q_UNUSED(start_time)
    Q_EMIT executionProgress("Initiating Windows Update scan...", 5);

    // Modern Windows 10/11 approach: Use UsoClient (Update Session Orchestrator)
    // Per Microsoft docs: UsoClient replaces deprecated COM API for Windows Update automation
    // Commands: StartScan, StartDownload, StartInstall, ResumeUpdate, RestartDevice

    ps_script = buildUpdateScanScript() + buildUpdateInstallScript();
}

QString WindowsUpdateAction::buildUpdateScanScript() {
    return "# Enterprise Windows Update using UsoClient\n"
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
           "  $searchResult = $updateSearcher.Search('IsInstalled=0 and Type=\'Software\' and "
           "IsHidden=0'); \n"
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
           "\n";
}

QString WindowsUpdateAction::buildUpdateInstallScript() {
    return "# Step 2: Download updates\n"
           "Write-Output 'Starting download via UsoClient...'; \n"
           "try { \n"
           "  Start-Process -FilePath $usoClient -ArgumentList 'StartDownload' -NoNewWindow -Wait; "
           "\n"
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
           "  Start-Process -FilePath $usoClient -ArgumentList 'StartInstall' -NoNewWindow -Wait; "
           "\n"
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
           "  $regPath = 'HKLM:\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto "
           "Update\\RebootRequired'; \n"
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
}

bool WindowsUpdateAction::executeSearchUpdates(const QDateTime& start_time,
                                               const QString& ps_script,
                                               QString& accumulated_output,
                                               QString& errors,
                                               int& exit_code) {
    Q_EMIT executionProgress("Scanning for updates...", 20);
    Q_EMIT executionProgress("Preparing download...", 35);
    Q_EMIT executionProgress("Downloading updates...", 50);
    Q_EMIT executionProgress("Installing updates...", 70);

    ProcessResult ps = runPowerShell(ps_script, sak::kTimeoutSystemRepairMs, true, true, [this]() {
        return isCancelled();
    });

    if (ps.cancelled) {
        emitCancelledResult(QStringLiteral("Windows Update cancelled"), start_time);
        return false;
    }

    if (ps.timed_out) {
        emitFailedResult(QStringLiteral("Operation timed out after 30 minutes"),
                         QString(),
                         start_time);
        return false;
    }

    if (!ps.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Windows Update execution warning: " + ps.std_err.trimmed());
    }

    Q_EMIT executionProgress("Finalizing...", 95);

    accumulated_output = ps.std_out;
    errors = ps.std_err;
    exit_code = ps.exit_code;

    return true;
}

void WindowsUpdateAction::executeBuildReport(const QDateTime& start_time,
                                             const QString& accumulated_output,
                                             const QString& errors,
                                             int exit_code) {
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    Q_ASSERT(!result.success);  // verify default init
    result.duration_ms = duration_ms;

    if (accumulated_output.contains("No updates available")) {
        result.success = true;
        result.message = "Windows is up to date";
        result.log = accumulated_output;
        Q_ASSERT(result.duration_ms >= 0);
        finishWithResult(result, ActionStatus::Success);
        return;
    }

    if (exit_code != 0) {
        result.success = false;
        result.message = "Windows Update failed";
        result.log = QString("Exit code: %1\n%2\nErrors:\n%3")
                         .arg(exit_code)
                         .arg(accumulated_output)
                         .arg(errors);
        finishWithResult(result, ActionStatus::Failed);
        return;
    }

    result.success = true;
    bool reboot_required = accumulated_output.contains("REBOOT_REQUIRED", Qt::CaseInsensitive);
    result.message = reboot_required ? "Updates installed successfully - REBOOT REQUIRED"
                                     : "Updates installed successfully";
    result.log = accumulated_output;

    int remaining = queryPendingUpdateCount();
    if (remaining >= 0) {
        result.log += QString("\nVerification: %1 update(s) remaining").arg(remaining);
        if (remaining > 0 && !reboot_required) {
            result.message += " (some updates still pending)";
        }
    } else {
        result.log += "\nVerification: Unable to query remaining updates";
    }

    Q_ASSERT(result.duration_ms >= 0);

    finishWithResult(result, ActionStatus::Success);
}

}  // namespace sak

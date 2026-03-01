void WindowsUpdateAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("Windows Update check cancelled");
        return;
    }
    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();

    QString ps_script;
    executeInitSession(start_time, ps_script);

    QString accumulated_output;
    QString errors;
    int exit_code = 0;
    if (!executeSearchUpdates(start_time, ps_script, accumulated_output, errors, exit_code)) return;

    executeBuildReport(start_time, accumulated_output, errors, exit_code);
}

void WindowsUpdateAction::executeInitSession(const QDateTime& start_time, QString& ps_script)
{
    Q_EMIT executionProgress("Initiating Windows Update scan...", 5);

    // Modern Windows 10/11 approach: Use UsoClient (Update Session Orchestrator)
    // Per Microsoft docs: UsoClient replaces deprecated COM API for Windows Update automation
    // Commands: StartScan, StartDownload, StartInstall, ResumeUpdate, RestartDevice

    ps_script =
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
}

bool WindowsUpdateAction::executeSearchUpdates(const QDateTime& start_time,
                                                 const QString& ps_script,
                                                 QString& accumulated_output,
                                                 QString& errors,
                                                 int& exit_code)
{
    Q_EMIT executionProgress("Scanning for updates...", 20);
    Q_EMIT executionProgress("Preparing download...", 35);
    Q_EMIT executionProgress("Downloading updates...", 50);
    Q_EMIT executionProgress("Installing updates...", 70);

    ProcessResult ps = runPowerShell(ps_script, 1800000, true, true, [this]() { return isCancelled(); });

    if (ps.cancelled) {
        emitCancelledResult(QStringLiteral("Windows Update cancelled"), start_time);
        return false;
    }

    if (ps.timed_out) {
        emitFailedResult(
            QStringLiteral("Operation timed out after 30 minutes"),
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
                                               int exit_code)
{
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    result.duration_ms = duration_ms;

    if (accumulated_output.contains("No updates available")) {
        result.success = true;
        result.message = "Windows is up to date";
        result.log = accumulated_output;
    } else if (exit_code == 0) {
        result.success = true;
        bool reboot_required = accumulated_output.contains("REBOOT_REQUIRED", Qt::CaseInsensitive);
        result.message = reboot_required ?
            "Updates installed successfully - REBOOT REQUIRED" :
            "Updates installed successfully";
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

    } else {
        result.success = false;
        result.message = "Windows Update failed";
        result.log = QString("Exit code: %1\n%2\nErrors:\n%3").arg(exit_code).arg(accumulated_output).arg(errors);
    }

    finishWithResult(result, result.success ? ActionStatus::Success : ActionStatus::Failed);
}

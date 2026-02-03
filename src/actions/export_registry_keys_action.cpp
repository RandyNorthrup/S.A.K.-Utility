// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/export_registry_keys_action.h"
#include "sak/process_runner.h"
#include <QProcess>
#include <QDir>
#include <QDateTime>
#include <QRegularExpression>

namespace sak {

ExportRegistryKeysAction::ExportRegistryKeysAction(const QString& backup_location, QObject* parent)
    : QuickAction(parent)
    , m_backup_location(backup_location)
{
}

void ExportRegistryKeysAction::exportKey(const QString& key_path, const QString& filename) {
    QString output_file = m_backup_location + "/Registry/" + filename;
    QString cmd = QString("reg export \"%1\" \"%2\" /y").arg(key_path, output_file);
    ProcessResult proc = runProcess("cmd.exe", QStringList() << "/c" << cmd, 10000);
    if (proc.timed_out || proc.exit_code != 0) {
        Q_EMIT logMessage("Registry export warning: " + proc.std_err.trimmed());
    }
    
    if (QFile::exists(output_file)) {
        m_keys_exported++;
        m_total_size += QFileInfo(output_file).size();
    }
}

void ExportRegistryKeysAction::scan() {
    setStatus(ActionStatus::Scanning);

    ScanResult result;
    result.applicable = true;
    result.summary = "Registry backup will export critical hives";
    result.details = "Exports HKLM/HKCU hives and creates a manifest";

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void ExportRegistryKeysAction::execute() {
    if (isCancelled()) {
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    Q_EMIT executionProgress("Preparing enterprise registry backup...", 5);
    
    QDir backup_dir(m_backup_location + "/Registry");
    backup_dir.mkpath(".");
    
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss");
    QString backup_path = backup_dir.absolutePath().replace("/", "\\");
    
    // Enterprise registry backup using PowerShell
    // Per Microsoft docs: reg export for comprehensive backup of entire hives
    // Includes verification and file size reporting
    
    QString ps_script = QString(
        "# Enterprise Registry Backup Script\n"
        "$ErrorActionPreference = 'Continue'; \n"
        "$backupPath = '%1'; \n"
        "$timestamp = '%2'; \n"
        "$keysExported = 0; \n"
        "$totalSize = 0; \n"
        "\n"
        "# Define comprehensive registry keys to backup\n"
        "$registryKeys = @(\n"
        "    @{Path='HKLM\\SOFTWARE'; Name='HKLM_SOFTWARE'},\n"
        "    @{Path='HKLM\\SYSTEM'; Name='HKLM_SYSTEM'},\n"
        "    @{Path='HKLM\\SAM'; Name='HKLM_SAM'},\n"
        "    @{Path='HKLM\\SECURITY'; Name='HKLM_SECURITY'},\n"
        "    @{Path='HKCU\\Software'; Name='HKCU_Software'},\n"
        "    @{Path='HKCU\\Control Panel'; Name='HKCU_ControlPanel'},\n"
        "    @{Path='HKCU\\Environment'; Name='HKCU_Environment'},\n"
        "    @{Path='HKU\\.DEFAULT'; Name='HKU_DEFAULT'}\n"
        "); \n"
        "\n"
        "foreach ($key in $registryKeys) { \n"
        "    $outputFile = Join-Path $backupPath (\"$($key.Name)_$timestamp.reg\"); \n"
        "    Write-Output \"Exporting $($key.Path)...\"; \n"
        "    \n"
        "    try { \n"
        "        # Use reg.exe for reliable export\n"
        "        $process = Start-Process -FilePath 'reg.exe' -ArgumentList @('export', $key.Path, $outputFile, '/y') -NoNewWindow -Wait -PassThru; \n"
        "        \n"
        "        if ($process.ExitCode -eq 0 -and (Test-Path $outputFile)) { \n"
        "            $fileInfo = Get-Item $outputFile; \n"
        "            $totalSize += $fileInfo.Length; \n"
        "            $keysExported++; \n"
        "            Write-Output \"SUCCESS: $($key.Name) - $([math]::Round($fileInfo.Length/1MB, 2)) MB\"; \n"
        "        } else { \n"
        "            Write-Warning \"FAILED: $($key.Path) - Exit code $($process.ExitCode)\"; \n"
        "        } \n"
        "    } catch { \n"
        "        Write-Warning \"ERROR exporting $($key.Path): $_\"; \n"
        "    } \n"
        "} \n"
        "\n"
        "# Create backup manifest\n"
        "$manifest = @{\n"
        "    BackupDate = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss'); \n"
        "    ComputerName = $env:COMPUTERNAME; \n"
        "    UserName = $env:USERNAME; \n"
        "    KeysExported = $keysExported; \n"
        "    TotalSizeMB = [math]::Round($totalSize/1MB, 2); \n"
        "    WindowsVersion = [System.Environment]::OSVersion.VersionString\n"
        "}; \n"
        "\n"
        "$manifestPath = Join-Path $backupPath \"backup_manifest_$timestamp.json\"; \n"
        "$manifest | ConvertTo-Json | Out-File $manifestPath -Encoding UTF8; \n"
        "\n"
        "Write-Output \"TOTAL_KEYS:$keysExported\"; \n"
        "Write-Output \"TOTAL_SIZE:$totalSize\"; \n"
        "Write-Output \"MANIFEST:$manifestPath\""
    ).arg(backup_path, timestamp);
    
    ProcessResult ps = runPowerShell(ps_script, 60000);
    if (!ps.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Registry export warning: " + ps.std_err.trimmed());
    }

    QString accumulated_output = ps.std_out;
    int keys_exported = 0;
    qint64 total_size = 0;
    
    // Parse results
    QRegularExpression totalKeysRe("TOTAL_KEYS:(\\d+)");
    QRegularExpression totalSizeRe("TOTAL_SIZE:(\\d+)");
    QRegularExpression manifestRe("MANIFEST:(.+)");
    
    QRegularExpressionMatch keysMatch = totalKeysRe.match(accumulated_output);
    QRegularExpressionMatch sizeMatch = totalSizeRe.match(accumulated_output);
    QRegularExpressionMatch manifestMatch = manifestRe.match(accumulated_output);
    
    if (keysMatch.hasMatch()) {
        keys_exported = keysMatch.captured(1).toInt();
    }
    if (sizeMatch.hasMatch()) {
        total_size = sizeMatch.captured(1).toLongLong();
    }
    
    QString manifest_path;
    if (manifestMatch.hasMatch()) {
        manifest_path = manifestMatch.captured(1).trimmed();
    }
    
    Q_EMIT executionProgress("Backup complete", 100);
    
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    
    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.files_processed = keys_exported;
    result.bytes_processed = total_size;
    result.output_path = backup_dir.absolutePath();
    
    if (keys_exported > 0) {
        result.success = true;
        double size_mb = total_size / (1024.0 * 1024.0);
        result.message = QString("Exported %1 registry hive(s) - %2 MB")
            .arg(keys_exported)
            .arg(size_mb, 0, 'f', 2);
        result.log = QString("Backup location: %1\nManifest: %2\n\nDetails:\n%3")
                        .arg(backup_dir.absolutePath())
                        .arg(manifest_path)
                        .arg(accumulated_output);
        setStatus(ActionStatus::Success);
    } else {
        result.success = false;
        result.message = "Failed to export registry keys";
        result.log = QString("No registry keys were successfully exported\n\nOutput:\n%1").arg(accumulated_output);
        setStatus(ActionStatus::Failed);
    }
    
    setExecutionResult(result);
    Q_EMIT executionComplete(result);
}

} // namespace sak

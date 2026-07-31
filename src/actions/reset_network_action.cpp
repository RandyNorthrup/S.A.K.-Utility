// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file reset_network_action.cpp
/// @brief Implements Windows network stack reset and adapter refresh

#include "sak/actions/reset_network_action.h"

#include "sak/action_constants.h"
#include "sak/layout_constants.h"
#include "sak/process_runner.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QThread>

namespace sak {

namespace {
constexpr qsizetype kAdapterCountPrefixLength = 9;
}

ResetNetworkAction::ResetNetworkAction(QObject* parent) : QuickAction(parent) {}

QString ResetNetworkAction::buildAdapterResetScript() {
    // Restart-NetAdapter is non-terminating: a per-adapter failure would leave
    // $LASTEXITCODE 0 and read as success. Force Stop + try/catch so any adapter
    // error terminates the pipeline and the process exits 1 (recorded as a
    // failure by the caller).
    return QStringLiteral(
        "$ErrorActionPreference = 'Stop'\n"
        "try {\n"
        "    Get-NetAdapter | Where-Object {$_.Status -eq 'Up'} | ForEach-Object {\n"
        "        Restart-NetAdapter -Name $_.Name -Confirm:$false -ErrorAction Stop\n"
        "    }\n"
        "    exit 0\n"
        "} catch {\n"
        "    Write-Error $_.Exception.Message\n"
        "    exit 1\n"
        "}\n");
}

bool ResetNetworkAction::stepFailed(bool timed_out, int exit_code) {
    return timed_out || exit_code != 0;
}

void ResetNetworkAction::scan() {
    setStatus(ActionStatus::Scanning);

    QString ps_cmd =
        "try { "
        "  $adapters = Get-NetAdapter | Where-Object {$_.Status -eq 'Up'}; "
        "  Write-Output \"ADAPTERS:$($adapters.Count)\"; "
        "} catch { Write-Output \"ADAPTERS:0\" }";
    ProcessResult proc = runPowerShell(ps_cmd, sak::kTimerNetshWaitMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Network adapter scan warning: " + proc.std_err.trimmed());
    }
    QString output = proc.std_out.trimmed();
    int adapters = 0;
    if (output.contains("ADAPTERS:")) {
        adapters =
            output.mid(output.indexOf("ADAPTERS:") + kAdapterCountPrefixLength).trimmed().toInt();
    }

    ScanResult result;
    result.applicable = adapters > 0;
    result.summary = adapters > 0 ? QString("Active adapters: %1").arg(adapters)
                                  : "No active network adapters detected";
    result.details = "Reset will refresh DNS, Winsock, TCP/IP and firewall";
    if (adapters == 0) {
        result.warning = "Network reset may not be applicable without active adapters";
    }

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void ResetNetworkAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("Network reset cancelled");
        return;
    }
    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    m_requires_reboot = false;
    m_winsock_backup_path.clear();
    m_firewall_backup_path.clear();
    QStringList errors;

    // Capture a Winsock backup before touching anything destructive; abort the
    // whole reset if it cannot be captured (emits its own failed result).
    if (!executeBackupWinsock(errors, start_time)) {
        return;
    }
    if (!executeFlushDns(errors)) {
        emitCancelledResult(QStringLiteral("Network reset cancelled by user"), start_time);
        return;
    }
    if (!executeResetWinsock(errors)) {
        emitCancelledResult(QStringLiteral("Network reset cancelled by user"), start_time);
        return;
    }
    if (!executeResetIpStack(errors)) {
        emitCancelledResult(QStringLiteral("Network reset cancelled by user"), start_time);
        return;
    }

    executeBuildReport(errors, start_time);
}

bool ResetNetworkAction::executeBackupWinsock(QStringList& errors, const QDateTime& start_time) {
    // Step 1: Backup current Winsock configuration BEFORE any destructive reset.
    Q_EMIT executionProgress("Backing up Winsock catalog...", progress::kStep5);

    QTemporaryFile backupFile(QDir::temp().filePath(QStringLiteral("sak_winsock_XXXXXX.txt")));
    QString backupError;
    if (!backupFile.open()) {
        backupError = "Failed to create temp file for Winsock backup";
    } else {
        const QString backupPath = backupFile.fileName();
        backupFile.close();
        // Run netsh directly and write its captured stdout to the backup file in
        // process. Wrapping this as `cmd.exe /C "netsh ... > <path>"` would let
        // cmd perform %VAR% expansion and metacharacter (& | < >) parsing on the
        // environment-derived TEMP path, an elevated command-injection vector.
        ProcessResult proc = runProcess("netsh",
                                        QStringList() << "winsock" << "show" << "catalog",
                                        sak::kTimeoutNetworkReadMs);
        if (stepFailed(proc.timed_out, proc.exit_code)) {
            backupError = QString("Winsock backup failed (exit %1)").arg(proc.exit_code);
        } else {
            // Only retain and report a backup that actually wrote successfully; on any
            // failure the QTemporaryFile auto-removes so no empty/false backup survives.
            QFile out(backupPath);
            const QByteArray bytes = proc.std_out.toUtf8();
            if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
                out.write(bytes) != bytes.size() || !out.flush()) {
                backupError = "Failed to write Winsock backup file";
            } else {
                out.close();
                backupFile.setAutoRemove(false);
                m_winsock_backup_path = backupPath;
            }
        }
    }

    if (!backupError.isEmpty()) {
        // Fail closed: do not run a destructive network reset with no rollback
        // reference captured.
        errors << backupError;
        ExecutionResult result;
        result.success = false;
        result.message = "Network reset aborted: Winsock backup failed";
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        result.log =
            "Aborted before any change because the pre-reset Winsock backup could not "
            "be captured:\n" +
            backupError;
        finishWithResult(result, ActionStatus::Failed);
        return false;
    }

    return true;
}

bool ResetNetworkAction::executeFlushDns(QStringList& errors) {
    if (isCancelled()) {
        return false;
    }

    // Step 2: Flush DNS cache
    Q_EMIT executionProgress("Flushing DNS cache...", progress::kStep15);
    {
        ProcessResult proc =
            runProcess("ipconfig", QStringList() << "/flushdns", sak::kTimeoutNetworkReadMs);
        if (stepFailed(proc.timed_out, proc.exit_code)) {
            errors << QString("DNS flush failed (exit %1)").arg(proc.exit_code);
        }
    }

    return !isCancelled();
}

bool ResetNetworkAction::executeResetWinsock(QStringList& errors) {
    // Step 3: Reset Winsock catalog
    Q_EMIT executionProgress("Resetting Winsock catalog...", progress::kStep30);
    {
        ProcessResult proc =
            runProcess("netsh", QStringList() << "winsock" << "reset", sak::kTimeoutNetworkReadMs);
        if (proc.timed_out) {
            errors << "Winsock reset timed out";
        } else if (proc.exit_code != 0) {
            errors << QString("Winsock reset failed (exit %1)").arg(proc.exit_code);
        }
    }
    m_requires_reboot = true;

    if (isCancelled()) {
        return false;
    }

    // Step 4: Reset TCP/IP stack
    Q_EMIT executionProgress("Resetting TCP/IP stack...", progress::kStep45);
    {
        ProcessResult proc = runProcess("netsh",
                                        QStringList() << "int" << "ip" << "reset",
                                        sak::kTimeoutNetworkReadMs);
        if (proc.timed_out) {
            errors << "TCP/IP reset timed out";
        } else if (proc.exit_code != 0) {
            errors << QString("TCP/IP reset failed (exit %1)").arg(proc.exit_code);
        }
    }
    {
        ProcessResult proc = runProcess("netsh",
                                        QStringList() << "int" << "ipv6" << "reset",
                                        sak::kTimeoutNetworkReadMs);
        if (proc.timed_out) {
            errors << "IPv6 reset timed out";
        } else if (proc.exit_code != 0) {
            errors << QString("IPv6 reset failed (exit %1)").arg(proc.exit_code);
        }
    }

    return !isCancelled();
}

bool ResetNetworkAction::executeResetIpStack(QStringList& errors) {
    // Step 5: Release and renew IP addresses
    Q_EMIT executionProgress("Releasing IP addresses...", progress::kStep60);
    {
        ProcessResult proc =
            runProcess("ipconfig", QStringList() << "/release", sak::kTimeoutNetworkReadMs);
        if (proc.timed_out) {
            errors << "IP release timed out";
        } else if (proc.exit_code != 0) {
            errors << QString("IP release failed (exit %1)").arg(proc.exit_code);
        }
    }

    QThread::msleep(sak::kTimerServiceDelayMs);

    Q_EMIT executionProgress("Renewing IP addresses...", progress::kStep70);
    {
        ProcessResult proc =
            runProcess("ipconfig", QStringList() << "/renew", sak::kTimeoutNetworkReadMs);
        if (proc.timed_out) {
            errors << "IP renew timed out";
        } else if (proc.exit_code != 0) {
            errors << QString("IP renew failed (exit %1)").arg(proc.exit_code);
        }
    }

    if (isCancelled()) {
        return false;
    }

    // Step 6: Export custom firewall rules, then reset (only if export succeeded)
    executeResetFirewall(errors);

    if (isCancelled()) {
        return false;
    }

    return executeResetAdaptersAndCache(errors);
}

void ResetNetworkAction::executeResetFirewall(QStringList& errors) {
    Q_EMIT executionProgress("Backing up firewall rules...", progress::kStep75);
    const QString backup = exportFirewallRules(errors);
    if (backup.isEmpty()) {
        // Fail closed for the firewall specifically: without a rules backup, do
        // NOT wipe potentially custom rules. Recorded as an error so overall
        // success reflects it; the rest of the reset still proceeds.
        errors << "Firewall reset skipped: rules backup failed (custom rules preserved)";
        return;
    }

    Q_EMIT executionProgress("Resetting Windows Firewall...", progress::kStep80);
    ProcessResult proc =
        runProcess("netsh", QStringList() << "advfirewall" << "reset", sak::kTimeoutNetworkReadMs);
    if (stepFailed(proc.timed_out, proc.exit_code)) {
        errors << QString("Firewall reset failed (exit %1)").arg(proc.exit_code);
    }
}

QString ResetNetworkAction::exportFirewallRules(QStringList& errors) {
    QTemporaryFile backupFile(QDir::temp().filePath(QStringLiteral("sak_fw_XXXXXX.wfw")));
    if (!backupFile.open()) {
        errors << "Failed to create temp file for firewall backup";
        return {};
    }
    const QString backupPath = backupFile.fileName();
    backupFile.close();
    // netsh writes the .wfw itself; remove the placeholder so export creates it
    // fresh and we can verify it is non-empty afterwards.
    QFile::remove(backupPath);

    ProcessResult proc = runProcess("netsh",
                                    QStringList() << "advfirewall" << "export" << backupPath,
                                    sak::kTimeoutNetworkReadMs);
    if (stepFailed(proc.timed_out, proc.exit_code) || QFileInfo(backupPath).size() <= 0) {
        errors << QString("Firewall rules export failed (exit %1)").arg(proc.exit_code);
        QFile::remove(backupPath);
        return {};
    }

    backupFile.setAutoRemove(false);
    m_firewall_backup_path = backupPath;
    return backupPath;
}

bool ResetNetworkAction::executeResetAdaptersAndCache(QStringList& errors) {
    // Step 7: Reset network adapter settings
    Q_EMIT executionProgress("Resetting network adapters...", progress::kStep85);
    {
        ProcessResult adapterReset = runPowerShell(buildAdapterResetScript(),
                                                   sak::kTimeoutNetworkReadMs);
        if (stepFailed(adapterReset.timed_out, adapterReset.exit_code)) {
            errors << QString("Adapter restart failed (exit %1)").arg(adapterReset.exit_code);
        }
        if (!adapterReset.std_err.trimmed().isEmpty()) {
            Q_EMIT logMessage("Adapter restart warning: " + adapterReset.std_err.trimmed());
        }
    }

    if (isCancelled()) {
        return false;
    }

    // Step 8: Clear NetBIOS cache
    Q_EMIT executionProgress("Clearing NetBIOS cache...", progress::kStep90);
    {
        ProcessResult proc =
            runProcess("nbtstat", QStringList() << "-R", sak::kTimeoutNetworkReadMs);
        if (proc.timed_out) {
            errors << "NetBIOS cache clear timed out";
        } else if (proc.exit_code != 0) {
            errors << QString("NetBIOS cache clear failed (exit %1)").arg(proc.exit_code);
        }
    }
    {
        ProcessResult proc =
            runProcess("nbtstat", QStringList() << "-RR", sak::kTimeoutNetworkReadMs);
        if (proc.timed_out) {
            errors << "NetBIOS refresh timed out";
        } else if (proc.exit_code != 0) {
            errors << QString("NetBIOS refresh failed (exit %1)").arg(proc.exit_code);
        }
    }

    return !isCancelled();
}

void ResetNetworkAction::executeBuildReport(const QStringList& errors,
                                            const QDateTime& start_time) {
    // Step 9: Verify network configuration
    Q_EMIT executionProgress("Verifying network configuration...", progress::kStep95);

    QString verifyScript =
        "$adapters = Get-NetAdapter | Where-Object {$_.Status -eq 'Up'}; "
        "$ipConfigs = Get-NetIPConfiguration; "
        "Write-Output \"Active adapters: $($adapters.Count)\"; "
        "Write-Output \"Configured IPs: $($ipConfigs.Count)\"";

    ProcessResult verifyProc = runPowerShell(verifyScript, sak::kTimeoutProcessShortMs);
    if (!verifyProc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Network verification warning: " + verifyProc.std_err.trimmed());
    }
    QString verifyOutput = verifyProc.std_out;

    // Gate success on the verify probe: a timed-out / non-zero verification means
    // we could not confirm the reset landed, so it must not be reported as a clean
    // success. (Previously the verify result was logged but ignored.)
    QStringList all_errors = errors;
    if (stepFailed(verifyProc.timed_out, verifyProc.exit_code)) {
        all_errors << QString("Network verification failed (exit %1)").arg(verifyProc.exit_code);
    }

    ExecutionResult result;
    result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    result.success = all_errors.isEmpty();
    result.message =
        all_errors.isEmpty()
            ? "Network settings reset successfully"
            : QString("Network reset completed with %1 issue(s)").arg(all_errors.size());

    if (m_requires_reboot) {
        result.message += " - REBOOT REQUIRED for Winsock/TCP-IP changes";
    }

    result.log = QString("Winsock backup: %1\nFirewall rules backup: %2\n\nVerification:\n%3")
                     .arg(m_winsock_backup_path.isEmpty() ? "unavailable" : m_winsock_backup_path,
                          m_firewall_backup_path.isEmpty() ? "unavailable" : m_firewall_backup_path,
                          verifyOutput);

    if (!all_errors.isEmpty()) {
        result.log += "\nErrors:\n" + all_errors.join("\n");
    }

    finishWithResult(result, result.success ? ActionStatus::Success : ActionStatus::Failed);
}

}  // namespace sak

// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file reset_network_action.cpp
/// @brief Implements Windows network stack reset and adapter refresh

#include "sak/actions/reset_network_action.h"
#include "sak/layout_constants.h"
#include "sak/process_runner.h"
#include <QThread>
#include <QDir>
#include <QTemporaryFile>

namespace sak {

ResetNetworkAction::ResetNetworkAction(QObject* parent)
    : QuickAction(parent)
{
}

void ResetNetworkAction::flushDNS() {
    Q_EMIT executionProgress("Flushing DNS cache...", 20);
    ProcessResult proc = runProcess("ipconfig", QStringList() << "/flushdns",
        sak::kTimeoutNetworkReadMs);
    if (!proc.succeeded()) {
        Q_EMIT logMessage("Flush DNS warning: " + proc.std_err.trimmed());
    }
}

void ResetNetworkAction::resetWinsock() {
    Q_EMIT executionProgress("Resetting Winsock catalog...", 40);
    ProcessResult proc = runProcess("netsh", QStringList() << "winsock" << "reset",
        sak::kTimeoutNetworkReadMs);
    if (!proc.succeeded()) {
        Q_EMIT logMessage("Winsock reset warning: " + proc.std_err.trimmed());
    }
    m_requires_reboot = true;
}

void ResetNetworkAction::resetTCPIP() {
    Q_EMIT executionProgress("Resetting TCP/IP stack...", 60);
    ProcessResult proc = runProcess("netsh", QStringList() << "int" << "ip" << "reset",
        sak::kTimeoutNetworkReadMs);
    if (!proc.succeeded()) {
        Q_EMIT logMessage("TCP/IP reset warning: " + proc.std_err.trimmed());
    }
    m_requires_reboot = true;
}

void ResetNetworkAction::releaseRenewIP() {
    Q_EMIT executionProgress("Releasing and renewing IP address...", 80);
    ProcessResult release_proc = runProcess("ipconfig", QStringList() << "/release",
        sak::kTimeoutNetworkReadMs);
    if (!release_proc.succeeded()) {
        Q_EMIT logMessage("IP release warning: " + release_proc.std_err.trimmed());
    }
    QThread::msleep(sak::kTimerProgressPollMs);
    ProcessResult renew_proc = runProcess("ipconfig", QStringList() << "/renew",
        sak::kTimeoutNetworkReadMs);
    if (!renew_proc.succeeded()) {
        Q_EMIT logMessage("IP renew warning: " + renew_proc.std_err.trimmed());
    }
}

void ResetNetworkAction::resetFirewall() {
    Q_EMIT executionProgress("Resetting firewall to defaults...", 90);
    ProcessResult proc = runProcess("netsh", QStringList() << "advfirewall" << "reset",
        sak::kTimeoutNetworkReadMs);
    if (!proc.succeeded()) {
        Q_EMIT logMessage("Firewall reset warning: " + proc.std_err.trimmed());
    }
}

void ResetNetworkAction::scan() {
    setStatus(ActionStatus::Scanning);
    Q_ASSERT(status() == ActionStatus::Scanning);

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
        adapters = output.mid(output.indexOf("ADAPTERS:") + 9).trimmed().toInt();
    }

    ScanResult result;
    result.applicable = adapters > 0;
    result.summary = adapters > 0
        ? QString("Active adapters: %1").arg(adapters)
        : "No active network adapters detected";
    result.details = "Reset will refresh DNS, Winsock, TCP/IP and firewall";
    if (adapters == 0) {
        result.warning = "Network reset may not be applicable without active adapters";
    }

    Q_ASSERT(!result.summary.isEmpty());

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
    Q_ASSERT(status() == ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    Q_ASSERT(start_time.isValid());

    m_requires_reboot = false;
    QStringList errors;

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

bool ResetNetworkAction::executeFlushDns(QStringList& errors) {
    // Step 1: Backup current Winsock configuration
    Q_EMIT executionProgress("Backing up Winsock catalog...", 5);

    QTemporaryFile backupFile(QDir::temp().filePath(QStringLiteral("sak_winsock_XXXXXX.txt")));
    backupFile.setAutoRemove(true);
    if (!backupFile.open()) {
        errors << "Failed to create temp file for Winsock backup";
    } else {
        const QString backupPath = backupFile.fileName();
        backupFile.close();
        QString backupCmd = QString("netsh winsock show catalog > \"%1\"").arg(backupPath);
        ProcessResult proc = runProcess("cmd.exe", QStringList() << "/C" << backupCmd,
            sak::kTimeoutNetworkReadMs);
        if (proc.timed_out) errors << "Winsock backup timed out";
        else if (proc.exit_code != 0) errors
            << QString("Winsock backup failed (exit %1)").arg(proc.exit_code);
    }

    if (isCancelled()) return false;

    // Step 2: Flush DNS cache
    Q_EMIT executionProgress("Flushing DNS cache...", 15);
    {
        ProcessResult proc = runProcess("ipconfig", QStringList() << "/flushdns",
            sak::kTimeoutNetworkReadMs);
        if (proc.timed_out) errors << "DNS flush timed out";
        else if (proc.exit_code != 0) errors
            << QString("DNS flush failed (exit %1)").arg(proc.exit_code);
    }

    return !isCancelled();
}

bool ResetNetworkAction::executeResetWinsock(QStringList& errors) {
    // Step 3: Reset Winsock catalog
    Q_EMIT executionProgress("Resetting Winsock catalog...", 30);
    {
        ProcessResult proc = runProcess("netsh", QStringList() << "winsock" << "reset",
            sak::kTimeoutNetworkReadMs);
        if (proc.timed_out) errors << "Winsock reset timed out";
        else if (proc.exit_code != 0) errors
            << QString("Winsock reset failed (exit %1)").arg(proc.exit_code);
    }
    m_requires_reboot = true;

    if (isCancelled()) return false;

    // Step 4: Reset TCP/IP stack
    Q_EMIT executionProgress("Resetting TCP/IP stack...", 45);
    {
        ProcessResult proc = runProcess("netsh", QStringList() << "int" << "ip" << "reset",
            sak::kTimeoutNetworkReadMs);
        if (proc.timed_out) errors << "TCP/IP reset timed out";
        else if (proc.exit_code != 0) errors
            << QString("TCP/IP reset failed (exit %1)").arg(proc.exit_code);
    }
    {
        ProcessResult proc = runProcess("netsh", QStringList() << "int" << "ipv6" << "reset",
            sak::kTimeoutNetworkReadMs);
        if (proc.timed_out) errors << "IPv6 reset timed out";
        else if (proc.exit_code != 0) errors
            << QString("IPv6 reset failed (exit %1)").arg(proc.exit_code);
    }

    return !isCancelled();
}

bool ResetNetworkAction::executeResetIpStack(QStringList& errors) {
    // Step 5: Release and renew IP addresses
    Q_EMIT executionProgress("Releasing IP addresses...", 60);
    {
        ProcessResult proc = runProcess("ipconfig", QStringList() << "/release",
            sak::kTimeoutNetworkReadMs);
        if (proc.timed_out) errors << "IP release timed out";
        else if (proc.exit_code != 0) errors
            << QString("IP release failed (exit %1)").arg(proc.exit_code);
    }

    QThread::msleep(sak::kTimerServiceDelayMs);

    Q_EMIT executionProgress("Renewing IP addresses...", 70);
    {
        ProcessResult proc = runProcess("ipconfig", QStringList() << "/renew",
            sak::kTimeoutNetworkReadMs);
        if (proc.timed_out) errors << "IP renew timed out";
        else if (proc.exit_code != 0) errors
            << QString("IP renew failed (exit %1)").arg(proc.exit_code);
    }

    if (isCancelled()) return false;

    // Step 6: Reset Windows Firewall to defaults
    Q_EMIT executionProgress("Resetting Windows Firewall...", 80);
    {
        ProcessResult proc = runProcess("netsh", QStringList() << "advfirewall" << "reset",
            sak::kTimeoutNetworkReadMs);
        if (proc.timed_out) errors << "Firewall reset timed out";
        else if (proc.exit_code != 0) errors
            << QString("Firewall reset failed (exit %1)").arg(proc.exit_code);
    }

    if (isCancelled()) return false;

    return executeResetAdaptersAndCache(errors);
}

bool ResetNetworkAction::executeResetAdaptersAndCache(QStringList& errors) {
    // Step 7: Reset network adapter settings
    Q_EMIT executionProgress("Resetting network adapters...", 85);
    QString resetAdapterScript =
        "Get-NetAdapter | Where-Object {$_.Status -eq 'Up'} | ForEach-Object { "
        "  Restart-NetAdapter -Name $_.Name -Confirm:$false "
        "}";
    {
        ProcessResult adapterReset = runPowerShell(resetAdapterScript, sak::kTimeoutNetworkReadMs);
        if (adapterReset.timed_out) {
            errors << "Adapter restart timed out";
        } else if (adapterReset.exit_code != 0) {
            errors << QString("Adapter restart failed (exit %1)").arg(adapterReset.exit_code);
        }
        if (!adapterReset.std_err.trimmed().isEmpty()) {
            Q_EMIT logMessage("Adapter restart warning: " + adapterReset.std_err.trimmed());
        }
    }

    if (isCancelled()) return false;

    // Step 8: Clear NetBIOS cache
    Q_EMIT executionProgress("Clearing NetBIOS cache...", 90);
    {
        ProcessResult proc = runProcess("nbtstat", QStringList() << "-R",
            sak::kTimeoutNetworkReadMs);
        if (proc.timed_out) errors << "NetBIOS cache clear timed out";
        else if (proc.exit_code != 0) errors
            << QString("NetBIOS cache clear failed (exit %1)").arg(proc.exit_code);
    }
    {
        ProcessResult proc = runProcess("nbtstat", QStringList() << "-RR",
            sak::kTimeoutNetworkReadMs);
        if (proc.timed_out) errors << "NetBIOS refresh timed out";
        else if (proc.exit_code != 0) errors
            << QString("NetBIOS refresh failed (exit %1)").arg(proc.exit_code);
    }

    return !isCancelled();
}

void ResetNetworkAction::executeBuildReport(const QStringList& errors,
    const QDateTime& start_time) {
    // Step 9: Verify network configuration
    Q_EMIT executionProgress("Verifying network configuration...", 95);

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

    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    Q_ASSERT(!result.success);  // verify default init
    result.duration_ms = duration_ms;
    result.success = errors.isEmpty();
    result.message = errors.isEmpty()
        ? "Network settings reset successfully"
        : QString("Network reset completed with %1 issue(s)").arg(errors.size());

    if (m_requires_reboot) {
        result.message += " - REBOOT REQUIRED for Winsock/TCP-IP changes";
    }

    QString backupPath = QDir::temp().filePath("winsock_backup.txt");
    result.log = QString("Winsock backup saved to: %1\n\nVerification:\n%2")
                    .arg(backupPath)
                    .arg(verifyOutput);

    if (!errors.isEmpty()) {
        result.log += "\nErrors:\n" + errors.join("\n");
    }

    finishWithResult(result, result.success ? ActionStatus::Success : ActionStatus::Failed);
}

} // namespace sak

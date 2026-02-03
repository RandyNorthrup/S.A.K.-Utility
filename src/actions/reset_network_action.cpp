// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/reset_network_action.h"
#include "sak/process_runner.h"
#include <QProcess>
#include <QThread>
#include <QDir>

namespace sak {

ResetNetworkAction::ResetNetworkAction(QObject* parent)
    : QuickAction(parent)
{
}

void ResetNetworkAction::flushDNS() {
    Q_EMIT executionProgress("Flushing DNS cache...", 20);
    ProcessResult proc = runProcess("ipconfig", QStringList() << "/flushdns", 15000);
    if (proc.timed_out || proc.exit_code != 0) {
        Q_EMIT logMessage("Flush DNS warning: " + proc.std_err.trimmed());
    }
}

void ResetNetworkAction::resetWinsock() {
    Q_EMIT executionProgress("Resetting Winsock catalog...", 40);
    ProcessResult proc = runProcess("netsh", QStringList() << "winsock" << "reset", 15000);
    if (proc.timed_out || proc.exit_code != 0) {
        Q_EMIT logMessage("Winsock reset warning: " + proc.std_err.trimmed());
    }
    m_requires_reboot = true;
}

void ResetNetworkAction::resetTCPIP() {
    Q_EMIT executionProgress("Resetting TCP/IP stack...", 60);
    ProcessResult proc = runProcess("netsh", QStringList() << "int" << "ip" << "reset", 15000);
    if (proc.timed_out || proc.exit_code != 0) {
        Q_EMIT logMessage("TCP/IP reset warning: " + proc.std_err.trimmed());
    }
    m_requires_reboot = true;
}

void ResetNetworkAction::releaseRenewIP() {
    Q_EMIT executionProgress("Releasing and renewing IP address...", 80);
    ProcessResult release_proc = runProcess("ipconfig", QStringList() << "/release", 15000);
    if (release_proc.timed_out || release_proc.exit_code != 0) {
        Q_EMIT logMessage("IP release warning: " + release_proc.std_err.trimmed());
    }
    QThread::msleep(1000);
    ProcessResult renew_proc = runProcess("ipconfig", QStringList() << "/renew", 15000);
    if (renew_proc.timed_out || renew_proc.exit_code != 0) {
        Q_EMIT logMessage("IP renew warning: " + renew_proc.std_err.trimmed());
    }
}

void ResetNetworkAction::resetFirewall() {
    Q_EMIT executionProgress("Resetting firewall to defaults...", 90);
    ProcessResult proc = runProcess("netsh", QStringList() << "advfirewall" << "reset", 15000);
    if (proc.timed_out || proc.exit_code != 0) {
        Q_EMIT logMessage("Firewall reset warning: " + proc.std_err.trimmed());
    }
}

void ResetNetworkAction::scan() {
    setStatus(ActionStatus::Scanning);

    QString ps_cmd =
        "try { "
        "  $adapters = Get-NetAdapter | Where-Object {$_.Status -eq 'Up'}; "
        "  Write-Output \"ADAPTERS:$($adapters.Count)\"; "
        "} catch { Write-Output \"ADAPTERS:0\" }";
    ProcessResult proc = runPowerShell(ps_cmd, 8000);
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

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void ResetNetworkAction::execute() {
    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    m_requires_reboot = false;

    QStringList errors;

    auto runCommand = [&errors](const QString& program, const QStringList& args, const QString& label) {
        ProcessResult proc = runProcess(program, args, 15000);
        if (proc.timed_out) {
            errors << QString("%1 timed out").arg(label);
            return false;
        }

        if (proc.exit_code != 0) {
            errors << QString("%1 failed (exit %2)").arg(label).arg(proc.exit_code);
            return false;
        }

        return true;
    };
    
    // Enterprise network reset sequence per Microsoft documentation
    // Proper order: Backup → Reset → Verify
    
    Q_EMIT executionProgress("Backing up Winsock catalog...", 5);
    
    // Step 1: Backup current Winsock configuration
    QString backupPath = QDir::temp().filePath("winsock_backup.txt");
    QString backupCmd = QString("netsh winsock show catalog > \"%1\"").arg(backupPath);
    runCommand("cmd.exe", QStringList() << "/C" << backupCmd, "Winsock backup");
    
    auto finish_cancelled = [this, &start_time]() {
        ExecutionResult result;
        result.success = false;
        result.message = "Network reset cancelled by user";
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        setExecutionResult(result);
        setStatus(ActionStatus::Cancelled);
        Q_EMIT executionComplete(result);
    };

    if (isCancelled()) {
        finish_cancelled();
        return;
    }
    
    // Step 2: Flush DNS cache
    Q_EMIT executionProgress("Flushing DNS cache...", 15);
    runCommand("ipconfig", QStringList() << "/flushdns", "DNS flush");
    
    if (isCancelled()) {
        finish_cancelled();
        return;
    }
    
    // Step 3: Reset Winsock catalog
    Q_EMIT executionProgress("Resetting Winsock catalog...", 30);
    runCommand("netsh", QStringList() << "winsock" << "reset", "Winsock reset");
    m_requires_reboot = true;
    
    if (isCancelled()) {
        finish_cancelled();
        return;
    }
    
    // Step 4: Reset TCP/IP stack
    Q_EMIT executionProgress("Resetting TCP/IP stack...", 45);
    runCommand("netsh", QStringList() << "int" << "ip" << "reset", "TCP/IP reset");
    runCommand("netsh", QStringList() << "int" << "ipv6" << "reset", "IPv6 reset");
    
    if (isCancelled()) {
        finish_cancelled();
        return;
    }
    
    // Step 5: Release and renew IP addresses
    Q_EMIT executionProgress("Releasing IP addresses...", 60);
    runCommand("ipconfig", QStringList() << "/release", "IP release");
    
    QThread::msleep(2000);
    
    Q_EMIT executionProgress("Renewing IP addresses...", 70);
    runCommand("ipconfig", QStringList() << "/renew", "IP renew");
    
    if (isCancelled()) {
        finish_cancelled();
        return;
    }
    
    // Step 6: Reset Windows Firewall to defaults
    Q_EMIT executionProgress("Resetting Windows Firewall...", 80);
    runCommand("netsh", QStringList() << "advfirewall" << "reset", "Firewall reset");
    
    if (isCancelled()) {
        finish_cancelled();
        return;
    }
    
    // Step 7: Reset network adapter settings
    Q_EMIT executionProgress("Resetting network adapters...", 85);
    QString resetAdapterScript = 
        "Get-NetAdapter | Where-Object {$_.Status -eq 'Up'} | ForEach-Object { "
        "  Restart-NetAdapter -Name $_.Name -Confirm:$false "
        "}";
    {
        ProcessResult adapterReset = runPowerShell(resetAdapterScript, 15000);
        if (adapterReset.timed_out) {
            errors << "Adapter restart timed out";
        } else if (adapterReset.exit_code != 0) {
            errors << QString("Adapter restart failed (exit %1)").arg(adapterReset.exit_code);
        }
        if (!adapterReset.std_err.trimmed().isEmpty()) {
            Q_EMIT logMessage("Adapter restart warning: " + adapterReset.std_err.trimmed());
        }
    }
    
    if (isCancelled()) {
        finish_cancelled();
        return;
    }
    
    // Step 8: Clear NetBIOS cache
    Q_EMIT executionProgress("Clearing NetBIOS cache...", 90);
    runCommand("nbtstat", QStringList() << "-R", "NetBIOS cache clear");
    runCommand("nbtstat", QStringList() << "-RR", "NetBIOS refresh");
    
    if (isCancelled()) {
        finish_cancelled();
        return;
    }
    
    // Step 9: Verify network configuration
    Q_EMIT executionProgress("Verifying network configuration...", 95);
    
    QString verifyScript = 
        "$adapters = Get-NetAdapter | Where-Object {$_.Status -eq 'Up'}; "
        "$ipConfigs = Get-NetIPConfiguration; "
        "Write-Output \"Active adapters: $($adapters.Count)\"; "
        "Write-Output \"Configured IPs: $($ipConfigs.Count)\"";
    
    ProcessResult verifyProc = runPowerShell(verifyScript, 5000);
    if (!verifyProc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Network verification warning: " + verifyProc.std_err.trimmed());
    }
    QString verifyOutput = verifyProc.std_out;
    
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    
    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.success = errors.isEmpty();
    result.message = errors.isEmpty()
        ? "Network settings reset successfully"
        : QString("Network reset completed with %1 issue(s)").arg(errors.size());
    
    if (m_requires_reboot) {
        result.message += " - REBOOT REQUIRED for Winsock/TCP-IP changes";
    }
    
    result.log = QString("Winsock backup saved to: %1\n\nVerification:\n%2")
                    .arg(backupPath)
                    .arg(verifyOutput);

    if (!errors.isEmpty()) {
        result.log += "\nErrors:\n" + errors.join("\n");
    }
    
    setExecutionResult(result);
    setStatus(result.success ? ActionStatus::Success : ActionStatus::Failed);
    Q_EMIT executionComplete(result);
}

} // namespace sak

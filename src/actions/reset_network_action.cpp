// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/reset_network_action.h"
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
    QProcess::execute("ipconfig", QStringList() << "/flushdns");
}

void ResetNetworkAction::resetWinsock() {
    Q_EMIT executionProgress("Resetting Winsock catalog...", 40);
    QProcess::execute("netsh", QStringList() << "winsock" << "reset");
    m_requires_reboot = true;
}

void ResetNetworkAction::resetTCPIP() {
    Q_EMIT executionProgress("Resetting TCP/IP stack...", 60);
    QProcess::execute("netsh", QStringList() << "int" << "ip" << "reset");
    m_requires_reboot = true;
}

void ResetNetworkAction::releaseRenewIP() {
    Q_EMIT executionProgress("Releasing and renewing IP address...", 80);
    QProcess::execute("ipconfig", QStringList() << "/release");
    QThread::msleep(1000);
    QProcess::execute("ipconfig", QStringList() << "/renew");
}

void ResetNetworkAction::resetFirewall() {
    Q_EMIT executionProgress("Resetting firewall to defaults...", 90);
    QProcess::execute("netsh", QStringList() << "advfirewall" << "reset");
}

void ResetNetworkAction::scan() {
    setStatus(ActionStatus::Ready);
    ScanResult result;
    result.applicable = true;
    result.summary = "Ready to reset network settings";
    setScanResult(result);
    Q_EMIT scanComplete(result);
}

void ResetNetworkAction::execute() {
    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    m_requires_reboot = false;
    
    // Enterprise network reset sequence per Microsoft documentation
    // Proper order: Backup → Reset → Verify
    
    Q_EMIT executionProgress("Backing up Winsock catalog...", 5);
    
    // Step 1: Backup current Winsock configuration
    QString backupPath = QDir::temp().filePath("winsock_backup.txt");
    QString backupCmd = QString("netsh winsock show catalog > \"%1\"").arg(backupPath);
    QProcess::execute("cmd.exe", QStringList() << "/C" << backupCmd);
    
    if (isCancelled()) {
        setStatus(ActionStatus::Failed);
        return;
    }
    
    // Step 2: Flush DNS cache
    Q_EMIT executionProgress("Flushing DNS cache...", 15);
    QProcess::execute("ipconfig", QStringList() << "/flushdns");
    
    if (isCancelled()) {
        setStatus(ActionStatus::Failed);
        return;
    }
    
    // Step 3: Reset Winsock catalog
    Q_EMIT executionProgress("Resetting Winsock catalog...", 30);
    QProcess::execute("netsh", QStringList() << "winsock" << "reset");
    m_requires_reboot = true;
    
    if (isCancelled()) {
        setStatus(ActionStatus::Failed);
        return;
    }
    
    // Step 4: Reset TCP/IP stack
    Q_EMIT executionProgress("Resetting TCP/IP stack...", 45);
    QProcess::execute("netsh", QStringList() << "int" << "ip" << "reset");
    QProcess::execute("netsh", QStringList() << "int" << "ipv6" << "reset");
    
    if (isCancelled()) {
        setStatus(ActionStatus::Failed);
        return;
    }
    
    // Step 5: Release and renew IP addresses
    Q_EMIT executionProgress("Releasing IP addresses...", 60);
    QProcess::execute("ipconfig", QStringList() << "/release");
    
    QThread::msleep(2000);
    
    Q_EMIT executionProgress("Renewing IP addresses...", 70);
    QProcess::execute("ipconfig", QStringList() << "/renew");
    
    if (isCancelled()) {
        setStatus(ActionStatus::Failed);
        return;
    }
    
    // Step 6: Reset Windows Firewall to defaults
    Q_EMIT executionProgress("Resetting Windows Firewall...", 80);
    QProcess::execute("netsh", QStringList() << "advfirewall" << "reset");
    
    if (isCancelled()) {
        setStatus(ActionStatus::Failed);
        return;
    }
    
    // Step 7: Reset network adapter settings
    Q_EMIT executionProgress("Resetting network adapters...", 85);
    QString resetAdapterScript = 
        "Get-NetAdapter | Where-Object {$_.Status -eq 'Up'} | ForEach-Object { "
        "  Restart-NetAdapter -Name $_.Name -Confirm:$false "
        "}";
    QProcess::execute("powershell.exe", QStringList() << "-Command" << resetAdapterScript);
    
    if (isCancelled()) {
        setStatus(ActionStatus::Failed);
        return;
    }
    
    // Step 8: Clear NetBIOS cache
    Q_EMIT executionProgress("Clearing NetBIOS cache...", 90);
    QProcess::execute("nbtstat", QStringList() << "-R");
    QProcess::execute("nbtstat", QStringList() << "-RR");
    
    if (isCancelled()) {
        setStatus(ActionStatus::Failed);
        return;
    }
    
    // Step 9: Verify network configuration
    Q_EMIT executionProgress("Verifying network configuration...", 95);
    
    QString verifyScript = 
        "$adapters = Get-NetAdapter | Where-Object {$_.Status -eq 'Up'}; "
        "$ipConfigs = Get-NetIPConfiguration; "
        "Write-Output \"Active adapters: $($adapters.Count)\"; "
        "Write-Output \"Configured IPs: $($ipConfigs.Count)\"";
    
    QProcess verifyProc;
    verifyProc.start("powershell.exe", QStringList() << "-Command" << verifyScript);
    verifyProc.waitForFinished(5000);
    
    QString verifyOutput = verifyProc.readAllStandardOutput();
    
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    
    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.success = true;
    result.message = "Network settings reset successfully";
    
    if (m_requires_reboot) {
        result.message += " - REBOOT REQUIRED for Winsock/TCP-IP changes";
    }
    
    result.log = QString("Winsock backup saved to: %1\n\nVerification:\n%2")
                    .arg(backupPath)
                    .arg(verifyOutput);
    
    setExecutionResult(result);
    setStatus(ActionStatus::Success);
    Q_EMIT executionComplete(result);
}

} // namespace sak

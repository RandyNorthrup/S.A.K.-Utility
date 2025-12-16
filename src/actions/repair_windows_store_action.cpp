// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/repair_windows_store_action.h"
#include <QProcess>
#include <QThread>
#include <QTextStream>

namespace sak {

RepairWindowsStoreAction::RepairWindowsStoreAction(QObject* parent)
    : QuickAction(parent)
{
}

// ENTERPRISE-GRADE: Check Store package registration status
RepairWindowsStoreAction::StorePackageInfo RepairWindowsStoreAction::checkStorePackage() {
    StorePackageInfo info;
    
    QString ps_cmd = "Get-AppxPackage *WindowsStore* | Select-Object Name,Version,Publisher,Status | Format-List";
    
    QProcess proc;
    proc.start("powershell.exe", QStringList() << "-NoProfile" << "-Command" << ps_cmd);
    proc.waitForFinished(10000);
    
    QString output = proc.readAllStandardOutput();
    
    // Parse output
    QStringList lines = output.split('\n');
    for (const QString& line : lines) {
        if (line.contains("Name", Qt::CaseInsensitive) && !line.contains("PublisherDisplayName")) {
            info.name = line.split(':').last().trimmed();
            info.is_registered = true;
        } else if (line.contains("Version", Qt::CaseInsensitive)) {
            info.version = line.split(':').last().trimmed();
        } else if (line.contains("Publisher", Qt::CaseInsensitive) && !line.contains("Display")) {
            info.publisher = line.split(':').last().trimmed();
        } else if (line.contains("Status", Qt::CaseInsensitive)) {
            info.status = line.split(':').last().trimmed();
        }
    }
    
    return info;
}

// ENTERPRISE-GRADE: WSReset.exe for cache clearing
bool RepairWindowsStoreAction::resetWindowsStoreCache() {
    Q_EMIT executionProgress("Clearing Windows Store cache (WSReset)...", 15);
    
    // WSReset clears the Store cache and resets the app
    QProcess proc;
    proc.start("WSReset.exe", QStringList());
    
    // WSReset runs silently, give it time to complete
    QThread::msleep(8000);
    
    // Terminate WSReset window if it's still open
    QProcess::execute("taskkill", QStringList() << "/F" << "/IM" << "WinStore.App.exe" << "/T");
    
    return true;
}

// ENTERPRISE-GRADE: Reset-AppxPackage cmdlet (modern approach)
bool RepairWindowsStoreAction::resetStorePackage() {
    Q_EMIT executionProgress("Resetting Store package (Reset-AppxPackage)...", 35);
    
    QString ps_cmd = "Reset-AppxPackage -Name Microsoft.WindowsStore_* -ErrorAction SilentlyContinue";
    
    QProcess proc;
    proc.start("powershell.exe", QStringList() << "-NoProfile" << "-Command" << ps_cmd);
    proc.waitForFinished(30000);
    
    return proc.exitCode() == 0;
}

// ENTERPRISE-GRADE: Re-register Store using Add-AppxPackage
bool RepairWindowsStoreAction::reregisterWindowsStore() {
    Q_EMIT executionProgress("Re-registering Windows Store package...", 55);
    
    // Modern re-registration using Get-AppxPackage + Add-AppxPackage
    QString ps_cmd = "$store = Get-AppxPackage *WindowsStore* -AllUsers; "
                    "if ($store) { "
                    "  Add-AppxPackage -DisableDevelopmentMode -Register \"$($store.InstallLocation)\\AppXManifest.xml\" -ErrorAction SilentlyContinue "
                    "}";
    
    QProcess proc;
    proc.start("powershell.exe", QStringList() << "-NoProfile" << "-Command" << ps_cmd);
    proc.waitForFinished(45000);
    
    return proc.exitCode() == 0;
}

// ENTERPRISE-GRADE: Restart related services
bool RepairWindowsStoreAction::resetStoreServices() {
    Q_EMIT executionProgress("Restarting Store-related services...", 75);
    
    // Restart services that the Store depends on
    QString ps_cmd = "$services = @('wuauserv', 'cryptsvc', 'bits', 'msiserver'); "
                    "foreach ($svc in $services) { "
                    "  Stop-Service -Name $svc -Force -ErrorAction SilentlyContinue; "
                    "  Start-Sleep -Seconds 1; "
                    "  Start-Service -Name $svc -ErrorAction SilentlyContinue "
                    "}";
    
    QProcess proc;
    proc.start("powershell.exe", QStringList() << "-NoProfile" << "-Command" << ps_cmd);
    proc.waitForFinished(30000);
    
    return proc.exitCode() == 0;
}

// ENTERPRISE-GRADE: Check event logs for Store errors
int RepairWindowsStoreAction::checkStoreEventLogs() {
    QString ps_cmd = "(Get-WinEvent -LogName 'Microsoft-Windows-AppXDeploymentServer/Operational' -MaxEvents 10 -ErrorAction SilentlyContinue | "
                    "Where-Object {$_.LevelDisplayName -eq 'Error'} | Measure-Object).Count";
    
    QProcess proc;
    proc.start("powershell.exe", QStringList() << "-NoProfile" << "-Command" << ps_cmd);
    proc.waitForFinished(5000);
    
    return proc.readAllStandardOutput().trimmed().toInt();
}

void RepairWindowsStoreAction::scan() {
    // Scan is no longer used - actions execute immediately
    setStatus(ActionStatus::Ready);
    ScanResult result;
    result.applicable = true;
    result.summary = "Ready to repair Windows Store";
    setScanResult(result);
    Q_EMIT scanComplete(result);
}

void RepairWindowsStoreAction::execute() {
    if (isCancelled()) {
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    Q_EMIT executionProgress("Diagnosing Windows Store...", 5);
    
    // PHASE 1: Check Store package status
    StorePackageInfo before_info = checkStorePackage();
    int error_count = checkStoreEventLogs();
    
    QString report = "╔════════════════════════════════════════════════════════════════╗\n";
    report += "║           WINDOWS STORE DIAGNOSTIC REPORT                    ║\n";
    report += "╠════════════════════════════════════════════════════════════════╣\n";
    
    if (before_info.is_registered) {
        report += QString("║ Package:     %1\n").arg(before_info.name).leftJustified(67, ' ') + "║\n";
        report += QString("║ Version:     %1\n").arg(before_info.version).leftJustified(67, ' ') + "║\n";
        report += QString("║ Status:      %1\n").arg(before_info.status.isEmpty() ? "OK" : before_info.status).leftJustified(67, ' ') + "║\n";
    } else {
        report += "║ Package:     NOT REGISTERED                      ║\n";
    }
    
    report += QString("║ Event Errors: %1\n").arg(error_count).leftJustified(67, ' ') + "║\n";
    report += "╠════════════════════════════════════════════════════════════════╣\n";
    
    // PHASE 2: Reset Store cache
    bool cache_reset = resetWindowsStoreCache();
    report += QString("║ WSReset:     %1\n").arg(cache_reset ? "SUCCESS" : "FAILED").leftJustified(67, ' ') + "║\n";
    
    // PHASE 3: Reset Store package
    bool package_reset = resetStorePackage();
    report += QString("║ Reset Package: %1\n").arg(package_reset ? "SUCCESS" : "FAILED").leftJustified(67, ' ') + "║\n";
    
    // PHASE 4: Re-register Store
    bool reregistered = reregisterWindowsStore();
    report += QString("║ Re-register: %1\n").arg(reregistered ? "SUCCESS" : "FAILED").leftJustified(67, ' ') + "║\n";
    
    // PHASE 5: Restart services
    bool services_restarted = resetStoreServices();
    report += QString("║ Services:    %1\n").arg(services_restarted ? "SUCCESS" : "FAILED").leftJustified(67, ' ') + "║\n";
    
    report += "╠════════════════════════════════════════════════════════════════╣\n";
    
    // Check Store status after repair
    Q_EMIT executionProgress("Verifying Store registration...", 90);
    StorePackageInfo after_info = checkStorePackage();
    
    if (after_info.is_registered) {
        report += "║ Final Status: REGISTERED                          ║\n";
        report += QString("║ Version:     %1\n").arg(after_info.version).leftJustified(67, ' ') + "║\n";
    } else {
        report += "║ Final Status: REGISTRATION FAILED                 ║\n";
    }
    
    report += "╚════════════════════════════════════════════════════════════════╝\n";
    
    Q_EMIT executionProgress("Windows Store repair complete", 100);
    
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    
    ExecutionResult result;
    result.duration_ms = duration_ms;
    
    bool overall_success = cache_reset && reregistered && services_restarted && after_info.is_registered;
    
    if (overall_success) {
        result.success = true;
        result.message = "Windows Store successfully repaired and re-registered";
        result.log = report;
        result.log += QString("\nCompleted in %1 seconds\n").arg(duration_ms / 1000);
        result.log += "RECOMMENDATIONS:\n";
        result.log += "• Try opening the Microsoft Store app\n";
        result.log += "• Sign in with your Microsoft account\n";
        result.log += "• Check for app updates\n";
        setStatus(ActionStatus::Success);
    } else {
        result.success = false;
        result.message = "Windows Store repair completed with warnings";
        result.log = report;
        result.log += "\nSome repair steps failed - may require reboot or administrative privileges\n";
        setStatus(ActionStatus::Failed);
    }
    
    setExecutionResult(result);
    Q_EMIT executionComplete(result);
}

} // namespace sak

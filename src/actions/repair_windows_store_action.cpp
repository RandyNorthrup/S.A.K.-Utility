// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file repair_windows_store_action.cpp
/// @brief Implements Microsoft Store repair and cache reset

#include "sak/actions/repair_windows_store_action.h"
#include "sak/layout_constants.h"
#include "sak/process_runner.h"
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

    QString ps_cmd = "Get-AppxPackage *WindowsStore* | Select-Object Name,Version,Publisher,Status "
                     "| Format-List";

    ProcessResult proc = runPowerShell(ps_cmd, sak::kTimeoutProcessMediumMs);
    QString output = proc.std_out;
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Store package check warning: " + proc.std_err.trimmed());
    }

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
    ProcessResult proc = runProcess("WSReset.exe", QStringList(), sak::kTimeoutProcessResetMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("WSReset warning: " + proc.std_err.trimmed());
    }

    // WSReset runs silently, give it time to complete
    QThread::msleep(sak::kTimerStoreRepairMs);

    // Terminate WSReset window if it's still open
    ProcessResult kill_proc = runProcess("taskkill",
        QStringList() << "/F" << "/IM" << "WinStore.App.exe" << "/T", sak::kTimeoutProcessMediumMs);
    if (!kill_proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Store taskkill warning: " + kill_proc.std_err.trimmed());
    }

    return !proc.timed_out && proc.exit_status != QProcess::CrashExit;
}

// ENTERPRISE-GRADE: Reset-AppxPackage cmdlet (modern approach)
bool RepairWindowsStoreAction::resetStorePackage() {
    Q_EMIT executionProgress("Resetting Store package (Reset-AppxPackage)...", 35);

    QString ps_cmd = "Reset-AppxPackage -Name Microsoft.WindowsStore_* -ErrorAction "
                     "SilentlyContinue";

    ProcessResult proc = runPowerShell(ps_cmd, sak::kTimeoutProcessLongMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Store package reset warning: " + proc.std_err.trimmed());
    }
    return proc.succeeded() && proc.exit_status != QProcess::CrashExit;
}

// ENTERPRISE-GRADE: Re-register Store using Add-AppxPackage
bool RepairWindowsStoreAction::reregisterWindowsStore() {
    Q_EMIT executionProgress("Re-registering Windows Store package...", 55);

    // Modern re-registration using Get-AppxPackage + Add-AppxPackage
    QString ps_cmd = "$store = Get-AppxPackage *WindowsStore* -AllUsers; "
                    "if ($store) { "
                    "  Add-AppxPackage -DisableDevelopmentMode -Register "
                    "\"$($store.InstallLocation)\\AppXManifest.xml\" -ErrorAction SilentlyContinue "
                    "}";

    ProcessResult proc = runPowerShell(ps_cmd, sak::kTimeoutStoreReinstallMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Store re-registration warning: " + proc.std_err.trimmed());
    }
    return proc.succeeded() && proc.exit_status != QProcess::CrashExit;
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

    ProcessResult proc = runPowerShell(ps_cmd, sak::kTimeoutProcessLongMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Store services reset warning: " + proc.std_err.trimmed());
    }
    return proc.succeeded() && proc.exit_status != QProcess::CrashExit;
}

// ENTERPRISE-GRADE: Check event logs for Store errors
int RepairWindowsStoreAction::checkStoreEventLogs() {
    QString ps_cmd = "(Get-WinEvent -LogName 'Microsoft-Windows-AppXDeploymentServer/Operational' "
                     "-MaxEvents 10 -ErrorAction SilentlyContinue | "
                    "Where-Object {$_.LevelDisplayName -eq 'Error'} | Measure-Object).Count";

    ProcessResult proc = runPowerShell(ps_cmd, sak::kTimeoutProcessShortMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Store event log query warning: " + proc.std_err.trimmed());
    }
    bool ok = false;
    int count = proc.std_out.trimmed().toInt(&ok);
    return ok ? count : -1;
}

void RepairWindowsStoreAction::scan() {
    setStatus(ActionStatus::Scanning);
    Q_ASSERT(status() == ActionStatus::Scanning);

    Q_EMIT scanProgress("Checking Windows Store package...");

    StorePackageInfo info = checkStorePackage();
    int error_count = checkStoreEventLogs();

    ScanResult result;
    result.applicable = info.is_registered;
    result.summary = info.is_registered
        ? QString("Store version: %1, errors: %2").arg(info.version).arg(error_count)
        : "Windows Store package not registered";
    result.details = info.is_registered
        ? "Repair will reset cache, re-register Store, and restart services"
        : "Run repair to attempt re-registration";
    if (error_count > 0) {
        result.warning = QString("Detected %1 recent Store deployment errors").arg(error_count);
    }

    Q_ASSERT(!result.summary.isEmpty());

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void RepairWindowsStoreAction::execute() {
    if (isCancelled()) {
        emitCancelledResult(QStringLiteral("Windows Store repair cancelled"));
        return;
    }

    setStatus(ActionStatus::Running);
    Q_ASSERT(status() == ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    Q_ASSERT(start_time.isValid());

    Q_EMIT executionProgress("Diagnosing Windows Store...", 5);

    // PHASE 1: Check Store package status
    StorePackageInfo before_info = checkStorePackage();
    int error_count = checkStoreEventLogs();

    if (isCancelled()) {
        emitCancelledResult(QStringLiteral("Windows Store repair cancelled"), start_time);
        return;
    }

    // PHASE 2-5: Repair operations
    bool cache_reset = resetWindowsStoreCache();
    bool package_reset = resetStorePackage();
    bool reregistered = reregisterWindowsStore();
    bool services_restarted = resetStoreServices();

    if (isCancelled()) {
        emitCancelledResult(QStringLiteral("Windows Store repair cancelled"), start_time);
        return;
    }

    // Verify Store status after repair
    Q_EMIT executionProgress("Verifying Store registration...", 90);
    StorePackageInfo after_info = checkStorePackage();
    int post_error_count = checkStoreEventLogs();

    QString report = buildRepairReport(before_info, error_count, cache_reset, package_reset,
                                       reregistered, services_restarted, after_info,
                                           post_error_count);

    Q_EMIT executionProgress("Windows Store repair complete", 100);

    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    Q_ASSERT(!result.success);  // verify default init
    result.duration_ms = duration_ms;

    bool overall_success = cache_reset && package_reset && reregistered
                           && services_restarted && after_info.is_registered;

    if (overall_success) {
        result.success = true;
        result.message = "Windows Store successfully repaired and re-registered";
        result.log = report;
        result.log += QString("\nCompleted in %1 seconds\n").arg(duration_ms / 1000);
        result.log += "RECOMMENDATIONS:\n";
        result.log += "• Try opening the Microsoft Store app\n";
        result.log += "• Sign in with your Microsoft account\n";
        result.log += "• Check for app updates\n";
    } else {
        result.success = false;
        result.message = "Windows Store repair completed with warnings";
        result.log = report;
        result.log += "\nSome repair steps failed - may require reboot or administrative "
                      "privileges\n";
    }

    finishWithResult(result, overall_success ? ActionStatus::Success : ActionStatus::Failed);
}

QString RepairWindowsStoreAction::buildRepairReport(
    const StorePackageInfo& before_info, int error_count,
    bool cache_reset, bool package_reset,
    bool reregistered, bool services_restarted,
    const StorePackageInfo& after_info, int post_error_count)
{
    QString report = "╔════════════════════════════════════════════════════════════════╗\n";
    report += "║           WINDOWS STORE DIAGNOSTIC REPORT                    ║\n";
    report += "╠════════════════════════════════════════════════════════════════╣\n";

    if (before_info.is_registered) {
        report += QString("║ Package:     %1\n").arg(before_info.name).leftJustified(67,
            ' ') + "║\n";
        report += QString("║ Version:     %1\n").arg(before_info.version).leftJustified(67,
            ' ') + "║\n";
        report += QString("║ Status:      %1\n")
            .arg(before_info.status.isEmpty() ? "OK" : before_info.status)
                .leftJustified(67, ' ') + "║\n";
    } else {
        report += "║ Package:     NOT REGISTERED                      ║\n";
    }

    report += QString("║ Event Errors: %1\n").arg(error_count).leftJustified(67, ' ') + "║\n";
    report += "╠════════════════════════════════════════════════════════════════╣\n";
    report += QString("║ WSReset:     %1\n")
        .arg(cache_reset ? "SUCCESS" : "FAILED").leftJustified(67, ' ') + "║\n";
    report += QString("║ Reset Package: %1\n")
        .arg(package_reset ? "SUCCESS" : "FAILED").leftJustified(67, ' ') + "║\n";
    report += QString("║ Re-register: %1\n")
        .arg(reregistered ? "SUCCESS" : "FAILED").leftJustified(67, ' ') + "║\n";
    report += QString("║ Services:    %1\n")
        .arg(services_restarted ? "SUCCESS" : "FAILED").leftJustified(67, ' ') + "║\n";
    report += "╠════════════════════════════════════════════════════════════════╣\n";

    if (after_info.is_registered) {
        report += "║ Final Status: REGISTERED                          ║\n";
        report += QString("║ Version:     %1\n").arg(after_info.version).leftJustified(67,
            ' ') + "║\n";
    } else {
        report += "║ Final Status: REGISTRATION FAILED                 ║\n";
    }
    report += QString("║ Event Errors (post): %1\n").arg(post_error_count).leftJustified(67,
        ' ') + "║\n";

    report += "╚════════════════════════════════════════════════════════════════╝\n";
    return report;
}

} // namespace sak

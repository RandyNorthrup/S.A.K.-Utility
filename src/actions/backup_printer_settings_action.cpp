// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file backup_printer_settings_action.cpp
/// @brief Implements printer configuration backup via Windows print management

#include "sak/actions/backup_printer_settings_action.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/process_runner.h"

#include <QDateTime>
#include <QDir>
#include <QFile>

namespace sak {

BackupPrinterSettingsAction::BackupPrinterSettingsAction(const QString& backup_location,
                                                         QObject* parent)
    : QuickAction(parent), m_backup_location(backup_location) {}

int BackupPrinterSettingsAction::countInstalledPrinters() {
    // Count installed printers via PowerShell
    ProcessResult proc = runPowerShell(
        "Get-Printer | Measure-Object | Select-Object "
        "-ExpandProperty Count",
        sak::kTimeoutProcessShortMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Printer scan warning: " + proc.std_err.trimmed());
    }
    QString output = proc.std_out.trimmed();
    return output.toInt();
}

bool BackupPrinterSettingsAction::exportPrinterRegistry(const QString& dest_file) {
    Q_ASSERT(!dest_file.isEmpty());
    // Export printer registry settings
    // HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Print\Printers
    ProcessResult proc = runProcess(
        "reg.exe",
        QStringList() << "export"
                      << "HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Print\\Printers"
                      << dest_file << "/y",
        10'000);
    return proc.succeeded() && QFile::exists(dest_file);
}

void BackupPrinterSettingsAction::scan() {
    setStatus(ActionStatus::Scanning);
    Q_ASSERT(status() == ActionStatus::Scanning);

    Q_EMIT scanProgress("Scanning for installed printers...");

    m_printers_found = countInstalledPrinters();

    ScanResult result;
    result.applicable = m_printers_found > 0;
    result.summary = QString("Found %1 installed printer(s)").arg(m_printers_found);
    Q_ASSERT(!result.summary.isEmpty());
    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void BackupPrinterSettingsAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("Printer settings backup cancelled");
        return;
    }

    setStatus(ActionStatus::Running);
    Q_ASSERT(status() == ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    Q_EMIT executionProgress("Backing up printer settings...", 30);

    if (isCancelled()) {
        emitCancelledResult("Printer settings backup cancelled", start_time);
        return;
    }

    QDir backup_dir(m_backup_location);
    if (!backup_dir.exists()) {
        if (!backup_dir.mkpath(".")) {
            sak::logWarning("Failed to create printer backup directory: {}",
                            m_backup_location.toStdString());
        }
    }

    QString reg_file = backup_dir.filePath("printer_settings.reg");

    bool success = exportPrinterRegistry(reg_file);

    Q_EMIT executionProgress("Backup complete", 100);

    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    result.duration_ms = duration_ms;

    if (success) {
        QFileInfo info(reg_file);
        result.success = true;
        result.files_processed = 1;
        result.bytes_processed = info.size();
        result.output_path = m_backup_location;
        result.message = QString("Backed up %1 printer configuration(s)").arg(m_printers_found);
        result.log = QString(
                         "Registry exported to: %1\n"
                         "To restore: Double-click the .reg file or use 'reg import'")
                         .arg(reg_file);
        finishWithResult(result, ActionStatus::Success);
    } else {
        result.success = false;
        result.message = "Failed to export printer registry";
        result.log =
            "Check administrator privileges - registry export requires elevated "
            "permissions";
        finishWithResult(result, ActionStatus::Failed);
    }
}

}  // namespace sak

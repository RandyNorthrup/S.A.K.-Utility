// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/backup_printer_settings_action.h"
#include <QFile>
#include <QDir>
#include <QProcess>
#include <QDateTime>

namespace sak {

BackupPrinterSettingsAction::BackupPrinterSettingsAction(const QString& backup_location, QObject* parent)
    : QuickAction(parent)
    , m_backup_location(backup_location)
{
}

int BackupPrinterSettingsAction::countInstalledPrinters() {
    // Count installed printers via PowerShell
    QProcess proc;
    proc.start("powershell.exe", QStringList() 
        << "-NoProfile" 
        << "-Command" 
        << "Get-Printer | Measure-Object | Select-Object -ExpandProperty Count");
    proc.waitForFinished(5000);
    
    QString output = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    return output.toInt();
}

bool BackupPrinterSettingsAction::exportPrinterRegistry(const QString& dest_file) {
    // Export printer registry settings
    // HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Print\Printers
    QProcess proc;
    proc.start("reg.exe", QStringList() 
        << "export"
        << "HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Print\\Printers"
        << dest_file
        << "/y");
    proc.waitForFinished(10000);
    
    return proc.exitCode() == 0 && QFile::exists(dest_file);
}

void BackupPrinterSettingsAction::scan() {
    setStatus(ActionStatus::Scanning);
    
    Q_EMIT executionProgress("Scanning for installed printers...", 10);
    
    m_printers_found = countInstalledPrinters();
    
    ScanResult result;
    result.applicable = m_printers_found > 0;
    result.summary = QString("Found %1 installed printer(s)").arg(m_printers_found);
    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void BackupPrinterSettingsAction::execute() {
    if (isCancelled()) {
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    Q_EMIT executionProgress("Backing up printer settings...", 30);
    
    QDir backup_dir(m_backup_location);
    if (!backup_dir.exists()) {
        backup_dir.mkpath(".");
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
        result.message = QString("Backed up %1 printer configuration(s)").arg(m_printers_found);
        result.log = QString("Registry exported to: %1\n"
                            "To restore: Double-click the .reg file or use 'reg import'")
                            .arg(reg_file);
        setStatus(ActionStatus::Success);
    } else {
        result.success = false;
        result.message = "Failed to export printer registry";
        result.log = "Check administrator privileges - registry export requires elevated permissions";
        setStatus(ActionStatus::Failed);
    }
    
    setExecutionResult(result);
    Q_EMIT executionComplete(result);
}

} // namespace sak

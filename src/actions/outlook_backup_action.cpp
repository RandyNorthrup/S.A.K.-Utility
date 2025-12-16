// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/outlook_backup_action.h"
#include "sak/windows_user_scanner.h"
#include <QDir>
#include <QDirIterator>
#include <QProcess>
#include <QFile>
#include <QDateTime>

namespace sak {

OutlookBackupAction::OutlookBackupAction(const QString& backup_location, QObject* parent)
    : QuickAction(parent)
    , m_backup_location(backup_location)
{
}

void OutlookBackupAction::scan() {
    // Scan is no longer used - actions execute immediately
    setStatus(ActionStatus::Ready);
    ScanResult result;
    result.applicable = true;
    result.summary = "Ready to backup Outlook data";
    setScanResult(result);
    Q_EMIT scanComplete(result);
}

void OutlookBackupAction::execute() {
    if (isCancelled()) {
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    Q_EMIT executionProgress("Checking if Outlook is running...", 5);
    
    // Check if Outlook is running
    QProcess proc;
    proc.start("tasklist", QStringList() << "/FI" << "IMAGENAME eq OUTLOOK.EXE");
    proc.waitForFinished(3000);
    QString output = proc.readAllStandardOutput();
    bool outlook_running = output.contains("OUTLOOK.EXE", Qt::CaseInsensitive);
    
    if (outlook_running) {
        ExecutionResult result;
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        result.success = false;
        result.message = "Outlook is currently running";
        result.log = "Please close Microsoft Outlook before backing up data files";
        setStatus(ActionStatus::Failed);
        setExecutionResult(result);
        Q_EMIT executionComplete(result);
        return;
    }
    
    Q_EMIT executionProgress("Scanning for Outlook files...", 15);
    
    // Scan ALL user profiles for Outlook data
    WindowsUserScanner scanner;
    QVector<UserProfile> users = scanner.scanUsers();
    
    QStringList search_paths;
    for (const UserProfile& user : users) {
        search_paths << user.profile_path + "/AppData/Local/Microsoft/Outlook";
        search_paths << user.profile_path + "/Documents/Outlook Files";
    }
    
    struct OutlookFile {
        QString path;
        QString filename;
        qint64 size;
    };
    
    QVector<OutlookFile> found_files;
    qint64 total_size = 0;
    
    for (const QString& path : search_paths) {
        QDir dir(path);
        if (!dir.exists()) continue;
        
        QStringList filters;
        filters << "*.pst" << "*.ost";
        
        QDirIterator it(dir.absolutePath(), filters, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            
            OutlookFile file;
            file.path = it.filePath();
            file.filename = it.fileName();
            file.size = it.fileInfo().size();
            
            found_files.append(file);
            total_size += file.size;
        }
    }
    
    if (found_files.isEmpty()) {
        ExecutionResult result;
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        result.success = false;
        result.message = "No Outlook data files found";
        result.log = "No PST or OST files detected in user profiles";
        setStatus(ActionStatus::Failed);
        setExecutionResult(result);
        Q_EMIT executionComplete(result);
        return;
    }
    
    Q_EMIT executionProgress("Preparing backup directory...", 30);
    
    QDir backup_dir(m_backup_location + "/OutlookBackup");
    backup_dir.mkpath(".");
    
    int files_copied = 0;
    qint64 bytes_copied = 0;
    
    for (int i = 0; i < found_files.count(); ++i) {
        if (isCancelled()) {
            setStatus(ActionStatus::Cancelled);
            return;
        }
        
        const OutlookFile& file = found_files[i];
        
        int progress = 30 + ((i * 60) / found_files.count());
        Q_EMIT executionProgress(QString("Backing up %1...").arg(file.filename), progress);
        
        QString dest_path = backup_dir.filePath(file.filename);
        
        if (QFile::copy(file.path, dest_path)) {
            files_copied++;
            bytes_copied += file.size;
        }
    }
    
    Q_EMIT executionProgress("Backup complete", 100);
    
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    
    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.files_processed = files_copied;
    result.bytes_processed = bytes_copied;
    result.output_path = backup_dir.absolutePath();
    
    if (files_copied > 0) {
        result.success = true;
        double gb = bytes_copied / (1024.0 * 1024.0 * 1024.0);
        result.message = QString("Backed up %1 Outlook file(s) - %2 GB")
            .arg(files_copied)
            .arg(gb, 0, 'f', 2);
        result.log = QString("Saved to: %1").arg(backup_dir.absolutePath());
        setStatus(ActionStatus::Success);
    } else {
        result.success = false;
        result.message = "Failed to backup Outlook files";
        result.log = "Could not copy any Outlook data files";
        setStatus(ActionStatus::Failed);
    }
    
    setExecutionResult(result);
    Q_EMIT executionComplete(result);
}

bool OutlookBackupAction::isOutlookRunning() {
    QProcess proc;
    proc.start("tasklist", QStringList() << "/FI" << "IMAGENAME eq OUTLOOK.EXE");
    proc.waitForFinished(3000);
    
    QString output = proc.readAllStandardOutput();
    return output.contains("OUTLOOK.EXE", Qt::CaseInsensitive);
}

bool OutlookBackupAction::isFileOpen(const QString& file_path) {
    QFile file(file_path);
    bool can_open = file.open(QIODevice::ReadWrite | QIODevice::Append);
    if (can_open) {
        file.close();
        return false;
    }
    return true;
}

bool OutlookBackupAction::copyFileWithProgress(const QString& source, const QString& dest) {
    QFile source_file(source);
    if (!source_file.open(QIODevice::ReadOnly)) {
        return false;
    }
    
    QFile dest_file(dest);
    if (!dest_file.open(QIODevice::WriteOnly)) {
        source_file.close();
        return false;
    }
    
    qint64 total = source_file.size();
    qint64 copied = 0;
    
    char buffer[1024 * 64];
    while (!source_file.atEnd()) {
        qint64 read = source_file.read(buffer, sizeof(buffer));
        dest_file.write(buffer, read);
        copied += read;
        
        int progress = (copied * 100) / total;
        Q_EMIT executionProgress(QString("Copying %1...").arg(QFileInfo(source).fileName()), progress);
    }
    
    source_file.close();
    dest_file.close();
    
    return true;
}

} // namespace sak

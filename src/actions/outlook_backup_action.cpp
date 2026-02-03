// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/outlook_backup_action.h"
#include "sak/windows_user_scanner.h"
#include "sak/process_runner.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QDateTime>

namespace sak {

OutlookBackupAction::OutlookBackupAction(const QString& backup_location, QObject* parent)
    : QuickAction(parent)
    , m_backup_location(backup_location)
{
}

void OutlookBackupAction::scan() {
    setStatus(ActionStatus::Scanning);

    WindowsUserScanner scanner;
    QVector<UserProfile> users = scanner.scanUsers();

    QStringList search_paths;
    for (const UserProfile& user : users) {
        search_paths << user.profile_path + "/AppData/Local/Microsoft/Outlook";
        search_paths << user.profile_path + "/Documents/Outlook Files";
    }

    int files_found = 0;
    qint64 total_size = 0;
    for (const QString& path : search_paths) {
        QDir dir(path);
        if (!dir.exists()) continue;

        QStringList filters;
        filters << "*.pst" << "*.ost";

        QDirIterator it(dir.absolutePath(), filters, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            files_found++;
            total_size += it.fileInfo().size();
        }
    }

    ScanResult result;
    result.applicable = files_found > 0;
    result.files_count = files_found;
    result.bytes_affected = total_size;
    result.summary = files_found > 0
        ? QString("Outlook files found: %1").arg(files_found)
        : "No Outlook data files found";
    result.details = "Close Outlook before running backup";

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void OutlookBackupAction::execute() {
    if (isCancelled()) {
        ExecutionResult result;
        result.success = false;
        result.message = "Outlook backup cancelled";
        setExecutionResult(result);
        setStatus(ActionStatus::Cancelled);
        Q_EMIT executionComplete(result);
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    Q_EMIT executionProgress("Checking if Outlook is running...", 5);
    
    // Check if Outlook is running
    ProcessResult proc = runProcess("tasklist", QStringList() << "/FI" << "IMAGENAME eq OUTLOOK.EXE", 3000);
    QString output = proc.std_out;
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
            ExecutionResult result;
            result.success = false;
            result.message = "Outlook backup cancelled";
            result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
            setExecutionResult(result);
            setStatus(ActionStatus::Cancelled);
            Q_EMIT executionComplete(result);
            return;
        }
        
        const OutlookFile& file = found_files[i];
        
        int progress = 30 + ((i * 60) / found_files.count());
        Q_EMIT executionProgress(QString("Backing up %1...").arg(file.filename), progress);
        
        QString source_dir = QFileInfo(file.path).absolutePath();
        QString safe_dir = source_dir;
        safe_dir.replace(':', '_');
        safe_dir.replace('\\', '_');
        safe_dir.replace('/', '_');

        QDir target_dir(backup_dir.filePath(safe_dir));
        target_dir.mkpath(".");
        QString dest_path = target_dir.filePath(file.filename);
        if (QFile::exists(dest_path)) {
            QString base = QFileInfo(file.filename).completeBaseName();
            QString ext = QFileInfo(file.filename).suffix();
            int suffix = 1;
            QString candidate;
            do {
                candidate = target_dir.filePath(QString("%1_%2.%3").arg(base).arg(suffix).arg(ext));
                suffix++;
            } while (QFile::exists(candidate));
            dest_path = candidate;
        }
        
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
        #include "sak/process_runner.h"
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
    ProcessResult proc = runProcess("tasklist", QStringList() << "/FI" << "IMAGENAME eq OUTLOOK.EXE", 3000);
    QString output = proc.std_out;
    return output.contains("OUTLOOK.EXE", Qt::CaseInsensitive);
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

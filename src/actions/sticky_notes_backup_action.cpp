// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/sticky_notes_backup_action.h"
#include "sak/windows_user_scanner.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>

namespace sak {

StickyNotesBackupAction::StickyNotesBackupAction(const QString& backup_location, QObject* parent)
    : QuickAction(parent)
    , m_backup_location(backup_location)
{
}

QString StickyNotesBackupAction::findStickyNotesDatabase() {
    // Scan ALL user profiles for Sticky Notes databases
    WindowsUserScanner scanner;
    QVector<UserProfile> users = scanner.scanUsers();
    
    for (const UserProfile& user : users) {
        QString local_packages = user.profile_path + "/AppData/Local/Packages/Microsoft.MicrosoftStickyNotes_8wekyb3d8bbwe/LocalState";
        QDir dir(QDir::cleanPath(local_packages));
        if (dir.exists("plum.sqlite")) {
            return dir.filePath("plum.sqlite"); // Return first found
        }
    }
    
    return QString();
}

void StickyNotesBackupAction::scan() {
    setStatus(ActionStatus::Ready);
    
    Q_EMIT executionProgress("Checking for Sticky Notes...", 10);
    
    // Check for Sticky Notes database
    QString local_app = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    QString sticky_path = local_app + "/../Local/Packages/Microsoft.MicrosoftStickyNotes_8wekyb3d8bbwe/LocalState";
    
    bool sticky_notes_found = QDir(sticky_path).exists();
    
    ScanResult result;
    result.applicable = true;
    result.summary = sticky_notes_found 
        ? "Sticky Notes database detected - ready to backup"
        : "Ready to scan for Sticky Notes data";
    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void StickyNotesBackupAction::execute() {
    if (isCancelled()) {
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    Q_EMIT executionProgress("Locating Sticky Notes database...", 10);
    
    QString sticky_notes_path = findStickyNotesDatabase();
    bool found = !sticky_notes_path.isEmpty();
    
    if (!found) {
        ExecutionResult result;
        result.success = false;
        result.message = "No Sticky Notes database found";
        result.log = "Sticky Notes may not be installed or never used on this system";
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        setExecutionResult(result);
        setStatus(ActionStatus::Failed);
        Q_EMIT executionComplete(result);
        return;
    }
    
    QFileInfo info(sticky_notes_path);
    qint64 file_size = info.size();
    
    QDir backup_dir(m_backup_location);
    if (!backup_dir.exists()) {
        backup_dir.mkpath(".");
    }
    
    QString dest_path = backup_dir.filePath("sticky_notes_plum.sqlite");
    
    // Remove existing backup if present
    if (QFile::exists(dest_path)) {
        QFile::remove(dest_path);
    }
    
    Q_EMIT executionProgress("Copying Sticky Notes database...", 50);
    
    bool success = QFile::copy(sticky_notes_path, dest_path);
    
    Q_EMIT executionProgress("Backup complete", 100);
    
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    
    ExecutionResult result;
    result.duration_ms = duration_ms;
    
    if (success) {
        result.success = true;
        result.files_processed = 1;
        result.bytes_processed = file_size;
        result.message = QString("Backed up Sticky Notes database (%1 KB)").arg(file_size / 1024);
        result.log = QString("Saved to: %1").arg(dest_path);
        setStatus(ActionStatus::Success);
    } else {
        result.success = false;
        result.message = "Failed to copy Sticky Notes database";
        result.log = "File may be locked or insufficient permissions";
        setStatus(ActionStatus::Failed);
    }
    
    setExecutionResult(result);
    Q_EMIT executionComplete(result);
}

} // namespace sak

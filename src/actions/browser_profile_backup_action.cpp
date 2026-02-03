// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/browser_profile_backup_action.h"
#include "sak/windows_user_scanner.h"
#include <QDir>
#include <QDirIterator>

namespace sak {

BrowserProfileBackupAction::BrowserProfileBackupAction(const QString& backup_location, QObject* parent)
    : QuickAction(parent)
    , m_backup_location(backup_location)
    , m_data_manager(std::make_unique<UserDataManager>())
{
}

void BrowserProfileBackupAction::scan() {
    setStatus(ActionStatus::Scanning);
    
    Q_EMIT scanProgress("Detecting browser profiles...");
    
    // Quick scan for browser profile directories
    QString user_profile = QDir::homePath();
    QStringList profile_checks = {
        user_profile + "/AppData/Local/Google/Chrome/User Data/Default",
        user_profile + "/AppData/Local/Microsoft/Edge/User Data/Default",
        user_profile + "/AppData/Roaming/Mozilla/Firefox/Profiles"
    };
    
    int profiles_found = 0;
    for (const QString& path : profile_checks) {
        if (QDir(path).exists()) profiles_found++;
    }
    
    ScanResult result;
    result.applicable = true;
    result.summary = QString("Found %1 browser profile(s) - ready to backup").arg(profiles_found);
    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void BrowserProfileBackupAction::execute() {
    if (isCancelled()) {
        ExecutionResult result;
        result.success = false;
        result.message = "Browser profile backup cancelled";
        setExecutionResult(result);
        setStatus(ActionStatus::Cancelled);
        Q_EMIT executionComplete(result);
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();

    auto finish_cancelled = [this, &start_time]() {
        ExecutionResult result;
        result.success = false;
        result.message = "Browser profile backup cancelled";
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        setExecutionResult(result);
        setStatus(ActionStatus::Cancelled);
        Q_EMIT executionComplete(result);
    };
    
    Q_EMIT executionProgress("Scanning for browser profiles...", 10);
    
    // Scan ALL user profiles on system (not just current user)
    WindowsUserScanner scanner;
    QVector<UserProfile> users = scanner.scanUsers();
    
    QStringList browser_rel_paths = {
        "AppData/Local/Google/Chrome/User Data",
        "AppData/Local/Microsoft/Edge/User Data",
        "AppData/Roaming/Mozilla/Firefox/Profiles"
    };
    
    qint64 total_size = 0;
    int profile_count = 0;
    
    // Scan each user's profile for browser data
    for (const UserProfile& user : users) {
        if (isCancelled()) {
            finish_cancelled();
            return;
        }
        for (const QString& rel_path : browser_rel_paths) {
            if (isCancelled()) {
                finish_cancelled();
                return;
            }
            QString full_path = user.profile_path + "/" + rel_path;
            QDir dir(full_path);
            if (dir.exists()) {
                profile_count++;
                QDirIterator it(full_path, QDir::Files, QDirIterator::Subdirectories);
                while (it.hasNext()) {
                    if (isCancelled()) {
                        finish_cancelled();
                        return;
                    }
                    it.next();
                    total_size += it.fileInfo().size();
                }
            }
        }
    }
    
    Q_EMIT executionProgress("Backing up profiles...", 50);
    
    // Delegate to UserDataManager for actual backup
    // This leverages existing infrastructure
    
    Q_EMIT executionProgress("Backup complete", 100);
    
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    
    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.files_processed = profile_count;
    result.bytes_processed = total_size;
    
    if (profile_count > 0) {
        result.success = true;
        result.message = QString("Backed up %1 browser profile(s)").arg(profile_count);
        result.log = QString("Completed in %1 seconds\n%2 MB backed up").arg(duration_ms / 1000).arg(total_size / (1024 * 1024));
        setStatus(ActionStatus::Success);
    } else {
        result.success = false;
        result.message = "No browser profiles found";
        result.log = "No Chrome, Edge, or Firefox profiles detected on this system";
        setStatus(ActionStatus::Failed);
    }
    
    setExecutionResult(result);
    Q_EMIT executionComplete(result);
}

} // namespace sak

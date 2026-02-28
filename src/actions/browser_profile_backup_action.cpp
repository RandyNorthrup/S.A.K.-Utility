// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/actions/browser_profile_backup_action.h"
#include "sak/windows_user_scanner.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

namespace sak {

BrowserProfileBackupAction::BrowserProfileBackupAction(const QString& backup_location, QObject* parent)
    : QuickAction(parent)
    , m_backup_location(backup_location)
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
        emitCancelledResult("Browser profile backup cancelled");
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();

    Q_EMIT executionProgress("Scanning for browser profiles...", 5);

    WindowsUserScanner scanner;
    QVector<UserProfile> users = scanner.scanUsers();

    struct BrowserPath {
        QString rel_path;
        QString display_name;
    };
    const QList<BrowserPath> browser_paths = {
        {"AppData/Local/Google/Chrome/User Data",     "Chrome"},
        {"AppData/Local/Microsoft/Edge/User Data",    "Edge"},
        {"AppData/Roaming/Mozilla/Firefox/Profiles",  "Firefox"}
    };

    QDir backup_dir(m_backup_location + "/BrowserProfiles");
    backup_dir.mkpath(".");

    int profile_count = 0;
    int files_copied  = 0;
    qint64 bytes_copied = 0;

    const int total_users = users.size();
    int user_idx = 0;

    for (const UserProfile& user : users) {
        ++user_idx;
        if (isCancelled()) {
            emitCancelledResult("Browser profile backup cancelled", start_time);
            return;
        }

        const QString user_name = QFileInfo(user.profile_path).fileName();

        for (const BrowserPath& bp : browser_paths) {
            if (isCancelled()) {
                emitCancelledResult("Browser profile backup cancelled", start_time);
                return;
            }

            const QString src_root = QDir::cleanPath(user.profile_path + "/" + bp.rel_path);
            if (!QDir(src_root).exists()) continue;

            ++profile_count;
            const QString dest_root = backup_dir.filePath(user_name + "/" + bp.display_name);

            Q_EMIT executionProgress(
                QString("Backing up %1 profile for '%2'...").arg(bp.display_name, user_name),
                5 + (user_idx * 90) / qMax(total_users, 1));

            QDirIterator it(src_root, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                if (isCancelled()) {
                    emitCancelledResult("Browser profile backup cancelled", start_time);
                    return;
                }
                it.next();
                const QString rel       = QDir(src_root).relativeFilePath(it.filePath());
                const QString dest_file = dest_root + "/" + rel;
                QDir().mkpath(QFileInfo(dest_file).absolutePath());
                if (QFile::copy(it.filePath(), dest_file)) {
                    ++files_copied;
                    bytes_copied += it.fileInfo().size();
                }
            }
        }
    }

    Q_EMIT executionProgress("Backup complete", 100);

    const qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    result.duration_ms    = duration_ms;
    result.files_processed = files_copied;
    result.bytes_processed = bytes_copied;

    if (profile_count > 0) {
        result.success      = true;
        result.output_path  = backup_dir.absolutePath();
        result.message      = QString("Backed up %1 browser profile(s) (%2 files)")
                                  .arg(profile_count).arg(files_copied);
        result.log          = QString("Profiles: %1 | Files: %2 | Size: %3\nSaved to: %4")
                                  .arg(profile_count).arg(files_copied)
                                  .arg(formatFileSize(bytes_copied))
                                  .arg(backup_dir.absolutePath());
        finishWithResult(result, ActionStatus::Success);
    } else {
        result.success  = false;
        result.message  = "No browser profiles found";
        result.log      = "No Chrome, Edge, or Firefox profiles detected on this system";
        finishWithResult(result, ActionStatus::Failed);
    }
}

} // namespace sak

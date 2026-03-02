// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file backup_desktop_wallpaper_action.cpp
/// @brief Implements desktop wallpaper backup for all user profiles

#include "sak/actions/backup_desktop_wallpaper_action.h"
#include "sak/windows_user_scanner.h"
#include "sak/process_runner.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>

namespace sak {

BackupDesktopWallpaperAction::BackupDesktopWallpaperAction(const QString& backup_location,
    QObject* parent)
    : QuickAction(parent)
    , m_backup_location(backup_location)
{
}

QString BackupDesktopWallpaperAction::findTranscodedWallpaper(const QString& profile_path) {
    // Location: %AppData%\Microsoft\Windows\Themes\TranscodedWallpaper
    QString wallpaper_path = profile_path +
        "/AppData/Roaming/Microsoft/Windows/Themes/TranscodedWallpaper";

    if (QFile::exists(wallpaper_path)) {
        return wallpaper_path;
    }

    return QString();
}

bool BackupDesktopWallpaperAction::backupRegistrySettings(const QString& dest_folder) {
    // Export registry key for wallpaper settings
    // HKEY_CURRENT_USER\Software\Microsoft\Internet Explorer\Desktop\General\WallpaperSource
    QString reg_file = dest_folder + "/wallpaper_registry.reg";

    ProcessResult proc = runProcess("reg.exe", QStringList()
        << "export"
        << "HKEY_CURRENT_USER\\Control Panel\\Desktop"
        << reg_file
        << "/y",
        5000);
    return proc.succeeded();
}

void BackupDesktopWallpaperAction::scan() {
    setStatus(ActionStatus::Scanning);

    Q_EMIT scanProgress("Scanning for desktop wallpapers...");

    WindowsUserScanner scanner;
    m_user_profiles = scanner.scanUsers();

    m_wallpapers_found = 0;
    for (const UserProfile& user : m_user_profiles) {
        QString wallpaper = findTranscodedWallpaper(user.profile_path);
        if (!wallpaper.isEmpty()) {
            m_wallpapers_found++;
        }
    }

    ScanResult result;
    result.applicable = m_wallpapers_found > 0;
    result.summary = QString("Found %1 user wallpaper(s)").arg(m_wallpapers_found);
    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void BackupDesktopWallpaperAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("Wallpaper backup cancelled");
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();

    Q_EMIT executionProgress("Backing up desktop wallpapers...", 20);

    QDir backup_dir(m_backup_location);
    if (!backup_dir.exists()) {
        backup_dir.mkpath(".");
    }

    QString wallpaper_folder = backup_dir.filePath("Wallpapers");
    QDir wallpaper_dir(wallpaper_folder);
    wallpaper_dir.mkpath(".");

    int backed_up = 0;
    qint64 total_bytes = 0;

    for (const UserProfile& user : m_user_profiles) {
        if (isCancelled()) {
            emitCancelledResult("Wallpaper backup cancelled", start_time);
            return;
        }

        QString wallpaper_path = findTranscodedWallpaper(user.profile_path);
        if (wallpaper_path.isEmpty()) {
            continue;
        }

        QFileInfo info(wallpaper_path);
        QString dest_file = wallpaper_folder + "/" + user.username + "_wallpaper.jpg";

        if (QFile::copy(wallpaper_path, dest_file)) {
            backed_up++;
            total_bytes += info.size();
        }
    }

    Q_EMIT executionProgress("Backing up registry settings...", 80);

    backupRegistrySettings(wallpaper_folder);

    Q_EMIT executionProgress("Backup complete", 100);

    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    result.duration_ms = duration_ms;

    if (backed_up > 0) {
        result.success = true;
        result.files_processed = backed_up;
        result.bytes_processed = total_bytes;
        result.message = QString("Backed up %1 wallpaper(s)").arg(backed_up);
        result.log = QString("Saved to: %1").arg(wallpaper_folder);
        finishWithResult(result, ActionStatus::Success);
    } else {
        result.success = false;
        result.message = "No wallpapers found to backup";
        result.log = "TranscodedWallpaper files not found in user profiles";
        finishWithResult(result, ActionStatus::Failed);
    }
}

} // namespace sak

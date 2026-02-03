// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/saved_game_data_backup_action.h"
#include "sak/windows_user_scanner.h"
#include <QDir>
#include <QDirIterator>

namespace sak {

SavedGameDataBackupAction::SavedGameDataBackupAction(const QString& backup_location, QObject* parent)
    : QuickAction(parent)
    , m_backup_location(backup_location)
{
}

void SavedGameDataBackupAction::scanSteamSaves() {
    // Scan all user profiles for Steam save data
    for (const UserProfile& user : m_user_profiles) {
        QStringList steam_paths = {
            user.profile_path + "/AppData/Roaming/Steam",
            "C:/Program Files (x86)/Steam/userdata"
        };
        
        for (const QString& path : steam_paths) {
            QDir dir(path);
            if (!dir.exists()) continue;
            
            QDirIterator it(path, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                if (it.fileName() == "remote") {
                    GameSaveLocation loc;
                    loc.platform = "Steam";
                    loc.path = it.filePath();
                    
                    qint64 size = 0;
                    int count = 0;
                    QDirIterator files(loc.path, QDir::Files, QDirIterator::Subdirectories);
                    while (files.hasNext()) {
                        files.next();
                        size += files.fileInfo().size();
                        count++;
                    }
                    
                    loc.size = size;
                    loc.file_count = count;
                    m_save_locations.append(loc);
                    m_total_size += size;
                }
            }
        }
    }
}

void SavedGameDataBackupAction::scanEpicSaves() {
    for (const UserProfile& user : m_user_profiles) {
        QString epic_path = user.profile_path + "/AppData/Local/EpicGamesLauncher/Saved";
        QDir dir(epic_path);
        
        if (dir.exists()) {
            GameSaveLocation loc;
            loc.platform = "Epic Games";
            loc.path = epic_path;
            
            qint64 size = 0;
            int count = 0;
            QDirIterator it(epic_path, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                size += it.fileInfo().size();
                count++;
            }
            
            loc.size = size;
            loc.file_count = count;
            m_save_locations.append(loc);
            m_total_size += size;
        }
    }
}

void SavedGameDataBackupAction::scanGOGSaves() {
    for (const UserProfile& user : m_user_profiles) {
        QString gog_path = user.profile_path + "/AppData/Local/GOG.com";
        QDir dir(gog_path);
        
        if (dir.exists()) {
            GameSaveLocation loc;
            loc.platform = "GOG";
            loc.path = gog_path;
            
            qint64 size = 0;
            int count = 0;
            QDirIterator it(gog_path, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                size += it.fileInfo().size();
                count++;
            }
            
            loc.size = size;
            loc.file_count = count;
            m_save_locations.append(loc);
            m_total_size += size;
        }
    }
}

void SavedGameDataBackupAction::scanDocumentsSaves() {
    // Many games save to Documents/My Games
    for (const UserProfile& user : m_user_profiles) {
        QString my_games = user.profile_path + "/Documents/My Games";
        QDir dir(my_games);
        
        if (dir.exists()) {
            GameSaveLocation loc;
            loc.platform = "Documents";
            loc.path = my_games;
            
            qint64 size = 0;
            int count = 0;
            QDirIterator it(my_games, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                size += it.fileInfo().size();
                count++;
            }
            
            loc.size = size;
            loc.file_count = count;
            m_save_locations.append(loc);
            m_total_size += size;
        }
    }
}

void SavedGameDataBackupAction::scan() {
    setStatus(ActionStatus::Scanning);
    
    // Scan all user profiles
    WindowsUserScanner scanner;
    m_user_profiles = scanner.scanUsers();
    
    m_save_locations.clear();
    m_total_size = 0;
    
    scanSteamSaves();
    scanEpicSaves();
    scanGOGSaves();
    scanDocumentsSaves();
    
    ScanResult result;
    result.applicable = (m_save_locations.count() > 0);
    result.bytes_affected = m_total_size;
    result.files_count = m_save_locations.count();
    result.estimated_duration_ms = (m_total_size / (10 * 1024 * 1024)) * 1000;
    
    if (m_save_locations.count() > 0) {
        result.summary = QString("Found %1 game save location(s) - %2 MB")
            .arg(m_save_locations.count())
            .arg(m_total_size / (1024 * 1024));
    } else {
        result.summary = "No game save data found";
    }
    
    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void SavedGameDataBackupAction::execute() {
    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    QDir backup_dir(m_backup_location + "/GameSaves");
    backup_dir.mkpath(".");
    
    int processed = 0;
    qint64 bytes_copied = 0;
    
    for (const GameSaveLocation& loc : m_save_locations) {
        if (isCancelled()) {
            ExecutionResult result;
            result.success = false;
            result.message = "Game save backup cancelled";
            result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
            setExecutionResult(result);
            setStatus(ActionStatus::Cancelled);
            Q_EMIT executionComplete(result);
            return;
        }

        QString safe_dir = loc.path;
        safe_dir.replace(':', '_');
        safe_dir.replace('\\', '_');
        safe_dir.replace('/', '_');
        QString dest = backup_dir.filePath(loc.platform + "/" + safe_dir);
        QDir().mkpath(dest);
        
        Q_EMIT executionProgress(QString("Backing up %1...").arg(loc.platform), 
                             (processed * 100) / m_save_locations.count());
        
        // Copy directory recursively
        QDirIterator it(loc.path, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            QString rel_path = QDir(loc.path).relativeFilePath(it.filePath());
            QString dest_file = dest + "/" + rel_path;
            
            QFileInfo dest_info(dest_file);
            QDir().mkpath(dest_info.absolutePath());
            
            if (QFile::copy(it.filePath(), dest_file)) {
                bytes_copied += it.fileInfo().size();
            }
        }
        
        processed++;
    }
    
    ExecutionResult result;
    result.success = processed > 0;
    result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    result.files_processed = processed;
    result.bytes_processed = bytes_copied;
    result.message = processed > 0
        ? QString("Backed up game saves from %1 location(s)").arg(processed)
        : "No game save locations were backed up";
    result.output_path = backup_dir.absolutePath();
    
    setExecutionResult(result);
    setStatus(processed > 0 ? ActionStatus::Success : ActionStatus::Failed);
    Q_EMIT executionComplete(result);
}

} // namespace sak

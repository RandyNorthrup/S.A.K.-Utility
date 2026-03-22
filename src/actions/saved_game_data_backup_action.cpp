// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file saved_game_data_backup_action.cpp
/// @brief Implements saved game data backup across user profiles

#include "sak/actions/saved_game_data_backup_action.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/windows_user_scanner.h"

#include <QDir>
#include <QDirIterator>

namespace sak {

SavedGameDataBackupAction::SavedGameDataBackupAction(const QString& backup_location,
                                                     QObject* parent)
    : QuickAction(parent), m_backup_location(backup_location) {}

void SavedGameDataBackupAction::scanSteamSaves() {
    for (const UserProfile& user : m_user_profiles) {
        scanSteamSavesForUser(user);
    }
}

void SavedGameDataBackupAction::scanSteamSavesForUser(const UserProfile& user) {
    const QString programFilesX86 = qEnvironmentVariable("ProgramFiles(x86)",
                                                         QStringLiteral("C:\\Program Files (x86)"));
    QStringList steam_paths = {user.profile_path + QStringLiteral("/AppData/Roaming/Steam"),
                               programFilesX86 + QStringLiteral("/Steam/userdata")};

    for (const QString& path : steam_paths) {
        QDir dir(path);
        if (!dir.exists()) {
            continue;
        }
        collectSteamRemoteDirs(path);
    }
}

void SavedGameDataBackupAction::collectSteamRemoteDirs(const QString& path) {
    QDirIterator it(path, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (it.fileName() != "remote") {
            continue;
        }
        addSaveLocation("Steam", it.filePath());
    }
}

void SavedGameDataBackupAction::addSaveLocation(const QString& platform, const QString& dir_path) {
    Q_ASSERT(!platform.isEmpty());
    Q_ASSERT(!dir_path.isEmpty());
    GameSaveLocation loc;
    loc.platform = platform;
    loc.path = dir_path;
    loc.size = 0;
    loc.file_count = 0;
    QDirIterator files(dir_path, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        files.next();
        loc.size += files.fileInfo().size();
        loc.file_count++;
    }
    m_save_locations.append(loc);
    m_total_size += loc.size;
}

void SavedGameDataBackupAction::scanEpicSaves() {
    for (const UserProfile& user : m_user_profiles) {
        QString epic_path = user.profile_path + "/AppData/Local/EpicGamesLauncher/Saved";
        if (!QDir(epic_path).exists()) {
            continue;
        }
        addSaveLocation("Epic Games", epic_path);
    }
}

void SavedGameDataBackupAction::scanGOGSaves() {
    for (const UserProfile& user : m_user_profiles) {
        QString gog_path = user.profile_path + "/AppData/Local/GOG.com";
        if (!QDir(gog_path).exists()) {
            continue;
        }
        addSaveLocation("GOG", gog_path);
    }
}

void SavedGameDataBackupAction::scanDocumentsSaves() {
    for (const UserProfile& user : m_user_profiles) {
        QString my_games = user.profile_path + "/Documents/My Games";
        if (!QDir(my_games).exists()) {
            continue;
        }
        addSaveLocation("Documents", my_games);
    }
}

void SavedGameDataBackupAction::scan() {
    setStatus(ActionStatus::Scanning);
    Q_ASSERT(status() == ActionStatus::Scanning);

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
    result.estimated_duration_ms = (m_total_size / (10 * sak::kBytesPerMB)) * 1000;

    if (m_save_locations.count() > 0) {
        result.summary = QString("Found %1 game save location(s) - %2 MB")
                             .arg(m_save_locations.count())
                             .arg(m_total_size / sak::kBytesPerMB);
    } else {
        result.summary = "No game save data found";
    }

    Q_ASSERT(!result.summary.isEmpty());

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void SavedGameDataBackupAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("Game save backup cancelled");
        return;
    }
    setStatus(ActionStatus::Running);
    Q_ASSERT(status() == ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    QDir backup_dir(m_backup_location + "/GameSaves");
    if (!backup_dir.mkpath(".")) {
        sak::logWarning("Failed to create game saves backup directory: {}",
                        backup_dir.absolutePath().toStdString());
    }

    int processed = 0;
    qint64 bytes_copied = 0;

    for (const GameSaveLocation& loc : m_save_locations) {
        if (isCancelled()) {
            emitCancelledResult("Game save backup cancelled", start_time);
            return;
        }

        QString safe_dir = sanitizePathForBackup(loc.path);
        QString dest = backup_dir.filePath(loc.platform + "/" + safe_dir);
        if (!QDir().mkpath(dest)) {
            sak::logWarning("Failed to create game save subdirectory: {}", dest.toStdString());
        }
        Q_EMIT executionProgress(QString("Backing up %1...").arg(loc.platform),
                                 (processed * 100) / m_save_locations.count());

        bytes_copied += copyDirectoryRecursive(loc.path, dest);

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

    finishWithResult(result, processed > 0 ? ActionStatus::Success : ActionStatus::Failed);
}

qint64 SavedGameDataBackupAction::copyDirectoryRecursive(const QString& src_path,
                                                         const QString& dest_path) {
    qint64 bytes_copied = 0;
    QDirIterator it(src_path, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        QString rel_path = QDir(src_path).relativeFilePath(it.filePath());
        QString dest_file = dest_path + "/" + rel_path;
        if (!QDir().mkpath(QFileInfo(dest_file).absolutePath())) {
            sak::logWarning("Failed to create directory for game save file");
        }
        if (QFile::copy(it.filePath(), dest_file)) {
            bytes_copied += it.fileInfo().size();
        }
    }
    return bytes_copied;
}

}  // namespace sak

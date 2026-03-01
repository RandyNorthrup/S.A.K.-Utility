// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"
#include "sak/user_profile_types.h"
#include <QString>
#include <QVector>

namespace sak {

/**
 * @brief Saved Game Data Backup Action
 * 
 * Backs up game saves from Steam, Epic Games, GOG, and common save locations.
 */
class SavedGameDataBackupAction : public QuickAction {
    Q_OBJECT

public:
    explicit SavedGameDataBackupAction(const QString& backup_location, QObject* parent = nullptr);

    QString name() const override { return "Saved Game Data Backup"; }
    QString description() const override { return "Backup Steam, Epic, GOG game saves"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::QuickBackup; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

private:
    /// @brief Represents a discovered game save data location for a specific platform
    struct GameSaveLocation {
        QString platform; // Steam, Epic, GOG, Documents
        QString path;
        qint64 size;
        int file_count;
    };

    QString m_backup_location;
    QVector<UserProfile> m_user_profiles;
    QVector<GameSaveLocation> m_save_locations;
    qint64 m_total_size{0};

    void scanSteamSaves();
    void scanEpicSaves();
    void scanGOGSaves();
    void scanDocumentsSaves();

    /// @brief Scan Steam save paths for a single user profile
    void scanSteamSavesForUser(const UserProfile& user);
    /// @brief Collect Steam remote save directories under the given path
    void collectSteamRemoteDirs(const QString& path);
    /// @brief Measure and append a save location to the list
    void addSaveLocation(const QString& platform, const QString& path);
    /// @brief Recursively copy all files from src_path to dest_path
    qint64 copyDirectoryRecursive(const QString& src_path, const QString& dest_path);
};

} // namespace sak


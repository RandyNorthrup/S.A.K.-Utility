// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
#include "sak/user_profile_types.h"
#include <QString>
#include <QVector>

namespace sak {

/**
 * @brief Desktop Wallpaper Backup Action
 * 
 * Backs up Windows desktop wallpaper (TranscodedWallpaper file + registry keys).
 */
class BackupDesktopWallpaperAction : public QuickAction {
    Q_OBJECT

public:
    explicit BackupDesktopWallpaperAction(const QString& backup_location, QObject* parent = nullptr);

    QString name() const override { return "Desktop Wallpaper Backup"; }
    QString description() const override { return "Backup desktop wallpaper settings"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::QuickBackup; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

private:
    QString m_backup_location;
    QVector<UserProfile> m_user_profiles;
    int m_wallpapers_found{0};

    QString findTranscodedWallpaper(const QString& profile_path);
    bool backupRegistrySettings(const QString& dest_folder);
};

} // namespace sak

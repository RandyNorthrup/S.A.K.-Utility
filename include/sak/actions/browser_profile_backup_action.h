// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"
#include <QDateTime>
#include <QDir>
#include <QString>
#include <QVector>

namespace sak {

struct UserProfile;

/**
 * @brief Browser Profile Backup Action
 * 
 * Backs up complete browser profiles using existing UserDataManager.
 * Includes bookmarks, passwords, extensions, and settings for Chrome, Firefox, Edge.
 */
class BrowserProfileBackupAction : public QuickAction {
    Q_OBJECT

public:
    explicit BrowserProfileBackupAction(const QString& backup_location, QObject* parent = nullptr);

    QString name() const override { return "Browser Profile Backup"; }
    QString description() const override { return "Backup browser profiles with bookmarks and passwords"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::QuickBackup; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

private:
    QString m_backup_location;

    /// @brief Backup browser profiles for all users
    /// @return false if cancelled, true if completed
    bool backupAllBrowserProfiles(const QVector<UserProfile>& users, const QDir& backup_dir,
                                  const QDateTime& start_time,
                                  int& profile_count, int& files_copied, qint64& bytes_copied);

    /// @brief Backup all browser profiles for a single user
    /// @return false if cancelled
    bool backupUserBrowserProfiles(const UserProfile& user, const QDir& backup_dir,
                                   const QDateTime& start_time,
                                   int user_idx, int total_users,
                                   int& profile_count, int& files_copied, qint64& bytes_copied);

    /// @brief Copy all files from a single browser profile directory
    /// @return false if cancelled
    bool copyProfileFiles(const QString& src_root, const QString& dest_root,
                          const QDateTime& start_time,
                          int& files_copied, qint64& bytes_copied);
};

} // namespace sak


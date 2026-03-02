// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"
#include "sak/user_profile_types.h"
#include <QDir>
#include <QString>
#include <QVector>

namespace sak {

/**
 * @brief Outlook Email Backup Action
 * 
 * Backs up Outlook PST/OST files and account configuration.
 * Detects running Outlook process and warns user.
 */
class OutlookBackupAction : public QuickAction {
    Q_OBJECT

public:
    explicit OutlookBackupAction(const QString& backup_location, QObject* parent = nullptr);

    QString name() const override { return "Outlook Email Backup"; }
    QString description() const override { return "Backup Outlook PST/OST files"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::QuickBackup; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

private:
    /// @brief Represents a discovered Outlook PST or OST data file
    struct OutlookFile {
        QString path;
        QString filename;
        QString type; // PST or OST
        qint64 size;
        bool is_open;
    };

    QString m_backup_location;
    QVector<UserProfile> m_user_profiles;
    QVector<OutlookFile> m_found_files;
    qint64 m_total_size{0};

    void scanOutlookFiles();
    bool isOutlookRunning();
    bool isFileOpen(const QString& file_path);
    bool copyFileWithProgress(const QString& source, const QString& dest);

    /// @brief Scan all user profiles for Outlook PST/OST data files
    void discoverOutlookFiles(QVector<OutlookFile>& found_files, qint64& total_size);
    /// @brief Copy discovered Outlook files to the backup directory
    bool copyOutlookFilesToBackup(const QVector<OutlookFile>& found_files, const QDir& backup_dir,
                                   int& files_copied, qint64& bytes_copied);
    void finalizeOutlookResult(int files_copied, qint64 bytes_copied,
                               const QDir& backup_dir, const QDateTime& start_time);
};

} // namespace sak


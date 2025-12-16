// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
#include "sak/user_profile_types.h"
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
};

} // namespace sak


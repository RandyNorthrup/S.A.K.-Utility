// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
#include "sak/user_profile_types.h"
#include <QString>

namespace sak {

/**
 * @brief Backup Email Data Action
 * 
 * Emergency backup of Outlook, Thunderbird, and Windows Mail data.
 */
class BackupEmailDataAction : public QuickAction {
    Q_OBJECT

public:
    explicit BackupEmailDataAction(const QString& backup_location, QObject* parent = nullptr);

    QString name() const override { return "Backup Email Data"; }
    QString description() const override { return "Emergency backup of email data"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::EmergencyRecovery; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

private:
    QString m_backup_location;
    qint64 m_total_size{0};
    QVector<UserProfile> m_user_profiles;
    int m_total_files{0};

    void backupOutlook();
    void backupThunderbird();
    void backupWindowsMail();
};

} // namespace sak


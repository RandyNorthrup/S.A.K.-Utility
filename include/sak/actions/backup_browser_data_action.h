// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
#include "sak/user_data_manager.h"
#include "sak/user_profile_types.h"
#include <QString>
#include <memory>

namespace sak {

/**
 * @brief Backup Browser Data Action
 * 
 * Emergency backup of all browser data using UserDataManager.
 */
class BackupBrowserDataAction : public QuickAction {
    Q_OBJECT

public:
    explicit BackupBrowserDataAction(const QString& backup_location, QObject* parent = nullptr);

    QString name() const override { return "Backup Browser Data"; }
    QString description() const override { return "Emergency backup of all browser data"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::EmergencyRecovery; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

private:
    QString m_backup_location;
    std::unique_ptr<UserDataManager> m_data_manager;
    QVector<UserProfile> m_user_profiles;
    qint64 m_total_size{0};
    int m_total_items{0};
};

} // namespace sak


// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
#include "sak/user_profile_types.h"
#include <QString>
#include <QVector>

namespace sak {

/**
 * @brief Development Configs Backup Action
 * 
 * Backs up Git configs, SSH keys, IDE settings (VS Code, Visual Studio, IntelliJ).
 */
class DevelopmentConfigsBackupAction : public QuickAction {
    Q_OBJECT

public:
    explicit DevelopmentConfigsBackupAction(const QString& backup_location, QObject* parent = nullptr);

    QString name() const override { return "Development Configs Backup"; }
    QString description() const override { return "Backup Git, SSH keys, IDE settings"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::QuickBackup; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

private:
    struct DevConfig {
        QString name;
        QString path;
        qint64 size;
        bool is_sensitive; // True for SSH keys
    };

    QString m_backup_location;
    QVector<UserProfile> m_user_profiles;
    QVector<DevConfig> m_configs;
    qint64 m_total_size{0};
    bool m_found_sensitive_data{false};

    void scanGitConfig();
    void scanSSHKeys();
    void scanVSCodeSettings();
    void scanVisualStudioSettings();
    void scanIntelliJSettings();
};

} // namespace sak


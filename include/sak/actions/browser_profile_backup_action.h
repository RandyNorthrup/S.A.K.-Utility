// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"
#include <QString>

namespace sak {

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
};

} // namespace sak


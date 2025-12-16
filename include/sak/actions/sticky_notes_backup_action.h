// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
#include "sak/user_profile_types.h"
#include <QString>
#include <QVector>

namespace sak {

/**
 * @brief Sticky Notes Backup Action
 * 
 * Backs up Windows Sticky Notes database (plum.sqlite).
 */
class StickyNotesBackupAction : public QuickAction {
    Q_OBJECT

public:
    explicit StickyNotesBackupAction(const QString& backup_location, QObject* parent = nullptr);

    QString name() const override { return "Sticky Notes Backup"; }
    QString description() const override { return "Backup Windows Sticky Notes"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::QuickBackup; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

private:
    QString m_backup_location;
    QVector<UserProfile> m_user_profiles;
    QString m_sticky_notes_path;
    qint64 m_file_size{0};
    bool m_found{false};

    QString findStickyNotesDatabase();
};

} // namespace sak


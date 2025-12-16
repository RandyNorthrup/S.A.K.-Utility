// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
#include <QString>
#include <memory>

namespace sak {

/**
 * @brief Backup Activation Keys Action
 * 
 * Backs up Windows and Office product keys using existing LicenseScanner.
 */
class BackupActivationKeysAction : public QuickAction {
    Q_OBJECT

public:
    explicit BackupActivationKeysAction(const QString& backup_location, QObject* parent = nullptr);

    QString name() const override { return "Backup Activation Keys"; }
    QString description() const override { return "Backup Windows/Office product keys"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::EmergencyRecovery; }
    bool requiresAdmin() const override { return true; }

    void scan() override;
    void execute() override;

private:
    QString m_backup_location;
    int m_keys_found{0};
};

} // namespace sak


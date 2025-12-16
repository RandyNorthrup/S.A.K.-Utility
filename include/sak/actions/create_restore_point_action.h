// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
#include <QString>

namespace sak {

/**
 * @brief Create Restore Point Action
 * 
 * Creates Windows System Restore point using WMI.
 */
class CreateRestorePointAction : public QuickAction {
    Q_OBJECT

public:
    explicit CreateRestorePointAction(QObject* parent = nullptr);

    QString name() const override { return "Create Restore Point"; }
    QString description() const override { return "Create Windows System Restore point"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::EmergencyRecovery; }
    bool requiresAdmin() const override { return true; }

    void scan() override;
    void execute() override;

private:
    bool m_restore_enabled{false};
    QString m_last_restore_point;

    void checkRestoreStatus();
    void createRestorePoint();
};

} // namespace sak


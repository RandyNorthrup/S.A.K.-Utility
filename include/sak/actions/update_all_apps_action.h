// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
#include "sak/chocolatey_manager.h"
#include <QString>
#include <QVector>
#include <memory>

namespace sak {

/**
 * @brief Update All Apps Action
 * 
 * Updates all Chocolatey packages using existing ChocolateyManager.
 */
class UpdateAllAppsAction : public QuickAction {
    Q_OBJECT

public:
    explicit UpdateAllAppsAction(QObject* parent = nullptr);

    QString name() const override { return "Update All Apps"; }
    QString description() const override { return "Update all Chocolatey packages"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::Maintenance; }
    bool requiresAdmin() const override { return true; }

    void scan() override;
    void execute() override;

private:
    std::unique_ptr<ChocolateyManager> m_choco_manager;
    int m_outdated_count{0};
    QVector<QString> m_outdated_packages;
    bool m_choco_installed{false};
};

} // namespace sak


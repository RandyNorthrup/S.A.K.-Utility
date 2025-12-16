// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
#include <QString>

namespace sak {

/**
 * @brief Optimize Power Settings Action
 * 
 * Switches Windows power plan to High Performance mode for maximum performance.
 */
class OptimizePowerSettingsAction : public QuickAction {
    Q_OBJECT

public:
    explicit OptimizePowerSettingsAction(QObject* parent = nullptr);

    QString name() const override { return "Optimize Power Settings"; }
    QString description() const override { return "Switch to High Performance power plan"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::SystemOptimization; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

private:
    struct PowerPlan {
        QString guid;
        QString name;
        bool is_active;
    };
    
    QVector<PowerPlan> enumeratePowerPlans();
    PowerPlan queryPowerPlan(const QString& guid);
    PowerPlan getActivePowerPlan();
    bool setPowerPlan(const QString& guid);
    PowerPlan findPowerPlanByName(const QString& name);
    QString getStandardPowerPlanGuid(const QString& plan_name);
};

} // namespace sak


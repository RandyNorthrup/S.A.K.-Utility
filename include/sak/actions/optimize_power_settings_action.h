// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"

#include <QDateTime>
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
    /// @brief Represents a Windows power plan with its GUID and active state
    struct PowerPlan {
        QString guid;
        QString name;
        bool isActive;
    };

    QVector<PowerPlan> enumeratePowerPlans();
    PowerPlan queryPowerPlan(const QString& guid);
    PowerPlan getActivePowerPlan();
    bool setPowerPlan(const QString& guid);
    PowerPlan findPowerPlanByName(const QString& name);
    QString getStandardPowerPlanGuid(const QString& plan_name);

    /// @brief Activate the High Performance plan, verify, and append status to report
    bool activateHighPerformancePlan(const PowerPlan& high_perf_plan,
                                     const QString& current_plan_name,
                                     QString& report);
    /// @brief Build the report header listing current and available power plans
    QString buildPowerPlanListReport(const PowerPlan& current_plan,
                                     const QVector<PowerPlan>& all_plans) const;
    /// @brief Create and emit the final execution result with recommendations
    void finalizePowerOptimizationResult(const QDateTime& start_time,
                                         const QString& report,
                                         const QString& previous_plan_name,
                                         const QString& high_perf_guid,
                                         bool already_optimized,
                                         bool success);
};

}  // namespace sak

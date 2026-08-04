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

    /// @brief True if the given active-scheme GUID is a high-performance plan
    /// (Windows High Performance or Ultimate Performance). Matching by GUID -- not
    /// by a name substring -- prevents a custom plan merely NAMED "...High
    /// Performance..." from being mistaken for the built-in one (which would skip
    /// activation of the real plan).
    static bool isHighPerformanceGuid(const QString& guid);

    /// @brief Whether powercfg discovery succeeded AND returned at least one plan. Only then may
    ///        the optimizer activate a plan; a failed discovery must fail closed (no mutation on a
    ///        guessed GUID). Pure decision seam.
    static bool discoveryPermitsActivation(bool discovery_ok, int plans_found);

private:
    /// @brief Represents a Windows power plan with its GUID and active state
    struct PowerPlan {
        QString guid;
        QString name;
        bool isActive{false};
    };

    struct OptimizationResultContext {
        QDateTime start_time;
        QString report;
        QString previous_plan_name;
        QString high_perf_guid;
        bool already_optimized{false};
        bool success{false};
    };

    QVector<PowerPlan> enumeratePowerPlans(bool& discovery_ok);
    PowerPlan queryPowerPlan(const QString& guid);
    PowerPlan getActivePowerPlan();
    bool setPowerPlan(const QString& guid);
    /// @brief Exact (case-insensitive) name match within an ALREADY-enumerated plan list, so the
    ///        caller enumerates once instead of re-running powercfg per lookup.
    static PowerPlan findPlanByNameIn(const QVector<PowerPlan>& plans, const QString& name);
    /// @brief Pick the High/Ultimate Performance plan from @p plans, falling back to the canonical
    ///        built-in GUID ONLY when discovery succeeded. Returns false (fail closed, no
    ///        mutation) when discovery failed or no usable plan/GUID could be resolved.
    bool resolveHighPerformancePlan(const QVector<PowerPlan>& plans,
                                    bool discovery_ok,
                                    PowerPlan& out_plan);
    QString getStandardPowerPlanGuid(const QString& plan_type);

    /// @brief Activate the High Performance plan, verify, and append status to report
    bool activateHighPerformancePlan(const PowerPlan& high_perf_plan,
                                     const QString& current_plan_name,
                                     QString& report);
    /// @brief Build the report header listing current and available power plans
    QString buildPowerPlanListReport(const PowerPlan& current_plan,
                                     const QVector<PowerPlan>& all_plans) const;
    /// @brief Create and emit the final execution result with recommendations
    void finalizePowerOptimizationResult(const OptimizationResultContext& context);
    /// @brief Append the discovery-failure status to @p report and emit a fail-closed result
    ///        (no plan mutation). Extracted so execute() stays within the length cap.
    void finalizeDiscoveryFailure(const QDateTime& start_time,
                                  QString& report,
                                  const QString& previous_plan_name);
};

}  // namespace sak

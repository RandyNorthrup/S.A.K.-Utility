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

    /// @brief Represents a Windows power plan with its GUID and active state
    struct PowerPlan {
        QString guid;
        QString name;
        bool isActive{false};
    };

    /// @brief Parse `powercfg /list` output into plans. LOCALE-INDEPENDENT by construction.
    ///
    /// The previous parser required the literal English label "Power Scheme GUID:", so on a
    /// non-English Windows it matched nothing, the plan list came back EMPTY, and the whole
    /// optimisation refused (fail-closed, but the feature was simply dead). The parts of that
    /// output that do NOT localise are the scheme GUID itself and the trailing '*' that marks the
    /// active plan, so those are what this anchors on -- the same "route around the localized
    /// text" rule already applied to the DNS cache and ethernet-config scrapes (R5-G23-4).
    ///
    /// The plan NAME is still read from the parenthesised text, which IS localised -- that is
    /// correct: it is a display string, and every decision (which plan is High Performance,
    /// which is active) is made on the GUID and the marker instead.
    ///
    /// Public and pure so the locale dimension is testable without a non-English Windows.
    [[nodiscard]] static QVector<PowerPlan> parsePowerPlanList(const QString& output);

    /// @brief Parse `powercfg -GETACTIVESCHEME` output into the active plan. Also
    ///        locale-independent, and for a sharper reason than the list parser: this is how the
    ///        "already using High Performance" check identifies the current plan, so an
    ///        English-only match meant that check could never succeed on a translated Windows and
    ///        the action would re-activate the plan on every single run. Returns a default-
    ///        constructed plan (empty guid) when no scheme line is present.
    [[nodiscard]] static PowerPlan parseActivePowerPlan(const QString& output);

private:
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

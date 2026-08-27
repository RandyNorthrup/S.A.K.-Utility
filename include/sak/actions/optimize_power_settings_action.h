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

    /// @brief One power setting's effective idle timeout, in seconds, for each power source.
    ///
    /// @c found false means the value was NOT READ. It is never a synthesised default, because
    /// the entire point of reading these back is that the report stops asserting settings
    /// nothing measured -- substituting a plausible zero here would recreate the defect.
    struct PowerTimeout {
        qint64 ac_seconds{0};
        qint64 dc_seconds{0};
        bool found{false};
    };

    /// @brief The three idle timeouts a technician asks about after a plan change.
    struct PowerTimeouts {
        PowerTimeout display;
        PowerTimeout sleep;
        PowerTimeout hibernate;
    };

    /// @brief What `powercfg -QUERY <guid>` reported: the plan it names and its timeouts.
    struct PowerPlanDetails {
        PowerPlan plan;
        PowerTimeouts timeouts;
    };

    /// @brief Read one setting's AC/DC idle timeout out of `powercfg -QUERY` output.
    ///
    /// LOCALE-INDEPENDENT, by the same rule the parsers above follow, and it needs to be: every
    /// label in this output translates ("Current AC Power Setting Index" included), so matching
    /// on the English text would make the read-back silently return nothing on a translated
    /// Windows -- the exact failure this file already suffered twice.
    ///
    /// What does NOT translate is the structure. Within a setting block, the two "Current ...
    /// Index" lines sit at the SAME indent as the "Power Setting GUID" line that opens it, while
    /// the block's Minimum/Maximum/increment attributes are indented deeper. Anchoring on that
    /// indent -- rather than on "the last two hex values", which would happily report a
    /// Maximum Possible Setting as a live reading if the Current lines were ever absent -- is
    /// what makes a missing value fail closed instead of fabricating one.
    ///
    /// Verified against real `powercfg -QUERY` output before being written: all 54 Current lines
    /// across 27 setting blocks sit at that indent, no attribute line does, and the rule agrees
    /// with the English labels on every block. Requires EXACTLY two readings; anything else
    /// returns found == false.
    [[nodiscard]] static PowerTimeout parsePowerTimeout(const QString& query_output,
                                                        const QString& setting_guid);

    /// @brief Read the display, sleep and hibernate idle timeouts in one pass.
    [[nodiscard]] static PowerTimeouts parsePowerTimeouts(const QString& query_output);

    /// @brief Render one timeout for a technician. 0 seconds is powercfg's encoding of "Never",
    ///        which as a bare "0" reads like an immediate timeout -- the opposite of what it means.
    [[nodiscard]] static QString formatPowerTimeout(qint64 seconds);

private:
    struct OptimizationResultContext {
        QDateTime start_time;
        QString report;
        QString previous_plan_name;
        QString high_perf_guid;
        /// The timeouts actually in force once the action finished, read back from powercfg.
        PowerTimeouts effective_timeouts;
        bool already_optimized{false};
        bool success{false};
    };

    QVector<PowerPlan> enumeratePowerPlans(bool& discovery_ok);
    /// @brief Run `powercfg -QUERY <guid>` and report BOTH the plan it names and the idle
    ///        timeouts it lists.
    ///
    /// This used to run the query -- the expensive part, and the only call that returns the
    /// settings tree at all -- and keep nothing but the plan NAME, discarding every value that
    /// made the query worth making. It had no caller, while the success report told the
    /// technician to go and run powercfg -QUERY themselves. The read-back is now wired up and
    /// the report states what it measured.
    PowerPlanDetails queryPowerPlan(const QString& guid);
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
    /// @brief Append the "Target Plan / Target GUID" block that closes the report header.
    static void appendTargetPlanHeader(const PowerPlan& target_plan, QString& report);
    /// @brief Query the timeouts now in force, against the plan that is ACTUALLY active rather
    ///        than the one activation aimed at, falling back to @p fallback_guid only when the
    ///        active-scheme query cannot answer.
    PowerTimeouts readEffectiveTimeouts(const QString& fallback_guid);
    /// @brief Append the measured idle timeouts, naming any the query did not return rather
    ///        than printing a default in their place.
    static void appendEffectiveTimeouts(const PowerTimeouts& timeouts, QString& log);
    /// @brief Append the discovery-failure status to @p report and emit a fail-closed result
    ///        (no plan mutation). Extracted so execute() stays within the length cap.
    void finalizeDiscoveryFailure(const QDateTime& start_time,
                                  QString& report,
                                  const QString& previous_plan_name);
};

}  // namespace sak

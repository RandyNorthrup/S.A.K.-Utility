// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file optimize_power_settings_action.cpp
/// @brief Implements Windows power plan optimization for performance

#include "sak/actions/optimize_power_settings_action.h"

#include "sak/action_constants.h"
#include "sak/layout_constants.h"
#include "sak/process_runner.h"

#include <QRegularExpression>
#include <QTextStream>

#include <algorithm>

namespace sak {

namespace {

constexpr auto kPowerPlanListPattern =
    "Power Scheme GUID:\\s*([0-9a-f\\-]+)\\s*\\(([^\\)]+)\\)(\\s*\\*)?";
constexpr auto kPowerPlanNamePattern = "Power Scheme GUID:\\s*[0-9a-f\\-]+\\s*\\(([^\\)]+)\\)";
constexpr auto kActivePowerPlanPattern = "Power Scheme GUID:\\s*([0-9a-f\\-]+)\\s*\\(([^\\)]+)\\)";
constexpr int kPowerPlanGuidCaptureGroup = 1;
constexpr int kPowerPlanNameCaptureGroup = 2;
constexpr int kPowerPlanActiveMarkerCaptureGroup = 3;
constexpr int kReportInnerWidth = 67;

// Built-in high-performance scheme GUIDs (lower-case, canonical).
constexpr auto kHighPerformanceGuid = "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c";      // SCHEME_MIN
constexpr auto kUltimatePerformanceGuid = "e9a42b02-d5df-448d-aa00-03f14749eb61";  // Ultimate

}  // namespace

OptimizePowerSettingsAction::OptimizePowerSettingsAction(QObject* parent) : QuickAction(parent) {}

bool OptimizePowerSettingsAction::isHighPerformanceGuid(const QString& guid) {
    const QString normalized = guid.trimmed();
    return normalized.compare(QLatin1String(kHighPerformanceGuid), Qt::CaseInsensitive) == 0 ||
           normalized.compare(QLatin1String(kUltimatePerformanceGuid), Qt::CaseInsensitive) == 0;
}

// ENTERPRISE-GRADE: Enumerate all power plans using powercfg -LIST
QVector<OptimizePowerSettingsAction::PowerPlan> OptimizePowerSettingsAction::enumeratePowerPlans(
    bool& discovery_ok) {
    QVector<PowerPlan> plans;

    const ProcessResult proc = runProcess(sak::system32Path(QStringLiteral("powercfg.exe")),
                                          QStringList() << "-LIST",
                                          sak::kTimeoutProcessShortMs);
    // Whether powercfg itself ran. A failed run must not later be treated as "no plans found"
    // and coerced into a hard-coded-GUID mutation.
    discovery_ok = proc.succeeded();
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Power plan list warning: " + proc.std_err.trimmed());
    }
    const QString output = proc.std_out;

    // Parse output: "Power Scheme GUID: {guid} (Plan Name) *"
    const QRegularExpression plan_regex(QString::fromLatin1(kPowerPlanListPattern),
                                        QRegularExpression::CaseInsensitiveOption);

    QRegularExpressionMatchIterator it = plan_regex.globalMatch(output);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        PowerPlan plan;
        plan.guid = match.captured(kPowerPlanGuidCaptureGroup);
        plan.name = match.captured(kPowerPlanNameCaptureGroup).trimmed();
        plan.isActive = !match.captured(kPowerPlanActiveMarkerCaptureGroup).isEmpty();
        plans.append(plan);
    }

    return plans;
}

// ENTERPRISE-GRADE: Get detailed power plan information using powercfg -QUERY
OptimizePowerSettingsAction::PowerPlan OptimizePowerSettingsAction::queryPowerPlan(
    const QString& guid) {
    PowerPlan plan;
    plan.guid = guid;
    plan.isActive = false;

    const ProcessResult proc = runProcess(sak::system32Path(QStringLiteral("powercfg.exe")),
                                          QStringList() << "-QUERY" << guid,
                                          sak::kTimeoutProcessMediumMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Power plan query warning: " + proc.std_err.trimmed());
    }
    const QString output = proc.std_out;

    // Parse plan name from output
    const QRegularExpression regex(QString::fromLatin1(kPowerPlanNamePattern));
    const QRegularExpressionMatch match = regex.match(output);
    if (match.hasMatch()) {
        plan.name = match.captured(kPowerPlanGuidCaptureGroup);
    }

    return plan;
}

// ENTERPRISE-GRADE: Set power plan using powercfg -SETACTIVE
bool OptimizePowerSettingsAction::setPowerPlan(const QString& guid) {
    Q_EMIT executionProgress("Activating power plan...", progress::kStep60);

    const ProcessResult proc = runProcess(sak::system32Path(QStringLiteral("powercfg.exe")),
                                          QStringList() << "-SETACTIVE" << guid,
                                          sak::kTimeoutProcessShortMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Power plan activate warning: " + proc.std_err.trimmed());
    }
    return proc.succeeded();
}

// ENTERPRISE-GRADE: Get active power plan using powercfg -GETACTIVESCHEME
OptimizePowerSettingsAction::PowerPlan OptimizePowerSettingsAction::getActivePowerPlan() {
    PowerPlan active_plan;

    const ProcessResult proc = runProcess(sak::system32Path(QStringLiteral("powercfg.exe")),
                                          QStringList() << "-GETACTIVESCHEME",
                                          sak::kTimeoutProcessShortMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Power plan active query warning: " + proc.std_err.trimmed());
    }
    const QString output = proc.std_out;

    // Parse: "Power Scheme GUID: {guid} (Plan Name)"
    const QRegularExpression regex(QString::fromLatin1(kActivePowerPlanPattern));
    const QRegularExpressionMatch match = regex.match(output);

    if (match.hasMatch()) {
        active_plan.guid = match.captured(kPowerPlanGuidCaptureGroup);
        active_plan.name = match.captured(kPowerPlanNameCaptureGroup).trimmed();
        active_plan.isActive = true;
    }

    return active_plan;
}

// ENTERPRISE-GRADE: Find power plan by name (case-insensitive) within an enumerated list.
OptimizePowerSettingsAction::PowerPlan OptimizePowerSettingsAction::findPlanByNameIn(
    const QVector<PowerPlan>& plans, const QString& name) {
    // Exact (case-insensitive) name match, NOT a substring match: a custom plan
    // named e.g. "My High Performance Rig" must not be selected when searching
    // for the built-in "High Performance". If no exact match exists, the caller
    // falls back to the canonical built-in GUID via getStandardPowerPlanGuid().
    auto it = std::find_if(plans.begin(), plans.end(), [&name](const PowerPlan& plan) {
        return plan.name.compare(name, Qt::CaseInsensitive) == 0;
    });
    return it != plans.end() ? *it : PowerPlan();
}

bool OptimizePowerSettingsAction::discoveryPermitsActivation(bool discovery_ok, int plans_found) {
    return discovery_ok && plans_found > 0;
}

void OptimizePowerSettingsAction::finalizeDiscoveryFailure(const QDateTime& start_time,
                                                           QString& report,
                                                           const QString& previous_plan_name) {
    report += QString("| Status:       Power plan discovery FAILED\n")
                  .leftJustified(kReportInnerWidth, ' ') +
              "|\n";
    report += QString("| Error:        Could not enumerate power plans; not mutating\n")
                  .leftJustified(kReportInnerWidth, ' ') +
              "|\n";
    report += "+================================================================+\n";
    finalizePowerOptimizationResult({/*.start_time =*/start_time,
                                     /*.report =*/report,
                                     /*.previous_plan_name =*/previous_plan_name,
                                     /*.high_perf_guid =*/QString(),
                                     /*.already_optimized =*/false,
                                     /*.success =*/false});
}

bool OptimizePowerSettingsAction::resolveHighPerformancePlan(const QVector<PowerPlan>& plans,
                                                             bool discovery_ok,
                                                             PowerPlan& out_plan) {
    // Fail closed if powercfg discovery did not actually succeed and return plans: we must never
    // fall back to a hard-coded GUID and then mutate the system on a guess.
    if (!discoveryPermitsActivation(discovery_ok, static_cast<int>(plans.size()))) {
        return false;
    }
    // Prefer a plan the enumeration identifies by its canonical built-in GUID: a custom
    // plan merely NAMED "High Performance"/"Ultimate Performance" must never win over the
    // real built-in scheme. Selection stays GUID-anchored (same predicate the
    // already-optimized check uses), not a name that an arbitrary plan can spoof.
    auto by_guid = std::find_if(plans.begin(), plans.end(), [](const PowerPlan& plan) {
        return isHighPerformanceGuid(plan.guid);
    });
    if (by_guid != plans.end()) {
        out_plan = *by_guid;
        return true;
    }
    PowerPlan found = findPlanByNameIn(plans, "High Performance");
    if (found.guid.isEmpty()) {
        found = findPlanByNameIn(plans, "Ultimate Performance");
    }
    if (!found.guid.isEmpty()) {
        out_plan = found;
        return true;
    }
    // The canonical built-in GUID is used ONLY now that discovery is known good and simply lacked
    // a matching named plan (e.g. the hidden High Performance plan on some SKUs).
    out_plan.guid = getStandardPowerPlanGuid("High Performance");
    out_plan.name = "High Performance (Standard)";
    return !out_plan.guid.isEmpty();
}

// ENTERPRISE-GRADE: Standard power scheme GUIDs
// These are Microsoft's documented, canonical built-in scheme GUIDs -- not a
// guessed default. Returning one when enumeration finds no exact-named plan
// activates the REAL built-in plan, so this is correct-by-design, not an
// error-hiding fallback.
QString OptimizePowerSettingsAction::getStandardPowerPlanGuid(const QString& plan_type) {
    if (plan_type == "High Performance" || plan_type == "high") {
        return "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c";  // SCHEME_MIN (High Performance)
    } else if (plan_type == "Balanced") {
        return "381b4222-f694-41f0-9685-ff5bb260df2e";  // SCHEME_BALANCED
    } else if (plan_type == "Power Saver") {
        return "a1841308-3541-4fab-bc81-f71556f20b4a";  // SCHEME_MAX (Power Saver)
    }
    return QString();
}

void OptimizePowerSettingsAction::scan() {
    setStatus(ActionStatus::Scanning);

    const PowerPlan current_plan = getActivePowerPlan();

    ScanResult result;
    result.applicable = true;
    result.summary = current_plan.name.isEmpty()
                         ? "Power plan detected"
                         : QString("Active plan: %1").arg(current_plan.name);
    result.details = "Optimization will switch to High Performance if available";

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

QString OptimizePowerSettingsAction::buildPowerPlanListReport(
    const PowerPlan& current_plan, const QVector<PowerPlan>& all_plans) const {
    QString report = "+================================================================+\n";
    report += "|             POWER PLAN OPTIMIZATION REPORT                   |\n";
    report += "+================================================================+\n";
    report += QString("| Current Plan: %1\n")
                  .arg(current_plan.name)
                  .leftJustified(kReportInnerWidth, ' ') +
              "|\n";
    report += QString("| Current GUID: %1\n")
                  .arg(current_plan.guid)
                  .leftJustified(kReportInnerWidth, ' ') +
              "|\n";
    report += "+================================================================+\n";
    report += QString("| Available Power Plans: %1\n")
                  .arg(all_plans.size())
                  .leftJustified(kReportInnerWidth, ' ') +
              "|\n";

    // List all plans
    for (const PowerPlan& plan : all_plans) {
        const QString plan_line =
            QString("|   %1 %2\n").arg(plan.isActive ? "[ACTIVE]" : "        ").arg(plan.name);
        report += plan_line.leftJustified(kReportInnerWidth, ' ') + "|\n";
    }

    report += "+================================================================+\n";
    return report;
}

void OptimizePowerSettingsAction::finalizePowerOptimizationResult(
    const OptimizationResultContext& context) {
    Q_EMIT executionProgress("Power optimization complete", progress::kComplete);

    const qint64 duration_ms = context.start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    result.duration_ms = duration_ms;

    if (context.already_optimized) {
        result.success = true;
        result.message = "Already using High Performance power plan";
        result.log = context.report;
        result.log += QString("\nCompleted in %1 ms\n").arg(duration_ms);
        result.log += "RECOMMENDATIONS:\n";
        result.log += "* System already optimized for performance\n";
        result.log += "* Processor performance boost enabled\n";
        result.log += "* Minimal power management restrictions\n";
    } else if (context.success) {
        result.success = true;
        result.message =
            QString("Switched to High Performance (was: %1)").arg(context.previous_plan_name);
        result.log = context.report;
        result.log += QString("\nCompleted in %1 ms\n").arg(duration_ms);
        result.log += "RECOMMENDATIONS:\n";
        result.log += "* Performance boost enabled\n";
        result.log += "* Sleep/hibernate settings unchanged\n";
        result.log += "* Display timeout settings unchanged\n";
        result.log += "* Use powercfg -QUERY for detailed settings\n";
    } else {
        result.success = false;
        result.message = "Failed to activate High Performance plan";
        result.log = context.report;
        result.log += "\nFailed to change power plan - administrative privileges may be required\n";
        result.log += "Try running as Administrator or use: powercfg -SETACTIVE " +
                      context.high_perf_guid + "\n";
    }

    finishWithResult(result, result.success ? ActionStatus::Success : ActionStatus::Failed);
}

bool OptimizePowerSettingsAction::activateHighPerformancePlan(const PowerPlan& high_perf_plan,
                                                              const QString& current_plan_name,
                                                              QString& report) {
    bool success = setPowerPlan(high_perf_plan.guid);

    if (success) {
        Q_EMIT executionProgress("Verifying power plan activation...", progress::kStep80);
        const PowerPlan new_active = getActivePowerPlan();

        // Verify by GUID: either the exact plan we set, or any built-in
        // high-performance scheme -- never a mere name-substring match.
        success = (new_active.guid.compare(high_perf_plan.guid, Qt::CaseInsensitive) == 0) ||
                  isHighPerformanceGuid(new_active.guid);

        if (success) {
            report += QString("| Status:       Power plan activated\n")
                          .leftJustified(kReportInnerWidth, ' ') +
                      "|\n";
            report += QString("| Previous:     %1\n")
                          .arg(current_plan_name)
                          .leftJustified(kReportInnerWidth, ' ') +
                      "|\n";
            report += QString("| Current:      %1\n")
                          .arg(new_active.name)
                          .leftJustified(kReportInnerWidth, ' ') +
                      "|\n";
        } else {
            report += QString("| Status:       Activation verification FAILED\n")
                          .leftJustified(kReportInnerWidth, ' ') +
                      "|\n";
        }
    } else {
        report += "| Status:       Activation FAILED                             |\n";
        report += "| Error:        powercfg command failed                       |\n";
    }
    return success;
}

void OptimizePowerSettingsAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("Power settings optimization cancelled");
        return;
    }

    setStatus(ActionStatus::Running);
    const QDateTime start_time = QDateTime::currentDateTime();
    Q_EMIT executionProgress("Enumerating power plans...", progress::kStep10);
    const PowerPlan current_plan = getActivePowerPlan();

    Q_EMIT executionProgress("Scanning available power plans...", progress::kStep25);
    bool discovery_ok = false;
    const QVector<PowerPlan> all_plans = enumeratePowerPlans(discovery_ok);

    QString report = buildPowerPlanListReport(current_plan, all_plans);

    Q_EMIT executionProgress("Locating High Performance plan...", progress::kStep40);
    PowerPlan high_perf_plan;
    if (!resolveHighPerformancePlan(all_plans, discovery_ok, high_perf_plan)) {
        // Discovery failed: fail closed. Do NOT mutate the active power plan on a guessed GUID.
        finalizeDiscoveryFailure(start_time, report, current_plan.name);
        return;
    }

    report += QString("| Target Plan:  %1\n")
                  .arg(high_perf_plan.name)
                  .leftJustified(kReportInnerWidth, ' ') +
              "|\n";
    report += QString("| Target GUID:  %1\n")
                  .arg(high_perf_plan.guid)
                  .leftJustified(kReportInnerWidth, ' ') +
              "|\n";
    report += "+================================================================+\n";

    // Match by GUID, not by name substring: a custom plan named e.g. "My High
    // Performance Rig" must NOT be treated as the built-in plan (which would skip
    // switching to the real one).
    const bool already_optimized = isHighPerformanceGuid(current_plan.guid);
    bool success = true;

    if (already_optimized) {
        report += QString("| Status:       Already using High Performance\n")
                      .leftJustified(kReportInnerWidth, ' ') +
                  "|\n";
        report +=
            QString("| Action:       No change needed\n").leftJustified(kReportInnerWidth, ' ') +
            "|\n";
    } else {
        // Discovery ran several powercfg processes; honor a cancellation that arrived
        // during it before mutating the active plan with -SETACTIVE (fail closed).
        if (isCancelled()) {
            emitCancelledResult("Power settings optimization cancelled");
            return;
        }
        success = activateHighPerformancePlan(high_perf_plan, current_plan.name, report);
    }

    report += "+================================================================+\n";
    finalizePowerOptimizationResult({/*.start_time =*/start_time,
                                     /*.report =*/report,
                                     /*.previous_plan_name =*/current_plan.name,
                                     /*.high_perf_guid =*/high_perf_plan.guid,
                                     /*.already_optimized =*/already_optimized,
                                     /*.success =*/success});
}

}  // namespace sak

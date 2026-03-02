// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file optimize_power_settings_action.cpp
/// @brief Implements Windows power plan optimization for performance

#include "sak/actions/optimize_power_settings_action.h"
#include "sak/process_runner.h"
#include "sak/layout_constants.h"
#include <QRegularExpression>
#include <QTextStream>

namespace sak {

OptimizePowerSettingsAction::OptimizePowerSettingsAction(QObject* parent)
    : QuickAction(parent)
{
}

// ENTERPRISE-GRADE: Enumerate all power plans using powercfg -LIST
QVector<OptimizePowerSettingsAction::PowerPlan> OptimizePowerSettingsAction::enumeratePowerPlans() {
    QVector<PowerPlan> plans;

    ProcessResult proc = runProcess("powercfg", QStringList() << "-LIST",
        sak::kTimeoutProcessShortMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Power plan list warning: " + proc.std_err.trimmed());
    }
    QString output = proc.std_out;

    // Parse output: "Power Scheme GUID: {guid} (Plan Name) *"
    QRegularExpression plan_regex(R"(Power Scheme GUID:\s*([0-9a-f\-]+)\s*\(([^\)]+)\)(\s*\*)?)",
                                  QRegularExpression::CaseInsensitiveOption);

    QRegularExpressionMatchIterator it = plan_regex.globalMatch(output);
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        PowerPlan plan;
        plan.guid = match.captured(1);
        plan.name = match.captured(2).trimmed();
        plan.isActive = !match.captured(3).isEmpty(); // * indicates active
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

    ProcessResult proc = runProcess("powercfg", QStringList() << "-QUERY" << guid,
        sak::kTimeoutProcessMediumMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Power plan query warning: " + proc.std_err.trimmed());
    }
    QString output = proc.std_out;

    // Parse plan name from output
    QRegularExpression regex(R"(Power Scheme GUID:\s*[0-9a-f\-]+\s*\(([^\)]+)\))");
    QRegularExpressionMatch match = regex.match(output);
    if (match.hasMatch()) {
        plan.name = match.captured(1);
    }

    return plan;
}

// ENTERPRISE-GRADE: Set power plan using powercfg -SETACTIVE
bool OptimizePowerSettingsAction::setPowerPlan(const QString& guid) {
    Q_EMIT executionProgress("Activating power plan...", 60);

    ProcessResult proc = runProcess("powercfg", QStringList() << "-SETACTIVE" << guid,
        sak::kTimeoutProcessShortMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Power plan activate warning: " + proc.std_err.trimmed());
    }
    return proc.succeeded();
}

// ENTERPRISE-GRADE: Get active power plan using powercfg -GETACTIVESCHEME
OptimizePowerSettingsAction::PowerPlan OptimizePowerSettingsAction::getActivePowerPlan() {
    PowerPlan active_plan;

    ProcessResult proc = runProcess("powercfg", QStringList() << "-GETACTIVESCHEME",
        sak::kTimeoutProcessShortMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Power plan active query warning: " + proc.std_err.trimmed());
    }
    QString output = proc.std_out;

    // Parse: "Power Scheme GUID: {guid} (Plan Name)"
    QRegularExpression regex(R"(Power Scheme GUID:\s*([0-9a-f\-]+)\s*\(([^\)]+)\))");
    QRegularExpressionMatch match = regex.match(output);

    if (match.hasMatch()) {
        active_plan.guid = match.captured(1);
        active_plan.name = match.captured(2).trimmed();
        active_plan.isActive = true;
    }

    return active_plan;
}

// ENTERPRISE-GRADE: Find power plan by name (case-insensitive)
OptimizePowerSettingsAction::PowerPlan OptimizePowerSettingsAction::findPowerPlanByName(
    const QString& name) {
    QVector<PowerPlan> plans = enumeratePowerPlans();

    for (const PowerPlan& plan : plans) {
        if (plan.name.contains(name, Qt::CaseInsensitive)) {
            return plan;
        }
    }
    return PowerPlan();
}

// ENTERPRISE-GRADE: Standard power scheme GUIDs
QString OptimizePowerSettingsAction::getStandardPowerPlanGuid(const QString& plan_type) {
    if (plan_type == "High Performance" || plan_type == "high") {
        return "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c"; // SCHEME_MIN (High Performance)
    } else if (plan_type == "Balanced") {
        return "381b4222-f694-41f0-9685-ff5bb260df2e"; // SCHEME_BALANCED
    } else if (plan_type == "Power Saver") {
        return "a1841308-3541-4fab-bc81-f71556f20b4a"; // SCHEME_MAX (Power Saver)
    }
    return QString();
}

void OptimizePowerSettingsAction::scan() {
    setStatus(ActionStatus::Scanning);
    Q_ASSERT(status() == ActionStatus::Scanning);

    PowerPlan current_plan = getActivePowerPlan();

    ScanResult result;
    result.applicable = true;
    result.summary = current_plan.name.isEmpty()
        ? "Power plan detected"
        : QString("Active plan: %1").arg(current_plan.name);
    result.details = "Optimization will switch to High Performance if available";

    Q_ASSERT(!result.summary.isEmpty());

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

QString OptimizePowerSettingsAction::buildPowerPlanListReport(
    const PowerPlan& current_plan,
    const QVector<PowerPlan>& all_plans) const
{
    QString report = "╔════════════════════════════════════════════════════════════════╗\n";
    report += "║             POWER PLAN OPTIMIZATION REPORT                   ║\n";
    report += "╠════════════════════════════════════════════════════════════════╣\n";
    report += QString("║ Current Plan: %1\n").arg(current_plan.name).leftJustified(67, ' ') + "║\n";
    report += QString("║ Current GUID: %1\n").arg(current_plan.guid).leftJustified(67, ' ') + "║\n";
    report += "╠════════════════════════════════════════════════════════════════╣\n";
    report += QString("║ Available Power Plans: %1\n").arg(all_plans.size()).leftJustified(67,
        ' ') + "║\n";

    // List all plans
    for (const PowerPlan& plan : all_plans) {
        QString plan_line = QString("║   %1 %2\n")
                               .arg(plan.isActive ? "[ACTIVE]" : "        ")
                               .arg(plan.name);
        report += plan_line.leftJustified(67, ' ') + "║\n";
    }

    report += "╠════════════════════════════════════════════════════════════════╣\n";
    return report;
}

void OptimizePowerSettingsAction::finalizePowerOptimizationResult(
    const QDateTime& start_time,
    const QString& report,
    const QString& previous_plan_name,
    const QString& high_perf_guid,
    bool already_optimized,
    bool success)
{
    Q_EMIT executionProgress("Power optimization complete", 100);

    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    Q_ASSERT(!result.success);  // verify default init
    result.duration_ms = duration_ms;

    if (already_optimized) {
        result.success = true;
        result.message = "Already using High Performance power plan";
        result.log = report;
        result.log += QString("\nCompleted in %1 ms\n").arg(duration_ms);
        result.log += "RECOMMENDATIONS:\n";
        result.log += "• System already optimized for performance\n";
        result.log += "• Processor performance boost enabled\n";
        result.log += "• Minimal power management restrictions\n";
    } else if (success) {
        result.success = true;
        result.message = QString("Switched to High Performance (was: %1)").arg(previous_plan_name);
        result.log = report;
        result.log += QString("\nCompleted in %1 ms\n").arg(duration_ms);
        result.log += "RECOMMENDATIONS:\n";
        result.log += "• Performance boost enabled\n";
        result.log += "• Sleep/hibernate settings unchanged\n";
        result.log += "• Display timeout settings unchanged\n";
        result.log += "• Use powercfg -QUERY for detailed settings\n";
    } else {
        result.success = false;
        result.message = "Failed to activate High Performance plan";
        result.log = report;
        result.log += "\nFailed to change power plan - administrative privileges may be required\n";
        result.log += "Try running as Administrator or use: powercfg -SETACTIVE " + high_perf_guid +
            "\n";
    }

    finishWithResult(result, result.success ? ActionStatus::Success : ActionStatus::Failed);
}

bool OptimizePowerSettingsAction::activateHighPerformancePlan(
    const PowerPlan& high_perf_plan,
    const QString& current_plan_name,
    QString& report)
{
    bool success = setPowerPlan(high_perf_plan.guid);

    if (success) {
        Q_EMIT executionProgress("Verifying power plan activation...", 80);
        PowerPlan new_active = getActivePowerPlan();

        success = (new_active.guid == high_perf_plan.guid) ||
                 new_active.name.contains("High Performance", Qt::CaseInsensitive);

        if (success) {
            report += QString("║ Status:       Power plan activated\n").leftJustified(67,
                ' ') + "║\n";
            report += QString("║ Previous:     %1\n").arg(current_plan_name).leftJustified(67,
                ' ') + "║\n";
            report += QString("║ Current:      %1\n").arg(new_active.name).leftJustified(67,
                ' ') + "║\n";
        } else {
            report += QString("║ Status:       Activation verification FAILED\n").leftJustified(67,
                ' ') + "║\n";
        }
    } else {
        report += "║ Status:       Activation FAILED                             ║\n";
        report += "║ Error:        powercfg command failed                       ║\n";
    }
    return success;
}

void OptimizePowerSettingsAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("Power settings optimization cancelled");
        return;
    }

    setStatus(ActionStatus::Running);
    Q_ASSERT(status() == ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    Q_ASSERT(start_time.isValid());

    Q_EMIT executionProgress("Enumerating power plans...", 10);
    PowerPlan current_plan = getActivePowerPlan();

    Q_EMIT executionProgress("Scanning available power plans...", 25);
    QVector<PowerPlan> all_plans = enumeratePowerPlans();

    QString report = buildPowerPlanListReport(current_plan, all_plans);

    Q_EMIT executionProgress("Locating High Performance plan...", 40);
    PowerPlan high_perf_plan = findPowerPlanByName("High Performance");
    if (high_perf_plan.guid.isEmpty())
        high_perf_plan = findPowerPlanByName("Ultimate Performance");
    if (high_perf_plan.guid.isEmpty()) {
        high_perf_plan.guid = getStandardPowerPlanGuid("High Performance");
        high_perf_plan.name = "High Performance (Standard)";
    }

    report += QString("║ Target Plan:  %1\n").arg(high_perf_plan.name).leftJustified(67,
        ' ') + "║\n";
    report += QString("║ Target GUID:  %1\n").arg(high_perf_plan.guid).leftJustified(67,
        ' ') + "║\n";
    report += "╠════════════════════════════════════════════════════════════════╣\n";

    bool already_optimized = current_plan.name.contains("High Performance", Qt::CaseInsensitive) ||
                            current_plan.name.contains("Ultimate Performance", Qt::CaseInsensitive);
    bool success = true;

    if (already_optimized) {
        report += QString("║ Status:       Already using High Performance\n").leftJustified(67,
            ' ') + "║\n";
        report += QString("║ Action:       No change needed\n").leftJustified(67, ' ') + "║\n";
    } else {
        success = activateHighPerformancePlan(high_perf_plan, current_plan.name, report);
    }

    report += "╚════════════════════════════════════════════════════════════════╝\n";
    finalizePowerOptimizationResult(start_time, report, current_plan.name,
                                     high_perf_plan.guid, already_optimized, success);
}

} // namespace sak

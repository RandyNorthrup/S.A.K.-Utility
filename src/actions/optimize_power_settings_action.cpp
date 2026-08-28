// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file optimize_power_settings_action.cpp
/// @brief Implements Windows power plan optimization for performance

#include "sak/actions/optimize_power_settings_action.h"

#include "sak/action_constants.h"
#include "sak/layout_constants.h"
#include "sak/process_runner.h"

#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>

#include <algorithm>
#include <limits>

namespace sak {

namespace {

// LOCALE-INDEPENDENT. The old pattern required the English label "Power Scheme GUID:", so on a
// non-English Windows it matched nothing, the plan list came back EMPTY and the whole
// optimisation refused -- fail-closed, but the feature was simply dead. A scheme GUID and the
// trailing '*' active marker do not localise, so the match anchors on those; the parenthesised
// NAME is captured for display only. Same rule as the DNS-cache and ethernet-config fixes.
constexpr auto kPowerPlanListPattern =
    "([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})\\s*"
    "\\(([^\\)]+)\\)(\\s*\\*)?";
// These two were MISSED when the list pattern above was de-localized, and the omission mattered:
// kActivePowerPlanPattern is how the CURRENTLY ACTIVE plan is identified, so on a non-English
// Windows the "already using High Performance" check could never succeed and the action would
// re-activate on every run. Anchored on the GUID for the same reason as the list pattern -- the
// English label is the only part that translates.
constexpr auto kPowerPlanNamePattern =
    "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\s*\\(([^\\)]+)\\)";
constexpr auto kActivePowerPlanPattern =
    "([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})\\s*\\(([^\\)]+)\\)";
constexpr int kPowerPlanGuidCaptureGroup = 1;
constexpr int kPowerPlanNameCaptureGroup = 2;
constexpr int kPowerPlanActiveMarkerCaptureGroup = 3;
constexpr int kReportInnerWidth = 67;

// Built-in high-performance scheme GUIDs (lower-case, canonical).
constexpr auto kHighPerformanceGuid = "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c";      // SCHEME_MIN
constexpr auto kUltimatePerformanceGuid = "e9a42b02-d5df-448d-aa00-03f14749eb61";  // Ultimate

// Well-known power SETTING GUIDs (not scheme GUIDs). Stable across Windows versions and
// locales, which is exactly why the read-back keys on them instead of on the printed labels.
constexpr auto kDisplayIdleSettingGuid = "3c0bc021-c8a8-4e07-a973-6b14cbcb2b7e";    // VIDEOIDLE
constexpr auto kSleepIdleSettingGuid = "29f6c1db-86da-48c5-9fdb-f2b67b1f44da";      // STANDBYIDLE
constexpr auto kHibernateIdleSettingGuid = "9d7815a6-7ee4-497e-8888-515a05f02364";  // HIBERNATEIDLE

// A setting block lists exactly two readings, "Current AC ..." then "Current DC ...", in that
// order. Anything else means the block was not the shape this parser understands, and the
// caller is told the value was not read rather than handed a guess.
constexpr int kPowerReadingsPerSetting = 2;
// sak::kSecondsPerMinute (layout_constants.h) is the canonical one; do not add a second copy.
constexpr int kMinutesPerHour = 60;
constexpr int kHexadecimalBase = 16;

/// Count the leading spaces of @p line. Indentation is emitted by powercfg's own format
/// strings and is not part of any translated text, which is what makes it a safe anchor.
[[nodiscard]] int leadingSpaces(const QString& line) {
    int count = 0;
    while (count < line.size() && line.at(count) == QLatin1Char(' ')) {
        ++count;
    }
    return count;
}

/// True when @p line carries a GUID, i.e. it OPENS a scheme/subgroup/setting block rather than
/// reporting a value inside one. Used as the block terminator so a setting's readings are never
/// read out of the following block.
[[nodiscard]] bool lineOpensGuidBlock(const QString& line) {
    static const QRegularExpression kGuidRe(
        QStringLiteral("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}"),
        QRegularExpression::CaseInsensitiveOption);
    return kGuidRe.match(line).hasMatch();
}

/// Extract a `0x...` reading from @p line, or -1 when it carries none.
[[nodiscard]] qint64 hexReading(const QString& line) {
    static const QRegularExpression kHexRe(QStringLiteral("0x([0-9a-f]+)"),
                                           QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = kHexRe.match(line);
    if (!match.hasMatch()) {
        return -1;
    }
    bool ok = false;
    const qulonglong value = match.captured(1).toULongLong(&ok, kHexadecimalBase);
    if (!ok || value > static_cast<qulonglong>(std::numeric_limits<qint64>::max())) {
        return -1;
    }
    return static_cast<qint64>(value);
}

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
    const ProcessResult proc = runProcess(sak::system32Path(QStringLiteral("powercfg.exe")),
                                          QStringList() << "-LIST",
                                          sak::kTimeoutProcessShortMs);
    // Whether powercfg itself ran. A failed run must not later be treated as "no plans found"
    // and coerced into a hard-coded-GUID mutation.
    discovery_ok = proc.succeeded();
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Power plan list warning: " + proc.std_err.trimmed());
    }
    return parsePowerPlanList(proc.std_out);
}

QVector<OptimizePowerSettingsAction::PowerPlan> OptimizePowerSettingsAction::parsePowerPlanList(
    const QString& output) {
    QVector<PowerPlan> plans;
    const QRegularExpression plan_regex(QString::fromLatin1(kPowerPlanListPattern),
                                        QRegularExpression::CaseInsensitiveOption);

    QRegularExpressionMatchIterator it = plan_regex.globalMatch(output);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        PowerPlan plan;
        // Lower-cased so a GUID comparison never depends on how powercfg happened to print it;
        // the built-in GUID constants in this file are canonical lower-case.
        plan.guid = match.captured(kPowerPlanGuidCaptureGroup).toLower();
        plan.name = match.captured(kPowerPlanNameCaptureGroup).trimmed();
        plan.isActive = !match.captured(kPowerPlanActiveMarkerCaptureGroup).isEmpty();
        plans.append(plan);
    }

    return plans;
}

namespace {

/// Index of the line opening @p setting_guid's block, or -1 when this scheme does not list it.
[[nodiscard]] int findSettingBlockLine(const QStringList& lines, const QString& setting_guid) {
    for (int index = 0; index < lines.size(); ++index) {
        if (lines.at(index).contains(setting_guid, Qt::CaseInsensitive)) {
            return index;
        }
    }
    return -1;
}

/// Collect the hex readings of the block opened at @p start_line.
///
/// The readings sit at the SAME indent as the line that opened the block, while the block's
/// Minimum/Maximum/increment attributes are indented deeper -- so filtering on that indent is
/// what keeps a "Maximum Possible Setting: 0xffffffff" from being reported as a live setting.
/// The walk stops at the next line that both opens a GUID block and is no deeper, so one
/// setting's readings can never be completed from the following setting's.
[[nodiscard]] QVector<qint64> collectBlockReadings(const QStringList& lines, int start_line) {
    const int block_indent = leadingSpaces(lines.at(start_line));
    QVector<qint64> readings;
    for (int index = start_line + 1; index < lines.size(); ++index) {
        const QString& line = lines.at(index);
        if (line.trimmed().isEmpty()) {
            continue;
        }
        const int indent = leadingSpaces(line);
        if (indent <= block_indent && lineOpensGuidBlock(line)) {
            break;  // the next scheme/subgroup/setting block starts here
        }
        if (indent != block_indent) {
            continue;  // a block attribute, not a reading
        }
        const qint64 reading = hexReading(line);
        if (reading >= 0) {
            readings.append(reading);
        }
    }
    return readings;
}

}  // namespace

OptimizePowerSettingsAction::PowerTimeout OptimizePowerSettingsAction::parsePowerTimeout(
    const QString& query_output, const QString& setting_guid) {
    PowerTimeout timeout;
    const QStringList lines = query_output.split(QLatin1Char('\n'));
    const int start_line = findSettingBlockLine(lines, setting_guid);
    if (start_line < 0) {
        return timeout;  // setting absent from this scheme -- NOT read, and said so
    }

    // EXACTLY two, in AC-then-DC order. Fewer means the block was not the shape this parser
    // understands; more means it read something it should not have. Either way the honest
    // answer is "not read", never a plausible-looking number.
    const QVector<qint64> readings = collectBlockReadings(lines, start_line);
    if (readings.size() != kPowerReadingsPerSetting) {
        return timeout;
    }
    timeout.ac_seconds = readings.at(0);
    timeout.dc_seconds = readings.at(1);
    timeout.found = true;
    return timeout;
}

OptimizePowerSettingsAction::PowerTimeouts OptimizePowerSettingsAction::parsePowerTimeouts(
    const QString& query_output) {
    PowerTimeouts timeouts;
    timeouts.display = parsePowerTimeout(query_output,
                                         QString::fromLatin1(kDisplayIdleSettingGuid));
    timeouts.sleep = parsePowerTimeout(query_output, QString::fromLatin1(kSleepIdleSettingGuid));
    timeouts.hibernate = parsePowerTimeout(query_output,
                                           QString::fromLatin1(kHibernateIdleSettingGuid));
    return timeouts;
}

QString OptimizePowerSettingsAction::formatPowerTimeout(qint64 seconds) {
    // powercfg encodes "never time out" as 0. Printing a bare "0" would read as an immediate
    // timeout -- the exact opposite of the setting's meaning -- to the technician this report
    // exists for.
    if (seconds <= 0) {
        return QStringLiteral("Never");
    }
    if (seconds < kSecondsPerMinute) {
        return QStringLiteral("%1 sec").arg(seconds);
    }
    const qint64 total_minutes = seconds / kSecondsPerMinute;
    const qint64 trailing_seconds = seconds % kSecondsPerMinute;
    QString rendered;
    if (total_minutes >= kMinutesPerHour) {
        rendered = QStringLiteral("%1 hr").arg(total_minutes / kMinutesPerHour);
        const qint64 trailing_minutes = total_minutes % kMinutesPerHour;
        if (trailing_minutes > 0) {
            rendered += QStringLiteral(" %1 min").arg(trailing_minutes);
        }
    } else {
        rendered = QStringLiteral("%1 min").arg(total_minutes);
    }
    if (trailing_seconds > 0) {
        rendered += QStringLiteral(" %1 sec").arg(trailing_seconds);
    }
    return rendered;
}

// ENTERPRISE-GRADE: Get detailed power plan information using powercfg -QUERY
OptimizePowerSettingsAction::PowerPlanDetails OptimizePowerSettingsAction::queryPowerPlan(
    const QString& guid) {
    PowerPlanDetails details;
    details.plan.guid = guid;
    details.plan.isActive = false;

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
        details.plan.name = match.captured(kPowerPlanGuidCaptureGroup);
    }

    // The settings tree is the reason this query is run at all; reading only the name meant
    // paying for the query and throwing away its answer.
    details.timeouts = parsePowerTimeouts(output);
    return details;
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
    return parseActivePowerPlan(proc.std_out);
}

OptimizePowerSettingsAction::PowerPlan OptimizePowerSettingsAction::parseActivePowerPlan(
    const QString& output) {
    PowerPlan active_plan;
    const QRegularExpression regex(QString::fromLatin1(kActivePowerPlanPattern),
                                   QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = regex.match(output);

    if (match.hasMatch()) {
        // Lower-cased so a comparison against the canonical built-in GUIDs holds regardless of
        // how powercfg happened to print it.
        active_plan.guid = match.captured(kPowerPlanGuidCaptureGroup).toLower();
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
    auto it = std::ranges::find_if(plans, [&name](const PowerPlan& plan) {
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
    finalizePowerOptimizationResult({.start_time = start_time,
                                     .report = report,
                                     .previous_plan_name = previous_plan_name,
                                     .high_perf_guid = QString(),
                                     .already_optimized = false,
                                     .success = false});
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
    auto by_guid = std::ranges::find_if(plans, [](const PowerPlan& plan) {
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

void OptimizePowerSettingsAction::appendTargetPlanHeader(const PowerPlan& target_plan,
                                                         QString& report) {
    report += QString("| Target Plan:  %1\n")
                  .arg(target_plan.name)
                  .leftJustified(kReportInnerWidth, ' ') +
              "|\n";
    report += QString("| Target GUID:  %1\n")
                  .arg(target_plan.guid)
                  .leftJustified(kReportInnerWidth, ' ') +
              "|\n";
    report += "+================================================================+\n";
}

OptimizePowerSettingsAction::PowerTimeouts OptimizePowerSettingsAction::readEffectiveTimeouts(
    const QString& fallback_guid) {
    // READ BACK WHAT IS NOW IN FORCE. Queried AFTER activation, and against the plan that is
    // ACTUALLY active rather than the one this action aimed at, so the figures describe the
    // machine as the technician will find it. If the active-scheme query cannot answer, the
    // target GUID is the next best subject -- and any setting that then fails to parse is
    // reported as unread rather than filled in.
    Q_EMIT executionProgress("Reading effective power settings...", progress::kStep90);
    const PowerPlan active_now = getActivePowerPlan();
    const QString query_guid = active_now.guid.isEmpty() ? fallback_guid : active_now.guid;
    return queryPowerPlan(query_guid).timeouts;
}

void OptimizePowerSettingsAction::appendEffectiveTimeouts(const PowerTimeouts& timeouts,
                                                          QString& log) {
    // Report what was MEASURED, and name what was not. The previous version of these lines told
    // the technician to go and run powercfg -QUERY themselves -- while the action already had a
    // queryPowerPlan() that ran exactly that command and discarded its answer.
    struct Row {
        const char* label;
        const PowerTimeout* timeout;
    };
    const Row rows[] = {
        {"Display off", &timeouts.display},
        {"Sleep after", &timeouts.sleep},
        {"Hibernate after", &timeouts.hibernate},
    };
    log += "EFFECTIVE IDLE TIMEOUTS (read back from powercfg):\n";
    for (const Row& row : rows) {
        if (!row.timeout->found) {
            // Never a substituted default: a value this action could not read is reported as
            // unread, which is the whole reason the read-back exists.
            log += QString("*   %1: NOT READ (powercfg did not report this setting)\n")
                       .arg(QString::fromLatin1(row.label));
            continue;
        }
        log += QString("*   %1: %2 on AC, %3 on battery\n")
                   .arg(QString::fromLatin1(row.label),
                        formatPowerTimeout(row.timeout->ac_seconds),
                        formatPowerTimeout(row.timeout->dc_seconds));
    }
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
        // Stated as what the PLAN is, not as measured facts. These lines used to read
        // "Processor performance boost enabled" and "Minimal power management restrictions" --
        // assertions about the machine's effective settings that nothing here ever queried.
        // Switching schemes can change sleep, display and processor settings, so a report that
        // announces them without reading them is telling a technician something it does not know.
        result.log += "RECOMMENDATIONS:\n";
        result.log += "* System is already on the High Performance plan\n";
        result.log += "* That plan is Windows' least power-restricted scheme\n";
        appendEffectiveTimeouts(context.effective_timeouts, result.log);
    } else if (context.success) {
        result.success = true;
        result.message =
            QString("Switched to High Performance (was: %1)").arg(context.previous_plan_name);
        result.log = context.report;
        result.log += QString("\nCompleted in %1 ms\n").arg(duration_ms);
        // "Sleep/hibernate settings unchanged" and "Display timeout settings unchanged" were not
        // merely unverified, they were WRONG: every scheme carries its OWN sleep, hibernate and
        // display timeouts, so activating a different plan changes those effective values. The
        // report told a technician the opposite of what the action had just done.
        result.log += "RECOMMENDATIONS:\n";
        result.log +=
            "* The active plan changed, so its sleep, hibernate and display timeouts "
            "now apply INSTEAD of the previous plan's\n";
        appendEffectiveTimeouts(context.effective_timeouts, result.log);
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

    appendTargetPlanHeader(high_perf_plan, report);

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

    // Skipped when activation failed: the report's failure branch must not print settings as
    // though something had been applied.
    const PowerTimeouts effective_timeouts = success ? readEffectiveTimeouts(high_perf_plan.guid)
                                                     : PowerTimeouts{};

    finalizePowerOptimizationResult({.start_time = start_time,
                                     .report = report,
                                     .previous_plan_name = current_plan.name,
                                     .high_perf_guid = high_perf_plan.guid,
                                     .effective_timeouts = effective_timeouts,
                                     .already_optimized = already_optimized,
                                     .success = success});
}

}  // namespace sak

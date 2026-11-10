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

// The report frame: a border of kReportRuleChars '=' between two '+', and rows of "| " + text +
// " |" padded to the same width. Rows used to end in '\n' BEFORE they were padded, so the padding
// and the closing bar landed on the next line, and the pad width (67) exceeded the 66-character
// border anyway: every row of the report rendered broken.
constexpr int kReportRuleChars = 64;
constexpr int kReportRowFrameChars = 2;  // the "| " opening and " |" closing take one column each
constexpr int kReportTextChars = kReportRuleChars - kReportRowFrameChars;

QString reportRule() {
    return QStringLiteral("+") + QString(kReportRuleChars, QLatin1Char('=')) +
           QStringLiteral("+\n");
}

QString reportTitleRow(const QString& title) {
    constexpr qsizetype kCenteringSides = 2;  // the free space splits between left and right
    const qsizetype left_pad =
        std::max<qsizetype>(0, (kReportTextChars - title.size()) / kCenteringSides);
    return QStringLiteral("| ") +
           (QString(left_pad, QLatin1Char(' ')) + title).leftJustified(kReportTextChars) +
           QStringLiteral(" |\n");
}

// The ONE built-in High Performance scheme GUID (SCHEME_MIN, lower-case, canonical). It was
// written twice -- here and as a literal inside a name-to-GUID lookup whose Balanced and Power
// Saver branches no caller could reach -- and is now only here.
constexpr auto kHighPerformanceGuid = "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c";
constexpr auto kUltimatePerformanceGuid = "e9a42b02-d5df-448d-aa00-03f14749eb61";  // Ultimate

// powercfg reports its failures on stdout as often as on stderr, so the cause is whichever
// stream carries text.
QString powercfgFailure(const QString& command, const ProcessResult& run) {
    if (run.timed_out) {
        return QStringLiteral("%1 timed out").arg(command);
    }
    QString detail = run.std_err.trimmed();
    if (detail.isEmpty()) {
        detail = run.std_out.trimmed();
    }
    if (detail.isEmpty()) {
        detail = QStringLiteral("no output");
    }
    return QStringLiteral("%1 failed (exit code %2): %3")
        .arg(command)
        .arg(run.exit_code)
        .arg(detail);
}

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
std::expected<void, QString> OptimizePowerSettingsAction::setPowerPlan(const QString& guid) {
    Q_EMIT executionProgress("Activating power plan...", progress::kStep60);

    const ProcessResult proc = runProcess(sak::system32Path(QStringLiteral("powercfg.exe")),
                                          QStringList() << "-SETACTIVE" << guid,
                                          sak::kTimeoutProcessShortMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Power plan activate warning: " + proc.std_err.trimmed());
    }
    if (!proc.succeeded()) {
        return std::unexpected(powercfgFailure(QStringLiteral("powercfg -SETACTIVE"), proc));
    }
    return {};
}

// ENTERPRISE-GRADE: Get active power plan using powercfg -GETACTIVESCHEME
std::expected<OptimizePowerSettingsAction::PowerPlan, QString>
OptimizePowerSettingsAction::getActivePowerPlan() {
    const ProcessResult proc = runProcess(sak::system32Path(QStringLiteral("powercfg.exe")),
                                          QStringList() << "-GETACTIVESCHEME",
                                          sak::kTimeoutProcessShortMs);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Power plan active query warning: " + proc.std_err.trimmed());
    }
    return activePlanFromQuery(proc);
}

std::expected<OptimizePowerSettingsAction::PowerPlan, QString>
OptimizePowerSettingsAction::activePlanFromQuery(const ProcessResult& query) {
    if (!query.succeeded()) {
        return std::unexpected(powercfgFailure(QStringLiteral("powercfg -GETACTIVESCHEME"), query));
    }
    const PowerPlan active = parseActivePowerPlan(query.std_out);
    if (active.guid.isEmpty()) {
        return std::unexpected(QStringLiteral("powercfg -GETACTIVESCHEME printed no scheme GUID"));
    }
    return active;
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

bool OptimizePowerSettingsAction::discoveryPermitsActivation(bool discovery_ok, int plans_found) {
    return discovery_ok && plans_found > 0;
}

QString OptimizePowerSettingsAction::reportRow(const QString& text) {
    // A row whose text is wider than the frame is printed whole rather than cut: a plan name is
    // the information, and a truncated one is a different name.
    return QStringLiteral("| ") + text.leftJustified(kReportTextChars) + QStringLiteral(" |\n");
}

void OptimizePowerSettingsAction::finalizeDiscoveryFailure(const QDateTime& start_time,
                                                           QString& report,
                                                           const QString& previous_plan_name,
                                                           const QString& reason) {
    report += reportRow(QStringLiteral("Status:       Power plan discovery FAILED"));
    report +=
        reportRow(QStringLiteral("Error:        Could not enumerate power plans; not mutating"));
    report += reportRule();
    finalizePowerOptimizationResult({.start_time = start_time,
                                     .report = report,
                                     .previous_plan_name = previous_plan_name,
                                     .high_perf_guid = QString(),
                                     .effective_timeouts = PowerTimeouts(),
                                     .outcome = Outcome::DiscoveryFailed,
                                     .failure_reason = reason});
}

std::optional<OptimizePowerSettingsAction::PowerPlan>
OptimizePowerSettingsAction::resolveHighPerformancePlan(const QVector<PowerPlan>& plans,
                                                        bool discovery_ok) {
    // Fail closed if powercfg discovery did not actually succeed and return plans: we must never
    // mutate the system on a GUID that discovery did not stand behind.
    if (!discoveryPermitsActivation(discovery_ok, static_cast<int>(plans.size()))) {
        return std::nullopt;
    }
    // A listed BUILT-IN scheme, by GUID -- the same predicate the already-optimized check uses,
    // so the plan activated here is the plan a later run recognises.
    const auto by_guid = std::ranges::find_if(plans, [](const PowerPlan& plan) {
        return isHighPerformanceGuid(plan.guid);
    });
    if (by_guid != plans.end()) {
        return *by_guid;
    }
    // Not listed: some SKUs hide the built-in High Performance plan. Its canonical GUID is
    // Microsoft's documented SCHEME_MIN, not a guess, and discovery is known good here, so it is
    // what gets activated. A custom plan that merely carries the name is NOT taken instead -- the
    // name is the one thing any plan can claim. If Windows refuses the hidden scheme, activation
    // fails and says so.
    PowerPlan built_in;
    built_in.guid = QString::fromLatin1(kHighPerformanceGuid);
    built_in.name = QStringLiteral("High performance (built-in, not listed by powercfg)");
    return built_in;
}

QString OptimizePowerSettingsAction::buildPowerPlanListReport(
    const std::expected<PowerPlan, QString>& current_plan, const QVector<PowerPlan>& all_plans) {
    QString report = reportRule();
    report += reportTitleRow(QStringLiteral("POWER PLAN OPTIMIZATION REPORT"));
    report += reportRule();
    if (current_plan) {
        report += reportRow(QStringLiteral("Current Plan: %1").arg(current_plan->name));
        report += reportRow(QStringLiteral("Current GUID: %1").arg(current_plan->guid));
    } else {
        report += reportRow(QStringLiteral("Current Plan: NOT READ"));
        report += reportRow(QStringLiteral("Reason:       %1").arg(current_plan.error()));
    }
    report += reportRule();
    report += reportRow(QStringLiteral("Available Power Plans: %1").arg(all_plans.size()));
    for (const PowerPlan& plan : all_plans) {
        report += reportRow(QStringLiteral("  %1 %2").arg(
            plan.isActive ? QStringLiteral("[ACTIVE]") : QStringLiteral("        "), plan.name));
    }
    report += reportRule();
    return report;
}

void OptimizePowerSettingsAction::appendTargetPlanHeader(const PowerPlan& target_plan,
                                                         QString& report) {
    report += reportRow(QStringLiteral("Target Plan:  %1").arg(target_plan.name));
    report += reportRow(QStringLiteral("Target GUID:  %1").arg(target_plan.guid));
    report += reportRule();
}

OptimizePowerSettingsAction::PowerTimeouts OptimizePowerSettingsAction::readEffectiveTimeouts(
    const QString& fallback_guid) {
    // READ BACK WHAT IS NOW IN FORCE. Queried AFTER activation, and against the plan that is
    // ACTUALLY active rather than the one this action aimed at, so the figures describe the
    // machine as the technician will find it. If the active-scheme query cannot answer, the
    // target GUID is the next best subject -- and any setting that then fails to parse is
    // reported as unread rather than filled in.
    Q_EMIT executionProgress("Reading effective power settings...", progress::kStep90);
    const auto active_now = getActivePowerPlan();
    const QString query_guid = active_now ? active_now->guid : fallback_guid;
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
        {.label = "Display off", .timeout = &timeouts.display},
        {.label = "Sleep after", .timeout = &timeouts.sleep},
        {.label = "Hibernate after", .timeout = &timeouts.hibernate},
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

void OptimizePowerSettingsAction::appendSuccessLog(const OptimizationResultContext& context,
                                                   QString& log) {
    log += "RECOMMENDATIONS:\n";
    if (context.outcome == Outcome::AlreadyOptimized) {
        // Stated as what the PLAN is, not as measured facts. These lines used to read
        // "Processor performance boost enabled" and "Minimal power management restrictions" --
        // assertions about the machine's effective settings that nothing here ever queried.
        // Switching schemes can change sleep, display and processor settings, so a report that
        // announces them without reading them is telling a technician something it does not know.
        log += "* System is already on the High performance plan\n";
        log += "* That plan is Windows' least power-restricted scheme\n";
    } else {
        // "Sleep/hibernate settings unchanged" and "Display timeout settings unchanged" were not
        // merely unverified, they were WRONG: every scheme carries its OWN sleep, hibernate and
        // display timeouts, so activating a different plan changes those effective values. The
        // report told a technician the opposite of what the action had just done.
        log +=
            "* The active plan changed, so its sleep, hibernate and display timeouts "
            "now apply INSTEAD of the previous plan's\n";
    }
    appendEffectiveTimeouts(context.effective_timeouts, log);
}

OptimizePowerSettingsAction::FailureText OptimizePowerSettingsAction::failureText(
    Outcome outcome, const QString& target_guid, const QString& reason) {
    FailureText text;
    if (outcome == Outcome::DiscoveryFailed) {
        // Nothing was attempted, so this is not an activation failure, and there is no plan to
        // offer back as a command.
        text.message = QStringLiteral("Power plan discovery failed; no plan was changed");
        text.log_tail = QStringLiteral(
                            "\nCould not list the power plans (%1), so nothing was "
                            "activated.\n")
                            .arg(reason);
        return text;
    }
    text.message = outcome == Outcome::VerificationFailed
                       ? QStringLiteral("High performance activation could not be verified")
                       : QStringLiteral("Failed to activate High performance plan");
    // The cause in powercfg's own words. The old line guessed "administrative privileges may be
    // required" whatever powercfg said; this action does not need elevation, and a hidden plan
    // or a policy-managed power scheme fails the same command for reasons that guess hides.
    text.log_tail = QStringLiteral("\n%1\n").arg(reason);
    if (!target_guid.isEmpty()) {
        text.log_tail +=
            QStringLiteral("To retry by hand: powercfg -SETACTIVE %1\n").arg(target_guid);
    }
    return text;
}

void OptimizePowerSettingsAction::finalizePowerOptimizationResult(
    const OptimizationResultContext& context) {
    Q_EMIT executionProgress("Power optimization complete", progress::kComplete);

    const qint64 duration_ms = context.start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.log = context.report;
    result.success = context.outcome == Outcome::AlreadyOptimized ||
                     context.outcome == Outcome::Switched;

    if (result.success) {
        result.message = context.outcome == Outcome::AlreadyOptimized
                             ? QStringLiteral("Already using High performance power plan")
                             : QStringLiteral("Switched to High performance (was: %1)")
                                   .arg(context.previous_plan_name);
        result.log += QString("\nCompleted in %1 ms\n").arg(duration_ms);
        appendSuccessLog(context, result.log);
    } else {
        const FailureText failure =
            failureText(context.outcome, context.high_perf_guid, context.failure_reason);
        result.message = failure.message;
        result.log += failure.log_tail;
    }

    finishWithResult(result, result.success ? ActionStatus::Success : ActionStatus::Failed);
}

OptimizePowerSettingsAction::Outcome OptimizePowerSettingsAction::activateHighPerformancePlan(
    const PowerPlan& high_perf_plan,
    const QString& current_plan_name,
    QString& report,
    QString& reason) {
    const auto activated = setPowerPlan(high_perf_plan.guid);
    if (!activated) {
        reason = activated.error();
        report += reportRow(QStringLiteral("Status:       Activation FAILED"));
        report += reportRow(QStringLiteral("Error:        %1").arg(reason));
        return Outcome::ActivationFailed;
    }

    Q_EMIT executionProgress("Verifying power plan activation...", progress::kStep80);
    const auto new_active = getActivePowerPlan();
    if (!new_active) {
        reason = QStringLiteral(
                     "powercfg -SETACTIVE succeeded, but the active plan could not be "
                     "read back: %1")
                     .arg(new_active.error());
        report += reportRow(QStringLiteral("Status:       Activation verification FAILED"));
        report += reportRow(QStringLiteral("Error:        %1").arg(new_active.error()));
        return Outcome::VerificationFailed;
    }
    // Verify by GUID: either the exact plan we set, or any built-in high-performance scheme --
    // never a name match.
    if (new_active->guid.compare(high_perf_plan.guid, Qt::CaseInsensitive) != 0 &&
        !isHighPerformanceGuid(new_active->guid)) {
        reason =
            QStringLiteral("powercfg -SETACTIVE succeeded, but the active plan is still %1 (%2)")
                .arg(new_active->name, new_active->guid);
        report += reportRow(QStringLiteral("Status:       Activation verification FAILED"));
        report += reportRow(QStringLiteral("Active:       %1").arg(new_active->name));
        return Outcome::VerificationFailed;
    }
    report += reportRow(QStringLiteral("Status:       Power plan activated"));
    report += reportRow(QStringLiteral("Previous:     %1").arg(current_plan_name));
    report += reportRow(QStringLiteral("Current:      %1").arg(new_active->name));
    return Outcome::Switched;
}

void OptimizePowerSettingsAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("Power settings optimization cancelled");
        return;
    }

    setStatus(ActionStatus::Running);
    const QDateTime start_time = QDateTime::currentDateTime();
    Q_EMIT executionProgress("Enumerating power plans...", progress::kStep10);
    const auto current_plan = getActivePowerPlan();
    const QString current_name = current_plan ? current_plan->name : QStringLiteral("unknown");

    Q_EMIT executionProgress("Scanning available power plans...", progress::kStep25);
    bool discovery_ok = false;
    const QVector<PowerPlan> all_plans = enumeratePowerPlans(discovery_ok);

    QString report = buildPowerPlanListReport(current_plan, all_plans);

    Q_EMIT executionProgress("Locating High performance plan...", progress::kStep40);
    const std::optional<PowerPlan> high_perf_plan = resolveHighPerformancePlan(all_plans,
                                                                               discovery_ok);
    if (!high_perf_plan) {
        // Discovery failed: fail closed. Do NOT mutate the active power plan on a guessed GUID.
        finalizeDiscoveryFailure(start_time,
                                 report,
                                 current_name,
                                 discovery_ok ? QStringLiteral("powercfg -LIST listed no plans")
                                              : QStringLiteral("powercfg -LIST failed"));
        return;
    }

    appendTargetPlanHeader(*high_perf_plan, report);

    // Match by GUID, not by name: a custom plan named e.g. "My High Performance Rig" must NOT be
    // treated as the built-in plan (which would skip switching to the real one). An active plan
    // that could not be read is not "already optimized" either.
    Outcome outcome = Outcome::AlreadyOptimized;
    QString failure_reason;
    if (current_plan && isHighPerformanceGuid(current_plan->guid)) {
        report += reportRow(QStringLiteral("Status:       Already using High performance"));
        report += reportRow(QStringLiteral("Action:       No change needed"));
    } else {
        // Discovery ran several powercfg processes; honor a cancellation that arrived
        // during it before mutating the active plan with -SETACTIVE (fail closed).
        if (isCancelled()) {
            emitCancelledResult("Power settings optimization cancelled");
            return;
        }
        outcome =
            activateHighPerformancePlan(*high_perf_plan, current_name, report, failure_reason);
    }

    report += reportRule();

    // Skipped when activation failed: the report's failure branch must not print settings as
    // though something had been applied.
    const bool applied = outcome == Outcome::AlreadyOptimized || outcome == Outcome::Switched;
    const PowerTimeouts effective_timeouts = applied ? readEffectiveTimeouts(high_perf_plan->guid)
                                                     : PowerTimeouts{};

    finalizePowerOptimizationResult({.start_time = start_time,
                                     .report = report,
                                     .previous_plan_name = current_name,
                                     .high_perf_guid = high_perf_plan->guid,
                                     .effective_timeouts = effective_timeouts,
                                     .outcome = outcome,
                                     .failure_reason = failure_reason});
}

}  // namespace sak

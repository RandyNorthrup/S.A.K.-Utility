// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_app_action_planner.h"

#include "sak/ai/ai_command_guard.h"
#include "sak/ai/ai_command_tool_planner.h"
#include "sak/ai/ai_tool_policy.h"

#include <QLatin1String>

#include <algorithm>

namespace sak::ai {

namespace {
constexpr int kAppActionDefaultTimeoutSeconds = 1800;
constexpr int kAppActionMinTimeoutSeconds = 5;
constexpr int kAppActionMaxTimeoutSeconds = 14'400;

// Read a manifest safety flag fail closed. A present-but-mistyped value (e.g. the string
// "true", or a number) must NOT silently downgrade to false and strip the risk/admin
// treatment. Only an explicit JSON bool is honored verbatim; any other PRESENT value is
// treated as set (risky/admin); an absent/null value stays the documented default (false).
bool safetyFlag(const QJsonObject& profile, const QString& key) {
    const QJsonValue value = profile.value(key);
    if (value.isBool()) {
        return value.toBool();
    }
    return !(value.isUndefined() || value.isNull());
}

// Validate a single win32_gui recipe step: an object with a non-empty 'tool', and -- when
// present -- correctly-typed 'arguments' (object), 'optional' (bool), and 'timeout_ms'
// (number). Returns empty on success, else an error string. Mistyped optional fields are a
// malformed recipe and are rejected rather than silently coerced.
QString validateWin32GuiStep(const QJsonValue& value, int index) {
    if (!value.isObject()) {
        return QStringLiteral("win32_gui step %1 must be an object").arg(index);
    }
    const QJsonObject step = value.toObject();
    if (step.value(QStringLiteral("tool")).toString().trimmed().isEmpty()) {
        return QStringLiteral("win32_gui step %1 is missing a 'tool' name").arg(index);
    }
    if (step.contains(QStringLiteral("arguments")) &&
        !step.value(QStringLiteral("arguments")).isObject()) {
        return QStringLiteral("win32_gui step %1 'arguments' must be an object").arg(index);
    }
    if (step.contains(QStringLiteral("optional")) &&
        !step.value(QStringLiteral("optional")).isBool()) {
        return QStringLiteral("win32_gui step %1 'optional' must be a boolean").arg(index);
    }
    if (step.contains(QStringLiteral("timeout_ms")) &&
        !step.value(QStringLiteral("timeout_ms")).isDouble()) {
        return QStringLiteral("win32_gui step %1 'timeout_ms' must be a number").arg(index);
    }
    return {};
}

// Structurally validate a win32_gui recipe: a non-empty array of steps (see
// validateWin32GuiStep). Deeper checks (tool exists / is not high-risk) are enforced at
// execution time by the step runner via planWin32McpCall, which sees the live provider
// manifest and risk classification. Returns empty on success, else an error string.
QString validateWin32GuiSteps(const QJsonArray& steps) {
    if (steps.isEmpty()) {
        return QStringLiteral(
            "win32_gui action requires a non-empty 'steps' array in the manifest");
    }
    for (int i = 0; i < steps.size(); ++i) {
        const QString step_error = validateWin32GuiStep(steps.at(i), i);
        if (!step_error.isEmpty()) {
            return step_error;
        }
    }
    return {};
}

// Finalize a win32_gui (GUI recipe) plan: validate the manifest steps and preview them. A GUI
// recipe injects mouse/keyboard input, so it is always at least input-tier risky;
// authorizeAppAction takes the single action-level consent (confirm in Assisted, restore point in
// Unattended). Individual steps are vetted manifest content, not model input, and high-risk desktop
// tools are rejected by the step runner. Sets error_message on failure.
void applyWin32GuiMethod(AiAppActionPlan& plan) {
    plan.steps = plan.action_profile.value(QStringLiteral("steps")).toArray();
    const QString steps_error = validateWin32GuiSteps(plan.steps);
    if (!steps_error.isEmpty()) {
        plan.error_message = steps_error;
        return;
    }
    plan.preview = QStringLiteral("Drive %1 GUI action '%2' (%3 desktop-control steps)")
                       .arg(plan.display_name, plan.action)
                       .arg(plan.steps.size());
    plan.risky = true;
}

// Finalize a powershell/cli plan: require a supported method + command, preview it, and run it
// through the command guard (blocking / approval / destructive classification). Sets error_message
// on failure.
void applyCommandMethod(AiAppActionPlan& plan) {
    if (plan.method != QLatin1String("powershell") && plan.method != QLatin1String("cli")) {
        plan.error_message = QStringLiteral(
            "app_run_action supports powershell/cli/win32_gui manifest actions only");
        return;
    }
    if (plan.request.command.isEmpty()) {
        plan.error_message = QStringLiteral("Supported app action has no command in manifest");
        return;
    }
    plan.preview = QStringLiteral("Run %1 action '%2': %3")
                       .arg(plan.display_name, plan.action, plan.request.command);
    plan.guard_block_error = commandGuardBlockError(plan.request, plan.preview);
    if (!plan.guard_block_error.isEmpty()) {
        plan.error_message = plan.guard_block_error;
        return;
    }
    plan.guard_approval_reason = commandGuardApprovalReason(plan.request, plan.preview);
    plan.risky = safetyFlag(plan.action_profile, QStringLiteral("high_risk")) ||
                 safetyFlag(plan.action_profile, QStringLiteral("requires_restore_point")) ||
                 AiCommandToolPlanner::isPotentiallyDestructiveCommand(plan.request, plan.preview);
    // A catastrophic or obfuscated manifest command must not be downgradable to a restore-point
    // offer in Unattended (the gap the provider-gateway app_run path had vs shell/own-action).
    plan.catastrophic = commandLooksCatastrophic(plan.request.command) ||
                        commandLooksObfuscated(plan.request.command);
}
}  // namespace

AiAppActionPlan AiAppActionPlanner::buildPlan(const QString& app_id,
                                              const QString& action,
                                              const QJsonObject& manifest,
                                              const QJsonObject& arguments,
                                              Options options) {
    AiAppActionPlan plan;
    plan.manifest = manifest;
    plan.app_id = app_id.trimmed();
    plan.action = action.trimmed();
    plan.display_name = manifest.value(QStringLiteral("display_name")).toString(plan.app_id);
    plan.action_profile = manifest.value(QStringLiteral("requested_action_profile")).toObject();

    if (plan.app_id.isEmpty() || plan.action.isEmpty()) {
        plan.error_message = QStringLiteral("app_run_action requires app_id and action");
        return plan;
    }
    if (!manifest.value(QStringLiteral("requested_action_supported")).toBool(false)) {
        plan.error_message = plan.action_profile.value(QStringLiteral("reason"))
                                 .toString(QStringLiteral(
                                     "Requested app action is not supported by bundled manifest"));
        return plan;
    }

    plan.method = plan.action_profile.value(QStringLiteral("method")).toString();
    plan.request.command =
        plan.action_profile.value(QStringLiteral("command")).toString().trimmed();
    plan.request.requires_admin = safetyFlag(plan.action_profile, QStringLiteral("requires_admin"));
    plan.request.timeout_seconds =
        std::clamp(arguments.value(QStringLiteral("timeout_seconds"))
                       .toInt(plan.action_profile.value(QStringLiteral("timeout_seconds"))
                                  .toInt(kAppActionDefaultTimeoutSeconds)),
                   kAppActionMinTimeoutSeconds,
                   kAppActionMaxTimeoutSeconds);
    plan.request.max_output_bytes = std::clamp(
        arguments.value(QStringLiteral("max_output_bytes")).toInt(options.default_output_bytes),
        options.min_output_bytes,
        options.max_output_bytes);
    plan.evidence = plan.action_profile.value(QStringLiteral("evidence")).toArray();

    if (plan.method == QLatin1String("win32_gui")) {
        applyWin32GuiMethod(plan);
    } else {
        applyCommandMethod(plan);
    }
    return plan;
}

}  // namespace sak::ai

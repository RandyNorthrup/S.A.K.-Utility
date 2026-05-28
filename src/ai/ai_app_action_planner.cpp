// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_app_action_planner.h"

#include "sak/ai/ai_command_guard.h"
#include "sak/ai/ai_command_tool_planner.h"

#include <QLatin1String>

#include <algorithm>

namespace sak::ai {

namespace {
constexpr int kAppActionDefaultTimeoutSeconds = 1800;
constexpr int kAppActionMinTimeoutSeconds = 5;
constexpr int kAppActionMaxTimeoutSeconds = 14'400;
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
    plan.request.requires_admin =
        plan.action_profile.value(QStringLiteral("requires_admin")).toBool(false);
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

    if (plan.method != QLatin1String("powershell") && plan.method != QLatin1String("cli")) {
        plan.error_message =
            QStringLiteral("app_run_action supports powershell/cli manifest actions only");
        return plan;
    }
    if (plan.request.command.isEmpty()) {
        plan.error_message = QStringLiteral("Supported app action has no command in manifest");
        return plan;
    }

    plan.preview = QStringLiteral("Run %1 action '%2': %3")
                       .arg(plan.display_name, plan.action, plan.request.command);
    plan.guard_block_error = commandGuardBlockError(plan.request, plan.preview);
    if (!plan.guard_block_error.isEmpty()) {
        plan.error_message = plan.guard_block_error;
        return plan;
    }
    plan.guard_approval_reason = commandGuardApprovalReason(plan.request, plan.preview);
    plan.risky =
        plan.action_profile.value(QStringLiteral("high_risk")).toBool(false) ||
        plan.action_profile.value(QStringLiteral("requires_restore_point")).toBool(false) ||
        AiCommandToolPlanner::isPotentiallyDestructiveCommand(plan.request, plan.preview);
    return plan;
}

}  // namespace sak::ai

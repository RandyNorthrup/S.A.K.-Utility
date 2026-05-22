// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_command_tool_planner.h"

#include "sak/ai/ai_command_guard.h"

#include <QLatin1Char>
#include <QLatin1String>

namespace sak::ai {

AiCommandToolPlan AiCommandToolPlanner::buildPlan(const QString& tool_name,
                                                  const QJsonObject& args,
                                                  AiToolPolicy policy,
                                                  Options options) {
    AiCommandToolPlan plan;
    if (tool_name == QLatin1String("run_powershell")) {
        plan.request = ExecutionBroker::requestFromJson(args);
        plan.shell_label = QStringLiteral("PowerShell");
        plan.preview = plan.request.command;
    } else if (tool_name == QLatin1String("run_cmd")) {
        plan.request = ExecutionBroker::requestFromJson(args);
        plan.shell_label = QStringLiteral("cmd.exe");
        plan.preview = plan.request.command;
    } else {
        plan.request = ExecutionBroker::processRequestFromJson(args);
        plan.shell_label = QStringLiteral("Process");
        plan.preview = QStringLiteral("%1 %2").arg(plan.request.program,
                                                   plan.request.arguments.join(QLatin1Char(' ')));
    }

    plan.request.max_output_bytes = options.max_output_bytes;
    plan.risky_change = isPotentiallyDestructiveCommand(plan.request, plan.preview);

    const AiCommandGuardResult guard = evaluateCommandGuard(plan.request, plan.preview);
    plan.guard_block_error = guard.block_error;
    plan.guard_approval_reason = guard.approval_reason;

    plan.policy_request.tool_name = tool_name;
    plan.policy_request.command_preview = plan.preview;
    plan.policy_request.requires_admin = plan.request.requires_admin;
    plan.policy_decision = evaluateToolPolicy(policy, plan.policy_request);
    return plan;
}

bool AiCommandToolPlanner::isPotentiallyDestructiveCommand(const AiCommandRequest& request,
                                                           const QString& preview) {
    const QString command =
        QStringLiteral("%1 %2 %3").arg(request.command, request.program, preview).toLower();
    return request.requires_admin || commandLooksRiskyChange(command);
}

}  // namespace sak::ai

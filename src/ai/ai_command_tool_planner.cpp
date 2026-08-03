// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_command_tool_planner.h"

#include "sak/ai/ai_command_guard.h"

#include <QLatin1Char>
#include <QLatin1String>
#include <QStringList>

#include <algorithm>

namespace sak::ai {

namespace {

bool isControlChar(QChar ch) {
    return ch.unicode() < 0x20 || ch.unicode() == 0x7f;
}

// Renders a control character as a visible escape so a newline/tab embedded in an
// argument cannot forge the displayed confirmation command.
QString escapeControlChar(QChar ch) {
    if (ch == QLatin1Char('\t')) {
        return QStringLiteral("\\t");
    }
    if (ch == QLatin1Char('\n')) {
        return QStringLiteral("\\n");
    }
    if (ch == QLatin1Char('\r')) {
        return QStringLiteral("\\r");
    }
    return QStringLiteral("\\x%1").arg(static_cast<uint>(ch.unicode()), 2, 16, QLatin1Char('0'));
}

// Makes control chars visible and escapes embedded double quotes (the preview's
// own delimiter) so the rendered command is unambiguous.
QString sanitizeForPreview(const QString& value) {
    QString out;
    out.reserve(value.size());
    for (const QChar ch : value) {
        if (isControlChar(ch)) {
            out += escapeControlChar(ch);
        } else if (ch == QLatin1Char('"')) {
            out += QStringLiteral("\\\"");
        } else {
            out += ch;
        }
    }
    return out;
}

bool previewNeedsQuoting(const QString& value) {
    if (value.isEmpty()) {
        return true;
    }
    return std::any_of(value.cbegin(), value.cend(), [](QChar ch) {
        return ch.isSpace() || isControlChar(ch) || ch == QLatin1Char('"');
    });
}

// One argv element rendered so its boundaries match QProcess argument boundaries:
// quoted (with contents escaped/visible) whenever it holds whitespace, a quote, or
// a control char, so it can never silently merge into or split from a neighbor.
QString quoteArgForPreview(const QString& value) {
    const QString sanitized = sanitizeForPreview(value);
    if (previewNeedsQuoting(value)) {
        return QLatin1Char('"') + sanitized + QLatin1Char('"');
    }
    return sanitized;
}

QString buildProcessPreview(const QString& program, const QStringList& arguments) {
    QStringList parts;
    parts.reserve(arguments.size() + 1);
    parts << quoteArgForPreview(program);
    for (const QString& arg : arguments) {
        parts << quoteArgForPreview(arg);
    }
    return parts.join(QLatin1Char(' '));
}

}  // namespace

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
        plan.preview = buildProcessPreview(plan.request.program, plan.request.arguments);
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

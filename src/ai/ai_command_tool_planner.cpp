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

// Cap on how much of a rejected tool name is echoed back in the refusal message.
constexpr int kToolNameErrorMaxChars = 64;

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
    // The command tools are a CLOSED set of EXACT names. The router (and every policy
    // helper) matches tool names case- and whitespace-insensitively, so a variant such as
    // "RUN_POWERSHELL" is authorized as a shell tool; letting it fall through to the
    // process branch here would parse and launch a model-supplied executable instead of
    // the shell that was authorized. Anything that is not one of the three canonical
    // names is refused outright.
    bool direct_process = false;
    if (tool_name == QLatin1String("run_powershell")) {
        plan.request = ExecutionBroker::requestFromJson(args);
        plan.shell_label = QStringLiteral("PowerShell");
        plan.preview = plan.request.command;
    } else if (tool_name == QLatin1String("run_cmd")) {
        plan.request = ExecutionBroker::requestFromJson(args);
        plan.shell_label = QStringLiteral("cmd.exe");
        plan.preview = plan.request.command;
    } else if (tool_name == QLatin1String("run_process")) {
        direct_process = true;
        plan.request = ExecutionBroker::processRequestFromJson(args);
        plan.shell_label = QStringLiteral("Process");
        plan.preview = buildProcessPreview(plan.request.program, plan.request.arguments);
    } else {
        plan.shell_label = QStringLiteral("Process");
        plan.risky_change = true;
        plan.request.validation_error =
            QStringLiteral("Unsupported command tool: %1")
                .arg(sanitizeForPreview(tool_name.left(kToolNameErrorMaxChars)));
        plan.guard_block_error = plan.request.validation_error;
        plan.policy_request.tool_name = tool_name;
        return plan;  // policy_decision stays default-denied.
    }

    plan.request.max_output_bytes = options.max_output_bytes;
    // A direct process launch is an arbitrary executable: its effect lives in the binary,
    // not in any text the risk classifier can read, so a launch can never be PROVEN
    // non-destructive. Fail closed and treat every direct launch as a risky change; the
    // shell branches keep their text classification.
    plan.risky_change = direct_process ||
                        isPotentiallyDestructiveCommand(plan.request, plan.preview);

    // Malformed / wrong-typed / out-of-domain arguments are rejected HERE rather than
    // repaired into defaults and carried through the confirmation, lease, and launch path
    // only to be refused at the broker's entry point.
    if (!plan.request.validation_error.isEmpty()) {
        plan.guard_block_error = plan.request.validation_error;
        plan.policy_request.tool_name = tool_name;
        plan.policy_request.command_preview = plan.preview;
        plan.policy_request.requires_admin = plan.request.requires_admin;
        return plan;  // policy_decision stays default-denied.
    }

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

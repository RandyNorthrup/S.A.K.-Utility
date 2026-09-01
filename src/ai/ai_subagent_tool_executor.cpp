// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_subagent_tool_executor.h"

#include "sak/elevation_manager.h"

#include <QJsonDocument>
#include <QJsonParseError>

#include <utility>

namespace sak::ai {

namespace {

// A tool call's arguments are model output (untrusted). Cap the JSON before parsing so a
// hostile oversize blob cannot drive an unbounded JSON-DOM allocation; real tool arguments
// are a handful of small fields.
constexpr qsizetype kMaxToolArgumentsChars = 1 << 20;
constexpr qsizetype kMaxToolArgumentsBytes = 1 << 20;  // 1 MiB encoded JSON

QString normalizedToolName(const QString& name) {
    return name.trimmed().toLower();
}

AiSubagentToolOutput errorOutput(const QString& call_id,
                                 const QString& tool,
                                 const QString& message) {
    QJsonObject payload;
    payload[QStringLiteral("error")] = message;
    if (!tool.isEmpty()) {
        payload[QStringLiteral("tool")] = tool;
    }
    AiSubagentToolOutput output;
    output.call_id = call_id;
    output.output_json = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    return output;
}

bool parseArguments(const AiSubagentToolCall& call,
                    QJsonObject* arguments,
                    AiSubagentToolOutput* failure) {
    // Bound UTF-16 input before encoding, then enforce the documented cap on the actual UTF-8
    // JSON bytes. Non-ASCII text can occupy several encoded bytes per QString code unit.
    if (call.arguments_json.size() > kMaxToolArgumentsChars) {
        *failure = errorOutput(call.call_id,
                               call.name,
                               QStringLiteral("Tool arguments JSON exceeds the size limit"));
        return false;
    }
    const QByteArray raw_args = call.arguments_json.trimmed().toUtf8();
    if (raw_args.size() > kMaxToolArgumentsBytes) {
        *failure = errorOutput(call.call_id,
                               call.name,
                               QStringLiteral("Tool arguments JSON exceeds the size limit"));
        return false;
    }
    if (raw_args.isEmpty()) {
        return true;
    }
    QJsonParseError parse_error{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw_args, &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        *failure = errorOutput(
            call.call_id,
            call.name,
            QStringLiteral("Invalid tool arguments JSON: %1").arg(parse_error.errorString()));
        return false;
    }
    if (!doc.isObject()) {
        *failure = errorOutput(call.call_id,
                               call.name,
                               QStringLiteral("Tool arguments JSON root must be an object"));
        return false;
    }
    *arguments = doc.object();
    return true;
}

}  // namespace

AiSubagentToolExecutor::AiSubagentToolExecutor(RawDispatch dispatch)
    : m_dispatch(std::move(dispatch)) {}

void AiSubagentToolExecutor::setAllowedTools(QSet<QString> tool_names) {
    m_allowed_tools = std::move(tool_names);
}

AiSubagentToolOutput AiSubagentToolExecutor::executeToolCall(const AiSubagentTask& task,
                                                             const AiSubagentToolCall& call,
                                                             const CancellationToken& token) {
    if (token.isValid() && token.isCancellationRequested()) {
        return errorOutput(call.call_id,
                           call.name,
                           QStringLiteral("Cancelled before tool dispatch"));
    }
    const QString normalized = normalizedToolName(call.name);
    if (normalized.isEmpty() || !m_allowed_tools.contains(normalized)) {
        return errorOutput(
            call.call_id,
            call.name,
            QStringLiteral("Tool '%1' is not permitted for subagents").arg(call.name));
    }
    if (!m_dispatch) {
        return errorOutput(call.call_id,
                           call.name,
                           QStringLiteral("No subagent tool dispatcher configured"));
    }

    // Empty argument strings are a valid "no arguments" call; anything non-empty
    // must be a JSON object or the call fails closed.
    QJsonObject arguments;
    AiSubagentToolOutput parse_failure;
    if (!parseArguments(call, &arguments, &parse_failure)) {
        return parse_failure;
    }

    AiToolCallRequest request;
    request.tool_name = call.name;
    request.operation = arguments.value(QStringLiteral("operation")).toString();
    request.command_preview = arguments.value(QStringLiteral("query")).toString();
    request.user_message = task.user_message;
    // A subagent's tool call runs in THIS process, so it inherits this process's elevation.
    // Without this the policy reads only the model's requires_admin claim and takes no lease
    // and no restore point for a command that has administrator rights (B1-3 / H6).
    request.host_elevated = ElevationManager::isElevated();

    const QJsonObject result =
        m_dispatch(task.tool_policy, request, arguments, task.agent_id, token);
    AiSubagentToolOutput output;
    output.call_id = call.call_id;
    output.output_json = QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
    return output;
}

}  // namespace sak::ai

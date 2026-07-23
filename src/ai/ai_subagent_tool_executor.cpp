// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_subagent_tool_executor.h"

#include <QJsonDocument>
#include <QJsonParseError>

#include <utility>

namespace sak::ai {

namespace {

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
    const QByteArray raw_args = call.arguments_json.trimmed().toUtf8();
    QJsonObject arguments;
    if (!raw_args.isEmpty()) {
        QJsonParseError parse_error{};
        const QJsonDocument doc = QJsonDocument::fromJson(raw_args, &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
            return errorOutput(
                call.call_id,
                call.name,
                QStringLiteral("Invalid tool arguments JSON: %1").arg(parse_error.errorString()));
        }
        arguments = doc.object();
    }

    AiToolCallRequest request;
    request.tool_name = call.name;
    request.operation = arguments.value(QStringLiteral("operation")).toString();
    request.command_preview = arguments.value(QStringLiteral("query")).toString();

    const QJsonObject result =
        m_dispatch(task.tool_policy, request, arguments, task.agent_id, token);
    AiSubagentToolOutput output;
    output.call_id = call.call_id;
    output.output_json = QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
    return output;
}

}  // namespace sak::ai

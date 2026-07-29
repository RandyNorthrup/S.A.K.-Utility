// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_dispatch.h"

#include "sak/ai/ai_mcp_jsonrpc.h"
#include "sak/win32mcp/win32_mcp_tools.h"

#include <QJsonArray>
#include <QLatin1String>

namespace sak::win32mcp {

namespace {

// JSON-RPC 2.0 standard error codes we can surface from a stdio MCP server.
constexpr int kMethodNotFound = -32'601;
constexpr int kInvalidParams = -32'602;

QJsonObject resultResponse(const QJsonValue& id, const QJsonObject& result) {
    return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                       {QStringLiteral("id"), id},
                       {QStringLiteral("result"), result}};
}

QJsonObject errorResponse(const QJsonValue& id, int code, const QString& message) {
    return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                       {QStringLiteral("id"), id},
                       {QStringLiteral("error"),
                        QJsonObject{{QStringLiteral("code"), code},
                                    {QStringLiteral("message"), message}}}};
}

QJsonObject initializeResult() {
    return QJsonObject{
        {QStringLiteral("protocolVersion"), QString::fromLatin1(ai::mcp::kProtocolVersion)},
        {QStringLiteral("capabilities"), QJsonObject{{QStringLiteral("tools"), QJsonObject{}}}},
        {QStringLiteral("serverInfo"),
         QJsonObject{{QStringLiteral("name"), serverName()},
                     {QStringLiteral("version"), serverVersion()}}}};
}

QJsonObject toolCallResult(const ToolResult& result) {
    return QJsonObject{{QStringLiteral("content"),
                        QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                               {QStringLiteral("text"), result.text}}}},
                       {QStringLiteral("isError"), result.is_error}};
}

std::optional<QJsonObject> handleToolsCall(const QJsonValue& id, const QJsonObject& params) {
    const QString name = params.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty()) {
        return errorResponse(id, kInvalidParams, QStringLiteral("tools/call requires params.name"));
    }
    const QJsonObject arguments = params.value(QStringLiteral("arguments")).toObject();
    return resultResponse(id, toolCallResult(invokeTool(name, arguments)));
}

}  // namespace

std::optional<QJsonObject> handleRequest(const QJsonObject& request) {
    const QString method = request.value(QStringLiteral("method")).toString();
    const bool has_id = request.contains(QStringLiteral("id")) &&
                        !request.value(QStringLiteral("id")).isNull();
    const QJsonValue id = request.value(QStringLiteral("id"));

    // Notifications (no id) get no response, per JSON-RPC 2.0. This covers
    // notifications/initialized and any future one-way message.
    if (!has_id) {
        return std::nullopt;
    }

    if (method == QLatin1String("initialize")) {
        return resultResponse(id, initializeResult());
    }
    if (method == QLatin1String("tools/list")) {
        return resultResponse(id, QJsonObject{{QStringLiteral("tools"), toolCatalog()}});
    }
    if (method == QLatin1String("tools/call")) {
        return handleToolsCall(id, request.value(QStringLiteral("params")).toObject());
    }
    if (method == QLatin1String("ping")) {
        return resultResponse(id, QJsonObject{});
    }
    return errorResponse(id, kMethodNotFound, QStringLiteral("Unknown method: %1").arg(method));
}

}  // namespace sak::win32mcp

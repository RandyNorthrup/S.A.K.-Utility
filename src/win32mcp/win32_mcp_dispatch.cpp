// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_dispatch.h"

#include "sak/ai/ai_mcp_jsonrpc.h"
#include "sak/win32mcp/browser_control.h"
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

// The full tool catalog: the built-in win32 tools plus, when browser control is live,
// the browser_* tools the facade owns.
QJsonArray fullToolCatalog(BrowserControl* browser) {
    QJsonArray tools = toolCatalog();
    if (browser != nullptr) {
        const QJsonArray browser_tools = browser->toolCatalog();
        for (const QJsonValue& tool : browser_tools) {
            tools.append(tool);
        }
    }
    return tools;
}

std::optional<QJsonObject> handleToolsCall(const QJsonValue& id,
                                           const QJsonObject& params,
                                           BrowserControl* browser) {
    const QString name = params.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty()) {
        return errorResponse(id, kInvalidParams, QStringLiteral("tools/call requires params.name"));
    }
    const QJsonObject arguments = params.value(QStringLiteral("arguments")).toObject();
    if (browser != nullptr && browser->handles(name)) {
        return resultResponse(id, toolCallResult(browser->invoke(name, arguments)));
    }
    return resultResponse(id, toolCallResult(invokeTool(name, arguments)));
}

}  // namespace

QJsonObject toolCallResult(const ToolResult& result) {
    QJsonArray content;
    if (!result.image_base64.isEmpty()) {
        const QString mime = result.image_mime.isEmpty() ? QStringLiteral("image/png")
                                                         : result.image_mime;
        content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("image")},
                                   {QStringLiteral("data"), result.image_base64},
                                   {QStringLiteral("mimeType"), mime}});
    }
    // Keep a text block whenever there is text, or when there is nothing else, so the
    // content array is never empty (a bare screenshot still carries its summary text).
    if (!result.text.isEmpty() || content.isEmpty()) {
        content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                   {QStringLiteral("text"), result.text}});
    }
    return QJsonObject{{QStringLiteral("content"), content},
                       {QStringLiteral("isError"), result.is_error}};
}

std::optional<QJsonObject> handleRequest(const QJsonObject& request, BrowserControl* browser) {
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
        return resultResponse(id, QJsonObject{{QStringLiteral("tools"), fullToolCatalog(browser)}});
    }
    if (method == QLatin1String("tools/call")) {
        return handleToolsCall(id, request.value(QStringLiteral("params")).toObject(), browser);
    }
    if (method == QLatin1String("ping")) {
        return resultResponse(id, QJsonObject{});
    }
    return errorResponse(id, kMethodNotFound, QStringLiteral("Unknown method: %1").arg(method));
}

}  // namespace sak::win32mcp

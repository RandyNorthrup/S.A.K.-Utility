// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_dispatch.h"

#include "sak/ai/ai_mcp_jsonrpc.h"
#include "sak/win32mcp/browser_control.h"
#include "sak/win32mcp/win32_mcp_tools.h"

#include <QJsonArray>
#include <QLatin1String>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>

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
// the browser_* tools the facade owns. Under a read-only profile only the read-only tools
// are advertised so the model is never shown a tool the profile forbids.
QJsonArray fullToolCatalog(const BrowserControl* browser, const Win32McpServerPolicy& policy) {
    QJsonArray tools = toolCatalog();
    if (browser != nullptr) {
        const QJsonArray browser_tools = browser->toolCatalog();
        for (const QJsonValue& tool : browser_tools) {
            tools.append(tool);
        }
    }
    if (!policy.read_only_profile) {
        return tools;
    }
    QJsonArray read_only_tools;
    for (const QJsonValue& tool : tools) {
        if (win32McpToolIsReadOnly(tool.toObject().value(QStringLiteral("name")).toString())) {
            read_only_tools.append(tool);
        }
    }
    return read_only_tools;
}

// Redact the text blocks of a tools/call result in place when the policy asks for it.
QJsonObject redactToolCallResult(QJsonObject result, const Win32McpServerPolicy& policy) {
    if (!policy.redact_sensitive_output) {
        return result;
    }
    QJsonArray content = result.value(QStringLiteral("content")).toArray();
    for (qsizetype i = 0; i < content.size(); ++i) {
        QJsonObject block = content.at(i).toObject();
        if (block.value(QStringLiteral("type")).toString() == QLatin1String("text")) {
            block[QStringLiteral("text")] =
                redactWin32McpSensitiveText(block.value(QStringLiteral("text")).toString());
            content[i] = block;
        }
    }
    result[QStringLiteral("content")] = content;
    return result;
}

std::optional<QJsonObject> handleToolsCall(const QJsonValue& id,
                                           const QJsonObject& params,
                                           BrowserControl* browser,
                                           const Win32McpServerPolicy& policy) {
    const QString name = params.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty()) {
        return errorResponse(id, kInvalidParams, QStringLiteral("tools/call requires params.name"));
    }
    // Fail closed under a read-only profile: refuse any tool that is not read-only, so a
    // mutating/input/process tool cannot run even if the client's own policy gate is bypassed.
    if (policy.read_only_profile && !win32McpToolIsReadOnly(name)) {
        return errorResponse(id,
                             kInvalidParams,
                             QStringLiteral(
                                 "Tool '%1' is not permitted under the read-only security profile")
                                 .arg(name));
    }
    const QJsonObject arguments = params.value(QStringLiteral("arguments")).toObject();
    if (browser != nullptr && browser->handles(name)) {
        return resultResponse(
            id, redactToolCallResult(toolCallResult(browser->invoke(name, arguments)), policy));
    }
    return resultResponse(
        id, redactToolCallResult(toolCallResult(invokeTool(name, arguments)), policy));
}

}  // namespace

Win32McpServerPolicy Win32McpServerPolicy::fromEnvironment() {
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    Win32McpServerPolicy policy;
    policy.read_only_profile =
        env.value(QStringLiteral("WIN32_MCP_SECURITY_PROFILE")).trimmed().toLower() ==
        QLatin1String("read_only");
    policy.redact_sensitive_output =
        env.value(QStringLiteral("WIN32_MCP_REDACT_SENSITIVE_OUTPUT")).trimmed().toLower() ==
        QLatin1String("true");
    return policy;
}

bool win32McpToolIsReadOnly(const QString& tool_name) {
    // Mirror of AiProviderGateway::isWin32ReadOnlyTool -- the server enforces the read-only
    // profile independently, never trusting the client. KEEP IN SYNC with that list.
    static const QSet<QString> read_only{
        QStringLiteral("assert_text_visible"),
        QStringLiteral("browser_box"),
        QStringLiteral("browser_extension_status"),
        QStringLiteral("browser_focus"),
        QStringLiteral("browser_get_attribute"),
        QStringLiteral("browser_get_value"),
        QStringLiteral("browser_hover"),
        QStringLiteral("browser_read"),
        QStringLiteral("browser_reveal"),
        QStringLiteral("browser_screenshot"),
        QStringLiteral("browser_snapshot"),
        QStringLiteral("browser_tabs"),
        QStringLiteral("browser_wait_for"),
        QStringLiteral("browser_windows"),
        QStringLiteral("capture_monitor"),
        QStringLiteral("capture_screen"),
        QStringLiteral("capture_window"),
        QStringLiteral("compare_screenshots"),
        QStringLiteral("find_text_on_screen"),
        QStringLiteral("get_pixel_color"),
        QStringLiteral("get_window_info"),
        QStringLiteral("get_window_snapshot"),
        QStringLiteral("health_check"),
        QStringLiteral("list_monitors"),
        QStringLiteral("list_processes"),
        QStringLiteral("list_windows"),
        QStringLiteral("mouse_position"),
        QStringLiteral("ocr_region"),
        QStringLiteral("ocr_region_structured"),
        QStringLiteral("ocr_screen"),
        QStringLiteral("ocr_screen_structured"),
        QStringLiteral("ocr_window"),
        QStringLiteral("uia_find_control"),
        QStringLiteral("uia_get_control_value"),
        QStringLiteral("uia_get_focused"),
        QStringLiteral("uia_inspect_window"),
        QStringLiteral("wait_for_idle"),
        QStringLiteral("wait_for_text"),
        QStringLiteral("wait_for_window"),
    };
    return read_only.contains(tool_name.trimmed());
}

QString redactWin32McpSensitiveText(const QString& text) {
    if (text.isEmpty()) {
        return text;
    }
    QString redacted = text;
    // key = value / key: value where key names a secret (password, token, secret, api key,
    // client_secret, ...). Capture the key + separator, replace the value with a marker.
    static const QRegularExpression assignment(
        QStringLiteral(
            R"((?i)\b(pass(word)?|pwd|secret|token|api[_-]?key|access[_-]?key|client[_-]?secret|bearer)\b\s*([:=]\s*|\s+)(\S+))"),
        QRegularExpression::CaseInsensitiveOption);
    redacted.replace(assignment, QStringLiteral("\\1\\3[REDACTED]"));
    return redacted;
}

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

std::optional<QJsonObject> handleRequest(const QJsonObject& request,
                                         BrowserControl* browser,
                                         const Win32McpServerPolicy& policy) {
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
        return resultResponse(
            id, QJsonObject{{QStringLiteral("tools"), fullToolCatalog(browser, policy)}});
    }
    if (method == QLatin1String("tools/call")) {
        return handleToolsCall(
            id, request.value(QStringLiteral("params")).toObject(), browser, policy);
    }
    if (method == QLatin1String("ping")) {
        return resultResponse(id, QJsonObject{});
    }
    return errorResponse(id, kMethodNotFound, QStringLiteral("Unknown method: %1").arg(method));
}

}  // namespace sak::win32mcp

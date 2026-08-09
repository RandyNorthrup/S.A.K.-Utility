// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_dispatch.h"

#include "sak/ai/ai_mcp_jsonrpc.h"
#include "sak/win32mcp/browser_control.h"
#include "sak/win32mcp/win32_mcp_tools.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QLatin1String>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>

#include <optional>

namespace sak::win32mcp {

namespace {

// JSON-RPC 2.0 standard error codes we can surface from a stdio MCP server.
constexpr int kInvalidRequest = -32'600;
constexpr int kMethodNotFound = -32'601;
constexpr int kInvalidParams = -32'602;

// Resolve the server's read-only lockdown from its env token. This profile is the independent
// fail-closed backstop ("never trust the client"), so an UNSET/empty token is the only value
// that keeps the intended full-access default; the exact token "read_only" restricts to
// read-only tools; and ANY OTHER non-empty token is treated as an operator typo (e.g.
// "readonly", "read-only") that MUST fail CLOSED to read-only rather than silently granting the
// full mutating/input/process surface.
bool resolveReadOnlyProfile(const QString& raw) {
    const QString token = raw.trimmed().toLower();
    if (token.isEmpty()) {
        return false;  // unset: the documented, intended full-access default
    }
    if (token == QLatin1String("read_only")) {
        return true;
    }
    qWarning(
        "WIN32_MCP_SECURITY_PROFILE='%s' is not a recognized profile; failing closed to "
        "read_only.",
        qUtf8Printable(raw));
    return true;
}

// Resolve output redaction from its env token, on the same fail-closed rule as the profile above:
// an UNSET/empty token keeps the documented no-redaction default (a server started outside the
// gateway, and every pure unit test); "true"/"false" mean what they say; and ANY OTHER non-empty
// token is an operator typo that MUST fail CLOSED to redaction rather than silently shipping raw
// tool output to the model.
bool resolveRedactSensitiveOutput(const QString& raw) {
    const QString token = raw.trimmed().toLower();
    if (token.isEmpty()) {
        return false;  // unset: the documented, intended no-redaction default
    }
    if (token == QLatin1String("true")) {
        return true;
    }
    if (token == QLatin1String("false")) {
        return false;
    }
    qWarning(
        "WIN32_MCP_REDACT_SENSITIVE_OUTPUT='%s' is not a recognized value; failing closed to "
        "redacting.",
        qUtf8Printable(raw));
    return true;
}

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

// Redact the text blocks of a tools/call result in place when the policy asks for it. Scope is
// deliberately the text blocks and the secret-key regex below: image blocks are opaque binary
// the client renders, and arbitrary value fields (e.g. cookie values) are out of scope by
// design -- this is an opt-in best-effort keyword scrubber, not a completeness guarantee. It
// never claims to have redacted content it does not model, so it does not fail open.
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

// True when a JSON value satisfies a JSON-Schema primitive type. An unconstrained/unknown type
// string is accepted (nothing to reject) rather than guessed.
bool win32McpJsonMatchesType(const QString& type, const QJsonValue& value) {
    if (type == QLatin1String("string")) {
        return value.isString();
    }
    if (type == QLatin1String("boolean")) {
        return value.isBool();
    }
    if (type == QLatin1String("array")) {
        return value.isArray();
    }
    if (type == QLatin1String("object")) {
        return value.isObject();
    }
    if (type == QLatin1String("number")) {
        return value.isDouble();
    }
    if (type == QLatin1String("integer")) {
        return value.isDouble() &&
               static_cast<double>(static_cast<qint64>(value.toDouble())) == value.toDouble();
    }
    return true;  // schema left the type unconstrained: nothing to reject (faithful, not fail-open)
}

QString schemaRequiredError(const QJsonObject& schema, const QJsonObject& args) {
    const QJsonArray required = schema.value(QStringLiteral("required")).toArray();
    for (const QJsonValue& r : required) {
        const QString key = r.toString();
        if (!args.contains(key)) {
            return QStringLiteral("Missing required argument '%1'").arg(key);
        }
    }
    return QString();
}

QString schemaUnknownKeyError(const QJsonObject& schema, const QJsonObject& args) {
    if (schema.value(QStringLiteral("additionalProperties")).toBool(true)) {
        return QString();  // schema permits extra keys
    }
    const QJsonObject props = schema.value(QStringLiteral("properties")).toObject();
    for (auto it = args.constBegin(); it != args.constEnd(); ++it) {
        if (!props.contains(it.key())) {
            return QStringLiteral("Unknown argument '%1'").arg(it.key());
        }
    }
    return QString();
}

QString schemaTypeError(const QJsonObject& schema, const QJsonObject& args) {
    const QJsonObject props = schema.value(QStringLiteral("properties")).toObject();
    for (auto it = props.constBegin(); it != props.constEnd(); ++it) {
        if (!args.contains(it.key())) {
            continue;
        }
        const QString type = it.value().toObject().value(QStringLiteral("type")).toString();
        if (!type.isEmpty() && !win32McpJsonMatchesType(type, args.value(it.key()))) {
            return QStringLiteral("Argument '%1' must be of type %2").arg(it.key(), type);
        }
    }
    return QString();
}

// The advertised inputSchema for a NATIVE (non-browser) tool, or nullopt if the tool is not in
// the native catalog. Browser tools validate their own args downstream, so they are not looked
// up here.
std::optional<QJsonObject> nativeToolInputSchema(const QString& name) {
    for (const QJsonValue& tool : toolCatalog()) {
        const QJsonObject entry = tool.toObject();
        if (entry.value(QStringLiteral("name")).toString() == name) {
            return entry.value(QStringLiteral("inputSchema")).toObject();
        }
    }
    return std::nullopt;
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
    // A present-but-non-object `arguments` must be rejected, not coerced to {}: coercion would
    // let a command run with its destructive defaults (e.g. browser_close_tab closing the active
    // tab) off a malformed envelope. Absent arguments legitimately means "no args".
    const QJsonValue arguments_value = params.value(QStringLiteral("arguments"));
    if (params.contains(QStringLiteral("arguments")) && !arguments_value.isObject()) {
        return errorResponse(id,
                             kInvalidParams,
                             QStringLiteral("tools/call arguments must be an object"));
    }
    const QJsonObject arguments = arguments_value.toObject();
    if (browser != nullptr && browser->handles(name)) {
        return resultResponse(
            id, redactToolCallResult(toolCallResult(browser->invoke(name, arguments)), policy));
    }
    // Independent server-side enforcement of the advertised inputSchema for NATIVE tools (never
    // trust the client): reject a call whose arguments violate the tool's schema -- missing
    // required, an unknown key under additionalProperties:false, or a wrong-typed value -- rather
    // than dispatch it. Only validated when the tool is in the native catalog, so an unknown tool
    // still takes the existing invokeTool error path.
    const std::optional<QJsonObject> schema = nativeToolInputSchema(name);
    if (schema.has_value()) {
        const QString invalid = win32McpValidateArgsAgainstSchema(*schema, arguments);
        if (!invalid.isEmpty()) {
            return errorResponse(id, kInvalidParams, invalid);
        }
    }
    return resultResponse(
        id, redactToolCallResult(toolCallResult(invokeTool(name, arguments)), policy));
}

}  // namespace

QString win32McpValidateArgsAgainstSchema(const QJsonObject& input_schema,
                                          const QJsonObject& args) {
    QString error = schemaRequiredError(input_schema, args);
    if (!error.isEmpty()) {
        return error;
    }
    error = schemaUnknownKeyError(input_schema, args);
    if (!error.isEmpty()) {
        return error;
    }
    return schemaTypeError(input_schema, args);
}

Win32McpServerPolicy Win32McpServerPolicy::fromEnvironment() {
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    Win32McpServerPolicy policy;
    policy.read_only_profile =
        resolveReadOnlyProfile(env.value(QStringLiteral("WIN32_MCP_SECURITY_PROFILE")));
    policy.redact_sensitive_output = resolveRedactSensitiveOutput(
        env.value(QStringLiteral("WIN32_MCP_REDACT_SENSITIVE_OUTPUT")));
    return policy;
}

bool win32McpToolIsReadOnly(const QString& tool_name) {
    // Mirror of AiProviderGateway::isWin32ReadOnlyTool -- the server enforces the read-only
    // profile independently, never trusting the client. KEEP IN SYNC with that list.
    // browser_focus/hover/reveal are deliberately NOT here: they mutate UI state (move focus,
    // dispatch hover, scroll into view), so they are not passive inspectors.
    static const QSet<QString> read_only{
        QStringLiteral("assert_text_visible"),
        QStringLiteral("browser_box"),
        QStringLiteral("browser_extension_status"),
        QStringLiteral("browser_get_attribute"),
        QStringLiteral("browser_get_value"),
        QStringLiteral("browser_read"),
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
    // client_secret, ...). Capture the key + separator, replace the value with a marker. An
    // optional closing quote after the key and an optional opening quote before the value let
    // this also catch quoted-JSON forms like "token":"secret"; the value stops at a quote,
    // whitespace, comma, or closing brace so structural punctuation is not swallowed.
    static const QRegularExpression assignment(
        QStringLiteral(
            R"RX((\b(?:pass(?:word)?|pwd|secret|token|api[_-]?key|access[_-]?key|client[_-]?secret|bearer)\b"?(?:\s*[:=]\s*|\s+)"?)([^"\s,}]+))RX"),
        QRegularExpression::CaseInsensitiveOption);
    redacted.replace(assignment, QStringLiteral("\\1[REDACTED]"));
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

    // Enforce the JSON-RPC 2.0 envelope: a request whose "jsonrpc" is missing or not exactly
    // "2.0" is malformed and rejected (an id-bearing one gets an Invalid Request error; a
    // notification is silently dropped since it cannot be answered) rather than dispatched.
    if (request.value(QStringLiteral("jsonrpc")).toString() != QLatin1String("2.0")) {
        return has_id ? std::optional<QJsonObject>(errorResponse(
                            id, kInvalidRequest, QStringLiteral("jsonrpc must be \"2.0\"")))
                      : std::nullopt;
    }

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

// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_provider_gateway.h"

#include "sak/ai/ai_mcp_http_client.h"
#include "sak/ai/ai_mcp_session_pool.h"
#include "sak/ai/ai_mcp_stdio_client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSet>
#include <QStringList>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace sak::ai {

namespace {

constexpr qsizetype kMcpResultPreviewChars = 49'152;
constexpr qsizetype kMcpInlineResultJsonBytes = 65'536;
constexpr int kWin32McpDefaultTimeoutMs = 20'000;
constexpr int kWin32McpMinimumTimeoutMs = 1000;
// A single win32 call may legitimately block for a long-running desktop operation -- a wait_for_*
// step that polls until an antivirus scan finishes runs for minutes to hours. The default stays
// short (20s); only a step that explicitly requests a long timeout_ms (e.g. a scan recipe) gets
// one. These calls run off the GUI thread, so a long block never freezes the app.
constexpr int kWin32McpMaximumTimeoutMs = 7'200'000;  // 2 hours

QString cappedString(const QString& value, qsizetype max_chars, bool* truncated = nullptr) {
    if (value.size() <= max_chars) {
        if (truncated) {
            *truncated = false;
        }
        return value;
    }
    if (truncated) {
        *truncated = true;
    }
    return value.left(max_chars) + QStringLiteral("\n...[truncated]");
}

QString compactJsonValue(const QJsonValue& value) {
    if (value.isObject()) {
        return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    }
    if (value.isArray()) {
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    }
    return QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("value"), value}})
                                 .toJson(QJsonDocument::Compact));
}

QString mcpTextContent(const QJsonValue& result_value) {
    const QJsonObject result = result_value.toObject();
    const QJsonArray content = result.value(QStringLiteral("content")).toArray();
    QStringList texts;
    for (const auto& item_value : content) {
        const QJsonObject item = item_value.toObject();
        if (item.value(QStringLiteral("type")).toString() == QLatin1String("text")) {
            const QString text = item.value(QStringLiteral("text")).toString().trimmed();
            if (!text.isEmpty()) {
                texts.append(text);
            }
        }
    }
    return texts.join(QStringLiteral("\n\n"));
}

QJsonObject mcpDocsQueryResult(const QJsonObject& provider,
                               const QString& provider_tool,
                               const QString& query,
                               const QJsonObject& tool_arguments,
                               const QJsonObject& mcp_message) {
    const QJsonValue result_value = mcp_message.value(QStringLiteral("result"));
    const QString compact_result = compactJsonValue(result_value);
    bool json_truncated = false;
    bool text_truncated = false;
    const QString text = mcpTextContent(result_value);

    QJsonObject result;
    result[QStringLiteral("provider_id")] = provider.value(QStringLiteral("id")).toString();
    result[QStringLiteral("provider_tool")] = provider_tool;
    result[QStringLiteral("query")] = query;
    result[QStringLiteral("mcp_request_arguments")] = tool_arguments;
    // Surface the MCP tools/call LOGICAL failure flag (result.isError) so docsQuery can fail
    // closed instead of reporting a documentation-provider error as a successful lookup.
    result[QStringLiteral("mcp_is_error")] =
        result_value.toObject().value(QStringLiteral("isError")).toBool(false);
    result[QStringLiteral("mcp_result_preview_json")] =
        cappedString(compact_result, kMcpResultPreviewChars, &json_truncated);
    result[QStringLiteral("mcp_result_truncated")] = json_truncated;
    if (compact_result.toUtf8().size() <= kMcpInlineResultJsonBytes) {
        result[QStringLiteral("mcp_result")] = result_value;
    }
    if (!text.isEmpty()) {
        result[QStringLiteral("result_text")] =
            cappedString(text, kMcpResultPreviewChars, &text_truncated);
        result[QStringLiteral("result_text_truncated")] = text_truncated;
    }
    return result;
}

// Non-empty when a docs result carries a logical MCP tool error (result.isError); the
// message embeds any surfaced result_text so the caller can fail closed with context.
QString docsQueryLogicalError(const QJsonObject& docs_result) {
    if (!docs_result.value(QStringLiteral("mcp_is_error")).toBool(false)) {
        return {};
    }
    const QString text = docs_result.value(QStringLiteral("result_text")).toString();
    return text.isEmpty() ? QStringLiteral("docs_query MCP tool reported an error")
                          : QStringLiteral("docs_query MCP tool error: %1").arg(text);
}

bool providerHasTool(const QJsonObject& provider, const QString& tool_name) {
    for (const auto& value : provider.value(QStringLiteral("tools")).toArray()) {
        if (value.toString() == tool_name) {
            return true;
        }
    }
    return false;
}

QJsonObject availabilityOk(const QString& operation) {
    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("operation")] = operation;
    return result;
}

QJsonObject availabilityError(const QString& operation,
                              const QString& failure_class,
                              const QString& message) {
    QJsonObject result;
    result[QStringLiteral("success")] = false;
    result[QStringLiteral("operation")] = operation;
    result[QStringLiteral("failure_class")] = failure_class;
    result[QStringLiteral("error_message")] = message;
    return result;
}

struct DocsToolPlan {
    QString provider_tool;
    QJsonObject tool_arguments;
    QString error;

    [[nodiscard]] bool ok() const { return error.isEmpty() && !provider_tool.isEmpty(); }
};

QString firstNonEmpty(const QJsonObject& object, const QString& primary, const QString& fallback) {
    const QString primary_value = object.value(primary).toString().trimmed();
    return primary_value.isEmpty() ? object.value(fallback).toString().trimmed() : primary_value;
}

DocsToolPlan context7DocsPlan(const QString& query, const QJsonObject& extra) {
    const QString library_id =
        firstNonEmpty(extra, QStringLiteral("libraryId"), QStringLiteral("library_id"));
    const QString library_name =
        firstNonEmpty(extra, QStringLiteral("libraryName"), QStringLiteral("library_name"));
    if (!library_id.isEmpty() && query.isEmpty()) {
        return {.error = QStringLiteral("Context7 query-docs requires query")};
    }
    if (!library_id.isEmpty()) {
        return {.provider_tool = QStringLiteral("query-docs"),
                .tool_arguments = QJsonObject{{QStringLiteral("libraryId"), library_id},
                                              {QStringLiteral("query"), query}}};
    }

    const QString resolved_name = library_name.isEmpty() ? query : library_name;
    if (resolved_name.isEmpty()) {
        return {
            .error = QStringLiteral("Context7 resolve-library-id requires libraryName or query")};
    }
    return {.provider_tool = QStringLiteral("resolve-library-id"),
            .tool_arguments =
                QJsonObject{{QStringLiteral("libraryName"), resolved_name},
                            {QStringLiteral("query"), query.isEmpty() ? resolved_name : query}}};
}

DocsToolPlan microsoftCodeSamplePlan(const QString& query,
                                     const QString& language,
                                     const QString& requested_tool) {
    if (requested_tool != QLatin1String("microsoft_code_sample_search") &&
        requested_tool != QLatin1String("code_sample_search")) {
        return {};
    }
    if (query.isEmpty()) {
        return {.error = QStringLiteral("Microsoft code sample search requires query")};
    }
    QJsonObject arguments{{QStringLiteral("query"), query}};
    if (!language.isEmpty()) {
        arguments[QStringLiteral("language")] = language;
    }
    return {.provider_tool = QStringLiteral("microsoft_code_sample_search"),
            .tool_arguments = arguments};
}

DocsToolPlan microsoftDocsPlan(const QString& query, const QJsonObject& extra) {
    const QString requested_tool = extra.value(QStringLiteral("tool")).toString().trimmed();
    const QString url = extra.value(QStringLiteral("url")).toString().trimmed();
    const QString language = extra.value(QStringLiteral("language")).toString().trimmed();
    if (!url.isEmpty() || query.startsWith(QLatin1String("http"), Qt::CaseInsensitive)) {
        return {.provider_tool = QStringLiteral("microsoft_docs_fetch"),
                .tool_arguments =
                    QJsonObject{{QStringLiteral("url"), url.isEmpty() ? query : url}}};
    }

    DocsToolPlan code_sample = microsoftCodeSamplePlan(query, language, requested_tool);
    if (code_sample.ok() || !code_sample.error.isEmpty()) {
        return code_sample;
    }
    if (query.isEmpty()) {
        return {.error = QStringLiteral("Microsoft docs search requires query")};
    }
    return {.provider_tool = QStringLiteral("microsoft_docs_search"),
            .tool_arguments = QJsonObject{{QStringLiteral("query"), query}}};
}

DocsToolPlan docsToolPlan(const QString& provider_id,
                          const QString& query,
                          const QJsonObject& extra) {
    if (provider_id == QLatin1String("context7")) {
        return context7DocsPlan(query, extra);
    }
    if (provider_id == QLatin1String("microsoft_docs")) {
        return microsoftDocsPlan(query, extra);
    }
    return {.error = QStringLiteral("docs_query supports context7 and microsoft_docs")};
}

QJsonObject loadHttpDocsProvider(const AiProviderRegistry& registry,
                                 const QString& provider_id,
                                 QString* error_message) {
    QJsonObject provider = registry.providerStatus(provider_id, error_message);
    if (provider.isEmpty()) {
        return {};
    }
    if (!provider.value(QStringLiteral("available")).toBool(false)) {
        if (error_message) {
            const QString reason = provider.value(QStringLiteral("missing_reason")).toString();
            *error_message = reason.isEmpty() ? QStringLiteral("Provider unavailable") : reason;
        }
        return {};
    }
    if (provider.value(QStringLiteral("transport")).toString() == QLatin1String("http")) {
        return provider;
    }
    if (error_message) {
        *error_message = QStringLiteral("docs_query supports HTTP MCP docs providers only");
    }
    return {};
}

bool requireProviderTool(const QJsonObject& provider,
                         const QString& provider_id,
                         const QString& provider_tool,
                         QString* error_message) {
    if (providerHasTool(provider, provider_tool)) {
        return true;
    }
    if (error_message) {
        *error_message =
            QStringLiteral("MCP provider tool is not in bundled provider manifest: %1/%2")
                .arg(provider_id, provider_tool);
    }
    return false;
}

bool isAppOperation(const QString& operation) {
    static const QSet<QString> operations{QStringLiteral("app_manifest"),
                                          QStringLiteral("app_capabilities"),
                                          QStringLiteral("app_run_action_plan"),
                                          QStringLiteral("app_run_action")};
    return operations.contains(operation);
}

QJsonObject providerRegistryAvailability(const AiProviderRegistry& registry,
                                         const QString& operation,
                                         QString* error_message) {
    QString error;
    const QJsonObject providers = registry.providersObject(&error);
    if (!error.isEmpty() || providers.isEmpty()) {
        if (error_message) {
            *error_message = error;
        }
        return availabilityError(operation, QStringLiteral("provider_registry_unavailable"), error);
    }
    if (error_message) {
        error_message->clear();
    }
    return availabilityOk(operation);
}

QJsonObject docsQueryAvailability(const AiProviderRegistry& registry,
                                  const QJsonObject& args,
                                  const QString& operation) {
    QString error;
    const QString provider_id =
        args.value(QStringLiteral("provider_id")).toString().trimmed().toLower();
    if (provider_id.isEmpty()) {
        return availabilityError(operation,
                                 QStringLiteral("invalid_request"),
                                 QStringLiteral("docs_query requires provider_id"));
    }
    const QJsonObject provider = registry.providerStatus(provider_id, &error);
    if (!error.isEmpty() || provider.isEmpty()) {
        return availabilityError(operation, QStringLiteral("provider_unknown"), error);
    }
    if (!provider.value(QStringLiteral("available")).toBool(false)) {
        const QString reason = provider.value(QStringLiteral("missing_reason"))
                                   .toString(QStringLiteral("Provider unavailable"));
        return availabilityError(operation, QStringLiteral("provider_unavailable"), reason);
    }
    if (provider.value(QStringLiteral("transport")).toString() != QLatin1String("http")) {
        return availabilityError(operation,
                                 QStringLiteral("unsupported_transport"),
                                 QStringLiteral("docs_query requires HTTP provider"));
    }
    QJsonObject ok = availabilityOk(operation);
    ok[QStringLiteral("provider_id")] = provider_id;
    return ok;
}

QJsonObject appOperationAvailability(const AiProviderRegistry& registry,
                                     const QJsonObject& args,
                                     const QString& operation) {
    QString error;
    const QString app_id = args.value(QStringLiteral("app_id")).toString().trimmed();
    const QString action = args.value(QStringLiteral("action")).toString().trimmed();
    if (app_id.isEmpty()) {
        return availabilityError(operation,
                                 QStringLiteral("invalid_request"),
                                 QStringLiteral("Provider app operation requires app_id"));
    }
    const QJsonObject manifest = operation == QLatin1String("app_manifest")
                                     ? registry.appManifest(app_id, &error)
                                     : registry.appCapabilities(app_id, action, &error);
    if (!error.isEmpty() || manifest.isEmpty()) {
        return availabilityError(operation, QStringLiteral("app_manifest_unavailable"), error);
    }
    if (operation != QLatin1String("app_manifest") &&
        !manifest.value(QStringLiteral("requested_action_supported")).toBool(false)) {
        return availabilityError(operation,
                                 QStringLiteral("app_action_unsupported"),
                                 QStringLiteral("App action is not supported by bundled manifest"));
    }

    QJsonObject ok = availabilityOk(operation);
    ok[QStringLiteral("app_id")] = app_id;
    if (!action.isEmpty()) {
        ok[QStringLiteral("action")] = action;
    }
    return ok;
}

QString win32ToolName(const QJsonObject& extra) {
    const QString tool_name = extra.value(QStringLiteral("tool_name")).toString().trimmed();
    return tool_name.isEmpty() ? extra.value(QStringLiteral("tool")).toString().trimmed()
                               : tool_name;
}

QJsonObject win32ToolArguments(QJsonObject extra) {
    QJsonObject tool_arguments = extra.value(QStringLiteral("tool_arguments")).toObject();
    if (!tool_arguments.isEmpty()) {
        return tool_arguments;
    }
    // By design (certified in test_ai_provider_gateway): when no explicit tool_arguments
    // object is supplied, the model may pass the tool's arguments inline as siblings of
    // tool_name. The four control keys below are the ONLY reserved fields; they are removed
    // so they can never be smuggled in as tool arguments, and whatever remains is passed
    // through. The resulting tool + arguments are still validated against the bundled
    // manifest and risk-gated, so this convenience does not widen the trust boundary.
    extra.remove(QStringLiteral("tool"));
    extra.remove(QStringLiteral("tool_name"));
    extra.remove(QStringLiteral("tool_arguments"));
    extra.remove(QStringLiteral("timeout_ms"));
    return extra;
}

bool requireAvailableWin32Provider(const QJsonObject& provider, QString* error_message) {
    if (provider.value(QStringLiteral("available")).toBool(false)) {
        return true;
    }
    if (error_message) {
        *error_message = provider.value(QStringLiteral("missing_reason"))
                             .toString(QStringLiteral("Win32 MCP provider unavailable"));
    }
    return false;
}

bool requireWin32Tool(const QJsonObject& provider,
                      const QString& tool_name,
                      QString* error_message) {
    if (tool_name.isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("win32_mcp_call requires arguments.tool_name");
        }
        return false;
    }
    if (providerHasTool(provider, tool_name)) {
        return true;
    }
    if (error_message) {
        *error_message =
            QStringLiteral("Win32 MCP tool is not in bundled provider manifest: %1").arg(tool_name);
    }
    return false;
}

void populateWin32Plan(AiProviderGateway::Win32McpCallPlan* plan,
                       QJsonObject provider,
                       const QString& tool_name,
                       const QJsonObject& tool_arguments,
                       const QJsonObject& extra) {
    plan->provider = std::move(provider);
    plan->tool_name = tool_name;
    plan->tool_arguments = tool_arguments;
    plan->read_only = AiProviderGateway::isWin32ReadOnlyTool(tool_name);
    plan->high_risk = AiProviderGateway::isWin32HighRiskTool(tool_name);
    // Fail CLOSED: any tool that is neither on the read-only allowlist nor a known
    // high-risk exec tool -- every input-injection tool, and anything added to the
    // manifest later -- requires an explicit human confirmation, instead of dropping into
    // the ungated "interactive" tier where it would auto-run in Unattended. This is the
    // safety net behind isWin32InputTool: a new automation tool cannot silently act as the
    // user just because someone forgot to classify it.
    plan->requires_confirmation = AiProviderGateway::isWin32InputTool(tool_name) ||
                                  !(plan->read_only || plan->high_risk);
    plan->security_profile = plan->read_only   ? QStringLiteral("read_only")
                             : plan->high_risk ? QStringLiteral("unrestricted")
                                               : QStringLiteral("interactive");
    plan->timeout_ms =
        std::clamp(extra.value(QStringLiteral("timeout_ms")).toInt(kWin32McpDefaultTimeoutMs),
                   kWin32McpMinimumTimeoutMs,
                   kWin32McpMaximumTimeoutMs);
    plan->preview =
        QStringLiteral("Win32 MCP %1 %2")
            .arg(tool_name,
                 QString::fromUtf8(QJsonDocument(tool_arguments).toJson(QJsonDocument::Compact)));
}

// Executes a planned docs tool call over HTTP and builds the surfaced result, failing closed
// on a transport error (callTool sets error_message and returns {}) or a logical isError.
QJsonObject runDocsToolCall(const QJsonObject& provider,
                            const DocsToolPlan& plan,
                            const QString& query,
                            QString* error_message) {
    const QUrl endpoint(provider.value(QStringLiteral("endpoint")).toString());
    const QJsonObject message = AiMcpHttpClient::callTool(endpoint,
                                                          plan.provider_tool,
                                                          plan.tool_arguments,
                                                          kDefaultProviderGatewayTimeoutMs,
                                                          error_message);
    if (message.isEmpty()) {
        return {};
    }
    QJsonObject result =
        mcpDocsQueryResult(provider, plan.provider_tool, query, plan.tool_arguments, message);
    const QString logical_error = docsQueryLogicalError(result);
    if (!logical_error.isEmpty()) {
        if (error_message) {
            *error_message = logical_error;
        }
        return {};
    }
    if (error_message) {
        error_message->clear();
    }
    return result;
}

}  // namespace

AiProviderGateway::AiProviderGateway(AiProviderRegistry registry)
    : m_registry(std::move(registry)) {}

QJsonObject AiProviderGateway::providers(QString* error_message) const {
    return m_registry.providersObject(error_message);
}

QJsonObject AiProviderGateway::providerStatuses(QString* error_message) const {
    return m_registry.providerStatuses(error_message);
}

QJsonObject AiProviderGateway::providerStatus(const QString& provider_id,
                                              QString* error_message) const {
    return m_registry.providerStatus(provider_id, error_message);
}

QJsonObject AiProviderGateway::docsQuery(const QJsonObject& args, QString* error_message) const {
    const QString provider_id =
        args.value(QStringLiteral("provider_id")).toString().trimmed().toLower();
    if (provider_id.isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("docs_query requires provider_id");
        }
        return {};
    }

    const QJsonObject provider = loadHttpDocsProvider(m_registry, provider_id, error_message);
    if (provider.isEmpty()) {
        return {};
    }

    const QString query = args.value(QStringLiteral("query")).toString().trimmed();
    const QJsonObject extra = args.value(QStringLiteral("arguments")).toObject();
    const DocsToolPlan plan = docsToolPlan(provider_id, query, extra);
    if (!plan.ok()) {
        if (error_message) {
            *error_message = plan.error;
        }
        return {};
    }
    if (!requireProviderTool(provider, provider_id, plan.provider_tool, error_message)) {
        return {};
    }
    // Fail closed on a logical tool error: runDocsToolCall does not let the read-op path force
    // success=true over a documentation provider that reported isError (a prompt-injection
    // surface otherwise).
    return runDocsToolCall(provider, plan, query, error_message);
}

QJsonObject AiProviderGateway::appManifest(const QString& app_id, QString* error_message) const {
    return m_registry.appManifest(app_id, error_message);
}

QJsonObject AiProviderGateway::appCapabilities(const QString& app_id,
                                               const QString& action,
                                               QString* error_message) const {
    return m_registry.appCapabilities(app_id, action, error_message);
}

QJsonObject AiProviderGateway::checkAvailability(const QJsonObject& args,
                                                 QString* error_message) const {
    const QString operation =
        args.value(QStringLiteral("operation")).toString().trimmed().toLower();
    if (operation.isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("Provider gateway requires operation");
        }
        return availabilityError({},
                                 QStringLiteral("invalid_request"),
                                 QStringLiteral("Provider gateway requires operation"));
    }

    if (operation == QLatin1String("providers") || operation == QLatin1String("provider_status")) {
        return providerRegistryAvailability(m_registry, operation, error_message);
    }

    if (operation == QLatin1String("docs_query")) {
        return docsQueryAvailability(m_registry, args, operation);
    }

    if (operation == QLatin1String("win32_mcp_call")) {
        QString error;
        const Win32McpCallPlan plan = planWin32McpCall(args, &error);
        if (!error.isEmpty() || plan.provider.isEmpty()) {
            return availabilityError(operation, QStringLiteral("win32_mcp_unavailable"), error);
        }
        QJsonObject ok = availabilityOk(operation);
        ok[QStringLiteral("provider_id")] = QStringLiteral("win32_mcp");
        ok[QStringLiteral("provider_tool")] = plan.tool_name;
        ok[QStringLiteral("read_only_tool")] = plan.read_only;
        ok[QStringLiteral("high_risk_tool")] = plan.high_risk;
        return ok;
    }

    if (isAppOperation(operation)) {
        return appOperationAvailability(m_registry, args, operation);
    }

    return availabilityError(
        operation,
        QStringLiteral("unsupported_operation"),
        QStringLiteral("Unsupported provider gateway operation: %1").arg(operation));
}

AiProviderGateway::Win32McpCallPlan AiProviderGateway::planWin32McpCall(
    const QJsonObject& args, QString* error_message) const {
    Win32McpCallPlan plan;
    QJsonObject provider = m_registry.providerStatus(QStringLiteral("win32_mcp"), error_message);
    if (provider.isEmpty()) {
        return plan;
    }
    if (!requireAvailableWin32Provider(provider, error_message)) {
        return plan;
    }

    const QJsonObject extra = args.value(QStringLiteral("arguments")).toObject();
    const QString tool_name = win32ToolName(extra);
    if (!requireWin32Tool(provider, tool_name, error_message)) {
        return plan;
    }

    const QJsonObject tool_arguments = win32ToolArguments(extra);
    populateWin32Plan(&plan, std::move(provider), tool_name, tool_arguments, extra);
    if (error_message) {
        error_message->clear();
    }
    return plan;
}

QJsonObject AiProviderGateway::callWin32Mcp(const Win32McpCallPlan& plan,
                                            QString* error_message) const {
    if (plan.provider.isEmpty() || plan.tool_name.trimmed().isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("Win32 MCP call plan is incomplete");
        }
        return {};
    }

    const AiMcpStdioCallRequest request{
        .command = plan.provider.value(QStringLiteral("resolved_command")).toString(),
        .tool_name = plan.tool_name,
        .arguments = plan.tool_arguments,
        .environment = win32McpEnvironment(plan.security_profile, plan.provider),
        .timeout_ms = plan.timeout_ms};
    // With a pool, the server process is reused across calls (keyed on command +
    // the full launch environment, so security profiles never share a process);
    // without one, fall back to a fresh process per call.
    const QJsonObject message = m_mcp_pool ? m_mcp_pool->callTool(request, error_message)
                                           : AiMcpStdioClient::callTool(request, error_message);
    if (message.isEmpty()) {
        return {};
    }
    if (error_message) {
        error_message->clear();
    }
    return win32McpResult(
        plan.provider, plan.tool_name, plan.tool_arguments, plan.security_profile, message);
}

bool AiProviderGateway::isWin32ReadOnlyTool(const QString& tool_name) {
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

bool AiProviderGateway::isWin32HighRiskTool(const QString& tool_name) {
    // Only tools the win32 server actually implements. list_processes/kill_process/start_process
    // were classified here (and as read-only) but were never in the server manifest, so their
    // classification was dead -- removed.
    static const QSet<QString> high_risk{
        QStringLiteral("close_window"),
    };
    return high_risk.contains(tool_name.trimmed());
}

bool AiProviderGateway::isWin32InputTool(const QString& tool_name) {
    static const QSet<QString> input_tools{
        // Installing/removing the browser-control extension writes user Chrome enterprise
        // policy that force-installs software; gate it at the hard-confirm tier so it needs
        // a human even in Unattended mode, like live input injection.
        QStringLiteral("browser_extension_install"),
        QStringLiteral("browser_extension_uninstall"),
        QStringLiteral("browser_click"),
        QStringLiteral("browser_click_at"),
        QStringLiteral("browser_dialog"),
        QStringLiteral("browser_drag"),
        QStringLiteral("browser_group_tabs"),
        QStringLiteral("browser_js_click"),
        QStringLiteral("browser_media"),
        QStringLiteral("browser_press_key"),
        QStringLiteral("browser_scroll"),
        QStringLiteral("browser_select"),
        QStringLiteral("browser_set_value"),
        QStringLiteral("browser_type"),
        QStringLiteral("browser_ungroup_tabs"),
        // Writing the system clipboard injects content the user may later paste elsewhere.
        QStringLiteral("clipboard_write"),
        // Live desktop input injection: physical mouse/keyboard via SendInput (mouse_click,
        // click_text, type_text, send_keys) and programmatic UIA activation (uia_click_control).
        // All drive the real desktop and must take the same hard gate as browser input.
        QStringLiteral("click_text"),
        QStringLiteral("uia_click_control"),
        // Invokes a pop-up's affirmative button (OK/Continue/...) -- a real state change on the
        // live desktop, so it takes the same hard gate as any other UIA activation.
        QStringLiteral("dismiss_dialog"),
        QStringLiteral("mouse_click"),
        QStringLiteral("type_text"),
        QStringLiteral("send_keys"),
        // Changing which window is foreground steals focus and redirects where subsequent typed
        // input lands, so it takes the same hard gate as the input it sets up.
        QStringLiteral("focus_window"),
    };
    return input_tools.contains(tool_name.trimmed());
}

bool AiProviderGateway::isWin32DesktopInputTool(const QString& tool_name) {
    // ONLY the physical-desktop-driving input tools: mouse/keyboard via SendInput, UIA
    // activation, dialog dismissal, and focus. Deliberately EXCLUDES the browser input tools,
    // clipboard_write, and extension install/uninstall -- those still require a per-call human
    // confirm even inside a win32_gui recipe (a SUPERAntiSpyware-style desktop recipe does not
    // need them). This is a positive allowlist, so a newly added input tool is rejected in
    // recipes until it is explicitly added here.
    static const QSet<QString> desktop_tools{
        QStringLiteral("click_text"),
        QStringLiteral("uia_click_control"),
        QStringLiteral("dismiss_dialog"),
        QStringLiteral("mouse_click"),
        QStringLiteral("type_text"),
        QStringLiteral("send_keys"),
        QStringLiteral("focus_window"),
    };
    return desktop_tools.contains(tool_name.trimmed());
}

QProcessEnvironment AiProviderGateway::win32McpEnvironment(const QString& security_profile,
                                                           const QJsonObject& provider) {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QJsonObject provider_env = provider.value(QStringLiteral("environment")).toObject();
    for (auto it = provider_env.constBegin(); it != provider_env.constEnd(); ++it) {
        env.insert(it.key(), it.value().toString());
    }
    env.insert(QStringLiteral("WIN32_MCP_SECURITY_PROFILE"), security_profile);
    env.insert(QStringLiteral("WIN32_MCP_RESULT_ENVELOPE"), QStringLiteral("true"));
    env.insert(QStringLiteral("WIN32_MCP_REDACT_SENSITIVE_OUTPUT"), QStringLiteral("true"));
    return env;
}

QJsonObject AiProviderGateway::win32McpResult(const QJsonObject& provider,
                                              const QString& tool_name,
                                              const QJsonObject& tool_arguments,
                                              const QString& security_profile,
                                              const QJsonObject& mcp_message) {
    bool json_truncated = false;
    const QJsonValue result_value = mcp_message.value(QStringLiteral("result"));
    const QString compact_result = compactJsonValue(result_value);

    QJsonObject result;
    result[QStringLiteral("provider_id")] = provider.value(QStringLiteral("id")).toString();
    result[QStringLiteral("provider_tool")] = tool_name;
    result[QStringLiteral("mcp_request_arguments")] = tool_arguments;
    result[QStringLiteral("security_profile")] = security_profile;
    // MCP tools/call signals a LOGICAL tool failure via result.isError (no transport
    // error); surface it so callers do not record the call as a success.
    result[QStringLiteral("mcp_is_error")] =
        result_value.toObject().value(QStringLiteral("isError")).toBool(false);
    result[QStringLiteral("read_only_tool")] = isWin32ReadOnlyTool(tool_name);
    result[QStringLiteral("high_risk_tool")] = isWin32HighRiskTool(tool_name);
    result[QStringLiteral("mcp_result_preview_json")] =
        cappedString(compact_result, kMcpResultPreviewChars, &json_truncated);
    result[QStringLiteral("mcp_result_truncated")] = json_truncated;
    if (compact_result.toUtf8().size() <= kMcpInlineResultJsonBytes) {
        result[QStringLiteral("mcp_result")] = result_value;
    }
    const QString text = mcpTextContent(result_value);
    if (!text.isEmpty()) {
        bool text_truncated = false;
        result[QStringLiteral("result_text")] =
            cappedString(text, kMcpResultPreviewChars, &text_truncated);
        result[QStringLiteral("result_text_truncated")] = text_truncated;
    }
    return result;
}

}  // namespace sak::ai

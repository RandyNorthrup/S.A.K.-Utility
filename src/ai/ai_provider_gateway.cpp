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
#include <limits>
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
        if (truncated != nullptr) {
            *truncated = false;
        }
        return value;
    }
    if (truncated != nullptr) {
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

// Non-empty when a tools/call response envelope is not the shape MCP defines. The peer that
// produced it is untrusted (a remote docs endpoint, or the win32 server), so a malformed
// envelope must FAIL CLOSED with the real shape error instead of being coerced into an empty
// text block with isError=false -- which would record a remote failure as a success.
QString mcpContentError(const QJsonArray& content) {
    for (const auto& item_value : content) {
        if (!item_value.isObject()) {
            return QStringLiteral("result.content entry is not an object");
        }
        const QJsonObject item = item_value.toObject();
        const QJsonValue type = item.value(QStringLiteral("type"));
        if (!type.isString()) {
            return QStringLiteral("result.content entry carries no type string");
        }
        if (type.toString() == QLatin1String("text") &&
            !item.value(QStringLiteral("text")).isString()) {
            return QStringLiteral("result.content text block carries no text string");
        }
    }
    return {};
}

QString mcpEnvelopeError(const QJsonValue& result_value) {
    if (!result_value.isObject()) {
        return QStringLiteral("response carries no result object");
    }
    const QJsonObject result = result_value.toObject();
    const QJsonValue is_error = result.value(QStringLiteral("isError"));
    if (!is_error.isUndefined() && !is_error.isBool()) {
        return QStringLiteral("result.isError is not a boolean");
    }
    const QJsonValue content = result.value(QStringLiteral("content"));
    if (content.isUndefined()) {
        return {};
    }
    if (!content.isArray()) {
        return QStringLiteral("result.content is not an array");
    }
    return mcpContentError(content.toArray());
}

// True only for an absolute http(s) URL that names a host. A docs FETCH target must be a real
// URL, never a search phrase that merely begins with "http".
bool isAbsoluteHttpUrl(const QString& value, bool require_tls) {
    const QUrl url(value, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty()) {
        return false;
    }
    const QString scheme = url.scheme().toLower();
    return scheme == QLatin1String("https") || (!require_tls && scheme == QLatin1String("http"));
}

// Non-empty when one of @p keys is present with a non-string value. Qt would turn such a value
// into an empty string and the caller would then report it as ABSENT, so reject it explicitly.
QString stringFieldTypeError(const QJsonObject& object,
                             const QStringList& keys,
                             const QString& field_prefix) {
    for (const QString& key : keys) {
        const QJsonValue value = object.value(key);
        if (!value.isUndefined() && !value.isString()) {
            return QStringLiteral("%1.%2 must be a string").arg(field_prefix, key);
        }
    }
    return {};
}

// True only for a JSON number that is an exact int. QJsonValue::toInt() silently returns its
// default for a string, a fractional value, or a value outside int range.
bool isIntegralJsonNumber(const QJsonValue& value) {
    if (!value.isDouble()) {
        return false;
    }
    const double number = value.toDouble();
    return number >= static_cast<double>(std::numeric_limits<int>::min()) &&
           number <= static_cast<double>(std::numeric_limits<int>::max()) &&
           static_cast<double>(static_cast<int>(number)) == number;
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
    const QJsonArray tools = provider.value(QStringLiteral("tools")).toArray();
    return std::ranges::any_of(tools, [&tool_name](const auto& value) {
        return value.toString() == tool_name;
    });
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
    QString m_provider_tool;
    QJsonObject m_tool_arguments;
    QString m_error;

    [[nodiscard]] bool ok() const { return m_error.isEmpty() && !m_provider_tool.isEmpty(); }
};

QString firstNonEmpty(const QJsonObject& object, const QString& primary, const QString& fallback) {
    const QString primary_value = object.value(primary).toString().trimmed();
    return primary_value.isEmpty() ? object.value(fallback).toString().trimmed() : primary_value;
}

DocsToolPlan context7DocsPlan(const QString& query, const QJsonObject& extra) {
    const QString type_error = stringFieldTypeError(extra,
                                                    {QStringLiteral("libraryId"),
                                                     QStringLiteral("library_id"),
                                                     QStringLiteral("libraryName"),
                                                     QStringLiteral("library_name")},
                                                    QStringLiteral("docs_query arguments"));
    if (!type_error.isEmpty()) {
        return {.m_error = type_error};
    }
    const QString library_id =
        firstNonEmpty(extra, QStringLiteral("libraryId"), QStringLiteral("library_id"));
    const QString library_name =
        firstNonEmpty(extra, QStringLiteral("libraryName"), QStringLiteral("library_name"));
    if (!library_id.isEmpty() && query.isEmpty()) {
        return {.m_error = QStringLiteral("Context7 query-docs requires query")};
    }
    if (!library_id.isEmpty()) {
        return {.m_provider_tool = QStringLiteral("query-docs"),
                .m_tool_arguments = QJsonObject{{QStringLiteral("libraryId"), library_id},
                                                {QStringLiteral("query"), query}}};
    }

    const QString resolved_name = library_name.isEmpty() ? query : library_name;
    if (resolved_name.isEmpty()) {
        return {
            .m_error = QStringLiteral("Context7 resolve-library-id requires libraryName or query")};
    }
    return {.m_provider_tool = QStringLiteral("resolve-library-id"),
            .m_tool_arguments =
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
        return {.m_error = QStringLiteral("Microsoft code sample search requires query")};
    }
    QJsonObject arguments{{QStringLiteral("query"), query}};
    if (!language.isEmpty()) {
        arguments[QStringLiteral("language")] = language;
    }
    return {.m_provider_tool = QStringLiteral("microsoft_code_sample_search"),
            .m_tool_arguments = arguments};
}

bool isKnownMicrosoftDocsTool(const QString& requested_tool) {
    static const QSet<QString> kTools{QStringLiteral("microsoft_docs_search"),
                                      QStringLiteral("microsoft_docs_fetch"),
                                      QStringLiteral("microsoft_code_sample_search"),
                                      QStringLiteral("code_sample_search")};
    return kTools.contains(requested_tool);
}

// Plans a microsoft_docs_fetch when an explicit url (or an explicit fetch request) is present,
// and an empty plan when this is not a fetch. A fetch target must be a real absolute URL: a
// query that merely starts with "http" is a search phrase, not a URL, and must not be fetched.
DocsToolPlan microsoftFetchPlan(const QString& query,
                                const QString& url,
                                const QString& requested_tool) {
    const bool explicit_fetch = requested_tool == QLatin1String("microsoft_docs_fetch");
    if (url.isEmpty() && !explicit_fetch && !isAbsoluteHttpUrl(query, false)) {
        return {};
    }
    const QString target = url.isEmpty() ? query : url;
    if (!isAbsoluteHttpUrl(target, false)) {
        return {.m_error = QStringLiteral(
                    "microsoft_docs_fetch requires an absolute http(s) url in arguments.url")};
    }
    return {.m_provider_tool = QStringLiteral("microsoft_docs_fetch"),
            .m_tool_arguments = QJsonObject{{QStringLiteral("url"), target}}};
}

DocsToolPlan microsoftDocsPlan(const QString& query, const QJsonObject& extra) {
    const QString type_error = stringFieldTypeError(
        extra,
        {QStringLiteral("tool"), QStringLiteral("url"), QStringLiteral("language")},
        QStringLiteral("docs_query arguments"));
    if (!type_error.isEmpty()) {
        return {.m_error = type_error};
    }
    const QString requested_tool = extra.value(QStringLiteral("tool")).toString().trimmed();
    const QString url = extra.value(QStringLiteral("url")).toString().trimmed();
    const QString language = extra.value(QStringLiteral("language")).toString().trimmed();
    // Fail CLOSED on an explicit tool request this provider does not implement, instead of
    // quietly downgrading it to an ordinary docs search the caller never asked for.
    if (!requested_tool.isEmpty() && !isKnownMicrosoftDocsTool(requested_tool)) {
        return {.m_error = QStringLiteral("Unknown microsoft_docs tool: %1").arg(requested_tool)};
    }

    DocsToolPlan fetch = microsoftFetchPlan(query, url, requested_tool);
    if (fetch.ok() || !fetch.m_error.isEmpty()) {
        return fetch;
    }

    DocsToolPlan code_sample = microsoftCodeSamplePlan(query, language, requested_tool);
    if (code_sample.ok() || !code_sample.m_error.isEmpty()) {
        return code_sample;
    }
    if (query.isEmpty()) {
        return {.m_error = QStringLiteral("Microsoft docs search requires query")};
    }
    return {.m_provider_tool = QStringLiteral("microsoft_docs_search"),
            .m_tool_arguments = QJsonObject{{QStringLiteral("query"), query}}};
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
    return {.m_error = QStringLiteral("docs_query supports context7 and microsoft_docs")};
}

QJsonObject loadHttpDocsProvider(const AiProviderRegistry& registry,
                                 const QString& provider_id,
                                 QString* error_message) {
    QJsonObject provider = registry.providerStatus(provider_id, error_message);
    if (provider.isEmpty()) {
        return {};
    }
    if (!provider.value(QStringLiteral("available")).toBool(false)) {
        if (error_message != nullptr) {
            const QString reason = provider.value(QStringLiteral("missing_reason")).toString();
            *error_message = reason.isEmpty() ? QStringLiteral("Provider unavailable") : reason;
        }
        return {};
    }
    if (provider.value(QStringLiteral("transport")).toString() == QLatin1String("http")) {
        // An on-disk providers.json silently overrides the bundled manifest, so the endpoint is
        // untrusted text. Refuse anything that is not an absolute https URL rather than posting
        // the model's query at whatever a rewritten manifest names.
        const QString endpoint = provider.value(QStringLiteral("endpoint")).toString().trimmed();
        if (!isAbsoluteHttpUrl(endpoint, true)) {
            if (error_message != nullptr) {
                *error_message =
                    QStringLiteral("docs_query provider endpoint is not an absolute https URL: %1")
                        .arg(provider_id);
            }
            return {};
        }
        return provider;
    }
    if (error_message != nullptr) {
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
    if (error_message != nullptr) {
        *error_message =
            QStringLiteral("MCP provider tool is not in bundled provider manifest: %1/%2")
                .arg(provider_id, provider_tool);
    }
    return false;
}

bool isAppOperation(const QString& operation) {
    static const QSet<QString> kOperations{QStringLiteral("app_manifest"),
                                           QStringLiteral("app_capabilities"),
                                           QStringLiteral("app_run_action_plan"),
                                           QStringLiteral("app_run_action")};
    return kOperations.contains(operation);
}

// Keeps the pointer-level error channel in step with the availability envelope: a success
// clears it (never leave stale text next to success=true), a failure carries the same message.
QJsonObject withAvailabilityError(QJsonObject result, QString* error_message) {
    if (error_message == nullptr) {
        return result;
    }
    if (result.value(QStringLiteral("success")).toBool(false)) {
        error_message->clear();
    } else {
        *error_message = result.value(QStringLiteral("error_message")).toString();
    }
    return result;
}

QJsonObject providerRegistryAvailability(const AiProviderRegistry& registry,
                                         const QString& operation) {
    QString error;
    const QJsonObject providers = registry.providersObject(&error);
    if (!error.isEmpty() || providers.isEmpty()) {
        // An empty registry is itself the failure; synthesize the reason so the caller never
        // receives a failure with no message at all.
        return availabilityError(operation,
                                 QStringLiteral("provider_registry_unavailable"),
                                 error.isEmpty()
                                     ? QStringLiteral("Provider registry is empty or unreadable")
                                     : error);
    }
    return availabilityOk(operation);
}

// provider_status is a question about ONE provider, so the registry loading is not enough:
// resolve the requested id too, or the check reports success for a provider that cannot be read.
QJsonObject providerStatusAvailability(const AiProviderRegistry& registry,
                                       const QJsonObject& args,
                                       const QString& operation) {
    const QJsonObject registry_status = providerRegistryAvailability(registry, operation);
    if (!registry_status.value(QStringLiteral("success")).toBool(false)) {
        return registry_status;
    }
    const QString provider_id = args.value(QStringLiteral("provider_id")).toString().trimmed();
    if (provider_id.isEmpty()) {
        return availabilityError(operation,
                                 QStringLiteral("invalid_request"),
                                 QStringLiteral("provider_status requires provider_id"));
    }
    QString error;
    const QJsonObject provider = registry.providerStatus(provider_id, &error);
    if (!error.isEmpty() || provider.isEmpty()) {
        return availabilityError(operation,
                                 QStringLiteral("provider_unknown"),
                                 error.isEmpty()
                                     ? QStringLiteral("Unknown provider: %1").arg(provider_id)
                                     : error);
    }
    QJsonObject ok = availabilityOk(operation);
    ok[QStringLiteral("provider_id")] = provider_id;
    return ok;
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
        return availabilityError(operation,
                                 QStringLiteral("provider_unknown"),
                                 error.isEmpty()
                                     ? QStringLiteral("Unknown provider: %1").arg(provider_id)
                                     : error);
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
    if (!isAbsoluteHttpUrl(provider.value(QStringLiteral("endpoint")).toString().trimmed(), true)) {
        return availabilityError(operation,
                                 QStringLiteral("invalid_endpoint"),
                                 QStringLiteral(
                                     "docs_query provider endpoint is not an absolute https URL"));
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
        return availabilityError(operation,
                                 QStringLiteral("app_manifest_unavailable"),
                                 error.isEmpty()
                                     ? QStringLiteral("No bundled manifest for app: %1").arg(app_id)
                                     : error);
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

// Non-empty when a docs_query request carries a wrong-typed field. A non-object "arguments"
// would otherwise become an empty object and the provider-specific planner would report the
// caller's fields as absent instead of rejecting the request.
QString docsQueryEnvelopeError(const QJsonObject& args) {
    const QJsonValue arguments = args.value(QStringLiteral("arguments"));
    if (!arguments.isUndefined() && !arguments.isObject()) {
        return QStringLiteral("docs_query arguments must be an object");
    }
    return stringFieldTypeError(args,
                                {QStringLiteral("provider_id"), QStringLiteral("query")},
                                QStringLiteral("docs_query"));
}

// Non-empty when a win32_mcp_call request carries a wrong-typed control field. Qt would coerce
// each of these to an empty/default value: a non-object tool_arguments would silently drop the
// model's explicit arguments and run the tool on server defaults (active tab/window), and a
// non-integer timeout_ms would silently become the 20s default.
QString win32CallEnvelopeError(const QJsonObject& args) {
    const QJsonValue arguments = args.value(QStringLiteral("arguments"));
    if (!arguments.isUndefined() && !arguments.isObject()) {
        return QStringLiteral("win32_mcp_call arguments must be an object");
    }
    const QJsonObject extra = arguments.toObject();
    const QString type_error =
        stringFieldTypeError(extra,
                             {QStringLiteral("tool"), QStringLiteral("tool_name")},
                             QStringLiteral("win32_mcp_call arguments"));
    if (!type_error.isEmpty()) {
        return type_error;
    }
    const QJsonValue tool_arguments = extra.value(QStringLiteral("tool_arguments"));
    if (!tool_arguments.isUndefined() && !tool_arguments.isObject()) {
        return QStringLiteral("win32_mcp_call arguments.tool_arguments must be an object");
    }
    const QJsonValue timeout = extra.value(QStringLiteral("timeout_ms"));
    if (!timeout.isUndefined() && !isIntegralJsonNumber(timeout)) {
        return QStringLiteral("win32_mcp_call arguments.timeout_ms must be an integer");
    }
    return {};
}

// Environment variables an on-disk providers.json must never set for the MCP child process:
// they redirect where that child resolves code (PATH, Qt plugin paths) or which user
// directories it reads and writes. The child can run elevated, so a manifest that names one of
// these is a configuration attack, not a preference -- fail closed instead of honoring it.
bool isProtectedChildEnvVar(const QString& name) {
    static const QSet<QString> kProtectedVars{
        QStringLiteral("PATH"),
        QStringLiteral("PATHEXT"),
        QStringLiteral("COMSPEC"),
        QStringLiteral("SYSTEMROOT"),
        QStringLiteral("WINDIR"),
        QStringLiteral("SYSTEMDRIVE"),
        QStringLiteral("TEMP"),
        QStringLiteral("TMP"),
        QStringLiteral("APPDATA"),
        QStringLiteral("LOCALAPPDATA"),
        QStringLiteral("PROGRAMDATA"),
        QStringLiteral("USERPROFILE"),
        QStringLiteral("HOMEDRIVE"),
        QStringLiteral("HOMEPATH"),
        QStringLiteral("PROGRAMFILES"),
        QStringLiteral("PROGRAMFILES(X86)"),
        QStringLiteral("PROGRAMW6432"),
        QStringLiteral("COMMONPROGRAMFILES"),
        QStringLiteral("COMMONPROGRAMFILES(X86)"),
        QStringLiteral("__COMPAT_LAYER"),
    };
    const QString key = name.trimmed().toUpper();
    return kProtectedVars.contains(key) || key.startsWith(QLatin1String("QT_")) ||
           key.startsWith(QLatin1String("LD_"));
}

// Fold a provider manifest's "environment" object into the child environment. Returns the
// first violation, empty on success -- a manifest that names a protected variable or supplies
// a non-string value is rejected outright rather than partially applied.
QString mergeManifestEnvironment(const QJsonObject& provider_env, QProcessEnvironment* env) {
    for (auto it = provider_env.constBegin(); it != provider_env.constEnd(); ++it) {
        if (isProtectedChildEnvVar(it.key())) {
            return QStringLiteral(
                       "Provider manifest may not set MCP child environment "
                       "variable: %1")
                .arg(it.key());
        }
        if (!it.value().isString()) {
            return QStringLiteral("Provider manifest environment value is not a string: %1")
                .arg(it.key());
        }
        env->insert(it.key(), it.value().toString());
    }
    return {};
}

bool isKnownWin32SecurityProfile(const QString& profile) {
    return profile == QLatin1String("read_only") || profile == QLatin1String("interactive") ||
           profile == QLatin1String("unrestricted");
}

// Refusing to build a child environment always means returning an empty one and, when the
// caller asked for it, the reason. Keeping that in one place stops the null check on the
// out-parameter from being repeated at every failure site.
QProcessEnvironment envFailure(QString* error_message, const QString& reason) {
    if (error_message != nullptr) {
        *error_message = reason;
    }
    return {};
}

QJsonObject jsonFailure(QString* error_message, const QString& reason) {
    if (error_message != nullptr) {
        *error_message = reason;
    }
    return {};
}

// Non-empty when a call plan cannot be executed as given. Checked on every call because a
// plan is a plain struct that a caller could have assembled by hand.
QString win32McpPlanError(const AiProviderGateway::Win32McpCallPlan& plan) {
    if (plan.provider.isEmpty() || plan.tool_name.trimmed().isEmpty()) {
        return QStringLiteral("Win32 MCP call plan is incomplete");
    }
    if (plan.timeout_ms < kWin32McpMinimumTimeoutMs ||
        plan.timeout_ms > kWin32McpMaximumTimeoutMs) {
        return QStringLiteral("Win32 MCP call plan timeout is out of range: %1")
            .arg(plan.timeout_ms);
    }
    return {};
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
    if (error_message != nullptr) {
        *error_message = provider.value(QStringLiteral("missing_reason"))
                             .toString(QStringLiteral("Win32 MCP provider unavailable"));
    }
    return false;
}

bool requireWin32Tool(const QJsonObject& provider,
                      const QString& tool_name,
                      QString* error_message) {
    if (tool_name.isEmpty()) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("win32_mcp_call requires arguments.tool_name");
        }
        return false;
    }
    if (providerHasTool(provider, tool_name)) {
        return true;
    }
    if (error_message != nullptr) {
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
                                                          plan.m_provider_tool,
                                                          plan.m_tool_arguments,
                                                          kDefaultProviderGatewayTimeoutMs,
                                                          error_message);
    if (message.isEmpty()) {
        return {};
    }
    const QString envelope_error = mcpEnvelopeError(message.value(QStringLiteral("result")));
    if (!envelope_error.isEmpty()) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("docs_query MCP %1").arg(envelope_error);
        }
        return {};
    }
    QJsonObject result =
        mcpDocsQueryResult(provider, plan.m_provider_tool, query, plan.m_tool_arguments, message);
    const QString logical_error = docsQueryLogicalError(result);
    if (!logical_error.isEmpty()) {
        if (error_message != nullptr) {
            *error_message = logical_error;
        }
        return {};
    }
    if (error_message != nullptr) {
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
    const QString envelope_error = docsQueryEnvelopeError(args);
    if (!envelope_error.isEmpty()) {
        if (error_message != nullptr) {
            *error_message = envelope_error;
        }
        return {};
    }
    const QString provider_id =
        args.value(QStringLiteral("provider_id")).toString().trimmed().toLower();
    if (provider_id.isEmpty()) {
        if (error_message != nullptr) {
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
        if (error_message != nullptr) {
            *error_message = plan.m_error;
        }
        return {};
    }
    if (!requireProviderTool(provider, provider_id, plan.m_provider_tool, error_message)) {
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

QJsonObject AiProviderGateway::win32McpAvailability(const QJsonObject& args,
                                                    const QString& operation) const {
    QString error;
    const Win32McpCallPlan plan = planWin32McpCall(args, &error);
    if (!error.isEmpty() || plan.provider.isEmpty()) {
        return availabilityError(operation,
                                 QStringLiteral("win32_mcp_unavailable"),
                                 error.isEmpty()
                                     ? QStringLiteral("Win32 MCP provider is unavailable")
                                     : error);
    }
    QJsonObject ok = availabilityOk(operation);
    ok[QStringLiteral("provider_id")] = QStringLiteral("win32_mcp");
    ok[QStringLiteral("provider_tool")] = plan.tool_name;
    ok[QStringLiteral("read_only_tool")] = plan.read_only;
    ok[QStringLiteral("high_risk_tool")] = plan.high_risk;
    return ok;
}

QJsonObject AiProviderGateway::checkAvailability(const QJsonObject& args,
                                                 QString* error_message) const {
    const QString operation =
        args.value(QStringLiteral("operation")).toString().trimmed().toLower();
    if (operation.isEmpty()) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("Provider gateway requires operation");
        }
        return availabilityError({},
                                 QStringLiteral("invalid_request"),
                                 QStringLiteral("Provider gateway requires operation"));
    }

    if (operation == QLatin1String("providers")) {
        return withAvailabilityError(providerRegistryAvailability(m_registry, operation),
                                     error_message);
    }

    if (operation == QLatin1String("provider_status")) {
        return withAvailabilityError(providerStatusAvailability(m_registry, args, operation),
                                     error_message);
    }

    if (operation == QLatin1String("docs_query")) {
        return withAvailabilityError(docsQueryAvailability(m_registry, args, operation),
                                     error_message);
    }

    if (operation == QLatin1String("win32_mcp_call")) {
        return withAvailabilityError(win32McpAvailability(args, operation), error_message);
    }

    if (isAppOperation(operation)) {
        return withAvailabilityError(appOperationAvailability(m_registry, args, operation),
                                     error_message);
    }

    return withAvailabilityError(
        availabilityError(
            operation,
            QStringLiteral("unsupported_operation"),
            QStringLiteral("Unsupported provider gateway operation: %1").arg(operation)),
        error_message);
}

AiProviderGateway::Win32McpCallPlan AiProviderGateway::planWin32McpCall(
    const QJsonObject& args, QString* error_message) const {
    Win32McpCallPlan plan;
    const QString envelope_error = win32CallEnvelopeError(args);
    if (!envelope_error.isEmpty()) {
        if (error_message != nullptr) {
            *error_message = envelope_error;
        }
        return plan;
    }
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
    if (error_message != nullptr) {
        error_message->clear();
    }
    return plan;
}

QJsonObject AiProviderGateway::callWin32Mcp(const Win32McpCallPlan& plan,
                                            QString* error_message) const {
    // A plan is a plain struct, so re-validate the execution-shaping fields here rather than
    // trusting that this one came from planWin32McpCall.
    const QString plan_error = win32McpPlanError(plan);
    if (!plan_error.isEmpty()) {
        return jsonFailure(error_message, plan_error);
    }

    QString environment_error;
    const QProcessEnvironment environment =
        win32McpEnvironment(plan.security_profile, plan.provider, &environment_error);
    if (!environment_error.isEmpty()) {
        return jsonFailure(error_message, environment_error);
    }

    const AiMcpStdioCallRequest request{
        .command = plan.provider.value(QStringLiteral("resolved_command")).toString(),
        .tool_name = plan.tool_name,
        .arguments = plan.tool_arguments,
        .environment = environment,
        .timeout_ms = plan.timeout_ms};
    // With a pool, the server process is reused across calls (keyed on command +
    // the full launch environment, so security profiles never share a process);
    // without one, fall back to a fresh process per call.
    const QJsonObject message = (m_mcp_pool != nullptr)
                                    ? m_mcp_pool->callTool(request, error_message)
                                    : AiMcpStdioClient::callTool(request, error_message);
    if (message.isEmpty()) {
        return {};
    }
    const QString result_error = mcpEnvelopeError(message.value(QStringLiteral("result")));
    if (!result_error.isEmpty()) {
        return jsonFailure(error_message, QStringLiteral("Win32 MCP %1").arg(result_error));
    }
    if (error_message != nullptr) {
        error_message->clear();
    }
    return win32McpResult(
        plan.provider, plan.tool_name, plan.tool_arguments, plan.security_profile, message);
}

bool AiProviderGateway::isWin32ReadOnlyTool(const QString& tool_name) {
    // KEEP IN SYNC with win32McpToolIsReadOnly (win32_mcp_dispatch.cpp): the server enforces
    // this same allowlist independently and REFUSES anything off it under the read_only profile.
    // browser_focus/hover/reveal are deliberately NOT here -- they mutate UI state (move focus,
    // dispatch hover, scroll into view), so the server rejects them under read_only. Listing them
    // here would send read_only AND report them safe, yet the call would fail; instead they fall
    // to the fail-closed default in populateWin32Plan (interactive profile + human confirmation).
    static const QSet<QString> kReadOnly{
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
    return kReadOnly.contains(tool_name.trimmed());
}

bool AiProviderGateway::isWin32HighRiskTool(const QString& tool_name) {
    // Only tools the win32 server actually implements. list_processes/kill_process/start_process
    // were classified here (and as read-only) but were never in the server manifest, so their
    // classification was dead -- removed.
    static const QSet<QString> kHighRisk{
        QStringLiteral("close_window"),
    };
    return kHighRisk.contains(tool_name.trimmed());
}

bool AiProviderGateway::isWin32InputTool(const QString& tool_name) {
    static const QSet<QString> kInputTools{
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
    return kInputTools.contains(tool_name.trimmed());
}

bool AiProviderGateway::isWin32DesktopInputTool(const QString& tool_name) {
    // ONLY the physical-desktop-driving input tools: mouse/keyboard via SendInput, UIA
    // activation, dialog dismissal, and focus. Deliberately EXCLUDES the browser input tools,
    // clipboard_write, and extension install/uninstall -- those still require a per-call human
    // confirm even inside a win32_gui recipe (a SUPERAntiSpyware-style desktop recipe does not
    // need them). This is a positive allowlist, so a newly added input tool is rejected in
    // recipes until it is explicitly added here.
    static const QSet<QString> kDesktopTools{
        QStringLiteral("click_text"),
        QStringLiteral("uia_click_control"),
        QStringLiteral("dismiss_dialog"),
        QStringLiteral("mouse_click"),
        QStringLiteral("type_text"),
        QStringLiteral("send_keys"),
        QStringLiteral("focus_window"),
    };
    return kDesktopTools.contains(tool_name.trimmed());
}

QProcessEnvironment AiProviderGateway::win32McpEnvironment(const QString& security_profile,
                                                           const QJsonObject& provider,
                                                           QString* error_message) {
    const QString profile = security_profile.trimmed().toLower();
    const bool read_only_profile = profile == QLatin1String("read_only");
    if (!isKnownWin32SecurityProfile(profile)) {
        return envFailure(
            error_message,
            QStringLiteral("Unknown Win32 MCP security profile: %1").arg(security_profile));
    }

    const QJsonValue provider_env_value = provider.value(QStringLiteral("environment"));
    if (!provider_env_value.isUndefined() && !provider_env_value.isObject()) {
        return envFailure(error_message,
                          QStringLiteral("Provider manifest environment is not an object"));
    }

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString merge_error = mergeManifestEnvironment(provider_env_value.toObject(), &env);
    if (!merge_error.isEmpty()) {
        return envFailure(error_message, merge_error);
    }
    // The server resolves WIN32_MCP_SECURITY_PROFILE with exactly two recognized states: the
    // token "read_only" locks it down, and an UNSET token is the documented full-access default
    // the interactive/unrestricted tiers plan for. Any OTHER token makes the server fail closed
    // to read-only, so passing "interactive"/"unrestricted" verbatim would silently reject every
    // mutating call AFTER planning and confirmation had already reported success. Clear the
    // variable instead, including any value inherited from this process or set by the manifest.
    if (read_only_profile) {
        env.insert(QStringLiteral("WIN32_MCP_SECURITY_PROFILE"), QStringLiteral("read_only"));
    } else {
        env.remove(QStringLiteral("WIN32_MCP_SECURITY_PROFILE"));
    }
    env.insert(QStringLiteral("WIN32_MCP_RESULT_ENVELOPE"), QStringLiteral("true"));
    env.insert(QStringLiteral("WIN32_MCP_REDACT_SENSITIVE_OUTPUT"), QStringLiteral("true"));
    if (error_message != nullptr) {
        error_message->clear();
    }
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

// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/openai_responses_client.h"

#include "sak/ai/ai_credential_store.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/version.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSslConfiguration>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace sak::ai {

namespace {

constexpr char kOpenAiBaseUrl[] = "https://api.openai.com";
constexpr int kOpenAiTimeoutMs = 120'000;
constexpr int kLocalToolTimeoutMinSeconds = 5;
constexpr int kLocalToolTimeoutMaxSeconds = 3600;
constexpr int kPackageToolTimeoutMaxSeconds = 7200;
constexpr int kSessionSearchMaxResults = kPercentMax;
constexpr qsizetype kMinimumOpenAiApiKeyLength = 20;

// Hard cap on an OpenAI HTTP response body before it is parsed and copied (JSON DOM,
// raw_json, joined output). A Responses/models JSON document is at most a few MiB;
// 64 MiB is generous while bounding a hostile or oversized endpoint (e.g. a MITM or
// redirected peer) from amplifying memory through repeated copies. Fails closed.
constexpr qsizetype kMaxResponseBodyBytes = 64LL * 1024 * 1024;

[[nodiscard]] QString firstNonEmptyError(const QString& first, const QString& fallback) {
    return first.trimmed().isEmpty() ? fallback : first;
}

[[nodiscard]] QString contentTextFromValue(const QJsonValue& value) {
    if (value.isString()) {
        return value.toString();
    }

    if (!value.isArray()) {
        return {};
    }

    QStringList parts;
    const auto array = value.toArray();
    for (const auto& item_value : array) {
        const auto item = item_value.toObject();
        const QString type = item.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("output_text") || type == QLatin1String("text") ||
            type == QLatin1String("input_text")) {
            const QString text = item.value(QStringLiteral("text")).toString();
            if (!text.isEmpty()) {
                parts.append(text);
            }
        } else if (type == QLatin1String("refusal")) {
            // Surface a model refusal as visible output rather than dropping it, which
            // would otherwise degrade into a misleading "no output text" error.
            const QString refusal = item.value(QStringLiteral("refusal")).toString();
            if (!refusal.isEmpty()) {
                parts.append(refusal);
            }
        }
    }
    return parts.join(QString());
}

void collectCitationsFromValue(const QJsonValue& value, QVector<OpenAIUrlCitation>& citations) {
    if (!value.isArray()) {
        return;
    }
    for (const auto& item_value : value.toArray()) {
        const auto item = item_value.toObject();
        const auto annotations = item.value(QStringLiteral("annotations")).toArray();
        for (const auto& annotation_value : annotations) {
            const auto annotation = annotation_value.toObject();
            if (annotation.value(QStringLiteral("type")).toString() !=
                QLatin1String("url_citation")) {
                continue;
            }
            OpenAIUrlCitation citation;
            citation.url = annotation.value(QStringLiteral("url")).toString();
            citation.title = annotation.value(QStringLiteral("title")).toString();
            citation.start_index = annotation.value(QStringLiteral("start_index")).toInt(-1);
            citation.end_index = annotation.value(QStringLiteral("end_index")).toInt(-1);
            if (!citation.url.isEmpty()) {
                citations.append(citation);
            }
        }
    }
}

std::optional<QJsonObject> responseRootObject(const QByteArray& data, QString* error_message) {
    if (data.size() > kMaxResponseBodyBytes) {
        if (error_message) {
            *error_message = QStringLiteral("OpenAI response body exceeds the size limit");
        }
        return std::nullopt;
    }
    QJsonParseError parse_error;
    const auto doc = QJsonDocument::fromJson(data, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error_message) {
            *error_message = firstNonEmptyError(parse_error.errorString(),
                                                QStringLiteral("Invalid OpenAI response JSON"));
        }
        return std::nullopt;
    }
    const QString api_error = OpenAIResponsesClient::extractApiError(data);
    if (!api_error.isEmpty()) {
        if (error_message) {
            *error_message = api_error;
        }
        return std::nullopt;
    }
    return doc.object();
}

// Return a non-empty error if the response is flagged incomplete (e.g. truncated by
// max_output_tokens), else an empty string.
[[nodiscard]] QString incompleteResponseError(const QJsonObject& root) {
    if (root.value(QStringLiteral("status")).toString() != QStringLiteral("incomplete")) {
        return {};
    }
    const QString reason = root.value(QStringLiteral("incomplete_details"))
                               .toObject()
                               .value(QStringLiteral("reason"))
                               .toString();
    return reason.isEmpty() ? QStringLiteral("OpenAI response is incomplete")
                            : QStringLiteral("OpenAI response is incomplete: %1").arg(reason);
}

// Return a non-empty error if the response terminated in a failed state. A status of
// "failed" carries no usable output and may include partial/dangerous tool calls, so it
// must fail closed rather than be treated as a normal (empty) completion.
[[nodiscard]] QString failedResponseError(const QJsonObject& root) {
    if (root.value(QStringLiteral("status")).toString() != QStringLiteral("failed")) {
        return {};
    }
    const QString reason =
        root.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
    return reason.isEmpty() ? QStringLiteral("OpenAI response failed")
                            : QStringLiteral("OpenAI response failed: %1").arg(reason);
}

// Non-empty when the response reached any non-success state that must fail closed
// instead of being read as a usable answer.
[[nodiscard]] QString terminalResponseError(const QJsonObject& root) {
    const QString incomplete = incompleteResponseError(root);
    if (!incomplete.isEmpty()) {
        return incomplete;
    }
    const QString failed = failedResponseError(root);
    if (!failed.isEmpty()) {
        return failed;
    }
    // Fail closed on any other non-success status. The synchronous /v1/responses
    // endpoint returns a terminal response object; a status that is present but not
    // "completed" (cancelled, in_progress, queued, or an unrecognized/wrong-typed
    // value) carries no output that can be trusted, so it must not be read as an
    // answer. A response with no status field (never produced by the live endpoint,
    // but used by unit fixtures) is left to the normal output/empty checks.
    const QJsonValue status_value = root.value(QStringLiteral("status"));
    if (status_value.isUndefined() || status_value.isNull()) {
        return {};
    }
    if (!status_value.isString()) {
        return QStringLiteral("OpenAI response status was not a string");
    }
    const QString status = status_value.toString();
    if (status != QStringLiteral("completed")) {
        return QStringLiteral("OpenAI response is not complete (status: %1)").arg(status);
    }
    return {};
}

void appendFunctionCallFromOutputItem(OpenAIResponseResult* result, const QJsonObject& item) {
    OpenAIFunctionCall call;
    call.call_id = item.value(QStringLiteral("call_id")).toString();
    call.name = item.value(QStringLiteral("name")).toString();
    call.arguments_json = item.value(QStringLiteral("arguments")).toString();
    if (!call.call_id.isEmpty() && !call.name.isEmpty()) {
        result->function_calls.append(call);
    }
}

void appendMessageFromOutputItem(OpenAIResponseResult* result,
                                 QStringList* output_parts,
                                 const QJsonObject& item) {
    const auto content_value = item.value(QStringLiteral("content"));
    const QString text = contentTextFromValue(content_value);
    if (!text.isEmpty()) {
        output_parts->append(text);
    }
    collectCitationsFromValue(content_value, result->citations);
}

void appendOutputItem(OpenAIResponseResult* result,
                      QStringList* output_parts,
                      const QJsonObject& item) {
    const QString type = item.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("function_call")) {
        appendFunctionCallFromOutputItem(result, item);
        return;
    }
    if (type == QLatin1String("message")) {
        appendMessageFromOutputItem(result, output_parts, item);
    }
}

QJsonArray functionOutputInputItems(const QVector<OpenAIFunctionOutput>& outputs) {
    QJsonArray input_items;
    for (const auto& output : outputs) {
        QJsonObject item;
        item[QStringLiteral("type")] = QStringLiteral("function_call_output");
        item[QStringLiteral("call_id")] = output.call_id;
        item[QStringLiteral("output")] = output.output;
        input_items.append(item);
    }
    return input_items;
}

QJsonObject textContentItem(const QString& text) {
    QJsonObject item;
    item[QStringLiteral("type")] = QStringLiteral("input_text");
    item[QStringLiteral("text")] = text;
    return item;
}

QJsonObject textAttachmentItem(const OpenAIInputAttachment& attachment) {
    return textContentItem(
        attachment.label.trimmed().isEmpty()
            ? attachment.text
            : QStringLiteral("Context: %1\n%2").arg(attachment.label, attachment.text));
}

QJsonObject imageAttachmentItem(const OpenAIInputAttachment& attachment) {
    QJsonObject item;
    if (!attachment.image_url.trimmed().isEmpty()) {
        item[QStringLiteral("type")] = QStringLiteral("input_image");
        item[QStringLiteral("image_url")] = attachment.image_url;
    }
    return item;
}

QJsonObject fileAttachmentItem(const OpenAIInputAttachment& attachment) {
    QJsonObject item;
    if (attachment.file_data.trimmed().isEmpty() && attachment.filename.trimmed().isEmpty()) {
        return item;
    }
    item[QStringLiteral("type")] = QStringLiteral("input_file");
    if (!attachment.filename.trimmed().isEmpty()) {
        item[QStringLiteral("filename")] = attachment.filename.trimmed();
    }
    if (!attachment.file_data.trimmed().isEmpty()) {
        item[QStringLiteral("file_data")] = attachment.file_data;
    }
    return item;
}

void addAttachmentDetail(QJsonObject* item, const OpenAIInputAttachment& attachment) {
    if (!item->isEmpty() && !attachment.detail.trimmed().isEmpty() &&
        attachment.detail != QLatin1String("auto")) {
        item->insert(QStringLiteral("detail"), attachment.detail.trimmed());
    }
}

QJsonObject attachmentContentItem(const OpenAIInputAttachment& attachment) {
    QJsonObject item;
    switch (attachment.type) {
    case OpenAIInputAttachment::Type::Text:
        item = textAttachmentItem(attachment);
        break;
    case OpenAIInputAttachment::Type::Image:
        item = imageAttachmentItem(attachment);
        break;
    case OpenAIInputAttachment::Type::File:
        item = fileAttachmentItem(attachment);
        break;
    }
    addAttachmentDetail(&item, attachment);
    return item;
}

QJsonArray attachmentInput(const OpenAIResponseRequest& request) {
    QJsonArray content;
    content.append(textContentItem(request.input));
    for (const auto& attachment : request.attachments) {
        const QJsonObject item = attachmentContentItem(attachment);
        if (!item.isEmpty()) {
            content.append(item);
        }
    }
    QJsonObject message;
    message[QStringLiteral("role")] = QStringLiteral("user");
    message[QStringLiteral("content")] = content;
    return QJsonArray{message};
}

QJsonObject stringParameter(const QString& description) {
    QJsonObject param;
    param[QStringLiteral("type")] = QStringLiteral("string");
    param[QStringLiteral("description")] = description;
    return param;
}

QJsonObject integerParameter(const QString& description, int minimum, int maximum) {
    QJsonObject param;
    param[QStringLiteral("type")] = QStringLiteral("integer");
    param[QStringLiteral("description")] = description;
    param[QStringLiteral("minimum")] = minimum;
    param[QStringLiteral("maximum")] = maximum;
    return param;
}

QJsonObject booleanParameter(const QString& description) {
    QJsonObject param;
    param[QStringLiteral("type")] = QStringLiteral("boolean");
    param[QStringLiteral("description")] = description;
    return param;
}

QJsonObject functionTool(const QString& name,
                         const QString& description,
                         const QJsonObject& properties,
                         const QJsonArray& required) {
    QJsonObject parameters;
    parameters[QStringLiteral("type")] = QStringLiteral("object");
    parameters[QStringLiteral("properties")] = properties;
    parameters[QStringLiteral("required")] = required;
    parameters[QStringLiteral("additionalProperties")] = false;

    QJsonObject tool;
    tool[QStringLiteral("type")] = QStringLiteral("function");
    tool[QStringLiteral("name")] = name;
    tool[QStringLiteral("description")] = description;
    tool[QStringLiteral("parameters")] = parameters;
    tool[QStringLiteral("strict")] = true;
    return tool;
}

QJsonObject shellTool(const QString& name,
                      const QString& description,
                      const QString& command_help) {
    QJsonObject properties;
    properties[QStringLiteral("command")] = stringParameter(command_help);
    properties[QStringLiteral("timeout_seconds")] =
        integerParameter(QStringLiteral("Timeout in seconds, from 5 to 3600."),
                         kLocalToolTimeoutMinSeconds,
                         kLocalToolTimeoutMaxSeconds);
    properties[QStringLiteral("requires_admin")] =
        booleanParameter(QStringLiteral("Whether this command requires administrator rights."));
    return functionTool(name,
                        description,
                        properties,
                        QJsonArray{QStringLiteral("command"),
                                   QStringLiteral("timeout_seconds"),
                                   QStringLiteral("requires_admin")});
}

QJsonObject processTool() {
    QJsonObject properties;
    properties[QStringLiteral("program")] =
        stringParameter(QStringLiteral("Absolute path or PATH-resolvable name of the executable."));
    QJsonObject arguments_param;
    arguments_param[QStringLiteral("type")] = QStringLiteral("array");
    arguments_param[QStringLiteral("description")] =
        QStringLiteral("Arguments passed verbatim to the program.");
    arguments_param[QStringLiteral("items")] = stringParameter(QString());
    properties[QStringLiteral("arguments")] = arguments_param;
    properties[QStringLiteral("timeout_seconds")] =
        integerParameter(QStringLiteral("Timeout in seconds, from 5 to 3600."),
                         kLocalToolTimeoutMinSeconds,
                         kLocalToolTimeoutMaxSeconds);
    properties[QStringLiteral("requires_admin")] = booleanParameter(QStringLiteral(
        "Whether the program requires admin rights. Must be false because run_process does not "
        "support elevation."));
    return functionTool(QStringLiteral("run_process"),
                        QStringLiteral("Launch an executable directly with explicit arguments. Use "
                                       "for vendor tools, installers, or any program where shell "
                                       "quoting would be error-prone. Elevation is NOT supported "
                                       "here yet; use run_powershell for admin tasks."),
                        properties,
                        QJsonArray{QStringLiteral("program"),
                                   QStringLiteral("arguments"),
                                   QStringLiteral("timeout_seconds"),
                                   QStringLiteral("requires_admin")});
}

QJsonObject screenshotTool() {
    QJsonObject properties;
    properties[QStringLiteral("reason")] = stringParameter(
        QStringLiteral("Brief reason for the capture, recorded in the session log."));
    return functionTool(QStringLiteral("take_screenshot"),
                        QStringLiteral(
                            "Capture the user's primary screen and save it to the "
                            "session artifacts. Returns the file path and dimensions. "
                            "Use when visual evidence will materially help diagnose a UI "
                            "issue or document state."),
                        properties,
                        QJsonArray{QStringLiteral("reason")});
}

QJsonObject downloadTool() {
    QJsonObject properties;
    properties[QStringLiteral("url")] =
        stringParameter(QStringLiteral("Absolute https URL to download."));
    properties[QStringLiteral("filename")] =
        stringParameter(QStringLiteral("Suggested filename. If empty, derived from the URL."));
    return functionTool(QStringLiteral("download_file"),
                        QStringLiteral(
                            "Download a file from an https URL into the session "
                            "artifacts/downloads folder. Returns the absolute path, byte "
                            "size, and SHA-256 of the downloaded file. Use for vendor "
                            "tools, log bundles, or other artifacts the user wants "
                            "captured."),
                        properties,
                        QJsonArray{QStringLiteral("url"), QStringLiteral("filename")});
}

QJsonObject packageTool() {
    QJsonObject properties;
    QJsonObject operation = stringParameter(QStringLiteral(
        "Operation to run: search, install, uninstall, upgrade, is_installed, installed_version, "
        "outdated, version."));
    operation[QStringLiteral("enum")] = QJsonArray{QStringLiteral("search"),
                                                   QStringLiteral("install"),
                                                   QStringLiteral("uninstall"),
                                                   QStringLiteral("upgrade"),
                                                   QStringLiteral("is_installed"),
                                                   QStringLiteral("installed_version"),
                                                   QStringLiteral("outdated"),
                                                   QStringLiteral("version")};
    properties[QStringLiteral("operation")] = operation;
    properties[QStringLiteral("query")] =
        stringParameter(QStringLiteral("Search text for operation=search; otherwise empty."));
    properties[QStringLiteral("package_id")] = stringParameter(
        QStringLiteral("Chocolatey package id for install/uninstall/upgrade/status operations."));
    properties[QStringLiteral("version")] = stringParameter(
        QStringLiteral("Optional pinned package version. Empty means latest stable."));
    properties[QStringLiteral("timeout_seconds")] = integerParameter(
        QStringLiteral("Timeout in seconds, from 5 to 7200. Use 1800 or more for installs."),
        kLocalToolTimeoutMinSeconds,
        kPackageToolTimeoutMaxSeconds);
    return functionTool(QStringLiteral("sak_package_manager"),
                        QStringLiteral("Use S.A.K. Utility's built-in bundled Chocolatey package "
                                       "manager. Use this before web search, raw choco/winget "
                                       "commands, or vendor downloads for app search, install, "
                                       "uninstall, upgrade, installed-version checks, and "
                                       "outdated-package checks. Do not use install/upgrade/"
                                       "uninstall as a substitute for running an installed app's "
                                       "scan/action. For scan requests, first check installed "
                                       "status and app capabilities through sak_provider_gateway; "
                                       "only install when the user explicitly asks to install, "
                                       "repair-install, or upgrade software."),
                        properties,
                        QJsonArray{QStringLiteral("operation"),
                                   QStringLiteral("query"),
                                   QStringLiteral("package_id"),
                                   QStringLiteral("version"),
                                   QStringLiteral("timeout_seconds")});
}

QJsonObject offlinePackageArrayParameter() {
    QJsonObject package_id = stringParameter(QStringLiteral("Chocolatey package id."));
    QJsonObject version =
        stringParameter(QStringLiteral("Pinned package version, empty for latest."));
    QJsonObject item_properties;
    item_properties[QStringLiteral("package_id")] = package_id;
    item_properties[QStringLiteral("version")] = version;

    QJsonObject item;
    item[QStringLiteral("type")] = QStringLiteral("object");
    item[QStringLiteral("properties")] = item_properties;
    item[QStringLiteral("required")] = QJsonArray{QStringLiteral("package_id"),
                                                  QStringLiteral("version")};
    item[QStringLiteral("additionalProperties")] = false;

    QJsonObject packages;
    packages[QStringLiteral("type")] = QStringLiteral("array");
    packages[QStringLiteral("description")] = QStringLiteral(
        "Packages for direct_download/build_bundle. Each item needs package_id and version; "
        "version may be empty/latest.");
    packages[QStringLiteral("items")] = item;
    return packages;
}

QJsonObject offlineTool() {
    QJsonObject operation = stringParameter(QStringLiteral(
        "Operation to run: search, presets, direct_download, build_bundle, install_bundle."));
    operation[QStringLiteral("enum")] = QJsonArray{QStringLiteral("search"),
                                                   QStringLiteral("presets"),
                                                   QStringLiteral("direct_download"),
                                                   QStringLiteral("build_bundle"),
                                                   QStringLiteral("install_bundle")};
    QJsonObject properties;
    properties[QStringLiteral("operation")] = operation;
    properties[QStringLiteral("query")] = stringParameter(
        QStringLiteral("Package/product search text for operation=search; otherwise empty."));
    properties[QStringLiteral("output_dir")] = stringParameter(QStringLiteral(
        "Output directory for direct_download/build_bundle. Empty uses the current AI session "
        "artifacts folder."));
    properties[QStringLiteral("manifest_path")] = stringParameter(
        QStringLiteral("Path to manifest.json for operation=install_bundle; otherwise empty."));
    properties[QStringLiteral("packages")] = offlinePackageArrayParameter();
    return functionTool(QStringLiteral("sak_offline_downloader"),
                        QStringLiteral("Use S.A.K. Utility's built-in offline "
                                       "deployment/downloader workflow. Use this first when the "
                                       "user asks for offline installers, direct installer "
                                       "downloads, offline Chocolatey bundles, package presets, or "
                                       "installing from an offline bundle."),
                        properties,
                        QJsonArray{QStringLiteral("operation"),
                                   QStringLiteral("query"),
                                   QStringLiteral("output_dir"),
                                   QStringLiteral("manifest_path"),
                                   QStringLiteral("packages")});
}

QJsonObject providerGatewayTool() {
    QJsonObject operation = stringParameter(QStringLiteral(
        "Operation to run: providers, provider_status, docs_query, win32_mcp_call, app_manifest, "
        "app_capabilities, app_run_action_plan, app_run_action."));
    operation[QStringLiteral("enum")] = QJsonArray{QStringLiteral("providers"),
                                                   QStringLiteral("provider_status"),
                                                   QStringLiteral("docs_query"),
                                                   QStringLiteral("win32_mcp_call"),
                                                   QStringLiteral("app_manifest"),
                                                   QStringLiteral("app_capabilities"),
                                                   QStringLiteral("app_run_action_plan"),
                                                   QStringLiteral("app_run_action")};

    QJsonObject arguments = stringParameter(
        QStringLiteral("JSON object string for provider-specific arguments. Use {} when unused. "
                       "For win32_mcp_call use {\"tool_name\":\"...\",\"tool_arguments\":{...},"
                       "\"timeout_ms\":20000}. For Context7 docs_query use {\"libraryId\":\"...\"} "
                       "after resolving a library id."));

    QJsonObject properties;
    properties[QStringLiteral("operation")] = operation;
    properties[QStringLiteral("provider_id")] =
        stringParameter(QStringLiteral("Provider id, e.g. microsoft_docs, context7, win32_mcp."));
    properties[QStringLiteral("app_id")] = stringParameter(
        QStringLiteral("App manifest id, e.g. windows_defender, superantispyware, windows_sfc."));
    properties[QStringLiteral("action")] =
        stringParameter(QStringLiteral("Requested app action, e.g. quick_scan or verify_only."));
    properties[QStringLiteral("query")] =
        stringParameter(QStringLiteral("Documentation or provider query; empty when unused."));
    properties[QStringLiteral("arguments")] = arguments;

    return functionTool(QStringLiteral("sak_provider_gateway"),
                        QStringLiteral("Use S.A.K. Utility's bundled provider/app-control "
                                       "registry. Use this before raw shell probing when checking "
                                       "MCP providers, app manifests, or whether an app action is "
                                       "supported. Use operation=docs_query for Microsoft Learn "
                                       "MCP or Context7 public documentation lookup. Use "
                                       "operation=win32_mcp_call for bundled Win32 MCP desktop "
                                       "automation when local tools are enabled. Use "
                                       "operation=app_run_action only for supported app manifest "
                                       "actions. Access mode and tool risk determine "
                                       "confirmation/security profile."),
                        properties,
                        QJsonArray{QStringLiteral("operation"),
                                   QStringLiteral("provider_id"),
                                   QStringLiteral("app_id"),
                                   QStringLiteral("action"),
                                   QStringLiteral("query"),
                                   QStringLiteral("arguments")});
}

QJsonObject sessionSearchTool() {
    QJsonObject properties;
    properties[QStringLiteral("query")] = stringParameter(QStringLiteral(
        "Text to search across saved AI session titles, transcript, and command records."));
    properties[QStringLiteral("max_results")] = integerParameter(
        QStringLiteral("Maximum hits to return, from 1 to 100."), 1, kSessionSearchMaxResults);

    return functionTool(QStringLiteral("sak_session_search"),
                        QStringLiteral("Search saved S.A.K. AI session transcript and command "
                                       "indexes. Use this when debugging prior runs, finding QA "
                                       "failures, locating tool-loop evidence, or comparing a "
                                       "current issue against earlier sessions."),
                        properties,
                        QJsonArray{QStringLiteral("query"), QStringLiteral("max_results")});
}

QJsonObject skillTool() {
    QJsonObject operation = stringParameter(
        QStringLiteral("Which action to take: \"list\" returns the skill catalog (id, title, "
                       "description, when-to-use); \"load\" returns the full guidance body for one "
                       "skill_id."));
    operation[QStringLiteral("enum")] = QJsonArray{QStringLiteral("list"), QStringLiteral("load")};

    QJsonObject properties;
    properties[QStringLiteral("operation")] = operation;
    properties[QStringLiteral("skill_id")] = stringParameter(
        QStringLiteral("Skill id to load when operation is \"load\" (taken from the catalog). Pass "
                       "an empty string when operation is \"list\"."));

    return functionTool(
        QStringLiteral("sak_skill"),
        QStringLiteral(
            "List and load reusable task-guidance skills. Call operation=\"list\" to discover "
            "available skills (id, description, when to use), then operation=\"load\" with a "
            "skill_id to read the full guidance before performing a matching task. Read-only "
            "guidance lookup with no PC, disk, or network access."),
        properties,
        QJsonArray{QStringLiteral("operation"), QStringLiteral("skill_id")});
}

QJsonObject delegateSubagentTool() {
    QJsonObject tool_policy = stringParameter(QStringLiteral(
        "Optional capability ceiling for the sub-agent, clamped to this session's access mode (the "
        "sub-agent can never be more permissive than the session). Omit or use "
        "\"read_only_pc\" for a read/diagnose sub-agent."));
    tool_policy[QStringLiteral("enum")] = QJsonArray{QStringLiteral("no_local_execution"),
                                                     QStringLiteral("read_only_pc"),
                                                     QStringLiteral("download_only"),
                                                     QStringLiteral("package_tools_only"),
                                                     QStringLiteral("mutating_requires_lease"),
                                                     QStringLiteral("exclusive_mutating_executor")};

    QJsonObject properties;
    properties[QStringLiteral("objective")] = stringParameter(QStringLiteral(
        "The single, self-contained task for the sub-agent, written as a complete instruction. "
        "Include the context it needs; the sub-agent does not see the chat history."));
    properties[QStringLiteral("tool_policy")] = tool_policy;
    properties[QStringLiteral("expected_output")] = stringParameter(QStringLiteral(
        "Short description of the result you want back (e.g. \"root-cause summary + fix steps\"). "
        "The sub-agent returns a structured JSON result you then use."));

    return functionTool(
        QStringLiteral("delegate_subagent"),
        QStringLiteral(
            "Delegate one scoped sub-task to a focused sub-agent and get its structured result "
            "back. Use this to parallelize independent investigation, isolate a noisy/large "
            "sub-task from the main thread, or run a specialist step (diagnose, research, verify) "
            "without cluttering this conversation. You remain the overseer: delegate concrete, "
            "self-contained objectives, then synthesize the sub-agent's result. For a large "
            "multi-step technician procedure, prefer a declared workflow instead."),
        properties,
        QJsonArray{QStringLiteral("objective"),
                   QStringLiteral("tool_policy"),
                   QStringLiteral("expected_output")});
}

QJsonObject runWorkflowTool() {
    QJsonObject properties;
    properties[QStringLiteral("workflow_id")] = stringParameter(QStringLiteral(
        "The id of a workflow from the workflow catalog shown in your context (the value in "
        "brackets, e.g. \"browser_issue_cleanup\")."));
    properties[QStringLiteral("input_values")] = stringParameter(QStringLiteral(
        "A JSON object, encoded as a string, mapping each of the workflow's required input ids to "
        "its value, e.g. {\"app_name\":\"Chrome\"}. Use \"{}\" if it needs none. If you are "
        "missing "
        "a required input, ask the user for it before calling this."));

    return functionTool(
        QStringLiteral("run_workflow"),
        QStringLiteral(
            "Run a declared multi-step S.A.K. Utility workflow from the catalog end-to-end and get "
            "its structured result back. Use this for a full technician procedure that the catalog "
            "already covers (multi-phase diagnose/repair/verify with its own agents and gates) "
            "instead of driving each step yourself. Each phase is independently gated as it runs. "
            "Provide all required inputs; for a single scoped step prefer delegate_subagent."),
        properties,
        QJsonArray{QStringLiteral("workflow_id"), QStringLiteral("input_values")});
}

QJsonObject appActionTool() {
    QJsonObject operation = stringParameter(QStringLiteral(
        "\"list\" enumerates S.A.K. Utility's own built-in technician actions (id, title, "
        "description, category, and risk flags: read_only/mutating/destructive/requires_admin); "
        "\"run\" executes one action by id."));
    operation[QStringLiteral("enum")] = QJsonArray{QStringLiteral("list"), QStringLiteral("run")};

    QJsonObject properties;
    properties[QStringLiteral("operation")] = operation;
    properties[QStringLiteral("action_id")] = stringParameter(QStringLiteral(
        "Action id to run when operation is \"run\" (taken from the list). Empty string for "
        "\"list\"."));
    properties[QStringLiteral("arguments")] = stringParameter(QStringLiteral(
        "JSON object of arguments for the action, passed as a string (e.g. \"{}\"). Most actions "
        "take none; pass \"{}\"."));

    return functionTool(
        QStringLiteral("sak_app_action"),
        QStringLiteral(
            "Enumerate and run S.A.K. Utility's OWN built-in technician actions headless -- the "
            "same operations the app's panels perform (system file repair, disk checks, network "
            "reset, power optimization, reports, backups, and more). Call operation=\"list\" first "
            "to discover actions and their risk flags, then operation=\"run\" with an action_id. "
            "PREFER these app actions over raw run_powershell when an action already covers the "
            "task. Mutating or admin actions are gated with a confirmation and, when destructive, "
            "a "
            "restore point."),
        properties,
        QJsonArray{
            QStringLiteral("operation"), QStringLiteral("action_id"), QStringLiteral("arguments")});
}

QJsonArray localToolDefinitions() {
    QJsonArray tools;
    tools.append(shellTool(
        QStringLiteral("run_powershell"),
        QStringLiteral(
            "Run arbitrary Windows PowerShell on the user's PC and return stdout, stderr, exit "
            "code, timeout, and cancellation status. Use this when the user asks you to inspect, "
            "diagnose, repair, install, uninstall, download, check logs, or verify the local "
            "computer. Prefer read-only diagnostic commands before repair commands. This is the "
            "only tool that supports elevation."),
        QStringLiteral("PowerShell command or multi-line script to execute.")));
    tools.append(shellTool(QStringLiteral("run_cmd"),
                           QStringLiteral("Run a Windows cmd.exe command. Use when the task "
                                          "naturally fits cmd syntax or classic Windows tooling. "
                                          "Elevation is NOT supported for this tool yet; if you "
                                          "need admin rights, use run_powershell instead."),
                           QStringLiteral("cmd.exe command line. Will be invoked as `cmd.exe /c "
                                          "<command>`.")));
    tools.append(processTool());
    tools.append(screenshotTool());
    tools.append(downloadTool());
    tools.append(packageTool());
    tools.append(offlineTool());
    tools.append(providerGatewayTool());
    tools.append(sessionSearchTool());
    tools.append(skillTool());
    tools.append(delegateSubagentTool());
    tools.append(runWorkflowTool());
    tools.append(appActionTool());
    return tools;
}

QJsonValue responseInputValue(const OpenAIResponseRequest& request) {
    if (!request.function_outputs.isEmpty()) {
        return functionOutputInputItems(request.function_outputs);
    }
    if (!request.attachments.isEmpty()) {
        return attachmentInput(request);
    }
    return request.input;
}

QJsonObject reasoningObject(const OpenAIResponseRequest& request) {
    const QString effort = request.reasoning_effort.trimmed().toLower();
    if (effort.isEmpty() || effort == QLatin1String("none")) {
        return {};
    }
    QJsonObject reasoning;
    reasoning[QStringLiteral("effort")] = effort;
    return reasoning;
}

QJsonArray enabledToolDefinitions(const OpenAIResponseRequest& request) {
    QJsonArray tools;
    if (request.enable_web_search) {
        tools.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("web_search_preview")}});
    }
    if (request.enable_local_tools) {
        for (const auto& tool : localToolDefinitions()) {
            tools.append(tool);
        }
    }
    return tools;
}

void appendResponseOptionalFields(QJsonObject* root, const OpenAIResponseRequest& request) {
    const QString previous_response_id = request.previous_response_id.trimmed();
    const QString safety_identifier = request.safety_identifier.trimmed();
    const QString truncation = request.truncation.trimmed();
    const QString prompt_cache_key = request.prompt_cache_key.trimmed();
    if (!previous_response_id.isEmpty()) {
        root->insert(QStringLiteral("previous_response_id"), previous_response_id);
    }
    if (!safety_identifier.isEmpty()) {
        root->insert(QStringLiteral("safety_identifier"), safety_identifier);
    }
    if (!truncation.isEmpty()) {
        root->insert(QStringLiteral("truncation"), truncation);
    }
    if (!prompt_cache_key.isEmpty()) {
        root->insert(QStringLiteral("prompt_cache_key"), prompt_cache_key);
    }
    const QJsonObject reasoning = reasoningObject(request);
    if (!reasoning.isEmpty()) {
        root->insert(QStringLiteral("reasoning"), reasoning);
    }
}

void appendResponseTools(QJsonObject* root, const OpenAIResponseRequest& request) {
    const QJsonArray tools = enabledToolDefinitions(request);
    if (tools.isEmpty()) {
        return;
    }
    root->insert(QStringLiteral("tools"), tools);
    root->insert(QStringLiteral("parallel_tool_calls"), false);
}

}  // namespace

OpenAIResponsesClient::OpenAIResponsesClient(QObject* parent) : QObject(parent) {
    // Fail closed on redirects: only follow a redirect that stays on the same origin as
    // api.openai.com. A cross-origin redirect from an untrusted/MITM peer could otherwise
    // carry the Authorization bearer token and the prompt/attachment payload to another
    // host. The OpenAI JSON endpoints do not redirect cross-origin.
    m_network_manager.setRedirectPolicy(QNetworkRequest::SameOriginRedirectPolicy);
}

OpenAIResponsesClient::~OpenAIResponsesClient() {
    cancel();
    cancelInputTokenCount();
}

void OpenAIResponsesClient::createResponse(const OpenAIResponseRequest& request) {
    if (isBusy()) {
        Q_EMIT requestFailed(QStringLiteral("OpenAI request already running"));
        return;
    }
    if (!hasUsableApiKey(request.api_key)) {
        Q_EMIT requestFailed(QStringLiteral("OpenAI API key is missing or too short"));
        return;
    }
    if (request.model.trimmed().isEmpty()) {
        Q_EMIT requestFailed(QStringLiteral("OpenAI model is empty"));
        return;
    }
    if (request.input.trimmed().isEmpty() && request.function_outputs.isEmpty()) {
        Q_EMIT requestFailed(QStringLiteral("Message is empty"));
        return;
    }

    auto network_request = buildRequest(QStringLiteral("/v1/responses"), request.api_key);
    const QByteArray payload = buildResponsePayload(request);
    auto* reply = m_network_manager.post(network_request, payload);
    setCurrentReply(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        handleCreateFinished(reply);
    });
    Q_EMIT requestStarted();
}

void OpenAIResponsesClient::countInputTokens(const OpenAIResponseRequest& request,
                                             const QString& request_id) {
    if (!hasUsableApiKey(request.api_key)) {
        Q_EMIT inputTokenCountFailed(request_id,
                                     QStringLiteral("OpenAI API key is missing or too short"));
        return;
    }
    if (request.model.trimmed().isEmpty()) {
        Q_EMIT inputTokenCountFailed(request_id, QStringLiteral("OpenAI model is empty"));
        return;
    }

    cancelInputTokenCount();
    auto network_request = buildRequest(QStringLiteral("/v1/responses/input_tokens"),
                                        request.api_key);
    auto count_request = request;
    count_request.safety_identifier.clear();
    // Strip generation-only policy fields the count does not depend on: truncation is
    // applied server-side during generation, and prompt_cache_key only affects cache
    // routing. Keeping the /v1/responses/input_tokens payload minimal avoids any
    // endpoint field-compat risk for what is purely a telemetry count.
    count_request.truncation.clear();
    count_request.prompt_cache_key.clear();
    const QByteArray payload = buildResponsePayload(count_request);
    auto* reply = m_network_manager.post(network_request, payload);
    m_input_tokens_reply = reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply, request_id]() {
        handleInputTokenCountFinished(reply, request_id);
    });
}

void OpenAIResponsesClient::listModels(const QString& api_key) {
    if (isBusy()) {
        Q_EMIT requestFailed(QStringLiteral("OpenAI request already running"));
        return;
    }
    if (!hasUsableApiKey(api_key)) {
        Q_EMIT requestFailed(QStringLiteral("OpenAI API key is missing or too short"));
        return;
    }

    auto request = buildRequest(QStringLiteral("/v1/models"), api_key);
    auto* reply = m_network_manager.get(request);
    setCurrentReply(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        handleModelsFinished(reply);
    });
    Q_EMIT requestStarted();
}

void OpenAIResponsesClient::cancel() {
    if (!m_current_reply) {
        return;
    }

    auto* reply = m_current_reply;
    m_current_reply = nullptr;
    // Detach our finished handler first: abort() fires finished synchronously, which
    // would otherwise run handleCreateFinished and emit requestFailed("Operation
    // canceled") plus a second requestFinished(), making the UI record the user's
    // deliberate stop as a failed request.
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
    Q_EMIT requestFinished();
}

OpenAIResponseResult OpenAIResponsesClient::parseResponseObject(const QByteArray& data,
                                                                QString* error_message) {
    if (error_message) {
        error_message->clear();
    }

    const auto root = responseRootObject(data, error_message);
    if (!root.has_value()) {
        return {};
    }

    OpenAIResponseResult result;
    result.id = root->value(QStringLiteral("id")).toString();
    result.raw_json = QString::fromUtf8(data);
    result.usage = TokenUsageTracker::fromJson(root->value(QStringLiteral("usage")).toObject());

    QStringList output_parts;
    const auto output = root->value(QStringLiteral("output")).toArray();
    for (const auto& output_value : output) {
        appendOutputItem(&result, &output_parts, output_value.toObject());
    }

    result.output_text = output_parts.join(QStringLiteral("\n"));
    if (result.output_text.isEmpty()) {
        result.output_text = root->value(QStringLiteral("output_text")).toString();
    }

    // A non-success status (e.g. "incomplete" from max_output_tokens) can carry a
    // truncated tool call with invalid JSON arguments; treating it as complete would let
    // a partial/dangerous call (e.g. a half-formed delete path) reach dispatch. Fail
    // closed and drop any partial output/calls, regardless of whether the caller asked
    // for an error string, so a nullptr error_message can never turn a terminal-error
    // response into a usable partial result.
    const QString terminal = terminalResponseError(*root);
    if (!terminal.isEmpty()) {
        if (error_message) {
            *error_message = terminal;
        }
        return {};
    }

    if (result.output_text.trimmed().isEmpty() && result.function_calls.isEmpty() &&
        error_message) {
        *error_message = QStringLiteral("OpenAI response had no output text");
    }
    return result;
}

namespace {

// Report a models-list failure through the optional out-parameter and fail closed.
[[nodiscard]] std::optional<QJsonArray> modelsListError(QString* error_message,
                                                        const QString& text) {
    if (error_message) {
        *error_message = text;
    }
    return std::nullopt;
}

// Validate a models-list body and return its "data" array, or std::nullopt with a
// non-empty error describing why the enumeration cannot be trusted: an oversized body,
// malformed JSON, an API error envelope, or a missing/wrong-typed "data".
[[nodiscard]] std::optional<QJsonArray> modelsListDataArray(const QByteArray& data,
                                                            QString* error_message) {
    if (data.size() > kMaxResponseBodyBytes) {
        return modelsListError(error_message,
                               QStringLiteral("OpenAI model list body exceeds the size limit"));
    }

    QJsonParseError parse_error;
    const auto doc = QJsonDocument::fromJson(data, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        return modelsListError(error_message,
                               firstNonEmptyError(parse_error.errorString(),
                                                  QStringLiteral("Invalid model list JSON")));
    }

    const QString api_error = OpenAIResponsesClient::extractApiError(data);
    if (!api_error.isEmpty()) {
        return modelsListError(error_message, api_error);
    }

    // Fail closed on a malformed envelope: a missing or wrong-typed "data" must not be
    // silently reported as a successful (empty) model enumeration.
    const QJsonValue data_value = doc.object().value(QStringLiteral("data"));
    if (!data_value.isArray()) {
        return modelsListError(error_message,
                               QStringLiteral("OpenAI model list is missing a data array"));
    }
    return data_value.toArray();
}

}  // namespace

QStringList OpenAIResponsesClient::parseModelsList(const QByteArray& data, QString* error_message) {
    if (error_message) {
        error_message->clear();
    }

    const auto data_array = modelsListDataArray(data, error_message);
    if (!data_array.has_value()) {
        return {};
    }

    QStringList models;
    for (const auto& value : *data_array) {
        const QString id = value.toObject().value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) {
            models.append(id);
        }
    }
    models.removeDuplicates();
    std::sort(models.begin(), models.end(), [](const QString& left, const QString& right) {
        return left.localeAwareCompare(right) < 0;
    });
    return models;
}

namespace {

// Format the error message carried by a non-empty OpenAI error object.
[[nodiscard]] QString describeApiErrorObject(const QJsonObject& error) {
    const QString message = error.value(QStringLiteral("message")).toString();
    const QString type = error.value(QStringLiteral("type")).toString();
    if (!message.isEmpty() && !type.isEmpty()) {
        return QStringLiteral("%1: %2").arg(type, message);
    }
    if (!message.isEmpty()) {
        return message;
    }
    // An error object carrying a type/code but no human-readable message must still fail
    // closed, not be read as a successful response with no error.
    const QString code = error.value(QStringLiteral("code")).toString();
    if (!type.isEmpty() || !code.isEmpty()) {
        return QStringLiteral("OpenAI API error: %1").arg(type.isEmpty() ? code : type);
    }
    return QStringLiteral("OpenAI API returned an error");
}

}  // namespace

QString OpenAIResponsesClient::extractApiError(const QByteArray& data) {
    if (data.size() > kMaxResponseBodyBytes) {
        return {};
    }
    QJsonParseError parse_error;
    const auto doc = QJsonDocument::fromJson(data, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        return {};
    }

    const QJsonValue error_value = doc.object().value(QStringLiteral("error"));
    // Absent or explicit null: no error. Anything else present is an error signal that
    // must fail closed rather than be read as success, even when malformed.
    if (error_value.isUndefined() || error_value.isNull()) {
        return {};
    }
    if (!error_value.isObject()) {
        return QStringLiteral("OpenAI API returned an error");
    }

    const QJsonObject error = error_value.toObject();
    if (error.isEmpty()) {
        return {};
    }
    return describeApiErrorObject(error);
}

qint64 OpenAIResponsesClient::parseInputTokenCountObject(const QByteArray& data,
                                                         QString* error_message) {
    if (error_message) {
        error_message->clear();
    }
    const auto root = responseRootObject(data, error_message);
    if (!root.has_value()) {
        return -1;
    }
    const auto value = root->value(QStringLiteral("input_tokens"));
    if (!value.isDouble()) {
        if (error_message) {
            *error_message = QStringLiteral("OpenAI input token count missing input_tokens");
        }
        return -1;
    }
    // Guard the double->qint64 cast: a non-finite or out-of-range magnitude is UB to cast.
    const double raw = value.toDouble();
    if (!std::isfinite(raw) || raw < 0.0) {
        if (error_message) {
            *error_message = QStringLiteral("OpenAI input token count was negative or non-finite");
        }
        return -1;
    }
    if (raw >= static_cast<double>(std::numeric_limits<qint64>::max())) {
        if (error_message) {
            *error_message = QStringLiteral("OpenAI input token count was out of range");
        }
        return -1;
    }
    return static_cast<qint64>(raw);
}

bool OpenAIResponsesClient::hasUsableApiKey(const QString& api_key) noexcept {
    return api_key.trimmed().size() >= kMinimumOpenAiApiKeyLength;
}

QNetworkRequest OpenAIResponsesClient::buildRequest(const QString& path,
                                                    const QString& api_key) const {
    QUrl url(QString::fromLatin1(kOpenAiBaseUrl) + path);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("SAK-Utility/%1 AI").arg(QString::fromLatin1(get_version())));
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(api_key).toUtf8());
    request.setTransferTimeout(kOpenAiTimeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::SameOriginRedirectPolicy);

    QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
    ssl.setProtocol(QSsl::TlsV1_2OrLater);
    request.setSslConfiguration(ssl);
    return request;
}

QByteArray OpenAIResponsesClient::buildResponsePayloadForTesting(
    const OpenAIResponseRequest& request) {
    OpenAIResponsesClient client;
    return client.buildResponsePayload(request);
}

QByteArray OpenAIResponsesClient::buildResponsePayload(const OpenAIResponseRequest& request) const {
    QJsonObject root;
    root[QStringLiteral("model")] = request.model.trimmed();
    root[QStringLiteral("instructions")] = request.instructions;
    root[QStringLiteral("input")] = responseInputValue(request);
    appendResponseOptionalFields(&root, request);
    appendResponseTools(&root, request);

    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

void OpenAIResponsesClient::setCurrentReply(QNetworkReply* reply) {
    // Both callers pass a reply straight from QNetworkAccessManager::post/get, which always
    // returns a live object (errors arrive on the reply, never as a null pointer).
    Q_ASSERT(reply);
    m_current_reply = reply;
}

void OpenAIResponsesClient::clearCurrentReply(QNetworkReply* reply) {
    if (m_current_reply == reply) {
        m_current_reply = nullptr;
    }
}

void OpenAIResponsesClient::handleCreateFinished(QNetworkReply* reply) {
    clearCurrentReply(reply);
    reply->deleteLater();

    const QByteArray body = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        const QString api_error = extractApiError(body);
        Q_EMIT requestFailed(firstNonEmptyError(api_error, reply->errorString()));
        Q_EMIT requestFinished();
        return;
    }

    QString parse_error;
    const auto result = parseResponseObject(body, &parse_error);
    if (!parse_error.isEmpty()) {
        Q_EMIT requestFailed(parse_error);
        Q_EMIT requestFinished();
        return;
    }

    Q_EMIT responseReady(result);
    Q_EMIT requestFinished();
}

void OpenAIResponsesClient::handleModelsFinished(QNetworkReply* reply) {
    clearCurrentReply(reply);
    reply->deleteLater();

    const QByteArray body = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        const QString api_error = extractApiError(body);
        Q_EMIT requestFailed(firstNonEmptyError(api_error, reply->errorString()));
        Q_EMIT requestFinished();
        return;
    }

    QString parse_error;
    const auto models = parseModelsList(body, &parse_error);
    if (!parse_error.isEmpty()) {
        Q_EMIT requestFailed(parse_error);
        Q_EMIT requestFinished();
        return;
    }

    Q_EMIT modelsReady(models);
    Q_EMIT requestFinished();
}

void OpenAIResponsesClient::handleInputTokenCountFinished(QNetworkReply* reply,
                                                          const QString& request_id) {
    if (m_input_tokens_reply == reply) {
        m_input_tokens_reply = nullptr;
    }
    reply->deleteLater();

    const QByteArray body = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        const QString api_error = extractApiError(body);
        Q_EMIT inputTokenCountFailed(request_id,
                                     firstNonEmptyError(api_error, reply->errorString()));
        return;
    }

    QString parse_error;
    const qint64 tokens = parseInputTokenCountObject(body, &parse_error);
    if (!parse_error.isEmpty()) {
        Q_EMIT inputTokenCountFailed(request_id, parse_error);
        return;
    }
    Q_EMIT inputTokenCountReady(request_id, tokens);
}

void OpenAIResponsesClient::cancelInputTokenCount() {
    if (!m_input_tokens_reply) {
        return;
    }
    auto* reply = m_input_tokens_reply;
    m_input_tokens_reply = nullptr;
    reply->abort();
    reply->deleteLater();
}

}  // namespace sak::ai

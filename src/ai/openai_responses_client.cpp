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
#include <optional>

namespace sak::ai {

namespace {

constexpr char kOpenAiBaseUrl[] = "https://api.openai.com";
constexpr int kOpenAiTimeoutMs = 120'000;

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
        integerParameter(QStringLiteral("Timeout in seconds, from 5 to 3600."), 5, 3600);
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
        integerParameter(QStringLiteral("Timeout in seconds, from 5 to 3600."), 5, 3600);
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
        5,
        7200);
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

    QJsonObject arguments;
    arguments[QStringLiteral("type")] = QStringLiteral("object");
    arguments[QStringLiteral("description")] = QStringLiteral(
        "Structured provider arguments. For win32_mcp_call use "
        "{tool_name, tool_arguments, timeout_ms}. For Context7 docs_query use "
        "{libraryId} after resolving a library id.");
    arguments[QStringLiteral("additionalProperties")] = true;

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
    properties[QStringLiteral("max_results")] =
        integerParameter(QStringLiteral("Maximum hits to return, from 1 to 100."), 1, 100);

    return functionTool(QStringLiteral("sak_session_search"),
                        QStringLiteral("Search saved S.A.K. AI session transcript and command "
                                       "indexes. Use this when debugging prior runs, finding QA "
                                       "failures, locating tool-loop evidence, or comparing a "
                                       "current issue against earlier sessions."),
                        properties,
                        QJsonArray{QStringLiteral("query"), QStringLiteral("max_results")});
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
    return tools;
}

}  // namespace

OpenAIResponsesClient::OpenAIResponsesClient(QObject* parent) : QObject(parent) {
    m_network_manager.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
}

OpenAIResponsesClient::~OpenAIResponsesClient() {
    cancel();
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

    if (result.output_text.trimmed().isEmpty() && result.function_calls.isEmpty() &&
        error_message) {
        *error_message = QStringLiteral("OpenAI response had no output text");
    }
    return result;
}

QStringList OpenAIResponsesClient::parseModelsList(const QByteArray& data, QString* error_message) {
    if (error_message) {
        error_message->clear();
    }

    QJsonParseError parse_error;
    const auto doc = QJsonDocument::fromJson(data, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error_message) {
            *error_message = firstNonEmptyError(parse_error.errorString(),
                                                QStringLiteral("Invalid model list JSON"));
        }
        return {};
    }

    const QString api_error = extractApiError(data);
    if (!api_error.isEmpty()) {
        if (error_message) {
            *error_message = api_error;
        }
        return {};
    }

    QStringList models;
    const auto data_array = doc.object().value(QStringLiteral("data")).toArray();
    for (const auto& value : data_array) {
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

QString OpenAIResponsesClient::extractApiError(const QByteArray& data) {
    QJsonParseError parse_error;
    const auto doc = QJsonDocument::fromJson(data, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        return {};
    }

    const auto error = doc.object().value(QStringLiteral("error")).toObject();
    if (error.isEmpty()) {
        return {};
    }

    const QString message = error.value(QStringLiteral("message")).toString();
    const QString type = error.value(QStringLiteral("type")).toString();
    if (!message.isEmpty() && !type.isEmpty()) {
        return QStringLiteral("%1: %2").arg(type, message);
    }
    return message;
}

bool OpenAIResponsesClient::hasUsableApiKey(const QString& api_key) noexcept {
    return api_key.trimmed().size() >= 20;
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
                         QNetworkRequest::NoLessSafeRedirectPolicy);

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
    if (!request.function_outputs.isEmpty()) {
        root[QStringLiteral("input")] = functionOutputInputItems(request.function_outputs);
    } else if (!request.attachments.isEmpty()) {
        root[QStringLiteral("input")] = attachmentInput(request);
    } else {
        root[QStringLiteral("input")] = request.input;
    }

    if (!request.previous_response_id.trimmed().isEmpty()) {
        root[QStringLiteral("previous_response_id")] = request.previous_response_id.trimmed();
    }

    const QString effort = request.reasoning_effort.trimmed().toLower();
    if (!effort.isEmpty() && effort != QLatin1String("none")) {
        QJsonObject reasoning;
        reasoning[QStringLiteral("effort")] = effort;
        root[QStringLiteral("reasoning")] = reasoning;
    }

    QJsonArray tools;
    if (request.enable_web_search) {
        QJsonObject web_search;
        web_search[QStringLiteral("type")] = QStringLiteral("web_search_preview");
        tools.append(web_search);
    }
    if (request.enable_local_tools) {
        for (const auto& tool : localToolDefinitions()) {
            tools.append(tool);
        }
    }
    if (!tools.isEmpty()) {
        root[QStringLiteral("tools")] = tools;
        root[QStringLiteral("parallel_tool_calls")] = false;
    }

    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

void OpenAIResponsesClient::setCurrentReply(QNetworkReply* reply) {
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

}  // namespace sak::ai

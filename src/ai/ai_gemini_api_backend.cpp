// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_gemini_api_backend.h"

#include "sak/ai/openai_responses_client.h"

#include <QJsonDocument>
#include <QNetworkReply>
#include <QUrlQuery>

namespace sak::ai {

namespace {

constexpr char kGeminiBaseUrl[] = "https://generativelanguage.googleapis.com/v1beta";
constexpr int kModelListPageSize = 1000;
constexpr int kLowThinkingBudget = 1024;
constexpr int kHighThinkingBudget = 24'576;

QString bareModelId(const QString& model) {
    QString id = model.trimmed();
    if (id.startsWith(QLatin1String("models/"))) {
        id = id.mid(QStringLiteral("models/").size());
    }
    return id;
}

bool isGeminiThree(const QString& model) {
    return bareModelId(model).startsWith(QLatin1String("gemini-3"));
}

QJsonObject textPart(const QString& text) {
    return QJsonObject{{QStringLiteral("text"), text}};
}

QJsonObject inlineDataPart(const DataUrlParts& parts) {
    return QJsonObject{{QStringLiteral("inlineData"),
                        QJsonObject{{QStringLiteral("mimeType"), parts.mime_type},
                                    {QStringLiteral("data"), parts.base64_data}}}};
}

QJsonObject attachmentPart(const OpenAIInputAttachment& attachment) {
    if (attachment.type == OpenAIInputAttachment::Type::Text) {
        return textPart(attachmentText(attachment));
    }
    const auto parts = attachmentBinary(attachment);
    if (!parts) {
        return textPart(unsupportedAttachmentNote(attachment));
    }
    if (parts->mime_type.startsWith(QLatin1String("text/"))) {
        const QString text =
            QString::fromUtf8(QByteArray::fromBase64(parts->base64_data.toLatin1()));
        return textPart(QStringLiteral("File: %1\n%2").arg(attachment.filename, text));
    }
    const bool supported = parts->mime_type.startsWith(QLatin1String("image/")) ||
                           parts->mime_type == QLatin1String("application/pdf");
    return supported ? inlineDataPart(*parts) : textPart(unsupportedAttachmentNote(attachment));
}

QJsonArray functionCallParts(const QJsonObject& model_content) {
    QJsonArray calls;
    for (const auto& value : model_content.value(QStringLiteral("parts")).toArray()) {
        const QJsonObject call = value.toObject().value(QStringLiteral("functionCall")).toObject();
        if (!call.isEmpty()) {
            calls.append(call);
        }
    }
    return calls;
}

/// Resolve the function name a tool output answers, by real or synthetic id.
QJsonObject matchingCall(const QJsonArray& calls, const QString& call_id) {
    for (int index = 0; index < calls.size(); ++index) {
        const QJsonObject call = calls.at(index).toObject();
        const QString id = call.value(QStringLiteral("id")).toString();
        if (id == call_id ||
            (id.isEmpty() && GeminiApiBackend::syntheticCallId(index) == call_id)) {
            return call;
        }
    }
    return {};
}

QJsonObject functionResponseBody(const QString& output) {
    const QJsonDocument doc = QJsonDocument::fromJson(output.toUtf8());
    if (doc.isObject()) {
        return doc.object();
    }
    return QJsonObject{{QStringLiteral("output"), output}};
}

std::optional<QJsonObject> functionResponseContent(const QJsonArray& history,
                                                   const QVector<OpenAIFunctionOutput>& outputs,
                                                   QString* error_message) {
    const QJsonArray calls = history.isEmpty() ? QJsonArray{}
                                               : functionCallParts(history.last().toObject());
    QJsonArray parts;
    for (const auto& output : outputs) {
        const QJsonObject call = matchingCall(calls, output.call_id);
        if (call.isEmpty()) {
            if (error_message) {
                *error_message = QStringLiteral("Gemini tool result %1 has no matching call")
                                     .arg(output.call_id);
            }
            return std::nullopt;
        }
        QJsonObject response{{QStringLiteral("name"), call.value(QStringLiteral("name"))},
                             {QStringLiteral("response"), functionResponseBody(output.output)}};
        if (call.contains(QStringLiteral("id"))) {
            response[QStringLiteral("id")] = call.value(QStringLiteral("id"));
        }
        parts.append(QJsonObject{{QStringLiteral("functionResponse"), response}});
    }
    return QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                       {QStringLiteral("parts"), parts}};
}

QJsonObject thinkingConfig(const OpenAIResponseRequest& request) {
    const QString effort = request.reasoning_effort.trimmed().toLower();
    if (effort != QLatin1String("low") && effort != QLatin1String("medium") &&
        effort != QLatin1String("high")) {
        return {};
    }
    if (isGeminiThree(request.model)) {
        return QJsonObject{{QStringLiteral("thinkingLevel"), effort.toUpper()}};
    }
    if (effort == QLatin1String("medium")) {
        return {};  // Gemini 2.5 default is dynamic thinking.
    }
    return QJsonObject{{QStringLiteral("thinkingBudget"),
                        effort == QLatin1String("low") ? kLowThinkingBudget : kHighThinkingBudget}};
}

QJsonArray functionDeclarations() {
    QJsonArray declarations;
    for (const auto& value : OpenAIResponsesClient::localToolDefinitionsForProviders()) {
        const QJsonObject tool = value.toObject();
        declarations.append(QJsonObject{
            {QStringLiteral("name"), tool.value(QStringLiteral("name"))},
            {QStringLiteral("description"), tool.value(QStringLiteral("description"))},
            {QStringLiteral("parametersJsonSchema"), tool.value(QStringLiteral("parameters"))}});
    }
    return declarations;
}

/// Gemini 3 can mix Google Search grounding with function calling; older
/// models reject the combination, so local tools win there.
void appendTools(QJsonObject* payload, const OpenAIResponseRequest& request) {
    const bool gemini_three = isGeminiThree(request.model);
    const bool search = request.enable_web_search && (gemini_three || !request.enable_local_tools);
    QJsonArray tools;
    if (request.enable_local_tools) {
        tools.append(QJsonObject{{QStringLiteral("functionDeclarations"), functionDeclarations()}});
    }
    if (search) {
        tools.append(QJsonObject{{QStringLiteral("googleSearch"), QJsonObject{}}});
    }
    if (tools.isEmpty()) {
        return;
    }
    payload->insert(QStringLiteral("tools"), tools);
    if (request.enable_local_tools && search) {
        payload->insert(QStringLiteral("toolConfig"),
                        QJsonObject{{QStringLiteral("includeServerSideToolInvocations"), true}});
    }
}

void appendPart(const QJsonObject& part, int* call_ordinal, OpenAIResponseResult* result) {
    const QJsonObject call = part.value(QStringLiteral("functionCall")).toObject();
    if (!call.isEmpty()) {
        OpenAIFunctionCall function_call;
        const QString id = call.value(QStringLiteral("id")).toString();
        function_call.call_id = id.isEmpty() ? GeminiApiBackend::syntheticCallId(*call_ordinal)
                                             : id;
        function_call.name = call.value(QStringLiteral("name")).toString();
        function_call.arguments_json =
            QString::fromUtf8(QJsonDocument(call.value(QStringLiteral("args")).toObject())
                                  .toJson(QJsonDocument::Compact));
        ++*call_ordinal;
        result->function_calls.append(function_call);
        return;
    }
    if (!part.value(QStringLiteral("thought")).toBool(false)) {
        result->output_text += part.value(QStringLiteral("text")).toString();
    }
}

void appendGroundingCitations(const QJsonObject& candidate, OpenAIResponseResult* result) {
    const QJsonObject grounding = candidate.value(QStringLiteral("groundingMetadata")).toObject();
    for (const auto& value : grounding.value(QStringLiteral("groundingChunks")).toArray()) {
        const QJsonObject web = value.toObject().value(QStringLiteral("web")).toObject();
        OpenAIUrlCitation citation;
        citation.url = web.value(QStringLiteral("uri")).toString();
        citation.title = web.value(QStringLiteral("title")).toString();
        if (!citation.url.isEmpty()) {
            result->citations.append(citation);
        }
    }
}

TokenUsage usageFromMetadata(const QJsonObject& usage) {
    const auto count = [&usage](const char* key) {
        return static_cast<qint64>(usage.value(QLatin1String(key)).toDouble(0.0));
    };
    TokenUsage tokens;
    tokens.input_tokens = count("promptTokenCount") + count("toolUsePromptTokenCount");
    tokens.cached_input_tokens = count("cachedContentTokenCount");
    tokens.reasoning_tokens = count("thoughtsTokenCount");
    tokens.output_tokens = count("candidatesTokenCount") + tokens.reasoning_tokens;
    tokens.total_tokens = count("totalTokenCount");
    if (tokens.total_tokens == 0) {
        tokens.total_tokens = tokens.input_tokens + tokens.output_tokens;
    }
    return tokens;
}

QString blockedMessage(const QJsonObject& root) {
    const QString reason = root.value(QStringLiteral("promptFeedback"))
                               .toObject()
                               .value(QStringLiteral("blockReason"))
                               .toString();
    return reason.isEmpty() ? QString()
                            : QStringLiteral("Gemini blocked the prompt (%1)").arg(reason);
}

bool isFailureFinish(const QString& finish_reason) {
    static const QStringList kOk{QStringLiteral("STOP"),
                                 QStringLiteral("MAX_TOKENS"),
                                 QStringLiteral("FINISH_REASON_UNSPECIFIED"),
                                 QString()};
    return !kOk.contains(finish_reason);
}

}  // namespace

GeminiApiBackend::GeminiApiBackend(QObject* parent)
    : AiChatBackend(parent), m_history(QStringLiteral("sak_gem_")) {
    m_network.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
}

GeminiApiBackend::~GeminiApiBackend() {
    cancel();
    if (m_count_reply) {
        m_count_reply->abort();
    }
}

QString GeminiApiBackend::syntheticCallId(int ordinal) {
    return QStringLiteral("sak_gcall_%1").arg(ordinal);
}

std::optional<QJsonArray> GeminiApiBackend::turnContents(const OpenAIResponseRequest& request,
                                                         const std::optional<QJsonArray>& history,
                                                         QString* error_message) {
    QJsonArray contents = history.value_or(QJsonArray{});
    if (request.function_outputs.isEmpty()) {
        contents.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                    {QStringLiteral("parts"), userParts(request)}});
        return contents;
    }
    if (!history) {
        if (error_message) {
            *error_message = QStringLiteral(
                "Gemini conversation state expired before tool results were returned; start a new "
                "message");
        }
        return std::nullopt;
    }
    const auto response =
        functionResponseContent(contents, request.function_outputs, error_message);
    if (!response) {
        return std::nullopt;
    }
    contents.append(*response);
    return contents;
}

QJsonArray GeminiApiBackend::userParts(const OpenAIResponseRequest& request) {
    QJsonArray parts;
    for (const auto& attachment : request.attachments) {
        parts.append(attachmentPart(attachment));
    }
    if (!request.input.trimmed().isEmpty() || parts.isEmpty()) {
        parts.append(textPart(request.input));
    }
    return parts;
}

QJsonObject GeminiApiBackend::buildPayload(const OpenAIResponseRequest& request,
                                           const QJsonArray& contents) {
    QJsonObject payload;
    payload[QStringLiteral("contents")] = contents;
    if (!request.instructions.trimmed().isEmpty()) {
        payload[QStringLiteral("systemInstruction")] =
            QJsonObject{{QStringLiteral("parts"), QJsonArray{textPart(request.instructions)}}};
    }
    appendTools(&payload, request);
    const QJsonObject thinking = thinkingConfig(request);
    if (!thinking.isEmpty()) {
        payload[QStringLiteral("generationConfig")] =
            QJsonObject{{QStringLiteral("thinkingConfig"), thinking}};
    }
    return payload;
}

std::optional<GeminiApiBackend::ParsedCandidate> GeminiApiBackend::parseResponse(
    const QByteArray& data, QString* error_message) {
    const auto root = parseJsonObject(data, error_message);
    if (!root) {
        return std::nullopt;
    }
    QString failure = extractApiError(data);
    if (failure.isEmpty()) {
        failure = blockedMessage(*root);
    }
    const QJsonObject candidate =
        root->value(QStringLiteral("candidates")).toArray().first().toObject();
    if (failure.isEmpty() && candidate.isEmpty()) {
        failure = QStringLiteral("Gemini response had no candidates");
    }
    if (!failure.isEmpty()) {
        if (error_message) {
            *error_message = failure;
        }
        return std::nullopt;
    }
    ParsedCandidate parsed;
    parsed.model_content = candidate.value(QStringLiteral("content")).toObject();
    parsed.model_content[QStringLiteral("role")] = QStringLiteral("model");
    parsed.finish_reason = candidate.value(QStringLiteral("finishReason")).toString();
    parsed.result.usage =
        usageFromMetadata(root->value(QStringLiteral("usageMetadata")).toObject());
    int call_ordinal = 0;
    for (const auto& value : parsed.model_content.value(QStringLiteral("parts")).toArray()) {
        appendPart(value.toObject(), &call_ordinal, &parsed.result);
    }
    appendGroundingCitations(candidate, &parsed.result);
    return parsed;
}

QStringList GeminiApiBackend::parseModels(const QByteArray& data, QString* error_message) {
    const auto root = parseJsonObject(data, error_message);
    const QString api_error = extractApiError(data);
    if (!root || !api_error.isEmpty()) {
        if (error_message && !api_error.isEmpty()) {
            *error_message = api_error;
        }
        return {};
    }
    QStringList models;
    for (const auto& value : root->value(QStringLiteral("models")).toArray()) {
        const QJsonObject model = value.toObject();
        const QJsonArray methods =
            model.value(QStringLiteral("supportedGenerationMethods")).toArray();
        if (methods.contains(QStringLiteral("generateContent"))) {
            models.append(bareModelId(model.value(QStringLiteral("name")).toString()));
        }
    }
    models.removeAll(QString());
    models.removeDuplicates();
    return models;
}

QString GeminiApiBackend::extractApiError(const QByteArray& data) {
    const auto root = parseJsonObject(data, nullptr);
    if (!root) {
        return {};
    }
    const QJsonObject error = root->value(QStringLiteral("error")).toObject();
    if (error.isEmpty()) {
        return {};
    }
    const QString status = error.value(QStringLiteral("status")).toString();
    const QString message = error.value(QStringLiteral("message")).toString();
    return status.isEmpty() ? message : QStringLiteral("%1: %2").arg(status, message);
}

QNetworkRequest GeminiApiBackend::apiRequest(const QString& path, const QString& api_key) const {
    QNetworkRequest request = vendorJsonRequest(QUrl(QString::fromLatin1(kGeminiBaseUrl) + path),
                                                kVendorApiTimeoutMs);
    request.setRawHeader("x-goog-api-key", api_key.trimmed().toUtf8());
    return request;
}

void GeminiApiBackend::createResponse(const OpenAIResponseRequest& request) {
    QString error;
    if (isBusy()) {
        error = tr("Gemini request already running");
    } else if (!hasUsableApiKey(provider(), request.api_key)) {
        error = tr("Gemini API key is missing or too short");
    } else if (bareModelId(request.model).isEmpty()) {
        error = tr("Gemini model is empty");
    } else if (request.input.trimmed().isEmpty() && request.function_outputs.isEmpty()) {
        error = tr("Message is empty");
    }
    const auto contents =
        error.isEmpty()
            ? turnContents(request, m_history.lookup(request.previous_response_id), &error)
            : std::nullopt;
    if (!contents) {
        Q_EMIT requestFailed(error);
        return;
    }
    m_pending_contents = *contents;
    const QString path =
        QStringLiteral("/models/%1:generateContent").arg(bareModelId(request.model));
    auto* reply = m_network.post(apiRequest(path, request.api_key),
                                 compactJson(buildPayload(request, *contents)));
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { handleTurnFinished(reply); });
    Q_EMIT requestStarted();
}

void GeminiApiBackend::handleTurnFinished(QNetworkReply* reply) {
    reply->deleteLater();
    if (m_reply != reply) {
        return;
    }
    m_reply.clear();
    const QByteArray body = reply->readAll();
    QString error;
    const auto parsed = parseResponse(body, &error);
    if (reply->error() != QNetworkReply::NoError || !parsed) {
        finishTurn(error.isEmpty() ? reply->errorString() : error);
        return;
    }
    if (isFailureFinish(parsed->finish_reason) && parsed->result.function_calls.isEmpty() &&
        parsed->result.output_text.trimmed().isEmpty()) {
        finishTurn(tr("Gemini stopped without an answer (%1)").arg(parsed->finish_reason));
        return;
    }
    OpenAIResponseResult result = parsed->result;
    if (parsed->finish_reason == QLatin1String("MAX_TOKENS")) {
        result.output_text += tr("\n\n[Response truncated at the output token limit]");
    }
    QJsonArray contents = m_pending_contents;
    contents.append(parsed->model_content);
    result.id = m_history.store(contents);
    result.raw_json = QString::fromUtf8(compactJson(parsed->model_content));
    m_pending_contents = {};
    Q_EMIT responseReady(result);
    Q_EMIT requestFinished();
}

void GeminiApiBackend::finishTurn(const QString& error_message) {
    m_pending_contents = {};
    Q_EMIT requestFailed(error_message);
    Q_EMIT requestFinished();
}

void GeminiApiBackend::countInputTokens(const OpenAIResponseRequest& request,
                                        const QString& request_id) {
    if (!hasUsableApiKey(provider(), request.api_key) || bareModelId(request.model).isEmpty()) {
        Q_EMIT inputTokenCountFailed(request_id, tr("Gemini API key or model missing"));
        return;
    }
    QString error;
    const auto contents =
        turnContents(request, m_history.lookup(request.previous_response_id), &error);
    if (!contents) {
        Q_EMIT inputTokenCountFailed(request_id, error);
        return;
    }
    const QString model = bareModelId(request.model);
    QJsonObject generate = buildPayload(request, *contents);
    generate.remove(QStringLiteral("generationConfig"));
    generate[QStringLiteral("model")] = QStringLiteral("models/%1").arg(model);
    if (m_count_reply) {
        m_count_reply->abort();
    }
    auto* reply = m_network.post(
        apiRequest(QStringLiteral("/models/%1:countTokens").arg(model), request.api_key),
        compactJson(QJsonObject{{QStringLiteral("generateContentRequest"), generate}}));
    m_count_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, request_id]() {
        handleCountFinished(reply, request_id);
    });
}

void GeminiApiBackend::handleCountFinished(QNetworkReply* reply, const QString& request_id) {
    reply->deleteLater();
    if (m_count_reply == reply) {
        m_count_reply.clear();
    }
    const QByteArray body = reply->readAll();
    const QString api_error = extractApiError(body);
    const auto root = parseJsonObject(body, nullptr);
    const QJsonValue tokens = root ? root->value(QStringLiteral("totalTokens")) : QJsonValue{};
    if (reply->error() != QNetworkReply::NoError || !api_error.isEmpty() || !tokens.isDouble()) {
        Q_EMIT inputTokenCountFailed(request_id,
                                     api_error.isEmpty() ? reply->errorString() : api_error);
        return;
    }
    Q_EMIT inputTokenCountReady(request_id, static_cast<qint64>(tokens.toDouble()));
}

void GeminiApiBackend::listModels(const QString& credential) {
    if (isBusy()) {
        Q_EMIT requestFailed(tr("Gemini request already running"));
        return;
    }
    if (!hasUsableApiKey(provider(), credential)) {
        Q_EMIT requestFailed(tr("Gemini API key is missing or too short"));
        return;
    }
    QNetworkRequest request = apiRequest(QStringLiteral("/models"), credential);
    QUrl url = request.url();
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("pageSize"), QString::number(kModelListPageSize));
    url.setQuery(query);
    request.setUrl(url);
    request.setTransferTimeout(kVendorApiShortTimeoutMs);
    auto* reply = m_network.get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        handleModelsFinished(reply);
    });
    Q_EMIT requestStarted();
}

void GeminiApiBackend::handleModelsFinished(QNetworkReply* reply) {
    reply->deleteLater();
    if (m_reply == reply) {
        m_reply.clear();
    }
    QString error;
    const QStringList models = parseModels(reply->readAll(), &error);
    if (reply->error() != QNetworkReply::NoError || !error.isEmpty()) {
        Q_EMIT requestFailed(error.isEmpty() ? reply->errorString() : error);
    } else {
        Q_EMIT modelsReady(models);
    }
    Q_EMIT requestFinished();
}

void GeminiApiBackend::cancel() {
    if (!m_reply) {
        return;
    }
    QNetworkReply* reply = m_reply;
    m_reply.clear();
    m_pending_contents = {};
    reply->abort();
    Q_EMIT requestFinished();
}

void GeminiApiBackend::resetConversation() {
    m_history.clear();
}

}  // namespace sak::ai

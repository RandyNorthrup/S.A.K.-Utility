// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_anthropic_api_backend.h"

#include "sak/ai/openai_responses_client.h"

#include <QJsonDocument>
#include <QNetworkReply>
#include <QUrlQuery>

#include <algorithm>

namespace sak::ai {

namespace {

constexpr char kAnthropicBaseUrl[] = "https://api.anthropic.com";
constexpr char kAnthropicVersion[] = "2023-06-01";
constexpr char kFallbackBeta[] = "server-side-fallback-2026-07-01";
constexpr int kAnthropicMaxTokens = 16'000;
constexpr int kMaxPauseResumes = 4;
constexpr int kModelListPageSize = 1000;

/// Models that take `thinking: {type: "adaptive"}` and `output_config.effort`.
const QStringList& adaptiveModelPrefixes() {
    static const QStringList kPrefixes{QStringLiteral("claude-fable"),
                                       QStringLiteral("claude-mythos"),
                                       QStringLiteral("claude-opus-5"),
                                       QStringLiteral("claude-sonnet-5"),
                                       QStringLiteral("claude-opus-4-6"),
                                       QStringLiteral("claude-opus-4-7"),
                                       QStringLiteral("claude-opus-4-8"),
                                       QStringLiteral("claude-sonnet-4-6")};
    return kPrefixes;
}

bool startsWithAny(const QString& value, const QStringList& prefixes) {
    return std::any_of(prefixes.cbegin(), prefixes.cend(), [&](const QString& prefix) {
        return value.startsWith(prefix);
    });
}

bool supportsAdaptiveThinking(const QString& model) {
    return startsWithAny(model.trimmed().toLower(), adaptiveModelPrefixes());
}

/// Server-side refusal fallbacks are recommended for the Fable and Opus 5 tiers.
bool wantsRefusalFallback(const QString& model) {
    return startsWithAny(model.trimmed().toLower(),
                         {QStringLiteral("claude-fable"),
                          QStringLiteral("claude-mythos"),
                          QStringLiteral("claude-opus-5")});
}

QString normalizedEffort(const QString& effort) {
    static const QStringList kEfforts{QStringLiteral("low"),
                                      QStringLiteral("medium"),
                                      QStringLiteral("high"),
                                      QStringLiteral("xhigh"),
                                      QStringLiteral("max")};
    const QString value = effort.trimmed().toLower();
    return kEfforts.contains(value) ? value : QString();
}

QJsonObject textBlock(const QString& text) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                       {QStringLiteral("text"), text}};
}

QJsonObject base64Source(const DataUrlParts& parts) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("base64")},
                       {QStringLiteral("media_type"), parts.mime_type},
                       {QStringLiteral("data"), parts.base64_data}};
}

QJsonObject imageBlock(const OpenAIInputAttachment& attachment) {
    const QString url = attachment.image_url.trimmed();
    if (url.startsWith(QLatin1String("https://"))) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("image")},
                           {QStringLiteral("source"),
                            QJsonObject{{QStringLiteral("type"), QStringLiteral("url")},
                                        {QStringLiteral("url"), url}}}};
    }
    const auto parts = attachmentBinary(attachment);
    if (!parts || !parts->mime_type.startsWith(QLatin1String("image/"))) {
        return textBlock(unsupportedAttachmentNote(attachment));
    }
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("image")},
                       {QStringLiteral("source"), base64Source(*parts)}};
}

QJsonObject fileBlock(const OpenAIInputAttachment& attachment) {
    const auto parts = attachmentBinary(attachment);
    if (!parts) {
        return textBlock(unsupportedAttachmentNote(attachment));
    }
    if (parts->mime_type == QLatin1String("application/pdf")) {
        QJsonObject block{{QStringLiteral("type"), QStringLiteral("document")},
                          {QStringLiteral("source"), base64Source(*parts)}};
        if (!attachment.filename.trimmed().isEmpty()) {
            block[QStringLiteral("title")] = attachment.filename.trimmed();
        }
        return block;
    }
    if (parts->mime_type.startsWith(QLatin1String("text/"))) {
        const QString text =
            QString::fromUtf8(QByteArray::fromBase64(parts->base64_data.toLatin1()));
        return textBlock(QStringLiteral("File: %1\n%2").arg(attachment.filename, text));
    }
    if (parts->mime_type.startsWith(QLatin1String("image/"))) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("image")},
                           {QStringLiteral("source"), base64Source(*parts)}};
    }
    return textBlock(unsupportedAttachmentNote(attachment));
}

QJsonObject attachmentBlock(const OpenAIInputAttachment& attachment) {
    switch (attachment.type) {
    case OpenAIInputAttachment::Type::Image:
        return imageBlock(attachment);
    case OpenAIInputAttachment::Type::File:
        return fileBlock(attachment);
    case OpenAIInputAttachment::Type::Text:
    default:
        return textBlock(attachmentText(attachment));
    }
}

QJsonArray toolResultContent(const QVector<OpenAIFunctionOutput>& outputs) {
    QJsonArray content;
    for (const auto& output : outputs) {
        content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("tool_result")},
                                   {QStringLiteral("tool_use_id"), output.call_id},
                                   {QStringLiteral("content"), output.output}});
    }
    return content;
}

QJsonObject webSearchTool(const QString& model) {
    const QString type = supportsAdaptiveThinking(model) ? QStringLiteral("web_search_20260209")
                                                         : QStringLiteral("web_search_20250305");
    return QJsonObject{{QStringLiteral("type"), type},
                       {QStringLiteral("name"), QStringLiteral("web_search")}};
}

QJsonObject anthropicToolFromOpenAi(const QJsonObject& tool) {
    // `strict` is deliberately not forwarded: the shared schemas use numeric
    // bounds that strict tool use does not accept, and the panel validates
    // arguments itself before running anything.
    return QJsonObject{{QStringLiteral("name"), tool.value(QStringLiteral("name"))},
                       {QStringLiteral("description"), tool.value(QStringLiteral("description"))},
                       {QStringLiteral("input_schema"), tool.value(QStringLiteral("parameters"))}};
}

void appendReasoningFields(QJsonObject* payload, const OpenAIResponseRequest& request) {
    if (!supportsAdaptiveThinking(request.model)) {
        return;
    }
    payload->insert(QStringLiteral("thinking"),
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("adaptive")}});
    const QString effort = normalizedEffort(request.reasoning_effort);
    if (!effort.isEmpty()) {
        payload->insert(QStringLiteral("output_config"),
                        QJsonObject{{QStringLiteral("effort"), effort}});
    }
}

void appendCitations(const QJsonObject& block,
                     qsizetype start,
                     qsizetype end,
                     QVector<OpenAIUrlCitation>* citations) {
    for (const auto& value : block.value(QStringLiteral("citations")).toArray()) {
        const QJsonObject citation = value.toObject();
        if (citation.value(QStringLiteral("type")).toString() !=
            QLatin1String("web_search_result_location")) {
            continue;
        }
        OpenAIUrlCitation url_citation;
        url_citation.url = citation.value(QStringLiteral("url")).toString();
        url_citation.title = citation.value(QStringLiteral("title")).toString();
        url_citation.start_index = static_cast<int>(start);
        url_citation.end_index = static_cast<int>(end);
        if (!url_citation.url.isEmpty()) {
            citations->append(url_citation);
        }
    }
}

void appendContentBlock(const QJsonObject& block, OpenAIResponseResult* result) {
    const QString type = block.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("text")) {
        const qsizetype start = result->output_text.size();
        result->output_text += block.value(QStringLiteral("text")).toString();
        appendCitations(block, start, result->output_text.size(), &result->citations);
        return;
    }
    if (type == QLatin1String("tool_use")) {
        OpenAIFunctionCall call;
        call.call_id = block.value(QStringLiteral("id")).toString();
        call.name = block.value(QStringLiteral("name")).toString();
        call.arguments_json =
            QString::fromUtf8(QJsonDocument(block.value(QStringLiteral("input")).toObject())
                                  .toJson(QJsonDocument::Compact));
        if (!call.call_id.isEmpty() && !call.name.isEmpty()) {
            result->function_calls.append(call);
        }
    }
}

TokenUsage usageFromJson(const QJsonObject& usage) {
    TokenUsage tokens;
    const auto count = [&usage](const char* key) {
        return static_cast<qint64>(usage.value(QLatin1String(key)).toDouble(0.0));
    };
    tokens.cached_input_tokens = count("cache_read_input_tokens");
    tokens.input_tokens = count("input_tokens") + tokens.cached_input_tokens +
                          count("cache_creation_input_tokens");
    tokens.output_tokens = count("output_tokens");
    tokens.total_tokens = tokens.input_tokens + tokens.output_tokens;
    return tokens;
}

void addUsage(TokenUsage* total, const TokenUsage& part) {
    total->input_tokens += part.input_tokens;
    total->cached_input_tokens += part.cached_input_tokens;
    total->output_tokens += part.output_tokens;
    total->reasoning_tokens += part.reasoning_tokens;
    total->total_tokens += part.total_tokens;
}

QString refusalDetail(const QJsonObject& root) {
    const QJsonObject details = root.value(QStringLiteral("stop_details")).toObject();
    const QString category = details.value(QStringLiteral("category")).toString();
    const QString explanation = details.value(QStringLiteral("explanation")).toString();
    if (category.isEmpty()) {
        return explanation;
    }
    return explanation.isEmpty() ? category : QStringLiteral("%1: %2").arg(category, explanation);
}

}  // namespace

AnthropicApiBackend::AnthropicApiBackend(QObject* parent)
    : AiChatBackend(parent), m_history(QStringLiteral("sak_anth_")) {
    m_network.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
}

AnthropicApiBackend::~AnthropicApiBackend() {
    cancel();
    if (m_count_reply) {
        m_count_reply->abort();
    }
}

std::optional<QJsonArray> AnthropicApiBackend::turnMessages(
    const OpenAIResponseRequest& request,
    const std::optional<QJsonArray>& history,
    QString* error_message) {
    QJsonArray messages = history.value_or(QJsonArray{});
    if (!request.function_outputs.isEmpty()) {
        if (!history) {
            if (error_message) {
                *error_message = QStringLiteral(
                    "Claude conversation state expired before tool results were returned; "
                    "start a new message");
            }
            return std::nullopt;
        }
        messages.append(
            QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                        {QStringLiteral("content"), toolResultContent(request.function_outputs)}});
        return messages;
    }
    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                {QStringLiteral("content"), userContent(request)}});
    return messages;
}

QJsonArray AnthropicApiBackend::userContent(const OpenAIResponseRequest& request) {
    QJsonArray content;
    for (const auto& attachment : request.attachments) {
        content.append(attachmentBlock(attachment));
    }
    // Documents and images go before the question, per Anthropic guidance.
    if (!request.input.trimmed().isEmpty() || content.isEmpty()) {
        content.append(textBlock(request.input));
    }
    return content;
}

QJsonArray AnthropicApiBackend::toolDefinitions(const OpenAIResponseRequest& request) {
    QJsonArray tools;
    if (request.enable_web_search) {
        tools.append(webSearchTool(request.model));
    }
    if (request.enable_local_tools) {
        for (const auto& value : OpenAIResponsesClient::localToolDefinitionsForProviders()) {
            tools.append(anthropicToolFromOpenAi(value.toObject()));
        }
    }
    return tools;
}

QJsonObject AnthropicApiBackend::buildPayload(const OpenAIResponseRequest& request,
                                              const QJsonArray& messages) {
    QJsonObject payload;
    payload[QStringLiteral("model")] = request.model.trimmed();
    payload[QStringLiteral("max_tokens")] = kAnthropicMaxTokens;
    payload[QStringLiteral("messages")] = messages;
    // Top-level automatic prompt caching: tool loops resend the same prefix.
    payload[QStringLiteral("cache_control")] =
        QJsonObject{{QStringLiteral("type"), QStringLiteral("ephemeral")}};
    if (!request.instructions.trimmed().isEmpty()) {
        payload[QStringLiteral("system")] = request.instructions;
    }
    appendReasoningFields(&payload, request);
    const QJsonArray tools = toolDefinitions(request);
    if (!tools.isEmpty()) {
        payload[QStringLiteral("tools")] = tools;
        // Mirror the OpenAI path: the panel runs one local tool at a time.
        payload[QStringLiteral("tool_choice")] =
            QJsonObject{{QStringLiteral("type"), QStringLiteral("auto")},
                        {QStringLiteral("disable_parallel_tool_use"), true}};
    }
    if (!request.safety_identifier.trimmed().isEmpty()) {
        payload[QStringLiteral("metadata")] =
            QJsonObject{{QStringLiteral("user_id"), request.safety_identifier.trimmed()}};
    }
    if (wantsRefusalFallback(request.model)) {
        payload[QStringLiteral("fallbacks")] = QStringLiteral("default");
    }
    return payload;
}

std::optional<AnthropicApiBackend::ParsedMessage> AnthropicApiBackend::parseMessage(
    const QByteArray& data, QString* error_message) {
    const auto root = parseJsonObject(data, error_message);
    if (!root) {
        return std::nullopt;
    }
    const QString api_error = extractApiError(data);
    if (!api_error.isEmpty()) {
        if (error_message) {
            *error_message = api_error;
        }
        return std::nullopt;
    }
    ParsedMessage parsed;
    parsed.assistant_content = root->value(QStringLiteral("content")).toArray();
    parsed.stop_reason = root->value(QStringLiteral("stop_reason")).toString();
    parsed.result.usage = usageFromJson(root->value(QStringLiteral("usage")).toObject());
    for (const auto& value : std::as_const(parsed.assistant_content)) {
        appendContentBlock(value.toObject(), &parsed.result);
    }
    if (parsed.stop_reason == QLatin1String("refusal")) {
        parsed.refusal_detail = refusalDetail(*root);
    }
    return parsed;
}

QStringList AnthropicApiBackend::parseModels(const QByteArray& data, QString* error_message) {
    const auto root = parseJsonObject(data, error_message);
    if (!root) {
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
    for (const auto& value : root->value(QStringLiteral("data")).toArray()) {
        const QString id = value.toObject().value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) {
            models.append(id);
        }
    }
    models.removeDuplicates();
    return models;
}

QString AnthropicApiBackend::extractApiError(const QByteArray& data) {
    const auto root = parseJsonObject(data, nullptr);
    if (!root || root->value(QStringLiteral("type")).toString() != QLatin1String("error")) {
        return {};
    }
    const QJsonObject error = root->value(QStringLiteral("error")).toObject();
    const QString type = error.value(QStringLiteral("type")).toString();
    const QString message = error.value(QStringLiteral("message")).toString();
    if (type.isEmpty()) {
        return message;
    }
    return message.isEmpty() ? type : QStringLiteral("%1: %2").arg(type, message);
}

QNetworkRequest AnthropicApiBackend::apiRequest(const QString& path,
                                                const QString& api_key,
                                                const QString& model) const {
    QNetworkRequest request = vendorJsonRequest(QUrl(QString::fromLatin1(kAnthropicBaseUrl) + path),
                                                kVendorApiTimeoutMs);
    request.setRawHeader("x-api-key", api_key.trimmed().toUtf8());
    request.setRawHeader("anthropic-version", kAnthropicVersion);
    if (wantsRefusalFallback(model) && path == QLatin1String("/v1/messages")) {
        request.setRawHeader("anthropic-beta", kFallbackBeta);
    }
    return request;
}

void AnthropicApiBackend::createResponse(const OpenAIResponseRequest& request) {
    if (isBusy()) {
        Q_EMIT requestFailed(tr("Claude request already running"));
        return;
    }
    if (!hasUsableApiKey(provider(), request.api_key)) {
        Q_EMIT requestFailed(tr("Anthropic API key is missing or too short"));
        return;
    }
    if (request.model.trimmed().isEmpty()) {
        Q_EMIT requestFailed(tr("Claude model is empty"));
        return;
    }
    if (request.input.trimmed().isEmpty() && request.function_outputs.isEmpty()) {
        Q_EMIT requestFailed(tr("Message is empty"));
        return;
    }
    QString error;
    const auto messages =
        turnMessages(request, m_history.lookup(request.previous_response_id), &error);
    if (!messages) {
        Q_EMIT requestFailed(error);
        return;
    }
    m_turn = PendingTurn{};
    m_turn.api_key = request.api_key;
    m_turn.messages = *messages;
    m_turn.payload = buildPayload(request, *messages);
    postTurn();
    Q_EMIT requestStarted();
}

void AnthropicApiBackend::postTurn() {
    m_turn.payload[QStringLiteral("messages")] = m_turn.messages;
    const QString model = m_turn.payload.value(QStringLiteral("model")).toString();
    auto* reply = m_network.post(apiRequest(QStringLiteral("/v1/messages"), m_turn.api_key, model),
                                 compactJson(m_turn.payload));
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { handleTurnFinished(reply); });
}

void AnthropicApiBackend::handleTurnFinished(QNetworkReply* reply) {
    reply->deleteLater();
    if (m_reply != reply) {
        return;
    }
    m_reply.clear();
    const QByteArray body = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        const QString api_error = extractApiError(body);
        failTurn(api_error.isEmpty() ? reply->errorString() : api_error);
        return;
    }
    QString error;
    const auto parsed = parseMessage(body, &error);
    if (!parsed) {
        failTurn(error);
        return;
    }
    addUsage(&m_turn.usage, parsed->result.usage);
    if (parsed->stop_reason == QLatin1String("pause_turn") &&
        m_turn.pause_resumes < kMaxPauseResumes) {
        // Server tools hit their iteration cap; resend so the API resumes.
        ++m_turn.pause_resumes;
        m_turn.partial_text += parsed->result.output_text;
        m_turn.partial_citations += parsed->result.citations;
        m_turn.messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                                           {QStringLiteral("content"), parsed->assistant_content}});
        postTurn();
        return;
    }
    completeTurn(*parsed);
}

void AnthropicApiBackend::completeTurn(const ParsedMessage& parsed) {
    OpenAIResponseResult result = parsed.result;
    result.output_text = m_turn.partial_text + result.output_text;
    result.citations = m_turn.partial_citations + result.citations;
    result.usage = m_turn.usage;
    if (parsed.stop_reason == QLatin1String("refusal") && result.output_text.trimmed().isEmpty()) {
        failTurn(tr("Claude declined this request (%1)").arg(parsed.refusal_detail));
        return;
    }
    if (parsed.stop_reason == QLatin1String("max_tokens") && !result.function_calls.isEmpty()) {
        failTurn(
            tr("Claude hit the output limit while writing a tool call; try a narrower request"));
        return;
    }
    if (parsed.stop_reason == QLatin1String("max_tokens")) {
        result.output_text += tr("\n\n[Response truncated at the output token limit]");
    }
    QJsonArray messages = m_turn.messages;
    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                                {QStringLiteral("content"), parsed.assistant_content}});
    result.id = m_history.store(messages);
    result.raw_json = QString::fromUtf8(
        compactJson(QJsonObject{{QStringLiteral("content"), parsed.assistant_content},
                                {QStringLiteral("stop_reason"), parsed.stop_reason}}));
    m_turn = PendingTurn{};
    Q_EMIT responseReady(result);
    Q_EMIT requestFinished();
}

void AnthropicApiBackend::failTurn(const QString& error_message) {
    m_turn = PendingTurn{};
    Q_EMIT requestFailed(error_message);
    Q_EMIT requestFinished();
}

void AnthropicApiBackend::countInputTokens(const OpenAIResponseRequest& request,
                                           const QString& request_id) {
    if (!hasUsableApiKey(provider(), request.api_key) || request.model.trimmed().isEmpty()) {
        Q_EMIT inputTokenCountFailed(request_id, tr("Anthropic API key or model missing"));
        return;
    }
    QString error;
    const auto messages =
        turnMessages(request, m_history.lookup(request.previous_response_id), &error);
    if (!messages) {
        Q_EMIT inputTokenCountFailed(request_id, error);
        return;
    }
    const QJsonObject full = buildPayload(request, *messages);
    QJsonObject payload;
    for (const auto* key : {"model", "messages", "system", "tools", "thinking"}) {
        const QString name = QString::fromLatin1(key);
        if (full.contains(name)) {
            payload[name] = full.value(name);
        }
    }
    if (m_count_reply) {
        m_count_reply->abort();
    }
    auto* reply = m_network.post(
        apiRequest(QStringLiteral("/v1/messages/count_tokens"), request.api_key, request.model),
        compactJson(payload));
    m_count_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, request_id]() {
        handleCountFinished(reply, request_id);
    });
}

void AnthropicApiBackend::handleCountFinished(QNetworkReply* reply, const QString& request_id) {
    reply->deleteLater();
    if (m_count_reply == reply) {
        m_count_reply.clear();
    }
    const QByteArray body = reply->readAll();
    const QString api_error = extractApiError(body);
    if (reply->error() != QNetworkReply::NoError || !api_error.isEmpty()) {
        Q_EMIT inputTokenCountFailed(request_id,
                                     api_error.isEmpty() ? reply->errorString() : api_error);
        return;
    }
    QString error;
    const auto root = parseJsonObject(body, &error);
    const QJsonValue tokens = root ? root->value(QStringLiteral("input_tokens")) : QJsonValue{};
    if (!tokens.isDouble()) {
        Q_EMIT inputTokenCountFailed(request_id,
                                     error.isEmpty() ? tr("Token count missing") : error);
        return;
    }
    Q_EMIT inputTokenCountReady(request_id, static_cast<qint64>(tokens.toDouble()));
}

void AnthropicApiBackend::listModels(const QString& credential) {
    if (isBusy()) {
        Q_EMIT requestFailed(tr("Claude request already running"));
        return;
    }
    if (!hasUsableApiKey(provider(), credential)) {
        Q_EMIT requestFailed(tr("Anthropic API key is missing or too short"));
        return;
    }
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("limit"), QString::number(kModelListPageSize));
    QNetworkRequest request = apiRequest(QStringLiteral("/v1/models"), credential, QString());
    QUrl url = request.url();
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

void AnthropicApiBackend::handleModelsFinished(QNetworkReply* reply) {
    reply->deleteLater();
    if (m_reply == reply) {
        m_reply.clear();
    }
    const QByteArray body = reply->readAll();
    QString error;
    const QStringList models = parseModels(body, &error);
    if (reply->error() != QNetworkReply::NoError || !error.isEmpty()) {
        Q_EMIT requestFailed(error.isEmpty() ? reply->errorString() : error);
    } else {
        Q_EMIT modelsReady(models);
    }
    Q_EMIT requestFinished();
}

void AnthropicApiBackend::cancel() {
    if (!m_reply) {
        return;
    }
    QNetworkReply* reply = m_reply;
    m_reply.clear();
    m_turn = PendingTurn{};
    reply->abort();
    Q_EMIT requestFinished();
}

void AnthropicApiBackend::resetConversation() {
    m_history.clear();
}

}  // namespace sak::ai

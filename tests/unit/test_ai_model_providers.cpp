// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_anthropic_api_backend.h"
#include "sak/ai/ai_api_support.h"
#include "sak/ai/ai_credential_store.h"
#include "sak/ai/ai_gemini_api_backend.h"
#include "sak/ai/ai_model_provider.h"
#include "sak/ai/ai_model_router.h"

#include <QJsonDocument>
#include <QSignalSpy>
#include <QtTest/QtTest>

using sak::ai::AiChatBackend;
using sak::ai::AnthropicApiBackend;
using sak::ai::GeminiApiBackend;
using sak::ai::ModelAuthMode;
using sak::ai::ModelProviderId;
using sak::ai::OpenAIResponseRequest;

class AiModelProviderTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void catalog_hasAllFourProvidersWithRoundTripKeys();
    void catalog_museAgentRedistributionIsUnverified();
    void apiKeyValidation_isPerProvider();
    void modelProviderForModel_guessesFamilies();
    void credentialPaths_keepLegacyOpenAiFileName();
    void historyCache_storesLooksUpAndEvicts();
    void parseDataUrl_splitsMimeAndPayload();
    void attachmentBinary_infersMimeFromFilename();
    void anthropicPayload_adaptiveThinkingEffortAndTools();
    void anthropicPayload_haikuSkipsThinkingAndUsesBasicWebSearch();
    void anthropicPayload_toolResultsRequireHistory();
    void anthropicPayload_imageAndPdfAttachments();
    void anthropicParse_extractsTextCitationsToolCallsAndUsage();
    void anthropicParse_reportsApiErrorsAndRefusals();
    void anthropicParseModels_readsIds();
    void router_forwardsOnlyActiveBackendAndCachesInstances();
    void geminiPayload_geminiThreeCombinesSearchAndFunctions();
    void geminiPayload_geminiTwoPrefersLocalToolsOverSearch();
    void geminiParse_keepsSignaturesAndAssignsSyntheticIds();
    void geminiTurn_functionResponsesMatchCallsByIdAndOrdinal();
    void geminiParse_reportsBlocksAndApiErrors();
    void geminiParseModels_filtersGenerateContent();
};

namespace {

class FakeBackend : public AiChatBackend {
public:
    FakeBackend(ModelProviderId provider, ModelAuthMode mode, QObject* parent)
        : AiChatBackend(parent), m_provider(provider), m_mode(mode) {}

    [[nodiscard]] ModelProviderId provider() const override { return m_provider; }
    [[nodiscard]] ModelAuthMode authMode() const override { return m_mode; }
    void createResponse(const OpenAIResponseRequest& request) override {
        last_model = request.model;
        sak::ai::OpenAIResponseResult result;
        result.output_text = sak::ai::modelProviderKey(m_provider);
        Q_EMIT responseReady(result);
    }
    void countInputTokens(const OpenAIResponseRequest&, const QString& request_id) override {
        Q_EMIT inputTokenCountReady(request_id, static_cast<qint64>(m_provider) + 1);
    }
    void listModels(const QString&) override { Q_EMIT modelsReady({last_model}); }
    void cancel() override { ++cancel_count; }
    [[nodiscard]] bool isBusy() const override { return false; }

    QString last_model;
    int cancel_count{0};

private:
    ModelProviderId m_provider;
    ModelAuthMode m_mode;
};

QJsonObject anthropicMessageFixture() {
    const QByteArray json = R"({
      "id": "msg_1", "type": "message", "role": "assistant", "model": "claude-opus-5",
      "stop_reason": "tool_use",
      "content": [
        {"type": "thinking", "thinking": "", "signature": "sig"},
        {"type": "text", "text": "Checking disk. "},
        {"type": "text", "text": "Source says so.",
         "citations": [{"type": "web_search_result_location", "url": "https://example.com/a",
                        "title": "Example", "cited_text": "x", "encrypted_index": "e"}]},
        {"type": "tool_use", "id": "toolu_1", "name": "run_powershell",
         "input": {"command": "Get-Volume", "timeout_seconds": 30, "requires_admin": false}}
      ],
      "usage": {"input_tokens": 100, "cache_read_input_tokens": 40,
                "cache_creation_input_tokens": 10, "output_tokens": 25}
    })";
    return QJsonDocument::fromJson(json).object();
}

OpenAIResponseRequest anthropicRequest(const QString& model) {
    OpenAIResponseRequest request;
    request.api_key = QStringLiteral("sk-ant-api03-") + QString(40, QChar(u'x'));
    request.model = model;
    request.instructions = QStringLiteral("You are S.A.K.");
    request.input = QStringLiteral("Check my disks");
    request.reasoning_effort = QStringLiteral("medium");
    request.safety_identifier = QStringLiteral("sid_1");
    request.enable_web_search = true;
    request.enable_local_tools = true;
    return request;
}

QJsonObject toolNamed(const QJsonArray& tools, const QString& name) {
    for (const auto& value : tools) {
        if (value.toObject().value(QStringLiteral("name")).toString() == name) {
            return value.toObject();
        }
    }
    return {};
}

}  // namespace


void AiModelProviderTests::catalog_hasAllFourProvidersWithRoundTripKeys() {
    const auto& providers = sak::ai::modelProviders();
    QCOMPARE(providers.size(), 4);
    for (const auto& info : providers) {
        QCOMPARE(sak::ai::modelProviderFromKey(info.key).value(), info.id);
        QVERIFY(!info.api_models.isEmpty());
        QVERIFY(!info.agent_name.isEmpty());
    }
    QCOMPARE(sak::ai::modelAuthModeFromKey(QStringLiteral("subscription")).value(),
             ModelAuthMode::Subscription);
    QVERIFY(!sak::ai::modelProviderFromKey(QStringLiteral("bogus")).has_value());
}

void AiModelProviderTests::catalog_museAgentRedistributionIsUnverified() {
    QVERIFY(!sak::ai::modelProviderInfo(ModelProviderId::Muse).agent_redistribution_verified);
    QVERIFY(sak::ai::modelProviderInfo(ModelProviderId::OpenAI).agent_redistribution_verified);
}

void AiModelProviderTests::apiKeyValidation_isPerProvider() {
    QVERIFY(sak::ai::hasUsableApiKey(ModelProviderId::OpenAI, QString(24, QChar(u'a'))));
    QVERIFY(!sak::ai::hasUsableApiKey(ModelProviderId::Anthropic, QString(24, QChar(u'a'))));
    QVERIFY(
        !sak::ai::hasUsableApiKey(ModelProviderId::OpenAI, QStringLiteral("has space in key 123")));
    QVERIFY(sak::ai::hasUsableApiKey(ModelProviderId::Muse,
                                     QStringLiteral("LLM|123456789|abcdefghijklmnop")));
}

void AiModelProviderTests::modelProviderForModel_guessesFamilies() {
    QCOMPARE(sak::ai::modelProviderForModel(QStringLiteral("claude-opus-5")).value(),
             ModelProviderId::Anthropic);
    QCOMPARE(sak::ai::modelProviderForModel(QStringLiteral("gemini-3-pro-preview")).value(),
             ModelProviderId::Google);
    QCOMPARE(sak::ai::modelProviderForModel(QStringLiteral("muse-spark-1.3")).value(),
             ModelProviderId::Muse);
    QCOMPARE(sak::ai::modelProviderForModel(QStringLiteral("gpt-5.5")).value(),
             ModelProviderId::OpenAI);
    QVERIFY(sak::ai::providerContextWindowTokens(QStringLiteral("claude-haiku-4-5")) > 0);
}

void AiModelProviderTests::credentialPaths_keepLegacyOpenAiFileName() {
    const sak::ai::CredentialStore store;
    QVERIFY(store.credentialFilePath().endsWith(QStringLiteral("openai_api_key.dpapi.json")));
    QCOMPARE(store.credentialFilePath(ModelProviderId::OpenAI), store.credentialFilePath());
    QVERIFY(store.credentialFilePath(ModelProviderId::Anthropic)
                .endsWith(QStringLiteral("anthropic_api_key.dpapi.json")));
    QVERIFY(store.credentialFilePath(ModelProviderId::Google) !=
            store.credentialFilePath(ModelProviderId::Muse));
}

void AiModelProviderTests::historyCache_storesLooksUpAndEvicts() {
    sak::ai::ConversationHistoryCache cache(QStringLiteral("t_"), 2);
    const QString first = cache.store(QJsonArray{1});
    const QString second = cache.store(QJsonArray{2});
    QVERIFY(first.startsWith(QStringLiteral("t_")));
    QVERIFY(cache.owns(first));
    QCOMPARE(cache.lookup(second).value(), QJsonArray{2});
    const QString third = cache.store(QJsonArray{3});
    QVERIFY(!cache.lookup(first).has_value());
    QVERIFY(cache.lookup(third).has_value());
    QCOMPARE(cache.size(), 2);
    cache.clear();
    QVERIFY(!cache.lookup(third).has_value());
}

void AiModelProviderTests::parseDataUrl_splitsMimeAndPayload() {
    const auto parts = sak::ai::parseDataUrl(QStringLiteral("data:image/png;base64,QUJD"));
    QVERIFY(parts.has_value());
    QCOMPARE(parts->mime_type, QStringLiteral("image/png"));
    QCOMPARE(parts->base64_data, QStringLiteral("QUJD"));
    QVERIFY(!sak::ai::parseDataUrl(QStringLiteral("https://example.com/a.png")).has_value());
    QVERIFY(!sak::ai::parseDataUrl(QStringLiteral("data:text/plain,hello")).has_value());
}

void AiModelProviderTests::attachmentBinary_infersMimeFromFilename() {
    sak::ai::OpenAIInputAttachment attachment;
    attachment.type = sak::ai::OpenAIInputAttachment::Type::File;
    attachment.filename = QStringLiteral("report.PDF");
    attachment.file_data = QStringLiteral("JVBERi0=");
    const auto parts = sak::ai::attachmentBinary(attachment);
    QVERIFY(parts.has_value());
    QCOMPARE(parts->mime_type, QStringLiteral("application/pdf"));
    attachment.filename = QStringLiteral("notes.docx");
    QVERIFY(!sak::ai::attachmentBinary(attachment).has_value());
}

void AiModelProviderTests::anthropicPayload_adaptiveThinkingEffortAndTools() {
    const OpenAIResponseRequest request = anthropicRequest(QStringLiteral("claude-opus-5"));
    QString error;
    const auto messages = AnthropicApiBackend::turnMessages(request, std::nullopt, &error);
    QVERIFY2(messages.has_value(), qPrintable(error));
    const QJsonObject payload = AnthropicApiBackend::buildPayload(request, *messages);

    QCOMPARE(payload.value(QStringLiteral("model")).toString(), QStringLiteral("claude-opus-5"));
    QVERIFY(payload.value(QStringLiteral("max_tokens")).toInt() > 0);
    QCOMPARE(payload.value(QStringLiteral("system")).toString(), request.instructions);
    QCOMPARE(payload.value(QStringLiteral("thinking")).toObject().value(QStringLiteral("type")),
             QJsonValue(QStringLiteral("adaptive")));
    QCOMPARE(
        payload.value(QStringLiteral("output_config")).toObject().value(QStringLiteral("effort")),
        QJsonValue(QStringLiteral("medium")));
    QCOMPARE(payload.value(QStringLiteral("fallbacks")).toString(), QStringLiteral("default"));
    QCOMPARE(payload.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("user_id")),
             QJsonValue(QStringLiteral("sid_1")));

    const QJsonArray tools = payload.value(QStringLiteral("tools")).toArray();
    QCOMPARE(toolNamed(tools, QStringLiteral("web_search")).value(QStringLiteral("type")),
             QJsonValue(QStringLiteral("web_search_20260209")));
    const QJsonObject shell = toolNamed(tools, QStringLiteral("run_powershell"));
    QVERIFY(shell.value(QStringLiteral("input_schema")).isObject());
    QVERIFY(!shell.contains(QStringLiteral("strict")));
    QVERIFY(!shell.contains(QStringLiteral("parameters")));
    QCOMPARE(payload.value(QStringLiteral("tool_choice"))
                 .toObject()
                 .value(QStringLiteral("disable_parallel_tool_use")),
             QJsonValue(true));

    const QJsonObject user = messages->last().toObject();
    QCOMPARE(user.value(QStringLiteral("role")).toString(), QStringLiteral("user"));
    QCOMPARE(user.value(QStringLiteral("content"))
                 .toArray()
                 .last()
                 .toObject()
                 .value(QStringLiteral("text")),
             QJsonValue(QStringLiteral("Check my disks")));
}

void AiModelProviderTests::anthropicPayload_haikuSkipsThinkingAndUsesBasicWebSearch() {
    OpenAIResponseRequest request = anthropicRequest(QStringLiteral("claude-haiku-4-5"));
    request.enable_local_tools = false;
    const auto messages = AnthropicApiBackend::turnMessages(request, std::nullopt, nullptr);
    const QJsonObject payload = AnthropicApiBackend::buildPayload(request, *messages);
    QVERIFY(!payload.contains(QStringLiteral("thinking")));
    QVERIFY(!payload.contains(QStringLiteral("output_config")));
    QVERIFY(!payload.contains(QStringLiteral("fallbacks")));
    const QJsonArray tools = payload.value(QStringLiteral("tools")).toArray();
    QCOMPARE(tools.size(), 1);
    QCOMPARE(tools.first().toObject().value(QStringLiteral("type")),
             QJsonValue(QStringLiteral("web_search_20250305")));
}

void AiModelProviderTests::anthropicPayload_toolResultsRequireHistory() {
    OpenAIResponseRequest request = anthropicRequest(QStringLiteral("claude-sonnet-5"));
    request.input.clear();
    request.function_outputs = {{QStringLiteral("toolu_1"), QStringLiteral("{\"exit_code\":0}")}};
    QString error;
    QVERIFY(!AnthropicApiBackend::turnMessages(request, std::nullopt, &error).has_value());
    QVERIFY(error.contains(QStringLiteral("expired")));

    QJsonArray history{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                   {QStringLiteral("content"), QStringLiteral("hi")}},
                       QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                                   {QStringLiteral("content"), QJsonArray{}}}};
    const auto messages = AnthropicApiBackend::turnMessages(request, history, &error);
    QVERIFY(messages.has_value());
    QCOMPARE(messages->size(), 3);
    const QJsonObject result =
        messages->last().toObject().value(QStringLiteral("content")).toArray().first().toObject();
    QCOMPARE(result.value(QStringLiteral("type")).toString(), QStringLiteral("tool_result"));
    QCOMPARE(result.value(QStringLiteral("tool_use_id")).toString(), QStringLiteral("toolu_1"));
}

void AiModelProviderTests::anthropicPayload_imageAndPdfAttachments() {
    OpenAIResponseRequest request = anthropicRequest(QStringLiteral("claude-opus-5"));
    sak::ai::OpenAIInputAttachment image;
    image.type = sak::ai::OpenAIInputAttachment::Type::Image;
    image.image_url = QStringLiteral("data:image/png;base64,iVBORw0K");
    sak::ai::OpenAIInputAttachment pdf;
    pdf.type = sak::ai::OpenAIInputAttachment::Type::File;
    pdf.filename = QStringLiteral("log.pdf");
    pdf.file_data = QStringLiteral("JVBERi0=");
    sak::ai::OpenAIInputAttachment docx;
    docx.type = sak::ai::OpenAIInputAttachment::Type::File;
    docx.filename = QStringLiteral("notes.docx");
    docx.file_data = QStringLiteral("UEsDBA==");
    request.attachments = {image, pdf, docx};

    const QJsonArray content = AnthropicApiBackend::userContent(request);
    QCOMPARE(content.size(), 4);
    QCOMPARE(content.at(0).toObject().value(QStringLiteral("type")).toString(),
             QStringLiteral("image"));
    QCOMPARE(content.at(1).toObject().value(QStringLiteral("type")).toString(),
             QStringLiteral("document"));
    QVERIFY(content.at(2)
                .toObject()
                .value(QStringLiteral("text"))
                .toString()
                .contains(QStringLiteral("notes.docx")));
    QCOMPARE(content.at(3).toObject().value(QStringLiteral("text")).toString(), request.input);
}

void AiModelProviderTests::anthropicParse_extractsTextCitationsToolCallsAndUsage() {
    QString error;
    const auto parsed = AnthropicApiBackend::parseMessage(
        QJsonDocument(anthropicMessageFixture()).toJson(), &error);
    QVERIFY2(parsed.has_value(), qPrintable(error));
    QCOMPARE(parsed->result.output_text, QStringLiteral("Checking disk. Source says so."));
    QCOMPARE(parsed->result.citations.size(), 1);
    QCOMPARE(parsed->result.citations.first().url, QStringLiteral("https://example.com/a"));
    QCOMPARE(parsed->result.citations.first().start_index, 15);
    QCOMPARE(parsed->result.function_calls.size(), 1);
    QCOMPARE(parsed->result.function_calls.first().call_id, QStringLiteral("toolu_1"));
    const QJsonObject args =
        QJsonDocument::fromJson(parsed->result.function_calls.first().arguments_json.toUtf8())
            .object();
    QCOMPARE(args.value(QStringLiteral("command")).toString(), QStringLiteral("Get-Volume"));
    QCOMPARE(parsed->result.usage.input_tokens, 150);
    QCOMPARE(parsed->result.usage.cached_input_tokens, 40);
    QCOMPARE(parsed->result.usage.total_tokens, 175);
    QCOMPARE(parsed->assistant_content.size(), 4);
    QCOMPARE(parsed->stop_reason, QStringLiteral("tool_use"));
}

void AiModelProviderTests::anthropicParse_reportsApiErrorsAndRefusals() {
    const QByteArray error_body =
        R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})";
    QString error;
    QVERIFY(!AnthropicApiBackend::parseMessage(error_body, &error).has_value());
    QCOMPARE(error, QStringLiteral("authentication_error: invalid x-api-key"));

    const QByteArray refusal = R"({"type":"message","content":[],"stop_reason":"refusal",
        "stop_details":{"type":"refusal","category":"cyber","explanation":"declined"},
        "usage":{"input_tokens":5,"output_tokens":0}})";
    const auto parsed = AnthropicApiBackend::parseMessage(refusal, &error);
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->refusal_detail, QStringLiteral("cyber: declined"));
}

void AiModelProviderTests::anthropicParseModels_readsIds() {
    const QByteArray body = R"({"data":[{"id":"claude-opus-5","type":"model"},
        {"id":"claude-sonnet-5","type":"model"}],"has_more":false})";
    QString error;
    const QStringList models = AnthropicApiBackend::parseModels(body, &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(models,
             QStringList({QStringLiteral("claude-opus-5"), QStringLiteral("claude-sonnet-5")}));
}

void AiModelProviderTests::router_forwardsOnlyActiveBackendAndCachesInstances() {
    int created = 0;
    sak::ai::AiModelRouter router(
        [&created](ModelProviderId provider, ModelAuthMode mode, QObject* parent) {
            ++created;
            return new FakeBackend(provider, mode, parent);
        });
    QSignalSpy responses(&router, &AiChatBackend::responseReady);

    router.setActive(ModelProviderId::Anthropic, ModelAuthMode::ApiKey);
    OpenAIResponseRequest request;
    request.model = QStringLiteral("claude-opus-5");
    router.createResponse(request);
    QCOMPARE(responses.count(), 1);
    QCOMPARE(responses.takeFirst().first().value<sak::ai::OpenAIResponseResult>().output_text,
             QStringLiteral("anthropic"));

    router.setActive(ModelProviderId::Google, ModelAuthMode::Subscription);
    router.createResponse(request);
    QCOMPARE(responses.takeFirst().first().value<sak::ai::OpenAIResponseResult>().output_text,
             QStringLiteral("google"));

    router.setActive(ModelProviderId::Anthropic, ModelAuthMode::ApiKey);
    router.createResponse(request);
    QCOMPARE(created, 2);
    QCOMPARE(router.provider(), ModelProviderId::Anthropic);

    // A backend that is not active must not leak responses into the panel.
    auto* google = dynamic_cast<FakeBackend*>(router.children().at(1));
    QVERIFY(google);
    responses.clear();
    Q_EMIT google->responseReady({});
    QCOMPARE(responses.count(), 0);

    router.cancel();
    QCOMPARE(google->cancel_count, 1);
}

void AiModelProviderTests::geminiPayload_geminiThreeCombinesSearchAndFunctions() {
    OpenAIResponseRequest request = anthropicRequest(QStringLiteral("gemini-3.1-pro-preview"));
    request.reasoning_effort = QStringLiteral("high");
    const auto contents = GeminiApiBackend::turnContents(request, std::nullopt, nullptr);
    QVERIFY(contents.has_value());
    const QJsonObject payload = GeminiApiBackend::buildPayload(request, *contents);
    const QJsonArray tools = payload.value(QStringLiteral("tools")).toArray();
    QCOMPARE(tools.size(), 2);
    const QJsonArray declarations =
        tools.at(0).toObject().value(QStringLiteral("functionDeclarations")).toArray();
    QVERIFY(!declarations.isEmpty());
    QVERIFY(declarations.first().toObject().contains(QStringLiteral("parametersJsonSchema")));
    QVERIFY(tools.at(1).toObject().contains(QStringLiteral("googleSearch")));
    QCOMPARE(payload.value(QStringLiteral("toolConfig"))
                 .toObject()
                 .value(QStringLiteral("includeServerSideToolInvocations")),
             QJsonValue(true));
    QCOMPARE(payload.value(QStringLiteral("generationConfig"))
                 .toObject()
                 .value(QStringLiteral("thinkingConfig"))
                 .toObject()
                 .value(QStringLiteral("thinkingLevel")),
             QJsonValue(QStringLiteral("HIGH")));
    QCOMPARE(payload.value(QStringLiteral("systemInstruction"))
                 .toObject()
                 .value(QStringLiteral("parts"))
                 .toArray()
                 .first()
                 .toObject()
                 .value(QStringLiteral("text")),
             QJsonValue(request.instructions));
}

void AiModelProviderTests::geminiPayload_geminiTwoPrefersLocalToolsOverSearch() {
    OpenAIResponseRequest request = anthropicRequest(QStringLiteral("gemini-2.5-pro"));
    request.reasoning_effort = QStringLiteral("low");
    const auto contents = GeminiApiBackend::turnContents(request, std::nullopt, nullptr);
    const QJsonObject payload = GeminiApiBackend::buildPayload(request, *contents);
    const QJsonArray tools = payload.value(QStringLiteral("tools")).toArray();
    QCOMPARE(tools.size(), 1);
    QVERIFY(tools.first().toObject().contains(QStringLiteral("functionDeclarations")));
    QVERIFY(!payload.contains(QStringLiteral("toolConfig")));
    QVERIFY(payload.value(QStringLiteral("generationConfig"))
                .toObject()
                .value(QStringLiteral("thinkingConfig"))
                .toObject()
                .contains(QStringLiteral("thinkingBudget")));

    request.enable_local_tools = false;
    const QJsonObject search_only = GeminiApiBackend::buildPayload(request, *contents);
    QVERIFY(search_only.value(QStringLiteral("tools"))
                .toArray()
                .first()
                .toObject()
                .contains(QStringLiteral("googleSearch")));
}

void AiModelProviderTests::geminiParse_keepsSignaturesAndAssignsSyntheticIds() {
    const QByteArray body = R"({"candidates":[{"content":{"role":"model","parts":[
        {"text":"thinking...","thought":true},
        {"text":"Let me look. "},
        {"functionCall":{"name":"run_powershell","args":{"command":"Get-Disk"}},
         "thoughtSignature":"c2ln"},
        {"functionCall":{"id":"fc_real","name":"take_screenshot","args":{"reason":"x"}}}]},
        "finishReason":"STOP",
        "groundingMetadata":{"groundingChunks":[{"web":{"uri":"https://g.co/r","title":"R"}}]}}],
        "usageMetadata":{"promptTokenCount":10,"candidatesTokenCount":5,"thoughtsTokenCount":7,
        "cachedContentTokenCount":2,"totalTokenCount":22}})";
    QString error;
    const auto parsed = GeminiApiBackend::parseResponse(body, &error);
    QVERIFY2(parsed.has_value(), qPrintable(error));
    QCOMPARE(parsed->result.output_text, QStringLiteral("Let me look. "));
    QCOMPARE(parsed->result.function_calls.size(), 2);
    QCOMPARE(parsed->result.function_calls.at(0).call_id, GeminiApiBackend::syntheticCallId(0));
    QCOMPARE(parsed->result.function_calls.at(1).call_id, QStringLiteral("fc_real"));
    QCOMPARE(parsed->result.citations.size(), 1);
    QCOMPARE(parsed->result.usage.reasoning_tokens, 7);
    QCOMPARE(parsed->result.usage.output_tokens, 12);
    QCOMPARE(parsed->result.usage.total_tokens, 22);
    const QJsonArray parts = parsed->model_content.value(QStringLiteral("parts")).toArray();
    QCOMPARE(parts.at(2).toObject().value(QStringLiteral("thoughtSignature")).toString(),
             QStringLiteral("c2ln"));
}

void AiModelProviderTests::geminiTurn_functionResponsesMatchCallsByIdAndOrdinal() {
    const QJsonObject model_turn{
        {QStringLiteral("role"), QStringLiteral("model")},
        {QStringLiteral("parts"),
         QJsonArray{QJsonObject{{QStringLiteral("functionCall"),
                                 QJsonObject{{QStringLiteral("name"), QStringLiteral("a")}}}},
                    QJsonObject{{QStringLiteral("functionCall"),
                                 QJsonObject{{QStringLiteral("id"), QStringLiteral("fc_b")},
                                             {QStringLiteral("name"), QStringLiteral("b")}}}}}}};
    const QJsonArray history{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}},
                             model_turn};
    OpenAIResponseRequest request = anthropicRequest(QStringLiteral("gemini-3.8-flash"));
    request.input.clear();
    request.function_outputs = {{GeminiApiBackend::syntheticCallId(0),
                                 QStringLiteral("{\"ok\":1}")},
                                {QStringLiteral("fc_b"), QStringLiteral("plain text")}};
    QString error;
    const auto contents = GeminiApiBackend::turnContents(request, history, &error);
    QVERIFY2(contents.has_value(), qPrintable(error));
    const QJsonArray parts = contents->last().toObject().value(QStringLiteral("parts")).toArray();
    QCOMPARE(parts.size(), 2);
    const QJsonObject first =
        parts.at(0).toObject().value(QStringLiteral("functionResponse")).toObject();
    QCOMPARE(first.value(QStringLiteral("name")).toString(), QStringLiteral("a"));
    QVERIFY(!first.contains(QStringLiteral("id")));
    QCOMPARE(first.value(QStringLiteral("response")).toObject().value(QStringLiteral("ok")),
             QJsonValue(1));
    const QJsonObject second =
        parts.at(1).toObject().value(QStringLiteral("functionResponse")).toObject();
    QCOMPARE(second.value(QStringLiteral("id")).toString(), QStringLiteral("fc_b"));
    QCOMPARE(second.value(QStringLiteral("response")).toObject().value(QStringLiteral("output")),
             QJsonValue(QStringLiteral("plain text")));

    request.function_outputs = {{QStringLiteral("unknown"), QStringLiteral("x")}};
    QVERIFY(!GeminiApiBackend::turnContents(request, history, &error).has_value());
    QVERIFY(!GeminiApiBackend::turnContents(request, std::nullopt, &error).has_value());
}

void AiModelProviderTests::geminiParse_reportsBlocksAndApiErrors() {
    QString error;
    QVERIFY(!GeminiApiBackend::parseResponse(
                 R"({"promptFeedback":{"blockReason":"SAFETY"},"candidates":[]})", &error)
                 .has_value());
    QVERIFY(error.contains(QStringLiteral("SAFETY")));
    QVERIFY(
        !GeminiApiBackend::parseResponse(
             R"({"error":{"code":400,"message":"API key not valid","status":"INVALID_ARGUMENT"}})",
             &error)
             .has_value());
    QCOMPARE(error, QStringLiteral("INVALID_ARGUMENT: API key not valid"));
}

void AiModelProviderTests::geminiParseModels_filtersGenerateContent() {
    const QByteArray body = R"({"models":[
        {"name":"models/gemini-3.8-flash","supportedGenerationMethods":["generateContent","countTokens"]},
        {"name":"models/text-embedding-005","supportedGenerationMethods":["embedContent"]}]})";
    QString error;
    QCOMPARE(GeminiApiBackend::parseModels(body, &error),
             QStringList{QStringLiteral("gemini-3.8-flash")});
}

QTEST_GUILESS_MAIN(AiModelProviderTests)
#include "test_ai_model_providers.moc"

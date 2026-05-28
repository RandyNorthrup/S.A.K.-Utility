// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_credential_store.h"
#include "sak/ai/openai_responses_client.h"

#include <QEventLoop>
#include <QJsonDocument>
#include <QTimer>
#include <QtTest/QtTest>

class OpenAIResponsesClientTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void parseResponseObject_extractsTextUsageAndId();
    void parseResponseObject_extractsFunctionCall();
    void parseResponseObject_extractsUrlCitations();
    void parseResponseObject_apiError_reportsMessage();
    void parseModelsList_extractsIds();
    void redactSecrets_redactsOpenAiAndBearerTokens();
    void redactSecrets_redactsGitHubAndCloudTokens();
    void redactSecrets_redactsAssignmentStyleSecrets();
    void buildPayload_chatModeOmitsLocalTools();
    void buildPayload_assistedOrUnattendedAdvertisesLocalTools();
    void buildPayload_providerGatewayArgumentsAreStrictSchemaSafe();
    void buildPayload_packageToolWarnsAgainstScanInstalls();
    void realModelSmokeTest_optIn();
};

namespace {

[[nodiscard]] QJsonObject payloadObject(const sak::ai::OpenAIResponseRequest& request) {
    const QByteArray payload =
        sak::ai::OpenAIResponsesClient::buildResponsePayloadForTesting(request);
    return QJsonDocument::fromJson(payload).object();
}

[[nodiscard]] bool hasFunctionTool(const QJsonArray& tools, const QString& name) {
    for (const auto& value : tools) {
        const QJsonObject tool = value.toObject();
        if (tool.value(QStringLiteral("type")).toString() == QLatin1String("function") &&
            tool.value(QStringLiteral("name")).toString() == name) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] QJsonObject functionToolByName(const QJsonArray& tools, const QString& name) {
    for (const auto& value : tools) {
        const QJsonObject tool = value.toObject();
        if (tool.value(QStringLiteral("type")).toString() == QLatin1String("function") &&
            tool.value(QStringLiteral("name")).toString() == name) {
            return tool;
        }
    }
    return {};
}

[[nodiscard]] QString firstEnvironmentValue(const QStringList& names) {
    for (const auto& name : names) {
        const QString value = qEnvironmentVariable(name.toUtf8().constData()).trimmed();
        if (!value.isEmpty()) {
            return value;
        }
    }
    return {};
}

[[nodiscard]] bool isSmokeTestModelCandidate(const QString& model) {
    const QString id = model.toLower();
    if (!id.startsWith(QLatin1String("gpt-"))) {
        return false;
    }
    const QStringList disallowed{QStringLiteral("audio"),
                                 QStringLiteral("embedding"),
                                 QStringLiteral("image"),
                                 QStringLiteral("moderation"),
                                 QStringLiteral("realtime"),
                                 QStringLiteral("search"),
                                 QStringLiteral("transcribe"),
                                 QStringLiteral("tts")};
    for (const auto& marker : disallowed) {
        if (id.contains(marker)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] QString pickSmokeTestModel(const QStringList& models) {
    const QString requested = firstEnvironmentValue({QStringLiteral("SAK_AI_REAL_MODEL")});
    if (!requested.isEmpty()) {
        return requested;
    }

    const QStringList preferred = {
        QStringLiteral("gpt-5.4-mini"),
        QStringLiteral("gpt-5.4"),
        QStringLiteral("gpt-5.5"),
        QStringLiteral("gpt-4.1-mini"),
        QStringLiteral("gpt-4o-mini"),
        QStringLiteral("gpt-4o"),
    };
    for (const auto& model : preferred) {
        if (models.contains(model)) {
            return model;
        }
    }

    for (const auto& model : models) {
        if (isSmokeTestModelCandidate(model)) {
            return model;
        }
    }
    return {};
}

[[nodiscard]] QStringList listModelsForSmoke(sak::ai::OpenAIResponsesClient& client,
                                             const QString& api_key,
                                             QString* failure) {
    QStringList models;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&client,
                     &sak::ai::OpenAIResponsesClient::modelsReady,
                     &loop,
                     [&](const QStringList& model_ids) {
                         models = model_ids;
                         loop.quit();
                     });
    QObject::connect(&client,
                     &sak::ai::OpenAIResponsesClient::requestFailed,
                     &loop,
                     [&](const QString& error_message) {
                         *failure = sak::ai::CredentialStore::redactSecrets(error_message);
                         loop.quit();
                     });
    timeout.start(120'000);
    client.listModels(api_key);
    loop.exec();
    return models;
}

[[nodiscard]] sak::ai::OpenAIResponseResult runSmokeResponse(sak::ai::OpenAIResponsesClient& client,
                                                             const QString& api_key,
                                                             const QString& model,
                                                             QString* failure) {
    sak::ai::OpenAIResponseResult result;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        *failure = QStringLiteral("Timed out waiting for OpenAI smoke response");
        loop.quit();
    });
    QObject::connect(&client,
                     &sak::ai::OpenAIResponsesClient::responseReady,
                     &loop,
                     [&](const sak::ai::OpenAIResponseResult& response) {
                         result = response;
                         loop.quit();
                     });
    QObject::connect(&client,
                     &sak::ai::OpenAIResponsesClient::requestFailed,
                     &loop,
                     [&](const QString& error_message) {
                         *failure = sak::ai::CredentialStore::redactSecrets(error_message);
                         loop.quit();
                     });

    sak::ai::OpenAIResponseRequest request;
    request.api_key = api_key;
    request.model = model;
    request.instructions = QStringLiteral(
        "You are a minimal integration smoke test. Reply with exactly "
        "SAK_REAL_MODEL_SMOKE_OK and no extra text.");
    request.input = QStringLiteral("Return exactly: SAK_REAL_MODEL_SMOKE_OK");
    timeout.start(120'000);
    client.createResponse(request);
    loop.exec();
    return result;
}

}  // namespace

void OpenAIResponsesClientTests::parseResponseObject_extractsTextUsageAndId() {
    const QByteArray json = R"({
      "id": "resp_123",
      "object": "response",
      "output": [
        {
          "type": "message",
          "status": "completed",
          "content": [
            {"type": "output_text", "text": "Hello from AI"}
          ]
        }
      ],
      "usage": {
        "input_tokens": 11,
        "input_tokens_details": {"cached_tokens": 2},
        "output_tokens": 7,
        "output_tokens_details": {"reasoning_tokens": 3},
        "total_tokens": 18
      }
    })";

    QString error;
    const auto result = sak::ai::OpenAIResponsesClient::parseResponseObject(json, &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(result.id, QStringLiteral("resp_123"));
    QCOMPARE(result.output_text, QStringLiteral("Hello from AI"));
    QCOMPARE(result.usage.input_tokens, qint64{11});
    QCOMPARE(result.usage.cached_input_tokens, qint64{2});
    QCOMPARE(result.usage.output_tokens, qint64{7});
    QCOMPARE(result.usage.reasoning_tokens, qint64{3});
    QCOMPARE(result.usage.total_tokens, qint64{18});
}

void OpenAIResponsesClientTests::parseResponseObject_extractsFunctionCall() {
    const QByteArray json = R"({
      "id": "resp_tool",
      "output": [
        {
          "id": "fc_123",
          "call_id": "call_123",
          "type": "function_call",
          "name": "run_powershell",
          "arguments": "{\"command\":\"Get-PhysicalDisk\",\"timeout_seconds\":30,\"requires_admin\":false}"
        }
      ],
      "usage": {"input_tokens": 5, "output_tokens": 6, "total_tokens": 11}
    })";

    QString error;
    const auto result = sak::ai::OpenAIResponsesClient::parseResponseObject(json, &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(result.function_calls.size(), 1);
    QCOMPARE(result.function_calls[0].call_id, QStringLiteral("call_123"));
    QCOMPARE(result.function_calls[0].name, QStringLiteral("run_powershell"));
    QVERIFY(result.function_calls[0].arguments_json.contains(QStringLiteral("Get-PhysicalDisk")));
}

void OpenAIResponsesClientTests::parseResponseObject_extractsUrlCitations() {
    const QByteArray json = R"({
      "id": "resp_cite",
      "output": [
        {
          "type": "message",
          "content": [
            {
              "type": "output_text",
              "text": "Per Microsoft, run sfc /scannow.",
              "annotations": [
                {
                  "type": "url_citation",
                  "start_index": 4,
                  "end_index": 13,
                  "url": "https://learn.microsoft.com/sfc",
                  "title": "SFC documentation"
                },
                {
                  "type": "file_citation",
                  "url": "ignored"
                }
              ]
            }
          ]
        }
      ],
      "usage": {"input_tokens": 4, "output_tokens": 8, "total_tokens": 12}
    })";

    QString error;
    const auto result = sak::ai::OpenAIResponsesClient::parseResponseObject(json, &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(result.citations.size(), 1);
    QCOMPARE(result.citations[0].url, QStringLiteral("https://learn.microsoft.com/sfc"));
    QCOMPARE(result.citations[0].title, QStringLiteral("SFC documentation"));
    QCOMPARE(result.citations[0].start_index, 4);
    QCOMPARE(result.citations[0].end_index, 13);
}

void OpenAIResponsesClientTests::parseResponseObject_apiError_reportsMessage() {
    const QByteArray json = R"({
      "error": {
        "message": "Bad key",
        "type": "invalid_request_error"
      }
    })";

    QString error;
    const auto result = sak::ai::OpenAIResponsesClient::parseResponseObject(json, &error);
    QVERIFY(result.output_text.isEmpty());
    QVERIFY(error.contains(QStringLiteral("Bad key")));
    QVERIFY(error.contains(QStringLiteral("invalid_request_error")));
}

void OpenAIResponsesClientTests::parseModelsList_extractsIds() {
    const QByteArray json = R"({
      "object": "list",
      "data": [
        {"id": "gpt-5.4-mini"},
        {"id": "gpt-5.5"},
        {"id": "gpt-5.4-mini"}
      ]
    })";

    QString error;
    const auto models = sak::ai::OpenAIResponsesClient::parseModelsList(json, &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(models.size(), 2);
    QVERIFY(models.contains(QStringLiteral("gpt-5.4-mini")));
    QVERIFY(models.contains(QStringLiteral("gpt-5.5")));
}

void OpenAIResponsesClientTests::redactSecrets_redactsOpenAiAndBearerTokens() {
    const QString openai_redaction_sample = QStringLiteral("sk") + QStringLiteral("-") +
                                            QStringLiteral("abcdef") +
                                            QStringLiteral("ghijklmnopqrstuvwxyz");
    const QString context7_redaction_sample =
        QStringLiteral("ctx7") + QStringLiteral("s") +
        QStringLiteral("k-fc513191-580d-40c0-b244-17ea71f182b9");
    const QString bearer_redaction_sample = QStringLiteral("abcdefghijklmnop");
    const QString input =
        QStringLiteral("key=%1 ctx %2 bearer Bearer %3")
            .arg(openai_redaction_sample, context7_redaction_sample, bearer_redaction_sample);
    const QString redacted = sak::ai::CredentialStore::redactSecrets(input);
    QVERIFY(!redacted.contains(openai_redaction_sample));
    QVERIFY(!redacted.contains(context7_redaction_sample));
    QVERIFY(!redacted.contains(bearer_redaction_sample));
    QVERIFY(redacted.contains(QStringLiteral("[redacted]")));
    QVERIFY(redacted.contains(QStringLiteral("[redacted-context7-key]")));
}

void OpenAIResponsesClientTests::redactSecrets_redactsGitHubAndCloudTokens() {
    // Google API keys are 39 chars total (AIza + 35 alnum/underscore/hyphen).
    const QString github_redaction_sample = QStringLiteral("ghp_") +
                                            QStringLiteral("abcdefghijklmnopqrstuvwxyz0123456789");
    const QString aws_redaction_sample = QStringLiteral("AKIA") +
                                         QStringLiteral("ABCDEFGHIJKLMNOP");
    const QString google_redaction_sample = QStringLiteral("AIza") +
                                            QStringLiteral("SyD1234567890abcdefghijklmnopqrstuv");
    const QString input =
        QStringLiteral("token %1 and %2 google %3")
            .arg(github_redaction_sample, aws_redaction_sample, google_redaction_sample);
    const QString redacted = sak::ai::CredentialStore::redactSecrets(input);
    QVERIFY(redacted.contains(QStringLiteral("[redacted-github-token]")));
    QVERIFY(redacted.contains(QStringLiteral("[redacted-aws-key]")));
    QVERIFY(redacted.contains(QStringLiteral("[redacted-google-key]")));
    QVERIFY(!redacted.contains(github_redaction_sample));
    QVERIFY(!redacted.contains(aws_redaction_sample));
}

void OpenAIResponsesClientTests::redactSecrets_redactsAssignmentStyleSecrets() {
    const QString password_redaction_sample = QStringLiteral("hunter") + QStringLiteral("2hunter");
    const QString secret_redaction_sample = QStringLiteral("another") + QStringLiteral("-secret");
    const QString api_key_redaction_sample = QStringLiteral("abcd") + QStringLiteral("1234");
    const QString token_redaction_sample = QStringLiteral("tok") + QStringLiteral("-1234");
    const QString input = QStringLiteral("password=%1 password: \"%2\" api_key=%3 token: %4")
                              .arg(password_redaction_sample,
                                   secret_redaction_sample,
                                   api_key_redaction_sample,
                                   token_redaction_sample);
    const QString redacted = sak::ai::CredentialStore::redactSecrets(input);
    QVERIFY(!redacted.contains(password_redaction_sample));
    QVERIFY(!redacted.contains(secret_redaction_sample));
    QVERIFY(!redacted.contains(api_key_redaction_sample));
    QVERIFY(!redacted.contains(token_redaction_sample));
    QVERIFY(redacted.contains(QStringLiteral("[redacted]")));
}

void OpenAIResponsesClientTests::buildPayload_chatModeOmitsLocalTools() {
    sak::ai::OpenAIResponseRequest request;
    request.model = QStringLiteral("gpt-5.5");
    request.input = QStringLiteral("research only");
    request.enable_web_search = true;
    request.enable_local_tools = false;

    const QJsonObject root = payloadObject(request);
    const QJsonArray tools = root.value(QStringLiteral("tools")).toArray();

    QCOMPARE(tools.size(), 1);
    QCOMPARE(tools.first().toObject().value(QStringLiteral("type")).toString(),
             QStringLiteral("web_search_preview"));
    QVERIFY(!hasFunctionTool(tools, QStringLiteral("run_powershell")));
    QVERIFY(!hasFunctionTool(tools, QStringLiteral("run_cmd")));
    QVERIFY(!hasFunctionTool(tools, QStringLiteral("run_process")));
    QVERIFY(!hasFunctionTool(tools, QStringLiteral("take_screenshot")));
    QVERIFY(!hasFunctionTool(tools, QStringLiteral("download_file")));
    QVERIFY(!hasFunctionTool(tools, QStringLiteral("sak_package_manager")));
    QVERIFY(!hasFunctionTool(tools, QStringLiteral("sak_offline_downloader")));
    QVERIFY(!hasFunctionTool(tools, QStringLiteral("sak_provider_gateway")));
    QVERIFY(!hasFunctionTool(tools, QStringLiteral("sak_session_search")));
}

void OpenAIResponsesClientTests::buildPayload_assistedOrUnattendedAdvertisesLocalTools() {
    sak::ai::OpenAIResponseRequest request;
    request.model = QStringLiteral("gpt-5.5");
    request.input = QStringLiteral("check my hard drive");
    request.enable_web_search = true;
    request.enable_local_tools = true;

    const QJsonObject root = payloadObject(request);
    const QJsonArray tools = root.value(QStringLiteral("tools")).toArray();

    QVERIFY(hasFunctionTool(tools, QStringLiteral("run_powershell")));
    QVERIFY(hasFunctionTool(tools, QStringLiteral("run_cmd")));
    QVERIFY(hasFunctionTool(tools, QStringLiteral("run_process")));
    QVERIFY(hasFunctionTool(tools, QStringLiteral("take_screenshot")));
    QVERIFY(hasFunctionTool(tools, QStringLiteral("download_file")));
    QVERIFY(hasFunctionTool(tools, QStringLiteral("sak_package_manager")));
    QVERIFY(hasFunctionTool(tools, QStringLiteral("sak_offline_downloader")));
    QVERIFY(hasFunctionTool(tools, QStringLiteral("sak_provider_gateway")));
    QVERIFY(hasFunctionTool(tools, QStringLiteral("sak_session_search")));
    const QJsonObject gateway = functionToolByName(tools, QStringLiteral("sak_provider_gateway"));
    const QJsonArray operations = gateway.value(QStringLiteral("parameters"))
                                      .toObject()
                                      .value(QStringLiteral("properties"))
                                      .toObject()
                                      .value(QStringLiteral("operation"))
                                      .toObject()
                                      .value(QStringLiteral("enum"))
                                      .toArray();
    QVERIFY(operations.contains(QStringLiteral("docs_query")));
    QVERIFY(operations.contains(QStringLiteral("win32_mcp_call")));
    QVERIFY(operations.contains(QStringLiteral("app_run_action")));
    QCOMPARE(root.value(QStringLiteral("parallel_tool_calls")).toBool(true), false);
}

void OpenAIResponsesClientTests::buildPayload_providerGatewayArgumentsAreStrictSchemaSafe() {
    sak::ai::OpenAIResponseRequest request;
    request.model = QStringLiteral("gpt-5.5");
    request.input = QStringLiteral("check provider status");
    request.enable_local_tools = true;

    const QJsonObject root = payloadObject(request);
    const QJsonObject gateway = functionToolByName(root.value(QStringLiteral("tools")).toArray(),
                                                   QStringLiteral("sak_provider_gateway"));
    const QJsonObject arguments = gateway.value(QStringLiteral("parameters"))
                                      .toObject()
                                      .value(QStringLiteral("properties"))
                                      .toObject()
                                      .value(QStringLiteral("arguments"))
                                      .toObject();

    QCOMPARE(arguments.value(QStringLiteral("type")).toString(), QStringLiteral("string"));
    QVERIFY(!arguments.contains(QStringLiteral("additionalProperties")));
}

void OpenAIResponsesClientTests::buildPayload_packageToolWarnsAgainstScanInstalls() {
    sak::ai::OpenAIResponseRequest request;
    request.model = QStringLiteral("gpt-5.5");
    request.input = QStringLiteral("run a SUPERAntiSpyware scan");
    request.enable_local_tools = true;

    const QJsonObject root = payloadObject(request);
    const QJsonObject package = functionToolByName(root.value(QStringLiteral("tools")).toArray(),
                                                   QStringLiteral("sak_package_manager"));
    const QString description = package.value(QStringLiteral("description")).toString();

    QVERIFY(description.contains(QStringLiteral("Do not use install/upgrade/uninstall")));
    QVERIFY(description.contains(QStringLiteral("scan/action")));
    QVERIFY(description.contains(QStringLiteral("sak_provider_gateway")));
}

void OpenAIResponsesClientTests::realModelSmokeTest_optIn() {
    if (qEnvironmentVariable("SAK_AI_REAL_MODEL_TEST").trimmed() != QLatin1String("1") &&
        qEnvironmentVariable("SAK_RUN_OPENAI_LIVE_TESTS").trimmed() != QLatin1String("1")) {
        QSKIP("Set SAK_AI_REAL_MODEL_TEST=1 to run the live OpenAI smoke test.");
    }

    QString api_key = firstEnvironmentValue(
        {QStringLiteral("OPENAI_API_KEY"), QStringLiteral("SAK_OPENAI_API_KEY")});
    if (api_key.isEmpty()) {
        QString credential_error;
        api_key = sak::ai::CredentialStore().loadApiKey(&credential_error).trimmed();
        if (api_key.isEmpty()) {
            QSKIP("No OpenAI API key found in environment or encrypted app storage.");
        }
    }
    QVERIFY2(sak::ai::OpenAIResponsesClient::hasUsableApiKey(api_key),
             "OpenAI API key is present but not usable.");

    sak::ai::OpenAIResponsesClient client;
    QString failure;
    const QStringList models = listModelsForSmoke(client, api_key, &failure);

    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QVERIFY2(!models.isEmpty(), "OpenAI model list was empty.");
    const QString model = pickSmokeTestModel(models);
    QVERIFY2(!model.isEmpty(), "No chat-capable GPT model found for smoke test.");

    failure.clear();
    const sak::ai::OpenAIResponseResult result = runSmokeResponse(client, api_key, model, &failure);

    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QVERIFY2(result.output_text.contains(QStringLiteral("SAK_REAL_MODEL_SMOKE_OK")),
             qPrintable(QStringLiteral("Unexpected smoke response from %1: %2")
                            .arg(model, result.output_text.left(500))));
    QVERIFY2(result.usage.total_tokens > 0, "Live response did not include token usage.");
}

QTEST_GUILESS_MAIN(OpenAIResponsesClientTests)
#include "test_openai_responses_client.moc"

// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_mcp_http_client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest/QtTest>

class AiMcpHttpClientTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void buildsMcpToolCallPayload();
    void parsesSseJsonRpcMessage();
    void parsesPlainJsonRpcMessage();
    void rejectsSseWithoutJsonRpcData();
};

void AiMcpHttpClientTests::buildsMcpToolCallPayload() {
    const QJsonObject payload = sak::ai::AiMcpHttpClient::toolCallPayloadForTesting(
        QStringLiteral("microsoft_docs_search"),
        QJsonObject{{QStringLiteral("query"), QStringLiteral("Win32 UI Automation")}});

    QCOMPARE(payload.value(QStringLiteral("jsonrpc")).toString(), QStringLiteral("2.0"));
    QCOMPARE(payload.value(QStringLiteral("method")).toString(), QStringLiteral("tools/call"));
    const QJsonObject params = payload.value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("name")).toString(),
             QStringLiteral("microsoft_docs_search"));
    QCOMPARE(params.value(QStringLiteral("arguments"))
                 .toObject()
                 .value(QStringLiteral("query"))
                 .toString(),
             QStringLiteral("Win32 UI Automation"));
}

void AiMcpHttpClientTests::parsesSseJsonRpcMessage() {
    const QByteArray response =
        "event: message\n"
        "data: "
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"ok\"}"
        "]}}\n"
        "\n";

    QString error;
    const QJsonObject message = sak::ai::AiMcpHttpClient::extractJsonRpcMessageForTesting(response,
                                                                                          &error);

    QVERIFY(error.isEmpty());
    QCOMPARE(message.value(QStringLiteral("id")).toInt(), 1);
    const QJsonObject result = message.value(QStringLiteral("result")).toObject();
    QCOMPARE(result.value(QStringLiteral("content"))
                 .toArray()
                 .at(0)
                 .toObject()
                 .value(QStringLiteral("text"))
                 .toString(),
             QStringLiteral("ok"));
}

void AiMcpHttpClientTests::parsesPlainJsonRpcMessage() {
    const QByteArray response =
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"tools\":[{\"name\":\"query-docs\"}]}}";

    QString error;
    const QJsonObject message = sak::ai::AiMcpHttpClient::extractJsonRpcMessageForTesting(response,
                                                                                          &error);

    QVERIFY(error.isEmpty());
    QCOMPARE(message.value(QStringLiteral("id")).toInt(), 7);
    QCOMPARE(message.value(QStringLiteral("result"))
                 .toObject()
                 .value(QStringLiteral("tools"))
                 .toArray()
                 .at(0)
                 .toObject()
                 .value(QStringLiteral("name"))
                 .toString(),
             QStringLiteral("query-docs"));
}

void AiMcpHttpClientTests::rejectsSseWithoutJsonRpcData() {
    const QByteArray response =
        "event: message\n"
        "data: not-json\n"
        "\n";

    QString error;
    const QJsonObject message = sak::ai::AiMcpHttpClient::extractJsonRpcMessageForTesting(response,
                                                                                          &error);

    QVERIFY(message.isEmpty());
    QVERIFY(error.contains(QStringLiteral("Invalid MCP JSON response")));
}

QTEST_GUILESS_MAIN(AiMcpHttpClientTests)
#include "test_ai_mcp_http_client.moc"

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
    void rejectsSseSplitAcrossEvents();  // R5-G10-9
    void skipsSseNotificationsBeforeResponse();
    void rejectsPlainObjectWithoutJsonRpcFields();
    void rejectsResponseWithMismatchedId();
    void rejectsInsecureRemoteEndpoint();
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

void AiMcpHttpClientTests::rejectsSseSplitAcrossEvents() {
    // A hostile MCP server could try to smuggle a fabricated JSON-RPC result by splitting one
    // object across TWO SSE events, each fragment invalid alone but valid if glued. flushSseEvent
    // isolates each event (stores the unparsed data by assignment, and clears the buffer per
    // event), so the fragments are never concatenated -- the scanner must return {} with an error,
    // not reassemble.
    const QByteArray split =
        "data: {\"jsonrpc\":\"2.0\",\"id\":1,\n"
        "\n"
        "data: \"result\":{\"content\":[]}}\n"
        "\n";
    QString error;
    const QJsonObject message = sak::ai::AiMcpHttpClient::extractJsonRpcMessageForTesting(split,
                                                                                          &error);
    QVERIFY2(message.isEmpty(), "two SSE fragments must not be reassembled into a JSON-RPC object");
    QVERIFY(!error.isEmpty());

    // Guard-isolation control: the SAME object delivered in ONE event parses fine, so the rejection
    // is caused by the split, not by an always-failing scanner.
    const QByteArray whole =
        "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":[]}}\n"
        "\n";
    QString okError;
    const QJsonObject ok = sak::ai::AiMcpHttpClient::extractJsonRpcMessageForTesting(whole,
                                                                                     &okError);
    QVERIFY(okError.isEmpty());
    QCOMPARE(ok.value(QStringLiteral("id")).toInt(), 1);
}

void AiMcpHttpClientTests::skipsSseNotificationsBeforeResponse() {
    // A spec-compliant server may stream a progress notification (no id) before the
    // id-bearing result. The first data event must not be mistaken for the response.
    const QByteArray response =
        "data: "
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\",\"params\":{\"progress\":1}}\n"
        "\n"
        "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":[{\"type\":\"text\","
        "\"text\":\"done\"}]}}\n"
        "\n";

    QString error;
    const QJsonObject message = sak::ai::AiMcpHttpClient::extractJsonRpcMessageForTesting(response,
                                                                                          &error);

    QVERIFY(error.isEmpty());
    QCOMPARE(message.value(QStringLiteral("id")).toInt(), 1);
    QVERIFY(message.contains(QStringLiteral("result")));
    QVERIFY(!message.contains(QStringLiteral("method")));
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

void AiMcpHttpClientTests::rejectsPlainObjectWithoutJsonRpcFields() {
    // A bare `{`-leading body that is a valid object but NOT a JSON-RPC response (no id +
    // result/error) must be rejected, not accepted verbatim as a successful tool result.
    const QByteArray response = "{\"content\":[{\"type\":\"text\",\"text\":\"injected\"}]}";

    QString error;
    const QJsonObject message = sak::ai::AiMcpHttpClient::extractJsonRpcMessageForTesting(response,
                                                                                          &error);

    QVERIFY(message.isEmpty());
    QVERIFY(!error.isEmpty());
}

void AiMcpHttpClientTests::rejectsResponseWithMismatchedId() {
    // JSON-RPC correlation: callTool() sends exactly one request (id 1) and must accept
    // ONLY a response that echoes that id. A response bearing a different id -- or a
    // missing/non-numeric one -- is a stray, out-of-band, or transport-crafted message and
    // must be refused rather than accepted as the tool result (which would let an injected
    // frame stand in for the real answer).
    using Client = sak::ai::AiMcpHttpClient;

    // Non-vacuity control: the real request id (1) DOES correlate.
    QVERIFY(Client::responseIdMatchesRequestForTesting(
        QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), 1},
                    {QStringLiteral("result"), QJsonObject{}}}));

    // A different numeric id -> not our answer.
    QVERIFY(!Client::responseIdMatchesRequestForTesting(
        QJsonObject{{QStringLiteral("id"), 2}, {QStringLiteral("result"), QJsonObject{}}}));

    // A missing id -> toInt(-1) defaults to -1, which is not the request id.
    QVERIFY(!Client::responseIdMatchesRequestForTesting(
        QJsonObject{{QStringLiteral("result"), QJsonObject{}}}));

    // A non-numeric (string) id -> not a JSON number, so toInt(-1) yields -1 and it is
    // refused; the correlation cannot be spoofed with the request id spelled as a string.
    QVERIFY(!Client::responseIdMatchesRequestForTesting(QJsonObject{
        {QStringLiteral("id"), QStringLiteral("1")}, {QStringLiteral("result"), QJsonObject{}}}));
}

void AiMcpHttpClientTests::rejectsInsecureRemoteEndpoint() {
    // A non-loopback http:// endpoint must be refused before any network I/O (no downgrade
    // to cleartext from a manifest typo or disk override).
    QString error;
    const QJsonObject message =
        sak::ai::AiMcpHttpClient::callTool(QUrl(QStringLiteral("http://docs.example.com/mcp")),
                                           QStringLiteral("query-docs"),
                                           QJsonObject{},
                                           1000,
                                           &error);

    QVERIFY(message.isEmpty());
    QVERIFY(error.contains(QStringLiteral("https")));
}

QTEST_GUILESS_MAIN(AiMcpHttpClientTests)
#include "test_ai_mcp_http_client.moc"

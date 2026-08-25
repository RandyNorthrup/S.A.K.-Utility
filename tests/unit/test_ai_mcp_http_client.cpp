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
    void acceptsLoopbackAndHttpsEndpoints();
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
    // The last unparsed event fragment drives the parse-error path (distinct from the
    // "did not contain JSON-RPC data" path); the errorString() tail is Qt-version-brittle.
    QVERIFY(error.contains(QStringLiteral("Invalid MCP JSON response")));

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
    // Arm 1 of settleSseFailure (src/ai/ai_mcp_http_client.cpp:129-131): the last malformed event
    // drives the REAL parse error, so the message must carry the QJsonParseError detail after the
    // prefix -- a hardcoded prefix with the detail dropped is not enough. The errorString() tail
    // itself is Qt-version-brittle, so pin that a detail is PRESENT, not its exact text.
    QVERIFY(error.startsWith(QStringLiteral("Invalid MCP JSON response: ")));
    QVERIFY(error.size() > QStringLiteral("Invalid MCP JSON response: ").size());

    // Arm 2 of settleSseFailure (:132-134): a stream of well-formed NOTIFICATIONS is parsable, so
    // flushSseEvent never records last_unparsed and nothing supplies a parse error. callTool's
    // contract with runDocsToolCall (src/ai/ai_provider_gateway.cpp:774-781) is "return {} AND set
    // error_message", so this arm must NAME the failure instead of leaving error_message untouched
    // -- otherwise the user sees a tool failure with an empty reason.
    const QByteArray notifications_only =
        "data: "
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\",\"params\":{\"progress\":1}}\n"
        "\n";
    QString notify_error;
    const QJsonObject no_response = sak::ai::AiMcpHttpClient::extractJsonRpcMessageForTesting(
        notifications_only, &notify_error);
    QVERIFY(no_response.isEmpty());
    QCOMPARE(notify_error, QStringLiteral("MCP response did not contain JSON-RPC data"));
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
    QCOMPARE(error, QStringLiteral("MCP response is not a JSON-RPC response"));

    // isJsonRpcResponse (src/ai/ai_mcp_http_client.cpp:91-95) is a THREE-arm predicate:
    // jsonrpc == "2.0" AND contains("id") AND (contains("result") OR contains("error")).
    // The body above misses ALL THREE at once, so it proves none of them individually. The
    // fixtures below are single-field deltas off an ACCEPTED baseline, so each goes red if --
    // and only if -- its own arm is deleted.
    using Client = sak::ai::AiMcpHttpClient;

    // ACCEPT-side baseline for the `error` half of the disjunction: a legitimate JSON-RPC
    // error response must be EXTRACTED, not refused as "not a JSON-RPC response", because
    // that is what lets callTool reach explainJsonRpcError (:496) and surface the server's
    // real message instead of a misleading transport error.
    QString error_arm_error;
    const QJsonObject error_response = Client::extractJsonRpcMessageForTesting(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32000,\"message\":\"tool failed\"}}",
        &error_arm_error);
    QVERIFY(error_arm_error.isEmpty());
    QCOMPARE(error_response.value(QStringLiteral("id")).toInt(), 1);
    QCOMPARE(error_response.value(QStringLiteral("error"))
                 .toObject()
                 .value(QStringLiteral("message"))
                 .toString(),
             QStringLiteral("tool failed"));

    // Arm 1 (version string): an otherwise-acceptable response bearing jsonrpc "1.0" is
    // refused. Relaxing the compare at :92 to contains("jsonrpc") would accept this.
    QString version_error;
    QVERIFY(Client::extractJsonRpcMessageForTesting("{\"jsonrpc\":\"1.0\",\"id\":1,\"result\":{}}",
                                                    &version_error)
                .isEmpty());
    QCOMPARE(version_error, QStringLiteral("MCP response is not a JSON-RPC response"));

    // Arm 2 (id): the accepted error response MINUS its id is refused. Deleting the
    // contains("id") arm at :93 would accept this id-less object as our answer.
    QString id_error;
    QVERIFY(Client::extractJsonRpcMessageForTesting(
                "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32000,\"message\":\"tool failed\"}}",
                &id_error)
                .isEmpty());
    QCOMPARE(id_error, QStringLiteral("MCP response is not a JSON-RPC response"));

    // Arm 3 (result OR error): version and id present, but neither payload key -> refused.
    QString payload_error;
    QVERIFY(
        Client::extractJsonRpcMessageForTesting("{\"jsonrpc\":\"2.0\",\"id\":1}", &payload_error)
            .isEmpty());
    QCOMPARE(payload_error, QStringLiteral("MCP response is not a JSON-RPC response"));
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
    QCOMPARE(error,
             QStringLiteral(
                 "MCP endpoint must use https (or http on loopback): http://docs.example.com/mcp"));
}

void AiMcpHttpClientTests::acceptsLoopbackAndHttpsEndpoints() {
    // R5-G18-4. The refusal above proves ONE of validateHttpToolCall's three arms, in the
    // reject direction only. The controls below pin the ACCEPT side and the two sibling arms.
    //
    // Guard isolation without network I/O: callTool checks the request-body cap
    // (ai_mcp_http_client.cpp:466-472) IMMEDIATELY after validateHttpToolCall (:460) and
    // BEFORE performHttpToolCall (:477). Every control here sends an OVERSIZE argument tree,
    // so a call that gets PAST validation stops deterministically at that cap. The accept
    // side is therefore proved with zero sockets, and no mutation of these guards can make
    // this unit test open one.
    constexpr int kRequestBodyCapBytes = 1024 * 1024;  // mirrors kMaxRequestBytes (:28)
    const QString body_cap_error =
        QStringLiteral("MCP request body exceeds the %1-byte cap").arg(kRequestBodyCapBytes);
    const QJsonObject oversize{
        {QStringLiteral("pad"), QString(kRequestBodyCapBytes + 1, QLatin1Char('a'))}};

    const auto callToolError = [&oversize](const QUrl& endpoint, const QString& tool_name) {
        QString call_error;
        const QJsonObject reply =
            sak::ai::AiMcpHttpClient::callTool(endpoint, tool_name, oversize, 1000, &call_error);
        return reply.isEmpty() ? call_error : QStringLiteral("<unexpected tool result>");
    };

    // ACCEPT side -- each arm of isLoopbackHost (:322-325). A local MCP server on plain http
    // MUST still be reachable; deleting any one arm turns that host's call back into the
    // scheme refusal, so that QCOMPARE goes RED.
    QCOMPARE(callToolError(QUrl(QStringLiteral("http://127.0.0.1:8931/mcp")),
                           QStringLiteral("query-docs")),
             body_cap_error);
    QCOMPARE(callToolError(QUrl(QStringLiteral("http://localhost:8931/mcp")),
                           QStringLiteral("query-docs")),
             body_cap_error);
    // QUrl::host() strips the brackets of an IPv6 literal, yielding the "::1" the guard compares.
    QCOMPARE(callToolError(QUrl(QStringLiteral("http://[::1]:8931/mcp")),
                           QStringLiteral("query-docs")),
             body_cap_error);
    // ACCEPT side -- the https arm of endpointSchemeIsSecure (:328-330) on a NON-loopback host,
    // so the refusal pinned above is caused by the scheme, not by an always-refusing gate.
    QCOMPARE(callToolError(QUrl(QStringLiteral("https://docs.example.com/mcp")),
                           QStringLiteral("query-docs")),
             body_cap_error);

    // Sibling arm -- endpoint SHAPE (:336). Built component-wise so the URL IS valid but has an
    // empty host; the two QVERIFYs isolate the host arm from the isValid arm, so deleting
    // `|| endpoint.host().isEmpty()` lets it reach the body cap instead -> RED.
    QUrl hostless;
    hostless.setScheme(QStringLiteral("https"));
    hostless.setPath(QStringLiteral("/mcp"));
    QVERIFY(hostless.isValid());
    QVERIFY(hostless.host().isEmpty());
    QCOMPARE(callToolError(hostless, QStringLiteral("query-docs")),
             QStringLiteral("Invalid MCP endpoint"));

    // Sibling arm -- TOOL NAME (:349-354). An all-whitespace name is refused before any I/O, so
    // a nameless tools/call can never be POSTed to a live server as a tools/call `name`.
    // Deleting the guard lets it reach the body cap instead -> RED.
    QCOMPARE(callToolError(QUrl(QStringLiteral("http://127.0.0.1:8931/mcp")),
                           QStringLiteral("   ")),
             QStringLiteral("MCP tool name is empty"));
}

QTEST_GUILESS_MAIN(AiMcpHttpClientTests)
#include "test_ai_mcp_http_client.moc"

// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_dispatch.h"
#include "sak/win32mcp/win32_mcp_tools.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest/QtTest>

using sak::win32mcp::handleRequest;
using sak::win32mcp::invokeTool;
using sak::win32mcp::toolCatalog;

namespace {

QJsonObject request(const QString& method, const QJsonValue& id, const QJsonObject& params = {}) {
    QJsonObject obj{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("method"), method}};
    if (!id.isUndefined()) {
        obj.insert(QStringLiteral("id"), id);
    }
    if (!params.isEmpty()) {
        obj.insert(QStringLiteral("params"), params);
    }
    return obj;
}

QJsonObject callToolText(const QJsonObject& response) {
    const QJsonObject result = response.value(QStringLiteral("result")).toObject();
    const QJsonArray content = result.value(QStringLiteral("content")).toArray();
    const QString text = content.isEmpty()
                             ? QString()
                             : content.at(0).toObject().value(QStringLiteral("text")).toString();
    return QJsonDocument::fromJson(text.toUtf8()).object();
}

}  // namespace

class Win32McpServerTests : public QObject {
    Q_OBJECT

private slots:
    void initialize_reportsNativeServerIdentityAndProtocol();
    void toolsList_advertisesReadOnlyBatchWithStrictSchemas();
    void notification_withoutIdGetsNoResponse();
    void unknownMethod_returnsMethodNotFound();
    void ping_returnsEmptyResult();
    void toolsCall_healthCheckReturnsOkNotError();
    void toolsCall_missingNameReturnsInvalidParams();
    void toolsCall_unknownToolReturnsIsError();
    void invokeTool_listWindowsReturnsStructuredArray();
    void invokeTool_getWindowInfoRequiresTitle();
};

void Win32McpServerTests::initialize_reportsNativeServerIdentityAndProtocol() {
    const auto response = handleRequest(request(QStringLiteral("initialize"), 1));
    QVERIFY(response.has_value());
    const QJsonObject result = response->value(QStringLiteral("result")).toObject();
    QCOMPARE(result.value(QStringLiteral("protocolVersion")).toString(),
             QStringLiteral("2024-11-05"));
    const QJsonObject info = result.value(QStringLiteral("serverInfo")).toObject();
    QCOMPARE(info.value(QStringLiteral("name")).toString(), QStringLiteral("sak-win32-mcp"));
    QVERIFY(!info.value(QStringLiteral("version")).toString().isEmpty());
    // Must advertise a tools capability so clients know tools/list is supported.
    QVERIFY(
        result.value(QStringLiteral("capabilities")).toObject().contains(QStringLiteral("tools")));
}

void Win32McpServerTests::toolsList_advertisesReadOnlyBatchWithStrictSchemas() {
    const auto response = handleRequest(request(QStringLiteral("tools/list"), 2));
    QVERIFY(response.has_value());
    const QJsonArray tools = response->value(QStringLiteral("result"))
                                 .toObject()
                                 .value(QStringLiteral("tools"))
                                 .toArray();
    QVERIFY(tools.size() >= 5);

    QStringList names;
    for (const auto& value : tools) {
        const QJsonObject tool = value.toObject();
        names << tool.value(QStringLiteral("name")).toString();
        QVERIFY(!tool.value(QStringLiteral("description")).toString().isEmpty());
        const QJsonObject schema = tool.value(QStringLiteral("inputSchema")).toObject();
        QCOMPARE(schema.value(QStringLiteral("type")).toString(), QStringLiteral("object"));
        QCOMPARE(schema.value(QStringLiteral("additionalProperties")).toBool(true), false);
    }
    for (const QString& expected : {QStringLiteral("health_check"),
                                    QStringLiteral("list_windows"),
                                    QStringLiteral("get_window_info"),
                                    QStringLiteral("list_monitors"),
                                    QStringLiteral("mouse_position")}) {
        QVERIFY2(names.contains(expected), qPrintable(expected));
    }
    // list_processes was dropped: it duplicated the app's own diagnostics/process listing, and the
    // rule is to use app code for any feature that overlaps. The MCP owns only what the app cannot
    // do headlessly (live-desktop inspection/automation).
    QVERIFY(!names.contains(QStringLiteral("list_processes")));
}

void Win32McpServerTests::notification_withoutIdGetsNoResponse() {
    const auto response =
        handleRequest(request(QStringLiteral("notifications/initialized"), QJsonValue::Undefined));
    QVERIFY(!response.has_value());
}

void Win32McpServerTests::unknownMethod_returnsMethodNotFound() {
    const auto response = handleRequest(request(QStringLiteral("does/not/exist"), 3));
    QVERIFY(response.has_value());
    QCOMPARE(
        response->value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(),
        -32'601);
}

void Win32McpServerTests::ping_returnsEmptyResult() {
    const auto response = handleRequest(request(QStringLiteral("ping"), 4));
    QVERIFY(response.has_value());
    QVERIFY(response->contains(QStringLiteral("result")));
    QVERIFY(!response->contains(QStringLiteral("error")));
}

void Win32McpServerTests::toolsCall_healthCheckReturnsOkNotError() {
    const auto response =
        handleRequest(request(QStringLiteral("tools/call"),
                              5,
                              QJsonObject{{QStringLiteral("name"), QStringLiteral("health_check")},
                                          {QStringLiteral("arguments"), QJsonObject{}}}));
    QVERIFY(response.has_value());
    const QJsonObject result = response->value(QStringLiteral("result")).toObject();
    QCOMPARE(result.value(QStringLiteral("isError")).toBool(true), false);
    const QJsonObject payload = callToolText(*response);
    QCOMPARE(payload.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    QCOMPARE(payload.value(QStringLiteral("server")).toString(), QStringLiteral("sak-win32-mcp"));
    QVERIFY(payload.value(QStringLiteral("native")).toBool());
}

void Win32McpServerTests::toolsCall_missingNameReturnsInvalidParams() {
    const auto response =
        handleRequest(request(QStringLiteral("tools/call"),
                              6,
                              QJsonObject{{QStringLiteral("arguments"), QJsonObject{}}}));
    QVERIFY(response.has_value());
    QCOMPARE(
        response->value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(),
        -32'602);
}

void Win32McpServerTests::toolsCall_unknownToolReturnsIsError() {
    const auto response = handleRequest(
        request(QStringLiteral("tools/call"),
                7,
                QJsonObject{{QStringLiteral("name"), QStringLiteral("no_such_tool")}}));
    QVERIFY(response.has_value());
    // A bad tool name is a successful RPC whose payload is flagged isError, not a
    // protocol error -- the model can read the reason and recover.
    const QJsonObject result = response->value(QStringLiteral("result")).toObject();
    QCOMPARE(result.value(QStringLiteral("isError")).toBool(false), true);
}

void Win32McpServerTests::invokeTool_listWindowsReturnsStructuredArray() {
    const sak::win32mcp::ToolResult result = invokeTool(QStringLiteral("list_windows"), {});
    QVERIFY(!result.is_error);
    const QJsonObject payload = QJsonDocument::fromJson(result.text.toUtf8()).object();
    QVERIFY(payload.contains(QStringLiteral("windows")));
    QVERIFY(payload.value(QStringLiteral("windows")).isArray());
    QCOMPARE(payload.value(QStringLiteral("count")).toInt(),
             payload.value(QStringLiteral("windows")).toArray().size());
}

void Win32McpServerTests::invokeTool_getWindowInfoRequiresTitle() {
    const sak::win32mcp::ToolResult result = invokeTool(QStringLiteral("get_window_info"), {});
    QVERIFY(result.is_error);
}

QTEST_MAIN(Win32McpServerTests)
#include "test_win32_mcp_server.moc"

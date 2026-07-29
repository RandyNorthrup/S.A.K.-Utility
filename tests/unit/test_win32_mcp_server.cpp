// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/browser_control.h"
#include "sak/win32mcp/browser_extension_installer.h"
#include "sak/win32mcp/win32_mcp_dispatch.h"
#include "sak/win32mcp/win32_mcp_tools.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest/QtTest>

using sak::win32mcp::handleRequest;
using sak::win32mcp::invokeTool;
using sak::win32mcp::toolCallResult;
using sak::win32mcp::toolCatalog;
using sak::win32mcp::ToolResult;

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
    void invokeTool_clipboardWriteRequiresText();
    void invokeTool_clipboardReadReturnsTextShape();
    void invokeTool_browserExtensionStatusReturnsShape();
    void toolsCall_browserExtensionRoutesToInstallerNotBridge();
    void toolCallResult_textOnlyIsSingleTextBlock();
    void toolCallResult_imageBecomesImageBlockPlusSummary();
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
                                    QStringLiteral("mouse_position"),
                                    QStringLiteral("clipboard_read"),
                                    QStringLiteral("clipboard_write"),
                                    QStringLiteral("browser_extension_install"),
                                    QStringLiteral("browser_extension_uninstall"),
                                    QStringLiteral("browser_extension_status")}) {
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

void Win32McpServerTests::invokeTool_clipboardWriteRequiresText() {
    // Missing 'text' must be a clean error and must NOT touch the clipboard.
    const sak::win32mcp::ToolResult result = invokeTool(QStringLiteral("clipboard_write"), {});
    QVERIFY(result.is_error);
}

void Win32McpServerTests::invokeTool_clipboardReadReturnsTextShape() {
    // A read is non-destructive, so it is safe to exercise against the real clipboard. It
    // must be honest: either a well-formed {text,has_text,truncated} payload or a flagged
    // error -- never a bare empty success. (We deliberately do NOT test clipboard_write's
    // success path, which would clobber the user's actual clipboard.)
    const sak::win32mcp::ToolResult result = invokeTool(QStringLiteral("clipboard_read"), {});
    const QJsonObject payload = QJsonDocument::fromJson(result.text.toUtf8()).object();
    if (result.is_error) {
        QVERIFY(payload.contains(QStringLiteral("error")));
        return;
    }
    QVERIFY(payload.contains(QStringLiteral("text")));
    QVERIFY(payload.value(QStringLiteral("has_text")).isBool());
    QVERIFY(payload.value(QStringLiteral("truncated")).isBool());
}

void Win32McpServerTests::invokeTool_browserExtensionStatusReturnsShape() {
    // status only READS the registry (never mutates policy), so it is safe here. It must
    // return a well-formed {state, crx_present, extension_id} payload with the pinned id.
    // (We deliberately do NOT invoke browser_extension_install/uninstall, which would write
    // the machine's real Chrome policy; their behavior is covered by the installer unit test
    // against a throwaway registry key.)
    const sak::win32mcp::ToolResult result = invokeTool(QStringLiteral("browser_extension_status"),
                                                        {});
    QVERIFY(!result.is_error);
    const QJsonObject payload = QJsonDocument::fromJson(result.text.toUtf8()).object();
    const QString state = payload.value(QStringLiteral("state")).toString();
    QVERIFY(state == QStringLiteral("installed") || state == QStringLiteral("not_installed") ||
            state == QStringLiteral("partial") || state == QStringLiteral("error"));
    QVERIFY(payload.value(QStringLiteral("crx_present")).isBool());
    QCOMPARE(payload.value(QStringLiteral("extension_id")).toString(),
             QString::fromLatin1(sak::win32mcp::kBrowserExtensionId));
}

void Win32McpServerTests::toolsCall_browserExtensionRoutesToInstallerNotBridge() {
    // Regression: the browser_extension_* tools are native installer tools. Their
    // "browser_" prefix must NOT route them to the browser bridge (which would answer
    // "browser not connected" before Chrome is even set up). handles() must reject them,
    // and tools/call must reach invokeTool even when a BrowserControl is present.
    sak::win32mcp::BrowserControl browser;  // not started; handles() is a pure check
    QVERIFY(!browser.handles(QStringLiteral("browser_extension_install")));
    QVERIFY(!browser.handles(QStringLiteral("browser_extension_uninstall")));
    QVERIFY(!browser.handles(QStringLiteral("browser_extension_status")));
    QVERIFY(browser.handles(QStringLiteral("browser_snapshot")));
    QVERIFY(browser.handles(QStringLiteral("browser_click")));

    const auto response = handleRequest(
        request(QStringLiteral("tools/call"),
                42,
                QJsonObject{{QStringLiteral("name"), QStringLiteral("browser_extension_status")}}),
        &browser);
    QVERIFY(response.has_value());
    const QJsonObject result = response->value(QStringLiteral("result")).toObject();
    QVERIFY(!result.value(QStringLiteral("isError")).toBool(true));
    const QString text = result.value(QStringLiteral("content"))
                             .toArray()
                             .first()
                             .toObject()
                             .value(QStringLiteral("text"))
                             .toString();
    const QJsonObject payload = QJsonDocument::fromJson(text.toUtf8()).object();
    QCOMPARE(payload.value(QStringLiteral("extension_id")).toString(),
             QString::fromLatin1(sak::win32mcp::kBrowserExtensionId));
}

void Win32McpServerTests::toolCallResult_textOnlyIsSingleTextBlock() {
    const QJsonObject out = toolCallResult(ToolResult{QStringLiteral("hello"), false, {}, {}});
    const QJsonArray content = out.value(QStringLiteral("content")).toArray();
    QCOMPARE(content.size(), 1);
    QCOMPARE(content.at(0).toObject().value(QStringLiteral("type")).toString(),
             QStringLiteral("text"));
    QCOMPARE(content.at(0).toObject().value(QStringLiteral("text")).toString(),
             QStringLiteral("hello"));
    QCOMPARE(out.value(QStringLiteral("isError")).toBool(true), false);
}

void Win32McpServerTests::toolCallResult_imageBecomesImageBlockPlusSummary() {
    ToolResult result;
    result.text = QStringLiteral("Captured a 800x600 PNG screenshot of the active tab.");
    result.image_base64 = QStringLiteral("iVBORw0KGgo");
    result.image_mime = QStringLiteral("image/png");
    const QJsonObject out = toolCallResult(result);
    const QJsonArray content = out.value(QStringLiteral("content")).toArray();
    QCOMPARE(content.size(), 2);
    // Image block first, carrying the base64 in `data` with a proper mimeType.
    const QJsonObject image = content.at(0).toObject();
    QCOMPARE(image.value(QStringLiteral("type")).toString(), QStringLiteral("image"));
    QCOMPARE(image.value(QStringLiteral("data")).toString(), QStringLiteral("iVBORw0KGgo"));
    QCOMPARE(image.value(QStringLiteral("mimeType")).toString(), QStringLiteral("image/png"));
    // Text summary follows so a text-only client still learns what happened.
    const QJsonObject text = content.at(1).toObject();
    QCOMPARE(text.value(QStringLiteral("type")).toString(), QStringLiteral("text"));
    QVERIFY(text.value(QStringLiteral("text")).toString().contains(QStringLiteral("screenshot")));
}

QTEST_MAIN(Win32McpServerTests)
#include "test_win32_mcp_server.moc"

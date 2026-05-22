// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_mcp_stdio_client.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QtTest/QtTest>

class AiMcpStdioClientTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void buildsInitializePayload();
    void buildsToolCallPayload();
    void liveWin32McpListWindows_optIn();
};

void AiMcpStdioClientTests::buildsInitializePayload() {
    const QJsonObject payload = sak::ai::AiMcpStdioClient::initializePayloadForTesting();

    QCOMPARE(payload.value(QStringLiteral("jsonrpc")).toString(), QStringLiteral("2.0"));
    QCOMPARE(payload.value(QStringLiteral("id")).toInt(), 1);
    QCOMPARE(payload.value(QStringLiteral("method")).toString(), QStringLiteral("initialize"));
    QCOMPARE(payload.value(QStringLiteral("params"))
                 .toObject()
                 .value(QStringLiteral("clientInfo"))
                 .toObject()
                 .value(QStringLiteral("name"))
                 .toString(),
             QStringLiteral("sak-utility"));
}

void AiMcpStdioClientTests::buildsToolCallPayload() {
    const QJsonObject payload = sak::ai::AiMcpStdioClient::toolCallPayloadForTesting(
        9,
        QStringLiteral("list_windows"),
        QJsonObject{{QStringLiteral("filter"), QStringLiteral("SAK")}});

    QCOMPARE(payload.value(QStringLiteral("jsonrpc")).toString(), QStringLiteral("2.0"));
    QCOMPARE(payload.value(QStringLiteral("id")).toInt(), 9);
    QCOMPARE(payload.value(QStringLiteral("method")).toString(), QStringLiteral("tools/call"));
    const QJsonObject params = payload.value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("name")).toString(), QStringLiteral("list_windows"));
    QCOMPARE(params.value(QStringLiteral("arguments"))
                 .toObject()
                 .value(QStringLiteral("filter"))
                 .toString(),
             QStringLiteral("SAK"));
}

void AiMcpStdioClientTests::liveWin32McpListWindows_optIn() {
    if (qEnvironmentVariable("SAK_WIN32_MCP_LIVE_TEST").trimmed() != QLatin1String("1")) {
        QSKIP("Set SAK_WIN32_MCP_LIVE_TEST=1 to run the bundled Win32 MCP stdio smoke test.");
    }

    const QString command =
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("tools/mcp/win32-mcp-server/win32-mcp-server.exe"));
    QVERIFY2(QFileInfo::exists(command), qPrintable(QStringLiteral("Missing %1").arg(command)));

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("WIN32_MCP_SECURITY_PROFILE"), QStringLiteral("read_only"));
    env.insert(QStringLiteral("WIN32_MCP_REDACT_SENSITIVE_OUTPUT"), QStringLiteral("true"));

    QString error;
    const QJsonObject message =
        sak::ai::AiMcpStdioClient::callTool({.command = command,
                                             .tool_name = QStringLiteral("list_windows"),
                                             .arguments = QJsonObject{},
                                             .environment = env,
                                             .timeout_ms = 20'000},
                                            &error);

    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QJsonArray content = message.value(QStringLiteral("result"))
                                   .toObject()
                                   .value(QStringLiteral("content"))
                                   .toArray();
    QVERIFY(!content.isEmpty());
    QCOMPARE(content.at(0).toObject().value(QStringLiteral("type")).toString(),
             QStringLiteral("text"));
}

QTEST_GUILESS_MAIN(AiMcpStdioClientTests)
#include "test_ai_mcp_stdio_client.moc"

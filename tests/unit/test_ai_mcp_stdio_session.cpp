// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_mcp_stdio_session.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QtTest/QtTest>

class AiMcpStdioSessionTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void buildsToolsListPayload();
    void parsesToolsListResult();
    void parseSkipsNamelessAndEmpty();
    void notOpenSessionRejectsCalls();
    void openFailsCleanlyForMissingCommand();
    void liveWin32McpPersistentSession_optIn();
};

void AiMcpStdioSessionTests::buildsToolsListPayload() {
    const QJsonObject payload = sak::ai::AiMcpStdioSession::toolsListPayloadForTesting(7);
    QCOMPARE(payload.value(QStringLiteral("jsonrpc")).toString(), QStringLiteral("2.0"));
    QCOMPARE(payload.value(QStringLiteral("id")).toInt(), 7);
    QCOMPARE(payload.value(QStringLiteral("method")).toString(), QStringLiteral("tools/list"));
}

void AiMcpStdioSessionTests::parsesToolsListResult() {
    const QJsonObject result{
        {QStringLiteral("tools"),
         QJsonArray{
             QJsonObject{{QStringLiteral("name"), QStringLiteral("list_windows")},
                         {QStringLiteral("description"), QStringLiteral("List top-level windows")},
                         {QStringLiteral("inputSchema"),
                          QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}}},
             QJsonObject{{QStringLiteral("name"), QStringLiteral("focus_window")}},
         }}};

    const auto tools = sak::ai::AiMcpStdioSession::parseToolsListResultForTesting(result);
    QCOMPARE(tools.size(), 2);
    QCOMPARE(tools[0].name, QStringLiteral("list_windows"));
    QCOMPARE(tools[0].description, QStringLiteral("List top-level windows"));
    QCOMPARE(tools[0].input_schema.value(QStringLiteral("type")).toString(),
             QStringLiteral("object"));
    QCOMPARE(tools[1].name, QStringLiteral("focus_window"));
}

void AiMcpStdioSessionTests::parseSkipsNamelessAndEmpty() {
    const QJsonObject result{
        {QStringLiteral("tools"),
         QJsonArray{
             QJsonObject{{QStringLiteral("description"), QStringLiteral("no name -> skipped")}},
             QJsonObject{{QStringLiteral("name"), QStringLiteral("  ")}},  // whitespace -> skipped
             QJsonObject{{QStringLiteral("name"), QStringLiteral("real_tool")}},
         }}};

    const auto tools = sak::ai::AiMcpStdioSession::parseToolsListResultForTesting(result);
    QCOMPARE(tools.size(), 1);
    QCOMPARE(tools.first().name, QStringLiteral("real_tool"));

    QCOMPARE(sak::ai::AiMcpStdioSession::parseToolsListResultForTesting(QJsonObject{}).size(), 0);
}

void AiMcpStdioSessionTests::notOpenSessionRejectsCalls() {
    sak::ai::AiMcpStdioSession session;
    QVERIFY(!session.isOpen());

    QString error;
    QVERIFY(session.listTools(&error).isEmpty());
    QCOMPARE(error, QStringLiteral("MCP session is not open"));

    error.clear();
    QVERIFY(session.callTool(QStringLiteral("list_windows"), QJsonObject{}, &error).isEmpty());
    QCOMPARE(error, QStringLiteral("MCP session is not open"));
}

void AiMcpStdioSessionTests::openFailsCleanlyForMissingCommand() {
    // A missing command must fail open() -- and must spin the worker thread up and
    // back down without hanging or leaking (the failure path calls close()).
    sak::ai::AiMcpStdioSession session;
    sak::ai::AiMcpSessionConfig config;
    config.command = QDir(QCoreApplication::applicationDirPath())
                         .filePath(QStringLiteral("no_such_mcp_server_binary.exe"));
    config.request_timeout_ms = 2000;

    QString error;
    QVERIFY(!session.open(config, &error));
    // Fail closed with the exact missing-command message (the command is a known, absent path).
    QCOMPARE(error, QStringLiteral("MCP stdio command missing: %1").arg(config.command));
    QVERIFY(!session.isOpen());
    // A second open attempt (which internally closes the first) must also be clean.
    QVERIFY(!session.open(config, &error));
    QVERIFY(!session.isOpen());
}

void AiMcpStdioSessionTests::liveWin32McpPersistentSession_optIn() {
    if (qEnvironmentVariable("SAK_MCP_SESSION_LIVE_TEST").trimmed() != QLatin1String("1")) {
        QSKIP(
            "Set SAK_MCP_SESSION_LIVE_TEST=1 to run the bundled Win32 MCP persistent-session "
            "test.");
    }

    const QString command =
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("tools/mcp/win32-mcp-server/win32-mcp-server.exe"));
    QVERIFY2(QFileInfo::exists(command), qPrintable(QStringLiteral("Missing %1").arg(command)));

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("WIN32_MCP_SECURITY_PROFILE"), QStringLiteral("read_only"));
    env.insert(QStringLiteral("WIN32_MCP_REDACT_SENSITIVE_OUTPUT"), QStringLiteral("true"));

    sak::ai::AiMcpStdioSession session;
    QString error;
    QVERIFY2(session.open({.command = command, .environment = env, .request_timeout_ms = 20'000},
                          &error),
             qPrintable(error));
    QVERIFY(session.isOpen());

    // Discovery: tools/list must advertise the server's tools.
    const auto tools = session.listTools(&error);
    QVERIFY2(!tools.isEmpty(), qPrintable(error));
    QVERIFY(std::any_of(tools.cbegin(), tools.cend(), [](const auto& tool) {
        return tool.name == QStringLiteral("list_windows");
    }));

    // Reuse: two tool calls over the SAME process (no re-spawn, no re-initialize).
    for (int i = 0; i < 2; ++i) {
        const QJsonObject result =
            session.callTool(QStringLiteral("list_windows"), QJsonObject{}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!result.value(QStringLiteral("content")).toArray().isEmpty());
    }

    session.close();
    QVERIFY(!session.isOpen());
}

QTEST_GUILESS_MAIN(AiMcpStdioSessionTests)
#include "test_ai_mcp_stdio_session.moc"

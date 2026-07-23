// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_mcp_session_pool.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QtTest/QtTest>

class AiMcpSessionPoolTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void keyIsolatesCommandAndEnvironment();
    void emptyCommandRejected();
    void missingCommandFailsCleanlyAndCloses();
    void liveWin32McpReuseAndIsolation_optIn();

private:
    static sak::ai::AiMcpStdioCallRequest requestWith(const QString& command,
                                                      const QString& security_profile) {
        QProcessEnvironment env;
        env.insert(QStringLiteral("WIN32_MCP_SECURITY_PROFILE"), security_profile);
        return {.command = command,
                .tool_name = QStringLiteral("list_windows"),
                .arguments = QJsonObject{},
                .environment = env,
                .timeout_ms = 4000};
    }
};

void AiMcpSessionPoolTests::keyIsolatesCommandAndEnvironment() {
    using Pool = sak::ai::AiMcpSessionPool;
    const auto read_only = requestWith(QStringLiteral("srv.exe"), QStringLiteral("read_only"));
    const auto read_only2 = requestWith(QStringLiteral("srv.exe"), QStringLiteral("read_only"));
    const auto full = requestWith(QStringLiteral("srv.exe"), QStringLiteral("full"));
    const auto other_cmd = requestWith(QStringLiteral("other.exe"), QStringLiteral("read_only"));

    // Same command + same environment -> same session.
    QCOMPARE(Pool::sessionKeyForTesting(read_only), Pool::sessionKeyForTesting(read_only2));
    // A different security profile is a different launch environment -> MUST NOT
    // share a process (this is the isolation guarantee).
    QVERIFY(Pool::sessionKeyForTesting(read_only) != Pool::sessionKeyForTesting(full));
    // A different command -> different session.
    QVERIFY(Pool::sessionKeyForTesting(read_only) != Pool::sessionKeyForTesting(other_cmd));
}

void AiMcpSessionPoolTests::emptyCommandRejected() {
    sak::ai::AiMcpSessionPool pool;
    sak::ai::AiMcpStdioCallRequest request;
    request.tool_name = QStringLiteral("list_windows");
    QString error;
    QVERIFY(pool.callTool(request, &error).isEmpty());
    QVERIFY(!error.isEmpty());
    QCOMPARE(pool.openSessionCount(), 0);  // nothing was cached
}

void AiMcpSessionPoolTests::missingCommandFailsCleanlyAndCloses() {
    // A missing server binary must fail the call (spinning the session worker
    // thread up and back down without hanging), and closeAll must tear everything
    // down cleanly afterwards.
    sak::ai::AiMcpSessionPool pool;
    const auto request = requestWith(QDir(QCoreApplication::applicationDirPath())
                                         .filePath(QStringLiteral("no_such_mcp_server.exe")),
                                     QStringLiteral("read_only"));

    QString error;
    QVERIFY(pool.callTool(request, &error).isEmpty());
    QVERIFY(!error.isEmpty());

    error.clear();
    QVERIFY(pool.listTools(request, &error).isEmpty());
    QVERIFY(!error.isEmpty());

    pool.closeAll();
    QCOMPARE(pool.openSessionCount(), 0);
    pool.closeAll();  // idempotent
}

void AiMcpSessionPoolTests::liveWin32McpReuseAndIsolation_optIn() {
    if (qEnvironmentVariable("SAK_MCP_SESSION_LIVE_TEST").trimmed() != QLatin1String("1")) {
        QSKIP("Set SAK_MCP_SESSION_LIVE_TEST=1 to run the bundled Win32 MCP pool reuse test.");
    }
    const QString command =
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("tools/mcp/win32-mcp-server/win32-mcp-server.exe"));
    QVERIFY2(QFileInfo::exists(command), qPrintable(QStringLiteral("Missing %1").arg(command)));

    QProcessEnvironment base = QProcessEnvironment::systemEnvironment();
    base.insert(QStringLiteral("WIN32_MCP_REDACT_SENSITIVE_OUTPUT"), QStringLiteral("true"));
    QProcessEnvironment read_only = base;
    read_only.insert(QStringLiteral("WIN32_MCP_SECURITY_PROFILE"), QStringLiteral("read_only"));

    sak::ai::AiMcpStdioCallRequest request{.command = command,
                                           .tool_name = QStringLiteral("list_windows"),
                                           .arguments = QJsonObject{},
                                           .environment = read_only,
                                           .timeout_ms = 20'000};

    sak::ai::AiMcpSessionPool pool;
    QString error;

    // Two calls with the same command+env must REUSE one pooled session.
    for (int i = 0; i < 2; ++i) {
        const QJsonObject message = pool.callTool(request, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!message.value(QStringLiteral("result")).toObject().isEmpty());
    }
    QCOMPARE(pool.openSessionCount(), 1);

    // Discovery over the same pooled session.
    const auto tools = pool.listTools(request, &error);
    QVERIFY2(!tools.isEmpty(), qPrintable(error));
    QCOMPARE(pool.openSessionCount(), 1);

    // A different launch environment must NOT reuse the read-only process.
    QProcessEnvironment full = base;
    full.insert(QStringLiteral("WIN32_MCP_SECURITY_PROFILE"), QStringLiteral("full"));
    sak::ai::AiMcpStdioCallRequest full_request = request;
    full_request.environment = full;
    QVERIFY2(!pool.callTool(full_request, &error).isEmpty(), qPrintable(error));
    QCOMPARE(pool.openSessionCount(), 2);

    pool.closeAll();
    QCOMPARE(pool.openSessionCount(), 0);
}

QTEST_GUILESS_MAIN(AiMcpSessionPoolTests)
#include "test_ai_mcp_session_pool.moc"

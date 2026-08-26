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
    void keyIsolatesTimeout();
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

    // INHERIT-FROM-PARENT IS NOT THE SAME AS EMPTY, and the key could not tell them apart:
    // QProcessEnvironment::toStringList() reports an EMPTY list for an inheriting environment,
    // exactly like one that was deliberately emptied. One hands the server every variable this
    // process holds -- including whatever secrets are in it -- and the other hands it none, so a
    // session opened with the full user environment could have been reused for a call that asked
    // for a bare one. Nothing constructs an inheriting environment today, which is why this never
    // fired; the key is the isolation guarantee, so it must hold before something does.
    auto inheriting = read_only;
    inheriting.environment = QProcessEnvironment(QProcessEnvironment::InheritFromParent);
    auto emptied = read_only;
    emptied.environment = QProcessEnvironment();

    QVERIFY(inheriting.environment.toStringList().isEmpty());
    QVERIFY(emptied.environment.toStringList().isEmpty());
    QVERIFY(inheriting.environment.inheritsFromParent());
    QVERIFY(!emptied.environment.inheritsFromParent());
    // Same command, same timeout, both env lists empty -- and still distinct keys.
    QVERIFY(Pool::sessionKeyForTesting(inheriting) != Pool::sessionKeyForTesting(emptied));
    // Each still keys stably to itself, so this is not just "always different".
    QCOMPARE(Pool::sessionKeyForTesting(inheriting), Pool::sessionKeyForTesting(inheriting));
    QCOMPARE(Pool::sessionKeyForTesting(emptied), Pool::sessionKeyForTesting(emptied));
}

void AiMcpSessionPoolTests::keyIsolatesTimeout() {
    using Pool = sak::ai::AiMcpSessionPool;
    const auto base = requestWith(QStringLiteral("srv.exe"), QStringLiteral("read_only"));
    auto longer = base;
    longer.timeout_ms = base.timeout_ms + 1000;

    // Same command + env but a different per-call timeout MUST key to a distinct
    // session, so a later call never inherits the first call's stale timeout.
    QVERIFY(Pool::sessionKeyForTesting(base) != Pool::sessionKeyForTesting(longer));
    // Identical timeout -> same session (reuse still works).
    const auto same = requestWith(QStringLiteral("srv.exe"), QStringLiteral("read_only"));
    QCOMPARE(Pool::sessionKeyForTesting(base), Pool::sessionKeyForTesting(same));
}

void AiMcpSessionPoolTests::emptyCommandRejected() {
    sak::ai::AiMcpSessionPool pool;
    sak::ai::AiMcpStdioCallRequest request;
    request.tool_name = QStringLiteral("list_windows");
    QString error;
    QVERIFY(pool.callTool(request, &error).isEmpty());
    // Fail closed with the exact empty-command message.
    QCOMPARE(error, QStringLiteral("MCP stdio command is empty"));
    QCOMPARE(pool.pooledSlotCount(), 0);  // nothing was cached
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
    QCOMPARE(pool.pooledSlotCount(), 0);
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
    QCOMPARE(pool.pooledSlotCount(), 1);

    // Discovery over the same pooled session must advertise the win32 catalog's own tools, not just
    // "some non-empty list" -- pin a known base tool.
    const auto tools = pool.listTools(request, &error);
    const bool has_list_windows =
        std::any_of(tools.cbegin(), tools.cend(), [](const sak::ai::AiMcpToolDescriptor& t) {
            return t.name == QLatin1String("list_windows");
        });
    QVERIFY2(has_list_windows, qPrintable(error));
    QCOMPARE(pool.pooledSlotCount(), 1);

    // A different launch environment must NOT reuse the read-only process.
    QProcessEnvironment full = base;
    full.insert(QStringLiteral("WIN32_MCP_SECURITY_PROFILE"), QStringLiteral("full"));
    sak::ai::AiMcpStdioCallRequest full_request = request;
    full_request.environment = full;
    QVERIFY2(!pool.callTool(full_request, &error).isEmpty(), qPrintable(error));
    QCOMPARE(pool.pooledSlotCount(), 2);

    pool.closeAll();
    QCOMPARE(pool.pooledSlotCount(), 0);
}

QTEST_GUILESS_MAIN(AiMcpSessionPoolTests)
#include "test_ai_mcp_session_pool.moc"

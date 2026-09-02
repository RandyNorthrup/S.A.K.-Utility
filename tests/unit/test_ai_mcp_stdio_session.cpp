// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_mcp_jsonrpc.h"
#include "sak/ai/ai_mcp_stdio_session.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QTextStream>
#include <QtTest/QtTest>

namespace {

constexpr auto kFakeMcpModeEnv = "SAK_TEST_MCP_SESSION_SERVER_MODE";
constexpr int kFakeOverToolCeilingCount = 4097;

void writeFakeResponse(QTextStream& output, int id, const QJsonObject& result) {
    const QJsonObject response{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                               {QStringLiteral("id"), id},
                               {QStringLiteral("result"), result}};
    output << QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact)) << '\n';
    output.flush();
}

QJsonObject fakeToolsListResult(const QString& mode, const QString& cursor) {
    if (mode == QLatin1String("tool-ceiling")) {
        QJsonArray tools;
        for (int index = 0; index < kFakeOverToolCeilingCount; ++index) {
            tools.append(
                QJsonObject{{QStringLiteral("name"), QStringLiteral("tool_%1").arg(index)}});
        }
        return QJsonObject{{QStringLiteral("tools"), tools}};
    }

    const QJsonObject tool{
        {QStringLiteral("name"),
         cursor.isEmpty() ? QStringLiteral("first_tool") : QStringLiteral("second_tool")}};
    QJsonObject result{{QStringLiteral("tools"), QJsonArray{tool}}};
    if (mode == QLatin1String("cursor-cycle")) {
        result[QStringLiteral("nextCursor")] = QStringLiteral("cycle");
    } else if (mode == QLatin1String("page-ceiling")) {
        const int page = cursor.isEmpty() ? 1 : cursor.mid(5).toInt() + 1;
        result[QStringLiteral("nextCursor")] = QStringLiteral("page_%1").arg(page);
    } else if (cursor.isEmpty()) {
        result[QStringLiteral("nextCursor")] = QStringLiteral("page-2");
    }
    return result;
}

int runFakeMcpServer(const QString& mode) {
    QTextStream input(stdin, QIODevice::ReadOnly);
    QTextStream output(stdout, QIODevice::WriteOnly);
    for (;;) {
        const QString line = input.readLine();
        if (line.isNull()) {
            return 0;
        }
        const QJsonObject request = QJsonDocument::fromJson(line.toUtf8()).object();
        const QString method = request.value(QStringLiteral("method")).toString();
        if (method == QLatin1String("initialize")) {
            writeFakeResponse(output,
                              request.value(QStringLiteral("id")).toInt(),
                              QJsonObject{{QStringLiteral("protocolVersion"),
                                           QStringLiteral("2024-11-05")}});
            continue;
        }
        if (method == QLatin1String("notifications/initialized")) {
            if (mode == QLatin1String("exit-after-handshake")) {
                return 0;
            }
            continue;
        }
        if (method == QLatin1String("tools/list")) {
            const QString cursor = request.value(QStringLiteral("params"))
                                       .toObject()
                                       .value(QStringLiteral("cursor"))
                                       .toString();
            writeFakeResponse(output,
                              request.value(QStringLiteral("id")).toInt(),
                              fakeToolsListResult(mode, cursor));
        }
    }
}

template <typename Session>
concept CanOpenWithoutErrorSink =
    requires(Session& session, const sak::ai::AiMcpSessionConfig& config) { session.open(config); };

template <typename Session>
concept CanListWithoutErrorSink = requires(Session& session) { session.listTools(); };

template <typename Session>
concept CanCallWithoutErrorSink = requires(Session& session, const QJsonObject& arguments) {
    session.callTool(QStringLiteral("tool"), arguments);
};

template <typename Session>
concept CanUseNullErrorSink = requires(Session& session,
                                       const sak::ai::AiMcpSessionConfig& config,
                                       const QJsonObject& arguments) {
    session.open(config, nullptr);
    session.listTools(nullptr);
    session.callTool(QStringLiteral("tool"), arguments, nullptr);
};

bool openFakeSession(sak::ai::AiMcpStdioSession* session,
                     const QString& mode,
                     QString& error_message) {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QString::fromLatin1(kFakeMcpModeEnv), mode);
    return session->open({.command = QCoreApplication::applicationFilePath(),
                          .environment = environment,
                          .request_timeout_ms = 5000},
                         error_message);
}

}  // namespace

class AiMcpStdioSessionTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void buildsToolsListPayload();
    void validatesInitializeProtocolVersion();
    void emptyJsonRpcErrorGetsDefaultMessage();
    void parsesToolsListPage();
    void rejectsMalformedToolsListPage();
    void persistenceErrorSinkIsMandatory();
    void notOpenSessionRejectsCalls();
    void openFailsCleanlyForMissingCommand();
    void listToolsFollowsPagination();
    void listToolsRejectsCursorCycle();
    void listToolsRejectsPageCeiling();
    void listToolsRejectsToolCeiling();
    void isOpenClearsAfterServerExit();
    void liveWin32McpPersistentSession_optIn();
};

void AiMcpStdioSessionTests::buildsToolsListPayload() {
    const QJsonObject payload = sak::ai::mcp::toolsListPayload(7);
    QCOMPARE(payload.value(QStringLiteral("jsonrpc")).toString(), QStringLiteral("2.0"));
    QCOMPARE(payload.value(QStringLiteral("id")).toInt(), 7);
    QCOMPARE(payload.value(QStringLiteral("method")).toString(), QStringLiteral("tools/list"));
    QCOMPARE(payload.value(QStringLiteral("params")).toObject(), QJsonObject{});

    const QJsonObject next = sak::ai::mcp::toolsListPayload(8, QStringLiteral("page-2"));
    QCOMPARE(
        next.value(QStringLiteral("params")).toObject().value(QStringLiteral("cursor")).toString(),
        QStringLiteral("page-2"));
}

void AiMcpStdioSessionTests::validatesInitializeProtocolVersion() {
    const QJsonObject valid{
        {QStringLiteral("result"),
         QJsonObject{{QStringLiteral("protocolVersion"), QStringLiteral("2024-11-05")}}}};
    QVERIFY(sak::ai::mcp::initializeResponseError(valid).isEmpty());

    QCOMPARE(sak::ai::mcp::initializeResponseError(QJsonObject{}),
             QStringLiteral("MCP initialize response is missing a result object"));
    const QJsonObject missing_version{{QStringLiteral("result"), QJsonObject{}}};
    QCOMPARE(sak::ai::mcp::initializeResponseError(missing_version),
             QStringLiteral("MCP initialize result missing protocolVersion"));
}

void AiMcpStdioSessionTests::emptyJsonRpcErrorGetsDefaultMessage() {
    const QJsonObject empty_message{
        {QStringLiteral("error"), QJsonObject{{QStringLiteral("message"), QString()}}}};
    QCOMPARE(sak::ai::mcp::jsonRpcErrorMessage(empty_message),
             QStringLiteral("MCP JSON-RPC error"));

    const QJsonObject detail{
        {QStringLiteral("error"),
         QJsonObject{{QStringLiteral("message"), QStringLiteral("tool refused")}}}};
    QCOMPARE(sak::ai::mcp::jsonRpcErrorMessage(detail), QStringLiteral("tool refused"));
}

void AiMcpStdioSessionTests::parsesToolsListPage() {
    const QJsonObject result{
        {QStringLiteral("tools"),
         QJsonArray{
             QJsonObject{{QStringLiteral("name"), QStringLiteral("list_windows")},
                         {QStringLiteral("description"), QStringLiteral("List top-level windows")},
                         {QStringLiteral("inputSchema"),
                          QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}}},
             QJsonObject{{QStringLiteral("name"), QStringLiteral("focus_window")}},
         }}};

    const auto page = sak::ai::AiMcpStdioSession::parseToolsListPage(result);
    QVERIFY(page.has_value());
    QCOMPARE(page->tools.size(), 2);
    QCOMPARE(page->tools[0].name, QStringLiteral("list_windows"));
    QCOMPARE(page->tools[0].description, QStringLiteral("List top-level windows"));
    QCOMPARE(page->tools[0].input_schema.value(QStringLiteral("type")).toString(),
             QStringLiteral("object"));
    QCOMPARE(page->tools[1].name, QStringLiteral("focus_window"));
}

void AiMcpStdioSessionTests::rejectsMalformedToolsListPage() {
    const auto missing_tools = sak::ai::AiMcpStdioSession::parseToolsListPage(QJsonObject{});
    QVERIFY(!missing_tools.has_value());
    QVERIFY(missing_tools.error().contains(QStringLiteral("tools array")));

    const QJsonObject nameless{
        {QStringLiteral("tools"),
         QJsonArray{QJsonObject{{QStringLiteral("description"), QStringLiteral("no name")}}}}};
    const auto missing_name = sak::ai::AiMcpStdioSession::parseToolsListPage(nameless);
    QVERIFY(!missing_name.has_value());
    QVERIFY(missing_name.error().contains(QStringLiteral("no name")));

    const QJsonObject bad_cursor{{QStringLiteral("tools"), QJsonArray{}},
                                 {QStringLiteral("nextCursor"), 7}};
    const auto cursor = sak::ai::AiMcpStdioSession::parseToolsListPage(bad_cursor);
    QVERIFY(!cursor.has_value());
    QVERIFY(cursor.error().contains(QStringLiteral("nextCursor")));

    const QJsonObject null_cursor{{QStringLiteral("tools"), QJsonArray{}},
                                  {QStringLiteral("nextCursor"), QJsonValue::Null}};
    const auto null_result = sak::ai::AiMcpStdioSession::parseToolsListPage(null_cursor);
    QVERIFY(!null_result.has_value());
    QVERIFY(null_result.error().contains(QStringLiteral("nextCursor")));
}

void AiMcpStdioSessionTests::persistenceErrorSinkIsMandatory() {
    using Session = sak::ai::AiMcpStdioSession;
    QVERIFY(!CanOpenWithoutErrorSink<Session>);
    QVERIFY(!CanListWithoutErrorSink<Session>);
    QVERIFY(!CanCallWithoutErrorSink<Session>);
    QVERIFY(!CanUseNullErrorSink<Session>);
}

void AiMcpStdioSessionTests::notOpenSessionRejectsCalls() {
    sak::ai::AiMcpStdioSession session;
    QVERIFY(!session.isOpen());

    QString error;
    QVERIFY(session.listTools(error).isEmpty());
    QCOMPARE(error, QStringLiteral("MCP session is not open"));

    error.clear();
    QVERIFY(session.callTool(QStringLiteral("list_windows"), QJsonObject{}, error).isEmpty());
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
    QVERIFY(!session.open(config, error));
    // Fail closed with the exact missing-command message (the command is a known, absent path).
    QCOMPARE(error, QStringLiteral("MCP stdio command missing: %1").arg(config.command));
    QVERIFY(!session.isOpen());
    // A second open attempt (which internally closes the first) must also be clean.
    QVERIFY(!session.open(config, error));
    QVERIFY(!session.isOpen());
}

void AiMcpStdioSessionTests::listToolsFollowsPagination() {
    sak::ai::AiMcpStdioSession session;
    QString error;
    QVERIFY2(openFakeSession(&session, QStringLiteral("paginate"), error), qPrintable(error));

    const auto tools = session.listTools(error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(tools.size(), 2);
    QCOMPARE(tools.at(0).name, QStringLiteral("first_tool"));
    QCOMPARE(tools.at(1).name, QStringLiteral("second_tool"));
    session.close();
}

void AiMcpStdioSessionTests::listToolsRejectsCursorCycle() {
    sak::ai::AiMcpStdioSession session;
    QString error;
    QVERIFY2(openFakeSession(&session, QStringLiteral("cursor-cycle"), error), qPrintable(error));
    QVERIFY(session.listTools(error).isEmpty());
    QCOMPARE(error, QStringLiteral("MCP tools/list returned a cursor cycle"));
    session.close();
}

void AiMcpStdioSessionTests::listToolsRejectsPageCeiling() {
    sak::ai::AiMcpStdioSession session;
    QString error;
    QVERIFY2(openFakeSession(&session, QStringLiteral("page-ceiling"), error), qPrintable(error));
    QVERIFY(session.listTools(error).isEmpty());
    QCOMPARE(error, QStringLiteral("MCP tools/list exceeded the 128-page ceiling"));
    session.close();
}

void AiMcpStdioSessionTests::listToolsRejectsToolCeiling() {
    sak::ai::AiMcpStdioSession session;
    QString error;
    QVERIFY2(openFakeSession(&session, QStringLiteral("tool-ceiling"), error), qPrintable(error));
    QVERIFY(session.listTools(error).isEmpty());
    QCOMPARE(error, QStringLiteral("MCP tools/list exceeded the 4096-tool ceiling"));
    session.close();
}

void AiMcpStdioSessionTests::isOpenClearsAfterServerExit() {
    sak::ai::AiMcpStdioSession session;
    QString error;
    QVERIFY2(openFakeSession(&session, QStringLiteral("exit-after-handshake"), error),
             qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!session.isOpen(), 5000);
    session.close();
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
                          error),
             qPrintable(error));
    QVERIFY(session.isOpen());

    // Discovery: tools/list must advertise the server's tools.
    const auto tools = session.listTools(error);
    QVERIFY2(!tools.isEmpty(), qPrintable(error));
    QVERIFY(std::any_of(tools.cbegin(), tools.cend(), [](const auto& tool) {
        return tool.name == QStringLiteral("list_windows");
    }));

    // Reuse: two tool calls over the SAME process (no re-spawn, no re-initialize).
    for (int i = 0; i < 2; ++i) {
        const QJsonObject result =
            session.callTool(QStringLiteral("list_windows"), QJsonObject{}, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!result.value(QStringLiteral("content")).toArray().isEmpty());
    }

    session.close();
    QVERIFY(!session.isOpen());
}

int main(int argc, char* argv[]) {
    const QString fake_mode = qEnvironmentVariable(kFakeMcpModeEnv).trimmed();
    if (!fake_mode.isEmpty()) {
        return runFakeMcpServer(fake_mode);
    }
    QCoreApplication application(argc, argv);
    AiMcpStdioSessionTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "test_ai_mcp_stdio_session.moc"

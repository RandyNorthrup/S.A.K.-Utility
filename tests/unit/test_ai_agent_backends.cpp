// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_agent_backend.h"
#include "sak/ai/ai_agent_runtime.h"
#include "sak/ai/ai_claude_code_backend.h"
#include "sak/ai/ai_codex_backend.h"
#include "sak/ai/ai_gemini_cli_backend.h"
#include "sak/ai/ai_muse_code_backend.h"
#include "sak/ai/ai_stdio_json_process.h"
#include "sak/ai/ai_tool_bridge_server.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace sak::ai;

class AiAgentBackendTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void takeJsonLines_splitsCompleteObjectsAndKeepsRemainder();
    void jsonRpcPeer_routesNotificationsAndRequests();
    void toolBridge_answersMcpHandshakeAndListsSakTools();
    void toolBridge_toolsCallIsHandedToTheApp();
    void runtime_locatesBundledCodexAndIsolatesEnvironment();
    void runtime_locatesGeminiNodeBundleAndMuseInstall();
    void runtime_reportsMissingRuntime();
    void knowledge_dropsToneLinesAndNamesFilePerRuntime();
    void codex_threadParamsMapPolicyAndAddSakServer();
    void codex_turnInputCarriesImagesAsDataUrls();
    void claude_launchArgumentsStayWithinHostingScope();
    void claude_parsesAuthStatus();
    void gemini_convertsMcpEnvAndPicksPermissionOptions();
    void muse_uuidV7AndApprovalChoices();
    void agentBackend_toolCallRoundTripAndCompletion();
    void agentBackend_approvalPolicy();
};

namespace {

void touch(const QString& path) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("x");
}

class FakeAgentBackend : public AiAgentBackend {
public:
    FakeAgentBackend(AiAgentRuntime runtime, QObject* parent = nullptr)
        : AiAgentBackend(ModelProviderId::OpenAI, std::move(runtime), parent) {}

    int begin_count{0};
    int interrupt_count{0};
    QList<QPair<QString, AiApprovalDecision>> answers;
    TurnRequest last_turn;

    void finish(const QString& text) {
        appendAssistantText(text);
        completeTurn(QStringLiteral("thread_1"));
    }
    void ask(const QString& id) { raiseApproval(AiAgentApproval{id, id, {}, true}); }
    QJsonObject toolEntry() { return toolServerEntry(); }

protected:
    void beginTurn(const TurnRequest& turn) override {
        ++begin_count;
        last_turn = turn;
    }
    void interruptTurn() override { ++interrupt_count; }
    void answerApproval(const QString& id, AiApprovalDecision decision) override {
        answers.append({id, decision});
    }
};

AiAgentRuntime runtimeIn(const QTemporaryDir& dir, QProcessEnvironment env = {}) {
    return AiAgentRuntime(dir.filePath(QStringLiteral("app")),
                          dir.filePath(QStringLiteral("data")),
                          env);
}

}  // namespace

void AiAgentBackendTests::takeJsonLines_splitsCompleteObjectsAndKeepsRemainder() {
    QByteArray buffer("{\"a\":1}\r\nnot json\n{\"b\":2}\n{\"partial\":");
    const auto messages = AiStdioJsonProcess::takeJsonLines(&buffer);
    QCOMPARE(messages.size(), 2);
    QCOMPARE(messages.at(1).value(QStringLiteral("b")).toInt(), 2);
    QCOMPARE(buffer, QByteArray("{\"partial\":"));
}

void AiAgentBackendTests::jsonRpcPeer_routesNotificationsAndRequests() {
    AiStdioJsonProcess process;
    AiJsonRpcPeer peer(&process, true);
    QSignalSpy notifications(&peer, &AiJsonRpcPeer::notificationReceived);
    QSignalSpy requests(&peer, &AiJsonRpcPeer::requestReceived);
    peer.handleMessage(QJsonObject{{QStringLiteral("method"), QStringLiteral("turn/started")}});
    peer.handleMessage(QJsonObject{{QStringLiteral("id"), 7},
                                   {QStringLiteral("method"), QStringLiteral("approval/request")}});
    QCOMPARE(notifications.count(), 1);
    QCOMPARE(requests.count(), 1);
    QCOMPARE(requests.first().at(1).toString(), QStringLiteral("approval/request"));

    QString failure;
    peer.request(QStringLiteral("initialize"),
                 {},
                 [&failure](const QJsonObject&, const QString& e) { failure = e; });
    QVERIFY(!failure.isEmpty());  // Not running: fails immediately, never hangs.
}

void AiAgentBackendTests::toolBridge_answersMcpHandshakeAndListsSakTools() {
    AiToolBridgeServer bridge;
    const QJsonObject init = bridge.handleMcpMessage(
        QStringLiteral("c1"),
        QJsonObject{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), 0},
            {QStringLiteral("method"), QStringLiteral("initialize")},
            {QStringLiteral("params"),
             QJsonObject{{QStringLiteral("protocolVersion"), QStringLiteral("2025-11-25")}}}});
    const QJsonObject result = init.value(QStringLiteral("result")).toObject();
    QCOMPARE(result.value(QStringLiteral("protocolVersion")).toString(),
             QStringLiteral("2025-11-25"));
    QVERIFY(
        result.value(QStringLiteral("capabilities")).toObject().contains(QStringLiteral("tools")));

    QVERIFY(bridge
                .handleMcpMessage(QStringLiteral("c1"),
                                  QJsonObject{{QStringLiteral("method"),
                                               QStringLiteral("notifications/initialized")}})
                .isEmpty());

    const QJsonObject list = bridge.handleMcpMessage(QStringLiteral("c1"),
                                                     QJsonObject{{QStringLiteral("id"), 1},
                                                                 {QStringLiteral("method"),
                                                                  QStringLiteral("tools/list")}});
    const QJsonArray tools =
        list.value(QStringLiteral("result")).toObject().value(QStringLiteral("tools")).toArray();
    QStringList names;
    for (const auto& value : tools) {
        names << value.toObject().value(QStringLiteral("name")).toString();
        QVERIFY(value.toObject().value(QStringLiteral("inputSchema")).isObject());
    }
    QVERIFY(names.contains(QStringLiteral("run_powershell")));
    QVERIFY(names.contains(QStringLiteral("sak_package_manager")));

    const QJsonObject unknown = bridge.handleMcpMessage(
        QStringLiteral("c1"),
        QJsonObject{{QStringLiteral("id"), 2},
                    {QStringLiteral("method"), QStringLiteral("resources/list")}});
    QVERIFY(unknown.contains(QStringLiteral("error")));
}

void AiAgentBackendTests::toolBridge_toolsCallIsHandedToTheApp() {
    AiToolBridgeServer bridge;
    QSignalSpy calls(&bridge, &AiToolBridgeServer::toolCallRequested);
    const QJsonObject reply = bridge.handleMcpMessage(
        QStringLiteral("c1"),
        QJsonObject{
            {QStringLiteral("id"), 5},
            {QStringLiteral("method"), QStringLiteral("tools/call")},
            {QStringLiteral("params"),
             QJsonObject{{QStringLiteral("name"), QStringLiteral("run_powershell")},
                         {QStringLiteral("arguments"),
                          QJsonObject{{QStringLiteral("command"), QStringLiteral("Get-Disk")}}}}}});
    QVERIFY(reply.isEmpty());  // Answered later, when the panel finishes the tool.
    QCOMPARE(calls.count(), 1);
    QCOMPARE(calls.first().at(1).toString(), QStringLiteral("run_powershell"));
    QVERIFY(calls.first().at(2).toString().contains(QStringLiteral("Get-Disk")));
    QCOMPARE(bridge.pendingCallCount(), 1);
    bridge.completeToolCall(calls.first().at(0).toString(), QStringLiteral("{}"), false);
    QCOMPARE(bridge.pendingCallCount(), 0);
    QVERIFY(bridge.token().size() >= 32);
}

void AiAgentBackendTests::runtime_locatesBundledCodexAndIsolatesEnvironment() {
    QTemporaryDir dir;
    touch(dir.filePath(QStringLiteral("app/tools/ai_agents/codex/bin/") +
                       AiAgentRuntime::executableName(QStringLiteral("codex"))));
    QProcessEnvironment env;
    env.insert(QStringLiteral("OPENAI_API_KEY"), QStringLiteral("sk-should-not-leak"));
    env.insert(QStringLiteral("PATH"), QStringLiteral("/bin"));
    const AiAgentLaunch launch = runtimeIn(dir, env).serverLaunch(ModelProviderId::OpenAI);
    QVERIFY(launch.available);
    QVERIFY(launch.bundled);
    QCOMPARE(launch.spec.arguments, QStringList{QStringLiteral("app-server")});
    QVERIFY(!launch.spec.environment.contains(QStringLiteral("OPENAI_API_KEY")));
    QVERIFY(launch.spec.environment.value(QStringLiteral("CODEX_HOME"))
                .contains(QStringLiteral("codex")));
    QVERIFY(QFileInfo(launch.home_directory).isDir());  // Codex requires it to exist.
}

void AiAgentBackendTests::runtime_locatesGeminiNodeBundleAndMuseInstall() {
    QTemporaryDir dir;
    touch(dir.filePath(QStringLiteral("app/tools/ai_agents/gemini/node/") +
                       AiAgentRuntime::executableName(QStringLiteral("node"))));
    touch(dir.filePath(QStringLiteral("app/tools/ai_agents/gemini/bundle/gemini.js")));
    const QString muse_dir = dir.filePath(QStringLiteral("local/Programs/muse"));
    touch(
        QDir(muse_dir).filePath(AiAgentRuntime::executableName(QStringLiteral("muse-bin-1.3.0"))));
    QFile version(QDir(muse_dir).filePath(QStringLiteral(".muse-version")));
    QVERIFY(version.open(QIODevice::WriteOnly));
    version.write("1.3.0\n");
    version.close();
    QProcessEnvironment env;
    env.insert(QStringLiteral("LOCALAPPDATA"), dir.filePath(QStringLiteral("local")));
    env.insert(QStringLiteral("GEMINI_API_KEY"), QStringLiteral("AIza-should-not-leak"));
    const AiAgentRuntime runtime = runtimeIn(dir, env);

    const AiAgentLaunch gemini = runtime.serverLaunch(ModelProviderId::Google);
    QVERIFY(gemini.available);
    QVERIFY(gemini.spec.arguments.first().endsWith(QStringLiteral("gemini.js")));
    QCOMPARE(gemini.spec.arguments.last(), QStringLiteral("--acp"));
    QVERIFY(!gemini.spec.environment.contains(QStringLiteral("GEMINI_API_KEY")));
    QCOMPARE(gemini.spec.environment.value(QStringLiteral("GEMINI_CLI_NO_RELAUNCH")),
             QStringLiteral("true"));

    const AiAgentLaunch muse = runtime.serverLaunch(ModelProviderId::Muse);
    QVERIFY(muse.available);
    QVERIFY(!muse.bundled);
    QVERIFY(muse.spec.program.contains(QStringLiteral("muse-bin-1.3.0")));
    QCOMPARE(muse.spec.arguments, QStringList{QStringLiteral("serve")});
    QVERIFY(muse.home_directory.isEmpty());
}

void AiAgentBackendTests::runtime_reportsMissingRuntime() {
    QTemporaryDir dir;
    const AiAgentLaunch launch = runtimeIn(dir).serverLaunch(ModelProviderId::Anthropic);
    QVERIFY(!launch.available);
    QVERIFY(launch.missing_reason.contains(QStringLiteral("Claude Code")));
}

void AiAgentBackendTests::knowledge_dropsToneLinesAndNamesFilePerRuntime() {
    const QString text = AiAgentBackend::knowledgeText(
        QStringLiteral("You are the S.A.K. Utility AI Assistant for Windows PC technicians.\n"
                       "Be practical, concise, and verify fixes when tools are available.\n"
                       "Use sak_package_manager before raw choco."));
    QVERIFY(!text.contains(QStringLiteral("Be practical")));
    QVERIFY(!text.contains(QStringLiteral("You are the S.A.K.")));
    QVERIFY(text.contains(QStringLiteral("sak_package_manager")));
    QVERIFY(text.contains(QStringLiteral("MCP server named `sak`")));
    QCOMPARE(AiAgentBackend::knowledgeFileName(ModelProviderId::Anthropic),
             QStringLiteral("CLAUDE.md"));
    QCOMPARE(AiAgentBackend::knowledgeFileName(ModelProviderId::Google),
             QStringLiteral("GEMINI.md"));
    QCOMPARE(AiAgentBackend::knowledgeFileName(ModelProviderId::OpenAI),
             QStringLiteral("AGENTS.md"));
}

void AiAgentBackendTests::codex_threadParamsMapPolicyAndAddSakServer() {
    OpenAIResponseRequest request;
    request.model = QStringLiteral("gpt-5.5");
    const QJsonObject server{{QStringLiteral("command"), QStringLiteral("bridge")}};
    const QJsonObject ask = CodexAppServerBackend::threadStartParams(
        request, QStringLiteral("C:/ws"), AiApprovalPolicy::Ask, server);
    QCOMPARE(ask.value(QStringLiteral("approvalPolicy")).toString(), QStringLiteral("untrusted"));
    QCOMPARE(ask.value(QStringLiteral("sandbox")).toString(), QStringLiteral("danger-full-access"));
    const QJsonObject sak = ask.value(QStringLiteral("config"))
                                .toObject()
                                .value(QStringLiteral("mcp_servers.sak"))
                                .toObject();
    QCOMPARE(sak.value(QStringLiteral("command")).toString(), QStringLiteral("bridge"));
    QVERIFY(sak.value(QStringLiteral("tool_timeout_sec")).toInt() > 60);

    const QJsonObject research = CodexAppServerBackend::threadStartParams(
        request, QStringLiteral("C:/ws"), AiApprovalPolicy::DenyAll, {});
    QCOMPARE(research.value(QStringLiteral("sandbox")).toString(), QStringLiteral("read-only"));
    QVERIFY(!research.contains(QStringLiteral("config")));
    const QJsonObject unattended = CodexAppServerBackend::threadStartParams(
        request, QStringLiteral("C:/ws"), AiApprovalPolicy::AllowAll, {});
    QCOMPARE(unattended.value(QStringLiteral("approvalPolicy")).toString(),
             QStringLiteral("never"));

    const TokenUsage usage = CodexAppServerBackend::usageFromBreakdown(
        QJsonObject{{QStringLiteral("inputTokens"), 10},
                    {QStringLiteral("outputTokens"), 4},
                    {QStringLiteral("reasoningOutputTokens"), 2},
                    {QStringLiteral("totalTokens"), 14}});
    QCOMPARE(usage.total_tokens, 14);
    QCOMPARE(usage.reasoning_tokens, 2);
}

void AiAgentBackendTests::codex_turnInputCarriesImagesAsDataUrls() {
    OpenAIResponseRequest request;
    request.input = QStringLiteral("What is on screen?");
    OpenAIInputAttachment image;
    image.type = OpenAIInputAttachment::Type::Image;
    image.image_url = QStringLiteral("data:image/png;base64,iVBOR");
    request.attachments = {image};
    const QJsonArray input = CodexAppServerBackend::turnInput(request);
    QCOMPARE(input.size(), 2);
    QCOMPARE(input.at(0).toObject().value(QStringLiteral("type")).toString(),
             QStringLiteral("text"));
    QCOMPARE(input.at(1).toObject().value(QStringLiteral("url")).toString(),
             QStringLiteral("data:image/png;base64,iVBOR"));
}

void AiAgentBackendTests::claude_launchArgumentsStayWithinHostingScope() {
    OpenAIResponseRequest request;
    request.model = QStringLiteral("opus");
    request.reasoning_effort = QStringLiteral("high");
    const QStringList args = ClaudeCodeBackend::launchArguments(
        request,
        QStringLiteral("sess-1"),
        QJsonObject{{QStringLiteral("command"), QStringLiteral("bridge")}});
    QVERIFY(args.contains(QStringLiteral("stream-json")));
    QVERIFY(args.contains(QStringLiteral("--resume=sess-1")));
    QCOMPARE(args.at(args.indexOf(QStringLiteral("--model")) + 1), QStringLiteral("opus"));
    QCOMPARE(args.at(args.indexOf(QStringLiteral("--allowedTools")) + 1),
             QStringLiteral("mcp__sak__*"));
    const QJsonObject config =
        QJsonDocument::fromJson(args.at(args.indexOf(QStringLiteral("--mcp-config")) + 1).toUtf8())
            .object();
    QCOMPARE(config.value(QStringLiteral("mcpServers"))
                 .toObject()
                 .value(QStringLiteral("sak"))
                 .toObject()
                 .value(QStringLiteral("type")),
             QJsonValue(QStringLiteral("stdio")));
    // Hosting scope: Claude Code keeps its own system prompt and auth.
    QVERIFY(!args.contains(QStringLiteral("--system-prompt")));
    QVERIFY(!args.contains(QStringLiteral("--append-system-prompt")));
    QVERIFY(!args.contains(QStringLiteral("--bare")));
}

void AiAgentBackendTests::claude_parsesAuthStatus() {
    const AiAccountStatus signed_in = ClaudeCodeBackend::parseAuthStatus(
        R"({"loggedIn":true,"authMethod":"claude.ai","email":"tech@example.com","subscriptionType":"max"})");
    QVERIFY(signed_in.signed_in);
    QCOMPARE(signed_in.account_label, QStringLiteral("tech@example.com"));
    QCOMPARE(signed_in.plan, QStringLiteral("max"));
    QVERIFY(
        !ClaudeCodeBackend::parseAuthStatus(R"({"loggedIn":false,"authMethod":"none"})").signed_in);
}

void AiAgentBackendTests::gemini_convertsMcpEnvAndPicksPermissionOptions() {
    const QJsonArray servers = GeminiCliBackend::mcpServers(QJsonObject{
        {QStringLiteral("command"), QStringLiteral("bridge")},
        {QStringLiteral("args"), QJsonArray{QStringLiteral("--pipe"), QStringLiteral("p")}},
        {QStringLiteral("env"), QJsonObject{{QStringLiteral("TOKEN"), QStringLiteral("t")}}}});
    const QJsonObject server = servers.first().toObject();
    QCOMPARE(server.value(QStringLiteral("name")).toString(), QStringLiteral("sak"));
    QCOMPARE(server.value(QStringLiteral("env"))
                 .toArray()
                 .first()
                 .toObject()
                 .value(QStringLiteral("name")),
             QJsonValue(QStringLiteral("TOKEN")));
    QVERIFY(GeminiCliBackend::mcpServers({}).isEmpty());

    const QJsonArray options{
        QJsonObject{{QStringLiteral("optionId"), QStringLiteral("proceed_once")},
                    {QStringLiteral("kind"), QStringLiteral("allow_once")}},
        QJsonObject{{QStringLiteral("optionId"), QStringLiteral("proceed_always")},
                    {QStringLiteral("kind"), QStringLiteral("allow_always")}},
        QJsonObject{{QStringLiteral("optionId"), QStringLiteral("cancel")},
                    {QStringLiteral("kind"), QStringLiteral("reject_once")}}};
    QCOMPARE(GeminiCliBackend::optionForDecision(options, AiApprovalDecision::AllowOnce),
             QStringLiteral("proceed_once"));
    QCOMPARE(GeminiCliBackend::optionForDecision(options, AiApprovalDecision::AllowForSession),
             QStringLiteral("proceed_always"));
    QCOMPARE(GeminiCliBackend::optionForDecision(options, AiApprovalDecision::Deny),
             QStringLiteral("cancel"));
}

void AiAgentBackendTests::muse_uuidV7AndApprovalChoices() {
    const QString id = MuseCodeBackend::uuidV7();
    QCOMPARE(id.size(), 36);
    QCOMPARE(id.at(14), QChar(u'7'));
    QVERIFY(QStringLiteral("89ab").contains(id.at(19)));
    QVERIFY(MuseCodeBackend::uuidV7() != id);

    const QJsonArray choices{QJsonObject{{QStringLiteral("choiceId"), QStringLiteral("c1")},
                                         {QStringLiteral("decision"), QStringLiteral("approved")},
                                         {QStringLiteral("scope"), QStringLiteral("once")}},
                             QJsonObject{{QStringLiteral("choiceId"), QStringLiteral("c2")},
                                         {QStringLiteral("decision"),
                                          QStringLiteral("approvedForSession")},
                                         {QStringLiteral("scope"), QStringLiteral("session")}},
                             QJsonObject{{QStringLiteral("choiceId"), QStringLiteral("c3")},
                                         {QStringLiteral("decision"), QStringLiteral("denied")},
                                         {QStringLiteral("scope"), QStringLiteral("once")}}};
    QCOMPARE(MuseCodeBackend::choiceForDecision(choices, AiApprovalDecision::AllowOnce),
             QStringLiteral("c1"));
    QCOMPARE(MuseCodeBackend::choiceForDecision(choices, AiApprovalDecision::AllowForSession),
             QStringLiteral("c2"));
    QCOMPARE(MuseCodeBackend::choiceForDecision(choices, AiApprovalDecision::Deny),
             QStringLiteral("c3"));
    QCOMPARE(MuseCodeBackend::approvalModeFor(AiApprovalPolicy::Ask),
             QStringLiteral("promptUnmatched"));
    QVERIFY(MuseCodeBackend::isSakTool(QStringLiteral("mcp__sak__run_powershell")));
    QVERIFY(!MuseCodeBackend::isSakTool(QStringLiteral("shell")));
}

void AiAgentBackendTests::agentBackend_toolCallRoundTripAndCompletion() {
    QTemporaryDir dir;
    touch(dir.filePath(QStringLiteral("app/tools/ai_agents/codex/") +
                       AiAgentRuntime::executableName(QStringLiteral("codex"))));
    FakeAgentBackend backend(runtimeIn(dir));
    backend.setWorkspaceDirectory(dir.filePath(QStringLiteral("ws")));
    QSignalSpy responses(&backend, &AiChatBackend::responseReady);
    QSignalSpy started(&backend, &AiChatBackend::requestStarted);

    OpenAIResponseRequest request;
    request.input = QStringLiteral("Check disks");
    request.instructions = QStringLiteral("Use run_powershell for diagnostics.");
    request.enable_local_tools = true;
    backend.createResponse(request);
    QCOMPARE(backend.begin_count, 1);
    QVERIFY(backend.last_turn.attach_tools);
    QVERIFY(backend.isBusy());
    QVERIFY(QFile::exists(
        QDir(dir.filePath(QStringLiteral("ws"))).filePath(QStringLiteral("AGENTS.md"))));

    // The runtime calls a S.A.K. tool over MCP: the panel receives a function call.
    QVERIFY(!backend.toolEntry().isEmpty());
    auto* bridge = backend.findChild<AiToolBridgeServer*>();
    QVERIFY(bridge);
    (void)bridge->handleMcpMessage(
        QStringLiteral("c"),
        QJsonObject{{QStringLiteral("id"), 1},
                    {QStringLiteral("method"), QStringLiteral("tools/call")},
                    {QStringLiteral("params"),
                     QJsonObject{{QStringLiteral("name"), QStringLiteral("run_powershell")}}}});
    QCOMPARE(responses.count(), 1);
    const auto call = responses.takeFirst().first().value<OpenAIResponseResult>();
    QCOMPARE(call.function_calls.size(), 1);
    QVERIFY(!backend.isBusy());  // The panel's tool turn owns busy state now.

    OpenAIResponseRequest outputs;
    outputs.function_outputs = {
        {call.function_calls.first().call_id, QStringLiteral("{\"ok\":1}")}};
    backend.createResponse(outputs);
    QCOMPARE(bridge->pendingCallCount(), 0);
    QVERIFY(started.count() >= 2);
    QCOMPARE(backend.begin_count, 1);  // Same turn continues; no new prompt.

    backend.finish(QStringLiteral("Disks look healthy."));
    QCOMPARE(responses.count(), 1);
    const auto final_result = responses.takeFirst().first().value<OpenAIResponseResult>();
    QCOMPARE(final_result.output_text, QStringLiteral("Disks look healthy."));
    QCOMPARE(final_result.id, QStringLiteral("thread_1"));
    QVERIFY(!backend.isBusy());
}

void AiAgentBackendTests::agentBackend_approvalPolicy() {
    QTemporaryDir dir;
    FakeAgentBackend backend(runtimeIn(dir));
    QSignalSpy asked(&backend, &AiChatBackend::approvalRequested);
    backend.setApprovalPolicy(AiApprovalPolicy::AllowAll);
    backend.ask(QStringLiteral("a1"));
    backend.setApprovalPolicy(AiApprovalPolicy::DenyAll);
    backend.ask(QStringLiteral("a2"));
    QCOMPARE(asked.count(), 0);
    QCOMPARE(backend.answers.at(0).second, AiApprovalDecision::AllowOnce);
    QCOMPARE(backend.answers.at(1).second, AiApprovalDecision::Deny);

    backend.setApprovalPolicy(AiApprovalPolicy::Ask);
    backend.ask(QStringLiteral("a3"));
    QCOMPARE(asked.count(), 1);
    backend.resolveApproval(QStringLiteral("a3"), AiApprovalDecision::AllowForSession);
    backend.resolveApproval(QStringLiteral("a3"), AiApprovalDecision::Deny);  // Already answered.
    QCOMPARE(backend.answers.size(), 3);
    QCOMPARE(backend.answers.last().second, AiApprovalDecision::AllowForSession);
}

QTEST_GUILESS_MAIN(AiAgentBackendTests)
#include "test_ai_agent_backends.moc"

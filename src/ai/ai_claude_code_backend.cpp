// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_claude_code_backend.h"

#include "sak/ai/ai_api_support.h"
#include "sak/ai/ai_stdio_json_process.h"
#include "sak/ai/ai_tool_bridge_server.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <utility>

namespace sak::ai {

namespace {

constexpr auto kSakToolPrefix = "mcp__sak__";

QJsonObject textBlock(const QString& text) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                       {QStringLiteral("text"), text}};
}

QJsonObject attachmentBlock(const OpenAIInputAttachment& attachment) {
    if (attachment.type == OpenAIInputAttachment::Type::Text) {
        return textBlock(attachmentText(attachment));
    }
    const auto parts = attachmentBinary(attachment);
    if (!parts || !parts->mime_type.startsWith(QLatin1String("image/"))) {
        return textBlock(unsupportedAttachmentNote(attachment));
    }
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("image")},
                       {QStringLiteral("source"),
                        QJsonObject{{QStringLiteral("type"), QStringLiteral("base64")},
                                    {QStringLiteral("media_type"), parts->mime_type},
                                    {QStringLiteral("data"), parts->base64_data}}}};
}

QString permissionDetail(const QJsonObject& request) {
    const QString description = request.value(QStringLiteral("description")).toString();
    const QJsonObject input = request.value(QStringLiteral("input")).toObject();
    const QString command = input.value(QStringLiteral("command")).toString();
    const QString file = input.value(QStringLiteral("file_path")).toString();
    QStringList lines;
    if (!description.isEmpty()) {
        lines << description;
    }
    if (!command.isEmpty()) {
        lines << QStringLiteral("Command: %1").arg(command);
    }
    if (!file.isEmpty()) {
        lines << QStringLiteral("File: %1").arg(file);
    }
    if (lines.isEmpty()) {
        lines << QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Compact))
                     .left(kAgentStderrTailBytes);
    }
    return lines.join(QChar(u'\n'));
}

}  // namespace

ClaudeCodeBackend::ClaudeCodeBackend(AiAgentRuntime runtime, QObject* parent)
    : AiAgentBackend(ModelProviderId::Anthropic, std::move(runtime), parent)
    , m_process(new AiStdioJsonProcess(this)) {
    connect(m_process, &AiStdioJsonProcess::jsonReceived, this, &ClaudeCodeBackend::handleMessage);
    connect(m_process, &AiStdioJsonProcess::finished, this, &ClaudeCodeBackend::handleExit);
    connect(m_process, &AiStdioJsonProcess::startFailed, this, [this](const QString& error) {
        failTurn(tr("Claude Code could not start: %1").arg(error));
    });
    connect(m_process, &AiStdioJsonProcess::started, this, [this]() {
        sendControlRequest(QJsonObject{{QStringLiteral("subtype"), QStringLiteral("initialize")},
                                       {QStringLiteral("hooks"), QJsonValue::Null}});
    });
}

ClaudeCodeBackend::~ClaudeCodeBackend() {
    m_process->disconnect(this);
    m_process->stop();
}

QStringList ClaudeCodeBackend::launchArguments(const OpenAIResponseRequest& request,
                                               const QString& resume_session_id,
                                               const QJsonObject& tool_server) {
    QStringList args{QStringLiteral("--output-format"),
                     QStringLiteral("stream-json"),
                     QStringLiteral("--verbose"),
                     QStringLiteral("--input-format"),
                     QStringLiteral("stream-json"),
                     QStringLiteral("--permission-prompt-tool"),
                     QStringLiteral("stdio")};
    if (!request.model.trimmed().isEmpty()) {
        args << QStringLiteral("--model") << request.model.trimmed();
    }
    const QString effort = request.reasoning_effort.trimmed().toLower();
    if (effort == QLatin1String("low") || effort == QLatin1String("medium") ||
        effort == QLatin1String("high")) {
        args << QStringLiteral("--effort") << effort;
    }
    if (!resume_session_id.isEmpty()) {
        args << QStringLiteral("--resume=%1").arg(resume_session_id);
    }
    if (!tool_server.isEmpty()) {
        QJsonObject server = tool_server;
        server[QStringLiteral("type")] = QStringLiteral("stdio");
        const QJsonObject config{
            {QStringLiteral("mcpServers"),
             QJsonObject{{QString::fromLatin1(kToolBridgeServerName), server}}}};
        // S.A.K. gates its own tools (access mode, human gates), so Claude
        // Code does not need to prompt for them a second time.
        args << QStringLiteral("--mcp-config")
             << QString::fromUtf8(QJsonDocument(config).toJson(QJsonDocument::Compact))
             << QStringLiteral("--allowedTools")
             << QStringLiteral("%1*").arg(QString::fromLatin1(kSakToolPrefix));
    }
    return args;
}

QJsonObject ClaudeCodeBackend::userMessage(const OpenAIResponseRequest& request) {
    QJsonArray content;
    for (const auto& attachment : request.attachments) {
        content.append(attachmentBlock(attachment));
    }
    if (!request.input.trimmed().isEmpty() || content.isEmpty()) {
        content.append(textBlock(request.input));
    }
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("user")},
                       {QStringLiteral("message"),
                        QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                    {QStringLiteral("content"), content}}},
                       {QStringLiteral("parent_tool_use_id"), QJsonValue::Null},
                       {QStringLiteral("session_id"), QString()}};
}

AiAccountStatus ClaudeCodeBackend::parseAuthStatus(const QByteArray& json) {
    AiAccountStatus status;
    const QJsonObject root = QJsonDocument::fromJson(json.trimmed()).object();
    status.signed_in = root.value(QStringLiteral("loggedIn")).toBool(false);
    status.account_label = root.value(QStringLiteral("email")).toString();
    if (status.account_label.isEmpty()) {
        status.account_label = root.value(QStringLiteral("authMethod")).toString();
    }
    status.plan = root.value(QStringLiteral("subscriptionType")).toString();
    return status;
}

TokenUsage ClaudeCodeBackend::usageFromResult(const QJsonObject& usage) {
    const auto count = [&usage](const char* key) {
        return static_cast<qint64>(usage.value(QLatin1String(key)).toDouble(0.0));
    };
    TokenUsage tokens;
    tokens.cached_input_tokens = count("cache_read_input_tokens");
    tokens.input_tokens = count("input_tokens") + tokens.cached_input_tokens +
                          count("cache_creation_input_tokens");
    tokens.output_tokens = count("output_tokens");
    tokens.total_tokens = tokens.input_tokens + tokens.output_tokens;
    return tokens;
}

QString ClaudeCodeBackend::processSignature(const TurnRequest& turn) {
    return QStringList{turn.request.model.trimmed(),
                       turn.request.reasoning_effort.trimmed().toLower(),
                       turn.attach_tools ? QStringLiteral("tools") : QString()}
        .join(QChar(u'|'));
}

void ClaudeCodeBackend::beginTurn(const TurnRequest& turn) {
    m_pending_message = userMessage(turn.request);
    const bool same_session = !m_session_id.isEmpty() && turn.session_hint == m_session_id;
    if (m_process->isRunning() && same_session && m_signature == processSignature(turn)) {
        m_process->send(std::exchange(m_pending_message, {}));
        return;
    }
    startProcess(turn);
}

void ClaudeCodeBackend::stopProcess() {
    m_stopping = true;  // Our own restart is not a crash.
    m_process->stop();
    m_stopping = false;
    m_initialized = false;
}

void ClaudeCodeBackend::startProcess(const TurnRequest& turn) {
    stopProcess();
    m_initialized = false;
    AiAgentLaunch launch = serverLaunch();
    // Resume the stored conversation when the panel continues one (also after
    // an app restart or a model/effort change).
    launch.spec.arguments += launchArguments(turn.request,
                                             turn.session_hint,
                                             turn.attach_tools ? toolServerEntry() : QJsonObject{});
    m_signature = processSignature(turn);
    m_session_id = turn.session_hint;
    m_process->start(launch.spec);
}

void ClaudeCodeBackend::sendControlRequest(const QJsonObject& request) {
    m_process->send(QJsonObject{{QStringLiteral("type"), QStringLiteral("control_request")},
                                {QStringLiteral("request_id"),
                                 QStringLiteral("sak_req_%1").arg(m_next_request++)},
                                {QStringLiteral("request"), request}});
}

void ClaudeCodeBackend::sendControlResponse(const QString& request_id,
                                            const QJsonObject& response) {
    m_process->send(QJsonObject{{QStringLiteral("type"), QStringLiteral("control_response")},
                                {QStringLiteral("response"),
                                 QJsonObject{{QStringLiteral("subtype"), QStringLiteral("success")},
                                             {QStringLiteral("request_id"), request_id},
                                             {QStringLiteral("response"), response}}}});
}

void ClaudeCodeBackend::handleMessage(const QJsonObject& message) {
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("control_response") && !m_initialized) {
        m_initialized = true;
        if (!m_pending_message.isEmpty()) {
            m_process->send(std::exchange(m_pending_message, {}));
        }
    } else if (type == QLatin1String("control_request")) {
        handleControlRequest(message);
    } else if (type == QLatin1String("control_cancel_request")) {
        m_permission_inputs.remove(message.value(QStringLiteral("request_id")).toString());
    } else if (type == QLatin1String("system")) {
        const QString session = message.value(QStringLiteral("session_id")).toString();
        m_session_id = session.isEmpty() ? m_session_id : session;
    } else if (type == QLatin1String("assistant")) {
        handleAssistant(message);
    } else if (type == QLatin1String("result")) {
        handleResult(message);
    }
}

void ClaudeCodeBackend::handleControlRequest(const QJsonObject& message) {
    const QString request_id = message.value(QStringLiteral("request_id")).toString();
    const QJsonObject request = message.value(QStringLiteral("request")).toObject();
    if (request.value(QStringLiteral("subtype")).toString() != QLatin1String("can_use_tool")) {
        m_process->send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("control_response")},
            {QStringLiteral("response"),
             QJsonObject{{QStringLiteral("subtype"), QStringLiteral("error")},
                         {QStringLiteral("request_id"), request_id},
                         {QStringLiteral("error"), QStringLiteral("Not supported by S.A.K.")}}}});
        return;
    }
    const QString tool = request.value(QStringLiteral("tool_name")).toString();
    m_permission_inputs.insert(request_id, request.value(QStringLiteral("input")).toObject());
    if (tool.startsWith(QLatin1String(kSakToolPrefix))) {
        answerApproval(request_id, AiApprovalDecision::AllowOnce);
        return;
    }
    raiseApproval(AiAgentApproval{
        request_id, tr("Claude Code wants to use %1").arg(tool), permissionDetail(request), false});
}

void ClaudeCodeBackend::answerApproval(const QString& approval_id, AiApprovalDecision decision) {
    if (!m_permission_inputs.contains(approval_id)) {
        return;
    }
    const QJsonObject input = m_permission_inputs.take(approval_id);
    if (decision == AiApprovalDecision::Deny) {
        sendControlResponse(approval_id,
                            QJsonObject{{QStringLiteral("behavior"), QStringLiteral("deny")},
                                        {QStringLiteral("message"),
                                         QStringLiteral("The technician denied this in S.A.K.")}});
        return;
    }
    sendControlResponse(approval_id,
                        QJsonObject{{QStringLiteral("behavior"), QStringLiteral("allow")},
                                    {QStringLiteral("updatedInput"), input}});
}

void ClaudeCodeBackend::handleAssistant(const QJsonObject& message) {
    if (!message.value(QStringLiteral("parent_tool_use_id")).isNull()) {
        return;  // Subagent chatter; the parent summarises it.
    }
    const QJsonObject body = message.value(QStringLiteral("message")).toObject();
    QStringList texts;
    for (const auto& value : body.value(QStringLiteral("content")).toArray()) {
        const QJsonObject block = value.toObject();
        if (block.value(QStringLiteral("type")).toString() == QLatin1String("text")) {
            texts << block.value(QStringLiteral("text")).toString();
        }
    }
    const QString error = message.value(QStringLiteral("error")).toString();
    if (!error.isEmpty()) {
        m_auth_error = error;
        return;  // Reported through the result message.
    }
    appendAssistantText(texts.join(QChar(u'\n')));
}

void ClaudeCodeBackend::handleResult(const QJsonObject& message) {
    const QString session = message.value(QStringLiteral("session_id")).toString();
    m_session_id = session.isEmpty() ? m_session_id : session;
    addTurnUsage(usageFromResult(message.value(QStringLiteral("usage")).toObject()));
    if (!message.value(QStringLiteral("is_error")).toBool(false)) {
        completeTurn(m_session_id);
        return;
    }
    const QString text = message.value(QStringLiteral("result")).toString();
    if (m_auth_error == QLatin1String("authentication_failed")) {
        AiAccountStatus status = baseAccountStatus();
        status.detail = tr("Not signed in to Claude");
        publishAccount(status);
    }
    m_auth_error.clear();
    failTurn(text.isEmpty() ? tr("Claude Code reported an error") : text);
}

void ClaudeCodeBackend::handleExit(int exit_code, const QString& stderr_tail) {
    if (m_stopping) {
        return;
    }
    m_initialized = false;
    m_permission_inputs.clear();
    failTurn(tr("Claude Code stopped (exit %1). %2").arg(exit_code).arg(stderr_tail));
}

void ClaudeCodeBackend::interruptTurn() {
    sendControlRequest(QJsonObject{{QStringLiteral("subtype"), QStringLiteral("interrupt")}});
    m_permission_inputs.clear();
}

void ClaudeCodeBackend::resetConversation() {
    AiAgentBackend::resetConversation();
    stopProcess();
    m_session_id.clear();
}

void ClaudeCodeBackend::refreshAccount() {
    AiAccountStatus base = baseAccountStatus();
    if (!base.runtime_available) {
        publishAccount(base);
        return;
    }
    runRuntimeCommand({QStringLiteral("auth"), QStringLiteral("status")},
                      [this](int, const QByteArray& output) {
                          AiAccountStatus status = parseAuthStatus(output);
                          const AiAccountStatus runtime = baseAccountStatus();
                          status.runtime_available = runtime.runtime_available;
                          status.runtime_path = runtime.runtime_path;
                          publishAccount(status);
                      });
}

void ClaudeCodeBackend::startSignIn() {
    AiAccountStatus status = baseAccountStatus();
    status.sign_in_pending = status.runtime_available;
    status.detail = tr("Finish signing in in the Claude Code window");
    publishAccount(status);
    runVisibleSignIn({QStringLiteral("auth"), QStringLiteral("login")},
                     [this](int, const QByteArray&) { refreshAccount(); });
}

void ClaudeCodeBackend::cancelSignIn() {
    cancelVisibleSignIn();
    refreshAccount();
}

void ClaudeCodeBackend::signOut() {
    resetConversation();
    runRuntimeCommand({QStringLiteral("auth"), QStringLiteral("logout")},
                      [this](int, const QByteArray&) { refreshAccount(); });
}

}  // namespace sak::ai

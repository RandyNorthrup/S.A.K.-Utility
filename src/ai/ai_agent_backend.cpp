// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_agent_backend.h"

#include "sak/ai/ai_tool_bridge_server.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QProcess>
#include <QSaveFile>
#include <QTimer>

#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace sak::ai {

namespace {

/// Style guidance in the S.A.K. tools guide; agent runtimes keep their own.
const QStringList& toneLines() {
    static const QStringList kLines{
        QStringLiteral("You are the S.A.K. Utility AI Assistant for Windows PC technicians."),
        QStringLiteral("Be practical, concise, and verify fixes when tools are available.")};
    return kLines;
}

QString knowledgeHeader() {
    return QStringLiteral(
        "# S.A.K. Utility\n\n"
        "This folder is an S.A.K. Utility AI session workspace on a Windows PC that a technician "
        "is servicing. S.A.K. Utility provides its own tools through the MCP server named `sak`; "
        "the tool names below refer to that server's tools. S.A.K. applies its access mode and "
        "approval gates to those tools itself.\n\n"
        "## How to use S.A.K. Utility\n\n");
}

void showInOwnConsole(QProcess* process) {
#ifdef Q_OS_WIN
    // A console the technician can see and type into, with the child's own
    // stdio instead of S.A.K.'s pipes.
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NEW_CONSOLE;
        args->startupInfo->dwFlags &= ~STARTF_USESTDHANDLES;
    });
#endif
    process->setProcessChannelMode(QProcess::ForwardedChannels);
    process->setInputChannelMode(QProcess::ForwardedInputChannel);
}

}  // namespace

AiAgentBackend::AiAgentBackend(ModelProviderId provider, AiAgentRuntime runtime, QObject* parent)
    : AiChatBackend(parent), m_provider(provider), m_runtime(std::move(runtime)) {}

AiAgentBackend::~AiAgentBackend() = default;

QString AiAgentBackend::knowledgeText(const QString& instructions) {
    QStringList kept;
    for (const auto& line : instructions.split(QChar(u'\n'))) {
        if (!toneLines().contains(line.trimmed())) {
            kept.append(line);
        }
    }
    return knowledgeHeader() + kept.join(QChar(u'\n')).trimmed() + QChar(u'\n');
}

QString AiAgentBackend::knowledgeFileName(ModelProviderId provider) {
    switch (provider) {
    case ModelProviderId::Anthropic:
        return QStringLiteral("CLAUDE.md");
    case ModelProviderId::Google:
        return QStringLiteral("GEMINI.md");
    case ModelProviderId::OpenAI:
    case ModelProviderId::Muse:
        break;
    }
    return QStringLiteral("AGENTS.md");
}

AiAgentLaunch AiAgentBackend::serverLaunch() const {
    AiAgentLaunch launch = m_runtime.serverLaunch(m_provider);
    launch.spec.working_directory = workspaceDirectory();
    return launch;
}

QString AiAgentBackend::workspaceDirectory() const {
    const QString directory = m_workspace.isEmpty() ? m_runtime.defaultWorkspace() : m_workspace;
    QDir().mkpath(directory);
    return QDir::cleanPath(directory);
}

void AiAgentBackend::setWorkspaceDirectory(const QString& directory) {
    m_workspace = directory.trimmed();
}

void AiAgentBackend::setApprovalPolicy(AiApprovalPolicy policy) {
    m_policy = policy;
}

QJsonObject AiAgentBackend::toolServerEntry() {
    if (!m_bridge) {
        m_bridge = new AiToolBridgeServer(this);
        connect(m_bridge,
                &AiToolBridgeServer::toolCallRequested,
                this,
                &AiAgentBackend::handleToolCall);
    }
    QString error;
    if (!m_bridge->start(&error)) {
        Q_EMIT activityText(tr("S.A.K. tools unavailable: %1").arg(error));
        return {};
    }
    return QJsonObject{
        {QStringLiteral("command"), QDir::toNativeSeparators(m_runtime.toolBridgeExecutable())},
        {QStringLiteral("args"), QJsonArray{QStringLiteral("--pipe"), m_bridge->serverName()}},
        {QStringLiteral("env"),
         QJsonObject{{QString::fromLatin1(kToolBridgeTokenVariable), m_bridge->token()}}}};
}

void AiAgentBackend::writeKnowledgeFile(const QString& instructions) {
    if (instructions.trimmed().isEmpty()) {
        return;
    }
    const QString path = QDir(workspaceDirectory()).filePath(knowledgeFileName(m_provider));
    const QByteArray content = knowledgeText(instructions).toUtf8();
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly) && existing.readAll() == content) {
        return;
    }
    existing.close();
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(content);
        file.commit();
    }
}

void AiAgentBackend::createResponse(const OpenAIResponseRequest& request) {
    if (!request.function_outputs.isEmpty()) {
        deliverToolOutputs(request.function_outputs);
        return;
    }
    if (m_turn_active) {
        Q_EMIT requestFailed(tr("%1 is still working on the previous message; press Stop first")
                                 .arg(modelProviderInfo(m_provider).agent_name));
        return;
    }
    if (request.input.trimmed().isEmpty() && request.attachments.isEmpty()) {
        Q_EMIT requestFailed(tr("Message is empty"));
        return;
    }
    const AiAgentLaunch launch = serverLaunch();
    if (!launch.available) {
        Q_EMIT requestFailed(launch.missing_reason);
        return;
    }
    TurnRequest turn;
    turn.request = request;
    turn.session_hint = request.previous_response_id.trimmed();
    turn.attach_tools = request.enable_local_tools;
    writeKnowledgeFile(request.instructions);
    resetTurnState();
    m_turn_active = true;
    Q_EMIT requestStarted();
    beginTurn(turn);
}

void AiAgentBackend::countInputTokens(const OpenAIResponseRequest& request,
                                      const QString& request_id) {
    Q_UNUSED(request);
    Q_EMIT inputTokenCountFailed(request_id,
                                 tr("%1 does not report token counts before a message is sent")
                                     .arg(modelProviderInfo(m_provider).agent_name));
}

void AiAgentBackend::listModels(const QString& credential) {
    Q_UNUSED(credential);
    Q_EMIT modelsReady(modelProviderInfo(m_provider).subscription_models);
}

bool AiAgentBackend::isBusy() const {
    // While the panel is running a S.A.K. tool for the agent, the panel's own
    // tool turn owns the busy state.
    return m_turn_active && m_outstanding_call.isEmpty();
}

void AiAgentBackend::cancel() {
    const QSet<QString> approvals = std::exchange(m_pending_approvals, {});
    for (const auto& id : approvals) {
        answerApproval(id, AiApprovalDecision::Deny);
    }
    if (m_bridge) {
        m_bridge->failPendingCalls(tr("Stopped by the technician"));
    }
    if (!m_turn_active) {
        return;
    }
    interruptTurn();
    resetTurnState();
    Q_EMIT requestFinished();
}

void AiAgentBackend::resetConversation() {
    if (m_turn_active) {
        cancel();
    }
    m_session_id.clear();
}

void AiAgentBackend::resetTurnState() {
    m_turn_active = false;
    m_turn_text.clear();
    m_turn_usage = {};
    m_queued_calls.clear();
    m_outstanding_call.clear();
}

void AiAgentBackend::appendAssistantText(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    if (!m_turn_text.isEmpty()) {
        m_turn_text += QStringLiteral("\n\n");
    }
    m_turn_text += trimmed;
}

void AiAgentBackend::appendAssistantChunk(const QString& chunk) {
    m_turn_text += chunk;
}

void AiAgentBackend::addTurnUsage(const TokenUsage& usage) {
    m_turn_usage.input_tokens += usage.input_tokens;
    m_turn_usage.cached_input_tokens += usage.cached_input_tokens;
    m_turn_usage.output_tokens += usage.output_tokens;
    m_turn_usage.reasoning_tokens += usage.reasoning_tokens;
    m_turn_usage.total_tokens += usage.total_tokens;
}

void AiAgentBackend::completeTurn(const QString& session_id) {
    if (!m_turn_active) {
        return;  // Late completion after a user stop.
    }
    if (m_bridge) {
        m_bridge->failPendingCalls(tr("Agent turn ended"));
    }
    m_session_id = session_id;
    OpenAIResponseResult result;
    result.id = session_id;
    result.output_text = m_turn_text.trimmed();
    result.usage = m_turn_usage;
    if (result.usage.total_tokens == 0) {
        result.usage.total_tokens = result.usage.input_tokens + result.usage.output_tokens;
    }
    resetTurnState();
    Q_EMIT responseReady(result);
    Q_EMIT requestFinished();
}

void AiAgentBackend::failTurn(const QString& error_message) {
    if (!m_turn_active) {
        return;
    }
    if (m_bridge) {
        m_bridge->failPendingCalls(error_message);
    }
    resetTurnState();
    Q_EMIT requestFailed(error_message);
    Q_EMIT requestFinished();
}

void AiAgentBackend::handleToolCall(const QString& call_id,
                                    const QString& name,
                                    const QString& arguments) {
    if (!m_turn_active) {
        m_bridge->completeToolCall(call_id, tr("No active S.A.K. assistant turn"), true);
        return;
    }
    m_queued_calls.append(OpenAIFunctionCall{call_id, name, arguments});
    if (m_outstanding_call.isEmpty()) {
        emitNextToolCall();
    }
}

void AiAgentBackend::emitNextToolCall() {
    if (m_queued_calls.isEmpty() || !m_turn_active) {
        return;
    }
    const OpenAIFunctionCall call = m_queued_calls.takeFirst();
    m_outstanding_call = call.call_id;
    OpenAIResponseResult result;
    result.id = m_session_id;
    result.function_calls.append(call);
    Q_EMIT responseReady(result);
    Q_EMIT requestFinished();
}

void AiAgentBackend::deliverToolOutputs(const QVector<OpenAIFunctionOutput>& outputs) {
    for (const auto& output : outputs) {
        if (m_bridge) {
            m_bridge->completeToolCall(output.call_id, output.output, false);
        }
        if (output.call_id == m_outstanding_call) {
            m_outstanding_call.clear();
        }
    }
    if (!m_turn_active) {
        Q_EMIT requestFailed(tr("The agent turn already ended"));
        return;
    }
    m_outstanding_call.clear();
    Q_EMIT requestStarted();
    // Let the panel finish handling requestStarted before the next call lands.
    QTimer::singleShot(0, this, &AiAgentBackend::emitNextToolCall);
}

void AiAgentBackend::raiseApproval(const AiAgentApproval& approval) {
    switch (m_policy) {
    case AiApprovalPolicy::AllowAll:
        answerApproval(approval.id, AiApprovalDecision::AllowOnce);
        return;
    case AiApprovalPolicy::DenyAll:
        answerApproval(approval.id, AiApprovalDecision::Deny);
        return;
    case AiApprovalPolicy::Ask:
        break;
    }
    m_pending_approvals.insert(approval.id);
    Q_EMIT approvalRequested(approval);
}

void AiAgentBackend::resolveApproval(const QString& approval_id, AiApprovalDecision decision) {
    if (m_pending_approvals.remove(approval_id)) {
        answerApproval(approval_id, decision);
    }
}

AiAccountStatus AiAgentBackend::baseAccountStatus() const {
    AiAccountStatus status;
    status.provider = m_provider;
    status.auth_mode = ModelAuthMode::Subscription;
    const AiAgentLaunch launch = m_runtime.serverLaunch(m_provider);
    status.runtime_available = launch.available;
    status.runtime_path = launch.spec.program;
    if (!launch.available) {
        status.detail = launch.missing_reason;
    }
    return status;
}

void AiAgentBackend::publishAccount(AiAccountStatus status) {
    status.provider = m_provider;
    status.auth_mode = ModelAuthMode::Subscription;
    Q_EMIT accountStatusChanged(status);
}

void AiAgentBackend::runRuntimeCommand(const QStringList& arguments, CommandFinished finished) {
    const AiAgentLaunch launch = m_runtime.commandLaunch(m_provider, arguments);
    if (!launch.available) {
        finished(-1, launch.missing_reason.toUtf8());
        return;
    }
    auto* process = new QProcess(this);
    process->setProgram(launch.spec.program);
    process->setArguments(launch.spec.arguments);
    process->setProcessEnvironment(launch.spec.environment);
    process->setWorkingDirectory(workspaceDirectory());
    process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process,
            &QProcess::finished,
            this,
            [process, finished](int exit_code, QProcess::ExitStatus) {
                finished(exit_code, process->readAllStandardOutput());
                process->deleteLater();
            });
    connect(process, &QProcess::errorOccurred, this, [process, finished](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            finished(-1, process->errorString().toUtf8());
            process->deleteLater();
        }
    });
    process->start();
    process->closeWriteChannel();
}

void AiAgentBackend::runVisibleSignIn(const QStringList& arguments, CommandFinished finished) {
    const AiAgentLaunch launch = m_runtime.commandLaunch(m_provider, arguments);
    if (!launch.available) {
        finished(-1, launch.missing_reason.toUtf8());
        return;
    }
    cancelVisibleSignIn();
    auto* process = new QProcess(this);
    m_sign_in_process = process;
    process->setProgram(launch.spec.program);
    process->setArguments(launch.spec.arguments);
    process->setProcessEnvironment(launch.spec.environment);
    process->setWorkingDirectory(workspaceDirectory());
    showInOwnConsole(process);
    connect(process,
            &QProcess::finished,
            this,
            [process, finished](int exit_code, QProcess::ExitStatus) {
                finished(exit_code, {});
                process->deleteLater();
            });
    connect(process, &QProcess::errorOccurred, this, [process, finished](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            finished(-1, process->errorString().toUtf8());
            process->deleteLater();
        }
    });
    process->start();
}

void AiAgentBackend::cancelVisibleSignIn() {
    if (m_sign_in_process) {
        m_sign_in_process->kill();
    }
}

}  // namespace sak::ai

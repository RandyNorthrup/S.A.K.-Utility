// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_gemini_cli_backend.h"

#include "sak/ai/ai_api_support.h"
#include "sak/ai/ai_stdio_json_process.h"
#include "sak/ai/ai_tool_bridge_server.h"
#include "sak/version.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <utility>

namespace sak::ai {

namespace {

constexpr int kAcpProtocolVersion = 1;
constexpr int kMethodNotFound = -32'601;

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
                       {QStringLiteral("data"), parts->base64_data},
                       {QStringLiteral("mimeType"), parts->mime_type}};
}

QString wantedKind(AiApprovalDecision decision) {
    switch (decision) {
    case AiApprovalDecision::AllowOnce:
        return QStringLiteral("allow_once");
    case AiApprovalDecision::AllowForSession:
        return QStringLiteral("allow_always");
    case AiApprovalDecision::Deny:
        break;
    }
    return QStringLiteral("reject_once");
}

bool isSakToolPermission(const QJsonObject& params, const QJsonArray& options) {
    const QString title = params.value(QStringLiteral("toolCall"))
                              .toObject()
                              .value(QStringLiteral("title"))
                              .toString();
    bool server_option = false;
    for (const auto& value : options) {
        server_option |= value.toObject().value(QStringLiteral("optionId")).toString() ==
                         QLatin1String("proceed_always_server");
    }
    return server_option && title.contains(QLatin1String(kToolBridgeServerName));
}

}  // namespace

GeminiCliBackend::GeminiCliBackend(AiAgentRuntime runtime, QObject* parent)
    : AiAgentBackend(ModelProviderId::Google, std::move(runtime), parent)
    , m_process(new AiStdioJsonProcess(this))
    , m_rpc(new AiJsonRpcPeer(m_process, true)) {
    connect(m_process, &AiStdioJsonProcess::finished, this, &GeminiCliBackend::handleExit);
    connect(m_process, &AiStdioJsonProcess::startFailed, this, [this](const QString& error) {
        m_starting = false;
        flushReadyCallbacks(error);
    });
    connect(m_process, &AiStdioJsonProcess::started, this, [this]() {
        const QJsonObject params{
            {QStringLiteral("protocolVersion"), kAcpProtocolVersion},
            {QStringLiteral("clientCapabilities"),
             QJsonObject{{QStringLiteral("fs"),
                          QJsonObject{{QStringLiteral("readTextFile"), false},
                                      {QStringLiteral("writeTextFile"), false}}},
                         {QStringLiteral("terminal"), false}}},
            {QStringLiteral("clientInfo"),
             QJsonObject{{QStringLiteral("name"), QStringLiteral("sak_utility")},
                         {QStringLiteral("title"), QStringLiteral("S.A.K. Utility")},
                         {QStringLiteral("version"), QString::fromLatin1(get_version())}}}};
        m_rpc->request(QStringLiteral("initialize"),
                       params,
                       [this](const QJsonObject&, const QString& error) {
                           m_starting = false;
                           m_initialized = error.isEmpty();
                           flushReadyCallbacks(error);
                       });
    });
    connect(m_rpc,
            &AiJsonRpcPeer::notificationReceived,
            this,
            [this](const QString& method, const QJsonObject& params) {
                if (method == QLatin1String("session/update")) {
                    handleUpdate(params);
                }
            });
    connect(m_rpc, &AiJsonRpcPeer::requestReceived, this, &GeminiCliBackend::handleServerRequest);
}

GeminiCliBackend::~GeminiCliBackend() {
    m_process->disconnect(this);
    m_process->stop();
}

QJsonArray GeminiCliBackend::promptBlocks(const OpenAIResponseRequest& request) {
    QJsonArray blocks;
    for (const auto& attachment : request.attachments) {
        blocks.append(attachmentBlock(attachment));
    }
    if (!request.input.trimmed().isEmpty() || blocks.isEmpty()) {
        blocks.append(textBlock(request.input));
    }
    return blocks;
}

QJsonArray GeminiCliBackend::mcpServers(const QJsonObject& tool_server) {
    if (tool_server.isEmpty()) {
        return {};
    }
    QJsonArray env;
    const QJsonObject variables = tool_server.value(QStringLiteral("env")).toObject();
    for (auto it = variables.begin(); it != variables.end(); ++it) {
        env.append(
            QJsonObject{{QStringLiteral("name"), it.key()}, {QStringLiteral("value"), it.value()}});
    }
    return QJsonArray{
        QJsonObject{{QStringLiteral("name"), QString::fromLatin1(kToolBridgeServerName)},
                    {QStringLiteral("command"), tool_server.value(QStringLiteral("command"))},
                    {QStringLiteral("args"), tool_server.value(QStringLiteral("args"))},
                    {QStringLiteral("env"), env}}};
}

QString GeminiCliBackend::optionForDecision(const QJsonArray& options,
                                            AiApprovalDecision decision) {
    const QString kind = wantedKind(decision);
    QString fallback;
    for (const auto& value : options) {
        const QJsonObject option = value.toObject();
        const QString option_kind = option.value(QStringLiteral("kind")).toString();
        if (option_kind == kind) {
            return option.value(QStringLiteral("optionId")).toString();
        }
        if (decision != AiApprovalDecision::Deny && option_kind == QLatin1String("allow_once")) {
            fallback = option.value(QStringLiteral("optionId")).toString();
        }
        if (decision == AiApprovalDecision::Deny &&
            option_kind.startsWith(QLatin1String("reject"))) {
            fallback = option.value(QStringLiteral("optionId")).toString();
        }
    }
    return fallback;
}

TokenUsage GeminiCliBackend::usageFromMeta(const QJsonObject& meta) {
    const QJsonObject counts = meta.value(QStringLiteral("quota"))
                                   .toObject()
                                   .value(QStringLiteral("token_count"))
                                   .toObject();
    TokenUsage usage;
    usage.input_tokens =
        static_cast<qint64>(counts.value(QStringLiteral("input_tokens")).toDouble(0.0));
    usage.output_tokens =
        static_cast<qint64>(counts.value(QStringLiteral("output_tokens")).toDouble(0.0));
    usage.total_tokens = usage.input_tokens + usage.output_tokens;
    return usage;
}

QString GeminiCliBackend::credentialFile() const {
    return QDir(runtime().homeDirectory(ModelProviderId::Google))
        .filePath(QStringLiteral(".gemini/oauth_creds.json"));
}

void GeminiCliBackend::ensureStarted(Ready ready) {
    if (m_initialized) {
        ready({});
        return;
    }
    m_ready_callbacks.append(std::move(ready));
    if (m_starting) {
        return;
    }
    const AiAgentLaunch launch = serverLaunch();
    if (!launch.available) {
        flushReadyCallbacks(launch.missing_reason);
        return;
    }
    m_starting = true;
    m_process->start(launch.spec);
}

void GeminiCliBackend::flushReadyCallbacks(const QString& error) {
    const auto callbacks = std::exchange(m_ready_callbacks, {});
    for (const auto& callback : callbacks) {
        callback(error);
    }
}

void GeminiCliBackend::handleExit(int exit_code, const QString& stderr_tail) {
    m_initialized = false;
    m_starting = false;
    m_session_id.clear();
    m_permissions.clear();
    const QString reason = tr("Gemini CLI stopped (exit %1). %2").arg(exit_code).arg(stderr_tail);
    m_rpc->failAll(reason);
    flushReadyCallbacks(reason);
    failTurn(reason);
}

void GeminiCliBackend::beginTurn(const TurnRequest& turn) {
    ensureStarted([this, turn](const QString& error) {
        if (!error.isEmpty()) {
            failTurn(error);
            return;
        }
        if (!m_session_id.isEmpty() && turn.session_hint == m_session_id) {
            sendPrompt(turn);
            return;
        }
        openSession(turn);
    });
}

void GeminiCliBackend::openSession(const TurnRequest& turn) {
    const QJsonObject params{{QStringLiteral("cwd"),
                              QDir::toNativeSeparators(workspaceDirectory())},
                             {QStringLiteral("mcpServers"),
                              mcpServers(turn.attach_tools ? toolServerEntry() : QJsonObject{})}};
    m_rpc->request(QStringLiteral("session/new"),
                   params,
                   [this, turn](const QJsonObject& result, const QString& error) {
                       if (!error.isEmpty()) {
                           failTurn(QFileInfo::exists(credentialFile())
                                        ? error
                                        : tr("Sign in with Google first (%1)").arg(error));
                           return;
                       }
                       m_session_id = result.value(QStringLiteral("sessionId")).toString();
                       m_session_model.clear();
                       sendPrompt(turn);
                   });
}

void GeminiCliBackend::sendPrompt(const TurnRequest& turn) {
    const QString model = turn.request.model.trimmed();
    if (!model.isEmpty() && model != m_session_model) {
        m_session_model = model;
        m_rpc->request(QStringLiteral("session/set_model"),
                       QJsonObject{{QStringLiteral("sessionId"), m_session_id},
                                   {QStringLiteral("modelId"), model}},
                       {});
    }
    const QJsonObject params{{QStringLiteral("sessionId"), m_session_id},
                             {QStringLiteral("prompt"), promptBlocks(turn.request)}};
    m_rpc->request(
        QStringLiteral("session/prompt"),
        params,
        [this](const QJsonObject& result, const QString& error) {
            if (!error.isEmpty()) {
                failTurn(error);
                return;
            }
            addTurnUsage(usageFromMeta(result.value(QStringLiteral("_meta")).toObject()));
            if (result.value(QStringLiteral("stopReason")).toString() == QLatin1String("refusal")) {
                appendAssistantText(tr("[Gemini declined this request]"));
            }
            completeTurn(m_session_id);
        });
}

void GeminiCliBackend::handleUpdate(const QJsonObject& params) {
    const QJsonObject update = params.value(QStringLiteral("update")).toObject();
    const QString kind = update.value(QStringLiteral("sessionUpdate")).toString();
    if (kind == QLatin1String("agent_message_chunk")) {
        appendAssistantChunk(update.value(QStringLiteral("content"))
                                 .toObject()
                                 .value(QStringLiteral("text"))
                                 .toString());
    } else if (kind == QLatin1String("tool_call")) {
        Q_EMIT activityText(update.value(QStringLiteral("title")).toString());
    }
}

void GeminiCliBackend::handleServerRequest(const QJsonValue& id,
                                           const QString& method,
                                           const QJsonObject& params) {
    if (method != QLatin1String("session/request_permission")) {
        m_rpc->respondError(id, kMethodNotFound, QStringLiteral("Not supported by S.A.K."));
        return;
    }
    const QJsonArray options = params.value(QStringLiteral("options")).toArray();
    const QString approval_id = QStringLiteral("gemini_%1").arg(id.toVariant().toString());
    m_permissions.insert(approval_id, PendingPermission{id, options});
    if (isSakToolPermission(params, options)) {
        // S.A.K. gates its own tools; trust the server for this session.
        m_rpc->respond(id,
                       QJsonObject{
                           {QStringLiteral("outcome"),
                            QJsonObject{{QStringLiteral("outcome"), QStringLiteral("selected")},
                                        {QStringLiteral("optionId"),
                                         QStringLiteral("proceed_always_server")}}}});
        m_permissions.remove(approval_id);
        return;
    }
    const QJsonObject tool_call = params.value(QStringLiteral("toolCall")).toObject();
    raiseApproval(AiAgentApproval{
        approval_id,
        tr("Gemini CLI wants to run: %1").arg(tool_call.value(QStringLiteral("title")).toString()),
        tool_call.value(QStringLiteral("kind")).toString(),
        true});
}

void GeminiCliBackend::answerApproval(const QString& approval_id, AiApprovalDecision decision) {
    const PendingPermission pending = m_permissions.take(approval_id);
    if (pending.rpc_id.isUndefined()) {
        return;
    }
    const QString option = optionForDecision(pending.options, decision);
    const QJsonObject outcome =
        option.isEmpty() ? QJsonObject{{QStringLiteral("outcome"), QStringLiteral("cancelled")}}
                         : QJsonObject{{QStringLiteral("outcome"), QStringLiteral("selected")},
                                       {QStringLiteral("optionId"), option}};
    m_rpc->respond(pending.rpc_id, QJsonObject{{QStringLiteral("outcome"), outcome}});
}

void GeminiCliBackend::interruptTurn() {
    // ACP: pending permission prompts must be answered "cancelled".
    const auto pending = std::exchange(m_permissions, {});
    for (const auto& permission : pending) {
        m_rpc->respond(
            permission.rpc_id,
            QJsonObject{{QStringLiteral("outcome"),
                         QJsonObject{{QStringLiteral("outcome"), QStringLiteral("cancelled")}}}});
    }
    if (!m_session_id.isEmpty()) {
        m_rpc->notify(QStringLiteral("session/cancel"),
                      QJsonObject{{QStringLiteral("sessionId"), m_session_id}});
    }
}

void GeminiCliBackend::resetConversation() {
    AiAgentBackend::resetConversation();
    m_session_id.clear();
}

void GeminiCliBackend::refreshAccount() {
    AiAccountStatus status = baseAccountStatus();
    // Presence only; S.A.K. never reads the token file's contents.
    status.signed_in = QFileInfo::exists(credentialFile());
    status.detail = status.runtime_available
                        ? tr("Google sign-in works for Workspace and Gemini Code Assist "
                             "Standard/Enterprise accounts")
                        : status.detail;
    publishAccount(status);
}

void GeminiCliBackend::startSignIn() {
    AiAccountStatus status = baseAccountStatus();
    status.sign_in_pending = status.runtime_available;
    status.detail = tr("Finish signing in in the browser window Gemini CLI opened");
    publishAccount(status);
    ensureStarted([this](const QString& error) {
        if (!error.isEmpty()) {
            refreshAccount();
            return;
        }
        m_rpc->request(QStringLiteral("authenticate"),
                       QJsonObject{{QStringLiteral("methodId"), QStringLiteral("oauth-personal")}},
                       [this](const QJsonObject&, const QString& auth_error) {
                           refreshAccount();
                           if (!auth_error.isEmpty()) {
                               Q_EMIT activityText(tr("Gemini sign-in failed: %1").arg(auth_error));
                           }
                       });
    });
}

void GeminiCliBackend::cancelSignIn() {
    // The ACP authenticate call cannot be cancelled; restarting the runtime ends it.
    m_process->stop();
    refreshAccount();
}

void GeminiCliBackend::signOut() {
    resetConversation();
    m_process->stop();
    const QDir gemini_dir(QFileInfo(credentialFile()).absolutePath());
    QFile::remove(credentialFile());
    QFile::remove(gemini_dir.filePath(QStringLiteral("google_accounts.json")));
    refreshAccount();
}

}  // namespace sak::ai

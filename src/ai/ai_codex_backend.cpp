// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_codex_backend.h"

#include "sak/ai/ai_api_support.h"
#include "sak/ai/ai_stdio_json_process.h"
#include "sak/ai/ai_tool_bridge_server.h"
#include "sak/version.h"

#include <QDir>
#include <QJsonArray>

#include <utility>

namespace sak::ai {

namespace {

constexpr int kMethodNotFound = -32'601;
constexpr int kToolServerStartupSeconds = 20;
constexpr int kToolServerCallSeconds = 3600;

QString approvalPolicyValue(AiApprovalPolicy policy) {
    return policy == AiApprovalPolicy::AllowAll ? QStringLiteral("never")
                                                : QStringLiteral("untrusted");
}

QString sandboxValue(AiApprovalPolicy policy) {
    // Research mode keeps Codex's own shell read-only; S.A.K. tools carry
    // their own access-mode gating.
    return policy == AiApprovalPolicy::DenyAll ? QStringLiteral("read-only")
                                               : QStringLiteral("danger-full-access");
}

QString decisionValue(AiApprovalDecision decision) {
    switch (decision) {
    case AiApprovalDecision::AllowOnce:
        return QStringLiteral("accept");
    case AiApprovalDecision::AllowForSession:
        return QStringLiteral("acceptForSession");
    case AiApprovalDecision::Deny:
        break;
    }
    return QStringLiteral("decline");
}

QJsonObject textInput(const QString& text) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                       {QStringLiteral("text"), text},
                       {QStringLiteral("text_elements"), QJsonArray{}}};
}

QJsonObject attachmentInput(const OpenAIInputAttachment& attachment) {
    if (attachment.type == OpenAIInputAttachment::Type::Text) {
        return textInput(attachmentText(attachment));
    }
    const auto parts = attachmentBinary(attachment);
    if (!parts || !parts->mime_type.startsWith(QLatin1String("image/"))) {
        return textInput(unsupportedAttachmentNote(attachment));
    }
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("image")},
        {QStringLiteral("url"),
         QStringLiteral("data:%1;base64,%2").arg(parts->mime_type, parts->base64_data)}};
}

}  // namespace

CodexAppServerBackend::CodexAppServerBackend(AiAgentRuntime runtime, QObject* parent)
    : AiAgentBackend(ModelProviderId::OpenAI, std::move(runtime), parent)
    , m_process(new AiStdioJsonProcess(this))
    , m_rpc(new AiJsonRpcPeer(m_process, false)) {
    connect(m_process, &AiStdioJsonProcess::started, this, &CodexAppServerBackend::handleStarted);
    connect(m_process, &AiStdioJsonProcess::finished, this, &CodexAppServerBackend::handleExit);
    connect(m_process, &AiStdioJsonProcess::startFailed, this, [this](const QString& error) {
        m_starting = false;
        flushReadyCallbacks(error);
    });
    connect(m_rpc,
            &AiJsonRpcPeer::notificationReceived,
            this,
            &CodexAppServerBackend::handleNotification);
    connect(
        m_rpc, &AiJsonRpcPeer::requestReceived, this, &CodexAppServerBackend::handleServerRequest);
}

CodexAppServerBackend::~CodexAppServerBackend() {
    m_process->disconnect(this);
    m_process->stop();
}

QJsonObject CodexAppServerBackend::threadStartParams(const OpenAIResponseRequest& request,
                                                     const QString& cwd,
                                                     AiApprovalPolicy policy,
                                                     const QJsonObject& tool_server) {
    QJsonObject params{{QStringLiteral("cwd"), QDir::toNativeSeparators(cwd)},
                       {QStringLiteral("approvalPolicy"), approvalPolicyValue(policy)},
                       {QStringLiteral("sandbox"), sandboxValue(policy)}};
    if (!request.model.trimmed().isEmpty()) {
        params[QStringLiteral("model")] = request.model.trimmed();
    }
    if (!tool_server.isEmpty()) {
        QJsonObject server = tool_server;
        server[QStringLiteral("startup_timeout_sec")] = kToolServerStartupSeconds;
        // S.A.K. tools can wait on a human gate or a long install.
        server[QStringLiteral("tool_timeout_sec")] = kToolServerCallSeconds;
        // A dotted key adds one server without replacing the user's table.
        params[QStringLiteral("config")] = QJsonObject{
            {QStringLiteral("mcp_servers.%1").arg(QString::fromLatin1(kToolBridgeServerName)),
             server}};
    }
    return params;
}

QJsonArray CodexAppServerBackend::turnInput(const OpenAIResponseRequest& request) {
    QJsonArray input;
    if (!request.input.trimmed().isEmpty()) {
        input.append(textInput(request.input));
    }
    for (const auto& attachment : request.attachments) {
        input.append(attachmentInput(attachment));
    }
    return input;
}

TokenUsage CodexAppServerBackend::usageFromBreakdown(const QJsonObject& breakdown) {
    const auto count = [&breakdown](const char* key) {
        return static_cast<qint64>(breakdown.value(QLatin1String(key)).toDouble(0.0));
    };
    TokenUsage usage;
    usage.input_tokens = count("inputTokens");
    usage.cached_input_tokens = count("cachedInputTokens");
    usage.output_tokens = count("outputTokens");
    usage.reasoning_tokens = count("reasoningOutputTokens");
    usage.total_tokens = count("totalTokens");
    return usage;
}

QString CodexAppServerBackend::approvalDetail(const QString& method, const QJsonObject& params) {
    QStringList lines;
    const QString command = params.value(QStringLiteral("command")).toString();
    if (!command.isEmpty()) {
        lines << QStringLiteral("Command: %1").arg(command);
    }
    const QString cwd = params.value(QStringLiteral("cwd")).toString();
    if (!cwd.isEmpty()) {
        lines << QStringLiteral("Folder: %1").arg(cwd);
    }
    const QString reason = params.value(QStringLiteral("reason")).toString();
    if (!reason.isEmpty()) {
        lines << QStringLiteral("Reason: %1").arg(reason);
    }
    if (lines.isEmpty() && method.contains(QLatin1String("fileChange"))) {
        lines << QStringLiteral("Codex wants to apply file changes in the session workspace.");
    }
    return lines.join(QChar(u'\n'));
}

void CodexAppServerBackend::ensureStarted(Ready ready) {
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

void CodexAppServerBackend::handleStarted() {
    const QJsonObject params{
        {QStringLiteral("clientInfo"),
         QJsonObject{{QStringLiteral("name"), QStringLiteral("sak_utility")},
                     {QStringLiteral("title"), QStringLiteral("S.A.K. Utility")},
                     {QStringLiteral("version"), QString::fromLatin1(get_version())}}}};
    m_rpc->request(QStringLiteral("initialize"),
                   params,
                   [this](const QJsonObject&, const QString& error) {
                       m_starting = false;
                       if (error.isEmpty()) {
                           m_rpc->notify(QStringLiteral("initialized"), {});
                           m_initialized = true;
                       }
                       flushReadyCallbacks(error);
                   });
}

void CodexAppServerBackend::flushReadyCallbacks(const QString& error) {
    const auto callbacks = std::exchange(m_ready_callbacks, {});
    for (const auto& callback : callbacks) {
        callback(error);
    }
}

void CodexAppServerBackend::handleExit(int exit_code, const QString& stderr_tail) {
    m_initialized = false;
    m_starting = false;
    m_thread_id.clear();
    const QString reason = tr("Codex stopped (exit %1). %2").arg(exit_code).arg(stderr_tail);
    m_rpc->failAll(reason);
    flushReadyCallbacks(reason);
    failTurn(reason);
}

void CodexAppServerBackend::beginTurn(const TurnRequest& turn) {
    ensureStarted([this, turn](const QString& error) {
        if (!error.isEmpty()) {
            failTurn(error);
            return;
        }
        openThread(turn);
    });
}

void CodexAppServerBackend::openThread(const TurnRequest& turn) {
    if (!m_thread_id.isEmpty() && turn.session_hint == m_thread_id) {
        startTurn(turn);
        return;
    }
    QJsonObject params = threadStartParams(turn.request,
                                           workspaceDirectory(),
                                           approvalPolicy(),
                                           turn.attach_tools ? toolServerEntry() : QJsonObject{});
    QString method = QStringLiteral("thread/start");
    if (!turn.session_hint.isEmpty()) {
        method = QStringLiteral("thread/resume");
        params[QStringLiteral("threadId")] = turn.session_hint;
    }
    m_rpc->request(method, params, [this, turn](const QJsonObject& result, const QString& error) {
        if (!error.isEmpty() && !turn.session_hint.isEmpty()) {
            TurnRequest fresh = turn;  // Resume failed (expired thread): start over.
            fresh.session_hint.clear();
            openThread(fresh);
            return;
        }
        if (!error.isEmpty()) {
            failTurn(error);
            return;
        }
        m_thread_id = result.value(QStringLiteral("thread"))
                          .toObject()
                          .value(QStringLiteral("id"))
                          .toString();
        startTurn(turn);
    });
}

void CodexAppServerBackend::startTurn(const TurnRequest& turn) {
    QJsonObject params{{QStringLiteral("threadId"), m_thread_id},
                       {QStringLiteral("input"), turnInput(turn.request)}};
    if (!turn.request.model.trimmed().isEmpty()) {
        params[QStringLiteral("model")] = turn.request.model.trimmed();
    }
    const QString effort = turn.request.reasoning_effort.trimmed().toLower();
    if (!effort.isEmpty() && effort != QLatin1String("none")) {
        params[QStringLiteral("effort")] = effort;
    }
    m_rpc->request(QStringLiteral("turn/start"),
                   params,
                   [this](const QJsonObject& result, const QString& error) {
                       if (!error.isEmpty()) {
                           failTurn(error);
                           return;
                       }
                       m_turn_id = result.value(QStringLiteral("turn"))
                                       .toObject()
                                       .value(QStringLiteral("id"))
                                       .toString();
                   });
}

void CodexAppServerBackend::handleNotification(const QString& method, const QJsonObject& params) {
    if (method == QLatin1String("item/completed")) {
        handleItemCompleted(params.value(QStringLiteral("item")).toObject());
    } else if (method == QLatin1String("turn/completed")) {
        handleTurnCompleted(params);
    } else if (method == QLatin1String("turn/started")) {
        m_turn_id =
            params.value(QStringLiteral("turn")).toObject().value(QStringLiteral("id")).toString();
    } else if (method == QLatin1String("thread/tokenUsage/updated")) {
        addTurnUsage(usageFromBreakdown(params.value(QStringLiteral("tokenUsage"))
                                            .toObject()
                                            .value(QStringLiteral("last"))
                                            .toObject()));
    } else if (method.startsWith(QLatin1String("account/"))) {
        refreshAccount();
    } else if (method == QLatin1String("error")) {
        Q_EMIT activityText(params.value(QStringLiteral("error"))
                                .toObject()
                                .value(QStringLiteral("message"))
                                .toString());
    }
}

void CodexAppServerBackend::handleItemCompleted(const QJsonObject& item) {
    const QString type = item.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("agentMessage")) {
        appendAssistantText(item.value(QStringLiteral("text")).toString());
    } else if (type == QLatin1String("commandExecution")) {
        Q_EMIT activityText(
            tr("Codex ran: %1").arg(item.value(QStringLiteral("command")).toString()));
    }
}

void CodexAppServerBackend::handleTurnCompleted(const QJsonObject& params) {
    const QJsonObject turn = params.value(QStringLiteral("turn")).toObject();
    const QString status = turn.value(QStringLiteral("status")).toString();
    m_turn_id.clear();
    if (status == QLatin1String("failed")) {
        const QString message = turn.value(QStringLiteral("error"))
                                    .toObject()
                                    .value(QStringLiteral("message"))
                                    .toString();
        failTurn(message.isEmpty() ? tr("Codex turn failed") : message);
        return;
    }
    completeTurn(m_thread_id);
}

void CodexAppServerBackend::handleServerRequest(const QJsonValue& id,
                                                const QString& method,
                                                const QJsonObject& params) {
    if (method == QLatin1String("item/commandExecution/requestApproval") ||
        method == QLatin1String("item/fileChange/requestApproval")) {
        const QString approval_id = QStringLiteral("codex_%1").arg(id.toVariant().toString());
        m_approval_requests.insert(approval_id, id);
        const bool command = method.contains(QLatin1String("commandExecution"));
        raiseApproval(AiAgentApproval{approval_id,
                                      command ? tr("Codex wants to run a command")
                                              : tr("Codex wants to change files"),
                                      approvalDetail(method, params),
                                      true});
        return;
    }
    if (method == QLatin1String("mcpServer/elicitation/request")) {
        // S.A.K.'s own tools are gated by the panel; other servers are declined.
        const bool ours = params.value(QStringLiteral("serverName")).toString() ==
                          QLatin1String(kToolBridgeServerName);
        m_rpc->respond(id,
                       QJsonObject{{QStringLiteral("action"),
                                    ours ? QStringLiteral("accept") : QStringLiteral("decline")},
                                   {QStringLiteral("content"), QJsonObject{}}});
        return;
    }
    m_rpc->respondError(id,
                        kMethodNotFound,
                        QStringLiteral("S.A.K. does not handle %1").arg(method));
}

void CodexAppServerBackend::answerApproval(const QString& approval_id,
                                           AiApprovalDecision decision) {
    const QJsonValue id = m_approval_requests.take(approval_id);
    if (!id.isUndefined()) {
        m_rpc->respond(id, QJsonObject{{QStringLiteral("decision"), decisionValue(decision)}});
    }
}

void CodexAppServerBackend::interruptTurn() {
    if (m_thread_id.isEmpty() || m_turn_id.isEmpty()) {
        return;
    }
    m_rpc->request(QStringLiteral("turn/interrupt"),
                   QJsonObject{{QStringLiteral("threadId"), m_thread_id},
                               {QStringLiteral("turnId"), m_turn_id}},
                   {});
}

void CodexAppServerBackend::resetConversation() {
    AiAgentBackend::resetConversation();
    m_thread_id.clear();
}

void CodexAppServerBackend::publishAccountFromRead(const QJsonObject& result,
                                                   const QString& error) {
    AiAccountStatus status = baseAccountStatus();
    const QJsonObject account = result.value(QStringLiteral("account")).toObject();
    const QString type = account.value(QStringLiteral("type")).toString();
    status.signed_in = !account.isEmpty();
    status.account_label =
        type == QLatin1String("chatgpt") ? account.value(QStringLiteral("email")).toString() : type;
    status.plan = account.value(QStringLiteral("planType")).toString();
    if (!error.isEmpty()) {
        status.detail = error;
    }
    publishAccount(status);
}

void CodexAppServerBackend::refreshAccount() {
    if (!baseAccountStatus().runtime_available) {
        publishAccount(baseAccountStatus());
        return;
    }
    ensureStarted([this](const QString& error) {
        if (!error.isEmpty()) {
            publishAccountFromRead({}, error);
            return;
        }
        m_rpc->request(QStringLiteral("account/read"),
                       QJsonObject{{QStringLiteral("refreshToken"), false}},
                       [this](const QJsonObject& result, const QString& read_error) {
                           publishAccountFromRead(result, read_error);
                       });
    });
}

void CodexAppServerBackend::startSignIn() {
    ensureStarted([this](const QString& error) {
        if (!error.isEmpty()) {
            publishAccountFromRead({}, error);
            return;
        }
        m_rpc->request(QStringLiteral("account/login/start"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("chatgpt")}},
                       [this](const QJsonObject& result, const QString& login_error) {
                           AiAccountStatus status = baseAccountStatus();
                           status.detail = login_error;
                           status.sign_in_pending = login_error.isEmpty();
                           m_login_id = result.value(QStringLiteral("loginId")).toString();
                           publishAccount(status);
                           const QUrl url(result.value(QStringLiteral("authUrl")).toString());
                           if (url.isValid() && !url.isEmpty()) {
                               // Codex hosts its own localhost callback; S.A.K.
                               // only opens the vendor page.
                               Q_EMIT signInUrlReady(url);
                           }
                       });
    });
}

void CodexAppServerBackend::cancelSignIn() {
    if (m_initialized && !m_login_id.isEmpty()) {
        m_rpc->request(QStringLiteral("account/login/cancel"),
                       QJsonObject{{QStringLiteral("loginId"), m_login_id}},
                       {});
    }
    m_login_id.clear();
    refreshAccount();
}

void CodexAppServerBackend::signOut() {
    ensureStarted([this](const QString& error) {
        if (!error.isEmpty()) {
            publishAccountFromRead({}, error);
            return;
        }
        m_rpc->request(QStringLiteral("account/logout"),
                       {},
                       [this](const QJsonObject&, const QString&) { refreshAccount(); });
    });
}

void CodexAppServerBackend::listModels(const QString& credential) {
    Q_UNUSED(credential);
    ensureStarted([this](const QString& error) {
        if (!error.isEmpty()) {
            AiAgentBackend::listModels({});
            return;
        }
        m_rpc->request(QStringLiteral("model/list"),
                       {},
                       [this](const QJsonObject& result, const QString& list_error) {
                           QStringList models;
                           for (const auto& value :
                                result.value(QStringLiteral("data")).toArray()) {
                               const QJsonObject model = value.toObject();
                               if (!model.value(QStringLiteral("hidden")).toBool(false)) {
                                   models.append(model.value(QStringLiteral("model")).toString());
                               }
                           }
                           models.removeAll(QString());
                           if (!list_error.isEmpty() || models.isEmpty()) {
                               AiAgentBackend::listModels({});
                               return;
                           }
                           Q_EMIT modelsReady(models);
                       });
    });
}

}  // namespace sak::ai

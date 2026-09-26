// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_muse_code_backend.h"

#include "sak/ai/ai_api_support.h"
#include "sak/ai/ai_stdio_json_process.h"
#include "sak/ai/ai_tool_bridge_server.h"
#include "sak/version.h"

#include <QDir>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QTimer>
#include <QUuid>

#include <climits>
#include <utility>

namespace sak::ai {

namespace {

constexpr int kMethodNotFound = -32'601;
constexpr int kUuidBytes = 16;
constexpr int kUuidTimestampBytes = 6;
constexpr int kBitsPerByte = 8;
constexpr int kUuidVersionByte = 6;
constexpr int kUuidVariantByte = 8;
constexpr unsigned char kUuidLowNibbleMask = 0x0F;
constexpr unsigned char kUuidVersion7 = 0x70;
constexpr unsigned char kUuidVariantMask = 0x3F;
constexpr unsigned char kUuidVariantRfc = 0x80;
constexpr int kSignInPollMs = 2000;
constexpr qint64 kSignInTimeoutSecs = 300;

QJsonObject textPart(const QString& text) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                       {QStringLiteral("text"), text}};
}

QJsonObject attachmentPart(const OpenAIInputAttachment& attachment) {
    if (attachment.type == OpenAIInputAttachment::Type::Text) {
        return textPart(attachmentText(attachment));
    }
    const auto parts = attachmentBinary(attachment);
    if (!parts || !parts->mime_type.startsWith(QLatin1String("image/"))) {
        return textPart(unsupportedAttachmentNote(attachment));
    }
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("image")},
                       {QStringLiteral("base64Data"), parts->base64_data},
                       {QStringLiteral("mediaType"), parts->mime_type}};
}

bool choiceMatches(const QJsonObject& choice, AiApprovalDecision decision) {
    const QString verdict = choice.value(QStringLiteral("decision")).toString();
    const QString scope = choice.value(QStringLiteral("scope")).toString();
    switch (decision) {
    case AiApprovalDecision::AllowOnce:
        return verdict == QLatin1String("approved") && scope == QLatin1String("once");
    case AiApprovalDecision::AllowForSession:
        return verdict == QLatin1String("approvedForSession") || scope == QLatin1String("session");
    case AiApprovalDecision::Deny:
        break;
    }
    return verdict == QLatin1String("denied");
}

}  // namespace

MuseCodeBackend::MuseCodeBackend(AiAgentRuntime runtime, QObject* parent)
    : AiAgentBackend(ModelProviderId::Muse, std::move(runtime), parent)
    , m_process(new AiStdioJsonProcess(this))
    , m_rpc(new AiJsonRpcPeer(m_process, true))
    , m_sign_in_poll(new QTimer(this)) {
    m_sign_in_poll->setInterval(kSignInPollMs);
    connect(m_sign_in_poll, &QTimer::timeout, this, &MuseCodeBackend::pollSignIn);
    connect(m_process, &AiStdioJsonProcess::finished, this, &MuseCodeBackend::handleExit);
    connect(m_process, &AiStdioJsonProcess::startFailed, this, [this](const QString& error) {
        m_starting = false;
        flushReadyCallbacks(error);
    });
    connect(m_process, &AiStdioJsonProcess::started, this, [this]() {
        const QJsonObject params{
            {QStringLiteral("clientInfo"),
             QJsonObject{{QStringLiteral("name"), QStringLiteral("sak_utility")},
                         {QStringLiteral("version"), QString::fromLatin1(get_version())}}},
            {QStringLiteral("capabilities"),
             QJsonObject{{QStringLiteral("requestedCapabilities"),
                          QJsonArray{QStringLiteral("sessionMcp")}}}}};
        m_rpc->request(QStringLiteral("initialize"),
                       params,
                       [this](const QJsonObject&, const QString& error) {
                           m_starting = false;
                           m_initialized = error.isEmpty();
                           if (m_initialized) {
                               m_rpc->notify(QStringLiteral("initialized"), {});
                           }
                           flushReadyCallbacks(error);
                       });
    });
    connect(
        m_rpc, &AiJsonRpcPeer::notificationReceived, this, &MuseCodeBackend::handleNotification);
    connect(m_rpc, &AiJsonRpcPeer::requestReceived, this, &MuseCodeBackend::handleServerRequest);
}

MuseCodeBackend::~MuseCodeBackend() {
    m_process->disconnect(this);
    m_process->stop();
}

QString MuseCodeBackend::uuidV7() {
    QByteArray bytes(kUuidBytes, Qt::Uninitialized);
    for (auto& byte : bytes) {
        byte = static_cast<char>(QRandomGenerator::system()->bounded(UCHAR_MAX + 1));
    }
    const quint64 now = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
    for (int index = 0; index < kUuidTimestampBytes; ++index) {
        const int shift = (kUuidTimestampBytes - 1 - index) * kBitsPerByte;
        bytes[index] = static_cast<char>((now >> shift) & UCHAR_MAX);
    }
    const auto version = static_cast<unsigned char>(bytes[kUuidVersionByte]);
    bytes[kUuidVersionByte] = static_cast<char>((version & kUuidLowNibbleMask) | kUuidVersion7);
    const auto variant = static_cast<unsigned char>(bytes[kUuidVariantByte]);
    bytes[kUuidVariantByte] = static_cast<char>((variant & kUuidVariantMask) | kUuidVariantRfc);
    return QUuid::fromRfc4122(bytes).toString(QUuid::WithoutBraces);
}

QJsonArray MuseCodeBackend::turnInput(const OpenAIResponseRequest& request) {
    QJsonArray input;
    for (const auto& attachment : request.attachments) {
        input.append(attachmentPart(attachment));
    }
    if (!request.input.trimmed().isEmpty() || input.isEmpty()) {
        input.append(textPart(request.input));
    }
    return input;
}

QString MuseCodeBackend::approvalModeFor(AiApprovalPolicy policy) {
    switch (policy) {
    case AiApprovalPolicy::AllowAll:
        return QStringLiteral("allowAll");
    case AiApprovalPolicy::DenyAll:
        return QStringLiteral("denyUnmatched");
    case AiApprovalPolicy::Ask:
        break;
    }
    return QStringLiteral("promptUnmatched");
}

QString MuseCodeBackend::choiceForDecision(const QJsonArray& choices, AiApprovalDecision decision) {
    for (const auto& value : choices) {
        const QJsonObject choice = value.toObject();
        if (choiceMatches(choice, decision)) {
            return choice.value(QStringLiteral("choiceId")).toString();
        }
    }
    if (decision == AiApprovalDecision::AllowForSession) {
        return choiceForDecision(choices, AiApprovalDecision::AllowOnce);
    }
    return {};
}

TokenUsage MuseCodeBackend::usageFromMsp(const QJsonObject& usage) {
    const auto count = [&usage](const char* key) {
        return static_cast<qint64>(usage.value(QLatin1String(key)).toDouble(0.0));
    };
    TokenUsage tokens;
    tokens.input_tokens = count("inputTokens");
    tokens.cached_input_tokens = count("cachedTokens");
    tokens.output_tokens = count("outputTokens");
    tokens.reasoning_tokens = count("reasoningTokens");
    tokens.total_tokens = tokens.input_tokens + tokens.output_tokens;
    return tokens;
}

bool MuseCodeBackend::isSakTool(const QString& tool_name) {
    const QString name = tool_name.trimmed().toLower();
    const QString server = QString::fromLatin1(kToolBridgeServerName);
    return name.startsWith(QStringLiteral("mcp__%1__").arg(server)) ||
           name.startsWith(server + QChar(u'.')) || name.startsWith(server + QChar(u'/'));
}

QString MuseCodeBackend::credentialFile() const {
    const QProcessEnvironment env = runtime().serverLaunch(provider()).spec.environment;
    QString config_home = env.value(QStringLiteral("XDG_CONFIG_HOME"));
    if (config_home.isEmpty()) {
        const QString home = env.value(QStringLiteral("USERPROFILE"),
                                       env.value(QStringLiteral("HOME")));
        config_home = QDir(home).filePath(QStringLiteral(".config"));
    }
    return QDir(config_home).filePath(QStringLiteral("muse/auth.json"));
}

void MuseCodeBackend::ensureStarted(Ready ready) {
    if (m_initialized) {
        ready({});
        return;
    }
    m_ready_callbacks.append(std::move(ready));
    if (m_starting) {
        return;
    }
    AiAgentLaunch launch = serverLaunch();
    if (!launch.available) {
        flushReadyCallbacks(launch.missing_reason);
        return;
    }
    // The session folder is created by S.A.K.; approvals come from approvalMode.
    launch.spec.arguments << QStringLiteral("--disable-sandbox")
                          << QStringLiteral("--trust-workspace");
    m_starting = true;
    m_process->start(launch.spec);
}

void MuseCodeBackend::flushReadyCallbacks(const QString& error) {
    const auto callbacks = std::exchange(m_ready_callbacks, {});
    for (const auto& callback : callbacks) {
        callback(error);
    }
}

void MuseCodeBackend::handleExit(int exit_code, const QString& stderr_tail) {
    m_initialized = false;
    m_starting = false;
    m_session_id.clear();
    m_approvals.clear();
    const QString reason = tr("Muse Code stopped (exit %1). %2").arg(exit_code).arg(stderr_tail);
    m_rpc->failAll(reason);
    flushReadyCallbacks(reason);
    failTurn(reason);
}

void MuseCodeBackend::beginTurn(const TurnRequest& turn) {
    ensureStarted([this, turn](const QString& error) {
        if (!error.isEmpty()) {
            failTurn(error);
            return;
        }
        if (!m_session_id.isEmpty() && turn.session_hint == m_session_id) {
            startTurn(turn);
            return;
        }
        openSession(turn);
    });
}

void MuseCodeBackend::openSession(const TurnRequest& turn) {
    QJsonObject params{{QStringLiteral("commandId"), uuidV7()},
                       {QStringLiteral("workspaceRoot"),
                        QDir::toNativeSeparators(workspaceDirectory())},
                       {QStringLiteral("approvalMode"), approvalModeFor(approvalPolicy())}};
    if (!turn.request.model.trimmed().isEmpty()) {
        params[QStringLiteral("modelId")] = turn.request.model.trimmed();
    }
    const QJsonObject tool_server = turn.attach_tools ? toolServerEntry() : QJsonObject{};
    if (!tool_server.isEmpty()) {
        QJsonObject server = tool_server;
        server[QStringLiteral("transport")] = QStringLiteral("stdio");
        server[QStringLiteral("mode")] = QStringLiteral("optional");
        params[QStringLiteral("config")] =
            QJsonObject{{QStringLiteral("mcpServers"),
                         QJsonObject{{QString::fromLatin1(kToolBridgeServerName), server}}}};
    }
    m_rpc->request(QStringLiteral("session/start"),
                   params,
                   [this, turn](const QJsonObject& result, const QString& error) {
                       if (!error.isEmpty()) {
                           failTurn(error);
                           return;
                       }
                       m_session_id = result.value(QStringLiteral("session"))
                                          .toObject()
                                          .value(QStringLiteral("sessionId"))
                                          .toString();
                       startTurn(turn);
                   });
}

void MuseCodeBackend::startTurn(const TurnRequest& turn) {
    QJsonObject params{{QStringLiteral("sessionId"), m_session_id},
                       {QStringLiteral("commandId"), uuidV7()},
                       {QStringLiteral("input"), turnInput(turn.request)}};
    const QString effort = turn.request.reasoning_effort.trimmed().toLower();
    if (!effort.isEmpty() && effort != QLatin1String("none")) {
        params[QStringLiteral("reasoningEffort")] = effort;
    }
    m_rpc->request(QStringLiteral("turn/start"),
                   params,
                   [this](const QJsonObject& result, const QString& error) {
                       if (!error.isEmpty()) {
                           failTurn(error);
                           return;
                       }
                       m_turn_id = result.value(QStringLiteral("turnId")).toString();
                   });
}

void MuseCodeBackend::handleNotification(const QString& method, const QJsonObject& params) {
    const QJsonObject item = params.value(QStringLiteral("item")).toObject();
    const QString kind = item.value(QStringLiteral("kind")).toString();
    if (method == QLatin1String("item/started")) {
        m_item_kinds.insert(item.value(QStringLiteral("itemId")).toString(), kind);
        if (kind == QLatin1String("toolCall")) {
            Q_EMIT activityText(
                tr("Muse is using %1").arg(item.value(QStringLiteral("tool")).toString()));
        }
    } else if (method == QLatin1String("item/completed") && kind == QLatin1String("agentMessage")) {
        appendAssistantText(item.value(QStringLiteral("text")).toString());
    } else if (method == QLatin1String("turn/completed")) {
        handleTurnCompleted(params);
    }
}

void MuseCodeBackend::handleTurnCompleted(const QJsonObject& params) {
    m_turn_id.clear();
    m_item_kinds.clear();
    addTurnUsage(usageFromMsp(params.value(QStringLiteral("usage")).toObject()));
    if (params.value(QStringLiteral("terminal")).toString() == QLatin1String("failed")) {
        const QString message = params.value(QStringLiteral("error"))
                                    .toObject()
                                    .value(QStringLiteral("message"))
                                    .toString();
        failTurn(message.isEmpty() ? tr("Muse turn failed") : message);
        return;
    }
    completeTurn(m_session_id);
}

void MuseCodeBackend::handleServerRequest(const QJsonValue& id,
                                          const QString& method,
                                          const QJsonObject& params) {
    if (method != QLatin1String("approval/request")) {
        m_rpc->respondError(id, kMethodNotFound, QStringLiteral("Not supported by S.A.K."));
        return;
    }
    m_rpc->respond(id, {});  // Receipt only; the decision is a separate command.
    const QString approval_id = params.value(QStringLiteral("approvalId")).toString();
    m_approvals.insert(approval_id,
                       PendingApproval{params.value(QStringLiteral("currentRequirementId")),
                                       params.value(QStringLiteral("availableChoices")).toArray()});
    const QString tool = params.value(QStringLiteral("toolName")).toString();
    if (isSakTool(tool)) {
        answerApproval(approval_id, AiApprovalDecision::AllowForSession);
        return;
    }
    raiseApproval(AiAgentApproval{approval_id,
                                  tr("Muse wants to use %1").arg(tool),
                                  params.value(QStringLiteral("rawArgs")).toString(),
                                  true});
}

void MuseCodeBackend::answerApproval(const QString& approval_id, AiApprovalDecision decision) {
    const PendingApproval pending = m_approvals.take(approval_id);
    const QString choice = choiceForDecision(pending.choices, decision);
    if (choice.isEmpty() || m_session_id.isEmpty()) {
        return;
    }
    m_rpc->request(QStringLiteral("approval/decide"),
                   QJsonObject{{QStringLiteral("sessionId"), m_session_id},
                               {QStringLiteral("commandId"), uuidV7()},
                               {QStringLiteral("approvalId"), approval_id},
                               {QStringLiteral("requirementId"), pending.requirement_id},
                               {QStringLiteral("choiceId"), choice}},
                   {});
}

void MuseCodeBackend::interruptTurn() {
    if (m_session_id.isEmpty()) {
        return;
    }
    QJsonObject params{{QStringLiteral("sessionId"), m_session_id},
                       {QStringLiteral("commandId"), uuidV7()}};
    if (!m_turn_id.isEmpty()) {
        params[QStringLiteral("turnId")] = m_turn_id;
    }
    m_rpc->request(QStringLiteral("turn/interrupt"), params, {});
}

void MuseCodeBackend::resetConversation() {
    AiAgentBackend::resetConversation();
    m_session_id.clear();
}

void MuseCodeBackend::refreshAccount() {
    AiAccountStatus status = baseAccountStatus();
    // Presence only, as the Muse Code CLI itself reports it.
    status.signed_in = QFileInfo::exists(credentialFile());
    status.sign_in_pending = m_sign_in_poll->isActive();
    if (!status.runtime_available) {
        status.detail = tr("Install Muse Code to use a Muse subscription");
    }
    publishAccount(status);
}

void MuseCodeBackend::startSignIn() {
    m_credential_stamp = QFileInfo(credentialFile()).lastModified();
    m_sign_in_deadline = QDateTime::currentDateTimeUtc().addSecs(kSignInTimeoutSecs);
    m_sign_in_poll->start();
    AiAccountStatus status = baseAccountStatus();
    status.sign_in_pending = status.runtime_available;
    status.detail = tr("Finish signing in in the Muse Code window");
    publishAccount(status);
    runVisibleSignIn({QStringLiteral("login")}, [this](int, const QByteArray&) { pollSignIn(); });
}

void MuseCodeBackend::pollSignIn() {
    const QFileInfo credential(credentialFile());
    // Only a credential written after sign-in started counts (a stale file
    // from an expired session would otherwise read as signed in).
    const bool fresh = credential.exists() && credential.lastModified() != m_credential_stamp;
    if (fresh || QDateTime::currentDateTimeUtc() > m_sign_in_deadline) {
        m_sign_in_poll->stop();
        refreshAccount();
    }
}

void MuseCodeBackend::cancelSignIn() {
    m_sign_in_poll->stop();
    cancelVisibleSignIn();
    refreshAccount();
}

void MuseCodeBackend::signOut() {
    resetConversation();
    runRuntimeCommand({QStringLiteral("logout")},
                      [this](int, const QByteArray&) { refreshAccount(); });
}

void MuseCodeBackend::listModels(const QString& credential) {
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
                                result.value(QStringLiteral("models")).toArray()) {
                               models
                                   << value.toObject().value(QStringLiteral("modelId")).toString();
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

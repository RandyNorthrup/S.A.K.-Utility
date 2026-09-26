// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_agent_backend.h"

#include <QHash>
#include <QJsonValue>

#include <functional>

namespace sak::ai {

class AiJsonRpcPeer;
class AiStdioJsonProcess;

/// @brief GPT via the bundled Codex runtime (`codex app-server`), signed in
/// with the technician's ChatGPT plan through Codex's own browser login.
class CodexAppServerBackend : public AiAgentBackend {
    Q_OBJECT

public:
    explicit CodexAppServerBackend(AiAgentRuntime runtime, QObject* parent = nullptr);
    ~CodexAppServerBackend() override;

    void refreshAccount() override;
    void startSignIn() override;
    void cancelSignIn() override;
    void signOut() override;
    void listModels(const QString& credential) override;
    void resetConversation() override;

    [[nodiscard]] static QJsonObject threadStartParams(const OpenAIResponseRequest& request,
                                                       const QString& cwd,
                                                       AiApprovalPolicy policy,
                                                       const QJsonObject& tool_server);
    [[nodiscard]] static QJsonArray turnInput(const OpenAIResponseRequest& request);
    [[nodiscard]] static TokenUsage usageFromBreakdown(const QJsonObject& breakdown);
    [[nodiscard]] static QString approvalDetail(const QString& method, const QJsonObject& params);

protected:
    void beginTurn(const TurnRequest& turn) override;
    void interruptTurn() override;
    void answerApproval(const QString& approval_id, AiApprovalDecision decision) override;

private:
    using Ready = std::function<void(const QString& error)>;

    void ensureStarted(Ready ready);
    void handleStarted();
    void handleExit(int exit_code, const QString& stderr_tail);
    void flushReadyCallbacks(const QString& error);
    void openThread(const TurnRequest& turn);
    void startTurn(const TurnRequest& turn);
    void handleNotification(const QString& method, const QJsonObject& params);
    void handleItemCompleted(const QJsonObject& item);
    void handleTurnCompleted(const QJsonObject& params);
    void handleServerRequest(const QJsonValue& id,
                             const QString& method,
                             const QJsonObject& params);
    void publishAccountFromRead(const QJsonObject& result, const QString& error);

    AiStdioJsonProcess* m_process;
    AiJsonRpcPeer* m_rpc;
    bool m_initialized{false};
    bool m_starting{false};
    QVector<Ready> m_ready_callbacks;
    QString m_thread_id;
    QString m_turn_id;
    QString m_login_id;
    QHash<QString, QJsonValue> m_approval_requests;
};

}  // namespace sak::ai

// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_agent_backend.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonValue>

#include <functional>

namespace sak::ai {

class AiJsonRpcPeer;
class AiStdioJsonProcess;

/// @brief Gemini via the bundled Gemini CLI speaking the Agent Client Protocol.
///
/// Google-account sign-in runs inside Gemini CLI (it opens the browser and
/// completes its own loopback redirect). Since 2026-06-18 Google only serves
/// Gemini CLI for Workspace / Gemini Code Assist Standard and Enterprise
/// accounts; consumer Google AI plans are not served, which the UI states.
class GeminiCliBackend : public AiAgentBackend {
    Q_OBJECT

public:
    explicit GeminiCliBackend(AiAgentRuntime runtime, QObject* parent = nullptr);
    ~GeminiCliBackend() override;

    void refreshAccount() override;
    void startSignIn() override;
    void cancelSignIn() override;
    void signOut() override;
    void resetConversation() override;

    [[nodiscard]] static QJsonArray promptBlocks(const OpenAIResponseRequest& request);
    [[nodiscard]] static QJsonArray mcpServers(const QJsonObject& tool_server);
    /// @brief Pick the ACP permission option matching a decision.
    [[nodiscard]] static QString optionForDecision(const QJsonArray& options,
                                                   AiApprovalDecision decision);
    [[nodiscard]] static TokenUsage usageFromMeta(const QJsonObject& meta);

protected:
    void beginTurn(const TurnRequest& turn) override;
    void interruptTurn() override;
    void answerApproval(const QString& approval_id, AiApprovalDecision decision) override;

private:
    using Ready = std::function<void(const QString& error)>;
    struct PendingPermission {
        QJsonValue rpc_id;
        QJsonArray options;
    };

    void ensureStarted(Ready ready);
    void flushReadyCallbacks(const QString& error);
    void handleExit(int exit_code, const QString& stderr_tail);
    void openSession(const TurnRequest& turn);
    void sendPrompt(const TurnRequest& turn);
    void handleUpdate(const QJsonObject& params);
    void handleServerRequest(const QJsonValue& id,
                             const QString& method,
                             const QJsonObject& params);
    [[nodiscard]] QString credentialFile() const;

    AiStdioJsonProcess* m_process;
    AiJsonRpcPeer* m_rpc;
    bool m_initialized{false};
    bool m_starting{false};
    QVector<Ready> m_ready_callbacks;
    QString m_session_id;
    QString m_session_model;
    QHash<QString, PendingPermission> m_permissions;
};

}  // namespace sak::ai

// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_agent_backend.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonValue>

#include <functional>

class QTimer;

namespace sak::ai {

class AiJsonRpcPeer;
class AiStdioJsonProcess;

/// @brief Muse via the technician's installed Muse Code CLI (`muse serve`,
/// Muse Session Protocol), billed to their Muse subscription.
///
/// Sign-in runs `muse login` in its own console window and completes when the
/// CLI writes its credential file; S.A.K. never reads that file's contents.
class MuseCodeBackend : public AiAgentBackend {
    Q_OBJECT

public:
    explicit MuseCodeBackend(AiAgentRuntime runtime, QObject* parent = nullptr);
    ~MuseCodeBackend() override;

    void refreshAccount() override;
    void startSignIn() override;
    void cancelSignIn() override;
    void signOut() override;
    void listModels(const QString& credential) override;
    void resetConversation() override;

    [[nodiscard]] static QString uuidV7();
    [[nodiscard]] static QJsonArray turnInput(const OpenAIResponseRequest& request);
    [[nodiscard]] static QString approvalModeFor(AiApprovalPolicy policy);
    [[nodiscard]] static QString choiceForDecision(const QJsonArray& choices,
                                                   AiApprovalDecision decision);
    [[nodiscard]] static TokenUsage usageFromMsp(const QJsonObject& usage);
    [[nodiscard]] static bool isSakTool(const QString& tool_name);

protected:
    void beginTurn(const TurnRequest& turn) override;
    void interruptTurn() override;
    void answerApproval(const QString& approval_id, AiApprovalDecision decision) override;

private:
    using Ready = std::function<void(const QString& error)>;
    struct PendingApproval {
        QJsonValue requirement_id;
        QJsonArray choices;
    };

    void ensureStarted(Ready ready);
    void flushReadyCallbacks(const QString& error);
    void handleExit(int exit_code, const QString& stderr_tail);
    void openSession(const TurnRequest& turn);
    void startTurn(const TurnRequest& turn);
    void handleNotification(const QString& method, const QJsonObject& params);
    void handleTurnCompleted(const QJsonObject& params);
    void handleServerRequest(const QJsonValue& id,
                             const QString& method,
                             const QJsonObject& params);
    void pollSignIn();
    [[nodiscard]] QString credentialFile() const;

    AiStdioJsonProcess* m_process;
    AiJsonRpcPeer* m_rpc;
    QTimer* m_sign_in_poll;
    bool m_initialized{false};
    bool m_starting{false};
    QVector<Ready> m_ready_callbacks;
    QString m_session_id;
    QString m_turn_id;
    QHash<QString, QString> m_item_kinds;
    QHash<QString, PendingApproval> m_approvals;
    QDateTime m_credential_stamp;
    QDateTime m_sign_in_deadline;
};

}  // namespace sak::ai

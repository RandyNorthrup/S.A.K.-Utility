// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_agent_backend.h"

#include <QHash>
#include <QJsonObject>

namespace sak::ai {

class AiStdioJsonProcess;

/// @brief Claude via the unmodified Claude Code executable, signed in with the
/// technician's own Claude plan.
///
/// Scope is kept to what Anthropic's published conditions allow for hosting
/// Claude Code: sign-in runs through Claude Code's own `auth login` in its own
/// console window (S.A.K. never sees codes or tokens), Claude Code keeps its
/// own system prompt, and S.A.K. adds only its tools (a standard MCP server)
/// and app knowledge (CLAUDE.md in the session workspace). S.A.K. does not use
/// SDK-only extension hooks and does not run multi-agent workflows on a Claude
/// subscription.
class ClaudeCodeBackend : public AiAgentBackend {
    Q_OBJECT

public:
    explicit ClaudeCodeBackend(AiAgentRuntime runtime, QObject* parent = nullptr);
    ~ClaudeCodeBackend() override;

    void refreshAccount() override;
    void startSignIn() override;
    void cancelSignIn() override;
    void signOut() override;
    void resetConversation() override;

    [[nodiscard]] static QStringList launchArguments(const OpenAIResponseRequest& request,
                                                     const QString& resume_session_id,
                                                     const QJsonObject& tool_server);
    [[nodiscard]] static QJsonObject userMessage(const OpenAIResponseRequest& request);
    [[nodiscard]] static AiAccountStatus parseAuthStatus(const QByteArray& json);
    [[nodiscard]] static TokenUsage usageFromResult(const QJsonObject& usage);

protected:
    void beginTurn(const TurnRequest& turn) override;
    void interruptTurn() override;
    void answerApproval(const QString& approval_id, AiApprovalDecision decision) override;

private:
    void startProcess(const TurnRequest& turn);
    void stopProcess();
    void sendControlRequest(const QJsonObject& request);
    void sendControlResponse(const QString& request_id, const QJsonObject& response);
    void handleMessage(const QJsonObject& message);
    void handleControlRequest(const QJsonObject& message);
    void handleAssistant(const QJsonObject& message);
    void handleResult(const QJsonObject& message);
    void handleExit(int exit_code, const QString& stderr_tail);
    [[nodiscard]] static QString processSignature(const TurnRequest& turn);

    AiStdioJsonProcess* m_process;
    QString m_session_id;
    QString m_signature;
    QJsonObject m_pending_message;
    bool m_initialized{false};
    bool m_stopping{false};
    int m_next_request{1};
    QHash<QString, QJsonObject> m_permission_inputs;
    QString m_auth_error;
};

}  // namespace sak::ai

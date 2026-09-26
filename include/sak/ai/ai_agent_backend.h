// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_agent_runtime.h"
#include "sak/ai/ai_chat_backend.h"

#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QVector>

#include <functional>

class QProcess;

namespace sak::ai {

class AiToolBridgeServer;

/// @brief Common behaviour for subscription backends that drive a vendor agent.
///
/// The vendor runtime runs its own agent loop with its own system prompt. S.A.K.
/// contributes only (1) its tools, as a standard MCP server, and (2) app
/// knowledge, as the runtime's project instructions file in the session
/// workspace. Tool calls arriving over MCP are handed to the panel as ordinary
/// function calls, so the panel's tool loop, access mode and human gates apply
/// unchanged; the panel's function outputs are returned over MCP.
class AiAgentBackend : public AiChatBackend {
    Q_OBJECT

public:
    AiAgentBackend(ModelProviderId provider, AiAgentRuntime runtime, QObject* parent);
    ~AiAgentBackend() override;

    [[nodiscard]] ModelProviderId provider() const override { return m_provider; }
    [[nodiscard]] ModelAuthMode authMode() const override { return ModelAuthMode::Subscription; }

    void createResponse(const OpenAIResponseRequest& request) override;
    void countInputTokens(const OpenAIResponseRequest& request, const QString& request_id) override;
    [[nodiscard]] bool supportsInputTokenCount() const override { return false; }
    void listModels(const QString& credential) override;
    void cancel() override;
    [[nodiscard]] bool isBusy() const override;
    void resetConversation() override;
    void setWorkspaceDirectory(const QString& directory) override;
    void setApprovalPolicy(AiApprovalPolicy policy) override;
    void resolveApproval(const QString& approval_id, AiApprovalDecision decision) override;

    /// @brief App knowledge handed to the runtime (tone lines removed).
    [[nodiscard]] static QString knowledgeText(const QString& instructions);
    [[nodiscard]] static QString knowledgeFileName(ModelProviderId provider);

protected:
    struct TurnRequest {
        OpenAIResponseRequest request;
        /// Runtime session/thread id to continue or resume; empty = new.
        QString session_hint;
        bool attach_tools{false};
    };

    virtual void beginTurn(const TurnRequest& turn) = 0;
    virtual void interruptTurn() = 0;
    virtual void answerApproval(const QString& approval_id, AiApprovalDecision decision) = 0;

    [[nodiscard]] const AiAgentRuntime& runtime() const { return m_runtime; }
    [[nodiscard]] AiAgentLaunch serverLaunch() const;
    [[nodiscard]] QString workspaceDirectory() const;
    [[nodiscard]] AiApprovalPolicy approvalPolicy() const { return m_policy; }
    /// @brief Stdio MCP server entry (command, args, env) for the S.A.K. tools.
    [[nodiscard]] QJsonObject toolServerEntry();
    [[nodiscard]] bool turnActive() const { return m_turn_active; }

    void appendAssistantText(const QString& text);
    void appendAssistantChunk(const QString& chunk);
    void addTurnUsage(const TokenUsage& usage);
    void completeTurn(const QString& session_id);
    void failTurn(const QString& error_message);
    /// @brief Apply the approval policy, or ask the user.
    void raiseApproval(const AiAgentApproval& approval);
    void publishAccount(AiAccountStatus status);
    [[nodiscard]] AiAccountStatus baseAccountStatus() const;

    using CommandFinished = std::function<void(int exit_code, const QByteArray& output)>;
    /// @brief Run a one-shot runtime command (e.g. `auth status`) asynchronously.
    void runRuntimeCommand(const QStringList& arguments, CommandFinished finished);
    /// @brief Run the vendor's own interactive sign-in in a visible console
    /// window; S.A.K. never sees the codes or tokens it exchanges.
    void runVisibleSignIn(const QStringList& arguments, CommandFinished finished);
    void cancelVisibleSignIn();

private:
    void handleToolCall(const QString& call_id, const QString& name, const QString& arguments);
    void emitNextToolCall();
    void deliverToolOutputs(const QVector<OpenAIFunctionOutput>& outputs);
    void writeKnowledgeFile(const QString& instructions);
    void resetTurnState();

    ModelProviderId m_provider;
    AiAgentRuntime m_runtime;
    QString m_workspace;
    AiApprovalPolicy m_policy{AiApprovalPolicy::Ask};
    AiToolBridgeServer* m_bridge{nullptr};
    bool m_turn_active{false};
    QString m_turn_text;
    TokenUsage m_turn_usage;
    QString m_session_id;
    QVector<OpenAIFunctionCall> m_queued_calls;
    QString m_outstanding_call;
    QSet<QString> m_pending_approvals;
    QPointer<QProcess> m_sign_in_process;
};

}  // namespace sak::ai

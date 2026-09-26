// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_chat_backend.h"

#include <QHash>

#include <functional>

namespace sak::ai {

/// @brief Routes the assistant's chat traffic to the selected provider backend.
///
/// Backends are created lazily per provider/auth-mode pair and kept alive so
/// switching back and forth preserves sign-in state and conversation caches.
/// Only the active backend's request signals are forwarded; account status is
/// forwarded from every backend so the UI can show all sign-in states.
class AiModelRouter : public AiChatBackend {
    Q_OBJECT

public:
    using BackendFactory =
        std::function<AiChatBackend*(ModelProviderId, ModelAuthMode, QObject* parent)>;

    explicit AiModelRouter(QObject* parent = nullptr);
    explicit AiModelRouter(BackendFactory factory, QObject* parent = nullptr);
    ~AiModelRouter() override;

    void setActive(ModelProviderId provider, ModelAuthMode mode);
    [[nodiscard]] AiChatBackend* activeBackend();

    [[nodiscard]] ModelProviderId provider() const override { return m_provider; }
    [[nodiscard]] ModelAuthMode authMode() const override { return m_mode; }

    void createResponse(const OpenAIResponseRequest& request) override;
    void countInputTokens(const OpenAIResponseRequest& request, const QString& request_id) override;
    void listModels(const QString& credential) override;
    void cancel() override;
    [[nodiscard]] bool isBusy() const override;
    [[nodiscard]] bool supportsInputTokenCount() const override;

    void refreshAccount() override;
    void startSignIn() override;
    void cancelSignIn() override;
    void signOut() override;
    void resetConversation() override;
    void setWorkspaceDirectory(const QString& directory) override;
    void setApprovalPolicy(AiApprovalPolicy policy) override;
    void resolveApproval(const QString& approval_id, AiApprovalDecision decision) override;

    /// @brief Factory for every built-in provider backend.
    [[nodiscard]] static AiChatBackend* createDefaultBackend(ModelProviderId provider,
                                                             ModelAuthMode mode,
                                                             QObject* parent);

private:
    [[nodiscard]] static int backendKey(ModelProviderId provider, ModelAuthMode mode);
    [[nodiscard]] AiChatBackend* backendFor(ModelProviderId provider, ModelAuthMode mode);
    void connectBackend(AiChatBackend* backend);

    BackendFactory m_factory;
    QHash<int, AiChatBackend*> m_backends;
    ModelProviderId m_provider{ModelProviderId::OpenAI};
    ModelAuthMode m_mode{ModelAuthMode::ApiKey};
    QString m_workspace_directory;
    AiApprovalPolicy m_approval_policy{AiApprovalPolicy::Ask};
};

}  // namespace sak::ai

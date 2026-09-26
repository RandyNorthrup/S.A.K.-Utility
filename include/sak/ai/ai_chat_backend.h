// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_model_provider.h"
#include "sak/ai/openai_response_types.h"

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace sak::ai {

/// @brief Sign-in and runtime state for a provider/auth-mode pair.
struct AiAccountStatus {
    ModelProviderId provider{ModelProviderId::OpenAI};
    ModelAuthMode auth_mode{ModelAuthMode::ApiKey};
    bool runtime_available{false};
    bool signed_in{false};
    bool sign_in_pending{false};
    QString account_label;
    QString plan;
    QString runtime_path;
    QString detail;
};

/// @brief Provider-neutral, signal-based chat transport used by the assistant.
///
/// The request/result structs keep their historical OpenAI names because they
/// are the panel's lingua franca: every backend translates them to its own
/// wire format. Stateless vendor APIs emulate `previous_response_id` chaining
/// with a local history cache, and agent runtimes map it onto their own
/// thread/session ids, so the panel's tool loop works unchanged.
class AiChatBackend : public QObject {
    Q_OBJECT

public:
    explicit AiChatBackend(QObject* parent = nullptr);
    ~AiChatBackend() override;

    AiChatBackend(const AiChatBackend&) = delete;
    AiChatBackend& operator=(const AiChatBackend&) = delete;

    [[nodiscard]] virtual ModelProviderId provider() const = 0;
    [[nodiscard]] virtual ModelAuthMode authMode() const = 0;

    virtual void createResponse(const OpenAIResponseRequest& request) = 0;
    virtual void countInputTokens(const OpenAIResponseRequest& request,
                                  const QString& request_id) = 0;
    /// @param credential API key for ApiKey mode; ignored by agent runtimes.
    virtual void listModels(const QString& credential) = 0;
    virtual void cancel() = 0;
    [[nodiscard]] virtual bool isBusy() const = 0;
    [[nodiscard]] virtual bool supportsInputTokenCount() const { return true; }

    /// @brief Probe runtime + sign-in state; emits accountStatusChanged.
    virtual void refreshAccount();
    /// @brief Begin the vendor's own browser sign-in; emits signInUrlReady.
    virtual void startSignIn();
    virtual void cancelSignIn();
    virtual void signOut();
    /// @brief Drop cached conversation state (new chat / session switch).
    virtual void resetConversation();

Q_SIGNALS:
    void requestStarted();
    void requestFinished();
    void responseReady(const sak::ai::OpenAIResponseResult& result);
    void modelsReady(const QStringList& model_ids);
    void inputTokenCountReady(const QString& request_id, qint64 input_tokens);
    void inputTokenCountFailed(const QString& request_id, const QString& error_message);
    void requestFailed(const QString& error_message);
    void accountStatusChanged(const sak::ai::AiAccountStatus& status);
    void signInUrlReady(const QUrl& url);
    void activityText(const QString& text);
};

}  // namespace sak::ai

Q_DECLARE_METATYPE(sak::ai::AiAccountStatus)

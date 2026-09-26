// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_api_support.h"
#include "sak/ai/ai_chat_backend.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>

#include <optional>

class QNetworkReply;

namespace sak::ai {

/// @brief Gemini via the Gemini Developer API (generateContent) with an API key.
///
/// generateContent is stateless: model turns are cached verbatim (thought
/// signatures must be echoed back) and addressed by synthetic response ids.
class GeminiApiBackend : public AiChatBackend {
    Q_OBJECT

public:
    struct ParsedCandidate {
        OpenAIResponseResult result;
        QJsonObject model_content;
        QString finish_reason;
    };

    explicit GeminiApiBackend(QObject* parent = nullptr);
    ~GeminiApiBackend() override;

    [[nodiscard]] ModelProviderId provider() const override { return ModelProviderId::Google; }
    [[nodiscard]] ModelAuthMode authMode() const override { return ModelAuthMode::ApiKey; }

    void createResponse(const OpenAIResponseRequest& request) override;
    void countInputTokens(const OpenAIResponseRequest& request, const QString& request_id) override;
    void listModels(const QString& credential) override;
    void cancel() override;
    [[nodiscard]] bool isBusy() const override { return !m_reply.isNull(); }
    void resetConversation() override;

    [[nodiscard]] static std::optional<QJsonArray> turnContents(
        const OpenAIResponseRequest& request,
        const std::optional<QJsonArray>& history,
        QString* error_message);
    [[nodiscard]] static QJsonObject buildPayload(const OpenAIResponseRequest& request,
                                                  const QJsonArray& contents);
    [[nodiscard]] static QJsonArray userParts(const OpenAIResponseRequest& request);
    [[nodiscard]] static std::optional<ParsedCandidate> parseResponse(const QByteArray& data,
                                                                      QString* error_message);
    [[nodiscard]] static QStringList parseModels(const QByteArray& data, QString* error_message);
    [[nodiscard]] static QString extractApiError(const QByteArray& data);
    /// @brief Call id for the n-th functionCall part when the model sent none.
    [[nodiscard]] static QString syntheticCallId(int ordinal);

private:
    [[nodiscard]] QNetworkRequest apiRequest(const QString& path, const QString& api_key) const;
    void handleTurnFinished(QNetworkReply* reply);
    void finishTurn(const QString& error_message);
    void handleCountFinished(QNetworkReply* reply, const QString& request_id);
    void handleModelsFinished(QNetworkReply* reply);

    QNetworkAccessManager m_network;
    ConversationHistoryCache m_history;
    QPointer<QNetworkReply> m_reply;
    QPointer<QNetworkReply> m_count_reply;
    QJsonArray m_pending_contents;
};

}  // namespace sak::ai

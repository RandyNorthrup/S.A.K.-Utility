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

/// @brief Claude via the Anthropic Messages API with a user API key.
///
/// The Messages API is stateless, so full transcripts (including thinking and
/// server-tool blocks, which must be echoed back verbatim) are kept in a
/// ConversationHistoryCache and addressed by synthetic response ids.
class AnthropicApiBackend : public AiChatBackend {
    Q_OBJECT

public:
    struct ParsedMessage {
        OpenAIResponseResult result;
        QJsonArray assistant_content;
        QString stop_reason;
        QString refusal_detail;
    };

    explicit AnthropicApiBackend(QObject* parent = nullptr);
    ~AnthropicApiBackend() override;

    [[nodiscard]] ModelProviderId provider() const override { return ModelProviderId::Anthropic; }
    [[nodiscard]] ModelAuthMode authMode() const override { return ModelAuthMode::ApiKey; }

    void createResponse(const OpenAIResponseRequest& request) override;
    void countInputTokens(const OpenAIResponseRequest& request, const QString& request_id) override;
    void listModels(const QString& credential) override;
    void cancel() override;
    [[nodiscard]] bool isBusy() const override { return !m_reply.isNull(); }
    void resetConversation() override;

    /// @brief Messages for this turn: cached history plus the new user turn.
    [[nodiscard]] static std::optional<QJsonArray> turnMessages(
        const OpenAIResponseRequest& request,
        const std::optional<QJsonArray>& history,
        QString* error_message);
    [[nodiscard]] static QJsonObject buildPayload(const OpenAIResponseRequest& request,
                                                  const QJsonArray& messages);
    [[nodiscard]] static QJsonArray userContent(const OpenAIResponseRequest& request);
    [[nodiscard]] static QJsonArray toolDefinitions(const OpenAIResponseRequest& request);
    [[nodiscard]] static std::optional<ParsedMessage> parseMessage(const QByteArray& data,
                                                                   QString* error_message);
    [[nodiscard]] static QStringList parseModels(const QByteArray& data, QString* error_message);
    [[nodiscard]] static QString extractApiError(const QByteArray& data);

private:
    struct PendingTurn {
        QString api_key;
        QJsonObject payload;
        QJsonArray messages;
        int pause_resumes{0};
        QString partial_text;
        QVector<OpenAIUrlCitation> partial_citations;
        TokenUsage usage;
    };

    [[nodiscard]] QNetworkRequest apiRequest(const QString& path,
                                             const QString& api_key,
                                             const QString& model) const;
    void postTurn();
    void handleTurnFinished(QNetworkReply* reply);
    void completeTurn(const ParsedMessage& parsed);
    void failTurn(const QString& error_message);
    void handleCountFinished(QNetworkReply* reply, const QString& request_id);
    void handleModelsFinished(QNetworkReply* reply);

    QNetworkAccessManager m_network;
    ConversationHistoryCache m_history;
    QPointer<QNetworkReply> m_reply;
    QPointer<QNetworkReply> m_count_reply;
    PendingTurn m_turn;
};

}  // namespace sak::ai

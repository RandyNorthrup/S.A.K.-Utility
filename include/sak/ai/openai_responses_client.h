// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/openai_response_types.h"

#include <QByteArray>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class QNetworkReply;

namespace sak::ai {

class OpenAIResponsesClient : public QObject {
    Q_OBJECT

public:
    explicit OpenAIResponsesClient(QObject* parent = nullptr);
    ~OpenAIResponsesClient() override;

    OpenAIResponsesClient(const OpenAIResponsesClient&) = delete;
    OpenAIResponsesClient& operator=(const OpenAIResponsesClient&) = delete;

    /// @brief Point the client at an OpenAI-compatible Responses API host.
    void setBaseUrl(const QString& base_url);
    void setVendorLabel(const QString& vendor_label);

    void createResponse(const OpenAIResponseRequest& request);
    void countInputTokens(const OpenAIResponseRequest& request, const QString& request_id);
    void listModels(const QString& api_key);
    void cancel();

    [[nodiscard]] bool isBusy() const noexcept { return m_current_reply != nullptr; }

    [[nodiscard]] static OpenAIResponseResult parseResponseObject(const QByteArray& data,
                                                                  QString* error_message);
    [[nodiscard]] static QStringList parseModelsList(const QByteArray& data,
                                                     QString* error_message);
    [[nodiscard]] static QString extractApiError(const QByteArray& data);
    [[nodiscard]] static qint64 parseInputTokenCountObject(const QByteArray& data,
                                                           QString* error_message);
    [[nodiscard]] static bool hasUsableApiKey(const QString& api_key) noexcept;
    [[nodiscard]] static QByteArray buildResponsePayloadForTesting(
        const OpenAIResponseRequest& request);
    /// @brief S.A.K. local tool definitions (OpenAI function-tool shape) for
    /// other provider backends to translate into their own tool formats.
    [[nodiscard]] static QJsonArray localToolDefinitionsForProviders();

Q_SIGNALS:
    void requestStarted();
    void requestFinished();
    void responseReady(const sak::ai::OpenAIResponseResult& result);
    void modelsReady(const QStringList& model_ids);
    void inputTokenCountReady(const QString& request_id, qint64 input_tokens);
    void inputTokenCountFailed(const QString& request_id, const QString& error_message);
    void requestFailed(const QString& error_message);

private:
    [[nodiscard]] QNetworkRequest buildRequest(const QString& path, const QString& api_key) const;
    [[nodiscard]] QByteArray buildResponsePayload(const OpenAIResponseRequest& request) const;
    void setCurrentReply(QNetworkReply* reply);
    void clearCurrentReply(QNetworkReply* reply);
    void handleCreateFinished(QNetworkReply* reply);
    void handleModelsFinished(QNetworkReply* reply);
    void handleInputTokenCountFinished(QNetworkReply* reply, const QString& request_id);
    void cancelInputTokenCount();

    QString m_base_url;
    QString m_vendor_label;
    QNetworkAccessManager m_network_manager;
    QNetworkReply* m_current_reply{nullptr};
    QNetworkReply* m_input_tokens_reply{nullptr};
};

}  // namespace sak::ai

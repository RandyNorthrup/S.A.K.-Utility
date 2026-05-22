// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/openai_response_types.h"

#include <QByteArray>
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

    void createResponse(const OpenAIResponseRequest& request);
    void listModels(const QString& api_key);
    void cancel();

    [[nodiscard]] bool isBusy() const noexcept { return m_current_reply != nullptr; }

    [[nodiscard]] static OpenAIResponseResult parseResponseObject(const QByteArray& data,
                                                                  QString* error_message);
    [[nodiscard]] static QStringList parseModelsList(const QByteArray& data,
                                                     QString* error_message);
    [[nodiscard]] static QString extractApiError(const QByteArray& data);
    [[nodiscard]] static bool hasUsableApiKey(const QString& api_key) noexcept;
    [[nodiscard]] static QByteArray buildResponsePayloadForTesting(
        const OpenAIResponseRequest& request);

Q_SIGNALS:
    void requestStarted();
    void requestFinished();
    void responseReady(const sak::ai::OpenAIResponseResult& result);
    void modelsReady(const QStringList& model_ids);
    void requestFailed(const QString& error_message);

private:
    [[nodiscard]] QNetworkRequest buildRequest(const QString& path, const QString& api_key) const;
    [[nodiscard]] QByteArray buildResponsePayload(const OpenAIResponseRequest& request) const;
    void setCurrentReply(QNetworkReply* reply);
    void clearCurrentReply(QNetworkReply* reply);
    void handleCreateFinished(QNetworkReply* reply);
    void handleModelsFinished(QNetworkReply* reply);

    QNetworkAccessManager m_network_manager;
    QNetworkReply* m_current_reply{nullptr};
};

}  // namespace sak::ai

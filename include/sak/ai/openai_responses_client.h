// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_token_usage_tracker.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class QNetworkReply;

namespace sak::ai {

struct OpenAIFunctionCall {
    QString call_id;
    QString name;
    QString arguments_json;
};

struct OpenAIUrlCitation {
    QString url;
    QString title;
    int start_index{-1};
    int end_index{-1};
};

struct OpenAIFunctionOutput {
    QString call_id;
    QString output;
};

struct OpenAIInputAttachment {
    enum class Type {
        Text,
        Image,
        File,
    };

    Type type{Type::Text};
    QString label;
    QString text;
    QString image_url;
    QString filename;
    QString file_data;
    QString detail{QStringLiteral("auto")};
};

struct OpenAIResponseRequest {
    QString api_key;
    QString model;
    QString instructions;
    QString input;
    QVector<OpenAIInputAttachment> attachments;
    QVector<OpenAIFunctionOutput> function_outputs;
    QString reasoning_effort;
    QString previous_response_id;
    bool enable_web_search{false};
    bool enable_local_tools{false};
};

struct OpenAIResponseResult {
    QString id;
    QString output_text;
    QString raw_json;
    TokenUsage usage;
    QVector<OpenAIFunctionCall> function_calls;
    QVector<OpenAIUrlCitation> citations;
};

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

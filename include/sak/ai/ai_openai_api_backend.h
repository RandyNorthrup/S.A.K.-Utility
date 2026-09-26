// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_chat_backend.h"
#include "sak/ai/openai_responses_client.h"

namespace sak::ai {

/// @brief Where an OpenAI-compatible Responses API lives and how it differs.
struct ResponsesApiEndpoint {
    ModelProviderId provider{ModelProviderId::OpenAI};
    QString base_url;
    QString web_search_tool_type;

    [[nodiscard]] static ResponsesApiEndpoint openAi();
    /// Meta Model API: Responses-compatible, `web_search` tool type.
    [[nodiscard]] static ResponsesApiEndpoint muse();
};

/// @brief AiChatBackend over the OpenAI Responses API client.
///
/// Also serves OpenAI-compatible vendors (Muse via the Meta Model API), which
/// keep server-side `previous_response_id` state like OpenAI does.
class OpenAIApiBackend : public AiChatBackend {
    Q_OBJECT

public:
    explicit OpenAIApiBackend(QObject* parent = nullptr);
    OpenAIApiBackend(ResponsesApiEndpoint endpoint, QObject* parent);
    ~OpenAIApiBackend() override;

    [[nodiscard]] ModelProviderId provider() const override { return m_endpoint.provider; }
    [[nodiscard]] ModelAuthMode authMode() const override { return ModelAuthMode::ApiKey; }

    void createResponse(const OpenAIResponseRequest& request) override;
    void countInputTokens(const OpenAIResponseRequest& request, const QString& request_id) override;
    void listModels(const QString& credential) override;
    void cancel() override;
    [[nodiscard]] bool isBusy() const override;

private:
    [[nodiscard]] OpenAIResponseRequest endpointRequest(const OpenAIResponseRequest& request) const;

    ResponsesApiEndpoint m_endpoint;
    OpenAIResponsesClient* m_client;
};

}  // namespace sak::ai

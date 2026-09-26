// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_openai_api_backend.h"

#include <utility>

namespace sak::ai {

ResponsesApiEndpoint ResponsesApiEndpoint::openAi() {
    return {ModelProviderId::OpenAI,
            QStringLiteral("https://api.openai.com"),
            QStringLiteral("web_search_preview")};
}

ResponsesApiEndpoint ResponsesApiEndpoint::muse() {
    return {ModelProviderId::Muse,
            QStringLiteral("https://api.meta.ai"),
            QStringLiteral("web_search")};
}

OpenAIApiBackend::OpenAIApiBackend(QObject* parent)
    : OpenAIApiBackend(ResponsesApiEndpoint::openAi(), parent) {}

OpenAIApiBackend::OpenAIApiBackend(ResponsesApiEndpoint endpoint, QObject* parent)
    : AiChatBackend(parent)
    , m_endpoint(std::move(endpoint))
    , m_client(new OpenAIResponsesClient(this)) {
    m_client->setBaseUrl(m_endpoint.base_url);
    m_client->setVendorLabel(modelProviderInfo(m_endpoint.provider).vendor);
    connect(m_client, &OpenAIResponsesClient::requestStarted, this, &AiChatBackend::requestStarted);
    connect(
        m_client, &OpenAIResponsesClient::requestFinished, this, &AiChatBackend::requestFinished);
    connect(m_client, &OpenAIResponsesClient::responseReady, this, &AiChatBackend::responseReady);
    connect(m_client, &OpenAIResponsesClient::modelsReady, this, &AiChatBackend::modelsReady);
    connect(m_client,
            &OpenAIResponsesClient::inputTokenCountReady,
            this,
            &AiChatBackend::inputTokenCountReady);
    connect(m_client,
            &OpenAIResponsesClient::inputTokenCountFailed,
            this,
            &AiChatBackend::inputTokenCountFailed);
    connect(m_client, &OpenAIResponsesClient::requestFailed, this, &AiChatBackend::requestFailed);
}

OpenAIApiBackend::~OpenAIApiBackend() = default;

OpenAIResponseRequest OpenAIApiBackend::endpointRequest(
    const OpenAIResponseRequest& request) const {
    OpenAIResponseRequest adjusted = request;
    adjusted.web_search_tool_type = m_endpoint.web_search_tool_type;
    return adjusted;
}

void OpenAIApiBackend::createResponse(const OpenAIResponseRequest& request) {
    m_client->createResponse(endpointRequest(request));
}

void OpenAIApiBackend::countInputTokens(const OpenAIResponseRequest& request,
                                        const QString& request_id) {
    m_client->countInputTokens(endpointRequest(request), request_id);
}

void OpenAIApiBackend::listModels(const QString& credential) {
    m_client->listModels(credential);
}

void OpenAIApiBackend::cancel() {
    m_client->cancel();
}

bool OpenAIApiBackend::isBusy() const {
    return m_client->isBusy();
}

}  // namespace sak::ai

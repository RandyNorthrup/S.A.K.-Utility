// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_anthropic_api_backend.h"
#include "sak/ai/ai_gemini_api_backend.h"
#include "sak/ai/ai_model_router.h"
#include "sak/ai/ai_openai_api_backend.h"

namespace sak::ai {

namespace {

AiChatBackend* createApiKeyBackend(ModelProviderId provider, QObject* parent) {
    switch (provider) {
    case ModelProviderId::OpenAI:
        return new OpenAIApiBackend(ResponsesApiEndpoint::openAi(), parent);
    case ModelProviderId::Anthropic:
        return new AnthropicApiBackend(parent);
    case ModelProviderId::Muse:
        return new OpenAIApiBackend(ResponsesApiEndpoint::muse(), parent);
    case ModelProviderId::Google:
        return new GeminiApiBackend(parent);
    }
    return nullptr;
}

AiChatBackend* createSubscriptionBackend(ModelProviderId provider, QObject* parent) {
    Q_UNUSED(provider);
    Q_UNUSED(parent);
    return nullptr;
}

}  // namespace

AiChatBackend* AiModelRouter::createDefaultBackend(ModelProviderId provider,
                                                   ModelAuthMode mode,
                                                   QObject* parent) {
    return mode == ModelAuthMode::ApiKey ? createApiKeyBackend(provider, parent)
                                         : createSubscriptionBackend(provider, parent);
}

}  // namespace sak::ai

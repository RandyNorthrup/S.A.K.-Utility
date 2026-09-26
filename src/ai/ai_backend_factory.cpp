// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_anthropic_api_backend.h"
#include "sak/ai/ai_claude_code_backend.h"
#include "sak/ai/ai_codex_backend.h"
#include "sak/ai/ai_gemini_api_backend.h"
#include "sak/ai/ai_gemini_cli_backend.h"
#include "sak/ai/ai_model_router.h"
#include "sak/ai/ai_muse_code_backend.h"
#include "sak/ai/ai_openai_api_backend.h"

#include <utility>

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
    AiAgentRuntime runtime = AiAgentRuntime::forApplication();
    switch (provider) {
    case ModelProviderId::OpenAI:
        return new CodexAppServerBackend(std::move(runtime), parent);
    case ModelProviderId::Anthropic:
        return new ClaudeCodeBackend(std::move(runtime), parent);
    case ModelProviderId::Google:
        return new GeminiCliBackend(std::move(runtime), parent);
    case ModelProviderId::Muse:
        return new MuseCodeBackend(std::move(runtime), parent);
    }
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

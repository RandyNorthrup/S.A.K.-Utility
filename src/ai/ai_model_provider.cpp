// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_model_provider.h"

#include <algorithm>

namespace sak::ai {

namespace {

constexpr qsizetype kOpenAiMinKeyLength = 20;
constexpr qsizetype kAnthropicMinKeyLength = 40;
constexpr qsizetype kGoogleMinKeyLength = 30;
constexpr qsizetype kMuseMinKeyLength = 20;

constexpr qint64 kClaudeMillionTokenContext = 1'000'000;
constexpr qint64 kClaudeHaikuContext = 200'000;
constexpr qint64 kMillionTokenContext = 1'048'576;

ModelProviderInfo openAiInfo() {
    ModelProviderInfo info;
    info.id = ModelProviderId::OpenAI;
    info.key = QStringLiteral("openai");
    info.display_name = QStringLiteral("GPT (OpenAI)");
    info.model_family = QStringLiteral("GPT");
    info.vendor = QStringLiteral("OpenAI");
    info.agent_name = QStringLiteral("Codex");
    info.agent_directory = QStringLiteral("codex");
    info.agent_executable = QStringLiteral("codex.exe");
    info.api_models = {QStringLiteral("gpt-5.5"),
                       QStringLiteral("gpt-5.4"),
                       QStringLiteral("gpt-5.4-mini")};
    info.subscription_models = info.api_models;
    info.api_key_console_url = QStringLiteral("https://platform.openai.com/api-keys");
    info.api_key_min_length = kOpenAiMinKeyLength;
    return info;
}

ModelProviderInfo anthropicInfo() {
    ModelProviderInfo info;
    info.id = ModelProviderId::Anthropic;
    info.key = QStringLiteral("anthropic");
    info.display_name = QStringLiteral("Claude (Anthropic)");
    info.model_family = QStringLiteral("Claude");
    info.vendor = QStringLiteral("Anthropic");
    info.agent_name = QStringLiteral("Claude Code");
    info.agent_directory = QStringLiteral("claude");
    info.agent_executable = QStringLiteral("claude.exe");
    info.api_models = {QStringLiteral("claude-opus-5"),
                       QStringLiteral("claude-sonnet-5"),
                       QStringLiteral("claude-haiku-4-5"),
                       QStringLiteral("claude-fable-5-1")};
    // Claude Code accepts model aliases that track the latest release.
    info.subscription_models = {QStringLiteral("opus"),
                                QStringLiteral("sonnet"),
                                QStringLiteral("haiku")};
    info.api_key_console_url = QStringLiteral("https://platform.claude.com/settings/keys");
    info.api_key_min_length = kAnthropicMinKeyLength;
    return info;
}

ModelProviderInfo googleInfo() {
    ModelProviderInfo info;
    info.id = ModelProviderId::Google;
    info.key = QStringLiteral("google");
    info.display_name = QStringLiteral("Gemini (Google)");
    info.model_family = QStringLiteral("Gemini");
    info.vendor = QStringLiteral("Google");
    info.agent_name = QStringLiteral("Gemini CLI");
    info.agent_directory = QStringLiteral("gemini");
    info.agent_executable = QStringLiteral("node.exe");
    info.api_models = {QStringLiteral("gemini-3.1-pro-preview"),
                       QStringLiteral("gemini-3.8-flash"),
                       QStringLiteral("gemini-3.5-flash-lite"),
                       QStringLiteral("gemini-2.5-pro")};
    // Gemini CLI model aliases; "auto" lets the CLI route per request.
    info.subscription_models = {QStringLiteral("auto"),
                                QStringLiteral("pro"),
                                QStringLiteral("flash"),
                                QStringLiteral("flash-lite")};
    info.api_key_console_url = QStringLiteral("https://aistudio.google.com/apikey");
    info.api_key_min_length = kGoogleMinKeyLength;
    return info;
}

ModelProviderInfo museInfo() {
    ModelProviderInfo info;
    info.id = ModelProviderId::Muse;
    info.key = QStringLiteral("muse");
    info.display_name = QStringLiteral("Muse (Meta)");
    info.model_family = QStringLiteral("Muse");
    info.vendor = QStringLiteral("Meta");
    info.agent_name = QStringLiteral("Muse Code");
    info.agent_directory = QStringLiteral("muse");
    info.agent_executable = QStringLiteral("muse.exe");
    // The "-contributor" price tier lets Meta train on prompts, so it is not
    // offered by default for a tool that handles customer machine data.
    info.api_models = {QStringLiteral("muse-spark-1.3"), QStringLiteral("muse-spark-1.2")};
    info.subscription_models = info.api_models;
    info.api_key_console_url = QStringLiteral("https://dev.meta.ai");
    info.api_key_min_length = kMuseMinKeyLength;
    info.agent_redistribution_verified = false;
    return info;
}

QString normalizedKey(const QString& value) {
    return value.trimmed().toLower();
}

}  // namespace

const QVector<ModelProviderInfo>& modelProviders() {
    static const QVector<ModelProviderInfo> kProviders{
        openAiInfo(), anthropicInfo(), googleInfo(), museInfo()};
    return kProviders;
}

const ModelProviderInfo& modelProviderInfo(ModelProviderId id) {
    const auto& providers = modelProviders();
    const auto it = std::find_if(providers.cbegin(), providers.cend(), [id](const auto& info) {
        return info.id == id;
    });
    return it == providers.cend() ? providers.front() : *it;
}

QString modelProviderKey(ModelProviderId id) {
    return modelProviderInfo(id).key;
}

std::optional<ModelProviderId> modelProviderFromKey(const QString& key) {
    const QString wanted = normalizedKey(key);
    for (const auto& info : modelProviders()) {
        if (info.key == wanted) {
            return info.id;
        }
    }
    return std::nullopt;
}

QString modelAuthModeKey(ModelAuthMode mode) {
    return mode == ModelAuthMode::Subscription ? QStringLiteral("subscription")
                                               : QStringLiteral("api_key");
}

std::optional<ModelAuthMode> modelAuthModeFromKey(const QString& key) {
    const QString wanted = normalizedKey(key);
    if (wanted == QLatin1String("subscription")) {
        return ModelAuthMode::Subscription;
    }
    if (wanted == QLatin1String("api_key")) {
        return ModelAuthMode::ApiKey;
    }
    return std::nullopt;
}

bool hasUsableApiKey(ModelProviderId id, const QString& api_key) {
    const QString trimmed = api_key.trimmed();
    if (trimmed.contains(QChar(u' '))) {
        return false;
    }
    return trimmed.size() >= modelProviderInfo(id).api_key_min_length;
}

std::optional<ModelProviderId> modelProviderForModel(const QString& model_id) {
    struct PrefixRule {
        QLatin1String prefix;
        ModelProviderId provider;
    };
    static const PrefixRule kRules[] = {
        {QLatin1String("claude"), ModelProviderId::Anthropic},
        {QLatin1String("opus"), ModelProviderId::Anthropic},
        {QLatin1String("sonnet"), ModelProviderId::Anthropic},
        {QLatin1String("haiku"), ModelProviderId::Anthropic},
        {QLatin1String("gemini"), ModelProviderId::Google},
        {QLatin1String("models/"), ModelProviderId::Google},
        {QLatin1String("muse"), ModelProviderId::Muse},
        {QLatin1String("gpt"), ModelProviderId::OpenAI},
        {QLatin1String("o3"), ModelProviderId::OpenAI},
        {QLatin1String("o4"), ModelProviderId::OpenAI},
        {QLatin1String("codex"), ModelProviderId::OpenAI},
    };
    const QString model = normalizedKey(model_id);
    for (const auto& rule : kRules) {
        if (!model.isEmpty() && model.startsWith(rule.prefix)) {
            return rule.provider;
        }
    }
    return std::nullopt;
}

qint64 providerContextWindowTokens(const QString& model_id) {
    const QString model = normalizedKey(model_id);
    if (model.startsWith(QLatin1String("claude-haiku")) || model == QLatin1String("haiku")) {
        return kClaudeHaikuContext;
    }
    if (model.startsWith(QLatin1String("claude")) || model == QLatin1String("opus") ||
        model == QLatin1String("sonnet")) {
        return kClaudeMillionTokenContext;
    }
    if (model.startsWith(QLatin1String("gemini-2.5")) ||
        model.startsWith(QLatin1String("gemini-3")) ||
        model.startsWith(QLatin1String("muse-spark"))) {
        return kMillionTokenContext;
    }
    return 0;
}

}  // namespace sak::ai

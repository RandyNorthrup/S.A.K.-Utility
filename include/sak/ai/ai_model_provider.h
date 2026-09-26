// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace sak::ai {

/// @brief Frontier model families the AI assistant can talk to.
enum class ModelProviderId {
    OpenAI,
    Anthropic,
    Google,
    Muse,
};

/// @brief How the user pays for / authenticates with a provider.
///
/// ApiKey talks to the vendor HTTP API directly with a user-supplied key.
/// Subscription drives the vendor's bundled native agent runtime (Codex,
/// Claude Code, Gemini CLI, Muse Code) which owns the browser sign-in flow and
/// bills the user's consumer subscription.
enum class ModelAuthMode {
    ApiKey,
    Subscription,
};

struct ModelProviderInfo {
    ModelProviderId id{ModelProviderId::OpenAI};
    QString key;
    QString display_name;
    QString model_family;
    QString vendor;
    QString agent_name;
    QString agent_directory;
    QString agent_executable;
    QStringList api_models;
    QStringList subscription_models;
    QString api_key_console_url;
    qsizetype api_key_min_length{0};
    bool api_supported{true};
    bool api_supports_token_count{true};
    bool subscription_supported{true};
    /// False until redistribution permission for the vendor agent binary has
    /// been confirmed; the UI surfaces this and packaging skips the binary.
    bool agent_redistribution_verified{true};
};

[[nodiscard]] const QVector<ModelProviderInfo>& modelProviders();
[[nodiscard]] const ModelProviderInfo& modelProviderInfo(ModelProviderId id);
[[nodiscard]] QString modelProviderKey(ModelProviderId id);
[[nodiscard]] std::optional<ModelProviderId> modelProviderFromKey(const QString& key);
[[nodiscard]] QString modelAuthModeKey(ModelAuthMode mode);
[[nodiscard]] std::optional<ModelAuthMode> modelAuthModeFromKey(const QString& key);

/// @brief Cheap client-side sanity check before a key is sent to the vendor.
[[nodiscard]] bool hasUsableApiKey(ModelProviderId id, const QString& api_key);

/// @brief Best-effort provider guess for a model id (used for legacy sessions).
[[nodiscard]] std::optional<ModelProviderId> modelProviderForModel(const QString& model_id);

/// @brief Published context window for a non-OpenAI model id, or 0 if unknown.
[[nodiscard]] qint64 providerContextWindowTokens(const QString& model_id);

}  // namespace sak::ai

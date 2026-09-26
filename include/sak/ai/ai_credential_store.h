// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_model_provider.h"

#include <QString>

namespace sak::ai {

/// @brief DPAPI-encrypted, per-provider API key storage.
///
/// The provider-less overloads address the OpenAI key and keep the original
/// file name and DPAPI entropy so keys remembered before multi-provider
/// support still load.
class CredentialStore {
public:
    [[nodiscard]] bool isPersistentStorageAvailable() const noexcept;
    [[nodiscard]] QString loadApiKey(QString* error_message = nullptr) const;
    [[nodiscard]] bool saveApiKey(const QString& api_key, QString* error_message = nullptr) const;
    [[nodiscard]] bool deleteApiKey(QString* error_message = nullptr) const;
    [[nodiscard]] QString credentialFilePath() const;

    [[nodiscard]] QString loadApiKey(ModelProviderId provider,
                                     QString* error_message = nullptr) const;
    [[nodiscard]] bool saveApiKey(ModelProviderId provider,
                                  const QString& api_key,
                                  QString* error_message = nullptr) const;
    [[nodiscard]] bool deleteApiKey(ModelProviderId provider,
                                    QString* error_message = nullptr) const;
    [[nodiscard]] QString credentialFilePath(ModelProviderId provider) const;

    [[nodiscard]] static QString redactSecrets(const QString& text);
};

}  // namespace sak::ai

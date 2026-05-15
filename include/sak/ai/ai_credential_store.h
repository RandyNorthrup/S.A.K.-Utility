// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QString>

namespace sak::ai {

class CredentialStore {
public:
    [[nodiscard]] bool isPersistentStorageAvailable() const noexcept;
    [[nodiscard]] QString loadApiKey(QString* error_message = nullptr) const;
    [[nodiscard]] bool saveApiKey(const QString& api_key, QString* error_message = nullptr) const;
    [[nodiscard]] bool deleteApiKey(QString* error_message = nullptr) const;
    [[nodiscard]] QString credentialFilePath() const;

    [[nodiscard]] static QString redactSecrets(const QString& text);
};

}  // namespace sak::ai

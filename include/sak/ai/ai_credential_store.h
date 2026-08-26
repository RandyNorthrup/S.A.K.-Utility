// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QString>

namespace sak::ai {

class CredentialStore {
public:
    [[nodiscard]] bool isPersistentStorageAvailable() const noexcept;
    [[nodiscard]] QString loadApiKey(QString* error_message = nullptr) const;
    [[nodiscard]] bool saveApiKey(const QString& api_key, QString* error_message = nullptr) const;
    [[nodiscard]] bool deleteApiKey(QString* error_message = nullptr) const;
    [[nodiscard]] static QString credentialFilePath();

    [[nodiscard]] static QString redactSecrets(const QString& text);

    /// @brief redactSecrets applied to a JSON document, VALUE BY VALUE.
    ///
    /// Do not reach for redactSecrets() on serialized JSON instead: the assignment pattern
    /// rewrites `"token":"abc"` to `token=[redacted]`, which deletes the quotes and the colon and
    /// destroys the document, so the re-parse fails and the record is lost. This walks the tree so
    /// the structure is never touched:
    ///   - a value whose KEY names a secret (password/passwd/secret/token/api_key) is replaced
    ///     wholesale, however it is spelled and whatever it holds;
    ///   - any other string value is passed through redactSecrets(), which catches a secret
    ///     embedded in captured stdout;
    ///   - objects and arrays are walked recursively; non-string scalars are left alone.
    [[nodiscard]] static QJsonObject redactSecretsInJson(const QJsonObject& object);
};

}  // namespace sak::ai

// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <expected>
#include <functional>

class AiCredentialStoreTests;

namespace sak::ai {

class CredentialStore {
    friend class ::AiCredentialStoreTests;

public:
    [[nodiscard]] static bool isPersistentStorageAvailable() noexcept;
    [[nodiscard]] static QString loadApiKey(QString* error_message = nullptr);
    [[nodiscard]] static bool saveApiKey(const QString& api_key, QString* error_message = nullptr);
    [[nodiscard]] static bool deleteApiKey(QString* error_message = nullptr);
    [[nodiscard]] static QString credentialFilePath();

    /// @brief DPAPI current-user protection for authenticated local state.
    /// Purpose is mandatory entropy that prevents ciphertext reuse across record types.
    [[nodiscard]] static std::expected<QByteArray, QString> protectCurrentUserData(
        const QByteArray& plaintext, const QByteArray& purpose);
    [[nodiscard]] static std::expected<QByteArray, QString> unprotectCurrentUserData(
        const QByteArray& ciphertext, const QByteArray& purpose);

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

private:
    static void setDpapiPlaintextWipeObserverForTesting(
        std::function<void(const QByteArray&)> observer);
    static void setCredentialDecryptInputWipeObserverForTesting(
        std::function<void(const QByteArray&)> observer);
    [[nodiscard]] static QString decryptCredentialBytesForTesting(QByteArray encrypted,
                                                                  QString* error_message);
};

}  // namespace sak::ai

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
    ///   - a value whose KEY names a secret (sak::ai::keyNamesASecret: password, access_token,
    ///     client_secret, Authorization, Cookie, ...) is replaced wholesale, whatever it holds;
    ///   - any other string value is passed through redactSecrets(), which catches a secret
    ///     embedded in captured stdout;
    ///   - objects and arrays are walked recursively; non-string scalars are left alone.
    [[nodiscard]] static QJsonObject redactSecretsInJson(const QJsonObject& object);

#ifdef SAK_AI_TEST_HOOKS
private:
    // Test-only (compiled only into tests that define SAK_AI_TEST_HOOKS): observe the buffers
    // the store wipes, and drive the decrypt path directly.
    static void setDpapiPlaintextWipeObserverForTesting(
        std::function<void(const QByteArray&)> observer);
    static void setCredentialDecryptInputWipeObserverForTesting(
        std::function<void(const QByteArray&)> observer);
    [[nodiscard]] static QString decryptCredentialBytesForTesting(QByteArray encrypted,
                                                                  QString* error_message);
    // loadApiKey / saveApiKey against a caller-chosen file, so the file format and the DACL can
    // be tested in a temporary directory instead of the shared portable data root.
    [[nodiscard]] static QString loadApiKeyFromPathForTesting(const QString& path,
                                                              QString* error_message);
    [[nodiscard]] static bool saveApiKeyToPathForTesting(const QString& path,
                                                         const QString& api_key,
                                                         QString* error_message);
#endif  // SAK_AI_TEST_HOOKS
};

}  // namespace sak::ai

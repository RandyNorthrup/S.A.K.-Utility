// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_credential_store.h"

#include "sak/ai/ai_paths.h"
#include "sak/ai/ai_secret_redaction.h"
#include "sak/secure_memory.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>

#include <algorithm>
#include <optional>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>

#include <dpapi.h>
#endif

namespace sak::ai {

namespace {

// An API key is a short token (real provider keys are well under a kilobyte). Cap the accepted
// length so a hostile/huge value cannot be encrypted/serialized, and so the qsizetype->DWORD
// narrowing at the DPAPI boundary can never truncate the protected plaintext.
constexpr int kMaxApiKeyChars = 8192;

// ASCII control-character boundaries: code points below the first printable character (space,
// U+0020) are C0 controls, and U+007F is DEL. A real API key never contains either.
constexpr char16_t kAsciiControlLimit = 0x20;
constexpr char16_t kAsciiDelete = 0x7F;

#ifdef Q_OS_WIN
constexpr auto kCredentialProvider = "dpapi-current-user-v1";
constexpr char kDpapiEntropy[] = "SAK Utility/OpenAI API Key/v1";
// The credential file holds one Base64 DPAPI blob wrapping a single API key; a few KiB
// at most. Cap the read so a corrupt/oversized file cannot be slurped whole into memory.
constexpr qint64 kMaxCredentialFileBytes = 256LL * 1024LL;

[[nodiscard]] QString winErrorMessage(DWORD code) {
    return QStringLiteral("Windows error %1").arg(static_cast<qulonglong>(code));
}
#endif

void setError(QString* error_message, const QString& message) {
    if (error_message != nullptr) {
        *error_message = message;
    }
}

#ifdef Q_OS_WIN
std::optional<QJsonObject> readCredentialRoot(const QString& path, QString* error_message) {
    QFile file(path);
    if (!file.exists()) {
        return QJsonObject{};
    }
    if (file.size() > kMaxCredentialFileBytes) {
        setError(error_message,
                 QStringLiteral("Encrypted API key file is implausibly large (%1 bytes)")
                     .arg(file.size()));
        return std::nullopt;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setError(
            error_message,
            QStringLiteral("Could not read encrypted API key file: %1").arg(file.errorString()));
        return std::nullopt;
    }

    // Bound the read at the cap+1 so a file that grew (or was swapped) between the size check
    // above and here cannot force an unbounded allocation -- the pre-check is a fast reject, this
    // is the real ceiling.
    const QByteArray raw = file.read(kMaxCredentialFileBytes + 1);
    if (raw.size() > kMaxCredentialFileBytes) {
        setError(error_message, QStringLiteral("Encrypted API key file is implausibly large"));
        return std::nullopt;
    }
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        setError(
            error_message,
            QStringLiteral("Encrypted API key file is invalid: %1").arg(parse_error.errorString()));
        return std::nullopt;
    }
    return doc.object();
}

std::optional<QByteArray> encryptedCredentialBytes(const QJsonObject& root,
                                                   QString* error_message) {
    if (root.value(QStringLiteral("provider")).toString() != QLatin1String(kCredentialProvider)) {
        setError(error_message, QStringLiteral("Encrypted API key provider is unsupported"));
        return std::nullopt;
    }
    // Strict Base64: reject any non-Base64 byte instead of silently discarding it, so a
    // corrupted ciphertext fails closed here rather than decoding to a different blob.
    const auto decoded = QByteArray::fromBase64Encoding(
        root.value(QStringLiteral("ciphertext")).toString().toLatin1(),
        QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) {
        setError(error_message, QStringLiteral("Encrypted API key payload is not valid Base64"));
        return std::nullopt;
    }
    if (decoded.decoded.isEmpty()) {
        setError(error_message, QStringLiteral("Encrypted API key payload is empty"));
        return std::nullopt;
    }
    return decoded.decoded;
}

QString decryptCredentialBytes(QByteArray encrypted, QString* error_message) {
    DATA_BLOB in_blob{};
    in_blob.pbData = reinterpret_cast<BYTE*>(encrypted.data());
    in_blob.cbData = static_cast<DWORD>(encrypted.size());

    QByteArray entropy(kDpapiEntropy, static_cast<int>(sizeof(kDpapiEntropy) - 1));
    DATA_BLOB entropy_blob{};
    entropy_blob.pbData = reinterpret_cast<BYTE*>(entropy.data());
    entropy_blob.cbData = static_cast<DWORD>(entropy.size());

    DATA_BLOB out_blob{};
    if (CryptUnprotectData(&in_blob,
                           nullptr,
                           &entropy_blob,
                           nullptr,
                           nullptr,
                           CRYPTPROTECT_UI_FORBIDDEN,
                           &out_blob) == 0) {
        setError(error_message,
                 QStringLiteral("Could not decrypt encrypted API key file: %1")
                     .arg(winErrorMessage(GetLastError())));
        return {};
    }

    QByteArray bytes(reinterpret_cast<const char*>(out_blob.pbData),
                     static_cast<int>(out_blob.cbData));
    const QString api_key = QString::fromUtf8(bytes);
    sak::secure_wiper::wipe(bytes.data(), static_cast<std::size_t>(bytes.size()));
    if (out_blob.pbData != nullptr) {
        SecureZeroMemory(out_blob.pbData, out_blob.cbData);
        LocalFree(out_blob.pbData);
    }
    sak::secure_wiper::wipe(encrypted.data(), static_cast<std::size_t>(encrypted.size()));
    sak::secure_wiper::wipe(entropy.data(), static_cast<std::size_t>(entropy.size()));
    return api_key;
}

bool ensureCredentialDirectory(const QString& path, QString* error_message) {
    const QFileInfo info(path);
    if (QDir().mkpath(info.absolutePath())) {
        return true;
    }
    setError(error_message,
             QStringLiteral("Could not create credential directory: %1").arg(info.absolutePath()));
    return false;
}

std::optional<QJsonObject> protectedCredentialRoot(const QString& api_key, QString* error_message) {
    QByteArray bytes = api_key.toUtf8();
    DATA_BLOB in_blob{};
    in_blob.pbData = reinterpret_cast<BYTE*>(bytes.data());
    in_blob.cbData = static_cast<DWORD>(bytes.size());

    QByteArray entropy(kDpapiEntropy, static_cast<int>(sizeof(kDpapiEntropy) - 1));
    DATA_BLOB entropy_blob{};
    entropy_blob.pbData = reinterpret_cast<BYTE*>(entropy.data());
    entropy_blob.cbData = static_cast<DWORD>(entropy.size());

    DATA_BLOB out_blob{};
    const BOOL protected_ok = CryptProtectData(&in_blob,
                                               L"SAK Utility OpenAI API Key",
                                               &entropy_blob,
                                               nullptr,
                                               nullptr,
                                               CRYPTPROTECT_UI_FORBIDDEN,
                                               &out_blob);
    sak::secure_wiper::wipe(bytes.data(), static_cast<std::size_t>(bytes.size()));
    sak::secure_wiper::wipe(entropy.data(), static_cast<std::size_t>(entropy.size()));
    if (protected_ok == 0) {
        setError(error_message, winErrorMessage(GetLastError()));
        return std::nullopt;
    }

    const QByteArray encrypted(reinterpret_cast<const char*>(out_blob.pbData),
                               static_cast<int>(out_blob.cbData));
    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("provider")] = QString::fromLatin1(kCredentialProvider);
    root[QStringLiteral("created_utc")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    root[QStringLiteral("ciphertext")] = QString::fromLatin1(encrypted.toBase64());

    if (out_blob.pbData != nullptr) {
        SecureZeroMemory(out_blob.pbData, out_blob.cbData);
        LocalFree(out_blob.pbData);
    }
    return root;
}

bool writeCredentialRoot(const QString& path, const QJsonObject& root, QString* error_message) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        setError(
            error_message,
            QStringLiteral("Could not write encrypted API key file: %1").arg(file.errorString()));
        return false;
    }
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size()) {
        setError(
            error_message,
            QStringLiteral("Short write to encrypted API key file: %1").arg(file.errorString()));
        return false;
    }
    if (!file.commit()) {
        setError(
            error_message,
            QStringLiteral("Could not commit encrypted API key file: %1").arg(file.errorString()));
        return false;
    }
    // Harden to owner-only; if that fails the file may be world-readable, so remove it and
    // fail closed rather than reporting success for an unprotected credential file.
    if (!QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner)) {
        QFile::remove(path);
        setError(error_message,
                 QStringLiteral("Could not restrict permissions on encrypted API key file: %1")
                     .arg(path));
        return false;
    }
    return true;
}
#endif

}  // namespace

bool CredentialStore::isPersistentStorageAvailable() const noexcept {
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

QString CredentialStore::credentialFilePath() {
    return QDir(credentialDirectory()).filePath(QStringLiteral("openai_api_key.dpapi.json"));
}

QString CredentialStore::loadApiKey(QString* error_message) const {
    if (error_message != nullptr) {
        error_message->clear();
    }

#ifdef Q_OS_WIN
    const auto root = readCredentialRoot(credentialFilePath(), error_message);
    if (!root.has_value()) {
        return {};
    }
    if (root->isEmpty()) {
        return {};
    }

    auto encrypted = encryptedCredentialBytes(*root, error_message);
    if (!encrypted.has_value()) {
        return {};
    }
    const QString api_key = decryptCredentialBytes(std::move(*encrypted), error_message);
    if (api_key.isEmpty()) {
        // A stored credential is never empty (saveApiKey rejects empty). An empty result here
        // means decryption failed (error already set) or the plaintext was empty/corrupt -- do
        // not hand back an empty key as if that were an error-free "not configured" state.
        if ((error_message != nullptr) && error_message->isEmpty()) {
            *error_message = QStringLiteral("Decrypted API key is empty or invalid");
        }
        return {};
    }
    return api_key;
#else
    setError(error_message,
             QStringLiteral("Encrypted persistent credential storage is not available"));
    return {};
#endif
}

bool CredentialStore::saveApiKey(const QString& api_key, QString* error_message) const {
    if (error_message != nullptr) {
        error_message->clear();
    }

    if (api_key.trimmed().isEmpty()) {
        setError(error_message, QStringLiteral("API key is empty"));
        return false;
    }
    if (api_key.size() > kMaxApiKeyChars) {
        setError(error_message, QStringLiteral("API key is implausibly long"));
        return false;
    }
    // Reject embedded control characters (NUL, CR, LF, tab, etc.): a real API key never contains
    // them, and letting one through could inject a newline into an outbound Authorization header
    // downstream. Fail closed rather than silently storing a malformed credential.
    if (std::ranges::any_of(api_key, [](const QChar ch) {
            return ch.unicode() < kAsciiControlLimit || ch.unicode() == kAsciiDelete;
        })) {
        setError(error_message, QStringLiteral("API key contains control characters"));
        return false;
    }

#ifdef Q_OS_WIN
    const QString path = credentialFilePath();
    if (!ensureCredentialDirectory(path, error_message)) {
        return false;
    }
    const auto root = protectedCredentialRoot(api_key, error_message);
    if (!root.has_value()) {
        return false;
    }
    return writeCredentialRoot(path, *root, error_message);
#else
    Q_UNUSED(api_key);
    setError(error_message,
             QStringLiteral("Encrypted persistent credential storage is not available"));
    return false;
#endif
}

bool CredentialStore::deleteApiKey(QString* error_message) const {
    if (error_message != nullptr) {
        error_message->clear();
    }

#ifdef Q_OS_WIN
    const QString path = credentialFilePath();
    if (!QFileInfo::exists(path)) {
        return true;
    }
    if (QFile::remove(path)) {
        return true;
    }
    if (error_message != nullptr) {
        *error_message = QStringLiteral("Could not remove encrypted API key file: %1").arg(path);
    }
    return false;
#else
    if (error_message) {
        *error_message = QStringLiteral("Encrypted persistent credential storage is not available");
    }
    return false;
#endif
}

QString CredentialStore::redactSecrets(const QString& text) {
    // The redaction rules live in their own dependency-free translation unit: they are pure text
    // work, while this class does DPAPI and file I/O. Callers that only need to scrub a string --
    // the chat-title generator, the conversation store -- must not have to link an encrypted
    // credential store to do it. This facade stays so existing callers keep working.
    return sak::ai::redactSecrets(text);
}

QJsonObject CredentialStore::redactSecretsInJson(const QJsonObject& object) {
    return sak::ai::redactSecretsInJson(object);
}
}  // namespace sak::ai

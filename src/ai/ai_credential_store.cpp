// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_credential_store.h"

#include "sak/ai/ai_paths.h"
#include "sak/ai/ai_secret_redaction.h"
#include "sak/permission_manager.h"
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
#include <QScopeGuard>

#include <algorithm>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include <dpapi.h>

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

constexpr auto kCredentialProvider = "dpapi-current-user-v1";
constexpr char kDpapiEntropy[] = "SAK Utility/OpenAI API Key/v1";
// The credential file holds one Base64 DPAPI blob wrapping a single API key; a few KiB
// at most. Cap the read so a corrupt/oversized file cannot be slurped whole into memory.
constexpr qint64 kMaxCredentialFileBytes = 256LL * 1024LL;
constexpr qsizetype kMaxDpapiPayloadBytes = 8LL * 1024LL * 1024LL;
constexpr qsizetype kMaxDpapiCiphertextBytes = kMaxDpapiPayloadBytes * 2;
constexpr qsizetype kMaxDpapiPurposeBytes = 512;
#ifdef SAK_AI_TEST_HOOKS
// Test-only wipe observers (SAK_AI_TEST_HOOKS): see the wiped buffer after the wipe.
std::function<void(const QByteArray&)> g_dpapi_plaintext_wipe_observer;
std::function<void(const QByteArray&)> g_credential_decrypt_input_wipe_observer;
#endif  // SAK_AI_TEST_HOOKS

[[nodiscard]] QString winErrorMessage(DWORD code) {
    return QStringLiteral("Windows error %1").arg(static_cast<qulonglong>(code));
}

void setError(QString* error_message, const QString& message) {
    if (error_message != nullptr) {
        *error_message = message;
    }
}

// The ONE rule for what an API key may look like, applied before a key is saved AND after one is
// decrypted. The load side used to skip it: any non-empty plaintext a DPAPI blob produced went
// straight into the outbound Authorization header, CR/LF included. Only a same-user writer can
// plant such a blob, so no trust boundary was crossed -- but a check that one side runs and the
// other does not is two definitions of a valid key. Returns the reason, or empty when valid.
[[nodiscard]] QString apiKeyTextError(const QString& api_key) {
    if (api_key.trimmed().isEmpty()) {
        return QStringLiteral("API key is empty");
    }
    if (api_key.size() > kMaxApiKeyChars) {
        return QStringLiteral("API key is implausibly long");
    }
    // Reject embedded control characters (NUL, CR, LF, tab, etc.): a real API key never contains
    // them, and letting one through could inject a newline into an outbound Authorization header
    // downstream. Fail closed rather than silently storing or using a malformed credential.
    if (std::ranges::any_of(api_key, [](const QChar ch) {
            return ch.unicode() < kAsciiControlLimit || ch.unicode() == kAsciiDelete;
        })) {
        return QStringLiteral("API key contains control characters");
    }
    return {};
}

// The credential file's root object, nullopt when NO FILE exists ("no key configured"), or the
// reason a file that does exist cannot be used.
//
// Absent and empty are different answers. Both used to come back as an empty object, so a key
// file truncated or overwritten to `{}` read as "no key configured" with no error at all, and the
// technician saw a missing key instead of a damaged one.
std::expected<std::optional<QJsonObject>, QString> readCredentialRoot(const QString& path) {
    QFile file(path);
    if (!file.exists()) {
        return std::optional<QJsonObject>{};
    }
    if (file.size() > kMaxCredentialFileBytes) {
        return std::unexpected(
            QStringLiteral("Encrypted API key file is implausibly large (%1 bytes)")
                .arg(file.size()));
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return std::unexpected(
            QStringLiteral("Could not read encrypted API key file: %1").arg(file.errorString()));
    }

    // Bound the read at the cap+1 so a file that grew (or was swapped) between the size check
    // above and here cannot force an unbounded allocation -- the pre-check is a fast reject, this
    // is the real ceiling.
    const QByteArray raw = file.read(kMaxCredentialFileBytes + 1);
    if (raw.size() > kMaxCredentialFileBytes) {
        return std::unexpected(QStringLiteral("Encrypted API key file is implausibly large"));
    }
    QJsonParseError parse_error{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        return std::unexpected(
            QStringLiteral("Encrypted API key file is invalid: %1").arg(parse_error.errorString()));
    }
    // Its own message: valid JSON whose root is an array or a scalar parses WITHOUT an error, so
    // sharing the parse branch reported "Encrypted API key file is invalid: no error occurred".
    if (!doc.isObject()) {
        return std::unexpected(
            QStringLiteral("Encrypted API key file is invalid: its JSON root is not an object"));
    }
    return std::optional<QJsonObject>(doc.object());
}

// The version this build writes and the only one it can read. The field was written and never
// checked, so a file from a future format with the same provider name would have been decrypted
// as this one.
constexpr int kCredentialFileVersion = 1;

[[nodiscard]] QString credentialRootError(const QJsonObject& root) {
    if (root.isEmpty()) {
        return QStringLiteral(
            "Encrypted API key file holds an empty JSON object; it was truncated or overwritten");
    }
    const QJsonValue version = root.value(QStringLiteral("version"));
    if (version.isUndefined()) {
        return QStringLiteral("Encrypted API key file has no version");
    }
    if (!version.isDouble()) {
        return QStringLiteral("Encrypted API key file version is not a number");
    }
    if (version.toInt(-1) != kCredentialFileVersion) {
        return QStringLiteral("Encrypted API key file version %1 is unsupported")
            .arg(version.toDouble());
    }
    if (root.value(QStringLiteral("provider")).toString() != QLatin1String(kCredentialProvider)) {
        return QStringLiteral("Encrypted API key provider is unsupported");
    }
    return {};
}

std::expected<QByteArray, QString> encryptedCredentialBytes(const QJsonObject& root) {
    const QString root_error = credentialRootError(root);
    if (!root_error.isEmpty()) {
        return std::unexpected(root_error);
    }
    // Strict Base64: reject any non-Base64 byte instead of silently discarding it, so a
    // corrupted ciphertext fails closed here rather than decoding to a different blob.
    const auto decoded = QByteArray::fromBase64Encoding(
        root.value(QStringLiteral("ciphertext")).toString().toLatin1(),
        QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) {
        return std::unexpected(QStringLiteral("Encrypted API key payload is not valid Base64"));
    }
    if (decoded.decoded.isEmpty()) {
        return std::unexpected(QStringLiteral("Encrypted API key payload is empty"));
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
    const auto wipe_inputs = qScopeGuard([&encrypted, &entropy]() {
        sak::secure_wiper::wipe(encrypted.data(), static_cast<std::size_t>(encrypted.size()));
        sak::secure_wiper::wipe(entropy.data(), static_cast<std::size_t>(entropy.size()));
#ifdef SAK_AI_TEST_HOOKS
        if (g_credential_decrypt_input_wipe_observer) {
            g_credential_decrypt_input_wipe_observer(encrypted);
            g_credential_decrypt_input_wipe_observer(entropy);
        }
#endif  // SAK_AI_TEST_HOOKS
    });

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
    root[QStringLiteral("version")] = kCredentialFileVersion;
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

// Whether the volume holding @p path stores ACLs at all: NTFS and ReFS do, FAT32 and exFAT do not.
std::expected<bool, QString> volumeStoresAcls(const QString& path) {
    const std::wstring native =
        QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()).toStdWString();
    // The volume root is never longer than the path it was resolved from, plus a trailing
    // separator and the terminating NUL.
    constexpr std::size_t kVolumeRootExtraChars = 2;
    std::vector<wchar_t> volume_root(native.size() + kVolumeRootExtraChars, L'\0');
    if (GetVolumePathNameW(
            native.c_str(), volume_root.data(), static_cast<DWORD>(volume_root.size())) == 0) {
        return std::unexpected(QStringLiteral("could not resolve the volume of %1 (%2)")
                                   .arg(path, winErrorMessage(GetLastError())));
    }
    DWORD flags = 0;
    if (GetVolumeInformationW(
            volume_root.data(), nullptr, 0, nullptr, nullptr, &flags, nullptr, 0) == 0) {
        return std::unexpected(
            QStringLiteral("could not read the file system flags of %1 (%2)")
                .arg(QString::fromWCharArray(volume_root.data()), winErrorMessage(GetLastError())));
    }
    return (flags & FILE_PERSISTENT_ACLS) != 0;
}

// A portable copy run from a FAT32 or exFAT stick has no ACL to set, because those file systems
// store none; DPAPI still binds the ciphertext to this Windows user, and that is the protection
// there. Refusing to save on such a volume would remove the feature without protecting anything.
std::expected<void, QString> restrictCredentialFile(const QString& path) {
    const auto stores_acls = volumeStoresAcls(path);
    if (!stores_acls) {
        return std::unexpected(stores_acls.error());
    }
    if (!*stores_acls) {
        return {};
    }
    return sak::restrictFileToCurrentUser(path);
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
    // Restrict the file to this user (plus SYSTEM and Administrators) with a protected DACL, and
    // fail closed -- removing the file -- when that cannot be done.
    //
    // This used to be QFile::setPermissions(ReadOwner | WriteOwner), which on Windows only toggles
    // the read-only attribute: the file kept the data folder's inherited DACL, the call returned
    // true, and its fail-closed branch could never run. The content was protected by DPAPI alone
    // while the comment claimed owner-only access.
    const auto restricted = restrictCredentialFile(path);
    if (!restricted) {
        const bool removed = QFile::remove(path);
        setError(error_message,
                 QStringLiteral("Could not restrict access to the encrypted API key file (%1); %2")
                     .arg(restricted.error(),
                          removed ? QStringLiteral("the file was removed")
                                  : QStringLiteral("the file could NOT be removed: %1").arg(path)));
        return false;
    }
    return true;
}

QString loadApiKeyFrom(const QString& path, QString* error_message) {
    const auto root = readCredentialRoot(path);
    if (!root) {
        setError(error_message, root.error());
        return {};
    }
    if (!root->has_value()) {
        return {};  // no file: no key configured, which is not an error
    }
    auto encrypted = encryptedCredentialBytes(**root);
    if (!encrypted) {
        setError(error_message, encrypted.error());
        return {};
    }
    QString decrypt_error;
    const QString api_key = decryptCredentialBytes(std::move(*encrypted), &decrypt_error);
    if (!decrypt_error.isEmpty()) {
        setError(error_message, decrypt_error);
        return {};
    }
    // A stored credential passed apiKeyTextError when it was saved; one that fails it now was not
    // written by this store, and it goes no further -- in particular not into a request header.
    const QString key_error = apiKeyTextError(api_key);
    if (!key_error.isEmpty()) {
        setError(error_message, QStringLiteral("Stored API key is invalid: %1").arg(key_error));
        return {};
    }
    return api_key;
}

bool saveApiKeyTo(const QString& path, const QString& api_key, QString* error_message) {
    const QString key_error = apiKeyTextError(api_key);
    if (!key_error.isEmpty()) {
        setError(error_message, key_error);
        return false;
    }
    if (!ensureCredentialDirectory(path, error_message)) {
        return false;
    }
    const auto root = protectedCredentialRoot(api_key, error_message);
    if (!root.has_value()) {
        return false;
    }
    return writeCredentialRoot(path, *root, error_message);
}
}  // namespace

bool CredentialStore::isPersistentStorageAvailable() noexcept {
    return true;
}

QString CredentialStore::credentialFilePath() {
    return QDir(credentialDirectory()).filePath(QStringLiteral("openai_api_key.dpapi.json"));
}

std::expected<QByteArray, QString> CredentialStore::protectCurrentUserData(
    const QByteArray& plaintext, const QByteArray& purpose) {
    if (plaintext.isEmpty() || plaintext.size() > kMaxDpapiPayloadBytes) {
        return std::unexpected(QStringLiteral("DPAPI plaintext is empty or exceeds its limit"));
    }
    if (purpose.isEmpty() || purpose.size() > kMaxDpapiPurposeBytes) {
        return std::unexpected(QStringLiteral("DPAPI purpose is empty or exceeds its limit"));
    }
    QByteArray plaintext_copy = plaintext;
    const auto wipe_plaintext = qScopeGuard([&plaintext_copy]() {
        sak::secure_wiper::wipe(plaintext_copy.data(),
                                static_cast<std::size_t>(plaintext_copy.size()));
#ifdef SAK_AI_TEST_HOOKS
        if (g_dpapi_plaintext_wipe_observer) {
            g_dpapi_plaintext_wipe_observer(plaintext_copy);
        }
#endif  // SAK_AI_TEST_HOOKS
    });
    QByteArray purpose_copy = purpose;
    DATA_BLOB input{static_cast<DWORD>(plaintext_copy.size()),
                    reinterpret_cast<BYTE*>(plaintext_copy.data())};
    DATA_BLOB entropy{static_cast<DWORD>(purpose_copy.size()),
                      reinterpret_cast<BYTE*>(purpose_copy.data())};
    DATA_BLOB output{};
    const BOOL protected_ok = CryptProtectData(&input,
                                               L"SAK Utility authenticated local state",
                                               &entropy,
                                               nullptr,
                                               nullptr,
                                               CRYPTPROTECT_UI_FORBIDDEN,
                                               &output);
    if (protected_ok == 0) {
        return std::unexpected(
            QStringLiteral("DPAPI protection failed: %1").arg(winErrorMessage(GetLastError())));
    }
    const qsizetype output_size = static_cast<qsizetype>(output.cbData);
    if (output_size <= 0 || output_size > kMaxDpapiCiphertextBytes) {
        SecureZeroMemory(output.pbData, output.cbData);
        LocalFree(output.pbData);
        return std::unexpected(QStringLiteral("DPAPI ciphertext has an invalid size"));
    }
    QByteArray ciphertext(reinterpret_cast<const char*>(output.pbData),
                          static_cast<int>(output.cbData));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return ciphertext;
}

std::expected<QByteArray, QString> CredentialStore::unprotectCurrentUserData(
    const QByteArray& ciphertext, const QByteArray& purpose) {
    if (ciphertext.isEmpty() || ciphertext.size() > kMaxDpapiCiphertextBytes) {
        return std::unexpected(QStringLiteral("DPAPI ciphertext is empty or exceeds its limit"));
    }
    if (purpose.isEmpty() || purpose.size() > kMaxDpapiPurposeBytes) {
        return std::unexpected(QStringLiteral("DPAPI purpose is empty or exceeds its limit"));
    }
    QByteArray ciphertext_copy = ciphertext;
    QByteArray purpose_copy = purpose;
    DATA_BLOB input{static_cast<DWORD>(ciphertext_copy.size()),
                    reinterpret_cast<BYTE*>(ciphertext_copy.data())};
    DATA_BLOB entropy{static_cast<DWORD>(purpose_copy.size()),
                      reinterpret_cast<BYTE*>(purpose_copy.data())};
    DATA_BLOB output{};
    const BOOL unprotected_ok = CryptUnprotectData(
        &input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
    if (unprotected_ok == 0) {
        return std::unexpected(
            QStringLiteral("DPAPI authentication failed: %1").arg(winErrorMessage(GetLastError())));
    }
    const qsizetype output_size = static_cast<qsizetype>(output.cbData);
    if (output_size <= 0 || output_size > kMaxDpapiPayloadBytes) {
        SecureZeroMemory(output.pbData, output.cbData);
        LocalFree(output.pbData);
        return std::unexpected(QStringLiteral("DPAPI plaintext has an invalid size"));
    }
    QByteArray plaintext(reinterpret_cast<const char*>(output.pbData),
                         static_cast<int>(output.cbData));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return plaintext;
}

#ifdef SAK_AI_TEST_HOOKS
void CredentialStore::setDpapiPlaintextWipeObserverForTesting(
    std::function<void(const QByteArray&)> observer) {
    g_dpapi_plaintext_wipe_observer = std::move(observer);
}

void CredentialStore::setCredentialDecryptInputWipeObserverForTesting(
    std::function<void(const QByteArray&)> observer) {
    g_credential_decrypt_input_wipe_observer = std::move(observer);
}

QString CredentialStore::decryptCredentialBytesForTesting(QByteArray encrypted,
                                                          QString* error_message) {
    return decryptCredentialBytes(std::move(encrypted), error_message);
}

QString CredentialStore::loadApiKeyFromPathForTesting(const QString& path, QString* error_message) {
    if (error_message != nullptr) {
        error_message->clear();
    }
    return loadApiKeyFrom(path, error_message);
}

bool CredentialStore::saveApiKeyToPathForTesting(const QString& path,
                                                 const QString& api_key,
                                                 QString* error_message) {
    if (error_message != nullptr) {
        error_message->clear();
    }
    return saveApiKeyTo(path, api_key, error_message);
}
#endif  // SAK_AI_TEST_HOOKS

QString CredentialStore::loadApiKey(QString* error_message) {
    if (error_message != nullptr) {
        error_message->clear();
    }
    return loadApiKeyFrom(credentialFilePath(), error_message);
}

bool CredentialStore::saveApiKey(const QString& api_key, QString* error_message) {
    if (error_message != nullptr) {
        error_message->clear();
    }
    return saveApiKeyTo(credentialFilePath(), api_key, error_message);
}

bool CredentialStore::deleteApiKey(QString* error_message) {
    if (error_message != nullptr) {
        error_message->clear();
    }

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

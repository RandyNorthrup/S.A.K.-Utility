// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_credential_store.h"

#include "sak/ai/ai_paths.h"
#include "sak/secure_memory.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QRegularExpressionMatchIterator>
#include <QSaveFile>

#include <optional>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>

#include <dpapi.h>
#endif

namespace sak::ai {

namespace {

#ifdef Q_OS_WIN
constexpr auto kCredentialProvider = "dpapi-current-user-v1";
constexpr char kDpapiEntropy[] = "SAK Utility/OpenAI API Key/v1";

[[nodiscard]] QString winErrorMessage(DWORD code) {
    return QStringLiteral("Windows error %1").arg(static_cast<qulonglong>(code));
}
#endif

void setError(QString* error_message, const QString& message) {
    if (error_message) {
        *error_message = message;
    }
}

#ifdef Q_OS_WIN
std::optional<QJsonObject> readCredentialRoot(const QString& path, QString* error_message) {
    QFile file(path);
    if (!file.exists()) {
        return QJsonObject{};
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setError(
            error_message,
            QStringLiteral("Could not read encrypted API key file: %1").arg(file.errorString()));
        return std::nullopt;
    }

    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse_error);
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
    QByteArray encrypted =
        QByteArray::fromBase64(root.value(QStringLiteral("ciphertext")).toString().toLatin1());
    if (encrypted.isEmpty()) {
        setError(error_message, QStringLiteral("Encrypted API key payload is empty"));
        return std::nullopt;
    }
    return encrypted;
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
    if (!CryptUnprotectData(&in_blob,
                            nullptr,
                            &entropy_blob,
                            nullptr,
                            nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN,
                            &out_blob)) {
        setError(error_message,
                 QStringLiteral("Could not decrypt encrypted API key file: %1")
                     .arg(winErrorMessage(GetLastError())));
        return {};
    }

    QByteArray bytes(reinterpret_cast<const char*>(out_blob.pbData),
                     static_cast<int>(out_blob.cbData));
    const QString api_key = QString::fromUtf8(bytes);
    sak::secure_wiper::wipe(bytes.data(), static_cast<std::size_t>(bytes.size()));
    if (out_blob.pbData) {
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
    if (!protected_ok) {
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

    if (out_blob.pbData) {
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
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        setError(
            error_message,
            QStringLiteral("Could not commit encrypted API key file: %1").arg(file.errorString()));
        return false;
    }
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
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

QString CredentialStore::credentialFilePath() const {
    return QDir(credentialDirectory()).filePath(QStringLiteral("openai_api_key.dpapi.json"));
}

QString CredentialStore::loadApiKey(QString* error_message) const {
    if (error_message) {
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
    return decryptCredentialBytes(std::move(*encrypted), error_message);
#else
    setError(error_message,
             QStringLiteral("Encrypted persistent credential storage is not available"));
    return {};
#endif
}

bool CredentialStore::saveApiKey(const QString& api_key, QString* error_message) const {
    if (error_message) {
        error_message->clear();
    }

    if (api_key.trimmed().isEmpty()) {
        setError(error_message, QStringLiteral("API key is empty"));
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
    if (error_message) {
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
    if (error_message) {
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
    QString result = text;

    static const QRegularExpression kOpenAiKey(QStringLiteral(R"(\bsk-[A-Za-z0-9_\-]{12,}\b)"),
                                               QRegularExpression::UseUnicodePropertiesOption);
    result.replace(kOpenAiKey, QStringLiteral("sk-...[redacted]"));

    static const QRegularExpression kBearer(QStringLiteral(R"((Bearer\s+)[A-Za-z0-9_\-\.]{12,})"),
                                            QRegularExpression::CaseInsensitiveOption);
    result.replace(kBearer, QStringLiteral("\\1[redacted]"));

    // GitHub personal/oauth/server/app/refresh tokens (ghp_, gho_, ghu_, ghs_, ghr_)
    static const QRegularExpression kGitHubToken(
        QStringLiteral(R"(\bgh[pousr]_[A-Za-z0-9]{20,}\b)"));
    result.replace(kGitHubToken, QStringLiteral("[redacted-github-token]"));

    // AWS access key IDs (AKIA/ASIA + 16 alnum) and Google API keys (AIza...)
    static const QRegularExpression kAwsAccessKey(
        QStringLiteral(R"(\b(?:AKIA|ASIA)[A-Z0-9]{16}\b)"));
    result.replace(kAwsAccessKey, QStringLiteral("[redacted-aws-key]"));
    static const QRegularExpression kGoogleApiKey(QStringLiteral(R"(\bAIza[A-Za-z0-9_\-]{35}\b)"));
    result.replace(kGoogleApiKey, QStringLiteral("[redacted-google-key]"));

    // Slack tokens (xox[bopas]-...) and Stripe keys (sk_live_..., rk_live_...)
    static const QRegularExpression kSlackToken(
        QStringLiteral(R"(\bxox[boapsr]-[A-Za-z0-9\-]{12,}\b)"));
    result.replace(kSlackToken, QStringLiteral("[redacted-slack-token]"));
    static const QRegularExpression kStripeKey(
        QStringLiteral(R"(\b(?:sk|rk)_(?:live|test)_[A-Za-z0-9]{16,}\b)"));
    result.replace(kStripeKey, QStringLiteral("[redacted-stripe-key]"));

    // Generic "password=", "passwd=", "secret=", "token=", "api[_-]?key=" values.
    static const QRegularExpression kAssignmentSecret(QStringLiteral(
        R"RX((?i)\b(password|passwd|secret|token|api[_\-]?key)\s*[:=]\s*"?([^\s"';,]{4,})"?)RX"));
    QRegularExpressionMatchIterator it = kAssignmentSecret.globalMatch(result);
    QString rewritten;
    rewritten.reserve(result.size());
    int last_end = 0;
    while (it.hasNext()) {
        const auto match = it.next();
        rewritten.append(result.mid(last_end, match.capturedStart(0) - last_end));
        rewritten.append(match.captured(1));
        rewritten.append(QStringLiteral("=[redacted]"));
        last_end = match.capturedEnd(0);
    }
    rewritten.append(result.mid(last_end));
    if (!rewritten.isEmpty() || last_end > 0) {
        result = rewritten;
    }

    return result;
}

}  // namespace sak::ai

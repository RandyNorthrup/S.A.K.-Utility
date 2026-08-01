// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file encryption.cpp
/// @brief AES-256 encryption implementation using Windows BCrypt API

#include "sak/encryption.h"

#include "sak/logger.h"
#include "sak/secure_memory.h"

#include <QCryptographicHash>
#include <QFile>

#include <optional>

#ifdef _WIN32
#include <windows.h>

#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace sak {

namespace {

#ifdef _WIN32
/// @brief Generate cryptographic random bytes
QByteArray generate_random_bytes(int size) {
    Q_ASSERT(size >= 0);
    if (size <= 0) {
        sak::logWarning("generate_random_bytes called with non-positive size {}", size);
        return {};
    }
    QByteArray result(size, 0);
    BCRYPT_ALG_HANDLE hAlg = nullptr;

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_RNG_ALGORITHM, nullptr, 0) != 0) {
        sak::logError("BCrypt: Failed to open RNG algorithm provider");
        return {};
    }

    if (BCryptGenRandom(
            hAlg, reinterpret_cast<PUCHAR>(result.data()), static_cast<ULONG>(size), 0) != 0) {
        sak::logError("BCrypt: Failed to generate {} random bytes", size);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }

    BCryptCloseAlgorithmProvider(hAlg, 0);
    return result;
}

/// @brief Derive encryption key from password using PBKDF2
QByteArray derive_key(const QString& password,
                      const QByteArray& salt,
                      int iterations,
                      int key_length) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;

    if (BCryptOpenAlgorithmProvider(
            &hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
        sak::logError("BCrypt: Failed to open SHA-256 HMAC algorithm provider for key derivation");
        return {};
    }

    QByteArray pwd_bytes = password.toUtf8();
    QByteArray derived_key(key_length, 0);

    NTSTATUS status =
        BCryptDeriveKeyPBKDF2(hAlg,
                              reinterpret_cast<PUCHAR>(pwd_bytes.data()),
                              pwd_bytes.size(),
                              reinterpret_cast<PUCHAR>(const_cast<char*>(salt.data())),
                              salt.size(),
                              iterations,
                              reinterpret_cast<PUCHAR>(derived_key.data()),
                              key_length,
                              0);

    BCryptCloseAlgorithmProvider(hAlg, 0);

    if (status != 0) {
        sak::logError("BCrypt: PBKDF2 key derivation failed (NTSTATUS 0x{:08X})",
                      static_cast<unsigned long>(status));
        sak::secure_wiper::wipe(pwd_bytes.data(), pwd_bytes.size());
        sak::secure_wiper::wipe(derived_key.data(), derived_key.size());
        return {};
    }

    sak::secure_wiper::wipe(pwd_bytes.data(), pwd_bytes.size());
    return derived_key;
}

bool initialize_aes_key(BCRYPT_ALG_HANDLE& hAlg,
                        BCRYPT_KEY_HANDLE& hKey,
                        const QByteArray& key,
                        const char* operation) {
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) {
        sak::logError("BCrypt: Failed to open AES algorithm provider for {}", operation);
        return false;
    }

    if (BCryptSetProperty(hAlg,
                          BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                          sizeof(BCRYPT_CHAIN_MODE_CBC),
                          0) != 0) {
        sak::logError("BCrypt: Failed to set CBC chaining mode for {}", operation);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        hAlg = nullptr;
        return false;
    }

    if (BCryptGenerateSymmetricKey(hAlg,
                                   &hKey,
                                   nullptr,
                                   0,
                                   reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
                                   key.size(),
                                   0) != 0) {
        sak::logError("BCrypt: Failed to generate symmetric key for {}", operation);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        hAlg = nullptr;
        return false;
    }

    return true;
}

void destroy_aes_key(BCRYPT_ALG_HANDLE hAlg, BCRYPT_KEY_HANDLE hKey) {
    if (hKey) {
        BCryptDestroyKey(hKey);
    }
    if (hAlg) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
    }
}

/// @brief Compute an HMAC-SHA256 authentication tag over a message
QByteArray compute_hmac(const QByteArray& mac_key, const QByteArray& message) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(
            &hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
        sak::logError("BCrypt: Failed to open SHA-256 HMAC provider for authentication");
        return {};
    }
    QByteArray tag(kEncryptionMacBytes, 0);
    NTSTATUS status = BCryptHash(hAlg,
                                 reinterpret_cast<PUCHAR>(const_cast<char*>(mac_key.data())),
                                 static_cast<ULONG>(mac_key.size()),
                                 reinterpret_cast<PUCHAR>(const_cast<char*>(message.data())),
                                 static_cast<ULONG>(message.size()),
                                 reinterpret_cast<PUCHAR>(tag.data()),
                                 static_cast<ULONG>(tag.size()));
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (status != 0) {
        sak::logError("BCrypt: HMAC-SHA256 computation failed");
        return {};
    }
    return tag;
}

/// @brief Constant-time byte comparison to avoid tag-verification timing leaks
bool constant_time_equal(const QByteArray& a, const QByteArray& b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

/// @brief Reject a hostile or default-broken EncryptionParams before any crypto runs: a
/// non-AES key size, an IV that is not one AES block, a too-small salt, or a non-positive
/// iteration count would otherwise derive weak keys or drive BCrypt into undefined behavior.
bool valid_encryption_params(const EncryptionParams& params) {
    const bool aes_key = params.key_size == 16 || params.key_size == 24 || params.key_size == 32;
    return aes_key && params.iv_size == kAesBlockBytes && params.salt_size >= 8 &&
           params.iterations > 0;
}

/// @brief Derive separate AES and HMAC keys from a password (encrypt-then-MAC)
bool derive_enc_and_mac_keys(const QString& password,
                             const QByteArray& salt,
                             const EncryptionParams& params,
                             QByteArray& enc_key,
                             QByteArray& mac_key) {
    QByteArray combined =
        derive_key(password, salt, params.iterations, params.key_size + kEncryptionMacKeyBytes);
    if (combined.isEmpty()) {
        return false;
    }
    enc_key = combined.left(params.key_size);
    mac_key = combined.mid(params.key_size);
    sak::secure_wiper::wipe(combined.data(), combined.size());
    return true;
}

/// @brief AES-256-CBC encryption
QByteArray aes_encrypt(const QByteArray& plaintext, const QByteArray& key, const QByteArray& iv) {
    // An empty plaintext is valid: CBC block padding turns it into one full padding block, which
    // decrypts back to empty. Only a missing key is a hard error here.
    if (key.isEmpty()) {
        return {};
    }
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;

    if (!initialize_aes_key(hAlg, hKey, key, "encryption")) {
        return {};
    }

    DWORD ciphertext_len = 0;
    QByteArray iv_copy = iv;  // BCrypt modifies IV

    if (BCryptEncrypt(hKey,
                      reinterpret_cast<PUCHAR>(const_cast<char*>(plaintext.data())),
                      plaintext.size(),
                      nullptr,
                      reinterpret_cast<PUCHAR>(iv_copy.data()),
                      iv_copy.size(),
                      nullptr,
                      0,
                      &ciphertext_len,
                      BCRYPT_BLOCK_PADDING) != 0) {
        sak::logError("BCrypt: Failed to calculate encrypted output size ({} bytes input)",
                      plaintext.size());
        destroy_aes_key(hAlg, hKey);
        return {};
    }

    QByteArray ciphertext(ciphertext_len, 0);
    iv_copy = iv;  // Reset IV

    if (BCryptEncrypt(hKey,
                      reinterpret_cast<PUCHAR>(const_cast<char*>(plaintext.data())),
                      plaintext.size(),
                      nullptr,
                      reinterpret_cast<PUCHAR>(iv_copy.data()),
                      iv_copy.size(),
                      reinterpret_cast<PUCHAR>(ciphertext.data()),
                      ciphertext_len,
                      &ciphertext_len,
                      BCRYPT_BLOCK_PADDING) != 0) {
        sak::logError("BCrypt: AES-256-CBC encryption failed ({} bytes input)", plaintext.size());
        destroy_aes_key(hAlg, hKey);
        return {};
    }

    ciphertext.resize(ciphertext_len);
    destroy_aes_key(hAlg, hKey);

    return ciphertext;
}

/// @brief AES-256-CBC decryption. Returns nullopt on failure; a present-but-empty result is a
/// legitimately empty plaintext (the inverse of encrypting empty data), which the QByteArray
/// return type could not distinguish from an error.
std::optional<QByteArray> aes_decrypt(const QByteArray& ciphertext,
                                      const QByteArray& key,
                                      const QByteArray& iv) {
    if (ciphertext.isEmpty() || key.isEmpty()) {
        return std::nullopt;
    }
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;

    if (!initialize_aes_key(hAlg, hKey, key, "decryption")) {
        return std::nullopt;
    }

    DWORD plaintext_len = 0;
    QByteArray iv_copy = iv;  // BCrypt modifies IV

    if (BCryptDecrypt(hKey,
                      reinterpret_cast<PUCHAR>(const_cast<char*>(ciphertext.data())),
                      ciphertext.size(),
                      nullptr,
                      reinterpret_cast<PUCHAR>(iv_copy.data()),
                      iv_copy.size(),
                      nullptr,
                      0,
                      &plaintext_len,
                      BCRYPT_BLOCK_PADDING) != 0) {
        sak::logError("BCrypt: Failed to calculate decrypted output size ({} bytes input)",
                      ciphertext.size());
        destroy_aes_key(hAlg, hKey);
        return {};
    }

    QByteArray plaintext(plaintext_len, 0);
    iv_copy = iv;  // Reset IV

    if (BCryptDecrypt(hKey,
                      reinterpret_cast<PUCHAR>(const_cast<char*>(ciphertext.data())),
                      ciphertext.size(),
                      nullptr,
                      reinterpret_cast<PUCHAR>(iv_copy.data()),
                      iv_copy.size(),
                      reinterpret_cast<PUCHAR>(plaintext.data()),
                      plaintext_len,
                      &plaintext_len,
                      BCRYPT_BLOCK_PADDING) != 0) {
        sak::logError("BCrypt: AES-256-CBC decryption failed ({} bytes input)", ciphertext.size());
        destroy_aes_key(hAlg, hKey);
        return {};
    }

    plaintext.resize(plaintext_len);
    destroy_aes_key(hAlg, hKey);

    return plaintext;
}
#endif

}  // anonymous namespace

auto encryptData(const QByteArray& data, const QString& password, const EncryptionParams& params)
    -> std::expected<QByteArray, error_code> {
#ifdef _WIN32
    if (password.isEmpty() || !valid_encryption_params(params)) {
        return std::unexpected(error_code::invalid_argument);
    }

    // Generate salt and IV
    QByteArray salt = generate_random_bytes(params.salt_size);
    QByteArray iv = generate_random_bytes(params.iv_size);

    if (salt.isEmpty() || iv.isEmpty()) {
        logError("Failed to generate random bytes for encryption");
        return std::unexpected(error_code::crypto_error);
    }

    // Derive separate AES and HMAC keys (encrypt-then-MAC)
    QByteArray enc_key;
    QByteArray mac_key;
    if (!derive_enc_and_mac_keys(password, salt, params, enc_key, mac_key)) {
        logError("Failed to derive encryption key");
        return std::unexpected(error_code::crypto_error);
    }

    QByteArray ciphertext = aes_encrypt(data, enc_key, iv);
    secure_wiper::wipe(enc_key.data(), enc_key.size());
    if (ciphertext.isEmpty()) {
        secure_wiper::wipe(mac_key.data(), mac_key.size());
        logError("AES encryption failed");
        return std::unexpected(error_code::crypto_error);
    }

    // Format: [salt][IV][ciphertext][HMAC tag]
    QByteArray result;
    result.append(salt);
    result.append(iv);
    result.append(ciphertext);

    QByteArray tag = compute_hmac(mac_key, result);
    secure_wiper::wipe(mac_key.data(), mac_key.size());
    if (tag.isEmpty()) {
        logError("Failed to authenticate encrypted data");
        return std::unexpected(error_code::crypto_error);
    }
    result.append(tag);

    logDebug("Encryption",
             std::format("Encrypted {} bytes to {} bytes", data.size(), result.size()));

    return result;
#else
    return std::unexpected(error_code::not_implemented);
#endif
}

auto decryptData(const QByteArray& encrypted_data,
                 const QString& password,
                 const EncryptionParams& params) -> std::expected<QByteArray, error_code> {
#ifdef _WIN32
    if (password.isEmpty() || !valid_encryption_params(params)) {
        return std::unexpected(error_code::invalid_argument);
    }

    int header_size = params.salt_size + params.iv_size;
    if (encrypted_data.size() < header_size + kEncryptionMacBytes) {
        logError("Encrypted data too small - corrupted or invalid");
        return std::unexpected(error_code::invalid_format);
    }

    // Split off the trailing HMAC tag; the message it authenticates is [salt][IV][ciphertext].
    int message_size = encrypted_data.size() - kEncryptionMacBytes;
    QByteArray message = encrypted_data.left(message_size);
    QByteArray stored_tag = encrypted_data.mid(message_size);

    QByteArray salt = message.left(params.salt_size);
    QByteArray iv = message.mid(params.salt_size, params.iv_size);
    QByteArray ciphertext = message.mid(header_size);

    QByteArray enc_key;
    QByteArray mac_key;
    if (!derive_enc_and_mac_keys(password, salt, params, enc_key, mac_key)) {
        logError("Failed to derive decryption key");
        return std::unexpected(error_code::crypto_error);
    }

    // Verify the tag in constant time BEFORE decrypting: a tamper or wrong password fails here,
    // never reaching BCryptDecrypt (no padding oracle).
    QByteArray tag = compute_hmac(mac_key, message);
    secure_wiper::wipe(mac_key.data(), mac_key.size());
    if (tag.isEmpty() || !constant_time_equal(tag, stored_tag)) {
        secure_wiper::wipe(enc_key.data(), enc_key.size());
        logError("Authentication failed - data tampered or wrong password");
        return std::unexpected(error_code::decrypt_failed);
    }

    // The HMAC above already proved authenticity, so reaching here means the data is genuine.
    // aes_decrypt returns nullopt only on a BCrypt error; a present-but-empty plaintext is the
    // valid inverse of encrypting empty data and must round-trip, not be treated as a failure.
    std::optional<QByteArray> plaintext = aes_decrypt(ciphertext, enc_key, iv);
    secure_wiper::wipe(enc_key.data(), enc_key.size());
    if (!plaintext.has_value()) {
        logError("AES decryption failed - wrong password or corrupted data");
        return std::unexpected(error_code::decrypt_failed);
    }

    logDebug(
        "Decryption",
        std::format("Decrypted {} bytes to {} bytes", encrypted_data.size(), plaintext->size()));

    return *plaintext;
#else
    return std::unexpected(error_code::not_implemented);
#endif
}

}  // namespace sak

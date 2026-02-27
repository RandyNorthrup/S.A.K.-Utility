// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file encryption.cpp
/// @brief AES-256 encryption implementation using Windows BCrypt API

#include "sak/encryption.h"
#include "sak/logger.h"
#include "sak/secure_memory.h"
#include <QFile>
#include <QCryptographicHash>

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
    QByteArray result(size, 0);
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_RNG_ALGORITHM, nullptr, 0) != 0) {
        sak::logError("BCrypt: Failed to open RNG algorithm provider");
        return {};
    }
    
    if (BCryptGenRandom(hAlg, reinterpret_cast<PUCHAR>(result.data()), size, 0) != 0) {
        sak::logError("BCrypt: Failed to generate {} random bytes", size);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return result;
}

/// @brief Derive encryption key from password using PBKDF2
QByteArray derive_key(const QString& password, const QByteArray& salt, int iterations, int key_length) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
        sak::logError("BCrypt: Failed to open SHA-256 HMAC algorithm provider for key derivation");
        return {};
    }
    
    QByteArray pwd_bytes = password.toUtf8();
    QByteArray derived_key(key_length, 0);
    
    NTSTATUS status = BCryptDeriveKeyPBKDF2(
        hAlg,
        reinterpret_cast<PUCHAR>(pwd_bytes.data()), pwd_bytes.size(),
        reinterpret_cast<PUCHAR>(const_cast<char*>(salt.data())), salt.size(),
        iterations,
        reinterpret_cast<PUCHAR>(derived_key.data()), key_length,
        0
    );
    
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

/// @brief AES-256-CBC encryption
QByteArray aes_encrypt(const QByteArray& plaintext, const QByteArray& key, const QByteArray& iv) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) {
        sak::logError("BCrypt: Failed to open AES algorithm provider for encryption");
        return {};
    }
    
    if (BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE, 
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)), 
                          sizeof(BCRYPT_CHAIN_MODE_CBC), 0) != 0) {
        sak::logError("BCrypt: Failed to set CBC chaining mode for encryption");
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    
    if (BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0, 
                                    reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())), 
                                    key.size(), 0) != 0) {
        sak::logError("BCrypt: Failed to generate symmetric key for encryption");
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    
    DWORD ciphertext_len = 0;
    QByteArray iv_copy = iv; // BCrypt modifies IV
    
    if (BCryptEncrypt(hKey, reinterpret_cast<PUCHAR>(const_cast<char*>(plaintext.data())), 
                      plaintext.size(), nullptr, 
                      reinterpret_cast<PUCHAR>(iv_copy.data()), iv_copy.size(),
                      nullptr, 0, &ciphertext_len, BCRYPT_BLOCK_PADDING) != 0) {
        sak::logError("BCrypt: Failed to calculate encrypted output size ({} bytes input)", plaintext.size());
        BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    
    QByteArray ciphertext(ciphertext_len, 0);
    iv_copy = iv; // Reset IV
    
    if (BCryptEncrypt(hKey, reinterpret_cast<PUCHAR>(const_cast<char*>(plaintext.data())), 
                      plaintext.size(), nullptr,
                      reinterpret_cast<PUCHAR>(iv_copy.data()), iv_copy.size(),
                      reinterpret_cast<PUCHAR>(ciphertext.data()), ciphertext_len, 
                      &ciphertext_len, BCRYPT_BLOCK_PADDING) != 0) {
        sak::logError("BCrypt: AES-256-CBC encryption failed ({} bytes input)", plaintext.size());
        BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    
    ciphertext.resize(ciphertext_len);
    
    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    
    return ciphertext;
}

/// @brief AES-256-CBC decryption
QByteArray aes_decrypt(const QByteArray& ciphertext, const QByteArray& key, const QByteArray& iv) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) {
        sak::logError("BCrypt: Failed to open AES algorithm provider for decryption");
        return {};
    }
    
    if (BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                          sizeof(BCRYPT_CHAIN_MODE_CBC), 0) != 0) {
        sak::logError("BCrypt: Failed to set CBC chaining mode for decryption");
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    
    if (BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0,
                                    reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
                                    key.size(), 0) != 0) {
        sak::logError("BCrypt: Failed to generate symmetric key for decryption");
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    
    DWORD plaintext_len = 0;
    QByteArray iv_copy = iv; // BCrypt modifies IV
    
    if (BCryptDecrypt(hKey, reinterpret_cast<PUCHAR>(const_cast<char*>(ciphertext.data())),
                      ciphertext.size(), nullptr,
                      reinterpret_cast<PUCHAR>(iv_copy.data()), iv_copy.size(),
                      nullptr, 0, &plaintext_len, BCRYPT_BLOCK_PADDING) != 0) {
        sak::logError("BCrypt: Failed to calculate decrypted output size ({} bytes input)", ciphertext.size());
        BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    
    QByteArray plaintext(plaintext_len, 0);
    iv_copy = iv; // Reset IV
    
    if (BCryptDecrypt(hKey, reinterpret_cast<PUCHAR>(const_cast<char*>(ciphertext.data())),
                      ciphertext.size(), nullptr,
                      reinterpret_cast<PUCHAR>(iv_copy.data()), iv_copy.size(),
                      reinterpret_cast<PUCHAR>(plaintext.data()), plaintext_len,
                      &plaintext_len, BCRYPT_BLOCK_PADDING) != 0) {
        sak::logError("BCrypt: AES-256-CBC decryption failed ({} bytes input)", ciphertext.size());
        BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    
    plaintext.resize(plaintext_len);
    
    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    
    return plaintext;
}
#endif

} // anonymous namespace

auto encryptData(
    const QByteArray& data,
    const QString& password,
    const EncryptionParams& params
) -> std::expected<QByteArray, error_code> {
#ifdef _WIN32
    if (password.isEmpty()) {
        return std::unexpected(error_code::invalid_argument);
    }
    
    // Generate salt and IV
    QByteArray salt = generate_random_bytes(params.salt_size);
    QByteArray iv = generate_random_bytes(params.iv_size);
    
    if (salt.isEmpty() || iv.isEmpty()) {
        logError("Failed to generate random bytes for encryption");
        return std::unexpected(error_code::crypto_error);
    }
    
    // Derive key from password
    QByteArray key = derive_key(password, salt, params.iterations, params.key_size);
    if (key.isEmpty()) {
        logError("Failed to derive encryption key");
        return std::unexpected(error_code::crypto_error);
    }
    
    // Encrypt data
    QByteArray ciphertext = aes_encrypt(data, key, iv);
    
    // Securely wipe key material
    secure_wiper::wipe(key.data(), key.size());
    
    if (ciphertext.isEmpty()) {
        logError("AES encryption failed");
        return std::unexpected(error_code::crypto_error);
    }
    
    // Format: [salt][IV][ciphertext]
    QByteArray result;
    result.append(salt);
    result.append(iv);
    result.append(ciphertext);
    
    logDebug("Encryption", std::format("Encrypted {} bytes to {} bytes", 
                                         data.size(), result.size()));
    
    return result;
#else
    return std::unexpected(error_code::not_implemented);
#endif
}

auto decryptData(
    const QByteArray& encrypted_data,
    const QString& password,
    const EncryptionParams& params
) -> std::expected<QByteArray, error_code> {
#ifdef _WIN32
    if (password.isEmpty()) {
        return std::unexpected(error_code::invalid_argument);
    }
    
    int header_size = params.salt_size + params.iv_size;
    if (encrypted_data.size() < header_size) {
        logError("Encrypted data too small - corrupted or invalid");
        return std::unexpected(error_code::invalid_format);
    }
    
    // Extract salt and IV
    QByteArray salt = encrypted_data.left(params.salt_size);
    QByteArray iv = encrypted_data.mid(params.salt_size, params.iv_size);
    QByteArray ciphertext = encrypted_data.mid(header_size);
    
    // Derive key from password
    QByteArray key = derive_key(password, salt, params.iterations, params.key_size);
    if (key.isEmpty()) {
        logError("Failed to derive decryption key");
        return std::unexpected(error_code::crypto_error);
    }
    
    // Decrypt data
    QByteArray plaintext = aes_decrypt(ciphertext, key, iv);
    
    // Securely wipe key material
    secure_wiper::wipe(key.data(), key.size());
    
    if (plaintext.isEmpty()) {
        logError("AES decryption failed - wrong password or corrupted data");
        return std::unexpected(error_code::decrypt_failed);
    }
    
    logDebug("Decryption", std::format("Decrypted {} bytes to {} bytes",
                                         encrypted_data.size(), plaintext.size()));
    
    return plaintext;
#else
    return std::unexpected(error_code::not_implemented);
#endif
}

} // namespace sak

// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/error_codes.h"

#include <QByteArray>
#include <QString>

#include <expected>

namespace sak {

/// @brief AES-GCM encrypted data with IV and authentication tag
struct EncryptedPayload {
    QByteArray iv;
    QByteArray ciphertext;
    QByteArray tag;
};

/// @brief AES-GCM encryption and key derivation for transfers
class TransferSecurityManager {
public:
    /// @brief Generate cryptographically secure random bytes
    static QByteArray generateRandomBytes(int size);

    /// @brief Derive an encryption key from a passphrase using PBKDF2
    /// @param passphrase The user-supplied passphrase
    /// @param salt Cryptographic salt for key derivation
    /// @param iterations PBKDF2 iteration count (default: 200,000)
    /// @param key_length Desired key length in bytes (default: 32 for AES-256)
    /// @return The derived key bytes, or error_code on failure
    static std::expected<QByteArray, error_code> deriveKey(const QString& passphrase,
                                                           const QByteArray& salt,
                                                           int iterations = 200'000,
                                                           int key_length = 32);

    /// @brief Encrypt data using AES-256-GCM authenticated encryption
    /// @param plaintext The data to encrypt
    /// @param key 32-byte AES-256 encryption key
    /// @param aad Optional additional authenticated data (integrity-checked but not encrypted)
    /// @return EncryptedPayload containing IV, ciphertext, and auth tag, or error_code on failure
    static std::expected<EncryptedPayload, error_code> encryptAesGcm(const QByteArray& plaintext,
                                                                     const QByteArray& key,
                                                                     const QByteArray& aad = {});

    /// @brief Decrypt AES-256-GCM encrypted data and verify authentication
    /// @param payload The encrypted payload (IV, ciphertext, tag)
    /// @param key 32-byte AES-256 decryption key
    /// @param aad Optional additional authenticated data (must match encryption AAD)
    /// @return Decrypted plaintext bytes, or error_code on auth failure or decryption error
    static std::expected<QByteArray, error_code> decryptAesGcm(const EncryptedPayload& payload,
                                                               const QByteArray& key,
                                                               const QByteArray& aad = {});
};

}  // namespace sak

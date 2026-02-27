// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file encryption.h
/// @brief AES-256 encryption/decryption using Windows BCrypt API
/// @note Enterprise-grade encryption with PBKDF2 key derivation

#pragma once

#include "error_codes.h"
#include <expected>
#include <QString>
#include <QByteArray>

namespace sak {

/// @brief AES-256 encryption parameters
struct EncryptionParams {
    int iterations = 100000;     // PBKDF2 iterations
    int key_size = 32;           // 256 bits
    int iv_size = 16;            // 128 bits (AES block size)
    int salt_size = 32;          // Salt for key derivation
};

/// @brief Encrypt data using AES-256-CBC with password
/// @param data Plain text data to encrypt
/// @param password User password for encryption
/// @param params Encryption parameters
/// @return Encrypted data (salt + IV + ciphertext) or error code
/// @note Format: [32 bytes salt][16 bytes IV][encrypted data]
[[nodiscard]] auto encryptData(
    const QByteArray& data,
    const QString& password,
    const EncryptionParams& params = EncryptionParams{}
) -> std::expected<QByteArray, error_code>;

/// @brief Decrypt data using AES-256-CBC with password
/// @param encrypted_data Encrypted data from encryptData
/// @param password User password for decryption
/// @param params Encryption parameters
/// @return Decrypted data or error code
[[nodiscard]] auto decryptData(
    const QByteArray& encrypted_data,
    const QString& password,
    const EncryptionParams& params = EncryptionParams{}
) -> std::expected<QByteArray, error_code>;

} // namespace sak


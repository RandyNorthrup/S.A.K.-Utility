// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file encryption.h
/// @brief AES-256 encryption/decryption using Windows BCrypt API
/// @note Enterprise-grade encryption with PBKDF2 key derivation

#pragma once

#include "error_codes.h"

#include <QByteArray>
#include <QString>

#include <expected>
#include <memory>

namespace sak {

inline constexpr int kDefaultPbkdf2Iterations = 100'000;
inline constexpr int kAes256KeyBytes = 32;
inline constexpr int kAesBlockBytes = 16;
inline constexpr int kEncryptionSaltBytes = 32;
inline constexpr int kEncryptionMacBytes = 32;     // HMAC-SHA256 tag length
inline constexpr int kEncryptionMacKeyBytes = 32;  // HMAC-SHA256 key length

/// @brief AES-256 encryption parameters
struct EncryptionParams {
    int iterations = kDefaultPbkdf2Iterations;
    int key_size = kAes256KeyBytes;
    int iv_size = kAesBlockBytes;
    int salt_size = kEncryptionSaltBytes;
};

/// @brief Encrypt data using AES-256-CBC with password
/// @param data Plain text data to encrypt
/// @param password User password for encryption
/// @param params Encryption parameters
/// @return Encrypted data (salt + IV + ciphertext + auth tag) or error code
/// @note Format: [32 bytes salt][16 bytes IV][ciphertext][32 bytes HMAC-SHA256 tag]
[[nodiscard]] auto encryptData(const QByteArray& data,
                               const QString& password,
                               const EncryptionParams& params = EncryptionParams{})
    -> std::expected<QByteArray, error_code>;

/// @brief Decrypt data using AES-256-CBC with password
/// @param encrypted_data Encrypted data from encryptData
/// @param password User password for decryption
/// @param params Encryption parameters
/// @return Decrypted data or error code
[[nodiscard]] auto decryptData(const QByteArray& encrypted_data,
                               const QString& password,
                               const EncryptionParams& params = EncryptionParams{})
    -> std::expected<QByteArray, error_code>;

/// @brief Incremental AES-256-CBC + HMAC-SHA256 (encrypt-then-MAC).
///
/// encryptData()/decryptData() hold the whole payload in memory, which is fine for a
/// settings blob and impossible for a user profile backup - those run to tens of
/// gigabytes. These classes carry the same construction (one PBKDF2 derivation, separate
/// AES and HMAC keys, encrypt-then-MAC, constant-time tag compare) across an unbounded
/// stream at constant memory.
///
/// The key is derived ONCE for the whole stream, and CBC chains across update() calls, so
/// the caller must feed the ciphertext back in exactly the order it was produced. Callers
/// need not align their writes: whole blocks are encrypted as they accumulate and the
/// remainder is buffered internally until finish() applies the padding.
///
/// Wire format, which the caller writes in this order:
///     [header(): salt][IV] [update()... ciphertext] [finish(): final block][32-byte tag]
/// The tag covers the header AND every ciphertext byte, so truncating, reordering or
/// editing any part of the stream fails verification.
class StreamEncryptor {
public:
    /// @brief Derive keys and start a stream. Fails on an empty password or bad params.
    [[nodiscard]] static auto create(const QString& password,
                                     const EncryptionParams& params = EncryptionParams{})
        -> std::expected<std::unique_ptr<StreamEncryptor>, error_code>;

    ~StreamEncryptor();

    StreamEncryptor(const StreamEncryptor&) = delete;
    StreamEncryptor& operator=(const StreamEncryptor&) = delete;
    StreamEncryptor(StreamEncryptor&&) = delete;
    StreamEncryptor& operator=(StreamEncryptor&&) = delete;

    /// @brief salt||IV. Write this first; it is covered by the tag.
    [[nodiscard]] QByteArray header() const;

    /// @brief Encrypt what is now a whole number of blocks; the rest is held back.
    /// Returns the ciphertext to append (possibly empty).
    [[nodiscard]] auto update(const QByteArray& plaintext) -> std::expected<QByteArray, error_code>;

    /// @brief Flush the buffered remainder with padding and append the tag.
    /// The stream is finished afterwards and update() must not be called again.
    [[nodiscard]] auto finish() -> std::expected<QByteArray, error_code>;

private:
    StreamEncryptor();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// @brief Incremental counterpart to StreamEncryptor.
///
/// IMPORTANT: the tag can only be checked once the whole stream has been seen, so
/// plaintext returned by update() is NOT yet authenticated. Callers must treat it as
/// untrusted until finish() succeeds - write it somewhere provisional and only publish it
/// (rename into place) after finish() returns without error. finish() failing means the
/// stream was truncated, reordered, corrupted, or the password was wrong; the plaintext
/// produced so far must be discarded.
class StreamDecryptor {
public:
    /// @brief Start a stream from the salt||IV header written by StreamEncryptor::header().
    [[nodiscard]] static auto create(const QString& password,
                                     const QByteArray& header,
                                     const EncryptionParams& params = EncryptionParams{})
        -> std::expected<std::unique_ptr<StreamDecryptor>, error_code>;

    ~StreamDecryptor();

    StreamDecryptor(const StreamDecryptor&) = delete;
    StreamDecryptor& operator=(const StreamDecryptor&) = delete;
    StreamDecryptor(StreamDecryptor&&) = delete;
    StreamDecryptor& operator=(StreamDecryptor&&) = delete;

    /// @brief Size of the salt||IV header for @p params, i.e. what create() expects.
    [[nodiscard]] static int headerSize(const EncryptionParams& params = EncryptionParams{});

    /// @brief Consume ciphertext and return UNAUTHENTICATED plaintext (see class note).
    /// The trailing tag must NOT be fed here; hold back the last kEncryptionMacBytes.
    [[nodiscard]] auto update(const QByteArray& ciphertext)
        -> std::expected<QByteArray, error_code>;

    /// @brief Decrypt the final block and verify @p tag in constant time.
    [[nodiscard]] auto finish(const QByteArray& tag) -> std::expected<QByteArray, error_code>;

private:
    StreamDecryptor();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace sak

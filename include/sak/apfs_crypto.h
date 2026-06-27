// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file apfs_crypto.h
/// @brief APFS FileVault cryptographic primitives (A6 / A-f).
///
/// Self-contained, enterprise-grade primitives required to read and write
/// software-encrypted (FileVault) APFS volumes:
///   - SHA-256 / HMAC-SHA-256
///   - PBKDF2-HMAC-SHA-256 (password / recovery-key -> derived unwrap key)
///   - AES-256 single-block ECB (RFC 3394 building block)
///   - RFC 3394 AES key wrap / unwrap (KEK<->VEK, password-key<->KEK)
///   - AES-XTS (128-bit data-unit key) over 512-byte APFS crypto units
///
/// All primitives are credential-in / never credential-stored: callers hold
/// secrets only in memory and wipe them. Backed by the Windows CNG (BCrypt)
/// provider; the platform-independent RFC 3394 logic is pure C++.

#pragma once

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <optional>

namespace sak::apfs_crypto {

/// APFS crypto unit size: AES-XTS tweak advances every 512 bytes, so a 4096-byte
/// APFS block spans 8 data units whose tweak base = block_addr * 8.
inline constexpr int kApfsCryptoUnitBytes = 512;
inline constexpr int kApfsCryptoUnitsPerBlock = 4096 / kApfsCryptoUnitBytes;

/// VEK / KEK are AES-256-wrapped 32-byte keys (XTS-128 uses two 16-byte halves).
inline constexpr int kApfsWrappedKeyBytes = 40;    ///< 32 key + 8 RFC 3394 overhead
inline constexpr int kApfsUnwrappedKeyBytes = 32;  ///< key1(16) || key2(16)
inline constexpr int kApfsKekBytes = 32;           ///< 256-bit KEK / derived key

/// @brief SHA-256 digest of @p data (32 bytes), or empty on failure.
[[nodiscard]] QByteArray sha256(const QByteArray& data);

/// @brief HMAC-SHA-256 of @p message under @p key (32 bytes), empty on failure.
[[nodiscard]] QByteArray hmacSha256(const QByteArray& key, const QByteArray& message);

/// @brief PBKDF2-HMAC-SHA-256(password, salt, iterations) -> @p keyLength bytes.
[[nodiscard]] QByteArray pbkdf2Sha256(const QByteArray& password,
                                      const QByteArray& salt,
                                      uint64_t iterations,
                                      int keyLength);

/// @brief AES-ECB encrypt one 16-byte block under @p key (AES-128/192/256).
/// Exposed as a building block (RFC 3394 / XTS reference checks); empty on failure.
[[nodiscard]] QByteArray aesEcbEncryptBlock(const QByteArray& key, const QByteArray& block16);

/// @brief RFC 3394 AES key wrap of @p plaintextKey under 256-bit @p kek.
/// @return wrapped key (plaintextKey.size() + 8 bytes), or empty on failure.
[[nodiscard]] QByteArray aesKeyWrap(const QByteArray& kek, const QByteArray& plaintextKey);

/// @brief RFC 3394 AES key unwrap of @p wrappedKey under 256-bit @p kek.
/// @return unwrapped key, or nullopt if the integrity check (A6A6...) fails.
[[nodiscard]] std::optional<QByteArray> aesKeyUnwrap(const QByteArray& kek,
                                                     const QByteArray& wrappedKey);

/// @brief AES-XTS encrypt @p data in place-style; tweak base = @p firstDataUnit.
/// @param xtsKey 32-byte XTS-128 key (key1 || key2).
/// @param firstDataUnit data-unit number of the first @p unitBytes-sized unit.
/// @param data length must be a multiple of @p unitBytes.
/// @return ciphertext, or empty on failure.
[[nodiscard]] QByteArray xtsEncrypt(const QByteArray& xtsKey,
                                    uint64_t firstDataUnit,
                                    const QByteArray& data,
                                    int unitBytes = kApfsCryptoUnitBytes);

/// @brief AES-XTS decrypt; mirror of xtsEncrypt.
[[nodiscard]] QByteArray xtsDecrypt(const QByteArray& xtsKey,
                                    uint64_t firstDataUnit,
                                    const QByteArray& data,
                                    int unitBytes = kApfsCryptoUnitBytes);

/// @brief Encrypt one 4096-byte APFS metadata/data block at physical @p blockAddr
/// with the volume encryption key (tweak base = blockAddr * 8).
[[nodiscard]] QByteArray xtsEncryptBlock(const QByteArray& vek,
                                         uint64_t blockAddr,
                                         const QByteArray& plaintextBlock);

/// @brief Decrypt one 4096-byte APFS block (mirror of xtsEncryptBlock).
[[nodiscard]] QByteArray xtsDecryptBlock(const QByteArray& vek,
                                         uint64_t blockAddr,
                                         const QByteArray& ciphertextBlock);

}  // namespace sak::apfs_crypto

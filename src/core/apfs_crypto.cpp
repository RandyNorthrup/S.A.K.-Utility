// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file apfs_crypto.cpp
/// @brief APFS FileVault cryptographic primitives implementation (Windows CNG).

#include "sak/apfs_crypto.h"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>

#include <cstring>

#ifdef _WIN32
#include <windows.h>

#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace sak::apfs_crypto {

namespace {

#ifdef _WIN32

/// @brief RAII wrapper for a BCrypt algorithm provider handle.
class AlgProvider {
public:
    explicit AlgProvider(LPCWSTR algId, ULONG flags = 0) {
        if (BCryptOpenAlgorithmProvider(&handle_, algId, nullptr, flags) != 0) {
            handle_ = nullptr;
        }
    }
    ~AlgProvider() {
        if (handle_ != nullptr) {
            BCryptCloseAlgorithmProvider(handle_, 0);
        }
    }
    AlgProvider(const AlgProvider&) = delete;
    AlgProvider& operator=(const AlgProvider&) = delete;
    [[nodiscard]] bool valid() const { return handle_ != nullptr; }
    [[nodiscard]] BCRYPT_ALG_HANDLE get() const { return handle_; }

private:
    BCRYPT_ALG_HANDLE handle_{nullptr};
};

/// @brief RAII wrapper for a BCrypt symmetric key handle.
class SymKey {
public:
    SymKey(BCRYPT_ALG_HANDLE alg, const QByteArray& key) {
        if (BCryptGenerateSymmetricKey(alg,
                                       &handle_,
                                       nullptr,
                                       0,
                                       reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
                                       static_cast<ULONG>(key.size()),
                                       0) != 0) {
            handle_ = nullptr;
        }
    }
    ~SymKey() {
        if (handle_ != nullptr) {
            BCryptDestroyKey(handle_);
        }
    }
    SymKey(const SymKey&) = delete;
    SymKey& operator=(const SymKey&) = delete;
    [[nodiscard]] bool valid() const { return handle_ != nullptr; }
    [[nodiscard]] BCRYPT_KEY_HANDLE get() const { return handle_; }

private:
    BCRYPT_KEY_HANDLE handle_{nullptr};
};

/// @brief Set the AES chaining mode (ECB) on an algorithm provider.
bool setEcbMode(BCRYPT_ALG_HANDLE alg) {
    return BCryptSetProperty(alg,
                             BCRYPT_CHAINING_MODE,
                             reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_ECB)),
                             sizeof(BCRYPT_CHAIN_MODE_ECB),
                             0) == 0;
}

/// @brief True if @p n is a valid AES key length (128/192/256-bit).
bool isAesKeyLen(qsizetype n) {
    return n == 16 || n == 24 || n == 32;
}

/// @brief AES-ECB transform of @p data (a multiple of 16 bytes) in one CNG call.
/// ECB processes each 16-byte block independently — the building block for both
/// RFC 3394 (single block) and the AES-XTS construction (whole data unit). The
/// Windows CNG XTS-AES provider rejects BCryptEncrypt on this platform, so XTS is
/// built here from AES-ECB, which is the textbook XTS definition.
QByteArray aesEcbTransform(const QByteArray& key, const QByteArray& data, bool encrypt) {
    if (!isAesKeyLen(key.size()) || data.isEmpty() || (data.size() % 16) != 0) {
        return {};
    }
    AlgProvider alg(BCRYPT_AES_ALGORITHM);
    if (!alg.valid() || !setEcbMode(alg.get())) {
        return {};
    }
    SymKey symKey(alg.get(), key);
    if (!symKey.valid()) {
        return {};
    }
    QByteArray out(data.size(), 0);
    ULONG produced = 0;
    auto* in = reinterpret_cast<PUCHAR>(const_cast<char*>(data.data()));
    auto* dst = reinterpret_cast<PUCHAR>(out.data());
    const auto len = static_cast<ULONG>(data.size());
    const NTSTATUS status =
        encrypt ? BCryptEncrypt(symKey.get(), in, len, nullptr, nullptr, 0, dst, len, &produced, 0)
                : BCryptDecrypt(symKey.get(), in, len, nullptr, nullptr, 0, dst, len, &produced, 0);
    if (status != 0 || produced != len) {
        return {};
    }
    return out;
}

/// @brief AES-ECB transform of exactly one 16-byte block.
QByteArray aesEcbBlock(const QByteArray& key, const QByteArray& block16, bool encrypt) {
    if (block16.size() != 16) {
        return {};
    }
    return aesEcbTransform(key, block16, encrypt);
}

/// @brief 16-byte little-endian serialization of an XTS data-unit number.
QByteArray le128(uint64_t value) {
    QByteArray out(16, 0);
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<char>((value >> (8 * i)) & 0xFF);
    }
    return out;
}

/// @brief GF(2^128) multiply-by-alpha (x), little-endian, reduction poly 0x87 —
/// advances the XTS tweak from one 16-byte block to the next (IEEE Std 1619).
QByteArray gf128MulAlpha(const QByteArray& t) {
    QByteArray out = t;
    unsigned char carry = 0;
    for (int i = 0; i < 16; ++i) {
        const auto b = static_cast<unsigned char>(out[i]);
        out[i] = static_cast<char>(((b << 1) | carry) & 0xFF);
        carry = (b >> 7) & 1;
    }
    if (carry != 0) {
        out[0] = static_cast<char>(static_cast<unsigned char>(out[0]) ^ 0x87);
    }
    return out;
}

/// @brief XTS transform of one data unit (size a multiple of 16) under key1/key2.
QByteArray xtsTransformUnit(const QByteArray& key1,
                            const QByteArray& key2,
                            uint64_t dataUnit,
                            const QByteArray& unitData,
                            bool encrypt) {
    QByteArray tj = aesEcbBlock(key2, le128(dataUnit), true);
    if (tj.size() != 16) {
        return {};
    }
    const int blocks = static_cast<int>(unitData.size() / 16);
    QByteArray masked(unitData.size(), 0);
    QByteArray tweakStream(unitData.size(), 0);
    for (int j = 0; j < blocks; ++j) {
        std::memcpy(tweakStream.data() + j * 16, tj.constData(), 16);
        for (int k = 0; k < 16; ++k) {
            masked[j * 16 + k] = static_cast<char>(unitData[j * 16 + k] ^ tj[k]);
        }
        tj = gf128MulAlpha(tj);
    }
    const QByteArray transformed = aesEcbTransform(key1, masked, encrypt);
    if (transformed.size() != unitData.size()) {
        return {};
    }
    QByteArray out(unitData.size(), 0);
    for (int i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>(transformed[i] ^ tweakStream[i]);
    }
    return out;
}

/// @brief AES-XTS-128 transform of @p data as consecutive @p unitBytes data units,
/// tweak of unit u = firstDataUnit + u. Built from AES-ECB (see aesEcbTransform).
QByteArray xtsTransform(const QByteArray& xtsKey,
                        uint64_t firstDataUnit,
                        const QByteArray& data,
                        int unitBytes,
                        bool encrypt) {
    if (xtsKey.size() != kApfsUnwrappedKeyBytes || unitBytes <= 0 || (unitBytes % 16) != 0 ||
        data.isEmpty() || (data.size() % unitBytes) != 0) {
        return {};
    }
    const QByteArray key1 = xtsKey.left(16);
    const QByteArray key2 = xtsKey.mid(16, 16);
    const int units = static_cast<int>(data.size() / unitBytes);
    QByteArray out(data.size(), 0);
    for (int u = 0; u < units; ++u) {
        const QByteArray unitData = data.mid(static_cast<qsizetype>(u) * unitBytes, unitBytes);
        const QByteArray r = xtsTransformUnit(key1, key2, firstDataUnit + u, unitData, encrypt);
        if (r.size() != unitBytes) {
            return {};
        }
        std::memcpy(out.data() + static_cast<qsizetype>(u) * unitBytes, r.constData(), unitBytes);
    }
    return out;
}

/// @brief XOR a big-endian 64-bit counter @p t into the 8-byte block @p a.
void xorCounter(unsigned char* a, uint64_t t) {
    for (int i = 0; i < 8; ++i) {
        a[7 - i] ^= static_cast<unsigned char>((t >> (8 * i)) & 0xFF);
    }
}

#endif  // _WIN32

}  // namespace

QByteArray sha256(const QByteArray& data) {
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

QByteArray hmacSha256(const QByteArray& key, const QByteArray& message) {
    return QMessageAuthenticationCode::hash(message, key, QCryptographicHash::Sha256);
}

QByteArray pbkdf2Sha256(const QByteArray& password,
                        const QByteArray& salt,
                        uint64_t iterations,
                        int keyLength) {
#ifdef _WIN32
    if (keyLength <= 0 || iterations == 0) {
        return {};
    }
    AlgProvider alg(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!alg.valid()) {
        return {};
    }
    QByteArray out(keyLength, 0);
    const NTSTATUS status =
        BCryptDeriveKeyPBKDF2(alg.get(),
                              reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
                              static_cast<ULONG>(password.size()),
                              reinterpret_cast<PUCHAR>(const_cast<char*>(salt.data())),
                              static_cast<ULONG>(salt.size()),
                              iterations,
                              reinterpret_cast<PUCHAR>(out.data()),
                              static_cast<ULONG>(keyLength),
                              0);
    if (status != 0) {
        return {};
    }
    return out;
#else
    Q_UNUSED(password);
    Q_UNUSED(salt);
    Q_UNUSED(iterations);
    Q_UNUSED(keyLength);
    return {};
#endif
}

QByteArray aesEcbEncryptBlock(const QByteArray& key, const QByteArray& block16) {
#ifdef _WIN32
    return aesEcbBlock(key, block16, true);
#else
    Q_UNUSED(key);
    Q_UNUSED(block16);
    return {};
#endif
}

QByteArray aesKeyWrap(const QByteArray& kek, const QByteArray& plaintextKey) {
#ifdef _WIN32
    if (kek.size() != kApfsKekBytes || plaintextKey.isEmpty() || (plaintextKey.size() % 8) != 0) {
        return {};
    }
    const int n = static_cast<int>(plaintextKey.size() / 8);
    QByteArray a(8, '\xA6');
    QByteArray r = plaintextKey;
    QByteArray buf(16, 0);
    for (int j = 0; j < 6; ++j) {
        for (int i = 1; i <= n; ++i) {
            std::memcpy(buf.data(), a.constData(), 8);
            std::memcpy(buf.data() + 8, r.constData() + (i - 1) * 8, 8);
            const QByteArray b = aesEcbBlock(kek, buf, true);
            if (b.size() != 16) {
                return {};
            }
            std::memcpy(a.data(), b.constData(), 8);
            xorCounter(reinterpret_cast<unsigned char*>(a.data()),
                       static_cast<uint64_t>(n) * j + i);
            std::memcpy(r.data() + (i - 1) * 8, b.constData() + 8, 8);
        }
    }
    QByteArray out;
    out.append(a);
    out.append(r);
    return out;
#else
    Q_UNUSED(kek);
    Q_UNUSED(plaintextKey);
    return {};
#endif
}

std::optional<QByteArray> aesKeyUnwrap(const QByteArray& kek, const QByteArray& wrappedKey) {
#ifdef _WIN32
    if (kek.size() != kApfsKekBytes || wrappedKey.size() < 16 || (wrappedKey.size() % 8) != 0) {
        return std::nullopt;
    }
    const int n = static_cast<int>(wrappedKey.size() / 8) - 1;
    QByteArray a = wrappedKey.left(8);
    QByteArray r = wrappedKey.mid(8);
    QByteArray buf(16, 0);
    for (int j = 5; j >= 0; --j) {
        for (int i = n; i >= 1; --i) {
            std::memcpy(buf.data(), a.constData(), 8);
            xorCounter(reinterpret_cast<unsigned char*>(buf.data()),
                       static_cast<uint64_t>(n) * j + i);
            std::memcpy(buf.data() + 8, r.constData() + (i - 1) * 8, 8);
            const QByteArray b = aesEcbBlock(kek, buf, false);
            if (b.size() != 16) {
                return std::nullopt;
            }
            std::memcpy(a.data(), b.constData(), 8);
            std::memcpy(r.data() + (i - 1) * 8, b.constData() + 8, 8);
        }
    }
    for (char byte : a) {
        if (static_cast<unsigned char>(byte) != 0xA6) {
            return std::nullopt;  // integrity check failed (wrong key)
        }
    }
    return r;
#else
    Q_UNUSED(kek);
    Q_UNUSED(wrappedKey);
    return std::nullopt;
#endif
}

QByteArray xtsEncrypt(const QByteArray& xtsKey,
                      uint64_t firstDataUnit,
                      const QByteArray& data,
                      int unitBytes) {
#ifdef _WIN32
    return xtsTransform(xtsKey, firstDataUnit, data, unitBytes, true);
#else
    Q_UNUSED(xtsKey);
    Q_UNUSED(firstDataUnit);
    Q_UNUSED(data);
    Q_UNUSED(unitBytes);
    return {};
#endif
}

QByteArray xtsDecrypt(const QByteArray& xtsKey,
                      uint64_t firstDataUnit,
                      const QByteArray& data,
                      int unitBytes) {
#ifdef _WIN32
    return xtsTransform(xtsKey, firstDataUnit, data, unitBytes, false);
#else
    Q_UNUSED(xtsKey);
    Q_UNUSED(firstDataUnit);
    Q_UNUSED(data);
    Q_UNUSED(unitBytes);
    return {};
#endif
}

QByteArray xtsEncryptBlock(const QByteArray& vek,
                           uint64_t blockAddr,
                           const QByteArray& plaintextBlock) {
    return xtsEncrypt(vek, blockAddr * kApfsCryptoUnitsPerBlock, plaintextBlock);
}

QByteArray xtsDecryptBlock(const QByteArray& vek,
                           uint64_t blockAddr,
                           const QByteArray& ciphertextBlock) {
    return xtsDecrypt(vek, blockAddr * kApfsCryptoUnitsPerBlock, ciphertextBlock);
}

}  // namespace sak::apfs_crypto

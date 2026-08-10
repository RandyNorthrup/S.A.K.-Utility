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
// The 32-bit ULONG ceiling BCrypt one-shot length parameters cannot exceed. 0xFFFFFFFF ==
// ULONG_MAX.
constexpr quint64 kBcryptMaxLengthBytes = 0xFF'FF'FF'FFULL;

/// @brief Guard against silent 32-bit truncation of a length. BCrypt's one-shot length
/// parameters are ULONG (32-bit); a QByteArray larger than that would narrow to its low 32
/// bits, so the cipher/HMAC would cover only a prefix while still reporting success. Any
/// over-size input must fail closed instead. 0xFFFFFFFF == ULONG_MAX.
bool bcrypt_length_ok(qsizetype n) {
    return static_cast<quint64>(n) <= kBcryptMaxLengthBytes;
}

/// @brief Generate cryptographic random bytes
QByteArray generate_random_bytes(int size) {
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
    if (!bcrypt_length_ok(pwd_bytes.size()) || !bcrypt_length_ok(salt.size())) {
        sak::logError("BCrypt: password or salt length exceeds the 32-bit BCrypt limit");
        sak::secure_wiper::wipe(pwd_bytes.data(), pwd_bytes.size());
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
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
    if (!bcrypt_length_ok(mac_key.size()) || !bcrypt_length_ok(message.size())) {
        sak::logError("BCrypt: HMAC input exceeds the 32-bit BCrypt length limit");
        return {};
    }
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

/// @brief Incremental HMAC-SHA256, so a tag can cover a stream that never fits in memory.
/// compute_hmac() above is the one-shot form used by the in-memory API.
class HmacStream {
public:
    HmacStream() = default;
    ~HmacStream() { reset(); }

    HmacStream(const HmacStream&) = delete;
    HmacStream& operator=(const HmacStream&) = delete;
    HmacStream(HmacStream&&) = delete;
    HmacStream& operator=(HmacStream&&) = delete;

    [[nodiscard]] bool init(const QByteArray& mac_key) {
        if (BCryptOpenAlgorithmProvider(
                &m_alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
            sak::logError("BCrypt: Failed to open HMAC provider for streaming authentication");
            return false;
        }
        DWORD object_len = 0;
        DWORD copied = 0;
        if (BCryptGetProperty(m_alg,
                              BCRYPT_OBJECT_LENGTH,
                              reinterpret_cast<PUCHAR>(&object_len),
                              sizeof(object_len),
                              &copied,
                              0) != 0) {
            sak::logError("BCrypt: Failed to size the streaming HMAC object");
            return false;
        }
        m_object.resize(static_cast<qsizetype>(object_len));
        if (BCryptCreateHash(m_alg,
                             &m_hash,
                             reinterpret_cast<PUCHAR>(m_object.data()),
                             object_len,
                             reinterpret_cast<PUCHAR>(const_cast<char*>(mac_key.data())),
                             static_cast<ULONG>(mac_key.size()),
                             0) != 0) {
            sak::logError("BCrypt: Failed to create the streaming HMAC");
            return false;
        }
        return true;
    }

    [[nodiscard]] bool update(const QByteArray& data) {
        if (!m_hash || data.isEmpty()) {
            return m_hash != nullptr;
        }
        if (!bcrypt_length_ok(data.size())) {
            sak::logError("BCrypt: streaming HMAC update exceeds the 32-bit BCrypt length limit");
            return false;
        }
        if (BCryptHashData(m_hash,
                           reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                           static_cast<ULONG>(data.size()),
                           0) != 0) {
            sak::logError("BCrypt: Streaming HMAC update failed");
            return false;
        }
        return true;
    }

    [[nodiscard]] QByteArray finish() {
        if (!m_hash) {
            return {};
        }
        QByteArray tag(kEncryptionMacBytes, 0);
        const NTSTATUS status = BCryptFinishHash(
            m_hash, reinterpret_cast<PUCHAR>(tag.data()), static_cast<ULONG>(tag.size()), 0);
        if (status != 0) {
            sak::logError("BCrypt: Streaming HMAC finalization failed");
            return {};
        }
        return tag;
    }

private:
    void reset() {
        if (m_hash) {
            BCryptDestroyHash(m_hash);
            m_hash = nullptr;
        }
        if (m_alg) {
            BCryptCloseAlgorithmProvider(m_alg, 0);
            m_alg = nullptr;
        }
    }

    BCRYPT_ALG_HANDLE m_alg{nullptr};
    BCRYPT_HASH_HANDLE m_hash{nullptr};
    QByteArray m_object;
};

/// @brief Encrypt whole blocks with the chaining IV updated in place, so the next call
/// continues the same CBC stream. No padding: the caller only passes whole blocks until
/// the final flush.
bool aes_encrypt_blocks(BCRYPT_KEY_HANDLE key,
                        QByteArray& iv,
                        const QByteArray& in,
                        QByteArray& out) {
    if (!bcrypt_length_ok(in.size())) {
        sak::logError("BCrypt: streaming AES block exceeds the 32-bit BCrypt length limit");
        return false;
    }
    out.resize(in.size());
    DWORD written = 0;
    if (BCryptEncrypt(key,
                      reinterpret_cast<PUCHAR>(const_cast<char*>(in.data())),
                      static_cast<ULONG>(in.size()),
                      nullptr,
                      reinterpret_cast<PUCHAR>(iv.data()),
                      static_cast<ULONG>(iv.size()),
                      reinterpret_cast<PUCHAR>(out.data()),
                      static_cast<ULONG>(out.size()),
                      &written,
                      0) != 0) {
        sak::logError("BCrypt: Streaming AES encryption failed ({} bytes)", in.size());
        return false;
    }
    out.resize(static_cast<qsizetype>(written));
    return true;
}

/// @brief Decrypt whole blocks with the chaining IV updated in place. No padding: the
/// caller always holds the final block back for the padded flush.
bool aes_decrypt_blocks(BCRYPT_KEY_HANDLE key,
                        QByteArray& iv,
                        const QByteArray& in,
                        QByteArray& out) {
    if (!bcrypt_length_ok(in.size())) {
        sak::logError("BCrypt: streaming AES block exceeds the 32-bit BCrypt length limit");
        return false;
    }
    out.resize(in.size());
    DWORD written = 0;
    if (BCryptDecrypt(key,
                      reinterpret_cast<PUCHAR>(const_cast<char*>(in.data())),
                      static_cast<ULONG>(in.size()),
                      nullptr,
                      reinterpret_cast<PUCHAR>(iv.data()),
                      static_cast<ULONG>(iv.size()),
                      reinterpret_cast<PUCHAR>(out.data()),
                      static_cast<ULONG>(out.size()),
                      &written,
                      0) != 0) {
        sak::logError("BCrypt: Streaming AES decryption failed ({} bytes)", in.size());
        return false;
    }
    out.resize(static_cast<qsizetype>(written));
    return true;
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
// Upper bounds so a hostile or defaulted-broken EncryptionParams cannot drive a
// multi-gigabyte salt allocation (and the salt_size + iv_size int overflow that follows) or
// an attacker-chosen PBKDF2 iteration count that pins the CPU. The defaults (salt 32,
// iterations 100000) sit far inside these.
constexpr int kMaxEncryptionSaltBytes = 1024;
constexpr int kMaxPbkdf2Iterations = 100'000'000;
// A salt shorter than this offers too little collision resistance to derive keys from.
constexpr int kMinEncryptionSaltBytes = 8;

bool valid_encryption_params(const EncryptionParams& params) {
    const bool aes_key = params.key_size == 16 || params.key_size == 24 || params.key_size == 32;
    return aes_key && params.iv_size == kAesBlockBytes &&
           params.salt_size >= kMinEncryptionSaltBytes &&
           params.salt_size <= kMaxEncryptionSaltBytes && params.iterations > 0 &&
           params.iterations <= kMaxPbkdf2Iterations;
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
    if (!bcrypt_length_ok(plaintext.size())) {
        sak::logError("BCrypt: plaintext exceeds the 32-bit BCrypt length limit");
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
    if (!bcrypt_length_ok(ciphertext.size())) {
        sak::logError("BCrypt: ciphertext exceeds the 32-bit BCrypt length limit");
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

    // qsizetype throughout: params are bounded, but encrypted_data is untrusted and can
    // exceed 2 GiB, where a narrowing to int would slice the message at the wrong offset.
    const qsizetype header_size = static_cast<qsizetype>(params.salt_size) + params.iv_size;
    if (encrypted_data.size() < header_size + kEncryptionMacBytes) {
        logError("Encrypted data too small - corrupted or invalid");
        return std::unexpected(error_code::invalid_format);
    }

    // Split off the trailing HMAC tag; the message it authenticates is [salt][IV][ciphertext].
    const qsizetype message_size = encrypted_data.size() - kEncryptionMacBytes;
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

// ============================================================================
// Streaming AES-256-CBC + HMAC-SHA256 (encrypt-then-MAC)
// ============================================================================

struct StreamEncryptor::Impl {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg{nullptr};
    BCRYPT_KEY_HANDLE key{nullptr};
    HmacStream mac;
#endif
    QByteArray salt;
    QByteArray iv;       // running CBC state, updated in place by each block call
    QByteArray pending;  // sub-block remainder awaiting more input or the final flush
    bool finished{false};
};

StreamEncryptor::StreamEncryptor() : m_impl(std::make_unique<Impl>()) {}

StreamEncryptor::~StreamEncryptor() {
#ifdef _WIN32
    destroy_aes_key(m_impl->alg, m_impl->key);
#endif
    secure_wiper::wipe(m_impl->iv.data(), m_impl->iv.size());
    secure_wiper::wipe(m_impl->pending.data(), m_impl->pending.size());
}

auto StreamEncryptor::create(const QString& password, const EncryptionParams& params)
    -> std::expected<std::unique_ptr<StreamEncryptor>, error_code> {
#ifdef _WIN32
    if (password.isEmpty() || !valid_encryption_params(params)) {
        return std::unexpected(error_code::invalid_argument);
    }
    std::unique_ptr<StreamEncryptor> self(new StreamEncryptor());
    Impl& impl = *self->m_impl;

    impl.salt = generate_random_bytes(params.salt_size);
    impl.iv = generate_random_bytes(params.iv_size);
    if (impl.salt.isEmpty() || impl.iv.isEmpty()) {
        logError("Failed to generate random bytes for the encryption stream");
        return std::unexpected(error_code::crypto_error);
    }

    QByteArray enc_key;
    QByteArray mac_key;
    if (!derive_enc_and_mac_keys(password, impl.salt, params, enc_key, mac_key)) {
        logError("Failed to derive keys for the encryption stream");
        return std::unexpected(error_code::crypto_error);
    }

    const bool ready = initialize_aes_key(impl.alg, impl.key, enc_key, "stream encryption") &&
                       impl.mac.init(mac_key);
    secure_wiper::wipe(enc_key.data(), enc_key.size());
    secure_wiper::wipe(mac_key.data(), mac_key.size());
    if (!ready) {
        return std::unexpected(error_code::crypto_error);
    }

    // The tag covers the header too, so a swapped salt or IV cannot go unnoticed.
    if (!impl.mac.update(self->header())) {
        return std::unexpected(error_code::crypto_error);
    }
    return self;
#else
    Q_UNUSED(password)
    Q_UNUSED(params)
    return std::unexpected(error_code::not_implemented);
#endif
}

QByteArray StreamEncryptor::header() const {
    return m_impl->salt + m_impl->iv;
}

auto StreamEncryptor::update(const QByteArray& plaintext) -> std::expected<QByteArray, error_code> {
#ifdef _WIN32
    if (m_impl->finished) {
        return std::unexpected(error_code::invalid_argument);
    }
    m_impl->pending.append(plaintext);

    // Encrypt only whole blocks; padding belongs to finish() alone.
    const qsizetype whole = (m_impl->pending.size() / kAesBlockBytes) * kAesBlockBytes;
    if (whole == 0) {
        return QByteArray{};
    }
    const QByteArray input = m_impl->pending.left(whole);
    QByteArray out;
    if (!aes_encrypt_blocks(m_impl->key, m_impl->iv, input, out) || !m_impl->mac.update(out)) {
        return std::unexpected(error_code::crypto_error);
    }
    m_impl->pending.remove(0, whole);
    return out;
#else
    Q_UNUSED(plaintext)
    return std::unexpected(error_code::not_implemented);
#endif
}

auto StreamEncryptor::finish() -> std::expected<QByteArray, error_code> {
#ifdef _WIN32
    if (m_impl->finished) {
        return std::unexpected(error_code::invalid_argument);
    }
    m_impl->finished = true;

    // BCRYPT_BLOCK_PADDING always emits a full extra block, so an empty remainder still
    // produces one block here and an empty payload round-trips to empty.
    QByteArray out(m_impl->pending.size() + kAesBlockBytes, 0);
    DWORD written = 0;
    if (BCryptEncrypt(m_impl->key,
                      reinterpret_cast<PUCHAR>(m_impl->pending.data()),
                      static_cast<ULONG>(m_impl->pending.size()),
                      nullptr,
                      reinterpret_cast<PUCHAR>(m_impl->iv.data()),
                      static_cast<ULONG>(m_impl->iv.size()),
                      reinterpret_cast<PUCHAR>(out.data()),
                      static_cast<ULONG>(out.size()),
                      &written,
                      BCRYPT_BLOCK_PADDING) != 0) {
        logError("BCrypt: Streaming AES final block encryption failed");
        return std::unexpected(error_code::crypto_error);
    }
    out.resize(static_cast<qsizetype>(written));
    secure_wiper::wipe(m_impl->pending.data(), m_impl->pending.size());
    m_impl->pending.clear();

    if (!m_impl->mac.update(out)) {
        return std::unexpected(error_code::crypto_error);
    }
    const QByteArray tag = m_impl->mac.finish();
    if (tag.isEmpty()) {
        return std::unexpected(error_code::crypto_error);
    }
    return out + tag;
#else
    return std::unexpected(error_code::not_implemented);
#endif
}

struct StreamDecryptor::Impl {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg{nullptr};
    BCRYPT_KEY_HANDLE key{nullptr};
    HmacStream mac;
#endif
    QByteArray iv;
    QByteArray pending;  // always retains the final block for the padded flush
    bool finished{false};
};

StreamDecryptor::StreamDecryptor() : m_impl(std::make_unique<Impl>()) {}

StreamDecryptor::~StreamDecryptor() {
#ifdef _WIN32
    destroy_aes_key(m_impl->alg, m_impl->key);
#endif
    secure_wiper::wipe(m_impl->iv.data(), m_impl->iv.size());
    secure_wiper::wipe(m_impl->pending.data(), m_impl->pending.size());
}

int StreamDecryptor::headerSize(const EncryptionParams& params) {
    return params.salt_size + params.iv_size;
}

auto StreamDecryptor::create(const QString& password,
                             const QByteArray& header,
                             const EncryptionParams& params)
    -> std::expected<std::unique_ptr<StreamDecryptor>, error_code> {
#ifdef _WIN32
    if (password.isEmpty() || !valid_encryption_params(params)) {
        return std::unexpected(error_code::invalid_argument);
    }
    if (header.size() != headerSize(params)) {
        logError("Encrypted stream header is the wrong size - corrupted or not our format");
        return std::unexpected(error_code::invalid_format);
    }
    std::unique_ptr<StreamDecryptor> self(new StreamDecryptor());
    Impl& impl = *self->m_impl;

    const QByteArray salt = header.left(params.salt_size);
    impl.iv = header.mid(params.salt_size, params.iv_size);

    QByteArray enc_key;
    QByteArray mac_key;
    if (!derive_enc_and_mac_keys(password, salt, params, enc_key, mac_key)) {
        logError("Failed to derive keys for the decryption stream");
        return std::unexpected(error_code::crypto_error);
    }

    const bool ready = initialize_aes_key(impl.alg, impl.key, enc_key, "stream decryption") &&
                       impl.mac.init(mac_key);
    secure_wiper::wipe(enc_key.data(), enc_key.size());
    secure_wiper::wipe(mac_key.data(), mac_key.size());
    if (!ready) {
        return std::unexpected(error_code::crypto_error);
    }

    if (!impl.mac.update(header)) {
        return std::unexpected(error_code::crypto_error);
    }
    return self;
#else
    Q_UNUSED(password)
    Q_UNUSED(header)
    Q_UNUSED(params)
    return std::unexpected(error_code::not_implemented);
#endif
}

auto StreamDecryptor::update(const QByteArray& ciphertext)
    -> std::expected<QByteArray, error_code> {
#ifdef _WIN32
    if (m_impl->finished) {
        return std::unexpected(error_code::invalid_argument);
    }
    if (!m_impl->mac.update(ciphertext)) {
        return std::unexpected(error_code::crypto_error);
    }
    m_impl->pending.append(ciphertext);

    // Always hold back at least one whole block: the padded flush in finish() needs it,
    // and only that call may strip padding.
    if (m_impl->pending.size() <= kAesBlockBytes) {
        return QByteArray{};
    }
    const qsizetype whole = ((m_impl->pending.size() - 1) / kAesBlockBytes) * kAesBlockBytes;
    if (whole == 0) {
        return QByteArray{};
    }
    const QByteArray input = m_impl->pending.left(whole);
    QByteArray out;
    if (!aes_decrypt_blocks(m_impl->key, m_impl->iv, input, out)) {
        return std::unexpected(error_code::crypto_error);
    }
    m_impl->pending.remove(0, whole);
    return out;
#else
    Q_UNUSED(ciphertext)
    return std::unexpected(error_code::not_implemented);
#endif
}

auto StreamDecryptor::finish(const QByteArray& tag) -> std::expected<QByteArray, error_code> {
#ifdef _WIN32
    if (m_impl->finished) {
        return std::unexpected(error_code::invalid_argument);
    }
    m_impl->finished = true;

    // Authenticate BEFORE the padded decrypt, exactly as decryptData() does: a wrong
    // password or an edited stream stops here and never reaches the padding check.
    const QByteArray computed = m_impl->mac.finish();
    if (computed.isEmpty() || !constant_time_equal(computed, tag)) {
        logError("Authentication failed - stream tampered, truncated, or wrong password");
        return std::unexpected(error_code::decrypt_failed);
    }
    // A genuine stream always ends on a whole block, because every byte of it came out of
    // BCryptEncrypt. Anything else is a truncation the tag should already have caught.
    if (m_impl->pending.size() != kAesBlockBytes) {
        logError("Encrypted stream does not end on a block boundary");
        return std::unexpected(error_code::invalid_format);
    }

    QByteArray out(m_impl->pending.size(), 0);
    DWORD written = 0;
    if (BCryptDecrypt(m_impl->key,
                      reinterpret_cast<PUCHAR>(m_impl->pending.data()),
                      static_cast<ULONG>(m_impl->pending.size()),
                      nullptr,
                      reinterpret_cast<PUCHAR>(m_impl->iv.data()),
                      static_cast<ULONG>(m_impl->iv.size()),
                      reinterpret_cast<PUCHAR>(out.data()),
                      static_cast<ULONG>(out.size()),
                      &written,
                      BCRYPT_BLOCK_PADDING) != 0) {
        logError("BCrypt: Streaming AES final block decryption failed");
        return std::unexpected(error_code::decrypt_failed);
    }
    out.resize(static_cast<qsizetype>(written));
    m_impl->pending.clear();
    return out;
#else
    Q_UNUSED(tag)
    return std::unexpected(error_code::not_implemented);
#endif
}

}  // namespace sak

#include "sak/network_transfer_security.h"
#include "sak/logger.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace sak {

QByteArray TransferSecurityManager::generateRandomBytes(int size) {
    QByteArray result(size, 0);
#ifdef _WIN32
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_RNG_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }

    if (BCryptGenRandom(hAlg, reinterpret_cast<PUCHAR>(result.data()), size, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }

    BCryptCloseAlgorithmProvider(hAlg, 0);
    return result;
#else
    Q_UNUSED(size)
    return {};
#endif
}

std::expected<QByteArray, error_code> TransferSecurityManager::deriveKey(
    const QString& passphrase,
    const QByteArray& salt,
    int iterations,
    int key_length) {
#ifdef _WIN32
    if (passphrase.isEmpty() || salt.isEmpty()) {
        return std::unexpected(error_code::invalid_argument);
    }

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
        return std::unexpected(error_code::crypto_error);
    }

    QByteArray pwd_bytes = passphrase.toUtf8();
    QByteArray derived_key(key_length, 0);

    NTSTATUS status = BCryptDeriveKeyPBKDF2(
        hAlg,
        reinterpret_cast<PUCHAR>(pwd_bytes.data()), pwd_bytes.size(),
        reinterpret_cast<PUCHAR>(const_cast<char*>(salt.data())), salt.size(),
        iterations,
        reinterpret_cast<PUCHAR>(derived_key.data()), key_length,
        0);

    BCryptCloseAlgorithmProvider(hAlg, 0);

    if (status != 0) {
        return std::unexpected(error_code::crypto_error);
    }

    return derived_key;
#else
    Q_UNUSED(passphrase)
    Q_UNUSED(salt)
    Q_UNUSED(iterations)
    Q_UNUSED(key_length)
    return std::unexpected(error_code::not_implemented);
#endif
}

std::expected<EncryptedPayload, error_code> TransferSecurityManager::encryptAesGcm(
    const QByteArray& plaintext,
    const QByteArray& key,
    const QByteArray& aad) {
#ifdef _WIN32
    EncryptedPayload payload;

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) {
        return std::unexpected(error_code::crypto_error);
    }

    if (BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return std::unexpected(error_code::crypto_error);
    }

    if (BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0,
                                   reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
                                   key.size(), 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return std::unexpected(error_code::crypto_error);
    }

    payload.iv = generateRandomBytes(12);
    payload.tag.resize(16);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth_info;
    BCRYPT_INIT_AUTH_MODE_INFO(auth_info);
    auth_info.pbNonce = reinterpret_cast<PUCHAR>(payload.iv.data());
    auth_info.cbNonce = payload.iv.size();
    auth_info.pbTag = reinterpret_cast<PUCHAR>(payload.tag.data());
    auth_info.cbTag = payload.tag.size();

    QByteArray aad_copy = aad;
    if (!aad_copy.isEmpty()) {
        auth_info.pbAuthData = reinterpret_cast<PUCHAR>(aad_copy.data());
        auth_info.cbAuthData = aad_copy.size();
    }

    DWORD ciphertext_len = 0;
    NTSTATUS status = BCryptEncrypt(
        hKey,
        reinterpret_cast<PUCHAR>(const_cast<char*>(plaintext.data())),
        plaintext.size(),
        &auth_info,
        nullptr,
        0,
        nullptr,
        0,
        &ciphertext_len,
        0);

    if (status != 0) {
        BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return std::unexpected(error_code::crypto_error);
    }

    payload.ciphertext.resize(ciphertext_len);
    status = BCryptEncrypt(
        hKey,
        reinterpret_cast<PUCHAR>(const_cast<char*>(plaintext.data())),
        plaintext.size(),
        &auth_info,
        nullptr,
        0,
        reinterpret_cast<PUCHAR>(payload.ciphertext.data()),
        ciphertext_len,
        &ciphertext_len,
        0);

    if (status != 0) {
        BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return std::unexpected(error_code::crypto_error);
    }

    payload.ciphertext.resize(ciphertext_len);

    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    return payload;
#else
    Q_UNUSED(plaintext)
    Q_UNUSED(key)
    Q_UNUSED(aad)
    return std::unexpected(error_code::not_implemented);
#endif
}

std::expected<QByteArray, error_code> TransferSecurityManager::decryptAesGcm(
    const EncryptedPayload& payload,
    const QByteArray& key,
    const QByteArray& aad) {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) {
        return std::unexpected(error_code::crypto_error);
    }

    if (BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return std::unexpected(error_code::crypto_error);
    }

    if (BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0,
                                   reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
                                   key.size(), 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return std::unexpected(error_code::crypto_error);
    }

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth_info;
    BCRYPT_INIT_AUTH_MODE_INFO(auth_info);
    auth_info.pbNonce = reinterpret_cast<PUCHAR>(const_cast<char*>(payload.iv.data()));
    auth_info.cbNonce = payload.iv.size();
    auth_info.pbTag = reinterpret_cast<PUCHAR>(const_cast<char*>(payload.tag.data()));
    auth_info.cbTag = payload.tag.size();

    QByteArray aad_copy = aad;
    if (!aad_copy.isEmpty()) {
        auth_info.pbAuthData = reinterpret_cast<PUCHAR>(aad_copy.data());
        auth_info.cbAuthData = aad_copy.size();
    }

    DWORD plaintext_len = 0;
    NTSTATUS status = BCryptDecrypt(
        hKey,
        reinterpret_cast<PUCHAR>(const_cast<char*>(payload.ciphertext.data())),
        payload.ciphertext.size(),
        &auth_info,
        nullptr,
        0,
        nullptr,
        0,
        &plaintext_len,
        0);

    if (status != 0) {
        BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return std::unexpected(error_code::decrypt_failed);
    }

    QByteArray plaintext(plaintext_len, 0);
    status = BCryptDecrypt(
        hKey,
        reinterpret_cast<PUCHAR>(const_cast<char*>(payload.ciphertext.data())),
        payload.ciphertext.size(),
        &auth_info,
        nullptr,
        0,
        reinterpret_cast<PUCHAR>(plaintext.data()),
        plaintext_len,
        &plaintext_len,
        0);

    if (status != 0) {
        BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return std::unexpected(error_code::decrypt_failed);
    }

    plaintext.resize(plaintext_len);

    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    return plaintext;
#else
    Q_UNUSED(payload)
    Q_UNUSED(key)
    Q_UNUSED(aad)
    return std::unexpected(error_code::not_implemented);
#endif
}

} // namespace sak

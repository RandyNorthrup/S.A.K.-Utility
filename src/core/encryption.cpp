/// @file encryption.cpp
/// @brief AES-256 encryption implementation using Windows BCrypt API

#include "sak/encryption.h"
#include "sak/logger.h"
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
        return {};
    }
    
    if (BCryptGenRandom(hAlg, reinterpret_cast<PUCHAR>(result.data()), size, 0) != 0) {
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
        return {};
    }
    
    return derived_key;
}

/// @brief AES-256-CBC encryption
QByteArray aes_encrypt(const QByteArray& plaintext, const QByteArray& key, const QByteArray& iv) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    
    if (BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE, 
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)), 
                          sizeof(BCRYPT_CHAIN_MODE_CBC), 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    
    if (BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0, 
                                    reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())), 
                                    key.size(), 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    
    DWORD ciphertext_len = 0;
    QByteArray iv_copy = iv; // BCrypt modifies IV
    
    if (BCryptEncrypt(hKey, reinterpret_cast<PUCHAR>(const_cast<char*>(plaintext.data())), 
                      plaintext.size(), nullptr, 
                      reinterpret_cast<PUCHAR>(iv_copy.data()), iv_copy.size(),
                      nullptr, 0, &ciphertext_len, BCRYPT_BLOCK_PADDING) != 0) {
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
        return {};
    }
    
    if (BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                          sizeof(BCRYPT_CHAIN_MODE_CBC), 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    
    if (BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0,
                                    reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
                                    key.size(), 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    
    DWORD plaintext_len = 0;
    QByteArray iv_copy = iv; // BCrypt modifies IV
    
    if (BCryptDecrypt(hKey, reinterpret_cast<PUCHAR>(const_cast<char*>(ciphertext.data())),
                      ciphertext.size(), nullptr,
                      reinterpret_cast<PUCHAR>(iv_copy.data()), iv_copy.size(),
                      nullptr, 0, &plaintext_len, BCRYPT_BLOCK_PADDING) != 0) {
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

auto encrypt_data(
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
        log_error("Failed to generate random bytes for encryption");
        return std::unexpected(error_code::crypto_error);
    }
    
    // Derive key from password
    QByteArray key = derive_key(password, salt, params.iterations, params.key_size);
    if (key.isEmpty()) {
        log_error("Failed to derive encryption key");
        return std::unexpected(error_code::crypto_error);
    }
    
    // Encrypt data
    QByteArray ciphertext = aes_encrypt(data, key, iv);
    if (ciphertext.isEmpty()) {
        log_error("AES encryption failed");
        return std::unexpected(error_code::crypto_error);
    }
    
    // Format: [salt][IV][ciphertext]
    QByteArray result;
    result.append(salt);
    result.append(iv);
    result.append(ciphertext);
    
    log_debug("Encryption", std::format("Encrypted {} bytes to {} bytes", 
                                         data.size(), result.size()));
    
    return result;
#else
    return std::unexpected(error_code::not_implemented);
#endif
}

auto decrypt_data(
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
        log_error("Encrypted data too small - corrupted or invalid");
        return std::unexpected(error_code::invalid_format);
    }
    
    // Extract salt and IV
    QByteArray salt = encrypted_data.left(params.salt_size);
    QByteArray iv = encrypted_data.mid(params.salt_size, params.iv_size);
    QByteArray ciphertext = encrypted_data.mid(header_size);
    
    // Derive key from password
    QByteArray key = derive_key(password, salt, params.iterations, params.key_size);
    if (key.isEmpty()) {
        log_error("Failed to derive decryption key");
        return std::unexpected(error_code::crypto_error);
    }
    
    // Decrypt data
    QByteArray plaintext = aes_decrypt(ciphertext, key, iv);
    if (plaintext.isEmpty()) {
        log_error("AES decryption failed - wrong password or corrupted data");
        return std::unexpected(error_code::decrypt_failed);
    }
    
    log_debug("Decryption", std::format("Decrypted {} bytes to {} bytes",
                                         encrypted_data.size(), plaintext.size()));
    
    return plaintext;
#else
    return std::unexpected(error_code::not_implemented);
#endif
}

auto encrypt_file(
    const QString& file_path,
    const QString& password,
    const EncryptionParams& params
) -> std::expected<void, error_code> {
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly)) {
        log_error(std::format("Cannot open file for encryption: {}", file_path.toStdString()));
        return std::unexpected(error_code::file_not_found);
    }
    
    QByteArray data = file.readAll();
    file.close();
    
    auto encrypted = encrypt_data(data, password, params);
    if (!encrypted) {
        return std::unexpected(encrypted.error());
    }
    
    // Write to temp file first for atomic replacement (prevents data loss on crash)
    QString tempPath = file_path + ".tmp";
    QFile tempFile(tempPath);
    if (!tempFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        log_error(std::format("Cannot create temp file for encryption: {}", tempPath.toStdString()));
        return std::unexpected(error_code::file_write_error);
    }
    
    if (tempFile.write(*encrypted) != encrypted->size()) {
        log_error(std::format("Incomplete write to temp file: {}", tempPath.toStdString()));
        tempFile.close();
        QFile::remove(tempPath);
        return std::unexpected(error_code::file_write_error);
    }
    tempFile.close();
    
    // Atomically replace original with encrypted version
    QFile::remove(file_path);
    if (!QFile::rename(tempPath, file_path)) {
        log_error(std::format("Cannot replace original file: {}", file_path.toStdString()));
        return std::unexpected(error_code::file_write_error);
    }
    
    log_info(std::format("Encrypted file: {}", file_path.toStdString()));
    return {};
}

auto decrypt_file(
    const QString& file_path,
    const QString& password,
    const EncryptionParams& params
) -> std::expected<void, error_code> {
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly)) {
        log_error(std::format("Cannot open file for decryption: {}", file_path.toStdString()));
        return std::unexpected(error_code::file_not_found);
    }
    
    QByteArray encrypted_data = file.readAll();
    file.close();
    
    auto decrypted = decrypt_data(encrypted_data, password, params);
    if (!decrypted) {
        return std::unexpected(decrypted.error());
    }
    
    // Write to temp file first for atomic replacement (prevents data loss on crash)
    QString tempPath = file_path + ".tmp";
    QFile tempFile(tempPath);
    if (!tempFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        log_error(std::format("Cannot create temp file for decryption: {}", tempPath.toStdString()));
        return std::unexpected(error_code::file_write_error);
    }
    
    if (tempFile.write(*decrypted) != decrypted->size()) {
        log_error(std::format("Incomplete write to temp file: {}", tempPath.toStdString()));
        tempFile.close();
        QFile::remove(tempPath);
        return std::unexpected(error_code::file_write_error);
    }
    tempFile.close();
    
    // Atomically replace original with decrypted version
    QFile::remove(file_path);
    if (!QFile::rename(tempPath, file_path)) {
        log_error(std::format("Cannot replace original file: {}", file_path.toStdString()));
        return std::unexpected(error_code::file_write_error);
    }
    
    log_info(std::format("Decrypted file: {}", file_path.toStdString()));
    return {};
}

} // namespace sak

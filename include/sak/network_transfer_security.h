#pragma once

#include <QByteArray>
#include <QString>
#include <expected>

#include "sak/error_codes.h"

namespace sak {

struct EncryptedPayload {
    QByteArray iv;
    QByteArray ciphertext;
    QByteArray tag;
};

class TransferSecurityManager {
public:
    static QByteArray generateRandomBytes(int size);

    static std::expected<QByteArray, error_code> deriveKey(
        const QString& passphrase,
        const QByteArray& salt,
        int iterations = 200000,
        int key_length = 32);

    static std::expected<EncryptedPayload, error_code> encryptAesGcm(
        const QByteArray& plaintext,
        const QByteArray& key,
        const QByteArray& aad = {});

    static std::expected<QByteArray, error_code> decryptAesGcm(
        const EncryptedPayload& payload,
        const QByteArray& key,
        const QByteArray& aad = {});
};

} // namespace sak

// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_encryption.cpp
/// @brief Unit tests for AES-256 encryption/decryption via BCrypt API

#include "sak/encryption.h"
#include "sak/error_codes.h"

#include <QByteArray>
#include <QString>
#include <QtTest/QtTest>

class EncryptionTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void roundTrip_basicString();
    void roundTrip_binaryData();
    void roundTrip_emptyData();
    void roundTrip_largeData();
    void roundTrip_nonAsciiPassword();
    void wrongPassword_failsDecrypt();
    void emptyPassword_rejected();
    void truncatedCiphertext_failsDecrypt();
    void corruptedCiphertext_failsDecrypt();
    void tamperedIV_failsDecrypt();
    void tamperedTag_failsDecrypt();
    void customParams_roundTrip();
    void invalidParams_rejected();
    void differentPasswords_produceDifferentCiphertext();
    void sameInput_producesDifferentCiphertext();
};

// ============================================================================
// Round-Trip Tests
// ============================================================================

void EncryptionTests::roundTrip_basicString() {
    const QByteArray original = "Hello, World! This is a test.";
    const QString password = "test_password_123";

    auto encrypted = sak::encryptData(original, password);
    QVERIFY2(encrypted.has_value(), "encryptData should succeed");
    QVERIFY(encrypted.value() != original);
    QVERIFY(!encrypted.value().isEmpty());

    auto decrypted = sak::decryptData(encrypted.value(), password);
    QVERIFY2(decrypted.has_value(), "decryptData should succeed");
    QCOMPARE(decrypted.value(), original);
}

void EncryptionTests::roundTrip_binaryData() {
    QByteArray original;
    original.resize(256);
    for (int i = 0; i < 256; ++i) {
        original[i] = static_cast<char>(i);
    }

    const QString password = "binary_test_pw";

    auto encrypted = sak::encryptData(original, password);
    QVERIFY(encrypted.has_value());

    auto decrypted = sak::decryptData(encrypted.value(), password);
    QVERIFY(decrypted.has_value());
    QCOMPARE(decrypted.value(), original);
}

void EncryptionTests::roundTrip_emptyData() {
    const QByteArray original;
    const QString password = "empty_data_password";

    auto encrypted = sak::encryptData(original, password);
    QVERIFY(encrypted.has_value());

    auto decrypted = sak::decryptData(encrypted.value(), password);
    // B11-12: empty data MUST round-trip to empty. Encrypting empty produces a full CBC
    // padding block; the HMAC verifies, and the decrypted empty plaintext must not be
    // mistaken for a decryption failure.
    QVERIFY(decrypted.has_value());
    QCOMPARE(decrypted.value(), original);
    QVERIFY(decrypted.value().isEmpty());
}

void EncryptionTests::roundTrip_largeData() {
    // 1 MB of data
    QByteArray original(1024 * 1024, 'X');
    for (int i = 0; i < original.size(); ++i) {
        original[i] = static_cast<char>(i % 256);
    }

    const QString password = "large_data_password";

    auto encrypted = sak::encryptData(original, password);
    QVERIFY(encrypted.has_value());
    QVERIFY(encrypted.value().size() > original.size());  // Ciphertext includes salt + IV + padding

    auto decrypted = sak::decryptData(encrypted.value(), password);
    QVERIFY(decrypted.has_value());
    QCOMPARE(decrypted.value(), original);
}

void EncryptionTests::roundTrip_nonAsciiPassword() {
    const QByteArray original = "Data with unicode password";
    const QString password = QString::fromUtf8(
        u8"Ð¿Ð°Ñ€Ð¾Ð»ÑŒå¯†ç ãƒ‘ã‚¹ãƒ¯ãƒ¼ãƒ‰");

    auto encrypted = sak::encryptData(original, password);
    QVERIFY(encrypted.has_value());

    auto decrypted = sak::decryptData(encrypted.value(), password);
    QVERIFY(decrypted.has_value());
    QCOMPARE(decrypted.value(), original);
}

// ============================================================================
// Error Path Tests
// ============================================================================

void EncryptionTests::wrongPassword_failsDecrypt() {
    const QByteArray original = "Secret data";
    const QString correctPassword = "correct_password";
    const QString wrongPassword = "wrong_password";

    auto encrypted = sak::encryptData(original, correctPassword);
    QVERIFY(encrypted.has_value());

    auto decrypted = sak::decryptData(encrypted.value(), wrongPassword);
    // Wrong password should fail -- either returns error or garbage data
    // BCrypt may return decrypt_failed or the padding check fails
    if (decrypted.has_value()) {
        // If it doesn't error, at least the data should differ
        QVERIFY(decrypted.value() != original);
    }
}

void EncryptionTests::emptyPassword_rejected() {
    const QByteArray data = "Some data";

    auto result = sak::encryptData(data, QString());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(), sak::error_code::invalid_argument);
}

void EncryptionTests::truncatedCiphertext_failsDecrypt() {
    const QByteArray original = "Test data for truncation";
    const QString password = "trunc_password";

    auto encrypted = sak::encryptData(original, password);
    QVERIFY(encrypted.has_value());

    // Truncate to less than salt_size + iv_size (32 + 16 = 48 bytes)
    QByteArray truncated = encrypted.value().left(10);
    auto decrypted = sak::decryptData(truncated, password);
    QVERIFY(!decrypted.has_value());
}

void EncryptionTests::corruptedCiphertext_failsDecrypt() {
    const QByteArray original = "Test data for corruption";
    const QString password = "corrupt_password";

    auto encrypted = sak::encryptData(original, password);
    QVERIFY(encrypted.has_value());

    // Corrupt the ciphertext portion (after salt + IV)
    QByteArray corrupted = encrypted.value();
    if (corrupted.size() > 50) {
        corrupted[50] = static_cast<char>(~corrupted[50]);
    }

    auto decrypted = sak::decryptData(corrupted, password);
    // Should either fail or return different data
    if (decrypted.has_value()) {
        QVERIFY(decrypted.value() != original);
    }
}

void EncryptionTests::tamperedIV_failsDecrypt() {
    const QByteArray original = "authentic secret";
    const QString password = "iv_tamper_pw";

    auto encrypted = sak::encryptData(original, password);
    QVERIFY(encrypted.has_value());

    // Flip one bit inside the IV region (right after the salt): Encrypt-then-MAC must reject it.
    QByteArray tampered = encrypted.value();
    tampered[sak::kEncryptionSaltBytes] = static_cast<char>(~tampered[sak::kEncryptionSaltBytes]);
    auto decrypted = sak::decryptData(tampered, password);
    QVERIFY(!decrypted.has_value());
    QCOMPARE(decrypted.error(), sak::error_code::decrypt_failed);
}

void EncryptionTests::tamperedTag_failsDecrypt() {
    const QByteArray original = "authentic secret";
    const QString password = "tag_tamper_pw";

    auto encrypted = sak::encryptData(original, password);
    QVERIFY(encrypted.has_value());

    // Flip one bit in the trailing HMAC tag: verification must fail closed.
    QByteArray tampered = encrypted.value();
    tampered[tampered.size() - 1] = static_cast<char>(~tampered[tampered.size() - 1]);
    auto decrypted = sak::decryptData(tampered, password);
    QVERIFY(!decrypted.has_value());
    QCOMPARE(decrypted.error(), sak::error_code::decrypt_failed);
}

// ============================================================================
// Edge Case Tests
// ============================================================================

void EncryptionTests::customParams_roundTrip() {
    const QByteArray original = "Custom params test";
    const QString password = "custom_params_pw";

    sak::EncryptionParams params;
    params.iterations = 10'000;  // Fewer iterations for speed in test

    auto encrypted = sak::encryptData(original, password, params);
    QVERIFY(encrypted.has_value());

    auto decrypted = sak::decryptData(encrypted.value(), password, params);
    QVERIFY(decrypted.has_value());
    QCOMPARE(decrypted.value(), original);
}

void EncryptionTests::invalidParams_rejected() {
    // B11-12: a hostile/broken EncryptionParams (non-AES key size, wrong IV length,
    // too-small salt, or non-positive iterations) must be rejected up front, not fed
    // into the KDF/cipher where it would derive weak keys or misbehave.
    const QByteArray data = "params validation";
    const QString password = "pw_params";
    const auto expectRejected = [&](sak::EncryptionParams p) {
        const auto enc = sak::encryptData(data, password, p);
        QVERIFY(!enc.has_value());
        QCOMPARE(enc.error(), sak::error_code::invalid_argument);
    };
    sak::EncryptionParams badKey;
    badKey.key_size = 20;  // not 16/24/32
    expectRejected(badKey);
    sak::EncryptionParams badIv;
    badIv.iv_size = 8;  // not one AES block
    expectRejected(badIv);
    sak::EncryptionParams badSalt;
    badSalt.salt_size = 4;  // too small
    expectRejected(badSalt);
    sak::EncryptionParams badIter;
    badIter.iterations = 0;  // non-positive
    expectRejected(badIter);
}

void EncryptionTests::differentPasswords_produceDifferentCiphertext() {
    const QByteArray original = "Same data, different keys";

    auto enc1 = sak::encryptData(original, "password_one");
    auto enc2 = sak::encryptData(original, "password_two");

    QVERIFY(enc1.has_value());
    QVERIFY(enc2.has_value());
    QVERIFY(enc1.value() != enc2.value());
}

void EncryptionTests::sameInput_producesDifferentCiphertext() {
    // Due to random salt/IV, encrypting the same data with the same password
    // should produce different ciphertext each time
    const QByteArray original = "Same input twice";
    const QString password = "same_password";

    auto enc1 = sak::encryptData(original, password);
    auto enc2 = sak::encryptData(original, password);

    QVERIFY(enc1.has_value());
    QVERIFY(enc2.has_value());
    QVERIFY(enc1.value() != enc2.value());
}

QTEST_GUILESS_MAIN(EncryptionTests)
#include "test_encryption.moc"

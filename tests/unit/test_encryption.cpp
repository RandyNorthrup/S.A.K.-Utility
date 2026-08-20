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

    // Streaming API (StreamEncryptor / StreamDecryptor)
    void stream_roundTrip_data();
    void stream_roundTrip();
    void stream_outputIsReadableByDecryptData();
    void stream_encryptDataOutputIsReadableByStream();
    void stream_chunkBoundariesDoNotChangeCiphertext();
    void stream_wrongPassword_failsAtFinish();
    void stream_tamperedCiphertext_failsAtFinish();
    void stream_tamperedHeader_failsAtFinish();
    void stream_truncatedStream_failsAtFinish();
    void stream_emptyPassword_rejected();
    void stream_badHeaderSize_rejected();
    void stream_updateAfterFinish_rejected();
};

// ============================================================================
// Streaming helpers
// ============================================================================

namespace {

/// Drive StreamEncryptor over @p plain in @p chunk_size slices and return the whole
/// wire image: header || ciphertext || tag.
QByteArray streamEncrypt(const QByteArray& plain,
                         const QString& password,
                         qsizetype chunk_size,
                         bool* ok) {
    *ok = false;
    auto enc = sak::StreamEncryptor::create(password);
    if (!enc.has_value()) {
        return {};
    }
    QByteArray wire = enc.value()->header();
    for (qsizetype offset = 0; offset < plain.size(); offset += chunk_size) {
        auto part = enc.value()->update(plain.mid(offset, chunk_size));
        if (!part.has_value()) {
            return {};
        }
        wire.append(part.value());
    }
    auto tail = enc.value()->finish();
    if (!tail.has_value()) {
        return {};
    }
    wire.append(tail.value());
    *ok = true;
    return wire;
}

/// Inverse of streamEncrypt. Sets @p ok only when finish() authenticates the stream.
QByteArray streamDecrypt(const QByteArray& wire,
                         const QString& password,
                         qsizetype chunk_size,
                         bool* ok) {
    *ok = false;
    const int header_size = sak::StreamDecryptor::headerSize();
    if (wire.size() < header_size + sak::kEncryptionMacBytes) {
        return {};
    }
    auto dec = sak::StreamDecryptor::create(password, wire.left(header_size));
    if (!dec.has_value()) {
        return {};
    }
    // The trailing tag is not ciphertext and must never be fed to update().
    const qsizetype body_end = wire.size() - sak::kEncryptionMacBytes;
    QByteArray plain;
    for (qsizetype offset = header_size; offset < body_end; offset += chunk_size) {
        const qsizetype take = qMin(chunk_size, body_end - offset);
        auto part = dec.value()->update(wire.mid(offset, take));
        if (!part.has_value()) {
            return {};
        }
        plain.append(part.value());
    }
    auto tail = dec.value()->finish(wire.mid(body_end));
    if (!tail.has_value()) {
        return {};
    }
    plain.append(tail.value());
    *ok = true;
    return plain;
}

QByteArray patternBytes(qsizetype size) {
    QByteArray data(size, 0);
    for (qsizetype i = 0; i < size; ++i) {
        data[i] = static_cast<char>((i * 31 + 7) & 0xFF);
    }
    return data;
}

}  // namespace

// ============================================================================
// Round-Trip Tests
// ============================================================================

void EncryptionTests::roundTrip_basicString() {
    const QByteArray original = "Hello, World! This is a test.";
    const QString password = "test_password_123";

    auto encrypted = sak::encryptData(original, password);
    QVERIFY2(encrypted.has_value(), "encryptData should succeed");
    QVERIFY(encrypted.value() != original);
    // Exact wire size: 32 salt + 16 IV + 32 PKCS7-padded ciphertext (29-byte plaintext -> next
    // 16-multiple) + 32 HMAC tag = 112. `!isEmpty()` would survive a dropped salt/IV header or tag.
    QCOMPARE(encrypted.value().size(),
             qsizetype(sak::kEncryptionSaltBytes + sak::kAesBlockBytes + 32 +
                       sak::kEncryptionMacBytes));

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
    // 1 MB is an exact block multiple, so PKCS7 adds one full 16-byte padding block. Fixed 96-byte
    // overhead = 32 salt + 16 IV + 16 padding + 32 tag. `>` would survive a dropped tag or salt.
    QCOMPARE(encrypted.value().size(),
             original.size() + qsizetype(sak::kEncryptionSaltBytes + sak::kAesBlockBytes +
                                         sak::kAesBlockBytes + sak::kEncryptionMacBytes));

    auto decrypted = sak::decryptData(encrypted.value(), password);
    QVERIFY(decrypted.has_value());
    QCOMPARE(decrypted.value(), original);
}

void EncryptionTests::roundTrip_nonAsciiPassword() {
    const QByteArray original = "Data with unicode password";
    const QString password = QString::fromUtf8(
        u8"\u043F\u0430\u0440\u043E\u043B\u044C"  // parol (Cyrillic)
        u8"\u5BC6\u7801"                          // mima (Chinese)
        u8"\u30D1\u30B9\u30EF\u30FC\u30C9");      // pasuwaado (Japanese)

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
    // decryptData is encrypt-then-MAC with a password-derived MAC key, so a wrong password ALWAYS
    // fails the constant-time tag compare and fails closed. The old `if (has_value())` branch was
    // dead, so it never actually asserted the fail-closed contract (a fail-open regression that
    // returned garbage plaintext would have passed). Pin it, matching the tampered-IV/tag siblings.
    QVERIFY(!decrypted.has_value());
    QCOMPARE(decrypted.error(), sak::error_code::decrypt_failed);
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
    // Byte 50 lands in the authenticated ciphertext body (salt 32 + IV 16 = 48), which the HMAC
    // covers, so decrypt fails closed. The old `if (has_value())` branch was dead and never
    // asserted the contract; pin it like the tampered-IV/tag siblings.
    QVERIFY(!decrypted.has_value());
    QCOMPARE(decrypted.error(), sak::error_code::decrypt_failed);
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

// ============================================================================
// Streaming API
// ============================================================================

void EncryptionTests::stream_roundTrip_data() {
    QTest::addColumn<qsizetype>("payload_size");
    QTest::addColumn<qsizetype>("chunk_size");

    // Sizes chosen around the 16-byte AES block so the internal sub-block buffering,
    // the exact-multiple case, and the always-a-full-padding-block case all execute.
    QTest::newRow("empty") << qsizetype(0) << qsizetype(64);
    QTest::newRow("one byte") << qsizetype(1) << qsizetype(64);
    QTest::newRow("under one block") << qsizetype(15) << qsizetype(64);
    QTest::newRow("exactly one block") << qsizetype(16) << qsizetype(64);
    QTest::newRow("one block plus one") << qsizetype(17) << qsizetype(64);
    QTest::newRow("many blocks, tiny chunks") << qsizetype(1024) << qsizetype(1);
    QTest::newRow("many blocks, odd chunks") << qsizetype(4096) << qsizetype(7);
    QTest::newRow("block-aligned chunks") << qsizetype(4096) << qsizetype(16);
    QTest::newRow("chunk larger than payload") << qsizetype(100) << qsizetype(1 << 20);
    QTest::newRow("1 MiB") << qsizetype(1 << 20) << qsizetype(64 * 1024);
}

void EncryptionTests::stream_roundTrip() {
    QFETCH(qsizetype, payload_size);
    QFETCH(qsizetype, chunk_size);

    const QByteArray original = patternBytes(payload_size);
    const QString password = "stream_pw_correct_horse";

    bool ok = false;
    const QByteArray wire = streamEncrypt(original, password, chunk_size, &ok);
    QVERIFY2(ok, "streaming encryption should succeed");
    // Exact framed size: 48-byte header (32 salt + 16 IV) + PKCS7-padded ciphertext (CBC always
    // emits a full extra block) + 32-byte tag. `>` would survive a dropped tag or short padding.
    const qsizetype padded = ((original.size() / sak::kAesBlockBytes) + 1) * sak::kAesBlockBytes;
    QCOMPARE(wire.size(),
             qsizetype(sak::kEncryptionSaltBytes + sak::kAesBlockBytes) + padded +
                 sak::kEncryptionMacBytes);

    const QByteArray recovered = streamDecrypt(wire, password, chunk_size, &ok);
    QVERIFY2(ok, "streaming decryption should authenticate and succeed");
    QCOMPARE(recovered, original);
}

void EncryptionTests::stream_outputIsReadableByDecryptData() {
    // The streaming wire image is the SAME layout as encryptData: [salt][IV][ct][tag].
    // Proving the in-memory reader accepts a streamed writer's output is what lets a
    // backup written in chunks be opened by either path.
    const QByteArray original = patternBytes(5000);
    const QString password = "interop_pw";

    bool ok = false;
    const QByteArray wire = streamEncrypt(original, password, 333, &ok);
    QVERIFY(ok);

    auto decrypted = sak::decryptData(wire, password);
    QVERIFY2(decrypted.has_value(), "decryptData must read a streamed image");
    QCOMPARE(decrypted.value(), original);
}

void EncryptionTests::stream_encryptDataOutputIsReadableByStream() {
    // ...and the reverse, so neither writer strands data the other cannot open.
    const QByteArray original = patternBytes(5000);
    const QString password = "interop_pw_2";

    auto encrypted = sak::encryptData(original, password);
    QVERIFY(encrypted.has_value());

    bool ok = false;
    const QByteArray recovered = streamDecrypt(encrypted.value(), password, 512, &ok);
    QVERIFY2(ok, "StreamDecryptor must read an encryptData image");
    QCOMPARE(recovered, original);
}

void EncryptionTests::stream_chunkBoundariesDoNotChangeCiphertext() {
    // The ciphertext must depend only on key, IV and plaintext - never on how the caller
    // happened to slice its writes. Each run picks a fresh random salt and IV, so the two
    // wire images cannot be compared byte for byte; instead each is decrypted with the
    // OTHER run's chunk size, which fails if slicing leaked into the stream state.
    const QByteArray original = patternBytes(3000);
    const QString password = "slicing_pw";

    bool ok = false;
    const QByteArray wire_small = streamEncrypt(original, password, 3, &ok);
    QVERIFY(ok);
    const QByteArray wire_large = streamEncrypt(original, password, 4096, &ok);
    QVERIFY(ok);
    QCOMPARE(wire_small.size(), wire_large.size());

    const QByteArray from_small = streamDecrypt(wire_small, password, 4096, &ok);
    QVERIFY(ok);
    const QByteArray from_large = streamDecrypt(wire_large, password, 3, &ok);
    QVERIFY(ok);
    QCOMPARE(from_small, original);
    QCOMPARE(from_large, original);
}

void EncryptionTests::stream_wrongPassword_failsAtFinish() {
    const QByteArray original = patternBytes(2048);
    bool ok = false;
    const QByteArray wire = streamEncrypt(original, "right_password", 256, &ok);
    QVERIFY(ok);

    const QByteArray recovered = streamDecrypt(wire, "wrong_password", 256, &ok);
    QVERIFY2(!ok, "a wrong password must fail authentication");
    QVERIFY(recovered.isEmpty());
}

void EncryptionTests::stream_tamperedCiphertext_failsAtFinish() {
    const QByteArray original = patternBytes(2048);
    const QString password = "tamper_pw";
    bool ok = false;
    QByteArray wire = streamEncrypt(original, password, 256, &ok);
    QVERIFY(ok);

    // Flip a bit in the middle of the ciphertext body.
    const qsizetype target = sak::StreamDecryptor::headerSize() + 100;
    wire[target] = static_cast<char>(wire[target] ^ 0x01);

    streamDecrypt(wire, password, 256, &ok);
    QVERIFY2(!ok, "an edited ciphertext must fail authentication");
}

void EncryptionTests::stream_tamperedHeader_failsAtFinish() {
    // The tag covers the header, so swapping the IV must be caught rather than
    // silently corrupting only the first block.
    const QByteArray original = patternBytes(2048);
    const QString password = "header_pw";
    bool ok = false;
    QByteArray wire = streamEncrypt(original, password, 256, &ok);
    QVERIFY(ok);

    wire[sak::kEncryptionSaltBytes] = static_cast<char>(wire[sak::kEncryptionSaltBytes] ^ 0x80);

    streamDecrypt(wire, password, 256, &ok);
    QVERIFY2(!ok, "an edited header must fail authentication");
}

void EncryptionTests::stream_truncatedStream_failsAtFinish() {
    const QByteArray original = patternBytes(4096);
    const QString password = "truncate_pw";
    bool ok = false;
    const QByteArray wire = streamEncrypt(original, password, 512, &ok);
    QVERIFY(ok);

    // Drop a whole block from the body but keep the real tag, i.e. exactly what a
    // truncated copy of a backup file looks like.
    QByteArray cut = wire.left(wire.size() - sak::kEncryptionMacBytes - sak::kAesBlockBytes);
    cut.append(wire.right(sak::kEncryptionMacBytes));

    streamDecrypt(cut, password, 512, &ok);
    QVERIFY2(!ok, "a truncated stream must fail authentication");
}

void EncryptionTests::stream_emptyPassword_rejected() {
    auto enc = sak::StreamEncryptor::create(QString());
    QVERIFY(!enc.has_value());
    QCOMPARE(enc.error(), sak::error_code::invalid_argument);

    const QByteArray header(sak::StreamDecryptor::headerSize(), 0);
    auto dec = sak::StreamDecryptor::create(QString(), header);
    QVERIFY(!dec.has_value());
    QCOMPARE(dec.error(), sak::error_code::invalid_argument);
}

void EncryptionTests::stream_badHeaderSize_rejected() {
    const QByteArray short_header(sak::StreamDecryptor::headerSize() - 1, 0);
    auto dec = sak::StreamDecryptor::create("pw", short_header);
    QVERIFY(!dec.has_value());
    QCOMPARE(dec.error(), sak::error_code::invalid_format);
}

void EncryptionTests::stream_updateAfterFinish_rejected() {
    auto enc = sak::StreamEncryptor::create("pw");
    QVERIFY(enc.has_value());
    auto tail = enc.value()->finish();
    QVERIFY(tail.has_value());

    auto late = enc.value()->update(QByteArray("more"));
    QVERIFY2(!late.has_value(), "update() after finish() must be refused");
    QCOMPARE(late.error(), sak::error_code::invalid_argument);

    auto twice = enc.value()->finish();
    QVERIFY2(!twice.has_value(), "finish() twice must be refused");
}

QTEST_GUILESS_MAIN(EncryptionTests)
#include "test_encryption.moc"

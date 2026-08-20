// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_backup_file_codec.cpp
/// @brief Unit tests for the streaming per-file backup container.

#include "sak/backup_file_codec.h"
#include "sak/error_codes.h"

#include <QByteArray>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class BackupFileCodecTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void init();

    void roundTrip_data();
    void roundTrip();

    void compressionActuallyShrinksCompressibleData();
    void encryptedBytesDoNotContainPlaintext();
    void containerKindReflectsHowItWasWritten();
    void containerKindOfPlainFileIsNone();

    void encryptWithoutPassword_refused();
    void passThroughOptions_refused();
    void compressionLevelOutOfRange_refused();

    void restoreWithWrongPassword_failsAndLeavesNoOutput();
    void restoreWithoutPassword_failsOnEncryptedContainer();
    void tamperedPayload_failsAndLeavesNoOutput();
    void truncatedContainer_failsAndLeavesNoOutput();
    void truncatedPlainCompressedPayload_failsAndLeavesNoOutput();
    void plainCompressedDeclaredSizeMismatch_failsAndLeavesNoOutput();
    void restoreOfNonContainer_refused();

    void cancelDuringWrite_removesPartialFile();

private:
    QString sourcePath() const;
    QString storedPath() const;
    QString restoredPath() const;
    void writeSource(const QByteArray& bytes);

    QTemporaryDir m_dir;
};

// ============================================================================
// Fixture helpers
// ============================================================================

void BackupFileCodecTests::init() {
    QVERIFY(m_dir.isValid());
    // Each test starts from a clean slate so a leftover file cannot make a later
    // assertion about "no output" pass for the wrong reason.
    const QDir dir(m_dir.path());
    for (const QString& name : dir.entryList(QDir::Files)) {
        QVERIFY(QFile::remove(dir.filePath(name)));
    }
}

QString BackupFileCodecTests::sourcePath() const {
    return QDir(m_dir.path()).filePath(QStringLiteral("source.bin"));
}

QString BackupFileCodecTests::storedPath() const {
    return QDir(m_dir.path()).filePath(QStringLiteral("stored.sak"));
}

QString BackupFileCodecTests::restoredPath() const {
    return QDir(m_dir.path()).filePath(QStringLiteral("restored.bin"));
}

void BackupFileCodecTests::writeSource(const QByteArray& bytes) {
    QFile file(sourcePath());
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), static_cast<qint64>(bytes.size()));
    file.close();
}

namespace {

/// Incompressible: a fixed LCG, so failures reproduce.
QByteArray randomish(qsizetype size) {
    QByteArray data(size, 0);
    quint32 state = 0x12'34'56'78U;
    for (qsizetype i = 0; i < size; ++i) {
        state = state * 1'664'525U + 1'013'904'223U;
        data[i] = static_cast<char>((state >> 16) & 0xFF);
    }
    return data;
}

/// Highly compressible.
QByteArray repetitive(qsizetype size) {
    return QByteArray(size, 'A');
}

QByteArray fileBytes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

}  // namespace

// ============================================================================
// Round trip
// ============================================================================

void BackupFileCodecTests::roundTrip_data() {
    QTest::addColumn<qsizetype>("size");
    QTest::addColumn<bool>("compressible");
    QTest::addColumn<bool>("compress");
    QTest::addColumn<bool>("encrypt");

    // Every combination of the two transforms, across sizes that straddle the 1 MiB
    // chunk and the 16-byte cipher block.
    const struct {
        const char* label;
        qsizetype size;
        bool compressible;
    } payloads[] = {
        {"empty", 0, true},
        {"one byte", 1, true},
        {"under a block", 15, false},
        {"exact block", 16, false},
        {"small text", 5000, true},
        {"random 64k", 64 * 1024, false},
        {"over one chunk", (1 << 20) + 12'345, false},
    };

    for (const auto& payload : payloads) {
        for (const bool compress : {false, true}) {
            for (const bool encrypt : {false, true}) {
                if (!compress && !encrypt) {
                    continue;  // pass-through is refused by design; covered separately
                }
                const QString row = QStringLiteral("%1 c=%2 e=%3")
                                        .arg(QLatin1String(payload.label))
                                        .arg(compress)
                                        .arg(encrypt);
                QTest::newRow(qPrintable(row))
                    << payload.size << payload.compressible << compress << encrypt;
            }
        }
    }
}

void BackupFileCodecTests::roundTrip() {
    QFETCH(qsizetype, size);
    QFETCH(bool, compressible);
    QFETCH(bool, compress);
    QFETCH(bool, encrypt);

    const QByteArray original = compressible ? repetitive(size) : randomish(size);
    writeSource(original);

    sak::BackupCodecOptions options;
    options.compress = compress;
    options.encrypt = encrypt;
    options.password = QStringLiteral("codec_pw");

    auto written = sak::writeBackupFile(sourcePath(), storedPath(), options);
    QVERIFY2(written.has_value(), "writeBackupFile should succeed");
    QCOMPARE(written.value().plain_bytes, static_cast<qint64>(original.size()));

    auto restored = sak::readBackupFile(storedPath(), restoredPath(), options.password);
    QVERIFY2(restored.has_value(), "readBackupFile should succeed");
    QCOMPARE(restored.value().plain_bytes, static_cast<qint64>(original.size()));
    QCOMPARE(fileBytes(restoredPath()), original);
}

// ============================================================================
// The transforms actually happen
// ============================================================================

void BackupFileCodecTests::compressionActuallyShrinksCompressibleData() {
    // Guards the defect this feature exists to end: a setting that is accepted, logged,
    // and then not applied. A megabyte of one repeated byte MUST get smaller.
    const QByteArray original = repetitive(1 << 20);
    writeSource(original);

    sak::BackupCodecOptions options;
    options.compress = true;

    auto written = sak::writeBackupFile(sourcePath(), storedPath(), options);
    QVERIFY(written.has_value());
    QVERIFY2(written.value().stored_bytes * 100 < written.value().plain_bytes,
             "1 MiB of a single repeated byte must compress by more than 100x");
}

void BackupFileCodecTests::encryptedBytesDoNotContainPlaintext() {
    // The same guard for encryption: the stored file must not contain the cleartext.
    const QByteArray marker = "TOP-SECRET-PROFILE-CONTENT-MARKER";
    QByteArray original;
    for (int i = 0; i < 100; ++i) {
        original.append(marker);
    }
    writeSource(original);

    sak::BackupCodecOptions options;
    options.encrypt = true;
    options.password = QStringLiteral("secret_pw");

    auto written = sak::writeBackupFile(sourcePath(), storedPath(), options);
    QVERIFY(written.has_value());

    const QByteArray stored = fileBytes(storedPath());
    QVERIFY(!stored.isEmpty());
    QVERIFY2(!stored.contains(marker), "ciphertext must not contain the plaintext marker");
}

void BackupFileCodecTests::containerKindReflectsHowItWasWritten() {
    writeSource(repetitive(1000));

    sak::BackupCodecOptions compressed_only;
    compressed_only.compress = true;
    QVERIFY(sak::writeBackupFile(sourcePath(), storedPath(), compressed_only).has_value());
    QCOMPARE(sak::backupContainerKind(storedPath()), sak::BackupContainerKind::Plain);

    sak::BackupCodecOptions encrypted;
    encrypted.encrypt = true;
    encrypted.password = QStringLiteral("pw");
    QVERIFY(sak::writeBackupFile(sourcePath(), storedPath(), encrypted).has_value());
    QCOMPARE(sak::backupContainerKind(storedPath()), sak::BackupContainerKind::Encrypted);
}

void BackupFileCodecTests::containerKindOfPlainFileIsNone() {
    writeSource(repetitive(1000));
    QCOMPARE(sak::backupContainerKind(sourcePath()), sak::BackupContainerKind::None);
    QCOMPARE(sak::backupContainerKind(QStringLiteral("does-not-exist")),
             sak::BackupContainerKind::None);
}

// ============================================================================
// Refusals (fail closed)
// ============================================================================

void BackupFileCodecTests::encryptWithoutPassword_refused() {
    writeSource(repetitive(100));

    sak::BackupCodecOptions options;
    options.encrypt = true;  // no password

    auto written = sak::writeBackupFile(sourcePath(), storedPath(), options);
    QVERIFY2(!written.has_value(), "encryption without a password must be refused");
    QCOMPARE(written.error(), sak::error_code::invalid_argument);
    QVERIFY2(!QFile::exists(storedPath()), "nothing may be written when the request is refused");
}

void BackupFileCodecTests::passThroughOptions_refused() {
    writeSource(repetitive(100));

    const sak::BackupCodecOptions options;  // neither transform
    QVERIFY(options.isPassThrough());

    auto written = sak::writeBackupFile(sourcePath(), storedPath(), options);
    QVERIFY2(!written.has_value(), "a container that transforms nothing must be refused");
    QCOMPARE(written.error(), sak::error_code::invalid_argument);
}

void BackupFileCodecTests::compressionLevelOutOfRange_refused() {
    writeSource(repetitive(100));

    sak::BackupCodecOptions options;
    options.compress = true;
    options.compression_level = 0;  // "store" is expressed by turning compression off
    // Pin the refusal's error code: validateWriteOptions rejects an out-of-range level (1..9) with
    // invalid_argument BEFORE any I/O. `!has_value()` alone would stay green even for level 10,
    // whose write is refused downstream by zlib (corrupted_data) if the range guard is deleted --
    // so only the exact code proves the compression-level guard is the one firing.
    const auto tooLow = sak::writeBackupFile(sourcePath(), storedPath(), options);
    QVERIFY(!tooLow.has_value());
    QCOMPARE(tooLow.error(), sak::error_code::invalid_argument);

    options.compression_level = 10;
    const auto tooHigh = sak::writeBackupFile(sourcePath(), storedPath(), options);
    QVERIFY(!tooHigh.has_value());
    QCOMPARE(tooHigh.error(), sak::error_code::invalid_argument);
}

// ============================================================================
// Corruption and wrong credentials
// ============================================================================

void BackupFileCodecTests::restoreWithWrongPassword_failsAndLeavesNoOutput() {
    writeSource(randomish(10'000));

    sak::BackupCodecOptions options;
    options.encrypt = true;
    options.compress = true;
    options.password = QStringLiteral("right_pw");
    QVERIFY(sak::writeBackupFile(sourcePath(), storedPath(), options).has_value());

    auto restored = sak::readBackupFile(storedPath(), restoredPath(), QStringLiteral("wrong_pw"));
    QVERIFY2(!restored.has_value(), "a wrong password must fail");
    QVERIFY2(!QFile::exists(restoredPath()), "no plaintext may survive a failed restore");
    QVERIFY2(!QFile::exists(restoredPath() + QStringLiteral(".sakpart")),
             "the staging file must be cleaned up");
}

void BackupFileCodecTests::restoreWithoutPassword_failsOnEncryptedContainer() {
    writeSource(randomish(1000));

    sak::BackupCodecOptions options;
    options.encrypt = true;
    options.password = QStringLiteral("pw");
    QVERIFY(sak::writeBackupFile(sourcePath(), storedPath(), options).has_value());

    auto restored = sak::readBackupFile(storedPath(), restoredPath(), QString());
    QVERIFY(!restored.has_value());
    QCOMPARE(restored.error(), sak::error_code::invalid_argument);
    QVERIFY(!QFile::exists(restoredPath()));
}

void BackupFileCodecTests::tamperedPayload_failsAndLeavesNoOutput() {
    writeSource(randomish(20'000));

    sak::BackupCodecOptions options;
    options.encrypt = true;
    options.password = QStringLiteral("pw");
    QVERIFY(sak::writeBackupFile(sourcePath(), storedPath(), options).has_value());

    // Flip a bit deep inside the ciphertext.
    QFile stored(storedPath());
    QVERIFY(stored.open(QIODevice::ReadWrite));
    const qint64 target = stored.size() / 2;
    QVERIFY(stored.seek(target));
    char byte = 0;
    QCOMPARE(stored.read(&byte, 1), qint64{1});
    byte = static_cast<char>(byte ^ 0x01);
    QVERIFY(stored.seek(target));
    QCOMPARE(stored.write(&byte, 1), qint64{1});
    stored.close();

    auto restored = sak::readBackupFile(storedPath(), restoredPath(), options.password);
    QVERIFY2(!restored.has_value(), "an edited container must fail authentication");
    QVERIFY2(!QFile::exists(restoredPath()), "no plaintext may survive a failed restore");
}

void BackupFileCodecTests::truncatedContainer_failsAndLeavesNoOutput() {
    writeSource(randomish(20'000));

    sak::BackupCodecOptions options;
    options.encrypt = true;
    options.password = QStringLiteral("pw");
    QVERIFY(sak::writeBackupFile(sourcePath(), storedPath(), options).has_value());

    QFile stored(storedPath());
    QVERIFY(stored.open(QIODevice::ReadWrite));
    QVERIFY(stored.resize(stored.size() - 64));
    stored.close();

    auto restored = sak::readBackupFile(storedPath(), restoredPath(), options.password);
    QVERIFY2(!restored.has_value(), "a truncated container must fail");
    QVERIFY(!QFile::exists(restoredPath()));
}

void BackupFileCodecTests::truncatedPlainCompressedPayload_failsAndLeavesNoOutput() {
    // A PLAIN (unencrypted) but COMPRESSED container: no AEAD tag stands between the reader
    // and the payload, so the codec's OWN completeness guard must reject a deflate stream
    // that was cut short. Dropping ONLY the trailing 4-byte zlib Adler-32 check leaves every
    // deflate block intact, so inflate still emits the whole payload (plain_bytes ==
    // original_size, which the size guard therefore CANNOT catch) yet never reaches
    // Z_STREAM_END. That is the exact state only the ended() guard can reject, so this slot
    // kills the mutant that drops the '!' on the Z_STREAM_END check.
    const QByteArray original = repetitive(64 * 1024);  // compresses to a multi-byte stream
    writeSource(original);

    sak::BackupCodecOptions options;
    options.compress = true;  // plain + compressed (encrypt defaults false)
    QVERIFY(!options.encrypt);

    auto written = sak::writeBackupFile(sourcePath(), storedPath(), options);
    QVERIFY(written.has_value());
    QCOMPARE(sak::backupContainerKind(storedPath()), sak::BackupContainerKind::Plain);

    // The last 4 bytes of a zlib stream are always its Adler-32 trailer; the deflate blocks
    // precede it untouched, so this truncates the payload end without disturbing the header
    // or the compressed data itself.
    QFile stored(storedPath());
    QVERIFY(stored.open(QIODevice::ReadWrite));
    QVERIFY(stored.resize(stored.size() - 4));
    stored.close();

    auto restored = sak::readBackupFile(storedPath(), restoredPath(), QString());
    QVERIFY2(!restored.has_value(),
             "a compressed stream cut short of Z_STREAM_END must fail closed");
    QCOMPARE(restored.error(), sak::error_code::invalid_format);
    QVERIFY(!QFile::exists(restoredPath()));
    QVERIFY(!QFile::exists(restoredPath() + QStringLiteral(".sakpart")));
}

void BackupFileCodecTests::plainCompressedDeclaredSizeMismatch_failsAndLeavesNoOutput() {
    // The decoded-length guard is the codec's last line for a PLAIN container. A truncated
    // deflate stream is caught earlier by the ended() guard, so a pure truncation can never
    // reach this check; the size guard earns its keep when the intact stream reaches
    // Z_STREAM_END but the (unauthenticated, because unencrypted) inner header lies about the
    // original size. This slot kills the mutant that inverts plain_bytes != original_size.
    const QByteArray original = repetitive(64 * 1024);
    writeSource(original);

    sak::BackupCodecOptions options;
    options.compress = true;  // plain + compressed (encrypt defaults false)
    QVERIFY(!options.encrypt);

    auto written = sak::writeBackupFile(sourcePath(), storedPath(), options);
    QVERIFY(written.has_value());
    QCOMPARE(sak::backupContainerKind(storedPath()), sak::BackupContainerKind::Plain);

    // Inner header layout: 8-byte magic, then flags(u32) reserved(u32) original_size(qint64,
    // little endian). Overwrite the declared size at offset 16 with one more than the truth.
    // The deflate stream is left intact, so it decompresses cleanly to the real size and
    // reaches Z_STREAM_END -- only the size comparison can reject the mismatch.
    QByteArray declared;
    {
        QDataStream stream(&declared, QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream << static_cast<qint64>(original.size() + 1);
    }
    QVERIFY(declared.size() == 8);

    QFile stored(storedPath());
    QVERIFY(stored.open(QIODevice::ReadWrite));
    QVERIFY(stored.seek(16));  // 8-byte magic + flags(4) + reserved(4)
    QCOMPARE(stored.write(declared), qint64{8});
    stored.close();

    auto restored = sak::readBackupFile(storedPath(), restoredPath(), QString());
    QVERIFY2(!restored.has_value(), "a header that lies about the payload size must fail closed");
    QCOMPARE(restored.error(), sak::error_code::invalid_format);
    QVERIFY(!QFile::exists(restoredPath()));
    QVERIFY(!QFile::exists(restoredPath() + QStringLiteral(".sakpart")));
}

void BackupFileCodecTests::restoreOfNonContainer_refused() {
    writeSource(repetitive(500));
    auto restored = sak::readBackupFile(sourcePath(), restoredPath(), QString());
    QVERIFY(!restored.has_value());
    QCOMPARE(restored.error(), sak::error_code::invalid_format);
}

// ============================================================================
// Cancellation
// ============================================================================

void BackupFileCodecTests::cancelDuringWrite_removesPartialFile() {
    // Bigger than one chunk so the cancel is observed mid-file.
    writeSource(randomish(4 * (1 << 20)));

    sak::BackupCodecOptions options;
    options.encrypt = true;
    options.compress = true;
    options.password = QStringLiteral("pw");

    int calls = 0;
    auto cancelled = [&calls]() {
        return ++calls > 1;
    };

    auto written = sak::writeBackupFile(sourcePath(), storedPath(), options, cancelled);
    QVERIFY2(!written.has_value(), "a cancelled write must report failure, not partial success");
    QCOMPARE(written.error(), sak::error_code::operation_cancelled);
    QVERIFY2(!QFile::exists(storedPath()), "a cancelled write must not leave a truncated file");
}

QTEST_GUILESS_MAIN(BackupFileCodecTests)
#include "test_backup_file_codec.moc"

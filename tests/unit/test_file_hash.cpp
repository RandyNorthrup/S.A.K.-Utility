// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_file_hash.cpp
/// @brief Unit tests for file hashing (MD5, SHA-256) and verification

#include "sak/error_codes.h"
#include "sak/file_hash.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QtTest/QtTest>

#include <filesystem>

class FileHashTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    // Constructor
    void constructor_defaultValues();
    void constructor_customValues();
    void constructor_zeroChunkCoercedToDefault();

    // MD5 file hashing
    void md5_knownContent();
    void md5_emptyFile();
    void md5_binaryFile();

    // SHA-256 file hashing
    void sha256_knownContent();
    void sha256_emptyFile();

    // In-memory hashing
    void md5_inMemory();
    void sha256_inMemory();
    void inMemory_emptySpan();

    // Verify hash
    void verifyHash_correct();
    void verifyHash_incorrect();
    void verifyHash_caseInsensitive();
    void verifyHash_highByteExpectedNoUb();

    // Error paths
    void hashNonExistentFile();
    void hashDirectory();

    // Progress callback
    void progressCallback_invoked();

    // Cancellation
    void cancellation_stopsHashing();

    // Convenience functions
    void md5File_convenience();
    void sha256File_convenience();
    void hash_nonAsciiPath();

private:
    QTemporaryDir m_tempDir;
    std::filesystem::path m_knownFile;
    std::filesystem::path m_emptyFile;
    std::filesystem::path m_binaryFile;
};

void FileHashTests::initTestCase() {
    QVERIFY(m_tempDir.isValid());

    // Create a file with known content: "Hello, World!\n"
    QString knownPath = m_tempDir.filePath("known.txt");
    QFile f(knownPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("Hello, World!\n");
    f.close();
    m_knownFile = knownPath.toStdWString();

    // Create an empty file
    QString emptyPath = m_tempDir.filePath("empty.txt");
    QFile e(emptyPath);
    QVERIFY(e.open(QIODevice::WriteOnly));
    e.close();
    m_emptyFile = emptyPath.toStdWString();

    // Create a binary file with all byte values
    QString binaryPath = m_tempDir.filePath("binary.bin");
    QFile b(binaryPath);
    QVERIFY(b.open(QIODevice::WriteOnly));
    QByteArray data;
    data.resize(256);
    for (int i = 0; i < 256; ++i) {
        data[i] = static_cast<char>(i);
    }
    b.write(data);
    b.close();
    m_binaryFile = binaryPath.toStdWString();
}

void FileHashTests::cleanupTestCase() {
    // QTemporaryDir auto-cleans
}

// ============================================================================
// Constructor Tests
// ============================================================================

void FileHashTests::constructor_defaultValues() {
    sak::file_hasher hasher;
    QCOMPARE(hasher.getAlgorithm(), sak::hash_algorithm::sha256);
    QCOMPARE(hasher.getChunkSize(), sak::file_hasher::DEFAULT_CHUNK_SIZE);
}

void FileHashTests::constructor_customValues() {
    sak::file_hasher hasher(sak::hash_algorithm::sha256, 4096);
    QCOMPARE(hasher.getAlgorithm(), sak::hash_algorithm::sha256);
    QCOMPARE(hasher.getChunkSize(), std::size_t{4096});
}

void FileHashTests::constructor_zeroChunkCoercedToDefault() {
    // B8-23: a zero chunk_size only trips the debug assert; in a release build it
    // would reach file.read(0), which returns an empty buffer, so hashing would
    // stop immediately and report the EMPTY-input digest as the file's hash. The
    // constructor coerces it to the default so the hash is correct.
    sak::file_hasher zero(sak::hash_algorithm::md5, 0);
    QCOMPARE(zero.getChunkSize(), sak::file_hasher::DEFAULT_CHUNK_SIZE);

    const auto zero_hash = zero.calculateHash(m_knownFile);
    QVERIFY(zero_hash.has_value());
    const auto ref_hash = sak::file_hasher(sak::hash_algorithm::md5).calculateHash(m_knownFile);
    QVERIFY(ref_hash.has_value());
    QCOMPARE(zero_hash.value(), ref_hash.value());
    // Crucially, NOT the empty-input digest.
    QVERIFY(QString::fromStdString(zero_hash.value()).toLower() !=
            QStringLiteral("d41d8cd98f00b204e9800998ecf8427e"));
}

// ============================================================================
// MD5 File Hashing
// ============================================================================

void FileHashTests::md5_knownContent() {
    sak::file_hasher hasher(sak::hash_algorithm::md5);
    auto result = hasher.calculateHash(m_knownFile);
    QVERIFY2(result.has_value(), "MD5 hash calculation should succeed");
    // MD5 of "Hello, World!\n" (matches the emptyFile sibling's exact-digest pin).
    QCOMPARE(QString::fromStdString(result.value()).toLower(),
             QString("bea8252ff4e80f41719ea13cdf007273"));
}

void FileHashTests::md5_emptyFile() {
    sak::file_hasher hasher(sak::hash_algorithm::md5);
    auto result = hasher.calculateHash(m_emptyFile);
    QVERIFY(result.has_value());
    // MD5 of empty input is d41d8cd98f00b204e9800998ecf8427e
    QCOMPARE(QString::fromStdString(result.value()).toLower(),
             QString("d41d8cd98f00b204e9800998ecf8427e"));
}

void FileHashTests::md5_binaryFile() {
    sak::file_hasher hasher(sak::hash_algorithm::md5);
    auto result = hasher.calculateHash(m_binaryFile);
    QVERIFY(result.has_value());
    // MD5 of the deterministic 256-byte file (every byte 0x00..0xFF).
    QCOMPARE(QString::fromStdString(result.value()).toLower(),
             QString("e2c865db4162bed963bfaa9ef6ac18f0"));
}

// ============================================================================
// SHA-256 File Hashing
// ============================================================================

void FileHashTests::sha256_knownContent() {
    sak::file_hasher hasher(sak::hash_algorithm::sha256);
    auto result = hasher.calculateHash(m_knownFile);
    QVERIFY(result.has_value());
    // SHA-256 of "Hello, World!\n".
    QCOMPARE(QString::fromStdString(result.value()).toLower(),
             QString("c98c24b677eff44860afea6f493bbaec5bb1c4cbb209c6fc2bbb47f66ff2ad31"));
}

void FileHashTests::sha256_emptyFile() {
    sak::file_hasher hasher(sak::hash_algorithm::sha256);
    auto result = hasher.calculateHash(m_emptyFile);
    QVERIFY(result.has_value());
    // SHA-256 of empty input is e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    QCOMPARE(QString::fromStdString(result.value()).toLower(),
             QString("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

// ============================================================================
// In-Memory Hashing
// ============================================================================

void FileHashTests::md5_inMemory() {
    const std::byte data[] = {std::byte{'H'}, std::byte{'i'}};
    sak::file_hasher hasher(sak::hash_algorithm::md5);
    auto result = hasher.calculateHash(std::span<const std::byte>(data, 2));
    QVERIFY(result.has_value());
    // MD5 of "Hi".
    QCOMPARE(QString::fromStdString(result.value()).toLower(),
             QString("c1a5298f939e87e8f962a5edfc206918"));
}

void FileHashTests::sha256_inMemory() {
    const std::byte data[] = {std::byte{'H'}, std::byte{'i'}};
    sak::file_hasher hasher(sak::hash_algorithm::sha256);
    auto result = hasher.calculateHash(std::span<const std::byte>(data, 2));
    QVERIFY(result.has_value());
    // SHA-256 of "Hi".
    QCOMPARE(QString::fromStdString(result.value()).toLower(),
             QString("3639efcd08abb273b1619e82e78c29a7df02c1051b1820e99fc395dcaa3326b8"));
}

void FileHashTests::inMemory_emptySpan() {
    sak::file_hasher hasher(sak::hash_algorithm::md5);
    auto result = hasher.calculateHash(std::span<const std::byte>{});
    QVERIFY(result.has_value());
    // Same as empty file MD5
    QCOMPARE(QString::fromStdString(result.value()).toLower(),
             QString("d41d8cd98f00b204e9800998ecf8427e"));
}

// ============================================================================
// Hash Verification
// ============================================================================

void FileHashTests::verifyHash_correct() {
    sak::file_hasher hasher(sak::hash_algorithm::md5);
    auto hash_result = hasher.calculateHash(m_knownFile);
    QVERIFY(hash_result.has_value());

    auto verify = hasher.verifyHash(m_knownFile, hash_result.value());
    QVERIFY(verify.has_value());
    QVERIFY(verify.value());
}

void FileHashTests::verifyHash_incorrect() {
    sak::file_hasher hasher(sak::hash_algorithm::md5);
    auto verify = hasher.verifyHash(m_knownFile, "00000000000000000000000000000000");
    QVERIFY(verify.has_value());
    QVERIFY(!verify.value());
}

void FileHashTests::verifyHash_caseInsensitive() {
    sak::file_hasher hasher(sak::hash_algorithm::md5);
    auto hash_result = hasher.calculateHash(m_knownFile);
    QVERIFY(hash_result.has_value());

    // Convert to uppercase for comparison
    std::string upper_hash = hash_result.value();
    for (auto& c : upper_hash) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    auto verify = hasher.verifyHash(m_knownFile, upper_hash);
    QVERIFY(verify.has_value());
    QVERIFY(verify.value());
}

void FileHashTests::verifyHash_highByteExpectedNoUb() {
    // B8-23: a garbled/hostile expected hash containing a byte > 127 must not
    // reach std::tolower with a negative (signed-char) value, which is undefined
    // behavior. The comparison simply reports a mismatch; the point is it runs
    // cleanly (verified under the sanitizer builds).
    sak::file_hasher hasher(sak::hash_algorithm::md5);
    const auto real = hasher.calculateHash(m_knownFile);
    QVERIFY(real.has_value());

    std::string garbled = real.value();
    garbled[0] = static_cast<char>(0xE9);  // high byte -> negative as signed char
    const auto verify = hasher.verifyHash(m_knownFile, garbled);
    QVERIFY(verify.has_value());
    QVERIFY(!verify.value());
}

// ============================================================================
// Error Paths
// ============================================================================

void FileHashTests::hashNonExistentFile() {
    sak::file_hasher hasher;
    auto result = hasher.calculateHash(std::filesystem::path("nonexistent_file_12345.txt"));
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(), sak::error_code::file_not_found);
}

void FileHashTests::hashDirectory() {
    sak::file_hasher hasher;
    auto result = hasher.calculateHash(m_tempDir.path().toStdWString());
    QVERIFY(!result.has_value());
    // A directory is not a regular file -> invalid_path (mirrors the file_not_found sibling).
    QCOMPARE(result.error(), sak::error_code::invalid_path);
}

// ============================================================================
// Progress Callback
// ============================================================================

void FileHashTests::progressCallback_invoked() {
    // Create a file larger than chunk size to trigger progress
    QString bigPath = m_tempDir.filePath("big.bin");
    QFile f(bigPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QByteArray chunk(1024, 'A');
    for (int i = 0; i < 100; ++i) {  // 100 KB
        f.write(chunk);
    }
    f.close();

    int callbackCount = 0;
    sak::file_hasher hasher(sak::hash_algorithm::md5, 8192);  // Small chunk for more callbacks
    auto result = hasher.calculateHash(
        bigPath.toStdWString(),
        [&callbackCount](std::size_t /*processed*/, std::size_t /*total*/) { ++callbackCount; });
    QVERIFY(result.has_value());
    // 102400 bytes / 8192 chunk = 12 full reads + one 4096 read = 13 non-empty reads.
    QCOMPARE(callbackCount, 13);
}

// ============================================================================
// Cancellation
// ============================================================================

void FileHashTests::cancellation_stopsHashing() {
    // Create a reasonably large file
    QString largePath = m_tempDir.filePath("large_cancel.bin");
    QFile f(largePath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QByteArray chunk(1024 * 1024, 'B');  // 1 MB chunks
    for (int i = 0; i < 10; ++i) {
        f.write(chunk);
    }
    f.close();

    std::stop_source stopSource;
    sak::file_hasher hasher(sak::hash_algorithm::sha256, 4096);

    // Request stop immediately
    stopSource.request_stop();

    auto result = hasher.calculateHash(largePath.toStdWString(), nullptr, stopSource.get_token());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(), sak::error_code::operation_cancelled);
}

// ============================================================================
// Convenience Functions
// ============================================================================

void FileHashTests::md5File_convenience() {
    auto result = sak::md5File(m_knownFile);
    QVERIFY(result.has_value());
    QCOMPARE(QString::fromStdString(result.value()).toLower(),
             QString("bea8252ff4e80f41719ea13cdf007273"));
}

void FileHashTests::sha256File_convenience() {
    auto result = sak::sha256File(m_knownFile);
    QVERIFY(result.has_value());
    QCOMPARE(QString::fromStdString(result.value()).toLower(),
             QString("c98c24b677eff44860afea6f493bbaec5bb1c4cbb209c6fc2bbb47f66ff2ad31"));
}

void FileHashTests::hash_nonAsciiPath() {
    // A path with non-ASCII characters must hash correctly. Previously the path
    // was round-tripped through a narrow (CP_ACP) string that QString misread as
    // UTF-8, mangling the name so the open failed with read_error.
    const QString unicodeName = QString::fromUtf8("caf\xC3\xA9_\xE6\x96\x87\xE4\xBB\xB6.txt");
    const QString unicodePath = m_tempDir.filePath(unicodeName);
    QFile f(unicodePath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("Hello, World!\n");
    f.close();

    // Same content as m_knownFile, so the digests must match.
    sak::file_hasher hasher(sak::hash_algorithm::md5);
    auto unicodeResult = hasher.calculateHash(std::filesystem::path(unicodePath.toStdWString()));
    auto knownResult = hasher.calculateHash(m_knownFile);
    QVERIFY2(unicodeResult.has_value(), "hashing a non-ASCII path must succeed");
    QVERIFY(knownResult.has_value());
    QCOMPARE(unicodeResult.value(), knownResult.value());
}

QTEST_GUILESS_MAIN(FileHashTests)
#include "test_file_hash.moc"

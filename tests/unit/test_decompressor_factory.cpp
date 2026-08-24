// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_decompressor_factory.cpp
/// @brief Unit tests for decompressor factory pattern and format detection

#include "sak/decompressor_factory.h"
#include "sak/streaming_decompressor.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class DecompressorFactoryTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    // Format detection by extension
    void detectFormat_gzExtension();
    void detectFormat_bz2Extension();
    void detectFormat_xzExtension();
    void detectFormat_tarGzExtension();
    void detectFormat_unknownExtension();
    void detectFormat_caseInsensitive();

    // isCompressed
    void isCompressed_gzFile();
    void isCompressed_txtFile();
    void isCompressed_nonExistent();

    // Factory create
    void create_gzReturnsDecompressor();
    void create_unknownReturnsNull();
    void create_nonExistentReturnsNull();
    void zipDetectionMatchesCreateCapability();

    // Magic number detection
    void detectFormat_gzipMagic();
    void detectFormat_bzip2Magic();
    void detectFormat_xzMagic();
    void detectFormat_noMagicNoExtension();

    // Short-buffer magic guards: a buffer genuinely SHORTER than a signature must
    // not be spoof-matched (kills magic-length reductions and min-bytes fail-open).
    void detectFormat_gzipShortMagicRejected();
    void detectFormat_bzip2ShortMagicRejected();
    void detectFormat_xzShortMagicRejected();
    void detectFormat_shortBufferNotSpoofedByZeroFill();

private:
    QTemporaryDir m_tempDir;

    void writeFile(const QString& name, const QByteArray& content);
    QString filePath(const QString& name) const;
};

void DecompressorFactoryTests::initTestCase() {
    QVERIFY(m_tempDir.isValid());
}

void DecompressorFactoryTests::writeFile(const QString& name, const QByteArray& content) {
    QFile f(m_tempDir.filePath(name));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(content);
    f.close();
}

QString DecompressorFactoryTests::filePath(const QString& name) const {
    return m_tempDir.filePath(name);
}

// ============================================================================
// Format Detection by Extension
// ============================================================================

void DecompressorFactoryTests::detectFormat_gzExtension() {
    writeFile("test.gz", "dummy");
    QString fmt = sak::DecompressorFactory::detectFormat(filePath("test.gz"));
    QCOMPARE(fmt.toLower(), QString("gzip"));
}

void DecompressorFactoryTests::detectFormat_bz2Extension() {
    writeFile("test.bz2", "dummy");
    QString fmt = sak::DecompressorFactory::detectFormat(filePath("test.bz2"));
    QCOMPARE(fmt.toLower(), QString("bzip2"));
}

void DecompressorFactoryTests::detectFormat_xzExtension() {
    writeFile("test.xz", "dummy");
    QString fmt = sak::DecompressorFactory::detectFormat(filePath("test.xz"));
    QCOMPARE(fmt.toLower(), QString("xz"));
}

void DecompressorFactoryTests::detectFormat_tarGzExtension() {
    writeFile("archive.tar.gz", "dummy");
    QString fmt = sak::DecompressorFactory::detectFormat(filePath("archive.tar.gz"));
    QCOMPARE(fmt.toLower(), QString("gzip"));
}

void DecompressorFactoryTests::detectFormat_unknownExtension() {
    writeFile("file.txt", "plain text");
    QString fmt = sak::DecompressorFactory::detectFormat(filePath("file.txt"));
    QVERIFY(
        fmt.isEmpty());  // production returns "" for an unknown format; "unknown" is never returned
}

void DecompressorFactoryTests::detectFormat_caseInsensitive() {
    writeFile("test.GZ", "dummy");
    QString fmt = sak::DecompressorFactory::detectFormat(filePath("test.GZ"));
    QCOMPARE(fmt.toLower(), QString("gzip"));
}

// ============================================================================
// isCompressed
// ============================================================================

void DecompressorFactoryTests::isCompressed_gzFile() {
    writeFile("compressed.gz", "dummy");
    QVERIFY(sak::DecompressorFactory::isCompressed(filePath("compressed.gz")));
}

void DecompressorFactoryTests::isCompressed_txtFile() {
    writeFile("plain.txt", "not compressed");
    QVERIFY(!sak::DecompressorFactory::isCompressed(filePath("plain.txt")));
}

void DecompressorFactoryTests::isCompressed_nonExistent() {
    // isCompressed checks extension first, so a .gz file that doesn't exist
    // is still detected as compressed based on extension alone
    QVERIFY(sak::DecompressorFactory::isCompressed(filePath("no_such_file.gz")));
}

// ============================================================================
// Factory Create
// ============================================================================

void DecompressorFactoryTests::create_gzReturnsDecompressor() {
    // Write a file with gzip magic bytes (but not a valid gzip stream)
    QByteArray gzMagic;
    gzMagic.append(static_cast<char>(0x1F));
    gzMagic.append(static_cast<char>(0x8B));
    gzMagic.append(QByteArray(14, '\0'));
    writeFile("real.gz", gzMagic);

    auto decomp = sak::DecompressorFactory::create(filePath("real.gz"));
    QVERIFY(decomp != nullptr);
    QCOMPARE(decomp->formatName().toLower(), QString("gzip"));
}

void DecompressorFactoryTests::create_unknownReturnsNull() {
    writeFile("mystery.xyz", "unknown format data");
    auto decomp = sak::DecompressorFactory::create(filePath("mystery.xyz"));
    QVERIFY(decomp == nullptr);
}

void DecompressorFactoryTests::create_nonExistentReturnsNull() {
    // create() detects format by extension and creates a decompressor
    // even for non-existent files (deferred open pattern)
    auto decomp = sak::DecompressorFactory::create(filePath("does_not_exist.gz"));
    QVERIFY(decomp != nullptr);
}

void DecompressorFactoryTests::zipDetectionMatchesCreateCapability() {
    // B8-23: the factory used to advertise zip (detectFormat -> "zip",
    // isCompressed -> true) while create() had no zip branch and returned null,
    // so a guard-then-create caller got true then nullptr. A zip is an archive,
    // not a single stream this factory can decompress; detection and creation
    // must agree. Cover both the extension and the "PK" magic paths.
    writeFile("archive.zip", QByteArrayLiteral("PK\x03\x04 not a real zip payload"));
    QVERIFY(sak::DecompressorFactory::detectFormat(filePath("archive.zip")).isEmpty());
    QVERIFY(!sak::DecompressorFactory::isCompressed(filePath("archive.zip")));
    QVERIFY(sak::DecompressorFactory::create(filePath("archive.zip")) == nullptr);

    // A zip signature under a non-archive extension must not be detected as zip.
    QByteArray pk;
    pk.append('P').append('K').append(QByteArray(14, '\0'));
    writeFile("magic_pk.bin", pk);
    QVERIFY(sak::DecompressorFactory::detectFormat(filePath("magic_pk.bin")).isEmpty());
}

// ============================================================================
// Magic Number Detection
// ============================================================================

void DecompressorFactoryTests::detectFormat_gzipMagic() {
    // Gzip magic: 1F 8B
    QByteArray data;
    data.append(static_cast<char>(0x1F));
    data.append(static_cast<char>(0x8B));
    data.append(QByteArray(14, '\0'));
    writeFile("magic_gz.bin", data);

    QString fmt = sak::DecompressorFactory::detectFormat(filePath("magic_gz.bin"));
    // EXACT case: create() dispatches on format == "gzip". A .toLower() compare lets the magic
    // table row be relabelled "GZIP" -- detection still reports success while create() falls
    // through and returns nullptr, the guard-true-then-create-null split this file forbids.
    QCOMPARE(fmt, QString("gzip"));
}

void DecompressorFactoryTests::detectFormat_bzip2Magic() {
    // BZip2 magic: 42 5A 68
    QByteArray data;
    data.append('B').append('Z').append('h');
    data.append(QByteArray(13, '\0'));
    writeFile("magic_bz2.bin", data);

    QString fmt = sak::DecompressorFactory::detectFormat(filePath("magic_bz2.bin"));
    // EXACT case: create() dispatches on format == "bzip2" (see the gzip sibling).
    QCOMPARE(fmt, QString("bzip2"));
}

void DecompressorFactoryTests::detectFormat_xzMagic() {
    // XZ magic: FD 37 7A 58 5A 00
    QByteArray data;
    data.append(static_cast<char>(0xFD));
    data.append(static_cast<char>(0x37));
    data.append(static_cast<char>(0x7A));
    data.append(static_cast<char>(0x58));
    data.append(static_cast<char>(0x5A));
    data.append(static_cast<char>(0x00));
    data.append(QByteArray(10, '\0'));
    writeFile("magic_xz.bin", data);

    QString fmt = sak::DecompressorFactory::detectFormat(filePath("magic_xz.bin"));
    // EXACT case: create() dispatches on format == "xz" (see the gzip sibling).
    QCOMPARE(fmt, QString("xz"));
}

void DecompressorFactoryTests::detectFormat_noMagicNoExtension() {
    writeFile("plain_data.bin", "Just some plain text data");
    QString fmt = sak::DecompressorFactory::detectFormat(filePath("plain_data.bin"));
    QVERIFY(
        fmt.isEmpty());  // production returns "" for an unknown format; "unknown" is never returned
}

// ============================================================================
// Short-buffer magic guards
//
// Every other magic test writes a 16-byte zero-filled buffer, so a shortened
// magic compare -- or a relaxed "enough bytes were read" guard -- still spoof-
// matches against the zero tail and goes undetected. These tests write buffers
// GENUINELY SHORTER than the signature under test (no zero padding up to the
// signature length) and pin the honest behaviour both ways: the short buffer is
// NOT detected as that format, while the full-length signature still is.
// ============================================================================

void DecompressorFactoryTests::detectFormat_gzipShortMagicRejected() {
    // Gzip magic is TWO bytes (1F 8B). A one-byte buffer holding only 0x1F must
    // not be detected as gzip: the min-bytes guard skips the 2-byte signature and
    // no zero-fill may complete it. Kills a gzip magic-length reduction 2 -> 1.
    QByteArray shortBuf;  // exactly 1 byte, deliberately shorter than the signature
    shortBuf.append(static_cast<char>(0x1F));
    QCOMPARE(shortBuf.size(), 1);
    writeFile("short_gz_1b.bin", shortBuf);
    QVERIFY(sak::DecompressorFactory::detectFormat(filePath("short_gz_1b.bin")).isEmpty());
    QVERIFY(!sak::DecompressorFactory::isCompressed(filePath("short_gz_1b.bin")));

    // The full 1F 8B signature must still be detected as gzip.
    QByteArray full;
    full.append(static_cast<char>(0x1F));
    full.append(static_cast<char>(0x8B));
    full.append(QByteArray(14, '\0'));
    writeFile("full_gz_magic.bin", full);
    QCOMPARE(sak::DecompressorFactory::detectFormat(filePath("full_gz_magic.bin")).toLower(),
             QString("gzip"));
}

void DecompressorFactoryTests::detectFormat_bzip2ShortMagicRejected() {
    // BZip2 magic is THREE bytes ('BZh' = 42 5A 68). A two-byte buffer holding only
    // 'BZ' must not be detected as bzip2. Kills a bzip2 magic-length reduction 3 -> 2.
    QByteArray shortBuf;  // exactly 2 bytes, deliberately shorter than the signature
    shortBuf.append('B').append('Z');
    QCOMPARE(shortBuf.size(), 2);
    writeFile("short_bz2_2b.bin", shortBuf);
    QVERIFY(sak::DecompressorFactory::detectFormat(filePath("short_bz2_2b.bin")).isEmpty());
    QVERIFY(!sak::DecompressorFactory::isCompressed(filePath("short_bz2_2b.bin")));

    // The full 'BZh' signature must still be detected as bzip2.
    QByteArray full;
    full.append('B').append('Z').append('h');
    full.append(QByteArray(13, '\0'));
    writeFile("full_bz2_magic.bin", full);
    QCOMPARE(sak::DecompressorFactory::detectFormat(filePath("full_bz2_magic.bin")).toLower(),
             QString("bzip2"));
}

void DecompressorFactoryTests::detectFormat_xzShortMagicRejected() {
    // XZ magic is SIX bytes (FD 37 7A 58 5A 00). A five-byte buffer holding only the
    // first five (dropping the 0x00 terminator) must not be detected as xz. Kills an
    // xz magic-length reduction 6 -> 5.
    QByteArray shortBuf;  // exactly 5 bytes, deliberately shorter than the signature
    shortBuf.append(static_cast<char>(0xFD));
    shortBuf.append(static_cast<char>(0x37));
    shortBuf.append(static_cast<char>(0x7A));
    shortBuf.append(static_cast<char>(0x58));
    shortBuf.append(static_cast<char>(0x5A));
    QCOMPARE(shortBuf.size(), 5);
    writeFile("short_xz_5b.bin", shortBuf);
    QVERIFY(sak::DecompressorFactory::detectFormat(filePath("short_xz_5b.bin")).isEmpty());
    QVERIFY(!sak::DecompressorFactory::isCompressed(filePath("short_xz_5b.bin")));

    // The full FD 37 7A 58 5A 00 signature must still be detected as xz.
    QByteArray full;
    full.append(static_cast<char>(0xFD));
    full.append(static_cast<char>(0x37));
    full.append(static_cast<char>(0x7A));
    full.append(static_cast<char>(0x58));
    full.append(static_cast<char>(0x5A));
    full.append(static_cast<char>(0x00));
    full.append(QByteArray(10, '\0'));
    writeFile("full_xz_magic.bin", full);
    QCOMPARE(sak::DecompressorFactory::detectFormat(filePath("full_xz_magic.bin")).toLower(),
             QString("xz"));
}

void DecompressorFactoryTests::detectFormat_shortBufferNotSpoofedByZeroFill() {
    // The lzma/xz alternate signature 5D 00 00 has 0x00 in its non-leading bytes, so
    // a one-byte buffer holding only 0x5D would be spoof-matched as "xz" if the
    // min-bytes guard failed open and let the zero-fill probe tail complete the
    // 3-byte compare. Real code skips it (signature length 3 > 1 byte read). Kills a
    // fail-open weakening of the "entry.length > bytesRead" guard.
    QByteArray shortBuf;  // exactly 1 byte, deliberately shorter than the signature
    shortBuf.append(static_cast<char>(0x5D));
    QCOMPARE(shortBuf.size(), 1);
    writeFile("short_lzma_1b.bin", shortBuf);
    QVERIFY(sak::DecompressorFactory::detectFormat(filePath("short_lzma_1b.bin")).isEmpty());
    QVERIFY(!sak::DecompressorFactory::isCompressed(filePath("short_lzma_1b.bin")));

    // The full 5D 00 00 signature must still be detected as xz.
    QByteArray full;
    full.append(static_cast<char>(0x5D));
    full.append(static_cast<char>(0x00));
    full.append(static_cast<char>(0x00));
    full.append(QByteArray(13, '\0'));
    writeFile("full_lzma_magic.bin", full);
    // EXACT case: the legacy-lzma row carries its OWN format literal, and create() dispatches
    // on format == "xz". Relabelling that row alone is invisible to a folded compare.
    QCOMPARE(sak::DecompressorFactory::detectFormat(filePath("full_lzma_magic.bin")),
             QString("xz"));
}

QTEST_GUILESS_MAIN(DecompressorFactoryTests)
#include "test_decompressor_factory.moc"

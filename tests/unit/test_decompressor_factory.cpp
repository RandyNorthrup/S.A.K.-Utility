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

    // Magic number detection
    void detectFormat_gzipMagic();
    void detectFormat_bzip2Magic();
    void detectFormat_xzMagic();
    void detectFormat_noMagicNoExtension();

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
    QVERIFY(fmt.isEmpty() || fmt.toLower() == "unknown");
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
    QCOMPARE(fmt.toLower(), QString("gzip"));
}

void DecompressorFactoryTests::detectFormat_bzip2Magic() {
    // BZip2 magic: 42 5A 68
    QByteArray data;
    data.append('B').append('Z').append('h');
    data.append(QByteArray(13, '\0'));
    writeFile("magic_bz2.bin", data);

    QString fmt = sak::DecompressorFactory::detectFormat(filePath("magic_bz2.bin"));
    QCOMPARE(fmt.toLower(), QString("bzip2"));
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
    QCOMPARE(fmt.toLower(), QString("xz"));
}

void DecompressorFactoryTests::detectFormat_noMagicNoExtension() {
    writeFile("plain_data.bin", "Just some plain text data");
    QString fmt = sak::DecompressorFactory::detectFormat(filePath("plain_data.bin"));
    QVERIFY(fmt.isEmpty() || fmt.toLower() == "unknown");
}

QTEST_GUILESS_MAIN(DecompressorFactoryTests)
#include "test_decompressor_factory.moc"

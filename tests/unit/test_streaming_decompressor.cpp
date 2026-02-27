// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_streaming_decompressor.cpp
/// @brief Unit tests for StreamingDecompressor and DecompressorFactory lifecycle

#include <QtTest/QtTest>

#include "sak/decompressor_factory.h"
#include "sak/streaming_decompressor.h"

#include <QTemporaryDir>
#include <QFile>

class StreamingDecompressorTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    // Factory + open/close lifecycle
    void gzipDecompressor_openClose();
    void bzip2Decompressor_openClose();
    void xzDecompressor_openClose();

    // Open non-existent file
    void open_nonExistentFile();

    // Read from invalid stream
    void read_beforeOpen();

    // Format names
    void gzip_formatName();
    void bzip2_formatName();
    void xz_formatName();

    // Compressed bytes tracking
    void compressedBytesRead_initiallyZero();

    // atEnd on fresh decompressor
    void atEnd_beforeOpen();

private:
    QTemporaryDir m_tempDir;

    void writeFile(const QString& name, const QByteArray& content);
    QString filePath(const QString& name) const;
};

void StreamingDecompressorTests::initTestCase()
{
    QVERIFY(m_tempDir.isValid());

    // Write files with correct magic bytes but invalid compressed content
    // These should still be openable by the factory

    // Gzip magic: 1F 8B 08 00... (minimum gzip header)
    QByteArray gzData;
    gzData.append(static_cast<char>(0x1F));
    gzData.append(static_cast<char>(0x8B));
    gzData.append(static_cast<char>(0x08));
    gzData.append(QByteArray(29, '\0'));
    writeFile("test.gz", gzData);

    // BZip2 magic: 42 5A 68 (BZh)
    QByteArray bz2Data;
    bz2Data.append('B').append('Z').append('h').append('9');
    bz2Data.append(QByteArray(28, '\0'));
    writeFile("test.bz2", bz2Data);

    // XZ magic: FD 37 7A 58 5A 00
    QByteArray xzData;
    xzData.append(static_cast<char>(0xFD));
    xzData.append(static_cast<char>(0x37));
    xzData.append(static_cast<char>(0x7A));
    xzData.append(static_cast<char>(0x58));
    xzData.append(static_cast<char>(0x5A));
    xzData.append(static_cast<char>(0x00));
    xzData.append(QByteArray(26, '\0'));
    writeFile("test.xz", xzData);
}

void StreamingDecompressorTests::writeFile(const QString& name, const QByteArray& content)
{
    QFile f(m_tempDir.filePath(name));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(content);
    f.close();
}

QString StreamingDecompressorTests::filePath(const QString& name) const
{
    return m_tempDir.filePath(name);
}

// ============================================================================
// Factory + Open/Close Lifecycle
// ============================================================================

void StreamingDecompressorTests::gzipDecompressor_openClose()
{
    auto decomp = sak::DecompressorFactory::create(filePath("test.gz"));
    QVERIFY(decomp != nullptr);

    // Open may fail with invalid compressed data, but should not crash
    bool opened = decomp->open(filePath("test.gz"));
    if (opened) {
        QVERIFY(decomp->isOpen());
        decomp->close();
        QVERIFY(!decomp->isOpen());
    }
}

void StreamingDecompressorTests::bzip2Decompressor_openClose()
{
    auto decomp = sak::DecompressorFactory::create(filePath("test.bz2"));
    QVERIFY(decomp != nullptr);

    bool opened = decomp->open(filePath("test.bz2"));
    if (opened) {
        QVERIFY(decomp->isOpen());
        decomp->close();
    }
}

void StreamingDecompressorTests::xzDecompressor_openClose()
{
    auto decomp = sak::DecompressorFactory::create(filePath("test.xz"));
    QVERIFY(decomp != nullptr);

    bool opened = decomp->open(filePath("test.xz"));
    if (opened) {
        QVERIFY(decomp->isOpen());
        decomp->close();
    }
}

// ============================================================================
// Open Non-Existent
// ============================================================================

void StreamingDecompressorTests::open_nonExistentFile()
{
    auto decomp = sak::DecompressorFactory::create(filePath("test.gz"));
    if (decomp) {
        bool opened = decomp->open(filePath("nonexistent_file.gz"));
        QVERIFY(!opened);
    }
}

// ============================================================================
// Read Before Open
// ============================================================================

void StreamingDecompressorTests::read_beforeOpen()
{
    auto decomp = sak::DecompressorFactory::create(filePath("test.gz"));
    if (decomp) {
        char buffer[64];
        qint64 bytesRead = decomp->read(buffer, sizeof(buffer));
        QVERIFY(bytesRead <= 0);
    }
}

// ============================================================================
// Format Names
// ============================================================================

void StreamingDecompressorTests::gzip_formatName()
{
    auto decomp = sak::DecompressorFactory::create(filePath("test.gz"));
    QVERIFY(decomp != nullptr);
    QCOMPARE(decomp->formatName().toLower(), QString("gzip"));
}

void StreamingDecompressorTests::bzip2_formatName()
{
    auto decomp = sak::DecompressorFactory::create(filePath("test.bz2"));
    QVERIFY(decomp != nullptr);
    QCOMPARE(decomp->formatName().toLower(), QString("bzip2"));
}

void StreamingDecompressorTests::xz_formatName()
{
    auto decomp = sak::DecompressorFactory::create(filePath("test.xz"));
    QVERIFY(decomp != nullptr);
    QCOMPARE(decomp->formatName().toLower(), QString("xz"));
}

// ============================================================================
// Compressed Bytes Tracking
// ============================================================================

void StreamingDecompressorTests::compressedBytesRead_initiallyZero()
{
    auto decomp = sak::DecompressorFactory::create(filePath("test.gz"));
    if (decomp) {
        QCOMPARE(decomp->compressedBytesRead(), qint64{0});
        QCOMPARE(decomp->decompressedBytesProduced(), qint64{0});
    }
}

// ============================================================================
// atEnd
// ============================================================================

void StreamingDecompressorTests::atEnd_beforeOpen()
{
    auto decomp = sak::DecompressorFactory::create(filePath("test.gz"));
    if (decomp) {
        // Before open, behavior is implementation-dependent
        // Just verify no crash
        [[maybe_unused]] bool ended = decomp->atEnd();
        QVERIFY(true);
    }
}

QTEST_GUILESS_MAIN(StreamingDecompressorTests)
#include "test_streaming_decompressor.moc"

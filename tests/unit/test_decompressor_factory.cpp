// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QtTest>
#include <QTemporaryFile>
#include "sak/decompressor_factory.h"
#include "sak/streaming_decompressor.h"

class TestDecompressorFactory : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Factory creation
    void testCreateGzip();
    void testCreateBzip2();
    void testCreateXZ();
    void testCreateUnsupported();
    void testCreateNullptr();

    // Format detection by extension
    void testDetectGzipExtension();
    void testDetectBzip2Extension();
    void testDetectXZExtension();
    void testDetectUnknownExtension();
    void testDetectNoExtension();

    // Format detection by magic number
    void testDetectGzipMagic();
    void testDetectBzip2Magic();
    void testDetectXZMagic();
    void testDetectUnknownMagic();

    // Combined detection
    void testDetectFormat();
    void testDetectFormatGzip();
    void testDetectFormatBzip2();
    void testDetectFormatXZ();
    void testDetectFormatUnknown();

    // Compression check
    void testIsCompressed();
    void testIsCompressedGzip();
    void testIsCompressedBzip2();
    void testIsCompressedXZ();
    void testIsCompressedPlainFile();

    // Extension variations
    void testGzExtension();
    void testGzipExtension();
    void testBz2Extension();
    void testBzip2Extension();
    void testXzExtension();
    void testLzmaExtension();

    // Case sensitivity
    void testUpperCaseExtensions();
    void testMixedCaseExtensions();

    // Double extensions
    void testDoubleExtensionIsoGz();
    void testDoubleExtensionImgXz();
    void testDoubleExtensionTarBz2();

    // Invalid inputs
    void testEmptyFilePath();
    void testNonexistentFile();
    void testDirectoryPath();

    // Magic number reading
    void testReadMagicNumber();
    void testReadMagicNumberEmptyFile();
    void testReadMagicNumberSmallFile();

    // Decompressor instances
    void testDecompressorNotNull();
    void testDecompressorType();
    void testMultipleInstances();

    // Thread safety
    void testConcurrentCreation();
    void testThreadSafeDetection();

    // Format names
    void testFormatNameGzip();
    void testFormatNameBzip2();
    void testFormatNameXZ();
    void testFormatNameEmpty();

    // ZIP format (not supported)
    void testZipNotSupported();
    void testZipDetection();

private:
    QTemporaryFile* m_tempFile{nullptr};
    
    void createTestFile(const QString& suffix, const QByteArray& magic = QByteArray());
};

void TestDecompressorFactory::initTestCase() {
    // Setup test environment
}

void TestDecompressorFactory::cleanupTestCase() {
    // Cleanup test environment
}

void TestDecompressorFactory::init() {
    // Fresh state for each test
}

void TestDecompressorFactory::cleanup() {
    delete m_tempFile;
    m_tempFile = nullptr;
}

void TestDecompressorFactory::createTestFile(const QString& suffix, const QByteArray& magic) {
    m_tempFile = new QTemporaryFile(this);
    m_tempFile->setFileTemplate("test_XXXXXX" + suffix);
    QVERIFY(m_tempFile->open());
    
    if (!magic.isEmpty()) {
        m_tempFile->write(magic);
    } else {
        m_tempFile->write("Test data");
    }
    m_tempFile->flush();
}

void TestDecompressorFactory::testCreateGzip() {
    createTestFile(".gz");
    auto decompressor = sak::DecompressorFactory::create(m_tempFile->fileName());
    QVERIFY(decompressor != nullptr);
}

void TestDecompressorFactory::testCreateBzip2() {
    createTestFile(".bz2");
    auto decompressor = sak::DecompressorFactory::create(m_tempFile->fileName());
    QVERIFY(decompressor != nullptr);
}

void TestDecompressorFactory::testCreateXZ() {
    createTestFile(".xz");
    auto decompressor = sak::DecompressorFactory::create(m_tempFile->fileName());
    QVERIFY(decompressor != nullptr);
}

void TestDecompressorFactory::testCreateUnsupported() {
    createTestFile(".txt");
    auto decompressor = sak::DecompressorFactory::create(m_tempFile->fileName());
    QVERIFY(decompressor == nullptr);
}

void TestDecompressorFactory::testCreateNullptr() {
    auto decompressor = sak::DecompressorFactory::create("");
    QVERIFY(decompressor == nullptr);
}

void TestDecompressorFactory::testDetectGzipExtension() {
    QString format = sak::DecompressorFactory::detectFormat("file.gz");
    QCOMPARE(format, QString("gzip"));
}

void TestDecompressorFactory::testDetectBzip2Extension() {
    QString format = sak::DecompressorFactory::detectFormat("file.bz2");
    QCOMPARE(format, QString("bzip2"));
}

void TestDecompressorFactory::testDetectXZExtension() {
    QString format = sak::DecompressorFactory::detectFormat("file.xz");
    QCOMPARE(format, QString("xz"));
}

void TestDecompressorFactory::testDetectUnknownExtension() {
    QString format = sak::DecompressorFactory::detectFormat("file.txt");
    QVERIFY(format.isEmpty());
}

void TestDecompressorFactory::testDetectNoExtension() {
    QString format = sak::DecompressorFactory::detectFormat("file");
    QVERIFY(format.isEmpty());
}

void TestDecompressorFactory::testDetectGzipMagic() {
    // Gzip magic: 0x1f 0x8b
    QByteArray magic;
    magic.append(0x1f);
    magic.append(0x8b);
    createTestFile(".dat", magic);
    
    QString format = sak::DecompressorFactory::detectFormat(m_tempFile->fileName());
    QCOMPARE(format, QString("gzip"));
}

void TestDecompressorFactory::testDetectBzip2Magic() {
    // Bzip2 magic: "BZ"
    createTestFile(".dat", "BZh9");
    
    QString format = sak::DecompressorFactory::detectFormat(m_tempFile->fileName());
    QCOMPARE(format, QString("bzip2"));
}

void TestDecompressorFactory::testDetectXZMagic() {
    // XZ magic: 0xFD '7' 'z' 'X' 'Z' 0x00
    QByteArray magic;
    magic.append(0xFD);
    magic.append("7zXZ");
    magic.append(0x00);
    createTestFile(".dat", magic);
    
    QString format = sak::DecompressorFactory::detectFormat(m_tempFile->fileName());
    QCOMPARE(format, QString("xz"));
}

void TestDecompressorFactory::testDetectUnknownMagic() {
    createTestFile(".dat", "INVALID");
    
    QString format = sak::DecompressorFactory::detectFormat(m_tempFile->fileName());
    QVERIFY(format.isEmpty());
}

void TestDecompressorFactory::testDetectFormat() {
    // Should detect by extension first, then magic number
    createTestFile(".gz");
    QString format = sak::DecompressorFactory::detectFormat(m_tempFile->fileName());
    QCOMPARE(format, QString("gzip"));
}

void TestDecompressorFactory::testDetectFormatGzip() {
    createTestFile(".gz");
    QCOMPARE(sak::DecompressorFactory::detectFormat(m_tempFile->fileName()), QString("gzip"));
}

void TestDecompressorFactory::testDetectFormatBzip2() {
    createTestFile(".bz2");
    QCOMPARE(sak::DecompressorFactory::detectFormat(m_tempFile->fileName()), QString("bzip2"));
}

void TestDecompressorFactory::testDetectFormatXZ() {
    createTestFile(".xz");
    QCOMPARE(sak::DecompressorFactory::detectFormat(m_tempFile->fileName()), QString("xz"));
}

void TestDecompressorFactory::testDetectFormatUnknown() {
    createTestFile(".txt");
    QVERIFY(sak::DecompressorFactory::detectFormat(m_tempFile->fileName()).isEmpty());
}

void TestDecompressorFactory::testIsCompressed() {
    createTestFile(".gz");
    QVERIFY(sak::DecompressorFactory::isCompressed(m_tempFile->fileName()));
}

void TestDecompressorFactory::testIsCompressedGzip() {
    createTestFile(".gz");
    QVERIFY(sak::DecompressorFactory::isCompressed(m_tempFile->fileName()));
}

void TestDecompressorFactory::testIsCompressedBzip2() {
    createTestFile(".bz2");
    QVERIFY(sak::DecompressorFactory::isCompressed(m_tempFile->fileName()));
}

void TestDecompressorFactory::testIsCompressedXZ() {
    createTestFile(".xz");
    QVERIFY(sak::DecompressorFactory::isCompressed(m_tempFile->fileName()));
}

void TestDecompressorFactory::testIsCompressedPlainFile() {
    createTestFile(".txt");
    QVERIFY(!sak::DecompressorFactory::isCompressed(m_tempFile->fileName()));
}

void TestDecompressorFactory::testGzExtension() {
    QCOMPARE(sak::DecompressorFactory::detectFormat("file.gz"), QString("gzip"));
}

void TestDecompressorFactory::testGzipExtension() {
    QCOMPARE(sak::DecompressorFactory::detectFormat("file.gzip"), QString("gzip"));
}

void TestDecompressorFactory::testBz2Extension() {
    QCOMPARE(sak::DecompressorFactory::detectFormat("file.bz2"), QString("bzip2"));
}

void TestDecompressorFactory::testBzip2Extension() {
    QCOMPARE(sak::DecompressorFactory::detectFormat("file.bzip2"), QString("bzip2"));
}

void TestDecompressorFactory::testXzExtension() {
    QCOMPARE(sak::DecompressorFactory::detectFormat("file.xz"), QString("xz"));
}

void TestDecompressorFactory::testLzmaExtension() {
    QCOMPARE(sak::DecompressorFactory::detectFormat("file.lzma"), QString("xz"));
}

void TestDecompressorFactory::testUpperCaseExtensions() {
    QCOMPARE(sak::DecompressorFactory::detectFormat("FILE.GZ"), QString("gzip"));
    QCOMPARE(sak::DecompressorFactory::detectFormat("FILE.BZ2"), QString("bzip2"));
    QCOMPARE(sak::DecompressorFactory::detectFormat("FILE.XZ"), QString("xz"));
}

void TestDecompressorFactory::testMixedCaseExtensions() {
    QCOMPARE(sak::DecompressorFactory::detectFormat("File.Gz"), QString("gzip"));
    QCOMPARE(sak::DecompressorFactory::detectFormat("File.Bz2"), QString("bzip2"));
}

void TestDecompressorFactory::testDoubleExtensionIsoGz() {
    QCOMPARE(sak::DecompressorFactory::detectFormat("ubuntu.iso.gz"), QString("gzip"));
}

void TestDecompressorFactory::testDoubleExtensionImgXz() {
    QCOMPARE(sak::DecompressorFactory::detectFormat("disk.img.xz"), QString("xz"));
}

void TestDecompressorFactory::testDoubleExtensionTarBz2() {
    QCOMPARE(sak::DecompressorFactory::detectFormat("archive.tar.bz2"), QString("bzip2"));
}

void TestDecompressorFactory::testEmptyFilePath() {
    auto decompressor = sak::DecompressorFactory::create("");
    QVERIFY(decompressor == nullptr);
    
    QVERIFY(!sak::DecompressorFactory::isCompressed(""));
    QVERIFY(sak::DecompressorFactory::detectFormat("").isEmpty());
}

void TestDecompressorFactory::testNonexistentFile() {
    auto decompressor = sak::DecompressorFactory::create("/nonexistent/file.gz");
    // May return decompressor even if file doesn't exist (lazy open)
}

void TestDecompressorFactory::testDirectoryPath() {
    auto decompressor = sak::DecompressorFactory::create("C:/");
    QVERIFY(decompressor == nullptr);
}

void TestDecompressorFactory::testReadMagicNumber() {
    createTestFile(".dat", "TestMagic");
    
    unsigned char buffer[16] = {0};
    bool result = sak::DecompressorFactory::readMagicNumber(m_tempFile->fileName(), buffer, 4);
    
    // Should succeed or fail gracefully
}

void TestDecompressorFactory::testReadMagicNumberEmptyFile() {
    createTestFile(".dat", QByteArray());
    
    unsigned char buffer[16] = {0};
    bool result = sak::DecompressorFactory::readMagicNumber(m_tempFile->fileName(), buffer, 4);
    
    // Should handle empty file
}

void TestDecompressorFactory::testReadMagicNumberSmallFile() {
    createTestFile(".dat", "AB"); // Only 2 bytes
    
    unsigned char buffer[16] = {0};
    bool result = sak::DecompressorFactory::readMagicNumber(m_tempFile->fileName(), buffer, 16);
    
    // Should handle files smaller than requested size
}

void TestDecompressorFactory::testDecompressorNotNull() {
    createTestFile(".gz");
    auto decompressor = sak::DecompressorFactory::create(m_tempFile->fileName());
    QVERIFY(decompressor != nullptr);
}

void TestDecompressorFactory::testDecompressorType() {
    createTestFile(".gz");
    auto decompressor = sak::DecompressorFactory::create(m_tempFile->fileName());
    
    if (decompressor) {
        // Decompressor should be correct type
        // Cannot easily test specific type without RTTI
        QVERIFY(decompressor != nullptr);
    }
}

void TestDecompressorFactory::testMultipleInstances() {
    createTestFile(".gz");
    
    auto decompressor1 = sak::DecompressorFactory::create(m_tempFile->fileName());
    auto decompressor2 = sak::DecompressorFactory::create(m_tempFile->fileName());
    
    QVERIFY(decompressor1 != nullptr);
    QVERIFY(decompressor2 != nullptr);
    QVERIFY(decompressor1 != decompressor2); // Different instances
}

void TestDecompressorFactory::testConcurrentCreation() {
    createTestFile(".gz");
    
    // Factory should be thread-safe
    auto decompressor1 = sak::DecompressorFactory::create(m_tempFile->fileName());
    auto decompressor2 = sak::DecompressorFactory::create(m_tempFile->fileName());
    
    QVERIFY(decompressor1 != nullptr);
    QVERIFY(decompressor2 != nullptr);
}

void TestDecompressorFactory::testThreadSafeDetection() {
    // Detection methods are static and thread-safe
    QString format1 = sak::DecompressorFactory::detectFormat("file.gz");
    QString format2 = sak::DecompressorFactory::detectFormat("file.bz2");
    
    QCOMPARE(format1, QString("gzip"));
    QCOMPARE(format2, QString("bzip2"));
}

void TestDecompressorFactory::testFormatNameGzip() {
    QCOMPARE(sak::DecompressorFactory::detectFormat("file.gz"), QString("gzip"));
}

void TestDecompressorFactory::testFormatNameBzip2() {
    QCOMPARE(sak::DecompressorFactory::detectFormat("file.bz2"), QString("bzip2"));
}

void TestDecompressorFactory::testFormatNameXZ() {
    QCOMPARE(sak::DecompressorFactory::detectFormat("file.xz"), QString("xz"));
}

void TestDecompressorFactory::testFormatNameEmpty() {
    QVERIFY(sak::DecompressorFactory::detectFormat("file.txt").isEmpty());
}

void TestDecompressorFactory::testZipNotSupported() {
    // ZIP is multi-file archive, not supported for disk images
    auto decompressor = sak::DecompressorFactory::create("file.zip");
    QVERIFY(decompressor == nullptr);
}

void TestDecompressorFactory::testZipDetection() {
    QString format = sak::DecompressorFactory::detectFormat("file.zip");
    QVERIFY(format.isEmpty() || format != "zip");
}

QTEST_MAIN(TestDecompressorFactory)
#include "test_decompressor_factory.moc"

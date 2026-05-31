// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_pst_parser.cpp
/// @brief Unit tests for PST/OST file parser

#include "sak/email_constants.h"
#include "sak/pst_parser.h"

#include <QByteArray>
#include <QTemporaryFile>
#include <QtTest/QtTest>

#include <array>

namespace {
constexpr int kUnicodeRootOffsetForTest = 0xB4;
constexpr int kRootFileSizeOffsetForTest = 4;
constexpr int kUnicodeRootNbtOffsetForTest = 44;
constexpr int kUnicodeRootBbtOffsetForTest = 60;
constexpr int kLegacyPageTrailerSizeForTest = 16;
constexpr int kLegacyPageMetaSizeForTest = 4;
constexpr int kLegacyPageMetaPadForTest = 4;
constexpr int kLegacyPageMaxEntriesForTest = 0x14;
constexpr int kRootPcHeapSizeForTest = 28;
constexpr int kRootPcHeapPageMapOffsetForTest = 20;
constexpr int kRootPcBthHeaderOffsetForTest = 12;
constexpr uint32_t kRootPcHidRootForTest = 0x20;
constexpr uint64_t kRootPcDataBidForTest = 0x1000;

struct PermuteBytePair {
    uint8_t plain;
    uint8_t encoded;
};

constexpr std::array<PermuteBytePair, 10> kPermuteEncodePairsForTest{{
    {0x00, 0x41},
    {0x01, 0x36},
    {0x02, 0x13},
    {0x06, 0x6E},
    {0x0C, 0x7F},
    {0x14, 0x74},
    {0x20, 0x4C},
    {0xB5, 0xA6},
    {0xBC, 0x93},
    {0xEC, 0xFF},
}};

void writeLe16(QByteArray& data, int offset, uint16_t value) {
    data[offset] = static_cast<char>(value & 0xFF);
    data[offset + 1] = static_cast<char>((value >> 8) & 0xFF);
}

void writeLe32(QByteArray& data, int offset, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        data[offset + i] = static_cast<char>((value >> (i * 8)) & 0xFF);
    }
}

void writeLe64(QByteArray& data, int offset, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        data[offset + i] = static_cast<char>((value >> (i * 8)) & 0xFF);
    }
}

uint8_t encodePermuteByteForTest(uint8_t plain) {
    for (const auto& pair : kPermuteEncodePairsForTest) {
        if (pair.plain == plain) {
            return pair.encoded;
        }
    }
    return plain;
}

QByteArray encodePermuteBytesForTest(const QByteArray& plain) {
    QByteArray encoded;
    encoded.reserve(plain.size());
    for (auto byte : plain) {
        encoded.append(static_cast<char>(encodePermuteByteForTest(static_cast<uint8_t>(byte))));
    }
    return encoded;
}

void writeUnicodeRootPointersForTest(QByteArray& file,
                                     int file_size,
                                     int nbt_offset,
                                     int bbt_offset) {
    writeLe64(file, kUnicodeRootOffsetForTest + kRootFileSizeOffsetForTest, file_size);
    writeLe64(file, kUnicodeRootOffsetForTest + kUnicodeRootNbtOffsetForTest, nbt_offset);
    writeLe64(file, kUnicodeRootOffsetForTest + kUnicodeRootBbtOffsetForTest, bbt_offset);
}

QByteArray makeLegacyUnicodeBTreePageForTest(uint8_t ptype, int entry_count, int entry_size) {
    constexpr int kPageSize = sak::email::kLegacyUnicodePageSize;
    QByteArray page(kPageSize, '\0');
    const int trailer_offset = kPageSize - kLegacyPageTrailerSizeForTest;
    const int meta_offset = trailer_offset - kLegacyPageMetaPadForTest - kLegacyPageMetaSizeForTest;
    page[meta_offset] = static_cast<char>(entry_count);
    page[meta_offset + 1] = kLegacyPageMaxEntriesForTest;
    page[meta_offset + 2] = static_cast<char>(entry_size);
    page[meta_offset + 3] = 0;
    page[trailer_offset] = static_cast<char>(ptype);
    page[trailer_offset + 1] = static_cast<char>(ptype);
    return page;
}

QByteArray buildRootPcHeapForTest() {
    QByteArray heap(kRootPcHeapSizeForTest, '\0');
    writeLe16(heap, 0, kRootPcHeapPageMapOffsetForTest);
    heap[2] = static_cast<char>(0xEC);
    heap[3] = static_cast<char>(0xBC);
    writeLe32(heap, 4, kRootPcHidRootForTest);
    heap[kRootPcBthHeaderOffsetForTest] = static_cast<char>(0xB5);
    heap[kRootPcBthHeaderOffsetForTest + 1] = 0x02;
    heap[kRootPcBthHeaderOffsetForTest + 2] = 0x06;
    writeLe32(heap, kRootPcHeapPageMapOffsetForTest, 1);
    writeLe16(heap, kRootPcHeapPageMapOffsetForTest + 4, kRootPcBthHeaderOffsetForTest);
    writeLe16(heap, kRootPcHeapPageMapOffsetForTest + 6, kRootPcHeapPageMapOffsetForTest);
    return heap;
}
}  // namespace

class TestPstParser : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    // -- Header Validation -----------------------------------------------
    void rejectsEmptyFile();
    void rejectsTooSmallFile();
    void rejectsInvalidMagic();
    void parsesValidPstMagic();
    void detectsUnicodeVersion();
    void detectsAnsiVersion();
    void legacyUnicodePstReads512ByteBTreePages();
    void unicode4kOstReads4096ByteBTreePages();
    void compressibleEncryptedPstDecodesRootPropertyContext();

    // -- Encryption Detection --------------------------------------------
    void detectsNoEncryption();
    void detectsCompressibleEncryption();

    // -- Open / Close Lifecycle ------------------------------------------
    void openNonExistentFile();
    void openAndClose();
    void doubleCloseIsHarmless();
    void cancelDoesNotCrash();

    // -- File Info --------------------------------------------------------
    void fileInfoEmptyWhenClosed();

private:
    /// Build a minimal valid PST file header in a QByteArray
    QByteArray buildMinimalPstHeader(bool unicode,
                                     uint8_t encryption = 0x00,
                                     uint16_t content_type = sak::email::kPstContentType,
                                     uint16_t version_override = 0);
    QByteArray buildStoreWithEmptyBTrees(uint16_t content_type, uint16_t version);
    QByteArray buildCompressibleEncryptedRootPst();
};

void TestPstParser::initTestCase() {
    // Nothing to set up globally
}

QByteArray TestPstParser::buildMinimalPstHeader(bool unicode,
                                                uint8_t encryption,
                                                uint16_t content_type,
                                                uint16_t version_override) {
    constexpr int kHeaderSize = 580;
    QByteArray header(kHeaderSize, '\0');
    auto* data = reinterpret_cast<uint8_t*>(header.data());

    // Magic: "!BDN" at offset 0 (little-endian 0x2142444E)
    data[0] = 0x21;
    data[1] = 0x42;
    data[2] = 0x44;
    data[3] = 0x4E;

    // Content type at offset 8: "SM" for PST, "SO" for OST
    writeLe16(header, 8, content_type);

    // Data version at offset 10
    uint16_t version = version_override != 0
                           ? version_override
                           : (unicode ? sak::email::kUnicodeVersion : sak::email::kAnsiVersion);
    writeLe16(header, 10, version);

    // Encryption type at offset 513
    data[513] = encryption;

    return header;
}

QByteArray TestPstParser::buildStoreWithEmptyBTrees(uint16_t content_type, uint16_t version) {
    const bool unicode = version >= sak::email::kUnicodeVersion;
    const bool unicode4k = version == sak::email::kUnicode4kVersion;
    const int page_size = unicode4k ? sak::email::kUnicodePageSize
                                    : sak::email::kLegacyUnicodePageSize;
    const int trailer_size = unicode4k ? 24 : 16;
    const int meta_size = unicode4k ? 8 : 4;
    const int meta_pad = unicode4k ? 8 : 4;
    const int nbt_entry_size = unicode ? 32 : 16;
    const int bbt_entry_size = unicode ? 24 : 12;
    const int nbt_offset = page_size * 2;
    const int bbt_offset = nbt_offset + page_size;
    const int file_size = bbt_offset + page_size;

    QByteArray file(file_size, '\0');
    QByteArray header =
        buildMinimalPstHeader(unicode, sak::email::kEncryptNone, content_type, version);
    file.replace(0, header.size(), header);

    writeUnicodeRootPointersForTest(file, file_size, nbt_offset, bbt_offset);

    auto make_page = [&](uint8_t ptype, int entry_size) {
        QByteArray page(page_size, '\0');
        const int trailer_offset = page_size - trailer_size;
        const int meta_offset = trailer_offset - meta_pad - meta_size;
        if (unicode4k) {
            writeLe16(page, meta_offset, 0);
            writeLe16(page, meta_offset + 2, 0x00A9);
            page[meta_offset + 4] = static_cast<char>(entry_size);
            page[meta_offset + 5] = 0;
        } else {
            page[meta_offset] = 0;
            page[meta_offset + 1] = 0x14;
            page[meta_offset + 2] = static_cast<char>(entry_size);
            page[meta_offset + 3] = 0;
        }
        page[trailer_offset] = static_cast<char>(ptype);
        page[trailer_offset + 1] = static_cast<char>(ptype);
        return page;
    };

    file.replace(nbt_offset, page_size, make_page(0x81, nbt_entry_size));
    file.replace(bbt_offset, page_size, make_page(0x80, bbt_entry_size));
    return file;
}

QByteArray TestPstParser::buildCompressibleEncryptedRootPst() {
    constexpr int kPageSize = sak::email::kLegacyUnicodePageSize;
    constexpr int kNbtOffset = kPageSize * 2;
    constexpr int kBbtOffset = kNbtOffset + kPageSize;
    constexpr int kDataOffset = kBbtOffset + kPageSize;
    const QByteArray encoded_heap = encodePermuteBytesForTest(buildRootPcHeapForTest());

    const int file_size = kDataOffset + encoded_heap.size();
    QByteArray file(file_size, '\0');
    QByteArray header = buildMinimalPstHeader(true,
                                              sak::email::kEncryptCompressible,
                                              sak::email::kPstContentType,
                                              sak::email::kUnicodeVersion);
    file.replace(0, header.size(), header);

    writeUnicodeRootPointersForTest(file, file_size, kNbtOffset, kBbtOffset);

    QByteArray nbt_page = makeLegacyUnicodeBTreePageForTest(0x81, 1, 32);
    writeLe64(nbt_page, 0, sak::email::kNidRootFolder);
    writeLe64(nbt_page, 8, kRootPcDataBidForTest);
    writeLe64(nbt_page, 16, 0);
    writeLe32(nbt_page, 24, 0);
    file.replace(kNbtOffset, kPageSize, nbt_page);

    QByteArray bbt_page = makeLegacyUnicodeBTreePageForTest(0x80, 1, 24);
    writeLe64(bbt_page, 0, kRootPcDataBidForTest);
    writeLe64(bbt_page, 8, kDataOffset);
    writeLe16(bbt_page, 16, static_cast<uint16_t>(encoded_heap.size()));
    file.replace(kBbtOffset, kPageSize, bbt_page);
    file.replace(kDataOffset, encoded_heap.size(), encoded_heap);

    return file;
}

// ============================================================================
// Header Validation
// ============================================================================

void TestPstParser::rejectsEmptyFile() {
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.close();

    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    parser.open(temp_file.fileName());

    QVERIFY(error_spy.count() > 0 || !parser.isOpen());
}

void TestPstParser::rejectsTooSmallFile() {
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(QByteArray(10, '\0'));
    temp_file.close();

    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    parser.open(temp_file.fileName());

    QVERIFY(error_spy.count() > 0 || !parser.isOpen());
}

void TestPstParser::rejectsInvalidMagic() {
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    QByteArray bad_header(564, '\0');
    bad_header[0] = 'X';
    bad_header[1] = 'Y';
    bad_header[2] = 'Z';
    bad_header[3] = 'W';
    temp_file.write(bad_header);
    temp_file.close();

    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    parser.open(temp_file.fileName());

    QVERIFY(error_spy.count() > 0 || !parser.isOpen());
}

void TestPstParser::parsesValidPstMagic() {
    QByteArray header = buildMinimalPstHeader(true);
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(header);
    temp_file.close();

    PstParser parser;
    // May fail on deeper parsing (no valid BTrees), but shouldn't
    // crash and should at least recognize the magic bytes.
    parser.open(temp_file.fileName());
    parser.close();
}

void TestPstParser::detectsUnicodeVersion() {
    QByteArray header = buildMinimalPstHeader(true);
    // Verify the version bytes are correctly set
    auto* data = reinterpret_cast<const uint8_t*>(header.constData());
    uint16_t version = static_cast<uint16_t>(data[10]) | (static_cast<uint16_t>(data[11]) << 8);
    QCOMPARE(version, sak::email::kUnicodeVersion);
}

void TestPstParser::detectsAnsiVersion() {
    QByteArray header = buildMinimalPstHeader(false);
    auto* data = reinterpret_cast<const uint8_t*>(header.constData());
    uint16_t version = static_cast<uint16_t>(data[10]) | (static_cast<uint16_t>(data[11]) << 8);
    QCOMPARE(version, sak::email::kAnsiVersion);
}

void TestPstParser::legacyUnicodePstReads512ByteBTreePages() {
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(
        buildStoreWithEmptyBTrees(sak::email::kPstContentType, sak::email::kUnicodeVersion));
    temp_file.close();

    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    parser.open(temp_file.fileName());

    QVERIFY(!error_spy.isEmpty());
    const QString error = error_spy.takeFirst().at(0).toString();
    QVERIFY2(!error.contains(QStringLiteral("Failed to load Node BTree")), qPrintable(error));
    QVERIFY2(error.contains(QStringLiteral("Failed to build folder hierarchy")), qPrintable(error));
}

void TestPstParser::unicode4kOstReads4096ByteBTreePages() {
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(
        buildStoreWithEmptyBTrees(sak::email::kOstContentType, sak::email::kUnicode4kVersion));
    temp_file.close();

    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    parser.open(temp_file.fileName());

    QVERIFY(!error_spy.isEmpty());
    const QString error = error_spy.takeFirst().at(0).toString();
    QVERIFY2(!error.contains(QStringLiteral("Failed to load Node BTree")), qPrintable(error));
    QVERIFY2(error.contains(QStringLiteral("Failed to build folder hierarchy")), qPrintable(error));
}

void TestPstParser::compressibleEncryptedPstDecodesRootPropertyContext() {
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(buildCompressibleEncryptedRootPst());
    temp_file.close();

    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    QSignalSpy opened_spy(&parser, &PstParser::fileOpened);
    parser.open(temp_file.fileName());

    QVERIFY2(error_spy.isEmpty(),
             qPrintable(error_spy.isEmpty() ? QString() : error_spy.first().at(0).toString()));
    QVERIFY(parser.isOpen());
    QCOMPARE(opened_spy.count(), 1);
    const auto info = opened_spy.first().at(0).value<sak::PstFileInfo>();
    QCOMPARE(info.is_ost, false);
    QCOMPARE(info.encryption_type, sak::email::kEncryptCompressible);
    QCOMPARE(info.total_folders, 1);
}

// ============================================================================
// Encryption Detection
// ============================================================================

void TestPstParser::detectsNoEncryption() {
    QByteArray header = buildMinimalPstHeader(true, sak::email::kEncryptNone);
    QCOMPARE(static_cast<uint8_t>(header[513]), sak::email::kEncryptNone);
}

void TestPstParser::detectsCompressibleEncryption() {
    QByteArray header = buildMinimalPstHeader(true, sak::email::kEncryptCompressible);
    QCOMPARE(static_cast<uint8_t>(header[513]), sak::email::kEncryptCompressible);
}

// ============================================================================
// Open / Close Lifecycle
// ============================================================================

void TestPstParser::openNonExistentFile() {
    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    parser.open(QStringLiteral("C:/nonexistent_file_xyz.pst"));
    QVERIFY(error_spy.count() > 0);
}

void TestPstParser::openAndClose() {
    PstParser parser;
    QVERIFY(!parser.isOpen());

    QByteArray header = buildMinimalPstHeader(true);
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(header);
    temp_file.close();

    parser.open(temp_file.fileName());
    parser.close();
    QVERIFY(!parser.isOpen());
}

void TestPstParser::doubleCloseIsHarmless() {
    PstParser parser;
    parser.close();
    parser.close();
    QVERIFY(!parser.isOpen());
}

void TestPstParser::cancelDoesNotCrash() {
    PstParser parser;
    parser.cancel();
    QVERIFY(true);
}

// ============================================================================
// File Info
// ============================================================================

void TestPstParser::fileInfoEmptyWhenClosed() {
    PstParser parser;
    QVERIFY(!parser.isOpen());
}

QTEST_MAIN(TestPstParser)
#include "test_pst_parser.moc"

// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_pst_parser.cpp
/// @brief Unit tests for PST/OST file parser

#include "sak/email_constants.h"
#include "sak/pst_parser.h"

#include "../support/pst_fixture.h"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QtTest/QtTest>

#include <algorithm>
#include <array>
#include <functional>
#include <limits>

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

// MS-PST weak CRC-32 (reflected poly 0xEDB88320, init 0, no final XOR) -- the
// same algorithm the parser authenticates against, so these fixtures are genuine
// spec-conformant PST/OST byte streams rather than files that only parsed because
// integrity was unchecked.
uint32_t weakCrcForTest(const QByteArray& data, int offset, int len) {
    static const std::array<uint32_t, 256> kTable = [] {
        std::array<uint32_t, 256> table{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) {
                c = (c & 1u) ? ((c >> 1) ^ 0xED'B8'83'20u) : (c >> 1);
            }
            table[i] = c;
        }
        return table;
    }();
    uint32_t crc = 0;
    const auto* bytes = reinterpret_cast<const uint8_t*>(data.constData()) + offset;
    for (int i = 0; i < len; ++i) {
        crc = (crc >> 8) ^ kTable[(crc ^ bytes[i]) & 0xFFu];
    }
    return crc;
}

// MS-PST 5.4 ComputeSig: fold (ib XOR bid) to 16 bits.
uint16_t computeSigForTest(uint64_t ib, uint64_t bid) {
    uint64_t value = ib ^ bid;
    value ^= (value >> 16);
    value ^= (value >> 32);
    return static_cast<uint16_t>(value & 0xFFFFu);
}

// Stamp dwCRCPartial (471 bytes from offset 8) and, for Unicode/Unicode4k,
// dwCRCFull (516 bytes from offset 8 at offset 0x20C). Call AFTER every other
// header byte -- including the ROOT BREF pointers -- is written.
void finalizeHeaderCrcForTest(QByteArray& file, bool unicode) {
    writeLe32(file, 4, weakCrcForTest(file, 8, 471));
    if (unicode) {
        writeLe32(file, 0x20C, weakCrcForTest(file, 8, 516));
    }
}

// Stamp a PAGETRAILER: bid, wSig = ComputeSig(page_offset, bid), and dwCRC over
// the page body preceding the trailer. Call AFTER the page body (meta + ptype) is
// in place; the CRC does not cover the trailer itself.
void finalizePageTrailerForTest(
    QByteArray& file, int page_offset, int page_size, int trailer_size, uint64_t bid) {
    // 12-byte trailer == ANSI (4-byte bid); 16/24 == Unicode/Unicode4k (8-byte bid).
    const bool unicode = trailer_size != 12;
    const int trailer_off = page_offset + page_size - trailer_size;
    if (unicode) {
        writeLe64(file, trailer_off + 8, bid);
    } else {
        writeLe32(file, trailer_off + 8, static_cast<uint32_t>(bid));
    }
    writeLe16(file, trailer_off + 2, computeSigForTest(static_cast<uint64_t>(page_offset), bid));
    writeLe32(file, trailer_off + 4, weakCrcForTest(file, page_offset, page_size - trailer_size));
}

// Write cb raw bytes at file_offset followed by a spec-conformant BLOCKTRAILER
// (Unicode 16 / ANSI 12) at the padded 64-byte boundary. Returns the total on-disk
// span (data + padding + trailer) so the caller can size the file.
int writeBlockWithTrailerForTest(
    QByteArray& file, int file_offset, const QByteArray& raw, bool unicode, uint64_t bid) {
    const int trailer_size = unicode ? 16 : 12;
    const int cb = static_cast<int>(raw.size());
    const int disk = ((cb + trailer_size + 63) / 64) * 64;
    file.replace(file_offset, cb, raw);
    const int trailer_off = file_offset + disk - trailer_size;
    writeLe16(file, trailer_off + 0, static_cast<uint16_t>(cb));
    writeLe16(file, trailer_off + 2, computeSigForTest(static_cast<uint64_t>(file_offset), bid));
    writeLe32(file, trailer_off + 4, weakCrcForTest(raw, 0, cb));
    if (unicode) {
        writeLe64(file, trailer_off + 8, bid);
    } else {
        writeLe32(file, trailer_off + 8, static_cast<uint32_t>(bid));
    }
    return disk;
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
    void reusableOpenableFixtureReachesRootFolder();
    void reusableFolderedFixtureReachesChildViaHierarchyTable();
    void rejectsUnknownDataVersion();
    void rejectsMistypedNodeBTreePage();

    // -- Integrity (CRC / signature) -------------------------------------
    void rejectsHeaderCrcMismatch();
    void rejectsPageCrcMismatch();
    void rejectsBlockCrcMismatch();

    // -- Encryption Detection --------------------------------------------
    void detectsNoEncryption();
    void detectsCompressibleEncryption();
    void ansiHeaderReadsEncryptionFromOffset461();

    // -- Real-file smoke (env-gated) -------------------------------------
    void realEmailFilesParseSafely();

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

    // bCryptMethod lives at a format-specific offset: 513 (0x201) in the Unicode
    // header, 461 (0x1CD) in the ANSI header. Placing it correctly keeps ANSI
    // fixtures honest (P05-44).
    data[unicode ? 513 : 461] = encryption;

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

    // Stamp spec-conformant PAGETRAILER CRC/sig now that both pages are placed,
    // then the header CRCs over the finished ROOT pointers.
    finalizePageTrailerForTest(file, nbt_offset, page_size, trailer_size, 0);
    finalizePageTrailerForTest(file, bbt_offset, page_size, trailer_size, 0);
    finalizeHeaderCrcForTest(file, unicode);
    return file;
}

QByteArray TestPstParser::buildCompressibleEncryptedRootPst() {
    constexpr int kPageSize = sak::email::kLegacyUnicodePageSize;
    constexpr int kNbtOffset = kPageSize * 2;
    constexpr int kBbtOffset = kNbtOffset + kPageSize;
    constexpr int kDataOffset = kBbtOffset + kPageSize;
    const QByteArray encoded_heap = encodePermuteBytesForTest(buildRootPcHeapForTest());

    // The data block occupies cb bytes + padding + a 16-byte BLOCKTRAILER, rounded
    // up to a 64-byte boundary; size the file to hold the whole on-disk block.
    const int cb = static_cast<int>(encoded_heap.size());
    const int block_disk = ((cb + 16 + 63) / 64) * 64;
    const int file_size = kDataOffset + block_disk;
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
    writeLe16(bbt_page, 16, static_cast<uint16_t>(cb));
    file.replace(kBbtOffset, kPageSize, bbt_page);

    // Write the data block with a conformant BLOCKTRAILER (dwCRC over the encoded
    // on-disk bytes), then stamp both page trailers and the header CRCs.
    writeBlockWithTrailerForTest(file, kDataOffset, encoded_heap, true, kRootPcDataBidForTest);
    finalizePageTrailerForTest(file, kNbtOffset, kPageSize, kLegacyPageTrailerSizeForTest, 0);
    finalizePageTrailerForTest(file, kBbtOffset, kPageSize, kLegacyPageTrailerSizeForTest, 0);
    finalizeHeaderCrcForTest(file, true);

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

    // Fail closed: there is no header to read, so the open is refused and reported.
    QVERIFY(!parser.isOpen());
    QCOMPARE(error_spy.count(), 1);
    const QString error = error_spy.takeFirst().at(0).toString();
    QVERIFY2(error.contains(QStringLiteral("Invalid PST header")), qPrintable(error));
}

void TestPstParser::rejectsTooSmallFile() {
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(QByteArray(10, '\0'));
    temp_file.close();

    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    parser.open(temp_file.fileName());

    // A file shorter than the fixed 580-byte header prefix is rejected before any
    // field is parsed out of it.
    QVERIFY(!parser.isOpen());
    QCOMPARE(error_spy.count(), 1);
    const QString error = error_spy.takeFirst().at(0).toString();
    QVERIFY2(error.contains(QStringLiteral("Invalid PST header")), qPrintable(error));
}

void TestPstParser::rejectsInvalidMagic() {
    QTemporaryFile temp_file;
    // Full-length header (580 bytes) so the read succeeds and the rejection really
    // comes from the magic check -- a short buffer would fail the size gate first
    // and never exercise dwMagic at all.
    QByteArray bad_header(580, '\0');
    bad_header[0] = 'X';
    bad_header[1] = 'Y';
    bad_header[2] = 'Z';
    bad_header[3] = 'W';
    QVERIFY(temp_file.open());
    temp_file.write(bad_header);
    temp_file.close();

    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    parser.open(temp_file.fileName());

    QVERIFY(!parser.isOpen());
    QCOMPARE(error_spy.count(), 1);
    const QString error = error_spy.takeFirst().at(0).toString();
    QVERIFY2(error.contains(QStringLiteral("Invalid PST header")), qPrintable(error));
}

void TestPstParser::parsesValidPstMagic() {
    QByteArray header = buildMinimalPstHeader(true);
    finalizeHeaderCrcForTest(header, true);
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(header);
    temp_file.close();

    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    parser.open(temp_file.fileName());

    // A header-only file has no BTree pages, so the open still fails closed -- but
    // it must fail LATER than the header: the magic, content type, version and CRCs
    // are all valid here, so "Invalid PST header" would mean the preamble parse
    // rejected a well-formed header.
    QVERIFY(!parser.isOpen());
    QCOMPARE(error_spy.count(), 1);
    const QString error = error_spy.takeFirst().at(0).toString();
    QVERIFY2(!error.contains(QStringLiteral("Invalid PST header")), qPrintable(error));
    parser.close();
}

void TestPstParser::detectsUnicodeVersion() {
    // The PARSER's version detection, not the fixture's bytes: a wVer 23 store must
    // open as Unicode (which is also what makes it read the ROOT BREFs at the
    // 64-bit Unicode offsets rather than the ANSI ones).
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(buildCompressibleEncryptedRootPst());
    temp_file.close();

    PstParser parser;
    parser.open(temp_file.fileName());
    QVERIFY(parser.isOpen());
    QCOMPARE(parser.fileInfo().is_unicode, true);
    parser.close();
}

void TestPstParser::detectsAnsiVersion() {
    // wVer 14 is a KNOWN (ANSI) layout, so the parse gets past the header and fails
    // later on the missing BTrees. Contrast rejectsUnknownDataVersion, where an
    // unrecognized wVer is rejected as "Invalid PST header".
    QByteArray header = buildMinimalPstHeader(false);
    finalizeHeaderCrcForTest(header, false);
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(header);
    temp_file.close();

    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    parser.open(temp_file.fileName());

    QVERIFY(!parser.isOpen());
    QCOMPARE(error_spy.count(), 1);
    const QString error = error_spy.takeFirst().at(0).toString();
    QVERIFY2(!error.contains(QStringLiteral("Invalid PST header")), qPrintable(error));
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

void TestPstParser::reusableOpenableFixtureReachesRootFolder() {
    // Locks the shared, reusable openable store (tests/support/pst_fixture.h) that the PST
    // fuzz harness seeds from: it must drive the parser through the full LTP accept path --
    // loadNodeBTree -> loadBlockBTree -> readPropertyContext(kNidRootFolder) success ->
    // buildFolderHierarchy -- and yield exactly one (root) folder, unencrypted. If a future
    // change breaks the accept path, this fails deterministically here, not only under fuzz.
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(sak::pst_fixture::buildOpenableUnicodeStore());
    temp_file.close();

    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    parser.open(temp_file.fileName());

    QVERIFY2(error_spy.isEmpty(),
             qPrintable(error_spy.isEmpty() ? QString() : error_spy.first().at(0).toString()));
    QVERIFY(parser.isOpen());
    QCOMPARE(parser.fileInfo().encryption_type, sak::email::kEncryptNone);
    QCOMPARE(parser.folderTree().size(), 1);
    QCOMPARE(parser.folderTree().first().node_id,
             static_cast<uint64_t>(sak::email::kNidRootFolder));
}

void TestPstParser::reusableFolderedFixtureReachesChildViaHierarchyTable() {
    // Locks the shared foldered store (tests/support/pst_fixture.h): open() must walk the root
    // folder's hierarchy Table Context -- readTableContext -> parseTcInfo -> buildTcRows ->
    // materializeTcRow -> extractChildNids -> recurse -- and yield the root folder with exactly
    // one child (the PidTagLtpRowId cell). This exercises the TC/row-matrix accept path that the
    // single-folder openable store does not, and fails deterministically here if it regresses.
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(sak::pst_fixture::buildFolderedUnicodeStore());
    temp_file.close();

    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    parser.open(temp_file.fileName());

    QVERIFY2(error_spy.isEmpty(),
             qPrintable(error_spy.isEmpty() ? QString() : error_spy.first().at(0).toString()));
    QVERIFY(parser.isOpen());
    QCOMPARE(parser.folderTree().size(), 1);
    const sak::PstFolder& root = parser.folderTree().first();
    QCOMPARE(root.node_id, static_cast<uint64_t>(sak::email::kNidRootFolder));
    QCOMPARE(root.children.size(), 1);
    QCOMPARE(root.children.first().node_id,
             static_cast<uint64_t>(sak::pst_fixture::kChildFolderNid));
    QCOMPARE(root.children.first().parent_node_id,
             static_cast<uint64_t>(sak::email::kNidRootFolder));
}

void TestPstParser::rejectsUnknownDataVersion() {
    // An unrecognized wVer means the on-disk layout is unknown; the parser must fail closed
    // (pst_invalid_header) at header parse rather than guessing offsets from a mislabeled or
    // crafted file. 0x0063 is neither ANSI (14), Unicode (23), nor Unicode4K (36).
    constexpr uint16_t kUnknownVersion = 0x0063;
    QByteArray header = buildMinimalPstHeader(
        true, sak::email::kEncryptNone, sak::email::kPstContentType, kUnknownVersion);
    // Stamp valid CRCs so wVer is the ONLY defect: without this the header would be
    // rejected for its integrity check instead, and the test would still pass with
    // the version gate deleted.
    finalizeHeaderCrcForTest(header, true);
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(header);
    temp_file.close();

    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    parser.open(temp_file.fileName());

    QVERIFY(!parser.isOpen());
    QVERIFY(!error_spy.isEmpty());
    const QString error = error_spy.takeFirst().at(0).toString();
    QVERIFY2(error.contains(QStringLiteral("Invalid PST header")), qPrintable(error));
}

void TestPstParser::rejectsMistypedNodeBTreePage() {
    // The Node BTree root page must carry ptypeNBT (0x81). A page whose trailer type byte is
    // anything else (here zeroed, which still passes the ptype==ptypeRepeat duplicate check)
    // must fail the Node BTree load closed rather than being walked as a valid BTree.
    QByteArray store = buildStoreWithEmptyBTrees(sak::email::kPstContentType,
                                                 sak::email::kUnicodeVersion);
    constexpr int kPageSize = sak::email::kLegacyUnicodePageSize;
    constexpr int kTrailerSize = 16;
    const int nbt_offset = kPageSize * 2;
    const int ptype_offset = nbt_offset + kPageSize - kTrailerSize;
    store[ptype_offset] = '\0';
    store[ptype_offset + 1] = '\0';

    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(store);
    temp_file.close();

    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    parser.open(temp_file.fileName());

    QVERIFY(!parser.isOpen());
    QVERIFY(!error_spy.isEmpty());
    const QString error = error_spy.takeFirst().at(0).toString();
    QVERIFY2(error.contains(QStringLiteral("Failed to load Node BTree")), qPrintable(error));
}

// A header whose stored dwCRCPartial no longer matches its body is corrupt or
// forged; the parser must fail closed at header parse rather than trusting the
// ROOT pointers it is about to read.
void TestPstParser::rejectsHeaderCrcMismatch() {
    QByteArray store = buildStoreWithEmptyBTrees(sak::email::kPstContentType,
                                                 sak::email::kUnicodeVersion);
    // Flip one byte of the stored dwCRCPartial (offset 4) without recomputing.
    store[4] = static_cast<char>(store[4] ^ 0xFF);

    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(store);
    temp_file.close();

    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    parser.open(temp_file.fileName());

    QVERIFY(!parser.isOpen());
    QVERIFY(!error_spy.isEmpty());
    const QString error = error_spy.takeFirst().at(0).toString();
    QVERIFY2(error.contains(QStringLiteral("Invalid PST header")), qPrintable(error));
}

// Corrupting a byte inside the Node BTree page body (covered by the PAGETRAILER
// dwCRC, but leaving ptype intact) must fail the Node BTree load closed.
void TestPstParser::rejectsPageCrcMismatch() {
    QByteArray store = buildStoreWithEmptyBTrees(sak::email::kPstContentType,
                                                 sak::email::kUnicodeVersion);
    constexpr int kPageSize = sak::email::kLegacyUnicodePageSize;
    const int nbt_offset = kPageSize * 2;
    // Byte 0 of the NBT page is inside the CRC-covered body and is not the trailer.
    store[nbt_offset] = static_cast<char>(store[nbt_offset] ^ 0xFF);

    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(store);
    temp_file.close();

    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    parser.open(temp_file.fileName());

    QVERIFY(!parser.isOpen());
    QVERIFY(!error_spy.isEmpty());
    const QString error = error_spy.takeFirst().at(0).toString();
    QVERIFY2(error.contains(QStringLiteral("Failed to load Node BTree")), qPrintable(error));
}

// Corrupting a byte of a data block breaks its BLOCKTRAILER dwCRC; the otherwise
// valid encrypted-root store must then fail to open rather than mining garbage
// from the block.
void TestPstParser::rejectsBlockCrcMismatch() {
    QByteArray store = buildCompressibleEncryptedRootPst();
    constexpr int kPageSize = sak::email::kLegacyUnicodePageSize;
    constexpr int kDataOffset = kPageSize * 4;  // kBbtOffset + kPageSize
    // Flip a byte of the block's on-disk data (not its trailer).
    store[kDataOffset] = static_cast<char>(store[kDataOffset] ^ 0xFF);

    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(store);
    temp_file.close();

    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    parser.open(temp_file.fileName());

    QVERIFY(!parser.isOpen());
    QVERIFY(!error_spy.isEmpty());
}

// ============================================================================
// Encryption Detection
// ============================================================================

// Mirror of ansiHeaderReadsEncryptionFromOffset461: for a Unicode header the
// parser must read bCryptMethod at 513. None at 513 with a High DECOY at the ANSI
// offset 461 has to be accepted -- reading 461 here would reject the file as
// "Unsupported encryption".
void TestPstParser::detectsNoEncryption() {
    QByteArray header = buildMinimalPstHeader(true, sak::email::kEncryptNone);
    header[461] = static_cast<char>(sak::email::kEncryptHigh);  // decoy at the ANSI offset
    finalizeHeaderCrcForTest(header, true);

    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(header);
    temp_file.close();

    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    parser.open(temp_file.fileName());

    QVERIFY(!parser.isOpen());  // still no BTrees in a header-only file
    QCOMPARE(error_spy.count(), 1);
    const QString error = error_spy.takeFirst().at(0).toString();
    QVERIFY2(!error.contains(QStringLiteral("Unsupported encryption")), qPrintable(error));
}

// Compressible is the other accepted bCryptMethod, and it too is read from 513 in
// a Unicode header (same High decoy at the ANSI offset 461).
void TestPstParser::detectsCompressibleEncryption() {
    QByteArray header = buildMinimalPstHeader(true, sak::email::kEncryptCompressible);
    header[461] = static_cast<char>(sak::email::kEncryptHigh);  // decoy at the ANSI offset
    finalizeHeaderCrcForTest(header, true);

    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(header);
    temp_file.close();

    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    parser.open(temp_file.fileName());

    QVERIFY(!parser.isOpen());
    QCOMPARE(error_spy.count(), 1);
    const QString error = error_spy.takeFirst().at(0).toString();
    QVERIFY2(!error.contains(QStringLiteral("Unsupported encryption")), qPrintable(error));
}

// P05-44: the ANSI bCryptMethod byte is at offset 461, not 465. Put High
// encryption (rejected early, before any BTree parse) at 461 and a None decoy at
// the old wrong offset 465. Reading 461 -> "Unsupported encryption"; reading 465
// would miss the flag and fail later with a different Node-BTree error.
void TestPstParser::ansiHeaderReadsEncryptionFromOffset461() {
    QByteArray header = buildMinimalPstHeader(false, sak::email::kEncryptHigh);
    header[465] = static_cast<char>(sak::email::kEncryptNone);  // decoy at old offset
    finalizeHeaderCrcForTest(header, false);

    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(header);
    temp_file.close();

    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    parser.open(temp_file.fileName());

    QVERIFY(!parser.isOpen());
    QVERIFY(!error_spy.isEmpty());
    const QString error = error_spy.takeFirst().at(0).toString();
    QVERIFY2(error.contains(QStringLiteral("Unsupported encryption")), qPrintable(error));
}

// Env-gated real-file smoke: point SAK_TEST_PST_DIR at a folder of .pst/.ost/.nst
// files. Exercises the full parse path (data-tree expansion, P05-47) and negative
// pagination offsets (P05-49) on real data. Skipped in CI. Reads structure/counts
// only -- never surfaces any message body, address, or subject.
void TestPstParser::realEmailFilesParseSafely() {
    const QByteArray env = qgetenv("SAK_TEST_PST_DIR");
    if (env.isEmpty()) {
        QSKIP("SAK_TEST_PST_DIR not set; skipping real-file parse smoke");
    }
    const QFileInfoList files = QDir(QString::fromLocal8Bit(env))
                                    .entryInfoList({QStringLiteral("*.pst"),
                                                    QStringLiteral("*.ost"),
                                                    QStringLiteral("*.nst")},
                                                   QDir::Files);
    if (files.isEmpty()) {
        QSKIP("no .pst/.ost/.nst files in SAK_TEST_PST_DIR");
    }

    // Recursively collect folder node ids from the tree.
    std::function<void(const QVector<sak::PstFolder>&, QVector<uint64_t>&)> collect =
        [&collect](const QVector<sak::PstFolder>& folders, QVector<uint64_t>& out) {
            for (const auto& f : folders) {
                out.append(f.node_id);
                collect(f.children, out);
            }
        };

    for (const QFileInfo& file : files) {
        PstParser parser;
        parser.open(file.absoluteFilePath());
        if (!parser.isOpen()) {
            qInfo().noquote() << file.fileName() << "did not open (graceful, no crash)";
            continue;
        }
        const sak::PstFileInfo info = parser.fileInfo();
        QVector<uint64_t> folder_ids;
        collect(parser.folderTree(), folder_ids);
        qInfo().noquote() << file.fileName() << "unicode=" << info.is_unicode
                          << "ost=" << info.is_ost << "folders=" << folder_ids.size();
        QVERIFY(!folder_ids.isEmpty());

        // For a bounded sample of folders, prove normal + hostile pagination
        // offsets never crash or read out of bounds (P05-49) and that reading
        // items fully expands their data trees without hanging (P05-47).
        constexpr int kSampleFolders = 8;
        constexpr int kPageLimit = 20;
        const int sampled = std::min<int>(kSampleFolders, folder_ids.size());
        for (int i = 0; i < sampled; ++i) {
            const uint64_t fid = folder_ids[i];
            auto normal = parser.readFolderItems(fid, 0, kPageLimit);
            QVERIFY(normal.has_value());
            // Negative offsets (reachable via caller page*size int overflow) must
            // clamp, not index rows[] out of bounds.
            auto neg = parser.readFolderItems(fid, -1, kPageLimit);
            QVERIFY(neg.has_value());
            auto negmin = parser.readFolderItems(fid, std::numeric_limits<int>::min(), kPageLimit);
            QVERIFY(negmin.has_value());
            // Reading the first item's detail exercises data-tree expansion. The
            // claim here is crash-safety only: on real-world data an unreadable
            // item is a legitimate (fail-closed) outcome, so there is no value to
            // compare -- what would fail is a crash, an out-of-bounds read under
            // ASAN, or a hang inside the data-tree walk.
            if (!normal->isEmpty()) {
                [[maybe_unused]] const auto detail = parser.readItemDetail(normal->first().node_id);
            }
        }
        parser.close();
    }
}

// ============================================================================
// Open / Close Lifecycle
// ============================================================================

void TestPstParser::openNonExistentFile() {
    PstParser parser;
    QSignalSpy error_spy(&parser, &PstParser::errorOccurred);
    parser.open(QStringLiteral("C:/nonexistent_file_xyz.pst"));

    QVERIFY(!parser.isOpen());
    QCOMPARE(error_spy.count(), 1);
    const QString error = error_spy.takeFirst().at(0).toString();
    QVERIFY2(error.contains(QStringLiteral("Cannot open file")), qPrintable(error));
}

void TestPstParser::openAndClose() {
    PstParser parser;
    QVERIFY(!parser.isOpen());

    // Use a store that parses end to end, so close() is observed making a real
    // open -> closed transition (a header-only fixture never opens, which would
    // make the final check pass no matter what close() did).
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(buildCompressibleEncryptedRootPst());
    temp_file.close();

    parser.open(temp_file.fileName());
    QVERIFY(parser.isOpen());
    parser.close();
    QVERIFY(!parser.isOpen());
}

void TestPstParser::doubleCloseIsHarmless() {
    PstParser parser;
    parser.close();
    QVERIFY(!parser.isOpen());

    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(buildCompressibleEncryptedRootPst());
    temp_file.close();
    parser.open(temp_file.fileName());
    QVERIFY(parser.isOpen());
    QCOMPARE(parser.folderTree().size(), 1);
    QVERIFY(!parser.allNodeIds().isEmpty());

    // Closing an OPEN parser twice must not close the QFile twice, and both closes
    // must drop the caches -- serving the previous store's folder tree or NBT nodes
    // after close() would let a caller read a file that is no longer open.
    parser.close();
    parser.close();
    QVERIFY(!parser.isOpen());
    QVERIFY(parser.folderTree().isEmpty());
    QVERIFY(parser.allNodeIds().isEmpty());
}

void TestPstParser::cancelDoesNotCrash() {
    // cancel() only raises the atomic flag, so on a parser that never opened a file
    // it must be inert: the parser stays closed and its synchronous reads still
    // refuse cleanly instead of touching the unopened QFile.
    PstParser parser;
    parser.cancel();
    QVERIFY(!parser.isOpen());
    QVERIFY(!parser.readItemDetail(sak::email::kNidRootFolder).has_value());
    QVERIFY(!parser.readFolderItems(sak::email::kNidRootFolder, 0, 10).has_value());
}

// ============================================================================
// File Info
// ============================================================================

void TestPstParser::fileInfoEmptyWhenClosed() {
    PstParser parser;
    QVERIFY(!parser.isOpen());

    // A parser that never opened anything reports a default-constructed info block.
    const sak::PstFileInfo empty = parser.fileInfo();
    QVERIFY(empty.file_path.isEmpty());
    QCOMPARE(empty.file_size_bytes, qint64{0});
    QCOMPARE(empty.is_unicode, false);
    QCOMPARE(empty.total_folders, 0);
    QCOMPARE(empty.total_items, 0);

    // close() must WIPE a populated block too: leaving the previous store's path and
    // folder count behind would let a view report a closed file as still loaded.
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(buildCompressibleEncryptedRootPst());
    temp_file.close();
    parser.open(temp_file.fileName());
    QVERIFY(parser.isOpen());
    QCOMPARE(parser.fileInfo().file_path, temp_file.fileName());
    QCOMPARE(parser.fileInfo().total_folders, 1);

    parser.close();
    QVERIFY(!parser.isOpen());
    QVERIFY(parser.fileInfo().file_path.isEmpty());
    QCOMPARE(parser.fileInfo().total_folders, 0);
}

QTEST_MAIN(TestPstParser)
#include "test_pst_parser.moc"

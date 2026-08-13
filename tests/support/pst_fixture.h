// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file pst_fixture.h
/// @brief Reusable spec-conformant PST store builders (fuzz seeds + accept-path fixture).
///
/// The single home for building legacy-Unicode PST fixtures the PstParser accepts as genuine.
/// Every header CRC, PAGETRAILER, and BLOCKTRAILER is stamped with the same MS-PST weak CRC-32
/// and ComputeSig the parser authenticates against, so these are real spec-conformant files,
/// not near-misses. Two depths:
///
///  * buildEmptyUnicodeStore(): genuine header + Node/Block BTree PAGETRAILERs but EMPTY BTrees.
///    A mutated seed survives the page-trailer checks, so the parser walks INTO parseBTreePage,
///    verifyPageTrailer (success), and buildFolderHierarchy before failing closed there (no root
///    folder node). A deeper reject-path seed than a header-only one.
///
///  * buildOpenableUnicodeStore(): adds a root-folder NBT entry, a BBT entry, and a Heap-on-Node
///    Property-Context data block, so PstParser::open() SUCCEEDS. A seed built from it drives the
///    LTP/messaging accept path -- readPropertyContext, readHeapOnNode, the folder-tree walk --
///    exercising the SUCCESS branches of the integrity gates, not just their rejects.
///
/// The byte layout mirrors test_pst_parser.cpp's own builders (which additionally cover the
/// ANSI, Unicode4k, and compressible-encrypted variants); this header is the shared, reusable
/// version consumed by both the PST fuzz harness and test_pst_parser's lock-in test.

#pragma once

#include "sak/email_constants.h"

#include <QByteArray>

#include <array>
#include <cstdint>

namespace sak::pst_fixture {

// MS-PST header field offsets (MS-PST 2.2.2.6) and legacy-Unicode page geometry.
inline constexpr int kMagicByte0 = 0x21;  // '!'
inline constexpr int kMagicByte1 = 0x42;  // 'B'
inline constexpr int kMagicByte2 = 0x44;  // 'D'
inline constexpr int kMagicByte3 = 0x4E;  // 'N'
inline constexpr int kContentTypeOffset = 8;
inline constexpr int kVersionOffset = 10;
inline constexpr int kCrcPartialOffset = 4;
inline constexpr int kCrcPartialStart = 8;
inline constexpr int kCrcPartialLen = 471;
inline constexpr int kCrcFullOffset = 0x20C;
inline constexpr int kCrcFullStart = 8;
inline constexpr int kCrcFullLen = 516;
inline constexpr int kCryptOffsetUnicode = 513;
inline constexpr int kHeaderSize = 580;
inline constexpr int kRootOffset = 0xB4;
inline constexpr int kRootFileSizeField = 4;
inline constexpr int kRootNbtField = 44;
inline constexpr int kRootBbtField = 60;

inline constexpr int kPageSize = sak::email::kLegacyUnicodePageSize;  // 512
inline constexpr int kTrailerSize = 16;
inline constexpr int kMetaSize = 4;
inline constexpr int kMetaPad = 4;
inline constexpr int kNbtEntrySize = 32;
inline constexpr int kBbtEntrySize = 24;
inline constexpr int kMaxEntriesPerPage = 0x14;
inline constexpr uint8_t kPtypeNbt = 0x81;
inline constexpr uint8_t kPtypeBbt = 0x80;

inline constexpr uint32_t kWeakCrcPoly = 0xED'B8'83'20u;
inline constexpr int kByteBits = 8;
inline constexpr int kCrcTableSize = 256;
inline constexpr int kSigFold16 = 16;
inline constexpr int kSigFold32 = 32;

// NBTENTRY / BBTENTRY leaf-record field offsets (MS-PST 2.2.2.7.7.3-4, Unicode).
inline constexpr int kNbtEntryNidOffset = 0;
inline constexpr int kNbtEntryDataBidOffset = 8;
inline constexpr int kNbtEntrySubBidOffset = 16;
inline constexpr int kNbtEntryParentOffset = 24;
inline constexpr int kBbtEntryBidOffset = 0;
inline constexpr int kBbtEntryIbOffset = 8;
inline constexpr int kBbtEntryCbOffset = 16;
inline constexpr int kBbtEntryRefOffset = 18;

// Root-folder Property-Context data block: a Heap-on-Node (MS-PST 2.3.1) carrying an
// empty PC BTree-on-Heap (2.3.2). The parser authenticates every one of these fields,
// so their values are the spec, not arbitrary.
inline constexpr uint8_t kHnSignature = 0xEC;        // HNHDR.bSig
inline constexpr uint8_t kPcClientSignature = 0xBC;  // HNHDR.bClientSig for a PC
inline constexpr uint8_t kBthSignature = 0xB5;       // BTHHEADER.bType
inline constexpr int kHnHdrIbHnpmOffset = 0;         // HNHDR.ibHnpm (u16)
inline constexpr int kHnHdrSigOffset = 2;            // HNHDR.bSig
inline constexpr int kHnHdrClientSigOffset = 3;      // HNHDR.bClientSig
inline constexpr int kHnHdrRootHidOffset = 4;        // HNHDR.hidUserRoot (u32)
inline constexpr int kHnHdrSize = 12;                // HNHDR then heap allocations
inline constexpr int kBthHdrOffset = 12;             // first heap allocation == BTHHEADER
inline constexpr int kBthHdrSize = 8;
inline constexpr int kBthKeySizeField = 1;
inline constexpr int kBthDataSizeField = 2;
inline constexpr int kBthIdxLevelsField = 3;
inline constexpr int kBthRootHidField = 4;          // (u32) 0 == empty PC (parser returns {})
inline constexpr int kPcBthKeySize = 2;             // wPropId
inline constexpr int kPcBthDataSize = 6;            // wPropType(2) + dwValueHnid(4)
inline constexpr int kHnPageMapOffset = 20;         // HNPAGEMAP begins here (== ibHnpm)
inline constexpr int kHnPmCallocOffset = 20;        // HNPAGEMAP.cAlloc (u16)
inline constexpr int kHnPmCfreeOffset = 22;         // HNPAGEMAP.cFree (u16)
inline constexpr int kHnPmRgib0Offset = 24;         // rgibAlloc[0] (u16) == BTHHEADER start
inline constexpr int kHnPmRgib1Offset = 26;         // rgibAlloc[1] (u16) == BTHHEADER end
inline constexpr int kRootBlockCb = 28;             // HNHDR + BTHHEADER + HNPAGEMAP
inline constexpr int kRootBlockDiskSize = 64;       // (cb + 16 trailer) rounded up to 64
inline constexpr int kRootBlockTrailerOffset = 48;  // kRootBlockDiskSize - 16
inline constexpr int kBlockTrailerCbField = 0;      // BLOCKTRAILER.cb (u16)
inline constexpr int kBlockTrailerSigField = 2;     // BLOCKTRAILER.wSig (u16)
inline constexpr int kBlockTrailerCrcField = 4;     // BLOCKTRAILER.dwCRC (u32)
inline constexpr int kBlockTrailerBidField = 8;     // BLOCKTRAILER.bid (u64)
inline constexpr uint32_t kRootFolderNid = sak::email::kNidRootFolder;  // 0x122
inline constexpr uint64_t kRootFolderDataBid = 4;  // external (fInternal bit 0x02 clear)
inline constexpr uint32_t kBthHeaderHid = 0x20;    // HID: index 1, block 0 (1 << 5)

// buildOpenableUnicodeStore region offsets (two zero pages of headroom, then NBT / BBT /
// PC block). Exposed so a structure-aware fuzz can mutate one region's BODY and re-stamp
// just that region's integrity fields, keeping the file integral (see the restamp*() below).
inline constexpr int kOpenableNbtOffset = kPageSize * 2;           // 0x400
inline constexpr int kOpenableBbtOffset = kPageSize * 3;           // 0x600
inline constexpr int kOpenableBlockOffset = kPageSize * 4;         // 0x800
inline constexpr int kOpenableFileSize = kOpenableBlockOffset + kRootBlockDiskSize;
inline constexpr int kLeafPageBodyLen = kPageSize - kTrailerSize;  // CRC-covered page body

// Foldered store: a root folder whose hierarchy Table Context lists one child folder, so
// PstParser::open() walks readTableContext -> parseTcInfo -> buildTcRows -> extractChildNids
// -> recurse into the child. The hierarchy TC node NID is (root & ~0x1F) | NID_TYPE_HIERARCHY.
inline constexpr uint32_t kHierarchyTableNid = 0x12D;  // (0x122 & ~0x1F) | 0x0D
inline constexpr uint32_t kChildFolderNid = 0x142;
inline constexpr uint64_t kHierarchyDataBid = 8;     // external (0x02 clear); generic 2nd-block BID
inline constexpr uint64_t kChildFolderDataBid = 12;  // generic 3rd-block BID

// Messaging store: the root folder's CONTENTS Table Context lists one message, so open() + a
// readFolderItems() walk drive readContentsTable -> readTableContext and readItemDetail ->
// readMessage. Same three-block layout as the foldered store; only the TC node type (0x0E
// contents vs 0x0D hierarchy) and the leaf node type (0x04 message vs a folder) differ.
inline constexpr uint32_t kContentsTableNid = 0x12E;  // (0x122 & ~0x1F) | 0x0E
inline constexpr uint32_t kMessageNid = 0x24;         // (1 << 5) | 0x04 normal-message type

// Populated message PC: a non-empty PC BTree-on-Heap with one Subject record whose value is an
// HNID pointing at a heap-stored UTF-16 string. Exercises parsePropertyRecords' variable-type
// branch -> resolveHnid -> formatUnicodeValue -> the Subject detail setter. Three heap allocations:
// the BTHHEADER, a one-record BTH leaf, and the subject string.
inline constexpr uint16_t kSubjectPropId =
    0x0037;                                  // PidTagSubjectW (== sak::email::kPropIdSubject)
inline constexpr int kPcRecordKeySize = 2;   // wPropId
inline constexpr int kPcValueRefOffset = 2;  // dwValueHnid sits key+2 into the record
inline constexpr int kPcRecordSize = 8;      // key(2) + type(2) + HNID(4)
inline constexpr int kMsgBthHdrOffset = kHnHdrSize;                           // 12
inline constexpr int kMsgBthLeafOffset = kMsgBthHdrOffset + kBthHdrSize;      // 20
inline constexpr int kMsgSubjectOffset = kMsgBthLeafOffset + kPcRecordSize;   // 28
inline constexpr int kMsgSubjectCharCount = 4;                                // "FUZZ"
inline constexpr int kMsgSubjectLen = kMsgSubjectCharCount * 2;               // UTF-16LE bytes == 8
inline constexpr int kMsgPageMapOffset = kMsgSubjectOffset + kMsgSubjectLen;  // 36
inline constexpr int kMessagePcCb = kMsgPageMapOffset +
                                    12;           // + HNPAGEMAP(cAlloc/cFree + 4 rgib)==48
inline constexpr uint32_t kMsgBthLeafHid = 0x40;  // HID index 2 (BTH leaf)
inline constexpr uint32_t kMsgSubjectHid = 0x60;  // HID index 3 (subject string)

// Table Context on-heap layout (MS-PST 2.3.4). A single 4-byte column (PidTagLtpRowId, whose
// cell IS the child NID) and no TCROWID BTH (hidRowIndex 0 -> the parser enumerates the one row).
inline constexpr uint8_t kTcClientSignature = 0x7C;  // HNHDR.bClientSig for a TC; also TCINFO.bType
inline constexpr int kTcinfoBTypeOffset = 0;
inline constexpr int kTcinfoColCountOffset = 1;
inline constexpr int kTcinfoRgib4bOffset = 2;
inline constexpr int kTcinfoRgib2bOffset = 4;
inline constexpr int kTcinfoRgib1bOffset = 6;  // CEB offset within a row
inline constexpr int kTcinfoRgibBmOffset = 8;  // row size
inline constexpr int kTcinfoHidRowIndexOffset = 10;
inline constexpr int kTcinfoHnidRowsOffset = 14;
inline constexpr int kTcinfoHeaderLen = 22;
inline constexpr int kTcColDescLen = 8;
inline constexpr int kTcColTypeOffset = 0;
inline constexpr int kTcColPropIdOffset = 2;
inline constexpr int kTcColIbDataOffset = 4;
inline constexpr int kTcColDataSizeOffset = 6;
inline constexpr int kTcColBitIndexOffset = 7;
inline constexpr uint16_t kPidTagLtpRowId = 0x67F2;
inline constexpr int kTcRowCellSize = 4;              // the LtpRowId cell (a 4-byte NID)
inline constexpr int kTcRowSize = 5;                  // 4-byte cell + 1 CEB byte
inline constexpr uint8_t kCebFirstColPresent = 0x80;  // CEB high bit set == column 0 present
inline constexpr uint32_t kTcHidUserRoot = 0x20;      // HID index 1 (TCINFO)
inline constexpr uint32_t kTcHnidRows = 0x40;         // HID index 2 (row matrix)
inline constexpr int kTcinfoOffset = kHnHdrSize;      // 12
inline constexpr int kTcRowMatrixOffset = kTcinfoOffset + kTcinfoHeaderLen + kTcColDescLen;  // 42
inline constexpr int kTcPageMapOffset = kTcRowMatrixOffset + kTcRowSize;                     // 47
inline constexpr int kTcBlockCb = kTcPageMapOffset +
                                  10;  // + HNPAGEMAP(cAlloc/cFree + 3 rgib) == 57
inline constexpr int kTcBlockDiskSize = ((kTcBlockCb + kTrailerSize + 63) / 64) * 64;  // 128

// Foldered store block offsets (same NBT/BBT pages as the openable store; three blocks after).
inline constexpr int kFolderedRootBlockOffset = kOpenableBlockOffset;                     // 0x800
inline constexpr int kFolderedTcBlockOffset = kFolderedRootBlockOffset +
                                              kRootBlockDiskSize;                         // 0x840
inline constexpr int kFolderedChildBlockOffset = kFolderedTcBlockOffset +
                                                 kTcBlockDiskSize;                        // 0x8C0
inline constexpr int kFolderedFileSize = kFolderedChildBlockOffset + kRootBlockDiskSize;  // 0x900

inline void writeLe16(QByteArray& data, int offset, uint16_t value) {
    data[offset] = static_cast<char>(value & 0xFF);
    data[offset + 1] = static_cast<char>((value >> kByteBits) & 0xFF);
}

inline void writeLe32(QByteArray& data, int offset, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        data[offset + i] = static_cast<char>((value >> (i * kByteBits)) & 0xFF);
    }
}

inline void writeLe64(QByteArray& data, int offset, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        data[offset + i] = static_cast<char>((value >> (i * kByteBits)) & 0xFF);
    }
}

// MS-PST weak CRC-32: reflected polynomial, zero init, no final inversion.
inline uint32_t weakCrc(const QByteArray& data, int offset, int len) {
    static const std::array<uint32_t, kCrcTableSize> table = [] {
        std::array<uint32_t, kCrcTableSize> built{};
        for (uint32_t i = 0; i < kCrcTableSize; ++i) {
            uint32_t c = i;
            for (int bit = 0; bit < kByteBits; ++bit) {
                c = (c & 1u) ? (kWeakCrcPoly ^ (c >> 1)) : (c >> 1);
            }
            built[i] = c;
        }
        return built;
    }();
    uint32_t crc = 0;
    for (int i = 0; i < len; ++i) {
        const auto byte = static_cast<uint8_t>(data.at(offset + i));
        crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> kByteBits);
    }
    return crc;
}

// MS-PST 5.4 ComputeSig: fold (ib XOR bid) to 16 bits.
inline uint16_t computeSig(uint64_t ib, uint64_t bid) {
    uint64_t value = ib ^ bid;
    value ^= (value >> kSigFold16);
    value ^= (value >> kSigFold32);
    return static_cast<uint16_t>(value & 0xFFFFu);
}

// A legacy-Unicode BTree page with an empty entry list and a valid PAGETRAILER.
inline QByteArray makeEmptyBTreePage(uint8_t ptype, int entry_size, int page_offset) {
    QByteArray page(kPageSize, '\0');
    const int trailer_offset = kPageSize - kTrailerSize;
    const int meta_offset = trailer_offset - kMetaPad - kMetaSize;
    page[meta_offset] = 0;  // entry count
    page[meta_offset + 1] = static_cast<char>(kMaxEntriesPerPage);
    page[meta_offset + 2] = static_cast<char>(entry_size);
    page[meta_offset + 3] = 0;
    page[trailer_offset] = static_cast<char>(ptype);
    page[trailer_offset + 1] = static_cast<char>(ptype);
    // PAGETRAILER: wSig at +2, dwCRC at +4 (over the page body), bid (8 bytes) at +8 == 0.
    writeLe16(page, trailer_offset + 2, computeSig(static_cast<uint64_t>(page_offset), 0));
    writeLe32(page, trailer_offset + 4, weakCrc(page, 0, kPageSize - kTrailerSize));
    return page;
}

/// Build a legacy-Unicode PST with empty Node and Block BTrees that PstParser::open()
/// accepts: genuine header CRCs and genuine page trailers, so the parser walks into the
/// BTree-load and folder-hierarchy code before finding nothing to enumerate.
inline QByteArray buildEmptyUnicodeStore() {
    const int nbt_offset = kPageSize * 2;
    const int bbt_offset = nbt_offset + kPageSize;
    const int file_size = bbt_offset + kPageSize;

    QByteArray file(file_size, '\0');
    file[0] = static_cast<char>(kMagicByte0);
    file[1] = static_cast<char>(kMagicByte1);
    file[2] = static_cast<char>(kMagicByte2);
    file[3] = static_cast<char>(kMagicByte3);
    writeLe16(file, kContentTypeOffset, sak::email::kPstContentType);
    writeLe16(file, kVersionOffset, sak::email::kUnicodeVersion);
    file[kCryptOffsetUnicode] = static_cast<char>(sak::email::kEncryptNone);

    writeLe64(file, kRootOffset + kRootFileSizeField, static_cast<uint64_t>(file_size));
    writeLe64(file, kRootOffset + kRootNbtField, static_cast<uint64_t>(nbt_offset));
    writeLe64(file, kRootOffset + kRootBbtField, static_cast<uint64_t>(bbt_offset));

    file.replace(nbt_offset, kPageSize, makeEmptyBTreePage(kPtypeNbt, kNbtEntrySize, nbt_offset));
    file.replace(bbt_offset, kPageSize, makeEmptyBTreePage(kPtypeBbt, kBbtEntrySize, bbt_offset));

    // Header CRCs last, over the finished ROOT pointers.
    writeLe32(file, kCrcPartialOffset, weakCrc(file, kCrcPartialStart, kCrcPartialLen));
    writeLe32(file, kCrcFullOffset, weakCrc(file, kCrcFullStart, kCrcFullLen));
    return file;
}

// A legacy-Unicode BTree leaf page holding entry_count entries (level 0) with a valid
// PAGETRAILER. entries (entry_count * entry_size bytes) is copied to page offset 0.
inline QByteArray makeLeafPageWithEntries(
    uint8_t ptype, int entry_size, int page_offset, const QByteArray& entries, int entry_count) {
    QByteArray page(kPageSize, '\0');
    const int trailer_offset = kPageSize - kTrailerSize;
    const int meta_offset = trailer_offset - kMetaPad - kMetaSize;
    page.replace(0, entries.size(), entries);
    page[meta_offset] = static_cast<char>(entry_count);  // cEnt
    page[meta_offset + 1] = static_cast<char>(kMaxEntriesPerPage);
    page[meta_offset + 2] = static_cast<char>(entry_size);
    page[meta_offset + 3] = 0;  // cLevel 0 == leaf
    page[trailer_offset] = static_cast<char>(ptype);
    page[trailer_offset + 1] = static_cast<char>(ptype);
    writeLe16(page, trailer_offset + 2, computeSig(static_cast<uint64_t>(page_offset), 0));
    writeLe32(page, trailer_offset + 4, weakCrc(page, 0, kPageSize - kTrailerSize));
    return page;
}

// One-entry convenience wrapper (the openable store's NBT/BBT each carry a single entry).
inline QByteArray makeLeafPageWithEntry(uint8_t ptype,
                                        int entry_size,
                                        int page_offset,
                                        const QByteArray& entry_bytes) {
    return makeLeafPageWithEntries(ptype, entry_size, page_offset, entry_bytes, 1);
}

// The on-disk span of a block: cb data bytes + a 16-byte BLOCKTRAILER, rounded up to 64.
inline int blockDiskSize(int cb) {
    return ((cb + kTrailerSize + 63) / 64) * 64;
}

// Stamp a BLOCKTRAILER at the end of the block that begins at block_pos in buf: cb, wSig from
// (sig_offset, bid), dwCRC over the cb data bytes, and the bid. The single source for the
// trailer formula, used by every block builder and by the structure-fuzz re-stamp.
inline void stampBlockTrailer(
    QByteArray& buf, int block_pos, int cb, uint64_t bid, uint64_t sig_offset) {
    const int t = block_pos + blockDiskSize(cb) - kTrailerSize;
    writeLe16(buf, t + kBlockTrailerCbField, static_cast<uint16_t>(cb));
    writeLe16(buf, t + kBlockTrailerSigField, computeSig(sig_offset, bid));
    writeLe32(buf, t + kBlockTrailerCrcField, weakCrc(buf, block_pos, cb));
    writeLe64(buf, t + kBlockTrailerBidField, bid);
}

// The root/child folder's data block: a Heap-on-Node whose PC BTree-on-Heap has hidRoot == 0
// (an empty property context). PstParser::readPropertyContext walks HNHDR -> BTHHEADER, sees the
// zero root HID, and returns an empty-but-valid PC -- which is what lets buildFolderHierarchy
// succeed for that folder NID. A genuine BLOCKTRAILER authenticates the 64-byte disk region.
inline QByteArray buildRootFolderPcBlock(uint64_t file_offset, uint64_t bid) {
    QByteArray blk(kRootBlockDiskSize, '\0');
    // HNHDR
    writeLe16(blk, kHnHdrIbHnpmOffset, static_cast<uint16_t>(kHnPageMapOffset));
    blk[kHnHdrSigOffset] = static_cast<char>(kHnSignature);
    blk[kHnHdrClientSigOffset] = static_cast<char>(kPcClientSignature);
    writeLe32(blk, kHnHdrRootHidOffset, kBthHeaderHid);
    // BTHHEADER (heap allocation 1): empty PC -> root HID 0.
    blk[kBthHdrOffset] = static_cast<char>(kBthSignature);
    blk[kBthHdrOffset + kBthKeySizeField] = static_cast<char>(kPcBthKeySize);
    blk[kBthHdrOffset + kBthDataSizeField] = static_cast<char>(kPcBthDataSize);
    blk[kBthHdrOffset + kBthIdxLevelsField] = 0;
    writeLe32(blk, kBthHdrOffset + kBthRootHidField, 0);
    // HNPAGEMAP: one allocation spanning [kBthHdrOffset, kHnPageMapOffset).
    writeLe16(blk, kHnPmCallocOffset, 1);
    writeLe16(blk, kHnPmCfreeOffset, 0);
    writeLe16(blk, kHnPmRgib0Offset, static_cast<uint16_t>(kBthHdrOffset));
    writeLe16(blk, kHnPmRgib1Offset, static_cast<uint16_t>(kHnPageMapOffset));
    stampBlockTrailer(blk, 0, kRootBlockCb, bid, file_offset);
    return blk;
}

// A single-row Table Context data block: an HN (client sig 0x7C) whose TCINFO declares one
// PidTagLtpRowId column and one row whose cell is @p row_value. hidRowIndex is 0, so the parser
// enumerates the single physical row. As a hierarchy table row_value is a child folder NID; as a
// contents table it is a message NID -- the byte layout is identical, only the owning node differs.
inline QByteArray buildSingleRowTcBlock(uint64_t file_offset, uint64_t bid, uint32_t row_value) {
    QByteArray blk(blockDiskSize(kTcBlockCb), '\0');
    // HNHDR -> TCINFO as the user root.
    writeLe16(blk, kHnHdrIbHnpmOffset, static_cast<uint16_t>(kTcPageMapOffset));
    blk[kHnHdrSigOffset] = static_cast<char>(kHnSignature);
    blk[kHnHdrClientSigOffset] = static_cast<char>(kTcClientSignature);
    writeLe32(blk, kHnHdrRootHidOffset, kTcHidUserRoot);
    // TCINFO (heap allocation 1): one 4-byte column, row size 5, CEB at offset 4.
    const int ti = kTcinfoOffset;
    blk[ti + kTcinfoBTypeOffset] = static_cast<char>(kTcClientSignature);
    blk[ti + kTcinfoColCountOffset] = 1;
    writeLe16(blk, ti + kTcinfoRgib4bOffset, static_cast<uint16_t>(kTcRowCellSize));
    writeLe16(blk, ti + kTcinfoRgib2bOffset, static_cast<uint16_t>(kTcRowCellSize));
    writeLe16(blk, ti + kTcinfoRgib1bOffset, static_cast<uint16_t>(kTcRowCellSize));
    writeLe16(blk, ti + kTcinfoRgibBmOffset, static_cast<uint16_t>(kTcRowSize));
    writeLe32(blk, ti + kTcinfoHidRowIndexOffset, 0);
    writeLe32(blk, ti + kTcinfoHnidRowsOffset, kTcHnidRows);
    // Column descriptor (PtypInteger32 so the 4 cell bytes are read literally, not HNID-resolved).
    const int col = ti + kTcinfoHeaderLen;
    writeLe16(blk, col + kTcColTypeOffset, sak::email::kPropTypeInt32);
    writeLe16(blk, col + kTcColPropIdOffset, kPidTagLtpRowId);
    writeLe16(blk, col + kTcColIbDataOffset, 0);
    blk[col + kTcColDataSizeOffset] = static_cast<char>(kTcRowCellSize);
    blk[col + kTcColBitIndexOffset] = 0;
    // Row matrix (heap allocation 2): the row-value cell + a CEB marking column 0 present.
    writeLe32(blk, kTcRowMatrixOffset, row_value);
    blk[kTcRowMatrixOffset + kTcRowCellSize] = static_cast<char>(kCebFirstColPresent);
    // HNPAGEMAP: two allocations -- TCINFO [12, 42) and the row matrix [42, 47).
    writeLe16(blk, kTcPageMapOffset, 2);
    writeLe16(blk, kTcPageMapOffset + 2, 0);
    writeLe16(blk, kTcPageMapOffset + 4, static_cast<uint16_t>(kTcinfoOffset));
    writeLe16(blk, kTcPageMapOffset + 6, static_cast<uint16_t>(kTcRowMatrixOffset));
    writeLe16(blk, kTcPageMapOffset + 8, static_cast<uint16_t>(kTcPageMapOffset));
    stampBlockTrailer(blk, 0, kTcBlockCb, bid, file_offset);
    return blk;
}

// A message's data block: a Heap-on-Node whose PC BTree-on-Heap carries ONE property record --
// the Subject (a variable-length Unicode string). readPropertyContext walks HNHDR -> BTHHEADER ->
// the BTH leaf, and for the variable-type record resolves the record's HNID to the heap-stored
// UTF-16 string, so readMessage/readItemProperties return a populated subject. The subject text is
// "FUZZ" (see kMsgSubject* constants). A genuine BLOCKTRAILER authenticates the disk region.
inline QByteArray buildMessagePcBlock(uint64_t file_offset, uint64_t bid) {
    QByteArray blk(blockDiskSize(kMessagePcCb), '\0');
    // HNHDR -> BTHHEADER as the user root.
    writeLe16(blk, kHnHdrIbHnpmOffset, static_cast<uint16_t>(kMsgPageMapOffset));
    blk[kHnHdrSigOffset] = static_cast<char>(kHnSignature);
    blk[kHnHdrClientSigOffset] = static_cast<char>(kPcClientSignature);
    writeLe32(blk, kHnHdrRootHidOffset, kBthHeaderHid);
    // BTHHEADER (heap allocation 1): key 2 / data 6, root HID -> the one-record leaf.
    blk[kMsgBthHdrOffset] = static_cast<char>(kBthSignature);
    blk[kMsgBthHdrOffset + kBthKeySizeField] = static_cast<char>(kPcBthKeySize);
    blk[kMsgBthHdrOffset + kBthDataSizeField] = static_cast<char>(kPcBthDataSize);
    blk[kMsgBthHdrOffset + kBthIdxLevelsField] = 0;
    writeLe32(blk, kMsgBthHdrOffset + kBthRootHidField, kMsgBthLeafHid);
    // BTH leaf (heap allocation 2): one PC record -- propId, type Unicode, then the value HNID.
    writeLe16(blk, kMsgBthLeafOffset, kSubjectPropId);
    writeLe16(blk, kMsgBthLeafOffset + kPcRecordKeySize, sak::email::kPropTypeUnicode);
    writeLe32(blk, kMsgBthLeafOffset + kPcRecordKeySize + kPcValueRefOffset, kMsgSubjectHid);
    // Subject string (heap allocation 3): "FUZZ" as UTF-16LE.
    const char subject[] = "FUZZ";
    for (int i = 0; i < kMsgSubjectCharCount; ++i) {
        writeLe16(blk, kMsgSubjectOffset + (i * 2), static_cast<uint16_t>(subject[i]));
    }
    // HNPAGEMAP: three allocations.
    writeLe16(blk, kMsgPageMapOffset, 3);
    writeLe16(blk, kMsgPageMapOffset + 2, 0);
    writeLe16(blk, kMsgPageMapOffset + 4, static_cast<uint16_t>(kMsgBthHdrOffset));
    writeLe16(blk, kMsgPageMapOffset + 6, static_cast<uint16_t>(kMsgBthLeafOffset));
    writeLe16(blk, kMsgPageMapOffset + 8, static_cast<uint16_t>(kMsgSubjectOffset));
    writeLe16(blk, kMsgPageMapOffset + 10, static_cast<uint16_t>(kMsgPageMapOffset));
    stampBlockTrailer(blk, 0, kMessagePcCb, bid, file_offset);
    return blk;
}

// Re-stamp a legacy leaf page's PAGETRAILER over its (possibly mutated) body: recompute
// dwCRC over [page_offset, page_offset + kLeafPageBodyLen) and wSig from (page_offset, bid 0).
// A structure-aware fuzz mutates the page BODY then calls this so the file stays integral and
// the parser accepts the trailer and walks the corrupt entries/meta -- the accept-path code.
inline void restampLeafPageTrailer(QByteArray& file, int page_offset) {
    const int trailer_offset = page_offset + kPageSize - kTrailerSize;
    writeLe16(file, trailer_offset + 2, computeSig(static_cast<uint64_t>(page_offset), 0));
    writeLe32(file, trailer_offset + 4, weakCrc(file, page_offset, kLeafPageBodyLen));
}

// Re-stamp the PC block's BLOCKTRAILER over its (possibly mutated) kRootBlockCb data bytes.
inline void restampBlockTrailer(QByteArray& file, int block_offset, uint64_t bid) {
    stampBlockTrailer(file, block_offset, kRootBlockCb, bid, static_cast<uint64_t>(block_offset));
}

// Re-stamp the header dwCRCPartial + dwCRCFull. Call after any header-body change.
inline void restampHeaderCrc(QByteArray& file) {
    writeLe32(file, kCrcPartialOffset, weakCrc(file, kCrcPartialStart, kCrcPartialLen));
    writeLe32(file, kCrcFullOffset, weakCrc(file, kCrcFullStart, kCrcFullLen));
}

// A Unicode NBTENTRY: node NID -> data BID, no sub-node, no parent.
inline QByteArray makeNbtEntry(uint32_t nid, uint64_t data_bid) {
    QByteArray entry(kNbtEntrySize, '\0');
    writeLe64(entry, kNbtEntryNidOffset, nid);
    writeLe64(entry, kNbtEntryDataBidOffset, data_bid);
    writeLe64(entry, kNbtEntrySubBidOffset, 0);
    writeLe32(entry, kNbtEntryParentOffset, 0);
    return entry;
}

// A Unicode BBTENTRY: block BID -> file offset + cb.
inline QByteArray makeBbtEntry(uint64_t bid, int file_offset, int cb) {
    QByteArray entry(kBbtEntrySize, '\0');
    writeLe64(entry, kBbtEntryBidOffset, bid);
    writeLe64(entry, kBbtEntryIbOffset, static_cast<uint64_t>(file_offset));
    writeLe16(entry, kBbtEntryCbOffset, static_cast<uint16_t>(cb));
    writeLe16(entry, kBbtEntryRefOffset, 2);  // cRef (>=2; not validated by the reader)
    return entry;
}

// Magic, content type, Unicode version, no-encryption, and the ROOT BREFs (file size / NBT / BBT).
inline void writeUnicodeStoreHeader(QByteArray& file, int nbt_offset, int bbt_offset) {
    file[0] = static_cast<char>(kMagicByte0);
    file[1] = static_cast<char>(kMagicByte1);
    file[2] = static_cast<char>(kMagicByte2);
    file[3] = static_cast<char>(kMagicByte3);
    writeLe16(file, kContentTypeOffset, sak::email::kPstContentType);
    writeLe16(file, kVersionOffset, sak::email::kUnicodeVersion);
    file[kCryptOffsetUnicode] = static_cast<char>(sak::email::kEncryptNone);
    writeLe64(file, kRootOffset + kRootFileSizeField, static_cast<uint64_t>(file.size()));
    writeLe64(file, kRootOffset + kRootNbtField, static_cast<uint64_t>(nbt_offset));
    writeLe64(file, kRootOffset + kRootBbtField, static_cast<uint64_t>(bbt_offset));
}

/// Build a legacy-Unicode PST that PstParser::open() ACCEPTS: the Node BTree carries a
/// single leaf entry for the root folder (kNidRootFolder), the Block BTree maps that
/// node's data BID to a Heap-on-Node PC block, and every CRC/signature is genuine. Unlike
/// buildEmptyUnicodeStore (which fails closed in buildFolderHierarchy for want of a root
/// node), this drives the parser all the way into the LTP/messaging accept path --
/// readPropertyContext, readHeapOnNode, and the folder-tree walk -- so a fuzz seed built
/// from it exercises the success branches of the integrity gates, not just their rejects.
inline QByteArray buildOpenableUnicodeStore() {
    const int nbt_offset = kOpenableNbtOffset;
    const int bbt_offset = kOpenableBbtOffset;
    const int block_offset = kOpenableBlockOffset;
    const int file_size = kOpenableFileSize;

    QByteArray file(file_size, '\0');
    writeUnicodeStoreHeader(file, nbt_offset, bbt_offset);

    file.replace(nbt_offset,
                 kPageSize,
                 makeLeafPageWithEntry(kPtypeNbt,
                                       kNbtEntrySize,
                                       nbt_offset,
                                       makeNbtEntry(kRootFolderNid, kRootFolderDataBid)));
    file.replace(
        bbt_offset,
        kPageSize,
        makeLeafPageWithEntry(kPtypeBbt,
                              kBbtEntrySize,
                              bbt_offset,
                              makeBbtEntry(kRootFolderDataBid, block_offset, kRootBlockCb)));
    file.replace(block_offset,
                 kRootBlockDiskSize,
                 buildRootFolderPcBlock(static_cast<uint64_t>(block_offset), kRootFolderDataBid));

    restampHeaderCrc(file);
    return file;
}

// Whether the leaf node's PC is empty (a folder) or a populated message (a Subject record).
enum class LeafPc {
    Empty,
    Message
};

/// Build a legacy-Unicode PST with three nodes: the root folder PC, a single-row Table Context
/// (@p tc_nid), and a leaf node (@p leaf_nid, also the TC row's value). @p leaf_kind selects the
/// leaf's PC -- an empty folder PC or a populated message PC. Whether the TC is a hierarchy table
/// (open() recurses into a child folder) or a contents table (readFolderItems() lists a message)
/// is decided by tc_nid's type; the block layout is identical (both leaf PCs are a 64-byte block).
inline QByteArray buildStoreWithSingleRowTc(uint32_t tc_nid, uint32_t leaf_nid, LeafPc leaf_kind) {
    const int nbt_offset = kOpenableNbtOffset;
    const int bbt_offset = kOpenableBbtOffset;
    const int root_block = kFolderedRootBlockOffset;   // root PC, disk 64
    const int tc_block = kFolderedTcBlockOffset;       // single-row TC, disk 128
    const int leaf_block = kFolderedChildBlockOffset;  // leaf PC, disk 64
    const int leaf_cb = (leaf_kind == LeafPc::Message) ? kMessagePcCb : kRootBlockCb;

    QByteArray file(kFolderedFileSize, '\0');
    writeUnicodeStoreHeader(file, nbt_offset, bbt_offset);

    const QByteArray nbt = makeNbtEntry(kRootFolderNid, kRootFolderDataBid) +
                           makeNbtEntry(tc_nid, kHierarchyDataBid) +
                           makeNbtEntry(leaf_nid, kChildFolderDataBid);
    file.replace(nbt_offset,
                 kPageSize,
                 makeLeafPageWithEntries(kPtypeNbt, kNbtEntrySize, nbt_offset, nbt, 3));

    const QByteArray bbt = makeBbtEntry(kRootFolderDataBid, root_block, kRootBlockCb) +
                           makeBbtEntry(kHierarchyDataBid, tc_block, kTcBlockCb) +
                           makeBbtEntry(kChildFolderDataBid, leaf_block, leaf_cb);
    file.replace(bbt_offset,
                 kPageSize,
                 makeLeafPageWithEntries(kPtypeBbt, kBbtEntrySize, bbt_offset, bbt, 3));

    file.replace(root_block,
                 kRootBlockDiskSize,
                 buildRootFolderPcBlock(static_cast<uint64_t>(root_block), kRootFolderDataBid));
    file.replace(
        tc_block,
        kTcBlockDiskSize,
        buildSingleRowTcBlock(static_cast<uint64_t>(tc_block), kHierarchyDataBid, leaf_nid));
    const QByteArray leaf_pc =
        (leaf_kind == LeafPc::Message)
            ? buildMessagePcBlock(static_cast<uint64_t>(leaf_block), kChildFolderDataBid)
            : buildRootFolderPcBlock(static_cast<uint64_t>(leaf_block), kChildFolderDataBid);
    file.replace(leaf_block, kRootBlockDiskSize, leaf_pc);

    restampHeaderCrc(file);
    return file;
}

/// The root folder has ONE child folder, reached through a hierarchy Table Context. open() walks
/// loadChildFolders -> readTableContext -> parseTcInfo -> buildTcRows -> extractChildNids and
/// recurses into the child's (empty) PC -- TC/row-matrix code the openable store never reaches.
inline QByteArray buildFolderedUnicodeStore() {
    return buildStoreWithSingleRowTc(kHierarchyTableNid, kChildFolderNid, LeafPc::Empty);
}

/// The root folder has a CONTENTS Table Context listing ONE message, and the message's PC carries
/// a Subject. readFolderItems(root) drives readContentsTable -> the summary loop, and
/// readItemDetail(message) drives readMessage -> readPropertyContext -> the populated-PC path
/// (parsePropertyRecords variable-type -> resolveHnid -> formatUnicodeValue) no folder-only store
/// reaches.
inline QByteArray buildMessagingUnicodeStore() {
    return buildStoreWithSingleRowTc(kContentsTableNid, kMessageNid, LeafPc::Message);
}

}  // namespace sak::pst_fixture

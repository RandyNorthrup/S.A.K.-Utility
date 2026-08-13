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

// A legacy-Unicode BTree leaf page holding exactly one entry (count 1, level 0) with a
// valid PAGETRAILER. entry_bytes is copied to page offset 0 and must be entry_size long.
inline QByteArray makeLeafPageWithEntry(uint8_t ptype,
                                        int entry_size,
                                        int page_offset,
                                        const QByteArray& entry_bytes) {
    QByteArray page(kPageSize, '\0');
    const int trailer_offset = kPageSize - kTrailerSize;
    const int meta_offset = trailer_offset - kMetaPad - kMetaSize;
    page.replace(0, entry_bytes.size(), entry_bytes);
    page[meta_offset] = 1;  // cEnt: one leaf entry
    page[meta_offset + 1] = static_cast<char>(kMaxEntriesPerPage);
    page[meta_offset + 2] = static_cast<char>(entry_size);
    page[meta_offset + 3] = 0;  // cLevel 0 == leaf
    page[trailer_offset] = static_cast<char>(ptype);
    page[trailer_offset + 1] = static_cast<char>(ptype);
    writeLe16(page, trailer_offset + 2, computeSig(static_cast<uint64_t>(page_offset), 0));
    writeLe32(page, trailer_offset + 4, weakCrc(page, 0, kPageSize - kTrailerSize));
    return page;
}

// The root folder's data block: a Heap-on-Node whose PC BTree-on-Heap has hidRoot == 0
// (an empty property context). PstParser::readPropertyContext walks HNHDR -> BTHHEADER,
// sees the zero root HID, and returns an empty-but-valid PC -- which is what lets
// buildFolderHierarchy(kNidRootFolder) succeed. A genuine BLOCKTRAILER (cb/wSig/dwCRC/bid)
// authenticates the block. The 64-byte disk region is (cb + 16) rounded to 64.
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
    // BLOCKTRAILER over the kRootBlockCb data bytes.
    const int t = kRootBlockTrailerOffset;
    writeLe16(blk, t + kBlockTrailerCbField, static_cast<uint16_t>(kRootBlockCb));
    writeLe16(blk, t + kBlockTrailerSigField, computeSig(file_offset, bid));
    writeLe32(blk, t + kBlockTrailerCrcField, weakCrc(blk, 0, kRootBlockCb));
    writeLe64(blk, t + kBlockTrailerBidField, bid);
    return blk;
}

/// Build a legacy-Unicode PST that PstParser::open() ACCEPTS: the Node BTree carries a
/// single leaf entry for the root folder (kNidRootFolder), the Block BTree maps that
/// node's data BID to a Heap-on-Node PC block, and every CRC/signature is genuine. Unlike
/// buildEmptyUnicodeStore (which fails closed in buildFolderHierarchy for want of a root
/// node), this drives the parser all the way into the LTP/messaging accept path --
/// readPropertyContext, readHeapOnNode, and the folder-tree walk -- so a fuzz seed built
/// from it exercises the success branches of the integrity gates, not just their rejects.
inline QByteArray buildOpenableUnicodeStore() {
    const int nbt_offset = kPageSize * 2;
    const int bbt_offset = nbt_offset + kPageSize;
    const int block_offset = bbt_offset + kPageSize;
    const int file_size = block_offset + kRootBlockDiskSize;

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

    // NBTENTRY for the root folder -> data BID, no sub-node, no parent.
    QByteArray nbt_entry(kNbtEntrySize, '\0');
    writeLe64(nbt_entry, kNbtEntryNidOffset, kRootFolderNid);
    writeLe64(nbt_entry, kNbtEntryDataBidOffset, kRootFolderDataBid);
    writeLe64(nbt_entry, kNbtEntrySubBidOffset, 0);
    writeLe32(nbt_entry, kNbtEntryParentOffset, 0);
    file.replace(nbt_offset,
                 kPageSize,
                 makeLeafPageWithEntry(kPtypeNbt, kNbtEntrySize, nbt_offset, nbt_entry));

    // BBTENTRY mapping that BID to the PC block region.
    QByteArray bbt_entry(kBbtEntrySize, '\0');
    writeLe64(bbt_entry, kBbtEntryBidOffset, kRootFolderDataBid);
    writeLe64(bbt_entry, kBbtEntryIbOffset, static_cast<uint64_t>(block_offset));
    writeLe16(bbt_entry, kBbtEntryCbOffset, static_cast<uint16_t>(kRootBlockCb));
    writeLe16(bbt_entry, kBbtEntryRefOffset, 2);  // cRef (>=2; not validated by the reader)
    file.replace(bbt_offset,
                 kPageSize,
                 makeLeafPageWithEntry(kPtypeBbt, kBbtEntrySize, bbt_offset, bbt_entry));

    file.replace(block_offset,
                 kRootBlockDiskSize,
                 buildRootFolderPcBlock(static_cast<uint64_t>(block_offset), kRootFolderDataBid));

    writeLe32(file, kCrcPartialOffset, weakCrc(file, kCrcPartialStart, kCrcPartialLen));
    writeLe32(file, kCrcFullOffset, weakCrc(file, kCrcFullStart, kCrcFullLen));
    return file;
}

}  // namespace sak::pst_fixture

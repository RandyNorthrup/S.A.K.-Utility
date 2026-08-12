// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file pst_fixture.h
/// @brief Reusable builder for a spec-conformant PST store fixture (BTree-layer fuzz seed).
///
/// The PST fuzz seeds that carry only a CRC-valid header fail closed at the first
/// PAGETRAILER, so a mutated file rarely reaches past PstParser::loadNodeBTree. This builder
/// produces a legacy-Unicode PST whose header CRCs AND whose Node/Block BTree PAGETRAILERs
/// are all genuine (the same MS-PST weak CRC-32 and ComputeSig the parser authenticates
/// against), with empty BTrees. As a fuzz seed it survives the page-trailer checks, so the
/// parser walks INTO parseBTreePage, verifyPageTrailer (its success path), and
/// buildFolderHierarchy before failing closed there -- a layer the header-only seeds never
/// reach.
///
/// NOTE: empty BTrees mean PstParser::open() ultimately REJECTS this (no root folder node
/// to build the hierarchy from), so it is a deeper reject-path seed, not an accept-path one.
/// Making open() SUCCEED needs a full message-store + root-folder fixture (a much larger
/// build, the LTP/messaging fuzz increment). The byte layout mirrors test_pst_parser.cpp's
/// buildStoreWithEmptyBTrees; this header is the shared, reusable version.

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

}  // namespace sak::pst_fixture

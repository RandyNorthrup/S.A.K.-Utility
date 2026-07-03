// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file apfs_lzbitmap_codec.h
/// @brief Clean-room codec for Apple's LZBITMAP whole-buffer stream format.
///
/// APFS stores LZBITMAP-compressed file content (com.apple.decmpfs algorithm 14)
/// in a com.apple.ResourceFork data stream. Each 64 KiB logical block of the file
/// is an independent LZBITMAP stream in the format decoded here: a 4-byte magic
/// ("ZBM\x09"), a sequence of chunks, and a terminating zero-length chunk. Every
/// chunk decompresses to at most 0x8000 (32 KiB) bytes, so a full 64 KiB block is
/// two chunks plus the terminator.
///
/// This is an original, Qt-free reimplementation written from the on-disk format
/// (an interface, not a copyrightable expression). The byte layout was
/// cross-referenced against the MIT-licensed libzbitmap by Corellium LLC
/// (Ernesto A. Fernandez) and Apple's apfsprogs `apfsck/compress.c`, which is the
/// validator this codec must satisfy; none of that code is reproduced here.
///
/// A chunk header is two little-endian 24-bit integers: `len` (total chunk bytes
/// including the header) and `decmp_len` (decompressed size). A chunk is stored
/// uncompressed when `len == decmp_len + 6`. A compressed chunk additionally
/// carries three 24-bit metadata-area offsets, then a data area, three metadata
/// areas, and a trailing 17-byte packed bitmap table at the chunk's end. Decoding
/// walks eight output bytes at a time: a per-group 8-bit bitmap selects, for each
/// byte, either a fresh literal from the data area or a copy from `period` bytes
/// earlier in the output; a nibble stream (metadata area 3) names which bitmap to
/// apply and how many times to repeat it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sak::zbm {

// On-disk format constants (Apple LZBITMAP; factual interface values).
inline constexpr int kMagicSize = 4;
inline constexpr uint8_t kMagic[kMagicSize] = {0x5A, 0x42, 0x4D, 0x09};  // "ZBM\x09"
inline constexpr uint32_t kMaxDecmpChunkSize = 0x8000;                   // 32 KiB per chunk
inline constexpr int kChunkHeaderSize = 6;                               // two le24 fields
inline constexpr int kCompressedChunkHeaderSize = 15;                    // header + 3 le24 offsets
inline constexpr int kBitmapCount = 16 - 1 - 3;                          // 12 selectable bitmaps
inline constexpr int kBitmapBase = 3;         // bitmap numbers >= 3 index the table
inline constexpr int kBitmapTableBytes = 17;  // 12 * 10 bits, byte-padded
inline constexpr int kMaxPeriodByteCount = 2;

// A bitmap plus how many bytes of a fresh repetition period accompany it.
struct Bitmap {
    uint8_t bitmap{0};
    uint8_t period_bytecnt{0};
};

// Read a little-endian 24-bit integer at @off (caller guarantees 3 bytes).
[[nodiscard]] inline uint32_t readU24(const uint8_t* p, size_t off) {
    return static_cast<uint32_t>(p[off]) | (static_cast<uint32_t>(p[off + 1]) << 8) |
           (static_cast<uint32_t>(p[off + 2]) << 16);
}

// Write a little-endian 24-bit integer at @off.
inline void writeU24(uint8_t* p, size_t off, uint32_t value) {
    p[off] = static_cast<uint8_t>(value);
    p[off + 1] = static_cast<uint8_t>(value >> 8);
    p[off + 2] = static_cast<uint8_t>(value >> 16);
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

// A cursor over half-bytes (nibbles) within the source buffer.
struct NibbleCursor {
    const uint8_t* base{nullptr};
    size_t addr{0};
    int nibble{0};
};

// Decoder working state for one whole-buffer LZBITMAP stream.
struct DecodeState {
    const uint8_t* src{nullptr};  // whole source buffer
    size_t src_size{0};

    uint8_t* dest{nullptr};  // whole destination buffer
    size_t dest_size{0};

    size_t cursor{0};       // start of the current chunk in src
    size_t src_left{0};     // bytes of src after cursor
    size_t chunk_end{0};    // one past the current chunk in src
    uint32_t len{0};        // total bytes of the current chunk
    uint32_t decmp_len{0};  // decompressed size of the current chunk

    size_t out_pos{0};      // total bytes written across all chunks
    uint32_t written{0};    // bytes written for the current chunk
    uint16_t period{8};     // active back-reference period

    size_t data{0};         // next literal byte in the current chunk
    size_t meta_1{0};       // period area
    size_t meta_2{0};       // spill-bitmap area
    NibbleCursor meta_3{};  // bitmap-selector nibble stream

    Bitmap bitmaps[kBitmapCount]{};
};

[[nodiscard]] inline bool zbmCheckMagic(DecodeState& s) {
    if (s.src_left < static_cast<size_t>(kMagicSize)) {
        return false;
    }
    if (std::memcmp(s.src + s.cursor, kMagic, kMagicSize) != 0) {
        return false;
    }
    s.cursor += kMagicSize;
    s.src_left -= kMagicSize;
    return true;
}

// Pull one nibble from the cursor; false if it would read at/after @limit.
[[nodiscard]] inline bool readNibble(const uint8_t* base,
                                     NibbleCursor& n,
                                     size_t limit,
                                     uint8_t* out) {
    if (n.addr >= limit) {
        return false;
    }
    if (n.nibble == 0) {
        *out = base[n.addr] & 0x0F;
        n.nibble = 1;
    } else {
        *out = (base[n.addr] >> 4) & 0x0F;
        n.nibble = 0;
        ++n.addr;
    }
    return true;
}

inline void rewindNibble(NibbleCursor& n) {
    if (n.nibble == 0) {
        n.nibble = 1;
        --n.addr;
    } else {
        n.nibble = 0;
    }
}

// A stored (header-only) chunk: copy its decmp_len raw bytes verbatim.
[[nodiscard]] inline bool zbmDecodeStoredChunk(DecodeState& s) {
    s.data = s.cursor + kChunkHeaderSize;
    if (s.data + s.decmp_len > s.src_size || s.out_pos + s.decmp_len > s.dest_size) {
        return false;
    }
    std::memcpy(s.dest + s.out_pos, s.src + s.data, s.decmp_len);
    s.out_pos += s.decmp_len;
    s.written = s.decmp_len;
    return true;
}

// Read a fresh repetition period (period_bytecnt bytes) from metadata area 1.
[[nodiscard]] inline bool zbmLoadPeriod(DecodeState& s, Bitmap bm) {
    if (!bm.period_bytecnt) {
        return true;
    }
    s.period = 0;
    for (int i = 0; i < bm.period_bytecnt; ++i) {
        if (s.meta_1 >= s.chunk_end) {
            return false;
        }
        s.period |= static_cast<uint16_t>(s.src[s.meta_1]) << (i * 8);
        ++s.meta_1;
    }
    return true;
}

// Apply one bitmap over up to eight output bytes.
[[nodiscard]] inline bool zbmApplyBitmap(DecodeState& s, Bitmap bm) {
    if (!zbmLoadPeriod(s, bm)) {
        return false;
    }
    if (s.period == 0) {
        return false;
    }
    for (int i = 0; i < 8; ++i) {
        if (s.written == s.decmp_len) {
            break;
        }
        if (bm.bitmap & (1 << i)) {
            if (s.data >= s.chunk_end || s.out_pos >= s.dest_size) {
                return false;
            }
            s.dest[s.out_pos] = s.src[s.data];
            ++s.data;
        } else {
            if (s.out_pos < s.period) {
                return false;
            }
            s.dest[s.out_pos] = s.dest[s.out_pos - s.period];
        }
        ++s.out_pos;
        ++s.written;
    }
    return true;
}

// Resolve a bitmap number: >= 3 indexes the trailing table; 0..2 pulls a fresh
// bitmap from metadata area 2 with that many period bytes. 0xF is invalid here.
[[nodiscard]] inline bool zbmApplyBitmapNumber(DecodeState& s, uint8_t num) {
    if (num == 0x0F) {
        return false;
    }
    if (num > kMaxPeriodByteCount) {
        return zbmApplyBitmap(s, s.bitmaps[num - kBitmapBase]);
    }
    if (s.meta_2 >= s.chunk_end) {
        return false;
    }
    Bitmap next{};
    next.bitmap = s.src[s.meta_2];
    next.period_bytecnt = num;
    ++s.meta_2;
    return zbmApplyBitmap(s, next);
}

// Read how many times to repeat the pending bitmap number (counts start at 4).
[[nodiscard]] inline bool zbmReadRepetitionCount(DecodeState& s, uint16_t* repeat) {
    if (s.decmp_len - s.written <= 8) {
        *repeat = 1;
        return true;
    }
    uint8_t nibble = 0;
    if (!readNibble(s.src, s.meta_3, s.chunk_end, &nibble)) {
        return false;
    }
    if (nibble != 0x0F) {
        rewindNibble(s.meta_3);
        *repeat = 1;
        return true;
    }
    uint16_t total = 4;
    while (nibble == 0x0F) {
        if (!readNibble(s.src, s.meta_3, s.chunk_end, &nibble)) {
            return false;
        }
        total = static_cast<uint16_t>(total + nibble);
        if (total < nibble) {
            return false;
        }
    }
    *repeat = total;
    return true;
}

[[nodiscard]] inline bool zbmDecodeSingleBitmap(DecodeState& s) {
    uint8_t num = 0;
    if (!readNibble(s.src, s.meta_3, s.chunk_end, &num)) {
        return false;
    }
    uint16_t repeat = 0;
    if (!zbmReadRepetitionCount(s, &repeat)) {
        return false;
    }
    for (uint16_t i = 0; i < repeat; ++i) {
        if (!zbmApplyBitmapNumber(s, num)) {
            return false;
        }
    }
    return true;
}

// A cursor over single bits within the source buffer.
struct BitCursor {
    size_t addr{0};
    int offset{0};
};

[[nodiscard]] inline int readSingleBit(const uint8_t* p, BitCursor& b) {
    int res = (p[b.addr] >> b.offset) & 1;
    ++b.offset;
    if (b.offset == 8) {
        b.offset = 0;
        ++b.addr;
    }
    return res;
}

[[nodiscard]] inline bool zbmReadSingleBitmap(const uint8_t* p,
                                              BitCursor& b,
                                              size_t limit,
                                              Bitmap* out) {
    out->bitmap = 0;
    out->period_bytecnt = 0;
    for (int i = 0; i < 8; ++i) {
        if (b.addr >= limit) {
            return false;
        }
        out->bitmap |= static_cast<uint8_t>(readSingleBit(p, b) << i);
    }
    for (int i = 0; i < 2; ++i) {
        if (b.addr >= limit) {
            return false;
        }
        out->period_bytecnt |= static_cast<uint8_t>(readSingleBit(p, b) << i);
    }
    return true;
}

// The 12 selectable bitmaps live packed in the last 17 bytes of the chunk.
[[nodiscard]] inline bool zbmReadBitmaps(DecodeState& s) {
    if (s.len < static_cast<uint32_t>(kBitmapTableBytes)) {
        return false;
    }
    BitCursor bc{};
    bc.addr = s.chunk_end - kBitmapTableBytes;
    bc.offset = 0;
    for (int i = 0; i < kBitmapCount; ++i) {
        if (!zbmReadSingleBitmap(s.src, bc, s.chunk_end, &s.bitmaps[i])) {
            return false;
        }
        if (s.bitmaps[i].period_bytecnt > kMaxPeriodByteCount) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool zbmDecodeCompressedChunk(DecodeState& s) {
    s.written = 0;
    s.period = 8;
    if (s.len < static_cast<uint32_t>(kCompressedChunkHeaderSize)) {
        return false;
    }
    s.data = s.cursor + kCompressedChunkHeaderSize;
    const uint32_t off1 = readU24(s.src, s.cursor + 6);
    const uint32_t off2 = readU24(s.src, s.cursor + 9);
    const uint32_t off3 = readU24(s.src, s.cursor + 12);
    if (off1 >= s.len || off2 >= s.len || off3 >= s.len) {
        return false;
    }
    s.meta_1 = s.cursor + off1;
    s.meta_2 = s.cursor + off2;
    s.meta_3.base = s.src;
    s.meta_3.addr = s.cursor + off3;
    s.meta_3.nibble = 0;
    if (!zbmReadBitmaps(s)) {
        return false;
    }
    while (s.written < s.decmp_len) {
        if (!zbmDecodeSingleBitmap(s)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool zbmDecodeChunk(DecodeState& s) {
    if (s.src_left < static_cast<size_t>(kChunkHeaderSize)) {
        return false;
    }
    s.len = readU24(s.src, s.cursor);
    if (s.len < static_cast<uint32_t>(kChunkHeaderSize) || s.len > s.src_left) {
        return false;
    }
    s.chunk_end = s.cursor + s.len;
    s.decmp_len = readU24(s.src, s.cursor + 3);
    if (s.decmp_len > kMaxDecmpChunkSize) {
        return false;
    }
    if (s.decmp_len == 0) {
        return true;  // terminator
    }
    if (s.out_pos + s.decmp_len > s.dest_size) {
        return false;
    }
    if (s.len == s.decmp_len + kChunkHeaderSize) {
        return zbmDecodeStoredChunk(s);
    }
    return zbmDecodeCompressedChunk(s);
}

// Decompress a whole LZBITMAP stream into @out (resized to the decoded length).
// Returns false on any malformed input. @maxOut bounds the output for safety.
[[nodiscard]] inline bool zbmDecompress(const uint8_t* src,
                                        size_t srcSize,
                                        size_t maxOut,
                                        std::vector<uint8_t>& out) {
    out.assign(maxOut, 0);
    DecodeState s{};
    s.src = src;
    s.src_size = srcSize;
    s.dest = out.data();
    s.dest_size = maxOut;
    s.src_left = srcSize;
    if (!zbmCheckMagic(s)) {
        return false;
    }
    do {
        if (!zbmDecodeChunk(s)) {
            return false;
        }
        s.cursor += s.len;
        s.src_left -= s.len;
    } while (s.decmp_len != 0);
    out.resize(s.out_pos);
    return true;
}

}  // namespace sak::zbm

#include "apfs_lzbitmap_encode.h"  // NOLINT(build/include) encoder shares the constants above

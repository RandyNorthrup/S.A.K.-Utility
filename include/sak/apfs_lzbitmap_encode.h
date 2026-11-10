// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file apfs_lzbitmap_encode.h
/// @brief Clean-room encoder for Apple's LZBITMAP whole-buffer stream format.
///
/// Companion to apfs_lzbitmap_codec.h (which defines the shared constants and the
/// decoder). The encoder produces a stream that Apple's kernel and apfsck decode:
/// a 4-byte magic, one compressed chunk per 32 KiB of input (falling back to a
/// stored chunk when compression would not help), and a zero-length terminator.
///
/// Original code written from the on-disk format; the layout was cross-referenced
/// against the MIT-licensed libzbitmap (Corellium LLC / Ernesto A. Fernandez).
/// The compressor has latitude in *how* it chooses bitmaps and periods, so the
/// output need not be byte-identical to any reference; it need only decode back to
/// the input, which apfsck and the decoder here both verify.

#pragma once

#include "apfs_lzbitmap_codec.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <vector>

namespace sak::zbm {

inline constexpr int kMaxBitmapsPerChunk = kMaxDecmpChunkSize >> 3;  // 4096 groups
inline constexpr int kPossibleBmprots = 1 << 10;                     // (bitmap<<2)|pbc
inline constexpr int kCantCompress = -1024;

// Load up to eight bytes little-endian at @p, zero-filling past @end (safe near
// the buffer tail; the extra bytes only steer a cost heuristic, never decoding).
[[nodiscard]] inline uint64_t loadU64(const uint8_t* p, const uint8_t* end) {
    uint64_t v = 0;
    const size_t n = static_cast<size_t>(end - p) < 8 ? static_cast<size_t>(end - p) : 8;
    for (size_t i = 0; i < n; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (i * 8);
    }
    return v;
}

[[nodiscard]] inline int bmprot(Bitmap b) {
    return (static_cast<int>(b.bitmap) << 2) | static_cast<int>(b.period_bytecnt);
}

// Count how many of the eight bytes differ between two 64-bit words.
[[nodiscard]] inline uint8_t compareBytes(uint64_t a, uint64_t b) {
    const uint64_t x = a ^ b;
    uint8_t diff = 0;
    for (int i = 0; i < 8; ++i) {
        diff = static_cast<uint8_t>(diff + ((x & (0xFFULL << (i * 8))) != 0));
    }
    return diff;
}

struct EncodeState {
    const uint8_t* src{nullptr};
    uint32_t read{0};
    uint32_t written{0};
    uint8_t* dest{nullptr};
    uint16_t period{8};

    const uint8_t* dest_end{nullptr};
    const uint8_t* src_end{nullptr};
    uint32_t decmp_len{0};

    size_t dest_left{0};
    size_t preread{0};
    size_t prewritten{0};
    size_t src_left{0};

    const uint8_t* src_base{nullptr};   // start of the whole input block
    const uint8_t* src_limit{nullptr};  // end of the whole input block

    std::vector<Bitmap> bitmaps;
    std::vector<uint16_t> periods;
    int bmp_cnt{0};
    std::vector<uint64_t> usecnts;
    Bitmap top_bitmaps[kBitmapCount]{};

    EncodeState()
        : bitmaps(kMaxBitmapsPerChunk), periods(kMaxBitmapsPerChunk), usecnts(kPossibleBmprots) {}
};

inline bool writeMagic(EncodeState& s) {
    if (s.dest_left < static_cast<size_t>(kMagicSize)) {
        return false;
    }
    std::memcpy(s.dest, kMagic, kMagicSize);
    s.dest += kMagicSize;
    s.dest_left -= kMagicSize;
    s.prewritten += kMagicSize;
    return true;
}

// Fall back to a stored chunk: undo the attempt and copy the raw bytes.
inline void buildStoredChunk(EncodeState& s) {
    s.src -= s.read;
    s.read = 0;
    s.dest -= s.written - kChunkHeaderSize;
    s.written = kChunkHeaderSize;
    std::memcpy(s.dest, s.src, s.decmp_len);
    s.src += s.decmp_len;
    s.read += s.decmp_len;
    s.written += s.decmp_len;
    s.dest += s.decmp_len;
}

inline void appendNewBitmap(EncodeState& s, uint8_t bitmap, uint16_t period) {
    Bitmap& nb = s.bitmaps[s.bmp_cnt];
    nb.bitmap = bitmap;
    s.periods[s.bmp_cnt] = period;
    if (period == s.period) {
        nb.period_bytecnt = 0;
    } else if (period <= 0xFF) {
        nb.period_bytecnt = 1;
    } else {
        nb.period_bytecnt = 2;
    }
    ++s.bmp_cnt;
    ++s.usecnts[bmprot(nb)];
    s.period = period;
}

[[nodiscard]] inline uint8_t calculateBitmap(const uint8_t* a, const uint8_t* b, int size) {
    uint8_t bitmap = 0;
    for (int i = 0; i < size; ++i) {
        if (a[i] != b[i]) {
            bitmap = static_cast<uint8_t>(bitmap | (1 << i));
        }
    }
    return bitmap;
}

// Best back-reference found so far: the source pointer and its estimated cost.
struct PatternMatch {
    const uint8_t* ref{nullptr};
    int cost{0};
};

// Search for a 1-byte-period back-reference (up to 0xFF bytes back) that beats @m.cost.
inline void searchOneBytePattern(const EncodeState& s,
                                 uint64_t needle,
                                 const uint8_t* from,
                                 PatternMatch& m) {
    for (const uint8_t* p = from; p <= s.src - 8; ++p) {
        const int diff = compareBytes(loadU64(p, s.src_limit), needle);
        if (diff + 1 < m.cost) {
            m.ref = p;
            m.cost = diff + 1;
        }
    }
}

// Search for a 2-byte-period back-reference (up to 0xFFFF back) starting with the same byte.
inline void searchTwoBytePattern(const EncodeState& s,
                                 uint64_t needle,
                                 const uint8_t* from,
                                 const uint8_t* split,
                                 PatternMatch& m) {
    const uint8_t* p = from;
    while (p < split && m.cost > 2) {
        const auto* next =
            static_cast<const uint8_t*>(std::memchr(p, s.src[0], static_cast<size_t>(split - p)));
        if (!next) {
            break;
        }
        const int diff = compareBytes(loadU64(next, s.src_limit), needle);
        if (diff + 2 < m.cost) {
            m.ref = next;
            m.cost = diff + 2;
        }
        p = next + 1;
    }
}

// Choose a back-reference source for the next up-to-eight bytes, estimating cost
// as period digits plus changed bytes, then record the resulting bitmap+period.
inline void findGoodPattern(EncodeState& s) {
    const int bytecnt = static_cast<int>(std::min<uint64_t>(8, s.decmp_len - s.read));
    uint64_t needle = 0;
    for (int i = 0; i < bytecnt; ++i) {
        needle |= static_cast<uint64_t>(s.src[i]) << (i * 8);
    }
    PatternMatch m{s.src - s.period, 0};
    m.cost = compareBytes(loadU64(m.ref, s.src_limit), needle);
    const size_t history = static_cast<size_t>(s.src - s.src_base);
    if (m.cost > 1) {
        const uint8_t* split = s.src - std::min<size_t>(history, 0xFF);
        searchOneBytePattern(s, needle, split, m);
        if (m.cost > 2) {
            searchTwoBytePattern(s, needle, s.src - std::min<size_t>(history, 0xFFFF), split, m);
        }
    }
    const uint8_t bitmap = calculateBitmap(m.ref, s.src, bytecnt);
    appendNewBitmap(s, bitmap, static_cast<uint16_t>(s.src - m.ref));
}

[[nodiscard]] inline int writeNewData(EncodeState& s) {
    const Bitmap& bm = s.bitmaps[s.bmp_cnt - 1];
    for (int i = 0; i < 8; ++i) {
        if (s.src == s.src_end) {
            break;
        }
        if (s.dest == s.dest_end) {
            return kCantCompress;
        }
        if (bm.bitmap & (1 << i)) {
            *s.dest = *s.src;
            ++s.dest;
            ++s.written;
        }
        ++s.src;
        ++s.read;
    }
    return 0;
}

[[nodiscard]] inline int compressEightBytes(EncodeState& s) {
    findGoodPattern(s);
    return writeNewData(s);
}

[[nodiscard]] inline int buildInitialBitmap(EncodeState& s) {
    Bitmap& init = s.bitmaps[0];
    init.bitmap = 0xFF;
    init.period_bytecnt = 0;
    s.bmp_cnt = 1;
    s.usecnts[bmprot(init)] = 1;
    if (s.decmp_len - s.read < 8) {
        return kCantCompress;
    }
    std::memcpy(s.dest, s.src, 8);
    s.dest += 8;
    s.src += 8;
    s.read += 8;
    s.written += 8;
    return 0;
}

[[nodiscard]] inline int writeMetadata1(EncodeState& s) {
    for (int i = 0; i < s.bmp_cnt; ++i) {
        const Bitmap& bm = s.bitmaps[i];
        const uint16_t period = s.periods[i];
        if (bm.period_bytecnt == 0) {
            continue;
        }
        if (s.dest == s.dest_end) {
            return kCantCompress;
        }
        *s.dest = static_cast<uint8_t>(period);
        ++s.dest;
        ++s.written;
        if (bm.period_bytecnt == 1) {
            continue;
        }
        if (s.dest == s.dest_end) {
            return kCantCompress;
        }
        *s.dest = static_cast<uint8_t>(period >> 8);
        ++s.dest;
        ++s.written;
    }
    return 0;
}

[[nodiscard]] inline int writeMetadata2(EncodeState& s) {
    for (int i = 0; i < s.bmp_cnt; ++i) {
        const Bitmap& bm = s.bitmaps[i];
        if (s.usecnts[bmprot(bm)] == 0) {
            continue;  // a top bitmap, stored in the trailing table instead
        }
        if (s.dest == s.dest_end) {
            return kCantCompress;
        }
        *s.dest = bm.bitmap;
        ++s.dest;
        ++s.written;
    }
    return 0;
}

[[nodiscard]] inline bool equalBitmaps(const Bitmap* a, const Bitmap* b) {
    return a && b && a->bitmap == b->bitmap && a->period_bytecnt == b->period_bytecnt;
}

[[nodiscard]] inline int bitmapNumForIndex(const EncodeState& s, int idx) {
    const Bitmap& bm = s.bitmaps[idx];
    if (s.usecnts[bmprot(bm)] == 0) {
        for (int i = 0; i < kBitmapCount; ++i) {
            if (equalBitmaps(&s.top_bitmaps[i], &bm)) {
                return i + kBitmapBase;
            }
        }
    }
    return bm.period_bytecnt;
}

[[nodiscard]] inline int bitmapNumAndRepeat(const EncodeState& s, int idx, int* repeat) {
    const int num = bitmapNumForIndex(s, idx);
    *repeat = 1;
    for (int i = idx + 1; i < s.bmp_cnt; ++i) {
        if (bitmapNumForIndex(s, i) != num) {
            break;
        }
        *repeat += 1;
    }
    return num;
}

[[nodiscard]] inline int writeNibble(uint8_t*& addr, int& half, const uint8_t* limit, uint8_t v) {
    if (addr >= limit) {
        return kCantCompress;
    }
    if (half == 0) {
        *addr = v;
        half = 1;
    } else {
        *addr = static_cast<uint8_t>(*addr | (v << 4));
        half = 0;
        ++addr;
    }
    return 0;
}

[[nodiscard]] inline int writeRepetitionCount(uint8_t*& addr,
                                              int& half,
                                              const uint8_t* limit,
                                              int repeat) {
    int nibble = 0x0F;
    int err = writeNibble(addr, half, limit, static_cast<uint8_t>(nibble));
    if (err) {
        return err;
    }
    repeat -= 4;
    while (repeat > 0) {
        nibble = repeat < 0x0F ? repeat : 0x0F;
        err = writeNibble(addr, half, limit, static_cast<uint8_t>(nibble));
        if (err) {
            return err;
        }
        repeat -= nibble;
    }
    if (nibble == 0x0F) {
        err = writeNibble(addr, half, limit, 0);
        if (err) {
            return err;
        }
    }
    return 0;
}

[[nodiscard]] inline int writeMetadata3(EncodeState& s) {
    uint8_t* addr = s.dest;
    int half = 0;
    int repeat = 0;
    for (int i = 0; i < s.bmp_cnt; i += repeat) {
        const int num = bitmapNumAndRepeat(s, i, &repeat);
        int err = writeNibble(addr, half, s.dest_end, static_cast<uint8_t>(num));
        if (err) {
            return err;
        }
        if (repeat <= 3) {
            for (int j = 1; j < repeat; ++j) {
                err = writeNibble(addr, half, s.dest_end, static_cast<uint8_t>(num));
                if (err) {
                    return err;
                }
            }
        } else {
            err = writeRepetitionCount(addr, half, s.dest_end, repeat);
            if (err) {
                return err;
            }
        }
    }
    if (half == 1) {
        ++addr;
    }
    s.written += static_cast<uint32_t>(addr - s.dest);
    s.dest = addr;
    return 0;
}

inline void writeSingleBit(uint8_t*& addr, int& offset, int val) {
    *addr = static_cast<uint8_t>(*addr | (val << offset));
    ++offset;
    if (offset == 8) {
        offset = 0;
        ++addr;
    }
}

inline void writeSingleBitmap(uint8_t*& addr, int& offset, Bitmap bm) {
    for (int i = 0; i < 8; ++i) {
        writeSingleBit(addr, offset, (bm.bitmap >> i) & 1);
    }
    for (int i = 0; i < 2; ++i) {
        writeSingleBit(addr, offset, (bm.period_bytecnt >> i) & 1);
    }
}

[[nodiscard]] inline int writeTrailingBitmaps(EncodeState& s) {
    if (s.dest_end - s.dest < kBitmapTableBytes) {
        return kCantCompress;
    }
    std::memset(s.dest, 0, kBitmapTableBytes);
    uint8_t* addr = s.dest;
    int offset = 0;
    for (int i = 0; i < kBitmapCount; ++i) {
        writeSingleBitmap(addr, offset, s.top_bitmaps[i]);
    }
    s.dest += kBitmapTableBytes;
    s.written += kBitmapTableBytes;
    return 0;
}

struct TopBitmap {
    Bitmap* bmap{nullptr};
    uint64_t usecnt{0};
};

// Keep the array sorted by use count, descending, deduplicating equal bitmaps.
inline void insertInTopBitmaps(Bitmap* bmap, uint64_t usecnt, TopBitmap tops[kBitmapCount]) {
    int i = 0;
    for (; i < kBitmapCount; ++i) {
        if (equalBitmaps(tops[i].bmap, bmap)) {
            return;
        }
        if (tops[i].usecnt < usecnt) {
            break;
        }
    }
    if (i == kBitmapCount) {
        return;
    }
    std::memmove(&tops[i + 1],
                 &tops[i],
                 static_cast<size_t>(kBitmapCount - i - 1) * sizeof(tops[0]));
    tops[i].bmap = bmap;
    tops[i].usecnt = usecnt;
}

inline void findMostCommonBitmaps(EncodeState& s) {
    TopBitmap tops[kBitmapCount] = {};
    for (int i = 0; i < s.bmp_cnt; ++i) {
        insertInTopBitmaps(&s.bitmaps[i], s.usecnts[bmprot(s.bitmaps[i])], tops);
    }
    for (int i = 0; i < kBitmapCount; ++i) {
        if (tops[i].usecnt == 0) {
            s.top_bitmaps[i].bitmap = 0;
            s.top_bitmaps[i].period_bytecnt = 0;
            continue;
        }
        s.top_bitmaps[i] = *tops[i].bmap;
        s.usecnts[bmprot(*tops[i].bmap)] = 0;  // now lives in the trailing table
    }
}

[[nodiscard]] inline int buildMetadata(EncodeState& s, uint8_t* hdr) {
    findMostCommonBitmaps(s);
    writeU24(hdr, 6, s.written);
    int err = writeMetadata1(s);
    if (err) {
        return err;
    }
    writeU24(hdr, 9, s.written);
    err = writeMetadata2(s);
    if (err) {
        return err;
    }
    writeU24(hdr, 12, s.written);
    err = writeMetadata3(s);
    if (err) {
        return err;
    }
    return writeTrailingBitmaps(s);
}

[[nodiscard]] inline int buildCompressedChunk(EncodeState& s) {
    if (static_cast<size_t>(kCompressedChunkHeaderSize) >
        static_cast<size_t>(s.dest_end - s.dest)) {
        return kCantCompress;
    }
    uint8_t* hdr = s.dest - kChunkHeaderSize;  // reuse the 6-byte header slot, extend to 12
    s.dest += kCompressedChunkHeaderSize - kChunkHeaderSize;
    s.written += kCompressedChunkHeaderSize - kChunkHeaderSize;

    std::fill(s.bitmaps.begin(), s.bitmaps.end(), Bitmap{});
    std::fill(s.periods.begin(), s.periods.end(), uint16_t{0});
    s.bmp_cnt = 0;
    std::fill(s.usecnts.begin(), s.usecnts.end(), uint64_t{0});
    std::fill(std::begin(s.top_bitmaps), std::end(s.top_bitmaps), Bitmap{});

    if (s.preread == 0) {
        const int err = buildInitialBitmap(s);
        if (err) {
            return err;
        }
    }
    s.period = 8;
    while (s.read < s.decmp_len) {
        const int err = compressEightBytes(s);
        if (err) {
            return err;
        }
    }
    return buildMetadata(s, hdr);
}

[[nodiscard]] inline int compressSingleChunk(EncodeState& s) {
    s.read = 0;
    s.written = 0;
    s.decmp_len = static_cast<uint32_t>(s.src_left < kMaxDecmpChunkSize ? s.src_left
                                                                        : kMaxDecmpChunkSize);
    s.src_end = s.src + s.decmp_len;

    const size_t max_chunk = s.decmp_len + kChunkHeaderSize;
    if (max_chunk > s.dest_left) {
        return kCantCompress;
    }
    uint8_t* decmp_hdr = s.dest;
    s.dest_end = s.dest + max_chunk;
    s.dest += kChunkHeaderSize;
    s.written += kChunkHeaderSize;

    int err = buildCompressedChunk(s);
    if (err == kCantCompress) {
        buildStoredChunk(s);
        err = 0;
    }
    if (err) {
        return err;
    }
    writeU24(decmp_hdr, 0, s.written);
    writeU24(decmp_hdr, 3, s.decmp_len);
    return 0;
}

// Compress @src into @out as a whole LZBITMAP stream (magic + chunks + terminator).
[[nodiscard]] inline bool zbmCompress(const uint8_t* src,
                                      size_t srcSize,
                                      std::vector<uint8_t>& out) {
    const size_t chunkCount = (srcSize + kMaxDecmpChunkSize - 1) / kMaxDecmpChunkSize;
    const size_t maxLen = kMagicSize + (chunkCount + 1) * (kChunkHeaderSize + kMaxDecmpChunkSize) +
                          kChunkHeaderSize;
    out.assign(maxLen, 0);

    auto st = std::make_unique<EncodeState>();
    EncodeState& s = *st;
    s.src = src;
    s.src_base = src;
    s.src_limit = src + srcSize;
    s.src_left = srcSize;
    s.dest = out.data();
    s.dest_left = out.size();

    if (!writeMagic(s)) {
        return false;
    }
    do {
        if (compressSingleChunk(s) != 0) {
            return false;
        }
        s.src_left -= s.read;
        s.dest_left -= s.written;
        s.preread += s.decmp_len;
        s.prewritten += s.written;
    } while (s.read != 0);

    out.resize(s.prewritten);
    return true;
}

}  // namespace sak::zbm

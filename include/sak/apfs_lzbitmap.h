// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file apfs_lzbitmap.h
/// @brief APFS LZBITMAP (com.apple.decmpfs algorithm 14) resource-fork container.
///
/// Unlike the ZLIB/LZVN/LZFSE resource algorithms (which share the Apple "cmpf"
/// rsrc_hdr/rsrc_data block table, see apfs_resource_fork.h), LZBITMAP stores its
/// resource fork as a bare little-endian `__le32 block_offs[]` table followed by
/// the per-block compressed data. This is the exact layout apfsck's
/// `apfs_compress_open`/`apfs_compress_read_block` parse for
/// APFS_COMPRESS_LZBITMAP_RSRC:
///
///   [0x00] __le32 block_offs[block_num + 1]   // byte offset of each block;
///                                             // block_offs[block_num] == d_size
///   [block_offs[0]] block 0 .. block N-1      // each an LZBITMAP stream or stored
///
/// The file is split into 64 KiB (APFS_COMPRESS_BLOCK) logical blocks; block_num
/// is DIV_ROUND_UP(size, 64 KiB) and apfs_compress_check enforces it exactly. Each
/// on-disk block is either an LZBITMAP whole-buffer stream (first byte 0x5A, the
/// "ZBM\x09" magic; decoded by apfs_lzbitmap_codec.h) or a stored block whose
/// leading byte has low nibble 0x0F followed by the raw bytes. Every block but the
/// last decodes to exactly 64 KiB.

#ifndef SAK_APFS_LZBITMAP_H
#define SAK_APFS_LZBITMAP_H

#include "sak/apfs_compression.h"
#include "sak/apfs_lzbitmap_codec.h"

#include <QByteArray>
#include <QtEndian>
#include <QVector>

#include <algorithm>
#include <optional>
#include <vector>

namespace sak {

// LZBITMAP decmpfs logical block size (APFS_COMPRESS_BLOCK, 64 KiB): the file is
// split into blocks of this size, each an independent LZBITMAP stream (or stored).
inline constexpr int kApfsLzbitmapBlockSize = 0x10000;

// Largest compressed block apfsck accepts (APFS_COMPRESS_BLOCK + 1): a stored
// 64 KiB block is a 1-byte marker plus 64 KiB of data.
inline constexpr int kApfsLzbitmapMaxBlockBytes = kApfsLzbitmapBlockSize + 1;

// Number of 64 KiB blocks that cover @size bytes (DIV_ROUND_UP); apfsck requires
// the resource fork to hold exactly this many blocks.
[[nodiscard]] inline int apfsLzbitmapBlockCount(uint64_t size) {
    return static_cast<int>((size + kApfsLzbitmapBlockSize - 1) / kApfsLzbitmapBlockSize);
}

// Compress one <= 64 KiB logical block. Returns the on-disk block bytes: an
// LZBITMAP stream (first byte 0x5A) when it is smaller than storing, otherwise a
// 0xFF-prefixed stored block. The result is always <= kApfsLzbitmapMaxBlockBytes,
// which keeps it within the apfsck per-block size bound.
[[nodiscard]] inline QByteArray apfsLzbitmapEncodeBlock(const QByteArray& block) {
    std::vector<uint8_t> compressed;
    const bool ok = zbm::zbmCompress(reinterpret_cast<const uint8_t*>(block.constData()),
                                     static_cast<size_t>(block.size()),
                                     compressed);
    if (ok && static_cast<int>(compressed.size()) < block.size() + 1) {
        return QByteArray(reinterpret_cast<const char*>(compressed.data()),
                          static_cast<qsizetype>(compressed.size()));
    }
    QByteArray stored;
    stored.reserve(block.size() + 1);
    stored.append(static_cast<char>(kApfsDecmpfsStoredMarker));
    stored.append(block);
    return stored;
}

// Decode one on-disk block to exactly @expectedBytes. Handles the 0x5A LZBITMAP
// and 0x0F-nibble stored forms; nullopt on any mismatch.
[[nodiscard]] inline std::optional<QByteArray> apfsLzbitmapDecodeBlock(const QByteArray& block,
                                                                       int expectedBytes) {
    if (block.isEmpty() || expectedBytes < 0) {
        return std::nullopt;
    }
    const auto first = static_cast<uint8_t>(block.at(0));
    if (first == zbm::kMagic[0]) {
        std::vector<uint8_t> out;
        if (!zbm::zbmDecompress(reinterpret_cast<const uint8_t*>(block.constData()),
                                static_cast<size_t>(block.size()),
                                static_cast<size_t>(expectedBytes),
                                out) ||
            static_cast<int>(out.size()) != expectedBytes) {
            return std::nullopt;
        }
        return QByteArray(reinterpret_cast<const char*>(out.data()),
                          static_cast<qsizetype>(out.size()));
    }
    if ((first & 0x0F) == 0x0F) {
        if (block.size() - 1 != expectedBytes) {
            return std::nullopt;
        }
        return block.mid(1);
    }
    return std::nullopt;
}

// Build the LZBITMAP resource-fork dstream for @data: a le32 block_offs[] table of
// (blockCount + 1) entries (the last equal to the total size) followed by the
// per-block compressed bytes. Returns an empty array (fail closed) if the blob
// would exceed the 32-bit offset range the on-disk table can express.
[[nodiscard]] inline QByteArray apfsBuildLzbitmapResourceFork(const QByteArray& data) {
    const int blockCount = apfsLzbitmapBlockCount(static_cast<uint64_t>(data.size()));
    const int tableBytes = (blockCount + 1) * static_cast<int>(sizeof(uint32_t));

    QByteArray blocks;
    QVector<uint32_t> offsets;
    offsets.reserve(blockCount + 1);
    int64_t offset = tableBytes;
    for (int i = 0; i < blockCount; ++i) {
        offsets.append(static_cast<uint32_t>(offset));
        const QByteArray slice = data.mid(static_cast<qsizetype>(i) * kApfsLzbitmapBlockSize,
                                          kApfsLzbitmapBlockSize);
        const QByteArray encoded = apfsLzbitmapEncodeBlock(slice);
        blocks.append(encoded);
        offset += encoded.size();
        if (offset > 0xFF'FF'FF'FFLL) {
            return {};
        }
    }
    offsets.append(static_cast<uint32_t>(offset));  // final sentinel == total size

    QByteArray table(tableBytes, '\0');
    for (int i = 0; i <= blockCount; ++i) {
        qToLittleEndian<uint32_t>(offsets.at(i), table.data() + i * sizeof(uint32_t));
    }
    return table + blocks;
}

// Extract and decode block @i from an LZBITMAP resource fork given its parsed offset table.
[[nodiscard]] inline std::optional<QByteArray> apfsLzbitmapBlockAt(const QByteArray& blob,
                                                                   const QVector<uint32_t>& offsets,
                                                                   int i,
                                                                   uint64_t uncompressedSize) {
    const uint32_t coffs = offsets.at(i);
    const uint32_t next = offsets.at(i + 1);
    if (next <= coffs || static_cast<uint64_t>(next) > static_cast<uint64_t>(blob.size())) {
        return std::nullopt;
    }
    const int expected = static_cast<int>(
        std::min<uint64_t>(kApfsLzbitmapBlockSize,
                           uncompressedSize - static_cast<uint64_t>(i) * kApfsLzbitmapBlockSize));
    return apfsLzbitmapDecodeBlock(
        blob.mid(static_cast<qsizetype>(coffs), static_cast<qsizetype>(next - coffs)), expected);
}

// Parse an LZBITMAP resource-fork dstream back to @uncompressedSize bytes. Returns
// nullopt on any structural mismatch (so the reader fails closed on bad input).
[[nodiscard]] inline std::optional<QByteArray> apfsParseLzbitmapResourceFork(
    const QByteArray& blob, uint64_t uncompressedSize) {
    const int blockCount = apfsLzbitmapBlockCount(uncompressedSize);
    if (blockCount == 0) {
        return uncompressedSize == 0 ? std::optional<QByteArray>(QByteArray()) : std::nullopt;
    }
    const int tableBytes = (blockCount + 1) * static_cast<int>(sizeof(uint32_t));
    if (blob.size() < tableBytes) {
        return std::nullopt;
    }
    QVector<uint32_t> offsets;
    offsets.reserve(blockCount + 1);
    for (int i = 0; i <= blockCount; ++i) {
        offsets.append(qFromLittleEndian<uint32_t>(blob.constData() + i * sizeof(uint32_t)));
    }
    // The final block_offs entry is the authoritative resource-fork size (apfsck reads it from the
    // dstream's d_size). The reader may hand us a block-padded assembly, so require the sentinel to
    // fit within the blob and treat any bytes past it as padding rather than demanding equality.
    if (static_cast<uint64_t>(offsets.at(blockCount)) > static_cast<uint64_t>(blob.size())) {
        return std::nullopt;
    }
    QByteArray out;
    out.reserve(static_cast<qsizetype>(uncompressedSize));
    for (int i = 0; i < blockCount; ++i) {
        const auto decoded = apfsLzbitmapBlockAt(blob, offsets, i, uncompressedSize);
        if (!decoded.has_value()) {
            return std::nullopt;
        }
        out.append(decoded.value());
    }
    if (static_cast<uint64_t>(out.size()) != uncompressedSize) {
        return std::nullopt;
    }
    return out;
}

}  // namespace sak

#endif  // SAK_APFS_LZBITMAP_H

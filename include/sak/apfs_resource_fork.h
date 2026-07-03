// SPDX-License-Identifier: MIT
//
// APFS transparent-compression resource-fork "cmpf" blob codec (com.apple.decmpfs
// algorithm 4: ZLIB_RSRC). LZVN (8) / LZFSE (12) / LZBITMAP (14) resource forks use
// the bare block_offs le32 table instead (see apfs_lzbitmap.h) - that is what the
// macOS kernel reads for them; only ZLIB uses this cmpf layout. When a compressed
// file exceeds one inline block, the payload is stored in a com.apple.ResourceFork
// data stream holding the Apple "cmpf" resource blob:
//
//   [0x00] apfs_compress_rsrc_hdr   (BIG-endian)   { data_offs, mgmt_offs,
//                                                    data_size, mgmt_size }
//   [data_offs] apfs_compress_rsrc_data (LITTLE-endian)
//                 { u32 unknown, u32 num, { u32 offs, u32 size } block[num] }
//   [...] the num compressed chunks, each covering APFS_COMPRESS_BLOCK (64 KiB)
//         of the original file (the final chunk covers the remainder).
//
// Block i's compressed bytes live at data_offs + block[i].offs + 4, length
// block[i].size -- i.e. block[i].offs is measured from the rsrc_data `num` field
// (data_offs + 4). This is exactly the arithmetic the macOS kernel and apfsck's
// compress.c perform (apfs_compress_read_block: coffs = block.offs + 4), so a
// blob built here is byte-compatible with both. Each chunk is encoded with the
// same per-chunk primitive the certified inline path uses: a raw zlib stream
// beginning 0x78, or a 0xFF-prefixed stored block when compression does not
// shrink the chunk.

#ifndef SAK_APFS_RESOURCE_FORK_H
#define SAK_APFS_RESOURCE_FORK_H

#include "sak/apfs_compression.h"

#include <QByteArray>
#include <QtEndian>
#include <QVector>

#include <cstring>
#include <optional>

namespace sak {

// Uncompressed chunk granularity of an APFS resource-fork compressed file
// (apfs/raw.h APFS_COMPRESS_BLOCK). Every chunk but the last decodes to exactly
// this many bytes; the kernel and apfsck both enforce it.
inline constexpr int kApfsCompressBlockSize = 0x10000;

// Size of apfs_compress_rsrc_hdr: four big-endian u32 (data/mgmt offs+size).
inline constexpr int kApfsResourceForkHeaderBytes = 16;

// Byte offset of the rsrc_data structure within the resource blob; the header is
// laid out immediately before it, so data_offs always equals the header size.
inline constexpr int kApfsResourceForkDataOffset = kApfsResourceForkHeaderBytes;

// Bytes of apfs_compress_rsrc_data before block[0] (u32 unknown + u32 num).
inline constexpr int kApfsResourceForkDataPrefixBytes = 8;

// Bytes per apfs_compress_rsrc_block entry (u32 offs + u32 size).
inline constexpr int kApfsResourceForkBlockEntryBytes = 8;

// Encode one uncompressed chunk for resource algorithm @algo. Returns the raw
// per-chunk bytes exactly as they are stored in the blob (and exactly as the
// kernel/apfsck decode them). Only ZLIB_RSRC is encoded here; LZVN/LZFSE resource
// chunks are added alongside their shims. Returns an empty array for an
// unsupported algorithm so the caller fails closed.
[[nodiscard]] inline QByteArray apfsEncodeResourceChunk(const QByteArray& chunk, uint32_t algo) {
    if (algo == kApfsCompressZlibRsrc) {
        // Identical per-chunk framing to the certified inline zlib path: a zlib
        // stream (begins 0x78) or a 0xFF-prefixed stored block.
        return apfsEncodeInlineZlibPayload(chunk);
    }
    return {};
}

// Decode one stored chunk of @expectedBytes for resource algorithm @algo. Returns
// nullopt on any mismatch or unsupported algorithm.
[[nodiscard]] inline std::optional<QByteArray> apfsDecodeResourceChunk(const QByteArray& chunk,
                                                                       uint64_t expectedBytes,
                                                                       uint32_t algo) {
    if (algo == kApfsCompressZlibRsrc) {
        return apfsDecodeInlineZlibPayload(chunk, expectedBytes);
    }
    return std::nullopt;
}

// True when @algo is a resource-fork (dstream-backed) compression algorithm whose
// blob this codec can build/parse.
[[nodiscard]] inline bool apfsResourceForkAlgoSupported(uint32_t algo) {
    return algo == kApfsCompressZlibRsrc;
}

// Build the complete com.apple.ResourceFork "cmpf" blob for @data, encoding each 64 KiB chunk
// with @encodeChunk (returns the on-disk per-chunk bytes, or an empty array to fail closed). Used
// by ZLIB_RSRC. NOTE: the macOS kernel reads this cmpf layout for ZLIB but does NOT accept a
// bare cmpf blob without an Apple resource map (verified on macOS 15.7.4: reads back as an I/O
// error) - the on-disk cmpf resource fork is host-apfsck/reader-verified but not yet macOS-kernel
// certified. LZVN/LZFSE/LZBITMAP avoid this by using the block_offs container.
template <class EncodeChunkFn>
[[nodiscard]] inline QByteArray apfsBuildResourceForkBlobWith(const QByteArray& data,
                                                              EncodeChunkFn encodeChunk) {
    if (data.isEmpty()) {
        return {};
    }
    const int chunkCount =
        static_cast<int>((data.size() + kApfsCompressBlockSize - 1) / kApfsCompressBlockSize);

    // Encode every chunk first so the block table can record exact offsets/sizes.
    QVector<QByteArray> encoded;
    encoded.reserve(chunkCount);
    for (int i = 0; i < chunkCount; ++i) {
        const QByteArray chunk = data.mid(static_cast<qsizetype>(i) * kApfsCompressBlockSize,
                                          kApfsCompressBlockSize);
        const QByteArray value = encodeChunk(chunk);
        if (value.isEmpty() || value.size() > kApfsCompressBlockSize + 1) {
            return {};
        }
        encoded.append(value);
    }

    const int tableBytes = kApfsResourceForkDataPrefixBytes +
                           chunkCount * kApfsResourceForkBlockEntryBytes;
    // File offset (from the blob start) of the first chunk's compressed bytes.
    const int firstChunkOffset = kApfsResourceForkDataOffset + tableBytes;

    int totalChunkBytes = 0;
    for (const QByteArray& value : encoded) {
        totalChunkBytes += static_cast<int>(value.size());
    }
    const int dataSize = tableBytes + totalChunkBytes;

    QByteArray blob(kApfsResourceForkDataOffset + dataSize, '\0');
    // apfs_compress_rsrc_hdr (big-endian): rsrc_data begins at data_offs and spans
    // data_size bytes; there is no management/resource-map region (mgmt_size 0).
    qToBigEndian<quint32>(static_cast<quint32>(kApfsResourceForkDataOffset), blob.data());
    qToBigEndian<quint32>(static_cast<quint32>(kApfsResourceForkDataOffset + dataSize),
                          blob.data() + 4);
    qToBigEndian<quint32>(static_cast<quint32>(dataSize), blob.data() + 8);
    qToBigEndian<quint32>(0, blob.data() + 12);

    // apfs_compress_rsrc_data (little-endian): unknown, then the block count.
    char* dataArea = blob.data() + kApfsResourceForkDataOffset;
    qToLittleEndian<quint32>(0, dataArea);
    qToLittleEndian<quint32>(static_cast<quint32>(chunkCount), dataArea + 4);

    int chunkOffset = firstChunkOffset;
    for (int i = 0; i < chunkCount; ++i) {
        char* entry = dataArea + kApfsResourceForkDataPrefixBytes +
                      i * kApfsResourceForkBlockEntryBytes;
        // block[i].offs is measured from the `num` field (data_offs + 4): the
        // kernel reads block bytes at data_offs + offs + 4 == chunkOffset.
        qToLittleEndian<quint32>(
            static_cast<quint32>(chunkOffset - kApfsResourceForkDataOffset - 4), entry);
        qToLittleEndian<quint32>(static_cast<quint32>(encoded[i].size()), entry + 4);
        std::memcpy(blob.data() + chunkOffset,
                    encoded[i].constData(),
                    static_cast<size_t>(encoded[i].size()));
        chunkOffset += static_cast<int>(encoded[i].size());
    }
    return blob;
}

// Build the com.apple.ResourceFork blob for @data under a header-supported resource algorithm
// (ZLIB_RSRC). LZVN/LZFSE resource callers use apfsBuildResourceForkBlobWith directly with their
// linked codec. Returns an empty array when @algo is unsupported or @data is empty.
[[nodiscard]] inline QByteArray apfsBuildResourceForkBlob(const QByteArray& data, uint32_t algo) {
    if (!apfsResourceForkAlgoSupported(algo)) {
        return {};
    }
    return apfsBuildResourceForkBlobWith(data, [algo](const QByteArray& chunk) {
        return apfsEncodeResourceChunk(chunk, algo);
    });
}

// Validated view of a resource blob's header: the rsrc_data offset and block
// count. Returns nullopt when the fixed header/block-table geometry is malformed.
struct ApfsResourceForkLayout {
    quint32 data_offs{0};
    quint32 num{0};
};

[[nodiscard]] inline std::optional<ApfsResourceForkLayout> apfsParseResourceForkLayout(
    const QByteArray& blob) {
    if (blob.size() < kApfsResourceForkDataOffset + kApfsResourceForkDataPrefixBytes) {
        return std::nullopt;
    }
    const quint32 dataOffs = qFromBigEndian<quint32>(blob.constData());
    const quint32 dataSize = qFromBigEndian<quint32>(blob.constData() + 8);
    const qint64 dataEnd = static_cast<qint64>(dataOffs) + dataSize;
    if (dataOffs < kApfsResourceForkHeaderBytes || dataEnd > blob.size() ||
        dataSize < kApfsResourceForkDataPrefixBytes) {
        return std::nullopt;
    }
    const quint32 num = qFromLittleEndian<quint32>(blob.constData() + dataOffs + 4);
    const qint64 tableBytes = static_cast<qint64>(kApfsResourceForkDataPrefixBytes) +
                              static_cast<qint64>(num) * kApfsResourceForkBlockEntryBytes;
    if (num == 0 || tableBytes > dataSize) {
        return std::nullopt;
    }
    return ApfsResourceForkLayout{dataOffs, num};
}

// Decode all @layout.num chunks of @blob into *out with @decodeChunk (bytes, expectedBytes) ->
// optional. Returns false on any block-position or per-chunk decode mismatch. Block i's bytes sit
// at data_offs + block[i].offs + 4 (kernel/apfsck arithmetic). Codec-independent.
template <class DecodeChunkFn>
[[nodiscard]] inline bool apfsDecodeResourceForkBlocksWith(const QByteArray& blob,
                                                           const ApfsResourceForkLayout& layout,
                                                           uint64_t uncompressedSize,
                                                           DecodeChunkFn decodeChunk,
                                                           QByteArray* out) {
    const char* dataArea = blob.constData() + layout.data_offs;
    for (quint32 i = 0; i < layout.num; ++i) {
        const char* entry = dataArea + kApfsResourceForkDataPrefixBytes +
                            i * kApfsResourceForkBlockEntryBytes;
        const quint32 offs = qFromLittleEndian<quint32>(entry);
        const quint32 size = qFromLittleEndian<quint32>(entry + 4);
        const qint64 chunkStart = static_cast<qint64>(layout.data_offs) + offs + 4;
        if (chunkStart < 0 || chunkStart + size > blob.size()) {
            return false;
        }
        const uint64_t remaining = uncompressedSize - static_cast<uint64_t>(out->size());
        const uint64_t expected = remaining < static_cast<uint64_t>(kApfsCompressBlockSize)
                                      ? remaining
                                      : static_cast<uint64_t>(kApfsCompressBlockSize);
        const auto chunk = decodeChunk(
            blob.mid(static_cast<qsizetype>(chunkStart), static_cast<qsizetype>(size)), expected);
        if (!chunk.has_value()) {
            return false;
        }
        out->append(*chunk);
    }
    return true;
}

// Parse a com.apple.ResourceFork "cmpf" @blob back to @uncompressedSize bytes, decoding each chunk
// with @decodeChunk. Mirrors apfsck's apfs_compress_read_block block addressing exactly. Shared by
// ZLIB (header codec) and LZVN/LZFSE (the reader's linked codecs).
template <class DecodeChunkFn>
[[nodiscard]] inline std::optional<QByteArray> apfsParseResourceForkBlobWith(
    const QByteArray& blob, uint64_t uncompressedSize, DecodeChunkFn decodeChunk) {
    const auto layout = apfsParseResourceForkLayout(blob);
    if (!layout.has_value()) {
        return std::nullopt;
    }
    QByteArray out;
    out.reserve(static_cast<qsizetype>(uncompressedSize));
    if (!apfsDecodeResourceForkBlocksWith(blob, *layout, uncompressedSize, decodeChunk, &out) ||
        static_cast<uint64_t>(out.size()) != uncompressedSize) {
        return std::nullopt;
    }
    return out;
}

// Parse a com.apple.ResourceFork @blob under a header-supported resource algorithm (ZLIB_RSRC).
// LZVN/LZFSE resource callers use apfsParseResourceForkBlobWith with their linked codec.
[[nodiscard]] inline std::optional<QByteArray> apfsParseResourceForkBlob(
    const QByteArray& blob, uint32_t algo, uint64_t uncompressedSize) {
    if (!apfsResourceForkAlgoSupported(algo)) {
        return std::nullopt;
    }
    return apfsParseResourceForkBlobWith(blob,
                                         uncompressedSize,
                                         [algo](const QByteArray& chunk, uint64_t expected) {
                                             return apfsDecodeResourceChunk(chunk, expected, algo);
                                         });
}

}  // namespace sak

#endif  // SAK_APFS_RESOURCE_FORK_H

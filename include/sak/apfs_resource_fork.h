// SPDX-License-Identifier: MIT
//
// APFS transparent-compression resource-fork "cmpf" blob codec (com.apple.decmpfs
// algorithm 4: ZLIB_RSRC). LZVN (8) / LZFSE (12) / LZBITMAP (14) resource forks use
// the bare block_offs le32 table instead (see apfs_lzbitmap.h) - that is what the
// macOS kernel reads for them; only ZLIB uses this cmpf layout. When a compressed
// file exceeds one inline block, the payload is stored in a com.apple.ResourceFork
// data stream holding a full Apple resource fork (the kernel decompresses ZLIB_RSRC
// through the classic HFS resource-fork reader, so the whole wrapper is required):
//
//   [0x00]  apfs_compress_rsrc_hdr (BIG-endian) { data_offs=0x100, mgmt_offs,
//                                                 data_size, mgmt_size=0x32 }
//   [0x10]  0xF0 bytes of zero padding up to data_offs
//   [0x100] u32 BIG-endian resource-data-length prefix (data_size - 4)
//   [0x104] u32 LITTLE-endian num, then { u32 offs, u32 size } block[num] (LE)
//   [...]   the num compressed chunks, each covering APFS_COMPRESS_BLOCK (64 KiB)
//           of the original file (the final chunk covers the remainder)
//   [mgmt_offs] 50-byte resource map naming the single 'cmpf' resource
//
// Block i's compressed bytes live at data_offs + block[i].offs + 4, length
// block[i].size -- i.e. block[i].offs is measured from the rsrc_data `num` field
// (data_offs + 4). This is exactly the arithmetic the macOS kernel and apfsck's
// compress.c perform (apfs_compress_read_block: coffs = block.offs + 4). Each chunk
// is encoded with the same per-chunk primitive the certified inline path uses: a
// raw zlib stream beginning 0x78, or a 0xFF-prefixed stored block when compression
// does not shrink the chunk. The 0x100 data offset, BE length prefix and resource
// map are macOS-kernel certified on 15.7.4 (byte-exact read-back).

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

// Byte offset of the rsrc_data structure within the resource blob. The macOS
// kernel decompresses ZLIB_RSRC through the classic HFS resource-fork reader,
// which requires the resource data to begin at 0x100 (a 0xF0-byte zero gap
// follows the 16-byte header) and a resource map to trail it. data_offs is thus
// 0x100, NOT the header size (verified kernel-certified on macOS 15.7.4).
inline constexpr int kApfsResourceForkDataOffset = 0x100;

// Bytes of the fixed resource-map trailer that follows the compressed blocks. The
// macOS resource-fork reader needs this Apple map (one 'cmpf' resource) to accept
// the fork; its exact 50 bytes are reproduced by apfsAppendZlibResourceMap below.
inline constexpr int kApfsResourceForkMapTrailerBytes = 50;

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

// Write the fixed 50-byte Apple resource-map trailer for a ZLIB_RSRC fork at @trailer (which must
// address at least kApfsResourceForkMapTrailerBytes zeroed bytes). These exact bytes - a minimal
// resource map naming the single 'cmpf' resource - are what the macOS resource-fork reader parses
// to accept the compressed fork; the layout is reproduced from a kernel-certified reference file
// (afsctool's decmpfs_resource_zlib_trailer) and proven byte-exact on macOS 15.7.4.
inline void apfsWriteZlibResourceMap(char* trailer) {
    std::memset(trailer, 0, kApfsResourceForkMapTrailerBytes);
    qToBigEndian<quint16>(0x001Cu, trailer + 24);  // magic1: offset to type list
    qToBigEndian<quint16>(0x0032u, trailer + 26);  // magic2: resource map length
    // trailer + 28: spacer1 (u16) stays zero
    qToBigEndian<quint32>(0x63'6D'70'66u, trailer + 30);       // compression_magic 'cmpf'
    qToBigEndian<quint32>(0x00'00'00'0Au, trailer + 34);       // magic3
    qToLittleEndian<quint64>(0xFF'FF'01'00ull, trailer + 38);  // magic4 (little-endian)
    // trailer + 46: spacer2 (u32) stays zero -> total 50 bytes
}

// Build the complete com.apple.ResourceFork "cmpf" blob for @data, encoding each 64 KiB chunk
// with @encodeChunk (returns the on-disk per-chunk bytes, or an empty array to fail closed). Used
// by ZLIB_RSRC. The blob is the full Apple resource fork the macOS kernel reads: a 16-byte header,
// a 0xF0 zero gap, the resource data at 0x100 (a big-endian data-length prefix, then the
// little-endian block table and compressed chunks), and the 50-byte resource-map trailer. This
// exact layout is macOS-kernel certified on 15.7.4 (byte-exact read-back of both compressible and
// incompressible files). LZVN/LZFSE/LZBITMAP instead use the block_offs container.
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

    QByteArray blob(kApfsResourceForkDataOffset + dataSize + kApfsResourceForkMapTrailerBytes,
                    '\0');
    // apfs_compress_rsrc_hdr (big-endian resource-fork header): the resource data
    // begins at data_offs (0x100) and spans data_size bytes; the resource map
    // (mgmt_offs/mgmt_size) is the 50-byte 'cmpf' trailer immediately after it.
    qToBigEndian<quint32>(static_cast<quint32>(kApfsResourceForkDataOffset), blob.data());
    qToBigEndian<quint32>(static_cast<quint32>(kApfsResourceForkDataOffset + dataSize),
                          blob.data() + 4);
    qToBigEndian<quint32>(static_cast<quint32>(dataSize), blob.data() + 8);
    qToBigEndian<quint32>(static_cast<quint32>(kApfsResourceForkMapTrailerBytes), blob.data() + 12);

    // apfs_compress_rsrc_data: a big-endian resource-data-length prefix (data_size
    // minus its own 4 bytes) as the resource-fork reader expects, then the
    // little-endian block count.
    char* dataArea = blob.data() + kApfsResourceForkDataOffset;
    qToBigEndian<quint32>(static_cast<quint32>(dataSize - 4), dataArea);
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
    apfsWriteZlibResourceMap(blob.data() + kApfsResourceForkDataOffset + dataSize);
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

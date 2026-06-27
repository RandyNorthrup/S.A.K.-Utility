// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file apfs_compression.h
/// @brief APFS transparent compression (com.apple.decmpfs) on-disk layout and the
///        inline zlib codec shared by the APFS writer and reader.
///
/// APFS stores a compressed file's logical content in a `com.apple.decmpfs`
/// extended attribute. The attribute begins with a 16-byte header
/// (`struct apfs_compress_hdr`: signature, algorithm, uncompressed size) and,
/// for the inline algorithms, is followed by the compressed payload. The file's
/// inode carries the `UF_COMPRESSED` BSD flag so the kernel and `fsck_apfs`
/// route reads through the decmpfs path instead of the (absent) data stream.
///
/// Field offsets and constraints mirror Apple's `apfs/raw.h` and the checks in
/// `apfsck`'s `compress.c` / `xattr.c` (the validator the cert must satisfy):
///  - signature is the decmpfs magic 'cmpf' (0x636D7066, little-endian on disk),
///    which the macOS kernel requires; `apfsck` only flags a non-zero signature
///    under `-u` (unused in the certification command), so the magic is safe.
///  - inline zlib (`APFS_COMPRESS_ZLIB_ATTR` = 3): the payload is a standard zlib
///    stream beginning 0x78, or a 0xFF-prefixed stored block when compression
///    does not shrink the data (`(byte0 & 0x0F) == 0x0F`). `apfsck` validates the
///    inflate length exactly; the kernel decodes identically.
///  - the embedded attribute value may not exceed APFS_XATTR_MAX_EMBEDDED_SIZE.

#pragma once

#include <QByteArray>
#include <QtEndian>

#include <cstdint>
#include <optional>

namespace sak {

// decmpfs header (struct apfs_compress_hdr): __le32 signature, __le32 algo,
// __le64 uncompressed_size. 16 bytes, identical to HFS+ decmpfs.
inline constexpr int kApfsDecmpfsHeaderBytes = 16;
inline constexpr uint32_t kApfsDecmpfsMagic = 0x63'6D'70'66;  // 'cmpf' (LE on disk: 66 70 6D 63)
inline constexpr int kApfsDecmpfsAlgoOffset = 4;
inline constexpr int kApfsDecmpfsSizeOffset = 8;

// Compression algorithms (apfs/raw.h). Inline (*_ATTR) keep the payload in the
// decmpfs attribute; resource (*_RSRC) keep it in a com.apple.ResourceFork
// dstream. LZVN/LZFSE share HFS+'s decmpfs algorithm numbers.
inline constexpr uint32_t kApfsCompressZlibAttr = 3;
inline constexpr uint32_t kApfsCompressZlibRsrc = 4;
inline constexpr uint32_t kApfsCompressLzvnAttr = 7;
inline constexpr uint32_t kApfsCompressLzvnRsrc = 8;
inline constexpr uint32_t kApfsCompressPlainAttr = 9;
inline constexpr uint32_t kApfsCompressPlainRsrc = 10;
inline constexpr uint32_t kApfsCompressLzfseAttr = 11;
inline constexpr uint32_t kApfsCompressLzfseRsrc = 12;
inline constexpr uint32_t kApfsCompressLzbitmapRsrc = 14;

// Inline payload markers: a zlib stream starts 0x78; a stored (uncompressed)
// block starts with a byte whose low nibble is 0x0F. The writer uses 0xFF.
inline constexpr uint8_t kApfsDecmpfsStoredMarker = 0xFF;

// Extended-attribute value flags (apfs_xattr_val.flags).
inline constexpr uint16_t kApfsXattrDataStream = 0x0001;
inline constexpr uint16_t kApfsXattrDataEmbedded = 0x0002;
inline constexpr uint16_t kApfsXattrFileSystemOwned = 0x0004;
inline constexpr int kApfsXattrMaxEmbeddedSize = 3804;

// Well-known xattr names (include the trailing NUL in the on-disk name_len).
inline constexpr char kApfsXattrNameCompressed[] = "com.apple.decmpfs";
inline constexpr char kApfsXattrNameResourceFork[] = "com.apple.ResourceFork";

// Inode accounting for a compressed file: the BSD UF_COMPRESSED flag triggers
// the decmpfs read path; APFS_INODE_HAS_UNCOMPRESSED_SIZE must accompany a
// non-zero apfs_inode_val.uncompressed_size.
inline constexpr uint32_t kApfsInodeBsdCompressed = 0x00'00'00'20;
inline constexpr uint64_t kApfsInodeHasUncompressedSize = 0x00'04'00'00;

// Build the 16-byte decmpfs header for @algo over @uncompressedSize bytes.
[[nodiscard]] inline QByteArray apfsBuildDecmpfsHeader(uint32_t algo, uint64_t uncompressedSize) {
    QByteArray header(kApfsDecmpfsHeaderBytes, '\0');
    qToLittleEndian<uint32_t>(kApfsDecmpfsMagic, header.data());
    qToLittleEndian<uint32_t>(algo, header.data() + kApfsDecmpfsAlgoOffset);
    qToLittleEndian<uint64_t>(uncompressedSize, header.data() + kApfsDecmpfsSizeOffset);
    return header;
}

// Encode @data as the inline zlib decmpfs payload: a standard zlib stream
// (Qt's qCompress with its 4-byte size prefix stripped, so it begins 0x78), or
// a 0xFF-prefixed stored block when compression does not shrink the data. This
// is byte-for-byte what the macOS kernel and apfsck accept for ZLIB_ATTR.
[[nodiscard]] inline QByteArray apfsEncodeInlineZlibPayload(const QByteArray& data) {
    const QByteArray zlibStream = qCompress(data, 9).mid(static_cast<qsizetype>(sizeof(quint32)));
    if (zlibStream.isEmpty() || zlibStream.size() >= data.size() + 1) {
        QByteArray stored;
        stored.reserve(data.size() + 1);
        stored.append(static_cast<char>(kApfsDecmpfsStoredMarker));
        stored.append(data);
        return stored;
    }
    return zlibStream;
}

// Build the full embedded com.apple.decmpfs attribute value (header + payload)
// for an inline zlib-compressed file. @ok is set false when the value would
// exceed the embedded-xattr limit (the caller must then use a resource fork).
[[nodiscard]] inline QByteArray apfsBuildInlineZlibDecmpfs(const QByteArray& data, bool* ok) {
    QByteArray value = apfsBuildDecmpfsHeader(kApfsCompressZlibAttr,
                                              static_cast<uint64_t>(data.size()));
    value.append(apfsEncodeInlineZlibPayload(data));
    if (ok) {
        *ok = value.size() <= kApfsXattrMaxEmbeddedSize;
    }
    return value;
}

// Decode an inline zlib/stored decmpfs payload (the bytes after the 16-byte
// header) to exactly @expectedBytes. Returns nullopt on any mismatch.
[[nodiscard]] inline std::optional<QByteArray> apfsDecodeInlineZlibPayload(
    const QByteArray& payload, uint64_t expectedBytes) {
    if (payload.isEmpty()) {
        return std::nullopt;
    }
    if ((static_cast<uint8_t>(payload.at(0)) & 0x0F) == 0x0F) {
        const QByteArray stored = payload.mid(1);
        if (static_cast<uint64_t>(stored.size()) != expectedBytes) {
            return std::nullopt;
        }
        return stored;
    }
    QByteArray prefixed;
    prefixed.reserve(payload.size() + static_cast<qsizetype>(sizeof(quint32)));
    QByteArray sizePrefix(sizeof(quint32), '\0');
    qToBigEndian<quint32>(static_cast<quint32>(expectedBytes), sizePrefix.data());
    prefixed.append(sizePrefix);
    prefixed.append(payload);
    const QByteArray out = qUncompress(prefixed);
    if (static_cast<uint64_t>(out.size()) != expectedBytes) {
        return std::nullopt;
    }
    return out;
}

// Parsed view of a com.apple.decmpfs attribute header.
struct ApfsDecmpfsHeader {
    uint32_t signature{0};
    uint32_t algo{0};
    uint64_t uncompressed_size{0};
};

[[nodiscard]] inline std::optional<ApfsDecmpfsHeader> apfsParseDecmpfsHeader(
    const QByteArray& attribute) {
    if (attribute.size() < kApfsDecmpfsHeaderBytes) {
        return std::nullopt;
    }
    return ApfsDecmpfsHeader{.signature = qFromLittleEndian<uint32_t>(attribute.constData()),
                             .algo = qFromLittleEndian<uint32_t>(attribute.constData() +
                                                                 kApfsDecmpfsAlgoOffset),
                             .uncompressed_size = qFromLittleEndian<uint64_t>(
                                 attribute.constData() + kApfsDecmpfsSizeOffset)};
}

// Decode a whole embedded inline decmpfs attribute (header + payload). Handles
// the inline algorithms whose payload lives in the attribute itself: zlib (3)
// and plain/stored (9). Resource-fork algorithms (4/8/12/14) keep their data in
// a separate dstream and are decoded by the reader's resource path; this returns
// nullopt for them so the caller can fall back.
[[nodiscard]] inline std::optional<QByteArray> apfsDecodeInlineDecmpfs(
    const QByteArray& attribute) {
    const auto header = apfsParseDecmpfsHeader(attribute);
    if (!header.has_value()) {
        return std::nullopt;
    }
    const QByteArray payload = attribute.mid(kApfsDecmpfsHeaderBytes);
    switch (header->algo) {
    case kApfsCompressZlibAttr:
        return apfsDecodeInlineZlibPayload(payload, header->uncompressed_size);
    case kApfsCompressPlainAttr:
        // Plain inline: a single stored byte prefix then the literal bytes.
        if (payload.isEmpty() ||
            static_cast<uint64_t>(payload.size() - 1) != header->uncompressed_size) {
            return std::nullopt;
        }
        return payload.mid(1);
    default:
        return std::nullopt;
    }
}

[[nodiscard]] inline bool apfsDecmpfsAlgoIsInline(uint32_t algo) {
    return algo == kApfsCompressZlibAttr || algo == kApfsCompressLzvnAttr ||
           algo == kApfsCompressPlainAttr || algo == kApfsCompressLzfseAttr;
}

}  // namespace sak

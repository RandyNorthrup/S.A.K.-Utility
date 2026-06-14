// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_file_system_detector.cpp
/// @brief Read-only raw file-system signature detector for Partition Manager.

#include "sak/partition_file_system_detector.h"

#include "sak/partition_raw_device_io.h"

#include <QFile>
#include <QIODevice>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <vector>

namespace sak {

namespace {

constexpr qsizetype kExtSuperblockOffset = 1024;
constexpr qsizetype kExtMagicOffset = 0x38;
constexpr qsizetype kExtInodesCountOffset = 0x0;
constexpr qsizetype kExtBlocksCountLoOffset = 0x4;
constexpr qsizetype kExtFreeBlocksCountLoOffset = 0xC;
constexpr qsizetype kExtFreeInodesCountOffset = 0x10;
constexpr qsizetype kExtLogBlockSizeOffset = 0x18;
constexpr qsizetype kExtBlocksPerGroupOffset = 0x20;
constexpr qsizetype kExtInodesPerGroupOffset = 0x28;
constexpr qsizetype kExtFeatureCompatOffset = 0x5C;
constexpr qsizetype kExtFeatureIncompatOffset = 0x60;
constexpr qsizetype kExtFeatureRoCompatOffset = 0x64;
constexpr qsizetype kExtVolumeNameOffset = 0x78;
constexpr qsizetype kExtVolumeNameSize = 16;
constexpr qsizetype kExtBlocksCountHiOffset = 0x150;
constexpr qsizetype kExtFreeBlocksCountHiOffset = 0x158;
constexpr unsigned char kExtMagicFirstByte = 0x53;
constexpr unsigned char kExtMagicSecondByte = 0xEF;
constexpr qsizetype kSecondSignatureByteOffset = 1;
constexpr qsizetype kXfsMagicOffset = 0;
constexpr qsizetype kXfsMagicSize = 4;
constexpr qsizetype kXfsBlockSizeOffset = 4;
constexpr qsizetype kXfsDataBlocksOffset = 8;
constexpr qsizetype kXfsUuidOffset = 32;
constexpr qsizetype kXfsAgBlocksOffset = 84;
constexpr qsizetype kXfsAgCountOffset = 88;
constexpr qsizetype kXfsVersionOffset = 100;
constexpr qsizetype kXfsSectorSizeOffset = 102;
constexpr qsizetype kXfsInodeSizeOffset = 104;
constexpr qsizetype kXfsNameOffset = 108;
constexpr qsizetype kXfsNameSize = 12;
constexpr qsizetype kXfsInodeCountOffset = 128;
constexpr qsizetype kXfsFreeInodeCountOffset = 136;
constexpr qsizetype kXfsFreeDataBlocksOffset = 144;
constexpr qsizetype kXfsFeatures2Offset = 200;
constexpr qsizetype kApfsMagicOffset = 0x20;
constexpr qsizetype kApfsMagicSize = 4;
constexpr qsizetype kApfsBlockSizeOffset = 0x24;
constexpr qsizetype kApfsBlockCountOffset = 0x28;
constexpr qsizetype kApfsFeaturesOffset = 0x30;
constexpr qsizetype kApfsReadOnlyCompatibleFeaturesOffset = 0x38;
constexpr qsizetype kApfsIncompatibleFeaturesOffset = 0x40;
constexpr qsizetype kApfsUuidOffset = 0x48;
constexpr qsizetype kApfsNextObjectIdOffset = 0x58;
constexpr qsizetype kApfsNextTransactionIdOffset = 0x60;
constexpr qsizetype kApfsCheckpointDescriptorBlocksOffset = 0x68;
constexpr qsizetype kApfsCheckpointDataBlocksOffset = 0x6C;
constexpr qsizetype kApfsCheckpointDescriptorBaseOffset = 0x70;
constexpr qsizetype kApfsCheckpointDataBaseOffset = 0x78;
constexpr qsizetype kApfsCheckpointDescriptorNextOffset = 0x80;
constexpr qsizetype kApfsCheckpointDataNextOffset = 0x84;
constexpr qsizetype kApfsCheckpointDescriptorIndexOffset = 0x88;
constexpr qsizetype kApfsCheckpointDescriptorLengthOffset = 0x8C;
constexpr qsizetype kApfsCheckpointDataIndexOffset = 0x90;
constexpr qsizetype kApfsCheckpointDataLengthOffset = 0x94;
constexpr qsizetype kApfsSpacemanOidOffset = 0x98;
constexpr qsizetype kApfsObjectMapOidOffset = 0xA0;
constexpr qsizetype kApfsReaperOidOffset = 0xA8;
constexpr qsizetype kApfsMaxFileSystemsOffset = 0xB4;
constexpr qsizetype kApfsFileSystemOidArrayOffset = 0xB8;
constexpr qsizetype kApfsFileSystemOidCount = 100;
constexpr qsizetype kHfsVolumeHeaderOffset = 1024;
constexpr qsizetype kHfsSignatureSize = 2;
constexpr qsizetype kHfsVersionOffset = 2;
constexpr qsizetype kHfsAttributesOffset = 4;
constexpr qsizetype kHfsFileCountOffset = 32;
constexpr qsizetype kHfsFolderCountOffset = 36;
constexpr qsizetype kHfsBlockSizeOffset = 40;
constexpr qsizetype kHfsTotalBlocksOffset = 44;
constexpr qsizetype kHfsFreeBlocksOffset = 48;
constexpr qsizetype kHfsWrapperMasterDirectoryBlockOffset = 1024;
constexpr qsizetype kHfsWrapperSignatureOffset = 0;
constexpr qsizetype kHfsWrapperAllocationBlockSizeOffset = 0x14;
constexpr qsizetype kHfsWrapperAllocationBlockStartOffset = 0x1C;
constexpr qsizetype kHfsWrapperEmbeddedSignatureOffset = 0x7C;
constexpr qsizetype kHfsWrapperEmbeddedExtentStartOffset = 0x7E;
constexpr qsizetype kHfsWrapperEmbeddedExtentCountOffset = 0x80;
constexpr qsizetype kBtrfsSuperblockOffset = 64 * 1024;
constexpr qsizetype kBtrfsUuidOffset = 0x20;
constexpr qsizetype kBtrfsMagicOffset = 0x40;
constexpr qsizetype kBtrfsMagicSize = 8;
constexpr qsizetype kBtrfsGenerationOffset = 0x48;
constexpr qsizetype kBtrfsTotalBytesOffset = 0x70;
constexpr qsizetype kBtrfsBytesUsedOffset = 0x78;
constexpr qsizetype kBtrfsNumDevicesOffset = 0x88;
constexpr qsizetype kBtrfsSectorSizeOffset = 0x90;
constexpr qsizetype kBtrfsNodeSizeOffset = 0x94;
constexpr qsizetype kBtrfsLeafSizeOffset = 0x98;
constexpr qsizetype kBtrfsCompatFlagsOffset = 0xAC;
constexpr qsizetype kBtrfsCompatRoFlagsOffset = 0xB4;
constexpr qsizetype kBtrfsIncompatFlagsOffset = 0xBC;
constexpr qsizetype kBtrfsLabelOffset = 0x12B;
constexpr qsizetype kBtrfsLabelSize = 0x100;
constexpr qsizetype kSwapSignatureSize = 10;
constexpr qsizetype kSwapInfoOffset = 1024;
constexpr qsizetype kSwapVersionOffset = kSwapInfoOffset;
constexpr qsizetype kSwapLastPageOffset = kSwapInfoOffset + 4;
constexpr qsizetype kSwapBadPagesOffset = kSwapInfoOffset + 8;
constexpr qsizetype kSwapUuidOffset = kSwapInfoOffset + 12;
constexpr qsizetype kSwapLabelOffset = kSwapInfoOffset + 28;
constexpr qsizetype kSwapLabelSize = 16;
constexpr qsizetype kHfsWrapperProbeBytes = 2 * 1024 * 1024;
constexpr qsizetype kMaxProbeBytes = kHfsWrapperProbeBytes;
constexpr qsizetype kUint16Size = 2;
constexpr qsizetype kUint32Size = 4;
constexpr qsizetype kUint64Size = 8;
constexpr qsizetype kUuidSize = 16;
constexpr qsizetype kApfsObjectOidOffset = 0x08;
constexpr qsizetype kApfsObjectXidOffset = 0x10;
constexpr qsizetype kApfsObjectTypeOffset = 0x18;
constexpr qsizetype kApfsObjectSubtypeOffset = 0x1C;
constexpr qsizetype kApfsOmapFlagsOffset = 0x20;
constexpr qsizetype kApfsOmapSnapshotCountOffset = 0x24;
constexpr qsizetype kApfsOmapTreeOidOffset = 0x30;
constexpr qsizetype kApfsOmapSnapshotTreeOidOffset = 0x38;
constexpr qsizetype kApfsOmapMostRecentSnapshotOffset = 0x40;
constexpr qsizetype kApfsOmapPendingRevertMinOffset = 0x48;
constexpr qsizetype kApfsOmapPendingRevertMaxOffset = 0x50;
constexpr qsizetype kApfsOmapMinimumBytes = kApfsOmapPendingRevertMaxOffset + kUint64Size;
constexpr qsizetype kApfsSpacemanBlockSizeOffset = 0x20;
constexpr qsizetype kApfsSpacemanBlocksPerChunkOffset = 0x24;
constexpr qsizetype kApfsSpacemanChunksPerCibOffset = 0x28;
constexpr qsizetype kApfsSpacemanMainDeviceOffset = 0x30;
constexpr qsizetype kApfsSpacemanDeviceBlockCountOffset = 0x00;
constexpr qsizetype kApfsSpacemanDeviceChunkCountOffset = 0x08;
constexpr qsizetype kApfsSpacemanDeviceCibCountOffset = 0x10;
constexpr qsizetype kApfsSpacemanDeviceFreeCountOffset = 0x18;
constexpr qsizetype kApfsSpacemanMinimumBytes = kApfsSpacemanMainDeviceOffset +
                                                kApfsSpacemanDeviceFreeCountOffset + kUint64Size;
constexpr qsizetype kApfsBtreeNodeFlagsOffset = 0x20;
constexpr qsizetype kApfsBtreeNodeLevelOffset = 0x22;
constexpr qsizetype kApfsBtreeNodeKeyCountOffset = 0x24;
constexpr qsizetype kApfsBtreeNodeTableSpaceOffsetOffset = 0x28;
constexpr qsizetype kApfsBtreeNodeTableSpaceLengthOffset = 0x2A;
constexpr qsizetype kApfsBtreeNodeFreeSpaceOffsetOffset = 0x2C;
constexpr qsizetype kApfsBtreeNodeFreeSpaceLengthOffset = 0x2E;
constexpr qsizetype kApfsBtreeNodeKeyFreeListOffsetOffset = 0x30;
constexpr qsizetype kApfsBtreeNodeKeyFreeListLengthOffset = 0x32;
constexpr qsizetype kApfsBtreeNodeValueFreeListOffsetOffset = 0x34;
constexpr qsizetype kApfsBtreeNodeValueFreeListLengthOffset = 0x36;
constexpr qsizetype kApfsBtreeNodeMinimumBytes = kApfsBtreeNodeValueFreeListLengthOffset +
                                                 kUint16Size;
constexpr qsizetype kApfsBtreeInfoSize = 40;
constexpr qsizetype kApfsBtreeInfoFlagsOffset = 0x00;
constexpr qsizetype kApfsBtreeInfoNodeSizeOffset = 0x04;
constexpr qsizetype kApfsBtreeInfoKeySizeOffset = 0x08;
constexpr qsizetype kApfsBtreeInfoValueSizeOffset = 0x0C;
constexpr qsizetype kApfsBtreeInfoLongestKeyOffset = 0x10;
constexpr qsizetype kApfsBtreeInfoLongestValueOffset = 0x14;
constexpr qsizetype kApfsBtreeInfoKeyCountOffset = 0x18;
constexpr qsizetype kApfsBtreeInfoNodeCountOffset = 0x20;
constexpr qsizetype kApfsVolumeMagicOffset = kApfsMagicOffset;
constexpr qsizetype kApfsVolumeMagicSize = kApfsMagicSize;
constexpr qsizetype kApfsVolumeIndexOffset = 0x24;
constexpr qsizetype kApfsVolumeReserveBlockCountOffset = 0x48;
constexpr qsizetype kApfsVolumeQuotaBlockCountOffset = 0x50;
constexpr qsizetype kApfsVolumeAllocatedBlockCountOffset = 0x58;
constexpr qsizetype kApfsVolumeObjectMapOidOffset = 0x80;
constexpr qsizetype kApfsVolumeRootTreeOidOffset = 0x88;
constexpr qsizetype kApfsVolumeExtentrefTreeOidOffset = 0x90;
constexpr qsizetype kApfsVolumeSnapshotMetadataTreeOidOffset = 0x98;
constexpr qsizetype kApfsVolumeUuidOffset = 0xF0;
constexpr qsizetype kApfsVolumeNameOffset = 0x2C0;
constexpr qsizetype kApfsVolumeNameSize = 256;
constexpr qsizetype kApfsVolumeRoleOffset = 0x3C4;
constexpr qsizetype kApfsVolumeRoleSize = 2;
constexpr qsizetype kApfsVolumeMinimumBytes = kApfsVolumeRoleOffset + kApfsVolumeRoleSize;
constexpr int kMaxApfsVolumeCandidates = 16;
constexpr int kMaxApfsReferencedObjectHeaders = 16;
constexpr int kMaxApfsObjectMapDetails = 16;
constexpr int kMaxApfsBtreeNodeDetails = 16;
constexpr qsizetype kUuidHexFirstGroupOffset = 0;
constexpr qsizetype kUuidHexFirstGroupSize = 8;
constexpr qsizetype kUuidHexSecondGroupOffset = 8;
constexpr qsizetype kUuidHexSecondGroupSize = 4;
constexpr qsizetype kUuidHexThirdGroupOffset = 12;
constexpr qsizetype kUuidHexThirdGroupSize = 4;
constexpr qsizetype kUuidHexFourthGroupOffset = 16;
constexpr qsizetype kUuidHexFourthGroupSize = 4;
constexpr qsizetype kUuidHexFifthGroupOffset = 20;
constexpr qsizetype kUuidHexFifthGroupSize = 12;
constexpr int kHex16FieldWidth = 4;
constexpr int kHex32FieldWidth = 8;
constexpr int kHex64FieldWidth = 16;
constexpr int kHexBase = 16;
constexpr int kBitsPerUint32 = 32;
constexpr qsizetype kBootSectorSignatureOffset = 510;
constexpr qsizetype kBootSectorSignatureSize = 2;
constexpr qsizetype kBootSectorOemOffset = 3;
constexpr qsizetype kNtfsOemSize = 8;
constexpr qsizetype kFat16TypeOffset = 54;
constexpr qsizetype kFat32TypeOffset = 82;
constexpr qsizetype kFatTypeSize = 8;
constexpr unsigned char kBootSectorSignatureFirstByte = 0x55;
constexpr unsigned char kBootSectorSignatureSecondByte = 0xAA;
constexpr uint32_t kExtCompatHasJournal = 0x0004;
constexpr uint32_t kExtIncompatExtents = 0x0040;
constexpr uint32_t kExtIncompat64Bit = 0x0080;
constexpr uint32_t kExtRoCompatHugeFile = 0x0008;
constexpr uint32_t kExtRoCompatGdtCsum = 0x0010;
constexpr uint32_t kExtRoCompatDirNlink = 0x0020;
constexpr uint32_t kExtRoCompatExtraIsize = 0x0040;
constexpr uint32_t kExtRoCompatMetadataCsum = 0x0400;
constexpr uint32_t kHfsVolumeJournaledMask = 0x00'00'20'00;
constexpr uint32_t kApfsObjectTypeMask = 0x00'00'FF'FF;
constexpr uint32_t kApfsObjectTypeBtreeRoot = 0x00'00'00'02;
constexpr uint32_t kApfsObjectTypeBtreeNode = 0x00'00'00'03;
constexpr uint32_t kApfsObjectTypeSpaceman = 0x00'00'00'05;
constexpr uint32_t kApfsObjectTypeObjectMap = 0x00'00'00'0B;
constexpr uint16_t kApfsBtreeNodeRootFlag = 0x0001;
constexpr uint16_t kApfsBtreeNodeLeafFlag = 0x0002;
constexpr uint16_t kApfsBtreeNodeFixedKvFlag = 0x0004;
constexpr uint16_t kApfsBtreeNodeHashedFlag = 0x0008;
constexpr uint16_t kApfsBtreeNodeNoHeaderFlag = 0x0010;
constexpr uint16_t kApfsBtreeNodeCheckKeyOffsetFlag = 0x8000;
constexpr uint32_t kMinimumFileSystemBlockSize = 512;
constexpr uint32_t kMinimumExtBlockSize = 1024;
constexpr uint32_t kMaximumExtBlockSize = 1024 * 1024;
constexpr uint32_t kMinimumApfsBlockSize = 4096;
constexpr uint32_t kMaximumApfsBlockSize = 65'536;
constexpr uint32_t kApfsSupplementalCheckpointScanBlocks = 512;
constexpr uint32_t kMinimumBtrfsTreeBlockSize = 4096;
constexpr uint32_t kMaximumXfsBlockSize = 65'536;
constexpr uint32_t kMinimumXfsInodeSize = 256;
constexpr uint64_t kHfsWrapperSectorBytes = 512;
constexpr uint64_t kMinimumDeviceCount = 1;
constexpr std::array<qsizetype, 4> kSwapPageSizes{4096, 8192, 16'384, 65'536};

struct HfsWrapperInfo {
    uint64_t embedded_offset_bytes{0};
    uint16_t extent_start_block{0};
    uint16_t extent_block_count{0};
    uint32_t allocation_block_size{0};
    uint16_t allocation_block_start_sector{0};
};

struct BtrfsSuperblockValues {
    uint64_t total_bytes{0};
    uint64_t bytes_used{0};
    uint64_t devices{0};
    uint32_t sector_size{0};
    uint32_t node_size{0};
    uint32_t leaf_size{0};
};

struct SwapSignatureInfo {
    qsizetype page_size{0};
    QString signature;
};

struct ApfsCheckpointValues {
    uint32_t descriptor_blocks{0};
    uint32_t data_blocks{0};
    uint64_t descriptor_base{0};
    uint64_t data_base{0};
    uint32_t descriptor_next{0};
    uint32_t data_next{0};
    uint32_t descriptor_index{0};
    uint32_t descriptor_length{0};
    uint32_t data_index{0};
    uint32_t data_length{0};
};

struct ApfsObjectReference {
    QString label;
    uint64_t oid{0};
};

void setProbeError(QString* errorMessage, const QString& message);

bool hasBytes(const QByteArray& bytes, qsizetype offset, qsizetype length) {
    return offset >= 0 && length >= 0 && offset <= bytes.size() && bytes.size() - offset >= length;
}

bool matchesBytes(const QByteArray& bytes,
                  qsizetype offset,
                  const char* expected,
                  qsizetype length) {
    return hasBytes(bytes, offset, length) &&
           std::equal(expected, expected + length, bytes.constData() + offset);
}

bool hasBootSectorSignature(const QByteArray& bytes) {
    return hasBytes(bytes, kBootSectorSignatureOffset, kBootSectorSignatureSize) &&
           static_cast<unsigned char>(bytes.at(kBootSectorSignatureOffset)) ==
               kBootSectorSignatureFirstByte &&
           static_cast<unsigned char>(
               bytes.at(kBootSectorSignatureOffset + kSecondSignatureByteOffset)) ==
               kBootSectorSignatureSecondByte;
}

uint32_t littleEndian32(const QByteArray& bytes, qsizetype offset) {
    if (!hasBytes(bytes, offset, kUint32Size)) {
        return 0;
    }
    return qFromLittleEndian<uint32_t>(bytes.constData() + offset);
}

uint64_t littleEndian64(const QByteArray& bytes, qsizetype offset) {
    if (!hasBytes(bytes, offset, kUint64Size)) {
        return 0;
    }
    return qFromLittleEndian<uint64_t>(bytes.constData() + offset);
}

uint16_t littleEndian16(const QByteArray& bytes, qsizetype offset) {
    if (!hasBytes(bytes, offset, kUint16Size)) {
        return 0;
    }
    return qFromLittleEndian<uint16_t>(bytes.constData() + offset);
}

uint16_t bigEndian16(const QByteArray& bytes, qsizetype offset) {
    if (!hasBytes(bytes, offset, kUint16Size)) {
        return 0;
    }
    return qFromBigEndian<uint16_t>(bytes.constData() + offset);
}

uint32_t bigEndian32(const QByteArray& bytes, qsizetype offset) {
    if (!hasBytes(bytes, offset, kUint32Size)) {
        return 0;
    }
    return qFromBigEndian<uint32_t>(bytes.constData() + offset);
}

uint64_t bigEndian64(const QByteArray& bytes, qsizetype offset) {
    if (!hasBytes(bytes, offset, kUint64Size)) {
        return 0;
    }
    return qFromBigEndian<uint64_t>(bytes.constData() + offset);
}

bool isPowerOfTwo(uint32_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

std::optional<uint64_t> checkedProduct(uint64_t left, uint64_t right) {
    if (left == 0 || right == 0) {
        return 0;
    }
    if (left > std::numeric_limits<uint64_t>::max() / right) {
        return std::nullopt;
    }
    return left * right;
}

QString fixedAsciiField(const QByteArray& bytes, qsizetype offset, qsizetype length) {
    if (!hasBytes(bytes, offset, length)) {
        return {};
    }
    QByteArray field = bytes.mid(offset, length);
    const qsizetype terminator = field.indexOf('\0');
    if (terminator >= 0) {
        field.truncate(terminator);
    }
    return QString::fromLatin1(field).trimmed();
}

QString fixedUtf8Field(const QByteArray& bytes, qsizetype offset, qsizetype length) {
    if (!hasBytes(bytes, offset, length)) {
        return {};
    }
    QByteArray field = bytes.mid(offset, length);
    const qsizetype terminator = field.indexOf('\0');
    if (terminator >= 0) {
        field.truncate(terminator);
    }
    return QString::fromUtf8(field).trimmed();
}

QString hex32(uint32_t value) {
    return QStringLiteral("0x%1").arg(
        static_cast<qulonglong>(value), kHex32FieldWidth, kHexBase, QLatin1Char('0'));
}

QString hex16(uint16_t value) {
    return QStringLiteral("0x%1").arg(
        static_cast<qulonglong>(value), kHex16FieldWidth, kHexBase, QLatin1Char('0'));
}

QString hex64(uint64_t value) {
    return QStringLiteral("0x%1").arg(
        static_cast<qulonglong>(value), kHex64FieldWidth, kHexBase, QLatin1Char('0'));
}

QString uuidField(const QByteArray& bytes, qsizetype offset) {
    if (!hasBytes(bytes, offset, kUuidSize)) {
        return {};
    }
    const QByteArray hex = bytes.mid(offset, kUuidSize).toHex();
    return QStringLiteral("%1-%2-%3-%4-%5")
        .arg(QString::fromLatin1(hex.mid(kUuidHexFirstGroupOffset, kUuidHexFirstGroupSize)),
             QString::fromLatin1(hex.mid(kUuidHexSecondGroupOffset, kUuidHexSecondGroupSize)),
             QString::fromLatin1(hex.mid(kUuidHexThirdGroupOffset, kUuidHexThirdGroupSize)),
             QString::fromLatin1(hex.mid(kUuidHexFourthGroupOffset, kUuidHexFourthGroupSize)),
             QString::fromLatin1(hex.mid(kUuidHexFifthGroupOffset, kUuidHexFifthGroupSize)));
}

bool isHfsPlusSignatureAt(const QByteArray& bytes, qsizetype offset) {
    return matchesBytes(bytes, offset, "H+", kHfsSignatureSize) ||
           matchesBytes(bytes, offset, "HX", kHfsSignatureSize);
}

QString hfsFamilyNameAt(const QByteArray& bytes, qsizetype offset) {
    if (matchesBytes(bytes, offset, "H+", kHfsSignatureSize)) {
        return QStringLiteral("HFS+");
    }
    if (matchesBytes(bytes, offset, "HX", kHfsSignatureSize)) {
        return QStringLiteral("HFSX");
    }
    return {};
}

std::optional<uint64_t> hfsWrapperEmbeddedOffset(uint16_t allocationStartSector,
                                                 uint16_t extentStartBlock,
                                                 uint32_t allocationBlockSize) {
    if (allocationBlockSize < kMinimumFileSystemBlockSize || !isPowerOfTwo(allocationBlockSize)) {
        return std::nullopt;
    }

    const uint64_t allocationStartBytes = static_cast<uint64_t>(allocationStartSector) *
                                          kHfsWrapperSectorBytes;
    const uint64_t extentStartBytes = static_cast<uint64_t>(extentStartBlock) * allocationBlockSize;
    if (allocationStartBytes > std::numeric_limits<uint64_t>::max() - extentStartBytes) {
        return std::nullopt;
    }
    return allocationStartBytes + extentStartBytes;
}

std::optional<HfsWrapperInfo> hfsWrapperInfo(const QByteArray& bytes) {
    const qsizetype mdb = kHfsWrapperMasterDirectoryBlockOffset;
    if (!matchesBytes(bytes, mdb + kHfsWrapperSignatureOffset, "BD", kHfsSignatureSize) ||
        !isHfsPlusSignatureAt(bytes, mdb + kHfsWrapperEmbeddedSignatureOffset)) {
        return std::nullopt;
    }

    const uint32_t allocationBlockSize = bigEndian32(bytes,
                                                     mdb + kHfsWrapperAllocationBlockSizeOffset);
    const uint16_t allocationStartSector = bigEndian16(bytes,
                                                       mdb + kHfsWrapperAllocationBlockStartOffset);
    const uint16_t extentStartBlock = bigEndian16(bytes,
                                                  mdb + kHfsWrapperEmbeddedExtentStartOffset);
    const uint16_t extentBlockCount = bigEndian16(bytes,
                                                  mdb + kHfsWrapperEmbeddedExtentCountOffset);
    if (extentBlockCount == 0) {
        return std::nullopt;
    }

    const auto embeddedOffset =
        hfsWrapperEmbeddedOffset(allocationStartSector, extentStartBlock, allocationBlockSize);
    if (!embeddedOffset.has_value() ||
        *embeddedOffset > static_cast<uint64_t>(std::numeric_limits<qsizetype>::max())) {
        return std::nullopt;
    }
    return HfsWrapperInfo{*embeddedOffset,
                          extentStartBlock,
                          extentBlockCount,
                          allocationBlockSize,
                          allocationStartSector};
}

bool hasExtMagic(const QByteArray& bytes) {
    const qsizetype magic = kExtSuperblockOffset + kExtMagicOffset;
    return hasBytes(bytes, magic, kHfsSignatureSize) &&
           static_cast<unsigned char>(bytes.at(magic)) == kExtMagicFirstByte &&
           static_cast<unsigned char>(bytes.at(magic + kSecondSignatureByteOffset)) ==
               kExtMagicSecondByte;
}

QString extFamilyName(uint32_t compat, uint32_t incompat, uint32_t roCompat) {
    const bool ext4Feature =
        (incompat & (kExtIncompatExtents | kExtIncompat64Bit)) != 0 ||
        (roCompat & (kExtRoCompatHugeFile | kExtRoCompatGdtCsum | kExtRoCompatDirNlink |
                     kExtRoCompatExtraIsize | kExtRoCompatMetadataCsum)) != 0;
    if (ext4Feature) {
        return QStringLiteral("ext4");
    }
    if ((compat & kExtCompatHasJournal) != 0) {
        return QStringLiteral("ext3");
    }
    return QStringLiteral("ext2");
}

uint64_t ext64BitCount(uint32_t low, uint32_t high, uint32_t incompat) {
    if ((incompat & kExtIncompat64Bit) == 0) {
        return low;
    }
    return (static_cast<uint64_t>(high) << kBitsPerUint32) | low;
}

void appendExtBlockDetails(PartitionFileSystemDetection* detection,
                           uint32_t blockSize,
                           uint64_t totalBlocks,
                           uint64_t freeBlocks) {
    if (blockSize < kMinimumExtBlockSize || blockSize > kMaximumExtBlockSize ||
        !isPowerOfTwo(blockSize) || totalBlocks == 0 || freeBlocks > totalBlocks) {
        return;
    }

    if (const auto totalBytes = checkedProduct(blockSize, totalBlocks); totalBytes.has_value()) {
        detection->total_bytes = *totalBytes;
    }
    if (const auto freeBytes = checkedProduct(blockSize, freeBlocks); freeBytes.has_value()) {
        detection->free_bytes = *freeBytes;
    }
    detection->details.append(QStringLiteral("Block size: %1").arg(blockSize));
    detection->details.append(QStringLiteral("Total blocks: %1").arg(totalBlocks));
    detection->details.append(QStringLiteral("Free blocks: %1").arg(freeBlocks));
}

void appendExtSuperblockDetails(PartitionFileSystemDetection* detection,
                                const QByteArray& bytes,
                                uint32_t compat,
                                uint32_t incompat,
                                uint32_t roCompat) {
    const qsizetype superblock = kExtSuperblockOffset;
    const uint32_t inodes = littleEndian32(bytes, superblock + kExtInodesCountOffset);
    const uint32_t freeInodes = littleEndian32(bytes, superblock + kExtFreeInodesCountOffset);
    const uint32_t logBlockSize = littleEndian32(bytes, superblock + kExtLogBlockSizeOffset);
    const uint32_t blockSize = logBlockSize < 32 ? 1024U << logBlockSize : 0;
    const uint64_t totalBlocks =
        ext64BitCount(littleEndian32(bytes, superblock + kExtBlocksCountLoOffset),
                      littleEndian32(bytes, superblock + kExtBlocksCountHiOffset),
                      incompat);
    const uint64_t freeBlocks =
        ext64BitCount(littleEndian32(bytes, superblock + kExtFreeBlocksCountLoOffset),
                      littleEndian32(bytes, superblock + kExtFreeBlocksCountHiOffset),
                      incompat);

    appendExtBlockDetails(detection, blockSize, totalBlocks, freeBlocks);
    detection->details.append(QStringLiteral("Inodes: %1").arg(inodes));
    detection->details.append(QStringLiteral("Free inodes: %1").arg(freeInodes));
    detection->details.append(
        QStringLiteral("Blocks per group: %1")
            .arg(littleEndian32(bytes, superblock + kExtBlocksPerGroupOffset)));
    detection->details.append(
        QStringLiteral("Inodes per group: %1")
            .arg(littleEndian32(bytes, superblock + kExtInodesPerGroupOffset)));
    detection->details.append(QStringLiteral("Journaled: %1")
                                  .arg((compat & kExtCompatHasJournal) != 0
                                           ? QStringLiteral("Yes")
                                           : QStringLiteral("No")));
    detection->details.append(QStringLiteral("Feature compat: %1").arg(hex32(compat)));
    detection->details.append(QStringLiteral("Feature incompat: %1").arg(hex32(incompat)));
    detection->details.append(QStringLiteral("Feature ro compat: %1").arg(hex32(roCompat)));
    const QString label =
        fixedAsciiField(bytes, superblock + kExtVolumeNameOffset, kExtVolumeNameSize);
    if (!label.isEmpty()) {
        detection->details.append(QStringLiteral("Volume label: %1").arg(label));
    }
}

std::optional<PartitionFileSystemDetection> detectExtFamily(const QByteArray& bytes) {
    if (!hasExtMagic(bytes)) {
        return std::nullopt;
    }

    const qsizetype superblock = kExtSuperblockOffset;
    const uint32_t compat = littleEndian32(bytes, superblock + kExtFeatureCompatOffset);
    const uint32_t incompat = littleEndian32(bytes, superblock + kExtFeatureIncompatOffset);
    const uint32_t roCompat = littleEndian32(bytes, superblock + kExtFeatureRoCompatOffset);
    PartitionFileSystemDetection detection{extFamilyName(compat, incompat, roCompat),
                                           PartitionFileSystemDetector::rawSignatureSource()};
    appendExtSuperblockDetails(&detection, bytes, compat, incompat, roCompat);
    return detection;
}

QString detectWindowsBootSectorFamily(const QByteArray& bytes) {
    if (!hasBootSectorSignature(bytes)) {
        return {};
    }
    if (matchesBytes(bytes, kBootSectorOemOffset, "NTFS    ", kNtfsOemSize)) {
        return QStringLiteral("NTFS");
    }
    if (matchesBytes(bytes, kBootSectorOemOffset, "EXFAT   ", kNtfsOemSize)) {
        return QStringLiteral("exFAT");
    }
    if (matchesBytes(bytes, kFat32TypeOffset, "FAT32   ", kFatTypeSize)) {
        return QStringLiteral("FAT32");
    }
    if (matchesBytes(bytes, kFat16TypeOffset, "FAT16   ", kFatTypeSize)) {
        return QStringLiteral("FAT16");
    }
    if (matchesBytes(bytes, kFat16TypeOffset, "FAT12   ", kFatTypeSize)) {
        return QStringLiteral("FAT12");
    }
    return {};
}

std::optional<PartitionFileSystemDetection> detectHfsHeaderAt(
    const QByteArray& bytes, qsizetype headerOffset, const std::optional<HfsWrapperInfo>& wrapper) {
    const QString familyName = hfsFamilyNameAt(bytes, headerOffset);
    if (familyName.isEmpty()) {
        return std::nullopt;
    }

    PartitionFileSystemDetection detection{familyName,
                                           PartitionFileSystemDetector::rawSignatureSource()};
    const uint16_t version = bigEndian16(bytes, headerOffset + kHfsVersionOffset);
    const uint32_t attributes = bigEndian32(bytes, headerOffset + kHfsAttributesOffset);
    const uint32_t fileCount = bigEndian32(bytes, headerOffset + kHfsFileCountOffset);
    const uint32_t folderCount = bigEndian32(bytes, headerOffset + kHfsFolderCountOffset);
    const uint32_t blockSize = bigEndian32(bytes, headerOffset + kHfsBlockSizeOffset);
    const uint32_t totalBlocks = bigEndian32(bytes, headerOffset + kHfsTotalBlocksOffset);
    const uint32_t freeBlocks = bigEndian32(bytes, headerOffset + kHfsFreeBlocksOffset);

    if (wrapper.has_value()) {
        detection.details.append(QStringLiteral("HFS wrapper: Yes"));
        detection.details.append(
            QStringLiteral("Embedded offset: %1").arg(wrapper->embedded_offset_bytes));
        detection.details.append(QStringLiteral("Wrapper allocation block size: %1")
                                     .arg(wrapper->allocation_block_size));
        detection.details.append(QStringLiteral("Wrapper allocation start sector: %1")
                                     .arg(wrapper->allocation_block_start_sector));
        detection.details.append(QStringLiteral("Embedded extent: %1 blocks at block %2")
                                     .arg(wrapper->extent_block_count)
                                     .arg(wrapper->extent_start_block));
    }
    detection.details.append(QStringLiteral("Version: %1").arg(version));
    detection.details.append(QStringLiteral("Files: %1").arg(fileCount));
    detection.details.append(QStringLiteral("Folders: %1").arg(folderCount));
    detection.details.append(QStringLiteral("Journaled: %1")
                                 .arg((attributes & kHfsVolumeJournaledMask) != 0
                                          ? QStringLiteral("Yes")
                                          : QStringLiteral("No")));
    if (blockSize >= kMinimumFileSystemBlockSize && isPowerOfTwo(blockSize) && totalBlocks > 0 &&
        freeBlocks <= totalBlocks) {
        detection.total_bytes = static_cast<uint64_t>(blockSize) * totalBlocks;
        detection.free_bytes = static_cast<uint64_t>(blockSize) * freeBlocks;
        detection.details.append(QStringLiteral("Block size: %1").arg(blockSize));
        detection.details.append(QStringLiteral("Total blocks: %1").arg(totalBlocks));
        detection.details.append(QStringLiteral("Free blocks: %1").arg(freeBlocks));
    }
    return detection;
}

std::optional<PartitionFileSystemDetection> detectHfsPlusFamily(const QByteArray& bytes) {
    if (const auto directHeader = detectHfsHeaderAt(bytes, kHfsVolumeHeaderOffset, std::nullopt);
        directHeader.has_value()) {
        return directHeader;
    }

    const auto wrapper = hfsWrapperInfo(bytes);
    if (!wrapper.has_value()) {
        return std::nullopt;
    }
    const uint64_t wrappedHeaderOffset = wrapper->embedded_offset_bytes + kHfsVolumeHeaderOffset;
    if (wrappedHeaderOffset > static_cast<uint64_t>(std::numeric_limits<qsizetype>::max())) {
        return std::nullopt;
    }
    return detectHfsHeaderAt(bytes, static_cast<qsizetype>(wrappedHeaderOffset), wrapper);
}

std::optional<SwapSignatureInfo> swapSignatureInfo(const QByteArray& bytes,
                                                   uint64_t partitionSizeBytes) {
    for (const qsizetype pageSize : kSwapPageSizes) {
        if (partitionSizeBytes > 0 && static_cast<uint64_t>(pageSize) > partitionSizeBytes) {
            continue;
        }
        const qsizetype offset = pageSize - kSwapSignatureSize;
        if (matchesBytes(bytes, offset, "SWAPSPACE2", kSwapSignatureSize)) {
            return SwapSignatureInfo{pageSize, QStringLiteral("SWAPSPACE2")};
        }
        if (matchesBytes(bytes, offset, "SWAP-SPACE", kSwapSignatureSize)) {
            return SwapSignatureInfo{pageSize, QStringLiteral("SWAP-SPACE")};
        }
    }
    return std::nullopt;
}

void appendDetailIfText(PartitionFileSystemDetection* detection,
                        const QString& label,
                        const QString& value) {
    if (!value.isEmpty()) {
        detection->details.append(QStringLiteral("%1: %2").arg(label, value));
    }
}

void appendDetailIfNonZero(PartitionFileSystemDetection* detection,
                           const QString& label,
                           uint64_t value) {
    if (value > 0) {
        detection->details.append(QStringLiteral("%1: %2").arg(label).arg(value));
    }
}

void appendSummaryPartIfText(QStringList* parts, const QString& label, const QString& value) {
    if (!value.isEmpty()) {
        parts->append(QStringLiteral("%1 %2").arg(label, value));
    }
}

void appendSummaryPartIfNonZero(QStringList* parts, const QString& label, uint64_t value) {
    if (value > 0) {
        parts->append(QStringLiteral("%1 %2").arg(label).arg(value));
    }
}

void appendMetadataSanity(PartitionFileSystemDetection* detection,
                          const QString& okMessage,
                          const QStringList& warnings) {
    if (warnings.isEmpty()) {
        detection->details.append(QStringLiteral("Metadata sanity: %1").arg(okMessage));
        return;
    }
    for (const auto& warning : warnings) {
        detection->details.append(QStringLiteral("Metadata sanity warning: %1").arg(warning));
    }
}

void appendXfsBlockDetails(PartitionFileSystemDetection* detection,
                           uint32_t blockSize,
                           uint64_t dataBlocks,
                           uint64_t freeDataBlocks) {
    if (blockSize < kMinimumFileSystemBlockSize || blockSize > kMaximumXfsBlockSize ||
        !isPowerOfTwo(blockSize) || dataBlocks == 0 || freeDataBlocks > dataBlocks) {
        return;
    }
    if (const auto totalBytes = checkedProduct(blockSize, dataBlocks); totalBytes.has_value()) {
        detection->total_bytes = *totalBytes;
    }
    if (const auto freeBytes = checkedProduct(blockSize, freeDataBlocks); freeBytes.has_value()) {
        detection->free_bytes = *freeBytes;
    }
    detection->details.append(QStringLiteral("Block size: %1").arg(blockSize));
    detection->details.append(QStringLiteral("Data blocks: %1").arg(dataBlocks));
    detection->details.append(QStringLiteral("Free data blocks: %1").arg(freeDataBlocks));
}

QStringList xfsMetadataWarnings(uint32_t blockSize,
                                uint64_t dataBlocks,
                                uint64_t freeDataBlocks,
                                uint16_t sectorSize,
                                uint16_t inodeSize) {
    QStringList warnings;
    if (blockSize < kMinimumFileSystemBlockSize || blockSize > kMaximumXfsBlockSize ||
        !isPowerOfTwo(blockSize)) {
        warnings.append(QStringLiteral("XFS block size is outside supported sane bounds"));
    }
    if (dataBlocks == 0) {
        warnings.append(QStringLiteral("XFS data block count is zero"));
    }
    if (freeDataBlocks > dataBlocks) {
        warnings.append(QStringLiteral("XFS free data blocks exceed total data blocks"));
    }
    if (sectorSize < kMinimumFileSystemBlockSize || !isPowerOfTwo(sectorSize)) {
        warnings.append(QStringLiteral("XFS sector size is outside supported sane bounds"));
    }
    if (inodeSize < kMinimumXfsInodeSize || !isPowerOfTwo(inodeSize)) {
        warnings.append(QStringLiteral("XFS inode size is outside supported sane bounds"));
    }
    return warnings;
}

void appendXfsIdentityDetails(PartitionFileSystemDetection* detection, const QByteArray& bytes) {
    appendDetailIfText(detection, QStringLiteral("UUID"), uuidField(bytes, kXfsUuidOffset));
    appendDetailIfText(detection,
                       QStringLiteral("File-system name"),
                       fixedAsciiField(bytes, kXfsNameOffset, kXfsNameSize));
}

void appendXfsGeometryDetails(PartitionFileSystemDetection* detection, const QByteArray& bytes) {
    appendDetailIfNonZero(detection,
                          QStringLiteral("AG blocks"),
                          bigEndian32(bytes, kXfsAgBlocksOffset));
    appendDetailIfNonZero(detection,
                          QStringLiteral("Allocation groups"),
                          bigEndian32(bytes, kXfsAgCountOffset));
    appendDetailIfNonZero(detection,
                          QStringLiteral("Sector size"),
                          bigEndian16(bytes, kXfsSectorSizeOffset));
    appendDetailIfNonZero(detection,
                          QStringLiteral("Inode size"),
                          bigEndian16(bytes, kXfsInodeSizeOffset));
    const uint64_t inodeCount = bigEndian64(bytes, kXfsInodeCountOffset);
    appendDetailIfNonZero(detection, QStringLiteral("Inodes"), inodeCount);
    if (inodeCount > 0) {
        detection->details.append(
            QStringLiteral("Free inodes: %1").arg(bigEndian64(bytes, kXfsFreeInodeCountOffset)));
    }
}

std::optional<PartitionFileSystemDetection> detectXfsFamily(const QByteArray& bytes) {
    if (!matchesBytes(bytes, kXfsMagicOffset, "XFSB", kXfsMagicSize)) {
        return std::nullopt;
    }

    PartitionFileSystemDetection detection{QStringLiteral("XFS"),
                                           PartitionFileSystemDetector::rawSignatureSource()};
    const uint32_t blockSize = bigEndian32(bytes, kXfsBlockSizeOffset);
    const uint64_t dataBlocks = bigEndian64(bytes, kXfsDataBlocksOffset);
    const uint64_t freeDataBlocks = bigEndian64(bytes, kXfsFreeDataBlocksOffset);
    const uint16_t sectorSize = bigEndian16(bytes, kXfsSectorSizeOffset);
    const uint16_t inodeSize = bigEndian16(bytes, kXfsInodeSizeOffset);
    appendXfsBlockDetails(&detection, blockSize, dataBlocks, freeDataBlocks);
    appendXfsIdentityDetails(&detection, bytes);
    appendXfsGeometryDetails(&detection, bytes);
    detection.details.append(
        QStringLiteral("Version flags: %1").arg(hex32(bigEndian16(bytes, kXfsVersionOffset))));
    detection.details.append(
        QStringLiteral("Features2: %1").arg(hex32(bigEndian32(bytes, kXfsFeatures2Offset))));
    appendMetadataSanity(
        &detection,
        QStringLiteral("XFS superblock geometry is internally consistent"),
        xfsMetadataWarnings(blockSize, dataBlocks, freeDataBlocks, sectorSize, inodeSize));
    return detection;
}

QStringList btrfsMetadataWarnings(const BtrfsSuperblockValues& values) {
    QStringList warnings;
    if (values.total_bytes == 0) {
        warnings.append(QStringLiteral("Btrfs total bytes is zero"));
    }
    if (values.bytes_used > values.total_bytes) {
        warnings.append(QStringLiteral("Btrfs used bytes exceed total bytes"));
    }
    if (values.devices < kMinimumDeviceCount) {
        warnings.append(QStringLiteral("Btrfs device count is zero"));
    }
    if (values.sector_size < kMinimumFileSystemBlockSize || !isPowerOfTwo(values.sector_size)) {
        warnings.append(QStringLiteral("Btrfs sector size is outside supported sane bounds"));
    }
    if (values.node_size < kMinimumBtrfsTreeBlockSize || !isPowerOfTwo(values.node_size)) {
        warnings.append(QStringLiteral("Btrfs node size is outside supported sane bounds"));
    }
    if (values.leaf_size < kMinimumBtrfsTreeBlockSize || !isPowerOfTwo(values.leaf_size)) {
        warnings.append(QStringLiteral("Btrfs leaf size is outside supported sane bounds"));
    }
    return warnings;
}

std::optional<PartitionFileSystemDetection> detectBtrfsFamily(const QByteArray& bytes) {
    if (!matchesBytes(
            bytes, kBtrfsSuperblockOffset + kBtrfsMagicOffset, "_BHRfS_M", kBtrfsMagicSize)) {
        return std::nullopt;
    }

    PartitionFileSystemDetection detection{QStringLiteral("Btrfs"),
                                           PartitionFileSystemDetector::rawSignatureSource()};
    const qsizetype superblock = kBtrfsSuperblockOffset;
    const BtrfsSuperblockValues values{
        .total_bytes = littleEndian64(bytes, superblock + kBtrfsTotalBytesOffset),
        .bytes_used = littleEndian64(bytes, superblock + kBtrfsBytesUsedOffset),
        .devices = littleEndian64(bytes, superblock + kBtrfsNumDevicesOffset),
        .sector_size = littleEndian32(bytes, superblock + kBtrfsSectorSizeOffset),
        .node_size = littleEndian32(bytes, superblock + kBtrfsNodeSizeOffset),
        .leaf_size = littleEndian32(bytes, superblock + kBtrfsLeafSizeOffset)};
    if (values.total_bytes > 0 && values.bytes_used <= values.total_bytes) {
        detection.total_bytes = values.total_bytes;
        detection.free_bytes = values.total_bytes - values.bytes_used;
        detection.details.append(QStringLiteral("Total bytes: %1").arg(values.total_bytes));
        detection.details.append(QStringLiteral("Bytes used: %1").arg(values.bytes_used));
    }

    const QString uuid = uuidField(bytes, superblock + kBtrfsUuidOffset);
    if (!uuid.isEmpty()) {
        detection.details.append(QStringLiteral("UUID: %1").arg(uuid));
    }
    const QString label = fixedUtf8Field(bytes, superblock + kBtrfsLabelOffset, kBtrfsLabelSize);
    if (!label.isEmpty()) {
        detection.details.append(QStringLiteral("Label: %1").arg(label));
    }
    detection.details.append(QStringLiteral("Generation: %1")
                                 .arg(littleEndian64(bytes, superblock + kBtrfsGenerationOffset)));
    detection.details.append(QStringLiteral("Devices: %1").arg(values.devices));
    detection.details.append(QStringLiteral("Sector size: %1").arg(values.sector_size));
    detection.details.append(QStringLiteral("Node size: %1").arg(values.node_size));
    detection.details.append(QStringLiteral("Leaf size: %1").arg(values.leaf_size));
    detection.details.append(
        QStringLiteral("Compat flags: %1")
            .arg(hex64(littleEndian64(bytes, superblock + kBtrfsCompatFlagsOffset))));
    detection.details.append(
        QStringLiteral("Compat ro flags: %1")
            .arg(hex64(littleEndian64(bytes, superblock + kBtrfsCompatRoFlagsOffset))));
    detection.details.append(
        QStringLiteral("Incompat flags: %1")
            .arg(hex64(littleEndian64(bytes, superblock + kBtrfsIncompatFlagsOffset))));
    appendMetadataSanity(&detection,
                         QStringLiteral("Btrfs superblock counters are internally consistent"),
                         btrfsMetadataWarnings(values));
    return detection;
}

void appendApfsSizeDetails(PartitionFileSystemDetection* detection,
                           uint32_t blockSize,
                           uint64_t blockCount) {
    if (blockSize < kMinimumApfsBlockSize || blockSize > kMaximumApfsBlockSize ||
        !isPowerOfTwo(blockSize) || blockCount == 0) {
        return;
    }
    if (const auto totalBytes = checkedProduct(blockSize, blockCount); totalBytes.has_value()) {
        detection->total_bytes = *totalBytes;
    }
    detection->details.append(QStringLiteral("Block size: %1").arg(blockSize));
    detection->details.append(QStringLiteral("Block count: %1").arg(blockCount));
}

ApfsCheckpointValues apfsCheckpointValues(const QByteArray& bytes) {
    return ApfsCheckpointValues{
        .descriptor_blocks = littleEndian32(bytes, kApfsCheckpointDescriptorBlocksOffset),
        .data_blocks = littleEndian32(bytes, kApfsCheckpointDataBlocksOffset),
        .descriptor_base = littleEndian64(bytes, kApfsCheckpointDescriptorBaseOffset),
        .data_base = littleEndian64(bytes, kApfsCheckpointDataBaseOffset),
        .descriptor_next = littleEndian32(bytes, kApfsCheckpointDescriptorNextOffset),
        .data_next = littleEndian32(bytes, kApfsCheckpointDataNextOffset),
        .descriptor_index = littleEndian32(bytes, kApfsCheckpointDescriptorIndexOffset),
        .descriptor_length = littleEndian32(bytes, kApfsCheckpointDescriptorLengthOffset),
        .data_index = littleEndian32(bytes, kApfsCheckpointDataIndexOffset),
        .data_length = littleEndian32(bytes, kApfsCheckpointDataLengthOffset)};
}

bool apfsCheckpointRangeExceedsContainer(uint64_t baseBlock,
                                         uint32_t lengthBlocks,
                                         uint64_t blockCount) {
    if (baseBlock == 0 || lengthBlocks == 0 || blockCount == 0) {
        return false;
    }
    return baseBlock >= blockCount || lengthBlocks > blockCount - baseBlock;
}

bool apfsCheckpointLengthExceedsArea(uint32_t areaBlocks, uint32_t lengthBlocks) {
    return areaBlocks > 0 && lengthBlocks > areaBlocks;
}

bool apfsCheckpointIndexOutsideArea(uint32_t areaBlocks, uint32_t index) {
    return areaBlocks > 0 && index >= areaBlocks;
}

void appendWarningIf(QStringList* warnings, bool condition, const QString& warning) {
    if (condition) {
        warnings->append(warning);
    }
}

void appendApfsCheckpointWarnings(QStringList* warnings,
                                  const ApfsCheckpointValues& checkpoint,
                                  uint64_t blockCount) {
    appendWarningIf(
        warnings,
        apfsCheckpointLengthExceedsArea(checkpoint.descriptor_blocks, checkpoint.descriptor_length),
        QStringLiteral("APFS checkpoint descriptor length exceeds descriptor block count"));
    appendWarningIf(warnings,
                    apfsCheckpointLengthExceedsArea(checkpoint.data_blocks, checkpoint.data_length),
                    QStringLiteral("APFS checkpoint data length exceeds data block count"));
    appendWarningIf(
        warnings,
        apfsCheckpointIndexOutsideArea(checkpoint.descriptor_blocks, checkpoint.descriptor_index),
        QStringLiteral("APFS checkpoint descriptor start index is outside descriptor area"));
    appendWarningIf(warnings,
                    apfsCheckpointIndexOutsideArea(checkpoint.data_blocks, checkpoint.data_index),
                    QStringLiteral("APFS checkpoint data start index is outside data area"));
    appendWarningIf(
        warnings,
        apfsCheckpointIndexOutsideArea(checkpoint.descriptor_blocks, checkpoint.descriptor_next),
        QStringLiteral("APFS checkpoint descriptor next index is outside descriptor area"));
    appendWarningIf(warnings,
                    apfsCheckpointIndexOutsideArea(checkpoint.data_blocks, checkpoint.data_next),
                    QStringLiteral("APFS checkpoint data next index is outside data area"));
    appendWarningIf(warnings,
                    apfsCheckpointRangeExceedsContainer(
                        checkpoint.descriptor_base, checkpoint.descriptor_length, blockCount),
                    QStringLiteral(
                        "APFS checkpoint descriptor range exceeds container block count"));
    appendWarningIf(warnings,
                    apfsCheckpointRangeExceedsContainer(
                        checkpoint.data_base, checkpoint.data_length, blockCount),
                    QStringLiteral("APFS checkpoint data range exceeds container block count"));
}

QStringList apfsMetadataWarnings(uint32_t blockSize,
                                 uint64_t blockCount,
                                 const ApfsCheckpointValues& checkpoint) {
    QStringList warnings;
    if (blockSize < kMinimumApfsBlockSize || blockSize > kMaximumApfsBlockSize ||
        !isPowerOfTwo(blockSize)) {
        warnings.append(QStringLiteral("APFS block size is outside supported sane bounds"));
    }
    if (blockCount == 0) {
        warnings.append(QStringLiteral("APFS block count is zero"));
    }
    appendApfsCheckpointWarnings(&warnings, checkpoint, blockCount);
    return warnings;
}

QStringList apfsVolumeOids(const QByteArray& bytes, uint32_t maxFileSystems) {
    QStringList oids;
    const uint32_t boundedCount = std::min<uint32_t>(maxFileSystems, kApfsFileSystemOidCount);
    for (uint32_t index = 0; index < boundedCount; ++index) {
        const qsizetype offset = kApfsFileSystemOidArrayOffset +
                                 static_cast<qsizetype>(index) * kUint64Size;
        const uint64_t oid = littleEndian64(bytes, offset);
        if (oid != 0) {
            oids.append(QStringLiteral("%1:%2").arg(index).arg(oid));
        }
    }
    return oids;
}

void appendApfsObjectReference(std::vector<ApfsObjectReference>* references,
                               const QString& label,
                               uint64_t oid) {
    if (oid > 0) {
        references->push_back(ApfsObjectReference{label, oid});
    }
}

void appendApfsContainerObjectReferences(std::vector<ApfsObjectReference>* references,
                                         const QByteArray& bytes,
                                         uint32_t maxFileSystems) {
    appendApfsObjectReference(references,
                              QStringLiteral("container space manager OID"),
                              littleEndian64(bytes, kApfsSpacemanOidOffset));
    appendApfsObjectReference(references,
                              QStringLiteral("container object map OID"),
                              littleEndian64(bytes, kApfsObjectMapOidOffset));
    appendApfsObjectReference(references,
                              QStringLiteral("container reaper OID"),
                              littleEndian64(bytes, kApfsReaperOidOffset));

    const uint32_t boundedCount = std::min<uint32_t>(maxFileSystems, kApfsFileSystemOidCount);
    for (uint32_t index = 0; index < boundedCount; ++index) {
        const qsizetype offset = kApfsFileSystemOidArrayOffset +
                                 static_cast<qsizetype>(index) * kUint64Size;
        appendApfsObjectReference(references,
                                  QStringLiteral("volume OID slot %1").arg(index),
                                  littleEndian64(bytes, offset));
    }
}

void appendApfsVolumeCandidateObjectReferences(std::vector<ApfsObjectReference>* references,
                                               const QByteArray& bytes,
                                               qsizetype blockOffset,
                                               uint32_t blockSize) {
    const QString block = QString::number(static_cast<qulonglong>(blockOffset / blockSize));
    appendApfsObjectReference(references,
                              QStringLiteral("volume candidate block %1 object map OID").arg(block),
                              littleEndian64(bytes, blockOffset + kApfsVolumeObjectMapOidOffset));
    appendApfsObjectReference(references,
                              QStringLiteral("volume candidate block %1 root tree OID").arg(block),
                              littleEndian64(bytes, blockOffset + kApfsVolumeRootTreeOidOffset));
    appendApfsObjectReference(
        references,
        QStringLiteral("volume candidate block %1 extentref tree OID").arg(block),
        littleEndian64(bytes, blockOffset + kApfsVolumeExtentrefTreeOidOffset));
    appendApfsObjectReference(
        references,
        QStringLiteral("volume candidate block %1 snapshot metadata tree OID").arg(block),
        littleEndian64(bytes, blockOffset + kApfsVolumeSnapshotMetadataTreeOidOffset));
}

std::vector<ApfsObjectReference> apfsObjectReferences(const QByteArray& bytes,
                                                      uint32_t blockSize,
                                                      uint32_t maxFileSystems) {
    std::vector<ApfsObjectReference> references;
    appendApfsContainerObjectReferences(&references, bytes, maxFileSystems);
    if (blockSize < kMinimumApfsBlockSize || blockSize > kMaximumApfsBlockSize ||
        !isPowerOfTwo(blockSize) || blockSize > static_cast<uint32_t>(bytes.size())) {
        return references;
    }

    const qsizetype step = static_cast<qsizetype>(blockSize);
    for (qsizetype blockOffset = step; blockOffset + kApfsVolumeMinimumBytes <= bytes.size();
         blockOffset += step) {
        if (matchesBytes(
                bytes, blockOffset + kApfsVolumeMagicOffset, "APSB", kApfsVolumeMagicSize)) {
            appendApfsVolumeCandidateObjectReferences(&references, bytes, blockOffset, blockSize);
        }
    }
    return references;
}

QStringList apfsLabelsForObjectOid(const std::vector<ApfsObjectReference>& references,
                                   uint64_t oid) {
    QStringList labels;
    for (const auto& reference : references) {
        if (reference.oid == oid && !labels.contains(reference.label)) {
            labels.append(reference.label);
        }
    }
    return labels;
}

QString apfsReferencedObjectHeaderSummary(const QByteArray& bytes,
                                          qsizetype blockOffset,
                                          uint32_t blockSize,
                                          const QStringList& labels,
                                          uint64_t oid) {
    QStringList parts;
    parts.append(QStringLiteral("APFS referenced object header block %1")
                     .arg(static_cast<qulonglong>(blockOffset / blockSize)));
    parts.append(QStringLiteral("labels %1").arg(labels.join(QStringLiteral(" / "))));
    parts.append(QStringLiteral("OID %1").arg(oid));
    parts.append(
        QStringLiteral("XID %1").arg(littleEndian64(bytes, blockOffset + kApfsObjectXidOffset)));
    parts.append(QStringLiteral("object type %1")
                     .arg(hex32(littleEndian32(bytes, blockOffset + kApfsObjectTypeOffset))));
    parts.append(QStringLiteral("object subtype %1")
                     .arg(hex32(littleEndian32(bytes, blockOffset + kApfsObjectSubtypeOffset))));
    return parts.join(QStringLiteral(", "));
}

bool apfsObjectMapLabels(const QStringList& labels) {
    return std::any_of(labels.cbegin(), labels.cend(), [](const QString& label) {
        return label.contains(QStringLiteral("object map OID"), Qt::CaseInsensitive);
    });
}

bool apfsProbeBlockScanSupported(const QByteArray& bytes, uint32_t blockSize) {
    return blockSize >= kMinimumApfsBlockSize && blockSize <= kMaximumApfsBlockSize &&
           isPowerOfTwo(blockSize) && blockSize <= static_cast<uint32_t>(bytes.size());
}

QString apfsObjectMapDetailSummary(const QByteArray& bytes,
                                   qsizetype blockOffset,
                                   uint32_t blockSize,
                                   const QStringList& labels) {
    if (!hasBytes(bytes, blockOffset, kApfsOmapMinimumBytes)) {
        return {};
    }

    QStringList parts;
    parts.append(QStringLiteral("APFS object map detail block %1")
                     .arg(static_cast<qulonglong>(blockOffset / blockSize)));
    parts.append(QStringLiteral("labels %1").arg(labels.join(QStringLiteral(" / "))));
    parts.append(QStringLiteral("flags %1")
                     .arg(hex32(littleEndian32(bytes, blockOffset + kApfsOmapFlagsOffset))));
    parts.append(QStringLiteral("snapshots %1")
                     .arg(littleEndian32(bytes, blockOffset + kApfsOmapSnapshotCountOffset)));
    appendSummaryPartIfNonZero(&parts,
                               QStringLiteral("tree OID"),
                               littleEndian64(bytes, blockOffset + kApfsOmapTreeOidOffset));
    appendSummaryPartIfNonZero(&parts,
                               QStringLiteral("snapshot tree OID"),
                               littleEndian64(bytes, blockOffset + kApfsOmapSnapshotTreeOidOffset));
    appendSummaryPartIfNonZero(&parts,
                               QStringLiteral("most recent snapshot XID"),
                               littleEndian64(bytes,
                                              blockOffset + kApfsOmapMostRecentSnapshotOffset));
    appendSummaryPartIfNonZero(&parts,
                               QStringLiteral("pending revert min XID"),
                               littleEndian64(bytes,
                                              blockOffset + kApfsOmapPendingRevertMinOffset));
    appendSummaryPartIfNonZero(&parts,
                               QStringLiteral("pending revert max XID"),
                               littleEndian64(bytes,
                                              blockOffset + kApfsOmapPendingRevertMaxOffset));
    return parts.join(QStringLiteral(", "));
}

bool apfsBlockHasObjectType(const QByteArray& bytes, qsizetype blockOffset, uint32_t objectType) {
    if (!hasBytes(bytes, blockOffset, kApfsObjectSubtypeOffset + kUint32Size)) {
        return false;
    }
    return (littleEndian32(bytes, blockOffset + kApfsObjectTypeOffset) & kApfsObjectTypeMask) ==
           objectType;
}

struct ApfsSpaceManagerCandidate {
    qsizetype blockOffset{0};
    uint64_t oid{0};
    uint32_t blockSize{0};
    uint32_t blocksPerChunk{0};
    uint32_t chunksPerCib{0};
    uint64_t mainBlockCount{0};
    uint64_t chunkCount{0};
    uint32_t cibCount{0};
    uint64_t freeBlocks{0};
    std::optional<uint64_t> freeBytes;
};

struct ApfsSpaceManagerContext {
    uint32_t containerBlockSize{0};
    uint64_t containerBlockCount{0};
};

QStringList apfsSpaceManagerWarnings(const ApfsSpaceManagerContext& context,
                                     const ApfsSpaceManagerCandidate& candidate) {
    QStringList warnings;
    appendWarningIf(&warnings,
                    candidate.blockSize != 0 && candidate.blockSize != context.containerBlockSize,
                    QStringLiteral("APFS space manager block size differs from container"));
    appendWarningIf(&warnings,
                    candidate.mainBlockCount == 0,
                    QStringLiteral("APFS space manager main block count is zero"));
    appendWarningIf(&warnings,
                    candidate.mainBlockCount > context.containerBlockCount,
                    QStringLiteral("APFS space manager main block count exceeds container"));
    appendWarningIf(&warnings,
                    candidate.blocksPerChunk == 0,
                    QStringLiteral("APFS space manager blocks-per-chunk is zero"));
    appendWarningIf(&warnings,
                    candidate.chunkCount == 0,
                    QStringLiteral("APFS space manager chunk count is zero"));
    if (candidate.blocksPerChunk != 0 && candidate.chunkCount != 0) {
        const auto coveredBlocks = checkedProduct(candidate.blocksPerChunk, candidate.chunkCount);
        appendWarningIf(&warnings,
                        !coveredBlocks.has_value() || candidate.mainBlockCount > *coveredBlocks,
                        QStringLiteral(
                            "APFS space manager chunk count does not cover main blocks"));
    }
    if (candidate.chunksPerCib != 0 && candidate.cibCount != 0) {
        const auto coveredChunks = checkedProduct(candidate.chunksPerCib, candidate.cibCount);
        appendWarningIf(&warnings,
                        !coveredChunks.has_value() || candidate.chunkCount > *coveredChunks,
                        QStringLiteral("APFS space manager CIB count does not cover chunks"));
    }
    appendWarningIf(&warnings,
                    candidate.freeBlocks > candidate.mainBlockCount,
                    QStringLiteral("APFS space manager free blocks exceed main blocks"));
    appendWarningIf(&warnings,
                    !candidate.freeBytes.has_value(),
                    QStringLiteral("APFS space manager free byte count overflows"));
    return warnings;
}

void appendApfsSpaceManagerSuccess(PartitionFileSystemDetection* detection,
                                   const ApfsSpaceManagerContext& context,
                                   const ApfsSpaceManagerCandidate& candidate) {
    detection->free_bytes = *candidate.freeBytes;
    detection->details.append(
        QStringLiteral("APFS space manager block: %1")
            .arg(static_cast<qulonglong>(candidate.blockOffset / context.containerBlockSize)));
    detection->details.append(QStringLiteral("APFS space manager OID: %1").arg(candidate.oid));
    detection->details.append(
        QStringLiteral("APFS space manager block size: %1")
            .arg(candidate.blockSize == 0 ? context.containerBlockSize : candidate.blockSize));
    detection->details.append(
        QStringLiteral("APFS space manager main blocks: %1").arg(candidate.mainBlockCount));
    detection->details.append(
        QStringLiteral("APFS space manager chunks: %1").arg(candidate.chunkCount));
    detection->details.append(
        QStringLiteral("APFS space manager CIBs: %1").arg(candidate.cibCount));
    detection->details.append(
        QStringLiteral("APFS space manager free blocks: %1").arg(candidate.freeBlocks));
    detection->details.append(QStringLiteral("APFS free bytes: %1").arg(*candidate.freeBytes));
}

std::optional<ApfsSpaceManagerCandidate> apfsSpaceManagerCandidateAt(const QByteArray& bytes,
                                                                     qsizetype blockOffset,
                                                                     uint64_t expectedOid,
                                                                     uint32_t blockSize) {
    if (!apfsBlockHasObjectType(bytes, blockOffset, kApfsObjectTypeSpaceman)) {
        return std::nullopt;
    }

    ApfsSpaceManagerCandidate candidate;
    candidate.blockOffset = blockOffset;
    candidate.oid = littleEndian64(bytes, blockOffset + kApfsObjectOidOffset);
    if (expectedOid != 0 && candidate.oid != expectedOid) {
        return std::nullopt;
    }

    candidate.blockSize = littleEndian32(bytes, blockOffset + kApfsSpacemanBlockSizeOffset);
    candidate.blocksPerChunk = littleEndian32(bytes,
                                              blockOffset + kApfsSpacemanBlocksPerChunkOffset);
    candidate.chunksPerCib = littleEndian32(bytes, blockOffset + kApfsSpacemanChunksPerCibOffset);
    candidate.mainBlockCount = littleEndian64(
        bytes, blockOffset + kApfsSpacemanMainDeviceOffset + kApfsSpacemanDeviceBlockCountOffset);
    candidate.chunkCount = littleEndian64(
        bytes, blockOffset + kApfsSpacemanMainDeviceOffset + kApfsSpacemanDeviceChunkCountOffset);
    candidate.cibCount = littleEndian32(
        bytes, blockOffset + kApfsSpacemanMainDeviceOffset + kApfsSpacemanDeviceCibCountOffset);
    candidate.freeBlocks = littleEndian64(
        bytes, blockOffset + kApfsSpacemanMainDeviceOffset + kApfsSpacemanDeviceFreeCountOffset);
    candidate.freeBytes = checkedProduct(blockSize, candidate.freeBlocks);
    return candidate;
}

std::optional<ApfsSpaceManagerCandidate> findApfsSpaceManagerCandidate(const QByteArray& bytes,
                                                                       uint32_t blockSize) {
    const uint64_t expectedOid = littleEndian64(bytes, kApfsSpacemanOidOffset);
    const qsizetype step = static_cast<qsizetype>(blockSize);
    for (qsizetype blockOffset = 0; blockOffset + kApfsSpacemanMinimumBytes <= bytes.size();
         blockOffset += step) {
        const auto candidate =
            apfsSpaceManagerCandidateAt(bytes, blockOffset, expectedOid, blockSize);
        if (candidate.has_value()) {
            return candidate;
        }
    }
    return std::nullopt;
}

bool appendApfsSpaceManagerDetails(PartitionFileSystemDetection* detection,
                                   const QByteArray& bytes,
                                   uint32_t blockSize,
                                   uint64_t blockCount) {
    if (!detection || !apfsProbeBlockScanSupported(bytes, blockSize) || blockCount == 0) {
        return false;
    }

    const auto candidate = findApfsSpaceManagerCandidate(bytes, blockSize);
    if (!candidate.has_value()) {
        return false;
    }

    const ApfsSpaceManagerContext context{blockSize, blockCount};
    const QStringList warnings = apfsSpaceManagerWarnings(context, *candidate);
    if (!warnings.isEmpty()) {
        detection->details.append(
            QStringLiteral("APFS space manager block %1 ignored")
                .arg(static_cast<qulonglong>(candidate->blockOffset / context.containerBlockSize)));
        appendMetadataSanity(detection,
                             QStringLiteral("APFS space manager counters are usable"),
                             warnings);
        return false;
    }

    appendApfsSpaceManagerSuccess(detection, context, *candidate);
    return true;
}

bool hasApfsSpaceManagerDetails(const PartitionFileSystemDetection& detection) {
    for (const auto& detail : detection.details) {
        if (detail.startsWith(QStringLiteral("APFS space manager block:"))) {
            return true;
        }
    }
    return false;
}

std::optional<uint64_t> checkedSum(uint64_t left, uint64_t right) {
    if (left > std::numeric_limits<uint64_t>::max() - right) {
        return std::nullopt;
    }
    return left + right;
}

std::optional<uint64_t> apfsBlockByteOffset(uint64_t partitionOffsetBytes,
                                            uint64_t blockNumber,
                                            uint32_t blockSize) {
    const auto relativeOffset = checkedProduct(blockNumber, blockSize);
    if (!relativeOffset.has_value()) {
        return std::nullopt;
    }
    return checkedSum(partitionOffsetBytes, *relativeOffset);
}

bool apfsBlockInsidePartition(uint64_t blockNumber,
                              uint32_t blockSize,
                              uint64_t partitionSizeBytes) {
    if (partitionSizeBytes == 0) {
        return true;
    }
    if (partitionSizeBytes < blockSize) {
        return false;
    }
    const auto blockOffset = checkedProduct(blockNumber, blockSize);
    if (!blockOffset.has_value()) {
        return false;
    }
    return *blockOffset <= partitionSizeBytes - blockSize;
}

std::optional<QByteArray> readExactDeviceBytes(QIODevice* device,
                                               uint64_t absoluteOffset,
                                               qsizetype byteCount,
                                               QString* errorMessage) {
    if (!device || !device->isOpen()) {
        setProbeError(errorMessage, QStringLiteral("Raw probe device is not open"));
        return std::nullopt;
    }
    if (byteCount < 0) {
        setProbeError(errorMessage, QStringLiteral("Raw probe byte count is invalid"));
        return std::nullopt;
    }
    if (absoluteOffset > static_cast<uint64_t>(std::numeric_limits<qint64>::max())) {
        setProbeError(errorMessage, QStringLiteral("Raw probe seek offset is too large"));
        return std::nullopt;
    }
    if (!device->seek(static_cast<qint64>(absoluteOffset))) {
        setProbeError(errorMessage,
                      QStringLiteral("Raw probe seek failed: %1").arg(device->errorString()));
        return std::nullopt;
    }

    QByteArray bytes = device->read(byteCount);
    if (bytes.size() == byteCount) {
        return bytes;
    }
    if (bytes.isEmpty() && device->errorString().isEmpty()) {
        setProbeError(errorMessage, QStringLiteral("Raw probe read returned no data"));
    } else {
        setProbeError(errorMessage,
                      QStringLiteral("Raw probe read returned %1 of %2 bytes: %3")
                          .arg(bytes.size())
                          .arg(byteCount)
                          .arg(device->errorString()));
    }
    return std::nullopt;
}

struct ApfsSupplementalReadContext {
    QIODevice* device{nullptr};
    uint64_t partitionOffsetBytes{0};
    uint64_t partitionSizeBytes{0};
    uint32_t blockSize{0};
    uint64_t blockCount{0};
    uint64_t expectedOid{0};
    ApfsCheckpointValues checkpoint;
};

struct ApfsSupplementalInput {
    const QByteArray* probeBytes{nullptr};
    QIODevice* device{nullptr};
    uint64_t partitionOffsetBytes{0};
    uint64_t partitionSizeBytes{0};
};

std::optional<ApfsSpaceManagerCandidate> apfsSpaceManagerCandidateAtDeviceBlock(
    const ApfsSupplementalReadContext& context, uint64_t blockNumber, QString* errorMessage) {
    const auto absoluteOffset =
        apfsBlockByteOffset(context.partitionOffsetBytes, blockNumber, context.blockSize);
    if (!absoluteOffset.has_value()) {
        setProbeError(errorMessage, QStringLiteral("APFS checkpoint block offset overflow"));
        return std::nullopt;
    }

    const auto blockBytes = readExactDeviceBytes(
        context.device, *absoluteOffset, static_cast<qsizetype>(context.blockSize), errorMessage);
    if (!blockBytes.has_value()) {
        return std::nullopt;
    }

    auto candidate =
        apfsSpaceManagerCandidateAt(*blockBytes, 0, context.expectedOid, context.blockSize);
    if (!candidate.has_value()) {
        return std::nullopt;
    }
    const auto relativeOffset = checkedProduct(blockNumber, context.blockSize);
    if (!relativeOffset.has_value() ||
        *relativeOffset > static_cast<uint64_t>(std::numeric_limits<qsizetype>::max())) {
        setProbeError(errorMessage, QStringLiteral("APFS checkpoint block offset is too large"));
        return std::nullopt;
    }
    candidate->blockOffset = static_cast<qsizetype>(*relativeOffset);
    return candidate;
}

std::optional<ApfsSupplementalReadContext> apfsSupplementalReadContext(
    const QByteArray& probeBytes,
    QIODevice* device,
    uint64_t partitionOffsetBytes,
    uint64_t partitionSizeBytes) {
    const uint32_t blockSize = littleEndian32(probeBytes, kApfsBlockSizeOffset);
    const uint64_t blockCount = littleEndian64(probeBytes, kApfsBlockCountOffset);
    if (blockSize < kMinimumApfsBlockSize || blockSize > kMaximumApfsBlockSize ||
        !isPowerOfTwo(blockSize) || blockCount == 0) {
        return std::nullopt;
    }

    const ApfsCheckpointValues checkpoint = apfsCheckpointValues(probeBytes);
    const uint64_t expectedOid = littleEndian64(probeBytes, kApfsSpacemanOidOffset);
    if (checkpoint.data_base == 0 || checkpoint.data_blocks == 0 || expectedOid == 0) {
        return std::nullopt;
    }

    return ApfsSupplementalReadContext{device,
                                       partitionOffsetBytes,
                                       partitionSizeBytes,
                                       blockSize,
                                       blockCount,
                                       expectedOid,
                                       checkpoint};
}

void setFirstSupplementalError(QString* target, const QString& error) {
    if (target && target->isEmpty() && !error.isEmpty()) {
        *target = error;
    }
}

bool appendApfsSupplementalCandidate(PartitionFileSystemDetection* detection,
                                     const ApfsSupplementalReadContext& context,
                                     const ApfsSpaceManagerCandidate& candidate) {
    const ApfsSpaceManagerContext spaceContext{context.blockSize, context.blockCount};
    const QStringList warnings = apfsSpaceManagerWarnings(spaceContext, candidate);
    if (!warnings.isEmpty()) {
        detection->details.append(
            QStringLiteral("APFS supplemental space manager block %1 ignored")
                .arg(static_cast<qulonglong>(candidate.blockOffset / context.blockSize)));
        appendMetadataSanity(detection,
                             QStringLiteral("APFS supplemental space manager counters are usable"),
                             warnings);
        return false;
    }

    detection->details.append(
        QStringLiteral("APFS space manager source: checkpoint data random read"));
    appendApfsSpaceManagerSuccess(detection, spaceContext, candidate);
    return true;
}

bool scanApfsSupplementalSpaceManager(PartitionFileSystemDetection* detection,
                                      const ApfsSupplementalReadContext& context,
                                      QString* errorMessage) {
    const uint32_t scanBlocks = std::min<uint32_t>(context.checkpoint.data_blocks,
                                                   kApfsSupplementalCheckpointScanBlocks);
    for (uint32_t delta = 0; delta < scanBlocks; ++delta) {
        const auto blockNumber = checkedSum(context.checkpoint.data_base, delta);
        if (!blockNumber.has_value() || !apfsBlockInsidePartition(*blockNumber,
                                                                  context.blockSize,
                                                                  context.partitionSizeBytes)) {
            continue;
        }
        QString readError;
        auto candidate = apfsSpaceManagerCandidateAtDeviceBlock(context, *blockNumber, &readError);
        if (!candidate.has_value()) {
            setFirstSupplementalError(errorMessage, readError);
            continue;
        }
        return appendApfsSupplementalCandidate(detection, context, *candidate);
    }

    return false;
}

bool appendApfsSupplementalSpaceManagerDetails(PartitionFileSystemDetection* detection,
                                               const ApfsSupplementalInput& input,
                                               QString* errorMessage) {
    if (!detection || !input.probeBytes || detection->file_system != QStringLiteral("APFS") ||
        hasApfsSpaceManagerDetails(*detection)) {
        return false;
    }

    const auto context = apfsSupplementalReadContext(
        *input.probeBytes, input.device, input.partitionOffsetBytes, input.partitionSizeBytes);
    return context.has_value() ? scanApfsSupplementalSpaceManager(detection, *context, errorMessage)
                               : false;
}

bool apfsBlockIsBtreeObject(const QByteArray& bytes, qsizetype blockOffset) {
    if (!hasBytes(bytes, blockOffset, kApfsObjectSubtypeOffset + kUint32Size)) {
        return false;
    }
    const uint32_t type = littleEndian32(bytes, blockOffset + kApfsObjectTypeOffset) &
                          kApfsObjectTypeMask;
    return type == kApfsObjectTypeBtreeRoot || type == kApfsObjectTypeBtreeNode;
}

QString apfsBlockReferenceLabel(qsizetype blockOffset,
                                uint32_t blockSize,
                                const QString& fieldLabel) {
    return QStringLiteral("object map block %1 %2")
        .arg(static_cast<qulonglong>(blockOffset / blockSize))
        .arg(fieldLabel);
}

void appendApfsReferenceFromBlockField(std::vector<ApfsObjectReference>* references,
                                       const QByteArray& bytes,
                                       qsizetype blockOffset,
                                       qsizetype fieldOffset,
                                       const QString& label) {
    appendApfsObjectReference(references, label, littleEndian64(bytes, blockOffset + fieldOffset));
}

void appendApfsVisibleObjectMapTreeReferences(std::vector<ApfsObjectReference>* references,
                                              const QByteArray& bytes,
                                              uint32_t blockSize) {
    if (!apfsProbeBlockScanSupported(bytes, blockSize)) {
        return;
    }

    const qsizetype step = static_cast<qsizetype>(blockSize);
    for (qsizetype blockOffset = 0; blockOffset + kApfsOmapMinimumBytes <= bytes.size();
         blockOffset += step) {
        const uint64_t oid = littleEndian64(bytes, blockOffset + kApfsObjectOidOffset);
        const QStringList labels = apfsLabelsForObjectOid(*references, oid);
        if (!apfsObjectMapLabels(labels) ||
            !apfsBlockHasObjectType(bytes, blockOffset, kApfsObjectTypeObjectMap)) {
            continue;
        }
        appendApfsReferenceFromBlockField(
            references,
            bytes,
            blockOffset,
            kApfsOmapTreeOidOffset,
            apfsBlockReferenceLabel(blockOffset, blockSize, QStringLiteral("tree OID")));
        appendApfsReferenceFromBlockField(
            references,
            bytes,
            blockOffset,
            kApfsOmapSnapshotTreeOidOffset,
            apfsBlockReferenceLabel(blockOffset, blockSize, QStringLiteral("snapshot tree OID")));
    }
}

void appendApfsObjectMapDetailIfVisible(QStringList* objectMapSummaries,
                                        const QByteArray& bytes,
                                        qsizetype blockOffset,
                                        uint32_t blockSize,
                                        const QStringList& labels) {
    if (!apfsObjectMapLabels(labels) || objectMapSummaries->size() >= kMaxApfsObjectMapDetails) {
        return;
    }
    const QString summary = apfsObjectMapDetailSummary(bytes, blockOffset, blockSize, labels);
    if (!summary.isEmpty()) {
        objectMapSummaries->append(summary);
    }
}

QStringList apfsBtreeNodeFlagNames(uint16_t flags) {
    QStringList names;
    if ((flags & kApfsBtreeNodeRootFlag) != 0) {
        names.append(QStringLiteral("root"));
    }
    if ((flags & kApfsBtreeNodeLeafFlag) != 0) {
        names.append(QStringLiteral("leaf"));
    }
    if ((flags & kApfsBtreeNodeFixedKvFlag) != 0) {
        names.append(QStringLiteral("fixed-kv"));
    }
    if ((flags & kApfsBtreeNodeHashedFlag) != 0) {
        names.append(QStringLiteral("hashed"));
    }
    if ((flags & kApfsBtreeNodeNoHeaderFlag) != 0) {
        names.append(QStringLiteral("no-header"));
    }
    if ((flags & kApfsBtreeNodeCheckKeyOffsetFlag) != 0) {
        names.append(QStringLiteral("check-key-offset"));
    }
    return names;
}

void appendApfsBtreeInfoIfRoot(QStringList* parts,
                               const QByteArray& bytes,
                               qsizetype blockOffset,
                               uint32_t blockSize,
                               uint16_t nodeFlags) {
    if ((nodeFlags & kApfsBtreeNodeRootFlag) == 0 || blockSize < kApfsBtreeInfoSize) {
        return;
    }
    const qsizetype infoOffset = blockOffset + static_cast<qsizetype>(blockSize) -
                                 kApfsBtreeInfoSize;
    if (!hasBytes(bytes, infoOffset, kApfsBtreeInfoSize)) {
        return;
    }

    parts->append(QStringLiteral("tree flags %1")
                      .arg(hex32(littleEndian32(bytes, infoOffset + kApfsBtreeInfoFlagsOffset))));
    parts->append(QStringLiteral("tree node size %1")
                      .arg(littleEndian32(bytes, infoOffset + kApfsBtreeInfoNodeSizeOffset)));
    parts->append(QStringLiteral("tree key size %1")
                      .arg(littleEndian32(bytes, infoOffset + kApfsBtreeInfoKeySizeOffset)));
    parts->append(QStringLiteral("tree value size %1")
                      .arg(littleEndian32(bytes, infoOffset + kApfsBtreeInfoValueSizeOffset)));
    parts->append(QStringLiteral("tree longest key %1")
                      .arg(littleEndian32(bytes, infoOffset + kApfsBtreeInfoLongestKeyOffset)));
    parts->append(QStringLiteral("tree longest value %1")
                      .arg(littleEndian32(bytes, infoOffset + kApfsBtreeInfoLongestValueOffset)));
    parts->append(QStringLiteral("tree key count %1")
                      .arg(littleEndian64(bytes, infoOffset + kApfsBtreeInfoKeyCountOffset)));
    parts->append(QStringLiteral("tree node count %1")
                      .arg(littleEndian64(bytes, infoOffset + kApfsBtreeInfoNodeCountOffset)));
}

QString apfsBtreeNodeDetailSummary(const QByteArray& bytes,
                                   qsizetype blockOffset,
                                   uint32_t blockSize,
                                   const QStringList& labels) {
    if (!hasBytes(bytes, blockOffset, kApfsBtreeNodeMinimumBytes)) {
        return {};
    }

    QStringList parts;
    const uint16_t nodeFlags = littleEndian16(bytes, blockOffset + kApfsBtreeNodeFlagsOffset);
    const QStringList flagNames = apfsBtreeNodeFlagNames(nodeFlags);
    parts.append(QStringLiteral("APFS B-tree node detail block %1")
                     .arg(static_cast<qulonglong>(blockOffset / blockSize)));
    parts.append(QStringLiteral("labels %1").arg(labels.join(QStringLiteral(" / "))));
    parts.append(
        QStringLiteral("OID %1").arg(littleEndian64(bytes, blockOffset + kApfsObjectOidOffset)));
    parts.append(QStringLiteral("flags %1").arg(hex16(nodeFlags)));
    if (!flagNames.isEmpty()) {
        parts.append(QStringLiteral("flag names %1").arg(flagNames.join(QStringLiteral("/"))));
    }
    parts.append(QStringLiteral("level %1")
                     .arg(littleEndian16(bytes, blockOffset + kApfsBtreeNodeLevelOffset)));
    parts.append(QStringLiteral("keys %1").arg(
        littleEndian32(bytes, blockOffset + kApfsBtreeNodeKeyCountOffset)));
    parts.append(
        QStringLiteral("table space %1:%2")
            .arg(littleEndian16(bytes, blockOffset + kApfsBtreeNodeTableSpaceOffsetOffset))
            .arg(littleEndian16(bytes, blockOffset + kApfsBtreeNodeTableSpaceLengthOffset)));
    parts.append(
        QStringLiteral("free space %1:%2")
            .arg(littleEndian16(bytes, blockOffset + kApfsBtreeNodeFreeSpaceOffsetOffset))
            .arg(littleEndian16(bytes, blockOffset + kApfsBtreeNodeFreeSpaceLengthOffset)));
    parts.append(
        QStringLiteral("key free list %1:%2")
            .arg(littleEndian16(bytes, blockOffset + kApfsBtreeNodeKeyFreeListOffsetOffset))
            .arg(littleEndian16(bytes, blockOffset + kApfsBtreeNodeKeyFreeListLengthOffset)));
    parts.append(
        QStringLiteral("value free list %1:%2")
            .arg(littleEndian16(bytes, blockOffset + kApfsBtreeNodeValueFreeListOffsetOffset))
            .arg(littleEndian16(bytes, blockOffset + kApfsBtreeNodeValueFreeListLengthOffset)));
    appendApfsBtreeInfoIfRoot(&parts, bytes, blockOffset, blockSize, nodeFlags);
    return parts.join(QStringLiteral(", "));
}

void appendApfsBtreeNodeDetails(PartitionFileSystemDetection* detection,
                                const QByteArray& bytes,
                                uint32_t blockSize,
                                const std::vector<ApfsObjectReference>& references) {
    if (references.empty() || !apfsProbeBlockScanSupported(bytes, blockSize)) {
        return;
    }

    QStringList summaries;
    const qsizetype step = static_cast<qsizetype>(blockSize);
    for (qsizetype blockOffset = 0; blockOffset + kApfsBtreeNodeMinimumBytes <= bytes.size();
         blockOffset += step) {
        const uint64_t oid = littleEndian64(bytes, blockOffset + kApfsObjectOidOffset);
        const QStringList labels = apfsLabelsForObjectOid(references, oid);
        if (labels.isEmpty() || !apfsBlockIsBtreeObject(bytes, blockOffset)) {
            continue;
        }
        if (summaries.size() >= kMaxApfsBtreeNodeDetails) {
            break;
        }
        summaries.append(apfsBtreeNodeDetailSummary(bytes, blockOffset, blockSize, labels));
    }
    if (!summaries.isEmpty()) {
        detection->details.append(
            QStringLiteral("APFS B-tree node details in probe window: %1").arg(summaries.size()));
        detection->details.append(summaries);
    }
}

void collectApfsReferencedObjectHeaderDetails(QStringList* summaries,
                                              QStringList* objectMapSummaries,
                                              const QByteArray& bytes,
                                              uint32_t blockSize,
                                              const std::vector<ApfsObjectReference>& references) {
    const qsizetype step = static_cast<qsizetype>(blockSize);
    for (qsizetype blockOffset = 0;
         blockOffset + kApfsObjectSubtypeOffset + kUint32Size <= bytes.size();
         blockOffset += step) {
        const uint64_t oid = littleEndian64(bytes, blockOffset + kApfsObjectOidOffset);
        const QStringList labels = apfsLabelsForObjectOid(references, oid);
        if (!labels.isEmpty() && summaries->size() < kMaxApfsReferencedObjectHeaders) {
            summaries->append(
                apfsReferencedObjectHeaderSummary(bytes, blockOffset, blockSize, labels, oid));
        }
        appendApfsObjectMapDetailIfVisible(
            objectMapSummaries, bytes, blockOffset, blockSize, labels);
    }
}

void appendApfsReferencedObjectHeaderDetails(PartitionFileSystemDetection* detection,
                                             const QByteArray& bytes,
                                             uint32_t blockSize,
                                             const std::vector<ApfsObjectReference>& references) {
    if (references.empty() || !apfsProbeBlockScanSupported(bytes, blockSize)) {
        return;
    }

    QStringList summaries;
    QStringList objectMapSummaries;
    collectApfsReferencedObjectHeaderDetails(
        &summaries, &objectMapSummaries, bytes, blockSize, references);
    if (summaries.isEmpty()) {
        return;
    }

    detection->details.append(
        QStringLiteral("APFS referenced object headers in probe window: %1").arg(summaries.size()));
    detection->details.append(summaries);
    if (!objectMapSummaries.isEmpty()) {
        detection->details.append(QStringLiteral("APFS object map details in probe window: %1")
                                      .arg(objectMapSummaries.size()));
        detection->details.append(objectMapSummaries);
    }
}

void appendApfsCheckpointDetails(PartitionFileSystemDetection* detection,
                                 const ApfsCheckpointValues& checkpoint) {
    appendDetailIfNonZero(detection,
                          QStringLiteral("Checkpoint descriptor base block"),
                          checkpoint.descriptor_base);
    appendDetailIfNonZero(detection,
                          QStringLiteral("Checkpoint data base block"),
                          checkpoint.data_base);
    detection->details.append(
        QStringLiteral("Checkpoint descriptor next index: %1").arg(checkpoint.descriptor_next));
    detection->details.append(
        QStringLiteral("Checkpoint data next index: %1").arg(checkpoint.data_next));
    detection->details.append(
        QStringLiteral("Checkpoint descriptor start index: %1").arg(checkpoint.descriptor_index));
    detection->details.append(
        QStringLiteral("Checkpoint data start index: %1").arg(checkpoint.data_index));
    appendDetailIfNonZero(detection,
                          QStringLiteral("Checkpoint descriptor length"),
                          checkpoint.descriptor_length);
    appendDetailIfNonZero(detection,
                          QStringLiteral("Checkpoint data length"),
                          checkpoint.data_length);
}

void appendApfsObjectDetails(PartitionFileSystemDetection* detection,
                             const QByteArray& bytes,
                             uint32_t maxFileSystems) {
    appendDetailIfNonZero(detection,
                          QStringLiteral("Space manager OID"),
                          littleEndian64(bytes, kApfsSpacemanOidOffset));
    appendDetailIfNonZero(detection,
                          QStringLiteral("Object map OID"),
                          littleEndian64(bytes, kApfsObjectMapOidOffset));
    appendDetailIfNonZero(detection,
                          QStringLiteral("Reaper OID"),
                          littleEndian64(bytes, kApfsReaperOidOffset));

    const QStringList volumeOids = apfsVolumeOids(bytes, maxFileSystems);
    detection->details.append(QStringLiteral("Volume OID slots used: %1").arg(volumeOids.size()));
    if (!volumeOids.isEmpty()) {
        detection->details.append(
            QStringLiteral("Volume OIDs: %1").arg(volumeOids.join(QStringLiteral(", "))));
    }
}

QString apfsVolumeCandidateSummary(const QByteArray& bytes,
                                   qsizetype blockOffset,
                                   uint32_t blockSize) {
    QStringList parts;
    parts.append(QStringLiteral("APFS volume candidate block %1")
                     .arg(static_cast<qulonglong>(blockOffset / blockSize)));
    parts.append(QStringLiteral("index %1")
                     .arg(littleEndian32(bytes, blockOffset + kApfsVolumeIndexOffset)));

    appendSummaryPartIfText(
        &parts,
        QStringLiteral("name"),
        fixedUtf8Field(bytes, blockOffset + kApfsVolumeNameOffset, kApfsVolumeNameSize));
    appendSummaryPartIfText(&parts,
                            QStringLiteral("uuid"),
                            uuidField(bytes, blockOffset + kApfsVolumeUuidOffset));
    appendSummaryPartIfNonZero(&parts,
                               QStringLiteral("role"),
                               littleEndian16(bytes, blockOffset + kApfsVolumeRoleOffset));
    appendSummaryPartIfNonZero(&parts,
                               QStringLiteral("reserve blocks"),
                               littleEndian64(bytes,
                                              blockOffset + kApfsVolumeReserveBlockCountOffset));
    appendSummaryPartIfNonZero(&parts,
                               QStringLiteral("quota blocks"),
                               littleEndian64(bytes,
                                              blockOffset + kApfsVolumeQuotaBlockCountOffset));
    appendSummaryPartIfNonZero(&parts,
                               QStringLiteral("allocated blocks"),
                               littleEndian64(bytes,
                                              blockOffset + kApfsVolumeAllocatedBlockCountOffset));
    appendSummaryPartIfNonZero(&parts,
                               QStringLiteral("volume object map OID"),
                               littleEndian64(bytes, blockOffset + kApfsVolumeObjectMapOidOffset));
    appendSummaryPartIfNonZero(&parts,
                               QStringLiteral("root tree OID"),
                               littleEndian64(bytes, blockOffset + kApfsVolumeRootTreeOidOffset));
    appendSummaryPartIfNonZero(&parts,
                               QStringLiteral("extentref tree OID"),
                               littleEndian64(bytes,
                                              blockOffset + kApfsVolumeExtentrefTreeOidOffset));
    appendSummaryPartIfNonZero(
        &parts,
        QStringLiteral("snapshot metadata tree OID"),
        littleEndian64(bytes, blockOffset + kApfsVolumeSnapshotMetadataTreeOidOffset));
    return parts.join(QStringLiteral(", "));
}

void appendApfsVolumeCandidateDetails(PartitionFileSystemDetection* detection,
                                      const QByteArray& bytes,
                                      uint32_t blockSize) {
    if (blockSize < kMinimumApfsBlockSize || blockSize > kMaximumApfsBlockSize ||
        !isPowerOfTwo(blockSize) || blockSize > static_cast<uint32_t>(bytes.size())) {
        return;
    }

    const qsizetype step = static_cast<qsizetype>(blockSize);
    int candidateCount = 0;
    QStringList candidateSummaries;
    for (qsizetype blockOffset = step; blockOffset + kApfsVolumeMinimumBytes <= bytes.size();
         blockOffset += step) {
        if (!matchesBytes(
                bytes, blockOffset + kApfsVolumeMagicOffset, "APSB", kApfsVolumeMagicSize)) {
            continue;
        }
        ++candidateCount;
        if (candidateSummaries.size() < kMaxApfsVolumeCandidates) {
            candidateSummaries.append(apfsVolumeCandidateSummary(bytes, blockOffset, blockSize));
        }
    }
    if (candidateCount == 0) {
        return;
    }

    detection->details.append(
        QStringLiteral("APFS volume superblock candidates in probe window: %1")
            .arg(candidateCount));
    detection->details.append(candidateSummaries);
    if (candidateCount > candidateSummaries.size()) {
        detection->details.append(
            QStringLiteral("APFS volume candidate output truncated after %1 entries")
                .arg(candidateSummaries.size()));
    }
}

std::optional<PartitionFileSystemDetection> detectApfsFamily(const QByteArray& bytes) {
    if (!matchesBytes(bytes, kApfsMagicOffset, "NXSB", kApfsMagicSize)) {
        return std::nullopt;
    }

    PartitionFileSystemDetection detection{QStringLiteral("APFS"),
                                           PartitionFileSystemDetector::rawSignatureSource()};
    const uint32_t blockSize = littleEndian32(bytes, kApfsBlockSizeOffset);
    const uint64_t blockCount = littleEndian64(bytes, kApfsBlockCountOffset);
    const ApfsCheckpointValues checkpoint = apfsCheckpointValues(bytes);
    const uint32_t maxFileSystems = littleEndian32(bytes, kApfsMaxFileSystemsOffset);
    appendApfsSizeDetails(&detection, blockSize, blockCount);
    appendDetailIfText(&detection,
                       QStringLiteral("Container UUID"),
                       uuidField(bytes, kApfsUuidOffset));
    detection.details.append(
        QStringLiteral("Features: %1").arg(hex64(littleEndian64(bytes, kApfsFeaturesOffset))));
    detection.details.append(
        QStringLiteral("Read-only compatible features: %1")
            .arg(hex64(littleEndian64(bytes, kApfsReadOnlyCompatibleFeaturesOffset))));
    detection.details.append(
        QStringLiteral("Incompatible features: %1")
            .arg(hex64(littleEndian64(bytes, kApfsIncompatibleFeaturesOffset))));
    appendDetailIfNonZero(&detection,
                          QStringLiteral("Next object ID"),
                          littleEndian64(bytes, kApfsNextObjectIdOffset));
    appendDetailIfNonZero(&detection,
                          QStringLiteral("Next transaction ID"),
                          littleEndian64(bytes, kApfsNextTransactionIdOffset));
    appendDetailIfNonZero(&detection,
                          QStringLiteral("Checkpoint descriptor blocks"),
                          checkpoint.descriptor_blocks);
    appendDetailIfNonZero(&detection,
                          QStringLiteral("Checkpoint data blocks"),
                          checkpoint.data_blocks);
    appendApfsCheckpointDetails(&detection, checkpoint);
    appendApfsObjectDetails(&detection, bytes, maxFileSystems);
    appendApfsSpaceManagerDetails(&detection, bytes, blockSize, blockCount);
    appendApfsVolumeCandidateDetails(&detection, bytes, blockSize);
    auto objectReferences = apfsObjectReferences(bytes, blockSize, maxFileSystems);
    appendApfsVisibleObjectMapTreeReferences(&objectReferences, bytes, blockSize);
    appendApfsReferencedObjectHeaderDetails(&detection, bytes, blockSize, objectReferences);
    appendApfsBtreeNodeDetails(&detection, bytes, blockSize, objectReferences);
    appendDetailIfNonZero(&detection, QStringLiteral("Max file systems"), maxFileSystems);
    appendMetadataSanity(&detection,
                         QStringLiteral("APFS container block geometry is internally consistent"),
                         apfsMetadataWarnings(blockSize, blockCount, checkpoint));
    return detection;
}

std::optional<PartitionFileSystemDetection> rawDetection(const QString& fileSystem) {
    if (fileSystem.isEmpty()) {
        return std::nullopt;
    }
    return PartitionFileSystemDetection{fileSystem,
                                        PartitionFileSystemDetector::rawSignatureSource()};
}

std::optional<PartitionFileSystemDetection> detectSwapFamily(const QByteArray& bytes,
                                                             uint64_t partitionSizeBytes) {
    const auto signature = swapSignatureInfo(bytes, partitionSizeBytes);
    if (!signature.has_value()) {
        return std::nullopt;
    }
    auto detection = rawDetection(QStringLiteral("Linux swap"));
    if (!detection.has_value()) {
        return std::nullopt;
    }
    detection->details.append(QStringLiteral("Signature: %1").arg(signature->signature));
    detection->details.append(QStringLiteral("Detected page size: %1").arg(signature->page_size));
    if (signature->signature != QStringLiteral("SWAPSPACE2")) {
        detection->details.append(
            QStringLiteral("Legacy swap signature does not expose modern label/UUID metadata"));
        return detection;
    }
    if (!hasBytes(bytes, kSwapInfoOffset, kSwapLabelOffset + kSwapLabelSize - kSwapInfoOffset)) {
        return detection;
    }

    const uint32_t version = littleEndian32(bytes, kSwapVersionOffset);
    const uint32_t lastPage = littleEndian32(bytes, kSwapLastPageOffset);
    const uint32_t badPages = littleEndian32(bytes, kSwapBadPagesOffset);
    detection->details.append(QStringLiteral("Header version: %1").arg(version));
    detection->details.append(QStringLiteral("Last page: %1").arg(lastPage));
    detection->details.append(QStringLiteral("Bad pages: %1").arg(badPages));
    appendDetailIfText(&*detection, QStringLiteral("UUID"), uuidField(bytes, kSwapUuidOffset));
    appendDetailIfText(&*detection,
                       QStringLiteral("Volume label"),
                       fixedAsciiField(bytes, kSwapLabelOffset, kSwapLabelSize));
    if (lastPage > 0) {
        const auto totalBytes = checkedProduct(static_cast<uint64_t>(lastPage) + 1ULL,
                                               static_cast<uint64_t>(signature->page_size));
        if (totalBytes.has_value()) {
            detection->total_bytes = *totalBytes;
            detection->details.append(QStringLiteral("Total swap bytes: %1").arg(*totalBytes));
        }
    }
    return detection;
}

void setProbeError(QString* errorMessage, const QString& message) {
    if (errorMessage) {
        *errorMessage = message;
    }
}

bool validateProbeReadRequest(const QString& devicePath,
                              uint64_t partitionOffsetBytes,
                              QString* errorMessage) {
    if (devicePath.trimmed().isEmpty()) {
        setProbeError(errorMessage, QStringLiteral("Raw probe device path is empty"));
        return false;
    }
    if (partitionOffsetBytes > static_cast<uint64_t>(std::numeric_limits<qint64>::max())) {
        setProbeError(errorMessage, QStringLiteral("Raw probe partition offset is too large"));
        return false;
    }
    return true;
}

bool seekProbeDevice(QIODevice* device, uint64_t offsetBytes, QString* errorMessage) {
    if (device->seek(static_cast<qint64>(offsetBytes))) {
        return true;
    }
    setProbeError(errorMessage,
                  QStringLiteral("Raw probe seek failed: %1").arg(device->errorString()));
    return false;
}

std::optional<QByteArray> readProbeBytes(QIODevice* device,
                                         qsizetype readLimit,
                                         QString* errorMessage) {
    const QByteArray bytes = device->read(readLimit);
    if (!bytes.isEmpty()) {
        return bytes;
    }
    setProbeError(errorMessage,
                  QStringLiteral("Raw probe read failed: %1").arg(device->errorString()));
    return std::nullopt;
}

}  // namespace

qsizetype PartitionFileSystemDetector::maxProbeBytes() noexcept {
    return kMaxProbeBytes;
}

QString PartitionFileSystemDetector::rawSignatureSource() {
    return QStringLiteral("RawSignature");
}

QString PartitionFileSystemDetector::windowsVolumeSource() {
    return QStringLiteral("WindowsVolume");
}

QString PartitionFileSystemDetector::inferredProtectedSource() {
    return QStringLiteral("InferredProtected");
}

QString PartitionFileSystemDetector::sourceDisplayName(const QString& source) {
    if (source == rawSignatureSource()) {
        return QStringLiteral("read-only raw signature");
    }
    if (source == windowsVolumeSource()) {
        return QStringLiteral("Windows volume metadata");
    }
    if (source == inferredProtectedSource()) {
        return QStringLiteral("protected partition type");
    }
    return {};
}

std::optional<PartitionFileSystemDetection> PartitionFileSystemDetector::detectBytes(
    const QByteArray& bytes, uint64_t partition_size_bytes) {
    if (bytes.isEmpty()) {
        return std::nullopt;
    }

    if (const auto swapDetection = detectSwapFamily(bytes, partition_size_bytes);
        swapDetection.has_value()) {
        return swapDetection;
    }
    if (const QString windowsFileSystem = detectWindowsBootSectorFamily(bytes);
        !windowsFileSystem.isEmpty()) {
        return rawDetection(windowsFileSystem);
    }
    if (const auto xfsDetection = detectXfsFamily(bytes); xfsDetection.has_value()) {
        return xfsDetection;
    }
    if (const auto btrfsDetection = detectBtrfsFamily(bytes); btrfsDetection.has_value()) {
        return btrfsDetection;
    }
    if (const auto apfsDetection = detectApfsFamily(bytes); apfsDetection.has_value()) {
        return apfsDetection;
    }
    if (const auto hfsDetection = detectHfsPlusFamily(bytes); hfsDetection.has_value()) {
        return hfsDetection;
    }
    if (const auto extDetection = detectExtFamily(bytes); extDetection.has_value()) {
        return extDetection;
    }
    return std::nullopt;
}

qsizetype PartitionFileSystemDetector::probeReadLimit(uint64_t partition_size_bytes) noexcept {
    if (partition_size_bytes == 0) {
        return kMaxProbeBytes;
    }
    return std::min<qsizetype>(kMaxProbeBytes,
                               static_cast<qsizetype>(std::min<uint64_t>(
                                   partition_size_bytes,
                                   static_cast<uint64_t>(std::numeric_limits<qsizetype>::max()))));
}

std::optional<QByteArray> PartitionFileSystemDetector::readProbeBytesFromDevicePath(
    const QString& device_path,
    uint64_t partition_offset_bytes,
    uint64_t partition_size_bytes,
    QString* error_message) {
    if (error_message) {
        error_message->clear();
    }
    if (!validateProbeReadRequest(device_path, partition_offset_bytes, error_message)) {
        return std::nullopt;
    }
    QString openError;
    auto device = openFileOrRawDeviceReadOnly(device_path, &openError);
    if (!device) {
        setProbeError(error_message, QStringLiteral("Raw probe open failed: %1").arg(openError));
        return std::nullopt;
    }
    if (!seekProbeDevice(device.get(), partition_offset_bytes, error_message)) {
        return std::nullopt;
    }

    return readProbeBytes(device.get(), probeReadLimit(partition_size_bytes), error_message);
}

std::optional<PartitionFileSystemDetection> PartitionFileSystemDetector::detectFromDevice(
    QIODevice* device,
    uint64_t partition_offset_bytes,
    uint64_t partition_size_bytes,
    QString* error_message) {
    if (error_message) {
        error_message->clear();
    }
    if (!device || !device->isOpen()) {
        setProbeError(error_message, QStringLiteral("Raw probe device is not open"));
        return std::nullopt;
    }
    if (partition_offset_bytes > static_cast<uint64_t>(std::numeric_limits<qint64>::max())) {
        setProbeError(error_message, QStringLiteral("Raw probe partition offset is too large"));
        return std::nullopt;
    }
    if (!seekProbeDevice(device, partition_offset_bytes, error_message)) {
        return std::nullopt;
    }

    const auto bytes = readProbeBytes(device, probeReadLimit(partition_size_bytes), error_message);
    if (!bytes.has_value()) {
        return std::nullopt;
    }

    auto detection = detectBytes(*bytes, partition_size_bytes);
    if (!detection.has_value()) {
        setProbeError(error_message, QStringLiteral("No filesystem signature detected"));
        return std::nullopt;
    }
    const ApfsSupplementalInput supplementalInput{
        &*bytes, device, partition_offset_bytes, partition_size_bytes};
    appendApfsSupplementalSpaceManagerDetails(&*detection, supplementalInput, error_message);
    if (error_message) {
        error_message->clear();
    }
    return detection;
}

std::optional<PartitionFileSystemDetection> PartitionFileSystemDetector::detectFromDevicePath(
    const QString& device_path,
    uint64_t partition_offset_bytes,
    uint64_t partition_size_bytes,
    QString* error_message) {
    if (error_message) {
        error_message->clear();
    }
    if (!validateProbeReadRequest(device_path, partition_offset_bytes, error_message)) {
        return std::nullopt;
    }
    QString openError;
    auto device = openFileOrRawDeviceReadOnly(device_path, &openError);
    if (!device) {
        setProbeError(error_message, QStringLiteral("Raw probe open failed: %1").arg(openError));
        return std::nullopt;
    }
    return detectFromDevice(
        device.get(), partition_offset_bytes, partition_size_bytes, error_message);
}

}  // namespace sak

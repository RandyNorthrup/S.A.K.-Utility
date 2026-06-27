// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_hfs_core.h
/// @brief Shared HFS+/HFSX primitives: on-disk structs plus byte, record,
///        extent and allocation-bitmap helpers. Consumed by the HfsReader engine
///        (partition_hfs_internal.h). Internal anonymous namespace, not public.

#pragma once

#include "sak/partition_hfs_case_folding.h"
#include "sak/partition_hfs_file_system_reader.h"
#include "sak/partition_raw_device_io.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <functional>
#include <limits>
#include <optional>
#include <utility>

namespace sak {
namespace {


constexpr uint64_t kHfsVolumeHeaderOffset = 1024;
constexpr qsizetype kHfsVolumeHeaderSize = 512;
constexpr qsizetype kHfsSignatureSize = 2;
constexpr qsizetype kHfsVersionOffset = 2;
constexpr qsizetype kHfsAttributesOffset = 4;
constexpr qsizetype kHfsFileCountOffset = 32;
constexpr qsizetype kHfsFolderCountOffset = 36;
constexpr qsizetype kHfsBlockSizeOffset = 40;
constexpr qsizetype kHfsTotalBlocksOffset = 44;
constexpr qsizetype kHfsFreeBlocksOffset = 48;
constexpr qsizetype kHfsNextCatalogIdOffset = 64;
constexpr qsizetype kHfsAllocationForkOffset = 112;
constexpr qsizetype kHfsExtentsForkOffset = 192;
constexpr qsizetype kHfsCatalogForkOffset = 272;
constexpr qsizetype kHfsAttributesForkOffset = 352;
constexpr qsizetype kHfsForkLogicalSizeOffset = 0;
constexpr qsizetype kHfsForkTotalBlocksOffset = 12;
constexpr qsizetype kHfsForkExtentsOffset = 16;
constexpr qsizetype kHfsExtentStartBlockOffset = 0;
constexpr qsizetype kHfsExtentBlockCountOffset = 4;
constexpr qsizetype kHfsExtentBytes = 8;
constexpr int kHfsExtentCount = 8;
constexpr qsizetype kUint16Size = 2;
constexpr qsizetype kUint32Size = 4;
constexpr qsizetype kUint64Size = 8;
constexpr uint32_t kBitsPerByte = 8;
constexpr int kHfsMaximumNameChars = 255;
constexpr int kHfsMaximumAttributeNameChars = 127;
constexpr qsizetype kHfsWrapperMdbOffset = 1024;
constexpr qsizetype kHfsWrapperMdbSize = 512;
constexpr qsizetype kHfsWrapperAllocationBlockSizeOffset = 0x14;
constexpr qsizetype kHfsWrapperAllocationBlockStartOffset = 0x1C;
constexpr qsizetype kHfsWrapperEmbeddedSignatureOffset = 0x7C;
constexpr qsizetype kHfsWrapperEmbeddedExtentStartOffset = 0x7E;
constexpr qsizetype kHfsWrapperEmbeddedExtentCountOffset = 0x80;
constexpr uint64_t kHfsWrapperSectorBytes = 512;
constexpr uint16_t kHfsPlusVersion = 4;
constexpr uint16_t kHfsXMinimumVersion = 5;
constexpr uint32_t kHfsVolumeJournaledMask = 0x00'00'20'00;
constexpr qsizetype kHfsVolumeJournalInfoBlockOffset = 12;
constexpr uint32_t kHfsJournalInFsMask = 0x00'00'00'01;
constexpr uint32_t kHfsJournalOnOtherDeviceMask = 0x00'00'00'02;
constexpr uint32_t kHfsJournalNeedInitMask = 0x00'00'00'04;
constexpr qsizetype kHfsJournalInfoOffsetField = 36;
constexpr qsizetype kHfsJournalInfoSizeField = 44;
constexpr uint32_t kHfsJournalHeaderMagic = 0x4a'4e'4c'78;  // 'JNLx'
constexpr uint32_t kHfsJournalEndianMagic = 0x12'34'56'78;
constexpr qsizetype kHfsJournalHeaderBytes = 44;
constexpr qsizetype kHfsJournalHeaderStartField = 8;
constexpr qsizetype kHfsJournalHeaderEndField = 16;
constexpr qsizetype kHfsJournalHeaderSizeField = 24;
constexpr qsizetype kHfsJournalHeaderBlhdrSizeField = 32;
constexpr qsizetype kHfsJournalHeaderChecksumField = 36;
constexpr qsizetype kHfsJournalHeaderJhdrSizeField = 40;
constexpr qsizetype kHfsJournalBlhdrChecksumBytes = 32;
constexpr qsizetype kHfsJournalBlhdrNumBlocksField = 2;
constexpr qsizetype kHfsJournalBlhdrBytesUsedField = 4;
constexpr qsizetype kHfsJournalBlhdrChecksumField = 8;
constexpr qsizetype kHfsJournalBlockInfoOffset = 16;
constexpr qsizetype kHfsJournalBlockInfoBytes = 16;
constexpr qsizetype kHfsJournalBlockInfoSizeField = 8;
constexpr uint64_t kHfsJournalSectorBytes = 512;
constexpr int kHfsJournalMaxTransactions = 4096;
constexpr int kHfsJournalMaxBlocksPerTransaction = 8192;
constexpr uint32_t kHfsVolumeInconsistentMask = 0x00'00'40'00;
constexpr uint32_t kHfsRootFolderId = 2;
constexpr uint32_t kHfsRootParentId = 1;
constexpr uint32_t kHfsExtentsFileId = 3;
constexpr uint32_t kHfsCatalogFileId = 4;
constexpr uint32_t kHfsAllocationFileId = 6;
constexpr uint32_t kHfsAttributesFileId = 8;
constexpr uint8_t kHfsDataForkType = 0x00;
constexpr uint8_t kHfsResourceForkType = 0xFF;
constexpr uint16_t kHfsCatalogFolderRecord = 1;
constexpr uint16_t kHfsCatalogFileRecord = 2;
constexpr uint16_t kHfsCatalogFolderThreadRecord = 3;
constexpr uint16_t kHfsCatalogFileThreadRecord = 4;
constexpr qsizetype kHfsCatalogRecordTypeOffset = 0;
constexpr qsizetype kHfsCatalogRecordFlagsOffset = 2;
constexpr qsizetype kHfsCatalogFolderValenceOffset = 4;
constexpr qsizetype kHfsCatalogRecordIdOffset = 8;
constexpr qsizetype kHfsCatalogCreateDateOffset = 12;
constexpr qsizetype kHfsCatalogContentModDateOffset = 16;
constexpr qsizetype kHfsCatalogAttributeModDateOffset = 20;
constexpr qsizetype kHfsCatalogAccessDateOffset = 24;
constexpr qsizetype kHfsCatalogBackupDateOffset = 28;
constexpr qsizetype kHfsCatalogBsdOwnerIdOffset = 32;
constexpr qsizetype kHfsCatalogBsdGroupIdOffset = 36;
constexpr qsizetype kHfsCatalogBsdOwnerFlagsOffset = 41;
constexpr qsizetype kHfsCatalogBsdFileModeOffset = 42;
// HFSPlusBSDInfo.special union @44: iNodeNum (hard-link alias), linkCount (inode).
constexpr qsizetype kHfsCatalogBsdSpecialOffset = 44;
// FileInfo userInfo @48: fdType @48, fdCreator @52, fdFlags @56.
constexpr qsizetype kHfsCatalogFileTypeOffset = 48;
constexpr qsizetype kHfsCatalogFileCreatorOffset = 52;
constexpr qsizetype kHfsCatalogFileFinderFlagsOffset = 56;
// HFSPlusCatalogFolder userInfo (FolderInfo) @48 (16 bytes: window rect + flags).
constexpr qsizetype kHfsCatalogFolderInfoOffset = 48;
constexpr uint32_t kHfsToUnixEpochSeconds = 2'082'844'800U;
constexpr uint16_t kHfsFileModeRegular = 0x81A4;     // S_IFREG | 0644
constexpr uint16_t kHfsFileModeDirectory = 0x41ED;   // S_IFDIR | 0755
constexpr uint16_t kHfsFileModeSymlink = 0xA1ED;     // S_IFLNK | 0755
constexpr uint16_t kHfsFileModePrivateDir = 0x4000;  // S_IFDIR | 0000 (metadata dir)
constexpr uint16_t kHfsFileModeWriteBits = 0x0092;   // S_IWUSR|S_IWGRP|S_IWOTH (0222)
// H5 hard-link / symlink record flags + type/creator (TN1150, harvested from macOS).
constexpr uint16_t kHfsCatalogThreadExistsMask = 0x0002;
constexpr uint16_t kHfsCatalogHasLinkChainMask = 0x0020;
constexpr uint32_t kHfsHardLinkFileType = 0x68'6C'6E'6B;     // 'hlnk'
constexpr uint32_t kHfsHardLinkFileCreator = 0x68'66'73'2B;  // 'hfs+'
constexpr uint32_t kHfsSymlinkFileType = 0x73'6C'6E'6B;      // 'slnk'
constexpr uint32_t kHfsSymlinkFileCreator = 0x72'68'61'70;   // 'rhap'
constexpr uint16_t kHfsHardLinkAliasFinderFlags = 0x0100;    // kHasBeenInited
constexpr uint8_t kHfsHardLinkAliasOwnerFlags = 0x02;
// HFSPlusCatalogFile.reserved1 @4 holds hl_firstLinkID on a hard-link inode.
constexpr qsizetype kHfsCatalogFirstLinkIdOffset = 4;
constexpr uint16_t kHfsPrivateDirFlags = 0x0010;
// FolderInfo magic macOS stamps on the private metadata directory (bytes @56..63 of
// the folder record: frFlags 0x5000 + frLocation {0x4000, 0x0000}).
constexpr uint16_t kHfsPrivateDirFinderFlags = 0x5000;
constexpr uint16_t kHfsPrivateDirFinderLocV = 0x4000;
constexpr qsizetype kHfsCatalogFileDataForkOffset = 88;
constexpr qsizetype kHfsForkDataBytes = kHfsForkExtentsOffset + kHfsExtentBytes * kHfsExtentCount;
constexpr qsizetype kHfsCatalogFileResourceForkOffset = kHfsCatalogFileDataForkOffset +
                                                        kHfsForkDataBytes;
constexpr qsizetype kHfsCatalogFolderRecordBytes = 88;
constexpr qsizetype kHfsCatalogFileRecordBytes = 248;
constexpr qsizetype kHfsCatalogMinimumKeyBytes = 6;
constexpr qsizetype kHfsCatalogMaximumKeyBytes = 516;
constexpr qsizetype kHfsCatalogKeyParentIdOffset = 2;
constexpr qsizetype kHfsCatalogKeyNameLengthOffset = 6;
constexpr qsizetype kHfsCatalogKeyNameOffset = 8;
constexpr qsizetype kHfsCatalogThreadParentIdOffset = 4;
constexpr qsizetype kHfsCatalogThreadNameLengthOffset = 8;
constexpr qsizetype kHfsCatalogThreadNameOffset = 10;
constexpr qsizetype kHfsCatalogThreadRecordBytes = kHfsCatalogThreadNameOffset +
                                                   kHfsMaximumNameChars * kUint16Size;
constexpr qsizetype kHfsExtentsKeyLength = 10;
constexpr qsizetype kHfsExtentsKeyForkTypeOffset = 2;
constexpr qsizetype kHfsExtentsKeyFileIdOffset = 4;
constexpr qsizetype kHfsExtentsKeyStartBlockOffset = 8;
constexpr qsizetype kHfsExtentsRecordBytes = kHfsExtentBytes * kHfsExtentCount;
constexpr qsizetype kHfsAttributeMinimumKeyBytes = 12;
constexpr qsizetype kHfsAttributeMaximumKeyBytes = 516;
constexpr qsizetype kHfsAttributeKeyFileIdOffset = 4;
constexpr qsizetype kHfsAttributeKeyStartBlockOffset = 8;
constexpr qsizetype kHfsAttributeKeyNameLengthOffset = 12;
constexpr qsizetype kHfsAttributeKeyNameOffset = 14;
constexpr qsizetype kHfsAttributeRecordTypeBytes = 4;
constexpr qsizetype kHfsAttributeInlineReservedBytes = 8;
constexpr qsizetype kHfsAttributeInlineSizeOffset = kHfsAttributeRecordTypeBytes +
                                                    kHfsAttributeInlineReservedBytes;
constexpr qsizetype kHfsAttributeInlineDataOffset = kHfsAttributeInlineSizeOffset + kUint32Size;
constexpr qsizetype kHfsAttributeForkDataOffset = kHfsAttributeRecordTypeBytes + kUint32Size;
constexpr uint32_t kHfsAttributeInlineDataRecord = 0x10;
constexpr uint32_t kHfsAttributeForkDataRecord = 0x20;
constexpr uint32_t kHfsAttributeExtentsRecord = 0x30;
constexpr qsizetype kHfsRecordAlignmentBytes = 2;
constexpr qsizetype kHfsNodeOffsetTableReservedEntries = 2;
constexpr qsizetype kBTreeNodeDescriptorBytes = 14;
constexpr qsizetype kBTreeKindOffset = 8;
constexpr qsizetype kBTreeHeightOffset = 9;
constexpr qsizetype kBTreeNumRecordsOffset = 10;
constexpr int8_t kBTreeLeafNode = -1;
constexpr int8_t kBTreeIndexNode = 0;
constexpr int8_t kBTreeHeaderNode = 1;
constexpr qsizetype kBTreeNodeForwardLinkOffset = 0;
constexpr qsizetype kBTreeNodeBackwardLinkOffset = 4;
constexpr qsizetype kBTreeHeaderRecordOffset = 14;
constexpr qsizetype kBTreeHeaderTreeDepthOffset = 0;
constexpr qsizetype kBTreeHeaderRootNodeOffset = 2;
constexpr qsizetype kBTreeHeaderLeafRecordsOffset = 6;
constexpr qsizetype kBTreeHeaderFirstLeafNodeOffset = 10;
constexpr qsizetype kBTreeHeaderLastLeafNodeOffset = 14;
constexpr qsizetype kBTreeHeaderNodeSizeOffset = 18;
constexpr qsizetype kBTreeHeaderMaxKeyLengthOffset = 20;
constexpr qsizetype kBTreeHeaderTotalNodesOffset = 22;
constexpr qsizetype kBTreeHeaderFreeNodesOffset = 26;
constexpr qsizetype kBTreeHeaderKeyCompareTypeOffset = 37;
constexpr qsizetype kBTreeHeaderAttributesOffset = 38;
constexpr qsizetype kBTreeHeaderMinimumReadBytes = 64;
constexpr int kBTreeHeaderNodeRecordCount = 3;
constexpr int kBTreeHeaderMapRecordIndex = 2;
constexpr uint32_t kBTreeBigKeysMask = 0x00'00'00'02;
constexpr uint32_t kBTreeVariableIndexKeysMask = 0x00'00'00'04;
constexpr int kHfsRootLeafSplitNodesNeeded = 3;
constexpr int kHfsRootLeafSplitFreeNodeDelta = -2;
// Worst-case extra B-tree nodes a single edit can consume: one new node per
// tree level that splits plus a new root, so reserve tree_depth + this margin.
constexpr uint32_t kHfsNodePoolGrowthReserve = 2;
constexpr uint8_t kHfsLeafNodeHeight = 1;
constexpr uint8_t kHfsRootIndexNodeHeight = 2;
constexpr uint16_t kHfsRootLeafSplitTreeDepth = 2;
constexpr uint8_t kHfsBinaryCompare = 0xBC;
constexpr uint32_t kMinimumHfsBlockSize = 512;
constexpr uint32_t kMaximumHfsBlockSize = 128U * 1024U * 1024U;
constexpr uint32_t kMinimumHfsBTreeNodeSize = 512;
constexpr uint32_t kMinimumHfsCatalogNodeSize = 4096;
constexpr uint32_t kMaximumHfsCatalogNodeSize = 32'768;
constexpr uint64_t kMaxForkReadBytes = 512ULL * 1024ULL * 1024ULL;
constexpr int kDefaultPathResolveEntryLimit = 100'000;
constexpr int kMaxCatalogLeafNodesToScan = 200'000;
constexpr int kExportNameFallbackBase = 10;
constexpr qsizetype kExportNameMaxCharacters = 180;
constexpr int kFirstExportNameCollisionIndex = 2;
constexpr int kMaxExportNameCollisionAttempts = 1000;
constexpr uint32_t kHfsMinimumUserCatalogId = 16;
constexpr int kHfsEmptyFileCreateRecordDelta = 2;
constexpr int kHfsEmptyFileCreateFileCountDelta = 1;
constexpr int kHfsEmptyFileDeleteFileCountDelta = -1;
constexpr int kHfsSingleCatalogRecordRemoved = 1;
constexpr int kHfsFileAndThreadCatalogRecordsRemoved = 2;
constexpr int kHfsEmptyFolderCreateFolderCountDelta = 1;
constexpr int kHfsEmptyFolderDeleteFolderCountDelta = -1;
constexpr uint64_t kHfsAllocationBitmapScanChunkBytes = 64ULL * 1024ULL;
constexpr uint64_t kHfsSecureWipeChunkBytes = 1024ULL * 1024ULL;

struct HfsExtent {
    uint32_t start_block{0};
    uint32_t block_count{0};
};

struct HfsForkData {
    uint64_t logical_size{0};
    uint32_t total_blocks{0};
    QVector<HfsExtent> extents;
};

struct HfsVolume {
    QString file_system;
    uint64_t volume_offset{0};
    uint32_t file_count{0};
    uint32_t folder_count{0};
    uint32_t next_catalog_id{0};
    uint32_t block_size{0};
    uint32_t total_blocks{0};
    uint32_t free_blocks{0};
    uint32_t attributes{0};
    bool wrapped{false};
    HfsForkData allocation_fork;
    HfsForkData extents_fork;
    HfsForkData catalog_fork;
    HfsForkData attributes_fork;
};

struct HfsBTreeHeader {
    uint16_t tree_depth{0};
    uint32_t root_node{0};
    uint32_t leaf_records{0};
    uint32_t first_leaf_node{0};
    uint32_t last_leaf_node{0};
    uint16_t node_size{0};
    uint16_t max_key_length{0};
    uint32_t total_nodes{0};
    uint32_t free_nodes{0};
    uint8_t key_compare_type{0};
    uint32_t attributes_mask{0};
};

struct HfsCatalogRecord {
    uint32_t parent_id{0};
    QString name;
    uint16_t record_type{0};
    uint32_t catalog_id{0};
    uint64_t catalog_data_offset{0};
    uint64_t data_size{0};
    uint64_t resource_size{0};
    HfsForkData data_fork;
    HfsForkData resource_fork;
    // FileInfo.userInfo fdType @48 / fdCreator @52 and HFSPlusBSDInfo.special @44.
    // On a hard-link alias these carry 'hlnk' / 'hfs+' and the inode number; on a
    // symlink fdType is 'slnk'. Zero for folders/threads (only filled for files).
    uint32_t file_type{0};
    uint32_t file_creator{0};
    uint32_t bsd_special{0};

    [[nodiscard]] bool directory() const noexcept { return record_type == kHfsCatalogFolderRecord; }

    [[nodiscard]] bool regularFile() const noexcept { return record_type == kHfsCatalogFileRecord; }

    // A hard-link alias is a 'hlnk'/'hfs+' file record whose special @44 is the
    // iNodeNum of the real data in the `␄␄␄␄HFS+ Private Data` metadata directory.
    [[nodiscard]] bool hardLinkAlias() const noexcept {
        return regularFile() && file_type == kHfsHardLinkFileType &&
               file_creator == kHfsHardLinkFileCreator;
    }

    [[nodiscard]] bool symlink() const noexcept {
        return regularFile() && file_type == kHfsSymlinkFileType &&
               file_creator == kHfsSymlinkFileCreator;
    }
};

struct HfsRawCatalogRecord {
    uint32_t parent_id{0};
    QString name;
    uint16_t record_type{0};
    uint32_t catalog_id{0};
    QByteArray bytes;
};

struct HfsCatalogKeyInfo {
    uint32_t parent_id{0};
    QString name;
    qsizetype data_offset{0};
    uint64_t catalog_data_offset{0};
};

struct HfsOverflowExtentRecord {
    uint8_t fork_type{0};
    uint32_t file_id{0};
    uint32_t start_block{0};
    QVector<HfsExtent> extents;
};

struct HfsCatalogScanSummary {
    int records{0};
    int directories{0};
    int files{0};
    int threads{0};
    int other_records{0};
    int invalid_records{0};
    int leaf_nodes{0};
};

struct HfsAttributeRecord {
    uint32_t file_id{0};
    uint32_t start_block{0};
    uint64_t attribute_data_offset{0};
    uint32_t record_type{0};
    QString name;
    bool payload_complete{true};
    bool has_inline_data{false};
    uint32_t inline_size{0};
    uint64_t inline_size_offset{0};
    uint64_t inline_data_offset{0};
    qsizetype inline_available_bytes{0};
    QByteArray inline_data;
    bool has_fork_data{false};
    HfsForkData fork_data;
    uint64_t fork_logical_size{0};
    uint32_t fork_total_blocks{0};
    int fork_extent_count{0};
    bool has_extent_data{false};
    int extent_count{0};
    QVector<HfsExtent> extents;
};

struct HfsAttributeLeafContext {
    const QByteArray& node;
    const QVector<qsizetype>& offsets;
    uint16_t node_size{0};
    uint32_t node_number{0};
};

struct HfsAttributeRecordBounds {
    qsizetype offset{0};
    qsizetype end{0};
    uint16_t node_size{0};
    uint32_t node_number{0};
};

struct HfsAttributeKeyInfo {
    uint32_t file_id{0};
    uint32_t start_block{0};
    qsizetype data_offset{0};
    QString name;
};

struct HfsAttributeScanSummary {
    bool present{false};
    int records{0};
    int inline_records{0};
    int fork_records{0};
    int extent_records{0};
    int other_records{0};
    int leaf_nodes{0};
    QStringList names;
    QStringList metadata;
    QVector<HfsAttributeRecord> parsed_records;
};

struct HfsForkAttributeWriteSelection {
    HfsAttributeRecord record;
    QVector<HfsAttributeRecord> all_records;
};

struct HfsForkReadChunk {
    QByteArray bytes;
    uint64_t size{0};
};

bool hasBytes(const QByteArray& bytes, qsizetype offset, qsizetype length) {
    return offset >= 0 && length >= 0 && offset <= bytes.size() && bytes.size() - offset >= length;
}

bool matchesBytes(const QByteArray& bytes, qsizetype offset, const char* value, qsizetype length) {
    return hasBytes(bytes, offset, length) &&
           std::equal(value, value + length, bytes.constData() + offset);
}

uint16_t be16(const QByteArray& bytes, qsizetype offset) {
    if (!hasBytes(bytes, offset, kUint16Size)) {
        return 0;
    }
    return qFromBigEndian<uint16_t>(bytes.constData() + offset);
}

uint32_t be32(const QByteArray& bytes, qsizetype offset) {
    if (!hasBytes(bytes, offset, kUint32Size)) {
        return 0;
    }
    return qFromBigEndian<uint32_t>(bytes.constData() + offset);
}

QByteArray be32Bytes(uint32_t value) {
    QByteArray bytes(kUint32Size, '\0');
    qToBigEndian<uint32_t>(value, bytes.data());
    return bytes;
}

void writeBe16(QByteArray* bytes, qsizetype offset, uint16_t value) {
    if (!bytes || !hasBytes(*bytes, offset, kUint16Size)) {
        return;
    }
    qToBigEndian<uint16_t>(value, bytes->data() + offset);
}

void writeBe32(QByteArray* bytes, qsizetype offset, uint32_t value) {
    if (!bytes || !hasBytes(*bytes, offset, kUint32Size)) {
        return;
    }
    qToBigEndian<uint32_t>(value, bytes->data() + offset);
}

uint64_t be64(const QByteArray& bytes, qsizetype offset) {
    if (!hasBytes(bytes, offset, kUint64Size)) {
        return 0;
    }
    return qFromBigEndian<uint64_t>(bytes.constData() + offset);
}

QByteArray be64Bytes(uint64_t value) {
    QByteArray bytes(kUint64Size, '\0');
    qToBigEndian<uint64_t>(value, bytes.data());
    return bytes;
}

bool checkedAdd(uint64_t left, uint64_t right, uint64_t* output) {
    if (!output || right > std::numeric_limits<uint64_t>::max() - left) {
        return false;
    }
    *output = left + right;
    return true;
}

bool checkedMul(uint64_t left, uint64_t right, uint64_t* output) {
    if (!output || (left != 0 && right > std::numeric_limits<uint64_t>::max() / left)) {
        return false;
    }
    *output = left * right;
    return true;
}

bool isPowerOfTwo(uint32_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

QString hfsFamilyName(const QByteArray& bytes, qsizetype offset) {
    if (matchesBytes(bytes, offset, "H+", kHfsSignatureSize)) {
        return QStringLiteral("HFS+");
    }
    if (matchesBytes(bytes, offset, "HX", kHfsSignatureSize)) {
        return QStringLiteral("HFSX");
    }
    return {};
}

uint64_t extentCoverageBytes(const HfsForkData& fork, uint32_t blockSize) {
    uint64_t blocks = 0;
    for (const auto& extent : fork.extents) {
        blocks += extent.block_count;
    }
    uint64_t bytes = 0;
    return checkedMul(blocks, blockSize, &bytes) ? bytes : 0;
}

QStringList hfsPathParts(const QString& input, QStringList* blockers) {
    const QString path = input.trimmed().isEmpty() ? QStringLiteral("/") : input.trimmed();
    if (path.contains(QChar::Null) || path.contains(QLatin1Char('\\'))) {
        blockers->append(
            QStringLiteral("HFS+ path contains unsupported control or backslash text"));
        return {};
    }

    QStringList parts;
    for (const auto& part : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part == QStringLiteral(".")) {
            continue;
        }
        if (part == QStringLiteral("..")) {
            blockers->append(QStringLiteral("HFS+ path traversal is not allowed"));
            return {};
        }
        parts.append(part);
    }
    return parts;
}

QString makeEntryPath(const QString& parent, const QString& name) {
    if (parent.trimmed().isEmpty() || parent == QStringLiteral("/")) {
        return QStringLiteral("/%1").arg(name);
    }
    return QStringLiteral("%1/%2").arg(parent, name);
}

QString typeNameForRecord(uint16_t recordType) {
    if (recordType == kHfsCatalogFolderRecord) {
        return QStringLiteral("Directory");
    }
    if (recordType == kHfsCatalogFileRecord) {
        return QStringLiteral("File");
    }
    if (recordType == kHfsCatalogFolderThreadRecord || recordType == kHfsCatalogFileThreadRecord) {
        return QStringLiteral("Thread");
    }
    return QStringLiteral("Other");
}

QByteArray hfsCatalogKeyBytes(uint32_t parentId, const QString& name) {
    const uint16_t keyLength = static_cast<uint16_t>(
        kHfsCatalogMinimumKeyBytes + static_cast<qsizetype>(name.size()) * kUint16Size);
    QByteArray key(kUint16Size + keyLength, '\0');
    writeBe16(&key, 0, keyLength);
    writeBe32(&key, kHfsCatalogKeyParentIdOffset, parentId);
    writeBe16(&key, kHfsCatalogKeyNameLengthOffset, static_cast<uint16_t>(name.size()));
    for (qsizetype index = 0; index < name.size(); ++index) {
        writeBe16(&key, kHfsCatalogKeyNameOffset + index * kUint16Size, name.at(index).unicode());
    }
    return key;
}

uint32_t hfsCurrentTimestamp() {
    return static_cast<uint32_t>(QDateTime::currentSecsSinceEpoch() + kHfsToUnixEpochSeconds);
}

// Stamps the HFS+ catalog dates and HFSPlusBSDInfo (owner root:wheel, mode) that
// macOS expects; without these a created entry shows _unknown owner and a 1904/1970
// epoch date and is not production-clean even though fsck_hfs tolerates it.
void hfsStampCommonCatalogFields(QByteArray* record, uint16_t fileMode) {
    const uint32_t now = hfsCurrentTimestamp();
    writeBe32(record, kHfsCatalogCreateDateOffset, now);
    writeBe32(record, kHfsCatalogContentModDateOffset, now);
    writeBe32(record, kHfsCatalogAttributeModDateOffset, now);
    writeBe32(record, kHfsCatalogAccessDateOffset, now);
    writeBe32(record, kHfsCatalogBsdOwnerIdOffset, 0);
    writeBe32(record, kHfsCatalogBsdGroupIdOffset, 0);
    writeBe16(record, kHfsCatalogBsdFileModeOffset, fileMode);
}

QByteArray hfsCatalogEmptyFileRecord(uint32_t fileId) {
    QByteArray record(kHfsCatalogFileRecordBytes, '\0');
    writeBe16(&record, kHfsCatalogRecordTypeOffset, kHfsCatalogFileRecord);
    writeBe32(&record, kHfsCatalogRecordIdOffset, fileId);
    hfsStampCommonCatalogFields(&record, kHfsFileModeRegular);
    return record;
}

QByteArray hfsForkDataRecordBytes(const HfsForkData& fork) {
    QByteArray bytes(kHfsForkDataBytes, '\0');
    const QByteArray logicalSize = be64Bytes(fork.logical_size);
    bytes.replace(kHfsForkLogicalSizeOffset, logicalSize.size(), logicalSize);
    writeBe32(&bytes, kHfsForkTotalBlocksOffset, fork.total_blocks);
    const int count = std::min<int>(fork.extents.size(), kHfsExtentCount);
    for (int index = 0; index < count; ++index) {
        const qsizetype offset = kHfsForkExtentsOffset + index * kHfsExtentBytes;
        writeBe32(&bytes, offset + kHfsExtentStartBlockOffset, fork.extents.at(index).start_block);
        writeBe32(&bytes, offset + kHfsExtentBlockCountOffset, fork.extents.at(index).block_count);
    }
    return bytes;
}

QByteArray hfsCatalogFileRecordWithDataFork(uint32_t fileId, const HfsForkData& fork) {
    QByteArray record = hfsCatalogEmptyFileRecord(fileId);
    const QByteArray forkBytes = hfsForkDataRecordBytes(fork);
    record.replace(kHfsCatalogFileDataForkOffset, forkBytes.size(), forkBytes);
    return record;
}

QByteArray hfsCatalogEmptyFolderRecord(uint32_t folderId) {
    QByteArray record(kHfsCatalogFolderRecordBytes, '\0');
    writeBe16(&record, kHfsCatalogRecordTypeOffset, kHfsCatalogFolderRecord);
    writeBe32(&record, kHfsCatalogRecordIdOffset, folderId);
    hfsStampCommonCatalogFields(&record, kHfsFileModeDirectory);
    return record;
}

// H5: the metadata directory `\0\0\0\0HFS+ Private Data` that stores hard-link
// inodes. QStringLiteral truncates at the first NUL, so build it from QChars.
QString hfsPrivateDataDirName() {
    QString name(4, QChar(u'\0'));
    name += QStringLiteral("HFS+ Private Data");
    return name;
}

// H5: a symlink catalog file record (S_IFLNK, type/creator 'slnk'/'rhap'); the data
// fork holds the target path string. Byte layout harvested from macOS Sequoia.
QByteArray hfsCatalogSymlinkRecord(uint32_t fileId, const HfsForkData& fork) {
    QByteArray record(kHfsCatalogFileRecordBytes, '\0');
    writeBe16(&record, kHfsCatalogRecordTypeOffset, kHfsCatalogFileRecord);
    writeBe16(&record, kHfsCatalogRecordFlagsOffset, kHfsCatalogThreadExistsMask);
    writeBe32(&record, kHfsCatalogRecordIdOffset, fileId);
    hfsStampCommonCatalogFields(&record, kHfsFileModeSymlink);
    writeBe32(&record, kHfsCatalogBsdSpecialOffset, 1);
    writeBe32(&record, kHfsCatalogFileTypeOffset, kHfsSymlinkFileType);
    writeBe32(&record, kHfsCatalogFileCreatorOffset, kHfsSymlinkFileCreator);
    const QByteArray forkBytes = hfsForkDataRecordBytes(fork);
    record.replace(kHfsCatalogFileDataForkOffset, forkBytes.size(), forkBytes);
    return record;
}

// H5: turn an existing regular-file catalog payload (248 bytes) into a hard-link
// inode in place: keep its mode, type/creator, dates, and fork data (so the inode
// owns the original blocks), and only set the link-chain flag, hl_firstLinkID @4,
// and the BSDInfo.special @44 = linkCount. Byte layout harvested from macOS Sequoia.
void hfsStampHardLinkInodeFields(QByteArray* payload, uint32_t firstLinkId, uint32_t linkCount) {
    writeBe16(payload,
              kHfsCatalogRecordFlagsOffset,
              kHfsCatalogThreadExistsMask | kHfsCatalogHasLinkChainMask);
    writeBe32(payload, kHfsCatalogFirstLinkIdOffset, firstLinkId);
    writeBe32(payload, kHfsCatalogBsdSpecialOffset, linkCount);
}

// H5: a hard-link alias catalog file record (the user-visible name). The BSDInfo
// owner/group fields @32/@36 are repurposed as hl_prevLinkID / hl_nextLinkID, the
// special @44 = the inode's CNID (iNodeNum), type/creator 'hlnk'/'hfs+'; forks empty.
QByteArray hfsCatalogHardLinkAliasRecord(uint32_t aliasId,
                                         uint32_t inodeId,
                                         uint16_t inodeFileMode,
                                         uint32_t prevLinkId,
                                         uint32_t nextLinkId) {
    QByteArray record(kHfsCatalogFileRecordBytes, '\0');
    writeBe16(&record, kHfsCatalogRecordTypeOffset, kHfsCatalogFileRecord);
    writeBe16(&record,
              kHfsCatalogRecordFlagsOffset,
              kHfsCatalogThreadExistsMask | kHfsCatalogHasLinkChainMask);
    writeBe32(&record, kHfsCatalogRecordIdOffset, aliasId);
    const uint32_t now = hfsCurrentTimestamp();
    writeBe32(&record, kHfsCatalogCreateDateOffset, now);
    writeBe32(&record, kHfsCatalogContentModDateOffset, now);
    writeBe32(&record, kHfsCatalogAttributeModDateOffset, now);
    writeBe32(&record, kHfsCatalogAccessDateOffset, now);
    writeBe32(&record, kHfsCatalogBsdOwnerIdOffset, prevLinkId);
    writeBe32(&record, kHfsCatalogBsdGroupIdOffset, nextLinkId);
    record[kHfsCatalogBsdOwnerFlagsOffset] = static_cast<char>(kHfsHardLinkAliasOwnerFlags);
    writeBe16(&record,
              kHfsCatalogBsdFileModeOffset,
              static_cast<uint16_t>(inodeFileMode & ~kHfsFileModeWriteBits));
    writeBe32(&record, kHfsCatalogBsdSpecialOffset, inodeId);
    writeBe32(&record, kHfsCatalogFileTypeOffset, kHfsHardLinkFileType);
    writeBe32(&record, kHfsCatalogFileCreatorOffset, kHfsHardLinkFileCreator);
    writeBe16(&record, kHfsCatalogFileFinderFlagsOffset, kHfsHardLinkAliasFinderFlags);
    return record;
}

// H5: the private metadata directory's folder record (mode 0000, invisible, with the
// FolderInfo magic macOS stamps so Finder hides it). valence = inode count.
QByteArray hfsCatalogPrivateDirRecord(uint32_t folderId, uint32_t valence) {
    QByteArray record(kHfsCatalogFolderRecordBytes, '\0');
    writeBe16(&record, kHfsCatalogRecordTypeOffset, kHfsCatalogFolderRecord);
    writeBe16(&record, kHfsCatalogRecordFlagsOffset, kHfsPrivateDirFlags);
    writeBe32(&record, kHfsCatalogFolderValenceOffset, valence);
    writeBe32(&record, kHfsCatalogRecordIdOffset, folderId);
    const uint32_t now = hfsCurrentTimestamp();
    writeBe32(&record, kHfsCatalogCreateDateOffset, now);
    writeBe16(&record, kHfsCatalogBsdFileModeOffset, kHfsFileModePrivateDir);
    writeBe32(&record, kHfsCatalogBsdSpecialOffset, 1);
    // FolderInfo @48: frRect zero, frFlags @56 = 0x5000, frLocation @58 = {0x4000,0x4000}.
    writeBe16(&record, kHfsCatalogFolderInfoOffset + 8, kHfsPrivateDirFinderFlags);
    writeBe16(&record, kHfsCatalogFolderInfoOffset + 10, kHfsPrivateDirFinderLocV);
    writeBe16(&record, kHfsCatalogFolderInfoOffset + 12, kHfsPrivateDirFinderLocV);
    return record;
}

QByteArray hfsCatalogThreadRecord(uint16_t recordType, uint32_t parentId, const QString& name) {
    QByteArray record(kHfsCatalogThreadNameOffset + name.size() * kUint16Size, '\0');
    writeBe16(&record, kHfsCatalogRecordTypeOffset, recordType);
    writeBe32(&record, kHfsCatalogThreadParentIdOffset, parentId);
    writeBe16(&record, kHfsCatalogThreadNameLengthOffset, static_cast<uint16_t>(name.size()));
    for (qsizetype index = 0; index < name.size(); ++index) {
        writeBe16(&record,
                  kHfsCatalogThreadNameOffset + index * kUint16Size,
                  name.at(index).unicode());
    }
    return record;
}

QByteArray hfsCatalogFileThreadRecord(uint32_t parentId, const QString& name) {
    return hfsCatalogThreadRecord(kHfsCatalogFileThreadRecord, parentId, name);
}

QByteArray hfsCatalogFolderThreadRecord(uint32_t parentId, const QString& name) {
    return hfsCatalogThreadRecord(kHfsCatalogFolderThreadRecord, parentId, name);
}

QByteArray hfsCatalogRecordBytes(uint32_t parentId,
                                 const QString& name,
                                 const QByteArray& payload) {
    QByteArray record = hfsCatalogKeyBytes(parentId, name);
    record.append(payload);
    if ((record.size() % kHfsRecordAlignmentBytes) != 0) {
        record.append('\0');
    }
    return record;
}

// ---- HFS+ decmpfs (AppleFSCompression) codec: types 3 (inline zlib/raw) and
// ---- 4 (resource-fork chunked zlib). Layout follows Apple's on-disk format as
// ---- documented by the open-source afsctool implementation.
constexpr uint32_t kHfsDecmpfsMagic = 0x63'6D'70'66;  // 'cmpf', stored little-endian
constexpr uint32_t kHfsDecmpfsTypeZlibInline = 3;
constexpr uint32_t kHfsDecmpfsTypeZlibResource = 4;
constexpr uint32_t kHfsDecmpfsTypeLzvnInline = 7;
constexpr uint32_t kHfsDecmpfsTypeLzvnResource = 8;
constexpr uint32_t kHfsDecmpfsTypeLzfseInline = 11;
constexpr uint32_t kHfsDecmpfsTypeLzfseResource = 12;
constexpr char kHfsDecmpfsLzvnRawChunkMarker = 0x06;
constexpr qsizetype kHfsDecmpfsLzfseEncodeSlackBytes = 64;
constexpr qsizetype kHfsDecmpfsChunkTableMinimumBytes = kUint32Size * 2;
constexpr qsizetype kHfsDecmpfsHeaderBytes = 16;
constexpr qsizetype kHfsDecmpfsTypeOffset = 4;
constexpr qsizetype kHfsDecmpfsSizeOffset = 8;
constexpr uint32_t kHfsDecmpfsChunkBytes = 0x10000;
constexpr qsizetype kHfsDecmpfsResourceHeaderBytes = 0x100;
constexpr qsizetype kHfsDecmpfsResourceBlockLenOffset = 0x100;
constexpr qsizetype kHfsDecmpfsResourceTableOffset = 0x104;
constexpr qsizetype kHfsDecmpfsResourceTrailerBytes = 50;
constexpr qsizetype kHfsDecmpfsRsrcMapOffsetFieldOffset = 4;
constexpr qsizetype kHfsDecmpfsRsrcDataLengthFieldOffset = 8;
constexpr qsizetype kHfsDecmpfsRsrcMapLengthFieldOffset = 12;
constexpr qsizetype kHfsDecmpfsChunkEntryBytes = 8;
constexpr qsizetype kHfsDecmpfsTrailerZeroBytes = 24;
constexpr qsizetype kHfsDecmpfsTrailerMagic2Offset = 26;
constexpr qsizetype kHfsDecmpfsTrailerCmpfOffset = 30;
constexpr qsizetype kHfsDecmpfsTrailerMagic3Offset = 34;
constexpr qsizetype kHfsDecmpfsTrailerMagic4Offset = 38;
constexpr uint16_t kHfsDecmpfsTrailerMagic1 = 0x1C;
constexpr uint16_t kHfsDecmpfsTrailerMagic2 = 0x32;
constexpr uint32_t kHfsDecmpfsTrailerMagic3 = 0x0A;
constexpr uint64_t kHfsDecmpfsTrailerMagic4 = 0xFF'FF'01'00ULL;
constexpr uint32_t kHfsDecmpfsMaxChunkCount = 0x10000;
constexpr uint8_t kHfsDecmpfsRawChunkMarker = 0xFF;
constexpr uint64_t kHfsDecmpfsMaxSupportedBytes = (1ULL << 31) - 1;
constexpr int kHfsRootLeafSplitMinimumRecords = 2;
constexpr int kHfsRootLeafSplitHalves = 2;
constexpr int kHfsSplitLeftNodeSlot = 0;
constexpr int kHfsSplitRightNodeSlot = 1;
constexpr int kHfsSplitIndexNodeSlot = 2;
// The catalog/extents/attributes B-tree mutation engine loads the whole tree into
// memory, edits leaf records, and rebuilds the index bottom-up; the load recursion
// and the bottom-up emit both handle arbitrary depth, so these caps are safety
// limits, not structural ones. They are sized to stay within a single header-node
// allocation map (allocateBTreeNodesFromHeaderMap fails closed past the map's bit
// capacity, ~node_size*8 nodes); map-node growth would lift them further.
constexpr int kHfsMaxMutationLeaves = 16'384;
constexpr uint16_t kHfsMaxMutationTreeDepth = 8;
// Upper bound on the doubling free-node growth target (withCatalogNodePoolGrowth);
// the whole index is rebuilt per mutation, so the pool can need ~index-node-count
// free nodes, but never more than a few times kHfsMaxMutationLeaves. Past this the
// mutation fails closed rather than grow the catalog file unboundedly.
constexpr uint32_t kHfsMaxNodePoolGrowthTarget = 1U << 19;
constexpr uint16_t kHfsCatalogHasAttributesMask = 0x0004;
constexpr uint64_t kHfsMinimumVolumeBytesForAlternateHeader = kHfsVolumeHeaderOffset * 2;
const char* const kHfsDecmpfsAttributeName = "com.apple.decmpfs";

uint16_t le16(const QByteArray& bytes, qsizetype offset) {
    if (!hasBytes(bytes, offset, kUint16Size)) {
        return 0;
    }
    return qFromLittleEndian<uint16_t>(bytes.constData() + offset);
}

uint32_t le32(const QByteArray& bytes, qsizetype offset) {
    if (!hasBytes(bytes, offset, kUint32Size)) {
        return 0;
    }
    return qFromLittleEndian<uint32_t>(bytes.constData() + offset);
}

uint64_t le64(const QByteArray& bytes, qsizetype offset) {
    if (!hasBytes(bytes, offset, kUint64Size)) {
        return 0;
    }
    return qFromLittleEndian<uint64_t>(bytes.constData() + offset);
}

void writeLe32(QByteArray* bytes, qsizetype offset, uint32_t value) {
    if (bytes && hasBytes(*bytes, offset, kUint32Size)) {
        qToLittleEndian(value, bytes->data() + offset);
    }
}

void writeLe64(QByteArray* bytes, qsizetype offset, uint64_t value) {
    if (bytes && hasBytes(*bytes, offset, kUint64Size)) {
        qToLittleEndian(value, bytes->data() + offset);
    }
}

struct HfsDecmpfsHeader {
    uint32_t compression_type{0};
    uint64_t uncompressed_size{0};
};

std::optional<HfsDecmpfsHeader> parseDecmpfsHeader(const QByteArray& attribute) {
    if (attribute.size() < kHfsDecmpfsHeaderBytes || le32(attribute, 0) != kHfsDecmpfsMagic) {
        return std::nullopt;
    }
    return HfsDecmpfsHeader{.compression_type = le32(attribute, kHfsDecmpfsTypeOffset),
                            .uncompressed_size = le64(attribute, kHfsDecmpfsSizeOffset)};
}

QByteArray decmpfsHeaderBytes(uint32_t compressionType, uint64_t uncompressedSize) {
    QByteArray header(kHfsDecmpfsHeaderBytes, '\0');
    writeLe32(&header, 0, kHfsDecmpfsMagic);
    writeLe32(&header, kHfsDecmpfsTypeOffset, compressionType);
    writeLe64(&header, kHfsDecmpfsSizeOffset, uncompressedSize);
    return header;
}

std::optional<QByteArray> zlibDecompressStream(const QByteArray& stream, uint32_t expectedBytes) {
    QByteArray prefixed;
    prefixed.reserve(stream.size() + static_cast<qsizetype>(kUint32Size));
    prefixed.append(be32Bytes(expectedBytes));
    prefixed.append(stream);
    const QByteArray decompressed = qUncompress(prefixed);
    if (decompressed.size() != static_cast<qsizetype>(expectedBytes)) {
        return std::nullopt;
    }
    return decompressed;
}

std::optional<QByteArray> decodeDecmpfsChunk(const QByteArray& chunk, uint32_t expectedBytes) {
    if (chunk.isEmpty()) {
        return std::nullopt;
    }
    if (static_cast<uint8_t>(chunk.at(0)) == kHfsDecmpfsRawChunkMarker) {
        const QByteArray raw = chunk.mid(1);
        if (raw.size() != static_cast<qsizetype>(expectedBytes)) {
            return std::nullopt;
        }
        return raw;
    }
    return zlibDecompressStream(chunk, expectedBytes);
}

QByteArray encodeDecmpfsChunk(const QByteArray& chunk) {
    const QByteArray compressed = qCompress(chunk, 9).mid(kUint32Size);
    if (compressed.size() >= chunk.size() + 1) {
        QByteArray raw;
        raw.reserve(chunk.size() + 1);
        raw.append(static_cast<char>(kHfsDecmpfsRawChunkMarker));
        raw.append(chunk);
        return raw;
    }
    return compressed;
}

// Vendored Apple lzfse/lzvn reference codecs (third_party/lzfse) via C shim.
extern "C" {
size_t sak_lzvn_encode(void* dst, size_t dst_size, const void* src, size_t src_size);
size_t sak_lzvn_decode(void* dst, size_t dst_size, const void* src, size_t src_size);
size_t sak_lzfse_encode(void* dst, size_t dst_size, const void* src, size_t src_size);
size_t sak_lzfse_decode(void* dst, size_t dst_size, const void* src, size_t src_size);
}

[[nodiscard]] bool decmpfsTypeUsesLzvn(uint32_t compressionType) {
    return compressionType == kHfsDecmpfsTypeLzvnInline ||
           compressionType == kHfsDecmpfsTypeLzvnResource;
}

[[nodiscard]] bool decmpfsTypeUsesLzfse(uint32_t compressionType) {
    return compressionType == kHfsDecmpfsTypeLzfseInline ||
           compressionType == kHfsDecmpfsTypeLzfseResource;
}

[[nodiscard]] bool decmpfsTypeUsesChunkedOffsetTable(uint32_t compressionType) {
    return compressionType == kHfsDecmpfsTypeLzvnResource ||
           compressionType == kHfsDecmpfsTypeLzfseResource;
}

std::optional<QByteArray> decodeLzvnChunk(const QByteArray& chunk, uint32_t expectedBytes) {
    if (chunk.isEmpty()) {
        return std::nullopt;
    }
    if (chunk.at(0) == kHfsDecmpfsLzvnRawChunkMarker) {
        const QByteArray raw = chunk.mid(1);
        if (raw.size() != static_cast<qsizetype>(expectedBytes)) {
            return std::nullopt;
        }
        return raw;
    }
    QByteArray output(static_cast<qsizetype>(expectedBytes), '\0');
    const size_t decoded = sak_lzvn_decode(output.data(),
                                           static_cast<size_t>(output.size()),
                                           chunk.constData(),
                                           static_cast<size_t>(chunk.size()));
    if (decoded != expectedBytes) {
        return std::nullopt;
    }
    return output;
}

QByteArray encodeLzvnChunk(const QByteArray& chunk) {
    QByteArray compressed(chunk.size() + 1, '\0');
    const size_t encoded = sak_lzvn_encode(compressed.data(),
                                           static_cast<size_t>(chunk.size()),
                                           chunk.constData(),
                                           static_cast<size_t>(chunk.size()));
    if (encoded == 0) {
        QByteArray raw;
        raw.reserve(chunk.size() + 1);
        raw.append(kHfsDecmpfsLzvnRawChunkMarker);
        raw.append(chunk);
        return raw;
    }
    compressed.truncate(static_cast<qsizetype>(encoded));
    return compressed;
}

std::optional<QByteArray> decodeLzfseChunk(const QByteArray& chunk, uint32_t expectedBytes) {
    if (chunk.isEmpty()) {
        return std::nullopt;
    }
    QByteArray output(static_cast<qsizetype>(expectedBytes), '\0');
    const size_t decoded = sak_lzfse_decode(output.data(),
                                            static_cast<size_t>(output.size()),
                                            chunk.constData(),
                                            static_cast<size_t>(chunk.size()));
    if (decoded != expectedBytes) {
        return std::nullopt;
    }
    return output;
}

QByteArray encodeLzfseChunk(const QByteArray& chunk) {
    // The lzfse container has its own uncompressed-block form, so the encoder
    // output is always a valid stream; size it generously for tiny inputs.
    QByteArray compressed(chunk.size() + kHfsDecmpfsLzfseEncodeSlackBytes, '\0');
    const size_t encoded = sak_lzfse_encode(compressed.data(),
                                            static_cast<size_t>(compressed.size()),
                                            chunk.constData(),
                                            static_cast<size_t>(chunk.size()));
    compressed.truncate(static_cast<qsizetype>(encoded));
    return compressed;
}

std::optional<QByteArray> decodeDecmpfsCodecChunk(uint32_t compressionType,
                                                  const QByteArray& chunk,
                                                  uint32_t expectedBytes) {
    if (decmpfsTypeUsesLzvn(compressionType)) {
        return decodeLzvnChunk(chunk, expectedBytes);
    }
    if (decmpfsTypeUsesLzfse(compressionType)) {
        return decodeLzfseChunk(chunk, expectedBytes);
    }
    return decodeDecmpfsChunk(chunk, expectedBytes);
}

QByteArray encodeDecmpfsCodecChunk(uint32_t compressionType, const QByteArray& chunk) {
    if (decmpfsTypeUsesLzvn(compressionType)) {
        return encodeLzvnChunk(chunk);
    }
    if (decmpfsTypeUsesLzfse(compressionType)) {
        return encodeLzfseChunk(chunk);
    }
    return encodeDecmpfsChunk(chunk);
}

QByteArray decmpfsResourceTrailerBytes() {
    QByteArray trailer(kHfsDecmpfsResourceTrailerBytes, '\0');
    writeBe16(&trailer, kHfsDecmpfsTrailerZeroBytes, kHfsDecmpfsTrailerMagic1);
    writeBe16(&trailer, kHfsDecmpfsTrailerMagic2Offset, kHfsDecmpfsTrailerMagic2);
    // 2-byte spacer stays zero.
    writeBe32(&trailer, kHfsDecmpfsTrailerCmpfOffset, kHfsDecmpfsMagic);
    writeBe32(&trailer, kHfsDecmpfsTrailerMagic3Offset, kHfsDecmpfsTrailerMagic3);
    qToLittleEndian<quint64>(kHfsDecmpfsTrailerMagic4,
                             trailer.data() + kHfsDecmpfsTrailerMagic4Offset);
    // 4-byte spacer stays zero.
    return trailer;
}

QByteArray buildDecmpfsResourceFork(const QByteArray& data) {
    const int chunkCount =
        std::max(1,
                 static_cast<int>((static_cast<uint64_t>(data.size()) + kHfsDecmpfsChunkBytes - 1) /
                                  kHfsDecmpfsChunkBytes));
    QVector<QByteArray> chunks;
    chunks.reserve(chunkCount);
    for (int index = 0; index < chunkCount; ++index) {
        chunks.append(encodeDecmpfsChunk(data.mid(
            static_cast<qsizetype>(index) * kHfsDecmpfsChunkBytes, kHfsDecmpfsChunkBytes)));
    }

    const qsizetype tableBytes = kUint32Size +
                                 static_cast<qsizetype>(chunkCount) * kHfsDecmpfsChunkEntryBytes;
    qsizetype chunkAreaBytes = 0;
    for (const auto& chunk : chunks) {
        chunkAreaBytes += chunk.size();
    }
    const qsizetype blockLen = tableBytes + chunkAreaBytes;
    const qsizetype mapOffset = kHfsDecmpfsResourceTableOffset + blockLen;

    QByteArray fork(mapOffset + kHfsDecmpfsResourceTrailerBytes, '\0');
    writeBe32(&fork, 0, static_cast<uint32_t>(kHfsDecmpfsResourceHeaderBytes));
    writeBe32(&fork, kHfsDecmpfsRsrcMapOffsetFieldOffset, static_cast<uint32_t>(mapOffset));
    writeBe32(&fork,
              kHfsDecmpfsRsrcDataLengthFieldOffset,
              static_cast<uint32_t>(mapOffset - kHfsDecmpfsResourceHeaderBytes));
    writeBe32(&fork,
              kHfsDecmpfsRsrcMapLengthFieldOffset,
              static_cast<uint32_t>(kHfsDecmpfsResourceTrailerBytes));
    writeBe32(&fork, kHfsDecmpfsResourceBlockLenOffset, static_cast<uint32_t>(blockLen));
    writeLe32(&fork, kHfsDecmpfsResourceTableOffset, static_cast<uint32_t>(chunkCount));

    qsizetype chunkCursor = tableBytes;
    for (int index = 0; index < chunkCount; ++index) {
        const qsizetype entryOffset = kHfsDecmpfsResourceTableOffset + kUint32Size +
                                      static_cast<qsizetype>(index) * kHfsDecmpfsChunkEntryBytes;
        writeLe32(&fork, entryOffset, static_cast<uint32_t>(chunkCursor));
        writeLe32(&fork, entryOffset + kUint32Size, static_cast<uint32_t>(chunks.at(index).size()));
        std::copy(chunks.at(index).cbegin(),
                  chunks.at(index).cend(),
                  fork.begin() + kHfsDecmpfsResourceTableOffset + chunkCursor);
        chunkCursor += chunks.at(index).size();
    }
    const QByteArray trailer = decmpfsResourceTrailerBytes();
    std::copy(trailer.cbegin(), trailer.cend(), fork.begin() + mapOffset);
    return fork;
}

void setDecmpfsDecodeError(QString* error, const QString& message) {
    if (error) {
        *error = message;
    }
}

// Types 8 and 12 use a bare little-endian offset table at the start of the
// resource fork: (chunkCount + 1) offsets, where entry 0 doubles as the table
// size and chunk i occupies [offset[i], offset[i + 1]).
QByteArray buildDecmpfsChunkedResourceFork(uint32_t compressionType, const QByteArray& data) {
    const int chunkCount =
        std::max(1,
                 static_cast<int>((static_cast<uint64_t>(data.size()) + kHfsDecmpfsChunkBytes - 1) /
                                  kHfsDecmpfsChunkBytes));
    QVector<QByteArray> chunks;
    chunks.reserve(chunkCount);
    for (int index = 0; index < chunkCount; ++index) {
        chunks.append(
            encodeDecmpfsCodecChunk(compressionType,
                                    data.mid(static_cast<qsizetype>(index) * kHfsDecmpfsChunkBytes,
                                             kHfsDecmpfsChunkBytes)));
    }
    const qsizetype tableBytes = static_cast<qsizetype>(chunkCount + 1) * kUint32Size;
    qsizetype total = tableBytes;
    for (const auto& chunk : chunks) {
        total += chunk.size();
    }
    QByteArray fork(total, '\0');
    qsizetype cursor = tableBytes;
    writeLe32(&fork, 0, static_cast<uint32_t>(tableBytes));
    for (int index = 0; index < chunkCount; ++index) {
        std::copy(chunks.at(index).cbegin(), chunks.at(index).cend(), fork.begin() + cursor);
        cursor += chunks.at(index).size();
        writeLe32(&fork,
                  static_cast<qsizetype>(index + 1) * kUint32Size,
                  static_cast<uint32_t>(cursor));
    }
    return fork;
}

std::optional<uint32_t> chunkedResourceForkChunkCount(const QByteArray& fork,
                                                      uint64_t uncompressedSize,
                                                      QString* error) {
    if (fork.size() < static_cast<qsizetype>(kUint32Size)) {
        setDecmpfsDecodeError(error, QStringLiteral("decmpfs resource fork is truncated"));
        return std::nullopt;
    }
    const uint32_t tableBytes = le32(fork, 0);
    const uint64_t expectedChunks = std::max<uint64_t>(
        1, (uncompressedSize + kHfsDecmpfsChunkBytes - 1) / kHfsDecmpfsChunkBytes);
    const uint32_t chunkCount = tableBytes / kUint32Size - 1;
    if (tableBytes % kUint32Size != 0 || tableBytes < kHfsDecmpfsChunkTableMinimumBytes ||
        static_cast<qsizetype>(tableBytes) > fork.size() || chunkCount > kHfsDecmpfsMaxChunkCount ||
        chunkCount != expectedChunks) {
        setDecmpfsDecodeError(error,
                              QStringLiteral("decmpfs resource fork offset table is invalid"));
        return std::nullopt;
    }
    return chunkCount;
}

std::optional<QByteArray> decodeDecmpfsChunkedResourceFork(uint32_t compressionType,
                                                           const QByteArray& fork,
                                                           uint64_t uncompressedSize,
                                                           QString* error) {
    const auto chunkCountChecked = chunkedResourceForkChunkCount(fork, uncompressedSize, error);
    if (!chunkCountChecked.has_value()) {
        return std::nullopt;
    }
    const uint32_t chunkCount = *chunkCountChecked;
    QByteArray output;
    output.reserve(static_cast<qsizetype>(uncompressedSize));
    uint64_t remaining = uncompressedSize;
    for (uint32_t index = 0; index < chunkCount; ++index) {
        const uint32_t start = le32(fork, static_cast<qsizetype>(index) * kUint32Size);
        const uint32_t end = le32(fork, static_cast<qsizetype>(index + 1) * kUint32Size);
        if (end < start || static_cast<qsizetype>(end) > fork.size()) {
            setDecmpfsDecodeError(
                error, QStringLiteral("decmpfs resource fork chunk entry is out of range"));
            return std::nullopt;
        }
        const uint32_t expectedBytes =
            static_cast<uint32_t>(std::min<uint64_t>(remaining, kHfsDecmpfsChunkBytes));
        const auto chunk = decodeDecmpfsCodecChunk(compressionType,
                                                   fork.mid(static_cast<qsizetype>(start),
                                                            static_cast<qsizetype>(end - start)),
                                                   expectedBytes);
        if (!chunk.has_value()) {
            setDecmpfsDecodeError(
                error, QStringLiteral("decmpfs resource fork chunk failed to decompress"));
            return std::nullopt;
        }
        output.append(*chunk);
        remaining -= expectedBytes;
    }
    if (remaining != 0) {
        setDecmpfsDecodeError(error,
                              QStringLiteral("decmpfs resource fork did not cover the full file"));
        return std::nullopt;
    }
    return output;
}

std::optional<QByteArray> decodeDecmpfsResourceChunkAt(const QByteArray& fork,
                                                       uint32_t index,
                                                       uint64_t remaining,
                                                       QString* error) {
    const qsizetype entryOffset = kHfsDecmpfsResourceTableOffset + kUint32Size +
                                  static_cast<qsizetype>(index) * kHfsDecmpfsChunkEntryBytes;
    const uint32_t chunkOffset = le32(fork, entryOffset);
    const uint32_t chunkSize = le32(fork, entryOffset + kUint32Size);
    const qsizetype absolute = kHfsDecmpfsResourceTableOffset + static_cast<qsizetype>(chunkOffset);
    if (chunkSize == 0 || !hasBytes(fork, absolute, static_cast<qsizetype>(chunkSize))) {
        setDecmpfsDecodeError(error,
                              QStringLiteral("decmpfs resource fork chunk entry is out of range"));
        return std::nullopt;
    }
    const uint32_t expectedBytes =
        static_cast<uint32_t>(std::min<uint64_t>(remaining, kHfsDecmpfsChunkBytes));
    const auto chunk = decodeDecmpfsChunk(fork.mid(absolute, static_cast<qsizetype>(chunkSize)),
                                          expectedBytes);
    if (!chunk.has_value()) {
        setDecmpfsDecodeError(error,
                              QStringLiteral("decmpfs resource fork chunk failed to decompress"));
        return std::nullopt;
    }
    return chunk;
}

std::optional<QByteArray> decodeDecmpfsResourceFork(const QByteArray& fork,
                                                    uint64_t uncompressedSize,
                                                    QString* error) {
    if (fork.size() < kHfsDecmpfsResourceTableOffset + static_cast<qsizetype>(kUint32Size)) {
        setDecmpfsDecodeError(error, QStringLiteral("decmpfs resource fork is truncated"));
        return std::nullopt;
    }
    if (be32(fork, 0) != static_cast<uint32_t>(kHfsDecmpfsResourceHeaderBytes)) {
        setDecmpfsDecodeError(error, QStringLiteral("decmpfs resource fork header is invalid"));
        return std::nullopt;
    }
    const uint32_t chunkCount = le32(fork, kHfsDecmpfsResourceTableOffset);
    const uint64_t expectedChunks = (uncompressedSize + kHfsDecmpfsChunkBytes - 1) /
                                    kHfsDecmpfsChunkBytes;
    if (chunkCount == 0 || chunkCount > kHfsDecmpfsMaxChunkCount ||
        (uncompressedSize > 0 && chunkCount != expectedChunks)) {
        setDecmpfsDecodeError(error,
                              QStringLiteral("decmpfs resource fork chunk table is invalid"));
        return std::nullopt;
    }
    QByteArray output;
    output.reserve(static_cast<qsizetype>(uncompressedSize));
    uint64_t remaining = uncompressedSize;
    for (uint32_t index = 0; index < chunkCount; ++index) {
        const auto chunk = decodeDecmpfsResourceChunkAt(fork, index, remaining, error);
        if (!chunk.has_value()) {
            return std::nullopt;
        }
        output.append(*chunk);
        remaining -= static_cast<uint32_t>(std::min<uint64_t>(remaining, kHfsDecmpfsChunkBytes));
    }
    if (remaining != 0) {
        setDecmpfsDecodeError(error,
                              QStringLiteral("decmpfs resource fork did not cover the full file"));
        return std::nullopt;
    }
    return output;
}

int compareCatalogNames(QString left, QString right, uint8_t keyCompareType) {
    if (keyCompareType == kHfsBinaryCompare) {
        // Case-sensitive HFSX volumes: raw code-unit comparison, no folding.
        const int common = std::min(left.size(), right.size());
        for (int index = 0; index < common; ++index) {
            const ushort leftChar = left.at(index).unicode();
            const ushort rightChar = right.at(index).unicode();
            if (leftChar != rightChar) {
                return leftChar < rightChar ? -1 : 1;
            }
        }
        if (left.size() == right.size()) {
            return 0;
        }
        return left.size() < right.size() ? -1 : 1;
    }
    // Standard case-folding HFS+ volume: defer to Apple's exact FastUnicodeCompare so a
    // rebuilt catalog collates bit-identically to the kernel and fsck_hfs.
    return fastUnicodeCompare(reinterpret_cast<const uint16_t*>(left.utf16()),
                              static_cast<int>(left.size()),
                              reinterpret_cast<const uint16_t*>(right.utf16()),
                              static_cast<int>(right.size()));
}

int compareCatalogKeys(uint32_t leftParent,
                       const QString& leftName,
                       uint32_t rightParent,
                       const QString& rightName,
                       uint8_t keyCompareType) {
    if (leftParent < rightParent) {
        return -1;
    }
    if (leftParent > rightParent) {
        return 1;
    }
    return compareCatalogNames(leftName, rightName, keyCompareType);
}

bool catalogKeysEqual(uint32_t leftParent,
                      const QString& leftName,
                      uint32_t rightParent,
                      const QString& rightName,
                      uint8_t keyCompareType) {
    return compareCatalogKeys(leftParent, leftName, rightParent, rightName, keyCompareType) == 0;
}

bool appendCatalogRecordToNode(QByteArray* node,
                               const QByteArray& record,
                               QVector<qsizetype>* offsets,
                               qsizetype* cursor) {
    if (!node || !offsets || !cursor) {
        return false;
    }
    const qsizetype nextCursor = *cursor + record.size();
    const qsizetype offsetTableStart =
        node->size() - ((offsets->size() + kHfsNodeOffsetTableReservedEntries) * kUint16Size);
    if (*cursor < kBTreeNodeDescriptorBytes || nextCursor > offsetTableStart) {
        return false;
    }
    offsets->append(*cursor);
    std::copy(record.cbegin(), record.cend(), node->begin() + *cursor);
    *cursor = nextCursor;
    return true;
}

void writeCatalogNodeOffsets(QByteArray* node,
                             const QVector<qsizetype>& offsets,
                             qsizetype freeOffset) {
    for (int index = 0; index < offsets.size(); ++index) {
        writeBe16(node,
                  node->size() - ((index + 1) * kUint16Size),
                  static_cast<uint16_t>(offsets.at(index)));
    }
    writeBe16(node,
              node->size() - ((offsets.size() + 1) * kUint16Size),
              static_cast<uint16_t>(freeOffset));
}


}  // namespace
}  // namespace sak

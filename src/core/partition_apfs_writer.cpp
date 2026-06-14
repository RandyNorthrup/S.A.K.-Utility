// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_apfs_writer.cpp
/// @brief Fail-closed APFS write preflight for future Partition Manager mutation support.

#include "sak/partition_apfs_writer.h"

#include "sak/partition_apfs_file_system_reader.h"
#include "sak/partition_raw_device_io.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QStringView>
#include <QtEndian>
#include <QUuid>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace sak {

namespace {

constexpr uint64_t kDefaultMaxApfsPayloadBytes = 64ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMinimumApfsContainerBytes = 64ULL * 1024ULL * 1024ULL;
constexpr uint32_t kSupportedApfsBlockSizeBytes = 4096;
constexpr qsizetype kMaximumApfsVolumeNameChars = 255;
constexpr qsizetype kMinimumKeyValuePartCount = 2;
constexpr qsizetype kApfsSnapshotCountPrefixLength = 10;
constexpr qsizetype kApfsObjectChecksumBytes = 8;
constexpr qsizetype kApfsChecksumWordBytes = 4;
constexpr uint64_t kFletcherModulo = 0xFF'FF'FF'FFULL;
constexpr uint64_t kFletcherModuloChunkWords = 1024ULL;
constexpr int kFletcherHighWordShift = 32;
// Block layout mirrored 1:1 from a container produced by Apple's newfs_apfs
// at the same 16384-block geometry (build/test_results/maclab/apple-fresh.img),
// including the genesis ghost checkpoint (xid 1) preceding the live
// checkpoint (xid 2) and the ghost objects the free queues schedule for reap.
constexpr uint64_t kApfsFormatNxsbBlock = 0;
constexpr uint64_t kApfsFormatGenesisXid = 1;
constexpr uint32_t kApfsFormatCheckpointDescBlocks = 8;
constexpr uint64_t kApfsFormatCheckpointDescBaseBlock = 1;
constexpr uint64_t kApfsFormatGenesisMapBlock = 1;
constexpr uint64_t kApfsFormatGenesisNxsbBlock = 2;
constexpr uint64_t kApfsFormatCheckpointMapBlock = 3;
constexpr uint64_t kApfsFormatCheckpointNxsbCopyBlock = 4;
constexpr uint64_t kApfsFormatCheckpointDataBaseBlock = 9;
constexpr uint32_t kApfsFormatCheckpointDataBlocks = 160;
constexpr uint64_t kApfsFormatGenesisSpacemanBlock = 9;
constexpr uint64_t kApfsFormatGenesisReaperBlock = 10;
constexpr uint64_t kApfsFormatSpacemanBlock = 11;
constexpr uint64_t kApfsFormatReaperBlock = 12;
constexpr uint64_t kApfsFormatFqIpTreeBlock = 13;
constexpr uint64_t kApfsFormatFqMainTreeBlock = 14;
constexpr uint32_t kApfsFormatIpBitmapBlocks = 16;
constexpr uint64_t kApfsFormatIpBitmapBaseBlock = 169;
constexpr uint64_t kApfsFormatIpBlockCount = 6;
constexpr uint64_t kApfsFormatIpBaseBlock = 185;
constexpr uint64_t kApfsFormatGhostChunkInfoBlock = 185;
constexpr uint64_t kApfsFormatGhostChunkBitmapBlock = 186;
constexpr uint64_t kApfsFormatChunkInfoBlock = 187;
constexpr uint64_t kApfsFormatChunkBitmapBlock = 188;
constexpr uint64_t kApfsFormatGhostContainerOmapBlock = 191;
constexpr uint64_t kApfsFormatGhostContainerOmapTreeBlock = 192;
constexpr uint64_t kApfsFormatVolumeOmapBlock = 193;
constexpr uint64_t kApfsFormatVolumeOmapTreeBlock = 194;
constexpr uint64_t kApfsFormatExtentRefTreeBlock = 195;
constexpr uint64_t kApfsFormatSnapMetaTreeBlock = 196;
constexpr uint64_t kApfsFormatRootTreeBlock = 197;
constexpr uint64_t kApfsFormatVolumeSuperblockBlock = 198;
constexpr uint64_t kApfsFormatContainerOmapBlock = 199;
constexpr uint64_t kApfsFormatContainerOmapTreeBlock = 200;
constexpr uint64_t kApfsFormatSeedFileDataBlock = 201;
// Blocks consumed by the genesis (xid 1) world: everything through the ghost
// container object-map tree.
constexpr uint64_t kApfsFormatGhostReservedBlocks = kApfsFormatGhostContainerOmapTreeBlock + 1;
// Blocks the volume itself accounts as allocated on a fresh container: its
// object-map tree, extent-ref tree, snapshot-metadata tree, root tree, and
// volume superblock.
constexpr uint64_t kApfsFormatVolumeBaseAllocatedBlocks = 5;
constexpr uint64_t kApfsFormatStaleSignatureClearBytes = 8ULL * 1024ULL * 1024ULL;
constexpr qsizetype kApfsFormatZeroChunkBytes = 1024 * 1024;
constexpr uint64_t kApfsMaximumSeedFileBytes = 8ULL * 1024ULL * 1024ULL;
constexpr qsizetype kApfsUuidBytes = 16;
constexpr int kApfsWriteRootListingMaxEntries = 1000;
constexpr int kApfsGeneratedRootRecordsPerFile = 3;
// APFS reserves dynamically assigned object IDs below OID_RESERVED_COUNT
// (1024); fsck_apfs rejects superblock OID fields inside the reserved range
// ("nx_reaper_oid (106) is less than minimum OID (1024)"). Physical objects
// instead carry their block address as the OID. The container superblock OID
// is the fixed OID_NX_SUPERBLOCK (1).
constexpr uint64_t kApfsFormatNxsbOid = 1;
constexpr uint64_t kApfsFormatContainerOmapOid = kApfsFormatContainerOmapBlock;
constexpr uint64_t kApfsFormatVolumeOmapOid = kApfsFormatVolumeOmapBlock;
constexpr uint64_t kApfsFormatSpacemanOid = 1024;
constexpr uint64_t kApfsFormatReaperOid = 1025;
constexpr uint64_t kApfsFormatVolumeOid = 1026;
constexpr uint64_t kApfsFormatFqIpTreeOid = 1027;
constexpr uint64_t kApfsFormatRootTreeOid = 1028;
constexpr uint64_t kApfsFormatFqMainTreeOid = 1029;
// Apple's newfs_apfs commits the genesis container at transaction 2 â€” a
// checkpoint at xid 1 fails fsck_apfs's consistency check.
constexpr uint64_t kApfsFormatXid = 2;
constexpr uint64_t kApfsNxMinimumNextOid = 1030;
constexpr uint64_t kApfsFormatGenesisNextOid = kApfsFormatVolumeOid;
constexpr uint64_t kApfsRootDirectoryId = 2;
// File-system object IDs follow Apple's numbering: the volume root directory
// is 2, the private directory is 3, and user files/directories count up from
// 16 (matching apfs_next_obj_id); each fresh object's private id equals its
// object id.
constexpr uint64_t kApfsSeedFileId = 16;
// APFS object-header o_type carries storage-class flags in its high bits
// (OBJ_PHYSICAL 0x40000000, OBJ_EPHEMERAL 0x80000000, OBJ_VIRTUAL 0). Apple's
// fsck_apfs rejects a container superblock whose o_type lacks these flags
// ("o_type 0x1 should be 0x80000001"), so every generated object embeds the
// storage class its role requires: the block-zero/checkpoint container
// superblock and the spaceman are ephemeral; the container and volume object
// maps and their B-trees are physical; the volume superblock and the
// omap-resolved root file-system tree are virtual.
constexpr uint32_t kApfsObjStoragePhysical = 0x40'00'00'00;
constexpr uint32_t kApfsObjStorageEphemeral = 0x80'00'00'00;
constexpr uint32_t kApfsObjectTypeNxSuperblock = kApfsObjStorageEphemeral | 0x00'00'00'01;
constexpr uint32_t kApfsObjectTypeBtree = 0x00'00'00'02;
constexpr uint32_t kApfsObjectTypeBtreePhysical = kApfsObjStoragePhysical | 0x00'00'00'02;
constexpr uint32_t kApfsObjectTypeObjectMap = kApfsObjStoragePhysical | 0x00'00'00'0B;
constexpr uint32_t kApfsObjectTypeFs = 0x00'00'00'0D;
constexpr uint32_t kApfsObjectTypeSpaceman = kApfsObjStorageEphemeral | 0x00'00'00'05;
constexpr uint32_t kApfsObjectTypeCheckpointMap = kApfsObjStoragePhysical | 0x00'00'00'0C;
constexpr uint32_t kApfsObjectTypeReaper = kApfsObjStorageEphemeral | 0x00'00'00'11;
constexpr uint32_t kApfsObjectSubtypeExtentRef = 0x00'00'00'0F;
constexpr uint32_t kApfsObjectSubtypeSnapMeta = 0x00'00'00'10;
constexpr uint32_t kApfsObjectTypeBtreeEphemeral = kApfsObjStorageEphemeral | 0x00'00'00'02;
constexpr uint32_t kApfsObjectSubtypeFreeQueue = 0x00'00'00'09;
constexpr uint32_t kApfsObjectTypeChunkInfoBlock = kApfsObjStoragePhysical | 0x00'00'00'07;
constexpr uint32_t kApfsObjectSubtypeFsTree = 0x00'00'00'0E;
constexpr uint32_t kApfsMagicNxsb = 0x42'53'58'4E;
constexpr uint32_t kApfsMagicApsb = 0x42'53'50'41;
constexpr uint32_t kApfsContainerIncompatVersion2 = 0x00'00'00'02;
constexpr uint16_t kApfsBtreeNodeRoot = 0x0001;
constexpr uint16_t kApfsBtreeNodeLeaf = 0x0002;
constexpr uint16_t kApfsBtreeNodeFixedKvSize = 0x0004;
constexpr uint32_t kApfsBtreePhysical = 0x00'00'00'10;
constexpr uint8_t kApfsRecordInode = 3;
constexpr uint8_t kApfsRecordDstreamId = 6;
constexpr uint8_t kApfsRecordFileExtent = 8;
constexpr uint8_t kApfsRecordDirectoryEntry = 9;
// Fixed timestamp written into generated records (an Apple-epoch nanosecond
// value captured from newfs_apfs output; deterministic images need a
// constant).
constexpr uint64_t kApfsGeneratedTimestamp = 0x18'B8'30'5B'1F'32'81'92ULL;
constexpr uint64_t kApfsTreeRootEntityId = 1;
constexpr uint64_t kApfsPrivateDirectoryId = 3;
constexpr uint64_t kApfsFirstUserObjectId = 16;
constexpr uint8_t kApfsInodeNameField = 4;
constexpr int kApfsObjTypeShift = 60;
constexpr uint16_t kApfsModeDirectory = 0040000;
constexpr uint16_t kApfsModeRegularFile = 0100000;
constexpr qsizetype kApfsObjectOidOffset = 0x08;
constexpr qsizetype kApfsObjectXidOffset = 0x10;
constexpr qsizetype kApfsObjectTypeOffset = 0x18;
constexpr qsizetype kApfsObjectSubtypeOffset = 0x1C;
constexpr qsizetype kApfsObjectMagicOffset = 0x20;
constexpr qsizetype kApfsNxBlockSizeOffset = 0x24;
constexpr qsizetype kApfsNxBlockCountOffset = 0x28;
constexpr qsizetype kApfsNxIncompatibleFeaturesOffset = 0x40;
constexpr qsizetype kApfsNxUuidOffset = 0x48;
constexpr qsizetype kApfsNxNextOidOffset = 0x58;
constexpr qsizetype kApfsNxNextXidOffset = 0x60;
constexpr qsizetype kApfsNxXpDescBlocksOffset = 0x68;
constexpr qsizetype kApfsNxXpDataBlocksOffset = 0x6C;
constexpr qsizetype kApfsNxXpDescBaseOffset = 0x70;
constexpr qsizetype kApfsNxXpDataBaseOffset = 0x78;
constexpr qsizetype kApfsNxXpDescNextOffset = 0x80;
constexpr qsizetype kApfsNxXpDataNextOffset = 0x84;
constexpr qsizetype kApfsNxXpDescIndexOffset = 0x88;
constexpr qsizetype kApfsNxXpDescLenOffset = 0x8C;
constexpr qsizetype kApfsNxXpDataIndexOffset = 0x90;
constexpr qsizetype kApfsNxXpDataLenOffset = 0x94;
constexpr qsizetype kApfsCheckpointMapFlagsOffset = 0x20;
constexpr qsizetype kApfsCheckpointMapCountOffset = 0x24;
constexpr qsizetype kApfsCheckpointMapEntriesOffset = 0x28;
constexpr qsizetype kApfsCheckpointMapEntryBytes = 40;
constexpr qsizetype kApfsCheckpointMapEntrySizeOffset = 8;
constexpr qsizetype kApfsCheckpointMapEntryOidOffset = 24;
constexpr qsizetype kApfsCheckpointMapEntryPaddrOffset = 32;
constexpr uint32_t kApfsCheckpointMapLastFlag = 0x00'00'00'01;
constexpr qsizetype kApfsNxReaperOidOffset = 0xA8;
// nx_ephemeral_info[0] packs min_block_count << 32 |
// NX_MAX_FILE_SYSTEM_EPH_STRUCTS (4) << 16 | NX_EPH_INFO_VERSION_1 (1);
// fsck_apfs/apfsck fail the checkpoint consistency check when it is zero or
// when min_block_count does not match the size-dependent reference value.
constexpr qsizetype kApfsNxEphemeralInfoOffset = 0x520;
constexpr uint64_t kApfsNxEphMinBlockCount = 8;
constexpr uint64_t kApfsNxMaxFileSystemEphStructs = 4;
constexpr uint64_t kApfsNxEphInfoVersion1 = 1;

// main free-queue B-tree node limit as a function of container block count
// (mirrors Apple newfs_apfs / apfsprogs lib/parameters.c main_fq_node_limit).
constexpr uint16_t mainFreeQueueNodeLimit(uint64_t blocks) {
    uint16_t ret = 0;
    if (blocks < 0x40000ULL) {
        ret = static_cast<uint16_t>(1 + (blocks - 1) / 4544);
    } else if (blocks < 0x10'00'00ULL) {
        ret = static_cast<uint16_t>(116 + (blocks - 261'281) / 2272);
    } else {
        ret = 512;
    }
    return ret == 2 ? 3 : ret;
}

// internal-pool free-queue B-tree node limit as a function of chunk count
// (mirrors apfsprogs lib/parameters.c ip_fq_node_limit).
constexpr uint16_t ipFreeQueueNodeLimit(uint64_t chunks) {
    const uint16_t ret = static_cast<uint16_t>(3 * (chunks + 751) / 1127 - 1);
    return ret == 2 ? 3 : ret;
}

// nx_ephemeral_info[0]: containers below 128 MiB use the main free-queue node
// limit as the minimum ephemeral block count; larger containers use a fixed
// minimum of NX_EPH_MIN_BLOCK_COUNT.
constexpr uint64_t apfsNxEphemeralInfoValue(uint64_t blockCount, uint32_t blockSize) {
    const uint64_t containerBytes = blockCount * static_cast<uint64_t>(blockSize);
    const uint64_t minBlockCount = containerBytes < 128ULL * 1024ULL * 1024ULL
                                       ? mainFreeQueueNodeLimit(blockCount)
                                       : kApfsNxEphMinBlockCount;
    return (minBlockCount << 32) | (kApfsNxMaxFileSystemEphStructs << 16) | kApfsNxEphInfoVersion1;
}

// nx_max_file_systems = ceil(container_bytes / 512 MiB), capped at the APFS
// maximum of 100 (mirrors Apple newfs_apfs / apfsprogs get_max_volumes).
constexpr uint32_t kApfsNxMaxFileSystemsCap = 100;
constexpr uint32_t apfsMaxFileSystems(uint64_t blockCount, uint32_t blockSize) {
    const uint64_t bytes = blockCount * static_cast<uint64_t>(blockSize);
    const uint64_t volumes = (bytes + (512ULL * 1024ULL * 1024ULL) - 1) /
                             (512ULL * 1024ULL * 1024ULL);
    if (volumes == 0) {
        return 1;
    }
    return static_cast<uint32_t>(volumes > kApfsNxMaxFileSystemsCap ? kApfsNxMaxFileSystemsCap
                                                                    : volumes);
}
constexpr qsizetype kApfsReaperNextReapIdOffset = 0x20;
constexpr qsizetype kApfsReaperFlagsOffset = 0x40;
constexpr uint32_t kApfsReaperBhmFlag = 0x00'00'00'01;
constexpr qsizetype kApfsNxSpacemanOidOffset = 0x98;
constexpr qsizetype kApfsNxOmapOidOffset = 0xA0;
constexpr qsizetype kApfsNxMaxFileSystemsOffset = 0xB4;
constexpr qsizetype kApfsNxFsOidArrayOffset = 0xB8;
constexpr qsizetype kApfsOmapTreeOidOffset = 0x30;
constexpr qsizetype kApfsOmapSnapshotCountOffset = 0x24;
constexpr qsizetype kApfsOmapSnapshotTreeOidOffset = 0x38;
constexpr qsizetype kApfsOmapMostRecentSnapshotOffset = 0x40;
constexpr qsizetype kApfsOmapPendingRevertMinOffset = 0x48;
constexpr qsizetype kApfsOmapPendingRevertMaxOffset = 0x50;
constexpr qsizetype kApfsVolumeIncompatibleFeaturesOffset = 0x38;
constexpr qsizetype kApfsVolumeAllocatedBlockCountOffset = 0x58;
constexpr qsizetype kApfsVolumeOmapOidOffset = 0x80;
constexpr qsizetype kApfsVolumeRootTreeOidOffset = 0x88;
constexpr qsizetype kApfsVolumeSnapMetaTreeOidOffset = 0x98;
constexpr qsizetype kApfsVolumeRevertToXidOffset = 0xA0;
constexpr qsizetype kApfsVolumeRevertToSblockOidOffset = 0xA8;
constexpr qsizetype kApfsVolumeNextObjectIdOffset = 0xB0;
constexpr qsizetype kApfsVolumeFileCountOffset = 0xB8;
constexpr qsizetype kApfsVolumeNumSnapshotsOffset = 0xD8;
constexpr qsizetype kApfsVolumeFsFlagsOffset = 0x108;
constexpr qsizetype kApfsVolumeUuidOffset = 0xF0;
constexpr qsizetype kApfsVolumeNameOffset = 0x2C0;
constexpr qsizetype kApfsVolumeNameBytes = 256;
constexpr qsizetype kApfsSpacemanBlockSizeOffset = 0x20;
constexpr qsizetype kApfsSpacemanMainDeviceOffset = 0x30;
constexpr qsizetype kApfsSpacemanDeviceBlockCountOffset = 0x00;
constexpr qsizetype kApfsSpacemanDeviceFreeCountOffset = 0x18;
// Spaceman internals (values mirrored from a real Sonoma BaseSystem container:
// blocks-per-chunk 32768, chunk-infos-per-cib 126, cibs-per-cab 507,
// SM_FLAG_VERSIONED, cib-address array at byte 2568).
constexpr qsizetype kApfsSpacemanBlocksPerChunkOffset = 0x24;
constexpr qsizetype kApfsSpacemanChunksPerCibOffset = 0x28;
constexpr qsizetype kApfsSpacemanCibsPerCabOffset = 0x2C;
constexpr qsizetype kApfsSpacemanDeviceChunkCountOffset = 0x08;
constexpr qsizetype kApfsSpacemanDeviceCibCountOffset = 0x10;
constexpr qsizetype kApfsSpacemanDeviceAddrOffsetOffset = 0x20;
constexpr qsizetype kApfsSpacemanFlagsOffset = 0x90;
constexpr uint32_t kApfsSpacemanBlocksPerChunk = 32'768;
constexpr uint32_t kApfsSpacemanChunksPerCib = 126;
constexpr uint32_t kApfsSpacemanCibsPerCab = 507;
constexpr uint64_t kGeneratedApfsSingleChunkMaxBlocks = kApfsSpacemanBlocksPerChunk;
constexpr uint64_t kGeneratedApfsSingleChunkMaxBytes =
    kGeneratedApfsSingleChunkMaxBlocks * static_cast<uint64_t>(kSupportedApfsBlockSizeBytes);
// Multi-chunk emission: a single chunk-info block addresses up to 126 chunks
// (no CAB tier), i.e. 126 * 128 MiB = 15.75 GiB of container.
constexpr uint64_t kGeneratedApfsMaxChunkCount = 126;
constexpr uint64_t kGeneratedApfsMultiChunkMaxBytes =
    kGeneratedApfsMaxChunkCount * kApfsSpacemanBlocksPerChunk *
    static_cast<uint64_t>(kSupportedApfsBlockSizeBytes);
constexpr uint32_t kApfsSpacemanFlagVersioned = 1;
constexpr qsizetype kApfsSpacemanCibAddrArrayOffset = 2568;
constexpr qsizetype kApfsSpacemanIpBmTxMultiplierOffset = 0x94;
constexpr qsizetype kApfsSpacemanIpBlockCountOffset = 0x98;
constexpr qsizetype kApfsSpacemanIpBmSizeOffset = 0xA0;
constexpr qsizetype kApfsSpacemanIpBmBlockCountOffset = 0xA4;
constexpr qsizetype kApfsSpacemanIpBmBaseOffset = 0xA8;
constexpr qsizetype kApfsSpacemanIpBaseOffset = 0xB0;
constexpr qsizetype kApfsSpacemanIpBmFreeHeadOffset = 0x140;
constexpr qsizetype kApfsSpacemanIpBmFreeTailOffset = 0x142;
constexpr uint32_t kApfsSpacemanIpBmTxMultiplier = 16;
// Free-queue tree_node_limit values mirrored from the reference container;
// nonzero even when the queues themselves are empty.
constexpr qsizetype kApfsSpacemanFqIpLimitOffset = 0xE0;
constexpr qsizetype kApfsSpacemanFqMainLimitOffset = 0x108;
constexpr qsizetype kApfsChunkInfoCountOffset = 0x24;
constexpr qsizetype kApfsChunkInfoEntriesOffset = 0x28;
constexpr qsizetype kApfsChunkInfoEntryAddrOffset = 8;
constexpr qsizetype kApfsChunkInfoEntryBlockCountOffset = 16;
constexpr qsizetype kApfsChunkInfoEntryFreeCountOffset = 20;
constexpr qsizetype kApfsChunkInfoEntryBitmapAddrOffset = 24;
constexpr qsizetype kApfsChunkInfoEntryStride = 32;
constexpr qsizetype kApfsBtreeNodeHeaderBytes = 0x38;
constexpr qsizetype kApfsBtreeNodeFlagsOffset = 0x20;
constexpr qsizetype kApfsBtreeNodeLevelOffset = 0x22;
constexpr qsizetype kApfsBtreeNodeCountOffset = 0x24;
constexpr qsizetype kApfsBtreeNodeTableOffsetOffset = 0x28;
constexpr qsizetype kApfsBtreeNodeTableLengthOffset = 0x2A;
constexpr qsizetype kApfsBtreeInfoBytes = 40;
constexpr qsizetype kApfsBtreeInfoFlagsOffset = 0x00;
constexpr qsizetype kApfsBtreeInfoNodeSizeOffset = 0x04;
constexpr qsizetype kApfsBtreeInfoKeySizeOffset = 0x08;
constexpr qsizetype kApfsBtreeInfoValueSizeOffset = 0x0C;
constexpr qsizetype kApfsBtreeInfoKeyCountOffset = 0x18;
constexpr qsizetype kApfsBtreeInfoNodeCountOffset = 0x20;
constexpr qsizetype kApfsOmapKeyXidOffset = 8;
constexpr qsizetype kApfsOmapValueSizeOffset = 4;
constexpr qsizetype kApfsOmapValuePaddrOffset = 8;
constexpr qsizetype kApfsObjectMapKeyBytes = 16;
constexpr qsizetype kApfsObjectMapValueBytes = 16;
constexpr qsizetype kApfsBtreeFixedTocEntryBytes = 4;
constexpr qsizetype kApfsBtreeVariableTocEntryBytes = 8;
constexpr qsizetype kApfsBtreeFixedTocValueOffset = 2;
constexpr qsizetype kApfsBtreeVariableTocKeyLengthOffset = 2;
constexpr qsizetype kApfsBtreeVariableTocValueOffset = 4;
constexpr qsizetype kApfsBtreeVariableTocValueLengthOffset = 6;
constexpr qsizetype kApfsInodePrivateIdOffset = 0x08;
constexpr qsizetype kApfsInodeModeOffset = 0x50;
constexpr qsizetype kApfsInodeXfieldsOffset = 0x5C;
constexpr qsizetype kApfsFormattedRootInodeValueBytes = 0x60;
constexpr qsizetype kApfsFormattedFileInodeValueBytes = 0x90;
constexpr qsizetype kApfsFormattedRootInodeKeyBytes = 8;
constexpr qsizetype kApfsFileExtentKeyBytes = 16;
constexpr qsizetype kApfsFileExtentValueBytes = 24;
constexpr qsizetype kApfsFileExtentKeyLogicalOffset = 8;
constexpr qsizetype kApfsFileExtentValuePhysicalBlockOffset = 8;
constexpr qsizetype kApfsFileExtentValueCryptoIdOffset = 16;
constexpr qsizetype kApfsDrecValueBytes = 18;
constexpr qsizetype kApfsDrecNameLengthOffset = 8;
constexpr qsizetype kApfsDrecNameOffset = 12;
constexpr qsizetype kApfsDrecFileIdOffset = 0;
constexpr qsizetype kApfsDrecTypeOffset = 16;
constexpr uint16_t kApfsDirTypeDirectory = 4;
constexpr uint16_t kApfsDirTypeRegularFile = 8;
constexpr uint8_t kApfsInodeDstreamField = 8;
constexpr qsizetype kApfsXfieldHeaderBytes = 4;
constexpr qsizetype kApfsXfieldDataBytesOffset = 2;
constexpr qsizetype kApfsXfieldTocEntryBytes = 4;
constexpr qsizetype kApfsXfieldSizeOffset = 2;
constexpr qsizetype kApfsXfieldAlignment = 8;
constexpr qsizetype kApfsXfieldAlignmentPadding = 7;
constexpr qsizetype kApfsDstreamMinBytes = 40;
constexpr QLatin1StringView kEvidenceStructureMapping("structure-mapping");
constexpr QLatin1StringView kEvidenceObjectChecksumVectors("object-checksum-vectors");
constexpr QLatin1StringView kEvidenceSourceImageHash("source-image-hash");
constexpr QLatin1StringView kEvidenceScratchImageHash("scratch-image-hash");
constexpr QLatin1StringView kEvidenceCopyOnWriteCheckpoint("copy-on-write-checkpoint");
constexpr QLatin1StringView kEvidenceObjectMapUpdate("object-map-update");
constexpr QLatin1StringView kEvidenceSpaceManagerAccounting("space-manager-accounting");
constexpr QLatin1StringView kEvidenceFsckValidation("fsck-validation");
constexpr QLatin1StringView kEvidenceTargetReadback("target-readback");
constexpr QLatin1StringView kEvidenceCrashReplay("crash-replay");
constexpr QLatin1StringView kEvidenceRollbackBoundary("rollback-boundary");

[[nodiscard]] bool detailStartsWith(const QString& detail, QStringView prefix) {
    return detail.startsWith(prefix, Qt::CaseInsensitive);
}

[[nodiscard]] std::optional<uint64_t> parseHexDetail(const QStringList& details,
                                                     QStringView prefix) {
    for (const auto& detail : details) {
        if (!detailStartsWith(detail, prefix)) {
            continue;
        }
        const qsizetype marker = detail.indexOf(QStringLiteral("0x"), 0, Qt::CaseInsensitive);
        if (marker < 0) {
            return std::nullopt;
        }
        bool ok = false;
        const uint64_t value = detail.sliced(marker + 2).trimmed().toULongLong(&ok, 16);
        if (ok) {
            return value;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<uint64_t> parseDecimalDetail(const QStringList& details,
                                                         QStringView prefix) {
    for (const auto& detail : details) {
        if (!detailStartsWith(detail, prefix)) {
            continue;
        }
        const auto parts = detail.split(QChar(':'));
        if (parts.size() < kMinimumKeyValuePartCount) {
            return std::nullopt;
        }
        bool ok = false;
        const uint64_t value = parts.last().trimmed().toULongLong(&ok, 10);
        if (ok) {
            return value;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool hasDetailContaining(const QStringList& details, QStringView needle) {
    for (const auto& detail : details) {
        if (detail.contains(needle, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool hasSnapshotMutationRisk(const QStringList& details) {
    for (const auto& detail : details) {
        // A populated snapshot-metadata tree OID alone is the normal state of
        // every Apple-formatted volume (the tree exists empty from mkfs);
        // mutation risk is signalled by actual snapshots, a pending revert,
        // or a most-recent-snapshot transaction.
        if (detail.contains(QStringLiteral("most recent snapshot XID"), Qt::CaseInsensitive) ||
            detail.contains(QStringLiteral("pending revert"), Qt::CaseInsensitive)) {
            return true;
        }

        const int snapshotCountAt =
            detail.indexOf(QStringLiteral("snapshots "), 0, Qt::CaseInsensitive);
        if (snapshotCountAt < 0) {
            continue;
        }
        const QString countText = detail.mid(snapshotCountAt + kApfsSnapshotCountPrefixLength)
                                      .section(QLatin1Char(','), 0, 0)
                                      .trimmed();
        bool ok = false;
        const uint64_t count = countText.toULongLong(&ok);
        if (ok && count > 0) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] QString normalizedApfsTargetPath(QString path) {
    path = path.trimmed();
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (path.contains(QStringLiteral("//"))) {
        path.replace(QStringLiteral("//"), QStringLiteral("/"));
    }
    if (path.isEmpty()) {
        return {};
    }
    if (!path.startsWith(QLatin1Char('/'))) {
        path.prepend(QLatin1Char('/'));
    }
    while (path.size() > 1 && path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    return path;
}

[[nodiscard]] QString normalizedApfsVolumeName(QString name) {
    name = name.trimmed();
    name.replace(QChar(0), QLatin1Char(' '));
    return name;
}

[[nodiscard]] bool containsUnsafePathSegment(const QString& normalized_path) {
    for (const auto& part : normalized_path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part == QStringLiteral(".") || part == QStringLiteral("..")) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool containsUnsafeVolumeNameCharacter(const QString& name) {
    return name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) ||
           name.contains(QLatin1Char(':'));
}

QString nulTerminatedUtf8Field(const QByteArray& block, qsizetype offset, qsizetype maxBytes) {
    if (offset < 0 || maxBytes <= 0 || offset >= block.size()) {
        return {};
    }
    const qsizetype available = std::min(maxBytes, block.size() - offset);
    const char* begin = block.constData() + offset;
    qsizetype length = 0;
    while (length < available && begin[length] != '\0') {
        ++length;
    }
    return QString::fromUtf8(begin, length);
}

void writeLe16(QByteArray* bytes, qsizetype offset, uint16_t value) {
    qToLittleEndian<uint16_t>(value, reinterpret_cast<uchar*>(bytes->data() + offset));
}

void writeLe32(QByteArray* bytes, qsizetype offset, uint32_t value) {
    qToLittleEndian<uint32_t>(value, reinterpret_cast<uchar*>(bytes->data() + offset));
}

void writeLe64(QByteArray* bytes, qsizetype offset, uint64_t value) {
    qToLittleEndian<uint64_t>(value, reinterpret_cast<uchar*>(bytes->data() + offset));
}

uint16_t le16(const QByteArray& bytes, qsizetype offset) {
    if (offset < 0 || bytes.size() - offset < static_cast<qsizetype>(sizeof(uint16_t))) {
        return 0;
    }
    return qFromLittleEndian<uint16_t>(reinterpret_cast<const uchar*>(bytes.constData() + offset));
}

uint32_t le32(const QByteArray& bytes, qsizetype offset) {
    if (offset < 0 || bytes.size() - offset < static_cast<qsizetype>(sizeof(uint32_t))) {
        return 0;
    }
    return qFromLittleEndian<uint32_t>(reinterpret_cast<const uchar*>(bytes.constData() + offset));
}

uint64_t le64(const QByteArray& bytes, qsizetype offset) {
    if (offset < 0 || bytes.size() - offset < static_cast<qsizetype>(sizeof(uint64_t))) {
        return 0;
    }
    return qFromLittleEndian<uint64_t>(reinterpret_cast<const uchar*>(bytes.constData() + offset));
}

void writeAscii(QByteArray* bytes, qsizetype offset, const QByteArray& value, qsizetype maxBytes) {
    const qsizetype copyBytes = std::min(maxBytes, value.size());
    std::fill(bytes->begin() + offset, bytes->begin() + offset + maxBytes, '\0');
    std::copy(value.cbegin(), value.cbegin() + copyBytes, bytes->begin() + offset);
}

QByteArray newApfsObjectBlock(uint32_t blockSize,
                              uint64_t oid,
                              uint64_t xid,
                              uint32_t objectType,
                              uint32_t objectSubtype = 0) {
    QByteArray block(static_cast<qsizetype>(blockSize), '\0');
    writeLe64(&block, kApfsObjectOidOffset, oid);
    writeLe64(&block, kApfsObjectXidOffset, xid);
    writeLe32(&block, kApfsObjectTypeOffset, objectType);
    writeLe32(&block, kApfsObjectSubtypeOffset, objectSubtype);
    return block;
}

bool stampApfsObjectBlock(QByteArray* block, QStringList* blockers) {
    if (PartitionApfsWriter::stampObjectChecksum(block)) {
        return true;
    }
    if (blockers) {
        blockers->append(QStringLiteral("Unable to stamp APFS object checksum"));
    }
    return false;
}

// APFS container/volume UUIDs must be unique per instance (newfs_apfs emits a
// random v4 UUID). Returns the 16-byte RFC 4122 representation.
QByteArray randomApfsUuid() {
    return QUuid::createUuid().toRfc4122();
}

struct ApfsBtreeInfoFields {
    uint32_t blockSize{0};
    uint32_t flags{0};
    uint32_t keySize{0};
    uint32_t valueSize{0};
    uint64_t keyCount{0};
    uint64_t nodeCount{0};
};

void writeBtreeInfo(QByteArray* block, const ApfsBtreeInfoFields& fields) {
    const qsizetype info = static_cast<qsizetype>(fields.blockSize) - kApfsBtreeInfoBytes;
    writeLe32(block, info + kApfsBtreeInfoFlagsOffset, fields.flags);
    writeLe32(block, info + kApfsBtreeInfoNodeSizeOffset, fields.blockSize);
    writeLe32(block, info + kApfsBtreeInfoKeySizeOffset, fields.keySize);
    writeLe32(block, info + kApfsBtreeInfoValueSizeOffset, fields.valueSize);
    writeLe64(block, info + kApfsBtreeInfoKeyCountOffset, fields.keyCount);
    writeLe64(block, info + kApfsBtreeInfoNodeCountOffset, fields.nodeCount);
}

struct ApfsObjectMapEntry {
    uint64_t oid{0};
    uint64_t xid{0};
    uint64_t physicalBlock{0};
};

QByteArray buildObjectMapTreeBlock(uint32_t blockSize,
                                   uint64_t oid,
                                   const QVector<ApfsObjectMapEntry>& mappings,
                                   uint64_t xid,
                                   QStringList* blockers) {
    // Object-map root/leaf mirrored from newfs_apfs output: fixed 448-byte
    // TOC, info flags FIXED_KV|SEQUENTIAL_INSERT (0x12), 16-byte keys and
    // values, longest-key/value fields populated when records exist.
    QByteArray block = newApfsObjectBlock(blockSize, oid, xid, kApfsObjectTypeBtreePhysical);
    writeLe32(&block, kApfsObjectSubtypeOffset, 0x00'00'00'0B);
    constexpr qsizetype kOmapTocBytes = 448;
    writeLe16(&block,
              kApfsBtreeNodeFlagsOffset,
              kApfsBtreeNodeRoot | kApfsBtreeNodeLeaf | kApfsBtreeNodeFixedKvSize);
    writeLe16(&block, kApfsBtreeNodeLevelOffset, 0);
    writeLe32(&block, kApfsBtreeNodeCountOffset, static_cast<uint32_t>(mappings.size()));
    writeLe16(&block, kApfsBtreeNodeTableOffsetOffset, 0);
    writeLe16(&block, kApfsBtreeNodeTableLengthOffset, kOmapTocBytes);

    const qsizetype tableStart = kApfsBtreeNodeHeaderBytes;
    const qsizetype keyAreaStart = tableStart + kOmapTocBytes;
    const qsizetype valueAreaEnd = static_cast<qsizetype>(blockSize) - kApfsBtreeInfoBytes;
    const qsizetype keyBytesUsed = static_cast<qsizetype>(mappings.size()) * kApfsObjectMapKeyBytes;
    const qsizetype valueBytesUsed = static_cast<qsizetype>(mappings.size()) *
                                     kApfsObjectMapValueBytes;
    writeLe16(&block, 0x2C, static_cast<uint16_t>(keyBytesUsed));
    writeLe16(&block,
              0x2E,
              static_cast<uint16_t>(valueAreaEnd - keyAreaStart - keyBytesUsed - valueBytesUsed));
    writeLe16(&block, 0x30, 0xFFFF);
    writeLe16(&block, 0x32, 0);
    writeLe16(&block, 0x34, 0xFFFF);
    writeLe16(&block, 0x36, 0);
    for (qsizetype index = 0; index < mappings.size(); ++index) {
        const qsizetype toc = tableStart + index * kApfsBtreeFixedTocEntryBytes;
        const qsizetype keyOffset = index * kApfsObjectMapKeyBytes;
        const qsizetype valueBackOffset = (index + 1) * kApfsObjectMapValueBytes;
        const qsizetype key = keyAreaStart + keyOffset;
        const qsizetype value = valueAreaEnd - valueBackOffset;
        writeLe16(&block, toc, static_cast<uint16_t>(keyOffset));
        writeLe16(&block,
                  toc + kApfsBtreeFixedTocValueOffset,
                  static_cast<uint16_t>(valueBackOffset));
        writeLe64(&block, key, mappings.at(index).oid);
        writeLe64(&block, key + kApfsOmapKeyXidOffset, mappings.at(index).xid);
        writeLe32(&block, value, 0);
        writeLe32(&block, value + kApfsOmapValueSizeOffset, blockSize);
        writeLe64(&block, value + kApfsOmapValuePaddrOffset, mappings.at(index).physicalBlock);
    }
    const qsizetype infoOffset = valueAreaEnd;
    writeLe32(&block, infoOffset + kApfsBtreeInfoFlagsOffset, 0x00'00'00'12);
    writeLe32(&block, infoOffset + kApfsBtreeInfoNodeSizeOffset, blockSize);
    writeLe32(&block, infoOffset + kApfsBtreeInfoKeySizeOffset, kApfsObjectMapKeyBytes);
    writeLe32(&block, infoOffset + kApfsBtreeInfoValueSizeOffset, kApfsObjectMapValueBytes);
    if (!mappings.isEmpty()) {
        writeLe32(&block, infoOffset + 16, kApfsObjectMapKeyBytes);
        writeLe32(&block, infoOffset + 20, kApfsObjectMapValueBytes);
    }
    writeLe64(&block,
              infoOffset + kApfsBtreeInfoKeyCountOffset,
              static_cast<uint64_t>(mappings.size()));
    writeLe64(&block, infoOffset + kApfsBtreeInfoNodeCountOffset, 1);
    stampApfsObjectBlock(&block, blockers);
    return block;
}

struct ApfsBtreeKeyValue {
    QByteArray key;
    QByteArray value;
};

struct ApfsRootFilePayload {
    QString fileName;
    QString parentDirectoryName;
    QByteArray data;
    uint64_t parentDirectoryId{kApfsRootDirectoryId};
    uint64_t fileId{0};
    uint64_t privateId{0};
    uint64_t dataStartBlock{0};
};

struct ApfsRootDirectoryPayload {
    QString directoryName;
    uint64_t directoryId{0};
    uint64_t privateId{0};
};

uint32_t crc32cWord(uint32_t crc, uint32_t word) {
    for (int byteIndex = 0; byteIndex < 4; ++byteIndex) {
        crc ^= (word >> (byteIndex * 8)) & 0xFF;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0x82'F6'3B'78u : 0u);
        }
    }
    return crc;
}

// j_drec_hashed_key name_len_and_hash: low 10 bits hold the name length
// (including the trailing NUL); the high 22 bits hold a CRC-32C over the
// name's UTF-32LE code points (seed ~0, no final inversion), verified against
// names written by Apple's own APFS driver ('root', 'private-dir',
// 'proof.txt').
uint32_t drecNameLenAndHash(const QString& name) {
    uint32_t crc = 0xFF'FF'FF'FFu;
    for (const QChar& ch : name) {
        crc = crc32cWord(crc, ch.unicode());
    }
    const uint32_t nameLength = static_cast<uint32_t>(name.toUtf8().size() + 1) & 0x3FF;
    return ((crc & 0x3F'FF'FF) << 10) | nameLength;
}

QByteArray fsKey(uint64_t objectId,
                 uint8_t recordType,
                 qsizetype keyBytes = kApfsFormattedRootInodeKeyBytes) {
    QByteArray key(keyBytes, '\0');
    writeLe64(&key, 0, objectId | (static_cast<uint64_t>(recordType) << kApfsObjTypeShift));
    return key;
}

// j_inode_val mirroring records written by Apple's APFS driver: parent and
// private ids, timestamps, link/child count, mode, plus extended fields — a
// NAME xfield on every inode and a 40-byte DSTREAM xfield on regular files.
struct ApfsInodeParams {
    uint64_t parentId;
    uint64_t privateId;
    uint16_t mode;
    QString name;
    uint64_t sizeBytes = 0;
    int32_t childOrLinkCount = 1;
};

QByteArray inodeValue(const ApfsInodeParams& params) {
    const auto& [parentId, privateId, mode, name, sizeBytes, childOrLinkCount] = params;
    const bool regularFile = (mode & kApfsModeRegularFile) == kApfsModeRegularFile;
    const QByteArray nameBytes = name.toUtf8() + '\0';
    const qsizetype xfieldCount = regularFile ? 2 : 1;
    const qsizetype tocBytes = kApfsXfieldHeaderBytes + xfieldCount * kApfsXfieldTocEntryBytes;
    const qsizetype namePadded =
        ((nameBytes.size() + kApfsXfieldAlignmentPadding) / kApfsXfieldAlignment) *
        kApfsXfieldAlignment;
    const qsizetype valueBytes = kApfsInodeXfieldsOffset + tocBytes + namePadded +
                                 (regularFile ? kApfsDstreamMinBytes : 0);
    QByteArray value(valueBytes, '\0');
    writeLe64(&value, 0, parentId);
    writeLe64(&value, kApfsInodePrivateIdOffset, privateId);
    writeLe64(&value, 0x10, kApfsGeneratedTimestamp);
    writeLe64(&value, 0x18, kApfsGeneratedTimestamp);
    writeLe64(&value, 0x20, kApfsGeneratedTimestamp);
    writeLe64(&value, 0x28, kApfsGeneratedTimestamp);
    writeLe64(&value, 0x30, 0x8000);
    writeLe32(&value, 0x38, static_cast<uint32_t>(childOrLinkCount));
    const uint16_t permissions = (mode & 0777) ? 0 : (regularFile ? 0644 : 0755);
    writeLe16(&value, kApfsInodeModeOffset, mode | permissions);
    writeLe16(&value, kApfsInodeXfieldsOffset, static_cast<uint16_t>(xfieldCount));
    writeLe16(&value,
              kApfsInodeXfieldsOffset + kApfsXfieldDataBytesOffset,
              static_cast<uint16_t>(namePadded + (regularFile ? kApfsDstreamMinBytes : 0)));
    qsizetype toc = kApfsInodeXfieldsOffset + kApfsXfieldHeaderBytes;
    value[toc] = static_cast<char>(kApfsInodeNameField);
    value[toc + 1] = 0x02;
    writeLe16(&value, toc + kApfsXfieldSizeOffset, static_cast<uint16_t>(nameBytes.size()));
    if (regularFile) {
        toc += kApfsXfieldTocEntryBytes;
        value[toc] = static_cast<char>(kApfsInodeDstreamField);
        value[toc + 1] = 0x08;
        writeLe16(&value, toc + kApfsXfieldSizeOffset, kApfsDstreamMinBytes);
    }
    const qsizetype dataStart = kApfsInodeXfieldsOffset + tocBytes;
    std::copy(nameBytes.cbegin(), nameBytes.cend(), value.begin() + dataStart);
    if (regularFile) {
        const qsizetype dstream = dataStart + namePadded;
        writeLe64(&value, dstream, sizeBytes);
        writeLe64(&value,
                  dstream + 8,
                  ((sizeBytes + kSupportedApfsBlockSizeBytes - 1) / kSupportedApfsBlockSizeBytes) *
                      kSupportedApfsBlockSizeBytes);
        writeLe64(&value, dstream + 24, sizeBytes);
    }
    return value;
}

QByteArray directoryEntryKey(uint64_t parentId, const QString& fileName) {
    const QByteArray nameBytes = fileName.toUtf8();
    QByteArray key =
        fsKey(parentId, kApfsRecordDirectoryEntry, kApfsDrecNameOffset + nameBytes.size() + 1);
    writeLe32(&key, kApfsDrecNameLengthOffset, drecNameLenAndHash(fileName));
    std::copy(nameBytes.cbegin(), nameBytes.cend(), key.begin() + kApfsDrecNameOffset);
    return key;
}

QByteArray directoryEntryValue(uint64_t fileId, uint16_t entryType) {
    QByteArray value(kApfsDrecValueBytes, '\0');
    writeLe64(&value, kApfsDrecFileIdOffset, fileId);
    writeLe64(&value, 8, kApfsGeneratedTimestamp);
    writeLe16(&value, kApfsDrecTypeOffset, entryType);
    return value;
}

QByteArray dstreamIdValue() {
    QByteArray value(4, '\0');
    writeLe32(&value, 0, 1);
    return value;
}

QByteArray fileExtentKey(uint64_t privateId) {
    QByteArray key = fsKey(privateId, kApfsRecordFileExtent, kApfsFileExtentKeyBytes);
    writeLe64(&key, kApfsFileExtentKeyLogicalOffset, 0);
    return key;
}

QByteArray fileExtentValue(uint64_t sizeBytes, uint64_t dataStartBlock) {
    QByteArray value(kApfsFileExtentValueBytes, '\0');
    // Extent lengths cover whole blocks; the logical size lives in the
    // inode's dstream xfield.
    writeLe64(&value,
              0,
              ((sizeBytes + kSupportedApfsBlockSizeBytes - 1) / kSupportedApfsBlockSizeBytes) *
                  kSupportedApfsBlockSizeBytes);
    writeLe64(&value, kApfsFileExtentValuePhysicalBlockOffset, dataStartBlock);
    writeLe64(&value, kApfsFileExtentValueCryptoIdOffset, 0);
    return value;
}

uint64_t roundedBlockCount(uint64_t bytes, uint32_t blockSize) {
    if (bytes == 0 || blockSize == 0) {
        return 0;
    }
    return (bytes + blockSize - 1) / blockSize;
}

qsizetype alignApfsValueBytes(qsizetype bytes) {
    return ((bytes + kApfsXfieldAlignmentPadding) / kApfsXfieldAlignment) * kApfsXfieldAlignment;
}

bool isSafeSeedFileName(const QString& fileName) {
    return !fileName.trimmed().isEmpty() && !fileName.contains(QLatin1Char('/')) &&
           !fileName.contains(QLatin1Char('\\')) && fileName != QStringLiteral(".") &&
           fileName != QStringLiteral("..");
}

void appendRootFileRecords(QVector<ApfsBtreeKeyValue>* records, const ApfsRootFilePayload& file) {
    records->append({fsKey(file.fileId, kApfsRecordInode),
                     inodeValue({.parentId = file.parentDirectoryId,
                                 .privateId = file.privateId,
                                 .mode = kApfsModeRegularFile,
                                 .name = file.fileName,
                                 .sizeBytes = static_cast<uint64_t>(file.data.size())})});
    records->append({directoryEntryKey(file.parentDirectoryId, file.fileName),
                     directoryEntryValue(file.fileId, kApfsDirTypeRegularFile)});
    records->append({fsKey(file.privateId, kApfsRecordDstreamId), dstreamIdValue()});
    records->append(
        {fileExtentKey(file.privateId),
         fileExtentValue(static_cast<uint64_t>(file.data.size()), file.dataStartBlock)});
}

void appendRootDirectoryRecords(QVector<ApfsBtreeKeyValue>* records,
                                const ApfsRootDirectoryPayload& directory) {
    records->append({fsKey(directory.directoryId, kApfsRecordInode),
                     inodeValue({.parentId = kApfsRootDirectoryId,
                                 .privateId = directory.privateId,
                                 .mode = kApfsModeDirectory,
                                 .name = directory.directoryName})});
    records->append({directoryEntryKey(kApfsRootDirectoryId, directory.directoryName),
                     directoryEntryValue(directory.directoryId, kApfsDirTypeDirectory)});
}

QByteArray buildRootTreeBlock(uint32_t blockSize,
                              const QVector<ApfsRootFilePayload>& files,
                              const QVector<ApfsRootDirectoryPayload>& directories,
                              QStringList* blockers) {
    QByteArray block = newApfsObjectBlock(blockSize,
                                          kApfsFormatRootTreeOid,
                                          kApfsFormatXid,
                                          kApfsObjectTypeBtree,
                                          kApfsObjectSubtypeFsTree);
    writeLe16(&block, kApfsBtreeNodeFlagsOffset, kApfsBtreeNodeRoot | kApfsBtreeNodeLeaf);
    writeLe16(&block, kApfsBtreeNodeLevelOffset, 0);

    // Base volume entities mirroring newfs_apfs output: the tree-root entity
    // (oid 1) carries 'root' and 'private-dir' directory entries; the root
    // directory (oid 2) and private directory (oid 3) carry inodes.
    QVector<ApfsBtreeKeyValue> records{
        {directoryEntryKey(kApfsTreeRootEntityId, QStringLiteral("root")),
         directoryEntryValue(kApfsRootDirectoryId, kApfsDirTypeDirectory)},
        {directoryEntryKey(kApfsTreeRootEntityId, QStringLiteral("private-dir")),
         directoryEntryValue(kApfsPrivateDirectoryId, kApfsDirTypeDirectory)},
        {fsKey(kApfsRootDirectoryId, kApfsRecordInode),
         inodeValue({.parentId = kApfsTreeRootEntityId,
                     .privateId = kApfsRootDirectoryId,
                     .mode = kApfsModeDirectory,
                     .name = QStringLiteral("root"),
                     .childOrLinkCount = static_cast<int32_t>(files.size() + directories.size())})},
        {fsKey(kApfsPrivateDirectoryId, kApfsRecordInode),
         inodeValue({.parentId = kApfsTreeRootEntityId,
                     .privateId = kApfsPrivateDirectoryId,
                     .mode = static_cast<uint16_t>(kApfsModeDirectory | 0644),
                     .name = QStringLiteral("private-dir"),
                     .childOrLinkCount = 0})}};
    for (const auto& file : files) {
        appendRootFileRecords(&records, file);
    }
    for (const auto& directory : directories) {
        appendRootDirectoryRecords(&records, directory);
    }
    // File-system B-tree records must be stored in key order (object id, then
    // record type, then key tail) for Apple's binary-search lookups.
    std::stable_sort(records.begin(),
                     records.end(),
                     [](const ApfsBtreeKeyValue& left, const ApfsBtreeKeyValue& right) {
                         const uint64_t leftHeader = le64(left.key, 0);
                         const uint64_t rightHeader = le64(right.key, 0);
                         const uint64_t leftId = leftHeader & ((1ULL << kApfsObjTypeShift) - 1);
                         const uint64_t rightId = rightHeader & ((1ULL << kApfsObjTypeShift) - 1);
                         if (leftId != rightId) {
                             return leftId < rightId;
                         }
                         const uint64_t leftType = leftHeader >> kApfsObjTypeShift;
                         const uint64_t rightType = rightHeader >> kApfsObjTypeShift;
                         if (leftType != rightType) {
                             return leftType < rightType;
                         }
                         // Sibling directory entries order by the numeric
                         // name_len_and_hash word; extents by logical offset.
                         if (leftType == kApfsRecordDirectoryEntry) {
                             return le32(left.key, kApfsDrecNameLengthOffset) <
                                    le32(right.key, kApfsDrecNameLengthOffset);
                         }
                         if (leftType == kApfsRecordFileExtent) {
                             return le64(left.key, kApfsFileExtentKeyLogicalOffset) <
                                    le64(right.key, kApfsFileExtentKeyLogicalOffset);
                         }
                         return left.key < right.key;
                     });

    writeLe32(&block, kApfsBtreeNodeCountOffset, static_cast<uint32_t>(records.size()));
    // Table-of-contents capacity grows in the 64-byte granules Apple's
    // B-tree code reserves (a fresh fs-tree carries a 64-byte TOC).
    const qsizetype tocLength =
        ((static_cast<qsizetype>(records.size()) * kApfsBtreeVariableTocEntryBytes + 63) / 64) * 64;
    writeLe16(&block, kApfsBtreeNodeTableOffsetOffset, 0);
    writeLe16(&block, kApfsBtreeNodeTableLengthOffset, static_cast<uint16_t>(tocLength));

    const qsizetype keyAreaStart = kApfsBtreeNodeHeaderBytes + tocLength;
    const qsizetype valueAreaEnd = static_cast<qsizetype>(blockSize) - kApfsBtreeInfoBytes;
    qsizetype keyCursor = 0;
    qsizetype valueBackCursor = 0;
    for (qsizetype index = 0; index < records.size(); ++index) {
        const auto& record = records.at(index);
        valueBackCursor += record.value.size();
        const qsizetype toc = kApfsBtreeNodeHeaderBytes + index * kApfsBtreeVariableTocEntryBytes;
        const qsizetype valueOffset = valueAreaEnd - valueBackCursor;
        writeLe16(&block, toc, static_cast<uint16_t>(keyCursor));
        writeLe16(&block,
                  toc + kApfsBtreeVariableTocKeyLengthOffset,
                  static_cast<uint16_t>(record.key.size()));
        writeLe16(&block,
                  toc + kApfsBtreeVariableTocValueOffset,
                  static_cast<uint16_t>(valueBackCursor));
        writeLe16(&block,
                  toc + kApfsBtreeVariableTocValueLengthOffset,
                  static_cast<uint16_t>(record.value.size()));
        std::copy(record.key.cbegin(), record.key.cend(), block.begin() + keyAreaStart + keyCursor);
        std::copy(record.value.cbegin(), record.value.cend(), block.begin() + valueOffset);
        keyCursor += record.key.size();
    }

    writeLe16(&block, 0x2C, static_cast<uint16_t>(keyCursor));
    writeLe16(&block,
              0x2E,
              static_cast<uint16_t>(valueAreaEnd - keyAreaStart - keyCursor - valueBackCursor));
    writeLe16(&block, 0x30, 0xFFFF);
    writeLe16(&block, 0x32, 0);
    writeLe16(&block, 0x34, 0xFFFF);
    writeLe16(&block, 0x36, 0);
    writeBtreeInfo(&block,
                   {.blockSize = blockSize,
                    .keyCount = static_cast<uint64_t>(records.size()),
                    .nodeCount = 1});
    // Info flags and longest-key/value fields per Apple's fs-trees
    // (ALLOWS_GHOSTS|KEY_HASHED-style flag word 0x42).
    qsizetype longestKey = 0;
    qsizetype longestValue = 0;
    for (const auto& record : records) {
        longestKey = std::max(longestKey, record.key.size());
        longestValue = std::max(longestValue, record.value.size());
    }
    const qsizetype infoOffset = static_cast<qsizetype>(blockSize) - kApfsBtreeInfoBytes;
    writeLe32(&block, infoOffset + kApfsBtreeInfoFlagsOffset, 0x00'00'00'42);
    writeLe32(&block, infoOffset + 16, static_cast<uint32_t>(longestKey));
    writeLe32(&block, infoOffset + 20, static_cast<uint32_t>(longestValue));
    stampApfsObjectBlock(&block, blockers);
    return block;
}

QByteArray buildRootTreeBlock(uint32_t blockSize,
                              const QVector<ApfsRootFilePayload>& files,
                              QStringList* blockers) {
    return buildRootTreeBlock(blockSize, files, {}, blockers);
}

QByteArray buildEmptyRootTreeBlock(uint32_t blockSize, QStringList* blockers) {
    return buildRootTreeBlock(blockSize, {}, blockers);
}

struct ApfsCheckpointPosition {
    uint64_t xid{kApfsFormatXid};
    uint32_t descIndex{2};
    uint32_t descNext{4};
    uint32_t dataIndex{2};
    uint32_t dataLen{4};
    uint32_t dataNext{6};
    uint64_t omapOid{kApfsFormatContainerOmapBlock};
    uint64_t fsOid{kApfsFormatVolumeOid};
    uint64_t nextOid{kApfsNxMinimumNextOid};
};

QByteArray buildNxSuperblock(uint32_t blockSize,
                             uint64_t blockCount,
                             const QByteArray& containerUuid,
                             const ApfsCheckpointPosition& position,
                             QStringList* blockers) {
    QByteArray block = newApfsObjectBlock(
        blockSize, kApfsFormatNxsbOid, position.xid, kApfsObjectTypeNxSuperblock);
    writeLe32(&block, kApfsObjectMagicOffset, kApfsMagicNxsb);
    writeLe32(&block, kApfsNxBlockSizeOffset, blockSize);
    writeLe64(&block, kApfsNxBlockCountOffset, blockCount);
    writeLe64(&block, 0x30, 1);
    writeLe64(&block, kApfsNxIncompatibleFeaturesOffset, kApfsContainerIncompatVersion2);
    std::copy(containerUuid.cbegin(), containerUuid.cend(), block.begin() + kApfsNxUuidOffset);
    writeLe64(&block, kApfsNxNextOidOffset, position.nextOid);
    writeLe64(&block, kApfsNxNextXidOffset, position.xid + 1);
    writeLe32(&block, kApfsNxXpDescBlocksOffset, kApfsFormatCheckpointDescBlocks);
    writeLe32(&block, kApfsNxXpDataBlocksOffset, kApfsFormatCheckpointDataBlocks);
    writeLe64(&block, kApfsNxXpDescBaseOffset, kApfsFormatCheckpointDescBaseBlock);
    writeLe64(&block, kApfsNxXpDataBaseOffset, kApfsFormatCheckpointDataBaseBlock);
    writeLe32(&block, kApfsNxXpDescNextOffset, position.descNext);
    writeLe32(&block, kApfsNxXpDataNextOffset, position.dataNext);
    writeLe32(&block, kApfsNxXpDescIndexOffset, position.descIndex);
    writeLe32(&block, kApfsNxXpDescLenOffset, 2);
    writeLe32(&block, kApfsNxXpDataIndexOffset, position.dataIndex);
    writeLe32(&block, kApfsNxXpDataLenOffset, position.dataLen);
    writeLe64(&block, kApfsNxSpacemanOidOffset, kApfsFormatSpacemanOid);
    writeLe64(&block, kApfsNxReaperOidOffset, kApfsFormatReaperOid);
    writeLe64(&block, kApfsNxEphemeralInfoOffset, apfsNxEphemeralInfoValue(blockCount, blockSize));
    writeLe64(&block, kApfsNxOmapOidOffset, position.omapOid);
    writeLe32(&block, kApfsNxMaxFileSystemsOffset, apfsMaxFileSystems(blockCount, blockSize));
    writeLe64(&block, kApfsNxFsOidArrayOffset, position.fsOid);
    stampApfsObjectBlock(&block, blockers);
    return block;
}

struct ApfsCheckpointMapEntry {
    uint32_t type{0};
    uint32_t subtype{0};
    uint64_t oid{0};
    uint64_t paddr{0};
};

QByteArray buildCheckpointMapBlock(uint32_t blockSize,
                                   uint64_t mapBlock,
                                   uint64_t xid,
                                   const QVector<ApfsCheckpointMapEntry>& entries,
                                   QStringList* blockers) {
    // Physical object: OID equals the block address. Single LAST map per
    // checkpoint resolving every ephemeral object of that transaction.
    QByteArray block = newApfsObjectBlock(blockSize, mapBlock, xid, kApfsObjectTypeCheckpointMap);
    writeLe32(&block, kApfsCheckpointMapFlagsOffset, kApfsCheckpointMapLastFlag);
    writeLe32(&block, kApfsCheckpointMapCountOffset, static_cast<uint32_t>(entries.size()));
    qsizetype entry = kApfsCheckpointMapEntriesOffset;
    for (const auto& mapping : entries) {
        writeLe32(&block, entry, mapping.type);
        writeLe32(&block, entry + 4, mapping.subtype);
        writeLe32(&block, entry + kApfsCheckpointMapEntrySizeOffset, blockSize);
        writeLe64(&block, entry + kApfsCheckpointMapEntryOidOffset, mapping.oid);
        writeLe64(&block, entry + kApfsCheckpointMapEntryPaddrOffset, mapping.paddr);
        entry += kApfsCheckpointMapEntryBytes;
    }
    stampApfsObjectBlock(&block, blockers);
    return block;
}

QByteArray buildReaperBlock(uint32_t blockSize, uint64_t xid, QStringList* blockers) {
    QByteArray block =
        newApfsObjectBlock(blockSize, kApfsFormatReaperOid, xid, kApfsObjectTypeReaper);
    // Fresh-container reaper, byte-matched against newfs_apfs output: no
    // attached reap lists, BHM flag, and the state-buffer size field.
    writeLe64(&block, kApfsReaperNextReapIdOffset, 1);
    writeLe32(&block, kApfsReaperFlagsOffset, kApfsReaperBhmFlag);
    writeLe32(&block, 0x6C, 3984);
    stampApfsObjectBlock(&block, blockers);
    return block;
}

QByteArray buildFreeQueueTreeBlock(uint32_t blockSize,
                                   uint64_t oid,
                                   uint64_t pendingFreePaddr,
                                   uint64_t pendingFreeCount,
                                   QStringList* blockers) {
    // Ephemeral free-queue B-tree root/leaf mirrored from newfs_apfs output:
    // node flags ROOT|LEAF|FIXED_KV_ALIGN (0x0007), 576-byte TOC, info flags
    // EPHEMERAL|ALLOWS_GHOSTS|SEQUENTIAL_INSERT (0x0E), 16-byte keys
    // {xid, paddr}, 8-byte count values. Fresh queues carry one record
    // scheduling the corresponding ghost blocks for reclamation.
    QByteArray block =
        newApfsObjectBlock(blockSize, oid, kApfsFormatXid, kApfsObjectTypeBtreeEphemeral);
    writeLe32(&block, kApfsObjectSubtypeOffset, kApfsObjectSubtypeFreeQueue);
    writeLe16(&block, kApfsBtreeNodeFlagsOffset, 0x0007);
    writeLe32(&block, kApfsBtreeNodeCountOffset, 1);
    writeLe16(&block, kApfsBtreeNodeTableOffsetOffset, 0);
    writeLe16(&block, kApfsBtreeNodeTableLengthOffset, 576);
    const qsizetype infoOffset = static_cast<qsizetype>(blockSize) - kApfsBtreeInfoBytes;
    const qsizetype keyAreaStart = kApfsBtreeNodeHeaderBytes + 576;
    const uint16_t freeLength = static_cast<uint16_t>(infoOffset - keyAreaStart - 16 - 8);
    writeLe16(&block, 0x2C, 16);
    writeLe16(&block, 0x2E, freeLength);
    writeLe16(&block, 0x30, 0xFFFF);
    writeLe16(&block, 0x32, 0);
    writeLe16(&block, 0x34, 0xFFFF);
    writeLe16(&block, 0x36, 0);
    writeLe16(&block, kApfsBtreeNodeHeaderBytes, 0);
    writeLe16(&block, kApfsBtreeNodeHeaderBytes + 2, 8);
    writeLe64(&block, keyAreaStart, kApfsFormatXid);
    writeLe64(&block, keyAreaStart + 8, pendingFreePaddr);
    writeLe64(&block, infoOffset - 8, pendingFreeCount);
    writeLe32(&block, infoOffset + kApfsBtreeInfoFlagsOffset, 0x00'00'00'0E);
    writeLe32(&block, infoOffset + kApfsBtreeInfoNodeSizeOffset, blockSize);
    writeLe32(&block, infoOffset + kApfsBtreeInfoKeySizeOffset, 16);
    writeLe32(&block, infoOffset + kApfsBtreeInfoValueSizeOffset, 8);
    writeLe32(&block, infoOffset + 16, 16);
    writeLe32(&block, infoOffset + 20, 8);
    writeLe64(&block, infoOffset + kApfsBtreeInfoKeyCountOffset, 1);
    writeLe64(&block, infoOffset + kApfsBtreeInfoNodeCountOffset, 1);
    stampApfsObjectBlock(&block, blockers);
    return block;
}

// Physical-extent-reference tree carrying one j_phys_ext record per file so
// fsck_apfs credits the file's data blocks during its space accounting.
// Key: physical_block_addr | (OBJ_TYPE_PHYSEXT << 60). Value: a 20-byte
// j_phys_ext_val { len_and_kind = block_count | (APFS_KIND_NEW << 60),
// owning_obj_id, refcnt }.
QByteArray buildExtentRefTreeBlock(uint32_t blockSize,
                                   const QVector<ApfsRootFilePayload>& files,
                                   QStringList* blockers) {
    constexpr uint64_t kApfsObjTypePhysExt = 2;
    constexpr uint64_t kApfsKindNew = 1;
    QByteArray block = newApfsObjectBlock(
        blockSize, kApfsFormatExtentRefTreeBlock, kApfsFormatXid, kApfsObjectTypeBtreePhysical);
    writeLe32(&block, kApfsObjectSubtypeOffset, kApfsObjectSubtypeExtentRef);
    writeLe16(&block, kApfsBtreeNodeFlagsOffset, 0x0003);

    struct PhysExtRecord {
        uint64_t paddr{0};
        uint64_t blockCount{0};
        uint64_t owner{0};
    };
    QVector<PhysExtRecord> records;
    for (const auto& file : files) {
        const uint64_t blockCount = roundedBlockCount(static_cast<uint64_t>(file.data.size()),
                                                      blockSize);
        if (blockCount == 0) {
            continue;
        }
        records.append({file.dataStartBlock, blockCount, file.fileId});
    }
    std::sort(records.begin(),
              records.end(),
              [](const PhysExtRecord& left, const PhysExtRecord& right) {
                  return left.paddr < right.paddr;
              });

    constexpr qsizetype kExtRefKeyBytes = 8;
    constexpr qsizetype kExtRefValueBytes = 20;
    const qsizetype tocLength = std::max<qsizetype>(
        64, ((records.size() * kApfsBtreeVariableTocEntryBytes + 63) / 64) * 64);
    writeLe32(&block, kApfsBtreeNodeCountOffset, static_cast<uint32_t>(records.size()));
    writeLe16(&block, kApfsBtreeNodeTableOffsetOffset, 0);
    writeLe16(&block, kApfsBtreeNodeTableLengthOffset, static_cast<uint16_t>(tocLength));
    const qsizetype keyAreaStart = kApfsBtreeNodeHeaderBytes + tocLength;
    const qsizetype valueAreaEnd = static_cast<qsizetype>(blockSize) - kApfsBtreeInfoBytes;
    qsizetype keyCursor = 0;
    qsizetype valueBackCursor = 0;
    for (qsizetype index = 0; index < records.size(); ++index) {
        const auto& record = records.at(index);
        const qsizetype toc = kApfsBtreeNodeHeaderBytes + index * kApfsBtreeVariableTocEntryBytes;
        writeLe16(&block, toc, static_cast<uint16_t>(keyCursor));
        writeLe16(&block, toc + kApfsBtreeVariableTocKeyLengthOffset, kExtRefKeyBytes);
        valueBackCursor += kExtRefValueBytes;
        writeLe16(&block,
                  toc + kApfsBtreeVariableTocValueOffset,
                  static_cast<uint16_t>(valueBackCursor));
        writeLe16(&block, toc + kApfsBtreeVariableTocValueLengthOffset, kExtRefValueBytes);
        const qsizetype key = keyAreaStart + keyCursor;
        writeLe64(&block, key, record.paddr | (kApfsObjTypePhysExt << kApfsObjTypeShift));
        const qsizetype value = valueAreaEnd - valueBackCursor;
        writeLe64(&block, value, record.blockCount | (kApfsKindNew << kApfsObjTypeShift));
        writeLe64(&block, value + 8, record.owner);
        writeLe32(&block, value + 16, 1);
        keyCursor += kExtRefKeyBytes;
    }
    writeLe16(&block, 0x2C, static_cast<uint16_t>(keyCursor));
    writeLe16(&block,
              0x2E,
              static_cast<uint16_t>(valueAreaEnd - keyAreaStart - keyCursor - valueBackCursor));
    writeLe16(&block, 0x30, 0xFFFF);
    writeLe16(&block, 0x32, 0);
    writeLe16(&block, 0x34, 0xFFFF);
    writeLe16(&block, 0x36, 0);
    const qsizetype infoOffset = valueAreaEnd;
    writeLe32(&block, infoOffset + kApfsBtreeInfoFlagsOffset, 0x00'00'00'52);
    writeLe32(&block, infoOffset + kApfsBtreeInfoNodeSizeOffset, blockSize);
    if (!records.isEmpty()) {
        writeLe32(&block, infoOffset + 16, kExtRefKeyBytes);
        writeLe32(&block, infoOffset + 20, kExtRefValueBytes);
    }
    writeLe64(&block,
              infoOffset + kApfsBtreeInfoKeyCountOffset,
              static_cast<uint64_t>(records.size()));
    writeLe64(&block, infoOffset + kApfsBtreeInfoNodeCountOffset, 1);
    stampApfsObjectBlock(&block, blockers);
    return block;
}

QByteArray buildEmptyVariableTreeBlock(uint32_t blockSize,
                                       uint64_t paddrOid,
                                       uint32_t subtype,
                                       QStringList* blockers) {
    // Empty physical root/leaf B-tree with variable-size keys (extent-ref and
    // snapshot-metadata trees on a fresh volume): node flags ROOT|LEAF
    // (0x0003), 64-byte TOC, info flags 0x52, zero key/value sizes.
    QByteArray block =
        newApfsObjectBlock(blockSize, paddrOid, kApfsFormatXid, kApfsObjectTypeBtreePhysical);
    writeLe32(&block, kApfsObjectSubtypeOffset, subtype);
    writeLe16(&block, kApfsBtreeNodeFlagsOffset, 0x0003);
    writeLe16(&block, kApfsBtreeNodeTableOffsetOffset, 0);
    writeLe16(&block, kApfsBtreeNodeTableLengthOffset, 64);
    const qsizetype infoOffset = static_cast<qsizetype>(blockSize) - kApfsBtreeInfoBytes;
    const uint16_t freeLength = static_cast<uint16_t>(infoOffset - kApfsBtreeNodeHeaderBytes - 64);
    writeLe16(&block, 0x2C, 0);
    writeLe16(&block, 0x2E, freeLength);
    writeLe16(&block, 0x30, 0xFFFF);
    writeLe16(&block, 0x32, 0);
    writeLe16(&block, 0x34, 0xFFFF);
    writeLe16(&block, 0x36, 0);
    writeLe32(&block, infoOffset + kApfsBtreeInfoFlagsOffset, 0x00'00'00'52);
    writeLe32(&block, infoOffset + kApfsBtreeInfoNodeSizeOffset, blockSize);
    writeLe64(&block, infoOffset + kApfsBtreeInfoNodeCountOffset, 1);
    stampApfsObjectBlock(&block, blockers);
    return block;
}

QByteArray buildIpBitmapBlock(uint32_t blockSize, uint8_t usageBits) {
    // Raw internal-pool usage bitmap: the ghost slot marks the ghost
    // chunk-info block and bitmap (0x3); the live slot additionally marks the
    // live pair (0xF).
    QByteArray block(static_cast<qsizetype>(blockSize), '\0');
    block[0] = static_cast<char>(usageBits);
    return block;
}

struct ApfsSpacemanParams {
    uint32_t blockSize;
    uint64_t blockCount;
    uint64_t reservedBlocks;
    uint64_t xid;
    bool genesis;
};

QByteArray buildSpacemanBlock(const ApfsSpacemanParams& params, QStringList* blockers) {
    const auto [blockSize, blockCount, reservedBlocks, xid, genesis] = params;
    QByteArray block =
        newApfsObjectBlock(blockSize, kApfsFormatSpacemanOid, xid, kApfsObjectTypeSpaceman);
    const uint64_t freeBlocks = blockCount > reservedBlocks ? blockCount - reservedBlocks : 0;
    const uint64_t chunkCount = (blockCount + kApfsSpacemanBlocksPerChunk - 1) /
                                kApfsSpacemanBlocksPerChunk;
    writeLe32(&block, kApfsSpacemanBlockSizeOffset, blockSize);
    writeLe32(&block, kApfsSpacemanBlocksPerChunkOffset, kApfsSpacemanBlocksPerChunk);
    writeLe32(&block, kApfsSpacemanChunksPerCibOffset, kApfsSpacemanChunksPerCib);
    writeLe32(&block, kApfsSpacemanCibsPerCabOffset, kApfsSpacemanCibsPerCab);
    writeLe64(&block,
              kApfsSpacemanMainDeviceOffset + kApfsSpacemanDeviceBlockCountOffset,
              blockCount);
    writeLe64(&block,
              kApfsSpacemanMainDeviceOffset + kApfsSpacemanDeviceChunkCountOffset,
              chunkCount);
    writeLe32(&block, kApfsSpacemanMainDeviceOffset + kApfsSpacemanDeviceCibCountOffset, 1);
    writeLe64(&block,
              kApfsSpacemanMainDeviceOffset + kApfsSpacemanDeviceFreeCountOffset,
              freeBlocks);
    writeLe32(&block,
              kApfsSpacemanMainDeviceOffset + kApfsSpacemanDeviceAddrOffsetOffset,
              static_cast<uint32_t>(kApfsSpacemanCibAddrArrayOffset));
    writeLe32(&block, kApfsSpacemanFlagsOffset, kApfsSpacemanFlagVersioned);
    writeLe32(&block, kApfsSpacemanIpBmTxMultiplierOffset, kApfsSpacemanIpBmTxMultiplier);
    // Internal-pool size scales with the allocator structures apfsck/fsck_apfs
    // require: ip_block_count = 3 * (chunk_count + cib_count + cab_count). One
    // CIB (no CAB) covers <=126 chunks, so this is 3 * (chunk_count + 1).
    // Validated against Apple newfs_apfs output (1-chunk 6; 8-chunk 27).
    const uint64_t ipBlockCount = 3 * (chunkCount + 1);
    writeLe64(&block, kApfsSpacemanIpBlockCountOffset, ipBlockCount);
    writeLe32(&block, kApfsSpacemanIpBmSizeOffset, 1);
    writeLe32(&block, kApfsSpacemanIpBmBlockCountOffset, kApfsFormatIpBitmapBlocks);
    writeLe64(&block, kApfsSpacemanIpBmBaseOffset, kApfsFormatIpBitmapBaseBlock);
    writeLe64(&block, kApfsSpacemanIpBaseOffset, kApfsFormatIpBaseBlock);
    writeLe32(&block,
              kApfsSpacemanMainDeviceOffset + 0x30 + kApfsSpacemanDeviceAddrOffsetOffset,
              static_cast<uint32_t>(kApfsSpacemanCibAddrArrayOffset + 8));
    writeLe16(&block, kApfsSpacemanFqIpLimitOffset, ipFreeQueueNodeLimit(chunkCount));
    writeLe16(&block, kApfsSpacemanFqMainLimitOffset, mainFreeQueueNodeLimit(blockCount));
    // Internal-offset table and bitmap ring state mirrored from the reference
    // container (sm_ip_bitmap_offset 2520, free-next offset 2528, ring array
    // at 2536 with 0xFFFF in-use markers; ring version entry carries the xid).
    writeLe32(&block, 0x144, 2520);
    writeLe32(&block, 0x148, 2528);
    writeLe32(&block, 0x14C, 2536);
    writeLe32(&block, 0x150, 1);
    writeLe32(&block, 0x154, 2520);
    writeLe16(&block, 2520, static_cast<uint16_t>(xid));
    if (genesis) {
        // One bitmap-ring slot in use; free chain starts at slot 1 and the
        // tail wraps to the final slot.
        writeLe16(&block, kApfsSpacemanIpBmFreeHeadOffset, 1);
        writeLe16(&block, kApfsSpacemanIpBmFreeTailOffset, 15);
        writeLe16(&block, 2528, 0);
        writeLe16(&block, 2536, 0xFFFF);
        for (uint16_t index = 1; index < 15; ++index) {
            writeLe16(&block, 2536 + index * 2, static_cast<uint16_t>(index + 1));
        }
        writeLe16(&block, 2536 + 15 * 2, 0xFFFF);
        writeLe64(&block, kApfsSpacemanCibAddrArrayOffset, kApfsFormatGhostChunkInfoBlock);
    } else {
        // Two ring slots in use (genesis + live transactions); the live free
        // queues reference their ephemeral B-trees and carry the counts of
        // ghost blocks pending reclamation.
        writeLe16(&block, kApfsSpacemanIpBmFreeHeadOffset, 2);
        writeLe16(&block, kApfsSpacemanIpBmFreeTailOffset, 0);
        writeLe16(&block, 2528, 1);
        writeLe16(&block, 2536, 0xFFFF);
        writeLe16(&block, 2538, 0xFFFF);
        for (uint16_t index = 2; index < 15; ++index) {
            writeLe16(&block, 2536 + index * 2, static_cast<uint16_t>(index + 1));
        }
        writeLe64(&block, 0xC8, 2);
        writeLe64(&block, 0xD0, kApfsFormatFqIpTreeOid);
        writeLe64(&block, 0xD8, kApfsFormatXid);
        writeLe64(&block, 0xF0, 2);
        writeLe64(&block, 0xF8, kApfsFormatFqMainTreeOid);
        writeLe64(&block, 0x100, kApfsFormatXid);
        writeLe64(&block, kApfsSpacemanCibAddrArrayOffset, kApfsFormatChunkInfoBlock);
    }
    stampApfsObjectBlock(&block, blockers);
    return block;
}

struct ApfsChunkInfoParams {
    uint32_t blockSize;
    uint64_t blockCount;
    uint64_t reservedBlocks;
    uint64_t xid;
    uint64_t selfBlock;
    uint64_t bitmapBlock;
};

QByteArray buildChunkInfoBlock(const ApfsChunkInfoParams& params, QStringList* blockers) {
    const auto [blockSize, blockCount, reservedBlocks, xid, selfBlock, bitmapBlock] = params;
    QByteArray block = newApfsObjectBlock(blockSize, selfBlock, xid, kApfsObjectTypeChunkInfoBlock);
    const uint64_t chunkCount = (blockCount + kApfsSpacemanBlocksPerChunk - 1) /
                                kApfsSpacemanBlocksPerChunk;
    writeLe32(&block, kApfsChunkInfoCountOffset, static_cast<uint32_t>(chunkCount));
    for (uint64_t chunk = 0; chunk < chunkCount; ++chunk) {
        const qsizetype entry = kApfsChunkInfoEntriesOffset +
                                static_cast<qsizetype>(chunk) * kApfsChunkInfoEntryStride;
        const uint64_t chunkAddr = chunk * kApfsSpacemanBlocksPerChunk;
        const uint64_t chunkBlocks = qMin<uint64_t>(kApfsSpacemanBlocksPerChunk,
                                                    blockCount - chunkAddr);
        // Chunk 0 carries the reserved metadata prefix and the only allocation
        // bitmap; later chunks are fully free (bitmap_addr 0, genesis xid), the
        // layout Apple's newfs_apfs emits for a freshly formatted container.
        const bool first = chunk == 0;
        const uint64_t usedBlocks = first ? qMin(reservedBlocks, chunkBlocks) : 0;
        writeLe64(&block, entry, first ? xid : kApfsFormatGenesisXid);
        writeLe64(&block, entry + kApfsChunkInfoEntryAddrOffset, chunkAddr);
        writeLe32(&block,
                  entry + kApfsChunkInfoEntryBlockCountOffset,
                  static_cast<uint32_t>(chunkBlocks));
        writeLe32(&block,
                  entry + kApfsChunkInfoEntryFreeCountOffset,
                  static_cast<uint32_t>(chunkBlocks - usedBlocks));
        writeLe64(&block, entry + kApfsChunkInfoEntryBitmapAddrOffset, first ? bitmapBlock : 0);
    }
    stampApfsObjectBlock(&block, blockers);
    return block;
}

QByteArray buildChunkBitmapBlock(uint32_t blockSize, uint64_t reservedBlocks) {
    // Raw allocation bitmap (no object header/checksum): one bit per block,
    // marking the contiguous reserved/metadata prefix as used.
    QByteArray block(static_cast<qsizetype>(blockSize), '\0');
    for (uint64_t index = 0; index < reservedBlocks; ++index) {
        block[static_cast<qsizetype>(index / 8)] |= static_cast<char>(1 << (index % 8));
    }
    return block;
}

struct ApfsObjectMapParams {
    uint32_t blockSize;
    uint64_t oid;
    uint64_t treeBlock;
    uint64_t xid;
    uint64_t omapFlags;
};

QByteArray buildObjectMapBlock(const ApfsObjectMapParams& params, QStringList* blockers) {
    const auto [blockSize, oid, treeBlock, xid, omapFlags] = params;
    QByteArray block = newApfsObjectBlock(blockSize, oid, xid, kApfsObjectTypeObjectMap);
    writeLe64(&block, 0x20, omapFlags);
    writeLe32(&block, 0x28, kApfsObjectTypeBtreePhysical);
    writeLe32(&block, 0x2C, kApfsObjectTypeBtreePhysical);
    writeLe64(&block, kApfsOmapTreeOidOffset, treeBlock);
    stampApfsObjectBlock(&block, blockers);
    return block;
}

struct ApfsVolumeSuperblockFields {
    uint32_t blockSize{0};
    QString volumeName;
    QByteArray volumeUuid;
    uint64_t allocatedBlocks{0};
    uint64_t nextObjectId{kApfsFirstUserObjectId};
    uint64_t fileCount{0};
    uint64_t directoryCount{0};
    uint64_t volumeOmapBlock{kApfsFormatVolumeOmapBlock};
    uint64_t extentRefTreeBlock{kApfsFormatExtentRefTreeBlock};
    uint64_t snapMetaTreeBlock{kApfsFormatSnapMetaTreeBlock};
};

QByteArray buildVolumeSuperblock(const ApfsVolumeSuperblockFields& fields, QStringList* blockers) {
    QByteArray block = newApfsObjectBlock(
        fields.blockSize, kApfsFormatVolumeOid, kApfsFormatXid, kApfsObjectTypeFs);
    writeLe32(&block, kApfsObjectMagicOffset, kApfsMagicApsb);
    // Feature words, meta-crypto state, and tree-type fields byte-matched
    // against newfs_apfs output (case-insensitive incompatible flag, defrag
    // features, physical omap/extent-ref tree types).
    writeLe64(&block, 0x28, 0x6);
    writeLe64(&block, kApfsVolumeIncompatibleFeaturesOffset, 1);
    writeLe64(&block, 0x60, 0x5);
    writeLe64(&block, 0x68, 0x17'41'01'58'00'00'00'06ULL);
    writeLe64(&block, 0x70, 0x00'00'00'02'00'00'00'01ULL);
    writeLe64(&block, 0x78, 0x40'00'00'02'40'00'00'02ULL);
    writeLe64(&block, 0xB0, fields.nextObjectId);
    writeLe64(&block, 0xB8, fields.fileCount);
    writeLe64(&block, 0xC0, fields.directoryCount);
    // Unencrypted-volume flag, modification timestamp, formatter identity,
    // first document ID, and trailing tree-type/extended fields mirrored from
    // newfs_apfs output (fsck enforces apfs_next_doc_id >= MIN_DOC_ID).
    writeLe64(&block, 0x100, 0x18'B8'30'5B'1F'32'81'92ULL);
    writeLe64(&block, kApfsVolumeFsFlagsOffset, 1);
    writeAscii(&block, 0x110, QByteArrayLiteral("S.A.K. Utility APFS writer"), 32);
    writeLe64(&block, 0x130, 0x18'B8'30'5B'1A'D4'FB'FBULL);
    writeLe64(&block, 0x138, kApfsFormatXid);
    writeLe32(&block, 0x3C0, 3);
    writeLe32(&block, 0x410, 0x40'00'00'02);
    writeLe32(&block, 0x414, 0x40'00'00'02);
    writeLe64(&block, 0x420, 0x2);
    writeLe32(&block, 0x428, 0x10);
    writeLe32(&block, 0x42C, 0x2);
    writeLe64(&block, 0x450, 0x2);
    writeLe64(&block, kApfsVolumeAllocatedBlockCountOffset, fields.allocatedBlocks);
    writeLe64(&block, kApfsVolumeOmapOidOffset, fields.volumeOmapBlock);
    writeLe64(&block, kApfsVolumeRootTreeOidOffset, kApfsFormatRootTreeOid);
    writeLe64(&block, 0x90, fields.extentRefTreeBlock);
    writeLe64(&block, kApfsVolumeSnapMetaTreeOidOffset, fields.snapMetaTreeBlock);
    std::copy(fields.volumeUuid.cbegin(),
              fields.volumeUuid.cend(),
              block.begin() + kApfsVolumeUuidOffset);
    writeAscii(&block,
               kApfsVolumeNameOffset,
               fields.volumeName.toUtf8(),
               std::min<qsizetype>(kApfsVolumeNameBytes, fields.volumeName.toUtf8().size() + 1));
    stampApfsObjectBlock(&block, blockers);
    return block;
}

bool writeBlock(QIODevice* device,
                uint64_t blockIndex,
                uint32_t blockSize,
                const QByteArray& block,
                QStringList* blockers) {
    if (!device || block.size() != static_cast<qsizetype>(blockSize)) {
        blockers->append(QStringLiteral("APFS format block has invalid size"));
        return false;
    }
    const uint64_t offset = blockIndex * static_cast<uint64_t>(blockSize);
    if (offset > static_cast<uint64_t>(std::numeric_limits<qint64>::max()) ||
        !device->seek(static_cast<qint64>(offset)) || device->write(block) != block.size()) {
        blockers->append(QStringLiteral("Unable to write APFS format block %1: %2")
                             .arg(blockIndex)
                             .arg(device->errorString()));
        return false;
    }
    return true;
}

QString fileSha256Hex(const QString& path, QStringList* blockers) {
    QString openError;
    auto device = openFileOrRawDeviceReadOnly(path, &openError);
    if (!device) {
        blockers->append(
            QStringLiteral("Unable to reopen APFS target for hashing: %1").arg(openError));
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(device.get())) {
        blockers->append(
            QStringLiteral("Unable to hash APFS target: %1").arg(device->errorString()));
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

QString bytesSha256Hex(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool imagePathIsSafeForCreate(const QString& imagePath, QStringList* blockers) {
    if (imagePath.trimmed().isEmpty()) {
        blockers->append(QStringLiteral("APFS format image path is required"));
        return false;
    }
    const QFileInfo info(imagePath);
    if (info.exists()) {
        blockers->append(
            QStringLiteral("APFS image-only format refuses to overwrite an existing image path"));
        return false;
    }
    if (!info.dir().exists()) {
        blockers->append(QStringLiteral("APFS format image parent directory does not exist"));
        return false;
    }
    return true;
}

bool isCertifiedApfsObjectType(uint32_t objectType) {
    static constexpr std::array kCertifiedObjectTypes{kApfsObjectTypeNxSuperblock,
                                                      kApfsObjectTypeBtree,
                                                      kApfsObjectTypeBtreePhysical,
                                                      kApfsObjectTypeObjectMap,
                                                      kApfsObjectTypeFs,
                                                      kApfsObjectTypeSpaceman,
                                                      kApfsObjectTypeCheckpointMap,
                                                      kApfsObjectTypeReaper,
                                                      kApfsObjectTypeBtreeEphemeral,
                                                      kApfsObjectTypeChunkInfoBlock};
    return std::ranges::find(kCertifiedObjectTypes, objectType) != kCertifiedObjectTypes.end();
}

bool readApfsRepairGeometry(QIODevice* image,
                            uint32_t* blockSize,
                            uint64_t* blockCount,
                            QStringList* blockers) {
    if (!image || !blockSize || !blockCount || !blockers) {
        return false;
    }
    if (!image->seek(0)) {
        blockers->append(QStringLiteral("Unable to read APFS container superblock"));
        return false;
    }
    QByteArray firstBlock(kSupportedApfsBlockSizeBytes, '\0');
    const qint64 bytesRead = image->read(firstBlock.data(), firstBlock.size());
    if (bytesRead < static_cast<qint64>(firstBlock.size())) {
        blockers->append(QStringLiteral("APFS repair image is smaller than one supported block"));
        return false;
    }
    if (le32(firstBlock, kApfsObjectMagicOffset) != kApfsMagicNxsb) {
        blockers->append(
            QStringLiteral("APFS repair requires a visible NXSB container superblock"));
        return false;
    }
    *blockSize = le32(firstBlock, kApfsNxBlockSizeOffset);
    *blockCount = le64(firstBlock, kApfsNxBlockCountOffset);
    if (*blockSize != kSupportedApfsBlockSizeBytes || *blockCount == 0) {
        blockers->append(QStringLiteral(
            "APFS repair certification currently supports only 4096-byte block images"));
        return false;
    }
    return true;
}

struct ApfsRepairGeometry {
    uint32_t blockSize{0};
    uint64_t blockCount{0};
};

struct ApfsRepairCounters {
    uint64_t scannedBlocks{0};
    uint64_t repairedBlocks{0};
};

bool readApfsRepairBlock(QIODevice* image,
                         const ApfsRepairGeometry& geometry,
                         uint64_t blockIndex,
                         QByteArray* block,
                         QStringList* blockers) {
    const uint64_t offset = blockIndex * static_cast<uint64_t>(geometry.blockSize);
    if (offset > static_cast<uint64_t>(std::numeric_limits<qint64>::max()) ||
        !image->seek(static_cast<qint64>(offset))) {
        blockers->append(QStringLiteral("Unable to seek APFS repair block %1").arg(blockIndex));
        return false;
    }
    if (image->read(block->data(), block->size()) != block->size()) {
        blockers->append(QStringLiteral("Unable to read APFS repair block %1").arg(blockIndex));
        return false;
    }
    return true;
}

bool writeApfsRepairBlock(QIODevice* image,
                          const ApfsRepairGeometry& geometry,
                          uint64_t blockIndex,
                          const QByteArray& block,
                          QStringList* blockers) {
    const uint64_t offset = blockIndex * static_cast<uint64_t>(geometry.blockSize);
    if (!image->seek(static_cast<qint64>(offset)) || image->write(block) != block.size()) {
        blockers->append(QStringLiteral("Unable to write APFS repair block %1: %2")
                             .arg(blockIndex)
                             .arg(image->errorString()));
        return false;
    }
    return true;
}

bool stampAndWriteApfsBlock(QIODevice* image,
                            const ApfsRepairGeometry& geometry,
                            uint64_t blockIndex,
                            QByteArray* block,
                            QStringList* blockers) {
    return stampApfsObjectBlock(block, blockers) &&
           writeApfsRepairBlock(image, geometry, blockIndex, *block, blockers);
}

// Live container checkpoint position parsed from the nx_superblock. The
// descriptor ring holds (checkpoint-map, nx_superblock) pairs; the data ring
// holds the ephemeral object set (spaceman, reaper, free-queue B-trees).
struct ApfsLiveCheckpoint {
    uint64_t xid{0};
    uint64_t descBase{0};
    uint32_t descBlocks{0};
    uint32_t descIndex{0};
    uint32_t descNext{0};
    uint64_t dataBase{0};
    uint32_t dataBlocks{0};
    uint32_t dataIndex{0};
    uint32_t dataNext{0};
};

ApfsLiveCheckpoint readLiveCheckpoint(const QByteArray& nxsb) {
    return {le64(nxsb, kApfsObjectXidOffset),
            le64(nxsb, kApfsNxXpDescBaseOffset),
            le32(nxsb, kApfsNxXpDescBlocksOffset),
            le32(nxsb, kApfsNxXpDescIndexOffset),
            le32(nxsb, kApfsNxXpDescNextOffset),
            le64(nxsb, kApfsNxXpDataBaseOffset),
            le32(nxsb, kApfsNxXpDataBlocksOffset),
            le32(nxsb, kApfsNxXpDataIndexOffset),
            le32(nxsb, kApfsNxXpDataNextOffset)};
}

struct ApfsCheckpointCommitContext {
    QIODevice* image{nullptr};
    ApfsRepairGeometry geometry;
    uint64_t dataBase{0};
    uint64_t newXid{0};
    uint64_t newDataIndex{0};
};

// Copy each live ephemeral object to the next data-ring slot, re-stamped at the
// new transaction id, and rewrite the checkpoint-map paddrs to the new slots.
bool reemitCheckpointEphemerals(const ApfsCheckpointCommitContext& ctx,
                                QByteArray* checkpointMap,
                                QStringList* blockers) {
    const uint32_t count = le32(*checkpointMap, kApfsCheckpointMapCountOffset);
    for (uint32_t index = 0; index < count; ++index) {
        const qsizetype entry = kApfsCheckpointMapEntriesOffset +
                                static_cast<qsizetype>(index) * kApfsCheckpointMapEntryBytes;
        const uint64_t oldPaddr = le64(*checkpointMap, entry + kApfsCheckpointMapEntryPaddrOffset);
        const uint64_t newPaddr = ctx.dataBase + ctx.newDataIndex + index;
        QByteArray object(ctx.geometry.blockSize, '\0');
        if (!readApfsRepairBlock(ctx.image, ctx.geometry, oldPaddr, &object, blockers)) {
            return false;
        }
        writeLe64(&object, kApfsObjectXidOffset, ctx.newXid);
        if (!stampAndWriteApfsBlock(ctx.image, ctx.geometry, newPaddr, &object, blockers)) {
            return false;
        }
        writeLe64(checkpointMap, entry + kApfsCheckpointMapEntryPaddrOffset, newPaddr);
    }
    return true;
}

void advanceNxSuperblockCheckpoint(QByteArray* nxsb,
                                   const ApfsLiveCheckpoint& live,
                                   uint64_t newXid,
                                   uint32_t ephemeralCount) {
    const uint32_t newDescIndex = live.descNext;
    const uint32_t newDataIndex = live.dataNext;
    writeLe64(nxsb, kApfsObjectXidOffset, newXid);
    writeLe64(nxsb, kApfsNxNextXidOffset, newXid + 1);
    writeLe32(nxsb, kApfsNxXpDescIndexOffset, newDescIndex);
    writeLe32(nxsb, kApfsNxXpDescLenOffset, 2);
    writeLe32(nxsb, kApfsNxXpDescNextOffset, (newDescIndex + 2) % live.descBlocks);
    writeLe32(nxsb, kApfsNxXpDataIndexOffset, newDataIndex);
    writeLe32(nxsb, kApfsNxXpDataLenOffset, ephemeralCount);
    writeLe32(nxsb, kApfsNxXpDataNextOffset, (newDataIndex + ephemeralCount) % live.dataBlocks);
}

struct ApfsInPlaceCheckpointResult {
    bool ok{false};
    uint64_t previous_xid{0};
    uint64_t new_xid{0};
    uint64_t checkpoint_map_block{0};
    uint64_t superblock_block{0};
};

struct ApfsCheckpointAdvanceRequest {
    QIODevice* image{nullptr};
    ApfsRepairGeometry geometry;
    QByteArray nxsb;
    ApfsLiveCheckpoint live;
    uint64_t newContainerOmap{0};  // 0 = carry the existing container object map forward
};

// Shared checkpoint-advance engine: read the live checkpoint-map, re-emit the
// ephemeral object set into the next data-ring slot, write a fresh
// checkpoint-map + nx_superblock into the next descriptor-ring slot (optionally
// re-pointing nx_omap_oid at a freshly COW'd container object map), and
// re-anchor block 0. Mirrors the Apple apfs.kext commit decoded in
// docs/APFS_A2_INPLACE_COMMIT_GROUND_TRUTH.md.
bool advanceCheckpoint(ApfsCheckpointAdvanceRequest request,
                       ApfsInPlaceCheckpointResult* result,
                       QStringList* blockers) {
    const ApfsRepairGeometry geometry = request.geometry;
    const ApfsLiveCheckpoint live = request.live;
    QByteArray checkpointMap(geometry.blockSize, '\0');
    if (!readApfsRepairBlock(
            request.image, geometry, live.descBase + live.descIndex, &checkpointMap, blockers)) {
        return false;
    }
    const uint32_t ephemeralCount = le32(checkpointMap, kApfsCheckpointMapCountOffset);
    if (live.dataNext + ephemeralCount > live.dataBlocks) {
        blockers->append(
            QStringLiteral("APFS in-place commit: checkpoint data ring would wrap (unsupported in "
                           "this increment)"));
        return false;
    }
    const uint64_t newXid = live.xid + 1;
    const uint64_t cpmBlock = live.descBase + live.descNext;
    const uint64_t nxsbBlock = cpmBlock + 1;
    const ApfsCheckpointCommitContext ctx{
        request.image, geometry, live.dataBase, newXid, live.dataNext};
    if (!reemitCheckpointEphemerals(ctx, &checkpointMap, blockers)) {
        return false;
    }
    writeLe64(&checkpointMap, kApfsObjectOidOffset, cpmBlock);
    writeLe64(&checkpointMap, kApfsObjectXidOffset, newXid);
    if (!stampAndWriteApfsBlock(request.image, geometry, cpmBlock, &checkpointMap, blockers)) {
        return false;
    }
    advanceNxSuperblockCheckpoint(&request.nxsb, live, newXid, ephemeralCount);
    if (request.newContainerOmap != 0) {
        writeLe64(&request.nxsb, kApfsNxOmapOidOffset, request.newContainerOmap);
    }
    if (!stampAndWriteApfsBlock(request.image, geometry, nxsbBlock, &request.nxsb, blockers) ||
        !writeApfsRepairBlock(
            request.image, geometry, kApfsFormatNxsbBlock, request.nxsb, blockers)) {
        return false;
    }
    *result = {true, live.xid, newXid, cpmBlock, nxsbBlock};
    return true;
}

// A2 increment 1: advance the container checkpoint by one transaction in place
// with no file-system COW (the volume content, object maps and spaceman free
// state carry forward unchanged). Proves the checkpoint-ring + checksum engine
// the file-mutating commit builds on.
bool commitInPlaceCheckpoint(QIODevice* image,
                             ApfsInPlaceCheckpointResult* result,
                             QStringList* blockers) {
    uint32_t blockSize = 0;
    uint64_t blockCount = 0;
    if (!readApfsRepairGeometry(image, &blockSize, &blockCount, blockers)) {
        return false;
    }
    const ApfsRepairGeometry geometry{blockSize, blockCount};
    QByteArray nxsb(blockSize, '\0');
    if (!readApfsRepairBlock(image, geometry, kApfsFormatNxsbBlock, &nxsb, blockers)) {
        return false;
    }
    const ApfsLiveCheckpoint live = readLiveCheckpoint(nxsb);
    return advanceCheckpoint({image, geometry, nxsb, live, 0}, result, blockers);
}

// Single-entry object-map B-tree leaf: the value paddr sits at the very end of
// the value area (blockSize - info(40) - value(16) + paddr offset(8)).
uint64_t readOmapSingleEntryPaddr(const QByteArray& omapTreeNode, uint32_t blockSize) {
    return le64(omapTreeNode,
                static_cast<qsizetype>(blockSize) - kApfsBtreeInfoBytes - kApfsObjectMapValueBytes +
                    kApfsOmapValuePaddrOffset);
}

struct ApfsLiveFsChain {
    uint64_t ctrOmapHdr{0};
    uint64_t ctrOmapTree{0};
    uint64_t volSb{0};
    uint64_t volOmapHdr{0};
    uint64_t volOmapTree{0};
    uint64_t rootTree{0};
};

// Walk the container object map -> volume superblock -> volume object map ->
// root file-system tree, recording the physical block of every node so the
// commit can copy-on-write the whole chain.
bool walkLiveFsChain(QIODevice* image,
                     const ApfsRepairGeometry& geometry,
                     uint64_t containerOmapHdr,
                     ApfsLiveFsChain* chain,
                     QStringList* blockers) {
    chain->ctrOmapHdr = containerOmapHdr;
    QByteArray node(geometry.blockSize, '\0');
    if (!readApfsRepairBlock(image, geometry, chain->ctrOmapHdr, &node, blockers)) {
        return false;
    }
    chain->ctrOmapTree = le64(node, kApfsOmapTreeOidOffset);
    if (!readApfsRepairBlock(image, geometry, chain->ctrOmapTree, &node, blockers)) {
        return false;
    }
    chain->volSb = readOmapSingleEntryPaddr(node, geometry.blockSize);
    if (!readApfsRepairBlock(image, geometry, chain->volSb, &node, blockers)) {
        return false;
    }
    chain->volOmapHdr = le64(node, kApfsVolumeOmapOidOffset);
    if (!readApfsRepairBlock(image, geometry, chain->volOmapHdr, &node, blockers)) {
        return false;
    }
    chain->volOmapTree = le64(node, kApfsOmapTreeOidOffset);
    if (!readApfsRepairBlock(image, geometry, chain->volOmapTree, &node, blockers)) {
        return false;
    }
    chain->rootTree = readOmapSingleEntryPaddr(node, geometry.blockSize);
    return true;
}

struct ApfsFreeBlockScan {
    QIODevice* image{nullptr};
    ApfsRepairGeometry geometry;
    uint64_t bitmapBlock{0};
    uint64_t startBlock{0};
    int count{0};
};

// Scan the raw allocation bitmap for `count` free blocks at or after
// `startBlock`, returning their addresses in ascending order.
bool findFreeBlocksInBitmap(const ApfsFreeBlockScan& scan,
                            QVector<uint64_t>* freeBlocks,
                            QStringList* blockers) {
    QByteArray bitmap(scan.geometry.blockSize, '\0');
    if (!readApfsRepairBlock(scan.image, scan.geometry, scan.bitmapBlock, &bitmap, blockers)) {
        return false;
    }
    for (uint64_t block = scan.startBlock;
         block < scan.geometry.blockCount && freeBlocks->size() < scan.count;
         ++block) {
        const bool used = (bitmap.at(static_cast<qsizetype>(block / 8)) >> (block % 8)) & 1;
        if (!used) {
            freeBlocks->append(block);
        }
    }
    if (freeBlocks->size() < scan.count) {
        blockers->append(QStringLiteral("APFS in-place file insert: not enough free blocks"));
        return false;
    }
    return true;
}

void flipChunkBitmapBits(QByteArray* bitmap,
                         const QVector<uint64_t>& freed,
                         const QVector<uint64_t>& allocated) {
    for (uint64_t block : freed) {
        (*bitmap)[static_cast<qsizetype>(block / 8)] &= static_cast<char>(~(1 << (block % 8)));
    }
    for (uint64_t block : allocated) {
        (*bitmap)[static_cast<qsizetype>(block / 8)] |= static_cast<char>(1 << (block % 8));
    }
}

struct ApfsCowFileInsert {
    QIODevice* image{nullptr};
    ApfsRepairGeometry geometry;
    uint64_t newXid{0};
    ApfsLiveFsChain live;
    // [rootTree, volOmapTree, volOmapHdr, volSb, ctrOmapTree, ctrOmapHdr]
    QVector<uint64_t> newBlocks;
    QVector<ApfsRootFilePayload> files;
};

// Copy-on-write the file-system metadata chain to the newly allocated blocks
// with the new file inserted: root tree -> volume object map -> volume
// superblock -> container object map, all stamped at the new transaction id.
bool writeFileInsertCowChain(const ApfsCowFileInsert& cow, QStringList* blockers) {
    const uint32_t bs = cow.geometry.blockSize;
    const uint64_t rootTree = cow.newBlocks.at(0);
    const uint64_t volOmapTree = cow.newBlocks.at(1);
    const uint64_t volOmapHdr = cow.newBlocks.at(2);
    const uint64_t volSb = cow.newBlocks.at(3);
    const uint64_t ctrOmapTree = cow.newBlocks.at(4);
    const uint64_t ctrOmapHdr = cow.newBlocks.at(5);

    QByteArray root = buildRootTreeBlock(bs, cow.files, {}, blockers);
    writeLe64(&root, kApfsObjectXidOffset, cow.newXid);
    if (!stampAndWriteApfsBlock(cow.image, cow.geometry, rootTree, &root, blockers)) {
        return false;
    }
    QByteArray volTree = buildObjectMapTreeBlock(
        bs, volOmapTree, {{kApfsFormatRootTreeOid, cow.newXid, rootTree}}, cow.newXid, blockers);
    QByteArray volHdr = buildObjectMapBlock({bs, volOmapHdr, volOmapTree, cow.newXid, 0}, blockers);
    if (!writeApfsRepairBlock(cow.image, cow.geometry, volOmapTree, volTree, blockers) ||
        !writeApfsRepairBlock(cow.image, cow.geometry, volOmapHdr, volHdr, blockers)) {
        return false;
    }
    QByteArray vol(bs, '\0');
    if (!readApfsRepairBlock(cow.image, cow.geometry, cow.live.volSb, &vol, blockers)) {
        return false;
    }
    writeLe64(&vol, kApfsVolumeOmapOidOffset, volOmapHdr);
    writeLe64(&vol, kApfsVolumeFileCountOffset, le64(vol, kApfsVolumeFileCountOffset) + 1);
    writeLe64(&vol, kApfsVolumeNextObjectIdOffset, le64(vol, kApfsVolumeNextObjectIdOffset) + 1);
    writeLe64(&vol, kApfsObjectXidOffset, cow.newXid);
    if (!stampAndWriteApfsBlock(cow.image, cow.geometry, volSb, &vol, blockers)) {
        return false;
    }
    QByteArray ctrTree = buildObjectMapTreeBlock(
        bs, ctrOmapTree, {{kApfsFormatVolumeOid, cow.newXid, volSb}}, cow.newXid, blockers);
    QByteArray ctrHdr = buildObjectMapBlock({bs, ctrOmapHdr, ctrOmapTree, cow.newXid, 1}, blockers);
    if (!writeApfsRepairBlock(cow.image, cow.geometry, ctrOmapTree, ctrTree, blockers) ||
        !writeApfsRepairBlock(cow.image, cow.geometry, ctrOmapHdr, ctrHdr, blockers)) {
        return false;
    }
    return true;
}

// A2 increment 2: insert one empty file into a generated container with a true
// in-place copy-on-write checkpoint commit. COW the fs-metadata chain to newly
// allocated blocks, swap the allocation bitmap (free the six old chain blocks,
// allocate the six new ones - a net-zero allocation so the spaceman free state
// is unchanged), then advance the checkpoint via the shared engine with
// nx_omap_oid re-pointed at the new container object map.
bool commitInPlaceFileInsert(QIODevice* image,
                             const QString& fileName,
                             ApfsInPlaceCheckpointResult* result,
                             QStringList* blockers) {
    uint32_t blockSize = 0;
    uint64_t blockCount = 0;
    if (!readApfsRepairGeometry(image, &blockSize, &blockCount, blockers)) {
        return false;
    }
    const ApfsRepairGeometry geometry{blockSize, blockCount};
    QByteArray nxsb(blockSize, '\0');
    if (!readApfsRepairBlock(image, geometry, kApfsFormatNxsbBlock, &nxsb, blockers)) {
        return false;
    }
    const ApfsLiveCheckpoint live = readLiveCheckpoint(nxsb);
    ApfsLiveFsChain chain;
    if (!walkLiveFsChain(image, geometry, le64(nxsb, kApfsNxOmapOidOffset), &chain, blockers)) {
        return false;
    }
    QVector<uint64_t> newBlocks;
    if (!findFreeBlocksInBitmap(
            {image, geometry, kApfsFormatChunkBitmapBlock, kApfsFormatSeedFileDataBlock, 6},
            &newBlocks,
            blockers)) {
        return false;
    }
    const uint64_t newXid = live.xid + 1;
    const QVector<ApfsRootFilePayload> files{{.fileName = fileName.trimmed(),
                                              .data = {},
                                              .parentDirectoryId = kApfsRootDirectoryId,
                                              .fileId = kApfsFirstUserObjectId,
                                              .privateId = kApfsFirstUserObjectId,
                                              .dataStartBlock = 0}};
    if (!writeFileInsertCowChain({image, geometry, newXid, chain, newBlocks, files}, blockers)) {
        return false;
    }
    QByteArray bitmap(blockSize, '\0');
    if (!readApfsRepairBlock(image, geometry, kApfsFormatChunkBitmapBlock, &bitmap, blockers)) {
        return false;
    }
    flipChunkBitmapBits(&bitmap,
                        {chain.rootTree,
                         chain.volOmapTree,
                         chain.volOmapHdr,
                         chain.volSb,
                         chain.ctrOmapTree,
                         chain.ctrOmapHdr},
                        newBlocks);
    if (!writeApfsRepairBlock(image, geometry, kApfsFormatChunkBitmapBlock, bitmap, blockers)) {
        return false;
    }
    return advanceCheckpoint({image, geometry, nxsb, live, newBlocks.at(5)}, result, blockers);
}

bool isRepairableApfsObjectBlock(const QByteArray& block) {
    const uint64_t oid = le64(block, kApfsObjectOidOffset);
    const uint64_t xid = le64(block, kApfsObjectXidOffset);
    return oid != 0 && xid != 0 && isCertifiedApfsObjectType(le32(block, kApfsObjectTypeOffset));
}

void appendGeneratedLayoutBlocker(bool valid, QStringView message, QStringList* blockers) {
    if (!valid) {
        blockers->append(message.toString());
    }
}

bool readGeneratedLayoutBlock(QIODevice* image,
                              const ApfsRepairGeometry& geometry,
                              uint64_t blockIndex,
                              QByteArray* block,
                              QStringList* blockers) {
    if (blockIndex >= geometry.blockCount) {
        blockers->append(
            QStringLiteral("APFS generated/minimal layout is missing required block %1")
                .arg(blockIndex));
        return false;
    }
    block->resize(static_cast<qsizetype>(geometry.blockSize));
    return readApfsRepairBlock(image, geometry, blockIndex, block, blockers);
}

struct GeneratedHeaderExpectation {
    const QByteArray* block{nullptr};
    uint64_t blockIndex{0};
    uint64_t expectedOid{0};
    uint32_t expectedType{0};
    uint32_t expectedSubtype{0};
    bool allowChecksumRepair{false};
    QStringList* blockers{nullptr};
};

bool appendGeneratedHeaderBlockers(const GeneratedHeaderExpectation& expected) {
    const int before = expected.blockers->size();
    appendGeneratedLayoutBlocker(
        le64(*expected.block, kApfsObjectOidOffset) == expected.expectedOid,
        QStringLiteral("APFS generated/minimal block %1 OID mismatch").arg(expected.blockIndex),
        expected.blockers);
    appendGeneratedLayoutBlocker(
        le64(*expected.block, kApfsObjectXidOffset) == kApfsFormatXid,
        QStringLiteral("APFS generated/minimal block %1 XID mismatch").arg(expected.blockIndex),
        expected.blockers);
    appendGeneratedLayoutBlocker(
        le32(*expected.block, kApfsObjectTypeOffset) == expected.expectedType,
        QStringLiteral("APFS generated/minimal block %1 type mismatch").arg(expected.blockIndex),
        expected.blockers);
    appendGeneratedLayoutBlocker(
        le32(*expected.block, kApfsObjectSubtypeOffset) == expected.expectedSubtype,
        QStringLiteral("APFS generated/minimal block %1 subtype mismatch").arg(expected.blockIndex),
        expected.blockers);
    if (!PartitionApfsWriter::verifyObjectChecksum(*expected.block) &&
        !(expected.allowChecksumRepair && isRepairableApfsObjectBlock(*expected.block))) {
        expected.blockers->append(
            QStringLiteral("APFS generated/minimal block %1 checksum is invalid")
                .arg(expected.blockIndex));
    }
    return expected.blockers->size() == before;
}

struct GeneratedObjectMapTreeExpectation {
    const QByteArray* block{nullptr};
    uint64_t blockIndex{0};
    uint64_t expectedMapOid{0};
    uint64_t expectedPhysicalBlock{0};
    bool allowChecksumRepair{false};
    QStringList* blockers{nullptr};
};

bool appendGeneratedObjectMapTreeBlockers(const GeneratedObjectMapTreeExpectation& expected) {
    const int before = expected.blockers->size();
    appendGeneratedHeaderBlockers({.block = expected.block,
                                   .blockIndex = expected.blockIndex,
                                   .expectedOid = expected.blockIndex,
                                   .expectedType = kApfsObjectTypeBtreePhysical,
                                   .expectedSubtype = 0x00'00'00'0B,
                                   .allowChecksumRepair = expected.allowChecksumRepair,
                                   .blockers = expected.blockers});
    const uint16_t flags = le16(*expected.block, kApfsBtreeNodeFlagsOffset);
    appendGeneratedLayoutBlocker(
        (flags & (kApfsBtreeNodeRoot | kApfsBtreeNodeLeaf | kApfsBtreeNodeFixedKvSize)) ==
            (kApfsBtreeNodeRoot | kApfsBtreeNodeLeaf | kApfsBtreeNodeFixedKvSize),
        QStringLiteral("APFS generated/minimal object-map tree block %1 flags mismatch")
            .arg(expected.blockIndex),
        expected.blockers);
    appendGeneratedLayoutBlocker(
        le32(*expected.block, kApfsBtreeNodeCountOffset) == 1,
        QStringLiteral("APFS generated/minimal object-map tree block %1 key count mismatch")
            .arg(expected.blockIndex),
        expected.blockers);
    const qsizetype keyOffset = kApfsBtreeNodeHeaderBytes + 448;
    const qsizetype valueOffset = static_cast<qsizetype>(kSupportedApfsBlockSizeBytes) -
                                  kApfsBtreeInfoBytes - kApfsObjectMapValueBytes;
    appendGeneratedLayoutBlocker(
        le64(*expected.block, keyOffset) == expected.expectedMapOid,
        QStringLiteral("APFS generated/minimal object-map tree block %1 map OID mismatch")
            .arg(expected.blockIndex),
        expected.blockers);
    appendGeneratedLayoutBlocker(
        le64(*expected.block, keyOffset + kApfsOmapKeyXidOffset) == kApfsFormatXid,
        QStringLiteral("APFS generated/minimal object-map tree block %1 map XID mismatch")
            .arg(expected.blockIndex),
        expected.blockers);
    appendGeneratedLayoutBlocker(
        le64(*expected.block, valueOffset + kApfsOmapValuePaddrOffset) ==
            expected.expectedPhysicalBlock,
        QStringLiteral("APFS generated/minimal object-map tree block %1 physical block mismatch")
            .arg(expected.blockIndex),
        expected.blockers);
    return expected.blockers->size() == before;
}

struct GeneratedRootTreeExpectation {
    const QByteArray* block{nullptr};
    bool allowChecksumRepair{false};
    QStringList* blockers{nullptr};
};

bool appendGeneratedRootTreeBlockers(const GeneratedRootTreeExpectation& expected) {
    const int before = expected.blockers->size();
    appendGeneratedHeaderBlockers({.block = expected.block,
                                   .blockIndex = kApfsFormatRootTreeBlock,
                                   .expectedOid = kApfsFormatRootTreeOid,
                                   .expectedType = kApfsObjectTypeBtree,
                                   .expectedSubtype = kApfsObjectSubtypeFsTree,
                                   .allowChecksumRepair = expected.allowChecksumRepair,
                                   .blockers = expected.blockers});
    const uint16_t flags = le16(*expected.block, kApfsBtreeNodeFlagsOffset);
    const uint32_t keys = le32(*expected.block, kApfsBtreeNodeCountOffset);
    appendGeneratedLayoutBlocker((flags & (kApfsBtreeNodeRoot | kApfsBtreeNodeLeaf)) ==
                                     (kApfsBtreeNodeRoot | kApfsBtreeNodeLeaf),
                                 QStringLiteral("APFS generated/minimal root tree flags mismatch"),
                                 expected.blockers);
    appendGeneratedLayoutBlocker(
        keys > 0 &&
            keys <= 1 + (kApfsWriteRootListingMaxEntries * kApfsGeneratedRootRecordsPerFile),
        QStringLiteral("APFS generated/minimal root tree key count is out of bounds"),
        expected.blockers);
    return expected.blockers->size() == before;
}

struct GeneratedApfsLayoutBlocks {
    QByteArray nxsb;
    QByteArray checkpointMap;
    QByteArray nxsbCopy;
    QByteArray containerOmap;
    QByteArray containerOmapTree;
    QByteArray volume;
    QByteArray volumeOmap;
    QByteArray volumeOmapTree;
    QByteArray rootTree;
    QByteArray spaceman;
    QByteArray reaper;
    QByteArray chunkInfo;
};

struct GeneratedApfsLayoutContext {
    QIODevice* image{nullptr};
    ApfsRepairGeometry geometry;
    QLatin1StringView purpose;
    bool allowChecksumRepair{false};
    QStringList* blockers{nullptr};
};

QString generatedApfsSingleChunkLimitBlocker(QLatin1StringView purpose, uint64_t blockCount) {
    const PartitionApfsContainerGeometry geometry =
        PartitionApfsWriter::computeContainerGeometry(blockCount);
    return QStringLiteral(
               "APFS %1 currently supports S.A.K.-generated single chunk-info-block containers "
               "(up to 126 spaceman chunks, 15.75 GiB); this %2-block target needs %3 spaceman "
               "chunk(s) across %4 chunk-info block(s)%5, which requires multi-CIB (CAB tier) "
               "spaceman emission and Apple fsck validation")
        .arg(purpose)
        .arg(blockCount)
        .arg(geometry.chunk_count)
        .arg(geometry.cib_count)
        .arg(geometry.multi_cib ? QStringLiteral(" (multi-CIB CAB tier)") : QString());
}

bool appendGeneratedGeometryBlocker(const GeneratedApfsLayoutContext& context) {
    if (context.geometry.blockSize != kSupportedApfsBlockSizeBytes ||
        context.geometry.blockCount < kApfsFormatSeedFileDataBlock) {
        context.blockers->append(
            QStringLiteral("APFS %1 requires a S.A.K.-generated/minimal 4096-byte block layout")
                .arg(context.purpose));
        return false;
    }
    if (context.geometry.blockCount > kGeneratedApfsSingleChunkMaxBlocks) {
        context.blockers->append(
            generatedApfsSingleChunkLimitBlocker(context.purpose, context.geometry.blockCount));
        return false;
    }
    return true;
}

bool readGeneratedApfsLayoutBlocks(const GeneratedApfsLayoutContext& context,
                                   GeneratedApfsLayoutBlocks* blocks) {
    const std::array<std::pair<uint64_t, QByteArray*>, 12> targets{{
        {kApfsFormatNxsbBlock, &blocks->nxsb},
        {kApfsFormatCheckpointMapBlock, &blocks->checkpointMap},
        {kApfsFormatCheckpointNxsbCopyBlock, &blocks->nxsbCopy},
        {kApfsFormatContainerOmapBlock, &blocks->containerOmap},
        {kApfsFormatContainerOmapTreeBlock, &blocks->containerOmapTree},
        {kApfsFormatVolumeSuperblockBlock, &blocks->volume},
        {kApfsFormatVolumeOmapBlock, &blocks->volumeOmap},
        {kApfsFormatVolumeOmapTreeBlock, &blocks->volumeOmapTree},
        {kApfsFormatRootTreeBlock, &blocks->rootTree},
        {kApfsFormatSpacemanBlock, &blocks->spaceman},
        {kApfsFormatReaperBlock, &blocks->reaper},
        {kApfsFormatChunkInfoBlock, &blocks->chunkInfo},
    }};
    for (const auto& [blockIndex, target] : targets) {
        if (!readGeneratedLayoutBlock(
                context.image, context.geometry, blockIndex, target, context.blockers)) {
            return false;
        }
    }
    return true;
}

void appendGeneratedNxsbBlockers(const GeneratedApfsLayoutContext& context,
                                 const GeneratedApfsLayoutBlocks& blocks) {
    appendGeneratedHeaderBlockers({.block = &blocks.nxsb,
                                   .blockIndex = kApfsFormatNxsbBlock,
                                   .expectedOid = kApfsFormatNxsbOid,
                                   .expectedType = kApfsObjectTypeNxSuperblock,
                                   .allowChecksumRepair = context.allowChecksumRepair,
                                   .blockers = context.blockers});
    appendGeneratedLayoutBlocker(le32(blocks.nxsb, kApfsObjectMagicOffset) == kApfsMagicNxsb,
                                 QStringLiteral("APFS generated/minimal NXSB magic mismatch"),
                                 context.blockers);
    appendGeneratedLayoutBlocker(le32(blocks.nxsb, kApfsNxBlockSizeOffset) ==
                                     context.geometry.blockSize,
                                 QStringLiteral("APFS generated/minimal block size mismatch"),
                                 context.blockers);
    appendGeneratedLayoutBlocker(le64(blocks.nxsb, kApfsNxBlockCountOffset) ==
                                     context.geometry.blockCount,
                                 QStringLiteral("APFS generated/minimal block count mismatch"),
                                 context.blockers);
    appendGeneratedLayoutBlocker(le64(blocks.nxsb, kApfsNxSpacemanOidOffset) ==
                                     kApfsFormatSpacemanOid,
                                 QStringLiteral("APFS generated/minimal spaceman OID mismatch"),
                                 context.blockers);
    appendGeneratedLayoutBlocker(le64(blocks.nxsb, kApfsNxOmapOidOffset) ==
                                     kApfsFormatContainerOmapOid,
                                 QStringLiteral("APFS generated/minimal object-map OID mismatch"),
                                 context.blockers);
    appendGeneratedLayoutBlocker(le32(blocks.nxsb, kApfsNxMaxFileSystemsOffset) ==
                                     apfsMaxFileSystems(context.geometry.blockCount,
                                                        context.geometry.blockSize),
                                 QStringLiteral("APFS generated/minimal max file systems mismatch"),
                                 context.blockers);
    appendGeneratedLayoutBlocker(le64(blocks.nxsb, kApfsNxFsOidArrayOffset) == kApfsFormatVolumeOid,
                                 QStringLiteral("APFS generated/minimal volume OID mismatch"),
                                 context.blockers);
}

struct ApfsCheckpointMapEntryExpect {
    uint32_t type;
    uint64_t oid;
    uint64_t paddr;
};

void appendCheckpointMapEntryBlocker(const GeneratedApfsLayoutContext& context,
                                     const QByteArray& map,
                                     qsizetype entry,
                                     const ApfsCheckpointMapEntryExpect& expect,
                                     const QString& message) {
    appendGeneratedLayoutBlocker(
        le32(map, entry) == expect.type &&
            le64(map, entry + kApfsCheckpointMapEntryOidOffset) == expect.oid &&
            le64(map, entry + kApfsCheckpointMapEntryPaddrOffset) == expect.paddr,
        message,
        context.blockers);
}

void appendGeneratedCheckpointBlockers(const GeneratedApfsLayoutContext& context,
                                       const GeneratedApfsLayoutBlocks& blocks) {
    appendGeneratedLayoutBlocker(
        le32(blocks.nxsb, kApfsNxXpDescBlocksOffset) == kApfsFormatCheckpointDescBlocks &&
            le64(blocks.nxsb, kApfsNxXpDescBaseOffset) == kApfsFormatCheckpointDescBaseBlock &&
            le32(blocks.nxsb, kApfsNxXpDescIndexOffset) == 2 &&
            le32(blocks.nxsb, kApfsNxXpDescLenOffset) == 2,
        QStringLiteral("APFS generated/minimal checkpoint descriptor geometry mismatch"),
        context.blockers);
    appendGeneratedLayoutBlocker(
        le32(blocks.nxsb, kApfsNxXpDataBlocksOffset) == kApfsFormatCheckpointDataBlocks &&
            le64(blocks.nxsb, kApfsNxXpDataBaseOffset) == kApfsFormatCheckpointDataBaseBlock &&
            le32(blocks.nxsb, kApfsNxXpDataIndexOffset) == 2 &&
            le32(blocks.nxsb, kApfsNxXpDataLenOffset) == 4,
        QStringLiteral("APFS generated/minimal checkpoint data geometry mismatch"),
        context.blockers);
    appendGeneratedLayoutBlocker(le64(blocks.nxsb, kApfsNxReaperOidOffset) == kApfsFormatReaperOid,
                                 QStringLiteral("APFS generated/minimal reaper OID mismatch"),
                                 context.blockers);
    appendGeneratedLayoutBlocker(le64(blocks.nxsb, kApfsNxEphemeralInfoOffset) ==
                                     apfsNxEphemeralInfoValue(context.geometry.blockCount,
                                                              context.geometry.blockSize),
                                 QStringLiteral("APFS generated/minimal ephemeral info mismatch"),
                                 context.blockers);
    appendGeneratedHeaderBlockers({.block = &blocks.checkpointMap,
                                   .blockIndex = kApfsFormatCheckpointMapBlock,
                                   .expectedOid = kApfsFormatCheckpointMapBlock,
                                   .expectedType = kApfsObjectTypeCheckpointMap,
                                   .allowChecksumRepair = context.allowChecksumRepair,
                                   .blockers = context.blockers});
    appendGeneratedLayoutBlocker(
        le32(blocks.checkpointMap, kApfsCheckpointMapFlagsOffset) == kApfsCheckpointMapLastFlag &&
            le32(blocks.checkpointMap, kApfsCheckpointMapCountOffset) == 4,
        QStringLiteral("APFS generated/minimal checkpoint map must be a single LAST map"),
        context.blockers);
    const qsizetype entry = kApfsCheckpointMapEntriesOffset;
    appendCheckpointMapEntryBlocker(
        context,
        blocks.checkpointMap,
        entry,
        {.type = kApfsObjectTypeSpaceman,
         .oid = kApfsFormatSpacemanOid,
         .paddr = kApfsFormatSpacemanBlock},
        QStringLiteral("APFS generated/minimal checkpoint map must resolve the space manager"));
    appendCheckpointMapEntryBlocker(
        context,
        blocks.checkpointMap,
        entry + kApfsCheckpointMapEntryBytes,
        {.type = kApfsObjectTypeReaper,
         .oid = kApfsFormatReaperOid,
         .paddr = kApfsFormatReaperBlock},
        QStringLiteral("APFS generated/minimal checkpoint map must resolve the reaper"));
    appendGeneratedHeaderBlockers({.block = &blocks.reaper,
                                   .blockIndex = kApfsFormatReaperBlock,
                                   .expectedOid = kApfsFormatReaperOid,
                                   .expectedType = kApfsObjectTypeReaper,
                                   .allowChecksumRepair = context.allowChecksumRepair,
                                   .blockers = context.blockers});
    appendGeneratedHeaderBlockers({.block = &blocks.chunkInfo,
                                   .blockIndex = kApfsFormatChunkInfoBlock,
                                   .expectedOid = kApfsFormatChunkInfoBlock,
                                   .expectedType = kApfsObjectTypeChunkInfoBlock,
                                   .allowChecksumRepair = context.allowChecksumRepair,
                                   .blockers = context.blockers});
    appendGeneratedLayoutBlocker(
        le64(blocks.chunkInfo, kApfsChunkInfoEntriesOffset + kApfsChunkInfoEntryBitmapAddrOffset) ==
            kApfsFormatChunkBitmapBlock,
        QStringLiteral("APFS generated/minimal chunk bitmap address mismatch"),
        context.blockers);
    appendGeneratedLayoutBlocker(
        blocks.nxsbCopy == blocks.nxsb,
        QStringLiteral("APFS generated/minimal checkpoint superblock copy must match block zero"),
        context.blockers);
}

void appendGeneratedOmapSnapshotStateBlockers(const QByteArray& omap,
                                              QStringView label,
                                              QStringList* blockers) {
    appendGeneratedLayoutBlocker(
        le32(omap, kApfsOmapSnapshotCountOffset) == 0 &&
            le64(omap, kApfsOmapSnapshotTreeOidOffset) == 0 &&
            le64(omap, kApfsOmapMostRecentSnapshotOffset) == 0,
        QStringLiteral("APFS generated/minimal %1 object map must not carry snapshot state")
            .arg(label),
        blockers);
    appendGeneratedLayoutBlocker(
        le64(omap, kApfsOmapPendingRevertMinOffset) == 0 &&
            le64(omap, kApfsOmapPendingRevertMaxOffset) == 0,
        QStringLiteral("APFS generated/minimal %1 object map must not carry pending revert state")
            .arg(label),
        blockers);
}

void appendGeneratedContainerMapBlockers(const GeneratedApfsLayoutContext& context,
                                         const GeneratedApfsLayoutBlocks& blocks) {
    appendGeneratedHeaderBlockers({.block = &blocks.containerOmap,
                                   .blockIndex = kApfsFormatContainerOmapBlock,
                                   .expectedOid = kApfsFormatContainerOmapOid,
                                   .expectedType = kApfsObjectTypeObjectMap,
                                   .allowChecksumRepair = context.allowChecksumRepair,
                                   .blockers = context.blockers});
    appendGeneratedLayoutBlocker(
        le64(blocks.containerOmap, kApfsOmapTreeOidOffset) == kApfsFormatContainerOmapTreeBlock,
        QStringLiteral("APFS generated/minimal container object-map tree mismatch"),
        context.blockers);
    appendGeneratedOmapSnapshotStateBlockers(blocks.containerOmap,
                                             QStringLiteral("container"),
                                             context.blockers);
    appendGeneratedObjectMapTreeBlockers({.block = &blocks.containerOmapTree,
                                          .blockIndex = kApfsFormatContainerOmapTreeBlock,
                                          .expectedMapOid = kApfsFormatVolumeOid,
                                          .expectedPhysicalBlock = kApfsFormatVolumeSuperblockBlock,
                                          .allowChecksumRepair = context.allowChecksumRepair,
                                          .blockers = context.blockers});
}

void appendGeneratedVolumeBlockers(const GeneratedApfsLayoutContext& context,
                                   const GeneratedApfsLayoutBlocks& blocks) {
    appendGeneratedHeaderBlockers({.block = &blocks.volume,
                                   .blockIndex = kApfsFormatVolumeSuperblockBlock,
                                   .expectedOid = kApfsFormatVolumeOid,
                                   .expectedType = kApfsObjectTypeFs,
                                   .allowChecksumRepair = context.allowChecksumRepair,
                                   .blockers = context.blockers});
    appendGeneratedLayoutBlocker(le32(blocks.volume, kApfsObjectMagicOffset) == kApfsMagicApsb,
                                 QStringLiteral("APFS generated/minimal APSB magic mismatch"),
                                 context.blockers);
    appendGeneratedLayoutBlocker(
        le64(blocks.volume, kApfsVolumeOmapOidOffset) == kApfsFormatVolumeOmapBlock,
        QStringLiteral("APFS generated/minimal volume object-map OID mismatch"),
        context.blockers);
    appendGeneratedLayoutBlocker(le64(blocks.volume, kApfsVolumeRootTreeOidOffset) ==
                                     kApfsFormatRootTreeOid,
                                 QStringLiteral("APFS generated/minimal root tree OID mismatch"),
                                 context.blockers);
    appendGeneratedLayoutBlocker(
        le64(blocks.volume, kApfsVolumeSnapMetaTreeOidOffset) == kApfsFormatSnapMetaTreeBlock,
        QStringLiteral("APFS generated/minimal snapshot-metadata tree OID mismatch"),
        context.blockers);
    appendGeneratedLayoutBlocker(le64(blocks.volume, 0x90) == kApfsFormatExtentRefTreeBlock,
                                 QStringLiteral(
                                     "APFS generated/minimal extent-ref tree OID mismatch"),
                                 context.blockers);
    appendGeneratedLayoutBlocker(
        le64(blocks.volume, kApfsVolumeRevertToXidOffset) == 0 &&
            le64(blocks.volume, kApfsVolumeRevertToSblockOidOffset) == 0,
        QStringLiteral("APFS generated/minimal volume must not carry revert metadata"),
        context.blockers);
    appendGeneratedLayoutBlocker(le64(blocks.volume, kApfsVolumeNumSnapshotsOffset) == 0,
                                 QStringLiteral(
                                     "APFS generated/minimal volume must not contain snapshots"),
                                 context.blockers);
    appendGeneratedLayoutBlocker(
        le64(blocks.volume, kApfsVolumeFsFlagsOffset) == 1,
        QStringLiteral("APFS generated/minimal volume flags must match the generated layout; "
                       "encrypted or protected volume state is blocked"),
        context.blockers);
    appendGeneratedHeaderBlockers({.block = &blocks.volumeOmap,
                                   .blockIndex = kApfsFormatVolumeOmapBlock,
                                   .expectedOid = kApfsFormatVolumeOmapOid,
                                   .expectedType = kApfsObjectTypeObjectMap,
                                   .allowChecksumRepair = context.allowChecksumRepair,
                                   .blockers = context.blockers});
    appendGeneratedLayoutBlocker(
        le64(blocks.volumeOmap, kApfsOmapTreeOidOffset) == kApfsFormatVolumeOmapTreeBlock,
        QStringLiteral("APFS generated/minimal volume object-map tree mismatch"),
        context.blockers);
    appendGeneratedOmapSnapshotStateBlockers(blocks.volumeOmap,
                                             QStringLiteral("volume"),
                                             context.blockers);
    appendGeneratedObjectMapTreeBlockers({.block = &blocks.volumeOmapTree,
                                          .blockIndex = kApfsFormatVolumeOmapTreeBlock,
                                          .expectedMapOid = kApfsFormatRootTreeOid,
                                          .expectedPhysicalBlock = kApfsFormatRootTreeBlock,
                                          .allowChecksumRepair = context.allowChecksumRepair,
                                          .blockers = context.blockers});
    appendGeneratedRootTreeBlockers({.block = &blocks.rootTree,
                                     .allowChecksumRepair = context.allowChecksumRepair,
                                     .blockers = context.blockers});
}

void appendGeneratedSpacemanBlockers(const GeneratedApfsLayoutContext& context,
                                     const GeneratedApfsLayoutBlocks& blocks) {
    appendGeneratedHeaderBlockers({.block = &blocks.spaceman,
                                   .blockIndex = kApfsFormatSpacemanBlock,
                                   .expectedOid = kApfsFormatSpacemanOid,
                                   .expectedType = kApfsObjectTypeSpaceman,
                                   .allowChecksumRepair = context.allowChecksumRepair,
                                   .blockers = context.blockers});
    appendGeneratedLayoutBlocker(
        le32(blocks.spaceman, kApfsSpacemanBlockSizeOffset) == context.geometry.blockSize,
        QStringLiteral("APFS generated/minimal spaceman block size mismatch"),
        context.blockers);
    const uint64_t allocatedBlocks = le64(blocks.volume, kApfsVolumeAllocatedBlockCountOffset);
    const uint64_t freeBlocks =
        le64(blocks.spaceman, kApfsSpacemanMainDeviceOffset + kApfsSpacemanDeviceFreeCountOffset);
    const uint64_t mainBlocks =
        le64(blocks.spaceman, kApfsSpacemanMainDeviceOffset + kApfsSpacemanDeviceBlockCountOffset);
    const uint64_t chunkCount =
        le64(blocks.spaceman, kApfsSpacemanMainDeviceOffset + kApfsSpacemanDeviceChunkCountOffset);
    const uint32_t cibCount =
        le32(blocks.spaceman, kApfsSpacemanMainDeviceOffset + kApfsSpacemanDeviceCibCountOffset);
    const uint32_t chunkInfoCount = le32(blocks.chunkInfo, kApfsChunkInfoCountOffset);
    const uint32_t chunkInfoBlocks =
        le32(blocks.chunkInfo, kApfsChunkInfoEntriesOffset + kApfsChunkInfoEntryBlockCountOffset);
    appendGeneratedLayoutBlocker(mainBlocks == context.geometry.blockCount,
                                 QStringLiteral(
                                     "APFS generated/minimal spaceman block count mismatch"),
                                 context.blockers);
    appendGeneratedLayoutBlocker(chunkCount == 1 && cibCount == 1 && chunkInfoCount == 1,
                                 QStringLiteral(
                                     "APFS generated/minimal spaceman chunk geometry exceeds "
                                     "the certified one-chunk layout"),
                                 context.blockers);
    appendGeneratedLayoutBlocker(
        chunkInfoBlocks == mainBlocks && chunkInfoBlocks <= kGeneratedApfsSingleChunkMaxBlocks,
        QStringLiteral("APFS generated/minimal chunk-info block count exceeds "
                       "the certified one-chunk layout"),
        context.blockers);
    appendGeneratedLayoutBlocker(allocatedBlocks >= kApfsFormatVolumeBaseAllocatedBlocks &&
                                     allocatedBlocks <= context.geometry.blockCount,
                                 QStringLiteral(
                                     "APFS generated/minimal allocated block count is invalid"),
                                 context.blockers);
    const uint64_t chunkFreeBlocks =
        le32(blocks.chunkInfo, kApfsChunkInfoEntriesOffset + kApfsChunkInfoEntryFreeCountOffset);
    appendGeneratedLayoutBlocker(
        freeBlocks == chunkFreeBlocks &&
            freeBlocks + kApfsFormatSeedFileDataBlock <= context.geometry.blockCount,
        QStringLiteral("APFS generated/minimal free-space accounting mismatch"),
        context.blockers);
}

bool appendGeneratedApfsLayoutBlockers(QIODevice* image,
                                       const ApfsRepairGeometry& geometry,
                                       QLatin1StringView purpose,
                                       bool allowChecksumRepair,
                                       QStringList* blockers) {
    const int before = blockers->size();
    const GeneratedApfsLayoutContext context{.image = image,
                                             .geometry = geometry,
                                             .purpose = purpose,
                                             .allowChecksumRepair = allowChecksumRepair,
                                             .blockers = blockers};
    if (!appendGeneratedGeometryBlocker(context)) {
        return false;
    }
    GeneratedApfsLayoutBlocks blocks;
    if (!readGeneratedApfsLayoutBlocks(context, &blocks)) {
        return false;
    }
    appendGeneratedNxsbBlockers(context, blocks);
    appendGeneratedCheckpointBlockers(context, blocks);
    appendGeneratedContainerMapBlockers(context, blocks);
    appendGeneratedVolumeBlockers(context, blocks);
    appendGeneratedSpacemanBlockers(context, blocks);
    return blockers->size() == before;
}

struct ApfsRepairBlockContext {
    QIODevice* image{nullptr};
    ApfsRepairGeometry geometry;
    uint64_t blockIndex{0};
    QByteArray* block{nullptr};
};

bool repairApfsObjectChecksumBlock(const ApfsRepairBlockContext& context,
                                   bool* repaired,
                                   QStringList* blockers) {
    *repaired = false;
    if (!readApfsRepairBlock(
            context.image, context.geometry, context.blockIndex, context.block, blockers)) {
        return false;
    }
    if (!isRepairableApfsObjectBlock(*context.block) ||
        PartitionApfsWriter::verifyObjectChecksum(*context.block)) {
        return true;
    }
    if (!PartitionApfsWriter::stampObjectChecksum(context.block)) {
        blockers->append(QStringLiteral("Unable to stamp APFS repair checksum block %1")
                             .arg(context.blockIndex));
        return false;
    }
    *repaired = true;
    return writeApfsRepairBlock(
        context.image, context.geometry, context.blockIndex, *context.block, blockers);
}

bool repairApfsObjectChecksumBlocks(QIODevice* image,
                                    const ApfsRepairGeometry& geometry,
                                    ApfsRepairCounters* counters,
                                    QStringList* blockers) {
    if (!image || !counters || !blockers) {
        return false;
    }
    *counters = {};
    QByteArray block(static_cast<qsizetype>(geometry.blockSize), '\0');
    for (uint64_t blockIndex = 0; blockIndex < geometry.blockCount; ++blockIndex) {
        bool repaired = false;
        if (!repairApfsObjectChecksumBlock(
                {.image = image, .geometry = geometry, .blockIndex = blockIndex, .block = &block},
                &repaired,
                blockers)) {
            return false;
        }
        ++counters->scannedBlocks;
        counters->repairedBlocks += repaired ? 1 : 0;
    }
    return true;
}

bool repairGeneratedApfsObjectChecksumBlocks(QIODevice* image,
                                             const ApfsRepairGeometry& geometry,
                                             ApfsRepairCounters* counters,
                                             QStringList* blockers) {
    if (!image || !counters || !blockers) {
        return false;
    }
    *counters = {};
    QByteArray block(static_cast<qsizetype>(geometry.blockSize), '\0');
    const QVector<uint64_t> generatedMetadataBlocks{kApfsFormatNxsbBlock,
                                                    kApfsFormatCheckpointMapBlock,
                                                    kApfsFormatCheckpointNxsbCopyBlock,
                                                    kApfsFormatContainerOmapBlock,
                                                    kApfsFormatContainerOmapTreeBlock,
                                                    kApfsFormatVolumeSuperblockBlock,
                                                    kApfsFormatVolumeOmapBlock,
                                                    kApfsFormatVolumeOmapTreeBlock,
                                                    kApfsFormatRootTreeBlock,
                                                    kApfsFormatSpacemanBlock,
                                                    kApfsFormatReaperBlock,
                                                    kApfsFormatFqMainTreeBlock,
                                                    kApfsFormatFqIpTreeBlock,
                                                    kApfsFormatGenesisMapBlock,
                                                    kApfsFormatGenesisNxsbBlock,
                                                    kApfsFormatGenesisSpacemanBlock,
                                                    kApfsFormatGenesisReaperBlock,
                                                    kApfsFormatGhostChunkInfoBlock,
                                                    kApfsFormatGhostContainerOmapBlock,
                                                    kApfsFormatGhostContainerOmapTreeBlock,
                                                    kApfsFormatExtentRefTreeBlock,
                                                    kApfsFormatSnapMetaTreeBlock,
                                                    kApfsFormatChunkInfoBlock};
    for (const uint64_t blockIndex : generatedMetadataBlocks) {
        bool repaired = false;
        if (!repairApfsObjectChecksumBlock(
                {.image = image, .geometry = geometry, .blockIndex = blockIndex, .block = &block},
                &repaired,
                blockers)) {
            return false;
        }
        counters->scannedBlocks++;
        if (repaired) {
            counters->repairedBlocks++;
        }
    }
    return true;
}

[[nodiscard]] QString normalizedEvidenceArtifactPath(QString path) {
    path = path.trimmed();
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (path.contains(QStringLiteral("//"))) {
        path.replace(QStringLiteral("//"), QStringLiteral("/"));
    }
    return path;
}

[[nodiscard]] bool isUnsafeEvidenceArtifactPath(const QString& path) {
    if (path.isEmpty()) {
        return true;
    }
    if (path.startsWith(QLatin1Char('/')) || path.startsWith(QStringLiteral("//"))) {
        return true;
    }
    if (path.size() > 1 && path.at(1) == QLatin1Char(':')) {
        return true;
    }
    if (!path.startsWith(QStringLiteral("artifacts/"), Qt::CaseInsensitive)) {
        return true;
    }
    return containsUnsafePathSegment(path);
}

struct ApfsEvidenceDescription {
    QLatin1StringView id;
    QLatin1StringView message;
};

constexpr std::array<ApfsEvidenceDescription, 11> kEvidenceDescriptions{{
    {kEvidenceStructureMapping, QLatin1StringView("APFS structure mapping evidence is missing")},
    {kEvidenceObjectChecksumVectors, QLatin1StringView("APFS object checksum evidence is missing")},
    {kEvidenceSourceImageHash, QLatin1StringView("APFS source image hash evidence is missing")},
    {kEvidenceScratchImageHash,
     QLatin1StringView("APFS scratch image hash-chain evidence is missing")},
    {kEvidenceCopyOnWriteCheckpoint,
     QLatin1StringView("APFS copy-on-write checkpoint evidence is missing")},
    {kEvidenceObjectMapUpdate, QLatin1StringView("APFS object-map update evidence is missing")},
    {kEvidenceSpaceManagerAccounting,
     QLatin1StringView("APFS space-manager accounting evidence is missing")},
    {kEvidenceFsckValidation,
     QLatin1StringView("APFS fsck_apfs/diskutil validation evidence is missing")},
    {kEvidenceTargetReadback, QLatin1StringView("APFS target read-back evidence is missing")},
    {kEvidenceCrashReplay, QLatin1StringView("APFS crash-replay evidence is missing")},
    {kEvidenceRollbackBoundary, QLatin1StringView("APFS rollback-boundary evidence is missing")},
}};

[[nodiscard]] QString evidenceRequirementDescription(const QString& requirement) {
    for (const auto& description : kEvidenceDescriptions) {
        if (requirement == description.id) {
            return QString(description.message);
        }
    }
    return QStringLiteral("APFS execution evidence requirement is missing: %1").arg(requirement);
}

[[nodiscard]] QStringList planRequiredEvidenceIds(const PartitionApfsImageMutationPlan& plan) {
    QStringList required;
    for (const auto& step : plan.steps) {
        for (const auto& requirement : step.required_evidence) {
            if (!required.contains(requirement)) {
                required.append(requirement);
            }
        }
    }
    if (required.isEmpty()) {
        required = {QString(kEvidenceStructureMapping),
                    QString(kEvidenceObjectChecksumVectors),
                    QString(kEvidenceSourceImageHash),
                    QString(kEvidenceScratchImageHash),
                    QString(kEvidenceCopyOnWriteCheckpoint),
                    QString(kEvidenceObjectMapUpdate),
                    QString(kEvidenceSpaceManagerAccounting),
                    QString(kEvidenceFsckValidation),
                    QString(kEvidenceTargetReadback),
                    QString(kEvidenceCrashReplay),
                    QString(kEvidenceRollbackBoundary)};
    }
    return required;
}

struct ApfsEvidenceFlag {
    QLatin1StringView id;
    bool PartitionApfsWriterExecutionEvidence::* member{nullptr};
};

constexpr std::array<ApfsEvidenceFlag, 11> kEvidenceFlags{{
    {kEvidenceStructureMapping, &PartitionApfsWriterExecutionEvidence::structure_mapping_verified},
    {kEvidenceObjectChecksumVectors,
     &PartitionApfsWriterExecutionEvidence::object_checksum_vectors_verified},
    {kEvidenceSourceImageHash, &PartitionApfsWriterExecutionEvidence::source_image_hash_verified},
    {kEvidenceScratchImageHash, &PartitionApfsWriterExecutionEvidence::scratch_image_hash_verified},
    {kEvidenceCopyOnWriteCheckpoint,
     &PartitionApfsWriterExecutionEvidence::copy_on_write_checkpoint_verified},
    {kEvidenceObjectMapUpdate, &PartitionApfsWriterExecutionEvidence::object_map_update_verified},
    {kEvidenceSpaceManagerAccounting,
     &PartitionApfsWriterExecutionEvidence::space_manager_accounting_verified},
    {kEvidenceFsckValidation, &PartitionApfsWriterExecutionEvidence::fsck_validation_verified},
    {kEvidenceTargetReadback, &PartitionApfsWriterExecutionEvidence::target_readback_verified},
    {kEvidenceCrashReplay, &PartitionApfsWriterExecutionEvidence::crash_replay_verified},
    {kEvidenceRollbackBoundary, &PartitionApfsWriterExecutionEvidence::rollback_boundary_verified},
}};

[[nodiscard]] QStringList verifiedEvidenceIds(
    const PartitionApfsWriterExecutionEvidence& evidence) {
    QStringList verified;
    for (const auto& flag : kEvidenceFlags) {
        if (evidence.*(flag.member)) {
            verified.append(flag.id);
        }
    }
    return verified;
}

void appendAlwaysRequiredApfsWriterBlockers(const PartitionApfsWriteOptions& options,
                                            PartitionApfsWritePreflight* result) {
    if (!options.enable_experimental_writer) {
        result->blockers.append(QStringLiteral(
            "APFS writer engine is disabled until transaction-safe implementation proof passes"));
    }
    if (!options.image_only) {
        if (!options.raw_media_hardware_certification_evidence) {
            result->blockers.append(QStringLiteral(
                "Raw APFS media writes require separate hardware certification before exposure"));
        }
    }
    if (!options.destructive_certification_evidence) {
        result->blockers.append(QStringLiteral(
            "APFS mutation requires destructive image and hardware certification evidence"));
    }
    if (options.evidence_id.trimmed().isEmpty()) {
        result->blockers.append(
            QStringLiteral("APFS mutation requires a linked certification evidence ID"));
    }
}

void appendDetectionBlockers(const PartitionFileSystemDetection& detection,
                             PartitionApfsWritePreflight* result) {
    if (detection.file_system.compare(QStringLiteral("APFS"), Qt::CaseInsensitive) != 0) {
        result->blockers.append(QStringLiteral("APFS writer requires detected APFS input"));
        return;
    }
    if (hasDetailContaining(detection.details, QStringLiteral("Metadata sanity warning:"))) {
        result->blockers.append(
            QStringLiteral("APFS metadata sanity warnings must be resolved before any mutation"));
    }
    if (detection.total_bytes == 0) {
        result->blockers.append(
            QStringLiteral("APFS writer requires known container size metadata"));
    }
}

void appendFeatureBlockers(const PartitionFileSystemDetection& detection,
                           const PartitionApfsWriteOptions& options,
                           PartitionApfsWritePreflight* result) {
    const auto incompatible = parseHexDetail(detection.details,
                                             QStringLiteral("Incompatible features:"));
    const uint64_t incompatibleMask = incompatible.value_or(0);
    const uint64_t unsupportedIncompatible = incompatibleMask &
                                             ~static_cast<uint64_t>(kApfsContainerIncompatVersion2);
    if (unsupportedIncompatible != 0 && !options.allow_encrypted_or_protected_volume) {
        result->blockers.append(
            QStringLiteral("APFS unsupported incompatible feature flags are present; "
                           "protected/encrypted variants stay blocked"));
    }
    if (!options.allow_snapshots && hasSnapshotMutationRisk(detection.details)) {
        result->blockers.append(QStringLiteral(
            "APFS active snapshot/revert metadata requires copy-on-write checkpoint proof"));
    }
    if (!options.allow_multi_volume_container) {
        const auto volumeSlots = parseDecimalDetail(detection.details,
                                                    QStringLiteral("Volume OID slots used:"));
        if (!volumeSlots.has_value() || *volumeSlots != 1) {
            result->blockers.append(QStringLiteral(
                "APFS multi-volume or unknown-volume containers require space-sharing proof"));
        }
    }
}

bool apfsOperationRequiresPayloadCap(PartitionApfsWriteOperation operation) {
    switch (operation) {
    case PartitionApfsWriteOperation::CreateDirectory:
    case PartitionApfsWriteOperation::DeleteDirectory:
    case PartitionApfsWriteOperation::CreateFile:
    case PartitionApfsWriteOperation::ReplaceFile:
    case PartitionApfsWriteOperation::DeleteFile:
        return true;
    default:
        return false;
    }
}

bool apfsOperationTouchesExistingFileData(PartitionApfsWriteOperation operation) {
    switch (operation) {
    case PartitionApfsWriteOperation::ReplaceFile:
    case PartitionApfsWriteOperation::DeleteFile:
        return true;
    default:
        return false;
    }
}

void appendApfsVolumeNameBlockers(const QString& volumeName,
                                  QLatin1StringView purpose,
                                  QStringList* blockers) {
    if (volumeName.isEmpty()) {
        blockers->append(QStringLiteral("APFS %1 requires a non-empty volume name").arg(purpose));
    }
    if (volumeName.size() > kMaximumApfsVolumeNameChars) {
        blockers->append(QStringLiteral("APFS %1 volume name exceeds 255 characters").arg(purpose));
    }
    if (volumeName.toUtf8().size() >= kApfsVolumeNameBytes) {
        blockers->append(
            QStringLiteral("APFS %1 volume name exceeds 255 UTF-8 bytes").arg(purpose));
    }
    if (containsUnsafeVolumeNameCharacter(volumeName)) {
        blockers->append(
            QStringLiteral(
                "APFS %1 volume name contains path separator or drive separator characters")
                .arg(purpose));
    }
}

void appendOperationBlockers(PartitionApfsWriteOperation operation,
                             const PartitionApfsWriteOptions& options,
                             PartitionApfsWritePreflight* result) {
    if (apfsOperationRequiresPayloadCap(operation) && options.max_payload_bytes == 0) {
        result->blockers.append(
            QStringLiteral("APFS file/directory mutation requires a bounded payload size"));
    }
    if (options.max_payload_bytes > kDefaultMaxApfsPayloadBytes) {
        result->blockers.append(QStringLiteral("APFS payload exceeds current certification cap"));
    }
    if (apfsOperationTouchesExistingFileData(operation) &&
        !options.allow_compressed_file_mutation) {
        result->blockers.append(QStringLiteral(
            "APFS compressed/encrypted file replacement and deletion require per-file proof"));
    }
}

PartitionApfsWritePreflight preflightNewContainerFormat(uint64_t target_container_bytes,
                                                        uint32_t block_size_bytes,
                                                        const QString& volume_name,
                                                        const PartitionApfsWriteOptions& options) {
    PartitionApfsWritePreflight result;
    result.required_evidence = PartitionApfsWriter::enterpriseCertificationRequirements();
    result.warnings.append(
        QStringLiteral("APFS format is limited to S.A.K. generated single-volume containers with "
                       "4096-byte blocks"));

    appendAlwaysRequiredApfsWriterBlockers(options, &result);
    if (target_container_bytes < kMinimumApfsContainerBytes) {
        result.blockers.append(
            QStringLiteral("APFS format requires at least 64 MiB target container size"));
    }
    if (target_container_bytes > kGeneratedApfsMultiChunkMaxBytes) {
        result.blockers.append(generatedApfsSingleChunkLimitBlocker(
            QLatin1StringView("format"), target_container_bytes / kSupportedApfsBlockSizeBytes));
    }
    if (block_size_bytes != kSupportedApfsBlockSizeBytes) {
        result.blockers.append(
            QStringLiteral("APFS format certification currently supports only 4096-byte blocks"));
    }
    appendApfsVolumeNameBlockers(volume_name, QLatin1StringView("format"), &result.blockers);

    result.allowed = result.blockers.isEmpty();
    return result;
}

void appendPlanTargetBlockers(PartitionApfsWriteOperation operation,
                              const QString& normalized_path,
                              PartitionApfsImageMutationPlan* plan) {
    if (operation == PartitionApfsWriteOperation::FormatContainer ||
        operation == PartitionApfsWriteOperation::ChangeVolumeLabel ||
        operation == PartitionApfsWriteOperation::RepairContainer ||
        operation == PartitionApfsWriteOperation::ResizeContainer) {
        if (!normalized_path.isEmpty()) {
            plan->preflight.blockers.append(QStringLiteral(
                "APFS whole-container operations must not carry a file-system target path"));
        }
        return;
    }

    if (normalized_path.isEmpty()) {
        plan->preflight.blockers.append(
            QStringLiteral("APFS file/directory mutation requires an absolute target path"));
        return;
    }
    if (containsUnsafePathSegment(normalized_path)) {
        plan->preflight.blockers.append(
            QStringLiteral("APFS mutation target path contains traversal segments"));
    }
}

QVector<PartitionApfsImageMutationStep> fileMutationSteps(PartitionApfsWriteOperation operation) {
    QVector<PartitionApfsImageMutationStep> steps;
    steps.append({QStringLiteral("preflight"),
                  QStringLiteral("Verify read-only APFS metadata, feature flags, object checksums, "
                                 "and linked certification evidence"),
                  false,
                  false,
                  {QString(kEvidenceStructureMapping), QString(kEvidenceObjectChecksumVectors)}});
    steps.append(
        {QStringLiteral("scratch-image"),
         QStringLiteral(
             "Clone the source image to a scratch image and hash the source before mutation"),
         false,
         false,
         {QString(kEvidenceSourceImageHash), QString(kEvidenceScratchImageHash)}});
    steps.append({QStringLiteral("block-allocation"),
                  QStringLiteral("Reserve new image blocks from the APFS free-space model without "
                                 "overwriting existing objects"),
                  true,
                  true,
                  {QString(kEvidenceSpaceManagerAccounting)}});
    steps.append(
        {QStringLiteral("fs-tree-update"),
         QStringLiteral(
             "Create copy-on-write file-system tree records for the requested APFS operation"),
         true,
         true,
         {QString(kEvidenceStructureMapping),
          QString(kEvidenceObjectChecksumVectors),
          QString(kEvidenceCopyOnWriteCheckpoint)}});
    if (operation == PartitionApfsWriteOperation::ReplaceFile ||
        operation == PartitionApfsWriteOperation::DeleteFile) {
        steps.append(
            {QStringLiteral("extent-reference-update"),
             QStringLiteral("Update extent-reference accounting for replaced or deleted file data"),
             true,
             true,
             {QString(kEvidenceStructureMapping),
              QString(kEvidenceSpaceManagerAccounting),
              QString(kEvidenceCopyOnWriteCheckpoint)}});
    }
    steps.append({QStringLiteral("object-map-update"),
                  QStringLiteral("Write updated object-map records that point each mutated object "
                                 "ID at a new physical block"),
                  true,
                  true,
                  {QString(kEvidenceObjectMapUpdate),
                   QString(kEvidenceObjectChecksumVectors),
                   QString(kEvidenceCopyOnWriteCheckpoint)}});
    steps.append({QStringLiteral("checkpoint-commit"),
                  QStringLiteral("Write a new checkpoint map and container superblock checkpoint "
                                 "with monotonic transaction ID"),
                  true,
                  true,
                  {QString(kEvidenceCopyOnWriteCheckpoint),
                   QString(kEvidenceRollbackBoundary),
                   QString(kEvidenceObjectChecksumVectors)}});
    steps.append({QStringLiteral("post-verify"),
                  QStringLiteral("Reopen the scratch image read-only, run platform APFS "
                                 "verification, and compare targeted read-back"),
                  false,
                  false,
                  {QString(kEvidenceFsckValidation),
                   QString(kEvidenceTargetReadback),
                   QString(kEvidenceCrashReplay)}});
    return steps;
}

QVector<PartitionApfsImageMutationStep> formatContainerSteps() {
    QVector<PartitionApfsImageMutationStep> steps;
    steps.append({QStringLiteral("target-image-snapshot"),
                  QStringLiteral("Create a scratch target image, hash any pre-format bytes, and "
                                 "keep rollback artifacts"),
                  false,
                  false,
                  {QString(kEvidenceSourceImageHash), QString(kEvidenceScratchImageHash)}});
    steps.append({QStringLiteral("container-layout"),
                  QStringLiteral("Lay out APFS NXSB container, object header fields, block "
                                 "geometry, feature flags, and UUIDs"),
                  true,
                  true,
                  {QString(kEvidenceStructureMapping), QString(kEvidenceObjectChecksumVectors)}});
    steps.append(
        {QStringLiteral("checkpoint-regions"),
         QStringLiteral(
             "Initialize checkpoint descriptor and data areas with monotonic transaction IDs"),
         true,
         true,
         {QString(kEvidenceCopyOnWriteCheckpoint),
          QString(kEvidenceRollbackBoundary),
          QString(kEvidenceObjectChecksumVectors)}});
    steps.append(
        {QStringLiteral("space-manager-init"),
         QStringLiteral("Initialize spaceman counters, device records, free-space accounting, and "
                        "allocation state"),
         true,
         true,
         {QString(kEvidenceSpaceManagerAccounting), QString(kEvidenceObjectChecksumVectors)}});
    steps.append({QStringLiteral("object-map-init"),
                  QStringLiteral(
                      "Create initial container and volume object maps for the empty APFS volume"),
                  true,
                  true,
                  {QString(kEvidenceObjectMapUpdate),
                   QString(kEvidenceStructureMapping),
                   QString(kEvidenceObjectChecksumVectors)}});
    steps.append({QStringLiteral("volume-superblock-root-tree-init"),
                  QStringLiteral("Create APFS volume superblock, empty root directory tree, extent "
                                 "tree, and metadata tree objects"),
                  true,
                  true,
                  {QString(kEvidenceStructureMapping),
                   QString(kEvidenceObjectChecksumVectors),
                   QString(kEvidenceCopyOnWriteCheckpoint)}});
    steps.append(
        {QStringLiteral("checkpoint-commit"),
         QStringLiteral(
             "Commit the formatted container by publishing the new checkpoint set atomically"),
         true,
         true,
         {QString(kEvidenceCopyOnWriteCheckpoint),
          QString(kEvidenceRollbackBoundary),
          QString(kEvidenceObjectChecksumVectors)}});
    steps.append({QStringLiteral("post-verify"),
                  QStringLiteral("Verify the formatted scratch image with platform APFS tooling "
                                 "and S.A.K. read-only browse"),
                  false,
                  false,
                  {QString(kEvidenceFsckValidation),
                   QString(kEvidenceTargetReadback),
                   QString(kEvidenceCrashReplay)}});
    return steps;
}

QVector<PartitionApfsImageMutationStep> repairContainerSteps() {
    QVector<PartitionApfsImageMutationStep> steps;
    steps.append({QStringLiteral("repair-scan"),
                  QStringLiteral("Scan all reachable checkpoints, object maps, spaceman objects, "
                                 "volume superblocks, and B-tree roots read-only"),
                  false,
                  false,
                  {QString(kEvidenceStructureMapping), QString(kEvidenceObjectChecksumVectors)}});
    steps.append(
        {QStringLiteral("scratch-image"),
         QStringLiteral(
             "Clone the damaged image to a scratch image and hash the source before repair"),
         false,
         false,
         {QString(kEvidenceSourceImageHash), QString(kEvidenceScratchImageHash)}});
    steps.append({QStringLiteral("object-map-rebuild-plan"),
                  QStringLiteral("Build a repair plan that maps each surviving object ID to a "
                                 "verified replacement block"),
                  true,
                  true,
                  {QString(kEvidenceObjectMapUpdate),
                   QString(kEvidenceStructureMapping),
                   QString(kEvidenceCopyOnWriteCheckpoint)}});
    steps.append(
        {QStringLiteral("space-manager-reconcile"),
         QStringLiteral(
             "Reconcile allocated/free block accounting against surviving APFS metadata objects"),
         true,
         true,
         {QString(kEvidenceSpaceManagerAccounting), QString(kEvidenceObjectChecksumVectors)}});
    steps.append(
        {QStringLiteral("repaired-checkpoint-commit"),
         QStringLiteral(
             "Write a repaired checkpoint without overwriting the last known source checkpoint"),
         true,
         true,
         {QString(kEvidenceCopyOnWriteCheckpoint),
          QString(kEvidenceRollbackBoundary),
          QString(kEvidenceObjectChecksumVectors)}});
    steps.append({QStringLiteral("post-verify"),
                  QStringLiteral("Verify repaired scratch image with fsck_apfs/diskutil, S.A.K. "
                                 "browse, and crash replay"),
                  false,
                  false,
                  {QString(kEvidenceFsckValidation),
                   QString(kEvidenceTargetReadback),
                   QString(kEvidenceCrashReplay)}});
    return steps;
}

QVector<PartitionApfsImageMutationStep> resizeContainerSteps() {
    QVector<PartitionApfsImageMutationStep> steps;
    steps.append(
        {QStringLiteral("resize-preflight"),
         QStringLiteral(
             "Verify APFS block geometry, volume sharing state, snapshots, and target size bounds"),
         false,
         false,
         {QString(kEvidenceStructureMapping), QString(kEvidenceObjectChecksumVectors)}});
    steps.append(
        {QStringLiteral("scratch-image"),
         QStringLiteral(
             "Clone and resize a scratch image while preserving source-image rollback artifacts"),
         false,
         false,
         {QString(kEvidenceSourceImageHash), QString(kEvidenceScratchImageHash)}});
    steps.append(
        {QStringLiteral("container-size-update"),
         QStringLiteral(
             "Write updated NXSB block counts, checkpoint bounds, and device allocation ranges"),
         true,
         true,
         {QString(kEvidenceStructureMapping),
          QString(kEvidenceCopyOnWriteCheckpoint),
          QString(kEvidenceObjectChecksumVectors)}});
    steps.append(
        {QStringLiteral("space-manager-rebalance"),
         QStringLiteral(
             "Update spaceman free queues and allocation counters for the resized container"),
         true,
         true,
         {QString(kEvidenceSpaceManagerAccounting), QString(kEvidenceObjectChecksumVectors)}});
    steps.append(
        {QStringLiteral("object-map-update"),
         QStringLiteral(
             "Publish resized container metadata through object-map records and a new checkpoint"),
         true,
         true,
         {QString(kEvidenceObjectMapUpdate),
          QString(kEvidenceCopyOnWriteCheckpoint),
          QString(kEvidenceObjectChecksumVectors)}});
    steps.append({QStringLiteral("post-verify"),
                  QStringLiteral("Verify resized scratch image with platform APFS tooling, "
                                 "read-back, and crash replay"),
                  false,
                  false,
                  {QString(kEvidenceFsckValidation),
                   QString(kEvidenceTargetReadback),
                   QString(kEvidenceCrashReplay),
                   QString(kEvidenceRollbackBoundary)}});
    return steps;
}

QVector<PartitionApfsImageMutationStep> volumeLabelChangeSteps() {
    QVector<PartitionApfsImageMutationStep> steps;
    steps.append({QStringLiteral("preflight-generated-layout"),
                  QStringLiteral("Verify S.A.K.-generated APFS layout, block geometry, and object "
                                 "checksums before label mutation"),
                  false,
                  false,
                  {QString(kEvidenceStructureMapping), QString(kEvidenceObjectChecksumVectors)}});
    steps.append({QStringLiteral("scratch-or-selected-target"),
                  QStringLiteral("Clone image output or require selected raw partition "
                                 "confirmation before metadata write"),
                  false,
                  false,
                  {QString(kEvidenceSourceImageHash), QString(kEvidenceScratchImageHash)}});
    steps.append(
        {QStringLiteral("volume-superblock-label-update"),
         QStringLiteral("Update APFS volume superblock name field and restamp object checksum"),
         true,
         true,
         {QString(kEvidenceStructureMapping),
          QString(kEvidenceObjectChecksumVectors),
          QString(kEvidenceRollbackBoundary)}});
    steps.append({QStringLiteral("post-verify"),
                  QStringLiteral("Reopen target read-only and verify S.A.K. APFS browser reports "
                                 "the requested volume name"),
                  false,
                  false,
                  {QString(kEvidenceTargetReadback), QString(kEvidenceCrashReplay)}});
    return steps;
}

QVector<PartitionApfsImageMutationStep> imageOnlyMutationSteps(
    PartitionApfsWriteOperation operation) {
    switch (operation) {
    case PartitionApfsWriteOperation::FormatContainer:
        return formatContainerSteps();
    case PartitionApfsWriteOperation::ChangeVolumeLabel:
        return volumeLabelChangeSteps();
    case PartitionApfsWriteOperation::RepairContainer:
        return repairContainerSteps();
    case PartitionApfsWriteOperation::ResizeContainer:
        return resizeContainerSteps();
    case PartitionApfsWriteOperation::CreateDirectory:
    case PartitionApfsWriteOperation::DeleteDirectory:
    case PartitionApfsWriteOperation::CreateFile:
    case PartitionApfsWriteOperation::ReplaceFile:
    case PartitionApfsWriteOperation::DeleteFile:
        return fileMutationSteps(operation);
    }
    return fileMutationSteps(operation);
}

void appendMissingExecutionEvidenceBlockers(const PartitionApfsImageMutationPlan& plan,
                                            const PartitionApfsWriterExecutionEvidence& evidence,
                                            PartitionApfsWritePreflight* result) {
    const QStringList required = planRequiredEvidenceIds(plan);
    const QStringList verified = verifiedEvidenceIds(evidence);
    for (const auto& requirement : required) {
        if (!verified.contains(requirement)) {
            result->blockers.append(evidenceRequirementDescription(requirement));
        }
    }
    if (evidence.artifacts.isEmpty()) {
        result->blockers.append(QStringLiteral("APFS execution evidence requires artifact paths"));
    }
    for (const auto& artifact : evidence.artifacts) {
        if (isUnsafeEvidenceArtifactPath(normalizedEvidenceArtifactPath(artifact))) {
            result->blockers.append(
                QStringLiteral("APFS execution evidence artifact path must be relative under "
                               "artifacts/ and traversal-free"));
            break;
        }
    }
}

void appendPlanEvidenceMismatchBlockers(const PartitionApfsImageMutationPlan& plan,
                                        const PartitionApfsWriterExecutionEvidence& evidence,
                                        PartitionApfsWritePreflight* result) {
    if (!plan.buildable) {
        result->blockers.append(QStringLiteral("APFS mutation plan is not buildable"));
    }
    if (plan.evidence_id != evidence.evidence_id.trimmed()) {
        result->blockers.append(QStringLiteral("APFS execution evidence ID does not match plan"));
    }
    if (plan.operation != evidence.operation.trimmed()) {
        result->blockers.append(
            QStringLiteral("APFS execution evidence operation does not match plan"));
    }
    if (plan.target_path != normalizedApfsTargetPath(evidence.target_path)) {
        result->blockers.append(
            QStringLiteral("APFS execution evidence target path does not match plan"));
    }
}

QStringList imageOnlyExecutionBlockers() {
    return {
        QStringLiteral(
            "APFS image mutation execution is blocked until structured evidence is supplied"),
        QStringLiteral(
            "Raw APFS media exposure remains blocked until separate hardware proof is supplied")};
}

}  // namespace

QString PartitionApfsWriter::operationName(PartitionApfsWriteOperation operation) {
    switch (operation) {
    case PartitionApfsWriteOperation::CreateDirectory:
        return QStringLiteral("Create directory");
    case PartitionApfsWriteOperation::DeleteDirectory:
        return QStringLiteral("Delete directory");
    case PartitionApfsWriteOperation::CreateFile:
        return QStringLiteral("Create file");
    case PartitionApfsWriteOperation::ReplaceFile:
        return QStringLiteral("Replace file");
    case PartitionApfsWriteOperation::DeleteFile:
        return QStringLiteral("Delete file");
    case PartitionApfsWriteOperation::ChangeVolumeLabel:
        return QStringLiteral("Change APFS volume label");
    case PartitionApfsWriteOperation::FormatContainer:
        return QStringLiteral("Format APFS container");
    case PartitionApfsWriteOperation::RepairContainer:
        return QStringLiteral("Repair APFS container");
    case PartitionApfsWriteOperation::ResizeContainer:
        return QStringLiteral("Resize APFS container");
    }
    return QStringLiteral("APFS operation");
}

PartitionApfsContainerGeometry PartitionApfsWriter::computeContainerGeometry(uint64_t block_count,
                                                                             uint32_t block_size) {
    PartitionApfsContainerGeometry geometry;
    geometry.block_count = block_count;
    geometry.block_size = block_size;
    geometry.blocks_per_chunk = kApfsSpacemanBlocksPerChunk;
    geometry.chunks_per_cib = kApfsSpacemanChunksPerCib;
    geometry.cibs_per_cab = kApfsSpacemanCibsPerCab;
    geometry.ip_bitmap_block_count = kApfsFormatIpBitmapBlocks;
    if (block_count == 0 || geometry.blocks_per_chunk == 0) {
        return geometry;
    }
    // One spaceman chunk covers blocks_per_chunk (32768) blocks; one 4096-byte
    // allocation bitmap (32768 bits) covers exactly one chunk.
    geometry.chunk_count = (block_count + geometry.blocks_per_chunk - 1) /
                           geometry.blocks_per_chunk;
    geometry.chunk_bitmap_block_count = geometry.chunk_count;
    // Chunk-info entries pack chunks_per_cib (126) per chunk-info block; CIBs
    // pack cibs_per_cab (507) per chunk-info address block.
    geometry.cib_count = (geometry.chunk_count + geometry.chunks_per_cib - 1) /
                         geometry.chunks_per_cib;
    if (geometry.cib_count == 0) {
        geometry.cib_count = 1;
    }
    geometry.multi_cib = geometry.cib_count > 1;
    geometry.cab_count = geometry.multi_cib ? (geometry.cib_count + geometry.cibs_per_cab - 1) /
                                                  geometry.cibs_per_cab
                                            : 0;
    // Internal-pool block count derived from real Apple newfs_apfs containers
    // (see PartitionApfsContainerGeometry doc): 3 * (cib_count + chunk_count).
    geometry.ip_block_count = 3ULL * (geometry.cib_count + geometry.chunk_count);
    geometry.single_chunk = geometry.chunk_count == 1;
    return geometry;
}

std::optional<uint64_t> PartitionApfsWriter::computeObjectChecksum(const QByteArray& object_bytes) {
    const qsizetype payloadBytes = object_bytes.size() - kApfsObjectChecksumBytes;
    if (payloadBytes <= 0 || payloadBytes % kApfsChecksumWordBytes != 0) {
        return std::nullopt;
    }

    uint64_t sum1 = 0;
    uint64_t sum2 = 0;
    uint64_t chunkWords = 0;
    for (qsizetype offset = kApfsObjectChecksumBytes; offset < object_bytes.size();
         offset += kApfsChecksumWordBytes) {
        const auto* word = reinterpret_cast<const uchar*>(object_bytes.constData() + offset);
        sum1 += qFromLittleEndian<uint32_t>(word);
        sum2 += sum1;
        ++chunkWords;
        if (chunkWords == kFletcherModuloChunkWords) {
            sum1 %= kFletcherModulo;
            sum2 %= kFletcherModulo;
            chunkWords = 0;
        }
    }

    sum1 %= kFletcherModulo;
    sum2 %= kFletcherModulo;

    const uint64_t low = kFletcherModulo - ((sum1 + sum2) % kFletcherModulo);
    const uint64_t high = kFletcherModulo - ((sum1 + low) % kFletcherModulo);
    return low | (high << kFletcherHighWordShift);
}

bool PartitionApfsWriter::stampObjectChecksum(QByteArray* object_bytes) {
    if (!object_bytes) {
        return false;
    }
    const auto checksum = computeObjectChecksum(*object_bytes);
    if (!checksum.has_value()) {
        return false;
    }
    qToLittleEndian<uint64_t>(*checksum, reinterpret_cast<uchar*>(object_bytes->data()));
    return true;
}

bool PartitionApfsWriter::verifyObjectChecksum(const QByteArray& object_bytes) {
    if (object_bytes.size() < kApfsObjectChecksumBytes) {
        return false;
    }
    const auto checksum = computeObjectChecksum(object_bytes);
    if (!checksum.has_value()) {
        return false;
    }
    const auto* stored = reinterpret_cast<const uchar*>(object_bytes.constData());
    return qFromLittleEndian<uint64_t>(stored) == *checksum;
}

QStringList PartitionApfsWriter::enterpriseCertificationRequirements() {
    return {QStringLiteral("Apple APFS Reference structure mapping for every mutated object"),
            QStringLiteral("Object checksum generation and verification"),
            QStringLiteral("Source-image and scratch-image hash-chain proof"),
            QStringLiteral("Copy-on-write checkpoint transaction generation"),
            QStringLiteral("Object-map update and rollback proof"),
            QStringLiteral("Space-manager allocation/free counter proof"),
            QStringLiteral("Single-volume image mutation proof with fsck_apfs/diskutil validation"),
            QStringLiteral("Target read-back size/hash proof through the S.A.K. APFS reader"),
            QStringLiteral("Crash-interruption replay proof at each transaction boundary"),
            QStringLiteral("Rollback-boundary proof for failed or interrupted writes"),
            QStringLiteral("Hardware proof on disposable APFS media before raw-device exposure")};
}

PartitionApfsWritePreflight PartitionApfsWriter::preflightExistingContainer(
    const PartitionFileSystemDetection& detection,
    PartitionApfsWriteOperation operation,
    const PartitionApfsWriteOptions& options) {
    PartitionApfsWritePreflight result;
    result.required_evidence = enterpriseCertificationRequirements();
    result.warnings.append(
        QStringLiteral("APFS mutation remains limited to certified generated/minimal layouts; "
                       "arbitrary Apple APFS mutation stays blocked"));

    appendAlwaysRequiredApfsWriterBlockers(options, &result);
    appendDetectionBlockers(detection, &result);
    appendFeatureBlockers(detection, options, &result);
    appendOperationBlockers(operation, options, &result);

    result.allowed = result.blockers.isEmpty();
    return result;
}

PartitionApfsImageMutationPlan PartitionApfsWriter::planImageOnlyMutation(
    const PartitionFileSystemDetection& detection,
    PartitionApfsWriteOperation operation,
    const PartitionApfsWriteOptions& options,
    const QString& target_path) {
    PartitionApfsImageMutationPlan plan;
    plan.preflight = preflightExistingContainer(detection, operation, options);
    plan.operation = operationName(operation);
    plan.target_path = normalizedApfsTargetPath(target_path);
    plan.evidence_id = options.evidence_id.trimmed();
    plan.max_payload_bytes = options.max_payload_bytes;
    plan.target_container_bytes = detection.total_bytes;
    if (const auto blockSize = parseDecimalDetail(detection.details,
                                                  QStringLiteral("Block size:"))) {
        plan.block_size_bytes = static_cast<uint32_t>(*blockSize);
    }

    appendPlanTargetBlockers(operation, plan.target_path, &plan);
    plan.preflight.allowed = plan.preflight.blockers.isEmpty();
    plan.buildable = plan.preflight.allowed;
    plan.execution_blockers = imageOnlyExecutionBlockers();
    if (!plan.buildable) {
        return plan;
    }

    plan.steps = imageOnlyMutationSteps(operation);
    if (operation == PartitionApfsWriteOperation::ChangeVolumeLabel) {
        plan.post_apply_verification = {
            QStringLiteral("Verify changed APFS volume superblock checksum before publication"),
            QStringLiteral(
                "Verify source-image and scratch-image hash chain before and after mutation"),
            QStringLiteral("Run fsck_apfs/diskutil read-only verification on the scratch image"),
            QStringLiteral("Reopen the target through S.A.K. read-only APFS browser"),
            QStringLiteral("Read back the APFS volume name and compare the requested label"),
            QStringLiteral("Record crash-interruption replay artifacts for the metadata write")};
        return plan;
    }
    plan.post_apply_verification = {
        QStringLiteral("Verify every mutated APFS object checksum before image publication"),
        QStringLiteral(
            "Verify source-image and scratch-image hash chain before and after mutation"),
        QStringLiteral("Run fsck_apfs/diskutil read-only verification on the scratch image"),
        QStringLiteral("Reopen the scratch image through S.A.K. read-only APFS browser"),
        QStringLiteral("Read back the target path and compare expected size/hash"),
        QStringLiteral("Record crash-interruption replay artifacts for the checkpoint boundary")};
    return plan;
}

PartitionApfsImageMutationPlan PartitionApfsWriter::planImageOnlyFormat(
    uint64_t target_container_bytes,
    uint32_t block_size_bytes,
    const QString& volume_name,
    const PartitionApfsWriteOptions& options) {
    PartitionApfsImageMutationPlan plan;
    plan.volume_name = normalizedApfsVolumeName(volume_name);
    plan.preflight = preflightNewContainerFormat(
        target_container_bytes, block_size_bytes, plan.volume_name, options);
    plan.operation = operationName(PartitionApfsWriteOperation::FormatContainer);
    plan.evidence_id = options.evidence_id.trimmed();
    plan.max_payload_bytes = options.max_payload_bytes;
    plan.target_container_bytes = target_container_bytes;
    plan.block_size_bytes = block_size_bytes;
    plan.execution_blockers = imageOnlyExecutionBlockers();
    plan.buildable = plan.preflight.allowed;
    if (!plan.buildable) {
        return plan;
    }

    plan.steps = formatContainerSteps();
    plan.post_apply_verification = {
        QStringLiteral("Verify every APFS object checksum in the formatted image"),
        QStringLiteral("Verify formatted image hash-chain and rollback artifact retention"),
        QStringLiteral(
            "Run fsck_apfs/diskutil read-only verification on the formatted scratch image"),
        QStringLiteral("Open the formatted image through S.A.K. read-only APFS browser"),
        QStringLiteral("Verify the empty root directory and requested APFS volume name"),
        QStringLiteral(
            "Record crash-interruption replay artifacts for format checkpoint publication")};
    return plan;
}

using ApfsImageBlock = std::pair<uint64_t, QByteArray>;

PartitionApfsImageBuildResult formatBuildResult(const PartitionApfsImageFormatRequest& request) {
    PartitionApfsImageBuildResult result;
    result.image_path = request.image_path.trimmed();
    result.plan = PartitionApfsWriter::planImageOnlyFormat(request.target_container_bytes,
                                                           request.block_size_bytes,
                                                           request.volume_name,
                                                           request.options);
    result.blockers = result.plan.preflight.blockers;
    result.warnings = result.plan.preflight.warnings;
    result.warnings.append(
        request.options.image_only
            ? QStringLiteral(
                  "Generated APFS image format remains image-only; raw partition format is exposed "
                  "only through sak_apfs_writer_cli.exe with explicit selected-target confirmation")
            : QStringLiteral("Raw APFS format is limited to selected partitions through the APFS "
                             "writer helper with explicit confirmation"));
    return result;
}

bool appendFormatGeometryBlockers(const PartitionApfsImageFormatRequest& request,
                                  PartitionApfsImageBuildResult* result) {
    if (request.target_container_bytes % request.block_size_bytes != 0) {
        result->blockers.append(
            QStringLiteral("APFS format target size must be an exact multiple of block size"));
        return false;
    }
    if (request.target_container_bytes >
        static_cast<uint64_t>(std::numeric_limits<qint64>::max())) {
        result->blockers.append(QStringLiteral("APFS format image is too large for this platform"));
        return false;
    }
    return true;
}

bool appendFormatCreateBlockers(const PartitionApfsImageFormatRequest& request,
                                PartitionApfsImageBuildResult* result) {
    if (!imagePathIsSafeForCreate(result->image_path, &result->blockers)) {
        return false;
    }
    return appendFormatGeometryBlockers(request, result);
}

bool appendRawFormatTargetBlockers(const PartitionApfsImageFormatRequest& request,
                                   bool rawTarget,
                                   PartitionApfsImageBuildResult* result) {
    if (!rawTarget) {
        return true;
    }
    if (!request.allow_raw_device_target) {
        result->blockers.append(
            QStringLiteral("APFS raw-device format target requires explicit raw-device opt-in"));
        return false;
    }
    if (request.options.image_only) {
        result->blockers.append(
            QStringLiteral("APFS raw-device format target requires non-image-only writer options"));
        return false;
    }
    return true;
}

bool appendFileFormatTargetBlockers(const PartitionApfsImageFormatRequest& request,
                                    bool rawTarget,
                                    PartitionApfsImageBuildResult* result) {
    if (rawTarget) {
        return true;
    }
    const QFileInfo target(result->image_path);
    if (result->image_path.isEmpty() || !target.exists() || !target.isFile()) {
        result->blockers.append(QStringLiteral("APFS existing-image format target is required"));
        return false;
    }
    if (static_cast<uint64_t>(target.size()) != request.target_container_bytes) {
        result->blockers.append(QStringLiteral(
            "APFS existing-image format target size must match requested container size"));
        return false;
    }
    return true;
}

bool appendFormatWipeConfirmationBlocker(const PartitionApfsImageFormatRequest& request,
                                         PartitionApfsImageBuildResult* result) {
    if (!request.target_wipe_confirmed) {
        result->blockers.append(
            QStringLiteral("APFS existing-image format requires destructive wipe confirmation"));
        return false;
    }
    return true;
}

bool appendExistingFormatTargetBlockers(const PartitionApfsImageFormatRequest& request,
                                        PartitionApfsImageBuildResult* result) {
    if (!appendFormatGeometryBlockers(request, result)) {
        return false;
    }
    const bool rawTarget = isWindowsRawDevicePath(result->image_path);
    return appendRawFormatTargetBlockers(request, rawTarget, result) &&
           appendFileFormatTargetBlockers(request, rawTarget, result) &&
           appendFormatWipeConfirmationBlocker(request, result);
}

bool createSizedApfsImage(const QString& path,
                          uint64_t sizeBytes,
                          QFile* image,
                          QStringList* blockers) {
    image->setFileName(path);
    if (!image->open(QIODevice::WriteOnly)) {
        blockers->append(
            QStringLiteral("Unable to create APFS image: %1").arg(image->errorString()));
        return false;
    }
    if (!image->resize(static_cast<qint64>(sizeBytes))) {
        blockers->append(QStringLiteral("Unable to size APFS image: %1").arg(image->errorString()));
        return false;
    }
    return true;
}

QVector<ApfsImageBlock> emptyFormatBlocks(const PartitionApfsImageFormatRequest& request,
                                          uint64_t blockCount,
                                          const QString& volumeName,
                                          QStringList* blockers) {
    const QByteArray containerUuid = randomApfsUuid();
    const QByteArray volumeUuid = randomApfsUuid();
    // The internal pool grows to 3*(chunk_count+1) blocks; every object placed
    // after the internal pool shifts by the same delta so the IP region and the
    // post-pool metadata never overlap (apfsck marks the whole IP region used).
    const uint64_t chunkCount = (blockCount + kApfsSpacemanBlocksPerChunk - 1) /
                                kApfsSpacemanBlocksPerChunk;
    const uint64_t ipDelta = 3 * (chunkCount + 1) - kApfsFormatIpBlockCount;
    const uint64_t ghostOmap = kApfsFormatGhostContainerOmapBlock + ipDelta;
    const uint64_t ghostOmapTree = kApfsFormatGhostContainerOmapTreeBlock + ipDelta;
    const uint64_t ghostReserved = kApfsFormatGhostReservedBlocks + ipDelta;
    const uint64_t volOmap = kApfsFormatVolumeOmapBlock + ipDelta;
    const uint64_t volOmapTree = kApfsFormatVolumeOmapTreeBlock + ipDelta;
    const uint64_t extentRef = kApfsFormatExtentRefTreeBlock + ipDelta;
    const uint64_t snapMeta = kApfsFormatSnapMetaTreeBlock + ipDelta;
    const uint64_t rootTree = kApfsFormatRootTreeBlock + ipDelta;
    const uint64_t volSuper = kApfsFormatVolumeSuperblockBlock + ipDelta;
    const uint64_t containerOmap = kApfsFormatContainerOmapBlock + ipDelta;
    const uint64_t containerOmapTree = kApfsFormatContainerOmapTreeBlock + ipDelta;
    const uint64_t seedData = kApfsFormatSeedFileDataBlock + ipDelta;
    const QVector<ApfsObjectMapEntry> containerMappings{
        {kApfsFormatVolumeOid, kApfsFormatXid, volSuper}};
    const QVector<ApfsObjectMapEntry> volumeMappings{
        {kApfsFormatRootTreeOid, kApfsFormatXid, rootTree}};
    const QByteArray nxsb = buildNxSuperblock(
        request.block_size_bytes, blockCount, containerUuid, {.omapOid = containerOmap}, blockers);
    const QByteArray genesisNxsb = buildNxSuperblock(request.block_size_bytes,
                                                     blockCount,
                                                     containerUuid,
                                                     {.xid = kApfsFormatGenesisXid,
                                                      .descIndex = 0,
                                                      .descNext = 2,
                                                      .dataIndex = 0,
                                                      .dataLen = 2,
                                                      .dataNext = 2,
                                                      .omapOid = ghostOmap,
                                                      .fsOid = 0,
                                                      .nextOid = kApfsFormatGenesisNextOid},
                                                     blockers);
    const QVector<ApfsCheckpointMapEntry> genesisMappings{
        {kApfsObjectTypeSpaceman, 0, kApfsFormatSpacemanOid, kApfsFormatGenesisSpacemanBlock},
        {kApfsObjectTypeReaper, 0, kApfsFormatReaperOid, kApfsFormatGenesisReaperBlock}};
    const QVector<ApfsCheckpointMapEntry> liveMappings{
        {kApfsObjectTypeSpaceman, 0, kApfsFormatSpacemanOid, kApfsFormatSpacemanBlock},
        {kApfsObjectTypeReaper, 0, kApfsFormatReaperOid, kApfsFormatReaperBlock},
        {kApfsObjectTypeBtreeEphemeral,
         kApfsObjectSubtypeFreeQueue,
         kApfsFormatFqIpTreeOid,
         kApfsFormatFqIpTreeBlock},
        {kApfsObjectTypeBtreeEphemeral,
         kApfsObjectSubtypeFreeQueue,
         kApfsFormatFqMainTreeOid,
         kApfsFormatFqMainTreeBlock}};
    return {{kApfsFormatNxsbBlock, nxsb},
            {kApfsFormatGenesisMapBlock,
             buildCheckpointMapBlock(request.block_size_bytes,
                                     kApfsFormatGenesisMapBlock,
                                     kApfsFormatGenesisXid,
                                     genesisMappings,
                                     blockers)},
            {kApfsFormatGenesisNxsbBlock, genesisNxsb},
            {kApfsFormatCheckpointMapBlock,
             buildCheckpointMapBlock(request.block_size_bytes,
                                     kApfsFormatCheckpointMapBlock,
                                     kApfsFormatXid,
                                     liveMappings,
                                     blockers)},
            {kApfsFormatCheckpointNxsbCopyBlock, nxsb},
            {kApfsFormatGenesisSpacemanBlock,
             buildSpacemanBlock({.blockSize = request.block_size_bytes,
                                 .blockCount = blockCount,
                                 .reservedBlocks = ghostReserved,
                                 .xid = kApfsFormatGenesisXid,
                                 .genesis = true},
                                blockers)},
            {kApfsFormatGenesisReaperBlock,
             buildReaperBlock(request.block_size_bytes, kApfsFormatGenesisXid, blockers)},
            {kApfsFormatSpacemanBlock,
             buildSpacemanBlock({.blockSize = request.block_size_bytes,
                                 .blockCount = blockCount,
                                 .reservedBlocks = seedData,
                                 .xid = kApfsFormatXid,
                                 .genesis = false},
                                blockers)},
            {kApfsFormatReaperBlock,
             buildReaperBlock(request.block_size_bytes, kApfsFormatXid, blockers)},
            {kApfsFormatFqIpTreeBlock,
             buildFreeQueueTreeBlock(request.block_size_bytes,
                                     kApfsFormatFqIpTreeOid,
                                     kApfsFormatGhostChunkInfoBlock,
                                     2,
                                     blockers)},
            {kApfsFormatFqMainTreeBlock,
             buildFreeQueueTreeBlock(
                 request.block_size_bytes, kApfsFormatFqMainTreeOid, ghostOmap, 2, blockers)},
            {kApfsFormatIpBitmapBaseBlock, buildIpBitmapBlock(request.block_size_bytes, 0x3)},
            {kApfsFormatIpBitmapBaseBlock + 1, buildIpBitmapBlock(request.block_size_bytes, 0xF)},
            {kApfsFormatGhostChunkInfoBlock,
             buildChunkInfoBlock({.blockSize = request.block_size_bytes,
                                  .blockCount = blockCount,
                                  .reservedBlocks = ghostReserved,
                                  .xid = kApfsFormatGenesisXid,
                                  .selfBlock = kApfsFormatGhostChunkInfoBlock,
                                  .bitmapBlock = kApfsFormatGhostChunkBitmapBlock},
                                 blockers)},
            {kApfsFormatGhostChunkBitmapBlock,
             buildChunkBitmapBlock(request.block_size_bytes, ghostReserved)},
            {kApfsFormatChunkInfoBlock,
             buildChunkInfoBlock({.blockSize = request.block_size_bytes,
                                  .blockCount = blockCount,
                                  .reservedBlocks = seedData,
                                  .xid = kApfsFormatXid,
                                  .selfBlock = kApfsFormatChunkInfoBlock,
                                  .bitmapBlock = kApfsFormatChunkBitmapBlock},
                                 blockers)},
            {kApfsFormatChunkBitmapBlock,
             buildChunkBitmapBlock(request.block_size_bytes, seedData)},
            {ghostOmap,
             buildObjectMapBlock({.blockSize = request.block_size_bytes,
                                  .oid = ghostOmap,
                                  .treeBlock = ghostOmapTree,
                                  .xid = kApfsFormatGenesisXid,
                                  .omapFlags = 1},
                                 blockers)},
            {ghostOmapTree,
             buildObjectMapTreeBlock(
                 request.block_size_bytes, ghostOmapTree, {}, kApfsFormatGenesisXid, blockers)},
            {volOmap,
             buildObjectMapBlock({.blockSize = request.block_size_bytes,
                                  .oid = volOmap,
                                  .treeBlock = volOmapTree,
                                  .xid = kApfsFormatXid,
                                  .omapFlags = 0},
                                 blockers)},
            {volOmapTree,
             buildObjectMapTreeBlock(
                 request.block_size_bytes, volOmapTree, volumeMappings, kApfsFormatXid, blockers)},
            {extentRef,
             buildEmptyVariableTreeBlock(
                 request.block_size_bytes, extentRef, kApfsObjectSubtypeExtentRef, blockers)},
            {snapMeta,
             buildEmptyVariableTreeBlock(
                 request.block_size_bytes, snapMeta, kApfsObjectSubtypeSnapMeta, blockers)},
            {rootTree, buildEmptyRootTreeBlock(request.block_size_bytes, blockers)},
            {volSuper,
             buildVolumeSuperblock({.blockSize = request.block_size_bytes,
                                    .volumeName = volumeName,
                                    .volumeUuid = volumeUuid,
                                    .allocatedBlocks = kApfsFormatVolumeBaseAllocatedBlocks,
                                    .volumeOmapBlock = volOmap,
                                    .extentRefTreeBlock = extentRef,
                                    .snapMetaTreeBlock = snapMeta},
                                   blockers)},
            {containerOmap,
             buildObjectMapBlock({.blockSize = request.block_size_bytes,
                                  .oid = containerOmap,
                                  .treeBlock = containerOmapTree,
                                  .xid = kApfsFormatXid,
                                  .omapFlags = 1},
                                 blockers)},
            {containerOmapTree,
             buildObjectMapTreeBlock(request.block_size_bytes,
                                     containerOmapTree,
                                     containerMappings,
                                     kApfsFormatXid,
                                     blockers)}};
}

bool writeImageBlocks(QIODevice* device,
                      uint32_t blockSize,
                      const QVector<ApfsImageBlock>& blocks,
                      QStringList* blockers) {
    for (const auto& block : blocks) {
        if (!writeBlock(device, block.first, blockSize, block.second, blockers)) {
            return false;
        }
    }
    return true;
}

bool writeZeroRange(QIODevice* device, uint64_t offset, uint64_t length, QStringList* blockers) {
    if (!device || offset > static_cast<uint64_t>(std::numeric_limits<qint64>::max())) {
        blockers->append(QStringLiteral("APFS zero-fill target offset is invalid"));
        return false;
    }
    if (!device->seek(static_cast<qint64>(offset))) {
        blockers->append(
            QStringLiteral("Unable to seek APFS zero-fill target: %1").arg(device->errorString()));
        return false;
    }

    const QByteArray zeroChunk(kApfsFormatZeroChunkBytes, '\0');
    uint64_t remaining = length;
    while (remaining > 0) {
        const qsizetype chunk = static_cast<qsizetype>(
            std::min<uint64_t>(remaining, static_cast<uint64_t>(zeroChunk.size())));
        if (device->write(zeroChunk.constData(), chunk) != chunk) {
            blockers->append(
                QStringLiteral("Unable to zero APFS format target: %1").arg(device->errorString()));
            return false;
        }
        remaining -= static_cast<uint64_t>(chunk);
    }
    return true;
}

bool zeroFormatStaleSignatureRanges(QIODevice* device,
                                    uint64_t targetBytes,
                                    QStringList* blockers) {
    const uint64_t edgeBytes = std::min(kApfsFormatStaleSignatureClearBytes, targetBytes);
    if (!writeZeroRange(device, 0, edgeBytes, blockers)) {
        return false;
    }
    if (targetBytes <= edgeBytes) {
        return true;
    }
    const uint64_t tailOffset = targetBytes - edgeBytes;
    if (tailOffset <= edgeBytes) {
        return true;
    }
    return writeZeroRange(device, tailOffset, edgeBytes, blockers);
}

void finalizeBuildResult(PartitionApfsImageBuildResult* result) {
    result->ok = result->blockers.isEmpty();
    if (result->ok) {
        result->image_sha256 = fileSha256Hex(result->image_path, &result->blockers);
        result->ok = result->blockers.isEmpty();
    }
    if (!result->ok) {
        QFile::remove(result->image_path);
    }
}

bool appendSeedFileBlockers(const PartitionApfsImageFormatRequest& request,
                            const QString& cleanFileName,
                            PartitionApfsImageBuildResult* result) {
    if (!isSafeSeedFileName(cleanFileName)) {
        result->blockers.append(QStringLiteral(
            "APFS seed-file format supports one root file name without path traversal"));
    }
    if (request.seed_file_data.isEmpty()) {
        result->blockers.append(QStringLiteral("APFS seed-file data must not be empty"));
    }
    if (static_cast<uint64_t>(request.seed_file_data.size()) > kApfsMaximumSeedFileBytes) {
        result->blockers.append(
            QStringLiteral("APFS seed-file data exceeds current image-only certification cap"));
    }
    result->ok = result->blockers.isEmpty();
    return result->ok;
}

struct ApfsRootFileInput {
    QString fileName;
    QString parentDirectoryName;
    QByteArray data;
};

struct ApfsRootDirectoryInput {
    QString directoryName;
};

uint64_t allocatedBlocksForFiles(const QVector<ApfsRootFilePayload>& files, uint32_t blockSize) {
    uint64_t allocatedBlocks = kApfsFormatSeedFileDataBlock;
    for (const auto& file : files) {
        allocatedBlocks =
            std::max(allocatedBlocks,
                     file.dataStartBlock +
                         roundedBlockCount(static_cast<uint64_t>(file.data.size()), blockSize));
    }
    return allocatedBlocks;
}

QVector<ApfsRootDirectoryPayload> assignedRootDirectoryPayloads(
    const QVector<ApfsRootDirectoryInput>& inputs, qsizetype fileCount) {
    QVector<ApfsRootDirectoryPayload> directories;
    directories.reserve(inputs.size());
    for (qsizetype index = 0; index < inputs.size(); ++index) {
        const auto& input = inputs.at(index);
        const uint64_t objectId = kApfsSeedFileId + static_cast<uint64_t>(fileCount) +
                                  static_cast<uint64_t>(index);
        directories.append(
            {.directoryName = input.directoryName, .directoryId = objectId, .privateId = objectId});
    }
    return directories;
}

uint64_t parentDirectoryIdForFile(const ApfsRootFileInput& input,
                                  const QVector<ApfsRootDirectoryPayload>& directories,
                                  QStringList* blockers) {
    if (input.parentDirectoryName.trimmed().isEmpty()) {
        return kApfsRootDirectoryId;
    }
    for (const auto& directory : directories) {
        if (directory.directoryName.compare(input.parentDirectoryName, Qt::CaseInsensitive) == 0) {
            return directory.directoryId;
        }
    }
    blockers->append(QStringLiteral("APFS generated file parent directory was not preserved: %1")
                         .arg(input.parentDirectoryName));
    return 0;
}

QVector<ApfsRootFilePayload> assignedRootFilePayloads(
    const QVector<ApfsRootFileInput>& inputs,
    uint32_t blockSize,
    const QVector<ApfsRootDirectoryPayload>& directories,
    QStringList* blockers) {
    QVector<ApfsRootFilePayload> files;
    files.reserve(inputs.size());
    uint64_t nextDataBlock = kApfsFormatSeedFileDataBlock;
    for (qsizetype index = 0; index < inputs.size(); ++index) {
        const auto& input = inputs.at(index);
        const uint64_t parentDirectoryId = parentDirectoryIdForFile(input, directories, blockers);
        if (parentDirectoryId == 0) {
            continue;
        }
        files.append({.fileName = input.fileName,
                      .parentDirectoryName = input.parentDirectoryName,
                      .data = input.data,
                      .parentDirectoryId = parentDirectoryId,
                      .fileId = kApfsSeedFileId + static_cast<uint64_t>(index),
                      .privateId = kApfsSeedFileId + static_cast<uint64_t>(index),
                      .dataStartBlock = nextDataBlock});
        nextDataBlock += roundedBlockCount(static_cast<uint64_t>(input.data.size()), blockSize);
    }
    return files;
}

void appendFilePayloadDataBlocks(QVector<ApfsImageBlock>* blocks,
                                 const ApfsRootFilePayload& file,
                                 uint32_t blockSize) {
    uint64_t bytesCopied = 0;
    const uint64_t dataBlocks = roundedBlockCount(static_cast<uint64_t>(file.data.size()),
                                                  blockSize);
    for (uint64_t blockIndex = 0; blockIndex < dataBlocks; ++blockIndex) {
        QByteArray dataBlock(static_cast<qsizetype>(blockSize), '\0');
        const uint64_t remaining = static_cast<uint64_t>(file.data.size()) - bytesCopied;
        const uint64_t chunk = std::min<uint64_t>(remaining, blockSize);
        std::copy(file.data.cbegin() + static_cast<qsizetype>(bytesCopied),
                  file.data.cbegin() + static_cast<qsizetype>(bytesCopied + chunk),
                  dataBlock.begin());
        blocks->append({file.dataStartBlock + blockIndex, dataBlock});
        bytesCopied += chunk;
    }
}

struct ApfsSeedRewrite {
    uint32_t blockSize{0};
    uint64_t blockCount{0};
    QString volumeName;
    QVector<ApfsRootFilePayload> files;
    QVector<ApfsRootDirectoryPayload> directories;
    QByteArray volumeUuid;
};

QVector<ApfsImageBlock> seedRewriteBlocks(const ApfsSeedRewrite& rewrite, QStringList* blockers) {
    const uint64_t allocatedBlocks = allocatedBlocksForFiles(rewrite.files, rewrite.blockSize);
    // Volume-owned blocks: the five metadata blocks plus every file-extent
    // block (fsck cross-checks this against its extent traversal).
    const uint64_t volumeAllocatedBlocks = kApfsFormatVolumeBaseAllocatedBlocks +
                                           (allocatedBlocks - kApfsFormatSeedFileDataBlock);
    if (rewrite.volumeUuid.size() != kApfsUuidBytes) {
        if (blockers) {
            blockers->append(QStringLiteral("APFS rewrite is missing the on-disk volume UUID"));
        }
        return {};
    }
    const QByteArray& volumeUuid = rewrite.volumeUuid;
    QVector<ApfsImageBlock> blocks{
        {kApfsFormatVolumeSuperblockBlock,
         buildVolumeSuperblock(
             {.blockSize = rewrite.blockSize,
              .volumeName = rewrite.volumeName,
              .volumeUuid = volumeUuid,
              .allocatedBlocks = volumeAllocatedBlocks,
              .nextObjectId = kApfsFirstUserObjectId + static_cast<uint64_t>(rewrite.files.size()) +
                              static_cast<uint64_t>(rewrite.directories.size()),
              .fileCount = static_cast<uint64_t>(rewrite.files.size()),
              .directoryCount = static_cast<uint64_t>(rewrite.directories.size())},
             blockers)},
        {kApfsFormatRootTreeBlock,
         buildRootTreeBlock(rewrite.blockSize, rewrite.files, rewrite.directories, blockers)},
        {kApfsFormatExtentRefTreeBlock,
         buildExtentRefTreeBlock(rewrite.blockSize, rewrite.files, blockers)},
        {kApfsFormatSpacemanBlock,
         buildSpacemanBlock({.blockSize = rewrite.blockSize,
                             .blockCount = rewrite.blockCount,
                             .reservedBlocks = allocatedBlocks,
                             .xid = kApfsFormatXid,
                             .genesis = false},
                            blockers)},
        {kApfsFormatChunkInfoBlock,
         buildChunkInfoBlock({.blockSize = rewrite.blockSize,
                              .blockCount = rewrite.blockCount,
                              .reservedBlocks = allocatedBlocks,
                              .xid = kApfsFormatXid,
                              .selfBlock = kApfsFormatChunkInfoBlock,
                              .bitmapBlock = kApfsFormatChunkBitmapBlock},
                             blockers)},
        {kApfsFormatChunkBitmapBlock, buildChunkBitmapBlock(rewrite.blockSize, allocatedBlocks)}};
    for (const auto& file : rewrite.files) {
        appendFilePayloadDataBlocks(&blocks, file, rewrite.blockSize);
    }
    return blocks;
}

// Reads the on-disk volume UUID from a generated single-chunk layout so a
// mutation rewrite preserves the container identity instead of minting a new
// one. Mutations are gated to the certified single-chunk layout, where the
// volume superblock sits at the fixed block.
QByteArray readGeneratedVolumeUuid(QIODevice* image,
                                   const ApfsRepairGeometry& geometry,
                                   QStringList* blockers) {
    QByteArray volumeBlock;
    if (!readGeneratedLayoutBlock(
            image, geometry, kApfsFormatVolumeSuperblockBlock, &volumeBlock, blockers)) {
        return {};
    }
    return volumeBlock.mid(kApfsVolumeUuidOffset, kApfsUuidBytes);
}

QVector<ApfsImageBlock> rewriteGeneratedBlocks(QIODevice* image,
                                               ApfsSeedRewrite rewrite,
                                               QStringList* blockers) {
    rewrite.volumeUuid = readGeneratedVolumeUuid(
        image, {.blockSize = rewrite.blockSize, .blockCount = rewrite.blockCount}, blockers);
    return seedRewriteBlocks(rewrite, blockers);
}

struct ApfsImageSource {
    QFileInfo info;
    bool ok{false};
};

ApfsImageSource validateImageOnlySource(const QString& path,
                                        QLatin1StringView purpose,
                                        QStringList* blockers) {
    ApfsImageSource source{QFileInfo(path), false};
    if (!source.info.exists() || !source.info.isFile()) {
        blockers->append(QStringLiteral("APFS %1 source image is required").arg(purpose));
        return source;
    }
    if (source.info.size() <= 0) {
        blockers->append(QStringLiteral("APFS %1 source image is empty").arg(purpose));
        return source;
    }
    if (static_cast<uint64_t>(source.info.size()) > kDefaultMaxApfsPayloadBytes) {
        blockers->append(
            QStringLiteral("APFS %1 source image exceeds current image-only certification cap")
                .arg(purpose));
        return source;
    }
    source.ok = true;
    return source;
}

bool appendSeparateOutputBlockers(const QFileInfo& sourceInfo,
                                  const QString& outputPath,
                                  QLatin1StringView purpose,
                                  QStringList* blockers) {
    if (!imagePathIsSafeForCreate(outputPath, blockers)) {
        return false;
    }
    if (QFileInfo(outputPath).absoluteFilePath() == sourceInfo.absoluteFilePath()) {
        blockers->append(
            QStringLiteral("APFS %1 output path must be separate from the source image")
                .arg(purpose));
        return false;
    }
    return true;
}

std::optional<PartitionFileSystemDetection> detectApfsImageSource(const QString& imagePath,
                                                                  uint64_t imageSize,
                                                                  QLatin1StringView purpose,
                                                                  QStringList* blockers) {
    QString detectError;
    const auto detection =
        PartitionFileSystemDetector::detectFromDevicePath(imagePath, 0, imageSize, &detectError);
    if (detection.has_value() &&
        detection->file_system.compare(QStringLiteral("APFS"), Qt::CaseInsensitive) == 0) {
        return detection;
    }
    blockers->append(detectError.isEmpty()
                         ? QStringLiteral("APFS %1 source was not detected as APFS").arg(purpose)
                         : detectError);
    return std::nullopt;
}

bool copyToScratchImage(const QString& sourcePath,
                        const QString& outputPath,
                        QLatin1StringView purpose,
                        QStringList* blockers) {
    if (QFile::copy(sourcePath, outputPath)) {
        return true;
    }
    blockers->append(QStringLiteral("Unable to create APFS %1 scratch image").arg(purpose));
    return false;
}

bool openScratchImage(const QString& imagePath,
                      QLatin1StringView purpose,
                      QFile* image,
                      QStringList* blockers) {
    image->setFileName(imagePath);
    if (image->open(QIODevice::ReadWrite)) {
        return true;
    }
    blockers->append(QStringLiteral("Unable to open APFS %1 scratch image: %2")
                         .arg(purpose, image->errorString()));
    QFile::remove(imagePath);
    return false;
}

bool appendPlanResult(const PartitionApfsImageMutationPlan& plan,
                      QStringList* blockers,
                      QStringList* warnings) {
    blockers->append(plan.preflight.blockers);
    warnings->append(plan.preflight.warnings);
    return plan.buildable;
}

bool runChecksumRepairOnScratch(QFile* image,
                                PartitionApfsImageRepairResult* result,
                                QStringList* blockers) {
    uint32_t blockSize = 0;
    uint64_t blockCount = 0;
    if (!readApfsRepairGeometry(image, &blockSize, &blockCount, blockers)) {
        return false;
    }
    if (!appendGeneratedApfsLayoutBlockers(image,
                                           {.blockSize = blockSize, .blockCount = blockCount},
                                           QLatin1StringView("repair"),
                                           true,
                                           blockers)) {
        return false;
    }
    ApfsRepairCounters counters;
    repairGeneratedApfsObjectChecksumBlocks(
        image, {.blockSize = blockSize, .blockCount = blockCount}, &counters, blockers);
    result->scanned_blocks = counters.scannedBlocks;
    result->repaired_checksum_blocks = counters.repairedBlocks;
    return blockers->isEmpty();
}

bool appendFileWritePayloadBlockers(const PartitionApfsImageRootFileWriteRequest& request,
                                    const QString& cleanFileName,
                                    QStringList* blockers) {
    if (!isSafeSeedFileName(cleanFileName)) {
        blockers->append(QStringLiteral(
            "APFS file-write certification supports one root file name without path traversal"));
    }
    if (request.file_data.isEmpty()) {
        blockers->append(QStringLiteral("APFS file-write data must not be empty"));
    }
    if (static_cast<uint64_t>(request.file_data.size()) > kApfsMaximumSeedFileBytes) {
        blockers->append(
            QStringLiteral("APFS file-write data exceeds current image-only certification cap"));
    }
    if (request.options.max_payload_bytes > 0 &&
        static_cast<uint64_t>(request.file_data.size()) > request.options.max_payload_bytes) {
        blockers->append(QStringLiteral("APFS file-write data exceeds configured payload bound"));
    }
    return blockers->isEmpty();
}

bool appendRootFileNameBlockers(const QString& cleanFileName,
                                QLatin1StringView purpose,
                                QStringList* blockers) {
    if (!isSafeSeedFileName(cleanFileName)) {
        blockers->append(
            QStringLiteral(
                "APFS %1 certification supports one root file name without path traversal")
                .arg(purpose));
    }
    return blockers->isEmpty();
}

bool appendFilePatchPayloadBlockers(const PartitionApfsImageRootFilePatchRequest& request,
                                    const QString& cleanFileName,
                                    QStringList* blockers) {
    appendRootFileNameBlockers(cleanFileName, QLatin1StringView("file-patch"), blockers);
    if (request.patch_data.isEmpty()) {
        blockers->append(QStringLiteral("APFS file-patch data must not be empty"));
    }
    if (static_cast<uint64_t>(request.patch_data.size()) > kApfsMaximumSeedFileBytes) {
        blockers->append(
            QStringLiteral("APFS file-patch data exceeds current image-only certification cap"));
    }
    if (request.options.max_payload_bytes > 0 &&
        static_cast<uint64_t>(request.patch_data.size()) > request.options.max_payload_bytes) {
        blockers->append(QStringLiteral("APFS file-patch data exceeds configured payload bound"));
    }
    return blockers->isEmpty();
}

PartitionApfsFileReadResult rootListingForWrite(const QString& imagePath, QStringList* blockers) {
    auto listing = PartitionApfsFileSystemReader::listDirectoryFromImage(
        imagePath, QStringLiteral("/"), kApfsWriteRootListingMaxEntries);
    if (!listing.ok) {
        blockers->append(QStringLiteral("APFS file-write source root listing failed: %1")
                             .arg(listing.blockers.join(QStringLiteral("; "))));
    }
    return listing;
}

bool appendGeneratedSourceLayoutBlockers(const QString& sourcePath,
                                         QLatin1StringView purpose,
                                         QStringList* blockers) {
    QString openError;
    auto source = openFileOrRawDeviceReadOnly(sourcePath, &openError);
    if (!source) {
        blockers->append(QStringLiteral("Unable to open APFS %1 source for layout proof: %2")
                             .arg(purpose, openError));
        return false;
    }

    uint32_t blockSize = 0;
    uint64_t blockCount = 0;
    if (!readApfsRepairGeometry(source.get(), &blockSize, &blockCount, blockers)) {
        return false;
    }
    return appendGeneratedApfsLayoutBlockers(
        source.get(), {.blockSize = blockSize, .blockCount = blockCount}, purpose, false, blockers);
}

bool rootFileWriteReplacesExisting(const PartitionApfsFileReadResult& rootListing,
                                   const QString& replacementName,
                                   QStringList* blockers) {
    for (const auto& entry : rootListing.entries) {
        if (entry.name != replacementName) {
            continue;
        }
        if (!entry.regular_file || entry.size_bytes > kApfsMaximumSeedFileBytes) {
            blockers->append(QStringLiteral(
                "APFS file-write certification can replace only bounded root regular files"));
        }
        return true;
    }
    return false;
}

std::optional<PartitionApfsFileEntry> boundedRootFileEntry(
    const PartitionApfsFileReadResult& rootListing,
    const QString& fileName,
    QLatin1StringView purpose,
    QStringList* blockers) {
    for (const auto& entry : rootListing.entries) {
        if (entry.name != fileName) {
            continue;
        }
        if (!entry.regular_file || entry.size_bytes > kApfsMaximumSeedFileBytes) {
            blockers->append(
                QStringLiteral("APFS %1 certification can mutate only bounded root regular files")
                    .arg(purpose));
            return std::nullopt;
        }
        return entry;
    }
    blockers->append(QStringLiteral("APFS %1 target root file was not found").arg(purpose));
    return std::nullopt;
}

std::optional<QByteArray> readBoundedRootFile(const QString& imagePath,
                                              const PartitionApfsFileEntry& entry,
                                              QLatin1StringView purpose,
                                              QStringList* blockers) {
    const auto readBack = PartitionApfsFileSystemReader::readFileFromImage(
        imagePath, entry.path, kApfsMaximumSeedFileBytes);
    if (!readBack.ok || static_cast<uint64_t>(readBack.data.size()) != entry.size_bytes) {
        blockers->append(
            QStringLiteral("APFS %1 could not read target root file %2").arg(purpose, entry.name));
        return std::nullopt;
    }
    return readBack.data;
}

bool appendEmptyRootDirectoryInput(const QString& imagePath,
                                   const PartitionApfsFileEntry& entry,
                                   QVector<ApfsRootDirectoryInput>* directories,
                                   QStringList* blockers) {
    const auto listing =
        PartitionApfsFileSystemReader::listDirectoryFromImage(imagePath, entry.path, 1);
    if (!listing.ok) {
        blockers->append(
            QStringLiteral("APFS directory mutation could not verify root directory %1: %2")
                .arg(entry.name, listing.blockers.join(QStringLiteral("; "))));
        return false;
    }
    if (!listing.entries.isEmpty()) {
        blockers->append(QStringLiteral(
            "APFS generated-layout mutation can preserve only empty root directories"));
        return false;
    }
    directories->append({entry.name});
    return true;
}

struct ApfsRootPreservationLists {
    QVector<ApfsRootFileInput>* files{nullptr};
    QVector<ApfsRootDirectoryInput>* directories{nullptr};
};

bool appendExistingRootEntries(const QString& imagePath,
                               const PartitionApfsFileReadResult& rootListing,
                               const QString& replacementName,
                               ApfsRootPreservationLists preservation,
                               QStringList* blockers) {
    for (const auto& entry : rootListing.entries) {
        if (entry.name == replacementName) {
            continue;
        }
        if (entry.directory) {
            if (!appendEmptyRootDirectoryInput(
                    imagePath, entry, preservation.directories, blockers)) {
                return false;
            }
            continue;
        }
        if (!entry.regular_file || entry.size_bytes > kApfsMaximumSeedFileBytes) {
            blockers->append(
                QStringLiteral("APFS generated-layout mutation can preserve only bounded root "
                               "regular files and empty root directories"));
            return false;
        }
        const auto readBack = PartitionApfsFileSystemReader::readFileFromImage(
            imagePath, entry.path, kApfsMaximumSeedFileBytes);
        if (!readBack.ok || static_cast<uint64_t>(readBack.data.size()) != entry.size_bytes) {
            blockers->append(
                QStringLiteral("APFS file-write could not preserve existing root file %1")
                    .arg(entry.name));
            return false;
        }
        preservation.files->append({.fileName = entry.name, .data = readBack.data});
    }
    return true;
}

struct ApfsBoundedRegularFileAppendContext {
    const QString* imagePath{nullptr};
    QString parentDirectoryName;
    QLatin1StringView purpose;
    QVector<ApfsRootFileInput>* inputs{nullptr};
    QStringList* blockers{nullptr};
};

bool appendBoundedRegularFileInput(const PartitionApfsFileEntry& entry,
                                   const ApfsBoundedRegularFileAppendContext& context) {
    if (!entry.regular_file || entry.size_bytes > kApfsMaximumSeedFileBytes) {
        context.blockers->append(
            QStringLiteral("APFS %1 can preserve only bounded regular files in generated layouts")
                .arg(context.purpose));
        return false;
    }
    const auto readBack = PartitionApfsFileSystemReader::readFileFromImage(
        *context.imagePath, entry.path, kApfsMaximumSeedFileBytes);
    if (!readBack.ok || static_cast<uint64_t>(readBack.data.size()) != entry.size_bytes) {
        context.blockers->append(
            QStringLiteral("APFS %1 could not preserve file %2").arg(context.purpose, entry.path));
        return false;
    }
    context.inputs->append({.fileName = entry.name,
                            .parentDirectoryName = context.parentDirectoryName,
                            .data = readBack.data});
    return true;
}

bool appendRootDirectoryChildFileInputs(const QString& imagePath,
                                        const PartitionApfsFileEntry& directory,
                                        const QString& replacementChildName,
                                        QVector<ApfsRootFileInput>* inputs,
                                        QStringList* blockers) {
    const auto listing = PartitionApfsFileSystemReader::listDirectoryFromImage(
        imagePath, directory.path, kApfsWriteRootListingMaxEntries);
    if (!listing.ok) {
        blockers->append(QStringLiteral("APFS directory-file mutation could not list %1: %2")
                             .arg(directory.path, listing.blockers.join(QStringLiteral("; "))));
        return false;
    }
    if (listing.entries.size() >= kApfsWriteRootListingMaxEntries) {
        blockers->append(QStringLiteral(
            "APFS generated root-directory child preservation exceeded entry limit"));
        return false;
    }
    const ApfsBoundedRegularFileAppendContext context{.imagePath = &imagePath,
                                                      .parentDirectoryName = directory.name,
                                                      .purpose = QLatin1StringView(
                                                          "directory-file mutation"),
                                                      .inputs = inputs,
                                                      .blockers = blockers};
    for (const auto& child : listing.entries) {
        if (!replacementChildName.isEmpty() &&
            child.name.compare(replacementChildName, Qt::CaseInsensitive) == 0) {
            continue;
        }
        if (!appendBoundedRegularFileInput(child, context)) {
            return false;
        }
    }
    return true;
}

struct ApfsDirectoryFilePreservationContext {
    const QString* imagePath{nullptr};
    QString targetDirectoryName;
    QString replacementChildName;
    ApfsRootPreservationLists preservation;
    QStringList* blockers{nullptr};
};

bool appendExistingRootEntriesWithDirectoryFiles(
    const PartitionApfsFileReadResult& rootListing,
    const ApfsDirectoryFilePreservationContext& context) {
    bool foundTargetDirectory = false;
    for (const auto& entry : rootListing.entries) {
        if (entry.directory) {
            const bool isTargetDirectory = entry.name.compare(context.targetDirectoryName,
                                                              Qt::CaseInsensitive) == 0;
            foundTargetDirectory = foundTargetDirectory || isTargetDirectory;
            context.preservation.directories->append({entry.name});
            if (!appendRootDirectoryChildFileInputs(*context.imagePath,
                                                    entry,
                                                    isTargetDirectory ? context.replacementChildName
                                                                      : QString(),
                                                    context.preservation.files,
                                                    context.blockers)) {
                return false;
            }
            continue;
        }
        const ApfsBoundedRegularFileAppendContext fileContext{.imagePath = context.imagePath,
                                                              .parentDirectoryName = QString(),
                                                              .purpose = QLatin1StringView(
                                                                  "directory-file mutation"),
                                                              .inputs = context.preservation.files,
                                                              .blockers = context.blockers};
        if (!appendBoundedRegularFileInput(entry, fileContext)) {
            return false;
        }
    }
    if (!foundTargetDirectory) {
        context.blockers->append(
            QStringLiteral("APFS directory-file mutation target root directory was not found"));
        return false;
    }
    return true;
}

bool appendExistingRootFileInputs(const QString& imagePath,
                                  const PartitionApfsFileReadResult& rootListing,
                                  const QString& replacementName,
                                  QVector<ApfsRootFileInput>* inputs,
                                  QStringList* blockers) {
    QVector<ApfsRootDirectoryInput> ignoredDirectories;
    return appendExistingRootEntries(
        imagePath, rootListing, replacementName, {inputs, &ignoredDirectories}, blockers);
}

struct ApfsRootFileWriteScratch {
    QIODevice* image{nullptr};
    QString sourceImagePath;
    QString fileName;
    QByteArray fileData;
    PartitionApfsFileReadResult rootListing;
    uint64_t* writtenDataBlocks{nullptr};
};

bool runRootFileWriteOnScratch(const ApfsRootFileWriteScratch& scratch, QStringList* blockers) {
    uint32_t blockSize = 0;
    uint64_t blockCount = 0;
    if (!readApfsRepairGeometry(scratch.image, &blockSize, &blockCount, blockers)) {
        return false;
    }
    if (!appendGeneratedApfsLayoutBlockers(scratch.image,
                                           {.blockSize = blockSize, .blockCount = blockCount},
                                           QLatin1StringView("file-write"),
                                           false,
                                           blockers)) {
        return false;
    }
    QVector<ApfsRootFileInput> inputs;
    QVector<ApfsRootDirectoryInput> directories;
    if (!appendExistingRootEntries(scratch.sourceImagePath,
                                   scratch.rootListing,
                                   scratch.fileName,
                                   {&inputs, &directories},
                                   blockers)) {
        return false;
    }
    inputs.append({.fileName = scratch.fileName, .data = scratch.fileData});
    const auto rootDirectories = assignedRootDirectoryPayloads(directories, inputs.size());
    const auto files = assignedRootFilePayloads(inputs, blockSize, rootDirectories, blockers);
    if (!blockers->isEmpty()) {
        return false;
    }
    const uint64_t dataBlocks = roundedBlockCount(static_cast<uint64_t>(scratch.fileData.size()),
                                                  blockSize);
    if (allocatedBlocksForFiles(files, blockSize) > blockCount) {
        blockers->append(QStringLiteral("APFS file-write data exceeds target container size"));
        return false;
    }
    const auto blocks = rewriteGeneratedBlocks(scratch.image,
                                               {
                                                   .blockSize = blockSize,
                                                   .blockCount = blockCount,
                                                   .volumeName = scratch.rootListing.volume_name,
                                                   .files = files,
                                                   .directories = rootDirectories,
                                               },
                                               blockers);
    writeImageBlocks(scratch.image, blockSize, blocks, blockers);
    if (scratch.writtenDataBlocks) {
        *scratch.writtenDataBlocks = dataBlocks;
    }
    return blockers->isEmpty();
}

struct ApfsRootFileDeleteScratch {
    QIODevice* image{nullptr};
    QString sourceImagePath;
    QString fileName;
    PartitionApfsFileReadResult rootListing;
    uint64_t targetSizeBytes{0};
    uint64_t* freedDataBlocks{nullptr};
};

bool zeroGeneratedRootFileDataArea(QIODevice* image,
                                   uint32_t blockSize,
                                   uint64_t oldAllocatedBlocks,
                                   QStringList* blockers) {
    if (oldAllocatedBlocks <= kApfsFormatSeedFileDataBlock) {
        return true;
    }
    const uint64_t offset = kApfsFormatSeedFileDataBlock * blockSize;
    const uint64_t bytes = (oldAllocatedBlocks - kApfsFormatSeedFileDataBlock) * blockSize;
    return writeZeroRange(image, offset, bytes, blockers);
}

bool runRootFileDeleteOnScratch(const ApfsRootFileDeleteScratch& scratch, QStringList* blockers) {
    uint32_t blockSize = 0;
    uint64_t blockCount = 0;
    if (!readApfsRepairGeometry(scratch.image, &blockSize, &blockCount, blockers)) {
        return false;
    }
    if (!appendGeneratedApfsLayoutBlockers(scratch.image,
                                           {.blockSize = blockSize, .blockCount = blockCount},
                                           QLatin1StringView("file-delete"),
                                           false,
                                           blockers)) {
        return false;
    }

    QVector<ApfsRootFileInput> allInputs;
    QVector<ApfsRootFileInput> preservedInputs;
    QVector<ApfsRootDirectoryInput> allDirectories;
    QVector<ApfsRootDirectoryInput> preservedDirectories;
    if (!appendExistingRootEntries(scratch.sourceImagePath,
                                   scratch.rootListing,
                                   QString(),
                                   {&allInputs, &allDirectories},
                                   blockers) ||
        !appendExistingRootEntries(scratch.sourceImagePath,
                                   scratch.rootListing,
                                   scratch.fileName,
                                   {&preservedInputs, &preservedDirectories},
                                   blockers)) {
        return false;
    }

    const auto rootDirectories = assignedRootDirectoryPayloads(preservedDirectories,
                                                               preservedInputs.size());
    const auto oldDirectories = assignedRootDirectoryPayloads(allDirectories, allInputs.size());
    const auto oldFiles = assignedRootFilePayloads(allInputs, blockSize, oldDirectories, blockers);
    const auto preservedFiles =
        assignedRootFilePayloads(preservedInputs, blockSize, rootDirectories, blockers);
    if (!blockers->isEmpty()) {
        return false;
    }
    const uint64_t oldAllocatedBlocks = allocatedBlocksForFiles(oldFiles, blockSize);
    if (allocatedBlocksForFiles(preservedFiles, blockSize) > blockCount) {
        blockers->append(
            QStringLiteral("APFS file-delete preserved data exceeds target container size"));
        return false;
    }
    if (!zeroGeneratedRootFileDataArea(scratch.image, blockSize, oldAllocatedBlocks, blockers)) {
        return false;
    }

    const auto blocks = rewriteGeneratedBlocks(scratch.image,
                                               {
                                                   .blockSize = blockSize,
                                                   .blockCount = blockCount,
                                                   .volumeName = scratch.rootListing.volume_name,
                                                   .files = preservedFiles,
                                                   .directories = rootDirectories,
                                               },
                                               blockers);
    writeImageBlocks(scratch.image, blockSize, blocks, blockers);
    if (scratch.freedDataBlocks) {
        *scratch.freedDataBlocks = roundedBlockCount(scratch.targetSizeBytes, blockSize);
    }
    return blockers->isEmpty();
}

bool rootEntryExists(const PartitionApfsFileReadResult& rootListing,
                     const QString& name,
                     QStringList* blockers) {
    for (const auto& entry : rootListing.entries) {
        if (entry.name.compare(name, Qt::CaseInsensitive) == 0) {
            blockers->append(
                QStringLiteral("APFS generated root-directory create target already exists"));
            return true;
        }
    }
    return false;
}

std::optional<PartitionApfsFileEntry> boundedRootDirectoryEntry(
    const PartitionApfsFileReadResult& rootListing,
    const QString& directoryName,
    QLatin1StringView purpose,
    QStringList* blockers) {
    for (const auto& entry : rootListing.entries) {
        if (entry.name.compare(directoryName, Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (!entry.directory) {
            blockers->append(
                QStringLiteral("APFS %1 certification can mutate only root directories")
                    .arg(purpose));
            return std::nullopt;
        }
        return entry;
    }
    blockers->append(QStringLiteral("APFS %1 target root directory was not found").arg(purpose));
    return std::nullopt;
}

bool appendRootDirectoryNameBlockers(const QString& cleanDirectoryName,
                                     QLatin1StringView purpose,
                                     QStringList* blockers) {
    if (!isSafeSeedFileName(cleanDirectoryName)) {
        blockers->append(
            QStringLiteral(
                "APFS %1 certification supports one root directory name without path traversal")
                .arg(purpose));
    }
    return blockers->isEmpty();
}

bool appendEmptyRootDirectoryDeleteBlockers(const QString& imagePath,
                                            const PartitionApfsFileEntry& entry,
                                            QStringList* blockers) {
    const auto listing =
        PartitionApfsFileSystemReader::listDirectoryFromImage(imagePath, entry.path, 1);
    if (!listing.ok) {
        blockers->append(
            QStringLiteral("APFS directory-delete could not verify root directory %1: %2")
                .arg(entry.name, listing.blockers.join(QStringLiteral("; "))));
        return false;
    }
    if (!listing.entries.isEmpty()) {
        blockers->append(QStringLiteral(
            "APFS generated root-directory delete is limited to empty root directories"));
        return false;
    }
    return true;
}

struct ApfsRootDirectoryRewriteScratch {
    QIODevice* image{nullptr};
    QString sourceImagePath;
    QString directoryName;
    PartitionApfsFileReadResult rootListing;
    bool createDirectory{false};
};

bool runRootDirectoryRewriteOnScratch(const ApfsRootDirectoryRewriteScratch& scratch,
                                      QStringList* blockers) {
    uint32_t blockSize = 0;
    uint64_t blockCount = 0;
    if (!readApfsRepairGeometry(scratch.image, &blockSize, &blockCount, blockers)) {
        return false;
    }
    if (!appendGeneratedApfsLayoutBlockers(scratch.image,
                                           {.blockSize = blockSize, .blockCount = blockCount},
                                           scratch.createDirectory
                                               ? QLatin1StringView("directory-create")
                                               : QLatin1StringView("directory-delete"),
                                           false,
                                           blockers)) {
        return false;
    }

    QVector<ApfsRootFileInput> inputs;
    QVector<ApfsRootDirectoryInput> directories;
    if (!appendExistingRootEntries(scratch.sourceImagePath,
                                   scratch.rootListing,
                                   scratch.createDirectory ? QString() : scratch.directoryName,
                                   {&inputs, &directories},
                                   blockers)) {
        return false;
    }
    if (scratch.createDirectory) {
        directories.append({scratch.directoryName});
    }

    const auto rootDirectories = assignedRootDirectoryPayloads(directories, inputs.size());
    const auto files = assignedRootFilePayloads(inputs, blockSize, rootDirectories, blockers);
    if (!blockers->isEmpty()) {
        return false;
    }
    if (allocatedBlocksForFiles(files, blockSize) > blockCount) {
        blockers->append(
            QStringLiteral("APFS directory mutation preserved data exceeds target container size"));
        return false;
    }
    const auto blocks = rewriteGeneratedBlocks(scratch.image,
                                               {
                                                   .blockSize = blockSize,
                                                   .blockCount = blockCount,
                                                   .volumeName = scratch.rootListing.volume_name,
                                                   .files = files,
                                                   .directories = rootDirectories,
                                               },
                                               blockers);
    writeImageBlocks(scratch.image, blockSize, blocks, blockers);
    return blockers->isEmpty();
}

bool runVolumeLabelChangeOnTarget(QIODevice* image,
                                  uint64_t expectedContainerBytes,
                                  const QString& cleanVolumeName,
                                  QString* oldVolumeName,
                                  QStringList* blockers) {
    uint32_t blockSize = 0;
    uint64_t blockCount = 0;
    if (!readApfsRepairGeometry(image, &blockSize, &blockCount, blockers)) {
        return false;
    }
    if (expectedContainerBytes > 0 && (expectedContainerBytes % blockSize != 0 ||
                                       blockCount != expectedContainerBytes / blockSize)) {
        blockers->append(QStringLiteral(
            "APFS volume-label target geometry does not match expected container size"));
        return false;
    }
    const ApfsRepairGeometry geometry{.blockSize = blockSize, .blockCount = blockCount};
    if (!appendGeneratedApfsLayoutBlockers(
            image, geometry, QLatin1StringView("volume-label"), false, blockers)) {
        return false;
    }

    QByteArray volumeBlock;
    if (!readGeneratedLayoutBlock(
            image, geometry, kApfsFormatVolumeSuperblockBlock, &volumeBlock, blockers)) {
        return false;
    }
    if (oldVolumeName) {
        *oldVolumeName =
            nulTerminatedUtf8Field(volumeBlock, kApfsVolumeNameOffset, kApfsVolumeNameBytes);
    }
    writeAscii(&volumeBlock, kApfsVolumeNameOffset, cleanVolumeName.toUtf8(), kApfsVolumeNameBytes);
    if (!stampApfsObjectBlock(&volumeBlock, blockers)) {
        return false;
    }
    return writeApfsRepairBlock(
        image, geometry, kApfsFormatVolumeSuperblockBlock, volumeBlock, blockers);
}

QString rootDirectoryFilePath(const QString& directoryName, const QString& fileName) {
    return QStringLiteral("/%1/%2").arg(directoryName, fileName);
}

bool appendDirectoryFileNameBlockers(const QString& cleanDirectoryName,
                                     const QString& cleanFileName,
                                     QLatin1StringView purpose,
                                     QStringList* blockers) {
    appendRootDirectoryNameBlockers(cleanDirectoryName, purpose, blockers);
    appendRootFileNameBlockers(cleanFileName, purpose, blockers);
    return blockers->isEmpty();
}

bool appendDirectoryFileWritePayloadBlockers(
    const PartitionApfsImageRootDirectoryFileWriteRequest& request,
    const QString& cleanDirectoryName,
    const QString& cleanFileName,
    QStringList* blockers) {
    appendDirectoryFileNameBlockers(
        cleanDirectoryName, cleanFileName, QLatin1StringView("directory-file-write"), blockers);
    if (request.file_data.isEmpty()) {
        blockers->append(QStringLiteral("APFS directory-file-write data must not be empty"));
    }
    if (static_cast<uint64_t>(request.file_data.size()) > kApfsMaximumSeedFileBytes) {
        blockers->append(
            QStringLiteral("APFS directory-file-write data exceeds current certification cap"));
    }
    if (request.options.max_payload_bytes > 0 &&
        static_cast<uint64_t>(request.file_data.size()) > request.options.max_payload_bytes) {
        blockers->append(
            QStringLiteral("APFS directory-file-write data exceeds configured payload bound"));
    }
    return blockers->isEmpty();
}

bool appendDirectoryFilePatchPayloadBlockers(
    const PartitionApfsImageRootDirectoryFilePatchRequest& request,
    const QString& cleanDirectoryName,
    const QString& cleanFileName,
    QStringList* blockers) {
    appendDirectoryFileNameBlockers(
        cleanDirectoryName, cleanFileName, QLatin1StringView("directory-file-patch"), blockers);
    if (request.patch_data.isEmpty()) {
        blockers->append(QStringLiteral("APFS directory-file-patch data must not be empty"));
    }
    if (static_cast<uint64_t>(request.patch_data.size()) > kApfsMaximumSeedFileBytes) {
        blockers->append(
            QStringLiteral("APFS directory-file-patch data exceeds current certification cap"));
    }
    if (request.options.max_payload_bytes > 0 &&
        static_cast<uint64_t>(request.patch_data.size()) > request.options.max_payload_bytes) {
        blockers->append(
            QStringLiteral("APFS directory-file-patch data exceeds configured payload bound"));
    }
    return blockers->isEmpty();
}

PartitionApfsFileReadResult directoryListingForWrite(const QString& imagePath,
                                                     const PartitionApfsFileEntry& directory,
                                                     QLatin1StringView purpose,
                                                     QStringList* blockers) {
    auto listing = PartitionApfsFileSystemReader::listDirectoryFromImage(
        imagePath, directory.path, kApfsWriteRootListingMaxEntries);
    if (!listing.ok) {
        blockers->append(QStringLiteral("APFS %1 source directory listing failed: %2")
                             .arg(purpose, listing.blockers.join(QStringLiteral("; "))));
    }
    return listing;
}

bool directoryFileWriteReplacesExisting(const PartitionApfsFileReadResult& directoryListing,
                                        const QString& replacementName,
                                        QStringList* blockers) {
    for (const auto& entry : directoryListing.entries) {
        if (entry.name.compare(replacementName, Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (!entry.regular_file || entry.size_bytes > kApfsMaximumSeedFileBytes) {
            blockers->append(
                QStringLiteral("APFS directory-file-write can replace only bounded regular files"));
        }
        return true;
    }
    return false;
}

std::optional<PartitionApfsFileEntry> boundedDirectoryFileEntry(
    const PartitionApfsFileReadResult& directoryListing,
    const QString& fileName,
    QLatin1StringView purpose,
    QStringList* blockers) {
    for (const auto& entry : directoryListing.entries) {
        if (entry.name.compare(fileName, Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (!entry.regular_file || entry.size_bytes > kApfsMaximumSeedFileBytes) {
            blockers->append(
                QStringLiteral("APFS %1 can mutate only bounded directory regular files")
                    .arg(purpose));
            return std::nullopt;
        }
        return entry;
    }
    blockers->append(QStringLiteral("APFS %1 target directory file was not found").arg(purpose));
    return std::nullopt;
}

struct ApfsRootDirectoryFileScratch {
    QIODevice* image{nullptr};
    QString sourceImagePath;
    QString directoryName;
    QString fileName;
    QByteArray fileData;
    PartitionApfsFileReadResult rootListing;
    uint64_t targetSizeBytes{0};
    uint64_t* changedDataBlocks{nullptr};
    bool deleteFile{false};
};

struct ApfsRootDirectoryFileInputs {
    QVector<ApfsRootFileInput> inputs;
    QVector<ApfsRootDirectoryInput> directories;
    QVector<ApfsRootFileInput> oldInputs;
    QVector<ApfsRootDirectoryInput> oldDirectories;
};

struct ApfsRootDirectoryFilePayloads {
    QVector<ApfsRootFilePayload> files;
    QVector<ApfsRootDirectoryPayload> rootDirectories;
    QVector<ApfsRootFilePayload> oldFiles;
};

QLatin1StringView apfsDirectoryFilePurpose(bool deleteFile) {
    return deleteFile ? QLatin1StringView("directory-file-delete")
                      : QLatin1StringView("directory-file-write");
}

bool collectRootDirectoryFileInputs(const ApfsRootDirectoryFileScratch& scratch,
                                    ApfsRootDirectoryFileInputs* payloads,
                                    QStringList* blockers) {
    if (scratch.deleteFile &&
        !appendExistingRootEntriesWithDirectoryFiles(scratch.rootListing,
                                                     {.imagePath = &scratch.sourceImagePath,
                                                      .targetDirectoryName = scratch.directoryName,
                                                      .replacementChildName = QString(),
                                                      .preservation = {&payloads->oldInputs,
                                                                       &payloads->oldDirectories},
                                                      .blockers = blockers})) {
        return false;
    }
    if (!appendExistingRootEntriesWithDirectoryFiles(scratch.rootListing,
                                                     {.imagePath = &scratch.sourceImagePath,
                                                      .targetDirectoryName = scratch.directoryName,
                                                      .replacementChildName = scratch.fileName,
                                                      .preservation = {&payloads->inputs,
                                                                       &payloads->directories},
                                                      .blockers = blockers})) {
        return false;
    }
    if (!scratch.deleteFile) {
        payloads->inputs.append({.fileName = scratch.fileName,
                                 .parentDirectoryName = scratch.directoryName,
                                 .data = scratch.fileData});
    }
    return true;
}

bool assignRootDirectoryFilePayloads(const ApfsRootDirectoryFileInputs& inputs,
                                     uint32_t blockSize,
                                     ApfsRootDirectoryFilePayloads* payloads,
                                     QStringList* blockers) {
    payloads->rootDirectories = assignedRootDirectoryPayloads(inputs.directories,
                                                              inputs.inputs.size());
    payloads->files =
        assignedRootFilePayloads(inputs.inputs, blockSize, payloads->rootDirectories, blockers);
    const auto oldRootDirectories = assignedRootDirectoryPayloads(inputs.oldDirectories,
                                                                  inputs.oldInputs.size());
    payloads->oldFiles =
        assignedRootFilePayloads(inputs.oldInputs, blockSize, oldRootDirectories, blockers);
    if (!blockers->isEmpty()) {
        return false;
    }
    return true;
}

bool validateRootDirectoryFileCapacity(const ApfsRootDirectoryFilePayloads& payloads,
                                       uint32_t blockSize,
                                       uint64_t blockCount,
                                       QStringList* blockers) {
    if (allocatedBlocksForFiles(payloads.files, blockSize) > blockCount) {
        blockers->append(QStringLiteral(
            "APFS directory-file mutation preserved data exceeds target container size"));
        return false;
    }
    return true;
}

bool updateRootDirectoryFileChangedBlocks(const ApfsRootDirectoryFileScratch& scratch,
                                          uint32_t blockSize,
                                          const ApfsRootDirectoryFilePayloads& payloads,
                                          QStringList* blockers) {
    if (scratch.deleteFile) {
        if (!zeroGeneratedRootFileDataArea(scratch.image,
                                           blockSize,
                                           allocatedBlocksForFiles(payloads.oldFiles, blockSize),
                                           blockers)) {
            return false;
        }
        const uint64_t oldBlocks = roundedBlockCount(scratch.targetSizeBytes, blockSize);
        if (scratch.changedDataBlocks) {
            *scratch.changedDataBlocks = oldBlocks;
        }
        return true;
    }
    if (scratch.changedDataBlocks) {
        *scratch.changedDataBlocks =
            roundedBlockCount(static_cast<uint64_t>(scratch.fileData.size()), blockSize);
    }
    return true;
}

bool runRootDirectoryFileRewriteOnScratch(const ApfsRootDirectoryFileScratch& scratch,
                                          QStringList* blockers) {
    uint32_t blockSize = 0;
    uint64_t blockCount = 0;
    if (!readApfsRepairGeometry(scratch.image, &blockSize, &blockCount, blockers)) {
        return false;
    }
    if (!appendGeneratedApfsLayoutBlockers(scratch.image,
                                           {.blockSize = blockSize, .blockCount = blockCount},
                                           apfsDirectoryFilePurpose(scratch.deleteFile),
                                           false,
                                           blockers)) {
        return false;
    }
    ApfsRootDirectoryFileInputs inputs;
    ApfsRootDirectoryFilePayloads payloads;
    if (!collectRootDirectoryFileInputs(scratch, &inputs, blockers) ||
        !assignRootDirectoryFilePayloads(inputs, blockSize, &payloads, blockers) ||
        !validateRootDirectoryFileCapacity(payloads, blockSize, blockCount, blockers) ||
        !updateRootDirectoryFileChangedBlocks(scratch, blockSize, payloads, blockers)) {
        return false;
    }
    const auto blocks = rewriteGeneratedBlocks(scratch.image,
                                               {
                                                   .blockSize = blockSize,
                                                   .blockCount = blockCount,
                                                   .volumeName = scratch.rootListing.volume_name,
                                                   .files = payloads.files,
                                                   .directories = payloads.rootDirectories,
                                               },
                                               blockers);
    writeImageBlocks(scratch.image, blockSize, blocks, blockers);
    return blockers->isEmpty();
}

struct ApfsRootFilePlanContext {
    const PartitionFileSystemDetection* detection{nullptr};
    const PartitionApfsWriteOptions* options{nullptr};
    QString sourceImagePath;
    QString cleanFileName;
    QString cleanDirectoryName;
    PartitionApfsImageMutationPlan* plan{nullptr};
    QStringList* blockers{nullptr};
    QStringList* warnings{nullptr};
};

std::optional<PartitionApfsFileReadResult> prepareRootFileWritePlan(
    const ApfsRootFilePlanContext& context) {
    auto rootListing = rootListingForWrite(context.sourceImagePath, context.blockers);
    const bool replacingExisting =
        rootFileWriteReplacesExisting(rootListing, context.cleanFileName, context.blockers);
    if (!context.blockers->isEmpty()) {
        return std::nullopt;
    }

    PartitionApfsWriteOptions planOptions = *context.options;
    if (replacingExisting && !planOptions.allow_compressed_file_mutation) {
        QStringList layoutBlockers;
        if (!appendGeneratedSourceLayoutBlockers(
                context.sourceImagePath, QLatin1StringView("file replacement"), &layoutBlockers)) {
            context.blockers->append(
                QStringLiteral("APFS file replacement without compressed/encrypted override "
                               "requires S.A.K.-generated layout proof"));
            context.blockers->append(layoutBlockers);
            return std::nullopt;
        }
        planOptions.allow_compressed_file_mutation = true;
    }

    *context.plan = PartitionApfsWriter::planImageOnlyMutation(
        *context.detection,
        replacingExisting ? PartitionApfsWriteOperation::ReplaceFile
                          : PartitionApfsWriteOperation::CreateFile,
        planOptions,
        QStringLiteral("/%1").arg(context.cleanFileName));
    if (!appendPlanResult(*context.plan, context.blockers, context.warnings)) {
        return std::nullopt;
    }
    return rootListing;
}

std::optional<PartitionApfsFileReadResult> prepareRootFileDeletePlan(
    const ApfsRootFilePlanContext& context,
    PartitionApfsFileEntry* deletedEntry,
    QByteArray* deletedData) {
    auto rootListing = rootListingForWrite(context.sourceImagePath, context.blockers);
    const auto target = boundedRootFileEntry(
        rootListing, context.cleanFileName, QLatin1StringView("file-delete"), context.blockers);
    if (!target.has_value()) {
        return std::nullopt;
    }
    const auto targetData = readBoundedRootFile(
        context.sourceImagePath, *target, QLatin1StringView("file-delete"), context.blockers);
    if (!targetData.has_value()) {
        return std::nullopt;
    }

    PartitionApfsWriteOptions planOptions = *context.options;
    QStringList layoutBlockers;
    if (!appendGeneratedSourceLayoutBlockers(
            context.sourceImagePath, QLatin1StringView("file deletion"), &layoutBlockers)) {
        context.blockers->append(
            QStringLiteral("APFS file deletion requires S.A.K.-generated layout proof"));
        context.blockers->append(layoutBlockers);
        return std::nullopt;
    }
    planOptions.allow_compressed_file_mutation = true;

    *context.plan = PartitionApfsWriter::planImageOnlyMutation(
        *context.detection,
        PartitionApfsWriteOperation::DeleteFile,
        planOptions,
        QStringLiteral("/%1").arg(context.cleanFileName));
    if (!appendPlanResult(*context.plan, context.blockers, context.warnings)) {
        return std::nullopt;
    }

    if (deletedEntry) {
        *deletedEntry = *target;
    }
    if (deletedData) {
        *deletedData = *targetData;
    }
    return rootListing;
}

std::optional<PartitionApfsFileReadResult> prepareRootDirectoryFileWritePlan(
    const ApfsRootFilePlanContext& context) {
    auto rootListing = rootListingForWrite(context.sourceImagePath, context.blockers);
    const auto directory = boundedRootDirectoryEntry(rootListing,
                                                     context.cleanDirectoryName,
                                                     QLatin1StringView("directory-file-write"),
                                                     context.blockers);
    if (!directory.has_value()) {
        return std::nullopt;
    }
    const auto directoryListing =
        directoryListingForWrite(context.sourceImagePath,
                                 *directory,
                                 QLatin1StringView("directory-file-write"),
                                 context.blockers);
    const bool replacingExisting = directoryFileWriteReplacesExisting(directoryListing,
                                                                      context.cleanFileName,
                                                                      context.blockers);
    if (!context.blockers->isEmpty()) {
        return std::nullopt;
    }

    QStringList layoutBlockers;
    PartitionApfsWriteOptions planOptions = *context.options;
    if (replacingExisting &&
        !appendGeneratedSourceLayoutBlockers(context.sourceImagePath,
                                             QLatin1StringView("directory-file replacement"),
                                             &layoutBlockers)) {
        context.blockers->append(QStringLiteral(
            "APFS directory-file replacement requires S.A.K.-generated layout proof"));
        context.blockers->append(layoutBlockers);
        return std::nullopt;
    }
    if (replacingExisting) {
        planOptions.allow_compressed_file_mutation = true;
    }

    *context.plan = PartitionApfsWriter::planImageOnlyMutation(
        *context.detection,
        replacingExisting ? PartitionApfsWriteOperation::ReplaceFile
                          : PartitionApfsWriteOperation::CreateFile,
        planOptions,
        rootDirectoryFilePath(context.cleanDirectoryName, context.cleanFileName));
    if (!appendPlanResult(*context.plan, context.blockers, context.warnings)) {
        return std::nullopt;
    }
    return rootListing;
}

std::optional<PartitionApfsFileReadResult> prepareRootDirectoryFileDeletePlan(
    const ApfsRootFilePlanContext& context,
    PartitionApfsFileEntry* deletedEntry,
    QByteArray* deletedData) {
    auto rootListing = rootListingForWrite(context.sourceImagePath, context.blockers);
    const auto directory = boundedRootDirectoryEntry(rootListing,
                                                     context.cleanDirectoryName,
                                                     QLatin1StringView("directory-file-delete"),
                                                     context.blockers);
    if (!directory.has_value()) {
        return std::nullopt;
    }
    const auto directoryListing =
        directoryListingForWrite(context.sourceImagePath,
                                 *directory,
                                 QLatin1StringView("directory-file-delete"),
                                 context.blockers);
    const auto target = boundedDirectoryFileEntry(directoryListing,
                                                  context.cleanFileName,
                                                  QLatin1StringView("directory-file-delete"),
                                                  context.blockers);
    if (!target.has_value()) {
        return std::nullopt;
    }
    const auto targetData = readBoundedRootFile(context.sourceImagePath,
                                                *target,
                                                QLatin1StringView("directory-file-delete"),
                                                context.blockers);
    if (!targetData.has_value()) {
        return std::nullopt;
    }

    QStringList layoutBlockers;
    PartitionApfsWriteOptions planOptions = *context.options;
    if (!appendGeneratedSourceLayoutBlockers(context.sourceImagePath,
                                             QLatin1StringView("directory-file deletion"),
                                             &layoutBlockers)) {
        context.blockers->append(
            QStringLiteral("APFS directory-file deletion requires S.A.K.-generated layout proof"));
        context.blockers->append(layoutBlockers);
        return std::nullopt;
    }
    planOptions.allow_compressed_file_mutation = true;

    *context.plan = PartitionApfsWriter::planImageOnlyMutation(
        *context.detection,
        PartitionApfsWriteOperation::DeleteFile,
        planOptions,
        rootDirectoryFilePath(context.cleanDirectoryName, context.cleanFileName));
    if (!appendPlanResult(*context.plan, context.blockers, context.warnings)) {
        return std::nullopt;
    }
    if (deletedEntry) {
        *deletedEntry = *target;
    }
    if (deletedData) {
        *deletedData = *targetData;
    }
    return rootListing;
}

std::optional<PartitionApfsFileReadResult> prepareRootDirectoryFilePatchPlan(
    const ApfsRootFilePlanContext& context,
    PartitionApfsFileEntry* patchedEntry,
    QByteArray* originalData) {
    auto rootListing = rootListingForWrite(context.sourceImagePath, context.blockers);
    const auto directory = boundedRootDirectoryEntry(rootListing,
                                                     context.cleanDirectoryName,
                                                     QLatin1StringView("directory-file-patch"),
                                                     context.blockers);
    if (!directory.has_value()) {
        return std::nullopt;
    }
    const auto directoryListing =
        directoryListingForWrite(context.sourceImagePath,
                                 *directory,
                                 QLatin1StringView("directory-file-patch"),
                                 context.blockers);
    const auto target = boundedDirectoryFileEntry(directoryListing,
                                                  context.cleanFileName,
                                                  QLatin1StringView("directory-file-patch"),
                                                  context.blockers);
    if (!target.has_value()) {
        return std::nullopt;
    }
    const auto targetData = readBoundedRootFile(context.sourceImagePath,
                                                *target,
                                                QLatin1StringView("directory-file-patch"),
                                                context.blockers);
    if (!targetData.has_value()) {
        return std::nullopt;
    }

    QStringList layoutBlockers;
    PartitionApfsWriteOptions planOptions = *context.options;
    if (!appendGeneratedSourceLayoutBlockers(
            context.sourceImagePath, QLatin1StringView("directory-file patch"), &layoutBlockers)) {
        context.blockers->append(
            QStringLiteral("APFS directory-file patch requires S.A.K.-generated layout proof"));
        context.blockers->append(layoutBlockers);
        return std::nullopt;
    }
    planOptions.allow_compressed_file_mutation = true;

    *context.plan = PartitionApfsWriter::planImageOnlyMutation(
        *context.detection,
        PartitionApfsWriteOperation::ReplaceFile,
        planOptions,
        rootDirectoryFilePath(context.cleanDirectoryName, context.cleanFileName));
    if (!appendPlanResult(*context.plan, context.blockers, context.warnings)) {
        return std::nullopt;
    }
    if (patchedEntry) {
        *patchedEntry = *target;
    }
    if (originalData) {
        *originalData = *targetData;
    }
    return rootListing;
}

std::optional<PartitionApfsFileReadResult> prepareRootDirectoryCreatePlan(
    const ApfsRootFilePlanContext& context) {
    auto rootListing = rootListingForWrite(context.sourceImagePath, context.blockers);
    if (rootEntryExists(rootListing, context.cleanFileName, context.blockers)) {
        return std::nullopt;
    }
    if (!context.blockers->isEmpty()) {
        return std::nullopt;
    }

    QStringList layoutBlockers;
    if (!appendGeneratedSourceLayoutBlockers(
            context.sourceImagePath, QLatin1StringView("directory creation"), &layoutBlockers)) {
        context.blockers->append(
            QStringLiteral("APFS directory creation requires S.A.K.-generated layout proof"));
        context.blockers->append(layoutBlockers);
        return std::nullopt;
    }

    *context.plan = PartitionApfsWriter::planImageOnlyMutation(
        *context.detection,
        PartitionApfsWriteOperation::CreateDirectory,
        *context.options,
        QStringLiteral("/%1").arg(context.cleanFileName));
    if (!appendPlanResult(*context.plan, context.blockers, context.warnings)) {
        return std::nullopt;
    }
    return rootListing;
}

std::optional<PartitionApfsFileReadResult> prepareRootDirectoryDeletePlan(
    const ApfsRootFilePlanContext& context) {
    auto rootListing = rootListingForWrite(context.sourceImagePath, context.blockers);
    const auto target = boundedRootDirectoryEntry(rootListing,
                                                  context.cleanFileName,
                                                  QLatin1StringView("directory-delete"),
                                                  context.blockers);
    if (!target.has_value() || !appendEmptyRootDirectoryDeleteBlockers(
                                   context.sourceImagePath, *target, context.blockers)) {
        return std::nullopt;
    }

    QStringList layoutBlockers;
    if (!appendGeneratedSourceLayoutBlockers(
            context.sourceImagePath, QLatin1StringView("directory deletion"), &layoutBlockers)) {
        context.blockers->append(
            QStringLiteral("APFS directory deletion requires S.A.K.-generated layout proof"));
        context.blockers->append(layoutBlockers);
        return std::nullopt;
    }

    *context.plan = PartitionApfsWriter::planImageOnlyMutation(
        *context.detection,
        PartitionApfsWriteOperation::DeleteDirectory,
        *context.options,
        QStringLiteral("/%1").arg(context.cleanFileName));
    if (!appendPlanResult(*context.plan, context.blockers, context.warnings)) {
        return std::nullopt;
    }
    return rootListing;
}

std::optional<PartitionApfsFileReadResult> prepareRootFilePatchPlan(
    const ApfsRootFilePlanContext& context,
    PartitionApfsFileEntry* patchedEntry,
    QByteArray* originalData) {
    auto rootListing = rootListingForWrite(context.sourceImagePath, context.blockers);
    const auto target = boundedRootFileEntry(
        rootListing, context.cleanFileName, QLatin1StringView("file-patch"), context.blockers);
    if (!target.has_value()) {
        return std::nullopt;
    }
    const auto targetData = readBoundedRootFile(
        context.sourceImagePath, *target, QLatin1StringView("file-patch"), context.blockers);
    if (!targetData.has_value()) {
        return std::nullopt;
    }

    PartitionApfsWriteOptions planOptions = *context.options;
    QStringList layoutBlockers;
    if (!appendGeneratedSourceLayoutBlockers(
            context.sourceImagePath, QLatin1StringView("file patch"), &layoutBlockers)) {
        context.blockers->append(
            QStringLiteral("APFS file patch requires S.A.K.-generated layout proof"));
        context.blockers->append(layoutBlockers);
        return std::nullopt;
    }
    planOptions.allow_compressed_file_mutation = true;

    *context.plan = PartitionApfsWriter::planImageOnlyMutation(
        *context.detection,
        PartitionApfsWriteOperation::ReplaceFile,
        planOptions,
        QStringLiteral("/%1").arg(context.cleanFileName));
    if (!appendPlanResult(*context.plan, context.blockers, context.warnings)) {
        return std::nullopt;
    }

    if (patchedEntry) {
        *patchedEntry = *target;
    }
    if (originalData) {
        *originalData = *targetData;
    }
    return rootListing;
}

void finalizeRepairResult(PartitionApfsImageRepairResult* result) {
    if (result->blockers.isEmpty()) {
        result->repaired_image_sha256 = fileSha256Hex(result->repaired_image_path,
                                                      &result->blockers);
    }
    result->ok = result->blockers.isEmpty();
    if (!result->ok) {
        QFile::remove(result->repaired_image_path);
    }
}

void finalizeFileWriteResult(PartitionApfsImageFileWriteResult* result) {
    if (result->blockers.isEmpty()) {
        result->written_image_sha256 = fileSha256Hex(result->written_image_path, &result->blockers);
    }
    result->ok = result->blockers.isEmpty();
    if (!result->ok) {
        QFile::remove(result->written_image_path);
    }
}

void finalizeFileDeleteResult(PartitionApfsImageFileDeleteResult* result) {
    if (result->blockers.isEmpty()) {
        result->written_image_sha256 = fileSha256Hex(result->written_image_path, &result->blockers);
    }
    result->ok = result->blockers.isEmpty();
    if (!result->ok) {
        QFile::remove(result->written_image_path);
    }
}

void finalizeFilePatchResult(PartitionApfsImageFilePatchResult* result) {
    if (result->blockers.isEmpty()) {
        result->written_image_sha256 = fileSha256Hex(result->written_image_path, &result->blockers);
    }
    result->ok = result->blockers.isEmpty();
    if (!result->ok) {
        QFile::remove(result->written_image_path);
    }
}

void finalizeDirectoryMutationResult(PartitionApfsImageDirectoryMutationResult* result) {
    if (result->blockers.isEmpty()) {
        result->written_image_sha256 = fileSha256Hex(result->written_image_path, &result->blockers);
    }
    result->ok = result->blockers.isEmpty();
    if (!result->ok) {
        QFile::remove(result->written_image_path);
    }
}

void finalizeVolumeLabelResult(PartitionApfsImageVolumeLabelResult* result) {
    if (result->blockers.isEmpty()) {
        result->written_image_sha256 = fileSha256Hex(result->written_image_path, &result->blockers);
    }
    result->ok = result->blockers.isEmpty();
    if (!result->ok) {
        QFile::remove(result->written_image_path);
    }
}

std::optional<PartitionApfsFileEntry> rootDirectoryReadbackEntry(const QString& imagePath,
                                                                 const QString& cleanDirectoryName,
                                                                 QLatin1StringView purpose,
                                                                 QStringList* blockers) {
    const auto rootListing = PartitionApfsFileSystemReader::listDirectoryFromImage(
        imagePath, QStringLiteral("/"), kApfsWriteRootListingMaxEntries);
    if (!rootListing.ok) {
        blockers->append(QStringLiteral("APFS %1 root listing read-back failed: %2")
                             .arg(purpose, rootListing.blockers.join(QStringLiteral("; "))));
        return std::nullopt;
    }
    for (const auto& entry : rootListing.entries) {
        if (entry.name.compare(cleanDirectoryName, Qt::CaseInsensitive) == 0) {
            return entry;
        }
    }
    return std::nullopt;
}

bool appendVolumeLabelReadback(const QString& targetPath,
                               const QString& expectedVolumeName,
                               QLatin1StringView purpose,
                               QStringList* blockers) {
    const auto rootListing =
        PartitionApfsFileSystemReader::listDirectoryFromImage(targetPath, QStringLiteral("/"), 1);
    if (!rootListing.ok) {
        blockers->append(QStringLiteral("APFS %1 volume-label read-back failed: %2")
                             .arg(purpose, rootListing.blockers.join(QStringLiteral("; "))));
        return false;
    }
    if (rootListing.volume_name != expectedVolumeName) {
        blockers->append(
            QStringLiteral("APFS %1 volume-label read-back mismatch: expected '%2', got '%3'")
                .arg(purpose, expectedVolumeName, rootListing.volume_name));
        return false;
    }
    return true;
}

void appendDirectoryCreateReadback(const QString& imagePath,
                                   const QString& cleanDirectoryName,
                                   PartitionApfsImageDirectoryMutationResult* result) {
    if (!result || !result->blockers.isEmpty()) {
        return;
    }
    const auto entry = rootDirectoryReadbackEntry(
        imagePath, cleanDirectoryName, QLatin1StringView("directory-create"), &result->blockers);
    if (!entry.has_value() || !entry->directory) {
        result->blockers.append(
            QStringLiteral("APFS directory-create read-back did not find the target directory"));
        return;
    }
    const auto childListing = PartitionApfsFileSystemReader::listDirectoryFromImage(
        imagePath, QStringLiteral("/%1").arg(cleanDirectoryName), 1);
    if (!childListing.ok) {
        result->blockers.append(QStringLiteral("APFS directory-create child listing failed: %1")
                                    .arg(childListing.blockers.join(QStringLiteral("; "))));
        return;
    }
    if (!childListing.entries.isEmpty()) {
        result->blockers.append(
            QStringLiteral("APFS directory-create read-back target was not empty"));
    }
}

void appendDirectoryDeleteReadback(const QString& imagePath,
                                   const QString& cleanDirectoryName,
                                   PartitionApfsImageDirectoryMutationResult* result) {
    if (!result || !result->blockers.isEmpty()) {
        return;
    }
    const auto entry = rootDirectoryReadbackEntry(
        imagePath, cleanDirectoryName, QLatin1StringView("directory-delete"), &result->blockers);
    if (entry.has_value()) {
        result->blockers.append(
            QStringLiteral("APFS directory-delete read-back still found target directory"));
    }
}

void appendFileWriteReadback(const QString& imagePath,
                             const QString& cleanFileName,
                             const QByteArray& expected,
                             PartitionApfsImageFileWriteResult* result) {
    if (!result || !result->blockers.isEmpty()) {
        return;
    }
    const auto readBack =
        PartitionApfsFileSystemReader::readFileFromImage(imagePath,
                                                         QStringLiteral("/%1").arg(cleanFileName),
                                                         static_cast<uint64_t>(expected.size()));
    if (!readBack.ok) {
        result->blockers.append(QStringLiteral("APFS file-write read-back failed: %1")
                                    .arg(readBack.blockers.join(QStringLiteral("; "))));
        return;
    }
    if (readBack.data != expected) {
        result->blockers.append(
            QStringLiteral("APFS file-write read-back payload mismatch: %1 (%2 vs %3 bytes)")
                .arg(cleanFileName)
                .arg(readBack.data.size())
                .arg(expected.size()));
        return;
    }
    result->readback_sha256 = bytesSha256Hex(readBack.data);
}

void appendDirectoryFileWriteReadback(const QString& imagePath,
                                      const QString& cleanDirectoryName,
                                      const QString& cleanFileName,
                                      const QByteArray& expected,
                                      PartitionApfsImageFileWriteResult* result) {
    if (!result || !result->blockers.isEmpty()) {
        return;
    }
    const auto readBack = PartitionApfsFileSystemReader::readFileFromImage(
        imagePath,
        rootDirectoryFilePath(cleanDirectoryName, cleanFileName),
        static_cast<uint64_t>(expected.size()));
    if (!readBack.ok) {
        result->blockers.append(QStringLiteral("APFS directory-file-write read-back failed: %1")
                                    .arg(readBack.blockers.join(QStringLiteral("; "))));
        return;
    }
    if (readBack.data != expected) {
        result->blockers.append(
            QStringLiteral("APFS directory-file-write read-back payload mismatch"));
        return;
    }
    result->readback_sha256 = bytesSha256Hex(readBack.data);
}

void appendDirectoryFilePatchReadback(const QString& imagePath,
                                      const QString& cleanDirectoryName,
                                      const QString& cleanFileName,
                                      const QByteArray& expected,
                                      PartitionApfsImageFilePatchResult* result) {
    if (!result || !result->blockers.isEmpty()) {
        return;
    }
    const auto readBack = PartitionApfsFileSystemReader::readFileFromImage(
        imagePath,
        rootDirectoryFilePath(cleanDirectoryName, cleanFileName),
        static_cast<uint64_t>(expected.size()));
    if (!readBack.ok) {
        result->blockers.append(QStringLiteral("APFS directory-file-patch read-back failed: %1")
                                    .arg(readBack.blockers.join(QStringLiteral("; "))));
        return;
    }
    if (readBack.data != expected) {
        result->blockers.append(
            QStringLiteral("APFS directory-file-patch read-back payload mismatch"));
        return;
    }
    result->readback_sha256 = bytesSha256Hex(readBack.data);
}

void appendFilePatchReadback(const QString& imagePath,
                             const QString& cleanFileName,
                             const QByteArray& expected,
                             PartitionApfsImageFilePatchResult* result) {
    if (!result || !result->blockers.isEmpty()) {
        return;
    }
    const auto readBack =
        PartitionApfsFileSystemReader::readFileFromImage(imagePath,
                                                         QStringLiteral("/%1").arg(cleanFileName),
                                                         static_cast<uint64_t>(expected.size()));
    if (!readBack.ok) {
        result->blockers.append(QStringLiteral("APFS file-patch read-back failed: %1")
                                    .arg(readBack.blockers.join(QStringLiteral("; "))));
        return;
    }
    if (readBack.data != expected) {
        result->blockers.append(QStringLiteral("APFS file-patch read-back payload mismatch"));
        return;
    }
    result->readback_sha256 = bytesSha256Hex(readBack.data);
}

void appendFileDeleteReadback(const QString& imagePath,
                              const QString& cleanFileName,
                              PartitionApfsImageFileDeleteResult* result) {
    if (!result || !result->blockers.isEmpty()) {
        return;
    }
    const auto readBack = PartitionApfsFileSystemReader::readFileFromImage(
        imagePath, QStringLiteral("/%1").arg(cleanFileName), 1);
    if (readBack.ok) {
        result->blockers.append(
            QStringLiteral("APFS file-delete read-back still found target file"));
    }
}

void appendDirectoryFileDeleteReadback(const QString& imagePath,
                                       const QString& cleanDirectoryName,
                                       const QString& cleanFileName,
                                       PartitionApfsImageFileDeleteResult* result) {
    if (!result || !result->blockers.isEmpty()) {
        return;
    }
    const auto readBack = PartitionApfsFileSystemReader::readFileFromImage(
        imagePath, rootDirectoryFilePath(cleanDirectoryName, cleanFileName), 1);
    if (readBack.ok) {
        result->blockers.append(
            QStringLiteral("APFS directory-file-delete read-back still found target file"));
    }
}

bool applyRootFilePatchPayload(const PartitionApfsImageRootFilePatchRequest& request,
                               const PartitionApfsFileEntry& patchedEntry,
                               QByteArray* patchedData,
                               PartitionApfsImageFilePatchResult* result) {
    result->file_bytes = patchedEntry.size_bytes;
    if (request.patch_offset_bytes > result->file_bytes ||
        result->patch_bytes > result->file_bytes - request.patch_offset_bytes) {
        result->blockers.append(QStringLiteral(
            "APFS file-patch range must stay inside the existing file; use replace for resize"));
        return false;
    }
    std::copy(request.patch_data.cbegin(),
              request.patch_data.cend(),
              patchedData->begin() + static_cast<qsizetype>(request.patch_offset_bytes));
    return true;
}

bool applyRawRootFilePatchPayload(const PartitionApfsRawRootFilePatchRequest& request,
                                  const PartitionApfsFileEntry& patchedEntry,
                                  QByteArray* patchedData,
                                  PartitionApfsRawFilePatchResult* result) {
    result->file_bytes = patchedEntry.size_bytes;
    if (request.patch_offset_bytes > result->file_bytes ||
        result->patch_bytes > result->file_bytes - request.patch_offset_bytes) {
        result->blockers.append(
            QStringLiteral("APFS raw file-patch range must stay inside the existing file; use "
                           "replace for resize"));
        return false;
    }
    std::copy(request.patch_data.cbegin(),
              request.patch_data.cend(),
              patchedData->begin() + static_cast<qsizetype>(request.patch_offset_bytes));
    return true;
}

bool applyDirectoryFilePatchPayload(const PartitionApfsImageRootDirectoryFilePatchRequest& request,
                                    const PartitionApfsFileEntry& patchedEntry,
                                    QByteArray* patchedData,
                                    PartitionApfsImageFilePatchResult* result) {
    result->file_bytes = patchedEntry.size_bytes;
    if (request.patch_offset_bytes > result->file_bytes ||
        result->patch_bytes > result->file_bytes - request.patch_offset_bytes) {
        result->blockers.append(
            QStringLiteral("APFS directory-file-patch range must stay inside the existing file; "
                           "use replace for resize"));
        return false;
    }
    std::copy(request.patch_data.cbegin(),
              request.patch_data.cend(),
              patchedData->begin() + static_cast<qsizetype>(request.patch_offset_bytes));
    return true;
}

bool applyRawDirectoryFilePatchPayload(const PartitionApfsRawRootDirectoryFilePatchRequest& request,
                                       const PartitionApfsFileEntry& patchedEntry,
                                       QByteArray* patchedData,
                                       PartitionApfsRawFilePatchResult* result) {
    result->file_bytes = patchedEntry.size_bytes;
    if (request.patch_offset_bytes > result->file_bytes ||
        result->patch_bytes > result->file_bytes - request.patch_offset_bytes) {
        result->blockers.append(
            QStringLiteral("APFS raw directory-file-patch range must stay inside the existing "
                           "file; use replace for resize"));
        return false;
    }
    std::copy(request.patch_data.cbegin(),
              request.patch_data.cend(),
              patchedData->begin() + static_cast<qsizetype>(request.patch_offset_bytes));
    return true;
}

struct ApfsRootFilePatchRewrite {
    QString sourceImagePath;
    QString writtenImagePath;
    QString fileName;
    QFileInfo sourceInfo;
    QByteArray fileData;
    PartitionApfsFileReadResult rootListing;
    uint64_t* writtenDataBlocks{nullptr};
};

bool runRootFilePatchRewriteOnImage(const ApfsRootFilePatchRewrite& rewrite,
                                    QStringList* blockers) {
    if (!copyToScratchImage(rewrite.sourceImagePath,
                            rewrite.writtenImagePath,
                            QLatin1StringView("file-patch"),
                            blockers)) {
        return false;
    }

    QFile image;
    if (!openScratchImage(
            rewrite.writtenImagePath, QLatin1StringView("file-patch"), &image, blockers)) {
        return false;
    }

    QStringList patchBlockers;
    runRootFileWriteOnScratch({.image = &image,
                               .sourceImagePath = rewrite.sourceImagePath,
                               .fileName = rewrite.fileName,
                               .fileData = rewrite.fileData,
                               .rootListing = rewrite.rootListing,
                               .writtenDataBlocks = rewrite.writtenDataBlocks},
                              &patchBlockers);
    image.close();
    blockers->append(patchBlockers);
    return blockers->isEmpty();
}

void appendRawFileWriteReadback(const QString& targetPath,
                                const QString& cleanFileName,
                                const QByteArray& expected,
                                PartitionApfsRawFileWriteResult* result) {
    if (!result || !result->blockers.isEmpty()) {
        return;
    }
    const auto readBack =
        PartitionApfsFileSystemReader::readFileFromImage(targetPath,
                                                         QStringLiteral("/%1").arg(cleanFileName),
                                                         static_cast<uint64_t>(expected.size()));
    if (!readBack.ok) {
        result->blockers.append(QStringLiteral("APFS raw file-write read-back failed: %1")
                                    .arg(readBack.blockers.join(QStringLiteral("; "))));
        return;
    }
    if (readBack.data != expected) {
        result->blockers.append(QStringLiteral("APFS raw file-write read-back payload mismatch"));
        return;
    }
    result->readback_sha256 = bytesSha256Hex(readBack.data);
}

void appendRawFilePatchReadback(const QString& targetPath,
                                const QString& cleanFileName,
                                const QByteArray& expected,
                                PartitionApfsRawFilePatchResult* result) {
    if (!result || !result->blockers.isEmpty()) {
        return;
    }
    const auto readBack =
        PartitionApfsFileSystemReader::readFileFromImage(targetPath,
                                                         QStringLiteral("/%1").arg(cleanFileName),
                                                         static_cast<uint64_t>(expected.size()));
    if (!readBack.ok) {
        result->blockers.append(QStringLiteral("APFS raw file-patch read-back failed: %1")
                                    .arg(readBack.blockers.join(QStringLiteral("; "))));
        return;
    }
    if (readBack.data != expected) {
        result->blockers.append(QStringLiteral("APFS raw file-patch read-back payload mismatch"));
        return;
    }
    result->readback_sha256 = bytesSha256Hex(readBack.data);
}

void appendRawFileDeleteReadback(const QString& targetPath,
                                 const QString& cleanFileName,
                                 PartitionApfsRawFileDeleteResult* result) {
    if (!result || !result->blockers.isEmpty()) {
        return;
    }
    const auto readBack = PartitionApfsFileSystemReader::readFileFromImage(
        targetPath, QStringLiteral("/%1").arg(cleanFileName), 1);
    if (readBack.ok) {
        result->blockers.append(
            QStringLiteral("APFS raw file-delete read-back still found target file"));
    }
}

void appendRawDirectoryFileWriteReadback(const QString& targetPath,
                                         const QString& cleanDirectoryName,
                                         const QString& cleanFileName,
                                         const QByteArray& expected,
                                         PartitionApfsRawFileWriteResult* result) {
    if (!result || !result->blockers.isEmpty()) {
        return;
    }
    const auto readBack = PartitionApfsFileSystemReader::readFileFromImage(
        targetPath,
        rootDirectoryFilePath(cleanDirectoryName, cleanFileName),
        static_cast<uint64_t>(expected.size()));
    if (!readBack.ok) {
        result->blockers.append(QStringLiteral("APFS raw directory-file-write read-back failed: %1")
                                    .arg(readBack.blockers.join(QStringLiteral("; "))));
        return;
    }
    if (readBack.data != expected) {
        result->blockers.append(
            QStringLiteral("APFS raw directory-file-write read-back payload mismatch"));
        return;
    }
    result->readback_sha256 = bytesSha256Hex(readBack.data);
}

void appendRawDirectoryFilePatchReadback(const QString& targetPath,
                                         const QString& cleanDirectoryName,
                                         const QString& cleanFileName,
                                         const QByteArray& expected,
                                         PartitionApfsRawFilePatchResult* result) {
    if (!result || !result->blockers.isEmpty()) {
        return;
    }
    const auto readBack = PartitionApfsFileSystemReader::readFileFromImage(
        targetPath,
        rootDirectoryFilePath(cleanDirectoryName, cleanFileName),
        static_cast<uint64_t>(expected.size()));
    if (!readBack.ok) {
        result->blockers.append(QStringLiteral("APFS raw directory-file-patch read-back failed: %1")
                                    .arg(readBack.blockers.join(QStringLiteral("; "))));
        return;
    }
    if (readBack.data != expected) {
        result->blockers.append(
            QStringLiteral("APFS raw directory-file-patch read-back payload mismatch"));
        return;
    }
    result->readback_sha256 = bytesSha256Hex(readBack.data);
}

void appendRawDirectoryFileDeleteReadback(const QString& targetPath,
                                          const QString& cleanDirectoryName,
                                          const QString& cleanFileName,
                                          PartitionApfsRawFileDeleteResult* result) {
    if (!result || !result->blockers.isEmpty()) {
        return;
    }
    const auto readBack = PartitionApfsFileSystemReader::readFileFromImage(
        targetPath, rootDirectoryFilePath(cleanDirectoryName, cleanFileName), 1);
    if (readBack.ok) {
        result->blockers.append(
            QStringLiteral("APFS raw directory-file-delete read-back still found target file"));
    }
}

void finalizeExistingFormatResult(PartitionApfsImageBuildResult* result, bool hashWholeTarget) {
    result->ok = result->blockers.isEmpty();
    if (result->ok && hashWholeTarget) {
        result->image_sha256 = fileSha256Hex(result->image_path, &result->blockers);
        result->ok = result->blockers.isEmpty();
    } else if (result->ok) {
        result->warnings.append(
            QStringLiteral("Raw APFS format target full-device SHA-256 skipped; certification uses "
                           "bounded APFS detection and root listing readback"));
    }
}

struct RawTargetMutationBlockersContext {
    QString targetPath;
    uint64_t targetBytes{0};
    bool confirmed{false};
    bool allowRawTarget{false};
    const PartitionApfsWriteOptions* options{nullptr};
    QLatin1StringView purpose;
};

void appendRawTargetMutationBlockers(const RawTargetMutationBlockersContext& context,
                                     QStringList* blockers) {
    if (context.targetPath.trimmed().isEmpty()) {
        blockers->append(
            QStringLiteral("APFS raw %1 target path is required").arg(context.purpose));
    }
    if (!isWindowsRawDevicePath(context.targetPath)) {
        blockers->append(
            QStringLiteral("APFS raw %1 requires a Windows raw-device path").arg(context.purpose));
    }
    if (!context.confirmed) {
        blockers->append(QStringLiteral("APFS raw %1 requires explicit target confirmation")
                             .arg(context.purpose));
    }
    if (!context.allowRawTarget) {
        blockers->append(
            QStringLiteral("APFS raw %1 requires explicit raw-target opt-in").arg(context.purpose));
    }
    if (!context.options || context.options->image_only) {
        blockers->append(QStringLiteral("APFS raw %1 requires non-image-only writer options")
                             .arg(context.purpose));
    }
    if (context.targetBytes < kMinimumApfsContainerBytes ||
        (context.targetBytes % kSupportedApfsBlockSizeBytes) != 0) {
        blockers->append(
            QStringLiteral("APFS raw %1 target size must be at least 64 MiB and 4096-byte aligned")
                .arg(context.purpose));
    }
    if (context.targetBytes > static_cast<uint64_t>(std::numeric_limits<qint64>::max())) {
        blockers->append(QStringLiteral("APFS raw %1 target is too large for this platform")
                             .arg(context.purpose));
    }
}

std::optional<PartitionFileSystemDetection> detectApfsRawTarget(const QString& targetPath,
                                                                uint64_t targetBytes,
                                                                QLatin1StringView purpose,
                                                                QStringList* blockers) {
    QString detectError;
    const auto detection =
        PartitionFileSystemDetector::detectFromDevicePath(targetPath, 0, targetBytes, &detectError);
    if (detection.has_value() &&
        detection->file_system.compare(QStringLiteral("APFS"), Qt::CaseInsensitive) == 0) {
        return detection;
    }
    blockers->append(
        detectError.isEmpty()
            ? QStringLiteral("APFS raw %1 target was not detected as APFS").arg(purpose)
            : detectError);
    return std::nullopt;
}

PartitionApfsImageBuildResult PartitionApfsWriter::buildImageOnlyFormatImage(
    const PartitionApfsImageFormatRequest& request) {
    PartitionApfsImageBuildResult result = formatBuildResult(request);
    if (!result.plan.buildable) {
        return result;
    }
    if (!appendFormatCreateBlockers(request, &result)) {
        return result;
    }

    QFile image;
    if (!createSizedApfsImage(
            result.image_path, request.target_container_bytes, &image, &result.blockers)) {
        return result;
    }

    QStringList writeBlockers;
    const auto blocks =
        emptyFormatBlocks(request,
                          result.plan.target_container_bytes / result.plan.block_size_bytes,
                          result.plan.volume_name,
                          &writeBlockers);
    writeImageBlocks(&image, request.block_size_bytes, blocks, &writeBlockers);
    image.close();
    result.blockers.append(writeBlockers);
    finalizeBuildResult(&result);
    return result;
}

PartitionApfsImageBuildResult PartitionApfsWriter::formatExistingImageOnlyContainer(
    const PartitionApfsImageFormatRequest& request) {
    auto imageOnlyRequest = request;
    imageOnlyRequest.allow_raw_device_target = false;
    return formatExistingContainerTarget(imageOnlyRequest);
}

PartitionApfsImageBuildResult PartitionApfsWriter::formatExistingContainerTarget(
    const PartitionApfsImageFormatRequest& request) {
    PartitionApfsImageBuildResult result = formatBuildResult(request);
    if (!result.plan.buildable) {
        return result;
    }
    if (!appendExistingFormatTargetBlockers(request, &result)) {
        return result;
    }

    QString openError;
    auto target = openFileOrRawDeviceReadWrite(result.image_path, &openError);
    if (!target) {
        result.blockers.append(
            QStringLiteral("Unable to open APFS format target: %1").arg(openError));
        return result;
    }
    if (!isWindowsRawDevicePath(result.image_path)) {
        const qint64 openedSize = target->size();
        if (openedSize < 0 || static_cast<uint64_t>(openedSize) != request.target_container_bytes) {
            result.blockers.append(QStringLiteral(
                "APFS format target opened size must match requested container size"));
            return result;
        }
    }

    QStringList writeBlockers;
    if (zeroFormatStaleSignatureRanges(
            target.get(), request.target_container_bytes, &writeBlockers)) {
        const auto blocks =
            emptyFormatBlocks(request,
                              result.plan.target_container_bytes / result.plan.block_size_bytes,
                              result.plan.volume_name,
                              &writeBlockers);
        writeImageBlocks(target.get(), request.block_size_bytes, blocks, &writeBlockers);
    }
    if (auto* file = dynamic_cast<QFile*>(target.get())) {
        file->flush();
    }
    target->close();
    result.blockers.append(writeBlockers);
    finalizeExistingFormatResult(&result, !isWindowsRawDevicePath(result.image_path));
    return result;
}

PartitionApfsImageBuildResult PartitionApfsWriter::buildImageOnlyFormatImageWithSeedFile(
    const PartitionApfsImageFormatRequest& request) {
    PartitionApfsImageBuildResult result = buildImageOnlyFormatImage(request);
    if (!result.ok) {
        return result;
    }

    const QString cleanFileName = request.seed_file_name.trimmed();
    if (!appendSeedFileBlockers(request, cleanFileName, &result)) {
        QFile::remove(result.image_path);
        return result;
    }

    QStringList seedBlockers;
    const auto files =
        assignedRootFilePayloads({{.fileName = cleanFileName, .data = request.seed_file_data}},
                                 request.block_size_bytes,
                                 {},
                                 &seedBlockers);
    if (!seedBlockers.isEmpty()) {
        result.blockers.append(seedBlockers);
        QFile::remove(result.image_path);
        return result;
    }
    QFile image(result.image_path);
    if (!image.open(QIODevice::ReadWrite)) {
        result.blockers.append(QStringLiteral("Unable to reopen APFS image for seed file: %1")
                                   .arg(image.errorString()));
        finalizeBuildResult(&result);
        return result;
    }

    QStringList writeBlockers;
    const uint64_t blockCount = request.target_container_bytes / request.block_size_bytes;
    if (allocatedBlocksForFiles(files, request.block_size_bytes) > blockCount) {
        result.blockers.append(QStringLiteral("APFS seed-file data exceeds target container size"));
        image.close();
        QFile::remove(result.image_path);
        return result;
    }

    const auto blocks = rewriteGeneratedBlocks(&image,
                                               {
                                                   .blockSize = request.block_size_bytes,
                                                   .blockCount = blockCount,
                                                   .volumeName = result.plan.volume_name,
                                                   .files = files,
                                               },
                                               &writeBlockers);
    writeImageBlocks(&image, request.block_size_bytes, blocks, &writeBlockers);
    image.close();

    result.blockers.append(writeBlockers);
    finalizeBuildResult(&result);
    return result;
}

PartitionApfsImageRepairResult PartitionApfsWriter::repairImageOnlyObjectChecksums(
    const PartitionApfsImageRepairRequest& request) {
    PartitionApfsImageRepairResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.repaired_image_path = request.repaired_image_path.trimmed();
    result.warnings.append(QStringLiteral(
        "Generated APFS image repair remains image-only; raw generated-layout repair is exposed "
        "only through sak_apfs_writer_cli.exe with explicit selected-target confirmation"));

    const auto source = validateImageOnlySource(result.source_image_path,
                                                QLatin1StringView("repair"),
                                                &result.blockers);
    if (!source.ok) {
        return result;
    }
    if (!appendSeparateOutputBlockers(source.info,
                                      result.repaired_image_path,
                                      QLatin1StringView("repair"),
                                      &result.blockers)) {
        return result;
    }
    const auto detection = detectApfsImageSource(result.source_image_path,
                                                 static_cast<uint64_t>(source.info.size()),
                                                 QLatin1StringView("repair"),
                                                 &result.blockers);
    if (!detection.has_value()) {
        return result;
    }

    result.plan = planImageOnlyMutation(
        *detection, PartitionApfsWriteOperation::RepairContainer, request.options, QString());
    if (!appendPlanResult(result.plan, &result.blockers, &result.warnings)) {
        return result;
    }

    if (!copyToScratchImage(result.source_image_path,
                            result.repaired_image_path,
                            QLatin1StringView("repair"),
                            &result.blockers)) {
        return result;
    }

    QFile image;
    if (!openScratchImage(
            result.repaired_image_path, QLatin1StringView("repair"), &image, &result.blockers)) {
        return result;
    }

    QStringList repairBlockers;
    runChecksumRepairOnScratch(&image, &result, &repairBlockers);
    image.close();
    result.blockers.append(repairBlockers);
    if (result.repaired_checksum_blocks == 0 && result.blockers.isEmpty()) {
        result.blockers.append(
            QStringLiteral("APFS repair did not find any incorrect supported object checksums"));
    }
    finalizeRepairResult(&result);
    return result;
}

PartitionApfsImageFileWriteResult PartitionApfsWriter::writeImageOnlyRootFile(
    const PartitionApfsImageRootFileWriteRequest& request) {
    PartitionApfsImageFileWriteResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();
    result.warnings.append(QStringLiteral(
        "Generated APFS file write remains image-only and is not exposed to user actions; raw "
        "file-write support requires fsck_apfs/diskutil, crash replay, and hardware evidence"));

    const auto source = validateImageOnlySource(result.source_image_path,
                                                QLatin1StringView("file-write"),
                                                &result.blockers);
    if (!source.ok) {
        return result;
    }
    if (!appendSeparateOutputBlockers(source.info,
                                      result.written_image_path,
                                      QLatin1StringView("file-write"),
                                      &result.blockers)) {
        return result;
    }

    const QString cleanFileName = request.file_name.trimmed();
    result.file_name = cleanFileName;
    result.file_bytes = static_cast<uint64_t>(request.file_data.size());
    result.payload_sha256 = bytesSha256Hex(request.file_data);
    if (!appendFileWritePayloadBlockers(request, cleanFileName, &result.blockers)) {
        return result;
    }
    const auto detection = detectApfsImageSource(result.source_image_path,
                                                 static_cast<uint64_t>(source.info.size()),
                                                 QLatin1StringView("file-write"),
                                                 &result.blockers);
    if (!detection.has_value()) {
        return result;
    }

    const auto rootListing = prepareRootFileWritePlan({.detection = &*detection,
                                                       .options = &request.options,
                                                       .sourceImagePath = result.source_image_path,
                                                       .cleanFileName = cleanFileName,
                                                       .plan = &result.plan,
                                                       .blockers = &result.blockers,
                                                       .warnings = &result.warnings});
    if (!rootListing.has_value()) {
        return result;
    }

    if (!copyToScratchImage(result.source_image_path,
                            result.written_image_path,
                            QLatin1StringView("file-write"),
                            &result.blockers)) {
        return result;
    }

    QFile image;
    if (!openScratchImage(
            result.written_image_path, QLatin1StringView("file-write"), &image, &result.blockers)) {
        return result;
    }

    QStringList writeBlockers;
    runRootFileWriteOnScratch({.image = &image,
                               .sourceImagePath = result.source_image_path,
                               .fileName = cleanFileName,
                               .fileData = request.file_data,
                               .rootListing = *rootListing,
                               .writtenDataBlocks = &result.written_data_blocks},
                              &writeBlockers);
    image.close();
    result.blockers.append(writeBlockers);
    appendFileWriteReadback(result.written_image_path, cleanFileName, request.file_data, &result);
    finalizeFileWriteResult(&result);
    return result;
}

PartitionApfsImageFileDeleteResult PartitionApfsWriter::deleteImageOnlyRootFile(
    const PartitionApfsImageRootFileDeleteRequest& request) {
    PartitionApfsImageFileDeleteResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();
    result.warnings.append(QStringLiteral(
        "Generated APFS file delete remains image-only and is not exposed to user actions; raw "
        "file-delete support requires fsck_apfs/diskutil, crash replay, and hardware evidence"));

    const auto source = validateImageOnlySource(result.source_image_path,
                                                QLatin1StringView("file-delete"),
                                                &result.blockers);
    if (!source.ok) {
        return result;
    }
    if (!appendSeparateOutputBlockers(source.info,
                                      result.written_image_path,
                                      QLatin1StringView("file-delete"),
                                      &result.blockers)) {
        return result;
    }

    const QString cleanFileName = request.file_name.trimmed();
    result.file_name = cleanFileName;
    if (!appendRootFileNameBlockers(
            cleanFileName, QLatin1StringView("file-delete"), &result.blockers)) {
        return result;
    }
    const auto detection = detectApfsImageSource(result.source_image_path,
                                                 static_cast<uint64_t>(source.info.size()),
                                                 QLatin1StringView("file-delete"),
                                                 &result.blockers);
    if (!detection.has_value()) {
        return result;
    }

    PartitionApfsFileEntry deletedEntry;
    QByteArray deletedData;
    const auto rootListing = prepareRootFileDeletePlan({.detection = &*detection,
                                                        .options = &request.options,
                                                        .sourceImagePath = result.source_image_path,
                                                        .cleanFileName = cleanFileName,
                                                        .plan = &result.plan,
                                                        .blockers = &result.blockers,
                                                        .warnings = &result.warnings},
                                                       &deletedEntry,
                                                       &deletedData);
    if (!rootListing.has_value()) {
        return result;
    }
    result.deleted_file_bytes = deletedEntry.size_bytes;
    result.deleted_file_sha256 = bytesSha256Hex(deletedData);

    if (!copyToScratchImage(result.source_image_path,
                            result.written_image_path,
                            QLatin1StringView("file-delete"),
                            &result.blockers)) {
        return result;
    }

    QFile image;
    if (!openScratchImage(result.written_image_path,
                          QLatin1StringView("file-delete"),
                          &image,
                          &result.blockers)) {
        return result;
    }

    QStringList deleteBlockers;
    runRootFileDeleteOnScratch({.image = &image,
                                .sourceImagePath = result.source_image_path,
                                .fileName = cleanFileName,
                                .rootListing = *rootListing,
                                .targetSizeBytes = result.deleted_file_bytes,
                                .freedDataBlocks = &result.freed_data_blocks},
                               &deleteBlockers);
    image.close();
    result.blockers.append(deleteBlockers);
    appendFileDeleteReadback(result.written_image_path, cleanFileName, &result);
    finalizeFileDeleteResult(&result);
    return result;
}

PartitionApfsImageFileWriteResult PartitionApfsWriter::writeImageOnlyRootDirectoryFile(
    const PartitionApfsImageRootDirectoryFileWriteRequest& request) {
    PartitionApfsImageFileWriteResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();
    result.warnings.append(
        QStringLiteral("Generated APFS directory-file write is limited to S.A.K.-generated "
                       "single-volume images with bounded root-directory child files"));

    const auto source = validateImageOnlySource(result.source_image_path,
                                                QLatin1StringView("directory-file-write"),
                                                &result.blockers);
    if (!source.ok) {
        return result;
    }
    if (!appendSeparateOutputBlockers(source.info,
                                      result.written_image_path,
                                      QLatin1StringView("directory-file-write"),
                                      &result.blockers)) {
        return result;
    }

    const QString cleanDirectoryName = request.directory_name.trimmed();
    const QString cleanFileName = request.file_name.trimmed();
    result.directory_name = cleanDirectoryName;
    result.file_name = cleanFileName;
    result.file_bytes = static_cast<uint64_t>(request.file_data.size());
    result.payload_sha256 = bytesSha256Hex(request.file_data);
    if (!appendDirectoryFileWritePayloadBlockers(
            request, cleanDirectoryName, cleanFileName, &result.blockers)) {
        return result;
    }
    const auto detection = detectApfsImageSource(result.source_image_path,
                                                 static_cast<uint64_t>(source.info.size()),
                                                 QLatin1StringView("directory-file-write"),
                                                 &result.blockers);
    if (!detection.has_value()) {
        return result;
    }
    const auto rootListing =
        prepareRootDirectoryFileWritePlan({.detection = &*detection,
                                           .options = &request.options,
                                           .sourceImagePath = result.source_image_path,
                                           .cleanFileName = cleanFileName,
                                           .cleanDirectoryName = cleanDirectoryName,
                                           .plan = &result.plan,
                                           .blockers = &result.blockers,
                                           .warnings = &result.warnings});
    if (!rootListing.has_value()) {
        return result;
    }
    if (!copyToScratchImage(result.source_image_path,
                            result.written_image_path,
                            QLatin1StringView("directory-file-write"),
                            &result.blockers)) {
        return result;
    }

    QFile image;
    if (!openScratchImage(result.written_image_path,
                          QLatin1StringView("directory-file-write"),
                          &image,
                          &result.blockers)) {
        return result;
    }
    QStringList writeBlockers;
    runRootDirectoryFileRewriteOnScratch({.image = &image,
                                          .sourceImagePath = result.source_image_path,
                                          .directoryName = cleanDirectoryName,
                                          .fileName = cleanFileName,
                                          .fileData = request.file_data,
                                          .rootListing = *rootListing,
                                          .changedDataBlocks = &result.written_data_blocks},
                                         &writeBlockers);
    image.close();
    result.blockers.append(writeBlockers);
    appendDirectoryFileWriteReadback(
        result.written_image_path, cleanDirectoryName, cleanFileName, request.file_data, &result);
    finalizeFileWriteResult(&result);
    return result;
}

PartitionApfsImageFileDeleteResult PartitionApfsWriter::deleteImageOnlyRootDirectoryFile(
    const PartitionApfsImageRootDirectoryFileDeleteRequest& request) {
    PartitionApfsImageFileDeleteResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();
    result.warnings.append(
        QStringLiteral("Generated APFS directory-file delete is limited to bounded child files in "
                       "S.A.K.-generated root directories"));

    const auto source = validateImageOnlySource(result.source_image_path,
                                                QLatin1StringView("directory-file-delete"),
                                                &result.blockers);
    if (!source.ok) {
        return result;
    }
    if (!appendSeparateOutputBlockers(source.info,
                                      result.written_image_path,
                                      QLatin1StringView("directory-file-delete"),
                                      &result.blockers)) {
        return result;
    }

    const QString cleanDirectoryName = request.directory_name.trimmed();
    const QString cleanFileName = request.file_name.trimmed();
    result.directory_name = cleanDirectoryName;
    result.file_name = cleanFileName;
    if (!appendDirectoryFileNameBlockers(cleanDirectoryName,
                                         cleanFileName,
                                         QLatin1StringView("directory-file-delete"),
                                         &result.blockers)) {
        return result;
    }
    const auto detection = detectApfsImageSource(result.source_image_path,
                                                 static_cast<uint64_t>(source.info.size()),
                                                 QLatin1StringView("directory-file-delete"),
                                                 &result.blockers);
    if (!detection.has_value()) {
        return result;
    }

    PartitionApfsFileEntry deletedEntry;
    QByteArray deletedData;
    const auto rootListing =
        prepareRootDirectoryFileDeletePlan({.detection = &*detection,
                                            .options = &request.options,
                                            .sourceImagePath = result.source_image_path,
                                            .cleanFileName = cleanFileName,
                                            .cleanDirectoryName = cleanDirectoryName,
                                            .plan = &result.plan,
                                            .blockers = &result.blockers,
                                            .warnings = &result.warnings},
                                           &deletedEntry,
                                           &deletedData);
    if (!rootListing.has_value()) {
        return result;
    }
    result.deleted_file_bytes = deletedEntry.size_bytes;
    result.deleted_file_sha256 = bytesSha256Hex(deletedData);
    if (!copyToScratchImage(result.source_image_path,
                            result.written_image_path,
                            QLatin1StringView("directory-file-delete"),
                            &result.blockers)) {
        return result;
    }

    QFile image;
    if (!openScratchImage(result.written_image_path,
                          QLatin1StringView("directory-file-delete"),
                          &image,
                          &result.blockers)) {
        return result;
    }
    QStringList deleteBlockers;
    runRootDirectoryFileRewriteOnScratch({.image = &image,
                                          .sourceImagePath = result.source_image_path,
                                          .directoryName = cleanDirectoryName,
                                          .fileName = cleanFileName,
                                          .rootListing = *rootListing,
                                          .targetSizeBytes = result.deleted_file_bytes,
                                          .changedDataBlocks = &result.freed_data_blocks,
                                          .deleteFile = true},
                                         &deleteBlockers);
    image.close();
    result.blockers.append(deleteBlockers);
    appendDirectoryFileDeleteReadback(
        result.written_image_path, cleanDirectoryName, cleanFileName, &result);
    finalizeFileDeleteResult(&result);
    return result;
}

PartitionApfsImageFilePatchResult PartitionApfsWriter::patchImageOnlyRootDirectoryFile(
    const PartitionApfsImageRootDirectoryFilePatchRequest& request) {
    PartitionApfsImageFilePatchResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();
    result.patch_offset_bytes = request.patch_offset_bytes;
    result.patch_bytes = static_cast<uint64_t>(request.patch_data.size());
    result.patch_sha256 = bytesSha256Hex(request.patch_data);
    result.warnings.append(
        QStringLiteral("Generated APFS directory-file patch is limited to bounded child files in "
                       "S.A.K.-generated root directories"));

    const auto source = validateImageOnlySource(result.source_image_path,
                                                QLatin1StringView("directory-file-patch"),
                                                &result.blockers);
    if (!source.ok) {
        return result;
    }
    if (!appendSeparateOutputBlockers(source.info,
                                      result.written_image_path,
                                      QLatin1StringView("directory-file-patch"),
                                      &result.blockers)) {
        return result;
    }

    const QString cleanDirectoryName = request.directory_name.trimmed();
    const QString cleanFileName = request.file_name.trimmed();
    result.directory_name = cleanDirectoryName;
    result.file_name = cleanFileName;
    if (!appendDirectoryFilePatchPayloadBlockers(
            request, cleanDirectoryName, cleanFileName, &result.blockers)) {
        return result;
    }
    const auto detection = detectApfsImageSource(result.source_image_path,
                                                 static_cast<uint64_t>(source.info.size()),
                                                 QLatin1StringView("directory-file-patch"),
                                                 &result.blockers);
    if (!detection.has_value()) {
        return result;
    }

    PartitionApfsFileEntry patchedEntry;
    QByteArray patchedData;
    const auto rootListing =
        prepareRootDirectoryFilePatchPlan({.detection = &*detection,
                                           .options = &request.options,
                                           .sourceImagePath = result.source_image_path,
                                           .cleanFileName = cleanFileName,
                                           .cleanDirectoryName = cleanDirectoryName,
                                           .plan = &result.plan,
                                           .blockers = &result.blockers,
                                           .warnings = &result.warnings},
                                          &patchedEntry,
                                          &patchedData);
    if (!rootListing.has_value() ||
        !applyDirectoryFilePatchPayload(request, patchedEntry, &patchedData, &result)) {
        return result;
    }
    if (!copyToScratchImage(result.source_image_path,
                            result.written_image_path,
                            QLatin1StringView("directory-file-patch"),
                            &result.blockers)) {
        return result;
    }

    QFile image;
    if (!openScratchImage(result.written_image_path,
                          QLatin1StringView("directory-file-patch"),
                          &image,
                          &result.blockers)) {
        return result;
    }
    QStringList patchBlockers;
    runRootDirectoryFileRewriteOnScratch({.image = &image,
                                          .sourceImagePath = result.source_image_path,
                                          .directoryName = cleanDirectoryName,
                                          .fileName = cleanFileName,
                                          .fileData = patchedData,
                                          .rootListing = *rootListing,
                                          .changedDataBlocks = &result.written_data_blocks},
                                         &patchBlockers);
    image.close();
    result.blockers.append(patchBlockers);
    appendDirectoryFilePatchReadback(
        result.written_image_path, cleanDirectoryName, cleanFileName, patchedData, &result);
    finalizeFilePatchResult(&result);
    return result;
}

PartitionApfsImageFilePatchResult PartitionApfsWriter::patchImageOnlyRootFile(
    const PartitionApfsImageRootFilePatchRequest& request) {
    PartitionApfsImageFilePatchResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();
    result.patch_offset_bytes = request.patch_offset_bytes;
    result.patch_bytes = static_cast<uint64_t>(request.patch_data.size());
    result.patch_sha256 = bytesSha256Hex(request.patch_data);
    result.warnings.append(QStringLiteral(
        "Generated APFS partial file patch remains image-only and is not exposed to user actions; "
        "raw file-patch support requires fsck_apfs/diskutil, crash replay, and hardware evidence"));

    const auto source = validateImageOnlySource(result.source_image_path,
                                                QLatin1StringView("file-patch"),
                                                &result.blockers);
    if (!source.ok) {
        return result;
    }
    if (!appendSeparateOutputBlockers(source.info,
                                      result.written_image_path,
                                      QLatin1StringView("file-patch"),
                                      &result.blockers)) {
        return result;
    }

    const QString cleanFileName = request.file_name.trimmed();
    result.file_name = cleanFileName;
    if (!appendFilePatchPayloadBlockers(request, cleanFileName, &result.blockers)) {
        return result;
    }
    const auto detection = detectApfsImageSource(result.source_image_path,
                                                 static_cast<uint64_t>(source.info.size()),
                                                 QLatin1StringView("file-patch"),
                                                 &result.blockers);
    if (!detection.has_value()) {
        return result;
    }

    PartitionApfsFileEntry patchedEntry;
    QByteArray patchedData;
    const auto rootListing = prepareRootFilePatchPlan({.detection = &*detection,
                                                       .options = &request.options,
                                                       .sourceImagePath = result.source_image_path,
                                                       .cleanFileName = cleanFileName,
                                                       .plan = &result.plan,
                                                       .blockers = &result.blockers,
                                                       .warnings = &result.warnings},
                                                      &patchedEntry,
                                                      &patchedData);
    if (!rootListing.has_value()) {
        return result;
    }
    if (!applyRootFilePatchPayload(request, patchedEntry, &patchedData, &result)) {
        return result;
    }
    runRootFilePatchRewriteOnImage({.sourceImagePath = result.source_image_path,
                                    .writtenImagePath = result.written_image_path,
                                    .fileName = cleanFileName,
                                    .sourceInfo = source.info,
                                    .fileData = patchedData,
                                    .rootListing = *rootListing,
                                    .writtenDataBlocks = &result.written_data_blocks},
                                   &result.blockers);
    appendFilePatchReadback(result.written_image_path, cleanFileName, patchedData, &result);
    finalizeFilePatchResult(&result);
    return result;
}

PartitionApfsImageDirectoryMutationResult PartitionApfsWriter::createImageOnlyRootDirectory(
    const PartitionApfsImageRootDirectoryMutationRequest& request) {
    PartitionApfsImageDirectoryMutationResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();
    result.warnings.append(
        QStringLiteral("Generated APFS root-directory create remains limited to S.A.K.-generated "
                       "single-volume images with empty root directories"));

    const auto source = validateImageOnlySource(result.source_image_path,
                                                QLatin1StringView("directory-create"),
                                                &result.blockers);
    if (!source.ok) {
        return result;
    }
    if (!appendSeparateOutputBlockers(source.info,
                                      result.written_image_path,
                                      QLatin1StringView("directory-create"),
                                      &result.blockers)) {
        return result;
    }

    const QString cleanDirectoryName = request.directory_name.trimmed();
    result.directory_name = cleanDirectoryName;
    if (!appendRootDirectoryNameBlockers(
            cleanDirectoryName, QLatin1StringView("directory-create"), &result.blockers)) {
        return result;
    }
    const auto detection = detectApfsImageSource(result.source_image_path,
                                                 static_cast<uint64_t>(source.info.size()),
                                                 QLatin1StringView("directory-create"),
                                                 &result.blockers);
    if (!detection.has_value()) {
        return result;
    }
    const auto rootListing =
        prepareRootDirectoryCreatePlan({.detection = &*detection,
                                        .options = &request.options,
                                        .sourceImagePath = result.source_image_path,
                                        .cleanFileName = cleanDirectoryName,
                                        .plan = &result.plan,
                                        .blockers = &result.blockers,
                                        .warnings = &result.warnings});
    if (!rootListing.has_value()) {
        return result;
    }
    if (!copyToScratchImage(result.source_image_path,
                            result.written_image_path,
                            QLatin1StringView("directory-create"),
                            &result.blockers)) {
        return result;
    }

    QFile image;
    if (!openScratchImage(result.written_image_path,
                          QLatin1StringView("directory-create"),
                          &image,
                          &result.blockers)) {
        return result;
    }
    QStringList writeBlockers;
    runRootDirectoryRewriteOnScratch(
        {
            .image = &image,
            .sourceImagePath = result.source_image_path,
            .directoryName = cleanDirectoryName,
            .rootListing = *rootListing,
            .createDirectory = true,
        },
        &writeBlockers);
    image.close();
    result.blockers.append(writeBlockers);
    appendDirectoryCreateReadback(result.written_image_path, cleanDirectoryName, &result);
    finalizeDirectoryMutationResult(&result);
    return result;
}

PartitionApfsImageDirectoryMutationResult PartitionApfsWriter::deleteImageOnlyRootDirectory(
    const PartitionApfsImageRootDirectoryMutationRequest& request) {
    PartitionApfsImageDirectoryMutationResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();
    result.warnings.append(
        QStringLiteral("Generated APFS root-directory delete is limited to empty root directories "
                       "in S.A.K.-generated single-volume images"));

    const auto source = validateImageOnlySource(result.source_image_path,
                                                QLatin1StringView("directory-delete"),
                                                &result.blockers);
    if (!source.ok) {
        return result;
    }
    if (!appendSeparateOutputBlockers(source.info,
                                      result.written_image_path,
                                      QLatin1StringView("directory-delete"),
                                      &result.blockers)) {
        return result;
    }

    const QString cleanDirectoryName = request.directory_name.trimmed();
    result.directory_name = cleanDirectoryName;
    if (!appendRootDirectoryNameBlockers(
            cleanDirectoryName, QLatin1StringView("directory-delete"), &result.blockers)) {
        return result;
    }
    const auto detection = detectApfsImageSource(result.source_image_path,
                                                 static_cast<uint64_t>(source.info.size()),
                                                 QLatin1StringView("directory-delete"),
                                                 &result.blockers);
    if (!detection.has_value()) {
        return result;
    }
    const auto rootListing =
        prepareRootDirectoryDeletePlan({.detection = &*detection,
                                        .options = &request.options,
                                        .sourceImagePath = result.source_image_path,
                                        .cleanFileName = cleanDirectoryName,
                                        .plan = &result.plan,
                                        .blockers = &result.blockers,
                                        .warnings = &result.warnings});
    if (!rootListing.has_value()) {
        return result;
    }
    if (!copyToScratchImage(result.source_image_path,
                            result.written_image_path,
                            QLatin1StringView("directory-delete"),
                            &result.blockers)) {
        return result;
    }

    QFile image;
    if (!openScratchImage(result.written_image_path,
                          QLatin1StringView("directory-delete"),
                          &image,
                          &result.blockers)) {
        return result;
    }
    QStringList writeBlockers;
    runRootDirectoryRewriteOnScratch(
        {
            .image = &image,
            .sourceImagePath = result.source_image_path,
            .directoryName = cleanDirectoryName,
            .rootListing = *rootListing,
            .createDirectory = false,
        },
        &writeBlockers);
    image.close();
    result.blockers.append(writeBlockers);
    appendDirectoryDeleteReadback(result.written_image_path, cleanDirectoryName, &result);
    finalizeDirectoryMutationResult(&result);
    return result;
}

PartitionApfsImageVolumeLabelResult PartitionApfsWriter::changeImageOnlyVolumeLabel(
    const PartitionApfsImageVolumeLabelRequest& request) {
    PartitionApfsImageVolumeLabelResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();
    result.new_volume_name = normalizedApfsVolumeName(request.volume_name);
    result.warnings.append(QStringLiteral(
        "Generated APFS volume-label change is limited to S.A.K.-generated single-volume images"));

    const auto source = validateImageOnlySource(result.source_image_path,
                                                QLatin1StringView("volume-label"),
                                                &result.blockers);
    if (!source.ok) {
        return result;
    }
    if (!appendSeparateOutputBlockers(source.info,
                                      result.written_image_path,
                                      QLatin1StringView("volume-label"),
                                      &result.blockers)) {
        return result;
    }
    appendApfsVolumeNameBlockers(result.new_volume_name,
                                 QLatin1StringView("volume-label"),
                                 &result.blockers);
    if (!result.blockers.isEmpty()) {
        return result;
    }

    const auto detection = detectApfsImageSource(result.source_image_path,
                                                 static_cast<uint64_t>(source.info.size()),
                                                 QLatin1StringView("volume-label"),
                                                 &result.blockers);
    if (!detection.has_value()) {
        return result;
    }
    result.plan = planImageOnlyMutation(
        *detection, PartitionApfsWriteOperation::ChangeVolumeLabel, request.options, QString());
    result.plan.volume_name = result.new_volume_name;
    if (!appendPlanResult(result.plan, &result.blockers, &result.warnings)) {
        return result;
    }

    if (!copyToScratchImage(result.source_image_path,
                            result.written_image_path,
                            QLatin1StringView("volume-label"),
                            &result.blockers)) {
        return result;
    }

    QFile image;
    if (!openScratchImage(result.written_image_path,
                          QLatin1StringView("volume-label"),
                          &image,
                          &result.blockers)) {
        return result;
    }
    QStringList labelBlockers;
    runVolumeLabelChangeOnTarget(&image,
                                 static_cast<uint64_t>(source.info.size()),
                                 result.new_volume_name,
                                 &result.old_volume_name,
                                 &labelBlockers);
    image.close();
    result.blockers.append(labelBlockers);
    appendVolumeLabelReadback(result.written_image_path,
                              result.new_volume_name,
                              QLatin1StringView("volume-label"),
                              &result.blockers);
    finalizeVolumeLabelResult(&result);
    return result;
}

PartitionApfsRawFileWriteResult PartitionApfsWriter::writeRawRootFile(
    const PartitionApfsRawRootFileWriteRequest& request) {
    PartitionApfsRawFileWriteResult result;
    result.target_path = request.target_path.trimmed();
    result.warnings.append(
        QStringLiteral("Raw APFS file write remains certifier-only and hidden from user actions "
                       "until operation-specific queue/apply wiring, fsck_apfs/diskutil "
                       "validation, and crash replay proof are complete"));

    appendRawTargetMutationBlockers({.targetPath = result.target_path,
                                     .targetBytes = request.target_container_bytes,
                                     .confirmed = request.target_write_confirmed,
                                     .allowRawTarget = request.allow_raw_device_target,
                                     .options = &request.options,
                                     .purpose = QLatin1StringView("file-write")},
                                    &result.blockers);
    const QString cleanFileName = request.file_name.trimmed();
    result.file_name = cleanFileName;
    result.file_bytes = static_cast<uint64_t>(request.file_data.size());
    result.payload_sha256 = bytesSha256Hex(request.file_data);
    if (!appendFileWritePayloadBlockers({.source_image_path = result.target_path,
                                         .written_image_path = result.target_path,
                                         .file_name = cleanFileName,
                                         .file_data = request.file_data,
                                         .options = request.options},
                                        cleanFileName,
                                        &result.blockers)) {
        return result;
    }
    if (!result.blockers.isEmpty()) {
        return result;
    }

    const auto detection = detectApfsRawTarget(result.target_path,
                                               request.target_container_bytes,
                                               QLatin1StringView("file-write"),
                                               &result.blockers);
    if (!detection.has_value()) {
        return result;
    }
    const auto rootListing = prepareRootFileWritePlan({.detection = &*detection,
                                                       .options = &request.options,
                                                       .sourceImagePath = result.target_path,
                                                       .cleanFileName = cleanFileName,
                                                       .plan = &result.plan,
                                                       .blockers = &result.blockers,
                                                       .warnings = &result.warnings});
    if (!rootListing.has_value()) {
        return result;
    }

    QString openError;
    auto target = openFileOrRawDeviceReadWrite(result.target_path, &openError);
    if (!target) {
        result.blockers.append(
            QStringLiteral("Unable to open APFS raw file-write target: %1").arg(openError));
        return result;
    }
    QStringList writeBlockers;
    runRootFileWriteOnScratch({.image = target.get(),
                               .sourceImagePath = result.target_path,
                               .fileName = cleanFileName,
                               .fileData = request.file_data,
                               .rootListing = *rootListing,
                               .writtenDataBlocks = &result.written_data_blocks},
                              &writeBlockers);
    target->close();
    result.blockers.append(writeBlockers);
    appendRawFileWriteReadback(result.target_path, cleanFileName, request.file_data, &result);
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsRawFileDeleteResult PartitionApfsWriter::deleteRawRootFile(
    const PartitionApfsRawRootFileDeleteRequest& request) {
    PartitionApfsRawFileDeleteResult result;
    result.target_path = request.target_path.trimmed();
    result.warnings.append(
        QStringLiteral("Raw APFS file delete remains certifier-only and hidden from user actions "
                       "until operation-specific queue/apply wiring, fsck_apfs/diskutil "
                       "validation, and crash replay proof are complete"));

    appendRawTargetMutationBlockers({.targetPath = result.target_path,
                                     .targetBytes = request.target_container_bytes,
                                     .confirmed = request.target_write_confirmed,
                                     .allowRawTarget = request.allow_raw_device_target,
                                     .options = &request.options,
                                     .purpose = QLatin1StringView("file-delete")},
                                    &result.blockers);
    const QString cleanFileName = request.file_name.trimmed();
    result.file_name = cleanFileName;
    appendRootFileNameBlockers(cleanFileName, QLatin1StringView("file-delete"), &result.blockers);
    if (!result.blockers.isEmpty()) {
        return result;
    }

    const auto detection = detectApfsRawTarget(result.target_path,
                                               request.target_container_bytes,
                                               QLatin1StringView("file-delete"),
                                               &result.blockers);
    if (!detection.has_value()) {
        return result;
    }
    PartitionApfsFileEntry deletedEntry;
    QByteArray deletedData;
    const auto rootListing = prepareRootFileDeletePlan({.detection = &*detection,
                                                        .options = &request.options,
                                                        .sourceImagePath = result.target_path,
                                                        .cleanFileName = cleanFileName,
                                                        .plan = &result.plan,
                                                        .blockers = &result.blockers,
                                                        .warnings = &result.warnings},
                                                       &deletedEntry,
                                                       &deletedData);
    if (!rootListing.has_value()) {
        return result;
    }
    result.deleted_file_bytes = deletedEntry.size_bytes;
    result.deleted_file_sha256 = bytesSha256Hex(deletedData);

    QString openError;
    auto target = openFileOrRawDeviceReadWrite(result.target_path, &openError);
    if (!target) {
        result.blockers.append(
            QStringLiteral("Unable to open APFS raw file-delete target: %1").arg(openError));
        return result;
    }
    QStringList deleteBlockers;
    runRootFileDeleteOnScratch({.image = target.get(),
                                .sourceImagePath = result.target_path,
                                .fileName = cleanFileName,
                                .rootListing = *rootListing,
                                .targetSizeBytes = result.deleted_file_bytes,
                                .freedDataBlocks = &result.freed_data_blocks},
                               &deleteBlockers);
    target->close();
    result.blockers.append(deleteBlockers);
    appendRawFileDeleteReadback(result.target_path, cleanFileName, &result);
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsRawFileWriteResult PartitionApfsWriter::writeRawRootDirectoryFile(
    const PartitionApfsRawRootDirectoryFileWriteRequest& request) {
    PartitionApfsRawFileWriteResult result;
    result.target_path = request.target_path.trimmed();
    result.warnings.append(QStringLiteral(
        "Raw APFS directory-file write remains certifier-only until fsck_apfs/diskutil validation, "
        "crash replay, and hardware proof cover this operation"));

    appendRawTargetMutationBlockers({.targetPath = result.target_path,
                                     .targetBytes = request.target_container_bytes,
                                     .confirmed = request.target_write_confirmed,
                                     .allowRawTarget = request.allow_raw_device_target,
                                     .options = &request.options,
                                     .purpose = QLatin1StringView("directory-file-write")},
                                    &result.blockers);
    const QString cleanDirectoryName = request.directory_name.trimmed();
    const QString cleanFileName = request.file_name.trimmed();
    result.directory_name = cleanDirectoryName;
    result.file_name = cleanFileName;
    result.file_bytes = static_cast<uint64_t>(request.file_data.size());
    result.payload_sha256 = bytesSha256Hex(request.file_data);
    appendDirectoryFileWritePayloadBlockers({.source_image_path = result.target_path,
                                             .written_image_path = result.target_path,
                                             .directory_name = cleanDirectoryName,
                                             .file_name = cleanFileName,
                                             .file_data = request.file_data,
                                             .options = request.options},
                                            cleanDirectoryName,
                                            cleanFileName,
                                            &result.blockers);
    if (!result.blockers.isEmpty()) {
        return result;
    }

    const auto detection = detectApfsRawTarget(result.target_path,
                                               request.target_container_bytes,
                                               QLatin1StringView("directory-file-write"),
                                               &result.blockers);
    if (!detection.has_value()) {
        return result;
    }
    const auto rootListing =
        prepareRootDirectoryFileWritePlan({.detection = &*detection,
                                           .options = &request.options,
                                           .sourceImagePath = result.target_path,
                                           .cleanFileName = cleanFileName,
                                           .cleanDirectoryName = cleanDirectoryName,
                                           .plan = &result.plan,
                                           .blockers = &result.blockers,
                                           .warnings = &result.warnings});
    if (!rootListing.has_value()) {
        return result;
    }

    QString openError;
    auto target = openFileOrRawDeviceReadWrite(result.target_path, &openError);
    if (!target) {
        result.blockers.append(
            QStringLiteral("Unable to open APFS raw directory-file-write target: %1")
                .arg(openError));
        return result;
    }
    QStringList writeBlockers;
    runRootDirectoryFileRewriteOnScratch({.image = target.get(),
                                          .sourceImagePath = result.target_path,
                                          .directoryName = cleanDirectoryName,
                                          .fileName = cleanFileName,
                                          .fileData = request.file_data,
                                          .rootListing = *rootListing,
                                          .changedDataBlocks = &result.written_data_blocks},
                                         &writeBlockers);
    target->close();
    result.blockers.append(writeBlockers);
    appendRawDirectoryFileWriteReadback(
        result.target_path, cleanDirectoryName, cleanFileName, request.file_data, &result);
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsRawFileDeleteResult PartitionApfsWriter::deleteRawRootDirectoryFile(
    const PartitionApfsRawRootDirectoryFileDeleteRequest& request) {
    PartitionApfsRawFileDeleteResult result;
    result.target_path = request.target_path.trimmed();
    result.warnings.append(QStringLiteral(
        "Raw APFS directory-file delete remains certifier-only until fsck_apfs/diskutil "
        "validation, crash replay, and hardware proof cover this operation"));

    appendRawTargetMutationBlockers({.targetPath = result.target_path,
                                     .targetBytes = request.target_container_bytes,
                                     .confirmed = request.target_write_confirmed,
                                     .allowRawTarget = request.allow_raw_device_target,
                                     .options = &request.options,
                                     .purpose = QLatin1StringView("directory-file-delete")},
                                    &result.blockers);
    const QString cleanDirectoryName = request.directory_name.trimmed();
    const QString cleanFileName = request.file_name.trimmed();
    result.directory_name = cleanDirectoryName;
    result.file_name = cleanFileName;
    appendDirectoryFileNameBlockers(cleanDirectoryName,
                                    cleanFileName,
                                    QLatin1StringView("directory-file-delete"),
                                    &result.blockers);
    if (!result.blockers.isEmpty()) {
        return result;
    }

    const auto detection = detectApfsRawTarget(result.target_path,
                                               request.target_container_bytes,
                                               QLatin1StringView("directory-file-delete"),
                                               &result.blockers);
    if (!detection.has_value()) {
        return result;
    }
    PartitionApfsFileEntry deletedEntry;
    QByteArray deletedData;
    const auto rootListing =
        prepareRootDirectoryFileDeletePlan({.detection = &*detection,
                                            .options = &request.options,
                                            .sourceImagePath = result.target_path,
                                            .cleanFileName = cleanFileName,
                                            .cleanDirectoryName = cleanDirectoryName,
                                            .plan = &result.plan,
                                            .blockers = &result.blockers,
                                            .warnings = &result.warnings},
                                           &deletedEntry,
                                           &deletedData);
    if (!rootListing.has_value()) {
        return result;
    }
    result.deleted_file_bytes = deletedEntry.size_bytes;
    result.deleted_file_sha256 = bytesSha256Hex(deletedData);

    QString openError;
    auto target = openFileOrRawDeviceReadWrite(result.target_path, &openError);
    if (!target) {
        result.blockers.append(
            QStringLiteral("Unable to open APFS raw directory-file-delete target: %1")
                .arg(openError));
        return result;
    }
    QStringList deleteBlockers;
    runRootDirectoryFileRewriteOnScratch({.image = target.get(),
                                          .sourceImagePath = result.target_path,
                                          .directoryName = cleanDirectoryName,
                                          .fileName = cleanFileName,
                                          .rootListing = *rootListing,
                                          .targetSizeBytes = result.deleted_file_bytes,
                                          .changedDataBlocks = &result.freed_data_blocks,
                                          .deleteFile = true},
                                         &deleteBlockers);
    target->close();
    result.blockers.append(deleteBlockers);
    appendRawDirectoryFileDeleteReadback(
        result.target_path, cleanDirectoryName, cleanFileName, &result);
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsRawFilePatchResult PartitionApfsWriter::patchRawRootDirectoryFile(
    const PartitionApfsRawRootDirectoryFilePatchRequest& request) {
    PartitionApfsRawFilePatchResult result;
    result.target_path = request.target_path.trimmed();
    result.patch_offset_bytes = request.patch_offset_bytes;
    result.patch_bytes = static_cast<uint64_t>(request.patch_data.size());
    result.patch_sha256 = bytesSha256Hex(request.patch_data);
    result.warnings.append(QStringLiteral(
        "Raw APFS directory-file patch remains certifier-only until fsck_apfs/diskutil validation, "
        "crash replay, and hardware proof cover this operation"));

    appendRawTargetMutationBlockers({.targetPath = result.target_path,
                                     .targetBytes = request.target_container_bytes,
                                     .confirmed = request.target_write_confirmed,
                                     .allowRawTarget = request.allow_raw_device_target,
                                     .options = &request.options,
                                     .purpose = QLatin1StringView("directory-file-patch")},
                                    &result.blockers);
    const QString cleanDirectoryName = request.directory_name.trimmed();
    const QString cleanFileName = request.file_name.trimmed();
    result.directory_name = cleanDirectoryName;
    result.file_name = cleanFileName;
    appendDirectoryFilePatchPayloadBlockers({.source_image_path = result.target_path,
                                             .written_image_path = result.target_path,
                                             .directory_name = cleanDirectoryName,
                                             .file_name = cleanFileName,
                                             .patch_offset_bytes = request.patch_offset_bytes,
                                             .patch_data = request.patch_data,
                                             .options = request.options},
                                            cleanDirectoryName,
                                            cleanFileName,
                                            &result.blockers);
    if (!result.blockers.isEmpty()) {
        return result;
    }

    const auto detection = detectApfsRawTarget(result.target_path,
                                               request.target_container_bytes,
                                               QLatin1StringView("directory-file-patch"),
                                               &result.blockers);
    if (!detection.has_value()) {
        return result;
    }
    PartitionApfsFileEntry patchedEntry;
    QByteArray patchedData;
    const auto rootListing =
        prepareRootDirectoryFilePatchPlan({.detection = &*detection,
                                           .options = &request.options,
                                           .sourceImagePath = result.target_path,
                                           .cleanFileName = cleanFileName,
                                           .cleanDirectoryName = cleanDirectoryName,
                                           .plan = &result.plan,
                                           .blockers = &result.blockers,
                                           .warnings = &result.warnings},
                                          &patchedEntry,
                                          &patchedData);
    if (!rootListing.has_value() ||
        !applyRawDirectoryFilePatchPayload(request, patchedEntry, &patchedData, &result)) {
        return result;
    }

    QString openError;
    auto target = openFileOrRawDeviceReadWrite(result.target_path, &openError);
    if (!target) {
        result.blockers.append(
            QStringLiteral("Unable to open APFS raw directory-file-patch target: %1")
                .arg(openError));
        return result;
    }
    QStringList patchBlockers;
    runRootDirectoryFileRewriteOnScratch({.image = target.get(),
                                          .sourceImagePath = result.target_path,
                                          .directoryName = cleanDirectoryName,
                                          .fileName = cleanFileName,
                                          .fileData = patchedData,
                                          .rootListing = *rootListing,
                                          .changedDataBlocks = &result.written_data_blocks},
                                         &patchBlockers);
    target->close();
    result.blockers.append(patchBlockers);
    appendRawDirectoryFilePatchReadback(
        result.target_path, cleanDirectoryName, cleanFileName, patchedData, &result);
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsRawFilePatchResult PartitionApfsWriter::patchRawRootFile(
    const PartitionApfsRawRootFilePatchRequest& request) {
    PartitionApfsRawFilePatchResult result;
    result.target_path = request.target_path.trimmed();
    result.patch_offset_bytes = request.patch_offset_bytes;
    result.patch_bytes = static_cast<uint64_t>(request.patch_data.size());
    result.patch_sha256 = bytesSha256Hex(request.patch_data);
    result.warnings.append(
        QStringLiteral("Raw APFS file patch remains certifier-only and hidden from user actions "
                       "until operation-specific queue/apply wiring, fsck_apfs/diskutil "
                       "validation, and crash replay proof are complete"));

    appendRawTargetMutationBlockers({.targetPath = result.target_path,
                                     .targetBytes = request.target_container_bytes,
                                     .confirmed = request.target_write_confirmed,
                                     .allowRawTarget = request.allow_raw_device_target,
                                     .options = &request.options,
                                     .purpose = QLatin1StringView("file-patch")},
                                    &result.blockers);
    const QString cleanFileName = request.file_name.trimmed();
    result.file_name = cleanFileName;
    appendFilePatchPayloadBlockers({.source_image_path = result.target_path,
                                    .written_image_path = result.target_path,
                                    .file_name = cleanFileName,
                                    .patch_offset_bytes = request.patch_offset_bytes,
                                    .patch_data = request.patch_data,
                                    .options = request.options},
                                   cleanFileName,
                                   &result.blockers);
    if (!result.blockers.isEmpty()) {
        return result;
    }

    const auto detection = detectApfsRawTarget(result.target_path,
                                               request.target_container_bytes,
                                               QLatin1StringView("file-patch"),
                                               &result.blockers);
    if (!detection.has_value()) {
        return result;
    }
    PartitionApfsFileEntry patchedEntry;
    QByteArray patchedData;
    const auto rootListing = prepareRootFilePatchPlan({.detection = &*detection,
                                                       .options = &request.options,
                                                       .sourceImagePath = result.target_path,
                                                       .cleanFileName = cleanFileName,
                                                       .plan = &result.plan,
                                                       .blockers = &result.blockers,
                                                       .warnings = &result.warnings},
                                                      &patchedEntry,
                                                      &patchedData);
    if (!rootListing.has_value() ||
        !applyRawRootFilePatchPayload(request, patchedEntry, &patchedData, &result)) {
        return result;
    }

    QString openError;
    auto target = openFileOrRawDeviceReadWrite(result.target_path, &openError);
    if (!target) {
        result.blockers.append(
            QStringLiteral("Unable to open APFS raw file-patch target: %1").arg(openError));
        return result;
    }
    QStringList patchBlockers;
    runRootFileWriteOnScratch({.image = target.get(),
                               .sourceImagePath = result.target_path,
                               .fileName = cleanFileName,
                               .fileData = patchedData,
                               .rootListing = *rootListing,
                               .writtenDataBlocks = &result.written_data_blocks},
                              &patchBlockers);
    target->close();
    result.blockers.append(patchBlockers);
    appendRawFilePatchReadback(result.target_path, cleanFileName, patchedData, &result);
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsRawDirectoryMutationResult PartitionApfsWriter::createRawRootDirectory(
    const PartitionApfsRawRootDirectoryMutationRequest& request) {
    PartitionApfsRawDirectoryMutationResult result;
    result.target_path = request.target_path.trimmed();
    result.warnings.append(
        QStringLiteral("Raw APFS directory create remains certifier-only until fsck_apfs/diskutil "
                       "validation, crash replay, and hardware proof cover this operation"));

    appendRawTargetMutationBlockers({.targetPath = result.target_path,
                                     .targetBytes = request.target_container_bytes,
                                     .confirmed = request.target_write_confirmed,
                                     .allowRawTarget = request.allow_raw_device_target,
                                     .options = &request.options,
                                     .purpose = QLatin1StringView("directory-create")},
                                    &result.blockers);
    const QString cleanDirectoryName = request.directory_name.trimmed();
    result.directory_name = cleanDirectoryName;
    appendRootDirectoryNameBlockers(cleanDirectoryName,
                                    QLatin1StringView("directory-create"),
                                    &result.blockers);
    if (!result.blockers.isEmpty()) {
        return result;
    }

    const auto detection = detectApfsRawTarget(result.target_path,
                                               request.target_container_bytes,
                                               QLatin1StringView("directory-create"),
                                               &result.blockers);
    if (!detection.has_value()) {
        return result;
    }
    const auto rootListing = prepareRootDirectoryCreatePlan({.detection = &*detection,
                                                             .options = &request.options,
                                                             .sourceImagePath = result.target_path,
                                                             .cleanFileName = cleanDirectoryName,
                                                             .plan = &result.plan,
                                                             .blockers = &result.blockers,
                                                             .warnings = &result.warnings});
    if (!rootListing.has_value()) {
        return result;
    }

    QString openError;
    auto target = openFileOrRawDeviceReadWrite(result.target_path, &openError);
    if (!target) {
        result.blockers.append(
            QStringLiteral("Unable to open APFS raw directory-create target: %1").arg(openError));
        return result;
    }
    QStringList writeBlockers;
    runRootDirectoryRewriteOnScratch(
        {
            .image = target.get(),
            .sourceImagePath = result.target_path,
            .directoryName = cleanDirectoryName,
            .rootListing = *rootListing,
            .createDirectory = true,
        },
        &writeBlockers);
    target->close();
    result.blockers.append(writeBlockers);
    if (result.blockers.isEmpty()) {
        const auto entry = rootDirectoryReadbackEntry(result.target_path,
                                                      cleanDirectoryName,
                                                      QLatin1StringView("directory-create"),
                                                      &result.blockers);
        if (!entry.has_value() || !entry->directory) {
            result.blockers.append(QStringLiteral(
                "APFS raw directory-create read-back did not find the target directory"));
        }
    }
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsRawDirectoryMutationResult PartitionApfsWriter::deleteRawRootDirectory(
    const PartitionApfsRawRootDirectoryMutationRequest& request) {
    PartitionApfsRawDirectoryMutationResult result;
    result.target_path = request.target_path.trimmed();
    result.warnings.append(
        QStringLiteral("Raw APFS directory delete remains certifier-only until fsck_apfs/diskutil "
                       "validation, crash replay, and hardware proof cover this operation"));

    appendRawTargetMutationBlockers({.targetPath = result.target_path,
                                     .targetBytes = request.target_container_bytes,
                                     .confirmed = request.target_write_confirmed,
                                     .allowRawTarget = request.allow_raw_device_target,
                                     .options = &request.options,
                                     .purpose = QLatin1StringView("directory-delete")},
                                    &result.blockers);
    const QString cleanDirectoryName = request.directory_name.trimmed();
    result.directory_name = cleanDirectoryName;
    appendRootDirectoryNameBlockers(cleanDirectoryName,
                                    QLatin1StringView("directory-delete"),
                                    &result.blockers);
    if (!result.blockers.isEmpty()) {
        return result;
    }

    const auto detection = detectApfsRawTarget(result.target_path,
                                               request.target_container_bytes,
                                               QLatin1StringView("directory-delete"),
                                               &result.blockers);
    if (!detection.has_value()) {
        return result;
    }
    const auto rootListing = prepareRootDirectoryDeletePlan({.detection = &*detection,
                                                             .options = &request.options,
                                                             .sourceImagePath = result.target_path,
                                                             .cleanFileName = cleanDirectoryName,
                                                             .plan = &result.plan,
                                                             .blockers = &result.blockers,
                                                             .warnings = &result.warnings});
    if (!rootListing.has_value()) {
        return result;
    }

    QString openError;
    auto target = openFileOrRawDeviceReadWrite(result.target_path, &openError);
    if (!target) {
        result.blockers.append(
            QStringLiteral("Unable to open APFS raw directory-delete target: %1").arg(openError));
        return result;
    }
    QStringList writeBlockers;
    runRootDirectoryRewriteOnScratch(
        {
            .image = target.get(),
            .sourceImagePath = result.target_path,
            .directoryName = cleanDirectoryName,
            .rootListing = *rootListing,
            .createDirectory = false,
        },
        &writeBlockers);
    target->close();
    result.blockers.append(writeBlockers);
    if (result.blockers.isEmpty()) {
        const auto entry = rootDirectoryReadbackEntry(result.target_path,
                                                      cleanDirectoryName,
                                                      QLatin1StringView("directory-delete"),
                                                      &result.blockers);
        if (entry.has_value()) {
            result.blockers.append(
                QStringLiteral("APFS raw directory-delete read-back still found target directory"));
        }
    }
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsRawVolumeLabelResult PartitionApfsWriter::changeRawVolumeLabel(
    const PartitionApfsRawVolumeLabelRequest& request) {
    PartitionApfsRawVolumeLabelResult result;
    result.target_path = request.target_path.trimmed();
    result.new_volume_name = normalizedApfsVolumeName(request.volume_name);
    result.warnings.append(
        QStringLiteral("Raw APFS volume-label change is limited to S.A.K. generated APFS layouts; "
                       "arbitrary Apple APFS metadata mutation remains blocked by layout guards"));

    appendRawTargetMutationBlockers({.targetPath = result.target_path,
                                     .targetBytes = request.target_container_bytes,
                                     .confirmed = request.target_write_confirmed,
                                     .allowRawTarget = request.allow_raw_device_target,
                                     .options = &request.options,
                                     .purpose = QLatin1StringView("volume-label")},
                                    &result.blockers);
    appendApfsVolumeNameBlockers(result.new_volume_name,
                                 QLatin1StringView("volume-label"),
                                 &result.blockers);
    if (!result.blockers.isEmpty()) {
        return result;
    }

    const auto detection = detectApfsRawTarget(result.target_path,
                                               request.target_container_bytes,
                                               QLatin1StringView("volume-label"),
                                               &result.blockers);
    if (!detection.has_value()) {
        return result;
    }
    result.plan = planImageOnlyMutation(
        *detection, PartitionApfsWriteOperation::ChangeVolumeLabel, request.options, QString());
    result.plan.volume_name = result.new_volume_name;
    if (!appendPlanResult(result.plan, &result.blockers, &result.warnings)) {
        return result;
    }

    QString openError;
    auto target = openFileOrRawDeviceReadWrite(result.target_path, &openError);
    if (!target) {
        result.blockers.append(
            QStringLiteral("Unable to open APFS raw volume-label target: %1").arg(openError));
        return result;
    }
    QStringList labelBlockers;
    runVolumeLabelChangeOnTarget(target.get(),
                                 request.target_container_bytes,
                                 result.new_volume_name,
                                 &result.old_volume_name,
                                 &labelBlockers);
    target->close();
    result.blockers.append(labelBlockers);
    appendVolumeLabelReadback(result.target_path,
                              result.new_volume_name,
                              QLatin1StringView("raw volume-label"),
                              &result.blockers);
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsRawRepairResult PartitionApfsWriter::repairRawObjectChecksums(
    const PartitionApfsRawRepairRequest& request) {
    PartitionApfsRawRepairResult result;
    result.target_path = request.target_path.trimmed();
    result.warnings.append(
        QStringLiteral("Raw APFS checksum repair is limited to S.A.K. generated APFS layouts; "
                       "arbitrary Apple APFS repair remains blocked by layout guards"));

    appendRawTargetMutationBlockers({.targetPath = result.target_path,
                                     .targetBytes = request.target_container_bytes,
                                     .confirmed = request.target_repair_confirmed,
                                     .allowRawTarget = request.allow_raw_device_target,
                                     .options = &request.options,
                                     .purpose = QLatin1StringView("repair")},
                                    &result.blockers);
    if (!result.blockers.isEmpty()) {
        return result;
    }

    const auto detection = detectApfsRawTarget(result.target_path,
                                               request.target_container_bytes,
                                               QLatin1StringView("repair"),
                                               &result.blockers);
    if (!detection.has_value()) {
        return result;
    }
    result.plan = planImageOnlyMutation(
        *detection, PartitionApfsWriteOperation::RepairContainer, request.options, QString());
    if (!appendPlanResult(result.plan, &result.blockers, &result.warnings)) {
        return result;
    }

    QString openError;
    auto target = openFileOrRawDeviceReadWrite(result.target_path, &openError);
    if (!target) {
        result.blockers.append(
            QStringLiteral("Unable to open APFS raw repair target: %1").arg(openError));
        return result;
    }

    uint32_t blockSize = 0;
    uint64_t blockCount = 0;
    QStringList repairBlockers;
    if (readApfsRepairGeometry(target.get(), &blockSize, &blockCount, &repairBlockers)) {
        if (blockSize != kSupportedApfsBlockSizeBytes ||
            blockCount != request.target_container_bytes / blockSize) {
            repairBlockers.append(QStringLiteral(
                "APFS raw repair target geometry does not match expected target size"));
        } else if (!appendGeneratedApfsLayoutBlockers(target.get(),
                                                      {.blockSize = blockSize,
                                                       .blockCount = blockCount},
                                                      QLatin1StringView("repair"),
                                                      true,
                                                      &repairBlockers)) {
            result.scanned_blocks = 0;
            result.repaired_checksum_blocks = 0;
        } else {
            ApfsRepairCounters counters;
            repairGeneratedApfsObjectChecksumBlocks(target.get(),
                                                    {.blockSize = blockSize,
                                                     .blockCount = blockCount},
                                                    &counters,
                                                    &repairBlockers);
            result.scanned_blocks = counters.scannedBlocks;
            result.repaired_checksum_blocks = counters.repairedBlocks;
        }
    }
    target->close();
    result.blockers.append(repairBlockers);
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitImageOnlyCheckpoint(
    const PartitionApfsImageCheckpointCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();
    result.warnings.append(QStringLiteral(
        "Generated APFS in-place checkpoint commit remains image-only and is not exposed to user "
        "actions; raw in-place commit support requires fsck_apfs/diskutil, crash replay, and "
        "hardware evidence"));

    const auto source = validateImageOnlySource(result.source_image_path,
                                                QLatin1StringView("checkpoint-commit"),
                                                &result.blockers);
    if (!source.ok) {
        return result;
    }
    if (!appendSeparateOutputBlockers(source.info,
                                      result.written_image_path,
                                      QLatin1StringView("checkpoint-commit"),
                                      &result.blockers)) {
        return result;
    }
    if (!detectApfsImageSource(result.source_image_path,
                               static_cast<uint64_t>(source.info.size()),
                               QLatin1StringView("checkpoint-commit"),
                               &result.blockers)
             .has_value()) {
        return result;
    }
    if (!copyToScratchImage(result.source_image_path,
                            result.written_image_path,
                            QLatin1StringView("checkpoint-commit"),
                            &result.blockers)) {
        return result;
    }

    QFile image;
    if (!openScratchImage(result.written_image_path,
                          QLatin1StringView("checkpoint-commit"),
                          &image,
                          &result.blockers)) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceCheckpoint(&image, &commit, &commitBlockers)) {
        result.previous_xid = commit.previous_xid;
        result.new_xid = commit.new_xid;
        result.checkpoint_map_block = commit.checkpoint_map_block;
        result.superblock_block = commit.superblock_block;
    }
    image.close();
    result.blockers.append(commitBlockers);
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitImageOnlyFileInsert(
    const PartitionApfsImageFileInsertCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();
    result.warnings.append(QStringLiteral(
        "Generated APFS in-place file insert remains image-only and is not exposed to user "
        "actions; raw in-place commit support requires fsck_apfs/diskutil, crash replay, and "
        "hardware evidence"));

    const QString cleanFileName = request.file_name.trimmed();
    if (!appendRootFileNameBlockers(
            cleanFileName, QLatin1StringView("file-insert-commit"), &result.blockers)) {
        return result;
    }
    const auto source = validateImageOnlySource(result.source_image_path,
                                                QLatin1StringView("file-insert-commit"),
                                                &result.blockers);
    if (!source.ok) {
        return result;
    }
    if (!appendSeparateOutputBlockers(source.info,
                                      result.written_image_path,
                                      QLatin1StringView("file-insert-commit"),
                                      &result.blockers)) {
        return result;
    }
    if (!detectApfsImageSource(result.source_image_path,
                               static_cast<uint64_t>(source.info.size()),
                               QLatin1StringView("file-insert-commit"),
                               &result.blockers)
             .has_value()) {
        return result;
    }
    if (!copyToScratchImage(result.source_image_path,
                            result.written_image_path,
                            QLatin1StringView("file-insert-commit"),
                            &result.blockers)) {
        return result;
    }

    QFile image;
    if (!openScratchImage(result.written_image_path,
                          QLatin1StringView("file-insert-commit"),
                          &image,
                          &result.blockers)) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceFileInsert(&image, cleanFileName, &commit, &commitBlockers)) {
        result.previous_xid = commit.previous_xid;
        result.new_xid = commit.new_xid;
        result.checkpoint_map_block = commit.checkpoint_map_block;
        result.superblock_block = commit.superblock_block;
    }
    image.close();
    result.blockers.append(commitBlockers);
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsWritePreflight PartitionApfsWriter::validateImageOnlyExecutionEvidence(
    const PartitionApfsImageMutationPlan& plan,
    const PartitionApfsWriterExecutionEvidence& evidence) {
    PartitionApfsWritePreflight result;
    result.required_evidence = enterpriseCertificationRequirements();
    appendPlanEvidenceMismatchBlockers(plan, evidence, &result);
    appendMissingExecutionEvidenceBlockers(plan, evidence, &result);
    if (!evidence.hardware_raw_media_verified) {
        result.warnings.append(
            QStringLiteral("APFS image-only evidence is not raw-media certification; raw helper "
                           "execution still requires separate hardware evidence"));
    }
    result.allowed = result.blockers.isEmpty();
    return result;
}

}  // namespace sak

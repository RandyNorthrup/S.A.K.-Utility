// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_apfs_writer.cpp
/// @brief Fail-closed APFS write preflight for future Partition Manager mutation support.

#include "sak/partition_apfs_writer.h"

#include "sak/apfs_compression.h"
#include "sak/apfs_crypto.h"
#include "sak/apfs_keybag.h"
#include "sak/partition_apfs_file_system_reader.h"
#include "sak/partition_raw_device_io.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QStringView>
#include <QtEndian>
#include <QUuid>

#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <utility>

namespace sak {

namespace {

constexpr uint64_t kDefaultMaxApfsPayloadBytes = 64ULL * 1024ULL * 1024ULL;
// In-place checkpoint commits run on any single-CIB, multi-CIB, metadata-overflow, or
// CAB-tier container (computeGeneratedLayout + the unified cib0/cab0/boundary rotation
// group are layout-general): apfsck-clean from 64 MiB single-CIB through the CAB tier,
// Apple fsck_apfs + kernel-continuation certified through the metadata-overflow tier
// (2 TiB) and the CAB tier (8 TiB, cab_count 2). The cap is a round ceiling above the
// Apple-certified CAB FORMAT range (24 TiB) and below the format fail-close (~48 TiB);
// the scratch copy stays cheap on reflink/sparse hosts (copy_file_range preserves
// holes), so logical size does not drive the commit cost there.
constexpr uint64_t kApfsInPlaceCommitMaxBytes = 32ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
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
// Multi-volume (A4): physical blocks each additional volume consumes -- its object
// map + that map's tree, extent-ref tree, snap-meta tree, root tree, and superblock.
constexpr uint64_t kApfsExtraVolumeBlockSpan = 6;
constexpr uint64_t kApfsFormatStaleSignatureClearBytes = 8ULL * 1024ULL * 1024ULL;
constexpr qsizetype kApfsFormatZeroChunkBytes = 1024 * 1024;
constexpr uint64_t kApfsMaximumSeedFileBytes = 256ULL * 1024ULL * 1024ULL;
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
constexpr uint32_t kApfsObjectTypeBtree = 0x00'00'00'02;      // B-tree root node
constexpr uint32_t kApfsObjectTypeBtreeNode = 0x00'00'00'03;  // non-root B-tree node
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
// SPACEMAN_CAB: the chunk-info ADDRESS block (apfs_cib_addr_block) the CAB tier
// uses when the inline cib-address array no longer fits the spaceman block. Each
// holds up to cibs_per_cab (507) cib block numbers.
constexpr uint32_t kApfsObjectTypeCibAddrBlock = kApfsObjStoragePhysical | 0x00'00'00'06;
constexpr uint32_t kApfsObjectSubtypeFsTree = 0x00'00'00'0E;
constexpr uint32_t kApfsMagicNxsb = 0x42'53'58'4E;
constexpr uint32_t kApfsMagicApsb = 0x42'53'50'41;
constexpr uint32_t kApfsContainerIncompatVersion2 = 0x00'00'00'02;
constexpr uint16_t kApfsBtreeNodeRoot = 0x0001;
constexpr uint16_t kApfsBtreeNodeLeaf = 0x0002;
constexpr uint16_t kApfsBtreeNodeFixedKvSize = 0x0004;
constexpr uint32_t kApfsBtreePhysical = 0x00'00'00'10;
constexpr uint8_t kApfsRecordInode = 3;
constexpr uint8_t kApfsRecordXattr = 4;
constexpr uint8_t kApfsRecordSiblingLink = 5;  // APFS_TYPE_SIBLING_LINK (hard-link names)
constexpr uint8_t kApfsRecordDstreamId = 6;
constexpr uint8_t kApfsRecordCryptoState = 7;  // APFS_TYPE_CRYPTO_STATE
constexpr uint8_t kApfsRecordFileExtent = 8;
constexpr uint8_t kApfsRecordDirectoryEntry = 9;
constexpr uint8_t kApfsRecordSiblingMap = 12;  // APFS_TYPE_SIBLING_MAP (sibling id -> inode)
// A7 (A-h) hard links: the dentry extended-field type carrying a name's sibling id
// (APFS_DREC_EXT_TYPE_SIBLING_ID); each hard-link dentry references one sibling record.
constexpr uint8_t kApfsDrecExtTypeSiblingId = 1;
// CRYPTO_SW_ID: the well-known object id of an encrypted volume's default
// whole-volume crypto-state record (A6).
constexpr uint64_t kApfsCryptoSwId = 4;
// apfs_inode_val.bsd_flags @0x44 and apfs_inode_val.uncompressed_size @0x54; a
// compressed file sets UF_COMPRESSED in the former and pairs a non-zero size in
// the latter with APFS_INODE_HAS_UNCOMPRESSED_SIZE in internal_flags @0x30.
constexpr qsizetype kApfsInodeBsdFlagsOffset = 0x44;
constexpr qsizetype kApfsInodeUncompressedSizeOffset = 0x54;
constexpr qsizetype kApfsInodeInternalFlagsOffset = 0x30;
// A7 (A-h) j_inode_val internal_flags (apfs/raw.h): a named-attribute or sparse
// inode must set the matching flag or fsck_apfs/apfsck rejects it (inode.c
// cross-checks i_xattr_bmap SECURITY/FINDER_INFO against these flags exactly).
constexpr uint64_t kApfsInodeFlagWasCloned = 0x00'00'00'10;
constexpr uint64_t kApfsInodeFlagHasSecurityEa = 0x00'00'00'40;
constexpr uint64_t kApfsInodeFlagHasFinderInfo = 0x00'00'01'00;
constexpr uint64_t kApfsInodeFlagIsSparse = 0x00'00'02'00;
constexpr uint64_t kApfsInodeFlagWasEverCloned = 0x00'00'04'00;
// Well-known extended-attribute names whose presence drives the inode flags above.
constexpr char kApfsXattrNameSecurity[] = "com.apple.system.Security";
constexpr char kApfsXattrNameFinderInfo[] = "com.apple.FinderInfo";
// A7 (A-h) sparse inode: an INO_EXT_TYPE_SPARSE_BYTES (13) xfield carrying the hole
// byte count, flags = XF_SYSTEM_FIELD | XF_CHILDREN_INHERIT (apfsck inode.c requires
// this xfield exactly when APFS_INODE_IS_SPARSE is set).
constexpr uint8_t kApfsInodeSparseBytesField = 13;
constexpr uint8_t kApfsXfieldFlagsSparseBytes = 0x28;
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
// nx_keylocker prange (A6): harvested byte-offset from a real macOS encrypted
// container (paddr@0x510, block_count@0x518).
constexpr qsizetype kApfsNxKeylockerOffset = 0x510;
constexpr qsizetype kApfsOmapTreeOidOffset = 0x30;
constexpr qsizetype kApfsOmapSnapshotCountOffset = 0x24;
constexpr qsizetype kApfsOmapSnapshotTreeOidOffset = 0x38;
constexpr qsizetype kApfsOmapMostRecentSnapshotOffset = 0x40;
constexpr qsizetype kApfsOmapPendingRevertMinOffset = 0x48;
constexpr qsizetype kApfsOmapPendingRevertMaxOffset = 0x50;
constexpr qsizetype kApfsVolumeIncompatibleFeaturesOffset = 0x38;
// A7 (A-h) sealed-volume policy: a signed-system (sealed) volume sets
// APFS_INCOMPAT_SEALED_VOLUME and carries an integrity-meta object. Mutating it
// breaks the volume seal, so S.A.K. fails closed on any sealed-volume commit.
constexpr uint64_t kApfsVolumeIncompatSealed = 0x00'00'00'20;
constexpr qsizetype kApfsVolumeIntegrityMetaOidOffset = 0x400;
constexpr qsizetype kApfsVolumeAllocatedBlockCountOffset = 0x58;
constexpr qsizetype kApfsVolumeOmapOidOffset = 0x80;
constexpr qsizetype kApfsVolumeRootTreeOidOffset = 0x88;
constexpr qsizetype kApfsVolumeSnapMetaTreeOidOffset = 0x98;
constexpr qsizetype kApfsVolumeRevertToXidOffset = 0xA0;
constexpr qsizetype kApfsVolumeRevertToSblockOidOffset = 0xA8;
constexpr qsizetype kApfsVolumeNextObjectIdOffset = 0xB0;
constexpr qsizetype kApfsVolumeFileCountOffset = 0xB8;
constexpr qsizetype kApfsVolumeDirectoryCountOffset = 0xC0;
constexpr qsizetype kApfsVolumeExtentRefTreeOidOffset = 0x90;
constexpr qsizetype kApfsVolumeNumSnapshotsOffset = 0xD8;
// j_snap_metadata value layout (snapshot-metadata tree leaf record, A3 create).
constexpr qsizetype kApfsSnapMetaExtentRefOidOffset = 0x00;
constexpr qsizetype kApfsSnapMetaSblockOidOffset = 0x08;
constexpr qsizetype kApfsSnapMetaCreateTimeOffset = 0x10;
constexpr qsizetype kApfsSnapMetaChangeTimeOffset = 0x18;
constexpr qsizetype kApfsSnapMetaInumOffset = 0x20;
constexpr qsizetype kApfsSnapMetaExtentRefTypeOffset = 0x28;
constexpr qsizetype kApfsSnapMetaFlagsOffset = 0x2C;
constexpr qsizetype kApfsSnapMetaNameLenOffset = 0x30;
constexpr qsizetype kApfsSnapMetaNameOffset = 0x32;
constexpr qsizetype kApfsSnapMetaValueHeaderBytes = 0x32;  // 50-byte fixed header before the name
// omap-snapshot tree node: fixed-kv, subtype 0x13, 8-byte xid key + 16-byte omap_snapshot value.
constexpr uint32_t kApfsObjectSubtypeOmapSnapshot = 0x00'00'00'13;
constexpr qsizetype kApfsOmapSnapshotTreeTocBytes = 576;
constexpr qsizetype kApfsOmapSnapshotKeyBytes = 8;
constexpr qsizetype kApfsOmapSnapshotValueBytes = 16;
// j_snap key obj_id_and_type record types (high nibble of the 8-byte key header).
constexpr uint64_t kApfsJObjTypeSnapMetadata = 1;
constexpr uint64_t kApfsJObjTypeSnapName = 11;
constexpr uint64_t kApfsJObjIdMask = (1ULL << kApfsObjTypeShift) - 1;
constexpr uint32_t kApfsVolumePhysicalSblockType =
    kApfsObjStoragePhysical | kApfsObjectTypeFs;            // 0x4000000d (frozen sblock object)
constexpr uint32_t kApfsExtentRefTreeType = 0x40'00'00'02;  // physical btree (j_snap_metadata)
// apfs_fs_alloc_count is a LOGICAL count: a snapshot re-walks the volume's catalog,
// its frozen extent-ref tree (a copy of the live one), and the file extents, but NOT
// the live volume superblock, its single-node object-map tree, or its single-node
// snap-meta tree. So creating the first snapshot adds (alloc_count - these 3) to the
// count -> new alloc_count = 2*before - 3. Verified against apfsck's walk (empty
// container: 5 -> 7) and confirmed by fsck_apfs. The 3 assumes the object-map tree is
// single-node (level 0); snapshot create fails closed on a multi-level object map.
constexpr uint64_t kApfsSnapshotLiveOnlyBlocks = 3;
// Creating a snapshot raises the volume's logical apfs_fs_alloc_count by a CONSTANT
// (the snapshot's fixed metadata the volume now owns: the frozen superblock + one
// snap structure), independent of how much data the volume holds. apfsck checks
// apfs_fs_alloc_count == the walked block count: on a 5-block empty volume it goes
// 5 -> 7, on a 6-block one-file volume 6 -> 8 (the earlier 2*before-3 formula only
// coincided with +2 at before=5; a one-file volume disproved it via apfsck).
constexpr uint64_t kApfsSnapshotAllocDelta = 2;
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
constexpr qsizetype kApfsSpacemanDeviceCabCountOffset = 0x14;
constexpr qsizetype kApfsSpacemanDeviceAddrOffsetOffset = 0x20;
constexpr qsizetype kApfsSpacemanFlagsOffset = 0x90;
constexpr uint32_t kApfsSpacemanBlocksPerChunk = 32'768;
constexpr uint32_t kApfsSpacemanChunksPerCib = 126;
constexpr uint32_t kApfsSpacemanCibsPerCab = 507;
constexpr uint64_t kGeneratedApfsSingleChunkMaxBlocks = kApfsSpacemanBlocksPerChunk;
constexpr uint64_t kGeneratedApfsSingleChunkMaxBytes =
    kGeneratedApfsSingleChunkMaxBlocks * static_cast<uint64_t>(kSupportedApfsBlockSizeBytes);
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
// IP-bitmap ring detail (offsets within the spaceman, mirrored from the kernel-
// advanced harvest): the ring transaction id, a per-commit sequence counter, and
// the 16-entry ring array whose two 0xFFFF markers are the in-use ip-bitmap slots.
constexpr qsizetype kApfsSpacemanIpBmRingXidOffset = 2520;
constexpr qsizetype kApfsSpacemanIpBmRingSeqOffset = 2528;
constexpr qsizetype kApfsSpacemanIpBmRingArrayOffset = 2536;
constexpr uint16_t kApfsSpacemanIpBmRingInUse = 0xFFFF;
// Free-queue tree_node_limit values mirrored from the reference container;
// nonzero even when the queues themselves are empty.
constexpr qsizetype kApfsSpacemanFqIpLimitOffset = 0xE0;
constexpr qsizetype kApfsSpacemanFqMainLimitOffset = 0x108;
// sm_fq[IP]: sfq_count (total ghost blocks pending in the IP free-queue) and
// sfq_oldest_xid (the oldest still-pending transaction). The kernel rebuilds both
// from the rolling free-queue window on every cib rotation.
constexpr qsizetype kApfsSpacemanFqIpCountOffset = 0xC8;
constexpr qsizetype kApfsSpacemanFqIpOldestXidOffset = 0xD8;
// sm_fq[MAIN]: the main-device free-queue tracks blocks the commit freed but keeps
// pending (still allocated in the bitmap) until they age past the rollback window,
// so an interrupted commit's predecessor keeps its blocks. sfq_count = total
// pending blocks, sfq_oldest_xid = the oldest still-pending transaction.
constexpr qsizetype kApfsSpacemanFqMainCountOffset = 0xF0;
constexpr qsizetype kApfsSpacemanFqMainOldestXidOffset = 0x100;
constexpr qsizetype kApfsChunkInfoIndexOffset = 0x20;
constexpr qsizetype kApfsChunkInfoCountOffset = 0x24;
constexpr qsizetype kApfsChunkInfoEntriesOffset = 0x28;
constexpr qsizetype kApfsChunkInfoEntryAddrOffset = 8;
constexpr qsizetype kApfsChunkInfoEntryBlockCountOffset = 16;
constexpr qsizetype kApfsChunkInfoEntryFreeCountOffset = 20;
constexpr qsizetype kApfsChunkInfoEntryBitmapAddrOffset = 24;
constexpr qsizetype kApfsChunkInfoEntryStride = 32;
// apfs_cib_addr_block (SPACEMAN_CAB) field offsets, past the 32-byte object header.
constexpr qsizetype kApfsCibAddrIndexOffset = 0x20;
constexpr qsizetype kApfsCibAddrCibCountOffset = 0x24;
constexpr qsizetype kApfsCibAddrEntriesOffset = 0x28;
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
// A6: omap value flag marking an AES-XTS-encrypted virtual object, and the volume
// fs_flags value for a software-encrypted whole-volume-key (FileVault) volume.
constexpr uint32_t kApfsOmapValueEncrypted = 0x00'00'00'04;
constexpr uint64_t kApfsVolumeFsFlagsOneKey = 0x8;
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
constexpr qsizetype kUint16Size = 2;
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
    // ov_flags (A6): OMAP_VAL_ENCRYPTED (0x4) marks the target virtual object
    // (the volume file-system tree) as AES-XTS-encrypted with the volume key.
    uint32_t flags{0};
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
        writeLe32(&block, value, mappings.at(index).flags);
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

// One contiguous run of a file's data: the file-logical block where the run
// starts, its physical start block, and its block count. A file whose data is
// fragmented across free space carries several of these.
struct ApfsDataExtent {
    uint64_t logicalBlock{0};
    uint64_t paddr{0};
    uint64_t blockCount{0};
};

// A7 (A-h) hard link: one additional name (beyond a file's primary name) resolving to
// the same inode. parentId is the directory holding the name; siblingId is the unique
// sibling-record id assigned to it.
struct ApfsHardLinkName {
    QString name;
    uint64_t parentId{kApfsRootDirectoryId};
    uint64_t siblingId{0};
};

struct ApfsRootFilePayload {
    QString fileName;
    QString parentDirectoryName;
    QByteArray data;
    uint64_t parentDirectoryId{kApfsRootDirectoryId};
    uint64_t fileId{0};
    uint64_t privateId{0};
    uint64_t dataStartBlock{0};
    // Empty: the data is the single contiguous run at dataStartBlock (the original
    // path, byte-identical). Non-empty: an explicit multi-run extent list.
    QVector<ApfsDataExtent> dataExtents;
    // Transparent compression (A5): when set, the file carries no data stream;
    // its logical content lives in an embedded com.apple.decmpfs xattr
    // (decmpfsXattr) and its inode is UF_COMPRESSED with uncompressedSize bytes.
    // `data` is empty for a compressed file (no extents are allocated).
    bool compressed{false};
    QByteArray decmpfsXattr;
    uint64_t uncompressedSize{0};
    // A7 (A-h): arbitrary named extended attributes (user xattrs, ACL blobs in
    // com.apple.system.Security, Finder info, ...). Each becomes a j_xattr record
    // with an embedded value. Empty keeps the certified layout byte-identical.
    QVector<QPair<QByteArray, QByteArray>> xattrs;
    // A7 (A-h): when set, the inode is sparse -- the dstream logical size exceeds
    // the bytes its extents cover, and the uncovered logical ranges read as zeros
    // (a hole). The reader already zero-fills gaps between extents.
    bool sparse{false};
    uint64_t sparseLogicalSize{0};
    // A7 (A-h): extra internal_flags OR-ed into the inode (WAS_CLONED / WAS_EVER_CLONED
    // on a clone). 0 keeps every other file byte-identical.
    uint64_t extraInodeFlags{0};
    // A7 (A-h) hard links: additional names (beyond fileName) that resolve to THIS
    // inode. When non-empty the file is hard-linked: nlink == 1 + additionalLinks
    // count, every name (primary + additional) carries a sibling-id dentry xfield and
    // gets a sibling-link + sibling-map record, and primarySiblingId is the lowest of
    // the sibling ids (Apple stores the primary name as the lowest-id sibling). Empty
    // keeps a single-link file byte-identical.
    QVector<ApfsHardLinkName> additionalLinks;
    uint64_t primarySiblingId{0};
};

// Group ascending block addresses into contiguous runs, assigning each run its
// file-logical start block (the cumulative block index).
QVector<ApfsDataExtent> groupContiguousRuns(const QVector<uint64_t>& blocks) {
    QVector<ApfsDataExtent> extents;
    uint64_t logical = 0;
    for (uint64_t block : blocks) {
        if (!extents.isEmpty() && extents.last().paddr + extents.last().blockCount == block) {
            extents.last().blockCount += 1;
        } else {
            extents.append({logical, block, 1});
        }
        logical += 1;
    }
    return extents;
}

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

// Unicode FULL case-folding expansions (CaseFolding.txt status F: the 1-to-many
// mappings) that Apple applies but Qt's toCaseFolded does not. Generated from
// Python str.casefold() (full folding): 104 BMP code points, each folding to at
// most three. Qt already applies the 1-to-1 (common) folds correctly, so adding
// these reproduces Apple's hash for every script (German eszett, the Greek
// iota-subscript letters, the Latin/Armenian ligatures, ...).
const QHash<char32_t, QVector<char32_t>>& fullCaseFoldExpansions() {
    static const QHash<char32_t, QVector<char32_t>> table = {
        {0x00DF, {0x0073, 0x0073}},         {0x0130, {0x0069, 0x0307}},
        {0x0149, {0x02BC, 0x006E}},         {0x01F0, {0x006A, 0x030C}},
        {0x0390, {0x03B9, 0x0308, 0x0301}}, {0x03B0, {0x03C5, 0x0308, 0x0301}},
        {0x0587, {0x0565, 0x0582}},         {0x1E96, {0x0068, 0x0331}},
        {0x1E97, {0x0074, 0x0308}},         {0x1E98, {0x0077, 0x030A}},
        {0x1E99, {0x0079, 0x030A}},         {0x1E9A, {0x0061, 0x02BE}},
        {0x1E9E, {0x0073, 0x0073}},         {0x1F50, {0x03C5, 0x0313}},
        {0x1F52, {0x03C5, 0x0313, 0x0300}}, {0x1F54, {0x03C5, 0x0313, 0x0301}},
        {0x1F56, {0x03C5, 0x0313, 0x0342}}, {0x1F80, {0x1F00, 0x03B9}},
        {0x1F81, {0x1F01, 0x03B9}},         {0x1F82, {0x1F02, 0x03B9}},
        {0x1F83, {0x1F03, 0x03B9}},         {0x1F84, {0x1F04, 0x03B9}},
        {0x1F85, {0x1F05, 0x03B9}},         {0x1F86, {0x1F06, 0x03B9}},
        {0x1F87, {0x1F07, 0x03B9}},         {0x1F88, {0x1F00, 0x03B9}},
        {0x1F89, {0x1F01, 0x03B9}},         {0x1F8A, {0x1F02, 0x03B9}},
        {0x1F8B, {0x1F03, 0x03B9}},         {0x1F8C, {0x1F04, 0x03B9}},
        {0x1F8D, {0x1F05, 0x03B9}},         {0x1F8E, {0x1F06, 0x03B9}},
        {0x1F8F, {0x1F07, 0x03B9}},         {0x1F90, {0x1F20, 0x03B9}},
        {0x1F91, {0x1F21, 0x03B9}},         {0x1F92, {0x1F22, 0x03B9}},
        {0x1F93, {0x1F23, 0x03B9}},         {0x1F94, {0x1F24, 0x03B9}},
        {0x1F95, {0x1F25, 0x03B9}},         {0x1F96, {0x1F26, 0x03B9}},
        {0x1F97, {0x1F27, 0x03B9}},         {0x1F98, {0x1F20, 0x03B9}},
        {0x1F99, {0x1F21, 0x03B9}},         {0x1F9A, {0x1F22, 0x03B9}},
        {0x1F9B, {0x1F23, 0x03B9}},         {0x1F9C, {0x1F24, 0x03B9}},
        {0x1F9D, {0x1F25, 0x03B9}},         {0x1F9E, {0x1F26, 0x03B9}},
        {0x1F9F, {0x1F27, 0x03B9}},         {0x1FA0, {0x1F60, 0x03B9}},
        {0x1FA1, {0x1F61, 0x03B9}},         {0x1FA2, {0x1F62, 0x03B9}},
        {0x1FA3, {0x1F63, 0x03B9}},         {0x1FA4, {0x1F64, 0x03B9}},
        {0x1FA5, {0x1F65, 0x03B9}},         {0x1FA6, {0x1F66, 0x03B9}},
        {0x1FA7, {0x1F67, 0x03B9}},         {0x1FA8, {0x1F60, 0x03B9}},
        {0x1FA9, {0x1F61, 0x03B9}},         {0x1FAA, {0x1F62, 0x03B9}},
        {0x1FAB, {0x1F63, 0x03B9}},         {0x1FAC, {0x1F64, 0x03B9}},
        {0x1FAD, {0x1F65, 0x03B9}},         {0x1FAE, {0x1F66, 0x03B9}},
        {0x1FAF, {0x1F67, 0x03B9}},         {0x1FB2, {0x1F70, 0x03B9}},
        {0x1FB3, {0x03B1, 0x03B9}},         {0x1FB4, {0x03AC, 0x03B9}},
        {0x1FB6, {0x03B1, 0x0342}},         {0x1FB7, {0x03B1, 0x0342, 0x03B9}},
        {0x1FBC, {0x03B1, 0x03B9}},         {0x1FC2, {0x1F74, 0x03B9}},
        {0x1FC3, {0x03B7, 0x03B9}},         {0x1FC4, {0x03AE, 0x03B9}},
        {0x1FC6, {0x03B7, 0x0342}},         {0x1FC7, {0x03B7, 0x0342, 0x03B9}},
        {0x1FCC, {0x03B7, 0x03B9}},         {0x1FD2, {0x03B9, 0x0308, 0x0300}},
        {0x1FD3, {0x03B9, 0x0308, 0x0301}}, {0x1FD6, {0x03B9, 0x0342}},
        {0x1FD7, {0x03B9, 0x0308, 0x0342}}, {0x1FE2, {0x03C5, 0x0308, 0x0300}},
        {0x1FE3, {0x03C5, 0x0308, 0x0301}}, {0x1FE4, {0x03C1, 0x0313}},
        {0x1FE6, {0x03C5, 0x0342}},         {0x1FE7, {0x03C5, 0x0308, 0x0342}},
        {0x1FF2, {0x1F7C, 0x03B9}},         {0x1FF3, {0x03C9, 0x03B9}},
        {0x1FF4, {0x03CE, 0x03B9}},         {0x1FF6, {0x03C9, 0x0342}},
        {0x1FF7, {0x03C9, 0x0342, 0x03B9}}, {0x1FFC, {0x03C9, 0x03B9}},
        {0xFB00, {0x0066, 0x0066}},         {0xFB01, {0x0066, 0x0069}},
        {0xFB02, {0x0066, 0x006C}},         {0xFB03, {0x0066, 0x0066, 0x0069}},
        {0xFB04, {0x0066, 0x0066, 0x006C}}, {0xFB05, {0x0073, 0x0074}},
        {0xFB06, {0x0073, 0x0074}},         {0xFB13, {0x0574, 0x0576}},
        {0xFB14, {0x0574, 0x0565}},         {0xFB15, {0x0574, 0x056B}},
        {0xFB16, {0x057E, 0x0576}},         {0xFB17, {0x0574, 0x056D}},
    };
    return table;
}

// Apple's case-insensitive dirent hash case-folds (FULL Unicode folding) then
// NFD-normalizes the name. Qt's toCaseFolded handles the 1-to-1 folds; the 1-to-
// many ones come from fullCaseFoldExpansions().
QString appleCaseFold(const QString& name) {
    QString folded;
    for (char32_t codePoint : name.toStdU32String()) {
        const auto expansion = fullCaseFoldExpansions().constFind(codePoint);
        if (expansion != fullCaseFoldExpansions().constEnd()) {
            for (char32_t out : *expansion) {
                folded += QString::fromUcs4(&out, 1);
            }
        } else {
            folded += QString::fromUcs4(&codePoint, 1).toCaseFolded();
        }
    }
    return folded;
}

// j_drec_hashed_key name_len_and_hash: low 10 bits hold the name length
// (including the trailing NUL); the high 22 bits hold a CRC-32C over the UTF-32
// code points of the name's FULL-case-folded NFD form (seed ~0, no final
// inversion). The S.A.K. volume sets APFS_INCOMPAT_CASE_INSENSITIVE, so Apple and
// apfsck normalize the name this way before hashing; ASCII decomposes to itself
// and folds A-Z -> a-z, so ASCII names stay byte-identical.
uint32_t drecNameLenAndHash(const QString& name) {
    const QString normalized = appleCaseFold(name).normalized(QString::NormalizationForm_D);
    uint32_t crc = 0xFF'FF'FF'FFu;
    for (char32_t codePoint : normalized.toStdU32String()) {
        crc = crc32cWord(crc, static_cast<uint32_t>(codePoint));
    }
    // The stored length is the byte count of the ORIGINAL name (+ NUL), not the
    // normalized form.
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
    // A5: a transparently-compressed regular file keeps its content in a
    // com.apple.decmpfs xattr, so it has no data-stream xfield. The inode is
    // stamped UF_COMPRESSED and reports uncompressedSize via the dedicated field.
    bool compressed = false;
    uint64_t uncompressedSize = 0;
    // A7 (A-h): extra internal_flags OR-ed in (IS_SPARSE / WAS_EVER_CLONED /
    // HAS_SECURITY_EA / HAS_FINDER_INFO), derived from the inode's attributes.
    uint64_t extraInternalFlags = 0;
    // A7 (A-h): a sparse inode's dstream allocated size is fewer bytes than its
    // logical size (the difference is the hole). 0 = dense (alloced == rounded size).
    uint64_t allocedSizeBytes = 0;
};

// internal_flags base (0x8000) plus APFS_INODE_HAS_UNCOMPRESSED_SIZE when the
// inode reports an uncompressed_size (compressed files only), plus any A7 attribute
// flags (sparse / cloned / security-EA / finder-info).
uint64_t inodeInternalFlags(bool compressed, uint64_t extra) {
    return 0x8000ULL | (compressed ? kApfsInodeHasUncompressedSize : 0ULL) | extra;
}

// Stamp the compressed-file accounting onto a j_inode_val: UF_COMPRESSED routes
// reads through the decmpfs xattr and uncompressed_size carries the logical size
// (paired with APFS_INODE_HAS_UNCOMPRESSED_SIZE set in internal_flags).
void stampCompressedInodeFields(QByteArray* value, bool compressed, uint64_t uncompressedSize) {
    if (!compressed) {
        return;
    }
    writeLe32(value, kApfsInodeBsdFlagsOffset, kApfsInodeBsdCompressed);
    writeLe64(value, kApfsInodeUncompressedSizeOffset, uncompressedSize);
}

// The xfield layout an inode value carries beyond its fixed header: a NAME xfield
// always, a DSTREAM on an uncompressed regular file, and a SPARSE_BYTES xfield on a
// sparse one.
struct InodeXfieldParams {
    bool hasDstream{false};
    bool sparse{false};
    QByteArray nameBytes;
    qsizetype namePadded{0};
    qsizetype tocBytes{0};
    uint64_t sizeBytes{0};
    uint64_t allocedSizeBytes{0};
};

// Write the inode's extended-field blob: the xf header, the TOC entries (NAME +
// optional DSTREAM/SPARSE_BYTES), then the padded values. DSTREAM flags are
// XF_SYSTEM_FIELD (0x20) and SPARSE_BYTES flags XF_SYSTEM_FIELD|XF_CHILDREN_INHERIT;
// alloced_size is the block-aligned logical size (covers holes), and sparse_bytes is
// the unbacked (hole) byte count.
void writeInodeXfields(QByteArray* value, const InodeXfieldParams& x) {
    const qsizetype dstreamBytes = x.hasDstream ? kApfsDstreamMinBytes : 0;
    const qsizetype sparseFieldBytes = x.sparse ? 8 : 0;
    const qsizetype xfieldCount = 1 + (x.hasDstream ? 1 : 0) + (x.sparse ? 1 : 0);
    writeLe16(value, kApfsInodeXfieldsOffset, static_cast<uint16_t>(xfieldCount));
    writeLe16(value,
              kApfsInodeXfieldsOffset + kApfsXfieldDataBytesOffset,
              static_cast<uint16_t>(x.namePadded + dstreamBytes + sparseFieldBytes));
    qsizetype toc = kApfsInodeXfieldsOffset + kApfsXfieldHeaderBytes;
    (*value)[toc] = static_cast<char>(kApfsInodeNameField);
    (*value)[toc + 1] = 0x02;
    writeLe16(value, toc + kApfsXfieldSizeOffset, static_cast<uint16_t>(x.nameBytes.size()));
    if (x.hasDstream) {
        toc += kApfsXfieldTocEntryBytes;
        (*value)[toc] = static_cast<char>(kApfsInodeDstreamField);
        (*value)[toc + 1] = static_cast<char>(0x20);
        writeLe16(value, toc + kApfsXfieldSizeOffset, kApfsDstreamMinBytes);
    }
    if (x.sparse) {
        toc += kApfsXfieldTocEntryBytes;
        (*value)[toc] = static_cast<char>(kApfsInodeSparseBytesField);
        (*value)[toc + 1] = static_cast<char>(kApfsXfieldFlagsSparseBytes);
        writeLe16(value, toc + kApfsXfieldSizeOffset, 8);
    }
    const qsizetype dataStart = kApfsInodeXfieldsOffset + x.tocBytes;
    std::copy(x.nameBytes.cbegin(), x.nameBytes.cend(), value->begin() + dataStart);
    const uint64_t roundedSize =
        ((x.sizeBytes + kSupportedApfsBlockSizeBytes - 1) / kSupportedApfsBlockSizeBytes) *
        kSupportedApfsBlockSizeBytes;
    if (x.hasDstream) {
        const qsizetype dstream = dataStart + x.namePadded;
        writeLe64(value, dstream, x.sizeBytes);       // size (logical)
        writeLe64(value, dstream + 8, roundedSize);   // alloced_size (covers holes)
        writeLe64(value, dstream + 24, roundedSize);  // total_bytes_written
    }
    if (x.sparse) {
        writeLe64(value,
                  dataStart + x.namePadded + dstreamBytes,
                  roundedSize > x.allocedSizeBytes ? roundedSize - x.allocedSizeBytes : 0);
    }
}

QByteArray inodeValue(const ApfsInodeParams& params) {
    const auto& [parentId,
                 privateId,
                 mode,
                 name,
                 sizeBytes,
                 childOrLinkCount,
                 compressed,
                 uncompressedSize,
                 extraInternalFlags,
                 allocedSizeBytes] = params;
    // A compressed regular file carries the NAME xfield only (its bytes live in the
    // decmpfs xattr); an uncompressed regular file additionally carries a DSTREAM.
    const bool regularFile = (mode & kApfsModeRegularFile) == kApfsModeRegularFile;
    const bool hasDstream = regularFile && !compressed;
    // A sparse regular file carries an extra INO_EXT_TYPE_SPARSE_BYTES xfield.
    const bool sparse = hasDstream && (extraInternalFlags & kApfsInodeFlagIsSparse) != 0;
    const QByteArray nameBytes = name.toUtf8() + '\0';
    const qsizetype xfieldCount = 1 + (hasDstream ? 1 : 0) + (sparse ? 1 : 0);
    const qsizetype tocBytes = kApfsXfieldHeaderBytes + xfieldCount * kApfsXfieldTocEntryBytes;
    const qsizetype namePadded =
        ((nameBytes.size() + kApfsXfieldAlignmentPadding) / kApfsXfieldAlignment) *
        kApfsXfieldAlignment;
    const qsizetype dstreamBytes = hasDstream ? kApfsDstreamMinBytes : 0;
    const qsizetype sparseFieldBytes = sparse ? 8 : 0;
    const qsizetype valueBytes = kApfsInodeXfieldsOffset + tocBytes + namePadded + dstreamBytes +
                                 sparseFieldBytes;
    QByteArray value(valueBytes, '\0');
    writeLe64(&value, 0, parentId);
    writeLe64(&value, kApfsInodePrivateIdOffset, privateId);
    writeLe64(&value, 0x10, kApfsGeneratedTimestamp);
    writeLe64(&value, 0x18, kApfsGeneratedTimestamp);
    writeLe64(&value, 0x20, kApfsGeneratedTimestamp);
    writeLe64(&value, 0x28, kApfsGeneratedTimestamp);
    writeLe64(&value,
              kApfsInodeInternalFlagsOffset,
              inodeInternalFlags(compressed, extraInternalFlags));
    writeLe32(&value, 0x38, static_cast<uint32_t>(childOrLinkCount));
    const uint16_t permissions = (mode & 0777) ? 0 : (regularFile ? 0644 : 0755);
    writeLe16(&value, kApfsInodeModeOffset, mode | permissions);
    stampCompressedInodeFields(&value, compressed, uncompressedSize);
    writeInodeXfields(&value,
                      {.hasDstream = hasDstream,
                       .sparse = sparse,
                       .nameBytes = nameBytes,
                       .namePadded = namePadded,
                       .tocBytes = tocBytes,
                       .sizeBytes = sizeBytes,
                       .allocedSizeBytes = allocedSizeBytes});
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

// A7 (A-h) hard link: a dentry value carrying a single sibling-id extended field, so
// the name links to a sibling record. Layout = j_drec_val (file_id, date_added, flags)
// + xf_blob{num_exts=1, used_data=8} + x_field{type=SIBLING_ID, flags=0, size=8} + the
// 8-byte sibling id. xf_used_data is the value-bytes-only count (8), per Apple.
QByteArray directoryEntryValueWithSibling(uint64_t fileId, uint16_t entryType, uint64_t siblingId) {
    QByteArray value = directoryEntryValue(fileId, entryType);
    QByteArray xf(kApfsXfieldHeaderBytes + kApfsXfieldTocEntryBytes + 8, '\0');
    writeLe16(&xf, 0, 1);                   // xf_num_exts
    writeLe16(&xf, 2, 8);                   // xf_used_data (the 8 value bytes only)
    xf[kApfsXfieldHeaderBytes] = static_cast<char>(kApfsDrecExtTypeSiblingId);
    xf[kApfsXfieldHeaderBytes + 1] = 0x00;  // x_flags
    writeLe16(&xf, kApfsXfieldHeaderBytes + kApfsXfieldSizeOffset, 8);
    writeLe64(&xf, kApfsXfieldHeaderBytes + kApfsXfieldTocEntryBytes, siblingId);
    return value + xf;
}

// j_sibling_link_key: 8-byte header (inode id | SIBLING_LINK<<60) + the 8-byte sibling
// id. One per hard-link name; lists the inode's links.
QByteArray siblingLinkKey(uint64_t inodeId, uint64_t siblingId) {
    QByteArray key = fsKey(inodeId, kApfsRecordSiblingLink, kApfsFormattedRootInodeKeyBytes + 8);
    writeLe64(&key, kApfsFormattedRootInodeKeyBytes, siblingId);
    return key;
}

// j_sibling_val: the parent directory id + the NUL-terminated name (name_len counts the
// NUL, matching the dentry's name length).
QByteArray siblingLinkValue(uint64_t parentId, const QString& name) {
    const QByteArray nameBytes = name.toUtf8() + '\0';
    QByteArray value(8 + kUint16Size + nameBytes.size(), '\0');
    writeLe64(&value, 0, parentId);
    writeLe16(&value, 8, static_cast<uint16_t>(nameBytes.size()));
    std::copy(nameBytes.cbegin(), nameBytes.cend(), value.begin() + 8 + kUint16Size);
    return value;
}

// j_sibling_map_key: 8-byte header (sibling id | SIBLING_MAP<<60). j_sibling_map_val:
// the inode id the sibling maps back to.
QByteArray siblingMapKey(uint64_t siblingId) {
    return fsKey(siblingId, kApfsRecordSiblingMap, kApfsFormattedRootInodeKeyBytes);
}

QByteArray siblingMapValue(uint64_t inodeId) {
    QByteArray value(8, '\0');
    writeLe64(&value, 0, inodeId);
    return value;
}

// j_xattr_key: 8-byte key header (inode id | XATTR<<60), then le16 name_len (the
// NUL-terminated name's byte count) and the name bytes including the NUL.
QByteArray xattrKey(uint64_t inodeId, const QByteArray& name) {
    const QByteArray nameBytes = name + '\0';
    QByteArray key = fsKey(inodeId,
                           kApfsRecordXattr,
                           kApfsFormattedRootInodeKeyBytes + kUint16Size + nameBytes.size());
    writeLe16(&key, kApfsFormattedRootInodeKeyBytes, static_cast<uint16_t>(nameBytes.size()));
    std::copy(nameBytes.cbegin(),
              nameBytes.cend(),
              key.begin() + kApfsFormattedRootInodeKeyBytes + kUint16Size);
    return key;
}

// j_xattr_val for an embedded attribute: le16 flags + le16 xdata_len + xdata.
QByteArray xattrEmbeddedValue(uint16_t flags, const QByteArray& xdata) {
    QByteArray value(2 * kUint16Size + xdata.size(), '\0');
    writeLe16(&value, 0, flags);
    writeLe16(&value, kUint16Size, static_cast<uint16_t>(xdata.size()));
    std::copy(xdata.cbegin(), xdata.cend(), value.begin() + 2 * kUint16Size);
    return value;
}

QByteArray fileExtentKey(uint64_t privateId, uint64_t logicalByteOffset) {
    QByteArray key = fsKey(privateId, kApfsRecordFileExtent, kApfsFileExtentKeyBytes);
    writeLe64(&key, kApfsFileExtentKeyLogicalOffset, logicalByteOffset);
    return key;
}

QByteArray fileExtentValue(uint64_t lengthBytes, uint64_t dataStartBlock) {
    QByteArray value(kApfsFileExtentValueBytes, '\0');
    // Extent lengths cover whole blocks; the logical size lives in the
    // inode's dstream xfield.
    writeLe64(&value,
              0,
              ((lengthBytes + kSupportedApfsBlockSizeBytes - 1) / kSupportedApfsBlockSizeBytes) *
                  kSupportedApfsBlockSizeBytes);
    writeLe64(&value, kApfsFileExtentValuePhysicalBlockOffset, dataStartBlock);
    writeLe64(&value, kApfsFileExtentValueCryptoIdOffset, 0);
    return value;
}

// The data extents a file's records describe: an explicit multi-run list when
// present, otherwise the single contiguous run rounded from the payload size.
QVector<ApfsDataExtent> fileDataExtents(const ApfsRootFilePayload& file, uint32_t blockSize) {
    if (!file.dataExtents.isEmpty()) {
        return file.dataExtents;
    }
    const uint64_t bytes = static_cast<uint64_t>(file.data.size());
    if (bytes == 0 || blockSize == 0) {
        return {};
    }
    return {{0, file.dataStartBlock, (bytes + blockSize - 1) / blockSize}};
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

// A7 (A-h): the j_inode_val internal_flags a file's attributes require -- IS_SPARSE
// for a holed file, plus HAS_SECURITY_EA / HAS_FINDER_INFO when the matching
// well-known xattr is present (apfsck/fsck cross-check these against the xattr set).
uint64_t inodeAttributeFlags(const ApfsRootFilePayload& file) {
    uint64_t flags = file.sparse ? kApfsInodeFlagIsSparse : 0ULL;
    flags |= file.extraInodeFlags;
    for (const auto& [name, value] : file.xattrs) {
        if (name == QByteArray(kApfsXattrNameSecurity)) {
            flags |= kApfsInodeFlagHasSecurityEa;
        } else if (name == QByteArray(kApfsXattrNameFinderInfo)) {
            flags |= kApfsInodeFlagHasFinderInfo;
        }
    }
    return flags;
}

// Bytes a file actually allocates on disk: the sum of its data-extent blocks. For
// a sparse file this is less than the rounded logical size (the hole is unallocated).
uint64_t fileAllocedBytes(const ApfsRootFilePayload& file, uint32_t blockSize) {
    uint64_t blocks = 0;
    for (const ApfsDataExtent& extent : fileDataExtents(file, blockSize)) {
        blocks += extent.blockCount;
    }
    return blocks * blockSize;
}

// Emit one j_xattr record per arbitrary named attribute (ACL, Finder info, user
// xattrs); each value rides embedded in the record (XATTR_DATA_EMBEDDED).
void appendInodeXattrs(QVector<ApfsBtreeKeyValue>* records, const ApfsRootFilePayload& file) {
    for (const auto& [name, value] : file.xattrs) {
        records->append(
            {xattrKey(file.fileId, name), xattrEmbeddedValue(kApfsXattrDataEmbedded, value)});
    }
}

// A7 (A-h) hard links: emit the sibling-link + sibling-map records that track every
// name of a hard-linked inode, plus the extra dentries for the additional names. The
// primary name's dentry already carries its sibling-id xfield (written by the caller).
// A single-link file (no additionalLinks) emits nothing, staying byte-identical.
void appendHardLinkRecords(QVector<ApfsBtreeKeyValue>* records, const ApfsRootFilePayload& file) {
    if (file.additionalLinks.isEmpty()) {
        return;
    }
    records->append({siblingLinkKey(file.fileId, file.primarySiblingId),
                     siblingLinkValue(file.parentDirectoryId, file.fileName)});
    records->append({siblingMapKey(file.primarySiblingId), siblingMapValue(file.fileId)});
    for (const ApfsHardLinkName& link : file.additionalLinks) {
        records->append(
            {directoryEntryKey(link.parentId, link.name),
             directoryEntryValueWithSibling(file.fileId, kApfsDirTypeRegularFile, link.siblingId)});
        records->append({siblingLinkKey(file.fileId, link.siblingId),
                         siblingLinkValue(link.parentId, link.name)});
        records->append({siblingMapKey(link.siblingId), siblingMapValue(file.fileId)});
    }
}

void appendRootFileRecords(QVector<ApfsBtreeKeyValue>* records, const ApfsRootFilePayload& file) {
    const uint64_t logicalSize = file.sparse ? file.sparseLogicalSize
                                             : static_cast<uint64_t>(file.data.size());
    const int32_t linkCount = 1 + static_cast<int32_t>(file.additionalLinks.size());
    records->append(
        {fsKey(file.fileId, kApfsRecordInode),
         inodeValue({.parentId = file.parentDirectoryId,
                     .privateId = file.privateId,
                     .mode = kApfsModeRegularFile,
                     .name = file.fileName,
                     .sizeBytes = logicalSize,
                     .childOrLinkCount = linkCount,
                     .compressed = file.compressed,
                     .uncompressedSize = file.uncompressedSize,
                     .extraInternalFlags = inodeAttributeFlags(file),
                     .allocedSizeBytes =
                         file.sparse ? fileAllocedBytes(file, kSupportedApfsBlockSizeBytes) : 0})});
    records->append({directoryEntryKey(file.parentDirectoryId, file.fileName),
                     file.additionalLinks.isEmpty()
                         ? directoryEntryValue(file.fileId, kApfsDirTypeRegularFile)
                         : directoryEntryValueWithSibling(
                               file.fileId, kApfsDirTypeRegularFile, file.primarySiblingId)});
    appendHardLinkRecords(records, file);
    if (file.compressed) {
        // A transparently-compressed file has no data stream: its content lives
        // in an embedded com.apple.decmpfs xattr, and the inode is UF_COMPRESSED.
        records->append({xattrKey(file.fileId, QByteArray(kApfsXattrNameCompressed)),
                         xattrEmbeddedValue(kApfsXattrDataEmbedded, file.decmpfsXattr)});
        appendInodeXattrs(records, file);
        return;
    }
    records->append({fsKey(file.privateId, kApfsRecordDstreamId), dstreamIdValue()});
    // A zero-length file has a size-0 data stream and no allocated blocks, so it
    // carries no file-extent record; emitting one produces a zero-length extent
    // at logical address 0, which fsck_apfs rejects ("invalid zero-length
    // extent"). A fragmented file carries one record per contiguous run, keyed by
    // its file-logical byte offset.
    for (const ApfsDataExtent& extent : fileDataExtents(file, kSupportedApfsBlockSizeBytes)) {
        records->append(
            {fileExtentKey(file.privateId, extent.logicalBlock * kSupportedApfsBlockSizeBytes),
             fileExtentValue(extent.blockCount * kSupportedApfsBlockSizeBytes, extent.paddr)});
    }
    if (file.sparse) {
        // The trailing hole is an explicit file-extent with phys_block_num 0 so the
        // dstream's extents stay consecutive up to its logical size (apfsck requires
        // it, and the hole's length feeds the sparse-bytes accounting). No block is
        // allocated for a phys 0 extent.
        const uint64_t alloced = fileAllocedBytes(file, kSupportedApfsBlockSizeBytes);
        if (file.sparseLogicalSize > alloced) {
            records->append({fileExtentKey(file.privateId, alloced),
                             fileExtentValue(file.sparseLogicalSize - alloced, 0)});
        }
    }
    appendInodeXattrs(records, file);
}

void appendRootDirectoryRecords(QVector<ApfsBtreeKeyValue>* records,
                                const ApfsRootDirectoryPayload& directory,
                                int32_t childCount) {
    records->append({fsKey(directory.directoryId, kApfsRecordInode),
                     inodeValue({.parentId = kApfsRootDirectoryId,
                                 .privateId = directory.privateId,
                                 .mode = kApfsModeDirectory,
                                 .name = directory.directoryName,
                                 .childOrLinkCount = childCount})});
    records->append({directoryEntryKey(kApfsRootDirectoryId, directory.directoryName),
                     directoryEntryValue(directory.directoryId, kApfsDirTypeDirectory)});
}

qsizetype btreeRecordsByteSize(const QVector<ApfsBtreeKeyValue>& records) {
    qsizetype total = 0;
    for (const auto& record : records) {
        total += record.key.size() + record.value.size();
    }
    return total;
}

void sortFsTreeRecords(QVector<ApfsBtreeKeyValue>* records) {
    // File-system B-tree records must be stored in key order (object id, then
    // record type, then key tail) for Apple's binary-search lookups.
    std::stable_sort(records->begin(),
                     records->end(),
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
                         // Hard-link sibling-link records for one inode order by their
                         // 8-byte sibling id (the key tail after the object-id header).
                         if (leftType == kApfsRecordSiblingLink) {
                             return le64(left.key, kApfsFormattedRootInodeKeyBytes) <
                                    le64(right.key, kApfsFormattedRootInodeKeyBytes);
                         }
                         // Sibling extended attributes order by name (Apple compares
                         // the xattr name string, not the raw key bytes).
                         if (leftType == kApfsRecordXattr) {
                             const auto name = [](const QByteArray& key) {
                                 return key.mid(kApfsFormattedRootInodeKeyBytes + kUint16Size,
                                                le16(key, kApfsFormattedRootInodeKeyBytes));
                             };
                             return name(left.key) < name(right.key);
                         }
                         return left.key < right.key;
                     });
}

// Sorted file-system record set: the base volume entities (tree-root entity
// oid 1 dirents, root + private directory inodes) plus one record group per
// file/directory.
// Count the direct children a directory inode reports: files whose parent is that
// directory id. The root directory (id 2) additionally parents every top-level
// directory. A flat root (every file parented to root, no directories) reduces to
// files.size(), byte-identical to the certified single-level layout.
int32_t directChildCount(uint64_t directoryId,
                         const QVector<ApfsRootFilePayload>& files,
                         qsizetype extraSubdirectories) {
    int32_t count = static_cast<int32_t>(extraSubdirectories);
    for (const auto& file : files) {
        if (file.parentDirectoryId == directoryId) {
            ++count;
        }
        // A7 (A-h): each additional hard-link name is a separate dentry in its parent,
        // so it counts toward that directory's child count too.
        for (const ApfsHardLinkName& link : file.additionalLinks) {
            if (link.parentId == directoryId) {
                ++count;
            }
        }
    }
    return count;
}

// The default whole-volume crypto-state record an encrypted (ONEKEY) volume
// carries: APFS_TYPE_CRYPTO_STATE (7) at obj_id CRYPTO_SW_ID (4), j_crypto_val
// = refcnt 1 + an all-zero wrapped_crypto_state (key_len 0). The macOS kernel
// reads this on mount; without it a decrypted volume fails to mount (-69842).
// Byte-harvested from a real macOS FileVault volume.
ApfsBtreeKeyValue defaultVolumeCryptoStateRecord() {
    QByteArray key(8, '\0');
    writeLe64(&key,
              0,
              (static_cast<uint64_t>(kApfsRecordCryptoState) << kApfsObjTypeShift) |
                  kApfsCryptoSwId);
    QByteArray value(24, '\0');
    writeLe32(&value, 0, 1);  // refcnt = 1; remaining wrapped_crypto_state all zero
    return {key, value};
}

QVector<ApfsBtreeKeyValue> buildFsTreeRecords(const QVector<ApfsRootFilePayload>& files,
                                              const QVector<ApfsRootDirectoryPayload>& directories,
                                              bool includeCryptoState = false) {
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
                     .childOrLinkCount =
                         directChildCount(kApfsRootDirectoryId, files, directories.size())})},
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
        appendRootDirectoryRecords(&records,
                                   directory,
                                   directChildCount(directory.directoryId, files, 0));
    }
    if (includeCryptoState) {
        records.append(defaultVolumeCryptoStateRecord());
    }
    sortFsTreeRecords(&records);
    return records;
}

// Write the variable-kv body (TOC, keys growing up from the table, values
// growing down from valueAreaEnd, free-space fields) of a B-tree node. Returns
// false (with a blocker) if the records would overflow it. valueAreaEnd is
// blockSize - btree_info on a root node (which carries the info trailer) and
// blockSize on a non-root leaf.
bool writeFsTreeNodeBody(QByteArray* block,
                         const QVector<ApfsBtreeKeyValue>& records,
                         qsizetype valueAreaEnd,
                         QStringList* blockers) {
    writeLe32(block, kApfsBtreeNodeCountOffset, static_cast<uint32_t>(records.size()));
    const qsizetype tocLength =
        ((static_cast<qsizetype>(records.size()) * kApfsBtreeVariableTocEntryBytes + 63) / 64) * 64;
    writeLe16(block, kApfsBtreeNodeTableOffsetOffset, 0);
    writeLe16(block, kApfsBtreeNodeTableLengthOffset, static_cast<uint16_t>(tocLength));
    const qsizetype keyAreaStart = kApfsBtreeNodeHeaderBytes + tocLength;
    if (btreeRecordsByteSize(records) > valueAreaEnd - keyAreaStart) {
        blockers->append(
            QStringLiteral("APFS file-system B-tree node overflow (%1 records do not fit one node)")
                .arg(records.size()));
        return false;
    }
    qsizetype keyCursor = 0;
    qsizetype valueBackCursor = 0;
    for (qsizetype index = 0; index < records.size(); ++index) {
        const auto& record = records.at(index);
        valueBackCursor += record.value.size();
        const qsizetype toc = kApfsBtreeNodeHeaderBytes + index * kApfsBtreeVariableTocEntryBytes;
        writeLe16(block, toc, static_cast<uint16_t>(keyCursor));
        writeLe16(block,
                  toc + kApfsBtreeVariableTocKeyLengthOffset,
                  static_cast<uint16_t>(record.key.size()));
        writeLe16(block,
                  toc + kApfsBtreeVariableTocValueOffset,
                  static_cast<uint16_t>(valueBackCursor));
        writeLe16(block,
                  toc + kApfsBtreeVariableTocValueLengthOffset,
                  static_cast<uint16_t>(record.value.size()));
        std::copy(record.key.cbegin(),
                  record.key.cend(),
                  block->begin() + keyAreaStart + keyCursor);
        std::copy(record.value.cbegin(),
                  record.value.cend(),
                  block->begin() + valueAreaEnd - valueBackCursor);
        keyCursor += record.key.size();
    }
    writeLe16(block, 0x2C, static_cast<uint16_t>(keyCursor));
    writeLe16(block,
              0x2E,
              static_cast<uint16_t>(valueAreaEnd - keyAreaStart - keyCursor - valueBackCursor));
    writeLe16(block, 0x30, 0xFFFF);
    writeLe16(block, 0x32, 0);
    writeLe16(block, 0x34, 0xFFFF);
    writeLe16(block, 0x36, 0);
    return true;
}

// Longest key / value across a record set, for the root node's btree_info.
void writeFsTreeInfoTrailer(QByteArray* block,
                            const QVector<ApfsBtreeKeyValue>& records,
                            uint32_t blockSize,
                            uint64_t totalKeyCount,
                            uint64_t totalNodeCount) {
    writeBtreeInfo(
        block, {.blockSize = blockSize, .keyCount = totalKeyCount, .nodeCount = totalNodeCount});
    qsizetype longestKey = 0;
    qsizetype longestValue = 0;
    for (const auto& record : records) {
        longestKey = std::max(longestKey, record.key.size());
        longestValue = std::max(longestValue, record.value.size());
    }
    const qsizetype infoOffset = static_cast<qsizetype>(blockSize) - kApfsBtreeInfoBytes;
    writeLe32(block, infoOffset + kApfsBtreeInfoFlagsOffset, 0x00'00'00'42);
    writeLe32(block, infoOffset + 16, static_cast<uint32_t>(longestKey));
    writeLe32(block, infoOffset + 20, static_cast<uint32_t>(longestValue));
}

QByteArray buildRootTreeBlock(uint32_t blockSize,
                              const QVector<ApfsRootFilePayload>& files,
                              const QVector<ApfsRootDirectoryPayload>& directories,
                              QStringList* blockers,
                              bool includeCryptoState = false) {
    QByteArray block = newApfsObjectBlock(blockSize,
                                          kApfsFormatRootTreeOid,
                                          kApfsFormatXid,
                                          kApfsObjectTypeBtree,
                                          kApfsObjectSubtypeFsTree);
    writeLe16(&block, kApfsBtreeNodeFlagsOffset, kApfsBtreeNodeRoot | kApfsBtreeNodeLeaf);
    writeLe16(&block, kApfsBtreeNodeLevelOffset, 0);
    const QVector<ApfsBtreeKeyValue> records =
        buildFsTreeRecords(files, directories, includeCryptoState);
    const qsizetype valueAreaEnd = static_cast<qsizetype>(blockSize) - kApfsBtreeInfoBytes;
    if (!writeFsTreeNodeBody(&block, records, valueAreaEnd, blockers)) {
        blockers->append(
            QStringLiteral("APFS root file-system tree exceeds a single B-tree node (%1 records); "
                           "multi-node fs-trees are not yet supported")
                .arg(records.size()));
        return block;
    }
    writeFsTreeInfoTrailer(&block, records, blockSize, records.size(), 1);
    stampApfsObjectBlock(&block, blockers);
    return block;
}

QByteArray buildRootTreeBlock(uint32_t blockSize,
                              const QVector<ApfsRootFilePayload>& files,
                              QStringList* blockers) {
    return buildRootTreeBlock(blockSize, files, {}, blockers);
}

QByteArray buildEmptyRootTreeBlock(uint32_t blockSize,
                                   QStringList* blockers,
                                   bool includeCryptoState = false) {
    return buildRootTreeBlock(blockSize, {}, {}, blockers, includeCryptoState);
}

// Multi-volume (A4): an empty root file-system tree published under a non-default
// virtual OID. Each extra volume's object map resolves its own root-tree OID to this
// block, so the object's stored OID must equal that omap key. Re-stamp after patching
// the OID so the fletcher64 covers the new value.
QByteArray buildEmptyRootTreeBlockForVolume(uint32_t blockSize,
                                            uint64_t rootTreeOid,
                                            QStringList* blockers) {
    QByteArray block = buildEmptyRootTreeBlock(blockSize, blockers);
    writeLe64(&block, kApfsObjectOidOffset, rootTreeOid);
    stampApfsObjectBlock(&block, blockers);
    return block;
}

struct ApfsFsTreeNode {
    uint64_t oid{0};
    QByteArray block;
};

// Greedily pack the sorted records into leaf-node-sized groups (each group fits
// a full block with its growing TOC), mirroring a B-tree bulk load.
QVector<QVector<ApfsBtreeKeyValue>> distributeFsTreeRecordsIntoLeaves(
    const QVector<ApfsBtreeKeyValue>& records, uint32_t blockSize) {
    QVector<QVector<ApfsBtreeKeyValue>> leaves;
    QVector<ApfsBtreeKeyValue> current;
    qsizetype currentBytes = 0;
    for (const auto& record : records) {
        const qsizetype count = current.size() + 1;
        const qsizetype toc = ((count * kApfsBtreeVariableTocEntryBytes + 63) / 64) * 64;
        const qsizetype need = kApfsBtreeNodeHeaderBytes + toc + currentBytes + record.key.size() +
                               record.value.size();
        if (need > blockSize && !current.isEmpty()) {
            leaves.append(current);
            current.clear();
            currentBytes = 0;
        }
        current.append(record);
        currentBytes += record.key.size() + record.value.size();
    }
    if (!current.isEmpty()) {
        leaves.append(current);
    }
    return leaves;
}

struct ApfsFsTreeBuildInput {
    uint32_t blockSize{0};
    QVector<ApfsRootFilePayload> files;
    QVector<ApfsRootDirectoryPayload> directories;
    uint64_t firstLeafOid{0};  // consecutive leaf oids (Apple uses nx_next_oid)
};

QByteArray newFsTreeNode(uint32_t blockSize, uint64_t oid, uint16_t flags, uint16_t level) {
    // The B-tree root node carries object type BTREE; non-root nodes (the leaves
    // of a multi-level tree) carry BTREE_NODE - Apple's fsck_apfs rejects a leaf
    // typed as a root (it reads the one-bit 0x2-vs-0x3 difference as a header bit
    // flip in the fsroot tree).
    const uint32_t objectType = (flags & kApfsBtreeNodeRoot) ? kApfsObjectTypeBtree
                                                             : kApfsObjectTypeBtreeNode;
    QByteArray block =
        newApfsObjectBlock(blockSize, oid, kApfsFormatXid, objectType, kApfsObjectSubtypeFsTree);
    writeLe16(&block, kApfsBtreeNodeFlagsOffset, flags);
    writeLe16(&block, kApfsBtreeNodeLevelOffset, level);
    return block;
}

// Build the file-system tree as either a single ROOT|LEAF node or, when the
// records overflow one node, an internal root (level 1) over N leaf nodes
// (level 0, no info trailer) - the two-level shape apfs.kext produces. Leaf oids
// run consecutively from firstLeafOid; the root keeps oid 1028. nodes[0] is the
// root. Mirrors docs/APFS_A2_MULTI_LEAF_FSTREE_DESIGN.md.
bool buildFsTreeNodes(const ApfsFsTreeBuildInput& in,
                      QVector<ApfsFsTreeNode>* nodes,
                      QStringList* blockers) {
    const QVector<ApfsBtreeKeyValue> records = buildFsTreeRecords(in.files, in.directories);
    const qsizetype rootValueEnd = static_cast<qsizetype>(in.blockSize) - kApfsBtreeInfoBytes;
    QStringList probe;
    QByteArray single = newFsTreeNode(
        in.blockSize, kApfsFormatRootTreeOid, kApfsBtreeNodeRoot | kApfsBtreeNodeLeaf, 0);
    if (writeFsTreeNodeBody(&single, records, rootValueEnd, &probe)) {
        writeFsTreeInfoTrailer(&single, records, in.blockSize, records.size(), 1);
        stampApfsObjectBlock(&single, blockers);
        nodes->append({kApfsFormatRootTreeOid, single});
        return true;
    }
    const auto leaves = distributeFsTreeRecordsIntoLeaves(records, in.blockSize);
    QVector<ApfsBtreeKeyValue> rootRecords;
    QVector<ApfsFsTreeNode> leafNodes;
    for (qsizetype index = 0; index < leaves.size(); ++index) {
        const uint64_t leafOid = in.firstLeafOid + static_cast<uint64_t>(index);
        QByteArray leaf = newFsTreeNode(in.blockSize, leafOid, kApfsBtreeNodeLeaf, 0);
        if (!writeFsTreeNodeBody(&leaf, leaves.at(index), in.blockSize, blockers)) {
            return false;
        }
        stampApfsObjectBlock(&leaf, blockers);
        leafNodes.append({leafOid, leaf});
        QByteArray childOid(8, '\0');
        writeLe64(&childOid, 0, leafOid);
        rootRecords.append({leaves.at(index).first().key, childOid});
    }
    QByteArray root = newFsTreeNode(in.blockSize, kApfsFormatRootTreeOid, kApfsBtreeNodeRoot, 1);
    if (!writeFsTreeNodeBody(&root, rootRecords, rootValueEnd, blockers)) {
        blockers->append(QStringLiteral("APFS fs-tree needs more than two levels (%1 leaves); deep "
                                        "trees are not yet supported")
                             .arg(leaves.size()));
        return false;
    }
    // The root's btree_info longest-key/value must describe the whole tree (the
    // leaves' fs-records, not the root's 8-byte child-oid pointers), so fsck_apfs
    // sees the true maxima - measure from the full record set.
    writeFsTreeInfoTrailer(
        &root, records, in.blockSize, records.size(), 1 + static_cast<uint64_t>(leaves.size()));
    stampApfsObjectBlock(&root, blockers);
    nodes->append({kApfsFormatRootTreeOid, root});
    for (const auto& leafNode : leafNodes) {
        nodes->append(leafNode);
    }
    return true;
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
    // Multi-volume (A4): every volume superblock OID, written across the nx_fs_oid
    // array (slot i = volume i). Empty keeps the single-volume layout byte-identical
    // (only nx_fs_oid[0] = fsOid is written).
    QVector<uint64_t> fsOids;
    uint64_t nextOid{kApfsNxMinimumNextOid};
    // nx_keylocker (A6): physical range of the container keybag. 0 = unencrypted
    // container (the field stays zero, byte-identical to the prior layout).
    uint64_t keylockerPaddr{0};
    uint64_t keylockerBlocks{0};
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
    // nx_keylocker (prange): container keybag location for an encrypted container.
    if (position.keylockerPaddr != 0) {
        writeLe64(&block, kApfsNxKeylockerOffset, position.keylockerPaddr);
        writeLe64(&block, kApfsNxKeylockerOffset + 8, position.keylockerBlocks);
    }
    // nx_max_file_systems is the size-derived volume capacity; fsck_apfs/the Apple
    // reference require it to equal DIV_ROUND_UP(container_bytes, 512 MiB) exactly, so
    // multi-volume containers are sized large enough that this covers every volume
    // rather than overriding the field.
    writeLe32(&block, kApfsNxMaxFileSystemsOffset, apfsMaxFileSystems(blockCount, blockSize));
    if (position.fsOids.isEmpty()) {
        writeLe64(&block, kApfsNxFsOidArrayOffset, position.fsOid);
    } else {
        for (qsizetype i = 0; i < position.fsOids.size(); ++i) {
            writeLe64(&block,
                      kApfsNxFsOidArrayOffset + i * static_cast<qsizetype>(sizeof(uint64_t)),
                      position.fsOids.at(i));
        }
    }
    stampApfsObjectBlock(&block, blockers);
    return block;
}

struct ApfsCheckpointMapEntry {
    uint32_t type{0};
    uint32_t subtype{0};
    uint64_t oid{0};
    uint64_t paddr{0};
    uint32_t size{0};  // object byte size; 0 means a single block (cpm_size = blockSize)
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
        writeLe32(&block,
                  entry + kApfsCheckpointMapEntrySizeOffset,
                  mapping.size != 0 ? mapping.size : blockSize);
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

// One free-queue record: the ghost blocks at `paddr` (count `length`) scheduled
// for reclamation once transaction `xid` ages past the rollback window.
struct ApfsFreeQueueEntry {
    uint64_t xid{0};
    uint64_t paddr{0};
    uint64_t length{0};
};

QByteArray buildFreeQueueLeaf(uint32_t blockSize,
                              uint64_t oid,
                              uint64_t xid,
                              const QVector<ApfsFreeQueueEntry>& entries,
                              QStringList* blockers) {
    // Ephemeral free-queue B-tree root/leaf mirrored from newfs_apfs output:
    // node flags ROOT|LEAF|FIXED_KV_ALIGN (0x0007), 576-byte TOC, info flags
    // EPHEMERAL|ALLOWS_GHOSTS|SEQUENTIAL_INSERT (0x0E), 16-byte keys
    // {xid, paddr}, 8-byte count values, records sorted ascending by (xid, paddr).
    // A single-block run (length == 1) is stored as a ghost: its TOC value offset
    // is BTOFF_INVALID (0xFFFF) and no value byte is emitted, matching the kernel.
    // Genesis carries one record; the rotation/main free-queue grow it to the
    // rolling window of freed blocks awaiting deferred reclamation.
    const int nkeys = static_cast<int>(entries.size());
    int valueRecords = 0;
    for (const ApfsFreeQueueEntry& fqEntry : entries) {
        if (fqEntry.length != 1) {
            ++valueRecords;
        }
    }
    QByteArray block = newApfsObjectBlock(blockSize, oid, xid, kApfsObjectTypeBtreeEphemeral);
    writeLe32(&block, kApfsObjectSubtypeOffset, kApfsObjectSubtypeFreeQueue);
    writeLe16(&block, kApfsBtreeNodeFlagsOffset, 0x0007);
    writeLe32(&block, kApfsBtreeNodeCountOffset, static_cast<uint32_t>(nkeys));
    writeLe16(&block, kApfsBtreeNodeTableOffsetOffset, 0);
    writeLe16(&block, kApfsBtreeNodeTableLengthOffset, 576);
    const qsizetype infoOffset = static_cast<qsizetype>(blockSize) - kApfsBtreeInfoBytes;
    const qsizetype keyAreaStart = kApfsBtreeNodeHeaderBytes + 576;
    writeLe16(&block, 0x2C, static_cast<uint16_t>(nkeys * 16));
    writeLe16(&block,
              0x2E,
              static_cast<uint16_t>(infoOffset - keyAreaStart - nkeys * static_cast<qsizetype>(16) -
                                    valueRecords * static_cast<qsizetype>(8)));
    writeLe16(&block, 0x30, 0xFFFF);
    writeLe16(&block, 0x32, 0);
    writeLe16(&block, 0x34, 0xFFFF);
    writeLe16(&block, 0x36, 0);
    int valueSlot = 0;
    for (int i = 0; i < nkeys; ++i) {
        writeLe16(&block, kApfsBtreeNodeHeaderBytes + i * 4, static_cast<uint16_t>(i * 16));
        uint16_t valueOffset = 0xFFFF;
        if (entries[i].length != 1) {
            ++valueSlot;
            valueOffset = static_cast<uint16_t>(valueSlot * 8);
            writeLe64(&block, infoOffset - valueSlot * 8, entries[i].length);
        }
        writeLe16(&block, kApfsBtreeNodeHeaderBytes + i * 4 + 2, valueOffset);
        writeLe64(&block, keyAreaStart + i * 16, entries[i].xid);
        writeLe64(&block, keyAreaStart + i * 16 + 8, entries[i].paddr);
    }
    writeLe32(&block, infoOffset + kApfsBtreeInfoFlagsOffset, 0x00'00'00'0E);
    writeLe32(&block, infoOffset + kApfsBtreeInfoNodeSizeOffset, blockSize);
    writeLe32(&block, infoOffset + kApfsBtreeInfoKeySizeOffset, 16);
    writeLe32(&block, infoOffset + kApfsBtreeInfoValueSizeOffset, 8);
    writeLe32(&block, infoOffset + 16, 16);
    writeLe32(&block, infoOffset + 20, 8);
    writeLe64(&block, infoOffset + kApfsBtreeInfoKeyCountOffset, static_cast<uint64_t>(nkeys));
    writeLe64(&block, infoOffset + kApfsBtreeInfoNodeCountOffset, 1);
    stampApfsObjectBlock(&block, blockers);
    return block;
}

QByteArray buildFreeQueueTreeBlock(uint32_t blockSize,
                                   uint64_t oid,
                                   uint64_t pendingFreePaddr,
                                   uint64_t pendingFreeCount,
                                   QStringList* blockers) {
    return buildFreeQueueLeaf(blockSize,
                              oid,
                              kApfsFormatXid,
                              {{kApfsFormatXid, pendingFreePaddr, pendingFreeCount}},
                              blockers);
}

// Physical-extent-reference tree carrying one j_phys_ext record per file so
// fsck_apfs credits the file's data blocks during its space accounting.
// Key: physical_block_addr | (OBJ_TYPE_PHYSEXT << 60). Value: a 20-byte
// j_phys_ext_val { len_and_kind = block_count | (APFS_KIND_NEW << 60),
// owning_obj_id, refcnt }.
struct ExtentRefPhysRecord {
    uint64_t paddr{0};
    uint64_t blockCount{0};
    uint64_t owner{0};
    uint32_t refcnt{1};
};

// Collects one merged j_phys_ext record per distinct physical block across all
// files, paddr-sorted. A7 (A-h) clones: when two files (the source and its
// clone) reference the same physical block, emit ONE record with refcnt = the
// number of referencing data streams (fsck/apfsck cross-check e_refcnt against
// the file-extent references). The owner stays the lowest id (the source).
QVector<ExtentRefPhysRecord> collectExtentRefRecords(uint32_t blockSize,
                                                     const QVector<ApfsRootFilePayload>& files) {
    QVector<ExtentRefPhysRecord> records;
    for (const auto& file : files) {
        for (const ApfsDataExtent& extent : fileDataExtents(file, blockSize)) {
            ExtentRefPhysRecord* shared = nullptr;
            for (ExtentRefPhysRecord& candidate : records) {
                if (candidate.paddr == extent.paddr) {
                    shared = &candidate;
                    break;
                }
            }
            if (shared != nullptr) {
                shared->refcnt += 1;
                shared->owner = std::min(shared->owner, file.fileId);
            } else {
                records.append({extent.paddr, extent.blockCount, file.fileId, 1});
            }
        }
    }
    std::sort(records.begin(),
              records.end(),
              [](const ExtentRefPhysRecord& left, const ExtentRefPhysRecord& right) {
                  return left.paddr < right.paddr;
              });
    return records;
}

// Emits the per-record TOC entries plus key/value pairs for the extent-ref tree
// and returns the consumed key-area length (keyCursor) so the caller can finish
// the node-free/info trailer. Mutates valueBackCursor to track the value area.
qsizetype writeExtentRefRecords(QByteArray* block,
                                const QVector<ExtentRefPhysRecord>& records,
                                qsizetype keyAreaStart,
                                qsizetype valueAreaEnd,
                                qsizetype* valueBackCursor) {
    constexpr uint64_t kApfsObjTypePhysExt = 2;
    constexpr uint64_t kApfsKindNew = 1;
    constexpr qsizetype kExtRefKeyBytes = 8;
    constexpr qsizetype kExtRefValueBytes = 20;
    qsizetype keyCursor = 0;
    for (qsizetype index = 0; index < records.size(); ++index) {
        const auto& record = records.at(index);
        const qsizetype toc = kApfsBtreeNodeHeaderBytes + index * kApfsBtreeVariableTocEntryBytes;
        writeLe16(block, toc, static_cast<uint16_t>(keyCursor));
        writeLe16(block, toc + kApfsBtreeVariableTocKeyLengthOffset, kExtRefKeyBytes);
        *valueBackCursor += kExtRefValueBytes;
        writeLe16(block,
                  toc + kApfsBtreeVariableTocValueOffset,
                  static_cast<uint16_t>(*valueBackCursor));
        writeLe16(block, toc + kApfsBtreeVariableTocValueLengthOffset, kExtRefValueBytes);
        const qsizetype key = keyAreaStart + keyCursor;
        writeLe64(block, key, record.paddr | (kApfsObjTypePhysExt << kApfsObjTypeShift));
        const qsizetype value = valueAreaEnd - *valueBackCursor;
        writeLe64(block, value, record.blockCount | (kApfsKindNew << kApfsObjTypeShift));
        writeLe64(block, value + 8, record.owner);
        writeLe32(block, value + 16, record.refcnt);
        keyCursor += kExtRefKeyBytes;
    }
    return keyCursor;
}

QByteArray buildExtentRefTreeBlock(uint32_t blockSize,
                                   const QVector<ApfsRootFilePayload>& files,
                                   QStringList* blockers) {
    QByteArray block = newApfsObjectBlock(
        blockSize, kApfsFormatExtentRefTreeBlock, kApfsFormatXid, kApfsObjectTypeBtreePhysical);
    writeLe32(&block, kApfsObjectSubtypeOffset, kApfsObjectSubtypeExtentRef);
    writeLe16(&block, kApfsBtreeNodeFlagsOffset, 0x0003);

    const QVector<ExtentRefPhysRecord> records = collectExtentRefRecords(blockSize, files);

    constexpr qsizetype kExtRefKeyBytes = 8;
    constexpr qsizetype kExtRefValueBytes = 20;
    const qsizetype tocLength = std::max<qsizetype>(
        64, ((records.size() * kApfsBtreeVariableTocEntryBytes + 63) / 64) * 64);
    writeLe32(&block, kApfsBtreeNodeCountOffset, static_cast<uint32_t>(records.size()));
    writeLe16(&block, kApfsBtreeNodeTableOffsetOffset, 0);
    writeLe16(&block, kApfsBtreeNodeTableLengthOffset, static_cast<uint16_t>(tocLength));
    const qsizetype keyAreaStart = kApfsBtreeNodeHeaderBytes + tocLength;
    const qsizetype valueAreaEnd = static_cast<qsizetype>(blockSize) - kApfsBtreeInfoBytes;
    qsizetype valueBackCursor = 0;
    const qsizetype keyCursor =
        writeExtentRefRecords(&block, records, keyAreaStart, valueAreaEnd, &valueBackCursor);
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

// A3: one snapshot's entry in the volume omap-snapshot tree (a fixed-kv physical
// B-tree, subtype 0x13). Key = snapshot xid; value = omap_snapshot{flags, pad, oid}.
struct ApfsOmapSnapshotEntry {
    uint64_t xid{0};
    uint32_t flags{0};  // oms_flags (0 for a normal snapshot)
    uint64_t oid{0};    // oms_oid (0 on a real macOS sealed-system snapshot)
};

// Build the volume omap-snapshot tree root/leaf. Byte-matched against a real macOS
// sealed-system snapshot: subtype 0x13, ROOT|LEAF|FIXED_KV node flags, 576-byte TOC,
// info flags 0x10, 8-byte xid keys and 16-byte omap_snapshot values.
QByteArray buildOmapSnapshotTreeBlock(uint32_t blockSize,
                                      uint64_t paddrOid,
                                      uint64_t xid,
                                      const QVector<ApfsOmapSnapshotEntry>& entries,
                                      QStringList* blockers) {
    QByteArray block = newApfsObjectBlock(blockSize, paddrOid, xid, kApfsObjectTypeBtreePhysical);
    writeLe32(&block, kApfsObjectSubtypeOffset, kApfsObjectSubtypeOmapSnapshot);
    writeLe16(&block,
              kApfsBtreeNodeFlagsOffset,
              kApfsBtreeNodeRoot | kApfsBtreeNodeLeaf | kApfsBtreeNodeFixedKvSize);
    writeLe16(&block, kApfsBtreeNodeLevelOffset, 0);
    writeLe32(&block, kApfsBtreeNodeCountOffset, static_cast<uint32_t>(entries.size()));
    writeLe16(&block, kApfsBtreeNodeTableOffsetOffset, 0);
    writeLe16(&block,
              kApfsBtreeNodeTableLengthOffset,
              static_cast<uint16_t>(kApfsOmapSnapshotTreeTocBytes));

    const qsizetype tableStart = kApfsBtreeNodeHeaderBytes;
    const qsizetype keyAreaStart = tableStart + kApfsOmapSnapshotTreeTocBytes;
    const qsizetype valueAreaEnd = static_cast<qsizetype>(blockSize) - kApfsBtreeInfoBytes;
    const qsizetype keyBytesUsed = static_cast<qsizetype>(entries.size()) *
                                   kApfsOmapSnapshotKeyBytes;
    const qsizetype valueBytesUsed = static_cast<qsizetype>(entries.size()) *
                                     kApfsOmapSnapshotValueBytes;
    writeLe16(&block, 0x2C, static_cast<uint16_t>(keyBytesUsed));
    writeLe16(&block,
              0x2E,
              static_cast<uint16_t>(valueAreaEnd - keyAreaStart - keyBytesUsed - valueBytesUsed));
    writeLe16(&block, 0x30, 0xFFFF);
    writeLe16(&block, 0x32, 0);
    writeLe16(&block, 0x34, 0xFFFF);
    writeLe16(&block, 0x36, 0);
    for (qsizetype index = 0; index < entries.size(); ++index) {
        const qsizetype toc = tableStart + index * kApfsBtreeFixedTocEntryBytes;
        const qsizetype keyOffset = index * kApfsOmapSnapshotKeyBytes;
        const qsizetype valueBackOffset = (index + 1) * kApfsOmapSnapshotValueBytes;
        writeLe16(&block, toc, static_cast<uint16_t>(keyOffset));
        writeLe16(&block,
                  toc + kApfsBtreeFixedTocValueOffset,
                  static_cast<uint16_t>(valueBackOffset));
        const qsizetype key = keyAreaStart + keyOffset;
        const qsizetype value = valueAreaEnd - valueBackOffset;
        writeLe64(&block, key, entries.at(index).xid);
        writeLe32(&block, value, entries.at(index).flags);    // oms_flags
        writeLe32(&block, value + 4, 0);                      // oms_pad
        writeLe64(&block, value + 8, entries.at(index).oid);  // oms_oid
    }
    const qsizetype infoOffset = valueAreaEnd;
    writeLe32(&block, infoOffset + kApfsBtreeInfoFlagsOffset, 0x00'00'00'10);
    writeLe32(&block, infoOffset + kApfsBtreeInfoNodeSizeOffset, blockSize);
    writeLe32(&block,
              infoOffset + kApfsBtreeInfoKeySizeOffset,
              static_cast<uint32_t>(kApfsOmapSnapshotKeyBytes));
    writeLe32(&block,
              infoOffset + kApfsBtreeInfoValueSizeOffset,
              static_cast<uint32_t>(kApfsOmapSnapshotValueBytes));
    if (!entries.isEmpty()) {
        writeLe32(&block, infoOffset + 16, static_cast<uint32_t>(kApfsOmapSnapshotKeyBytes));
        writeLe32(&block, infoOffset + 20, static_cast<uint32_t>(kApfsOmapSnapshotValueBytes));
    }
    writeLe64(&block,
              infoOffset + kApfsBtreeInfoKeyCountOffset,
              static_cast<uint64_t>(entries.size()));
    writeLe64(&block, infoOffset + kApfsBtreeInfoNodeCountOffset, 1);
    stampApfsObjectBlock(&block, blockers);
    return block;
}

struct ApfsVariableKvRecord {
    QByteArray key;
    QByteArray value;
};

// Build a variable-kv physical B-tree root/leaf (info flags 0x52, kvloc_t TOC)
// holding the given {key, value} records in their given order. Generalises
// buildExtentRefTreeBlock for the snapshot-metadata tree (subtype 0x10), whose keys
// and values are variable length; longest-key/longest-value are tracked per record.
struct ApfsVariableKvTree {
    uint32_t blockSize{0};
    uint64_t paddrOid{0};
    uint64_t xid{0};
    uint32_t subtype{0};
    QVector<ApfsVariableKvRecord> records;
};

QByteArray buildVariableKvLeafBlock(const ApfsVariableKvTree& tree, QStringList* blockers) {
    const uint32_t blockSize = tree.blockSize;
    const QVector<ApfsVariableKvRecord>& records = tree.records;
    QByteArray block =
        newApfsObjectBlock(blockSize, tree.paddrOid, tree.xid, kApfsObjectTypeBtreePhysical);
    writeLe32(&block, kApfsObjectSubtypeOffset, tree.subtype);
    writeLe16(&block, kApfsBtreeNodeFlagsOffset, kApfsBtreeNodeRoot | kApfsBtreeNodeLeaf);
    const qsizetype tocLength = std::max<qsizetype>(
        64, ((records.size() * kApfsBtreeVariableTocEntryBytes + 63) / 64) * 64);
    writeLe32(&block, kApfsBtreeNodeCountOffset, static_cast<uint32_t>(records.size()));
    writeLe16(&block, kApfsBtreeNodeTableOffsetOffset, 0);
    writeLe16(&block, kApfsBtreeNodeTableLengthOffset, static_cast<uint16_t>(tocLength));
    const qsizetype keyAreaStart = kApfsBtreeNodeHeaderBytes + tocLength;
    const qsizetype valueAreaEnd = static_cast<qsizetype>(blockSize) - kApfsBtreeInfoBytes;
    qsizetype keyCursor = 0;
    qsizetype valueBackCursor = 0;
    uint32_t longestKey = 0;
    uint32_t longestValue = 0;
    for (qsizetype index = 0; index < records.size(); ++index) {
        const QByteArray& key = records.at(index).key;
        const QByteArray& value = records.at(index).value;
        const qsizetype toc = kApfsBtreeNodeHeaderBytes + index * kApfsBtreeVariableTocEntryBytes;
        writeLe16(&block, toc, static_cast<uint16_t>(keyCursor));
        writeLe16(&block,
                  toc + kApfsBtreeVariableTocKeyLengthOffset,
                  static_cast<uint16_t>(key.size()));
        valueBackCursor += value.size();
        writeLe16(&block,
                  toc + kApfsBtreeVariableTocValueOffset,
                  static_cast<uint16_t>(valueBackCursor));
        writeLe16(&block,
                  toc + kApfsBtreeVariableTocValueLengthOffset,
                  static_cast<uint16_t>(value.size()));
        std::copy(key.cbegin(), key.cend(), block.begin() + keyAreaStart + keyCursor);
        std::copy(value.cbegin(), value.cend(), block.begin() + valueAreaEnd - valueBackCursor);
        keyCursor += key.size();
        longestKey = std::max<uint32_t>(longestKey, static_cast<uint32_t>(key.size()));
        longestValue = std::max<uint32_t>(longestValue, static_cast<uint32_t>(value.size()));
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
        writeLe32(&block, infoOffset + 16, longestKey);
        writeLe32(&block, infoOffset + 20, longestValue);
    }
    writeLe64(&block,
              infoOffset + kApfsBtreeInfoKeyCountOffset,
              static_cast<uint64_t>(records.size()));
    writeLe64(&block, infoOffset + kApfsBtreeInfoNodeCountOffset, 1);
    stampApfsObjectBlock(&block, blockers);
    return block;
}

// A3: the metadata describing one snapshot, used to emit its snap-meta tree records.
struct ApfsSnapshotMetadata {
    uint64_t snapXid{0};
    uint64_t extentRefTreeOid{0};  // paddr of the snapshot's frozen extent-ref tree
    uint64_t sblockOid{0};         // paddr of the snapshot's frozen superblock copy
    uint64_t createTimeNs{0};
    uint64_t changeTimeNs{0};
    uint64_t inum{0};
    QByteArray name;  // UTF-8 snapshot name (no trailing NUL; added on emit)
};

// Build the two snap-meta tree records for one snapshot: j_snap_metadata (keyed by
// (SNAP_METADATA<<60)|xid) and j_snap_name (keyed by (SNAP_NAME<<60)|OBJ_ID_MASK +
// the name). Returned in ascending key-header order (metadata before name), the
// order the snapshot-metadata tree stores them.
QVector<ApfsVariableKvRecord> buildSnapMetaRecords(const ApfsSnapshotMetadata& snap) {
    const QByteArray nameZ = snap.name + QByteArray(1, '\0');  // NUL-terminated
    const uint16_t nameLen = static_cast<uint16_t>(nameZ.size());

    QByteArray metaKey(8, '\0');
    writeLe64(&metaKey, 0, (kApfsJObjTypeSnapMetadata << kApfsObjTypeShift) | snap.snapXid);
    QByteArray metaVal(kApfsSnapMetaValueHeaderBytes + nameZ.size(), '\0');
    writeLe64(&metaVal, kApfsSnapMetaExtentRefOidOffset, snap.extentRefTreeOid);
    writeLe64(&metaVal, kApfsSnapMetaSblockOidOffset, snap.sblockOid);
    writeLe64(&metaVal, kApfsSnapMetaCreateTimeOffset, snap.createTimeNs);
    writeLe64(&metaVal, kApfsSnapMetaChangeTimeOffset, snap.changeTimeNs);
    writeLe64(&metaVal, kApfsSnapMetaInumOffset, snap.inum);
    writeLe32(&metaVal, kApfsSnapMetaExtentRefTypeOffset, kApfsExtentRefTreeType);
    writeLe32(&metaVal, kApfsSnapMetaFlagsOffset, 0);
    writeLe16(&metaVal, kApfsSnapMetaNameLenOffset, nameLen);
    std::copy(nameZ.cbegin(), nameZ.cend(), metaVal.begin() + kApfsSnapMetaNameOffset);

    QByteArray nameKey(10 + nameZ.size(), '\0');
    writeLe64(&nameKey, 0, (kApfsJObjTypeSnapName << kApfsObjTypeShift) | kApfsJObjIdMask);
    writeLe16(&nameKey, 8, nameLen);
    std::copy(nameZ.cbegin(), nameZ.cend(), nameKey.begin() + 10);
    QByteArray nameVal(8, '\0');
    writeLe64(&nameVal, 0, snap.snapXid);

    QVector<ApfsVariableKvRecord> records;
    records.append({metaKey, metaVal});
    records.append({nameKey, nameVal});
    return records;
}

// A3: build a snapshot's frozen superblock copy from the live APSB. It freezes the
// tree pointers as the snapshot sees them: keeps root_tree_oid, zeros the live-only
// object-map / extent-ref / snap-meta tree oids (the snapshot resolves virtual oids
// through the volume omap filtered by its xid) and the revert fields, and is a
// PHYSICAL object (o_oid == its own paddr, o_type carries the physical flag).
QByteArray buildFrozenSnapshotSuperblock(const QByteArray& liveVolSb,
                                         uint64_t frozenPaddr,
                                         uint64_t snapXid,
                                         uint64_t newSnapshotCount,
                                         QStringList* blockers) {
    QByteArray block = liveVolSb;
    writeLe64(&block, kApfsObjectOidOffset, frozenPaddr);
    writeLe64(&block, kApfsObjectXidOffset, snapXid);
    writeLe32(&block, kApfsObjectTypeOffset, kApfsVolumePhysicalSblockType);
    writeLe64(&block, kApfsVolumeOmapOidOffset, 0);
    writeLe64(&block, kApfsVolumeExtentRefTreeOidOffset, 0);
    writeLe64(&block, kApfsVolumeSnapMetaTreeOidOffset, 0);
    writeLe64(&block, kApfsVolumeRevertToXidOffset, 0);
    writeLe64(&block, kApfsVolumeRevertToSblockOidOffset, 0);
    writeLe64(&block, kApfsVolumeNumSnapshotsOffset, newSnapshotCount);
    stampApfsObjectBlock(&block, blockers);
    return block;
}

QByteArray buildIpBitmapBlock(uint32_t blockSize, uint64_t usedBlocks) {
    // Raw internal-pool usage bitmap: marks the first `usedBlocks` IP-region
    // blocks used. The ghost checkpoint marks its own cib(s)+bitmap slot
    // (single-CIB: 2 blocks -> 0x3); the live checkpoint additionally marks the
    // ghost pair it still references via the IP free-queue (single-CIB: 4 ->
    // 0xF). Multi-CIB scales the slot to cib_count cibs + one chunk-0 bitmap, so
    // the run can span several bytes.
    QByteArray block(static_cast<qsizetype>(blockSize), '\0');
    for (uint64_t bit = 0; bit < usedBlocks; ++bit) {
        block[static_cast<qsizetype>(bit / 8)] |= static_cast<char>(1 << (bit % 8));
    }
    return block;
}

struct ApfsSpacemanParams {
    uint32_t blockSize;
    uint64_t blockCount;
    uint64_t reservedBlocks;
    uint64_t xid;
    bool genesis;
    // Live chunk-info-block addresses this checkpoint's spaceman publishes in its
    // cib-address array (offset 2568). Single-CIB containers pass one address;
    // multi-CIB (>126 chunks, cab_count 0) pass cib_count addresses. cibAddrs.size()
    // is always the device cib_count (used for ip sizing) even in the CAB tier.
    QVector<uint64_t> cibAddrs;
    // CAB tier (cib_count > 507): the device address array holds cab_count CAB block
    // numbers instead of the inline cib addresses; the cib addresses live inside the
    // CABs. cabAddrs is empty and cabCount 0 for every certified (cab_count 0) tier.
    uint64_t cabCount{0};
    QVector<uint64_t> cabAddrs;
    // Contiguous ghost IP blocks the live checkpoint holds in sm_fq[IP] (the genesis
    // rotation group: cib 0 + chunk-0 bitmap, plus the ghost cab 0 / ghost boundary
    // bitmap). 2 for the certified single-/multi-CIB tiers.
    uint64_t ghostIpFreeCount{2};
};

// Blocks each internal-pool allocation bitmap occupies: one 4096-byte block holds
// 32768 bits (one IP block each), so a large IP region needs several. mkapfs at
// 4 TB: ip_block_count 90138 -> 3 blocks.
uint64_t ipBitmapSizeBlocks(uint64_t chunkCount, uint64_t cibCount, uint64_t cabCount = 0) {
    const uint64_t ipBlockCount = 3 * (chunkCount + cibCount + cabCount);
    return std::max<uint64_t>(
        1, (ipBlockCount + kApfsSpacemanBlocksPerChunk - 1) / kApfsSpacemanBlocksPerChunk);
}

// First block of the internal pool: the ip-bitmap ring (16 slots of
// ip_bm_size_in_blocks blocks each) sits between ip_bm_base and ip_base, so a larger
// ip bitmap pushes ip_base out. Single-block bitmaps keep the certified 185.
uint64_t generatedIpBaseBlock(uint64_t chunkCount, uint64_t cibCount, uint64_t cabCount = 0) {
    return kApfsFormatIpBitmapBaseBlock +
           ipBitmapSizeBlocks(chunkCount, cibCount, cabCount) * kApfsSpacemanIpBmTxMultiplier;
}

// Apple's spaceman packs three variable-length inline arrays ahead of the
// cib-address array, each scaled by ip_bm_size, exactly as apfsprogs mkapfs does
// (off its base 0x150). Apple's base is 2520: the per-bitmap xid array (u64) at
// 2520, the active-bitmap index array (u16) next, then the 16*ip_bm_size free-next
// ring (u16), 8-byte aligned. With ip_bm_size == 1 this collapses to the certified
// 2520 / 2528 / 2536 / 2568 byte-for-byte.
constexpr qsizetype kApfsSpacemanBitmapXidOffset = 2520;

uint64_t spacemanBmAddrOffset(uint64_t ipBmSize) {
    return static_cast<uint64_t>(kApfsSpacemanBitmapXidOffset) + 8 * ipBmSize;
}
uint64_t spacemanFreeNextOffset(uint64_t ipBmSize) {
    const uint64_t ringBytes = 2 * ipBmSize;
    return spacemanBmAddrOffset(ipBmSize) + (ringBytes + 7) / 8 * 8;
}
uint64_t spacemanCibArrayOffset(uint64_t ipBmSize) {
    return spacemanFreeNextOffset(ipBmSize) + 16 * ipBmSize * 2;
}

// Number of blocks the spaceman ephemeral object spans. The inline cib/cab-address
// array sits at spacemanCibArrayOffset; when cib_count * 8 overflows the first block
// (the versioned layout overflows at ~181 cibs, i.e. the ~2.9 TiB metadata-overflow
// band), Apple/mkapfs grow the spaceman object into a second block rather than forcing
// a CAB (Apple forbids CAB for cib_count <= 507). At or below the overflow point this is
// 1, so every certified single-block tier stays byte-identical; cibs_per_cab (507)
// bounds it to 2 before the CAB tier takes over.
uint64_t spacemanBlockSpan(uint64_t blockSize, uint64_t ipBmSize, uint64_t addrEntries) {
    const uint64_t bytes = spacemanCibArrayOffset(ipBmSize) + addrEntries * 8;
    return (bytes + blockSize - 1) / blockSize;
}

// Placement of the genesis + live checkpoint-data ephemerals, accounting for a spaceman
// object that spills past one block. The genesis spaceman + reaper, then the live
// spaceman + reaper + the two free-queue trees, occupy contiguous checkpoint-data ring
// segments; a multi-block spaceman shifts everything after it by spacemanBlocks-1. For a
// single-block spaceman these are the certified constants (9/10/11/12/13/14, data
// index 2, len 4) so every existing tier stays byte-identical.
struct ApfsFormatEphemeralLayout {
    uint64_t spacemanBlocks{1};
    uint64_t genesisSpaceman{kApfsFormatGenesisSpacemanBlock};
    uint64_t genesisReaper{kApfsFormatGenesisReaperBlock};
    uint64_t liveSpaceman{kApfsFormatSpacemanBlock};
    uint64_t liveReaper{kApfsFormatReaperBlock};
    uint64_t fqIpTree{kApfsFormatFqIpTreeBlock};
    uint64_t fqMainTree{kApfsFormatFqMainTreeBlock};
    uint32_t liveDataIndex{2};
    uint32_t liveDataLen{4};
};

ApfsFormatEphemeralLayout apfsFormatEphemeralLayout(uint32_t blockSize,
                                                    uint64_t chunkCount,
                                                    uint64_t cibCount,
                                                    uint64_t cabCount) {
    const uint64_t ipBmSize = ipBitmapSizeBlocks(chunkCount, cibCount, cabCount);
    const uint64_t addrEntries = cabCount > 0 ? cabCount : cibCount;
    const uint64_t span = spacemanBlockSpan(blockSize, ipBmSize, addrEntries);
    ApfsFormatEphemeralLayout layout;
    layout.spacemanBlocks = span;
    layout.genesisSpaceman = kApfsFormatGenesisSpacemanBlock;
    layout.genesisReaper = layout.genesisSpaceman + span;
    layout.liveSpaceman = layout.genesisReaper + 1;
    layout.liveReaper = layout.liveSpaceman + span;
    layout.fqIpTree = layout.liveReaper + 1;
    layout.fqMainTree = layout.fqIpTree + 1;
    layout.liveDataIndex = static_cast<uint32_t>(span + 1);
    layout.liveDataLen = static_cast<uint32_t>(span + 3);
    return layout;
}

// Lay out the ip-bitmap free-next ring for a checkpoint: the in-use checkpoint's
// ip_bm_size blocks (slots 0 for genesis, ip_bm_size for live) are marked 0xFFFF;
// the rest form a linked free list (the genesis slots trail the live free list as
// the most-recently-freed copies). apfsck requires the used count == ip_bm_size.
void setupIpBitmapRing(QByteArray* block, uint64_t ipBmSize, bool genesis) {
    const uint64_t ring = spacemanFreeNextOffset(ipBmSize);
    const uint64_t bmapCount = 16 * ipBmSize;
    const auto set = [&](uint64_t i, uint16_t v) {
        writeLe16(block, static_cast<qsizetype>(ring + i * 2), v);
    };
    const uint64_t inUseStart = genesis ? 0 : ipBmSize;
    for (uint64_t i = 0; i < ipBmSize; ++i) {
        set(inUseStart + i, kApfsSpacemanIpBmRingInUse);
    }
    if (genesis) {
        writeLe16(block, kApfsSpacemanIpBmFreeHeadOffset, static_cast<uint16_t>(ipBmSize));
        writeLe16(block, kApfsSpacemanIpBmFreeTailOffset, static_cast<uint16_t>(bmapCount - 1));
        for (uint64_t i = ipBmSize; i < bmapCount - 1; ++i) {
            set(i, static_cast<uint16_t>(i + 1));
        }
        set(bmapCount - 1, kApfsSpacemanIpBmRingInUse);
        return;
    }
    // Live: free list runs 2*ip_bm_size .. bmapCount-1, then wraps through the
    // freed genesis slot 0 .. ip_bm_size-1 (the free tail).
    writeLe16(block, kApfsSpacemanIpBmFreeHeadOffset, static_cast<uint16_t>(2 * ipBmSize));
    writeLe16(block, kApfsSpacemanIpBmFreeTailOffset, static_cast<uint16_t>(ipBmSize - 1));
    for (uint64_t i = 2 * ipBmSize; i < bmapCount - 1; ++i) {
        set(i, static_cast<uint16_t>(i + 1));
    }
    set(bmapCount - 1, 0);
    for (uint64_t i = 0; i + 1 < ipBmSize; ++i) {
        set(i, static_cast<uint16_t>(i + 1));
    }
    set(ipBmSize - 1, kApfsSpacemanIpBmRingInUse);
}

// Bundles the sizing values both spaceman-block writers consume (block/chunk/cib/cab
// counts, free count, offsets, checkpoint xid, ip bitmap size, genesis flag) so each
// helper stays within the parameter-count gate. Same values the inline code computed.
struct ApfsSpacemanWriteParams {
    uint32_t blockSize;
    uint64_t xid;
    uint64_t blockCount;
    uint64_t chunkCount;
    uint64_t cibCount;
    uint64_t cabCount;
    uint64_t freeBlocks;
    uint64_t cibArrayOffset;
    uint64_t ipBmSize;
    bool genesis;
};

// Writes the spaceman object header plus the main-device descriptor (block/chunk/
// cib/cab counts, free count, cib-array offset). Byte-identical to the inline
// sequence it replaces.
void writeSpacemanHeaderAndMainDevice(QByteArray* block, const ApfsSpacemanWriteParams& p) {
    writeLe64(block, kApfsObjectOidOffset, kApfsFormatSpacemanOid);
    writeLe64(block, kApfsObjectXidOffset, p.xid);
    writeLe32(block, kApfsObjectTypeOffset, kApfsObjectTypeSpaceman);
    writeLe32(block, kApfsSpacemanBlockSizeOffset, p.blockSize);
    writeLe32(block, kApfsSpacemanBlocksPerChunkOffset, kApfsSpacemanBlocksPerChunk);
    writeLe32(block, kApfsSpacemanChunksPerCibOffset, kApfsSpacemanChunksPerCib);
    writeLe32(block, kApfsSpacemanCibsPerCabOffset, kApfsSpacemanCibsPerCab);
    writeLe64(block,
              kApfsSpacemanMainDeviceOffset + kApfsSpacemanDeviceBlockCountOffset,
              p.blockCount);
    writeLe64(block,
              kApfsSpacemanMainDeviceOffset + kApfsSpacemanDeviceChunkCountOffset,
              p.chunkCount);
    writeLe32(block,
              kApfsSpacemanMainDeviceOffset + kApfsSpacemanDeviceCibCountOffset,
              static_cast<uint32_t>(p.cibCount));
    writeLe32(block,
              kApfsSpacemanMainDeviceOffset + kApfsSpacemanDeviceCabCountOffset,
              static_cast<uint32_t>(p.cabCount));
    writeLe64(block,
              kApfsSpacemanMainDeviceOffset + kApfsSpacemanDeviceFreeCountOffset,
              p.freeBlocks);
    writeLe32(block,
              kApfsSpacemanMainDeviceOffset + kApfsSpacemanDeviceAddrOffsetOffset,
              static_cast<uint32_t>(p.cibArrayOffset));
}

// Writes the internal-pool sizing, tier2 cib-address offset, free-queue limits,
// internal-offset table, per-bitmap xid array, active-bitmap index array, and the
// ip bitmap ring. Byte-identical to the inline sequence it replaces.
void writeSpacemanInternalPoolAndBitmaps(QByteArray* block, const ApfsSpacemanWriteParams& p) {
    const uint64_t blockCount = p.blockCount;
    const uint64_t chunkCount = p.chunkCount;
    const uint64_t cibCount = p.cibCount;
    const uint64_t cabCount = p.cabCount;
    const uint64_t cibArrayOffset = p.cibArrayOffset;
    const uint64_t ipBmSize = p.ipBmSize;
    const uint64_t xid = p.xid;
    const bool genesis = p.genesis;
    writeLe32(block, kApfsSpacemanFlagsOffset, kApfsSpacemanFlagVersioned);
    writeLe32(block, kApfsSpacemanIpBmTxMultiplierOffset, kApfsSpacemanIpBmTxMultiplier);
    // Internal-pool size scales with the allocator structures apfsck/fsck_apfs
    // require: ip_block_count = 3 * (chunk_count + cib_count + cab_count). With
    // cab_count 0 (<=507 cibs) this is 3 * (chunk_count + cib_count). Validated
    // against Apple newfs_apfs output (1-chunk/1-cib 6; 8-chunk 27) and mkapfs
    // (128-chunk/2-cib -> 390).
    const uint64_t ipBlockCount = 3 * (chunkCount + cibCount + cabCount);
    writeLe64(block, kApfsSpacemanIpBlockCountOffset, ipBlockCount);
    // Each ip allocation bitmap is ip_bm_size blocks (one 4096-byte block per 32768
    // IP blocks); the 16-slot ring is ip_bm_size * 16 blocks and ip_base follows it.
    writeLe32(block, kApfsSpacemanIpBmSizeOffset, static_cast<uint32_t>(ipBmSize));
    writeLe32(block,
              kApfsSpacemanIpBmBlockCountOffset,
              static_cast<uint32_t>(ipBmSize * kApfsSpacemanIpBmTxMultiplier));
    writeLe64(block, kApfsSpacemanIpBmBaseOffset, kApfsFormatIpBitmapBaseBlock);
    writeLe64(block,
              kApfsSpacemanIpBaseOffset,
              generatedIpBaseBlock(chunkCount, cibCount, cabCount));
    // The secondary (tier2) device's cib-address offset sits immediately after the
    // main device's inline cib-address array. fsck_apfs rejects overlapping spaceman
    // structs, so this clears the whole main array (cib_count entries) past the
    // cib-array offset - single-CIB keeps the certified 2576.
    const uint64_t addrArrayEntries = cabCount > 0 ? cabCount : cibCount;
    writeLe32(block,
              kApfsSpacemanMainDeviceOffset + 0x30 + kApfsSpacemanDeviceAddrOffsetOffset,
              static_cast<uint32_t>(cibArrayOffset + addrArrayEntries * 8));
    writeLe16(block, kApfsSpacemanFqIpLimitOffset, ipFreeQueueNodeLimit(chunkCount));
    writeLe16(block, kApfsSpacemanFqMainLimitOffset, mainFreeQueueNodeLimit(blockCount));
    // Internal-offset table: sm_ip_bm_xid_offset / sm_ip_bitmap_offset /
    // sm_ip_bm_free_next_offset point at the three inline arrays, each scaled by
    // ip_bm_size (single-block bitmaps keep the certified 2520 / 2528 / 2536).
    const uint64_t bmAddrOffset = spacemanBmAddrOffset(ipBmSize);
    const uint64_t freeNextOffset = spacemanFreeNextOffset(ipBmSize);
    writeLe32(block, 0x144, static_cast<uint32_t>(kApfsSpacemanBitmapXidOffset));
    writeLe32(block, 0x148, static_cast<uint32_t>(bmAddrOffset));
    writeLe32(block, 0x14C, static_cast<uint32_t>(freeNextOffset));
    writeLe32(block, 0x150, 1);
    writeLe32(block, 0x154, 2520);
    // Per-bitmap xid array: one u64 per active bitmap block, all the checkpoint xid.
    for (uint64_t i = 0; i < ipBmSize; ++i) {
        writeLe64(block, kApfsSpacemanBitmapXidOffset + static_cast<qsizetype>(i) * 8, xid);
    }
    // Active-bitmap index array: the live checkpoint's ip_bm_size bitmap blocks sit
    // at ring slots 0..ip_bm_size-1 (genesis) or ip_bm_size..2*ip_bm_size-1 (rotated).
    // Each entry must be a distinct ring slot or apfsck reports "same bitmap twice".
    const uint64_t activeBase = genesis ? 0 : ipBmSize;
    for (uint64_t i = 0; i < ipBmSize; ++i) {
        writeLe16(block,
                  static_cast<qsizetype>(bmAddrOffset + i * 2),
                  static_cast<uint16_t>(activeBase + i));
    }
    setupIpBitmapRing(block, ipBmSize, genesis);
}

QByteArray buildSpacemanBlock(const ApfsSpacemanParams& params, QStringList* blockers) {
    const auto& [blockSize,
                 blockCount,
                 reservedBlocks,
                 xid,
                 genesis,
                 cibAddrs,
                 cabCount,
                 cabAddrs,
                 ghostIpFreeCount] = params;
    const uint64_t cibCount = static_cast<uint64_t>(cibAddrs.size());
    const uint64_t freeBlocks = blockCount > reservedBlocks ? blockCount - reservedBlocks : 0;
    const uint64_t chunkCount = (blockCount + kApfsSpacemanBlocksPerChunk - 1) /
                                kApfsSpacemanBlocksPerChunk;
    const uint64_t ipBmSize = ipBitmapSizeBlocks(chunkCount, cibCount, cabCount);
    const uint64_t cibArrayOffset = spacemanCibArrayOffset(ipBmSize);
    // The spaceman is one block until its inline cib/cab-address array overflows, then it
    // spans a second block (the dead-zone fix); the object header lives in block 0 and the
    // fletcher64 covers the whole object.
    const uint64_t addrEntries = cabCount > 0 ? cabCount : cibCount;
    const uint64_t spacemanBlocks = spacemanBlockSpan(blockSize, ipBmSize, addrEntries);
    QByteArray block(static_cast<qsizetype>(blockSize) * static_cast<qsizetype>(spacemanBlocks),
                     '\0');
    const ApfsSpacemanWriteParams smWrite{.blockSize = blockSize,
                                          .xid = xid,
                                          .blockCount = blockCount,
                                          .chunkCount = chunkCount,
                                          .cibCount = cibCount,
                                          .cabCount = cabCount,
                                          .freeBlocks = freeBlocks,
                                          .cibArrayOffset = cibArrayOffset,
                                          .ipBmSize = ipBmSize,
                                          .genesis = genesis};
    writeSpacemanHeaderAndMainDevice(&block, smWrite);
    writeSpacemanInternalPoolAndBitmaps(&block, smWrite);
    if (!genesis) {
        // sm_fq[IP] holds the genesis rotation group pending reclamation: cib 0 +
        // chunk-0 bitmap, plus the ghost cab 0 (CAB tier) / ghost boundary bitmap
        // (overflow tier). Only the group rotates, so ghostIpFreeCount is the group
        // stride (2..4) regardless of cib_count - the immutable cibs are never freed.
        writeLe64(&block, 0xC8, ghostIpFreeCount);
        writeLe64(&block, 0xD0, kApfsFormatFqIpTreeOid);
        writeLe64(&block, 0xD8, kApfsFormatXid);
        writeLe64(&block, 0xF0, 2);
        writeLe64(&block, 0xF8, kApfsFormatFqMainTreeOid);
        writeLe64(&block, 0x100, kApfsFormatXid);
    }
    // CAB tier publishes cab_count CAB block numbers here; every certified cab_count-0
    // tier publishes cib_count inline CIB block numbers (byte-identical to before).
    const QVector<uint64_t>& addrArray = cabCount > 0 ? cabAddrs : cibAddrs;
    for (qsizetype i = 0; i < addrArray.size(); ++i) {
        writeLe64(&block, static_cast<qsizetype>(cibArrayOffset) + i * 8, addrArray.at(i));
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
    // The chunk range [chunkStart, chunkStart + chunkSpan) this chunk-info block
    // covers. Single-CIB containers pass the whole device (start 0, span =
    // chunk_count); multi-CIB split the device across cib_count blocks of up to
    // chunks_per_cib (126) chunks each. cibIndex is the block's position in the
    // spaceman cib-address array (apfsck checks cib_index matches).
    uint64_t chunkStart;
    uint64_t chunkSpan;
    uint64_t cibIndex;
    // Metadata-overflow: how many chunks the reserved prefix (reservedBlocks)
    // spans, and the immutable per-chunk bitmaps for chunks 1..M-1 (chunk 0 uses
    // bitmapBlock). For the common case M == 1 (metadata fits chunk 0) these are
    // {1, empty} and the entry below reduces to the certified single-chunk-0 form.
    uint64_t metadataChunks{1};
    QVector<uint64_t> extraChunkBitmaps;
    // Multi-chunk overflow tier (CAB included): the boundary chunk (M-1) takes this
    // rotating bitmap (ghost for the genesis cib, live for the live cib) instead of an
    // immutable extraChunkBitmaps entry. 0 below that tier.
    uint64_t boundaryBitmap{0};
};

// Blocks of chunk `chunkIndex` consumed by a reserved prefix of `reservedBlocks`
// blocks (clamped to the 32768-block chunk so a multi-chunk prefix never overruns
// a single bitmap block).
uint64_t chunkReservedBlocks(uint64_t reservedBlocks, uint64_t chunkIndex) {
    const uint64_t start = chunkIndex * kApfsSpacemanBlocksPerChunk;
    if (reservedBlocks <= start) {
        return 0;
    }
    return std::min<uint64_t>(reservedBlocks - start, kApfsSpacemanBlocksPerChunk);
}

// The allocation bitmap a metadata chunk's chunk-info entry references: chunk 0's
// rotating bitmap, the boundary chunk's rotating bitmap (any multi-chunk tier), or an
// immutable chunk 1..M-2 bitmap.
uint64_t chunkEntryBitmap(uint64_t chunk,
                          bool boundary,
                          uint64_t bitmapBlock,
                          uint64_t boundaryBitmap,
                          const QVector<uint64_t>& extraChunkBitmaps) {
    if (chunk == 0) {
        return bitmapBlock;
    }
    if (boundary && boundaryBitmap != 0) {
        return boundaryBitmap;
    }
    return extraChunkBitmaps.value(static_cast<qsizetype>(chunk) - 1);
}

QByteArray buildChunkInfoBlock(const ApfsChunkInfoParams& params, QStringList* blockers) {
    const auto [blockSize,
                blockCount,
                reservedBlocks,
                xid,
                selfBlock,
                bitmapBlock,
                chunkStart,
                chunkSpan,
                cibIndex,
                metadataChunks,
                extraChunkBitmaps,
                boundaryBitmap] = params;
    // apfsck requires a cib's object xid to equal the newest transaction id among
    // its chunks (a cib only advances when one of its chunks changes). The reserved
    // prefix changes genesis->live only in its boundary chunk (metadataChunks - 1,
    // = chunk 0 when the prefix fits chunk 0); a cib without that chunk is
    // all-genesis, so its object xid stays at the genesis transaction.
    const uint64_t boundaryChunk = metadataChunks - 1;
    const uint64_t cibObjXid = chunkStart == 0 ? xid : kApfsFormatGenesisXid;
    QByteArray block =
        newApfsObjectBlock(blockSize, selfBlock, cibObjXid, kApfsObjectTypeChunkInfoBlock);
    writeLe32(&block, kApfsChunkInfoIndexOffset, static_cast<uint32_t>(cibIndex));
    writeLe32(&block, kApfsChunkInfoCountOffset, static_cast<uint32_t>(chunkSpan));
    for (uint64_t index = 0; index < chunkSpan; ++index) {
        const uint64_t chunk = chunkStart + index;
        const qsizetype entry = kApfsChunkInfoEntriesOffset +
                                static_cast<qsizetype>(index) * kApfsChunkInfoEntryStride;
        const uint64_t chunkAddr = chunk * kApfsSpacemanBlocksPerChunk;
        const uint64_t chunkBlocks = qMin<uint64_t>(kApfsSpacemanBlocksPerChunk,
                                                    blockCount - chunkAddr);
        // Chunks 0..M-1 hold the reserved metadata prefix, each with a bitmap
        // (chunk 0's rotating bitmap, chunks 1..M-1 immutable); later chunks are
        // fully free (bitmap_addr 0, genesis xid).
        const bool metadataChunk = chunkStart == 0 && chunk < metadataChunks;
        const uint64_t usedBlocks = metadataChunk ? chunkReservedBlocks(reservedBlocks, chunk) : 0;
        const bool boundary = chunkStart == 0 && chunk == boundaryChunk;
        const uint64_t entryBitmap =
            metadataChunk
                ? chunkEntryBitmap(chunk, boundary, bitmapBlock, boundaryBitmap, extraChunkBitmaps)
                : 0;
        writeLe64(&block, entry, boundary ? xid : kApfsFormatGenesisXid);
        writeLe64(&block, entry + kApfsChunkInfoEntryAddrOffset, chunkAddr);
        writeLe32(&block,
                  entry + kApfsChunkInfoEntryBlockCountOffset,
                  static_cast<uint32_t>(chunkBlocks));
        writeLe32(&block,
                  entry + kApfsChunkInfoEntryFreeCountOffset,
                  static_cast<uint32_t>(chunkBlocks - usedBlocks));
        writeLe64(&block, entry + kApfsChunkInfoEntryBitmapAddrOffset, entryBitmap);
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

struct ApfsCibAddrParams {
    uint32_t blockSize;
    uint64_t selfBlock;
    uint64_t xid;
    uint32_t cabIndex;
    QVector<uint64_t> cibAddrs;  // cib block numbers (ghost or live variant for cib 0)
};

// apfs_cib_addr_block (SPACEMAN_CAB): the CAB tier's chunk-info ADDRESS block. It
// carries this CAB's index and up to cibs_per_cab (507) cib block numbers, letting
// the spaceman address cib_count > 507 cibs through a two-level table.
QByteArray buildCibAddrBlock(const ApfsCibAddrParams& params, QStringList* blockers) {
    const auto& [blockSize, selfBlock, xid, cabIndex, cibAddrs] = params;
    QByteArray block = newApfsObjectBlock(blockSize, selfBlock, xid, kApfsObjectTypeCibAddrBlock);
    writeLe32(&block, kApfsCibAddrIndexOffset, cabIndex);
    writeLe32(&block, kApfsCibAddrCibCountOffset, static_cast<uint32_t>(cibAddrs.size()));
    for (qsizetype i = 0; i < cibAddrs.size(); ++i) {
        writeLe64(&block, kApfsCibAddrEntriesOffset + i * 8, cibAddrs.at(i));
    }
    stampApfsObjectBlock(&block, blockers);
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
    // Multi-volume (A4): the virtual OID this volume superblock is published under
    // in the container object map + nx_fs_oid array, its slot index in that array
    // (apfs_fs_index), and the virtual OID its own object map resolves the root
    // file-system tree to. The first volume keeps the certified single-volume OIDs.
    uint64_t volumeOid{kApfsFormatVolumeOid};
    uint32_t fsIndex{0};
    uint64_t rootTreeOid{kApfsFormatRootTreeOid};
    // apfs_fs_flags (A6): 1 = APFS_FS_UNENCRYPTED (default); 0x8 = APFS_FS_ONEKEY
    // for a software-encrypted whole-volume-key (FileVault) volume.
    uint64_t fsFlags{1};
};

QByteArray buildVolumeSuperblock(const ApfsVolumeSuperblockFields& fields, QStringList* blockers) {
    QByteArray block =
        newApfsObjectBlock(fields.blockSize, fields.volumeOid, kApfsFormatXid, kApfsObjectTypeFs);
    writeLe32(&block, kApfsObjectMagicOffset, kApfsMagicApsb);
    // apfs_fs_index: this volume's slot in the container's nx_fs_oid array.
    writeLe32(&block, 0x24, fields.fsIndex);
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
    writeLe64(&block, kApfsVolumeFsFlagsOffset, fields.fsFlags);
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
    writeLe64(&block, kApfsVolumeRootTreeOidOffset, fields.rootTreeOid);
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
                                                      kApfsObjectTypeChunkInfoBlock,
                                                      kApfsObjectTypeCibAddrBlock};
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

// Walk the live checkpoint-map to the ephemeral spaceman and read its first
// cib_addr (offset 2568) - the chunk-info block the live checkpoint uses, i.e.
// the slot a crash-safe commit rotates away from (pairs with nextIpSlot).
// Returns 0 if no spaceman is found.
uint64_t readLiveSpacemanCibAddr(QIODevice* image,
                                 const ApfsRepairGeometry& geometry,
                                 const QByteArray& nxsb,
                                 QStringList* blockers) {
    const ApfsLiveCheckpoint live = readLiveCheckpoint(nxsb);
    QByteArray checkpointMap(geometry.blockSize, '\0');
    if (!readApfsRepairBlock(
            image, geometry, live.descBase + live.descIndex, &checkpointMap, blockers)) {
        return 0;
    }
    const uint32_t count = le32(checkpointMap, kApfsCheckpointMapCountOffset);
    for (uint32_t index = 0; index < count; ++index) {
        const qsizetype entry = kApfsCheckpointMapEntriesOffset +
                                static_cast<qsizetype>(index) * kApfsCheckpointMapEntryBytes;
        const uint64_t paddr = le64(checkpointMap, entry + kApfsCheckpointMapEntryPaddrOffset);
        QByteArray object(geometry.blockSize, '\0');
        if (!readApfsRepairBlock(image, geometry, paddr, &object, blockers)) {
            return 0;
        }
        if (le32(object, kApfsObjectTypeOffset) == kApfsObjectTypeSpaceman) {
            // The cib-address array offset scales with ip_bm_size (overflow tier);
            // single-block bitmaps keep the certified 2568.
            const uint64_t ipBmSize = le32(object, kApfsSpacemanIpBmSizeOffset);
            return le64(object, static_cast<qsizetype>(spacemanCibArrayOffset(ipBmSize)));
        }
    }
    return 0;
}

struct ApfsCheckpointCommitContext {
    QIODevice* image{nullptr};
    ApfsRepairGeometry geometry;
    uint64_t dataBase{0};
    uint64_t newXid{0};
    uint64_t newDataIndex{0};
    int64_t spacemanFreeDelta{0};  // applied to the re-emitted spaceman free count
    uint64_t newCibAddr{0};        // re-point the spaceman cib_addr 0 (crash-safe rotation)
    uint64_t cibCount{1};          // cib-address array length (multi-CIB > 1)
    uint64_t ipBitmapUsage{0};     // 0 = leave the sm_ip_bitmap ring; else advance it
    QVector<ApfsFreeQueueEntry> ipFqEntries;    // non-empty: rebuild the IP free-queue + counts
    QVector<ApfsFreeQueueEntry> mainFqEntries;  // non-empty: rebuild the main free-queue + counts
    // A7 (A-g) in-chunk grow: blocks added to the main device's sm_block_count (0 = no
    // resize). The matching free-count rise rides in spacemanFreeDelta.
    int64_t spacemanBlockCountDelta{0};
};

// Advance the sm_ip_bitmap ring in lockstep with the cib rotation (decoded from
// the kernel-advanced harvest, docs/APFS_A2_CRASH_SAFETY_DESIGN.md): allocate the
// next free ring index for this checkpoint's IP-usage bitmap, mark it in-use,
// release the index that ages out into the free-list, and advance the head/tail/
// xid/seq. Writes the new live ip-bitmap block (the two active cib/bitmap slots).
// Without this the ring stays at genesis while the cib rotates, and the kernel's
// continuation draws an internal-pool overallocation warning.
bool advanceIpBitmapRing(QByteArray* spaceman,
                         const ApfsCheckpointCommitContext& ctx,
                         QStringList* blockers) {
    const uint64_t base = le64(*spaceman, kApfsSpacemanIpBmBaseOffset);
    const uint64_t bmSize = le32(*spaceman, kApfsSpacemanIpBmSizeOffset);
    const uint64_t bmCount = le32(*spaceman, kApfsSpacemanIpBmBlockCountOffset);
    const qsizetype xidOff = kApfsSpacemanBitmapXidOffset;
    const qsizetype bmAddrOff = static_cast<qsizetype>(spacemanBmAddrOffset(bmSize));
    const qsizetype ringOff = static_cast<qsizetype>(spacemanFreeNextOffset(bmSize));
    const auto ringNext = [&](uint16_t i) {
        return le16(*spaceman, ringOff + i * 2);
    };
    const auto setRing = [&](uint16_t i, uint16_t v) {
        writeLe16(spaceman, ringOff + i * 2, v);
    };
    // Allocate bmSize fresh slots off the free-list head for the new ip-bitmap copy,
    // marking each in-use; the old active slots (the live bitmap copy) are released
    // back onto the free-list tail. With bmSize == 1 this is byte-identical to the
    // certified single-block ring advance.
    QVector<uint16_t> newSlots;
    uint16_t head = le16(*spaceman, kApfsSpacemanIpBmFreeHeadOffset);
    for (uint64_t i = 0; i < bmSize; ++i) {
        newSlots.append(head);
        const uint16_t next = ringNext(head);
        setRing(head, kApfsSpacemanIpBmRingInUse);
        head = next;
    }
    uint16_t tail = le16(*spaceman, kApfsSpacemanIpBmFreeTailOffset);
    for (uint64_t i = 0; i < bmSize; ++i) {
        const uint16_t oldActive = le16(*spaceman, bmAddrOff + static_cast<qsizetype>(i) * 2);
        setRing(tail, oldActive);
        tail = oldActive;
    }
    setRing(tail, kApfsSpacemanIpBmRingInUse);
    writeLe16(spaceman, kApfsSpacemanIpBmFreeHeadOffset, head);
    writeLe16(spaceman, kApfsSpacemanIpBmFreeTailOffset, tail);
    for (uint64_t i = 0; i < bmSize; ++i) {
        writeLe16(spaceman, bmAddrOff + static_cast<qsizetype>(i) * 2, newSlots.at(i));
        writeLe64(spaceman, xidOff + static_cast<qsizetype>(i) * 8, ctx.newXid);
    }
    // The whole ip usage (ipBitmapUsage contiguous blocks) fits the first bitmap
    // block; the remaining bmSize-1 copies are all-zero.
    for (uint64_t i = 0; i < bmSize; ++i) {
        const QByteArray bmp =
            i == 0 ? buildIpBitmapBlock(ctx.geometry.blockSize, ctx.ipBitmapUsage)
                   : QByteArray(static_cast<qsizetype>(ctx.geometry.blockSize), '\0');
        if (!writeApfsRepairBlock(ctx.image, ctx.geometry, base + newSlots.at(i), bmp, blockers)) {
            return false;
        }
    }
    if (bmCount < bmSize) {
        blockers->append(QStringLiteral("APFS commit: ip-bitmap ring smaller than ip_bm_size"));
        return false;
    }
    return true;
}

// Apply this commit's mutations to the re-emitted spaceman: the net free-count
// delta, the rotated cib_addr, the IP free-queue counts (sfq_count + oldest_xid
// from the rolling window), and the sm_ip_bitmap ring advance.
bool applySpacemanCommitMutations(const ApfsCheckpointCommitContext& ctx,
                                  QByteArray* object,
                                  QStringList* blockers) {
    if (ctx.spacemanFreeDelta != 0) {
        const qsizetype freeOffset = kApfsSpacemanMainDeviceOffset +
                                     kApfsSpacemanDeviceFreeCountOffset;
        writeLe64(object,
                  freeOffset,
                  le64(*object, freeOffset) + static_cast<uint64_t>(ctx.spacemanFreeDelta));
    }
    if (ctx.spacemanBlockCountDelta != 0) {
        // A7 (A-g) in-chunk grow: the main device's block count rises by the blocks
        // the device gained (the chunk count is unchanged - the grow stays in chunk 0).
        const qsizetype countOffset = kApfsSpacemanMainDeviceOffset +
                                      kApfsSpacemanDeviceBlockCountOffset;
        const uint64_t grownBlockCount = le64(*object, countOffset) +
                                         static_cast<uint64_t>(ctx.spacemanBlockCountDelta);
        writeLe64(object, countOffset, grownBlockCount);
        // The main free-queue's node limit scales with the device block count, so it has
        // to be recomputed for the new size (fsck checks it).
        writeLe16(object, kApfsSpacemanFqMainLimitOffset, mainFreeQueueNodeLimit(grownBlockCount));
    }
    if (ctx.newCibAddr != 0) {
        // Only group slot 0 rotates, so just re-point spaceman address-array entry 0
        // (the live cib 0, or the live cab 0 on the CAB tier); the rest of the array
        // (immutable cibs/cabs) carries through from the previous checkpoint's copy.
        const uint64_t ipBmSize = le32(*object, kApfsSpacemanIpBmSizeOffset);
        writeLe64(object, static_cast<qsizetype>(spacemanCibArrayOffset(ipBmSize)), ctx.newCibAddr);
    }
    if (!ctx.ipFqEntries.isEmpty()) {
        uint64_t pendingBlocks = 0;
        for (const ApfsFreeQueueEntry& fqEntry : ctx.ipFqEntries) {
            pendingBlocks += fqEntry.length;
        }
        writeLe64(object, kApfsSpacemanFqIpCountOffset, pendingBlocks);
        writeLe64(object, kApfsSpacemanFqIpOldestXidOffset, ctx.ipFqEntries.first().xid);
    }
    if (!ctx.mainFqEntries.isEmpty()) {
        uint64_t pendingBlocks = 0;
        for (const ApfsFreeQueueEntry& fqEntry : ctx.mainFqEntries) {
            pendingBlocks += fqEntry.length;
        }
        writeLe64(object, kApfsSpacemanFqMainCountOffset, pendingBlocks);
        writeLe64(object, kApfsSpacemanFqMainOldestXidOffset, ctx.mainFqEntries.first().xid);
    }
    return ctx.ipBitmapUsage == 0 || advanceIpBitmapRing(object, ctx, blockers);
}

bool isFreeQueueTreeWithOid(const QByteArray& object, uint64_t oid) {
    return le32(object, kApfsObjectSubtypeOffset) == kApfsObjectSubtypeFreeQueue &&
           le64(object, kApfsObjectOidOffset) == oid;
}

// Copy each live ephemeral object to the next data-ring slot, re-stamped at the
// new transaction id, and rewrite the checkpoint-map paddrs to the new slots.
// The spaceman gains this commit's free/cib/free-queue mutations; the IP
// free-queue tree is rebuilt with the rolling freed-slot window.
// Block span of one checkpoint-map ephemeral object (cpm_size / block_size). The
// spaceman spills past one block in the metadata-overflow dead zone; every other
// ephemeral is a single block.
uint64_t checkpointEntryBlockSpan(const QByteArray& map, qsizetype entry, uint32_t blockSize) {
    const uint32_t cpmSize = le32(map, entry + kApfsCheckpointMapEntrySizeOffset);
    const uint32_t bytes = cpmSize != 0 ? cpmSize : blockSize;
    return (bytes + blockSize - 1) / blockSize;
}

// Total block span of every ephemeral in the checkpoint (the data-ring length the
// checkpoint consumes, which exceeds the entry count once the spaceman is multi-block).
uint64_t checkpointDataBlockSpan(const QByteArray& map, uint32_t blockSize) {
    const uint32_t count = le32(map, kApfsCheckpointMapCountOffset);
    uint64_t total = 0;
    for (uint32_t index = 0; index < count; ++index) {
        total += checkpointEntryBlockSpan(map,
                                          kApfsCheckpointMapEntriesOffset +
                                              static_cast<qsizetype>(index) *
                                                  kApfsCheckpointMapEntryBytes,
                                          blockSize);
    }
    return total;
}

// Read a (possibly multi-block) ephemeral object contiguously from paddr.
bool readEphemeralObject(const ApfsCheckpointCommitContext& ctx,
                         uint64_t paddr,
                         uint64_t span,
                         QByteArray* object,
                         QStringList* blockers) {
    object->clear();
    for (uint64_t block = 0; block < span; ++block) {
        QByteArray slice(ctx.geometry.blockSize, '\0');
        if (!readApfsRepairBlock(ctx.image, ctx.geometry, paddr + block, &slice, blockers)) {
            return false;
        }
        object->append(slice);
    }
    return true;
}

// Apply this commit's mutations to one re-emitted ephemeral object: restamp the xid,
// fold the spaceman's free/cib/free-queue changes, and rebuild either free-queue tree.
bool mutateEphemeralObject(const ApfsCheckpointCommitContext& ctx,
                           QByteArray* object,
                           QStringList* blockers) {
    writeLe64(object, kApfsObjectXidOffset, ctx.newXid);
    if (le32(*object, kApfsObjectTypeOffset) == kApfsObjectTypeSpaceman) {
        return applySpacemanCommitMutations(ctx, object, blockers);
    }
    if (!ctx.ipFqEntries.isEmpty() && isFreeQueueTreeWithOid(*object, kApfsFormatFqIpTreeOid)) {
        *object = buildFreeQueueLeaf(
            ctx.geometry.blockSize, kApfsFormatFqIpTreeOid, ctx.newXid, ctx.ipFqEntries, blockers);
    } else if (!ctx.mainFqEntries.isEmpty() &&
               isFreeQueueTreeWithOid(*object, kApfsFormatFqMainTreeOid)) {
        *object = buildFreeQueueLeaf(ctx.geometry.blockSize,
                                     kApfsFormatFqMainTreeOid,
                                     ctx.newXid,
                                     ctx.mainFqEntries,
                                     blockers);
    }
    return true;
}

bool reemitCheckpointEphemerals(const ApfsCheckpointCommitContext& ctx,
                                QByteArray* checkpointMap,
                                QStringList* blockers) {
    const uint32_t count = le32(*checkpointMap, kApfsCheckpointMapCountOffset);
    uint64_t dataOffset = ctx.newDataIndex;
    for (uint32_t index = 0; index < count; ++index) {
        const qsizetype entry = kApfsCheckpointMapEntriesOffset +
                                static_cast<qsizetype>(index) * kApfsCheckpointMapEntryBytes;
        const uint64_t oldPaddr = le64(*checkpointMap, entry + kApfsCheckpointMapEntryPaddrOffset);
        const uint64_t span =
            checkpointEntryBlockSpan(*checkpointMap, entry, ctx.geometry.blockSize);
        const uint64_t newPaddr = ctx.dataBase + dataOffset;
        QByteArray object;
        if (!readEphemeralObject(ctx, oldPaddr, span, &object, blockers) ||
            !mutateEphemeralObject(ctx, &object, blockers) ||
            !stampAndWriteApfsBlock(ctx.image, ctx.geometry, newPaddr, &object, blockers)) {
            return false;
        }
        writeLe64(checkpointMap, entry + kApfsCheckpointMapEntryPaddrOffset, newPaddr);
        dataOffset += span;
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
    int64_t spacemanFreeDelta{0};  // net free-block change (negative when blocks are consumed)
    uint64_t nextOidAdvance{0};    // nx_next_oid increment (fs-tree leaf oids consumed)
    uint64_t newCibAddr{0};        // re-point the spaceman cib_addr 0 (crash-safe rotation)
    uint64_t cibCount{1};          // cib-address array length (multi-CIB > 1)
    uint64_t ipSlotStride{2};      // IP rotation-slot size (cib_count + 1); fq run length
    uint64_t ipBitmapUsage{0};     // sm_ip_bitmap ring advance usage count (0 = leave)
    uint64_t freedCibSlot{0};      // cib slot freed this commit -> IP free-queue (0 = no update)
    uint64_t prevFreedCibSlot{0};  // cib slot freed the previous commit (the window's older entry)
    QVector<uint64_t> extraFreedIpBlocks;  // overflow: old boundary-chunk bitmap freed this commit
    QVector<ApfsFreeQueueEntry> mainFqEntries;  // rebuilt main free-queue (empty = leave it)
    // A7 (A-g) in-chunk grow: blocks added to nx_block_count + the spaceman main device
    // (0 = no resize). Applied to the carried-forward nx_superblock + spaceman.
    int64_t blockCountDelta{0};
};

// Shared checkpoint-advance engine: read the live checkpoint-map, re-emit the
// ephemeral object set into the next data-ring slot, write a fresh
// checkpoint-map + nx_superblock into the next descriptor-ring slot (optionally
// re-pointing nx_omap_oid at a freshly COW'd container object map), and
// re-anchor block 0. Mirrors the Apple apfs.kext commit decoded in
// docs/APFS_A2_INPLACE_COMMIT_GROUND_TRUTH.md.
// The rolling two-deep IP free-queue window: the slot freed the previous commit (or
// the genesis seed at the first commit) plus what this commit freed - the cib-0
// rotation slot (ip_slot_stride ghost blocks) and, on the overflow tier, the
// boundary chunk's old bitmap block. Sorted ascending by (xid, paddr) for the tree.
QVector<ApfsFreeQueueEntry> buildIpFreeQueueWindow(const ApfsCheckpointAdvanceRequest& request,
                                                   uint64_t newXid) {
    QVector<ApfsFreeQueueEntry> entries;
    if (request.freedCibSlot == 0) {
        return entries;
    }
    entries = {{newXid - 1, request.prevFreedCibSlot, request.ipSlotStride},
               {newXid, request.freedCibSlot, request.ipSlotStride}};
    for (uint64_t freedIpBlock : request.extraFreedIpBlocks) {
        entries.append({newXid, freedIpBlock, 1});
    }
    std::sort(entries.begin(),
              entries.end(),
              [](const ApfsFreeQueueEntry& a, const ApfsFreeQueueEntry& b) {
                  return a.xid != b.xid ? a.xid < b.xid : a.paddr < b.paddr;
              });
    return entries;
}

// Advances the in-memory nxsb to the new checkpoint and applies the optional
// block-count grow, container-omap, and next-oid deltas. Byte-identical to the
// inline sequence it replaces.
void applyNxSuperblockCheckpointDeltas(ApfsCheckpointAdvanceRequest& request,
                                       const ApfsLiveCheckpoint& live,
                                       uint64_t newXid,
                                       uint32_t ephemeralBlocks,
                                       const ApfsRepairGeometry& geometry) {
    advanceNxSuperblockCheckpoint(&request.nxsb, live, newXid, ephemeralBlocks);
    if (request.blockCountDelta != 0) {
        // A7 (A-g) in-chunk grow: the container gained blocks within the existing chunk.
        const uint64_t grownBlockCount = le64(request.nxsb, kApfsNxBlockCountOffset) +
                                         static_cast<uint64_t>(request.blockCountDelta);
        writeLe64(&request.nxsb, kApfsNxBlockCountOffset, grownBlockCount);
        // nx_ephemeral_info[0] encodes main_fq_node_limit(block_count) for a sub-128-MiB
        // container, so it has to track the new block count (fsck checks it exactly).
        writeLe64(&request.nxsb,
                  kApfsNxEphemeralInfoOffset,
                  apfsNxEphemeralInfoValue(grownBlockCount, geometry.blockSize));
    }
    if (request.newContainerOmap != 0) {
        writeLe64(&request.nxsb, kApfsNxOmapOidOffset, request.newContainerOmap);
    }
    if (request.nextOidAdvance != 0) {
        writeLe64(&request.nxsb,
                  kApfsNxNextOidOffset,
                  le64(request.nxsb, kApfsNxNextOidOffset) + request.nextOidAdvance);
    }
}

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
    // The data-ring length is the total block span of the ephemerals, which exceeds the
    // entry count once the spaceman is multi-block (the metadata-overflow dead zone).
    const uint64_t ephemeralBlocks = checkpointDataBlockSpan(checkpointMap, geometry.blockSize);
    if (live.dataNext + ephemeralBlocks > live.dataBlocks) {
        blockers->append(
            QStringLiteral("APFS in-place commit: checkpoint data ring would wrap (unsupported in "
                           "this increment)"));
        return false;
    }
    const uint64_t newXid = live.xid + 1;
    const uint64_t cpmBlock = live.descBase + live.descNext;
    const uint64_t nxsbBlock = cpmBlock + 1;
    const QVector<ApfsFreeQueueEntry> ipFqEntries = buildIpFreeQueueWindow(request, newXid);
    const ApfsCheckpointCommitContext ctx{request.image,
                                          geometry,
                                          live.dataBase,
                                          newXid,
                                          live.dataNext,
                                          request.spacemanFreeDelta,
                                          request.newCibAddr,
                                          request.cibCount,
                                          request.ipBitmapUsage,
                                          ipFqEntries,
                                          request.mainFqEntries,
                                          request.blockCountDelta};
    if (!reemitCheckpointEphemerals(ctx, &checkpointMap, blockers)) {
        return false;
    }
    writeLe64(&checkpointMap, kApfsObjectOidOffset, cpmBlock);
    writeLe64(&checkpointMap, kApfsObjectXidOffset, newXid);
    if (!stampAndWriteApfsBlock(request.image, geometry, cpmBlock, &checkpointMap, blockers)) {
        return false;
    }
    applyNxSuperblockCheckpointDeltas(
        request, live, newXid, static_cast<uint32_t>(ephemeralBlocks), geometry);
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

// Parse every {oid, xid, paddr, flags} record from a single-leaf object-map
// B-tree node. The container object map holds one such record per volume
// superblock, so a multi-volume container (A4/A8) yields one entry per volume.
QVector<ApfsObjectMapEntry> readOmapLeafEntries(const QByteArray& omapTreeNode,
                                                uint32_t blockSize) {
    QVector<ApfsObjectMapEntry> entries;
    const uint32_t nkeys = le32(omapTreeNode, kApfsBtreeNodeCountOffset);
    const qsizetype keyAreaStart = kApfsBtreeNodeHeaderBytes + 448;
    const qsizetype valueAreaEnd = static_cast<qsizetype>(blockSize) - kApfsBtreeInfoBytes;
    for (uint32_t index = 0; index < nkeys; ++index) {
        const qsizetype key = keyAreaStart + static_cast<qsizetype>(index) * kApfsObjectMapKeyBytes;
        const qsizetype value = valueAreaEnd -
                                (static_cast<qsizetype>(index) + 1) * kApfsObjectMapValueBytes;
        entries.append({le64(omapTreeNode, key),
                        le64(omapTreeNode, key + kApfsOmapKeyXidOffset),
                        le64(omapTreeNode, value + kApfsOmapValuePaddrOffset),
                        le32(omapTreeNode, value)});
    }
    return entries;
}

// Rebuild the container object-map tree leaf after a COW commit: the mutated
// volume (kApfsFormatVolumeOid) maps to its new superblock at the new xid, while
// every other volume's record (multi-volume containers, A4/A8) is preserved
// verbatim so its superblock stays reachable. Records sort by (oid, xid).
QByteArray buildContainerOmapTreeBlock(uint32_t blockSize,
                                       uint64_t treeOid,
                                       const ApfsObjectMapEntry& mutatedVolume,
                                       const QVector<ApfsObjectMapEntry>& preservedVolumes,
                                       QStringList* blockers) {
    QVector<ApfsObjectMapEntry> mappings = preservedVolumes;
    mappings.append(mutatedVolume);
    std::sort(mappings.begin(),
              mappings.end(),
              [](const ApfsObjectMapEntry& a, const ApfsObjectMapEntry& b) {
                  return a.oid != b.oid ? a.oid < b.oid : a.xid < b.xid;
              });
    return buildObjectMapTreeBlock(blockSize, treeOid, mappings, mutatedVolume.xid, blockers);
}

struct ApfsLiveFsChain {
    uint64_t ctrOmapHdr{0};
    uint64_t ctrOmapTree{0};
    uint64_t volSb{0};
    uint64_t volOmapHdr{0};
    uint64_t volOmapTree{0};
    uint64_t rootTree{0};
    uint64_t extentRef{0};
    // Multi-volume (A4/A8): container-omap records for volumes OTHER than the
    // mutated target (kApfsFormatVolumeOid), preserved verbatim across the COW
    // commit so their superblocks are not orphaned. Empty for single-volume.
    QVector<ApfsObjectMapEntry> containerOmapOthers;
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
    // Multi-volume (A4/A8): COW targets kApfsFormatVolumeOid; record every other
    // volume's container-omap entry so the commit re-publishes them unchanged.
    chain->containerOmapOthers.clear();
    for (const ApfsObjectMapEntry& entry : readOmapLeafEntries(node, geometry.blockSize)) {
        if (entry.oid == kApfsFormatVolumeOid) {
            chain->volSb = entry.physicalBlock;
        } else {
            chain->containerOmapOthers.append(entry);
        }
    }
    if (!readApfsRepairBlock(image, geometry, chain->volSb, &node, blockers)) {
        return false;
    }
    chain->volOmapHdr = le64(node, kApfsVolumeOmapOidOffset);
    chain->extentRef = le64(node, kApfsVolumeExtentRefTreeOidOffset);
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

// Map each owning file id to its data extent's start block by reading the
// extent-ref tree (the inverse of buildExtentRefTreeBlock), so a chained commit
// can reproduce the existing files' extents without moving their data.
QHash<uint64_t, uint64_t> parseExtentRefOwners(QIODevice* image,
                                               const ApfsRepairGeometry& geometry,
                                               uint64_t extentRefBlock,
                                               QStringList* blockers) {
    QHash<uint64_t, uint64_t> owners;
    QByteArray node(geometry.blockSize, '\0');
    if (extentRefBlock == 0 ||
        !readApfsRepairBlock(image, geometry, extentRefBlock, &node, blockers)) {
        return owners;
    }
    const uint32_t nkeys = le32(node, kApfsBtreeNodeCountOffset);
    const qsizetype tocLength =
        std::max<qsizetype>(64, ((nkeys * kApfsBtreeVariableTocEntryBytes + 63) / 64) * 64);
    const qsizetype keyAreaStart = kApfsBtreeNodeHeaderBytes + tocLength;
    const qsizetype valueAreaEnd = static_cast<qsizetype>(geometry.blockSize) - kApfsBtreeInfoBytes;
    const uint64_t paddrMask = (1ULL << kApfsObjTypeShift) - 1;
    for (uint32_t index = 0; index < nkeys; ++index) {
        const qsizetype toc = kApfsBtreeNodeHeaderBytes +
                              static_cast<qsizetype>(index) * kApfsBtreeVariableTocEntryBytes;
        const uint16_t keyOffset = le16(node, toc);
        const uint16_t valueOffset = le16(node, toc + kApfsBtreeVariableTocValueOffset);
        const uint64_t paddr = le64(node, keyAreaStart + keyOffset) & paddrMask;
        owners.insert(le64(node, valueAreaEnd - valueOffset + 8), paddr);
    }
    return owners;
}

// Number of chunk-info blocks a device of `chunkCount` chunks needs: one cib per
// chunks_per_cib (126) chunks. A container of <=126 chunks is single-CIB.
uint64_t cibCountForChunks(uint64_t chunkCount) {
    if (chunkCount == 0) {
        return 1;
    }
    return (chunkCount + kApfsSpacemanChunksPerCib - 1) / kApfsSpacemanChunksPerCib;
}

// Number of chunk-info ADDRESS blocks (CABs) a device of `cibCount` cibs needs: one
// CAB per cibs_per_cab (507) cibs, but only once the inline cib-address array would
// overflow the spaceman block (cib_count > 507). At or below 507 cibs the spaceman
// lists cib addresses inline and cab_count is 0 (the certified tiers).
uint64_t cabCountForCibs(uint64_t cibCount) {
    return cibCount > kApfsSpacemanCibsPerCab
               ? (cibCount + kApfsSpacemanCibsPerCab - 1) / kApfsSpacemanCibsPerCab
               : 0;
}

// Checkpoint-data ephemeral layout derived from a container's block count (the form the
// post-format validator has, which only carries block size + count).
ApfsFormatEphemeralLayout apfsFormatEphemeralLayoutForBlockCount(uint32_t blockSize,
                                                                 uint64_t blockCount) {
    const uint64_t chunkCount = (blockCount + kApfsSpacemanBlocksPerChunk - 1) /
                                kApfsSpacemanBlocksPerChunk;
    const uint64_t cibCount = cibCountForChunks(chunkCount);
    const uint64_t cabCount = cabCountForCibs(cibCount);
    return apfsFormatEphemeralLayout(blockSize, chunkCount, cibCount, cabCount);
}

// Internal-pool placement matching how the macOS kernel rotates a multi-CIB
// container's chunk-info blocks (harvested 2026-06-15): the kernel rotates ONLY
// cib 0 (chunk 0's allocation cib) and keeps cib 1..N-1 IMMUTABLE - those describe
// the upper, all-free chunks and never change. So the IP region (ip_base = 185) is
//   [cib 1 .. cib N-1] (N-1 immutable blocks, one each)
//   then cib 0's three crash-safe rotation slots {cib0, chunk-0 bitmap}:
//   genesis (xid 1), live (xid 2), and a spare the first commit rotates into.
// This keeps the IP usage contiguous from ip_base and reduces to the certified
// single-CIB {185,186}/{187,188}/{189,190} layout when cib_count is 1.
struct ApfsMultiCibLayout {
    uint64_t cibCount{1};
    uint64_t ipBase{kApfsFormatIpBaseBlock};
    QVector<uint64_t> immutableCibs;            // cib 1..N-1, shared by every checkpoint
    uint64_t cib0Base{kApfsFormatIpBaseBlock};  // first cib-0 rotation slot
    uint64_t ghostBitmap{0};                    // genesis chunk-0 allocation bitmap
    uint64_t liveBitmap{0};                     // live chunk-0 allocation bitmap
    QVector<uint64_t> ghostCibs;                // the genesis cib-address array (cib0 + immutable)
    QVector<uint64_t> liveCibs;                 // the live cib-address array (cib0 + immutable)
    uint64_t genesisIpUsage{2};                 // IP blocks the genesis checkpoint marks used
    uint64_t liveIpUsage{4};                    // IP blocks the live checkpoint marks used
    // Metadata-overflow tier (>~1.3 TiB): the internal pool + metadata prefix
    // (seedData) no longer fit chunk 0, so it spans metadataChunks chunks. Chunk 0
    // uses the rotating cib-0 bitmap; chunks 1..M-1 get immutable per-chunk bitmaps.
    uint64_t metadataChunks{1};
    QVector<uint64_t> extraChunkBitmaps;  // immutable bitmaps for chunks 1..M-1
    // CAB tier (>~7.8 TiB, cib_count > 507): the spaceman publishes cab_count CAB
    // (apfs_cib_addr_block) block numbers instead of the inline cib addresses; each
    // CAB holds up to 507 cib block numbers. Like cib 0, only CAB 0 rotates (it
    // references the rotating cib 0); cabs 1..N-1 are immutable. All 0/empty below
    // the CAB tier so the certified inline-cib layout is byte-identical.
    uint64_t cabCount{0};
    QVector<uint64_t> immutableCabs;  // cab 1..N-1, shared by every checkpoint
    uint64_t ghostCab0{0};            // genesis cab 0 (references ghost cib 0)
    uint64_t liveCab0{0};             // live cab 0 (references live cib 0)
    QVector<uint64_t> ghostCabs;      // genesis cab-address array ({ghostCab0} + immutable)
    QVector<uint64_t> liveCabs;       // live cab-address array ({liveCab0} + immutable)
    // Unified crash-safe rotation group. cib 0, its chunk-0 bitmap, (the cab 0 on the
    // CAB tier) and (the boundary-chunk bitmap on the non-CAB overflow tier) rotate
    // together every commit, so the IP region holds 3 group slots of ipGroupStride
    // blocks each. The boundary chunk (M-1) rotates inside the group only for the
    // non-CAB overflow tier; the CAB tier keeps its certified format (boundary
    // immutable in extraChunkBitmaps, in-place commit a later cascade). ipGroupStride
    // is 2 for the certified single-/multi-CIB tiers (byte-identical).
    uint64_t ipGroupStride{2};
    uint64_t boundaryChunk{0};        // M-1 when the boundary rotates; 0 otherwise
    uint64_t ghostBoundaryBitmap{0};  // genesis boundary-chunk bitmap (group offset 2)
    uint64_t liveBoundaryBitmap{0};   // live boundary-chunk bitmap (next group, offset 2)
};

// The post-internal-pool metadata shift: the genesis omap and everything after it
// move by this relative to the single-chunk layout. The post-pool region starts at
// ip_base + ip_block_count; the original base is the ghost container omap (block
// 191), so ipDelta is their difference (0 single-chunk).
uint64_t generatedIpDelta(uint64_t chunkCount, uint64_t cibCount, uint64_t cabCount = 0) {
    return generatedIpBaseBlock(chunkCount, cibCount, cabCount) +
           3 * (chunkCount + cibCount + cabCount) - kApfsFormatGhostContainerOmapBlock;
}

// seedData (the reserved metadata prefix length) = the live allocation-region start
// = the post-internal-pool seed-file block shifted by ipDelta.
uint64_t generatedSeedDataBlock(uint64_t chunkCount, uint64_t cibCount, uint64_t cabCount = 0) {
    return kApfsFormatSeedFileDataBlock + generatedIpDelta(chunkCount, cibCount, cabCount);
}

// Precomputed counts that drive the internal-pool placement (derived in
// computeMultiCibLayout so the placement helper stays branch-light).
struct ApfsIpRegionPlan {
    uint64_t cibCount{1};
    uint64_t cabCount{0};
    uint64_t immutableCabCount{0};  // cab 1..N-1
    uint64_t extraBitmaps{0};       // immutable chunk 1..M-2 bitmaps
    uint64_t boundaryRotates{0};    // boundary chunk M-1 rides cib 0's group (any multi-chunk tier)
};

// Lay out the internal-pool prefix: immutable cibs (N-1), immutable cabs, the
// fully-reserved overflow chunk bitmaps, then cib 0's ghost and live rotation groups
// {cib0, bitmap, (cab0), (boundary)}. The spare group stays free for the first commit.
void placeMultiCibIpRegion(ApfsMultiCibLayout* layout, const ApfsIpRegionPlan& plan) {
    uint64_t block = layout->ipBase;
    for (uint64_t i = 0; i + 1 < plan.cibCount; ++i) {
        layout->immutableCibs.append(block++);
    }
    for (uint64_t i = 0; i < plan.immutableCabCount; ++i) {
        layout->immutableCabs.append(block++);
    }
    for (uint64_t i = 0; i < plan.extraBitmaps; ++i) {
        layout->extraChunkBitmaps.append(block++);
    }
    layout->cib0Base = block;
    const uint64_t ghostCib0 = block++;  // ghost group: cib0, bitmap, (cab0), (boundary)
    layout->ghostBitmap = block++;
    if (plan.cabCount > 0) {
        layout->ghostCab0 = block++;
    }
    if (plan.boundaryRotates > 0) {
        layout->boundaryChunk = layout->metadataChunks - 1;
        layout->ghostBoundaryBitmap = block++;
    }
    const uint64_t liveCib0 = block++;  // live group: cib0, bitmap, (cab0), (boundary)
    layout->liveBitmap = block++;
    if (plan.cabCount > 0) {
        layout->liveCab0 = block++;
    }
    if (plan.boundaryRotates > 0) {
        // Last block assigned; the spare group that follows is free (never written).
        layout->liveBoundaryBitmap = block;
    }
    layout->ghostCibs = QVector<uint64_t>{ghostCib0} + layout->immutableCibs;
    layout->liveCibs = QVector<uint64_t>{liveCib0} + layout->immutableCibs;
    if (plan.cabCount > 0) {
        layout->ghostCabs = QVector<uint64_t>{layout->ghostCab0} + layout->immutableCabs;
        layout->liveCabs = QVector<uint64_t>{layout->liveCab0} + layout->immutableCabs;
    }
}

ApfsMultiCibLayout computeMultiCibLayout(uint64_t chunkCount) {
    const uint64_t cibCount = cibCountForChunks(chunkCount);
    const uint64_t cabCount = cabCountForCibs(cibCount);
    ApfsMultiCibLayout layout;
    layout.cibCount = cibCount;
    layout.cabCount = cabCount;
    layout.ipBase = generatedIpBaseBlock(chunkCount, cibCount, cabCount);
    // Metadata-overflow (>~1.3 TiB): the reserved prefix spans metadataChunks
    // chunks, so chunks 1..M-1 need immutable per-chunk bitmaps.
    const uint64_t seedData = generatedSeedDataBlock(chunkCount, cibCount, cabCount);
    layout.metadataChunks =
        std::max<uint64_t>(1,
                           std::min<uint64_t>(chunkCount,
                                              (seedData + kApfsSpacemanBlocksPerChunk - 1) /
                                                  kApfsSpacemanBlocksPerChunk));
    const uint64_t cabSlot = cabCount > 0 ? 1 : 0;
    // The boundary chunk (M-1) joins cib 0's rotation group on every multi-chunk
    // overflow tier, CAB included: it is the only reserved chunk with free space, so
    // its bitmap copies-on-write each commit and must rotate crash-safely. On the CAB
    // tier the group also carries cab 0 (groupSize 4); with cab_count 0 it is 3.
    const uint64_t boundaryRotates = (layout.metadataChunks > 1) ? 1 : 0;
    // Immutable per-chunk bitmaps cover chunks 1..M-1, minus the boundary chunk when it
    // rotates inside the group.
    const uint64_t extraBitmaps = layout.metadataChunks - 1 - boundaryRotates;
    const uint64_t immutableCabCount = cabCount > 0 ? cabCount - 1 : 0;
    // The rotation group = {cib0, chunk0-bitmap, (cab0), (boundary)}; 3 group slots of
    // groupSize blocks rotate crash-safely (ghost / live / spare). The ghost slots stay
    // contiguous before the live slots so the IP usage bitmap is a single run. With
    // cab_count 0 and metadataChunks 1 the cab/boundary terms vanish (groupSize 2) and
    // this reduces to the certified single-/multi-CIB layout byte-for-byte.
    const uint64_t groupSize = 2 + cabSlot + boundaryRotates;
    layout.ipGroupStride = groupSize;
    placeMultiCibIpRegion(&layout,
                          {.cibCount = cibCount,
                           .cabCount = cabCount,
                           .immutableCabCount = immutableCabCount,
                           .extraBitmaps = extraBitmaps,
                           .boundaryRotates = boundaryRotates});
    // Genesis marks the ghost group; the live checkpoint additionally marks the live
    // group (it still holds the ghost group via the IP free queue). The spare group
    // stays free until the first commit rotates into it.
    layout.genesisIpUsage = (cibCount - 1) + immutableCabCount + extraBitmaps + groupSize;
    layout.liveIpUsage = layout.genesisIpUsage + groupSize;
    return layout;
}

// Locations of a generated container's chunk-0 allocation structures. The live
// chunk-info block and its bitmap sit at fixed addresses inside the internal
// pool regardless of size, but the post-pool free region shifts by
// ipDelta = 3*(chunk_count+cib_count) - kApfsFormatIpBlockCount (the formula
// emptyFormatBlocks uses), so the in-place engine works on any container size,
// not only the 64 MiB envelope. Allocation stays inside chunk 0 (the only chunk
// with a populated bitmap on a fresh container).
struct ApfsGeneratedLayout {
    uint64_t chunkBitmap{0};
    uint64_t chunkInfo{0};
    uint64_t seedData{0};
    uint64_t chunk0Blocks{0};
    uint64_t cibCount{1};        // chunk-info blocks per checkpoint slot (multi-CIB > 1)
    uint64_t metadataChunks{1};  // chunks the reserved prefix spans (overflow tier > 1)
    // Metadata-overflow commit allocation. On the overflow tier chunk 0 is fully
    // reserved, so a commit allocates from the boundary chunk (metadataChunks - 1),
    // which holds the post-internal-pool free region (and the genesis fs chain). Its
    // bitmap rotates in cib 0's group (allocateFsCommitBlocks), keeping the internal-
    // pool usage contiguous. 0 when M == 1.
    uint64_t allocChunk{0};
    // First block of cib 0's three rotation slots = ip_base + (cib_count - 1) immutable
    // cibs + (cab_count - 1) immutable cabs + the immutable chunk bitmaps. nextIpSlot
    // rotates within this region; single/multi-CIB reduce it to ip_base + (cib_count-1).
    uint64_t cib0Base{kApfsFormatIpBaseBlock};
    // Crash-safe rotation group stride: cib0 + bitmap (2), +1 for cab 0 (CAB tier) and
    // +1 for the boundary-chunk bitmap (any multi-chunk tier), so 2 (single/multi-CIB),
    // 3 (overflow, cab_count 0), or 4 (CAB). boundaryChunk is the M-1 chunk that rotates.
    uint64_t ipGroupStride{2};
    uint64_t boundaryChunk{0};
    // CAB tier (cib_count > 507): the spaceman addresses cibs through a cab-address
    // array, so cab 0 rotates in the group (at group offset 2) and the commit
    // re-points the cab-array, not the cib-array. 0 below the CAB tier.
    uint64_t cabCount{0};
};

ApfsGeneratedLayout computeGeneratedLayout(uint64_t blockCount) {
    const uint64_t chunkCount = (blockCount + kApfsSpacemanBlocksPerChunk - 1) /
                                kApfsSpacemanBlocksPerChunk;
    const ApfsMultiCibLayout mcib = computeMultiCibLayout(chunkCount);
    const uint64_t ipDelta = generatedIpDelta(chunkCount, mcib.cibCount, mcib.cabCount);
    return {.chunkBitmap = mcib.liveBitmap,
            .chunkInfo = mcib.liveCibs.first(),
            .seedData = kApfsFormatSeedFileDataBlock + ipDelta,
            .chunk0Blocks = std::min<uint64_t>(blockCount, kApfsSpacemanBlocksPerChunk),
            .cibCount = mcib.cibCount,
            .metadataChunks = mcib.metadataChunks,
            .allocChunk = mcib.metadataChunks > 1 ? mcib.metadataChunks - 1 : 0,
            .cib0Base = mcib.cib0Base,
            .ipGroupStride = mcib.ipGroupStride,
            .boundaryChunk = mcib.boundaryChunk,
            .cabCount = mcib.cabCount};
}

// One internal-pool cib/bitmap slot. A single-chunk generated container's IP
// region (ip_base = chunkInfo - 2 = 185, six blocks) holds three slots:
// (185,186), (187,188), (189,190).
struct ApfsIpSlot {
    uint64_t cib{0};
    uint64_t bitmap{0};
};

// The slot a crash-safe commit writes next: the round-robin successor of the
// live slot, so the new cib(s)/bitmap land in a slot that is neither the live
// one nor the most recent ghost, leaving the previous checkpoint's slot intact
// (single-CIB: live cib 187 -> {189,190}; 189 -> {185,186}; 185 -> {187,188}).
// A slot spans slot_stride = cib_count + 1 blocks (cib_count cibs then the
// chunk-0 bitmap); the returned cib is the slot's first block and bitmap its
// last. Foundation for the crash-safe IP-region rotation
// (docs/APFS_A2_CRASH_SAFETY_DESIGN.md).
ApfsIpSlot nextIpSlot(uint64_t liveCib, const ApfsGeneratedLayout& layout) {
    constexpr int kIpSlotCount = 3;  // cib 0 rotates through 3 group slots
    // The rotation group spans ipGroupStride blocks (cib0 + chunk-0 bitmap, plus the
    // boundary bitmap on the non-CAB overflow tier); the new group is the round-robin
    // successor of the live group. cib0Base is the precomputed first group slot
    // (single/multi-CIB reduce to ip_base + (N-1), stride 2).
    const uint64_t cib0Base = layout.cib0Base;
    const uint64_t stride = layout.ipGroupStride;
    const int liveIndex = static_cast<int>((liveCib - cib0Base) / stride);
    const uint64_t nextBase = cib0Base +
                              static_cast<uint64_t>((liveIndex + 1) % kIpSlotCount) * stride;
    return {nextBase, nextBase + 1};
}

// The full cib/bitmap rotation a crash-safe commit performs: read the live slot,
// write the new cib/bitmap into the spare slot (nextIpSlot), and reclaim the
// previous ghost slot (the third, = nextIpSlot of the spare) which is no longer
// referenced by any ring checkpoint. live 187 -> new {189,190}, free {185,186}.
struct ApfsIpRotation {
    uint64_t liveCib{0};
    uint64_t liveBitmap{0};
    uint64_t newCib{0};
    uint64_t newBitmap{0};
    uint64_t freeCib{0};
    uint64_t freeBitmap{0};
};

ApfsIpRotation computeIpRotation(uint64_t liveCib,
                                 uint64_t liveBitmap,
                                 const ApfsGeneratedLayout& layout) {
    const ApfsIpSlot newSlot = nextIpSlot(liveCib, layout);
    const ApfsIpSlot freeSlot = nextIpSlot(newSlot.cib, layout);
    return {liveCib, liveBitmap, newSlot.cib, newSlot.bitmap, freeSlot.cib, freeSlot.bitmap};
}

struct ApfsFreeBlockScan {
    QIODevice* image{nullptr};
    ApfsRepairGeometry geometry;
    uint64_t bitmapBlock{0};
    uint64_t startBlock{0};
    uint64_t maxBlock{0};
    int count{0};
};

// Scan the raw allocation bitmap for `count` free blocks at or after
// `startBlock` (capped at maxBlock, the chunk-0 boundary), returning their
// addresses in ascending order.
bool findFreeBlocksInBitmap(const ApfsFreeBlockScan& scan,
                            QVector<uint64_t>* freeBlocks,
                            QStringList* blockers) {
    QByteArray bitmap(scan.geometry.blockSize, '\0');
    if (!readApfsRepairBlock(scan.image, scan.geometry, scan.bitmapBlock, &bitmap, blockers)) {
        return false;
    }
    for (uint64_t block = scan.startBlock; block < scan.maxBlock && freeBlocks->size() < scan.count;
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

// A chunk-bitmap scan window: chunkBase is the chunk's first absolute block (chunk
// 1's bit 0 is block 32768), [startBlock, maxBlock) the absolute range to scan.
struct ApfsBitmapScanRange {
    uint64_t chunkBase{0};
    uint64_t startBlock{0};
    uint64_t maxBlock{0};
};

// Scan a chunk's allocation bitmap CONTENT for up to `count` free blocks in the
// window, ascending (skipping used); appends their absolute addresses.
void findFreeBlocksInBitmapContent(const QByteArray& bitmap,
                                   const ApfsBitmapScanRange& range,
                                   int count,
                                   QVector<uint64_t>* freeBlocks) {
    for (uint64_t block = range.startBlock; block < range.maxBlock && freeBlocks->size() < count;
         ++block) {
        const uint64_t bit = block - range.chunkBase;
        const bool used = (bitmap.at(static_cast<qsizetype>(bit / 8)) >> (bit % 8)) & 1;
        if (!used) {
            freeBlocks->append(block);
        }
    }
}

// Read chunk `chunkIndex`'s allocation bitmap from the live cib. A cib entry with
// bitmap_addr 0 means the chunk is entirely free (an all-zero bitmap).
QByteArray readChunkAllocationBitmap(QIODevice* image,
                                     const ApfsRepairGeometry& geometry,
                                     const QByteArray& cib,
                                     uint64_t chunkIndex,
                                     QStringList* blockers) {
    QByteArray bitmap(geometry.blockSize, '\0');
    const qsizetype entry = kApfsChunkInfoEntriesOffset +
                            static_cast<qsizetype>(chunkIndex) * kApfsChunkInfoEntryStride;
    const uint64_t bitmapAddr = le64(cib, entry + kApfsChunkInfoEntryBitmapAddrOffset);
    if (bitmapAddr != 0) {
        readApfsRepairBlock(image, geometry, bitmapAddr, &bitmap, blockers);
    }
    return bitmap;
}

// Parse a generated volume object-map B-tree leaf (448-byte TOC, 16-byte
// fixed keys/values, as buildObjectMapTreeBlock emits) into {oid -> paddr}.
QHash<uint64_t, uint64_t> parseVolOmapEntries(QIODevice* image,
                                              const ApfsRepairGeometry& geometry,
                                              uint64_t volOmapTree,
                                              QStringList* blockers) {
    QHash<uint64_t, uint64_t> entries;
    QByteArray node(geometry.blockSize, '\0');
    if (!readApfsRepairBlock(image, geometry, volOmapTree, &node, blockers)) {
        return entries;
    }
    const uint32_t nkeys = le32(node, kApfsBtreeNodeCountOffset);
    const qsizetype keyAreaStart = kApfsBtreeNodeHeaderBytes + 448;
    const qsizetype valueAreaEnd = static_cast<qsizetype>(geometry.blockSize) - kApfsBtreeInfoBytes;
    for (uint32_t index = 0; index < nkeys; ++index) {
        const uint64_t oid = le64(node, keyAreaStart + static_cast<qsizetype>(index) * 16);
        const uint64_t paddr = le64(node,
                                    valueAreaEnd - (static_cast<qsizetype>(index) + 1) * 16 +
                                        kApfsOmapValuePaddrOffset);
        entries.insert(oid, paddr);
    }
    return entries;
}

// Every block the live fs-tree occupies (the root, plus its leaves when the root
// is an internal node), so a commit can free them all when it copies the tree.
bool collectOldFsTreeNodePaddrs(QIODevice* image,
                                const ApfsRepairGeometry& geometry,
                                const ApfsLiveFsChain& chain,
                                QVector<uint64_t>* paddrs,
                                QStringList* blockers) {
    QByteArray root(geometry.blockSize, '\0');
    if (!readApfsRepairBlock(image, geometry, chain.rootTree, &root, blockers)) {
        return false;
    }
    paddrs->append(chain.rootTree);
    if (le16(root, kApfsBtreeNodeLevelOffset) == 0) {
        return true;  // single leaf
    }
    const QHash<uint64_t, uint64_t> omap =
        parseVolOmapEntries(image, geometry, chain.volOmapTree, blockers);
    const uint32_t nkeys = le32(root, kApfsBtreeNodeCountOffset);
    const qsizetype valueAreaEnd = static_cast<qsizetype>(geometry.blockSize) - kApfsBtreeInfoBytes;
    for (uint32_t index = 0; index < nkeys; ++index) {
        const qsizetype toc = kApfsBtreeNodeHeaderBytes +
                              static_cast<qsizetype>(index) * kApfsBtreeVariableTocEntryBytes;
        const uint16_t valueOffset = le16(root, toc + kApfsBtreeVariableTocValueOffset);
        paddrs->append(omap.value(le64(root, valueAreaEnd - valueOffset)));
    }
    return true;
}

// Recover a file's data extents in logical order from the live fs-tree's
// j_file_extent records (key = {fileId, logical byte offset}, value = {length,
// physical block}). parseExtentRefOwners only yields one paddr per owner, so a file
// fragmented across several runs must be rebuilt from these records to be preserved
// across a chained commit. Returns the runs sorted by logical block (empty for an
// empty file - no extent records).
QVector<ApfsDataExtent> recoverFileDataExtents(QIODevice* image,
                                               const ApfsRepairGeometry& geometry,
                                               const ApfsLiveFsChain& chain,
                                               uint64_t fileId,
                                               QStringList* blockers) {
    QVector<uint64_t> nodes;
    if (!collectOldFsTreeNodePaddrs(image, geometry, chain, &nodes, blockers)) {
        return {};
    }
    QVector<ApfsDataExtent> extents;
    const uint64_t oidMask = (1ULL << kApfsObjTypeShift) - 1;
    const qsizetype valueAreaEnd = static_cast<qsizetype>(geometry.blockSize) - kApfsBtreeInfoBytes;
    for (uint64_t nodeBlock : nodes) {
        QByteArray node(geometry.blockSize, '\0');
        if (!readApfsRepairBlock(image, geometry, nodeBlock, &node, blockers)) {
            return {};
        }
        if (le16(node, kApfsBtreeNodeLevelOffset) != 0) {
            continue;  // internal node carries child pointers, not file records
        }
        const uint32_t nkeys = le32(node, kApfsBtreeNodeCountOffset);
        const qsizetype keyAreaStart = kApfsBtreeNodeHeaderBytes +
                                       le16(node, kApfsBtreeNodeTableLengthOffset);
        for (uint32_t index = 0; index < nkeys; ++index) {
            const qsizetype toc = kApfsBtreeNodeHeaderBytes +
                                  static_cast<qsizetype>(index) * kApfsBtreeVariableTocEntryBytes;
            const uint16_t keyOffset = le16(node, toc);
            const uint16_t valueOffset = le16(node, toc + kApfsBtreeVariableTocValueOffset);
            const uint64_t keyHeader = le64(node, keyAreaStart + keyOffset);
            if ((keyHeader >> kApfsObjTypeShift) != kApfsRecordFileExtent ||
                (keyHeader & oidMask) != fileId) {
                continue;
            }
            const uint64_t logicalBytes =
                le64(node, keyAreaStart + keyOffset + kApfsFileExtentKeyLogicalOffset);
            const qsizetype value = valueAreaEnd - valueOffset;
            const uint64_t lengthBytes = le64(node, value);
            const uint64_t paddr = le64(node, value + kApfsFileExtentValuePhysicalBlockOffset);
            extents.append({logicalBytes / kSupportedApfsBlockSizeBytes,
                            paddr,
                            lengthBytes / kSupportedApfsBlockSizeBytes});
        }
    }
    std::sort(extents.begin(), extents.end(), [](const ApfsDataExtent& a, const ApfsDataExtent& b) {
        return a.logicalBlock < b.logicalBlock;
    });
    return extents;
}

struct ApfsCowFileInsert {
    QIODevice* image{nullptr};
    ApfsRepairGeometry geometry;
    uint64_t newXid{0};
    ApfsLiveFsChain live;
    QVector<ApfsFsTreeNode> fsNodes;  // fs-tree nodes (root first); paddrs are newBlocks[0..K-1]
    // [fs-tree nodes (K), volOmapTree, volOmapHdr, volSb, ctrOmapTree, ctrOmapHdr]
    QVector<uint64_t> newBlocks;
    QVector<ApfsRootFilePayload> files;
    int64_t allocBlockDelta{0};      // change to the volume allocated-block count (data blocks)
    uint64_t extentRefNew{0};        // new extent-ref tree block (0 = keep the existing tree)
    int64_t fileCountDelta{1};       // +1 for a file insert, -1 for a file delete
    int64_t directoryCountDelta{0};  // +1 for a directory create, -1 for a directory delete
    uint64_t nextObjIdDelta{1};      // +1 when the mutation consumes an object id, else 0
};

// Copy-on-write the file-system metadata chain to the newly allocated blocks
// with the new file inserted: root tree -> volume object map -> volume
// superblock -> container object map, all stamped at the new transaction id.
// Copy-on-write the extent-ref tree with a j_phys_ext record per data extent so
// fsck_apfs credits the file's data blocks; an empty file (extentRefNew == 0)
// keeps the existing empty tree in place.
bool cowExtentRefTree(const ApfsCowFileInsert& cow, QStringList* blockers) {
    if (cow.extentRefNew == 0) {
        return true;
    }
    QByteArray extentRef = buildExtentRefTreeBlock(cow.geometry.blockSize, cow.files, blockers);
    writeLe64(&extentRef, kApfsObjectOidOffset, cow.extentRefNew);
    writeLe64(&extentRef, kApfsObjectXidOffset, cow.newXid);
    return stampAndWriteApfsBlock(cow.image, cow.geometry, cow.extentRefNew, &extentRef, blockers);
}

// Write each COW'd fs-tree node (root + leaves) at its allocated block, stamped
// at the new xid, and collect the {node oid -> paddr} mappings the volume object
// map records.
bool writeCowFsTreeNodes(const ApfsCowFileInsert& cow,
                         QVector<ApfsObjectMapEntry>* mappings,
                         QStringList* blockers) {
    for (qsizetype index = 0; index < cow.fsNodes.size(); ++index) {
        QByteArray node = cow.fsNodes.at(index).block;
        const uint64_t paddr = cow.newBlocks.at(index);
        writeLe64(&node, kApfsObjectXidOffset, cow.newXid);
        if (!stampAndWriteApfsBlock(cow.image, cow.geometry, paddr, &node, blockers)) {
            return false;
        }
        mappings->append({cow.fsNodes.at(index).oid, cow.newXid, paddr});
    }
    return true;
}

bool writeFileInsertCowChain(const ApfsCowFileInsert& cow, QStringList* blockers) {
    const uint32_t bs = cow.geometry.blockSize;
    const qsizetype nodeCount = cow.fsNodes.size();
    const uint64_t volOmapTree = cow.newBlocks.at(nodeCount);
    const uint64_t volOmapHdr = cow.newBlocks.at(nodeCount + 1);
    const uint64_t volSb = cow.newBlocks.at(nodeCount + 2);
    const uint64_t ctrOmapTree = cow.newBlocks.at(nodeCount + 3);
    const uint64_t ctrOmapHdr = cow.newBlocks.at(nodeCount + 4);

    QVector<ApfsObjectMapEntry> fsMappings;
    if (!writeCowFsTreeNodes(cow, &fsMappings, blockers)) {
        return false;
    }
    QByteArray volTree = buildObjectMapTreeBlock(bs, volOmapTree, fsMappings, cow.newXid, blockers);
    QByteArray volHdr = buildObjectMapBlock({bs, volOmapHdr, volOmapTree, cow.newXid, 0}, blockers);
    if (!writeApfsRepairBlock(cow.image, cow.geometry, volOmapTree, volTree, blockers) ||
        !writeApfsRepairBlock(cow.image, cow.geometry, volOmapHdr, volHdr, blockers)) {
        return false;
    }
    if (!cowExtentRefTree(cow, blockers)) {
        return false;
    }
    QByteArray vol(bs, '\0');
    if (!readApfsRepairBlock(cow.image, cow.geometry, cow.live.volSb, &vol, blockers)) {
        return false;
    }
    writeLe64(&vol, kApfsVolumeOmapOidOffset, volOmapHdr);
    if (cow.extentRefNew != 0) {
        writeLe64(&vol, kApfsVolumeExtentRefTreeOidOffset, cow.extentRefNew);
    }
    writeLe64(&vol,
              kApfsVolumeFileCountOffset,
              le64(vol, kApfsVolumeFileCountOffset) + static_cast<uint64_t>(cow.fileCountDelta));
    writeLe64(&vol,
              kApfsVolumeDirectoryCountOffset,
              le64(vol, kApfsVolumeDirectoryCountOffset) +
                  static_cast<uint64_t>(cow.directoryCountDelta));
    writeLe64(&vol,
              kApfsVolumeNextObjectIdOffset,
              le64(vol, kApfsVolumeNextObjectIdOffset) + cow.nextObjIdDelta);
    writeLe64(&vol,
              kApfsVolumeAllocatedBlockCountOffset,
              le64(vol, kApfsVolumeAllocatedBlockCountOffset) +
                  static_cast<uint64_t>(cow.allocBlockDelta));
    writeLe64(&vol, kApfsObjectXidOffset, cow.newXid);
    if (!stampAndWriteApfsBlock(cow.image, cow.geometry, volSb, &vol, blockers)) {
        return false;
    }
    QByteArray ctrTree = buildContainerOmapTreeBlock(bs,
                                                     ctrOmapTree,
                                                     {kApfsFormatVolumeOid, cow.newXid, volSb, 0},
                                                     cow.live.containerOmapOthers,
                                                     blockers);
    QByteArray ctrHdr = buildObjectMapBlock({bs, ctrOmapHdr, ctrOmapTree, cow.newXid, 1}, blockers);
    if (!writeApfsRepairBlock(cow.image, cow.geometry, ctrOmapTree, ctrTree, blockers) ||
        !writeApfsRepairBlock(cow.image, cow.geometry, ctrOmapHdr, ctrHdr, blockers)) {
        return false;
    }
    return true;
}

// Write the file payload into its newly allocated data blocks (the final block
// is zero-padded). A zero-length file allocates no data blocks.
bool writeApfsFileDataBlocks(QIODevice* image,
                             const ApfsRepairGeometry& geometry,
                             const QVector<uint64_t>& dataBlocks,
                             const QByteArray& fileData,
                             QStringList* blockers) {
    for (qsizetype index = 0; index < dataBlocks.size(); ++index) {
        QByteArray block(geometry.blockSize, '\0');
        const qsizetype offset = index * geometry.blockSize;
        const qsizetype bytes = qMin<qsizetype>(geometry.blockSize, fileData.size() - offset);
        std::copy(fileData.cbegin() + offset, fileData.cbegin() + offset + bytes, block.begin());
        if (!writeApfsRepairBlock(image, geometry, dataBlocks.at(index), block, blockers)) {
            return false;
        }
    }
    return true;
}

struct ApfsFileInsertAllocation {
    QIODevice* image{nullptr};
    ApfsRepairGeometry geometry;
    ApfsGeneratedLayout layout;
    ApfsIpRotation rotation;        // crash-safe cib/bitmap slot rotation
    QVector<uint64_t> freed;        // old COW-chain blocks (+ freed data on delete)
    QVector<uint64_t> allocated;    // newly written COW blocks
    int64_t cibFreeDelta{0};        // chunk-0 free-block delta if all allocation hit chunk 0
    uint64_t newXid{0};
    uint64_t chunk1BitmapBlock{0};  // 0 = single-chunk; else the chunk-1 bitmap's chunk-0 block
    // A7 (A-g) in-chunk grow: blocks added to chunk 0's ci_block_count (the device grew
    // within the existing chunk). 0 for every non-resize commit.
    int64_t chunk0BlockCountDelta{0};
};

// The allocated blocks that landed in chunk 1+ (>= chunk0Blocks) - their data went
// past chunk 0, so they are tracked by chunk 1's bitmap, not chunk 0's.
QVector<uint64_t> chunk1AllocatedBlocks(const ApfsFileInsertAllocation& alloc) {
    QVector<uint64_t> chunk1;
    for (uint64_t block : alloc.allocated) {
        if (block >= alloc.layout.chunk0Blocks) {
            chunk1.append(block);
        }
    }
    return chunk1;
}

// Overflow-tier rotated cib: chunk 0 stays full (free unchanged), its bitmap just
// moves to the new rotation slot. The boundary chunk (M-1) absorbs the allocation:
// its entry takes the fresh copy-on-write bitmap, the net free delta, and the new
// xid. M-1 < chunks_per_cib keeps it inside this (cib 0) block. Byte-identical to
// the inline branch it replaces.
bool writeRotatedCibOverflow(QByteArray* cib,
                             const ApfsFileInsertAllocation& alloc,
                             QStringList* blockers) {
    writeLe64(cib,
              kApfsChunkInfoEntriesOffset + kApfsChunkInfoEntryBitmapAddrOffset,
              alloc.rotation.newBitmap);
    const qsizetype entryM = kApfsChunkInfoEntriesOffset +
                             static_cast<qsizetype>(alloc.layout.allocChunk) *
                                 kApfsChunkInfoEntryStride;
    writeLe32(cib,
              entryM + kApfsChunkInfoEntryFreeCountOffset,
              static_cast<uint32_t>(
                  static_cast<int64_t>(le32(*cib, entryM + kApfsChunkInfoEntryFreeCountOffset)) +
                  alloc.cibFreeDelta));
    writeLe64(cib, entryM + kApfsChunkInfoEntryBitmapAddrOffset, alloc.chunk1BitmapBlock);
    writeLe64(cib, entryM, alloc.newXid);
    writeLe64(cib, kApfsObjectOidOffset, alloc.rotation.newCib);
    writeLe64(cib, kApfsObjectXidOffset, alloc.newXid);
    return stampAndWriteApfsBlock(
        alloc.image, alloc.geometry, alloc.rotation.newCib, cib, blockers);
}

// Write the rotated chunk-info block: read the live cib, adjust chunk 0's free count
// (and chunk 1's, on a multi-chunk overflow), re-point chunk 0's bitmap_addr at the
// new rotated bitmap slot (and chunk 1's at its fresh physical bitmap block), re-stamp
// at the new xid, and write it into the spare cib slot - leaving the live cib intact
// for the previous checkpoint (crash-safe).
bool writeRotatedCib(const ApfsFileInsertAllocation& alloc, QStringList* blockers) {
    QByteArray cib(alloc.geometry.blockSize, '\0');
    if (!readApfsRepairBlock(alloc.image, alloc.geometry, alloc.rotation.liveCib, &cib, blockers)) {
        return false;
    }
    if (alloc.layout.allocChunk != 0) {
        return writeRotatedCibOverflow(&cib, alloc, blockers);
    }
    const int chunk1Count = static_cast<int>(chunk1AllocatedBlocks(alloc).size());
    // cibFreeDelta assumed every allocation hit chunk 0; move the chunk-1 data back
    // out of chunk 0's free count (chunk 1's bitmap is an internal-pool slot already
    // reserved in chunk 0, so it does not change chunk 0's free count).
    const int64_t chunk0Delta = alloc.cibFreeDelta + chunk1Count;
    const qsizetype freeOffset = kApfsChunkInfoEntriesOffset + kApfsChunkInfoEntryFreeCountOffset;
    writeLe32(&cib,
              freeOffset,
              static_cast<uint32_t>(static_cast<int64_t>(le32(cib, freeOffset)) + chunk0Delta));
    // A7 (A-g) in-chunk grow: the device grew within chunk 0, so its ci_block_count
    // rises by the same delta (the matching free-count rise rides in cibFreeDelta).
    if (alloc.chunk0BlockCountDelta != 0) {
        const qsizetype countOffset = kApfsChunkInfoEntriesOffset +
                                      kApfsChunkInfoEntryBlockCountOffset;
        writeLe32(&cib,
                  countOffset,
                  static_cast<uint32_t>(static_cast<int64_t>(le32(cib, countOffset)) +
                                        alloc.chunk0BlockCountDelta));
    }
    writeLe64(&cib,
              kApfsChunkInfoEntriesOffset + kApfsChunkInfoEntryBitmapAddrOffset,
              alloc.rotation.newBitmap);
    // Chunk 0's allocation state changes every commit (its bitmap rotates), so stamp
    // its ci_xid at the commit xid: fsck requires the cib's object xid to equal the
    // most recent chunk xid (cib only changes if a chunk changes).
    writeLe64(&cib, kApfsChunkInfoEntriesOffset, alloc.newXid);
    if (alloc.chunk1BitmapBlock != 0) {
        const qsizetype entry1 = kApfsChunkInfoEntriesOffset + kApfsChunkInfoEntryStride;
        writeLe32(&cib,
                  entry1 + kApfsChunkInfoEntryFreeCountOffset,
                  static_cast<uint32_t>(
                      static_cast<int64_t>(le32(cib, entry1 + kApfsChunkInfoEntryFreeCountOffset)) -
                      chunk1Count));
        writeLe64(&cib, entry1 + kApfsChunkInfoEntryBitmapAddrOffset, alloc.chunk1BitmapBlock);
        writeLe64(&cib, entry1, alloc.newXid);
    }
    // The cib is a physical object: its o_oid must equal its block address, so it
    // has to move with the rotation (or a rolled-back checkpoint whose live cib is
    // not the genesis slot fails fsck with "cib at address 0x0"). Only cib 0
    // rotates; cib 1..N-1 are immutable and stay where the live array already
    // points them, so the rest of the array carries through unchanged.
    writeLe64(&cib, kApfsObjectOidOffset, alloc.rotation.newCib);
    writeLe64(&cib, kApfsObjectXidOffset, alloc.newXid);
    return stampAndWriteApfsBlock(
        alloc.image, alloc.geometry, alloc.rotation.newCib, &cib, blockers);
}

struct ApfsCab0Rotation {
    QIODevice* image{nullptr};
    ApfsRepairGeometry geometry;
    uint64_t liveCab0{0};  // the live cab 0 to copy forward
    uint64_t newCab0{0};   // its rotated group slot (group offset 2)
    uint64_t newCib0{0};   // the rotated cib 0 cab_cib_addr[0] must now point at
    uint64_t newXid{0};
};

// CAB tier: re-emit cab 0 into its rotated group slot (newCab0) with cab_cib_addr[0]
// re-pointed at the rotated cib 0. Only cib 0 moved; cab 0's other entries (cibs
// 1..506) and cabs 1..N-1 are immutable, so the live cab 0 carries through with just
// entry 0 and the object oid/xid restamped - the cab analogue of writeRotatedCib.
bool writeRotatedCab0(const ApfsCab0Rotation& rot, QStringList* blockers) {
    QByteArray cab(rot.geometry.blockSize, '\0');
    if (!readApfsRepairBlock(rot.image, rot.geometry, rot.liveCab0, &cab, blockers)) {
        return false;
    }
    writeLe64(&cab, kApfsCibAddrEntriesOffset, rot.newCib0);  // cab_cib_addr[0]
    writeLe64(&cab, kApfsObjectOidOffset, rot.newCab0);
    writeLe64(&cab, kApfsObjectXidOffset, rot.newXid);
    return stampAndWriteApfsBlock(rot.image, rot.geometry, rot.newCab0, &cab, blockers);
}

// Crash-safe allocation swap: copy the live allocation bitmap to the new (spare)
// bitmap slot with the commit's freed/allocated blocks flipped, then write the
// rotated cib into the new cib slot. The live cib/bitmap (referenced by the
// previous checkpoint) are never overwritten, so an interrupted commit leaves
// that checkpoint intact. The whole internal-pool region stays reserved in the
// chunk bitmap. (The sm_ip_bitmap ring is not advanced to match the cib rotation,
// so a container left on a non-genesis cib slot draws one cosmetic fsck
// "internal-pool overallocation" warning when the kernel continues it - see
// docs/APFS_A2_CRASH_SAFETY_DESIGN.md; fsck self-answers no and passes.)
// Overflow-tier allocation swap. Chunk 0 stays fully reserved: its bitmap rotates
// into the new cib-0 slot with its content unchanged. This commit's frees and
// allocations all land in the boundary chunk (metadataChunks - 1), whose bitmap is
// copied-on-write from its live block to the next group's boundary slot
// (chunk1BitmapBlock). The boundary chunk's updated cib entry is written by
// writeRotatedCib.
bool applyOverflowAllocation(const ApfsFileInsertAllocation& alloc, QStringList* blockers) {
    QByteArray chunk0(alloc.geometry.blockSize, '\0');
    if (!readApfsRepairBlock(
            alloc.image, alloc.geometry, alloc.rotation.liveBitmap, &chunk0, blockers)) {
        return false;
    }
    if (!writeApfsRepairBlock(
            alloc.image, alloc.geometry, alloc.rotation.newBitmap, chunk0, blockers)) {
        return false;
    }
    QByteArray liveCib(alloc.geometry.blockSize, '\0');
    if (!readApfsRepairBlock(
            alloc.image, alloc.geometry, alloc.rotation.liveCib, &liveCib, blockers)) {
        return false;
    }
    QByteArray allocBitmap = readChunkAllocationBitmap(
        alloc.image, alloc.geometry, liveCib, alloc.layout.allocChunk, blockers);
    const uint64_t chunkBase = alloc.layout.allocChunk * kApfsSpacemanBlocksPerChunk;
    QVector<uint64_t> freedRel;
    QVector<uint64_t> allocRel;
    for (uint64_t block : alloc.freed) {
        freedRel.append(block - chunkBase);
    }
    for (uint64_t block : alloc.allocated) {
        allocRel.append(block - chunkBase);
    }
    flipChunkBitmapBits(&allocBitmap, freedRel, allocRel);
    if (!writeApfsRepairBlock(
            alloc.image, alloc.geometry, alloc.chunk1BitmapBlock, allocBitmap, blockers)) {
        return false;
    }
    return writeRotatedCib(alloc, blockers);
}

bool applyFileInsertAllocation(const ApfsFileInsertAllocation& alloc, QStringList* blockers) {
    if (alloc.layout.allocChunk != 0) {
        return applyOverflowAllocation(alloc, blockers);
    }
    QByteArray bitmap(alloc.geometry.blockSize, '\0');
    if (!readApfsRepairBlock(
            alloc.image, alloc.geometry, alloc.rotation.liveBitmap, &bitmap, blockers)) {
        return false;
    }
    // Chunk 0's bitmap tracks the chunk-0 allocations (and, on overflow, the chunk-1
    // bitmap block, which physically lives in chunk 0); chunk 1's data is tracked by
    // its own bitmap below. freed blocks are always the old chain in chunk 0.
    QVector<uint64_t> chunk0Allocated;
    for (uint64_t block : alloc.allocated) {
        if (block < alloc.layout.chunk0Blocks) {
            chunk0Allocated.append(block);
        }
    }
    // chunk 1's bitmap lives in chunk 1's reserved internal-pool slot, which the
    // chunk-0 bitmap already marks used, so it is not added here.
    flipChunkBitmapBits(&bitmap, alloc.freed, chunk0Allocated);
    if (!writeApfsRepairBlock(
            alloc.image, alloc.geometry, alloc.rotation.newBitmap, bitmap, blockers)) {
        return false;
    }
    if (alloc.chunk1BitmapBlock != 0) {
        QByteArray liveCib(alloc.geometry.blockSize, '\0');
        if (!readApfsRepairBlock(
                alloc.image, alloc.geometry, alloc.rotation.liveCib, &liveCib, blockers)) {
            return false;
        }
        QByteArray chunk1Bitmap =
            readChunkAllocationBitmap(alloc.image, alloc.geometry, liveCib, 1, blockers);
        for (uint64_t block : chunk1AllocatedBlocks(alloc)) {
            const uint64_t bit = block - alloc.layout.chunk0Blocks;
            chunk1Bitmap[static_cast<qsizetype>(bit / 8)] |= static_cast<char>(1 << (bit % 8));
        }
        if (!writeApfsRepairBlock(
                alloc.image, alloc.geometry, alloc.chunk1BitmapBlock, chunk1Bitmap, blockers)) {
            return false;
        }
    }
    return writeRotatedCib(alloc, blockers);
}

// The blocks a commit frees: the old fs-tree nodes (root + any leaves), the
// volume/container object-map chain, and the old extent-ref tree when it was
// copied-on-written.
QVector<uint64_t> oldChainFreedBlocks(const ApfsLiveFsChain& chain,
                                      const QVector<uint64_t>& oldFsTreeNodes,
                                      bool extentRefCowed) {
    QVector<uint64_t> freed = oldFsTreeNodes;
    freed.append(chain.volOmapTree);
    freed.append(chain.volOmapHdr);
    freed.append(chain.volSb);
    freed.append(chain.ctrOmapTree);
    freed.append(chain.ctrOmapHdr);
    if (extentRefCowed) {
        freed.append(chain.extentRef);
    }
    return freed;
}

// The live paddr of an ephemeral object (by oid) in the current checkpoint map.
uint64_t findEphemeralPaddrByOid(QIODevice* image,
                                 const ApfsRepairGeometry& geometry,
                                 const ApfsLiveCheckpoint& live,
                                 uint64_t oid,
                                 QStringList* blockers) {
    QByteArray cpm(geometry.blockSize, '\0');
    if (!readApfsRepairBlock(image, geometry, live.descBase + live.descIndex, &cpm, blockers)) {
        return 0;
    }
    const uint32_t count = le32(cpm, kApfsCheckpointMapCountOffset);
    for (uint32_t index = 0; index < count; ++index) {
        const qsizetype entry = kApfsCheckpointMapEntriesOffset +
                                static_cast<qsizetype>(index) * kApfsCheckpointMapEntryBytes;
        if (le64(cpm, entry + kApfsCheckpointMapEntryOidOffset) == oid) {
            return le64(cpm, entry + kApfsCheckpointMapEntryPaddrOffset);
        }
    }
    return 0;
}

// Decode the {xid, paddr}->length records of a free-queue leaf (a 0xFFFF value
// offset is a ghost = length 1).
QVector<ApfsFreeQueueEntry> parseFreeQueueEntries(QIODevice* image,
                                                  const ApfsRepairGeometry& geometry,
                                                  uint64_t paddr,
                                                  QStringList* blockers) {
    QVector<ApfsFreeQueueEntry> entries;
    QByteArray node(geometry.blockSize, '\0');
    if (paddr == 0 || !readApfsRepairBlock(image, geometry, paddr, &node, blockers)) {
        return entries;
    }
    const uint32_t nkeys = le32(node, kApfsBtreeNodeCountOffset);
    const qsizetype keyStart = kApfsBtreeNodeHeaderBytes +
                               le16(node, kApfsBtreeNodeTableLengthOffset);
    const qsizetype infoOffset = static_cast<qsizetype>(geometry.blockSize) - kApfsBtreeInfoBytes;
    for (uint32_t index = 0; index < nkeys; ++index) {
        const uint16_t koff = le16(node, kApfsBtreeNodeHeaderBytes + index * 4);
        const uint16_t voff = le16(node, kApfsBtreeNodeHeaderBytes + index * 4 + 2);
        const uint64_t entryXid = le64(node, keyStart + koff);
        const uint64_t entryPaddr = le64(node, keyStart + koff + 8);
        const uint64_t length = (voff == 0xFFFF) ? 1 : le64(node, infoOffset - voff);
        entries.append({entryXid, entryPaddr, length});
    }
    return entries;
}

// Coalesce freed blocks into ascending contiguous runs tagged at this xid.
QVector<ApfsFreeQueueEntry> coalesceFreedRuns(QVector<uint64_t> blocks, uint64_t xid) {
    std::sort(blocks.begin(), blocks.end());
    QVector<ApfsFreeQueueEntry> runs;
    for (uint64_t block : blocks) {
        if (!runs.isEmpty() && runs.last().paddr + runs.last().length == block) {
            runs.last().length += 1;
        } else if (runs.isEmpty() || runs.last().paddr != block) {
            runs.append({xid, block, 1});
        }
    }
    return runs;
}

// Expand free-queue runs back into their individual block addresses.
QVector<uint64_t> expandFreeQueueEntries(const QVector<ApfsFreeQueueEntry>& entries) {
    QVector<uint64_t> blocks;
    for (const ApfsFreeQueueEntry& entry : entries) {
        for (uint64_t offset = 0; offset < entry.length; ++offset) {
            blocks.append(entry.paddr + offset);
        }
    }
    return blocks;
}

// Advance the main free-queue one commit: reclaim every run older than the
// rollback window (its blocks return to the allocator), keep the rest, and append
// this commit's freed blocks as fresh runs. Records sorted ascending by (xid,
// paddr); the window equals the COW chain region count (descriptor-ring depth).
struct ApfsMainFqAdvance {
    QVector<ApfsFreeQueueEntry> entries;  // the rebuilt queue (kept window + new runs)
    QVector<uint64_t> reclaimed;          // blocks freed back into the bitmap this commit
};

ApfsMainFqAdvance advanceMainFreeQueue(const QVector<ApfsFreeQueueEntry>& live,
                                       const QVector<uint64_t>& freedThisCommit,
                                       uint64_t newXid,
                                       uint64_t window) {
    ApfsMainFqAdvance out;
    const uint64_t reclaimThrough = newXid > window ? newXid - window : 0;
    QVector<ApfsFreeQueueEntry> reclaimedRuns;
    for (const ApfsFreeQueueEntry& entry : live) {
        if (entry.xid <= reclaimThrough) {
            reclaimedRuns.append(entry);
        } else {
            out.entries.append(entry);
        }
    }
    out.reclaimed = expandFreeQueueEntries(reclaimedRuns);
    out.entries += coalesceFreedRuns(freedThisCommit, newXid);
    return out;
}

struct ApfsFileInsertRequest {
    QVector<ApfsRootFilePayload> existingFiles;  // {fileName, fileId, parentDirectoryId, size}
    QString fileName;
    QByteArray fileData;
    // Existing directories (with their children carried in existingFiles via
    // parentDirectoryId) preserved across the commit. Empty on a flat-root container,
    // keeping the certified single-level layout byte-identical.
    QVector<ApfsRootDirectoryPayload> directories;
    // The new file's parent directory (the root directory for a root file, or a
    // directory's object id for a directory child).
    uint64_t newFileParentId{kApfsRootDirectoryId};
    // Object id for the written file. 0 = take the volume's next object id (a fresh
    // insert); non-zero reuses an existing id (an in-place patch that keeps the file's
    // identity while replacing its data extents).
    uint64_t explicitFileId{0};
    // A5: insert the file transparently compressed. fileData stays empty (no data
    // blocks are allocated); the content rides in decmpfsXattr (an embedded
    // com.apple.decmpfs value) and the inode is UF_COMPRESSED / uncompressedSize.
    bool compressed{false};
    QByteArray decmpfsXattr;
    uint64_t uncompressedSize{0};
    // A7 (A-h): arbitrary named xattrs attached to the inserted file (ACL, Finder
    // info, user attributes). Each becomes a j_xattr record; no data is allocated.
    QVector<QPair<QByteArray, QByteArray>> xattrs;
    // A7 (A-h): insert the file sparse with a trailing hole -- its extents cover
    // fileData, but the inode's logical size is sparseLogicalSize and the gap reads
    // as zeros (INODE_IS_SPARSE). 0 = dense.
    bool sparse{false};
    uint64_t sparseLogicalSize{0};
    // A7 (A-h) file clone: when non-zero, the inserted file is a clone that shares
    // the data stream owned by this private (dstream) id -- it allocates no data
    // blocks and emits no dstream-id/extent records, reusing the source's. Its inode
    // logical size is cloneLogicalSize (the shared stream's size). fileData must be
    // empty for a clone (no allocation).
    uint64_t cloneSourcePrivateId{0};
    uint64_t cloneLogicalSize{0};
    // A7 (A-h) hard link: when non-zero, this commit adds a new name (fileName, in
    // newFileParentId) for this existing inode rather than inserting a new file. No
    // data or inode is allocated; two sibling ids are taken from the id pool (the
    // primary name's + the new name's) and the inode's link count rises to 2.
    uint64_t hardlinkTargetId{0};
};

struct ApfsChainedListInput {
    QIODevice* image{nullptr};
    ApfsRepairGeometry geometry;
    ApfsLiveFsChain chain;
    ApfsFileInsertRequest request;
    uint64_t newDataStart{0};
    QVector<ApfsDataExtent> newDataExtents;  // the new file's runs (one if contiguous)
};

struct ApfsLiveTreeSource {
    QIODevice* image{nullptr};
    ApfsRepairGeometry geometry;
    ApfsLiveFsChain chain;
};

// Preserve every existing file (root or directory child) in place: keep its parent,
// fill in its data start, and recover a fragmented file's runs from the fs-tree so its
// data is not moved. A single-extent file keeps the dataStartBlock fast path so its
// records stay byte-identical.
bool recoverPreservedFiles(const ApfsLiveTreeSource& source,
                           const QVector<ApfsRootFilePayload>& inputFiles,
                           QVector<ApfsRootFilePayload>* files,
                           QStringList* blockers) {
    const QHash<uint64_t, uint64_t> owners =
        parseExtentRefOwners(source.image, source.geometry, source.chain.extentRef, blockers);
    for (ApfsRootFilePayload existing : inputFiles) {
        existing.privateId = existing.fileId;
        const QVector<ApfsDataExtent> recovered = recoverFileDataExtents(
            source.image, source.geometry, source.chain, existing.fileId, blockers);
        // The start block comes from the file's OWN file-extent records, not the
        // extent-ref owner map: a clone's shared block is owned by the source id in the
        // extent-ref tree, so an owner-map lookup on the clone's id would miss (yielding
        // block 0, a phantom hole). The empty-file fallback keeps the owner map.
        existing.dataStartBlock = recovered.isEmpty() ? owners.value(existing.fileId)
                                                      : recovered.first().paddr;
        if (recovered.size() > 1) {
            existing.dataExtents = recovered;
        }
        files->append(existing);
    }
    return true;
}

// Build the full root-file list for the commit: every existing file preserved
// in place plus the new file, which is assigned the volume's next object id.
bool buildChainedFileList(const ApfsChainedListInput& in,
                          QVector<ApfsRootFilePayload>* files,
                          QStringList* blockers) {
    QByteArray volSb(in.geometry.blockSize, '\0');
    if (!readApfsRepairBlock(in.image, in.geometry, in.chain.volSb, &volSb, blockers)) {
        return false;
    }
    const uint64_t newFileId = in.request.explicitFileId != 0
                                   ? in.request.explicitFileId
                                   : le64(volSb, kApfsVolumeNextObjectIdOffset);
    if (!recoverPreservedFiles(
            {in.image, in.geometry, in.chain}, in.request.existingFiles, files, blockers)) {
        return false;
    }
    if (in.request.hardlinkTargetId != 0) {
        // A hard link: add a new name for an existing inode instead of a new file. Two
        // sibling ids are taken from the id pool -- the primary name's (newFileId, the
        // lower) and the new name's (newFileId + 1).
        for (ApfsRootFilePayload& file : *files) {
            if (file.fileId != in.request.hardlinkTargetId) {
                continue;
            }
            file.primarySiblingId = newFileId;
            file.additionalLinks.append({.name = in.request.fileName.trimmed(),
                                         .parentId = in.request.newFileParentId,
                                         .siblingId = newFileId + 1});
            return true;
        }
        blockers->append(
            QStringLiteral("APFS file-hardlink-commit: target inode not found in the live tree"));
        return false;
    }
    if (in.request.cloneSourcePrivateId != 0) {
        // A clone is its OWN inode + data stream referencing the SOURCE's physical
        // blocks: it carries its own file-extent records (keyed by its own id) pointing
        // at the source's extents, so the macOS kernel -- which looks up extents by the
        // inode's own id, not the source's -- reads the cloned data. The shared blocks'
        // extent-ref refcount rises to 2 (buildExtentRefTreeBlock merges them). A
        // size-only buffer carries the inode's logical size; the extents come from
        // dataExtents, so no data is allocated.
        const QVector<ApfsDataExtent> sourceExtents = recoverFileDataExtents(
            in.image, in.geometry, in.chain, in.request.cloneSourcePrivateId, blockers);
        files->append(
            {.fileName = in.request.fileName.trimmed(),
             .data = QByteArray(static_cast<qsizetype>(in.request.cloneLogicalSize), '\0'),
             .parentDirectoryId = in.request.newFileParentId,
             .fileId = newFileId,
             .privateId = newFileId,
             .dataExtents = sourceExtents,
             .xattrs = in.request.xattrs,
             .extraInodeFlags = kApfsInodeFlagWasCloned | kApfsInodeFlagWasEverCloned});
        return true;
    }
    files->append({.fileName = in.request.fileName.trimmed(),
                   .data = in.request.fileData,
                   .parentDirectoryId = in.request.newFileParentId,
                   .fileId = newFileId,
                   .privateId = newFileId,
                   .dataStartBlock = in.newDataStart,
                   .dataExtents = in.newDataExtents,
                   .compressed = in.request.compressed,
                   .decmpfsXattr = in.request.decmpfsXattr,
                   .uncompressedSize = in.request.uncompressedSize,
                   .xattrs = in.request.xattrs,
                   .sparse = in.request.sparse,
                   .sparseLogicalSize = in.request.sparseLogicalSize});
    return true;
}

// The inputs an in-place file-insert commit needs: the preserved tree, the new
// file, and whether to store it transparently compressed (inline zlib decmpfs).
struct ApfsFileInsertBuildInputs {
    QVector<ApfsRootFilePayload> existingFiles;
    QString fileName;
    QByteArray fileData;
    QVector<ApfsRootDirectoryPayload> directories;
    bool compress{false};
    // A7 (A-h): arbitrary named xattrs + a trailing-hole sparse size (0 = dense).
    QVector<QPair<QByteArray, QByteArray>> xattrs;
    uint64_t sparseLogicalSize{0};
    // A7 (A-h) file clone: when non-zero, the inserted file clones the data stream
    // owned by this dstream id (cloneLogicalSize = the shared stream's size).
    uint64_t cloneSourcePrivateId{0};
    uint64_t cloneLogicalSize{0};
    // A7 (A-h) hard link: when non-zero, add fileName as a new name for this existing
    // inode (no new file/inode/data).
    uint64_t hardlinkTargetId{0};
};

// Build the in-place insert request for a (possibly compressed) file. When
// compress is set the payload is stored as an inline zlib com.apple.decmpfs
// xattr and no data blocks are allocated; the inode is UF_COMPRESSED. Fails
// closed if the compressed attribute would exceed the embedded-xattr limit
// (resource-fork compression for larger files is a documented follow-on).
bool buildFileInsertRequest(const ApfsFileInsertBuildInputs& in,
                            ApfsFileInsertRequest* request,
                            QStringList* blockers) {
    *request = {in.existingFiles, in.fileName, in.fileData, in.directories};
    request->xattrs = in.xattrs;
    request->cloneSourcePrivateId = in.cloneSourcePrivateId;
    request->cloneLogicalSize = in.cloneLogicalSize;
    request->hardlinkTargetId = in.hardlinkTargetId;
    // A trailing-hole sparse file: extents cover fileData, the inode logical size is
    // sparseLogicalSize, and the gap reads as zeros. Must exceed the data size.
    if (in.sparseLogicalSize > static_cast<uint64_t>(in.fileData.size())) {
        request->sparse = true;
        request->sparseLogicalSize = in.sparseLogicalSize;
    }
    if (!in.compress) {
        return true;
    }
    bool fits = false;
    const QByteArray decmpfs = apfsBuildInlineZlibDecmpfs(in.fileData, &fits);
    if (!fits) {
        blockers->append(
            QStringLiteral("APFS inline zlib compression: the compressed com.apple.decmpfs "
                           "attribute (%1 bytes) exceeds the embedded-xattr limit (%2 bytes); "
                           "resource-fork compression for larger files is not yet supported")
                .arg(decmpfs.size())
                .arg(kApfsXattrMaxEmbeddedSize));
        return false;
    }
    request->fileData.clear();
    request->compressed = true;
    request->decmpfsXattr = decmpfs;
    request->uncompressedSize = static_cast<uint64_t>(in.fileData.size());
    return true;
}

// The live container state every in-place fs-tree commit reads up front: the
// nx_superblock, its decoded checkpoint, the live fs-metadata chain, the
// generated layout, the paddrs of the old fs-tree nodes (freed by the commit),
// and the next virtual object id (the source of fresh fs-tree leaf oids).
struct ApfsFsCommitContext {
    QIODevice* image{nullptr};
    ApfsRepairGeometry geometry;
    QByteArray nxsb;
    ApfsLiveCheckpoint live;
    ApfsLiveFsChain chain;
    ApfsGeneratedLayout layout;
    QVector<uint64_t> oldFsNodes;  // every block the live fs-tree occupies
    uint64_t firstLeafOid{0};      // nx_next_oid - first oid a new leaf consumes
    uint64_t liveCib{0};           // live chunk-info block (rotates through IP slots)
    uint64_t liveBitmap{0};        // the live cib's allocation bitmap
    uint64_t allocChunkBitmap{0};  // overflow: live bitmap of the boundary chunk (M-1); 0 = chunk-0
    uint64_t liveCab0{0};          // CAB tier: live cab 0 (spaceman addr[0]); 0 below CAB
};

// Resolve the live chunk-info block (the spaceman cib_addr) and its bitmap. As
// the crash-safe commit rotates the cib/bitmap through the IP slots, these are
// no longer the fixed genesis 187/188 - allocation must read the live bitmap or
// it would scan a stale slot and reuse live blocks.
bool loadLiveAllocationSlot(ApfsFsCommitContext* ctx, QStringList* blockers) {
    const uint64_t addr0 = readLiveSpacemanCibAddr(ctx->image, ctx->geometry, ctx->nxsb, blockers);
    if (addr0 == 0) {
        return false;
    }
    if (ctx->layout.cabCount > 0) {
        // CAB tier: the spaceman's addr[0] is cab 0, not cib 0. Dereference its first
        // entry (cab_cib_addr[0]) to the live cib 0 the rotation operates on; keep cab 0
        // so the commit can re-emit it pointing at the rotated cib 0.
        ctx->liveCab0 = addr0;
        QByteArray cab(ctx->geometry.blockSize, '\0');
        if (!readApfsRepairBlock(ctx->image, ctx->geometry, addr0, &cab, blockers)) {
            return false;
        }
        ctx->liveCib = le64(cab, kApfsCibAddrEntriesOffset);
    } else {
        ctx->liveCib = addr0;
    }
    QByteArray cib(ctx->geometry.blockSize, '\0');
    if (ctx->liveCib == 0 ||
        !readApfsRepairBlock(ctx->image, ctx->geometry, ctx->liveCib, &cib, blockers)) {
        return false;
    }
    ctx->liveBitmap = le64(cib, kApfsChunkInfoEntriesOffset + kApfsChunkInfoEntryBitmapAddrOffset);
    if (ctx->layout.allocChunk != 0) {
        // Overflow tier: the boundary chunk (M-1) is the allocation chunk; read its
        // live bitmap from the live cib (M-1 < 126 keeps it inside cib 0).
        const qsizetype entry = kApfsChunkInfoEntriesOffset +
                                static_cast<qsizetype>(ctx->layout.allocChunk) *
                                    kApfsChunkInfoEntryStride;
        ctx->allocChunkBitmap = le64(cib, entry + kApfsChunkInfoEntryBitmapAddrOffset);
    }
    return true;
}

// A7 (A-h) sealed-volume policy: fail closed before any in-place commit touches a
// signed-system (sealed) volume. A sealed volume sets APFS_INCOMPAT_SEALED_VOLUME
// and references an integrity-meta object whose hash tree authenticates every block;
// mutating it silently breaks that seal (the OS refuses to boot the volume). The
// mutation is blocked unconditionally here -- a typed seal-invalidation confirmation
// is the documented opt-in override at the safety-validation layer, not a silent path.
bool appendSealedVolumeBlocker(QIODevice* image,
                               const ApfsRepairGeometry& geometry,
                               uint64_t volSbBlock,
                               QStringList* blockers) {
    QByteArray volSb(geometry.blockSize, '\0');
    if (!readApfsRepairBlock(image, geometry, volSbBlock, &volSb, blockers)) {
        return false;
    }
    const bool sealedFeature =
        (le64(volSb, kApfsVolumeIncompatibleFeaturesOffset) & kApfsVolumeIncompatSealed) != 0;
    if (sealedFeature || le64(volSb, kApfsVolumeIntegrityMetaOidOffset) != 0) {
        blockers->append(
            QStringLiteral("APFS sealed (signed-system) volume: any mutation breaks the volume "
                           "seal and is blocked; a typed seal-invalidation confirmation is "
                           "required to override"));
        return false;
    }
    return true;
}

bool loadFsCommitContext(QIODevice* image, ApfsFsCommitContext* ctx, QStringList* blockers) {
    uint32_t blockSize = 0;
    uint64_t blockCount = 0;
    if (!readApfsRepairGeometry(image, &blockSize, &blockCount, blockers)) {
        return false;
    }
    ctx->image = image;
    ctx->geometry = {blockSize, blockCount};
    ctx->nxsb = QByteArray(blockSize, '\0');
    if (!readApfsRepairBlock(image, ctx->geometry, kApfsFormatNxsbBlock, &ctx->nxsb, blockers)) {
        return false;
    }
    ctx->live = readLiveCheckpoint(ctx->nxsb);
    if (!walkLiveFsChain(
            image, ctx->geometry, le64(ctx->nxsb, kApfsNxOmapOidOffset), &ctx->chain, blockers)) {
        return false;
    }
    if (!appendSealedVolumeBlocker(image, ctx->geometry, ctx->chain.volSb, blockers)) {
        return false;
    }
    ctx->layout = computeGeneratedLayout(blockCount);
    // Metadata-overflow tier (>~1.3 TiB): chunk 0 is fully reserved, so the commit
    // allocates from the boundary chunk (metadataChunks - 1). That chunk must live
    // in cib 0 (index < chunks_per_cib) so the existing cib-0 rotation can carry its
    // updated entry; a boundary chunk in an immutable cib (only past ~10 TiB, where
    // M-1 >= 126) would need that cib to rotate too - not yet built.
    if (ctx->layout.allocChunk >= kApfsSpacemanChunksPerCib) {
        blockers->append(QStringLiteral(
            "APFS in-place commit on this metadata-overflow container is not yet supported: the "
            "boundary chunk falls outside cib 0"));
        return false;
    }
    // Repeated overflow commits (CAB tier included) are supported: the boundary-chunk
    // bitmap - and, on the CAB tier, cab 0 - ride in cib 0's rotation group and
    // copy-on-write into the next group's slot each commit, so a second commit never
    // re-COWs a block the previous checkpoint still references. Every reserved chunk
    // bitmap stays inside cib 0 (the boundary-outside-cib-0 guard above fail-closes the
    // larger tier that needs an immutable cib to rotate too).
    if (!collectOldFsTreeNodePaddrs(image, ctx->geometry, ctx->chain, &ctx->oldFsNodes, blockers)) {
        return false;
    }
    ctx->firstLeafOid = le64(ctx->nxsb, kApfsNxNextOidOffset);
    return loadLiveAllocationSlot(ctx, blockers);
}

// How many blocks a commit allocates: K fs-tree nodes + the five-block
// object-map chain + an optional extent-ref tree block + the file's data blocks.
struct ApfsCommitBlockSizing {
    qsizetype fsNodeCount{0};
    int extentRefSlots{0};
    uint64_t dataBlocks{0};
};

// The main free-queue rollback window: a freed block is reclaimed (returned to the
// bitmap) only once it has aged this many commits, so it stays allocated while any
// of the last few checkpoints can still roll back onto it. Set to the descriptor-
// ring depth (8 desc blocks / 2 = 4 checkpoints) - the same depth that guarantees
// an interrupted commit's predecessor keeps its blocks (the truncation test).
constexpr uint64_t kMainFqRollbackWindow = 4;

// Overflow tier (chunk 0 fully reserved): allocate the whole commit (metadata chain +
// data) from the boundary chunk's free region [seedData, end-of-chunk) and assign the
// rotated boundary-bitmap slot. That slot rides in cib 0's rotation group after cib 0,
// the chunk-0 bitmap, and (CAB tier) cab 0 - group offset 2 + cabSlot - so it
// copies-on-writes into the NEXT group's slot every commit, never re-COWing the live
// bitmap the previous checkpoint still references.
bool allocateOverflowCommitBlocks(const ApfsFsCommitContext& ctx,
                                  int need,
                                  QVector<uint64_t>* newBlocks,
                                  uint64_t* chunk1BitmapBlock,
                                  QStringList* blockers) {
    const uint64_t chunkBase = ctx.layout.allocChunk * kApfsSpacemanBlocksPerChunk;
    QByteArray cib(ctx.geometry.blockSize, '\0');
    if (!readApfsRepairBlock(ctx.image, ctx.geometry, ctx.liveCib, &cib, blockers)) {
        return false;
    }
    const QByteArray allocBitmap =
        readChunkAllocationBitmap(ctx.image, ctx.geometry, cib, ctx.layout.allocChunk, blockers);
    QVector<uint64_t> free;
    findFreeBlocksInBitmapContent(
        allocBitmap,
        {chunkBase, ctx.layout.seedData, chunkBase + kApfsSpacemanBlocksPerChunk},
        need,
        &free);
    if (free.size() < need) {
        blockers->append(QStringLiteral(
            "APFS in-place commit: not enough free space in the overflow boundary chunk"));
        return false;
    }
    *newBlocks = free.mid(0, need);
    const uint64_t cabSlot = ctx.layout.cabCount > 0 ? 1 : 0;
    *chunk1BitmapBlock = nextIpSlot(ctx.liveCib, ctx.layout).cib + 2 + cabSlot;
    return true;
}

// Allocate the commit's blocks: fs-tree nodes + object-map chain + data, taken as
// the lowest free blocks of chunk 0 (ascending, skipping used - non-contiguous when
// fragmented, the data extent then splits into runs). Crash-safety comes from the
// main free-queue (recently-freed blocks stay allocated until they age past the
// rollback window), so there is no per-commit size cap.
//
// When chunk 0 cannot hold everything, the data overflow goes into chunk 1 and one
// chunk-0 block is reserved for chunk 1's allocation bitmap (*chunk1BitmapBlock,
// 0 when no overflow). The fs-tree/extent records already handle chunk-1 data
// addresses; only the chunk-1 bitmap + its cib entry are special (finalizeFsCommit/
// applyFileInsertAllocation). The chunk-1 bitmap is a physical chunk-0 block, as
// Apple's large containers place per-chunk bitmaps, so the IP rotation is untouched.
bool allocateFsCommitBlocks(const ApfsFsCommitContext& ctx,
                            const ApfsCommitBlockSizing& sizing,
                            QVector<uint64_t>* newBlocks,
                            uint64_t* chunk1BitmapBlock,
                            QStringList* blockers) {
    *chunk1BitmapBlock = 0;
    const int metaCount = static_cast<int>(sizing.fsNodeCount) + 5 + sizing.extentRefSlots;
    const int need = metaCount + static_cast<int>(sizing.dataBlocks);
    if (ctx.layout.allocChunk != 0) {
        return allocateOverflowCommitBlocks(ctx, need, newBlocks, chunk1BitmapBlock, blockers);
    }
    QByteArray chunk0Bitmap(ctx.geometry.blockSize, '\0');
    if (!readApfsRepairBlock(ctx.image, ctx.geometry, ctx.liveBitmap, &chunk0Bitmap, blockers)) {
        return false;
    }
    QVector<uint64_t> chunk0Free;
    findFreeBlocksInBitmapContent(
        chunk0Bitmap, {0, ctx.layout.seedData, ctx.layout.chunk0Blocks}, need, &chunk0Free);
    if (chunk0Free.size() >= need) {
        *newBlocks = chunk0Free.mid(0, need);
        return true;
    }
    // Overflow into chunk 1: keep the fs-tree/chain metadata in chunk 0, spill the
    // remaining data blocks into chunk 1. Chunk 1's allocation bitmap goes in chunk
    // 1's reserved internal-pool slot, the first block after the immutable cibs
    // (cib_count - 1) and cib 0's three rotation slots (6): ip_base + cib_count + 5
    // (= ip_base + 6 single-CIB) - no chunk-0 block is consumed and the slot is
    // already reserved in the chunk-0 bitmap.
    const uint64_t chunkCount = (ctx.geometry.blockCount + kApfsSpacemanBlocksPerChunk - 1) /
                                kApfsSpacemanBlocksPerChunk;
    if (chunkCount < 2 || chunk0Free.size() < metaCount) {
        blockers->append(QStringLiteral("APFS in-place commit: not enough free space in chunk 0"));
        return false;
    }
    *chunk1BitmapBlock = kApfsFormatIpBaseBlock + ctx.layout.cibCount + 5;
    *newBlocks = chunk0Free;
    QByteArray cib(ctx.geometry.blockSize, '\0');
    if (!readApfsRepairBlock(ctx.image, ctx.geometry, ctx.liveCib, &cib, blockers)) {
        return false;
    }
    const QByteArray chunk1Bitmap =
        readChunkAllocationBitmap(ctx.image, ctx.geometry, cib, 1, blockers);
    const int overflow = need - static_cast<int>(newBlocks->size());
    QVector<uint64_t> chunk1Free;
    findFreeBlocksInBitmapContent(
        chunk1Bitmap,
        {ctx.layout.chunk0Blocks, ctx.layout.chunk0Blocks, 2 * ctx.layout.chunk0Blocks},
        overflow,
        &chunk1Free);
    if (chunk1Free.size() < overflow) {
        blockers->append(QStringLiteral("APFS in-place commit: not enough free space in chunk 1"));
        return false;
    }
    *newBlocks += chunk1Free;
    return true;
}

// The contiguous data blocks a file occupies (for the freed list on delete).
QVector<uint64_t> dataBlockRange(uint64_t start, uint64_t count) {
    QVector<uint64_t> blocks;
    for (uint64_t index = 0; index < count; ++index) {
        blocks.append(start + index);
    }
    return blocks;
}

// Every data block a file occupies across all its runs (for the freed list on
// delete). A single-extent file reduces to one contiguous dataBlockRange.
QVector<uint64_t> fileFreedDataBlocks(const ApfsRootFilePayload& file, uint32_t blockSize) {
    QVector<uint64_t> blocks;
    for (const ApfsDataExtent& extent : fileDataExtents(file, blockSize)) {
        blocks += dataBlockRange(extent.paddr, extent.blockCount);
    }
    return blocks;
}

struct ApfsFsCommitFinalize {
    ApfsFsCommitContext ctx;
    uint64_t newXid{0};
    QVector<ApfsFsTreeNode> fsNodes;     // root first; paddrs are newBlocks[0..K-1]
    QVector<uint64_t> newBlocks;         // [K nodes, 5-chain, extentRef?, data?]
    QVector<ApfsRootFilePayload> files;  // the committed root-file set
    uint64_t extentRefNew{0};            // new extent-ref tree block (0 = keep)
    QVector<uint64_t> freedDataBlocks;   // data blocks the commit releases
    int64_t dataBlocksNew{0};            // data blocks the commit allocates
    int64_t fileCountDelta{0};
    int64_t directoryCountDelta{0};
    uint64_t nextObjIdDelta{0};
    uint64_t chunk1BitmapBlock{0};  // chunk-1 bitmap's chunk-0 block (0 = single-chunk)
};

// Whether the live volume carries a snapshot (om_snap_count > 0). In-place
// mutation of a snapshotted container needs a multi-version volume omap +
// live-version reader (a documented follow-on), so callers fail closed.
bool liveVolumeHasSnapshot(const ApfsFsCommitContext& ctx, QStringList* blockers) {
    QByteArray volOmapHdr(ctx.geometry.blockSize, '\0');
    if (!readApfsRepairBlock(
            ctx.image, ctx.geometry, ctx.chain.volOmapHdr, &volOmapHdr, blockers)) {
        return false;
    }
    return le32(volOmapHdr, kApfsOmapSnapshotCountOffset) > 0;
}

// Shared commit tail: write the COW fs-tree + object-map chain, swap the
// allocation bitmap (old fs-tree nodes + object-map chain + freed data out, the
// new blocks in), and advance the checkpoint. The net block change (extra
// fs-tree nodes plus new minus freed data) threads identically through the
// volume allocated-count, the CIB/spaceman free counts, and nx_next_oid grows by
// the number of new fs-tree leaves.
// Cumulative IP-bitmap usage after the rotation touches the spare slot: the
// immutable cibs (cib_count - 1), the immutable cabs (cab_count - 1, CAB tier),
// the fully-reserved overflow chunk bitmaps (M - 2 on the overflow tier; the
// boundary chunk now rides in the group), all three rotation group slots
// (3 * groupSize, with 2 ghost groups held by the IP free-queue + the live
// group), and a non-overflow multi-chunk spill's chunk-1 slot. Single-CIB
// reduces to 6 (0x3f). Byte-identical to the inline computation it replaces.
uint64_t computeFinalizeIpBitmapUsage(const ApfsFsCommitFinalize& f, uint64_t groupSize) {
    const uint64_t extraBitmaps = f.ctx.layout.metadataChunks > 1 ? f.ctx.layout.metadataChunks - 2
                                                                  : 0;
    const uint64_t immutableCabCount = f.ctx.layout.cabCount > 0 ? f.ctx.layout.cabCount - 1 : 0;
    return (f.ctx.layout.cibCount - 1) + immutableCabCount + extraBitmaps + 3 * groupSize +
           (f.ctx.layout.allocChunk == 0 && f.chunk1BitmapBlock != 0 ? 1 : 0);
}

// Writes the file-insert COW chain for the commit. Byte-identical to the inline
// writeFileInsertCowChain call it replaces.
bool writeFinalizeCowChain(const ApfsFsCommitFinalize& f,
                           qsizetype nodeCount,
                           int64_t netConsumed,
                           QStringList* blockers) {
    return writeFileInsertCowChain({.image = f.ctx.image,
                                    .geometry = f.ctx.geometry,
                                    .newXid = f.newXid,
                                    .live = f.ctx.chain,
                                    .fsNodes = f.fsNodes,
                                    .newBlocks = f.newBlocks.mid(0, nodeCount + 5),
                                    .files = f.files,
                                    .allocBlockDelta = netConsumed,
                                    .extentRefNew = f.extentRefNew,
                                    .fileCountDelta = f.fileCountDelta,
                                    .directoryCountDelta = f.directoryCountDelta,
                                    .nextObjIdDelta = f.nextObjIdDelta},
                                   blockers);
}

struct ApfsFinalizeFreeQueue {
    ApfsMainFqAdvance mainFq;
    int64_t freedCount{0};
};

// Gathers this commit's freed blocks (old COW chain + freed data extents), queues
// them on the live main free-queue, and reclaims only the runs aged past the
// rollback window. Returns the advance plus the freed-block count for the
// net-queued accounting. Byte-identical to the inline sequence it replaces.
//
// Queue this commit's freed blocks on the main free-queue (they stay allocated
// in the bitmap so an interrupted commit's predecessor keeps them) and reclaim
// only the runs that have aged past the rollback window. The bitmap therefore
// releases the reclaimed runs, not this commit's frees; the cib/spaceman free
// counts move by (reclaimed - allocated) while the volume allocated-count still
// moves by netConsumed (the queued blocks leave the volume but not the device).
ApfsFinalizeFreeQueue advanceFinalizeMainFreeQueue(const ApfsFsCommitFinalize& f,
                                                   QStringList* blockers) {
    QVector<uint64_t> freed =
        oldChainFreedBlocks(f.ctx.chain, f.ctx.oldFsNodes, f.extentRefNew != 0);
    freed += f.freedDataBlocks;
    const QVector<ApfsFreeQueueEntry> liveMainFq = parseFreeQueueEntries(
        f.ctx.image,
        f.ctx.geometry,
        findEphemeralPaddrByOid(
            f.ctx.image, f.ctx.geometry, f.ctx.live, kApfsFormatFqMainTreeOid, blockers),
        blockers);
    const ApfsMainFqAdvance mainFq =
        advanceMainFreeQueue(liveMainFq, freed, f.newXid, kMainFqRollbackWindow);
    return {mainFq, static_cast<int64_t>(freed.size())};
}

// CAB tier: re-emit cab 0 into its group slot (offset 2) pointing at the freshly
// rotated cib 0 and report the new cab-0 address (which the spaceman cab-address
// array must use) via *newAddr0. Below the CAB tier (*newAddr0 already holds the
// new cib 0) this is a no-op. Byte-identical to the inline branch it replaces.
bool finalizeRotateCab0(const ApfsFsCommitFinalize& f,
                        const ApfsIpRotation& rotation,
                        uint64_t* newAddr0,
                        QStringList* blockers) {
    if (f.ctx.layout.cabCount > 0) {
        const uint64_t newCab0 = rotation.newCib + 2;
        if (!writeRotatedCab0({.image = f.ctx.image,
                               .geometry = f.ctx.geometry,
                               .liveCab0 = f.ctx.liveCab0,
                               .newCab0 = newCab0,
                               .newCib0 = rotation.newCib,
                               .newXid = f.newXid},
                              blockers)) {
            return false;
        }
        *newAddr0 = newCab0;
    }
    return true;
}

bool finalizeFsCommit(const ApfsFsCommitFinalize& f,
                      ApfsInPlaceCheckpointResult* result,
                      QStringList* blockers) {
    const qsizetype nodeCount = f.fsNodes.size();
    // Mutating a container that ALREADY carries a snapshot is a documented follow-on
    // (multi-version volume omap + live-version reader); fail closed. The A8
    // multi-volume + snapshot gate is met via mutate-then-snapshot.
    if (liveVolumeHasSnapshot(f.ctx, blockers)) {
        blockers->append(QStringLiteral(
            "APFS in-place mutation of a container that already has a snapshot is not yet "
            "supported; create the snapshot after the file mutations instead"));
        return false;
    }
    const int64_t extraNodes = static_cast<int64_t>(nodeCount) -
                               static_cast<int64_t>(f.ctx.oldFsNodes.size());
    const int64_t netConsumed = extraNodes +
                                (f.dataBlocksNew - static_cast<int64_t>(f.freedDataBlocks.size()));
    const ApfsIpRotation rotation =
        computeIpRotation(f.ctx.liveCib, f.ctx.liveBitmap, f.ctx.layout);
    if (!writeFinalizeCowChain(f, nodeCount, netConsumed, blockers)) {
        return false;
    }
    const ApfsFinalizeFreeQueue fq = advanceFinalizeMainFreeQueue(f, blockers);
    const ApfsMainFqAdvance& mainFq = fq.mainFq;
    const int64_t netQueued = fq.freedCount - static_cast<int64_t>(mainFq.reclaimed.size());
    const int64_t freeDelta = -netConsumed - netQueued;
    const uint64_t groupSize = f.ctx.layout.ipGroupStride;
    const uint64_t ipBitmapUsage = computeFinalizeIpBitmapUsage(f, groupSize);
    if (!applyFileInsertAllocation({.image = f.ctx.image,
                                    .geometry = f.ctx.geometry,
                                    .layout = f.ctx.layout,
                                    .rotation = rotation,
                                    .freed = mainFq.reclaimed,
                                    .allocated = f.newBlocks,
                                    .cibFreeDelta = freeDelta,
                                    .newXid = f.newXid,
                                    .chunk1BitmapBlock = f.chunk1BitmapBlock},
                                   blockers)) {
        return false;
    }
    // CAB tier: cib 0 just rotated, so re-emit cab 0 into its group slot (offset 2)
    // pointing at the new cib 0, and re-point the spaceman's cab-address array at the
    // new cab 0 (not the cib). Below the CAB tier the spaceman addresses cib 0 inline.
    uint64_t newAddr0 = rotation.newCib;
    if (!finalizeRotateCab0(f, rotation, &newAddr0, blockers)) {
        return false;
    }
    return advanceCheckpoint({.image = f.ctx.image,
                              .geometry = f.ctx.geometry,
                              .nxsb = f.ctx.nxsb,
                              .live = f.ctx.live,
                              .newContainerOmap = f.newBlocks.at(nodeCount + 4),
                              .spacemanFreeDelta = freeDelta,
                              .nextOidAdvance = static_cast<uint64_t>(nodeCount - 1),
                              .newCibAddr = newAddr0,
                              .cibCount = f.ctx.layout.cibCount,
                              // The whole rotation group (cib0 + bitmap + boundary)
                              // frees as one stride-length run, so the IP free-queue
                              // window covers the boundary bitmap - no separate entry.
                              .ipSlotStride = groupSize,
                              .ipBitmapUsage = ipBitmapUsage,
                              .freedCibSlot = rotation.liveCib,
                              .prevFreedCibSlot = rotation.freeCib,
                              .mainFqEntries = mainFq.entries},
                             result,
                             blockers);
}

// Pass 1 of an insert: build the merged file list with a placeholder data-start
// and size the resulting fs-tree (the node count is independent of the extent's
// paddr, so the placeholder is harmless) to learn how many blocks to allocate.
bool sizeInsertFsTree(const ApfsFsCommitContext& ctx,
                      const ApfsFileInsertRequest& request,
                      qsizetype* nodeCount,
                      QStringList* blockers) {
    QVector<ApfsRootFilePayload> files;
    QVector<ApfsFsTreeNode> probe;
    if (!buildChainedFileList({ctx.image, ctx.geometry, ctx.chain, request, 0}, &files, blockers) ||
        !buildFsTreeNodes({ctx.geometry.blockSize, files, request.directories, ctx.firstLeafOid},
                          &probe,
                          blockers)) {
        return false;
    }
    *nodeCount = probe.size();
    return true;
}

struct ApfsInsertFsNodes {
    QVector<ApfsRootFilePayload> files;  // the committed root-file set
    QVector<ApfsFsTreeNode> nodes;       // the fs-tree nodes (root first)
};

// Pass 2 of an insert: rebuild the file list with the real data-start, write the
// payload to its data blocks, and build the fs-tree nodes with the new file's
// data extent.
bool buildInsertFsNodes(const ApfsFsCommitContext& ctx,
                        const ApfsFileInsertRequest& request,
                        const QVector<uint64_t>& dataBlockList,
                        ApfsInsertFsNodes* out,
                        QStringList* blockers) {
    return buildChainedFileList({ctx.image,
                                 ctx.geometry,
                                 ctx.chain,
                                 request,
                                 dataBlockList.value(0),
                                 groupContiguousRuns(dataBlockList)},
                                &out->files,
                                blockers) &&
           writeApfsFileDataBlocks(
               ctx.image, ctx.geometry, dataBlockList, request.fileData, blockers) &&
           buildFsTreeNodes(
               {ctx.geometry.blockSize, out->files, request.directories, ctx.firstLeafOid},
               &out->nodes,
               blockers);
}

// A2: insert one file into a generated container with a true in-place
// copy-on-write checkpoint commit, preserving any files already present. COW
// the fs-metadata chain (fs-tree nodes -> volume object map -> volume superblock
// -> container object map, plus the extent-ref tree when there is data) to newly
// allocated blocks with the merged record set, write the new file's payload to
// further newly allocated data blocks (existing files' data stays in place),
// swap the allocation bitmap, adjust the spaceman/CIB free counts by the net
// data-block allocation, then advance the checkpoint with nx_omap_oid
// re-pointed at the new container object map. The fs-tree splits into an
// internal root over leaf nodes once the records overflow a single node.
bool commitInPlaceFileInsert(QIODevice* image,
                             const ApfsFileInsertRequest& request,
                             ApfsInPlaceCheckpointResult* result,
                             QStringList* blockers) {
    ApfsFsCommitContext ctx;
    if (!loadFsCommitContext(image, &ctx, blockers)) {
        return false;
    }
    const uint64_t dataBlocks = roundedBlockCount(static_cast<uint64_t>(request.fileData.size()),
                                                  ctx.geometry.blockSize);
    qsizetype nodeCount = 0;
    if (!sizeInsertFsTree(ctx, request, &nodeCount, blockers)) {
        return false;
    }
    // A file with data also copy-on-writes the extent-ref tree (one extra block); an
    // empty new file leaves it in place. A clone allocates no data but still rewrites
    // the extent-ref tree (the shared blocks' refcount rises to 2), so it COWs it too.
    const int extentRefSlots = static_cast<int>(dataBlocks > 0 ||
                                                request.cloneSourcePrivateId != 0);
    QVector<uint64_t> newBlocks;
    uint64_t chunk1BitmapBlock = 0;
    if (!allocateFsCommitBlocks(ctx,
                                {nodeCount, extentRefSlots, dataBlocks},
                                &newBlocks,
                                &chunk1BitmapBlock,
                                blockers)) {
        return false;
    }
    // The data blocks may be non-contiguous when free space is fragmented or when the
    // file overflows into chunk 1; they are grouped into one file-extent + extent-ref
    // record per contiguous run.
    const QVector<uint64_t> dataBlockList = newBlocks.mid(nodeCount + 5 + extentRefSlots);
    ApfsInsertFsNodes built;
    if (!buildInsertFsNodes(ctx, request, dataBlockList, &built, blockers)) {
        return false;
    }
    const uint64_t extentRefNew = extentRefSlots != 0 ? newBlocks.value(nodeCount + 5) : 0;
    // A hard link adds no inode (the file count is unchanged) but consumes two object
    // ids for its sibling records; a normal insert adds one inode and one id.
    const bool hardlink = request.hardlinkTargetId != 0;
    return finalizeFsCommit({.ctx = ctx,
                             .newXid = ctx.live.xid + 1,
                             .fsNodes = built.nodes,
                             .newBlocks = newBlocks,
                             .files = built.files,
                             .extentRefNew = extentRefNew,
                             .freedDataBlocks = {},
                             .dataBlocksNew = static_cast<int64_t>(dataBlocks),
                             .fileCountDelta = hardlink ? 0 : 1,
                             .nextObjIdDelta = hardlink ? 2ULL : 1ULL,
                             .chunk1BitmapBlock = chunk1BitmapBlock},
                            result,
                            blockers);
}

// Build a (possibly compressed) file-insert request and run the in-place
// checkpoint commit against @device, recording the resulting xids/blocks on the
// public result. Shared by the image-only and raw-device insert entry points so
// the compression preflight + commit live in one place.
void runInPlaceFileInsertCommit(QIODevice* device,
                                const ApfsFileInsertBuildInputs& inputs,
                                PartitionApfsImageCheckpointCommitResult* result) {
    ApfsFileInsertRequest request;
    if (!buildFileInsertRequest(inputs, &request, &result->blockers)) {
        return;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceFileInsert(device, request, &commit, &commitBlockers)) {
        result->previous_xid = commit.previous_xid;
        result->new_xid = commit.new_xid;
        result->checkpoint_map_block = commit.checkpoint_map_block;
        result->superblock_block = commit.superblock_block;
    }
    result->blockers.append(commitBlockers);
}

struct ApfsFilePatchRequest {
    QVector<ApfsRootFilePayload> otherFiles;  // every file except the patched one (preserved)
    QString fileName;
    QByteArray patchedData;  // the target's full data after applying the byte-range patch
    QVector<ApfsRootDirectoryPayload> directories;
    uint64_t parentDirectoryId{kApfsRootDirectoryId};
    uint64_t targetFileId{0};  // the patched file's existing object id (preserved)
};

// The on-disk data blocks a file currently owns (its extents), recovered from the live
// extent-ref tree - the blocks an in-place patch frees once the new data is written.
QVector<uint64_t> recoverFileExtentBlocks(const ApfsFsCommitContext& ctx,
                                          uint64_t fileId,
                                          QStringList* blockers) {
    ApfsRootFilePayload file;
    file.fileId = fileId;
    // Take the file's on-disk extents directly (paddr + length per run), so the freed
    // list is correct without the file's byte data: a single-extent file needs its real
    // extent here, not the data.size() fallback fileDataExtents uses for preserved files.
    file.dataExtents = recoverFileDataExtents(ctx.image, ctx.geometry, ctx.chain, fileId, blockers);
    return fileFreedDataBlocks(file, ctx.geometry.blockSize);
}

// A2: patch one file's data in place. Re-COW the file's data extents to new blocks
// holding the patched content while preserving the file's object id, name, parent, and
// the rest of the tree; the old extents are freed (net-zero file count and next-oid).
// Mirrors commitInPlaceFileInsert but reuses the target's id (explicitFileId) and frees
// the old data blocks - a true byte-range patch that is also crash-safe (the new data
// and the COW'd fs-tree/extent-ref publish atomically at the checkpoint).
bool commitInPlaceFilePatch(QIODevice* image,
                            const ApfsFilePatchRequest& request,
                            ApfsInPlaceCheckpointResult* result,
                            QStringList* blockers) {
    ApfsFsCommitContext ctx;
    if (!loadFsCommitContext(image, &ctx, blockers)) {
        return false;
    }
    const ApfsFileInsertRequest insert{.existingFiles = request.otherFiles,
                                       .fileName = request.fileName,
                                       .fileData = request.patchedData,
                                       .directories = request.directories,
                                       .newFileParentId = request.parentDirectoryId,
                                       .explicitFileId = request.targetFileId};
    const uint64_t dataBlocks = roundedBlockCount(static_cast<uint64_t>(request.patchedData.size()),
                                                  ctx.geometry.blockSize);
    qsizetype nodeCount = 0;
    if (!sizeInsertFsTree(ctx, insert, &nodeCount, blockers)) {
        return false;
    }
    const int extentRefSlots = static_cast<int>(dataBlocks > 0);
    QVector<uint64_t> newBlocks;
    uint64_t chunk1BitmapBlock = 0;
    if (!allocateFsCommitBlocks(ctx,
                                {nodeCount, extentRefSlots, dataBlocks},
                                &newBlocks,
                                &chunk1BitmapBlock,
                                blockers)) {
        return false;
    }
    const QVector<uint64_t> dataBlockList = newBlocks.mid(nodeCount + 5 + extentRefSlots);
    ApfsInsertFsNodes built;
    if (!buildInsertFsNodes(ctx, insert, dataBlockList, &built, blockers)) {
        return false;
    }
    const uint64_t extentRefNew = extentRefSlots != 0 ? newBlocks.value(nodeCount + 5) : 0;
    return finalizeFsCommit({.ctx = ctx,
                             .newXid = ctx.live.xid + 1,
                             .fsNodes = built.nodes,
                             .newBlocks = newBlocks,
                             .files = built.files,
                             .extentRefNew = extentRefNew,
                             .freedDataBlocks =
                                 recoverFileExtentBlocks(ctx, request.targetFileId, blockers),
                             .dataBlocksNew = static_cast<int64_t>(dataBlocks),
                             .fileCountDelta = 0,
                             .nextObjIdDelta = 0,
                             .chunk1BitmapBlock = chunk1BitmapBlock},
                            result,
                            blockers);
}

// Overwrite [offset, offset+patch.size()) of @p data with @p patch, growing the file
// (zero-padded) if the patch extends past the current end.
QByteArray applyBytePatch(QByteArray data, uint64_t offset, const QByteArray& patch) {
    const qsizetype end = static_cast<qsizetype>(offset) + patch.size();
    if (end > data.size()) {
        data.resize(end);
    }
    for (qsizetype i = 0; i < patch.size(); ++i) {
        data[static_cast<qsizetype>(offset) + i] = patch.at(i);
    }
    return data;
}

// Split the collected tree into the patch target (matched by parent + name) and the
// other files (preserved). Returns false if the target is absent.
bool selectPatchTarget(const QVector<ApfsRootFilePayload>& allFiles,
                       uint64_t parentId,
                       const QString& fileName,
                       uint64_t* targetFileId,
                       QVector<ApfsRootFilePayload>* otherFiles) {
    bool found = false;
    for (const ApfsRootFilePayload& file : allFiles) {
        if (!found && file.parentDirectoryId == parentId && file.fileName == fileName) {
            *targetFileId = file.fileId;
            found = true;
        } else {
            otherFiles->append(file);
        }
    }
    return found;
}

uint64_t resolveParentId(const QVector<ApfsRootDirectoryPayload>& directories,
                         const QString& directoryName);

struct ApfsPatchPrep {
    QString sourcePath;     // image or raw-device path to read the current data from
    QString directoryName;  // empty = root
    QString fileName;
    uint64_t offset{0};
    QByteArray patchBytes;
    QVector<ApfsRootFilePayload> allFiles;
    QVector<ApfsRootDirectoryPayload> directories;
};

// Read the target file's current data, apply the byte-range patch, and split the tree
// into the patch request (target id + preserved others). Fails closed if the directory
// or file is absent.
bool buildFilePatchRequest(const ApfsPatchPrep& in,
                           ApfsFilePatchRequest* out,
                           QStringList* blockers) {
    const uint64_t parentId = resolveParentId(in.directories, in.directoryName);
    if (parentId == 0) {
        blockers->append(QStringLiteral("APFS file-patch-commit: directory '%1' was not found")
                             .arg(in.directoryName));
        return false;
    }
    const QString path = in.directoryName.isEmpty()
                             ? QStringLiteral("/%1").arg(in.fileName)
                             : QStringLiteral("/%1/%2").arg(in.directoryName, in.fileName);
    const auto read = PartitionApfsFileSystemReader::readFileFromImage(in.sourcePath,
                                                                       path,
                                                                       kApfsMaximumSeedFileBytes);
    uint64_t targetFileId = 0;
    QVector<ApfsRootFilePayload> otherFiles;
    if (!read.ok ||
        !selectPatchTarget(in.allFiles, parentId, in.fileName, &targetFileId, &otherFiles)) {
        blockers->append(
            QStringLiteral("APFS file-patch-commit: file '%1' was not found").arg(in.fileName));
        return false;
    }
    *out = {.otherFiles = otherFiles,
            .fileName = in.fileName,
            .patchedData = applyBytePatch(read.data, in.offset, in.patchBytes),
            .directories = in.directories,
            .parentDirectoryId = parentId,
            .targetFileId = targetFileId};
    return true;
}

struct ApfsDeleteListInput {
    QIODevice* image{nullptr};
    ApfsRepairGeometry geometry;
    ApfsLiveFsChain chain;
    QVector<ApfsRootFilePayload> allFiles;
    QString targetName;
    uint64_t targetParentId{kApfsRootDirectoryId};  // root for a root file, else a directory id
};

// Split the container's root files into the delete target (matched by name,
// with its data extent recovered) and the remaining files (data extents filled
// so they are preserved in place). Fails closed if the target is not found.
bool buildDeleteFileList(const ApfsDeleteListInput& in,
                         QVector<ApfsRootFilePayload>* remaining,
                         ApfsRootFilePayload* target,
                         QStringList* blockers) {
    const QHash<uint64_t, uint64_t> owners =
        parseExtentRefOwners(in.image, in.geometry, in.chain.extentRef, blockers);
    bool found = false;
    for (ApfsRootFilePayload file : in.allFiles) {
        // Keep each file's parent (root or a directory) so directory children survive
        // the commit; a flat-root container collected every file with the root parent.
        file.privateId = file.fileId;
        file.dataStartBlock = owners.value(file.fileId);
        // Recover a fragmented file's runs (logical order) so the target frees every
        // extent on delete and a preserved file keeps all of them; single-extent
        // files keep the dataStartBlock fast path (byte-identical).
        const QVector<ApfsDataExtent> recovered =
            recoverFileDataExtents(in.image, in.geometry, in.chain, file.fileId, blockers);
        if (recovered.size() > 1) {
            file.dataExtents = recovered;
        }
        // Match the target in its requested parent only (the root directory for a root
        // file, or a directory id for a directory child), so a same-named file under a
        // different parent is left untouched.
        if (file.parentDirectoryId == in.targetParentId && file.fileName == in.targetName) {
            *target = file;
            found = true;
        } else {
            remaining->append(file);
        }
    }
    if (!found) {
        blockers->append(
            QStringLiteral("APFS file-delete-commit: file '%1' was not found").arg(in.targetName));
        return false;
    }
    return true;
}

// A3: request to create one snapshot of a generated APFS volume.
struct ApfsSnapshotCreateRequest {
    QString snapshotName;
    uint64_t createTimeNs{0};  // APFS time (ns since 1970-01-01 UTC); 0 = caller fills it in
};

// COW inputs for a snapshot create. newBlocks layout (8 freshly allocated blocks):
//   [0]=frozenExtentRef [1]=frozenSblock  [2]=omapSnapTree [3]=snapMetaTree
//   [4]=volOmapHdr      [5]=volSb          [6]=ctrOmapTree  [7]=ctrOmapHdr
struct ApfsSnapshotCreateCow {
    QIODevice* image{nullptr};
    ApfsRepairGeometry geometry;
    uint64_t newXid{0};
    ApfsLiveFsChain live;
    QVector<uint64_t> newBlocks;
    ApfsSnapshotCreateRequest request;
};

// Per-commit snapshot-create scalars derived from the live volume superblock.
struct ApfsSnapshotCowState {
    QByteArray liveVol;
    uint64_t newSnapCount{0};
    uint64_t snapInum{0};
    uint64_t newAllocCount{0};
};

// Write the four snapshot structures (newBlocks[0..3]): the frozen extent-ref tree
// copy, the frozen physical superblock copy, the omap-snapshot tree, and the
// snap-meta tree (j_snap_metadata + j_snap_name).
bool writeSnapshotFrozenBlocks(const ApfsSnapshotCreateCow& cow,
                               const ApfsSnapshotCowState& st,
                               QStringList* blockers) {
    const uint32_t bs = cow.geometry.blockSize;
    const uint64_t frozenExtentRef = cow.newBlocks.at(0);
    const uint64_t frozenSblock = cow.newBlocks.at(1);
    const uint64_t omapSnapTree = cow.newBlocks.at(2);
    const uint64_t snapMetaTree = cow.newBlocks.at(3);
    const uint64_t snapXid = cow.newXid;
    // (A) the LIVE volume's post-snapshot extent-ref tree: a fresh EMPTY tree.
    // The snapshot owns the existing blocks via the ORIGINAL extent-ref tree
    // (cow.live.extentRef, left in place with its KIND_NEW records, referenced by
    // j_snap_metadata); the live volume starts with no extent-ref records and gains
    // them only as later writes diverge. apfsck processes the snapshot first (NEW
    // sets the refcnt + marks each block used once) then the live (empty -> refcnt
    // unchanged), and the per-version e_references each equal the file's references.
    QByteArray extentRef = buildExtentRefTreeBlock(bs, {}, blockers);
    writeLe64(&extentRef, kApfsObjectOidOffset, frozenExtentRef);
    writeLe64(&extentRef, kApfsObjectXidOffset, snapXid);
    if (!stampAndWriteApfsBlock(cow.image, cow.geometry, frozenExtentRef, &extentRef, blockers)) {
        return false;
    }
    // (B) frozen superblock copy (physical object; o_oid == its own paddr).
    QByteArray frozen =
        buildFrozenSnapshotSuperblock(st.liveVol, frozenSblock, snapXid, st.newSnapCount, blockers);
    if (!writeApfsRepairBlock(cow.image, cow.geometry, frozenSblock, frozen, blockers)) {
        return false;
    }
    // (C) omap-snapshot tree (one entry: this snapshot's xid; oms value all-zero).
    QByteArray omapSnap =
        buildOmapSnapshotTreeBlock(bs, omapSnapTree, snapXid, {{snapXid, 0, 0}}, blockers);
    if (!writeApfsRepairBlock(cow.image, cow.geometry, omapSnapTree, omapSnap, blockers)) {
        return false;
    }
    // (D) snap-meta tree (j_snap_metadata + j_snap_name). The snapshot owns the
    // ORIGINAL (older) extent-ref tree with its KIND_NEW records (cow.live.extentRef,
    // left in place, not freed); the LIVE volume's extent-ref is re-pointed at the
    // newBlocks[0] copy whose records were converted to KIND_UPDATE (the +1 reference
    // the snapshot adds). apfsck looks an UPDATE record up in the snapshot extref
    // trees, so the NEW record must live in the snapshot's tree, the UPDATE in the live.
    const ApfsSnapshotMetadata meta{.snapXid = snapXid,
                                    .extentRefTreeOid = cow.live.extentRef,
                                    .sblockOid = frozenSblock,
                                    .createTimeNs = cow.request.createTimeNs,
                                    .changeTimeNs = cow.request.createTimeNs,
                                    .inum = st.snapInum,
                                    .name = cow.request.snapshotName.toUtf8()};
    QByteArray snapMeta = buildVariableKvLeafBlock(
        {bs, snapMetaTree, snapXid, kApfsObjectSubtypeSnapMeta, buildSnapMetaRecords(meta)},
        blockers);
    return writeApfsRepairBlock(cow.image, cow.geometry, snapMetaTree, snapMeta, blockers);
}

// Write the volume + container chain (newBlocks[4..7]): the volume omap header (with
// the snapshot-tree pointer and counters), the live volume superblock (re-pointed at
// the new omap + snap-meta tree), and the container object map re-pointed at it.
bool writeSnapshotVolumeChain(const ApfsSnapshotCreateCow& cow,
                              const ApfsSnapshotCowState& st,
                              QStringList* blockers) {
    const uint32_t bs = cow.geometry.blockSize;
    const uint64_t omapSnapTree = cow.newBlocks.at(2);
    const uint64_t snapMetaTree = cow.newBlocks.at(3);
    const uint64_t volOmapHdr = cow.newBlocks.at(4);
    const uint64_t volSb = cow.newBlocks.at(5);
    const uint64_t ctrOmapTree = cow.newBlocks.at(6);
    const uint64_t ctrOmapHdr = cow.newBlocks.at(7);
    const uint64_t snapXid = cow.newXid;
    // (E) volume omap header: COW the live omap_phys, keep its object-map tree
    // (unchanged), add the snapshot-tree pointer + counters.
    QByteArray volOmap(bs, '\0');
    if (!readApfsRepairBlock(cow.image, cow.geometry, cow.live.volOmapHdr, &volOmap, blockers)) {
        return false;
    }
    writeLe64(&volOmap, kApfsObjectOidOffset, volOmapHdr);
    writeLe64(&volOmap, kApfsObjectXidOffset, snapXid);
    writeLe32(&volOmap,
              kApfsOmapSnapshotCountOffset,
              le32(volOmap, kApfsOmapSnapshotCountOffset) + 1);
    writeLe64(&volOmap, kApfsOmapSnapshotTreeOidOffset, omapSnapTree);
    writeLe64(&volOmap, kApfsOmapMostRecentSnapshotOffset, snapXid);
    writeLe64(&volOmap, kApfsOmapPendingRevertMinOffset, 0);
    writeLe64(&volOmap, kApfsOmapPendingRevertMaxOffset, 0);
    if (!stampAndWriteApfsBlock(cow.image, cow.geometry, volOmapHdr, &volOmap, blockers)) {
        return false;
    }
    // (F) volume superblock: COW the live APSB, re-point its object map + snap-meta
    // tree, bump num_snapshots + next_obj_id + allocated-block count.
    QByteArray vol = st.liveVol;
    writeLe64(&vol, kApfsObjectXidOffset, snapXid);
    writeLe64(&vol, kApfsVolumeOmapOidOffset, volOmapHdr);
    // The live volume now references the KIND_UPDATE extent-ref copy (newBlocks[0]);
    // the original NEW tree (cow.live.extentRef) became the snapshot's, preserved.
    writeLe64(&vol, kApfsVolumeExtentRefTreeOidOffset, cow.newBlocks.at(0));
    writeLe64(&vol, kApfsVolumeSnapMetaTreeOidOffset, snapMetaTree);
    writeLe64(&vol, kApfsVolumeNumSnapshotsOffset, st.newSnapCount);
    writeLe64(&vol, kApfsVolumeNextObjectIdOffset, st.snapInum + 1);
    writeLe64(&vol, kApfsVolumeAllocatedBlockCountOffset, st.newAllocCount);
    if (!stampAndWriteApfsBlock(cow.image, cow.geometry, volSb, &vol, blockers)) {
        return false;
    }
    // (G) container object map (tree + header) re-pointed at the new volume superblock.
    QByteArray ctrTree = buildContainerOmapTreeBlock(bs,
                                                     ctrOmapTree,
                                                     {kApfsFormatVolumeOid, snapXid, volSb, 0},
                                                     cow.live.containerOmapOthers,
                                                     blockers);
    QByteArray ctrHdr = buildObjectMapBlock({bs, ctrOmapHdr, ctrOmapTree, snapXid, 1}, blockers);
    return writeApfsRepairBlock(cow.image, cow.geometry, ctrOmapTree, ctrTree, blockers) &&
           writeApfsRepairBlock(cow.image, cow.geometry, ctrOmapHdr, ctrHdr, blockers);
}

// Copy-on-write chain for a snapshot create. The root fs-tree, the volume object-map
// tree, and the live extent-ref tree are left in place (shared with the snapshot until
// a later write diverges); only the snapshot structures and the volume/container
// superblock + object-map headers are written. Byte recipe harvested from a real
// macOS sealed-system snapshot (temp/snapshot-recipe-resolved.txt).
bool writeSnapshotCreateCowChain(const ApfsSnapshotCreateCow& cow, QStringList* blockers) {
    const uint32_t bs = cow.geometry.blockSize;
    ApfsSnapshotCowState st;
    st.liveVol = QByteArray(bs, '\0');
    if (!readApfsRepairBlock(cow.image, cow.geometry, cow.live.volSb, &st.liveVol, blockers)) {
        return false;
    }
    st.newSnapCount = le64(st.liveVol, kApfsVolumeNumSnapshotsOffset) + 1;
    st.snapInum = le64(st.liveVol, kApfsVolumeNextObjectIdOffset);
    const uint64_t beforeAlloc = le64(st.liveVol, kApfsVolumeAllocatedBlockCountOffset);
    // The logical alloc-count formula (2*before - 3) holds only for a single-node
    // volume object-map tree; a multi-level omap would not re-walk to a known count.
    QByteArray volOmapTreeNode(bs, '\0');
    if (!readApfsRepairBlock(
            cow.image, cow.geometry, cow.live.volOmapTree, &volOmapTreeNode, blockers)) {
        return false;
    }
    if (le16(volOmapTreeNode, kApfsBtreeNodeLevelOffset) != 0) {
        blockers->append(QStringLiteral(
            "APFS snapshot-create: multi-level volume object map is not yet supported"));
        return false;
    }
    if (beforeAlloc < kApfsSnapshotLiveOnlyBlocks) {
        blockers->append(
            QStringLiteral("APFS snapshot-create: implausible volume allocated-block count"));
        return false;
    }
    st.newAllocCount = beforeAlloc + kApfsSnapshotAllocDelta;
    return writeSnapshotFrozenBlocks(cow, st, blockers) &&
           writeSnapshotVolumeChain(cow, st, blockers);
}

// Inputs to the shared snapshot-create commit tail.
struct ApfsSnapshotCreateFinalize {
    ApfsFsCommitContext ctx;
    ApfsSnapshotCreateRequest request;
    uint64_t oldSnapMetaTree{0};
    QVector<uint64_t> newBlocks;
    uint64_t chunk1BitmapBlock{0};
};

// Inputs to the shared snapshot-commit checkpoint tail.
struct ApfsSnapshotCheckpointTail {
    ApfsFsCommitContext ctx;
    uint64_t newXid{0};
    QVector<uint64_t> freed;      // old chain + snapshot-exclusive blocks released this commit
    QVector<uint64_t> allocated;  // the newly written blocks
    uint64_t containerOmapBlock{0};
    uint64_t chunk1BitmapBlock{0};
};

// Shared snapshot commit tail (create + delete): thread the freed/allocated block sets
// through the main free-queue, the allocation bitmap, the CIB/spaceman free counts, and
// the checkpoint - the same crash-safe machinery the file-mutating commits use. The COW
// chain must already be written; this advances the checkpoint that publishes it.
bool commitSnapshotCheckpointTail(const ApfsSnapshotCheckpointTail& t,
                                  ApfsInPlaceCheckpointResult* result,
                                  QStringList* blockers) {
    const ApfsFsCommitContext& ctx = t.ctx;
    const int64_t netConsumed = static_cast<int64_t>(t.allocated.size()) -
                                static_cast<int64_t>(t.freed.size());
    const ApfsIpRotation rotation = computeIpRotation(ctx.liveCib, ctx.liveBitmap, ctx.layout);
    const QVector<ApfsFreeQueueEntry> liveMainFq = parseFreeQueueEntries(
        ctx.image,
        ctx.geometry,
        findEphemeralPaddrByOid(
            ctx.image, ctx.geometry, ctx.live, kApfsFormatFqMainTreeOid, blockers),
        blockers);
    const ApfsMainFqAdvance mainFq =
        advanceMainFreeQueue(liveMainFq, t.freed, t.newXid, kMainFqRollbackWindow);
    const int64_t netQueued = static_cast<int64_t>(t.freed.size()) -
                              static_cast<int64_t>(mainFq.reclaimed.size());
    const int64_t freeDelta = -netConsumed - netQueued;
    const uint64_t groupSize = ctx.layout.ipGroupStride;
    const uint64_t extraBitmaps = ctx.layout.metadataChunks > 1 ? ctx.layout.metadataChunks - 2 : 0;
    const uint64_t immutableCabCount = ctx.layout.cabCount > 0 ? ctx.layout.cabCount - 1 : 0;
    const uint64_t ipBitmapUsage = (ctx.layout.cibCount - 1) + immutableCabCount + extraBitmaps +
                                   3 * groupSize +
                                   (ctx.layout.allocChunk == 0 && t.chunk1BitmapBlock != 0 ? 1 : 0);
    if (!applyFileInsertAllocation({.image = ctx.image,
                                    .geometry = ctx.geometry,
                                    .layout = ctx.layout,
                                    .rotation = rotation,
                                    .freed = mainFq.reclaimed,
                                    .allocated = t.allocated,
                                    .cibFreeDelta = freeDelta,
                                    .newXid = t.newXid,
                                    .chunk1BitmapBlock = t.chunk1BitmapBlock},
                                   blockers)) {
        return false;
    }
    uint64_t newAddr0 = rotation.newCib;
    if (ctx.layout.cabCount > 0) {
        const uint64_t newCab0 = rotation.newCib + 2;
        if (!writeRotatedCab0({.image = ctx.image,
                               .geometry = ctx.geometry,
                               .liveCab0 = ctx.liveCab0,
                               .newCab0 = newCab0,
                               .newCib0 = rotation.newCib,
                               .newXid = t.newXid},
                              blockers)) {
            return false;
        }
        newAddr0 = newCab0;
    }
    return advanceCheckpoint({.image = ctx.image,
                              .geometry = ctx.geometry,
                              .nxsb = ctx.nxsb,
                              .live = ctx.live,
                              .newContainerOmap = t.containerOmapBlock,
                              .spacemanFreeDelta = freeDelta,
                              .nextOidAdvance = 0,
                              .newCibAddr = newAddr0,
                              .cibCount = ctx.layout.cibCount,
                              .ipSlotStride = groupSize,
                              .ipBitmapUsage = ipBitmapUsage,
                              .freedCibSlot = rotation.liveCib,
                              .prevFreedCibSlot = rotation.freeCib,
                              .mainFqEntries = mainFq.entries},
                             result,
                             blockers);
}

// A7 (A-g) resize: the number of blocks to grow a container to reach newSizeBytes, or 0
// (with a blocker) if the new size is not block-aligned or is not larger than the current
// size (shrink is a later increment).
uint64_t resolveResizeGrowDelta(QIODevice* image, uint64_t newSizeBytes, QStringList* blockers) {
    uint32_t blockSize = 0;
    uint64_t blockCount = 0;
    if (!readApfsRepairGeometry(image, &blockSize, &blockCount, blockers)) {
        return 0;
    }
    if (blockSize == 0 || newSizeBytes % blockSize != 0) {
        blockers->append(QStringLiteral(
            "APFS resize-commit: the new size must be a multiple of the block size"));
        return 0;
    }
    const uint64_t newBlockCount = newSizeBytes / blockSize;
    if (newBlockCount <= blockCount) {
        blockers->append(QStringLiteral(
            "APFS resize-commit: the new size must be larger than the current size (shrink is a "
            "later increment)"));
        return 0;
    }
    return newBlockCount - blockCount;
}

// A7 (A-g) container grow: extend a generated container in place by growDeltaBlocks with
// a true crash-safe checkpoint commit. Bounded to an IN-CHUNK grow of a single-chunk
// container -- the device gains blocks inside its one existing chunk, so no chunk is
// added and the spaceman's internal-pool region does not reshape (a chunk-adding grow
// relocates every metadata block, a later increment). The grow rotates the cib/bitmap
// like every commit, but instead of allocating/freeing it raises chunk 0's block + free
// counts, the spaceman main device's block + free counts, and nx_block_count by the same
// delta; the new blocks are already free (0) in the carried-forward chunk bitmap. No
// file-system metadata moves.
// Validates the resize-grow request, loads the commit context into *ctx, enforces
// the single-chunk in-chunk-grow constraint, and grows the backing image/device.
// Byte-identical to the inline preflight it replaces.
// Either grow the backing image to newSizeBytes, or (raw target) verify the already-sized
// device spans it -- a block device / partition cannot itself be resized.
bool sizeOrVerifyResizeTarget(QIODevice* image,
                              uint64_t newSizeBytes,
                              bool deviceAlreadySized,
                              QStringList* blockers) {
    if (deviceAlreadySized) {
        const qint64 deviceSize = image->size();
        if (deviceSize > 0 && static_cast<uint64_t>(deviceSize) < newSizeBytes) {
            blockers->append(QStringLiteral(
                "APFS resize-grow: the raw device is smaller than the requested new size"));
            return false;
        }
        return true;
    }
    auto* fileDevice = qobject_cast<QFileDevice*>(image);
    if (fileDevice == nullptr || !fileDevice->resize(static_cast<qint64>(newSizeBytes))) {
        blockers->append(
            QStringLiteral("APFS resize-grow: unable to grow the backing image/device"));
        return false;
    }
    return true;
}

bool preflightResizeGrow(QIODevice* image,
                         uint64_t growDeltaBlocks,
                         bool deviceAlreadySized,
                         ApfsFsCommitContext* ctx,
                         QStringList* blockers) {
    if (growDeltaBlocks == 0) {
        blockers->append(QStringLiteral("APFS resize-grow: the grow amount must be non-zero"));
        return false;
    }
    if (!loadFsCommitContext(image, ctx, blockers)) {
        return false;
    }
    if (ctx->layout.cibCount != 1 || ctx->layout.cabCount != 0 || ctx->layout.allocChunk != 0) {
        blockers->append(QStringLiteral(
            "APFS resize-grow: only an in-chunk grow of a single-chunk container is supported in "
            "this increment (a chunk-adding grow reshapes the spaceman internal pool)"));
        return false;
    }
    const uint64_t oldBlockCount = ctx->geometry.blockCount;
    if (oldBlockCount + growDeltaBlocks > kApfsSpacemanBlocksPerChunk) {
        blockers->append(
            QStringLiteral("APFS resize-grow: the new size would add a chunk (exceeds the single "
                           "chunk's %1-block "
                           "capacity); only an in-chunk grow is supported in this increment")
                .arg(kApfsSpacemanBlocksPerChunk));
        return false;
    }
    const uint64_t newSizeBytes = (oldBlockCount + growDeltaBlocks) * ctx->geometry.blockSize;
    return sizeOrVerifyResizeTarget(image, newSizeBytes, deviceAlreadySized, blockers);
}

bool commitInPlaceResizeGrow(QIODevice* image,
                             uint64_t growDeltaBlocks,
                             bool deviceAlreadySized,
                             ApfsInPlaceCheckpointResult* result,
                             QStringList* blockers) {
    ApfsFsCommitContext ctx;
    if (!preflightResizeGrow(image, growDeltaBlocks, deviceAlreadySized, &ctx, blockers)) {
        return false;
    }
    const uint64_t newXid = ctx.live.xid + 1;
    const int64_t growDelta = static_cast<int64_t>(growDeltaBlocks);
    const ApfsIpRotation rotation = computeIpRotation(ctx.liveCib, ctx.liveBitmap, ctx.layout);
    const uint64_t groupSize = ctx.layout.ipGroupStride;
    // Single-chunk, single-CIB: the IP bitmap marks the three rotation-group slots only.
    const uint64_t ipBitmapUsage = (ctx.layout.cibCount - 1) + 3 * groupSize;
    // No blocks are allocated or freed; the cib's chunk 0 simply gains growDelta blocks,
    // all free. cibFreeDelta/spacemanFreeDelta raise the free counts, the *BlockCountDelta
    // fields raise the block counts.
    if (!applyFileInsertAllocation({.image = ctx.image,
                                    .geometry = ctx.geometry,
                                    .layout = ctx.layout,
                                    .rotation = rotation,
                                    .freed = {},
                                    .allocated = {},
                                    .cibFreeDelta = growDelta,
                                    .newXid = newXid,
                                    .chunk1BitmapBlock = 0,
                                    .chunk0BlockCountDelta = growDelta},
                                   blockers)) {
        return false;
    }
    return advanceCheckpoint({.image = ctx.image,
                              .geometry = ctx.geometry,
                              .nxsb = ctx.nxsb,
                              .live = ctx.live,
                              .newContainerOmap = 0,
                              .spacemanFreeDelta = growDelta,
                              .nextOidAdvance = 0,
                              .newCibAddr = rotation.newCib,
                              .cibCount = ctx.layout.cibCount,
                              .ipSlotStride = groupSize,
                              .ipBitmapUsage = ipBitmapUsage,
                              .freedCibSlot = rotation.liveCib,
                              .prevFreedCibSlot = rotation.freeCib,
                              .blockCountDelta = growDelta},
                             result,
                             blockers);
}

// Snapshot-create commit tail: write the COW chain (eight new blocks), then advance the
// checkpoint. Five old chain blocks are freed (old volume superblock, volume omap header,
// snap-meta tree, container omap tree + header); the root fs-tree, volume omap tree, and
// live extent-ref tree stay in place, shared with the snapshot.
bool finalizeSnapshotCreate(const ApfsSnapshotCreateFinalize& f,
                            ApfsInPlaceCheckpointResult* result,
                            QStringList* blockers) {
    const ApfsFsCommitContext& ctx = f.ctx;
    const uint64_t newXid = ctx.live.xid + 1;
    if (!writeSnapshotCreateCowChain({.image = ctx.image,
                                      .geometry = ctx.geometry,
                                      .newXid = newXid,
                                      .live = ctx.chain,
                                      .newBlocks = f.newBlocks,
                                      .request = f.request},
                                     blockers)) {
        return false;
    }
    const QVector<uint64_t> freed = {ctx.chain.volSb,
                                     ctx.chain.volOmapHdr,
                                     f.oldSnapMetaTree,
                                     ctx.chain.ctrOmapTree,
                                     ctx.chain.ctrOmapHdr};
    return commitSnapshotCheckpointTail({.ctx = ctx,
                                         .newXid = newXid,
                                         .freed = freed,
                                         .allocated = f.newBlocks,
                                         .containerOmapBlock = f.newBlocks.at(7),
                                         .chunk1BitmapBlock = f.chunk1BitmapBlock},
                                        result,
                                        blockers);
}

// A3: create one snapshot of a generated APFS volume with a true in-place
// copy-on-write checkpoint commit. Freezes the volume's current tree pointers into a
// physical superblock copy + frozen extent-ref tree, records the snapshot in a new
// omap-snapshot tree and the snap-meta tree (j_snap_metadata + j_snap_name), and
// re-points the volume omap header / superblock / container omap - all published
// atomically at the checkpoint. Fails closed on a volume that already carries a
// snapshot (multi-snapshot create is a later increment).
bool commitInPlaceSnapshotCreate(QIODevice* image,
                                 const ApfsSnapshotCreateRequest& request,
                                 ApfsInPlaceCheckpointResult* result,
                                 QStringList* blockers) {
    if (request.snapshotName.trimmed().isEmpty()) {
        blockers->append(QStringLiteral("APFS snapshot-create: the snapshot name is empty"));
        return false;
    }
    ApfsFsCommitContext ctx;
    if (!loadFsCommitContext(image, &ctx, blockers)) {
        return false;
    }
    QByteArray liveVol(ctx.geometry.blockSize, '\0');
    if (!readApfsRepairBlock(image, ctx.geometry, ctx.chain.volSb, &liveVol, blockers)) {
        return false;
    }
    if (le64(liveVol, kApfsVolumeNumSnapshotsOffset) != 0) {
        blockers->append(
            QStringLiteral("APFS snapshot-create: the volume already carries a snapshot "
                           "(multi-snapshot create is a later increment)"));
        return false;
    }
    const uint64_t oldSnapMetaTree = le64(liveVol, kApfsVolumeSnapMetaTreeOidOffset);
    QVector<uint64_t> newBlocks;
    uint64_t chunk1BitmapBlock = 0;
    if (!allocateFsCommitBlocks(ctx, {3, 0, 0}, &newBlocks, &chunk1BitmapBlock, blockers)) {
        return false;
    }
    ApfsSnapshotCreateRequest clean = request;
    clean.snapshotName = request.snapshotName.trimmed();
    return finalizeSnapshotCreate({.ctx = ctx,
                                   .request = clean,
                                   .oldSnapMetaTree = oldSnapMetaTree,
                                   .newBlocks = newBlocks,
                                   .chunk1BitmapBlock = chunk1BitmapBlock},
                                  result,
                                  blockers);
}

// A snapshot's frozen block addresses, recovered from the snap-meta tree so a delete
// can free them.
struct ApfsSnapshotFrozenRefs {
    uint64_t sblockOid{0};
    uint64_t extentRefOid{0};
    bool found{false};
};

// Read the j_snap_metadata record from the snap-meta tree and recover the snapshot's
// frozen superblock + extent-ref tree block addresses.
ApfsSnapshotFrozenRefs readSnapshotFrozenRefs(QIODevice* image,
                                              const ApfsRepairGeometry& geometry,
                                              uint64_t snapMetaTree,
                                              QStringList* blockers) {
    ApfsSnapshotFrozenRefs refs;
    QByteArray node(geometry.blockSize, '\0');
    if (snapMetaTree == 0 || !readApfsRepairBlock(image, geometry, snapMetaTree, &node, blockers)) {
        return refs;
    }
    const uint32_t nkeys = le32(node, kApfsBtreeNodeCountOffset);
    const qsizetype keyArea = kApfsBtreeNodeHeaderBytes +
                              le16(node, kApfsBtreeNodeTableLengthOffset);
    const qsizetype valArea = static_cast<qsizetype>(geometry.blockSize) - kApfsBtreeInfoBytes;
    for (uint32_t index = 0; index < nkeys; ++index) {
        const qsizetype toc = kApfsBtreeNodeHeaderBytes + index * kApfsBtreeVariableTocEntryBytes;
        const uint64_t keyHdr = le64(node, keyArea + le16(node, toc));
        if ((keyHdr >> kApfsObjTypeShift) != kApfsJObjTypeSnapMetadata) {
            continue;
        }
        const qsizetype val = valArea - le16(node, toc + kApfsBtreeVariableTocValueOffset);
        refs.extentRefOid = le64(node, val + kApfsSnapMetaExtentRefOidOffset);
        refs.sblockOid = le64(node, val + kApfsSnapMetaSblockOidOffset);
        refs.found = true;
        return refs;
    }
    return refs;
}

// COW inputs for a snapshot delete. newBlocks layout (5 freshly allocated blocks):
//   [0]=snapMetaTree(empty) [1]=volOmapHdr [2]=volSb [3]=ctrOmapTree [4]=ctrOmapHdr
struct ApfsSnapshotDeleteCow {
    QIODevice* image{nullptr};
    ApfsRepairGeometry geometry;
    uint64_t newXid{0};
    ApfsLiveFsChain live;
    QVector<uint64_t> newBlocks;
    // The deleted snapshot owned the volume's blocks via its extent-ref tree (the
    // original KIND_NEW tree); when the last snapshot goes, the live volume re-adopts
    // it (the live extent-ref tree had been the empty post-snapshot one). 0 = none.
    uint64_t snapshotExtentRef{0};
};

// Copy-on-write chain for deleting the last snapshot: rewrite the snap-meta tree empty,
// clear the volume omap header's snapshot state (count/tree/most-recent all back to the
// snapshot-free zero), drop num_snapshots and restore the logical alloc count, and
// re-point the container omap. The frozen superblock, frozen extent-ref tree, and
// omap-snapshot tree are freed by the finalize step (they are snapshot-exclusive).
bool writeSnapshotDeleteCowChain(const ApfsSnapshotDeleteCow& cow, QStringList* blockers) {
    const uint32_t bs = cow.geometry.blockSize;
    const uint64_t snapMetaTree = cow.newBlocks.at(0);
    const uint64_t volOmapHdr = cow.newBlocks.at(1);
    const uint64_t volSb = cow.newBlocks.at(2);
    const uint64_t ctrOmapTree = cow.newBlocks.at(3);
    const uint64_t ctrOmapHdr = cow.newBlocks.at(4);
    const uint64_t xid = cow.newXid;

    QByteArray liveVol(bs, '\0');
    if (!readApfsRepairBlock(cow.image, cow.geometry, cow.live.volSb, &liveVol, blockers)) {
        return false;
    }
    const uint64_t curAlloc = le64(liveVol, kApfsVolumeAllocatedBlockCountOffset);
    // Inverse of create's +kApfsSnapshotAllocDelta (single snapshot, single-node trees).
    const uint64_t newAlloc = curAlloc - kApfsSnapshotAllocDelta;

    QByteArray emptyMeta =
        buildVariableKvLeafBlock({bs, snapMetaTree, xid, kApfsObjectSubtypeSnapMeta, {}}, blockers);
    if (!writeApfsRepairBlock(cow.image, cow.geometry, snapMetaTree, emptyMeta, blockers)) {
        return false;
    }
    QByteArray volOmap(bs, '\0');
    if (!readApfsRepairBlock(cow.image, cow.geometry, cow.live.volOmapHdr, &volOmap, blockers)) {
        return false;
    }
    writeLe64(&volOmap, kApfsObjectOidOffset, volOmapHdr);
    writeLe64(&volOmap, kApfsObjectXidOffset, xid);
    writeLe32(&volOmap,
              kApfsOmapSnapshotCountOffset,
              le32(volOmap, kApfsOmapSnapshotCountOffset) - 1);
    writeLe64(&volOmap, kApfsOmapSnapshotTreeOidOffset, 0);
    writeLe64(&volOmap, kApfsOmapMostRecentSnapshotOffset, 0);
    if (!stampAndWriteApfsBlock(cow.image, cow.geometry, volOmapHdr, &volOmap, blockers)) {
        return false;
    }
    QByteArray vol = liveVol;
    writeLe64(&vol, kApfsObjectXidOffset, xid);
    writeLe64(&vol, kApfsVolumeOmapOidOffset, volOmapHdr);
    // The live volume re-adopts the (formerly snapshot-owned) extent-ref tree with
    // its KIND_NEW records, so deleting the snapshot does not orphan the file blocks.
    if (cow.snapshotExtentRef != 0) {
        writeLe64(&vol, kApfsVolumeExtentRefTreeOidOffset, cow.snapshotExtentRef);
    }
    writeLe64(&vol, kApfsVolumeSnapMetaTreeOidOffset, snapMetaTree);
    writeLe64(&vol, kApfsVolumeNumSnapshotsOffset, le64(vol, kApfsVolumeNumSnapshotsOffset) - 1);
    writeLe64(&vol, kApfsVolumeAllocatedBlockCountOffset, newAlloc);
    if (!stampAndWriteApfsBlock(cow.image, cow.geometry, volSb, &vol, blockers)) {
        return false;
    }
    QByteArray ctrTree = buildContainerOmapTreeBlock(bs,
                                                     ctrOmapTree,
                                                     {kApfsFormatVolumeOid, xid, volSb, 0},
                                                     cow.live.containerOmapOthers,
                                                     blockers);
    QByteArray ctrHdr = buildObjectMapBlock({bs, ctrOmapHdr, ctrOmapTree, xid, 1}, blockers);
    return writeApfsRepairBlock(cow.image, cow.geometry, ctrOmapTree, ctrTree, blockers) &&
           writeApfsRepairBlock(cow.image, cow.geometry, ctrOmapHdr, ctrHdr, blockers);
}

// Inputs to the snapshot-delete commit tail.
struct ApfsSnapshotDeleteFinalize {
    ApfsFsCommitContext ctx;
    uint64_t oldSnapMetaTree{0};
    uint64_t omapSnapTree{0};       // live om_snapshot_tree_oid (freed)
    ApfsSnapshotFrozenRefs frozen;  // frozen superblock + extent-ref tree (freed)
    QVector<uint64_t> newBlocks;
    uint64_t chunk1BitmapBlock{0};
};

// Snapshot-delete commit tail: write the COW chain (five new blocks), then advance the
// checkpoint. Eight blocks are freed - the five COW'd chain blocks (old volume
// superblock, volume omap header, snap-meta tree, container omap tree + header) plus the
// three snapshot-exclusive blocks (omap-snapshot tree, frozen superblock, frozen
// extent-ref tree); the shared root fs-tree, volume omap tree, and live extent-ref tree
// stay in place.
bool finalizeSnapshotDelete(const ApfsSnapshotDeleteFinalize& f,
                            ApfsInPlaceCheckpointResult* result,
                            QStringList* blockers) {
    const ApfsFsCommitContext& ctx = f.ctx;
    const uint64_t newXid = ctx.live.xid + 1;
    if (!writeSnapshotDeleteCowChain({.image = ctx.image,
                                      .geometry = ctx.geometry,
                                      .newXid = newXid,
                                      .live = ctx.chain,
                                      .newBlocks = f.newBlocks,
                                      .snapshotExtentRef = f.frozen.extentRefOid},
                                     blockers)) {
        return false;
    }
    // The snapshot's extent-ref tree (f.frozen.extentRefOid) is NOT freed - the live
    // volume re-adopts it; the now-orphaned empty post-snapshot live tree is freed.
    const QVector<uint64_t> freed = {ctx.chain.volSb,
                                     ctx.chain.volOmapHdr,
                                     f.oldSnapMetaTree,
                                     ctx.chain.ctrOmapTree,
                                     ctx.chain.ctrOmapHdr,
                                     f.omapSnapTree,
                                     f.frozen.sblockOid,
                                     ctx.chain.extentRef};
    return commitSnapshotCheckpointTail({.ctx = ctx,
                                         .newXid = newXid,
                                         .freed = freed,
                                         .allocated = f.newBlocks,
                                         .containerOmapBlock = f.newBlocks.at(4),
                                         .chunk1BitmapBlock = f.chunk1BitmapBlock},
                                        result,
                                        blockers);
}

// A3: delete the (single) snapshot of a generated APFS volume with a true in-place
// copy-on-write checkpoint commit. Strips the j_snap_metadata + j_snap_name records and
// the omap-snapshot entry, frees the snapshot-exclusive frozen blocks, and restores the
// volume to its snapshot-free state. Fails closed unless the volume carries exactly one
// snapshot (multi-snapshot delete is a later increment).
bool commitInPlaceSnapshotDelete(QIODevice* image,
                                 ApfsInPlaceCheckpointResult* result,
                                 QStringList* blockers) {
    ApfsFsCommitContext ctx;
    if (!loadFsCommitContext(image, &ctx, blockers)) {
        return false;
    }
    QByteArray liveVol(ctx.geometry.blockSize, '\0');
    if (!readApfsRepairBlock(image, ctx.geometry, ctx.chain.volSb, &liveVol, blockers)) {
        return false;
    }
    if (le64(liveVol, kApfsVolumeNumSnapshotsOffset) != 1) {
        blockers->append(
            QStringLiteral("APFS snapshot-delete: the volume must carry exactly one snapshot "
                           "(multi-snapshot delete is a later increment)"));
        return false;
    }
    const uint64_t oldSnapMetaTree = le64(liveVol, kApfsVolumeSnapMetaTreeOidOffset);
    QByteArray liveOmap(ctx.geometry.blockSize, '\0');
    if (!readApfsRepairBlock(image, ctx.geometry, ctx.chain.volOmapHdr, &liveOmap, blockers)) {
        return false;
    }
    const uint64_t omapSnapTree = le64(liveOmap, kApfsOmapSnapshotTreeOidOffset);
    const ApfsSnapshotFrozenRefs frozen =
        readSnapshotFrozenRefs(image, ctx.geometry, oldSnapMetaTree, blockers);
    if (!frozen.found || frozen.sblockOid == 0 || omapSnapTree == 0) {
        blockers->append(
            QStringLiteral("APFS snapshot-delete: could not recover the snapshot's frozen blocks"));
        return false;
    }
    QVector<uint64_t> newBlocks;
    uint64_t chunk1BitmapBlock = 0;
    if (!allocateFsCommitBlocks(ctx, {0, 0, 0}, &newBlocks, &chunk1BitmapBlock, blockers)) {
        return false;
    }
    return finalizeSnapshotDelete({.ctx = ctx,
                                   .oldSnapMetaTree = oldSnapMetaTree,
                                   .omapSnapTree = omapSnapTree,
                                   .frozen = frozen,
                                   .newBlocks = newBlocks,
                                   .chunk1BitmapBlock = chunk1BitmapBlock},
                                  result,
                                  blockers);
}

// COW inputs for a snapshot revert. newBlocks layout (5 freshly allocated blocks):
//   [0]=snapMetaTree(re-emitted identical) [1]=volOmapHdr [2]=volSb [3]=ctrOmapTree [4]=ctrOmapHdr
struct ApfsSnapshotRevertCow {
    QIODevice* image{nullptr};
    ApfsRepairGeometry geometry;
    uint64_t newXid{0};
    ApfsLiveFsChain live;
    QVector<uint64_t> newBlocks;
    uint64_t snapshotXid{0};      // the snapshot to revert to (== om_most_recent_snap)
    uint64_t frozenSblockOid{0};  // the snapshot's frozen physical-superblock paddr
    uint64_t oldSnapMetaTree{0};
};

// Copy-on-write chain for a snapshot revert: write the deferred-revert TAG that Apple's
// fs_snapshot op 5 lays down. The live volume superblock is COW'd with two fields set -
// revert_to_xid (the snapshot's xid) and revert_to_sblock_oid (its frozen superblock
// paddr) - and EVERYTHING else (omap, root tree, extent-ref, snap-meta, allocated count,
// num_snapshots, next_obj_id) left identical: the snapshot is kept, the divergence is
// discarded by the kernel when it completes the revert on the next mount. The snap-meta
// tree and volume omap header are re-emitted byte-identical (the revert does not change
// their content); only their block xid moves with the new checkpoint. Byte recipe
// harvested from a real macOS revert (temp/a3cert/revert-recipe-decoded.txt).
bool writeSnapshotRevertCowChain(const ApfsSnapshotRevertCow& cow, QStringList* blockers) {
    const uint32_t bs = cow.geometry.blockSize;
    const uint64_t snapMetaTree = cow.newBlocks.at(0);
    const uint64_t volOmapHdr = cow.newBlocks.at(1);
    const uint64_t volSb = cow.newBlocks.at(2);
    const uint64_t ctrOmapTree = cow.newBlocks.at(3);
    const uint64_t ctrOmapHdr = cow.newBlocks.at(4);
    const uint64_t xid = cow.newXid;

    // (A) snap-meta tree: re-emit identical - a revert keeps the snapshot.
    QByteArray snapMeta(bs, '\0');
    if (!readApfsRepairBlock(cow.image, cow.geometry, cow.oldSnapMetaTree, &snapMeta, blockers)) {
        return false;
    }
    writeLe64(&snapMeta, kApfsObjectOidOffset, snapMetaTree);
    writeLe64(&snapMeta, kApfsObjectXidOffset, xid);
    if (!stampAndWriteApfsBlock(cow.image, cow.geometry, snapMetaTree, &snapMeta, blockers)) {
        return false;
    }
    // (B) volume omap header: COW identical, keep all snapshot state.
    QByteArray volOmap(bs, '\0');
    if (!readApfsRepairBlock(cow.image, cow.geometry, cow.live.volOmapHdr, &volOmap, blockers)) {
        return false;
    }
    writeLe64(&volOmap, kApfsObjectOidOffset, volOmapHdr);
    writeLe64(&volOmap, kApfsObjectXidOffset, xid);
    if (!stampAndWriteApfsBlock(cow.image, cow.geometry, volOmapHdr, &volOmap, blockers)) {
        return false;
    }
    // (C) volume superblock: COW the live APSB, set the deferred-revert tag; re-point its
    // object map + snap-meta tree at the COW'd blocks; leave alloc/num_snapshots/next_obj_id
    // and the root + extent-ref pointers untouched.
    QByteArray vol(bs, '\0');
    if (!readApfsRepairBlock(cow.image, cow.geometry, cow.live.volSb, &vol, blockers)) {
        return false;
    }
    writeLe64(&vol, kApfsObjectXidOffset, xid);
    writeLe64(&vol, kApfsVolumeOmapOidOffset, volOmapHdr);
    writeLe64(&vol, kApfsVolumeSnapMetaTreeOidOffset, snapMetaTree);
    writeLe64(&vol, kApfsVolumeRevertToXidOffset, cow.snapshotXid);
    writeLe64(&vol, kApfsVolumeRevertToSblockOidOffset, cow.frozenSblockOid);
    if (!stampAndWriteApfsBlock(cow.image, cow.geometry, volSb, &vol, blockers)) {
        return false;
    }
    // (D) container object map (tree + header) re-pointed at the new volume superblock.
    QByteArray ctrTree = buildContainerOmapTreeBlock(bs,
                                                     ctrOmapTree,
                                                     {kApfsFormatVolumeOid, xid, volSb, 0},
                                                     cow.live.containerOmapOthers,
                                                     blockers);
    QByteArray ctrHdr = buildObjectMapBlock({bs, ctrOmapHdr, ctrOmapTree, xid, 1}, blockers);
    return writeApfsRepairBlock(cow.image, cow.geometry, ctrOmapTree, ctrTree, blockers) &&
           writeApfsRepairBlock(cow.image, cow.geometry, ctrOmapHdr, ctrHdr, blockers);
}

// Inputs to the snapshot-revert commit tail.
struct ApfsSnapshotRevertFinalize {
    ApfsFsCommitContext ctx;
    uint64_t snapshotXid{0};
    uint64_t frozenSblockOid{0};
    uint64_t oldSnapMetaTree{0};
    QVector<uint64_t> newBlocks;
    uint64_t chunk1BitmapBlock{0};
};

// Snapshot-revert commit tail: write the COW chain (five new blocks), then advance the
// checkpoint. Five old chain blocks are freed (old volume superblock, volume omap header,
// snap-meta tree, container omap tree + header); the snapshot-exclusive blocks (frozen
// superblock, frozen extent-ref tree, omap-snapshot tree) are NOT freed - the snapshot
// survives a revert. Net block change is zero (alloc count unchanged).
bool finalizeSnapshotRevert(const ApfsSnapshotRevertFinalize& f,
                            ApfsInPlaceCheckpointResult* result,
                            QStringList* blockers) {
    const ApfsFsCommitContext& ctx = f.ctx;
    const uint64_t newXid = ctx.live.xid + 1;
    if (!writeSnapshotRevertCowChain({.image = ctx.image,
                                      .geometry = ctx.geometry,
                                      .newXid = newXid,
                                      .live = ctx.chain,
                                      .newBlocks = f.newBlocks,
                                      .snapshotXid = f.snapshotXid,
                                      .frozenSblockOid = f.frozenSblockOid,
                                      .oldSnapMetaTree = f.oldSnapMetaTree},
                                     blockers)) {
        return false;
    }
    const QVector<uint64_t> freed = {ctx.chain.volSb,
                                     ctx.chain.volOmapHdr,
                                     f.oldSnapMetaTree,
                                     ctx.chain.ctrOmapTree,
                                     ctx.chain.ctrOmapHdr};
    return commitSnapshotCheckpointTail({.ctx = ctx,
                                         .newXid = newXid,
                                         .freed = freed,
                                         .allocated = f.newBlocks,
                                         .containerOmapBlock = f.newBlocks.at(4),
                                         .chunk1BitmapBlock = f.chunk1BitmapBlock},
                                        result,
                                        blockers);
}

// A3: revert a generated APFS volume to its (single) snapshot with a true in-place
// copy-on-write checkpoint commit. Writes the deferred-revert tag exactly as Apple's
// fs_snapshot op 5 does - sets revert_to_xid + revert_to_sblock_oid on a COW'd volume
// superblock and leaves the rest untouched - so a kernel mount completes the revert
// (discarding the post-snapshot divergence) and fsck_apfs validates it. Fails closed
// unless the volume carries exactly one snapshot and no revert is already pending.
bool commitInPlaceSnapshotRevert(QIODevice* image,
                                 ApfsInPlaceCheckpointResult* result,
                                 QStringList* blockers) {
    ApfsFsCommitContext ctx;
    if (!loadFsCommitContext(image, &ctx, blockers)) {
        return false;
    }
    QByteArray liveVol(ctx.geometry.blockSize, '\0');
    if (!readApfsRepairBlock(image, ctx.geometry, ctx.chain.volSb, &liveVol, blockers)) {
        return false;
    }
    if (le64(liveVol, kApfsVolumeNumSnapshotsOffset) != 1) {
        blockers->append(
            QStringLiteral("APFS snapshot-revert: the volume must carry exactly one snapshot "
                           "(multi-snapshot revert is a later increment)"));
        return false;
    }
    if (le64(liveVol, kApfsVolumeRevertToXidOffset) != 0) {
        blockers->append(
            QStringLiteral("APFS snapshot-revert: a revert is already pending on this volume"));
        return false;
    }
    const uint64_t oldSnapMetaTree = le64(liveVol, kApfsVolumeSnapMetaTreeOidOffset);
    QByteArray liveOmap(ctx.geometry.blockSize, '\0');
    if (!readApfsRepairBlock(image, ctx.geometry, ctx.chain.volOmapHdr, &liveOmap, blockers)) {
        return false;
    }
    const uint64_t snapshotXid = le64(liveOmap, kApfsOmapMostRecentSnapshotOffset);
    const ApfsSnapshotFrozenRefs frozen =
        readSnapshotFrozenRefs(image, ctx.geometry, oldSnapMetaTree, blockers);
    if (!frozen.found || frozen.sblockOid == 0 || snapshotXid == 0) {
        blockers->append(
            QStringLiteral("APFS snapshot-revert: could not recover the snapshot to revert to"));
        return false;
    }
    QVector<uint64_t> newBlocks;
    uint64_t chunk1BitmapBlock = 0;
    if (!allocateFsCommitBlocks(ctx, {0, 0, 0}, &newBlocks, &chunk1BitmapBlock, blockers)) {
        return false;
    }
    return finalizeSnapshotRevert({.ctx = ctx,
                                   .snapshotXid = snapshotXid,
                                   .frozenSblockOid = frozen.sblockOid,
                                   .oldSnapMetaTree = oldSnapMetaTree,
                                   .newBlocks = newBlocks,
                                   .chunk1BitmapBlock = chunk1BitmapBlock},
                                  result,
                                  blockers);
}

struct ApfsDeleteRequest {
    QVector<ApfsRootFilePayload> allFiles;
    QVector<ApfsRootDirectoryPayload> directories;  // preserved across the commit
    QString targetName;
    uint64_t targetParentId{kApfsRootDirectoryId};  // root for a root file, else a directory id
};

// A2: delete one file from a generated container with a true in-place
// copy-on-write checkpoint commit, preserving the other files (and any
// directories and their children). COW the fs-metadata chain (and the extent-ref
// tree, when the deleted file had data) to newly allocated blocks with the
// target's records removed, free the target's data blocks and the old fs-tree/chain
// blocks, return the freed blocks to the spaceman/CIB free counts, then advance the
// checkpoint.
bool commitInPlaceFileDelete(QIODevice* image,
                             const ApfsDeleteRequest& request,
                             ApfsInPlaceCheckpointResult* result,
                             QStringList* blockers) {
    ApfsFsCommitContext ctx;
    if (!loadFsCommitContext(image, &ctx, blockers)) {
        return false;
    }
    QVector<ApfsRootFilePayload> remaining;
    ApfsRootFilePayload target;
    if (!buildDeleteFileList({ctx.image,
                              ctx.geometry,
                              ctx.chain,
                              request.allFiles,
                              request.targetName,
                              request.targetParentId},
                             &remaining,
                             &target,
                             blockers)) {
        return false;
    }
    const uint64_t targetBlocks = roundedBlockCount(static_cast<uint64_t>(target.data.size()),
                                                    ctx.geometry.blockSize);
    // Removing a file with data rewrites the extent-ref tree (one extra block);
    // deleting an empty file leaves the extent-ref tree in place.
    const int extentRefSlots = static_cast<int>(targetBlocks > 0);
    QVector<ApfsFsTreeNode> fsNodes;
    if (!buildFsTreeNodes(
            {ctx.geometry.blockSize, remaining, request.directories, ctx.firstLeafOid},
            &fsNodes,
            blockers)) {
        return false;
    }
    const qsizetype nodeCount = fsNodes.size();
    QVector<uint64_t> newBlocks;
    uint64_t deleteChunk1Bitmap = 0;
    if (!allocateFsCommitBlocks(
            ctx, {nodeCount, extentRefSlots, 0}, &newBlocks, &deleteChunk1Bitmap, blockers)) {
        return false;
    }
    const uint64_t extentRefNew = extentRefSlots != 0 ? newBlocks.value(nodeCount + 5) : 0;
    return finalizeFsCommit({.ctx = ctx,
                             .newXid = ctx.live.xid + 1,
                             .fsNodes = fsNodes,
                             .newBlocks = newBlocks,
                             .files = remaining,
                             .extentRefNew = extentRefNew,
                             .freedDataBlocks = fileFreedDataBlocks(target, ctx.geometry.blockSize),
                             .dataBlocksNew = 0,
                             .fileCountDelta = -1,
                             .nextObjIdDelta = 0},
                            result,
                            blockers);
}

struct ApfsRenameListInput {
    QIODevice* image{nullptr};
    ApfsRepairGeometry geometry;
    ApfsLiveFsChain chain;
    QVector<ApfsRootFilePayload> allFiles;
    QString oldName;
    QString newName;
    uint64_t targetParentId{kApfsRootDirectoryId};  // source parent (root, or a directory id)
    uint64_t destinationParentId{0};  // move target parent; 0 = same parent (rename, not move)
};

// Build the root-file list with one file renamed or moved (data extents + object id
// preserved; the rebuilt fs-tree re-sorts dirents and recomputes parent valences). Fails
// closed if the source is missing or the destination name already exists in its parent.
// Restore a file's private id + data extents (the rename/move keeps them in place).
void restoreRenamePayload(ApfsRootFilePayload* file,
                          const ApfsRenameListInput& in,
                          const QHash<uint64_t, uint64_t>& owners,
                          QStringList* blockers) {
    file->privateId = file->fileId;
    file->dataStartBlock = owners.value(file->fileId);
    const QVector<ApfsDataExtent> recovered =
        recoverFileDataExtents(in.image, in.geometry, in.chain, file->fileId, blockers);
    if (recovered.size() > 1) {
        file->dataExtents = recovered;
    }
}

bool buildRenameFileList(const ApfsRenameListInput& in,
                         QVector<ApfsRootFilePayload>* files,
                         QStringList* blockers) {
    const QHash<uint64_t, uint64_t> owners =
        parseExtentRefOwners(in.image, in.geometry, in.chain.extentRef, blockers);
    const uint64_t destParent = in.destinationParentId != 0 ? in.destinationParentId
                                                            : in.targetParentId;
    bool found = false;
    bool duplicate = false;
    for (ApfsRootFilePayload file : in.allFiles) {
        // Keep each file's parent so the rest of the tree survives; the source is matched
        // in its parent and any name collision in the DESTINATION parent (excluding the
        // moved file itself), so a same-named file under a different parent is left alone.
        restoreRenamePayload(&file, in, owners, blockers);
        const bool isSource = file.parentDirectoryId == in.targetParentId &&
                              file.fileName == in.oldName;
        duplicate = duplicate || (!isSource && file.parentDirectoryId == destParent &&
                                  file.fileName == in.newName);
        if (isSource) {
            file.fileName = in.newName;
            file.parentDirectoryId = destParent;
            found = true;
        }
        files->append(file);
    }
    if (!found) {
        blockers->append(
            QStringLiteral("APFS file-rename-commit: file '%1' was not found").arg(in.oldName));
        return false;
    }
    if (duplicate) {
        blockers->append(QStringLiteral("APFS file-rename-commit: a file named '%1' already exists")
                             .arg(in.newName));
        return false;
    }
    return true;
}

struct ApfsRenameRequest {
    QVector<ApfsRootFilePayload> allFiles;
    QString oldName;
    QString newName;
    QVector<ApfsRootDirectoryPayload> directories;     // preserved across the commit
    uint64_t parentDirectoryId{kApfsRootDirectoryId};  // source parent (root, or a directory id)
    uint64_t destinationParentDirectoryId{0};  // move target; 0 = same parent (rename, not move)
};

// A2: rename one root file in place. The renamed dirent/inode are rebuilt into
// the copy-on-written fs-tree; the file keeps its object id and its data extent
// stays put, so this is a net-zero allocation when the tree shape is unchanged
// (extra/fewer fs-tree nodes thread through the free counts like any commit, no
// extent-ref change).
bool commitInPlaceFileRename(QIODevice* image,
                             const ApfsRenameRequest& request,
                             ApfsInPlaceCheckpointResult* result,
                             QStringList* blockers) {
    ApfsFsCommitContext ctx;
    if (!loadFsCommitContext(image, &ctx, blockers)) {
        return false;
    }
    QVector<ApfsRootFilePayload> files;
    if (!buildRenameFileList({ctx.image,
                              ctx.geometry,
                              ctx.chain,
                              request.allFiles,
                              request.oldName,
                              request.newName,
                              request.parentDirectoryId,
                              request.destinationParentDirectoryId},
                             &files,
                             blockers)) {
        return false;
    }
    QVector<ApfsFsTreeNode> fsNodes;
    if (!buildFsTreeNodes({ctx.geometry.blockSize, files, request.directories, ctx.firstLeafOid},
                          &fsNodes,
                          blockers)) {
        return false;
    }
    const qsizetype nodeCount = fsNodes.size();
    QVector<uint64_t> newBlocks;
    uint64_t renameChunk1Bitmap = 0;
    if (!allocateFsCommitBlocks(
            ctx, {nodeCount, 0, 0}, &newBlocks, &renameChunk1Bitmap, blockers)) {
        return false;
    }
    return finalizeFsCommit({.ctx = ctx,
                             .newXid = ctx.live.xid + 1,
                             .fsNodes = fsNodes,
                             .newBlocks = newBlocks,
                             .files = files,
                             .extentRefNew = 0,
                             .freedDataBlocks = {},
                             .dataBlocksNew = 0,
                             .fileCountDelta = 0,
                             .nextObjIdDelta = 0},
                            result,
                            blockers);
}

struct ApfsDirectoryCreateRequest {
    QVector<ApfsRootFilePayload> existingFiles;
    QVector<ApfsRootDirectoryPayload> existingDirectories;
    QString directoryName;
};

// A2-3.2: create one empty root directory with a true in-place copy-on-write commit.
// The new directory takes the volume's next object id and is appended to the preserved
// full tree (existing files and directories carried in place); the rebuilt fs-tree gains
// the directory inode + its dirent under root, the root directory's valence grows by one,
// and the volume directory count + next object id advance. No data blocks or extent-ref
// change. Mirrors the file-insert commit with a directory in place of the file.
bool commitInPlaceDirectoryCreate(QIODevice* image,
                                  const ApfsDirectoryCreateRequest& request,
                                  ApfsInPlaceCheckpointResult* result,
                                  QStringList* blockers) {
    ApfsFsCommitContext ctx;
    if (!loadFsCommitContext(image, &ctx, blockers)) {
        return false;
    }
    QByteArray volSb(ctx.geometry.blockSize, '\0');
    if (!readApfsRepairBlock(ctx.image, ctx.geometry, ctx.chain.volSb, &volSb, blockers)) {
        return false;
    }
    const uint64_t newDirId = le64(volSb, kApfsVolumeNextObjectIdOffset);
    QVector<ApfsRootFilePayload> files;
    if (!recoverPreservedFiles(
            {ctx.image, ctx.geometry, ctx.chain}, request.existingFiles, &files, blockers)) {
        return false;
    }
    QVector<ApfsRootDirectoryPayload> directories = request.existingDirectories;
    directories.append({.directoryName = request.directoryName.trimmed(),
                        .directoryId = newDirId,
                        .privateId = newDirId});
    QVector<ApfsFsTreeNode> fsNodes;
    if (!buildFsTreeNodes(
            {ctx.geometry.blockSize, files, directories, ctx.firstLeafOid}, &fsNodes, blockers)) {
        return false;
    }
    const qsizetype nodeCount = fsNodes.size();
    QVector<uint64_t> newBlocks;
    uint64_t directoryChunk1Bitmap = 0;
    if (!allocateFsCommitBlocks(
            ctx, {nodeCount, 0, 0}, &newBlocks, &directoryChunk1Bitmap, blockers)) {
        return false;
    }
    return finalizeFsCommit({.ctx = ctx,
                             .newXid = ctx.live.xid + 1,
                             .fsNodes = fsNodes,
                             .newBlocks = newBlocks,
                             .files = files,
                             .extentRefNew = 0,
                             .freedDataBlocks = {},
                             .dataBlocksNew = 0,
                             .fileCountDelta = 0,
                             .directoryCountDelta = 1,
                             .nextObjIdDelta = 1,
                             .chunk1BitmapBlock = directoryChunk1Bitmap},
                            result,
                            blockers);
}

// A2-3.2: delete one empty root directory with a true in-place copy-on-write commit.
// The directory is dropped from the preserved full tree, root's valence falls by one,
// and the volume directory count decreases. Fails closed if the directory is missing or
// still holds children (a non-empty directory is emptied through the child paths first).
bool commitInPlaceDirectoryDelete(QIODevice* image,
                                  const ApfsDirectoryCreateRequest& request,
                                  ApfsInPlaceCheckpointResult* result,
                                  QStringList* blockers) {
    ApfsFsCommitContext ctx;
    if (!loadFsCommitContext(image, &ctx, blockers)) {
        return false;
    }
    const QString name = request.directoryName.trimmed();
    uint64_t targetDirId = 0;
    QVector<ApfsRootDirectoryPayload> remainingDirectories;
    for (const auto& directory : request.existingDirectories) {
        if (directory.directoryName == name) {
            targetDirId = directory.directoryId;
        } else {
            remainingDirectories.append(directory);
        }
    }
    if (targetDirId == 0) {
        blockers->append(
            QStringLiteral("APFS directory-delete-commit: directory '%1' was not found").arg(name));
        return false;
    }
    for (const auto& file : request.existingFiles) {
        if (file.parentDirectoryId == targetDirId) {
            blockers->append(
                QStringLiteral("APFS directory-delete-commit: directory '%1' is not empty")
                    .arg(name));
            return false;
        }
    }
    QVector<ApfsRootFilePayload> files;
    if (!recoverPreservedFiles(
            {ctx.image, ctx.geometry, ctx.chain}, request.existingFiles, &files, blockers)) {
        return false;
    }
    QVector<ApfsFsTreeNode> fsNodes;
    if (!buildFsTreeNodes({ctx.geometry.blockSize, files, remainingDirectories, ctx.firstLeafOid},
                          &fsNodes,
                          blockers)) {
        return false;
    }
    const qsizetype nodeCount = fsNodes.size();
    QVector<uint64_t> newBlocks;
    uint64_t directoryChunk1Bitmap = 0;
    if (!allocateFsCommitBlocks(
            ctx, {nodeCount, 0, 0}, &newBlocks, &directoryChunk1Bitmap, blockers)) {
        return false;
    }
    return finalizeFsCommit({.ctx = ctx,
                             .newXid = ctx.live.xid + 1,
                             .fsNodes = fsNodes,
                             .newBlocks = newBlocks,
                             .files = files,
                             .extentRefNew = 0,
                             .freedDataBlocks = {},
                             .dataBlocksNew = 0,
                             .fileCountDelta = 0,
                             .directoryCountDelta = -1,
                             .nextObjIdDelta = 0,
                             .chunk1BitmapBlock = directoryChunk1Bitmap},
                            result,
                            blockers);
}

// A2-3: create-or-replace one root file with a true in-place copy-on-write commit
// (the production mutation route's file-write primitive). A new name is one insert
// commit; an existing name is a faithful replace - a delete commit (frees the old
// file's data and records) followed by an insert commit (adds the new payload) on the
// same device handle, each individually crash-safe and Apple-certified. The replace is
// two checkpoints; an interrupted replace leaves the file cleanly deleted, never
// corrupt.
struct ApfsRootFileWriteRequest {
    QVector<ApfsRootFilePayload> allFiles;  // every file on the device (root + children)
    QString fileName;
    QByteArray fileData;
    QVector<ApfsRootDirectoryPayload> directories;     // preserved across the commit
    uint64_t parentDirectoryId{kApfsRootDirectoryId};  // root for a root file, else a directory id
};

bool commitInPlaceRootFileWrite(QIODevice* image,
                                const ApfsRootFileWriteRequest& request,
                                ApfsInPlaceCheckpointResult* result,
                                QStringList* blockers) {
    const QString& fileName = request.fileName;
    const uint64_t parentId = request.parentDirectoryId;
    // The write target lives in one parent (root, or a directory); a same-named file
    // under a different parent does not collide with it (only the matching one replaced).
    const auto isReplacedFile = [&fileName, parentId](const ApfsRootFilePayload& file) {
        return file.parentDirectoryId == parentId && file.fileName == fileName;
    };
    const bool exists =
        std::any_of(request.allFiles.cbegin(), request.allFiles.cend(), isReplacedFile);
    QVector<ApfsRootFilePayload> existing;
    if (exists) {
        ApfsInPlaceCheckpointResult deleteResult;
        if (!commitInPlaceFileDelete(image,
                                     {request.allFiles, request.directories, fileName, parentId},
                                     &deleteResult,
                                     blockers)) {
            return false;
        }
        for (const ApfsRootFilePayload& file : request.allFiles) {
            if (!isReplacedFile(file)) {
                existing.append(file);
            }
        }
    } else {
        existing = request.allFiles;
    }
    return commitInPlaceFileInsert(
        image,
        {existing, fileName, request.fileData, request.directories, parentId},
        result,
        blockers);
}


// Enumerate the full one-level tree of a generated container - root regular files plus
// each root directory and the regular files it contains - so an in-place commit can
// round-trip directories and their children instead of dropping them. Files carry their
// parentDirectoryId (the root directory id, or the owning directory's object id);
// directories carry their object id. Nested subdirectories are not supported yet and
// fail closed. A flat-root container yields an empty directory list, byte-identical to
// the certified single-level layout.
bool collectFullFsTree(const QString& sourcePath,
                       QVector<ApfsRootFilePayload>* files,
                       QVector<ApfsRootDirectoryPayload>* directories,
                       QStringList* blockers) {
    const auto rootListing = PartitionApfsFileSystemReader::listDirectoryFromImage(
        sourcePath, QStringLiteral("/"), kApfsWriteRootListingMaxEntries);
    if (!rootListing.ok) {
        blockers->append(rootListing.blockers.value(
            0, QStringLiteral("APFS in-place commit: unable to read the existing tree")));
        return false;
    }
    for (const auto& entry : rootListing.entries) {
        if (entry.directory) {
            directories->append({.directoryName = entry.name,
                                 .directoryId = entry.object_id,
                                 .privateId = entry.object_id});
            const auto childListing = PartitionApfsFileSystemReader::listDirectoryFromImage(
                sourcePath, QStringLiteral("/") + entry.name, kApfsWriteRootListingMaxEntries);
            if (!childListing.ok) {
                blockers->append(childListing.blockers.value(
                    0,
                    QStringLiteral("APFS in-place commit: unable to read directory '%1'")
                        .arg(entry.name)));
                return false;
            }
            for (const auto& child : childListing.entries) {
                if (child.directory) {
                    blockers->append(
                        QStringLiteral("APFS in-place commit does not yet preserve nested "
                                       "subdirectories (in '%1')")
                            .arg(entry.name));
                    return false;
                }
                if (child.regular_file) {
                    files->append(
                        {.fileName = child.name,
                         .data = QByteArray(static_cast<qsizetype>(child.size_bytes), '\0'),
                         .parentDirectoryId = entry.object_id,
                         .fileId = child.object_id});
                }
            }
        } else if (entry.regular_file) {
            files->append({.fileName = entry.name,
                           .data = QByteArray(static_cast<qsizetype>(entry.size_bytes), '\0'),
                           .fileId = entry.object_id});
        }
    }
    return true;
}

// A7 (A-h) clone: the shared data stream id and logical size a clone inherits from its
// source file.
struct ApfsCloneSourceInfo {
    uint64_t privateId{0};
    uint64_t logicalSize{0};
};

// Locate the named root file in a collected tree and mark it as a clone source: its
// physical blocks gain a second extent-ref reference (refcount 2) and it gains
// WAS_EVER_CLONED. Returns the source id + logical size the clone inode reuses. Fails
// closed if the source is absent or zero-length (a zero-length file has no data to clone).
bool prepareCloneSource(QVector<ApfsRootFilePayload>* existingFiles,
                        const QString& sourceName,
                        ApfsCloneSourceInfo* out,
                        QStringList* blockers) {
    for (auto& file : *existingFiles) {
        if (file.parentDirectoryId != kApfsRootDirectoryId || file.fileName != sourceName) {
            continue;
        }
        if (file.data.isEmpty()) {
            blockers->append(QStringLiteral(
                "APFS file-clone-commit: cloning a zero-length source file is not supported"));
            return false;
        }
        file.extraInodeFlags |= kApfsInodeFlagWasEverCloned;
        *out = {file.fileId, static_cast<uint64_t>(file.data.size())};
        return true;
    }
    blockers->append(
        QStringLiteral("APFS file-clone-commit: source file '%1' not found in the container root")
            .arg(sourceName));
    return false;
}

// A7 (A-h) hard link: the object id of the named root file to link, or 0 (with a
// blocker) if it is absent.
uint64_t resolveHardlinkTargetId(const QVector<ApfsRootFilePayload>& existingFiles,
                                 const QString& sourceName,
                                 QStringList* blockers) {
    for (const auto& file : existingFiles) {
        if (file.parentDirectoryId == kApfsRootDirectoryId && file.fileName == sourceName) {
            return file.fileId;
        }
    }
    blockers->append(
        QStringLiteral(
            "APFS file-hardlink-commit: source file '%1' not found in the container root")
            .arg(sourceName));
    return 0;
}

// Full one-level tree for a chained insert; fails closed if the new root file name
// already exists as a root file or directory.
// The object id of the named root directory in a collected tree, or 0 if absent.
uint64_t resolveDirectoryId(const QVector<ApfsRootDirectoryPayload>& directories,
                            const QString& directoryName) {
    for (const auto& directory : directories) {
        if (directory.directoryName == directoryName) {
            return directory.directoryId;
        }
    }
    return 0;
}

// Resolve a parent component to its object id: an empty name is the container root, a
// non-empty name resolves to its directory id (0 if that directory is absent).
uint64_t resolveParentId(const QVector<ApfsRootDirectoryPayload>& directories,
                         const QString& directoryName) {
    return directoryName.isEmpty() ? kApfsRootDirectoryId
                                   : resolveDirectoryId(directories, directoryName);
}

bool collectExistingFullFsTree(const QString& sourcePath,
                               const QString& newFileName,
                               QVector<ApfsRootFilePayload>* files,
                               QVector<ApfsRootDirectoryPayload>* directories,
                               QStringList* blockers) {
    if (!collectFullFsTree(sourcePath, files, directories, blockers)) {
        return false;
    }
    for (const auto& file : *files) {
        if (file.parentDirectoryId == kApfsRootDirectoryId && file.fileName == newFileName) {
            blockers->append(
                QStringLiteral("APFS file-insert-commit: a file named '%1' already exists")
                    .arg(newFileName));
            return false;
        }
    }
    for (const auto& directory : *directories) {
        if (directory.directoryName == newFileName) {
            blockers->append(
                QStringLiteral("APFS file-insert-commit: a directory named '%1' already exists")
                    .arg(newFileName));
            return false;
        }
    }
    return true;
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

// Empty when S.A.K. can format a container of `blockCount` blocks, otherwise the
// blocker. S.A.K. emits single-CIB, multi-CIB (cab_count 0), the metadata-overflow
// tier (reserved prefix spans chunks, multi-block ip bitmaps), and the CAB tier
// (cab_count > 0, >~7.8 TiB, the spaceman publishes a cab-address array pointing at
// apfs_cib_addr_blocks). Two ceilings remain: the published address array must fit
// the spaceman block, and the live checkpoint's IP usage must fit a single chunk-0
// allocation bitmap (32768 blocks, ~553 TiB of cibs).
QString generatedApfsContainerFormatBlocker(QLatin1StringView purpose,
                                            uint64_t blockCount,
                                            uint32_t blockSize) {
    const PartitionApfsContainerGeometry geometry =
        PartitionApfsWriter::computeContainerGeometry(blockCount, blockSize);
    // The published address array (inline cibs when cab_count 0, else the cab addresses)
    // sits just past the ip-bitmap free-next ring (which grows with ip_bm_size). When it
    // overflows one block the spaceman object spills into more blocks (Apple/mkapfs size
    // the object to fit, since Apple forbids CAB for cib_count <= 507), so the real ceiling
    // is that the genesis + live spaceman objects and the live checkpoint's reaper + two
    // free-queue trees all fit the fixed checkpoint-data ring.
    const uint64_t ipBmSize =
        ipBitmapSizeBlocks(geometry.chunk_count, geometry.cib_count, geometry.cab_count);
    const uint64_t addrEntries = geometry.cab_count > 0 ? geometry.cab_count : geometry.cib_count;
    const uint64_t spacemanBlocks = spacemanBlockSpan(blockSize, ipBmSize, addrEntries);
    const uint64_t liveEphemeralEnd = kApfsFormatGenesisSpacemanBlock + 2 * spacemanBlocks + 3;
    const bool arrayFits = liveEphemeralEnd <
                           kApfsFormatCheckpointDataBaseBlock + kApfsFormatCheckpointDataBlocks;
    // The genesis/live IP usage bitmap is a single 4096-byte block; the live
    // checkpoint marks every internal-pool block it owns (immutable cibs + cabs +
    // metadata-chunk bitmaps + the six-block cib0/cab0 rotation), which must fit.
    const uint64_t seedData =
        generatedSeedDataBlock(geometry.chunk_count, geometry.cib_count, geometry.cab_count);
    const uint64_t metadataChunks = std::min<uint64_t>(
        geometry.chunk_count,
        (seedData + kApfsSpacemanBlocksPerChunk - 1) / kApfsSpacemanBlocksPerChunk);
    const uint64_t liveIpUsage = geometry.cib_count + geometry.cab_count + metadataChunks + 6;
    const bool ipUsageFits = liveIpUsage < kApfsSpacemanBlocksPerChunk;
    if (arrayFits && ipUsageFits) {
        return QString();
    }
    return QStringLiteral(
               "APFS %1 supports S.A.K.-generated containers up to the CAB tier whose published "
               "cib/cab-address array and single-block IP usage bitmap fit the spaceman (~553 "
               "TiB); "
               "this %2-block target needs %3 chunk(s) across %4 chunk-info block(s) and %5 "
               "CAB(s), "
               "which exceeds the certified ceiling")
        .arg(purpose)
        .arg(blockCount)
        .arg(geometry.chunk_count)
        .arg(geometry.cib_count)
        .arg(geometry.cab_count);
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
    const ApfsFormatEphemeralLayout ephemeral = apfsFormatEphemeralLayoutForBlockCount(
        context.geometry.blockSize, context.geometry.blockCount);
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
        {ephemeral.liveSpaceman, &blocks->spaceman},
        {ephemeral.liveReaper, &blocks->reaper},
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

// Validates the nxsb's checkpoint descriptor/data geometry, reaper OID, and
// ephemeral info. Byte-identical to the inline blocker sequence it replaces.
void appendGeneratedNxsbCheckpointBlockers(const GeneratedApfsLayoutContext& context,
                                           const GeneratedApfsLayoutBlocks& blocks,
                                           const ApfsFormatEphemeralLayout& ephemeral) {
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
            le32(blocks.nxsb, kApfsNxXpDataIndexOffset) == ephemeral.liveDataIndex &&
            le32(blocks.nxsb, kApfsNxXpDataLenOffset) == ephemeral.liveDataLen,
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
}

// Validates the checkpoint-map header/entries, reaper, and chunk-info blocks, plus
// the nxsb copy. Byte-identical to the inline blocker sequence it replaces.
void appendGeneratedCheckpointMapBlockers(const GeneratedApfsLayoutContext& context,
                                          const GeneratedApfsLayoutBlocks& blocks,
                                          const ApfsFormatEphemeralLayout& ephemeral) {
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
         .paddr = ephemeral.liveSpaceman},
        QStringLiteral("APFS generated/minimal checkpoint map must resolve the space manager"));
    appendCheckpointMapEntryBlocker(
        context,
        blocks.checkpointMap,
        entry + kApfsCheckpointMapEntryBytes,
        {.type = kApfsObjectTypeReaper, .oid = kApfsFormatReaperOid, .paddr = ephemeral.liveReaper},
        QStringLiteral("APFS generated/minimal checkpoint map must resolve the reaper"));
    appendGeneratedHeaderBlockers({.block = &blocks.reaper,
                                   .blockIndex = ephemeral.liveReaper,
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

void appendGeneratedCheckpointBlockers(const GeneratedApfsLayoutContext& context,
                                       const GeneratedApfsLayoutBlocks& blocks) {
    const ApfsFormatEphemeralLayout ephemeral = apfsFormatEphemeralLayoutForBlockCount(
        context.geometry.blockSize, context.geometry.blockCount);
    appendGeneratedNxsbCheckpointBlockers(context, blocks, ephemeral);
    appendGeneratedCheckpointMapBlockers(context, blocks, ephemeral);
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
    if (block_size_bytes == kSupportedApfsBlockSizeBytes) {
        const QString formatBlocker =
            generatedApfsContainerFormatBlocker(QLatin1StringView("format"),
                                                target_container_bytes / block_size_bytes,
                                                block_size_bytes);
        if (!formatBlocker.isEmpty()) {
            result.blockers.append(formatBlocker);
        }
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
    // The CAB tier (chunk-info ADDRESS blocks) is only needed when the cib count
    // exceeds cibs_per_cab (507); below that the spaceman lists the cib addresses
    // inline, so cab_count stays 0 (matching Apple newfs_apfs / mkapfs, e.g. a
    // 100 GiB container has 7 cibs and cab_count 0).
    geometry.cab_count = geometry.cib_count > geometry.cibs_per_cab
                             ? (geometry.cib_count + geometry.cibs_per_cab - 1) /
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

// Multi-volume (A4): every name must be non-empty, the count is bounded by the
// container superblock's nx_fs_oid capacity, and the container must be large enough
// that fsck_apfs's required nx_max_file_systems = ceil(bytes / 512 MiB) covers every
// volume (so 2 volumes need > 512 MiB, etc.). The additional volumes' six-block sets
// share chunk 0's reserved prefix, so the metadata-overflow tier (whose prefix spills
// past chunk 0) is fail-closed for multi-volume until separately certified.
bool appendMultiVolumeFormatBlockers(const PartitionApfsImageFormatRequest& request,
                                     PartitionApfsImageBuildResult* result) {
    const qsizetype extraVolumes = request.additional_volume_names.size();
    if (extraVolumes == 0) {
        return true;
    }
    if (request.block_size_bytes == 0 || request.target_container_bytes == 0) {
        return true;  // size/geometry already rejected upstream
    }
    if (request.block_size_bytes != kSupportedApfsBlockSizeBytes) {
        result->blockers.append(
            QStringLiteral("APFS multi-volume containers require the 4096-byte generated layout"));
        return false;
    }
    for (const QString& name : request.additional_volume_names) {
        if (name.trimmed().isEmpty()) {
            result->blockers.append(
                QStringLiteral("APFS additional volume names must each be non-empty"));
            return false;
        }
    }
    const uint64_t blockCount = request.target_container_bytes / request.block_size_bytes;
    const uint64_t totalVolumes = 1 + static_cast<uint64_t>(extraVolumes);
    if (totalVolumes > kApfsNxMaxFileSystemsCap) {
        result->blockers.append(QStringLiteral("APFS containers support at most %1 volumes")
                                    .arg(kApfsNxMaxFileSystemsCap));
        return false;
    }
    const uint32_t maxFileSystems = apfsMaxFileSystems(blockCount, request.block_size_bytes);
    if (totalVolumes > maxFileSystems) {
        result->blockers.append(
            QStringLiteral("APFS container is too small for %1 volumes: fsck_apfs requires "
                           "nx_max_file_systems = ceil(size / 512 MiB) (%2 here) to cover every "
                           "volume, so %1 volumes need a container of at least %1 x 512 MiB")
                .arg(totalVolumes)
                .arg(maxFileSystems));
        return false;
    }
    const uint64_t chunkCount = (blockCount + kApfsSpacemanBlocksPerChunk - 1) /
                                kApfsSpacemanBlocksPerChunk;
    const ApfsMultiCibLayout mcib = computeMultiCibLayout(chunkCount);
    const uint64_t reservedSeed = generatedSeedDataBlock(chunkCount, mcib.cibCount, mcib.cabCount) +
                                  kApfsExtraVolumeBlockSpan * static_cast<uint64_t>(extraVolumes);
    if (reservedSeed >= kApfsSpacemanBlocksPerChunk) {
        result->blockers.append(QStringLiteral(
            "APFS multi-volume containers are not certified on the metadata-overflow "
            "tier (the additional volumes' metadata must fit chunk 0)"));
        return false;
    }
    return true;
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
    return appendMultiVolumeFormatBlockers(request, result);
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

// Mark the freshly created image file sparse where the platform supports it, so a
// large container (the metadata-overflow/CAB tiers reach multiple TiB) materializes
// only the metadata blocks actually written rather than its full logical size. Best
// effort: on a file system without sparse support the subsequent resize still works
// when the volume has room. On POSIX, ftruncate already produces a sparse file.
void markImageSparse(QFile* image) {
    markFileSparse(image->handle());
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
    markImageSparse(image);
    if (!image->resize(static_cast<qint64>(sizeBytes))) {
        blockers->append(QStringLiteral("Unable to size APFS image: %1").arg(image->errorString()));
        return false;
    }
    return true;
}

// Multi-volume (A4): the placement + identity of one additional APFS volume.
struct ExtraApfsVolume {
    QString name;
    QByteArray uuid;
    uint32_t fsIndex{0};    // slot in nx_fs_oid (1, 2, ... -- the first volume is 0)
    uint64_t volumeOid{0};  // virtual OID published in the container omap + nx_fs_oid
    uint64_t rootTreeOid{0};
    uint64_t volOmap{0};
    uint64_t volOmapTree{0};
    uint64_t extentRef{0};
    uint64_t snapMeta{0};
    uint64_t rootTree{0};
    uint64_t volSuper{0};
};

struct ExtraApfsVolumes {
    QVector<ExtraApfsVolume> volumes;
    uint64_t blockCount{0};  // total blocks consumed by all additional volumes
};

// Place each additional volume's six metadata blocks contiguously from baseBlock
// (the first volume's reserved-prefix end) and assign its virtual OIDs. The first
// volume owns OIDs 1026 (superblock) and 1028 (root tree); each additional volume k
// (1-based) takes superblock OID 1030+2*(k-1) and root-tree OID one above it, keeping
// every assigned OID below nx_next_oid.
ExtraApfsVolumes layOutExtraApfsVolumes(const QStringList& names, uint64_t baseBlock) {
    ExtraApfsVolumes out;
    uint64_t block = baseBlock;
    for (qsizetype i = 0; i < names.size(); ++i) {
        ExtraApfsVolume v;
        v.name = names.at(i);
        v.uuid = randomApfsUuid();
        v.fsIndex = static_cast<uint32_t>(i + 1);
        v.volumeOid = kApfsNxMinimumNextOid + 2 * static_cast<uint64_t>(i);
        v.rootTreeOid = v.volumeOid + 1;
        v.volOmap = block++;
        v.volOmapTree = block++;
        v.extentRef = block++;
        v.snapMeta = block++;
        v.rootTree = block++;
        v.volSuper = block++;
        out.volumes.append(v);
    }
    out.blockCount = block - baseBlock;
    return out;
}

// A6 (A-f) software-FileVault encryption material. PBKDF2 iteration count for a
// S.A.K.-generated volume (the kernel re-derives with whatever count the keybag
// records; a real macOS volume uses a machine-tuned count near this).
constexpr uint64_t kApfsEncryptionIterations = 100'000;
// Harvested inner-keyblob `flags` integers for a pure-software (AES-256 wrap,
// no CoreStorage/hardware bits) FileVault volume: VEK keyblob vs KEK keyblob.
const QByteArray kApfsVekBlobFlags = QByteArray::fromHex("000000000100bcac");
const QByteArray kApfsKekBlobFlags = QByteArray::fromHex("000000000200bcac");

struct ApfsEncryptionMaterial {
    bool ok{false};
    QByteArray vek;                   // 32-byte AES-XTS volume key
    QByteArray containerKeybagBlock;  // encrypted, ready to write
    QByteArray volumeKeybagBlock;     // encrypted, ready to write
};

struct ApfsEncryptionInputs {
    QString password;
    QString recoveryKey;  // optional personal-recovery-key credential (PRK)
    QByteArray containerUuid;
    QByteArray volumeUuid;
    uint64_t containerKbBlock{0};
    uint64_t volumeKbBlock{0};
    int blockSize{0};
};

/// @brief Fletcher-stamp a plaintext keybag block then AES-XTS-encrypt the whole
/// 4096B block with @p uuid||uuid (tweak base = blockAddr * 8).
QByteArray sealKeybagBlock(QByteArray plaintext,
                           const QByteArray& uuid,
                           uint64_t blockAddr,
                           QStringList* blockers) {
    if (!stampApfsObjectBlock(&plaintext, blockers)) {
        return {};
    }
    return sak::apfs_crypto::xtsEncryptBlock(uuid + uuid, blockAddr, plaintext);
}

/// @brief Build one per-volume-keybag unlock record: the volume KEK wrapped by a
/// credential (password or recovery key) via PBKDF2-HMAC-SHA256, keyed by @p uuid.
sak::apfs_keybag::KeybagEntry buildVolumeUnlockRecord(const QByteArray& kek,
                                                      const QByteArray& credential,
                                                      const QByteArray& uuid,
                                                      QStringList* blockers) {
    using namespace sak::apfs_crypto;
    const QByteArray salt = randomBytes(16);
    const QByteArray derived = pbkdf2Sha256(credential, salt, kApfsEncryptionIterations, 32);
    const QByteArray wrappedKek = aesKeyWrap(derived, kek);
    if (wrappedKek.size() != kApfsWrappedKeyBytes) {
        blockers->append(QStringLiteral("APFS encryption unlock-record generation failed"));
        return {};
    }
    const QByteArray kekBlob =
        sak::apfs_keybag::buildKekBlob({.uuid = uuid,
                                        .wrappedKey = wrappedKek,
                                        .flags8 = kApfsKekBlobFlags,
                                        .outerSalt = randomBytes(8),
                                        .iterations = kApfsEncryptionIterations,
                                        .salt = salt});
    return {uuid, sak::apfs_keybag::kKbTagVolumeUnlockRecords, kekBlob};
}

/// @brief Generate the VEK/KEK, build the container + per-volume keybags from the
/// user password (and optional recovery key), and return both keybag blocks
/// encrypted + ready to place. With a recovery key the volume keybag gets a second
/// unlock record so the volume unlocks by EITHER the password or the recovery key.
ApfsEncryptionMaterial buildApfsEncryptionMaterial(const ApfsEncryptionInputs& in,
                                                   QStringList* blockers) {
    using namespace sak::apfs_crypto;
    using namespace sak::apfs_keybag;
    const QByteArray& volumeUuid = in.volumeUuid;
    const int blockSize = in.blockSize;
    ApfsEncryptionMaterial mat;
    const QByteArray vek = randomBytes(32);
    const QByteArray kek = randomBytes(32);
    const QByteArray wrappedVek = aesKeyWrap(kek, vek);
    if (vek.size() != 32 || wrappedVek.size() != kApfsWrappedKeyBytes) {
        blockers->append(QStringLiteral("APFS encryption key generation failed"));
        return mat;
    }
    QList<KeybagEntry> volumeEntries{
        buildVolumeUnlockRecord(kek, in.password.toUtf8(), volumeUuid, blockers)};
    if (!in.recoveryKey.isEmpty()) {
        const QByteArray prkUuid =
            QUuid::fromString(QString::fromLatin1(kApfsRecoveryKeyUuid)).toRfc4122();
        volumeEntries.append(
            buildVolumeUnlockRecord(kek, in.recoveryKey.toUtf8(), prkUuid, blockers));
    }
    // Fail closed if any unlock record failed to generate: buildVolumeUnlockRecord
    // returns a default (empty-uuid) entry on failure, which buildKeybagBlock would
    // pack with a 16-byte memcpy from empty data (out-of-bounds). The blocker is
    // already recorded, so just stop before assembling the keybag.
    for (const auto& entry : volumeEntries) {
        if (entry.uuid.size() != kApfsUuidBytes || entry.keydata.isEmpty()) {
            return mat;
        }
    }
    const QByteArray volumeKb =
        buildKeybagBlock(kApfsObjectTypeVolumeKeybag, 0, kApfsFormatXid, volumeEntries, blockSize);
    // Container keybag: VOLUME_UNLOCK_RECORDS = prange to the per-volume keybag,
    // VOLUME_KEY = the VEK blob (both keyed by the volume UUID).
    const QByteArray vekBlob = buildVekBlob({.uuid = volumeUuid,
                                             .wrappedKey = wrappedVek,
                                             .flags8 = kApfsVekBlobFlags,
                                             .outerSalt = randomBytes(8)});
    QByteArray prange(16, '\0');
    writeLe64(&prange, 0, in.volumeKbBlock);
    writeLe64(&prange, 8, 1);
    const QByteArray containerKb = buildKeybagBlock(
        kApfsObjectTypeContainerKeybag,
        0,
        kApfsFormatXid,
        {{volumeUuid, kKbTagVolumeUnlockRecords, prange}, {volumeUuid, kKbTagVolumeKey, vekBlob}},
        blockSize);
    mat.vek = vek;
    mat.containerKeybagBlock =
        sealKeybagBlock(containerKb, in.containerUuid, in.containerKbBlock, blockers);
    mat.volumeKeybagBlock = sealKeybagBlock(volumeKb, volumeUuid, in.volumeKbBlock, blockers);
    mat.ok = mat.containerKeybagBlock.size() == blockSize &&
             mat.volumeKeybagBlock.size() == blockSize;
    return mat;
}

// Per-format encryption plan: the field values the format path reads when a
// software-encrypted volume is requested (all zero / unencrypted otherwise).
struct ApfsFormatEncryption {
    bool enabled{false};
    bool ok{false};
    uint64_t containerKbBlock{0};
    uint64_t volumeKbBlock{0};
    uint64_t reservedDelta{0};  // extra reserved blocks for the two keybags
    uint32_t omapFlag{0};       // OMAP_VAL_ENCRYPTED on the fs-tree mapping
    uint64_t fsFlags{1};        // APFS_FS_UNENCRYPTED vs APFS_FS_ONEKEY
    uint64_t keylockerBlocks{0};
    QByteArray vek;
    QByteArray containerKeybagBlock;
    QByteArray volumeKeybagBlock;
};

/// @brief Resolve the encryption plan for a format request. The two keybags take
/// the next blocks after the reserved metadata prefix at @p baseReserved.
ApfsFormatEncryption prepareFormatEncryption(const PartitionApfsImageFormatRequest& request,
                                             uint64_t baseReserved,
                                             const QByteArray& containerUuid,
                                             const QByteArray& volumeUuid,
                                             QStringList* blockers) {
    ApfsFormatEncryption enc;
    if (request.volume_password.isEmpty()) {
        return enc;
    }
    const ApfsEncryptionMaterial mat =
        buildApfsEncryptionMaterial({.password = request.volume_password,
                                     .recoveryKey = request.recovery_key,
                                     .containerUuid = containerUuid,
                                     .volumeUuid = volumeUuid,
                                     .containerKbBlock = baseReserved,
                                     .volumeKbBlock = baseReserved + 1,
                                     .blockSize = static_cast<int>(request.block_size_bytes)},
                                    blockers);
    if (!mat.ok) {
        // Key material failed (blocker already recorded). Leave the plan disabled so
        // the format never emits an inconsistent ONEKEY-flagged-but-plaintext volume;
        // the operation fails closed on the blocker.
        return enc;
    }
    enc.enabled = true;
    enc.ok = true;
    enc.containerKbBlock = baseReserved;
    enc.volumeKbBlock = baseReserved + 1;
    enc.reservedDelta = 2;
    enc.omapFlag = kApfsOmapValueEncrypted;
    enc.fsFlags = kApfsVolumeFsFlagsOneKey;
    enc.keylockerBlocks = 1;
    enc.vek = mat.vek;
    enc.containerKeybagBlock = mat.containerKeybagBlock;
    enc.volumeKeybagBlock = mat.volumeKeybagBlock;
    return enc;
}

/// @brief AES-XTS-encrypt the volume fs-tree with the VEK and append the two
/// keybag blocks. No-op for an unencrypted (or failed) format.
void applyFormatEncryption(QVector<ApfsImageBlock>* blocks,
                           const ApfsFormatEncryption& enc,
                           uint64_t rootTree) {
    if (!enc.enabled || !enc.ok) {
        return;
    }
    for (auto& blk : *blocks) {
        if (blk.first == rootTree) {
            blk.second = sak::apfs_crypto::xtsEncryptBlock(enc.vek, rootTree, blk.second);
        }
    }
    blocks->append({enc.containerKbBlock, enc.containerKeybagBlock});
    blocks->append({enc.volumeKbBlock, enc.volumeKeybagBlock});
}

// Multi-volume (A4): each additional volume's six metadata blocks. Mirror the
// first volume's structures with this volume's own OIDs/UUID/index. Each volume's
// object map (a physical object, OID = block) resolves its own root-tree OID to
// its empty root tree; the extent-ref and snap-meta trees are physical objects.
// Byte-identical to the inline loop it replaces (no-op when extras is empty).
void appendExtraVolumeBlocks(QVector<ApfsImageBlock>* blocks,
                             const PartitionApfsImageFormatRequest& request,
                             const ExtraApfsVolumes& extras,
                             QStringList* blockers) {
    for (const auto& extra : extras.volumes) {
        const QVector<ApfsObjectMapEntry> extraVolumeMappings{
            {extra.rootTreeOid, kApfsFormatXid, extra.rootTree}};
        blocks->append({extra.volOmap,
                        buildObjectMapBlock({.blockSize = request.block_size_bytes,
                                             .oid = extra.volOmap,
                                             .treeBlock = extra.volOmapTree,
                                             .xid = kApfsFormatXid,
                                             .omapFlags = 0},
                                            blockers)});
        blocks->append({extra.volOmapTree,
                        buildObjectMapTreeBlock(request.block_size_bytes,
                                                extra.volOmapTree,
                                                extraVolumeMappings,
                                                kApfsFormatXid,
                                                blockers)});
        blocks->append({extra.extentRef,
                        buildEmptyVariableTreeBlock(request.block_size_bytes,
                                                    extra.extentRef,
                                                    kApfsObjectSubtypeExtentRef,
                                                    blockers)});
        blocks->append(
            {extra.snapMeta,
             buildEmptyVariableTreeBlock(
                 request.block_size_bytes, extra.snapMeta, kApfsObjectSubtypeSnapMeta, blockers)});
        blocks->append({extra.rootTree,
                        buildEmptyRootTreeBlockForVolume(
                            request.block_size_bytes, extra.rootTreeOid, blockers)});
        blocks->append(
            {extra.volSuper,
             buildVolumeSuperblock({.blockSize = request.block_size_bytes,
                                    .volumeName = extra.name,
                                    .volumeUuid = extra.uuid,
                                    .allocatedBlocks = kApfsFormatVolumeBaseAllocatedBlocks,
                                    .volumeOmapBlock = extra.volOmap,
                                    .extentRefTreeBlock = extra.extentRef,
                                    .snapMetaTreeBlock = extra.snapMeta,
                                    .volumeOid = extra.volumeOid,
                                    .fsIndex = extra.fsIndex,
                                    .rootTreeOid = extra.rootTreeOid},
                                   blockers)});
    }
}

// Bundles the genesis/live spaceman object spans for the continuation-block writer so
// it stays within the parameter-count gate. Same values the inline loop used.
struct ApfsSpacemanContinuationParams {
    uint64_t spacemanBlocks;
    uint64_t genesisSpacemanBlock;
    uint64_t liveSpacemanBlock;
    const QByteArray& genesisSpacemanObj;
    const QByteArray& liveSpacemanObj;
};

// Continuation blocks of any multi-block (overflow) spaceman object. The header +
// fletcher64 stamped over the whole object live in the first block (already in the
// list); these are the raw remaining blocks of the same contiguous object.
// Byte-identical to the inline loop it replaces (no-op when spacemanBlocks == 1).
void appendSpacemanContinuationBlocks(QVector<ApfsImageBlock>* blocks,
                                      const PartitionApfsImageFormatRequest& request,
                                      const ApfsSpacemanContinuationParams& p) {
    for (uint64_t i = 1; i < p.spacemanBlocks; ++i) {
        const qsizetype off = static_cast<qsizetype>(i * request.block_size_bytes);
        blocks->append(
            {p.genesisSpacemanBlock + i,
             p.genesisSpacemanObj.mid(off, static_cast<qsizetype>(request.block_size_bytes))});
        blocks->append(
            {p.liveSpacemanBlock + i,
             p.liveSpacemanObj.mid(off, static_cast<qsizetype>(request.block_size_bytes))});
    }
}

struct ApfsInternalPoolBlocksParams {
    QVector<ApfsImageBlock>* blocks;
    const PartitionApfsImageFormatRequest& request;
    uint64_t blockCount;
    uint64_t chunkCount;
    const ApfsMultiCibLayout& mcib;
    uint64_t ghostReserved;
    uint64_t reservedSeed;
    uint64_t seedData;
};

// Emits the internal-pool chunk-info blocks (genesis/live cib 0 + immutable cibs),
// the ip allocation bitmaps, the per-chunk allocation bitmaps (chunk 0, overflow
// chunks, boundary chunk), and the CAB-tier cib-address blocks. Byte-identical to
// the inline sequence it replaces.
// Internal-pool chunk-info blocks + ip allocation bitmaps. cib 0 (chunk 0's
// allocation cib) gets a genesis (xid 1) and a live (xid 2) copy in its rotation
// slots; cib 1..N-1 describe the all-free upper chunks and are emitted ONCE as
// immutable blocks. Single-CIB reduces to the certified {185,186} ghost / {187,188}
// live blocks (no immutable cibs). Byte-identical to the inline sequence it replaces.
void appendChunkInfoBlocks(const ApfsInternalPoolBlocksParams& p, QStringList* blockers) {
    const PartitionApfsImageFormatRequest& request = p.request;
    const ApfsMultiCibLayout& mcib = p.mcib;
    QVector<ApfsImageBlock>* blocks = p.blocks;
    const uint64_t cib0Span = std::min<uint64_t>(kApfsSpacemanChunksPerCib, p.chunkCount);
    // ip allocation bitmaps: ring slot 0 = genesis, slot 1 = live, each ip_bm_size
    // blocks (only the first holds usage bits; the IP usage is contiguous from
    // ip_base so it stays inside the first bitmap block). Larger bitmaps shift the
    // live slot from ip_bm_base + 1 to ip_bm_base + ip_bm_size.
    const uint64_t ipBmSize = ipBitmapSizeBlocks(p.chunkCount, mcib.cibCount, mcib.cabCount);
    blocks->append({kApfsFormatIpBitmapBaseBlock,
                    buildIpBitmapBlock(request.block_size_bytes, mcib.genesisIpUsage)});
    blocks->append({kApfsFormatIpBitmapBaseBlock + ipBmSize,
                    buildIpBitmapBlock(request.block_size_bytes, mcib.liveIpUsage)});
    blocks->append({mcib.ghostCibs.first(),
                    buildChunkInfoBlock({.blockSize = request.block_size_bytes,
                                         .blockCount = p.blockCount,
                                         .reservedBlocks = p.ghostReserved,
                                         .xid = kApfsFormatGenesisXid,
                                         .selfBlock = mcib.ghostCibs.first(),
                                         .bitmapBlock = mcib.ghostBitmap,
                                         .chunkStart = 0,
                                         .chunkSpan = cib0Span,
                                         .cibIndex = 0,
                                         .metadataChunks = mcib.metadataChunks,
                                         .extraChunkBitmaps = mcib.extraChunkBitmaps,
                                         .boundaryBitmap = mcib.ghostBoundaryBitmap},
                                        blockers)});
    blocks->append({mcib.liveCibs.first(),
                    buildChunkInfoBlock({.blockSize = request.block_size_bytes,
                                         .blockCount = p.blockCount,
                                         .reservedBlocks = p.reservedSeed,
                                         .xid = kApfsFormatXid,
                                         .selfBlock = mcib.liveCibs.first(),
                                         .bitmapBlock = mcib.liveBitmap,
                                         .chunkStart = 0,
                                         .chunkSpan = cib0Span,
                                         .cibIndex = 0,
                                         .metadataChunks = mcib.metadataChunks,
                                         .extraChunkBitmaps = mcib.extraChunkBitmaps,
                                         .boundaryBitmap = mcib.liveBoundaryBitmap},
                                        blockers)});
    for (qsizetype i = 0; i < mcib.immutableCibs.size(); ++i) {
        const uint64_t cibIndex = static_cast<uint64_t>(i) + 1;
        const uint64_t chunkStart = cibIndex * kApfsSpacemanChunksPerCib;
        const uint64_t chunkSpan = std::min<uint64_t>(kApfsSpacemanChunksPerCib,
                                                      p.chunkCount - chunkStart);
        blocks->append({mcib.immutableCibs.at(i),
                        buildChunkInfoBlock({.blockSize = request.block_size_bytes,
                                             .blockCount = p.blockCount,
                                             .reservedBlocks = 0,
                                             .xid = kApfsFormatGenesisXid,
                                             .selfBlock = mcib.immutableCibs.at(i),
                                             .bitmapBlock = 0,
                                             .chunkStart = chunkStart,
                                             .chunkSpan = chunkSpan,
                                             .cibIndex = cibIndex},
                                            blockers)});
    }
}

// Per-chunk allocation bitmaps: chunk-0 genesis/live, the overflow chunks 1..M-1,
// and the boundary chunk's genesis/live rotating bitmaps. Each marks that chunk's
// slice of the reserved prefix. Byte-identical to the inline sequence it replaces.
void appendChunkBitmapBlocks(const ApfsInternalPoolBlocksParams& p) {
    const PartitionApfsImageFormatRequest& request = p.request;
    const ApfsMultiCibLayout& mcib = p.mcib;
    QVector<ApfsImageBlock>* blocks = p.blocks;
    // Chunk-0 allocation bitmaps mark only chunk 0's slice of the reserved prefix
    // (clamped to 32768 so a multi-chunk prefix does not overrun the bitmap block).
    blocks->append(
        {mcib.ghostBitmap,
         buildChunkBitmapBlock(request.block_size_bytes, chunkReservedBlocks(p.ghostReserved, 0))});
    blocks->append(
        {mcib.liveBitmap,
         buildChunkBitmapBlock(request.block_size_bytes, chunkReservedBlocks(p.reservedSeed, 0))});
    // Metadata-overflow: immutable per-chunk bitmaps for chunks 1..M-1 (each marks
    // that chunk's slice of the reserved prefix).
    for (qsizetype i = 0; i < mcib.extraChunkBitmaps.size(); ++i) {
        const uint64_t chunk = static_cast<uint64_t>(i) + 1;
        blocks->append({mcib.extraChunkBitmaps.at(i),
                        buildChunkBitmapBlock(request.block_size_bytes,
                                              chunkReservedBlocks(p.seedData, chunk))});
    }
    // Multi-chunk overflow (CAB included): the boundary chunk (M-1) gets genesis + live
    // rotating bitmaps (the spare group slot stays free for the first commit). Each
    // marks the boundary chunk's slice of its checkpoint's reserved prefix.
    if (mcib.ghostBoundaryBitmap != 0) {
        blocks->append(
            {mcib.ghostBoundaryBitmap,
             buildChunkBitmapBlock(request.block_size_bytes,
                                   chunkReservedBlocks(p.ghostReserved, mcib.boundaryChunk))});
        blocks->append(
            {mcib.liveBoundaryBitmap,
             buildChunkBitmapBlock(request.block_size_bytes,
                                   chunkReservedBlocks(p.seedData, mcib.boundaryChunk))});
    }
}

// CAB tier (cib_count > 507): emit the chunk-info ADDRESS blocks the spaceman
// points at. cab 0 rotates with cib 0 (genesis + live copies); cabs 1..N-1 are
// immutable. Each CAB holds up to 507 cib block numbers from the matching
// checkpoint's cib list - only cib 0 differs genesis vs live, so the immutable
// cabs are identical in both lists and emitted once. Byte-identical to the inline
// branch it replaces (no-op below the CAB tier).
void appendCabAddrBlocks(const ApfsInternalPoolBlocksParams& p, QStringList* blockers) {
    const PartitionApfsImageFormatRequest& request = p.request;
    const ApfsMultiCibLayout& mcib = p.mcib;
    QVector<ApfsImageBlock>* blocks = p.blocks;
    if (mcib.cabCount > 0) {
        const auto cibSlice = [](const QVector<uint64_t>& cibs, uint64_t cabIndex) {
            const qsizetype start = static_cast<qsizetype>(cabIndex) * kApfsSpacemanCibsPerCab;
            const qsizetype len = std::min<qsizetype>(kApfsSpacemanCibsPerCab, cibs.size() - start);
            return cibs.mid(start, len);
        };
        blocks->append({mcib.ghostCab0,
                        buildCibAddrBlock({.blockSize = request.block_size_bytes,
                                           .selfBlock = mcib.ghostCab0,
                                           .xid = kApfsFormatGenesisXid,
                                           .cabIndex = 0,
                                           .cibAddrs = cibSlice(mcib.ghostCibs, 0)},
                                          blockers)});
        blocks->append({mcib.liveCab0,
                        buildCibAddrBlock({.blockSize = request.block_size_bytes,
                                           .selfBlock = mcib.liveCab0,
                                           .xid = kApfsFormatXid,
                                           .cabIndex = 0,
                                           .cibAddrs = cibSlice(mcib.liveCibs, 0)},
                                          blockers)});
        for (qsizetype i = 0; i < mcib.immutableCabs.size(); ++i) {
            const uint64_t cabIndex = static_cast<uint64_t>(i) + 1;
            blocks->append({mcib.immutableCabs.at(i),
                            buildCibAddrBlock({.blockSize = request.block_size_bytes,
                                               .selfBlock = mcib.immutableCabs.at(i),
                                               .xid = kApfsFormatGenesisXid,
                                               .cabIndex = static_cast<uint32_t>(cabIndex),
                                               .cibAddrs = cibSlice(mcib.liveCibs, cabIndex)},
                                              blockers)});
        }
    }
}

void appendInternalPoolBlocks(const ApfsInternalPoolBlocksParams& p, QStringList* blockers) {
    appendChunkInfoBlocks(p, blockers);
    appendChunkBitmapBlocks(p);
    appendCabAddrBlocks(p, blockers);
}

// The fully-resolved block layout for an empty container: random UUIDs, the
// multi-CIB/CAB layout, every post-internal-pool object's block number, the extra
// volumes, the encryption plan, the container/volume omap mappings, and the
// pre-built superblock/spaceman byte arrays. computeEmptyFormatPlan fills it; the
// base-block and append helpers consume it.
struct ApfsEmptyFormatPlan {
    QByteArray containerUuid;
    QByteArray volumeUuid;
    uint64_t chunkCount{0};
    ApfsMultiCibLayout mcib;
    uint64_t spacemanBlocks{0};
    uint64_t genesisSpacemanBlock{0};
    uint64_t genesisReaperBlock{0};
    uint64_t liveSpacemanBlock{0};
    uint64_t liveReaperBlock{0};
    uint64_t fqIpTreeBlock{0};
    uint64_t fqMainTreeBlock{0};
    uint64_t ghostIpFreeCount{0};
    uint64_t ghostOmap{0};
    uint64_t ghostOmapTree{0};
    uint64_t ghostReserved{0};
    uint64_t volOmap{0};
    uint64_t volOmapTree{0};
    uint64_t extentRef{0};
    uint64_t snapMeta{0};
    uint64_t rootTree{0};
    uint64_t volSuper{0};
    uint64_t containerOmap{0};
    uint64_t containerOmapTree{0};
    uint64_t seedData{0};
    uint64_t reservedSeed{0};
    ExtraApfsVolumes extras;
    ApfsFormatEncryption enc;
    QVector<ApfsObjectMapEntry> volumeMappings;
    QVector<ApfsObjectMapEntry> containerMappings;
    QVector<ApfsCheckpointMapEntry> genesisMappings;
    QVector<ApfsCheckpointMapEntry> liveMappings;
    QByteArray nxsb;
    QByteArray genesisNxsb;
    QByteArray genesisSpacemanObj;
    QByteArray liveSpacemanObj;
};

// Fills the random UUIDs, multi-CIB/CAB layout, ephemeral spaceman/reaper/free-queue
// block numbers, and every post-internal-pool object's block number into *p.
// Byte-identical to the inline prologue it replaces.
void resolveEmptyFormatBlockNumbers(const PartitionApfsImageFormatRequest& request,
                                    uint64_t blockCount,
                                    ApfsEmptyFormatPlan* p) {
    p->containerUuid = randomApfsUuid();
    p->volumeUuid = randomApfsUuid();
    // The internal pool grows to 3*(chunk_count+cib_count) blocks; every object
    // placed after the internal pool shifts by the same delta so the IP region
    // and the post-pool metadata never overlap (apfsck marks the whole IP region
    // used). Multi-CIB (>126 chunks) widens each rotation slot to cib_count cibs.
    p->chunkCount = (blockCount + kApfsSpacemanBlocksPerChunk - 1) / kApfsSpacemanBlocksPerChunk;
    p->mcib = computeMultiCibLayout(p->chunkCount);
    const uint64_t ipDelta = generatedIpDelta(p->chunkCount, p->mcib.cibCount, p->mcib.cabCount);
    // The spaceman ephemeral object spans one block until its inline cib-address array
    // overflows (the ~2.9 TiB metadata-overflow band), then a second; the genesis and
    // live checkpoint-data ephemerals after each spaceman shift by spacemanBlocks-1 so
    // every certified single-block tier stays byte-identical.
    const ApfsFormatEphemeralLayout ephemeral = apfsFormatEphemeralLayout(
        request.block_size_bytes, p->chunkCount, p->mcib.cibCount, p->mcib.cabCount);
    p->spacemanBlocks = ephemeral.spacemanBlocks;
    p->genesisSpacemanBlock = ephemeral.genesisSpaceman;
    p->genesisReaperBlock = ephemeral.genesisReaper;
    p->liveSpacemanBlock = ephemeral.liveSpaceman;
    p->liveReaperBlock = ephemeral.liveReaper;
    p->fqIpTreeBlock = ephemeral.fqIpTree;
    p->fqMainTreeBlock = ephemeral.fqMainTree;
    // The genesis rotation group the live checkpoint holds in sm_fq[IP] = the group
    // stride (cib0 + bitmap, +cab0/boundary). apfsck marks these used via the
    // free-queue, so the count must match the ghost-group size emitted in the layout.
    p->ghostIpFreeCount = p->mcib.ipGroupStride;
    p->ghostOmap = kApfsFormatGhostContainerOmapBlock + ipDelta;
    p->ghostOmapTree = kApfsFormatGhostContainerOmapTreeBlock + ipDelta;
    p->ghostReserved = kApfsFormatGhostReservedBlocks + ipDelta;
    p->volOmap = kApfsFormatVolumeOmapBlock + ipDelta;
    p->volOmapTree = kApfsFormatVolumeOmapTreeBlock + ipDelta;
    p->extentRef = kApfsFormatExtentRefTreeBlock + ipDelta;
    p->snapMeta = kApfsFormatSnapMetaTreeBlock + ipDelta;
    p->rootTree = kApfsFormatRootTreeBlock + ipDelta;
    p->volSuper = kApfsFormatVolumeSuperblockBlock + ipDelta;
    p->containerOmap = kApfsFormatContainerOmapBlock + ipDelta;
    p->containerOmapTree = kApfsFormatContainerOmapTreeBlock + ipDelta;
    p->seedData = kApfsFormatSeedFileDataBlock + ipDelta;
}

// Fills the extra volumes, encryption plan, omap/checkpoint mappings, and the
// pre-built nxsb/spaceman byte arrays into *p (the block numbers must already be
// resolved). Byte-identical to the inline sequence it replaces.
// Bundles the caller's already-computed nxsb inputs (container block count, fs OIDs,
// next container OID) so the nxsb builder stays within the parameter-count gate. Same
// values the inline code passed.
struct ApfsEmptyFormatNxsbInputs {
    uint64_t blockCount;
    const QVector<uint64_t>& fsOids;
    uint64_t containerNextOid;
};

// Builds the live + genesis nx superblocks into *pp from the resolved extras, enc
// plan, and container mappings. fsOids/containerNextOid are the caller's already
// computed values. Byte-identical to the inline nxsb builds it replaces.
void buildEmptyFormatNxsbs(const PartitionApfsImageFormatRequest& request,
                           const ApfsEmptyFormatNxsbInputs& in,
                           ApfsEmptyFormatPlan* pp,
                           QStringList* blockers) {
    ApfsEmptyFormatPlan& p = *pp;
    const uint64_t blockCount = in.blockCount;
    const QVector<uint64_t>& fsOids = in.fsOids;
    const uint64_t containerNextOid = in.containerNextOid;
    // Checkpoint-data ring: genesis holds spacemanBlocks + 1 (reaper) blocks; live holds
    // spacemanBlocks + 3 (reaper + the two free-queue trees) right after it. For a
    // single-block spaceman these reduce to the certified 0/2/2 and 2/4/6.
    const uint32_t genesisDataLen = static_cast<uint32_t>(p.spacemanBlocks + 1);
    const uint32_t liveDataIndex = static_cast<uint32_t>(p.spacemanBlocks + 1);
    const uint32_t liveDataLen = static_cast<uint32_t>(p.spacemanBlocks + 3);
    const uint32_t liveDataNext = static_cast<uint32_t>(2 * p.spacemanBlocks + 4);
    p.nxsb = buildNxSuperblock(request.block_size_bytes,
                               blockCount,
                               p.containerUuid,
                               {.dataIndex = liveDataIndex,
                                .dataLen = liveDataLen,
                                .dataNext = liveDataNext,
                                .omapOid = p.containerOmap,
                                .fsOids = fsOids,
                                .nextOid = containerNextOid,
                                .keylockerPaddr = p.enc.containerKbBlock,
                                .keylockerBlocks = p.enc.keylockerBlocks},
                               blockers);
    p.genesisNxsb = buildNxSuperblock(request.block_size_bytes,
                                      blockCount,
                                      p.containerUuid,
                                      {.xid = kApfsFormatGenesisXid,
                                       .descIndex = 0,
                                       .descNext = 2,
                                       .dataIndex = 0,
                                       .dataLen = genesisDataLen,
                                       .dataNext = genesisDataLen,
                                       .omapOid = p.ghostOmap,
                                       .fsOid = 0,
                                       .nextOid = kApfsFormatGenesisNextOid},
                                      blockers);
}

// Builds the genesis + live spaceman objects into *pp. The objects span
// spacemanBlocks blocks; the first block goes in the block list and any
// continuation block is appended after (writeBlock takes one block at a time, but
// the object header + fletcher64 already cover the whole object). Byte-identical to
// the inline buildSpacemanBlock calls it replaces.
void buildEmptyFormatSpacemanObjects(const PartitionApfsImageFormatRequest& request,
                                     uint64_t blockCount,
                                     ApfsEmptyFormatPlan* pp,
                                     QStringList* blockers) {
    ApfsEmptyFormatPlan& p = *pp;
    p.genesisSpacemanObj = buildSpacemanBlock({.blockSize = request.block_size_bytes,
                                               .blockCount = blockCount,
                                               .reservedBlocks = p.ghostReserved,
                                               .xid = kApfsFormatGenesisXid,
                                               .genesis = true,
                                               .cibAddrs = p.mcib.ghostCibs,
                                               .cabCount = p.mcib.cabCount,
                                               .cabAddrs = p.mcib.ghostCabs},
                                              blockers);
    p.liveSpacemanObj = buildSpacemanBlock({.blockSize = request.block_size_bytes,
                                            .blockCount = blockCount,
                                            .reservedBlocks = p.reservedSeed,
                                            .xid = kApfsFormatXid,
                                            .genesis = false,
                                            .cibAddrs = p.mcib.liveCibs,
                                            .cabCount = p.mcib.cabCount,
                                            .cabAddrs = p.mcib.liveCabs,
                                            .ghostIpFreeCount = p.ghostIpFreeCount},
                                           blockers);
}

void resolveEmptyFormatSuperblocks(const PartitionApfsImageFormatRequest& request,
                                   uint64_t blockCount,
                                   ApfsEmptyFormatPlan* pp,
                                   QStringList* blockers) {
    ApfsEmptyFormatPlan& p = *pp;
    const uint32_t spacemanByteSize =
        static_cast<uint32_t>(request.block_size_bytes * p.spacemanBlocks);
    // Multi-volume (A4): each additional volume gets its own six-block set placed in
    // the previously-free region right after the first volume's reserved prefix
    // (volume object map + its tree, extent-ref tree, snap-meta tree, root tree, and
    // volume superblock), plus its own virtual superblock/root-tree OIDs and a unique
    // UUID. They share the container space manager; the reserved prefix grows to cover
    // them. The first volume keeps the certified single-volume OIDs/positions, so an
    // empty additional_volume_names list is byte-identical to the prior layout.
    p.extras = layOutExtraApfsVolumes(request.additional_volume_names, p.seedData);
    // A6: resolve the encryption plan. The two keybags take the blocks after the
    // reserved metadata prefix, so the space manager's reserved count (free count +
    // allocation bitmap) accounts for them; the fs-tree mapping is flagged
    // OMAP_VAL_ENCRYPTED. Unencrypted leaves every value default -> byte-identical.
    p.enc = prepareFormatEncryption(
        request, p.seedData + p.extras.blockCount, p.containerUuid, p.volumeUuid, blockers);
    p.reservedSeed = p.seedData + p.extras.blockCount + p.enc.reservedDelta;
    p.volumeMappings = {{kApfsFormatRootTreeOid, kApfsFormatXid, p.rootTree, p.enc.omapFlag}};
    const uint64_t containerNextOid = kApfsNxMinimumNextOid +
                                      2 * static_cast<uint64_t>(p.extras.volumes.size());
    p.containerMappings = {{kApfsFormatVolumeOid, kApfsFormatXid, p.volSuper}};
    QVector<uint64_t> fsOids{kApfsFormatVolumeOid};
    for (const auto& extra : p.extras.volumes) {
        p.containerMappings.append({extra.volumeOid, kApfsFormatXid, extra.volSuper});
        fsOids.append(extra.volumeOid);
    }
    buildEmptyFormatNxsbs(
        request,
        {.blockCount = blockCount, .fsOids = fsOids, .containerNextOid = containerNextOid},
        pp,
        blockers);
    p.genesisMappings = {{kApfsObjectTypeSpaceman,
                          0,
                          kApfsFormatSpacemanOid,
                          p.genesisSpacemanBlock,
                          spacemanByteSize},
                         {kApfsObjectTypeReaper, 0, kApfsFormatReaperOid, p.genesisReaperBlock}};
    p.liveMappings = {
        {kApfsObjectTypeSpaceman, 0, kApfsFormatSpacemanOid, p.liveSpacemanBlock, spacemanByteSize},
        {kApfsObjectTypeReaper, 0, kApfsFormatReaperOid, p.liveReaperBlock},
        {kApfsObjectTypeBtreeEphemeral,
         kApfsObjectSubtypeFreeQueue,
         kApfsFormatFqIpTreeOid,
         p.fqIpTreeBlock},
        {kApfsObjectTypeBtreeEphemeral,
         kApfsObjectSubtypeFreeQueue,
         kApfsFormatFqMainTreeOid,
         p.fqMainTreeBlock}};
    buildEmptyFormatSpacemanObjects(request, blockCount, pp, blockers);
}

ApfsEmptyFormatPlan computeEmptyFormatPlan(const PartitionApfsImageFormatRequest& request,
                                           uint64_t blockCount,
                                           QStringList* blockers) {
    ApfsEmptyFormatPlan p;
    resolveEmptyFormatBlockNumbers(request, blockCount, &p);
    resolveEmptyFormatSuperblocks(request, blockCount, &p, blockers);
    return p;
}

// The container/checkpoint blocks: block 0, the genesis + live checkpoint maps and
// nxsb copies, the spaceman/reaper first blocks, and the two free-queue trees.
// Byte-identical to the head of the inline initializer list it replaces.
QVector<ApfsImageBlock> buildEmptyFormatCheckpointBlocks(
    const ApfsEmptyFormatPlan& p,
    const PartitionApfsImageFormatRequest& request,
    QStringList* blockers) {
    return {{kApfsFormatNxsbBlock, p.nxsb},
            {kApfsFormatGenesisMapBlock,
             buildCheckpointMapBlock(request.block_size_bytes,
                                     kApfsFormatGenesisMapBlock,
                                     kApfsFormatGenesisXid,
                                     p.genesisMappings,
                                     blockers)},
            {kApfsFormatGenesisNxsbBlock, p.genesisNxsb},
            {kApfsFormatCheckpointMapBlock,
             buildCheckpointMapBlock(request.block_size_bytes,
                                     kApfsFormatCheckpointMapBlock,
                                     kApfsFormatXid,
                                     p.liveMappings,
                                     blockers)},
            {kApfsFormatCheckpointNxsbCopyBlock, p.nxsb},
            {p.genesisSpacemanBlock,
             p.genesisSpacemanObj.left(static_cast<qsizetype>(request.block_size_bytes))},
            {p.genesisReaperBlock,
             buildReaperBlock(request.block_size_bytes, kApfsFormatGenesisXid, blockers)},
            {p.liveSpacemanBlock,
             p.liveSpacemanObj.left(static_cast<qsizetype>(request.block_size_bytes))},
            {p.liveReaperBlock,
             buildReaperBlock(request.block_size_bytes, kApfsFormatXid, blockers)},
            {p.fqIpTreeBlock,
             buildFreeQueueTreeBlock(request.block_size_bytes,
                                     kApfsFormatFqIpTreeOid,
                                     p.mcib.ghostCibs.first(),
                                     p.ghostIpFreeCount,
                                     blockers)},
            {p.fqMainTreeBlock,
             buildFreeQueueTreeBlock(
                 request.block_size_bytes, kApfsFormatFqMainTreeOid, p.ghostOmap, 2, blockers)}};
}

// The volume/object-map blocks: the ghost + live + container object maps and trees,
// the extent-ref / snap-meta trees, the root tree, and the volume superblock.
// Byte-identical to the tail of the inline initializer list it replaces.
QVector<ApfsImageBlock> buildEmptyFormatVolumeBlocks(const ApfsEmptyFormatPlan& p,
                                                     const PartitionApfsImageFormatRequest& request,
                                                     const QString& volumeName,
                                                     QStringList* blockers) {
    return {
        {p.ghostOmap,
         buildObjectMapBlock({.blockSize = request.block_size_bytes,
                              .oid = p.ghostOmap,
                              .treeBlock = p.ghostOmapTree,
                              .xid = kApfsFormatGenesisXid,
                              .omapFlags = 1},
                             blockers)},
        {p.ghostOmapTree,
         buildObjectMapTreeBlock(
             request.block_size_bytes, p.ghostOmapTree, {}, kApfsFormatGenesisXid, blockers)},
        {p.volOmap,
         buildObjectMapBlock({.blockSize = request.block_size_bytes,
                              .oid = p.volOmap,
                              .treeBlock = p.volOmapTree,
                              .xid = kApfsFormatXid,
                              .omapFlags = 0},
                             blockers)},
        {p.volOmapTree,
         buildObjectMapTreeBlock(
             request.block_size_bytes, p.volOmapTree, p.volumeMappings, kApfsFormatXid, blockers)},
        {p.extentRef,
         buildEmptyVariableTreeBlock(
             request.block_size_bytes, p.extentRef, kApfsObjectSubtypeExtentRef, blockers)},
        {p.snapMeta,
         buildEmptyVariableTreeBlock(
             request.block_size_bytes, p.snapMeta, kApfsObjectSubtypeSnapMeta, blockers)},
        {p.rootTree, buildEmptyRootTreeBlock(request.block_size_bytes, blockers, p.enc.enabled)},
        {p.volSuper,
         buildVolumeSuperblock({.blockSize = request.block_size_bytes,
                                .volumeName = volumeName,
                                .volumeUuid = p.volumeUuid,
                                .allocatedBlocks = kApfsFormatVolumeBaseAllocatedBlocks,
                                .volumeOmapBlock = p.volOmap,
                                .extentRefTreeBlock = p.extentRef,
                                .snapMetaTreeBlock = p.snapMeta,
                                .fsFlags = p.enc.fsFlags},
                               blockers)},
        {p.containerOmap,
         buildObjectMapBlock({.blockSize = request.block_size_bytes,
                              .oid = p.containerOmap,
                              .treeBlock = p.containerOmapTree,
                              .xid = kApfsFormatXid,
                              .omapFlags = 1},
                             blockers)},
        {p.containerOmapTree,
         buildObjectMapTreeBlock(request.block_size_bytes,
                                 p.containerOmapTree,
                                 p.containerMappings,
                                 kApfsFormatXid,
                                 blockers)}};
}

// Builds the base block set (block 0 through the container object-map tree) for an
// empty container from the resolved plan. Byte-identical to the inline initializer
// list it replaces.
QVector<ApfsImageBlock> buildEmptyFormatBaseBlocks(const ApfsEmptyFormatPlan& p,
                                                   const PartitionApfsImageFormatRequest& request,
                                                   const QString& volumeName,
                                                   QStringList* blockers) {
    QVector<ApfsImageBlock> blocks = buildEmptyFormatCheckpointBlocks(p, request, blockers);
    blocks += buildEmptyFormatVolumeBlocks(p, request, volumeName, blockers);
    return blocks;
}

QVector<ApfsImageBlock> emptyFormatBlocks(const PartitionApfsImageFormatRequest& request,
                                          uint64_t blockCount,
                                          const QString& volumeName,
                                          QStringList* blockers) {
    const ApfsEmptyFormatPlan p = computeEmptyFormatPlan(request, blockCount, blockers);
    QVector<ApfsImageBlock> blocks = buildEmptyFormatBaseBlocks(p, request, volumeName, blockers);
    appendExtraVolumeBlocks(&blocks, request, p.extras, blockers);
    appendSpacemanContinuationBlocks(&blocks,
                                     request,
                                     {.spacemanBlocks = p.spacemanBlocks,
                                      .genesisSpacemanBlock = p.genesisSpacemanBlock,
                                      .liveSpacemanBlock = p.liveSpacemanBlock,
                                      .genesisSpacemanObj = p.genesisSpacemanObj,
                                      .liveSpacemanObj = p.liveSpacemanObj});
    appendInternalPoolBlocks({.blocks = &blocks,
                              .request = request,
                              .blockCount = blockCount,
                              .chunkCount = p.chunkCount,
                              .mcib = p.mcib,
                              .ghostReserved = p.ghostReserved,
                              .reservedSeed = p.reservedSeed,
                              .seedData = p.seedData},
                             blockers);
    // A6: AES-XTS-encrypt the volume file-system tree with the VEK (its omap value
    // already carries OMAP_VAL_ENCRYPTED) and place the two keybag blocks. Everything
    // else - the container, the volume superblock, the object maps, the extent-ref /
    // snap-meta trees - stays plaintext, exactly as a real macOS FileVault volume.
    applyFormatEncryption(&blocks, p.enc, p.rootTree);
    return blocks;
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
        // The whole-image SHA-256 scans the entire container. Above the single-chunk
        // envelope (multi-CIB / metadata-overflow / CAB tiers, up to terabytes) that
        // is a multi-minute read of mostly-sparse space and adds no certification
        // value - those tiers are certified by Apple fsck_apfs, not the hash. Skip it
        // for large targets; small image-only builds keep the integrity hash.
        const qint64 imageBytes = QFileInfo(result->image_path).size();
        if (imageBytes >= 0 &&
            static_cast<uint64_t>(imageBytes) <= kGeneratedApfsSingleChunkMaxBytes) {
            result->image_sha256 = fileSha256Hex(result->image_path, &result->blockers);
            result->ok = result->blockers.isEmpty();
        }
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
                             .genesis = false,
                             .cibAddrs = {kApfsFormatChunkInfoBlock}},
                            blockers)},
        {kApfsFormatChunkInfoBlock,
         buildChunkInfoBlock({.blockSize = rewrite.blockSize,
                              .blockCount = rewrite.blockCount,
                              .reservedBlocks = allocatedBlocks,
                              .xid = kApfsFormatXid,
                              .selfBlock = kApfsFormatChunkInfoBlock,
                              .bitmapBlock = kApfsFormatChunkBitmapBlock,
                              .chunkStart = 0,
                              .chunkSpan = (rewrite.blockCount + kApfsSpacemanBlocksPerChunk - 1) /
                                           kApfsSpacemanBlocksPerChunk,
                              .cibIndex = 0},
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
                                        QStringList* blockers,
                                        uint64_t maxBytes = kDefaultMaxApfsPayloadBytes) {
    ApfsImageSource source{QFileInfo(path), false};
    if (!source.info.exists() || !source.info.isFile()) {
        blockers->append(QStringLiteral("APFS %1 source image is required").arg(purpose));
        return source;
    }
    if (source.info.size() <= 0) {
        blockers->append(QStringLiteral("APFS %1 source image is empty").arg(purpose));
        return source;
    }
    if (static_cast<uint64_t>(source.info.size()) > maxBytes) {
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
    // Preserve sparseness: a multi-TiB container has only its metadata written, so the
    // scratch copies in the size of its allocated data, not its logical size.
    QString copyError;
    if (copyFileSparse(sourcePath, outputPath, &copyError)) {
        return true;
    }
    blockers->append(
        QStringLiteral("Unable to create APFS %1 scratch image: %2").arg(purpose, copyError));
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

// Test-only seam (installed via PartitionApfsWriter::setRawDeviceTargetPredicateForTesting):
// when set, this predicate replaces isWindowsRawDevicePath for classifying a raw-device
// commit target, so unit tests can drive the production commitRaw* orchestration against a
// temporary file while every other production guard (explicit confirmation, raw opt-in,
// non-image-only options, size alignment, APFS detection) still runs unchanged. Null in
// production, where the real Windows raw-device rule is the sole classifier.
std::function<bool(const QString&)>& rawDeviceTargetPredicate() {
    static std::function<bool(const QString&)> predicate;
    return predicate;
}

bool rawTargetPathAccepted(const QString& path) {
    const auto& predicate = rawDeviceTargetPredicate();
    return predicate ? predicate(path) : isWindowsRawDevicePath(path);
}

void appendRawTargetMutationBlockers(const RawTargetMutationBlockersContext& context,
                                     QStringList* blockers) {
    if (context.targetPath.trimmed().isEmpty()) {
        blockers->append(
            QStringLiteral("APFS raw %1 target path is required").arg(context.purpose));
    }
    if (!rawTargetPathAccepted(context.targetPath)) {
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
    // The whole-target SHA-256 reads the entire container; on a multi-CIB / metadata-overflow
    // container (held as a sparse image) that means reading terabytes of free space and
    // dominates the format. Skip it above the single-chunk size -- those tiers are certified
    // via fsck_apfs + kernel mount, not a whole-image hash -- and keep it for the small
    // generated containers whose evidence chain is the image hash.
    const bool hashWholeTarget = !isWindowsRawDevicePath(result.image_path) &&
                                 request.target_container_bytes <=
                                     kGeneratedApfsSingleChunkMaxBytes;
    finalizeExistingFormatResult(&result, hashWholeTarget);
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

// Copies the source to the scratch image, opens it, runs the root-file write, and
// finalizes the result. Byte-identical to the inline tail it replaces (early-returns
// on copy/open failure without finalizing, exactly as before).
void runImageOnlyRootFileScratchWrite(const PartitionApfsImageRootFileWriteRequest& request,
                                      const QString& cleanFileName,
                                      const PartitionApfsFileReadResult& rootListing,
                                      PartitionApfsImageFileWriteResult* result) {
    if (!copyToScratchImage(result->source_image_path,
                            result->written_image_path,
                            QLatin1StringView("file-write"),
                            &result->blockers)) {
        return;
    }

    QFile image;
    if (!openScratchImage(result->written_image_path,
                          QLatin1StringView("file-write"),
                          &image,
                          &result->blockers)) {
        return;
    }

    QStringList writeBlockers;
    runRootFileWriteOnScratch({.image = &image,
                               .sourceImagePath = result->source_image_path,
                               .fileName = cleanFileName,
                               .fileData = request.file_data,
                               .rootListing = rootListing,
                               .writtenDataBlocks = &result->written_data_blocks},
                              &writeBlockers);
    image.close();
    result->blockers.append(writeBlockers);
    appendFileWriteReadback(result->written_image_path, cleanFileName, request.file_data, result);
    finalizeFileWriteResult(result);
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

    runImageOnlyRootFileScratchWrite(request, cleanFileName, *rootListing, &result);
    return result;
}

// Copies the source to the scratch image, opens it, runs the root-file delete, and
// finalizes the result. Byte-identical to the inline tail it replaces (early-returns
// on copy/open failure without finalizing, exactly as before).
void runImageOnlyRootFileScratchDelete(const QString& cleanFileName,
                                       const PartitionApfsFileReadResult& rootListing,
                                       PartitionApfsImageFileDeleteResult* result) {
    if (!copyToScratchImage(result->source_image_path,
                            result->written_image_path,
                            QLatin1StringView("file-delete"),
                            &result->blockers)) {
        return;
    }

    QFile image;
    if (!openScratchImage(result->written_image_path,
                          QLatin1StringView("file-delete"),
                          &image,
                          &result->blockers)) {
        return;
    }

    QStringList deleteBlockers;
    runRootFileDeleteOnScratch({.image = &image,
                                .sourceImagePath = result->source_image_path,
                                .fileName = cleanFileName,
                                .rootListing = rootListing,
                                .targetSizeBytes = result->deleted_file_bytes,
                                .freedDataBlocks = &result->freed_data_blocks},
                               &deleteBlockers);
    image.close();
    result->blockers.append(deleteBlockers);
    appendFileDeleteReadback(result->written_image_path, cleanFileName, result);
    finalizeFileDeleteResult(result);
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

    runImageOnlyRootFileScratchDelete(cleanFileName, *rootListing, &result);
    return result;
}

// Copies the source to the scratch image, opens it, runs the root-directory child
// rewrite, and finalizes the result. Byte-identical to the inline tail it replaces
// (early-returns on copy/open failure without finalizing, exactly as before).
void runImageOnlyRootDirectoryFileScratchWrite(
    const PartitionApfsImageRootDirectoryFileWriteRequest& request,
    const QString& cleanDirectoryName,
    const QString& cleanFileName,
    const PartitionApfsFileReadResult& rootListing,
    PartitionApfsImageFileWriteResult* result) {
    if (!copyToScratchImage(result->source_image_path,
                            result->written_image_path,
                            QLatin1StringView("directory-file-write"),
                            &result->blockers)) {
        return;
    }

    QFile image;
    if (!openScratchImage(result->written_image_path,
                          QLatin1StringView("directory-file-write"),
                          &image,
                          &result->blockers)) {
        return;
    }
    QStringList writeBlockers;
    runRootDirectoryFileRewriteOnScratch({.image = &image,
                                          .sourceImagePath = result->source_image_path,
                                          .directoryName = cleanDirectoryName,
                                          .fileName = cleanFileName,
                                          .fileData = request.file_data,
                                          .rootListing = rootListing,
                                          .changedDataBlocks = &result->written_data_blocks},
                                         &writeBlockers);
    image.close();
    result->blockers.append(writeBlockers);
    appendDirectoryFileWriteReadback(
        result->written_image_path, cleanDirectoryName, cleanFileName, request.file_data, result);
    finalizeFileWriteResult(result);
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
    runImageOnlyRootDirectoryFileScratchWrite(
        request, cleanDirectoryName, cleanFileName, *rootListing, &result);
    return result;
}

// Copies the source to the scratch image, opens it, runs the root-directory child
// delete (rewrite with deleteFile=true), and finalizes the result. Byte-identical to
// the inline tail it replaces (early-returns on copy/open failure, exactly as before).
void runImageOnlyRootDirectoryFileScratchDelete(const QString& cleanDirectoryName,
                                                const QString& cleanFileName,
                                                const PartitionApfsFileReadResult& rootListing,
                                                PartitionApfsImageFileDeleteResult* result) {
    if (!copyToScratchImage(result->source_image_path,
                            result->written_image_path,
                            QLatin1StringView("directory-file-delete"),
                            &result->blockers)) {
        return;
    }

    QFile image;
    if (!openScratchImage(result->written_image_path,
                          QLatin1StringView("directory-file-delete"),
                          &image,
                          &result->blockers)) {
        return;
    }
    QStringList deleteBlockers;
    runRootDirectoryFileRewriteOnScratch({.image = &image,
                                          .sourceImagePath = result->source_image_path,
                                          .directoryName = cleanDirectoryName,
                                          .fileName = cleanFileName,
                                          .rootListing = rootListing,
                                          .targetSizeBytes = result->deleted_file_bytes,
                                          .changedDataBlocks = &result->freed_data_blocks,
                                          .deleteFile = true},
                                         &deleteBlockers);
    image.close();
    result->blockers.append(deleteBlockers);
    appendDirectoryFileDeleteReadback(
        result->written_image_path, cleanDirectoryName, cleanFileName, result);
    finalizeFileDeleteResult(result);
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
    runImageOnlyRootDirectoryFileScratchDelete(
        cleanDirectoryName, cleanFileName, *rootListing, &result);
    return result;
}

// Copies the source to the scratch image, opens it, runs the root-directory child
// patch (rewrite with the patched payload), and finalizes the result. Byte-identical
// to the inline tail it replaces (early-returns on copy/open failure, exactly as
// before).
void runImageOnlyRootDirectoryFileScratchPatch(const QString& cleanDirectoryName,
                                               const QString& cleanFileName,
                                               const QByteArray& patchedData,
                                               const PartitionApfsFileReadResult& rootListing,
                                               PartitionApfsImageFilePatchResult* result) {
    if (!copyToScratchImage(result->source_image_path,
                            result->written_image_path,
                            QLatin1StringView("directory-file-patch"),
                            &result->blockers)) {
        return;
    }

    QFile image;
    if (!openScratchImage(result->written_image_path,
                          QLatin1StringView("directory-file-patch"),
                          &image,
                          &result->blockers)) {
        return;
    }
    QStringList patchBlockers;
    runRootDirectoryFileRewriteOnScratch({.image = &image,
                                          .sourceImagePath = result->source_image_path,
                                          .directoryName = cleanDirectoryName,
                                          .fileName = cleanFileName,
                                          .fileData = patchedData,
                                          .rootListing = rootListing,
                                          .changedDataBlocks = &result->written_data_blocks},
                                         &patchBlockers);
    image.close();
    result->blockers.append(patchBlockers);
    appendDirectoryFilePatchReadback(
        result->written_image_path, cleanDirectoryName, cleanFileName, patchedData, result);
    finalizeFilePatchResult(result);
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
    runImageOnlyRootDirectoryFileScratchPatch(
        cleanDirectoryName, cleanFileName, patchedData, *rootListing, &result);
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

// Copies the source to the scratch image, opens it, runs the root-directory create
// or delete rewrite (createDirectory selects which), reads it back, and finalizes the
// result. Byte-identical to the inline create/delete tails it replaces (the
// operation label and createDirectory flag are the only differences; the readback
// is selected by createDirectory exactly as the two inline tails did).
void runImageOnlyRootDirectoryScratchMutation(QLatin1StringView operation,
                                              const QString& cleanDirectoryName,
                                              const PartitionApfsFileReadResult& rootListing,
                                              bool createDirectory,
                                              PartitionApfsImageDirectoryMutationResult* result) {
    if (!copyToScratchImage(
            result->source_image_path, result->written_image_path, operation, &result->blockers)) {
        return;
    }

    QFile image;
    if (!openScratchImage(result->written_image_path, operation, &image, &result->blockers)) {
        return;
    }
    QStringList writeBlockers;
    runRootDirectoryRewriteOnScratch(
        {
            .image = &image,
            .sourceImagePath = result->source_image_path,
            .directoryName = cleanDirectoryName,
            .rootListing = rootListing,
            .createDirectory = createDirectory,
        },
        &writeBlockers);
    image.close();
    result->blockers.append(writeBlockers);
    if (createDirectory) {
        appendDirectoryCreateReadback(result->written_image_path, cleanDirectoryName, result);
    } else {
        appendDirectoryDeleteReadback(result->written_image_path, cleanDirectoryName, result);
    }
    finalizeDirectoryMutationResult(result);
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
    runImageOnlyRootDirectoryScratchMutation(
        QLatin1StringView("directory-create"), cleanDirectoryName, *rootListing, true, &result);
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
    runImageOnlyRootDirectoryScratchMutation(
        QLatin1StringView("directory-delete"), cleanDirectoryName, *rootListing, false, &result);
    return result;
}

// Copies the source to the scratch image, opens it, changes the volume label, reads
// it back, and finalizes the result. Byte-identical to the inline tail it replaces
// (early-returns on copy/open failure without finalizing, exactly as before).
void runImageOnlyVolumeLabelScratchChange(uint64_t sourceSizeBytes,
                                          PartitionApfsImageVolumeLabelResult* result) {
    if (!copyToScratchImage(result->source_image_path,
                            result->written_image_path,
                            QLatin1StringView("volume-label"),
                            &result->blockers)) {
        return;
    }

    QFile image;
    if (!openScratchImage(result->written_image_path,
                          QLatin1StringView("volume-label"),
                          &image,
                          &result->blockers)) {
        return;
    }
    QStringList labelBlockers;
    runVolumeLabelChangeOnTarget(
        &image, sourceSizeBytes, result->new_volume_name, &result->old_volume_name, &labelBlockers);
    image.close();
    result->blockers.append(labelBlockers);
    appendVolumeLabelReadback(result->written_image_path,
                              result->new_volume_name,
                              QLatin1StringView("volume-label"),
                              &result->blockers);
    finalizeVolumeLabelResult(result);
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

    runImageOnlyVolumeLabelScratchChange(static_cast<uint64_t>(source.info.size()), &result);
    return result;
}

// Opens the raw file-write target read-write, runs the root-file write, reads it
// back, and sets result->ok. Byte-identical to the inline tail it replaces
// (early-returns with the open-error blocker on open failure, exactly as before).
void runRawRootFileTargetWrite(const PartitionApfsRawRootFileWriteRequest& request,
                               const QString& cleanFileName,
                               const PartitionApfsFileReadResult& rootListing,
                               PartitionApfsRawFileWriteResult* result) {
    QString openError;
    auto target = openFileOrRawDeviceReadWrite(result->target_path, &openError);
    if (!target) {
        result->blockers.append(
            QStringLiteral("Unable to open APFS raw file-write target: %1").arg(openError));
        return;
    }
    QStringList writeBlockers;
    runRootFileWriteOnScratch({.image = target.get(),
                               .sourceImagePath = result->target_path,
                               .fileName = cleanFileName,
                               .fileData = request.file_data,
                               .rootListing = rootListing,
                               .writtenDataBlocks = &result->written_data_blocks},
                              &writeBlockers);
    target->close();
    result->blockers.append(writeBlockers);
    appendRawFileWriteReadback(result->target_path, cleanFileName, request.file_data, result);
    result->ok = result->blockers.isEmpty();
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

    runRawRootFileTargetWrite(request, cleanFileName, *rootListing, &result);
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

// Opens the raw directory-file-write target read-write, runs the root-directory
// child rewrite, reads it back, and sets result->ok. Byte-identical to the inline
// tail it replaces (early-returns with the open-error blocker on open failure).
void runRawRootDirectoryFileTargetWrite(
    const PartitionApfsRawRootDirectoryFileWriteRequest& request,
    const QString& cleanDirectoryName,
    const QString& cleanFileName,
    const PartitionApfsFileReadResult& rootListing,
    PartitionApfsRawFileWriteResult* result) {
    QString openError;
    auto target = openFileOrRawDeviceReadWrite(result->target_path, &openError);
    if (!target) {
        result->blockers.append(
            QStringLiteral("Unable to open APFS raw directory-file-write target: %1")
                .arg(openError));
        return;
    }
    QStringList writeBlockers;
    runRootDirectoryFileRewriteOnScratch({.image = target.get(),
                                          .sourceImagePath = result->target_path,
                                          .directoryName = cleanDirectoryName,
                                          .fileName = cleanFileName,
                                          .fileData = request.file_data,
                                          .rootListing = rootListing,
                                          .changedDataBlocks = &result->written_data_blocks},
                                         &writeBlockers);
    target->close();
    result->blockers.append(writeBlockers);
    appendRawDirectoryFileWriteReadback(
        result->target_path, cleanDirectoryName, cleanFileName, request.file_data, result);
    result->ok = result->blockers.isEmpty();
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

    runRawRootDirectoryFileTargetWrite(
        request, cleanDirectoryName, cleanFileName, *rootListing, &result);
    return result;
}

// Opens the raw directory-file-delete target read-write, runs the root-directory
// child delete (rewrite with deleteFile=true), reads it back, and sets result->ok.
// Byte-identical to the inline tail it replaces (early-returns with the open-error
// blocker on open failure).
void runRawRootDirectoryFileTargetDelete(const QString& cleanDirectoryName,
                                         const QString& cleanFileName,
                                         const PartitionApfsFileReadResult& rootListing,
                                         PartitionApfsRawFileDeleteResult* result) {
    QString openError;
    auto target = openFileOrRawDeviceReadWrite(result->target_path, &openError);
    if (!target) {
        result->blockers.append(
            QStringLiteral("Unable to open APFS raw directory-file-delete target: %1")
                .arg(openError));
        return;
    }
    QStringList deleteBlockers;
    runRootDirectoryFileRewriteOnScratch({.image = target.get(),
                                          .sourceImagePath = result->target_path,
                                          .directoryName = cleanDirectoryName,
                                          .fileName = cleanFileName,
                                          .rootListing = rootListing,
                                          .targetSizeBytes = result->deleted_file_bytes,
                                          .changedDataBlocks = &result->freed_data_blocks,
                                          .deleteFile = true},
                                         &deleteBlockers);
    target->close();
    result->blockers.append(deleteBlockers);
    appendRawDirectoryFileDeleteReadback(
        result->target_path, cleanDirectoryName, cleanFileName, result);
    result->ok = result->blockers.isEmpty();
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

    runRawRootDirectoryFileTargetDelete(cleanDirectoryName, cleanFileName, *rootListing, &result);
    return result;
}

// Opens the raw directory-file-patch target read-write, runs the root-directory
// child rewrite with the patched payload, reads it back, and sets result->ok.
// Byte-identical to the inline tail it replaces (early-returns with the open-error
// blocker on open failure).
void runRawRootDirectoryFileTargetPatch(const QString& cleanDirectoryName,
                                        const QString& cleanFileName,
                                        const QByteArray& patchedData,
                                        const PartitionApfsFileReadResult& rootListing,
                                        PartitionApfsRawFilePatchResult* result) {
    QString openError;
    auto target = openFileOrRawDeviceReadWrite(result->target_path, &openError);
    if (!target) {
        result->blockers.append(
            QStringLiteral("Unable to open APFS raw directory-file-patch target: %1")
                .arg(openError));
        return;
    }
    QStringList patchBlockers;
    runRootDirectoryFileRewriteOnScratch({.image = target.get(),
                                          .sourceImagePath = result->target_path,
                                          .directoryName = cleanDirectoryName,
                                          .fileName = cleanFileName,
                                          .fileData = patchedData,
                                          .rootListing = rootListing,
                                          .changedDataBlocks = &result->written_data_blocks},
                                         &patchBlockers);
    target->close();
    result->blockers.append(patchBlockers);
    appendRawDirectoryFilePatchReadback(
        result->target_path, cleanDirectoryName, cleanFileName, patchedData, result);
    result->ok = result->blockers.isEmpty();
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

    runRawRootDirectoryFileTargetPatch(
        cleanDirectoryName, cleanFileName, patchedData, *rootListing, &result);
    return result;
}

// Opens the raw file-patch target read-write, runs the root-file rewrite with the
// patched payload, reads it back, and sets result->ok. Byte-identical to the inline
// tail it replaces (early-returns with the open-error blocker on open failure).
void runRawRootFileTargetPatch(const QString& cleanFileName,
                               const QByteArray& patchedData,
                               const PartitionApfsFileReadResult& rootListing,
                               PartitionApfsRawFilePatchResult* result) {
    QString openError;
    auto target = openFileOrRawDeviceReadWrite(result->target_path, &openError);
    if (!target) {
        result->blockers.append(
            QStringLiteral("Unable to open APFS raw file-patch target: %1").arg(openError));
        return;
    }
    QStringList patchBlockers;
    runRootFileWriteOnScratch({.image = target.get(),
                               .sourceImagePath = result->target_path,
                               .fileName = cleanFileName,
                               .fileData = patchedData,
                               .rootListing = rootListing,
                               .writtenDataBlocks = &result->written_data_blocks},
                              &patchBlockers);
    target->close();
    result->blockers.append(patchBlockers);
    appendRawFilePatchReadback(result->target_path, cleanFileName, patchedData, result);
    result->ok = result->blockers.isEmpty();
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

    runRawRootFileTargetPatch(cleanFileName, patchedData, *rootListing, &result);
    return result;
}

// Opens the raw directory-create/delete target read-write, runs the root-directory
// rewrite (createDirectory selects which), reads it back, and sets result->ok.
// Byte-identical to the inline create/delete tails it replaces (the open-error
// blocker uses `operation`; the read-back check and message are selected by
// createDirectory exactly as the two inline tails did).
void runRawRootDirectoryTargetMutation(QLatin1StringView operation,
                                       const QString& cleanDirectoryName,
                                       const PartitionApfsFileReadResult& rootListing,
                                       bool createDirectory,
                                       PartitionApfsRawDirectoryMutationResult* result) {
    QString openError;
    auto target = openFileOrRawDeviceReadWrite(result->target_path, &openError);
    if (!target) {
        result->blockers.append(
            QStringLiteral("Unable to open APFS raw %1 target: %2").arg(operation).arg(openError));
        return;
    }
    QStringList writeBlockers;
    runRootDirectoryRewriteOnScratch(
        {
            .image = target.get(),
            .sourceImagePath = result->target_path,
            .directoryName = cleanDirectoryName,
            .rootListing = rootListing,
            .createDirectory = createDirectory,
        },
        &writeBlockers);
    target->close();
    result->blockers.append(writeBlockers);
    if (result->blockers.isEmpty()) {
        const auto entry = rootDirectoryReadbackEntry(
            result->target_path, cleanDirectoryName, operation, &result->blockers);
        if (createDirectory) {
            if (!entry.has_value() || !entry->directory) {
                result->blockers.append(QStringLiteral(
                    "APFS raw directory-create read-back did not find the target directory"));
            }
        } else {
            if (entry.has_value()) {
                result->blockers.append(QStringLiteral(
                    "APFS raw directory-delete read-back still found target directory"));
            }
        }
    }
    result->ok = result->blockers.isEmpty();
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

    runRawRootDirectoryTargetMutation(
        QLatin1StringView("directory-create"), cleanDirectoryName, *rootListing, true, &result);
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

    runRawRootDirectoryTargetMutation(
        QLatin1StringView("directory-delete"), cleanDirectoryName, *rootListing, false, &result);
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

// Reads the repair geometry from the open target, validates it against the expected
// container size, and (if the generated-layout guard passes) repairs the object
// checksums, recording the scanned/repaired counts. Appends any repair blockers to
// result->blockers. Byte-identical to the inline scan body it replaces.
void runRawRepairChecksumScanOnTarget(QIODevice* target,
                                      uint64_t targetContainerBytes,
                                      PartitionApfsRawRepairResult* result) {
    uint32_t blockSize = 0;
    uint64_t blockCount = 0;
    QStringList repairBlockers;
    if (readApfsRepairGeometry(target, &blockSize, &blockCount, &repairBlockers)) {
        if (blockSize != kSupportedApfsBlockSizeBytes ||
            blockCount != targetContainerBytes / blockSize) {
            repairBlockers.append(QStringLiteral(
                "APFS raw repair target geometry does not match expected target size"));
        } else if (!appendGeneratedApfsLayoutBlockers(target,
                                                      {.blockSize = blockSize,
                                                       .blockCount = blockCount},
                                                      QLatin1StringView("repair"),
                                                      true,
                                                      &repairBlockers)) {
            result->scanned_blocks = 0;
            result->repaired_checksum_blocks = 0;
        } else {
            ApfsRepairCounters counters;
            repairGeneratedApfsObjectChecksumBlocks(target,
                                                    {.blockSize = blockSize,
                                                     .blockCount = blockCount},
                                                    &counters,
                                                    &repairBlockers);
            result->scanned_blocks = counters.scannedBlocks;
            result->repaired_checksum_blocks = counters.repairedBlocks;
        }
    }
    result->blockers.append(repairBlockers);
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

    runRawRepairChecksumScanOnTarget(target.get(), request.target_container_bytes, &result);
    target->close();
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

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitImageOnlyResize(
    const PartitionApfsImageResizeCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();
    result.warnings.append(QStringLiteral(
        "Generated APFS in-place resize remains image-only and is not exposed to user actions; "
        "raw in-place commit support requires fsck_apfs/diskutil, crash replay, and hardware "
        "evidence"));

    const auto source = validateImageOnlySource(result.source_image_path,
                                                QLatin1StringView("resize-commit"),
                                                &result.blockers);
    if (!source.ok) {
        return result;
    }
    if (!appendSeparateOutputBlockers(source.info,
                                      result.written_image_path,
                                      QLatin1StringView("resize-commit"),
                                      &result.blockers)) {
        return result;
    }
    if (!detectApfsImageSource(result.source_image_path,
                               static_cast<uint64_t>(source.info.size()),
                               QLatin1StringView("resize-commit"),
                               &result.blockers)
             .has_value()) {
        return result;
    }
    if (!copyToScratchImage(result.source_image_path,
                            result.written_image_path,
                            QLatin1StringView("resize-commit"),
                            &result.blockers)) {
        return result;
    }

    QFile image;
    if (!openScratchImage(result.written_image_path,
                          QLatin1StringView("resize-commit"),
                          &image,
                          &result.blockers)) {
        return result;
    }
    const uint64_t growDelta =
        resolveResizeGrowDelta(&image, request.new_size_bytes, &result.blockers);
    if (growDelta == 0) {
        image.close();
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceResizeGrow(
            &image, growDelta, /*deviceAlreadySized=*/false, &commit, &commitBlockers)) {
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

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitImageOnlyDirectoryCreate(
    const PartitionApfsImageRootDirectoryMutationRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();

    const QString cleanDirectoryName = request.directory_name.trimmed();
    if (!appendRootDirectoryNameBlockers(
            cleanDirectoryName, QLatin1StringView("directory-create-commit"), &result.blockers)) {
        return result;
    }
    const auto source = validateImageOnlySource(result.source_image_path,
                                                QLatin1StringView("directory-create-commit"),
                                                &result.blockers,
                                                kApfsInPlaceCommitMaxBytes);
    if (!source.ok) {
        return result;
    }
    if (!appendSeparateOutputBlockers(source.info,
                                      result.written_image_path,
                                      QLatin1StringView("directory-create-commit"),
                                      &result.blockers)) {
        return result;
    }
    if (!detectApfsImageSource(result.source_image_path,
                               static_cast<uint64_t>(source.info.size()),
                               QLatin1StringView("directory-create-commit"),
                               &result.blockers)
             .has_value()) {
        return result;
    }
    QVector<ApfsRootFilePayload> existingFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    if (!collectExistingFullFsTree(result.source_image_path,
                                   cleanDirectoryName,
                                   &existingFiles,
                                   &directories,
                                   &result.blockers)) {
        return result;
    }
    if (!copyToScratchImage(result.source_image_path,
                            result.written_image_path,
                            QLatin1StringView("directory-create-commit"),
                            &result.blockers)) {
        return result;
    }
    QFile image;
    if (!openScratchImage(result.written_image_path,
                          QLatin1StringView("directory-create-commit"),
                          &image,
                          &result.blockers)) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceDirectoryCreate(
            &image, {existingFiles, directories, cleanDirectoryName}, &commit, &commitBlockers)) {
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

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitImageOnlyDirectoryDelete(
    const PartitionApfsImageRootDirectoryMutationRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();
    const QString cleanDirectoryName = request.directory_name.trimmed();
    const QLatin1StringView purpose("directory-delete-commit");
    const auto source = validateImageOnlySource(
        result.source_image_path, purpose, &result.blockers, kApfsInPlaceCommitMaxBytes);
    if (!source.ok ||
        !appendSeparateOutputBlockers(
            source.info, result.written_image_path, purpose, &result.blockers) ||
        !detectApfsImageSource(result.source_image_path,
                               static_cast<uint64_t>(source.info.size()),
                               purpose,
                               &result.blockers)
             .has_value()) {
        return result;
    }
    QVector<ApfsRootFilePayload> existingFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    if (!collectFullFsTree(
            result.source_image_path, &existingFiles, &directories, &result.blockers) ||
        !copyToScratchImage(
            result.source_image_path, result.written_image_path, purpose, &result.blockers)) {
        return result;
    }
    QFile image;
    if (!openScratchImage(result.written_image_path, purpose, &image, &result.blockers)) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceDirectoryDelete(
            &image, {existingFiles, directories, cleanDirectoryName}, &commit, &commitBlockers)) {
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

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitImageOnlyDirectoryChildWrite(
    const PartitionApfsImageRootDirectoryFileWriteRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();
    const QString cleanDirectoryName = request.directory_name.trimmed();
    const QString cleanFileName = request.file_name.trimmed();
    const QLatin1StringView purpose("directory-child-write-commit");
    if (static_cast<uint64_t>(request.file_data.size()) > kApfsMaximumSeedFileBytes) {
        result.blockers.append(QStringLiteral(
            "APFS directory-child-write-commit payload exceeds the current size cap"));
        return result;
    }
    const auto source = validateImageOnlySource(
        result.source_image_path, purpose, &result.blockers, kApfsInPlaceCommitMaxBytes);
    if (!source.ok ||
        !appendSeparateOutputBlockers(
            source.info, result.written_image_path, purpose, &result.blockers) ||
        !detectApfsImageSource(result.source_image_path,
                               static_cast<uint64_t>(source.info.size()),
                               purpose,
                               &result.blockers)
             .has_value()) {
        return result;
    }
    QVector<ApfsRootFilePayload> allFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    if (!collectFullFsTree(result.source_image_path, &allFiles, &directories, &result.blockers)) {
        return result;
    }
    const uint64_t parentId = resolveDirectoryId(directories, cleanDirectoryName);
    if (parentId == 0) {
        result.blockers.append(
            QStringLiteral("APFS directory-child-write-commit: directory '%1' was not found")
                .arg(cleanDirectoryName));
        return result;
    }
    if (!copyToScratchImage(
            result.source_image_path, result.written_image_path, purpose, &result.blockers)) {
        return result;
    }
    QFile image;
    if (!openScratchImage(result.written_image_path, purpose, &image, &result.blockers)) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceRootFileWrite(
            &image,
            {allFiles, cleanFileName, request.file_data, directories, parentId},
            &commit,
            &commitBlockers)) {
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

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitImageOnlyDirectoryChildDelete(
    const PartitionApfsImageRootDirectoryFileDeleteRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();
    const QString cleanDirectoryName = request.directory_name.trimmed();
    const QString cleanFileName = request.file_name.trimmed();
    const QLatin1StringView purpose("directory-child-delete-commit");
    const auto source = validateImageOnlySource(
        result.source_image_path, purpose, &result.blockers, kApfsInPlaceCommitMaxBytes);
    if (!source.ok ||
        !appendSeparateOutputBlockers(
            source.info, result.written_image_path, purpose, &result.blockers) ||
        !detectApfsImageSource(result.source_image_path,
                               static_cast<uint64_t>(source.info.size()),
                               purpose,
                               &result.blockers)
             .has_value()) {
        return result;
    }
    QVector<ApfsRootFilePayload> allFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    if (!collectFullFsTree(result.source_image_path, &allFiles, &directories, &result.blockers)) {
        return result;
    }
    const uint64_t parentId = resolveDirectoryId(directories, cleanDirectoryName);
    if (parentId == 0) {
        result.blockers.append(
            QStringLiteral("APFS directory-child-delete-commit: directory '%1' was not found")
                .arg(cleanDirectoryName));
        return result;
    }
    if (!copyToScratchImage(
            result.source_image_path, result.written_image_path, purpose, &result.blockers)) {
        return result;
    }
    QFile image;
    if (!openScratchImage(result.written_image_path, purpose, &image, &result.blockers)) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceFileDelete(
            &image, {allFiles, directories, cleanFileName, parentId}, &commit, &commitBlockers)) {
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

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitImageOnlyDirectoryChildRename(
    const PartitionApfsImageRootDirectoryFileRenameRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();
    const QString cleanDirectoryName = request.directory_name.trimmed();
    const QString oldName = request.file_name.trimmed();
    const QString newName = request.new_file_name.trimmed();
    const QLatin1StringView purpose("directory-child-rename-commit");
    const auto source = validateImageOnlySource(
        result.source_image_path, purpose, &result.blockers, kApfsInPlaceCommitMaxBytes);
    if (!source.ok ||
        !appendSeparateOutputBlockers(
            source.info, result.written_image_path, purpose, &result.blockers) ||
        !detectApfsImageSource(result.source_image_path,
                               static_cast<uint64_t>(source.info.size()),
                               purpose,
                               &result.blockers)
             .has_value()) {
        return result;
    }
    QVector<ApfsRootFilePayload> allFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    if (!collectFullFsTree(result.source_image_path, &allFiles, &directories, &result.blockers)) {
        return result;
    }
    const uint64_t parentId = resolveDirectoryId(directories, cleanDirectoryName);
    if (parentId == 0) {
        result.blockers.append(
            QStringLiteral("APFS directory-child-rename-commit: directory '%1' was not found")
                .arg(cleanDirectoryName));
        return result;
    }
    if (!copyToScratchImage(
            result.source_image_path, result.written_image_path, purpose, &result.blockers)) {
        return result;
    }
    QFile image;
    if (!openScratchImage(result.written_image_path, purpose, &image, &result.blockers)) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceFileRename(&image,
                                {allFiles, oldName, newName, directories, parentId},
                                &commit,
                                &commitBlockers)) {
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

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitImageOnlyFileMove(
    const PartitionApfsImageFileMoveCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();
    const QString sourceDir = request.source_directory_name.trimmed();
    const QString destDir = request.destination_directory_name.trimmed();
    const QString oldName = request.file_name.trimmed();
    const QString newName = request.new_file_name.trimmed();
    const QLatin1StringView purpose("file-move-commit");
    const auto source = validateImageOnlySource(
        result.source_image_path, purpose, &result.blockers, kApfsInPlaceCommitMaxBytes);
    if (!source.ok ||
        !appendSeparateOutputBlockers(
            source.info, result.written_image_path, purpose, &result.blockers) ||
        !detectApfsImageSource(result.source_image_path,
                               static_cast<uint64_t>(source.info.size()),
                               purpose,
                               &result.blockers)
             .has_value()) {
        return result;
    }
    QVector<ApfsRootFilePayload> allFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    if (!collectFullFsTree(result.source_image_path, &allFiles, &directories, &result.blockers)) {
        return result;
    }
    const uint64_t sourceParent = resolveParentId(directories, sourceDir);
    const uint64_t destParent = resolveParentId(directories, destDir);
    if (sourceParent == 0 || destParent == 0) {
        result.blockers.append(QStringLiteral(
            "APFS file-move-commit: a source or destination directory was not found"));
        return result;
    }
    if (!copyToScratchImage(
            result.source_image_path, result.written_image_path, purpose, &result.blockers)) {
        return result;
    }
    QFile image;
    if (!openScratchImage(result.written_image_path, purpose, &image, &result.blockers)) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceFileRename(&image,
                                {allFiles, oldName, newName, directories, sourceParent, destParent},
                                &commit,
                                &commitBlockers)) {
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

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitImageOnlyFilePatch(
    const PartitionApfsImageFilePatchCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();
    const QString cleanDirectoryName = request.directory_name.trimmed();
    const QString cleanFileName = request.file_name.trimmed();
    const QLatin1StringView purpose("file-patch-commit");
    const auto source = validateImageOnlySource(
        result.source_image_path, purpose, &result.blockers, kApfsInPlaceCommitMaxBytes);
    if (!source.ok ||
        !appendSeparateOutputBlockers(
            source.info, result.written_image_path, purpose, &result.blockers) ||
        !detectApfsImageSource(result.source_image_path,
                               static_cast<uint64_t>(source.info.size()),
                               purpose,
                               &result.blockers)
             .has_value()) {
        return result;
    }
    QVector<ApfsRootFilePayload> allFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    if (!collectFullFsTree(result.source_image_path, &allFiles, &directories, &result.blockers)) {
        return result;
    }
    ApfsFilePatchRequest patch;
    if (!buildFilePatchRequest({result.source_image_path,
                                cleanDirectoryName,
                                cleanFileName,
                                request.patch_offset_bytes,
                                request.patch_data,
                                allFiles,
                                directories},
                               &patch,
                               &result.blockers) ||
        !copyToScratchImage(
            result.source_image_path, result.written_image_path, purpose, &result.blockers)) {
        return result;
    }
    QFile image;
    if (!openScratchImage(result.written_image_path, purpose, &image, &result.blockers)) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceFilePatch(&image, patch, &commit, &commitBlockers)) {
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

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitImageOnlyFileWrite(
    const PartitionApfsImageFileInsertCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();

    const QString cleanFileName = request.file_name.trimmed();
    if (!appendRootFileNameBlockers(
            cleanFileName, QLatin1StringView("file-write-commit"), &result.blockers)) {
        return result;
    }
    if (static_cast<uint64_t>(request.file_data.size()) > kApfsMaximumSeedFileBytes) {
        result.blockers.append(
            QStringLiteral("APFS file-write-commit payload exceeds the current size cap"));
        return result;
    }
    const auto source = validateImageOnlySource(result.source_image_path,
                                                QLatin1StringView("file-write-commit"),
                                                &result.blockers,
                                                kApfsInPlaceCommitMaxBytes);
    if (!source.ok) {
        return result;
    }
    if (!appendSeparateOutputBlockers(source.info,
                                      result.written_image_path,
                                      QLatin1StringView("file-write-commit"),
                                      &result.blockers)) {
        return result;
    }
    if (!detectApfsImageSource(result.source_image_path,
                               static_cast<uint64_t>(source.info.size()),
                               QLatin1StringView("file-write-commit"),
                               &result.blockers)
             .has_value()) {
        return result;
    }
    QVector<ApfsRootFilePayload> allFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    if (!collectFullFsTree(result.source_image_path, &allFiles, &directories, &result.blockers)) {
        return result;
    }
    if (!copyToScratchImage(result.source_image_path,
                            result.written_image_path,
                            QLatin1StringView("file-write-commit"),
                            &result.blockers)) {
        return result;
    }
    QFile image;
    if (!openScratchImage(result.written_image_path,
                          QLatin1StringView("file-write-commit"),
                          &image,
                          &result.blockers)) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceRootFileWrite(&image,
                                   {allFiles, cleanFileName, request.file_data, directories},
                                   &commit,
                                   &commitBlockers)) {
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

// Validates the in-place commit source (using the in-place commit size cap), the
// separate-output guard, and the source-detection guard for the given operation
// label. Returns false (with the appropriate blocker appended) on any failure.
// Byte-identical to the inline validate/output/detect preamble it replaces.
bool validateImageOnlyCommitSource(QLatin1StringView operation,
                                   PartitionApfsImageCheckpointCommitResult* result) {
    const auto source = validateImageOnlySource(
        result->source_image_path, operation, &result->blockers, kApfsInPlaceCommitMaxBytes);
    if (!source.ok) {
        return false;
    }
    if (!appendSeparateOutputBlockers(
            source.info, result->written_image_path, operation, &result->blockers)) {
        return false;
    }
    if (!detectApfsImageSource(result->source_image_path,
                               static_cast<uint64_t>(source.info.size()),
                               operation,
                               &result->blockers)
             .has_value()) {
        return false;
    }
    return true;
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
    if (static_cast<uint64_t>(request.file_data.size()) > kApfsMaximumSeedFileBytes) {
        result.blockers.append(
            QStringLiteral("APFS file-insert-commit payload exceeds the current size cap"));
        return result;
    }
    if (!validateImageOnlyCommitSource(QLatin1StringView("file-insert-commit"), &result)) {
        return result;
    }
    if (!copyToScratchImage(result.source_image_path,
                            result.written_image_path,
                            QLatin1StringView("file-insert-commit"),
                            &result.blockers)) {
        return result;
    }

    QVector<ApfsRootFilePayload> existingFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    if (!collectExistingFullFsTree(result.source_image_path,
                                   cleanFileName,
                                   &existingFiles,
                                   &directories,
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
    runInPlaceFileInsertCommit(&image,
                               {.existingFiles = existingFiles,
                                .fileName = cleanFileName,
                                .fileData = request.file_data,
                                .directories = directories,
                                .compress = request.compress_zlib,
                                .xattrs = request.xattrs,
                                .sparseLogicalSize = request.sparse_logical_size},
                               &result);
    image.close();
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitImageOnlyFileClone(
    const PartitionApfsImageFileCloneCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();
    result.warnings.append(QStringLiteral(
        "Generated APFS in-place file clone remains image-only and is not exposed to user "
        "actions; raw in-place commit support requires fsck_apfs/diskutil, crash replay, and "
        "hardware evidence"));

    const QString sourceName = request.source_file_name.trimmed();
    const QString cloneName = request.clone_file_name.trimmed();
    if (!appendRootFileNameBlockers(
            cloneName, QLatin1StringView("file-clone-commit"), &result.blockers)) {
        return result;
    }
    if (!validateImageOnlyCommitSource(QLatin1StringView("file-clone-commit"), &result)) {
        return result;
    }

    // Collect the live tree (rejecting a clone name that already exists), then locate the
    // source root file and bump its shared data stream's reference count to 2 while marking
    // it WAS_EVER_CLONED -- the clone inode then reuses that dstream and its extents.
    QVector<ApfsRootFilePayload> existingFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    ApfsCloneSourceInfo cloneSource;
    if (!collectExistingFullFsTree(
            result.source_image_path, cloneName, &existingFiles, &directories, &result.blockers) ||
        !prepareCloneSource(&existingFiles, sourceName, &cloneSource, &result.blockers)) {
        return result;
    }

    if (!copyToScratchImage(result.source_image_path,
                            result.written_image_path,
                            QLatin1StringView("file-clone-commit"),
                            &result.blockers)) {
        return result;
    }
    QFile image;
    if (!openScratchImage(result.written_image_path,
                          QLatin1StringView("file-clone-commit"),
                          &image,
                          &result.blockers)) {
        return result;
    }
    runInPlaceFileInsertCommit(&image,
                               {.existingFiles = existingFiles,
                                .fileName = cloneName,
                                .directories = directories,
                                .cloneSourcePrivateId = cloneSource.privateId,
                                .cloneLogicalSize = cloneSource.logicalSize},
                               &result);
    image.close();
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitImageOnlyFileHardlink(
    const PartitionApfsImageFileHardlinkCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();
    result.warnings.append(QStringLiteral(
        "Generated APFS in-place file hard link remains image-only and is not exposed to user "
        "actions; raw in-place commit support requires fsck_apfs/diskutil, crash replay, and "
        "hardware evidence"));

    const QString sourceName = request.source_file_name.trimmed();
    const QString linkName = request.link_file_name.trimmed();
    if (!appendRootFileNameBlockers(
            linkName, QLatin1StringView("file-hardlink-commit"), &result.blockers)) {
        return result;
    }
    if (!validateImageOnlyCommitSource(QLatin1StringView("file-hardlink-commit"), &result)) {
        return result;
    }

    // Collect the live tree (rejecting a link name that already exists) and resolve the
    // source inode the new name links to; the commit adds the name + sibling records.
    QVector<ApfsRootFilePayload> existingFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    if (!collectExistingFullFsTree(
            result.source_image_path, linkName, &existingFiles, &directories, &result.blockers)) {
        return result;
    }
    const uint64_t targetId = resolveHardlinkTargetId(existingFiles, sourceName, &result.blockers);
    if (targetId == 0) {
        return result;
    }

    if (!copyToScratchImage(result.source_image_path,
                            result.written_image_path,
                            QLatin1StringView("file-hardlink-commit"),
                            &result.blockers)) {
        return result;
    }
    QFile image;
    if (!openScratchImage(result.written_image_path,
                          QLatin1StringView("file-hardlink-commit"),
                          &image,
                          &result.blockers)) {
        return result;
    }
    runInPlaceFileInsertCommit(&image,
                               {.existingFiles = existingFiles,
                                .fileName = linkName,
                                .directories = directories,
                                .hardlinkTargetId = targetId},
                               &result);
    image.close();
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitImageOnlyFileDelete(
    const PartitionApfsImageFileDeleteCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();
    result.warnings.append(QStringLiteral(
        "Generated APFS in-place file delete remains image-only and is not exposed to user "
        "actions; raw in-place commit support requires fsck_apfs/diskutil, crash replay, and "
        "hardware evidence"));

    const QString cleanFileName = request.file_name.trimmed();
    if (!appendRootFileNameBlockers(
            cleanFileName, QLatin1StringView("file-delete-commit"), &result.blockers)) {
        return result;
    }
    const auto source = validateImageOnlySource(result.source_image_path,
                                                QLatin1StringView("file-delete-commit"),
                                                &result.blockers,
                                                kApfsInPlaceCommitMaxBytes);
    if (!source.ok) {
        return result;
    }
    if (!appendSeparateOutputBlockers(source.info,
                                      result.written_image_path,
                                      QLatin1StringView("file-delete-commit"),
                                      &result.blockers)) {
        return result;
    }
    if (!detectApfsImageSource(result.source_image_path,
                               static_cast<uint64_t>(source.info.size()),
                               QLatin1StringView("file-delete-commit"),
                               &result.blockers)
             .has_value()) {
        return result;
    }
    QVector<ApfsRootFilePayload> allFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    if (!collectFullFsTree(result.source_image_path, &allFiles, &directories, &result.blockers)) {
        return result;
    }
    if (!copyToScratchImage(result.source_image_path,
                            result.written_image_path,
                            QLatin1StringView("file-delete-commit"),
                            &result.blockers)) {
        return result;
    }

    QFile image;
    if (!openScratchImage(result.written_image_path,
                          QLatin1StringView("file-delete-commit"),
                          &image,
                          &result.blockers)) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceFileDelete(
            &image, {allFiles, directories, cleanFileName}, &commit, &commitBlockers)) {
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

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitImageOnlyFileRename(
    const PartitionApfsImageFileRenameCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();
    result.warnings.append(QStringLiteral(
        "Generated APFS in-place file rename remains image-only and is not exposed to user "
        "actions; raw in-place commit support requires fsck_apfs/diskutil, crash replay, and "
        "hardware evidence"));

    const QString oldName = request.file_name.trimmed();
    const QString newName = request.new_file_name.trimmed();
    if (!appendRootFileNameBlockers(
            oldName, QLatin1StringView("file-rename-commit"), &result.blockers) ||
        !appendRootFileNameBlockers(
            newName, QLatin1StringView("file-rename-commit"), &result.blockers)) {
        return result;
    }
    if (!validateImageOnlyCommitSource(QLatin1StringView("file-rename-commit"), &result)) {
        return result;
    }
    QVector<ApfsRootFilePayload> allFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    if (!collectFullFsTree(result.source_image_path, &allFiles, &directories, &result.blockers)) {
        return result;
    }
    if (!copyToScratchImage(result.source_image_path,
                            result.written_image_path,
                            QLatin1StringView("file-rename-commit"),
                            &result.blockers)) {
        return result;
    }

    QFile image;
    if (!openScratchImage(result.written_image_path,
                          QLatin1StringView("file-rename-commit"),
                          &image,
                          &result.blockers)) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceFileRename(
            &image, {allFiles, oldName, newName, directories}, &commit, &commitBlockers)) {
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

// Gate + open a raw in-place commit target: require explicit destructive
// confirmation + raw opt-in, validate it is a generated APFS container within the
// in-place commit cap, and open the device read-write. Returns nullptr (with
// blockers populated) on any failure. The caller collects existing files from the
// device BEFORE calling this (the reader's read-only handle is released first), so
// no two handles are open on the device at once.
static std::unique_ptr<QIODevice> openRawInPlaceCommitTarget(
    const RawTargetMutationBlockersContext& gate, QStringList* blockers) {
    appendRawTargetMutationBlockers(gate, blockers);
    if (gate.targetBytes > kApfsInPlaceCommitMaxBytes) {
        blockers->append(QStringLiteral("APFS raw %1 container exceeds the in-place commit cap")
                             .arg(gate.purpose));
    }
    if (!blockers->isEmpty()) {
        return nullptr;
    }
    if (!detectApfsRawTarget(gate.targetPath, gate.targetBytes, gate.purpose, blockers)
             .has_value()) {
        return nullptr;
    }
    QString openError;
    auto target = openFileOrRawDeviceReadWrite(gate.targetPath, &openError);
    if (!target) {
        blockers->append(
            QStringLiteral("Unable to open APFS raw commit target: %1").arg(openError));
    }
    return target;
}

void PartitionApfsWriter::setRawDeviceTargetPredicateForTesting(
    std::function<bool(const QString&)> predicate) {
    rawDeviceTargetPredicate() = std::move(predicate);
}

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitRawFileWrite(
    const PartitionApfsRawFileInsertCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.target_path.trimmed();
    result.written_image_path = request.target_path.trimmed();
    const QString cleanFileName = request.file_name.trimmed();
    if (!appendRootFileNameBlockers(
            cleanFileName, QLatin1StringView("raw file-write-commit"), &result.blockers)) {
        return result;
    }
    if (static_cast<uint64_t>(request.file_data.size()) > kApfsMaximumSeedFileBytes) {
        result.blockers.append(
            QStringLiteral("APFS raw file-write-commit payload exceeds the current size cap"));
        return result;
    }
    QVector<ApfsRootFilePayload> allFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    if (!collectFullFsTree(result.written_image_path, &allFiles, &directories, &result.blockers)) {
        return result;
    }
    auto target = openRawInPlaceCommitTarget({.targetPath = result.written_image_path,
                                              .targetBytes = request.target_container_bytes,
                                              .confirmed = request.target_mutation_confirmed,
                                              .allowRawTarget = request.allow_raw_device_target,
                                              .options = &request.options,
                                              .purpose = QLatin1StringView("file-write-commit")},
                                             &result.blockers);
    if (!target) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceRootFileWrite(target.get(),
                                   {allFiles, cleanFileName, request.file_data, directories},
                                   &commit,
                                   &commitBlockers)) {
        result.previous_xid = commit.previous_xid;
        result.new_xid = commit.new_xid;
        result.checkpoint_map_block = commit.checkpoint_map_block;
        result.superblock_block = commit.superblock_block;
    }
    target->close();
    result.blockers.append(commitBlockers);
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitRawFileInsert(
    const PartitionApfsRawFileInsertCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.target_path.trimmed();
    result.written_image_path = request.target_path.trimmed();
    const QString cleanFileName = request.file_name.trimmed();
    if (!appendRootFileNameBlockers(
            cleanFileName, QLatin1StringView("raw file-insert-commit"), &result.blockers)) {
        return result;
    }
    if (static_cast<uint64_t>(request.file_data.size()) > kApfsMaximumSeedFileBytes) {
        result.blockers.append(
            QStringLiteral("APFS raw file-insert-commit payload exceeds the current size cap"));
        return result;
    }
    QVector<ApfsRootFilePayload> existingFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    if (!collectExistingFullFsTree(result.written_image_path,
                                   cleanFileName,
                                   &existingFiles,
                                   &directories,
                                   &result.blockers)) {
        return result;
    }
    auto target = openRawInPlaceCommitTarget({.targetPath = result.written_image_path,
                                              .targetBytes = request.target_container_bytes,
                                              .confirmed = request.target_mutation_confirmed,
                                              .allowRawTarget = request.allow_raw_device_target,
                                              .options = &request.options,
                                              .purpose = QLatin1StringView("file-insert-commit")},
                                             &result.blockers);
    if (!target) {
        return result;
    }
    runInPlaceFileInsertCommit(
        target.get(),
        {existingFiles, cleanFileName, request.file_data, directories, request.compress_zlib},
        &result);
    target->close();
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitRawFileDelete(
    const PartitionApfsRawFileDeleteCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.target_path.trimmed();
    result.written_image_path = request.target_path.trimmed();
    const QString cleanFileName = request.file_name.trimmed();
    if (!appendRootFileNameBlockers(
            cleanFileName, QLatin1StringView("raw file-delete-commit"), &result.blockers)) {
        return result;
    }
    QVector<ApfsRootFilePayload> allFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    if (!collectFullFsTree(result.written_image_path, &allFiles, &directories, &result.blockers)) {
        return result;
    }
    auto target = openRawInPlaceCommitTarget({.targetPath = result.written_image_path,
                                              .targetBytes = request.target_container_bytes,
                                              .confirmed = request.target_mutation_confirmed,
                                              .allowRawTarget = request.allow_raw_device_target,
                                              .options = &request.options,
                                              .purpose = QLatin1StringView("file-delete-commit")},
                                             &result.blockers);
    if (!target) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceFileDelete(
            target.get(), {allFiles, directories, cleanFileName}, &commit, &commitBlockers)) {
        result.previous_xid = commit.previous_xid;
        result.new_xid = commit.new_xid;
        result.checkpoint_map_block = commit.checkpoint_map_block;
        result.superblock_block = commit.superblock_block;
    }
    target->close();
    result.blockers.append(commitBlockers);
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitRawFileRename(
    const PartitionApfsRawFileRenameCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.target_path.trimmed();
    result.written_image_path = request.target_path.trimmed();
    const QString oldName = request.file_name.trimmed();
    const QString newName = request.new_file_name.trimmed();
    if (!appendRootFileNameBlockers(
            oldName, QLatin1StringView("raw file-rename-commit"), &result.blockers) ||
        !appendRootFileNameBlockers(
            newName, QLatin1StringView("raw file-rename-commit"), &result.blockers)) {
        return result;
    }
    QVector<ApfsRootFilePayload> allFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    if (!collectFullFsTree(result.written_image_path, &allFiles, &directories, &result.blockers)) {
        return result;
    }
    auto target = openRawInPlaceCommitTarget({.targetPath = result.written_image_path,
                                              .targetBytes = request.target_container_bytes,
                                              .confirmed = request.target_mutation_confirmed,
                                              .allowRawTarget = request.allow_raw_device_target,
                                              .options = &request.options,
                                              .purpose = QLatin1StringView("file-rename-commit")},
                                             &result.blockers);
    if (!target) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceFileRename(
            target.get(), {allFiles, oldName, newName, directories}, &commit, &commitBlockers)) {
        result.previous_xid = commit.previous_xid;
        result.new_xid = commit.new_xid;
        result.checkpoint_map_block = commit.checkpoint_map_block;
        result.superblock_block = commit.superblock_block;
    }
    target->close();
    result.blockers.append(commitBlockers);
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitRawDirectoryCreate(
    const PartitionApfsRawDirectoryMutationCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.target_path.trimmed();
    result.written_image_path = request.target_path.trimmed();
    const QString cleanDirectoryName = request.directory_name.trimmed();
    if (!appendRootDirectoryNameBlockers(cleanDirectoryName,
                                         QLatin1StringView("raw directory-create-commit"),
                                         &result.blockers)) {
        return result;
    }
    QVector<ApfsRootFilePayload> existingFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    if (!collectExistingFullFsTree(result.written_image_path,
                                   cleanDirectoryName,
                                   &existingFiles,
                                   &directories,
                                   &result.blockers)) {
        return result;
    }
    auto target =
        openRawInPlaceCommitTarget({.targetPath = result.written_image_path,
                                    .targetBytes = request.target_container_bytes,
                                    .confirmed = request.target_mutation_confirmed,
                                    .allowRawTarget = request.allow_raw_device_target,
                                    .options = &request.options,
                                    .purpose = QLatin1StringView("directory-create-commit")},
                                   &result.blockers);
    if (!target) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceDirectoryCreate(target.get(),
                                     {existingFiles, directories, cleanDirectoryName},
                                     &commit,
                                     &commitBlockers)) {
        result.previous_xid = commit.previous_xid;
        result.new_xid = commit.new_xid;
        result.checkpoint_map_block = commit.checkpoint_map_block;
        result.superblock_block = commit.superblock_block;
    }
    target->close();
    result.blockers.append(commitBlockers);
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitRawDirectoryDelete(
    const PartitionApfsRawDirectoryMutationCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.target_path.trimmed();
    result.written_image_path = request.target_path.trimmed();
    const QString cleanDirectoryName = request.directory_name.trimmed();
    if (!appendRootDirectoryNameBlockers(cleanDirectoryName,
                                         QLatin1StringView("raw directory-delete-commit"),
                                         &result.blockers)) {
        return result;
    }
    QVector<ApfsRootFilePayload> existingFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    if (!collectFullFsTree(
            result.written_image_path, &existingFiles, &directories, &result.blockers)) {
        return result;
    }
    auto target =
        openRawInPlaceCommitTarget({.targetPath = result.written_image_path,
                                    .targetBytes = request.target_container_bytes,
                                    .confirmed = request.target_mutation_confirmed,
                                    .allowRawTarget = request.allow_raw_device_target,
                                    .options = &request.options,
                                    .purpose = QLatin1StringView("directory-delete-commit")},
                                   &result.blockers);
    if (!target) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceDirectoryDelete(target.get(),
                                     {existingFiles, directories, cleanDirectoryName},
                                     &commit,
                                     &commitBlockers)) {
        result.previous_xid = commit.previous_xid;
        result.new_xid = commit.new_xid;
        result.checkpoint_map_block = commit.checkpoint_map_block;
        result.superblock_block = commit.superblock_block;
    }
    target->close();
    result.blockers.append(commitBlockers);
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitRawDirectoryChildWrite(
    const PartitionApfsRawDirectoryChildWriteCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.target_path.trimmed();
    result.written_image_path = request.target_path.trimmed();
    const QString cleanDirectoryName = request.directory_name.trimmed();
    const QString cleanFileName = request.file_name.trimmed();
    if (!appendRootFileNameBlockers(cleanFileName,
                                    QLatin1StringView("raw directory-child-write-commit"),
                                    &result.blockers)) {
        return result;
    }
    if (static_cast<uint64_t>(request.file_data.size()) > kApfsMaximumSeedFileBytes) {
        result.blockers.append(QStringLiteral(
            "APFS raw directory-child-write-commit payload exceeds the current size cap"));
        return result;
    }
    QVector<ApfsRootFilePayload> allFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    if (!collectFullFsTree(result.written_image_path, &allFiles, &directories, &result.blockers)) {
        return result;
    }
    const uint64_t parentId = resolveDirectoryId(directories, cleanDirectoryName);
    if (parentId == 0) {
        result.blockers.append(
            QStringLiteral("APFS raw directory-child-write-commit: directory '%1' was not found")
                .arg(cleanDirectoryName));
        return result;
    }
    auto target =
        openRawInPlaceCommitTarget({.targetPath = result.written_image_path,
                                    .targetBytes = request.target_container_bytes,
                                    .confirmed = request.target_mutation_confirmed,
                                    .allowRawTarget = request.allow_raw_device_target,
                                    .options = &request.options,
                                    .purpose = QLatin1StringView("directory-child-write-commit")},
                                   &result.blockers);
    if (!target) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceRootFileWrite(
            target.get(),
            {allFiles, cleanFileName, request.file_data, directories, parentId},
            &commit,
            &commitBlockers)) {
        result.previous_xid = commit.previous_xid;
        result.new_xid = commit.new_xid;
        result.checkpoint_map_block = commit.checkpoint_map_block;
        result.superblock_block = commit.superblock_block;
    }
    target->close();
    result.blockers.append(commitBlockers);
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitRawDirectoryChildDelete(
    const PartitionApfsRawDirectoryChildDeleteCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.target_path.trimmed();
    result.written_image_path = request.target_path.trimmed();
    const QString cleanDirectoryName = request.directory_name.trimmed();
    const QString cleanFileName = request.file_name.trimmed();
    if (!appendRootFileNameBlockers(cleanFileName,
                                    QLatin1StringView("raw directory-child-delete-commit"),
                                    &result.blockers)) {
        return result;
    }
    QVector<ApfsRootFilePayload> allFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    if (!collectFullFsTree(result.written_image_path, &allFiles, &directories, &result.blockers)) {
        return result;
    }
    const uint64_t parentId = resolveDirectoryId(directories, cleanDirectoryName);
    if (parentId == 0) {
        result.blockers.append(
            QStringLiteral("APFS raw directory-child-delete-commit: directory '%1' was not found")
                .arg(cleanDirectoryName));
        return result;
    }
    auto target =
        openRawInPlaceCommitTarget({.targetPath = result.written_image_path,
                                    .targetBytes = request.target_container_bytes,
                                    .confirmed = request.target_mutation_confirmed,
                                    .allowRawTarget = request.allow_raw_device_target,
                                    .options = &request.options,
                                    .purpose = QLatin1StringView("directory-child-delete-commit")},
                                   &result.blockers);
    if (!target) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceFileDelete(target.get(),
                                {allFiles, directories, cleanFileName, parentId},
                                &commit,
                                &commitBlockers)) {
        result.previous_xid = commit.previous_xid;
        result.new_xid = commit.new_xid;
        result.checkpoint_map_block = commit.checkpoint_map_block;
        result.superblock_block = commit.superblock_block;
    }
    target->close();
    result.blockers.append(commitBlockers);
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitRawDirectoryChildRename(
    const PartitionApfsRawDirectoryChildRenameCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.target_path.trimmed();
    result.written_image_path = request.target_path.trimmed();
    const QString cleanDirectoryName = request.directory_name.trimmed();
    const QString oldName = request.file_name.trimmed();
    const QString newName = request.new_file_name.trimmed();
    if (!appendRootFileNameBlockers(
            oldName, QLatin1StringView("raw directory-child-rename-commit"), &result.blockers) ||
        !appendRootFileNameBlockers(
            newName, QLatin1StringView("raw directory-child-rename-commit"), &result.blockers)) {
        return result;
    }
    QVector<ApfsRootFilePayload> allFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    if (!collectFullFsTree(result.written_image_path, &allFiles, &directories, &result.blockers)) {
        return result;
    }
    const uint64_t parentId = resolveDirectoryId(directories, cleanDirectoryName);
    if (parentId == 0) {
        result.blockers.append(
            QStringLiteral("APFS raw directory-child-rename-commit: directory '%1' was not found")
                .arg(cleanDirectoryName));
        return result;
    }
    auto target =
        openRawInPlaceCommitTarget({.targetPath = result.written_image_path,
                                    .targetBytes = request.target_container_bytes,
                                    .confirmed = request.target_mutation_confirmed,
                                    .allowRawTarget = request.allow_raw_device_target,
                                    .options = &request.options,
                                    .purpose = QLatin1StringView("directory-child-rename-commit")},
                                   &result.blockers);
    if (!target) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceFileRename(target.get(),
                                {allFiles, oldName, newName, directories, parentId},
                                &commit,
                                &commitBlockers)) {
        result.previous_xid = commit.previous_xid;
        result.new_xid = commit.new_xid;
        result.checkpoint_map_block = commit.checkpoint_map_block;
        result.superblock_block = commit.superblock_block;
    }
    target->close();
    result.blockers.append(commitBlockers);
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitRawFileMove(
    const PartitionApfsRawFileMoveCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.target_path.trimmed();
    result.written_image_path = request.target_path.trimmed();
    const QString sourceDir = request.source_directory_name.trimmed();
    const QString destDir = request.destination_directory_name.trimmed();
    const QString oldName = request.file_name.trimmed();
    const QString newName = request.new_file_name.trimmed();
    if (!appendRootFileNameBlockers(
            oldName, QLatin1StringView("raw file-move-commit"), &result.blockers) ||
        !appendRootFileNameBlockers(
            newName, QLatin1StringView("raw file-move-commit"), &result.blockers)) {
        return result;
    }
    QVector<ApfsRootFilePayload> allFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    if (!collectFullFsTree(result.written_image_path, &allFiles, &directories, &result.blockers)) {
        return result;
    }
    const uint64_t sourceParent = resolveParentId(directories, sourceDir);
    const uint64_t destParent = resolveParentId(directories, destDir);
    if (sourceParent == 0 || destParent == 0) {
        result.blockers.append(QStringLiteral(
            "APFS raw file-move-commit: a source or destination directory was not found"));
        return result;
    }
    auto target = openRawInPlaceCommitTarget({.targetPath = result.written_image_path,
                                              .targetBytes = request.target_container_bytes,
                                              .confirmed = request.target_mutation_confirmed,
                                              .allowRawTarget = request.allow_raw_device_target,
                                              .options = &request.options,
                                              .purpose = QLatin1StringView("file-move-commit")},
                                             &result.blockers);
    if (!target) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceFileRename(target.get(),
                                {allFiles, oldName, newName, directories, sourceParent, destParent},
                                &commit,
                                &commitBlockers)) {
        result.previous_xid = commit.previous_xid;
        result.new_xid = commit.new_xid;
        result.checkpoint_map_block = commit.checkpoint_map_block;
        result.superblock_block = commit.superblock_block;
    }
    target->close();
    result.blockers.append(commitBlockers);
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitRawFilePatch(
    const PartitionApfsRawFilePatchCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.target_path.trimmed();
    result.written_image_path = request.target_path.trimmed();
    const QString cleanDirectoryName = request.directory_name.trimmed();
    const QString cleanFileName = request.file_name.trimmed();
    if (!appendRootFileNameBlockers(
            cleanFileName, QLatin1StringView("raw file-patch-commit"), &result.blockers)) {
        return result;
    }
    QVector<ApfsRootFilePayload> allFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    if (!collectFullFsTree(result.written_image_path, &allFiles, &directories, &result.blockers)) {
        return result;
    }
    ApfsFilePatchRequest patch;
    if (!buildFilePatchRequest({result.written_image_path,
                                cleanDirectoryName,
                                cleanFileName,
                                request.patch_offset_bytes,
                                request.patch_data,
                                allFiles,
                                directories},
                               &patch,
                               &result.blockers)) {
        return result;
    }
    auto target = openRawInPlaceCommitTarget({.targetPath = result.written_image_path,
                                              .targetBytes = request.target_container_bytes,
                                              .confirmed = request.target_mutation_confirmed,
                                              .allowRawTarget = request.allow_raw_device_target,
                                              .options = &request.options,
                                              .purpose = QLatin1StringView("file-patch-commit")},
                                             &result.blockers);
    if (!target) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceFilePatch(target.get(), patch, &commit, &commitBlockers)) {
        result.previous_xid = commit.previous_xid;
        result.new_xid = commit.new_xid;
        result.checkpoint_map_block = commit.checkpoint_map_block;
        result.superblock_block = commit.superblock_block;
    }
    target->close();
    result.blockers.append(commitBlockers);
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitRawFileClone(
    const PartitionApfsRawFileCloneCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.target_path.trimmed();
    result.written_image_path = request.target_path.trimmed();
    const QString sourceName = request.source_file_name.trimmed();
    const QString cloneName = request.clone_file_name.trimmed();
    if (!appendRootFileNameBlockers(
            cloneName, QLatin1StringView("raw file-clone-commit"), &result.blockers)) {
        return result;
    }
    // Collect the live tree (rejecting a clone name that already exists) and bump the source
    // file's shared data stream's reference count -- identical to commitImageOnlyFileClone but
    // applied to a confirmed raw device in place.
    QVector<ApfsRootFilePayload> existingFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    ApfsCloneSourceInfo cloneSource;
    if (!collectExistingFullFsTree(
            result.written_image_path, cloneName, &existingFiles, &directories, &result.blockers) ||
        !prepareCloneSource(&existingFiles, sourceName, &cloneSource, &result.blockers)) {
        return result;
    }
    auto target = openRawInPlaceCommitTarget({.targetPath = result.written_image_path,
                                              .targetBytes = request.target_container_bytes,
                                              .confirmed = request.target_mutation_confirmed,
                                              .allowRawTarget = request.allow_raw_device_target,
                                              .options = &request.options,
                                              .purpose = QLatin1StringView("file-clone-commit")},
                                             &result.blockers);
    if (!target) {
        return result;
    }
    runInPlaceFileInsertCommit(target.get(),
                               {.existingFiles = existingFiles,
                                .fileName = cloneName,
                                .directories = directories,
                                .cloneSourcePrivateId = cloneSource.privateId,
                                .cloneLogicalSize = cloneSource.logicalSize},
                               &result);
    target->close();
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitRawFileHardlink(
    const PartitionApfsRawFileHardlinkCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.target_path.trimmed();
    result.written_image_path = request.target_path.trimmed();
    const QString sourceName = request.source_file_name.trimmed();
    const QString linkName = request.link_file_name.trimmed();
    if (!appendRootFileNameBlockers(
            linkName, QLatin1StringView("raw file-hardlink-commit"), &result.blockers)) {
        return result;
    }
    // Collect the live tree (rejecting a link name that already exists) and resolve the source
    // inode the new name links to -- identical to commitImageOnlyFileHardlink applied in place.
    QVector<ApfsRootFilePayload> existingFiles;
    QVector<ApfsRootDirectoryPayload> directories;
    if (!collectExistingFullFsTree(
            result.written_image_path, linkName, &existingFiles, &directories, &result.blockers)) {
        return result;
    }
    const uint64_t targetId = resolveHardlinkTargetId(existingFiles, sourceName, &result.blockers);
    if (targetId == 0) {
        return result;
    }
    auto target = openRawInPlaceCommitTarget({.targetPath = result.written_image_path,
                                              .targetBytes = request.target_container_bytes,
                                              .confirmed = request.target_mutation_confirmed,
                                              .allowRawTarget = request.allow_raw_device_target,
                                              .options = &request.options,
                                              .purpose = QLatin1StringView("file-hardlink-commit")},
                                             &result.blockers);
    if (!target) {
        return result;
    }
    runInPlaceFileInsertCommit(target.get(),
                               {.existingFiles = existingFiles,
                                .fileName = linkName,
                                .directories = directories,
                                .hardlinkTargetId = targetId},
                               &result);
    target->close();
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitRawResize(
    const PartitionApfsRawResizeCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.target_path.trimmed();
    result.written_image_path = request.target_path.trimmed();
    auto target = openRawInPlaceCommitTarget({.targetPath = result.written_image_path,
                                              .targetBytes = request.target_container_bytes,
                                              .confirmed = request.target_mutation_confirmed,
                                              .allowRawTarget = request.allow_raw_device_target,
                                              .options = &request.options,
                                              .purpose = QLatin1StringView("resize-commit")},
                                             &result.blockers);
    if (!target) {
        return result;
    }
    // The device (partition) already spans the new size; the grow only raises the container's
    // claim into that span (a raw block device cannot itself be resized).
    const uint64_t growDelta =
        resolveResizeGrowDelta(target.get(), request.new_size_bytes, &result.blockers);
    if (growDelta == 0) {
        target->close();
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceResizeGrow(
            target.get(), growDelta, /*deviceAlreadySized=*/true, &commit, &commitBlockers)) {
        result.previous_xid = commit.previous_xid;
        result.new_xid = commit.new_xid;
        result.checkpoint_map_block = commit.checkpoint_map_block;
        result.superblock_block = commit.superblock_block;
    }
    target->close();
    result.blockers.append(commitBlockers);
    result.ok = result.blockers.isEmpty();
    return result;
}

namespace {
// APFS snapshot timestamps are nanoseconds since 1970-01-01 UTC. A request that
// supplies 0 is stamped with the current wall-clock time.
uint64_t resolveSnapshotCreateTimeNs(uint64_t requested) {
    if (requested != 0) {
        return requested;
    }
    const qint64 msecs = QDateTime::currentMSecsSinceEpoch();
    return static_cast<uint64_t>(msecs) * 1'000'000ULL;
}
}  // namespace

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitImageOnlySnapshotCreate(
    const PartitionApfsImageSnapshotCreateCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();
    const QString cleanName = request.snapshot_name.trimmed();
    const QLatin1StringView purpose("snapshot-create-commit");
    if (cleanName.isEmpty()) {
        result.blockers.append(
            QStringLiteral("APFS snapshot-create-commit: the snapshot name is empty"));
        return result;
    }
    const auto source = validateImageOnlySource(
        result.source_image_path, purpose, &result.blockers, kApfsInPlaceCommitMaxBytes);
    if (!source.ok ||
        !appendSeparateOutputBlockers(
            source.info, result.written_image_path, purpose, &result.blockers) ||
        !detectApfsImageSource(result.source_image_path,
                               static_cast<uint64_t>(source.info.size()),
                               purpose,
                               &result.blockers)
             .has_value()) {
        return result;
    }
    if (!copyToScratchImage(
            result.source_image_path, result.written_image_path, purpose, &result.blockers)) {
        return result;
    }
    QFile image;
    if (!openScratchImage(result.written_image_path, purpose, &image, &result.blockers)) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceSnapshotCreate(&image,
                                    {cleanName,
                                     resolveSnapshotCreateTimeNs(request.create_time_ns)},
                                    &commit,
                                    &commitBlockers)) {
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

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitRawSnapshotCreate(
    const PartitionApfsRawSnapshotCreateCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.target_path.trimmed();
    result.written_image_path = request.target_path.trimmed();
    const QString cleanName = request.snapshot_name.trimmed();
    if (cleanName.isEmpty()) {
        result.blockers.append(
            QStringLiteral("APFS raw snapshot-create-commit: the snapshot name is empty"));
        return result;
    }
    auto target =
        openRawInPlaceCommitTarget({.targetPath = result.written_image_path,
                                    .targetBytes = request.target_container_bytes,
                                    .confirmed = request.target_mutation_confirmed,
                                    .allowRawTarget = request.allow_raw_device_target,
                                    .options = &request.options,
                                    .purpose = QLatin1StringView("snapshot-create-commit")},
                                   &result.blockers);
    if (!target) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceSnapshotCreate(target.get(),
                                    {cleanName,
                                     resolveSnapshotCreateTimeNs(request.create_time_ns)},
                                    &commit,
                                    &commitBlockers)) {
        result.previous_xid = commit.previous_xid;
        result.new_xid = commit.new_xid;
        result.checkpoint_map_block = commit.checkpoint_map_block;
        result.superblock_block = commit.superblock_block;
    }
    target->close();
    result.blockers.append(commitBlockers);
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitImageOnlySnapshotDelete(
    const PartitionApfsImageSnapshotDeleteCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();
    const QLatin1StringView purpose("snapshot-delete-commit");
    const auto source = validateImageOnlySource(
        result.source_image_path, purpose, &result.blockers, kApfsInPlaceCommitMaxBytes);
    if (!source.ok ||
        !appendSeparateOutputBlockers(
            source.info, result.written_image_path, purpose, &result.blockers) ||
        !detectApfsImageSource(result.source_image_path,
                               static_cast<uint64_t>(source.info.size()),
                               purpose,
                               &result.blockers)
             .has_value()) {
        return result;
    }
    if (!copyToScratchImage(
            result.source_image_path, result.written_image_path, purpose, &result.blockers)) {
        return result;
    }
    QFile image;
    if (!openScratchImage(result.written_image_path, purpose, &image, &result.blockers)) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceSnapshotDelete(&image, &commit, &commitBlockers)) {
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

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitRawSnapshotDelete(
    const PartitionApfsRawSnapshotDeleteCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.target_path.trimmed();
    result.written_image_path = request.target_path.trimmed();
    auto target =
        openRawInPlaceCommitTarget({.targetPath = result.written_image_path,
                                    .targetBytes = request.target_container_bytes,
                                    .confirmed = request.target_mutation_confirmed,
                                    .allowRawTarget = request.allow_raw_device_target,
                                    .options = &request.options,
                                    .purpose = QLatin1StringView("snapshot-delete-commit")},
                                   &result.blockers);
    if (!target) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceSnapshotDelete(target.get(), &commit, &commitBlockers)) {
        result.previous_xid = commit.previous_xid;
        result.new_xid = commit.new_xid;
        result.checkpoint_map_block = commit.checkpoint_map_block;
        result.superblock_block = commit.superblock_block;
    }
    target->close();
    result.blockers.append(commitBlockers);
    result.ok = result.blockers.isEmpty();
    return result;
}

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitImageOnlySnapshotRevert(
    const PartitionApfsImageSnapshotRevertCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.source_image_path.trimmed();
    result.written_image_path = request.written_image_path.trimmed();
    const QLatin1StringView purpose("snapshot-revert-commit");
    const auto source = validateImageOnlySource(
        result.source_image_path, purpose, &result.blockers, kApfsInPlaceCommitMaxBytes);
    if (!source.ok ||
        !appendSeparateOutputBlockers(
            source.info, result.written_image_path, purpose, &result.blockers) ||
        !detectApfsImageSource(result.source_image_path,
                               static_cast<uint64_t>(source.info.size()),
                               purpose,
                               &result.blockers)
             .has_value()) {
        return result;
    }
    if (!copyToScratchImage(
            result.source_image_path, result.written_image_path, purpose, &result.blockers)) {
        return result;
    }
    QFile image;
    if (!openScratchImage(result.written_image_path, purpose, &image, &result.blockers)) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceSnapshotRevert(&image, &commit, &commitBlockers)) {
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

PartitionApfsImageCheckpointCommitResult PartitionApfsWriter::commitRawSnapshotRevert(
    const PartitionApfsRawSnapshotRevertCommitRequest& request) {
    PartitionApfsImageCheckpointCommitResult result;
    result.source_image_path = request.target_path.trimmed();
    result.written_image_path = request.target_path.trimmed();
    auto target =
        openRawInPlaceCommitTarget({.targetPath = result.written_image_path,
                                    .targetBytes = request.target_container_bytes,
                                    .confirmed = request.target_mutation_confirmed,
                                    .allowRawTarget = request.allow_raw_device_target,
                                    .options = &request.options,
                                    .purpose = QLatin1StringView("snapshot-revert-commit")},
                                   &result.blockers);
    if (!target) {
        return result;
    }
    ApfsInPlaceCheckpointResult commit;
    QStringList commitBlockers;
    if (commitInPlaceSnapshotRevert(target.get(), &commit, &commitBlockers)) {
        result.previous_xid = commit.previous_xid;
        result.new_xid = commit.new_xid;
        result.checkpoint_map_block = commit.checkpoint_map_block;
        result.superblock_block = commit.superblock_block;
    }
    target->close();
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

QVector<QByteArray> PartitionApfsWriter::buildFsTreeNodeBlocks(uint32_t block_size,
                                                               const QStringList& file_names,
                                                               uint64_t first_leaf_oid,
                                                               QStringList* blockers) {
    QStringList localBlockers;
    QStringList* sink = blockers != nullptr ? blockers : &localBlockers;
    QVector<ApfsRootFilePayload> files;
    files.reserve(file_names.size());
    for (qsizetype index = 0; index < file_names.size(); ++index) {
        files.append({.fileName = file_names.at(index),
                      .parentDirectoryId = kApfsRootDirectoryId,
                      .fileId = kApfsFirstUserObjectId + static_cast<uint64_t>(index),
                      .privateId = kApfsFirstUserObjectId + static_cast<uint64_t>(index)});
    }
    QVector<ApfsFsTreeNode> nodes;
    if (!buildFsTreeNodes({block_size, files, {}, first_leaf_oid}, &nodes, sink)) {
        return {};
    }
    QVector<QByteArray> blocks;
    blocks.reserve(nodes.size());
    for (const auto& node : nodes) {
        blocks.append(node.block);
    }
    return blocks;
}

QPair<quint64, quint64> PartitionApfsWriter::nextCrashSafeIpSlot(quint64 live_cib,
                                                                 quint64 block_count) {
    const ApfsIpSlot slot = nextIpSlot(live_cib, computeGeneratedLayout(block_count));
    return {slot.cib, slot.bitmap};
}

quint64 PartitionApfsWriter::readGeneratedLiveCibAddr(const QString& image_path) {
    QFile image(image_path);
    if (!image.open(QIODevice::ReadOnly)) {
        return 0;
    }
    uint32_t blockSize = 0;
    uint64_t blockCount = 0;
    QStringList blockers;
    if (!readApfsRepairGeometry(&image, &blockSize, &blockCount, &blockers)) {
        return 0;
    }
    const ApfsRepairGeometry geometry{blockSize, blockCount};
    QByteArray nxsb(blockSize, '\0');
    if (!readApfsRepairBlock(&image, geometry, kApfsFormatNxsbBlock, &nxsb, &blockers)) {
        return 0;
    }
    return readLiveSpacemanCibAddr(&image, geometry, nxsb, &blockers);
}

}  // namespace sak

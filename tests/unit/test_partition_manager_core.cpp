// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_partition_manager_core.cpp
/// @brief Unit tests for Partition Manager core planning and safety.

#include "sak/apfs_crypto.h"
#include "sak/apfs_keybag.h"
#include "sak/file_recovery_engine.h"
#include "sak/partition_apfs_file_system_reader.h"
#include "sak/partition_apfs_writer.h"
#include "sak/partition_ext_file_system_reader.h"
#include "sak/partition_file_system_detector.h"
#include "sak/partition_file_system_registry.h"
#include "sak/partition_file_system_tool_manifest.h"
#include "sak/partition_file_system_tool_runner.h"
#include "sak/partition_hfs_file_system_reader.h"
#include "sak/partition_operation_planner.h"
#include "sak/partition_operation_queue.h"
#include "sak/partition_script_builder.h"
#include "sak/storage_inventory_worker.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest/QtTest>

#include <algorithm>
#include <tuple>

using namespace sak;

namespace {

constexpr int kExpectedRecoveredFixtureCount = 2;
constexpr qsizetype kTestExtSuperblockOffset = 1024;
constexpr qsizetype kTestExtInodesCountOffset = 0x0;
constexpr qsizetype kTestExtBlocksCountLoOffset = 0x4;
constexpr qsizetype kTestExtFreeBlocksCountLoOffset = 0xC;
constexpr qsizetype kTestExtFreeInodesCountOffset = 0x10;
constexpr qsizetype kTestExtFirstDataBlockOffset = 0x14;
constexpr qsizetype kTestExtLogBlockSizeOffset = 0x18;
constexpr qsizetype kTestExtBlocksPerGroupOffset = 0x20;
constexpr qsizetype kTestExtInodesPerGroupOffset = 0x28;
constexpr qsizetype kTestExtMagicOffset = 0x38;
constexpr qsizetype kTestExtFeatureCompatOffset = 0x5C;
constexpr qsizetype kTestExtFeatureIncompatOffset = 0x60;
constexpr qsizetype kTestExtFeatureRoCompatOffset = 0x64;
constexpr qsizetype kTestExtVolumeNameOffset = 0x78;
constexpr qsizetype kTestExtInodeSizeOffset = 0x58;
constexpr qsizetype kTestExtGroupDescriptorInodeTableLoOffset = 0x08;
constexpr qsizetype kTestExtInodeModeOffset = 0x00;
constexpr qsizetype kTestExtInodeSizeLoOffset = 0x04;
constexpr qsizetype kTestExtInodeFlagsOffset = 0x20;
constexpr qsizetype kTestExtInodeBlocksOffset = 0x28;
constexpr qsizetype kTestExtInodeSizeHiOffset = 0x6C;
constexpr qsizetype kTestXfsBlockSizeOffset = 4;
constexpr qsizetype kTestXfsDataBlocksOffset = 8;
constexpr qsizetype kTestXfsUuidOffset = 32;
constexpr qsizetype kTestXfsAgBlocksOffset = 84;
constexpr qsizetype kTestXfsAgCountOffset = 88;
constexpr qsizetype kTestXfsVersionOffset = 100;
constexpr qsizetype kTestXfsSectorSizeOffset = 102;
constexpr qsizetype kTestXfsInodeSizeOffset = 104;
constexpr qsizetype kTestXfsNameOffset = 108;
constexpr qsizetype kTestXfsInodeCountOffset = 128;
constexpr qsizetype kTestXfsFreeInodeCountOffset = 136;
constexpr qsizetype kTestXfsFreeDataBlocksOffset = 144;
constexpr qsizetype kTestXfsFeatures2Offset = 200;
constexpr qsizetype kTestBtrfsSuperblockOffset = 64 * 1024;
constexpr qsizetype kTestBtrfsUuidOffset = 0x20;
constexpr qsizetype kTestBtrfsMagicOffset = 64 * 1024 + 0x40;
constexpr qsizetype kTestBtrfsGenerationOffset = 0x48;
constexpr qsizetype kTestBtrfsTotalBytesOffset = 0x70;
constexpr qsizetype kTestBtrfsBytesUsedOffset = 0x78;
constexpr qsizetype kTestBtrfsNumDevicesOffset = 0x88;
constexpr qsizetype kTestBtrfsSectorSizeOffset = 0x90;
constexpr qsizetype kTestBtrfsNodeSizeOffset = 0x94;
constexpr qsizetype kTestBtrfsLeafSizeOffset = 0x98;
constexpr qsizetype kTestBtrfsCompatFlagsOffset = 0xAC;
constexpr qsizetype kTestBtrfsCompatRoFlagsOffset = 0xB4;
constexpr qsizetype kTestBtrfsIncompatFlagsOffset = 0xBC;
constexpr qsizetype kTestBtrfsLabelOffset = 0x12B;
constexpr qsizetype kTestHfsHeaderOffset = 1024;
constexpr qsizetype kTestApfsMagicOffset = 0x20;
constexpr qsizetype kTestApfsChecksumObjectBytes = 12;
constexpr uint64_t kTestApfsChecksumZeroObject = 0xFF'FF'FF'FF'FF'FF'FF'FFULL;
constexpr uint64_t kTestApfsChecksumOneWordObject = 0x00'00'00'01'FF'FF'FF'FDULL;
constexpr qsizetype kTestApfsChecksumPayloadOffset = 8;
constexpr int kMinimumApfsMutationPlanSteps = 2;
constexpr char kMutatedApfsChecksumPayloadByte = 2;
constexpr qsizetype kTestApfsBlockSizeOffset = 0x24;
constexpr qsizetype kTestApfsBlockCountOffset = 0x28;
constexpr qsizetype kTestApfsFeaturesOffset = 0x30;
constexpr qsizetype kTestApfsReadOnlyCompatibleFeaturesOffset = 0x38;
constexpr qsizetype kTestApfsIncompatibleFeaturesOffset = 0x40;
constexpr qsizetype kTestApfsUuidOffset = 0x48;
constexpr qsizetype kTestApfsNextObjectIdOffset = 0x58;
constexpr qsizetype kTestApfsNextTransactionIdOffset = 0x60;
constexpr qsizetype kTestApfsCheckpointDescriptorBlocksOffset = 0x68;
constexpr qsizetype kTestApfsCheckpointDataBlocksOffset = 0x6C;
constexpr qsizetype kTestApfsCheckpointDescriptorBaseOffset = 0x70;
constexpr qsizetype kTestApfsCheckpointDataBaseOffset = 0x78;
constexpr qsizetype kTestApfsCheckpointDescriptorNextOffset = 0x80;
constexpr qsizetype kTestApfsCheckpointDataNextOffset = 0x84;
constexpr qsizetype kTestApfsCheckpointDescriptorIndexOffset = 0x88;
constexpr qsizetype kTestApfsCheckpointDescriptorLengthOffset = 0x8C;
constexpr qsizetype kTestApfsCheckpointDataIndexOffset = 0x90;
constexpr qsizetype kTestApfsCheckpointDataLengthOffset = 0x94;
constexpr qsizetype kTestApfsSpacemanOidOffset = 0x98;
constexpr qsizetype kTestApfsObjectMapOidOffset = 0xA0;
constexpr qsizetype kTestApfsReaperOidOffset = 0xA8;
constexpr qsizetype kTestApfsMaxFileSystemsOffset = 0xB4;
constexpr qsizetype kTestApfsFileSystemOidArrayOffset = 0xB8;
constexpr uint32_t kTestApfsBlockSize = 4096;
constexpr qsizetype kTestApfsSpacemanBlock = 15;
constexpr qsizetype kTestApfsVolumeCandidateBlock = 16;
constexpr qsizetype kTestApfsObjectMapBlock = 17;
constexpr qsizetype kTestApfsRootTreeBlock = 18;
constexpr qsizetype kTestApfsObjectMapTreeBlock = 19;
constexpr qsizetype kTestApfsSpacemanOffset = kTestApfsSpacemanBlock * kTestApfsBlockSize;
constexpr qsizetype kTestApfsVolumeCandidateOffset = kTestApfsVolumeCandidateBlock *
                                                     kTestApfsBlockSize;
constexpr qsizetype kTestApfsObjectMapOffset = kTestApfsObjectMapBlock * kTestApfsBlockSize;
constexpr qsizetype kTestApfsRootTreeOffset = kTestApfsRootTreeBlock * kTestApfsBlockSize;
constexpr qsizetype kTestApfsObjectMapTreeOffset = kTestApfsObjectMapTreeBlock * kTestApfsBlockSize;
constexpr qsizetype kTestApfsObjectOidOffset = 0x08;
constexpr qsizetype kTestApfsObjectXidOffset = 0x10;
constexpr qsizetype kTestApfsObjectTypeOffset = 0x18;
constexpr qsizetype kTestApfsObjectSubtypeOffset = 0x1C;
constexpr qsizetype kTestApfsOmapFlagsOffset = 0x20;
constexpr qsizetype kTestApfsOmapSnapshotCountOffset = 0x24;
constexpr qsizetype kTestApfsOmapTreeOidOffset = 0x30;
constexpr qsizetype kTestApfsOmapSnapshotTreeOidOffset = 0x38;
constexpr qsizetype kTestApfsOmapMostRecentSnapshotOffset = 0x40;
constexpr qsizetype kTestApfsOmapPendingRevertMinOffset = 0x48;
constexpr qsizetype kTestApfsOmapPendingRevertMaxOffset = 0x50;
constexpr qsizetype kTestApfsSpacemanBlockSizeOffset = 0x20;
constexpr qsizetype kTestApfsSpacemanBlocksPerChunkOffset = 0x24;
constexpr qsizetype kTestApfsSpacemanChunksPerCibOffset = 0x28;
constexpr qsizetype kTestApfsSpacemanMainDeviceOffset = 0x30;
constexpr qsizetype kTestApfsSpacemanDeviceBlockCountOffset = 0x00;
constexpr qsizetype kTestApfsSpacemanDeviceChunkCountOffset = 0x08;
constexpr qsizetype kTestApfsSpacemanDeviceCibCountOffset = 0x10;
constexpr qsizetype kTestApfsSpacemanDeviceFreeCountOffset = 0x18;
constexpr qsizetype kTestApfsBtreeNodeFlagsOffset = 0x20;
constexpr qsizetype kTestApfsBtreeNodeLevelOffset = 0x22;
constexpr qsizetype kTestApfsBtreeNodeKeyCountOffset = 0x24;
constexpr qsizetype kTestApfsBtreeNodeTableSpaceOffsetOffset = 0x28;
constexpr qsizetype kTestApfsBtreeNodeTableSpaceLengthOffset = 0x2A;
constexpr qsizetype kTestApfsBtreeNodeFreeSpaceOffsetOffset = 0x2C;
constexpr qsizetype kTestApfsBtreeNodeFreeSpaceLengthOffset = 0x2E;
constexpr qsizetype kTestApfsBtreeNodeKeyFreeListOffsetOffset = 0x30;
constexpr qsizetype kTestApfsBtreeNodeKeyFreeListLengthOffset = 0x32;
constexpr qsizetype kTestApfsBtreeNodeValueFreeListOffsetOffset = 0x34;
constexpr qsizetype kTestApfsBtreeNodeValueFreeListLengthOffset = 0x36;
constexpr qsizetype kTestApfsBtreeInfoSize = 40;
constexpr qsizetype kTestApfsBtreeInfoFlagsOffset = 0x00;
constexpr qsizetype kTestApfsBtreeInfoNodeSizeOffset = 0x04;
constexpr qsizetype kTestApfsBtreeInfoKeySizeOffset = 0x08;
constexpr qsizetype kTestApfsBtreeInfoValueSizeOffset = 0x0C;
constexpr qsizetype kTestApfsBtreeInfoLongestKeyOffset = 0x10;
constexpr qsizetype kTestApfsBtreeInfoLongestValueOffset = 0x14;
constexpr qsizetype kTestApfsBtreeInfoKeyCountOffset = 0x18;
constexpr qsizetype kTestApfsBtreeInfoNodeCountOffset = 0x20;
constexpr qsizetype kTestApfsVolumeMagicOffset = 0x20;
constexpr qsizetype kTestApfsVolumeIndexOffset = 0x24;
constexpr qsizetype kTestApfsVolumeReserveBlockCountOffset = 0x48;
constexpr qsizetype kTestApfsVolumeQuotaBlockCountOffset = 0x50;
constexpr qsizetype kTestApfsVolumeAllocatedBlockCountOffset = 0x58;
constexpr qsizetype kTestApfsVolumeObjectMapOidOffset = 0x80;
constexpr qsizetype kTestApfsVolumeRootTreeOidOffset = 0x88;
constexpr qsizetype kTestApfsVolumeExtentrefTreeOidOffset = 0x90;
constexpr qsizetype kTestApfsVolumeSnapshotMetadataTreeOidOffset = 0x98;
constexpr qsizetype kTestApfsVolumeUuidOffset = 0xF0;
constexpr qsizetype kTestApfsVolumeNameOffset = 0x2C0;
constexpr qsizetype kTestApfsVolumeRoleOffset = 0x3C4;
constexpr qsizetype kTestSwapSignatureOffset = 4096 - 10;
constexpr qsizetype kTestSwapVersionOffset = 1024;
constexpr qsizetype kTestSwapLastPageOffset = 1028;
constexpr qsizetype kTestSwapBadPagesOffset = 1032;
constexpr qsizetype kTestSwapUuidOffset = 1036;
constexpr qsizetype kTestSwapLabelOffset = 1052;
constexpr qsizetype kTestBootSectorOemOffset = 3;
constexpr qsizetype kTestFat16TypeOffset = 54;
constexpr qsizetype kTestFat32TypeOffset = 82;
constexpr qsizetype kTestBootSectorSignatureOffset = 510;
constexpr qsizetype kTestHfsVersionOffset = 2;
constexpr qsizetype kTestHfsAttributesOffset = 4;
constexpr qsizetype kTestHfsFileCountOffset = 32;
constexpr qsizetype kTestHfsFolderCountOffset = 36;
constexpr qsizetype kTestHfsBlockSizeOffset = 40;
constexpr qsizetype kTestHfsTotalBlocksOffset = 44;
constexpr qsizetype kTestHfsFreeBlocksOffset = 48;
constexpr qsizetype kTestHfsWrapperMdbOffset = 1024;
constexpr qsizetype kTestHfsWrapperAllocationBlockSizeOffset = 0x14;
constexpr qsizetype kTestHfsWrapperAllocationBlockStartOffset = 0x1C;
constexpr qsizetype kTestHfsWrapperEmbeddedSignatureOffset = 0x7C;
constexpr qsizetype kTestHfsWrapperEmbeddedExtentStartOffset = 0x7E;
constexpr qsizetype kTestHfsWrapperEmbeddedExtentCountOffset = 0x80;
constexpr qsizetype kTestHfsAllocationForkOffset = 112;
constexpr qsizetype kTestHfsExtentsForkOffset = 192;
constexpr qsizetype kTestHfsCatalogForkOffset = 272;
constexpr qsizetype kTestHfsAttributesForkOffset = 352;
constexpr qsizetype kTestHfsForkLogicalSizeOffset = 0;
constexpr qsizetype kTestHfsForkTotalBlocksOffset = 12;
constexpr qsizetype kTestHfsForkExtentsOffset = 16;
constexpr qsizetype kTestHfsExtentStartBlockOffset = 0;
constexpr qsizetype kTestHfsExtentBlockCountOffset = 4;
constexpr qsizetype kTestHfsCatalogFileDataForkOffset = 88;
constexpr qsizetype kTestHfsForkDataBytes = 80;
constexpr qsizetype kTestHfsCatalogFileResourceForkOffset = kTestHfsCatalogFileDataForkOffset +
                                                            kTestHfsForkDataBytes;
constexpr qsizetype kTestHfsBTreeKindOffset = 8;
constexpr qsizetype kTestHfsBTreeHeightOffset = 9;
constexpr qsizetype kTestHfsBTreeNumRecordsOffset = 10;
constexpr qsizetype kTestHfsBTreeHeaderRecordOffset = 14;
constexpr qsizetype kTestHfsBTreeHeaderTreeDepthOffset = 0;
constexpr qsizetype kTestHfsBTreeHeaderRootNodeOffset = 2;
constexpr qsizetype kTestHfsBTreeHeaderLeafRecordsOffset = 6;
constexpr qsizetype kTestHfsBTreeHeaderFirstLeafNodeOffset = 10;
constexpr qsizetype kTestHfsBTreeHeaderLastLeafNodeOffset = 14;
constexpr qsizetype kTestHfsBTreeHeaderNodeSizeOffset = 18;
constexpr qsizetype kTestHfsBTreeHeaderMaxKeyLengthOffset = 20;
constexpr qsizetype kTestHfsBTreeHeaderTotalNodesOffset = 22;
constexpr qsizetype kTestHfsBTreeHeaderFreeNodesOffset = 26;
constexpr qsizetype kTestHfsBTreeHeaderKeyCompareTypeOffset = 37;
constexpr qsizetype kTestHfsBTreeHeaderAttributesOffset = 38;
constexpr qsizetype kTestHfsBTreeHeaderMapRecordOffset = 248;
constexpr qsizetype kTestHfsBTreeNodeDescriptorSize = 14;
constexpr uint32_t kTestHfsBTreeBigKeysMask = 0x00'00'00'02;
constexpr uint32_t kTestHfsBTreeVariableIndexKeysMask = 0x00'00'00'04;
constexpr uint32_t kTestHfsSplitCatalogTotalNodes = 8;
constexpr uint16_t kTestHfsCatalogMaxKeyLength = 516;
constexpr uint16_t kTestHfsExtentsMaxKeyLength = 10;
constexpr int kTestHfsSplitFixtureFileCount = 14;
constexpr qsizetype kTestHfsCatalogRecordIdOffset = 8;
constexpr qsizetype kTestHfsExtentsKeyLength = 10;
constexpr qsizetype kTestHfsExtentsRecordBytes = 64;
constexpr uint32_t kTestExtCompatHasJournal = 0x0004;
constexpr uint32_t kTestExtIncompatExtents = 0x0040;
constexpr uint32_t kTestExtInodeFlagExtents = 0x00'08'00'00;
constexpr uint32_t kTestHfsJournaledMask = 0x00'00'20'00;
constexpr uint32_t kTestExtBlockSize = 1024;
constexpr uint32_t kTestExtInodeSize = 128;
constexpr qsizetype kTestExtInodeBlockBytes = 60;
constexpr uint32_t kTestExtInodeTableBlock = 5;
constexpr uint32_t kTestExtRootDirectoryBlock = 10;
constexpr uint32_t kTestExtHelloFileBlock = 11;
constexpr uint32_t kTestExtDocsDirectoryBlock = 12;
constexpr uint32_t kTestExtNoteFileBlock = 13;
constexpr uint32_t kTestHfsWrapperAllocationBlockSize = 4096;
constexpr uint16_t kTestHfsWrapperAllocationStartSector = 8;
constexpr uint16_t kTestHfsWrapperEmbeddedStartBlock = 10;
constexpr uint16_t kTestHfsWrapperEmbeddedBlockCount = 64;
constexpr uint32_t kTestHfsBlockSize = 4096;
constexpr uint32_t kTestHfsCatalogStartBlock = 2;
constexpr uint32_t kTestHfsCatalogNodeSize = 4096;
constexpr uint32_t kTestHfsCatalogTotalNodes = 2;
constexpr uint32_t kTestHfsAttributesStartBlock = 50;
constexpr uint32_t kTestHfsAttributesNodeSize = 4096;
constexpr uint32_t kTestHfsAttributesTotalNodes = 2;
constexpr uint32_t kTestHfsHelloFileBlock = 4;
constexpr qsizetype kTestHfsVolumeJournalInfoBlockOffset = 12;
constexpr uint32_t kTestHfsNoteFileBlock = 5;
constexpr uint32_t kTestHfsExtentsStartBlock = 6;
constexpr uint32_t kTestHfsExtentsNodeSize = 1024;
constexpr uint32_t kTestHfsDataForkType = 0x00;
constexpr uint32_t kTestHfsResourceForkType = 0xFF;
constexpr uint32_t kTestHfsCatalogFileId = 4;
constexpr uint32_t kTestHfsAllocationStartBlock = 60;
constexpr uint32_t kTestHfsAttributeInlineRecord = 0x10;
constexpr uint32_t kTestHfsAttributeForkRecord = 0x20;
constexpr uint32_t kTestHfsResourceFileBlock = 9;
constexpr uint32_t kTestHfsAttributeForkValueBlock = 56;

QByteArray fileSha256(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

bool writeBytes(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(bytes) == bytes.size();
}

QByteArray readBytes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

QByteArray approvedToolManifest(const QString& id,
                                const QString& relativePath,
                                const QByteArray& binaryBytes,
                                const QStringList& fileSystems,
                                const QStringList& operations) {
    QJsonObject tool;
    tool.insert(QStringLiteral("id"), id);
    tool.insert(QStringLiteral("display_name"), id);
    tool.insert(QStringLiteral("version"), QStringLiteral("1.0.0"));
    tool.insert(QStringLiteral("upstream_url"), QStringLiteral("https://example.invalid/tool"));
    tool.insert(QStringLiteral("license"), QStringLiteral("MIT"));
    tool.insert(
        QStringLiteral("source_archive_sha256"),
        QString::fromLatin1(
            QCryptographicHash::hash(QByteArray("source"), QCryptographicHash::Sha256).toHex()));
    tool.insert(QStringLiteral("relative_path"), relativePath);
    tool.insert(QStringLiteral("binary_sha256"),
                QString::fromLatin1(
                    QCryptographicHash::hash(binaryBytes, QCryptographicHash::Sha256).toHex()));
    tool.insert(QStringLiteral("file_systems"), QJsonArray::fromStringList(fileSystems));
    tool.insert(QStringLiteral("operations"), QJsonArray::fromStringList(operations));

    QJsonObject root;
    root.insert(QStringLiteral("schema_version"), 1);
    root.insert(QStringLiteral("tools"), QJsonArray{tool});
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QByteArray signatureFixture() {
    return QByteArray(PartitionFileSystemDetector::maxProbeBytes(), '\0');
}

void writeAscii(QByteArray* bytes, qsizetype offset, const char* value) {
    Q_ASSERT(bytes);
    const QByteArray text(value);
    for (qsizetype index = 0; index < text.size(); ++index) {
        (*bytes)[offset + index] = text.at(index);
    }
}

void writeRaw(QByteArray* bytes, qsizetype offset, const QByteArray& value) {
    Q_ASSERT(bytes);
    for (qsizetype index = 0; index < value.size(); ++index) {
        (*bytes)[offset + index] = value.at(index);
    }
}

void writeLe32(QByteArray* bytes, qsizetype offset, uint32_t value) {
    Q_ASSERT(bytes);
    (*bytes)[offset] = static_cast<char>(value & 0xFF);
    (*bytes)[offset + 1] = static_cast<char>((value >> 8) & 0xFF);
    (*bytes)[offset + 2] = static_cast<char>((value >> 16) & 0xFF);
    (*bytes)[offset + 3] = static_cast<char>((value >> 24) & 0xFF);
}

void writeLe16(QByteArray* bytes, qsizetype offset, uint16_t value) {
    Q_ASSERT(bytes);
    (*bytes)[offset] = static_cast<char>(value & 0xFF);
    (*bytes)[offset + 1] = static_cast<char>((value >> 8) & 0xFF);
}

void writeLe64(QByteArray* bytes, qsizetype offset, uint64_t value) {
    Q_ASSERT(bytes);
    (*bytes)[offset] = static_cast<char>(value & 0xFF);
    (*bytes)[offset + 1] = static_cast<char>((value >> 8) & 0xFF);
    (*bytes)[offset + 2] = static_cast<char>((value >> 16) & 0xFF);
    (*bytes)[offset + 3] = static_cast<char>((value >> 24) & 0xFF);
    (*bytes)[offset + 4] = static_cast<char>((value >> 32) & 0xFF);
    (*bytes)[offset + 5] = static_cast<char>((value >> 40) & 0xFF);
    (*bytes)[offset + 6] = static_cast<char>((value >> 48) & 0xFF);
    (*bytes)[offset + 7] = static_cast<char>((value >> 56) & 0xFF);
}

void writeBe16(QByteArray* bytes, qsizetype offset, uint16_t value) {
    Q_ASSERT(bytes);
    (*bytes)[offset] = static_cast<char>((value >> 8) & 0xFF);
    (*bytes)[offset + 1] = static_cast<char>(value & 0xFF);
}

void writeBe32(QByteArray* bytes, qsizetype offset, uint32_t value) {
    Q_ASSERT(bytes);
    (*bytes)[offset] = static_cast<char>((value >> 24) & 0xFF);
    (*bytes)[offset + 1] = static_cast<char>((value >> 16) & 0xFF);
    (*bytes)[offset + 2] = static_cast<char>((value >> 8) & 0xFF);
    (*bytes)[offset + 3] = static_cast<char>(value & 0xFF);
}

uint16_t readBe16(const QByteArray& bytes, qsizetype offset) {
    return qFromBigEndian<uint16_t>(bytes.constData() + offset);
}

uint32_t readBe32(const QByteArray& bytes, qsizetype offset) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(bytes.at(offset))) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(bytes.at(offset + 1))) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(bytes.at(offset + 2))) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(bytes.at(offset + 3)));
}

void writeBe64(QByteArray* bytes, qsizetype offset, uint64_t value) {
    Q_ASSERT(bytes);
    (*bytes)[offset] = static_cast<char>((value >> 56) & 0xFF);
    (*bytes)[offset + 1] = static_cast<char>((value >> 48) & 0xFF);
    (*bytes)[offset + 2] = static_cast<char>((value >> 40) & 0xFF);
    (*bytes)[offset + 3] = static_cast<char>((value >> 32) & 0xFF);
    (*bytes)[offset + 4] = static_cast<char>((value >> 24) & 0xFF);
    (*bytes)[offset + 5] = static_cast<char>((value >> 16) & 0xFF);
    (*bytes)[offset + 6] = static_cast<char>((value >> 8) & 0xFF);
    (*bytes)[offset + 7] = static_cast<char>(value & 0xFF);
}

void writeBootSectorSignature(QByteArray* bytes) {
    Q_ASSERT(bytes);
    (*bytes)[kTestBootSectorSignatureOffset] = static_cast<char>(0x55);
    (*bytes)[kTestBootSectorSignatureOffset + 1] = static_cast<char>(0xAA);
}

qsizetype testExtInodeOffset(uint32_t inodeNumber) {
    return static_cast<qsizetype>(kTestExtInodeTableBlock * kTestExtBlockSize +
                                  (inodeNumber - 1) * kTestExtInodeSize);
}

uint16_t alignedExtRecordLength(qsizetype nameLength) {
    return static_cast<uint16_t>((8 + nameLength + 3) & ~3);
}

struct ExtDirectoryEntryFixture {
    uint32_t inode{0};
    QByteArray name;
    uint8_t file_type{0};
    uint16_t record_length{0};
};

void writeExtDirectoryEntry(QByteArray* bytes,
                            qsizetype offset,
                            const ExtDirectoryEntryFixture& entry) {
    writeLe32(bytes, offset, entry.inode);
    writeLe16(bytes, offset + 4, entry.record_length);
    (*bytes)[offset + 6] = static_cast<char>(entry.name.size());
    (*bytes)[offset + 7] = static_cast<char>(entry.file_type);
    for (qsizetype index = 0; index < entry.name.size(); ++index) {
        (*bytes)[offset + 8 + index] = entry.name.at(index);
    }
}

void writeExtExtentMappedBlock(QByteArray* bytes,
                               qsizetype inodeOffset,
                               uint32_t physicalBlock,
                               uint16_t blockCount) {
    const qsizetype blockMap = inodeOffset + kTestExtInodeBlocksOffset;
    writeLe16(bytes, blockMap, 0xF30A);
    writeLe16(bytes, blockMap + 2, 1);
    writeLe16(bytes, blockMap + 4, 4);
    writeLe16(bytes, blockMap + 6, 0);
    writeLe32(bytes, blockMap + 12, 0);
    writeLe16(bytes, blockMap + 16, blockCount);
    writeLe16(bytes, blockMap + 18, 0);
    writeLe32(bytes, blockMap + 20, physicalBlock);
}

struct ExtInodeFixture {
    uint32_t inode_number{0};
    uint16_t mode{0};
    uint64_t size{0};
    uint32_t first_block{0};
    bool extent_mapped{false};
    QByteArray inline_data;
};

void writeExtInode(QByteArray* bytes, const ExtInodeFixture& inode) {
    const qsizetype offset = testExtInodeOffset(inode.inode_number);
    writeLe16(bytes, offset + kTestExtInodeModeOffset, inode.mode);
    writeLe32(bytes, offset + kTestExtInodeSizeLoOffset, static_cast<uint32_t>(inode.size));
    writeLe32(bytes, offset + kTestExtInodeSizeHiOffset, static_cast<uint32_t>(inode.size >> 32));
    if (inode.extent_mapped) {
        writeLe32(bytes, offset + kTestExtInodeFlagsOffset, kTestExtInodeFlagExtents);
        writeExtExtentMappedBlock(bytes, offset, inode.first_block, 1);
    } else if (!inode.inline_data.isEmpty()) {
        const qsizetype blockMap = offset + kTestExtInodeBlocksOffset;
        for (qsizetype index = 0;
             index < inode.inline_data.size() && index < kTestExtInodeBlockBytes;
             ++index) {
            (*bytes)[blockMap + index] = inode.inline_data.at(index);
        }
    } else {
        writeLe32(bytes, offset + kTestExtInodeBlocksOffset, inode.first_block);
    }
}

void writeExtDirectoryBlock(QByteArray* bytes,
                            uint32_t blockNumber,
                            const QVector<std::tuple<uint32_t, QByteArray, uint8_t>>& entries) {
    qsizetype offset = static_cast<qsizetype>(blockNumber * kTestExtBlockSize);
    qsizetype remaining = kTestExtBlockSize;
    for (int index = 0; index < entries.size(); ++index) {
        const auto& [inode, name, fileType] = entries.at(index);
        const uint16_t recordLength = index == entries.size() - 1
                                          ? static_cast<uint16_t>(remaining)
                                          : alignedExtRecordLength(name.size());
        writeExtDirectoryEntry(bytes,
                               offset,
                               ExtDirectoryEntryFixture{.inode = inode,
                                                        .name = name,
                                                        .file_type = fileType,
                                                        .record_length = recordLength});
        offset += recordLength;
        remaining -= recordLength;
    }
}

QByteArray extReaderFixture(bool extentMappedHello = false) {
    QByteArray image(static_cast<qsizetype>(64 * kTestExtBlockSize), '\0');
    writeLe32(&image, kTestExtSuperblockOffset + kTestExtInodesCountOffset, 64);
    writeLe32(&image, kTestExtSuperblockOffset + kTestExtBlocksCountLoOffset, 64);
    writeLe32(&image, kTestExtSuperblockOffset + kTestExtFirstDataBlockOffset, 1);
    writeLe32(&image, kTestExtSuperblockOffset + kTestExtLogBlockSizeOffset, 0);
    writeLe32(&image, kTestExtSuperblockOffset + kTestExtBlocksPerGroupOffset, 8192);
    writeLe32(&image, kTestExtSuperblockOffset + kTestExtInodesPerGroupOffset, 64);
    writeLe16(&image, kTestExtSuperblockOffset + kTestExtMagicOffset, 0xEF53);
    writeLe16(&image, kTestExtSuperblockOffset + kTestExtInodeSizeOffset, kTestExtInodeSize);
    if (extentMappedHello) {
        writeLe32(&image,
                  kTestExtSuperblockOffset + kTestExtFeatureIncompatOffset,
                  kTestExtIncompatExtents);
    }
    writeLe32(&image,
              2 * kTestExtBlockSize + kTestExtGroupDescriptorInodeTableLoOffset,
              kTestExtInodeTableBlock);

    const QByteArray hello("hello from ext\n");
    const QByteArray note("nested note\n");
    const QByteArray helloLink("hello.txt");
    writeExtInode(&image,
                  ExtInodeFixture{.inode_number = 2,
                                  .mode = 0x4000 | 0755,
                                  .size = kTestExtBlockSize,
                                  .first_block = kTestExtRootDirectoryBlock});
    writeExtInode(&image,
                  ExtInodeFixture{.inode_number = 12,
                                  .mode = 0x8000 | 0644,
                                  .size = static_cast<uint64_t>(hello.size()),
                                  .first_block = kTestExtHelloFileBlock,
                                  .extent_mapped = extentMappedHello});
    writeExtInode(&image,
                  ExtInodeFixture{.inode_number = 13,
                                  .mode = 0x4000 | 0755,
                                  .size = kTestExtBlockSize,
                                  .first_block = kTestExtDocsDirectoryBlock});
    writeExtInode(&image,
                  ExtInodeFixture{.inode_number = 14,
                                  .mode = 0x8000 | 0644,
                                  .size = static_cast<uint64_t>(note.size()),
                                  .first_block = kTestExtNoteFileBlock});
    writeExtInode(&image,
                  ExtInodeFixture{.inode_number = 15,
                                  .mode = 0xA000 | 0777,
                                  .size = static_cast<uint64_t>(helloLink.size()),
                                  .inline_data = helloLink});
    writeExtDirectoryBlock(&image,
                           kTestExtRootDirectoryBlock,
                           {{2, QByteArray("."), 2},
                            {2, QByteArray(".."), 2},
                            {12, QByteArray("hello.txt"), 1},
                            {13, QByteArray("docs"), 2},
                            {15, QByteArray("hello-link"), 7}});
    writeExtDirectoryBlock(
        &image,
        kTestExtDocsDirectoryBlock,
        {{13, QByteArray("."), 2}, {2, QByteArray(".."), 2}, {14, QByteArray("note.txt"), 1}});
    std::copy(hello.cbegin(),
              hello.cend(),
              image.begin() + static_cast<qsizetype>(kTestExtHelloFileBlock * kTestExtBlockSize));
    std::copy(note.cbegin(),
              note.cend(),
              image.begin() + static_cast<qsizetype>(kTestExtNoteFileBlock * kTestExtBlockSize));
    return image;
}

void writeHfsExtent(QByteArray* bytes, qsizetype offset, uint32_t startBlock, uint32_t blockCount) {
    writeBe32(bytes, offset + kTestHfsExtentStartBlockOffset, startBlock);
    writeBe32(bytes, offset + kTestHfsExtentBlockCountOffset, blockCount);
}

struct HfsForkFixture {
    uint64_t logical_size{0};
    uint32_t total_blocks{0};
    uint32_t first_block{0};
    uint32_t first_block_count{0};
};

struct HfsFileRecordFixture {
    uint32_t file_id{0};
    HfsForkFixture data_fork;
    HfsForkFixture resource_fork;
};

void writeHfsFork(QByteArray* bytes,
                  qsizetype offset,
                  uint64_t logicalSize,
                  uint32_t totalBlocks,
                  uint32_t firstBlock) {
    writeBe64(bytes, offset + kTestHfsForkLogicalSizeOffset, logicalSize);
    writeBe32(bytes, offset + kTestHfsForkTotalBlocksOffset, totalBlocks);
    writeHfsExtent(bytes, offset + kTestHfsForkExtentsOffset, firstBlock, totalBlocks);
}

void setHfsAllocationBit(QByteArray* bytes, uint32_t block) {
    const qsizetype offset =
        static_cast<qsizetype>(kTestHfsAllocationStartBlock * kTestHfsBlockSize + block / 8U);
    const char mask = static_cast<char>(0x80U >> (block % 8U));
    (*bytes)[offset] = static_cast<char>((*bytes)[offset] | mask);
}

void clearHfsAllocationBit(QByteArray* bytes, uint32_t block) {
    const qsizetype offset =
        static_cast<qsizetype>(kTestHfsAllocationStartBlock * kTestHfsBlockSize + block / 8U);
    const char mask = static_cast<char>(0x80U >> (block % 8U));
    (*bytes)[offset] = static_cast<char>((*bytes)[offset] & ~mask);
}

bool hfsAllocationBitSet(const QByteArray& bytes, uint32_t block) {
    const qsizetype offset =
        static_cast<qsizetype>(kTestHfsAllocationStartBlock * kTestHfsBlockSize + block / 8U);
    const auto value = static_cast<unsigned char>(bytes.at(offset));
    const auto mask = static_cast<unsigned char>(0x80U >> (block % 8U));
    return (value & mask) != 0;
}

void writeHfsAllocationFork(QByteArray* image, const QVector<uint32_t>& allocatedBlocks) {
    writeHfsFork(image,
                 kTestHfsHeaderOffset + kTestHfsAllocationForkOffset,
                 kTestHfsBlockSize,
                 1,
                 kTestHfsAllocationStartBlock);
    for (uint32_t block : allocatedBlocks) {
        setHfsAllocationBit(image, block);
    }
}

void writeHfsForkWithInitialExtent(QByteArray* bytes,
                                   qsizetype offset,
                                   const HfsForkFixture& fork) {
    writeBe64(bytes, offset + kTestHfsForkLogicalSizeOffset, fork.logical_size);
    writeBe32(bytes, offset + kTestHfsForkTotalBlocksOffset, fork.total_blocks);
    writeHfsExtent(
        bytes, offset + kTestHfsForkExtentsOffset, fork.first_block, fork.first_block_count);
}

QByteArray hfsCatalogKey(uint32_t parentId, const QString& name) {
    const uint16_t keyLength = static_cast<uint16_t>(6 + name.size() * 2);
    QByteArray key(2 + keyLength, '\0');
    writeBe16(&key, 0, keyLength);
    writeBe32(&key, 2, parentId);
    writeBe16(&key, 6, static_cast<uint16_t>(name.size()));
    for (qsizetype index = 0; index < name.size(); ++index) {
        writeBe16(&key, 8 + index * 2, name.at(index).unicode());
    }
    return key;
}

QByteArray hfsFolderRecord(uint32_t folderId) {
    QByteArray record(88, '\0');
    writeBe16(&record, 0, 1);
    writeBe32(&record, kTestHfsCatalogRecordIdOffset, folderId);
    return record;
}

QByteArray hfsFileRecord(uint32_t fileId, const QByteArray& data, uint32_t firstBlock) {
    QByteArray record(248, '\0');
    writeBe16(&record, 0, 2);
    writeBe32(&record, kTestHfsCatalogRecordIdOffset, fileId);
    writeHfsFork(&record,
                 kTestHfsCatalogFileDataForkOffset,
                 static_cast<uint64_t>(data.size()),
                 1,
                 firstBlock);
    return record;
}

QByteArray hfsFileRecordWithInitialExtent(uint32_t fileId,
                                          uint64_t logicalSize,
                                          uint32_t totalBlocks,
                                          uint32_t firstBlock,
                                          uint32_t firstBlockCount) {
    QByteArray record(248, '\0');
    writeBe16(&record, 0, 2);
    writeBe32(&record, kTestHfsCatalogRecordIdOffset, fileId);
    writeHfsForkWithInitialExtent(&record,
                                  kTestHfsCatalogFileDataForkOffset,
                                  HfsForkFixture{.logical_size = logicalSize,
                                                 .total_blocks = totalBlocks,
                                                 .first_block = firstBlock,
                                                 .first_block_count = firstBlockCount});
    return record;
}

QByteArray hfsFileRecordWithForks(const HfsFileRecordFixture& fixture) {
    QByteArray record(248, '\0');
    writeBe16(&record, 0, 2);
    writeBe32(&record, kTestHfsCatalogRecordIdOffset, fixture.file_id);
    writeHfsForkWithInitialExtent(&record, kTestHfsCatalogFileDataForkOffset, fixture.data_fork);
    writeHfsForkWithInitialExtent(&record,
                                  kTestHfsCatalogFileResourceForkOffset,
                                  fixture.resource_fork);
    return record;
}

QByteArray hfsCatalogRecord(uint32_t parentId, const QString& name, const QByteArray& data) {
    QByteArray record = hfsCatalogKey(parentId, name);
    record.append(data);
    if ((record.size() % 2) != 0) {
        record.append('\0');
    }
    return record;
}

void writeHfsNodeOffsets(QByteArray* node,
                         const QVector<qsizetype>& offsets,
                         qsizetype freeOffset) {
    for (int index = 0; index < offsets.size(); ++index) {
        writeBe16(node, node->size() - ((index + 1) * 2), static_cast<uint16_t>(offsets.at(index)));
    }
    writeBe16(node, node->size() - ((offsets.size() + 1) * 2), static_cast<uint16_t>(freeOffset));
}

void setFixtureMapBit(QByteArray* node, uint32_t nodeNumber, bool set) {
    const qsizetype offset = kTestHfsBTreeHeaderMapRecordOffset + nodeNumber / 8;
    const auto mask = static_cast<char>(0x80U >> (nodeNumber % 8U));
    (*node)[offset] = set ? static_cast<char>((*node)[offset] | mask)
                          : static_cast<char>((*node)[offset] & ~mask);
}

void writeHfsCatalogHeaderNode(QByteArray* image) {
    const qsizetype nodeOffset = kTestHfsCatalogStartBlock * kTestHfsBlockSize;
    QByteArray node(kTestHfsCatalogNodeSize, '\0');
    node[kTestHfsBTreeKindOffset] = static_cast<char>(1);
    writeBe16(&node, kTestHfsBTreeNumRecordsOffset, 3);

    const qsizetype header = kTestHfsBTreeHeaderRecordOffset;
    writeBe16(&node, header + kTestHfsBTreeHeaderTreeDepthOffset, 1);
    writeBe32(&node, header + kTestHfsBTreeHeaderRootNodeOffset, 1);
    writeBe32(&node, header + kTestHfsBTreeHeaderLeafRecordsOffset, 3);
    writeBe32(&node, header + kTestHfsBTreeHeaderFirstLeafNodeOffset, 1);
    writeBe32(&node, header + kTestHfsBTreeHeaderLastLeafNodeOffset, 1);
    writeBe16(&node, header + kTestHfsBTreeHeaderNodeSizeOffset, kTestHfsCatalogNodeSize);
    writeBe16(&node, header + kTestHfsBTreeHeaderMaxKeyLengthOffset, kTestHfsCatalogMaxKeyLength);
    writeBe32(&node, header + kTestHfsBTreeHeaderTotalNodesOffset, kTestHfsCatalogTotalNodes);
    writeBe32(&node, header + kTestHfsBTreeHeaderFreeNodesOffset, 0);
    node[header + kTestHfsBTreeHeaderKeyCompareTypeOffset] = static_cast<char>(0xCF);
    writeBe32(&node,
              header + kTestHfsBTreeHeaderAttributesOffset,
              kTestHfsBTreeBigKeysMask | kTestHfsBTreeVariableIndexKeysMask);
    writeHfsNodeOffsets(&node, {14, 120, 248}, 256);
    setFixtureMapBit(&node, 0, true);
    setFixtureMapBit(&node, 1, true);

    std::copy(node.cbegin(), node.cend(), image->begin() + nodeOffset);
}

void writeHfsCatalogLeafNode(QByteArray* image) {
    const QByteArray hello("hello from hfs\n");
    const QByteArray note("nested hfs note\n");
    const QVector<QByteArray> records{
        hfsCatalogRecord(2, QStringLiteral("Docs"), hfsFolderRecord(16)),
        hfsCatalogRecord(
            2, QStringLiteral("hello.txt"), hfsFileRecord(17, hello, kTestHfsHelloFileBlock)),
        hfsCatalogRecord(
            16, QStringLiteral("note.txt"), hfsFileRecord(18, note, kTestHfsNoteFileBlock))};

    const qsizetype nodeOffset = (kTestHfsCatalogStartBlock + 1) * kTestHfsBlockSize;
    QByteArray node(kTestHfsCatalogNodeSize, '\0');
    node[kTestHfsBTreeKindOffset] = static_cast<char>(0xFF);
    node[kTestHfsBTreeHeightOffset] = static_cast<char>(1);
    writeBe16(&node, kTestHfsBTreeNumRecordsOffset, static_cast<uint16_t>(records.size()));

    QVector<qsizetype> offsets;
    qsizetype cursor = 14;
    for (const auto& record : records) {
        offsets.append(cursor);
        std::copy(record.cbegin(), record.cend(), node.begin() + cursor);
        cursor += record.size();
    }
    writeHfsNodeOffsets(&node, offsets, cursor);
    std::copy(node.cbegin(), node.cend(), image->begin() + nodeOffset);
    std::copy(hello.cbegin(),
              hello.cend(),
              image->begin() + static_cast<qsizetype>(kTestHfsHelloFileBlock * kTestHfsBlockSize));
    std::copy(note.cbegin(),
              note.cend(),
              image->begin() + static_cast<qsizetype>(kTestHfsNoteFileBlock * kTestHfsBlockSize));
}

void writeHfsCatalogLeafRecords(QByteArray* image,
                                uint32_t physicalBlock,
                                const QVector<QByteArray>& records) {
    const qsizetype nodeOffset = static_cast<qsizetype>(physicalBlock * kTestHfsBlockSize);
    QByteArray node(kTestHfsCatalogNodeSize, '\0');
    node[kTestHfsBTreeKindOffset] = static_cast<char>(0xFF);
    node[kTestHfsBTreeHeightOffset] = static_cast<char>(1);
    writeBe16(&node, kTestHfsBTreeNumRecordsOffset, static_cast<uint16_t>(records.size()));

    QVector<qsizetype> offsets;
    qsizetype cursor = 14;
    for (const auto& record : records) {
        offsets.append(cursor);
        std::copy(record.cbegin(), record.cend(), node.begin() + cursor);
        cursor += record.size();
    }
    writeHfsNodeOffsets(&node, offsets, cursor);
    std::copy(node.cbegin(), node.cend(), image->begin() + nodeOffset);
}

QByteArray hfsInlineAttributeValue() {
    QByteArray value("finder-info-attribute");
    value.resize(32, '\0');
    return value;
}

QByteArray hfsForkAttributeValue() {
    return QByteArrayLiteral("large hfs attribute payload from fork");
}

QByteArray hfsAttributeRecord(uint32_t fileId,
                              const QString& name,
                              uint32_t recordType,
                              const std::optional<HfsForkFixture>& fork = std::nullopt,
                              const QByteArray& inlineData = {}) {
    const uint16_t keyLength = static_cast<uint16_t>(12 + name.size() * 2);
    qsizetype payloadBytes = 0;
    if (fork.has_value()) {
        payloadBytes = 4 + kTestHfsForkDataBytes;
    } else if (recordType == kTestHfsAttributeInlineRecord) {
        payloadBytes = 12 + inlineData.size();
    }
    QByteArray record(2 + keyLength + 4 + payloadBytes, '\0');
    writeBe16(&record, 0, keyLength);
    writeBe32(&record, 4, fileId);
    writeBe32(&record, 8, 0);
    writeBe16(&record, 12, static_cast<uint16_t>(name.size()));
    for (qsizetype index = 0; index < name.size(); ++index) {
        writeBe16(&record, 14 + index * 2, name.at(index).unicode());
    }
    writeBe32(&record, 2 + keyLength, recordType);
    if (recordType == kTestHfsAttributeInlineRecord) {
        writeBe32(&record, 2 + keyLength + 12, static_cast<uint32_t>(inlineData.size()));
        std::copy(inlineData.cbegin(), inlineData.cend(), record.begin() + 2 + keyLength + 16);
    } else if (fork.has_value()) {
        writeHfsForkWithInitialExtent(&record, 2 + keyLength + 8, *fork);
    }
    if ((record.size() % 2) != 0) {
        record.append('\0');
    }
    return record;
}

void writeHfsAttributesHeaderNode(QByteArray* image) {
    const qsizetype nodeOffset = kTestHfsAttributesStartBlock * kTestHfsBlockSize;
    QByteArray node(kTestHfsAttributesNodeSize, '\0');
    node[kTestHfsBTreeKindOffset] = static_cast<char>(1);
    writeBe16(&node, kTestHfsBTreeNumRecordsOffset, 3);

    const qsizetype header = kTestHfsBTreeHeaderRecordOffset;
    writeBe16(&node, header + kTestHfsBTreeHeaderTreeDepthOffset, 1);
    writeBe32(&node, header + kTestHfsBTreeHeaderRootNodeOffset, 1);
    writeBe32(&node, header + kTestHfsBTreeHeaderLeafRecordsOffset, 2);
    writeBe32(&node, header + kTestHfsBTreeHeaderFirstLeafNodeOffset, 1);
    writeBe32(&node, header + kTestHfsBTreeHeaderLastLeafNodeOffset, 1);
    writeBe16(&node, header + kTestHfsBTreeHeaderNodeSizeOffset, kTestHfsAttributesNodeSize);
    writeBe32(&node, header + kTestHfsBTreeHeaderTotalNodesOffset, kTestHfsAttributesTotalNodes);
    node[header + kTestHfsBTreeHeaderKeyCompareTypeOffset] = static_cast<char>(0xBC);
    writeBe32(&node,
              header + kTestHfsBTreeHeaderAttributesOffset,
              kTestHfsBTreeBigKeysMask | kTestHfsBTreeVariableIndexKeysMask);
    writeHfsNodeOffsets(&node, {14, 120, 248}, 256);
    setFixtureMapBit(&node, 0, true);
    setFixtureMapBit(&node, 1, true);

    std::copy(node.cbegin(), node.cend(), image->begin() + nodeOffset);
}

void writeHfsAttributesLeafNode(QByteArray* image) {
    const QVector<QByteArray> records{
        hfsAttributeRecord(17,
                           QStringLiteral("com.apple.FinderInfo"),
                           kTestHfsAttributeInlineRecord,
                           std::nullopt,
                           hfsInlineAttributeValue()),
        hfsAttributeRecord(17,
                           QStringLiteral("com.apple.ResourceFork"),
                           kTestHfsAttributeForkRecord,
                           HfsForkFixture{.logical_size =
                                              static_cast<uint64_t>(hfsForkAttributeValue().size()),
                                          .total_blocks = 1,
                                          .first_block = kTestHfsAttributeForkValueBlock,
                                          .first_block_count = 1})};
    const qsizetype nodeOffset =
        static_cast<qsizetype>((kTestHfsAttributesStartBlock + 1) * kTestHfsBlockSize);
    QByteArray node(kTestHfsAttributesNodeSize, '\0');
    node[kTestHfsBTreeKindOffset] = static_cast<char>(0xFF);
    node[kTestHfsBTreeHeightOffset] = static_cast<char>(1);
    writeBe16(&node, kTestHfsBTreeNumRecordsOffset, static_cast<uint16_t>(records.size()));

    QVector<qsizetype> offsets;
    qsizetype cursor = 14;
    for (const auto& record : records) {
        offsets.append(cursor);
        std::copy(record.cbegin(), record.cend(), node.begin() + cursor);
        cursor += record.size();
    }
    writeHfsNodeOffsets(&node, offsets, cursor);
    std::copy(node.cbegin(), node.cend(), image->begin() + nodeOffset);
}

QByteArray hfsReaderFixture();

QByteArray hfsReaderAttributeFixture() {
    QByteArray image = hfsReaderFixture();
    writeBe32(&image, kTestHfsHeaderOffset + kTestHfsFreeBlocksOffset, 37);
    writeHfsAllocationFork(&image,
                           {kTestHfsCatalogStartBlock,
                            kTestHfsCatalogStartBlock + 1,
                            kTestHfsHelloFileBlock,
                            kTestHfsNoteFileBlock,
                            kTestHfsAllocationStartBlock,
                            kTestHfsAttributesStartBlock,
                            kTestHfsAttributesStartBlock + 1,
                            kTestHfsAttributeForkValueBlock});
    writeHfsFork(&image,
                 kTestHfsHeaderOffset + kTestHfsAttributesForkOffset,
                 kTestHfsAttributesNodeSize * kTestHfsAttributesTotalNodes,
                 kTestHfsAttributesTotalNodes,
                 kTestHfsAttributesStartBlock);
    writeHfsAttributesHeaderNode(&image);
    writeHfsAttributesLeafNode(&image);
    const QByteArray forkValue = hfsForkAttributeValue();
    std::copy(forkValue.cbegin(),
              forkValue.cend(),
              image.begin() +
                  static_cast<qsizetype>(kTestHfsAttributeForkValueBlock * kTestHfsBlockSize));
    return image;
}

QByteArray hfsExtentsOverflowRecord(uint32_t fileId,
                                    uint32_t forkType,
                                    uint32_t logicalStartBlock,
                                    uint32_t physicalStartBlock,
                                    uint32_t blockCount) {
    QByteArray record(2 + kTestHfsExtentsKeyLength + kTestHfsExtentsRecordBytes, '\0');
    writeBe16(&record, 0, kTestHfsExtentsKeyLength);
    record[2] = static_cast<char>(forkType);
    writeBe32(&record, 4, fileId);
    writeBe32(&record, 8, logicalStartBlock);
    writeHfsExtent(&record, 2 + kTestHfsExtentsKeyLength, physicalStartBlock, blockCount);
    return record;
}

void writeHfsExtentsHeaderNode(QByteArray* image, uint32_t leafRecords) {
    const qsizetype nodeOffset =
        static_cast<qsizetype>(kTestHfsExtentsStartBlock * kTestHfsBlockSize);
    QByteArray node(kTestHfsExtentsNodeSize, '\0');
    node[kTestHfsBTreeKindOffset] = static_cast<char>(1);
    writeBe16(&node, kTestHfsBTreeNumRecordsOffset, 3);

    const qsizetype header = kTestHfsBTreeHeaderRecordOffset;
    writeBe16(&node, header + kTestHfsBTreeHeaderTreeDepthOffset, 1);
    writeBe32(&node, header + kTestHfsBTreeHeaderRootNodeOffset, 1);
    writeBe32(&node, header + kTestHfsBTreeHeaderLeafRecordsOffset, leafRecords);
    writeBe32(&node, header + kTestHfsBTreeHeaderFirstLeafNodeOffset, 1);
    writeBe32(&node, header + kTestHfsBTreeHeaderLastLeafNodeOffset, 1);
    writeBe16(&node, header + kTestHfsBTreeHeaderNodeSizeOffset, kTestHfsExtentsNodeSize);
    writeBe32(&node, header + kTestHfsBTreeHeaderTotalNodesOffset, 2);
    writeHfsNodeOffsets(&node, {14, 120, 248}, 256);
    // A real extents B-tree header marks its allocated nodes (header node 0 and
    // the single leaf node 1) in the node-allocation map record; the streaming
    // mutation engine validates these bits when it loads the tree.
    setFixtureMapBit(&node, 0, true);
    setFixtureMapBit(&node, 1, true);

    std::copy(node.cbegin(), node.cend(), image->begin() + nodeOffset);
}

void writeHfsExtentsLeafNode(QByteArray* image, const QVector<QByteArray>& records) {
    const qsizetype nodeOffset = static_cast<qsizetype>(
        kTestHfsExtentsStartBlock * kTestHfsBlockSize + kTestHfsExtentsNodeSize);
    QByteArray node(kTestHfsExtentsNodeSize, '\0');
    node[kTestHfsBTreeKindOffset] = static_cast<char>(0xFF);
    node[kTestHfsBTreeHeightOffset] = static_cast<char>(1);
    writeBe16(&node, kTestHfsBTreeNumRecordsOffset, static_cast<uint16_t>(records.size()));

    QVector<qsizetype> offsets;
    qsizetype cursor = 14;
    for (const auto& record : records) {
        offsets.append(cursor);
        std::copy(record.cbegin(), record.cend(), node.begin() + cursor);
        cursor += record.size();
    }
    writeHfsNodeOffsets(&node, offsets, cursor);
    std::copy(node.cbegin(), node.cend(), image->begin() + nodeOffset);
}

QByteArray hfsReaderFixture() {
    QByteArray image(static_cast<qsizetype>(64 * kTestHfsBlockSize), '\0');
    writeAscii(&image, kTestHfsHeaderOffset, "H+");
    writeBe16(&image, kTestHfsHeaderOffset + kTestHfsVersionOffset, 4);
    writeBe32(&image, kTestHfsHeaderOffset + kTestHfsAttributesOffset, kTestHfsJournaledMask);
    writeBe32(&image, kTestHfsHeaderOffset + kTestHfsFileCountOffset, 2);
    writeBe32(&image, kTestHfsHeaderOffset + kTestHfsFolderCountOffset, 1);
    writeBe32(&image, kTestHfsHeaderOffset + kTestHfsBlockSizeOffset, kTestHfsBlockSize);
    writeBe32(&image, kTestHfsHeaderOffset + kTestHfsTotalBlocksOffset, 64);
    writeBe32(&image, kTestHfsHeaderOffset + kTestHfsFreeBlocksOffset, 40);
    writeHfsAllocationFork(&image,
                           {kTestHfsCatalogStartBlock,
                            kTestHfsCatalogStartBlock + 1,
                            kTestHfsHelloFileBlock,
                            kTestHfsNoteFileBlock,
                            kTestHfsAllocationStartBlock});
    writeHfsFork(&image,
                 kTestHfsHeaderOffset + kTestHfsCatalogForkOffset,
                 kTestHfsCatalogNodeSize * kTestHfsCatalogTotalNodes,
                 kTestHfsCatalogTotalNodes,
                 kTestHfsCatalogStartBlock);
    writeHfsCatalogHeaderNode(&image);
    writeHfsCatalogLeafNode(&image);
    return image;
}

void writeHfsOverflowFixtureForks(QByteArray* image) {
    writeAscii(image, kTestHfsHeaderOffset, "H+");
    writeBe16(image, kTestHfsHeaderOffset + kTestHfsVersionOffset, 4);
    writeBe32(image, kTestHfsHeaderOffset + kTestHfsAttributesOffset, kTestHfsJournaledMask);
    writeBe32(image, kTestHfsHeaderOffset + kTestHfsFileCountOffset, 1);
    writeBe32(image, kTestHfsHeaderOffset + kTestHfsFolderCountOffset, 0);
    writeBe32(image, kTestHfsHeaderOffset + kTestHfsBlockSizeOffset, kTestHfsBlockSize);
    writeBe32(image, kTestHfsHeaderOffset + kTestHfsTotalBlocksOffset, 64);
    writeBe32(image, kTestHfsHeaderOffset + kTestHfsFreeBlocksOffset, 40);
    writeHfsForkWithInitialExtent(image,
                                  kTestHfsHeaderOffset + kTestHfsExtentsForkOffset,
                                  HfsForkFixture{.logical_size = kTestHfsExtentsNodeSize * 2,
                                                 .total_blocks = 1,
                                                 .first_block = kTestHfsExtentsStartBlock,
                                                 .first_block_count = 1});
    writeHfsForkWithInitialExtent(image,
                                  kTestHfsHeaderOffset + kTestHfsCatalogForkOffset,
                                  HfsForkFixture{.logical_size = kTestHfsCatalogNodeSize *
                                                                 kTestHfsCatalogTotalNodes,
                                                 .total_blocks = kTestHfsCatalogTotalNodes,
                                                 .first_block = kTestHfsCatalogStartBlock,
                                                 .first_block_count = 1});
}

QByteArray overflowFixtureContent(char fill, const QByteArray& tail) {
    QByteArray content(kTestHfsBlockSize, fill);
    content.append(tail);
    return content;
}

void assignOptionalExpected(QByteArray* target, const QByteArray& content) {
    if (target) {
        *target = content;
    }
}

void writeHfsOverflowCatalog(QByteArray* image,
                             const QByteArray& hello,
                             const QByteArray& resource) {
    writeHfsCatalogHeaderNode(image);
    const QVector<QByteArray> catalogRecords{hfsCatalogRecord(
        2,
        QStringLiteral("hello.txt"),
        hfsFileRecordWithForks(HfsFileRecordFixture{
            .file_id = 17,
            .data_fork = HfsForkFixture{.logical_size = static_cast<uint64_t>(hello.size()),
                                        .total_blocks = 2,
                                        .first_block = kTestHfsHelloFileBlock,
                                        .first_block_count = 1},
            .resource_fork = HfsForkFixture{.logical_size = static_cast<uint64_t>(resource.size()),
                                            .total_blocks = 2,
                                            .first_block = kTestHfsResourceFileBlock,
                                            .first_block_count = 1}}))};
    writeHfsCatalogLeafRecords(image, kTestHfsCatalogStartBlock + 1, catalogRecords);
}

void writeContentPair(QByteArray* image, qsizetype firstBlock, const QByteArray& content) {
    const QByteArray firstChunk = content.left(kTestHfsBlockSize);
    const QByteArray tailChunk = content.mid(kTestHfsBlockSize);
    std::copy(firstChunk.cbegin(),
              firstChunk.cend(),
              image->begin() + static_cast<qsizetype>(firstBlock * kTestHfsBlockSize));
    std::copy(tailChunk.cbegin(),
              tailChunk.cend(),
              image->begin() + static_cast<qsizetype>((firstBlock + 1) * kTestHfsBlockSize));
}

void writeHfsOverflowExtents(QByteArray* image) {
    const QVector<QByteArray> overflowRecords{
        hfsExtentsOverflowRecord(
            kTestHfsCatalogFileId, kTestHfsDataForkType, 1, kTestHfsCatalogStartBlock + 1, 1),
        hfsExtentsOverflowRecord(17, kTestHfsDataForkType, 1, kTestHfsHelloFileBlock + 1, 1),
        hfsExtentsOverflowRecord(
            17, kTestHfsResourceForkType, 1, kTestHfsResourceFileBlock + 1, 1)};
    writeHfsExtentsHeaderNode(image, static_cast<uint32_t>(overflowRecords.size()));
    writeHfsExtentsLeafNode(image, overflowRecords);
}

QByteArray hfsReaderOverflowFixture(QByteArray* expectedHello,
                                    QByteArray* expectedResource = nullptr) {
    QByteArray image(static_cast<qsizetype>(64 * kTestHfsBlockSize), '\0');
    writeHfsOverflowFixtureForks(&image);

    const QByteArray hello = overflowFixtureContent('A',
                                                    QByteArrayLiteral("tail from hfs overflow\n"));
    const QByteArray resource =
        overflowFixtureContent('R', QByteArrayLiteral("tail from hfs resource overflow\n"));
    if (expectedHello) {
        *expectedHello = hello;
    }
    assignOptionalExpected(expectedResource, resource);
    writeHfsOverflowCatalog(&image, hello, resource);
    writeContentPair(&image, kTestHfsHelloFileBlock, hello);
    writeContentPair(&image, kTestHfsResourceFileBlock, resource);
    writeHfsOverflowExtents(&image);
    return image;
}

QByteArray hfsReaderOverflowFixtureWithAllocation() {
    QByteArray image = hfsReaderOverflowFixture(nullptr);
    writeHfsAllocationFork(&image,
                           {kTestHfsCatalogStartBlock,
                            kTestHfsCatalogStartBlock + 1,
                            kTestHfsHelloFileBlock,
                            kTestHfsHelloFileBlock + 1,
                            kTestHfsExtentsStartBlock,
                            kTestHfsResourceFileBlock,
                            kTestHfsResourceFileBlock + 1,
                            kTestHfsAllocationStartBlock});
    return image;
}

constexpr uint32_t kTestHfsCompressedFileId = 19;
constexpr uint32_t kTestHfsCompressedResourceStartBlock = 30;
constexpr uint32_t kTestHfsCompressedResourceBlocks = 16;

QByteArray testDecmpfsHeader(uint32_t type, uint64_t uncompressedSize) {
    QByteArray header(16, '\0');
    header[0] = 'f';
    header[1] = 'p';
    header[2] = 'm';
    header[3] = 'c';
    header[4] = static_cast<char>(type & 0xFF);
    header[5] = static_cast<char>((type >> 8) & 0xFF);
    header[6] = static_cast<char>((type >> 16) & 0xFF);
    header[7] = static_cast<char>((type >> 24) & 0xFF);
    for (int index = 0; index < 8; ++index) {
        header[8 + index] = static_cast<char>((uncompressedSize >> (8 * index)) & 0xFF);
    }
    return header;
}

void writeTestLe32(QByteArray* bytes, qsizetype offset, uint32_t value) {
    (*bytes)[offset] = static_cast<char>(value & 0xFF);
    (*bytes)[offset + 1] = static_cast<char>((value >> 8) & 0xFF);
    (*bytes)[offset + 2] = static_cast<char>((value >> 16) & 0xFF);
    (*bytes)[offset + 3] = static_cast<char>((value >> 24) & 0xFF);
}

QByteArray testDecmpfsChunk(const QByteArray& chunk) {
    const QByteArray compressed = qCompress(chunk, 9).mid(4);
    if (compressed.size() >= chunk.size() + 1) {
        QByteArray raw;
        raw.append(static_cast<char>(0xFF));
        raw.append(chunk);
        return raw;
    }
    return compressed;
}

QByteArray testDecmpfsResourceFork(const QByteArray& content) {
    const int chunkCount = std::max<int>(1, static_cast<int>((content.size() + 65'535) / 65'536));
    QVector<QByteArray> chunks;
    for (int index = 0; index < chunkCount; ++index) {
        chunks.append(
            testDecmpfsChunk(content.mid(static_cast<qsizetype>(index) * 65'536, 65'536)));
    }
    const qsizetype tableBytes = 4 + static_cast<qsizetype>(chunkCount) * 8;
    qsizetype chunkBytes = 0;
    for (const auto& chunk : chunks) {
        chunkBytes += chunk.size();
    }
    const qsizetype mapOffset = 0x104 + tableBytes + chunkBytes;
    QByteArray fork(mapOffset + 50, '\0');
    writeBe32(&fork, 0, 0x100);
    writeBe32(&fork, 4, static_cast<uint32_t>(mapOffset));
    writeBe32(&fork, 8, static_cast<uint32_t>(mapOffset - 0x100));
    writeBe32(&fork, 12, 0x32);
    writeBe32(&fork, 0x100, static_cast<uint32_t>(tableBytes + chunkBytes));
    writeTestLe32(&fork, 0x104, static_cast<uint32_t>(chunkCount));
    qsizetype cursor = tableBytes;
    for (int index = 0; index < chunkCount; ++index) {
        writeTestLe32(&fork,
                      0x108 + static_cast<qsizetype>(index) * 8,
                      static_cast<uint32_t>(cursor));
        writeTestLe32(&fork,
                      0x108 + static_cast<qsizetype>(index) * 8 + 4,
                      static_cast<uint32_t>(chunks.at(index).size()));
        std::copy(chunks.at(index).cbegin(),
                  chunks.at(index).cend(),
                  fork.begin() + 0x104 + cursor);
        cursor += chunks.at(index).size();
    }
    writeBe16(&fork, mapOffset + 24, 0x1C);
    writeBe16(&fork, mapOffset + 26, 0x32);
    writeAscii(&fork, mapOffset + 30, "cmpf");
    writeBe32(&fork, mapOffset + 34, 0x0A);
    fork[mapOffset + 38] = '\0';
    fork[mapOffset + 39] = 0x01;
    fork[mapOffset + 40] = static_cast<char>(0xFF);
    fork[mapOffset + 41] = static_cast<char>(0xFF);
    return fork;
}

QByteArray testLzvnRawChunk(const QByteArray& content) {
    QByteArray chunk;
    chunk.reserve(content.size() + 1);
    chunk.append('\x06');
    chunk.append(content);
    return chunk;
}

QByteArray testLzfseRawBlock(const QByteArray& content) {
    QByteArray block(8, '\0');
    writeTestLe32(&block, 0, 0x2d'78'76'62U);  // 'bvx-' uncompressed-block magic
    writeTestLe32(&block, 4, static_cast<uint32_t>(content.size()));
    block.append(content);
    QByteArray end(4, '\0');
    writeTestLe32(&end, 0, 0x24'78'76'62U);  // 'bvx$' end-of-stream magic
    block.append(end);
    return block;
}

QByteArray testChunkedResourceFork(const QByteArray& chunkBytes) {
    QByteArray fork(8, '\0');
    writeTestLe32(&fork, 0, 8);
    writeTestLe32(&fork, 4, static_cast<uint32_t>(8 + chunkBytes.size()));
    fork.append(chunkBytes);
    return fork;
}

QByteArray testCompressedResourceForkBytes(uint32_t type, const QByteArray& content) {
    if (type == 4) {
        return testDecmpfsResourceFork(content);
    }
    if (type == 8) {
        return testChunkedResourceFork(testLzvnRawChunk(content));
    }
    if (type == 12) {
        return testChunkedResourceFork(testLzfseRawBlock(content));
    }
    return {};
}

QByteArray testCompressedAttrPayload(uint32_t type, const QByteArray& content) {
    QByteArray attrPayload = testDecmpfsHeader(type, static_cast<uint64_t>(content.size()));
    if (type == 3) {
        attrPayload.append(testDecmpfsChunk(content));
    } else if (type == 7) {
        attrPayload.append(testLzvnRawChunk(content));
    } else if (type == 11) {
        attrPayload.append(testLzfseRawBlock(content));
    } else if (type != 4 && type != 8 && type != 12) {
        attrPayload.append(QByteArray(8, 'x'));
    }
    return attrPayload;
}

QByteArray hfsCompressedFixture(uint32_t type,
                                const QByteArray& content,
                                int inlineSpareBytes = 0) {
    QByteArray image = hfsReaderAttributeFixture();

    HfsFileRecordFixture packed;
    packed.file_id = kTestHfsCompressedFileId;
    const QByteArray resourceForkBytes = testCompressedResourceForkBytes(type, content);
    if (!resourceForkBytes.isEmpty()) {
        packed.resource_fork =
            HfsForkFixture{.logical_size = static_cast<uint64_t>(resourceForkBytes.size()),
                           .total_blocks = kTestHfsCompressedResourceBlocks,
                           .first_block = kTestHfsCompressedResourceStartBlock,
                           .first_block_count = kTestHfsCompressedResourceBlocks};
    }
    const QVector<QByteArray> catalogRecords{
        hfsCatalogRecord(2, QStringLiteral("Docs"), hfsFolderRecord(16)),
        hfsCatalogRecord(
            2,
            QStringLiteral("hello.txt"),
            hfsFileRecord(17, QByteArrayLiteral("hello from hfs\n"), kTestHfsHelloFileBlock)),
        hfsCatalogRecord(2, QStringLiteral("packed.bin"), hfsFileRecordWithForks(packed)),
        hfsCatalogRecord(
            16,
            QStringLiteral("note.txt"),
            hfsFileRecord(18, QByteArrayLiteral("nested hfs note\n"), kTestHfsNoteFileBlock))};
    writeHfsCatalogLeafRecords(&image, kTestHfsCatalogStartBlock + 1, catalogRecords);
    const qsizetype catalogHeaderOffset =
        static_cast<qsizetype>(kTestHfsCatalogStartBlock * kTestHfsBlockSize);
    writeBe32(&image,
              catalogHeaderOffset + kTestHfsBTreeHeaderRecordOffset +
                  kTestHfsBTreeHeaderLeafRecordsOffset,
              4);
    writeBe32(&image, kTestHfsHeaderOffset + kTestHfsFileCountOffset, 3);

    const QByteArray attrPayload = testCompressedAttrPayload(type, content);
    QByteArray paddedPayload = attrPayload;
    paddedPayload.append(QByteArray(inlineSpareBytes, '\0'));
    QByteArray decmpfsRecord = hfsAttributeRecord(kTestHfsCompressedFileId,
                                                  QStringLiteral("com.apple.decmpfs"),
                                                  kTestHfsAttributeInlineRecord,
                                                  std::nullopt,
                                                  paddedPayload);
    const qsizetype sizeFieldOffset = 2 + (12 + 17 * 2) + 12;
    writeBe32(&decmpfsRecord, sizeFieldOffset, static_cast<uint32_t>(attrPayload.size()));

    const QVector<QByteArray> attrRecords{
        hfsAttributeRecord(17,
                           QStringLiteral("com.apple.FinderInfo"),
                           kTestHfsAttributeInlineRecord,
                           std::nullopt,
                           hfsInlineAttributeValue()),
        hfsAttributeRecord(17,
                           QStringLiteral("com.apple.ResourceFork"),
                           kTestHfsAttributeForkRecord,
                           HfsForkFixture{.logical_size =
                                              static_cast<uint64_t>(hfsForkAttributeValue().size()),
                                          .total_blocks = 1,
                                          .first_block = kTestHfsAttributeForkValueBlock,
                                          .first_block_count = 1}),
        decmpfsRecord};
    const qsizetype attrNodeOffset =
        static_cast<qsizetype>((kTestHfsAttributesStartBlock + 1) * kTestHfsBlockSize);
    QByteArray attrNode(kTestHfsAttributesNodeSize, '\0');
    attrNode[kTestHfsBTreeKindOffset] = static_cast<char>(0xFF);
    attrNode[kTestHfsBTreeHeightOffset] = static_cast<char>(1);
    writeBe16(&attrNode, kTestHfsBTreeNumRecordsOffset, static_cast<uint16_t>(attrRecords.size()));
    QVector<qsizetype> attrOffsets;
    qsizetype attrCursor = 14;
    for (const auto& record : attrRecords) {
        attrOffsets.append(attrCursor);
        std::copy(record.cbegin(), record.cend(), attrNode.begin() + attrCursor);
        attrCursor += record.size();
    }
    writeHfsNodeOffsets(&attrNode, attrOffsets, attrCursor);
    std::copy(attrNode.cbegin(), attrNode.cend(), image.begin() + attrNodeOffset);
    const qsizetype attrHeaderOffset =
        static_cast<qsizetype>(kTestHfsAttributesStartBlock * kTestHfsBlockSize);
    writeBe32(&image,
              attrHeaderOffset + kTestHfsBTreeHeaderRecordOffset +
                  kTestHfsBTreeHeaderLeafRecordsOffset,
              3);

    if (!resourceForkBytes.isEmpty()) {
        for (uint32_t block = 0; block < kTestHfsCompressedResourceBlocks; ++block) {
            setHfsAllocationBit(&image, kTestHfsCompressedResourceStartBlock + block);
        }
        std::copy(resourceForkBytes.cbegin(),
                  resourceForkBytes.cend(),
                  image.begin() + static_cast<qsizetype>(kTestHfsCompressedResourceStartBlock *
                                                         kTestHfsBlockSize));
    }
    return image;
}

QByteArray hfsEmptyFileRecordFixture(uint32_t fileId) {
    QByteArray record(248, '\0');
    writeBe16(&record, 0, 2);
    writeBe32(&record, kTestHfsCatalogRecordIdOffset, fileId);
    return record;
}

struct HfsSplitFixtureOptions {
    uint32_t free_nodes{kTestHfsSplitCatalogTotalNodes - 2};
    uint32_t attributes_mask{kTestHfsBTreeBigKeysMask | kTestHfsBTreeVariableIndexKeysMask};
    bool leaf_map_bit{true};
    bool full_volume{false};
};

void writeHfsSplitCatalogHeaderNode(QByteArray* image,
                                    uint32_t leafRecords,
                                    const HfsSplitFixtureOptions& options) {
    const qsizetype nodeOffset = kTestHfsCatalogStartBlock * kTestHfsBlockSize;
    QByteArray node(kTestHfsCatalogNodeSize, '\0');
    node[kTestHfsBTreeKindOffset] = static_cast<char>(1);
    writeBe16(&node, kTestHfsBTreeNumRecordsOffset, 3);

    const qsizetype header = kTestHfsBTreeHeaderRecordOffset;
    writeBe16(&node, header + kTestHfsBTreeHeaderTreeDepthOffset, 1);
    writeBe32(&node, header + kTestHfsBTreeHeaderRootNodeOffset, 1);
    writeBe32(&node, header + kTestHfsBTreeHeaderLeafRecordsOffset, leafRecords);
    writeBe32(&node, header + kTestHfsBTreeHeaderFirstLeafNodeOffset, 1);
    writeBe32(&node, header + kTestHfsBTreeHeaderLastLeafNodeOffset, 1);
    writeBe16(&node, header + kTestHfsBTreeHeaderNodeSizeOffset, kTestHfsCatalogNodeSize);
    writeBe16(&node, header + kTestHfsBTreeHeaderMaxKeyLengthOffset, kTestHfsCatalogMaxKeyLength);
    writeBe32(&node, header + kTestHfsBTreeHeaderTotalNodesOffset, kTestHfsSplitCatalogTotalNodes);
    writeBe32(&node, header + kTestHfsBTreeHeaderFreeNodesOffset, options.free_nodes);
    node[header + kTestHfsBTreeHeaderKeyCompareTypeOffset] = static_cast<char>(0xCF);
    writeBe32(&node, header + kTestHfsBTreeHeaderAttributesOffset, options.attributes_mask);
    writeHfsNodeOffsets(&node, {14, 120, 248}, 256);
    setFixtureMapBit(&node, 0, true);
    setFixtureMapBit(&node, 1, options.leaf_map_bit);

    std::copy(node.cbegin(), node.cend(), image->begin() + nodeOffset);
}

QByteArray hfsSplitReadyFixture(const HfsSplitFixtureOptions& options = {}) {
    QByteArray image(static_cast<qsizetype>(64 * kTestHfsBlockSize), '\0');
    writeAscii(&image, kTestHfsHeaderOffset, "H+");
    writeBe16(&image, kTestHfsHeaderOffset + kTestHfsVersionOffset, 4);
    writeBe32(&image, kTestHfsHeaderOffset + kTestHfsAttributesOffset, kTestHfsJournaledMask);
    writeBe32(&image,
              kTestHfsHeaderOffset + kTestHfsFileCountOffset,
              kTestHfsSplitFixtureFileCount);
    writeBe32(&image, kTestHfsHeaderOffset + kTestHfsFolderCountOffset, 0);
    writeBe32(&image, kTestHfsHeaderOffset + kTestHfsBlockSizeOffset, kTestHfsBlockSize);
    writeBe32(&image, kTestHfsHeaderOffset + kTestHfsTotalBlocksOffset, 64);
    writeBe32(&image,
              kTestHfsHeaderOffset + kTestHfsFreeBlocksOffset,
              options.full_volume ? 0U : 53U);

    QVector<uint32_t> usedBlocks{kTestHfsAllocationStartBlock};
    const uint32_t lastUsedBlock = options.full_volume ? 63U : 9U;
    for (uint32_t block = 0; block <= lastUsedBlock; ++block) {
        usedBlocks.append(block);
    }
    writeHfsAllocationFork(&image, usedBlocks);
    writeHfsFork(&image,
                 kTestHfsHeaderOffset + kTestHfsCatalogForkOffset,
                 kTestHfsCatalogNodeSize * kTestHfsSplitCatalogTotalNodes,
                 kTestHfsSplitCatalogTotalNodes,
                 kTestHfsCatalogStartBlock);
    writeHfsSplitCatalogHeaderNode(&image, kTestHfsSplitFixtureFileCount, options);

    QVector<QByteArray> records;
    for (int index = 0; index < kTestHfsSplitFixtureFileCount; ++index) {
        const QString name = QStringLiteral("file-%1%2")
                                 .arg(QChar(QLatin1Char(static_cast<char>('a' + index / 8))))
                                 .arg(QChar(QLatin1Char(static_cast<char>('a' + index % 8))));
        records.append(hfsCatalogRecord(
            2, name, hfsEmptyFileRecordFixture(16U + static_cast<uint32_t>(index))));
    }
    writeHfsCatalogLeafRecords(&image, kTestHfsCatalogStartBlock + 1, records);
    return image;
}

constexpr uint32_t kTestHfsDeepCatalogVolumeBlocks = 256;
constexpr uint32_t kTestHfsDeepCatalogStartBlock = 64;
constexpr uint32_t kTestHfsDeepCatalogTotalNodes = 160;
constexpr int kTestHfsDeepFixtureFileCount = 4;

// One-level catalog with a large node pool and a 32-byte map record, sized so
// repeated creates can drive the tree through depth 2 into depth 3.
QByteArray hfsDeepCatalogFixture() {
    QByteArray image(static_cast<qsizetype>(kTestHfsDeepCatalogVolumeBlocks * kTestHfsBlockSize),
                     '\0');
    writeAscii(&image, kTestHfsHeaderOffset, "H+");
    writeBe16(&image, kTestHfsHeaderOffset + kTestHfsVersionOffset, 4);
    writeBe32(&image, kTestHfsHeaderOffset + kTestHfsAttributesOffset, kTestHfsJournaledMask);
    writeBe32(&image, kTestHfsHeaderOffset + kTestHfsFileCountOffset, kTestHfsDeepFixtureFileCount);
    writeBe32(&image, kTestHfsHeaderOffset + kTestHfsFolderCountOffset, 0);
    writeBe32(&image, kTestHfsHeaderOffset + kTestHfsBlockSizeOffset, kTestHfsBlockSize);
    writeBe32(&image,
              kTestHfsHeaderOffset + kTestHfsTotalBlocksOffset,
              kTestHfsDeepCatalogVolumeBlocks);

    QVector<uint32_t> usedBlocks{
        0, 1, kTestHfsAllocationStartBlock, kTestHfsDeepCatalogVolumeBlocks - 1};
    for (uint32_t block = 0; block < kTestHfsDeepCatalogTotalNodes; ++block) {
        usedBlocks.append(kTestHfsDeepCatalogStartBlock + block);
    }
    writeHfsAllocationFork(&image, usedBlocks);
    writeBe32(&image,
              kTestHfsHeaderOffset + kTestHfsFreeBlocksOffset,
              kTestHfsDeepCatalogVolumeBlocks - static_cast<uint32_t>(usedBlocks.size()));
    writeHfsFork(&image,
                 kTestHfsHeaderOffset + kTestHfsCatalogForkOffset,
                 static_cast<uint64_t>(kTestHfsCatalogNodeSize) * kTestHfsDeepCatalogTotalNodes,
                 kTestHfsDeepCatalogTotalNodes,
                 kTestHfsDeepCatalogStartBlock);

    const qsizetype nodeOffset = static_cast<qsizetype>(kTestHfsDeepCatalogStartBlock) *
                                 kTestHfsBlockSize;
    QByteArray node(kTestHfsCatalogNodeSize, '\0');
    node[kTestHfsBTreeKindOffset] = static_cast<char>(1);
    writeBe16(&node, kTestHfsBTreeNumRecordsOffset, 3);
    const qsizetype header = kTestHfsBTreeHeaderRecordOffset;
    writeBe16(&node, header + kTestHfsBTreeHeaderTreeDepthOffset, 1);
    writeBe32(&node, header + kTestHfsBTreeHeaderRootNodeOffset, 1);
    writeBe32(&node, header + kTestHfsBTreeHeaderLeafRecordsOffset, kTestHfsDeepFixtureFileCount);
    writeBe32(&node, header + kTestHfsBTreeHeaderFirstLeafNodeOffset, 1);
    writeBe32(&node, header + kTestHfsBTreeHeaderLastLeafNodeOffset, 1);
    writeBe16(&node, header + kTestHfsBTreeHeaderNodeSizeOffset, kTestHfsCatalogNodeSize);
    writeBe16(&node, header + kTestHfsBTreeHeaderMaxKeyLengthOffset, kTestHfsCatalogMaxKeyLength);
    writeBe32(&node, header + kTestHfsBTreeHeaderTotalNodesOffset, kTestHfsDeepCatalogTotalNodes);
    writeBe32(&node,
              header + kTestHfsBTreeHeaderFreeNodesOffset,
              kTestHfsDeepCatalogTotalNodes - 2);
    node[header + kTestHfsBTreeHeaderKeyCompareTypeOffset] = static_cast<char>(0xCF);
    writeBe32(&node,
              header + kTestHfsBTreeHeaderAttributesOffset,
              kTestHfsBTreeBigKeysMask | kTestHfsBTreeVariableIndexKeysMask);
    // 32-byte map record (256 bits) so all 160 nodes fit one map record.
    writeHfsNodeOffsets(&node, {14, 120, 248}, 280);
    setFixtureMapBit(&node, 0, true);
    setFixtureMapBit(&node, 1, true);
    std::copy(node.cbegin(), node.cend(), image.begin() + nodeOffset);

    QVector<QByteArray> records;
    for (int index = 0; index < kTestHfsDeepFixtureFileCount; ++index) {
        const QString name = QStringLiteral("seed-%1").arg(index);
        records.append(hfsCatalogRecord(
            2, name, hfsEmptyFileRecordFixture(16U + static_cast<uint32_t>(index))));
    }
    writeHfsCatalogLeafRecords(&image, kTestHfsDeepCatalogStartBlock + 1, records);
    return image;
}

void writeHfsExtentsMutationHeaderNode(QByteArray* image,
                                       uint32_t totalNodes,
                                       uint32_t freeNodes,
                                       uint32_t leafRecords,
                                       uint32_t rootNode) {
    const qsizetype nodeOffset =
        static_cast<qsizetype>(kTestHfsExtentsStartBlock * kTestHfsBlockSize);
    QByteArray node(kTestHfsExtentsNodeSize, '\0');
    node[kTestHfsBTreeKindOffset] = static_cast<char>(1);
    writeBe16(&node, kTestHfsBTreeNumRecordsOffset, 3);

    const qsizetype header = kTestHfsBTreeHeaderRecordOffset;
    writeBe16(&node, header + kTestHfsBTreeHeaderTreeDepthOffset, rootNode == 0 ? 0 : 1);
    writeBe32(&node, header + kTestHfsBTreeHeaderRootNodeOffset, rootNode);
    writeBe32(&node, header + kTestHfsBTreeHeaderLeafRecordsOffset, leafRecords);
    writeBe32(&node, header + kTestHfsBTreeHeaderFirstLeafNodeOffset, rootNode);
    writeBe32(&node, header + kTestHfsBTreeHeaderLastLeafNodeOffset, rootNode);
    writeBe16(&node, header + kTestHfsBTreeHeaderNodeSizeOffset, kTestHfsExtentsNodeSize);
    writeBe16(&node, header + kTestHfsBTreeHeaderMaxKeyLengthOffset, kTestHfsExtentsMaxKeyLength);
    writeBe32(&node, header + kTestHfsBTreeHeaderTotalNodesOffset, totalNodes);
    writeBe32(&node, header + kTestHfsBTreeHeaderFreeNodesOffset, freeNodes);
    node[header + kTestHfsBTreeHeaderKeyCompareTypeOffset] = static_cast<char>(0xBC);
    writeBe32(&node, header + kTestHfsBTreeHeaderAttributesOffset, kTestHfsBTreeBigKeysMask);
    writeHfsNodeOffsets(&node, {14, 120, 248}, 256);
    setFixtureMapBit(&node, 0, true);
    if (rootNode != 0) {
        setFixtureMapBit(&node, rootNode, true);
    }

    std::copy(node.cbegin(), node.cend(), image->begin() + nodeOffset);
}

QByteArray hfsExtentsGrowthFixture() {
    QByteArray image = hfsReaderFixture();
    writeBe32(&image, kTestHfsHeaderOffset + kTestHfsFreeBlocksOffset, 12);
    writeHfsFork(&image,
                 kTestHfsHeaderOffset + kTestHfsExtentsForkOffset,
                 kTestHfsExtentsNodeSize * 4,
                 1,
                 kTestHfsExtentsStartBlock);
    writeHfsExtentsMutationHeaderNode(&image, 4, 3, 0, 0);

    setHfsAllocationBit(&image, kTestHfsExtentsStartBlock);
    for (uint32_t block = 7; block <= 40; ++block) {
        setHfsAllocationBit(&image, block);
    }
    for (uint32_t block = 21; block <= 39; block += 2) {
        clearHfsAllocationBit(&image, block);
    }
    for (uint32_t block = 41; block < 60; ++block) {
        setHfsAllocationBit(&image, block);
    }
    for (uint32_t block = 61; block < 64; ++block) {
        setHfsAllocationBit(&image, block);
    }
    setHfsAllocationBit(&image, 1);
    return image;
}

QByteArray hfsExtentsSplitFixture() {
    QByteArray image = hfsExtentsGrowthFixture();
    writeHfsFork(&image,
                 kTestHfsHeaderOffset + kTestHfsExtentsForkOffset,
                 kTestHfsExtentsNodeSize * 8,
                 2,
                 kTestHfsExtentsStartBlock);
    setHfsAllocationBit(&image, kTestHfsExtentsStartBlock + 1);
    writeHfsExtentsMutationHeaderNode(&image, 8, 6, 12, 1);

    QVector<QByteArray> records;
    for (int index = 0; index < 12; ++index) {
        records.append(hfsExtentsOverflowRecord(99,
                                                kTestHfsDataForkType,
                                                static_cast<uint32_t>(index) * 8U,
                                                45U + static_cast<uint32_t>(index),
                                                1));
    }
    writeHfsExtentsLeafNode(&image, records);
    return image;
}

struct HfsFixtureExtentsNode {
    int8_t kind{0};
    uint8_t height{0};
    uint32_t forward_link{0};
    uint32_t backward_link{0};
};

void writeHfsExtentsNodeBytesAt(QByteArray* image,
                                uint32_t nodeNumber,
                                const HfsFixtureExtentsNode& meta,
                                const QVector<QByteArray>& records) {
    const qsizetype nodeOffset =
        static_cast<qsizetype>(kTestHfsExtentsStartBlock * kTestHfsBlockSize) +
        static_cast<qsizetype>(nodeNumber) * kTestHfsExtentsNodeSize;
    QByteArray node(kTestHfsExtentsNodeSize, '\0');
    writeBe32(&node, 0, meta.forward_link);
    writeBe32(&node, 4, meta.backward_link);
    node[kTestHfsBTreeKindOffset] = static_cast<char>(meta.kind);
    node[kTestHfsBTreeHeightOffset] = static_cast<char>(meta.height);
    writeBe16(&node, kTestHfsBTreeNumRecordsOffset, static_cast<uint16_t>(records.size()));
    QVector<qsizetype> offsets;
    qsizetype cursor = 14;
    for (const auto& record : records) {
        offsets.append(cursor);
        std::copy(record.cbegin(), record.cend(), node.begin() + cursor);
        cursor += record.size();
    }
    writeHfsNodeOffsets(&node, offsets, cursor);
    std::copy(node.cbegin(), node.cend(), image->begin() + nodeOffset);
}

void writeHfsExtentsLeafNodeAt(QByteArray* image,
                               uint32_t nodeNumber,
                               uint32_t forwardLink,
                               uint32_t backwardLink,
                               const QVector<QByteArray>& records) {
    writeHfsExtentsNodeBytesAt(image,
                               nodeNumber,
                               HfsFixtureExtentsNode{.kind = static_cast<int8_t>(0xFF),
                                                     .height = 1,
                                                     .forward_link = forwardLink,
                                                     .backward_link = backwardLink},
                               records);
}

void writeHfsExtentsIndexNodeAt(QByteArray* image,
                                uint32_t nodeNumber,
                                const QVector<QByteArray>& records) {
    writeHfsExtentsNodeBytesAt(
        image, nodeNumber, HfsFixtureExtentsNode{.kind = 0, .height = 2}, records);
}

void setExtentsHeaderMapBitInImage(QByteArray* image, uint32_t nodeNumber) {
    const qsizetype headerNodeOffset =
        static_cast<qsizetype>(kTestHfsExtentsStartBlock * kTestHfsBlockSize);
    const qsizetype offset = headerNodeOffset + kTestHfsBTreeHeaderMapRecordOffset + nodeNumber / 8;
    const auto mask = static_cast<char>(0x80U >> (nodeNumber % 8U));
    (*image)[offset] = static_cast<char>((*image)[offset] | mask);
}

QByteArray hfsExtentsIndexRecord(uint32_t fileId, uint32_t logicalStartBlock, uint32_t childNode) {
    QByteArray record =
        hfsExtentsOverflowRecord(fileId, kTestHfsDataForkType, logicalStartBlock, 0, 0)
            .left(2 + kTestHfsExtentsKeyLength);
    record.append(QByteArray(4, '\0'));
    writeBe32(&record, 2 + kTestHfsExtentsKeyLength, childNode);
    return record;
}

// A depth-2 extents tree whose first leaf (node 1) is full: a single inserted
// record splits it, exercising the streaming engine's leaf split below an
// existing index node (the non-root split branch the bounded engine never had).
QByteArray hfsExtentsTwoLeafFixture() {
    QByteArray image = hfsExtentsGrowthFixture();
    writeHfsFork(&image,
                 kTestHfsHeaderOffset + kTestHfsExtentsForkOffset,
                 kTestHfsExtentsNodeSize * 8,
                 2,
                 kTestHfsExtentsStartBlock);
    setHfsAllocationBit(&image, kTestHfsExtentsStartBlock + 1);
    // total_nodes 8, used 0/1/2/3 -> free_nodes 4 (room for one split: a new leaf
    // and a rebuilt index), root index node 3, leaves 1 and 2, 20 leaf records.
    // Leaf 2 holds eight records so it is not underfull and survives the split
    // (an underfull neighbour would merge away and hide the three-leaf result).
    writeHfsExtentsMutationHeaderNode(&image, 8, 4, 20, 3);
    // The mutation-header helper assumes a depth-1 tree (root == sole leaf); patch
    // the header for the real depth-2 shape: index root 3 over leaves 1 and 2.
    const qsizetype extentsHeaderRecord =
        static_cast<qsizetype>(kTestHfsExtentsStartBlock * kTestHfsBlockSize) +
        kTestHfsBTreeHeaderRecordOffset;
    writeBe16(&image, extentsHeaderRecord + kTestHfsBTreeHeaderTreeDepthOffset, 2);
    writeBe32(&image, extentsHeaderRecord + kTestHfsBTreeHeaderFirstLeafNodeOffset, 1);
    writeBe32(&image, extentsHeaderRecord + kTestHfsBTreeHeaderLastLeafNodeOffset, 2);
    setExtentsHeaderMapBitInImage(&image, 1);
    setExtentsHeaderMapBitInImage(&image, 2);

    QVector<QByteArray> leafOne;
    for (int index = 0; index < 12; ++index) {
        leafOne.append(hfsExtentsOverflowRecord(
            99, kTestHfsDataForkType, static_cast<uint32_t>(index) * 8U, 45U, 1));
    }
    QVector<QByteArray> leafTwo;
    for (int index = 12; index < 20; ++index) {
        leafTwo.append(hfsExtentsOverflowRecord(
            99, kTestHfsDataForkType, static_cast<uint32_t>(index) * 8U, 45U, 1));
    }
    writeHfsExtentsLeafNodeAt(&image, 1, 2, 0, leafOne);
    writeHfsExtentsLeafNodeAt(&image, 2, 0, 1, leafTwo);
    writeHfsExtentsIndexNodeAt(
        &image, 3, {hfsExtentsIndexRecord(99, 0U, 1U), hfsExtentsIndexRecord(99, 96U, 2U)});
    return image;
}

}  // namespace

class PartitionManagerCoreTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void fileSystemDetector_detectsRawSignatures();
    void fileSystemDetector_flagsMalformedMetadataSanityWarnings();
    void fileSystemDetector_readsProbeBytesFromPath();
    void fileSystemDetector_supplementsApfsSpaceManagerFromCheckpointData();
    void extFileSystemReader_listsDirectoriesAndReadsFiles();
    void extFileSystemReader_exportsDirectoriesRecursively();
    void extFileSystemReader_readsExtentMappedFilesAndBlocksUnsafePaths();
    void hfsFileSystemReader_listsDirectoriesAndReadsFiles();
    void hfsFileSystemReader_exportsDirectoriesAndResourceForks();
    void hfsFileSystemReader_checksCatalogConsistency();
    void hfsFileSystemReader_scansAttributeKeys();
    void hfsFileSystemReader_usesOverflowExtentsForCatalogAndFiles();
    void hfsFileSystemReader_readsResourceForksWithOverflowExtents();
    void hfsFileSystemReader_blocksUnsafePathsAndOversizedReads();
    void hfsFileSystemWriter_overwritesSameSizeFilesWithReadback();
    void hfsFileSystemWriter_overwritesOverflowExtentFilesWithReadback();
    void hfsFileSystemWriter_replacesFileWithinAllocatedBlocks();
    void hfsFileSystemWriter_replacesResourceForkWithinAllocatedBlocks();
    void hfsFileSystemWriter_growsForksWithAllocationBitmapUpdates();
    void hfsFileSystemReader_readsDecmpfsCompressedFiles();
    void hfsFileSystemWriter_replacesDecmpfsCompressedContent();
    void hfsFileSystemWriter_splitsCatalogRootLeafOnCreate();
    void hfsFileSystemWriter_mutatesDepthTwoCatalogs();
    void hfsFileSystemWriter_mutatesDepthThreeCatalogs();
    void hfsFileSystemWriter_mergesUnderfullCatalogLeavesOnDelete();
    void hfsFileSystemWriter_survivesRandomizedCreateDeleteChurn();
    void hfsFileSystemWriter_replaysJournalTransactions();
    void hfsFileSystemWriter_replaysBigEndianJournal();
    void hfsFileSystemWriter_writesIntoWrappedVolume();
    void hfsFileSystemWriter_growsCatalogNodePoolOrFailsClosed();
    void hfsFileSystemWriter_growsAttributesNodePoolOnRootLeafSplit();
    void hfsFileSystemWriter_growsForkIntoExtentsOverflowRecords();
    void hfsFileSystemWriter_splitsExtentsOverflowRootLeaf();
    void hfsFileSystemWriter_collapsesDepthTwoExtentsTreeOnDelete();
    void hfsFileSystemWriter_splitsLeafBelowExistingIndexNode();
    void hfsFileSystemWriter_truncatesResourceForkWithinAllocatedBlocks();
    void hfsFileSystemWriter_replacesInlineAttributesWithReadback();
    void hfsFileSystemWriter_createsAndDeletesEmptyFilesWithCatalogReadback();
    void hfsFileSystemWriter_deletesAllocatedFilesAndReleasesBitmap();
    void hfsFileSystemWriter_createsHardlinksAndSymlinks();
    void apfsFileSystemReader_rejectsCorruptMetadataChecksum();
    void apfsWriter_computesAndVerifiesObjectChecksums();
    void apfsWriter_computesMultiChunkContainerGeometry();
    void apfsWriter_blocksOversizedGeneratedContainers();
    void apfsWriter_blocksGeneratedLayoutWithSnapshotState();
    void apfsWriter_preflightFailsClosedUntilCertified();
    void apfsWriter_inPlaceCheckpointCommitAdvancesTransaction();
    void apfsWriter_inPlaceFileInsertCommitAddsReadableFile();
    void apfsWriter_inPlaceFileWriteCreatesThenReplaces();
    void apfsWriter_inPlaceDirectoryCreatePreservesTree();
    void apfsWriter_inPlaceDirectoryMutationsRoundTrip();
    void apfsWriter_inPlaceDirectoryChildRename();
    void apfsWriter_inPlaceFileMoveAcrossDirectories();
    void apfsWriter_inPlaceFilePatchPreservesObjectId();
    void apfsWriter_inPlaceSnapshotCreateAddsSnapshot();
    void apfsWriter_inPlaceSnapshotDeleteRestoresSnapshotFreeState();
    void apfsWriter_inPlaceSnapshotRevertTagsDeferredRevert();
    void apfsWriter_rawCommitWrappersMutateConfirmedTarget();
    void apfsWriter_rawCommitWrappersFailClosedWithoutRawDevice();
    void apfsWriter_formatsMetadataOverflowDeadZoneMultiBlockSpaceman();
    void apfsWriter_formatsMultiVolumeContainer();
    void apfsWriter_insertsInlineCompressedFile();
    void apfsWriter_insertsSparseAndXattrFile();
    void apfsWriter_clonesFileSharingDataStream();
    void apfsWriter_addsHardLinkToFile();
    void apfsWriter_blocksSealedVolumeMutation();
    void apfsCrypto_matchesPublishedVectors();
    void apfsKeybag_reproducesHarvestedFileVaultBlobs();
    void apfsWriter_formatsUnlockableEncryptedVolume();
    void apfsReader_decryptsEncryptedVolumeWithCredential();
    void apfsWriter_recoveryKeyUnlocksEncryptedVolume();
    void apfsKeybag_failsClosedOnMalformedEntry();
    void apfsWriter_inPlaceFileInsertWritesMultiBlockExtent();
    void apfsWriter_inPlaceFileInsertChainsAndPreservesExistingFiles();
    void apfsWriter_inPlaceFileDeleteRemovesFileAndPreservesOthers();
    void apfsWriter_inPlaceFileRenameKeepsContentAndObjectId();
    void apfsWriter_inPlaceFileInsertGrowsIntoMultiLeafFsTree();
    void apfsWriter_buildsTwoLevelFsTreeOnOverflow();
    void apfsWriter_crashSafeIpSlotRoundRobinsThreeSlots();
    void apfsWriter_readsGeneratedLiveCibAddr();
    void apfsWriter_crashBeforeCheckpointDurableRollsBack();
    void fileSystemRegistry_reportsNativeAndNonNativeCapability();
    void fileSystemToolManifest_validatesPinnedTool();
    void fileSystemToolManifest_blocksMissingMetadataHashMismatchAndPathTraversal();
    void fileSystemToolRunner_buildsReadOnlyCheckCommands();
    void fileSystemToolRunner_buildsExtWriteCommandShapesWithConfirmationGate();
    void fileSystemToolRunner_blocksUnapprovedOrUnsupportedReadOnlyChecks();
    void fileSystemToolRunner_resolvesApprovedToolPath();
    void inventoryParser_parsesDiskAndPartition();
    void inventoryParser_infersHiddenProtectedPartitionFileSystems();
    void inventoryParser_keepsRawBasicDiskInitializable();
    void inventoryScript_handlesRawDisksWithoutAbort();
    void safetyValidator_blocksSystemPartitionDelete();
    void scriptBuilder_createRespectsWizardPayload();
    void scriptBuilder_buildsNonNativeCreateFormatScripts();
    void scriptBuilder_rejectsInvalidCreatePartitionType();
    void scriptBuilder_rejectsUnsupportedAllocationUnit();
    void scriptBuilder_rejectsNativeFormatForNonWindowsFilesystem();
    void scriptBuilder_buildsExtToolScriptsWithManifestGate();
    void scriptBuilder_buildsApfsRootFileMutationScripts();
    void scriptBuilder_buildsHfsFileMutationScripts();
    void scriptBuilder_buildsLinuxSwapFormatScript();
    void scriptBuilder_buildsResizeScript();
    void scriptBuilder_buildsMergeScript();
    void scriptBuilder_formatsByPartitionIdentity();
    void scriptBuilder_setsPartitionLabelByMountedDriveLetter();
    void scriptBuilder_buildsAdvancedParityScripts();
    void scriptBuilder_buildsRecoveredPartitionRestoreScript();
    void scriptBuilder_buildsCloneVerificationScript();
    void scriptBuilder_buildsOffsetPartitionCloneScript();
    void scriptBuilder_buildsOsMigrationBootValidationScript();
    void safetyValidator_blocksUnsafeParityOperations();
    void scriptBuilder_buildsClearLevelDiskWipeScript();
    void safetyValidator_blocksUnsafeSystemStyleConversion();
    void scriptBuilder_buildsEmptyDataDiskStyleConversionScript();
    void safetyValidator_requiresCloneOverwriteConfirmation();
    void safetyValidator_createImageUsesReadOnlyRiskAndBlocksUnsafeDestinations();
    void safetyValidator_restoreImageRequiresSizesAndOverwriteConfirmation();
    void safetyValidator_requiresRecoveredPartitionRestoreAcknowledgement();
    void safetyValidator_requiresPartitionRegionCloneConfirmation();
    void safetyValidator_blocksUnsafePayloadTargetDisk();
    void safetyValidator_blocksTooSmallCloneTarget();
    void safetyValidator_blocksTooSmallPartitionRegionClone();
    void safetyValidator_blocksUnsupportedFileSystemConversion();
    void safetyValidator_gatesNonNativeWriteOperations();
    void safetyValidator_gatesApfsRootFileMutations();
    void safetyValidator_gatesHfsFileMutations();
    void safetyValidator_allowsResizeIntoAdjacentFreeSpace();
    void safetyValidator_blocksResizeBeyondAdjacentFreeSpace();
    void safetyValidator_blocksResizeBelowUsedBytes();
    void safetyValidator_blocksNoopResize();
    void safetyValidator_blocksResizeStartMovePayload();
    void safetyValidator_blocksResizeDonorPayload();
    void safetyValidator_blocksOversizedCreate();
    void safetyValidator_blocksInvalidCreateTypePayload();
    void safetyValidator_blocksCreateOffsetOutsideSelectedRegion();
    void safetyValidator_blocksDynamicUnallocatedCreate();
    void scriptBuilder_buildsMbr2GptScript();
    void scriptBuilder_buildsAllocateFreeSpaceScript();
    void safetyValidator_blocksUnsafeAllocateFreeSpacePayloads();
    void scriptBuilder_buildsOfflineMoveAndMetadataScripts();
    void safetyValidator_allowsConfirmedOfflineRebuildOperations();
    void safetyValidator_blocksUnsafeOfflineRebuildOperations();
    void scriptBuilder_buildsChangeClusterSizeScript();
    void safetyValidator_blocksUnsafeClusterSizePayloads();
    void scriptBuilder_buildsBitLockerMutationScripts();
    void scriptBuilder_buildsDirectDefragScript();
    void safetyValidator_allowsHddDefragOnlyOnReportedHdd();
    void scriptBuilder_buildsBiosBootRepairScript();
    void operationQueue_blocksLayoutMismatch();
    void operationQueue_redoAvailableOnlyAfterUndo();
    void powershellQuoting_escapesSingleQuotes();
    void fileRecoveryEngine_scansAndRestoresOfflineImage();
};

QByteArray fixtureJson() {
    const QJsonObject systemPartition{{QStringLiteral("PartitionNumber"), 1},
                                      {QStringLiteral("Guid"), QStringLiteral("{efi}")},
                                      {QStringLiteral("Type"), QStringLiteral("System")},
                                      {QStringLiteral("GptType"),
                                       QStringLiteral("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")},
                                      {QStringLiteral("Offset"), QStringLiteral("1048576")},
                                      {QStringLiteral("Size"), QStringLiteral("104857600")},
                                      {QStringLiteral("IsBoot"), false},
                                      {QStringLiteral("IsSystem"), true},
                                      {QStringLiteral("IsReadOnly"), false},
                                      {QStringLiteral("Volume"), QJsonValue()}};
    const QJsonObject dataVolume{{QStringLiteral("DriveLetter"), QStringLiteral("C")},
                                 {QStringLiteral("FileSystem"), QStringLiteral("NTFS")},
                                 {QStringLiteral("FileSystemLabel"), QStringLiteral("Windows")},
                                 {QStringLiteral("HealthStatus"), QStringLiteral("Healthy")},
                                 {QStringLiteral("Size"), QStringLiteral("53687091200")},
                                 {QStringLiteral("SizeRemaining"), QStringLiteral("26843545600")},
                                 {QStringLiteral("BitLockerEnabled"), true},
                                 {QStringLiteral("BitLockerLocked"), false},
                                 {QStringLiteral("DirtyBitSet"), true}};
    const QJsonObject dataPartition{{QStringLiteral("PartitionNumber"), 2},
                                    {QStringLiteral("Guid"), QStringLiteral("{data}")},
                                    {QStringLiteral("Type"), QStringLiteral("Basic")},
                                    {QStringLiteral("GptType"),
                                     QStringLiteral("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7")},
                                    {QStringLiteral("Offset"), QStringLiteral("105906176")},
                                    {QStringLiteral("Size"), QStringLiteral("53687091200")},
                                    {QStringLiteral("IsBoot"), true},
                                    {QStringLiteral("IsSystem"), true},
                                    {QStringLiteral("IsReadOnly"), false},
                                    {QStringLiteral("Volume"), dataVolume}};
    const QJsonObject disk{
        {QStringLiteral("Number"), 0},
        {QStringLiteral("FriendlyName"), QStringLiteral("Test SSD")},
        {QStringLiteral("SerialNumber"), QStringLiteral("ABC123")},
        {QStringLiteral("HealthStatus"), QStringLiteral("Healthy")},
        {QStringLiteral("OperationalStatus"), QStringLiteral("Online")},
        {QStringLiteral("BusType"), QStringLiteral("NVMe")},
        {QStringLiteral("MediaType"), QStringLiteral("SSD")},
        {QStringLiteral("PartitionStyle"), QStringLiteral("GPT")},
        {QStringLiteral("SmartSummary"), QStringLiteral("Storage reliability counters available")},
        {QStringLiteral("TemperatureCelsius"), 38},
        {QStringLiteral("PowerOnHours"), QStringLiteral("1234")},
        {QStringLiteral("ReadErrorsTotal"), QStringLiteral("2")},
        {QStringLiteral("WriteErrorsTotal"), QStringLiteral("3")},
        {QStringLiteral("WearPercent"), QStringLiteral("4")},
        {QStringLiteral("Size"), QStringLiteral("107374182400")},
        {QStringLiteral("IsBoot"), true},
        {QStringLiteral("IsSystem"), true},
        {QStringLiteral("IsReadOnly"), false},
        {QStringLiteral("Partitions"), QJsonArray{systemPartition, dataPartition}}};
    return QJsonDocument(QJsonArray{disk}).toJson(QJsonDocument::Compact);
}

QByteArray rawBasicDiskFixtureJson() {
    const QJsonObject disk{{QStringLiteral("Number"), 2},
                           {QStringLiteral("FriendlyName"), QStringLiteral("Disposable RAW Disk")},
                           {QStringLiteral("SerialNumber"), QStringLiteral("RAW123")},
                           {QStringLiteral("HealthStatus"), QStringLiteral("Healthy")},
                           {QStringLiteral("OperationalStatus"), QStringLiteral("Online")},
                           {QStringLiteral("BusType"), QStringLiteral("SATA")},
                           {QStringLiteral("MediaType"), QStringLiteral("HDD")},
                           {QStringLiteral("PartitionStyle"), QStringLiteral("RAW")},
                           {QStringLiteral("Size"), QStringLiteral("4294967296")},
                           {QStringLiteral("IsBoot"), false},
                           {QStringLiteral("IsSystem"), false},
                           {QStringLiteral("IsReadOnly"), false},
                           {QStringLiteral("IsDynamic"), false},
                           {QStringLiteral("Partitions"), QJsonArray()}};
    return QJsonDocument(QJsonArray{disk}).toJson(QJsonDocument::Compact);
}

QByteArray hiddenProtectedFixtureJson() {
    const QJsonObject efiPartition{{QStringLiteral("PartitionNumber"), 1},
                                   {QStringLiteral("Guid"), QStringLiteral("{efi}")},
                                   {QStringLiteral("Type"), QStringLiteral("System")},
                                   {QStringLiteral("GptType"),
                                    QStringLiteral("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")},
                                   {QStringLiteral("Offset"), QStringLiteral("1048576")},
                                   {QStringLiteral("Size"), QStringLiteral("103809024")},
                                   {QStringLiteral("IsSystem"), true},
                                   {QStringLiteral("Volume"), QJsonValue()}};
    const QJsonObject msrPartition{{QStringLiteral("PartitionNumber"), 2},
                                   {QStringLiteral("Guid"), QStringLiteral("{msr}")},
                                   {QStringLiteral("Type"), QStringLiteral("Reserved")},
                                   {QStringLiteral("GptType"),
                                    QStringLiteral("E3C9E316-0B5C-4DB8-817D-F92DF00215AE")},
                                   {QStringLiteral("Offset"), QStringLiteral("104857600")},
                                   {QStringLiteral("Size"), QStringLiteral("16777216")},
                                   {QStringLiteral("Volume"), QJsonValue()}};
    const QJsonObject disk{{QStringLiteral("Number"), 0},
                           {QStringLiteral("FriendlyName"), QStringLiteral("System Disk")},
                           {QStringLiteral("HealthStatus"), QStringLiteral("Healthy")},
                           {QStringLiteral("OperationalStatus"), QStringLiteral("Online")},
                           {QStringLiteral("BusType"), QStringLiteral("NVMe")},
                           {QStringLiteral("MediaType"), QStringLiteral("SSD")},
                           {QStringLiteral("PartitionStyle"), QStringLiteral("GPT")},
                           {QStringLiteral("Size"), QStringLiteral("107374182400")},
                           {QStringLiteral("IsBoot"), true},
                           {QStringLiteral("IsSystem"), true},
                           {QStringLiteral("IsReadOnly"), false},
                           {QStringLiteral("Partitions"), QJsonArray{efiPartition, msrPartition}}};
    return QJsonDocument(QJsonArray{disk}).toJson(QJsonDocument::Compact);
}

void expectRawDetection(const QByteArray& bytes, const QString& fileSystem) {
    const auto detection = PartitionFileSystemDetector::detectBytes(bytes, bytes.size());
    QVERIFY(detection.has_value());
    QCOMPARE(detection->file_system, fileSystem);
    QCOMPARE(detection->source, PartitionFileSystemDetector::rawSignatureSource());
}

void verifyExtRawDetections() {
    auto ext2 = signatureFixture();
    ext2[kTestExtSuperblockOffset + kTestExtMagicOffset] = static_cast<char>(0x53);
    ext2[kTestExtSuperblockOffset + kTestExtMagicOffset + 1] = static_cast<char>(0xEF);
    expectRawDetection(ext2, QStringLiteral("ext2"));

    auto ext3 = ext2;
    writeLe32(&ext3,
              kTestExtSuperblockOffset + kTestExtFeatureCompatOffset,
              kTestExtCompatHasJournal);
    expectRawDetection(ext3, QStringLiteral("ext3"));

    auto ext4 = ext2;
    writeLe32(&ext4, kTestExtSuperblockOffset + kTestExtInodesCountOffset, 128);
    writeLe32(&ext4, kTestExtSuperblockOffset + kTestExtBlocksCountLoOffset, 2048);
    writeLe32(&ext4, kTestExtSuperblockOffset + kTestExtFreeBlocksCountLoOffset, 512);
    writeLe32(&ext4, kTestExtSuperblockOffset + kTestExtFreeInodesCountOffset, 96);
    writeLe32(&ext4, kTestExtSuperblockOffset + kTestExtLogBlockSizeOffset, 2);
    writeLe32(&ext4, kTestExtSuperblockOffset + kTestExtBlocksPerGroupOffset, 32'768);
    writeLe32(&ext4, kTestExtSuperblockOffset + kTestExtInodesPerGroupOffset, 8192);
    writeLe32(&ext4,
              kTestExtSuperblockOffset + kTestExtFeatureIncompatOffset,
              kTestExtIncompatExtents);
    writeLe32(&ext4, kTestExtSuperblockOffset + kTestExtFeatureRoCompatOffset, 0x0400);
    writeAscii(&ext4, kTestExtSuperblockOffset + kTestExtVolumeNameOffset, "SAK_EXT4");
    const auto detection = PartitionFileSystemDetector::detectBytes(ext4, ext4.size());
    QVERIFY(detection.has_value());
    QCOMPARE(detection->file_system, QStringLiteral("ext4"));
    QCOMPARE(detection->total_bytes, 4096ULL * 2048ULL);
    QCOMPARE(detection->free_bytes, 4096ULL * 512ULL);
    const QString details = detection->details.join(' ');
    QVERIFY(details.contains(QStringLiteral("Block size: 4096")));
    QVERIFY(details.contains(QStringLiteral("Inodes: 128")));
    QVERIFY(details.contains(QStringLiteral("Volume label: SAK_EXT4")));
    QVERIFY(details.contains(QStringLiteral("Feature incompat: 0x00000040")));
}

void verifyWindowsRawDetections() {
    auto ntfs = signatureFixture();
    writeBootSectorSignature(&ntfs);
    writeAscii(&ntfs, kTestBootSectorOemOffset, "NTFS    ");
    expectRawDetection(ntfs, QStringLiteral("NTFS"));

    auto exfat = signatureFixture();
    writeBootSectorSignature(&exfat);
    writeAscii(&exfat, kTestBootSectorOemOffset, "EXFAT   ");
    expectRawDetection(exfat, QStringLiteral("exFAT"));

    auto fat32 = signatureFixture();
    writeBootSectorSignature(&fat32);
    writeAscii(&fat32, kTestFat32TypeOffset, "FAT32   ");
    expectRawDetection(fat32, QStringLiteral("FAT32"));

    auto fat16 = signatureFixture();
    writeBootSectorSignature(&fat16);
    writeAscii(&fat16, kTestFat16TypeOffset, "FAT16   ");
    expectRawDetection(fat16, QStringLiteral("FAT16"));

    auto fat12 = signatureFixture();
    writeBootSectorSignature(&fat12);
    writeAscii(&fat12, kTestFat16TypeOffset, "FAT12   ");
    expectRawDetection(fat12, QStringLiteral("FAT12"));
}

void verifyXfsRawDetection() {
    auto xfs = signatureFixture();
    writeAscii(&xfs, 0, "XFSB");
    writeBe32(&xfs, kTestXfsBlockSizeOffset, 4096);
    writeBe64(&xfs, kTestXfsDataBlocksOffset, 32'768);
    writeRaw(&xfs, kTestXfsUuidOffset, QByteArray::fromHex("00112233445566778899aabbccddeeff"));
    writeBe32(&xfs, kTestXfsAgBlocksOffset, 4096);
    writeBe32(&xfs, kTestXfsAgCountOffset, 8);
    writeBe16(&xfs, kTestXfsVersionOffset, 0xB084);
    writeBe16(&xfs, kTestXfsSectorSizeOffset, 512);
    writeBe16(&xfs, kTestXfsInodeSizeOffset, 512);
    writeAscii(&xfs, kTestXfsNameOffset, "SAK_XFS");
    writeBe64(&xfs, kTestXfsInodeCountOffset, 128);
    writeBe64(&xfs, kTestXfsFreeInodeCountOffset, 64);
    writeBe64(&xfs, kTestXfsFreeDataBlocksOffset, 8192);
    writeBe32(&xfs, kTestXfsFeatures2Offset, 0x8);
    const auto detection = PartitionFileSystemDetector::detectBytes(xfs, xfs.size());
    QVERIFY(detection.has_value());
    QCOMPARE(detection->file_system, QStringLiteral("XFS"));
    QCOMPARE(detection->total_bytes, 4096ULL * 32'768ULL);
    QCOMPARE(detection->free_bytes, 4096ULL * 8192ULL);
    const QString details = detection->details.join(' ');
    QVERIFY(details.contains(QStringLiteral("Block size: 4096")));
    QVERIFY(details.contains(QStringLiteral("Allocation groups: 8")));
    QVERIFY(details.contains(QStringLiteral("UUID: 00112233-4455-6677-8899-aabbccddeeff")));
    QVERIFY(details.contains(QStringLiteral("File-system name: SAK_XFS")));
    QVERIFY(details.contains(QStringLiteral("Features2: 0x00000008")));
    QVERIFY(details.contains(
        QStringLiteral("Metadata sanity: XFS superblock geometry is internally consistent")));
}

void verifyBtrfsRawDetection() {
    auto btrfs = signatureFixture();
    writeAscii(&btrfs, kTestBtrfsMagicOffset, "_BHRfS_M");
    writeRaw(&btrfs,
             kTestBtrfsSuperblockOffset + kTestBtrfsUuidOffset,
             QByteArray::fromHex("102132435465768798a9babbdcedfe0f"));
    writeLe64(&btrfs, kTestBtrfsSuperblockOffset + kTestBtrfsGenerationOffset, 42);
    writeLe64(&btrfs, kTestBtrfsSuperblockOffset + kTestBtrfsTotalBytesOffset, 268'435'456);
    writeLe64(&btrfs, kTestBtrfsSuperblockOffset + kTestBtrfsBytesUsedOffset, 67'108'864);
    writeLe64(&btrfs, kTestBtrfsSuperblockOffset + kTestBtrfsNumDevicesOffset, 1);
    writeLe32(&btrfs, kTestBtrfsSuperblockOffset + kTestBtrfsSectorSizeOffset, 4096);
    writeLe32(&btrfs, kTestBtrfsSuperblockOffset + kTestBtrfsNodeSizeOffset, 16'384);
    writeLe32(&btrfs, kTestBtrfsSuperblockOffset + kTestBtrfsLeafSizeOffset, 16'384);
    writeLe64(&btrfs, kTestBtrfsSuperblockOffset + kTestBtrfsCompatFlagsOffset, 1);
    writeLe64(&btrfs, kTestBtrfsSuperblockOffset + kTestBtrfsCompatRoFlagsOffset, 2);
    writeLe64(&btrfs, kTestBtrfsSuperblockOffset + kTestBtrfsIncompatFlagsOffset, 0x40);
    writeAscii(&btrfs, kTestBtrfsSuperblockOffset + kTestBtrfsLabelOffset, "SAK_BTRFS");
    const auto detection = PartitionFileSystemDetector::detectBytes(btrfs, btrfs.size());
    QVERIFY(detection.has_value());
    QCOMPARE(detection->file_system, QStringLiteral("Btrfs"));
    QCOMPARE(detection->total_bytes, 268'435'456ULL);
    QCOMPARE(detection->free_bytes, 201'326'592ULL);
    const QString details = detection->details.join(' ');
    QVERIFY(details.contains(QStringLiteral("Label: SAK_BTRFS")));
    QVERIFY(details.contains(QStringLiteral("Generation: 42")));
    QVERIFY(details.contains(QStringLiteral("Node size: 16384")));
    QVERIFY(details.contains(QStringLiteral("Incompat flags: 0x0000000000000040")));
    QVERIFY(details.contains(
        QStringLiteral("Metadata sanity: Btrfs superblock counters are internally consistent")));
}

void verifyHfsRawDetections() {
    auto hfsPlus = signatureFixture();
    writeAscii(&hfsPlus, kTestHfsHeaderOffset, "H+");
    writeBe16(&hfsPlus, kTestHfsHeaderOffset + kTestHfsVersionOffset, 4);
    writeBe32(&hfsPlus, kTestHfsHeaderOffset + kTestHfsAttributesOffset, kTestHfsJournaledMask);
    writeBe32(&hfsPlus, kTestHfsHeaderOffset + kTestHfsFileCountOffset, 12);
    writeBe32(&hfsPlus, kTestHfsHeaderOffset + kTestHfsFolderCountOffset, 3);
    writeBe32(&hfsPlus, kTestHfsHeaderOffset + kTestHfsBlockSizeOffset, 4096);
    writeBe32(&hfsPlus, kTestHfsHeaderOffset + kTestHfsTotalBlocksOffset, 1000);
    writeBe32(&hfsPlus, kTestHfsHeaderOffset + kTestHfsFreeBlocksOffset, 250);
    const auto detection = PartitionFileSystemDetector::detectBytes(hfsPlus, hfsPlus.size());
    QVERIFY(detection.has_value());
    QCOMPARE(detection->file_system, QStringLiteral("HFS+"));
    QCOMPARE(detection->source, PartitionFileSystemDetector::rawSignatureSource());
    QCOMPARE(detection->total_bytes, 4096ULL * 1000ULL);
    QCOMPARE(detection->free_bytes, 4096ULL * 250ULL);
    QVERIFY(detection->details.join(' ').contains(QStringLiteral("Journaled: Yes")));

    auto hfsx = signatureFixture();
    writeAscii(&hfsx, kTestHfsHeaderOffset, "HX");
    expectRawDetection(hfsx, QStringLiteral("HFSX"));
}

void verifyWrappedHfsRawDetection() {
    auto wrapped = signatureFixture();
    writeAscii(&wrapped, kTestHfsWrapperMdbOffset, "BD");
    writeBe32(&wrapped,
              kTestHfsWrapperMdbOffset + kTestHfsWrapperAllocationBlockSizeOffset,
              kTestHfsWrapperAllocationBlockSize);
    writeBe16(&wrapped,
              kTestHfsWrapperMdbOffset + kTestHfsWrapperAllocationBlockStartOffset,
              kTestHfsWrapperAllocationStartSector);
    writeAscii(&wrapped, kTestHfsWrapperMdbOffset + kTestHfsWrapperEmbeddedSignatureOffset, "H+");
    writeBe16(&wrapped,
              kTestHfsWrapperMdbOffset + kTestHfsWrapperEmbeddedExtentStartOffset,
              kTestHfsWrapperEmbeddedStartBlock);
    writeBe16(&wrapped,
              kTestHfsWrapperMdbOffset + kTestHfsWrapperEmbeddedExtentCountOffset,
              kTestHfsWrapperEmbeddedBlockCount);
    const qsizetype headerOffset = static_cast<qsizetype>(
        kTestHfsWrapperAllocationStartSector * 512ULL +
        kTestHfsWrapperEmbeddedStartBlock * kTestHfsWrapperAllocationBlockSize +
        kTestHfsHeaderOffset);
    writeAscii(&wrapped, headerOffset, "H+");
    writeBe16(&wrapped, headerOffset + kTestHfsVersionOffset, 4);
    writeBe32(&wrapped, headerOffset + kTestHfsAttributesOffset, kTestHfsJournaledMask);
    writeBe32(&wrapped, headerOffset + kTestHfsFileCountOffset, 44);
    writeBe32(&wrapped, headerOffset + kTestHfsFolderCountOffset, 11);
    writeBe32(&wrapped, headerOffset + kTestHfsBlockSizeOffset, kTestHfsWrapperAllocationBlockSize);
    writeBe32(&wrapped, headerOffset + kTestHfsTotalBlocksOffset, 2000);
    writeBe32(&wrapped, headerOffset + kTestHfsFreeBlocksOffset, 500);
    const auto detection = PartitionFileSystemDetector::detectBytes(wrapped, wrapped.size());
    QVERIFY(detection.has_value());
    QCOMPARE(detection->file_system, QStringLiteral("HFS+"));
    QCOMPARE(detection->total_bytes, 4096ULL * 2000ULL);
    QCOMPARE(detection->free_bytes, 4096ULL * 500ULL);
    const QString details = detection->details.join(' ');
    QVERIFY(details.contains(QStringLiteral("HFS wrapper: Yes")));
    QVERIFY(details.contains(QStringLiteral("Embedded offset: 45056")));
}

void writeApfsContainerFixture(QByteArray* apfs) {
    writeAscii(apfs, kTestApfsMagicOffset, "NXSB");
    writeLe32(apfs, kTestApfsBlockSizeOffset, 4096);
    writeLe64(apfs, kTestApfsBlockCountOffset, 4096);
    writeLe64(apfs, kTestApfsFeaturesOffset, 0x2);
    writeLe64(apfs, kTestApfsReadOnlyCompatibleFeaturesOffset, 0x4);
    writeLe64(apfs, kTestApfsIncompatibleFeaturesOffset, 0x100);
    writeRaw(apfs, kTestApfsUuidOffset, QByteArray::fromHex("11223344556677889900aabbccddeeff"));
    writeLe64(apfs, kTestApfsNextObjectIdOffset, 77);
    writeLe64(apfs, kTestApfsNextTransactionIdOffset, 88);
    writeLe32(apfs, kTestApfsCheckpointDescriptorBlocksOffset, 4);
    writeLe32(apfs, kTestApfsCheckpointDataBlocksOffset, 8);
    writeLe64(apfs, kTestApfsCheckpointDescriptorBaseOffset, 128);
    writeLe64(apfs, kTestApfsCheckpointDataBaseOffset, 256);
    writeLe32(apfs, kTestApfsCheckpointDescriptorNextOffset, 1);
    writeLe32(apfs, kTestApfsCheckpointDataNextOffset, 2);
    writeLe32(apfs, kTestApfsCheckpointDescriptorIndexOffset, 3);
    writeLe32(apfs, kTestApfsCheckpointDescriptorLengthOffset, 3);
    writeLe32(apfs, kTestApfsCheckpointDataIndexOffset, 6);
    writeLe32(apfs, kTestApfsCheckpointDataLengthOffset, 7);
    writeLe64(apfs, kTestApfsSpacemanOidOffset, 2001);
    writeLe64(apfs, kTestApfsObjectMapOidOffset, 2002);
    writeLe64(apfs, kTestApfsReaperOidOffset, 2003);
    writeLe32(apfs, kTestApfsMaxFileSystemsOffset, 100);
    writeLe64(apfs, kTestApfsFileSystemOidArrayOffset + 2 * 8, 3002);
    writeLe64(apfs, kTestApfsFileSystemOidArrayOffset + 5 * 8, 3005);
}

void writeApfsReferencedObjects(QByteArray* apfs) {
    writeLe64(apfs, kTestApfsSpacemanOffset + kTestApfsObjectOidOffset, 2001);
    writeLe64(apfs, kTestApfsSpacemanOffset + kTestApfsObjectXidOffset, 88);
    writeLe32(apfs, kTestApfsSpacemanOffset + kTestApfsObjectTypeOffset, 0x80'00'00'05);
    writeLe32(apfs, kTestApfsSpacemanOffset + kTestApfsSpacemanBlockSizeOffset, kTestApfsBlockSize);
    writeLe32(apfs, kTestApfsSpacemanOffset + kTestApfsSpacemanBlocksPerChunkOffset, 32'768);
    writeLe32(apfs, kTestApfsSpacemanOffset + kTestApfsSpacemanChunksPerCibOffset, 126);
    writeLe64(apfs,
              kTestApfsSpacemanOffset + kTestApfsSpacemanMainDeviceOffset +
                  kTestApfsSpacemanDeviceBlockCountOffset,
              4096);
    writeLe64(apfs,
              kTestApfsSpacemanOffset + kTestApfsSpacemanMainDeviceOffset +
                  kTestApfsSpacemanDeviceChunkCountOffset,
              1);
    writeLe32(apfs,
              kTestApfsSpacemanOffset + kTestApfsSpacemanMainDeviceOffset +
                  kTestApfsSpacemanDeviceCibCountOffset,
              1);
    writeLe64(apfs,
              kTestApfsSpacemanOffset + kTestApfsSpacemanMainDeviceOffset +
                  kTestApfsSpacemanDeviceFreeCountOffset,
              512);
    writeLe64(apfs, kTestApfsObjectMapOffset + kTestApfsObjectOidOffset, 2002);
    writeLe64(apfs, kTestApfsObjectMapOffset + kTestApfsObjectXidOffset, 88);
    writeLe32(apfs, kTestApfsObjectMapOffset + kTestApfsObjectTypeOffset, 0x00'00'00'0B);
    writeLe32(apfs, kTestApfsObjectMapOffset + kTestApfsOmapFlagsOffset, 0x1);
    writeLe32(apfs, kTestApfsObjectMapOffset + kTestApfsOmapSnapshotCountOffset, 2);
    writeLe64(apfs, kTestApfsObjectMapOffset + kTestApfsOmapTreeOidOffset, 5001);
    writeLe64(apfs, kTestApfsObjectMapOffset + kTestApfsOmapSnapshotTreeOidOffset, 5002);
    writeLe64(apfs, kTestApfsObjectMapOffset + kTestApfsOmapMostRecentSnapshotOffset, 99);
    writeLe64(apfs, kTestApfsObjectMapOffset + kTestApfsOmapPendingRevertMinOffset, 77);
    writeLe64(apfs, kTestApfsObjectMapOffset + kTestApfsOmapPendingRevertMaxOffset, 88);
}

void writeApfsVolumeAndBtrees(QByteArray* apfs) {
    writeAscii(apfs, kTestApfsVolumeCandidateOffset + kTestApfsVolumeMagicOffset, "APSB");
    writeLe64(apfs, kTestApfsVolumeCandidateOffset + kTestApfsObjectOidOffset, 3002);
    writeLe64(apfs, kTestApfsVolumeCandidateOffset + kTestApfsObjectXidOffset, 90);
    writeLe32(apfs, kTestApfsVolumeCandidateOffset + kTestApfsObjectTypeOffset, 0x00'00'00'0D);
    writeLe32(apfs, kTestApfsVolumeCandidateOffset + kTestApfsVolumeIndexOffset, 2);
    writeLe64(apfs, kTestApfsVolumeCandidateOffset + kTestApfsVolumeReserveBlockCountOffset, 100);
    writeLe64(apfs, kTestApfsVolumeCandidateOffset + kTestApfsVolumeQuotaBlockCountOffset, 200);
    writeLe64(apfs,
              kTestApfsVolumeCandidateOffset + kTestApfsVolumeAllocatedBlockCountOffset,
              12'345);
    writeLe64(apfs, kTestApfsVolumeCandidateOffset + kTestApfsVolumeObjectMapOidOffset, 4002);
    writeLe64(apfs, kTestApfsVolumeCandidateOffset + kTestApfsVolumeRootTreeOidOffset, 4003);
    writeLe64(apfs, kTestApfsVolumeCandidateOffset + kTestApfsVolumeExtentrefTreeOidOffset, 4004);
    writeLe64(apfs,
              kTestApfsVolumeCandidateOffset + kTestApfsVolumeSnapshotMetadataTreeOidOffset,
              4005);
    writeRaw(apfs,
             kTestApfsVolumeCandidateOffset + kTestApfsVolumeUuidOffset,
             QByteArray::fromHex("0102030405060708090a0b0c0d0e0f10"));
    writeAscii(apfs, kTestApfsVolumeCandidateOffset + kTestApfsVolumeNameOffset, "SAK_APFS_VOL");
    writeLe16(apfs, kTestApfsVolumeCandidateOffset + kTestApfsVolumeRoleOffset, 4);
    writeLe64(apfs, kTestApfsRootTreeOffset + kTestApfsObjectOidOffset, 4003);
    writeLe64(apfs, kTestApfsRootTreeOffset + kTestApfsObjectXidOffset, 91);
    writeLe32(apfs, kTestApfsRootTreeOffset + kTestApfsObjectTypeOffset, 0x00'00'00'02);
    writeLe32(apfs, kTestApfsRootTreeOffset + kTestApfsObjectSubtypeOffset, 0x00'00'00'0E);
    writeLe16(apfs, kTestApfsRootTreeOffset + kTestApfsBtreeNodeFlagsOffset, 0x0003);
    writeLe32(apfs, kTestApfsRootTreeOffset + kTestApfsBtreeNodeKeyCountOffset, 4);
    writeLe64(apfs, kTestApfsObjectMapTreeOffset + kTestApfsObjectOidOffset, 5001);
    writeLe64(apfs, kTestApfsObjectMapTreeOffset + kTestApfsObjectXidOffset, 92);
    writeLe32(apfs, kTestApfsObjectMapTreeOffset + kTestApfsObjectTypeOffset, 0x00'00'00'02);
    writeLe16(apfs, kTestApfsObjectMapTreeOffset + kTestApfsBtreeNodeFlagsOffset, 0x0005);
    writeLe16(apfs, kTestApfsObjectMapTreeOffset + kTestApfsBtreeNodeLevelOffset, 1);
    writeLe32(apfs, kTestApfsObjectMapTreeOffset + kTestApfsBtreeNodeKeyCountOffset, 2);
}

void writeApfsBtreeInfo(QByteArray* apfs) {
    const qsizetype rootInfo = kTestApfsRootTreeOffset + kTestApfsBlockSize -
                               kTestApfsBtreeInfoSize;
    writeLe32(apfs, rootInfo + kTestApfsBtreeInfoNodeSizeOffset, 4096);
    writeLe32(apfs, rootInfo + kTestApfsBtreeInfoLongestKeyOffset, 32);
    writeLe32(apfs, rootInfo + kTestApfsBtreeInfoLongestValueOffset, 128);
    writeLe64(apfs, rootInfo + kTestApfsBtreeInfoKeyCountOffset, 4);
    writeLe64(apfs, rootInfo + kTestApfsBtreeInfoNodeCountOffset, 1);
    const qsizetype objectMapInfo = kTestApfsObjectMapTreeOffset + kTestApfsBlockSize -
                                    kTestApfsBtreeInfoSize;
    writeLe16(apfs, kTestApfsObjectMapTreeOffset + kTestApfsBtreeNodeKeyFreeListOffsetOffset, 8);
    writeLe16(apfs, kTestApfsObjectMapTreeOffset + kTestApfsBtreeNodeKeyFreeListLengthOffset, 4);
    writeLe16(apfs, kTestApfsObjectMapTreeOffset + kTestApfsBtreeNodeValueFreeListOffsetOffset, 12);
    writeLe16(apfs, kTestApfsObjectMapTreeOffset + kTestApfsBtreeNodeValueFreeListLengthOffset, 6);
    writeLe32(apfs, objectMapInfo + kTestApfsBtreeInfoFlagsOffset, 0x10);
    writeLe32(apfs, objectMapInfo + kTestApfsBtreeInfoNodeSizeOffset, 4096);
    writeLe32(apfs, objectMapInfo + kTestApfsBtreeInfoKeySizeOffset, 16);
    writeLe32(apfs, objectMapInfo + kTestApfsBtreeInfoValueSizeOffset, 16);
    writeLe64(apfs, objectMapInfo + kTestApfsBtreeInfoNodeCountOffset, 3);
}

QByteArray apfsRawDetectionFixture() {
    auto apfs = signatureFixture();
    writeApfsContainerFixture(&apfs);
    writeApfsReferencedObjects(&apfs);
    writeApfsVolumeAndBtrees(&apfs);
    writeApfsBtreeInfo(&apfs);
    return apfs;
}

bool stampApfsObjectBlock(QByteArray* apfs, qsizetype blockIndex) {
    if (!apfs || blockIndex < 0) {
        return false;
    }
    const qsizetype blockOffset = blockIndex * kTestApfsBlockSize;
    if (blockOffset < 0 || blockOffset + kTestApfsBlockSize > apfs->size()) {
        return false;
    }

    QByteArray block = apfs->mid(blockOffset, kTestApfsBlockSize);
    if (!PartitionApfsWriter::stampObjectChecksum(&block)) {
        return false;
    }
    std::copy(block.cbegin(), block.cend(), apfs->begin() + blockOffset);
    return true;
}

bool rewriteApfsImageObjectField64(const QString& imagePath,
                                   qsizetype blockIndex,
                                   qsizetype fieldOffset,
                                   uint64_t value) {
    QFile image(imagePath);
    if (!image.open(QIODevice::ReadWrite)) {
        return false;
    }
    const qint64 blockOffset = static_cast<qint64>(blockIndex * kTestApfsBlockSize);
    if (!image.seek(blockOffset)) {
        return false;
    }
    QByteArray block = image.read(kTestApfsBlockSize);
    if (block.size() != static_cast<qsizetype>(kTestApfsBlockSize)) {
        return false;
    }
    writeLe64(&block, fieldOffset, value);
    if (!PartitionApfsWriter::stampObjectChecksum(&block)) {
        return false;
    }
    if (!image.seek(blockOffset)) {
        return false;
    }
    return image.write(block) == block.size();
}

QByteArray apfsFarCheckpointSpaceManagerFixture() {
    constexpr qsizetype farCheckpointDataBaseBlock = 544;
    constexpr qsizetype farSpacemanBlock = farCheckpointDataBaseBlock + 1;
    constexpr uint64_t freeBlocks = 777;
    QByteArray image((farSpacemanBlock + 1) * kTestApfsBlockSize, '\0');
    writeApfsContainerFixture(&image);
    writeLe32(&image, kTestApfsCheckpointDataBlocksOffset, 64);
    writeLe64(&image, kTestApfsCheckpointDataBaseOffset, farCheckpointDataBaseBlock);
    writeLe32(&image, kTestApfsCheckpointDataIndexOffset, 0);
    writeLe32(&image, kTestApfsCheckpointDataLengthOffset, 4);
    const qsizetype spacemanOffset = farSpacemanBlock * kTestApfsBlockSize;
    writeLe64(&image, spacemanOffset + kTestApfsObjectOidOffset, 2001);
    writeLe64(&image, spacemanOffset + kTestApfsObjectXidOffset, 88);
    writeLe32(&image, spacemanOffset + kTestApfsObjectTypeOffset, 0x80'00'00'05);
    writeLe32(&image, spacemanOffset + kTestApfsSpacemanBlockSizeOffset, kTestApfsBlockSize);
    writeLe32(&image, spacemanOffset + kTestApfsSpacemanBlocksPerChunkOffset, 32'768);
    writeLe32(&image, spacemanOffset + kTestApfsSpacemanChunksPerCibOffset, 126);
    writeLe64(&image,
              spacemanOffset + kTestApfsSpacemanMainDeviceOffset +
                  kTestApfsSpacemanDeviceBlockCountOffset,
              4096);
    writeLe64(&image,
              spacemanOffset + kTestApfsSpacemanMainDeviceOffset +
                  kTestApfsSpacemanDeviceChunkCountOffset,
              1);
    writeLe32(&image,
              spacemanOffset + kTestApfsSpacemanMainDeviceOffset +
                  kTestApfsSpacemanDeviceCibCountOffset,
              1);
    writeLe64(&image,
              spacemanOffset + kTestApfsSpacemanMainDeviceOffset +
                  kTestApfsSpacemanDeviceFreeCountOffset,
              freeBlocks);
    return image;
}

void expectDetailsContain(const QString& details, const QStringList& needles) {
    for (const auto& needle : needles) {
        QVERIFY2(details.contains(needle), qPrintable(needle));
    }
}

void verifyApfsContainerDetails(const QString& details) {
    expectDetailsContain(details,
                         {QStringLiteral("Block size: 4096"),
                          QStringLiteral("Container UUID: 11223344-5566-7788-9900-aabbccddeeff"),
                          QStringLiteral("Features: 0x0000000000000002"),
                          QStringLiteral("Read-only compatible features: 0x0000000000000004"),
                          QStringLiteral("Incompatible features: 0x0000000000000100"),
                          QStringLiteral("Next object ID: 77"),
                          QStringLiteral("Next transaction ID: 88"),
                          QStringLiteral("Max file systems: 100")});
}

void verifyApfsCheckpointDetails(const QString& details) {
    expectDetailsContain(details,
                         {QStringLiteral("Checkpoint descriptor blocks: 4"),
                          QStringLiteral("Checkpoint data blocks: 8"),
                          QStringLiteral("Checkpoint descriptor base block: 128"),
                          QStringLiteral("Checkpoint data base block: 256"),
                          QStringLiteral("Checkpoint descriptor next index: 1"),
                          QStringLiteral("Checkpoint data next index: 2"),
                          QStringLiteral("Checkpoint descriptor start index: 3"),
                          QStringLiteral("Checkpoint data start index: 6"),
                          QStringLiteral("Checkpoint descriptor length: 3"),
                          QStringLiteral("Checkpoint data length: 7")});
}

void verifyApfsObjectDetails(const QString& details) {
    expectDetailsContain(details,
                         {QStringLiteral("Space manager OID: 2001"),
                          QStringLiteral("Object map OID: 2002"),
                          QStringLiteral("Reaper OID: 2003"),
                          QStringLiteral("Volume OID slots used: 2"),
                          QStringLiteral("Volume OIDs: 2:3002, 5:3005")});
}

void verifyApfsSpaceManagerDetails(const QString& details) {
    expectDetailsContain(details,
                         {QStringLiteral("APFS space manager block: 15"),
                          QStringLiteral("APFS space manager OID: 2001"),
                          QStringLiteral("APFS space manager block size: 4096"),
                          QStringLiteral("APFS space manager main blocks: 4096"),
                          QStringLiteral("APFS space manager free blocks: 512"),
                          QStringLiteral("APFS free bytes: 2097152")});
}

void verifyApfsVolumeCandidateDetails(const QString& details) {
    expectDetailsContain(details,
                         {QStringLiteral("APFS volume superblock candidates in probe window: 1"),
                          QStringLiteral("APFS volume candidate block 16"),
                          QStringLiteral("index 2"),
                          QStringLiteral("name SAK_APFS_VOL"),
                          QStringLiteral("uuid 01020304-0506-0708-090a-0b0c0d0e0f10"),
                          QStringLiteral("role 4"),
                          QStringLiteral("reserve blocks 100"),
                          QStringLiteral("quota blocks 200"),
                          QStringLiteral("allocated blocks 12345"),
                          QStringLiteral("volume object map OID 4002"),
                          QStringLiteral("root tree OID 4003"),
                          QStringLiteral("extentref tree OID 4004"),
                          QStringLiteral("snapshot metadata tree OID 4005")});
}

void verifyApfsReferencedObjectDetails(const QString& details) {
    expectDetailsContain(details,
                         {QStringLiteral("APFS referenced object headers in probe window: 5"),
                          QStringLiteral("APFS referenced object header block 15"),
                          QStringLiteral("labels container space manager OID"),
                          QStringLiteral("object type 0x80000005"),
                          QStringLiteral("APFS referenced object header block 16"),
                          QStringLiteral("labels volume OID slot 2"),
                          QStringLiteral("APFS referenced object header block 17"),
                          QStringLiteral("labels container object map OID"),
                          QStringLiteral("object type 0x0000000b"),
                          QStringLiteral("APFS referenced object header block 18"),
                          QStringLiteral("volume candidate block 16 root tree OID"),
                          QStringLiteral("APFS referenced object header block 19"),
                          QStringLiteral("object map block 17 tree OID")});
}

void verifyApfsObjectMapDetails(const QString& details) {
    expectDetailsContain(details,
                         {QStringLiteral("APFS object map details in probe window: 1"),
                          QStringLiteral("APFS object map detail block 17"),
                          QStringLiteral("flags 0x00000001"),
                          QStringLiteral("snapshots 2"),
                          QStringLiteral("tree OID 5001"),
                          QStringLiteral("snapshot tree OID 5002"),
                          QStringLiteral("most recent snapshot XID 99"),
                          QStringLiteral("pending revert min XID 77"),
                          QStringLiteral("pending revert max XID 88")});
}

void verifyApfsBtreeDetails(const QString& details) {
    expectDetailsContain(details,
                         {QStringLiteral("APFS B-tree node details in probe window: 2"),
                          QStringLiteral("APFS B-tree node detail block 18"),
                          QStringLiteral("OID 4003"),
                          QStringLiteral("object subtype 0x0000000e"),
                          QStringLiteral("flags 0x0003"),
                          QStringLiteral("flag names root/leaf"),
                          QStringLiteral("keys 4"),
                          QStringLiteral("tree node size 4096"),
                          QStringLiteral("tree longest key 32"),
                          QStringLiteral("tree longest value 128"),
                          QStringLiteral("tree key count 4"),
                          QStringLiteral("tree node count 1"),
                          QStringLiteral("APFS B-tree node detail block 19"),
                          QStringLiteral("labels object map block 17 tree OID"),
                          QStringLiteral("flag names root/fixed-kv"),
                          QStringLiteral("level 1"),
                          QStringLiteral("key free list 8:4"),
                          QStringLiteral("value free list 12:6"),
                          QStringLiteral("tree flags 0x00000010"),
                          QStringLiteral("tree key size 16"),
                          QStringLiteral("tree value size 16"),
                          QStringLiteral("tree node count 3")});
}

void verifyApfsRawDetection() {
    const auto apfs = apfsRawDetectionFixture();
    const auto detection = PartitionFileSystemDetector::detectBytes(apfs, apfs.size());
    QVERIFY(detection.has_value());
    QCOMPARE(detection->file_system, QStringLiteral("APFS"));
    QCOMPARE(detection->total_bytes, 4096ULL * kTestApfsBlockSize);
    QCOMPARE(detection->free_bytes, 4096ULL * 512ULL);
    const QString details = detection->details.join(' ');
    verifyApfsContainerDetails(details);
    verifyApfsCheckpointDetails(details);
    verifyApfsObjectDetails(details);
    verifyApfsSpaceManagerDetails(details);
    verifyApfsVolumeCandidateDetails(details);
    verifyApfsReferencedObjectDetails(details);
    verifyApfsObjectMapDetails(details);
    verifyApfsBtreeDetails(details);
    expectDetailsContain(
        details,
        {QStringLiteral(
            "Metadata sanity: APFS container block geometry is internally consistent")});
}

void verifyLinuxSwapRawDetection() {
    auto swap = signatureFixture();
    writeAscii(&swap, kTestSwapSignatureOffset, "SWAPSPACE2");
    writeLe32(&swap, kTestSwapVersionOffset, 1);
    writeLe32(&swap, kTestSwapLastPageOffset, 255);
    writeLe32(&swap, kTestSwapBadPagesOffset, 2);
    const QByteArray uuid = QByteArray::fromHex("00112233445566778899aabbccddeeff");
    std::copy(uuid.cbegin(), uuid.cend(), swap.begin() + kTestSwapUuidOffset);
    writeAscii(&swap, kTestSwapLabelOffset, "SAK_SWAP");
    const auto detection = PartitionFileSystemDetector::detectBytes(swap, swap.size());
    QVERIFY(detection.has_value());
    QCOMPARE(detection->file_system, QStringLiteral("Linux swap"));
    QCOMPARE(detection->total_bytes, 4096ULL * 256ULL);
    const QString details = detection->details.join(' ');
    expectDetailsContain(details,
                         {QStringLiteral("Signature: SWAPSPACE2"),
                          QStringLiteral("Header version: 1"),
                          QStringLiteral("Bad pages: 2"),
                          QStringLiteral("Volume label: SAK_SWAP"),
                          QStringLiteral("UUID: 00112233-4455-6677-8899-aabbccddeeff")});
}

void verifyNativeRegistryCapabilities() {
    const auto ntfs = PartitionFileSystemRegistry::capabilityFor(QStringLiteral("NTFS"));
    QCOMPARE(ntfs.id, QStringLiteral("ntfs"));
    QVERIFY(!ntfs.non_native);
    QVERIFY(ntfs.support_level.contains(QStringLiteral("Windows-native")));
    const auto fat16 = PartitionFileSystemRegistry::capabilityFor(QStringLiteral("FAT16"));
    QCOMPARE(fat16.id, QStringLiteral("fat16"));
    QVERIFY(!fat16.non_native);
    const auto fat12 = PartitionFileSystemRegistry::capabilityFor(QStringLiteral("FAT12"));
    QCOMPARE(fat12.id, QStringLiteral("fat12"));
    QVERIFY(!fat12.non_native);
}

void verifyExtRegistryCapability() {
    const auto ext4 = PartitionFileSystemRegistry::capabilityFor(QStringLiteral("ext4"));
    QCOMPARE(ext4.id, QStringLiteral("ext4"));
    QVERIFY(ext4.non_native);
    QVERIFY(ext4.required_tools.contains(QStringLiteral("e2fsck")));
    QVERIFY(ext4.required_tools.contains(QStringLiteral("mke2fs")));
    QVERIFY(ext4.required_tools.contains(QStringLiteral("resize2fs")));
    const QString available = PartitionFileSystemRegistry::actionSummary(ext4.available_actions);
    QVERIFY(available.contains(QStringLiteral("Read-only e2fsck check")));
    QVERIFY(available.contains(QStringLiteral("Confirmed ext format")));
    QVERIFY(available.contains(QStringLiteral("Read-only directory browse")));
    QVERIFY(available.contains(QStringLiteral("recursive directory export")));
    QVERIFY(available.contains(QStringLiteral("Confirmed ext shrink")));
    QVERIFY(PartitionFileSystemRegistry::actionSummary(ext4.blocked_actions)
                .contains(QStringLiteral("XFS/Btrfs/APFS write workflows")));
}

void verifyMetadataOnlyRegistryCapabilities() {
    const auto xfs = PartitionFileSystemRegistry::capabilityFor(QStringLiteral("XFS"));
    QCOMPARE(xfs.id, QStringLiteral("xfs"));
    QVERIFY(xfs.support_level.contains(QStringLiteral("metadata checks")));
    QVERIFY(PartitionFileSystemRegistry::actionSummary(xfs.available_actions)
                .contains(QStringLiteral("metadata consistency check")));
    QVERIFY(PartitionFileSystemRegistry::actionSummary(xfs.blocked_actions)
                .contains(QStringLiteral("Deep xfs_repair check")));

    const auto btrfs = PartitionFileSystemRegistry::capabilityFor(QStringLiteral("Btrfs"));
    QCOMPARE(btrfs.id, QStringLiteral("btrfs"));
    QVERIFY(btrfs.support_level.contains(QStringLiteral("metadata checks")));
    QVERIFY(PartitionFileSystemRegistry::actionSummary(btrfs.available_actions)
                .contains(QStringLiteral("metadata consistency check")));
    QVERIFY(PartitionFileSystemRegistry::actionSummary(btrfs.blocked_actions)
                .contains(QStringLiteral("Deep btrfs check")));
}

void verifyApfsRegistryCapability() {
    const auto apfs = PartitionFileSystemRegistry::capabilityFor(QStringLiteral("APFS"));
    QCOMPARE(apfs.id, QStringLiteral("apfs"));
    QVERIFY(apfs.support_level.contains(QStringLiteral("generated APFS format/repair")));
    const QString available = PartitionFileSystemRegistry::actionSummary(apfs.available_actions);
    expectDetailsContain(available,
                         {QStringLiteral("volume-OID"),
                          QStringLiteral("checkpoint ring"),
                          QStringLiteral("volume-superblock candidate"),
                          QStringLiteral("object-map"),
                          QStringLiteral("container metadata consistency check"),
                          QStringLiteral("volume browse"),
                          QStringLiteral("selected-file extraction"),
                          QStringLiteral("bounded recursive export"),
                          QStringLiteral("generated APFS create"),
                          QStringLiteral("generated APFS metadata-checksum repair")});
    // A1/A2 promotion: multi-CIB + CAB-tier format/repair confirmed; in-place write
    // still single-chunk.
    QVERIFY(available.contains(QStringLiteral("multi-CIB")));
    QVERIFY(available.contains(QStringLiteral("CAB-tier")));
    QVERIFY(available.contains(QStringLiteral("24 TiB")));
    const QString blocked = PartitionFileSystemRegistry::actionSummary(apfs.blocked_actions);
    QVERIFY(blocked.contains(QStringLiteral("Arbitrary existing Apple APFS mutation")));
    QVERIFY(blocked.contains(QStringLiteral("Encrypted/compressed files")));
    QVERIFY(blocked.contains(QStringLiteral("single spaceman chunk")));
    QVERIFY(PartitionFileSystemRegistry::actionSummary(apfs.required_tools)
                .contains(QStringLiteral("sak_apfs_writer_cli")));
}

void verifyHfsRegistryCapability() {
    const auto hfs = PartitionFileSystemRegistry::capabilityFor(QStringLiteral("HFS+"));
    QCOMPARE(hfs.id, QStringLiteral("hfsplus"));
    QVERIFY(hfs.support_level.contains(QStringLiteral("bundled format/repair")));
    const QString available = PartitionFileSystemRegistry::actionSummary(hfs.available_actions);
    expectDetailsContain(available,
                         {QStringLiteral("Read-only catalog browse"),
                          QStringLiteral("catalog consistency check"),
                          QStringLiteral("attributes B-tree key scan"),
                          QStringLiteral("selected attribute value extract"),
                          QStringLiteral("extents overflow"),
                          QStringLiteral("resource-fork"),
                          QStringLiteral("resource-fork sidecars"),
                          QStringLiteral("same-size HFS+ data-fork overwrite"),
                          QStringLiteral("Staged raw-partition HFS+ data/resource-fork"),
                          QStringLiteral("bounded data/resource-fork allocation growth"),
                          QStringLiteral("Staged raw-partition HFS+ empty-file create/delete"),
                          QStringLiteral("bounded file create"),
                          QStringLiteral("Staged raw-partition HFS+ allocated-file delete"),
                          QStringLiteral("optional released-block zeroing"),
                          QStringLiteral("Staged raw-partition HFS+ empty-folder create/delete"),
                          QStringLiteral("Staged raw-partition HFS+ bounded folder-tree delete"),
                          QStringLiteral("Staged raw-partition HFS+ inline attribute"),
                          QStringLiteral("Staged raw-partition HFS+ fork-backed attribute"),
                          QStringLiteral("bounded fork-backed attribute allocation growth"),
                          QStringLiteral("bundled newfs_hfs"),
                          QStringLiteral("bundled fsck_hfs")});
    const QString confirmed = PartitionFileSystemRegistry::actionSummary(hfs.available_actions);
    QVERIFY(confirmed.contains(QStringLiteral("arbitrary-depth catalog B-tree split")));
    const QString blocked = PartitionFileSystemRegistry::actionSummary(hfs.blocked_actions);
    QVERIFY(blocked.contains(QStringLiteral("Raw-partition HFS+ complex file delete")));
    QVERIFY(blocked.contains(QStringLiteral("unbounded folder-tree delete")));
    QVERIFY(blocked.contains(QStringLiteral("complex file delete")));
    QVERIFY(blocked.contains(QStringLiteral("broad allocation growth")));
    QVERIFY(blocked.contains(QStringLiteral("Inline/broad attribute growth")));
    // H2 promotion: catalog B-tree split/rebalance is no longer a blocked action.
    QVERIFY(!blocked.contains(QStringLiteral("B-tree split/rebalance")));
    const QString requiredTools = PartitionFileSystemRegistry::actionSummary(hfs.required_tools);
    QVERIFY(requiredTools.contains(QStringLiteral("newfs_hfs")));
    QVERIFY(requiredTools.contains(QStringLiteral("fsck_hfs")));
    QVERIFY(requiredTools.contains(QStringLiteral("sak_hfs_writer_cli")));
}

void verifySwapAndUnknownRegistryCapability() {
    const auto swap = PartitionFileSystemRegistry::capabilityFor(QStringLiteral("Linux swap"));
    QCOMPARE(swap.id, QStringLiteral("linux-swap"));
    QVERIFY(swap.support_level.contains(QStringLiteral("header metadata")));
    QVERIFY(swap.support_level.contains(QStringLiteral("confirmed format")));
    const QString available = PartitionFileSystemRegistry::actionSummary(swap.available_actions);
    QVERIFY(available.contains(QStringLiteral("swap header metadata inspection")));
    QVERIFY(available.contains(QStringLiteral("Confirmed Linux swap format")));
    QVERIFY(PartitionFileSystemRegistry::actionSummary(swap.blocked_actions)
                .contains(QStringLiteral("Repair is not applicable")));

    const auto unknown = PartitionFileSystemRegistry::capabilityFor(QString());
    QCOMPARE(unknown.id, QStringLiteral("unknown"));
    QVERIFY(PartitionFileSystemRegistry::actionSummary(unknown.blocked_actions)
                .contains(QStringLiteral("must be identified")));
}

QString sha256Hex(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QByteArray approvedToolManifestJson(const QString& sourceHash,
                                    const QString& binaryHash,
                                    const QString& runtimeHash) {
    const QJsonObject runtime{{QStringLiteral("relative_path"),
                               QStringLiteral("e2fsprogs/cygfake1.dll")},
                              {QStringLiteral("sha256"), runtimeHash}};
    const QJsonObject tool{
        {QStringLiteral("id"), QStringLiteral("e2fsck")},
        {QStringLiteral("display_name"), QStringLiteral("e2fsck")},
        {QStringLiteral("version"), QStringLiteral("1.47.3")},
        {QStringLiteral("upstream_url"), QStringLiteral("https://e2fsprogs.sourceforge.net/")},
        {QStringLiteral("license"), QStringLiteral("GPL-2.0-only")},
        {QStringLiteral("source_archive_sha256"), sourceHash},
        {QStringLiteral("relative_path"), QStringLiteral("e2fsprogs/e2fsck.exe")},
        {QStringLiteral("binary_sha256"), binaryHash},
        {QStringLiteral("file_systems"),
         QJsonArray{QStringLiteral("ext2"), QStringLiteral("ext3"), QStringLiteral("ext4")}},
        {QStringLiteral("operations"),
         QJsonArray{QStringLiteral("check"), QStringLiteral("repair")}},
        {QStringLiteral("runtime_files"), QJsonArray{runtime}}};
    const QJsonObject manifest{{QStringLiteral("schema_version"), 1},
                               {QStringLiteral("tools"), QJsonArray{tool}}};
    return QJsonDocument(manifest).toJson(QJsonDocument::Compact);
}

QByteArray invalidToolManifestJson(const QString& validHash) {
    const QJsonObject badRuntime{{QStringLiteral("relative_path"), QStringLiteral("../escape.dll")},
                                 {QStringLiteral("sha256"), validHash}};
    const QJsonObject badRuntimeHash{{QStringLiteral("relative_path"), QStringLiteral("bad.exe")},
                                     {QStringLiteral("sha256"), QStringLiteral("not-a-sha")}};
    const QJsonObject badHash{
        {QStringLiteral("id"), QStringLiteral("bad-hash")},
        {QStringLiteral("display_name"), QStringLiteral("Bad Hash")},
        {QStringLiteral("version"), QStringLiteral("1.0")},
        {QStringLiteral("upstream_url"), QStringLiteral("https://example.invalid/tool")},
        {QStringLiteral("license"), QString()},
        {QStringLiteral("source_archive_sha256"), QStringLiteral("not-a-sha")},
        {QStringLiteral("relative_path"), QStringLiteral("bad.exe")},
        {QStringLiteral("binary_sha256"), validHash},
        {QStringLiteral("file_systems"), QJsonArray{QStringLiteral("ext4")}},
        {QStringLiteral("operations"), QJsonArray{QStringLiteral("repair")}},
        {QStringLiteral("runtime_files"), QJsonArray{badRuntime, badRuntimeHash}}};
    const QJsonObject escape{{QStringLiteral("id"), QStringLiteral("escape")},
                             {QStringLiteral("display_name"), QStringLiteral("Escaping Tool")},
                             {QStringLiteral("version"), QStringLiteral("1.0")},
                             {QStringLiteral("upstream_url"),
                              QStringLiteral("https://example.invalid/escape")},
                             {QStringLiteral("license"), QStringLiteral("GPL-2.0-only")},
                             {QStringLiteral("source_archive_sha256"), validHash},
                             {QStringLiteral("relative_path"), QStringLiteral("../escape.exe")},
                             {QStringLiteral("binary_sha256"), validHash},
                             {QStringLiteral("file_systems"), QJsonArray{QStringLiteral("xfs")}},
                             {QStringLiteral("operations"), QJsonArray{QStringLiteral("repair")}}};
    const QJsonObject manifest{{QStringLiteral("schema_version"), 1},
                               {QStringLiteral("tools"), QJsonArray{badHash, escape}}};
    return QJsonDocument(manifest).toJson(QJsonDocument::Compact);
}

QByteArray emptyToolManifestJson() {
    return QJsonDocument(QJsonObject{{QStringLiteral("schema_version"), 1},
                                     {QStringLiteral("tools"), QJsonArray()}})
        .toJson(QJsonDocument::Compact);
}

void verifyExtFormatCommandShape() {
    const auto format =
        PartitionFileSystemToolRunner::buildFormatCommand(QStringLiteral("ext4"),
                                                          QStringLiteral("target.img"),
                                                          QStringLiteral("TechnicianData"),
                                                          true);
    QVERIFY2(format.ok(), qPrintable(format.blockers.join(QStringLiteral("; "))));
    QCOMPARE(format.tool_id, QStringLiteral("mke2fs"));
    QCOMPARE(format.operation, PartitionFileSystemToolRunner::formatOperation());
    QCOMPARE(format.arguments,
             QStringList({QStringLiteral("-t"),
                          QStringLiteral("ext4"),
                          QStringLiteral("-F"),
                          QStringLiteral("-L"),
                          QStringLiteral("TechnicianData"),
                          QStringLiteral("target.img")}));
}

void verifyExtRepairAndResizeCommandShapes() {
    const auto repair = PartitionFileSystemToolRunner::buildRepairCommand(
        QStringLiteral("ext3"), QStringLiteral("target.img"), true);
    QVERIFY2(repair.ok(), qPrintable(repair.blockers.join(QStringLiteral("; "))));
    QCOMPARE(repair.tool_id, QStringLiteral("e2fsck"));
    QCOMPARE(repair.operation, PartitionFileSystemToolRunner::repairOperation());
    QCOMPARE(repair.arguments,
             QStringList(
                 {QStringLiteral("-p"), QStringLiteral("-f"), QStringLiteral("target.img")}));

    const auto resize = PartitionFileSystemToolRunner::buildResizeCommand(
        QStringLiteral("ext2"), QStringLiteral("target.img"), true);
    QVERIFY2(resize.ok(), qPrintable(resize.blockers.join(QStringLiteral("; "))));
    QCOMPARE(resize.tool_id, QStringLiteral("resize2fs"));
    QCOMPARE(resize.operation, PartitionFileSystemToolRunner::resizeOperation());
    QCOMPARE(resize.arguments, QStringList({QStringLiteral("target.img")}));
}

void verifyUnconfirmedExtFormatBlocked() {
    const auto unconfirmed = PartitionFileSystemToolRunner::buildFormatCommand(
        QStringLiteral("ext4"), QStringLiteral("target.img"), QString(), false);
    QVERIFY(!unconfirmed.ok());
    QVERIFY(unconfirmed.blockers.join(' ').contains(QStringLiteral("requires confirmation")));
}

void verifyHfsCommandShapes() {
    const auto hfsFormat = PartitionFileSystemToolRunner::buildFormatCommand(
        QStringLiteral("HFS+"), QStringLiteral("target.img"), QStringLiteral("MacData"), true);
    QVERIFY2(hfsFormat.ok(), qPrintable(hfsFormat.blockers.join(QStringLiteral("; "))));
    QCOMPARE(hfsFormat.tool_id, QStringLiteral("newfs_hfs"));
    QCOMPARE(hfsFormat.arguments,
             QStringList(
                 {QStringLiteral("-v"), QStringLiteral("MacData"), QStringLiteral("target.img")}));

    const auto hfsxFormat = PartitionFileSystemToolRunner::buildFormatCommand(
        QStringLiteral("HFSX"), QStringLiteral("target.img"), QString(), true);
    QVERIFY2(hfsxFormat.ok(), qPrintable(hfsxFormat.blockers.join(QStringLiteral("; "))));
    QCOMPARE(hfsxFormat.arguments,
             QStringList({QStringLiteral("-s"), QStringLiteral("target.img")}));

    const auto hfsRepair = PartitionFileSystemToolRunner::buildRepairCommand(
        QStringLiteral("hfs+"), QStringLiteral("target.img"), true);
    QVERIFY2(hfsRepair.ok(), qPrintable(hfsRepair.blockers.join(QStringLiteral("; "))));
    QCOMPARE(hfsRepair.tool_id, QStringLiteral("fsck_hfs"));
    QCOMPARE(hfsRepair.arguments,
             QStringList(
                 {QStringLiteral("-p"), QStringLiteral("-f"), QStringLiteral("target.img")}));
}

void verifyHfsRawTargetConversion() {
    const QString rawTarget = QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk2\\Partition1");
    const QString msysTarget = QStringLiteral("//?/GLOBALROOT/Device/Harddisk2/Partition1");
    const auto hfsRawCheck =
        PartitionFileSystemToolRunner::buildReadOnlyCheckCommand(QStringLiteral("HFS+"), rawTarget);
    QVERIFY2(hfsRawCheck.ok(), qPrintable(hfsRawCheck.blockers.join(QStringLiteral("; "))));
    QCOMPARE(hfsRawCheck.arguments.constLast(), msysTarget);

    const auto hfsRawFormat = PartitionFileSystemToolRunner::buildFormatCommand(
        QStringLiteral("HFS+"), rawTarget, QString(), true);
    QVERIFY2(hfsRawFormat.ok(), qPrintable(hfsRawFormat.blockers.join(QStringLiteral("; "))));
    QCOMPARE(hfsRawFormat.arguments.constLast(), msysTarget);

    const auto hfsRawRepair =
        PartitionFileSystemToolRunner::buildRepairCommand(QStringLiteral("HFS+"), rawTarget, true);
    QVERIFY2(hfsRawRepair.ok(), qPrintable(hfsRawRepair.blockers.join(QStringLiteral("; "))));
    QCOMPARE(hfsRawRepair.arguments.constLast(), msysTarget);
}

void verifyUnsupportedRepairBlocked() {
    const auto unsupported = PartitionFileSystemToolRunner::buildRepairCommand(
        QStringLiteral("xfs"), QStringLiteral("target.img"), true);
    QVERIFY(!unsupported.ok());
    QVERIFY(unsupported.blockers.join(' ').contains(QStringLiteral("No approved repair tool")));
}

PartitionTarget extToolPartitionTarget() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 2;
    target.partition_number = 1;
    target.size_bytes = 1024ULL * 1024ULL * 1024ULL;
    return target;
}

QString extToolRawTargetPath() {
    return QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk2\\Partition1");
}

QJsonObject baseExtToolPayload() {
    QJsonObject payload;
    payload[QStringLiteral("non_native_file_system_tool")] = true;
    payload[QStringLiteral("file_system")] = QStringLiteral("ext4");
    payload[QStringLiteral("label")] = QStringLiteral("SAK_EXT4");
    payload[QStringLiteral("target_path")] = extToolRawTargetPath();
    payload[QStringLiteral("target_wipe_confirmed")] = true;
    return payload;
}

QJsonObject baseApfsRootFileMutationPayload() {
    QJsonObject payload = baseExtToolPayload();
    payload[QStringLiteral("file_system")] = QStringLiteral("APFS");
    payload[QStringLiteral("apfs_generated_layout_confirmed")] = true;
    payload[QStringLiteral("apfs_root_file_name")] = QStringLiteral("sak-test.txt");
    payload[QStringLiteral("apfs_root_file_payload_text")] = QStringLiteral("hello from apply");
    payload[QStringLiteral("apfs_root_file_patch_offset_bytes")] = QStringLiteral("6");
    return payload;
}

QJsonObject baseHfsFileMutationPayload() {
    QJsonObject payload = baseExtToolPayload();
    payload[QStringLiteral("file_system")] = QStringLiteral("HFS+");
    payload[QStringLiteral("hfs_path")] = QStringLiteral("/hello.txt");
    payload[QStringLiteral("hfs_payload_text")] = QStringLiteral("hello from hfs apply");
    payload[QStringLiteral("hfs_allow_journaled_volume")] = true;
    payload[QStringLiteral("hfs_allow_wrapped_volume")] = true;
    payload[QStringLiteral("hfs_file_id")] = QStringLiteral("17");
    payload[QStringLiteral("hfs_attribute_name")] = QStringLiteral("com.apple.FinderInfo");
    return payload;
}

void verifyExtFormatScript(const PartitionScriptBuilder* builder, const PartitionTarget& target) {
    const auto script = builder->buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::Format, target, baseExtToolPayload()));
    QVERIFY2(script.valid(), qPrintable(script.blockers.join(QStringLiteral("; "))));
    QVERIFY(script.script.contains(QStringLiteral("Get-FileHash")));
    QVERIFY(script.script.contains(QStringLiteral("mke2fs")));
    QVERIFY(script.script.contains(QStringLiteral("Dismount-Volume")));
    QVERIFY(script.script.contains(QStringLiteral("'ext4'")));
    QVERIFY(script.script.contains(extToolRawTargetPath()));
}

void verifyHfsFormatAndRepairScripts(const PartitionScriptBuilder* builder,
                                     const PartitionTarget& target) {
    QJsonObject hfsFormatPayload = baseExtToolPayload();
    hfsFormatPayload[QStringLiteral("file_system")] = QStringLiteral("HFSX");
    hfsFormatPayload[QStringLiteral("label")] = QStringLiteral("SAK_HFSX");
    const auto hfsFormat = builder->buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::Format, target, hfsFormatPayload));
    QVERIFY2(hfsFormat.valid(), qPrintable(hfsFormat.blockers.join(QStringLiteral("; "))));
    QVERIFY(hfsFormat.script.contains(QStringLiteral("newfs_hfs")));
    QVERIFY(hfsFormat.script.contains(QStringLiteral("fsck_hfs")));
    QVERIFY(hfsFormat.script.contains(QStringLiteral("sparse staging")));
    QVERIFY(hfsFormat.script.contains(QStringLiteral("Copy-SakSparseImageToRawTarget")));
    QVERIFY(hfsFormat.script.contains(extToolRawTargetPath()));

    QJsonObject hfsRepairPayload = baseExtToolPayload();
    hfsRepairPayload[QStringLiteral("file_system")] = QStringLiteral("HFS+");
    const auto hfsRepair = builder->buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::CheckFileSystem, target, hfsRepairPayload));
    QVERIFY2(hfsRepair.valid(), qPrintable(hfsRepair.blockers.join(QStringLiteral("; "))));
    QVERIFY(hfsRepair.script.contains(QStringLiteral("fsck_hfs")));
    QVERIFY(hfsRepair.script.contains(QStringLiteral("Copy-SakRawTargetToImage")));
    QVERIFY(hfsRepair.script.contains(QStringLiteral("Copy-SakSparseImageToRawTarget")));
}

void verifyApfsFormatAndRepairScripts(const PartitionScriptBuilder* builder,
                                      const PartitionTarget& target) {
    PartitionTarget apfsTarget = target;
    apfsTarget.size_bytes = 128ULL * 1024ULL * 1024ULL;
    QJsonObject apfsPayload = baseExtToolPayload();
    apfsPayload[QStringLiteral("file_system")] = QStringLiteral("APFS");
    apfsPayload[QStringLiteral("label")] = QStringLiteral("SAK_APFS");
    const auto apfsFormat = builder->buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::Format, apfsTarget, apfsPayload));
    QVERIFY2(apfsFormat.valid(), qPrintable(apfsFormat.blockers.join(QStringLiteral("; "))));
    QVERIFY(apfsFormat.script.contains(QStringLiteral("sak_apfs_writer_cli.exe")));
    QVERIFY(apfsFormat.script.contains(QStringLiteral("format-raw")));
    QVERIFY(apfsFormat.script.contains(QStringLiteral("ui.apfs-generated-raw-format")));
    QVERIFY(apfsFormat.script.contains(extToolRawTargetPath()));

    const auto apfsRepair = builder->buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::CheckFileSystem, apfsTarget, apfsPayload));
    QVERIFY2(apfsRepair.valid(), qPrintable(apfsRepair.blockers.join(QStringLiteral("; "))));
    QVERIFY(apfsRepair.script.contains(QStringLiteral("sak_apfs_writer_cli.exe")));
    QVERIFY(apfsRepair.script.contains(QStringLiteral("repair-raw")));
    QVERIFY(apfsRepair.script.contains(QStringLiteral("ui.apfs-generated-raw-repair")));
    QVERIFY(apfsRepair.script.contains(extToolRawTargetPath()));

    // A1 promotion: multi-CIB generated format targets above one spaceman chunk (128 MiB)
    // are accepted through the production script route up to the certified ~24 TiB edge.
    PartitionTarget multiCibTarget = target;
    multiCibTarget.size_bytes = 256ULL * 1024ULL * 1024ULL;
    const auto multiCibFormat = builder->buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::Format, multiCibTarget, apfsPayload));
    QVERIFY2(multiCibFormat.valid(),
             qPrintable(multiCibFormat.blockers.join(QStringLiteral("; "))));
    QVERIFY(multiCibFormat.script.contains(QStringLiteral("format-raw")));

    // A2 CAB promotion: CAB-tier targets (>~7.8 TiB) up to the certified 24 TiB edge now
    // format through the production script route (Apple fsck_apfs + kernel-RW-mount certified).
    PartitionTarget cabTarget = target;
    cabTarget.size_bytes = 8ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
    const auto cabFormat = builder->buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::Format, cabTarget, apfsPayload));
    QVERIFY2(cabFormat.valid(), qPrintable(cabFormat.blockers.join(QStringLiteral("; "))));
    QVERIFY(cabFormat.script.contains(QStringLiteral("format-raw")));

    // Targets above the certified 24 TiB edge still remain blocked.
    PartitionTarget oversizedTarget = target;
    oversizedTarget.size_bytes = 30ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
    const auto oversizedFormat = builder->buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::Format, oversizedTarget, apfsPayload));
    QVERIFY(!oversizedFormat.valid());
    QVERIFY(oversizedFormat.blockers.join(' ').contains(QStringLiteral("24 TiB")));
}

void verifyExtRepairScript(const PartitionScriptBuilder* builder, const PartitionTarget& target) {
    const auto repair = builder->buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::CheckFileSystem, target, baseExtToolPayload()));
    QVERIFY2(repair.valid(), qPrintable(repair.blockers.join(QStringLiteral("; "))));
    QVERIFY(repair.script.contains(QStringLiteral("e2fsck")));
    QVERIFY(repair.script.contains(QStringLiteral("-AcceptedExitCodes @(0, 1)")));
    QVERIFY(!repair.script.contains(QStringLiteral("Repair-Volume -DriveLetter")));
}

void verifyExtResizeScripts(const PartitionScriptBuilder* builder, const PartitionTarget& target) {
    QJsonObject resizePayload = baseExtToolPayload();
    resizePayload[QStringLiteral("target_size_bytes")] =
        QString::number(2ULL * 1024ULL * 1024ULL * 1024ULL);
    const auto resize = builder->buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::Resize, target, resizePayload));
    QVERIFY2(resize.valid(), qPrintable(resize.blockers.join(QStringLiteral("; "))));
    QVERIFY(resize.script.contains(QStringLiteral("Resize-Partition -DiskNumber 2")));
    QVERIFY(resize.script.contains(QStringLiteral("resize2fs")));

    QJsonObject shrinkPayload = resizePayload;
    shrinkPayload[QStringLiteral("target_size_bytes")] =
        QString::number(512ULL * 1024ULL * 1024ULL);
    const auto shrink = builder->buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::Resize, target, shrinkPayload));
    QVERIFY2(shrink.valid(), qPrintable(shrink.blockers.join(QStringLiteral("; "))));
    QVERIFY(shrink.script.contains(QStringLiteral("e2fsck pre-shrink repair")));
    QVERIFY(shrink.script.contains(QStringLiteral("resize2fs shrink")));
    QVERIFY(shrink.script.contains(QStringLiteral("524288K")));
    QVERIFY(shrink.script.contains(QStringLiteral("Partition shrink did not reach target size")));
}

void verifyExtTargetPathGates(const PartitionScriptBuilder* builder,
                              const PartitionTarget& target) {
    QJsonObject forgedTargetPayload = baseExtToolPayload();
    forgedTargetPayload[QStringLiteral("target_path")] =
        QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk2\\Partition99");
    const auto forgedTargetScript = builder->buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::Format, target, forgedTargetPayload));
    QVERIFY(!forgedTargetScript.valid());
    QVERIFY(
        forgedTargetScript.blockers.join(' ').contains(QStringLiteral("selected raw partition")));

    QJsonObject missingTargetPayload = baseExtToolPayload();
    missingTargetPayload.remove(QStringLiteral("target_path"));
    const auto missingTargetScript = builder->buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::CheckFileSystem, target, missingTargetPayload));
    QVERIFY(!missingTargetScript.valid());
    QVERIFY(missingTargetScript.blockers.join(' ').contains(
        QStringLiteral("raw partition target path")));
}

void makeFixtureDataPartitionMutable(PartitionInventory* inventory) {
    auto& disk = inventory->disks.first();
    auto& partition = disk.partitions[1];
    disk.is_system = false;
    disk.is_boot = false;
    partition.is_system = false;
    partition.is_boot = false;
    partition.is_efi = false;
    partition.is_msr = false;
    partition.is_recovery = false;
    if (partition.volume) {
        partition.volume->bitlocker_enabled = false;
        partition.volume->bitlocker_locked = false;
        partition.volume->dirty_bit_set = false;
    }
}

void appendDisposableTargetDisk(PartitionInventory* inventory,
                                uint32_t diskNumber = 1,
                                uint64_t sizeBytes = 107'374'182'400ULL) {
    PartitionDiskInfo disk;
    disk.disk_number = diskNumber;
    disk.device_path = QStringLiteral("\\\\.\\PhysicalDrive%1").arg(diskNumber);
    disk.model = QStringLiteral("Disposable Target Disk");
    disk.bus_type = QStringLiteral("SATA");
    disk.media_type = QStringLiteral("HDD");
    disk.partition_style = QStringLiteral("RAW");
    disk.health_status = QStringLiteral("Healthy");
    disk.operational_status = QStringLiteral("Online");
    disk.size_bytes = sizeBytes;
    disk.unallocated_regions.append({diskNumber, 1'048'576ULL, sizeBytes - 1'048'576ULL});
    inventory->disks.append(disk);
}

const UnallocatedRegion* adjacentFreeRegionAfter(const PartitionDiskInfo& disk,
                                                 const PartitionInfoEx& partition) {
    const uint64_t partitionEnd = partition.offset_bytes + partition.size_bytes;
    const auto it = std::find_if(disk.unallocated_regions.cbegin(),
                                 disk.unallocated_regions.cend(),
                                 [partitionEnd](const auto& region) {
                                     return region.offset_bytes == partitionEnd;
                                 });
    return it == disk.unallocated_regions.cend() ? nullptr : &(*it);
}

void appendAdjacentDonorPartition(PartitionInventory* inventory) {
    auto& disk = inventory->disks.first();
    const auto& target = disk.partitions.at(1);
    PartitionVolumeInfo donorVolume;
    donorVolume.drive_letter = QStringLiteral("D");
    donorVolume.label = QStringLiteral("Donor");
    donorVolume.file_system = QStringLiteral("NTFS");
    donorVolume.total_bytes = 1024ULL * 1024ULL * 1024ULL;
    donorVolume.free_bytes = 768ULL * 1024ULL * 1024ULL;
    donorVolume.health_status = QStringLiteral("Healthy");

    PartitionInfoEx donor;
    donor.disk_number = disk.disk_number;
    donor.partition_number = 3;
    donor.type_name = QStringLiteral("Basic");
    donor.offset_bytes = target.offset_bytes + target.size_bytes;
    donor.size_bytes = donorVolume.total_bytes;
    donor.volume = donorVolume;
    disk.partitions.append(donor);
}

PartitionInventory singleDataDiskInventory(bool dynamicDisk = false) {
    PartitionInventory inventory;
    PartitionDiskInfo disk;
    disk.disk_number = 2;
    disk.device_path = QStringLiteral("\\\\.\\PhysicalDrive2");
    disk.model = QStringLiteral("Disposable Data Disk");
    disk.bus_type = QStringLiteral("SATA");
    disk.media_type = QStringLiteral("HDD");
    disk.partition_style = dynamicDisk ? QStringLiteral("Dynamic") : QStringLiteral("MBR");
    disk.health_status = QStringLiteral("Healthy");
    disk.operational_status = QStringLiteral("Online");
    disk.size_bytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
    disk.is_dynamic = dynamicDisk;

    PartitionVolumeInfo volume;
    volume.drive_letter = QStringLiteral("T");
    volume.label = dynamicDisk ? QStringLiteral("DynData") : QStringLiteral("Data");
    volume.file_system = QStringLiteral("NTFS");
    volume.total_bytes = 1024ULL * 1024ULL * 1024ULL;
    volume.free_bytes = 768ULL * 1024ULL * 1024ULL;
    volume.health_status = QStringLiteral("Healthy");

    PartitionInfoEx partition;
    partition.disk_number = disk.disk_number;
    partition.partition_number = 1;
    partition.type_name = dynamicDisk ? QStringLiteral("Simple") : QStringLiteral("Basic");
    partition.offset_bytes = 1024ULL * 1024ULL;
    partition.size_bytes = volume.total_bytes;
    partition.volume = volume;
    disk.partitions.append(partition);
    disk.unallocated_regions.append(
        {disk.disk_number,
         partition.offset_bytes + partition.size_bytes,
         disk.size_bytes - partition.offset_bytes - partition.size_bytes});
    inventory.disks.append(disk);
    return inventory;
}

void PartitionManagerCoreTests::fileSystemDetector_detectsRawSignatures() {
    verifyExtRawDetections();
    verifyWindowsRawDetections();
    verifyXfsRawDetection();
    verifyBtrfsRawDetection();
    verifyHfsRawDetections();
    verifyWrappedHfsRawDetection();
    verifyApfsRawDetection();
    verifyLinuxSwapRawDetection();
    QVERIFY(!PartitionFileSystemDetector::detectBytes(signatureFixture()).has_value());
}

void PartitionManagerCoreTests::fileSystemDetector_flagsMalformedMetadataSanityWarnings() {
    auto xfs = signatureFixture();
    writeAscii(&xfs, 0, "XFSB");
    writeBe32(&xfs, kTestXfsBlockSizeOffset, 3000);
    writeBe64(&xfs, kTestXfsDataBlocksOffset, 128);
    writeBe64(&xfs, kTestXfsFreeDataBlocksOffset, 256);
    writeBe16(&xfs, kTestXfsSectorSizeOffset, 300);
    writeBe16(&xfs, kTestXfsInodeSizeOffset, 128);
    const auto xfsDetection = PartitionFileSystemDetector::detectBytes(xfs, xfs.size());
    QVERIFY(xfsDetection.has_value());
    const QString xfsDetails = xfsDetection->details.join(' ');
    QVERIFY(xfsDetails.contains(QStringLiteral("Metadata sanity warning")));
    QVERIFY(xfsDetails.contains(QStringLiteral("XFS block size")));
    QVERIFY(xfsDetails.contains(QStringLiteral("XFS free data blocks")));
    QVERIFY(xfsDetection->total_bytes == 0);
    QVERIFY(xfsDetection->free_bytes == 0);

    auto btrfs = signatureFixture();
    writeAscii(&btrfs, kTestBtrfsMagicOffset, "_BHRfS_M");
    writeLe64(&btrfs, kTestBtrfsSuperblockOffset + kTestBtrfsTotalBytesOffset, 1024);
    writeLe64(&btrfs, kTestBtrfsSuperblockOffset + kTestBtrfsBytesUsedOffset, 2048);
    writeLe64(&btrfs, kTestBtrfsSuperblockOffset + kTestBtrfsNumDevicesOffset, 0);
    writeLe32(&btrfs, kTestBtrfsSuperblockOffset + kTestBtrfsSectorSizeOffset, 300);
    writeLe32(&btrfs, kTestBtrfsSuperblockOffset + kTestBtrfsNodeSizeOffset, 1024);
    writeLe32(&btrfs, kTestBtrfsSuperblockOffset + kTestBtrfsLeafSizeOffset, 3000);
    const auto btrfsDetection = PartitionFileSystemDetector::detectBytes(btrfs, btrfs.size());
    QVERIFY(btrfsDetection.has_value());
    const QString btrfsDetails = btrfsDetection->details.join(' ');
    QVERIFY(btrfsDetails.contains(QStringLiteral("Metadata sanity warning")));
    QVERIFY(btrfsDetails.contains(QStringLiteral("Btrfs used bytes exceed total bytes")));
    QVERIFY(btrfsDetails.contains(QStringLiteral("Btrfs device count is zero")));
    QVERIFY(btrfsDetection->total_bytes == 0);
    QVERIFY(btrfsDetection->free_bytes == 0);

    auto apfs = signatureFixture();
    writeAscii(&apfs, kTestApfsMagicOffset, "NXSB");
    writeLe32(&apfs, kTestApfsBlockSizeOffset, 3000);
    writeLe64(&apfs, kTestApfsBlockCountOffset, 0);
    writeLe32(&apfs, kTestApfsCheckpointDescriptorBlocksOffset, 2);
    writeLe32(&apfs, kTestApfsCheckpointDataBlocksOffset, 2);
    writeLe64(&apfs, kTestApfsCheckpointDescriptorBaseOffset, 9);
    writeLe64(&apfs, kTestApfsCheckpointDataBaseOffset, 8);
    writeLe32(&apfs, kTestApfsCheckpointDescriptorNextOffset, 2);
    writeLe32(&apfs, kTestApfsCheckpointDataNextOffset, 3);
    writeLe32(&apfs, kTestApfsCheckpointDescriptorIndexOffset, 2);
    writeLe32(&apfs, kTestApfsCheckpointDescriptorLengthOffset, 3);
    writeLe32(&apfs, kTestApfsCheckpointDataIndexOffset, 2);
    writeLe32(&apfs, kTestApfsCheckpointDataLengthOffset, 3);
    const auto apfsDetection = PartitionFileSystemDetector::detectBytes(apfs, apfs.size());
    QVERIFY(apfsDetection.has_value());
    const QString apfsDetails = apfsDetection->details.join(' ');
    QVERIFY(apfsDetails.contains(QStringLiteral("Metadata sanity warning")));
    QVERIFY(apfsDetails.contains(QStringLiteral("APFS block size")));
    QVERIFY(apfsDetails.contains(QStringLiteral("APFS block count is zero")));
    QVERIFY(apfsDetails.contains(QStringLiteral("descriptor length exceeds")));
    QVERIFY(apfsDetails.contains(QStringLiteral("data length exceeds")));
    QVERIFY(apfsDetails.contains(QStringLiteral("descriptor start index")));
    QVERIFY(apfsDetails.contains(QStringLiteral("data next index")));
    QVERIFY(apfsDetection->total_bytes == 0);
}

void PartitionManagerCoreTests::fileSystemDetector_readsProbeBytesFromPath() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString imagePath = dir.filePath(QStringLiteral("ext4.img"));

    auto ext4 = signatureFixture();
    ext4[kTestExtSuperblockOffset + kTestExtMagicOffset] = static_cast<char>(0x53);
    ext4[kTestExtSuperblockOffset + kTestExtMagicOffset + 1] = static_cast<char>(0xEF);
    writeLe32(&ext4, kTestExtSuperblockOffset + kTestExtInodesCountOffset, 128);
    writeLe32(&ext4, kTestExtSuperblockOffset + kTestExtBlocksCountLoOffset, 2048);
    writeLe32(&ext4, kTestExtSuperblockOffset + kTestExtFreeBlocksCountLoOffset, 512);
    writeLe32(&ext4, kTestExtSuperblockOffset + kTestExtLogBlockSizeOffset, 2);
    writeLe32(&ext4, kTestExtSuperblockOffset + kTestExtFeatureIncompatOffset, 0x0040);

    QFile image(imagePath);
    QVERIFY(image.open(QIODevice::WriteOnly));
    QCOMPARE(image.write(ext4), ext4.size());
    image.close();

    QString error;
    const auto bytes = PartitionFileSystemDetector::readProbeBytesFromDevicePath(
        imagePath, 0, static_cast<uint64_t>(ext4.size()), &error);
    QVERIFY2(bytes.has_value(), qPrintable(error));
    QCOMPARE(bytes->size(), ext4.size());

    const auto detection = PartitionFileSystemDetector::detectFromDevicePath(
        imagePath, 0, static_cast<uint64_t>(ext4.size()));
    QVERIFY(detection.has_value());
    QCOMPARE(detection->file_system, QStringLiteral("ext4"));
    QCOMPARE(detection->total_bytes, 4096ULL * 2048ULL);
    QCOMPARE(PartitionFileSystemDetector::probeReadLimit(0),
             PartitionFileSystemDetector::maxProbeBytes());

    const auto missing = PartitionFileSystemDetector::readProbeBytesFromDevicePath(
        dir.filePath(QStringLiteral("missing.img")), 0, 4096, &error);
    QVERIFY(!missing.has_value());
    QVERIFY(error.contains(QStringLiteral("open failed")));
}

void PartitionManagerCoreTests::fileSystemDetector_supplementsApfsSpaceManagerFromCheckpointData() {
    QByteArray image = apfsFarCheckpointSpaceManagerFixture();
    const auto probeOnly = PartitionFileSystemDetector::detectBytes(
        image.left(PartitionFileSystemDetector::maxProbeBytes()),
        static_cast<uint64_t>(image.size()));
    QVERIFY(probeOnly.has_value());
    QCOMPARE(probeOnly->file_system, QStringLiteral("APFS"));
    QCOMPARE(probeOnly->free_bytes, 0ULL);

    QBuffer buffer(&image);
    QVERIFY(buffer.open(QIODevice::ReadOnly));
    QString error;
    const auto detection = PartitionFileSystemDetector::detectFromDevice(
        &buffer, 0, static_cast<uint64_t>(image.size()), &error);
    QVERIFY2(detection.has_value(), qPrintable(error));
    QCOMPARE(detection->file_system, QStringLiteral("APFS"));
    QCOMPARE(detection->free_bytes, 777ULL * kTestApfsBlockSize);
    const QString details = detection->details.join(' ');
    expectDetailsContain(details,
                         {QStringLiteral("APFS space manager source: checkpoint data random read"),
                          QStringLiteral("APFS space manager block: 545"),
                          QStringLiteral("APFS space manager free blocks: 777")});
}

void PartitionManagerCoreTests::extFileSystemReader_listsDirectoriesAndReadsFiles() {
    QByteArray image = extReaderFixture();
    QBuffer buffer(&image);
    QVERIFY(buffer.open(QIODevice::ReadOnly));

    const auto root = PartitionExtFileSystemReader::listDirectory(&buffer, QStringLiteral("/"), 20);
    QVERIFY2(root.ok, qPrintable(root.blockers.join(QStringLiteral("; "))));
    QCOMPARE(root.file_system, QStringLiteral("ext2"));
    QCOMPARE(root.entries.size(), 3);
    QStringList names;
    for (const auto& entry : root.entries) {
        names.append(entry.name);
    }
    QVERIFY(names.contains(QStringLiteral("hello.txt")));
    QVERIFY(names.contains(QStringLiteral("docs")));
    QVERIFY(names.contains(QStringLiteral("hello-link")));
    const auto symlink =
        std::find_if(root.entries.cbegin(), root.entries.cend(), [](const auto& entry) {
            return entry.name == QStringLiteral("hello-link");
        });
    QVERIFY(symlink != root.entries.cend());
    QVERIFY(symlink->symlink);
    QCOMPARE(symlink->type, QStringLiteral("Symlink"));
    QCOMPARE(symlink->symlink_target, QStringLiteral("hello.txt"));

    const auto docs =
        PartitionExtFileSystemReader::listDirectory(&buffer, QStringLiteral("/docs"), 20);
    QVERIFY2(docs.ok, qPrintable(docs.blockers.join(QStringLiteral("; "))));
    QCOMPARE(docs.entries.size(), 1);
    QCOMPARE(docs.entries.first().name, QStringLiteral("note.txt"));
    QVERIFY(docs.entries.first().regular_file);

    const auto hello =
        PartitionExtFileSystemReader::readFile(&buffer, QStringLiteral("/hello.txt"), 1024);
    QVERIFY2(hello.ok, qPrintable(hello.blockers.join(QStringLiteral("; "))));
    QCOMPARE(QString::fromLatin1(hello.data), QStringLiteral("hello from ext\n"));

    const auto note =
        PartitionExtFileSystemReader::readFile(&buffer, QStringLiteral("docs/note.txt"), 1024);
    QVERIFY2(note.ok, qPrintable(note.blockers.join(QStringLiteral("; "))));
    QCOMPARE(QString::fromLatin1(note.data), QStringLiteral("nested note\n"));
}

void PartitionManagerCoreTests::extFileSystemReader_exportsDirectoriesRecursively() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString imagePath = dir.filePath(QStringLiteral("ext.img"));
    QVERIFY(writeBytes(imagePath, extReaderFixture()));
    const QString outputPath = dir.filePath(QStringLiteral("export"));

    const auto exported = PartitionExtFileSystemReader::exportDirectoryFromImage(
        imagePath,
        QStringLiteral("/"),
        outputPath,
        PartitionExtDirectoryExportOptions{20, 1024, 4096});
    QVERIFY2(exported.ok, qPrintable(exported.blockers.join(QStringLiteral("; "))));
    QCOMPARE(exported.files_exported, 2);
    QCOMPARE(exported.directories_exported, 1);
    QCOMPARE(exported.symlinks_exported, 1);
    QCOMPARE(readBytes(QDir(outputPath).filePath(QStringLiteral("hello.txt"))),
             QByteArray("hello from ext\n"));
    QCOMPARE(readBytes(QDir(outputPath).filePath(QStringLiteral("docs/note.txt"))),
             QByteArray("nested note\n"));
    QCOMPARE(readBytes(QDir(outputPath).filePath(QStringLiteral("hello-link.symlink.txt"))),
             QByteArray("hello.txt"));

    const auto capped = PartitionExtFileSystemReader::exportDirectoryFromImage(
        imagePath,
        QStringLiteral("/"),
        dir.filePath(QStringLiteral("capped")),
        PartitionExtDirectoryExportOptions{20, 4, 4096});
    QVERIFY(!capped.ok);
    QVERIFY(capped.blockers.join(' ').contains(QStringLiteral("byte cap")));
}

void PartitionManagerCoreTests::extFileSystemReader_readsExtentMappedFilesAndBlocksUnsafePaths() {
    QByteArray image = extReaderFixture(true);
    QBuffer buffer(&image);
    QVERIFY(buffer.open(QIODevice::ReadOnly));

    const auto hello =
        PartitionExtFileSystemReader::readFile(&buffer, QStringLiteral("/hello.txt"), 1024);
    QVERIFY2(hello.ok, qPrintable(hello.blockers.join(QStringLiteral("; "))));
    QCOMPARE(hello.file_system, QStringLiteral("ext4"));
    QCOMPARE(QString::fromLatin1(hello.data), QStringLiteral("hello from ext\n"));

    const auto blocked =
        PartitionExtFileSystemReader::listDirectory(&buffer, QStringLiteral("../etc"), 20);
    QVERIFY(!blocked.ok);
    QVERIFY(blocked.blockers.join(' ').contains(QStringLiteral("path traversal")));

    const auto capped =
        PartitionExtFileSystemReader::readFile(&buffer, QStringLiteral("/hello.txt"), 4);
    QVERIFY(!capped.ok);
    QVERIFY(capped.blockers.join(' ').contains(QStringLiteral("read cap")));
}

void PartitionManagerCoreTests::hfsFileSystemReader_listsDirectoriesAndReadsFiles() {
    QByteArray image = hfsReaderFixture();
    QBuffer buffer(&image);
    QVERIFY(buffer.open(QIODevice::ReadOnly));

    const auto root = PartitionHfsFileSystemReader::listDirectory(&buffer, QStringLiteral("/"), 20);
    QVERIFY2(root.ok, qPrintable(root.blockers.join(QStringLiteral("; "))));
    QCOMPARE(root.file_system, QStringLiteral("HFS+"));
    QCOMPARE(root.entries.size(), 2);
    QVERIFY(root.warnings.join(' ').contains(QStringLiteral("journal replay")));

    QStringList names;
    for (const auto& entry : root.entries) {
        names.append(entry.name);
    }
    QVERIFY(names.contains(QStringLiteral("hello.txt")));
    QVERIFY(names.contains(QStringLiteral("Docs")));

    const auto docs =
        PartitionHfsFileSystemReader::listDirectory(&buffer, QStringLiteral("/docs"), 20);
    QVERIFY2(docs.ok, qPrintable(docs.blockers.join(QStringLiteral("; "))));
    QCOMPARE(docs.entries.size(), 1);
    QCOMPARE(docs.entries.first().name, QStringLiteral("note.txt"));
    QVERIFY(docs.entries.first().regular_file);

    const auto hello =
        PartitionHfsFileSystemReader::readFile(&buffer, QStringLiteral("/hello.txt"), 1024);
    QVERIFY2(hello.ok, qPrintable(hello.blockers.join(QStringLiteral("; "))));
    QCOMPARE(QString::fromLatin1(hello.data), QStringLiteral("hello from hfs\n"));

    const auto note =
        PartitionHfsFileSystemReader::readFile(&buffer, QStringLiteral("Docs/note.txt"), 1024);
    QVERIFY2(note.ok, qPrintable(note.blockers.join(QStringLiteral("; "))));
    QCOMPARE(QString::fromLatin1(note.data), QStringLiteral("nested hfs note\n"));
}

void PartitionManagerCoreTests::hfsFileSystemReader_exportsDirectoriesAndResourceForks() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString imagePath = dir.filePath(QStringLiteral("hfs.img"));
    QVERIFY(writeBytes(imagePath, hfsReaderFixture()));
    const QString outputPath = dir.filePath(QStringLiteral("export"));

    const auto exported = PartitionHfsFileSystemReader::exportDirectoryFromImage(
        imagePath,
        QStringLiteral("/"),
        outputPath,
        PartitionHfsDirectoryExportOptions{20, 1024, 4096});
    QVERIFY2(exported.ok, qPrintable(exported.blockers.join(QStringLiteral("; "))));
    QCOMPARE(exported.files_exported, 2);
    QCOMPARE(exported.directories_exported, 1);
    QCOMPARE(readBytes(QDir(outputPath).filePath(QStringLiteral("hello.txt"))),
             QByteArray("hello from hfs\n"));
    QCOMPARE(readBytes(QDir(outputPath).filePath(QStringLiteral("Docs/note.txt"))),
             QByteArray("nested hfs note\n"));

    QByteArray expectedHello;
    QByteArray expectedResource;
    const QString resourceImagePath = dir.filePath(QStringLiteral("hfs-resource.img"));
    QVERIFY(
        writeBytes(resourceImagePath, hfsReaderOverflowFixture(&expectedHello, &expectedResource)));
    const QString resourceOutputPath = dir.filePath(QStringLiteral("resource-export"));
    const auto resourceExport = PartitionHfsFileSystemReader::exportDirectoryFromImage(
        resourceImagePath,
        QStringLiteral("/"),
        resourceOutputPath,
        PartitionHfsDirectoryExportOptions{20, 64 * 1024, 128 * 1024});
    QVERIFY2(resourceExport.ok, qPrintable(resourceExport.blockers.join(QStringLiteral("; "))));
    QCOMPARE(resourceExport.files_exported, 1);
    QCOMPARE(resourceExport.resource_forks_exported, 1);
    QCOMPARE(readBytes(QDir(resourceOutputPath).filePath(QStringLiteral("hello.txt"))),
             expectedHello);
    QCOMPARE(readBytes(QDir(resourceOutputPath).filePath(QStringLiteral("hello.txt.rsrc"))),
             expectedResource);
}

void PartitionManagerCoreTests::hfsFileSystemReader_checksCatalogConsistency() {
    QByteArray image = hfsReaderFixture();
    QBuffer buffer(&image);
    QVERIFY(buffer.open(QIODevice::ReadOnly));

    const auto clean = PartitionHfsFileSystemReader::checkConsistency(&buffer, 20);
    QVERIFY2(clean.ok, qPrintable(clean.blockers.join(QStringLiteral("; "))));
    QCOMPARE(clean.file_system, QStringLiteral("HFS+"));
    QCOMPARE(clean.records_scanned, 3);
    QCOMPARE(clean.invalid_records_skipped, 0);
    QCOMPARE(clean.directories, 1);
    QCOMPARE(clean.files, 2);
    QVERIFY(clean.details.join(' ').contains(QStringLiteral("Catalog records scanned: 3")));
    QVERIFY(clean.warnings.join(' ').contains(QStringLiteral("journal replay")));

    QByteArray tolerant = hfsReaderFixture();
    const qsizetype leafOffset =
        static_cast<qsizetype>((kTestHfsCatalogStartBlock + 1) * kTestHfsBlockSize);
    const qsizetype secondRecordOffset =
        kTestHfsBTreeHeaderRecordOffset +
        hfsCatalogRecord(2, QStringLiteral("Docs"), hfsFolderRecord(16)).size();
    const qsizetype secondRecordDataOffset = leafOffset + secondRecordOffset +
                                             hfsCatalogKey(2, QStringLiteral("hello.txt")).size();
    writeBe16(&tolerant, secondRecordDataOffset, 0);
    QBuffer tolerantBuffer(&tolerant);
    QVERIFY(tolerantBuffer.open(QIODevice::ReadOnly));

    const auto tolerantResult = PartitionHfsFileSystemReader::checkConsistency(&tolerantBuffer, 20);
    QVERIFY2(tolerantResult.ok, qPrintable(tolerantResult.blockers.join(QStringLiteral("; "))));
    QCOMPARE(tolerantResult.records_scanned, 2);
    QCOMPARE(tolerantResult.invalid_records_skipped, 1);
    QVERIFY(tolerantResult.details.join(' ').contains(
        QStringLiteral("Invalid catalog records skipped: 1")));
    QVERIFY(tolerantResult.warnings.join(' ').contains(
        QStringLiteral("Skipped 1 invalid HFS+ catalog records")));

    QByteArray corrupted = hfsReaderFixture();
    writeBe16(&corrupted, leafOffset + kTestHfsCatalogNodeSize - 2, 1);
    QBuffer corruptedBuffer(&corrupted);
    QVERIFY(corruptedBuffer.open(QIODevice::ReadOnly));

    const auto blocked = PartitionHfsFileSystemReader::checkConsistency(&corruptedBuffer, 20);
    QVERIFY(!blocked.ok);
    QVERIFY(blocked.blockers.join(' ').contains(QStringLiteral("record offset")));
}

void PartitionManagerCoreTests::hfsFileSystemReader_scansAttributeKeys() {
    QByteArray image = hfsReaderAttributeFixture();
    QBuffer buffer(&image);
    QVERIFY(buffer.open(QIODevice::ReadOnly));

    const auto result = PartitionHfsFileSystemReader::checkConsistency(&buffer, 20);
    QVERIFY2(result.ok, qPrintable(result.blockers.join(QStringLiteral("; "))));
    QVERIFY(result.attributes_present);
    QCOMPARE(result.attribute_records_scanned, 2);
    QCOMPARE(result.inline_attribute_records, 1);
    QCOMPARE(result.fork_attribute_records, 1);
    QCOMPARE(result.extent_attribute_records, 0);
    QVERIFY(result.attribute_names.contains(QStringLiteral("com.apple.FinderInfo")));
    QVERIFY(result.attribute_names.contains(QStringLiteral("com.apple.ResourceFork")));
    QCOMPARE(result.attribute_records.size(), 2);
    QCOMPARE(result.attribute_records.at(0).name, QStringLiteral("com.apple.FinderInfo"));
    QCOMPARE(result.attribute_records.at(0).storage, QStringLiteral("inline"));
    QCOMPARE(result.attribute_records.at(0).size_bytes,
             static_cast<uint64_t>(hfsInlineAttributeValue().size()));
    QVERIFY(result.attribute_records.at(0).readable);
    QCOMPARE(result.attribute_records.at(1).name, QStringLiteral("com.apple.ResourceFork"));
    QCOMPARE(result.attribute_records.at(1).storage, QStringLiteral("fork"));
    QCOMPARE(result.attribute_records.at(1).size_bytes,
             static_cast<uint64_t>(hfsForkAttributeValue().size()));
    QCOMPARE(result.attribute_records.at(1).extent_count, 1);
    QVERIFY(result.attribute_records.at(1).readable);
    QVERIFY(result.attribute_metadata.join(' ').contains(
        QStringLiteral("com.apple.ResourceFork on file-id 17: fork attribute size %1 bytes")
            .arg(hfsForkAttributeValue().size())));
    QVERIFY(result.attribute_metadata.join(' ').contains(
        QStringLiteral("com.apple.FinderInfo on file-id 17: inline attribute size 32 bytes")));
    QVERIFY(result.details.join(' ').contains(QStringLiteral("Attributes file: present")));
    QVERIFY(result.details.join(' ').contains(QStringLiteral("Attribute records scanned: 2")));
    QVERIFY(result.details.join(' ').contains(QStringLiteral("Attribute metadata:")));

    const auto inlineAttribute = PartitionHfsFileSystemReader::readAttributeValue(
        &buffer, 17, QStringLiteral("com.apple.FinderInfo"), 1024);
    QVERIFY2(inlineAttribute.ok, qPrintable(inlineAttribute.blockers.join(QStringLiteral("; "))));
    QCOMPARE(inlineAttribute.storage, QStringLiteral("inline"));
    QCOMPARE(inlineAttribute.data, hfsInlineAttributeValue());

    const auto forkAttribute = PartitionHfsFileSystemReader::readAttributeValue(
        &buffer, 17, QStringLiteral("com.apple.ResourceFork"), 1024);
    QVERIFY2(forkAttribute.ok, qPrintable(forkAttribute.blockers.join(QStringLiteral("; "))));
    QCOMPARE(forkAttribute.storage, QStringLiteral("fork"));
    QCOMPARE(forkAttribute.data, hfsForkAttributeValue());

    const auto cappedAttribute = PartitionHfsFileSystemReader::readAttributeValue(
        &buffer, 17, QStringLiteral("com.apple.ResourceFork"), 4);
    QVERIFY(!cappedAttribute.ok);
    QVERIFY(cappedAttribute.blockers.join(' ').contains(QStringLiteral("read cap")));

    const auto missingAttribute = PartitionHfsFileSystemReader::readAttributeValue(
        &buffer, 17, QStringLiteral("com.apple.Missing"), 1024);
    QVERIFY(!missingAttribute.ok);
    QVERIFY(missingAttribute.blockers.join(' ').contains(QStringLiteral("not found")));
}

void PartitionManagerCoreTests::hfsFileSystemReader_usesOverflowExtentsForCatalogAndFiles() {
    QByteArray expectedHello;
    QByteArray expectedResource;
    QByteArray image = hfsReaderOverflowFixture(&expectedHello, &expectedResource);
    QBuffer buffer(&image);
    QVERIFY(buffer.open(QIODevice::ReadOnly));

    const auto root = PartitionHfsFileSystemReader::listDirectory(&buffer, QStringLiteral("/"), 20);
    QVERIFY2(root.ok, qPrintable(root.blockers.join(QStringLiteral("; "))));
    QCOMPARE(root.entries.size(), 1);
    QCOMPARE(root.entries.first().name, QStringLiteral("hello.txt"));
    QCOMPARE(root.entries.first().size_bytes, static_cast<uint64_t>(expectedHello.size()));
    QCOMPARE(root.entries.first().resource_fork_size_bytes,
             static_cast<uint64_t>(expectedResource.size()));

    const auto hello = PartitionHfsFileSystemReader::readFile(&buffer,
                                                              QStringLiteral("/hello.txt"),
                                                              expectedHello.size());
    QVERIFY2(hello.ok, qPrintable(hello.blockers.join(QStringLiteral("; "))));
    QCOMPARE(hello.data, expectedHello);
    QVERIFY(hello.data.endsWith(QByteArrayLiteral("tail from hfs overflow\n")));
}

void PartitionManagerCoreTests::hfsFileSystemReader_readsResourceForksWithOverflowExtents() {
    QByteArray expectedHello;
    QByteArray expectedResource;
    QByteArray image = hfsReaderOverflowFixture(&expectedHello, &expectedResource);
    QBuffer buffer(&image);
    QVERIFY(buffer.open(QIODevice::ReadOnly));

    const auto resource = PartitionHfsFileSystemReader::readResourceFork(
        &buffer, QStringLiteral("/hello.txt"), expectedResource.size());
    QVERIFY2(resource.ok, qPrintable(resource.blockers.join(QStringLiteral("; "))));
    QCOMPARE(resource.data, expectedResource);
    QVERIFY(resource.data.endsWith(QByteArrayLiteral("tail from hfs resource overflow\n")));

    const auto capped =
        PartitionHfsFileSystemReader::readResourceFork(&buffer, QStringLiteral("/hello.txt"), 4);
    QVERIFY(!capped.ok);
    QVERIFY(capped.blockers.join(' ').contains(QStringLiteral("resource fork")));
    QVERIFY(capped.blockers.join(' ').contains(QStringLiteral("read cap")));
}

void PartitionManagerCoreTests::hfsFileSystemReader_blocksUnsafePathsAndOversizedReads() {
    QByteArray image = hfsReaderFixture();
    QBuffer buffer(&image);
    QVERIFY(buffer.open(QIODevice::ReadOnly));

    const auto blocked =
        PartitionHfsFileSystemReader::listDirectory(&buffer, QStringLiteral("../Users"), 20);
    QVERIFY(!blocked.ok);
    QVERIFY(blocked.blockers.join(' ').contains(QStringLiteral("path traversal")));

    const auto capped =
        PartitionHfsFileSystemReader::readFile(&buffer, QStringLiteral("/hello.txt"), 4);
    QVERIFY(!capped.ok);
    QVERIFY(capped.blockers.join(' ').contains(QStringLiteral("read cap")));
}

void PartitionManagerCoreTests::hfsFileSystemWriter_overwritesSameSizeFilesWithReadback() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString imagePath = temp.filePath(QStringLiteral("hfs-write.img"));
    QVERIFY(writeBytes(imagePath, hfsReaderFixture()));

    const QByteArray replacement("patched hfs ok\n");
    QCOMPARE(replacement.size(), QByteArray("hello from hfs\n").size());

    PartitionHfsFileWriteOptions options;
    auto blocked = PartitionHfsFileSystemWriter::overwriteFileSameSizeFromImage(
        imagePath, QStringLiteral("/hello.txt"), replacement, options);
    QVERIFY(!blocked.ok);
    QVERIFY(blocked.blockers.join(' ').contains(QStringLiteral("writer is not enabled")));
    QVERIFY(blocked.blockers.join(' ').contains(QStringLiteral("target confirmation")));

    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.evidence_id = QStringLiteral("unit.hfs-same-size-overwrite");
    blocked = PartitionHfsFileSystemWriter::overwriteFileSameSizeFromImage(
        imagePath, QStringLiteral("/hello.txt"), replacement, options);
    QVERIFY(!blocked.ok);
    QVERIFY(blocked.blockers.join(' ').contains(QStringLiteral("journal")));

    options.allow_journaled_volume = true;
    const auto written = PartitionHfsFileSystemWriter::overwriteFileSameSizeFromImage(
        imagePath, QStringLiteral("/hello.txt"), replacement, options);
    QVERIFY2(written.ok, qPrintable(written.blockers.join(QStringLiteral("; "))));
    QCOMPARE(written.file_system, QStringLiteral("HFS+"));
    QCOMPARE(written.catalog_id, 17U);
    QCOMPARE(written.bytes_written, static_cast<uint64_t>(replacement.size()));
    QCOMPARE(written.chunks_written, 1);
    QVERIFY(!written.before_sha256.isEmpty());
    QVERIFY(!written.after_sha256.isEmpty());
    QVERIFY(written.before_sha256 != written.after_sha256);

    const auto readBack = PartitionHfsFileSystemReader::readFileFromImage(
        imagePath, QStringLiteral("/hello.txt"), 1024);
    QVERIFY2(readBack.ok, qPrintable(readBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(readBack.data, replacement);

    const auto sizeBlocked = PartitionHfsFileSystemWriter::overwriteFileSameSizeFromImage(
        imagePath, QStringLiteral("/hello.txt"), QByteArrayLiteral("short"), options);
    QVERIFY(!sizeBlocked.ok);
    QVERIFY(sizeBlocked.blockers.join(' ').contains(QStringLiteral("same-size")));
}

void PartitionManagerCoreTests::hfsFileSystemWriter_overwritesOverflowExtentFilesWithReadback() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString imagePath = temp.filePath(QStringLiteral("hfs-overflow-write.img"));

    QByteArray expectedHello;
    QVERIFY(writeBytes(imagePath, hfsReaderOverflowFixture(&expectedHello)));
    QByteArray replacement(expectedHello.size(), 'Z');
    replacement.replace(0,
                        QByteArrayLiteral("patched overflow hfs").size(),
                        QByteArrayLiteral("patched overflow hfs"));

    PartitionHfsFileWriteOptions options;
    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.allow_journaled_volume = true;
    options.evidence_id = QStringLiteral("unit.hfs-overflow-same-size-overwrite");

    const auto written = PartitionHfsFileSystemWriter::overwriteFileSameSizeFromImage(
        imagePath, QStringLiteral("/hello.txt"), replacement, options);
    QVERIFY2(written.ok, qPrintable(written.blockers.join(QStringLiteral("; "))));
    QCOMPARE(written.file_system, QStringLiteral("HFS+"));
    QCOMPARE(written.catalog_id, 17U);
    QCOMPARE(written.bytes_written, static_cast<uint64_t>(replacement.size()));
    QVERIFY(written.chunks_written >= 2);
    QVERIFY(!written.before_sha256.isEmpty());
    QVERIFY(!written.after_sha256.isEmpty());
    QVERIFY(written.before_sha256 != written.after_sha256);

    const auto readBack = PartitionHfsFileSystemReader::readFileFromImage(
        imagePath, QStringLiteral("/hello.txt"), static_cast<uint64_t>(replacement.size()));
    QVERIFY2(readBack.ok, qPrintable(readBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(readBack.data, replacement);
}

void PartitionManagerCoreTests::hfsFileSystemWriter_replacesFileWithinAllocatedBlocks() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString imagePath = temp.filePath(QStringLiteral("hfs-resize-write.img"));
    QVERIFY(writeBytes(imagePath, hfsReaderFixture()));

    PartitionHfsFileWriteOptions options;
    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.allow_journaled_volume = true;
    options.evidence_id = QStringLiteral("unit.hfs-allocated-block-replace");

    const QByteArray grown("grown hfs payload inside allocation\n");
    const auto grownWrite = PartitionHfsFileSystemWriter::replaceFileWithinAllocatedBlocksFromImage(
        imagePath, QStringLiteral("/hello.txt"), grown, options);
    QVERIFY2(grownWrite.ok, qPrintable(grownWrite.blockers.join(QStringLiteral("; "))));
    QCOMPARE(grownWrite.catalog_id, 17U);
    QCOMPARE(grownWrite.bytes_written, static_cast<uint64_t>(grown.size()));
    QVERIFY(grownWrite.warnings.join(' ').contains(QStringLiteral("already allocated")));

    auto readBack = PartitionHfsFileSystemReader::readFileFromImage(imagePath,
                                                                    QStringLiteral("/hello.txt"),
                                                                    4096);
    QVERIFY2(readBack.ok, qPrintable(readBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(readBack.data, grown);

    const QByteArray shrunk("tiny hfs\n");
    const auto shrunkWrite =
        PartitionHfsFileSystemWriter::replaceFileWithinAllocatedBlocksFromImage(
            imagePath, QStringLiteral("/hello.txt"), shrunk, options);
    QVERIFY2(shrunkWrite.ok, qPrintable(shrunkWrite.blockers.join(QStringLiteral("; "))));
    QCOMPARE(shrunkWrite.bytes_written, static_cast<uint64_t>(shrunk.size()));

    readBack = PartitionHfsFileSystemReader::readFileFromImage(imagePath,
                                                               QStringLiteral("/hello.txt"),
                                                               4096);
    QVERIFY2(readBack.ok, qPrintable(readBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(readBack.data, shrunk);

    const QByteArray tooLarge(static_cast<int>(kTestHfsBlockSize) + 1, 'x');
    const auto blocked = PartitionHfsFileSystemWriter::replaceFileWithinAllocatedBlocksFromImage(
        imagePath, QStringLiteral("/hello.txt"), tooLarge, options);
    QVERIFY(!blocked.ok);
    QVERIFY(blocked.blockers.join(' ').contains(QStringLiteral("existing allocation")));

    const auto truncated = PartitionHfsFileSystemWriter::truncateFileWithinAllocatedBlocksFromImage(
        imagePath, QStringLiteral("/hello.txt"), options);
    QVERIFY2(truncated.ok, qPrintable(truncated.blockers.join(QStringLiteral("; "))));
    QCOMPARE(truncated.catalog_id, 17U);
    QCOMPARE(truncated.bytes_written, 0ULL);
    QVERIFY(truncated.chunks_written > 0);
    QVERIFY(truncated.warnings.join(' ').contains(QStringLiteral("truncated")));
    QCOMPARE(truncated.after_sha256, sha256Hex(QByteArray()));

    readBack = PartitionHfsFileSystemReader::readFileFromImage(imagePath,
                                                               QStringLiteral("/hello.txt"),
                                                               4096);
    QVERIFY2(readBack.ok, qPrintable(readBack.blockers.join(QStringLiteral("; "))));
    QVERIFY(readBack.data.isEmpty());
}

void PartitionManagerCoreTests::hfsFileSystemWriter_replacesResourceForkWithinAllocatedBlocks() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString imagePath = temp.filePath(QStringLiteral("hfs-resource-resize-write.img"));

    QByteArray expectedHello;
    QVERIFY(writeBytes(imagePath, hfsReaderOverflowFixture(&expectedHello)));

    PartitionHfsFileWriteOptions options;
    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.allow_journaled_volume = true;
    options.evidence_id = QStringLiteral("unit.hfs-resource-allocated-block-replace");

    QByteArray grown(kTestHfsBlockSize + 64, 'S');
    grown.replace(0,
                  QByteArrayLiteral("grown hfs resource").size(),
                  QByteArrayLiteral("grown hfs resource"));
    const auto grownWrite =
        PartitionHfsFileSystemWriter::replaceResourceForkWithinAllocatedBlocksFromImage(
            imagePath, QStringLiteral("/hello.txt"), grown, options);
    QVERIFY2(grownWrite.ok, qPrintable(grownWrite.blockers.join(QStringLiteral("; "))));
    QCOMPARE(grownWrite.catalog_id, 17U);
    QCOMPARE(grownWrite.bytes_written, static_cast<uint64_t>(grown.size()));
    QVERIFY(grownWrite.chunks_written >= 2);
    QVERIFY(grownWrite.warnings.join(' ').contains(QStringLiteral("resource fork")));

    auto resourceBack = PartitionHfsFileSystemReader::readResourceForkFromImage(
        imagePath, QStringLiteral("/hello.txt"), 2 * kTestHfsBlockSize);
    QVERIFY2(resourceBack.ok, qPrintable(resourceBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(resourceBack.data, grown);

    auto dataBack = PartitionHfsFileSystemReader::readFileFromImage(imagePath,
                                                                    QStringLiteral("/hello.txt"),
                                                                    2 * kTestHfsBlockSize);
    QVERIFY2(dataBack.ok, qPrintable(dataBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(dataBack.data, expectedHello);

    const QByteArray shrunk("tiny resource fork\n");
    const auto shrunkWrite =
        PartitionHfsFileSystemWriter::replaceResourceForkWithinAllocatedBlocksFromImage(
            imagePath, QStringLiteral("/hello.txt"), shrunk, options);
    QVERIFY2(shrunkWrite.ok, qPrintable(shrunkWrite.blockers.join(QStringLiteral("; "))));
    QCOMPARE(shrunkWrite.bytes_written, static_cast<uint64_t>(shrunk.size()));

    resourceBack = PartitionHfsFileSystemReader::readResourceForkFromImage(
        imagePath, QStringLiteral("/hello.txt"), 2 * kTestHfsBlockSize);
    QVERIFY2(resourceBack.ok, qPrintable(resourceBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(resourceBack.data, shrunk);

    const QByteArray tooLarge(static_cast<int>(2 * kTestHfsBlockSize) + 1, 'x');
    const auto blocked =
        PartitionHfsFileSystemWriter::replaceResourceForkWithinAllocatedBlocksFromImage(
            imagePath, QStringLiteral("/hello.txt"), tooLarge, options);
    QVERIFY(!blocked.ok);
    QVERIFY(blocked.blockers.join(' ').contains(QStringLiteral("existing allocation")));
    QVERIFY(blocked.blockers.join(' ').contains(QStringLiteral("resource fork")));
}

void PartitionManagerCoreTests::hfsFileSystemWriter_growsForksWithAllocationBitmapUpdates() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    PartitionHfsFileWriteOptions options;
    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.allow_journaled_volume = true;
    options.evidence_id = QStringLiteral("unit.hfs-allocation-growth");

    const QString dataImagePath = temp.filePath(QStringLiteral("hfs-data-growth.img"));
    QVERIFY(writeBytes(dataImagePath, hfsReaderFixture()));
    const QByteArray dataPayload(static_cast<int>(kTestHfsBlockSize + 128), 'D');
    const auto dataWritten = PartitionHfsFileSystemWriter::replaceFileWithAllocationGrowthFromImage(
        dataImagePath, QStringLiteral("/hello.txt"), dataPayload, options);
    QVERIFY2(dataWritten.ok, qPrintable(dataWritten.blockers.join(QStringLiteral("; "))));
    QCOMPARE(dataWritten.catalog_id, 17U);
    QCOMPARE(dataWritten.bytes_written, static_cast<uint64_t>(dataPayload.size()));
    QVERIFY(dataWritten.warnings.join(' ').contains(QStringLiteral("allocating 1 new block")));

    auto dataBack = PartitionHfsFileSystemReader::readFileFromImage(dataImagePath,
                                                                    QStringLiteral("/hello.txt"),
                                                                    2 * kTestHfsBlockSize);
    QVERIFY2(dataBack.ok, qPrintable(dataBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(dataBack.data, dataPayload);
    const QByteArray dataAfter = readBytes(dataImagePath);
    QVERIFY(hfsAllocationBitSet(dataAfter, 1));
    QCOMPARE(readBe32(dataAfter, kTestHfsHeaderOffset + kTestHfsFreeBlocksOffset), 39U);

    const auto noGrowth = PartitionHfsFileSystemWriter::replaceFileWithAllocationGrowthFromImage(
        dataImagePath, QStringLiteral("/hello.txt"), QByteArrayLiteral("small"), options);
    QVERIFY(!noGrowth.ok);
    QVERIFY(
        noGrowth.blockers.join(' ').contains(QStringLiteral("does not require allocation growth")));

    const QString resourceImagePath = temp.filePath(QStringLiteral("hfs-resource-growth.img"));
    QVERIFY(writeBytes(resourceImagePath, hfsReaderFixture()));
    const QByteArray resourcePayload("new resource fork allocation");
    const auto resourceWritten =
        PartitionHfsFileSystemWriter::replaceResourceForkWithAllocationGrowthFromImage(
            resourceImagePath, QStringLiteral("/hello.txt"), resourcePayload, options);
    QVERIFY2(resourceWritten.ok, qPrintable(resourceWritten.blockers.join(QStringLiteral("; "))));
    QCOMPARE(resourceWritten.catalog_id, 17U);
    QVERIFY(resourceWritten.warnings.join(' ').contains(QStringLiteral("resource fork")));

    auto resourceBack = PartitionHfsFileSystemReader::readResourceForkFromImage(
        resourceImagePath, QStringLiteral("/hello.txt"), kTestHfsBlockSize);
    QVERIFY2(resourceBack.ok, qPrintable(resourceBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(resourceBack.data, resourcePayload);
    const QByteArray resourceAfter = readBytes(resourceImagePath);
    QVERIFY(hfsAllocationBitSet(resourceAfter, 1));
    QCOMPARE(readBe32(resourceAfter, kTestHfsHeaderOffset + kTestHfsFreeBlocksOffset), 39U);

    QByteArray fullImage = hfsReaderFixture();
    writeBe32(&fullImage, kTestHfsHeaderOffset + kTestHfsFreeBlocksOffset, 0);
    const QString fullImagePath = temp.filePath(QStringLiteral("hfs-data-growth-full.img"));
    QVERIFY(writeBytes(fullImagePath, fullImage));
    const auto blocked = PartitionHfsFileSystemWriter::replaceFileWithAllocationGrowthFromImage(
        fullImagePath, QStringLiteral("/hello.txt"), dataPayload, options);
    QVERIFY(!blocked.ok);
    QVERIFY(blocked.blockers.join(' ').contains(QStringLiteral("not have enough free blocks")));
}

void PartitionManagerCoreTests::hfsFileSystemReader_readsDecmpfsCompressedFiles() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    QByteArray inlineContent(900, 'Z');
    inlineContent.replace(0, 26, QByteArrayLiteral("decmpfs inline zlib proof\n"));
    const QString inlinePath = temp.filePath(QStringLiteral("hfs-decmpfs-inline.img"));
    QVERIFY(writeBytes(inlinePath, hfsCompressedFixture(3, inlineContent)));
    const auto inlineRead = PartitionHfsFileSystemReader::readFileFromImage(
        inlinePath, QStringLiteral("/packed.bin"), 4096);
    QVERIFY2(inlineRead.ok, qPrintable(inlineRead.blockers.join(QStringLiteral("; "))));
    QCOMPARE(inlineRead.data, inlineContent);
    QVERIFY(inlineRead.warnings.join(' ').contains(QStringLiteral("decmpfs type-3")));

    QByteArray rawContent = QByteArrayLiteral("incompressible?");
    while (rawContent.size() < 64) {
        rawContent.append(static_cast<char>((rawContent.size() * 89 + 7) % 251));
    }
    const QString rawPath = temp.filePath(QStringLiteral("hfs-decmpfs-raw.img"));
    QVERIFY(writeBytes(rawPath, hfsCompressedFixture(3, rawContent)));
    const auto rawRead = PartitionHfsFileSystemReader::readFileFromImage(
        rawPath, QStringLiteral("/packed.bin"), 4096);
    QVERIFY2(rawRead.ok, qPrintable(rawRead.blockers.join(QStringLiteral("; "))));
    QCOMPARE(rawRead.data, rawContent);

    QByteArray resourceContent(70'000, 'R');
    for (int index = 0; index < resourceContent.size(); index += 97) {
        resourceContent[index] = static_cast<char>('a' + (index % 23));
    }
    const QString resourcePath = temp.filePath(QStringLiteral("hfs-decmpfs-resource.img"));
    QVERIFY(writeBytes(resourcePath, hfsCompressedFixture(4, resourceContent)));
    const auto resourceRead = PartitionHfsFileSystemReader::readFileFromImage(
        resourcePath, QStringLiteral("/packed.bin"), 1024 * 1024);
    QVERIFY2(resourceRead.ok, qPrintable(resourceRead.blockers.join(QStringLiteral("; "))));
    QCOMPARE(resourceRead.data, resourceContent);
    QVERIFY(resourceRead.warnings.join(' ').contains(QStringLiteral("decmpfs type-4")));

    const auto capBlocked = PartitionHfsFileSystemReader::readFileFromImage(
        resourcePath, QStringLiteral("/packed.bin"), 100);
    QVERIFY(!capBlocked.ok);
    QVERIFY(capBlocked.blockers.join(' ').contains(QStringLiteral("read cap")));

    // LZVN and LZFSE containers (raw-escape inline chunks, chunked offset-table
    // resource forks, and a handcrafted lzfse uncompressed block).
    const QString lzvnInlinePath = temp.filePath(QStringLiteral("hfs-decmpfs-lzvn-inline.img"));
    QVERIFY(writeBytes(lzvnInlinePath, hfsCompressedFixture(7, inlineContent)));
    const auto lzvnInlineRead = PartitionHfsFileSystemReader::readFileFromImage(
        lzvnInlinePath, QStringLiteral("/packed.bin"), 4096);
    QVERIFY2(lzvnInlineRead.ok, qPrintable(lzvnInlineRead.blockers.join(QStringLiteral("; "))));
    QCOMPARE(lzvnInlineRead.data, inlineContent);
    QVERIFY(lzvnInlineRead.warnings.join(' ').contains(QStringLiteral("decmpfs type-7")));

    const QString lzvnResourcePath = temp.filePath(QStringLiteral("hfs-decmpfs-lzvn-resource.img"));
    QVERIFY(writeBytes(lzvnResourcePath, hfsCompressedFixture(8, rawContent)));
    const auto lzvnResourceRead = PartitionHfsFileSystemReader::readFileFromImage(
        lzvnResourcePath, QStringLiteral("/packed.bin"), 4096);
    QVERIFY2(lzvnResourceRead.ok, qPrintable(lzvnResourceRead.blockers.join(QStringLiteral("; "))));
    QCOMPARE(lzvnResourceRead.data, rawContent);

    const QString lzfseInlinePath = temp.filePath(QStringLiteral("hfs-decmpfs-lzfse-inline.img"));
    QVERIFY(writeBytes(lzfseInlinePath, hfsCompressedFixture(11, inlineContent)));
    const auto lzfseInlineRead = PartitionHfsFileSystemReader::readFileFromImage(
        lzfseInlinePath, QStringLiteral("/packed.bin"), 4096);
    QVERIFY2(lzfseInlineRead.ok, qPrintable(lzfseInlineRead.blockers.join(QStringLiteral("; "))));
    QCOMPARE(lzfseInlineRead.data, inlineContent);
    QVERIFY(lzfseInlineRead.warnings.join(' ').contains(QStringLiteral("decmpfs type-11")));

    const QString lzfseResourcePath =
        temp.filePath(QStringLiteral("hfs-decmpfs-lzfse-resource.img"));
    QVERIFY(writeBytes(lzfseResourcePath, hfsCompressedFixture(12, rawContent)));
    const auto lzfseResourceRead = PartitionHfsFileSystemReader::readFileFromImage(
        lzfseResourcePath, QStringLiteral("/packed.bin"), 4096);
    QVERIFY2(lzfseResourceRead.ok,
             qPrintable(lzfseResourceRead.blockers.join(QStringLiteral("; "))));
    QCOMPARE(lzfseResourceRead.data, rawContent);

    const QString unsupportedPath = temp.filePath(QStringLiteral("hfs-decmpfs-unknown.img"));
    QVERIFY(writeBytes(unsupportedPath, hfsCompressedFixture(9, inlineContent)));
    const auto unsupportedRead = PartitionHfsFileSystemReader::readFileFromImage(
        unsupportedPath, QStringLiteral("/packed.bin"), 4096);
    QVERIFY(!unsupportedRead.ok);
    QVERIFY(unsupportedRead.blockers.join(' ').contains(
        QStringLiteral("compression type 9 is not supported")));
}

void PartitionManagerCoreTests::hfsFileSystemWriter_replacesDecmpfsCompressedContent() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    PartitionHfsFileWriteOptions options;
    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.allow_journaled_volume = true;
    options.evidence_id = QStringLiteral("unit.hfs-decmpfs-replace");

    QByteArray inlineContent(900, 'Z');
    const QString inlinePath = temp.filePath(QStringLiteral("hfs-decmpfs-inline-write.img"));
    QVERIFY(writeBytes(inlinePath, hfsCompressedFixture(3, inlineContent, 128)));

    QByteArray newInlineContent(700, 'Q');
    newInlineContent.replace(0, 24, QByteArrayLiteral("replaced inline payload\n"));
    const auto blockedWithoutOptIn =
        PartitionHfsFileSystemWriter::replaceCompressedFileContentFromImage(
            inlinePath, QStringLiteral("/packed.bin"), newInlineContent, options);
    QVERIFY(!blockedWithoutOptIn.ok);
    QVERIFY(blockedWithoutOptIn.blockers.join(' ').contains(
        QStringLiteral("compressed-file mutation opt-in")));

    options.allow_compressed_file_mutation = true;
    const auto inlineReplaced = PartitionHfsFileSystemWriter::replaceCompressedFileContentFromImage(
        inlinePath, QStringLiteral("/packed.bin"), newInlineContent, options);
    QVERIFY2(inlineReplaced.ok, qPrintable(inlineReplaced.blockers.join(QStringLiteral("; "))));
    QCOMPARE(inlineReplaced.catalog_id, kTestHfsCompressedFileId);
    QCOMPARE(inlineReplaced.bytes_written, static_cast<uint64_t>(newInlineContent.size()));
    QVERIFY(inlineReplaced.warnings.join(' ').contains(QStringLiteral("decmpfs type-3")));
    QVERIFY(!inlineReplaced.before_sha256.isEmpty());
    QVERIFY(inlineReplaced.before_sha256 != inlineReplaced.after_sha256);

    const auto inlineReadBack = PartitionHfsFileSystemReader::readFileFromImage(
        inlinePath, QStringLiteral("/packed.bin"), 4096);
    QVERIFY2(inlineReadBack.ok, qPrintable(inlineReadBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(inlineReadBack.data, newInlineContent);

    QByteArray resourceContent(70'000, 'R');
    const QString resourcePath = temp.filePath(QStringLiteral("hfs-decmpfs-resource-write.img"));
    QVERIFY(writeBytes(resourcePath, hfsCompressedFixture(4, resourceContent)));

    QByteArray newResourceContent(81'000, 'S');
    for (int index = 0; index < newResourceContent.size(); index += 53) {
        newResourceContent[index] = static_cast<char>('A' + (index % 26));
    }
    const auto resourceReplaced =
        PartitionHfsFileSystemWriter::replaceCompressedFileContentFromImage(
            resourcePath, QStringLiteral("/packed.bin"), newResourceContent, options);
    QVERIFY2(resourceReplaced.ok, qPrintable(resourceReplaced.blockers.join(QStringLiteral("; "))));
    QCOMPARE(resourceReplaced.catalog_id, kTestHfsCompressedFileId);
    QVERIFY(resourceReplaced.warnings.join(' ').contains(QStringLiteral("decmpfs type-4")));

    const auto resourceReadBack = PartitionHfsFileSystemReader::readFileFromImage(
        resourcePath, QStringLiteral("/packed.bin"), 1024 * 1024);
    QVERIFY2(resourceReadBack.ok, qPrintable(resourceReadBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(resourceReadBack.data, newResourceContent);

    const auto attrAfter = PartitionHfsFileSystemReader::readAttributeValueFromImage(
        resourcePath, kTestHfsCompressedFileId, QStringLiteral("com.apple.decmpfs"), 4096);
    QVERIFY2(attrAfter.ok, qPrintable(attrAfter.blockers.join(QStringLiteral("; "))));
    QCOMPARE(attrAfter.data.size(), 16);
    uint64_t storedSize = 0;
    for (int index = 0; index < 8; ++index) {
        storedSize |=
            static_cast<uint64_t>(static_cast<unsigned char>(attrAfter.data.at(8 + index)))
            << (8 * index);
    }
    QCOMPARE(storedSize, static_cast<uint64_t>(newResourceContent.size()));

    // LZVN and LZFSE replacement round trips: the replacement payload runs
    // through the vendored Apple reference encoders and must read back exactly
    // through the matching decoders.
    struct CodecReplaceCase {
        uint32_t type;
        const char* name;
        const char* warning;
    };
    const QVector<CodecReplaceCase> codecCases{
        {7, "hfs-decmpfs-lzvn-inline-write.img", "decmpfs type-7"},
        {8, "hfs-decmpfs-lzvn-resource-write.img", "decmpfs type-8"},
        {11, "hfs-decmpfs-lzfse-inline-write.img", "decmpfs type-11"},
        {12, "hfs-decmpfs-lzfse-resource-write.img", "decmpfs type-12"},
    };
    for (const auto& codecCase : codecCases) {
        const bool isResource = codecCase.type == 8 || codecCase.type == 12;
        // Resource-case seeds stay single-chunk so they fit the fixture's
        // 16-block resource fork before the replacement grows it.
        const QByteArray seedContent = isResource ? QByteArray(2000, 's') : inlineContent;
        QByteArray replacement(isResource ? 81'000 : 700, '\0');
        for (int index = 0; index < replacement.size(); ++index) {
            replacement[index] = static_cast<char>('a' + ((index / 7) % 13));
        }
        const QString codecPath = temp.filePath(QString::fromLatin1(codecCase.name));
        QVERIFY(writeBytes(
            codecPath, hfsCompressedFixture(codecCase.type, seedContent, isResource ? 0 : 192)));
        const auto replaced = PartitionHfsFileSystemWriter::replaceCompressedFileContentFromImage(
            codecPath, QStringLiteral("/packed.bin"), replacement, options);
        QVERIFY2(replaced.ok,
                 qPrintable(QStringLiteral("type %1: %2")
                                .arg(codecCase.type)
                                .arg(replaced.blockers.join(QStringLiteral("; ")))));
        QVERIFY(replaced.warnings.join(' ').contains(QString::fromLatin1(codecCase.warning)));
        const auto codecReadBack = PartitionHfsFileSystemReader::readFileFromImage(
            codecPath, QStringLiteral("/packed.bin"), 1024 * 1024);
        QVERIFY2(codecReadBack.ok,
                 qPrintable(QStringLiteral("type %1: %2")
                                .arg(codecCase.type)
                                .arg(codecReadBack.blockers.join(QStringLiteral("; ")))));
        QCOMPARE(codecReadBack.data, replacement);
    }

    const QString unsupportedPath = temp.filePath(QStringLiteral("hfs-decmpfs-unknown-write.img"));
    QVERIFY(writeBytes(unsupportedPath, hfsCompressedFixture(9, inlineContent)));
    const auto unsupportedReplace =
        PartitionHfsFileSystemWriter::replaceCompressedFileContentFromImage(
            unsupportedPath, QStringLiteral("/packed.bin"), newInlineContent, options);
    QVERIFY(!unsupportedReplace.ok);
    QVERIFY(unsupportedReplace.blockers.join(' ').contains(
        QStringLiteral("supported decmpfs payload")));
}

void PartitionManagerCoreTests::hfsFileSystemWriter_splitsCatalogRootLeafOnCreate() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    PartitionHfsFileWriteOptions options;
    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.allow_journaled_volume = true;
    options.evidence_id = QStringLiteral("unit.hfs-catalog-root-leaf-split");

    const QString imagePath = temp.filePath(QStringLiteral("hfs-catalog-split-create.img"));
    QVERIFY(writeBytes(imagePath, hfsSplitReadyFixture()));

    const auto created = PartitionHfsFileSystemWriter::createEmptyFileFromImage(
        imagePath, QStringLiteral("/split-created.txt"), options);
    QVERIFY2(created.ok, qPrintable(created.blockers.join(QStringLiteral("; "))));
    QCOMPARE(created.catalog_id, 30U);
    QVERIFY(created.warnings.join(' ').contains(QStringLiteral("split into two leaf nodes")));

    const QByteArray after = readBytes(imagePath);
    const qsizetype headerOffset =
        static_cast<qsizetype>(kTestHfsCatalogStartBlock * kTestHfsBlockSize) +
        kTestHfsBTreeHeaderRecordOffset;
    QCOMPARE(readBe16(after, headerOffset + kTestHfsBTreeHeaderTreeDepthOffset), 2);
    const uint32_t rootNode = readBe32(after, headerOffset + kTestHfsBTreeHeaderRootNodeOffset);
    const uint32_t firstLeaf = readBe32(after,
                                        headerOffset + kTestHfsBTreeHeaderFirstLeafNodeOffset);
    const uint32_t lastLeaf = readBe32(after, headerOffset + kTestHfsBTreeHeaderLastLeafNodeOffset);
    QCOMPARE(firstLeaf, 2U);
    QCOMPARE(lastLeaf, 3U);
    QCOMPARE(rootNode, 4U);
    QCOMPARE(readBe32(after, headerOffset + kTestHfsBTreeHeaderLeafRecordsOffset),
             static_cast<uint32_t>(kTestHfsSplitFixtureFileCount + 2));
    QCOMPARE(readBe32(after, headerOffset + kTestHfsBTreeHeaderFreeNodesOffset),
             static_cast<uint32_t>(kTestHfsSplitCatalogTotalNodes - 4));
    const auto mapByte = static_cast<quint8>(
        after.at(static_cast<qsizetype>(kTestHfsCatalogStartBlock * kTestHfsBlockSize) +
                 kTestHfsBTreeHeaderMapRecordOffset));
    QCOMPARE(mapByte, static_cast<quint8>(0xB8));

    const qsizetype leftLeafOffset =
        static_cast<qsizetype>((kTestHfsCatalogStartBlock + firstLeaf) * kTestHfsBlockSize);
    const qsizetype rightLeafOffset =
        static_cast<qsizetype>((kTestHfsCatalogStartBlock + lastLeaf) * kTestHfsBlockSize);
    QCOMPARE(readBe32(after, leftLeafOffset), lastLeaf);
    QCOMPARE(readBe32(after, rightLeafOffset + 4), firstLeaf);

    const auto root =
        PartitionHfsFileSystemReader::listDirectoryFromImage(imagePath, QStringLiteral("/"), 40);
    QVERIFY2(root.ok, qPrintable(root.blockers.join(QStringLiteral("; "))));
    QCOMPARE(root.entries.size(), kTestHfsSplitFixtureFileCount + 1);
    QVERIFY(std::any_of(root.entries.cbegin(), root.entries.cend(), [](const auto& entry) {
        return entry.name == QStringLiteral("split-created.txt") && entry.regular_file;
    }));

    const auto firstFile =
        PartitionHfsFileSystemReader::readFileFromImage(imagePath, QStringLiteral("/file-aa"), 1);
    QVERIFY2(firstFile.ok, qPrintable(firstFile.blockers.join(QStringLiteral("; "))));
    QVERIFY(firstFile.data.isEmpty());

    const auto consistency = PartitionHfsFileSystemReader::checkConsistencyFromImage(imagePath,
                                                                                     200);
    QVERIFY2(consistency.ok, qPrintable(consistency.blockers.join(QStringLiteral("; "))));
    QCOMPARE(consistency.files, kTestHfsSplitFixtureFileCount + 1);
    QCOMPARE(consistency.records_scanned, kTestHfsSplitFixtureFileCount + 2);
    QVERIFY(!consistency.warnings.join(' ').contains(QStringLiteral("record count mismatch")));

    const QString dataImagePath = temp.filePath(QStringLiteral("hfs-catalog-split-data.img"));
    QVERIFY(writeBytes(dataImagePath, hfsSplitReadyFixture()));
    const QByteArray payload(300, 'P');
    const auto dataCreated = PartitionHfsFileSystemWriter::createFileWithDataFromImage(
        dataImagePath, QStringLiteral("/split-data.bin"), payload, options);
    QVERIFY2(dataCreated.ok, qPrintable(dataCreated.blockers.join(QStringLiteral("; "))));
    QVERIFY(dataCreated.warnings.join(' ').contains(QStringLiteral("split into two leaf nodes")));
    const auto dataReadBack = PartitionHfsFileSystemReader::readFileFromImage(
        dataImagePath, QStringLiteral("/split-data.bin"), 1024);
    QVERIFY2(dataReadBack.ok, qPrintable(dataReadBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(dataReadBack.data, payload);

    const QString folderImagePath = temp.filePath(QStringLiteral("hfs-catalog-split-folder.img"));
    QVERIFY(writeBytes(folderImagePath, hfsSplitReadyFixture()));
    const QString splitFolderName =
        QStringLiteral("/Split Folder %1").arg(QString(120, QLatin1Char('x')));
    const auto folderCreated = PartitionHfsFileSystemWriter::createEmptyFolderFromImage(
        folderImagePath, splitFolderName, options);
    QVERIFY2(folderCreated.ok, qPrintable(folderCreated.blockers.join(QStringLiteral("; "))));
    QVERIFY(folderCreated.warnings.join(' ').contains(QStringLiteral("split into two leaf nodes")));
    const auto folderListing =
        PartitionHfsFileSystemReader::listDirectoryFromImage(folderImagePath, splitFolderName, 5);
    QVERIFY2(folderListing.ok, qPrintable(folderListing.blockers.join(QStringLiteral("; "))));
    QVERIFY(folderListing.entries.isEmpty());
}

void PartitionManagerCoreTests::hfsFileSystemWriter_mutatesDepthTwoCatalogs() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    PartitionHfsFileWriteOptions options;
    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.allow_journaled_volume = true;
    options.evidence_id = QStringLiteral("unit.hfs-depth-two-mutation");

    const QString imagePath = temp.filePath(QStringLiteral("hfs-depth-two.img"));
    QVERIFY(writeBytes(imagePath, hfsSplitReadyFixture()));

    const auto splitCreate = PartitionHfsFileSystemWriter::createEmptyFileFromImage(
        imagePath, QStringLiteral("/split-created.txt"), options);
    QVERIFY2(splitCreate.ok, qPrintable(splitCreate.blockers.join(QStringLiteral("; "))));
    QVERIFY(splitCreate.warnings.join(' ').contains(QStringLiteral("split into two leaf nodes")));

    // Post-split create that sorts before every existing record: exercises the
    // first-leaf insert plus the root index-key update for the left child.
    const auto frontCreate = PartitionHfsFileSystemWriter::createEmptyFileFromImage(
        imagePath, QStringLiteral("/aaa-front.txt"), options);
    QVERIFY2(frontCreate.ok, qPrintable(frontCreate.blockers.join(QStringLiteral("; "))));
    const auto frontReadBack = PartitionHfsFileSystemReader::readFileFromImage(
        imagePath, QStringLiteral("/aaa-front.txt"), 1);
    QVERIFY2(frontReadBack.ok, qPrintable(frontReadBack.blockers.join(QStringLiteral("; "))));

    // Fill the rightmost leaf until a depth-2 leaf split produces a third leaf.
    for (int index = 0; index < 14; ++index) {
        const QString name = QStringLiteral("/zzz-%1.txt").arg(index, 2, 10, QLatin1Char('0'));
        const auto created =
            PartitionHfsFileSystemWriter::createEmptyFileFromImage(imagePath, name, options);
        QVERIFY2(created.ok, qPrintable(created.blockers.join(QStringLiteral("; "))));
    }
    const QByteArray afterThirdLeaf = readBytes(imagePath);
    const qsizetype headerOffset =
        static_cast<qsizetype>(kTestHfsCatalogStartBlock * kTestHfsBlockSize) +
        kTestHfsBTreeHeaderRecordOffset;
    QCOMPARE(readBe16(afterThirdLeaf, headerOffset + kTestHfsBTreeHeaderTreeDepthOffset), 2);
    const uint32_t firstLeaf = readBe32(afterThirdLeaf,
                                        headerOffset + kTestHfsBTreeHeaderFirstLeafNodeOffset);
    int chainLeaves = 0;
    uint32_t chainNode = firstLeaf;
    while (chainNode != 0 && chainLeaves < 8) {
        ++chainLeaves;
        const qsizetype leafOffset =
            static_cast<qsizetype>((kTestHfsCatalogStartBlock + chainNode) * kTestHfsBlockSize);
        chainNode = readBe32(afterThirdLeaf, leafOffset);
    }
    QVERIFY(chainLeaves >= 3);

    const auto consistency = PartitionHfsFileSystemReader::checkConsistencyFromImage(imagePath,
                                                                                     400);
    QVERIFY2(consistency.ok, qPrintable(consistency.blockers.join(QStringLiteral("; "))));
    QVERIFY(!consistency.warnings.join(' ').contains(QStringLiteral("record count mismatch")));

    // Depth-2 delete and rename, including a cross-leaf move.
    const auto deleted = PartitionHfsFileSystemWriter::deleteEmptyFileFromImage(
        imagePath, QStringLiteral("/zzz-00.txt"), options);
    QVERIFY2(deleted.ok, qPrintable(deleted.blockers.join(QStringLiteral("; "))));
    const auto deletedReadBack = PartitionHfsFileSystemReader::readFileFromImage(
        imagePath, QStringLiteral("/zzz-00.txt"), 1);
    QVERIFY(!deletedReadBack.ok);

    const auto renamed = PartitionHfsFileSystemWriter::renameOrMoveCatalogEntryFromImage(
        imagePath, QStringLiteral("/zzz-01.txt"), QStringLiteral("/aaa-moved.txt"), options);
    QVERIFY2(renamed.ok, qPrintable(renamed.blockers.join(QStringLiteral("; "))));
    const auto renamedReadBack = PartitionHfsFileSystemReader::readFileFromImage(
        imagePath, QStringLiteral("/aaa-moved.txt"), 1);
    QVERIFY2(renamedReadBack.ok, qPrintable(renamedReadBack.blockers.join(QStringLiteral("; "))));

    // Depth-2 folder lifecycle.
    const auto folderCreated = PartitionHfsFileSystemWriter::createEmptyFolderFromImage(
        imagePath, QStringLiteral("/Depth Two Folder"), options);
    QVERIFY2(folderCreated.ok, qPrintable(folderCreated.blockers.join(QStringLiteral("; "))));
    const auto childCreated = PartitionHfsFileSystemWriter::createFileWithDataFromImage(
        imagePath,
        QStringLiteral("/Depth Two Folder/nested.bin"),
        QByteArrayLiteral("depth two nested payload\n"),
        options);
    QVERIFY2(childCreated.ok, qPrintable(childCreated.blockers.join(QStringLiteral("; "))));
    const auto treeDeleted =
        PartitionHfsFileSystemWriter::deleteFolderTreeAndReleaseAllocatedBlocksFromImage(
            imagePath, QStringLiteral("/Depth Two Folder"), options);
    QVERIFY2(treeDeleted.ok, qPrintable(treeDeleted.blockers.join(QStringLiteral("; "))));

    // Drain every file; emptied leaves are removed and the tree collapses back
    // to a one-level catalog with the freed nodes erased.
    const auto rootListing =
        PartitionHfsFileSystemReader::listDirectoryFromImage(imagePath, QStringLiteral("/"), 100);
    QVERIFY2(rootListing.ok, qPrintable(rootListing.blockers.join(QStringLiteral("; "))));
    for (const auto& entry : rootListing.entries) {
        const auto drained = PartitionHfsFileSystemWriter::deleteEmptyFileFromImage(
            imagePath, QStringLiteral("/%1").arg(entry.name), options);
        QVERIFY2(drained.ok,
                 qPrintable(QStringLiteral("%1: %2").arg(
                     entry.name, drained.blockers.join(QStringLiteral("; ")))));
    }
    const QByteArray afterDrain = readBytes(imagePath);
    QCOMPARE(readBe16(afterDrain, headerOffset + kTestHfsBTreeHeaderTreeDepthOffset), 1);
    QCOMPARE(readBe32(afterDrain, headerOffset + kTestHfsBTreeHeaderLeafRecordsOffset), 0U);
    QCOMPARE(readBe32(afterDrain, headerOffset + kTestHfsBTreeHeaderFreeNodesOffset),
             static_cast<uint32_t>(kTestHfsSplitCatalogTotalNodes - 2));
    const auto emptyListing =
        PartitionHfsFileSystemReader::listDirectoryFromImage(imagePath, QStringLiteral("/"), 10);
    QVERIFY2(emptyListing.ok, qPrintable(emptyListing.blockers.join(QStringLiteral("; "))));
    QVERIFY(emptyListing.entries.isEmpty());

    const auto finalConsistency = PartitionHfsFileSystemReader::checkConsistencyFromImage(imagePath,
                                                                                          400);
    QVERIFY2(finalConsistency.ok, qPrintable(finalConsistency.blockers.join(QStringLiteral("; "))));
}

void PartitionManagerCoreTests::hfsFileSystemWriter_mutatesDepthThreeCatalogs() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    PartitionHfsFileWriteOptions options;
    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.allow_journaled_volume = true;
    options.evidence_id = QStringLiteral("unit.hfs-depth-three-mutation");

    const QString imagePath = temp.filePath(QStringLiteral("hfs-depth-three.img"));
    QVERIFY(writeBytes(imagePath, hfsDeepCatalogFixture()));

    const qsizetype headerOffset = static_cast<qsizetype>(kTestHfsDeepCatalogStartBlock) *
                                       kTestHfsBlockSize +
                                   kTestHfsBTreeHeaderRecordOffset;
    const auto treeDepth = [&]() {
        return readBe16(readBytes(imagePath), headerOffset + kTestHfsBTreeHeaderTreeDepthOffset);
    };

    // Create until the root index node overflows and the tree reaches depth 3.
    // Long names shrink both leaf and index fanout so the third level arrives
    // within the fixture's node budget.
    const QString namePad(48, QLatin1Char('x'));
    int created = 0;
    while (treeDepth() < 3 && created < 800) {
        const QString name =
            QStringLiteral("/zz-%1-%2.txt").arg(created, 4, 10, QLatin1Char('0')).arg(namePad);
        const auto result =
            PartitionHfsFileSystemWriter::createEmptyFileFromImage(imagePath, name, options);
        QVERIFY2(result.ok,
                 qPrintable(QStringLiteral("create %1: %2")
                                .arg(created)
                                .arg(result.blockers.join(QStringLiteral("; ")))));
        ++created;
    }
    QCOMPARE(treeDepth(), 3);
    QVERIFY(created > 100);

    const auto consistency = PartitionHfsFileSystemReader::checkConsistencyFromImage(imagePath,
                                                                                     4000);
    QVERIFY2(consistency.ok, qPrintable(consistency.blockers.join(QStringLiteral("; "))));

    const auto listing =
        PartitionHfsFileSystemReader::listDirectoryFromImage(imagePath, QStringLiteral("/"), 2000);
    QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(listing.entries.size(), created + kTestHfsDeepFixtureFileCount);

    // Depth-3 rename and delete still work.
    const auto renamed = PartitionHfsFileSystemWriter::renameOrMoveCatalogEntryFromImage(
        imagePath,
        QStringLiteral("/zz-0000-%1.txt").arg(namePad),
        QStringLiteral("/aa-renamed.txt"),
        options);
    QVERIFY2(renamed.ok, qPrintable(renamed.blockers.join(QStringLiteral("; "))));
    const auto renamedReadBack = PartitionHfsFileSystemReader::readFileFromImage(
        imagePath, QStringLiteral("/aa-renamed.txt"), 1);
    QVERIFY2(renamedReadBack.ok, qPrintable(renamedReadBack.blockers.join(QStringLiteral("; "))));

    // Drain everything: the tree must collapse 3 -> 2 -> 1 and stay readable.
    const auto drainListing =
        PartitionHfsFileSystemReader::listDirectoryFromImage(imagePath, QStringLiteral("/"), 2000);
    QVERIFY(drainListing.ok);
    for (const auto& entry : drainListing.entries) {
        const auto deleted = PartitionHfsFileSystemWriter::deleteEmptyFileFromImage(
            imagePath, QStringLiteral("/%1").arg(entry.name), options);
        QVERIFY2(deleted.ok,
                 qPrintable(QStringLiteral("%1: %2").arg(
                     entry.name, deleted.blockers.join(QStringLiteral("; ")))));
    }
    QCOMPARE(treeDepth(), 1);
    const QByteArray drained = readBytes(imagePath);
    QCOMPARE(readBe32(drained, headerOffset + kTestHfsBTreeHeaderLeafRecordsOffset), 0U);
    QCOMPARE(readBe32(drained, headerOffset + kTestHfsBTreeHeaderFreeNodesOffset),
             kTestHfsDeepCatalogTotalNodes - 2);
    const auto finalConsistency = PartitionHfsFileSystemReader::checkConsistencyFromImage(imagePath,
                                                                                          4000);
    QVERIFY2(finalConsistency.ok, qPrintable(finalConsistency.blockers.join(QStringLiteral("; "))));
}

void PartitionManagerCoreTests::hfsFileSystemWriter_mergesUnderfullCatalogLeavesOnDelete() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    PartitionHfsFileWriteOptions options;
    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.allow_journaled_volume = true;
    options.evidence_id = QStringLiteral("unit.hfs-underfull-leaf-merge");

    const QString imagePath = temp.filePath(QStringLiteral("hfs-merge.img"));
    QVERIFY(writeBytes(imagePath, hfsDeepCatalogFixture()));

    const qsizetype headerOffset = static_cast<qsizetype>(kTestHfsDeepCatalogStartBlock) *
                                       kTestHfsBlockSize +
                                   kTestHfsBTreeHeaderRecordOffset;
    const auto treeDepth = [&]() {
        return readBe16(readBytes(imagePath), headerOffset + kTestHfsBTreeHeaderTreeDepthOffset);
    };
    const auto freeNodes = [&]() {
        return readBe32(readBytes(imagePath), headerOffset + kTestHfsBTreeHeaderFreeNodesOffset);
    };

    // Grow the catalog to a multi-leaf depth-2 tree.
    const QString namePad(48, QLatin1Char('x'));
    QStringList created;
    while ((treeDepth() < 2 || created.size() < 20) && created.size() < 400) {
        const QString name = QStringLiteral("/m-%1-%2.txt")
                                 .arg(created.size(), 4, 10, QLatin1Char('0'))
                                 .arg(namePad);
        const auto result =
            PartitionHfsFileSystemWriter::createEmptyFileFromImage(imagePath, name, options);
        QVERIFY2(result.ok, qPrintable(result.blockers.join(QStringLiteral("; "))));
        created.append(name);
    }
    QVERIFY(treeDepth() >= 2);

    // Delete everything except the extreme keys. The middle leaves empty out and
    // are freed; the surviving first and last leaves are each left holding a
    // single underfull record and become adjacent. Without delete-side merge
    // they would remain two separate leaves under a root index node (depth 2);
    // the merge coalesces them and the whole set of survivors into one leaf, so
    // the tree must collapse to depth 1 with only the header and root-leaf nodes
    // in use.
    for (int index = 1; index + 1 < created.size(); ++index) {
        const auto deleted = PartitionHfsFileSystemWriter::deleteEmptyFileFromImage(
            imagePath, created.at(index), options);
        QVERIFY2(deleted.ok, qPrintable(deleted.blockers.join(QStringLiteral("; "))));
    }

    QCOMPARE(treeDepth(), static_cast<uint16_t>(1));
    QCOMPARE(freeNodes(), kTestHfsDeepCatalogTotalNodes - 2);

    const auto listing =
        PartitionHfsFileSystemReader::listDirectoryFromImage(imagePath, QStringLiteral("/"), 2000);
    QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(listing.entries.size(), 2 + kTestHfsDeepFixtureFileCount);
    for (const QString& survivor : {created.first(), created.last()}) {
        const auto readBack =
            PartitionHfsFileSystemReader::readFileFromImage(imagePath, survivor, 1);
        QVERIFY2(readBack.ok, qPrintable(readBack.blockers.join(QStringLiteral("; "))));
    }

    const auto consistency = PartitionHfsFileSystemReader::checkConsistencyFromImage(imagePath,
                                                                                     4000);
    QVERIFY2(consistency.ok, qPrintable(consistency.blockers.join(QStringLiteral("; "))));
}

void PartitionManagerCoreTests::hfsFileSystemWriter_survivesRandomizedCreateDeleteChurn() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    PartitionHfsFileWriteOptions options;
    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.allow_journaled_volume = true;
    options.evidence_id = QStringLiteral("unit.hfs-create-delete-churn");

    const QString imagePath = temp.filePath(QStringLiteral("hfs-churn.img"));
    QVERIFY(writeBytes(imagePath, hfsDeepCatalogFixture()));

    const qsizetype headerOffset = static_cast<qsizetype>(kTestHfsDeepCatalogStartBlock) *
                                       kTestHfsBlockSize +
                                   kTestHfsBTreeHeaderRecordOffset;
    const auto treeDepth = [&]() {
        return readBe16(readBytes(imagePath), headerOffset + kTestHfsBTreeHeaderTreeDepthOffset);
    };

    // Deterministic LCG so the churn is reproducible across runs and platforms.
    uint32_t rng = 0x12'34'56'7Bu;
    const auto nextRandom = [&]() {
        rng = rng * 1'664'525u + 1'013'904'223u;
        return rng;
    };

    const QString namePad(32, QLatin1Char('z'));
    QStringList live;
    uint32_t createCounter = 0;
    uint16_t peakDepth = treeDepth();

    const auto verifyTree = [&]() {
        const auto listing = PartitionHfsFileSystemReader::listDirectoryFromImage(
            imagePath, QStringLiteral("/"), 4000);
        QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));
        QCOMPARE(listing.entries.size(), live.size() + kTestHfsDeepFixtureFileCount);
        const auto consistency = PartitionHfsFileSystemReader::checkConsistencyFromImage(imagePath,
                                                                                         8000);
        QVERIFY2(consistency.ok, qPrintable(consistency.blockers.join(QStringLiteral("; "))));
    };

    constexpr int kChurnOperations = 300;
    for (int op = 0; op < kChurnOperations; ++op) {
        // Keep the live set oscillating across the depth-2 split/merge boundary
        // so both directions of the rebalance get exercised repeatedly.
        const bool doCreate = live.size() < 8 || (live.size() <= 90 && (nextRandom() & 1u) == 0u);
        if (doCreate) {
            const QString name = QStringLiteral("/c-%1-%2.txt")
                                     .arg(createCounter++, 6, 10, QLatin1Char('0'))
                                     .arg(namePad);
            const auto created =
                PartitionHfsFileSystemWriter::createEmptyFileFromImage(imagePath, name, options);
            QVERIFY2(created.ok, qPrintable(created.blockers.join(QStringLiteral("; "))));
            live.append(name);
        } else {
            const int index = static_cast<int>(nextRandom() % static_cast<uint32_t>(live.size()));
            const auto deleted = PartitionHfsFileSystemWriter::deleteEmptyFileFromImage(
                imagePath, live.at(index), options);
            QVERIFY2(deleted.ok, qPrintable(deleted.blockers.join(QStringLiteral("; "))));
            live.removeAt(index);
        }
        peakDepth = std::max(peakDepth, treeDepth());
        if ((op % 50) == 49) {
            verifyTree();
        }
    }

    // The churn must have actually grown the tree past a single leaf, otherwise
    // it never exercised the split/merge paths this test is here to cover.
    QVERIFY(peakDepth >= 2);
    verifyTree();

    // Drain the remaining churn files; the tree collapses back to the fixture.
    for (const QString& name : live) {
        const auto deleted =
            PartitionHfsFileSystemWriter::deleteEmptyFileFromImage(imagePath, name, options);
        QVERIFY2(deleted.ok, qPrintable(deleted.blockers.join(QStringLiteral("; "))));
    }
    live.clear();
    QCOMPARE(treeDepth(), static_cast<uint16_t>(1));
    verifyTree();
}

namespace {

uint32_t testJournalChecksum(const QByteArray& data) {
    uint32_t cksum = 0;
    for (const char byte : data) {
        cksum = (cksum << 8) ^ (cksum + static_cast<uint8_t>(byte));
    }
    return ~cksum;
}

void writeTestLe16(QByteArray* bytes, qsizetype offset, uint16_t value) {
    (*bytes)[offset] = static_cast<char>(value & 0xFF);
    (*bytes)[offset + 1] = static_cast<char>((value >> 8) & 0xFF);
}

void writeTestLe64(QByteArray* bytes, qsizetype offset, uint64_t value) {
    for (int index = 0; index < 8; ++index) {
        (*bytes)[offset + index] = static_cast<char>((value >> (8 * index)) & 0xFF);
    }
}

}  // namespace

void PartitionManagerCoreTests::hfsFileSystemWriter_replaysJournalTransactions() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    PartitionHfsFileWriteOptions options;
    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.allow_journaled_volume = true;
    options.evidence_id = QStringLiteral("unit.hfs-journal-replay");

    // Expected end state: the certified direct-write path applies the change.
    QByteArray expectedImage = hfsReaderFixture();
    const QString expectedPath = temp.filePath(QStringLiteral("hfs-journal-expected.img"));
    QVERIFY(writeBytes(expectedPath, expectedImage));
    const QByteArray newContent = QByteArrayLiteral("journal replayed content!\n");
    const auto direct = PartitionHfsFileSystemWriter::overwriteFileSameSizeFromImage(
        expectedPath,
        QStringLiteral("/hello.txt"),
        newContent.leftJustified(15, '\n', true),
        options);
    QVERIFY2(direct.ok, qPrintable(direct.blockers.join(QStringLiteral("; "))));
    const QByteArray expectedBytes = readBytes(expectedPath);

    // Dirty-journal image: the same change captured as a journal transaction
    // (one 4096-byte block) over the unmodified fixture.
    QByteArray dirty = hfsReaderFixture();
    const uint32_t jibBlock = 50;
    const uint64_t journalOffset = 51ULL * kTestHfsBlockSize;
    const uint64_t journalSize = 8ULL * kTestHfsBlockSize;
    writeBe32(&dirty, kTestHfsHeaderOffset + kTestHfsVolumeJournalInfoBlockOffset, jibBlock);
    const qsizetype jibOffset = static_cast<qsizetype>(jibBlock) * kTestHfsBlockSize;
    writeBe32(&dirty, jibOffset, 1);  // kJIJournalInFSMask
    writeBe64(&dirty, jibOffset + 36, journalOffset);
    writeBe64(&dirty, jibOffset + 44, journalSize);

    const qsizetype targetOffset = static_cast<qsizetype>(kTestHfsHelloFileBlock) *
                                   kTestHfsBlockSize;
    const QByteArray replayBlock = expectedBytes.mid(targetOffset, kTestHfsBlockSize);

    constexpr uint64_t kJhdrSize = 512;
    constexpr uint64_t kBlhdrSize = 512;
    QByteArray blhdr(static_cast<qsizetype>(kBlhdrSize), '\0');
    writeTestLe16(&blhdr, 0, 31);  // max_blocks
    writeTestLe16(&blhdr, 2, 2);   // num_blocks (sentinel + 1)
    const uint64_t bytesUsed = kBlhdrSize + kTestHfsBlockSize;
    writeTestLe32(&blhdr, 4, static_cast<uint32_t>(bytesUsed));
    writeTestLe64(&blhdr, 16, std::numeric_limits<uint64_t>::max());  // binfo[0]
    writeTestLe64(&blhdr, 32, static_cast<uint64_t>(targetOffset) / 512);
    writeTestLe32(&blhdr, 40, kTestHfsBlockSize);
    QByteArray blhdrChecksumInput = blhdr.left(32);
    writeTestLe32(&blhdrChecksumInput, 8, 0);
    writeTestLe32(&blhdr, 8, testJournalChecksum(blhdrChecksumInput));

    QByteArray journalHeader(44, '\0');
    writeTestLe32(&journalHeader, 0, 0x4a'4e'4c'78);           // 'JNLx'
    writeTestLe32(&journalHeader, 4, 0x12'34'56'78);           // little-endian marker
    writeTestLe64(&journalHeader, 8, kJhdrSize);               // start
    writeTestLe64(&journalHeader, 16, kJhdrSize + bytesUsed);  // end
    writeTestLe64(&journalHeader, 24, journalSize);
    writeTestLe32(&journalHeader, 32, static_cast<uint32_t>(kBlhdrSize));
    writeTestLe32(&journalHeader, 40, static_cast<uint32_t>(kJhdrSize));
    writeTestLe32(&journalHeader, 36, testJournalChecksum(journalHeader));

    const qsizetype journalBase = static_cast<qsizetype>(journalOffset);
    std::copy(journalHeader.cbegin(), journalHeader.cend(), dirty.begin() + journalBase);
    std::copy(blhdr.cbegin(),
              blhdr.cend(),
              dirty.begin() + journalBase + static_cast<qsizetype>(kJhdrSize));
    std::copy(replayBlock.cbegin(),
              replayBlock.cend(),
              dirty.begin() + journalBase + static_cast<qsizetype>(kJhdrSize + kBlhdrSize));

    const QString dirtyPath = temp.filePath(QStringLiteral("hfs-journal-dirty.img"));
    QVERIFY(writeBytes(dirtyPath, dirty));

    const auto replayed = PartitionHfsFileSystemWriter::replayJournalFromImage(dirtyPath, options);
    QVERIFY2(replayed.ok, qPrintable(replayed.blockers.join(QStringLiteral("; "))));
    QVERIFY(replayed.warnings.join(' ').contains(
        QStringLiteral("journal replayed: 1 transactions, 1 blocks")));

    // The replayed data region must byte-match the certified direct write.
    const QByteArray afterReplay = readBytes(dirtyPath);
    QCOMPARE(afterReplay.mid(targetOffset, kTestHfsBlockSize),
             expectedBytes.mid(targetOffset, kTestHfsBlockSize));
    const auto readBack = PartitionHfsFileSystemReader::readFileFromImage(
        dirtyPath, QStringLiteral("/hello.txt"), 4096);
    QVERIFY2(readBack.ok, qPrintable(readBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(readBack.data, newContent.leftJustified(15, '\n', true));

    // Journal header is now clean (start == end, checksum valid).
    const qsizetype headerAt = static_cast<qsizetype>(journalOffset);
    const QByteArray headerAfter = afterReplay.mid(headerAt, 44);
    QCOMPARE(headerAfter.mid(8, 8), headerAfter.mid(16, 8));

    // Replaying a clean journal is a no-op.
    const auto cleanReplay = PartitionHfsFileSystemWriter::replayJournalFromImage(dirtyPath,
                                                                                  options);
    QVERIFY2(cleanReplay.ok, qPrintable(cleanReplay.blockers.join(QStringLiteral("; "))));
    QVERIFY(cleanReplay.warnings.join(' ').contains(QStringLiteral("journal is clean")));

    // Tampered transaction checksum fails closed without writing.
    QByteArray tampered = dirty;
    tampered[journalBase + static_cast<qsizetype>(kJhdrSize) + 20] =
        static_cast<char>(tampered.at(journalBase + static_cast<qsizetype>(kJhdrSize) + 20) ^ 0x5A);
    const QString tamperedPath = temp.filePath(QStringLiteral("hfs-journal-tampered.img"));
    QVERIFY(writeBytes(tamperedPath, tampered));
    const auto blocked = PartitionHfsFileSystemWriter::replayJournalFromImage(tamperedPath,
                                                                              options);
    QVERIFY(!blocked.ok);
    QVERIFY(blocked.blockers.join(' ').contains(QStringLiteral("transaction checksum")));
    QCOMPARE(readBytes(tamperedPath), tampered);

    // Non-journaled volumes are blocked.
    const QString plainPath = temp.filePath(QStringLiteral("hfs-journal-plain.img"));
    QByteArray plain = hfsReaderFixture();
    writeBe32(&plain, kTestHfsHeaderOffset + kTestHfsAttributesOffset, 0);
    QVERIFY(writeBytes(plainPath, plain));
    const auto notJournaled = PartitionHfsFileSystemWriter::replayJournalFromImage(plainPath,
                                                                                   options);
    QVERIFY(!notJournaled.ok);
    QVERIFY(notJournaled.blockers.join(' ').contains(QStringLiteral("not journaled")));
}

void PartitionManagerCoreTests::hfsFileSystemWriter_replaysBigEndianJournal() {
    // H-g: a PowerPC-era HFS+ journal stores every journal header / block-list /
    // block-info field big-endian (the endian marker selects the byte order). The
    // replay must produce the byte-identical result of the certified little-endian
    // direct-write path. No Apple big-endian rig exists, so this byte-equality is
    // the H-g exit gate.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    PartitionHfsFileWriteOptions options;
    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.allow_journaled_volume = true;
    options.evidence_id = QStringLiteral("unit.hfs-journal-replay-be");

    // Expected end state via the certified direct-write path (endian-independent).
    const QString expectedPath = temp.filePath(QStringLiteral("hfs-bejournal-expected.img"));
    QVERIFY(writeBytes(expectedPath, hfsReaderFixture()));
    const QByteArray newContent = QByteArrayLiteral("big-endian journal replay!\n");
    const auto direct = PartitionHfsFileSystemWriter::overwriteFileSameSizeFromImage(
        expectedPath,
        QStringLiteral("/hello.txt"),
        newContent.leftJustified(15, '\n', true),
        options);
    QVERIFY2(direct.ok, qPrintable(direct.blockers.join(QStringLiteral("; "))));
    const QByteArray expectedBytes = readBytes(expectedPath);

    // Dirty image carrying the same change as ONE big-endian journal transaction.
    QByteArray dirty = hfsReaderFixture();
    const uint32_t jibBlock = 50;
    const uint64_t journalOffset = 51ULL * kTestHfsBlockSize;
    const uint64_t journalSize = 8ULL * kTestHfsBlockSize;
    writeBe32(&dirty, kTestHfsHeaderOffset + kTestHfsVolumeJournalInfoBlockOffset, jibBlock);
    const qsizetype jibOffset = static_cast<qsizetype>(jibBlock) * kTestHfsBlockSize;
    writeBe32(&dirty, jibOffset, 1);  // kJIJournalInFSMask (JIB is always big-endian)
    writeBe64(&dirty, jibOffset + 36, journalOffset);
    writeBe64(&dirty, jibOffset + 44, journalSize);

    const qsizetype targetOffset = static_cast<qsizetype>(kTestHfsHelloFileBlock) *
                                   kTestHfsBlockSize;
    const QByteArray replayBlock = expectedBytes.mid(targetOffset, kTestHfsBlockSize);

    constexpr uint64_t kJhdrSize = 512;
    constexpr uint64_t kBlhdrSize = 512;
    const uint64_t bytesUsed = kBlhdrSize + kTestHfsBlockSize;

    // Block-list header + block-info, all big-endian.
    QByteArray blhdr(static_cast<qsizetype>(kBlhdrSize), '\0');
    writeBe16(&blhdr, 0, 31);                                     // max_blocks
    writeBe16(&blhdr, 2, 2);                                      // num_blocks (sentinel + 1)
    writeBe32(&blhdr, 4, static_cast<uint32_t>(bytesUsed));
    writeBe64(&blhdr, 16, std::numeric_limits<uint64_t>::max());  // binfo[0] sentinel
    writeBe64(&blhdr, 32, static_cast<uint64_t>(targetOffset) / 512);
    writeBe32(&blhdr, 40, kTestHfsBlockSize);
    QByteArray blhdrChecksumInput = blhdr.left(32);
    writeBe32(&blhdrChecksumInput, 8, 0);
    writeBe32(&blhdr, 8, testJournalChecksum(blhdrChecksumInput));

    // Journal header, all big-endian (endian marker stored as bytes 12 34 56 78).
    QByteArray journalHeader(44, '\0');
    writeBe32(&journalHeader, 0, 0x4a'4e'4c'78);           // 'JNLx'
    writeBe32(&journalHeader, 4, 0x12'34'56'78);           // endian marker (big-endian on disk)
    writeBe64(&journalHeader, 8, kJhdrSize);               // start
    writeBe64(&journalHeader, 16, kJhdrSize + bytesUsed);  // end
    writeBe64(&journalHeader, 24, journalSize);
    writeBe32(&journalHeader, 32, static_cast<uint32_t>(kBlhdrSize));
    writeBe32(&journalHeader, 40, static_cast<uint32_t>(kJhdrSize));
    writeBe32(&journalHeader, 36, testJournalChecksum(journalHeader));

    const qsizetype journalBase = static_cast<qsizetype>(journalOffset);
    std::copy(journalHeader.cbegin(), journalHeader.cend(), dirty.begin() + journalBase);
    std::copy(blhdr.cbegin(),
              blhdr.cend(),
              dirty.begin() + journalBase + static_cast<qsizetype>(kJhdrSize));
    std::copy(replayBlock.cbegin(),
              replayBlock.cend(),
              dirty.begin() + journalBase + static_cast<qsizetype>(kJhdrSize + kBlhdrSize));

    const QString dirtyPath = temp.filePath(QStringLiteral("hfs-bejournal-dirty.img"));
    QVERIFY(writeBytes(dirtyPath, dirty));

    const auto replayed = PartitionHfsFileSystemWriter::replayJournalFromImage(dirtyPath, options);
    QVERIFY2(replayed.ok, qPrintable(replayed.blockers.join(QStringLiteral("; "))));
    QVERIFY(replayed.warnings.join(' ').contains(
        QStringLiteral("journal replayed: 1 transactions, 1 blocks")));

    // Byte-equality vs the certified direct-write path -- the H-g exit gate.
    const QByteArray afterReplay = readBytes(dirtyPath);
    QCOMPARE(afterReplay.mid(targetOffset, kTestHfsBlockSize),
             expectedBytes.mid(targetOffset, kTestHfsBlockSize));
    const auto readBack = PartitionHfsFileSystemReader::readFileFromImage(
        dirtyPath, QStringLiteral("/hello.txt"), 4096);
    QVERIFY2(readBack.ok, qPrintable(readBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(readBack.data, newContent.leftJustified(15, '\n', true));

    // The big-endian journal header is now clean (start == end) and a re-replay no-ops.
    const QByteArray headerAfter = afterReplay.mid(journalBase, 44);
    QCOMPARE(headerAfter.mid(8, 8), headerAfter.mid(16, 8));
    const auto cleanReplay = PartitionHfsFileSystemWriter::replayJournalFromImage(dirtyPath,
                                                                                  options);
    QVERIFY2(cleanReplay.ok, qPrintable(cleanReplay.blockers.join(QStringLiteral("; "))));
    QVERIFY(cleanReplay.warnings.join(' ').contains(QStringLiteral("journal is clean")));

    // A tampered big-endian transaction checksum fails closed without writing.
    QByteArray tampered = dirty;
    tampered[journalBase + static_cast<qsizetype>(kJhdrSize) + 20] =
        static_cast<char>(tampered.at(journalBase + static_cast<qsizetype>(kJhdrSize) + 20) ^ 0x5A);
    const QString tamperedPath = temp.filePath(QStringLiteral("hfs-bejournal-tampered.img"));
    QVERIFY(writeBytes(tamperedPath, tampered));
    const auto blocked = PartitionHfsFileSystemWriter::replayJournalFromImage(tamperedPath,
                                                                              options);
    QVERIFY(!blocked.ok);
    QVERIFY(blocked.blockers.join(' ').contains(QStringLiteral("transaction checksum")));
    QCOMPARE(readBytes(tamperedPath), tampered);
}

void PartitionManagerCoreTests::hfsFileSystemWriter_writesIntoWrappedVolume() {
    // H-h: an HFS+ volume embedded inside a legacy HFS wrapper. Every I/O is offset
    // by the embedded volume's start; a write must land in the embedded volume and
    // round-trip, leave the wrapper MDB untouched, and stay fail-closed without the
    // explicit wrapper override.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    // Embed a complete writable HFS+ volume at the wrapper's embedded offset and put
    // an HFS wrapper master directory block (signature 'BD') at offset 1024.
    const qsizetype embeddedOffset =
        static_cast<qsizetype>(kTestHfsWrapperAllocationStartSector * 512ULL +
                               static_cast<uint64_t>(kTestHfsWrapperEmbeddedStartBlock) *
                                   kTestHfsWrapperAllocationBlockSize);
    const QByteArray embedded = hfsReaderFixture();
    QByteArray wrapped(embeddedOffset + embedded.size() + kTestHfsBlockSize, '\0');
    std::copy(embedded.cbegin(), embedded.cend(), wrapped.begin() + embeddedOffset);
    writeAscii(&wrapped, kTestHfsWrapperMdbOffset, "BD");
    writeBe32(&wrapped,
              kTestHfsWrapperMdbOffset + kTestHfsWrapperAllocationBlockSizeOffset,
              kTestHfsWrapperAllocationBlockSize);
    writeBe16(&wrapped,
              kTestHfsWrapperMdbOffset + kTestHfsWrapperAllocationBlockStartOffset,
              kTestHfsWrapperAllocationStartSector);
    writeAscii(&wrapped, kTestHfsWrapperMdbOffset + kTestHfsWrapperEmbeddedSignatureOffset, "H+");
    writeBe16(&wrapped,
              kTestHfsWrapperMdbOffset + kTestHfsWrapperEmbeddedExtentStartOffset,
              kTestHfsWrapperEmbeddedStartBlock);
    writeBe16(&wrapped,
              kTestHfsWrapperMdbOffset + kTestHfsWrapperEmbeddedExtentCountOffset,
              kTestHfsWrapperEmbeddedBlockCount);

    const QString wrappedPath = temp.filePath(QStringLiteral("hfs-wrapped.img"));
    QVERIFY(writeBytes(wrappedPath, wrapped));

    PartitionHfsFileWriteOptions options;
    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.allow_journaled_volume = true;
    options.evidence_id = QStringLiteral("unit.hfs-wrapped-write");
    const QByteArray newContent = QByteArrayLiteral("wrapped!\n").leftJustified(15, '\n', true);

    // Fail-closed without the wrapper override; the image is untouched.
    const auto blocked = PartitionHfsFileSystemWriter::overwriteFileSameSizeFromImage(
        wrappedPath, QStringLiteral("/hello.txt"), newContent, options);
    QVERIFY(!blocked.ok);
    QVERIFY(blocked.blockers.join(' ').contains(QStringLiteral("Wrapped HFS+ volume writes")));
    QCOMPARE(readBytes(wrappedPath), wrapped);

    // With the override the write lands in the embedded volume and round-trips.
    options.allow_wrapped_volume = true;
    const auto written = PartitionHfsFileSystemWriter::overwriteFileSameSizeFromImage(
        wrappedPath, QStringLiteral("/hello.txt"), newContent, options);
    QVERIFY2(written.ok, qPrintable(written.blockers.join(QStringLiteral("; "))));

    const auto readBack = PartitionHfsFileSystemReader::readFileFromImage(
        wrappedPath, QStringLiteral("/hello.txt"), 4096);
    QVERIFY2(readBack.ok, qPrintable(readBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(readBack.data, newContent);

    // The mutation landed INSIDE the embedded volume (offset by the wrapper start),
    // and the wrapper MDB at offset 1024 is untouched.
    const QByteArray after = readBytes(wrappedPath);
    QCOMPARE(after.mid(kTestHfsWrapperMdbOffset, 2), QByteArrayLiteral("BD"));
    const qsizetype dataAt = embeddedOffset +
                             static_cast<qsizetype>(kTestHfsHelloFileBlock) * kTestHfsBlockSize;
    QVERIFY(after.mid(dataAt, 8) == QByteArrayLiteral("wrapped!"));
    // Nothing was written at the non-wrapped (offset-0) data position.
    QVERIFY(after.mid(static_cast<qsizetype>(kTestHfsHelloFileBlock) * kTestHfsBlockSize, 8) !=
            QByteArrayLiteral("wrapped!"));
}

void PartitionManagerCoreTests::hfsFileSystemWriter_growsCatalogNodePoolOrFailsClosed() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    PartitionHfsFileWriteOptions options;
    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.allow_journaled_volume = true;
    options.evidence_id = QStringLiteral("unit.hfs-catalog-node-pool-growth");

    // H-a: a catalog split that exhausts the node pool now GROWS the catalog
    // B-tree file (allocation bitmap + node map + counts) instead of failing.
    HfsSplitFixtureOptions lowFreeNodes;
    lowFreeNodes.free_nodes = 1;
    const QString growPath = temp.filePath(QStringLiteral("hfs-split-grow.img"));
    QVERIFY(writeBytes(growPath, hfsSplitReadyFixture(lowFreeNodes)));
    const qsizetype catalogHeader = static_cast<qsizetype>(kTestHfsCatalogStartBlock) *
                                        kTestHfsBlockSize +
                                    kTestHfsBTreeHeaderRecordOffset;
    const QByteArray growBefore = readBytes(growPath);
    const uint32_t totalNodesBefore = readBe32(growBefore,
                                               catalogHeader + kTestHfsBTreeHeaderTotalNodesOffset);
    const uint32_t freeBlocksBefore = readBe32(growBefore,
                                               kTestHfsHeaderOffset + kTestHfsFreeBlocksOffset);
    const auto grown = PartitionHfsFileSystemWriter::createEmptyFileFromImage(
        growPath, QStringLiteral("/split-created.txt"), options);
    QVERIFY2(grown.ok, qPrintable(grown.blockers.join(QStringLiteral("; "))));
    const QByteArray growAfter = readBytes(growPath);
    QVERIFY(readBe32(growAfter, catalogHeader + kTestHfsBTreeHeaderTotalNodesOffset) >
            totalNodesBefore);
    QVERIFY(readBe32(growAfter, kTestHfsHeaderOffset + kTestHfsFreeBlocksOffset) <
            freeBlocksBefore);
    const auto growConsistency = PartitionHfsFileSystemReader::checkConsistencyFromImage(growPath,
                                                                                         200);
    QVERIFY2(growConsistency.ok, qPrintable(growConsistency.blockers.join(QStringLiteral("; "))));
    const auto growListing =
        PartitionHfsFileSystemReader::listDirectoryFromImage(growPath, QStringLiteral("/"), 200);
    QVERIFY2(growListing.ok, qPrintable(growListing.blockers.join(QStringLiteral("; "))));
    bool foundCreated = false;
    for (const auto& entry : growListing.entries) {
        if (entry.name == QStringLiteral("split-created.txt")) {
            foundCreated = true;
        }
    }
    QVERIFY(foundCreated);

    // Fail closed when the node pool is exhausted AND there are no free blocks
    // to grow into; the image must be left untouched.
    HfsSplitFixtureOptions noFreeBlocks;
    noFreeBlocks.free_nodes = 1;
    noFreeBlocks.full_volume = true;
    const QString noBlocksPath = temp.filePath(QStringLiteral("hfs-split-no-free-blocks.img"));
    QVERIFY(writeBytes(noBlocksPath, hfsSplitReadyFixture(noFreeBlocks)));
    const QByteArray noBlocksBefore = readBytes(noBlocksPath);
    const auto noBlocksBlocked = PartitionHfsFileSystemWriter::createEmptyFileFromImage(
        noBlocksPath, QStringLiteral("/split-created.txt"), options);
    QVERIFY(!noBlocksBlocked.ok);
    QCOMPARE(readBytes(noBlocksPath), noBlocksBefore);

    HfsSplitFixtureOptions fixedIndexKeys;
    fixedIndexKeys.attributes_mask = kTestHfsBTreeBigKeysMask;
    const QString fixedKeysPath = temp.filePath(QStringLiteral("hfs-split-fixed-index-keys.img"));
    QVERIFY(writeBytes(fixedKeysPath, hfsSplitReadyFixture(fixedIndexKeys)));
    const QByteArray fixedKeysBefore = readBytes(fixedKeysPath);
    const auto fixedKeysBlocked = PartitionHfsFileSystemWriter::createEmptyFileFromImage(
        fixedKeysPath, QStringLiteral("/split-created.txt"), options);
    QVERIFY(!fixedKeysBlocked.ok);
    QVERIFY(
        fixedKeysBlocked.blockers.join(' ').contains(QStringLiteral("variable-length index keys")));
    QCOMPARE(readBytes(fixedKeysPath), fixedKeysBefore);

    HfsSplitFixtureOptions inconsistentMap;
    inconsistentMap.leaf_map_bit = false;
    const QString badMapPath = temp.filePath(QStringLiteral("hfs-split-bad-map.img"));
    QVERIFY(writeBytes(badMapPath, hfsSplitReadyFixture(inconsistentMap)));
    const QByteArray badMapBefore = readBytes(badMapPath);
    const auto badMapBlocked = PartitionHfsFileSystemWriter::createEmptyFileFromImage(
        badMapPath, QStringLiteral("/split-created.txt"), options);
    QVERIFY(!badMapBlocked.ok);
    QVERIFY(badMapBlocked.blockers.join(' ').contains(
        QStringLiteral("inconsistent with allocated nodes")));
    QCOMPARE(readBytes(badMapPath), badMapBefore);
}

void PartitionManagerCoreTests::hfsFileSystemWriter_growsAttributesNodePoolOnRootLeafSplit() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    PartitionHfsFileWriteOptions options;
    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.allow_journaled_volume = true;
    options.evidence_id = QStringLiteral("unit.hfs-attributes-node-pool-growth");

    const QString imagePath = temp.filePath(QStringLiteral("hfs-attr-grow.img"));
    QVERIFY(writeBytes(imagePath, hfsReaderAttributeFixture()));
    const qsizetype attrHeader = static_cast<qsizetype>(kTestHfsAttributesStartBlock) *
                                     kTestHfsBlockSize +
                                 kTestHfsBTreeHeaderRecordOffset;

    // H-f: the streaming attributes B-tree engine grows the tree to arbitrary
    // depth/width. Every inline create succeeds (no single-leaf cap), splitting
    // leaves and growing the node pool as needed; the tree reaches depth >= 2.
    const QByteArray value(96, 'a');
    const int created = 50;
    for (int index = 0; index < created; ++index) {
        const auto result = PartitionHfsFileSystemWriter::createInlineAttributeValueFromImage(
            imagePath,
            18,
            QStringLiteral("org.sak.attr%1").arg(index, 4, 10, QLatin1Char('0')),
            value,
            options);
        QVERIFY2(result.ok,
                 qPrintable(QStringLiteral("attr%1: %2")
                                .arg(index)
                                .arg(result.blockers.join(QStringLiteral("; ")))));
    }
    const uint16_t depth = readBe16(readBytes(imagePath),
                                    attrHeader + kTestHfsBTreeHeaderTreeDepthOffset);
    const uint32_t leafRecords = readBe32(readBytes(imagePath),
                                          attrHeader + kTestHfsBTreeHeaderLeafRecordsOffset);
    QVERIFY2(depth >= 2,
             qPrintable(QStringLiteral("attributes tree depth %1 is not multi-leaf").arg(depth)));
    QVERIFY2(leafRecords >= static_cast<uint32_t>(created),
             qPrintable(
                 QStringLiteral("leafRecords %1 < created %2").arg(leafRecords).arg(created)));

    // Every attribute reads back across the multi-leaf tree (the reader walks the
    // leaf fLink chain).
    for (int index : {0, created / 2, created - 1}) {
        const auto readBack = PartitionHfsFileSystemReader::readAttributeValueFromImage(
            imagePath,
            18,
            QStringLiteral("org.sak.attr%1").arg(index, 4, 10, QLatin1Char('0')),
            1024);
        QVERIFY2(readBack.ok, qPrintable(readBack.blockers.join(QStringLiteral("; "))));
        QCOMPARE(readBack.data, value);
    }
    QVERIFY2(PartitionHfsFileSystemReader::checkConsistencyFromImage(imagePath).ok,
             "attributes tree consistency after multi-leaf create");

    // Deleting attributes drains the multi-leaf tree (merge/collapse) and stays
    // consistent; survivors still read back.
    for (int index = 0; index < created; index += 2) {
        const auto del = PartitionHfsFileSystemWriter::deleteAttributeValueFromImage(
            imagePath,
            18,
            QStringLiteral("org.sak.attr%1").arg(index, 4, 10, QLatin1Char('0')),
            options);
        QVERIFY2(del.ok, qPrintable(del.blockers.join(QStringLiteral("; "))));
    }
    QVERIFY2(PartitionHfsFileSystemReader::checkConsistencyFromImage(imagePath).ok,
             "attributes tree consistency after multi-leaf delete");
    const auto survivor = PartitionHfsFileSystemReader::readAttributeValueFromImage(
        imagePath, 18, QStringLiteral("org.sak.attr0001"), 1024);
    QVERIFY2(survivor.ok, qPrintable(survivor.blockers.join(QStringLiteral("; "))));
    QCOMPARE(survivor.data, value);
}

void PartitionManagerCoreTests::hfsFileSystemWriter_growsForkIntoExtentsOverflowRecords() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    PartitionHfsFileWriteOptions options;
    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.allow_journaled_volume = true;
    options.evidence_id = QStringLiteral("unit.hfs-extents-overflow-growth");

    const QString imagePath = temp.filePath(QStringLiteral("hfs-extents-overflow-growth.img"));
    QVERIFY(writeBytes(imagePath, hfsExtentsGrowthFixture()));

    QByteArray payload(static_cast<int>(9 * kTestHfsBlockSize + 100), 'G');
    payload.replace(0, 21, QByteArrayLiteral("grown into overflow!\n"));
    const auto grown = PartitionHfsFileSystemWriter::replaceFileWithAllocationGrowthFromImage(
        imagePath, QStringLiteral("/hello.txt"), payload, options);
    QVERIFY2(grown.ok, qPrintable(grown.blockers.join(QStringLiteral("; "))));
    QCOMPARE(grown.catalog_id, 17U);
    QCOMPARE(grown.bytes_written, static_cast<uint64_t>(payload.size()));
    QVERIFY(grown.warnings.join(' ').contains(QStringLiteral("extents-overflow record")));

    const auto readBack = PartitionHfsFileSystemReader::readFileFromImage(
        imagePath, QStringLiteral("/hello.txt"), 16 * kTestHfsBlockSize);
    QVERIFY2(readBack.ok, qPrintable(readBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(readBack.data, payload);

    const QByteArray after = readBytes(imagePath);
    const qsizetype extentsHeaderOffset =
        static_cast<qsizetype>(kTestHfsExtentsStartBlock * kTestHfsBlockSize) +
        kTestHfsBTreeHeaderRecordOffset;
    QCOMPARE(readBe16(after, extentsHeaderOffset + kTestHfsBTreeHeaderTreeDepthOffset), 1);
    QCOMPARE(readBe32(after, extentsHeaderOffset + kTestHfsBTreeHeaderRootNodeOffset), 1U);
    QCOMPARE(readBe32(after, extentsHeaderOffset + kTestHfsBTreeHeaderLeafRecordsOffset), 1U);
    QCOMPARE(readBe32(after, extentsHeaderOffset + kTestHfsBTreeHeaderFreeNodesOffset), 2U);

    const qsizetype leafOffset =
        static_cast<qsizetype>(kTestHfsExtentsStartBlock * kTestHfsBlockSize) +
        kTestHfsExtentsNodeSize;
    QCOMPARE(readBe16(after, leafOffset + kTestHfsBTreeNumRecordsOffset), 1);
    QCOMPARE(readBe32(after, leafOffset + kTestHfsBTreeNodeDescriptorSize + 4), 17U);
    QCOMPARE(readBe32(after, leafOffset + kTestHfsBTreeNodeDescriptorSize + 8), 8U);

    const auto deleted = PartitionHfsFileSystemWriter::deleteFileAndReleaseAllocatedBlocksFromImage(
        imagePath, QStringLiteral("/hello.txt"), options);
    QVERIFY2(deleted.ok, qPrintable(deleted.blockers.join(QStringLiteral("; "))));
    QVERIFY(deleted.warnings.join(' ').contains(
        QStringLiteral("extents-overflow records for the deleted file were removed")));

    const QByteArray afterDelete = readBytes(imagePath);
    QCOMPARE(readBe16(afterDelete, extentsHeaderOffset + kTestHfsBTreeHeaderTreeDepthOffset), 0);
    QCOMPARE(readBe32(afterDelete, extentsHeaderOffset + kTestHfsBTreeHeaderRootNodeOffset), 0U);
    QCOMPARE(readBe32(afterDelete, extentsHeaderOffset + kTestHfsBTreeHeaderLeafRecordsOffset), 0U);
    QCOMPARE(readBe32(afterDelete, extentsHeaderOffset + kTestHfsBTreeHeaderFreeNodesOffset), 3U);
    QVERIFY(!hfsAllocationBitSet(afterDelete, 21));
    QVERIFY(!hfsAllocationBitSet(afterDelete, 4));

    const auto missing = PartitionHfsFileSystemReader::readFileFromImage(
        imagePath, QStringLiteral("/hello.txt"), 16);
    QVERIFY(!missing.ok);
}

void PartitionManagerCoreTests::hfsFileSystemWriter_splitsExtentsOverflowRootLeaf() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    PartitionHfsFileWriteOptions options;
    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.allow_journaled_volume = true;
    options.evidence_id = QStringLiteral("unit.hfs-extents-overflow-split");

    const QString imagePath = temp.filePath(QStringLiteral("hfs-extents-overflow-split.img"));
    QVERIFY(writeBytes(imagePath, hfsExtentsSplitFixture()));

    QByteArray payload(static_cast<int>(9 * kTestHfsBlockSize + 64), 'S');
    const auto grown = PartitionHfsFileSystemWriter::replaceFileWithAllocationGrowthFromImage(
        imagePath, QStringLiteral("/hello.txt"), payload, options);
    QVERIFY2(grown.ok, qPrintable(grown.blockers.join(QStringLiteral("; "))));
    QVERIFY(grown.warnings.join(' ').contains(QStringLiteral("extents-overflow record")));

    const QByteArray after = readBytes(imagePath);
    const qsizetype extentsHeaderOffset =
        static_cast<qsizetype>(kTestHfsExtentsStartBlock * kTestHfsBlockSize) +
        kTestHfsBTreeHeaderRecordOffset;
    QCOMPARE(readBe16(after, extentsHeaderOffset + kTestHfsBTreeHeaderTreeDepthOffset), 2);
    const uint32_t firstLeaf =
        readBe32(after, extentsHeaderOffset + kTestHfsBTreeHeaderFirstLeafNodeOffset);
    const uint32_t lastLeaf = readBe32(after,
                                       extentsHeaderOffset + kTestHfsBTreeHeaderLastLeafNodeOffset);
    QCOMPARE(firstLeaf, 2U);
    QCOMPARE(lastLeaf, 3U);
    QCOMPARE(readBe32(after, extentsHeaderOffset + kTestHfsBTreeHeaderRootNodeOffset), 4U);
    QCOMPARE(readBe32(after, extentsHeaderOffset + kTestHfsBTreeHeaderLeafRecordsOffset), 13U);

    const auto readBack = PartitionHfsFileSystemReader::readFileFromImage(
        imagePath, QStringLiteral("/hello.txt"), 16 * kTestHfsBlockSize);
    QVERIFY2(readBack.ok, qPrintable(readBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(readBack.data, payload);
}

// H-d: the old extents engine rejected any tree that was not empty or a single
// leaf. The streaming model engine loads a real depth-2 tree (here built by the
// split above), removes records across its leaves, merges the survivors, and
// collapses the depth back to one - none of which the bounded engine could do.
void PartitionManagerCoreTests::hfsFileSystemWriter_collapsesDepthTwoExtentsTreeOnDelete() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    PartitionHfsFileWriteOptions options;
    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.allow_journaled_volume = true;
    options.evidence_id = QStringLiteral("unit.hfs-extents-depth-two-collapse");

    const QString imagePath = temp.filePath(QStringLiteral("hfs-extents-depth-two-collapse.img"));
    QVERIFY(writeBytes(imagePath, hfsExtentsSplitFixture()));

    // Grow /hello.txt so the engine splits the single leaf into a depth-2 tree.
    QByteArray payload(static_cast<int>(9 * kTestHfsBlockSize + 64), 'S');
    const auto grown = PartitionHfsFileSystemWriter::replaceFileWithAllocationGrowthFromImage(
        imagePath, QStringLiteral("/hello.txt"), payload, options);
    QVERIFY2(grown.ok, qPrintable(grown.blockers.join(QStringLiteral("; "))));

    const qsizetype extentsHeaderOffset =
        static_cast<qsizetype>(kTestHfsExtentsStartBlock * kTestHfsBlockSize) +
        kTestHfsBTreeHeaderRecordOffset;
    const QByteArray afterGrow = readBytes(imagePath);
    QCOMPARE(readBe16(afterGrow, extentsHeaderOffset + kTestHfsBTreeHeaderTreeDepthOffset), 2);
    QCOMPARE(readBe32(afterGrow, extentsHeaderOffset + kTestHfsBTreeHeaderLeafRecordsOffset), 13U);

    // Deleting /hello.txt removes its record from the depth-2 tree; the twelve
    // remaining filler records merge back into a single leaf and the depth
    // collapses to one.
    const auto deleted = PartitionHfsFileSystemWriter::deleteFileAndReleaseAllocatedBlocksFromImage(
        imagePath, QStringLiteral("/hello.txt"), options);
    QVERIFY2(deleted.ok, qPrintable(deleted.blockers.join(QStringLiteral("; "))));

    const QByteArray afterDelete = readBytes(imagePath);
    QCOMPARE(readBe16(afterDelete, extentsHeaderOffset + kTestHfsBTreeHeaderTreeDepthOffset), 1);
    QCOMPARE(readBe32(afterDelete, extentsHeaderOffset + kTestHfsBTreeHeaderLeafRecordsOffset),
             12U);
    const uint32_t firstLeaf =
        readBe32(afterDelete, extentsHeaderOffset + kTestHfsBTreeHeaderFirstLeafNodeOffset);
    QCOMPARE(firstLeaf,
             readBe32(afterDelete, extentsHeaderOffset + kTestHfsBTreeHeaderLastLeafNodeOffset));
    QCOMPARE(readBe32(afterDelete, extentsHeaderOffset + kTestHfsBTreeHeaderRootNodeOffset),
             firstLeaf);

    const auto missing =
        PartitionHfsFileSystemReader::readFileFromImage(imagePath, QStringLiteral("/hello.txt"), 1);
    QVERIFY(!missing.ok);
}

// H-d: splitting a leaf that sits below an existing index node - the streaming
// engine allocates a sibling leaf in place (no root relocation) and rebuilds the
// index level with three children. The bounded engine only ever split the root
// leaf of a depth-1 tree.
void PartitionManagerCoreTests::hfsFileSystemWriter_splitsLeafBelowExistingIndexNode() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    PartitionHfsFileWriteOptions options;
    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.allow_journaled_volume = true;
    options.evidence_id = QStringLiteral("unit.hfs-extents-multi-leaf-split");

    const QString imagePath = temp.filePath(QStringLiteral("hfs-extents-multi-leaf-split.img"));
    QVERIFY(writeBytes(imagePath, hfsExtentsTwoLeafFixture()));

    QByteArray payload(static_cast<int>(9 * kTestHfsBlockSize + 64), 'M');
    const auto grown = PartitionHfsFileSystemWriter::replaceFileWithAllocationGrowthFromImage(
        imagePath, QStringLiteral("/hello.txt"), payload, options);
    QVERIFY2(grown.ok, qPrintable(grown.blockers.join(QStringLiteral("; "))));

    const QByteArray after = readBytes(imagePath);
    const qsizetype extentsHeaderOffset =
        static_cast<qsizetype>(kTestHfsExtentsStartBlock * kTestHfsBlockSize) +
        kTestHfsBTreeHeaderRecordOffset;
    QCOMPARE(readBe16(after, extentsHeaderOffset + kTestHfsBTreeHeaderTreeDepthOffset), 2);
    QCOMPARE(readBe32(after, extentsHeaderOffset + kTestHfsBTreeHeaderLeafRecordsOffset), 21U);
    QCOMPARE(readBe32(after, extentsHeaderOffset + kTestHfsBTreeHeaderFirstLeafNodeOffset), 1U);
    QCOMPARE(readBe32(after, extentsHeaderOffset + kTestHfsBTreeHeaderLastLeafNodeOffset), 2U);

    // The rebuilt root index node must now reference three leaf children.
    const uint32_t rootNode = readBe32(after,
                                       extentsHeaderOffset + kTestHfsBTreeHeaderRootNodeOffset);
    const qsizetype rootNodeOffset =
        static_cast<qsizetype>(kTestHfsExtentsStartBlock * kTestHfsBlockSize) +
        static_cast<qsizetype>(rootNode) * kTestHfsExtentsNodeSize;
    QCOMPARE(readBe16(after, rootNodeOffset + kTestHfsBTreeNumRecordsOffset), 3);

    const auto readBack = PartitionHfsFileSystemReader::readFileFromImage(
        imagePath, QStringLiteral("/hello.txt"), 16 * kTestHfsBlockSize);
    QVERIFY2(readBack.ok, qPrintable(readBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(readBack.data, payload);
}

void PartitionManagerCoreTests::hfsFileSystemWriter_truncatesResourceForkWithinAllocatedBlocks() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString imagePath = temp.filePath(QStringLiteral("hfs-resource-truncate-write.img"));

    QByteArray expectedHello;
    QVERIFY(writeBytes(imagePath, hfsReaderOverflowFixture(&expectedHello)));

    PartitionHfsFileWriteOptions options;
    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.allow_journaled_volume = true;
    options.evidence_id = QStringLiteral("unit.hfs-resource-allocated-block-truncate");

    const auto truncated =
        PartitionHfsFileSystemWriter::truncateResourceForkWithinAllocatedBlocksFromImage(
            imagePath, QStringLiteral("/hello.txt"), options);
    QVERIFY2(truncated.ok, qPrintable(truncated.blockers.join(QStringLiteral("; "))));
    QCOMPARE(truncated.catalog_id, 17U);
    QCOMPARE(truncated.bytes_written, 0ULL);
    QVERIFY(truncated.chunks_written > 0);
    QVERIFY(truncated.warnings.join(' ').contains(QStringLiteral("resource fork")));
    QCOMPARE(truncated.after_sha256, sha256Hex(QByteArray()));

    const auto resourceBack = PartitionHfsFileSystemReader::readResourceForkFromImage(
        imagePath, QStringLiteral("/hello.txt"), 2 * kTestHfsBlockSize);
    QVERIFY2(resourceBack.ok, qPrintable(resourceBack.blockers.join(QStringLiteral("; "))));
    QVERIFY(resourceBack.data.isEmpty());

    const auto dataBack = PartitionHfsFileSystemReader::readFileFromImage(
        imagePath, QStringLiteral("/hello.txt"), 2 * kTestHfsBlockSize);
    QVERIFY2(dataBack.ok, qPrintable(dataBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(dataBack.data, expectedHello);
}

void PartitionManagerCoreTests::hfsFileSystemWriter_replacesInlineAttributesWithReadback() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString imagePath = temp.filePath(QStringLiteral("hfs-inline-attribute-write.img"));
    QVERIFY(writeBytes(imagePath, hfsReaderAttributeFixture()));

    PartitionHfsFileWriteOptions options;
    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.allow_journaled_volume = true;
    options.evidence_id = QStringLiteral("unit.hfs-inline-attribute-replace");

    const QByteArray replacement("finder-patched");
    const auto written = PartitionHfsFileSystemWriter::replaceInlineAttributeValueFromImage(
        imagePath, 17, QStringLiteral("com.apple.FinderInfo"), replacement, options);
    QVERIFY2(written.ok, qPrintable(written.blockers.join(QStringLiteral("; "))));
    QCOMPARE(written.file_id, 17U);
    QCOMPARE(written.attribute_name, QStringLiteral("com.apple.FinderInfo"));
    QCOMPARE(written.bytes_written, static_cast<uint64_t>(replacement.size()));
    QVERIFY(!written.before_sha256.isEmpty());
    QVERIFY(!written.after_sha256.isEmpty());
    QVERIFY(written.before_sha256 != written.after_sha256);
    QVERIFY(written.warnings.join(' ').contains(QStringLiteral("existing record capacity")));

    const auto readBack = PartitionHfsFileSystemReader::readAttributeValueFromImage(
        imagePath, 17, QStringLiteral("com.apple.FinderInfo"), 1024);
    QVERIFY2(readBack.ok, qPrintable(readBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(readBack.data, replacement);

    const QByteArray tooLarge(33, 'x');
    const auto blocked = PartitionHfsFileSystemWriter::replaceInlineAttributeValueFromImage(
        imagePath, 17, QStringLiteral("com.apple.FinderInfo"), tooLarge, options);
    QVERIFY(!blocked.ok);
    QVERIFY(blocked.blockers.join(' ').contains(QStringLiteral("existing record capacity")));

    const auto forkBlocked = PartitionHfsFileSystemWriter::replaceInlineAttributeValueFromImage(
        imagePath,
        17,
        QStringLiteral("com.apple.ResourceFork"),
        QByteArrayLiteral("not-inline"),
        options);
    QVERIFY(!forkBlocked.ok);
    QVERIFY(forkBlocked.blockers.join(' ').contains(QStringLiteral("inline")));

    const QByteArray forkReplacement("fork-attribute-patched");
    const auto forkWritten =
        PartitionHfsFileSystemWriter::replaceForkAttributeValueWithinAllocatedBlocksFromImage(
            imagePath, 17, QStringLiteral("com.apple.ResourceFork"), forkReplacement, options);
    QVERIFY2(forkWritten.ok, qPrintable(forkWritten.blockers.join(QStringLiteral("; "))));
    QCOMPARE(forkWritten.file_id, 17U);
    QCOMPARE(forkWritten.attribute_name, QStringLiteral("com.apple.ResourceFork"));
    QCOMPARE(forkWritten.bytes_written, static_cast<uint64_t>(forkReplacement.size()));
    QVERIFY(!forkWritten.before_sha256.isEmpty());
    QVERIFY(!forkWritten.after_sha256.isEmpty());
    QVERIFY(forkWritten.before_sha256 != forkWritten.after_sha256);
    QVERIFY(forkWritten.warnings.join(' ').contains(QStringLiteral("allocated blocks")));

    const auto forkReadBack = PartitionHfsFileSystemReader::readAttributeValueFromImage(
        imagePath, 17, QStringLiteral("com.apple.ResourceFork"), 1024);
    QVERIFY2(forkReadBack.ok, qPrintable(forkReadBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(forkReadBack.storage, QStringLiteral("fork"));
    QCOMPARE(forkReadBack.data, forkReplacement);

    const QByteArray oversizedFork(kTestHfsBlockSize + 1, 'x');
    const auto forkTooLarge =
        PartitionHfsFileSystemWriter::replaceForkAttributeValueWithinAllocatedBlocksFromImage(
            imagePath, 17, QStringLiteral("com.apple.ResourceFork"), oversizedFork, options);
    QVERIFY(!forkTooLarge.ok);
    QVERIFY(forkTooLarge.blockers.join(' ').contains(QStringLiteral("existing allocation")));

    const auto inlineAsForkBlocked =
        PartitionHfsFileSystemWriter::replaceForkAttributeValueWithinAllocatedBlocksFromImage(
            imagePath,
            17,
            QStringLiteral("com.apple.FinderInfo"),
            QByteArrayLiteral("not-fork"),
            options);
    QVERIFY(!inlineAsForkBlocked.ok);
    QVERIFY(inlineAsForkBlocked.blockers.join(' ').contains(QStringLiteral("fork-backed")));

    const auto noGrowth =
        PartitionHfsFileSystemWriter::replaceForkAttributeValueWithAllocationGrowthFromImage(
            imagePath, 17, QStringLiteral("com.apple.ResourceFork"), forkReplacement, options);
    QVERIFY(!noGrowth.ok);
    QVERIFY(
        noGrowth.blockers.join(' ').contains(QStringLiteral("does not require allocation growth")));

    const QByteArray forkGrowth(kTestHfsBlockSize + 32, 'G');
    const auto grown =
        PartitionHfsFileSystemWriter::replaceForkAttributeValueWithAllocationGrowthFromImage(
            imagePath, 17, QStringLiteral("com.apple.ResourceFork"), forkGrowth, options);
    QVERIFY2(grown.ok, qPrintable(grown.blockers.join(QStringLiteral("; "))));
    QCOMPARE(grown.file_id, 17U);
    QCOMPARE(grown.attribute_name, QStringLiteral("com.apple.ResourceFork"));
    QCOMPARE(grown.bytes_written, static_cast<uint64_t>(forkGrowth.size()));
    QVERIFY(grown.before_sha256 != grown.after_sha256);
    QVERIFY(grown.warnings.join(' ').contains(QStringLiteral("allocating")));

    const auto grownReadBack = PartitionHfsFileSystemReader::readAttributeValueFromImage(
        imagePath, 17, QStringLiteral("com.apple.ResourceFork"), 2 * kTestHfsBlockSize);
    QVERIFY2(grownReadBack.ok, qPrintable(grownReadBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(grownReadBack.storage, QStringLiteral("fork"));
    QCOMPARE(grownReadBack.data, forkGrowth);

    const QByteArray grownImage = readBytes(imagePath);
    QVERIFY(hfsAllocationBitSet(grownImage, 1));
    QCOMPARE(readBe32(grownImage, kTestHfsHeaderOffset + kTestHfsFreeBlocksOffset), 36U);

    const QString fullImagePath =
        temp.filePath(QStringLiteral("hfs-fork-attribute-growth-full.img"));
    QByteArray fullImage = hfsReaderAttributeFixture();
    writeBe32(&fullImage, kTestHfsHeaderOffset + kTestHfsFreeBlocksOffset, 0);
    QVERIFY(writeBytes(fullImagePath, fullImage));
    const auto noFree =
        PartitionHfsFileSystemWriter::replaceForkAttributeValueWithAllocationGrowthFromImage(
            fullImagePath, 17, QStringLiteral("com.apple.ResourceFork"), forkGrowth, options);
    QVERIFY(!noFree.ok);
    QVERIFY(noFree.blockers.join(' ').contains(QStringLiteral("not have enough free blocks")));

    // New-attribute create: insert into the existing single attributes leaf,
    // reject duplicates, and verify read-back of the created value.
    const QByteArray createdValue = QByteArrayLiteral("created xattr value");
    const auto createdAttr = PartitionHfsFileSystemWriter::createInlineAttributeValueFromImage(
        imagePath, 18, QStringLiteral("org.sak.created"), createdValue, options);
    QVERIFY2(createdAttr.ok, qPrintable(createdAttr.blockers.join(QStringLiteral("; "))));
    QCOMPARE(createdAttr.file_id, 18U);
    QCOMPARE(createdAttr.bytes_written, static_cast<uint64_t>(createdValue.size()));
    QVERIFY(createdAttr.warnings.join(' ').contains(QStringLiteral("inline attribute created")));

    const auto createdReadBack = PartitionHfsFileSystemReader::readAttributeValueFromImage(
        imagePath, 18, QStringLiteral("org.sak.created"), 1024);
    QVERIFY2(createdReadBack.ok, qPrintable(createdReadBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(createdReadBack.storage, QStringLiteral("inline"));
    QCOMPARE(createdReadBack.data, createdValue);

    // The owning catalog record must now carry kHFSHasAttributesMask so
    // fsck_hfs attribute-count buckets stay consistent.
    {
        const QByteArray afterAttrCreate = readBytes(imagePath);
        const QByteArray namePattern =
            QByteArray::fromHex("006e006f00740065002e007400780074");  // "note.txt" UTF-16BE
        const qsizetype nameAt = afterAttrCreate.indexOf(namePattern);
        QVERIFY(nameAt > 8);
        const qsizetype keyStart = nameAt - 8;
        const uint16_t keyLength = readBe16(afterAttrCreate, keyStart);
        const qsizetype flagsAt = keyStart + 2 + keyLength + 2;
        QCOMPARE(readBe16(afterAttrCreate, keyStart + 2 + keyLength), 2);
        QCOMPARE(readBe16(afterAttrCreate, flagsAt) & 0x0004, 0x0004);
    }

    const auto duplicateAttr = PartitionHfsFileSystemWriter::createInlineAttributeValueFromImage(
        imagePath, 18, QStringLiteral("org.sak.created"), createdValue, options);
    QVERIFY(!duplicateAttr.ok);
    QVERIFY(duplicateAttr.blockers.join(' ').contains(QStringLiteral("already exists")));

    const auto missingTarget = PartitionHfsFileSystemWriter::createInlineAttributeValueFromImage(
        imagePath, 999, QStringLiteral("org.sak.orphan"), createdValue, options);
    QVERIFY(!missingTarget.ok);
    QVERIFY(missingTarget.blockers.join(' ').contains(QStringLiteral("catalog ID")));

    const auto decmpfsBlocked = PartitionHfsFileSystemWriter::createInlineAttributeValueFromImage(
        imagePath, 18, QStringLiteral("com.apple.decmpfs"), createdValue, options);
    QVERIFY(!decmpfsBlocked.ok);
    QVERIFY(decmpfsBlocked.blockers.join(' ').contains(
        QStringLiteral("compressed-file mutation is blocked")));

    // Fork-backed attribute create: allocates blocks, writes the value into
    // the new extents, and reads it back through the fork storage path.
    QByteArray forkAttrValue(9000, '\0');
    for (int index = 0; index < forkAttrValue.size(); ++index) {
        forkAttrValue[index] = static_cast<char>('A' + index % 23);
    }
    const auto forkCreated = PartitionHfsFileSystemWriter::createForkAttributeValueFromImage(
        imagePath, 18, QStringLiteral("org.sak.fork-created"), forkAttrValue, options);
    QVERIFY2(forkCreated.ok, qPrintable(forkCreated.blockers.join(QStringLiteral("; "))));
    QVERIFY(
        forkCreated.warnings.join(' ').contains(QStringLiteral("fork-backed attribute created")));
    const auto forkCreatedReadBack = PartitionHfsFileSystemReader::readAttributeValueFromImage(
        imagePath, 18, QStringLiteral("org.sak.fork-created"), 65'536);
    QVERIFY2(forkCreatedReadBack.ok,
             qPrintable(forkCreatedReadBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(forkCreatedReadBack.storage, QStringLiteral("fork"));
    QCOMPARE(forkCreatedReadBack.data, forkAttrValue);

    // Attribute delete: fork-backed delete releases allocation blocks; the
    // free-block counter is restored exactly.
    const QByteArray beforeDelete = readBytes(imagePath);
    const uint32_t freeBefore = readBe32(beforeDelete,
                                         kTestHfsHeaderOffset + kTestHfsFreeBlocksOffset);
    const auto forkDeleted = PartitionHfsFileSystemWriter::deleteAttributeValueFromImage(
        imagePath, 18, QStringLiteral("org.sak.fork-created"), options);
    QVERIFY2(forkDeleted.ok, qPrintable(forkDeleted.blockers.join(QStringLiteral("; "))));
    QVERIFY(forkDeleted.warnings.join(' ').contains(QStringLiteral("released")));
    const QByteArray afterDelete = readBytes(imagePath);
    const uint32_t freeAfter = readBe32(afterDelete,
                                        kTestHfsHeaderOffset + kTestHfsFreeBlocksOffset);
    QCOMPARE(freeAfter, freeBefore + 3);
    const auto forkGone = PartitionHfsFileSystemReader::readAttributeValueFromImage(
        imagePath, 18, QStringLiteral("org.sak.fork-created"), 65'536);
    QVERIFY(!forkGone.ok);

    // Inline attribute delete; deleting a missing attribute fails closed.
    const auto inlineDeleted = PartitionHfsFileSystemWriter::deleteAttributeValueFromImage(
        imagePath, 18, QStringLiteral("org.sak.created"), options);
    QVERIFY2(inlineDeleted.ok, qPrintable(inlineDeleted.blockers.join(QStringLiteral("; "))));
    const auto missingDelete = PartitionHfsFileSystemWriter::deleteAttributeValueFromImage(
        imagePath, 18, QStringLiteral("org.sak.created"), options);
    QVERIFY(!missingDelete.ok);
    QVERIFY(missingDelete.blockers.join(' ').contains(QStringLiteral("was not found")));
}

void PartitionManagerCoreTests::
    hfsFileSystemWriter_createsAndDeletesEmptyFilesWithCatalogReadback() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString imagePath = temp.filePath(QStringLiteral("hfs-empty-file-create-delete.img"));
    QVERIFY(writeBytes(imagePath, hfsReaderFixture()));

    PartitionHfsFileWriteOptions options;
    auto blocked = PartitionHfsFileSystemWriter::createEmptyFileFromImage(
        imagePath, QStringLiteral("/created.txt"), options);
    QVERIFY(!blocked.ok);
    QVERIFY(blocked.blockers.join(' ').contains(QStringLiteral("writer is not enabled")));

    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.evidence_id = QStringLiteral("unit.hfs-empty-file-create-delete");
    blocked = PartitionHfsFileSystemWriter::createEmptyFileFromImage(imagePath,
                                                                     QStringLiteral("/created.txt"),
                                                                     options);
    QVERIFY(!blocked.ok);
    QVERIFY(blocked.blockers.join(' ').contains(QStringLiteral("journal")));

    options.allow_journaled_volume = true;
    const auto rawBlocked = PartitionHfsFileSystemWriter::createEmptyFileFromImage(
        extToolRawTargetPath(), QStringLiteral("/created.txt"), options);
    QVERIFY(!rawBlocked.ok);
    QVERIFY(rawBlocked.blockers.join(' ').contains(QStringLiteral("image-only")));

    const auto created = PartitionHfsFileSystemWriter::createEmptyFileFromImage(
        imagePath, QStringLiteral("/created.txt"), options);
    QVERIFY2(created.ok, qPrintable(created.blockers.join(QStringLiteral("; "))));
    QCOMPARE(created.file_system, QStringLiteral("HFS+"));
    QVERIFY(created.catalog_id >= 19U);
    QCOMPARE(created.bytes_written, 0ULL);
    QVERIFY(created.chunks_written >= 4);
    QVERIFY(!created.before_sha256.isEmpty());
    QVERIFY(!created.after_sha256.isEmpty());
    QVERIFY(created.before_sha256 != created.after_sha256);
    QVERIFY(created.warnings.join(' ').contains(QStringLiteral("empty file created")));

    const auto root =
        PartitionHfsFileSystemReader::listDirectoryFromImage(imagePath, QStringLiteral("/"), 20);
    QVERIFY2(root.ok, qPrintable(root.blockers.join(QStringLiteral("; "))));
    QVERIFY(std::any_of(root.entries.cbegin(), root.entries.cend(), [](const auto& entry) {
        return entry.name == QStringLiteral("created.txt") && entry.regular_file &&
               entry.size_bytes == 0;
    }));

    const auto readBack = PartitionHfsFileSystemReader::readFileFromImage(
        imagePath, QStringLiteral("/created.txt"), 1);
    QVERIFY2(readBack.ok, qPrintable(readBack.blockers.join(QStringLiteral("; "))));
    QVERIFY(readBack.data.isEmpty());

    const auto renamedEmpty = PartitionHfsFileSystemWriter::renameOrMoveCatalogEntryFromImage(
        imagePath, QStringLiteral("/created.txt"), QStringLiteral("/created-renamed.txt"), options);
    QVERIFY2(renamedEmpty.ok, qPrintable(renamedEmpty.blockers.join(QStringLiteral("; "))));
    QCOMPARE(renamedEmpty.catalog_id, created.catalog_id);
    QCOMPARE(renamedEmpty.bytes_written, 1ULL);
    QVERIFY(renamedEmpty.warnings.join(' ').contains(QStringLiteral("renamed")));

    const auto oldEmptyReadBack = PartitionHfsFileSystemReader::readFileFromImage(
        imagePath, QStringLiteral("/created.txt"), 1);
    QVERIFY(!oldEmptyReadBack.ok);
    QVERIFY(oldEmptyReadBack.blockers.join(' ').contains(QStringLiteral("not found")));

    const auto renamedEmptyReadBack = PartitionHfsFileSystemReader::readFileFromImage(
        imagePath, QStringLiteral("/created-renamed.txt"), 1);
    QVERIFY2(renamedEmptyReadBack.ok,
             qPrintable(renamedEmptyReadBack.blockers.join(QStringLiteral("; "))));
    QVERIFY(renamedEmptyReadBack.data.isEmpty());

    const auto consistency = PartitionHfsFileSystemReader::checkConsistencyFromImage(imagePath, 20);
    QVERIFY2(consistency.ok, qPrintable(consistency.blockers.join(QStringLiteral("; "))));
    QCOMPARE(consistency.files, 3);
    QCOMPARE(consistency.threads, 1);
    QCOMPARE(consistency.records_scanned, 5);
    QVERIFY(!consistency.warnings.join(' ').contains(QStringLiteral("record count mismatch")));

    const auto duplicate = PartitionHfsFileSystemWriter::createEmptyFileFromImage(
        imagePath, QStringLiteral("/created-renamed.txt"), options);
    QVERIFY(!duplicate.ok);
    QVERIFY(duplicate.blockers.join(' ').contains(QStringLiteral("already exists")));

    const auto nonEmptyDelete = PartitionHfsFileSystemWriter::deleteEmptyFileFromImage(
        imagePath, QStringLiteral("/hello.txt"), options);
    QVERIFY(!nonEmptyDelete.ok);
    QVERIFY(nonEmptyDelete.blockers.join(' ').contains(QStringLiteral("zero-byte")));

    const auto deleted = PartitionHfsFileSystemWriter::deleteEmptyFileFromImage(
        imagePath, QStringLiteral("/created-renamed.txt"), options);
    QVERIFY2(deleted.ok, qPrintable(deleted.blockers.join(QStringLiteral("; "))));
    QCOMPARE(deleted.catalog_id, created.catalog_id);
    QCOMPARE(deleted.bytes_written, 0ULL);
    QVERIFY(deleted.chunks_written >= 3);
    QVERIFY(deleted.warnings.join(' ').contains(QStringLiteral("empty file deleted")));
    QVERIFY(!deleted.before_sha256.isEmpty());
    QVERIFY(!deleted.after_sha256.isEmpty());
    QVERIFY(deleted.before_sha256 != deleted.after_sha256);

    const auto missingReadBack = PartitionHfsFileSystemReader::readFileFromImage(
        imagePath, QStringLiteral("/created-renamed.txt"), 1);
    QVERIFY(!missingReadBack.ok);
    QVERIFY(missingReadBack.blockers.join(' ').contains(QStringLiteral("not found")));

    const auto finalConsistency = PartitionHfsFileSystemReader::checkConsistencyFromImage(imagePath,
                                                                                          20);
    QVERIFY2(finalConsistency.ok, qPrintable(finalConsistency.blockers.join(QStringLiteral("; "))));
    QCOMPARE(finalConsistency.files, 2);
    QCOMPARE(finalConsistency.threads, 0);
    QCOMPARE(finalConsistency.records_scanned, 3);

    const auto moveTargetCreated = PartitionHfsFileSystemWriter::createEmptyFolderFromImage(
        imagePath, QStringLiteral("/Move Target"), options);
    QVERIFY2(moveTargetCreated.ok,
             qPrintable(moveTargetCreated.blockers.join(QStringLiteral("; "))));

    const QByteArray createFilePayload = QByteArrayLiteral("created file payload\n");
    const auto dataCreated = PartitionHfsFileSystemWriter::createFileWithDataFromImage(
        imagePath, QStringLiteral("/created-data.bin"), createFilePayload, options);
    QVERIFY2(dataCreated.ok, qPrintable(dataCreated.blockers.join(QStringLiteral("; "))));
    QCOMPARE(dataCreated.file_system, QStringLiteral("HFS+"));
    QVERIFY(dataCreated.catalog_id >= 19U);
    QCOMPARE(dataCreated.bytes_written, static_cast<uint64_t>(createFilePayload.size()));
    QVERIFY(dataCreated.chunks_written >= 6);
    QVERIFY(dataCreated.warnings.join(' ').contains(QStringLiteral("file created")));

    const auto dataReadBack = PartitionHfsFileSystemReader::readFileFromImage(
        imagePath, QStringLiteral("/created-data.bin"), 1024);
    QVERIFY2(dataReadBack.ok, qPrintable(dataReadBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(dataReadBack.data, createFilePayload);
    const QByteArray afterDataCreate = readBytes(imagePath);
    QVERIFY(hfsAllocationBitSet(afterDataCreate, 1));
    QCOMPARE(readBe32(afterDataCreate, kTestHfsHeaderOffset + kTestHfsFreeBlocksOffset), 39U);

    const auto movedData = PartitionHfsFileSystemWriter::renameOrMoveCatalogEntryFromImage(
        imagePath,
        QStringLiteral("/created-data.bin"),
        QStringLiteral("/Move Target/renamed-data.bin"),
        options);
    QVERIFY2(movedData.ok, qPrintable(movedData.blockers.join(QStringLiteral("; "))));
    QCOMPARE(movedData.catalog_id, dataCreated.catalog_id);
    QCOMPARE(movedData.bytes_written, 1ULL);
    QVERIFY(movedData.warnings.join(' ').contains(QStringLiteral("moved")));

    const auto oldDataReadBack = PartitionHfsFileSystemReader::readFileFromImage(
        imagePath, QStringLiteral("/created-data.bin"), 1024);
    QVERIFY(!oldDataReadBack.ok);
    QVERIFY(oldDataReadBack.blockers.join(' ').contains(QStringLiteral("not found")));

    const auto movedDataReadBack = PartitionHfsFileSystemReader::readFileFromImage(
        imagePath, QStringLiteral("/Move Target/renamed-data.bin"), 1024);
    QVERIFY2(movedDataReadBack.ok,
             qPrintable(movedDataReadBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(movedDataReadBack.data, createFilePayload);

    const auto moveTargetListing = PartitionHfsFileSystemReader::listDirectoryFromImage(
        imagePath, QStringLiteral("/Move Target"), 20);
    QVERIFY2(moveTargetListing.ok,
             qPrintable(moveTargetListing.blockers.join(QStringLiteral("; "))));
    QVERIFY(std::any_of(moveTargetListing.entries.cbegin(),
                        moveTargetListing.entries.cend(),
                        [](const auto& entry) {
                            return entry.name == QStringLiteral("renamed-data.bin") &&
                                   entry.regular_file;
                        }));

    const auto dataDeleted =
        PartitionHfsFileSystemWriter::deleteFileAndReleaseAllocatedBlocksFromImage(
            imagePath, QStringLiteral("/Move Target/renamed-data.bin"), options);
    QVERIFY2(dataDeleted.ok, qPrintable(dataDeleted.blockers.join(QStringLiteral("; "))));
    QCOMPARE(dataDeleted.catalog_id, dataCreated.catalog_id);
    QVERIFY(dataDeleted.warnings.join(' ').contains(QStringLiteral("allocated blocks released")));
    const QByteArray afterDataDelete = readBytes(imagePath);
    QVERIFY(!hfsAllocationBitSet(afterDataDelete, 1));
    QCOMPARE(readBe32(afterDataDelete, kTestHfsHeaderOffset + kTestHfsFreeBlocksOffset), 40U);

    const auto moveTargetDeleted = PartitionHfsFileSystemWriter::deleteEmptyFolderFromImage(
        imagePath, QStringLiteral("/Move Target"), options);
    QVERIFY2(moveTargetDeleted.ok,
             qPrintable(moveTargetDeleted.blockers.join(QStringLiteral("; "))));
    QCOMPARE(moveTargetDeleted.catalog_id, moveTargetCreated.catalog_id);

    const auto folderCreated = PartitionHfsFileSystemWriter::createEmptyFolderFromImage(
        imagePath, QStringLiteral("/Created Folder"), options);
    QVERIFY2(folderCreated.ok, qPrintable(folderCreated.blockers.join(QStringLiteral("; "))));
    QVERIFY(folderCreated.catalog_id >= 19U);
    QCOMPARE(folderCreated.bytes_written, 0ULL);
    QVERIFY(folderCreated.chunks_written >= 4);
    QVERIFY(folderCreated.warnings.join(' ').contains(QStringLiteral("empty folder created")));

    const auto rootWithFolder =
        PartitionHfsFileSystemReader::listDirectoryFromImage(imagePath, QStringLiteral("/"), 20);
    QVERIFY2(rootWithFolder.ok, qPrintable(rootWithFolder.blockers.join(QStringLiteral("; "))));
    QVERIFY(std::any_of(
        rootWithFolder.entries.cbegin(), rootWithFolder.entries.cend(), [](const auto& entry) {
            return entry.name == QStringLiteral("Created Folder") && entry.directory;
        }));

    const auto createdFolderListing = PartitionHfsFileSystemReader::listDirectoryFromImage(
        imagePath, QStringLiteral("/Created Folder"), 20);
    QVERIFY2(createdFolderListing.ok,
             qPrintable(createdFolderListing.blockers.join(QStringLiteral("; "))));
    QVERIFY(createdFolderListing.entries.isEmpty());

    const auto folderRenamed = PartitionHfsFileSystemWriter::renameOrMoveCatalogEntryFromImage(
        imagePath, QStringLiteral("/Created Folder"), QStringLiteral("/Renamed Folder"), options);
    QVERIFY2(folderRenamed.ok, qPrintable(folderRenamed.blockers.join(QStringLiteral("; "))));
    QCOMPARE(folderRenamed.catalog_id, folderCreated.catalog_id);
    QVERIFY(folderRenamed.warnings.join(' ').contains(QStringLiteral("renamed")));

    const auto renamedFolderListing = PartitionHfsFileSystemReader::listDirectoryFromImage(
        imagePath, QStringLiteral("/Renamed Folder"), 20);
    QVERIFY2(renamedFolderListing.ok,
             qPrintable(renamedFolderListing.blockers.join(QStringLiteral("; "))));
    QVERIFY(renamedFolderListing.entries.isEmpty());

    const auto oldFolderListing = PartitionHfsFileSystemReader::listDirectoryFromImage(
        imagePath, QStringLiteral("/Created Folder"), 20);
    QVERIFY(!oldFolderListing.ok);
    QVERIFY(oldFolderListing.blockers.join(' ').contains(QStringLiteral("not found")));

    const auto rootDeleteBlocked = PartitionHfsFileSystemWriter::deleteEmptyFolderFromImage(
        imagePath, QStringLiteral("/"), options);
    QVERIFY(!rootDeleteBlocked.ok);
    QVERIFY(rootDeleteBlocked.blockers.join(' ').contains(QStringLiteral("root folder")));

    const auto fileAsFolderBlocked = PartitionHfsFileSystemWriter::deleteEmptyFolderFromImage(
        imagePath, QStringLiteral("/hello.txt"), options);
    QVERIFY(!fileAsFolderBlocked.ok);
    QVERIFY(fileAsFolderBlocked.blockers.join(' ').contains(QStringLiteral("not a directory")));

    const auto folderDeleted = PartitionHfsFileSystemWriter::deleteEmptyFolderFromImage(
        imagePath, QStringLiteral("/Renamed Folder"), options);
    QVERIFY2(folderDeleted.ok, qPrintable(folderDeleted.blockers.join(QStringLiteral("; "))));
    QCOMPARE(folderDeleted.catalog_id, folderCreated.catalog_id);
    QVERIFY(folderDeleted.warnings.join(' ').contains(QStringLiteral("empty folder deleted")));
    QVERIFY(!folderDeleted.before_sha256.isEmpty());
    QVERIFY(!folderDeleted.after_sha256.isEmpty());
    QVERIFY(folderDeleted.before_sha256 != folderDeleted.after_sha256);

    const auto missingFolder = PartitionHfsFileSystemReader::listDirectoryFromImage(
        imagePath, QStringLiteral("/Renamed Folder"), 20);
    QVERIFY(!missingFolder.ok);
    QVERIFY(missingFolder.blockers.join(' ').contains(QStringLiteral("not found")));

    const auto finalFolderConsistency =
        PartitionHfsFileSystemReader::checkConsistencyFromImage(imagePath, 20);
    QVERIFY2(finalFolderConsistency.ok,
             qPrintable(finalFolderConsistency.blockers.join(QStringLiteral("; "))));
    QCOMPARE(finalFolderConsistency.files, 2);
    QCOMPARE(finalFolderConsistency.threads, 0);
    QCOMPARE(finalFolderConsistency.records_scanned, 3);
}

void PartitionManagerCoreTests::hfsFileSystemWriter_deletesAllocatedFilesAndReleasesBitmap() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString imagePath = temp.filePath(QStringLiteral("hfs-allocated-file-delete.img"));
    QVERIFY(writeBytes(imagePath, hfsReaderFixture()));

    PartitionHfsFileWriteOptions options;
    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.allow_journaled_volume = true;
    options.evidence_id = QStringLiteral("unit.hfs-allocated-file-delete");

    const QByteArray beforeImage = readBytes(imagePath);
    QVERIFY(hfsAllocationBitSet(beforeImage, kTestHfsNoteFileBlock));

    const auto emptyDeleteBlocked = PartitionHfsFileSystemWriter::deleteEmptyFileFromImage(
        imagePath, QStringLiteral("/Docs/note.txt"), options);
    QVERIFY(!emptyDeleteBlocked.ok);
    QVERIFY(emptyDeleteBlocked.blockers.join(' ').contains(QStringLiteral("zero-byte")));

    const auto deleted = PartitionHfsFileSystemWriter::deleteFileAndReleaseAllocatedBlocksFromImage(
        imagePath, QStringLiteral("/Docs/note.txt"), options);
    QVERIFY2(deleted.ok, qPrintable(deleted.blockers.join(QStringLiteral("; "))));
    QCOMPARE(deleted.file_system, QStringLiteral("HFS+"));
    QCOMPARE(deleted.catalog_id, 18U);
    QCOMPARE(deleted.bytes_written, static_cast<uint64_t>(kTestHfsBlockSize));
    QVERIFY(deleted.chunks_written >= 5);
    QVERIFY(deleted.warnings.join(' ').contains(QStringLiteral("allocated blocks released")));
    QVERIFY(!deleted.before_sha256.isEmpty());
    QVERIFY(!deleted.after_sha256.isEmpty());
    QVERIFY(deleted.before_sha256 != deleted.after_sha256);

    const QByteArray afterImage = readBytes(imagePath);
    QVERIFY(!hfsAllocationBitSet(afterImage, kTestHfsNoteFileBlock));
    QCOMPARE(qFromBigEndian<uint32_t>(afterImage.constData() + kTestHfsHeaderOffset +
                                      kTestHfsFreeBlocksOffset),
             41U);

    const auto docs = PartitionHfsFileSystemReader::listDirectoryFromImage(imagePath,
                                                                           QStringLiteral("/Docs"),
                                                                           20);
    QVERIFY2(docs.ok, qPrintable(docs.blockers.join(QStringLiteral("; "))));
    QVERIFY(docs.entries.isEmpty());

    const auto missingReadBack = PartitionHfsFileSystemReader::readFileFromImage(
        imagePath, QStringLiteral("/Docs/note.txt"), 1);
    QVERIFY(!missingReadBack.ok);
    QVERIFY(missingReadBack.blockers.join(' ').contains(QStringLiteral("not found")));

    const auto consistency = PartitionHfsFileSystemReader::checkConsistencyFromImage(imagePath, 20);
    QVERIFY2(consistency.ok, qPrintable(consistency.blockers.join(QStringLiteral("; "))));
    QCOMPARE(consistency.files, 1);
    QCOMPARE(consistency.directories, 1);
    QCOMPARE(consistency.records_scanned, 2);

    const auto missingAgain =
        PartitionHfsFileSystemWriter::deleteFileAndReleaseAllocatedBlocksFromImage(
            imagePath, QStringLiteral("/Docs/note.txt"), options);
    QVERIFY(!missingAgain.ok);
    QVERIFY(missingAgain.blockers.join(' ').contains(QStringLiteral("not found")));

    const QString secureDeleteImagePath =
        temp.filePath(QStringLiteral("hfs-allocated-file-secure-delete.img"));
    QVERIFY(writeBytes(secureDeleteImagePath, hfsReaderFixture()));
    PartitionHfsFileWriteOptions secureDeleteOptions = options;
    secureDeleteOptions.secure_wipe_deleted_blocks = true;
    const auto secureDeleted =
        PartitionHfsFileSystemWriter::deleteFileAndReleaseAllocatedBlocksFromImage(
            secureDeleteImagePath, QStringLiteral("/Docs/note.txt"), secureDeleteOptions);
    QVERIFY2(secureDeleted.ok, qPrintable(secureDeleted.blockers.join(QStringLiteral("; "))));
    QVERIFY(secureDeleted.warnings.join(' ').contains(
        QStringLiteral("zeroing released allocated blocks")));
    const QByteArray secureDeletedImage = readBytes(secureDeleteImagePath);
    QCOMPARE(secureDeletedImage.mid(kTestHfsNoteFileBlock * kTestHfsBlockSize, kTestHfsBlockSize),
             QByteArray(kTestHfsBlockSize, '\0'));
    QVERIFY(!hfsAllocationBitSet(secureDeletedImage, kTestHfsNoteFileBlock));

    const QString folderTreeImagePath = temp.filePath(QStringLiteral("hfs-folder-tree-delete.img"));
    QVERIFY(writeBytes(folderTreeImagePath, hfsReaderFixture()));
    const auto rootTreeBlocked =
        PartitionHfsFileSystemWriter::deleteFolderTreeAndReleaseAllocatedBlocksFromImage(
            folderTreeImagePath, QStringLiteral("/"), options);
    QVERIFY(!rootTreeBlocked.ok);
    QVERIFY(rootTreeBlocked.blockers.join(' ').contains(QStringLiteral("root folder")));

    const auto fileTreeBlocked =
        PartitionHfsFileSystemWriter::deleteFolderTreeAndReleaseAllocatedBlocksFromImage(
            folderTreeImagePath, QStringLiteral("/hello.txt"), options);
    QVERIFY(!fileTreeBlocked.ok);
    QVERIFY(fileTreeBlocked.blockers.join(' ').contains(QStringLiteral("not a directory")));

    const auto treeDeleted =
        PartitionHfsFileSystemWriter::deleteFolderTreeAndReleaseAllocatedBlocksFromImage(
            folderTreeImagePath, QStringLiteral("/Docs"), options);
    QVERIFY2(treeDeleted.ok, qPrintable(treeDeleted.blockers.join(QStringLiteral("; "))));
    QCOMPARE(treeDeleted.catalog_id, 16U);
    QCOMPARE(treeDeleted.bytes_written, static_cast<uint64_t>(kTestHfsBlockSize));
    QVERIFY(treeDeleted.chunks_written >= 5);
    QVERIFY(treeDeleted.warnings.join(' ').contains(QStringLiteral("folder tree deleted")));
    const QByteArray folderTreeAfter = readBytes(folderTreeImagePath);
    QVERIFY(!hfsAllocationBitSet(folderTreeAfter, kTestHfsNoteFileBlock));
    QCOMPARE(qFromBigEndian<uint32_t>(folderTreeAfter.constData() + kTestHfsHeaderOffset +
                                      kTestHfsFileCountOffset),
             1U);
    QCOMPARE(qFromBigEndian<uint32_t>(folderTreeAfter.constData() + kTestHfsHeaderOffset +
                                      kTestHfsFolderCountOffset),
             0U);
    QCOMPARE(qFromBigEndian<uint32_t>(folderTreeAfter.constData() + kTestHfsHeaderOffset +
                                      kTestHfsFreeBlocksOffset),
             41U);
    const auto rootAfterTreeDelete = PartitionHfsFileSystemReader::listDirectoryFromImage(
        folderTreeImagePath, QStringLiteral("/"), 20);
    QVERIFY2(rootAfterTreeDelete.ok,
             qPrintable(rootAfterTreeDelete.blockers.join(QStringLiteral("; "))));
    QVERIFY(std::none_of(rootAfterTreeDelete.entries.cbegin(),
                         rootAfterTreeDelete.entries.cend(),
                         [](const auto& entry) { return entry.name == QStringLiteral("Docs"); }));
    const auto missingTreeReadBack = PartitionHfsFileSystemReader::readFileFromImage(
        folderTreeImagePath, QStringLiteral("/Docs/note.txt"), 1);
    QVERIFY(!missingTreeReadBack.ok);
    QVERIFY(missingTreeReadBack.blockers.join(' ').contains(QStringLiteral("not found")));
    const auto treeConsistency =
        PartitionHfsFileSystemReader::checkConsistencyFromImage(folderTreeImagePath, 20);
    QVERIFY2(treeConsistency.ok, qPrintable(treeConsistency.blockers.join(QStringLiteral("; "))));
    QCOMPARE(treeConsistency.files, 1);
    QCOMPARE(treeConsistency.directories, 0);
    QCOMPARE(treeConsistency.records_scanned, 1);

    const QString secureTreeImagePath =
        temp.filePath(QStringLiteral("hfs-folder-tree-secure-delete.img"));
    QVERIFY(writeBytes(secureTreeImagePath, hfsReaderFixture()));
    const auto secureTreeDeleted =
        PartitionHfsFileSystemWriter::deleteFolderTreeAndReleaseAllocatedBlocksFromImage(
            secureTreeImagePath, QStringLiteral("/Docs"), secureDeleteOptions);
    QVERIFY2(secureTreeDeleted.ok,
             qPrintable(secureTreeDeleted.blockers.join(QStringLiteral("; "))));
    QVERIFY(secureTreeDeleted.warnings.join(' ').contains(
        QStringLiteral("zeroing released file blocks")));
    const QByteArray secureTreeDeletedImage = readBytes(secureTreeImagePath);
    QCOMPARE(secureTreeDeletedImage.mid(kTestHfsNoteFileBlock * kTestHfsBlockSize,
                                        kTestHfsBlockSize),
             QByteArray(kTestHfsBlockSize, '\0'));
    QVERIFY(!hfsAllocationBitSet(secureTreeDeletedImage, kTestHfsNoteFileBlock));

    const QString attributeImagePath =
        temp.filePath(QStringLiteral("hfs-allocated-file-delete-attribute-block.img"));
    QVERIFY(writeBytes(attributeImagePath, hfsReaderAttributeFixture()));
    const auto attributeBlocked =
        PartitionHfsFileSystemWriter::deleteFileAndReleaseAllocatedBlocksFromImage(
            attributeImagePath, QStringLiteral("/hello.txt"), options);
    QVERIFY(!attributeBlocked.ok);
    QVERIFY(attributeBlocked.blockers.join(' ').contains(QStringLiteral("attribute records")));

    QByteArray unsetBitmapImage = hfsReaderFixture();
    clearHfsAllocationBit(&unsetBitmapImage, kTestHfsNoteFileBlock);
    const QString unsetBitmapImagePath =
        temp.filePath(QStringLiteral("hfs-allocated-file-delete-unset-bitmap.img"));
    QVERIFY(writeBytes(unsetBitmapImagePath, unsetBitmapImage));
    const auto bitmapBlocked =
        PartitionHfsFileSystemWriter::deleteFileAndReleaseAllocatedBlocksFromImage(
            unsetBitmapImagePath, QStringLiteral("/Docs/note.txt"), options);
    QVERIFY(!bitmapBlocked.ok);
    QVERIFY(bitmapBlocked.blockers.join(' ').contains(QStringLiteral("mark file block allocated")));

    const QString overflowImagePath =
        temp.filePath(QStringLiteral("hfs-allocated-file-delete-overflow.img"));
    QVERIFY(writeBytes(overflowImagePath, hfsReaderOverflowFixtureWithAllocation()));
    const auto overflowDeleted =
        PartitionHfsFileSystemWriter::deleteFileAndReleaseAllocatedBlocksFromImage(
            overflowImagePath, QStringLiteral("/hello.txt"), options);
    QVERIFY2(overflowDeleted.ok, qPrintable(overflowDeleted.blockers.join(QStringLiteral("; "))));
    QCOMPARE(overflowDeleted.bytes_written, static_cast<uint64_t>(4 * kTestHfsBlockSize));
    const QByteArray overflowAfter = readBytes(overflowImagePath);
    QVERIFY(!hfsAllocationBitSet(overflowAfter, kTestHfsHelloFileBlock));
    QVERIFY(!hfsAllocationBitSet(overflowAfter, kTestHfsHelloFileBlock + 1));
    QVERIFY(!hfsAllocationBitSet(overflowAfter, kTestHfsResourceFileBlock));
    QVERIFY(!hfsAllocationBitSet(overflowAfter, kTestHfsResourceFileBlock + 1));

    const auto overflowMissing = PartitionHfsFileSystemReader::readFileFromImage(
        overflowImagePath, QStringLiteral("/hello.txt"), 1);
    QVERIFY(!overflowMissing.ok);
    QVERIFY(overflowMissing.blockers.join(' ').contains(QStringLiteral("not found")));
}

void PartitionManagerCoreTests::hfsFileSystemWriter_createsHardlinksAndSymlinks() {
    // H5: symlink + hard-link create/delete. Byte layout harvested from macOS and
    // certified clean by Apple fsck_hfs (multi-linked files + catalog hierarchy).
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString imagePath = temp.filePath(QStringLiteral("hfs-h5.img"));
    QVERIFY(writeBytes(imagePath, hfsReaderFixture()));

    PartitionHfsFileWriteOptions options;
    options.enable_writer = true;
    options.target_write_confirmed = true;
    options.allow_journaled_volume = true;
    options.evidence_id = QStringLiteral("unit.hfs-h5-links");

    const QByteArray payload(200, 'H');
    QVERIFY2(PartitionHfsFileSystemWriter::createFileWithDataFromImage(
                 imagePath, QStringLiteral("/hl.txt"), payload, options)
                 .ok,
             "create base file");

    // Symbolic link: a file with the S_IFLNK / 'slnk':'rhap' record whose data fork
    // holds the target path.
    const auto symlink = PartitionHfsFileSystemWriter::createSymlinkFromImage(
        imagePath, QStringLiteral("/sym.txt"), QStringLiteral("hl.txt"), options);
    QVERIFY2(symlink.ok, qPrintable(symlink.blockers.join(QStringLiteral("; "))));
    QVERIFY(symlink.warnings.join(' ').contains(QStringLiteral("symbolic link")));
    const auto symReadBack =
        PartitionHfsFileSystemReader::readFileFromImage(imagePath, QStringLiteral("/sym.txt"), 64);
    QVERIFY2(symReadBack.ok, qPrintable(symReadBack.blockers.join(QStringLiteral("; "))));
    QCOMPARE(symReadBack.data, QByteArrayLiteral("hl.txt"));

    // First hard link converts /hl.txt into an inode (link count 2); a second extends
    // the chain (link count 3). Both aliases resolve to the inode's shared data.
    const auto link1 = PartitionHfsFileSystemWriter::createHardlinkFromImage(
        imagePath, QStringLiteral("/hl.txt"), QStringLiteral("/a.txt"), options);
    QVERIFY2(link1.ok, qPrintable(link1.blockers.join(QStringLiteral("; "))));
    QVERIFY(link1.warnings.join(' ').contains(QStringLiteral("link count 2")));
    const auto link2 = PartitionHfsFileSystemWriter::createHardlinkFromImage(
        imagePath, QStringLiteral("/a.txt"), QStringLiteral("/b.txt"), options);
    QVERIFY2(link2.ok, qPrintable(link2.blockers.join(QStringLiteral("; "))));
    QVERIFY(link2.warnings.join(' ').contains(QStringLiteral("link count 3")));

    // The reader follows each 'hlnk' alias to the shared inode in the private
    // metadata directory and returns the inode's data -- the same indirection the
    // macOS kernel performs. All three names read back the identical payload.
    for (const QString& alias :
         {QStringLiteral("/hl.txt"), QStringLiteral("/a.txt"), QStringLiteral("/b.txt")}) {
        const auto aliasRead =
            PartitionHfsFileSystemReader::readFileFromImage(imagePath, alias, 4096);
        QVERIFY2(aliasRead.ok,
                 qPrintable(QStringLiteral("%1: %2").arg(
                     alias, aliasRead.blockers.join(QStringLiteral("; ")))));
        QCOMPARE(aliasRead.data, payload);
    }

    // All three aliases plus the hidden private metadata directory are present; the
    // listing resolves each alias's shared inode so size_bytes reports the inode's
    // 200-byte fork (not the alias's empty fork) and the hard-link flag is set.
    const auto rootListing =
        PartitionHfsFileSystemReader::listDirectoryFromImage(imagePath, QStringLiteral("/"), 50);
    QVERIFY2(rootListing.ok, qPrintable(rootListing.blockers.join(QStringLiteral("; "))));
    for (const QString& name : {QStringLiteral("hl.txt"),
                                QStringLiteral("a.txt"),
                                QStringLiteral("b.txt"),
                                QStringLiteral("sym.txt")}) {
        QVERIFY2(std::any_of(rootListing.entries.cbegin(),
                             rootListing.entries.cend(),
                             [&](const auto& e) { return e.name == name; }),
                 qPrintable(QStringLiteral("missing entry %1").arg(name)));
    }
    for (const auto& entry : rootListing.entries) {
        if (entry.name == QStringLiteral("hl.txt") || entry.name == QStringLiteral("a.txt") ||
            entry.name == QStringLiteral("b.txt")) {
            QVERIFY2(entry.hard_link,
                     qPrintable(QStringLiteral("%1 not flagged hard link").arg(entry.name)));
            QCOMPARE(entry.size_bytes, static_cast<uint64_t>(payload.size()));
            QVERIFY(entry.link_target_id != 0);
        }
        if (entry.name == QStringLiteral("sym.txt")) {
            QVERIFY(entry.symbolic_link);
        }
    }
    QVERIFY2(PartitionHfsFileSystemReader::checkConsistencyFromImage(imagePath, 50).ok,
             "consistency after hard-link create");

    // A non-hard-link cannot be deleted via the hard-link path.
    const auto notLink = PartitionHfsFileSystemWriter::deleteHardlinkFromImage(
        imagePath, QStringLiteral("/sym.txt"), options);
    QVERIFY(!notLink.ok);
    QVERIFY(notLink.blockers.join(' ').contains(QStringLiteral("not a hard link")));

    // Delete the links one by one: count 3 -> 2 -> 1 -> 0 (inode + block reclaimed).
    const auto del1 = PartitionHfsFileSystemWriter::deleteHardlinkFromImage(
        imagePath, QStringLiteral("/b.txt"), options);
    QVERIFY2(del1.ok, qPrintable(del1.blockers.join(QStringLiteral("; "))));
    QVERIFY(del1.warnings.join(' ').contains(QStringLiteral("link count is now 2")));
    const auto del2 = PartitionHfsFileSystemWriter::deleteHardlinkFromImage(
        imagePath, QStringLiteral("/hl.txt"), options);
    QVERIFY2(del2.ok, qPrintable(del2.blockers.join(QStringLiteral("; "))));
    QVERIFY(del2.warnings.join(' ').contains(QStringLiteral("link count is now 1")));
    // One alias remains (count 1); it still resolves through the inode to the data.
    const auto lastLinkRead =
        PartitionHfsFileSystemReader::readFileFromImage(imagePath, QStringLiteral("/a.txt"), 4096);
    QVERIFY2(lastLinkRead.ok, qPrintable(lastLinkRead.blockers.join(QStringLiteral("; "))));
    QCOMPARE(lastLinkRead.data, payload);
    const auto del3 = PartitionHfsFileSystemWriter::deleteHardlinkFromImage(
        imagePath, QStringLiteral("/a.txt"), options);
    QVERIFY2(del3.ok, qPrintable(del3.blockers.join(QStringLiteral("; "))));
    QVERIFY(del3.warnings.join(' ').contains(QStringLiteral("reclaimed")));

    // The last surviving alias's data is gone and the volume is still consistent.
    QVERIFY(!PartitionHfsFileSystemReader::readFileFromImage(imagePath, QStringLiteral("/a.txt"), 1)
                 .ok);
    QVERIFY2(PartitionHfsFileSystemReader::checkConsistencyFromImage(imagePath, 50).ok,
             "consistency after hard-link delete");
}

void PartitionManagerCoreTests::apfsFileSystemReader_rejectsCorruptMetadataChecksum() {
    QByteArray corrupt = apfsRawDetectionFixture();
    QBuffer corruptBuffer(&corrupt);
    QVERIFY(corruptBuffer.open(QIODevice::ReadOnly));

    const auto corruptResult =
        PartitionApfsFileSystemReader::listDirectory(&corruptBuffer, QStringLiteral("/"), 20);
    QVERIFY(!corruptResult.ok);
    QVERIFY(corruptResult.blockers.join(' ').contains(
        QStringLiteral("APFS object checksum failed at block 0")));

    QByteArray stamped = apfsRawDetectionFixture();
    QVERIFY(stampApfsObjectBlock(&stamped, 0));
    QBuffer stampedBuffer(&stamped);
    QVERIFY(stampedBuffer.open(QIODevice::ReadOnly));

    const auto stampedResult =
        PartitionApfsFileSystemReader::listDirectory(&stampedBuffer, QStringLiteral("/"), 20);
    QVERIFY(!stampedResult.ok);
    QVERIFY(
        !stampedResult.blockers.join(' ').contains(QStringLiteral("APFS object checksum failed")));
    QVERIFY(stampedResult.blockers.join(' ').contains(QStringLiteral("APFS seek failed")));
}

void PartitionManagerCoreTests::apfsWriter_computesAndVerifiesObjectChecksums() {
    QByteArray zeroObject(kTestApfsChecksumObjectBytes, '\0');
    const auto zeroChecksum = PartitionApfsWriter::computeObjectChecksum(zeroObject);
    QVERIFY(zeroChecksum.has_value());
    QCOMPARE(*zeroChecksum, kTestApfsChecksumZeroObject);
    QVERIFY(PartitionApfsWriter::stampObjectChecksum(&zeroObject));
    QVERIFY(PartitionApfsWriter::verifyObjectChecksum(zeroObject));

    QByteArray oneWordObject(kTestApfsChecksumObjectBytes, '\0');
    qToLittleEndian<uint32_t>(
        1, reinterpret_cast<uchar*>(oneWordObject.data() + kTestApfsChecksumPayloadOffset));
    const auto oneWordChecksum = PartitionApfsWriter::computeObjectChecksum(oneWordObject);
    QVERIFY(oneWordChecksum.has_value());
    QCOMPARE(*oneWordChecksum, kTestApfsChecksumOneWordObject);
    QVERIFY(PartitionApfsWriter::stampObjectChecksum(&oneWordObject));
    QVERIFY(PartitionApfsWriter::verifyObjectChecksum(oneWordObject));

    oneWordObject[kTestApfsChecksumPayloadOffset] = kMutatedApfsChecksumPayloadByte;
    QVERIFY(!PartitionApfsWriter::verifyObjectChecksum(oneWordObject));

    QByteArray malformed(kTestApfsChecksumObjectBytes - 1, '\0');
    QVERIFY(!PartitionApfsWriter::computeObjectChecksum(malformed).has_value());
    QVERIFY(!PartitionApfsWriter::stampObjectChecksum(nullptr));
}

void PartitionManagerCoreTests::apfsWriter_computesMultiChunkContainerGeometry() {
    constexpr uint64_t kBlocksPerChunk = 32'768;  // one spaceman chunk (4096-byte blocks)
    constexpr uint64_t kChunksPerCib = 126;

    // Empty/degenerate input is fully defined and harmless.
    const auto empty = PartitionApfsWriter::computeContainerGeometry(0);
    QCOMPARE(empty.chunk_count, static_cast<uint64_t>(0));
    QCOMPARE(empty.cib_count, static_cast<uint64_t>(0));
    QCOMPARE(empty.ip_block_count, static_cast<uint64_t>(0));

    // 64 MiB (16384 blocks) -> the current single-chunk certified floor.
    const auto sixtyFour = PartitionApfsWriter::computeContainerGeometry(16'384);
    QCOMPARE(sixtyFour.chunk_count, static_cast<uint64_t>(1));
    QCOMPARE(sixtyFour.cib_count, static_cast<uint64_t>(1));
    QCOMPARE(sixtyFour.chunk_bitmap_block_count, static_cast<uint64_t>(1));
    QVERIFY(sixtyFour.single_chunk);
    QVERIFY(!sixtyFour.multi_cib);
    // Matches the real apple-fresh.img spaceman ip_block_count (6).
    QCOMPARE(sixtyFour.ip_block_count, static_cast<uint64_t>(6));

    // 128 MiB (32768 blocks) is exactly one chunk -> the single-chunk ceiling.
    const auto oneTwentyEight = PartitionApfsWriter::computeContainerGeometry(kBlocksPerChunk);
    QCOMPARE(oneTwentyEight.chunk_count, static_cast<uint64_t>(1));
    QVERIFY(oneTwentyEight.single_chunk);
    QCOMPARE(oneTwentyEight.ip_block_count, static_cast<uint64_t>(6));

    // One block past 128 MiB needs a second chunk (multi-chunk, single CIB).
    const auto justOver = PartitionApfsWriter::computeContainerGeometry(kBlocksPerChunk + 1);
    QCOMPARE(justOver.chunk_count, static_cast<uint64_t>(2));
    QCOMPARE(justOver.cib_count, static_cast<uint64_t>(1));
    QVERIFY(!justOver.single_chunk);
    QVERIFY(!justOver.multi_cib);
    QCOMPARE(justOver.ip_block_count, static_cast<uint64_t>(9));  // 3 * (1 + 2)

    // 512 MiB (131072 blocks) -> 4 chunks, 1 CIB.
    const auto fiveTwelve = PartitionApfsWriter::computeContainerGeometry(131'072);
    QCOMPARE(fiveTwelve.chunk_count, static_cast<uint64_t>(4));
    QCOMPARE(fiveTwelve.cib_count, static_cast<uint64_t>(1));
    QCOMPARE(fiveTwelve.chunk_bitmap_block_count, static_cast<uint64_t>(4));
    QCOMPARE(fiveTwelve.ip_block_count, static_cast<uint64_t>(15));  // 3 * (1 + 4)

    // The real ref/3.apfs geometry: 300032 blocks -> 10 chunks, 1 CIB,
    // ip_block_count 33 (validated host-side against the container).
    const auto refThree = PartitionApfsWriter::computeContainerGeometry(300'032);
    QCOMPARE(refThree.chunk_count, static_cast<uint64_t>(10));
    QCOMPARE(refThree.cib_count, static_cast<uint64_t>(1));
    QCOMPARE(refThree.ip_block_count, static_cast<uint64_t>(33));  // 3 * (1 + 10)

    // Exactly chunks_per_cib chunks still fits one CIB.
    const auto fullCib =
        PartitionApfsWriter::computeContainerGeometry(kChunksPerCib * kBlocksPerChunk);
    QCOMPARE(fullCib.chunk_count, kChunksPerCib);
    QCOMPARE(fullCib.cib_count, static_cast<uint64_t>(1));
    QVERIFY(!fullCib.multi_cib);

    // One chunk past a full CIB crosses into the multi-CIB tier, but stays below
    // the CAB tier: the spaceman lists the cib addresses inline (cab_count 0,
    // matching Apple newfs_apfs / mkapfs), so the CAB tier only begins above
    // cibs_per_cab (507) cibs.
    const auto multiCib =
        PartitionApfsWriter::computeContainerGeometry((kChunksPerCib + 1) * kBlocksPerChunk);
    QCOMPARE(multiCib.chunk_count, kChunksPerCib + 1);
    QCOMPARE(multiCib.cib_count, static_cast<uint64_t>(2));
    QVERIFY(multiCib.multi_cib);
    QCOMPARE(multiCib.cab_count, static_cast<uint64_t>(0));
    QCOMPARE(multiCib.ip_block_count, 3ULL * (2 + (kChunksPerCib + 1)));

    // A 100 GiB container (the harvested Apple newfs_apfs reference) has 800
    // chunks across 7 cibs with cab_count 0.
    const auto hundredGiB = PartitionApfsWriter::computeContainerGeometry(26'214'400);
    QCOMPARE(hundredGiB.chunk_count, static_cast<uint64_t>(800));
    QCOMPARE(hundredGiB.cib_count, static_cast<uint64_t>(7));
    QCOMPARE(hundredGiB.cab_count, static_cast<uint64_t>(0));

    // Partial trailing chunk rounds up.
    const auto partial = PartitionApfsWriter::computeContainerGeometry(kBlocksPerChunk * 3 + 17);
    QCOMPARE(partial.chunk_count, static_cast<uint64_t>(4));
}

PartitionApfsWriteOptions certifiedApfsImageOnlyOptions() {
    PartitionApfsWriteOptions options;
    options.enable_experimental_writer = true;
    options.destructive_certification_evidence = true;
    options.allow_encrypted_or_protected_volume = true;
    options.allow_compressed_file_mutation = true;
    options.allow_snapshots = true;
    options.allow_multi_volume_container = true;
    options.max_payload_bytes = 64ULL * 1024ULL;
    options.evidence_id = QStringLiteral("external.apfs-image-writer-fixture");
    return options;
}

// Options for the on-hardware raw commit path: non-image-only with raw-media hardware
// certification evidence (the production commitRaw* family requires both).
PartitionApfsWriteOptions certifiedApfsRawCommitOptions() {
    PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    options.image_only = false;
    options.raw_media_hardware_certification_evidence = true;
    options.evidence_id = QStringLiteral("external.apfs-raw-commit-fixture");
    return options;
}

constexpr qsizetype kTestApfsTamperBlockSize = 4096;
constexpr qsizetype kTestApfsTamperMagicOffset = 0x20;

uint32_t readTestApfsLe32(const QByteArray& image, qsizetype offset) {
    return static_cast<uint8_t>(image.at(offset)) |
           (static_cast<uint32_t>(static_cast<uint8_t>(image.at(offset + 1))) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(image.at(offset + 2))) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(image.at(offset + 3))) << 24);
}

uint64_t readTestApfsLe64(const QByteArray& image, qsizetype offset) {
    return static_cast<uint64_t>(readTestApfsLe32(image, offset)) |
           (static_cast<uint64_t>(readTestApfsLe32(image, offset + 4)) << 32);
}

qsizetype findApfsMagicBlockOffset(const QByteArray& image, uint32_t magic) {
    for (qsizetype offset = 0; offset + kTestApfsTamperBlockSize <= image.size();
         offset += kTestApfsTamperBlockSize) {
        if (readTestApfsLe32(image, offset + kTestApfsTamperMagicOffset) == magic) {
            return offset;
        }
    }
    return -1;
}

struct ApfsBlockFieldPatch {
    qsizetype block_offset{0};
    qsizetype field_offset{0};
    uint64_t value{0};
    bool field32{false};
};

bool patchAndStampApfsBlock(QByteArray* image, const ApfsBlockFieldPatch& patch) {
    QByteArray block = image->mid(patch.block_offset, kTestApfsTamperBlockSize);
    const int width = patch.field32 ? 4 : 8;
    for (int index = 0; index < width; ++index) {
        block[patch.field_offset + index] = static_cast<char>((patch.value >> (8 * index)) & 0xFF);
    }
    if (!PartitionApfsWriter::stampObjectChecksum(&block)) {
        return false;
    }
    std::copy(block.cbegin(), block.cend(), image->begin() + patch.block_offset);
    return true;
}

bool tamperApfsBlockField(
    const QString& imagePath, uint32_t magic, qsizetype fieldOffset, uint64_t value, bool field32) {
    QByteArray image = readBytes(imagePath);
    const qsizetype blockOffset = findApfsMagicBlockOffset(image, magic);
    if (blockOffset < 0) {
        return false;
    }
    return patchAndStampApfsBlock(&image,
                                  {.block_offset = blockOffset,
                                   .field_offset = fieldOffset,
                                   .value = value,
                                   .field32 = field32}) &&
           writeBytes(imagePath, image);
}

bool tamperApfsVolumeOmapField(const QString& imagePath,
                               qsizetype fieldOffset,
                               uint64_t value,
                               bool field32) {
    constexpr uint32_t kApsbMagic = 0x42'53'50'41;  // 'APSB'
    QByteArray image = readBytes(imagePath);
    const qsizetype apsbOffset = findApfsMagicBlockOffset(image, kApsbMagic);
    if (apsbOffset < 0) {
        return false;
    }
    const qsizetype omapOffset =
        static_cast<qsizetype>(readTestApfsLe64(image, apsbOffset + 0x80)) *
        kTestApfsTamperBlockSize;
    if (omapOffset <= 0 || omapOffset + kTestApfsTamperBlockSize > image.size()) {
        return false;
    }
    return patchAndStampApfsBlock(&image,
                                  {.block_offset = omapOffset,
                                   .field_offset = fieldOffset,
                                   .value = value,
                                   .field32 = field32}) &&
           writeBytes(imagePath, image);
}

void verifyApfsWriterBlockedByDefault(const PartitionFileSystemDetection& detection) {
    PartitionApfsWriteOptions defaults;
    const auto blocked = PartitionApfsWriter::preflightExistingContainer(
        detection, PartitionApfsWriteOperation::CreateFile, defaults);
    QVERIFY(!blocked.allowed);
    const QString blockers = blocked.blockers.join(QStringLiteral("; "));
    QVERIFY(blockers.contains(QStringLiteral("disabled")));
    QVERIFY(blockers.contains(QStringLiteral("certification evidence")));
    QVERIFY(blockers.contains(QStringLiteral("checkpoint")));
    QVERIFY(blockers.contains(QStringLiteral("multi-volume")));
    QVERIFY(blocked.required_evidence.join(' ').contains(QStringLiteral("Object-map")));
    QVERIFY(blocked.required_evidence.join(' ').contains(QStringLiteral("Crash-interruption")));
}

void verifyApfsImageOnlyPlanShape(const PartitionApfsImageMutationPlan& plan) {
    QVERIFY2(plan.buildable, qPrintable(plan.preflight.blockers.join(QStringLiteral("; "))));
    QVERIFY(!plan.executable);
    QCOMPARE(plan.target_path, QStringLiteral("/Users/Test/new-file.txt"));
    QVERIFY(plan.execution_blockers.join(' ').contains(QStringLiteral("structured evidence")));
    QVERIFY(plan.steps.size() >= kMinimumApfsMutationPlanSteps);
    QVERIFY(std::any_of(
        plan.steps.cbegin(), plan.steps.cend(), [](const PartitionApfsImageMutationStep& step) {
            return step.name == QStringLiteral("checkpoint-commit") && step.requires_checkpoint;
        }));
    QVERIFY(std::all_of(
        plan.steps.cbegin(), plan.steps.cend(), [](const PartitionApfsImageMutationStep& step) {
            return !step.required_evidence.isEmpty();
        }));
    QVERIFY(std::any_of(
        plan.steps.cbegin(), plan.steps.cend(), [](const PartitionApfsImageMutationStep& step) {
            return step.required_evidence.contains(QStringLiteral("target-readback")) &&
                   step.name == QStringLiteral("post-verify");
        }));
    QVERIFY(plan.preflight.required_evidence.join(' ').contains(QStringLiteral("hash-chain")));
    QVERIFY(
        plan.preflight.required_evidence.join(' ').contains(QStringLiteral("Rollback-boundary")));
    QVERIFY(plan.post_apply_verification.join(' ').contains(QStringLiteral("checksum")));
}

void verifyApfsImageOnlyExecutionEvidenceGate(const PartitionApfsImageMutationPlan& plan) {
    PartitionApfsWriterExecutionEvidence missingEvidence;
    missingEvidence.evidence_id = plan.evidence_id;
    missingEvidence.operation = plan.operation;
    missingEvidence.target_path = plan.target_path;
    const auto missingEvidenceGate =
        PartitionApfsWriter::validateImageOnlyExecutionEvidence(plan, missingEvidence);
    QVERIFY(!missingEvidenceGate.allowed);
    QVERIFY(missingEvidenceGate.blockers.join(' ').contains(QStringLiteral("checksum evidence")));
    QVERIFY(missingEvidenceGate.blockers.join(' ').contains(QStringLiteral("source image hash")));
    QVERIFY(missingEvidenceGate.blockers.join(' ').contains(QStringLiteral("target read-back")));
    QVERIFY(missingEvidenceGate.blockers.join(' ').contains(QStringLiteral("rollback-boundary")));
    QVERIFY(missingEvidenceGate.blockers.join(' ').contains(QStringLiteral("artifact paths")));

    PartitionApfsWriterExecutionEvidence imageEvidence;
    imageEvidence.evidence_id = plan.evidence_id;
    imageEvidence.operation = plan.operation;
    imageEvidence.target_path = plan.target_path;
    imageEvidence.structure_mapping_verified = true;
    imageEvidence.object_checksum_vectors_verified = true;
    imageEvidence.source_image_hash_verified = true;
    imageEvidence.scratch_image_hash_verified = true;
    imageEvidence.copy_on_write_checkpoint_verified = true;
    imageEvidence.object_map_update_verified = true;
    imageEvidence.space_manager_accounting_verified = true;
    imageEvidence.fsck_validation_verified = true;
    imageEvidence.target_readback_verified = true;
    imageEvidence.crash_replay_verified = true;
    imageEvidence.rollback_boundary_verified = true;
    imageEvidence.artifacts = {QStringLiteral("artifacts/apfs-image-writer/report.json")};
    const auto imageEvidenceGate =
        PartitionApfsWriter::validateImageOnlyExecutionEvidence(plan, imageEvidence);
    QVERIFY2(imageEvidenceGate.allowed,
             qPrintable(imageEvidenceGate.blockers.join(QStringLiteral("; "))));
    QVERIFY(imageEvidenceGate.warnings.join(' ').contains(QStringLiteral("raw-media")));

    PartitionApfsWriterExecutionEvidence unsafeArtifactEvidence = imageEvidence;
    unsafeArtifactEvidence.artifacts = {QStringLiteral("../report.json")};
    const auto unsafeArtifactGate =
        PartitionApfsWriter::validateImageOnlyExecutionEvidence(plan, unsafeArtifactEvidence);
    QVERIFY(!unsafeArtifactGate.allowed);
    QVERIFY(unsafeArtifactGate.blockers.join(' ').contains(QStringLiteral("artifact path")));

    PartitionApfsWriterExecutionEvidence mismatchEvidence = imageEvidence;
    mismatchEvidence.target_path = QStringLiteral("/Users/Test/other-file.txt");
    const auto mismatchGate =
        PartitionApfsWriter::validateImageOnlyExecutionEvidence(plan, mismatchEvidence);
    QVERIFY(!mismatchGate.allowed);
    QVERIFY(mismatchGate.blockers.join(' ').contains(QStringLiteral("target path")));
}

void verifyApfsWholeContainerPlanShapes(const PartitionFileSystemDetection& detection,
                                        const PartitionApfsWriteOptions& options) {
    const auto formatPlan = PartitionApfsWriter::planImageOnlyFormat(
        128ULL * 1024ULL * 1024ULL, 4096, QStringLiteral("SAK APFS"), options);
    QVERIFY2(formatPlan.buildable,
             qPrintable(formatPlan.preflight.blockers.join(QStringLiteral("; "))));
    QVERIFY(!formatPlan.executable);
    QCOMPARE(formatPlan.operation, QStringLiteral("Format APFS container"));
    QCOMPARE(formatPlan.volume_name, QStringLiteral("SAK APFS"));
    QCOMPARE(formatPlan.target_container_bytes, 128ULL * 1024ULL * 1024ULL);
    QCOMPARE(formatPlan.block_size_bytes, 4096u);
    QVERIFY(std::any_of(formatPlan.steps.cbegin(),
                        formatPlan.steps.cend(),
                        [](const PartitionApfsImageMutationStep& step) {
                            return step.name == QStringLiteral("container-layout") &&
                                   step.writes_metadata;
                        }));
    QVERIFY(std::any_of(formatPlan.steps.cbegin(),
                        formatPlan.steps.cend(),
                        [](const PartitionApfsImageMutationStep& step) {
                            return step.name == QStringLiteral("space-manager-init") &&
                                   step.required_evidence.contains(
                                       QStringLiteral("space-manager-accounting"));
                        }));
    QVERIFY(formatPlan.post_apply_verification.join(' ').contains(
        QStringLiteral("empty root directory")));
    verifyApfsImageOnlyExecutionEvidenceGate(formatPlan);

    const auto tinyFormatPlan = PartitionApfsWriter::planImageOnlyFormat(
        8ULL * 1024ULL * 1024ULL, 4096, QStringLiteral("SAK APFS"), options);
    QVERIFY(!tinyFormatPlan.buildable);
    QVERIFY(tinyFormatPlan.preflight.blockers.join(' ').contains(QStringLiteral("64 MiB")));

    const auto unsafeNamePlan = PartitionApfsWriter::planImageOnlyFormat(
        128ULL * 1024ULL * 1024ULL, 4096, QStringLiteral("Bad/Name"), options);
    QVERIFY(!unsafeNamePlan.buildable);
    QVERIFY(unsafeNamePlan.preflight.blockers.join(' ').contains(QStringLiteral("separator")));

    const auto repairPlan = PartitionApfsWriter::planImageOnlyMutation(
        detection, PartitionApfsWriteOperation::RepairContainer, options, {});
    QVERIFY2(repairPlan.buildable,
             qPrintable(repairPlan.preflight.blockers.join(QStringLiteral("; "))));
    QCOMPARE(repairPlan.operation, QStringLiteral("Repair APFS container"));
    QVERIFY(std::any_of(repairPlan.steps.cbegin(),
                        repairPlan.steps.cend(),
                        [](const PartitionApfsImageMutationStep& step) {
                            return step.name == QStringLiteral("repair-scan");
                        }));
    QVERIFY(std::any_of(repairPlan.steps.cbegin(),
                        repairPlan.steps.cend(),
                        [](const PartitionApfsImageMutationStep& step) {
                            return step.name == QStringLiteral("repaired-checkpoint-commit") &&
                                   step.requires_checkpoint;
                        }));
    verifyApfsImageOnlyExecutionEvidenceGate(repairPlan);

    const auto resizePlan = PartitionApfsWriter::planImageOnlyMutation(
        detection, PartitionApfsWriteOperation::ResizeContainer, options, {});
    QVERIFY2(resizePlan.buildable,
             qPrintable(resizePlan.preflight.blockers.join(QStringLiteral("; "))));
    QCOMPARE(resizePlan.operation, QStringLiteral("Resize APFS container"));
    QVERIFY(std::any_of(resizePlan.steps.cbegin(),
                        resizePlan.steps.cend(),
                        [](const PartitionApfsImageMutationStep& step) {
                            return step.name == QStringLiteral("space-manager-rebalance");
                        }));

    const auto labelPlan = PartitionApfsWriter::planImageOnlyMutation(
        detection, PartitionApfsWriteOperation::ChangeVolumeLabel, options, {});
    QVERIFY2(labelPlan.buildable,
             qPrintable(labelPlan.preflight.blockers.join(QStringLiteral("; "))));
    QCOMPARE(labelPlan.operation, QStringLiteral("Change APFS volume label"));
    QVERIFY(std::any_of(labelPlan.steps.cbegin(),
                        labelPlan.steps.cend(),
                        [](const PartitionApfsImageMutationStep& step) {
                            return step.name == QStringLiteral("volume-superblock-label-update") &&
                                   step.writes_metadata;
                        }));
    QVERIFY(labelPlan.post_apply_verification.join(' ').contains(QStringLiteral("volume name")));

    const auto badRepairTarget =
        PartitionApfsWriter::planImageOnlyMutation(detection,
                                                   PartitionApfsWriteOperation::RepairContainer,
                                                   options,
                                                   QStringLiteral("/file.txt"));
    QVERIFY(!badRepairTarget.buildable);
    QVERIFY(
        badRepairTarget.preflight.blockers.join(' ').contains(QStringLiteral("whole-container")));
}

void verifyApfsImageOnlyFormatBuild(const PartitionApfsWriteOptions& options) {
    PartitionApfsWriteOptions generatedOnlyOptions = options;
    generatedOnlyOptions.allow_encrypted_or_protected_volume = false;
    generatedOnlyOptions.allow_compressed_file_mutation = false;
    generatedOnlyOptions.allow_snapshots = false;
    generatedOnlyOptions.allow_multi_volume_container = false;

    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString imagePath = QDir(temp.path()).filePath(QStringLiteral("formatted.apfs"));
    const auto build = PartitionApfsWriter::buildImageOnlyFormatImage(
        {.image_path = imagePath,
         .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
         .block_size_bytes = 4096,
         .volume_name = QStringLiteral("SAK Empty"),
         .options = generatedOnlyOptions});
    QVERIFY2(build.ok, qPrintable(build.blockers.join(QStringLiteral("; "))));
    QVERIFY(QFileInfo::exists(imagePath));
    QVERIFY(!build.image_sha256.isEmpty());
    QCOMPARE(build.plan.volume_name, QStringLiteral("SAK Empty"));
    QVERIFY(build.warnings.join(' ').contains(QStringLiteral("image-only")));

    const auto detection =
        PartitionFileSystemDetector::detectFromDevicePath(imagePath, 0, 64ULL * 1024ULL * 1024ULL);
    QVERIFY(detection.has_value());
    QCOMPARE(detection->file_system, QStringLiteral("APFS"));
    QCOMPARE(detection->total_bytes, 64ULL * 1024ULL * 1024ULL);
    QVERIFY(detection->free_bytes > 0);
    QVERIFY(detection->details.join(' ').contains(QStringLiteral("APFS free bytes")));

    const auto rootListing =
        PartitionApfsFileSystemReader::listDirectoryFromImage(imagePath, QStringLiteral("/"), 20);
    QVERIFY2(rootListing.ok, qPrintable(rootListing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(rootListing.volume_name, QStringLiteral("SAK Empty"));
    QCOMPARE(rootListing.entries.size(), 0);

    const QString relabeledImagePath = QDir(temp.path()).filePath(QStringLiteral("relabeled.apfs"));
    const auto relabeled = PartitionApfsWriter::changeImageOnlyVolumeLabel(
        {.source_image_path = imagePath,
         .written_image_path = relabeledImagePath,
         .volume_name = QStringLiteral("SAK Relabeled"),
         .options = generatedOnlyOptions});
    QVERIFY2(relabeled.ok, qPrintable(relabeled.blockers.join(QStringLiteral("; "))));
    QCOMPARE(relabeled.old_volume_name, QStringLiteral("SAK Empty"));
    QCOMPARE(relabeled.new_volume_name, QStringLiteral("SAK Relabeled"));
    QCOMPARE(relabeled.plan.operation, QStringLiteral("Change APFS volume label"));
    QVERIFY(!relabeled.written_image_sha256.isEmpty());
    const auto relabeledListing = PartitionApfsFileSystemReader::listDirectoryFromImage(
        relabeledImagePath, QStringLiteral("/"), 20);
    QVERIFY2(relabeledListing.ok, qPrintable(relabeledListing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(relabeledListing.volume_name, QStringLiteral("SAK Relabeled"));

    const auto invalidRelabel = PartitionApfsWriter::changeImageOnlyVolumeLabel(
        {.source_image_path = imagePath,
         .written_image_path = QDir(temp.path()).filePath(QStringLiteral("bad-label.apfs")),
         .volume_name = QStringLiteral("Bad/Name"),
         .options = generatedOnlyOptions});
    QVERIFY(!invalidRelabel.ok);
    QVERIFY(invalidRelabel.blockers.join(' ').contains(QStringLiteral("separator")));

    const QString existingTargetPath = QDir(temp.path()).filePath(QStringLiteral("existing.apfs"));
    QFile existingTarget(existingTargetPath);
    QVERIFY(existingTarget.open(QIODevice::WriteOnly));
    QVERIFY(existingTarget.resize(64ULL * 1024ULL * 1024ULL));
    existingTarget.close();

    const auto existingFormatBlocked = PartitionApfsWriter::formatExistingImageOnlyContainer(
        {.image_path = existingTargetPath,
         .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
         .block_size_bytes = 4096,
         .volume_name = QStringLiteral("SAK Existing"),
         .options = generatedOnlyOptions});
    QVERIFY(!existingFormatBlocked.ok);
    QVERIFY(existingFormatBlocked.blockers.join(' ').contains(QStringLiteral("confirmation")));
    QVERIFY(QFileInfo::exists(existingTargetPath));

    const auto existingFormat = PartitionApfsWriter::formatExistingImageOnlyContainer(
        {.image_path = existingTargetPath,
         .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
         .block_size_bytes = 4096,
         .volume_name = QStringLiteral("SAK Existing"),
         .target_wipe_confirmed = true,
         .options = generatedOnlyOptions});
    QVERIFY2(existingFormat.ok, qPrintable(existingFormat.blockers.join(QStringLiteral("; "))));
    QVERIFY(!existingFormat.image_sha256.isEmpty());
    const auto existingDetection = PartitionFileSystemDetector::detectFromDevicePath(
        existingTargetPath, 0, 64ULL * 1024ULL * 1024ULL);
    QVERIFY(existingDetection.has_value());
    QCOMPARE(existingDetection->file_system, QStringLiteral("APFS"));
    const auto existingListing = PartitionApfsFileSystemReader::listDirectoryFromImage(
        existingTargetPath, QStringLiteral("/"), 20);
    QVERIFY2(existingListing.ok, qPrintable(existingListing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(existingListing.volume_name, QStringLiteral("SAK Existing"));
    QCOMPARE(existingListing.entries.size(), 0);

    const QString fakeRawTarget =
        QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk99\\Partition1");
    const auto rawOptInBlocked = PartitionApfsWriter::formatExistingContainerTarget(
        {.image_path = fakeRawTarget,
         .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
         .block_size_bytes = 4096,
         .volume_name = QStringLiteral("SAK Raw"),
         .target_wipe_confirmed = true,
         .options = generatedOnlyOptions});
    QVERIFY(!rawOptInBlocked.ok);
    QVERIFY(rawOptInBlocked.blockers.join(' ').contains(QStringLiteral("raw-device opt-in")));

    const auto rawImageOnlyBlocked = PartitionApfsWriter::formatExistingContainerTarget(
        {.image_path = fakeRawTarget,
         .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
         .block_size_bytes = 4096,
         .volume_name = QStringLiteral("SAK Raw"),
         .target_wipe_confirmed = true,
         .allow_raw_device_target = true,
         .options = generatedOnlyOptions});
    QVERIFY(!rawImageOnlyBlocked.ok);
    QVERIFY(rawImageOnlyBlocked.blockers.join(' ').contains(QStringLiteral("non-image-only")));

    PartitionApfsWriteOptions certifiedRaw = generatedOnlyOptions;
    certifiedRaw.image_only = false;
    certifiedRaw.raw_media_hardware_certification_evidence = true;
    const auto rawWriteNonRawBlocked =
        PartitionApfsWriter::writeRawRootFile({.target_path = imagePath,
                                               .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
                                               .file_name = QStringLiteral("raw.txt"),
                                               .file_data = QByteArray("raw proof"),
                                               .target_write_confirmed = true,
                                               .allow_raw_device_target = true,
                                               .options = certifiedRaw});
    QVERIFY(!rawWriteNonRawBlocked.ok);
    QVERIFY(rawWriteNonRawBlocked.blockers.join(' ').contains(QStringLiteral("raw-device path")));

    const auto rawWriteConfirmationBlocked =
        PartitionApfsWriter::writeRawRootFile({.target_path = fakeRawTarget,
                                               .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
                                               .file_name = QStringLiteral("raw.txt"),
                                               .file_data = QByteArray("raw proof"),
                                               .allow_raw_device_target = true,
                                               .options = certifiedRaw});
    QVERIFY(!rawWriteConfirmationBlocked.ok);
    QVERIFY(
        rawWriteConfirmationBlocked.blockers.join(' ').contains(QStringLiteral("confirmation")));

    const auto rawPatchNonRawBlocked =
        PartitionApfsWriter::patchRawRootFile({.target_path = imagePath,
                                               .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
                                               .file_name = QStringLiteral("raw.txt"),
                                               .patch_offset_bytes = 0,
                                               .patch_data = QByteArray("raw patch"),
                                               .target_write_confirmed = true,
                                               .allow_raw_device_target = true,
                                               .options = certifiedRaw});
    QVERIFY(!rawPatchNonRawBlocked.ok);
    QVERIFY(rawPatchNonRawBlocked.blockers.join(' ').contains(QStringLiteral("raw-device path")));

    const auto rawDeleteConfirmationBlocked =
        PartitionApfsWriter::deleteRawRootFile({.target_path = fakeRawTarget,
                                                .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
                                                .file_name = QStringLiteral("raw.txt"),
                                                .allow_raw_device_target = true,
                                                .options = certifiedRaw});
    QVERIFY(!rawDeleteConfirmationBlocked.ok);
    QVERIFY(
        rawDeleteConfirmationBlocked.blockers.join(' ').contains(QStringLiteral("confirmation")));

    const auto rawLabelNonRawBlocked = PartitionApfsWriter::changeRawVolumeLabel(
        {.target_path = imagePath,
         .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
         .volume_name = QStringLiteral("Raw Relabeled"),
         .target_write_confirmed = true,
         .allow_raw_device_target = true,
         .options = certifiedRaw});
    QVERIFY(!rawLabelNonRawBlocked.ok);
    QVERIFY(rawLabelNonRawBlocked.blockers.join(' ').contains(QStringLiteral("raw-device path")));

    const auto rawLabelConfirmationBlocked = PartitionApfsWriter::changeRawVolumeLabel(
        {.target_path = fakeRawTarget,
         .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
         .volume_name = QStringLiteral("Raw Relabeled"),
         .allow_raw_device_target = true,
         .options = certifiedRaw});
    QVERIFY(!rawLabelConfirmationBlocked.ok);
    QVERIFY(
        rawLabelConfirmationBlocked.blockers.join(' ').contains(QStringLiteral("confirmation")));

    const auto rawRepairImageOnlyBlocked = PartitionApfsWriter::repairRawObjectChecksums(
        {.target_path = fakeRawTarget,
         .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
         .target_repair_confirmed = true,
         .allow_raw_device_target = true,
         .options = generatedOnlyOptions});
    QVERIFY(!rawRepairImageOnlyBlocked.ok);
    QVERIFY(
        rawRepairImageOnlyBlocked.blockers.join(' ').contains(QStringLiteral("non-image-only")));

    const auto overwriteBlocked = PartitionApfsWriter::buildImageOnlyFormatImage(
        {.image_path = imagePath,
         .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
         .block_size_bytes = 4096,
         .volume_name = QStringLiteral("SAK Empty"),
         .options = generatedOnlyOptions});
    QVERIFY(!overwriteBlocked.ok);
    QVERIFY(overwriteBlocked.blockers.join(' ').contains(QStringLiteral("overwrite")));

    PartitionApfsWriteOptions defaults;
    const auto defaultsBlocked = PartitionApfsWriter::buildImageOnlyFormatImage(
        {.image_path = QDir(temp.path()).filePath(QStringLiteral("blocked.apfs")),
         .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
         .block_size_bytes = 4096,
         .volume_name = QStringLiteral("SAK Empty"),
         .options = defaults});
    QVERIFY(!defaultsBlocked.ok);
    QVERIFY(defaultsBlocked.blockers.join(' ').contains(QStringLiteral("disabled")));

    const QString seededImagePath = QDir(temp.path()).filePath(QStringLiteral("seeded.apfs"));
    QByteArray seedData;
    while (seedData.size() < 9000) {
        seedData.append("APFS seed file proof across contiguous writer blocks.\n");
    }
    seedData.resize(9000);

    const QString writtenImagePath = QDir(temp.path()).filePath(QStringLiteral("written.apfs"));
    const auto writeBuild =
        PartitionApfsWriter::writeImageOnlyRootFile({.source_image_path = imagePath,
                                                     .written_image_path = writtenImagePath,
                                                     .file_name = QStringLiteral("proof.txt"),
                                                     .file_data = seedData,
                                                     .options = generatedOnlyOptions});
    QVERIFY2(writeBuild.ok, qPrintable(writeBuild.blockers.join(QStringLiteral("; "))));
    QCOMPARE(writeBuild.plan.operation,
             PartitionApfsWriter::operationName(PartitionApfsWriteOperation::CreateFile));
    QVERIFY(writeBuild.written_data_blocks > 1);
    QVERIFY(!writeBuild.written_image_sha256.isEmpty());
    const auto writtenListing = PartitionApfsFileSystemReader::listDirectoryFromImage(
        writtenImagePath, QStringLiteral("/"), 20);
    QVERIFY2(writtenListing.ok, qPrintable(writtenListing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(writtenListing.volume_name, QStringLiteral("SAK Empty"));
    QCOMPARE(writtenListing.entries.size(), 1);
    QCOMPARE(writtenListing.entries.first().name, QStringLiteral("proof.txt"));
    const auto writtenRead = PartitionApfsFileSystemReader::readFileFromImage(
        writtenImagePath, QStringLiteral("/proof.txt"), static_cast<uint64_t>(seedData.size()));
    QVERIFY2(writtenRead.ok, qPrintable(writtenRead.blockers.join(QStringLiteral("; "))));
    QCOMPARE(writtenRead.data, seedData);

    const auto seededBuild = PartitionApfsWriter::buildImageOnlyFormatImageWithSeedFile(
        {.image_path = seededImagePath,
         .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
         .block_size_bytes = 4096,
         .volume_name = QStringLiteral("SAK Seeded"),
         .seed_file_name = QStringLiteral("proof.txt"),
         .seed_file_data = seedData,
         .options = generatedOnlyOptions});
    QVERIFY2(seededBuild.ok, qPrintable(seededBuild.blockers.join(QStringLiteral("; "))));
    const auto seededListing = PartitionApfsFileSystemReader::listDirectoryFromImage(
        seededImagePath, QStringLiteral("/"), 20);
    QVERIFY2(seededListing.ok, qPrintable(seededListing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(seededListing.volume_name, QStringLiteral("SAK Seeded"));
    QCOMPARE(seededListing.entries.size(), 1);
    QCOMPARE(seededListing.entries.first().name, QStringLiteral("proof.txt"));
    QCOMPARE(seededListing.entries.first().size_bytes, static_cast<uint64_t>(seedData.size()));

    const auto seededRead = PartitionApfsFileSystemReader::readFileFromImage(
        seededImagePath, QStringLiteral("/proof.txt"), static_cast<uint64_t>(seedData.size()));
    QVERIFY2(seededRead.ok, qPrintable(seededRead.blockers.join(QStringLiteral("; "))));
    QCOMPARE(seededRead.data, seedData);

    QByteArray secondSeedData("second generated APFS file proof");
    const QString nonEmptyWritePath =
        QDir(temp.path()).filePath(QStringLiteral("non-empty-write.apfs"));
    const auto nonEmptyWrite =
        PartitionApfsWriter::writeImageOnlyRootFile({.source_image_path = seededImagePath,
                                                     .written_image_path = nonEmptyWritePath,
                                                     .file_name = QStringLiteral("other.txt"),
                                                     .file_data = secondSeedData,
                                                     .options = generatedOnlyOptions});
    QVERIFY2(nonEmptyWrite.ok, qPrintable(nonEmptyWrite.blockers.join(QStringLiteral("; "))));
    QCOMPARE(nonEmptyWrite.plan.operation,
             PartitionApfsWriter::operationName(PartitionApfsWriteOperation::CreateFile));
    const auto nonEmptyListing = PartitionApfsFileSystemReader::listDirectoryFromImage(
        nonEmptyWritePath, QStringLiteral("/"), 20);
    QVERIFY2(nonEmptyListing.ok, qPrintable(nonEmptyListing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(nonEmptyListing.entries.size(), 2);
    const auto preservedRead = PartitionApfsFileSystemReader::readFileFromImage(
        nonEmptyWritePath, QStringLiteral("/proof.txt"), static_cast<uint64_t>(seedData.size()));
    QVERIFY2(preservedRead.ok, qPrintable(preservedRead.blockers.join(QStringLiteral("; "))));
    QCOMPARE(preservedRead.data, seedData);
    const auto secondRead = PartitionApfsFileSystemReader::readFileFromImage(
        nonEmptyWritePath,
        QStringLiteral("/other.txt"),
        static_cast<uint64_t>(secondSeedData.size()));
    QVERIFY2(secondRead.ok, qPrintable(secondRead.blockers.join(QStringLiteral("; "))));
    QCOMPARE(secondRead.data, secondSeedData);

    QByteArray replacementSeedData;
    while (replacementSeedData.size() < 7000) {
        replacementSeedData.append("replacement generated APFS file proof.\n");
    }
    replacementSeedData.resize(7000);
    const QString replaceWritePath =
        QDir(temp.path()).filePath(QStringLiteral("replace-write.apfs"));
    const auto replaceWrite =
        PartitionApfsWriter::writeImageOnlyRootFile({.source_image_path = nonEmptyWritePath,
                                                     .written_image_path = replaceWritePath,
                                                     .file_name = QStringLiteral("proof.txt"),
                                                     .file_data = replacementSeedData,
                                                     .options = generatedOnlyOptions});
    QVERIFY2(replaceWrite.ok, qPrintable(replaceWrite.blockers.join(QStringLiteral("; "))));
    QCOMPARE(replaceWrite.plan.operation,
             PartitionApfsWriter::operationName(PartitionApfsWriteOperation::ReplaceFile));
    const auto replaceListing = PartitionApfsFileSystemReader::listDirectoryFromImage(
        replaceWritePath, QStringLiteral("/"), 20);
    QVERIFY2(replaceListing.ok, qPrintable(replaceListing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(replaceListing.entries.size(), 2);
    const auto replacementRead = PartitionApfsFileSystemReader::readFileFromImage(
        replaceWritePath,
        QStringLiteral("/proof.txt"),
        static_cast<uint64_t>(replacementSeedData.size()));
    QVERIFY2(replacementRead.ok, qPrintable(replacementRead.blockers.join(QStringLiteral("; "))));
    QCOMPARE(replacementRead.data, replacementSeedData);
    const auto otherPreservedAfterReplace = PartitionApfsFileSystemReader::readFileFromImage(
        replaceWritePath,
        QStringLiteral("/other.txt"),
        static_cast<uint64_t>(secondSeedData.size()));
    QVERIFY2(otherPreservedAfterReplace.ok,
             qPrintable(otherPreservedAfterReplace.blockers.join(QStringLiteral("; "))));
    QCOMPARE(otherPreservedAfterReplace.data, secondSeedData);

    QByteArray patchPayload("APFS byte-range patch proof");
    QByteArray patchedReplacement = replacementSeedData;
    constexpr uint64_t kPatchOffset = 512;
    std::copy(patchPayload.cbegin(),
              patchPayload.cend(),
              patchedReplacement.begin() + static_cast<qsizetype>(kPatchOffset));
    const QString patchWritePath = QDir(temp.path()).filePath(QStringLiteral("patch-write.apfs"));
    const auto patchWrite =
        PartitionApfsWriter::patchImageOnlyRootFile({.source_image_path = replaceWritePath,
                                                     .written_image_path = patchWritePath,
                                                     .file_name = QStringLiteral("proof.txt"),
                                                     .patch_offset_bytes = kPatchOffset,
                                                     .patch_data = patchPayload,
                                                     .options = generatedOnlyOptions});
    QVERIFY2(patchWrite.ok, qPrintable(patchWrite.blockers.join(QStringLiteral("; "))));
    QCOMPARE(patchWrite.plan.operation,
             PartitionApfsWriter::operationName(PartitionApfsWriteOperation::ReplaceFile));
    QCOMPARE(patchWrite.file_bytes, static_cast<uint64_t>(replacementSeedData.size()));
    QCOMPARE(patchWrite.patch_offset_bytes, kPatchOffset);
    QCOMPARE(patchWrite.patch_bytes, static_cast<uint64_t>(patchPayload.size()));
    QCOMPARE(patchWrite.patch_sha256, sha256Hex(patchPayload));
    QCOMPARE(patchWrite.readback_sha256, sha256Hex(patchedReplacement));
    const auto patchedRead = PartitionApfsFileSystemReader::readFileFromImage(
        patchWritePath,
        QStringLiteral("/proof.txt"),
        static_cast<uint64_t>(patchedReplacement.size()));
    QVERIFY2(patchedRead.ok, qPrintable(patchedRead.blockers.join(QStringLiteral("; "))));
    QCOMPARE(patchedRead.data, patchedReplacement);
    const auto otherPreservedAfterPatch = PartitionApfsFileSystemReader::readFileFromImage(
        patchWritePath, QStringLiteral("/other.txt"), static_cast<uint64_t>(secondSeedData.size()));
    QVERIFY2(otherPreservedAfterPatch.ok,
             qPrintable(otherPreservedAfterPatch.blockers.join(QStringLiteral("; "))));
    QCOMPARE(otherPreservedAfterPatch.data, secondSeedData);

    const auto rangeBlockedPatch = PartitionApfsWriter::patchImageOnlyRootFile(
        {.source_image_path = replaceWritePath,
         .written_image_path =
             QDir(temp.path()).filePath(QStringLiteral("range-blocked-patch.apfs")),
         .file_name = QStringLiteral("proof.txt"),
         .patch_offset_bytes = static_cast<uint64_t>(replacementSeedData.size()),
         .patch_data = QByteArray("too-large"),
         .options = generatedOnlyOptions});
    QVERIFY(!rangeBlockedPatch.ok);
    QVERIFY(rangeBlockedPatch.blockers.join(' ').contains(QStringLiteral("inside")));

    const QString deleteWritePath = QDir(temp.path()).filePath(QStringLiteral("delete-write.apfs"));
    const auto deleteWrite =
        PartitionApfsWriter::deleteImageOnlyRootFile({.source_image_path = patchWritePath,
                                                      .written_image_path = deleteWritePath,
                                                      .file_name = QStringLiteral("other.txt"),
                                                      .options = generatedOnlyOptions});
    QVERIFY2(deleteWrite.ok, qPrintable(deleteWrite.blockers.join(QStringLiteral("; "))));
    QCOMPARE(deleteWrite.plan.operation,
             PartitionApfsWriter::operationName(PartitionApfsWriteOperation::DeleteFile));
    QCOMPARE(deleteWrite.deleted_file_bytes, static_cast<uint64_t>(secondSeedData.size()));
    QCOMPARE(deleteWrite.deleted_file_sha256, sha256Hex(secondSeedData));
    QVERIFY(deleteWrite.freed_data_blocks > 0);
    const auto deleteListing = PartitionApfsFileSystemReader::listDirectoryFromImage(
        deleteWritePath, QStringLiteral("/"), 20);
    QVERIFY2(deleteListing.ok, qPrintable(deleteListing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(deleteListing.entries.size(), 1);
    QCOMPARE(deleteListing.entries.first().name, QStringLiteral("proof.txt"));
    const auto deletedRead = PartitionApfsFileSystemReader::readFileFromImage(
        deleteWritePath,
        QStringLiteral("/other.txt"),
        static_cast<uint64_t>(secondSeedData.size()));
    QVERIFY(!deletedRead.ok);
    const auto preservedAfterDelete = PartitionApfsFileSystemReader::readFileFromImage(
        deleteWritePath,
        QStringLiteral("/proof.txt"),
        static_cast<uint64_t>(patchedReplacement.size()));
    QVERIFY2(preservedAfterDelete.ok,
             qPrintable(preservedAfterDelete.blockers.join(QStringLiteral("; "))));
    QCOMPARE(preservedAfterDelete.data, patchedReplacement);

    const QString emptyAfterDeletePath =
        QDir(temp.path()).filePath(QStringLiteral("empty-after-delete.apfs"));
    const auto deleteLastWrite =
        PartitionApfsWriter::deleteImageOnlyRootFile({.source_image_path = deleteWritePath,
                                                      .written_image_path = emptyAfterDeletePath,
                                                      .file_name = QStringLiteral("proof.txt"),
                                                      .options = generatedOnlyOptions});
    QVERIFY2(deleteLastWrite.ok, qPrintable(deleteLastWrite.blockers.join(QStringLiteral("; "))));
    const auto emptyAfterDeleteListing = PartitionApfsFileSystemReader::listDirectoryFromImage(
        emptyAfterDeletePath, QStringLiteral("/"), 20);
    QVERIFY2(emptyAfterDeleteListing.ok,
             qPrintable(emptyAfterDeleteListing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(emptyAfterDeleteListing.entries.size(), 0);

    const QString directoryCreatePath =
        QDir(temp.path()).filePath(QStringLiteral("directory-create.apfs"));
    const auto directoryCreate = PartitionApfsWriter::createImageOnlyRootDirectory(
        {.source_image_path = emptyAfterDeletePath,
         .written_image_path = directoryCreatePath,
         .directory_name = QStringLiteral("Proof Folder"),
         .options = generatedOnlyOptions});
    QVERIFY2(directoryCreate.ok, qPrintable(directoryCreate.blockers.join(QStringLiteral("; "))));
    QCOMPARE(directoryCreate.plan.operation,
             PartitionApfsWriter::operationName(PartitionApfsWriteOperation::CreateDirectory));
    const auto directoryCreateListing = PartitionApfsFileSystemReader::listDirectoryFromImage(
        directoryCreatePath, QStringLiteral("/"), 20);
    QVERIFY2(directoryCreateListing.ok,
             qPrintable(directoryCreateListing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(directoryCreateListing.entries.size(), 1);
    QCOMPARE(directoryCreateListing.entries.first().name, QStringLiteral("Proof Folder"));
    QVERIFY(directoryCreateListing.entries.first().directory);
    const auto emptyDirectoryListing = PartitionApfsFileSystemReader::listDirectoryFromImage(
        directoryCreatePath, QStringLiteral("/Proof Folder"), 20);
    QVERIFY2(emptyDirectoryListing.ok,
             qPrintable(emptyDirectoryListing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(emptyDirectoryListing.entries.size(), 0);

    // Full-tree round-trip: the in-place COW insert preserves the existing directory
    // while adding the new root file (it no longer drops directories).
    const QString roundTripInsertPath =
        QDir(temp.path()).filePath(QStringLiteral("dir-roundtrip.apfs"));
    const auto roundTripInsert =
        PartitionApfsWriter::commitImageOnlyFileInsert({.source_image_path = directoryCreatePath,
                                                        .written_image_path = roundTripInsertPath,
                                                        .file_name = QStringLiteral("added.txt"),
                                                        .options = generatedOnlyOptions});
    QVERIFY2(roundTripInsert.ok, qPrintable(roundTripInsert.blockers.join(QStringLiteral("; "))));
    const auto roundTripListing = PartitionApfsFileSystemReader::listDirectoryFromImage(
        roundTripInsertPath, QStringLiteral("/"), 20);
    QVERIFY2(roundTripListing.ok, qPrintable(roundTripListing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(roundTripListing.entries.size(), 2);
    bool roundTripSawDir = false;
    bool roundTripSawFile = false;
    for (const auto& entry : roundTripListing.entries) {
        if (entry.name == QStringLiteral("Proof Folder") && entry.directory) {
            roundTripSawDir = true;
        }
        if (entry.name == QStringLiteral("added.txt") && entry.regular_file) {
            roundTripSawFile = true;
        }
    }
    QVERIFY(roundTripSawDir);
    QVERIFY(roundTripSawFile);
    const auto preservedDirListing = PartitionApfsFileSystemReader::listDirectoryFromImage(
        roundTripInsertPath, QStringLiteral("/Proof Folder"), 20);
    QVERIFY2(preservedDirListing.ok,
             qPrintable(preservedDirListing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(preservedDirListing.entries.size(), 0);

    const QByteArray childFileData("APFS child file in generated root directory");
    const QString childFileWritePath =
        QDir(temp.path()).filePath(QStringLiteral("child-file-write.apfs"));
    const auto childFileWrite = PartitionApfsWriter::writeImageOnlyRootDirectoryFile(
        {.source_image_path = directoryCreatePath,
         .written_image_path = childFileWritePath,
         .directory_name = QStringLiteral("Proof Folder"),
         .file_name = QStringLiteral("child.txt"),
         .file_data = childFileData,
         .options = generatedOnlyOptions});
    QVERIFY2(childFileWrite.ok, qPrintable(childFileWrite.blockers.join(QStringLiteral("; "))));
    QCOMPARE(childFileWrite.directory_name, QStringLiteral("Proof Folder"));
    QCOMPARE(childFileWrite.file_name, QStringLiteral("child.txt"));
    QCOMPARE(childFileWrite.file_bytes, static_cast<uint64_t>(childFileData.size()));
    QCOMPARE(childFileWrite.readback_sha256, sha256Hex(childFileData));
    const auto childFileRead = PartitionApfsFileSystemReader::readFileFromImage(
        childFileWritePath,
        QStringLiteral("/Proof Folder/child.txt"),
        static_cast<uint64_t>(childFileData.size()));
    QVERIFY2(childFileRead.ok, qPrintable(childFileRead.blockers.join(QStringLiteral("; "))));
    QCOMPARE(childFileRead.data, childFileData);

    const auto nonEmptyDirectoryDelete = PartitionApfsWriter::deleteImageOnlyRootDirectory(
        {.source_image_path = childFileWritePath,
         .written_image_path =
             QDir(temp.path()).filePath(QStringLiteral("non-empty-directory-delete.apfs")),
         .directory_name = QStringLiteral("Proof Folder"),
         .options = generatedOnlyOptions});
    QVERIFY(!nonEmptyDirectoryDelete.ok);
    QVERIFY(nonEmptyDirectoryDelete.blockers.join(' ').contains(QStringLiteral("empty")));

    const QByteArray childReplacement("replacement APFS child file data");
    const QString childFileReplacePath =
        QDir(temp.path()).filePath(QStringLiteral("child-file-replace.apfs"));
    const auto childFileReplace = PartitionApfsWriter::writeImageOnlyRootDirectoryFile(
        {.source_image_path = childFileWritePath,
         .written_image_path = childFileReplacePath,
         .directory_name = QStringLiteral("Proof Folder"),
         .file_name = QStringLiteral("child.txt"),
         .file_data = childReplacement,
         .options = generatedOnlyOptions});
    QVERIFY2(childFileReplace.ok, qPrintable(childFileReplace.blockers.join(QStringLiteral("; "))));
    QCOMPARE(childFileReplace.plan.operation,
             PartitionApfsWriter::operationName(PartitionApfsWriteOperation::ReplaceFile));
    const auto childReplacementRead = PartitionApfsFileSystemReader::readFileFromImage(
        childFileReplacePath,
        QStringLiteral("/Proof Folder/child.txt"),
        static_cast<uint64_t>(childReplacement.size()));
    QVERIFY2(childReplacementRead.ok,
             qPrintable(childReplacementRead.blockers.join(QStringLiteral("; "))));
    QCOMPARE(childReplacementRead.data, childReplacement);

    const QByteArray childPatchPayload("PATCH");
    QByteArray patchedChildReplacement = childReplacement;
    constexpr uint64_t kChildPatchOffset = 12;
    std::copy(childPatchPayload.cbegin(),
              childPatchPayload.cend(),
              patchedChildReplacement.begin() + static_cast<qsizetype>(kChildPatchOffset));
    const QString childFilePatchPath =
        QDir(temp.path()).filePath(QStringLiteral("child-file-patch.apfs"));
    const auto childFilePatch = PartitionApfsWriter::patchImageOnlyRootDirectoryFile(
        {.source_image_path = childFileReplacePath,
         .written_image_path = childFilePatchPath,
         .directory_name = QStringLiteral("Proof Folder"),
         .file_name = QStringLiteral("child.txt"),
         .patch_offset_bytes = kChildPatchOffset,
         .patch_data = childPatchPayload,
         .options = generatedOnlyOptions});
    QVERIFY2(childFilePatch.ok, qPrintable(childFilePatch.blockers.join(QStringLiteral("; "))));
    QCOMPARE(childFilePatch.directory_name, QStringLiteral("Proof Folder"));
    QCOMPARE(childFilePatch.file_name, QStringLiteral("child.txt"));
    QCOMPARE(childFilePatch.file_bytes, static_cast<uint64_t>(childReplacement.size()));
    QCOMPARE(childFilePatch.patch_offset_bytes, kChildPatchOffset);
    QCOMPARE(childFilePatch.patch_bytes, static_cast<uint64_t>(childPatchPayload.size()));
    QCOMPARE(childFilePatch.patch_sha256, sha256Hex(childPatchPayload));
    QCOMPARE(childFilePatch.readback_sha256, sha256Hex(patchedChildReplacement));
    const auto childPatchRead = PartitionApfsFileSystemReader::readFileFromImage(
        childFilePatchPath,
        QStringLiteral("/Proof Folder/child.txt"),
        static_cast<uint64_t>(patchedChildReplacement.size()));
    QVERIFY2(childPatchRead.ok, qPrintable(childPatchRead.blockers.join(QStringLiteral("; "))));
    QCOMPARE(childPatchRead.data, patchedChildReplacement);

    const auto childRangeBlockedPatch = PartitionApfsWriter::patchImageOnlyRootDirectoryFile(
        {.source_image_path = childFileReplacePath,
         .written_image_path =
             QDir(temp.path()).filePath(QStringLiteral("child-range-blocked-patch.apfs")),
         .directory_name = QStringLiteral("Proof Folder"),
         .file_name = QStringLiteral("child.txt"),
         .patch_offset_bytes = static_cast<uint64_t>(childReplacement.size()),
         .patch_data = QByteArray("too-large"),
         .options = generatedOnlyOptions});
    QVERIFY(!childRangeBlockedPatch.ok);
    QVERIFY(childRangeBlockedPatch.blockers.join(' ').contains(QStringLiteral("inside")));

    const QString childFileDeletePath =
        QDir(temp.path()).filePath(QStringLiteral("child-file-delete.apfs"));
    const auto childFileDelete = PartitionApfsWriter::deleteImageOnlyRootDirectoryFile(
        {.source_image_path = childFilePatchPath,
         .written_image_path = childFileDeletePath,
         .directory_name = QStringLiteral("Proof Folder"),
         .file_name = QStringLiteral("child.txt"),
         .options = generatedOnlyOptions});
    QVERIFY2(childFileDelete.ok, qPrintable(childFileDelete.blockers.join(QStringLiteral("; "))));
    QCOMPARE(childFileDelete.directory_name, QStringLiteral("Proof Folder"));
    QCOMPARE(childFileDelete.file_name, QStringLiteral("child.txt"));
    QCOMPARE(childFileDelete.deleted_file_bytes,
             static_cast<uint64_t>(patchedChildReplacement.size()));
    QCOMPARE(childFileDelete.deleted_file_sha256, sha256Hex(patchedChildReplacement));
    const auto deletedChildRead = PartitionApfsFileSystemReader::readFileFromImage(
        childFileDeletePath, QStringLiteral("/Proof Folder/child.txt"), 1);
    QVERIFY(!deletedChildRead.ok);
    const auto emptyAfterChildDelete = PartitionApfsFileSystemReader::listDirectoryFromImage(
        childFileDeletePath, QStringLiteral("/Proof Folder"), 20);
    QVERIFY2(emptyAfterChildDelete.ok,
             qPrintable(emptyAfterChildDelete.blockers.join(QStringLiteral("; "))));
    QCOMPARE(emptyAfterChildDelete.entries.size(), 0);

    const QString fileAfterDirectoryPath =
        QDir(temp.path()).filePath(QStringLiteral("file-after-directory.apfs"));
    const auto fileAfterDirectory = PartitionApfsWriter::writeImageOnlyRootFile(
        {.source_image_path = childFileDeletePath,
         .written_image_path = fileAfterDirectoryPath,
         .file_name = QStringLiteral("after-dir.txt"),
         .file_data = QByteArray("file after APFS directory proof"),
         .options = generatedOnlyOptions});
    QVERIFY2(fileAfterDirectory.ok,
             qPrintable(fileAfterDirectory.blockers.join(QStringLiteral("; "))));
    const auto fileAfterDirectoryListing = PartitionApfsFileSystemReader::listDirectoryFromImage(
        fileAfterDirectoryPath, QStringLiteral("/"), 20);
    QVERIFY2(fileAfterDirectoryListing.ok,
             qPrintable(fileAfterDirectoryListing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(fileAfterDirectoryListing.entries.size(), 2);
    QVERIFY(std::any_of(fileAfterDirectoryListing.entries.cbegin(),
                        fileAfterDirectoryListing.entries.cend(),
                        [](const PartitionApfsFileEntry& entry) {
                            return entry.name == QStringLiteral("Proof Folder") && entry.directory;
                        }));

    const QString directoryDeletePath =
        QDir(temp.path()).filePath(QStringLiteral("directory-delete.apfs"));
    const auto directoryDelete = PartitionApfsWriter::deleteImageOnlyRootDirectory(
        {.source_image_path = fileAfterDirectoryPath,
         .written_image_path = directoryDeletePath,
         .directory_name = QStringLiteral("Proof Folder"),
         .options = generatedOnlyOptions});
    QVERIFY2(directoryDelete.ok, qPrintable(directoryDelete.blockers.join(QStringLiteral("; "))));
    QCOMPARE(directoryDelete.plan.operation,
             PartitionApfsWriter::operationName(PartitionApfsWriteOperation::DeleteDirectory));
    const auto directoryDeleteListing = PartitionApfsFileSystemReader::listDirectoryFromImage(
        directoryDeletePath, QStringLiteral("/"), 20);
    QVERIFY2(directoryDeleteListing.ok,
             qPrintable(directoryDeleteListing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(directoryDeleteListing.entries.size(), 1);
    QCOMPARE(directoryDeleteListing.entries.first().name, QStringLiteral("after-dir.txt"));
    QVERIFY(directoryDeleteListing.entries.first().regular_file);

    const auto missingDirectoryDelete = PartitionApfsWriter::deleteImageOnlyRootDirectory(
        {.source_image_path = directoryDeletePath,
         .written_image_path =
             QDir(temp.path()).filePath(QStringLiteral("missing-directory-delete.apfs")),
         .directory_name = QStringLiteral("Proof Folder"),
         .options = generatedOnlyOptions});
    QVERIFY(!missingDirectoryDelete.ok);
    QVERIFY(missingDirectoryDelete.blockers.join(' ').contains(QStringLiteral("not found")));

    const QString corruptedImagePath = QDir(temp.path()).filePath(QStringLiteral("corrupted.apfs"));
    QVERIFY(QFile::copy(seededImagePath, corruptedImagePath));
    QFile corruptedImage(corruptedImagePath);
    QVERIFY(corruptedImage.open(QIODevice::ReadWrite));
    constexpr qint64 kGeneratedApfsRootTreeChecksumOffset = 197LL * 4096LL;
    QVERIFY(corruptedImage.seek(kGeneratedApfsRootTreeChecksumOffset));
    char checksumByte = '\0';
    QVERIFY(corruptedImage.getChar(&checksumByte));
    checksumByte = static_cast<char>(checksumByte ^ 0x5A);
    QVERIFY(corruptedImage.seek(kGeneratedApfsRootTreeChecksumOffset));
    QVERIFY(corruptedImage.putChar(checksumByte));
    corruptedImage.close();

    const auto corruptedListing = PartitionApfsFileSystemReader::listDirectoryFromImage(
        corruptedImagePath, QStringLiteral("/"), 20);
    QVERIFY(!corruptedListing.ok);
    QVERIFY(corruptedListing.blockers.join(' ').contains(QStringLiteral("checksum")));

    const QString repairedImagePath = QDir(temp.path()).filePath(QStringLiteral("repaired.apfs"));
    const auto repair = PartitionApfsWriter::repairImageOnlyObjectChecksums(
        {.source_image_path = corruptedImagePath,
         .repaired_image_path = repairedImagePath,
         .options = generatedOnlyOptions});
    QVERIFY2(repair.ok, qPrintable(repair.blockers.join(QStringLiteral("; "))));
    QCOMPARE(repair.repaired_checksum_blocks, 1ULL);
    QVERIFY(repair.scanned_blocks > repair.repaired_checksum_blocks);
    QVERIFY(!repair.repaired_image_sha256.isEmpty());

    const auto repairedListing = PartitionApfsFileSystemReader::listDirectoryFromImage(
        repairedImagePath, QStringLiteral("/"), 20);
    QVERIFY2(repairedListing.ok, qPrintable(repairedListing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(repairedListing.entries.size(), 1);
    const auto repairedRead = PartitionApfsFileSystemReader::readFileFromImage(
        repairedImagePath, QStringLiteral("/proof.txt"), static_cast<uint64_t>(seedData.size()));
    QVERIFY2(repairedRead.ok, qPrintable(repairedRead.blockers.join(QStringLiteral("; "))));
    QCOMPARE(repairedRead.data, seedData);

    const auto cleanRepair = PartitionApfsWriter::repairImageOnlyObjectChecksums(
        {.source_image_path = seededImagePath,
         .repaired_image_path = QDir(temp.path()).filePath(QStringLiteral("clean-repair.apfs")),
         .options = generatedOnlyOptions});
    QVERIFY(!cleanRepair.ok);
    QVERIFY(cleanRepair.blockers.join(' ').contains(QStringLiteral("did not find")));

    const QString malformedGeneratedPath =
        QDir(temp.path()).filePath(QStringLiteral("malformed-generated.apfs"));
    QVERIFY(QFile::copy(imagePath, malformedGeneratedPath));
    QVERIFY(rewriteApfsImageObjectField64(malformedGeneratedPath, 0, kTestApfsObjectOidOffset, 99));
    const auto malformedGeneratedWrite = PartitionApfsWriter::writeImageOnlyRootFile(
        {.source_image_path = malformedGeneratedPath,
         .written_image_path =
             QDir(temp.path()).filePath(QStringLiteral("malformed-generated-write.apfs")),
         .file_name = QStringLiteral("blocked.txt"),
         .file_data = QByteArray("blocked"),
         .options = generatedOnlyOptions});
    QVERIFY(!malformedGeneratedWrite.ok);
    QVERIFY(
        malformedGeneratedWrite.blockers.join(' ').contains(QStringLiteral("generated/minimal")));

    const QString unsupportedApfsPath =
        QDir(temp.path()).filePath(QStringLiteral("unsupported-layout.apfs"));
    QVERIFY(writeBytes(unsupportedApfsPath, apfsRawDetectionFixture()));
    const auto unsupportedRepair = PartitionApfsWriter::repairImageOnlyObjectChecksums(
        {.source_image_path = unsupportedApfsPath,
         .repaired_image_path =
             QDir(temp.path()).filePath(QStringLiteral("unsupported-layout-repair.apfs")),
         .options = options});
    QVERIFY(!unsupportedRepair.ok);
    QVERIFY(unsupportedRepair.blockers.join(' ').contains(QStringLiteral("generated/minimal")));

    const auto badSeed = PartitionApfsWriter::buildImageOnlyFormatImageWithSeedFile(
        {.image_path = QDir(temp.path()).filePath(QStringLiteral("bad-seed.apfs")),
         .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
         .block_size_bytes = 4096,
         .volume_name = QStringLiteral("SAK Seeded"),
         .seed_file_name = QStringLiteral("../proof.txt"),
         .seed_file_data = seedData,
         .options = generatedOnlyOptions});
    QVERIFY(!badSeed.ok);
    QVERIFY(badSeed.blockers.join(' ').contains(QStringLiteral("traversal")));
}

void PartitionManagerCoreTests::apfsWriter_blocksOversizedGeneratedContainers() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    // Multi-chunk containers up to 126 spaceman chunks (a single chunk-info
    // block) are supported and emit an apfsck-clean layout; 256 MiB = 65536
    // blocks = 2 chunks.
    const QString multiChunkPath = QDir(temp.path()).filePath(QStringLiteral("multichunk.apfs"));
    const auto multiChunk = PartitionApfsWriter::buildImageOnlyFormatImage(
        {.image_path = multiChunkPath,
         .target_container_bytes = 256ULL * 1024ULL * 1024ULL,
         .block_size_bytes = 4096,
         .volume_name = QStringLiteral("SAK MultiChunk"),
         .options = options});
    QVERIFY2(multiChunk.ok, qPrintable(multiChunk.blockers.join(QStringLiteral("; "))));
    QVERIFY(QFileInfo::exists(multiChunkPath));

    // Multi-CIB containers (>126 chunks) up to the inline cib-address-array /
    // chunk-0 internal-pool limit are emitted, and the CAB tier (cib_count >
    // cibs_per_cab, 507) is now emitted too: the spaceman publishes a cab-address
    // array pointing at apfs_cib_addr_blocks. An 8 TiB target needs 65536 chunks
    // across 521 cibs and 2 CABs; the writer formats it (sparse metadata near the
    // container start, Apple fsck_apfs-validated separately on the cert VM).
    const auto cabGeometry = PartitionApfsWriter::computeContainerGeometry(
        8ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL / 4096ULL);
    QCOMPARE(cabGeometry.chunk_count, static_cast<uint64_t>(65'536));
    QCOMPARE(cabGeometry.cib_count, static_cast<uint64_t>(521));
    QCOMPARE(cabGeometry.cab_count, static_cast<uint64_t>(2));
    // The engine no longer rejects the CAB tier at the geometry gate. An 8 TiB
    // image-only format gets past that gate to the file-sizing step; whether it then
    // succeeds depends only on the host filesystem accepting an 8 TiB sparse file
    // (the unit host's NTFS scratch refuses it, the VM lab's sparse XFS does not).
    // The full CAB write + Apple fsck_apfs cert therefore runs on the cert VM; the
    // unit-level guarantee is that no "CAB tier" geometry blocker is raised.
    const QString cabImagePath = QDir(temp.path()).filePath(QStringLiteral("cab.apfs"));
    const auto cabFormat = PartitionApfsWriter::buildImageOnlyFormatImage(
        {.image_path = cabImagePath,
         .target_container_bytes = 8ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL,
         .block_size_bytes = 4096,
         .volume_name = QStringLiteral("SAK Cab"),
         .options = options});
    const QString cabBlockers = cabFormat.blockers.join(QStringLiteral("; "));
    QVERIFY2(cabFormat.ok || !cabBlockers.contains(QStringLiteral("CAB")), qPrintable(cabBlockers));

    auto oldFalsePassFixture = apfsRawDetectionFixture();
    constexpr uint64_t kMultiChunkBlocks = 130ULL * 32'768ULL;
    writeLe64(&oldFalsePassFixture, kTestApfsBlockCountOffset, kMultiChunkBlocks);
    writeLe64(&oldFalsePassFixture,
              kTestApfsSpacemanOffset + kTestApfsSpacemanMainDeviceOffset +
                  kTestApfsSpacemanDeviceBlockCountOffset,
              kMultiChunkBlocks);
    writeLe64(&oldFalsePassFixture,
              kTestApfsSpacemanOffset + kTestApfsSpacemanMainDeviceOffset +
                  kTestApfsSpacemanDeviceChunkCountOffset,
              130);
    writeLe32(&oldFalsePassFixture,
              kTestApfsSpacemanOffset + kTestApfsSpacemanMainDeviceOffset +
                  kTestApfsSpacemanDeviceCibCountOffset,
              1);
    const auto detection = PartitionFileSystemDetector::detectBytes(oldFalsePassFixture,
                                                                    oldFalsePassFixture.size());
    QVERIFY(detection.has_value());
    QCOMPARE(detection->file_system, QStringLiteral("APFS"));
    const QString details = detection->details.join(' ');
    QVERIFY(!details.contains(QStringLiteral("APFS space manager block:")));
    QVERIFY(details.contains(QStringLiteral("CIB count does not cover chunks")));
    QCOMPARE(detection->free_bytes, 0ULL);
}

void PartitionManagerCoreTests::apfsWriter_blocksGeneratedLayoutWithSnapshotState() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();

    const QString imagePath = QDir(temp.path()).filePath(QStringLiteral("snap-gate.apfs"));
    const auto build = PartitionApfsWriter::buildImageOnlyFormatImage(
        {.image_path = imagePath,
         .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
         .block_size_bytes = 4096,
         .volume_name = QStringLiteral("SAK Snap Gate"),
         .options = options});
    QVERIFY2(build.ok, qPrintable(build.blockers.join(QStringLiteral("; "))));

    const QByteArray payload = QByteArrayLiteral("snapshot gate baseline payload\n");
    const auto baseline = PartitionApfsWriter::writeImageOnlyRootFile(
        {.source_image_path = imagePath,
         .written_image_path = QDir(temp.path()).filePath(QStringLiteral("snap-baseline.apfs")),
         .file_name = QStringLiteral("baseline.txt"),
         .file_data = payload,
         .options = options});
    QVERIFY2(baseline.ok, qPrintable(baseline.blockers.join(QStringLiteral("; "))));

    constexpr uint32_t kApsbMagic = 0x42'53'50'41;  // 'APSB'
    const QString snapMetaPath = QDir(temp.path()).filePath(QStringLiteral("snap-meta.apfs"));
    QVERIFY(QFile::copy(imagePath, snapMetaPath));
    QVERIFY(tamperApfsBlockField(snapMetaPath, kApsbMagic, 0x98, 7, false));
    const auto snapMetaBlocked = PartitionApfsWriter::writeImageOnlyRootFile(
        {.source_image_path = snapMetaPath,
         .written_image_path = QDir(temp.path()).filePath(QStringLiteral("snap-meta-out.apfs")),
         .file_name = QStringLiteral("blocked.txt"),
         .file_data = payload,
         .options = options});
    QVERIFY(!snapMetaBlocked.ok);
    QVERIFY(snapMetaBlocked.blockers.join(' ').contains(
        QStringLiteral("snapshot-metadata tree OID mismatch")));

    const QString revertPath = QDir(temp.path()).filePath(QStringLiteral("snap-revert.apfs"));
    QVERIFY(QFile::copy(imagePath, revertPath));
    QVERIFY(tamperApfsBlockField(revertPath, kApsbMagic, 0xA0, 12, false));
    const auto revertBlocked = PartitionApfsWriter::writeImageOnlyRootFile(
        {.source_image_path = revertPath,
         .written_image_path = QDir(temp.path()).filePath(QStringLiteral("snap-revert-out.apfs")),
         .file_name = QStringLiteral("blocked.txt"),
         .file_data = payload,
         .options = options});
    QVERIFY(!revertBlocked.ok);
    QVERIFY(revertBlocked.blockers.join(' ').contains(
        QStringLiteral("must not carry revert metadata")));

    const QString omapSnapPath = QDir(temp.path()).filePath(QStringLiteral("snap-omap.apfs"));
    QVERIFY(QFile::copy(imagePath, omapSnapPath));
    QVERIFY(tamperApfsVolumeOmapField(omapSnapPath, 0x24, 1, true));
    const auto omapBlocked = PartitionApfsWriter::writeImageOnlyRootFile(
        {.source_image_path = omapSnapPath,
         .written_image_path = QDir(temp.path()).filePath(QStringLiteral("snap-omap-out.apfs")),
         .file_name = QStringLiteral("blocked.txt"),
         .file_data = payload,
         .options = options});
    QVERIFY(!omapBlocked.ok);
    QVERIFY(
        omapBlocked.blockers.join(' ').contains(QStringLiteral("must not carry snapshot state")));

    const QString omapRevertPath =
        QDir(temp.path()).filePath(QStringLiteral("snap-omap-revert.apfs"));
    QVERIFY(QFile::copy(imagePath, omapRevertPath));
    QVERIFY(tamperApfsVolumeOmapField(omapRevertPath, 0x48, 5, false));
    const auto omapRevertBlocked = PartitionApfsWriter::writeImageOnlyRootFile(
        {.source_image_path = omapRevertPath,
         .written_image_path =
             QDir(temp.path()).filePath(QStringLiteral("snap-omap-revert-out.apfs")),
         .file_name = QStringLiteral("blocked.txt"),
         .file_data = payload,
         .options = options});
    QVERIFY(!omapRevertBlocked.ok);
    QVERIFY(omapRevertBlocked.blockers.join(' ').contains(
        QStringLiteral("must not carry pending revert state")));

    const QString numSnapshotsPath = QDir(temp.path()).filePath(QStringLiteral("snap-count.apfs"));
    QVERIFY(QFile::copy(imagePath, numSnapshotsPath));
    QVERIFY(tamperApfsBlockField(numSnapshotsPath, kApsbMagic, 0xD8, 3, false));
    const auto numSnapshotsBlocked = PartitionApfsWriter::writeImageOnlyRootFile(
        {.source_image_path = numSnapshotsPath,
         .written_image_path = QDir(temp.path()).filePath(QStringLiteral("snap-count-out.apfs")),
         .file_name = QStringLiteral("blocked.txt"),
         .file_data = payload,
         .options = options});
    QVERIFY(!numSnapshotsBlocked.ok);
    QVERIFY(numSnapshotsBlocked.blockers.join(' ').contains(
        QStringLiteral("must not contain snapshots")));

    const QString encryptedPath = QDir(temp.path()).filePath(QStringLiteral("snap-encrypted.apfs"));
    QVERIFY(QFile::copy(imagePath, encryptedPath));
    QVERIFY(tamperApfsBlockField(encryptedPath, kApsbMagic, 0x108, 0x04, false));
    const auto encryptedBlocked = PartitionApfsWriter::writeImageOnlyRootFile(
        {.source_image_path = encryptedPath,
         .written_image_path =
             QDir(temp.path()).filePath(QStringLiteral("snap-encrypted-out.apfs")),
         .file_name = QStringLiteral("blocked.txt"),
         .file_data = payload,
         .options = options});
    QVERIFY(!encryptedBlocked.ok);
    QVERIFY(encryptedBlocked.blockers.join(' ').contains(
        QStringLiteral("encrypted or protected volume state is blocked")));
}

void PartitionManagerCoreTests::apfsWriter_preflightFailsClosedUntilCertified() {
    const auto fixture = apfsRawDetectionFixture();
    const auto detection = PartitionFileSystemDetector::detectBytes(fixture, fixture.size());
    QVERIFY(detection.has_value());
    QCOMPARE(detection->file_system, QStringLiteral("APFS"));

    verifyApfsWriterBlockedByDefault(*detection);
    PartitionApfsWriteOptions certifiedImageOnly = certifiedApfsImageOnlyOptions();
    const auto allowed = PartitionApfsWriter::preflightExistingContainer(
        *detection, PartitionApfsWriteOperation::CreateFile, certifiedImageOnly);
    QVERIFY2(allowed.allowed, qPrintable(allowed.blockers.join(QStringLiteral("; "))));

    const auto plan =
        PartitionApfsWriter::planImageOnlyMutation(*detection,
                                                   PartitionApfsWriteOperation::CreateFile,
                                                   certifiedImageOnly,
                                                   QStringLiteral("Users/Test/new-file.txt"));
    verifyApfsImageOnlyPlanShape(plan);
    verifyApfsImageOnlyExecutionEvidenceGate(plan);
    verifyApfsWholeContainerPlanShapes(*detection, certifiedImageOnly);
    verifyApfsImageOnlyFormatBuild(certifiedImageOnly);

    const auto traversalPlan =
        PartitionApfsWriter::planImageOnlyMutation(*detection,
                                                   PartitionApfsWriteOperation::CreateFile,
                                                   certifiedImageOnly,
                                                   QStringLiteral("../escape.txt"));
    QVERIFY(!traversalPlan.buildable);
    QVERIFY(traversalPlan.preflight.blockers.join(' ').contains(QStringLiteral("traversal")));

    certifiedImageOnly.image_only = false;
    const auto rawMediaBlocked = PartitionApfsWriter::preflightExistingContainer(
        *detection, PartitionApfsWriteOperation::CreateFile, certifiedImageOnly);
    QVERIFY(!rawMediaBlocked.allowed);
    QVERIFY(rawMediaBlocked.blockers.join(' ').contains(QStringLiteral("Raw APFS media writes")));
}

void PartitionManagerCoreTests::apfsWriter_inPlaceCheckpointCommitAdvancesTransaction() {
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir dir(temp.path());
    const QString base = dir.filePath(QStringLiteral("a2-base.apfs"));
    const auto build = PartitionApfsWriter::buildImageOnlyFormatImage(
        {.image_path = base,
         .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
         .block_size_bytes = 4096,
         .volume_name = QStringLiteral("A2 Commit"),
         .options = options});
    QVERIFY2(build.ok, qPrintable(build.blockers.join(QStringLiteral("; "))));

    const auto readBlock = [](const QString& path, quint64 block) {
        QFile file(path);
        file.open(QIODevice::ReadOnly);
        file.seek(static_cast<qint64>(block * 4096));
        return file.read(4096);
    };
    const auto le32 = [](const QByteArray& bytes, int offset) {
        return qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(bytes.constData() + offset));
    };
    const auto le64 = [](const QByteArray& bytes, int offset) {
        return qFromLittleEndian<quint64>(
            reinterpret_cast<const uchar*>(bytes.constData() + offset));
    };

    // First in-place commit: xid 2 -> 3, checkpoint into descriptor-ring slot 5/6.
    const QString x3 = dir.filePath(QStringLiteral("a2-x3.apfs"));
    const auto commit = PartitionApfsWriter::commitImageOnlyCheckpoint(
        {.source_image_path = base, .written_image_path = x3, .options = options});
    QVERIFY2(commit.ok, qPrintable(commit.blockers.join(QStringLiteral("; "))));
    QCOMPARE(commit.previous_xid, 2ULL);
    QCOMPARE(commit.new_xid, 3ULL);
    QCOMPARE(commit.checkpoint_map_block, 5ULL);
    QCOMPARE(commit.superblock_block, 6ULL);

    const QByteArray nxsb = readBlock(x3, 0);
    QVERIFY(PartitionApfsWriter::verifyObjectChecksum(nxsb));
    QCOMPARE(le64(nxsb, 0x10), 3ULL);    // o_xid advanced
    QCOMPARE(le64(nxsb, 0x60), 4ULL);    // nx_next_xid
    QCOMPARE(le32(nxsb, 0x88), 4U);      // nx_xp_desc_index
    QCOMPARE(le32(nxsb, 0x90), 6U);      // nx_xp_data_index
    QCOMPARE(le64(nxsb, 0xA0), 199ULL);  // nx_omap_oid carried forward (no fs COW)
    QCOMPARE(readBlock(x3, 6), nxsb);    // descriptor-ring nx_superblock mirrors block 0
    for (quint64 block : {0ULL, 5ULL, 6ULL, 15ULL, 16ULL, 17ULL, 18ULL}) {
        QVERIFY2(PartitionApfsWriter::verifyObjectChecksum(readBlock(x3, block)),
                 qPrintable(QStringLiteral("invalid object checksum at block %1").arg(block)));
    }

    // The volume content carries forward unchanged: the reader still walks it.
    const auto listing =
        PartitionApfsFileSystemReader::listDirectoryFromImage(x3, QStringLiteral("/"), 20);
    QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(listing.volume_name, QStringLiteral("A2 Commit"));

    // A second commit advances again (xid 3 -> 4) into the next ring slots.
    const QString x4 = dir.filePath(QStringLiteral("a2-x4.apfs"));
    const auto commit2 = PartitionApfsWriter::commitImageOnlyCheckpoint(
        {.source_image_path = x3, .written_image_path = x4, .options = options});
    QVERIFY2(commit2.ok, qPrintable(commit2.blockers.join(QStringLiteral("; "))));
    QCOMPARE(commit2.new_xid, 4ULL);
    QCOMPARE(commit2.checkpoint_map_block, 7ULL);
    const QByteArray nxsb4 = readBlock(x4, 0);
    QVERIFY(PartitionApfsWriter::verifyObjectChecksum(nxsb4));
    QCOMPARE(le64(nxsb4, 0x10), 4ULL);
    QCOMPARE(le32(nxsb4, 0x88), 6U);   // descriptor index advanced one pair
    QCOMPARE(le32(nxsb4, 0x90), 10U);  // data index advanced one quad
}

void PartitionManagerCoreTests::apfsWriter_inPlaceFileInsertCommitAddsReadableFile() {
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir dir(temp.path());
    const QString base = dir.filePath(QStringLiteral("a2fi-base.apfs"));
    const auto build = PartitionApfsWriter::buildImageOnlyFormatImage(
        {.image_path = base,
         .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
         .block_size_bytes = 4096,
         .volume_name = QStringLiteral("A2FI"),
         .options = options});
    QVERIFY2(build.ok, qPrintable(build.blockers.join(QStringLiteral("; "))));

    const QString out = dir.filePath(QStringLiteral("a2fi-out.apfs"));
    const auto commit =
        PartitionApfsWriter::commitImageOnlyFileInsert({.source_image_path = base,
                                                        .written_image_path = out,
                                                        .file_name = QStringLiteral("proof.txt"),
                                                        .options = options});
    QVERIFY2(commit.ok, qPrintable(commit.blockers.join(QStringLiteral("; "))));
    QCOMPARE(commit.previous_xid, 2ULL);
    QCOMPARE(commit.new_xid, 3ULL);

    // The S.A.K. reader walks the copy-on-written object-map chain
    // (nx_omap_oid -> container omap -> volume superblock -> volume omap ->
    // root tree) and finds the inserted file.
    const auto listing =
        PartitionApfsFileSystemReader::listDirectoryFromImage(out, QStringLiteral("/"), 20);
    QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(listing.volume_name, QStringLiteral("A2FI"));
    QCOMPARE(listing.entries.size(), 1);
    QCOMPARE(listing.entries.first().name, QStringLiteral("proof.txt"));
    QCOMPARE(listing.entries.first().size_bytes, 0ULL);

    const auto readBlock = [](const QString& path, quint64 block) {
        QFile file(path);
        file.open(QIODevice::ReadOnly);
        file.seek(static_cast<qint64>(block * 4096));
        return file.read(4096);
    };
    const auto le64 = [](const QByteArray& bytes, int offset) {
        return qFromLittleEndian<quint64>(
            reinterpret_cast<const uchar*>(bytes.constData() + offset));
    };
    const QByteArray nxsb = readBlock(out, 0);
    QVERIFY(PartitionApfsWriter::verifyObjectChecksum(nxsb));
    QCOMPARE(le64(nxsb, 0x10), 3ULL);  // o_xid advanced
    QCOMPARE(le64(nxsb, 0xA0),
             206ULL);  // nx_omap_oid -> the new container omap header (6th free block)

    // Every copy-on-written object block carries a valid object checksum.
    for (quint64 block : {201ULL, 202ULL, 203ULL, 204ULL, 205ULL, 206ULL}) {
        QVERIFY2(PartitionApfsWriter::verifyObjectChecksum(readBlock(out, block)),
                 qPrintable(QStringLiteral("invalid object checksum at block %1").arg(block)));
    }

    // Crash-safe IP rotation: the commit wrote the new chunk-info + bitmap into
    // the spare internal-pool slot (187/188 -> 189/190), leaving the previous
    // checkpoint's cib/bitmap intact, and moved the spaceman cib_addr to 189. The
    // live allocation bitmap is therefore block 190.
    QCOMPARE(PartitionApfsWriter::readGeneratedLiveCibAddr(out), 189ULL);
    // Deferred reclamation: the old chain blocks are NOT freed immediately - they
    // are queued on the main free-queue (kept allocated in the rotated chunk bitmap
    // until they age past the rollback window), so they still read as used; the six
    // new blocks are allocated.
    const QByteArray bitmap = readBlock(out, 190);
    const auto used = [&bitmap](quint64 block) {
        return (static_cast<quint8>(bitmap.at(static_cast<qsizetype>(block / 8))) >> (block % 8)) &
               1;
    };
    for (quint64 block : {193ULL, 194ULL, 197ULL, 198ULL, 199ULL, 200ULL}) {
        QVERIFY2(used(block) == 1,
                 qPrintable(QStringLiteral("old block %1 should stay queued (used)").arg(block)));
    }
    for (quint64 block : {201ULL, 202ULL, 203ULL, 204ULL, 205ULL, 206ULL}) {
        QVERIFY2(used(block) == 1,
                 qPrintable(QStringLiteral("new block %1 not allocated").arg(block)));
    }

    // A non-empty file inserts a data extent: the payload is written to a data
    // block, the reader reads back the exact size, and the spaceman + CIB free
    // counts drop by the one allocated data block.
    const QString base2 = dir.filePath(QStringLiteral("a2fi-base2.apfs"));
    const auto build2 = PartitionApfsWriter::buildImageOnlyFormatImage(
        {.image_path = base2,
         .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
         .block_size_bytes = 4096,
         .volume_name = QStringLiteral("A2NZ"),
         .options = options});
    QVERIFY2(build2.ok, qPrintable(build2.blockers.join(QStringLiteral("; "))));
    const QByteArray payload = QByteArrayLiteral("Hello from the in-place COW file insert.");
    const QString out2 = dir.filePath(QStringLiteral("a2nz-out.apfs"));
    const auto commit2 =
        PartitionApfsWriter::commitImageOnlyFileInsert({.source_image_path = base2,
                                                        .written_image_path = out2,
                                                        .file_name = QStringLiteral("hello.txt"),
                                                        .file_data = payload,
                                                        .options = options});
    QVERIFY2(commit2.ok, qPrintable(commit2.blockers.join(QStringLiteral("; "))));
    QCOMPARE(commit2.new_xid, 3ULL);

    const auto listing2 =
        PartitionApfsFileSystemReader::listDirectoryFromImage(out2, QStringLiteral("/"), 20);
    QVERIFY2(listing2.ok, qPrintable(listing2.blockers.join(QStringLiteral("; "))));
    QCOMPARE(listing2.entries.size(), 1);
    QCOMPARE(listing2.entries.first().name, QStringLiteral("hello.txt"));
    QCOMPARE(listing2.entries.first().size_bytes, static_cast<uint64_t>(payload.size()));
    // The payload landed in the data block past the seven COW chain blocks (six
    // metadata + the copy-on-written extent-ref tree), i.e. block 208.
    QCOMPARE(readBlock(out2, 208).left(payload.size()), payload);
    // The copy-on-written extent-ref tree (block 207) carries the data extent.
    QVERIFY(PartitionApfsWriter::verifyObjectChecksum(readBlock(out2, 207)));
    // Deferred reclamation: nothing ages out at this first commit, so the spaceman
    // free count (block 15) drops by every newly allocated block - the seven-block
    // COW chain (one fs node + five object-map blocks + the copy-on-written extent-
    // ref tree) plus the one data block - while the seven old chain blocks are
    // queued on the main free-queue rather than released. apfsck confirms the
    // resulting space accounting is exact.
    const auto deviceFree = [&le64](const QByteArray& spaceman) {
        return le64(spaceman, 0x30 + 0x18);  // sm_dev[MAIN].sm_free_count
    };
    QCOMPARE(deviceFree(readBlock(out2, 15)) + 8ULL, deviceFree(readBlock(base2, 11)));
}

void PartitionManagerCoreTests::apfsWriter_inPlaceFileWriteCreatesThenReplaces() {
    // The production file-write primitive (commitImageOnly/RawFileWrite) is
    // create-or-replace: a new name is a single insert commit; an existing name is a
    // delete-then-insert replace. Both reuse the Apple-certified COW commit engine.
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir dir(temp.path());
    const QString base = dir.filePath(QStringLiteral("a2fw-base.apfs"));
    QVERIFY(PartitionApfsWriter::buildImageOnlyFormatImage(
                {.image_path = base,
                 .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
                 .block_size_bytes = 4096,
                 .volume_name = QStringLiteral("A2FW"),
                 .options = options})
                .ok);

    // Create: a new name routes through a single insert commit (xid 2 -> 3).
    const QByteArray v1 = QByteArrayLiteral("first version of the file");
    const QString created = dir.filePath(QStringLiteral("a2fw-created.apfs"));
    const auto create =
        PartitionApfsWriter::commitImageOnlyFileWrite({.source_image_path = base,
                                                       .written_image_path = created,
                                                       .file_name = QStringLiteral("doc.txt"),
                                                       .file_data = v1,
                                                       .options = options});
    QVERIFY2(create.ok, qPrintable(create.blockers.join(QStringLiteral("; "))));
    QCOMPARE(create.new_xid, 3ULL);
    {
        const auto listing =
            PartitionApfsFileSystemReader::listDirectoryFromImage(created, QStringLiteral("/"), 20);
        QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));
        QCOMPARE(listing.entries.size(), 1);
        QCOMPARE(listing.entries.first().name, QStringLiteral("doc.txt"));
        QCOMPARE(listing.entries.first().size_bytes, static_cast<uint64_t>(v1.size()));
        const auto read = PartitionApfsFileSystemReader::readFileFromImage(
            created, QStringLiteral("/doc.txt"), 4096);
        QVERIFY2(read.ok, qPrintable(read.blockers.join(QStringLiteral("; "))));
        QCOMPARE(read.data, v1);
    }

    // Replace: writing the same name overwrites it (delete xid 3 -> 4, then insert
    // xid 4 -> 5). Still one entry, the new (longer) content, the new size.
    const QByteArray v2 = QByteArrayLiteral("second, longer version of the same file's content");
    const QString replaced = dir.filePath(QStringLiteral("a2fw-replaced.apfs"));
    const auto replace =
        PartitionApfsWriter::commitImageOnlyFileWrite({.source_image_path = created,
                                                       .written_image_path = replaced,
                                                       .file_name = QStringLiteral("doc.txt"),
                                                       .file_data = v2,
                                                       .options = options});
    QVERIFY2(replace.ok, qPrintable(replace.blockers.join(QStringLiteral("; "))));
    QCOMPARE(replace.new_xid, 5ULL);
    const auto listing =
        PartitionApfsFileSystemReader::listDirectoryFromImage(replaced, QStringLiteral("/"), 20);
    QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(listing.entries.size(), 1);
    QCOMPARE(listing.entries.first().name, QStringLiteral("doc.txt"));
    QCOMPARE(listing.entries.first().size_bytes, static_cast<uint64_t>(v2.size()));
    const auto read = PartitionApfsFileSystemReader::readFileFromImage(replaced,
                                                                       QStringLiteral("/doc.txt"),
                                                                       4096);
    QVERIFY2(read.ok, qPrintable(read.blockers.join(QStringLiteral("; "))));
    QCOMPARE(read.data, v2);
}

void PartitionManagerCoreTests::apfsWriter_inPlaceDirectoryCreatePreservesTree() {
    // The in-place COW directory-create commit adds an empty root directory while
    // preserving the existing tree (Apple-certified: kernel reads the directory,
    // fsck_apfs clean). A duplicate name fails closed.
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir dir(temp.path());
    const QString base = dir.filePath(QStringLiteral("a2dc-base.apfs"));
    QVERIFY(PartitionApfsWriter::buildImageOnlyFormatImage(
                {.image_path = base,
                 .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
                 .block_size_bytes = 4096,
                 .volume_name = QStringLiteral("A2DC"),
                 .options = options})
                .ok);
    const QString withFile = dir.filePath(QStringLiteral("a2dc-file.apfs"));
    QVERIFY(
        PartitionApfsWriter::commitImageOnlyFileWrite({.source_image_path = base,
                                                       .written_image_path = withFile,
                                                       .file_name = QStringLiteral("keep.txt"),
                                                       .file_data = QByteArrayLiteral("keep me"),
                                                       .options = options})
            .ok);

    const QString withDir = dir.filePath(QStringLiteral("a2dc-dir.apfs"));
    const auto create = PartitionApfsWriter::commitImageOnlyDirectoryCreate(
        {.source_image_path = withFile,
         .written_image_path = withDir,
         .directory_name = QStringLiteral("folder"),
         .options = options});
    QVERIFY2(create.ok, qPrintable(create.blockers.join(QStringLiteral("; "))));
    const auto listing =
        PartitionApfsFileSystemReader::listDirectoryFromImage(withDir, QStringLiteral("/"), 20);
    QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(listing.entries.size(), 2);
    bool sawFile = false;
    bool sawDir = false;
    for (const auto& entry : listing.entries) {
        if (entry.name == QStringLiteral("keep.txt") && entry.regular_file) {
            sawFile = true;
        }
        if (entry.name == QStringLiteral("folder") && entry.directory) {
            sawDir = true;
        }
    }
    QVERIFY(sawFile);
    QVERIFY(sawDir);
    const auto emptyDir = PartitionApfsFileSystemReader::listDirectoryFromImage(
        withDir, QStringLiteral("/folder"), 20);
    QVERIFY2(emptyDir.ok, qPrintable(emptyDir.blockers.join(QStringLiteral("; "))));
    QCOMPARE(emptyDir.entries.size(), 0);

    // A duplicate directory name fails closed.
    const auto dup = PartitionApfsWriter::commitImageOnlyDirectoryCreate(
        {.source_image_path = withDir,
         .written_image_path = dir.filePath(QStringLiteral("a2dc-dup.apfs")),
         .directory_name = QStringLiteral("folder"),
         .options = options});
    QVERIFY(!dup.ok);
}

void PartitionManagerCoreTests::apfsWriter_inPlaceDirectoryMutationsRoundTrip() {
    // The in-place COW directory-child write/delete and empty-directory delete commits
    // preserve the rest of the tree (Apple-certified: kernel reads docs/b.txt, tmp and
    // docs/a.txt removed, fsck_apfs clean). Non-empty / missing targets fail closed.
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir dir(temp.path());
    const QString base = dir.filePath(QStringLiteral("a2dm-base.apfs"));
    QVERIFY(PartitionApfsWriter::buildImageOnlyFormatImage(
                {.image_path = base,
                 .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
                 .block_size_bytes = 4096,
                 .volume_name = QStringLiteral("A2DM"),
                 .options = options})
                .ok);
    const QString withFile = dir.filePath(QStringLiteral("a2dm-file.apfs"));
    QVERIFY(
        PartitionApfsWriter::commitImageOnlyFileWrite({.source_image_path = base,
                                                       .written_image_path = withFile,
                                                       .file_name = QStringLiteral("root.txt"),
                                                       .file_data = QByteArrayLiteral("root me"),
                                                       .options = options})
            .ok);
    const QString withDir = dir.filePath(QStringLiteral("a2dm-dir.apfs"));
    QVERIFY(PartitionApfsWriter::commitImageOnlyDirectoryCreate(
                {.source_image_path = withFile,
                 .written_image_path = withDir,
                 .directory_name = QStringLiteral("docs"),
                 .options = options})
                .ok);

    // Two children -> docs valence 2.
    const QString withA = dir.filePath(QStringLiteral("a2dm-a.apfs"));
    QVERIFY2(PartitionApfsWriter::commitImageOnlyDirectoryChildWrite(
                 {.source_image_path = withDir,
                  .written_image_path = withA,
                  .directory_name = QStringLiteral("docs"),
                  .file_name = QStringLiteral("a.txt"),
                  .file_data = QByteArrayLiteral("child-a"),
                  .options = options})
                 .ok,
             "child write a.txt");
    const QString withB = dir.filePath(QStringLiteral("a2dm-b.apfs"));
    QVERIFY(PartitionApfsWriter::commitImageOnlyDirectoryChildWrite(
                {.source_image_path = withA,
                 .written_image_path = withB,
                 .directory_name = QStringLiteral("docs"),
                 .file_name = QStringLiteral("b.txt"),
                 .file_data = QByteArrayLiteral("child-b"),
                 .options = options})
                .ok);
    const auto docsTwo =
        PartitionApfsFileSystemReader::listDirectoryFromImage(withB, QStringLiteral("/docs"), 20);
    QVERIFY2(docsTwo.ok, qPrintable(docsTwo.blockers.join(QStringLiteral("; "))));
    QCOMPARE(docsTwo.entries.size(), 2);

    // Delete a.txt -> docs holds only b.txt.
    const QString withDelA = dir.filePath(QStringLiteral("a2dm-dela.apfs"));
    QVERIFY(PartitionApfsWriter::commitImageOnlyDirectoryChildDelete(
                {.source_image_path = withB,
                 .written_image_path = withDelA,
                 .directory_name = QStringLiteral("docs"),
                 .file_name = QStringLiteral("a.txt"),
                 .options = options})
                .ok);
    const auto docsOne = PartitionApfsFileSystemReader::listDirectoryFromImage(
        withDelA, QStringLiteral("/docs"), 20);
    QVERIFY2(docsOne.ok, qPrintable(docsOne.blockers.join(QStringLiteral("; "))));
    QCOMPARE(docsOne.entries.size(), 1);
    QCOMPARE(docsOne.entries.first().name, QStringLiteral("b.txt"));

    // Create then delete an empty directory.
    const QString withTmp = dir.filePath(QStringLiteral("a2dm-tmp.apfs"));
    QVERIFY(PartitionApfsWriter::commitImageOnlyDirectoryCreate(
                {.source_image_path = withDelA,
                 .written_image_path = withTmp,
                 .directory_name = QStringLiteral("tmp"),
                 .options = options})
                .ok);
    const QString withDelTmp = dir.filePath(QStringLiteral("a2dm-deltmp.apfs"));
    QVERIFY2(PartitionApfsWriter::commitImageOnlyDirectoryDelete(
                 {.source_image_path = withTmp,
                  .written_image_path = withDelTmp,
                  .directory_name = QStringLiteral("tmp"),
                  .options = options})
                 .ok,
             "delete empty tmp");
    const auto rootListing =
        PartitionApfsFileSystemReader::listDirectoryFromImage(withDelTmp, QStringLiteral("/"), 20);
    QVERIFY2(rootListing.ok, qPrintable(rootListing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(rootListing.entries.size(), 2);
    bool sawDocs = false;
    bool sawRoot = false;
    bool sawTmp = false;
    for (const auto& entry : rootListing.entries) {
        sawDocs = sawDocs || (entry.name == QStringLiteral("docs") && entry.directory);
        sawRoot = sawRoot || (entry.name == QStringLiteral("root.txt"));
        sawTmp = sawTmp || (entry.name == QStringLiteral("tmp"));
    }
    QVERIFY(sawDocs);
    QVERIFY(sawRoot);
    QVERIFY(!sawTmp);

    // Fail-closed: deleting a non-empty directory, a missing directory, and writing a
    // child into a missing directory are all rejected.
    QVERIFY(!PartitionApfsWriter::commitImageOnlyDirectoryDelete(
                 {.source_image_path = withDelTmp,
                  .written_image_path = dir.filePath(QStringLiteral("a2dm-x1.apfs")),
                  .directory_name = QStringLiteral("docs"),
                  .options = options})
                 .ok);
    QVERIFY(!PartitionApfsWriter::commitImageOnlyDirectoryDelete(
                 {.source_image_path = withDelTmp,
                  .written_image_path = dir.filePath(QStringLiteral("a2dm-x2.apfs")),
                  .directory_name = QStringLiteral("ghost"),
                  .options = options})
                 .ok);
    QVERIFY(!PartitionApfsWriter::commitImageOnlyDirectoryChildWrite(
                 {.source_image_path = withDelTmp,
                  .written_image_path = dir.filePath(QStringLiteral("a2dm-x3.apfs")),
                  .directory_name = QStringLiteral("ghost"),
                  .file_name = QStringLiteral("c.txt"),
                  .file_data = QByteArrayLiteral("nope"),
                  .options = options})
                 .ok);
}

void PartitionManagerCoreTests::apfsWriter_inPlaceDirectoryChildRename() {
    // Same-directory child rename routes onto the certified COW engine (the production
    // parity follow-on to the directory write/delete generalization): the renamed child
    // keeps its data, the rest of the tree is preserved, and a missing child fails closed.
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir dir(temp.path());
    const QString base = dir.filePath(QStringLiteral("a2dr-base.apfs"));
    QVERIFY(PartitionApfsWriter::buildImageOnlyFormatImage(
                {.image_path = base,
                 .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
                 .block_size_bytes = 4096,
                 .volume_name = QStringLiteral("A2DR"),
                 .options = options})
                .ok);
    const QString withFile = dir.filePath(QStringLiteral("a2dr-file.apfs"));
    QVERIFY(
        PartitionApfsWriter::commitImageOnlyFileWrite({.source_image_path = base,
                                                       .written_image_path = withFile,
                                                       .file_name = QStringLiteral("root.txt"),
                                                       .file_data = QByteArrayLiteral("root me"),
                                                       .options = options})
            .ok);
    const QString withDir = dir.filePath(QStringLiteral("a2dr-dir.apfs"));
    QVERIFY(PartitionApfsWriter::commitImageOnlyDirectoryCreate(
                {.source_image_path = withFile,
                 .written_image_path = withDir,
                 .directory_name = QStringLiteral("docs"),
                 .options = options})
                .ok);
    const QString withChild = dir.filePath(QStringLiteral("a2dr-child.apfs"));
    QVERIFY(PartitionApfsWriter::commitImageOnlyDirectoryChildWrite(
                {.source_image_path = withDir,
                 .written_image_path = withChild,
                 .directory_name = QStringLiteral("docs"),
                 .file_name = QStringLiteral("a.txt"),
                 .file_data = QByteArrayLiteral("child-data"),
                 .options = options})
                .ok);

    const QString renamed = dir.filePath(QStringLiteral("a2dr-renamed.apfs"));
    const auto rename = PartitionApfsWriter::commitImageOnlyDirectoryChildRename(
        {.source_image_path = withChild,
         .written_image_path = renamed,
         .directory_name = QStringLiteral("docs"),
         .file_name = QStringLiteral("a.txt"),
         .new_file_name = QStringLiteral("b.txt"),
         .options = options});
    QVERIFY2(rename.ok, qPrintable(rename.blockers.join(QStringLiteral("; "))));
    const auto docs =
        PartitionApfsFileSystemReader::listDirectoryFromImage(renamed, QStringLiteral("/docs"), 20);
    QVERIFY2(docs.ok, qPrintable(docs.blockers.join(QStringLiteral("; "))));
    QCOMPARE(docs.entries.size(), 1);
    QCOMPARE(docs.entries.first().name, QStringLiteral("b.txt"));
    const auto root =
        PartitionApfsFileSystemReader::listDirectoryFromImage(renamed, QStringLiteral("/"), 20);
    QVERIFY(root.ok);
    QCOMPARE(root.entries.size(), 2);  // docs + root.txt

    // Renaming a missing child fails closed.
    QVERIFY(!PartitionApfsWriter::commitImageOnlyDirectoryChildRename(
                 {.source_image_path = renamed,
                  .written_image_path = dir.filePath(QStringLiteral("a2dr-x.apfs")),
                  .directory_name = QStringLiteral("docs"),
                  .file_name = QStringLiteral("ghost.txt"),
                  .new_file_name = QStringLiteral("z.txt"),
                  .options = options})
                 .ok);
}

void PartitionManagerCoreTests::apfsWriter_inPlaceFilePatchPreservesObjectId() {
    // The true in-place COW patch replaces a byte range while preserving the file's object
    // id (it re-COWs the data extents, not the inode): the object id is unchanged, the
    // content is patched, and no new file/object id is consumed.
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir dir(temp.path());
    const QString base = dir.filePath(QStringLiteral("a2pt-base.apfs"));
    QVERIFY(PartitionApfsWriter::buildImageOnlyFormatImage(
                {.image_path = base,
                 .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
                 .block_size_bytes = 4096,
                 .volume_name = QStringLiteral("A2PT"),
                 .options = options})
                .ok);
    const QString withFile = dir.filePath(QStringLiteral("a2pt-file.apfs"));
    QVERIFY(
        PartitionApfsWriter::commitImageOnlyFileWrite({.source_image_path = base,
                                                       .written_image_path = withFile,
                                                       .file_name = QStringLiteral("doc.txt"),
                                                       .file_data = QByteArrayLiteral("AAAAAAAAAA"),
                                                       .options = options})
            .ok);
    const auto before =
        PartitionApfsFileSystemReader::listDirectoryFromImage(withFile, QStringLiteral("/"), 20);
    QVERIFY(before.ok);
    QCOMPARE(before.entries.size(), 1);
    const uint64_t idBefore = before.entries.first().object_id;

    // Patch bytes [3,5) -> "ZZ".
    const QString patched = dir.filePath(QStringLiteral("a2pt-patched.apfs"));
    const auto patch =
        PartitionApfsWriter::commitImageOnlyFilePatch({.source_image_path = withFile,
                                                       .written_image_path = patched,
                                                       .directory_name = QString(),
                                                       .file_name = QStringLiteral("doc.txt"),
                                                       .patch_offset_bytes = 3,
                                                       .patch_data = QByteArrayLiteral("ZZ"),
                                                       .options = options});
    QVERIFY2(patch.ok, qPrintable(patch.blockers.join(QStringLiteral("; "))));

    const auto read = PartitionApfsFileSystemReader::readFileFromImage(patched,
                                                                       QStringLiteral("/doc.txt"),
                                                                       64ULL * 1024ULL);
    QVERIFY2(read.ok, qPrintable(read.blockers.join(QStringLiteral("; "))));
    QCOMPARE(read.data, QByteArrayLiteral("AAAZZAAAAA"));

    const auto after =
        PartitionApfsFileSystemReader::listDirectoryFromImage(patched, QStringLiteral("/"), 20);
    QVERIFY(after.ok);
    QCOMPARE(after.entries.size(), 1);                    // no new file
    QCOMPARE(after.entries.first().object_id, idBefore);  // identity preserved

    // A missing file fails closed.
    QVERIFY(!PartitionApfsWriter::commitImageOnlyFilePatch(
                 {.source_image_path = patched,
                  .written_image_path = dir.filePath(QStringLiteral("a2pt-x.apfs")),
                  .directory_name = QString(),
                  .file_name = QStringLiteral("ghost.txt"),
                  .patch_offset_bytes = 0,
                  .patch_data = QByteArrayLiteral("x"),
                  .options = options})
                 .ok);
}

void PartitionManagerCoreTests::apfsWriter_inPlaceSnapshotCreateAddsSnapshot() {
    // A3: creating a snapshot freezes the volume's tree pointers into a snapshot the
    // kernel + fsck_apfs enumerate. Verifies the on-disk structures: the volume gains
    // num_snapshots=1, an omap snapshot tree (subtype 0x13, one record), a snap-meta
    // tree (two records: j_snap_metadata + j_snap_name), a physical frozen superblock
    // copy that keeps the root tree, and the logical alloc_count grows to 2*before - 3.
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir dir(temp.path());
    const QString base = dir.filePath(QStringLiteral("a3-base.apfs"));
    QVERIFY(PartitionApfsWriter::buildImageOnlyFormatImage(
                {.image_path = base,
                 .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
                 .block_size_bytes = 4096,
                 .volume_name = QStringLiteral("A3SNAP"),
                 .options = options})
                .ok);

    const auto readBlock = [](const QString& path, quint64 block) {
        QFile file(path);
        file.open(QIODevice::ReadOnly);
        file.seek(static_cast<qint64>(block * 4096));
        return file.read(4096);
    };
    const auto le16 = [](const QByteArray& b, int o) {
        return qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(b.constData() + o));
    };
    const auto le32 = [](const QByteArray& b, int o) {
        return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(b.constData() + o));
    };
    const auto le64 = [](const QByteArray& b, int o) {
        return qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(b.constData() + o));
    };
    // Walk nx_superblock -> container omap header -> container omap tree -> volume APSB.
    const auto apsbBlockOf = [&](const QString& path) -> quint64 {
        const QByteArray nxsb = readBlock(path, 0);
        const QByteArray ctrHdr = readBlock(path, le64(nxsb, 0xA0));
        const QByteArray tree = readBlock(path, le64(ctrHdr, 0x30));
        return le64(tree, 4096 - 40 - 16 + 8);  // single omap entry, paddr at value+8
    };

    const quint64 baseApsb = apsbBlockOf(base);
    const QByteArray baseVol = readBlock(base, baseApsb);
    const quint64 beforeAlloc = le64(baseVol, 0x58);
    QCOMPARE(le64(baseVol, 0xD8), 0ULL);  // no snapshots yet

    const QString snap = dir.filePath(QStringLiteral("a3-snap.apfs"));
    const auto commit = PartitionApfsWriter::commitImageOnlySnapshotCreate(
        {.source_image_path = base,
         .written_image_path = snap,
         .snapshot_name = QStringLiteral("sak.a3.snapshot"),
         .create_time_ns = 1'782'096'003'133'454'505ULL,
         .options = options});
    QVERIFY2(commit.ok, qPrintable(commit.blockers.join(QStringLiteral("; "))));
    QCOMPARE(commit.previous_xid, 2ULL);
    QCOMPARE(commit.new_xid, 3ULL);

    // The live tree is shared and untouched: the reader still lists the volume.
    const auto listing =
        PartitionApfsFileSystemReader::listDirectoryFromImage(snap, QStringLiteral("/"), 20);
    QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(listing.volume_name, QStringLiteral("A3SNAP"));

    const QByteArray vol = readBlock(snap, apsbBlockOf(snap));
    QVERIFY(PartitionApfsWriter::verifyObjectChecksum(vol));
    QCOMPARE(le64(vol, 0xD8), 1ULL);                 // num_snapshots
    QCOMPARE(le64(vol, 0x58), 2 * beforeAlloc - 3);  // logical alloc count
    const quint64 snapMetaTree = le64(vol, 0x98);
    const QByteArray volOmap = readBlock(snap, le64(vol, 0x80));
    QVERIFY(PartitionApfsWriter::verifyObjectChecksum(volOmap));
    QCOMPARE(le32(volOmap, 0x24), 1U);    // om_snap_count
    QCOMPARE(le64(volOmap, 0x40), 3ULL);  // om_most_recent_snap = new xid
    const quint64 omapSnapTree = le64(volOmap, 0x38);
    QVERIFY(omapSnapTree != 0);

    // Omap snapshot tree: subtype 0x13, one fixed-kv record keyed by the snapshot xid.
    const QByteArray omapSnap = readBlock(snap, omapSnapTree);
    QVERIFY(PartitionApfsWriter::verifyObjectChecksum(omapSnap));
    QCOMPARE(le32(omapSnap, 0x1C), 0x13U);       // subtype
    QCOMPARE(le32(omapSnap, 0x24), 1U);          // nkeys
    QCOMPARE(le64(omapSnap, 0x38 + 576), 3ULL);  // key = snapshot xid

    // Snap-meta tree: two records (j_snap_metadata then j_snap_name).
    const QByteArray snapMeta = readBlock(snap, snapMetaTree);
    QVERIFY(PartitionApfsWriter::verifyObjectChecksum(snapMeta));
    QCOMPARE(le32(snapMeta, 0x1C), 0x10U);  // subtype SNAP_META
    QCOMPARE(le32(snapMeta, 0x24), 2U);     // nkeys

    // Record 0 (j_snap_metadata) -> physical frozen superblock copy that keeps the root.
    const quint16 keyOff = le16(snapMeta, 0x38);
    const quint16 valBack = le16(snapMeta, 0x38 + 4);
    const quint64 metaKey = le64(snapMeta, 0x38 + 64 + keyOff);
    QCOMPARE(metaKey >> 60, 1ULL);  // SNAP_METADATA type
    const quint64 sblockOid = le64(snapMeta, (4096 - 40) - valBack + 0x08);
    const QByteArray frozen = readBlock(snap, sblockOid);
    QVERIFY(PartitionApfsWriter::verifyObjectChecksum(frozen));
    QCOMPARE(le64(frozen, 0x08), sblockOid);            // physical object: oid == paddr
    QCOMPARE(le32(frozen, 0x18), 0x40'00'00'0DU);       // OBJ_PHYSICAL | OBJECT_TYPE_FS
    QCOMPARE(frozen.mid(0x20, 4), QByteArrayLiteral("APSB"));
    QCOMPARE(le64(frozen, 0x88), le64(baseVol, 0x88));  // root tree kept
    QCOMPARE(le64(frozen, 0x80), 0ULL);                 // omap zeroed
    QCOMPARE(le64(frozen, 0x90), 0ULL);                 // extentref zeroed
    QCOMPARE(le64(frozen, 0x98), 0ULL);                 // snap-meta zeroed
    QCOMPARE(le64(frozen, 0xD8), 1ULL);                 // frozen num_snapshots

    // A volume that already carries a snapshot fails closed (multi-snapshot is later).
    QVERIFY(!PartitionApfsWriter::commitImageOnlySnapshotCreate(
                 {.source_image_path = snap,
                  .written_image_path = dir.filePath(QStringLiteral("a3-x.apfs")),
                  .snapshot_name = QStringLiteral("second"),
                  .options = options})
                 .ok);
    // An empty snapshot name fails closed.
    QVERIFY(!PartitionApfsWriter::commitImageOnlySnapshotCreate(
                 {.source_image_path = base,
                  .written_image_path = dir.filePath(QStringLiteral("a3-y.apfs")),
                  .snapshot_name = QString(),
                  .options = options})
                 .ok);
}

void PartitionManagerCoreTests::apfsWriter_inPlaceSnapshotDeleteRestoresSnapshotFreeState() {
    // A3: deleting the single snapshot frees its frozen blocks and restores the volume
    // exactly to its snapshot-free state (num_snapshots 0, omap snapshot fields 0,
    // alloc_count back to the pre-create value). A delete with no snapshot fails closed.
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir dir(temp.path());
    const QString base = dir.filePath(QStringLiteral("a3d-base.apfs"));
    QVERIFY(PartitionApfsWriter::buildImageOnlyFormatImage(
                {.image_path = base,
                 .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
                 .block_size_bytes = 4096,
                 .volume_name = QStringLiteral("A3DEL"),
                 .options = options})
                .ok);

    const auto readBlock = [](const QString& path, quint64 block) {
        QFile file(path);
        file.open(QIODevice::ReadOnly);
        file.seek(static_cast<qint64>(block * 4096));
        return file.read(4096);
    };
    const auto le32 = [](const QByteArray& b, int o) {
        return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(b.constData() + o));
    };
    const auto le64 = [](const QByteArray& b, int o) {
        return qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(b.constData() + o));
    };
    const auto apsbBlockOf = [&](const QString& path) -> quint64 {
        const QByteArray nxsb = readBlock(path, 0);
        const QByteArray ctrHdr = readBlock(path, le64(nxsb, 0xA0));
        const QByteArray tree = readBlock(path, le64(ctrHdr, 0x30));
        return le64(tree, 4096 - 40 - 16 + 8);
    };

    const quint64 baseAlloc = le64(readBlock(base, apsbBlockOf(base)), 0x58);

    const QString snap = dir.filePath(QStringLiteral("a3d-snap.apfs"));
    QVERIFY(PartitionApfsWriter::commitImageOnlySnapshotCreate(
                {.source_image_path = base,
                 .written_image_path = snap,
                 .snapshot_name = QStringLiteral("to-delete"),
                 .options = options})
                .ok);
    const QString del = dir.filePath(QStringLiteral("a3d-del.apfs"));
    const auto d = PartitionApfsWriter::commitImageOnlySnapshotDelete(
        {.source_image_path = snap, .written_image_path = del, .options = options});
    QVERIFY2(d.ok, qPrintable(d.blockers.join(QStringLiteral("; "))));
    QCOMPARE(d.previous_xid, 3ULL);
    QCOMPARE(d.new_xid, 4ULL);

    // The volume still lists; the snapshot-free state is restored exactly.
    const auto listing =
        PartitionApfsFileSystemReader::listDirectoryFromImage(del, QStringLiteral("/"), 20);
    QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(listing.volume_name, QStringLiteral("A3DEL"));
    const QByteArray vol = readBlock(del, apsbBlockOf(del));
    QVERIFY(PartitionApfsWriter::verifyObjectChecksum(vol));
    QCOMPARE(le64(vol, 0xD8), 0ULL);       // num_snapshots back to 0
    QCOMPARE(le64(vol, 0x58), baseAlloc);  // alloc_count restored
    const QByteArray omap = readBlock(del, le64(vol, 0x80));
    QCOMPARE(le32(omap, 0x24), 0U);        // om_snap_count 0
    QCOMPARE(le64(omap, 0x38), 0ULL);      // om_snapshot_tree_oid 0
    QCOMPARE(le64(omap, 0x40), 0ULL);      // om_most_recent_snap 0

    // A delete with no snapshot fails closed.
    QVERIFY(!PartitionApfsWriter::commitImageOnlySnapshotDelete(
                 {.source_image_path = base,
                  .written_image_path = dir.filePath(QStringLiteral("a3d-x.apfs")),
                  .options = options})
                 .ok);
}

void PartitionManagerCoreTests::apfsWriter_inPlaceSnapshotRevertTagsDeferredRevert() {
    // A3: reverting the single snapshot writes Apple's deferred-revert tag - the volume
    // superblock gains revert_to_xid (the snapshot xid) + revert_to_sblock_oid (its frozen
    // superblock paddr) while everything else (num_snapshots, alloc_count, root tree,
    // next_obj_id) is unchanged and the snapshot is kept; a kernel mount then completes the
    // revert. A revert with no snapshot, or one already pending, fails closed. Byte recipe
    // harvested from a real macOS revert (temp/a3cert/revert-recipe-decoded.txt).
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir dir(temp.path());
    const QString base = dir.filePath(QStringLiteral("a3r-base.apfs"));
    QVERIFY(PartitionApfsWriter::buildImageOnlyFormatImage(
                {.image_path = base,
                 .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
                 .block_size_bytes = 4096,
                 .volume_name = QStringLiteral("A3REV"),
                 .options = options})
                .ok);

    const auto readBlock = [](const QString& path, quint64 block) {
        QFile file(path);
        file.open(QIODevice::ReadOnly);
        file.seek(static_cast<qint64>(block * 4096));
        return file.read(4096);
    };
    const auto le16 = [](const QByteArray& b, int o) {
        return qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(b.constData() + o));
    };
    const auto le32 = [](const QByteArray& b, int o) {
        return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(b.constData() + o));
    };
    const auto le64 = [](const QByteArray& b, int o) {
        return qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(b.constData() + o));
    };
    const auto apsbBlockOf = [&](const QString& path) -> quint64 {
        const QByteArray nxsb = readBlock(path, 0);
        const QByteArray ctrHdr = readBlock(path, le64(nxsb, 0xA0));
        const QByteArray tree = readBlock(path, le64(ctrHdr, 0x30));
        return le64(tree, 4096 - 40 - 16 + 8);
    };

    const QString snap = dir.filePath(QStringLiteral("a3r-snap.apfs"));
    QVERIFY(PartitionApfsWriter::commitImageOnlySnapshotCreate(
                {.source_image_path = base,
                 .written_image_path = snap,
                 .snapshot_name = QStringLiteral("to-revert"),
                 .options = options})
                .ok);

    // The snapshot state to revert to: its xid (== om_most_recent_snap) and the frozen
    // physical superblock paddr recorded in the j_snap_metadata record.
    const QByteArray snapVol = readBlock(snap, apsbBlockOf(snap));
    const quint64 snapAlloc = le64(snapVol, 0x58);
    const quint64 snapRoot = le64(snapVol, 0x88);
    const quint64 snapNextObj = le64(snapVol, 0xB0);
    const QByteArray snapMeta = readBlock(snap, le64(snapVol, 0x98));
    const quint16 valBack = le16(snapMeta, 0x38 + 4);
    const quint64 frozenSblock = le64(snapMeta, (4096 - 40) - valBack + 0x08);
    QVERIFY(frozenSblock != 0);

    const QString rev = dir.filePath(QStringLiteral("a3r-rev.apfs"));
    const auto r = PartitionApfsWriter::commitImageOnlySnapshotRevert(
        {.source_image_path = snap, .written_image_path = rev, .options = options});
    QVERIFY2(r.ok, qPrintable(r.blockers.join(QStringLiteral("; "))));
    QCOMPARE(r.previous_xid, 3ULL);
    QCOMPARE(r.new_xid, 4ULL);

    // The volume still lists; the deferred-revert tag is set and everything else is intact.
    const auto listing =
        PartitionApfsFileSystemReader::listDirectoryFromImage(rev, QStringLiteral("/"), 20);
    QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(listing.volume_name, QStringLiteral("A3REV"));
    const QByteArray vol = readBlock(rev, apsbBlockOf(rev));
    QVERIFY(PartitionApfsWriter::verifyObjectChecksum(vol));
    QCOMPARE(le64(vol, 0xA0), 3ULL);          // revert_to_xid == snapshot xid
    QCOMPARE(le64(vol, 0xA8), frozenSblock);  // revert_to_sblock_oid == frozen paddr
    QCOMPARE(le64(vol, 0xD8), 1ULL);          // snapshot kept (num_snapshots)
    QCOMPARE(le64(vol, 0x58), snapAlloc);     // alloc_count unchanged
    QCOMPARE(le64(vol, 0x88), snapRoot);      // root tree unchanged
    QCOMPARE(le64(vol, 0xB0), snapNextObj);   // next_obj_id unchanged (monotonic)
    const QByteArray omap = readBlock(rev, le64(vol, 0x80));
    QVERIFY(PartitionApfsWriter::verifyObjectChecksum(omap));
    QCOMPARE(le32(omap, 0x24), 1U);  // om_snap_count still 1 (snapshot survives)

    // A revert with no snapshot fails closed.
    QVERIFY(!PartitionApfsWriter::commitImageOnlySnapshotRevert(
                 {.source_image_path = base,
                  .written_image_path = dir.filePath(QStringLiteral("a3r-x.apfs")),
                  .options = options})
                 .ok);
    // A second revert (one already pending) fails closed.
    QVERIFY(!PartitionApfsWriter::commitImageOnlySnapshotRevert(
                 {.source_image_path = rev,
                  .written_image_path = dir.filePath(QStringLiteral("a3r-y.apfs")),
                  .options = options})
                 .ok);
}

void PartitionManagerCoreTests::apfsWriter_inPlaceFileMoveAcrossDirectories() {
    // Cross-parent move (root <-> directory) on the COW engine: the file keeps its data,
    // both parents' valences update, and a missing source/destination fails closed.
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir dir(temp.path());
    const QString base = dir.filePath(QStringLiteral("a2mv-base.apfs"));
    QVERIFY(PartitionApfsWriter::buildImageOnlyFormatImage(
                {.image_path = base,
                 .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
                 .block_size_bytes = 4096,
                 .volume_name = QStringLiteral("A2MV"),
                 .options = options})
                .ok);
    const QString withFile = dir.filePath(QStringLiteral("a2mv-file.apfs"));
    QVERIFY(
        PartitionApfsWriter::commitImageOnlyFileWrite({.source_image_path = base,
                                                       .written_image_path = withFile,
                                                       .file_name = QStringLiteral("root.txt"),
                                                       .file_data = QByteArrayLiteral("root me"),
                                                       .options = options})
            .ok);
    const QString withDir = dir.filePath(QStringLiteral("a2mv-dir.apfs"));
    QVERIFY(PartitionApfsWriter::commitImageOnlyDirectoryCreate(
                {.source_image_path = withFile,
                 .written_image_path = withDir,
                 .directory_name = QStringLiteral("docs"),
                 .options = options})
                .ok);
    const QString withChild = dir.filePath(QStringLiteral("a2mv-child.apfs"));
    QVERIFY(PartitionApfsWriter::commitImageOnlyDirectoryChildWrite(
                {.source_image_path = withDir,
                 .written_image_path = withChild,
                 .directory_name = QStringLiteral("docs"),
                 .file_name = QStringLiteral("a.txt"),
                 .file_data = QByteArrayLiteral("child-a"),
                 .options = options})
                .ok);

    // Move root.txt (root) into docs, renamed to moved.txt.
    const QString intoDir = dir.filePath(QStringLiteral("a2mv-into.apfs"));
    const auto moveIn = PartitionApfsWriter::commitImageOnlyFileMove(
        {.source_image_path = withChild,
         .written_image_path = intoDir,
         .source_directory_name = QString(),
         .file_name = QStringLiteral("root.txt"),
         .destination_directory_name = QStringLiteral("docs"),
         .new_file_name = QStringLiteral("moved.txt"),
         .options = options});
    QVERIFY2(moveIn.ok, qPrintable(moveIn.blockers.join(QStringLiteral("; "))));
    auto docs =
        PartitionApfsFileSystemReader::listDirectoryFromImage(intoDir, QStringLiteral("/docs"), 20);
    QVERIFY2(docs.ok, qPrintable(docs.blockers.join(QStringLiteral("; "))));
    QCOMPARE(docs.entries.size(), 2);  // a.txt + moved.txt
    auto root =
        PartitionApfsFileSystemReader::listDirectoryFromImage(intoDir, QStringLiteral("/"), 20);
    QVERIFY(root.ok);
    QCOMPARE(root.entries.size(), 1);  // docs (root.txt moved out)

    // Move docs/a.txt out to the root as top.txt.
    const QString outToRoot = dir.filePath(QStringLiteral("a2mv-out.apfs"));
    const auto moveOut = PartitionApfsWriter::commitImageOnlyFileMove(
        {.source_image_path = intoDir,
         .written_image_path = outToRoot,
         .source_directory_name = QStringLiteral("docs"),
         .file_name = QStringLiteral("a.txt"),
         .destination_directory_name = QString(),
         .new_file_name = QStringLiteral("top.txt"),
         .options = options});
    QVERIFY2(moveOut.ok, qPrintable(moveOut.blockers.join(QStringLiteral("; "))));
    root =
        PartitionApfsFileSystemReader::listDirectoryFromImage(outToRoot, QStringLiteral("/"), 20);
    QVERIFY(root.ok);
    QCOMPARE(root.entries.size(), 2);  // docs + top.txt
    docs = PartitionApfsFileSystemReader::listDirectoryFromImage(outToRoot,
                                                                 QStringLiteral("/docs"),
                                                                 20);
    QVERIFY(docs.ok);
    QCOMPARE(docs.entries.size(), 1);  // moved.txt

    // A missing source directory fails closed.
    QVERIFY(!PartitionApfsWriter::commitImageOnlyFileMove(
                 {.source_image_path = outToRoot,
                  .written_image_path = dir.filePath(QStringLiteral("a2mv-x.apfs")),
                  .source_directory_name = QStringLiteral("ghost"),
                  .file_name = QStringLiteral("top.txt"),
                  .destination_directory_name = QString(),
                  .new_file_name = QStringLiteral("z.txt"),
                  .options = options})
                 .ok);
}

void PartitionManagerCoreTests::apfsWriter_rawCommitWrappersMutateConfirmedTarget() {
    // The on-hardware raw commitRaw* wrappers run the same certified COW core as the
    // image-only twins. A test seam classifies a temporary container file as an acceptable
    // raw-device target so the full production orchestration (guard + open + commit +
    // readback) is exercised end to end, while the explicit-confirmation, raw-opt-in,
    // non-image-only, size, and APFS-detection guards still run. The real device-path rule
    // is restored on scope exit even if an assertion returns early.
    const PartitionApfsWriteOptions rawOptions = certifiedApfsRawCommitOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir dir(temp.path());
    const QString container = dir.filePath(QStringLiteral("a2raw.apfs"));
    const uint64_t bytes = 64ULL * 1024ULL * 1024ULL;
    QVERIFY(
        PartitionApfsWriter::buildImageOnlyFormatImage({.image_path = container,
                                                        .target_container_bytes = bytes,
                                                        .block_size_bytes = 4096,
                                                        .volume_name = QStringLiteral("A2RAW"),
                                                        .options = certifiedApfsImageOnlyOptions()})
            .ok);

    struct RawTargetPredicateGuard {
        ~RawTargetPredicateGuard() {
            PartitionApfsWriter::setRawDeviceTargetPredicateForTesting({});
        }
    } guard;
    PartitionApfsWriter::setRawDeviceTargetPredicateForTesting(
        [container](const QString& path) { return path == container; });

    const auto insert =
        PartitionApfsWriter::commitRawFileInsert({.target_path = container,
                                                  .target_container_bytes = bytes,
                                                  .file_name = QStringLiteral("seed.txt"),
                                                  .file_data = QByteArrayLiteral("seed-data"),
                                                  .target_mutation_confirmed = true,
                                                  .allow_raw_device_target = true,
                                                  .options = rawOptions});
    QVERIFY2(insert.ok, qPrintable(insert.blockers.join(QStringLiteral("; "))));

    QVERIFY(PartitionApfsWriter::commitRawFileWrite(
                {.target_path = container,
                 .target_container_bytes = bytes,
                 .file_name = QStringLiteral("seed.txt"),
                 .file_data = QByteArrayLiteral("seed-data-replaced"),
                 .target_mutation_confirmed = true,
                 .allow_raw_device_target = true,
                 .options = rawOptions})
                .ok);

    QVERIFY2(PartitionApfsWriter::commitRawDirectoryCreate(
                 {.target_path = container,
                  .target_container_bytes = bytes,
                  .directory_name = QStringLiteral("docs"),
                  .target_mutation_confirmed = true,
                  .allow_raw_device_target = true,
                  .options = rawOptions})
                 .ok,
             "raw create docs");
    QVERIFY(PartitionApfsWriter::commitRawDirectoryChildWrite(
                {.target_path = container,
                 .target_container_bytes = bytes,
                 .directory_name = QStringLiteral("docs"),
                 .file_name = QStringLiteral("a.txt"),
                 .file_data = QByteArrayLiteral("child-data"),
                 .target_mutation_confirmed = true,
                 .allow_raw_device_target = true,
                 .options = rawOptions})
                .ok);

    const auto docsListing = PartitionApfsFileSystemReader::listDirectoryFromImage(
        container, QStringLiteral("/docs"), 20);
    QVERIFY2(docsListing.ok, qPrintable(docsListing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(docsListing.entries.size(), 1);
    QCOMPARE(docsListing.entries.first().name, QStringLiteral("a.txt"));

    QVERIFY(PartitionApfsWriter::commitRawDirectoryChildDelete(
                {.target_path = container,
                 .target_container_bytes = bytes,
                 .directory_name = QStringLiteral("docs"),
                 .file_name = QStringLiteral("a.txt"),
                 .target_mutation_confirmed = true,
                 .allow_raw_device_target = true,
                 .options = rawOptions})
                .ok);
    QVERIFY(
        PartitionApfsWriter::commitRawFileRename({.target_path = container,
                                                  .target_container_bytes = bytes,
                                                  .file_name = QStringLiteral("seed.txt"),
                                                  .new_file_name = QStringLiteral("renamed.txt"),
                                                  .target_mutation_confirmed = true,
                                                  .allow_raw_device_target = true,
                                                  .options = rawOptions})
            .ok);
    QVERIFY(PartitionApfsWriter::commitRawFileDelete({.target_path = container,
                                                      .target_container_bytes = bytes,
                                                      .file_name = QStringLiteral("renamed.txt"),
                                                      .target_mutation_confirmed = true,
                                                      .allow_raw_device_target = true,
                                                      .options = rawOptions})
                .ok);
    QVERIFY2(PartitionApfsWriter::commitRawDirectoryDelete(
                 {.target_path = container,
                  .target_container_bytes = bytes,
                  .directory_name = QStringLiteral("docs"),
                  .target_mutation_confirmed = true,
                  .allow_raw_device_target = true,
                  .options = rawOptions})
                 .ok,
             "raw delete empty docs");

    const auto rootListing =
        PartitionApfsFileSystemReader::listDirectoryFromImage(container, QStringLiteral("/"), 20);
    QVERIFY2(rootListing.ok, qPrintable(rootListing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(rootListing.entries.size(), 0);
}

void PartitionManagerCoreTests::apfsWriter_rawCommitWrappersFailClosedWithoutRawDevice() {
    // Defense in depth: the raw commit wrappers fail closed on a non-raw image-file path,
    // and even on an accepted raw path still independently require explicit confirmation
    // and non-image-only options. The directory routes reach the same guard as the file
    // routes.
    const PartitionApfsWriteOptions rawOptions = certifiedApfsRawCommitOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir dir(temp.path());
    const QString container = dir.filePath(QStringLiteral("a2rawfc.apfs"));
    const uint64_t bytes = 64ULL * 1024ULL * 1024ULL;
    QVERIFY(
        PartitionApfsWriter::buildImageOnlyFormatImage({.image_path = container,
                                                        .target_container_bytes = bytes,
                                                        .block_size_bytes = 4096,
                                                        .volume_name = QStringLiteral("A2RFC"),
                                                        .options = certifiedApfsImageOnlyOptions()})
            .ok);

    // No predicate installed: a plain file path is not a raw device.
    const auto noDevice =
        PartitionApfsWriter::commitRawDirectoryCreate({.target_path = container,
                                                       .target_container_bytes = bytes,
                                                       .directory_name = QStringLiteral("docs"),
                                                       .target_mutation_confirmed = true,
                                                       .allow_raw_device_target = true,
                                                       .options = rawOptions});
    QVERIFY(!noDevice.ok);
    QVERIFY(noDevice.blockers.join(QLatin1Char(' '))
                .contains(QStringLiteral("requires a Windows raw-device path")));

    struct RawTargetPredicateGuard {
        ~RawTargetPredicateGuard() {
            PartitionApfsWriter::setRawDeviceTargetPredicateForTesting({});
        }
    } guard;
    PartitionApfsWriter::setRawDeviceTargetPredicateForTesting(
        [container](const QString& path) { return path == container; });

    const auto unconfirmed =
        PartitionApfsWriter::commitRawDirectoryCreate({.target_path = container,
                                                       .target_container_bytes = bytes,
                                                       .directory_name = QStringLiteral("docs"),
                                                       .target_mutation_confirmed = false,
                                                       .allow_raw_device_target = true,
                                                       .options = rawOptions});
    QVERIFY(!unconfirmed.ok);
    QVERIFY(unconfirmed.blockers.join(QLatin1Char(' '))
                .contains(QStringLiteral("explicit target confirmation")));

    PartitionApfsWriteOptions imageOnly = rawOptions;
    imageOnly.image_only = true;
    const auto imageOnlyReject =
        PartitionApfsWriter::commitRawDirectoryCreate({.target_path = container,
                                                       .target_container_bytes = bytes,
                                                       .directory_name = QStringLiteral("docs"),
                                                       .target_mutation_confirmed = true,
                                                       .allow_raw_device_target = true,
                                                       .options = imageOnly});
    QVERIFY(!imageOnlyReject.ok);
    QVERIFY(
        imageOnlyReject.blockers.join(QLatin1Char(' ')).contains(QStringLiteral("non-image-only")));
}

void PartitionManagerCoreTests::apfsWriter_formatsMetadataOverflowDeadZoneMultiBlockSpaceman() {
    // The ~2.9-7.8 TiB metadata-overflow "dead zone" (cib_count 182-507) overflows the
    // spaceman's inline cib-address array; the spaceman object spans a second block
    // instead of failing closed, and Apple forbids the CAB tier below 508 cibs. This is
    // Apple-certified (kernel auto-mount + fsck_apfs container/volume clean on a 4 TiB
    // container). The image is created sparse, so the 4 TiB logical size is cheap.
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString image = QDir(temp.path()).filePath(QStringLiteral("a2ddz.apfs"));
    const uint64_t bytes = 4ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL;  // 4 TiB -> 261 cibs
    const auto build =
        PartitionApfsWriter::buildImageOnlyFormatImage({.image_path = image,
                                                        .target_container_bytes = bytes,
                                                        .block_size_bytes = 4096,
                                                        .volume_name = QStringLiteral("A2DDZ"),
                                                        .options = options});
    QVERIFY2(build.ok, qPrintable(build.blockers.join(QStringLiteral("; "))));

    // The geometry sits in the dead zone: more cibs than fit one inline array, but still
    // at or below the 507-cib CAB threshold (cab_count stays 0).
    const auto geometry = PartitionApfsWriter::computeContainerGeometry(bytes / 4096, 4096);
    QVERIFY(geometry.cib_count > 181);
    QVERIFY(geometry.cib_count <= 507);
    QVERIFY(geometry.cab_count == 0);

    // The live checkpoint map (block 3) resolves the spaceman; its first entry is the
    // space manager, whose cpm_size must report a two-block object (the overflow spill).
    QFile file(image);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(file.seek(3 * 4096));
    const QByteArray map = file.read(4096);
    file.close();
    QCOMPARE(map.size(), static_cast<qsizetype>(4096));
    constexpr qsizetype kMapEntriesOffset = 0x28;
    constexpr qsizetype kMapEntrySizeOffset = 8;
    const uint32_t spacemanCpmSize = readTestApfsLe32(map, kMapEntriesOffset + kMapEntrySizeOffset);
    QCOMPARE(spacemanCpmSize, 2u * 4096u);
}

void PartitionManagerCoreTests::apfsWriter_formatsMultiVolumeContainer() {
    // A4 multi-volume containers: a second volume gets its own omap, root/extent-ref/
    // snap-meta trees, superblock, fs_index, virtual OIDs, and UUID, all sharing the
    // container space manager. fsck_apfs requires nx_max_file_systems = ceil(bytes /
    // 512 MiB), so two volumes need a > 512 MiB container; 1 GiB yields max_fs 2.
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir dir(temp.path());
    const QString image = dir.filePath(QStringLiteral("a4-multivol.apfs"));
    const auto build = PartitionApfsWriter::buildImageOnlyFormatImage(
        {.image_path = image,
         .target_container_bytes = 1024ULL * 1024ULL * 1024ULL,
         .block_size_bytes = 4096,
         .volume_name = QStringLiteral("SAKVOL1"),
         .additional_volume_names = {QStringLiteral("SAKVOL2")},
         .options = options});
    QVERIFY2(build.ok, qPrintable(build.blockers.join(QStringLiteral("; "))));

    const auto readBlock = [&](quint64 block) {
        QFile file(image);
        file.open(QIODevice::ReadOnly);
        file.seek(static_cast<qint64>(block * 4096));
        return file.read(4096);
    };
    const auto le32 = [](const QByteArray& bytes, int offset) {
        return qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(bytes.constData() + offset));
    };
    const auto le64 = [](const QByteArray& bytes, int offset) {
        return qFromLittleEndian<quint64>(
            reinterpret_cast<const uchar*>(bytes.constData() + offset));
    };

    // Container superblock: nx_max_file_systems = 2 (1 GiB), nx_fs_oid carries both
    // volume OIDs (1026, 1030) with the rest of the array zero, and nx_next_oid clears
    // every assigned virtual OID (superblock + root tree per volume).
    const QByteArray nxsb = readBlock(0);
    QVERIFY(PartitionApfsWriter::verifyObjectChecksum(nxsb));
    QCOMPARE(le32(nxsb, 0xB4), 2U);
    QCOMPARE(le64(nxsb, 0xB8), 1026ULL);
    QCOMPARE(le64(nxsb, 0xC0), 1030ULL);
    QCOMPARE(le64(nxsb, 0xC8), 0ULL);
    QCOMPARE(le64(nxsb, 0x58), 1032ULL);  // nx_next_oid

    // Locate both volume superblocks by their APSB magic in the reserved prefix.
    QList<quint64> apsbBlocks;
    for (quint64 block = 185; block < 1024; ++block) {
        const QByteArray candidate = readBlock(block);
        if (candidate.size() == 4096 && le32(candidate, 0x20) == 0x42'53'50'41U /* 'APSB' */) {
            apsbBlocks.append(block);
        }
    }
    QCOMPARE(apsbBlocks.size(), 2);

    const QByteArray vol1 = readBlock(apsbBlocks.at(0));
    const QByteArray vol2 = readBlock(apsbBlocks.at(1));
    QVERIFY(PartitionApfsWriter::verifyObjectChecksum(vol1));
    QVERIFY(PartitionApfsWriter::verifyObjectChecksum(vol2));
    // fs_index @0x24, superblock OID @0x08, root-tree OID @0x88 are distinct per volume.
    QCOMPARE(le32(vol1, 0x24), 0U);
    QCOMPARE(le32(vol2, 0x24), 1U);
    QCOMPARE(le64(vol1, 0x08), 1026ULL);
    QCOMPARE(le64(vol2, 0x08), 1030ULL);
    QCOMPARE(le64(vol1, 0x88), 1028ULL);
    QCOMPARE(le64(vol2, 0x88), 1031ULL);
    QCOMPARE(QString::fromUtf8(QByteArray(vol1.constData() + 0x2C0)), QStringLiteral("SAKVOL1"));
    QCOMPARE(QString::fromUtf8(QByteArray(vol2.constData() + 0x2C0)), QStringLiteral("SAKVOL2"));
    // Each volume carries its own UUID (@0xF0).
    QVERIFY(vol1.mid(0xF0, 16) != vol2.mid(0xF0, 16));

    // The reader still walks the first volume by default.
    const auto listing =
        PartitionApfsFileSystemReader::listDirectoryFromImage(image, QStringLiteral("/"), 20);
    QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(listing.volume_name, QStringLiteral("SAKVOL1"));

    // Too-small container for the requested volume count is fail-closed: 64 MiB yields
    // nx_max_file_systems 1, which cannot cover two volumes.
    const auto tooSmall = PartitionApfsWriter::buildImageOnlyFormatImage(
        {.image_path = dir.filePath(QStringLiteral("a4-small.apfs")),
         .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
         .block_size_bytes = 4096,
         .volume_name = QStringLiteral("SAKVOL1"),
         .additional_volume_names = {QStringLiteral("SAKVOL2")},
         .options = options});
    QVERIFY(!tooSmall.ok);
    QVERIFY(tooSmall.blockers.join(' ').contains(QStringLiteral("nx_max_file_systems")));
}

void PartitionManagerCoreTests::apfsWriter_insertsInlineCompressedFile() {
    // A5: insert a transparently-compressed file (inline zlib com.apple.decmpfs)
    // and prove the byte-match round trip the macOS kernel performs: the writer
    // stores the content in an embedded decmpfs xattr with no data stream, and the
    // reader decodes that attribute back to the exact original bytes.
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir dir(temp.path());
    const QString base = dir.filePath(QStringLiteral("a5-base.apfs"));
    QVERIFY2(PartitionApfsWriter::buildImageOnlyFormatImage(
                 {.image_path = base,
                  .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
                  .block_size_bytes = 4096,
                  .volume_name = QStringLiteral("SAKA5"),
                  .options = options})
                 .ok,
             "format A5 base");

    // A compressible payload (>1 block uncompressed) that still fits an embedded
    // decmpfs attribute once zlib-deflated.
    QByteArray payload;
    for (int line = 0; line < 400; ++line) {
        payload.append(
            QStringLiteral("APFS transparent compression round-trip line %1\n").arg(line).toUtf8());
    }
    QVERIFY(payload.size() > 4096);

    const QString out = dir.filePath(QStringLiteral("a5-out.apfs"));
    const auto commit =
        PartitionApfsWriter::commitImageOnlyFileInsert({.source_image_path = base,
                                                        .written_image_path = out,
                                                        .file_name = QStringLiteral("doc.txt"),
                                                        .file_data = payload,
                                                        .compress_zlib = true,
                                                        .options = options});
    QVERIFY2(commit.ok, qPrintable(commit.blockers.join(QStringLiteral("; "))));
    QCOMPARE(commit.new_xid, 3ULL);

    // The listing reports the file's logical (uncompressed) size from the decmpfs
    // header even though the inode carries no data stream.
    const auto listing =
        PartitionApfsFileSystemReader::listDirectoryFromImage(out, QStringLiteral("/"), 20);
    QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(listing.entries.size(), 1);
    QCOMPARE(listing.entries.first().name, QStringLiteral("doc.txt"));
    QCOMPARE(listing.entries.first().size_bytes, static_cast<uint64_t>(payload.size()));

    // Byte-match decode: the reader follows the decmpfs attribute and returns the
    // exact uncompressed content.
    const auto read = PartitionApfsFileSystemReader::readFileFromImage(
        out, QStringLiteral("/doc.txt"), static_cast<uint64_t>(payload.size()));
    QVERIFY2(read.ok, qPrintable(read.blockers.join(QStringLiteral("; "))));
    QCOMPARE(read.data, payload);

    // A truncated read still decodes and returns just the requested prefix.
    const auto partial =
        PartitionApfsFileSystemReader::readFileFromImage(out, QStringLiteral("/doc.txt"), 100);
    QVERIFY2(partial.ok, qPrintable(partial.blockers.join(QStringLiteral("; "))));
    QCOMPARE(partial.data, payload.left(100));

    // The image's container superblock is self-consistent (object checksum intact,
    // COW xid advanced); the inserted file allocated no data block (compressed
    // inline, so the commit copied only the metadata chain).
    QFile outImage(out);
    QVERIFY(outImage.open(QIODevice::ReadOnly));
    const QByteArray nxsb = outImage.read(4096);
    outImage.close();
    QVERIFY(PartitionApfsWriter::verifyObjectChecksum(nxsb));
    QCOMPARE(qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(nxsb.constData() + 0x10)),
             3ULL);

    // Fail-closed: an incompressible payload whose decmpfs value would exceed the
    // embedded-xattr limit is rejected (resource-fork compression is a follow-on).
    QByteArray incompressible(8192, '\0');
    uint32_t state = 0x12'34'56'78U;
    for (int i = 0; i < incompressible.size(); ++i) {
        state = state * 1'103'515'245U + 12'345U;  // high-entropy LCG: zlib cannot shrink it
        incompressible[i] = static_cast<char>(state >> 16);
    }
    const auto tooBig = PartitionApfsWriter::commitImageOnlyFileInsert(
        {.source_image_path = base,
         .written_image_path = dir.filePath(QStringLiteral("a5-toobig.apfs")),
         .file_name = QStringLiteral("big.bin"),
         .file_data = incompressible,
         .compress_zlib = true,
         .options = options});
    QVERIFY(!tooBig.ok);
    QVERIFY(tooBig.blockers.join(' ').contains(QStringLiteral("embedded-xattr limit")));
}

void PartitionManagerCoreTests::apfsWriter_insertsSparseAndXattrFile() {
    // A7 (A-h): insert one file that is BOTH sparse (a trailing hole) and carries
    // arbitrary named extended attributes (an ACL in com.apple.system.Security, a
    // Finder-info blob, a user attribute). The reader must zero-fill the hole and
    // surface every attribute; the on-disk structure is apfsck/kernel-certified.
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir dir(temp.path());
    const QString base = dir.filePath(QStringLiteral("a7-base.apfs"));
    QVERIFY2(PartitionApfsWriter::buildImageOnlyFormatImage(
                 {.image_path = base,
                  .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
                  .block_size_bytes = 4096,
                  .volume_name = QStringLiteral("SAKA7"),
                  .options = options})
                 .ok,
             "format A7 base");

    const QByteArray payload = QByteArrayLiteral("sparse data in the first block\n");
    const uint64_t logicalSize = 64ULL * 1024ULL;  // ~16 blocks; only block 0 allocated
    const QByteArray acl = QByteArrayLiteral(
        "\x00\x00\x00\x04"
        "acl-blob-bytes");
    const QByteArray finder(32, '\x07');
    const QVector<QPair<QByteArray, QByteArray>> xattrs{
        {QByteArrayLiteral("com.apple.system.Security"), acl},
        {QByteArrayLiteral("com.apple.FinderInfo"), finder},
        {QByteArrayLiteral("user.note"), QByteArrayLiteral("hello-xattr")}};

    const QString out = dir.filePath(QStringLiteral("a7-out.apfs"));
    const auto commit =
        PartitionApfsWriter::commitImageOnlyFileInsert({.source_image_path = base,
                                                        .written_image_path = out,
                                                        .file_name = QStringLiteral("sparse.bin"),
                                                        .file_data = payload,
                                                        .xattrs = xattrs,
                                                        .sparse_logical_size = logicalSize,
                                                        .options = options});
    QVERIFY2(commit.ok, qPrintable(commit.blockers.join(QStringLiteral("; "))));
    QCOMPARE(commit.new_xid, 3ULL);

    // The listing reports the full (logical) sparse size.
    const auto listing =
        PartitionApfsFileSystemReader::listDirectoryFromImage(out, QStringLiteral("/"), 20);
    QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(listing.entries.size(), 1);
    QCOMPARE(listing.entries.first().size_bytes, logicalSize);

    // The read reassembles the data plus the trailing hole as zeros.
    const auto read = PartitionApfsFileSystemReader::readFileFromImage(
        out, QStringLiteral("/sparse.bin"), logicalSize);
    QVERIFY2(read.ok, qPrintable(read.blockers.join(QStringLiteral("; "))));
    QCOMPARE(static_cast<uint64_t>(read.data.size()), logicalSize);
    QCOMPARE(read.data.left(payload.size()), payload);
    QCOMPARE(read.data.right(static_cast<qsizetype>(logicalSize) - payload.size()),
             QByteArray(static_cast<qsizetype>(logicalSize) - payload.size(), '\0'));

    // Every named attribute round-trips by name + value.
    QMap<QString, QByteArray> got;
    for (const auto& x : read.xattrs) {
        got.insert(x.first, x.second);
    }
    QCOMPARE(got.value(QStringLiteral("com.apple.system.Security")), acl);
    QCOMPARE(got.value(QStringLiteral("com.apple.FinderInfo")), finder);
    QCOMPARE(got.value(QStringLiteral("user.note")), QByteArrayLiteral("hello-xattr"));

    // The container superblock stays self-consistent (object checksum + xid).
    QFile outImage(out);
    QVERIFY(outImage.open(QIODevice::ReadOnly));
    const QByteArray nxsb = outImage.read(4096);
    outImage.close();
    QVERIFY(PartitionApfsWriter::verifyObjectChecksum(nxsb));
}

void PartitionManagerCoreTests::apfsWriter_clonesFileSharingDataStream() {
    // A7 (A-h) file clone: cloning a file shares its data stream -- no data is copied.
    // A new inode is added whose private id points at the source's dstream; the shared
    // dstream's reference count rises to 2, and both inodes are flagged
    // WAS_CLONED/WAS_EVER_CLONED. The on-disk structure is apfsck/kernel-certified, and
    // the reader resolves both names (source + clone) to the identical bytes.
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir dir(temp.path());
    const QString base = dir.filePath(QStringLiteral("clone-base.apfs"));
    QVERIFY2(PartitionApfsWriter::buildImageOnlyFormatImage(
                 {.image_path = base,
                  .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
                  .block_size_bytes = 4096,
                  .volume_name = QStringLiteral("SAKA7CL"),
                  .options = options})
                 .ok,
             "format clone base");

    // A multi-block source file so the clone genuinely shares > 1 extent block.
    const QByteArray payload = QByteArray("clone-source-payload-").repeated(400);
    const QString src = dir.filePath(QStringLiteral("clone-src.apfs"));
    const auto insert =
        PartitionApfsWriter::commitImageOnlyFileInsert({.source_image_path = base,
                                                        .written_image_path = src,
                                                        .file_name = QStringLiteral("orig.bin"),
                                                        .file_data = payload,
                                                        .options = options});
    QVERIFY2(insert.ok, qPrintable(insert.blockers.join(QStringLiteral("; "))));

    const QString out = dir.filePath(QStringLiteral("clone-out.apfs"));
    const auto commit = PartitionApfsWriter::commitImageOnlyFileClone(
        {.source_image_path = src,
         .written_image_path = out,
         .source_file_name = QStringLiteral("orig.bin"),
         .clone_file_name = QStringLiteral("clone.bin"),
         .options = options});
    QVERIFY2(commit.ok, qPrintable(commit.blockers.join(QStringLiteral("; "))));
    QCOMPARE(commit.new_xid, 4ULL);

    // Both names are present, each reporting the shared stream's logical size.
    const auto listing =
        PartitionApfsFileSystemReader::listDirectoryFromImage(out, QStringLiteral("/"), 20);
    QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(listing.entries.size(), 2);
    for (const auto& entry : listing.entries) {
        QCOMPARE(entry.size_bytes, static_cast<uint64_t>(payload.size()));
    }

    // The source and the clone both read back the identical bytes (shared extents).
    const auto sourceRead = PartitionApfsFileSystemReader::readFileFromImage(
        out, QStringLiteral("/orig.bin"), static_cast<uint64_t>(payload.size()));
    QVERIFY2(sourceRead.ok, qPrintable(sourceRead.blockers.join(QStringLiteral("; "))));
    QCOMPARE(sourceRead.data, payload);
    const auto cloneRead = PartitionApfsFileSystemReader::readFileFromImage(
        out, QStringLiteral("/clone.bin"), static_cast<uint64_t>(payload.size()));
    QVERIFY2(cloneRead.ok, qPrintable(cloneRead.blockers.join(QStringLiteral("; "))));
    QCOMPARE(cloneRead.data, payload);

    // Cloning a source that does not exist fails closed.
    const auto missing = PartitionApfsWriter::commitImageOnlyFileClone(
        {.source_image_path = src,
         .written_image_path = dir.filePath(QStringLiteral("clone-miss.apfs")),
         .source_file_name = QStringLiteral("absent.bin"),
         .clone_file_name = QStringLiteral("x.bin"),
         .options = options});
    QVERIFY(!missing.ok);

    // The container superblock stays self-consistent (object checksum).
    QFile outImage(out);
    QVERIFY(outImage.open(QIODevice::ReadOnly));
    const QByteArray nxsb = outImage.read(4096);
    outImage.close();
    QVERIFY(PartitionApfsWriter::verifyObjectChecksum(nxsb));
}

void PartitionManagerCoreTests::apfsWriter_addsHardLinkToFile() {
    // A7 (A-h) hard links: adding a second name to a file makes both names resolve to
    // the same inode (link count 2). No data or inode is copied; sibling-link and
    // sibling-map records track the two names. apfsck/kernel-certified; the reader reads
    // the identical bytes through either name.
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir dir(temp.path());
    const QString base = dir.filePath(QStringLiteral("hl-base.apfs"));
    QVERIFY2(PartitionApfsWriter::buildImageOnlyFormatImage(
                 {.image_path = base,
                  .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
                  .block_size_bytes = 4096,
                  .volume_name = QStringLiteral("SAKA7HL"),
                  .options = options})
                 .ok,
             "format hard-link base");

    const QByteArray payload = QByteArray("hard-link-payload-").repeated(300);
    const QString src = dir.filePath(QStringLiteral("hl-src.apfs"));
    const auto insert =
        PartitionApfsWriter::commitImageOnlyFileInsert({.source_image_path = base,
                                                        .written_image_path = src,
                                                        .file_name = QStringLiteral("orig.bin"),
                                                        .file_data = payload,
                                                        .options = options});
    QVERIFY2(insert.ok, qPrintable(insert.blockers.join(QStringLiteral("; "))));

    const QString out = dir.filePath(QStringLiteral("hl-out.apfs"));
    const auto commit = PartitionApfsWriter::commitImageOnlyFileHardlink(
        {.source_image_path = src,
         .written_image_path = out,
         .source_file_name = QStringLiteral("orig.bin"),
         .link_file_name = QStringLiteral("link.bin"),
         .options = options});
    QVERIFY2(commit.ok, qPrintable(commit.blockers.join(QStringLiteral("; "))));
    QCOMPARE(commit.new_xid, 4ULL);

    // Both names are present, each reporting the inode's size.
    const auto listing =
        PartitionApfsFileSystemReader::listDirectoryFromImage(out, QStringLiteral("/"), 20);
    QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(listing.entries.size(), 2);
    for (const auto& entry : listing.entries) {
        QCOMPARE(entry.size_bytes, static_cast<uint64_t>(payload.size()));
    }

    // Both names read back the identical bytes (one inode, one data stream).
    const auto origRead = PartitionApfsFileSystemReader::readFileFromImage(
        out, QStringLiteral("/orig.bin"), static_cast<uint64_t>(payload.size()));
    QVERIFY2(origRead.ok, qPrintable(origRead.blockers.join(QStringLiteral("; "))));
    QCOMPARE(origRead.data, payload);
    const auto linkRead = PartitionApfsFileSystemReader::readFileFromImage(
        out, QStringLiteral("/link.bin"), static_cast<uint64_t>(payload.size()));
    QVERIFY2(linkRead.ok, qPrintable(linkRead.blockers.join(QStringLiteral("; "))));
    QCOMPARE(linkRead.data, payload);

    // Linking a source that does not exist fails closed.
    const auto missing = PartitionApfsWriter::commitImageOnlyFileHardlink(
        {.source_image_path = src,
         .written_image_path = dir.filePath(QStringLiteral("hl-miss.apfs")),
         .source_file_name = QStringLiteral("absent.bin"),
         .link_file_name = QStringLiteral("y.bin"),
         .options = options});
    QVERIFY(!missing.ok);

    // The container superblock stays self-consistent (object checksum).
    QFile outImage(out);
    QVERIFY(outImage.open(QIODevice::ReadOnly));
    const QByteArray nxsb = outImage.read(4096);
    outImage.close();
    QVERIFY(PartitionApfsWriter::verifyObjectChecksum(nxsb));
}

void PartitionManagerCoreTests::apfsWriter_blocksSealedVolumeMutation() {
    // A7 (A-h) sealed-volume policy: an in-place commit must fail closed on a
    // signed-system (sealed) volume -- mutating it breaks the volume seal. The
    // gate fires for APFS_INCOMPAT_SEALED_VOLUME and for an integrity-meta object.
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir dir(temp.path());
    const QString base = dir.filePath(QStringLiteral("seal-base.apfs"));
    QVERIFY(PartitionApfsWriter::buildImageOnlyFormatImage(
                {.image_path = base,
                 .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
                 .block_size_bytes = 4096,
                 .volume_name = QStringLiteral("SAKSEAL"),
                 .options = options})
                .ok);

    // A normal (unsealed) volume still mutates.
    QVERIFY(PartitionApfsWriter::commitImageOnlyFileInsert(
                {.source_image_path = base,
                 .written_image_path = dir.filePath(QStringLiteral("seal-ok.apfs")),
                 .file_name = QStringLiteral("a.txt"),
                 .file_data = QByteArrayLiteral("hi"),
                 .options = options})
                .ok);

    // Set APFS_INCOMPAT_SEALED_VOLUME (0x20) in the volume superblock -> blocked.
    constexpr uint32_t kApsbMagic = 0x42'53'50'41;  // 'APSB'
    const QString sealedFeature = dir.filePath(QStringLiteral("seal-feature.apfs"));
    QVERIFY(QFile::copy(base, sealedFeature));
    QVERIFY(tamperApfsBlockField(sealedFeature, kApsbMagic, 0x38, 0x21, false));
    const auto blockedFeature = PartitionApfsWriter::commitImageOnlyFileInsert(
        {.source_image_path = sealedFeature,
         .written_image_path = dir.filePath(QStringLiteral("seal-feature-out.apfs")),
         .file_name = QStringLiteral("a.txt"),
         .file_data = QByteArrayLiteral("hi"),
         .options = options});
    QVERIFY(!blockedFeature.ok);
    QVERIFY(blockedFeature.blockers.join(' ').contains(QStringLiteral("sealed")));

    // An integrity-meta object reference (apfs_integrity_meta_oid != 0) -> blocked.
    const QString sealedMeta = dir.filePath(QStringLiteral("seal-meta.apfs"));
    QVERIFY(QFile::copy(base, sealedMeta));
    QVERIFY(tamperApfsBlockField(sealedMeta, kApsbMagic, 0x400, 0x2002, false));
    const auto blockedMeta = PartitionApfsWriter::commitImageOnlyFileInsert(
        {.source_image_path = sealedMeta,
         .written_image_path = dir.filePath(QStringLiteral("seal-meta-out.apfs")),
         .file_name = QStringLiteral("a.txt"),
         .file_data = QByteArrayLiteral("hi"),
         .options = options});
    QVERIFY(!blockedMeta.ok);
    QVERIFY(blockedMeta.blockers.join(' ').contains(QStringLiteral("sealed")));
}

void PartitionManagerCoreTests::apfsCrypto_matchesPublishedVectors() {
    // A6: the FileVault crypto primitives are certified against published test
    // vectors before any APFS keybag depends on them. A single wrong byte here
    // would silently corrupt every encrypted volume, so each primitive is pinned
    // to an independent standard vector (FIPS 180-4 / FIPS 197 / RFC 4231 / RFC
    // 3394 / IEEE 1619), with PBKDF2 cross-checked against this module's own HMAC.
    using namespace sak::apfs_crypto;

    // SHA-256("abc") — FIPS 180-4 example B.1.
    QCOMPARE(sha256(QByteArrayLiteral("abc")).toHex(),
             QByteArray("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    // HMAC-SHA-256 — RFC 4231 Test Case 1.
    QCOMPARE(hmacSha256(QByteArray::fromHex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"),
                        QByteArrayLiteral("Hi There"))
                 .toHex(),
             QByteArray("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));

    // PBKDF2-HMAC-SHA-256(P, S, c=1, dkLen=32) is, by definition, the first HMAC
    // block: HMAC(P, S || INT_BE32(1)). Cross-checking against this module's own
    // HMAC pins the PBKDF2 wiring (salt/password/length) without a memorized
    // vector; the HMAC itself is already pinned to RFC 4231 above.
    const QByteArray pwd = QByteArrayLiteral("passwd");
    const QByteArray salt = QByteArrayLiteral("salt");
    QCOMPARE(pbkdf2Sha256(pwd, salt, 1, 32),
             hmacSha256(pwd, salt + QByteArray::fromHex("00000001")));
    // Iterations are honoured (c=2 differs from c=1) and multi-block output spans.
    QVERIFY(pbkdf2Sha256(pwd, salt, 2, 32) != pbkdf2Sha256(pwd, salt, 1, 32));
    QCOMPARE(pbkdf2Sha256(pwd, salt, 4, 64).size(), 64);

    // RFC 3394 §4.6 — wrap a 256-bit key under a 256-bit KEK.
    const QByteArray kek =
        QByteArray::fromHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    const QByteArray key =
        QByteArray::fromHex("00112233445566778899aabbccddeeff000102030405060708090a0b0c0d0e0f");
    const QByteArray wrapped = aesKeyWrap(kek, key);
    QCOMPARE(wrapped.toHex(),
             QByteArray("28c9f404c4b810f4cbccb35cfb87f8263f5786e2d80ed326cbc7f0e71a99f43b"
                        "fb988b9b7a02dd21"));
    const auto unwrapped = aesKeyUnwrap(kek, wrapped);
    QVERIFY(unwrapped.has_value());
    QCOMPARE(*unwrapped, key);

    // Wrong KEK fails the RFC 3394 integrity check (A6A6...) — never silent garbage.
    QByteArray wrongKek = kek;
    wrongKek[0] = static_cast<char>(wrongKek[0] ^ 0x01);
    QVERIFY(!aesKeyUnwrap(wrongKek, wrapped).has_value());

    // AES-128 ECB pinned to the FIPS-197 known-answer (Appendix B / C.1); this is
    // the primitive the AES-XTS construction is built from.
    QCOMPARE(aesEcbEncryptBlock(QByteArray::fromHex("000102030405060708090a0b0c0d0e0f"),
                                QByteArray::fromHex("00112233445566778899aabbccddeeff"))
                 .toHex(),
             QByteArray("69c4e0d86a7b0430d8cdb78070b4c55a"));

    // AES-XTS — IEEE Std 1619-2007 Vector 1 (32-byte data unit, sequence 0,
    // all-zero key and plaintext): a definitive external known-answer.
    const QByteArray zeroKey(32, '\0');
    const QByteArray zeroPt(32, '\0');
    const QByteArray vec1 = xtsEncrypt(zeroKey, 0, zeroPt, 32);
    QCOMPARE(vec1.toHex(),
             QByteArray("917cf69ebd68b2ec9b9fe9a3eadda692cd43d2f59598ed858c02c2652fbf922e"));
    QCOMPARE(xtsDecrypt(zeroKey, 0, vec1, 32), zeroPt);

    // Round trip at the APFS 512-byte data-unit size with a non-trivial key/tweak.
    QByteArray xtsKey(32, 0);
    for (int i = 0; i < 16; ++i) {
        xtsKey[i] = static_cast<char>(0x20 + i);
        xtsKey[16 + i] = static_cast<char>(0x40 + i);
    }
    QByteArray ptx(512, 0);
    for (int i = 0; i < ptx.size(); ++i) {
        ptx[i] = static_cast<char>((i * 5 + 1) & 0xFF);
    }
    const uint64_t dataUnit = 0x01'02'03'04'05'06'07'08ULL;
    const QByteArray ctx = xtsEncrypt(xtsKey, dataUnit, ptx, 512);
    QCOMPARE(ctx.size(), 512);
    QVERIFY(ctx != ptx);
    QCOMPARE(xtsDecrypt(xtsKey, dataUnit, ctx, 512), ptx);
    // Multi-unit input advances the tweak per unit (unit 1 != unit 0 ciphertext).
    QByteArray twoUnits = ptx + ptx;
    const QByteArray ctx2 = xtsEncrypt(xtsKey, dataUnit, twoUnits, 512);
    QVERIFY(ctx2.left(512) != ctx2.mid(512));
    QCOMPARE(xtsDecrypt(xtsKey, dataUnit, ctx2, 512), twoUnits);

    // APFS 4096-byte block round trip: 512-byte data units, tweak base = addr * 8.
    QByteArray vek(32, 0);
    for (int i = 0; i < vek.size(); ++i) {
        vek[i] = static_cast<char>(0x10 + i);
    }
    QByteArray block(4096, 0);
    for (int i = 0; i < block.size(); ++i) {
        block[i] = static_cast<char>(i * 7 + 3);
    }
    const QByteArray encBlock = xtsEncryptBlock(vek, 198, block);
    QCOMPARE(encBlock.size(), block.size());
    QVERIFY(encBlock != block);
    QCOMPARE(xtsDecryptBlock(vek, 198, encBlock), block);
    // The tweak is address-bound: the same plaintext at a different block differs.
    QVERIFY(xtsEncryptBlock(vek, 199, block) != encBlock);
}

void PartitionManagerCoreTests::apfsKeybag_reproducesHarvestedFileVaultBlobs() {
    // A6: the keybag/DER-blob builders are pinned to bytes harvested from a REAL
    // macOS software-FileVault volume (diskutil apfs addVolume -passphrase
    // sakpass1234). Reproducing those bytes exactly, and recovering the same VEK
    // the macOS kernel derives, is what lets the kernel unlock a S.A.K. volume.
    using namespace sak::apfs_keybag;
    const QByteArray uuid = QByteArray::fromHex("d822b542ebba48c3a760f2e35e991fb5");
    const QByteArray wrappedKek = QByteArray::fromHex(
        "98a9c7a040c1fa4cdfab4fd57c5a60cfb82f8e03d776b097a4ed0c6b039d6685124a6ee96a108d73");
    const QByteArray kekSalt = QByteArray::fromHex("6a89f1d655c0880685544950244f7a2d");
    const QByteArray wrappedVek = QByteArray::fromHex(
        "03a51ebf59a36d9d51658de808a8aa2f9811a5e4eb852aa344bfd85128755da3d3ed19dde8750dab");

    // Byte-exact KEK blob (volume keybag KB_TAG_VOLUME_UNLOCK_RECORDS payload).
    const QByteArray kekBlob = buildKekBlob({.uuid = uuid,
                                             .wrappedKey = wrappedKek,
                                             .flags8 = QByteArray::fromHex("000000000200bcac"),
                                             .outerSalt = QByteArray::fromHex("e2fa559ef3ab357d"),
                                             .iterations = 113'939,
                                             .salt = kekSalt});
    QCOMPARE(kekBlob.toHex(),
             QByteArray("3081918001008120f3ad4e06ab404e03607f00445b46b01016a5ac022ecba8dc8db2b62"
                        "7667d014c8208e2fa559ef3ab357da3608001008110d822b542ebba48c3a760f2e35e991"
                        "fb58208000000000200bcac832898a9c7a040c1fa4cdfab4fd57c5a60cfb82f8e03d776b0"
                        "97a4ed0c6b039d6685124a6ee96a108d73840301bd1385106a89f1d655c08806855449502"
                        "44f7a2d"));

    // Byte-exact VEK blob (container keybag KB_TAG_VOLUME_KEY payload).
    const QByteArray vekBlob = buildVekBlob({.uuid = uuid,
                                             .wrappedKey = wrappedVek,
                                             .flags8 = QByteArray::fromHex("000000000100bcac"),
                                             .outerSalt = QByteArray::fromHex("8e48b75f1a87d3bb")});
    QCOMPARE(vekBlob.toHex(),
             QByteArray("307a80010081202b546ddc5bd92afe4a13d556d724e800b7b680a4a1d235a691f7395cfe1"
                        "181a982088e48b75f1a87d3bba3498001008110d822b542ebba48c3a760f2e35e991fb5820"
                        "8000000000100bcac832803a51ebf59a36d9d51658de808a8aa2f9811a5e4eb852aa344bfd"
                        "85128755da3d3ed19dde8750dab"));

    // The harvested unlock chain: PBKDF2 -> unwrap KEK -> unwrap VEK == the real
    // VEK that XTS-decrypts the volume fs-tree on disk.
    const QByteArray derived =
        sak::apfs_crypto::pbkdf2Sha256(QByteArrayLiteral("sakpass1234"), kekSalt, 113'939, 32);
    const auto kek = sak::apfs_crypto::aesKeyUnwrap(derived, wrappedKek);
    QVERIFY(kek.has_value());
    QCOMPARE(kek->toHex(),
             QByteArray("8577094d56f91a131e83268a7de7f0c61036c950cd72075c7118d9ad6393df87"));
    const auto vek = sak::apfs_crypto::aesKeyUnwrap(*kek, wrappedVek);
    QVERIFY(vek.has_value());
    QCOMPARE(vek->toHex(),
             QByteArray("e23bad1abfb21839e58e5b64d6654e91ea802952ada97b377b68073e9a8c383a"));

    // Container keybag block matches the harvested topology: o_type 'keys'
    // (bytes 73 79 65 6b), kl_version 2, 2 entries, kl_nbytes = 16 + 48 + 160 = 224.
    QList<KeybagEntry> entries;
    entries.append(
        {uuid, kKbTagVolumeUnlockRecords, QByteArray::fromHex("af010000000000000100000000000000")});
    entries.append({uuid, kKbTagVolumeKey, vekBlob});
    const QByteArray kb = buildKeybagBlock(kApfsObjectTypeContainerKeybag, 0, 2, entries, 4096);
    QCOMPARE(kb.size(), 4096);
    QCOMPARE(kb.mid(0x18, 4).toHex(), QByteArray("7379656b"));
    const auto* p = reinterpret_cast<const uchar*>(kb.constData());
    QCOMPARE(qFromLittleEndian<quint16>(p + 0x20), quint16(2));    // kl_version
    QCOMPARE(qFromLittleEndian<quint16>(p + 0x22), quint16(2));    // kl_nkeys
    QCOMPARE(qFromLittleEndian<quint32>(p + 0x24), quint32(224));  // kl_nbytes

    // parseKeybagBlock round-trips the entries; parseKeyBlob recovers the fields.
    const QList<KeybagEntry> parsed = parseKeybagBlock(kb);
    QCOMPARE(parsed.size(), 2);
    QCOMPARE(parsed.at(1).tag, kKbTagVolumeKey);
    KeyBlobParams vp;
    QVERIFY(parseKeyBlob(parsed.at(1).keydata, &vp));
    QCOMPARE(vp.uuid, uuid);
    QCOMPARE(vp.wrappedKey, wrappedVek);
}

void PartitionManagerCoreTests::apfsWriter_formatsUnlockableEncryptedVolume() {
    // A6: the writer produces a software-encrypted (FileVault) volume that the
    // macOS kernel can unlock. This test does exactly what the kernel does on
    // mount: walk the on-disk keybag chain, derive the VEK from the password, and
    // AES-XTS-decrypt the volume file-system tree to a valid, checksum-correct node.
    using namespace sak::apfs_crypto;
    using namespace sak::apfs_keybag;
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString img = QDir(temp.path()).filePath(QStringLiteral("enc.apfs"));
    const QByteArray password = QByteArrayLiteral("sakpass1234");
    QVERIFY2(PartitionApfsWriter::buildImageOnlyFormatImage(
                 {.image_path = img,
                  .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
                  .block_size_bytes = 4096,
                  .volume_name = QStringLiteral("SAKENC"),
                  .volume_password = QString::fromUtf8(password),
                  .options = options})
                 .ok,
             "format encrypted volume");

    QFile f(img);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const auto readBlk = [&f](uint64_t blk) {
        f.seek(static_cast<qint64>(blk) * 4096);
        return f.read(4096);
    };
    const auto le64 = [](const QByteArray& b, int off) {
        return qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(b.constData() + off));
    };

    // nx_keylocker @ 0x510 points at the container keybag.
    const QByteArray nxsb = readBlk(0);
    const QByteArray nxUuid = nxsb.mid(0x48, 16);
    const uint64_t keylocker = le64(nxsb, 0x510);
    QVERIFY(keylocker != 0);

    // Decrypt + parse the container keybag (XTS key = container UUID).
    const QList<KeybagEntry> containerEntries =
        parseKeybagBlock(xtsDecryptBlock(nxUuid + nxUuid, keylocker, readBlk(keylocker)));
    QCOMPARE(containerEntries.size(), 2);
    QByteArray volUuid;
    QByteArray vekBlob;
    uint64_t volumeKbBlock = 0;
    for (const auto& e : containerEntries) {
        if (e.tag == kKbTagVolumeKey) {
            volUuid = e.uuid;
            vekBlob = e.keydata;
        } else if (e.tag == kKbTagVolumeUnlockRecords) {
            volumeKbBlock = le64(e.keydata, 0);
        }
    }
    QVERIFY(!vekBlob.isEmpty());
    QVERIFY(volumeKbBlock != 0);

    // Decrypt + parse the per-volume keybag (XTS key = volume UUID) -> KEK blob.
    const QList<KeybagEntry> volumeEntries =
        parseKeybagBlock(xtsDecryptBlock(volUuid + volUuid, volumeKbBlock, readBlk(volumeKbBlock)));
    QByteArray kekBlob;
    for (const auto& e : volumeEntries) {
        if (e.tag == kKbTagVolumeUnlockRecords) {
            kekBlob = e.keydata;
        }
    }
    QVERIFY(!kekBlob.isEmpty());

    // The unlock chain: PBKDF2(password) -> unwrap KEK -> unwrap VEK.
    KeyBlobParams kek;
    KeyBlobParams vek;
    QVERIFY(parseKeyBlob(kekBlob, &kek));
    QVERIFY(parseKeyBlob(vekBlob, &vek));
    const QByteArray derived = pbkdf2Sha256(password, kek.salt, kek.iterations, 32);
    const auto kekKey = aesKeyUnwrap(derived, kek.wrappedKey);
    QVERIFY(kekKey.has_value());
    const auto vekKey = aesKeyUnwrap(*kekKey, vek.wrappedKey);
    QVERIFY(vekKey.has_value());
    QCOMPARE(vekKey->size(), 32);

    // APSB (block 198, single-chunk 64 MiB) is plaintext + flagged ONEKEY-encrypted.
    const QByteArray apsb = readBlk(198);
    QCOMPARE(apsb.mid(0x20, 4), QByteArrayLiteral("APSB"));
    QCOMPARE(le64(apsb, 0x108), quint64(0x8));

    // The file-system tree (block 197) decrypts with the VEK to a valid, Fletcher-64
    // correct b-tree node -- the volume is genuinely unlockable.
    const QByteArray fsTree = xtsDecryptBlock(*vekKey, 197, readBlk(197));
    QVERIFY(PartitionApfsWriter::verifyObjectChecksum(fsTree));
    const auto otype =
        qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(fsTree.constData() + 0x18));
    QVERIFY(otype == 2 || otype == 3);

    // A wrong password fails the RFC 3394 integrity check (no silent bad unlock).
    const QByteArray wrongDerived =
        pbkdf2Sha256(QByteArrayLiteral("wrongpw"), kek.salt, kek.iterations, 32);
    QVERIFY(!aesKeyUnwrap(wrongDerived, kek.wrappedKey).has_value());
}

void PartitionManagerCoreTests::apfsReader_decryptsEncryptedVolumeWithCredential() {
    // A6 read-side: the S.A.K. APFS reader itself (not just the macOS kernel)
    // unlocks a software-encrypted volume from the credential, walks the keybag
    // chain to the VEK, and AES-XTS-decrypts the fs-tree to list the volume.
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString img = QDir(temp.path()).filePath(QStringLiteral("rdr-enc.apfs"));
    const QString password = QStringLiteral("sakpass1234");
    QVERIFY2(PartitionApfsWriter::buildImageOnlyFormatImage(
                 {.image_path = img,
                  .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
                  .block_size_bytes = 4096,
                  .volume_name = QStringLiteral("SAKENC"),
                  .volume_password = password,
                  .options = options})
                 .ok,
             "format encrypted volume");

    // Correct credential -> the reader decrypts + lists the volume.
    const auto unlocked = PartitionApfsFileSystemReader::listDirectoryFromImage(
        img, QStringLiteral("/"), 1000, password);
    QVERIFY2(unlocked.ok, qPrintable(unlocked.blockers.join(QStringLiteral("; "))));
    QCOMPARE(unlocked.volume_name, QStringLiteral("SAKENC"));

    // Wrong credential -> fail closed (no silent bad unlock, no garbage listing).
    const auto wrong = PartitionApfsFileSystemReader::listDirectoryFromImage(
        img, QStringLiteral("/"), 1000, QStringLiteral("not-the-password"));
    QVERIFY(!wrong.ok);
    QVERIFY(wrong.blockers.join(QStringLiteral("; ")).contains(QStringLiteral("incorrect")));

    // No credential -> fail closed with an explicit "encrypted" blocker.
    const auto locked =
        PartitionApfsFileSystemReader::listDirectoryFromImage(img, QStringLiteral("/"), 1000);
    QVERIFY(!locked.ok);
    QVERIFY(locked.blockers.join(QStringLiteral("; ")).contains(QStringLiteral("encrypted")));

    // A plaintext volume still lists with no credential (no regression).
    const QString plain = QDir(temp.path()).filePath(QStringLiteral("rdr-plain.apfs"));
    QVERIFY(PartitionApfsWriter::buildImageOnlyFormatImage(
                {.image_path = plain,
                 .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
                 .block_size_bytes = 4096,
                 .volume_name = QStringLiteral("SAKPLAIN"),
                 .options = options})
                .ok);
    const auto plainListing =
        PartitionApfsFileSystemReader::listDirectoryFromImage(plain, QStringLiteral("/"), 1000);
    QVERIFY2(plainListing.ok, qPrintable(plainListing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(plainListing.volume_name, QStringLiteral("SAKPLAIN"));
}

void PartitionManagerCoreTests::apfsWriter_recoveryKeyUnlocksEncryptedVolume() {
    // A6 follow-on: a volume formatted with BOTH a password and a personal recovery
    // key carries two unlock records (the same KEK wrapped by each credential), so
    // the reader unlocks it with EITHER secret -- and rejects anything else.
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString img = QDir(temp.path()).filePath(QStringLiteral("prk-enc.apfs"));
    const QString password = QStringLiteral("sakpass1234");
    const QString recoveryKey = QStringLiteral("ASDF-1234-QWER-5678-ZXCV-9012");
    QVERIFY2(PartitionApfsWriter::buildImageOnlyFormatImage(
                 {.image_path = img,
                  .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
                  .block_size_bytes = 4096,
                  .volume_name = QStringLiteral("SAKPRK"),
                  .volume_password = password,
                  .recovery_key = recoveryKey,
                  .options = options})
                 .ok,
             "format encrypted volume with recovery key");

    // The password still unlocks the volume.
    const auto byPassword = PartitionApfsFileSystemReader::listDirectoryFromImage(
        img, QStringLiteral("/"), 1000, password);
    QVERIFY2(byPassword.ok, qPrintable(byPassword.blockers.join(QStringLiteral("; "))));
    QCOMPARE(byPassword.volume_name, QStringLiteral("SAKPRK"));

    // The personal recovery key unlocks the SAME volume via its own unlock record.
    const auto byRecovery = PartitionApfsFileSystemReader::listDirectoryFromImage(
        img, QStringLiteral("/"), 1000, recoveryKey);
    QVERIFY2(byRecovery.ok, qPrintable(byRecovery.blockers.join(QStringLiteral("; "))));
    QCOMPARE(byRecovery.volume_name, QStringLiteral("SAKPRK"));

    // A credential matching neither record fails closed.
    const auto wrong = PartitionApfsFileSystemReader::listDirectoryFromImage(
        img, QStringLiteral("/"), 1000, QStringLiteral("NOPE-0000-NOPE-0000-NOPE-0000"));
    QVERIFY(!wrong.ok);
    QVERIFY(wrong.blockers.join(QStringLiteral("; ")).contains(QStringLiteral("incorrect")));
}

void PartitionManagerCoreTests::apfsKeybag_failsClosedOnMalformedEntry() {
    // Defense-in-depth: buildKeybagBlock copies a fixed 16-byte ke_uuid per entry,
    // so a malformed (undersized-uuid) entry must fail closed -- return empty -- not
    // read past the source buffer or emit a corrupt keybag. This guards the OOB the
    // adversarial review found in the encryption-material failure path.
    using namespace sak::apfs_keybag;
    const QByteArray goodUuid(16, '\x11');
    const QByteArray key = QByteArrayLiteral("unlock-record-bytes");

    // A well-formed entry builds a full block.
    QVERIFY(
        !buildKeybagBlock(
             kApfsObjectTypeVolumeKeybag, 0, 2, {{goodUuid, kKbTagVolumeUnlockRecords, key}}, 4096)
             .isEmpty());

    // An empty-uuid entry (what buildVolumeUnlockRecord returns on key-wrap failure)
    // fails closed.
    QVERIFY(buildKeybagBlock(kApfsObjectTypeVolumeKeybag,
                             0,
                             2,
                             {{QByteArray(), kKbTagVolumeUnlockRecords, key}},
                             4096)
                .isEmpty());

    // An entry set that cannot fit the block fails closed (no overflow write).
    QVERIFY(buildKeybagBlock(kApfsObjectTypeVolumeKeybag,
                             0,
                             2,
                             {{goodUuid, kKbTagVolumeUnlockRecords, QByteArray(8192, 'x')}},
                             4096)
                .isEmpty());
}

void PartitionManagerCoreTests::apfsWriter_inPlaceFileInsertWritesMultiBlockExtent() {
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir dir(temp.path());
    const QString base = dir.filePath(QStringLiteral("a2mb-base.apfs"));
    const auto build = PartitionApfsWriter::buildImageOnlyFormatImage(
        {.image_path = base,
         .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
         .block_size_bytes = 4096,
         .volume_name = QStringLiteral("A2MB"),
         .options = options});
    QVERIFY2(build.ok, qPrintable(build.blockers.join(QStringLiteral("; "))));

    // A 5000-byte payload spans two 4096-byte blocks -> one two-block extent.
    QByteArray payload(5000, '\0');
    for (int i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>((i * 37 + 11) & 0xFF);
    }
    const QString out = dir.filePath(QStringLiteral("a2mb-out.apfs"));
    const auto commit =
        PartitionApfsWriter::commitImageOnlyFileInsert({.source_image_path = base,
                                                        .written_image_path = out,
                                                        .file_name = QStringLiteral("big.bin"),
                                                        .file_data = payload,
                                                        .options = options});
    QVERIFY2(commit.ok, qPrintable(commit.blockers.join(QStringLiteral("; "))));

    const auto listing =
        PartitionApfsFileSystemReader::listDirectoryFromImage(out, QStringLiteral("/"), 20);
    QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(listing.entries.size(), 1);
    QCOMPARE(listing.entries.first().size_bytes, static_cast<uint64_t>(payload.size()));

    // Two contiguous data blocks (208, 209) past the seven COW chain blocks hold
    // the full payload.
    QFile image(out);
    QVERIFY(image.open(QIODevice::ReadOnly));
    image.seek(208 * 4096);
    const QByteArray storedData = image.read(payload.size());
    QCOMPARE(storedData, payload);
}

void PartitionManagerCoreTests::apfsWriter_inPlaceFileInsertChainsAndPreservesExistingFiles() {
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir dir(temp.path());
    const QString c0 = dir.filePath(QStringLiteral("cc0.apfs"));
    const auto build = PartitionApfsWriter::buildImageOnlyFormatImage(
        {.image_path = c0,
         .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
         .block_size_bytes = 4096,
         .volume_name = QStringLiteral("CC"),
         .options = options});
    QVERIFY2(build.ok, qPrintable(build.blockers.join(QStringLiteral("; "))));

    // Insert two files with distinct content in two chained in-place commits.
    const QByteArray firstData = QByteArrayLiteral("FIRST FILE CONTENT aaaa");
    const QByteArray secondData = QByteArrayLiteral("SECOND FILE different bbbb");
    const QString c1 = dir.filePath(QStringLiteral("cc1.apfs"));
    const auto commit1 =
        PartitionApfsWriter::commitImageOnlyFileInsert({.source_image_path = c0,
                                                        .written_image_path = c1,
                                                        .file_name = QStringLiteral("f1.txt"),
                                                        .file_data = firstData,
                                                        .options = options});
    QVERIFY2(commit1.ok, qPrintable(commit1.blockers.join(QStringLiteral("; "))));
    QCOMPARE(commit1.new_xid, 3ULL);

    const QString c2 = dir.filePath(QStringLiteral("cc2.apfs"));
    const auto commit2 =
        PartitionApfsWriter::commitImageOnlyFileInsert({.source_image_path = c1,
                                                        .written_image_path = c2,
                                                        .file_name = QStringLiteral("f2.txt"),
                                                        .file_data = secondData,
                                                        .options = options});
    QVERIFY2(commit2.ok, qPrintable(commit2.blockers.join(QStringLiteral("; "))));
    QCOMPARE(commit2.new_xid, 4ULL);

    // Both files are present, and the first file's content survived the second
    // chained commit (its data extent was preserved in place).
    const auto listing =
        PartitionApfsFileSystemReader::listDirectoryFromImage(c2, QStringLiteral("/"), 20);
    QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(listing.entries.size(), 2);

    const auto firstRead =
        PartitionApfsFileSystemReader::readFileFromImage(c2, QStringLiteral("/f1.txt"), 4096);
    QVERIFY2(firstRead.ok, qPrintable(firstRead.blockers.join(QStringLiteral("; "))));
    QCOMPARE(firstRead.data, firstData);
    const auto secondRead =
        PartitionApfsFileSystemReader::readFileFromImage(c2, QStringLiteral("/f2.txt"), 4096);
    QVERIFY2(secondRead.ok, qPrintable(secondRead.blockers.join(QStringLiteral("; "))));
    QCOMPARE(secondRead.data, secondData);

    // Inserting a duplicate name fails closed.
    const auto duplicate = PartitionApfsWriter::commitImageOnlyFileInsert(
        {.source_image_path = c2,
         .written_image_path = dir.filePath(QStringLiteral("dup.apfs")),
         .file_name = QStringLiteral("f1.txt"),
         .file_data = {},
         .options = options});
    QVERIFY(!duplicate.ok);
    QVERIFY(duplicate.blockers.join(' ').contains(QStringLiteral("already exists")));
}

void PartitionManagerCoreTests::apfsWriter_inPlaceFileDeleteRemovesFileAndPreservesOthers() {
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir dir(temp.path());
    const QString d0 = dir.filePath(QStringLiteral("del0.apfs"));
    const auto build = PartitionApfsWriter::buildImageOnlyFormatImage(
        {.image_path = d0,
         .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
         .block_size_bytes = 4096,
         .volume_name = QStringLiteral("DEL"),
         .options = options});
    QVERIFY2(build.ok, qPrintable(build.blockers.join(QStringLiteral("; "))));

    // Build three content files via chained inserts.
    const QByteArray a = QByteArrayLiteral("AAAA file one content");
    const QByteArray b = QByteArrayLiteral("BBBB file two content");
    const QByteArray c = QByteArrayLiteral("CCCC file three content");
    const QString d1 = dir.filePath(QStringLiteral("del1.apfs"));
    const QString d2 = dir.filePath(QStringLiteral("del2.apfs"));
    const QString d3 = dir.filePath(QStringLiteral("del3.apfs"));
    QVERIFY(PartitionApfsWriter::commitImageOnlyFileInsert({.source_image_path = d0,
                                                            .written_image_path = d1,
                                                            .file_name = "f1.txt",
                                                            .file_data = a,
                                                            .options = options})
                .ok);
    QVERIFY(PartitionApfsWriter::commitImageOnlyFileInsert({.source_image_path = d1,
                                                            .written_image_path = d2,
                                                            .file_name = "f2.txt",
                                                            .file_data = b,
                                                            .options = options})
                .ok);
    QVERIFY(PartitionApfsWriter::commitImageOnlyFileInsert({.source_image_path = d2,
                                                            .written_image_path = d3,
                                                            .file_name = "f3.txt",
                                                            .file_data = c,
                                                            .options = options})
                .ok);

    // Delete the middle file in place.
    const QString d4 = dir.filePath(QStringLiteral("del4.apfs"));
    const auto del =
        PartitionApfsWriter::commitImageOnlyFileDelete({.source_image_path = d3,
                                                        .written_image_path = d4,
                                                        .file_name = QStringLiteral("f2.txt"),
                                                        .options = options});
    QVERIFY2(del.ok, qPrintable(del.blockers.join(QStringLiteral("; "))));

    // Only f1 and f3 remain, with their content intact.
    const auto listing =
        PartitionApfsFileSystemReader::listDirectoryFromImage(d4, QStringLiteral("/"), 20);
    QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(listing.entries.size(), 2);
    QStringList names;
    for (const auto& entry : listing.entries) {
        names.append(entry.name);
    }
    QVERIFY(names.contains(QStringLiteral("f1.txt")));
    QVERIFY(names.contains(QStringLiteral("f3.txt")));
    QVERIFY(!names.contains(QStringLiteral("f2.txt")));
    QCOMPARE(
        PartitionApfsFileSystemReader::readFileFromImage(d4, QStringLiteral("/f1.txt"), 4096).data,
        a);
    QCOMPARE(
        PartitionApfsFileSystemReader::readFileFromImage(d4, QStringLiteral("/f3.txt"), 4096).data,
        c);

    // Deleting a missing file fails closed.
    const auto missing = PartitionApfsWriter::commitImageOnlyFileDelete(
        {.source_image_path = d4,
         .written_image_path = dir.filePath(QStringLiteral("del5.apfs")),
         .file_name = QStringLiteral("nope.txt"),
         .options = options});
    QVERIFY(!missing.ok);
    QVERIFY(missing.blockers.join(' ').contains(QStringLiteral("not found")));
}

void PartitionManagerCoreTests::apfsWriter_inPlaceFileRenameKeepsContentAndObjectId() {
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir dir(temp.path());
    const QString r0 = dir.filePath(QStringLiteral("rn0.apfs"));
    const auto build = PartitionApfsWriter::buildImageOnlyFormatImage(
        {.image_path = r0,
         .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
         .block_size_bytes = 4096,
         .volume_name = QStringLiteral("RN"),
         .options = options});
    QVERIFY2(build.ok, qPrintable(build.blockers.join(QStringLiteral("; "))));

    const QByteArray payload = QByteArrayLiteral("renamed file keeps content");
    const QString r1 = dir.filePath(QStringLiteral("rn1.apfs"));
    QVERIFY(PartitionApfsWriter::commitImageOnlyFileInsert({.source_image_path = r0,
                                                            .written_image_path = r1,
                                                            .file_name = "old.txt",
                                                            .file_data = payload,
                                                            .options = options})
                .ok);

    const QString r2 = dir.filePath(QStringLiteral("rn2.apfs"));
    const auto rename =
        PartitionApfsWriter::commitImageOnlyFileRename({.source_image_path = r1,
                                                        .written_image_path = r2,
                                                        .file_name = QStringLiteral("old.txt"),
                                                        .new_file_name = QStringLiteral("new.txt"),
                                                        .options = options});
    QVERIFY2(rename.ok, qPrintable(rename.blockers.join(QStringLiteral("; "))));

    // The file is renamed but keeps its content (its data extent stayed put).
    const auto listing =
        PartitionApfsFileSystemReader::listDirectoryFromImage(r2, QStringLiteral("/"), 20);
    QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));
    QCOMPARE(listing.entries.size(), 1);
    QCOMPARE(listing.entries.first().name, QStringLiteral("new.txt"));
    QCOMPARE(
        PartitionApfsFileSystemReader::readFileFromImage(r2, QStringLiteral("/new.txt"), 4096).data,
        payload);

    // Renaming a missing file, or to an existing name, fails closed.
    QVERIFY(!PartitionApfsWriter::commitImageOnlyFileRename(
                 {.source_image_path = r2,
                  .written_image_path = dir.filePath(QStringLiteral("rn-miss.apfs")),
                  .file_name = QStringLiteral("nope.txt"),
                  .new_file_name = QStringLiteral("x.txt"),
                  .options = options})
                 .ok);
    const QString r3 = dir.filePath(QStringLiteral("rn3.apfs"));
    QVERIFY(PartitionApfsWriter::commitImageOnlyFileInsert({.source_image_path = r2,
                                                            .written_image_path = r3,
                                                            .file_name = "keep.txt",
                                                            .file_data = {},
                                                            .options = options})
                .ok);
    const auto collide = PartitionApfsWriter::commitImageOnlyFileRename(
        {.source_image_path = r3,
         .written_image_path = dir.filePath(QStringLiteral("rn-collide.apfs")),
         .file_name = QStringLiteral("new.txt"),
         .new_file_name = QStringLiteral("keep.txt"),
         .options = options});
    QVERIFY(!collide.ok);
    QVERIFY(collide.blockers.join(' ').contains(QStringLiteral("already exists")));
}

void PartitionManagerCoreTests::apfsWriter_inPlaceFileInsertGrowsIntoMultiLeafFsTree() {
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir dir(temp.path());
    QString current = dir.filePath(QStringLiteral("ml0.apfs"));
    QVERIFY(PartitionApfsWriter::buildImageOnlyFormatImage(
                {.image_path = current,
                 .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
                 .block_size_bytes = 4096,
                 .volume_name = QStringLiteral("ML"),
                 .options = options})
                .ok);

    // Chained inserts keep succeeding past the ~15-file single-leaf cap: the
    // fs-tree splits into an internal root over leaf nodes and the reader still
    // lists every file (no silent truncation). The only accepted stop is the
    // checkpoint data ring filling - a separate, known limit, not corruption.
    int inserted = 0;
    for (int index = 1; index <= 40; ++index) {
        const QString next = dir.filePath(QStringLiteral("ml%1.apfs").arg(index));
        const auto commit = PartitionApfsWriter::commitImageOnlyFileInsert(
            {.source_image_path = current,
             .written_image_path = next,
             .file_name = QStringLiteral("f%1.txt").arg(index),
             .file_data = {},
             .options = options});
        if (!commit.ok) {
            QVERIFY2(commit.blockers.join(' ').contains(QStringLiteral("data ring would wrap")),
                     qPrintable(commit.blockers.join(QStringLiteral("; "))));
            break;
        }
        const auto listing =
            PartitionApfsFileSystemReader::listDirectoryFromImage(next, QStringLiteral("/"), 64);
        QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));
        QCOMPARE(listing.entries.size(), index);  // multi-leaf: every file readable
        inserted = index;
        current = next;
    }
    QVERIFY2(inserted > 15, "expected chained inserts to grow past the single-leaf fs-tree cap");
}

void PartitionManagerCoreTests::apfsWriter_buildsTwoLevelFsTreeOnOverflow() {
    QStringList names;
    for (int index = 0; index < 40; ++index) {
        names << QStringLiteral("file%1.txt").arg(index);
    }
    QStringList blockers;
    const auto blocks = PartitionApfsWriter::buildFsTreeNodeBlocks(4096, names, 1030, &blockers);
    QVERIFY2(blockers.isEmpty(), qPrintable(blockers.join(QStringLiteral("; "))));
    QVERIFY2(blocks.size() > 1, "expected the fs-tree to split into an internal root + leaves");

    const auto le16 = [](const QByteArray& b, int o) {
        return qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(b.constData() + o));
    };
    const auto le32 = [](const QByteArray& b, int o) {
        return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(b.constData() + o));
    };
    const auto le64 = [](const QByteArray& b, int o) {
        return qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(b.constData() + o));
    };

    // Every node carries a valid object checksum.
    for (const auto& block : blocks) {
        QVERIFY(PartitionApfsWriter::verifyObjectChecksum(block));
    }
    // Root: ROOT (0x01, not LEAF), level 1, one record per leaf, oid 1028.
    const QByteArray& root = blocks.first();
    QCOMPARE(le16(root, 0x20), static_cast<quint16>(0x0001));
    QCOMPARE(le16(root, 0x22), static_cast<quint16>(1));
    QCOMPARE(le32(root, 0x24), static_cast<quint32>(blocks.size() - 1));
    QCOMPARE(le64(root, 0x08), static_cast<quint64>(1028));
    // Leaves: LEAF (0x02), level 0, consecutive oids from 1030.
    QSet<quint64> leafOids;
    for (int index = 1; index < blocks.size(); ++index) {
        QCOMPARE(le16(blocks[index], 0x20), static_cast<quint16>(0x0002));
        QCOMPARE(le16(blocks[index], 0x22), static_cast<quint16>(0));
        QCOMPARE(le64(blocks[index], 0x08), static_cast<quint64>(1030 + index - 1));
        leafOids.insert(le64(blocks[index], 0x08));
    }
    // Each root record's 8-byte value is a child leaf oid (value offsets are
    // measured back from the value-area end).
    const int valueAreaEnd = 4096 - 40;
    for (int index = 0; index < static_cast<int>(le32(root, 0x24)); ++index) {
        const int valueOff = le16(root, 0x38 + index * 8 + 4);
        QVERIFY(leafOids.contains(le64(root, valueAreaEnd - valueOff)));
    }
}

void PartitionManagerCoreTests::apfsWriter_crashSafeIpSlotRoundRobinsThreeSlots() {
    // A generated 64 MiB single-chunk container's internal pool has three
    // cib/bitmap slots: (185,186), (187,188), (189,190). A crash-safe commit
    // writes the next slot after the live cib, round-robin, so the previous
    // checkpoint's cib/bitmap survive an interrupted commit.
    const quint64 blocks = 16'384;  // 64 MiB / 4096
    const auto from187 = PartitionApfsWriter::nextCrashSafeIpSlot(187, blocks);
    QCOMPARE(from187.first, static_cast<quint64>(189));
    QCOMPARE(from187.second, static_cast<quint64>(190));
    const auto from189 = PartitionApfsWriter::nextCrashSafeIpSlot(189, blocks);
    QCOMPARE(from189.first, static_cast<quint64>(185));
    QCOMPARE(from189.second, static_cast<quint64>(186));
    const auto from185 = PartitionApfsWriter::nextCrashSafeIpSlot(185, blocks);
    QCOMPARE(from185.first, static_cast<quint64>(187));
    QCOMPARE(from185.second, static_cast<quint64>(188));
    // Full cycle returns to the start (no slot is ever the live one).
    quint64 cib = 187;
    for (int i = 0; i < 3; ++i) {
        cib = PartitionApfsWriter::nextCrashSafeIpSlot(cib, blocks).first;
    }
    QCOMPARE(cib, static_cast<quint64>(187));
}

void PartitionManagerCoreTests::apfsWriter_readsGeneratedLiveCibAddr() {
    // Reading the live spaceman's cib_addr (the crash-safe rotation's starting
    // point) walks the live checkpoint-map to the ephemeral spaceman. A freshly
    // formatted single-chunk container reports the genesis live cib block, 187.
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = QDir(temp.path()).filePath(QStringLiteral("cib.apfs"));
    QVERIFY(PartitionApfsWriter::buildImageOnlyFormatImage(
                {.image_path = path,
                 .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
                 .block_size_bytes = 4096,
                 .volume_name = QStringLiteral("CIB"),
                 .options = options})
                .ok);
    QCOMPARE(PartitionApfsWriter::readGeneratedLiveCibAddr(path), static_cast<quint64>(187));
}

void PartitionManagerCoreTests::apfsWriter_crashBeforeCheckpointDurableRollsBack() {
    // Crash-safety: a commit interrupted before its checkpoint becomes durable
    // must roll back to the previous checkpoint with its data intact. The cib/
    // bitmap and COW-chain both rotate, so the interrupted commit never touches
    // the previous checkpoint's blocks.
    const PartitionApfsWriteOptions options = certifiedApfsImageOnlyOptions();
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir dir(temp.path());
    const auto commit = [&](const QString& src, const QString& dst, const QString& name) {
        return PartitionApfsWriter::commitImageOnlyFileInsert({.source_image_path = src,
                                                               .written_image_path = dst,
                                                               .file_name = name,
                                                               .file_data = {},
                                                               .options = options})
            .ok;
    };
    const QString fmt = dir.filePath(QStringLiteral("r0.apfs"));
    const QString a = dir.filePath(QStringLiteral("ra.apfs"));
    const QString n = dir.filePath(QStringLiteral("rn.apfs"));    // last committed (2 files)
    const QString n1 = dir.filePath(QStringLiteral("rn1.apfs"));  // interrupted commit (3 files)
    QVERIFY(PartitionApfsWriter::buildImageOnlyFormatImage(
                {.image_path = fmt,
                 .target_container_bytes = 64ULL * 1024ULL * 1024ULL,
                 .block_size_bytes = 4096,
                 .volume_name = QStringLiteral("ROLLBACK"),
                 .options = options})
                .ok);
    QVERIFY(commit(fmt, a, QStringLiteral("alpha.txt")));
    QVERIFY(commit(a, n, QStringLiteral("bravo.txt")));
    QVERIFY(commit(n, n1, QStringLiteral("charlie.txt")));

    const auto readImage = [](const QString& p) {
        QFile f(p);
        f.open(QIODevice::ReadOnly);
        return f.readAll();
    };
    const auto le64 = [](const QByteArray& b, int o) {
        return qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(b.constData() + o));
    };
    const auto le32 = [](const QByteArray& b, int o) {
        return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(b.constData() + o));
    };
    const QByteArray nBytes = readImage(n);
    QByteArray crashedBytes = readImage(n1);
    const quint64 descBase = le64(crashedBytes, 0x70);   // nx_xp_desc_base
    const quint32 descIndex = le32(crashedBytes, 0x88);  // nx_xp_desc_index
    crashedBytes.replace(0, 4096, nBytes.left(4096));    // block 0 -> previous checkpoint
    const qsizetype checkpoint = static_cast<qsizetype>((descBase + descIndex) * 4096);
    crashedBytes.replace(checkpoint,
                         8192,
                         QByteArray(8192, '\0'));  // void the interrupted checkpoint

    const QString crashed = dir.filePath(QStringLiteral("rcrash.apfs"));
    {
        QFile f(crashed);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(crashedBytes);
    }
    const auto listing =
        PartitionApfsFileSystemReader::listDirectoryFromImage(crashed, QStringLiteral("/"), 64);
    QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));
    QStringList names;
    for (const auto& entry : listing.entries) {
        names.append(entry.name);
    }
    names.sort();
    // Rolled back: the interrupted commit's charlie.txt is gone, the previous
    // checkpoint's two files survive intact.
    QCOMPARE(names, (QStringList{QStringLiteral("alpha.txt"), QStringLiteral("bravo.txt")}));
}

void PartitionManagerCoreTests::fileSystemRegistry_reportsNativeAndNonNativeCapability() {
    verifyNativeRegistryCapabilities();
    verifyExtRegistryCapability();
    verifyMetadataOnlyRegistryCapabilities();
    verifyApfsRegistryCapability();
    verifyHfsRegistryCapability();
    verifySwapAndUnknownRegistryCapability();
}

void PartitionManagerCoreTests::fileSystemToolManifest_validatesPinnedTool() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir root(temp.path());
    QVERIFY(root.mkpath(QStringLiteral("e2fsprogs")));
    const QByteArray binaryBytes("fake-e2fsck-binary");
    const QByteArray runtimeBytes("fake-runtime-dll");
    const QString binaryPath = root.filePath(QStringLiteral("e2fsprogs/e2fsck.exe"));
    const QString runtimePath = root.filePath(QStringLiteral("e2fsprogs/cygfake1.dll"));
    QVERIFY(writeBytes(binaryPath, binaryBytes));
    QVERIFY(writeBytes(runtimePath, runtimeBytes));
    const QString binaryHash = sha256Hex(binaryBytes);
    const QString runtimeHash = sha256Hex(runtimeBytes);
    const QString sourceHash = sha256Hex(QByteArray("fake-source-archive"));
    const QByteArray manifest = approvedToolManifestJson(sourceHash, binaryHash, runtimeHash);

    const auto result = PartitionFileSystemToolManifest::validateManifestJson(manifest,
                                                                              root.path());
    QVERIFY2(result.ok, qPrintable(result.errors.join(QStringLiteral("; "))));
    QCOMPARE(result.tools.size(), 1);
    QCOMPARE(result.tools.first().id, QStringLiteral("e2fsck"));
    QCOMPARE(result.tools.first().runtime_files.size(), 1);
    QCOMPARE(result.tools.first().runtime_files.first().relative_path,
             QStringLiteral("e2fsprogs/cygfake1.dll"));

    const QString manifestPath = root.filePath(QStringLiteral("manifest.json"));
    QVERIFY(writeBytes(manifestPath, manifest));
    const auto required =
        PartitionFileSystemToolManifest::validateRequiredTool(manifestPath, root.path(), "e2fsck");
    QVERIFY2(required.ok, qPrintable(required.errors.join(QStringLiteral("; "))));
    const auto missing =
        PartitionFileSystemToolManifest::validateRequiredTool(manifestPath, root.path(), "mke2fs");
    QVERIFY(!missing.ok);
    QVERIFY(missing.errors.join(' ').contains(QStringLiteral("not manifest-approved")));
}

void PartitionManagerCoreTests::
    fileSystemToolManifest_blocksMissingMetadataHashMismatchAndPathTraversal() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir root(temp.path());
    QVERIFY(writeBytes(root.filePath(QStringLiteral("bad.exe")), QByteArray("bad-binary")));
    const QString validHash = sha256Hex(QByteArray("expected"));
    const QByteArray manifest = invalidToolManifestJson(validHash);
    const auto result = PartitionFileSystemToolManifest::validateManifestJson(manifest,
                                                                              root.path());
    QVERIFY(!result.ok);
    const QString errors = result.errors.join(' ');
    QVERIFY(errors.contains(QStringLiteral("license")));
    QVERIFY(errors.contains(QStringLiteral("source_archive_sha256")));
    QVERIFY(errors.contains(QStringLiteral("binary hash mismatch")));
    QVERIFY(errors.contains(QStringLiteral("runtime file sha256")));
    QVERIFY(
        errors.contains(QStringLiteral("runtime file relative_path must stay under tools root")));
    QVERIFY(errors.contains(QStringLiteral("relative_path must stay under tools root")));
}

void PartitionManagerCoreTests::fileSystemToolRunner_buildsReadOnlyCheckCommands() {
    const auto ext4 = PartitionFileSystemToolRunner::buildReadOnlyCheckCommand(
        QStringLiteral("ext4"), QStringLiteral("target.img"));
    QVERIFY2(ext4.ok(), qPrintable(ext4.blockers.join(QStringLiteral("; "))));
    QCOMPARE(ext4.tool_id, QStringLiteral("e2fsck"));
    QCOMPARE(ext4.operation, PartitionFileSystemToolRunner::readOnlyCheckOperation());
    QCOMPARE(ext4.arguments,
             QStringList(
                 {QStringLiteral("-n"), QStringLiteral("-f"), QStringLiteral("target.img")}));

    const auto xfs = PartitionFileSystemToolRunner::buildReadOnlyCheckCommand(
        QStringLiteral("XFS"), QStringLiteral("target.img"));
    QVERIFY2(xfs.ok(), qPrintable(xfs.blockers.join(QStringLiteral("; "))));
    QCOMPARE(xfs.tool_id, QStringLiteral("xfs_repair"));
    QCOMPARE(xfs.arguments, QStringList({QStringLiteral("-n"), QStringLiteral("target.img")}));

    const auto btrfs = PartitionFileSystemToolRunner::buildReadOnlyCheckCommand(
        QStringLiteral("btrfs"), QStringLiteral("target.img"));
    QVERIFY2(btrfs.ok(), qPrintable(btrfs.blockers.join(QStringLiteral("; "))));
    QCOMPARE(btrfs.tool_id, QStringLiteral("btrfs"));
    QCOMPARE(btrfs.arguments,
             QStringList({QStringLiteral("check"),
                          QStringLiteral("--readonly"),
                          QStringLiteral("target.img")}));

    const auto hfs = PartitionFileSystemToolRunner::buildReadOnlyCheckCommand(
        QStringLiteral("hfs+"), QStringLiteral("target.img"));
    QVERIFY2(hfs.ok(), qPrintable(hfs.blockers.join(QStringLiteral("; "))));
    QCOMPARE(hfs.tool_id, QStringLiteral("fsck_hfs"));
    QCOMPARE(hfs.file_system, QStringLiteral("hfs+"));
    QCOMPARE(hfs.arguments,
             QStringList(
                 {QStringLiteral("-n"), QStringLiteral("-f"), QStringLiteral("target.img")}));
}

void PartitionManagerCoreTests::
    fileSystemToolRunner_buildsExtWriteCommandShapesWithConfirmationGate() {
    verifyExtFormatCommandShape();
    verifyExtRepairAndResizeCommandShapes();
    verifyUnconfirmedExtFormatBlocked();
    verifyHfsCommandShapes();
    verifyHfsRawTargetConversion();
    verifyUnsupportedRepairBlocked();
}

void PartitionManagerCoreTests::fileSystemToolRunner_blocksUnapprovedOrUnsupportedReadOnlyChecks() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir root(temp.path());
    const QString manifestPath = root.filePath(QStringLiteral("manifest.json"));
    QVERIFY(writeBytes(manifestPath, emptyToolManifestJson()));

    PartitionFileSystemReadOnlyCheckRequest request{.manifest_path = manifestPath,
                                                    .tools_root = root.path(),
                                                    .file_system = QStringLiteral("ext4"),
                                                    .target_path = QStringLiteral("target.img")};
    auto result = PartitionFileSystemToolRunner::runReadOnlyCheck(request);
    QVERIFY(result.blocked);
    QVERIFY(!result.success);
    QVERIFY(result.blockers.join(' ').contains(QStringLiteral("not manifest-approved")));

    const QByteArray fakeBinary("fake-e2fsck");
    QVERIFY(writeBytes(root.filePath(QStringLiteral("e2fsck.exe")), fakeBinary));
    QVERIFY(writeBytes(manifestPath,
                       approvedToolManifest(QStringLiteral("e2fsck"),
                                            QStringLiteral("e2fsck.exe"),
                                            fakeBinary,
                                            {QStringLiteral("ext4")},
                                            {QStringLiteral("repair")})));
    result = PartitionFileSystemToolRunner::runReadOnlyCheck(request);
    QVERIFY(result.blocked);
    QVERIFY(result.blockers.join(' ').contains(QStringLiteral("does not approve operation")));
}

void PartitionManagerCoreTests::fileSystemToolRunner_resolvesApprovedToolPath() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QDir root(temp.path());
    const QByteArray fakeBinary("fake-e2fsck");
    const QString relativePath = QStringLiteral("tools/e2fsck.exe");
    QVERIFY(root.mkpath(QStringLiteral("tools")));
    QVERIFY(writeBytes(root.filePath(relativePath), fakeBinary));
    const QString manifestPath = root.filePath(QStringLiteral("manifest.json"));
    QVERIFY(writeBytes(
        manifestPath,
        approvedToolManifest(QStringLiteral("e2fsck"),
                             relativePath,
                             fakeBinary,
                             {QStringLiteral("ext4")},
                             {PartitionFileSystemToolRunner::readOnlyCheckOperation()})));

    const auto resolution = PartitionFileSystemToolRunner::resolveApprovedTool(
        manifestPath,
        root.path(),
        QStringLiteral("e2fsck"),
        PartitionFileSystemToolRunner::readOnlyCheckOperation(),
        QStringLiteral("ext4"));
    QVERIFY2(resolution.ok, qPrintable(resolution.blockers.join(QStringLiteral("; "))));
    QCOMPARE(QFileInfo(resolution.tool_path).fileName(), QStringLiteral("e2fsck.exe"));
    QCOMPARE(resolution.tool.id, QStringLiteral("e2fsck"));
}

void PartitionManagerCoreTests::inventoryParser_parsesDiskAndPartition() {
    const auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    QCOMPARE(inventory.disks.size(), 1);
    QCOMPARE(inventory.disks.first().disk_number, 0u);
    QCOMPARE(inventory.disks.first().partitions.size(), 2);
    QVERIFY(inventory.disks.first().partitions.first().is_efi);
    QVERIFY(inventory.disks.first().partitions.first().volume.has_value());
    QCOMPARE(inventory.disks.first().partitions.first().volume->file_system,
             QStringLiteral("FAT32"));
    QCOMPARE(inventory.disks.first().partitions.first().volume->file_system_source,
             PartitionFileSystemDetector::inferredProtectedSource());
    QCOMPARE(inventory.disks.first().operational_status, QStringLiteral("Online"));
    QCOMPARE(inventory.disks.first().temperature_celsius, 38);
    QCOMPARE(inventory.disks.first().power_on_hours, 1234ULL);
    QCOMPARE(inventory.disks.first().read_errors_total, 2ULL);
    QCOMPARE(inventory.disks.first().write_errors_total, 3ULL);
    QCOMPARE(inventory.disks.first().wear_percent, 4ULL);
    QVERIFY(inventory.disks.first().partitions.at(1).volume->bitlocker_enabled);
    QVERIFY(inventory.disks.first().partitions.at(1).volume->dirty_bit_set);
    QCOMPARE(inventory.disks.first().partitions.at(1).volume->file_system_source,
             PartitionFileSystemDetector::windowsVolumeSource());
    QVERIFY(!inventory.layout_hash.isEmpty());
}

void PartitionManagerCoreTests::inventoryParser_infersHiddenProtectedPartitionFileSystems() {
    const auto inventory = StorageInventoryWorker::parseInventoryJson(hiddenProtectedFixtureJson());
    QCOMPARE(inventory.disks.size(), 1);
    const auto& partitions = inventory.disks.first().partitions;
    QCOMPARE(partitions.size(), 2);
    QVERIFY(partitions.at(0).is_efi);
    QVERIFY(partitions.at(0).volume.has_value());
    QCOMPARE(partitions.at(0).volume->file_system, QStringLiteral("FAT32"));
    QCOMPARE(partitions.at(0).volume->file_system_source,
             PartitionFileSystemDetector::inferredProtectedSource());
    QCOMPARE(partitions.at(0).volume->total_bytes, 0ULL);
    QVERIFY(partitions.at(1).is_msr);
    QVERIFY(partitions.at(1).volume.has_value());
    QCOMPARE(partitions.at(1).volume->file_system, QStringLiteral("Other"));
    QCOMPARE(partitions.at(1).volume->file_system_source,
             PartitionFileSystemDetector::inferredProtectedSource());
    QCOMPARE(partitions.at(1).volume->total_bytes, 0ULL);
}

void PartitionManagerCoreTests::inventoryParser_keepsRawBasicDiskInitializable() {
    const auto inventory = StorageInventoryWorker::parseInventoryJson(rawBasicDiskFixtureJson());
    QCOMPARE(inventory.disks.size(), 1);
    const auto& disk = inventory.disks.first();
    QCOMPARE(disk.disk_number, 2u);
    QCOMPARE(disk.partition_style, QStringLiteral("RAW"));
    QVERIFY2(!disk.is_dynamic, "RAW partition style is not a dynamic-disk signal.");
    QCOMPARE(disk.partitions.size(), 0);
    QCOMPARE(disk.unallocated_regions.size(), 1);
    QCOMPARE(disk.unallocated_regions.first().offset_bytes, 0ULL);
    QCOMPARE(disk.unallocated_regions.first().size_bytes, disk.size_bytes);

    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = disk.disk_number;
    target.size_bytes = disk.size_bytes;
    const auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::InitializeDisk, target);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));
}

void PartitionManagerCoreTests::inventoryScript_handlesRawDisksWithoutAbort() {
    const QString script = StorageInventoryWorker::inventoryPowerShellScript();
    QVERIFY2(script.contains(QStringLiteral("$ProgressPreference = 'SilentlyContinue'")),
             "Inventory script should suppress progress records so stdout remains JSON-only.");
    QVERIFY2(script.contains(QStringLiteral(
                 "Get-Partition -DiskNumber $disk.Number -ErrorAction SilentlyContinue")),
             "RAW disks with no partitions must not abort the whole inventory scan.");
    QVERIFY2(script.contains(QStringLiteral("Get-SakVolumeForPartition")),
             "Hidden partitions should use a resilient volume lookup helper.");
    QVERIFY2(script.contains(QStringLiteral("Get-Volume -Path $accessPath")),
             "Volume GUID access paths should be tried when direct partition lookup fails.");
}

void PartitionManagerCoreTests::safetyValidator_blocksSystemPartitionDelete() {
    const auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 0;
    target.partition_number = 2;
    target.size_bytes = inventory.disks.first().partitions.at(1).size_bytes;
    auto operation = PartitionOperationPlanner::makeOperation(PartitionOperationType::Delete,
                                                              target);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("protected")));
}

void PartitionManagerCoreTests::scriptBuilder_createRespectsWizardPayload() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Unallocated;
    target.disk_number = 5;
    target.offset_bytes = 1'048'576;
    target.size_bytes = 64 * 1024 * 1024;
    QJsonObject payload;
    payload[QStringLiteral("size_bytes")] = QStringLiteral("33554432");
    payload[QStringLiteral("relative_offset_bytes")] = QStringLiteral("2097152");
    payload[QStringLiteral("file_system")] = QStringLiteral("exFAT");
    payload[QStringLiteral("label")] = QStringLiteral("Field Media");
    payload[QStringLiteral("drive_letter")] = QStringLiteral("M");
    payload[QStringLiteral("full_format")] = true;
    payload[QStringLiteral("allocation_unit_bytes")] = QStringLiteral("4096");
    payload[QStringLiteral("gpt_type")] = QStringLiteral("{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}");
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Create, target, payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("New-Partition -DiskNumber 5")));
    QVERIFY(script.script.contains(QStringLiteral("-DriveLetter M")));
    QVERIFY(script.script.contains(QStringLiteral("-Offset 3145728")));
    QVERIFY(script.script.contains(
        QStringLiteral("-GptType '{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}'")));
    QVERIFY(
        script.script.contains(QStringLiteral("Format-Volume -Partition $p -FileSystem EXFAT")));
    QVERIFY(script.script.contains(
        QStringLiteral("-NewFileSystemLabel 'Field Media' -Full -AllocationUnitSize 4096")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsNonNativeCreateFormatScripts() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Unallocated;
    target.disk_number = 2;
    target.offset_bytes = 1'048'576;
    target.size_bytes = 256ULL * 1024ULL * 1024ULL;

    QJsonObject payload;
    payload[QStringLiteral("size_bytes")] = QStringLiteral("134217728");
    payload[QStringLiteral("relative_offset_bytes")] = QStringLiteral("1048576");
    payload[QStringLiteral("non_native_file_system_tool")] = true;
    payload[QStringLiteral("file_system")] = QStringLiteral("ext4");
    payload[QStringLiteral("label")] = QStringLiteral("SAK_EXT4");
    payload[QStringLiteral("target_wipe_confirmed")] = true;

    PartitionScriptBuilder builder;
    const auto extScript = builder.buildScript(
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Create, target, payload));
    QVERIFY2(extScript.valid(), qPrintable(extScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(extScript.script.contains(QStringLiteral("New-Partition -DiskNumber 2")));
    QVERIFY(extScript.script.contains(QStringLiteral("-Offset 2097152")));
    QVERIFY(extScript.script.contains(QStringLiteral("$rawTargetPath")));
    QVERIFY(extScript.script.contains(QStringLiteral("mke2fs")));
    QVERIFY(extScript.script.contains(QStringLiteral("'ext4'")));
    QVERIFY(!extScript.script.contains(QStringLiteral("-AssignDriveLetter")));
    QVERIFY(!extScript.script.contains(QStringLiteral("Format-Volume")));
    QVERIFY(!extScript.script.contains(QStringLiteral("__SAK_NEW_PARTITION_RAW_TARGET__")));

    QJsonObject hfsPayload = payload;
    hfsPayload[QStringLiteral("file_system")] = QStringLiteral("HFSX");
    hfsPayload[QStringLiteral("label")] = QStringLiteral("SAK_HFSX");
    const auto hfsScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::Create, target, hfsPayload));
    QVERIFY2(hfsScript.valid(), qPrintable(hfsScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(hfsScript.script.contains(QStringLiteral("newfs_hfs")));
    QVERIFY(hfsScript.script.contains(QStringLiteral("fsck_hfs")));
    QVERIFY(hfsScript.script.contains(QStringLiteral("Copy-SakSparseImageToRawTarget")));
    QVERIFY(hfsScript.script.contains(QStringLiteral("$stagedImagePath")));
    QVERIFY(!hfsScript.script.contains(QStringLiteral("Format-Volume")));

    QJsonObject swapPayload = payload;
    swapPayload[QStringLiteral("file_system")] = QStringLiteral("Linux swap");
    swapPayload[QStringLiteral("label")] = QStringLiteral("SAK_SWAP");
    swapPayload[QStringLiteral("linux_swap_page_size_bytes")] = QStringLiteral("4096");
    const auto swapScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::Create, target, swapPayload));
    QVERIFY2(swapScript.valid(), qPrintable(swapScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(swapScript.script.contains(QStringLiteral("SWAPSPACE2")));
    QVERIFY(swapScript.script.contains(QStringLiteral("$rawTargetPath")));
    QVERIFY(!swapScript.script.contains(QStringLiteral("Format-Volume")));

    QJsonObject apfsPayload = payload;
    apfsPayload[QStringLiteral("file_system")] = QStringLiteral("APFS");
    apfsPayload[QStringLiteral("label")] = QStringLiteral("SAK_APFS");
    apfsPayload[QStringLiteral("gpt_type")] =
        QStringLiteral("{7C3457EF-0000-11AA-AA11-00306543ECAC}");
    const auto apfsScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::Create, target, apfsPayload));
    QVERIFY2(apfsScript.valid(), qPrintable(apfsScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(apfsScript.script.contains(QStringLiteral("sak_apfs_writer_cli.exe")));
    QVERIFY(apfsScript.script.contains(QStringLiteral("format-raw")));
    QVERIFY(apfsScript.script.contains(QStringLiteral("ui.apfs-generated-raw-create-format")));
    QVERIFY(!apfsScript.script.contains(QStringLiteral("Format-Volume")));

    QJsonObject unconfirmedPayload = payload;
    unconfirmedPayload[QStringLiteral("target_wipe_confirmed")] = false;
    const auto blockedScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::Create, target, unconfirmedPayload));
    QVERIFY(!blockedScript.valid());
    QVERIFY(blockedScript.blockers.join(' ').contains(QStringLiteral("confirmation")));
}

void PartitionManagerCoreTests::scriptBuilder_rejectsInvalidCreatePartitionType() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Unallocated;
    target.disk_number = 5;
    target.size_bytes = 64 * 1024 * 1024;
    QJsonObject payload;
    payload[QStringLiteral("size_bytes")] = QStringLiteral("33554432");
    payload[QStringLiteral("file_system")] = QStringLiteral("NTFS");
    payload[QStringLiteral("mbr_type")] = QStringLiteral("FAT32");
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Create, target, payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(!script.valid());
    QVERIFY(script.blockers.join(' ').contains(QStringLiteral("incompatible")));
}

void PartitionManagerCoreTests::scriptBuilder_rejectsUnsupportedAllocationUnit() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 1;
    target.partition_number = 3;
    target.size_bytes = 10 * 1024 * 1024;
    QJsonObject payload;
    payload[QStringLiteral("file_system")] = QStringLiteral("NTFS");
    payload[QStringLiteral("label")] = QStringLiteral("Data");
    payload[QStringLiteral("allocation_unit_bytes")] = QStringLiteral("12345");
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Format, target, payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(!script.valid());
    QVERIFY(script.blockers.join(' ').contains(QStringLiteral("allocation unit")));
}

void PartitionManagerCoreTests::scriptBuilder_rejectsNativeFormatForNonWindowsFilesystem() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 1;
    target.partition_number = 3;
    target.size_bytes = 10 * 1024 * 1024;
    QJsonObject payload;
    payload[QStringLiteral("file_system")] = QStringLiteral("ext4");
    payload[QStringLiteral("label")] = QStringLiteral("LinuxData");
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Format, target, payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(!script.valid());
    QVERIFY(script.blockers.join(' ').contains(QStringLiteral("Unsupported file system")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsExtToolScriptsWithManifestGate() {
    const PartitionTarget target = extToolPartitionTarget();
    PartitionScriptBuilder builder;
    verifyExtFormatScript(&builder, target);
    verifyHfsFormatAndRepairScripts(&builder, target);
    verifyApfsFormatAndRepairScripts(&builder, target);
    verifyExtRepairScript(&builder, target);
    verifyExtResizeScripts(&builder, target);
    verifyExtTargetPathGates(&builder, target);
}

void PartitionManagerCoreTests::scriptBuilder_buildsApfsRootFileMutationScripts() {
    PartitionTarget target = extToolPartitionTarget();
    target.size_bytes = 128ULL * 1024ULL * 1024ULL;
    PartitionScriptBuilder builder;

    const auto writeScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ApfsWriteRootFile, target, baseApfsRootFileMutationPayload()));
    QVERIFY2(writeScript.valid(), qPrintable(writeScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(writeScript.script.contains(QStringLiteral("sak_apfs_writer_cli.exe")));
    QVERIFY(writeScript.script.contains(QStringLiteral("commit-raw-file-write")));
    QVERIFY(writeScript.script.contains(QStringLiteral("--payload-file")));
    QVERIFY(writeScript.script.contains(QStringLiteral("FromBase64String")));
    QVERIFY(writeScript.script.contains(QStringLiteral("ui.apfs-generated-raw-root-file-write")));
    QVERIFY(writeScript.script.contains(extToolRawTargetPath()));

    const auto patchScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ApfsPatchRootFile, target, baseApfsRootFileMutationPayload()));
    QVERIFY2(patchScript.valid(), qPrintable(patchScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(patchScript.script.contains(QStringLiteral("commit-raw-file-patch")));
    QVERIFY(patchScript.script.contains(QStringLiteral("-PatchOffsetBytes 6")));
    QVERIFY(patchScript.script.contains(QStringLiteral("ui.apfs-generated-raw-root-file-patch")));

    QJsonObject deletePayload = baseApfsRootFileMutationPayload();
    deletePayload.remove(QStringLiteral("apfs_root_file_payload_text"));
    deletePayload.remove(QStringLiteral("apfs_root_file_patch_offset_bytes"));
    const auto deleteScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ApfsDeleteRootFile, target, deletePayload));
    QVERIFY2(deleteScript.valid(), qPrintable(deleteScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(deleteScript.script.contains(QStringLiteral("commit-raw-file-delete")));
    QVERIFY(!deleteScript.script.contains(QStringLiteral("FromBase64String")));
    QVERIFY(deleteScript.script.contains(QStringLiteral("ui.apfs-generated-raw-root-file-delete")));

    QJsonObject directoryFilePayload = baseApfsRootFileMutationPayload();
    directoryFilePayload[QStringLiteral("apfs_root_directory_name")] =
        QStringLiteral("Proof Folder");
    const auto directoryFileWriteScript =
        builder.buildScript(PartitionOperationPlanner::makeOperation(
            PartitionOperationType::ApfsWriteRootDirectoryFile, target, directoryFilePayload));
    QVERIFY2(directoryFileWriteScript.valid(),
             qPrintable(directoryFileWriteScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(directoryFileWriteScript.script.contains(
        QStringLiteral("commit-raw-directory-child-write")));
    QVERIFY(directoryFileWriteScript.script.contains(QStringLiteral("-DirectoryName")));
    QVERIFY(directoryFileWriteScript.script.contains(QStringLiteral("-FileName")));
    QVERIFY(directoryFileWriteScript.script.contains(QStringLiteral("-PayloadFile $payloadPath")));
    QVERIFY(directoryFileWriteScript.script.contains(
        QStringLiteral("ui.apfs-generated-raw-root-directory-file-write")));

    const auto directoryFilePatchScript =
        builder.buildScript(PartitionOperationPlanner::makeOperation(
            PartitionOperationType::ApfsPatchRootDirectoryFile, target, directoryFilePayload));
    QVERIFY2(directoryFilePatchScript.valid(),
             qPrintable(directoryFilePatchScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(directoryFilePatchScript.script.contains(QStringLiteral("commit-raw-file-patch")));
    QVERIFY(directoryFilePatchScript.script.contains(QStringLiteral("-DirectoryName")));
    QVERIFY(directoryFilePatchScript.script.contains(QStringLiteral("-FileName")));
    QVERIFY(directoryFilePatchScript.script.contains(QStringLiteral("-PayloadFile $payloadPath")));
    QVERIFY(directoryFilePatchScript.script.contains(QStringLiteral("-PatchOffsetBytes 6")));
    QVERIFY(directoryFilePatchScript.script.contains(
        QStringLiteral("ui.apfs-generated-raw-root-directory-file-patch")));

    QJsonObject directoryFileDeletePayload = directoryFilePayload;
    directoryFileDeletePayload.remove(QStringLiteral("apfs_root_file_payload_text"));
    directoryFileDeletePayload.remove(QStringLiteral("apfs_root_file_patch_offset_bytes"));
    const auto directoryFileDeleteScript =
        builder.buildScript(PartitionOperationPlanner::makeOperation(
            PartitionOperationType::ApfsDeleteRootDirectoryFile,
            target,
            directoryFileDeletePayload));
    QVERIFY2(directoryFileDeleteScript.valid(),
             qPrintable(directoryFileDeleteScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(directoryFileDeleteScript.script.contains(
        QStringLiteral("commit-raw-directory-child-delete")));
    QVERIFY(directoryFileDeleteScript.script.contains(QStringLiteral("-DirectoryName")));
    QVERIFY(directoryFileDeleteScript.script.contains(QStringLiteral("-FileName")));
    QVERIFY(!directoryFileDeleteScript.script.contains(QStringLiteral("FromBase64String")));
    QVERIFY(directoryFileDeleteScript.script.contains(
        QStringLiteral("ui.apfs-generated-raw-root-directory-file-delete")));

    QJsonObject createDirectoryPayload = deletePayload;
    createDirectoryPayload[QStringLiteral("apfs_root_directory_name")] =
        QStringLiteral("Proof Folder");
    createDirectoryPayload.remove(QStringLiteral("apfs_root_file_name"));
    const auto createDirectoryScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ApfsCreateRootDirectory, target, createDirectoryPayload));
    QVERIFY2(createDirectoryScript.valid(),
             qPrintable(createDirectoryScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(createDirectoryScript.script.contains(QStringLiteral("commit-raw-directory-create")));
    QVERIFY(createDirectoryScript.script.contains(QStringLiteral("-DirectoryName")));
    QVERIFY(createDirectoryScript.script.contains(
        QStringLiteral("ui.apfs-generated-raw-root-directory-create")));
    QVERIFY(!createDirectoryScript.script.contains(QStringLiteral("FromBase64String")));

    const auto deleteDirectoryScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ApfsDeleteRootDirectory, target, createDirectoryPayload));
    QVERIFY2(deleteDirectoryScript.valid(),
             qPrintable(deleteDirectoryScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(deleteDirectoryScript.script.contains(QStringLiteral("commit-raw-directory-delete")));
    QVERIFY(deleteDirectoryScript.script.contains(
        QStringLiteral("ui.apfs-generated-raw-root-directory-delete")));

    QJsonObject labelPayload = deletePayload;
    labelPayload.remove(QStringLiteral("apfs_root_file_name"));
    labelPayload[QStringLiteral("label")] = QStringLiteral("SAK Relabeled");
    const auto labelScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ApfsChangeVolumeLabel, target, labelPayload));
    QVERIFY2(labelScript.valid(), qPrintable(labelScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(labelScript.script.contains(QStringLiteral("change-raw-volume-label")));
    QVERIFY(labelScript.script.contains(QStringLiteral("-VolumeName 'SAK Relabeled'")));
    QVERIFY(
        labelScript.script.contains(QStringLiteral("ui.apfs-generated-raw-volume-label-change")));
    QVERIFY(!labelScript.script.contains(QStringLiteral("FromBase64String")));
    QVERIFY(!labelScript.script.contains(QStringLiteral("-FileName")));

    QJsonObject missingLayout = baseApfsRootFileMutationPayload();
    missingLayout[QStringLiteral("apfs_generated_layout_confirmed")] = false;
    const auto blocked = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ApfsWriteRootFile, target, missingLayout));
    QVERIFY(!blocked.valid());
    QVERIFY(blocked.blockers.join(' ').contains(QStringLiteral("generated-layout")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsHfsFileMutationScripts() {
    const PartitionTarget target = extToolPartitionTarget();
    PartitionScriptBuilder builder;

    const auto replaceScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::HfsReplaceFile, target, baseHfsFileMutationPayload()));
    QVERIFY2(replaceScript.valid(), qPrintable(replaceScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(replaceScript.script.contains(QStringLiteral("sak_hfs_writer_cli.exe")));
    QVERIFY(replaceScript.script.contains(QStringLiteral("replace-image")));
    QVERIFY(replaceScript.script.contains(QStringLiteral("Copy-SakRawTargetToImage")));
    QVERIFY(replaceScript.script.contains(QStringLiteral("Get-SakHfsVolumeStagingSizeBytes")));
    QVERIFY(replaceScript.script.contains(
        QStringLiteral("$targetPartitionSizeBytes = [uint64]$p.Size")));
    QVERIFY(replaceScript.script.contains(QStringLiteral("-SizeBytes $targetSizeBytes")));
    QVERIFY(!replaceScript.script.contains(QStringLiteral("-SizeBytes $targetPartitionSizeBytes")));
    QVERIFY(replaceScript.script.contains(QStringLiteral("Invoke-SakHfsRepairUntilClean")));
    QVERIFY(replaceScript.script.contains(QStringLiteral("Copy-SakSparseImageToRawTarget")));
    QVERIFY(replaceScript.script.contains(QStringLiteral("--allow-journaled-volume")));
    QVERIFY(replaceScript.script.contains(QStringLiteral("--allow-wrapped-volume")));
    QVERIFY(replaceScript.script.contains(QStringLiteral("-AllowJournaledIncomplete")));
    QVERIFY(replaceScript.script.contains(extToolRawTargetPath()));

    const auto growScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::HfsGrowFile, target, baseHfsFileMutationPayload()));
    QVERIFY2(growScript.valid(), qPrintable(growScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(growScript.script.contains(QStringLiteral("grow-image")));
    QVERIFY(growScript.script.contains(QStringLiteral("ui.hfs.raw-file-allocation-growth")));

    const auto overwriteScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::HfsOverwriteFile, target, baseHfsFileMutationPayload()));
    QVERIFY2(overwriteScript.valid(),
             qPrintable(overwriteScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(overwriteScript.script.contains(QStringLiteral("overwrite-image")));

    QJsonObject truncatePayload = baseHfsFileMutationPayload();
    truncatePayload.remove(QStringLiteral("hfs_payload_text"));
    const auto truncateScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::HfsTruncateFile, target, truncatePayload));
    QVERIFY2(truncateScript.valid(),
             qPrintable(truncateScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(truncateScript.script.contains(QStringLiteral("truncate-image")));
    QVERIFY(!truncateScript.script.contains(QStringLiteral("FromBase64String")));

    const auto resourceScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::HfsReplaceResourceFork, target, baseHfsFileMutationPayload()));
    QVERIFY2(resourceScript.valid(),
             qPrintable(resourceScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(resourceScript.script.contains(QStringLiteral("replace-resource-fork-image")));

    const auto resourceGrowScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::HfsGrowResourceFork, target, baseHfsFileMutationPayload()));
    QVERIFY2(resourceGrowScript.valid(),
             qPrintable(resourceGrowScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(resourceGrowScript.script.contains(QStringLiteral("grow-resource-fork-image")));
    QVERIFY(resourceGrowScript.script.contains(
        QStringLiteral("ui.hfs.raw-resource-fork-allocation-growth")));

    QJsonObject createPayload = baseHfsFileMutationPayload();
    createPayload[QStringLiteral("hfs_path")] = QStringLiteral("/created.txt");
    createPayload.remove(QStringLiteral("hfs_payload_text"));
    const auto createScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::HfsCreateEmptyFile, target, createPayload));
    QVERIFY2(createScript.valid(), qPrintable(createScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(createScript.script.contains(QStringLiteral("create-empty-file-image")));
    QVERIFY(createScript.script.contains(QStringLiteral("ui.hfs.raw-empty-file-create")));
    QVERIFY(!createScript.script.contains(QStringLiteral("FromBase64String")));

    QJsonObject createFilePayload = createPayload;
    createFilePayload[QStringLiteral("hfs_payload_text")] = QStringLiteral("created payload");
    const auto createFileScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::HfsCreateFile, target, createFilePayload));
    QVERIFY2(createFileScript.valid(),
             qPrintable(createFileScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(createFileScript.script.contains(QStringLiteral("create-file-image")));
    QVERIFY(createFileScript.script.contains(QStringLiteral("ui.hfs.raw-file-create")));
    QVERIFY(createFileScript.script.contains(QStringLiteral("FromBase64String")));
    QVERIFY(createFileScript.script.contains(QStringLiteral("-PayloadFile $payloadPath")));

    const auto deleteScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::HfsDeleteEmptyFile, target, createPayload));
    QVERIFY2(deleteScript.valid(), qPrintable(deleteScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(deleteScript.script.contains(QStringLiteral("delete-empty-file-image")));
    QVERIFY(deleteScript.script.contains(QStringLiteral("ui.hfs.raw-empty-file-delete")));
    QVERIFY(!deleteScript.script.contains(QStringLiteral("FromBase64String")));

    const auto deleteAllocatedScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::HfsDeleteFile, target, createPayload));
    QVERIFY2(deleteAllocatedScript.valid(),
             qPrintable(deleteAllocatedScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(deleteAllocatedScript.script.contains(QStringLiteral("delete-file-image")));
    QVERIFY(deleteAllocatedScript.script.contains(QStringLiteral("ui.hfs.raw-file-delete")));
    QVERIFY(!deleteAllocatedScript.script.contains(QStringLiteral("FromBase64String")));

    QJsonObject secureDeletePayload = createPayload;
    secureDeletePayload[QStringLiteral("hfs_secure_wipe_released_blocks")] = true;
    const auto secureDeleteScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::HfsDeleteFile, target, secureDeletePayload));
    QVERIFY2(secureDeleteScript.valid(),
             qPrintable(secureDeleteScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(secureDeleteScript.script.contains(QStringLiteral("-SecureWipeReleasedBlocks $true")));

    QJsonObject unsupportedSecurePayload = baseHfsFileMutationPayload();
    unsupportedSecurePayload[QStringLiteral("hfs_secure_wipe_released_blocks")] = true;
    const auto unsupportedSecureScript =
        builder.buildScript(PartitionOperationPlanner::makeOperation(
            PartitionOperationType::HfsReplaceFile, target, unsupportedSecurePayload));
    QVERIFY(!unsupportedSecureScript.valid());
    QVERIFY(
        unsupportedSecureScript.blockers.join(' ').contains(QStringLiteral("secure block wipe")));

    QJsonObject folderPayload = createPayload;
    folderPayload[QStringLiteral("hfs_path")] = QStringLiteral("/Created Folder");
    const auto createFolderScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::HfsCreateEmptyFolder, target, folderPayload));
    QVERIFY2(createFolderScript.valid(),
             qPrintable(createFolderScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(createFolderScript.script.contains(QStringLiteral("create-empty-folder-image")));
    QVERIFY(createFolderScript.script.contains(QStringLiteral("ui.hfs.raw-empty-folder-create")));
    QVERIFY(!createFolderScript.script.contains(QStringLiteral("FromBase64String")));

    const auto deleteFolderScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::HfsDeleteEmptyFolder, target, folderPayload));
    QVERIFY2(deleteFolderScript.valid(),
             qPrintable(deleteFolderScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(deleteFolderScript.script.contains(QStringLiteral("delete-empty-folder-image")));
    QVERIFY(deleteFolderScript.script.contains(QStringLiteral("ui.hfs.raw-empty-folder-delete")));
    QVERIFY(!deleteFolderScript.script.contains(QStringLiteral("FromBase64String")));

    const auto deleteFolderTreeScript =
        builder.buildScript(PartitionOperationPlanner::makeOperation(
            PartitionOperationType::HfsDeleteFolderTree, target, folderPayload));
    QVERIFY2(deleteFolderTreeScript.valid(),
             qPrintable(deleteFolderTreeScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(deleteFolderTreeScript.script.contains(QStringLiteral("delete-folder-tree-image")));
    QVERIFY(
        deleteFolderTreeScript.script.contains(QStringLiteral("ui.hfs.raw-folder-tree-delete")));
    QVERIFY(!deleteFolderTreeScript.script.contains(QStringLiteral("FromBase64String")));

    QJsonObject renamePayload = folderPayload;
    renamePayload[QStringLiteral("hfs_destination_path")] = QStringLiteral("/Renamed Folder");
    const auto renameMoveScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::HfsRenameMoveCatalogEntry, target, renamePayload));
    QVERIFY2(renameMoveScript.valid(),
             qPrintable(renameMoveScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(renameMoveScript.script.contains(QStringLiteral("rename-catalog-entry-image")));
    QVERIFY(renameMoveScript.script.contains(QStringLiteral("-DestinationHfsPath")));
    QVERIFY(renameMoveScript.script.contains(QStringLiteral("ui.hfs.raw-catalog-rename-move")));
    QVERIFY(!renameMoveScript.script.contains(QStringLiteral("FromBase64String")));

    QJsonObject secureFolderPayload = folderPayload;
    secureFolderPayload[QStringLiteral("hfs_secure_wipe_released_blocks")] = true;
    const auto secureFolderTreeScript =
        builder.buildScript(PartitionOperationPlanner::makeOperation(
            PartitionOperationType::HfsDeleteFolderTree, target, secureFolderPayload));
    QVERIFY2(secureFolderTreeScript.valid(),
             qPrintable(secureFolderTreeScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(
        secureFolderTreeScript.script.contains(QStringLiteral("-SecureWipeReleasedBlocks $true")));

    const auto attributeScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::HfsReplaceInlineAttribute, target, baseHfsFileMutationPayload()));
    QVERIFY2(attributeScript.valid(),
             qPrintable(attributeScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(attributeScript.script.contains(QStringLiteral("replace-inline-attribute-image")));
    QVERIFY(attributeScript.script.contains(QStringLiteral("-FileId 17")));
    QVERIFY(attributeScript.script.contains(QStringLiteral("com.apple.FinderInfo")));

    QJsonObject forkAttributePayload = baseHfsFileMutationPayload();
    forkAttributePayload[QStringLiteral("hfs_attribute_name")] =
        QStringLiteral("com.apple.ResourceFork");
    const auto forkAttributeScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::HfsReplaceForkAttribute, target, forkAttributePayload));
    QVERIFY2(forkAttributeScript.valid(),
             qPrintable(forkAttributeScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(forkAttributeScript.script.contains(QStringLiteral("replace-fork-attribute-image")));
    QVERIFY(forkAttributeScript.script.contains(QStringLiteral("-FileId 17")));
    QVERIFY(forkAttributeScript.script.contains(QStringLiteral("com.apple.ResourceFork")));

    const auto growForkAttributeScript =
        builder.buildScript(PartitionOperationPlanner::makeOperation(
            PartitionOperationType::HfsGrowForkAttribute, target, forkAttributePayload));
    QVERIFY2(growForkAttributeScript.valid(),
             qPrintable(growForkAttributeScript.blockers.join(QStringLiteral("; "))));
    QVERIFY(growForkAttributeScript.script.contains(QStringLiteral("grow-fork-attribute-image")));
    QVERIFY(growForkAttributeScript.script.contains(
        QStringLiteral("ui.hfs.raw-fork-attribute-allocation-growth")));
    QVERIFY(growForkAttributeScript.script.contains(QStringLiteral("-FileId 17")));
    QVERIFY(growForkAttributeScript.script.contains(QStringLiteral("com.apple.ResourceFork")));

    QJsonObject forgedTarget = baseHfsFileMutationPayload();
    forgedTarget[QStringLiteral("target_path")] =
        QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk2\\Partition99");
    const auto blocked = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::HfsReplaceFile, target, forgedTarget));
    QVERIFY(!blocked.valid());
    QVERIFY(blocked.blockers.join(' ').contains(QStringLiteral("raw partition")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsLinuxSwapFormatScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 2;
    target.partition_number = 1;
    target.size_bytes = 256ULL * 1024ULL * 1024ULL;
    const QString rawTarget = QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk2\\Partition1");

    QJsonObject payload;
    payload[QStringLiteral("non_native_file_system_tool")] = true;
    payload[QStringLiteral("file_system")] = QStringLiteral("Linux swap");
    payload[QStringLiteral("label")] = QStringLiteral("SAK_SWAP");
    payload[QStringLiteral("target_path")] = rawTarget;
    payload[QStringLiteral("target_wipe_confirmed")] = true;
    payload[QStringLiteral("linux_swap_page_size_bytes")] = QStringLiteral("4096");

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Format, target, payload));
    QVERIFY2(script.valid(), qPrintable(script.blockers.join(QStringLiteral("; "))));
    QVERIFY(script.script.contains(QStringLiteral("SWAPSPACE2")));
    QVERIFY(script.script.contains(QStringLiteral("Write-UInt32Le")));
    QVERIFY(script.script.contains(QStringLiteral("Offset 1028")));
    QVERIFY(script.script.contains(QStringLiteral("SAK_SWAP")));
    QVERIFY(script.script.contains(rawTarget));
    QVERIFY(!script.script.contains(QStringLiteral("mke2fs")));

    QJsonObject forgedTargetPayload = payload;
    forgedTargetPayload[QStringLiteral("target_path")] =
        QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk2\\Partition99");
    const auto forgedScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::Format, target, forgedTargetPayload));
    QVERIFY(!forgedScript.valid());
    QVERIFY(forgedScript.blockers.join(' ').contains(QStringLiteral("selected raw partition")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsResizeScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 1;
    target.partition_number = 3;
    target.size_bytes = 10 * 1024 * 1024;
    QJsonObject payload;
    payload[QStringLiteral("target_size_bytes")] = QStringLiteral("20971520");
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Resize, target, payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("Resize-Partition")));
    QVERIFY(script.script.contains(QStringLiteral("Get-PartitionSupportedSize")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsMergeScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 2;
    target.partition_number = 1;
    target.size_bytes = 50 * 1024 * 1024;
    QJsonObject payload;
    payload[QStringLiteral("source_partition_number")] = QStringLiteral("2");
    payload[QStringLiteral("target_folder")] = QStringLiteral("Merged");
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Merge, target, payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("robocopy.exe")));
    QVERIFY(script.script.contains(QStringLiteral("Remove-Partition")));
    QVERIFY(script.script.contains(QStringLiteral("Resize-Partition")));
}

void PartitionManagerCoreTests::scriptBuilder_formatsByPartitionIdentity() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 1;
    target.partition_number = 3;
    target.size_bytes = 10 * 1024 * 1024;
    QJsonObject payload;
    payload[QStringLiteral("file_system")] = QStringLiteral("NTFS");
    payload[QStringLiteral("label")] = QStringLiteral("Data");
    payload[QStringLiteral("allocation_unit_bytes")] = QStringLiteral("65536");
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Format, target, payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(
        script.script.contains(QStringLiteral("Get-Partition -DiskNumber 1 -PartitionNumber 3")));
    QVERIFY(script.script.contains(QStringLiteral("Format-Volume -FileSystem NTFS")));
    QVERIFY(script.script.contains(QStringLiteral("-AllocationUnitSize 65536")));
    QVERIFY(!script.script.contains(QStringLiteral("Format-Volume -DriveLetter")));
}

void PartitionManagerCoreTests::scriptBuilder_setsPartitionLabelByMountedDriveLetter() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 1;
    target.partition_number = 3;
    target.size_bytes = 10 * 1024 * 1024;
    target.drive_letter = QStringLiteral("E");
    QJsonObject payload;
    payload[QStringLiteral("label")] = QStringLiteral("Backup Data");
    auto operation = PartitionOperationPlanner::makeOperation(
        PartitionOperationType::SetPartitionLabel, target, payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("Set-Volume -DriveLetter E")));
    QVERIFY(script.script.contains(QStringLiteral("-NewFileSystemLabel 'Backup Data'")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsAdvancedParityScripts() {
    PartitionTarget partitionTarget;
    partitionTarget.kind = PartitionTargetKind::Partition;
    partitionTarget.disk_number = 1;
    partitionTarget.partition_number = 3;
    partitionTarget.size_bytes = 10 * 1024 * 1024;
    partitionTarget.drive_letter = QStringLiteral("E");

    PartitionScriptBuilder builder;
    auto check = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::CheckFileSystem, partitionTarget));
    QVERIFY(check.valid());
    QVERIFY(check.script.contains(QStringLiteral("Repair-Volume -DriveLetter E -Scan")));

    auto hidden = builder.buildScript(
        PartitionOperationPlanner::makeOperation(PartitionOperationType::SetPartitionHidden,
                                                 partitionTarget,
                                                 QJsonObject{{QStringLiteral("hidden"), true}}));
    QVERIFY(hidden.valid());
    QVERIFY(hidden.script.contains(QStringLiteral("-IsHidden $true")));
    QVERIFY(hidden.script.contains(QStringLiteral("Remove-PartitionAccessPath")));

    auto active = builder.buildScript(
        PartitionOperationPlanner::makeOperation(PartitionOperationType::SetPartitionActive,
                                                 partitionTarget,
                                                 QJsonObject{{QStringLiteral("active"), true}}));
    QVERIFY(active.valid());
    QVERIFY(active.script.contains(QStringLiteral("-IsActive $true")));

    auto type = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::SetPartitionTypeId,
        partitionTarget,
        QJsonObject{
            {QStringLiteral("type_id"), QStringLiteral("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7")}}));
    QVERIFY(type.valid());
    QVERIFY(type.script.contains(QStringLiteral("Set-Partition")));
    QVERIFY(type.script.contains(QStringLiteral("-GptType $typeId")));

    PartitionTarget diskTarget;
    diskTarget.kind = PartitionTargetKind::Disk;
    diskTarget.disk_number = 7;
    auto init = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::InitializeDisk,
        diskTarget,
        QJsonObject{{QStringLiteral("target_style"), QStringLiteral("GPT")}}));
    QVERIFY(init.valid());
    QVERIFY(init.script.contains(QStringLiteral("Initialize-Disk -Number 7 -PartitionStyle GPT")));

    auto deleteAll = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::DeleteAllPartitions, diskTarget));
    QVERIFY(deleteAll.valid());
    QVERIFY(deleteAll.script.contains(QStringLiteral("Remove-Partition -DiskNumber 7")));

    auto surface = builder.buildScript(
        PartitionOperationPlanner::makeOperation(PartitionOperationType::SurfaceTest, diskTarget));
    QVERIFY(surface.valid());
    QVERIFY(surface.script.contains(QStringLiteral("Get-StorageReliabilityCounter")));

    auto recovery = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::PartitionRecoveryScan,
        diskTarget,
        QJsonObject{{QStringLiteral("scan_mode"), QStringLiteral("Quick")}}));
    QVERIFY(recovery.valid());
    QVERIFY(recovery.script.contains(QStringLiteral("\\\\.\\PhysicalDrive7")));
    QVERIFY(recovery.script.contains(QStringLiteral("Candidate partition boot sector")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsRecoveredPartitionRestoreScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 4;
    QJsonObject payload;
    payload[QStringLiteral("offset_bytes")] = QStringLiteral("1048576000");
    payload[QStringLiteral("size_bytes")] = QStringLiteral("209715200");
    payload[QStringLiteral("type_id")] = QStringLiteral("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7");
    payload[QStringLiteral("partition_style")] = QStringLiteral("GPT");
    auto operation = PartitionOperationPlanner::makeOperation(
        PartitionOperationType::RestoreRecoveredPartition, target, payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("New-Partition -DiskNumber 4")));
    QVERIFY(script.script.contains(QStringLiteral("-Offset 1048576000")));
    QVERIFY(script.script.contains(QStringLiteral("-GptType")));
    QVERIFY(script.script.contains(QStringLiteral("Candidate overlaps existing partition")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsCloneVerificationScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 3;
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = QStringLiteral("C:\\images\\source.img");
    payload[QStringLiteral("target_path")] = QStringLiteral("C:\\images\\target.img");
    payload[QStringLiteral("source_size_bytes")] = QStringLiteral("1048576");
    payload[QStringLiteral("target_size_bytes")] = QStringLiteral("2097152");
    payload[QStringLiteral("verify_mode")] = QStringLiteral("Full verification");
    auto operation = PartitionOperationPlanner::makeOperation(PartitionOperationType::CloneDisk,
                                                              target,
                                                              payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("Copy-SakBytes")));
    QVERIFY(script.script.contains(QStringLiteral("Running full clone verification")));
    QVERIFY(script.script.contains(QStringLiteral("Assert-SakFullCopy")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsOffsetPartitionCloneScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 0;
    target.partition_number = 2;
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = QStringLiteral("\\\\.\\C:");
    payload[QStringLiteral("target_path")] = QStringLiteral("\\\\.\\PhysicalDrive2");
    payload[QStringLiteral("source_size_bytes")] = QStringLiteral("1048576");
    payload[QStringLiteral("target_size_bytes")] = QStringLiteral("2097152");
    payload[QStringLiteral("source_offset_bytes")] = QStringLiteral("4096");
    payload[QStringLiteral("target_offset_bytes")] = QStringLiteral("1048576");
    payload[QStringLiteral("verify_mode")] = QStringLiteral("Sample verification");
    auto operation = PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ClonePartition, target, payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("$sourceOffset = [uint64]4096")));
    QVERIFY(script.script.contains(QStringLiteral("$targetOffset = [uint64]1048576")));
    QVERIFY(script.script.contains(QStringLiteral(
        "Assert-SakSampleCopy $srcVerify $dstVerify $expectedBytes $sourceOffset $targetOffset")));
    QVERIFY(script.script.contains(QStringLiteral("[int64]($targetStart + $point)")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsOsMigrationBootValidationScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 0;
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = QStringLiteral("\\\\.\\PhysicalDrive0");
    payload[QStringLiteral("target_path")] = QStringLiteral("\\\\.\\PhysicalDrive2");
    payload[QStringLiteral("source_size_bytes")] = QStringLiteral("1048576");
    payload[QStringLiteral("target_size_bytes")] = QStringLiteral("2097152");
    payload[QStringLiteral("verify_mode")] = QStringLiteral("Sample verification");
    auto operation = PartitionOperationPlanner::makeOperation(PartitionOperationType::MigrateOs,
                                                              target,
                                                              payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("SAK OS migration boot validation")));
    QVERIFY(script.script.contains(QStringLiteral("EFI System Partition")));
    QVERIFY(script.script.contains(QStringLiteral("BIOS validation passed")));
    QVERIFY(script.script.contains(QStringLiteral("Boot Repair")));
}

void PartitionManagerCoreTests::safetyValidator_blocksUnsafeParityOperations() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    auto& disk = inventory.disks.first();
    disk.is_system = false;
    auto& partition = disk.partitions[1];
    partition.is_system = false;
    partition.is_boot = false;

    PartitionOperationPlanner planner;
    PartitionTarget diskTarget;
    diskTarget.kind = PartitionTargetKind::Disk;
    diskTarget.disk_number = disk.disk_number;
    auto init = PartitionOperationPlanner::makeOperation(PartitionOperationType::InitializeDisk,
                                                         diskTarget);
    auto initPreview = planner.previewOperation(inventory, init);
    QVERIFY(!initPreview.canApply());
    QVERIFY(initPreview.blockers.join(' ').contains(QStringLiteral("empty/raw disk")));

    PartitionTarget partitionTarget;
    partitionTarget.kind = PartitionTargetKind::Partition;
    partitionTarget.disk_number = disk.disk_number;
    partitionTarget.partition_number = partition.partition_number;
    partitionTarget.size_bytes = partition.size_bytes;
    partitionTarget.drive_letter = partition.volume->drive_letter;
    auto active =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::SetPartitionActive,
                                                 partitionTarget,
                                                 QJsonObject{{QStringLiteral("active"), true}});
    auto activePreview = planner.previewOperation(inventory, active);
    QVERIFY(!activePreview.canApply());
    QVERIFY(activePreview.blockers.join(' ').contains(QStringLiteral("MBR")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsClearLevelDiskWipeScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 4;
    auto operation = PartitionOperationPlanner::makeOperation(PartitionOperationType::WipeDisk,
                                                              target);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("Clear-Disk -Number 4 -RemoveData -RemoveOEM")));
    QVERIFY(script.script.contains(QStringLiteral("Format-Volume -FileSystem NTFS -Full")));
    QVERIFY(script.script.contains(QStringLiteral("Remove-Partition -DiskNumber 4")));

    QJsonObject ssdPayload;
    ssdPayload[QStringLiteral("ssd_secure_erase")] = true;
    const auto ssdScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::WipeDisk, target, ssdPayload));
    QVERIFY(ssdScript.valid());
    QVERIFY(ssdScript.script.contains(QStringLiteral("Optimize-Volume -DriveLetter")));
    QVERIFY(ssdScript.script.contains(QStringLiteral("-ReTrim -Verbose")));
}

void PartitionManagerCoreTests::safetyValidator_blocksUnsafeSystemStyleConversion() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    inventory.disks.first().partition_style = QStringLiteral("MBR");
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 0;
    QJsonObject payload;
    payload[QStringLiteral("target_style")] = QStringLiteral("GPT");
    auto operation = PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ConvertPartitionStyle, target, payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("MBR2GPT")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsEmptyDataDiskStyleConversionScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 4;
    QJsonObject payload;
    payload[QStringLiteral("target_style")] = QStringLiteral("MBR");
    auto operation = PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ConvertPartitionStyle, target, payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("Set-Disk -Number 4 -IsOffline $false")));
    QVERIFY(script.script.contains(QStringLiteral("System disk conversion must use MBR2GPT")));
    QVERIFY(script.script.contains(
        QStringLiteral("Data disk partition-style conversion requires an empty disk")));
    QVERIFY(script.script.contains(QStringLiteral("Clear-Disk -Number 4 -RemoveData")));
    QVERIFY(
        script.script.contains(QStringLiteral("Initialize-Disk -Number 4 -PartitionStyle MBR")));
}

void PartitionManagerCoreTests::safetyValidator_requiresCloneOverwriteConfirmation() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    appendDisposableTargetDisk(&inventory);
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 0;
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = QStringLiteral("\\\\.\\PhysicalDrive0");
    payload[QStringLiteral("target_path")] = QStringLiteral("\\\\.\\PhysicalDrive1");
    auto operation = PartitionOperationPlanner::makeOperation(PartitionOperationType::CloneDisk,
                                                              target,
                                                              payload);

    PartitionOperationPlanner planner;
    auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("overwrite confirmation")));

    operation.payload[QStringLiteral("target_wipe_confirmed")] = true;
    preview = planner.previewOperation(inventory, operation);
    QVERIFY(preview.canApply());
}

void PartitionManagerCoreTests::
    safetyValidator_createImageUsesReadOnlyRiskAndBlocksUnsafeDestinations() {
    const auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 0;
    target.size_bytes = inventory.disks.first().size_bytes;

    QJsonObject payload;
    payload[QStringLiteral("source_path")] = QStringLiteral("\\\\.\\PhysicalDrive0");
    payload[QStringLiteral("target_path")] = QStringLiteral("D:\\images\\disk0.img");
    payload[QStringLiteral("source_size_bytes")] = QString::number(target.size_bytes);
    auto operation = PartitionOperationPlanner::makeOperation(PartitionOperationType::CreateImage,
                                                              target,
                                                              payload);

    PartitionOperationPlanner planner;
    auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(preview.canApply());
    QCOMPARE(preview.operations.first().risk, OperationRisk::ReadOnly);

    operation.payload[QStringLiteral("target_path")] = QStringLiteral("\\\\.\\PhysicalDrive1");
    preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("file path")));
    const auto rawTargetScript = PartitionScriptBuilder().buildScript(operation);
    QVERIFY(!rawTargetScript.valid());
    QVERIFY(rawTargetScript.blockers.join(' ').contains(QStringLiteral("file path")));

    operation.payload[QStringLiteral("target_path")] = QStringLiteral("C:\\disk0.img");
    preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("source disk")));
}

void PartitionManagerCoreTests::
    safetyValidator_restoreImageRequiresSizesAndOverwriteConfirmation() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    inventory.disks.first().is_system = false;
    inventory.disks.first().is_boot = false;
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 0;
    target.size_bytes = inventory.disks.first().size_bytes;
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = QStringLiteral("D:\\images\\disk0.img");
    payload[QStringLiteral("target_path")] = QStringLiteral("\\\\.\\PhysicalDrive0");
    auto operation = PartitionOperationPlanner::makeOperation(PartitionOperationType::RestoreImage,
                                                              target,
                                                              payload);

    PartitionOperationPlanner planner;
    auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    const auto missingEvidenceBlockers = preview.blockers.join(' ');
    QVERIFY(missingEvidenceBlockers.contains(QStringLiteral("overwrite confirmation")));
    QVERIFY(missingEvidenceBlockers.contains(QStringLiteral("known image and target sizes")));
    auto script = PartitionScriptBuilder().buildScript(operation);
    QVERIFY(!script.valid());
    QVERIFY(script.blockers.join(' ').contains(QStringLiteral("overwrite confirmation")));

    operation.payload[QStringLiteral("target_wipe_confirmed")] = true;
    script = PartitionScriptBuilder().buildScript(operation);
    QVERIFY(!script.valid());
    QVERIFY(script.blockers.join(' ').contains(QStringLiteral("known image and target sizes")));

    operation.payload[QStringLiteral("source_size_bytes")] = QStringLiteral("2097152");
    operation.payload[QStringLiteral("target_size_bytes")] = QStringLiteral("1048576");
    preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("smaller than the source")));
    script = PartitionScriptBuilder().buildScript(operation);
    QVERIFY(!script.valid());
    QVERIFY(script.blockers.join(' ').contains(QStringLiteral("smaller than source")));

    operation.payload[QStringLiteral("target_size_bytes")] = QStringLiteral("4194304");
    preview = planner.previewOperation(inventory, operation);
    QVERIFY(preview.canApply());
    script = PartitionScriptBuilder().buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("PhysicalDrive0")));
}

void PartitionManagerCoreTests::safetyValidator_requiresRecoveredPartitionRestoreAcknowledgement() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    inventory.disks.first().is_system = false;
    inventory.disks.first().is_boot = false;
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 0;
    QJsonObject payload;
    payload[QStringLiteral("offset_bytes")] = QStringLiteral("85899345920");
    payload[QStringLiteral("size_bytes")] = QStringLiteral("104857600");
    payload[QStringLiteral("type_id")] = QStringLiteral("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7");
    auto operation = PartitionOperationPlanner::makeOperation(
        PartitionOperationType::RestoreRecoveredPartition, target, payload);

    PartitionOperationPlanner planner;
    auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("acknowledgement")));

    operation.payload[QStringLiteral("restore_acknowledged")] = true;
    preview = planner.previewOperation(inventory, operation);
    QVERIFY(preview.canApply());
}

void PartitionManagerCoreTests::safetyValidator_requiresPartitionRegionCloneConfirmation() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    makeFixtureDataPartitionMutable(&inventory);
    appendDisposableTargetDisk(&inventory);
    const auto& partition = inventory.disks.first().partitions.at(1);
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 0;
    target.partition_number = partition.partition_number;
    target.size_bytes = partition.size_bytes;
    target.drive_letter = partition.volume->drive_letter;
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = QStringLiteral("\\\\.\\C:");
    payload[QStringLiteral("target_path")] = QStringLiteral("\\\\.\\PhysicalDrive1");
    payload[QStringLiteral("source_size_bytes")] = QStringLiteral("1048576");
    payload[QStringLiteral("target_size_bytes")] = QStringLiteral("2097152");
    payload[QStringLiteral("target_disk_number")] = 1;
    payload[QStringLiteral("target_offset_bytes")] = QStringLiteral("1048576");
    auto operation = PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ClonePartition, target, payload);

    PartitionOperationPlanner planner;
    auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("overwrite confirmation")));

    operation.payload[QStringLiteral("target_wipe_confirmed")] = true;
    preview = planner.previewOperation(inventory, operation);
    QVERIFY(preview.canApply());
}

void PartitionManagerCoreTests::safetyValidator_blocksUnsafePayloadTargetDisk() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    appendDisposableTargetDisk(&inventory);
    inventory.disks.last().is_system = true;

    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 0;
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = QStringLiteral("\\\\.\\PhysicalDrive0");
    payload[QStringLiteral("target_path")] = QStringLiteral("\\\\.\\PhysicalDrive1");
    payload[QStringLiteral("source_size_bytes")] = QStringLiteral("2097152");
    payload[QStringLiteral("target_size_bytes")] = QStringLiteral("4194304");
    payload[QStringLiteral("target_wipe_confirmed")] = true;
    auto operation = PartitionOperationPlanner::makeOperation(PartitionOperationType::CloneDisk,
                                                              target,
                                                              payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("Payload target disk")));
}

void PartitionManagerCoreTests::safetyValidator_blocksTooSmallCloneTarget() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    appendDisposableTargetDisk(&inventory, 1, 1'048'576ULL);
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 0;
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = QStringLiteral("\\\\.\\PhysicalDrive0");
    payload[QStringLiteral("target_path")] = QStringLiteral("\\\\.\\PhysicalDrive1");
    payload[QStringLiteral("source_size_bytes")] = QStringLiteral("2097152");
    payload[QStringLiteral("target_size_bytes")] = QStringLiteral("1048576");
    payload[QStringLiteral("target_wipe_confirmed")] = true;
    auto operation = PartitionOperationPlanner::makeOperation(PartitionOperationType::CloneDisk,
                                                              target,
                                                              payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("smaller than the source")));
}

void PartitionManagerCoreTests::safetyValidator_blocksTooSmallPartitionRegionClone() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    makeFixtureDataPartitionMutable(&inventory);
    appendDisposableTargetDisk(&inventory, 1, 1'048'576ULL);
    const auto& partition = inventory.disks.first().partitions.at(1);
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 0;
    target.partition_number = partition.partition_number;
    target.size_bytes = partition.size_bytes;
    target.drive_letter = partition.volume->drive_letter;
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = QStringLiteral("\\\\.\\C:");
    payload[QStringLiteral("target_path")] = QStringLiteral("\\\\.\\PhysicalDrive1");
    payload[QStringLiteral("source_size_bytes")] = QStringLiteral("2097152");
    payload[QStringLiteral("target_size_bytes")] = QStringLiteral("1048576");
    payload[QStringLiteral("target_disk_number")] = 1;
    payload[QStringLiteral("target_offset_bytes")] = QStringLiteral("1048576");
    payload[QStringLiteral("target_wipe_confirmed")] = true;
    auto operation = PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ClonePartition, target, payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("smaller than the source")));
}

void PartitionManagerCoreTests::safetyValidator_blocksUnsupportedFileSystemConversion() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    auto& disk = inventory.disks.first();
    auto& partition = disk.partitions[1];
    disk.is_system = false;
    partition.is_system = false;
    partition.is_boot = false;
    partition.volume->file_system = QStringLiteral("NTFS");

    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 0;
    target.partition_number = 2;
    target.size_bytes = partition.size_bytes;
    target.drive_letter = partition.volume->drive_letter;
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::ConvertFileSystem, target);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("FAT/FAT32")));
}

QJsonObject nonNativeWritePayload(const QString& fileSystem = QStringLiteral("ext4")) {
    QJsonObject payload;
    payload[QStringLiteral("non_native_file_system_tool")] = true;
    payload[QStringLiteral("file_system")] = fileSystem;
    payload[QStringLiteral("target_path")] =
        QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk2\\Partition1");
    payload[QStringLiteral("target_wipe_confirmed")] = true;
    return payload;
}

OperationPreview previewNonNativeOperation(const PartitionInventory& inventory,
                                           PartitionOperationType type,
                                           const PartitionTarget& target,
                                           const QJsonObject& payload) {
    PartitionOperationPlanner planner;
    return planner.previewOperation(
        inventory, PartitionOperationPlanner::makeOperation(type, target, payload));
}

void verifyNonNativeCreateSafetyGates(const PartitionInventory& inventory,
                                      const PartitionDiskInfo& disk) {
    const auto& region = disk.unallocated_regions.first();
    PartitionTarget createTarget;
    createTarget.kind = PartitionTargetKind::Unallocated;
    createTarget.disk_number = disk.disk_number;
    createTarget.offset_bytes = region.offset_bytes;
    createTarget.size_bytes = region.size_bytes;

    QJsonObject createPayload;
    createPayload[QStringLiteral("size_bytes")] = QString::number(128ULL * 1024ULL * 1024ULL);
    createPayload[QStringLiteral("non_native_file_system_tool")] = true;
    createPayload[QStringLiteral("file_system")] = QStringLiteral("ext4");
    createPayload[QStringLiteral("target_wipe_confirmed")] = true;
    auto preview = previewNonNativeOperation(
        inventory, PartitionOperationType::Create, createTarget, createPayload);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));

    QJsonObject blockedCreate = createPayload;
    blockedCreate[QStringLiteral("target_wipe_confirmed")] = false;
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::Create, createTarget, blockedCreate);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("confirmation")));

    QJsonObject unsupportedCreate = createPayload;
    unsupportedCreate[QStringLiteral("file_system")] = QStringLiteral("XFS");
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::Create, createTarget, unsupportedCreate);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("Non-Windows write support")));

    QJsonObject apfsCreate = createPayload;
    apfsCreate[QStringLiteral("file_system")] = QStringLiteral("APFS");
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::Create, createTarget, apfsCreate);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("GPT disk")));

    auto gptInventory = inventory;
    gptInventory.disks[0].partition_style = QStringLiteral("GPT");
    apfsCreate[QStringLiteral("gpt_type")] =
        QStringLiteral("{7C3457EF-0000-11AA-AA11-00306543ECAC}");
    preview = previewNonNativeOperation(
        gptInventory, PartitionOperationType::Create, createTarget, apfsCreate);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));
}

void verifyExtPartitionWriteSafetyGates(const PartitionInventory& inventory,
                                        const PartitionTarget& target,
                                        const PartitionInfoEx& partition,
                                        const QJsonObject& payload) {
    auto preview =
        previewNonNativeOperation(inventory, PartitionOperationType::Format, target, payload);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));

    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::CheckFileSystem, target, payload);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));

    QJsonObject growPayload = payload;
    growPayload[QStringLiteral("target_size_bytes")] =
        QString::number(partition.size_bytes + 256ULL * 1024ULL * 1024ULL);
    preview =
        previewNonNativeOperation(inventory, PartitionOperationType::Resize, target, growPayload);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));

    QJsonObject missingConfirmation = payload;
    missingConfirmation[QStringLiteral("target_wipe_confirmed")] = false;
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::Format, target, missingConfirmation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("confirmation")));

    QJsonObject forgedTarget = payload;
    forgedTarget[QStringLiteral("target_path")] =
        QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk2\\Partition99");
    preview =
        previewNonNativeOperation(inventory, PartitionOperationType::Format, target, forgedTarget);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("selected raw partition")));
}

void verifyHfsAndSwapSafetyGates(const PartitionInventory& inventory,
                                 const PartitionTarget& target,
                                 const QJsonObject& payload) {
    QJsonObject hfsFormat = payload;
    hfsFormat[QStringLiteral("file_system")] = QStringLiteral("hfs+");
    auto preview =
        previewNonNativeOperation(inventory, PartitionOperationType::Format, target, hfsFormat);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));

    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::CheckFileSystem, target, hfsFormat);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));

    QJsonObject unsupported = payload;
    unsupported[QStringLiteral("file_system")] = QStringLiteral("xfs");
    preview =
        previewNonNativeOperation(inventory, PartitionOperationType::Format, target, unsupported);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("HFS+/HFSX")));

    QJsonObject swapFormat = payload;
    swapFormat[QStringLiteral("file_system")] = QStringLiteral("Linux swap");
    swapFormat[QStringLiteral("linux_swap_page_size_bytes")] = QStringLiteral("4096");
    preview =
        previewNonNativeOperation(inventory, PartitionOperationType::Format, target, swapFormat);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));

    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::CheckFileSystem, target, swapFormat);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("Non-Windows write support")));

    QJsonObject apfsFormat = payload;
    apfsFormat[QStringLiteral("file_system")] = QStringLiteral("APFS");
    preview =
        previewNonNativeOperation(inventory, PartitionOperationType::Format, target, apfsFormat);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));

    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::CheckFileSystem, target, apfsFormat);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));
}

void verifyExtShrinkUsageSafetyGate(const PartitionInventory* inventory,
                                    const PartitionTarget& target,
                                    PartitionInfoEx* partition,
                                    const QJsonObject& payload) {
    QJsonObject shrinkPayload = payload;
    shrinkPayload[QStringLiteral("target_size_bytes")] =
        QString::number(partition->size_bytes - 128ULL * 1024ULL * 1024ULL);
    auto preview = previewNonNativeOperation(
        *inventory, PartitionOperationType::Resize, target, shrinkPayload);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));

    partition->volume->total_bytes = 0;
    preview = previewNonNativeOperation(
        *inventory, PartitionOperationType::Resize, target, shrinkPayload);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("usage metadata")));
}

void PartitionManagerCoreTests::safetyValidator_gatesNonNativeWriteOperations() {
    auto inventory = singleDataDiskInventory();
    auto& disk = inventory.disks.first();
    auto& partition = disk.partitions.first();
    partition.volume->file_system = QStringLiteral("ext4");
    partition.volume->drive_letter.clear();
    partition.volume->total_bytes = partition.size_bytes;
    partition.volume->free_bytes = partition.size_bytes / 2;

    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = disk.disk_number;
    target.partition_number = partition.partition_number;
    target.size_bytes = partition.size_bytes;
    target.offset_bytes = partition.offset_bytes;

    const QJsonObject payload = nonNativeWritePayload();
    verifyNonNativeCreateSafetyGates(inventory, disk);
    verifyExtPartitionWriteSafetyGates(inventory, target, partition, payload);
    verifyHfsAndSwapSafetyGates(inventory, target, payload);
    verifyExtShrinkUsageSafetyGate(&inventory, target, &partition, payload);
}

void PartitionManagerCoreTests::safetyValidator_gatesApfsRootFileMutations() {
    auto inventory = singleDataDiskInventory();
    auto& disk = inventory.disks.first();
    auto& partition = disk.partitions.first();
    partition.volume->file_system = QStringLiteral("APFS");
    partition.volume->drive_letter.clear();
    partition.volume->file_system_details = {QStringLiteral("APFS space manager block: 10"),
                                             QStringLiteral("APFS volume candidate block 6"),
                                             QStringLiteral("volume object map OID 103"),
                                             QStringLiteral("root tree OID 104"),
                                             QStringLiteral("Volume OIDs: 102")};

    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = disk.disk_number;
    target.partition_number = partition.partition_number;
    target.size_bytes = partition.size_bytes;
    target.offset_bytes = partition.offset_bytes;

    auto payload = baseApfsRootFileMutationPayload();
    auto preview = previewNonNativeOperation(
        inventory, PartitionOperationType::ApfsWriteRootFile, target, payload);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));
    QCOMPARE(preview.operations.first().risk, OperationRisk::Destructive);

    QJsonObject labelPayload = payload;
    labelPayload.remove(QStringLiteral("apfs_root_file_name"));
    labelPayload.remove(QStringLiteral("apfs_root_file_payload_text"));
    labelPayload.remove(QStringLiteral("apfs_root_file_patch_offset_bytes"));
    labelPayload[QStringLiteral("label")] = QStringLiteral("SAK Relabeled");
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::ApfsChangeVolumeLabel, target, labelPayload);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));
    QCOMPARE(preview.operations.first().risk, OperationRisk::Destructive);

    QJsonObject missingLabel = labelPayload;
    missingLabel.remove(QStringLiteral("label"));
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::ApfsChangeVolumeLabel, target, missingLabel);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("requires a label")));

    QJsonObject invalidLabel = labelPayload;
    invalidLabel[QStringLiteral("label")] = QStringLiteral("Bad/Name");
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::ApfsChangeVolumeLabel, target, invalidLabel);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("path separators")));

    QJsonObject directoryFilePayload = payload;
    directoryFilePayload[QStringLiteral("apfs_root_directory_name")] =
        QStringLiteral("Proof Folder");
    preview = previewNonNativeOperation(inventory,
                                        PartitionOperationType::ApfsWriteRootDirectoryFile,
                                        target,
                                        directoryFilePayload);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));
    QCOMPARE(preview.operations.first().risk, OperationRisk::Destructive);

    preview = previewNonNativeOperation(inventory,
                                        PartitionOperationType::ApfsPatchRootDirectoryFile,
                                        target,
                                        directoryFilePayload);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));
    QCOMPARE(preview.operations.first().risk, OperationRisk::Destructive);

    QJsonObject directoryFileDeletePayload = directoryFilePayload;
    directoryFileDeletePayload.remove(QStringLiteral("apfs_root_file_payload_text"));
    directoryFileDeletePayload.remove(QStringLiteral("apfs_root_file_patch_offset_bytes"));
    preview = previewNonNativeOperation(inventory,
                                        PartitionOperationType::ApfsDeleteRootDirectoryFile,
                                        target,
                                        directoryFileDeletePayload);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));

    QJsonObject missingDirectoryForChild = directoryFilePayload;
    missingDirectoryForChild.remove(QStringLiteral("apfs_root_directory_name"));
    preview = previewNonNativeOperation(inventory,
                                        PartitionOperationType::ApfsWriteRootDirectoryFile,
                                        target,
                                        missingDirectoryForChild);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("directory name")));

    preview = previewNonNativeOperation(inventory,
                                        PartitionOperationType::ApfsPatchRootDirectoryFile,
                                        target,
                                        missingDirectoryForChild);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("directory name")));

    QJsonObject directoryPayload = payload;
    directoryPayload.remove(QStringLiteral("apfs_root_file_name"));
    directoryPayload.remove(QStringLiteral("apfs_root_file_payload_text"));
    directoryPayload.remove(QStringLiteral("apfs_root_file_patch_offset_bytes"));
    directoryPayload[QStringLiteral("apfs_root_directory_name")] = QStringLiteral("Proof Folder");
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::ApfsCreateRootDirectory, target, directoryPayload);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));
    QCOMPARE(preview.operations.first().risk, OperationRisk::Destructive);

    QJsonObject missingDirectoryName = directoryPayload;
    missingDirectoryName.remove(QStringLiteral("apfs_root_directory_name"));
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::ApfsDeleteRootDirectory, target, missingDirectoryName);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("directory name")));

    QJsonObject missingLayout = payload;
    missingLayout[QStringLiteral("apfs_generated_layout_confirmed")] = false;
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::ApfsWriteRootFile, target, missingLayout);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("generated-layout")));

    QJsonObject forgedTarget = payload;
    forgedTarget[QStringLiteral("target_path")] =
        QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk2\\Partition99");
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::ApfsDeleteRootFile, target, forgedTarget);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("selected raw partition")));

    QJsonObject missingPayload = payload;
    missingPayload.remove(QStringLiteral("apfs_root_file_payload_text"));
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::ApfsPatchRootFile, target, missingPayload);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("payload")));

    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::ApfsPatchRootDirectoryFile, target, missingPayload);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("payload")));

    QJsonObject missingOffset = payload;
    missingOffset.remove(QStringLiteral("apfs_root_file_patch_offset_bytes"));
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::ApfsPatchRootFile, target, missingOffset);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("byte offset")));

    QJsonObject childMissingOffset = directoryFilePayload;
    childMissingOffset.remove(QStringLiteral("apfs_root_file_patch_offset_bytes"));
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::ApfsPatchRootDirectoryFile, target, childMissingOffset);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("byte offset")));
}

void PartitionManagerCoreTests::safetyValidator_gatesHfsFileMutations() {
    auto inventory = singleDataDiskInventory();
    auto& disk = inventory.disks.first();
    auto& partition = disk.partitions.first();
    partition.volume->file_system = QStringLiteral("HFS+");
    partition.volume->drive_letter.clear();

    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = disk.disk_number;
    target.partition_number = partition.partition_number;
    target.size_bytes = partition.size_bytes;
    target.offset_bytes = partition.offset_bytes;

    auto payload = baseHfsFileMutationPayload();
    auto preview = previewNonNativeOperation(
        inventory, PartitionOperationType::HfsReplaceFile, target, payload);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));
    QCOMPARE(preview.operations.first().risk, OperationRisk::Destructive);

    preview =
        previewNonNativeOperation(inventory, PartitionOperationType::HfsGrowFile, target, payload);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));
    QCOMPARE(preview.operations.first().risk, OperationRisk::Destructive);

    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::HfsGrowResourceFork, target, payload);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));
    QCOMPARE(preview.operations.first().risk, OperationRisk::Destructive);

    QJsonObject forgedTarget = payload;
    forgedTarget[QStringLiteral("target_path")] =
        QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk2\\Partition99");
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::HfsTruncateFile, target, forgedTarget);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("selected raw partition")));

    QJsonObject missingPath = payload;
    missingPath.remove(QStringLiteral("hfs_path"));
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::HfsReplaceFile, target, missingPath);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("HFS path")));

    QJsonObject createPayload = payload;
    createPayload[QStringLiteral("hfs_path")] = QStringLiteral("/created.txt");
    createPayload.remove(QStringLiteral("hfs_payload_text"));
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::HfsCreateEmptyFile, target, createPayload);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));
    QCOMPARE(preview.operations.first().risk, OperationRisk::Destructive);

    QJsonObject createFilePayload = createPayload;
    createFilePayload[QStringLiteral("hfs_payload_text")] = QStringLiteral("created payload");
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::HfsCreateFile, target, createFilePayload);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));
    QCOMPARE(preview.operations.first().risk, OperationRisk::Destructive);

    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::HfsDeleteEmptyFile, target, createPayload);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));
    QCOMPARE(preview.operations.first().risk, OperationRisk::Destructive);

    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::HfsDeleteFile, target, createPayload);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));
    QCOMPARE(preview.operations.first().risk, OperationRisk::Destructive);

    QJsonObject secureDeletePayload = createPayload;
    secureDeletePayload[QStringLiteral("hfs_secure_wipe_released_blocks")] = true;
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::HfsDeleteFile, target, secureDeletePayload);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));
    QCOMPARE(preview.operations.first().risk, OperationRisk::Destructive);

    QJsonObject unsupportedSecurePayload = payload;
    unsupportedSecurePayload[QStringLiteral("hfs_secure_wipe_released_blocks")] = true;
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::HfsReplaceFile, target, unsupportedSecurePayload);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("secure block wipe")));

    QJsonObject folderPayload = createPayload;
    folderPayload[QStringLiteral("hfs_path")] = QStringLiteral("/Created Folder");
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::HfsCreateEmptyFolder, target, folderPayload);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));
    QCOMPARE(preview.operations.first().risk, OperationRisk::Destructive);

    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::HfsDeleteEmptyFolder, target, folderPayload);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));
    QCOMPARE(preview.operations.first().risk, OperationRisk::Destructive);

    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::HfsDeleteFolderTree, target, folderPayload);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));
    QCOMPARE(preview.operations.first().risk, OperationRisk::Destructive);

    QJsonObject renamePayload = folderPayload;
    renamePayload[QStringLiteral("hfs_destination_path")] = QStringLiteral("/Renamed Folder");
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::HfsRenameMoveCatalogEntry, target, renamePayload);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));
    QCOMPARE(preview.operations.first().risk, OperationRisk::Destructive);

    QJsonObject missingDestinationPath = folderPayload;
    preview = previewNonNativeOperation(inventory,
                                        PartitionOperationType::HfsRenameMoveCatalogEntry,
                                        target,
                                        missingDestinationPath);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("destination HFS path")));

    QJsonObject missingPayload = payload;
    missingPayload.remove(QStringLiteral("hfs_payload_text"));
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::HfsReplaceResourceFork, target, missingPayload);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("payload")));

    QJsonObject missingAttribute = payload;
    missingAttribute.remove(QStringLiteral("hfs_file_id"));
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::HfsReplaceInlineAttribute, target, missingAttribute);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("attribute")));

    QJsonObject forkAttributePayload = payload;
    forkAttributePayload[QStringLiteral("hfs_attribute_name")] =
        QStringLiteral("com.apple.ResourceFork");
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::HfsReplaceForkAttribute, target, forkAttributePayload);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));
    QCOMPARE(preview.operations.first().risk, OperationRisk::Destructive);

    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::HfsGrowForkAttribute, target, forkAttributePayload);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));
    QCOMPARE(preview.operations.first().risk, OperationRisk::Destructive);

    QJsonObject missingForkGrowthPayload = forkAttributePayload;
    missingForkGrowthPayload.remove(QStringLiteral("hfs_payload_text"));
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::HfsGrowForkAttribute, target, missingForkGrowthPayload);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("payload")));

    QJsonObject apfsPayload = payload;
    apfsPayload[QStringLiteral("file_system")] = QStringLiteral("APFS");
    preview = previewNonNativeOperation(
        inventory, PartitionOperationType::HfsReplaceFile, target, apfsPayload);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("HFS+ or HFSX")));
}

void PartitionManagerCoreTests::safetyValidator_allowsResizeIntoAdjacentFreeSpace() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    makeFixtureDataPartitionMutable(&inventory);
    const auto& disk = inventory.disks.first();
    const auto& partition = disk.partitions.at(1);
    QVERIFY(adjacentFreeRegionAfter(disk, partition) != nullptr);

    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = disk.disk_number;
    target.partition_number = partition.partition_number;
    target.size_bytes = partition.size_bytes;
    target.drive_letter = partition.volume->drive_letter;
    QJsonObject payload;
    payload[QStringLiteral("target_size_bytes")] =
        QString::number(partition.size_bytes + 1024 * 1024);
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Resize, target, payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(preview.canApply());
}

void PartitionManagerCoreTests::safetyValidator_blocksResizeBeyondAdjacentFreeSpace() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    makeFixtureDataPartitionMutable(&inventory);
    const auto& disk = inventory.disks.first();
    const auto& partition = disk.partitions.at(1);
    const auto* region = adjacentFreeRegionAfter(disk, partition);
    QVERIFY(region != nullptr);

    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = disk.disk_number;
    target.partition_number = partition.partition_number;
    target.size_bytes = partition.size_bytes;
    target.drive_letter = partition.volume->drive_letter;
    QJsonObject payload;
    payload[QStringLiteral("target_size_bytes")] =
        QString::number(partition.size_bytes + region->size_bytes + 1);
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Resize, target, payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("contiguous free space")));
}

void PartitionManagerCoreTests::safetyValidator_blocksResizeBelowUsedBytes() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    makeFixtureDataPartitionMutable(&inventory);
    const auto& disk = inventory.disks.first();
    const auto& partition = disk.partitions.at(1);

    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = disk.disk_number;
    target.partition_number = partition.partition_number;
    target.size_bytes = partition.size_bytes;
    target.drive_letter = partition.volume->drive_letter;
    QJsonObject payload;
    payload[QStringLiteral("target_size_bytes")] =
        QString::number(partition.volume->total_bytes - partition.volume->free_bytes - 1);
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Resize, target, payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("used volume space")));
}

void PartitionManagerCoreTests::safetyValidator_blocksNoopResize() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    makeFixtureDataPartitionMutable(&inventory);
    const auto& disk = inventory.disks.first();
    const auto& partition = disk.partitions.at(1);

    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = disk.disk_number;
    target.partition_number = partition.partition_number;
    target.size_bytes = partition.size_bytes;
    target.drive_letter = partition.volume->drive_letter;
    QJsonObject payload;
    payload[QStringLiteral("target_size_bytes")] = QString::number(partition.size_bytes);
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Resize, target, payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("must change")));
}

void PartitionManagerCoreTests::safetyValidator_blocksResizeStartMovePayload() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    makeFixtureDataPartitionMutable(&inventory);
    const auto& disk = inventory.disks.first();
    const auto& partition = disk.partitions.at(1);

    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = disk.disk_number;
    target.partition_number = partition.partition_number;
    target.size_bytes = partition.size_bytes;
    target.drive_letter = partition.volume->drive_letter;
    QJsonObject payload;
    payload[QStringLiteral("target_size_bytes")] = QString::number(partition.size_bytes + 1);
    payload[QStringLiteral("target_offset_bytes")] = QString::number(partition.offset_bytes - 1);
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Resize, target, payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("offline move engine")));
}

void PartitionManagerCoreTests::safetyValidator_blocksResizeDonorPayload() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    makeFixtureDataPartitionMutable(&inventory);
    const auto& disk = inventory.disks.first();
    const auto& partition = disk.partitions.at(1);

    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = disk.disk_number;
    target.partition_number = partition.partition_number;
    target.size_bytes = partition.size_bytes;
    target.drive_letter = partition.volume->drive_letter;
    QJsonObject payload;
    payload[QStringLiteral("target_size_bytes")] = QString::number(partition.size_bytes + 1);
    payload[QStringLiteral("donor_partition_number")] = QStringLiteral("1");
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Resize, target, payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("Donor-space")));
}

void PartitionManagerCoreTests::safetyValidator_blocksOversizedCreate() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    auto& disk = inventory.disks.first();
    disk.is_system = false;
    const auto region = disk.unallocated_regions.last();

    PartitionTarget target;
    target.kind = PartitionTargetKind::Unallocated;
    target.disk_number = region.disk_number;
    target.offset_bytes = region.offset_bytes;
    target.size_bytes = region.size_bytes;
    QJsonObject payload;
    payload[QStringLiteral("size_bytes")] = QString::number(region.size_bytes + 1);
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Create, target, payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("unallocated region")));
}

void PartitionManagerCoreTests::safetyValidator_blocksInvalidCreateTypePayload() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    auto& disk = inventory.disks.first();
    disk.is_system = false;
    const auto region = disk.unallocated_regions.last();

    PartitionTarget target;
    target.kind = PartitionTargetKind::Unallocated;
    target.disk_number = region.disk_number;
    target.offset_bytes = region.offset_bytes;
    target.size_bytes = region.size_bytes;
    QJsonObject payload;
    payload[QStringLiteral("size_bytes")] = QStringLiteral("1048576");
    payload[QStringLiteral("file_system")] = QStringLiteral("NTFS");
    payload[QStringLiteral("mbr_type")] = QStringLiteral("FAT32");
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Create, target, payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("incompatible")));
}

void PartitionManagerCoreTests::safetyValidator_blocksCreateOffsetOutsideSelectedRegion() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    auto& disk = inventory.disks.first();
    disk.is_system = false;
    const auto region = disk.unallocated_regions.last();

    PartitionTarget target;
    target.kind = PartitionTargetKind::Unallocated;
    target.disk_number = region.disk_number;
    target.offset_bytes = region.offset_bytes;
    target.size_bytes = region.size_bytes;
    QJsonObject payload;
    payload[QStringLiteral("size_bytes")] = QString::number(region.size_bytes);
    payload[QStringLiteral("relative_offset_bytes")] = QStringLiteral("1048576");
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Create, target, payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("selected unallocated region")));
}

void PartitionManagerCoreTests::safetyValidator_blocksDynamicUnallocatedCreate() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    auto& disk = inventory.disks.first();
    disk.is_system = false;
    disk.is_dynamic = true;
    const auto region = disk.unallocated_regions.last();

    PartitionTarget target;
    target.kind = PartitionTargetKind::Unallocated;
    target.disk_number = region.disk_number;
    target.offset_bytes = region.offset_bytes;
    target.size_bytes = region.size_bytes;
    QJsonObject payload;
    payload[QStringLiteral("size_bytes")] = QStringLiteral("1048576");
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Create, target, payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("Dynamic disks")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsMbr2GptScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 0;
    QJsonObject payload;
    payload[QStringLiteral("target_style")] = QStringLiteral("GPT");
    payload[QStringLiteral("mode")] = QStringLiteral("mbr2gpt");
    auto operation = PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ConvertPartitionStyle, target, payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("mbr2gpt.exe /validate")));
    QVERIFY(script.script.contains(QStringLiteral("mbr2gpt.exe /convert")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsAllocateFreeSpaceScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 2;
    target.partition_number = 1;
    target.size_bytes = 512ULL * 1024ULL * 1024ULL;
    target.drive_letter = QStringLiteral("T");

    QJsonObject payload;
    payload[QStringLiteral("source_partition_number")] = QStringLiteral("2");
    payload[QStringLiteral("source_size_bytes")] = QStringLiteral("536870912");
    payload[QStringLiteral("bytes_to_allocate")] = QStringLiteral("134217728");
    payload[QStringLiteral("source_drive_letter")] = QStringLiteral("S");
    payload[QStringLiteral("source_file_system")] = QStringLiteral("NTFS");
    payload[QStringLiteral("source_label")] = QStringLiteral("Donor");
    payload[QStringLiteral("backup_directory")] = QStringLiteral("C:\\SAKBackups");
    payload[QStringLiteral("target_wipe_confirmed")] = true;

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::AllocateFreeSpace, target, payload));
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("Donor partition must be directly after")));
    QVERIFY(script.script.contains(QStringLiteral("robocopy.exe $from $to /MIR /COPYALL")));
    QVERIFY(script.script.contains(QStringLiteral("Get-SakFileManifest")));
    QVERIFY(script.script.contains(QStringLiteral("Remove-Partition -DiskNumber 2")));
    QVERIFY(script.script.contains(QStringLiteral("Resize-Partition -DiskNumber 2")));
    QVERIFY(script.script.contains(QStringLiteral("New-Partition -DiskNumber 2")));
    QVERIFY(script.script.contains(QStringLiteral("Compare-Object")));
    QVERIFY(script.script.contains(QStringLiteral("Repair-Volume -DriveLetter $sourceDrive")));
}

void PartitionManagerCoreTests::safetyValidator_blocksUnsafeAllocateFreeSpacePayloads() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    makeFixtureDataPartitionMutable(&inventory);
    appendAdjacentDonorPartition(&inventory);
    const auto& disk = inventory.disks.first();
    const auto& partition = disk.partitions.at(1);
    const auto& donor = disk.partitions.last();

    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = disk.disk_number;
    target.partition_number = partition.partition_number;
    target.size_bytes = partition.size_bytes;
    target.drive_letter = partition.volume->drive_letter;

    PartitionOperationPlanner planner;
    auto preview = planner.previewOperation(inventory,
                                            PartitionOperationPlanner::makeOperation(
                                                PartitionOperationType::AllocateFreeSpace, target));
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("donor partition")));
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("amount")));
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("backup")));
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("confirmation")));

    QJsonObject validPayload;
    validPayload[QStringLiteral("source_partition_number")] =
        QString::number(donor.partition_number);
    validPayload[QStringLiteral("source_size_bytes")] = QString::number(donor.size_bytes);
    validPayload[QStringLiteral("bytes_to_allocate")] = QStringLiteral("134217728");
    validPayload[QStringLiteral("source_drive_letter")] = donor.volume->drive_letter;
    validPayload[QStringLiteral("source_file_system")] = donor.volume->file_system;
    validPayload[QStringLiteral("source_label")] = donor.volume->label;
    validPayload[QStringLiteral("backup_directory")] = QStringLiteral("Z:\\SAKBackups");
    validPayload[QStringLiteral("target_wipe_confirmed")] = true;
    preview = planner.previewOperation(
        inventory,
        PartitionOperationPlanner::makeOperation(
            PartitionOperationType::AllocateFreeSpace, target, validPayload));
    QVERIFY(preview.canApply());

    QJsonObject sameVolumeBackup = validPayload;
    sameVolumeBackup[QStringLiteral("backup_directory")] = QStringLiteral("D:\\backup");
    preview = planner.previewOperation(
        inventory,
        PartitionOperationPlanner::makeOperation(
            PartitionOperationType::AllocateFreeSpace, target, sameVolumeBackup));
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("target or donor")));

    QJsonObject tooLarge = validPayload;
    tooLarge[QStringLiteral("bytes_to_allocate")] = QStringLiteral("1000000000");
    preview =
        planner.previewOperation(inventory,
                                 PartitionOperationPlanner::makeOperation(
                                     PartitionOperationType::AllocateFreeSpace, target, tooLarge));
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("safety reserve")));

    QJsonObject staleSize = validPayload;
    staleSize[QStringLiteral("source_size_bytes")] = QStringLiteral("1");
    preview =
        planner.previewOperation(inventory,
                                 PartitionOperationPlanner::makeOperation(
                                     PartitionOperationType::AllocateFreeSpace, target, staleSize));
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("stale")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsOfflineMoveAndMetadataScripts() {
    PartitionTarget partitionTarget;
    partitionTarget.kind = PartitionTargetKind::Partition;
    partitionTarget.disk_number = 2;
    partitionTarget.partition_number = 1;
    partitionTarget.size_bytes = 1024ULL * 1024ULL * 1024ULL;
    partitionTarget.drive_letter = QStringLiteral("T");

    QJsonObject movePayload;
    movePayload[QStringLiteral("target_offset_bytes")] = QStringLiteral("2147483648");
    movePayload[QStringLiteral("target_size_bytes")] = QStringLiteral("1073741824");
    movePayload[QStringLiteral("drive_letter")] = QStringLiteral("T");
    movePayload[QStringLiteral("file_system")] = QStringLiteral("NTFS");
    movePayload[QStringLiteral("label")] = QStringLiteral("Moved");
    movePayload[QStringLiteral("backup_directory")] = QStringLiteral("Z:\\SAKBackups");
    movePayload[QStringLiteral("target_wipe_confirmed")] = true;

    PartitionScriptBuilder builder;
    const auto moveScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::MovePartition, partitionTarget, movePayload));
    QVERIFY(moveScript.valid());
    QVERIFY(moveScript.script.contains(QStringLiteral("Remove-Partition -DiskNumber 2")));
    QVERIFY(moveScript.script.contains(QStringLiteral("New-Partition -DiskNumber 2")));
    QVERIFY(moveScript.script.contains(QStringLiteral("-Offset $targetOffset")));
    QVERIFY(moveScript.script.contains(QStringLiteral("Assert-SakManifestMatch")));

    QJsonObject primaryPayload = movePayload;
    primaryPayload[QStringLiteral("target_layout")] = QStringLiteral("logical");
    primaryPayload[QStringLiteral("source_size_bytes")] = QStringLiteral("1073741824");
    const auto primaryScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ConvertPrimaryLogical, partitionTarget, primaryPayload));
    QVERIFY(primaryScript.valid());
    QVERIFY(primaryScript.script.contains(QStringLiteral("Invoke-SakDiskPart")));
    QVERIFY(primaryScript.script.contains(QStringLiteral("create partition logical")));
    QVERIFY(primaryScript.script.contains(QStringLiteral("Assert-SakManifestMatch")));

    const auto serialScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ChangeVolumeSerialNumber, partitionTarget, movePayload));
    QVERIFY(serialScript.valid());
    QVERIFY(serialScript.script.contains(QStringLiteral("Get-SakVolumeSerial")));
    QVERIFY(serialScript.script.contains(QStringLiteral("Format-Volume -DriveLetter $drive")));
    QVERIFY(serialScript.script.contains(QStringLiteral("Volume serial changed")));

    PartitionTarget diskTarget;
    diskTarget.kind = PartitionTargetKind::Disk;
    diskTarget.disk_number = 2;
    diskTarget.size_bytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
    QJsonObject dynamicPayload = primaryPayload;
    const auto dynamicScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ConvertDynamicDiskToBasic, diskTarget, dynamicPayload));
    QVERIFY(dynamicScript.valid());
    QVERIFY(dynamicScript.script.contains(QStringLiteral("delete volume override")));
    QVERIFY(dynamicScript.script.contains(QStringLiteral("convert basic")));
    QVERIFY(dynamicScript.script.contains(QStringLiteral("Assert-SakManifestMatch")));
}

void PartitionManagerCoreTests::safetyValidator_allowsConfirmedOfflineRebuildOperations() {
    auto inventory = singleDataDiskInventory();
    const auto& disk = inventory.disks.first();
    const auto& partition = disk.partitions.first();

    PartitionTarget partitionTarget;
    partitionTarget.kind = PartitionTargetKind::Partition;
    partitionTarget.disk_number = disk.disk_number;
    partitionTarget.partition_number = partition.partition_number;
    partitionTarget.size_bytes = partition.size_bytes;
    partitionTarget.drive_letter = partition.volume->drive_letter;

    QJsonObject payload;
    payload[QStringLiteral("target_offset_bytes")] = QStringLiteral("2147483648");
    payload[QStringLiteral("target_size_bytes")] = QStringLiteral("1073741824");
    payload[QStringLiteral("drive_letter")] = partition.volume->drive_letter;
    payload[QStringLiteral("file_system")] = partition.volume->file_system;
    payload[QStringLiteral("label")] = partition.volume->label;
    payload[QStringLiteral("backup_directory")] = QStringLiteral("Z:\\SAKBackups");
    payload[QStringLiteral("target_wipe_confirmed")] = true;

    PartitionOperationPlanner planner;
    auto preview = planner.previewOperation(
        inventory,
        PartitionOperationPlanner::makeOperation(
            PartitionOperationType::MovePartition, partitionTarget, payload));
    QVERIFY(preview.canApply());

    QJsonObject primaryPayload = payload;
    primaryPayload[QStringLiteral("target_layout")] = QStringLiteral("logical");
    primaryPayload[QStringLiteral("source_size_bytes")] = QString::number(partition.size_bytes);
    preview = planner.previewOperation(
        inventory,
        PartitionOperationPlanner::makeOperation(
            PartitionOperationType::ConvertPrimaryLogical, partitionTarget, primaryPayload));
    QVERIFY(preview.canApply());

    preview = planner.previewOperation(
        inventory,
        PartitionOperationPlanner::makeOperation(
            PartitionOperationType::ChangeVolumeSerialNumber, partitionTarget, payload));
    QVERIFY(preview.canApply());

    auto dynamicInventory = singleDataDiskInventory(true);
    PartitionTarget diskTarget;
    diskTarget.kind = PartitionTargetKind::Disk;
    diskTarget.disk_number = dynamicInventory.disks.first().disk_number;
    diskTarget.size_bytes = dynamicInventory.disks.first().size_bytes;
    preview = planner.previewOperation(
        dynamicInventory,
        PartitionOperationPlanner::makeOperation(
            PartitionOperationType::ConvertDynamicDiskToBasic, diskTarget, primaryPayload));
    QVERIFY(preview.canApply());
}

void PartitionManagerCoreTests::safetyValidator_blocksUnsafeOfflineRebuildOperations() {
    auto inventory = singleDataDiskInventory();
    const auto& partition = inventory.disks.first().partitions.first();
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = partition.disk_number;
    target.partition_number = partition.partition_number;
    target.size_bytes = partition.size_bytes;
    target.drive_letter = partition.volume->drive_letter;

    PartitionOperationPlanner planner;
    auto preview = planner.previewOperation(
        inventory,
        PartitionOperationPlanner::makeOperation(PartitionOperationType::MovePartition, target));
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("backup")));
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("confirmation")));

    QJsonObject sameVolumePayload;
    sameVolumePayload[QStringLiteral("target_offset_bytes")] = QStringLiteral("2147483648");
    sameVolumePayload[QStringLiteral("target_size_bytes")] = QStringLiteral("1073741824");
    sameVolumePayload[QStringLiteral("drive_letter")] = QStringLiteral("T");
    sameVolumePayload[QStringLiteral("file_system")] = QStringLiteral("NTFS");
    sameVolumePayload[QStringLiteral("backup_directory")] = QStringLiteral("T:\\backup");
    sameVolumePayload[QStringLiteral("target_wipe_confirmed")] = true;
    preview = planner.previewOperation(
        inventory,
        PartitionOperationPlanner::makeOperation(
            PartitionOperationType::MovePartition, target, sameVolumePayload));
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("selected volume")));

    auto multiPartitionInventory = inventory;
    PartitionInfoEx extra = multiPartitionInventory.disks.first().partitions.first();
    extra.partition_number = 2;
    extra.offset_bytes = extra.offset_bytes + extra.size_bytes;
    extra.volume->drive_letter = QStringLiteral("U");
    multiPartitionInventory.disks.first().partitions.append(extra);
    QJsonObject primaryPayload = sameVolumePayload;
    primaryPayload[QStringLiteral("backup_directory")] = QStringLiteral("Z:\\backup");
    primaryPayload[QStringLiteral("target_layout")] = QStringLiteral("logical");
    primaryPayload[QStringLiteral("source_size_bytes")] = QString::number(partition.size_bytes);
    preview = planner.previewOperation(
        multiPartitionInventory,
        PartitionOperationPlanner::makeOperation(
            PartitionOperationType::ConvertPrimaryLogical, target, primaryPayload));
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("single mounted")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsChangeClusterSizeScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 2;
    target.partition_number = 1;
    target.size_bytes = 128 * 1024 * 1024;
    target.drive_letter = QStringLiteral("T");

    QJsonObject payload;
    payload[QStringLiteral("drive_letter")] = QStringLiteral("T");
    payload[QStringLiteral("file_system")] = QStringLiteral("NTFS");
    payload[QStringLiteral("allocation_unit_bytes")] = QStringLiteral("4096");
    payload[QStringLiteral("label")] = QStringLiteral("Data");
    payload[QStringLiteral("backup_directory")] = QStringLiteral("C:\\SAKBackups");
    payload[QStringLiteral("target_wipe_confirmed")] = true;

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ChangeClusterSize, target, payload));
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("$drive = 'T'")));
    QVERIFY(script.script.contains(QStringLiteral("robocopy.exe $from $to /MIR /COPYALL")));
    QVERIFY(script.script.contains(QStringLiteral("Get-SakFileManifest")));
    QVERIFY(script.script.contains(QStringLiteral("Format-Volume -DriveLetter $drive")));
    QVERIFY(script.script.contains(QStringLiteral("-AllocationUnitSize $allocationUnitBytes")));
    QVERIFY(script.script.contains(QStringLiteral("Compare-Object")));
    QVERIFY(script.script.contains(QStringLiteral("Repair-Volume -DriveLetter $drive -Scan")));
}

void PartitionManagerCoreTests::safetyValidator_blocksUnsafeClusterSizePayloads() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    makeFixtureDataPartitionMutable(&inventory);
    const auto& disk = inventory.disks.first();
    const auto& partition = disk.partitions.at(1);

    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = disk.disk_number;
    target.partition_number = partition.partition_number;
    target.size_bytes = partition.size_bytes;
    target.drive_letter = partition.volume->drive_letter;

    PartitionOperationPlanner planner;
    auto missing =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::ChangeClusterSize, target);
    auto preview = planner.previewOperation(inventory, missing);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("allocation unit")));
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("backup directory")));
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("confirmation")));

    QJsonObject sameVolumePayload;
    sameVolumePayload[QStringLiteral("allocation_unit_bytes")] = QStringLiteral("4096");
    sameVolumePayload[QStringLiteral("backup_directory")] =
        QStringLiteral("%1:\\backup").arg(target.drive_letter.left(1).toUpper());
    sameVolumePayload[QStringLiteral("target_wipe_confirmed")] = true;
    preview = planner.previewOperation(
        inventory,
        PartitionOperationPlanner::makeOperation(
            PartitionOperationType::ChangeClusterSize, target, sameVolumePayload));
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("selected volume")));

    QJsonObject validPayload = sameVolumePayload;
    validPayload[QStringLiteral("backup_directory")] = QStringLiteral("D:\\SAKBackups");
    preview = planner.previewOperation(
        inventory,
        PartitionOperationPlanner::makeOperation(
            PartitionOperationType::ChangeClusterSize, target, validPayload));
    QVERIFY(preview.canApply());
}

void PartitionManagerCoreTests::scriptBuilder_buildsBitLockerMutationScripts() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 2;
    target.partition_number = 1;
    target.drive_letter = QStringLiteral("D");

    QJsonObject unlockPayload;
    unlockPayload[QStringLiteral("recovery_password")] =
        QStringLiteral("111111-222222-333333-444444-555555-666666-777777-888888");
    auto unlock = PartitionOperationPlanner::makeOperation(PartitionOperationType::BitLockerUnlock,
                                                           target,
                                                           unlockPayload);
    PartitionScriptBuilder builder;
    const auto unlockScript = builder.buildScript(unlock);
    QVERIFY(unlockScript.valid());
    QVERIFY(unlockScript.script.contains(QStringLiteral("manage-bde.exe -unlock")));
    QVERIFY(unlockScript.script.contains(
        QStringLiteral("111111-222222-333333-444444-555555-666666-777777-888888")));
    QVERIFY(unlockScript.dry_run_script.contains(QStringLiteral("<redacted>")));
    QVERIFY(!unlockScript.dry_run_script.contains(
        QStringLiteral("111111-222222-333333-444444-555555-666666-777777-888888")));

    const auto suspend = builder.buildScript(
        PartitionOperationPlanner::makeOperation(PartitionOperationType::BitLockerSuspend, target));
    QVERIFY(suspend.valid());
    QVERIFY(suspend.script.contains(QStringLiteral("manage-bde.exe -protectors -disable")));

    const auto resume = builder.buildScript(
        PartitionOperationPlanner::makeOperation(PartitionOperationType::BitLockerResume, target));
    QVERIFY(resume.valid());
    QVERIFY(resume.script.contains(QStringLiteral("manage-bde.exe -protectors -enable")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsDirectDefragScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 2;
    target.partition_number = 1;
    target.drive_letter = QStringLiteral("T");

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(
        PartitionOperationPlanner::makeOperation(PartitionOperationType::DefragVolume, target));
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("$driveLetter = 'T'")));
    QVERIFY(script.script.contains(QStringLiteral("Refusing HDD defrag on SSD/NVMe media")));
    QVERIFY(script.script.contains(
        QStringLiteral("Optimize-Volume -DriveLetter $driveLetter -Analyze")));
    QVERIFY(script.script.contains(
        QStringLiteral("Optimize-Volume -DriveLetter $driveLetter -Defrag")));
    QVERIFY(
        script.script.contains(QStringLiteral("Repair-Volume -DriveLetter $driveLetter -Scan")));
}

void PartitionManagerCoreTests::safetyValidator_allowsHddDefragOnlyOnReportedHdd() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    makeFixtureDataPartitionMutable(&inventory);
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 0;
    target.partition_number = 2;
    target.size_bytes = inventory.disks.first().partitions.at(1).size_bytes;
    target.drive_letter = QStringLiteral("C");

    PartitionOperationPlanner planner;
    auto operation = PartitionOperationPlanner::makeOperation(PartitionOperationType::DefragVolume,
                                                              target);
    auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("SSD/NVMe")));

    inventory.disks.first().media_type = QStringLiteral("Unspecified");
    inventory.disks.first().bus_type = QStringLiteral("USB");
    inventory.disks.first().model = QStringLiteral("Virtual Disk");
    preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("reported as HDD")));

    inventory.disks.first().media_type = QStringLiteral("HDD");
    inventory.disks.first().model = QStringLiteral("Disposable HDD");
    preview = planner.previewOperation(inventory, operation);
    QVERIFY(preview.canApply());
}

void PartitionManagerCoreTests::scriptBuilder_buildsBiosBootRepairScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 0;
    QJsonObject payload;
    payload[QStringLiteral("windows_path")] = QStringLiteral("D:\\Windows");
    payload[QStringLiteral("esp_letter")] = QStringLiteral("S");
    payload[QStringLiteral("boot_mode")] = QStringLiteral("BIOS");

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::RepairBoot, target, payload));
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("bcdboot.exe 'D:\\Windows' /s S: /f BIOS")));
    QVERIFY(script.script.contains(QStringLiteral("bootsect.exe /nt60 S: /mbr")));
}

void PartitionManagerCoreTests::operationQueue_blocksLayoutMismatch() {
    PartitionOperationQueue queue;
    OperationPreview preview;
    preview.before_layout_hash = QStringLiteral("a");
    preview.operations.append(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::OptimizeSsd, PartitionTarget{}));
    queue.addPreview(preview);
    QVERIFY(queue.canApply(QStringLiteral("a")));
    QVERIFY(!queue.canApply(QStringLiteral("b")));
}

void PartitionManagerCoreTests::operationQueue_redoAvailableOnlyAfterUndo() {
    PartitionOperationQueue queue;
    OperationPreview preview;
    preview.before_layout_hash = QStringLiteral("layout");
    preview.operations.append(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::OptimizeSsd, PartitionTarget{}));
    queue.addPreview(preview);

    QVERIFY(!queue.canRedo());
    QVERIFY(queue.undo());
    QVERIFY(queue.isEmpty());
    QVERIFY(queue.canRedo());
    QVERIFY(queue.redo());
    QVERIFY(!queue.isEmpty());
    QVERIFY(!queue.canRedo());

    QVERIFY(queue.undo());
    QVERIFY(queue.canRedo());
    queue.discard();
    QVERIFY(!queue.canRedo());

    queue.addPreview(preview);
    QVERIFY(!queue.canRedo());
}

void PartitionManagerCoreTests::powershellQuoting_escapesSingleQuotes() {
    QCOMPARE(PartitionScriptBuilder::quotePowerShell(QStringLiteral("Randy's Disk")),
             QStringLiteral("'Randy''s Disk'"));
}

void PartitionManagerCoreTests::fileRecoveryEngine_scansAndRestoresOfflineImage() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QDir root(temp.path());
    QVERIFY(root.mkpath(QStringLiteral("source")));
    QVERIFY(root.mkpath(QStringLiteral("restore")));

    const QString imagePath = root.filePath(QStringLiteral("source/recovery-image.bin"));
    QFile image(imagePath);
    QVERIFY(image.open(QIODevice::WriteOnly));
    const QByteArray pdf =
        QByteArrayLiteral("%PDF-1.7\n1 0 obj\n<< /Type /Catalog >>\nendobj\n%%EOF");
    const QByteArray jpeg = QByteArray::fromHex("ffd8ffe000104a46494600010100000100010000ffd9");
    image.write(QByteArrayLiteral("padding-before"));
    image.write(pdf);
    image.write(QByteArrayLiteral("padding-between"));
    image.write(jpeg);
    image.write(QByteArrayLiteral("padding-after"));
    image.close();

    const QByteArray beforeHash = fileSha256(imagePath);
    FileRecoveryScanOptions scanOptions;
    scanOptions.image_path = imagePath;
    const auto scan = FileRecoveryEngine::scanOfflineImage(scanOptions);
    QVERIFY(scan.source_opened_read_only);
    QVERIFY(scan.warnings.isEmpty());
    QCOMPARE(scan.candidates.size(), kExpectedRecoveredFixtureCount);

    QStringList extensions;
    for (const auto& candidate : scan.candidates) {
        extensions.append(candidate.extension);
        QVERIFY(candidate.size_bytes > 0);
        QVERIFY(!candidate.sha256.isEmpty());
    }
    QVERIFY(extensions.contains(QStringLiteral("pdf")));
    QVERIFY(extensions.contains(QStringLiteral("jpg")));

    FileRecoveryRestoreOptions restoreOptions;
    restoreOptions.image_path = imagePath;
    restoreOptions.destination_directory = root.filePath(QStringLiteral("restore"));
    restoreOptions.candidates = scan.candidates;
    const auto restore = FileRecoveryEngine::restoreCandidates(restoreOptions);
    QVERIFY(restore.source_opened_read_only);
    QVERIFY(restore.source_not_mutated);
    QVERIFY(restore.warnings.isEmpty());
    QCOMPARE(restore.restored_paths.size(), kExpectedRecoveredFixtureCount);
    QCOMPARE(fileSha256(imagePath), beforeHash);

    for (const auto& restoredPath : restore.restored_paths) {
        QVERIFY(QFileInfo::exists(restoredPath));
        QVERIFY(QFileInfo(restoredPath).size() > 0);
    }
}

QTEST_MAIN(PartitionManagerCoreTests)
#include "test_partition_manager_core.moc"

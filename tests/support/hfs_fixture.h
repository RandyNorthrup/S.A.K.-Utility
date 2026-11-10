// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file hfs_fixture.h
/// @brief Single home for the minimal openable HFS+ image and its B-tree/fork builders.
///
/// hfsReaderFixture() lays out a real, spec-conformant HFS+ volume header, an allocation fork, a
/// catalog B-tree (header node + leaf node with a folder, a file, and a nested file), and the file
/// data blocks, so PartitionHfsFileSystemReader walks it end to end. The lock-in tests in
/// test_partition_manager_core.cpp assert the accept path over this image and its many sibling
/// fixtures build on the same fork/record/node writers; test_fuzz_hfs_reader.cpp mutates the image
/// bytes and asserts the reader stays fail-closed and never crashes. Keeping the layout and its
/// shared builders here means those consumers can never drift apart.
///
/// Symbol names match the original in-place fixture (kTestHfs*, writeHfs*, hfsReaderFixture, ...)
/// so the big partition test consuming this header needs no call-site renames. The little-/big-
/// endian pokers come from byte_writer.h.

#pragma once

#include "byte_writer.h"

#include <QByteArray>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cstdint>

namespace sak::testfixtures::hfs {

inline constexpr qsizetype kTestHfsHeaderOffset = 1024;
inline constexpr qsizetype kTestHfsVersionOffset = 2;
inline constexpr qsizetype kTestHfsAttributesOffset = 4;
inline constexpr qsizetype kTestHfsFileCountOffset = 32;
inline constexpr qsizetype kTestHfsFolderCountOffset = 36;
inline constexpr qsizetype kTestHfsBlockSizeOffset = 40;
inline constexpr qsizetype kTestHfsTotalBlocksOffset = 44;
inline constexpr qsizetype kTestHfsFreeBlocksOffset = 48;
inline constexpr qsizetype kTestHfsWrapperMdbOffset = 1024;
inline constexpr qsizetype kTestHfsWrapperAllocationBlockSizeOffset = 0x14;
inline constexpr qsizetype kTestHfsWrapperAllocationBlockStartOffset = 0x1C;
inline constexpr qsizetype kTestHfsWrapperEmbeddedSignatureOffset = 0x7C;
inline constexpr qsizetype kTestHfsWrapperEmbeddedExtentStartOffset = 0x7E;
inline constexpr qsizetype kTestHfsWrapperEmbeddedExtentCountOffset = 0x80;
inline constexpr qsizetype kTestHfsAllocationForkOffset = 112;
inline constexpr qsizetype kTestHfsExtentsForkOffset = 192;
inline constexpr qsizetype kTestHfsCatalogForkOffset = 272;
inline constexpr qsizetype kTestHfsAttributesForkOffset = 352;
inline constexpr qsizetype kTestHfsForkLogicalSizeOffset = 0;
inline constexpr qsizetype kTestHfsForkTotalBlocksOffset = 12;
inline constexpr qsizetype kTestHfsForkExtentsOffset = 16;
inline constexpr qsizetype kTestHfsExtentStartBlockOffset = 0;
inline constexpr qsizetype kTestHfsExtentBlockCountOffset = 4;
inline constexpr qsizetype kTestHfsCatalogFileDataForkOffset = 88;
inline constexpr qsizetype kTestHfsForkDataBytes = 80;
inline constexpr qsizetype kTestHfsCatalogFileResourceForkOffset =
    kTestHfsCatalogFileDataForkOffset + kTestHfsForkDataBytes;
inline constexpr qsizetype kTestHfsBTreeKindOffset = 8;
inline constexpr qsizetype kTestHfsBTreeHeightOffset = 9;
inline constexpr qsizetype kTestHfsBTreeNumRecordsOffset = 10;
inline constexpr qsizetype kTestHfsBTreeHeaderRecordOffset = 14;
inline constexpr qsizetype kTestHfsBTreeHeaderTreeDepthOffset = 0;
inline constexpr qsizetype kTestHfsBTreeHeaderRootNodeOffset = 2;
inline constexpr qsizetype kTestHfsBTreeHeaderLeafRecordsOffset = 6;
inline constexpr qsizetype kTestHfsBTreeHeaderFirstLeafNodeOffset = 10;
inline constexpr qsizetype kTestHfsBTreeHeaderLastLeafNodeOffset = 14;
inline constexpr qsizetype kTestHfsBTreeHeaderNodeSizeOffset = 18;
inline constexpr qsizetype kTestHfsBTreeHeaderMaxKeyLengthOffset = 20;
inline constexpr qsizetype kTestHfsBTreeHeaderTotalNodesOffset = 22;
inline constexpr qsizetype kTestHfsBTreeHeaderFreeNodesOffset = 26;
inline constexpr qsizetype kTestHfsBTreeHeaderKeyCompareTypeOffset = 37;
inline constexpr qsizetype kTestHfsBTreeHeaderAttributesOffset = 38;
inline constexpr qsizetype kTestHfsBTreeHeaderMapRecordOffset = 248;
inline constexpr qsizetype kTestHfsBTreeNodeDescriptorSize = 14;
inline constexpr uint32_t kTestHfsBTreeBigKeysMask = 0x00'00'00'02;
inline constexpr uint32_t kTestHfsBTreeVariableIndexKeysMask = 0x00'00'00'04;
inline constexpr uint32_t kTestHfsSplitCatalogTotalNodes = 8;
inline constexpr uint16_t kTestHfsCatalogMaxKeyLength = 516;
inline constexpr uint16_t kTestHfsExtentsMaxKeyLength = 10;
inline constexpr int kTestHfsSplitFixtureFileCount = 14;
inline constexpr qsizetype kTestHfsCatalogRecordIdOffset = 8;
inline constexpr qsizetype kTestHfsExtentsKeyLength = 10;
inline constexpr qsizetype kTestHfsExtentsRecordBytes = 64;
inline constexpr uint32_t kTestHfsJournaledMask = 0x00'00'20'00;
inline constexpr uint32_t kTestHfsWrapperAllocationBlockSize = 4096;
inline constexpr uint16_t kTestHfsWrapperAllocationStartSector = 8;
inline constexpr uint16_t kTestHfsWrapperEmbeddedStartBlock = 10;
inline constexpr uint16_t kTestHfsWrapperEmbeddedBlockCount = 64;
inline constexpr uint32_t kTestHfsBlockSize = 4096;
inline constexpr uint32_t kTestHfsCatalogStartBlock = 2;
inline constexpr uint32_t kTestHfsCatalogNodeSize = 4096;
inline constexpr uint32_t kTestHfsCatalogTotalNodes = 2;
inline constexpr uint32_t kTestHfsAttributesStartBlock = 50;
inline constexpr uint32_t kTestHfsAttributesNodeSize = 4096;
inline constexpr uint32_t kTestHfsAttributesTotalNodes = 2;
inline constexpr uint32_t kTestHfsHelloFileBlock = 4;
inline constexpr qsizetype kTestHfsVolumeJournalInfoBlockOffset = 12;
inline constexpr uint32_t kTestHfsNoteFileBlock = 5;
inline constexpr uint32_t kTestHfsExtentsStartBlock = 6;
inline constexpr uint32_t kTestHfsExtentsNodeSize = 1024;
inline constexpr uint32_t kTestHfsDataForkType = 0x00;
inline constexpr uint32_t kTestHfsResourceForkType = 0xFF;
inline constexpr uint32_t kTestHfsCatalogFileId = 4;
inline constexpr uint32_t kTestHfsAllocationStartBlock = 60;
inline constexpr uint32_t kTestHfsAttributeInlineRecord = 0x10;
inline constexpr uint32_t kTestHfsAttributeForkRecord = 0x20;
inline constexpr uint32_t kTestHfsResourceFileBlock = 9;
inline constexpr uint32_t kTestHfsAttributeForkValueBlock = 56;

inline void writeHfsExtent(QByteArray* bytes,
                           qsizetype offset,
                           uint32_t startBlock,
                           uint32_t blockCount) {
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

inline void writeHfsFork(QByteArray* bytes,
                         qsizetype offset,
                         uint64_t logicalSize,
                         uint32_t totalBlocks,
                         uint32_t firstBlock) {
    writeBe64(bytes, offset + kTestHfsForkLogicalSizeOffset, logicalSize);
    writeBe32(bytes, offset + kTestHfsForkTotalBlocksOffset, totalBlocks);
    writeHfsExtent(bytes, offset + kTestHfsForkExtentsOffset, firstBlock, totalBlocks);
}

inline void setHfsAllocationBit(QByteArray* bytes, uint32_t block) {
    const qsizetype offset =
        static_cast<qsizetype>(kTestHfsAllocationStartBlock * kTestHfsBlockSize + block / 8U);
    const char mask = static_cast<char>(0x80U >> (block % 8U));
    (*bytes)[offset] = static_cast<char>((*bytes)[offset] | mask);
}

inline void clearHfsAllocationBit(QByteArray* bytes, uint32_t block) {
    const qsizetype offset =
        static_cast<qsizetype>(kTestHfsAllocationStartBlock * kTestHfsBlockSize + block / 8U);
    const char mask = static_cast<char>(0x80U >> (block % 8U));
    (*bytes)[offset] = static_cast<char>((*bytes)[offset] & ~mask);
}

inline bool hfsAllocationBitSet(const QByteArray& bytes, uint32_t block) {
    const qsizetype offset =
        static_cast<qsizetype>(kTestHfsAllocationStartBlock * kTestHfsBlockSize + block / 8U);
    const auto value = static_cast<unsigned char>(bytes.at(offset));
    const auto mask = static_cast<unsigned char>(0x80U >> (block % 8U));
    return (value & mask) != 0;
}

inline void writeHfsAllocationFork(QByteArray* image, const QVector<uint32_t>& allocatedBlocks) {
    writeHfsFork(image,
                 kTestHfsHeaderOffset + kTestHfsAllocationForkOffset,
                 kTestHfsBlockSize,
                 1,
                 kTestHfsAllocationStartBlock);
    for (uint32_t block : allocatedBlocks) {
        setHfsAllocationBit(image, block);
    }
}

inline void writeHfsForkWithInitialExtent(QByteArray* bytes,
                                          qsizetype offset,
                                          const HfsForkFixture& fork) {
    writeBe64(bytes, offset + kTestHfsForkLogicalSizeOffset, fork.logical_size);
    writeBe32(bytes, offset + kTestHfsForkTotalBlocksOffset, fork.total_blocks);
    writeHfsExtent(
        bytes, offset + kTestHfsForkExtentsOffset, fork.first_block, fork.first_block_count);
}

inline QByteArray hfsCatalogKey(uint32_t parentId, const QString& name) {
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

inline QByteArray hfsFolderRecord(uint32_t folderId) {
    QByteArray record(88, '\0');
    writeBe16(&record, 0, 1);
    writeBe32(&record, kTestHfsCatalogRecordIdOffset, folderId);
    return record;
}

inline QByteArray hfsFileRecord(uint32_t fileId, const QByteArray& data, uint32_t firstBlock) {
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

inline QByteArray hfsFileRecordWithInitialExtent(uint32_t fileId,
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

inline QByteArray hfsFileRecordWithForks(const HfsFileRecordFixture& fixture) {
    QByteArray record(248, '\0');
    writeBe16(&record, 0, 2);
    writeBe32(&record, kTestHfsCatalogRecordIdOffset, fixture.file_id);
    writeHfsForkWithInitialExtent(&record, kTestHfsCatalogFileDataForkOffset, fixture.data_fork);
    writeHfsForkWithInitialExtent(&record,
                                  kTestHfsCatalogFileResourceForkOffset,
                                  fixture.resource_fork);
    return record;
}

inline QByteArray hfsCatalogRecord(uint32_t parentId, const QString& name, const QByteArray& data) {
    QByteArray record = hfsCatalogKey(parentId, name);
    record.append(data);
    if ((record.size() % 2) != 0) {
        record.append('\0');
    }
    return record;
}

inline void writeHfsNodeOffsets(QByteArray* node,
                                const QVector<qsizetype>& offsets,
                                qsizetype freeOffset) {
    for (int index = 0; index < offsets.size(); ++index) {
        writeBe16(node, node->size() - ((index + 1) * 2), static_cast<uint16_t>(offsets.at(index)));
    }
    writeBe16(node, node->size() - ((offsets.size() + 1) * 2), static_cast<uint16_t>(freeOffset));
}

inline void setFixtureMapBit(QByteArray* node, uint32_t nodeNumber, bool set) {
    const qsizetype offset = kTestHfsBTreeHeaderMapRecordOffset + nodeNumber / 8;
    const auto mask = static_cast<char>(0x80U >> (nodeNumber % 8U));
    (*node)[offset] = set ? static_cast<char>((*node)[offset] | mask)
                          : static_cast<char>((*node)[offset] & ~mask);
}

inline void writeHfsCatalogHeaderNode(QByteArray* image) {
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

inline void writeHfsCatalogLeafNode(QByteArray* image) {
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

inline void writeHfsCatalogLeafRecords(QByteArray* image,
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

// A minimal but genuinely walkable HFS+ image: volume header + allocation fork + catalog B-tree
// (header node and a leaf with a folder, a file, and a nested file) + the file data blocks.
inline QByteArray hfsReaderFixture() {
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

}  // namespace sak::testfixtures::hfs

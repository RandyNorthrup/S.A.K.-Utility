// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file ext_fixture.h
/// @brief Single home for the minimal openable ext2/ext3/ext4 image used by fixtures and fuzzing.
///
/// extReaderFixture() lays out a real, spec-conformant ext superblock, a group descriptor, an
/// inode table (root dir, a regular file, a nested dir with a file, and a symlink) and the
/// directory blocks that reference them, so PartitionExtFileSystemReader walks it end to end.
/// The lock-in tests in test_partition_manager_core.cpp assert the accept path over this image;
/// test_fuzz_ext_reader.cpp mutates its bytes and asserts the reader stays fail-closed and never
/// crashes. Keeping the layout here means those two consumers can never drift apart.
///
/// Symbol names match the original in-place fixture (kTestExt*, testExtInodeOffset, ...) so the
/// big partition test consuming this header needs no call-site renames.

#pragma once

#include "byte_writer.h"

#include <QByteArray>
#include <QVector>

#include <algorithm>
#include <cstdint>
#include <tuple>

namespace sak::testfixtures::ext {

inline constexpr qsizetype kTestExtSuperblockOffset = 1024;
inline constexpr qsizetype kTestExtInodesCountOffset = 0x0;
inline constexpr qsizetype kTestExtBlocksCountLoOffset = 0x4;
inline constexpr qsizetype kTestExtFreeBlocksCountLoOffset = 0xC;
inline constexpr qsizetype kTestExtFreeInodesCountOffset = 0x10;
inline constexpr qsizetype kTestExtFirstDataBlockOffset = 0x14;
inline constexpr qsizetype kTestExtLogBlockSizeOffset = 0x18;
inline constexpr qsizetype kTestExtBlocksPerGroupOffset = 0x20;
inline constexpr qsizetype kTestExtInodesPerGroupOffset = 0x28;
inline constexpr qsizetype kTestExtMagicOffset = 0x38;
inline constexpr qsizetype kTestExtFeatureCompatOffset = 0x5C;
inline constexpr qsizetype kTestExtFeatureIncompatOffset = 0x60;
inline constexpr qsizetype kTestExtFeatureRoCompatOffset = 0x64;
inline constexpr qsizetype kTestExtVolumeNameOffset = 0x78;
inline constexpr qsizetype kTestExtInodeSizeOffset = 0x58;
inline constexpr qsizetype kTestExtGroupDescriptorInodeTableLoOffset = 0x08;
inline constexpr qsizetype kTestExtInodeModeOffset = 0x00;
inline constexpr qsizetype kTestExtInodeSizeLoOffset = 0x04;
inline constexpr qsizetype kTestExtInodeFlagsOffset = 0x20;
inline constexpr qsizetype kTestExtInodeBlocksOffset = 0x28;
inline constexpr qsizetype kTestExtInodeSizeHiOffset = 0x6C;

inline constexpr uint32_t kTestExtCompatHasJournal = 0x0004;
inline constexpr uint32_t kTestExtIncompatExtents = 0x0040;
inline constexpr uint32_t kTestExtInodeFlagExtents = 0x00'08'00'00;
inline constexpr uint32_t kTestExtBlockSize = 1024;
inline constexpr uint32_t kTestExtInodeSize = 128;
inline constexpr qsizetype kTestExtInodeBlockBytes = 60;
inline constexpr uint32_t kTestExtInodeTableBlock = 5;
inline constexpr uint32_t kTestExtRootDirectoryBlock = 10;
inline constexpr uint32_t kTestExtHelloFileBlock = 11;
inline constexpr uint32_t kTestExtDocsDirectoryBlock = 12;
inline constexpr uint32_t kTestExtNoteFileBlock = 13;

inline constexpr uint16_t kTestExtMagic = 0xEF53;

inline qsizetype testExtInodeOffset(uint32_t inodeNumber) {
    return static_cast<qsizetype>(kTestExtInodeTableBlock * kTestExtBlockSize +
                                  (inodeNumber - 1) * kTestExtInodeSize);
}

inline uint16_t alignedExtRecordLength(qsizetype nameLength) {
    return static_cast<uint16_t>((8 + nameLength + 3) & ~3);
}

struct ExtDirectoryEntryFixture {
    uint32_t inode{0};
    QByteArray name;
    uint8_t file_type{0};
    uint16_t record_length{0};
};

inline void writeExtDirectoryEntry(QByteArray* bytes,
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

inline void writeExtExtentMappedBlock(QByteArray* bytes,
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

inline void writeExtInode(QByteArray* bytes, const ExtInodeFixture& inode) {
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

inline void writeExtDirectoryBlock(
    QByteArray* bytes,
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

// A minimal but genuinely walkable ext image: superblock + group descriptor + inode table +
// directory blocks. @p extentMappedHello lays the "hello.txt" data out via an ext4 extent header
// instead of a direct block pointer, so the extent-tree path is exercised too.
inline QByteArray extReaderFixture(bool extentMappedHello = false) {
    QByteArray image(static_cast<qsizetype>(64 * kTestExtBlockSize), '\0');
    writeLe32(&image, kTestExtSuperblockOffset + kTestExtInodesCountOffset, 64);
    writeLe32(&image, kTestExtSuperblockOffset + kTestExtBlocksCountLoOffset, 64);
    writeLe32(&image, kTestExtSuperblockOffset + kTestExtFirstDataBlockOffset, 1);
    writeLe32(&image, kTestExtSuperblockOffset + kTestExtLogBlockSizeOffset, 0);
    writeLe32(&image, kTestExtSuperblockOffset + kTestExtBlocksPerGroupOffset, 8192);
    writeLe32(&image, kTestExtSuperblockOffset + kTestExtInodesPerGroupOffset, 64);
    writeLe16(&image, kTestExtSuperblockOffset + kTestExtMagicOffset, kTestExtMagic);
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

}  // namespace sak::testfixtures::ext

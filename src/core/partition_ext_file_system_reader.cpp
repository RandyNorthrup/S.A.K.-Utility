// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_ext_file_system_reader.cpp
/// @brief Read-only ext2/ext3/ext4 file browser for Partition Manager.

#include "sak/partition_ext_file_system_reader.h"

#include "sak/partition_raw_device_io.h"

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

namespace sak {

namespace {

constexpr uint64_t kExtSuperblockOffset = 1024;
constexpr qsizetype kExtSuperblockSize = 1024;
constexpr qsizetype kExtMagicOffset = 0x38;
constexpr qsizetype kExtInodesCountOffset = 0x0;
constexpr qsizetype kExtBlocksCountLoOffset = 0x4;
constexpr qsizetype kExtFirstDataBlockOffset = 0x14;
constexpr qsizetype kExtLogBlockSizeOffset = 0x18;
constexpr qsizetype kExtBlocksPerGroupOffset = 0x20;
constexpr qsizetype kExtInodesPerGroupOffset = 0x28;
constexpr qsizetype kExtFeatureCompatOffset = 0x5C;
constexpr qsizetype kExtFeatureIncompatOffset = 0x60;
constexpr qsizetype kExtFeatureRoCompatOffset = 0x64;
constexpr qsizetype kExtInodeSizeOffset = 0x58;
constexpr qsizetype kExtBlocksCountHiOffset = 0x150;
constexpr qsizetype kExtGroupDescSizeOffset = 0xFE;
constexpr uint16_t kExtMagic = 0xEF53;
constexpr uint16_t kExtExtentMagic = 0xF30A;
constexpr uint32_t kExtCompatHasJournal = 0x0004;
constexpr uint32_t kExtIncompatCompression = 0x0001;
constexpr uint32_t kExtIncompatNeedsRecovery = 0x0004;
constexpr uint32_t kExtIncompatJournalDevice = 0x0008;
constexpr uint32_t kExtIncompatExtents = 0x0040;
constexpr uint32_t kExtIncompat64Bit = 0x0080;
constexpr uint32_t kExtIncompatInlineData = 0x8000;
constexpr uint32_t kExtIncompatEncrypt = 0x10000;
constexpr uint32_t kExtRoCompatHugeFile = 0x0008;
constexpr uint32_t kExtRoCompatGdtCsum = 0x0010;
constexpr uint32_t kExtRoCompatDirNlink = 0x0020;
constexpr uint32_t kExtRoCompatExtraIsize = 0x0040;
constexpr uint32_t kExtRoCompatBigAlloc = 0x0200;
constexpr uint32_t kExtRoCompatMetadataCsum = 0x0400;
constexpr uint32_t kExtInodeModeDirectory = 0x4000;
constexpr uint32_t kExtInodeModeRegular = 0x8000;
constexpr uint32_t kExtInodeModeSymlink = 0xA000;
constexpr uint32_t kExtInodeModeTypeMask = 0xF000;
constexpr uint32_t kExtInodeFlagExtents = 0x00'08'00'00;
constexpr qsizetype kExtGroupDescInodeTableLoOffset = 0x08;
constexpr qsizetype kExtGroupDescInodeTableHiOffset = 0x28;
constexpr qsizetype kExtInodeModeOffset = 0x00;
constexpr qsizetype kExtInodeSizeLoOffset = 0x04;
constexpr qsizetype kExtInodeBlockCountLoOffset = 0x1C;
constexpr qsizetype kExtInodeBlocksOffset = 0x28;
constexpr qsizetype kExtInodeFlagsOffset = 0x20;
constexpr qsizetype kExtInodeSizeHiOffset = 0x6C;
constexpr qsizetype kExtInodeBlockBytes = 60;
constexpr qsizetype kExtDirectBlockCount = 12;
constexpr qsizetype kExtSingleIndirectIndex = 12;
constexpr qsizetype kExtDoubleIndirectIndex = 13;
constexpr qsizetype kUint16Size = 2;
constexpr qsizetype kUint32Size = 4;
constexpr uint32_t kRootInode = 2;
constexpr uint32_t kMinimumBlockSize = 1024;
constexpr uint32_t kMaximumBlockSize = 64 * 1024;
constexpr uint32_t kMinimumInodeSize = 128;
constexpr uint32_t kDefaultInodeSize = 128;
constexpr uint32_t kMinimumGroupDescriptorSize = 32;
constexpr uint64_t kMaxDirectoryBytes = 64ULL * 1024ULL * 1024ULL;
constexpr int kDefaultPathResolveEntryLimit = 100'000;
constexpr qsizetype kDirEntryHeaderBytes = 8;
constexpr int kDirEntryTypeRegular = 1;
constexpr int kDirEntryTypeDirectory = 2;
constexpr int kDirEntryTypeSymlink = 7;
constexpr int kHexFieldWidth = 8;
constexpr int kHexBase = 16;
constexpr int kBitsPerUint32 = 32;
constexpr uint32_t kMaxExtLogBlockSizeShift = 31;
constexpr int kExtentHeaderBytes = 12;
constexpr int kExtentRecordBytes = 12;
constexpr qsizetype kExtentLogicalBlockOffset = 0;
constexpr qsizetype kExtentLengthOffset = 4;
constexpr qsizetype kExtentPhysicalHighOffset = 6;
constexpr qsizetype kExtentPhysicalLowOffset = 8;
constexpr qsizetype kExtentIndexLeafLowOffset = 4;
constexpr qsizetype kExtentIndexLeafHighOffset = 8;
constexpr int kExtentTreeMaxDepth = 5;
constexpr uint16_t kExtentUninitializedMask = 0x8000;
constexpr uint64_t kZeroPhysicalBlock = 0;
constexpr int kExportNameFallbackBase = 10;
constexpr qsizetype kExportNameMaxCharacters = 180;
constexpr int kFirstExportNameCollisionIndex = 2;
constexpr int kMaxExportNameCollisionAttempts = 1000;
constexpr uint64_t kMaxSymlinkTargetBytes = 16ULL * 1024ULL;

struct ExtSuperblock {
    QString file_system;
    uint32_t block_size{0};
    uint32_t inode_size{kDefaultInodeSize};
    uint32_t group_desc_size{kMinimumGroupDescriptorSize};
    uint32_t blocks_per_group{0};
    uint32_t inodes_per_group{0};
    uint32_t first_data_block{0};
    uint32_t compat{0};
    uint32_t incompat{0};
    uint32_t ro_compat{0};
    uint64_t blocks_count{0};
};

struct ExtInode {
    uint32_t inode_number{0};
    uint16_t mode{0};
    uint32_t flags{0};
    uint32_t allocated_512_byte_blocks{0};
    uint64_t size_bytes{0};
    QByteArray blocks;

    [[nodiscard]] bool directory() const noexcept {
        return (mode & kExtInodeModeTypeMask) == kExtInodeModeDirectory;
    }

    [[nodiscard]] bool regularFile() const noexcept {
        return (mode & kExtInodeModeTypeMask) == kExtInodeModeRegular;
    }

    [[nodiscard]] bool symlink() const noexcept {
        return (mode & kExtInodeModeTypeMask) == kExtInodeModeSymlink;
    }

    [[nodiscard]] bool extentMapped() const noexcept { return (flags & kExtInodeFlagExtents) != 0; }
};

struct ExtentRecord {
    uint32_t logical_start{0};
    uint32_t length{0};
    uint64_t physical_start{0};
    bool initialized{true};
};

struct DirectoryRecord {
    QString name;
    uint32_t inode{0};
    int file_type{0};
};

bool hasBytes(const QByteArray& bytes, qsizetype offset, qsizetype length) {
    return offset >= 0 && length >= 0 && offset <= bytes.size() && bytes.size() - offset >= length;
}

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

uint64_t joinLowHigh32(uint32_t low, uint32_t high, bool useHigh) {
    if (!useHigh) {
        return low;
    }
    return (static_cast<uint64_t>(high) << kBitsPerUint32) | low;
}

QString hex32(uint32_t value) {
    return QStringLiteral("0x%1").arg(
        static_cast<qulonglong>(value), kHexFieldWidth, kHexBase, QLatin1Char('0'));
}

bool isPowerOfTwo(uint32_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

bool checkedMul(uint64_t left, uint64_t right, uint64_t* output) {
    if (!output) {
        return false;
    }
    if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) {
        return false;
    }
    *output = left * right;
    return true;
}

bool checkedAdd(uint64_t left, uint64_t right, uint64_t* output) {
    if (!output) {
        return false;
    }
    if (right > std::numeric_limits<uint64_t>::max() - left) {
        return false;
    }
    *output = left + right;
    return true;
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

QString cleanEntryName(const QByteArray& bytes) {
    QString name = QString::fromUtf8(bytes);
    if (name.contains(QChar::ReplacementCharacter)) {
        name = QString::fromLatin1(bytes);
    }
    return name;
}

QStringList pathParts(const QString& input, QStringList* blockers) {
    const QString path = input.trimmed().isEmpty() ? QStringLiteral("/") : input.trimmed();
    if (path.contains(QChar::Null) || path.contains(QLatin1Char('\\'))) {
        blockers->append(QStringLiteral("ext path contains unsupported control or backslash text"));
        return {};
    }

    QStringList parts;
    for (const auto& part : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part == QStringLiteral(".")) {
            continue;
        }
        if (part == QStringLiteral("..")) {
            blockers->append(QStringLiteral("ext path traversal is not allowed"));
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

QString typeNameFromMode(uint16_t mode) {
    const uint16_t kind = mode & kExtInodeModeTypeMask;
    if (kind == kExtInodeModeDirectory) {
        return QStringLiteral("Directory");
    }
    if (kind == kExtInodeModeRegular) {
        return QStringLiteral("File");
    }
    if (kind == kExtInodeModeSymlink) {
        return QStringLiteral("Symlink");
    }
    return QStringLiteral("Other");
}

class ExtReader {
public:
    explicit ExtReader(QIODevice* device) : m_device(device) {}

    [[nodiscard]] bool load() {
        if (!m_device || !m_device->isReadable()) {
            m_blockers.append(QStringLiteral("Readable ext device or image is required"));
            return false;
        }

        const auto superblockBytes = readAt(kExtSuperblockOffset, kExtSuperblockSize);
        if (!superblockBytes.has_value()) {
            m_blockers.append(QStringLiteral("Unable to read ext superblock"));
            return false;
        }
        parseSuperblock(*superblockBytes);
        validateSuperblock();
        return m_blockers.isEmpty();
    }

    [[nodiscard]] PartitionExtFileReadResult listDirectory(const QString& path, int maxEntries) {
        PartitionExtFileReadResult result = baseResult();
        const auto inode = resolvePath(path);
        if (!inode.has_value()) {
            result.blockers = m_blockers;
            return result;
        }
        if (!inode->directory()) {
            result.blockers.append(QStringLiteral("Selected ext path is not a directory"));
            result.blockers.append(m_blockers);
            return result;
        }

        const QString parent = normalizedDisplayPath(path);
        const auto records = readDirectoryRecords(*inode, std::max(1, maxEntries));
        if (!records.has_value()) {
            result.blockers = m_blockers;
            return result;
        }
        for (const auto& record : *records) {
            const auto child = readInode(record.inode);
            if (!child.has_value()) {
                result.warnings.append(QStringLiteral("Skipped inode %1 while listing %2")
                                           .arg(record.inode)
                                           .arg(parent));
                continue;
            }
            result.entries.append(entryFor(record, *child, parent));
        }
        result.blockers = m_blockers;
        result.warnings.append(m_warnings);
        result.ok = result.blockers.isEmpty();
        return result;
    }

    [[nodiscard]] PartitionExtFileReadResult readFile(const QString& path, uint64_t maxBytes) {
        PartitionExtFileReadResult result = baseResult();
        const auto inode = resolvePath(path);
        if (!inode.has_value()) {
            result.blockers = m_blockers;
            return result;
        }
        if (!inode->regularFile()) {
            result.blockers.append(QStringLiteral("Selected ext path is not a regular file"));
            result.blockers.append(m_blockers);
            return result;
        }
        if (inode->size_bytes > maxBytes) {
            result.blockers.append(
                QStringLiteral("Selected ext file is larger than the read cap (%1 > %2 bytes)")
                    .arg(inode->size_bytes)
                    .arg(maxBytes));
            result.blockers.append(m_blockers);
            return result;
        }

        const auto bytes = readInodeData(*inode, maxBytes);
        if (!bytes.has_value()) {
            result.blockers = m_blockers;
            return result;
        }
        result.data = *bytes;
        result.blockers = m_blockers;
        result.warnings.append(m_warnings);
        result.ok = result.blockers.isEmpty();
        return result;
    }

private:
    [[nodiscard]] PartitionExtFileReadResult baseResult() const {
        PartitionExtFileReadResult result;
        result.file_system = m_superblock.file_system;
        result.warnings = m_warnings;
        return result;
    }

    void parseSuperblock(const QByteArray& bytes) {
        const uint32_t logBlockSize = le32(bytes, kExtLogBlockSizeOffset);
        m_superblock.compat = le32(bytes, kExtFeatureCompatOffset);
        m_superblock.incompat = le32(bytes, kExtFeatureIncompatOffset);
        m_superblock.ro_compat = le32(bytes, kExtFeatureRoCompatOffset);
        m_superblock.file_system =
            extFamilyName(m_superblock.compat, m_superblock.incompat, m_superblock.ro_compat);
        m_superblock.block_size =
            logBlockSize <= kMaxExtLogBlockSizeShift ? kMinimumBlockSize << logBlockSize : 0;
        m_superblock.inode_size = le16(bytes, kExtInodeSizeOffset);
        if (m_superblock.inode_size == 0) {
            m_superblock.inode_size = kDefaultInodeSize;
        }
        m_superblock.group_desc_size =
            (m_superblock.incompat & kExtIncompat64Bit) != 0
                ? std::max<uint32_t>(le16(bytes, kExtGroupDescSizeOffset),
                                     kMinimumGroupDescriptorSize)
                : kMinimumGroupDescriptorSize;
        m_superblock.blocks_per_group = le32(bytes, kExtBlocksPerGroupOffset);
        m_superblock.inodes_per_group = le32(bytes, kExtInodesPerGroupOffset);
        m_superblock.first_data_block = le32(bytes, kExtFirstDataBlockOffset);
        m_superblock.blocks_count = joinLowHigh32(le32(bytes, kExtBlocksCountLoOffset),
                                                  le32(bytes, kExtBlocksCountHiOffset),
                                                  (m_superblock.incompat & kExtIncompat64Bit) != 0);

        if (le16(bytes, kExtMagicOffset) != kExtMagic) {
            m_blockers.append(QStringLiteral("Ext superblock magic not found"));
        }
    }

    void validateSuperblock() {
        appendSuperblockGeometryBlockers();
        appendUnsupportedIncompatBlockers();
        appendUnsupportedRoCompatBlockers();
        appendSuperblockWarnings();
    }

    void appendSuperblockGeometryBlockers() {
        if (m_superblock.block_size < kMinimumBlockSize ||
            m_superblock.block_size > kMaximumBlockSize || !isPowerOfTwo(m_superblock.block_size)) {
            m_blockers.append(QStringLiteral("Unsupported ext block size"));
        }
        if (m_superblock.inode_size < kMinimumInodeSize || !isPowerOfTwo(m_superblock.inode_size)) {
            m_blockers.append(QStringLiteral("Unsupported ext inode size"));
        }
        if (m_superblock.blocks_per_group == 0 || m_superblock.inodes_per_group == 0) {
            m_blockers.append(QStringLiteral("Ext group sizing is invalid"));
        }
    }

    void appendUnsupportedIncompatBlockers() {
        if ((m_superblock.incompat & kExtIncompatCompression) != 0) {
            m_blockers.append(QStringLiteral("Compressed ext volumes are not supported"));
        }
        if ((m_superblock.incompat & kExtIncompatJournalDevice) != 0) {
            m_blockers.append(QStringLiteral("External journal ext volumes are not supported"));
        }
        if ((m_superblock.incompat & kExtIncompatInlineData) != 0) {
            m_blockers.append(QStringLiteral("Inline-data ext volumes are not supported"));
        }
        if ((m_superblock.incompat & kExtIncompatEncrypt) != 0) {
            m_blockers.append(QStringLiteral("Encrypted ext volumes are not supported"));
        }
    }

    void appendUnsupportedRoCompatBlockers() {
        if ((m_superblock.ro_compat & kExtRoCompatBigAlloc) != 0) {
            m_blockers.append(QStringLiteral("Bigalloc ext volumes are not supported"));
        }
    }

    void appendSuperblockWarnings() {
        if ((m_superblock.incompat & kExtIncompatNeedsRecovery) != 0) {
            m_warnings.append(
                QStringLiteral("Ext journal replay is not performed in read-only browser mode"));
        }
    }

    [[nodiscard]] std::optional<QByteArray> readAt(uint64_t offset, qsizetype length) {
        if (length < 0 || offset > static_cast<uint64_t>(std::numeric_limits<qint64>::max())) {
            return std::nullopt;
        }
        if (!m_device->seek(static_cast<qint64>(offset))) {
            return std::nullopt;
        }
        QByteArray bytes = m_device->read(length);
        if (bytes.size() != length) {
            return std::nullopt;
        }
        return bytes;
    }

    [[nodiscard]] std::optional<QByteArray> readBlock(uint64_t blockNumber) {
        uint64_t offset = 0;
        if (!checkedMul(blockNumber, m_superblock.block_size, &offset)) {
            m_blockers.append(QStringLiteral("Ext block offset overflow"));
            return std::nullopt;
        }
        return readAt(offset, static_cast<qsizetype>(m_superblock.block_size));
    }

    [[nodiscard]] std::optional<ExtInode> readInode(uint32_t inodeNumber) {
        if (inodeNumber == 0 || m_superblock.inodes_per_group == 0) {
            m_blockers.append(QStringLiteral("Invalid ext inode reference"));
            return std::nullopt;
        }

        const uint32_t group = (inodeNumber - 1) / m_superblock.inodes_per_group;
        const uint32_t index = (inodeNumber - 1) % m_superblock.inodes_per_group;
        uint64_t descOffset = 0;
        if (!groupDescriptorOffset(group, &descOffset)) {
            return std::nullopt;
        }
        const auto descriptor = readAt(descOffset,
                                       static_cast<qsizetype>(m_superblock.group_desc_size));
        if (!descriptor.has_value()) {
            m_blockers.append(QStringLiteral("Unable to read ext group descriptor"));
            return std::nullopt;
        }

        const uint64_t inodeTableBlock =
            joinLowHigh32(le32(*descriptor, kExtGroupDescInodeTableLoOffset),
                          le32(*descriptor, kExtGroupDescInodeTableHiOffset),
                          (m_superblock.incompat & kExtIncompat64Bit) != 0);
        uint64_t inodeTableOffset = 0;
        uint64_t inodeOffset = 0;
        if (!checkedMul(inodeTableBlock, m_superblock.block_size, &inodeTableOffset) ||
            !checkedAdd(inodeTableOffset,
                        static_cast<uint64_t>(index) * m_superblock.inode_size,
                        &inodeOffset)) {
            m_blockers.append(QStringLiteral("Ext inode offset overflow"));
            return std::nullopt;
        }

        const auto inodeBytes = readAt(inodeOffset,
                                       static_cast<qsizetype>(m_superblock.inode_size));
        if (!inodeBytes.has_value()) {
            m_blockers.append(QStringLiteral("Unable to read ext inode"));
            return std::nullopt;
        }
        return parseInode(inodeNumber, *inodeBytes);
    }

    [[nodiscard]] bool groupDescriptorOffset(uint32_t group, uint64_t* output) const {
        const uint64_t descriptorTableBlock = m_superblock.block_size == kMinimumBlockSize ? 2 : 1;
        uint64_t tableOffset = 0;
        uint64_t groupOffset = 0;
        if (!checkedMul(descriptorTableBlock, m_superblock.block_size, &tableOffset) ||
            !checkedMul(group, m_superblock.group_desc_size, &groupOffset) ||
            !checkedAdd(tableOffset, groupOffset, output)) {
            return false;
        }
        return true;
    }

    [[nodiscard]] ExtInode parseInode(uint32_t inodeNumber, const QByteArray& bytes) const {
        ExtInode inode;
        inode.inode_number = inodeNumber;
        inode.mode = le16(bytes, kExtInodeModeOffset);
        inode.flags = le32(bytes, kExtInodeFlagsOffset);
        inode.allocated_512_byte_blocks = le32(bytes, kExtInodeBlockCountLoOffset);
        inode.size_bytes = joinLowHigh32(le32(bytes, kExtInodeSizeLoOffset),
                                         le32(bytes, kExtInodeSizeHiOffset),
                                         true);
        inode.blocks = bytes.mid(kExtInodeBlocksOffset, kExtInodeBlockBytes);
        return inode;
    }

    [[nodiscard]] std::optional<ExtInode> resolvePath(const QString& path) {
        QStringList blockers;
        const QStringList parts = pathParts(path, &blockers);
        if (!blockers.isEmpty()) {
            m_blockers.append(blockers);
            return std::nullopt;
        }

        auto current = readInode(kRootInode);
        for (const auto& part : parts) {
            if (!current.has_value() || !current->directory()) {
                m_blockers.append(
                    QStringLiteral("Ext path component is not a directory: %1").arg(part));
                return std::nullopt;
            }
            const auto records = readDirectoryRecords(*current, kDefaultPathResolveEntryLimit);
            if (!records.has_value()) {
                return std::nullopt;
            }
            const auto match = std::find_if(records->cbegin(),
                                            records->cend(),
                                            [&part](const DirectoryRecord& record) {
                                                return record.name == part;
                                            });
            if (match == records->cend()) {
                m_blockers.append(QStringLiteral("Ext path not found: %1").arg(part));
                return std::nullopt;
            }
            current = readInode(match->inode);
        }
        return current;
    }

    [[nodiscard]] QString normalizedDisplayPath(const QString& path) const {
        QStringList blockers;
        const QStringList parts = pathParts(path, &blockers);
        if (parts.isEmpty()) {
            return QStringLiteral("/");
        }
        return QStringLiteral("/%1").arg(parts.join(QLatin1Char('/')));
    }

    [[nodiscard]] std::optional<QVector<DirectoryRecord>> readDirectoryRecords(
        const ExtInode& inode, int maxEntries) {
        if (inode.size_bytes > kMaxDirectoryBytes) {
            m_blockers.append(QStringLiteral("Ext directory is too large for read-only listing"));
            return std::nullopt;
        }
        const auto bytes = readInodeData(inode, kMaxDirectoryBytes);
        if (!bytes.has_value()) {
            return std::nullopt;
        }

        QVector<DirectoryRecord> records;
        for (qsizetype blockOffset = 0; blockOffset < bytes->size();
             blockOffset += m_superblock.block_size) {
            parseDirectoryBlock(*bytes, blockOffset, maxEntries, &records);
            if (records.size() >= maxEntries || !m_blockers.isEmpty()) {
                break;
            }
        }
        return m_blockers.isEmpty() ? std::optional<QVector<DirectoryRecord>>(records)
                                    : std::nullopt;
    }

    void parseDirectoryBlock(const QByteArray& bytes,
                             qsizetype blockOffset,
                             int maxEntries,
                             QVector<DirectoryRecord>* records) {
        const qsizetype blockEnd = std::min<qsizetype>(blockOffset + m_superblock.block_size,
                                                       bytes.size());
        qsizetype offset = blockOffset;
        while (offset + kDirEntryHeaderBytes <= blockEnd && records->size() < maxEntries) {
            const uint16_t recLen = le16(bytes, offset + 4);
            const auto nameLen = static_cast<unsigned char>(bytes.at(offset + 6));
            if (!directoryRecordIsValid(offset, blockEnd, recLen, nameLen)) {
                m_blockers.append(QStringLiteral("Invalid ext directory record"));
                return;
            }
            appendDirectoryRecord(bytes, offset, nameLen, records);
            offset += recLen;
        }
    }

    [[nodiscard]] bool directoryRecordIsValid(qsizetype offset,
                                              qsizetype blockEnd,
                                              uint16_t recLen,
                                              unsigned char nameLen) const {
        return recLen >= kDirEntryHeaderBytes && offset + recLen <= blockEnd &&
               nameLen <= recLen - kDirEntryHeaderBytes;
    }

    void appendDirectoryRecord(const QByteArray& bytes,
                               qsizetype offset,
                               unsigned char nameLen,
                               QVector<DirectoryRecord>* records) const {
        const uint32_t inode = le32(bytes, offset);
        if (inode == 0 || nameLen == 0) {
            return;
        }
        const QString name = cleanEntryName(bytes.mid(offset + kDirEntryHeaderBytes, nameLen));
        if (name == QStringLiteral(".") || name == QStringLiteral("..") ||
            name.contains(QLatin1Char('/')) || name.contains(QChar::Null)) {
            return;
        }
        const auto fileType = static_cast<unsigned char>(bytes.at(offset + 7));
        records->append(DirectoryRecord{.name = name, .inode = inode, .file_type = fileType});
    }

    [[nodiscard]] PartitionExtFileEntry entryFor(const DirectoryRecord& record,
                                                 const ExtInode& inode,
                                                 const QString& parentPath) {
        const bool isDirectory = record.file_type == kDirEntryTypeDirectory || inode.directory();
        const bool isRegular = record.file_type == kDirEntryTypeRegular || inode.regularFile();
        const bool isSymlink = record.file_type == kDirEntryTypeSymlink || inode.symlink();
        PartitionExtFileEntry entry{.path = makeEntryPath(parentPath, record.name),
                                    .name = record.name,
                                    .type = typeNameFromMode(inode.mode),
                                    .inode = inode.inode_number,
                                    .size_bytes = inode.size_bytes,
                                    .directory = isDirectory,
                                    .regular_file = isRegular,
                                    .symlink = isSymlink};
        if (isSymlink) {
            entry.symlink_target = readSymlinkTarget(inode).value_or(QString());
        }
        return entry;
    }

    [[nodiscard]] std::optional<QString> readSymlinkTarget(const ExtInode& inode) {
        if (!inode.symlink()) {
            return std::nullopt;
        }
        if (inode.size_bytes > kMaxSymlinkTargetBytes) {
            m_warnings.append(QStringLiteral("Skipped oversized ext symlink target: inode %1")
                                  .arg(inode.inode_number));
            return std::nullopt;
        }

        std::optional<QByteArray> bytes;
        if (inode.size_bytes <= static_cast<uint64_t>(inode.blocks.size()) &&
            inode.allocated_512_byte_blocks == 0) {
            bytes = inode.blocks.left(static_cast<qsizetype>(inode.size_bytes));
        } else {
            bytes = readInodeData(inode, kMaxSymlinkTargetBytes);
        }
        if (!bytes.has_value()) {
            m_warnings.append(QStringLiteral("Unable to read ext symlink target: inode %1")
                                  .arg(inode.inode_number));
            return std::nullopt;
        }
        if (bytes->contains('\0')) {
            m_warnings.append(QStringLiteral("Skipped binary ext symlink target: inode %1")
                                  .arg(inode.inode_number));
            return std::nullopt;
        }
        QString target = QString::fromUtf8(*bytes);
        if (target.contains(QChar::ReplacementCharacter)) {
            target = QString::fromLatin1(*bytes);
        }
        return target;
    }

    [[nodiscard]] std::optional<QByteArray> readInodeData(const ExtInode& inode,
                                                          uint64_t maxBytes) {
        if (!fileSizeWithinReadCap(inode, maxBytes)) {
            m_blockers.append(QStringLiteral("Ext file exceeds configured read cap"));
            return std::nullopt;
        }

        QByteArray output;
        output.reserve(static_cast<qsizetype>(inode.size_bytes));
        const uint64_t blockCount = (inode.size_bytes + m_superblock.block_size - 1) /
                                    m_superblock.block_size;
        const auto extents = optionalExtentMap(inode);
        if (inode.extentMapped() && !extents.has_value()) {
            return std::nullopt;
        }

        for (uint64_t logical = 0; logical < blockCount; ++logical) {
            const auto blockBytes = readLogicalDataBlock(inode, extents, logical);
            if (!blockBytes.has_value()) {
                return std::nullopt;
            }
            appendInodeDataBlock(&output, *blockBytes, inode.size_bytes);
        }
        return output;
    }

    [[nodiscard]] bool fileSizeWithinReadCap(const ExtInode& inode, uint64_t maxBytes) const {
        return inode.size_bytes <= maxBytes &&
               inode.size_bytes <= static_cast<uint64_t>(std::numeric_limits<qsizetype>::max());
    }

    [[nodiscard]] std::optional<QVector<ExtentRecord>> optionalExtentMap(const ExtInode& inode) {
        return inode.extentMapped() ? collectExtents(inode)
                                    : std::optional<QVector<ExtentRecord>>();
    }

    [[nodiscard]] std::optional<QByteArray> readLogicalDataBlock(
        const ExtInode& inode,
        const std::optional<QVector<ExtentRecord>>& extents,
        uint64_t logical) {
        const auto block = inode.extentMapped() ? physicalBlockFromExtents(*extents, logical)
                                                : physicalBlockFromLegacyMap(inode, logical);
        if (!block.has_value()) {
            return std::nullopt;
        }
        if (*block == kZeroPhysicalBlock) {
            return QByteArray(static_cast<qsizetype>(m_superblock.block_size), '\0');
        }
        const auto read = readBlock(*block);
        if (!read.has_value()) {
            m_blockers.append(QStringLiteral("Unable to read ext data block"));
            return std::nullopt;
        }
        return read;
    }

    void appendInodeDataBlock(QByteArray* output,
                              const QByteArray& blockBytes,
                              uint64_t fileSize) const {
        const uint64_t remaining = fileSize - output->size();
        output->append(blockBytes.left(
            static_cast<qsizetype>(std::min<uint64_t>(remaining, m_superblock.block_size))));
    }

    [[nodiscard]] std::optional<uint64_t> physicalBlockFromLegacyMap(const ExtInode& inode,
                                                                     uint64_t logical) {
        if (logical < kExtDirectBlockCount) {
            return le32(inode.blocks, static_cast<qsizetype>(logical) * kUint32Size);
        }

        const uint64_t pointersPerBlock = m_superblock.block_size / kUint32Size;
        if (logical < kExtDirectBlockCount + pointersPerBlock) {
            return indirectPointer(le32(inode.blocks, kExtSingleIndirectIndex * kUint32Size),
                                   logical - kExtDirectBlockCount);
        }

        const uint64_t doubleBase = kExtDirectBlockCount + pointersPerBlock;
        const uint64_t doubleSpan = pointersPerBlock * pointersPerBlock;
        if (logical < doubleBase + doubleSpan) {
            const uint64_t relative = logical - doubleBase;
            const auto first =
                indirectPointer(le32(inode.blocks, kExtDoubleIndirectIndex * kUint32Size),
                                relative / pointersPerBlock);
            if (!first.has_value()) {
                return std::nullopt;
            }
            return indirectPointer(*first, relative % pointersPerBlock);
        }

        m_blockers.append(QStringLiteral("Triple-indirect ext files are not supported yet"));
        return std::nullopt;
    }

    [[nodiscard]] std::optional<uint64_t> indirectPointer(uint64_t blockNumber, uint64_t index) {
        if (blockNumber == 0) {
            return kZeroPhysicalBlock;
        }
        if (index >= static_cast<uint64_t>(m_superblock.block_size / kUint32Size)) {
            m_blockers.append(QStringLiteral("Ext indirect block index is out of range"));
            return std::nullopt;
        }
        const auto bytes = readBlock(blockNumber);
        if (!bytes.has_value()) {
            m_blockers.append(QStringLiteral("Unable to read ext indirect block"));
            return std::nullopt;
        }
        return le32(*bytes, static_cast<qsizetype>(index * kUint32Size));
    }

    [[nodiscard]] std::optional<QVector<ExtentRecord>> collectExtents(const ExtInode& inode) {
        QVector<ExtentRecord> extents;
        if (!collectExtentsFromNode(inode.blocks, 0, &extents)) {
            return std::nullopt;
        }
        std::sort(extents.begin(), extents.end(), [](const auto& left, const auto& right) {
            return left.logical_start < right.logical_start;
        });
        return extents;
    }

    bool collectExtentsFromNode(const QByteArray& node,
                                int recursionDepth,
                                QVector<ExtentRecord>* extents) {
        if (recursionDepth > kExtentTreeMaxDepth || !hasBytes(node, 0, kExtentHeaderBytes) ||
            le16(node, 0) != kExtExtentMagic) {
            m_blockers.append(QStringLiteral("Invalid ext extent tree"));
            return false;
        }

        const uint16_t entries = le16(node, 2);
        const uint16_t depth = le16(node, 6);
        if (!hasBytes(node, kExtentHeaderBytes, entries * kExtentRecordBytes)) {
            m_blockers.append(QStringLiteral("Ext extent entries exceed node size"));
            return false;
        }

        for (uint16_t index = 0; index < entries; ++index) {
            const qsizetype offset = kExtentHeaderBytes + index * kExtentRecordBytes;
            if (depth == 0) {
                appendLeafExtent(node, offset, extents);
                continue;
            }
            if (!collectExtentsFromIndex(node, offset, recursionDepth, extents)) {
                return false;
            }
        }
        return true;
    }

    void appendLeafExtent(const QByteArray& node,
                          qsizetype offset,
                          QVector<ExtentRecord>* extents) const {
        const uint16_t rawLength = le16(node, offset + kExtentLengthOffset);
        const uint64_t physical = joinLowHigh32(le32(node, offset + kExtentPhysicalLowOffset),
                                                le16(node, offset + kExtentPhysicalHighOffset),
                                                true);
        extents->append(
            ExtentRecord{.logical_start = le32(node, offset + kExtentLogicalBlockOffset),
                         .length = static_cast<uint32_t>(rawLength & ~kExtentUninitializedMask),
                         .physical_start = physical,
                         .initialized = (rawLength & kExtentUninitializedMask) == 0});
    }

    bool collectExtentsFromIndex(const QByteArray& node,
                                 qsizetype offset,
                                 int recursionDepth,
                                 QVector<ExtentRecord>* extents) {
        const uint64_t leafBlock = joinLowHigh32(le32(node, offset + kExtentIndexLeafLowOffset),
                                                 le16(node, offset + kExtentIndexLeafHighOffset),
                                                 true);
        const auto leaf = readBlock(leafBlock);
        if (!leaf.has_value()) {
            m_blockers.append(QStringLiteral("Unable to read ext extent leaf"));
            return false;
        }
        return collectExtentsFromNode(*leaf, recursionDepth + 1, extents);
    }

    [[nodiscard]] std::optional<uint64_t> physicalBlockFromExtents(
        const QVector<ExtentRecord>& extents, uint64_t logical) const {
        const auto match =
            std::find_if(extents.cbegin(), extents.cend(), [logical](const ExtentRecord& extent) {
                return logical >= extent.logical_start &&
                       logical < extent.logical_start + extent.length;
            });
        if (match == extents.cend()) {
            return kZeroPhysicalBlock;
        }
        if (!match->initialized) {
            return kZeroPhysicalBlock;
        }
        return match->physical_start + (logical - match->logical_start);
    }

    QIODevice* m_device{nullptr};
    ExtSuperblock m_superblock;
    QStringList m_blockers;
    QStringList m_warnings;
};

PartitionExtFileReadResult withOpenImage(
    const QString& imagePath,
    const std::function<PartitionExtFileReadResult(QIODevice*)>& callback) {
    PartitionExtFileReadResult result;
    if (imagePath.trimmed().isEmpty()) {
        result.blockers.append(QStringLiteral("Image path is required"));
        return result;
    }
    QString openError;
    auto image = openFileOrRawDeviceReadOnly(imagePath, &openError);
    if (!image) {
        result.blockers.append(
            QStringLiteral("Unable to open ext image read-only: %1").arg(openError));
        return result;
    }
    return callback(image.get());
}

QString safeExportName(QString name, const QString& fallbackId) {
    name = name.trimmed();
    static const QRegularExpression unsafeChars(QStringLiteral(R"([<>:"/\\|?*\x00-\x1F])"));
    name.replace(unsafeChars, QStringLiteral("_"));
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
        name.chop(1);
    }
    if (name.isEmpty()) {
        name = QStringLiteral("entry_%1").arg(fallbackId);
    }
    return name.left(kExportNameMaxCharacters);
}

QString uniquePath(const QDir& dir, const QString& safeName, const QString& suffixId) {
    QString candidate = dir.filePath(safeName);
    if (!QFileInfo::exists(candidate)) {
        return candidate;
    }

    const QFileInfo info(safeName);
    const QString base = info.completeBaseName().isEmpty() ? safeName : info.completeBaseName();
    const QString suffix = info.suffix().isEmpty() ? QString()
                                                   : QStringLiteral(".%1").arg(info.suffix());
    for (int index = kFirstExportNameCollisionIndex; index < kMaxExportNameCollisionAttempts;
         ++index) {
        candidate =
            dir.filePath(QStringLiteral("%1_%2_%3%4").arg(base, suffixId).arg(index).arg(suffix));
        if (!QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

bool writeExportFile(const QString& path,
                     const QByteArray& data,
                     QStringList* blockers,
                     const QString& label) {
    QFile output(path);
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        output.write(data) != data.size()) {
        blockers->append(QStringLiteral("Unable to write exported %1: %2").arg(label, path));
        return false;
    }
    return true;
}

struct ExtExportFrame {
    QString source_path;
    QString output_directory;
};

void appendExtExportRequestBlockers(const QString& imagePath,
                                    const QString& outputDirectory,
                                    const PartitionExtDirectoryExportOptions& options,
                                    QStringList* blockers) {
    if (imagePath.trimmed().isEmpty()) {
        blockers->append(QStringLiteral("Image path is required"));
    }
    if (outputDirectory.trimmed().isEmpty()) {
        blockers->append(QStringLiteral("Output directory is required"));
    }
    if (options.max_entries <= 0) {
        blockers->append(QStringLiteral("Export entry cap must be positive"));
    }
    if (options.max_file_bytes == 0 || options.max_total_bytes == 0) {
        blockers->append(QStringLiteral("Export byte caps must be positive"));
    }
}

class ExtDirectoryExporter {
public:
    ExtDirectoryExporter(QIODevice* image, PartitionExtDirectoryExportOptions options)
        : image_(image), options_(options) {}

    PartitionExtDirectoryExportResult run(const QString& sourcePath,
                                          const QString& outputDirectory) {
        pending_.append(
            {sourcePath.trimmed().isEmpty() ? QStringLiteral("/") : sourcePath.trimmed(),
             outputDirectory});
        while (!pending_.isEmpty()) {
            if (!processFrame(pending_.takeLast())) {
                break;
            }
        }
        result_.ok = result_.blockers.isEmpty();
        return result_;
    }

private:
    bool processFrame(const ExtExportFrame& frame) {
        const QString visitKey = frame.source_path.toLower();
        if (visited_directories_.contains(visitKey)) {
            return true;
        }
        visited_directories_.insert(visitKey);

        const auto listing = PartitionExtFileSystemReader::listDirectory(
            image_, frame.source_path, std::max(1, options_.max_entries - result_.entries_scanned));
        result_.warnings.append(listing.warnings);
        if (!listing.ok) {
            result_.blockers.append(listing.blockers);
            return false;
        }
        return processEntries(listing.entries, QDir(frame.output_directory));
    }

    bool processEntries(const QVector<PartitionExtFileEntry>& entries, const QDir& targetDir) {
        for (const auto& entry : entries) {
            if (!processEntry(entry, targetDir)) {
                return false;
            }
        }
        return true;
    }

    bool processEntry(const PartitionExtFileEntry& entry, const QDir& targetDir) {
        if (!consumeEntrySlot()) {
            return false;
        }
        const QString suffixId = QString::number(entry.inode, kExportNameFallbackBase);
        const QString safeName = safeExportName(entry.name, suffixId);
        const QString targetPath = uniquePath(targetDir, safeName, suffixId);
        if (targetPath.isEmpty()) {
            result_.blockers.append(
                QStringLiteral("Unable to allocate unique output path for %1").arg(entry.path));
            return false;
        }
        if (entry.directory) {
            return exportDirectory(entry, targetPath);
        }
        if (entry.symlink) {
            return exportSymlink(entry, targetDir, safeName, suffixId);
        }
        if (!entry.regular_file) {
            result_.warnings.append(
                QStringLiteral("Skipped unsupported ext entry: %1").arg(entry.path));
            return true;
        }
        return exportFile(entry, targetPath);
    }

    bool consumeEntrySlot() {
        if (result_.entries_scanned >= options_.max_entries) {
            result_.blockers.append(QStringLiteral("Export entry cap reached"));
            return false;
        }
        ++result_.entries_scanned;
        return true;
    }

    bool exportDirectory(const PartitionExtFileEntry& entry, const QString& targetPath) {
        if (!QDir().mkpath(targetPath)) {
            result_.blockers.append(
                QStringLiteral("Unable to create exported directory: %1").arg(targetPath));
            return false;
        }
        ++result_.directories_exported;
        pending_.append({entry.path, targetPath});
        return true;
    }

    bool exportSymlink(const PartitionExtFileEntry& entry,
                       const QDir& targetDir,
                       const QString& safeName,
                       const QString& suffixId) {
        if (entry.symlink_target.isEmpty()) {
            result_.warnings.append(
                QStringLiteral("Skipped ext symlink with missing target: %1").arg(entry.path));
            return true;
        }
        const QByteArray targetBytes = entry.symlink_target.toUtf8();
        if (!fitsByteCaps(static_cast<uint64_t>(targetBytes.size()), entry.path)) {
            return false;
        }
        const QString sidecarName = QStringLiteral("%1.symlink.txt").arg(safeName);
        const QString sidecarPath = uniquePath(targetDir, sidecarName, suffixId);
        if (sidecarPath.isEmpty()) {
            result_.blockers.append(
                QStringLiteral("Unable to allocate unique symlink sidecar for %1").arg(entry.path));
            return false;
        }
        if (!writeExportFile(sidecarPath, targetBytes, &result_.blockers, entry.path)) {
            return false;
        }
        ++result_.symlinks_exported;
        result_.bytes_exported += static_cast<uint64_t>(targetBytes.size());
        return true;
    }

    bool exportFile(const PartitionExtFileEntry& entry, const QString& targetPath) {
        if (!fitsByteCaps(entry.size_bytes, entry.path)) {
            return false;
        }
        const auto file =
            PartitionExtFileSystemReader::readFile(image_, entry.path, options_.max_file_bytes);
        result_.warnings.append(file.warnings);
        if (!file.ok) {
            result_.blockers.append(file.blockers);
            return false;
        }
        if (!writeExportFile(targetPath, file.data, &result_.blockers, entry.path)) {
            return false;
        }
        ++result_.files_exported;
        result_.bytes_exported += static_cast<uint64_t>(file.data.size());
        return true;
    }

    bool fitsByteCaps(uint64_t entryBytes, const QString& entryPath) {
        const bool wouldOverflowTotal = result_.bytes_exported > options_.max_total_bytes ||
                                        entryBytes >
                                            options_.max_total_bytes - result_.bytes_exported;
        if (entryBytes > options_.max_file_bytes || wouldOverflowTotal) {
            result_.blockers.append(
                QStringLiteral("Export byte cap reached before %1").arg(entryPath));
            return false;
        }
        return true;
    }

    QIODevice* image_;
    PartitionExtDirectoryExportOptions options_;
    PartitionExtDirectoryExportResult result_;
    QVector<ExtExportFrame> pending_;
    QSet<QString> visited_directories_;
};

}  // namespace

PartitionExtFileReadResult PartitionExtFileSystemReader::listDirectory(QIODevice* device,
                                                                       const QString& path,
                                                                       int max_entries) {
    ExtReader reader(device);
    if (!reader.load()) {
        PartitionExtFileReadResult result;
        result.blockers.append(QStringLiteral("Unable to open ext filesystem for listing"));
        return result;
    }
    return reader.listDirectory(path, max_entries);
}

PartitionExtFileReadResult PartitionExtFileSystemReader::listDirectoryFromImage(
    const QString& image_path, const QString& path, int max_entries) {
    return withOpenImage(image_path, [path, max_entries](QIODevice* device) {
        return PartitionExtFileSystemReader::listDirectory(device, path, max_entries);
    });
}

PartitionExtFileReadResult PartitionExtFileSystemReader::readFile(QIODevice* device,
                                                                  const QString& path,
                                                                  uint64_t max_bytes) {
    ExtReader reader(device);
    if (!reader.load()) {
        PartitionExtFileReadResult result;
        result.blockers.append(QStringLiteral("Unable to open ext filesystem for reading"));
        return result;
    }
    return reader.readFile(path, max_bytes);
}

PartitionExtFileReadResult PartitionExtFileSystemReader::readFileFromImage(
    const QString& image_path, const QString& path, uint64_t max_bytes) {
    return withOpenImage(image_path, [path, max_bytes](QIODevice* device) {
        return PartitionExtFileSystemReader::readFile(device, path, max_bytes);
    });
}

PartitionExtDirectoryExportResult PartitionExtFileSystemReader::exportDirectoryFromImage(
    const QString& image_path,
    const QString& source_path,
    const QString& output_directory,
    const PartitionExtDirectoryExportOptions& options) {
    PartitionExtDirectoryExportResult exportResult;
    appendExtExportRequestBlockers(image_path, output_directory, options, &exportResult.blockers);
    if (!exportResult.blockers.isEmpty()) {
        return exportResult;
    }

    QString openError;
    auto image = openFileOrRawDeviceReadOnly(image_path, &openError);
    if (!image) {
        exportResult.blockers.append(
            QStringLiteral("Unable to open ext image read-only: %1").arg(openError));
        return exportResult;
    }

    QDir root(output_directory);
    if (!root.mkpath(QStringLiteral("."))) {
        exportResult.blockers.append(QStringLiteral("Unable to create output directory"));
        return exportResult;
    }

    return ExtDirectoryExporter(image.get(), options).run(source_path, root.absolutePath());
}

}  // namespace sak

// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_apfs_file_system_reader.cpp
/// @brief Read-only APFS file browser for Partition Manager.

#include "sak/partition_apfs_file_system_reader.h"

#include "sak/apfs_compression.h"
#include "sak/apfs_crypto.h"
#include "sak/apfs_keybag.h"
#include "sak/partition_apfs_writer.h"
#include "sak/partition_raw_device_io.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <functional>
#include <limits>
#include <optional>

namespace sak {

namespace {

constexpr qsizetype kApfsObjectHeaderBytes = 0x20;
constexpr qsizetype kApfsObjectOidOffset = 0x08;
constexpr qsizetype kApfsObjectXidOffset = 0x10;
constexpr qsizetype kApfsObjectTypeOffset = 0x18;
constexpr qsizetype kApfsObjectSubtypeOffset = 0x1C;
constexpr qsizetype kApfsObjectMagicOffset = 0x20;
constexpr qsizetype kApfsBtreeNodeHeaderBytes = 0x38;
constexpr qsizetype kApfsBtreeInfoBytes = 40;
constexpr qsizetype kApfsBtreeInfoNodeSizeOffset = 4;
constexpr qsizetype kApfsBtreeInfoKeySizeOffset = 8;
constexpr qsizetype kApfsBtreeInfoValueSizeOffset = 12;
constexpr qsizetype kApfsBtreeInfoKeyCountOffset = 24;
constexpr qsizetype kApfsBtreeInfoNodeCountOffset = 32;
constexpr qsizetype kApfsBtreeNodeFlagsOffset = 0x20;
constexpr qsizetype kApfsBtreeNodeLevelOffset = 0x22;
constexpr qsizetype kApfsBtreeNodeCountOffset = 0x24;
constexpr qsizetype kApfsBtreeNodeTableOffsetOffset = 0x28;
constexpr qsizetype kApfsBtreeNodeTableLengthOffset = 0x2A;
constexpr qsizetype kApfsBtreeFixedTocEntryBytes = 4;
constexpr qsizetype kApfsBtreeVariableTocEntryBytes = 8;
constexpr qsizetype kApfsBtreeFixedTocValueOffset = 2;
constexpr qsizetype kApfsBtreeVariableTocKeyLengthOffset = 2;
constexpr qsizetype kApfsBtreeVariableTocValueOffset = 4;
constexpr qsizetype kApfsBtreeVariableTocValueLengthOffset = 6;
constexpr qsizetype kApfsBtreeChildPointerBytes = 8;
constexpr uint32_t kApfsBtreeMaxEntryCount = 100'000;
constexpr uint32_t kApfsMagicNxsb = 0x42'53'58'4E;  // NXSB
constexpr uint32_t kApfsMagicApsb = 0x42'53'50'41;  // APSB
constexpr uint32_t kApfsObjectTypeMask = 0x00'00'FF'FF;
constexpr uint32_t kApfsObjectTypeNxSuperblock = 0x00'00'00'01;
constexpr uint32_t kApfsObjectTypeBtree = 0x00'00'00'02;
constexpr uint32_t kApfsObjectTypeBtreeNode = 0x00'00'00'03;
constexpr uint32_t kApfsObjectTypeObjectMap = 0x00'00'00'0B;
constexpr uint32_t kApfsObjectTypeCheckpointMap = 0x00'00'00'0C;
constexpr uint32_t kApfsObjectTypeFs = 0x00'00'00'0D;
constexpr uint32_t kApfsObjectSubtypeFsTree = 0x00'00'00'0E;
constexpr uint32_t kApfsObjectPhysical = 0x40'00'00'00;
constexpr uint16_t kApfsBtreeNodeRoot = 0x0001;
constexpr uint16_t kApfsBtreeNodeLeaf = 0x0002;
constexpr uint16_t kApfsBtreeNodeFixedKvSize = 0x0004;
constexpr uint32_t kApfsBtreePhysical = 0x00'00'00'10;
constexpr uint32_t kApfsContainerIncompatVersion1 = 0x00'00'00'01;
constexpr uint32_t kApfsContainerIncompatVersion2 = 0x00'00'00'02;
constexpr uint32_t kApfsContainerIncompatFusion = 0x00'00'01'00;
constexpr uint64_t kApfsSupportedContainerIncompat = kApfsContainerIncompatVersion2 |
                                                     kApfsContainerIncompatFusion;
constexpr uint64_t kApfsVolumeIncompatIncompleteRestore = 0x00'00'00'10;
constexpr uint64_t kApfsObjIdMask = 0x0F'FF'FF'FF'FF'FF'FF'FFULL;
constexpr uint64_t kApfsObjTypeMask = 0xF0'00'00'00'00'00'00'00ULL;
constexpr int kApfsObjTypeShift = 60;
constexpr uint8_t kApfsRecordInode = 3;
constexpr uint8_t kApfsRecordXattr = 4;
constexpr uint8_t kApfsRecordFileExtent = 8;
constexpr uint8_t kApfsRecordDirectoryEntry = 9;
// j_xattr_key: 8-byte header then le16 name_len; j_xattr_val: le16 flags + le16
// xdata_len + xdata.
constexpr qsizetype kApfsXattrKeyNameLenOffset = 8;
constexpr qsizetype kApfsXattrKeyNameOffset = 10;
constexpr qsizetype kApfsXattrValueFlagsOffset = 0;
constexpr qsizetype kApfsXattrValueXdataLenOffset = 2;
constexpr qsizetype kApfsXattrValueXdataOffset = 4;
constexpr uint16_t kApfsDirTypeDirectory = 4;
constexpr uint16_t kApfsDirTypeRegularFile = 8;
constexpr uint16_t kApfsDirTypeSymlink = 10;
constexpr uint64_t kApfsRootDirectoryId = 2;
constexpr uint8_t kApfsInodeDstreamField = 8;
constexpr qsizetype kApfsInodePrivateIdOffset = 0x08;
constexpr qsizetype kApfsInodeModeOffset = 0x50;
constexpr qsizetype kApfsInodeXfieldsOffset = 0x5C;
constexpr uint16_t kApfsModeTypeMask = 0170000;
constexpr uint16_t kApfsModeDirectory = 0040000;
constexpr uint16_t kApfsModeRegularFile = 0100000;
constexpr uint16_t kApfsModeSymlink = 0120000;
constexpr uint64_t kApfsFileExtentLengthMask = 0x00'FF'FF'FF'FF'FF'FF'FFULL;
constexpr uint64_t kApfsFileExtentFlagMask = 0xFF'00'00'00'00'00'00'00ULL;
constexpr qsizetype kApfsFileExtentKeyBytes = 16;
constexpr qsizetype kApfsFileExtentValueBytes = 24;
constexpr qsizetype kApfsFileExtentKeyLogicalOffset = 8;
constexpr qsizetype kApfsFileExtentValuePhysicalBlockOffset = 8;
constexpr qsizetype kApfsFileExtentValueCryptoIdOffset = 16;
constexpr uint32_t kApfsOmapValueDeleted = 0x00'00'00'01;
constexpr uint32_t kApfsOmapValueEncrypted = 0x00'00'00'04;
constexpr uint32_t kApfsOmapValueNoHeader = 0x00'00'00'08;
constexpr qsizetype kApfsOmapKeyBytes = 16;
constexpr qsizetype kApfsOmapKeyXidOffset = 8;
constexpr qsizetype kApfsOmapValueBytes = 16;
constexpr qsizetype kApfsOmapValueSizeOffset = 4;
constexpr qsizetype kApfsOmapValuePaddrOffset = 8;
constexpr qsizetype kApfsNxBlockSizeOffset = 0x24;
constexpr qsizetype kApfsNxBlockCountOffset = 0x28;
constexpr qsizetype kApfsNxIncompatibleFeaturesOffset = 0x40;
constexpr qsizetype kApfsNxNextXidOffset = 0x60;
constexpr qsizetype kApfsNxDescBlocksOffset = 0x68;
constexpr qsizetype kApfsNxDataBlocksOffset = 0x6C;
constexpr qsizetype kApfsNxDescBaseOffset = 0x70;
constexpr qsizetype kApfsNxDataBaseOffset = 0x78;
constexpr qsizetype kApfsNxDescIndexOffset = 0x88;
constexpr qsizetype kApfsNxDescLenOffset = 0x8C;
constexpr qsizetype kApfsNxOmapOidOffset = 0xA0;
constexpr qsizetype kApfsNxMaxFileSystemsOffset = 0xB4;
constexpr qsizetype kApfsNxFsOidArrayOffset = 0xB8;
constexpr qsizetype kApfsObjectIdBytes = 8;
constexpr int kApfsMaxFileSystems = 100;
constexpr qsizetype kApfsOmapFlagsOffset = 0x20;
constexpr qsizetype kApfsOmapTreeOidOffset = 0x30;
constexpr qsizetype kApfsVolumeIncompatibleFeaturesOffset = 0x38;
constexpr qsizetype kApfsVolumeOmapOidOffset = 0x80;
constexpr qsizetype kApfsVolumeRootTreeOidOffset = 0x88;
// A6 read-side decrypt: container UUID + keylocker prange (NXSB) and per-volume
// UUID + fs-flags (APSB) drive the keybag-chain VEK derivation.
constexpr qsizetype kApfsUuidBytes = 16;
constexpr qsizetype kApfsNxUuidOffset = 0x48;
constexpr qsizetype kApfsNxKeylockerPaddrOffset = 0x510;
constexpr qsizetype kApfsVolumeUuidOffset = 0xF0;
constexpr qsizetype kApfsVolumeFsFlagsOffset = 0x108;
constexpr uint64_t kApfsVolumeFsFlagUnencrypted = 0x1;
constexpr uint64_t kApfsVolumeFsFlagOneKey = 0x8;
constexpr qsizetype kApfsVolumeFlagsOffset = 0x198;
constexpr qsizetype kApfsVolumeNameOffset = 0x2C0;
constexpr qsizetype kApfsVolumeNameBytes = 256;
constexpr uint32_t kApfsCheckpointMapLast = 0x00'00'00'01;
constexpr uint32_t kApfsCheckpointCountNonContiguousMask = 0x80'00'00'00;
constexpr uint64_t kApfsInitialProbeBytes = 4096;
constexpr uint64_t kApfsMinBlockSize = 512;
constexpr uint64_t kApfsBlockSizeAlignment = 512;
constexpr uint64_t kMaxApfsBlockSize = 1024ULL * 1024ULL;
constexpr int kMaxCheckpointDescriptorBlocks = 65'536;
constexpr int kMaxObjectMapDepth = 16;
constexpr int kMaxFsTreeDepth = 16;
constexpr int kMaxFsTreeNodes = 20'000;
constexpr int kMaxFsTreeRecords = 300'000;
constexpr uint64_t kMaxFileReadBytes = 512ULL * 1024ULL * 1024ULL;
constexpr int kExportNameFallbackBase = 10;
constexpr int kUniqueExportNameFirstSuffix = 2;
constexpr int kMaxUniqueExportNameAttempts = 10'000;
constexpr qsizetype kApfsFsKeyBytes = 8;
constexpr qsizetype kApfsDrecMinValueBytes = 18;
constexpr qsizetype kApfsDrecHashedNameOffset = 12;
constexpr qsizetype kApfsDrecHashedKeyMinBytes = 12;
constexpr qsizetype kApfsDrecNameLengthOffset = 8;
constexpr uint32_t kApfsDrecNameLengthMask = 0x3FF;
constexpr qsizetype kApfsDrecLegacyNameOffset = 10;
constexpr qsizetype kApfsDrecTypeOffset = 16;
constexpr uint16_t kApfsDrecTypeMask = 0x000F;
constexpr qsizetype kApfsXfieldHeaderBytes = 4;
constexpr qsizetype kApfsXfieldUsedBytesOffset = 2;
constexpr uint16_t kApfsMaxXfieldCount = 64;
constexpr qsizetype kApfsXfieldTocEntryBytes = 4;
constexpr qsizetype kApfsXfieldSizeOffset = 2;
constexpr qsizetype kApfsXfieldAlignment = 8;
constexpr qsizetype kApfsXfieldAlignmentPadding = 7;
constexpr qsizetype kApfsDstreamMinBytes = 40;
constexpr int kDisplayHexBase = 16;

uint16_t le16(const QByteArray& bytes, qsizetype offset) {
    if (offset < 0 || offset + static_cast<qsizetype>(sizeof(uint16_t)) > bytes.size()) {
        return 0;
    }
    return qFromLittleEndian<uint16_t>(reinterpret_cast<const uchar*>(bytes.constData() + offset));
}

uint32_t le32(const QByteArray& bytes, qsizetype offset) {
    if (offset < 0 || offset + static_cast<qsizetype>(sizeof(uint32_t)) > bytes.size()) {
        return 0;
    }
    return qFromLittleEndian<uint32_t>(reinterpret_cast<const uchar*>(bytes.constData() + offset));
}

uint64_t le64(const QByteArray& bytes, qsizetype offset) {
    if (offset < 0 || offset + static_cast<qsizetype>(sizeof(uint64_t)) > bytes.size()) {
        return 0;
    }
    return qFromLittleEndian<uint64_t>(reinterpret_cast<const uchar*>(bytes.constData() + offset));
}

QString utf8Field(const QByteArray& bytes, qsizetype offset, qsizetype length) {
    if (offset < 0 || length <= 0 || offset >= bytes.size()) {
        return {};
    }
    const qsizetype clamped = std::min(length, bytes.size() - offset);
    const QByteArray raw = bytes.mid(offset, clamped);
    const int terminator = raw.indexOf('\0');
    return QString::fromUtf8(terminator >= 0 ? raw.left(terminator) : raw).trimmed();
}

QString cleanPath(const QString& path) {
    QString value = path.trimmed();
    if (value.isEmpty() || value == QStringLiteral("/")) {
        return QStringLiteral("/");
    }
    value.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (value.contains(QStringLiteral("//"))) {
        value.replace(QStringLiteral("//"), QStringLiteral("/"));
    }
    if (!value.startsWith(QLatin1Char('/'))) {
        value.prepend(QLatin1Char('/'));
    }
    while (value.size() > 1 && value.endsWith(QLatin1Char('/'))) {
        value.chop(1);
    }
    return value;
}

QStringList pathParts(const QString& path) {
    const QString cleaned = cleanPath(path);
    if (cleaned == QStringLiteral("/")) {
        return {};
    }
    return cleaned.mid(1).split(QLatin1Char('/'), Qt::SkipEmptyParts);
}

QString childPath(const QString& parent, const QString& name) {
    const QString cleanedParent = cleanPath(parent);
    return cleanedParent == QStringLiteral("/") ? QStringLiteral("/%1").arg(name)
                                                : QStringLiteral("%1/%2").arg(cleanedParent, name);
}

QString entryTypeName(uint16_t directoryType, uint16_t mode) {
    if (directoryType == kApfsDirTypeDirectory ||
        (mode & kApfsModeTypeMask) == kApfsModeDirectory) {
        return QStringLiteral("Directory");
    }
    if (directoryType == kApfsDirTypeRegularFile ||
        (mode & kApfsModeTypeMask) == kApfsModeRegularFile) {
        return QStringLiteral("File");
    }
    if (directoryType == kApfsDirTypeSymlink || (mode & kApfsModeTypeMask) == kApfsModeSymlink) {
        return QStringLiteral("Symlink");
    }
    return QStringLiteral("Other");
}

QString safeExportName(QString name, const QString& fallbackId) {
    name = name.trimmed();
    static const QSet<QChar> kInvalidNameChars{QLatin1Char('<'),
                                               QLatin1Char('>'),
                                               QLatin1Char(':'),
                                               QLatin1Char('"'),
                                               QLatin1Char('/'),
                                               QLatin1Char('\\'),
                                               QLatin1Char('|'),
                                               QLatin1Char('?'),
                                               QLatin1Char('*')};
    for (auto& ch : name) {
        if (kInvalidNameChars.contains(ch) || ch.category() == QChar::Other_Control) {
            ch = QLatin1Char('_');
        }
    }
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
        name.chop(1);
    }
    return name.isEmpty() ? QStringLiteral("apfs-entry-%1").arg(fallbackId) : name;
}

QString uniquePath(const QDir& dir, const QString& safeName, const QString& suffixId) {
    QString candidate = dir.filePath(safeName);
    if (!QFileInfo::exists(candidate)) {
        return candidate;
    }
    const QFileInfo info(safeName);
    const QString base = info.completeBaseName().isEmpty() ? safeName : info.completeBaseName();
    const QString extension = info.suffix().isEmpty() ? QString()
                                                      : QStringLiteral(".%1").arg(info.suffix());
    for (int index = kUniqueExportNameFirstSuffix; index < kMaxUniqueExportNameAttempts; ++index) {
        candidate = dir.filePath(
            QStringLiteral("%1-%2-%3%4").arg(base).arg(suffixId).arg(index).arg(extension));
        if (!QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

bool writeExportFile(const QString& path,
                     const QByteArray& data,
                     QStringList* blockers,
                     const QString& sourcePath) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(data) != data.size()) {
        blockers->append(
            QStringLiteral("Unable to write exported APFS file for %1").arg(sourcePath));
        return false;
    }
    return true;
}

struct ApfsObjectHeader {
    uint64_t oid{0};
    uint64_t xid{0};
    uint32_t type{0};
    uint32_t subtype{0};

    [[nodiscard]] uint32_t baseType() const { return type & kApfsObjectTypeMask; }
};

ApfsObjectHeader objectHeader(const QByteArray& block) {
    return {le64(block, kApfsObjectOidOffset),
            le64(block, kApfsObjectXidOffset),
            le32(block, kApfsObjectTypeOffset),
            le32(block, kApfsObjectSubtypeOffset)};
}

struct BtreeInfo {
    uint32_t flags{0};
    uint32_t node_size{0};
    uint32_t key_size{0};
    uint32_t value_size{0};
    uint64_t key_count{0};
    uint64_t node_count{0};
};

struct BtreeEntryView {
    qsizetype key_offset{0};
    qsizetype key_length{0};
    qsizetype value_offset{0};
    qsizetype value_length{0};
};

struct BtreeEntryContext {
    qsizetype key_area_start{0};
    qsizetype value_area_end{0};
    uint16_t level{0};
    BtreeInfo info;
};

struct ObjectMapValue {
    uint32_t flags{0};
    uint32_t size{0};
    uint64_t physical_address{0};
    uint64_t xid{0};
    bool encrypted{false};  // OMAP_VAL_ENCRYPTED: target block is AES-XTS(VEK)
};

struct ObjectMapState {
    uint64_t root_address{0};
    uint32_t flags{0};
    bool physical_tree{true};
};

struct DirectoryRecord {
    QString name;
    uint64_t parent_id{0};
    uint64_t file_id{0};
    uint16_t directory_type{0};
};

struct InodeRecord {
    uint64_t object_id{0};
    uint64_t private_id{0};
    uint64_t size{0};
    uint16_t mode{0};
};

struct FileExtentRecord {
    uint64_t owner_id{0};
    uint64_t logical_offset{0};
    uint64_t length{0};
    uint64_t physical_block{0};
    uint64_t flags{0};
    uint64_t crypto_id{0};
};

struct FileReadTarget {
    InodeRecord inode;
    uint64_t bytes_to_read{0};
    // A5: a transparently-compressed file's content comes from its decmpfs
    // attribute (decmpfs_xattr), not data-stream extents.
    bool compressed{false};
    QByteArray decmpfs_xattr;
};

struct FsTreeScanState {
    BtreeInfo info;
    QSet<uint64_t> seen_nodes;
    int nodes_visited{0};
    int records_visited{0};
};

class ApfsReader {
public:
    explicit ApfsReader(QIODevice* device, QString credential = {})
        : device_(device), credential_(std::move(credential)) {}

    [[nodiscard]] PartitionApfsFileReadResult listDirectory(const QString& path, int maxEntries) {
        PartitionApfsFileReadResult result;
        result.file_system = QStringLiteral("APFS");
        if (!mount(&result)) {
            return result;
        }
        if (!scanFileSystem(&result)) {
            return result;
        }

        const QString normalized = cleanPath(path);
        const auto directoryId = resolveDirectory(normalized, &result);
        if (!directoryId.has_value()) {
            return result;
        }

        QVector<DirectoryRecord> entries = directoryRecordsFor(*directoryId);
        std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
            return QString::localeAwareCompare(left.name, right.name) < 0;
        });

        const int limit = maxEntries <= 0 ? kPartitionApfsDefaultBrowseEntryLimit : maxEntries;
        for (const auto& record : entries) {
            if (result.entries.size() >= limit) {
                result.warnings.append(
                    QStringLiteral("APFS listing truncated at %1 entries").arg(limit));
                break;
            }
            result.entries.append(entryFromRecord(record, normalized));
        }
        result.volume_name = volumeName_;
        result.ok = result.blockers.isEmpty();
        return result;
    }

    [[nodiscard]] PartitionApfsFileReadResult readFile(const QString& path, uint64_t maxBytes) {
        PartitionApfsFileReadResult result;
        result.file_system = QStringLiteral("APFS");
        if (!mountAndScan(&result)) {
            return result;
        }

        const auto target = resolveFileReadTarget(path, maxBytes, &result);
        if (!target.has_value()) {
            return result;
        }
        if (!appendFileData(*target, &result)) {
            return result;
        }
        result.volume_name = volumeName_;
        result.ok = result.blockers.isEmpty();
        return result;
    }

private:
    [[nodiscard]] bool mountAndScan(PartitionApfsFileReadResult* result) {
        return mount(result) && scanFileSystem(result);
    }

    [[nodiscard]] bool readInitialContainerBlock(QByteArray* firstBlock,
                                                 PartitionApfsFileReadResult* result) {
        if (!readBytes(0, kApfsInitialProbeBytes, firstBlock, result)) {
            return false;
        }
        if (le32(*firstBlock, kApfsObjectMagicOffset) != kApfsMagicNxsb) {
            result->blockers.append(QStringLiteral("APFS container superblock was not found"));
            return false;
        }

        blockSize_ = le32(*firstBlock, kApfsNxBlockSizeOffset);
        blockCount_ = le64(*firstBlock, kApfsNxBlockCountOffset);
        if (!validateBlockGeometry(result)) {
            return false;
        }
        if (firstBlock->size() < static_cast<qsizetype>(blockSize_)) {
            return readBlock(0, firstBlock, result);
        }
        if (!validateObjectBlockChecksum(0, *firstBlock, result)) {
            return false;
        }
        return true;
    }

    [[nodiscard]] bool validateBlockGeometry(PartitionApfsFileReadResult* result) const {
        if (blockSize_ < kApfsMinBlockSize || blockSize_ > kMaxApfsBlockSize ||
            blockSize_ % kApfsBlockSizeAlignment != 0) {
            result->blockers.append(
                QStringLiteral("APFS block size %1 is outside supported bounds").arg(blockSize_));
            return false;
        }
        return true;
    }

    [[nodiscard]] bool validateContainerFeatures(const QByteArray& nxBlock,
                                                 PartitionApfsFileReadResult* result) const {
        const uint64_t incompatible = le64(nxBlock, kApfsNxIncompatibleFeaturesOffset);
        if ((incompatible & kApfsContainerIncompatVersion1) != 0) {
            result->blockers.append(QStringLiteral("APFS version 1 containers are unsupported"));
            return false;
        }
        if ((incompatible & ~kApfsSupportedContainerIncompat) != 0) {
            result->blockers.append(QStringLiteral("APFS container has unsupported incompatible "
                                                   "features 0x%1")
                                        .arg(incompatible, 0, kDisplayHexBase));
            return false;
        }
        return true;
    }

    [[nodiscard]] std::optional<ObjectMapValue> resolveVolumeMapping(
        const QByteArray& nxBlock, PartitionApfsFileReadResult* result) {
        const auto fsOid = firstVolumeOid(nxBlock);
        if (!fsOid.has_value()) {
            result->blockers.append(QStringLiteral("APFS container has no volume OID"));
            return std::nullopt;
        }

        ObjectMapState containerOmap;
        const uint64_t containerOmapAddress = le64(nxBlock, kApfsNxOmapOidOffset);
        if (!loadObjectMap(containerOmapAddress, &containerOmap, result)) {
            return std::nullopt;
        }
        const auto volumeMapping = objectMapLookup(containerOmap, *fsOid, containerXid_, result);
        if (!volumeMapping.has_value()) {
            result->blockers.append(
                QStringLiteral("APFS volume OID %1 was not found in container object map")
                    .arg(*fsOid));
        }
        return volumeMapping;
    }

    [[nodiscard]] bool loadVolumeSuperblock(const ObjectMapValue& volumeMapping,
                                            QByteArray* volumeBlock,
                                            PartitionApfsFileReadResult* result) {
        if (!readBlock(volumeMapping.physical_address, volumeBlock, result)) {
            return false;
        }
        const auto volumeHeader = objectHeader(*volumeBlock);
        if (volumeHeader.baseType() != kApfsObjectTypeFs ||
            le32(*volumeBlock, kApfsObjectMagicOffset) != kApfsMagicApsb) {
            result->blockers.append(QStringLiteral("APFS volume superblock is invalid"));
            return false;
        }
        volumeXid_ = volumeHeader.xid;
        return true;
    }

    [[nodiscard]] bool initializeVolumeState(const QByteArray& volumeBlock,
                                             PartitionApfsFileReadResult* result) {
        rootTreeOid_ = le64(volumeBlock, kApfsVolumeRootTreeOidOffset);
        volumeName_ = utf8Field(volumeBlock, kApfsVolumeNameOffset, kApfsVolumeNameBytes);
        if (volumeName_.isEmpty()) {
            volumeName_ = QStringLiteral("APFS");
        }
        const uint64_t volumeIncompatible = le64(volumeBlock,
                                                 kApfsVolumeIncompatibleFeaturesOffset);
        if ((volumeIncompatible & kApfsVolumeIncompatIncompleteRestore) != 0) {
            result->blockers.append(QStringLiteral("APFS volume has incomplete-restore state"));
            return false;
        }
        if (!unlockEncryptedVolume(volumeBlock, result)) {
            return false;
        }
        return loadVolumeObjectMap(volumeBlock, result) && loadRootTree(result);
    }

    // A6 read-side decrypt: if the volume's fs-flags clear APFS_FS_UNENCRYPTED, walk
    // the keybag chain with the supplied credential to recover the volume key (VEK).
    // The credential loop is credential-agnostic, so a personal-recovery-key unlock
    // record is tried the same way as the password record. Unencrypted volumes and
    // the empty-credential locked case are handled without touching the keybags.
    [[nodiscard]] bool unlockEncryptedVolume(const QByteArray& volumeBlock,
                                             PartitionApfsFileReadResult* result) {
        const uint64_t fsFlags = le64(volumeBlock, kApfsVolumeFsFlagsOffset);
        // Software-encrypted only when the UNENCRYPTED flag is clear AND the
        // container keylocker references a keybag. A cleared flag with no keylocker
        // carries no key material (not an unlockable FileVault volume), so the
        // fs-tree omap flags alone drive any per-object decrypt and we read as-is.
        if ((fsFlags & kApfsVolumeFsFlagUnencrypted) != 0 || keylockerPaddr_ == 0) {
            return true;
        }
        volumeOneKey_ = (fsFlags & kApfsVolumeFsFlagOneKey) != 0;
        if (credential_.isEmpty()) {
            result->blockers.append(
                QStringLiteral("APFS volume is encrypted; a password or recovery key is required"));
            return false;
        }
        const QByteArray volumeUuid = volumeBlock.mid(kApfsVolumeUuidOffset, kApfsUuidBytes);
        const auto vek = deriveVolumeKey(volumeUuid, result);
        if (!vek.has_value()) {
            return false;
        }
        vek_ = *vek;
        return true;
    }

    // Read a keybag block (raw ciphertext on disk) and AES-XTS-decrypt it with
    // uuid||uuid (tweak base = block * 8), then verify the recovered object checksum.
    [[nodiscard]] std::optional<QByteArray> readKeybagBlock(uint64_t paddr,
                                                            const QByteArray& uuid,
                                                            PartitionApfsFileReadResult* result) {
        QByteArray cipher;
        if (!readBlock(paddr, &cipher, result, true, false)) {
            return std::nullopt;
        }
        QByteArray plain = sak::apfs_crypto::xtsDecryptBlock(uuid + uuid, paddr, cipher);
        if (plain.size() != static_cast<qsizetype>(blockSize_) ||
            !PartitionApfsWriter::verifyObjectChecksum(plain)) {
            result->blockers.append(
                QStringLiteral("APFS keybag block %1 failed to decrypt").arg(paddr));
            return std::nullopt;
        }
        return plain;
    }

    // Walk container keybag -> volume keybag -> KEK -> VEK for @p volumeUuid.
    [[nodiscard]] std::optional<QByteArray> deriveVolumeKey(const QByteArray& volumeUuid,
                                                            PartitionApfsFileReadResult* result) {
        using namespace sak::apfs_keybag;
        const auto containerKb = readKeybagBlock(keylockerPaddr_, containerUuid_, result);
        if (!containerKb.has_value()) {
            return std::nullopt;
        }
        QByteArray vekBlob;
        uint64_t volumeKbPaddr = 0;
        for (const auto& entry : parseKeybagBlock(*containerKb)) {
            if (entry.uuid != volumeUuid) {
                continue;
            }
            if (entry.tag == kKbTagVolumeKey) {
                vekBlob = entry.keydata;
            } else if (entry.tag == kKbTagVolumeUnlockRecords && entry.keydata.size() >= 8) {
                volumeKbPaddr = qFromLittleEndian<quint64>(
                    reinterpret_cast<const uchar*>(entry.keydata.constData()));
            }
        }
        if (vekBlob.isEmpty() || volumeKbPaddr == 0) {
            result->blockers.append(QStringLiteral("APFS container keybag has no volume key"));
            return std::nullopt;
        }
        return unwrapVolumeKey(volumeUuid, volumeKbPaddr, vekBlob, result);
    }

    // Try every unlock record in the volume keybag with the supplied credential
    // (password or recovery key) until one unwraps the KEK and then the VEK.
    [[nodiscard]] std::optional<QByteArray> unwrapVolumeKey(const QByteArray& volumeUuid,
                                                            uint64_t volumeKbPaddr,
                                                            const QByteArray& vekBlob,
                                                            PartitionApfsFileReadResult* result) {
        using namespace sak::apfs_keybag;
        const auto volumeKb = readKeybagBlock(volumeKbPaddr, volumeUuid, result);
        if (!volumeKb.has_value()) {
            return std::nullopt;
        }
        KeyBlobParams vekParams;
        if (!parseKeyBlob(vekBlob, &vekParams)) {
            result->blockers.append(QStringLiteral("APFS volume key blob is malformed"));
            return std::nullopt;
        }
        for (const auto& entry : parseKeybagBlock(*volumeKb)) {
            if (entry.tag != kKbTagVolumeUnlockRecords) {
                continue;
            }
            const auto vek = tryUnlockRecord(entry.keydata, vekParams.wrappedKey);
            if (vek.has_value()) {
                return vek;
            }
        }
        result->blockers.append(
            QStringLiteral("APFS volume password or recovery key is incorrect"));
        return std::nullopt;
    }

    // One credential attempt: PBKDF2(credential, salt, iters) -> unwrap KEK ->
    // unwrap VEK. Returns nullopt when the credential does not match this record.
    [[nodiscard]] std::optional<QByteArray> tryUnlockRecord(const QByteArray& kekBlob,
                                                            const QByteArray& wrappedVek) const {
        using namespace sak::apfs_crypto;
        sak::apfs_keybag::KeyBlobParams kek;
        if (!sak::apfs_keybag::parseKeyBlob(kekBlob, &kek) || kek.iterations == 0 ||
            kek.salt.isEmpty()) {
            return std::nullopt;
        }
        const QByteArray derived =
            pbkdf2Sha256(credential_.toUtf8(), kek.salt, kek.iterations, kApfsKekBytes);
        const auto unwrappedKek = aesKeyUnwrap(derived, kek.wrappedKey);
        if (!unwrappedKek.has_value()) {
            return std::nullopt;
        }
        const auto vek = aesKeyUnwrap(*unwrappedKek, wrappedVek);
        if (!vek.has_value() || vek->size() != kApfsUnwrappedKeyBytes) {
            return std::nullopt;
        }
        return *vek;
    }

    [[nodiscard]] bool loadVolumeObjectMap(const QByteArray& volumeBlock,
                                           PartitionApfsFileReadResult* result) {
        const uint64_t volumeOmapAddress = le64(volumeBlock, kApfsVolumeOmapOidOffset);
        return loadObjectMap(volumeOmapAddress, &volumeOmap_, result);
    }

    [[nodiscard]] bool loadRootTree(PartitionApfsFileReadResult* result) {
        const auto rootMapping = objectMapLookup(volumeOmap_, rootTreeOid_, volumeXid_, result);
        if (!rootMapping.has_value()) {
            result->blockers.append(QStringLiteral("APFS root file-system tree was not found"));
            return false;
        }
        rootTreeAddress_ = rootMapping->physical_address;
        rootTreeEncrypted_ = rootMapping->encrypted;
        return true;
    }

    [[nodiscard]] std::optional<FileReadTarget> resolveFileReadTarget(
        const QString& path, uint64_t maxBytes, PartitionApfsFileReadResult* result) const {
        const QString normalized = cleanPath(path);
        const auto record = resolveFile(normalized, result);
        if (!record.has_value()) {
            return std::nullopt;
        }
        const auto inode = inodeById_.constFind(record->file_id);
        if (inode == inodeById_.cend()) {
            result->blockers.append(
                QStringLiteral("APFS inode %1 was not found").arg(record->file_id));
            return std::nullopt;
        }
        if ((inode->mode & kApfsModeTypeMask) != kApfsModeRegularFile &&
            record->directory_type != kApfsDirTypeRegularFile) {
            result->blockers.append(QStringLiteral("APFS path is not a regular file"));
            return std::nullopt;
        }

        const uint64_t effectiveMax =
            maxBytes == 0 ? kMaxFileReadBytes : std::min<uint64_t>(maxBytes, kMaxFileReadBytes);
        // A compressed file has no data stream; its logical size is the decmpfs
        // header's uncompressed_size, decoded from the attribute on read.
        const auto decmpfs = decmpfsByInode_.constFind(record->file_id);
        if (decmpfs != decmpfsByInode_.cend()) {
            const auto header = apfsParseDecmpfsHeader(*decmpfs);
            if (!header.has_value()) {
                result->blockers.append(QStringLiteral("APFS decmpfs attribute is malformed"));
                return std::nullopt;
            }
            const uint64_t logical = header->uncompressed_size;
            const uint64_t bytesToRead = std::min<uint64_t>(logical, effectiveMax);
            if (logical > bytesToRead) {
                result->warnings.append(
                    QStringLiteral("APFS file read truncated at %1 bytes").arg(bytesToRead));
            }
            return FileReadTarget{*inode, bytesToRead, true, *decmpfs};
        }
        const uint64_t bytesToRead = std::min<uint64_t>(inode->size, effectiveMax);
        if (inode->size > bytesToRead) {
            result->warnings.append(
                QStringLiteral("APFS file read truncated at %1 bytes").arg(bytesToRead));
        }
        return FileReadTarget{*inode, bytesToRead};
    }

    // Decode a transparently-compressed file's content from its inline decmpfs
    // attribute and append the requested prefix. The macOS kernel performs the
    // identical reconstruction; this is the reader's byte-match decode (A5).
    [[nodiscard]] bool appendCompressedFileData(const FileReadTarget& target,
                                                PartitionApfsFileReadResult* result) {
        const auto header = apfsParseDecmpfsHeader(target.decmpfs_xattr);
        if (!header.has_value()) {
            result->blockers.append(QStringLiteral("APFS decmpfs attribute is malformed"));
            return false;
        }
        if (!apfsDecmpfsAlgoIsInline(header->algo)) {
            result->blockers.append(
                QStringLiteral("APFS resource-fork compression (algorithm %1) read is not yet "
                               "supported; the inline decmpfs path is certified")
                    .arg(header->algo));
            return false;
        }
        const auto decoded = apfsDecodeInlineDecmpfs(target.decmpfs_xattr);
        if (!decoded.has_value()) {
            result->blockers.append(
                QStringLiteral("APFS decmpfs decode failed for algorithm %1").arg(header->algo));
            return false;
        }
        result->data = decoded->left(static_cast<qsizetype>(target.bytes_to_read));
        return true;
    }

    [[nodiscard]] QVector<FileExtentRecord> sortedExtents(uint64_t privateId) const {
        QVector<FileExtentRecord> extents = extentsByOwner_.values(privateId).toVector();
        std::sort(extents.begin(), extents.end(), [](const auto& left, const auto& right) {
            return left.logical_offset < right.logical_offset;
        });
        return extents;
    }

    [[nodiscard]] bool appendFileData(const FileReadTarget& target,
                                      PartitionApfsFileReadResult* result) {
        if (target.compressed) {
            return appendCompressedFileData(target, result);
        }
        const auto extents = sortedExtents(target.inode.private_id);
        if (target.bytes_to_read > 0 && extents.isEmpty()) {
            result->blockers.append(QStringLiteral("APFS file has no readable extents"));
            return false;
        }
        result->data.reserve(static_cast<qsizetype>(std::min<uint64_t>(
            target.bytes_to_read, static_cast<uint64_t>(std::numeric_limits<int>::max()))));

        uint64_t cursor = 0;
        for (const auto& extent : extents) {
            if (!appendReadableExtent(extent, target.bytes_to_read, &cursor, result)) {
                return false;
            }
        }
        if (cursor < target.bytes_to_read) {
            result->blockers.append(QStringLiteral("APFS file extents ended before expected size"));
            return false;
        }
        return true;
    }

    [[nodiscard]] bool appendReadableExtent(const FileExtentRecord& extent,
                                            uint64_t bytesToRead,
                                            uint64_t* cursor,
                                            PartitionApfsFileReadResult* result) {
        if (!cursor || *cursor >= bytesToRead) {
            return true;
        }
        if (extent.flags != 0) {
            result->blockers.append(QStringLiteral("APFS file extent flags are not supported"));
            return false;
        }
        // Encrypted extents are readable for a ONEKEY whole-volume FileVault volume
        // once the VEK is unlocked (data tweak = physical block addr, like metadata);
        // per-file-key (non-ONEKEY) crypto stays fail-closed.
        if (extent.crypto_id != 0 && !(volumeOneKey_ && !vek_.isEmpty())) {
            result->blockers.append(
                QStringLiteral("APFS encrypted/tweaked file extents are not supported"));
            return false;
        }
        appendSparsePrefix(extent, bytesToRead, cursor, result);
        if (*cursor >= bytesToRead || extent.logical_offset + extent.length <= *cursor) {
            return true;
        }

        const uint64_t extentOffset = *cursor - extent.logical_offset;
        const uint64_t readable = std::min<uint64_t>(extent.length - extentOffset,
                                                     bytesToRead - *cursor);
        if (!appendExtentBytes(extent, extentOffset, readable, result)) {
            return false;
        }
        *cursor += readable;
        return true;
    }

    void appendSparsePrefix(const FileExtentRecord& extent,
                            uint64_t bytesToRead,
                            uint64_t* cursor,
                            PartitionApfsFileReadResult* result) const {
        if (!cursor || extent.logical_offset <= *cursor) {
            return;
        }
        const uint64_t sparseBytes = std::min<uint64_t>(extent.logical_offset - *cursor,
                                                        bytesToRead - *cursor);
        result->data.append(QByteArray(static_cast<int>(sparseBytes), '\0'));
        *cursor += sparseBytes;
    }

    [[nodiscard]] bool mount(PartitionApfsFileReadResult* result) {
        if (!device_ || !device_->isOpen()) {
            result->blockers.append(QStringLiteral("APFS input is not open"));
            return false;
        }

        QByteArray firstBlock;
        if (!readInitialContainerBlock(&firstBlock, result)) {
            return false;
        }

        QByteArray nxBlock = latestContainerSuperblock(firstBlock, result);
        const auto nxHeader = objectHeader(nxBlock);
        containerXid_ = nxHeader.xid;
        containerUuid_ = nxBlock.mid(kApfsNxUuidOffset, kApfsUuidBytes);
        keylockerPaddr_ = le64(nxBlock, kApfsNxKeylockerPaddrOffset);
        if (!validateContainerFeatures(nxBlock, result)) {
            return false;
        }

        const auto volumeMapping = resolveVolumeMapping(nxBlock, result);
        if (!volumeMapping.has_value()) {
            return false;
        }

        QByteArray volumeBlock;
        if (!loadVolumeSuperblock(*volumeMapping, &volumeBlock, result) ||
            !initializeVolumeState(volumeBlock, result)) {
            return false;
        }
        return true;
    }

    [[nodiscard]] QByteArray latestContainerSuperblock(const QByteArray& firstBlock,
                                                       PartitionApfsFileReadResult* result) {
        const uint32_t descBlockCountRaw = le32(firstBlock, kApfsNxDescBlocksOffset);
        const uint32_t dataBlockCountRaw = le32(firstBlock, kApfsNxDataBlocksOffset);
        if ((descBlockCountRaw & kApfsCheckpointCountNonContiguousMask) != 0 ||
            (dataBlockCountRaw & kApfsCheckpointCountNonContiguousMask) != 0) {
            result->warnings.append(QStringLiteral(
                "APFS non-contiguous checkpoint areas are not traversed; block-zero copy used"));
            return firstBlock;
        }

        const uint32_t descBlockCount = descBlockCountRaw;
        if (descBlockCount == 0 || descBlockCount > kMaxCheckpointDescriptorBlocks) {
            return firstBlock;
        }
        const uint64_t descBase = le64(firstBlock, kApfsNxDescBaseOffset);
        QByteArray best = bestCheckpointSuperblock(firstBlock, descBlockCount, descBase, result);
        if (!checkpointMapTerminatorObserved(best, descBlockCount, descBase, result)) {
            result->warnings.append(
                QStringLiteral("APFS latest checkpoint map terminator was not observed"));
        }
        return best;
    }

    [[nodiscard]] QByteArray bestCheckpointSuperblock(const QByteArray& firstBlock,
                                                      uint32_t descBlockCount,
                                                      uint64_t descBase,
                                                      PartitionApfsFileReadResult* result) {
        QByteArray best = firstBlock;
        uint64_t bestXid = objectHeader(firstBlock).xid;
        for (uint32_t index = 0; index < descBlockCount; ++index) {
            QByteArray block;
            if (!readBlock(descBase + index, &block, result, false)) {
                continue;
            }
            const auto header = objectHeader(block);
            if (header.baseType() == kApfsObjectTypeNxSuperblock &&
                le32(block, kApfsObjectMagicOffset) == kApfsMagicNxsb && header.xid >= bestXid) {
                best = block;
                bestXid = header.xid;
            }
        }
        return best;
    }

    [[nodiscard]] bool checkpointMapTerminatorObserved(const QByteArray& best,
                                                       uint32_t descBlockCount,
                                                       uint64_t descBase,
                                                       PartitionApfsFileReadResult* result) {
        const uint32_t descIndex = le32(best, kApfsNxDescIndexOffset);
        const uint32_t descLen = le32(best, kApfsNxDescLenOffset);
        if (descLen == 0 || descLen >= descBlockCount) {
            return true;
        }
        for (uint32_t offset = 0; offset < descLen; ++offset) {
            const uint64_t ringIndex = (static_cast<uint64_t>(descIndex) + offset) %
                                       static_cast<uint64_t>(descBlockCount);
            QByteArray block;
            if (!readBlock(descBase + ringIndex, &block, result, false)) {
                continue;
            }
            if (objectHeader(block).baseType() == kApfsObjectTypeCheckpointMap &&
                (le32(block, kApfsObjectMagicOffset) & kApfsCheckpointMapLast) != 0) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::optional<uint64_t> firstVolumeOid(const QByteArray& nxBlock) const {
        const uint32_t maxFileSystems =
            std::min<uint32_t>(le32(nxBlock, kApfsNxMaxFileSystemsOffset), kApfsMaxFileSystems);
        for (uint32_t index = 0; index < maxFileSystems; ++index) {
            const uint64_t oid =
                le64(nxBlock,
                     kApfsNxFsOidArrayOffset + static_cast<qsizetype>(index) * kApfsObjectIdBytes);
            if (oid != 0) {
                return oid;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool loadObjectMap(uint64_t physicalAddress,
                                     ObjectMapState* state,
                                     PartitionApfsFileReadResult* result) {
        QByteArray block;
        if (!readBlock(physicalAddress, &block, result)) {
            return false;
        }
        const auto header = objectHeader(block);
        if (header.baseType() != kApfsObjectTypeObjectMap) {
            result->blockers.append(
                QStringLiteral("APFS object map %1 is invalid").arg(physicalAddress));
            return false;
        }
        state->flags = le32(block, kApfsOmapFlagsOffset);
        state->root_address = le64(block, kApfsOmapTreeOidOffset);
        QByteArray root;
        if (!readBlock(state->root_address, &root, result)) {
            return false;
        }
        const BtreeInfo info = btreeInfo(root);
        state->physical_tree = (info.flags & kApfsBtreePhysical) != 0;
        if (!state->physical_tree) {
            result->blockers.append(
                QStringLiteral("APFS object map tree is not physical; unsupported layout"));
            return false;
        }
        return true;
    }

    [[nodiscard]] std::optional<ObjectMapValue> objectMapLookup(
        const ObjectMapState& state,
        uint64_t oid,
        uint64_t xid,
        PartitionApfsFileReadResult* result) {
        QByteArray root;
        if (!readBlock(state.root_address, &root, result)) {
            return std::nullopt;
        }
        const BtreeInfo info = btreeInfo(root);
        QByteArray node = root;
        for (int depth = 0; depth < kMaxObjectMapDepth; ++depth) {
            const uint16_t level = le16(node, kApfsBtreeNodeLevelOffset);
            const auto entries = btreeEntries(node, info);
            if (entries.isEmpty()) {
                result->blockers.append(QStringLiteral("APFS object-map B-tree node is empty"));
                return std::nullopt;
            }
            if (level == 0) {
                return usableObjectMapValue(bestObjectMapLeafValue(node, entries, oid, xid));
            }

            const auto childAddress = objectMapChildAddress(node, entries, oid, xid);
            if (!readBlock(*childAddress, &node, result)) {
                return std::nullopt;
            }
        }
        result->blockers.append(QStringLiteral("APFS object-map B-tree depth limit exceeded"));
        return std::nullopt;
    }

    [[nodiscard]] std::optional<ObjectMapValue> bestObjectMapLeafValue(
        const QByteArray& node,
        const QVector<BtreeEntryView>& entries,
        uint64_t oid,
        uint64_t xid) const {
        std::optional<ObjectMapValue> best;
        for (const auto& entry : entries) {
            if (!isObjectMapLeafEntry(entry)) {
                continue;
            }
            const uint64_t keyOid = le64(node, entry.key_offset);
            const uint64_t keyXid = le64(node, entry.key_offset + kApfsOmapKeyXidOffset);
            if (keyOid == oid && keyXid <= xid && (!best.has_value() || keyXid > best->xid)) {
                const uint32_t valueFlags = le32(node, entry.value_offset);
                best = ObjectMapValue{valueFlags,
                                      le32(node, entry.value_offset + kApfsOmapValueSizeOffset),
                                      le64(node, entry.value_offset + kApfsOmapValuePaddrOffset),
                                      keyXid,
                                      (valueFlags & kApfsOmapValueEncrypted) != 0};
            }
        }
        return best;
    }

    [[nodiscard]] bool isObjectMapLeafEntry(const BtreeEntryView& entry) const {
        return entry.key_length >= kApfsOmapKeyBytes && entry.value_length >= kApfsOmapValueBytes;
    }

    [[nodiscard]] std::optional<ObjectMapValue> usableObjectMapValue(
        const std::optional<ObjectMapValue>& value) const {
        if (!value.has_value()) {
            return std::nullopt;
        }
        // OMAP_VAL_ENCRYPTED is usable once the VEK is unlocked (readBlock decrypts
        // the target with AES-XTS); without a key it stays fail-closed.
        uint32_t blockedFlags = kApfsOmapValueDeleted | kApfsOmapValueNoHeader;
        if (vek_.isEmpty()) {
            blockedFlags |= kApfsOmapValueEncrypted;
        }
        if ((value->flags & blockedFlags) != 0) {
            return std::nullopt;
        }
        return value;
    }

    [[nodiscard]] std::optional<uint64_t> objectMapChildAddress(
        const QByteArray& node,
        const QVector<BtreeEntryView>& entries,
        uint64_t oid,
        uint64_t xid) const {
        std::optional<uint64_t> childAddress;
        for (const auto& entry : entries) {
            if (!isObjectMapChildEntry(entry)) {
                continue;
            }
            const uint64_t keyOid = le64(node, entry.key_offset);
            const uint64_t keyXid = le64(node, entry.key_offset + kApfsOmapKeyXidOffset);
            if (std::make_pair(keyOid, keyXid) <= std::make_pair(oid, xid)) {
                childAddress = le64(node, entry.value_offset);
            }
        }
        if (!childAddress.has_value() && !entries.isEmpty()) {
            childAddress = le64(node, entries.first().value_offset);
        }
        return childAddress;
    }

    [[nodiscard]] bool isObjectMapChildEntry(const BtreeEntryView& entry) const {
        return entry.key_length >= kApfsOmapKeyBytes &&
               entry.value_length >= kApfsBtreeChildPointerBytes;
    }

    [[nodiscard]] bool scanFileSystem(PartitionApfsFileReadResult* result) {
        if (fileSystemScanned_) {
            return true;
        }
        QByteArray root;
        if (!readDecryptedNode(rootTreeAddress_, rootTreeEncrypted_, &root, result)) {
            return false;
        }
        const auto rootHeader = objectHeader(root);
        if (rootHeader.baseType() != kApfsObjectTypeBtree ||
            rootHeader.subtype != kApfsObjectSubtypeFsTree) {
            result->blockers.append(QStringLiteral("APFS root file-system tree is invalid"));
            return false;
        }
        FsTreeScanState state{btreeInfo(root), {}, 0, 0};
        if (!visitFileSystemTreeNode(rootTreeAddress_, rootTreeEncrypted_, 0, &state, result)) {
            return false;
        }
        fileSystemScanned_ = true;
        return true;
    }

    [[nodiscard]] bool visitFileSystemTreeNode(uint64_t paddr,
                                               bool encrypted,
                                               int depth,
                                               FsTreeScanState* state,
                                               PartitionApfsFileReadResult* result) {
        if (depth > kMaxFsTreeDepth) {
            result->blockers.append(QStringLiteral("APFS file-system tree depth limit exceeded"));
            return false;
        }
        if (state->seen_nodes.contains(paddr)) {
            return true;
        }
        if (++state->nodes_visited > kMaxFsTreeNodes) {
            result->blockers.append(QStringLiteral("APFS file-system tree node limit exceeded"));
            return false;
        }
        state->seen_nodes.insert(paddr);
        QByteArray node;
        if (!readDecryptedNode(paddr, encrypted, &node, result)) {
            return false;
        }
        const auto entries = btreeEntries(node, state->info);
        const uint16_t level = le16(node, kApfsBtreeNodeLevelOffset);
        return level == 0 ? processFileSystemLeaf(node, entries, state, result)
                          : visitFileSystemChildren(node, entries, depth, state, result);
    }

    [[nodiscard]] bool processFileSystemLeaf(const QByteArray& node,
                                             const QVector<BtreeEntryView>& entries,
                                             FsTreeScanState* state,
                                             PartitionApfsFileReadResult* result) {
        for (const auto& entry : entries) {
            if (++state->records_visited > kMaxFsTreeRecords) {
                result->blockers.append(QStringLiteral("APFS file-system record limit exceeded"));
                return false;
            }
            parseFileSystemRecord(node, entry);
        }
        return true;
    }

    [[nodiscard]] bool visitFileSystemChildren(const QByteArray& node,
                                               const QVector<BtreeEntryView>& entries,
                                               int depth,
                                               FsTreeScanState* state,
                                               PartitionApfsFileReadResult* result) {
        for (const auto& entry : entries) {
            if (entry.value_length < kApfsBtreeChildPointerBytes) {
                continue;
            }
            if (!visitMappedFileSystemChild(node, entry, depth, state, result)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool visitMappedFileSystemChild(const QByteArray& node,
                                                  const BtreeEntryView& entry,
                                                  int depth,
                                                  FsTreeScanState* state,
                                                  PartitionApfsFileReadResult* result) {
        const uint64_t childOid = le64(node, entry.value_offset);
        const auto childMapping = objectMapLookup(volumeOmap_, childOid, volumeXid_, result);
        if (!childMapping.has_value()) {
            result->blockers.append(
                QStringLiteral("APFS child B-tree node %1 was not found").arg(childOid));
            return false;
        }
        return visitFileSystemTreeNode(
            childMapping->physical_address, childMapping->encrypted, depth + 1, state, result);
    }

    void parseFileSystemRecord(const QByteArray& node, const BtreeEntryView& entry) {
        if (entry.key_length < kApfsFsKeyBytes) {
            return;
        }
        const uint64_t key = le64(node, entry.key_offset);
        const uint64_t objectId = key & kApfsObjIdMask;
        const auto recordType = static_cast<uint8_t>((key & kApfsObjTypeMask) >> kApfsObjTypeShift);
        if (recordType == kApfsRecordDirectoryEntry) {
            parseDirectoryRecord(node, entry, objectId);
        } else if (recordType == kApfsRecordInode) {
            parseInodeRecord(node, entry, objectId);
        } else if (recordType == kApfsRecordFileExtent) {
            parseFileExtentRecord(node, entry, objectId);
        } else if (recordType == kApfsRecordXattr) {
            parseXattrRecord(node, entry, objectId);
        }
    }

    // Capture an embedded com.apple.decmpfs attribute so a compressed file's
    // logical content can be reconstructed from the attribute instead of the
    // (absent) data stream. Resource-fork compression (dstream-backed) is a
    // documented read follow-on; this handles the inline-xattr case.
    void parseXattrRecord(const QByteArray& node, const BtreeEntryView& entry, uint64_t objectId) {
        if (entry.key_length < kApfsXattrKeyNameOffset ||
            entry.value_length < kApfsXattrValueXdataOffset) {
            return;
        }
        const uint16_t nameLen = le16(node, entry.key_offset + kApfsXattrKeyNameLenOffset);
        if (nameLen == 0 || entry.key_offset + kApfsXattrKeyNameOffset + nameLen >
                                entry.key_offset + entry.key_length) {
            return;
        }
        const qsizetype payloadLen = std::max<qsizetype>(0, nameLen - 1);
        const QString name = QString::fromUtf8(
            node.mid(entry.key_offset + kApfsXattrKeyNameOffset,
                     std::min<qsizetype>(
                         payloadLen, node.size() - (entry.key_offset + kApfsXattrKeyNameOffset))));
        if (name != QLatin1StringView(kApfsXattrNameCompressed)) {
            return;
        }
        const uint16_t flags = le16(node, entry.value_offset + kApfsXattrValueFlagsOffset);
        const uint16_t xdataLen = le16(node, entry.value_offset + kApfsXattrValueXdataLenOffset);
        if ((flags & kApfsXattrDataEmbedded) == 0) {
            // Dstream-backed (resource-fork) decmpfs is a documented follow-on; the
            // inline-embedded case below is the certified A5 read path.
            return;
        }
        if (entry.value_offset + kApfsXattrValueXdataOffset + xdataLen >
            entry.value_offset + entry.value_length) {
            return;
        }
        decmpfsByInode_.insert(objectId,
                               node.mid(entry.value_offset + kApfsXattrValueXdataOffset, xdataLen));
    }

    void parseDirectoryRecord(const QByteArray& node,
                              const BtreeEntryView& entry,
                              uint64_t parentId) {
        if (entry.value_length < kApfsDrecMinValueBytes) {
            return;
        }
        qsizetype nameOffset = entry.key_offset + kApfsDrecHashedNameOffset;
        qsizetype nameLength = 0;
        if (entry.key_length >= kApfsDrecHashedKeyMinBytes) {
            nameLength = le32(node, entry.key_offset + kApfsDrecNameLengthOffset) &
                         kApfsDrecNameLengthMask;
        }
        if (nameLength == 0 || nameOffset + nameLength > entry.key_offset + entry.key_length) {
            nameOffset = entry.key_offset + kApfsDrecLegacyNameOffset;
            nameLength = le16(node, entry.key_offset + kApfsDrecNameLengthOffset);
        }
        if (nameLength <= 0 || nameOffset >= node.size()) {
            return;
        }
        const qsizetype payloadLength = std::max<qsizetype>(0, nameLength - 1);
        QString name = QString::fromUtf8(
            node.mid(nameOffset, std::min(payloadLength, node.size() - nameOffset)));
        if (name.isEmpty()) {
            return;
        }
        DirectoryRecord record;
        record.parent_id = parentId;
        record.name = name;
        record.file_id = le64(node, entry.value_offset);
        record.directory_type = le16(node, entry.value_offset + kApfsDrecTypeOffset) &
                                kApfsDrecTypeMask;
        directoryRecords_.append(record);
    }

    void parseInodeRecord(const QByteArray& node, const BtreeEntryView& entry, uint64_t objectId) {
        if (entry.value_length < kApfsInodeXfieldsOffset) {
            return;
        }
        InodeRecord record;
        record.object_id = objectId;
        record.private_id = le64(node, entry.value_offset + kApfsInodePrivateIdOffset);
        record.mode = le16(node, entry.value_offset + kApfsInodeModeOffset);
        record.size = inodeDstreamSize(node, entry);
        inodeById_.insert(record.object_id, record);
    }

    uint64_t inodeDstreamSize(const QByteArray& node, const BtreeEntryView& entry) const {
        const qsizetype blobOffset = entry.value_offset + kApfsInodeXfieldsOffset;
        if (blobOffset + kApfsXfieldHeaderBytes > entry.value_offset + entry.value_length) {
            return 0;
        }
        const uint16_t count = le16(node, blobOffset);
        const uint16_t usedBytes = le16(node, blobOffset + kApfsXfieldUsedBytesOffset);
        if (count > kApfsMaxXfieldCount || usedBytes > entry.value_length) {
            return 0;
        }
        // Extended-field offsets are aligned relative to the start of the
        // inode value (values are packed unaligned within the node, matching
        // Apple's on-disk layout).
        const qsizetype metadataRelative = kApfsInodeXfieldsOffset + kApfsXfieldHeaderBytes;
        qsizetype dataRelative =
            ((metadataRelative + static_cast<qsizetype>(count) * kApfsXfieldTocEntryBytes +
              kApfsXfieldAlignmentPadding) /
             kApfsXfieldAlignment) *
            kApfsXfieldAlignment;
        for (uint16_t index = 0; index < count; ++index) {
            const qsizetype fieldRelative = metadataRelative + static_cast<qsizetype>(index) *
                                                                   kApfsXfieldTocEntryBytes;
            if (fieldRelative + kApfsXfieldTocEntryBytes > entry.value_length) {
                return 0;
            }
            const qsizetype fieldOffset = entry.value_offset + fieldRelative;
            const uint8_t type = static_cast<uint8_t>(node.at(fieldOffset));
            const uint16_t size = le16(node, fieldOffset + kApfsXfieldSizeOffset);
            if (dataRelative + size > entry.value_length) {
                return 0;
            }
            if (type == kApfsInodeDstreamField && size >= kApfsDstreamMinBytes) {
                return le64(node, entry.value_offset + dataRelative);
            }
            dataRelative =
                ((dataRelative + size + kApfsXfieldAlignmentPadding) / kApfsXfieldAlignment) *
                kApfsXfieldAlignment;
        }
        return 0;
    }

    void parseFileExtentRecord(const QByteArray& node,
                               const BtreeEntryView& entry,
                               uint64_t ownerId) {
        if (entry.key_length < kApfsFileExtentKeyBytes ||
            entry.value_length < kApfsFileExtentValueBytes) {
            return;
        }
        FileExtentRecord record;
        record.owner_id = ownerId;
        record.logical_offset = le64(node, entry.key_offset + kApfsFileExtentKeyLogicalOffset);
        const uint64_t lenAndFlags = le64(node, entry.value_offset);
        record.length = lenAndFlags & kApfsFileExtentLengthMask;
        record.flags = lenAndFlags & kApfsFileExtentFlagMask;
        record.physical_block = le64(node,
                                     entry.value_offset + kApfsFileExtentValuePhysicalBlockOffset);
        record.crypto_id = le64(node, entry.value_offset + kApfsFileExtentValueCryptoIdOffset);
        if (record.length > 0) {
            extentsByOwner_.insert(record.owner_id, record);
        }
    }

    [[nodiscard]] QVector<DirectoryRecord> directoryRecordsFor(uint64_t parentId) const {
        QVector<DirectoryRecord> records;
        for (const auto& record : directoryRecords_) {
            if (record.parent_id == parentId) {
                records.append(record);
            }
        }
        return records;
    }

    [[nodiscard]] std::optional<uint64_t> resolveDirectory(
        const QString& path, PartitionApfsFileReadResult* result) const {
        uint64_t current = kApfsRootDirectoryId;
        for (const auto& part : pathParts(path)) {
            const auto records = directoryRecordsFor(current);
            auto match = std::find_if(records.cbegin(), records.cend(), [&](const auto& record) {
                return record.name.compare(part, Qt::CaseInsensitive) == 0;
            });
            if (match == records.cend()) {
                result->blockers.append(
                    QStringLiteral("APFS directory path not found: %1").arg(path));
                return std::nullopt;
            }
            if (match->directory_type != kApfsDirTypeDirectory) {
                result->blockers.append(
                    QStringLiteral("APFS path is not a directory: %1").arg(path));
                return std::nullopt;
            }
            current = match->file_id;
        }
        return current;
    }

    [[nodiscard]] std::optional<DirectoryRecord> resolveFile(
        const QString& path, PartitionApfsFileReadResult* result) const {
        const auto parts = pathParts(path);
        if (parts.isEmpty()) {
            result->blockers.append(QStringLiteral("APFS file path is required"));
            return std::nullopt;
        }
        uint64_t parent = kApfsRootDirectoryId;
        for (int index = 0; index < parts.size() - 1; ++index) {
            const auto records = directoryRecordsFor(parent);
            auto match = std::find_if(records.cbegin(), records.cend(), [&](const auto& record) {
                return record.name.compare(parts.at(index), Qt::CaseInsensitive) == 0;
            });
            if (match == records.cend() || match->directory_type != kApfsDirTypeDirectory) {
                result->blockers.append(
                    QStringLiteral("APFS parent directory not found: %1").arg(path));
                return std::nullopt;
            }
            parent = match->file_id;
        }
        const auto records = directoryRecordsFor(parent);
        auto match = std::find_if(records.cbegin(), records.cend(), [&](const auto& record) {
            return record.name.compare(parts.constLast(), Qt::CaseInsensitive) == 0;
        });
        if (match == records.cend()) {
            result->blockers.append(QStringLiteral("APFS file path not found: %1").arg(path));
            return std::nullopt;
        }
        return *match;
    }

    [[nodiscard]] PartitionApfsFileEntry entryFromRecord(const DirectoryRecord& record,
                                                         const QString& parentPath) const {
        const auto inode = inodeById_.constFind(record.file_id);
        const uint16_t mode = inode == inodeById_.cend() ? 0 : inode->mode;
        PartitionApfsFileEntry entry;
        entry.name = record.name;
        entry.path = childPath(parentPath, record.name);
        entry.object_id = record.file_id;
        entry.size_bytes = inode == inodeById_.cend() ? 0 : inode->size;
        // A compressed file has no data stream (inode size 0); report its logical
        // size from the decmpfs header so listings show the real size.
        const auto decmpfs = decmpfsByInode_.constFind(record.file_id);
        if (decmpfs != decmpfsByInode_.cend()) {
            const auto header = apfsParseDecmpfsHeader(*decmpfs);
            if (header.has_value()) {
                entry.size_bytes = header->uncompressed_size;
            }
        }
        entry.directory = record.directory_type == kApfsDirTypeDirectory ||
                          (mode & kApfsModeTypeMask) == kApfsModeDirectory;
        entry.regular_file = record.directory_type == kApfsDirTypeRegularFile ||
                             (mode & kApfsModeTypeMask) == kApfsModeRegularFile;
        entry.symlink = record.directory_type == kApfsDirTypeSymlink ||
                        (mode & kApfsModeTypeMask) == kApfsModeSymlink;
        entry.type = entryTypeName(record.directory_type, mode);
        return entry;
    }

    [[nodiscard]] bool appendExtentBytes(const FileExtentRecord& extent,
                                         uint64_t extentOffset,
                                         uint64_t length,
                                         PartitionApfsFileReadResult* result) {
        uint64_t remaining = length;
        uint64_t absolute = extent.physical_block * blockSize_ + extentOffset;
        const bool encrypted = extent.crypto_id != 0 && volumeOneKey_ && !vek_.isEmpty();
        while (remaining > 0) {
            const uint64_t blockIndex = absolute / blockSize_;
            const uint64_t blockOffset = absolute % blockSize_;
            QByteArray block;
            if (!readBlock(blockIndex, &block, result, true, false)) {
                return false;
            }
            if (encrypted) {
                // ONEKEY data block: AES-XTS-decrypt with the VEK (no object header,
                // so no checksum); tweak base = physical block addr * 8.
                const QByteArray plain = sak::apfs_crypto::xtsDecryptBlock(vek_, blockIndex, block);
                if (plain.size() == static_cast<qsizetype>(blockSize_)) {
                    block = plain;
                }
            }
            const uint64_t chunk =
                std::min<uint64_t>(remaining, static_cast<uint64_t>(block.size()) - blockOffset);
            result->data.append(block.constData() + static_cast<qsizetype>(blockOffset),
                                static_cast<qsizetype>(chunk));
            remaining -= chunk;
            absolute += chunk;
        }
        return true;
    }

    [[nodiscard]] BtreeInfo btreeInfo(const QByteArray& rootBlock) const {
        const qsizetype offset = static_cast<qsizetype>(blockSize_) - kApfsBtreeInfoBytes;
        return {le32(rootBlock, offset),
                le32(rootBlock, offset + kApfsBtreeInfoNodeSizeOffset),
                le32(rootBlock, offset + kApfsBtreeInfoKeySizeOffset),
                le32(rootBlock, offset + kApfsBtreeInfoValueSizeOffset),
                le64(rootBlock, offset + kApfsBtreeInfoKeyCountOffset),
                le64(rootBlock, offset + kApfsBtreeInfoNodeCountOffset)};
    }

    [[nodiscard]] QVector<BtreeEntryView> btreeEntries(const QByteArray& node,
                                                       const BtreeInfo& info) const {
        QVector<BtreeEntryView> entries;
        const uint16_t flags = le16(node, kApfsBtreeNodeFlagsOffset);
        const uint16_t level = le16(node, kApfsBtreeNodeLevelOffset);
        const uint32_t count = le32(node, kApfsBtreeNodeCountOffset);
        const uint16_t tableOffset = le16(node, kApfsBtreeNodeTableOffsetOffset);
        const uint16_t tableLength = le16(node, kApfsBtreeNodeTableLengthOffset);
        const bool fixed = (flags & kApfsBtreeNodeFixedKvSize) != 0;
        const bool root = (flags & kApfsBtreeNodeRoot) != 0;
        const qsizetype tableStart = kApfsBtreeNodeHeaderBytes + tableOffset;
        const qsizetype keyAreaStart = tableStart + tableLength;
        const qsizetype valueAreaEnd = root ? static_cast<qsizetype>(blockSize_) -
                                                  kApfsBtreeInfoBytes
                                            : static_cast<qsizetype>(blockSize_);
        const qsizetype tocEntryBytes = fixed ? kApfsBtreeFixedTocEntryBytes
                                              : kApfsBtreeVariableTocEntryBytes;
        if (!btreeTableInBounds(node, count, tableStart, keyAreaStart, tocEntryBytes)) {
            return entries;
        }
        entries.reserve(static_cast<int>(std::min<uint32_t>(count, kApfsBtreeMaxEntryCount)));
        const BtreeEntryContext context{keyAreaStart, valueAreaEnd, level, info};
        for (uint32_t index = 0; index < count; ++index) {
            const qsizetype toc = tableStart + static_cast<qsizetype>(index) * tocEntryBytes;
            const auto entry = fixed ? fixedBtreeEntry(node, toc, context)
                                     : variableBtreeEntry(node, toc, context);
            if (entry.has_value() && btreeEntryInBounds(node, *entry)) {
                entries.append(*entry);
            }
        }
        return entries;
    }

    [[nodiscard]] bool btreeTableInBounds(const QByteArray& node,
                                          uint32_t count,
                                          qsizetype tableStart,
                                          qsizetype keyAreaStart,
                                          qsizetype tocEntryBytes) const {
        return count <= kApfsBtreeMaxEntryCount && tableStart >= kApfsBtreeNodeHeaderBytes &&
               tableStart + static_cast<qsizetype>(count) * tocEntryBytes <= node.size() &&
               keyAreaStart <= node.size();
    }

    [[nodiscard]] std::optional<BtreeEntryView> fixedBtreeEntry(
        const QByteArray& node, qsizetype toc, const BtreeEntryContext& context) const {
        const uint16_t keyOffset = le16(node, toc);
        const uint16_t valueOffset = le16(node, toc + kApfsBtreeFixedTocValueOffset);
        return BtreeEntryView{context.key_area_start + keyOffset,
                              context.info.key_size,
                              context.value_area_end - valueOffset,
                              context.level > 0 ? kApfsBtreeChildPointerBytes
                                                : context.info.value_size};
    }

    [[nodiscard]] std::optional<BtreeEntryView> variableBtreeEntry(
        const QByteArray& node, qsizetype toc, const BtreeEntryContext& context) const {
        const uint16_t keyOffset = le16(node, toc);
        const uint16_t keyLength = le16(node, toc + kApfsBtreeVariableTocKeyLengthOffset);
        const uint16_t valueOffset = le16(node, toc + kApfsBtreeVariableTocValueOffset);
        const uint16_t valueLength = le16(node, toc + kApfsBtreeVariableTocValueLengthOffset);
        return BtreeEntryView{context.key_area_start + keyOffset,
                              keyLength,
                              context.value_area_end - valueOffset,
                              valueLength};
    }

    [[nodiscard]] bool btreeEntryInBounds(const QByteArray& node,
                                          const BtreeEntryView& entry) const {
        return entry.key_length > 0 && entry.value_length >= 0 && entry.key_offset >= 0 &&
               entry.value_offset >= 0 && entry.key_offset + entry.key_length <= node.size() &&
               entry.value_offset + entry.value_length <= node.size();
    }

    [[nodiscard]] bool readBlock(uint64_t block,
                                 QByteArray* bytes,
                                 PartitionApfsFileReadResult* result,
                                 bool appendBlocker = true,
                                 bool validateObjectChecksum = true) {
        if (block >= blockCount_ && blockCount_ != 0) {
            if (appendBlocker) {
                result->blockers.append(
                    QStringLiteral("APFS block %1 is outside container bounds").arg(block));
            }
            return false;
        }
        if (!readBytes(block * blockSize_, blockSize_, bytes, result, appendBlocker)) {
            return false;
        }
        return !validateObjectChecksum ||
               validateObjectBlockChecksum(block, *bytes, result, appendBlocker);
    }

    // Read a virtual fs-tree node, AES-XTS-decrypting it with the VEK (tweak base =
    // block * 8) before the Fletcher-64 check when the omap flagged it encrypted.
    [[nodiscard]] bool readDecryptedNode(uint64_t block,
                                         bool encrypted,
                                         QByteArray* bytes,
                                         PartitionApfsFileReadResult* result) {
        if (!encrypted || vek_.isEmpty()) {
            return readBlock(block, bytes, result);
        }
        if (!readBlock(block, bytes, result, true, false)) {
            return false;
        }
        const QByteArray plain = sak::apfs_crypto::xtsDecryptBlock(vek_, block, *bytes);
        if (plain.size() == static_cast<qsizetype>(blockSize_)) {
            *bytes = plain;
        }
        return validateObjectBlockChecksum(block, *bytes, result);
    }

    [[nodiscard]] bool validateObjectBlockChecksum(uint64_t block,
                                                   const QByteArray& bytes,
                                                   PartitionApfsFileReadResult* result,
                                                   bool appendBlocker = true) const {
        const QByteArray objectBytes = bytes.size() > static_cast<qsizetype>(blockSize_)
                                           ? bytes.left(static_cast<qsizetype>(blockSize_))
                                           : bytes;
        if (PartitionApfsWriter::verifyObjectChecksum(objectBytes)) {
            return true;
        }
        if (appendBlocker) {
            result->blockers.append(
                QStringLiteral("APFS object checksum failed at block %1").arg(block));
        }
        return false;
    }

    [[nodiscard]] bool readBytes(uint64_t offset,
                                 uint64_t length,
                                 QByteArray* bytes,
                                 PartitionApfsFileReadResult* result,
                                 bool appendBlocker = true) {
        if (!bytes) {
            return false;
        }
        if (length > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
            if (appendBlocker) {
                result->blockers.append(QStringLiteral("APFS read request is too large"));
            }
            return false;
        }
        if (offset > static_cast<uint64_t>(std::numeric_limits<qint64>::max()) ||
            !device_->seek(static_cast<qint64>(offset))) {
            if (appendBlocker) {
                result->blockers.append(QStringLiteral("APFS seek failed at byte %1: %2")
                                            .arg(offset)
                                            .arg(device_->errorString()));
            }
            return false;
        }
        *bytes = device_->read(static_cast<qint64>(length));
        if (bytes->size() < static_cast<qsizetype>(length)) {
            if (appendBlocker) {
                result->blockers.append(QStringLiteral("APFS read failed at byte %1: %2")
                                            .arg(offset)
                                            .arg(device_->errorString()));
            }
            return false;
        }
        return true;
    }

    QIODevice* device_{nullptr};
    QString credential_;  // A6: volume password or personal recovery key (in memory only)
    uint32_t blockSize_{0};
    uint64_t blockCount_{0};
    uint64_t containerXid_{0};
    uint64_t volumeXid_{0};
    uint64_t rootTreeOid_{0};
    uint64_t rootTreeAddress_{0};
    bool rootTreeEncrypted_{false};
    bool volumeOneKey_{false};  // ONEKEY whole-volume FileVault: data tweak = block addr
    QByteArray containerUuid_;
    uint64_t keylockerPaddr_{0};
    QByteArray vek_;  // 32-byte AES-XTS volume key (empty = locked / unencrypted)
    QString volumeName_;
    ObjectMapState volumeOmap_;
    bool fileSystemScanned_{false};
    QVector<DirectoryRecord> directoryRecords_;
    QHash<uint64_t, InodeRecord> inodeById_;
    QMultiHash<uint64_t, FileExtentRecord> extentsByOwner_;
    // Embedded com.apple.decmpfs attribute value (16-byte header + inline payload)
    // keyed by inode object id, for transparently-compressed files (A5).
    QHash<uint64_t, QByteArray> decmpfsByInode_;
};

struct ApfsExportFrame {
    QString source_path;
    QString output_directory;
};

void appendApfsExportRequestBlockers(const QString& imagePath,
                                     const QString& outputDirectory,
                                     const PartitionApfsDirectoryExportOptions& options,
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

class ApfsDirectoryExporter {
public:
    ApfsDirectoryExporter(QIODevice* image, PartitionApfsDirectoryExportOptions options)
        : reader_(image), options_(options) {}

    PartitionApfsDirectoryExportResult run(const QString& sourcePath,
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
    [[nodiscard]] bool processFrame(const ApfsExportFrame& frame) {
        const QString visitKey = cleanPath(frame.source_path).toLower();
        if (visited_directories_.contains(visitKey)) {
            return true;
        }
        visited_directories_.insert(visitKey);
        const auto listing = reader_.listDirectory(
            frame.source_path, std::max(1, options_.max_entries - result_.entries_scanned));
        result_.warnings.append(listing.warnings);
        if (!listing.ok) {
            result_.blockers.append(listing.blockers);
            return false;
        }
        return processEntries(listing.entries, QDir(frame.output_directory));
    }

    [[nodiscard]] bool processEntries(const QVector<PartitionApfsFileEntry>& entries,
                                      const QDir& targetDir) {
        for (const auto& entry : entries) {
            if (!processEntry(entry, targetDir)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool processEntry(const PartitionApfsFileEntry& entry, const QDir& targetDir) {
        if (!consumeEntrySlot()) {
            return false;
        }
        const QString suffixId = QString::number(entry.object_id, kExportNameFallbackBase);
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
            return skipSymlink(entry);
        }
        if (!entry.regular_file) {
            result_.warnings.append(
                QStringLiteral("Skipped unsupported APFS entry: %1").arg(entry.path));
            return true;
        }
        return exportFile(entry, targetPath);
    }

    [[nodiscard]] bool consumeEntrySlot() {
        if (result_.entries_scanned >= options_.max_entries) {
            result_.warnings.append(QStringLiteral("APFS export entry cap reached"));
            return false;
        }
        ++result_.entries_scanned;
        return true;
    }

    [[nodiscard]] bool exportDirectory(const PartitionApfsFileEntry& entry,
                                       const QString& targetPath) {
        if (!QDir().mkpath(targetPath)) {
            result_.blockers.append(
                QStringLiteral("Unable to create exported directory: %1").arg(targetPath));
            return false;
        }
        ++result_.directories_exported;
        pending_.append({entry.path, targetPath});
        return true;
    }

    [[nodiscard]] bool skipSymlink(const PartitionApfsFileEntry& entry) {
        ++result_.symlinks_skipped;
        result_.warnings.append(QStringLiteral("Skipped APFS symlink: %1").arg(entry.path));
        return true;
    }

    [[nodiscard]] bool exportFile(const PartitionApfsFileEntry& entry, const QString& targetPath) {
        if (!fitsByteCaps(entry)) {
            return false;
        }
        const auto file = reader_.readFile(entry.path, options_.max_file_bytes);
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

    [[nodiscard]] bool fitsByteCaps(const PartitionApfsFileEntry& entry) {
        const bool fileTooLarge = entry.size_bytes > options_.max_file_bytes;
        const bool totalTooLarge = result_.bytes_exported > options_.max_total_bytes ||
                                   entry.size_bytes >
                                       options_.max_total_bytes - result_.bytes_exported;
        if (fileTooLarge || totalTooLarge) {
            result_.blockers.append(
                QStringLiteral("Export byte cap reached before %1").arg(entry.path));
            return false;
        }
        return true;
    }

    ApfsReader reader_;
    PartitionApfsDirectoryExportOptions options_;
    PartitionApfsDirectoryExportResult result_;
    QVector<ApfsExportFrame> pending_;
    QSet<QString> visited_directories_;
};

PartitionApfsFileReadResult withOpenedApfsImage(
    const QString& path, const std::function<PartitionApfsFileReadResult(QIODevice*)>& operation) {
    QString openError;
    auto device = openFileOrRawDeviceReadOnly(path, &openError);
    if (!device) {
        PartitionApfsFileReadResult result;
        result.file_system = QStringLiteral("APFS");
        result.blockers.append(QStringLiteral("Open failed: %1").arg(openError));
        return result;
    }
    return operation(device.get());
}

}  // namespace

PartitionApfsFileReadResult PartitionApfsFileSystemReader::listDirectory(
    QIODevice* device, const QString& path, int max_entries, const QString& credential) {
    return ApfsReader(device, credential).listDirectory(path, max_entries);
}

PartitionApfsFileReadResult PartitionApfsFileSystemReader::listDirectoryFromImage(
    const QString& image_path, const QString& path, int max_entries, const QString& credential) {
    return withOpenedApfsImage(image_path, [&](QIODevice* device) {
        return PartitionApfsFileSystemReader::listDirectory(device, path, max_entries, credential);
    });
}

PartitionApfsFileReadResult PartitionApfsFileSystemReader::readFile(QIODevice* device,
                                                                    const QString& path,
                                                                    uint64_t max_bytes,
                                                                    const QString& credential) {
    return ApfsReader(device, credential).readFile(path, max_bytes);
}

PartitionApfsFileReadResult PartitionApfsFileSystemReader::readFileFromImage(
    const QString& image_path, const QString& path, uint64_t max_bytes, const QString& credential) {
    return withOpenedApfsImage(image_path, [&](QIODevice* device) {
        return PartitionApfsFileSystemReader::readFile(device, path, max_bytes, credential);
    });
}

PartitionApfsDirectoryExportResult PartitionApfsFileSystemReader::exportDirectoryFromImage(
    const QString& image_path,
    const QString& source_path,
    const QString& output_directory,
    const PartitionApfsDirectoryExportOptions& options) {
    PartitionApfsDirectoryExportResult exportResult;
    appendApfsExportRequestBlockers(image_path, output_directory, options, &exportResult.blockers);
    if (!exportResult.blockers.isEmpty()) {
        return exportResult;
    }

    QString openError;
    auto image = openFileOrRawDeviceReadOnly(image_path, &openError);
    if (!image) {
        exportResult.blockers.append(
            QStringLiteral("Unable to open APFS image read-only: %1").arg(openError));
        return exportResult;
    }

    QDir root(output_directory);
    if (!root.mkpath(QStringLiteral("."))) {
        exportResult.blockers.append(QStringLiteral("Unable to create output directory"));
        return exportResult;
    }

    return ApfsDirectoryExporter(image.get(), options).run(source_path, root.absolutePath());
}

}  // namespace sak

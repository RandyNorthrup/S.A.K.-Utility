// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_safety_validator.cpp
/// @brief Safety rule implementation for Partition Manager operations.

#include "sak/partition_safety_validator.h"

#include <QSet>
#include <QStringList>

#include <algorithm>
#include <iterator>
#include <limits>
#include <optional>

namespace sak {

namespace {

constexpr uint64_t kAllocationUnitDefaultBytes = 0;
constexpr uint64_t kAllocationUnit512Bytes = 512;
constexpr uint64_t kAllocationUnit1KbBytes = 1024;
constexpr uint64_t kAllocationUnit2KbBytes = 2 * 1024;
constexpr uint64_t kAllocationUnit4KbBytes = 4 * 1024;
constexpr uint64_t kAllocationUnit8KbBytes = 8 * 1024;
constexpr uint64_t kAllocationUnit16KbBytes = 16 * 1024;
constexpr uint64_t kAllocationUnit32KbBytes = 32 * 1024;
constexpr uint64_t kAllocationUnit64KbBytes = 64 * 1024;
constexpr uint64_t kAllocateFreeSpaceMinimumRemainderBytes = 64ULL * 1024ULL * 1024ULL;
constexpr auto kApfsRootFileNamePayload = "apfs_root_file_name";
constexpr auto kApfsRootDirectoryNamePayload = "apfs_root_directory_name";
constexpr auto kApfsRootFilePayloadBase64 = "apfs_root_file_payload_base64";
constexpr auto kApfsRootFilePayloadText = "apfs_root_file_payload_text";
constexpr auto kApfsRootFilePatchOffsetPayload = "apfs_root_file_patch_offset_bytes";
constexpr auto kApfsGeneratedLayoutConfirmedPayload = "apfs_generated_layout_confirmed";
constexpr auto kApfsEncryptVolumePayload = "apfs_encrypt_volume";
constexpr auto kApfsVolumePasswordPayload = "apfs_volume_password";
constexpr qsizetype kApfsVolumeLabelMaxChars = 255;
constexpr qsizetype kApfsVolumeLabelFieldBytes = 256;
constexpr auto kHfsPathPayload = "hfs_path";
constexpr auto kHfsDestinationPathPayload = "hfs_destination_path";
constexpr auto kHfsPayloadBase64 = "hfs_payload_base64";
constexpr auto kHfsPayloadText = "hfs_payload_text";
constexpr auto kHfsFileIdPayload = "hfs_file_id";
constexpr auto kHfsAttributeNamePayload = "hfs_attribute_name";
constexpr auto kHfsSecureWipeReleasedBlocksPayload = "hfs_secure_wipe_released_blocks";

bool isDestructiveDiskOperation(PartitionOperationType type) {
    static const QSet<int> kTypes = {
        static_cast<int>(PartitionOperationType::ConvertPartitionStyle),
        static_cast<int>(PartitionOperationType::InitializeDisk),
        static_cast<int>(PartitionOperationType::DeleteAllPartitions),
        static_cast<int>(PartitionOperationType::CloneDisk),
        static_cast<int>(PartitionOperationType::RestoreRecoveredPartition),
        static_cast<int>(PartitionOperationType::RestoreImage),
        static_cast<int>(PartitionOperationType::MigrateOs),
        static_cast<int>(PartitionOperationType::WipeDisk),
        static_cast<int>(PartitionOperationType::ConvertDynamicDiskToBasic),
    };
    return kTypes.contains(static_cast<int>(type));
}

bool isDestructivePartitionOperation(PartitionOperationType type) {
    static const QSet<int> kTypes = {
        static_cast<int>(PartitionOperationType::Delete),
        static_cast<int>(PartitionOperationType::Format),
        static_cast<int>(PartitionOperationType::Resize),
        static_cast<int>(PartitionOperationType::AllocateFreeSpace),
        static_cast<int>(PartitionOperationType::Merge),
        static_cast<int>(PartitionOperationType::Split),
        static_cast<int>(PartitionOperationType::ConvertFileSystem),
        static_cast<int>(PartitionOperationType::ChangeClusterSize),
        static_cast<int>(PartitionOperationType::ClonePartition),
        static_cast<int>(PartitionOperationType::RestoreImage),
        static_cast<int>(PartitionOperationType::MovePartition),
        static_cast<int>(PartitionOperationType::ConvertPrimaryLogical),
        static_cast<int>(PartitionOperationType::ChangeVolumeSerialNumber),
        static_cast<int>(PartitionOperationType::SetPartitionActive),
        static_cast<int>(PartitionOperationType::SetPartitionHidden),
        static_cast<int>(PartitionOperationType::SetPartitionTypeId),
        static_cast<int>(PartitionOperationType::WipePartition),
        static_cast<int>(PartitionOperationType::WipeFreeSpace),
        static_cast<int>(PartitionOperationType::ApfsWriteRootFile),
        static_cast<int>(PartitionOperationType::ApfsPatchRootFile),
        static_cast<int>(PartitionOperationType::ApfsPatchRootDirectoryFile),
        static_cast<int>(PartitionOperationType::ApfsDeleteRootFile),
        static_cast<int>(PartitionOperationType::ApfsWriteRootDirectoryFile),
        static_cast<int>(PartitionOperationType::ApfsDeleteRootDirectoryFile),
        static_cast<int>(PartitionOperationType::ApfsCreateRootDirectory),
        static_cast<int>(PartitionOperationType::ApfsDeleteRootDirectory),
        static_cast<int>(PartitionOperationType::ApfsChangeVolumeLabel),
        static_cast<int>(PartitionOperationType::ApfsSnapshotCreate),
        static_cast<int>(PartitionOperationType::ApfsSnapshotDelete),
        static_cast<int>(PartitionOperationType::ApfsSnapshotRevert),
        static_cast<int>(PartitionOperationType::ApfsCloneRootFile),
        static_cast<int>(PartitionOperationType::ApfsHardlinkRootFile),
        static_cast<int>(PartitionOperationType::ApfsResizeContainer),
        static_cast<int>(PartitionOperationType::HfsOverwriteFile),
        static_cast<int>(PartitionOperationType::HfsReplaceFile),
        static_cast<int>(PartitionOperationType::HfsGrowFile),
        static_cast<int>(PartitionOperationType::HfsTruncateFile),
        static_cast<int>(PartitionOperationType::HfsReplaceResourceFork),
        static_cast<int>(PartitionOperationType::HfsGrowResourceFork),
        static_cast<int>(PartitionOperationType::HfsTruncateResourceFork),
        static_cast<int>(PartitionOperationType::HfsCreateEmptyFile),
        static_cast<int>(PartitionOperationType::HfsCreateFile),
        static_cast<int>(PartitionOperationType::HfsDeleteEmptyFile),
        static_cast<int>(PartitionOperationType::HfsDeleteFile),
        static_cast<int>(PartitionOperationType::HfsCreateEmptyFolder),
        static_cast<int>(PartitionOperationType::HfsDeleteEmptyFolder),
        static_cast<int>(PartitionOperationType::HfsDeleteFolderTree),
        static_cast<int>(PartitionOperationType::HfsRenameMoveCatalogEntry),
        static_cast<int>(PartitionOperationType::HfsReplaceInlineAttribute),
        static_cast<int>(PartitionOperationType::HfsReplaceForkAttribute),
        static_cast<int>(PartitionOperationType::HfsGrowForkAttribute),
        static_cast<int>(PartitionOperationType::HfsCreateSymlink),
        static_cast<int>(PartitionOperationType::HfsCreateHardlink),
        static_cast<int>(PartitionOperationType::HfsDeleteHardlink),
    };
    return kTypes.contains(static_cast<int>(type));
}

bool isSystemAllowedOperation(PartitionOperationType type) {
    static const QSet<int> kTypes = {
        static_cast<int>(PartitionOperationType::ConvertPartitionStyle),
        static_cast<int>(PartitionOperationType::CloneDisk),
        static_cast<int>(PartitionOperationType::CreateImage),
        static_cast<int>(PartitionOperationType::MigrateOs),
        static_cast<int>(PartitionOperationType::RepairBoot),
        static_cast<int>(PartitionOperationType::OptimizeSsd),
        static_cast<int>(PartitionOperationType::DefragVolume),
        static_cast<int>(PartitionOperationType::BitLockerUnlock),
        static_cast<int>(PartitionOperationType::BitLockerSuspend),
        static_cast<int>(PartitionOperationType::BitLockerResume),
    };
    return kTypes.contains(static_cast<int>(type));
}

void addBlockerIf(PartitionValidationResult* result, bool blocked, const QString& message) {
    if (blocked) {
        result->blockers.append(message);
    }
}

void addWarningIf(PartitionValidationResult* result, bool warned, const QString& message) {
    if (warned) {
        result->warnings.append(message);
    }
}

bool blocksCurrentOsDiskMutation(const PartitionDiskInfo& disk, PartitionOperationType type) {
    return disk.is_system && isDestructiveDiskOperation(type) && !isSystemAllowedOperation(type);
}

bool allowsDynamicDiskOperation(PartitionOperationType type) {
    return type == PartitionOperationType::ConvertDynamicDiskToBasic;
}

bool blocksDataDiskStyleConversion(const PartitionDiskInfo& disk, PartitionOperationType type) {
    return type == PartitionOperationType::ConvertPartitionStyle && !disk.partitions.isEmpty() &&
           !disk.is_system;
}

bool blocksUnsafeSystemStyleConversion(const PartitionDiskInfo& disk,
                                       const PartitionOperation& operation) {
    const QString mode = operation.payload.value(QStringLiteral("mode")).toString();
    const QString target_style =
        operation.payload.value(QStringLiteral("target_style")).toString().toUpper();
    return disk.is_system && operation.type == PartitionOperationType::ConvertPartitionStyle &&
           (mode != QStringLiteral("mbr2gpt") || target_style != QStringLiteral("GPT"));
}

bool diskLooksSsd(const PartitionDiskInfo& disk) {
    const QString media =
        QStringLiteral("%1 %2 %3").arg(disk.media_type, disk.bus_type, disk.model).toUpper();
    return media.contains(QStringLiteral("SSD")) || media.contains(QStringLiteral("NVME"));
}

bool diskLooksHdd(const PartitionDiskInfo& disk) {
    const QString media = QStringLiteral("%1 %2").arg(disk.media_type, disk.model).toUpper();
    return media.contains(QStringLiteral("HDD")) || media.contains(QStringLiteral("HARD"));
}

bool requiresTargetOverwriteConfirmation(const PartitionOperation& operation) {
    return (operation.type == PartitionOperationType::CloneDisk ||
            operation.type == PartitionOperationType::MigrateOs ||
            operation.type == PartitionOperationType::RestoreImage) &&
           !operation.payload.value(QStringLiteral("target_wipe_confirmed")).toBool(false);
}

bool requiresRecoveryRestoreAcknowledgement(const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::RestoreRecoveredPartition &&
           !operation.payload.value(QStringLiteral("restore_acknowledged")).toBool(false);
}

bool requiresDriveLetter(PartitionOperationType type) {
    static const QSet<int> kTypes = {
        static_cast<int>(PartitionOperationType::CheckFileSystem),
        static_cast<int>(PartitionOperationType::SurfaceTest),
        static_cast<int>(PartitionOperationType::ConvertFileSystem),
        static_cast<int>(PartitionOperationType::ChangeClusterSize),
        static_cast<int>(PartitionOperationType::WipeFreeSpace),
        static_cast<int>(PartitionOperationType::OptimizeSsd),
        static_cast<int>(PartitionOperationType::DefragVolume),
        static_cast<int>(PartitionOperationType::BitLockerUnlock),
        static_cast<int>(PartitionOperationType::BitLockerSuspend),
        static_cast<int>(PartitionOperationType::BitLockerResume),
    };
    return kTypes.contains(static_cast<int>(type));
}

bool blocksProtectedPartition(const PartitionInfoEx& partition, PartitionOperationType type) {
    return PartitionSafetyValidator::isSystemProtectedPartition(partition) &&
           isDestructivePartitionOperation(type);
}

bool hasLockedBitLockerVolume(const PartitionInfoEx& partition) {
    return partition.volume && partition.volume->bitlocker_locked;
}

bool isBitLockerManagementOperation(PartitionOperationType type) {
    return type == PartitionOperationType::BitLockerUnlock ||
           type == PartitionOperationType::BitLockerSuspend ||
           type == PartitionOperationType::BitLockerResume;
}

bool hasUnlockedBitLockerVolume(const PartitionInfoEx& partition) {
    return partition.volume && partition.volume->bitlocker_enabled &&
           !partition.volume->bitlocker_locked;
}

bool hasDirtyVolume(const PartitionInfoEx& partition, PartitionOperationType type) {
    return partition.volume && partition.volume->dirty_bit_set &&
           type != PartitionOperationType::RepairBoot;
}

uint64_t payloadUInt64(const PartitionOperation& operation, const QString& key) {
    const auto value = operation.payload.value(key);
    if (value.isDouble()) {
        return static_cast<uint64_t>(value.toDouble());
    }
    bool ok = false;
    const auto parsed = value.toString().toULongLong(&ok);
    return ok ? parsed : 0;
}

bool payloadBool(const PartitionOperation& operation, const QString& key) {
    const auto value = operation.payload.value(key);
    if (value.isBool()) {
        return value.toBool();
    }
    const QString text = value.toString().trimmed().toLower();
    return text == QStringLiteral("true") || text == QStringLiteral("1") ||
           text == QStringLiteral("yes");
}

bool isNonNativeFileSystemToolOperation(const PartitionOperation& operation) {
    return payloadBool(operation, QStringLiteral("non_native_file_system_tool"));
}

bool isExtFileSystemToken(const QString& fileSystem) {
    const QString token = fileSystem.trimmed().toLower();
    return token == QStringLiteral("ext2") || token == QStringLiteral("ext3") ||
           token == QStringLiteral("ext4");
}

bool isLinuxSwapFileSystemToken(const QString& fileSystem) {
    const QString token = fileSystem.trimmed().toLower();
    return token == QStringLiteral("linux swap") || token == QStringLiteral("linux-swap") ||
           token == QStringLiteral("swap");
}

bool isHfsFileSystemToken(const QString& fileSystem) {
    const QString token = fileSystem.trimmed().toLower();
    return token == QStringLiteral("hfs+") || token == QStringLiteral("hfsplus") ||
           token == QStringLiteral("hfsx");
}

bool isApfsFileSystemToken(const QString& fileSystem) {
    return fileSystem.trimmed().compare(QStringLiteral("apfs"), Qt::CaseInsensitive) == 0;
}

bool isSupportedNonNativeFileSystemToolOperation(const PartitionOperation& operation) {
    constexpr PartitionOperationType kSupportedTypes[] = {
        PartitionOperationType::Create,
        PartitionOperationType::Format,
        PartitionOperationType::CheckFileSystem,
        PartitionOperationType::Resize,
        PartitionOperationType::ApfsWriteRootFile,
        PartitionOperationType::ApfsPatchRootFile,
        PartitionOperationType::ApfsPatchRootDirectoryFile,
        PartitionOperationType::ApfsDeleteRootFile,
        PartitionOperationType::ApfsWriteRootDirectoryFile,
        PartitionOperationType::ApfsDeleteRootDirectoryFile,
        PartitionOperationType::ApfsCreateRootDirectory,
        PartitionOperationType::ApfsDeleteRootDirectory,
        PartitionOperationType::ApfsChangeVolumeLabel,
        PartitionOperationType::ApfsSnapshotCreate,
        PartitionOperationType::ApfsSnapshotDelete,
        PartitionOperationType::ApfsSnapshotRevert,
        PartitionOperationType::ApfsCloneRootFile,
        PartitionOperationType::ApfsHardlinkRootFile,
        PartitionOperationType::ApfsResizeContainer,
        PartitionOperationType::HfsOverwriteFile,
        PartitionOperationType::HfsReplaceFile,
        PartitionOperationType::HfsGrowFile,
        PartitionOperationType::HfsTruncateFile,
        PartitionOperationType::HfsReplaceResourceFork,
        PartitionOperationType::HfsGrowResourceFork,
        PartitionOperationType::HfsTruncateResourceFork,
        PartitionOperationType::HfsCreateEmptyFile,
        PartitionOperationType::HfsCreateFile,
        PartitionOperationType::HfsDeleteEmptyFile,
        PartitionOperationType::HfsDeleteFile,
        PartitionOperationType::HfsCreateEmptyFolder,
        PartitionOperationType::HfsDeleteEmptyFolder,
        PartitionOperationType::HfsDeleteFolderTree,
        PartitionOperationType::HfsRenameMoveCatalogEntry,
        PartitionOperationType::HfsReplaceInlineAttribute,
        PartitionOperationType::HfsReplaceForkAttribute,
        PartitionOperationType::HfsGrowForkAttribute,
        PartitionOperationType::HfsCreateSymlink,
        PartitionOperationType::HfsCreateHardlink,
        PartitionOperationType::HfsDeleteHardlink,
    };
    return std::find(std::begin(kSupportedTypes), std::end(kSupportedTypes), operation.type) !=
           std::end(kSupportedTypes);
}

bool isApfsRootFileMutationOperation(PartitionOperationType type) {
    return type == PartitionOperationType::ApfsWriteRootFile ||
           type == PartitionOperationType::ApfsPatchRootFile ||
           type == PartitionOperationType::ApfsPatchRootDirectoryFile ||
           type == PartitionOperationType::ApfsDeleteRootFile ||
           type == PartitionOperationType::ApfsWriteRootDirectoryFile ||
           type == PartitionOperationType::ApfsDeleteRootDirectoryFile ||
           type == PartitionOperationType::ApfsCreateRootDirectory ||
           type == PartitionOperationType::ApfsDeleteRootDirectory ||
           type == PartitionOperationType::ApfsChangeVolumeLabel;
}

bool apfsRootFileMutationWritesPayload(PartitionOperationType type) {
    return type == PartitionOperationType::ApfsWriteRootFile ||
           type == PartitionOperationType::ApfsWriteRootDirectoryFile ||
           type == PartitionOperationType::ApfsPatchRootDirectoryFile ||
           type == PartitionOperationType::ApfsPatchRootFile;
}

bool apfsRootDirectoryMutation(PartitionOperationType type) {
    return type == PartitionOperationType::ApfsCreateRootDirectory ||
           type == PartitionOperationType::ApfsDeleteRootDirectory;
}

bool apfsRootDirectoryFileMutation(PartitionOperationType type) {
    return type == PartitionOperationType::ApfsWriteRootDirectoryFile ||
           type == PartitionOperationType::ApfsPatchRootDirectoryFile ||
           type == PartitionOperationType::ApfsDeleteRootDirectoryFile;
}

bool apfsRootDirectoryNameRequired(PartitionOperationType type) {
    return apfsRootDirectoryMutation(type) || apfsRootDirectoryFileMutation(type);
}

bool apfsRootFileNameRequired(PartitionOperationType type) {
    return isApfsRootFileMutationOperation(type) && !apfsRootDirectoryMutation(type);
}

bool isHfsFileMutationOperation(PartitionOperationType type) {
    static const QSet<int> kTypes = {
        static_cast<int>(PartitionOperationType::HfsOverwriteFile),
        static_cast<int>(PartitionOperationType::HfsReplaceFile),
        static_cast<int>(PartitionOperationType::HfsGrowFile),
        static_cast<int>(PartitionOperationType::HfsTruncateFile),
        static_cast<int>(PartitionOperationType::HfsReplaceResourceFork),
        static_cast<int>(PartitionOperationType::HfsGrowResourceFork),
        static_cast<int>(PartitionOperationType::HfsTruncateResourceFork),
        static_cast<int>(PartitionOperationType::HfsCreateEmptyFile),
        static_cast<int>(PartitionOperationType::HfsCreateFile),
        static_cast<int>(PartitionOperationType::HfsDeleteEmptyFile),
        static_cast<int>(PartitionOperationType::HfsDeleteFile),
        static_cast<int>(PartitionOperationType::HfsCreateEmptyFolder),
        static_cast<int>(PartitionOperationType::HfsDeleteEmptyFolder),
        static_cast<int>(PartitionOperationType::HfsDeleteFolderTree),
        static_cast<int>(PartitionOperationType::HfsRenameMoveCatalogEntry),
        static_cast<int>(PartitionOperationType::HfsReplaceInlineAttribute),
        static_cast<int>(PartitionOperationType::HfsReplaceForkAttribute),
        static_cast<int>(PartitionOperationType::HfsGrowForkAttribute),
        static_cast<int>(PartitionOperationType::HfsCreateSymlink),
        static_cast<int>(PartitionOperationType::HfsCreateHardlink),
        static_cast<int>(PartitionOperationType::HfsDeleteHardlink),
    };
    return kTypes.contains(static_cast<int>(type));
}

bool hfsFileMutationWritesPayload(PartitionOperationType type) {
    return type == PartitionOperationType::HfsOverwriteFile ||
           type == PartitionOperationType::HfsReplaceFile ||
           type == PartitionOperationType::HfsGrowFile ||
           type == PartitionOperationType::HfsCreateFile ||
           type == PartitionOperationType::HfsReplaceResourceFork ||
           type == PartitionOperationType::HfsGrowResourceFork ||
           type == PartitionOperationType::HfsReplaceInlineAttribute ||
           type == PartitionOperationType::HfsReplaceForkAttribute ||
           type == PartitionOperationType::HfsGrowForkAttribute;
}

bool hfsFileMutationNeedsPath(PartitionOperationType type) {
    return type != PartitionOperationType::HfsReplaceInlineAttribute &&
           type != PartitionOperationType::HfsReplaceForkAttribute &&
           type != PartitionOperationType::HfsGrowForkAttribute;
}

QString normalizedTargetPath(const PartitionOperation& operation) {
    QString path = operation.payload.value(QStringLiteral("target_path")).toString().trimmed();
    path.replace('/', '\\');
    return path;
}

QString canonicalRawPartitionTargetPath(const PartitionOperation& operation) {
    if (operation.target.partition_number == 0) {
        return {};
    }
    return QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk%1\\Partition%2")
        .arg(operation.target.disk_number)
        .arg(operation.target.partition_number);
}

bool targetPathMatchesSelectedRawPartition(const PartitionOperation& operation) {
    const QString targetPath = normalizedTargetPath(operation);
    if (targetPath.isEmpty()) {
        return false;
    }
    const QString expected = canonicalRawPartitionTargetPath(operation);
    return !expected.isEmpty() && targetPath.compare(expected, Qt::CaseInsensitive) == 0;
}

QString normalizedBackupDirectory(const PartitionOperation& operation) {
    QString path = operation.payload.value(QStringLiteral("backup_directory")).toString().trimmed();
    path.replace('/', '\\');
    return path;
}

bool targetPathIsRawDevice(const PartitionOperation& operation) {
    return normalizedTargetPath(operation).startsWith(QStringLiteral("\\\\.\\"),
                                                      Qt::CaseInsensitive);
}

std::optional<uint32_t> physicalDriveNumberFromPath(const QString& targetPath) {
    QString path = targetPath.trimmed();
    path.replace('/', '\\');
    const QString prefix = QStringLiteral("\\\\.\\PhysicalDrive");
    if (!path.startsWith(prefix, Qt::CaseInsensitive)) {
        return std::nullopt;
    }

    bool ok = false;
    const uint value = path.mid(prefix.size()).toUInt(&ok);
    if (!ok) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(value);
}

std::optional<uint32_t> targetPhysicalDriveNumber(const PartitionOperation& operation) {
    return physicalDriveNumberFromPath(normalizedTargetPath(operation));
}

bool targetPathIsPhysicalDrive(const PartitionOperation& operation) {
    return targetPhysicalDriveNumber(operation).has_value();
}

bool targetPathStartsWithDrive(const QString& targetPath, const QString& driveLetter) {
    const QString letter = driveLetter.left(1).toUpper();
    if (letter.isEmpty()) {
        return false;
    }
    const QString drivePrefix = letter + QStringLiteral(":");
    return targetPath.startsWith(drivePrefix, Qt::CaseInsensitive);
}

bool createImageTargetsRawDevice(const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::CreateImage &&
           targetPathIsRawDevice(operation);
}

bool createImageTargetsSourcePartition(const PartitionInfoEx& partition,
                                       const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::CreateImage && partition.volume &&
           targetPathStartsWithDrive(normalizedTargetPath(operation),
                                     partition.volume->drive_letter);
}

bool createImageTargetsSourceDisk(const PartitionDiskInfo& disk,
                                  const PartitionOperation& operation) {
    if (operation.type != PartitionOperationType::CreateImage) {
        return false;
    }
    const QString targetPath = normalizedTargetPath(operation);
    return std::any_of(
        disk.partitions.cbegin(), disk.partitions.cend(), [&targetPath](const auto& p) {
            return p.volume && targetPathStartsWithDrive(targetPath, p.volume->drive_letter);
        });
}

bool isSupportedAllocationUnitSize(uint64_t value) {
    return value == kAllocationUnitDefaultBytes || value == kAllocationUnit512Bytes ||
           value == kAllocationUnit1KbBytes || value == kAllocationUnit2KbBytes ||
           value == kAllocationUnit4KbBytes || value == kAllocationUnit8KbBytes ||
           value == kAllocationUnit16KbBytes || value == kAllocationUnit32KbBytes ||
           value == kAllocationUnit64KbBytes;
}

bool isSupportedExplicitChangeClusterUnit(const PartitionOperation& operation) {
    const uint64_t value = payloadUInt64(operation, QStringLiteral("allocation_unit_bytes"));
    return value != kAllocationUnitDefaultBytes && isSupportedAllocationUnitSize(value);
}

bool changeClusterMissingBackup(const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::ChangeClusterSize &&
           normalizedBackupDirectory(operation).isEmpty();
}

bool changeClusterBackupOnTargetVolume(const PartitionInfoEx& partition,
                                       const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::ChangeClusterSize && partition.volume &&
           targetPathStartsWithDrive(normalizedBackupDirectory(operation),
                                     partition.volume->drive_letter);
}

bool changeClusterMissingConfirmation(const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::ChangeClusterSize &&
           !payloadBool(operation, QStringLiteral("target_wipe_confirmed"));
}

bool isSupportedGptCreateType(const QString& value) {
    return value.isEmpty() || QStringList{QStringLiteral("{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}"),
                                          QStringLiteral("{DE94BBA4-06D1-4D40-A16A-BFD50179D6AC}"),
                                          QStringLiteral("{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}"),
                                          QStringLiteral("{7C3457EF-0000-11AA-AA11-00306543ECAC}")}
                                  .contains(value.toUpper());
}

bool isSupportedMbrCreateType(const QString& value) {
    return value.isEmpty() ||
           QStringList{QStringLiteral("IFS"), QStringLiteral("FAT32")}.contains(value.toUpper());
}

bool hasMixedCreateTypes(const PartitionOperation& operation) {
    return !operation.payload.value(QStringLiteral("gpt_type")).toString().trimmed().isEmpty() &&
           !operation.payload.value(QStringLiteral("mbr_type")).toString().trimmed().isEmpty();
}

bool createTypeMatchesFileSystem(const PartitionOperation& operation) {
    const QString fs = operation.payload.value(QStringLiteral("file_system"))
                           .toString(QStringLiteral("NTFS"))
                           .toUpper();
    const QString gptType =
        operation.payload.value(QStringLiteral("gpt_type")).toString().trimmed().toUpper();
    const QString mbrType =
        operation.payload.value(QStringLiteral("mbr_type")).toString().trimmed().toUpper();
    if (gptType == QStringLiteral("{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}") ||
        mbrType == QStringLiteral("FAT32")) {
        return fs == QStringLiteral("FAT32");
    }
    if (gptType == QStringLiteral("{DE94BBA4-06D1-4D40-A16A-BFD50179D6AC}") ||
        mbrType == QStringLiteral("IFS")) {
        return fs == QStringLiteral("NTFS");
    }
    if (gptType == QStringLiteral("{7C3457EF-0000-11AA-AA11-00306543ECAC}")) {
        return fs == QStringLiteral("APFS");
    }
    return true;
}

bool createFitsSelectedRegion(const PartitionOperation& operation) {
    const uint64_t requestedSize = payloadUInt64(operation, QStringLiteral("size_bytes"));
    const uint64_t relativeOffset = payloadUInt64(operation,
                                                  QStringLiteral("relative_offset_bytes"));
    return relativeOffset <= operation.target.size_bytes &&
           requestedSize <= operation.target.size_bytes - relativeOffset;
}

void validateCreateUnallocatedPayload(const PartitionOperation& operation,
                                      PartitionValidationResult* result) {
    const auto requested_size = payloadUInt64(operation, QStringLiteral("size_bytes"));
    addBlockerIf(result,
                 requested_size == 0 || !createFitsSelectedRegion(operation),
                 QStringLiteral("Create size must fit inside the selected unallocated region"));

    const QString gptType =
        operation.payload.value(QStringLiteral("gpt_type")).toString().trimmed();
    const QString mbrType =
        operation.payload.value(QStringLiteral("mbr_type")).toString().trimmed();
    addBlockerIf(result,
                 !isSupportedAllocationUnitSize(
                     payloadUInt64(operation, QStringLiteral("allocation_unit_bytes"))),
                 QStringLiteral("Create uses an unsupported allocation unit size"));
    addBlockerIf(result,
                 hasMixedCreateTypes(operation) || !isSupportedGptCreateType(gptType) ||
                     !isSupportedMbrCreateType(mbrType),
                 QStringLiteral("Create uses an unsupported partition type"));
    addBlockerIf(result,
                 !createTypeMatchesFileSystem(operation),
                 QStringLiteral("Create partition type is incompatible with the file system"));
}

uint64_t saturatingAdd(uint64_t left, uint64_t right) {
    if (std::numeric_limits<uint64_t>::max() - left < right) {
        return std::numeric_limits<uint64_t>::max();
    }
    return left + right;
}

uint64_t usedBytes(const PartitionInfoEx& partition) {
    if (!partition.volume || partition.volume->total_bytes < partition.volume->free_bytes) {
        return 0;
    }
    return partition.volume->total_bytes - partition.volume->free_bytes;
}

uint64_t adjacentFreeBytesAfter(const PartitionDiskInfo& disk, const PartitionInfoEx& partition) {
    const uint64_t partitionEnd = saturatingAdd(partition.offset_bytes, partition.size_bytes);
    const auto it = std::find_if(disk.unallocated_regions.cbegin(),
                                 disk.unallocated_regions.cend(),
                                 [partitionEnd](const auto& region) {
                                     return region.offset_bytes == partitionEnd;
                                 });
    return it == disk.unallocated_regions.cend() ? 0 : it->size_bytes;
}

bool resizeTargetMissing(const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::Resize &&
           payloadUInt64(operation, QStringLiteral("target_size_bytes")) == 0;
}

bool resizeTargetIsNoop(const PartitionInfoEx& partition, const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::Resize &&
           payloadUInt64(operation, QStringLiteral("target_size_bytes")) == partition.size_bytes;
}

bool resizeShrinksBelowUsedBytes(const PartitionInfoEx& partition,
                                 const PartitionOperation& operation) {
    const uint64_t targetSize = payloadUInt64(operation, QStringLiteral("target_size_bytes"));
    const uint64_t currentUsedBytes = usedBytes(partition);
    return operation.type == PartitionOperationType::Resize && targetSize != 0 &&
           currentUsedBytes != 0 && targetSize < currentUsedBytes;
}

bool resizeExtendsBeyondAdjacentFreeSpace(const PartitionDiskInfo& disk,
                                          const PartitionInfoEx& partition,
                                          const PartitionOperation& operation) {
    const uint64_t targetSize = payloadUInt64(operation, QStringLiteral("target_size_bytes"));
    if (operation.type != PartitionOperationType::Resize || targetSize <= partition.size_bytes) {
        return false;
    }
    return targetSize - partition.size_bytes > adjacentFreeBytesAfter(disk, partition);
}

bool resizeRequestsStartMove(const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::Resize &&
           operation.payload.contains(QStringLiteral("target_offset_bytes"));
}

bool resizeRequestsDonorSpace(const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::Resize &&
           operation.payload.contains(QStringLiteral("donor_partition_number"));
}

bool nonNativeToolUnsupportedOperation(const PartitionOperation& operation) {
    return isNonNativeFileSystemToolOperation(operation) &&
           !isSupportedNonNativeFileSystemToolOperation(operation);
}

bool nonNativeFileSystemSupportedForOperation(PartitionOperationType type,
                                              const QString& fileSystem) {
    if (isExtFileSystemToken(fileSystem)) {
        return true;
    }
    const bool createOrFormat = type == PartitionOperationType::Create ||
                                type == PartitionOperationType::Format;
    if (isLinuxSwapFileSystemToken(fileSystem)) {
        return createOrFormat;
    }
    const bool createFormatOrCheck = createOrFormat ||
                                     type == PartitionOperationType::CheckFileSystem;
    const bool apfsRootMutation = isApfsRootFileMutationOperation(type);
    if (apfsRootMutation) {
        return isApfsFileSystemToken(fileSystem);
    }
    if (isHfsFileMutationOperation(type)) {
        return isHfsFileSystemToken(fileSystem);
    }
    return createFormatOrCheck &&
           (isHfsFileSystemToken(fileSystem) || isApfsFileSystemToken(fileSystem));
}

bool nonNativeToolUnsupportedFileSystem(const PartitionOperation& operation) {
    if (!isNonNativeFileSystemToolOperation(operation)) {
        return false;
    }
    const QString fileSystem = operation.payload.value(QStringLiteral("file_system")).toString();
    return !nonNativeFileSystemSupportedForOperation(operation.type, fileSystem);
}

bool nonNativeToolMissingTarget(const PartitionOperation& operation) {
    return isNonNativeFileSystemToolOperation(operation) &&
           operation.type != PartitionOperationType::Create &&
           !targetPathMatchesSelectedRawPartition(operation);
}

bool nonNativeToolMissingConfirmation(const PartitionOperation& operation) {
    return isNonNativeFileSystemToolOperation(operation) &&
           !payloadBool(operation, QStringLiteral("target_wipe_confirmed"));
}

bool apfsRootFileMutationMissingToolMarker(const PartitionOperation& operation) {
    return isApfsRootFileMutationOperation(operation.type) &&
           !isNonNativeFileSystemToolOperation(operation);
}

bool apfsRootFileMutationUnsupportedFileSystem(const PartitionOperation& operation) {
    return isApfsRootFileMutationOperation(operation.type) &&
           !isApfsFileSystemToken(
               operation.payload.value(QStringLiteral("file_system")).toString());
}

bool apfsRootFileMutationMissingTarget(const PartitionOperation& operation) {
    return isApfsRootFileMutationOperation(operation.type) &&
           !targetPathMatchesSelectedRawPartition(operation);
}

bool apfsRootFileMutationMissingConfirmation(const PartitionOperation& operation) {
    return isApfsRootFileMutationOperation(operation.type) &&
           !payloadBool(operation, QStringLiteral("target_wipe_confirmed"));
}

bool apfsRootFileMutationMissingLayoutConfirmation(const PartitionOperation& operation) {
    return isApfsRootFileMutationOperation(operation.type) &&
           !payloadBool(operation, QString::fromLatin1(kApfsGeneratedLayoutConfirmedPayload));
}

bool apfsRootFileMutationMissingFileName(const PartitionOperation& operation) {
    if (!isApfsRootFileMutationOperation(operation.type)) {
        return false;
    }
    if (operation.type == PartitionOperationType::ApfsChangeVolumeLabel) {
        return false;
    }
    const QString directoryName = operation.payload
                                      .value(QString::fromLatin1(kApfsRootDirectoryNamePayload))
                                      .toString()
                                      .trimmed();
    const QString fileName =
        operation.payload.value(QString::fromLatin1(kApfsRootFileNamePayload)).toString().trimmed();
    if (apfsRootDirectoryMutation(operation.type)) {
        return directoryName.isEmpty() && fileName.isEmpty();
    }
    if (apfsRootDirectoryNameRequired(operation.type) && directoryName.isEmpty()) {
        return true;
    }
    return apfsRootFileNameRequired(operation.type) && fileName.isEmpty();
}

QString apfsRootMutationMissingNameMessage(PartitionOperationType type) {
    if (apfsRootDirectoryFileMutation(type)) {
        return QStringLiteral(
            "APFS root-directory file mutation requires a root directory name and child file name");
    }
    return apfsRootDirectoryMutation(type)
               ? QStringLiteral("APFS root-directory mutation requires a root directory name")
               : QStringLiteral("APFS root-file mutation requires a root file name");
}

bool apfsRootFileMutationMissingPayload(const PartitionOperation& operation) {
    if (!apfsRootFileMutationWritesPayload(operation.type)) {
        return false;
    }
    const QString payloadBase64 = operation.payload
                                      .value(QString::fromLatin1(kApfsRootFilePayloadBase64))
                                      .toString()
                                      .trimmed();
    const bool hasText =
        operation.payload.contains(QString::fromLatin1(kApfsRootFilePayloadText)) &&
        !operation.payload.value(QString::fromLatin1(kApfsRootFilePayloadText))
             .toString()
             .isEmpty();
    return payloadBase64.isEmpty() && !hasText;
}

bool apfsVolumeLabelMutationMissingLabel(const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::ApfsChangeVolumeLabel &&
           operation.payload.value(QStringLiteral("label")).toString().trimmed().isEmpty();
}

bool apfsVolumeLabelMutationInvalidLabel(const PartitionOperation& operation) {
    if (operation.type != PartitionOperationType::ApfsChangeVolumeLabel) {
        return false;
    }
    const QString label = operation.payload.value(QStringLiteral("label")).toString().trimmed();
    return label.size() > kApfsVolumeLabelMaxChars ||
           label.toUtf8().size() >= kApfsVolumeLabelFieldBytes ||
           label.contains(QLatin1Char('/')) || label.contains(QLatin1Char('\\')) ||
           label.contains(QLatin1Char(':'));
}

bool apfsRootFilePatchMissingOffset(const PartitionOperation& operation) {
    return (operation.type == PartitionOperationType::ApfsPatchRootFile ||
            operation.type == PartitionOperationType::ApfsPatchRootDirectoryFile) &&
           !operation.payload.contains(QString::fromLatin1(kApfsRootFilePatchOffsetPayload));
}

bool hfsFileMutationMissingToolMarker(const PartitionOperation& operation) {
    return isHfsFileMutationOperation(operation.type) &&
           !isNonNativeFileSystemToolOperation(operation);
}

bool hfsFileMutationUnsupportedFileSystem(const PartitionOperation& operation) {
    return isHfsFileMutationOperation(operation.type) &&
           !isHfsFileSystemToken(operation.payload.value(QStringLiteral("file_system")).toString());
}

bool hfsFileMutationMissingTarget(const PartitionOperation& operation) {
    return isHfsFileMutationOperation(operation.type) &&
           !targetPathMatchesSelectedRawPartition(operation);
}

bool hfsFileMutationMissingConfirmation(const PartitionOperation& operation) {
    return isHfsFileMutationOperation(operation.type) &&
           !payloadBool(operation, QStringLiteral("target_wipe_confirmed"));
}

bool hfsFileMutationMissingPath(const PartitionOperation& operation) {
    return isHfsFileMutationOperation(operation.type) && hfsFileMutationNeedsPath(operation.type) &&
           operation.payload.value(QString::fromLatin1(kHfsPathPayload))
               .toString()
               .trimmed()
               .isEmpty();
}

bool hfsRenameMoveMissingDestinationPath(const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::HfsRenameMoveCatalogEntry &&
           operation.payload.value(QString::fromLatin1(kHfsDestinationPathPayload))
               .toString()
               .trimmed()
               .isEmpty();
}

bool hfsFileMutationMissingPayload(const PartitionOperation& operation) {
    if (!hfsFileMutationWritesPayload(operation.type)) {
        return false;
    }
    const QString payloadBase64 =
        operation.payload.value(QString::fromLatin1(kHfsPayloadBase64)).toString().trimmed();
    const bool hasText =
        operation.payload.contains(QString::fromLatin1(kHfsPayloadText)) &&
        !operation.payload.value(QString::fromLatin1(kHfsPayloadText)).toString().isEmpty();
    return payloadBase64.isEmpty() && !hasText;
}

bool hfsAttributeMutationMissingIdentity(const PartitionOperation& operation) {
    if (operation.type != PartitionOperationType::HfsReplaceInlineAttribute &&
        operation.type != PartitionOperationType::HfsReplaceForkAttribute &&
        operation.type != PartitionOperationType::HfsGrowForkAttribute) {
        return false;
    }
    return payloadUInt64(operation, QString::fromLatin1(kHfsFileIdPayload)) == 0 ||
           operation.payload.value(QString::fromLatin1(kHfsAttributeNamePayload))
               .toString()
               .trimmed()
               .isEmpty();
}

bool hfsSecureWipeUnsupported(const PartitionOperation& operation) {
    if (!isHfsFileMutationOperation(operation.type) ||
        !payloadBool(operation, QString::fromLatin1(kHfsSecureWipeReleasedBlocksPayload))) {
        return false;
    }
    return operation.type != PartitionOperationType::HfsDeleteFile &&
           operation.type != PartitionOperationType::HfsDeleteFolderTree;
}

bool nonNativeResizeShrinkNeedsUsageMetadata(const PartitionInfoEx& partition,
                                             const PartitionOperation& operation) {
    return isNonNativeFileSystemToolOperation(operation) &&
           operation.type == PartitionOperationType::Resize &&
           payloadUInt64(operation, QStringLiteral("target_size_bytes")) < partition.size_bytes &&
           (!partition.volume || partition.volume->total_bytes == 0 ||
            partition.volume->total_bytes < partition.volume->free_bytes);
}

bool blocksTooSmallCloneTarget(const PartitionOperation& operation) {
    const bool cloneOperation = operation.type == PartitionOperationType::CloneDisk ||
                                operation.type == PartitionOperationType::MigrateOs ||
                                operation.type == PartitionOperationType::RestoreImage;
    const uint64_t sourceSize = payloadUInt64(operation, QStringLiteral("source_size_bytes"));
    const uint64_t targetSize = payloadUInt64(operation, QStringLiteral("target_size_bytes"));
    return cloneOperation && sourceSize != 0 && targetSize != 0 && sourceSize > targetSize;
}

bool restoreImageMissingKnownSizes(const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::RestoreImage &&
           (payloadUInt64(operation, QStringLiteral("source_size_bytes")) == 0 ||
            payloadUInt64(operation, QStringLiteral("target_size_bytes")) == 0);
}

bool clonePartitionTargetsRawDevice(const PartitionOperation& operation) {
    const QString targetPath =
        operation.payload.value(QStringLiteral("target_path")).toString().trimmed();
    return operation.type == PartitionOperationType::ClonePartition &&
           targetPath.startsWith(QStringLiteral("\\\\.\\"), Qt::CaseInsensitive);
}

bool clonePartitionTargetsRegion(const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::ClonePartition &&
           (operation.payload.contains(QStringLiteral("target_disk_number")) ||
            operation.payload.contains(QStringLiteral("target_offset_bytes")));
}

bool clonePartitionRegionMissingFields(const PartitionOperation& operation) {
    if (!clonePartitionTargetsRegion(operation)) {
        return false;
    }
    const QString targetPath =
        operation.payload.value(QStringLiteral("target_path")).toString().trimmed();
    return !operation.payload.contains(QStringLiteral("target_disk_number")) ||
           !targetPath.startsWith(QStringLiteral("\\\\.\\PhysicalDrive"), Qt::CaseInsensitive) ||
           payloadUInt64(operation, QStringLiteral("target_size_bytes")) == 0;
}

bool clonePartitionPhysicalTargetMissingRegion(const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::ClonePartition &&
           targetPathIsPhysicalDrive(operation) && !clonePartitionTargetsRegion(operation);
}

bool blocksTooSmallPartitionCloneTarget(const PartitionOperation& operation) {
    if (operation.type != PartitionOperationType::ClonePartition) {
        return false;
    }
    const uint64_t sourceSize = payloadUInt64(operation, QStringLiteral("source_size_bytes"));
    const uint64_t targetSize = payloadUInt64(operation, QStringLiteral("target_size_bytes"));
    return sourceSize != 0 && targetSize != 0 && sourceSize > targetSize;
}

bool requiresPartitionCloneTargetConfirmation(const PartitionOperation& operation) {
    return (clonePartitionTargetsRawDevice(operation) || clonePartitionTargetsRegion(operation)) &&
           !payloadBool(operation, QStringLiteral("target_wipe_confirmed"));
}

bool recoveryCandidateOverlapsExistingPartition(const PartitionDiskInfo& disk,
                                                const PartitionOperation& operation) {
    if (operation.type != PartitionOperationType::RestoreRecoveredPartition) {
        return false;
    }
    const uint64_t start = payloadUInt64(operation, QStringLiteral("offset_bytes"));
    const uint64_t size = payloadUInt64(operation, QStringLiteral("size_bytes"));
    if (start == 0 || size == 0) {
        return false;
    }
    const uint64_t end = start + size;
    return std::any_of(disk.partitions.cbegin(),
                       disk.partitions.cend(),
                       [start, end](const auto& p) {
                           const uint64_t partition_end = p.offset_bytes + p.size_bytes;
                           return start < partition_end && end > p.offset_bytes;
                       });
}

bool recoveryCandidateExceedsDisk(const PartitionDiskInfo& disk,
                                  const PartitionOperation& operation) {
    if (operation.type != PartitionOperationType::RestoreRecoveredPartition) {
        return false;
    }
    const uint64_t start = payloadUInt64(operation, QStringLiteral("offset_bytes"));
    const uint64_t size = payloadUInt64(operation, QStringLiteral("size_bytes"));
    return start != 0 && size != 0 && (start > disk.size_bytes || size > disk.size_bytes - start);
}

bool missingRecoveryCandidateFields(const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::RestoreRecoveredPartition &&
           (payloadUInt64(operation, QStringLiteral("offset_bytes")) == 0 ||
            payloadUInt64(operation, QStringLiteral("size_bytes")) == 0 ||
            operation.payload.value(QStringLiteral("type_id")).toString().trimmed().isEmpty());
}

bool restoreOrMigrateMissingPhysicalTarget(const PartitionOperation& operation) {
    return (operation.type == PartitionOperationType::RestoreImage ||
            operation.type == PartitionOperationType::MigrateOs) &&
           !targetPathIsPhysicalDrive(operation);
}

bool payloadTargetDiskMustExist(const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::CloneDisk ||
           operation.type == PartitionOperationType::MigrateOs ||
           operation.type == PartitionOperationType::RestoreImage ||
           operation.type == PartitionOperationType::ClonePartition;
}

bool isUnsafeRawWriteTargetDisk(const PartitionDiskInfo& disk) {
    return disk.is_system || disk.is_boot || disk.is_read_only || disk.is_dynamic ||
           disk.is_storage_spaces;
}

bool isFatToNtfsConversion(const PartitionInfoEx& partition, PartitionOperationType type) {
    if (type != PartitionOperationType::ConvertFileSystem || !partition.volume) {
        return true;
    }
    const QString fs = partition.volume->file_system.toUpper();
    return fs == QStringLiteral("FAT") || fs == QStringLiteral("FAT32");
}

bool mergeSourceIsAdjacentAfterTarget(const PartitionDiskInfo& disk,
                                      const PartitionInfoEx& target,
                                      uint32_t source_partition_number) {
    const auto* source = PartitionSafetyValidator::findPartition(disk, source_partition_number);
    return source && source->offset_bytes == target.offset_bytes + target.size_bytes;
}

const PartitionInfoEx* allocateDonorPartition(const PartitionDiskInfo& disk,
                                              const PartitionOperation& operation) {
    if (operation.type != PartitionOperationType::AllocateFreeSpace) {
        return nullptr;
    }
    const auto sourcePartition =
        static_cast<uint32_t>(payloadUInt64(operation, QStringLiteral("source_partition_number")));
    return PartitionSafetyValidator::findPartition(disk, sourcePartition);
}

bool allocationDonorSizeMismatch(const PartitionInfoEx* donor,
                                 const PartitionOperation& operation) {
    if (operation.type != PartitionOperationType::AllocateFreeSpace || !donor) {
        return false;
    }
    const uint64_t sourceSize = payloadUInt64(operation, QStringLiteral("source_size_bytes"));
    return sourceSize == 0 || sourceSize != donor->size_bytes;
}

bool allocationDonorIsAdjacentAfterTarget(const PartitionInfoEx& target,
                                          const PartitionInfoEx* donor,
                                          const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::AllocateFreeSpace && donor &&
           donor->offset_bytes == target.offset_bytes + target.size_bytes;
}

bool allocationDonorIsProtected(const PartitionInfoEx* donor, const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::AllocateFreeSpace && donor &&
           (PartitionSafetyValidator::isSystemProtectedPartition(*donor) || donor->is_read_only);
}

bool allocationDonorMissingMountedVolume(const PartitionInfoEx* donor,
                                         const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::AllocateFreeSpace && donor &&
           (!donor->volume || donor->volume->drive_letter.isEmpty());
}

bool allocationDonorVolumePayloadMismatch(const PartitionInfoEx* donor,
                                          const PartitionOperation& operation) {
    if (operation.type != PartitionOperationType::AllocateFreeSpace || !donor || !donor->volume) {
        return false;
    }
    const QString payloadDrive =
        operation.payload.value(QStringLiteral("source_drive_letter")).toString().left(1).toUpper();
    const QString payloadFileSystem = operation.payload.value(QStringLiteral("source_file_system"))
                                          .toString()
                                          .trimmed()
                                          .toUpper();
    return payloadDrive != donor->volume->drive_letter.left(1).toUpper() ||
           payloadFileSystem != donor->volume->file_system.trimmed().toUpper();
}

bool allocationAmountMissing(const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::AllocateFreeSpace &&
           payloadUInt64(operation, QStringLiteral("bytes_to_allocate")) == 0;
}

bool allocationLeavesDonorTooSmall(const PartitionInfoEx* donor,
                                   const PartitionOperation& operation) {
    if (operation.type != PartitionOperationType::AllocateFreeSpace || !donor) {
        return false;
    }
    const uint64_t bytes = payloadUInt64(operation, QStringLiteral("bytes_to_allocate"));
    if (bytes == 0 || bytes >= donor->size_bytes) {
        return bytes >= donor->size_bytes;
    }
    const uint64_t remainingBytes = donor->size_bytes - bytes;
    return remainingBytes <
           saturatingAdd(usedBytes(*donor), kAllocateFreeSpaceMinimumRemainderBytes);
}

bool allocationMissingBackup(const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::AllocateFreeSpace &&
           normalizedBackupDirectory(operation).isEmpty();
}

bool allocationBackupOnTargetOrDonor(const PartitionInfoEx& target,
                                     const PartitionInfoEx* donor,
                                     const PartitionOperation& operation) {
    if (operation.type != PartitionOperationType::AllocateFreeSpace) {
        return false;
    }
    const QString backup = normalizedBackupDirectory(operation);
    return (target.volume && targetPathStartsWithDrive(backup, target.volume->drive_letter)) ||
           (donor && donor->volume &&
            targetPathStartsWithDrive(backup, donor->volume->drive_letter));
}

bool allocationMissingConfirmation(const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::AllocateFreeSpace &&
           !payloadBool(operation, QStringLiteral("target_wipe_confirmed"));
}

bool isSupportedBackupRestoreFileSystem(const QString& fs) {
    const QString normalized = fs.trimmed().toUpper();
    return normalized == QStringLiteral("NTFS") || normalized == QStringLiteral("FAT32") ||
           normalized == QStringLiteral("EXFAT");
}

QString payloadFileSystem(const PartitionOperation& operation, const PartitionInfoEx& partition) {
    return operation.payload.value(QStringLiteral("file_system"))
        .toString(partition.volume ? partition.volume->file_system : QStringLiteral("NTFS"))
        .trimmed();
}

bool backupRestorePartitionOperation(PartitionOperationType type) {
    return type == PartitionOperationType::MovePartition ||
           type == PartitionOperationType::ConvertPrimaryLogical ||
           type == PartitionOperationType::ChangeVolumeSerialNumber;
}

bool backupRestoreMissingMountedVolume(const PartitionInfoEx& partition,
                                       const PartitionOperation& operation) {
    return backupRestorePartitionOperation(operation.type) &&
           (!partition.volume || partition.volume->drive_letter.isEmpty());
}

bool backupRestoreMissingBackup(const PartitionOperation& operation) {
    return backupRestorePartitionOperation(operation.type) &&
           normalizedBackupDirectory(operation).isEmpty();
}

bool backupRestoreBackupOnTargetVolume(const PartitionInfoEx& partition,
                                       const PartitionOperation& operation) {
    return backupRestorePartitionOperation(operation.type) && partition.volume &&
           targetPathStartsWithDrive(normalizedBackupDirectory(operation),
                                     partition.volume->drive_letter);
}

bool backupRestoreMissingConfirmation(const PartitionOperation& operation) {
    return backupRestorePartitionOperation(operation.type) &&
           !payloadBool(operation, QStringLiteral("target_wipe_confirmed"));
}

bool backupRestoreUnsupportedFileSystem(const PartitionInfoEx& partition,
                                        const PartitionOperation& operation) {
    return backupRestorePartitionOperation(operation.type) &&
           !isSupportedBackupRestoreFileSystem(payloadFileSystem(operation, partition));
}

bool movePartitionMissingTarget(const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::MovePartition &&
           (payloadUInt64(operation, QStringLiteral("target_offset_bytes")) == 0 ||
            payloadUInt64(operation, QStringLiteral("target_size_bytes")) == 0);
}

bool movePartitionIsNoop(const PartitionInfoEx& partition, const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::MovePartition &&
           payloadUInt64(operation, QStringLiteral("target_offset_bytes")) ==
               partition.offset_bytes &&
           payloadUInt64(operation, QStringLiteral("target_size_bytes")) == partition.size_bytes;
}

bool movePartitionShrinksBelowUsedBytes(const PartitionInfoEx& partition,
                                        const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::MovePartition &&
           payloadUInt64(operation, QStringLiteral("target_size_bytes")) < usedBytes(partition);
}

bool movePartitionExceedsDisk(const PartitionDiskInfo& disk, const PartitionOperation& operation) {
    if (operation.type != PartitionOperationType::MovePartition) {
        return false;
    }
    const uint64_t start = payloadUInt64(operation, QStringLiteral("target_offset_bytes"));
    const uint64_t size = payloadUInt64(operation, QStringLiteral("target_size_bytes"));
    return start == 0 || size == 0 || start > disk.size_bytes || size > disk.size_bytes - start;
}

bool movePartitionOverlapsOtherPartitions(const PartitionDiskInfo& disk,
                                          const PartitionInfoEx& partition,
                                          const PartitionOperation& operation) {
    if (operation.type != PartitionOperationType::MovePartition ||
        movePartitionExceedsDisk(disk, operation)) {
        return false;
    }
    const uint64_t start = payloadUInt64(operation, QStringLiteral("target_offset_bytes"));
    const uint64_t end = start + payloadUInt64(operation, QStringLiteral("target_size_bytes"));
    return std::any_of(
        disk.partitions.cbegin(), disk.partitions.cend(), [&](const auto& candidate) {
            if (candidate.partition_number == partition.partition_number) {
                return false;
            }
            const uint64_t candidateEnd = candidate.offset_bytes + candidate.size_bytes;
            return start < candidateEnd && end > candidate.offset_bytes;
        });
}

bool primaryLogicalTargetLayoutInvalid(const PartitionOperation& operation) {
    if (operation.type != PartitionOperationType::ConvertPrimaryLogical) {
        return false;
    }
    const QString targetLayout =
        operation.payload.value(QStringLiteral("target_layout")).toString().trimmed().toLower();
    return targetLayout != QStringLiteral("primary") && targetLayout != QStringLiteral("logical");
}

bool primaryLogicalRequiresMbrDisk(const PartitionDiskInfo& disk,
                                   const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::ConvertPrimaryLogical &&
           disk.partition_style.compare(QStringLiteral("MBR"), Qt::CaseInsensitive) != 0;
}

bool primaryLogicalRequiresSingleVolumeDisk(const PartitionDiskInfo& disk,
                                            const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::ConvertPrimaryLogical &&
           disk.partitions.size() != 1;
}

bool dynamicToBasicMissingPayload(const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::ConvertDynamicDiskToBasic &&
           (payloadUInt64(operation, QStringLiteral("source_size_bytes")) == 0 ||
            operation.payload.value(QStringLiteral("drive_letter"))
                .toString()
                .trimmed()
                .isEmpty() ||
            operation.payload.value(QStringLiteral("file_system")).toString().trimmed().isEmpty() ||
            normalizedBackupDirectory(operation).isEmpty());
}

bool dynamicToBasicMissingConfirmation(const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::ConvertDynamicDiskToBasic &&
           !payloadBool(operation, QStringLiteral("target_wipe_confirmed"));
}

bool dynamicToBasicUnsupportedFileSystem(const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::ConvertDynamicDiskToBasic &&
           !isSupportedBackupRestoreFileSystem(
               operation.payload.value(QStringLiteral("file_system")).toString());
}

bool dynamicToBasicBackupOnSourceVolume(const PartitionOperation& operation) {
    if (operation.type != PartitionOperationType::ConvertDynamicDiskToBasic) {
        return false;
    }
    const QString drive = operation.payload.value(QStringLiteral("drive_letter")).toString();
    return targetPathStartsWithDrive(normalizedBackupDirectory(operation), drive);
}

bool dynamicToBasicUnsupportedDisk(const PartitionDiskInfo& disk,
                                   const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::ConvertDynamicDiskToBasic &&
           (!disk.is_dynamic || disk.partitions.size() != 1);
}

void validatePartitionTargetState(const PartitionDiskInfo& disk,
                                  const PartitionInfoEx& partition,
                                  const PartitionOperation& operation,
                                  PartitionValidationResult* result) {
    addBlockerIf(result,
                 disk.is_read_only || partition.is_read_only,
                 QStringLiteral("Target partition is read-only"));
    addBlockerIf(result,
                 disk.is_dynamic || disk.is_storage_spaces,
                 QStringLiteral("Dynamic disks and Storage Spaces are read-only in v1"));
    addBlockerIf(result,
                 blocksProtectedPartition(partition, operation.type),
                 QStringLiteral(
                     "System, boot, EFI, MSR, and recovery partitions are protected in v1"));
    addBlockerIf(result,
                 hasLockedBitLockerVolume(partition) &&
                     operation.type != PartitionOperationType::BitLockerUnlock,
                 QStringLiteral("BitLocker volume is locked"));
    addWarningIf(
        result,
        hasUnlockedBitLockerVolume(partition) && !isBitLockerManagementOperation(operation.type),
        QStringLiteral("BitLocker is enabled; suspend protection before applying changes"));
    addWarningIf(result,
                 hasDirtyVolume(partition, operation.type),
                 QStringLiteral("File system dirty bit is set; run repair first"));
}

void validatePartitionMetadataOperation(const PartitionDiskInfo& disk,
                                        const PartitionInfoEx& partition,
                                        const PartitionOperation& operation,
                                        PartitionValidationResult* result) {
    addBlockerIf(
        result,
        requiresDriveLetter(operation.type) && !isNonNativeFileSystemToolOperation(operation) &&
            (!partition.volume || partition.volume->drive_letter.isEmpty()),
        QStringLiteral("Selected operation requires a mounted volume with a drive letter"));
    addBlockerIf(result,
                 operation.type == PartitionOperationType::SetPartitionActive &&
                     disk.partition_style.compare(QStringLiteral("MBR"), Qt::CaseInsensitive) != 0,
                 QStringLiteral("Active flag is only valid on MBR disks"));
    addBlockerIf(
        result,
        operation.type == PartitionOperationType::SetPartitionTypeId &&
            operation.payload.value(QStringLiteral("type_id")).toString().trimmed().isEmpty(),
        QStringLiteral("Partition type change requires a type ID"));
    addBlockerIf(result,
                 operation.type == PartitionOperationType::DefragVolume && diskLooksSsd(disk),
                 QStringLiteral("HDD defrag is blocked on SSD/NVMe media"));
    addBlockerIf(result,
                 operation.type == PartitionOperationType::DefragVolume && !diskLooksHdd(disk),
                 QStringLiteral("HDD defrag requires media reported as HDD"));
    addBlockerIf(result,
                 backupRestoreMissingMountedVolume(partition, operation),
                 QStringLiteral("Operation requires a mounted volume with a drive letter"));
    addBlockerIf(result,
                 backupRestoreUnsupportedFileSystem(partition, operation),
                 QStringLiteral("Backup/recreate operation supports NTFS, FAT32, and exFAT only"));
    addBlockerIf(result,
                 backupRestoreMissingBackup(operation),
                 QStringLiteral("Operation requires an off-volume backup directory"));
    addBlockerIf(result,
                 backupRestoreBackupOnTargetVolume(partition, operation),
                 QStringLiteral("Backup directory must not be on the selected volume"));
    addBlockerIf(result,
                 backupRestoreMissingConfirmation(operation),
                 QStringLiteral("Operation requires destructive backup/restore confirmation"));
    addBlockerIf(result,
                 primaryLogicalRequiresMbrDisk(disk, operation),
                 QStringLiteral("Primary/logical conversion requires an MBR data disk"));
    addBlockerIf(result,
                 primaryLogicalRequiresSingleVolumeDisk(disk, operation),
                 QStringLiteral(
                     "Primary/logical conversion requires a single mounted data partition"));
    addBlockerIf(result,
                 primaryLogicalTargetLayoutInvalid(operation),
                 QStringLiteral(
                     "Primary/logical conversion requires primary or logical target layout"));
}

void validateImageRestoreContentBlockers(const PartitionInfoEx& partition,
                                         const PartitionOperation& operation,
                                         PartitionValidationResult* result) {
    addBlockerIf(result,
                 createImageTargetsRawDevice(operation),
                 QStringLiteral("Create Image destination must be a file path, not a raw device"));
    addBlockerIf(result,
                 requiresTargetOverwriteConfirmation(operation),
                 QStringLiteral("Target partition overwrite confirmation is required"));
    addBlockerIf(result,
                 restoreImageMissingKnownSizes(operation),
                 QStringLiteral("Restore Image requires known image and target sizes"));
    addBlockerIf(result,
                 operation.type == PartitionOperationType::RestoreImage &&
                     blocksTooSmallCloneTarget(operation),
                 QStringLiteral("Target partition is smaller than the image"));
    addBlockerIf(result,
                 createImageTargetsSourcePartition(partition, operation),
                 QStringLiteral("Create Image destination cannot be on the source partition"));
}

// A6: a Format that opts into FileVault encryption must carry a non-empty password and target
// APFS; otherwise the operation fails closed rather than emitting an inconsistent format.
bool apfsEncryptedFormatRequested(const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::Format &&
           payloadBool(operation, QString::fromLatin1(kApfsEncryptVolumePayload));
}

bool apfsEncryptedFormatMissingPassword(const PartitionOperation& operation) {
    return apfsEncryptedFormatRequested(operation) &&
           operation.payload.value(QString::fromLatin1(kApfsVolumePasswordPayload))
               .toString()
               .isEmpty();
}

bool apfsEncryptedFormatWrongFileSystem(const PartitionOperation& operation) {
    return apfsEncryptedFormatRequested(operation) &&
           !isApfsFileSystemToken(
               operation.payload.value(QStringLiteral("file_system")).toString());
}

void validateNonNativeToolContentBlockers(const PartitionInfoEx& partition,
                                          const PartitionOperation& operation,
                                          PartitionValidationResult* result) {
    addBlockerIf(result,
                 operation.type == PartitionOperationType::Format &&
                     operation.target.partition_number == 0,
                 QStringLiteral("Format requires a partition identity"));
    addBlockerIf(
        result,
        nonNativeToolUnsupportedOperation(operation),
        QStringLiteral(
            "Non-Windows filesystem tool support is limited to format, repair, resize, APFS "
            "generated root-file/root-directory mutation, and HFS+ staged file mutation"));
    addBlockerIf(
        result,
        nonNativeToolUnsupportedFileSystem(operation),
        QStringLiteral(
            "Non-Windows write support is limited to ext2/ext3/ext4 create/format/repair/resize, "
            "HFS+/HFSX create/format/repair/staged file mutation, Linux swap create/format, and "
            "APFS generated create/format/repair/root-file/root-directory mutation"));
    addBlockerIf(result,
                 nonNativeToolMissingTarget(operation),
                 QStringLiteral(
                     "Non-Windows filesystem tool target must match the selected raw partition"));
    addBlockerIf(result,
                 nonNativeToolMissingConfirmation(operation),
                 QStringLiteral(
                     "Non-Windows filesystem tool operation requires destructive confirmation"));
    addBlockerIf(result,
                 nonNativeResizeShrinkNeedsUsageMetadata(partition, operation),
                 QStringLiteral("Ext shrink requires detected filesystem usage metadata"));
    addBlockerIf(result,
                 apfsEncryptedFormatMissingPassword(operation),
                 QStringLiteral("APFS encrypted format requires a volume password"));
    addBlockerIf(result,
                 apfsEncryptedFormatWrongFileSystem(operation),
                 QStringLiteral("Volume encryption is only supported for APFS format"));
}

void validateApfsRootMutationContentBlockers(const PartitionOperation& operation,
                                             PartitionValidationResult* result) {
    addBlockerIf(result,
                 apfsRootFileMutationMissingToolMarker(operation),
                 QStringLiteral("APFS generated root mutation must use non-Windows tool payload"));
    addBlockerIf(result,
                 apfsRootFileMutationUnsupportedFileSystem(operation),
                 QStringLiteral("APFS generated root mutation requires APFS file system"));
    addBlockerIf(result,
                 apfsRootFileMutationMissingTarget(operation),
                 QStringLiteral(
                     "APFS generated root mutation target must match selected raw partition"));
    addBlockerIf(result,
                 apfsRootFileMutationMissingConfirmation(operation),
                 QStringLiteral("APFS generated root mutation requires destructive confirmation"));
    addBlockerIf(result,
                 apfsRootFileMutationMissingLayoutConfirmation(operation),
                 QStringLiteral(
                     "APFS generated root mutation requires S.A.K. generated-layout confirmation"));
    addBlockerIf(result,
                 apfsRootFileMutationMissingFileName(operation),
                 apfsRootMutationMissingNameMessage(operation.type));
    addBlockerIf(result,
                 apfsRootFileMutationMissingPayload(operation),
                 QStringLiteral("APFS root or child-file write/patch requires a payload"));
    addBlockerIf(result,
                 apfsVolumeLabelMutationMissingLabel(operation),
                 QStringLiteral("APFS volume-label mutation requires a label"));
    addBlockerIf(result,
                 apfsVolumeLabelMutationInvalidLabel(operation),
                 QStringLiteral("APFS volume-label mutation label must fit APFS UTF-8 field and "
                                "not contain path separators"));
    addBlockerIf(result,
                 apfsRootFilePatchMissingOffset(operation),
                 QStringLiteral("APFS root or child-file patch requires a byte offset"));
}

void validateHfsFileMutationContentBlockers(const PartitionOperation& operation,
                                            PartitionValidationResult* result) {
    addBlockerIf(result,
                 hfsFileMutationMissingToolMarker(operation),
                 QStringLiteral("HFS+ file mutation must use non-Windows tool payload"));
    addBlockerIf(result,
                 hfsFileMutationUnsupportedFileSystem(operation),
                 QStringLiteral("HFS+ file mutation requires HFS+ or HFSX file system"));
    addBlockerIf(result,
                 hfsFileMutationMissingTarget(operation),
                 QStringLiteral("HFS+ file mutation target must match selected raw partition"));
    addBlockerIf(result,
                 hfsFileMutationMissingConfirmation(operation),
                 QStringLiteral("HFS+ file mutation requires destructive confirmation"));
    addBlockerIf(result,
                 hfsFileMutationMissingPath(operation),
                 QStringLiteral("HFS+ file mutation requires an HFS path"));
    addBlockerIf(result,
                 hfsRenameMoveMissingDestinationPath(operation),
                 QStringLiteral("HFS+ rename/move requires a destination HFS path"));
    addBlockerIf(result,
                 hfsFileMutationMissingPayload(operation),
                 QStringLiteral("HFS+ write/replace mutation requires a payload"));
    addBlockerIf(result,
                 hfsAttributeMutationMissingIdentity(operation),
                 QStringLiteral("HFS+ attribute mutation requires file ID and attribute name"));
    addBlockerIf(result,
                 hfsSecureWipeUnsupported(operation),
                 QStringLiteral("HFS+ secure block wipe is supported only for delete-file or "
                                "delete-folder-tree mutations"));
}

void validateNonNativeContentBlockers(const PartitionInfoEx& partition,
                                      const PartitionOperation& operation,
                                      PartitionValidationResult* result) {
    validateNonNativeToolContentBlockers(partition, operation, result);
    validateApfsRootMutationContentBlockers(operation, result);
    validateHfsFileMutationContentBlockers(operation, result);
}

void validateResizeContentBlockers(const PartitionInfoEx& partition,
                                   const PartitionOperation& operation,
                                   PartitionValidationResult* result) {
    addBlockerIf(result,
                 resizeTargetMissing(operation),
                 QStringLiteral("Resize target size is required"));
    addBlockerIf(result,
                 resizeTargetIsNoop(partition, operation),
                 QStringLiteral("Resize target must change the selected partition size"));
    addBlockerIf(result,
                 resizeShrinksBelowUsedBytes(partition, operation),
                 QStringLiteral("Resize target is smaller than used volume space"));
    addBlockerIf(result,
                 resizeRequestsStartMove(operation),
                 QStringLiteral("Moving partition starts requires an offline move engine"));
    addBlockerIf(result,
                 resizeRequestsDonorSpace(operation),
                 QStringLiteral(
                     "Donor-space extend requires an offline move engine and certification"));
}

void validateConversionAndClusterBlockers(const PartitionInfoEx& partition,
                                          const PartitionOperation& operation,
                                          PartitionValidationResult* result) {
    addBlockerIf(result,
                 operation.type == PartitionOperationType::ConvertFileSystem &&
                     !isFatToNtfsConversion(partition, operation.type),
                 QStringLiteral("File-system conversion supports FAT/FAT32 to NTFS only"));
    addBlockerIf(result,
                 operation.type == PartitionOperationType::ChangeClusterSize &&
                     !isSupportedExplicitChangeClusterUnit(operation),
                 QStringLiteral(
                     "Cluster-size change requires an explicit supported allocation unit size"));
    addBlockerIf(result,
                 changeClusterMissingBackup(operation),
                 QStringLiteral("Cluster-size change requires an off-volume backup directory"));
    addBlockerIf(result,
                 changeClusterBackupOnTargetVolume(partition, operation),
                 QStringLiteral("Backup directory must not be on the selected volume"));
    addBlockerIf(result,
                 changeClusterMissingConfirmation(operation),
                 QStringLiteral("Cluster-size change requires destructive reformat confirmation"));
}

void validatePartitionCloneRegionBlockers(const PartitionOperation& operation,
                                          PartitionValidationResult* result) {
    addBlockerIf(result,
                 clonePartitionRegionMissingFields(operation),
                 QStringLiteral("Partition clone target region requires a physical disk path, "
                                "target disk, and target size"));
    addBlockerIf(result,
                 clonePartitionPhysicalTargetMissingRegion(operation),
                 QStringLiteral("Partition clone raw physical disk targets require an explicit "
                                "target disk and offset"));
    addBlockerIf(result,
                 blocksTooSmallPartitionCloneTarget(operation),
                 QStringLiteral("Target partition region is smaller than the source partition"));
    addBlockerIf(
        result,
        requiresPartitionCloneTargetConfirmation(operation),
        QStringLiteral(
            "Partition clone to a raw device or target region requires overwrite confirmation"));
}

void validateSplitMoveContentBlockers(const PartitionInfoEx& partition,
                                      const PartitionOperation& operation,
                                      PartitionValidationResult* result) {
    const auto split_size = payloadUInt64(operation, QStringLiteral("first_size_bytes"));
    addBlockerIf(result,
                 operation.type == PartitionOperationType::Split &&
                     (split_size == 0 || split_size >= partition.size_bytes),
                 QStringLiteral("Split size must be smaller than the selected partition"));
    addBlockerIf(result,
                 movePartitionMissingTarget(operation),
                 QStringLiteral("Move Partition requires target offset and size"));
    addBlockerIf(result,
                 movePartitionIsNoop(partition, operation),
                 QStringLiteral("Move Partition target must change offset or size"));
    addBlockerIf(result,
                 movePartitionShrinksBelowUsedBytes(partition, operation),
                 QStringLiteral("Move Partition target is smaller than used volume space"));
}

void validatePartitionContentOperation(const PartitionInfoEx& partition,
                                       const PartitionOperation& operation,
                                       PartitionValidationResult* result) {
    validateImageRestoreContentBlockers(partition, operation, result);
    validateNonNativeContentBlockers(partition, operation, result);
    validateResizeContentBlockers(partition, operation, result);
    validateConversionAndClusterBlockers(partition, operation, result);
    validatePartitionCloneRegionBlockers(operation, result);
    validateSplitMoveContentBlockers(partition, operation, result);
}

void validatePartitionCompositeOperation(const PartitionDiskInfo& disk,
                                         const PartitionInfoEx& partition,
                                         const PartitionOperation& operation,
                                         PartitionValidationResult* result) {
    const auto merge_source =
        static_cast<uint32_t>(payloadUInt64(operation, QStringLiteral("source_partition_number")));
    const auto* allocationDonor = allocateDonorPartition(disk, operation);
    addBlockerIf(result,
                 operation.type == PartitionOperationType::Merge &&
                     !mergeSourceIsAdjacentAfterTarget(disk, partition, merge_source),
                 QStringLiteral(
                     "Merge source must be an existing adjacent partition after target"));
    addBlockerIf(result,
                 operation.type == PartitionOperationType::AllocateFreeSpace && !allocationDonor,
                 QStringLiteral("Allocate Free Space requires an existing donor partition"));
    addBlockerIf(result,
                 allocationDonorSizeMismatch(allocationDonor, operation),
                 QStringLiteral("Allocate Free Space donor identity is missing or stale"));
    addBlockerIf(result,
                 operation.type == PartitionOperationType::AllocateFreeSpace && allocationDonor &&
                     !allocationDonorIsAdjacentAfterTarget(partition, allocationDonor, operation),
                 QStringLiteral(
                     "Allocate Free Space donor must be directly after the target partition"));
    addBlockerIf(result,
                 allocationDonorIsProtected(allocationDonor, operation),
                 QStringLiteral("Allocate Free Space donor partition is protected or read-only"));
    addBlockerIf(result,
                 allocationDonorMissingMountedVolume(allocationDonor, operation),
                 QStringLiteral("Allocate Free Space donor requires a mounted drive letter"));
    addBlockerIf(result,
                 allocationDonorVolumePayloadMismatch(allocationDonor, operation),
                 QStringLiteral("Allocate Free Space donor volume identity is missing or stale"));
    addBlockerIf(result,
                 allocationAmountMissing(operation),
                 QStringLiteral("Allocate Free Space amount is required"));
    addBlockerIf(result,
                 allocationLeavesDonorTooSmall(allocationDonor, operation),
                 QStringLiteral(
                     "Allocate Free Space donor must retain existing data plus safety reserve"));
    addBlockerIf(result,
                 allocationMissingBackup(operation),
                 QStringLiteral("Allocate Free Space requires an off-volume backup directory"));
    addBlockerIf(result,
                 allocationBackupOnTargetOrDonor(partition, allocationDonor, operation),
                 QStringLiteral("Backup directory must not be on the target or donor volume"));
    addBlockerIf(result,
                 allocationMissingConfirmation(operation),
                 QStringLiteral("Allocate Free Space requires destructive backup confirmation"));
    addBlockerIf(result,
                 resizeExtendsBeyondAdjacentFreeSpace(disk, partition, operation),
                 QStringLiteral("Resize target exceeds contiguous free space after the partition"));
    addBlockerIf(result,
                 movePartitionExceedsDisk(disk, operation),
                 QStringLiteral("Move Partition target exceeds disk bounds"));
    addBlockerIf(result,
                 movePartitionOverlapsOtherPartitions(disk, partition, operation),
                 QStringLiteral("Move Partition target overlaps another partition"));
    addWarningIf(result,
                 (operation.type == PartitionOperationType::WipePartition ||
                  operation.type == PartitionOperationType::WipeFreeSpace) &&
                     disk.media_type.contains(QStringLiteral("SSD"), Qt::CaseInsensitive),
                 QStringLiteral(
                     "SSD wipe uses software clear; vendor secure erase is required for purge"));
}

}  // namespace

const PartitionDiskInfo* PartitionSafetyValidator::findDisk(const PartitionInventory& inventory,
                                                            uint32_t disk_number) {
    auto it = std::find_if(inventory.disks.begin(),
                           inventory.disks.end(),
                           [disk_number](const auto& d) { return d.disk_number == disk_number; });
    return it == inventory.disks.end() ? nullptr : &(*it);
}

const PartitionInfoEx* PartitionSafetyValidator::findPartition(const PartitionDiskInfo& disk,
                                                               uint32_t partition_number) {
    auto it = std::find_if(disk.partitions.begin(),
                           disk.partitions.end(),
                           [partition_number](const auto& p) {
                               return p.partition_number == partition_number;
                           });
    return it == disk.partitions.end() ? nullptr : &(*it);
}

bool PartitionSafetyValidator::isSystemProtectedPartition(const PartitionInfoEx& partition) {
    return partition.is_system || partition.is_boot || partition.is_efi || partition.is_msr ||
           partition.is_recovery;
}

PartitionValidationResult PartitionSafetyValidator::validate(
    const PartitionInventory& inventory, const PartitionOperation& operation) const {
    PartitionValidationResult result;
    const PartitionDiskInfo* disk = findDisk(inventory, operation.target.disk_number);
    if (!disk) {
        result.blockers.append(QStringLiteral("Target disk was not found in current inventory"));
        return result;
    }

    addCommonDiskWarnings(*disk, &result);

    switch (operation.target.kind) {
    case PartitionTargetKind::Disk:
        validateDiskOperation(inventory, *disk, operation, &result);
        break;
    case PartitionTargetKind::Partition:
    case PartitionTargetKind::Volume: {
        const PartitionInfoEx* partition = findPartition(*disk, operation.target.partition_number);
        if (!partition) {
            result.blockers.append(QStringLiteral("Target partition was not found"));
            break;
        }
        validatePartitionOperation(inventory, *disk, *partition, operation, &result);
        break;
    }
    case PartitionTargetKind::Unallocated:
        validateUnallocatedOperation(*disk, operation, &result);
        break;
    }

    return result;
}

void validateDiskStateBlockers(const PartitionDiskInfo& disk,
                               const PartitionOperation& operation,
                               PartitionValidationResult* result) {
    addBlockerIf(result, disk.is_read_only, QStringLiteral("Target disk is read-only"));
    addBlockerIf(result,
                 (disk.is_dynamic && !allowsDynamicDiskOperation(operation.type)) ||
                     disk.is_storage_spaces,
                 QStringLiteral("Dynamic disks and Storage Spaces are read-only in v1"));
    addBlockerIf(result,
                 blocksCurrentOsDiskMutation(disk, operation.type),
                 QStringLiteral("Current OS disk destructive mutation is blocked in v1"));
    addBlockerIf(result,
                 operation.type == PartitionOperationType::WipeDisk && disk.is_system,
                 QStringLiteral("Current OS disk wipe is blocked"));
}

void validateDynamicToBasicBlockers(const PartitionDiskInfo& disk,
                                    const PartitionOperation& operation,
                                    PartitionValidationResult* result) {
    addBlockerIf(result,
                 dynamicToBasicUnsupportedDisk(disk, operation),
                 QStringLiteral(
                     "Dynamic-to-basic conversion requires one mounted simple dynamic volume"));
    addBlockerIf(
        result,
        dynamicToBasicMissingPayload(operation),
        QStringLiteral(
            "Dynamic-to-basic conversion requires source volume, size, file system, and backup"));
    addBlockerIf(result,
                 dynamicToBasicUnsupportedFileSystem(operation),
                 QStringLiteral(
                     "Dynamic-to-basic conversion supports NTFS, FAT32, and exFAT only"));
    addBlockerIf(result,
                 dynamicToBasicBackupOnSourceVolume(operation),
                 QStringLiteral("Backup directory must not be on the dynamic source volume"));
    addBlockerIf(
        result,
        dynamicToBasicMissingConfirmation(operation),
        QStringLiteral(
            "Dynamic-to-basic conversion requires destructive backup/restore confirmation"));
}

void validateDiskStyleBlockers(const PartitionDiskInfo& disk,
                               const PartitionOperation& operation,
                               PartitionValidationResult* result) {
    addBlockerIf(result,
                 blocksDataDiskStyleConversion(disk, operation.type),
                 QStringLiteral(
                     "Data disk partition-style conversion requires an empty disk in v1"));
    addBlockerIf(result,
                 operation.type == PartitionOperationType::InitializeDisk &&
                     !disk.partitions.isEmpty(),
                 QStringLiteral("Initialize Disk requires an empty/raw disk"));
    addBlockerIf(result,
                 operation.type == PartitionOperationType::DeleteAllPartitions &&
                     disk.partitions.isEmpty(),
                 QStringLiteral("Disk has no partitions to delete"));
    addBlockerIf(result,
                 blocksUnsafeSystemStyleConversion(disk, operation),
                 QStringLiteral("System disk conversion must use MBR2GPT to GPT"));
}

void validateDiskCloneImageBlockers(const PartitionDiskInfo& disk,
                                    const PartitionOperation& operation,
                                    PartitionValidationResult* result) {
    addBlockerIf(result,
                 operation.type == PartitionOperationType::MigrateOs && !disk.is_system,
                 QStringLiteral("OS migration source must be a system disk"));
    addBlockerIf(result,
                 restoreOrMigrateMissingPhysicalTarget(operation),
                 QStringLiteral("Restore Image and OS migration require a physical target disk"));
    addBlockerIf(result,
                 requiresTargetOverwriteConfirmation(operation),
                 QStringLiteral("Target disk overwrite confirmation is required"));
    addBlockerIf(result,
                 restoreImageMissingKnownSizes(operation),
                 QStringLiteral("Restore Image requires known image and target sizes"));
    addBlockerIf(result,
                 blocksTooSmallCloneTarget(operation),
                 QStringLiteral("Target disk is smaller than the source disk"));
    addBlockerIf(result,
                 createImageTargetsRawDevice(operation),
                 QStringLiteral("Create Image destination must be a file path, not a raw device"));
    addBlockerIf(result,
                 createImageTargetsSourceDisk(disk, operation),
                 QStringLiteral("Create Image destination cannot be on the source disk"));
}

void validateDiskRecoveryBlockers(const PartitionDiskInfo& disk,
                                  const PartitionOperation& operation,
                                  PartitionValidationResult* result) {
    addBlockerIf(result,
                 requiresRecoveryRestoreAcknowledgement(operation),
                 QStringLiteral("Recovered partition restore acknowledgement is required"));
    addBlockerIf(result,
                 missingRecoveryCandidateFields(operation),
                 QStringLiteral("Recovered partition restore requires offset, size, and type ID"));
    addBlockerIf(result,
                 recoveryCandidateOverlapsExistingPartition(disk, operation),
                 QStringLiteral("Recovered partition candidate overlaps an existing partition"));
    addBlockerIf(result,
                 recoveryCandidateExceedsDisk(disk, operation),
                 QStringLiteral("Recovered partition candidate exceeds disk bounds"));
}

void PartitionSafetyValidator::validateDiskOperation(const PartitionInventory& inventory,
                                                     const PartitionDiskInfo& disk,
                                                     const PartitionOperation& operation,
                                                     PartitionValidationResult* result) const {
    validatePayloadRawWriteTarget(inventory, disk, operation, result);
    validateDiskStateBlockers(disk, operation, result);
    validateDynamicToBasicBlockers(disk, operation, result);
    validateDiskStyleBlockers(disk, operation, result);
    validateDiskCloneImageBlockers(disk, operation, result);
    validateDiskRecoveryBlockers(disk, operation, result);
    addWarningIf(result,
                 operation.type == PartitionOperationType::WipeDisk &&
                     disk.media_type.contains(QStringLiteral("SSD"), Qt::CaseInsensitive),
                 QStringLiteral(
                     "SSD wipe uses software clear; vendor secure erase is required for purge"));
}

void PartitionSafetyValidator::validatePayloadRawWriteTarget(
    const PartitionInventory& inventory,
    const PartitionDiskInfo& selectedDisk,
    const PartitionOperation& operation,
    PartitionValidationResult* result) const {
    if (restoreOrMigrateMissingPhysicalTarget(operation)) {
        return;
    }

    const auto targetDiskNumber = targetPhysicalDriveNumber(operation);
    if (!targetDiskNumber.has_value()) {
        return;
    }

    const PartitionDiskInfo* targetDisk = findDisk(inventory, *targetDiskNumber);
    addBlockerIf(result,
                 payloadTargetDiskMustExist(operation) && targetDisk == nullptr,
                 QStringLiteral("Payload target disk was not found in current inventory"));
    if (!targetDisk) {
        return;
    }

    addCommonDiskWarnings(*targetDisk, result);
    addBlockerIf(result,
                 isUnsafeRawWriteTargetDisk(*targetDisk),
                 QStringLiteral(
                     "Payload target disk is system, boot, read-only, dynamic, or Storage Spaces"));

    if (operation.type == PartitionOperationType::CloneDisk ||
        operation.type == PartitionOperationType::MigrateOs) {
        addBlockerIf(result,
                     targetDisk->disk_number == selectedDisk.disk_number,
                     QStringLiteral("Source and payload target disk must be different"));
    }

    if (operation.type == PartitionOperationType::RestoreImage) {
        addBlockerIf(result,
                     targetDisk->disk_number != selectedDisk.disk_number,
                     QStringLiteral("Restore Image payload target must match the selected disk"));
    }

    if (operation.type == PartitionOperationType::ClonePartition &&
        operation.payload.contains(QStringLiteral("target_disk_number"))) {
        const uint32_t payloadDisk =
            static_cast<uint32_t>(payloadUInt64(operation, QStringLiteral("target_disk_number")));
        addBlockerIf(result,
                     payloadDisk != targetDisk->disk_number,
                     QStringLiteral(
                         "Partition clone target disk number must match the physical target path"));
    }
}

void PartitionSafetyValidator::validatePartitionOperation(const PartitionInventory& inventory,
                                                          const PartitionDiskInfo& disk,
                                                          const PartitionInfoEx& partition,
                                                          const PartitionOperation& operation,
                                                          PartitionValidationResult* result) const {
    validatePayloadRawWriteTarget(inventory, disk, operation, result);
    validatePartitionTargetState(disk, partition, operation, result);
    validatePartitionMetadataOperation(disk, partition, operation, result);
    validatePartitionContentOperation(partition, operation, result);
    validatePartitionCompositeOperation(disk, partition, operation, result);
}

void PartitionSafetyValidator::validateUnallocatedOperation(
    const PartitionDiskInfo& disk,
    const PartitionOperation& operation,
    PartitionValidationResult* result) const {
    if (operation.type != PartitionOperationType::Create) {
        result->blockers.append(QStringLiteral("Only create is valid for unallocated space"));
    }
    if (operation.type == PartitionOperationType::Create) {
        validateCreateUnallocatedPayload(operation, result);
        addBlockerIf(result,
                     nonNativeToolUnsupportedOperation(operation),
                     QStringLiteral("Non-Windows filesystem tool support is limited to create, "
                                    "format, repair, and resize"));
        addBlockerIf(result,
                     nonNativeToolUnsupportedFileSystem(operation),
                     QStringLiteral(
                         "Non-Windows write support is limited to ext2/ext3/ext4 "
                         "create/format/repair/resize, HFS+/HFSX create/format/repair/staged file "
                         "mutation, Linux swap create/format, and APFS generated "
                         "create/format/repair/root-file/root-directory mutation"));
        addBlockerIf(
            result,
            nonNativeToolMissingConfirmation(operation),
            QStringLiteral(
                "Non-Windows filesystem tool operation requires destructive confirmation"));
        addBlockerIf(result,
                     isApfsFileSystemToken(
                         operation.payload.value(QStringLiteral("file_system")).toString()) &&
                         disk.partition_style.compare(QStringLiteral("GPT"), Qt::CaseInsensitive) !=
                             0,
                     QStringLiteral("APFS create requires a GPT disk"));
    }
    if (disk.is_system) {
        result->blockers.append(
            QStringLiteral("Creating partitions on current OS disk is blocked in v1"));
    }
    if (disk.is_read_only) {
        result->blockers.append(QStringLiteral("Target disk is read-only"));
    }
    if (disk.is_dynamic || disk.is_storage_spaces) {
        result->blockers.append(
            QStringLiteral("Dynamic disks and Storage Spaces are read-only in v1"));
    }
}

void PartitionSafetyValidator::addCommonDiskWarnings(const PartitionDiskInfo& disk,
                                                     PartitionValidationResult* result) const {
    if (disk.is_removable) {
        result->warnings.append(QStringLiteral("Target disk is removable"));
    }
    if (disk.health_status.compare(QStringLiteral("Healthy"), Qt::CaseInsensitive) != 0 &&
        !disk.health_status.isEmpty()) {
        result->warnings.append(QStringLiteral("Disk health is %1").arg(disk.health_status));
    }
}

}  // namespace sak

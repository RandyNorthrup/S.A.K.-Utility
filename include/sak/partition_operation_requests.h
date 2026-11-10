// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_operation_requests.h
/// @brief What a person's choices in a Partition Manager dialog become.
///
/// The panel reads these choices from its widgets, and the certification runner is given them.
/// Both queue what these functions build, so what certification runs is what Apply runs: a copy of
/// this code in the runner would drift from the panel, and certify the copy.

#pragma once

#include "sak/partition_manager_types.h"

#include <QJsonObject>
#include <QString>
#include <QVector>

#include <cstdint>
#include <optional>

namespace sak {

/// MBR holds four partitions, and Windows may represent the fourth as a logical one.
inline constexpr int kQuickPartitionMbrDataMaxCount = 4;
inline constexpr int kQuickPartitionMinimumSizeMb = 64;
inline constexpr int kQuickPartitionReservedTailMb = 32;

// Targets, as a row of the panel's table or a choice in a wizard selects them.
[[nodiscard]] PartitionTarget diskOperationTarget(const PartitionDiskInfo& disk);
[[nodiscard]] PartitionTarget partitionSelectionTarget(const PartitionInfoEx& partition);
[[nodiscard]] PartitionTarget partitionIdentityTarget(const PartitionInfoEx& partition);
[[nodiscard]] PartitionTarget unallocatedOperationTarget(DiskNumber disk_number,
                                                         uint64_t offset_bytes,
                                                         uint64_t size_bytes);

struct CreatePartitionChoices {
    uint64_t size_bytes{0};
    uint64_t relative_offset_bytes{0};
    QString file_system;
    uint64_t allocation_unit_bytes{0};
    /// "gpt:<type>" or "mbr:<type>", or empty for the default of the disk's style.
    QString partition_type;
    QString label;
    bool full_format{false};
    QString drive_letter;
};
[[nodiscard]] QJsonObject createPartitionPayload(const CreatePartitionChoices& choices);

struct FormatPartitionChoices {
    QString file_system;
    uint64_t allocation_unit_bytes{0};
    QString label;
    bool full_format{false};
};
[[nodiscard]] QJsonObject formatPartitionPayload(const FormatPartitionChoices& choices);

/// A destructive rebuild keeps its files in a backup off the volume, and a person has to say so.
struct BackupRestoreChoices {
    QString backup_directory;
    bool confirmed{false};
};

[[nodiscard]] QJsonObject clusterSizePayload(const PartitionVolumeInfo& volume,
                                             const FormatPartitionChoices& choices,
                                             const BackupRestoreChoices& backup);

[[nodiscard]] QJsonObject resizePartitionPayload(uint64_t target_size_bytes,
                                                 uint64_t adjacent_free_bytes);

/// Adds what moving a partition's start needs to a resize's payload, or starts a payload for it.
void addMovePartitionFields(QJsonObject* payload,
                            const PartitionInfoEx& partition,
                            uint64_t target_offset_bytes,
                            const BackupRestoreChoices& backup);

/// Give a partition the free space before it, by moving its start back over that space.
[[nodiscard]] QJsonObject freeSpaceMovePayload(const PartitionInfoEx& partition,
                                               uint64_t amount_bytes,
                                               const BackupRestoreChoices& backup);
/// Give a partition the free space after it, by growing it in place.
[[nodiscard]] QJsonObject freeSpaceResizePayload(const PartitionInfoEx& partition,
                                                 uint64_t free_bytes,
                                                 uint64_t amount_bytes);

[[nodiscard]] QJsonObject allocateFreeSpacePayload(const PartitionInfoEx& donor,
                                                   uint64_t bytes_to_allocate,
                                                   const BackupRestoreChoices& backup);

[[nodiscard]] QJsonObject primaryLogicalPayload(const PartitionInfoEx& partition,
                                                const QString& target_layout,
                                                const BackupRestoreChoices& backup);
[[nodiscard]] QJsonObject volumeSerialPayload(const PartitionVolumeInfo& volume,
                                              const BackupRestoreChoices& backup);
[[nodiscard]] QJsonObject dynamicToBasicPayload(const PartitionInfoEx& partition,
                                                const BackupRestoreChoices& backup);

[[nodiscard]] QJsonObject convertStylePayload(const PartitionDiskInfo& disk, const QString& scheme);

// Quick Partition.
[[nodiscard]] uint64_t quickPartitionUsableBytes(const PartitionDiskInfo& disk);
[[nodiscard]] QVector<uint64_t> quickPartitionEqualSizes(uint64_t usable_bytes,
                                                         int partition_count);
[[nodiscard]] QVector<uint64_t> quickPartitionSizesFromOptions(const QJsonObject& options,
                                                               uint64_t usable_bytes);
[[nodiscard]] uint64_t quickPartitionTotalBytes(const QVector<uint64_t>& sizes);
[[nodiscard]] bool quickPartitionSizesAreValid(const QVector<uint64_t>& sizes,
                                               uint64_t usable_bytes);
[[nodiscard]] bool diskNeedsInitializeForQuickPartition(const PartitionDiskInfo& disk);
/// Each operation of a full-disk layout, in order. Empty when the layout cannot be represented.
[[nodiscard]] QVector<PartitionOperationRequest> quickPartitionRequests(
    const PartitionDiskInfo& disk, const QJsonObject& options, const QVector<uint64_t>& sizes);

/// What a clone needs, for a whole disk or for one partition.
struct CopyChoices {
    QString source_path;
    QString target_path;
    QString verify_mode;
    uint64_t source_size_bytes{0};
    uint64_t target_size_bytes{0};
    bool target_wipe_confirmed{false};
};
[[nodiscard]] QJsonObject copyPayload(const CopyChoices& choices);

/// A partition copied into free space on a disk rather than onto a device or an image.
struct CopyRegion {
    uint32_t disk_number{0};
    uint64_t offset_bytes{0};
};
[[nodiscard]] QJsonObject partitionCopyPayload(const CopyChoices& choices,
                                               const std::optional<CopyRegion>& region);

[[nodiscard]] QString rawDiskPath(uint32_t disk_number);

/// Where Create Image reads from: the whole disk when the disk is offline or a disk was chosen,
/// and otherwise the volume. Empty when the partition has no stable volume identity.
[[nodiscard]] QString createImageSourcePath(const PartitionTarget& selected,
                                            const PartitionDiskInfo& disk,
                                            bool offline_source);
[[nodiscard]] QJsonObject createImagePayload(const PartitionTarget& selected,
                                             bool offline_source,
                                             const QString& source_path,
                                             const QString& image_path,
                                             bool overwrite_confirmed);
[[nodiscard]] QJsonObject restoreImagePayload(const PartitionTarget& selected,
                                              const QString& image_path,
                                              uint64_t image_size_bytes,
                                              const QString& image_sha256);

struct RecoveredPartitionChoices {
    uint64_t offset_bytes{0};
    uint64_t size_bytes{0};
    QString type_id;
    QString partition_style;
    bool restore_acknowledged{false};
};
[[nodiscard]] QJsonObject recoveryScanPayload(const QString& scan_mode);
[[nodiscard]] QJsonObject restoreRecoveredPartitionPayload(
    const RecoveredPartitionChoices& choices);

/// Migrate OS, with the plan for this source and target carried beside the operation.
[[nodiscard]] QJsonObject migrateOsPayload(const PartitionDiskInfo& source,
                                           const CopyChoices& choices,
                                           bool allow_no_snapshot);

/// Boot repair of the Windows on a disk other than the one this PC runs from.
[[nodiscard]] QJsonObject repairAttachedBootPayload(const QString& boot_mode);
/// Boot repair of the Windows this PC is running.
[[nodiscard]] QJsonObject repairRunningBootPayload(const QString& windows_path,
                                                   const QString& boot_mode);

/// A volume's letter is what BitLocker, TRIM and defrag are told to act on.
[[nodiscard]] QJsonObject driveLetterPayload(const QString& mount_point);
[[nodiscard]] QJsonObject bitLockerUnlockPayload(const QString& mount_point,
                                                 const QString& recovery_password);
[[nodiscard]] QJsonObject ssdSecureErasePayload();

}  // namespace sak

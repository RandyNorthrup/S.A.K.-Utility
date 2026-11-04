// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_operation_requests.cpp
/// @brief What a person's choices in a Partition Manager dialog become.

#include "sak/partition_operation_requests.h"

#include "sak/partition_os_migration.h"
#include "sak/partition_raw_device_io.h"
#include "sak/partition_safety_validator.h"

#include <QJsonArray>

#include <algorithm>
#include <limits>
#include <numeric>

namespace sak {

namespace {

constexpr uint64_t kMegabyte = 1024ULL * 1024ULL;
// The first megabyte of a disk holds its partition table.
constexpr uint64_t kFirstPartitionOffsetBytes = kMegabyte;

const QString kBasicDataGptType = QStringLiteral("{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}");

uint64_t saturatingSum(uint64_t left, uint64_t right) {
    return left > std::numeric_limits<uint64_t>::max() - right
               ? std::numeric_limits<uint64_t>::max()
               : left + right;
}

uint64_t alignDownToMegabyte(uint64_t bytes) {
    return (bytes / kMegabyte) * kMegabyte;
}

uint64_t quickPartitionSliceBytes(uint64_t usable_bytes, int partition_count, int partition_index) {
    const auto count = static_cast<uint64_t>(std::max(1, partition_count));
    const uint64_t base_bytes = alignDownToMegabyte(usable_bytes / count);
    const uint64_t used_before = base_bytes * static_cast<uint64_t>(partition_index);
    return partition_index == partition_count - 1 ? usable_bytes - used_before : base_bytes;
}

QString quickPartitionLabel(const QJsonObject& options, int partition_index) {
    if (options.value(QStringLiteral("size_mode")).toString() == QStringLiteral("custom")) {
        const auto labels = options.value(QStringLiteral("custom_labels")).toArray();
        const QString custom_label = partition_index < labels.size()
                                         ? labels.at(partition_index).toString().trimmed()
                                         : QString();
        if (!custom_label.isEmpty()) {
            return custom_label;
        }
    }
    return QStringLiteral("%1%2")
        .arg(options.value(QStringLiteral("label_prefix")).toString())
        .arg(partition_index + 1);
}

QJsonObject quickPartitionCreatePayload(const QJsonObject& options,
                                        uint64_t size_bytes,
                                        int partition_index) {
    QJsonObject payload;
    payload[QStringLiteral("size_bytes")] = QString::number(size_bytes);
    payload[QStringLiteral("relative_offset_bytes")] = QStringLiteral("0");
    payload[QStringLiteral("file_system")] =
        options.value(QStringLiteral("file_system")).toString();
    payload[QStringLiteral("allocation_unit_bytes")] =
        options.value(QStringLiteral("allocation_unit_bytes")).toString();
    payload[QStringLiteral("label")] = quickPartitionLabel(options, partition_index);
    payload[QStringLiteral("full_format")] =
        options.value(QStringLiteral("full_format")).toBool(false);
    const QString scheme = options.value(QStringLiteral("partition_scheme")).toString();
    if (scheme == QStringLiteral("GPT")) {
        payload[QStringLiteral("gpt_type")] = kBasicDataGptType;
    } else if (scheme == QStringLiteral("MBR")) {
        const bool fat32 = payload.value(QStringLiteral("file_system"))
                               .toString()
                               .compare(QStringLiteral("FAT32"), Qt::CaseInsensitive) == 0;
        payload[QStringLiteral("mbr_type")] = fat32 ? QStringLiteral("FAT32")
                                                    : QStringLiteral("IFS");
    }
    return payload;
}

void addVolumeFields(QJsonObject* payload, const PartitionInfoEx& partition) {
    if (!partition.volume) {
        return;
    }
    (*payload)[QStringLiteral("drive_letter")] = partition.volume->drive_letter.left(1).toUpper();
    (*payload)[QStringLiteral("file_system")] = partition.volume->file_system;
    (*payload)[QStringLiteral("label")] = partition.volume->label;
}

void addBackupFields(QJsonObject* payload, const BackupRestoreChoices& backup) {
    (*payload)[QStringLiteral("backup_directory")] = backup.backup_directory;
    (*payload)[QStringLiteral("target_wipe_confirmed")] = backup.confirmed;
}

}  // namespace

PartitionTarget diskOperationTarget(const PartitionDiskInfo& disk) {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = disk.disk_number;
    target.size_bytes = disk.size_bytes;
    return target;
}

PartitionTarget partitionSelectionTarget(const PartitionInfoEx& partition) {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = partition.disk_number;
    target.partition_number = partition.partition_number;
    target.offset_bytes = partition.offset_bytes;
    target.size_bytes = partition.size_bytes;
    if (partition.volume) {
        target.drive_letter = partition.volume->drive_letter;
    }
    return target;
}

PartitionTarget partitionIdentityTarget(const PartitionInfoEx& partition) {
    PartitionTarget target = partitionSelectionTarget(partition);
    target.partition_guid = partition.partition_guid;
    if (partition.volume) {
        target.volume_guid = partition.volume->volume_guid;
    }
    return target;
}

PartitionTarget unallocatedOperationTarget(DiskNumber disk_number,
                                           uint64_t offset_bytes,
                                           uint64_t size_bytes) {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Unallocated;
    target.disk_number = disk_number;
    target.offset_bytes = offset_bytes;
    target.size_bytes = size_bytes;
    return target;
}

QJsonObject createPartitionPayload(const CreatePartitionChoices& choices) {
    QJsonObject payload;
    payload[QStringLiteral("size_bytes")] = QString::number(choices.size_bytes);
    payload[QStringLiteral("relative_offset_bytes")] =
        QString::number(choices.relative_offset_bytes);
    payload[QStringLiteral("file_system")] = choices.file_system;
    payload[QStringLiteral("allocation_unit_bytes")] =
        QString::number(choices.allocation_unit_bytes);
    if (choices.partition_type.startsWith(QStringLiteral("gpt:"))) {
        payload[QStringLiteral("gpt_type")] =
            choices.partition_type.mid(QStringLiteral("gpt:").size());
    } else if (choices.partition_type.startsWith(QStringLiteral("mbr:"))) {
        payload[QStringLiteral("mbr_type")] =
            choices.partition_type.mid(QStringLiteral("mbr:").size());
    }
    payload[QStringLiteral("label")] = choices.label;
    payload[QStringLiteral("full_format")] = choices.full_format;
    if (!choices.drive_letter.isEmpty()) {
        payload[QStringLiteral("drive_letter")] = choices.drive_letter;
    }
    return payload;
}

QJsonObject formatPartitionPayload(const FormatPartitionChoices& choices) {
    QJsonObject payload;
    payload[QStringLiteral("file_system")] = choices.file_system;
    payload[QStringLiteral("allocation_unit_bytes")] =
        QString::number(choices.allocation_unit_bytes);
    payload[QStringLiteral("label")] = choices.label;
    payload[QStringLiteral("full_format")] = choices.full_format;
    return payload;
}

QJsonObject clusterSizePayload(const PartitionVolumeInfo& volume,
                               const FormatPartitionChoices& choices,
                               const BackupRestoreChoices& backup) {
    QJsonObject payload;
    payload[QStringLiteral("drive_letter")] = volume.drive_letter.left(1).toUpper();
    payload[QStringLiteral("file_system")] = choices.file_system;
    payload[QStringLiteral("allocation_unit_bytes")] =
        QString::number(choices.allocation_unit_bytes);
    payload[QStringLiteral("label")] = choices.label;
    addBackupFields(&payload, backup);
    return payload;
}

QJsonObject resizePartitionPayload(uint64_t target_size_bytes, uint64_t adjacent_free_bytes) {
    QJsonObject payload;
    payload[QStringLiteral("target_size_bytes")] = QString::number(target_size_bytes);
    payload[QStringLiteral("adjacent_free_bytes")] = QString::number(adjacent_free_bytes);
    return payload;
}

void addMovePartitionFields(QJsonObject* payload,
                            const PartitionInfoEx& partition,
                            uint64_t target_offset_bytes,
                            const BackupRestoreChoices& backup) {
    (*payload)[QStringLiteral("target_offset_bytes")] = QString::number(target_offset_bytes);
    addVolumeFields(payload, partition);
    addBackupFields(payload, backup);
}

QJsonObject freeSpaceMovePayload(const PartitionInfoEx& partition,
                                 uint64_t amount_bytes,
                                 const BackupRestoreChoices& backup) {
    QJsonObject payload;
    payload[QStringLiteral("target_size_bytes")] =
        QString::number(saturatingSum(partition.size_bytes, amount_bytes));
    addMovePartitionFields(&payload, partition, partition.offset_bytes - amount_bytes, backup);
    return payload;
}

QJsonObject freeSpaceResizePayload(const PartitionInfoEx& partition,
                                   uint64_t free_bytes,
                                   uint64_t amount_bytes) {
    return resizePartitionPayload(saturatingSum(partition.size_bytes, amount_bytes), free_bytes);
}

QJsonObject allocateFreeSpacePayload(const PartitionInfoEx& donor,
                                     uint64_t bytes_to_allocate,
                                     const BackupRestoreChoices& backup) {
    QJsonObject payload;
    payload[QStringLiteral("source_partition_number")] =
        QString::number(donor.partition_number.value());
    payload[QStringLiteral("source_size_bytes")] = QString::number(donor.size_bytes);
    payload[QStringLiteral("bytes_to_allocate")] = QString::number(bytes_to_allocate);
    if (donor.volume) {
        payload[QStringLiteral("source_drive_letter")] =
            donor.volume->drive_letter.left(1).toUpper();
        payload[QStringLiteral("source_file_system")] = donor.volume->file_system;
        payload[QStringLiteral("source_label")] = donor.volume->label;
    }
    addBackupFields(&payload, backup);
    return payload;
}

QJsonObject primaryLogicalPayload(const PartitionInfoEx& partition,
                                  const QString& target_layout,
                                  const BackupRestoreChoices& backup) {
    QJsonObject payload;
    payload[QStringLiteral("target_layout")] = target_layout;
    payload[QStringLiteral("source_size_bytes")] = QString::number(partition.size_bytes);
    addVolumeFields(&payload, partition);
    addBackupFields(&payload, backup);
    return payload;
}

QJsonObject volumeSerialPayload(const PartitionVolumeInfo& volume,
                                const BackupRestoreChoices& backup) {
    QJsonObject payload;
    payload[QStringLiteral("drive_letter")] = volume.drive_letter.left(1).toUpper();
    payload[QStringLiteral("file_system")] = volume.file_system;
    payload[QStringLiteral("label")] = volume.label;
    addBackupFields(&payload, backup);
    return payload;
}

QJsonObject dynamicToBasicPayload(const PartitionInfoEx& partition,
                                  const BackupRestoreChoices& backup) {
    QJsonObject payload;
    payload[QStringLiteral("source_size_bytes")] = QString::number(partition.size_bytes);
    addVolumeFields(&payload, partition);
    addBackupFields(&payload, backup);
    return payload;
}

QJsonObject convertStylePayload(const PartitionDiskInfo& disk, const QString& scheme) {
    QJsonObject payload;
    payload[QStringLiteral("target_style")] = scheme;
    if (disk.is_system &&
        disk.partition_style.compare(QStringLiteral("MBR"), Qt::CaseInsensitive) == 0 &&
        scheme == QStringLiteral("GPT")) {
        payload[QStringLiteral("mode")] = QStringLiteral("mbr2gpt");
    }
    return payload;
}

uint64_t quickPartitionUsableBytes(const PartitionDiskInfo& disk) {
    const uint64_t reserved_bytes = static_cast<uint64_t>(kQuickPartitionReservedTailMb + 1) *
                                    kMegabyte;
    return disk.size_bytes > reserved_bytes ? alignDownToMegabyte(disk.size_bytes - reserved_bytes)
                                            : 0;
}

QVector<uint64_t> quickPartitionEqualSizes(uint64_t usable_bytes, int partition_count) {
    QVector<uint64_t> sizes;
    sizes.reserve(partition_count);
    for (int index = 0; index < partition_count; ++index) {
        sizes.append(quickPartitionSliceBytes(usable_bytes, partition_count, index));
    }
    return sizes;
}

QVector<uint64_t> quickPartitionSizesFromOptions(const QJsonObject& options,
                                                 uint64_t usable_bytes) {
    const int count = options.value(QStringLiteral("partition_count")).toInt();
    if (options.value(QStringLiteral("size_mode")).toString() != QStringLiteral("custom")) {
        return quickPartitionEqualSizes(usable_bytes, count);
    }

    // Custom mode was requested: fail CLOSED on any malformed custom array rather than silently
    // substituting an equal-size layout the user never asked for. An empty result makes
    // quickPartitionSizesAreValid reject the request so nothing destructive is enqueued.
    const auto array = options.value(QStringLiteral("custom_size_bytes")).toArray();
    if (array.size() != count) {
        return {};
    }
    QVector<uint64_t> sizes;
    sizes.reserve(count);
    for (const auto& value : array) {
        bool ok = false;
        const auto size_bytes = value.toString().toULongLong(&ok);
        if (!ok || size_bytes == 0) {
            return {};
        }
        sizes.append(size_bytes);
    }
    return sizes;
}

uint64_t quickPartitionTotalBytes(const QVector<uint64_t>& sizes) {
    // Saturate instead of wrapping: adversarial custom sizes near UINT64_MAX must not overflow
    // into a small total that then slips past the "total > usable_bytes" validity check.
    return std::accumulate(sizes.cbegin(), sizes.cend(), uint64_t{0}, saturatingSum);
}

bool quickPartitionSizesAreValid(const QVector<uint64_t>& sizes, uint64_t usable_bytes) {
    const uint64_t minimum_bytes = static_cast<uint64_t>(kQuickPartitionMinimumSizeMb) * kMegabyte;
    if (sizes.isEmpty() || quickPartitionTotalBytes(sizes) > usable_bytes) {
        return false;
    }
    return std::ranges::all_of(sizes,
                               [minimum_bytes](uint64_t size) { return size >= minimum_bytes; });
}

bool diskNeedsInitializeForQuickPartition(const PartitionDiskInfo& disk) {
    const QString style = disk.partition_style.toUpper();
    return style != QStringLiteral("GPT") && style != QStringLiteral("MBR");
}

QVector<PartitionOperationRequest> quickPartitionRequests(const PartitionDiskInfo& disk,
                                                          const QJsonObject& options,
                                                          const QVector<uint64_t>& sizes) {
    QVector<PartitionOperationRequest> requests;
    const PartitionTarget disk_target = diskOperationTarget(disk);
    if (diskNeedsInitializeForQuickPartition(disk)) {
        QJsonObject style;
        style[QStringLiteral("target_style")] = options.value(QStringLiteral("partition_scheme"));
        requests.append({.type = PartitionOperationType::InitializeDisk,
                         .target = disk_target,
                         .payload = style});
    }
    if (!disk.partitions.isEmpty()) {
        requests.append({.type = PartitionOperationType::DeleteAllPartitions,
                         .target = disk_target,
                         .payload = {}});
    }
    uint64_t offset_bytes = kFirstPartitionOffsetBytes;
    for (int index = 0; index < sizes.size(); ++index) {
        const uint64_t size_bytes = sizes.at(index);
        requests.append(
            {.type = PartitionOperationType::Create,
             .target = unallocatedOperationTarget(disk.disk_number, offset_bytes, size_bytes),
             .payload = quickPartitionCreatePayload(options, size_bytes, index)});
        if (size_bytes > std::numeric_limits<uint64_t>::max() - offset_bytes) {
            return {};
        }
        offset_bytes += size_bytes;
    }
    return requests;
}

QJsonObject copyPayload(const CopyChoices& choices) {
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = choices.source_path;
    payload[QStringLiteral("target_path")] = choices.target_path;
    payload[QStringLiteral("verify_mode")] = choices.verify_mode;
    payload[QStringLiteral("source_size_bytes")] = QString::number(choices.source_size_bytes);
    payload[QStringLiteral("target_size_bytes")] = QString::number(choices.target_size_bytes);
    payload[QStringLiteral("target_wipe_confirmed")] = choices.target_wipe_confirmed;
    return payload;
}

QJsonObject partitionCopyPayload(const CopyChoices& choices,
                                 const std::optional<CopyRegion>& region) {
    QJsonObject payload = copyPayload(choices);
    if (region) {
        payload[QStringLiteral("target_disk_number")] = static_cast<int>(region->disk_number);
        payload[QStringLiteral("target_offset_bytes")] = QString::number(region->offset_bytes);
    }
    return payload;
}

QString rawDiskPath(uint32_t disk_number) {
    return QStringLiteral("\\\\.\\PhysicalDrive%1").arg(disk_number);
}

QString createImageSourcePath(const PartitionTarget& selected,
                              const PartitionDiskInfo& disk,
                              bool offline_source) {
    if (selected.kind == PartitionTargetKind::Disk || offline_source) {
        return rawDiskPath(selected.disk_number.value());
    }
    const auto* partition = PartitionSafetyValidator::findPartition(disk,
                                                                    selected.partition_number);
    if (partition == nullptr) {
        return {};
    }
    const auto target = partitionIdentityTarget(*partition);
    if (!target.drive_letter.isEmpty()) {
        return QStringLiteral("\\\\.\\%1:").arg(target.drive_letter.left(1).toUpper());
    }
    QString volume_path = target.volume_guid;
    if (volume_path.endsWith(QLatin1Char('\\')) || volume_path.endsWith(QLatin1Char('/'))) {
        volume_path.chop(1);
    }
    return canonicalRawDevicePath(volume_path);
}

QJsonObject createImagePayload(const PartitionTarget& selected,
                               bool offline_source,
                               const QString& source_path,
                               const QString& image_path,
                               bool overwrite_confirmed) {
    const uint64_t source_offset = offline_source && selected.kind == PartitionTargetKind::Partition
                                       ? selected.offset_bytes
                                       : 0;
    return {{QStringLiteral("source_path"), source_path},
            {QStringLiteral("target_path"), image_path},
            {QStringLiteral("overwrite_confirmed"), overwrite_confirmed},
            {QStringLiteral("source_size_bytes"), QString::number(selected.size_bytes)},
            {QStringLiteral("verify_mode"), QStringLiteral("Full")},
            {QStringLiteral("source_capture_mode"),
             offline_source ? QStringLiteral("OfflineDisk") : QStringLiteral("LockedVolumes")},
            {QStringLiteral("source_offset_bytes"), QString::number(source_offset)}};
}

QJsonObject restoreImagePayload(const PartitionTarget& selected,
                                const QString& image_path,
                                uint64_t image_size_bytes,
                                const QString& image_sha256) {
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = image_path;
    payload[QStringLiteral("target_path")] = rawDiskPath(selected.disk_number.value());
    payload[QStringLiteral("source_size_bytes")] = QString::number(image_size_bytes);
    payload[QStringLiteral("target_size_bytes")] = QString::number(selected.size_bytes);
    payload[QStringLiteral("source_sha256")] = image_sha256;
    payload[QStringLiteral("target_wipe_confirmed")] = true;
    return payload;
}

QJsonObject recoveryScanPayload(const QString& scan_mode) {
    QJsonObject payload;
    payload[QStringLiteral("scan_mode")] = scan_mode;
    return payload;
}

QJsonObject restoreRecoveredPartitionPayload(const RecoveredPartitionChoices& choices) {
    QJsonObject payload;
    payload[QStringLiteral("offset_bytes")] = QString::number(choices.offset_bytes);
    payload[QStringLiteral("size_bytes")] = QString::number(choices.size_bytes);
    payload[QStringLiteral("type_id")] = choices.type_id;
    payload[QStringLiteral("partition_style")] = choices.partition_style;
    payload[QStringLiteral("restore_acknowledged")] = choices.restore_acknowledged;
    return payload;
}

QJsonObject migrateOsPayload(const PartitionDiskInfo& source,
                             const CopyChoices& choices,
                             bool allow_no_snapshot) {
    QJsonObject payload = copyPayload(choices);
    payload[QStringLiteral("allow_no_snapshot")] = allow_no_snapshot;
    payload[QStringLiteral("migration_plan")] =
        osMigrationPlanToJson(planOsMigration(source, choices.target_size_bytes));
    return payload;
}

QJsonObject repairAttachedBootPayload(const QString& boot_mode) {
    QJsonObject payload;
    payload[QStringLiteral("repair_scope")] = QStringLiteral("attached");
    payload[QStringLiteral("boot_mode")] = boot_mode;
    return payload;
}

QJsonObject repairRunningBootPayload(const QString& windows_path, const QString& boot_mode) {
    QJsonObject payload;
    payload[QStringLiteral("windows_path")] = windows_path;
    payload[QStringLiteral("esp_letter")] = QStringLiteral("S");
    payload[QStringLiteral("boot_mode")] = boot_mode;
    return payload;
}

QJsonObject driveLetterPayload(const QString& mount_point) {
    QJsonObject payload;
    payload[QStringLiteral("drive_letter")] = mount_point.left(1);
    return payload;
}

QJsonObject bitLockerUnlockPayload(const QString& mount_point, const QString& recovery_password) {
    QJsonObject payload = driveLetterPayload(mount_point);
    payload[QStringLiteral("recovery_password")] = recovery_password.trimmed();
    return payload;
}

QJsonObject ssdSecureErasePayload() {
    QJsonObject payload;
    payload[QStringLiteral("ssd_secure_erase")] = true;
    payload[QStringLiteral("target_wipe_confirmed")] = true;
    return payload;
}

}  // namespace sak

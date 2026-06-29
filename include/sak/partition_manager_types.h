// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_manager_types.h
/// @brief Shared types for the Partition Manager panel and backend.

#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <optional>

namespace sak {

inline constexpr int kPartitionDefaultTaskTimeoutSeconds = 120;
inline constexpr int kPartitionMediumTaskTimeoutSeconds = 600;
inline constexpr int kPartitionFormatTaskTimeoutSeconds = 900;
inline constexpr int kPartitionConversionTaskTimeoutSeconds = 1800;
inline constexpr int kPartitionLongTaskTimeoutSeconds = 3600;
inline constexpr int kPartitionReportOutputPreviewChars = 4000;
inline constexpr int kPartitionLayoutHashPreviewChars = 10;
inline constexpr int kPartitionCenterTableStretch = 3;
inline constexpr int kPartitionByteDisplayPrecision = 2;

// Shared ceiling for S.A.K.-generated APFS containers (format + in-place file
// mutation). The certified writer formats and commits in place across the
// single-CIB, multi-CIB, metadata-overflow, and CAB tiers up to this size
// (Apple fsck_apfs + kernel RW mount, milestones A1/A2). Both the Partition
// Manager format/mutation gate and the File Management write-capability gate
// derive from this one value so they cannot drift apart.
inline constexpr uint64_t kMaximumApfsGeneratedContainerBytes = 24ULL * 1024ULL * 1024ULL *
                                                                1024ULL * 1024ULL;

enum class PartitionOperationType {
    Create,
    Delete,
    Format,
    SetDriveLetter,
    SetPartitionLabel,
    CheckFileSystem,
    SurfaceTest,
    PartitionRecoveryScan,
    RestoreRecoveredPartition,
    SetPartitionHidden,
    SetPartitionActive,
    SetPartitionTypeId,
    InitializeDisk,
    DeleteAllPartitions,
    Resize,
    AllocateFreeSpace,
    ConvertPartitionStyle,
    Merge,
    Split,
    ConvertFileSystem,
    ChangeClusterSize,
    CloneDisk,
    ClonePartition,
    CreateImage,
    RestoreImage,
    MigrateOs,
    RepairBoot,
    OptimizeSsd,
    DefragVolume,
    BitLockerUnlock,
    BitLockerSuspend,
    BitLockerResume,
    WipePartition,
    WipeDisk,
    WipeFreeSpace,
    MovePartition,
    ConvertPrimaryLogical,
    ChangeVolumeSerialNumber,
    ConvertDynamicDiskToBasic,
    ApfsWriteRootFile,
    ApfsPatchRootFile,
    ApfsPatchRootDirectoryFile,
    ApfsDeleteRootFile,
    ApfsWriteRootDirectoryFile,
    ApfsDeleteRootDirectoryFile,
    ApfsCreateRootDirectory,
    ApfsDeleteRootDirectory,
    ApfsChangeVolumeLabel,
    HfsOverwriteFile,
    HfsReplaceFile,
    HfsGrowFile,
    HfsTruncateFile,
    HfsReplaceResourceFork,
    HfsGrowResourceFork,
    HfsTruncateResourceFork,
    HfsCreateEmptyFile,
    HfsCreateFile,
    HfsDeleteEmptyFile,
    HfsDeleteFile,
    HfsCreateEmptyFolder,
    HfsDeleteEmptyFolder,
    HfsDeleteFolderTree,
    HfsRenameMoveCatalogEntry,
    HfsReplaceInlineAttribute,
    HfsReplaceForkAttribute,
    HfsGrowForkAttribute,
};

enum class OperationRisk {
    ReadOnly,
    Low,
    Destructive,
    SystemCritical,
};

enum class PartitionTargetKind {
    Disk,
    Partition,
    Volume,
    Unallocated,
};

enum class PartitionManagerState {
    Idle,
    RefreshingInventory,
    Ready,
    PlanningOperation,
    QueueDirty,
    PreflightRunning,
    AwaitingElevation,
    Applying,
    Verifying,
    Failed,
    Cancelled,
};

struct PartitionVolumeInfo {
    QString volume_guid;
    QString drive_letter;
    QString label;
    QString file_system;
    QString file_system_source;
    QStringList file_system_details;
    QString health_status;
    uint64_t total_bytes{0};
    uint64_t free_bytes{0};
    bool bitlocker_enabled{false};
    bool bitlocker_locked{false};
    bool dirty_bit_set{false};

    [[nodiscard]] bool hasDriveLetter() const noexcept { return !drive_letter.isEmpty(); }
};

struct PartitionInfoEx {
    uint32_t disk_number{0};
    uint32_t partition_number{0};
    QString partition_guid;
    QString type_name;
    QString gpt_type;
    uint64_t offset_bytes{0};
    uint64_t size_bytes{0};
    bool is_system{false};
    bool is_boot{false};
    bool is_efi{false};
    bool is_recovery{false};
    bool is_msr{false};
    bool is_active{false};
    bool is_read_only{false};
    std::optional<PartitionVolumeInfo> volume;

    [[nodiscard]] bool hasVolume() const noexcept { return volume.has_value(); }
};

struct UnallocatedRegion {
    uint32_t disk_number{0};
    uint64_t offset_bytes{0};
    uint64_t size_bytes{0};
};

struct PartitionDiskInfo {
    uint32_t disk_number{0};
    QString device_path;
    QString model;
    QString serial_number;
    QString bus_type;
    QString media_type;
    QString partition_style;
    QString health_status;
    QString operational_status;
    QString smart_summary;
    int temperature_celsius{-1};
    uint64_t power_on_hours{0};
    uint64_t read_errors_total{0};
    uint64_t write_errors_total{0};
    uint64_t wear_percent{0};
    uint64_t size_bytes{0};
    bool is_system{false};
    bool is_boot{false};
    bool is_removable{false};
    bool is_read_only{false};
    bool is_dynamic{false};
    bool is_storage_spaces{false};
    QVector<PartitionInfoEx> partitions;
    QVector<UnallocatedRegion> unallocated_regions;
};

struct PartitionInventory {
    QVector<PartitionDiskInfo> disks;
    QDateTime captured_at;
    QString layout_hash;
    QStringList warnings;

    [[nodiscard]] bool isEmpty() const noexcept { return disks.isEmpty(); }
};

struct PartitionTarget {
    PartitionTargetKind kind{PartitionTargetKind::Disk};
    uint32_t disk_number{0};
    uint32_t partition_number{0};
    QString partition_guid;
    QString volume_guid;
    QString drive_letter;
    uint64_t offset_bytes{0};
    uint64_t size_bytes{0};
};

struct PartitionOperation {
    QString id;
    PartitionOperationType type{PartitionOperationType::Create};
    OperationRisk risk{OperationRisk::Low};
    PartitionTarget target;
    QJsonObject payload;
    QString summary;
    QStringList warnings;
    QStringList blockers;

    [[nodiscard]] bool isBlocked() const noexcept { return !blockers.isEmpty(); }
};

struct OperationPreview {
    QVector<PartitionOperation> operations;
    QString before_layout_hash;
    QString after_layout_description;
    QStringList blockers;
    QStringList warnings;

    [[nodiscard]] bool canApply() const noexcept { return blockers.isEmpty(); }
};

struct PartitionExecutionStep {
    QString operation_id;
    QString summary;
    bool success{false};
    bool skipped{false};
    QString stdout_text;
    QString stderr_text;
    QString error_message;
};

struct PartitionExecutionResult {
    QString batch_id;
    bool success{false};
    bool dry_run{false};
    bool cancelled{false};
    bool timed_out{false};
    QString message;
    QString report_html;
    QString report_json;
    QVector<PartitionExecutionStep> steps;
};

[[nodiscard]] QString toDisplayString(PartitionOperationType type);
[[nodiscard]] QString toDisplayString(OperationRisk risk);
[[nodiscard]] QString toDisplayString(PartitionManagerState state);
[[nodiscard]] QString makePartitionOperationId();
[[nodiscard]] QString formatPartitionBytes(uint64_t bytes);

}  // namespace sak

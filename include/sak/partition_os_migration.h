// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_os_migration.h
/// @brief What Migrate OS will do to a target of a given size, worked out from the inventory.
///
/// The plan is pure arithmetic over a PartitionDiskInfo, so the wizard, the safety validator and
/// the script builder all read one answer, and a test can reach every branch with no disk. The
/// script that carries it out is in src/core/partition_scripts.

#pragma once

#include "sak/partition_manager_types.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <optional>

namespace sak {

/// Partitions start on this boundary on the target, as Windows places them.
inline constexpr uint64_t kOsMigrationAlignmentBytes = 1024ULL * 1024ULL;
/// Left free past the last partition: GPT keeps a second copy of its table at the end of the disk,
/// 33 sectors plus the backup header, and a sector can be 4 KiB.
inline constexpr uint64_t kOsMigrationDiskTailBytes = 34ULL * 4096ULL;
/// How many partitions an MBR table holds without extended ones.
inline constexpr qsizetype kOsMigrationMbrPrimaryLimit = 4;
/// The most a 512-byte-sector MBR disk can address.
inline constexpr uint64_t kOsMigrationMbrLimitBytes = 2048ULL * 1024ULL * 1024ULL * 1024ULL;
/// Room Windows must have past its used data on a smaller target, whichever is larger.
inline constexpr uint64_t kOsMigrationMinimumHeadroomBytes = 2048ULL * 1024ULL * 1024ULL;
inline constexpr uint64_t kOsMigrationHeadroomDivisor = 20;
/// A smaller target holds a temporary compressed image too: 3/4 of the used data, plus this. Real
/// data can compress less, and then the capture stops and says so; nothing but the target changed.
inline constexpr uint64_t kOsMigrationImageDivisor = 4;
inline constexpr uint64_t kOsMigrationImageShare = 3;
inline constexpr uint64_t kOsMigrationImageSlackBytes = 1024ULL * 1024ULL * 1024ULL;
/// What a smaller target has spare goes half to Windows' new volume and half to the image.
inline constexpr uint64_t kOsMigrationSpareShareDivisor = 2;
/// Lost to rounding where the first partition and Windows start.
inline constexpr uint64_t kOsMigrationPlacementSlackBytes = 2ULL * kOsMigrationAlignmentBytes;

enum class OsMigrationPartitionKind {
    System,    ///< EFI System Partition, or the active partition of an MBR disk
    Reserved,  ///< Microsoft Reserved: no file system
    Windows,   ///< the partition Windows runs from
    Recovery,
    Data,      ///< another volume: read from its own snapshot
    Other,
};

/// How Windows' own volume reaches the target.
enum class OsMigrationRoute {
    /// The target's partition is at least as large: the volume's used blocks are copied from its
    /// snapshot, and the file system is then extended into any extra room.
    Blocks,
    /// The target's partition is smaller: blocks cannot be moved into it, so a new NTFS volume
    /// receives an image Windows' own DISM captures from the snapshot.
    Image,
};

/// The word for a kind, in the plan's payload and in the script's parameters.
[[nodiscard]] QString osMigrationKindName(OsMigrationPartitionKind kind);

struct OsMigrationPartition {
    uint32_t source_number{0};
    uint64_t source_offset{0};
    uint64_t source_size{0};
    uint64_t target_offset{0};
    /// The partition as first written to the target.
    uint64_t initial_size{0};
    /// The partition once the migration is done. Differs from initial_size for Windows only.
    uint64_t final_size{0};
    OsMigrationPartitionKind kind{OsMigrationPartitionKind::Other};
    QString drive_letter;
    QString volume_path;
    QString file_system;
    /// Read through a VSS snapshot rather than raw.
    bool from_snapshot{false};
};

struct OsMigrationPlan {
    QString partition_scheme;
    OsMigrationRoute route{OsMigrationRoute::Blocks};
    QVector<OsMigrationPartition> partitions;
    uint64_t source_bytes{0};
    uint64_t target_bytes{0};
    uint64_t windows_used_bytes{0};
    /// Smallest whole target this source fits on, by the route a target that size would take.
    uint64_t minimum_target_bytes{0};
    /// Image route: where the temporary partition holding the image goes, and how large it is.
    uint64_t scratch_offset{0};
    uint64_t scratch_size{0};
    /// Why this cannot run. Empty means it can.
    QStringList blockers;

    [[nodiscard]] bool canRun() const noexcept { return blockers.isEmpty(); }
    /// The partition Windows runs from, or nullptr when the source has none.
    [[nodiscard]] const OsMigrationPartition* windowsPartition() const;
};

/// Everything Migrate OS can know before it starts, from the inventory alone. A blocker here stops
/// the operation at the queue; the script checks the same facts again on the live disks.
[[nodiscard]] OsMigrationPlan planOsMigration(const PartitionDiskInfo& source,
                                              uint64_t target_bytes);

/// The form a plan takes in an operation's payload.
[[nodiscard]] QJsonObject osMigrationPlanToJson(const OsMigrationPlan& plan);
[[nodiscard]] std::optional<OsMigrationPlan> osMigrationPlanFromJson(const QJsonObject& json);

/// True when both plans put the same partitions in the same places by the same route. How much of
/// Windows' volume is in use moves all the time and is not part of this.
[[nodiscard]] bool osMigrationPlansAgree(const OsMigrationPlan& left, const OsMigrationPlan& right);

/// The PowerShell that carries out @p operation's plan on the live disks, or a reason it cannot be
/// built, in @p blocker.
[[nodiscard]] QString buildOsMigrationScriptText(const PartitionOperation& operation,
                                                 const OsMigrationPlan& plan,
                                                 QString* blocker);

/// What a Dry Run shows: the plan in words, one line per partition.
[[nodiscard]] QString osMigrationPlanSummary(const OsMigrationPlan& plan);

/// PowerShell functions shared with Boot Repair: temporary letters on a disk that is not the one
/// Windows runs from, and rebuilding that disk's boot files.
[[nodiscard]] QString targetBootFilesFunctionScript();

/// The PowerShell that finds the Windows on @p disk_number, which must not be a disk Windows is
/// running from, and rebuilds its boot files on that disk. The caller pins the disk's identity.
[[nodiscard]] QString buildAttachedBootRepairScriptText(uint32_t disk_number,
                                                        const QString& boot_mode);

}  // namespace sak

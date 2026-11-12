// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_safety_validator.h
/// @brief Safety rules for Partition Manager operations.

#pragma once

#include "sak/partition_manager_types.h"

#include <optional>

namespace sak {

struct PartitionValidationResult {
    QStringList blockers;
    QStringList warnings;

    [[nodiscard]] bool allowed() const noexcept { return blockers.isEmpty(); }
};

struct PartitionPathVolumeIdentity {
    QString volume_guid;
    QString error;

    [[nodiscard]] bool valid() const noexcept { return !volume_guid.isEmpty(); }
};

/// Resolve an existing Windows path to its stable volume GUID. No drive-text inference.
[[nodiscard]] PartitionPathVolumeIdentity resolvePartitionPathVolumeIdentity(const QString& path);

/// @brief Read an unsigned 64-bit field out of a partition-inventory JSON object.
/// @return 0 when the key is absent, holds a non-numeric string, or holds a NEGATIVE number.
/// @note The negative clamp is the point. A negative double cast straight to uint64_t wraps to
///       roughly 1.8e19, and this function reads partition Offset and Size and volume Size and
///       SizeRemaining -- so a single malformed field would not produce a small wrong number, it
///       would produce an exabyte-scale one that then feeds the offset arithmetic and the
///       free-space checks. Two copies of this existed and only the GUI's clamped; the core
///       inventory parser, which is the one that actually reads a disk's geometry, did not.
[[nodiscard]] uint64_t jsonUInt64(const QJsonObject& object, const QString& key);

/// @brief @p left + @p right, clamped to the maximum instead of wrapping.
/// @note A partition's end offset is offset + size, and both come from the inventory. Wrapping
///       there would produce a SMALL end offset for a huge partition, which is how an adjacency
///       test starts matching the wrong region on a disk this program writes to.
[[nodiscard]] uint64_t saturatingAdd(uint64_t left, uint64_t right);

/// @brief Bytes in use on @p partition's volume, or 0 when that cannot be established.
/// @note Returns 0 rather than a negative-turned-huge value when free exceeds total, which the
///       inventory can legitimately report mid-refresh. The shrink guard compares against this,
///       and a wrapped value there would permit a shrink below the data actually stored.
[[nodiscard]] uint64_t usedBytes(const PartitionInfoEx& partition);

/// @brief Size of the unallocated region that begins exactly where @p partition ends, or 0.
/// @note Matched EXACTLY, never "the nearest region after": growing a partition into a region
///       that does not physically abut it would write over whatever lies between.
[[nodiscard]] uint64_t adjacentFreeBytesAfter(const PartitionDiskInfo& disk,
                                              const PartitionInfoEx& partition);

/// @brief Whether @p disk reports itself as solid state (SSD or NVMe).
/// @note This is the SAME judgement the validator uses to BLOCK "HDD defrag is blocked on
///       SSD/NVMe media", so the panel must not decide it separately. It did: the panel's own
///       copy searched only media_type and bus_type, omitting the model string, so a drive
///       identified as solid state only by its model ("Samsung SSD 990 PRO" with an unhelpful
///       media_type) read as neither SSD nor HDD in the panel while the validator read it as an
///       SSD. The panel would then offer a defrag that the validator refuses -- fail-closed, so
///       no SSD was ever defragged, but the operator was offered an operation that could not run.
/// @note All three fields are matched because Windows reports media type inconsistently across
///       drivers; the model string is often the only place "SSD" or "NVMe" appears.
[[nodiscard]] bool diskLooksSsd(const PartitionDiskInfo& disk);

/// @brief Whether @p disk reports itself as rotational.
/// @note The validator requires this to be true before allowing a defrag, so the panel shares it
///       for the same reason as diskLooksSsd. Not simply the negation: a drive that reports
///       neither is UNKNOWN, and a defrag is refused on an unknown drive rather than attempted.
/// @note A drive Windows reports as "Unspecified" also counts when the drive itself reports a
///       nonzero rotation rate: a USB bridge often hides a hard disk from Windows. Windows
///       reporting anything else is final, and a rate never overrules it.
[[nodiscard]] bool diskLooksHdd(const PartitionDiskInfo& disk);

/// @brief Whether Windows reports no media type for @p disk.
[[nodiscard]] bool diskMediaIsUnspecified(const PartitionDiskInfo& disk);

/// @brief Whether this scan could not say if @p disk is a hard disk, and Apply's scan will.
/// @note Windows reports it as "Unspecified" and the scan had no administrator rights, so the drive
///       was not asked. A defrag may queue, and the validator decides again on Apply's own scan.
[[nodiscard]] bool diskMediaUnknownUntilApply(const PartitionDiskInfo& disk);

class PartitionSafetyValidator {
public:
    [[nodiscard]] static PartitionValidationResult validate(const PartitionInventory& inventory,
                                                            const PartitionOperation& operation);

    [[nodiscard]] static const PartitionDiskInfo* findDisk(const PartitionInventory& inventory,
                                                           DiskNumber disk_number);
    [[nodiscard]] static const PartitionInfoEx* findPartition(const PartitionDiskInfo& disk,
                                                              PartitionNumber partition_number);
    [[nodiscard]] static bool isSystemProtectedPartition(const PartitionInfoEx& partition);
    /// An MBR partition type as a technician writes it: one byte in hex ("83", "0x83" and "83h"
    /// are all 0x83, as diskpart's SET ID reads it). Empty when the text is not one byte, or names
    /// a type that would break the table: 00 an empty entry; 05, 0F and 85 an extended partition;
    /// EE a GPT disk's protective partition. A quoted "83" reached Set-Partition -MbrType as
    /// decimal 83, which is 0x53 (operation proof, 2026-11-11).
    [[nodiscard]] static std::optional<int> mbrPartitionType(const QString& typed);
    /// Why @p typed is no partition type for a disk of @p partition_style (MBR or GPT), in plain
    /// words, or empty when it is one.
    [[nodiscard]] static QString partitionTypeBlocker(const QString& partition_style,
                                                      const QString& typed);

private:
    static void validateDiskOperation(const PartitionInventory& inventory,
                                      const PartitionDiskInfo& disk,
                                      const PartitionOperation& operation,
                                      PartitionValidationResult* result);
    static void validatePartitionOperation(const PartitionInventory& inventory,
                                           const PartitionDiskInfo& disk,
                                           const PartitionInfoEx& partition,
                                           const PartitionOperation& operation,
                                           PartitionValidationResult* result);
    static void validatePayloadRawWriteTarget(const PartitionInventory& inventory,
                                              const PartitionDiskInfo& selectedDisk,
                                              const PartitionOperation& operation,
                                              PartitionValidationResult* result);
    static void validateRawVolumeAliasWriteTarget(const PartitionInventory& inventory,
                                                  const PartitionDiskInfo& selectedDisk,
                                                  const PartitionOperation& operation,
                                                  PartitionValidationResult* result);
    static void validateUnallocatedOperation(const PartitionDiskInfo& disk,
                                             const PartitionOperation& operation,
                                             PartitionValidationResult* result);
    static void addCommonDiskWarnings(const PartitionDiskInfo& disk,
                                      PartitionValidationResult* result);
};

}  // namespace sak

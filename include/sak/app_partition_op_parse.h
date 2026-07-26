// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file app_partition_op_parse.h
/// @brief Shared, header-only mapping from AI-assistant partition-op args to a
/// sak::PartitionOperation. ONE copy so the preview op (read-only) and the apply
/// op (mutating) can never drift: the KIND is fixed by the operation, never
/// inferred from which args were supplied, and byte counts convert without UB.

#pragma once

#include "sak/partition_manager_types.h"

#include <QJsonObject>
#include <QLatin1String>
#include <QString>

#include <cstdint>
#include <limits>
#include <optional>

namespace sak {

// A parsed partition op: its enum type plus the target KIND the safety validator
// expects for it. Kind is fixed by the operation, never inferred from which args
// the model supplied -- a partition-scoped op (format/delete/resize/...) must be
// validated on the partition path even when partition_number is omitted, so that a
// missing/invalid partition surfaces as a BLOCKER, never a false "ALLOWED" from the
// rule-less whole-disk path.
struct ParsedPartitionOp {
    PartitionOperationType type;
    PartitionTargetKind kind;
};

// Canonical snake_case name -> (operation type, required target kind). Deliberately
// a CURATED subset of PartitionOperationType: the disk/partition layout ops a
// technician assistant reasons about. The APFS/HFS foreign-file-surgery, imaging,
// and BitLocker/defrag types are a different domain and are omitted -- these ops
// preview/apply "a partition-layout change", nothing else.
[[nodiscard]] inline std::optional<ParsedPartitionOp> parsePartitionOpType(const QString& name) {
    struct Entry {
        const char* name;
        PartitionOperationType type;
        PartitionTargetKind kind;
    };
    constexpr PartitionTargetKind kDisk = PartitionTargetKind::Disk;
    constexpr PartitionTargetKind kPart = PartitionTargetKind::Partition;
    constexpr PartitionTargetKind kFree = PartitionTargetKind::Unallocated;
    static constexpr Entry kMap[] = {
        // Unallocated-scoped (the only op the validator accepts for free space).
        {"create", PartitionOperationType::Create, kFree},
        // Partition-scoped.
        {"delete", PartitionOperationType::Delete, kPart},
        {"format", PartitionOperationType::Format, kPart},
        {"resize", PartitionOperationType::Resize, kPart},
        {"allocate_free_space", PartitionOperationType::AllocateFreeSpace, kPart},
        {"set_drive_letter", PartitionOperationType::SetDriveLetter, kPart},
        {"set_partition_label", PartitionOperationType::SetPartitionLabel, kPart},
        {"set_partition_active", PartitionOperationType::SetPartitionActive, kPart},
        {"set_partition_hidden", PartitionOperationType::SetPartitionHidden, kPart},
        {"set_partition_type_id", PartitionOperationType::SetPartitionTypeId, kPart},
        {"check_file_system", PartitionOperationType::CheckFileSystem, kPart},
        {"merge", PartitionOperationType::Merge, kPart},
        {"split", PartitionOperationType::Split, kPart},
        {"move_partition", PartitionOperationType::MovePartition, kPart},
        {"clone_partition", PartitionOperationType::ClonePartition, kPart},
        {"wipe_partition", PartitionOperationType::WipePartition, kPart},
        {"wipe_free_space", PartitionOperationType::WipeFreeSpace, kPart},
        // Disk-scoped.
        {"convert_partition_style", PartitionOperationType::ConvertPartitionStyle, kDisk},
        {"initialize_disk", PartitionOperationType::InitializeDisk, kDisk},
        {"delete_all_partitions", PartitionOperationType::DeleteAllPartitions, kDisk},
        {"clone_disk", PartitionOperationType::CloneDisk, kDisk},
        {"wipe_disk", PartitionOperationType::WipeDisk, kDisk},
    };
    for (const Entry& entry : kMap) {
        if (name == QLatin1String(entry.name)) {
            return ParsedPartitionOp{entry.type, entry.kind};
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline QString supportedPartitionOpTypes() {
    return QStringLiteral(
        "create, delete, format, resize, allocate_free_space, set_drive_letter, "
        "set_partition_label, set_partition_active, set_partition_hidden, "
        "set_partition_type_id, check_file_system, convert_partition_style, merge, split, "
        "move_partition, initialize_disk, delete_all_partitions, clone_disk, clone_partition, "
        "wipe_partition, wipe_disk, wipe_free_space");
}

// Convert an untrusted JSON byte-count double to uint64_t with no UB: a negative,
// NaN, or out-of-range magnitude never reaches a floating-to-unsigned cast that
// [conv.fpint] leaves undefined. Callers reject negatives up front; this is the
// belt-and-suspenders clamp.
[[nodiscard]] inline uint64_t safeByteCount(double value) {
    if (!(value > 0.0)) {  // <= 0 or NaN
        return 0;
    }
    constexpr double kUint64Ceiling = 18446744073709551616.0;  // 2^64
    if (value >= kUint64Ceiling) {
        return std::numeric_limits<uint64_t>::max();
    }
    return static_cast<uint64_t>(value);
}

// Build the validator/executor target. The KIND is fixed by the operation (passed
// in), not inferred from which args are present, so a partition-scoped op with
// no/invalid partition_number is still validated on the partition path. offset/size
// can exceed 2^31, so read them as double (exact for ints < 2^53) and convert
// without UB.
[[nodiscard]] inline PartitionTarget buildPartitionOpTarget(const QJsonObject& args,
                                                            uint32_t disk_number,
                                                            PartitionTargetKind kind) {
    PartitionTarget target;
    target.kind = kind;
    target.disk_number = disk_number;
    target.partition_number =
        static_cast<uint32_t>(args.value(QStringLiteral("partition_number")).toInt(0));
    target.offset_bytes = safeByteCount(args.value(QStringLiteral("offset_bytes")).toDouble(0.0));
    target.size_bytes = safeByteCount(args.value(QStringLiteral("size_bytes")).toDouble(0.0));
    return target;
}

}  // namespace sak

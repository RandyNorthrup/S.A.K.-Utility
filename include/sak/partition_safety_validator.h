// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_safety_validator.h
/// @brief Safety rules for Partition Manager operations.

#pragma once

#include "sak/partition_manager_types.h"

namespace sak {

struct PartitionValidationResult {
    QStringList blockers;
    QStringList warnings;

    [[nodiscard]] bool allowed() const noexcept { return blockers.isEmpty(); }
};

class PartitionSafetyValidator {
public:
    [[nodiscard]] PartitionValidationResult validate(const PartitionInventory& inventory,
                                                     const PartitionOperation& operation) const;

    [[nodiscard]] static const PartitionDiskInfo* findDisk(const PartitionInventory& inventory,
                                                           uint32_t disk_number);
    [[nodiscard]] static const PartitionInfoEx* findPartition(const PartitionDiskInfo& disk,
                                                              uint32_t partition_number);
    [[nodiscard]] static bool isSystemProtectedPartition(const PartitionInfoEx& partition);

private:
    void validateDiskOperation(const PartitionInventory& inventory,
                               const PartitionDiskInfo& disk,
                               const PartitionOperation& operation,
                               PartitionValidationResult* result) const;
    void validatePartitionOperation(const PartitionInventory& inventory,
                                    const PartitionDiskInfo& disk,
                                    const PartitionInfoEx& partition,
                                    const PartitionOperation& operation,
                                    PartitionValidationResult* result) const;
    void validatePayloadRawWriteTarget(const PartitionInventory& inventory,
                                       const PartitionDiskInfo& selectedDisk,
                                       const PartitionOperation& operation,
                                       PartitionValidationResult* result) const;
    void validateUnallocatedOperation(const PartitionDiskInfo& disk,
                                      const PartitionOperation& operation,
                                      PartitionValidationResult* result) const;
    void addCommonDiskWarnings(const PartitionDiskInfo& disk,
                               PartitionValidationResult* result) const;
};

}  // namespace sak

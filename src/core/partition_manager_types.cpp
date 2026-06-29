// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_manager_types.cpp
/// @brief Shared type helpers for Partition Manager.

#include "sak/partition_manager_types.h"

#include <QUuid>

#include <array>

namespace sak {

namespace {

// Names for the core block/partition-level operations (Create..Initialize Disk).
QStringList corePartitionOperationNames() {
    return {
        QStringLiteral("Create Partition"),
        QStringLiteral("Delete Partition"),
        QStringLiteral("Format Partition"),
        QStringLiteral("Set Drive Letter"),
        QStringLiteral("Set Partition Label"),
        QStringLiteral("Check File System"),
        QStringLiteral("Surface Test"),
        QStringLiteral("Partition Recovery Scan"),
        QStringLiteral("Restore Recovered Partition"),
        QStringLiteral("Hide/Unhide Partition"),
        QStringLiteral("Set Active/Inactive"),
        QStringLiteral("Change Partition Type ID"),
        QStringLiteral("Initialize Disk"),
    };
}

// Names for the advanced layout / clone / imaging / wipe operations.
QStringList advancedPartitionOperationNames() {
    return {
        QStringLiteral("Delete All Partitions"),
        QStringLiteral("Resize Partition"),
        QStringLiteral("Allocate Free Space"),
        QStringLiteral("Convert MBR/GPT"),
        QStringLiteral("Merge Partitions"),
        QStringLiteral("Split Partition"),
        QStringLiteral("Convert File System"),
        QStringLiteral("Change Cluster Size"),
        QStringLiteral("Clone Disk"),
        QStringLiteral("Clone Partition"),
        QStringLiteral("Create Image"),
        QStringLiteral("Restore Image"),
        QStringLiteral("Migrate OS"),
        QStringLiteral("Repair Boot"),
        QStringLiteral("Optimize SSD"),
        QStringLiteral("Defrag Volume"),
        QStringLiteral("BitLocker Unlock"),
        QStringLiteral("BitLocker Suspend"),
        QStringLiteral("BitLocker Resume"),
        QStringLiteral("Wipe Partition"),
        QStringLiteral("Wipe Disk"),
        QStringLiteral("Wipe Free Space"),
        QStringLiteral("Move Partition"),
        QStringLiteral("Convert Primary/Logical"),
        QStringLiteral("Change Volume Serial Number"),
        QStringLiteral("Convert Dynamic Disk to Basic"),
    };
}

// Names for the APFS root file-system mutation operations.
QStringList apfsOperationNames() {
    return {
        QStringLiteral("APFS Write Root File"),
        QStringLiteral("APFS Patch Root File"),
        QStringLiteral("APFS Patch Root Directory File"),
        QStringLiteral("APFS Delete Root File"),
        QStringLiteral("APFS Write Root Directory File"),
        QStringLiteral("APFS Delete Root Directory File"),
        QStringLiteral("APFS Create Root Directory"),
        QStringLiteral("APFS Delete Root Directory"),
        QStringLiteral("APFS Change Volume Label"),
        QStringLiteral("APFS Snapshot Create"),
        QStringLiteral("APFS Snapshot Delete"),
        QStringLiteral("APFS Snapshot Revert"),
    };
}

// Names for the HFS catalog / fork / attribute mutation operations.
QStringList hfsOperationNames() {
    return {
        QStringLiteral("HFS Overwrite File"),
        QStringLiteral("HFS Replace File"),
        QStringLiteral("HFS Grow File"),
        QStringLiteral("HFS Truncate File"),
        QStringLiteral("HFS Replace Resource Fork"),
        QStringLiteral("HFS Grow Resource Fork"),
        QStringLiteral("HFS Truncate Resource Fork"),
        QStringLiteral("HFS Create Empty File"),
        QStringLiteral("HFS Create File"),
        QStringLiteral("HFS Delete Empty File"),
        QStringLiteral("HFS Delete File"),
        QStringLiteral("HFS Create Empty Folder"),
        QStringLiteral("HFS Delete Empty Folder"),
        QStringLiteral("HFS Delete Folder Tree"),
        QStringLiteral("HFS Rename/Move Catalog Entry"),
        QStringLiteral("HFS Replace Inline Attribute"),
        QStringLiteral("HFS Replace Fork Attribute"),
        QStringLiteral("HFS Grow Fork Attribute"),
    };
}

// Builds the operation-name table in PartitionOperationType enum order.
QStringList buildPartitionOperationNames() {
    QStringList names = corePartitionOperationNames();
    names += advancedPartitionOperationNames();
    names += apfsOperationNames();
    names += hfsOperationNames();
    return names;
}

}  // namespace

QString toDisplayString(PartitionOperationType type) {
    static const QStringList kNames = buildPartitionOperationNames();
    const int index = static_cast<int>(type);
    return index >= 0 && index < kNames.size() ? kNames.at(index)
                                               : QStringLiteral("Unknown Operation");
}

QString toDisplayString(OperationRisk risk) {
    switch (risk) {
    case OperationRisk::ReadOnly:
        return QStringLiteral("Read-only");
    case OperationRisk::Low:
        return QStringLiteral("Low");
    case OperationRisk::Destructive:
        return QStringLiteral("Destructive");
    case OperationRisk::SystemCritical:
        return QStringLiteral("System-critical");
    }
    return QStringLiteral("Unknown");
}

QString toDisplayString(PartitionManagerState state) {
    static const QStringList kNames = {
        QStringLiteral("Idle"),
        QStringLiteral("Refreshing inventory"),
        QStringLiteral("Ready"),
        QStringLiteral("Planning operation"),
        QStringLiteral("Queue dirty"),
        QStringLiteral("Running preflight"),
        QStringLiteral("Awaiting elevation"),
        QStringLiteral("Applying operations"),
        QStringLiteral("Verifying"),
        QStringLiteral("Failed"),
        QStringLiteral("Cancelled"),
    };
    const int index = static_cast<int>(state);
    return index >= 0 && index < kNames.size() ? kNames.at(index) : QStringLiteral("Unknown");
}

QString makePartitionOperationId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString formatPartitionBytes(uint64_t bytes) {
    constexpr double kUnit = 1024.0;
    constexpr uint64_t kKiB = 1024ULL;
    constexpr uint64_t kMiB = kKiB * 1024ULL;
    constexpr uint64_t kGiB = kMiB * 1024ULL;
    constexpr uint64_t kTiB = kGiB * 1024ULL;

    if (bytes >= kTiB) {
        return QStringLiteral("%1 TB").arg(
            static_cast<double>(bytes) / kTiB, 0, 'f', kPartitionByteDisplayPrecision);
    }
    if (bytes >= kGiB) {
        return QStringLiteral("%1 GB").arg(
            static_cast<double>(bytes) / kGiB, 0, 'f', kPartitionByteDisplayPrecision);
    }
    if (bytes >= kMiB) {
        return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / kMiB, 0, 'f', 1);
    }
    if (bytes >= kKiB) {
        return QStringLiteral("%1 KB").arg(static_cast<double>(bytes) / kUnit, 0, 'f', 1);
    }
    return QStringLiteral("%1 bytes").arg(bytes);
}

}  // namespace sak

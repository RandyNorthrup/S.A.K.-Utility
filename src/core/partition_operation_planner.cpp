// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_operation_planner.cpp
/// @brief Operation preview builder for Partition Manager.

#include "sak/partition_operation_planner.h"

#include <QHash>

namespace sak {

namespace {

OperationRisk riskForType(PartitionOperationType type) {
    static const QHash<int, OperationRisk> kRisks = {
        {static_cast<int>(PartitionOperationType::Create), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::Delete), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::Format), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::SetDriveLetter), OperationRisk::Low},
        {static_cast<int>(PartitionOperationType::SetPartitionLabel), OperationRisk::Low},
        {static_cast<int>(PartitionOperationType::CheckFileSystem), OperationRisk::ReadOnly},
        {static_cast<int>(PartitionOperationType::SurfaceTest), OperationRisk::ReadOnly},
        {static_cast<int>(PartitionOperationType::PartitionRecoveryScan), OperationRisk::ReadOnly},
        {static_cast<int>(PartitionOperationType::RestoreRecoveredPartition),
         OperationRisk::SystemCritical},
        {static_cast<int>(PartitionOperationType::SetPartitionHidden), OperationRisk::Low},
        {static_cast<int>(PartitionOperationType::SetPartitionActive),
         OperationRisk::SystemCritical},
        {static_cast<int>(PartitionOperationType::SetPartitionTypeId),
         OperationRisk::SystemCritical},
        {static_cast<int>(PartitionOperationType::InitializeDisk), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::DeleteAllPartitions), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::Resize), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::AllocateFreeSpace), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::ConvertPartitionStyle),
         OperationRisk::SystemCritical},
        {static_cast<int>(PartitionOperationType::Merge), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::Split), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::ConvertFileSystem), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::ChangeClusterSize), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::CloneDisk), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::ClonePartition), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::CreateImage), OperationRisk::ReadOnly},
        {static_cast<int>(PartitionOperationType::RestoreImage), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::MigrateOs), OperationRisk::SystemCritical},
        {static_cast<int>(PartitionOperationType::RepairBoot), OperationRisk::SystemCritical},
        {static_cast<int>(PartitionOperationType::OptimizeSsd), OperationRisk::Low},
        {static_cast<int>(PartitionOperationType::DefragVolume), OperationRisk::Low},
        {static_cast<int>(PartitionOperationType::BitLockerUnlock), OperationRisk::Low},
        {static_cast<int>(PartitionOperationType::BitLockerSuspend), OperationRisk::Low},
        {static_cast<int>(PartitionOperationType::BitLockerResume), OperationRisk::Low},
        {static_cast<int>(PartitionOperationType::WipePartition), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::WipeDisk), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::WipeFreeSpace), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::MovePartition), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::ConvertPrimaryLogical),
         OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::ChangeVolumeSerialNumber),
         OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::ConvertDynamicDiskToBasic),
         OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::ApfsWriteRootFile), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::ApfsPatchRootFile), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::ApfsPatchRootDirectoryFile),
         OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::ApfsDeleteRootFile), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::ApfsWriteRootDirectoryFile),
         OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::ApfsDeleteRootDirectoryFile),
         OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::ApfsCreateRootDirectory), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::ApfsDeleteRootDirectory), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::ApfsChangeVolumeLabel), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::HfsOverwriteFile), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::HfsReplaceFile), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::HfsGrowFile), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::HfsTruncateFile), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::HfsReplaceResourceFork),
         OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::HfsGrowResourceFork),
         OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::HfsTruncateResourceFork),
         OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::HfsCreateEmptyFile),
         OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::HfsCreateFile), OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::HfsDeleteEmptyFile),
         OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::HfsDeleteFile),
         OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::HfsCreateEmptyFolder),
         OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::HfsDeleteEmptyFolder),
         OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::HfsDeleteFolderTree),
         OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::HfsRenameMoveCatalogEntry),
         OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::HfsReplaceInlineAttribute),
         OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::HfsReplaceForkAttribute),
         OperationRisk::Destructive},
        {static_cast<int>(PartitionOperationType::HfsGrowForkAttribute),
         OperationRisk::Destructive},
    };
    return kRisks.value(static_cast<int>(type), OperationRisk::Low);
}

QString targetSummary(const PartitionTarget& target) {
    switch (target.kind) {
    case PartitionTargetKind::Disk:
        return QStringLiteral("Disk %1").arg(target.disk_number);
    case PartitionTargetKind::Partition:
    case PartitionTargetKind::Volume:
        return QStringLiteral("Disk %1 Partition %2")
            .arg(target.disk_number)
            .arg(target.partition_number);
    case PartitionTargetKind::Unallocated:
        return QStringLiteral("Disk %1 unallocated region at %2")
            .arg(target.disk_number)
            .arg(formatPartitionBytes(target.offset_bytes));
    }
    return QStringLiteral("Unknown target");
}

}  // namespace

OperationPreview PartitionOperationPlanner::previewOperation(const PartitionInventory& inventory,
                                                             PartitionOperation operation) const {
    OperationPreview preview;
    preview.before_layout_hash = inventory.layout_hash;
    if (operation.id.isEmpty()) {
        operation.id = makePartitionOperationId();
    }
    fillRiskAndSummary(&operation);

    const auto validation = m_validator.validate(inventory, operation);
    operation.blockers.append(validation.blockers);
    operation.warnings.append(validation.warnings);
    preview.blockers.append(validation.blockers);
    preview.warnings.append(validation.warnings);

    expandCompositeOperation(operation, &preview);
    preview.after_layout_description =
        preview.operations.isEmpty()
            ? QStringLiteral("No operation planned")
            : QStringLiteral("%1 operation(s) queued").arg(preview.operations.size());
    return preview;
}

PartitionOperation PartitionOperationPlanner::makeOperation(PartitionOperationType type,
                                                            const PartitionTarget& target,
                                                            const QJsonObject& payload) {
    PartitionOperation operation;
    operation.id = makePartitionOperationId();
    operation.type = type;
    operation.target = target;
    operation.payload = payload;
    operation.risk = riskForType(type);
    return operation;
}

void PartitionOperationPlanner::fillRiskAndSummary(PartitionOperation* operation) const {
    operation->risk = riskForType(operation->type);
    operation->summary = QStringLiteral("%1 - %2").arg(toDisplayString(operation->type),
                                                       targetSummary(operation->target));
}

void PartitionOperationPlanner::expandCompositeOperation(const PartitionOperation& operation,
                                                         OperationPreview* preview) const {
    preview->operations.append(operation);
}

}  // namespace sak

// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_script_builder.h
/// @brief Typed PowerShell script generation for Partition Manager.

#pragma once

#include "sak/partition_manager_types.h"

#include <QHash>

namespace sak {

struct PartitionFileSystemToolCommand;
struct ExternalFileSystemToolScriptRequest;

struct PartitionScript {
    QString script;
    QString dry_run_script;
    QString preview;
    QStringList blockers;
    int timeout_seconds{kPartitionDefaultTaskTimeoutSeconds};

    [[nodiscard]] bool valid() const noexcept { return blockers.isEmpty() && !script.isEmpty(); }
};

class PartitionScriptBuilder {
public:
    [[nodiscard]] PartitionScript buildScript(const PartitionOperation& operation) const;
    [[nodiscard]] static QString quotePowerShell(const QString& value);
    [[nodiscard]] static bool isValidDriveLetter(const QString& value);
    [[nodiscard]] static bool isSupportedFileSystem(const QString& value);

private:
    using Builder = PartitionScript (PartitionScriptBuilder::*)(const PartitionOperation&) const;

    [[nodiscard]] static QHash<int, Builder> buildOperationDispatchTable();
    static void appendCoreBuilders(QHash<int, Builder>* builders);
    static void appendLayoutBuilders(QHash<int, Builder>* builders);
    static void appendCloneAndMaintenanceBuilders(QHash<int, Builder>* builders);
    static void appendAdvancedBuilders(QHash<int, Builder>* builders);

    [[nodiscard]] PartitionScript buildCreateScript(const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildDeleteScript(const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildFormatScript(const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildSetDriveLetterScript(
        const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildSetPartitionLabelScript(
        const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildCheckFileSystemScript(
        const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildExternalFileSystemToolScript(
        const PartitionOperation& operation,
        const ExternalFileSystemToolScriptRequest& request) const;
    [[nodiscard]] PartitionScript buildSurfaceTestScript(const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildPartitionRecoveryScanScript(
        const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildRestoreRecoveredPartitionScript(
        const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildSetPartitionHiddenScript(
        const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildSetPartitionActiveScript(
        const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildSetPartitionTypeIdScript(
        const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildInitializeDiskScript(
        const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildDeleteAllPartitionsScript(
        const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildResizeScript(const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildAllocateFreeSpaceScript(
        const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildConvertStyleScript(
        const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildMergeScript(const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildSplitScript(const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildConvertFileSystemScript(
        const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildChangeClusterSizeScript(
        const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildCloneOrImageScript(
        const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildBootRepairScript(const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildOptimizeSsdScript(const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildDefragVolumeScript(
        const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildBitLockerScript(const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildWipeScript(const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildMovePartitionScript(
        const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildConvertPrimaryLogicalScript(
        const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildChangeVolumeSerialNumberScript(
        const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildConvertDynamicDiskToBasicScript(
        const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildApfsRootFileMutationScript(
        const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildHfsFileMutationScript(
        const PartitionOperation& operation) const;

    [[nodiscard]] static PartitionScript invalidScript(const QString& blocker);
};

}  // namespace sak

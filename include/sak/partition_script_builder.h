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

// A secret the executor must materialize to a locked temp file just before running the
// script: it replaces `placeholder` in `script` with the temp file's path, then shred-deletes
// the file afterward. Keeps credentials (e.g. a FileVault format password) out of the script
// text and off every child-process command line.
struct PartitionScriptCredential {
    QString placeholder;
    QString secret;
};

struct PartitionScript {
    QString script{};
    QString dry_run_script{};
    QString preview{};
    QStringList blockers{};
    QList<PartitionScriptCredential> credential_files{};
    int timeout_seconds{kPartitionDefaultTaskTimeoutSeconds};

    [[nodiscard]] bool valid() const noexcept { return blockers.isEmpty() && !script.isEmpty(); }
};

/// The PowerShell that finds the dynamic disks Get-Disk does not list. The scan and the conversion
/// both run it, so they cannot disagree about which disk is which.
[[nodiscard]] QString dynamicDiskFunctionsScript();

/// How long one question to a drive or a volume may take before it is that drive's or that
/// volume's error, in a scan and in a script that asks the same question.
inline constexpr int kDriveQuerySeconds = 20;

/// PowerShell function Invoke-SakBoundedProgram: runs a program for at most a number of seconds and
/// says whether it started and finished, with its exit code and output. Public so a test can run
/// it.
[[nodiscard]] QString boundedProgramFunctionScript();

/// PowerShell function Get-SakRotationRateRpm: the rotation rate a drive reports through the tool
/// at @p tool_path, or 0 when it reports none, or the tool is absent, fails, or takes too long.
/// The scan and the defrag script both run it, so they cannot disagree. It takes the path so a
/// test can run it against a stand-in that reads no disk.
[[nodiscard]] QString driveRotationRateFunctionScript(const QString& tool_path);
/// The same, for the smartctl this application ships.
[[nodiscard]] QString bundledDriveRotationRateFunctionScript();

class PartitionScriptBuilder {
public:
    [[nodiscard]] PartitionScript buildScript(const PartitionOperation& operation) const;
    [[nodiscard]] static QString quotePowerShell(const QString& value);

    /// The line an elevated script starts its native types with (security brief 4a):
    /// sak_script_types.dll beside the program, verified and held with every program file before
    /// the helper started, loaded from its bytes. Nothing is compiled into the temporary folder.
    [[nodiscard]] static QString scriptTypesLoaderScript();
    [[nodiscard]] static bool isValidDriveLetter(const QString& value);
    [[nodiscard]] static bool isSupportedFileSystem(const QString& value);
    /// PowerShell function: which file system a 512-byte sector is the boot sector of, or $null.
    /// Public so a test can run it on bytes, with no disk involved.
    [[nodiscard]] static QString bootSectorKindFunctionScript();
    /// PowerShell functions of the partition recovery scan: what each boot sector found is, its
    /// size, and whether it is a partition there now (Invoke-SakRecoveryScan reads any stream).
    [[nodiscard]] static QString recoveryScanFunctionScript();
    /// PowerShell function the helper runs before writing a recovered partition back: refuses
    /// one past the disk's end or over any partition there now.
    [[nodiscard]] static QString recoveredPartitionFitFunctionScript();
    /// PowerShell function: a drive letter nothing holds, the preferred one if it is free.
    [[nodiscard]] static QString freeBootLetterFunctionScript();
    /// PowerShell that removes a partition's drive letters and folder paths (its own
    /// \\?\Volume{...}\ path is not one). A non-Windows Format runs it before it takes the
    /// partition: a lettered RAW volume is what Explorer holds open to offer "You need to format
    /// the disk", and the APFS writer could not lock one (VM proof, 2026-11-11). Public so a test
    /// can run it.
    [[nodiscard]] static QString removePartitionAccessPathsScript(const QString& disk,
                                                                  const QString& partition);
    /// PowerShell run after S.A.K. formats or creates a partition with a file system Windows does
    /// not mount (ext2/3/4, Linux swap, HFS+, APFS): removes the partition's drive letters and
    /// folder paths, then sets that file system's own partition type (GPT: Linux file system,
    /// Linux swap, Apple HFS+, APFS; MBR: 0x83, 0x82, 0xAF), so Windows never offers to format
    /// it. Empty for any other file system. @p disk and @p partition are PowerShell expressions.
    /// Public so a test can run it.
    [[nodiscard]] static QString nonWindowsPartitionTypeScript(const QString& file_system,
                                                               const QString& disk,
                                                               const QString& partition);
    /// PowerShell run before Windows formats a partition (Format-Volume): a partition with one of
    /// the types above has no Windows volume to format, so its type is set back to a Windows data
    /// partition (GPT basic data; MBR 0x07, or 0x0C for FAT32) and the volume waited for. Any
    /// other type is left as it is. Public so a test can run it.
    [[nodiscard]] static QString windowsDataPartitionTypeScript(const QString& file_system,
                                                                const QString& disk,
                                                                const QString& partition);
    /// PowerShell functions every Resize and APFS grow runs: Invoke-SakResizeAsDataPartition
    /// resizes a partition of any type, as a data partition for the moment of the resize when it
    /// is not one, and gives it its own type, attributes and letters back afterwards, the resize
    /// failed or not. Public so a test can run it against Windows itself.
    [[nodiscard]] static QString resizeAsDataPartitionFunctionScript();

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
    [[nodiscard]] static PartitionScript buildExternalFileSystemToolScript(
        const PartitionOperation& operation, const ExternalFileSystemToolScriptRequest& request);
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
    [[nodiscard]] PartitionScript buildOsMigrationScript(const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildBootRepairScript(const PartitionOperation& operation) const;
    [[nodiscard]] static PartitionScript buildAttachedBootRepairScript(
        const PartitionOperation& operation);
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
    [[nodiscard]] PartitionScript buildApfsSnapshotScript(
        const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildApfsCloneHardlinkResizeScript(
        const PartitionOperation& operation) const;
    [[nodiscard]] PartitionScript buildHfsFileMutationScript(
        const PartitionOperation& operation) const;

    [[nodiscard]] static PartitionScript invalidScript(const QString& blocker);
};

}  // namespace sak

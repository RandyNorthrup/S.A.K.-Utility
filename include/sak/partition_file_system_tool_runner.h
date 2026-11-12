// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_file_system_tool_runner.h
/// @brief Manifest-gated external filesystem tool runner for Partition Manager.

#pragma once

#include "sak/partition_file_system_tool_manifest.h"
#include "sak/partition_manager_types.h"
#include "sak/process_runner.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace sak {

struct PartitionFileSystemToolCommand {
    QString tool_id;
    QString operation;
    QString file_system;
    QStringList arguments;
    QStringList blockers;

    [[nodiscard]] bool ok() const noexcept { return blockers.isEmpty() && !tool_id.isEmpty(); }
};

struct PartitionFileSystemToolResolution {
    bool ok{false};
    QString tool_path;
    PartitionFileSystemToolSpec tool;
    QStringList blockers;
};

struct PartitionFileSystemReadOnlyCheckRequest {
    QString manifest_path;
    QString tools_root;
    QString file_system;
    QString target_path;
    int timeout_ms{0};
    /// The read-only CheckFileSystem operation the elevated helper runs for this check, built
    /// from the scan's partition at @p target_path, or nothing when the scan has none there; and
    /// why the queue's validation would refuse it (PartitionManagerController's helper route).
    std::optional<PartitionOperation> helper_operation;
    QStringList helper_blockers;
};

struct PartitionFileSystemToolRunResult {
    bool success{false};
    bool blocked{false};
    QString tool_id;
    QString tool_path;
    QStringList arguments;
    int exit_code{-1};
    bool timed_out{false};
    bool cancelled{false};
    QString std_out;
    QString std_err;
    QStringList blockers;
    /// Why the run did not pass, in plain words, when the tool's own answer says more than its
    /// exit code (e2fsck: PartitionFileSystemToolRunner::e2fsckProblem).
    QString problem;
};

class PartitionFileSystemToolRunner {
public:
    [[nodiscard]] static QString readOnlyCheckOperation();
    [[nodiscard]] static QString formatOperation();
    [[nodiscard]] static QString repairOperation();
    [[nodiscard]] static QString resizeOperation();
    [[nodiscard]] static PartitionFileSystemToolCommand buildReadOnlyCheckCommand(
        const QString& file_system, const QString& target_path);
    [[nodiscard]] static PartitionFileSystemToolCommand buildFormatCommand(
        const QString& file_system,
        const QString& target_path,
        const QString& label,
        bool destructive_confirmed);
    [[nodiscard]] static PartitionFileSystemToolCommand buildRepairCommand(
        const QString& file_system, const QString& target_path, bool destructive_confirmed);
    [[nodiscard]] static PartitionFileSystemToolCommand buildResizeCommand(
        const QString& file_system,
        const QString& target_path,
        bool destructive_confirmed,
        const QString& new_size_argument = {});
    [[nodiscard]] static PartitionFileSystemToolResolution resolveApprovedTool(
        const QString& manifest_path,
        const QString& tools_root,
        const QString& tool_id,
        const QString& operation,
        const QString& file_system);
    [[nodiscard]] static PartitionFileSystemToolRunResult runReadOnlyCheck(
        const PartitionFileSystemReadOnlyCheckRequest& request,
        const CancelCheck& should_cancel = {});
    /// Why an e2fsck run did not pass, in plain words, or empty when it did. Its output is read
    /// first: e2fsck -n exits 0 after answering "no" to "Abort?" on a file system larger than its
    /// partition ("Either the superblock or the partition table is likely to be corrupt!";
    /// operation proof, 2026-11-11), so an exit code alone said healthy. Then the exit code, as
    /// e2fsck defines it: 0 clean, 1 and 2 corrected (only a repair, -p or -y, corrects), 4 and up
    /// errors left, an operational error, a usage error or a cancel. @p accepted_exit_codes are
    /// the codes this run may end with.
    [[nodiscard]] static QString e2fsckProblem(int exit_code,
                                               const QString& output,
                                               const QList<int>& accepted_exit_codes);
    /// Get-SakE2fsckProblem: the same reading, in Windows PowerShell, for the generated scripts
    /// that run e2fsck (a repair, and the checks before and after a shrink).
    [[nodiscard]] static QString e2fsckProblemFunctionScript();
    /// When a tool says it could not open the partition, that, in plain words, or empty. e2fsck
    /// run without the rights to open \\?\GLOBALROOT\Device\HarddiskN\PartitionM says
    /// "Illegal or malformed device name while trying to open" and then that the superblock could
    /// not be read or is corrupt, which reads as damage to a file system it never read (operation
    /// proof, 2026-11-12, F25). Checked before anything else a tool says.
    [[nodiscard]] static QString openFailureProblem(const QString& output);
    /// Get-SakToolOpenFailure: the same reading, in Windows PowerShell.
    [[nodiscard]] static QString openFailureFunctionScript();
};

}  // namespace sak

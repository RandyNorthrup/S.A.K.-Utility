// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_file_system_tool_runner.h
/// @brief Manifest-gated external filesystem tool runner for Partition Manager.

#pragma once

#include "sak/partition_file_system_tool_manifest.h"
#include "sak/process_runner.h"

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
};

}  // namespace sak

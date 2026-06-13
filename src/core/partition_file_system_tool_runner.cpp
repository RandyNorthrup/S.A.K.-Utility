// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_file_system_tool_runner.cpp
/// @brief Manifest-gated external filesystem tool runner for Partition Manager.

#include "sak/partition_file_system_tool_runner.h"

#include "sak/layout_constants.h"
#include "sak/partition_manager_types.h"

#include <QDir>
#include <QFileInfo>

#include <optional>

namespace sak {

namespace {

constexpr auto kReadOnlyCheckOperation = "check-read-only";
constexpr auto kFormatOperation = "format";
constexpr auto kRepairOperation = "repair";
constexpr auto kResizeOperation = "resize";
constexpr auto kE2fsckToolId = "e2fsck";
constexpr auto kMke2fsToolId = "mke2fs";
constexpr auto kResize2fsToolId = "resize2fs";
constexpr auto kNewfsHfsToolId = "newfs_hfs";
constexpr auto kFsckHfsToolId = "fsck_hfs";
constexpr auto kXfsRepairToolId = "xfs_repair";
constexpr auto kBtrfsToolId = "btrfs";
constexpr auto kExt2FileSystem = "ext2";
constexpr auto kExt3FileSystem = "ext3";
constexpr auto kExt4FileSystem = "ext4";
constexpr auto kHfsPlusFileSystem = "hfs+";
constexpr auto kHfsPlusAltFileSystem = "hfsplus";
constexpr auto kHfsxFileSystem = "hfsx";
constexpr auto kXfsFileSystem = "xfs";
constexpr auto kBtrfsFileSystem = "btrfs";
constexpr qsizetype kExtVolumeLabelMaxChars = 16;
constexpr qsizetype kHfsVolumeLabelMaxChars = 255;

QString normalizedToken(const QString& value) {
    return value.trimmed().toLower();
}

bool containsToken(const QStringList& values, const QString& token) {
    return values.contains(token, Qt::CaseInsensitive);
}

bool containsUnsafeText(const QString& value) {
    return value.contains(QLatin1Char('\n')) || value.contains(QLatin1Char('\r')) ||
           value.contains(QChar::Null);
}

void appendTargetBlockers(const QString& targetPath, QStringList* blockers) {
    if (targetPath.trimmed().isEmpty()) {
        blockers->append(QStringLiteral("Target device or image path is required"));
    }
    if (containsUnsafeText(targetPath)) {
        blockers->append(QStringLiteral("Target path contains unsafe control characters"));
    }
}

void appendDestructiveConfirmationBlocker(bool destructiveConfirmed, QStringList* blockers) {
    if (!destructiveConfirmed) {
        blockers->append(QStringLiteral("Destructive filesystem operation requires confirmation"));
    }
}

std::optional<PartitionFileSystemToolSpec> findTool(
    const QVector<PartitionFileSystemToolSpec>& tools, const QString& toolId) {
    for (const auto& tool : tools) {
        if (tool.id == toolId) {
            return tool;
        }
    }
    return std::nullopt;
}

int effectiveTimeoutMs(int requestedTimeoutMs) {
    if (requestedTimeoutMs > 0) {
        return requestedTimeoutMs;
    }
    return kPartitionMediumTaskTimeoutSeconds * kMillisecondsPerSecond;
}

PartitionFileSystemToolCommand blockedCommand(const QString& fileSystem,
                                              const QString& targetPath,
                                              const QString& operation,
                                              const QString& blocker) {
    PartitionFileSystemToolCommand command;
    command.operation = operation;
    command.file_system = normalizedToken(fileSystem);
    command.blockers.append(blocker);
    appendTargetBlockers(targetPath, &command.blockers);
    return command;
}

bool isExtFileSystem(const QString& fileSystem) {
    return fileSystem == QString::fromLatin1(kExt2FileSystem) ||
           fileSystem == QString::fromLatin1(kExt3FileSystem) ||
           fileSystem == QString::fromLatin1(kExt4FileSystem);
}

bool isHfsFileSystem(const QString& fileSystem) {
    return fileSystem == QString::fromLatin1(kHfsPlusFileSystem) ||
           fileSystem == QString::fromLatin1(kHfsPlusAltFileSystem) ||
           fileSystem == QString::fromLatin1(kHfsxFileSystem);
}

QString normalizedHfsToolFileSystem(const QString& fileSystem) {
    return fileSystem == QString::fromLatin1(kHfsxFileSystem)
               ? QString::fromLatin1(kHfsxFileSystem)
               : QString::fromLatin1(kHfsPlusFileSystem);
}

QString hfsToolTargetPath(const QString& targetPath) {
    const QString trimmed = targetPath.trimmed();
    if (trimmed.startsWith(QStringLiteral("\\\\?\\")) ||
        trimmed.startsWith(QStringLiteral("\\\\.\\"))) {
        QString converted = trimmed;
        converted.replace(QLatin1Char('\\'), QLatin1Char('/'));
        return converted;
    }
    return trimmed;
}

}  // namespace

QString PartitionFileSystemToolRunner::readOnlyCheckOperation() {
    return QString::fromLatin1(kReadOnlyCheckOperation);
}

QString PartitionFileSystemToolRunner::formatOperation() {
    return QString::fromLatin1(kFormatOperation);
}

QString PartitionFileSystemToolRunner::repairOperation() {
    return QString::fromLatin1(kRepairOperation);
}

QString PartitionFileSystemToolRunner::resizeOperation() {
    return QString::fromLatin1(kResizeOperation);
}

PartitionFileSystemToolCommand PartitionFileSystemToolRunner::buildReadOnlyCheckCommand(
    const QString& file_system, const QString& target_path) {
    const QString fileSystem = normalizedToken(file_system);
    const QString targetPath = target_path.trimmed();
    if (isExtFileSystem(fileSystem)) {
        PartitionFileSystemToolCommand command{.tool_id = QString::fromLatin1(kE2fsckToolId),
                                               .operation = readOnlyCheckOperation(),
                                               .file_system = fileSystem,
                                               .arguments = {QStringLiteral("-n"),
                                                             QStringLiteral("-f"),
                                                             targetPath}};
        appendTargetBlockers(target_path, &command.blockers);
        return command;
    }
    if (fileSystem == QString::fromLatin1(kXfsFileSystem)) {
        PartitionFileSystemToolCommand command{.tool_id = QString::fromLatin1(kXfsRepairToolId),
                                               .operation = readOnlyCheckOperation(),
                                               .file_system = fileSystem,
                                               .arguments = {QStringLiteral("-n"), targetPath}};
        appendTargetBlockers(target_path, &command.blockers);
        return command;
    }
    if (fileSystem == QString::fromLatin1(kBtrfsFileSystem)) {
        PartitionFileSystemToolCommand command{.tool_id = QString::fromLatin1(kBtrfsToolId),
                                               .operation = readOnlyCheckOperation(),
                                               .file_system = fileSystem,
                                               .arguments = {QStringLiteral("check"),
                                                             QStringLiteral("--readonly"),
                                                             targetPath}};
        appendTargetBlockers(target_path, &command.blockers);
        return command;
    }
    if (isHfsFileSystem(fileSystem)) {
        const QString hfsTargetPath = hfsToolTargetPath(targetPath);
        PartitionFileSystemToolCommand command{
            .tool_id = QString::fromLatin1(kFsckHfsToolId),
            .operation = readOnlyCheckOperation(),
            .file_system = normalizedHfsToolFileSystem(fileSystem),
            .arguments = {QStringLiteral("-n"), QStringLiteral("-f"), hfsTargetPath}};
        appendTargetBlockers(target_path, &command.blockers);
        return command;
    }
    return blockedCommand(fileSystem,
                          target_path,
                          readOnlyCheckOperation(),
                          QStringLiteral("No approved read-only checker is mapped for '%1'")
                              .arg(fileSystem.isEmpty() ? QStringLiteral("<unknown>")
                                                        : fileSystem));
}

PartitionFileSystemToolCommand PartitionFileSystemToolRunner::buildFormatCommand(
    const QString& file_system,
    const QString& target_path,
    const QString& label,
    bool destructive_confirmed) {
    const QString fileSystem = normalizedToken(file_system);
    const QString targetPath = target_path.trimmed();
    if (isHfsFileSystem(fileSystem)) {
        const QString hfsTargetPath = hfsToolTargetPath(targetPath);
        PartitionFileSystemToolCommand command{
            .tool_id = QString::fromLatin1(kNewfsHfsToolId),
            .operation = formatOperation(),
            .file_system = normalizedHfsToolFileSystem(fileSystem),
            .arguments = {}};
        if (fileSystem == QString::fromLatin1(kHfsxFileSystem)) {
            command.arguments.append(QStringLiteral("-s"));
        }
        const QString trimmedLabel = label.trimmed();
        if (!trimmedLabel.isEmpty()) {
            command.arguments.append(QStringLiteral("-v"));
            command.arguments.append(trimmedLabel.left(kHfsVolumeLabelMaxChars));
        }
        command.arguments.append(hfsTargetPath);
        appendTargetBlockers(target_path, &command.blockers);
        appendDestructiveConfirmationBlocker(destructive_confirmed, &command.blockers);
        return command;
    }

    if (!isExtFileSystem(fileSystem)) {
        return blockedCommand(fileSystem,
                              target_path,
                              formatOperation(),
                              QStringLiteral("No approved formatter is mapped for '%1'")
                                  .arg(fileSystem.isEmpty() ? QStringLiteral("<unknown>")
                                                            : fileSystem));
    }

    PartitionFileSystemToolCommand command{.tool_id = QString::fromLatin1(kMke2fsToolId),
                                           .operation = formatOperation(),
                                           .file_system = fileSystem,
                                           .arguments = {QStringLiteral("-t"),
                                                         fileSystem,
                                                         QStringLiteral("-F")}};
    const QString trimmedLabel = label.trimmed();
    if (!trimmedLabel.isEmpty()) {
        command.arguments.append(QStringLiteral("-L"));
        command.arguments.append(trimmedLabel.left(kExtVolumeLabelMaxChars));
    }
    command.arguments.append(targetPath);
    appendTargetBlockers(target_path, &command.blockers);
    appendDestructiveConfirmationBlocker(destructive_confirmed, &command.blockers);
    return command;
}

PartitionFileSystemToolCommand PartitionFileSystemToolRunner::buildRepairCommand(
    const QString& file_system, const QString& target_path, bool destructive_confirmed) {
    const QString fileSystem = normalizedToken(file_system);
    const QString targetPath = target_path.trimmed();
    if (isHfsFileSystem(fileSystem)) {
        const QString hfsTargetPath = hfsToolTargetPath(targetPath);
        PartitionFileSystemToolCommand command{
            .tool_id = QString::fromLatin1(kFsckHfsToolId),
            .operation = repairOperation(),
            .file_system = normalizedHfsToolFileSystem(fileSystem),
            .arguments = {QStringLiteral("-p"), QStringLiteral("-f"), hfsTargetPath}};
        appendTargetBlockers(target_path, &command.blockers);
        appendDestructiveConfirmationBlocker(destructive_confirmed, &command.blockers);
        return command;
    }

    if (!isExtFileSystem(fileSystem)) {
        return blockedCommand(fileSystem,
                              target_path,
                              repairOperation(),
                              QStringLiteral("No approved repair tool is mapped for '%1'")
                                  .arg(fileSystem.isEmpty() ? QStringLiteral("<unknown>")
                                                            : fileSystem));
    }

    PartitionFileSystemToolCommand command{.tool_id = QString::fromLatin1(kE2fsckToolId),
                                           .operation = repairOperation(),
                                           .file_system = fileSystem,
                                           .arguments = {QStringLiteral("-p"),
                                                         QStringLiteral("-f"),
                                                         targetPath}};
    appendTargetBlockers(target_path, &command.blockers);
    appendDestructiveConfirmationBlocker(destructive_confirmed, &command.blockers);
    return command;
}

PartitionFileSystemToolCommand PartitionFileSystemToolRunner::buildResizeCommand(
    const QString& file_system,
    const QString& target_path,
    bool destructive_confirmed,
    const QString& new_size_argument) {
    const QString fileSystem = normalizedToken(file_system);
    const QString targetPath = target_path.trimmed();
    if (!isExtFileSystem(fileSystem)) {
        return blockedCommand(fileSystem,
                              target_path,
                              resizeOperation(),
                              QStringLiteral("No approved resize tool is mapped for '%1'")
                                  .arg(fileSystem.isEmpty() ? QStringLiteral("<unknown>")
                                                            : fileSystem));
    }

    PartitionFileSystemToolCommand command{.tool_id = QString::fromLatin1(kResize2fsToolId),
                                           .operation = resizeOperation(),
                                           .file_system = fileSystem,
                                           .arguments = {targetPath}};
    const QString sizeArgument = new_size_argument.trimmed();
    if (!sizeArgument.isEmpty()) {
        command.arguments.append(sizeArgument);
    }
    appendTargetBlockers(target_path, &command.blockers);
    appendDestructiveConfirmationBlocker(destructive_confirmed, &command.blockers);
    return command;
}

PartitionFileSystemToolResolution PartitionFileSystemToolRunner::resolveApprovedTool(
    const QString& manifest_path,
    const QString& tools_root,
    const QString& tool_id,
    const QString& operation,
    const QString& file_system) {
    PartitionFileSystemToolResolution resolution;
    const auto manifest =
        PartitionFileSystemToolManifest::validateRequiredTool(manifest_path, tools_root, tool_id);
    if (!manifest.ok) {
        resolution.blockers = manifest.errors;
        return resolution;
    }
    const auto tool = findTool(manifest.tools, tool_id);
    if (!tool.has_value()) {
        resolution.blockers.append(
            QStringLiteral("Required filesystem tool is not manifest-approved: %1").arg(tool_id));
        return resolution;
    }
    if (!containsToken(tool->operations, operation)) {
        resolution.blockers.append(
            QStringLiteral("Tool '%1' does not approve operation '%2'").arg(tool_id, operation));
    }
    if (!containsToken(tool->file_systems, file_system)) {
        resolution.blockers.append(
            QStringLiteral("Tool '%1' does not approve file system '%2'").arg(tool_id,
                                                                              file_system));
    }
    if (!resolution.blockers.isEmpty()) {
        return resolution;
    }

    resolution.tool = *tool;
    resolution.tool_path =
        QFileInfo(QDir(tools_root).filePath(tool->relative_path)).absoluteFilePath();
    resolution.ok = true;
    return resolution;
}

PartitionFileSystemToolRunResult PartitionFileSystemToolRunner::runReadOnlyCheck(
    const PartitionFileSystemReadOnlyCheckRequest& request, const CancelCheck& should_cancel) {
    PartitionFileSystemToolRunResult result;
    const auto command = buildReadOnlyCheckCommand(request.file_system, request.target_path);
    result.tool_id = command.tool_id;
    result.arguments = command.arguments;
    if (!command.ok()) {
        result.blocked = true;
        result.blockers = command.blockers;
        return result;
    }

    const auto resolution = resolveApprovedTool(request.manifest_path,
                                                request.tools_root,
                                                command.tool_id,
                                                command.operation,
                                                command.file_system);
    result.tool_path = resolution.tool_path;
    if (!resolution.ok) {
        result.blocked = true;
        result.blockers = resolution.blockers;
        return result;
    }

    const ProcessResult process = runProcess(resolution.tool_path,
                                             command.arguments,
                                             effectiveTimeoutMs(request.timeout_ms),
                                             should_cancel);
    result.success = process.succeeded();
    result.exit_code = process.exit_code;
    result.timed_out = process.timed_out;
    result.cancelled = process.cancelled;
    result.std_out = process.std_out;
    result.std_err = process.std_err;
    return result;
}

}  // namespace sak

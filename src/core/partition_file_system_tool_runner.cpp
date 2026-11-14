// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_file_system_tool_runner.cpp
/// @brief Manifest-gated external filesystem tool runner for Partition Manager.

#include "sak/partition_file_system_tool_runner.h"

#include "sak/layout_constants.h"
#include "sak/partition_manager_types.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

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

// e2fsck's exit code is a bit mask (e2fsck(8)): 4 errors left uncorrected, 8 operational error,
// 32 cancelled by the user. 1 and 2 (corrected) are an accepted code only for a repair.
constexpr int kE2fsckErrorsLeft = 4;
constexpr int kE2fsckOperationalError = 8;
constexpr int kE2fsckCancelled = 32;
// What e2fsck prints when the superblock says the file system is larger than its device, before it
// asks "Abort?". A check answers no and goes on, and exits 0 (operation proof, 2026-11-11).
constexpr auto kE2fsckCorruptLine = "the partition table is likely to be corrupt";
constexpr auto kE2fsckCorruptMessage =
    "e2fsck says the superblock or the partition table is likely corrupt: the file system is "
    "larger than its partition. It went on only because a check answers no to every question, so "
    "the file system did not pass. Put the partition back to the file system's size, or restore "
    "the file system from a backup.";
constexpr auto kE2fsckAbortMessage =
    "e2fsck asked to stop because of a serious problem (\"Abort?\"). It went on only because a "
    "check answers no to every question, so the file system did not pass.";
constexpr auto kE2fsckOperationalMessage =
    "e2fsck could not check the file system (an operational error, exit code {0}).";
constexpr auto kE2fsckErrorsLeftMessage =
    "e2fsck found errors in the file system and left them uncorrected (exit code {0}).";
constexpr auto kE2fsckCancelledMessage = "e2fsck was stopped before it finished (exit code {0}).";
constexpr auto kE2fsckOtherMessage =
    "e2fsck ended with exit code {0}, which this run does not accept.";
constexpr auto kOpenFailureMessage =
    "S.A.K. could not open the partition: {0}. The check read nothing, so this says nothing about "
    "the file system.";

QString e2fsckExitMessage(const char* message, int exit_code) {
    return QString::fromLatin1(message).replace(QStringLiteral("{0}"), QString::number(exit_code));
}

QString quotedPowerShell(const char* text) {
    return QStringLiteral("'") +
           QString::fromLatin1(text).replace(QStringLiteral("'"), QStringLiteral("''")) +
           QStringLiteral("'");
}

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
    const QString trimmed = targetPath.trimmed();
    if (trimmed.isEmpty()) {
        blockers->append(QStringLiteral("Target device or image path is required"));
    }
    if (containsUnsafeText(targetPath)) {
        blockers->append(QStringLiteral("Target path contains unsafe control characters"));
    }
    // A real device or image path never begins with '-'. Reject one that does so it can never
    // be smuggled to fsck as an option instead of the positional target (argument injection);
    // this is tool-agnostic, unlike a '--' separator that not every bundled fsck accepts.
    if (trimmed.startsWith(QLatin1Char('-'))) {
        blockers->append(QStringLiteral("Target path must not begin with '-'"));
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
        PartitionFileSystemToolCommand command{
            .tool_id = QString::fromLatin1(kE2fsckToolId),
            .operation = readOnlyCheckOperation(),
            .file_system = fileSystem,
            .arguments = {QStringLiteral("-n"), QStringLiteral("-f"), targetPath},
            .blockers = {}};
        appendTargetBlockers(target_path, &command.blockers);
        return command;
    }
    if (fileSystem == QString::fromLatin1(kXfsFileSystem)) {
        PartitionFileSystemToolCommand command{.tool_id = QString::fromLatin1(kXfsRepairToolId),
                                               .operation = readOnlyCheckOperation(),
                                               .file_system = fileSystem,
                                               .arguments = {QStringLiteral("-n"), targetPath},
                                               .blockers = {}};
        appendTargetBlockers(target_path, &command.blockers);
        return command;
    }
    if (fileSystem == QString::fromLatin1(kBtrfsFileSystem)) {
        PartitionFileSystemToolCommand command{
            .tool_id = QString::fromLatin1(kBtrfsToolId),
            .operation = readOnlyCheckOperation(),
            .file_system = fileSystem,
            .arguments = {QStringLiteral("check"), QStringLiteral("--readonly"), targetPath},
            .blockers = {}};
        appendTargetBlockers(target_path, &command.blockers);
        return command;
    }
    if (isHfsFileSystem(fileSystem)) {
        const QString hfsTargetPath = hfsToolTargetPath(targetPath);
        PartitionFileSystemToolCommand command{
            .tool_id = QString::fromLatin1(kFsckHfsToolId),
            .operation = readOnlyCheckOperation(),
            .file_system = normalizedHfsToolFileSystem(fileSystem),
            .arguments = {QStringLiteral("-n"), QStringLiteral("-f"), hfsTargetPath},
            .blockers = {}};
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
        PartitionFileSystemToolCommand command{.tool_id = QString::fromLatin1(kNewfsHfsToolId),
                                               .operation = formatOperation(),
                                               .file_system =
                                                   normalizedHfsToolFileSystem(fileSystem),
                                               .arguments = {},
                                               .blockers = {}};
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

    PartitionFileSystemToolCommand command{
        .tool_id = QString::fromLatin1(kMke2fsToolId),
        .operation = formatOperation(),
        .file_system = fileSystem,
        .arguments = {QStringLiteral("-t"), fileSystem, QStringLiteral("-F")},
        .blockers = {}};
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
            .arguments = {QStringLiteral("-p"), QStringLiteral("-f"), hfsTargetPath},
            .blockers = {}};
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

    PartitionFileSystemToolCommand command{
        .tool_id = QString::fromLatin1(kE2fsckToolId),
        .operation = repairOperation(),
        .file_system = fileSystem,
        .arguments = {QStringLiteral("-p"), QStringLiteral("-f"), targetPath},
        .blockers = {}};
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
                                           .arguments = {targetPath},
                                           .blockers = {}};
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
        resolution.blockers.append(QStringLiteral("Tool '%1' does not approve file system '%2'")
                                       .arg(tool_id, file_system));
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
    if (command.tool_id == QString::fromLatin1(kE2fsckToolId) && !process.timed_out &&
        !process.cancelled) {
        // A read-only check accepts 0 only: it corrects nothing.
        result.problem = e2fsckProblem(process.exit_code,
                                       process.std_out + QLatin1Char('\n') + process.std_err,
                                       {0});
    } else if (!process.timed_out && !process.cancelled) {
        result.problem = openFailureProblem(process.std_out + QLatin1Char('\n') + process.std_err);
    }
    result.success = result.success && result.problem.isEmpty();
    return result;
}

QString PartitionFileSystemToolRunner::e2fsckProblem(int exit_code,
                                                     const QString& output,
                                                     const QList<int>& accepted_exit_codes) {
    if (const QString open_failure = openFailureProblem(output); !open_failure.isEmpty()) {
        return open_failure;
    }
    if (output.contains(QLatin1String(kE2fsckCorruptLine), Qt::CaseInsensitive)) {
        return QString::fromLatin1(kE2fsckCorruptMessage);
    }
    static const QRegularExpression kAbortAnsweredNo(QStringLiteral("^\\s*Abort\\? no\\b"),
                                                     QRegularExpression::MultilineOption |
                                                         QRegularExpression::CaseInsensitiveOption);
    if (kAbortAnsweredNo.match(output).hasMatch()) {
        return QString::fromLatin1(kE2fsckAbortMessage);
    }
    if (accepted_exit_codes.contains(exit_code)) {
        return {};
    }
    if (exit_code < 0) {
        return e2fsckExitMessage(kE2fsckOtherMessage, exit_code);
    }
    if ((exit_code & kE2fsckOperationalError) != 0) {
        return e2fsckExitMessage(kE2fsckOperationalMessage, exit_code);
    }
    if ((exit_code & kE2fsckErrorsLeft) != 0) {
        return e2fsckExitMessage(kE2fsckErrorsLeftMessage, exit_code);
    }
    if ((exit_code & kE2fsckCancelled) != 0) {
        return e2fsckExitMessage(kE2fsckCancelledMessage, exit_code);
    }
    return e2fsckExitMessage(kE2fsckOtherMessage, exit_code);
}

QString PartitionFileSystemToolRunner::openFailureProblem(const QString& output) {
    // e2fsprogs: "e2fsck: <reason> while trying to open <device>". Other tools: "<tool>: can't
    // open <device>: <reason>".
    static const QRegularExpression kWhileTryingToOpen(
        QStringLiteral(R"(^\s*[\w.+-]+: (.+?) while trying to open\b)"),
        QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression kCannotOpen(
        QStringLiteral(
            R"(^\s*[\w.+-]+: (?:can\x27?t|cannot|could not|unable to) open [^:\r\n]+: (.+)$)"),
        QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
    QString reason;
    if (const auto found = kWhileTryingToOpen.match(output); found.hasMatch()) {
        reason = found.captured(1);
    } else if (const auto other = kCannotOpen.match(output); other.hasMatch()) {
        reason = other.captured(1);
    }
    reason = reason.trimmed();
    while (reason.endsWith(QLatin1Char('.'))) {
        reason.chop(1);
    }
    return reason.isEmpty()
               ? QString()
               : QString::fromLatin1(kOpenFailureMessage).replace(QStringLiteral("{0}"), reason);
}

QString PartitionFileSystemToolRunner::openFailureFunctionScript() {
    return QStringLiteral(R"PS(
function Get-SakToolOpenFailure([string]$Output) {
  $reason = ''
  if ($Output -match '(?im)^\s*[\w.+-]+: (.+?) while trying to open\b') { $reason = $Matches[1] }
  elseif ($Output -match '(?im)^\s*[\w.+-]+: (?:can\x27?t|cannot|could not|unable to) open [^:\r\n]+: (.+)$') { $reason = $Matches[1] }
  $reason = $reason.Trim().TrimEnd('.')
  if (-not $reason) { return '' }
  return (%1 -f $reason)
}
)PS")
        .arg(quotedPowerShell(kOpenFailureMessage));
}

QString PartitionFileSystemToolRunner::e2fsckProblemFunctionScript() {
    // The same order as e2fsckProblem: an open failure, the output, then the exit code.
    return openFailureFunctionScript() +
           QStringLiteral(
               "function Get-SakE2fsckProblem([int]$ExitCode, [string]$Output, [int[]]$Accepted) "
               "{\n"
               "  $openFailure = Get-SakToolOpenFailure $Output\n"
               "  if ($openFailure) { return $openFailure }\n"
               "  if ($Output -match %1) { return %2 }\n"
               "  if ($Output -match '(?im)^\\s*Abort\\? no\\b') { return %3 }\n"
               "  if ($Accepted -contains $ExitCode) { return '' }\n"
               "  if ($ExitCode -lt 0) { return (%4 -f $ExitCode) }\n"
               "  if ($ExitCode -band %5) { return (%6 -f $ExitCode) }\n"
               "  if ($ExitCode -band %7) { return (%8 -f $ExitCode) }\n"
               "  if ($ExitCode -band %9) { return (%10 -f $ExitCode) }\n"
               "  return (%4 -f $ExitCode)\n"
               "}\n")
               .arg(quotedPowerShell(kE2fsckCorruptLine),
                    quotedPowerShell(kE2fsckCorruptMessage),
                    quotedPowerShell(kE2fsckAbortMessage),
                    quotedPowerShell(kE2fsckOtherMessage),
                    QString::number(kE2fsckOperationalError),
                    quotedPowerShell(kE2fsckOperationalMessage),
                    QString::number(kE2fsckErrorsLeft),
                    quotedPowerShell(kE2fsckErrorsLeftMessage),
                    QString::number(kE2fsckCancelled))
               .arg(quotedPowerShell(kE2fsckCancelledMessage));
}

}  // namespace sak

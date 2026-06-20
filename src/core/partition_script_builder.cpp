// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_script_builder.cpp
/// @brief PowerShell script generation for Partition Manager.

#include "sak/partition_script_builder.h"

#include "sak/partition_file_system_tool_runner.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QStringList>

#include <algorithm>
#include <optional>

namespace sak {

namespace {

constexpr uint64_t kCloneIoBufferBytes = 1024ULL * 1024ULL;
constexpr uint64_t kKilobyteBytes = 1024ULL;
constexpr uint64_t kMinimumExtShrinkTargetBytes = 16ULL * 1024ULL * 1024ULL;
constexpr uint64_t kLinuxSwapDefaultPageSize = 4ULL * kKilobyteBytes;
constexpr uint64_t kLinuxSwap8KbPageSize = 8ULL * kKilobyteBytes;
constexpr uint64_t kLinuxSwap16KbPageSize = 16ULL * kKilobyteBytes;
constexpr uint64_t kLinuxSwap64KbPageSize = 64ULL * kKilobyteBytes;
constexpr uint64_t kLinuxSwapSupportedPageSizes[] = {kLinuxSwapDefaultPageSize,
                                                     kLinuxSwap8KbPageSize,
                                                     kLinuxSwap16KbPageSize,
                                                     kLinuxSwap64KbPageSize};
constexpr uint64_t kLinuxSwapMinPages = 2ULL;
constexpr uint64_t kLinuxSwapMaxPages = 0xFF'FF'FF'FFULL;
constexpr qsizetype kLinuxSwapLabelMaxChars = 16;
constexpr uint64_t kAllocationUnitDefaultBytes = 0;
constexpr uint64_t kAllocationUnit512Bytes = 512;
constexpr uint64_t kAllocationUnit1KbBytes = 1024;
constexpr uint64_t kAllocationUnit2KbBytes = 2 * 1024;
constexpr uint64_t kAllocationUnit4KbBytes = 4 * 1024;
constexpr uint64_t kAllocationUnit8KbBytes = 8 * 1024;
constexpr uint64_t kAllocationUnit16KbBytes = 16 * 1024;
constexpr uint64_t kAllocationUnit32KbBytes = 32 * 1024;
constexpr uint64_t kAllocationUnit64KbBytes = 64 * 1024;
constexpr qsizetype kDiskPartLabelMaxChars = 32;
constexpr uint64_t kMinimumDiskPartSizeMb = 1;
constexpr uint64_t kHfsStagedCopyBufferBytes = 4ULL * 1024ULL * 1024ULL;
constexpr uint64_t kHfsStaleSignatureClearBytes = 16ULL * 1024ULL * 1024ULL;
constexpr auto kNonNativeFileSystemToolPayload = "non_native_file_system_tool";
constexpr auto kTargetWipeConfirmedPayload = "target_wipe_confirmed";
constexpr auto kApfsRootFileNamePayload = "apfs_root_file_name";
constexpr auto kApfsRootDirectoryNamePayload = "apfs_root_directory_name";
constexpr auto kApfsRootFilePayloadBase64 = "apfs_root_file_payload_base64";
constexpr auto kApfsRootFilePayloadText = "apfs_root_file_payload_text";
constexpr auto kApfsRootFilePatchOffsetPayload = "apfs_root_file_patch_offset_bytes";
constexpr auto kApfsGeneratedLayoutConfirmedPayload = "apfs_generated_layout_confirmed";
constexpr uint64_t kMinimumApfsGeneratedRawFormatBytes = 64ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaximumApfsGeneratedSingleChunkBytes = 128ULL * 1024ULL * 1024ULL;
// A1/A2 (multi-CIB + CAB spaceman) FORMAT is Apple-certified: the writer emits single-CIB,
// multi-CIB, metadata-overflow, AND the CAB tier (cab_count > 0, the spaceman publishes a
// cab-address array pointing at apfs_cib_addr_blocks). macOS fsck_apfs reports an 8 TiB
// CAB container (cab_count 2) "appears to be OK" and the APFS kernel extension mounts it
// read-write and commits to it; apfsprogs apfsck validates cab_count 2/3/4 clean through
// 24 TiB. This pre-check stays inside that certified range (and below the writer's own
// ~48 TiB ip-bitmap-ring ceiling, which is the precise fail-closed authority). In-place
// root-file mutation stays single-chunk (kMaximumApfsGeneratedSingleChunkBytes) until the
// A2 multi-CIB/CAB in-place commit path is wired into the production write commands.
constexpr uint64_t kMaximumApfsGeneratedMultiCibFormatBytes = 24ULL * 1024ULL * 1024ULL * 1024ULL *
                                                              1024ULL;
constexpr qsizetype kApfsVolumeLabelMaxChars = 255;
constexpr qsizetype kApfsVolumeLabelFieldBytes = 256;
constexpr auto kHfsPathPayload = "hfs_path";
constexpr auto kHfsDestinationPathPayload = "hfs_destination_path";
constexpr auto kHfsPayloadBase64 = "hfs_payload_base64";
constexpr auto kHfsPayloadText = "hfs_payload_text";
constexpr auto kHfsFileIdPayload = "hfs_file_id";
constexpr auto kHfsAttributeNamePayload = "hfs_attribute_name";
constexpr auto kHfsAllowJournaledVolumePayload = "hfs_allow_journaled_volume";
constexpr auto kHfsAllowWrappedVolumePayload = "hfs_allow_wrapped_volume";
constexpr auto kHfsSecureWipeReleasedBlocksPayload = "hfs_secure_wipe_released_blocks";
constexpr int kRuntimeManifestSourceFallbackDepth = 3;

QString uintArg(uint64_t value) {
    return QString::number(value);
}

QString commonHeader(const QString& title) {
    return QStringLiteral(
               "$ErrorActionPreference = 'Stop'\n"
               "Write-Output %1\n")
        .arg(PartitionScriptBuilder::quotePowerShell(QStringLiteral("SAK Partition Manager: ") +
                                                     title));
}

QString requirePartitionIdentity(uint32_t disk, uint32_t partition, uint64_t size) {
    return QStringLiteral(
               "$p = Get-Partition -DiskNumber %1 -PartitionNumber %2 -ErrorAction Stop\n"
               "if ([uint64]$p.Size -ne [uint64]%3) { throw 'Partition identity mismatch' }\n")
        .arg(disk)
        .arg(partition)
        .arg(uintArg(size));
}

QString payloadString(const PartitionOperation& operation,
                      const QString& key,
                      const QString& fallback = {}) {
    return operation.payload.value(key).toString(fallback);
}

uint64_t payloadUInt64(const PartitionOperation& operation, const QString& key) {
    const auto value = operation.payload.value(key);
    if (value.isDouble()) {
        return static_cast<uint64_t>(value.toDouble());
    }
    bool ok = false;
    const uint64_t parsed = value.toString().toULongLong(&ok);
    return ok ? parsed : 0;
}

bool payloadBool(const PartitionOperation& operation, const QString& key, bool fallback = false) {
    const auto value = operation.payload.value(key);
    if (value.isBool()) {
        return value.toBool();
    }
    const QString text = value.toString(fallback ? QStringLiteral("true") : QStringLiteral("false"))
                             .trimmed()
                             .toLower();
    return text == QStringLiteral("true") || text == QStringLiteral("1") ||
           text == QStringLiteral("yes");
}

bool isNonNativeFileSystemToolOperation(const PartitionOperation& operation) {
    return payloadBool(operation, QString::fromLatin1(kNonNativeFileSystemToolPayload));
}

bool isExtFileSystemToken(const QString& fileSystem) {
    const QString token = fileSystem.trimmed().toLower();
    return token == QStringLiteral("ext2") || token == QStringLiteral("ext3") ||
           token == QStringLiteral("ext4");
}

bool isLinuxSwapFileSystemToken(const QString& fileSystem) {
    const QString token = fileSystem.trimmed().toLower();
    return token == QStringLiteral("linux swap") || token == QStringLiteral("linux-swap") ||
           token == QStringLiteral("swap");
}

bool isHfsFileSystemToken(const QString& fileSystem) {
    const QString token = fileSystem.trimmed().toLower();
    return token == QStringLiteral("hfs+") || token == QStringLiteral("hfsplus") ||
           token == QStringLiteral("hfsx");
}

bool isApfsFileSystemToken(const QString& fileSystem) {
    return fileSystem.trimmed().compare(QStringLiteral("apfs"), Qt::CaseInsensitive) == 0;
}

bool isSupportedNonNativeCreateFormatFileSystem(const QString& fileSystem) {
    return isExtFileSystemToken(fileSystem) || isHfsFileSystemToken(fileSystem) ||
           isLinuxSwapFileSystemToken(fileSystem) || isApfsFileSystemToken(fileSystem);
}

bool isSupportedLinuxSwapPageSize(uint64_t value) {
    for (const uint64_t pageSize : kLinuxSwapSupportedPageSizes) {
        if (value == pageSize) {
            return true;
        }
    }
    return false;
}

uint64_t linuxSwapPageSize(const PartitionOperation& operation) {
    const uint64_t requested = payloadUInt64(operation,
                                             QStringLiteral("linux_swap_page_size_bytes"));
    return requested == 0 ? kLinuxSwapDefaultPageSize : requested;
}

PartitionScript invalidPartitionScript(const QString& blocker) {
    PartitionScript script;
    script.blockers.append(blocker);
    return script;
}

QString linuxSwapFormatBodyScript(const QString& targetPathExpression,
                                  const QString& partitionSizeExpression,
                                  QString label,
                                  uint64_t pageSize) {
    label = label.trimmed().left(kLinuxSwapLabelMaxChars);
    return QStringLiteral(
               "$targetPath = %1\n"
               "$label = %2\n"
               "$pageSize = [int]%3\n"
               "$partitionSizeBytes = [uint64](%4)\n"
               "$pageCount = [uint64][Math]::Floor($partitionSizeBytes / $pageSize)\n"
               "if ($pageCount -lt 2) { throw 'Linux swap partition is too small' }\n"
               "if ($pageCount -gt [uint64]4294967295) { throw 'Linux swap page count exceeds v1 "
               "header limit' }\n"
               "$volume = $null\n"
               "try { $volume = $p | Get-Volume -ErrorAction Stop } catch { }\n"
               "if ($volume -and $volume.DriveLetter) {\n"
               "  Dismount-Volume -DriveLetter $volume.DriveLetter -Force -ErrorAction Stop\n"
               "}\n"
               "function Write-UInt32Le([byte[]]$Buffer, [int]$Offset, [uint32]$Value) {\n"
               "  $bytes = [BitConverter]::GetBytes($Value)\n"
               "  [Array]::Copy($bytes, 0, $Buffer, $Offset, 4)\n"
               "}\n"
               "$header = New-Object byte[] $pageSize\n"
               "Write-UInt32Le -Buffer $header -Offset 1024 -Value ([uint32]1)\n"
               "Write-UInt32Le -Buffer $header -Offset 1028 -Value ([uint32]($pageCount - 1))\n"
               "Write-UInt32Le -Buffer $header -Offset 1032 -Value ([uint32]0)\n"
               "$rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()\n"
               "$uuidBytes = New-Object byte[] 16\n"
               "try { $rng.GetBytes($uuidBytes) } finally { $rng.Dispose() }\n"
               "[Array]::Copy($uuidBytes, 0, $header, 1036, 16)\n"
               "$labelBytes = [System.Text.Encoding]::ASCII.GetBytes($label)\n"
               "$labelLength = [Math]::Min($labelBytes.Length, 16)\n"
               "if ($labelLength -gt 0) { [Array]::Copy($labelBytes, 0, $header, 1052, "
               "$labelLength) }\n"
               "$signatureBytes = [System.Text.Encoding]::ASCII.GetBytes('SWAPSPACE2')\n"
               "[Array]::Copy($signatureBytes, 0, $header, $pageSize - 10, 10)\n"
               "$stream = [System.IO.File]::Open($targetPath, [System.IO.FileMode]::Open, "
               "[System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::ReadWrite)\n"
               "try {\n"
               "  [void]$stream.Seek(0, [System.IO.SeekOrigin]::Begin)\n"
               "  $stream.Write($header, 0, $header.Length)\n"
               "  $stream.Flush()\n"
               "  $verify = New-Object byte[] $pageSize\n"
               "  [void]$stream.Seek(0, [System.IO.SeekOrigin]::Begin)\n"
               "  $read = $stream.Read($verify, 0, $verify.Length)\n"
               "  if ($read -lt $pageSize) { throw 'Linux swap header verify read was short' }\n"
               "  $actualSignature = [System.Text.Encoding]::ASCII.GetString($verify, "
               "$pageSize - 10, 10)\n"
               "  if ($actualSignature -ne 'SWAPSPACE2') { throw 'Linux swap signature "
               "verification failed' }\n"
               "  $actualVersion = [BitConverter]::ToUInt32($verify, 1024)\n"
               "  if ($actualVersion -ne 1) { throw 'Linux swap version verification failed' }\n"
               "} finally {\n"
               "  $stream.Dispose()\n"
               "}\n"
               "Update-HostStorageCache -ErrorAction SilentlyContinue\n"
               "Write-Output ('Linux swap header written: page_size={0}; pages={1}; label={2}' -f "
               "$pageSize, $pageCount, $label)\n")
        .arg(targetPathExpression,
             PartitionScriptBuilder::quotePowerShell(label),
             uintArg(pageSize),
             partitionSizeExpression);
}

QString rawPartitionTargetPath(const PartitionOperation& operation) {
    if (operation.target.partition_number == 0) {
        return {};
    }
    const QString expected = QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk%1\\Partition%2")
                                 .arg(operation.target.disk_number)
                                 .arg(operation.target.partition_number);
    QString payloadPath = payloadString(operation, QStringLiteral("target_path")).trimmed();
    payloadPath.replace('/', '\\');
    if (payloadPath.compare(expected, Qt::CaseInsensitive) != 0) {
        return {};
    }
    return expected;
}

QString rawPartitionTargetPathBlocker(const PartitionOperation& operation) {
    if (operation.target.partition_number == 0 || operation.target.size_bytes == 0) {
        return QStringLiteral("Filesystem tool operation requires a partition identity");
    }
    if (payloadString(operation, QStringLiteral("target_path")).trimmed().isEmpty()) {
        return QStringLiteral("Filesystem tool operation requires a raw partition target path");
    }
    if (rawPartitionTargetPath(operation).isEmpty()) {
        return QStringLiteral("Filesystem tool target path must match the selected raw partition");
    }
    return {};
}

PartitionScript buildLinuxSwapFormatScript(const PartitionOperation& operation, QString label) {
    const QString targetPath = rawPartitionTargetPath(operation);
    const uint64_t pageSize = linuxSwapPageSize(operation);
    if (operation.target.partition_number == 0 || operation.target.size_bytes == 0) {
        return invalidPartitionScript(
            QStringLiteral("Linux swap format requires a partition identity"));
    }
    const QString targetBlocker = rawPartitionTargetPathBlocker(operation);
    if (!targetBlocker.isEmpty()) {
        return invalidPartitionScript(targetBlocker);
    }
    if (targetPath.trimmed().isEmpty()) {
        return invalidPartitionScript(
            QStringLiteral("Linux swap format requires a raw target path"));
    }
    if (!payloadBool(operation, QString::fromLatin1(kTargetWipeConfirmedPayload))) {
        return invalidPartitionScript(
            QStringLiteral("Linux swap format requires destructive confirmation"));
    }
    if (!isSupportedLinuxSwapPageSize(pageSize)) {
        return invalidPartitionScript(QStringLiteral("Unsupported Linux swap page size"));
    }
    const uint64_t pageCount = operation.target.size_bytes / pageSize;
    if (pageCount < kLinuxSwapMinPages || pageCount > kLinuxSwapMaxPages) {
        return invalidPartitionScript(
            QStringLiteral("Linux swap partition size is outside supported bounds"));
    }

    PartitionScript out;
    out.preview = QStringLiteral("Format Disk %1 Partition %2 as Linux swap")
                      .arg(operation.target.disk_number)
                      .arg(operation.target.partition_number);
    out.script = commonHeader(out.preview) +
                 requirePartitionIdentity(operation.target.disk_number,
                                          operation.target.partition_number,
                                          operation.target.size_bytes) +
                 linuxSwapFormatBodyScript(PartitionScriptBuilder::quotePowerShell(targetPath),
                                           QStringLiteral("[uint64]$p.Size"),
                                           label,
                                           pageSize);
    out.dry_run_script = out.preview + QStringLiteral("\nWrite SWAPSPACE2 v1 header to ") +
                         PartitionScriptBuilder::quotePowerShell(targetPath);
    out.timeout_seconds = kPartitionFormatTaskTimeoutSeconds;
    return out;
}

QString apfsWriterCliFunctionScript();
QString dismountSelectedPartitionVolumeScript();

PartitionScript buildApfsRawFormatScript(const PartitionOperation& operation,
                                         const QString& label) {
    const QString targetBlocker = rawPartitionTargetPathBlocker(operation);
    if (!targetBlocker.isEmpty()) {
        return invalidPartitionScript(targetBlocker);
    }
    if (!payloadBool(operation, QString::fromLatin1(kTargetWipeConfirmedPayload))) {
        return invalidPartitionScript(
            QStringLiteral("APFS format requires destructive confirmation"));
    }
    if (operation.target.size_bytes < kMinimumApfsGeneratedRawFormatBytes ||
        operation.target.size_bytes > kMaximumApfsGeneratedMultiCibFormatBytes) {
        return invalidPartitionScript(QStringLiteral(
            "APFS generated raw format supports single-CIB, multi-CIB, metadata-overflow, and "
            "CAB-tier targets from 64 MiB through 24 TiB; larger targets remain blocked"));
    }

    const QString targetPath = rawPartitionTargetPath(operation);
    PartitionScript out;
    out.preview = QStringLiteral("Format Disk %1 Partition %2 as APFS with S.A.K. APFS writer")
                      .arg(operation.target.disk_number)
                      .arg(operation.target.partition_number);
    out.script = commonHeader(out.preview) +
                 requirePartitionIdentity(operation.target.disk_number,
                                          operation.target.partition_number,
                                          operation.target.size_bytes) +
                 dismountSelectedPartitionVolumeScript() + apfsWriterCliFunctionScript() +
                 QStringLiteral(
                     "$targetPath = %1\n"
                     "$targetSizeBytes = [uint64]$p.Size\n"
                     "Invoke-SakApfsWriterCli -Command 'format-raw' -TargetPath $targetPath "
                     "-SizeBytes $targetSizeBytes -VolumeName %2 -EvidenceId "
                     "'ui.apfs-generated-raw-format'\n"
                     "Update-HostStorageCache -ErrorAction SilentlyContinue\n")
                     .arg(PartitionScriptBuilder::quotePowerShell(targetPath),
                          PartitionScriptBuilder::quotePowerShell(label));
    out.dry_run_script = out.preview + QStringLiteral("\nRun sak_apfs_writer_cli.exe format-raw ") +
                         PartitionScriptBuilder::quotePowerShell(targetPath);
    out.timeout_seconds = kPartitionFormatTaskTimeoutSeconds;
    return out;
}

PartitionScript buildApfsRawRepairScript(const PartitionOperation& operation,
                                         const QString& fileSystem) {
    const QString targetBlocker = rawPartitionTargetPathBlocker(operation);
    if (!targetBlocker.isEmpty()) {
        return invalidPartitionScript(targetBlocker);
    }
    if (!payloadBool(operation, QString::fromLatin1(kTargetWipeConfirmedPayload))) {
        return invalidPartitionScript(
            QStringLiteral("APFS repair requires destructive confirmation"));
    }
    if (operation.target.size_bytes < kMinimumApfsGeneratedRawFormatBytes ||
        operation.target.size_bytes > kMaximumApfsGeneratedMultiCibFormatBytes) {
        return invalidPartitionScript(QStringLiteral(
            "APFS generated raw repair supports single-CIB, multi-CIB, metadata-overflow, and "
            "CAB-tier targets from 64 MiB through 24 TiB; larger targets remain blocked"));
    }

    const QString targetPath = rawPartitionTargetPath(operation);
    PartitionScript out;
    out.preview =
        QStringLiteral(
            "Repair Disk %1 Partition %2 %3 generated metadata checksums with S.A.K. APFS writer")
            .arg(operation.target.disk_number)
            .arg(operation.target.partition_number)
            .arg(fileSystem.toUpper());
    out.script = commonHeader(out.preview) +
                 requirePartitionIdentity(operation.target.disk_number,
                                          operation.target.partition_number,
                                          operation.target.size_bytes) +
                 dismountSelectedPartitionVolumeScript() + apfsWriterCliFunctionScript() +
                 QStringLiteral(
                     "$targetPath = %1\n"
                     "$targetSizeBytes = [uint64]$p.Size\n"
                     "Invoke-SakApfsWriterCli -Command 'repair-raw' -TargetPath $targetPath "
                     "-SizeBytes $targetSizeBytes -VolumeName '' -EvidenceId "
                     "'ui.apfs-generated-raw-repair'\n"
                     "Update-HostStorageCache -ErrorAction SilentlyContinue\n")
                     .arg(PartitionScriptBuilder::quotePowerShell(targetPath));
    out.dry_run_script = out.preview + QStringLiteral("\nRun sak_apfs_writer_cli.exe repair-raw ") +
                         PartitionScriptBuilder::quotePowerShell(targetPath);
    out.timeout_seconds = kPartitionFormatTaskTimeoutSeconds;
    return out;
}

bool isApfsRootFileMutationOperation(PartitionOperationType type) {
    return type == PartitionOperationType::ApfsWriteRootFile ||
           type == PartitionOperationType::ApfsPatchRootFile ||
           type == PartitionOperationType::ApfsPatchRootDirectoryFile ||
           type == PartitionOperationType::ApfsDeleteRootFile ||
           type == PartitionOperationType::ApfsWriteRootDirectoryFile ||
           type == PartitionOperationType::ApfsDeleteRootDirectoryFile ||
           type == PartitionOperationType::ApfsCreateRootDirectory ||
           type == PartitionOperationType::ApfsDeleteRootDirectory ||
           type == PartitionOperationType::ApfsChangeVolumeLabel;
}

bool apfsRootFileMutationWritesPayload(PartitionOperationType type) {
    return type == PartitionOperationType::ApfsWriteRootFile ||
           type == PartitionOperationType::ApfsWriteRootDirectoryFile ||
           type == PartitionOperationType::ApfsPatchRootDirectoryFile ||
           type == PartitionOperationType::ApfsPatchRootFile;
}

bool isHfsFileMutationOperation(PartitionOperationType type) {
    static const QHash<int, bool> kTypes{
        {static_cast<int>(PartitionOperationType::HfsOverwriteFile), true},
        {static_cast<int>(PartitionOperationType::HfsReplaceFile), true},
        {static_cast<int>(PartitionOperationType::HfsGrowFile), true},
        {static_cast<int>(PartitionOperationType::HfsTruncateFile), true},
        {static_cast<int>(PartitionOperationType::HfsReplaceResourceFork), true},
        {static_cast<int>(PartitionOperationType::HfsGrowResourceFork), true},
        {static_cast<int>(PartitionOperationType::HfsTruncateResourceFork), true},
        {static_cast<int>(PartitionOperationType::HfsCreateEmptyFile), true},
        {static_cast<int>(PartitionOperationType::HfsCreateFile), true},
        {static_cast<int>(PartitionOperationType::HfsDeleteEmptyFile), true},
        {static_cast<int>(PartitionOperationType::HfsDeleteFile), true},
        {static_cast<int>(PartitionOperationType::HfsCreateEmptyFolder), true},
        {static_cast<int>(PartitionOperationType::HfsDeleteEmptyFolder), true},
        {static_cast<int>(PartitionOperationType::HfsDeleteFolderTree), true},
        {static_cast<int>(PartitionOperationType::HfsRenameMoveCatalogEntry), true},
        {static_cast<int>(PartitionOperationType::HfsReplaceInlineAttribute), true},
        {static_cast<int>(PartitionOperationType::HfsReplaceForkAttribute), true},
        {static_cast<int>(PartitionOperationType::HfsGrowForkAttribute), true},
    };
    return kTypes.contains(static_cast<int>(type));
}

bool hfsFileMutationWritesPayload(PartitionOperationType type) {
    return type == PartitionOperationType::HfsOverwriteFile ||
           type == PartitionOperationType::HfsReplaceFile ||
           type == PartitionOperationType::HfsGrowFile ||
           type == PartitionOperationType::HfsCreateFile ||
           type == PartitionOperationType::HfsReplaceResourceFork ||
           type == PartitionOperationType::HfsGrowResourceFork ||
           type == PartitionOperationType::HfsReplaceInlineAttribute ||
           type == PartitionOperationType::HfsReplaceForkAttribute ||
           type == PartitionOperationType::HfsGrowForkAttribute;
}

bool hfsFileMutationNeedsPath(PartitionOperationType type) {
    return type != PartitionOperationType::HfsReplaceInlineAttribute &&
           type != PartitionOperationType::HfsReplaceForkAttribute &&
           type != PartitionOperationType::HfsGrowForkAttribute;
}

bool hfsFileMutationIsAttribute(PartitionOperationType type) {
    return type == PartitionOperationType::HfsReplaceInlineAttribute ||
           type == PartitionOperationType::HfsReplaceForkAttribute ||
           type == PartitionOperationType::HfsGrowForkAttribute;
}

bool hfsFileMutationSupportsSecureWipe(PartitionOperationType type) {
    return type == PartitionOperationType::HfsDeleteFile ||
           type == PartitionOperationType::HfsDeleteFolderTree;
}

QString apfsRootFileMutationCommand(PartitionOperationType type) {
    switch (type) {
    // File/directory create/delete/patch route onto the certified crash-safe COW engine
    // (commit-raw-*); the COW patch is a true in-place byte-range patch that preserves the
    // file's object id. Volume-label change is not a checkpoint mutation, so it stays on
    // the legacy raw writer.
    case PartitionOperationType::ApfsWriteRootFile:
        return QStringLiteral("commit-raw-file-write");
    case PartitionOperationType::ApfsPatchRootFile:
    case PartitionOperationType::ApfsPatchRootDirectoryFile:
        return QStringLiteral("commit-raw-file-patch");
    case PartitionOperationType::ApfsDeleteRootFile:
        return QStringLiteral("commit-raw-file-delete");
    case PartitionOperationType::ApfsWriteRootDirectoryFile:
        return QStringLiteral("commit-raw-directory-child-write");
    case PartitionOperationType::ApfsDeleteRootDirectoryFile:
        return QStringLiteral("commit-raw-directory-child-delete");
    case PartitionOperationType::ApfsCreateRootDirectory:
        return QStringLiteral("commit-raw-directory-create");
    case PartitionOperationType::ApfsDeleteRootDirectory:
        return QStringLiteral("commit-raw-directory-delete");
    case PartitionOperationType::ApfsChangeVolumeLabel:
        return QStringLiteral("change-raw-volume-label");
    default:
        return {};
    }
}

QString apfsRootFileMutationEvidenceId(PartitionOperationType type) {
    switch (type) {
    case PartitionOperationType::ApfsWriteRootFile:
        return QStringLiteral("ui.apfs-generated-raw-root-file-write");
    case PartitionOperationType::ApfsPatchRootFile:
        return QStringLiteral("ui.apfs-generated-raw-root-file-patch");
    case PartitionOperationType::ApfsPatchRootDirectoryFile:
        return QStringLiteral("ui.apfs-generated-raw-root-directory-file-patch");
    case PartitionOperationType::ApfsDeleteRootFile:
        return QStringLiteral("ui.apfs-generated-raw-root-file-delete");
    case PartitionOperationType::ApfsWriteRootDirectoryFile:
        return QStringLiteral("ui.apfs-generated-raw-root-directory-file-write");
    case PartitionOperationType::ApfsDeleteRootDirectoryFile:
        return QStringLiteral("ui.apfs-generated-raw-root-directory-file-delete");
    case PartitionOperationType::ApfsCreateRootDirectory:
        return QStringLiteral("ui.apfs-generated-raw-root-directory-create");
    case PartitionOperationType::ApfsDeleteRootDirectory:
        return QStringLiteral("ui.apfs-generated-raw-root-directory-delete");
    case PartitionOperationType::ApfsChangeVolumeLabel:
        return QStringLiteral("ui.apfs-generated-raw-volume-label-change");
    default:
        return {};
    }
}

QString apfsRootFileMutationVerb(PartitionOperationType type) {
    switch (type) {
    case PartitionOperationType::ApfsWriteRootFile:
        return QStringLiteral("Write");
    case PartitionOperationType::ApfsPatchRootFile:
        return QStringLiteral("Patch");
    case PartitionOperationType::ApfsPatchRootDirectoryFile:
        return QStringLiteral("Patch directory file");
    case PartitionOperationType::ApfsDeleteRootFile:
        return QStringLiteral("Delete");
    case PartitionOperationType::ApfsWriteRootDirectoryFile:
        return QStringLiteral("Write directory file");
    case PartitionOperationType::ApfsDeleteRootDirectoryFile:
        return QStringLiteral("Delete directory file");
    case PartitionOperationType::ApfsCreateRootDirectory:
        return QStringLiteral("Create directory");
    case PartitionOperationType::ApfsDeleteRootDirectory:
        return QStringLiteral("Delete directory");
    case PartitionOperationType::ApfsChangeVolumeLabel:
        return QStringLiteral("Change volume label");
    default:
        return {};
    }
}

bool apfsVolumeLabelMutation(PartitionOperationType type) {
    return type == PartitionOperationType::ApfsChangeVolumeLabel;
}

bool apfsRootDirectoryMutation(PartitionOperationType type) {
    return type == PartitionOperationType::ApfsCreateRootDirectory ||
           type == PartitionOperationType::ApfsDeleteRootDirectory;
}

bool apfsRootDirectoryFileMutation(PartitionOperationType type) {
    return type == PartitionOperationType::ApfsWriteRootDirectoryFile ||
           type == PartitionOperationType::ApfsPatchRootDirectoryFile ||
           type == PartitionOperationType::ApfsDeleteRootDirectoryFile;
}

bool apfsRootDirectoryNameRequired(PartitionOperationType type) {
    return apfsRootDirectoryMutation(type) || apfsRootDirectoryFileMutation(type);
}

bool apfsRootFileNameRequired(PartitionOperationType type) {
    return isApfsRootFileMutationOperation(type) && !apfsRootDirectoryMutation(type) &&
           !apfsVolumeLabelMutation(type);
}

struct HfsFileMutationSpec {
    QString command;
    QString evidence_id;
    QString verb;
};

const QHash<int, HfsFileMutationSpec>& hfsFileMutationSpecs() {
    static const QHash<int, HfsFileMutationSpec> kSpecs{
        {static_cast<int>(PartitionOperationType::HfsOverwriteFile),
         {QStringLiteral("overwrite-image"),
          QStringLiteral("ui.hfs.raw-file-overwrite"),
          QStringLiteral("Overwrite")}},
        {static_cast<int>(PartitionOperationType::HfsReplaceFile),
         {QStringLiteral("replace-image"),
          QStringLiteral("ui.hfs.raw-file-replace"),
          QStringLiteral("Replace")}},
        {static_cast<int>(PartitionOperationType::HfsGrowFile),
         {QStringLiteral("grow-image"),
          QStringLiteral("ui.hfs.raw-file-allocation-growth"),
          QStringLiteral("Grow file")}},
        {static_cast<int>(PartitionOperationType::HfsTruncateFile),
         {QStringLiteral("truncate-image"),
          QStringLiteral("ui.hfs.raw-file-truncate"),
          QStringLiteral("Truncate")}},
        {static_cast<int>(PartitionOperationType::HfsReplaceResourceFork),
         {QStringLiteral("replace-resource-fork-image"),
          QStringLiteral("ui.hfs.raw-resource-fork-replace"),
          QStringLiteral("Replace resource fork")}},
        {static_cast<int>(PartitionOperationType::HfsGrowResourceFork),
         {QStringLiteral("grow-resource-fork-image"),
          QStringLiteral("ui.hfs.raw-resource-fork-allocation-growth"),
          QStringLiteral("Grow resource fork")}},
        {static_cast<int>(PartitionOperationType::HfsTruncateResourceFork),
         {QStringLiteral("truncate-resource-fork-image"),
          QStringLiteral("ui.hfs.raw-resource-fork-truncate"),
          QStringLiteral("Truncate resource fork")}},
        {static_cast<int>(PartitionOperationType::HfsCreateEmptyFile),
         {QStringLiteral("create-empty-file-image"),
          QStringLiteral("ui.hfs.raw-empty-file-create"),
          QStringLiteral("Create empty file")}},
        {static_cast<int>(PartitionOperationType::HfsCreateFile),
         {QStringLiteral("create-file-image"),
          QStringLiteral("ui.hfs.raw-file-create"),
          QStringLiteral("Create file")}},
        {static_cast<int>(PartitionOperationType::HfsDeleteEmptyFile),
         {QStringLiteral("delete-empty-file-image"),
          QStringLiteral("ui.hfs.raw-empty-file-delete"),
          QStringLiteral("Delete empty file")}},
        {static_cast<int>(PartitionOperationType::HfsDeleteFile),
         {QStringLiteral("delete-file-image"),
          QStringLiteral("ui.hfs.raw-file-delete"),
          QStringLiteral("Delete file")}},
        {static_cast<int>(PartitionOperationType::HfsCreateEmptyFolder),
         {QStringLiteral("create-empty-folder-image"),
          QStringLiteral("ui.hfs.raw-empty-folder-create"),
          QStringLiteral("Create empty folder")}},
        {static_cast<int>(PartitionOperationType::HfsDeleteEmptyFolder),
         {QStringLiteral("delete-empty-folder-image"),
          QStringLiteral("ui.hfs.raw-empty-folder-delete"),
          QStringLiteral("Delete empty folder")}},
        {static_cast<int>(PartitionOperationType::HfsDeleteFolderTree),
         {QStringLiteral("delete-folder-tree-image"),
          QStringLiteral("ui.hfs.raw-folder-tree-delete"),
          QStringLiteral("Delete folder tree")}},
        {static_cast<int>(PartitionOperationType::HfsRenameMoveCatalogEntry),
         {QStringLiteral("rename-catalog-entry-image"),
          QStringLiteral("ui.hfs.raw-catalog-rename-move"),
          QStringLiteral("Rename/move catalog entry")}},
        {static_cast<int>(PartitionOperationType::HfsReplaceInlineAttribute),
         {QStringLiteral("replace-inline-attribute-image"),
          QStringLiteral("ui.hfs.raw-inline-attribute-replace"),
          QStringLiteral("Replace inline attribute")}},
        {static_cast<int>(PartitionOperationType::HfsReplaceForkAttribute),
         {QStringLiteral("replace-fork-attribute-image"),
          QStringLiteral("ui.hfs.raw-fork-attribute-replace"),
          QStringLiteral("Replace fork attribute")}},
        {static_cast<int>(PartitionOperationType::HfsGrowForkAttribute),
         {QStringLiteral("grow-fork-attribute-image"),
          QStringLiteral("ui.hfs.raw-fork-attribute-allocation-growth"),
          QStringLiteral("Grow fork attribute")}}};
    return kSpecs;
}

QString hfsFileMutationCommand(PartitionOperationType type) {
    return hfsFileMutationSpecs().value(static_cast<int>(type)).command;
}

QString hfsFileMutationEvidenceId(PartitionOperationType type) {
    return hfsFileMutationSpecs().value(static_cast<int>(type)).evidence_id;
}

QString hfsFileMutationVerb(PartitionOperationType type) {
    return hfsFileMutationSpecs().value(static_cast<int>(type)).verb;
}

std::optional<QString> apfsRootFilePayloadBase64(const PartitionOperation& operation,
                                                 QString* blocker) {
    if (!apfsRootFileMutationWritesPayload(operation.type)) {
        return QString();
    }

    const QString explicitBase64 =
        payloadString(operation, QString::fromLatin1(kApfsRootFilePayloadBase64)).trimmed();
    if (!explicitBase64.isEmpty()) {
        QByteArray decoded = QByteArray::fromBase64(explicitBase64.toLatin1());
        if (decoded.isEmpty()) {
            *blocker = QStringLiteral("APFS root-file payload_base64 is empty or invalid");
            return std::nullopt;
        }
        return QString::fromLatin1(decoded.toBase64());
    }

    if (operation.payload.contains(QString::fromLatin1(kApfsRootFilePayloadText))) {
        const QByteArray bytes =
            payloadString(operation, QString::fromLatin1(kApfsRootFilePayloadText)).toUtf8();
        if (bytes.isEmpty()) {
            *blocker = QStringLiteral("APFS root-file payload text must not be empty");
            return std::nullopt;
        }
        return QString::fromLatin1(bytes.toBase64());
    }

    *blocker = QStringLiteral("APFS root-file payload is required");
    return std::nullopt;
}

std::optional<uint64_t> apfsRootFilePatchOffset(const PartitionOperation& operation,
                                                QString* blocker) {
    if (operation.type != PartitionOperationType::ApfsPatchRootFile &&
        operation.type != PartitionOperationType::ApfsPatchRootDirectoryFile) {
        return 0ULL;
    }
    if (!operation.payload.contains(QString::fromLatin1(kApfsRootFilePatchOffsetPayload))) {
        *blocker = QStringLiteral("APFS root-file patch offset is required");
        return std::nullopt;
    }
    return payloadUInt64(operation, QString::fromLatin1(kApfsRootFilePatchOffsetPayload));
}

std::optional<QString> hfsMutationPayloadBase64(const PartitionOperation& operation,
                                                QString* blocker) {
    if (!hfsFileMutationWritesPayload(operation.type)) {
        return QString();
    }

    const QString explicitBase64 =
        payloadString(operation, QString::fromLatin1(kHfsPayloadBase64)).trimmed();
    if (!explicitBase64.isEmpty()) {
        const QByteArray decoded = QByteArray::fromBase64(explicitBase64.toLatin1());
        if (decoded.isEmpty()) {
            *blocker = QStringLiteral("HFS+ payload_base64 is empty or invalid");
            return std::nullopt;
        }
        return QString::fromLatin1(decoded.toBase64());
    }

    if (operation.payload.contains(QString::fromLatin1(kHfsPayloadText))) {
        const QByteArray bytes =
            payloadString(operation, QString::fromLatin1(kHfsPayloadText)).toUtf8();
        if (bytes.isEmpty()) {
            *blocker = QStringLiteral("HFS+ payload text must not be empty");
            return std::nullopt;
        }
        return QString::fromLatin1(bytes.toBase64());
    }

    *blocker = QStringLiteral("HFS+ payload is required");
    return std::nullopt;
}

QString apfsRootFilePayloadScript(const QString& payloadBase64) {
    if (payloadBase64.isEmpty()) {
        return {};
    }
    return QStringLiteral(
               "$payloadPath = Join-Path ([System.IO.Path]::GetTempPath()) "
               "('sak-apfs-root-file-' + [guid]::NewGuid().ToString('N') + '.bin')\n"
               "[System.IO.File]::WriteAllBytes($payloadPath, "
               "[Convert]::FromBase64String(%1))\n")
        .arg(PartitionScriptBuilder::quotePowerShell(payloadBase64));
}

QString hfsPayloadScript(const QString& payloadBase64) {
    if (payloadBase64.isEmpty()) {
        return {};
    }
    return QStringLiteral(
               "$payloadPath = Join-Path ([System.IO.Path]::GetTempPath()) "
               "('sak-hfs-payload-' + [guid]::NewGuid().ToString('N') + '.bin')\n"
               "[System.IO.File]::WriteAllBytes($payloadPath, "
               "[Convert]::FromBase64String(%1))\n")
        .arg(PartitionScriptBuilder::quotePowerShell(payloadBase64));
}

QString apfsRootFileCleanupScript(const QString& payloadBase64) {
    if (payloadBase64.isEmpty()) {
        return {};
    }
    return QStringLiteral(
        "} finally {\n"
        "  Remove-Item -LiteralPath $payloadPath -Force -ErrorAction SilentlyContinue\n"
        "}\n");
}

QString hfsPayloadCleanupScript(const QString& payloadBase64) {
    if (payloadBase64.isEmpty()) {
        return {};
    }
    return QStringLiteral(
        "  Remove-Item -LiteralPath $payloadPath -Force -ErrorAction SilentlyContinue\n");
}

struct ApfsRootFileMutationScriptInput {
    QString blocker;
    QString file_name;
    QString directory_name;
    QString volume_name;
    QString payload_base64;
    uint64_t patch_offset_bytes{0};
    QString command;
    QString evidence_id;
};

QString apfsRootFileMutationPrerequisiteBlocker(const PartitionOperation& operation) {
    if (!isApfsRootFileMutationOperation(operation.type)) {
        return QStringLiteral("Unsupported APFS root-file mutation");
    }
    const QString targetBlocker = rawPartitionTargetPathBlocker(operation);
    if (!targetBlocker.isEmpty()) {
        return targetBlocker;
    }
    const QString fs =
        payloadString(operation, QStringLiteral("file_system"), QStringLiteral("APFS"));
    if (!isApfsFileSystemToken(fs)) {
        return QStringLiteral("APFS root-file mutation requires APFS file system");
    }
    if (!payloadBool(operation, QString::fromLatin1(kTargetWipeConfirmedPayload))) {
        return QStringLiteral("APFS root-file mutation requires confirmation");
    }
    if (!payloadBool(operation, QString::fromLatin1(kApfsGeneratedLayoutConfirmedPayload))) {
        return QStringLiteral("APFS root-file mutation requires generated-layout confirmation");
    }
    if (operation.target.size_bytes < kMinimumApfsGeneratedRawFormatBytes ||
        operation.target.size_bytes > kMaximumApfsGeneratedSingleChunkBytes) {
        return QStringLiteral(
            "APFS generated raw mutation currently supports one-spaceman-chunk targets "
            "from 64 MiB through 128 MiB");
    }
    return {};
}

QString apfsRootMutationDirectoryName(const PartitionOperation& operation) {
    const QString directoryName =
        payloadString(operation, QString::fromLatin1(kApfsRootDirectoryNamePayload)).trimmed();
    if (!directoryName.isEmpty()) {
        return directoryName;
    }
    return apfsRootDirectoryMutation(operation.type)
               ? payloadString(operation, QString::fromLatin1(kApfsRootFileNamePayload)).trimmed()
               : QString();
}

QString apfsRootMutationFileName(const PartitionOperation& operation) {
    return payloadString(operation, QString::fromLatin1(kApfsRootFileNamePayload)).trimmed();
}

QString apfsRootMutationMissingNameBlocker(PartitionOperationType type) {
    if (apfsRootDirectoryFileMutation(type)) {
        return QStringLiteral(
            "APFS root-directory file mutation requires a directory name and file name");
    }
    return apfsRootDirectoryMutation(type)
               ? QStringLiteral("APFS root-directory mutation requires a directory name")
               : QStringLiteral("APFS root-file mutation requires a file name");
}

QString apfsVolumeLabel(const PartitionOperation& operation) {
    return payloadString(operation, QStringLiteral("label")).trimmed();
}

bool apfsVolumeLabelInvalid(const QString& label) {
    return label.size() > kApfsVolumeLabelMaxChars ||
           label.toUtf8().size() >= kApfsVolumeLabelFieldBytes ||
           label.contains(QLatin1Char('/')) || label.contains(QLatin1Char('\\')) ||
           label.contains(QLatin1Char(':'));
}

QString apfsVolumeLabelBlocker(PartitionOperationType type, const QString& label) {
    if (!apfsVolumeLabelMutation(type)) {
        return {};
    }
    if (label.isEmpty()) {
        return QStringLiteral("APFS volume-label mutation requires a label");
    }
    if (apfsVolumeLabelInvalid(label)) {
        return QStringLiteral(
            "APFS volume-label mutation label must fit APFS UTF-8 field and not contain path "
            "separators");
    }
    return {};
}

QString apfsRootMutationNameBlocker(const PartitionOperation& operation,
                                    const ApfsRootFileMutationScriptInput& input) {
    if ((apfsRootFileNameRequired(operation.type) && input.file_name.isEmpty()) ||
        (apfsRootDirectoryNameRequired(operation.type) && input.directory_name.isEmpty())) {
        return apfsRootMutationMissingNameBlocker(operation.type);
    }
    return {};
}

ApfsRootFileMutationScriptInput apfsRootFileMutationScriptInput(
    const PartitionOperation& operation) {
    ApfsRootFileMutationScriptInput input;
    input.blocker = apfsRootFileMutationPrerequisiteBlocker(operation);
    if (!input.blocker.isEmpty()) {
        return input;
    }

    input.file_name = apfsRootMutationFileName(operation);
    input.directory_name = apfsRootMutationDirectoryName(operation);
    input.volume_name = apfsVolumeLabel(operation);

    input.blocker = apfsVolumeLabelBlocker(operation.type, input.volume_name);
    if (!input.blocker.isEmpty()) {
        return input;
    }

    input.blocker = apfsRootMutationNameBlocker(operation, input);
    if (!input.blocker.isEmpty()) {
        return input;
    }

    QString blocker;
    const auto payloadBase64 = apfsRootFilePayloadBase64(operation, &blocker);
    if (!payloadBase64.has_value()) {
        input.blocker = blocker;
        return input;
    }
    const auto patchOffset = apfsRootFilePatchOffset(operation, &blocker);
    if (!patchOffset.has_value()) {
        input.blocker = blocker;
        return input;
    }

    input.payload_base64 = payloadBase64.value();
    input.patch_offset_bytes = patchOffset.value();
    input.command = apfsRootFileMutationCommand(operation.type);
    input.evidence_id = apfsRootFileMutationEvidenceId(operation.type);
    return input;
}

QString apfsRootFileMutationInvoke(const PartitionOperation& operation,
                                   const ApfsRootFileMutationScriptInput& input) {
    QString invoke = QStringLiteral(
                         "Invoke-SakApfsWriterCli -Command %1 -TargetPath $targetPath "
                         "-SizeBytes $targetSizeBytes -VolumeName %2 -EvidenceId %3")
                         .arg(PartitionScriptBuilder::quotePowerShell(input.command),
                              PartitionScriptBuilder::quotePowerShell(input.volume_name),
                              PartitionScriptBuilder::quotePowerShell(input.evidence_id));
    if (apfsRootDirectoryNameRequired(operation.type)) {
        invoke += QStringLiteral(" -DirectoryName %1")
                      .arg(PartitionScriptBuilder::quotePowerShell(input.directory_name));
    }
    if (apfsRootFileNameRequired(operation.type)) {
        invoke += QStringLiteral(" -FileName %1")
                      .arg(PartitionScriptBuilder::quotePowerShell(input.file_name));
    }
    if (apfsRootFileMutationWritesPayload(operation.type)) {
        invoke += QStringLiteral(" -PayloadFile $payloadPath");
    }
    if (operation.type == PartitionOperationType::ApfsPatchRootFile ||
        operation.type == PartitionOperationType::ApfsPatchRootDirectoryFile) {
        invoke += QStringLiteral(" -PatchOffsetBytes %1").arg(uintArg(input.patch_offset_bytes));
    }
    return invoke + QLatin1Char('\n');
}

QString apfsRootFileMutationDisplayName(const ApfsRootFileMutationScriptInput& input) {
    if (!input.volume_name.isEmpty()) {
        return input.volume_name;
    }
    if (!input.directory_name.isEmpty() && !input.file_name.isEmpty()) {
        return QStringLiteral("%1/%2").arg(input.directory_name, input.file_name);
    }
    return input.directory_name.isEmpty() ? input.file_name : input.directory_name;
}

QString apfsRootFilePayloadTryPrefix(const QString& payloadScript) {
    if (payloadScript.isEmpty()) {
        return {};
    }
    return QStringLiteral("try {\n");
}

struct HfsFileMutationScriptInput {
    QString blocker;
    QString hfs_path;
    QString destination_hfs_path;
    QString payload_base64;
    uint64_t file_id{0};
    QString attribute_name;
    QString command;
    QString evidence_id;
    QString file_system;
    bool allow_journaled{false};
    bool allow_wrapped{false};
    bool secure_wipe_released_blocks{false};
};

bool populateHfsMutationBaseInput(const PartitionOperation& operation,
                                  HfsFileMutationScriptInput* input) {
    if (!isHfsFileMutationOperation(operation.type)) {
        input->blocker = QStringLiteral("Unsupported HFS+ file mutation");
        return false;
    }
    const QString targetBlocker = rawPartitionTargetPathBlocker(operation);
    if (!targetBlocker.isEmpty()) {
        input->blocker = targetBlocker;
        return false;
    }
    input->file_system =
        payloadString(operation, QStringLiteral("file_system"), QStringLiteral("HFS+"));
    if (!isHfsFileSystemToken(input->file_system)) {
        input->blocker = QStringLiteral("HFS+ file mutation requires HFS+ or HFSX file system");
        return false;
    }
    if (!payloadBool(operation, QString::fromLatin1(kTargetWipeConfirmedPayload))) {
        input->blocker = QStringLiteral("HFS+ file mutation requires confirmation");
        return false;
    }
    return true;
}

bool populateHfsMutationPathInput(const PartitionOperation& operation,
                                  HfsFileMutationScriptInput* input) {
    if (hfsFileMutationNeedsPath(operation.type)) {
        input->hfs_path = payloadString(operation, QString::fromLatin1(kHfsPathPayload)).trimmed();
        if (input->hfs_path.isEmpty()) {
            input->blocker = QStringLiteral("HFS+ file mutation requires an HFS path");
            return false;
        }
    }
    if (operation.type == PartitionOperationType::HfsRenameMoveCatalogEntry) {
        input->destination_hfs_path =
            payloadString(operation, QString::fromLatin1(kHfsDestinationPathPayload)).trimmed();
        if (input->destination_hfs_path.isEmpty()) {
            input->blocker = QStringLiteral("HFS+ rename/move requires a destination HFS path");
            return false;
        }
    }
    return true;
}

bool populateHfsMutationPayloadInput(const PartitionOperation& operation,
                                     HfsFileMutationScriptInput* input) {
    QString blocker;
    const auto payloadBase64 = hfsMutationPayloadBase64(operation, &blocker);
    if (!payloadBase64.has_value()) {
        input->blocker = blocker;
        return false;
    }
    input->payload_base64 = payloadBase64.value();
    return true;
}

bool populateHfsMutationAttributeInput(const PartitionOperation& operation,
                                       HfsFileMutationScriptInput* input) {
    if (hfsFileMutationIsAttribute(operation.type)) {
        input->file_id = payloadUInt64(operation, QString::fromLatin1(kHfsFileIdPayload));
        input->attribute_name =
            payloadString(operation, QString::fromLatin1(kHfsAttributeNamePayload)).trimmed();
        if (input->file_id == 0 || input->attribute_name.isEmpty()) {
            input->blocker =
                QStringLiteral("HFS+ attribute mutation requires file ID and attribute name");
            return false;
        }
    }
    return true;
}

bool populateHfsMutationSecureWipeInput(const PartitionOperation& operation,
                                        HfsFileMutationScriptInput* input) {
    const bool requested = payloadBool(operation,
                                       QString::fromLatin1(kHfsSecureWipeReleasedBlocksPayload));
    if (requested && !hfsFileMutationSupportsSecureWipe(operation.type)) {
        input->blocker = QStringLiteral(
            "HFS+ secure block wipe is supported only for delete-file or delete-folder-tree "
            "mutations");
        return false;
    }
    input->secure_wipe_released_blocks = requested;
    return true;
}

void populateHfsMutationCommandInput(const PartitionOperation& operation,
                                     HfsFileMutationScriptInput* input) {
    input->command = hfsFileMutationCommand(operation.type);
    input->evidence_id = hfsFileMutationEvidenceId(operation.type);
    input->allow_journaled = payloadBool(operation,
                                         QString::fromLatin1(kHfsAllowJournaledVolumePayload));
    input->allow_wrapped = payloadBool(operation,
                                       QString::fromLatin1(kHfsAllowWrappedVolumePayload));
}

HfsFileMutationScriptInput hfsFileMutationScriptInput(const PartitionOperation& operation) {
    HfsFileMutationScriptInput input;
    if (!populateHfsMutationBaseInput(operation, &input) ||
        !populateHfsMutationPathInput(operation, &input) ||
        !populateHfsMutationPayloadInput(operation, &input) ||
        !populateHfsMutationAttributeInput(operation, &input) ||
        !populateHfsMutationSecureWipeInput(operation, &input)) {
        return input;
    }
    populateHfsMutationCommandInput(operation, &input);
    return input;
}

QString hfsFileMutationInvoke(const PartitionOperation& operation,
                              const HfsFileMutationScriptInput& input) {
    QString invoke =
        QStringLiteral(
            "Invoke-SakHfsWriterCli -Command %1 -ImagePath $stagedImagePath "
            "-EvidenceId %2 -AllowJournaled $%3 -AllowWrapped $%4")
            .arg(PartitionScriptBuilder::quotePowerShell(input.command),
                 PartitionScriptBuilder::quotePowerShell(input.evidence_id),
                 input.allow_journaled ? QStringLiteral("true") : QStringLiteral("false"),
                 input.allow_wrapped ? QStringLiteral("true") : QStringLiteral("false"));
    if (hfsFileMutationIsAttribute(operation.type)) {
        invoke += QStringLiteral(" -FileId %1 -AttributeName %2")
                      .arg(uintArg(input.file_id),
                           PartitionScriptBuilder::quotePowerShell(input.attribute_name));
    } else {
        invoke += QStringLiteral(" -HfsPath %1")
                      .arg(PartitionScriptBuilder::quotePowerShell(input.hfs_path));
    }
    if (operation.type == PartitionOperationType::HfsRenameMoveCatalogEntry) {
        invoke += QStringLiteral(" -DestinationHfsPath %1")
                      .arg(PartitionScriptBuilder::quotePowerShell(input.destination_hfs_path));
    }
    if (hfsFileMutationWritesPayload(operation.type)) {
        invoke += QStringLiteral(" -PayloadFile $payloadPath");
    }
    if (input.secure_wipe_released_blocks) {
        invoke += QStringLiteral(" -SecureWipeReleasedBlocks $true");
    }
    return invoke + QLatin1Char('\n');
}

QString runtimeFilesystemManifestPath() {
    QDir sourceCandidate = QDir::current();
    for (int depth = 0; depth < kRuntimeManifestSourceFallbackDepth; ++depth) {
        const QString sourceManifest =
            sourceCandidate.filePath(PartitionFileSystemToolManifest::defaultRuntimeRelativePath());
        const bool sourceCandidateLooksLikeSourceTree =
            QFileInfo::exists(sourceCandidate.filePath(QStringLiteral("CMakeLists.txt"))) &&
            QFileInfo::exists(
                sourceCandidate.filePath(QStringLiteral("src/core/partition_script_builder.cpp")));
        if (sourceCandidateLooksLikeSourceTree && QFileInfo::exists(sourceManifest)) {
            return sourceManifest;
        }
        if (!sourceCandidate.cdUp()) {
            break;
        }
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.trimmed().isEmpty()) {
        const QString appManifest =
            PartitionFileSystemToolManifest::defaultRuntimeManifestPath(appDir);
        if (QFileInfo::exists(appManifest)) {
            return appManifest;
        }
    }
    return QDir::current().filePath(PartitionFileSystemToolManifest::defaultRuntimeRelativePath());
}

QString runtimeApfsWriterCliPath() {
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.trimmed().isEmpty()) {
        return QDir(appDir).filePath(QStringLiteral("sak_apfs_writer_cli.exe"));
    }
    return QDir::current().filePath(QStringLiteral("sak_apfs_writer_cli.exe"));
}

QString runtimeHfsWriterCliPath() {
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.trimmed().isEmpty()) {
        return QDir(appDir).filePath(QStringLiteral("sak_hfs_writer_cli.exe"));
    }
    return QDir::current().filePath(QStringLiteral("sak_hfs_writer_cli.exe"));
}

QString apfsWriterCliFunctionScript() {
    return QStringLiteral(
               "function Invoke-SakApfsWriterCli {\n"
               "  param([string]$Command, [string]$TargetPath, [uint64]$SizeBytes, "
               "[string]$VolumeName, [string]$EvidenceId, [string]$FileName = '', "
               "[string]$DirectoryName = '', [string]$PayloadFile = '', "
               "[uint64]$PatchOffsetBytes = 0)\n"
               "  $cliPath = %1\n"
               "  if (-not (Test-Path -LiteralPath $cliPath -PathType Leaf)) {\n"
               "    throw ('APFS writer helper missing: {0}' -f $cliPath)\n"
               "  }\n"
               "  $args = @($Command, '--target', $TargetPath, '--size-bytes', "
               "([string]$SizeBytes), '--block-size-bytes', '4096', '--evidence-id', "
               "$EvidenceId, '--confirm-target', '--allow-raw-target')\n"
               "  if ($Command -eq 'format-raw') { $args += @('--volume-name', $VolumeName) }\n"
               "  if ($Command -in @('write-raw-root-file', 'patch-raw-root-file', "
               "'patch-raw-root-directory-file', "
               "'delete-raw-root-file', 'write-raw-root-directory-file', "
               "'delete-raw-root-directory-file', 'commit-raw-file-write', "
               "'commit-raw-file-delete', 'commit-raw-directory-child-write', "
               "'commit-raw-directory-child-delete', 'commit-raw-file-patch')) {\n"
               "    if ([string]::IsNullOrWhiteSpace($FileName)) { throw 'APFS root file name is "
               "required' }\n"
               "    $args += @('--file-name', $FileName)\n"
               "  }\n"
               "  if ($Command -eq 'commit-raw-file-patch' -and "
               "-not [string]::IsNullOrWhiteSpace($DirectoryName)) {\n"
               "    $args += @('--directory-name', $DirectoryName)\n"
               "  }\n"
               "  if ($Command -in @('create-raw-root-directory', 'delete-raw-root-directory', "
               "'write-raw-root-directory-file', 'patch-raw-root-directory-file', "
               "'delete-raw-root-directory-file', 'commit-raw-directory-create', "
               "'commit-raw-directory-delete', 'commit-raw-directory-child-write', "
               "'commit-raw-directory-child-delete')) {\n"
               "    if ([string]::IsNullOrWhiteSpace($DirectoryName)) { throw 'APFS root directory "
               "name is "
               "required' }\n"
               "    $args += @('--directory-name', $DirectoryName)\n"
               "  }\n"
               "  if ($Command -in @('write-raw-root-file', 'patch-raw-root-file', "
               "'patch-raw-root-directory-file', 'write-raw-root-directory-file', "
               "'commit-raw-file-write', 'commit-raw-directory-child-write', "
               "'commit-raw-file-patch')) {\n"
               "    if ([string]::IsNullOrWhiteSpace($PayloadFile)) { throw 'APFS root file "
               "payload "
               "is required' }\n"
               "    $args += @('--payload-file', $PayloadFile)\n"
               "  }\n"
               "  if ($Command -in @('patch-raw-root-file', 'patch-raw-root-directory-file', "
               "'commit-raw-file-patch')) {\n"
               "    $args += @('--patch-offset-bytes', ([string]$PatchOffsetBytes))\n"
               "  }\n"
               "  Write-Output ('Running S.A.K. APFS writer helper: {0} {1}' -f $cliPath, "
               "($args -join ' '))\n"
               "  $output = & $cliPath @args 2>&1\n"
               "  $exitCode = [int]$LASTEXITCODE\n"
               "  $output | ForEach-Object { Write-Output $_ }\n"
               "  if ($exitCode -ne 0) { throw ('APFS writer helper failed with exit code {0}' "
               "-f $exitCode) }\n"
               "}\n")
        .arg(PartitionScriptBuilder::quotePowerShell(
            QDir::toNativeSeparators(runtimeApfsWriterCliPath())));
}

QString hfsWriterCliFunctionScript() {
    return QStringLiteral(
               "function Invoke-SakHfsWriterCli {\n"
               "  param([string]$Command, [string]$ImagePath, [string]$HfsPath = '', "
               "[string]$DestinationHfsPath = '', "
               "[string]$PayloadFile = '', [uint64]$FileId = 0, [string]$AttributeName = '', "
               "[string]$EvidenceId, [bool]$AllowJournaled = $false, [bool]$AllowWrapped = $false, "
               "[bool]$SecureWipeReleasedBlocks = $false)\n"
               "  $cliPath = %1\n"
               "  if (-not (Test-Path -LiteralPath $cliPath -PathType Leaf)) {\n"
               "    throw ('HFS writer helper missing: {0}' -f $cliPath)\n"
               "  }\n"
               "  $args = @($Command, '--target', $ImagePath, '--evidence-id', $EvidenceId, "
               "'--confirm-target')\n"
               "  if ($Command -in @('replace-inline-attribute-image', "
               "'replace-fork-attribute-image', 'grow-fork-attribute-image')) {\n"
               "    if ($FileId -eq 0 -or [string]::IsNullOrWhiteSpace($AttributeName)) { "
               "throw 'HFS attribute identity is required' }\n"
               "    $args += @('--file-id', ([string]$FileId), '--attribute-name', "
               "$AttributeName)\n"
               "  } else {\n"
               "    if ([string]::IsNullOrWhiteSpace($HfsPath)) { throw 'HFS path is required' }\n"
               "    $args += @('--hfs-path', $HfsPath)\n"
               "  }\n"
               "  if ($Command -eq 'rename-catalog-entry-image') {\n"
               "    if ([string]::IsNullOrWhiteSpace($DestinationHfsPath)) { "
               "throw 'HFS destination path is required' }\n"
               "    $args += @('--destination-hfs-path', $DestinationHfsPath)\n"
               "  }\n"
               "  if ($Command -notin @('truncate-image', 'truncate-resource-fork-image', "
               "'create-empty-file-image', 'delete-empty-file-image', "
               "'delete-file-image', "
               "'create-empty-folder-image', 'delete-empty-folder-image', "
               "'delete-folder-tree-image', 'rename-catalog-entry-image')) {\n"
               "    if ([string]::IsNullOrWhiteSpace($PayloadFile)) { throw 'HFS payload file is "
               "required' }\n"
               "    $args += @('--payload-file', $PayloadFile)\n"
               "  }\n"
               "  if ($AllowJournaled) { $args += '--allow-journaled-volume' }\n"
               "  if ($AllowWrapped) { $args += '--allow-wrapped-volume' }\n"
               "  if ($SecureWipeReleasedBlocks) { $args += '--secure-wipe-released-blocks' }\n"
               "  Write-Output ('Running S.A.K. HFS writer helper: {0} {1}' -f $cliPath, "
               "($args -join ' '))\n"
               "  $output = & $cliPath @args 2>&1\n"
               "  $exitCode = [int]$LASTEXITCODE\n"
               "  $output | ForEach-Object { Write-Output $_ }\n"
               "  if ($exitCode -ne 0) { throw ('HFS writer helper failed with exit code {0}' "
               "-f $exitCode) }\n"
               "}\n")
        .arg(PartitionScriptBuilder::quotePowerShell(
            QDir::toNativeSeparators(runtimeHfsWriterCliPath())));
}

QString powerShellArrayLiteral(const QStringList& values) {
    QStringList quoted;
    quoted.reserve(values.size());
    for (const auto& value : values) {
        quoted.append(PartitionScriptBuilder::quotePowerShell(value));
    }
    return QStringLiteral("@(%1)").arg(quoted.join(QStringLiteral(", ")));
}

QString powerShellArrayLiteralWithTrailingExpression(const QStringList& values,
                                                     const QString& trailingExpression) {
    QStringList quoted;
    const qsizetype prefixSize = std::max<qsizetype>(0, values.size() - 1);
    quoted.reserve(prefixSize + 1);
    for (qsizetype index = 0; index < prefixSize; ++index) {
        quoted.append(PartitionScriptBuilder::quotePowerShell(values.at(index)));
    }
    quoted.append(trailingExpression);
    return QStringLiteral("@(%1)").arg(quoted.join(QStringLiteral(", ")));
}

QString filesystemToolFunctionScript() {
    return QStringLiteral(
        "function Invoke-SakFilesystemTool([string]$Name, [string]$ToolPath, "
        "[string]$ExpectedHash, [string[]]$ToolArgs, [int[]]$AcceptedExitCodes) {\n"
        "  $actualHash = (Get-FileHash -LiteralPath $ToolPath -Algorithm SHA256).Hash."
        "ToLowerInvariant()\n"
        "  if ($actualHash -ne $ExpectedHash) { throw \"Filesystem tool hash mismatch: $Name\" }\n"
        "  Write-Output ('Running approved filesystem tool: {0} {1}' -f $ToolPath, "
        "($ToolArgs -join ' '))\n"
        "  & $ToolPath @ToolArgs 2>&1 | ForEach-Object { Write-Output $_ }\n"
        "  $toolExitCode = $LASTEXITCODE\n"
        "  Write-Output ('Filesystem tool exit code: {0}' -f $toolExitCode)\n"
        "  if ($AcceptedExitCodes -notcontains $toolExitCode) { exit $toolExitCode }\n"
        "}\n");
}

QString filesystemToolCallScript(const QString& name,
                                 const PartitionFileSystemToolResolution& resolution,
                                 const QStringList& arguments,
                                 const QStringList& acceptedExitCodes) {
    return QStringLiteral(
               "Invoke-SakFilesystemTool -Name %1 -ToolPath %2 -ExpectedHash %3 "
               "-ToolArgs %4 -AcceptedExitCodes @(%5)\n")
        .arg(PartitionScriptBuilder::quotePowerShell(name),
             PartitionScriptBuilder::quotePowerShell(
                 QDir::toNativeSeparators(resolution.tool_path)),
             PartitionScriptBuilder::quotePowerShell(resolution.tool.binary_sha256.toLower()),
             powerShellArrayLiteral(arguments),
             acceptedExitCodes.join(QStringLiteral(", ")));
}

QString filesystemToolCallScriptWithArgsExpression(
    const QString& name,
    const PartitionFileSystemToolResolution& resolution,
    const QString& argumentsExpression,
    const QStringList& acceptedExitCodes) {
    return QStringLiteral(
               "Invoke-SakFilesystemTool -Name %1 -ToolPath %2 -ExpectedHash %3 "
               "-ToolArgs %4 -AcceptedExitCodes @(%5)\n")
        .arg(PartitionScriptBuilder::quotePowerShell(name),
             PartitionScriptBuilder::quotePowerShell(
                 QDir::toNativeSeparators(resolution.tool_path)),
             PartitionScriptBuilder::quotePowerShell(resolution.tool.binary_sha256.toLower()),
             argumentsExpression,
             acceptedExitCodes.join(QStringLiteral(", ")));
}

QString dismountSelectedPartitionVolumeScript() {
    return QStringLiteral(
        "$volume = $null\n"
        "try { $volume = $p | Get-Volume -ErrorAction Stop } catch { }\n"
        "if ($volume -and $volume.DriveLetter) {\n"
        "  Dismount-Volume -DriveLetter $volume.DriveLetter -Force -ErrorAction Stop\n"
        "}\n");
}

QString diskPartLabel(QString label) {
    label = label.trimmed().left(kDiskPartLabelMaxChars);
    label.replace(QStringLiteral("\""), QString());
    return label.isEmpty() ? QStringLiteral("SAKDATA") : label;
}

QString sizeMbArg(uint64_t bytes) {
    return QString::number(std::max<uint64_t>(kMinimumDiskPartSizeMb, bytes / kCloneIoBufferBytes));
}

QString backupRestoreHelpersScript() {
    return QStringLiteral(
        "function Invoke-SakRobocopy([string]$from, [string]$to) {\n"
        "  robocopy.exe $from $to /MIR /COPYALL /DCOPY:DAT /XJ /R:1 /W:1\n"
        "  $code = $LASTEXITCODE\n"
        "  if ($code -gt 7) { exit $code }\n"
        "}\n"
        "function Get-SakFileManifest([string]$root) {\n"
        "  $rootFull = [System.IO.Path]::GetFullPath($root)\n"
        "  @(Get-ChildItem -LiteralPath $rootFull -Recurse -Force -File | ForEach-Object {\n"
        "    $relative = $_.FullName.Substring($rootFull.Length).TrimStart('\\')\n"
        "    $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash\n"
        "    [pscustomobject]@{ RelativePath = $relative; Length = [uint64]$_.Length; Hash = $hash "
        "}\n"
        "  } | Sort-Object RelativePath)\n"
        "}\n"
        "function Assert-SakBackupRoot([string]$backupRoot, [string[]]$blockedRoots) {\n"
        "  $full = [System.IO.Path]::GetFullPath($backupRoot)\n"
        "  if (-not (Test-Path -LiteralPath $full -PathType Container)) { throw 'Backup directory "
        "does not exist' }\n"
        "  foreach ($root in $blockedRoots) { if ($root -and $full.StartsWith($root, "
        "[System.StringComparison]::OrdinalIgnoreCase)) { throw 'Backup directory must be outside "
        "affected volumes' } }\n"
        "  return $full\n"
        "}\n"
        "function Assert-SakManifestMatch($expected, $actual) {\n"
        "  $diff = @(Compare-Object -ReferenceObject $expected -DifferenceObject $actual -Property "
        "RelativePath,Length,Hash)\n"
        "  if ($diff.Count -gt 0) { $diff | Format-Table | Out-String | Write-Output; throw "
        "'Restored file manifest differs from backup' }\n"
        "}\n");
}

QString diskPartRunnerScript() {
    return QStringLiteral(
        "function Invoke-SakDiskPart([string[]]$lines) {\n"
        "  $scriptPath = Join-Path $env:TEMP ('sak-diskpart-{0}.txt' -f "
        "[guid]::NewGuid().ToString('N'))\n"
        "  try { $lines | Set-Content -LiteralPath $scriptPath -Encoding ASCII; diskpart.exe /s "
        "$scriptPath; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } }\n"
        "  finally { Remove-Item -LiteralPath $scriptPath -Force -ErrorAction SilentlyContinue }\n"
        "}\n");
}

bool isRawDevicePath(QString path) {
    path = path.trimmed();
    path.replace('/', '\\');
    return path.startsWith(QStringLiteral("\\\\.\\"), Qt::CaseInsensitive);
}

bool isPhysicalDrivePath(QString path) {
    path = path.trimmed();
    path.replace('/', '\\');
    return path.startsWith(QStringLiteral("\\\\.\\PhysicalDrive"), Qt::CaseInsensitive);
}

bool isSupportedAllocationUnitSize(uint64_t value) {
    return value == kAllocationUnitDefaultBytes || value == kAllocationUnit512Bytes ||
           value == kAllocationUnit1KbBytes || value == kAllocationUnit2KbBytes ||
           value == kAllocationUnit4KbBytes || value == kAllocationUnit8KbBytes ||
           value == kAllocationUnit16KbBytes || value == kAllocationUnit32KbBytes ||
           value == kAllocationUnit64KbBytes;
}

bool isSupportedGptCreateType(const QString& value) {
    return value.isEmpty() || QStringList{QStringLiteral("{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}"),
                                          QStringLiteral("{DE94BBA4-06D1-4D40-A16A-BFD50179D6AC}"),
                                          QStringLiteral("{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}"),
                                          QStringLiteral("{7C3457EF-0000-11AA-AA11-00306543ECAC}")}
                                  .contains(value.toUpper());
}

bool isSupportedMbrCreateType(const QString& value) {
    return value.isEmpty() ||
           QStringList{QStringLiteral("IFS"), QStringLiteral("FAT32")}.contains(value.toUpper());
}

bool hasMixedCreatePartitionTypes(const PartitionOperation& operation) {
    return !payloadString(operation, QStringLiteral("gpt_type")).trimmed().isEmpty() &&
           !payloadString(operation, QStringLiteral("mbr_type")).trimmed().isEmpty();
}

bool createTypeMatchesFileSystem(const PartitionOperation& operation, const QString& fs) {
    const QString gptType =
        payloadString(operation, QStringLiteral("gpt_type")).trimmed().toUpper();
    const QString mbrType =
        payloadString(operation, QStringLiteral("mbr_type")).trimmed().toUpper();
    if (gptType == QStringLiteral("{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}") ||
        mbrType == QStringLiteral("FAT32")) {
        return fs.compare(QStringLiteral("FAT32"), Qt::CaseInsensitive) == 0;
    }
    if (gptType == QStringLiteral("{DE94BBA4-06D1-4D40-A16A-BFD50179D6AC}") ||
        mbrType == QStringLiteral("IFS")) {
        return fs.compare(QStringLiteral("NTFS"), Qt::CaseInsensitive) == 0;
    }
    if (gptType == QStringLiteral("{7C3457EF-0000-11AA-AA11-00306543ECAC}")) {
        return isApfsFileSystemToken(fs);
    }
    return true;
}

bool createFitsSelectedRegion(const PartitionOperation& operation, uint64_t size) {
    const uint64_t relativeOffset = payloadUInt64(operation,
                                                  QStringLiteral("relative_offset_bytes"));
    return operation.target.size_bytes == 0 ||
           (relativeOffset <= operation.target.size_bytes &&
            size <= operation.target.size_bytes - relativeOffset);
}

QString allocationUnitArg(const PartitionOperation& operation) {
    const uint64_t value = payloadUInt64(operation, QStringLiteral("allocation_unit_bytes"));
    return value == kAllocationUnitDefaultBytes
               ? QString()
               : QStringLiteral(" -AllocationUnitSize %1").arg(uintArg(value));
}

QString createOffsetArg(const PartitionOperation& operation) {
    const uint64_t relativeOffset = payloadUInt64(operation,
                                                  QStringLiteral("relative_offset_bytes"));
    const uint64_t selectedOffset = operation.target.offset_bytes + relativeOffset;
    return selectedOffset == 0 ? QString()
                               : QStringLiteral(" -Offset %1").arg(uintArg(selectedOffset));
}

QString partitionTypeArg(const PartitionOperation& operation) {
    const QString gptType = payloadString(operation, QStringLiteral("gpt_type")).trimmed();
    if (!gptType.isEmpty()) {
        return QStringLiteral(" -GptType %1").arg(PartitionScriptBuilder::quotePowerShell(gptType));
    }
    const QString mbrType =
        payloadString(operation, QStringLiteral("mbr_type")).trimmed().toUpper();
    return mbrType.isEmpty() ? QString() : QStringLiteral(" -MbrType %1").arg(mbrType);
}

struct CloneTransferSpec {
    QString source;
    QString target;
    uint64_t source_size{0};
    uint64_t target_size{0};
    QString verify_mode;
    uint64_t source_offset{0};
    uint64_t target_offset{0};
};

CloneTransferSpec cloneTransferSpec(const PartitionOperation& operation) {
    CloneTransferSpec spec;
    spec.source = payloadString(operation, QStringLiteral("source_path"));
    spec.target = payloadString(operation, QStringLiteral("target_path"));
    spec.source_size = payloadUInt64(operation, QStringLiteral("source_size_bytes"));
    spec.target_size = payloadUInt64(operation, QStringLiteral("target_size_bytes"));
    spec.verify_mode = payloadString(operation, QStringLiteral("verify_mode"));
    spec.source_offset = payloadUInt64(operation, QStringLiteral("source_offset_bytes"));
    spec.target_offset = payloadUInt64(operation, QStringLiteral("target_offset_bytes"));
    return spec;
}

bool missingClonePaths(const CloneTransferSpec& spec) {
    return spec.source.isEmpty() || spec.target.isEmpty();
}

bool createImageTargetsRawDevice(const PartitionOperation& operation,
                                 const CloneTransferSpec& spec) {
    return operation.type == PartitionOperationType::CreateImage && isRawDevicePath(spec.target);
}

bool restoreImageMissingConfirmation(const PartitionOperation& operation) {
    return operation.type == PartitionOperationType::RestoreImage &&
           !payloadBool(operation, QStringLiteral("target_wipe_confirmed"));
}

bool restoreImageMissingSizes(const PartitionOperation& operation, const CloneTransferSpec& spec) {
    const bool missingSizes = spec.source_size == 0 || spec.target_size == 0;
    return operation.type == PartitionOperationType::RestoreImage && missingSizes;
}

bool restoreOrMigrateMissingPhysicalTarget(const PartitionOperation& operation,
                                           const CloneTransferSpec& spec) {
    return (operation.type == PartitionOperationType::RestoreImage ||
            operation.type == PartitionOperationType::MigrateOs) &&
           !isPhysicalDrivePath(spec.target);
}

bool cloneTargetTooSmall(const CloneTransferSpec& spec) {
    if (spec.source_size == 0 || spec.target_size == 0) {
        return false;
    }
    return spec.source_size > spec.target_size;
}

QString validateCloneOrImageScript(const PartitionOperation& operation,
                                   const CloneTransferSpec& spec) {
    if (missingClonePaths(spec)) {
        return QStringLiteral("Clone/image operation requires source_path and target_path");
    }
    if (createImageTargetsRawDevice(operation, spec)) {
        return QStringLiteral("Create Image destination must be a file path");
    }
    if (restoreImageMissingConfirmation(operation)) {
        return QStringLiteral("Restore Image requires target overwrite confirmation");
    }
    if (restoreImageMissingSizes(operation, spec)) {
        return QStringLiteral("Restore Image requires known image and target sizes");
    }
    if (restoreOrMigrateMissingPhysicalTarget(operation, spec)) {
        return QStringLiteral("Restore Image and OS migration require a physical target disk");
    }
    if (cloneTargetTooSmall(spec)) {
        return QStringLiteral("Target is smaller than source");
    }
    return {};
}

struct CreateScriptSpec {
    uint64_t size{0};
    QString file_system;
    QString label;
    QString drive_letter;
    QString full_format_arg;
    uint64_t allocation_unit{0};
    QString gpt_type;
    QString mbr_type;
};

CreateScriptSpec createScriptSpec(const PartitionOperation& operation) {
    CreateScriptSpec spec;
    spec.size = payloadUInt64(operation, QStringLiteral("size_bytes"));
    spec.file_system =
        payloadString(operation, QStringLiteral("file_system"), QStringLiteral("NTFS"));
    spec.label = payloadString(operation, QStringLiteral("label"), QStringLiteral("Data"));
    spec.drive_letter = payloadString(operation, QStringLiteral("drive_letter")).left(1).toUpper();
    spec.full_format_arg = payloadBool(operation, QStringLiteral("full_format"))
                               ? QStringLiteral(" -Full")
                               : QString();
    spec.allocation_unit = payloadUInt64(operation, QStringLiteral("allocation_unit_bytes"));
    spec.gpt_type = payloadString(operation, QStringLiteral("gpt_type")).trimmed();
    spec.mbr_type = payloadString(operation, QStringLiteral("mbr_type")).trimmed();
    return spec;
}

QString validateCreateFileSystemSupport(const CreateScriptSpec& spec, bool nonNativeCreate) {
    if (nonNativeCreate && !isSupportedNonNativeCreateFormatFileSystem(spec.file_system)) {
        return QStringLiteral("Unsupported non-Windows create file system");
    }
    if (!nonNativeCreate && !PartitionScriptBuilder::isSupportedFileSystem(spec.file_system)) {
        return QStringLiteral("Unsupported file system");
    }
    if (!nonNativeCreate && !isSupportedAllocationUnitSize(spec.allocation_unit)) {
        return QStringLiteral("Unsupported allocation unit size");
    }
    return {};
}

QString validateCreatePartitionType(const PartitionOperation& operation,
                                    const CreateScriptSpec& spec) {
    if (hasMixedCreatePartitionTypes(operation)) {
        return QStringLiteral("Create requires either gpt_type or mbr_type, not both");
    }
    if (!isSupportedGptCreateType(spec.gpt_type) || !isSupportedMbrCreateType(spec.mbr_type)) {
        return QStringLiteral("Unsupported create partition type");
    }
    if (!createTypeMatchesFileSystem(operation, spec.file_system)) {
        return QStringLiteral("Selected partition type is incompatible with file system");
    }
    return {};
}

QString validateNonNativeCreatePayload(const PartitionOperation& operation,
                                       const CreateScriptSpec& spec,
                                       bool nonNativeCreate) {
    if (!nonNativeCreate) {
        return {};
    }
    if (!payloadBool(operation, QString::fromLatin1(kTargetWipeConfirmedPayload))) {
        return QStringLiteral("Non-Windows create format requires destructive confirmation");
    }
    if (isLinuxSwapFileSystemToken(spec.file_system) &&
        !isSupportedLinuxSwapPageSize(linuxSwapPageSize(operation))) {
        return QStringLiteral("Unsupported Linux swap page size");
    }
    return {};
}

QString validateCreateScriptSpec(const PartitionOperation& operation,
                                 const CreateScriptSpec& spec) {
    const bool nonNativeCreate = isNonNativeFileSystemToolOperation(operation);
    if (spec.size == 0) {
        return QStringLiteral("Create requires size_bytes");
    }
    if (const QString blocker = validateCreateFileSystemSupport(spec, nonNativeCreate);
        !blocker.isEmpty()) {
        return blocker;
    }
    if (const QString blocker = validateCreatePartitionType(operation, spec); !blocker.isEmpty()) {
        return blocker;
    }
    if (const QString blocker = validateNonNativeCreatePayload(operation, spec, nonNativeCreate);
        !blocker.isEmpty()) {
        return blocker;
    }
    if (!createFitsSelectedRegion(operation, spec.size)) {
        return QStringLiteral("Create size and offset must fit selected unallocated region");
    }
    return {};
}

QString cloneTransferPreludeScript(const CloneTransferSpec& spec) {
    return QStringLiteral(
               "$src = %1\n"
               "$dst = %2\n"
               "$expectedBytes = [uint64]%3\n"
               "$targetBytes = [uint64]%4\n"
               "$sourceOffset = [uint64]%5\n"
               "$targetOffset = [uint64]%6\n"
               "$verifyMode = %7\n"
               "$bufferBytes = %8\n"
               "if ($targetBytes -gt 0 -and $expectedBytes -gt $targetBytes) { throw 'Target is "
               "smaller than source' }\n")
        .arg(PartitionScriptBuilder::quotePowerShell(QDir::toNativeSeparators(spec.source)),
             PartitionScriptBuilder::quotePowerShell(QDir::toNativeSeparators(spec.target)),
             uintArg(spec.source_size),
             uintArg(spec.target_size),
             uintArg(spec.source_offset),
             uintArg(spec.target_offset),
             PartitionScriptBuilder::quotePowerShell(spec.verify_mode),
             uintArg(kCloneIoBufferBytes));
}

QString cloneTransferOpenFunctionsScript() {
    return QStringLiteral(
        "function Open-SakRead([string]$p) { return [System.IO.File]::Open($p, "
        "[System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, "
        "[System.IO.FileShare]::ReadWrite) }\n"
        "function Open-SakWrite([string]$p) { if ($p.StartsWith('\\\\.\\')) { return "
        "[System.IO.File]::Open($p, [System.IO.FileMode]::Open, "
        "[System.IO.FileAccess]::Write, [System.IO.FileShare]::None) }; return "
        "[System.IO.File]::Open($p, [System.IO.FileMode]::Create, "
        "[System.IO.FileAccess]::Write, [System.IO.FileShare]::None) }\n"
        "function Read-SakExact($stream, [int]$count) { $buf = New-Object byte[] $count; "
        "$off = 0; while ($off -lt $count) { $read = $stream.Read($buf, $off, $count - "
        "$off); if ($read -le 0) { throw 'Unexpected end of stream during verification' }; "
        "$off += $read }; return $buf }\n"
        "function Copy-SakBytes($from, $to, [uint64]$bytes) { if ($bytes -eq 0) { "
        "$from.CopyTo($to, $bufferBytes); return }; $buf = New-Object byte[] $bufferBytes; "
        "$left = $bytes; while ($left -gt 0) { $take = "
        "[int][Math]::Min([uint64]$bufferBytes, $left); $read = $from.Read($buf, 0, $take); "
        "if ($read -le 0) { throw 'Source ended before expected byte count' }; "
        "$to.Write($buf, 0, $read); $left -= [uint64]$read } }\n");
}

QString cloneTransferVerificationFunctionsScript() {
    return QStringLiteral(
        "function Assert-SakFullCopy($from, $to, [uint64]$bytes, [uint64]$sourceStart, "
        "[uint64]$targetStart) { $left = $bytes; $pos = [uint64]0; $from.Position = "
        "[int64]$sourceStart; $to.Position = [int64]$targetStart; while ($left -gt 0) { "
        "$take = [int][Math]::Min([uint64]$bufferBytes, $left); $a = Read-SakExact $from "
        "$take; $b = Read-SakExact $to $take; for ($i = 0; $i -lt $take; $i++) { if ($a[$i] "
        "-ne $b[$i]) { throw \"Full verification mismatch at byte $($pos + [uint64]$i)\" } "
        "}; $pos += [uint64]$take; $left -= [uint64]$take } }\n"
        "function Assert-SakSampleCopy($from, $to, [uint64]$bytes, [uint64]$sourceStart, "
        "[uint64]$targetStart) { $sample = [int][Math]::Min([uint64]$bufferBytes, $bytes); "
        "$middle = [uint64]([Math]::Max([int64]0, [int64]($bytes / 2) - [int64]($sample / "
        "2))); $tail = [uint64]([Math]::Max([int64]0, [int64]$bytes - [int64]$sample)); "
        "foreach ($point in @([uint64]0, $middle, $tail) | Select-Object -Unique) { "
        "$from.Position = [int64]($sourceStart + $point); $to.Position = "
        "[int64]($targetStart + $point); $a = Read-SakExact $from $sample; $b = "
        "Read-SakExact $to $sample; for ($i = 0; $i -lt $sample; $i++) { if ($a[$i] -ne "
        "$b[$i]) { throw \"Sample verification mismatch at byte $($point + [uint64]$i)\" } "
        "} } }\n");
}

QString cloneTransferRawTargetFunctionsScript() {
    return QStringLiteral(
        "function Get-SakPhysicalDriveNumber([string]$p) { if (-not "
        "$p.StartsWith('\\\\.\\PhysicalDrive')) { return $null }; $suffix = "
        "$p.Substring('\\\\.\\PhysicalDrive'.Length); $n = -1; if (-not "
        "[int]::TryParse($suffix, [ref]$n)) { throw 'Invalid physical target disk path' }; "
        "return $n }\n"
        "function Assert-SakRawWriteTarget([string]$p) { $n = Get-SakPhysicalDriveNumber "
        "$p; if ($null -eq $n) { return $null }; $disk = Get-Disk -Number $n -ErrorAction "
        "Stop; if ($disk.IsBoot -or $disk.IsSystem) { throw 'Target disk is current OS "
        "disk' }; if ($disk.IsReadOnly) { throw 'Target disk is read-only' }; if "
        "($disk.BusType -eq 'Spaces') { throw 'Storage Spaces target disks are blocked' }; "
        "Set-Disk -Number $n -IsOffline $true -ErrorAction SilentlyContinue; return $n }\n"
        "function Restore-SakRawWriteTarget($n) { if ($null -ne $n) { Set-Disk -Number $n "
        "-IsOffline $false -ErrorAction SilentlyContinue } }\n");
}

QString cloneTransferExecutionScript() {
    return QStringLiteral(
        "$in = Open-SakRead $src\n"
        "$sakRawTargetDisk = Assert-SakRawWriteTarget $dst\n"
        "try { try { if ($expectedBytes -eq 0) { try { $expectedBytes = [uint64]$in.Length "
        "- $sourceOffset } catch {} }; [void]$in.Seek([int64]$sourceOffset, "
        "[System.IO.SeekOrigin]::Begin); $out = Open-SakWrite $dst; try { "
        "[void]$out.Seek([int64]$targetOffset, [System.IO.SeekOrigin]::Begin); "
        "Copy-SakBytes $in $out $expectedBytes; $out.Flush() } finally { $out.Dispose() } } "
        "finally { $in.Dispose() } }\n"
        "finally { Restore-SakRawWriteTarget $sakRawTargetDisk }\n");
}

QString cloneTransferVerifyExecutionScript() {
    return QStringLiteral(
        "if (-not [string]::IsNullOrWhiteSpace($verifyMode)) { if ($expectedBytes -eq 0) { "
        "throw 'Verification requires known source size' }; $srcVerify = Open-SakRead $src; "
        "$dstVerify = Open-SakRead $dst; try { if ($verifyMode -like 'Full*') { "
        "Write-Output 'Running full clone verification'; Assert-SakFullCopy $srcVerify "
        "$dstVerify $expectedBytes $sourceOffset $targetOffset } else { Write-Output "
        "'Running sample clone verification'; Assert-SakSampleCopy $srcVerify $dstVerify "
        "$expectedBytes $sourceOffset $targetOffset } } finally { $dstVerify.Dispose(); "
        "$srcVerify.Dispose() } }\n");
}

QString cloneTransferScript(const CloneTransferSpec& spec) {
    return cloneTransferPreludeScript(spec) + cloneTransferOpenFunctionsScript() +
           cloneTransferVerificationFunctionsScript() + cloneTransferRawTargetFunctionsScript() +
           cloneTransferExecutionScript() + cloneTransferVerifyExecutionScript();
}

QString osMigrationBootValidationScript() {
    return QStringLiteral(
        "Write-Output 'SAK OS migration boot validation'\n"
        "$targetDiskNumber = -1\n"
        "if ($dst.StartsWith('\\\\.\\PhysicalDrive')) { $suffix = "
        "$dst.Substring('\\\\.\\PhysicalDrive'.Length); [void][int]::TryParse($suffix, "
        "[ref]$targetDiskNumber) }\n"
        "if ($targetDiskNumber -lt 0) { Write-Warning 'Boot validation skipped: target is not a "
        "physical disk path'; return }\n"
        "$disk = Get-Disk -Number $targetDiskNumber -ErrorAction Stop\n"
        "$parts = @(Get-Partition -DiskNumber $targetDiskNumber -ErrorAction Stop | Sort-Object "
        "PartitionNumber)\n"
        "Write-Output (\"Target Disk {0}: Scheme={1}; Operational={2}; Health={3}\" -f "
        "$disk.Number, $disk.PartitionStyle, ($disk.OperationalStatus -join ','), "
        "$disk.HealthStatus)\n"
        "foreach ($part in $parts) { Write-Output (\"Partition {0}: Type={1}; GptType={2}; "
        "Size={3}; Drive={4}; Active={5}\" -f $part.PartitionNumber, $part.Type, $part.GptType, "
        "$part.Size, $part.DriveLetter, $part.IsActive) }\n"
        "if ($disk.PartitionStyle -eq 'GPT') { $esp = @($parts | Where-Object { $_.GptType -eq "
        "'{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}' -or $_.Type -eq 'System' }); if ($esp.Count -eq "
        "0) { throw 'UEFI boot validation failed: no EFI System Partition on target disk' }; $data "
        "= @($parts | Where-Object { $_.GptType -eq '{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}' -or "
        "$_.Type -eq 'Basic' }); if ($data.Count -eq 0) { throw 'UEFI boot validation failed: no "
        "Windows data partition candidate on target disk' }; Write-Output 'UEFI validation passed: "
        "EFI System Partition and Windows data candidate found' }\n"
        "elseif ($disk.PartitionStyle -eq 'MBR') { $active = @($parts | Where-Object { $_.IsActive "
        "}); if ($active.Count -eq 0) { throw 'BIOS boot validation failed: no active partition on "
        "target disk' }; Write-Output 'BIOS validation passed: active partition found' }\n"
        "else { throw 'Boot validation failed: target disk partition style is RAW or unknown' }\n"
        "Write-Output 'Next step: select the target disk in BIOS/UEFI boot order. If boot fails, "
        "run Partition Manager Boot Repair on the target Windows install.'\n");
}

struct PrimaryLogicalScriptPayload {
    QString target_layout;
    bool make_logical{false};
    uint64_t source_size{0};
    QString drive;
    QString file_system;
    QString label;
    QString backup_directory;
};

PrimaryLogicalScriptPayload primaryLogicalScriptPayload(const PartitionOperation& operation) {
    PrimaryLogicalScriptPayload payload;
    payload.target_layout =
        payloadString(operation, QStringLiteral("target_layout"), QStringLiteral("logical"))
            .trimmed()
            .toLower();
    payload.make_logical = payload.target_layout == QStringLiteral("logical");
    payload.source_size = payloadUInt64(operation, QStringLiteral("source_size_bytes"));
    payload.drive =
        payloadString(operation, QStringLiteral("drive_letter"), operation.target.drive_letter)
            .left(1)
            .toUpper();
    payload.file_system =
        payloadString(operation, QStringLiteral("file_system"), QStringLiteral("NTFS"))
            .trimmed()
            .toLower();
    payload.label = diskPartLabel(payloadString(
        operation,
        QStringLiteral("label"),
        payload.make_logical ? QStringLiteral("SAKLOGICAL") : QStringLiteral("SAKPRIMARY")));
    payload.backup_directory =
        payloadString(operation, QStringLiteral("backup_directory")).trimmed();
    return payload;
}

QString primaryLogicalPayloadError(const PrimaryLogicalScriptPayload& payload,
                                   const PartitionOperation& operation) {
    if (!payload.make_logical && payload.target_layout != QStringLiteral("primary")) {
        return QStringLiteral("Primary/logical conversion requires target_layout");
    }
    if (payload.source_size == 0 || !PartitionScriptBuilder::isValidDriveLetter(payload.drive)) {
        return QStringLiteral("Primary/logical conversion requires source size and drive letter");
    }
    if (!PartitionScriptBuilder::isSupportedFileSystem(payload.file_system)) {
        return QStringLiteral("Unsupported file system");
    }
    if (payload.backup_directory.isEmpty()) {
        return QStringLiteral("Primary/logical conversion requires backup_directory");
    }
    if (!payloadBool(operation, QStringLiteral("target_wipe_confirmed"))) {
        return QStringLiteral("Primary/logical conversion requires destructive confirmation");
    }
    return {};
}

struct MovePartitionScriptPayload {
    uint64_t target_offset{0};
    uint64_t target_size{0};
    QString drive;
    QString file_system;
    QString label;
    QString backup_directory;
};

MovePartitionScriptPayload movePartitionScriptPayload(const PartitionOperation& operation) {
    MovePartitionScriptPayload payload;
    payload.target_offset = payloadUInt64(operation, QStringLiteral("target_offset_bytes"));
    payload.target_size = payloadUInt64(operation, QStringLiteral("target_size_bytes"));
    payload.drive =
        payloadString(operation, QStringLiteral("drive_letter"), operation.target.drive_letter)
            .left(1)
            .toUpper();
    payload.file_system =
        payloadString(operation, QStringLiteral("file_system"), QStringLiteral("NTFS"))
            .trimmed()
            .toUpper();
    payload.label = payloadString(operation, QStringLiteral("label"), QStringLiteral("Data"));
    payload.backup_directory =
        payloadString(operation, QStringLiteral("backup_directory")).trimmed();
    return payload;
}

QString movePartitionPayloadError(const MovePartitionScriptPayload& payload,
                                  const PartitionOperation& operation) {
    if (payload.target_offset == 0 || payload.target_size == 0) {
        return QStringLiteral("Move Partition requires target offset and size");
    }
    if (!PartitionScriptBuilder::isValidDriveLetter(payload.drive)) {
        return QStringLiteral("Move Partition requires a mounted drive letter");
    }
    if (!PartitionScriptBuilder::isSupportedFileSystem(payload.file_system)) {
        return QStringLiteral("Unsupported file system");
    }
    if (payload.backup_directory.isEmpty()) {
        return QStringLiteral("Move Partition requires backup_directory");
    }
    if (!payloadBool(operation, QStringLiteral("target_wipe_confirmed"))) {
        return QStringLiteral("Move Partition requires destructive confirmation");
    }
    return {};
}

}  // namespace

struct ExternalFileSystemToolScriptRequest {
    QString operation_name;
    QString preview;
    QStringList accepted_exit_codes;
    QString pre_tool_script;
    PartitionFileSystemToolCommand command;
};

PartitionScript buildNativeResizeScript(const PartitionOperation& operation, uint64_t targetSize) {
    PartitionScript out;
    out.preview = QStringLiteral("Resize Disk %1 Partition %2 to %3")
                      .arg(operation.target.disk_number)
                      .arg(operation.target.partition_number)
                      .arg(formatPartitionBytes(targetSize));
    out.script = commonHeader(out.preview) +
                 requirePartitionIdentity(operation.target.disk_number,
                                          operation.target.partition_number,
                                          operation.target.size_bytes) +
                 QStringLiteral(
                     "$s = Get-PartitionSupportedSize -DiskNumber %1 -PartitionNumber %2\n"
                     "if ([uint64]%3 -lt [uint64]$s.SizeMin -or [uint64]%3 -gt [uint64]$s.SizeMax) "
                     "{ throw 'Target size outside supported range' }\n"
                     "Resize-Partition -DiskNumber %1 -PartitionNumber %2 -Size %3\n")
                     .arg(operation.target.disk_number)
                     .arg(operation.target.partition_number)
                     .arg(uintArg(targetSize));
    out.timeout_seconds = kPartitionFormatTaskTimeoutSeconds;
    return out;
}

QString growExtResizePreToolScript(const PartitionOperation& operation, uint64_t targetSize) {
    return QStringLiteral(
               "$targetSizeBytes = [uint64]%1\n"
               "Resize-Partition -DiskNumber %2 -PartitionNumber %3 -Size $targetSizeBytes\n"
               "Update-HostStorageCache -ErrorAction SilentlyContinue\n"
               "$p = Get-Partition -DiskNumber %2 -PartitionNumber %3 -ErrorAction Stop\n"
               "if ([uint64]$p.Size -ne $targetSizeBytes) { throw 'Partition resize did not reach "
               "target size' }\n")
        .arg(uintArg(targetSize))
        .arg(operation.target.disk_number)
        .arg(operation.target.partition_number);
}

ExternalFileSystemToolScriptRequest growExtResizeRequest(const PartitionOperation& operation,
                                                         const QString& fs,
                                                         const QString& targetPath,
                                                         uint64_t targetSize) {
    ExternalFileSystemToolScriptRequest request;
    request.operation_name = PartitionFileSystemToolRunner::resizeOperation();
    request.preview =
        QStringLiteral("Grow Disk %1 Partition %2 %3 file system with bundled resize2fs")
            .arg(operation.target.disk_number)
            .arg(operation.target.partition_number)
            .arg(fs.toLower());
    request.accepted_exit_codes = {QStringLiteral("0")};
    request.pre_tool_script = growExtResizePreToolScript(operation, targetSize);
    request.command = PartitionFileSystemToolRunner::buildResizeCommand(
        fs, targetPath, payloadBool(operation, QString::fromLatin1(kTargetWipeConfirmedPayload)));
    return request;
}

QString extShrinkPreToolScript(uint64_t targetSize) {
    return QStringLiteral(
               "$targetSizeBytes = [uint64]%1\n"
               "$volume = $null\n"
               "try { $volume = $p | Get-Volume -ErrorAction Stop } catch { }\n"
               "if ($volume -and $volume.DriveLetter) {\n"
               "  Dismount-Volume -DriveLetter $volume.DriveLetter -Force -ErrorAction Stop\n"
               "}\n")
        .arg(uintArg(targetSize));
}

QString extShrinkPartitionScript(const PartitionOperation& operation) {
    return QStringLiteral(
               "Resize-Partition -DiskNumber %1 -PartitionNumber %2 -Size $targetSizeBytes\n"
               "Update-HostStorageCache -ErrorAction SilentlyContinue\n"
               "$p = Get-Partition -DiskNumber %1 -PartitionNumber %2 -ErrorAction Stop\n"
               "if ([uint64]$p.Size -ne $targetSizeBytes) { throw 'Partition shrink did not "
               "reach target size' }\n")
        .arg(operation.target.disk_number)
        .arg(operation.target.partition_number);
}

PartitionScript buildExtShrinkResizeScript(const PartitionOperation& operation,
                                           const QString& fs,
                                           const QString& targetPath,
                                           uint64_t targetSize) {
    if (targetSize < kMinimumExtShrinkTargetBytes) {
        return invalidPartitionScript(QStringLiteral("Ext shrink target is too small"));
    }
    const QString manifestPath = runtimeFilesystemManifestPath();
    const QString toolsRoot = QFileInfo(manifestPath).absolutePath();
    const auto e2fsck = PartitionFileSystemToolRunner::resolveApprovedTool(
        manifestPath,
        toolsRoot,
        QStringLiteral("e2fsck"),
        PartitionFileSystemToolRunner::repairOperation(),
        fs.toLower());
    if (!e2fsck.ok) {
        return invalidPartitionScript(e2fsck.blockers.join(QStringLiteral("; ")));
    }
    const auto resize2fs = PartitionFileSystemToolRunner::resolveApprovedTool(
        manifestPath,
        toolsRoot,
        QStringLiteral("resize2fs"),
        PartitionFileSystemToolRunner::resizeOperation(),
        fs.toLower());
    if (!resize2fs.ok) {
        return invalidPartitionScript(resize2fs.blockers.join(QStringLiteral("; ")));
    }

    const QString resizeSizeArg = QStringLiteral("%1K").arg(uintArg(targetSize / kKilobyteBytes));
    PartitionScript out;
    out.preview =
        QStringLiteral("Shrink Disk %1 Partition %2 %3 file system with bundled resize2fs")
            .arg(operation.target.disk_number)
            .arg(operation.target.partition_number)
            .arg(fs.toLower());
    out.script = commonHeader(out.preview) +
                 requirePartitionIdentity(operation.target.disk_number,
                                          operation.target.partition_number,
                                          operation.target.size_bytes) +
                 filesystemToolFunctionScript() + extShrinkPreToolScript(targetSize) +
                 filesystemToolCallScript(QStringLiteral("e2fsck pre-shrink repair"),
                                          e2fsck,
                                          {QStringLiteral("-p"), QStringLiteral("-f"), targetPath},
                                          {QStringLiteral("0"), QStringLiteral("1")}) +
                 filesystemToolCallScript(QStringLiteral("resize2fs shrink"),
                                          resize2fs,
                                          {QStringLiteral("-p"), targetPath, resizeSizeArg},
                                          {QStringLiteral("0")}) +
                 extShrinkPartitionScript(operation) +
                 filesystemToolCallScript(QStringLiteral("e2fsck post-shrink read-only check"),
                                          e2fsck,
                                          {QStringLiteral("-n"), QStringLiteral("-f"), targetPath},
                                          {QStringLiteral("0")}) +
                 QStringLiteral("Update-HostStorageCache -ErrorAction SilentlyContinue\n");
    out.dry_run_script =
        out.preview + QStringLiteral("\n") +
        PartitionScriptBuilder::quotePowerShell(QDir::toNativeSeparators(resize2fs.tool_path)) +
        QStringLiteral(" -p ") + PartitionScriptBuilder::quotePowerShell(targetPath) +
        QStringLiteral(" ") + PartitionScriptBuilder::quotePowerShell(resizeSizeArg);
    out.timeout_seconds = kPartitionFormatTaskTimeoutSeconds;
    return out;
}

QString robocopyManifestFunctionsScript() {
    return QStringLiteral(
        "function Invoke-SakRobocopy([string]$from, [string]$to) {\n"
        "  robocopy.exe $from $to /MIR /COPYALL /DCOPY:DAT /XJ /R:1 /W:1\n"
        "  $code = $LASTEXITCODE\n"
        "  if ($code -gt 7) { exit $code }\n"
        "}\n"
        "function Get-SakFileManifest([string]$root) {\n"
        "  $rootFull = [System.IO.Path]::GetFullPath($root)\n"
        "  @(Get-ChildItem -LiteralPath $rootFull -Recurse -Force -File | ForEach-Object {\n"
        "    $relative = $_.FullName.Substring($rootFull.Length).TrimStart('\\')\n"
        "    $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash\n"
        "    [pscustomobject]@{ RelativePath = $relative; Length = [uint64]$_.Length; Hash = "
        "$hash }\n"
        "  } | Sort-Object RelativePath)\n"
        "}\n");
}

struct AllocateFreeSpacePayload {
    uint32_t source_partition{0};
    uint64_t source_size{0};
    uint64_t bytes_to_allocate{0};
    QString source_letter;
    QString source_file_system;
    QString source_label;
    QString backup_directory;
};

AllocateFreeSpacePayload allocateFreeSpacePayload(const PartitionOperation& operation) {
    AllocateFreeSpacePayload payload;
    payload.source_partition =
        static_cast<uint32_t>(payloadUInt64(operation, QStringLiteral("source_partition_number")));
    payload.source_size = payloadUInt64(operation, QStringLiteral("source_size_bytes"));
    payload.bytes_to_allocate = payloadUInt64(operation, QStringLiteral("bytes_to_allocate"));
    payload.source_letter =
        payloadString(operation, QStringLiteral("source_drive_letter")).left(1).toUpper();
    payload.source_file_system =
        payloadString(operation, QStringLiteral("source_file_system"), QStringLiteral("NTFS"))
            .trimmed()
            .toUpper();
    payload.source_label =
        payloadString(operation, QStringLiteral("source_label"), QStringLiteral("Data"));
    payload.backup_directory =
        payloadString(operation, QStringLiteral("backup_directory")).trimmed();
    return payload;
}

QString allocateFreeSpacePayloadError(const AllocateFreeSpacePayload& payload,
                                      const PartitionOperation& operation) {
    if (payload.source_partition == 0 || payload.source_size == 0 ||
        payload.bytes_to_allocate == 0) {
        return QStringLiteral("Allocate Free Space requires source partition, size, and bytes");
    }
    if (payload.bytes_to_allocate >= payload.source_size) {
        return QStringLiteral("Allocate Free Space must leave donor partition space");
    }
    if (!PartitionScriptBuilder::isValidDriveLetter(payload.source_letter)) {
        return QStringLiteral("Allocate Free Space requires donor drive letter");
    }
    if (!PartitionScriptBuilder::isSupportedFileSystem(payload.source_file_system)) {
        return QStringLiteral("Unsupported donor file system");
    }
    if (payload.backup_directory.isEmpty()) {
        return QStringLiteral("Allocate Free Space requires backup_directory");
    }
    if (!payloadBool(operation, QStringLiteral("target_wipe_confirmed"))) {
        return QStringLiteral("Allocate Free Space requires destructive confirmation");
    }
    return {};
}

QString allocateSetupScript(const PartitionOperation& operation,
                            const AllocateFreeSpacePayload& payload,
                            uint64_t targetSize,
                            uint64_t donorRemainingBytes) {
    return QStringLiteral(
               "$target = $p\n"
               "$source = Get-Partition -DiskNumber %1 -PartitionNumber %2 -ErrorAction Stop\n"
               "if ([uint64]$source.Size -ne [uint64]%3) { throw 'Donor partition identity "
               "mismatch' "
               "}\n"
               "if ([uint64]$source.Offset -ne ([uint64]$target.Offset + [uint64]$target.Size)) { "
               "throw 'Donor partition must be directly after target partition' }\n"
               "$sourceDrive = %4\n"
               "$sourceFileSystem = %5\n"
               "$sourceLabel = %6\n"
               "$bytesToAllocate = [uint64]%7\n"
               "$targetSizeBytes = [uint64]%8\n"
               "$donorRemainingBytes = [uint64]%9\n"
               "$backupRoot = %10\n"
               "$sourceRoot = ('{0}:\\' -f $sourceDrive)\n"
               "$backupRootFull = [System.IO.Path]::GetFullPath($backupRoot)\n"
               "$targetVolume = $target | Get-Volume -ErrorAction SilentlyContinue\n"
               "$targetRoot = if ($targetVolume -and $targetVolume.DriveLetter) { ('{0}:\\' -f "
               "$targetVolume.DriveLetter) } else { '' }\n"
               "if ($backupRootFull.StartsWith($sourceRoot, "
               "[System.StringComparison]::OrdinalIgnoreCase)) { throw 'Backup directory must not "
               "be "
               "on the donor volume' }\n"
               "if ($targetRoot -and $backupRootFull.StartsWith($targetRoot, "
               "[System.StringComparison]::OrdinalIgnoreCase)) { throw 'Backup directory must not "
               "be "
               "on the target volume' }\n"
               "if (-not (Test-Path -LiteralPath $backupRootFull -PathType Container)) { throw "
               "'Backup directory does not exist' }\n"
               "$backupPath = Join-Path $backupRootFull ('sak-allocate-backup-{0}' -f "
               "[guid]::NewGuid().ToString('N'))\n"
               "New-Item -ItemType Directory -Force -Path $backupPath | Out-Null\n")
        .arg(operation.target.disk_number)
        .arg(payload.source_partition)
        .arg(uintArg(payload.source_size))
        .arg(PartitionScriptBuilder::quotePowerShell(payload.source_letter),
             PartitionScriptBuilder::quotePowerShell(payload.source_file_system),
             PartitionScriptBuilder::quotePowerShell(payload.source_label),
             uintArg(payload.bytes_to_allocate),
             uintArg(targetSize),
             uintArg(donorRemainingBytes),
             PartitionScriptBuilder::quotePowerShell(
                 QDir::toNativeSeparators(payload.backup_directory)));
}

QString allocateExecutionScript(const PartitionOperation& operation,
                                const AllocateFreeSpacePayload& payload) {
    return QStringLiteral(
               "Write-Output ('Backing up donor {0} to {1}' -f $sourceRoot, $backupPath)\n"
               "Invoke-SakRobocopy $sourceRoot $backupPath\n"
               "$backupManifest = @(Get-SakFileManifest $backupPath)\n"
               "Remove-Partition -DiskNumber %1 -PartitionNumber %2 -Confirm:$false\n"
               "Resize-Partition -DiskNumber %1 -PartitionNumber %3 -Size $targetSizeBytes\n"
               "$newDonor = New-Partition -DiskNumber %1 -Size $donorRemainingBytes -DriveLetter "
               "$sourceDrive\n"
               "Format-Volume -Partition $newDonor -FileSystem $sourceFileSystem "
               "-NewFileSystemLabel "
               "$sourceLabel -Confirm:$false -Force\n"
               "Invoke-SakRobocopy $backupPath $sourceRoot\n"
               "$restoredManifest = @(Get-SakFileManifest $sourceRoot)\n"
               "$diff = @(Compare-Object -ReferenceObject $backupManifest -DifferenceObject "
               "$restoredManifest -Property RelativePath,Length,Hash)\n"
               "if ($diff.Count -gt 0) { $diff | Format-Table | Out-String | Write-Output; throw "
               "'Restored donor manifest differs from backup' }\n"
               "if ($targetVolume -and $targetVolume.DriveLetter) { Repair-Volume -DriveLetter "
               "$targetVolume.DriveLetter -Scan }\n"
               "Repair-Volume -DriveLetter $sourceDrive -Scan\n"
               "Get-Partition -DiskNumber %1 | Sort-Object Offset | Format-Table "
               "DiskNumber,PartitionNumber,DriveLetter,Offset,Size,Type -AutoSize\n")
        .arg(operation.target.disk_number)
        .arg(payload.source_partition)
        .arg(operation.target.partition_number);
}

struct ClusterSizePayload {
    QString drive;
    QString file_system;
    QString label;
    uint64_t allocation_unit{0};
    QString backup_directory;
};

ClusterSizePayload clusterSizePayload(const PartitionOperation& operation) {
    ClusterSizePayload payload;
    payload.drive =
        payloadString(operation, QStringLiteral("drive_letter"), operation.target.drive_letter)
            .left(1);
    payload.file_system =
        payloadString(operation, QStringLiteral("file_system"), QStringLiteral("NTFS"))
            .trimmed()
            .toUpper();
    payload.label = payloadString(operation, QStringLiteral("label"), QStringLiteral("Data"));
    payload.allocation_unit = payloadUInt64(operation, QStringLiteral("allocation_unit_bytes"));
    payload.backup_directory =
        payloadString(operation, QStringLiteral("backup_directory")).trimmed();
    return payload;
}

QString clusterSizePayloadError(const ClusterSizePayload& payload,
                                const PartitionOperation& operation) {
    if (!PartitionScriptBuilder::isValidDriveLetter(payload.drive)) {
        return QStringLiteral("Cluster-size change requires a drive letter");
    }
    if (!PartitionScriptBuilder::isSupportedFileSystem(payload.file_system)) {
        return QStringLiteral("Unsupported file system");
    }
    if (payload.allocation_unit == kAllocationUnitDefaultBytes ||
        !isSupportedAllocationUnitSize(payload.allocation_unit)) {
        return QStringLiteral(
            "Cluster-size change requires an explicit supported allocation unit size");
    }
    if (payload.backup_directory.isEmpty()) {
        return QStringLiteral("Cluster-size change requires backup_directory");
    }
    if (!payloadBool(operation, QStringLiteral("target_wipe_confirmed"))) {
        return QStringLiteral("Cluster-size change requires destructive confirmation");
    }
    return {};
}

QString clusterSetupScript(const ClusterSizePayload& payload) {
    return QStringLiteral(
               "$drive = %1\n"
               "$fileSystem = %2\n"
               "$allocationUnitBytes = [uint32]%3\n"
               "$label = %4\n"
               "$backupRoot = %5\n"
               "$targetRoot = ('{0}:\\' -f $drive)\n"
               "$targetDrive = ('{0}:' -f $drive)\n"
               "$backupRootFull = [System.IO.Path]::GetFullPath($backupRoot)\n"
               "$backupRootDrive = [System.IO.Path]::GetPathRoot($backupRootFull)\n"
               "if ([string]::IsNullOrWhiteSpace($backupRootDrive)) { throw 'Backup directory must "
               "be "
               "on a mounted volume' }\n"
               "if ($backupRootFull.StartsWith($targetRoot, "
               "[System.StringComparison]::OrdinalIgnoreCase)) { throw 'Backup directory must not "
               "be "
               "on the target volume' }\n"
               "if (-not (Test-Path -LiteralPath $backupRootFull -PathType Container)) { throw "
               "'Backup directory does not exist' }\n"
               "$backupPath = Join-Path $backupRootFull ('sak-cluster-backup-{0}' -f "
               "[guid]::NewGuid().ToString('N'))\n"
               "New-Item -ItemType Directory -Force -Path $backupPath | Out-Null\n")
        .arg(PartitionScriptBuilder::quotePowerShell(payload.drive.toUpper()),
             PartitionScriptBuilder::quotePowerShell(payload.file_system),
             uintArg(payload.allocation_unit),
             PartitionScriptBuilder::quotePowerShell(payload.label),
             PartitionScriptBuilder::quotePowerShell(
                 QDir::toNativeSeparators(payload.backup_directory)));
}

QString clusterExecutionScript() {
    return QStringLiteral(
        "Write-Output ('Backing up {0} to {1}' -f $targetRoot, $backupPath)\n"
        "Invoke-SakRobocopy $targetRoot $backupPath\n"
        "$backupManifest = @(Get-SakFileManifest $backupPath)\n"
        "Format-Volume -DriveLetter $drive -FileSystem $fileSystem -NewFileSystemLabel $label "
        "-AllocationUnitSize $allocationUnitBytes -Confirm:$false -Force\n"
        "Invoke-SakRobocopy $backupPath $targetRoot\n"
        "$restoredManifest = @(Get-SakFileManifest $targetRoot)\n"
        "$diff = @(Compare-Object -ReferenceObject $backupManifest -DifferenceObject "
        "$restoredManifest -Property RelativePath,Length,Hash)\n"
        "if ($diff.Count -gt 0) { $diff | Format-Table | Out-String | Write-Output; throw "
        "'Restored file manifest differs from backup' }\n"
        "Repair-Volume -DriveLetter $drive -Scan\n"
        "fsutil.exe fsinfo volumeinfo $targetDrive\n"
        "if ($fileSystem -eq 'NTFS') { fsutil.exe fsinfo ntfsinfo $targetDrive }\n");
}

bool isNativeRawDevicePath(const QString& path) {
    const QString trimmed = path.trimmed();
    return trimmed.startsWith(QStringLiteral("\\\\?\\")) ||
           trimmed.startsWith(QStringLiteral("\\\\.\\"));
}

bool isHfsToolFileSystem(const QString& fileSystem) {
    return fileSystem.compare(QStringLiteral("hfs+"), Qt::CaseInsensitive) == 0 ||
           fileSystem.compare(QStringLiteral("hfsx"), Qt::CaseInsensitive) == 0;
}

QString hfsCheckedToolFunctionScript() {
    return QStringLiteral(
        "function Invoke-SakCheckedHfsTool {\n"
        "  param([string]$Name, [string]$FilePath, [string]$ExpectedHash, "
        "[string[]]$Arguments, [int[]]$AcceptedExitCodes)\n"
        "  $actualHash = (Get-FileHash -LiteralPath $FilePath -Algorithm "
        "SHA256).Hash.ToLowerInvariant()\n"
        "  if ($actualHash -ne $ExpectedHash) { throw \"Filesystem tool hash mismatch: $Name\" }\n"
        "  Write-Output ('Running approved filesystem tool: {0} {1}' -f $FilePath, "
        "($Arguments -join ' '))\n"
        "  $oldPreference = $ErrorActionPreference\n"
        "  $ErrorActionPreference = 'Continue'\n"
        "  try {\n"
        "    $output = & $FilePath @Arguments 2>&1\n"
        "    $toolExitCode = [int]$LASTEXITCODE\n"
        "  } finally {\n"
        "    $ErrorActionPreference = $oldPreference\n"
        "  }\n"
        "  $output | ForEach-Object { Write-Output $_ }\n"
        "  Write-Output ('Filesystem tool exit code: {0}' -f $toolExitCode)\n"
        "  if ($AcceptedExitCodes -notcontains $toolExitCode) { exit $toolExitCode }\n"
        "  return $toolExitCode\n"
        "}\n");
}

QString hfsSparseFileFunctionsScript() {
    return QStringLiteral(
        "function New-SakSparseImageFile {\n"
        "  param([string]$Path, [uint64]$SizeBytes)\n"
        "  Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue\n"
        "  New-Item -ItemType File -Path $Path -Force | Out-Null\n"
        "  $sparseOutput = fsutil sparse setflag $Path 2>&1\n"
        "  if ($LASTEXITCODE -ne 0) { throw ('Failed to mark staged HFS image sparse: {0}' "
        "-f (($sparseOutput | Out-String).Trim())) }\n"
        "  $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, "
        "[System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)\n"
        "  try { $stream.SetLength([int64]$SizeBytes) } finally { $stream.Dispose() }\n"
        "}\n"
        "function Get-SakSparseAllocatedRanges {\n"
        "  param([string]$Path)\n"
        "  $output = fsutil sparse queryrange $Path 2>&1\n"
        "  if ($LASTEXITCODE -ne 0) { throw ('Failed to query staged HFS sparse ranges: {0}' "
        "-f (($output | Out-String).Trim())) }\n"
        "  $ranges = @()\n"
        "  foreach ($line in $output) {\n"
        "    if ($line -match 'Offset:\\s*(0x[0-9a-fA-F]+)\\s+Length:\\s*(0x[0-9a-fA-F]+)') {\n"
        "      $ranges += [pscustomobject]@{ offset = [Convert]::ToUInt64($Matches[1]."
        "Substring(2), 16); length = [Convert]::ToUInt64($Matches[2].Substring(2), 16) }\n"
        "    }\n"
        "  }\n"
        "  if ($ranges.Count -eq 0) { throw 'No allocated sparse ranges found after staged HFS "
        "operation.' }\n"
        "  return $ranges\n"
        "}\n");
}

QString hfsVolumeEndianFunctionScript() {
    return QStringLiteral(
        "function Read-SakBigEndianUInt16 {\n"
        "  param([byte[]]$Buffer, [int]$Offset)\n"
        "  return [uint16]((([uint16]$Buffer[$Offset]) -shl 8) -bor "
        "([uint16]$Buffer[$Offset + 1]))\n"
        "}\n"
        "function Read-SakBigEndianUInt32 {\n"
        "  param([byte[]]$Buffer, [int]$Offset)\n"
        "  return [uint32]((([uint32]$Buffer[$Offset]) -shl 24) -bor "
        "(([uint32]$Buffer[$Offset + 1]) -shl 16) -bor "
        "(([uint32]$Buffer[$Offset + 2]) -shl 8) -bor "
        "([uint32]$Buffer[$Offset + 3]))\n"
        "}\n"
        "function Test-SakPowerOfTwoUInt32 {\n"
        "  param([uint32]$Value)\n"
        "  return $Value -ne 0 -and (($Value -band ($Value - 1)) -eq 0)\n"
        "}\n");
}

QString hfsVolumeHeaderStagingFunctionScript() {
    return QStringLiteral(
        "function Get-SakHfsStagingSizeFromHeader {\n"
        "  param([System.IO.FileStream]$Stream, [uint64]$HeaderOffset, "
        "[uint64]$VolumeOffset, [uint64]$PartitionSizeBytes)\n"
        "  if ($HeaderOffset -gt [uint64]([int64]::MaxValue - 512)) { "
        "throw 'HFS volume header offset overflow.' }\n"
        "  [void]$Stream.Seek([int64]$HeaderOffset, [System.IO.SeekOrigin]::Begin)\n"
        "  $header = New-Object byte[] 512\n"
        "  $read = $Stream.Read($header, 0, $header.Length)\n"
        "  if ($read -ne $header.Length) { throw 'Unable to read complete HFS volume "
        "header.' }\n"
        "  $signature = [System.Text.Encoding]::ASCII.GetString($header, 0, 2)\n"
        "  if ($signature -ne 'H+' -and $signature -ne 'HX') { return $null }\n"
        "  $blockSize = Read-SakBigEndianUInt32 -Buffer $header -Offset 40\n"
        "  $totalBlocks = Read-SakBigEndianUInt32 -Buffer $header -Offset 44\n"
        "  if ($blockSize -lt 512 -or $blockSize -gt 1048576 -or "
        "-not (Test-SakPowerOfTwoUInt32 -Value $blockSize) -or $totalBlocks -eq 0) { "
        "throw 'Invalid HFS volume geometry in volume header.' }\n"
        "  if ([uint64]$blockSize -gt ([uint64]::MaxValue / [uint64]$totalBlocks)) { "
        "throw 'HFS volume geometry overflows 64-bit size.' }\n"
        "  $volumeBytes = [uint64]$blockSize * [uint64]$totalBlocks\n"
        "  if ($VolumeOffset -gt ([uint64]::MaxValue - $volumeBytes)) { "
        "throw 'HFS wrapped volume size overflows 64-bit size.' }\n"
        "  $stagingBytes = $VolumeOffset + $volumeBytes\n"
        "  if ($stagingBytes -lt 2048 -or $stagingBytes -gt $PartitionSizeBytes) { "
        "throw ('HFS logical staging size {0} is outside partition size {1}.' -f "
        "$stagingBytes, $PartitionSizeBytes) }\n"
        "  return [pscustomobject]@{ size = [uint64]$stagingBytes; signature = "
        "$signature; block_size = [uint64]$blockSize; total_blocks = "
        "[uint64]$totalBlocks; volume_offset = [uint64]$VolumeOffset }\n"
        "}\n");
}

QString hfsVolumeStagingResolverFunctionScript() {
    return QStringLiteral(
        "function Get-SakHfsVolumeStagingSizeBytes {\n"
        "  param([string]$RawTarget, [uint64]$PartitionSizeBytes)\n"
        "  if ($PartitionSizeBytes -lt 2048) { throw 'HFS partition is too small.' }\n"
        "  $stream = [System.IO.File]::Open($RawTarget, [System.IO.FileMode]::Open, "
        "[System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)\n"
        "  try {\n"
        "    $direct = Get-SakHfsStagingSizeFromHeader -Stream $stream -HeaderOffset "
        "1024 -VolumeOffset 0 -PartitionSizeBytes $PartitionSizeBytes\n"
        "    if ($null -ne $direct) {\n"
        "      return [uint64]$direct.size\n"
        "    }\n"
        "    [void]$stream.Seek(1024, [System.IO.SeekOrigin]::Begin)\n"
        "    $mdb = New-Object byte[] 512\n"
        "    $read = $stream.Read($mdb, 0, $mdb.Length)\n"
        "    if ($read -ne $mdb.Length) { throw 'Unable to read HFS wrapper header.' }\n"
        "    $mdbSignature = [System.Text.Encoding]::ASCII.GetString($mdb, 0, 2)\n"
        "    $embeddedSignature = [System.Text.Encoding]::ASCII.GetString($mdb, 0x7c, 2)\n"
        "    if ($mdbSignature -eq 'BD' -and "
        "($embeddedSignature -eq 'H+' -or $embeddedSignature -eq 'HX')) {\n"
        "      $allocationBlockSize = Read-SakBigEndianUInt32 -Buffer $mdb -Offset 0x14\n"
        "      $allocationStartSector = Read-SakBigEndianUInt16 -Buffer $mdb -Offset 0x1c\n"
        "      $extentStartBlock = Read-SakBigEndianUInt16 -Buffer $mdb -Offset 0x7e\n"
        "      $extentBlockCount = Read-SakBigEndianUInt16 -Buffer $mdb -Offset 0x80\n"
        "      if ($extentBlockCount -eq 0 -or $allocationBlockSize -lt 512 -or "
        "-not (Test-SakPowerOfTwoUInt32 -Value $allocationBlockSize)) { throw "
        "'Invalid HFS wrapper geometry.' }\n"
        "      $volumeOffset = ([uint64]$allocationStartSector * 512) + "
        "([uint64]$extentStartBlock * [uint64]$allocationBlockSize)\n"
        "      $wrapped = Get-SakHfsStagingSizeFromHeader -Stream $stream -HeaderOffset "
        "($volumeOffset + 1024) -VolumeOffset $volumeOffset "
        "-PartitionSizeBytes $PartitionSizeBytes\n"
        "      if ($null -eq $wrapped) { throw 'HFS wrapper points to missing HFS+ "
        "volume header.' }\n"
        "      return [uint64]$wrapped.size\n"
        "    }\n"
        "    throw 'Unable to locate a direct or wrapped HFS+/HFSX volume header on "
        "selected raw target.'\n"
        "  } finally {\n"
        "    $stream.Dispose()\n"
        "  }\n"
        "}\n");
}

QString hfsVolumeStagingSizeFunctionScript() {
    return hfsVolumeEndianFunctionScript() + hfsVolumeHeaderStagingFunctionScript() +
           hfsVolumeStagingResolverFunctionScript();
}

QString hfsRangeCopyFunctionsScript() {
    return QStringLiteral(
               "function Test-SakZeroBuffer {\n"
               "  param([byte[]]$Buffer, [int]$Count)\n"
               "  for ($i = 0; $i -lt $Count; $i += 1) {\n"
               "    if ($Buffer[$i] -ne 0) { return $false }\n"
               "  }\n"
               "  return $true\n"
               "}\n"
               "function Write-SakZeroRange {\n"
               "  param([System.IO.FileStream]$TargetStream, [uint64]$Offset, "
               "[uint64]$Length)\n"
               "  $zero = New-Object byte[] ([int][Math]::Min([uint64]%1, $Length))\n"
               "  [void]$TargetStream.Seek([int64]$Offset, [System.IO.SeekOrigin]::Begin)\n"
               "  $remaining = $Length\n"
               "  while ($remaining -gt 0) {\n"
               "    $chunk = [int][Math]::Min([uint64]$zero.Length, $remaining)\n"
               "    $TargetStream.Write($zero, 0, $chunk)\n"
               "    $remaining -= [uint64]$chunk\n"
               "  }\n"
               "}\n"
               "function Copy-SakFileRange {\n"
               "  param([System.IO.FileStream]$SourceStream, [System.IO.FileStream]$TargetStream, "
               "[uint64]$Offset, [uint64]$Length)\n"
               "  $buffer = New-Object byte[] ([int]%1)\n"
               "  [void]$SourceStream.Seek([int64]$Offset, [System.IO.SeekOrigin]::Begin)\n"
               "  [void]$TargetStream.Seek([int64]$Offset, [System.IO.SeekOrigin]::Begin)\n"
               "  $remaining = $Length\n"
               "  while ($remaining -gt 0) {\n"
               "    $chunk = [int][Math]::Min([uint64]$buffer.Length, $remaining)\n"
               "    $read = $SourceStream.Read($buffer, 0, $chunk)\n"
               "    if ($read -le 0) { throw 'Staged HFS image read ended before range copy "
               "completed.' }\n"
               "    $TargetStream.Write($buffer, 0, $read)\n"
               "    $remaining -= [uint64]$read\n"
               "  }\n"
               "}\n")
        .arg(uintArg(kHfsStagedCopyBufferBytes));
}

QString hfsSparseImageWritebackFunctionScript() {
    return QStringLiteral(
               "function Copy-SakSparseImageToRawTarget {\n"
               "  param([string]$ImagePath, [string]$RawTarget, [uint64]$TargetSizeBytes)\n"
               "  $ranges = @(Get-SakSparseAllocatedRanges -Path $ImagePath)\n"
               "  $source = [System.IO.File]::Open($ImagePath, [System.IO.FileMode]::Open, "
               "[System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)\n"
               "  $target = [System.IO.File]::Open($RawTarget, [System.IO.FileMode]::Open, "
               "[System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::ReadWrite)\n"
               "  $copiedBytes = [uint64]0\n"
               "  $clippedRanges = 0\n"
               "  try {\n"
               "    $edgeBytes = [uint64][Math]::Min([uint64]%1, $TargetSizeBytes)\n"
               "    Write-SakZeroRange -TargetStream $target -Offset 0 -Length $edgeBytes\n"
               "    if ($TargetSizeBytes -gt $edgeBytes) { Write-SakZeroRange -TargetStream "
               "$target -Offset ($TargetSizeBytes - $edgeBytes) -Length $edgeBytes }\n"
               "    foreach ($range in $ranges) {\n"
               "      if ([uint64]$range.offset -ge $TargetSizeBytes) { $clippedRanges += 1; "
               "continue }\n"
               "      $rangeLength = [uint64]$range.length\n"
               "      $maxLength = $TargetSizeBytes - [uint64]$range.offset\n"
               "      if ($rangeLength -gt $maxLength) { $rangeLength = $maxLength; "
               "$clippedRanges += 1 }\n"
               "      if ($rangeLength -eq 0) { continue }\n"
               "      Copy-SakFileRange -SourceStream $source -TargetStream $target -Offset "
               "$range.offset -Length $rangeLength\n"
               "      $copiedBytes += $rangeLength\n"
               "    }\n"
               "    $target.Flush()\n"
               "  } finally {\n"
               "    $source.Dispose()\n"
               "    $target.Dispose()\n"
               "  }\n"
               "  Write-Output ('Copied staged HFS image to raw target: ranges={0}; bytes={1}; "
               "clipped={2}' -f $ranges.Count, $copiedBytes, $clippedRanges)\n"
               "}\n")
        .arg(uintArg(kHfsStaleSignatureClearBytes));
}

QString hfsRawTargetStagingFunctionScript() {
    return QStringLiteral(
               "function Copy-SakRawTargetToImage {\n"
               "  param([string]$RawTarget, [string]$ImagePath, [uint64]$SizeBytes)\n"
               "  $source = [System.IO.File]::Open($RawTarget, [System.IO.FileMode]::Open, "
               "[System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)\n"
               "  $target = [System.IO.File]::Open($ImagePath, [System.IO.FileMode]::Open, "
               "[System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::ReadWrite)\n"
               "  $buffer = New-Object byte[] ([int]%1)\n"
               "  $copiedBytes = [uint64]0\n"
               "  try {\n"
               "    $remaining = $SizeBytes\n"
               "    while ($remaining -gt 0) {\n"
               "      $chunk = [int][Math]::Min([uint64]$buffer.Length, $remaining)\n"
               "      $read = $source.Read($buffer, 0, $chunk)\n"
               "      if ($read -le 0) { throw 'Raw HFS target read ended before staged copy "
               "completed.' }\n"
               "      if (-not (Test-SakZeroBuffer -Buffer $buffer -Count $read)) {\n"
               "        $target.Write($buffer, 0, $read)\n"
               "        $copiedBytes += [uint64]$read\n"
               "      } else {\n"
               "        [void]$target.Seek([int64]$read, [System.IO.SeekOrigin]::Current)\n"
               "      }\n"
               "      $remaining -= [uint64]$read\n"
               "    }\n"
               "    $target.Flush()\n"
               "  } finally {\n"
               "    $source.Dispose()\n"
               "    $target.Dispose()\n"
               "  }\n"
               "  Write-Output ('Copied raw HFS target into sparse staging image: "
               "nonzero_bytes={0}' "
               "-f $copiedBytes)\n"
               "}\n")
        .arg(uintArg(kHfsStagedCopyBufferBytes));
}

QString hfsRepairFunctionScript() {
    return QStringLiteral(
        "function Invoke-SakHfsRepairUntilClean {\n"
        "  param([string]$FsckPath, [string]$ExpectedHash, [string]$ImagePath, "
        "[string]$FileSystem, [switch]$AllowJournaledIncomplete)\n"
        "  $isHfsx = $FileSystem -ieq 'hfsx'\n"
        "  $allowIncomplete = $isHfsx -or [bool]$AllowJournaledIncomplete\n"
        "  $repairCodes = if ($allowIncomplete) { @(0, 8) } else { @(0) }\n"
        "  $checkCodes = if ($allowIncomplete) { @(0, 8) } else { @(0) }\n"
        "  [void](Invoke-SakCheckedHfsTool -Name 'fsck_hfs repair' -FilePath $FsckPath "
        "-ExpectedHash $ExpectedHash -Arguments @('-p', '-f', $ImagePath) "
        "-AcceptedExitCodes $repairCodes)\n"
        "  $checkExit = Invoke-SakCheckedHfsTool -Name 'fsck_hfs final check' "
        "-FilePath $FsckPath -ExpectedHash $ExpectedHash -Arguments @('-n', '-f', $ImagePath) "
        "-AcceptedExitCodes $checkCodes\n"
        "  if ($isHfsx -and [int]$checkExit -ne 0) {\n"
        "    [void](Invoke-SakCheckedHfsTool -Name 'fsck_hfs repair retry' -FilePath $FsckPath "
        "-ExpectedHash $ExpectedHash -Arguments @('-p', '-f', $ImagePath) "
        "-AcceptedExitCodes @(0))\n"
        "    [void](Invoke-SakCheckedHfsTool -Name 'fsck_hfs final retry check' "
        "-FilePath $FsckPath -ExpectedHash $ExpectedHash -Arguments @('-n', '-f', $ImagePath) "
        "-AcceptedExitCodes @(0))\n"
        "  } elseif ($AllowJournaledIncomplete -and [int]$checkExit -eq 8) {\n"
        "    Write-Output 'fsck_hfs returned journaled/incomplete verification code 8; "
        "continuing because HFS file mutation explicitly allowed journaled staging and "
        "post-copy read-back must still pass.'\n"
        "  } elseif ([int]$checkExit -ne 0) {\n"
        "    exit ([int]$checkExit)\n"
        "  }\n"
        "}\n");
}

QString hfsStagedFunctionsScript() {
    return hfsCheckedToolFunctionScript() + hfsSparseFileFunctionsScript() +
           hfsVolumeStagingSizeFunctionScript() + hfsRangeCopyFunctionsScript() +
           hfsSparseImageWritebackFunctionScript() + hfsRawTargetStagingFunctionScript() +
           hfsRepairFunctionScript();
}

QString hfsStagedImagePathScript(const QString& prefix) {
    return QStringLiteral(
               "$stagedImagePath = Join-Path ([System.IO.Path]::GetTempPath()) "
               "('%1-' + [guid]::NewGuid().ToString('N') + '.img')\n")
        .arg(prefix);
}

PartitionScript buildStagedHfsFormatScript(
    const PartitionOperation& operation,
    const ExternalFileSystemToolScriptRequest& request,
    const PartitionFileSystemToolResolution& formatResolution,
    const PartitionFileSystemToolResolution& fsckResolution,
    const QString& rawTargetPath) {
    PartitionScript out;
    out.preview = request.preview + QStringLiteral(" using sparse staging");
    const QStringList formatArgs = request.command.arguments;
    out.script =
        commonHeader(out.preview) +
        requirePartitionIdentity(operation.target.disk_number,
                                 operation.target.partition_number,
                                 operation.target.size_bytes) +
        hfsStagedFunctionsScript() +
        QStringLiteral(
            "$targetPath = %1\n"
            "$targetSizeBytes = [uint64]$p.Size\n")
            .arg(PartitionScriptBuilder::quotePowerShell(rawTargetPath)) +
        hfsStagedImagePathScript(QStringLiteral("sak-hfs-format")) +
        QStringLiteral(
            "try {\n"
            "  New-SakSparseImageFile -Path $stagedImagePath -SizeBytes $targetSizeBytes\n"
            "  $formatArgs = %1\n"
            "  [void](Invoke-SakCheckedHfsTool -Name 'newfs_hfs format' -FilePath %2 "
            "-ExpectedHash %3 -Arguments $formatArgs -AcceptedExitCodes @(0))\n"
            "  Invoke-SakHfsRepairUntilClean -FsckPath %4 -ExpectedHash %5 "
            "-ImagePath $stagedImagePath -FileSystem %6\n"
            "  Copy-SakSparseImageToRawTarget -ImagePath $stagedImagePath -RawTarget "
            "$targetPath -TargetSizeBytes $targetSizeBytes\n"
            "} finally {\n"
            "  Remove-Item -LiteralPath $stagedImagePath -Force -ErrorAction SilentlyContinue\n"
            "}\n"
            "Update-HostStorageCache -ErrorAction SilentlyContinue\n")
            .arg(powerShellArrayLiteralWithTrailingExpression(formatArgs,
                                                              QStringLiteral("$stagedImagePath")),
                 PartitionScriptBuilder::quotePowerShell(
                     QDir::toNativeSeparators(formatResolution.tool_path)),
                 PartitionScriptBuilder::quotePowerShell(
                     formatResolution.tool.binary_sha256.toLower()),
                 PartitionScriptBuilder::quotePowerShell(
                     QDir::toNativeSeparators(fsckResolution.tool_path)),
                 PartitionScriptBuilder::quotePowerShell(
                     fsckResolution.tool.binary_sha256.toLower()),
                 PartitionScriptBuilder::quotePowerShell(request.command.file_system));
    out.dry_run_script = out.preview + QStringLiteral("\nStage HFS image with ") +
                         PartitionScriptBuilder::quotePowerShell(formatResolution.tool_path) +
                         QStringLiteral("; copy sparse ranges to ") +
                         PartitionScriptBuilder::quotePowerShell(rawTargetPath);
    out.timeout_seconds = kPartitionLongTaskTimeoutSeconds;
    return out;
}

PartitionScript buildStagedHfsRepairScript(const PartitionOperation& operation,
                                           const ExternalFileSystemToolScriptRequest& request,
                                           const PartitionFileSystemToolResolution& fsckResolution,
                                           const QString& rawTargetPath) {
    PartitionScript out;
    out.preview = request.preview + QStringLiteral(" using sparse staging");
    out.script =
        commonHeader(out.preview) +
        requirePartitionIdentity(operation.target.disk_number,
                                 operation.target.partition_number,
                                 operation.target.size_bytes) +
        hfsStagedFunctionsScript() +
        QStringLiteral(
            "$targetPath = %1\n"
            "$targetSizeBytes = [uint64]$p.Size\n")
            .arg(PartitionScriptBuilder::quotePowerShell(rawTargetPath)) +
        hfsStagedImagePathScript(QStringLiteral("sak-hfs-repair")) +
        QStringLiteral(
            "try {\n"
            "  New-SakSparseImageFile -Path $stagedImagePath -SizeBytes $targetSizeBytes\n"
            "  Copy-SakRawTargetToImage -RawTarget $targetPath -ImagePath $stagedImagePath "
            "-SizeBytes $targetSizeBytes\n"
            "  Invoke-SakHfsRepairUntilClean -FsckPath %1 -ExpectedHash %2 "
            "-ImagePath $stagedImagePath -FileSystem %3\n"
            "  Copy-SakSparseImageToRawTarget -ImagePath $stagedImagePath -RawTarget "
            "$targetPath -TargetSizeBytes $targetSizeBytes\n"
            "} finally {\n"
            "  Remove-Item -LiteralPath $stagedImagePath -Force -ErrorAction SilentlyContinue\n"
            "}\n"
            "Update-HostStorageCache -ErrorAction SilentlyContinue\n")
            .arg(PartitionScriptBuilder::quotePowerShell(
                     QDir::toNativeSeparators(fsckResolution.tool_path)),
                 PartitionScriptBuilder::quotePowerShell(
                     fsckResolution.tool.binary_sha256.toLower()),
                 PartitionScriptBuilder::quotePowerShell(request.command.file_system));
    out.dry_run_script = out.preview + QStringLiteral("\nStage raw HFS target, repair with ") +
                         PartitionScriptBuilder::quotePowerShell(fsckResolution.tool_path) +
                         QStringLiteral(", copy sparse ranges back to ") +
                         PartitionScriptBuilder::quotePowerShell(rawTargetPath);
    out.timeout_seconds = kPartitionLongTaskTimeoutSeconds;
    return out;
}

bool isHfsFormatRequest(const ExternalFileSystemToolScriptRequest& request) {
    return request.operation_name == PartitionFileSystemToolRunner::formatOperation() &&
           request.command.tool_id == QStringLiteral("newfs_hfs");
}

bool isHfsRepairRequest(const ExternalFileSystemToolScriptRequest& request) {
    return request.operation_name == PartitionFileSystemToolRunner::repairOperation() &&
           request.command.tool_id == QStringLiteral("fsck_hfs");
}

PartitionFileSystemToolResolution resolveFsckHfsTool(const QString& manifestPath,
                                                     const QString& toolsRoot,
                                                     const QString& fileSystem) {
    return PartitionFileSystemToolRunner::resolveApprovedTool(
        manifestPath,
        toolsRoot,
        QStringLiteral("fsck_hfs"),
        PartitionFileSystemToolRunner::repairOperation(),
        fileSystem);
}

struct StagedRawHfsScriptRequest {
    const PartitionOperation* operation{nullptr};
    const ExternalFileSystemToolScriptRequest* request{nullptr};
    QString manifest_path;
    QString tools_root;
    const PartitionFileSystemToolResolution* resolution{nullptr};
    QString raw_target_path;
};

std::optional<PartitionScript> maybeBuildStagedRawHfsScript(
    const StagedRawHfsScriptRequest& input) {
    const auto& request = *input.request;
    if (!isNativeRawDevicePath(input.raw_target_path) ||
        !isHfsToolFileSystem(request.command.file_system)) {
        return std::nullopt;
    }
    if (isHfsRepairRequest(request)) {
        return buildStagedHfsRepairScript(
            *input.operation, request, *input.resolution, input.raw_target_path);
    }
    if (!isHfsFormatRequest(request)) {
        return std::nullopt;
    }

    const auto repairResolution =
        resolveFsckHfsTool(input.manifest_path, input.tools_root, request.command.file_system);
    if (!repairResolution.ok) {
        return invalidPartitionScript(repairResolution.blockers.join(QStringLiteral("; ")));
    }
    return buildStagedHfsFormatScript(
        *input.operation, request, *input.resolution, repairResolution, input.raw_target_path);
}

struct ExternalToolPostScript {
    QString script;
    QStringList blockers;
};

ExternalToolPostScript hfsPostFormatRepairScript(const ExternalFileSystemToolScriptRequest& request,
                                                 const QString& manifestPath,
                                                 const QString& toolsRoot) {
    if (!isHfsFormatRequest(request) || request.command.arguments.isEmpty()) {
        return {};
    }

    const auto repairResolution =
        resolveFsckHfsTool(manifestPath, toolsRoot, request.command.file_system);
    if (!repairResolution.ok) {
        return {.blockers = repairResolution.blockers};
    }

    const QStringList repairArguments = {QStringLiteral("-p"),
                                         QStringLiteral("-f"),
                                         request.command.arguments.constLast()};
    return {
        .script =
            QStringLiteral(
                "$postToolPath = %1\n"
                "$postExpectedHash = %2\n"
                "$postActualHash = (Get-FileHash -LiteralPath $postToolPath -Algorithm "
                "SHA256).Hash."
                "ToLowerInvariant()\n"
                "if ($postActualHash -ne $postExpectedHash) { throw 'Filesystem post-format tool "
                "hash mismatch' }\n"
                "$postToolArgs = %3\n"
                "Write-Output ('Running approved post-format filesystem tool: {0} {1}' -f "
                "$postToolPath, ($postToolArgs -join ' '))\n"
                "& $postToolPath @postToolArgs 2>&1 | ForEach-Object { Write-Output $_ }\n"
                "$postToolExitCode = $LASTEXITCODE\n"
                "Write-Output ('Post-format filesystem tool exit code: {0}' -f $postToolExitCode)\n"
                "if ($postToolExitCode -ne 0) { exit $postToolExitCode }\n")
                .arg(PartitionScriptBuilder::quotePowerShell(
                         QDir::toNativeSeparators(repairResolution.tool_path)),
                     PartitionScriptBuilder::quotePowerShell(
                         repairResolution.tool.binary_sha256.toLower()),
                     powerShellArrayLiteral(repairArguments))};
}

QString newPartitionCommandScript(const PartitionOperation& operation,
                                  const CreateScriptSpec& spec,
                                  const QString& driveArg) {
    return QStringLiteral("$p = New-Partition -DiskNumber %1 -Size %2%3%4%5\n")
        .arg(operation.target.disk_number)
        .arg(uintArg(spec.size))
        .arg(driveArg, partitionTypeArg(operation), createOffsetArg(operation));
}

QString refreshCreatedPartitionAndRawTargetScript(const PartitionOperation& operation,
                                                  const CreateScriptSpec& spec) {
    return QStringLiteral(
               "Update-HostStorageCache -ErrorAction SilentlyContinue\n"
               "$p = Get-Partition -DiskNumber %1 -PartitionNumber $p.PartitionNumber "
               "-ErrorAction Stop\n"
               "if ([uint64]$p.Size -lt [uint64]%2) { throw 'Created partition is smaller than "
               "requested' }\n"
               "$rawTargetPath = ('\\\\?\\GLOBALROOT\\Device\\Harddisk{0}\\Partition{1}' -f "
               "$p.DiskNumber, $p.PartitionNumber)\n"
               "Write-Output ('Created raw partition target: {0}' -f $rawTargetPath)\n")
        .arg(operation.target.disk_number)
        .arg(uintArg(spec.size));
}

PartitionScript buildCreateLinuxSwapScript(const PartitionOperation& operation,
                                           const CreateScriptSpec& spec) {
    const uint64_t pageSize = linuxSwapPageSize(operation);
    if (!isSupportedLinuxSwapPageSize(pageSize)) {
        return invalidPartitionScript(QStringLiteral("Unsupported Linux swap page size"));
    }
    const uint64_t pageCount = spec.size / pageSize;
    if (pageCount < kLinuxSwapMinPages || pageCount > kLinuxSwapMaxPages) {
        return invalidPartitionScript(
            QStringLiteral("Linux swap partition size is outside supported bounds"));
    }

    PartitionScript out;
    out.preview = QStringLiteral("Create %1 Linux swap partition on Disk %2")
                      .arg(formatPartitionBytes(spec.size),
                           QString::number(operation.target.disk_number));
    out.script = commonHeader(out.preview) + newPartitionCommandScript(operation, spec, QString()) +
                 refreshCreatedPartitionAndRawTargetScript(operation, spec) +
                 linuxSwapFormatBodyScript(QStringLiteral("$rawTargetPath"),
                                           QStringLiteral("[uint64]$p.Size"),
                                           spec.label,
                                           pageSize) +
                 QStringLiteral("$p | ConvertTo-Json -Compress\n");
    out.dry_run_script =
        out.preview +
        QStringLiteral("\nCreate partition, derive raw target, write SWAPSPACE2 v1 header");
    out.timeout_seconds = kPartitionFormatTaskTimeoutSeconds;
    return out;
}

PartitionScript buildCreateApfsScript(const PartitionOperation& operation,
                                      const CreateScriptSpec& spec) {
    PartitionScript out;
    out.preview = QStringLiteral("Create %1 APFS partition on Disk %2 with S.A.K. APFS writer")
                      .arg(formatPartitionBytes(spec.size),
                           QString::number(operation.target.disk_number));
    out.script = commonHeader(out.preview) + newPartitionCommandScript(operation, spec, QString()) +
                 refreshCreatedPartitionAndRawTargetScript(operation, spec) +
                 dismountSelectedPartitionVolumeScript() + apfsWriterCliFunctionScript() +
                 QStringLiteral(
                     "Invoke-SakApfsWriterCli -Command 'format-raw' -TargetPath $rawTargetPath "
                     "-SizeBytes ([uint64]$p.Size) -VolumeName %1 -EvidenceId "
                     "'ui.apfs-generated-raw-create-format'\n"
                     "Update-HostStorageCache -ErrorAction SilentlyContinue\n"
                     "$p | ConvertTo-Json -Compress\n")
                     .arg(PartitionScriptBuilder::quotePowerShell(spec.label));
    out.dry_run_script =
        out.preview +
        QStringLiteral("\nCreate partition, derive raw target, run APFS writer helper");
    out.timeout_seconds = kPartitionFormatTaskTimeoutSeconds;
    return out;
}

PartitionScript buildCreateExtScript(const PartitionOperation& operation,
                                     const CreateScriptSpec& spec,
                                     const PartitionFileSystemToolCommand& command,
                                     const PartitionFileSystemToolResolution& resolution) {
    PartitionScript out;
    out.preview = QStringLiteral("Create %1 %2 partition on Disk %3 with bundled %4")
                      .arg(formatPartitionBytes(spec.size),
                           spec.file_system,
                           QString::number(operation.target.disk_number),
                           command.tool_id);
    out.script = commonHeader(out.preview) + newPartitionCommandScript(operation, spec, QString()) +
                 refreshCreatedPartitionAndRawTargetScript(operation, spec) +
                 filesystemToolFunctionScript() +
                 QStringLiteral(
                     "$volume = $null\n"
                     "try { $volume = $p | Get-Volume -ErrorAction Stop } catch { }\n"
                     "if ($volume -and $volume.DriveLetter) {\n"
                     "  Dismount-Volume -DriveLetter $volume.DriveLetter -Force -ErrorAction Stop\n"
                     "}\n") +
                 filesystemToolCallScriptWithArgsExpression(
                     QStringLiteral("mke2fs create format"),
                     resolution,
                     powerShellArrayLiteralWithTrailingExpression(command.arguments,
                                                                  QStringLiteral("$rawTargetPath")),
                     {QStringLiteral("0")}) +
                 QStringLiteral(
                     "Update-HostStorageCache -ErrorAction SilentlyContinue\n"
                     "$p | ConvertTo-Json -Compress\n");
    out.dry_run_script = out.preview +
                         QStringLiteral("\nCreate partition, derive raw target, run ") +
                         PartitionScriptBuilder::quotePowerShell(resolution.tool_path);
    out.timeout_seconds = kPartitionFormatTaskTimeoutSeconds;
    return out;
}

PartitionScript buildCreateStagedHfsScript(
    const PartitionOperation& operation,
    const CreateScriptSpec& spec,
    const PartitionFileSystemToolCommand& command,
    const PartitionFileSystemToolResolution& formatResolution,
    const PartitionFileSystemToolResolution& fsckResolution) {
    PartitionScript out;
    out.preview = QStringLiteral("Create %1 %2 partition on Disk %3 with sparse-staged %4")
                      .arg(formatPartitionBytes(spec.size),
                           spec.file_system,
                           QString::number(operation.target.disk_number),
                           command.tool_id);
    const QStringList formatArgs = command.arguments;
    out.script =
        commonHeader(out.preview) + newPartitionCommandScript(operation, spec, QString()) +
        refreshCreatedPartitionAndRawTargetScript(operation, spec) + hfsStagedFunctionsScript() +
        QStringLiteral(
            "$targetPath = $rawTargetPath\n"
            "$targetSizeBytes = [uint64]$p.Size\n") +
        hfsStagedImagePathScript(QStringLiteral("sak-hfs-create-format")) +
        QStringLiteral(
            "try {\n"
            "  New-SakSparseImageFile -Path $stagedImagePath -SizeBytes $targetSizeBytes\n"
            "  $formatArgs = %1\n"
            "  [void](Invoke-SakCheckedHfsTool -Name 'newfs_hfs create format' -FilePath %2 "
            "-ExpectedHash %3 -Arguments $formatArgs -AcceptedExitCodes @(0))\n"
            "  Invoke-SakHfsRepairUntilClean -FsckPath %4 -ExpectedHash %5 "
            "-ImagePath $stagedImagePath -FileSystem %6\n"
            "  Copy-SakSparseImageToRawTarget -ImagePath $stagedImagePath -RawTarget "
            "$targetPath -TargetSizeBytes $targetSizeBytes\n"
            "} finally {\n"
            "  Remove-Item -LiteralPath $stagedImagePath -Force -ErrorAction SilentlyContinue\n"
            "}\n"
            "Update-HostStorageCache -ErrorAction SilentlyContinue\n"
            "$p | ConvertTo-Json -Compress\n")
            .arg(powerShellArrayLiteralWithTrailingExpression(formatArgs,
                                                              QStringLiteral("$stagedImagePath")),
                 PartitionScriptBuilder::quotePowerShell(
                     QDir::toNativeSeparators(formatResolution.tool_path)),
                 PartitionScriptBuilder::quotePowerShell(
                     formatResolution.tool.binary_sha256.toLower()),
                 PartitionScriptBuilder::quotePowerShell(
                     QDir::toNativeSeparators(fsckResolution.tool_path)),
                 PartitionScriptBuilder::quotePowerShell(
                     fsckResolution.tool.binary_sha256.toLower()),
                 PartitionScriptBuilder::quotePowerShell(command.file_system));
    out.dry_run_script =
        out.preview +
        QStringLiteral("\nCreate partition, stage HFS image, copy sparse ranges to raw target");
    out.timeout_seconds = kPartitionLongTaskTimeoutSeconds;
    return out;
}

PartitionScript buildCreateNonNativeFormatScript(const PartitionOperation& operation,
                                                 const CreateScriptSpec& spec) {
    if (isLinuxSwapFileSystemToken(spec.file_system)) {
        return buildCreateLinuxSwapScript(operation, spec);
    }
    if (isApfsFileSystemToken(spec.file_system)) {
        return buildCreateApfsScript(operation, spec);
    }

    constexpr auto kCreateRawTargetPlaceholder = "__SAK_NEW_PARTITION_RAW_TARGET__";
    const auto command = PartitionFileSystemToolRunner::buildFormatCommand(
        spec.file_system,
        QString::fromLatin1(kCreateRawTargetPlaceholder),
        spec.label,
        payloadBool(operation, QString::fromLatin1(kTargetWipeConfirmedPayload)));
    if (!command.ok()) {
        return invalidPartitionScript(command.blockers.join(QStringLiteral("; ")));
    }

    const QString manifestPath = runtimeFilesystemManifestPath();
    const QString toolsRoot = QFileInfo(manifestPath).absolutePath();
    const auto resolution = PartitionFileSystemToolRunner::resolveApprovedTool(
        manifestPath, toolsRoot, command.tool_id, command.operation, command.file_system);
    if (!resolution.ok) {
        return invalidPartitionScript(resolution.blockers.join(QStringLiteral("; ")));
    }

    if (isHfsToolFileSystem(command.file_system)) {
        const auto repairResolution =
            resolveFsckHfsTool(manifestPath, toolsRoot, command.file_system);
        if (!repairResolution.ok) {
            return invalidPartitionScript(repairResolution.blockers.join(QStringLiteral("; ")));
        }
        return buildCreateStagedHfsScript(operation, spec, command, resolution, repairResolution);
    }
    return buildCreateExtScript(operation, spec, command, resolution);
}

QHash<int, PartitionScriptBuilder::Builder> PartitionScriptBuilder::buildOperationDispatchTable() {
    QHash<int, Builder> builders;
    appendCoreBuilders(&builders);
    appendLayoutBuilders(&builders);
    appendCloneAndMaintenanceBuilders(&builders);
    appendAdvancedBuilders(&builders);
    return builders;
}

void PartitionScriptBuilder::appendCoreBuilders(QHash<int, Builder>* builders) {
    builders->insert(static_cast<int>(PartitionOperationType::Create), &buildCreateScript);
    builders->insert(static_cast<int>(PartitionOperationType::Delete), &buildDeleteScript);
    builders->insert(static_cast<int>(PartitionOperationType::Format), &buildFormatScript);
    builders->insert(static_cast<int>(PartitionOperationType::SetDriveLetter),
                     &buildSetDriveLetterScript);
    builders->insert(static_cast<int>(PartitionOperationType::SetPartitionLabel),
                     &buildSetPartitionLabelScript);
    builders->insert(static_cast<int>(PartitionOperationType::CheckFileSystem),
                     &buildCheckFileSystemScript);
    builders->insert(static_cast<int>(PartitionOperationType::SurfaceTest),
                     &buildSurfaceTestScript);
}

void PartitionScriptBuilder::appendLayoutBuilders(QHash<int, Builder>* builders) {
    builders->insert(static_cast<int>(PartitionOperationType::PartitionRecoveryScan),
                     &buildPartitionRecoveryScanScript);
    builders->insert(static_cast<int>(PartitionOperationType::RestoreRecoveredPartition),
                     &buildRestoreRecoveredPartitionScript);
    builders->insert(static_cast<int>(PartitionOperationType::SetPartitionHidden),
                     &buildSetPartitionHiddenScript);
    builders->insert(static_cast<int>(PartitionOperationType::SetPartitionActive),
                     &buildSetPartitionActiveScript);
    builders->insert(static_cast<int>(PartitionOperationType::SetPartitionTypeId),
                     &buildSetPartitionTypeIdScript);
    builders->insert(static_cast<int>(PartitionOperationType::InitializeDisk),
                     &buildInitializeDiskScript);
    builders->insert(static_cast<int>(PartitionOperationType::DeleteAllPartitions),
                     &buildDeleteAllPartitionsScript);
    builders->insert(static_cast<int>(PartitionOperationType::Resize), &buildResizeScript);
    builders->insert(static_cast<int>(PartitionOperationType::AllocateFreeSpace),
                     &buildAllocateFreeSpaceScript);
    builders->insert(static_cast<int>(PartitionOperationType::ConvertPartitionStyle),
                     &buildConvertStyleScript);
    builders->insert(static_cast<int>(PartitionOperationType::Merge), &buildMergeScript);
    builders->insert(static_cast<int>(PartitionOperationType::Split), &buildSplitScript);
}

void PartitionScriptBuilder::appendCloneAndMaintenanceBuilders(QHash<int, Builder>* builders) {
    for (const auto type : {PartitionOperationType::CloneDisk,
                            PartitionOperationType::ClonePartition,
                            PartitionOperationType::CreateImage,
                            PartitionOperationType::RestoreImage,
                            PartitionOperationType::MigrateOs}) {
        builders->insert(static_cast<int>(type), &buildCloneOrImageScript);
    }
    builders->insert(static_cast<int>(PartitionOperationType::RepairBoot), &buildBootRepairScript);
    builders->insert(static_cast<int>(PartitionOperationType::OptimizeSsd),
                     &buildOptimizeSsdScript);
    builders->insert(static_cast<int>(PartitionOperationType::DefragVolume),
                     &buildDefragVolumeScript);
    builders->insert(static_cast<int>(PartitionOperationType::ConvertFileSystem),
                     &buildConvertFileSystemScript);
    builders->insert(static_cast<int>(PartitionOperationType::ChangeClusterSize),
                     &buildChangeClusterSizeScript);
}

void PartitionScriptBuilder::appendAdvancedBuilders(QHash<int, Builder>* builders) {
    for (const auto type : {PartitionOperationType::BitLockerUnlock,
                            PartitionOperationType::BitLockerSuspend,
                            PartitionOperationType::BitLockerResume}) {
        builders->insert(static_cast<int>(type), &buildBitLockerScript);
    }
    for (const auto type : {PartitionOperationType::WipePartition,
                            PartitionOperationType::WipeDisk,
                            PartitionOperationType::WipeFreeSpace}) {
        builders->insert(static_cast<int>(type), &buildWipeScript);
    }
    builders->insert(static_cast<int>(PartitionOperationType::MovePartition),
                     &buildMovePartitionScript);
    builders->insert(static_cast<int>(PartitionOperationType::ConvertPrimaryLogical),
                     &buildConvertPrimaryLogicalScript);
    builders->insert(static_cast<int>(PartitionOperationType::ChangeVolumeSerialNumber),
                     &buildChangeVolumeSerialNumberScript);
    builders->insert(static_cast<int>(PartitionOperationType::ConvertDynamicDiskToBasic),
                     &buildConvertDynamicDiskToBasicScript);
    for (const auto type : {PartitionOperationType::ApfsWriteRootFile,
                            PartitionOperationType::ApfsPatchRootFile,
                            PartitionOperationType::ApfsPatchRootDirectoryFile,
                            PartitionOperationType::ApfsDeleteRootFile,
                            PartitionOperationType::ApfsWriteRootDirectoryFile,
                            PartitionOperationType::ApfsDeleteRootDirectoryFile,
                            PartitionOperationType::ApfsCreateRootDirectory,
                            PartitionOperationType::ApfsDeleteRootDirectory,
                            PartitionOperationType::ApfsChangeVolumeLabel}) {
        builders->insert(static_cast<int>(type), &buildApfsRootFileMutationScript);
    }
    for (const auto type : {PartitionOperationType::HfsOverwriteFile,
                            PartitionOperationType::HfsReplaceFile,
                            PartitionOperationType::HfsGrowFile,
                            PartitionOperationType::HfsTruncateFile,
                            PartitionOperationType::HfsReplaceResourceFork,
                            PartitionOperationType::HfsGrowResourceFork,
                            PartitionOperationType::HfsTruncateResourceFork,
                            PartitionOperationType::HfsCreateEmptyFile,
                            PartitionOperationType::HfsCreateFile,
                            PartitionOperationType::HfsDeleteEmptyFile,
                            PartitionOperationType::HfsDeleteFile,
                            PartitionOperationType::HfsCreateEmptyFolder,
                            PartitionOperationType::HfsDeleteEmptyFolder,
                            PartitionOperationType::HfsDeleteFolderTree,
                            PartitionOperationType::HfsRenameMoveCatalogEntry,
                            PartitionOperationType::HfsReplaceInlineAttribute,
                            PartitionOperationType::HfsReplaceForkAttribute,
                            PartitionOperationType::HfsGrowForkAttribute}) {
        builders->insert(static_cast<int>(type), &buildHfsFileMutationScript);
    }
}

PartitionScript PartitionScriptBuilder::buildScript(const PartitionOperation& operation) const {
    static const QHash<int, Builder> kBuilders = buildOperationDispatchTable();
    const auto builder = kBuilders.constFind(static_cast<int>(operation.type));
    return builder == kBuilders.constEnd()
               ? invalidScript(QStringLiteral("Unsupported partition operation"))
               : (this->*builder.value())(operation);
}

QString PartitionScriptBuilder::quotePowerShell(const QString& value) {
    return QStringLiteral("'") + QString(value).replace(QStringLiteral("'"), QStringLiteral("''")) +
           QStringLiteral("'");
}

bool PartitionScriptBuilder::isValidDriveLetter(const QString& value) {
    return value.size() == 1 && value.at(0).isLetter();
}

bool PartitionScriptBuilder::isSupportedFileSystem(const QString& value) {
    const QString fs = value.trimmed().toUpper();
    return fs == QStringLiteral("NTFS") || fs == QStringLiteral("FAT32") ||
           fs == QStringLiteral("EXFAT") || fs == QStringLiteral("REFS");
}

PartitionScript PartitionScriptBuilder::invalidScript(const QString& blocker) {
    PartitionScript script;
    script.blockers.append(blocker);
    return script;
}

PartitionScript PartitionScriptBuilder::buildCreateScript(
    const PartitionOperation& operation) const {
    const CreateScriptSpec spec = createScriptSpec(operation);
    const QString blocker = validateCreateScriptSpec(operation, spec);
    if (!blocker.isEmpty()) {
        return invalidScript(blocker);
    }
    if (isNonNativeFileSystemToolOperation(operation)) {
        return buildCreateNonNativeFormatScript(operation, spec);
    }

    const QString driveArg = isValidDriveLetter(spec.drive_letter)
                                 ? QStringLiteral("-DriveLetter %1").arg(spec.drive_letter)
                                 : QStringLiteral("-AssignDriveLetter");
    PartitionScript out;
    out.preview = QStringLiteral("Create %1 %2 partition on Disk %3")
                      .arg(formatPartitionBytes(spec.size),
                           spec.file_system,
                           QString::number(operation.target.disk_number));
    out.script = commonHeader(out.preview) +
                 newPartitionCommandScript(operation, spec, QStringLiteral(" ") + driveArg) +
                 QStringLiteral(
                     "Format-Volume -Partition $p -FileSystem %1 -NewFileSystemLabel %2%3%4 "
                     "-Confirm:$false\n"
                     "$p | ConvertTo-Json -Compress\n")
                     .arg(spec.file_system.toUpper(),
                          quotePowerShell(spec.label),
                          spec.full_format_arg,
                          allocationUnitArg(operation));
    return out;
}

PartitionScript PartitionScriptBuilder::buildDeleteScript(
    const PartitionOperation& operation) const {
    PartitionScript out;
    out.preview = QStringLiteral("Delete Disk %1 Partition %2")
                      .arg(operation.target.disk_number)
                      .arg(operation.target.partition_number);
    out.script = commonHeader(out.preview) +
                 requirePartitionIdentity(operation.target.disk_number,
                                          operation.target.partition_number,
                                          operation.target.size_bytes) +
                 QStringLiteral(
                     "Remove-Partition -DiskNumber %1 -PartitionNumber %2 "
                     "-Confirm:$false\n")
                     .arg(operation.target.disk_number)
                     .arg(operation.target.partition_number);
    return out;
}

PartitionScript PartitionScriptBuilder::buildFormatScript(
    const PartitionOperation& operation) const {
    const QString fs =
        payloadString(operation, QStringLiteral("file_system"), QStringLiteral("NTFS"));
    const QString label = payloadString(operation, QStringLiteral("label"), QStringLiteral("Data"));
    if (isNonNativeFileSystemToolOperation(operation)) {
        const QString targetBlocker = rawPartitionTargetPathBlocker(operation);
        if (!targetBlocker.isEmpty()) {
            return invalidScript(targetBlocker);
        }
        if (isLinuxSwapFileSystemToken(fs)) {
            return buildLinuxSwapFormatScript(operation, label);
        }
        if (isApfsFileSystemToken(fs)) {
            return buildApfsRawFormatScript(operation, label);
        }
        const QString targetPath = rawPartitionTargetPath(operation);
        const auto command = PartitionFileSystemToolRunner::buildFormatCommand(
            fs,
            targetPath,
            label,
            payloadBool(operation, QString::fromLatin1(kTargetWipeConfirmedPayload)));
        ExternalFileSystemToolScriptRequest request;
        request.operation_name = PartitionFileSystemToolRunner::formatOperation();
        request.preview = QStringLiteral("Format Disk %1 Partition %2 as %3 with bundled %4")
                              .arg(operation.target.disk_number)
                              .arg(operation.target.partition_number)
                              .arg(fs.toLower(), command.tool_id);
        request.accepted_exit_codes = {QStringLiteral("0")};
        request.command = command;
        return buildExternalFileSystemToolScript(operation, request);
    }
    const QString fullArg = payloadBool(operation, QStringLiteral("full_format"))
                                ? QStringLiteral(" -Full")
                                : QString();
    const uint64_t allocationUnit = payloadUInt64(operation,
                                                  QStringLiteral("allocation_unit_bytes"));
    if (operation.target.partition_number == 0 || operation.target.size_bytes == 0) {
        return invalidScript(QStringLiteral("Format requires a partition identity"));
    }
    if (!isSupportedFileSystem(fs)) {
        return invalidScript(QStringLiteral("Unsupported file system"));
    }
    if (!isSupportedAllocationUnitSize(allocationUnit)) {
        return invalidScript(QStringLiteral("Unsupported allocation unit size"));
    }

    PartitionScript out;
    out.preview = QStringLiteral("Format Disk %1 Partition %2 as %3")
                      .arg(operation.target.disk_number)
                      .arg(operation.target.partition_number)
                      .arg(fs.toUpper());
    out.script =
        commonHeader(out.preview) +
        requirePartitionIdentity(operation.target.disk_number,
                                 operation.target.partition_number,
                                 operation.target.size_bytes) +
        QStringLiteral(
            "$p | Format-Volume -FileSystem %1 -NewFileSystemLabel %2%3%4 "
            "-Confirm:$false\n")
            .arg(fs.toUpper(), quotePowerShell(label), fullArg, allocationUnitArg(operation));
    out.timeout_seconds = kPartitionFormatTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildSetDriveLetterScript(
    const PartitionOperation& operation) const {
    const QString letter = payloadString(operation, QStringLiteral("new_drive_letter")).left(1);
    if (!isValidDriveLetter(letter)) {
        return invalidScript(QStringLiteral("Set drive letter requires new_drive_letter"));
    }

    PartitionScript out;
    out.preview = QStringLiteral("Set Disk %1 Partition %2 drive letter to %3:")
                      .arg(operation.target.disk_number)
                      .arg(operation.target.partition_number)
                      .arg(letter.toUpper());
    out.script = commonHeader(out.preview) +
                 requirePartitionIdentity(operation.target.disk_number,
                                          operation.target.partition_number,
                                          operation.target.size_bytes) +
                 QStringLiteral(
                     "Set-Partition -DiskNumber %1 -PartitionNumber %2 "
                     "-NewDriveLetter %3\n")
                     .arg(operation.target.disk_number)
                     .arg(operation.target.partition_number)
                     .arg(letter.toUpper());
    return out;
}

PartitionScript PartitionScriptBuilder::buildSetPartitionLabelScript(
    const PartitionOperation& operation) const {
    const QString label = payloadString(operation, QStringLiteral("label"));
    const QString letter = operation.target.drive_letter.left(1);
    if (!isValidDriveLetter(letter)) {
        return invalidScript(QStringLiteral("Set label requires a mounted drive letter"));
    }

    PartitionScript out;
    out.preview = QStringLiteral("Set Disk %1 Partition %2 label to %3")
                      .arg(operation.target.disk_number)
                      .arg(operation.target.partition_number)
                      .arg(label.isEmpty() ? QStringLiteral("(blank)") : label);
    out.script = commonHeader(out.preview) +
                 requirePartitionIdentity(operation.target.disk_number,
                                          operation.target.partition_number,
                                          operation.target.size_bytes) +
                 QStringLiteral("Set-Volume -DriveLetter %1 -NewFileSystemLabel %2\n")
                     .arg(letter.toUpper(), quotePowerShell(label));
    return out;
}

PartitionScript PartitionScriptBuilder::buildCheckFileSystemScript(
    const PartitionOperation& operation) const {
    if (isNonNativeFileSystemToolOperation(operation)) {
        const QString targetBlocker = rawPartitionTargetPathBlocker(operation);
        if (!targetBlocker.isEmpty()) {
            return invalidScript(targetBlocker);
        }
        const QString fs =
            payloadString(operation, QStringLiteral("file_system"), QStringLiteral("ext4"));
        if (isApfsFileSystemToken(fs)) {
            return buildApfsRawRepairScript(operation, fs);
        }
        const QString targetPath = rawPartitionTargetPath(operation);
        const auto command = PartitionFileSystemToolRunner::buildRepairCommand(
            fs,
            targetPath,
            payloadBool(operation, QString::fromLatin1(kTargetWipeConfirmedPayload)));
        ExternalFileSystemToolScriptRequest request;
        request.operation_name = PartitionFileSystemToolRunner::repairOperation();
        request.preview =
            QStringLiteral("Repair Disk %1 Partition %2 %3 file system with bundled %4")
                .arg(operation.target.disk_number)
                .arg(operation.target.partition_number)
                .arg(fs.toLower(), command.tool_id);
        request.accepted_exit_codes = command.tool_id == QStringLiteral("e2fsck")
                                          ? QStringList{QStringLiteral("0"), QStringLiteral("1")}
                                          : QStringList{QStringLiteral("0")};
        request.command = command;
        return buildExternalFileSystemToolScript(operation, request);
    }
    const QString letter = operation.target.drive_letter.left(1);
    if (!isValidDriveLetter(letter)) {
        return invalidScript(QStringLiteral("File-system check requires a drive letter"));
    }

    PartitionScript out;
    out.preview = QStringLiteral("Check file system on %1:").arg(letter.toUpper());
    out.script =
        commonHeader(out.preview) +
        QStringLiteral("Repair-Volume -DriveLetter %1 -Scan -Verbose\n").arg(letter.toUpper());
    out.timeout_seconds = kPartitionMediumTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildApfsRootFileMutationScript(
    const PartitionOperation& operation) const {
    const auto input = apfsRootFileMutationScriptInput(operation);
    if (!input.blocker.isEmpty()) {
        return invalidScript(input.blocker);
    }

    const QString payloadScript = apfsRootFilePayloadScript(input.payload_base64);
    PartitionScript out;
    const QString targetNoun =
        apfsVolumeLabelMutation(operation.type)
            ? QStringLiteral("volume label")
            : (apfsRootDirectoryFileMutation(operation.type)
                   ? QStringLiteral("root directory file")
                   : (apfsRootDirectoryMutation(operation.type) ? QStringLiteral("root directory")
                                                                : QStringLiteral("root file")));
    out.preview = QStringLiteral("%1 APFS generated %2 %3 on Disk %4 Partition %5")
                      .arg(apfsRootFileMutationVerb(operation.type),
                           targetNoun,
                           apfsRootFileMutationDisplayName(input),
                           QString::number(operation.target.disk_number),
                           QString::number(operation.target.partition_number));
    out.script = commonHeader(out.preview) +
                 requirePartitionIdentity(operation.target.disk_number,
                                          operation.target.partition_number,
                                          operation.target.size_bytes) +
                 dismountSelectedPartitionVolumeScript() + apfsWriterCliFunctionScript() +
                 QStringLiteral(
                     "$targetPath = %1\n"
                     "$targetSizeBytes = [uint64]$p.Size\n")
                     .arg(quotePowerShell(rawPartitionTargetPath(operation))) +
                 payloadScript + apfsRootFilePayloadTryPrefix(payloadScript) +
                 apfsRootFileMutationInvoke(operation, input) +
                 apfsRootFileCleanupScript(input.payload_base64) +
                 QStringLiteral("Update-HostStorageCache -ErrorAction SilentlyContinue\n");
    out.dry_run_script = out.preview + QStringLiteral("\nRun sak_apfs_writer_cli.exe ") +
                         input.command + QStringLiteral(" ") +
                         quotePowerShell(rawPartitionTargetPath(operation));
    out.timeout_seconds = kPartitionFormatTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildHfsFileMutationScript(
    const PartitionOperation& operation) const {
    const auto input = hfsFileMutationScriptInput(operation);
    if (!input.blocker.isEmpty()) {
        return invalidScript(input.blocker);
    }

    const QString manifestPath = runtimeFilesystemManifestPath();
    const QString toolsRoot = QFileInfo(manifestPath).absolutePath();
    const auto fsckResolution = resolveFsckHfsTool(manifestPath, toolsRoot, input.file_system);
    if (!fsckResolution.ok) {
        return invalidScript(fsckResolution.blockers.join(QStringLiteral("; ")));
    }

    const QString payloadScript = hfsPayloadScript(input.payload_base64);
    PartitionScript out;
    out.preview = QStringLiteral("%1 HFS+ staged file mutation on Disk %2 Partition %3")
                      .arg(hfsFileMutationVerb(operation.type),
                           QString::number(operation.target.disk_number),
                           QString::number(operation.target.partition_number));
    out.script =
        commonHeader(out.preview) +
        requirePartitionIdentity(operation.target.disk_number,
                                 operation.target.partition_number,
                                 operation.target.size_bytes) +
        hfsStagedFunctionsScript() + hfsWriterCliFunctionScript() +
        QStringLiteral(
            "$targetPath = %1\n"
            "$targetPartitionSizeBytes = [uint64]$p.Size\n"
            "$targetSizeBytes = Get-SakHfsVolumeStagingSizeBytes -RawTarget $targetPath "
            "-PartitionSizeBytes $targetPartitionSizeBytes\n"
            "Write-Output ('HFS file mutation staging bytes: {0}; partition bytes: {1}' "
            "-f $targetSizeBytes, $targetPartitionSizeBytes)\n")
            .arg(quotePowerShell(rawPartitionTargetPath(operation))) +
        hfsStagedImagePathScript(QStringLiteral("sak-hfs-file-mutation")) + payloadScript +
        QStringLiteral(
            "try {\n"
            "  New-SakSparseImageFile -Path $stagedImagePath -SizeBytes $targetSizeBytes\n"
            "  Copy-SakRawTargetToImage -RawTarget $targetPath -ImagePath $stagedImagePath "
            "-SizeBytes $targetSizeBytes\n") +
        hfsFileMutationInvoke(operation, input) +
        QStringLiteral(
            "  Invoke-SakHfsRepairUntilClean -FsckPath %1 -ExpectedHash %2 "
            "-ImagePath $stagedImagePath -FileSystem %3%4\n"
            "  Copy-SakSparseImageToRawTarget -ImagePath $stagedImagePath -RawTarget "
            "$targetPath -TargetSizeBytes $targetSizeBytes\n"
            "} finally {\n")
            .arg(quotePowerShell(QDir::toNativeSeparators(fsckResolution.tool_path)),
                 quotePowerShell(fsckResolution.tool.binary_sha256.toLower()),
                 quotePowerShell(input.file_system),
                 input.allow_journaled ? QStringLiteral(" -AllowJournaledIncomplete") : QString()) +
        hfsPayloadCleanupScript(input.payload_base64) +
        QStringLiteral(
            "  Remove-Item -LiteralPath $stagedImagePath -Force -ErrorAction SilentlyContinue\n"
            "}\n"
            "Update-HostStorageCache -ErrorAction SilentlyContinue\n");
    out.dry_run_script =
        out.preview + QStringLiteral("\nStage raw HFS target, run sak_hfs_writer_cli.exe ") +
        input.command + QStringLiteral(", repair/check, copy sparse ranges back to ") +
        quotePowerShell(rawPartitionTargetPath(operation));
    out.timeout_seconds = kPartitionLongTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildExternalFileSystemToolScript(
    const PartitionOperation& operation, const ExternalFileSystemToolScriptRequest& request) const {
    if (operation.target.partition_number == 0 || operation.target.size_bytes == 0) {
        return invalidScript(
            QStringLiteral("Filesystem tool operation requires a partition identity"));
    }
    if (!request.command.ok()) {
        return invalidScript(request.command.blockers.join(QStringLiteral("; ")));
    }

    const QString manifestPath = runtimeFilesystemManifestPath();
    const QString toolsRoot = QFileInfo(manifestPath).absolutePath();
    const auto resolution =
        PartitionFileSystemToolRunner::resolveApprovedTool(manifestPath,
                                                           toolsRoot,
                                                           request.command.tool_id,
                                                           request.operation_name,
                                                           request.command.file_system);
    if (!resolution.ok) {
        return invalidScript(resolution.blockers.join(QStringLiteral("; ")));
    }

    const QString rawTargetPath = payloadString(operation, QStringLiteral("target_path")).trimmed();
    if (const auto hfsScript = maybeBuildStagedRawHfsScript(
            {&operation, &request, manifestPath, toolsRoot, &resolution, rawTargetPath})) {
        return *hfsScript;
    }

    const auto postTool = hfsPostFormatRepairScript(request, manifestPath, toolsRoot);
    if (!postTool.blockers.isEmpty()) {
        return invalidScript(postTool.blockers.join(QStringLiteral("; ")));
    }

    PartitionScript out;
    out.preview = request.preview;
    out.script = commonHeader(out.preview) +
                 requirePartitionIdentity(operation.target.disk_number,
                                          operation.target.partition_number,
                                          operation.target.size_bytes) +
                 request.pre_tool_script + filesystemToolFunctionScript() +
                 dismountSelectedPartitionVolumeScript() +
                 filesystemToolCallScript(request.command.tool_id,
                                          resolution,
                                          request.command.arguments,
                                          request.accepted_exit_codes) +
                 postTool.script +
                 QStringLiteral("Update-HostStorageCache -ErrorAction SilentlyContinue\n");
    out.dry_run_script = out.preview + QStringLiteral("\n") +
                         quotePowerShell(resolution.tool_path) + QStringLiteral(" ") +
                         request.command.arguments.join(QLatin1Char(' '));
    out.timeout_seconds = kPartitionFormatTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildSurfaceTestScript(
    const PartitionOperation& operation) const {
    PartitionScript out;
    if (operation.target.kind == PartitionTargetKind::Disk) {
        out.preview = QStringLiteral("Run read-only surface and health test for Disk %1")
                          .arg(operation.target.disk_number);
        out.script =
            commonHeader(out.preview) +
            QStringLiteral(
                "$disk = Get-Disk -Number %1 -ErrorAction Stop\n"
                "$physical = Get-PhysicalDisk | Where-Object { $_.DeviceId -eq $disk.Number } | "
                "Select-Object -First 1\n"
                "if ($physical) {\n"
                "  $physical | Get-StorageReliabilityCounter -ErrorAction SilentlyContinue | "
                "Format-List | Out-String | Write-Output\n"
                "}\n"
                "Get-Partition -DiskNumber %1 | Where-Object DriveLetter | ForEach-Object {\n"
                "  Write-Output \"Scanning volume $($_.DriveLetter):\"\n"
                "  Repair-Volume -DriveLetter $_.DriveLetter -Scan -Verbose\n"
                "}\n")
                .arg(operation.target.disk_number);
        out.timeout_seconds = kPartitionLongTaskTimeoutSeconds;
        return out;
    }

    const QString letter = operation.target.drive_letter.left(1);
    if (!isValidDriveLetter(letter)) {
        return invalidScript(QStringLiteral("Partition surface test requires a drive letter"));
    }
    out.preview = QStringLiteral("Run online surface test on %1:").arg(letter.toUpper());
    out.script =
        commonHeader(out.preview) +
        QStringLiteral("chkdsk.exe %1: /scan /perf\nexit $LASTEXITCODE\n").arg(letter.toUpper());
    out.timeout_seconds = kPartitionLongTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildPartitionRecoveryScanScript(
    const PartitionOperation& operation) const {
    if (operation.target.kind != PartitionTargetKind::Disk) {
        return invalidScript(QStringLiteral("Partition recovery scan requires a disk target"));
    }
    const QString mode =
        payloadString(operation, QStringLiteral("scan_mode"), QStringLiteral("Quick"))
            .trimmed()
            .toLower();
    const bool fullScan = mode == QStringLiteral("full");

    PartitionScript out;
    out.preview = QStringLiteral("%1 partition recovery scan on Disk %2")
                      .arg(fullScan ? QStringLiteral("Full") : QStringLiteral("Quick"))
                      .arg(operation.target.disk_number);
    out.script =
        commonHeader(out.preview) +
        QStringLiteral(
            "$disk = Get-Disk -Number %1 -ErrorAction Stop\n"
            "$path = '\\\\.\\PhysicalDrive%1'\n"
            "$limit = [uint64]$disk.Size\n"
            "if (-not %2) { $limit = [Math]::Min([uint64]$disk.Size, [uint64]137438953472) }\n"
            "$step = [uint64]1048576\n"
            "$buffer = New-Object byte[] 512\n"
            "$stream = [System.IO.File]::Open($path, [System.IO.FileMode]::Open, "
            "[System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)\n"
            "try {\n"
            "  for ($offset = [uint64]0; $offset -lt $limit; $offset += $step) {\n"
            "    [void]$stream.Seek([int64]$offset, [System.IO.SeekOrigin]::Begin)\n"
            "    [void]$stream.Read($buffer, 0, $buffer.Length)\n"
            "    $oem = [System.Text.Encoding]::ASCII.GetString($buffer, 3, 8)\n"
            "    if ($oem -match 'NTFS|FAT32|FAT16|EXFAT') {\n"
            "      Write-Output \"Candidate partition boot sector at byte offset $offset ($oem)\"\n"
            "    }\n"
            "  }\n"
            "} finally { $stream.Dispose() }\n")
            .arg(operation.target.disk_number)
            .arg(fullScan ? QStringLiteral("$true") : QStringLiteral("$false"));
    out.timeout_seconds = kPartitionLongTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildRestoreRecoveredPartitionScript(
    const PartitionOperation& operation) const {
    const uint64_t offset = payloadUInt64(operation, QStringLiteral("offset_bytes"));
    const uint64_t size = payloadUInt64(operation, QStringLiteral("size_bytes"));
    const QString typeId = payloadString(operation, QStringLiteral("type_id")).trimmed();
    if (operation.target.kind != PartitionTargetKind::Disk || offset == 0 || size == 0 ||
        typeId.isEmpty()) {
        return invalidScript(
            QStringLiteral("Recovered partition restore requires disk, offset, size, and type_id"));
    }

    const bool gpt = payloadString(operation, QStringLiteral("partition_style"))
                         .compare(QStringLiteral("GPT"), Qt::CaseInsensitive) == 0;
    const QString typeArg = gpt ? QStringLiteral("-GptType %1").arg(quotePowerShell(typeId))
                                : QStringLiteral("-MbrType %1").arg(quotePowerShell(typeId));
    PartitionScript out;
    out.preview = QStringLiteral("Restore recovered partition on Disk %1 at %2")
                      .arg(operation.target.disk_number)
                      .arg(formatPartitionBytes(offset));
    out.script = commonHeader(out.preview) +
                 QStringLiteral(
                     "$disk = Get-Disk -Number %1 -ErrorAction Stop\n"
                     "if ($disk.IsReadOnly) { throw 'Target disk is read-only' }\n"
                     "$existing = @(Get-Partition -DiskNumber %1 -ErrorAction Stop)\n"
                     "$start = [uint64]%2\n"
                     "$end = [uint64](%2 + %3)\n"
                     "if ($start -gt [uint64]$disk.Size -or [uint64]%3 -gt ([uint64]$disk.Size - "
                     "$start)) { throw 'Candidate exceeds disk bounds' }\n"
                     "foreach ($p in $existing) { $pStart = [uint64]$p.Offset; $pEnd = "
                     "[uint64]($p.Offset + $p.Size); if ($start -lt $pEnd -and $end -gt "
                     "$pStart) { throw 'Candidate overlaps existing partition' } }\n"
                     "New-Partition -DiskNumber %1 -Offset %2 -Size %3 %4\n")
                     .arg(operation.target.disk_number)
                     .arg(uintArg(offset), uintArg(size), typeArg);
    out.timeout_seconds = kPartitionMediumTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildSetPartitionHiddenScript(
    const PartitionOperation& operation) const {
    const bool hidden = payloadBool(operation, QStringLiteral("hidden"));
    PartitionScript out;
    out.preview = QStringLiteral("%1 Disk %2 Partition %3")
                      .arg(hidden ? QStringLiteral("Hide") : QStringLiteral("Unhide"))
                      .arg(operation.target.disk_number)
                      .arg(operation.target.partition_number);
    out.script = commonHeader(out.preview) +
                 requirePartitionIdentity(operation.target.disk_number,
                                          operation.target.partition_number,
                                          operation.target.size_bytes) +
                 QStringLiteral(
                     "Set-Partition -DiskNumber %1 -PartitionNumber %2 "
                     "-IsHidden $%3 -NoDefaultDriveLetter $%3\n")
                     .arg(operation.target.disk_number)
                     .arg(operation.target.partition_number)
                     .arg(hidden ? QStringLiteral("true") : QStringLiteral("false"));
    if (hidden && isValidDriveLetter(operation.target.drive_letter.left(1))) {
        out.script += QStringLiteral(
                          "Remove-PartitionAccessPath -DiskNumber %1 -PartitionNumber %2 "
                          "-AccessPath %3 -ErrorAction SilentlyContinue\n")
                          .arg(operation.target.disk_number)
                          .arg(operation.target.partition_number)
                          .arg(quotePowerShell(operation.target.drive_letter.left(1).toUpper() +
                                               QStringLiteral(":\\")));
    }
    return out;
}

PartitionScript PartitionScriptBuilder::buildSetPartitionActiveScript(
    const PartitionOperation& operation) const {
    const bool active = payloadBool(operation, QStringLiteral("active"));
    PartitionScript out;
    out.preview = QStringLiteral("Set Disk %1 Partition %2 active flag to %3")
                      .arg(operation.target.disk_number)
                      .arg(operation.target.partition_number)
                      .arg(active ? QStringLiteral("active") : QStringLiteral("inactive"));
    out.script = commonHeader(out.preview) +
                 requirePartitionIdentity(operation.target.disk_number,
                                          operation.target.partition_number,
                                          operation.target.size_bytes) +
                 QStringLiteral("Set-Partition -DiskNumber %1 -PartitionNumber %2 -IsActive $%3\n")
                     .arg(operation.target.disk_number)
                     .arg(operation.target.partition_number)
                     .arg(active ? QStringLiteral("true") : QStringLiteral("false"));
    return out;
}

PartitionScript PartitionScriptBuilder::buildSetPartitionTypeIdScript(
    const PartitionOperation& operation) const {
    const QString typeId = payloadString(operation, QStringLiteral("type_id")).trimmed();
    if (typeId.isEmpty()) {
        return invalidScript(QStringLiteral("Partition type change requires type_id"));
    }

    PartitionScript out;
    out.preview = QStringLiteral("Change Disk %1 Partition %2 type ID to %3")
                      .arg(operation.target.disk_number)
                      .arg(operation.target.partition_number)
                      .arg(typeId);
    out.script = commonHeader(out.preview) +
                 requirePartitionIdentity(operation.target.disk_number,
                                          operation.target.partition_number,
                                          operation.target.size_bytes) +
                 QStringLiteral(
                     "$d = Get-Disk -Number %1 -ErrorAction Stop\n"
                     "$typeId = %2\n"
                     "if ($d.PartitionStyle -eq 'GPT') {\n"
                     "  Set-Partition -DiskNumber %1 -PartitionNumber %3 -GptType $typeId\n"
                     "} elseif ($d.PartitionStyle -eq 'MBR') {\n"
                     "  Set-Partition -DiskNumber %1 -PartitionNumber %3 -MbrType $typeId\n"
                     "} else { throw 'Partition type ID requires MBR or GPT disk' }\n")
                     .arg(operation.target.disk_number)
                     .arg(quotePowerShell(typeId))
                     .arg(operation.target.partition_number);
    return out;
}

PartitionScript PartitionScriptBuilder::buildInitializeDiskScript(
    const PartitionOperation& operation) const {
    const QString style =
        payloadString(operation, QStringLiteral("target_style"), QStringLiteral("GPT"))
            .trimmed()
            .toUpper();
    if (style != QStringLiteral("GPT") && style != QStringLiteral("MBR")) {
        return invalidScript(QStringLiteral("Initialize disk requires GPT or MBR target_style"));
    }

    PartitionScript out;
    out.preview =
        QStringLiteral("Initialize Disk %1 as %2").arg(operation.target.disk_number).arg(style);
    out.script = commonHeader(out.preview) +
                 QStringLiteral(
                     "Set-Disk -Number %1 -IsOffline $false -ErrorAction Stop\n"
                     "Set-Disk -Number %1 -IsReadOnly $false -ErrorAction Stop\n"
                     "Initialize-Disk -Number %1 -PartitionStyle %2\n")
                     .arg(operation.target.disk_number)
                     .arg(style);
    out.timeout_seconds = kPartitionMediumTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildDeleteAllPartitionsScript(
    const PartitionOperation& operation) const {
    PartitionScript out;
    out.preview =
        QStringLiteral("Delete all partitions on Disk %1").arg(operation.target.disk_number);
    out.script = commonHeader(out.preview) +
                 QStringLiteral(
                     "$parts = Get-Partition -DiskNumber %1 -ErrorAction Stop | "
                     "Sort-Object PartitionNumber -Descending\n"
                     "foreach ($part in $parts) {\n"
                     "  Remove-Partition -DiskNumber %1 -PartitionNumber $part.PartitionNumber "
                     "-Confirm:$false\n"
                     "}\n")
                     .arg(operation.target.disk_number);
    out.timeout_seconds = kPartitionLongTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildResizeScript(
    const PartitionOperation& operation) const {
    const uint64_t targetSize = payloadUInt64(operation, QStringLiteral("target_size_bytes"));
    if (targetSize == 0) {
        return invalidScript(QStringLiteral("Resize requires target_size_bytes"));
    }
    if (isNonNativeFileSystemToolOperation(operation)) {
        const QString targetBlocker = rawPartitionTargetPathBlocker(operation);
        if (!targetBlocker.isEmpty()) {
            return invalidScript(targetBlocker);
        }
        const QString fs =
            payloadString(operation, QStringLiteral("file_system"), QStringLiteral("ext4"));
        if (!isExtFileSystemToken(fs)) {
            return invalidScript(
                QStringLiteral("Non-Windows resize currently supports ext2/ext3/ext4 only"));
        }
        const QString targetPath = rawPartitionTargetPath(operation);
        if (targetSize < operation.target.size_bytes) {
            return buildExtShrinkResizeScript(operation, fs, targetPath, targetSize);
        }
        const auto request = growExtResizeRequest(operation, fs, targetPath, targetSize);
        return buildExternalFileSystemToolScript(operation, request);
    }

    return buildNativeResizeScript(operation, targetSize);
}

PartitionScript PartitionScriptBuilder::buildAllocateFreeSpaceScript(
    const PartitionOperation& operation) const {
    const auto payload = allocateFreeSpacePayload(operation);
    if (const QString blocker = allocateFreeSpacePayloadError(payload, operation);
        !blocker.isEmpty()) {
        return invalidScript(blocker);
    }

    const uint64_t targetSize = operation.target.size_bytes + payload.bytes_to_allocate;
    const uint64_t donorRemainingBytes = payload.source_size - payload.bytes_to_allocate;
    PartitionScript out;
    out.preview = QStringLiteral("Allocate %1 from Disk %2 Partition %3 to Partition %4")
                      .arg(formatPartitionBytes(payload.bytes_to_allocate))
                      .arg(operation.target.disk_number)
                      .arg(payload.source_partition)
                      .arg(operation.target.partition_number);
    out.script = commonHeader(out.preview) +
                 requirePartitionIdentity(operation.target.disk_number,
                                          operation.target.partition_number,
                                          operation.target.size_bytes) +
                 allocateSetupScript(operation, payload, targetSize, donorRemainingBytes) +
                 robocopyManifestFunctionsScript() + allocateExecutionScript(operation, payload);
    out.timeout_seconds = kPartitionLongTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildConvertStyleScript(
    const PartitionOperation& operation) const {
    const QString target_style =
        payloadString(operation, QStringLiteral("target_style"), QStringLiteral("GPT")).toUpper();
    PartitionScript out;
    out.preview =
        QStringLiteral("Convert Disk %1 to %2").arg(operation.target.disk_number).arg(target_style);
    if (payloadString(operation, QStringLiteral("mode")) == QStringLiteral("mbr2gpt")) {
        out.script = commonHeader(out.preview) +
                     QStringLiteral(
                         "mbr2gpt.exe /validate /disk:%1 /allowFullOS\n"
                         "if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }\n"
                         "mbr2gpt.exe /convert /disk:%1 /allowFullOS\n"
                         "exit $LASTEXITCODE\n")
                         .arg(operation.target.disk_number);
    } else {
        out.script = commonHeader(out.preview) +
                     QStringLiteral(
                         "Set-Disk -Number %1 -IsOffline $false -ErrorAction Stop\n"
                         "Set-Disk -Number %1 -IsReadOnly $false -ErrorAction Stop\n"
                         "$disk = Get-Disk -Number %1 -ErrorAction Stop\n"
                         "if ($disk.IsBoot -or $disk.IsSystem) { throw 'System disk conversion "
                         "must use MBR2GPT' }\n"
                         "$parts = @(Get-Partition -DiskNumber %1 -ErrorAction SilentlyContinue)\n"
                         "if ($parts.Count -gt 0) { throw 'Data disk partition-style conversion "
                         "requires an empty disk' }\n"
                         "if ($disk.PartitionStyle -ne 'RAW') {\n"
                         "  Clear-Disk -Number %1 -RemoveData -Confirm:$false\n"
                         "}\n"
                         "Initialize-Disk -Number %1 -PartitionStyle %2\n")
                         .arg(operation.target.disk_number)
                         .arg(target_style);
    }
    out.timeout_seconds = kPartitionConversionTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildMergeScript(
    const PartitionOperation& operation) const {
    const uint32_t source_partition =
        static_cast<uint32_t>(payloadUInt64(operation, QStringLiteral("source_partition_number")));
    const QString target_folder = payloadString(operation,
                                                QStringLiteral("target_folder"),
                                                QStringLiteral("MergedPartition"));
    if (source_partition == 0) {
        return invalidScript(QStringLiteral("Merge requires source_partition_number"));
    }

    PartitionScript out;
    out.preview = QStringLiteral("Merge Disk %1 Partition %2 into Partition %3")
                      .arg(operation.target.disk_number)
                      .arg(source_partition)
                      .arg(operation.target.partition_number);
    out.script = commonHeader(out.preview) +
                 requirePartitionIdentity(operation.target.disk_number,
                                          operation.target.partition_number,
                                          operation.target.size_bytes) +
                 QStringLiteral(
                     "$target = $p\n"
                     "$source = Get-Partition -DiskNumber %1 -PartitionNumber %2 "
                     "-ErrorAction Stop\n"
                     "if ([uint64]$source.Offset -ne ([uint64]$target.Offset + "
                     "[uint64]$target.Size)) { throw 'Source partition must be directly after "
                     "target partition' }\n"
                     "$targetVolume = $target | Get-Volume -ErrorAction Stop\n"
                     "$sourceVolume = $source | Get-Volume -ErrorAction Stop\n"
                     "if (-not $targetVolume.DriveLetter -or -not $sourceVolume.DriveLetter) "
                     "{ throw 'Merge requires drive letters on both partitions' }\n"
                     "$mergeFolder = %3\n"
                     "$destination = ('{0}:\\{1}' -f $targetVolume.DriveLetter, $mergeFolder)\n"
                     "New-Item -ItemType Directory -Force -Path $destination | Out-Null\n"
                     "robocopy.exe ('{0}:\\' -f $sourceVolume.DriveLetter) $destination /E "
                     "/COPY:DAT /DCOPY:DAT /R:1 /W:1\n"
                     "if ($LASTEXITCODE -gt 7) { exit $LASTEXITCODE }\n"
                     "Remove-Partition -DiskNumber %1 -PartitionNumber %2 -Confirm:$false\n"
                     "$supported = Get-PartitionSupportedSize -DiskNumber %1 -PartitionNumber %4\n"
                     "Resize-Partition -DiskNumber %1 -PartitionNumber %4 -Size "
                     "$supported.SizeMax\n")
                     .arg(operation.target.disk_number)
                     .arg(source_partition)
                     .arg(quotePowerShell(target_folder))
                     .arg(operation.target.partition_number);
    out.timeout_seconds = kPartitionLongTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildSplitScript(
    const PartitionOperation& operation) const {
    const uint64_t first_size = payloadUInt64(operation, QStringLiteral("first_size_bytes"));
    const QString fs =
        payloadString(operation, QStringLiteral("new_file_system"), QStringLiteral("NTFS"));
    if (first_size == 0) {
        return invalidScript(QStringLiteral("Split requires first_size_bytes"));
    }

    PartitionScript out;
    out.preview = QStringLiteral("Split Disk %1 Partition %2")
                      .arg(operation.target.disk_number)
                      .arg(operation.target.partition_number);
    out.script = commonHeader(out.preview) +
                 requirePartitionIdentity(operation.target.disk_number,
                                          operation.target.partition_number,
                                          operation.target.size_bytes) +
                 QStringLiteral(
                     "Resize-Partition -DiskNumber %1 -PartitionNumber %2 -Size %3\n"
                     "$new = New-Partition -DiskNumber %1 -UseMaximumSize -AssignDriveLetter\n"
                     "Format-Volume -Partition $new -FileSystem %4 -Confirm:$false\n")
                     .arg(operation.target.disk_number)
                     .arg(operation.target.partition_number)
                     .arg(uintArg(first_size), fs.toUpper());
    out.timeout_seconds = kPartitionFormatTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildConvertFileSystemScript(
    const PartitionOperation& operation) const {
    const QString letter = operation.target.drive_letter.left(1);
    if (!isValidDriveLetter(letter)) {
        return invalidScript(QStringLiteral("File-system conversion requires a drive letter"));
    }

    PartitionScript out;
    out.preview = QStringLiteral("Convert %1: to NTFS").arg(letter.toUpper());
    out.script = commonHeader(out.preview) + QStringLiteral(
                                                 "convert.exe %1: /FS:NTFS /NoSecurity\n"
                                                 "exit $LASTEXITCODE\n")
                                                 .arg(letter.toUpper());
    out.timeout_seconds = kPartitionConversionTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildChangeClusterSizeScript(
    const PartitionOperation& operation) const {
    const auto payload = clusterSizePayload(operation);
    if (const QString blocker = clusterSizePayloadError(payload, operation); !blocker.isEmpty()) {
        return invalidScript(blocker);
    }

    PartitionScript out;
    out.preview = QStringLiteral("Change %1: cluster size to %2 bytes")
                      .arg(payload.drive.toUpper())
                      .arg(uintArg(payload.allocation_unit));
    out.script = commonHeader(out.preview) +
                 requirePartitionIdentity(operation.target.disk_number,
                                          operation.target.partition_number,
                                          operation.target.size_bytes) +
                 clusterSetupScript(payload) + robocopyManifestFunctionsScript() +
                 clusterExecutionScript();
    out.timeout_seconds = kPartitionLongTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildCloneOrImageScript(
    const PartitionOperation& operation) const {
    const CloneTransferSpec spec = cloneTransferSpec(operation);
    const QString blocker = validateCloneOrImageScript(operation, spec);
    if (!blocker.isEmpty()) {
        return invalidScript(blocker);
    }

    PartitionScript out;
    out.preview = toDisplayString(operation.type) + QStringLiteral(" from ") + spec.source +
                  QStringLiteral(" to ") + spec.target;
    out.script = commonHeader(out.preview) + cloneTransferScript(spec);
    if (operation.type == PartitionOperationType::MigrateOs) {
        out.script += osMigrationBootValidationScript();
    }
    out.timeout_seconds = kPartitionLongTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildBootRepairScript(
    const PartitionOperation& operation) const {
    const QString windows_path =
        payloadString(operation, QStringLiteral("windows_path"), QStringLiteral("C:\\Windows"));
    const QString esp_letter =
        payloadString(operation, QStringLiteral("esp_letter"), QStringLiteral("S")).left(1);
    const QString boot_mode =
        payloadString(operation, QStringLiteral("boot_mode"), QStringLiteral("UEFI"))
            .trimmed()
            .toUpper();
    if (!isValidDriveLetter(esp_letter)) {
        return invalidScript(QStringLiteral("Boot repair requires valid esp_letter"));
    }
    if (boot_mode != QStringLiteral("UEFI") && boot_mode != QStringLiteral("BIOS")) {
        return invalidScript(QStringLiteral("Boot repair requires UEFI or BIOS boot_mode"));
    }

    PartitionScript out;
    out.preview = QStringLiteral("Repair %1 boot files for %2").arg(boot_mode, windows_path);
    if (boot_mode == QStringLiteral("BIOS")) {
        out.script = commonHeader(out.preview) +
                     QStringLiteral(
                         "bcdboot.exe %1 /s %2: /f BIOS\n"
                         "$code = $LASTEXITCODE\n"
                         "bootsect.exe /nt60 %2: /mbr\n"
                         "if ($LASTEXITCODE -ne 0 -and $code -eq 0) { $code = $LASTEXITCODE }\n"
                         "reagentc.exe /info\n"
                         "exit $code\n")
                         .arg(quotePowerShell(windows_path), esp_letter.toUpper());
    } else {
        out.script = commonHeader(out.preview) +
                     QStringLiteral(
                         "mountvol %1: /S\n"
                         "try { bcdboot.exe %2 /s %1: /f UEFI; $code = $LASTEXITCODE }\n"
                         "finally { mountvol %1: /D }\n"
                         "reagentc.exe /info\n"
                         "exit $code\n")
                         .arg(esp_letter.toUpper(), quotePowerShell(windows_path));
    }
    out.timeout_seconds = kPartitionMediumTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildOptimizeSsdScript(
    const PartitionOperation& operation) const {
    const QString letter =
        payloadString(operation, QStringLiteral("drive_letter"), operation.target.drive_letter)
            .left(1);
    if (!isValidDriveLetter(letter)) {
        return invalidScript(QStringLiteral("SSD optimization requires a drive letter"));
    }

    PartitionScript out;
    out.preview = QStringLiteral("Run SSD ReTrim on %1:").arg(letter.toUpper());
    out.script = commonHeader(out.preview) +
                 QStringLiteral(
                     "fsutil behavior query DisableDeleteNotify\n"
                     "Optimize-Volume -DriveLetter %1 -ReTrim -Verbose\n")
                     .arg(letter.toUpper());
    out.timeout_seconds = kPartitionMediumTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildDefragVolumeScript(
    const PartitionOperation& operation) const {
    const QString letter =
        payloadString(operation, QStringLiteral("drive_letter"), operation.target.drive_letter)
            .left(1);
    if (!isValidDriveLetter(letter)) {
        return invalidScript(QStringLiteral("HDD defrag requires a drive letter"));
    }

    PartitionScript out;
    out.preview = QStringLiteral("Run HDD defrag on %1:").arg(letter.toUpper());
    out.script = commonHeader(out.preview) +
                 QStringLiteral(
                     "$driveLetter = %1\n"
                     "$partition = Get-Partition -DriveLetter $driveLetter -ErrorAction Stop\n"
                     "$disk = Get-Disk -Number $partition.DiskNumber -ErrorAction Stop\n"
                     "$mediaText = \"$($disk.FriendlyName) $($disk.BusType) $($disk.MediaType)\"\n"
                     "try {\n"
                     "    $physical = Get-PhysicalDisk -ErrorAction SilentlyContinue |\n"
                     "        Where-Object { $_.DeviceId -eq \"$($disk.Number)\" } |\n"
                     "        Select-Object -First 1\n"
                     "    if ($physical) { $mediaText = \"$mediaText $($physical.MediaType)\" }\n"
                     "} catch { }\n"
                     "if ($mediaText -match '(?i)SSD|NVMe') {\n"
                     "    throw \"Refusing HDD defrag on SSD/NVMe media: $mediaText\"\n"
                     "}\n"
                     "Optimize-Volume -DriveLetter $driveLetter -Analyze -Verbose\n"
                     "Optimize-Volume -DriveLetter $driveLetter -Defrag -Verbose\n"
                     "Repair-Volume -DriveLetter $driveLetter -Scan\n")
                     .arg(quotePowerShell(letter.toUpper()));
    out.timeout_seconds = kPartitionLongTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildBitLockerScript(
    const PartitionOperation& operation) const {
    const QString letter =
        payloadString(operation, QStringLiteral("drive_letter"), operation.target.drive_letter)
            .left(1);
    if (!isValidDriveLetter(letter)) {
        return invalidScript(QStringLiteral("BitLocker mutation requires a drive letter"));
    }

    const QString mountPoint = letter.toUpper() + QStringLiteral(":");
    const QString recoveryPassword =
        payloadString(operation, QStringLiteral("recovery_password")).trimmed();
    PartitionScript out;
    out.preview = toDisplayString(operation.type) + QStringLiteral(" ") + mountPoint;
    out.script = commonHeader(out.preview) + QStringLiteral(
                                                 "$mountPoint = %1\n"
                                                 "manage-bde.exe -status $mountPoint\n")
                                                 .arg(quotePowerShell(mountPoint));

    if (operation.type == PartitionOperationType::BitLockerUnlock) {
        if (recoveryPassword.isEmpty()) {
            return invalidScript(QStringLiteral("BitLocker unlock requires recovery_password"));
        }
        out.script += QStringLiteral(
                          "$recoveryPassword = %1\n"
                          "manage-bde.exe -unlock $mountPoint -RecoveryPassword "
                          "$recoveryPassword\n"
                          "if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }\n")
                          .arg(quotePowerShell(recoveryPassword));
        out.dry_run_script = out.script;
        out.dry_run_script.replace(quotePowerShell(recoveryPassword),
                                   quotePowerShell(QStringLiteral("<redacted>")));
    } else if (operation.type == PartitionOperationType::BitLockerSuspend) {
        out.script += QStringLiteral(
            "manage-bde.exe -protectors -disable $mountPoint\n"
            "if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }\n");
    } else {
        out.script += QStringLiteral(
            "manage-bde.exe -protectors -enable $mountPoint\n"
            "if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }\n");
    }

    out.script += QStringLiteral(
        "$state = Get-BitLockerVolume -MountPoint $mountPoint -ErrorAction Stop\n"
        "$state | Select-Object MountPoint,VolumeStatus,ProtectionStatus,LockStatus,"
        "EncryptionMethod | Format-List | Out-String | Write-Output\n");
    out.timeout_seconds = kPartitionMediumTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildWipeScript(const PartitionOperation& operation) const {
    PartitionScript out;
    if (operation.type == PartitionOperationType::WipeFreeSpace) {
        const QString letter = operation.target.drive_letter.left(1);
        if (!isValidDriveLetter(letter)) {
            return invalidScript(QStringLiteral("Free-space wipe requires a drive letter"));
        }
        out.preview = QStringLiteral("Wipe free space on %1:").arg(letter.toUpper());
        out.script =
            commonHeader(out.preview) +
            QStringLiteral("cipher.exe /w:%1:\\\nexit $LASTEXITCODE\n").arg(letter.toUpper());
        out.timeout_seconds = kPartitionLongTaskTimeoutSeconds;
        return out;
    }
    if (operation.type == PartitionOperationType::WipeDisk) {
        out.preview = QStringLiteral("Wipe Disk %1").arg(operation.target.disk_number);
        const QString ssdPrelude =
            payloadBool(operation, QStringLiteral("ssd_secure_erase"))
                ? QStringLiteral(
                      "$trimVolumes = @(Get-Partition -DiskNumber %1 -ErrorAction SilentlyContinue "
                      "| "
                      "Get-Volume -ErrorAction SilentlyContinue | Where-Object { $_.DriveLetter "
                      "})\n"
                      "foreach ($volume in $trimVolumes) { Optimize-Volume -DriveLetter "
                      "$volume.DriveLetter -ReTrim -Verbose -ErrorAction SilentlyContinue }\n")
                      .arg(operation.target.disk_number)
                : QString();
        out.script = commonHeader(out.preview) + ssdPrelude +
                     QStringLiteral(
                         "Clear-Disk -Number %1 -RemoveData -RemoveOEM -Confirm:$false\n"
                         "Initialize-Disk -Number %1 -PartitionStyle GPT\n"
                         "$p = New-Partition -DiskNumber %1 -UseMaximumSize\n"
                         "$p | Format-Volume -FileSystem NTFS -Full "
                         "-NewFileSystemLabel 'SAK_WIPE' -Confirm:$false\n"
                         "Remove-Partition -DiskNumber %1 -PartitionNumber "
                         "$p.PartitionNumber -Confirm:$false\n"
                         "Clear-Disk -Number %1 -RemoveData -RemoveOEM -Confirm:$false\n")
                         .arg(operation.target.disk_number);
        out.timeout_seconds = kPartitionLongTaskTimeoutSeconds;
        return out;
    }
    out.preview = QStringLiteral("Full format Disk %1 Partition %2")
                      .arg(operation.target.disk_number)
                      .arg(operation.target.partition_number);
    out.script = commonHeader(out.preview) +
                 requirePartitionIdentity(operation.target.disk_number,
                                          operation.target.partition_number,
                                          operation.target.size_bytes) +
                 QStringLiteral("$p | Format-Volume -FileSystem NTFS -Full -Confirm:$false\n");
    out.timeout_seconds = kPartitionLongTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildMovePartitionScript(
    const PartitionOperation& operation) const {
    const auto payload = movePartitionScriptPayload(operation);
    const QString payloadError = movePartitionPayloadError(payload, operation);
    if (!payloadError.isEmpty()) {
        return invalidScript(payloadError);
    }

    PartitionScript out;
    out.preview = QStringLiteral("Move Disk %1 Partition %2")
                      .arg(operation.target.disk_number)
                      .arg(operation.target.partition_number);
    out.script = commonHeader(out.preview) +
                 requirePartitionIdentity(operation.target.disk_number,
                                          operation.target.partition_number,
                                          operation.target.size_bytes) +
                 backupRestoreHelpersScript() +
                 QStringLiteral(
                     "$drive = %1\n"
                     "$sourceRoot = ('{0}:\\' -f $drive)\n"
                     "$backupRoot = %2\n"
                     "$fileSystem = %3\n"
                     "$label = %4\n"
                     "$targetOffset = [uint64]%5\n"
                     "$targetSize = [uint64]%6\n"
                     "$backupRootFull = Assert-SakBackupRoot $backupRoot @($sourceRoot)\n"
                     "$backupPath = Join-Path $backupRootFull ('sak-move-backup-{0}' -f "
                     "[guid]::NewGuid().ToString('N'))\n"
                     "New-Item -ItemType Directory -Force -Path $backupPath | Out-Null\n"
                     "Invoke-SakRobocopy $sourceRoot $backupPath\n"
                     "$backupManifest = @(Get-SakFileManifest $backupPath)\n"
                     "Remove-Partition -DiskNumber %7 -PartitionNumber %8 -Confirm:$false\n"
                     "$newPartition = New-Partition -DiskNumber %7 -Size $targetSize -Offset "
                     "$targetOffset -DriveLetter $drive\n"
                     "Format-Volume -Partition $newPartition -FileSystem $fileSystem "
                     "-NewFileSystemLabel $label -Confirm:$false -Force\n"
                     "Invoke-SakRobocopy $backupPath $sourceRoot\n"
                     "$restoredManifest = @(Get-SakFileManifest $sourceRoot)\n"
                     "Assert-SakManifestMatch $backupManifest $restoredManifest\n"
                     "Repair-Volume -DriveLetter $drive -Scan\n"
                     "Get-Partition -DiskNumber %7 | Sort-Object Offset | Format-Table "
                     "DiskNumber,PartitionNumber,DriveLetter,Offset,Size,Type -AutoSize\n")
                     .arg(quotePowerShell(payload.drive),
                          quotePowerShell(QDir::toNativeSeparators(payload.backup_directory)),
                          quotePowerShell(payload.file_system),
                          quotePowerShell(payload.label),
                          uintArg(payload.target_offset),
                          uintArg(payload.target_size),
                          QString::number(operation.target.disk_number),
                          QString::number(operation.target.partition_number));
    out.timeout_seconds = kPartitionLongTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildConvertPrimaryLogicalScript(
    const PartitionOperation& operation) const {
    const auto payload = primaryLogicalScriptPayload(operation);
    const QString payloadError = primaryLogicalPayloadError(payload, operation);
    if (!payloadError.isEmpty()) {
        return invalidScript(payloadError);
    }

    const QString logicalBranch = payload.make_logical ? QStringLiteral("$true")
                                                       : QStringLiteral("$false");
    PartitionScript out;
    out.preview = QStringLiteral("Convert Disk %1 Partition %2 to %3")
                      .arg(operation.target.disk_number)
                      .arg(operation.target.partition_number)
                      .arg(payload.make_logical ? QStringLiteral("logical")
                                                : QStringLiteral("primary"));
    out.script =
        commonHeader(out.preview) +
        requirePartitionIdentity(operation.target.disk_number,
                                 operation.target.partition_number,
                                 operation.target.size_bytes) +
        backupRestoreHelpersScript() + diskPartRunnerScript() +
        QStringLiteral(
            "$drive = %1\n"
            "$sourceRoot = ('{0}:\\' -f $drive)\n"
            "$backupRoot = %2\n"
            "$sourceSizeMb = %3\n"
            "$extendedSizeMb = [int]($sourceSizeMb + 128)\n"
            "$fileSystem = %4\n"
            "$label = %5\n"
            "$makeLogical = %6\n"
            "$parts = @(Get-Partition -DiskNumber %7 -ErrorAction Stop)\n"
            "if ($parts.Count -ne 1) { throw 'Primary/logical conversion requires a single data "
            "partition on the disk' }\n"
            "$backupRootFull = Assert-SakBackupRoot $backupRoot @($sourceRoot)\n"
            "$backupPath = Join-Path $backupRootFull ('sak-primary-logical-backup-{0}' -f "
            "[guid]::NewGuid().ToString('N'))\n"
            "New-Item -ItemType Directory -Force -Path $backupPath | Out-Null\n"
            "Invoke-SakRobocopy $sourceRoot $backupPath\n"
            "$backupManifest = @(Get-SakFileManifest $backupPath)\n"
            "$lines = @('select disk %7','clean','convert mbr')\n"
            "if ($makeLogical) { $lines += ('create partition extended size={0}' -f "
            "$extendedSizeMb); $lines += ('create partition logical size={0}' -f $sourceSizeMb) }\n"
            "else { $lines += ('create partition primary size={0}' -f $sourceSizeMb) }\n"
            "$lines += ('format fs={0} quick label=\"{1}\"' -f $fileSystem, $label)\n"
            "$lines += ('assign letter={0}' -f $drive)\n"
            "Invoke-SakDiskPart $lines\n"
            "Invoke-SakRobocopy $backupPath $sourceRoot\n"
            "$restoredManifest = @(Get-SakFileManifest $sourceRoot)\n"
            "Assert-SakManifestMatch $backupManifest $restoredManifest\n"
            "Repair-Volume -DriveLetter $drive -Scan\n"
            "Get-Partition -DiskNumber %7 | Sort-Object Offset | Format-Table "
            "DiskNumber,PartitionNumber,DriveLetter,Offset,Size,Type -AutoSize\n")
            .arg(quotePowerShell(payload.drive),
                 quotePowerShell(QDir::toNativeSeparators(payload.backup_directory)),
                 sizeMbArg(payload.source_size),
                 quotePowerShell(payload.file_system),
                 quotePowerShell(payload.label),
                 logicalBranch,
                 QString::number(operation.target.disk_number));
    out.timeout_seconds = kPartitionLongTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildChangeVolumeSerialNumberScript(
    const PartitionOperation& operation) const {
    const QString drive =
        payloadString(operation, QStringLiteral("drive_letter"), operation.target.drive_letter)
            .left(1)
            .toUpper();
    const QString fs =
        payloadString(operation, QStringLiteral("file_system"), QStringLiteral("NTFS"))
            .trimmed()
            .toUpper();
    const QString label = payloadString(operation, QStringLiteral("label"), QStringLiteral("Data"));
    const QString backupDirectory =
        payloadString(operation, QStringLiteral("backup_directory")).trimmed();
    if (!isValidDriveLetter(drive)) {
        return invalidScript(QStringLiteral("Volume serial-number change requires drive letter"));
    }
    if (!isSupportedFileSystem(fs)) {
        return invalidScript(QStringLiteral("Unsupported file system"));
    }
    if (backupDirectory.isEmpty()) {
        return invalidScript(
            QStringLiteral("Volume serial-number change requires backup_directory"));
    }
    if (!payloadBool(operation, QStringLiteral("target_wipe_confirmed"))) {
        return invalidScript(
            QStringLiteral("Volume serial-number change requires destructive confirmation"));
    }

    PartitionScript out;
    out.preview = QStringLiteral("Regenerate serial number on %1:").arg(drive);
    out.script =
        commonHeader(out.preview) + backupRestoreHelpersScript() +
        QStringLiteral(
            "$drive = %1\n"
            "$sourceRoot = ('{0}:\\' -f $drive)\n"
            "$backupRoot = %2\n"
            "$fileSystem = %3\n"
            "$label = %4\n"
            "function Get-SakVolumeSerial([string]$d) { $output = cmd.exe /c \"vol ${d}:\"; $match "
            "= [regex]::Match(($output -join \"`n\"), '[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}'); if (-not "
            "$match.Success) { throw \"Could not read volume serial for ${d}:\" }; return "
            "$match.Value.ToUpperInvariant() }\n"
            "$beforeSerial = Get-SakVolumeSerial $drive\n"
            "$backupRootFull = Assert-SakBackupRoot $backupRoot @($sourceRoot)\n"
            "$backupPath = Join-Path $backupRootFull ('sak-serial-backup-{0}' -f "
            "[guid]::NewGuid().ToString('N'))\n"
            "New-Item -ItemType Directory -Force -Path $backupPath | Out-Null\n"
            "Invoke-SakRobocopy $sourceRoot $backupPath\n"
            "$backupManifest = @(Get-SakFileManifest $backupPath)\n"
            "Format-Volume -DriveLetter $drive -FileSystem $fileSystem -NewFileSystemLabel $label "
            "-Confirm:$false -Force\n"
            "$afterSerial = Get-SakVolumeSerial $drive\n"
            "if ($afterSerial -eq $beforeSerial) { throw 'Volume serial number did not change "
            "after reformat' }\n"
            "Invoke-SakRobocopy $backupPath $sourceRoot\n"
            "$restoredManifest = @(Get-SakFileManifest $sourceRoot)\n"
            "Assert-SakManifestMatch $backupManifest $restoredManifest\n"
            "Repair-Volume -DriveLetter $drive -Scan\n"
            "Write-Output ('Volume serial changed from {0} to {1}' -f $beforeSerial, "
            "$afterSerial)\n")
            .arg(quotePowerShell(drive),
                 quotePowerShell(QDir::toNativeSeparators(backupDirectory)),
                 quotePowerShell(fs),
                 quotePowerShell(label));
    out.timeout_seconds = kPartitionLongTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildConvertDynamicDiskToBasicScript(
    const PartitionOperation& operation) const {
    const uint64_t sourceSize = payloadUInt64(operation, QStringLiteral("source_size_bytes"));
    const QString drive =
        payloadString(operation, QStringLiteral("drive_letter")).left(1).toUpper();
    const QString fs =
        payloadString(operation, QStringLiteral("file_system"), QStringLiteral("NTFS"))
            .trimmed()
            .toLower();
    const QString label = diskPartLabel(
        payloadString(operation, QStringLiteral("label"), QStringLiteral("SAKBASIC")));
    const QString backupDirectory =
        payloadString(operation, QStringLiteral("backup_directory")).trimmed();
    if (sourceSize == 0 || !isValidDriveLetter(drive)) {
        return invalidScript(
            QStringLiteral("Dynamic-to-basic conversion requires source size and drive letter"));
    }
    if (!isSupportedFileSystem(fs)) {
        return invalidScript(QStringLiteral("Unsupported file system"));
    }
    if (backupDirectory.isEmpty()) {
        return invalidScript(
            QStringLiteral("Dynamic-to-basic conversion requires backup_directory"));
    }
    if (!payloadBool(operation, QStringLiteral("target_wipe_confirmed"))) {
        return invalidScript(
            QStringLiteral("Dynamic-to-basic conversion requires destructive confirmation"));
    }

    PartitionScript out;
    out.preview =
        QStringLiteral("Convert Dynamic Disk %1 to Basic").arg(operation.target.disk_number);
    out.script =
        commonHeader(out.preview) + backupRestoreHelpersScript() + diskPartRunnerScript() +
        QStringLiteral(
            "$drive = %1\n"
            "$sourceRoot = ('{0}:\\' -f $drive)\n"
            "$backupRoot = %2\n"
            "$sourceSizeMb = %3\n"
            "$fileSystem = %4\n"
            "$label = %5\n"
            "$disk = Get-Disk -Number %6 -ErrorAction Stop\n"
            "if ($disk.IsBoot -or $disk.IsSystem) { throw 'Current OS disk dynamic-to-basic "
            "conversion is blocked' }\n"
            "if ($disk.IsReadOnly) { throw 'Target disk is read-only' }\n"
            "$backupRootFull = Assert-SakBackupRoot $backupRoot @($sourceRoot)\n"
            "$backupPath = Join-Path $backupRootFull ('sak-dynamic-basic-backup-{0}' -f "
            "[guid]::NewGuid().ToString('N'))\n"
            "New-Item -ItemType Directory -Force -Path $backupPath | Out-Null\n"
            "Invoke-SakRobocopy $sourceRoot $backupPath\n"
            "$backupManifest = @(Get-SakFileManifest $backupPath)\n"
            "$lines = @('select volume ' + $drive,'delete volume override','select disk "
            "%6','convert basic',('create partition primary size={0}' -f $sourceSizeMb),('format "
            "fs={0} quick label=\"{1}\"' -f $fileSystem, $label),('assign letter={0}' -f $drive))\n"
            "Invoke-SakDiskPart $lines\n"
            "Update-HostStorageCache -ErrorAction SilentlyContinue\n"
            "Invoke-SakRobocopy $backupPath $sourceRoot\n"
            "$restoredManifest = @(Get-SakFileManifest $sourceRoot)\n"
            "Assert-SakManifestMatch $backupManifest $restoredManifest\n"
            "Repair-Volume -DriveLetter $drive -Scan\n"
            "Get-Disk -Number %6 | Format-List Number,PartitionStyle,IsBoot,IsSystem,IsReadOnly\n")
            .arg(quotePowerShell(drive),
                 quotePowerShell(QDir::toNativeSeparators(backupDirectory)),
                 sizeMbArg(sourceSize),
                 quotePowerShell(fs),
                 quotePowerShell(label),
                 QString::number(operation.target.disk_number));
    out.timeout_seconds = kPartitionLongTaskTimeoutSeconds;
    return out;
}

}  // namespace sak

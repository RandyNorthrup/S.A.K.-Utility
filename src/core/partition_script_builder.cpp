// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_script_builder.cpp
/// @brief PowerShell script generation for Partition Manager.

#include "sak/partition_script_builder.h"

#include <QDir>
#include <QHash>
#include <QStringList>

#include <algorithm>

namespace sak {

namespace {

constexpr uint64_t kCloneIoBufferBytes = 1024ULL * 1024ULL;
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
                                          QStringLiteral("{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}")}
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

QString validateCreateScriptSpec(const PartitionOperation& operation,
                                 const CreateScriptSpec& spec) {
    if (spec.size == 0) {
        return QStringLiteral("Create requires size_bytes");
    }
    if (!PartitionScriptBuilder::isSupportedFileSystem(spec.file_system)) {
        return QStringLiteral("Unsupported file system");
    }
    if (!isSupportedAllocationUnitSize(spec.allocation_unit)) {
        return QStringLiteral("Unsupported allocation unit size");
    }
    if (hasMixedCreatePartitionTypes(operation)) {
        return QStringLiteral("Create requires either gpt_type or mbr_type, not both");
    }
    if (!isSupportedGptCreateType(spec.gpt_type) || !isSupportedMbrCreateType(spec.mbr_type)) {
        return QStringLiteral("Unsupported create partition type");
    }
    if (!createTypeMatchesFileSystem(operation, spec.file_system)) {
        return QStringLiteral("Selected partition type is incompatible with file system");
    }
    if (!createFitsSelectedRegion(operation, spec.size)) {
        return QStringLiteral("Create size and offset must fit selected unallocated region");
    }
    return {};
}

QString cloneTransferScript(const CloneTransferSpec& spec) {
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
               "smaller than source' }\n"
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
               "$to.Write($buf, 0, $read); $left -= [uint64]$read } }\n"
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
               "} } }\n"
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
               "-IsOffline $false -ErrorAction SilentlyContinue } }\n"
               "$in = Open-SakRead $src\n"
               "$sakRawTargetDisk = Assert-SakRawWriteTarget $dst\n"
               "try { try { if ($expectedBytes -eq 0) { try { $expectedBytes = [uint64]$in.Length "
               "- $sourceOffset } catch {} }; [void]$in.Seek([int64]$sourceOffset, "
               "[System.IO.SeekOrigin]::Begin); $out = Open-SakWrite $dst; try { "
               "[void]$out.Seek([int64]$targetOffset, [System.IO.SeekOrigin]::Begin); "
               "Copy-SakBytes $in $out $expectedBytes; $out.Flush() } finally { $out.Dispose() } } "
               "finally { $in.Dispose() } }\n"
               "finally { Restore-SakRawWriteTarget $sakRawTargetDisk }\n"
               "if (-not [string]::IsNullOrWhiteSpace($verifyMode)) { if ($expectedBytes -eq 0) { "
               "throw 'Verification requires known source size' }; $srcVerify = Open-SakRead $src; "
               "$dstVerify = Open-SakRead $dst; try { if ($verifyMode -like 'Full*') { "
               "Write-Output 'Running full clone verification'; Assert-SakFullCopy $srcVerify "
               "$dstVerify $expectedBytes $sourceOffset $targetOffset } else { Write-Output "
               "'Running sample clone verification'; Assert-SakSampleCopy $srcVerify $dstVerify "
               "$expectedBytes $sourceOffset $targetOffset } } finally { $dstVerify.Dispose(); "
               "$srcVerify.Dispose() } }\n")
        .arg(PartitionScriptBuilder::quotePowerShell(QDir::toNativeSeparators(spec.source)),
             PartitionScriptBuilder::quotePowerShell(QDir::toNativeSeparators(spec.target)),
             uintArg(spec.source_size),
             uintArg(spec.target_size),
             uintArg(spec.source_offset),
             uintArg(spec.target_offset),
             PartitionScriptBuilder::quotePowerShell(spec.verify_mode),
             uintArg(kCloneIoBufferBytes));
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

}  // namespace

PartitionScript PartitionScriptBuilder::buildScript(const PartitionOperation& operation) const {
    using Builder = PartitionScript (PartitionScriptBuilder::*)(const PartitionOperation&) const;
    static const QHash<int, Builder> kBuilders = {
        {static_cast<int>(PartitionOperationType::Create),
         &PartitionScriptBuilder::buildCreateScript},
        {static_cast<int>(PartitionOperationType::Delete),
         &PartitionScriptBuilder::buildDeleteScript},
        {static_cast<int>(PartitionOperationType::Format),
         &PartitionScriptBuilder::buildFormatScript},
        {static_cast<int>(PartitionOperationType::SetDriveLetter),
         &PartitionScriptBuilder::buildSetDriveLetterScript},
        {static_cast<int>(PartitionOperationType::SetPartitionLabel),
         &PartitionScriptBuilder::buildSetPartitionLabelScript},
        {static_cast<int>(PartitionOperationType::CheckFileSystem),
         &PartitionScriptBuilder::buildCheckFileSystemScript},
        {static_cast<int>(PartitionOperationType::SurfaceTest),
         &PartitionScriptBuilder::buildSurfaceTestScript},
        {static_cast<int>(PartitionOperationType::PartitionRecoveryScan),
         &PartitionScriptBuilder::buildPartitionRecoveryScanScript},
        {static_cast<int>(PartitionOperationType::RestoreRecoveredPartition),
         &PartitionScriptBuilder::buildRestoreRecoveredPartitionScript},
        {static_cast<int>(PartitionOperationType::SetPartitionHidden),
         &PartitionScriptBuilder::buildSetPartitionHiddenScript},
        {static_cast<int>(PartitionOperationType::SetPartitionActive),
         &PartitionScriptBuilder::buildSetPartitionActiveScript},
        {static_cast<int>(PartitionOperationType::SetPartitionTypeId),
         &PartitionScriptBuilder::buildSetPartitionTypeIdScript},
        {static_cast<int>(PartitionOperationType::InitializeDisk),
         &PartitionScriptBuilder::buildInitializeDiskScript},
        {static_cast<int>(PartitionOperationType::DeleteAllPartitions),
         &PartitionScriptBuilder::buildDeleteAllPartitionsScript},
        {static_cast<int>(PartitionOperationType::Resize),
         &PartitionScriptBuilder::buildResizeScript},
        {static_cast<int>(PartitionOperationType::AllocateFreeSpace),
         &PartitionScriptBuilder::buildAllocateFreeSpaceScript},
        {static_cast<int>(PartitionOperationType::ConvertPartitionStyle),
         &PartitionScriptBuilder::buildConvertStyleScript},
        {static_cast<int>(PartitionOperationType::Merge),
         &PartitionScriptBuilder::buildMergeScript},
        {static_cast<int>(PartitionOperationType::Split),
         &PartitionScriptBuilder::buildSplitScript},
        {static_cast<int>(PartitionOperationType::ConvertFileSystem),
         &PartitionScriptBuilder::buildConvertFileSystemScript},
        {static_cast<int>(PartitionOperationType::ChangeClusterSize),
         &PartitionScriptBuilder::buildChangeClusterSizeScript},
        {static_cast<int>(PartitionOperationType::CloneDisk),
         &PartitionScriptBuilder::buildCloneOrImageScript},
        {static_cast<int>(PartitionOperationType::ClonePartition),
         &PartitionScriptBuilder::buildCloneOrImageScript},
        {static_cast<int>(PartitionOperationType::CreateImage),
         &PartitionScriptBuilder::buildCloneOrImageScript},
        {static_cast<int>(PartitionOperationType::RestoreImage),
         &PartitionScriptBuilder::buildCloneOrImageScript},
        {static_cast<int>(PartitionOperationType::MigrateOs),
         &PartitionScriptBuilder::buildCloneOrImageScript},
        {static_cast<int>(PartitionOperationType::RepairBoot),
         &PartitionScriptBuilder::buildBootRepairScript},
        {static_cast<int>(PartitionOperationType::OptimizeSsd),
         &PartitionScriptBuilder::buildOptimizeSsdScript},
        {static_cast<int>(PartitionOperationType::DefragVolume),
         &PartitionScriptBuilder::buildDefragVolumeScript},
        {static_cast<int>(PartitionOperationType::BitLockerUnlock),
         &PartitionScriptBuilder::buildBitLockerScript},
        {static_cast<int>(PartitionOperationType::BitLockerSuspend),
         &PartitionScriptBuilder::buildBitLockerScript},
        {static_cast<int>(PartitionOperationType::BitLockerResume),
         &PartitionScriptBuilder::buildBitLockerScript},
        {static_cast<int>(PartitionOperationType::WipePartition),
         &PartitionScriptBuilder::buildWipeScript},
        {static_cast<int>(PartitionOperationType::WipeDisk),
         &PartitionScriptBuilder::buildWipeScript},
        {static_cast<int>(PartitionOperationType::WipeFreeSpace),
         &PartitionScriptBuilder::buildWipeScript},
        {static_cast<int>(PartitionOperationType::MovePartition),
         &PartitionScriptBuilder::buildMovePartitionScript},
        {static_cast<int>(PartitionOperationType::ConvertPrimaryLogical),
         &PartitionScriptBuilder::buildConvertPrimaryLogicalScript},
        {static_cast<int>(PartitionOperationType::ChangeVolumeSerialNumber),
         &PartitionScriptBuilder::buildChangeVolumeSerialNumberScript},
        {static_cast<int>(PartitionOperationType::ConvertDynamicDiskToBasic),
         &PartitionScriptBuilder::buildConvertDynamicDiskToBasicScript},
    };

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
    const QString driveArg = isValidDriveLetter(spec.drive_letter)
                                 ? QStringLiteral("-DriveLetter %1").arg(spec.drive_letter)
                                 : QStringLiteral("-AssignDriveLetter");
    const QString blocker = validateCreateScriptSpec(operation, spec);
    if (!blocker.isEmpty()) {
        return invalidScript(blocker);
    }

    PartitionScript out;
    out.preview = QStringLiteral("Create %1 %2 partition on Disk %3")
                      .arg(formatPartitionBytes(spec.size),
                           spec.file_system,
                           QString::number(operation.target.disk_number));
    out.script = commonHeader(out.preview) +
                 QStringLiteral(
                     "$p = New-Partition -DiskNumber %1 -Size %2 %3%4%5\n"
                     "Format-Volume -Partition $p -FileSystem %6 -NewFileSystemLabel %7%8%9 "
                     "-Confirm:$false\n"
                     "$p | ConvertTo-Json -Compress\n")
                     .arg(operation.target.disk_number)
                     .arg(uintArg(spec.size))
                     .arg(driveArg,
                          partitionTypeArg(operation),
                          createOffsetArg(operation),
                          spec.file_system.toUpper(),
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
    const uint64_t target_size = payloadUInt64(operation, QStringLiteral("target_size_bytes"));
    if (target_size == 0) {
        return invalidScript(QStringLiteral("Resize requires target_size_bytes"));
    }

    PartitionScript out;
    out.preview = QStringLiteral("Resize Disk %1 Partition %2 to %3")
                      .arg(operation.target.disk_number)
                      .arg(operation.target.partition_number)
                      .arg(formatPartitionBytes(target_size));
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
                     .arg(uintArg(target_size));
    out.timeout_seconds = kPartitionFormatTaskTimeoutSeconds;
    return out;
}

PartitionScript PartitionScriptBuilder::buildAllocateFreeSpaceScript(
    const PartitionOperation& operation) const {
    const uint32_t sourcePartition =
        static_cast<uint32_t>(payloadUInt64(operation, QStringLiteral("source_partition_number")));
    const uint64_t sourceSize = payloadUInt64(operation, QStringLiteral("source_size_bytes"));
    const uint64_t bytesToAllocate = payloadUInt64(operation, QStringLiteral("bytes_to_allocate"));
    const QString sourceLetter =
        payloadString(operation, QStringLiteral("source_drive_letter")).left(1).toUpper();
    const QString sourceFileSystem =
        payloadString(operation, QStringLiteral("source_file_system"), QStringLiteral("NTFS"))
            .trimmed()
            .toUpper();
    const QString sourceLabel =
        payloadString(operation, QStringLiteral("source_label"), QStringLiteral("Data"));
    const QString backupDirectory =
        payloadString(operation, QStringLiteral("backup_directory")).trimmed();
    if (sourcePartition == 0 || sourceSize == 0 || bytesToAllocate == 0) {
        return invalidScript(
            QStringLiteral("Allocate Free Space requires source partition, size, and bytes"));
    }
    if (bytesToAllocate >= sourceSize) {
        return invalidScript(
            QStringLiteral("Allocate Free Space must leave donor partition space"));
    }
    if (!isValidDriveLetter(sourceLetter)) {
        return invalidScript(QStringLiteral("Allocate Free Space requires donor drive letter"));
    }
    if (!isSupportedFileSystem(sourceFileSystem)) {
        return invalidScript(QStringLiteral("Unsupported donor file system"));
    }
    if (backupDirectory.isEmpty()) {
        return invalidScript(QStringLiteral("Allocate Free Space requires backup_directory"));
    }
    if (!payloadBool(operation, QStringLiteral("target_wipe_confirmed"))) {
        return invalidScript(
            QStringLiteral("Allocate Free Space requires destructive confirmation"));
    }

    const uint64_t targetSize = operation.target.size_bytes + bytesToAllocate;
    const uint64_t donorRemainingBytes = sourceSize - bytesToAllocate;
    PartitionScript out;
    out.preview = QStringLiteral("Allocate %1 from Disk %2 Partition %3 to Partition %4")
                      .arg(formatPartitionBytes(bytesToAllocate))
                      .arg(operation.target.disk_number)
                      .arg(sourcePartition)
                      .arg(operation.target.partition_number);
    out.script =
        commonHeader(out.preview) +
        requirePartitionIdentity(operation.target.disk_number,
                                 operation.target.partition_number,
                                 operation.target.size_bytes) +
        QStringLiteral(
            "$target = $p\n"
            "$source = Get-Partition -DiskNumber %1 -PartitionNumber %2 -ErrorAction Stop\n"
            "if ([uint64]$source.Size -ne [uint64]%3) { throw 'Donor partition identity mismatch' "
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
            "[System.StringComparison]::OrdinalIgnoreCase)) { throw 'Backup directory must not be "
            "on the donor volume' }\n"
            "if ($targetRoot -and $backupRootFull.StartsWith($targetRoot, "
            "[System.StringComparison]::OrdinalIgnoreCase)) { throw 'Backup directory must not be "
            "on the target volume' }\n"
            "if (-not (Test-Path -LiteralPath $backupRootFull -PathType Container)) { throw "
            "'Backup directory does not exist' }\n"
            "$backupPath = Join-Path $backupRootFull ('sak-allocate-backup-{0}' -f "
            "[guid]::NewGuid().ToString('N'))\n"
            "New-Item -ItemType Directory -Force -Path $backupPath | Out-Null\n"
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
            "}\n"
            "Write-Output ('Backing up donor {0} to {1}' -f $sourceRoot, $backupPath)\n"
            "Invoke-SakRobocopy $sourceRoot $backupPath\n"
            "$backupManifest = @(Get-SakFileManifest $backupPath)\n"
            "Remove-Partition -DiskNumber %1 -PartitionNumber %2 -Confirm:$false\n"
            "Resize-Partition -DiskNumber %1 -PartitionNumber %11 -Size $targetSizeBytes\n"
            "$newDonor = New-Partition -DiskNumber %1 -Size $donorRemainingBytes -DriveLetter "
            "$sourceDrive\n"
            "Format-Volume -Partition $newDonor -FileSystem $sourceFileSystem -NewFileSystemLabel "
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
            .arg(sourcePartition)
            .arg(uintArg(sourceSize))
            .arg(quotePowerShell(sourceLetter),
                 quotePowerShell(sourceFileSystem),
                 quotePowerShell(sourceLabel),
                 uintArg(bytesToAllocate),
                 uintArg(targetSize),
                 uintArg(donorRemainingBytes),
                 quotePowerShell(QDir::toNativeSeparators(backupDirectory)))
            .arg(operation.target.partition_number);
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
    const QString letter =
        payloadString(operation, QStringLiteral("drive_letter"), operation.target.drive_letter)
            .left(1);
    const QString fs =
        payloadString(operation, QStringLiteral("file_system"), QStringLiteral("NTFS"))
            .trimmed()
            .toUpper();
    const QString label = payloadString(operation, QStringLiteral("label"), QStringLiteral("Data"));
    const uint64_t allocationUnit = payloadUInt64(operation,
                                                  QStringLiteral("allocation_unit_bytes"));
    const QString backupDirectory =
        payloadString(operation, QStringLiteral("backup_directory")).trimmed();
    if (!isValidDriveLetter(letter)) {
        return invalidScript(QStringLiteral("Cluster-size change requires a drive letter"));
    }
    if (!isSupportedFileSystem(fs)) {
        return invalidScript(QStringLiteral("Unsupported file system"));
    }
    if (allocationUnit == kAllocationUnitDefaultBytes ||
        !isSupportedAllocationUnitSize(allocationUnit)) {
        return invalidScript(QStringLiteral(
            "Cluster-size change requires an explicit supported allocation unit size"));
    }
    if (backupDirectory.isEmpty()) {
        return invalidScript(QStringLiteral("Cluster-size change requires backup_directory"));
    }
    if (!payloadBool(operation, QStringLiteral("target_wipe_confirmed"))) {
        return invalidScript(
            QStringLiteral("Cluster-size change requires destructive confirmation"));
    }

    PartitionScript out;
    out.preview = QStringLiteral("Change %1: cluster size to %2 bytes")
                      .arg(letter.toUpper())
                      .arg(uintArg(allocationUnit));
    out.script =
        commonHeader(out.preview) +
        requirePartitionIdentity(operation.target.disk_number,
                                 operation.target.partition_number,
                                 operation.target.size_bytes) +
        QStringLiteral(
            "$drive = %1\n"
            "$fileSystem = %2\n"
            "$allocationUnitBytes = [uint32]%3\n"
            "$label = %4\n"
            "$backupRoot = %5\n"
            "$targetRoot = ('{0}:\\' -f $drive)\n"
            "$targetDrive = ('{0}:' -f $drive)\n"
            "$backupRootFull = [System.IO.Path]::GetFullPath($backupRoot)\n"
            "$backupRootDrive = [System.IO.Path]::GetPathRoot($backupRootFull)\n"
            "if ([string]::IsNullOrWhiteSpace($backupRootDrive)) { throw 'Backup directory must be "
            "on a mounted volume' }\n"
            "if ($backupRootFull.StartsWith($targetRoot, "
            "[System.StringComparison]::OrdinalIgnoreCase)) { throw 'Backup directory must not be "
            "on the target volume' }\n"
            "if (-not (Test-Path -LiteralPath $backupRootFull -PathType Container)) { throw "
            "'Backup directory does not exist' }\n"
            "$backupPath = Join-Path $backupRootFull ('sak-cluster-backup-{0}' -f "
            "[guid]::NewGuid().ToString('N'))\n"
            "New-Item -ItemType Directory -Force -Path $backupPath | Out-Null\n"
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
            "}\n"
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
            "if ($fileSystem -eq 'NTFS') { fsutil.exe fsinfo ntfsinfo $targetDrive }\n")
            .arg(quotePowerShell(letter.toUpper()),
                 quotePowerShell(fs),
                 uintArg(allocationUnit),
                 quotePowerShell(label),
                 quotePowerShell(QDir::toNativeSeparators(backupDirectory)));
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
    out.preview = toDisplayString(operation.type);
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
    const uint64_t targetOffset = payloadUInt64(operation, QStringLiteral("target_offset_bytes"));
    const uint64_t targetSize = payloadUInt64(operation, QStringLiteral("target_size_bytes"));
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
    if (targetOffset == 0 || targetSize == 0) {
        return invalidScript(QStringLiteral("Move Partition requires target offset and size"));
    }
    if (!isValidDriveLetter(drive)) {
        return invalidScript(QStringLiteral("Move Partition requires a mounted drive letter"));
    }
    if (!isSupportedFileSystem(fs)) {
        return invalidScript(QStringLiteral("Unsupported file system"));
    }
    if (backupDirectory.isEmpty()) {
        return invalidScript(QStringLiteral("Move Partition requires backup_directory"));
    }
    if (!payloadBool(operation, QStringLiteral("target_wipe_confirmed"))) {
        return invalidScript(QStringLiteral("Move Partition requires destructive confirmation"));
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
                     .arg(quotePowerShell(drive),
                          quotePowerShell(QDir::toNativeSeparators(backupDirectory)),
                          quotePowerShell(fs),
                          quotePowerShell(label),
                          uintArg(targetOffset),
                          uintArg(targetSize),
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

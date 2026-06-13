param(
    [Parameter(Mandatory = $true)]
    [int]$DiskNumber,

    [Parameter(Mandatory = $true)]
    [int]$PartitionNumber,

    [string]$ExpectedSerialNumber = "",
    [string]$ExpectedFriendlyNamePattern = "",
    [string]$VolumeName = "SAK APFS Raw Proof",
    [string]$RelabeledVolumeName = "SAK APFS Raw Relabeled",
    [string]$WriteFileName = "sak-apfs-raw-proof.txt",
    [string]$WriteFileText = "SAK APFS raw write and repair proof.",
    [string]$DirectoryName = "SAK Raw Proof Folder",
    [string]$ChildFileName = "sak-apfs-child-proof.txt",
    [string]$ChildFileText = "SAK APFS root-directory child-file proof.",
    [uint64]$PatchOffsetBytes = 8,
    [string]$PatchFileText = "PATCHED",
    [uint64]$ChildPatchOffsetBytes = 8,
    [string]$ChildPatchFileText = "CHILD",
    [uint64]$RepairCorruptMetadataBlock = 199,
    [string]$CertifierPath = "",
    [string]$ApfsWriterCliPath = "",
    [string]$OutputRoot = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]$identity
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Resolve-RepoPath {
    param([string]$RelativePath)
    return [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot $RelativePath))
}

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Invoke-Certifier {
    param([string]$Path, [string[]]$Arguments)
    $output = & $Path @Arguments 2>&1
    [pscustomobject]@{
        ExitCode = $LASTEXITCODE
        Output = @($output)
    }
}

function Write-Json {
    param([string]$Path, [object]$Value)
    $Value | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Get-Sha256Hex {
    param([string]$Text)
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
    return Get-Sha256HexFromBytes -Bytes $bytes
}

function Get-Sha256HexFromBytes {
    param([byte[]]$Bytes)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace("-", "").ToLowerInvariant()
    }
    finally {
        $sha.Dispose()
    }
}

function Get-PatchedUtf8Bytes {
    param([string]$OriginalText, [uint64]$OffsetBytes, [string]$PatchText)
    $originalBytes = [System.Text.Encoding]::UTF8.GetBytes($OriginalText)
    $patchBytes = [System.Text.Encoding]::UTF8.GetBytes($PatchText)
    Assert-Condition -Condition ($OffsetBytes -le [uint64]$originalBytes.Length) -Message "Patch offset is beyond file length."
    Assert-Condition -Condition ([uint64]$patchBytes.Length -le ([uint64]$originalBytes.Length - $OffsetBytes)) -Message "Patch text does not fit in existing file."
    [Array]::Copy($patchBytes, 0, $originalBytes, [int64]$OffsetBytes, $patchBytes.Length)
    return $originalBytes
}

if (-not (Test-IsAdmin)) {
    throw "APFS raw format validation must run elevated."
}
if (-not $Force) {
    throw "APFS raw format validation is destructive. Re-run with -Force against expendable media."
}

if ([string]::IsNullOrWhiteSpace($CertifierPath)) {
    $CertifierPath = Resolve-RepoPath "..\build\Release\partition_filesystem_probe_certifier.exe"
}
if ([string]::IsNullOrWhiteSpace($ApfsWriterCliPath)) {
    $ApfsWriterCliPath = Resolve-RepoPath "..\build\Release\sak_apfs_writer_cli.exe"
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Resolve-RepoPath "..\artifacts\partition-manager-certification\vm-lab\external-evidence\external.apfs-raw-format"
}

Assert-Condition -Condition (Test-Path -LiteralPath $CertifierPath -PathType Leaf) -Message "Certifier not found: $CertifierPath"
Assert-Condition -Condition (Test-Path -LiteralPath $ApfsWriterCliPath -PathType Leaf) -Message "APFS writer CLI not found: $ApfsWriterCliPath"

$runId = "run-" + (Get-Date -Format "yyyyMMdd-HHmmss")
$runRoot = Join-Path $OutputRoot $runId
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null

$reportPath = Join-Path $OutputRoot "report.json"
$formatReportPath = Join-Path $runRoot "apfs-raw-format-report.json"
$verifyReportPath = Join-Path $runRoot "apfs-raw-verify-report.json"
$writeReportPath = Join-Path $runRoot "apfs-raw-write-report.json"
$writeVerifyReportPath = Join-Path $runRoot "apfs-raw-write-verify-report.json"
$patchReportPath = Join-Path $runRoot "apfs-raw-patch-report.json"
$patchVerifyReportPath = Join-Path $runRoot "apfs-raw-patch-verify-report.json"
$directoryCreateReportPath = Join-Path $runRoot "apfs-raw-directory-create-report.json"
$directoryCreateVerifyReportPath = Join-Path $runRoot "apfs-raw-directory-create-verify-report.json"
$childPayloadPath = Join-Path $runRoot "apfs-raw-directory-child-payload.bin"
$childPatchPayloadPath = Join-Path $runRoot "apfs-raw-directory-child-patch-payload.bin"
$childWriteReportPath = Join-Path $runRoot "apfs-raw-directory-child-write-report.json"
$childWriteVerifyReportPath = Join-Path $runRoot "apfs-raw-directory-child-write-verify-report.json"
$childPatchReportPath = Join-Path $runRoot "apfs-raw-directory-child-patch-report.json"
$childPatchVerifyReportPath = Join-Path $runRoot "apfs-raw-directory-child-patch-verify-report.json"
$nonEmptyDirectoryDeleteReportPath = Join-Path $runRoot "apfs-raw-non-empty-directory-delete-report.json"
$childDeleteReportPath = Join-Path $runRoot "apfs-raw-directory-child-delete-report.json"
$childDeleteVerifyReportPath = Join-Path $runRoot "apfs-raw-directory-child-delete-verify-report.json"
$directoryDeleteReportPath = Join-Path $runRoot "apfs-raw-directory-delete-report.json"
$directoryDeleteVerifyReportPath = Join-Path $runRoot "apfs-raw-directory-delete-verify-report.json"
$repairReportPath = Join-Path $runRoot "apfs-raw-repair-report.json"
$repairVerifyReportPath = Join-Path $runRoot "apfs-raw-repair-verify-report.json"
$labelReportPath = Join-Path $runRoot "apfs-raw-volume-label-report.json"
$labelVerifyReportPath = Join-Path $runRoot "apfs-raw-volume-label-verify-report.json"
$deleteReportPath = Join-Path $runRoot "apfs-raw-delete-report.json"

try {
    $disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
    Assert-Condition -Condition (-not $disk.IsBoot -and -not $disk.IsSystem) -Message "Target disk is boot/system."
    Assert-Condition -Condition (-not $disk.IsReadOnly) -Message "Target disk is read-only."
    Assert-Condition -Condition ($disk.BusType -ne "Spaces") -Message "Storage Spaces disks are blocked."
    if (-not [string]::IsNullOrWhiteSpace($ExpectedSerialNumber)) {
        Assert-Condition -Condition ($disk.SerialNumber -eq $ExpectedSerialNumber) -Message "Disk serial mismatch."
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedFriendlyNamePattern)) {
        Assert-Condition -Condition ($disk.FriendlyName -match $ExpectedFriendlyNamePattern) -Message "Disk friendly name mismatch."
    }

    $partition = Get-Partition -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber -ErrorAction Stop
    $apfsGuid = "{7c3457ef-0000-11aa-aa11-00306543ecac}"
    $maxGeneratedSingleChunkBytes = [uint64]134217728
    Assert-Condition -Condition ([string]$partition.GptType -eq $apfsGuid) -Message "Target partition is not an Apple APFS GPT partition."
    Assert-Condition -Condition ([uint64]$partition.Size -ge [uint64]67108864) -Message "Target partition is smaller than APFS minimum."
    Assert-Condition -Condition ([uint64]$partition.Size -le $maxGeneratedSingleChunkBytes) -Message "Target partition exceeds the certified one-spaceman-chunk APFS generated layout limit (128 MiB); repartition to a 64-128 MiB APFS test slice or implement multi-CIB spaceman support first."

    $targetPath = "\\?\GLOBALROOT\Device\Harddisk$DiskNumber\Partition$PartitionNumber"
    $formatResult = Invoke-Certifier -Path $CertifierPath -Arguments @(
        "--output", $formatReportPath,
        "--apfs-format-existing-target", $targetPath,
        "--apfs-format-size-bytes", ([string][uint64]$partition.Size),
        "--apfs-format-block-size", "4096",
        "--apfs-format-volume-name", $VolumeName,
        "--apfs-format-target-wipe-confirmed",
        "--apfs-format-allow-raw-target",
        "--apfs-format-raw-hardware-proof")
    Assert-Condition -Condition ($formatResult.ExitCode -eq 0) -Message ("APFS raw format failed: " + ($formatResult.Output -join "`n"))

    $verifyResult = Invoke-Certifier -Path $CertifierPath -Arguments @(
        "--input", $targetPath,
        "--output", $verifyReportPath,
        "--expect", "APFS",
        "--apfs-list-path", "/")
    Assert-Condition -Condition ($verifyResult.ExitCode -eq 0) -Message ("APFS raw verify failed: " + ($verifyResult.Output -join "`n"))

    $formatReport = Get-Content -LiteralPath $formatReportPath -Raw | ConvertFrom-Json
    $verifyReport = Get-Content -LiteralPath $verifyReportPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($formatReport.status -eq "Passed") -Message "APFS raw format report did not pass."
    Assert-Condition -Condition ($verifyReport.status -eq "Passed") -Message "APFS raw verify report did not pass."
    Assert-Condition -Condition ($verifyReport.apfs_listing.volume_name -eq $VolumeName) -Message "APFS raw volume name did not round-trip."

    $writeResult = Invoke-Certifier -Path $CertifierPath -Arguments @(
        "--output", $writeReportPath,
        "--apfs-write-root-file-target", $targetPath,
        "--apfs-write-target-size-bytes", ([string][uint64]$partition.Size),
        "--apfs-write-root-file-name", $WriteFileName,
        "--apfs-write-root-file-text", $WriteFileText,
        "--apfs-write-target-confirmed",
        "--apfs-write-allow-raw-target",
        "--apfs-write-raw-hardware-proof")
    Assert-Condition -Condition ($writeResult.ExitCode -eq 0) -Message ("APFS raw write failed: " + ($writeResult.Output -join "`n"))

    $writeReadPath = "/" + $WriteFileName
    $writeVerifyResult = Invoke-Certifier -Path $CertifierPath -Arguments @(
        "--input", $targetPath,
        "--output", $writeVerifyReportPath,
        "--expect", "APFS",
        "--apfs-list-path", "/",
        "--apfs-read-file", $writeReadPath,
        "--apfs-read-max-bytes", ([string][System.Text.Encoding]::UTF8.GetByteCount($WriteFileText)))
    Assert-Condition -Condition ($writeVerifyResult.ExitCode -eq 0) -Message ("APFS raw write verify failed: " + ($writeVerifyResult.Output -join "`n"))

    $patchedFileBytes = Get-PatchedUtf8Bytes -OriginalText $WriteFileText -OffsetBytes $PatchOffsetBytes -PatchText $PatchFileText
    $expectedPatchedHash = Get-Sha256HexFromBytes -Bytes $patchedFileBytes
    $patchResult = Invoke-Certifier -Path $CertifierPath -Arguments @(
        "--output", $patchReportPath,
        "--apfs-patch-root-file-target", $targetPath,
        "--apfs-write-target-size-bytes", ([string][uint64]$partition.Size),
        "--apfs-patch-root-file-name", $WriteFileName,
        "--apfs-patch-root-file-offset-bytes", ([string]$PatchOffsetBytes),
        "--apfs-patch-root-file-text", $PatchFileText,
        "--apfs-write-target-confirmed",
        "--apfs-write-allow-raw-target",
        "--apfs-write-raw-hardware-proof")
    Assert-Condition -Condition ($patchResult.ExitCode -eq 0) -Message ("APFS raw patch failed: " + ($patchResult.Output -join "`n"))

    $patchVerifyResult = Invoke-Certifier -Path $CertifierPath -Arguments @(
        "--input", $targetPath,
        "--output", $patchVerifyReportPath,
        "--expect", "APFS",
        "--apfs-list-path", "/",
        "--apfs-read-file", $writeReadPath,
        "--apfs-read-max-bytes", ([string]$patchedFileBytes.Length))
    Assert-Condition -Condition ($patchVerifyResult.ExitCode -eq 0) -Message ("APFS raw patch verify failed: " + ($patchVerifyResult.Output -join "`n"))

    $directoryPath = "/" + $DirectoryName
    $directoryCreateResult = Invoke-Certifier -Path $ApfsWriterCliPath -Arguments @(
        "create-raw-root-directory",
        "--target", $targetPath,
        "--size-bytes", ([string][uint64]$partition.Size),
        "--directory-name", $DirectoryName,
        "--confirm-target",
        "--allow-raw-target",
        "--evidence-id", "external.apfs-raw-format.root-directory-create",
        "--output-json", $directoryCreateReportPath)
    Assert-Condition -Condition ($directoryCreateResult.ExitCode -eq 0) -Message ("APFS raw directory create failed: " + ($directoryCreateResult.Output -join "`n"))

    $directoryCreateVerifyResult = Invoke-Certifier -Path $CertifierPath -Arguments @(
        "--input", $targetPath,
        "--output", $directoryCreateVerifyReportPath,
        "--expect", "APFS",
        "--apfs-list-path", $directoryPath)
    Assert-Condition -Condition ($directoryCreateVerifyResult.ExitCode -eq 0) -Message ("APFS raw directory create verify failed: " + ($directoryCreateVerifyResult.Output -join "`n"))

    [System.IO.File]::WriteAllBytes($childPayloadPath, [System.Text.Encoding]::UTF8.GetBytes($ChildFileText))
    $childPath = $directoryPath + "/" + $ChildFileName
    $childWriteResult = Invoke-Certifier -Path $ApfsWriterCliPath -Arguments @(
        "write-raw-root-directory-file",
        "--target", $targetPath,
        "--size-bytes", ([string][uint64]$partition.Size),
        "--directory-name", $DirectoryName,
        "--file-name", $ChildFileName,
        "--payload-file", $childPayloadPath,
        "--confirm-target",
        "--allow-raw-target",
        "--evidence-id", "external.apfs-raw-format.root-directory-file-write",
        "--output-json", $childWriteReportPath)
    Assert-Condition -Condition ($childWriteResult.ExitCode -eq 0) -Message ("APFS raw directory child write failed: " + ($childWriteResult.Output -join "`n"))

    $childWriteVerifyResult = Invoke-Certifier -Path $CertifierPath -Arguments @(
        "--input", $targetPath,
        "--output", $childWriteVerifyReportPath,
        "--expect", "APFS",
        "--apfs-list-path", $directoryPath,
        "--apfs-read-file", $childPath,
        "--apfs-read-max-bytes", ([string][System.Text.Encoding]::UTF8.GetByteCount($ChildFileText)))
    Assert-Condition -Condition ($childWriteVerifyResult.ExitCode -eq 0) -Message ("APFS raw directory child write verify failed: " + ($childWriteVerifyResult.Output -join "`n"))

    [System.IO.File]::WriteAllBytes($childPatchPayloadPath, [System.Text.Encoding]::UTF8.GetBytes($ChildPatchFileText))
    $patchedChildFileBytes = Get-PatchedUtf8Bytes -OriginalText $ChildFileText -OffsetBytes $ChildPatchOffsetBytes -PatchText $ChildPatchFileText
    $expectedPatchedChildHash = Get-Sha256HexFromBytes -Bytes $patchedChildFileBytes
    $childPatchResult = Invoke-Certifier -Path $ApfsWriterCliPath -Arguments @(
        "patch-raw-root-directory-file",
        "--target", $targetPath,
        "--size-bytes", ([string][uint64]$partition.Size),
        "--directory-name", $DirectoryName,
        "--file-name", $ChildFileName,
        "--payload-file", $childPatchPayloadPath,
        "--patch-offset-bytes", ([string]$ChildPatchOffsetBytes),
        "--confirm-target",
        "--allow-raw-target",
        "--evidence-id", "external.apfs-raw-format.root-directory-file-patch",
        "--output-json", $childPatchReportPath)
    Assert-Condition -Condition ($childPatchResult.ExitCode -eq 0) -Message ("APFS raw directory child patch failed: " + ($childPatchResult.Output -join "`n"))

    $childPatchVerifyResult = Invoke-Certifier -Path $CertifierPath -Arguments @(
        "--input", $targetPath,
        "--output", $childPatchVerifyReportPath,
        "--expect", "APFS",
        "--apfs-list-path", $directoryPath,
        "--apfs-read-file", $childPath,
        "--apfs-read-max-bytes", ([string]$patchedChildFileBytes.Length))
    Assert-Condition -Condition ($childPatchVerifyResult.ExitCode -eq 0) -Message ("APFS raw directory child patch verify failed: " + ($childPatchVerifyResult.Output -join "`n"))

    $nonEmptyDirectoryDeleteResult = Invoke-Certifier -Path $ApfsWriterCliPath -Arguments @(
        "delete-raw-root-directory",
        "--target", $targetPath,
        "--size-bytes", ([string][uint64]$partition.Size),
        "--directory-name", $DirectoryName,
        "--confirm-target",
        "--allow-raw-target",
        "--evidence-id", "external.apfs-raw-format.root-directory-delete.non-empty-blocked",
        "--output-json", $nonEmptyDirectoryDeleteReportPath)
    Assert-Condition -Condition ($nonEmptyDirectoryDeleteResult.ExitCode -ne 0) -Message "APFS raw directory delete accepted a non-empty directory."

    $childDeleteResult = Invoke-Certifier -Path $ApfsWriterCliPath -Arguments @(
        "delete-raw-root-directory-file",
        "--target", $targetPath,
        "--size-bytes", ([string][uint64]$partition.Size),
        "--directory-name", $DirectoryName,
        "--file-name", $ChildFileName,
        "--confirm-target",
        "--allow-raw-target",
        "--evidence-id", "external.apfs-raw-format.root-directory-file-delete",
        "--output-json", $childDeleteReportPath)
    Assert-Condition -Condition ($childDeleteResult.ExitCode -eq 0) -Message ("APFS raw directory child delete failed: " + ($childDeleteResult.Output -join "`n"))

    $childDeleteVerifyResult = Invoke-Certifier -Path $CertifierPath -Arguments @(
        "--input", $targetPath,
        "--output", $childDeleteVerifyReportPath,
        "--expect", "APFS",
        "--apfs-list-path", $directoryPath)
    Assert-Condition -Condition ($childDeleteVerifyResult.ExitCode -eq 0) -Message ("APFS raw directory child delete verify failed: " + ($childDeleteVerifyResult.Output -join "`n"))

    $directoryDeleteResult = Invoke-Certifier -Path $ApfsWriterCliPath -Arguments @(
        "delete-raw-root-directory",
        "--target", $targetPath,
        "--size-bytes", ([string][uint64]$partition.Size),
        "--directory-name", $DirectoryName,
        "--confirm-target",
        "--allow-raw-target",
        "--evidence-id", "external.apfs-raw-format.root-directory-delete",
        "--output-json", $directoryDeleteReportPath)
    Assert-Condition -Condition ($directoryDeleteResult.ExitCode -eq 0) -Message ("APFS raw directory delete failed: " + ($directoryDeleteResult.Output -join "`n"))

    $directoryDeleteVerifyResult = Invoke-Certifier -Path $CertifierPath -Arguments @(
        "--input", $targetPath,
        "--output", $directoryDeleteVerifyReportPath,
        "--expect", "APFS",
        "--apfs-list-path", "/")
    Assert-Condition -Condition ($directoryDeleteVerifyResult.ExitCode -eq 0) -Message ("APFS raw directory delete verify failed: " + ($directoryDeleteVerifyResult.Output -join "`n"))

    $repairResult = Invoke-Certifier -Path $CertifierPath -Arguments @(
        "--output", $repairReportPath,
        "--apfs-repair-object-checksums-target", $targetPath,
        "--apfs-repair-target-size-bytes", ([string][uint64]$partition.Size),
        "--apfs-repair-target-confirmed",
        "--apfs-repair-allow-raw-target",
        "--apfs-repair-raw-hardware-proof",
        "--apfs-repair-corrupt-metadata-block", ([string]$RepairCorruptMetadataBlock),
        "--apfs-repair-read-file", $writeReadPath,
        "--apfs-repair-read-max-bytes", ([string]$patchedFileBytes.Length))
    Assert-Condition -Condition ($repairResult.ExitCode -eq 0) -Message ("APFS raw repair failed: " + ($repairResult.Output -join "`n"))

    $repairVerifyResult = Invoke-Certifier -Path $CertifierPath -Arguments @(
        "--input", $targetPath,
        "--output", $repairVerifyReportPath,
        "--expect", "APFS",
        "--apfs-list-path", "/",
        "--apfs-read-file", $writeReadPath,
        "--apfs-read-max-bytes", ([string]$patchedFileBytes.Length))
    Assert-Condition -Condition ($repairVerifyResult.ExitCode -eq 0) -Message ("APFS raw repair verify failed: " + ($repairVerifyResult.Output -join "`n"))

    $labelResult = Invoke-Certifier -Path $ApfsWriterCliPath -Arguments @(
        "change-raw-volume-label",
        "--target", $targetPath,
        "--size-bytes", ([string][uint64]$partition.Size),
        "--volume-name", $RelabeledVolumeName,
        "--confirm-target",
        "--allow-raw-target",
        "--evidence-id", "external.apfs-raw-format.volume-label-change",
        "--output-json", $labelReportPath)
    Assert-Condition -Condition ($labelResult.ExitCode -eq 0) -Message ("APFS raw volume-label change failed: " + ($labelResult.Output -join "`n"))

    $labelVerifyResult = Invoke-Certifier -Path $CertifierPath -Arguments @(
        "--input", $targetPath,
        "--output", $labelVerifyReportPath,
        "--expect", "APFS",
        "--apfs-list-path", "/",
        "--apfs-read-file", $writeReadPath,
        "--apfs-read-max-bytes", ([string]$patchedFileBytes.Length))
    Assert-Condition -Condition ($labelVerifyResult.ExitCode -eq 0) -Message ("APFS raw volume-label verify failed: " + ($labelVerifyResult.Output -join "`n"))

    $deleteResult = Invoke-Certifier -Path $CertifierPath -Arguments @(
        "--output", $deleteReportPath,
        "--apfs-delete-root-file-target", $targetPath,
        "--apfs-write-target-size-bytes", ([string][uint64]$partition.Size),
        "--apfs-delete-root-file-name", $WriteFileName,
        "--apfs-write-target-confirmed",
        "--apfs-write-allow-raw-target",
        "--apfs-write-raw-hardware-proof")
    Assert-Condition -Condition ($deleteResult.ExitCode -eq 0) -Message ("APFS raw delete failed: " + ($deleteResult.Output -join "`n"))

    $writeReport = Get-Content -LiteralPath $writeReportPath -Raw | ConvertFrom-Json
    $writeVerifyReport = Get-Content -LiteralPath $writeVerifyReportPath -Raw | ConvertFrom-Json
    $patchReport = Get-Content -LiteralPath $patchReportPath -Raw | ConvertFrom-Json
    $patchVerifyReport = Get-Content -LiteralPath $patchVerifyReportPath -Raw | ConvertFrom-Json
    $directoryCreateReport = Get-Content -LiteralPath $directoryCreateReportPath -Raw | ConvertFrom-Json
    $directoryCreateVerifyReport = Get-Content -LiteralPath $directoryCreateVerifyReportPath -Raw | ConvertFrom-Json
    $childWriteReport = Get-Content -LiteralPath $childWriteReportPath -Raw | ConvertFrom-Json
    $childWriteVerifyReport = Get-Content -LiteralPath $childWriteVerifyReportPath -Raw | ConvertFrom-Json
    $childPatchReport = Get-Content -LiteralPath $childPatchReportPath -Raw | ConvertFrom-Json
    $childPatchVerifyReport = Get-Content -LiteralPath $childPatchVerifyReportPath -Raw | ConvertFrom-Json
    $nonEmptyDirectoryDeleteReport = Get-Content -LiteralPath $nonEmptyDirectoryDeleteReportPath -Raw | ConvertFrom-Json
    $childDeleteReport = Get-Content -LiteralPath $childDeleteReportPath -Raw | ConvertFrom-Json
    $childDeleteVerifyReport = Get-Content -LiteralPath $childDeleteVerifyReportPath -Raw | ConvertFrom-Json
    $directoryDeleteReport = Get-Content -LiteralPath $directoryDeleteReportPath -Raw | ConvertFrom-Json
    $directoryDeleteVerifyReport = Get-Content -LiteralPath $directoryDeleteVerifyReportPath -Raw | ConvertFrom-Json
    $repairReport = Get-Content -LiteralPath $repairReportPath -Raw | ConvertFrom-Json
    $repairVerifyReport = Get-Content -LiteralPath $repairVerifyReportPath -Raw | ConvertFrom-Json
    $labelReport = Get-Content -LiteralPath $labelReportPath -Raw | ConvertFrom-Json
    $labelVerifyReport = Get-Content -LiteralPath $labelVerifyReportPath -Raw | ConvertFrom-Json
    $deleteReport = Get-Content -LiteralPath $deleteReportPath -Raw | ConvertFrom-Json
    $expectedWriteHash = Get-Sha256Hex -Text $WriteFileText
    Assert-Condition -Condition ($writeReport.status -eq "Passed") -Message "APFS raw write report did not pass."
    Assert-Condition -Condition ($writeVerifyReport.status -eq "Passed") -Message "APFS raw write verify report did not pass."
    Assert-Condition -Condition ($writeVerifyReport.apfs_read_file.sha256 -eq $expectedWriteHash) -Message "APFS raw write file hash did not round-trip."
    Assert-Condition -Condition ($patchReport.status -eq "Passed") -Message "APFS raw patch report did not pass."
    Assert-Condition -Condition ($patchVerifyReport.status -eq "Passed") -Message "APFS raw patch verify report did not pass."
    Assert-Condition -Condition ($patchVerifyReport.apfs_read_file.sha256 -eq $expectedPatchedHash) -Message "APFS raw patch file hash did not round-trip."
    Assert-Condition -Condition ([bool]$directoryCreateReport.ok) -Message "APFS raw directory create report did not pass."
    Assert-Condition -Condition ($directoryCreateVerifyReport.status -eq "Passed") -Message "APFS raw directory create verify report did not pass."
    Assert-Condition -Condition (@($directoryCreateVerifyReport.apfs_listing.entries).Count -eq 0) -Message "APFS raw created directory was not empty."
    $expectedChildHash = Get-Sha256Hex -Text $ChildFileText
    Assert-Condition -Condition ([bool]$childWriteReport.ok) -Message "APFS raw directory child write report did not pass."
    Assert-Condition -Condition ($childWriteVerifyReport.status -eq "Passed") -Message "APFS raw directory child write verify report did not pass."
    Assert-Condition -Condition ($childWriteVerifyReport.apfs_read_file.sha256 -eq $expectedChildHash) -Message "APFS raw directory child file hash did not round-trip."
    Assert-Condition -Condition ([bool]$childPatchReport.ok) -Message "APFS raw directory child patch report did not pass."
    Assert-Condition -Condition ($childPatchVerifyReport.status -eq "Passed") -Message "APFS raw directory child patch verify report did not pass."
    Assert-Condition -Condition ($childPatchVerifyReport.apfs_read_file.sha256 -eq $expectedPatchedChildHash) -Message "APFS raw directory child patch hash did not round-trip."
    Assert-Condition -Condition (-not [bool]$nonEmptyDirectoryDeleteReport.ok) -Message "APFS raw non-empty directory delete was not blocked."
    Assert-Condition -Condition (([string]::Join(" ", @($nonEmptyDirectoryDeleteReport.blockers))).Contains("empty")) -Message "APFS raw non-empty directory delete did not report empty-directory guard."
    Assert-Condition -Condition ([bool]$childDeleteReport.ok) -Message "APFS raw directory child delete report did not pass."
    Assert-Condition -Condition ($childDeleteReport.deleted_file_sha256 -eq $expectedPatchedChildHash) -Message "APFS raw directory child delete hash mismatch."
    Assert-Condition -Condition ($childDeleteVerifyReport.status -eq "Passed") -Message "APFS raw directory child delete verify report did not pass."
    Assert-Condition -Condition (@($childDeleteVerifyReport.apfs_listing.entries).Count -eq 0) -Message "APFS raw directory child delete left entries behind."
    Assert-Condition -Condition ([bool]$directoryDeleteReport.ok) -Message "APFS raw directory delete report did not pass."
    Assert-Condition -Condition ($directoryDeleteVerifyReport.status -eq "Passed") -Message "APFS raw directory delete verify report did not pass."
    $directoryDeleteRootNames = @($directoryDeleteVerifyReport.apfs_listing.entries | ForEach-Object { [string]$_.name })
    Assert-Condition -Condition ($directoryDeleteRootNames -notcontains $DirectoryName) -Message "APFS raw deleted directory still appears in root listing."
    Assert-Condition -Condition ($repairReport.status -eq "Passed") -Message "APFS raw repair report did not pass."
    Assert-Condition -Condition ([uint64]$repairReport.repaired_checksum_blocks -ge [uint64]1) -Message "APFS raw repair did not repair a corrupted checksum."
    Assert-Condition -Condition ($repairVerifyReport.status -eq "Passed") -Message "APFS raw repair verify report did not pass."
    Assert-Condition -Condition ($repairVerifyReport.apfs_read_file.sha256 -eq $expectedPatchedHash) -Message "APFS raw repair file hash did not round-trip."
    Assert-Condition -Condition ([bool]$labelReport.ok) -Message "APFS raw volume-label report did not pass."
    Assert-Condition -Condition ($labelReport.old_volume_name -eq $VolumeName) -Message "APFS raw volume-label old name mismatch."
    Assert-Condition -Condition ($labelReport.new_volume_name -eq $RelabeledVolumeName) -Message "APFS raw volume-label new name mismatch."
    Assert-Condition -Condition ($labelVerifyReport.status -eq "Passed") -Message "APFS raw volume-label verify report did not pass."
    Assert-Condition -Condition ($labelVerifyReport.apfs_listing.volume_name -eq $RelabeledVolumeName) -Message "APFS raw relabeled volume name did not round-trip."
    Assert-Condition -Condition ($labelVerifyReport.apfs_read_file.sha256 -eq $expectedPatchedHash) -Message "APFS raw relabel did not preserve patched file hash."
    Assert-Condition -Condition ($deleteReport.status -eq "Passed") -Message "APFS raw delete report did not pass."
    Assert-Condition -Condition ($deleteReport.raw_deleted_file_negative_read.status -eq "Passed") -Message "APFS raw delete negative read did not pass."

    $report = [ordered]@{
        status = "Passed"
        gate_id = "external.apfs-raw-format"
        destructive = $true
        disk_number = $DiskNumber
        partition_number = $PartitionNumber
        target_path = $targetPath
        disk_friendly_name = $disk.FriendlyName
        disk_serial_number = $disk.SerialNumber
        partition_size_bytes = [string][uint64]$partition.Size
        volume_name = $VolumeName
        relabeled_volume_name = $RelabeledVolumeName
        write_file_name = $WriteFileName
        write_file_sha256 = $expectedWriteHash
        patch_offset_bytes = [string]$PatchOffsetBytes
        patch_text_sha256 = (Get-Sha256Hex -Text $PatchFileText)
        patched_file_sha256 = $expectedPatchedHash
        directory_name = $DirectoryName
        child_file_name = $ChildFileName
        child_file_sha256 = $expectedChildHash
        child_patch_offset_bytes = [string]$ChildPatchOffsetBytes
        child_patch_text_sha256 = (Get-Sha256Hex -Text $ChildPatchFileText)
        child_patched_file_sha256 = $expectedPatchedChildHash
        certifier = [System.IO.Path]::GetFullPath($CertifierPath)
        apfs_writer_cli = [System.IO.Path]::GetFullPath($ApfsWriterCliPath)
        format_report = $formatReportPath
        verify_report = $verifyReportPath
        write_report = $writeReportPath
        write_verify_report = $writeVerifyReportPath
        patch_report = $patchReportPath
        patch_verify_report = $patchVerifyReportPath
        directory_create_report = $directoryCreateReportPath
        directory_create_verify_report = $directoryCreateVerifyReportPath
        child_write_report = $childWriteReportPath
        child_write_verify_report = $childWriteVerifyReportPath
        child_patch_report = $childPatchReportPath
        child_patch_verify_report = $childPatchVerifyReportPath
        non_empty_directory_delete_report = $nonEmptyDirectoryDeleteReportPath
        child_delete_report = $childDeleteReportPath
        child_delete_verify_report = $childDeleteVerifyReportPath
        directory_delete_report = $directoryDeleteReportPath
        directory_delete_verify_report = $directoryDeleteVerifyReportPath
        repair_report = $repairReportPath
        repair_verify_report = $repairVerifyReportPath
        label_report = $labelReportPath
        label_verify_report = $labelVerifyReportPath
        delete_report = $deleteReportPath
        completed_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    }
    Write-Json -Path $reportPath -Value $report
    Write-Host "APFS raw format validation passed: $reportPath"
}
catch {
    $report = [ordered]@{
        status = "Failed"
        gate_id = "external.apfs-raw-format"
        destructive = $true
        disk_number = $DiskNumber
        partition_number = $PartitionNumber
        error = $_.Exception.Message
        completed_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    }
    Write-Json -Path $reportPath -Value $report
    throw
}

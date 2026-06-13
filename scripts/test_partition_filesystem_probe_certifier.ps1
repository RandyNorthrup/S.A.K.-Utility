<#
.SYNOPSIS
    Self-tests the raw filesystem probe certifier CLI.

.DESCRIPTION
    Generates tiny local fixtures under artifacts/ and verifies that
    partition_filesystem_probe_certifier.exe honors --input-offset-bytes,
    reports deterministic JSON, and rejects invalid offset input.
#>

[CmdletBinding()]
param(
    [string]$CertifierPath = "",
    [string]$OutputRoot = "artifacts\partition-manager-certification\tool-tests\filesystem-probe-certifier"
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)

function Assert-Condition {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Resolve-CertifierPath {
    param([string]$Path)

    if (-not [string]::IsNullOrWhiteSpace($Path)) {
        $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
        return $resolved.Path
    }

    $candidate = Join-Path $ProjectRoot "build\Release\partition_filesystem_probe_certifier.exe"
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        throw "partition_filesystem_probe_certifier.exe was not found. Build target partition_filesystem_probe_certifier first."
    }
    return (Resolve-Path -LiteralPath $candidate).Path
}

function Write-BytesAt {
    param(
        [Parameter(Mandatory = $true)]
        [System.IO.FileStream]$Stream,
        [Parameter(Mandatory = $true)]
        [int64]$Offset,
        [Parameter(Mandatory = $true)]
        [byte[]]$Bytes
    )

    $Stream.Position = $Offset
    $Stream.Write($Bytes, 0, $Bytes.Length)
}

function Write-UInt32LittleEndianAt {
    param(
        [Parameter(Mandatory = $true)]
        [System.IO.FileStream]$Stream,
        [Parameter(Mandatory = $true)]
        [int64]$Offset,
        [Parameter(Mandatory = $true)]
        [uint32]$Value
    )

    Write-BytesAt -Stream $Stream -Offset $Offset -Bytes ([System.BitConverter]::GetBytes($Value))
}

function Toggle-ByteAt {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [int64]$Offset,
        [Parameter(Mandatory = $true)]
        [byte]$Mask
    )

    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::Read)
    try {
        $stream.Position = $Offset
        $value = $stream.ReadByte()
        if ($value -lt 0) {
            throw "Unable to read byte at offset $Offset in $Path"
        }
        $stream.Position = $Offset
        $stream.WriteByte([byte]($value -bxor $Mask))
    }
    finally {
        $stream.Dispose()
    }
}

function New-OffsetExt2Fixture {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [int64]$OffsetBytes
    )

    $parent = Split-Path -Parent $Path
    New-Item -ItemType Directory -Path $parent -Force | Out-Null

    $stream = [System.IO.File]::Open(
        $Path,
        [System.IO.FileMode]::Create,
        [System.IO.FileAccess]::ReadWrite,
        [System.IO.FileShare]::Read)
    try {
        $fixtureBytesAfterOffset = 4096
        $stream.SetLength($OffsetBytes + $fixtureBytesAfterOffset)
        $superblock = $OffsetBytes + 1024

        Write-UInt32LittleEndianAt -Stream $stream -Offset ($superblock + 0x00) -Value 128
        Write-UInt32LittleEndianAt -Stream $stream -Offset ($superblock + 0x04) -Value 4096
        Write-UInt32LittleEndianAt -Stream $stream -Offset ($superblock + 0x0C) -Value 2048
        Write-UInt32LittleEndianAt -Stream $stream -Offset ($superblock + 0x10) -Value 64
        Write-UInt32LittleEndianAt -Stream $stream -Offset ($superblock + 0x18) -Value 0
        Write-UInt32LittleEndianAt -Stream $stream -Offset ($superblock + 0x20) -Value 8192
        Write-UInt32LittleEndianAt -Stream $stream -Offset ($superblock + 0x28) -Value 128
        Write-BytesAt -Stream $stream -Offset ($superblock + 0x38) -Bytes ([byte[]](0x53, 0xEF))
        Write-BytesAt -Stream $stream -Offset ($superblock + 0x78) -Bytes ([System.Text.Encoding]::ASCII.GetBytes("SAK_OFFSET_TEST"))
    }
    finally {
        $stream.Dispose()
    }
}

function Invoke-Certifier {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    $global:LASTEXITCODE = 0
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $output = & $Path @Arguments 2>&1 | ForEach-Object { $_.ToString() }
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    return [pscustomobject]@{
        ExitCode = $LASTEXITCODE
        Output = @($output)
    }
}

Push-Location $ProjectRoot
try {
    $resolvedCertifier = Resolve-CertifierPath -Path $CertifierPath
    $resolvedOutputRoot = if ([System.IO.Path]::IsPathRooted($OutputRoot)) {
        $OutputRoot
    }
    else {
        Join-Path $ProjectRoot $OutputRoot
    }
    $runRoot = Join-Path $resolvedOutputRoot ("run-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
    New-Item -ItemType Directory -Path $runRoot -Force | Out-Null

    $offsetBytes = 4096
    $fixturePath = Join-Path $runRoot "offset-ext2-fixture.bin"
    $reportPath = Join-Path $runRoot "offset-ext2-report.json"
    New-OffsetExt2Fixture -Path $fixturePath -OffsetBytes $offsetBytes

    $result = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $fixturePath,
        "--input-offset-bytes", [string]$offsetBytes,
        "--output", $reportPath,
        "--expect", "ext2")
    Assert-Condition -Condition ($result.ExitCode -eq 0) -Message ("offset probe failed: " + ($result.Output -join "`n"))
    Assert-Condition -Condition (Test-Path -LiteralPath $reportPath -PathType Leaf) -Message "offset probe report missing"

    $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    $fixtureLength = (Get-Item -LiteralPath $fixturePath).Length
    Assert-Condition -Condition ($report.status -eq "Passed") -Message "offset probe status mismatch"
    Assert-Condition -Condition ($report.detected_file_system -eq "ext2") -Message "offset probe filesystem mismatch"
    Assert-Condition -Condition ([uint64]$report.input_offset_bytes -eq [uint64]$offsetBytes) -Message "offset probe JSON offset mismatch"
    Assert-Condition -Condition ([uint64]$report.input_size_bytes -eq [uint64]($fixtureLength - $offsetBytes)) -Message "offset probe input size mismatch"
    Assert-Condition -Condition ([uint64]$report.total_bytes -eq 4194304) -Message "offset probe total bytes mismatch"
    Assert-Condition -Condition ([uint64]$report.free_bytes -eq 2097152) -Message "offset probe free bytes mismatch"
    Assert-Condition -Condition ((@($report.details) -join "`n").Contains("Volume label: SAK_OFFSET_TEST")) -Message "offset probe details missing volume label"

    $invalidReportPath = Join-Path $runRoot "invalid-offset-report.json"
    $invalidResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $fixturePath,
        "--input-offset-bytes", "not-a-number",
        "--output", $invalidReportPath,
        "--expect", "ext2")
    Assert-Condition -Condition ($invalidResult.ExitCode -ne 0) -Message "invalid offset unexpectedly passed"
    Assert-Condition -Condition (Test-Path -LiteralPath $invalidReportPath -PathType Leaf) -Message "invalid offset report missing"
    $invalidReport = Get-Content -LiteralPath $invalidReportPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($invalidReport.status -eq "Failed") -Message "invalid offset report status mismatch"
    Assert-Condition -Condition ($invalidReport.error.Contains("--input-offset-bytes")) -Message "invalid offset report missing error"

    $invalidHfsMaxReportPath = Join-Path $runRoot "invalid-hfs-max-report.json"
    $invalidHfsMaxResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $fixturePath,
        "--output", $invalidHfsMaxReportPath,
        "--expect", "ext2",
        "--hfs-read-max-bytes", "not-a-number")
    Assert-Condition -Condition ($invalidHfsMaxResult.ExitCode -ne 0) -Message "invalid HFS max bytes unexpectedly passed"
    Assert-Condition -Condition (Test-Path -LiteralPath $invalidHfsMaxReportPath -PathType Leaf) -Message "invalid HFS max bytes report missing"
    $invalidHfsMaxReport = Get-Content -LiteralPath $invalidHfsMaxReportPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($invalidHfsMaxReport.status -eq "Failed") -Message "invalid HFS max bytes report status mismatch"
    Assert-Condition -Condition ($invalidHfsMaxReport.error.Contains("--hfs-read-max-bytes")) -Message "invalid HFS max bytes report missing error"

    $invalidHfsAttributeIdReportPath = Join-Path $runRoot "invalid-hfs-attribute-id-report.json"
    $invalidHfsAttributeIdResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $fixturePath,
        "--output", $invalidHfsAttributeIdReportPath,
        "--expect", "ext2",
        "--hfs-read-attribute-file-id", "not-a-number",
        "--hfs-read-attribute-name", "com.apple.FinderInfo")
    Assert-Condition -Condition ($invalidHfsAttributeIdResult.ExitCode -ne 0) -Message "invalid HFS attribute ID unexpectedly passed"
    Assert-Condition -Condition (Test-Path -LiteralPath $invalidHfsAttributeIdReportPath -PathType Leaf) -Message "invalid HFS attribute ID report missing"
    $invalidHfsAttributeIdReport = Get-Content -LiteralPath $invalidHfsAttributeIdReportPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($invalidHfsAttributeIdReport.status -eq "Failed") -Message "invalid HFS attribute ID report status mismatch"
    Assert-Condition -Condition ($invalidHfsAttributeIdReport.error.Contains("--hfs-read-attribute-file-id")) -Message "invalid HFS attribute ID report missing error"

    $invalidApfsMaxReportPath = Join-Path $runRoot "invalid-apfs-max-report.json"
    $invalidApfsMaxResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $fixturePath,
        "--output", $invalidApfsMaxReportPath,
        "--expect", "ext2",
        "--apfs-read-max-bytes", "not-a-number")
    Assert-Condition -Condition ($invalidApfsMaxResult.ExitCode -ne 0) -Message "invalid APFS max bytes unexpectedly passed"
    Assert-Condition -Condition (Test-Path -LiteralPath $invalidApfsMaxReportPath -PathType Leaf) -Message "invalid APFS max bytes report missing"
    $invalidApfsMaxReport = Get-Content -LiteralPath $invalidApfsMaxReportPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($invalidApfsMaxReport.status -eq "Failed") -Message "invalid APFS max bytes report status mismatch"
    Assert-Condition -Condition ($invalidApfsMaxReport.error.Contains("--apfs-read-max-bytes")) -Message "invalid APFS max bytes report missing error"

    $invalidApfsExportMaxReportPath = Join-Path $runRoot "invalid-apfs-export-max-report.json"
    $invalidApfsExportMaxResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $fixturePath,
        "--output", $invalidApfsExportMaxReportPath,
        "--expect", "ext2",
        "--apfs-export-max-entries", "not-a-number")
    Assert-Condition -Condition ($invalidApfsExportMaxResult.ExitCode -ne 0) -Message "invalid APFS export max unexpectedly passed"
    Assert-Condition -Condition (Test-Path -LiteralPath $invalidApfsExportMaxReportPath -PathType Leaf) -Message "invalid APFS export max report missing"
    $invalidApfsExportMaxReport = Get-Content -LiteralPath $invalidApfsExportMaxReportPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($invalidApfsExportMaxReport.status -eq "Failed") -Message "invalid APFS export max report status mismatch"
    Assert-Condition -Condition ($invalidApfsExportMaxReport.error.Contains("--apfs-export-max-entries")) -Message "invalid APFS export max report missing error"

    $wrongHfsReportPath = Join-Path $runRoot "wrong-hfs-operation-report.json"
    $wrongHfsResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $fixturePath,
        "--input-offset-bytes", [string]$offsetBytes,
        "--output", $wrongHfsReportPath,
        "--expect", "ext2",
        "--hfs-check",
        "--hfs-list-path", "/")
    Assert-Condition -Condition ($wrongHfsResult.ExitCode -ne 0) -Message "HFS operation on ext2 unexpectedly passed"
    Assert-Condition -Condition (Test-Path -LiteralPath $wrongHfsReportPath -PathType Leaf) -Message "wrong HFS operation report missing"
    $wrongHfsReport = Get-Content -LiteralPath $wrongHfsReportPath -Raw | ConvertFrom-Json
    $wrongHfsBlockers = (@($wrongHfsReport.blockers) + @($wrongHfsReport.hfs_operation_blockers)) -join "`n"
    Assert-Condition -Condition ($wrongHfsReport.status -eq "Failed") -Message "wrong HFS operation report status mismatch"
    Assert-Condition -Condition ($wrongHfsBlockers.Contains("HFS check/browse/read/attribute proof requires detected HFS+ or HFSX")) -Message "wrong HFS operation blocker missing filesystem reason"

    $wrongApfsReportPath = Join-Path $runRoot "wrong-apfs-operation-report.json"
    $wrongApfsResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $fixturePath,
        "--input-offset-bytes", [string]$offsetBytes,
        "--output", $wrongApfsReportPath,
        "--expect", "ext2",
        "--apfs-list-path", "/")
    Assert-Condition -Condition ($wrongApfsResult.ExitCode -ne 0) -Message "APFS operation on ext2 unexpectedly passed"
    Assert-Condition -Condition (Test-Path -LiteralPath $wrongApfsReportPath -PathType Leaf) -Message "wrong APFS operation report missing"
    $wrongApfsReport = Get-Content -LiteralPath $wrongApfsReportPath -Raw | ConvertFrom-Json
    $wrongApfsBlockers = (@($wrongApfsReport.blockers) + @($wrongApfsReport.apfs_operation_blockers)) -join "`n"
    Assert-Condition -Condition ($wrongApfsReport.status -eq "Failed") -Message "wrong APFS operation report status mismatch"
    Assert-Condition -Condition ($wrongApfsBlockers.Contains("APFS browse/read proof does not support input offsets")) -Message "wrong APFS operation offset blocker missing"
    Assert-Condition -Condition ($wrongApfsBlockers.Contains("APFS browse/read proof requires detected APFS")) -Message "wrong APFS operation filesystem blocker missing"

    $apfsFormatImagePath = Join-Path $runRoot "generated-format.apfs"
    $apfsFormatReportPath = Join-Path $runRoot "generated-format-report.json"
    $apfsSeedTextBuilder = [System.Text.StringBuilder]::new()
    while ($apfsSeedTextBuilder.Length -lt 9000) {
        [void]$apfsSeedTextBuilder.Append("APFS seed file proof across contiguous writer blocks. ")
    }
    $apfsSeedText = $apfsSeedTextBuilder.ToString().Substring(0, 9000)
    $apfsFormatResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--output", $apfsFormatReportPath,
        "--apfs-build-format-image", $apfsFormatImagePath,
        "--apfs-format-size-bytes", "67108864",
        "--apfs-format-block-size", "4096",
        "--apfs-format-volume-name", "SAK Empty",
        "--apfs-format-seed-file-name", "proof.txt",
        "--apfs-format-seed-file-text", $apfsSeedText)
    Assert-Condition -Condition ($apfsFormatResult.ExitCode -eq 0) -Message ("APFS format image build failed: " + ($apfsFormatResult.Output -join "`n"))
    Assert-Condition -Condition (Test-Path -LiteralPath $apfsFormatImagePath -PathType Leaf) -Message "APFS format image missing"
    Assert-Condition -Condition (Test-Path -LiteralPath $apfsFormatReportPath -PathType Leaf) -Message "APFS format report missing"
    $apfsFormatReport = Get-Content -LiteralPath $apfsFormatReportPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($apfsFormatReport.status -eq "Passed") -Message "APFS format report status mismatch"
    Assert-Condition -Condition ($apfsFormatReport.generated_detection.file_system -eq "APFS") -Message "APFS format detection mismatch"
    Assert-Condition -Condition ([uint64]$apfsFormatReport.generated_detection.free_bytes -gt 0) -Message "APFS format free bytes missing"
    Assert-Condition -Condition ((@($apfsFormatReport.generated_detection.details) -join "`n").Contains("APFS free bytes")) -Message "APFS format spaceman details missing"
    Assert-Condition -Condition ([uint64]$apfsFormatReport.target_container_bytes -eq [uint64]67108864) -Message "APFS format target size mismatch"
    Assert-Condition -Condition ([int]$apfsFormatReport.block_size_bytes -eq 4096) -Message "APFS format block size mismatch"
    Assert-Condition -Condition ($apfsFormatReport.volume_name -eq "SAK Empty") -Message "APFS format volume name mismatch"
    Assert-Condition -Condition ($apfsFormatReport.generated_apfs_listing.status -eq "Passed") -Message "APFS generated root listing failed"
    Assert-Condition -Condition ($apfsFormatReport.generated_apfs_listing.volume_name -eq "SAK Empty") -Message "APFS generated root listing volume mismatch"
    Assert-Condition -Condition ([int]$apfsFormatReport.generated_apfs_listing.entry_count -eq 1) -Message "APFS generated root listing seed count mismatch"
    Assert-Condition -Condition ($apfsFormatReport.generated_seed_file_read.status -eq "Passed") -Message "APFS generated seed read failed"
    Assert-Condition -Condition ([int]$apfsFormatReport.generated_seed_file_read.bytes_read -eq $apfsSeedText.Length) -Message "APFS generated seed byte count mismatch"
    Assert-Condition -Condition (@($apfsFormatReport.plan_steps).Count -ge 6) -Message "APFS format plan steps missing"
    Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace($apfsFormatReport.image_sha256)) -Message "APFS format image hash missing"

    $apfsEmptyImagePath = Join-Path $runRoot "generated-empty.apfs"
    $apfsEmptyReportPath = Join-Path $runRoot "generated-empty-report.json"
    $apfsEmptyResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--output", $apfsEmptyReportPath,
        "--apfs-build-format-image", $apfsEmptyImagePath,
        "--apfs-format-size-bytes", "67108864",
        "--apfs-format-block-size", "4096",
        "--apfs-format-volume-name", "SAK Empty")
    Assert-Condition -Condition ($apfsEmptyResult.ExitCode -eq 0) -Message ("APFS empty format image build failed: " + ($apfsEmptyResult.Output -join "`n"))
    $apfsEmptyReport = Get-Content -LiteralPath $apfsEmptyReportPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($apfsEmptyReport.status -eq "Passed") -Message "APFS empty format report status mismatch"
    Assert-Condition -Condition ([int]$apfsEmptyReport.generated_apfs_listing.entry_count -eq 0) -Message "APFS empty root listing mismatch"

    $apfsExistingImagePath = Join-Path $runRoot "generated-existing-format.apfs"
    $apfsExistingStream = [System.IO.File]::Open($apfsExistingImagePath, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    try {
        $apfsExistingStream.SetLength(67108864)
    }
    finally {
        $apfsExistingStream.Dispose()
    }
    $apfsExistingReportPath = Join-Path $runRoot "generated-existing-format-report.json"
    $apfsExistingResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--output", $apfsExistingReportPath,
        "--apfs-format-existing-target", $apfsExistingImagePath,
        "--apfs-format-size-bytes", "67108864",
        "--apfs-format-block-size", "4096",
        "--apfs-format-volume-name", "SAK Existing",
        "--apfs-format-target-wipe-confirmed")
    Assert-Condition -Condition ($apfsExistingResult.ExitCode -eq 0) -Message ("APFS existing-target format failed: " + ($apfsExistingResult.Output -join "`n"))
    $apfsExistingReport = Get-Content -LiteralPath $apfsExistingReportPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($apfsExistingReport.status -eq "Passed") -Message "APFS existing-target format report status mismatch"
    Assert-Condition -Condition ($apfsExistingReport.operation -eq "APFS existing-target format") -Message "APFS existing-target operation mismatch"
    Assert-Condition -Condition ($apfsExistingReport.generated_detection.file_system -eq "APFS") -Message "APFS existing-target detection mismatch"
    Assert-Condition -Condition ($apfsExistingReport.generated_apfs_listing.volume_name -eq "SAK Existing") -Message "APFS existing-target volume name mismatch"
    Assert-Condition -Condition ([int]$apfsExistingReport.generated_apfs_listing.entry_count -eq 0) -Message "APFS existing-target root listing mismatch"

    $apfsExistingBlockedImagePath = Join-Path $runRoot "generated-existing-format-blocked.apfs"
    $apfsExistingBlockedStream = [System.IO.File]::Open($apfsExistingBlockedImagePath, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    try {
        $apfsExistingBlockedStream.SetLength(67108864)
    }
    finally {
        $apfsExistingBlockedStream.Dispose()
    }
    $apfsExistingBlockedReportPath = Join-Path $runRoot "generated-existing-format-blocked-report.json"
    $apfsExistingBlockedResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--output", $apfsExistingBlockedReportPath,
        "--apfs-format-existing-target", $apfsExistingBlockedImagePath,
        "--apfs-format-size-bytes", "67108864",
        "--apfs-format-block-size", "4096",
        "--apfs-format-volume-name", "SAK Existing")
    Assert-Condition -Condition ($apfsExistingBlockedResult.ExitCode -ne 0) -Message "APFS existing-target format without confirmation unexpectedly passed"
    $apfsExistingBlockedReport = Get-Content -LiteralPath $apfsExistingBlockedReportPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ((@($apfsExistingBlockedReport.blockers) -join "`n").Contains("confirmation")) -Message "APFS existing-target confirmation blocker missing"

    $apfsWriteImagePath = Join-Path $runRoot "generated-write.apfs"
    $apfsWriteReportPath = Join-Path $runRoot "generated-write-report.json"
    $apfsWriteResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $apfsEmptyImagePath,
        "--output", $apfsWriteReportPath,
        "--apfs-write-root-file-image", $apfsWriteImagePath,
        "--apfs-write-root-file-name", "proof.txt",
        "--apfs-write-root-file-text", $apfsSeedText)
    Assert-Condition -Condition ($apfsWriteResult.ExitCode -eq 0) -Message ("APFS root-file write failed: " + ($apfsWriteResult.Output -join "`n"))
    $apfsWriteReport = Get-Content -LiteralPath $apfsWriteReportPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($apfsWriteReport.status -eq "Passed") -Message "APFS write report status mismatch"
    Assert-Condition -Condition ($apfsWriteReport.plan_operation -eq "Create file") -Message "APFS empty-root write operation mismatch"
    Assert-Condition -Condition ([uint64]$apfsWriteReport.written_data_blocks -gt [uint64]1) -Message "APFS write data block count mismatch"
    Assert-Condition -Condition ($apfsWriteReport.written_detection.file_system -eq "APFS") -Message "APFS write detection mismatch"
    Assert-Condition -Condition ($apfsWriteReport.written_apfs_listing.status -eq "Passed") -Message "APFS write listing failed"
    Assert-Condition -Condition ([int]$apfsWriteReport.written_apfs_listing.entry_count -eq 1) -Message "APFS write root listing seed count mismatch"
    Assert-Condition -Condition ($apfsWriteReport.written_file_read.status -eq "Passed") -Message "APFS write read-back failed"
    Assert-Condition -Condition ([int]$apfsWriteReport.written_file_read.bytes_read -eq $apfsSeedText.Length) -Message "APFS write read-back byte count mismatch"
    Assert-Condition -Condition ($apfsWriteReport.written_file_read.sha256 -eq $apfsFormatReport.generated_seed_file_read.sha256) -Message "APFS write read-back hash mismatch"

    $apfsNonEmptyWriteReportPath = Join-Path $runRoot "generated-non-empty-write-report.json"
    $apfsNonEmptyWriteImagePath = Join-Path $runRoot "generated-non-empty-write.apfs"
    $apfsSecondSeedText = "APFS second generated root file proof."
    $apfsNonEmptyWriteResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $apfsFormatImagePath,
        "--output", $apfsNonEmptyWriteReportPath,
        "--apfs-write-root-file-image", $apfsNonEmptyWriteImagePath,
        "--apfs-write-root-file-name", "other.txt",
        "--apfs-write-root-file-text", $apfsSecondSeedText)
    Assert-Condition -Condition ($apfsNonEmptyWriteResult.ExitCode -eq 0) -Message ("APFS non-empty write failed: " + ($apfsNonEmptyWriteResult.Output -join "`n"))
    $apfsNonEmptyWriteReport = Get-Content -LiteralPath $apfsNonEmptyWriteReportPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($apfsNonEmptyWriteReport.status -eq "Passed") -Message "APFS non-empty write status mismatch"
    Assert-Condition -Condition ($apfsNonEmptyWriteReport.plan_operation -eq "Create file") -Message "APFS non-empty add operation mismatch"
    Assert-Condition -Condition ([int]$apfsNonEmptyWriteReport.written_apfs_listing.entry_count -eq 2) -Message "APFS non-empty write root count mismatch"
    Assert-Condition -Condition ([int]$apfsNonEmptyWriteReport.written_file_read.bytes_read -eq $apfsSecondSeedText.Length) -Message "APFS non-empty write target byte count mismatch"

    $apfsPreservedReadReportPath = Join-Path $runRoot "generated-non-empty-preserved-read-report.json"
    $apfsPreservedReadResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $apfsNonEmptyWriteImagePath,
        "--output", $apfsPreservedReadReportPath,
        "--expect", "APFS",
        "--apfs-read-file", "/proof.txt",
        "--apfs-read-max-bytes", ([string]$apfsSeedText.Length))
    Assert-Condition -Condition ($apfsPreservedReadResult.ExitCode -eq 0) -Message ("APFS preserved read failed: " + ($apfsPreservedReadResult.Output -join "`n"))
    $apfsPreservedReadReport = Get-Content -LiteralPath $apfsPreservedReadReportPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($apfsPreservedReadReport.apfs_read_file.status -eq "Passed") -Message "APFS preserved read status mismatch"
    Assert-Condition -Condition ($apfsPreservedReadReport.apfs_read_file.sha256 -eq $apfsFormatReport.generated_seed_file_read.sha256) -Message "APFS preserved read hash mismatch"

    $apfsReplaceReportPath = Join-Path $runRoot "generated-replace-write-report.json"
    $apfsReplaceImagePath = Join-Path $runRoot "generated-replace-write.apfs"
    $apfsReplacementText = "APFS replacement root file proof with new bytes."
    $apfsReplaceResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $apfsNonEmptyWriteImagePath,
        "--output", $apfsReplaceReportPath,
        "--apfs-write-root-file-image", $apfsReplaceImagePath,
        "--apfs-write-root-file-name", "proof.txt",
        "--apfs-write-root-file-text", $apfsReplacementText)
    Assert-Condition -Condition ($apfsReplaceResult.ExitCode -eq 0) -Message ("APFS replace write failed: " + ($apfsReplaceResult.Output -join "`n"))
    $apfsReplaceReport = Get-Content -LiteralPath $apfsReplaceReportPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($apfsReplaceReport.status -eq "Passed") -Message "APFS replace write status mismatch"
    Assert-Condition -Condition ($apfsReplaceReport.plan_operation -eq "Replace file") -Message "APFS replace operation mismatch"
    Assert-Condition -Condition ([int]$apfsReplaceReport.written_apfs_listing.entry_count -eq 2) -Message "APFS replace root count mismatch"
    Assert-Condition -Condition ([int]$apfsReplaceReport.written_file_read.bytes_read -eq $apfsReplacementText.Length) -Message "APFS replace target byte count mismatch"
    Assert-Condition -Condition ($apfsReplaceReport.written_file_read.sha256 -ne $apfsFormatReport.generated_seed_file_read.sha256) -Message "APFS replace hash did not change"

    $apfsReplacePreservedReportPath = Join-Path $runRoot "generated-replace-preserved-read-report.json"
    $apfsReplacePreservedResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $apfsReplaceImagePath,
        "--output", $apfsReplacePreservedReportPath,
        "--expect", "APFS",
        "--apfs-read-file", "/other.txt",
        "--apfs-read-max-bytes", ([string]$apfsSecondSeedText.Length))
    Assert-Condition -Condition ($apfsReplacePreservedResult.ExitCode -eq 0) -Message ("APFS replace preserved read failed: " + ($apfsReplacePreservedResult.Output -join "`n"))
    $apfsReplacePreservedReport = Get-Content -LiteralPath $apfsReplacePreservedReportPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($apfsReplacePreservedReport.apfs_read_file.status -eq "Passed") -Message "APFS replace preserved read status mismatch"
    Assert-Condition -Condition ($apfsReplacePreservedReport.apfs_read_file.sha256 -eq $apfsNonEmptyWriteReport.written_file_read.sha256) -Message "APFS replace preserved read hash mismatch"

    $apfsPatchReportPath = Join-Path $runRoot "generated-patch-report.json"
    $apfsPatchImagePath = Join-Path $runRoot "generated-patch.apfs"
    $apfsPatchText = "PATCHED-BY-SAK"
    $apfsPatchOffset = 7
    $apfsPatchResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $apfsReplaceImagePath,
        "--output", $apfsPatchReportPath,
        "--apfs-patch-root-file-image", $apfsPatchImagePath,
        "--apfs-patch-root-file-name", "proof.txt",
        "--apfs-patch-root-file-offset-bytes", ([string]$apfsPatchOffset),
        "--apfs-patch-root-file-text", $apfsPatchText)
    Assert-Condition -Condition ($apfsPatchResult.ExitCode -eq 0) -Message ("APFS patch failed: " + ($apfsPatchResult.Output -join "`n"))
    $apfsPatchReport = Get-Content -LiteralPath $apfsPatchReportPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($apfsPatchReport.status -eq "Passed") -Message "APFS patch status mismatch"
    Assert-Condition -Condition ($apfsPatchReport.plan_operation -eq "Replace file") -Message "APFS patch operation mismatch"
    Assert-Condition -Condition ([uint64]$apfsPatchReport.patch_offset_bytes -eq [uint64]$apfsPatchOffset) -Message "APFS patch offset mismatch"
    Assert-Condition -Condition ([uint64]$apfsPatchReport.patch_bytes -eq [uint64]$apfsPatchText.Length) -Message "APFS patch byte count mismatch"
    Assert-Condition -Condition ($apfsPatchReport.patched_file_read.status -eq "Passed") -Message "APFS patch read-back failed"
    Assert-Condition -Condition ([int]$apfsPatchReport.patched_file_read.bytes_read -eq $apfsReplacementText.Length) -Message "APFS patch file size changed"
    Assert-Condition -Condition ($apfsPatchReport.patched_file_read.sha256 -ne $apfsReplaceReport.written_file_read.sha256) -Message "APFS patch hash did not change"

    $apfsDeleteReportPath = Join-Path $runRoot "generated-delete-report.json"
    $apfsDeleteImagePath = Join-Path $runRoot "generated-delete.apfs"
    $apfsDeleteResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $apfsPatchImagePath,
        "--output", $apfsDeleteReportPath,
        "--apfs-delete-root-file-image", $apfsDeleteImagePath,
        "--apfs-delete-root-file-name", "other.txt")
    Assert-Condition -Condition ($apfsDeleteResult.ExitCode -eq 0) -Message ("APFS delete failed: " + ($apfsDeleteResult.Output -join "`n"))
    $apfsDeleteReport = Get-Content -LiteralPath $apfsDeleteReportPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($apfsDeleteReport.status -eq "Passed") -Message "APFS delete status mismatch"
    Assert-Condition -Condition ($apfsDeleteReport.plan_operation -eq "Delete file") -Message "APFS delete operation mismatch"
    Assert-Condition -Condition ([int]$apfsDeleteReport.deleted_apfs_listing.entry_count -eq 1) -Message "APFS delete root count mismatch"
    Assert-Condition -Condition ($apfsDeleteReport.deleted_file_negative_read.status -eq "Passed") -Message "APFS delete negative read mismatch"
    Assert-Condition -Condition ($apfsDeleteReport.deleted_file_sha256 -eq $apfsNonEmptyWriteReport.written_file_read.sha256) -Message "APFS delete removed-file hash mismatch"

    $apfsRawPatchBlockedReportPath = Join-Path $runRoot "raw-patch-normal-file-blocked-report.json"
    $apfsRawPatchBlockedResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--output", $apfsRawPatchBlockedReportPath,
        "--apfs-patch-root-file-target", $apfsReplaceImagePath,
        "--apfs-write-target-size-bytes", "67108864",
        "--apfs-patch-root-file-name", "proof.txt",
        "--apfs-patch-root-file-offset-bytes", "0",
        "--apfs-patch-root-file-text", "BLOCKED",
        "--apfs-write-target-confirmed",
        "--apfs-write-allow-raw-target",
        "--apfs-write-raw-hardware-proof")
    Assert-Condition -Condition ($apfsRawPatchBlockedResult.ExitCode -ne 0) -Message "APFS raw patch normal-file target unexpectedly passed"
    $apfsRawPatchBlockedReport = Get-Content -LiteralPath $apfsRawPatchBlockedReportPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($apfsRawPatchBlockedReport.status -eq "Failed") -Message "APFS raw patch blocker status mismatch"
    Assert-Condition -Condition ((@($apfsRawPatchBlockedReport.blockers) -join "`n").Contains("Windows raw-device path")) -Message "APFS raw patch normal-file blocker missing"

    $apfsRawDeleteBlockedReportPath = Join-Path $runRoot "raw-delete-normal-file-blocked-report.json"
    $apfsRawDeleteBlockedResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--output", $apfsRawDeleteBlockedReportPath,
        "--apfs-delete-root-file-target", $apfsPatchImagePath,
        "--apfs-write-target-size-bytes", "67108864",
        "--apfs-delete-root-file-name", "other.txt",
        "--apfs-write-target-confirmed",
        "--apfs-write-allow-raw-target",
        "--apfs-write-raw-hardware-proof")
    Assert-Condition -Condition ($apfsRawDeleteBlockedResult.ExitCode -ne 0) -Message "APFS raw delete normal-file target unexpectedly passed"
    $apfsRawDeleteBlockedReport = Get-Content -LiteralPath $apfsRawDeleteBlockedReportPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($apfsRawDeleteBlockedReport.status -eq "Failed") -Message "APFS raw delete blocker status mismatch"
    Assert-Condition -Condition ((@($apfsRawDeleteBlockedReport.blockers) -join "`n").Contains("Windows raw-device path")) -Message "APFS raw delete normal-file blocker missing"

    $apfsCorruptImagePath = Join-Path $runRoot "generated-format-corrupt.apfs"
    Copy-Item -LiteralPath $apfsFormatImagePath -Destination $apfsCorruptImagePath -Force
    Toggle-ByteAt -Path $apfsCorruptImagePath -Offset (197 * 4096) -Mask 0x5A

    $apfsCorruptReadReportPath = Join-Path $runRoot "generated-format-corrupt-read-report.json"
    $apfsCorruptReadResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $apfsCorruptImagePath,
        "--output", $apfsCorruptReadReportPath,
        "--expect", "APFS",
        "--apfs-list-path", "/")
    Assert-Condition -Condition ($apfsCorruptReadResult.ExitCode -ne 0) -Message "Corrupt APFS checksum unexpectedly listed"
    $apfsCorruptReadReport = Get-Content -LiteralPath $apfsCorruptReadReportPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($apfsCorruptReadReport.status -eq "Failed") -Message "Corrupt APFS read report status mismatch"
    Assert-Condition -Condition ((@($apfsCorruptReadReport.apfs_listing.blockers) -join "`n").Contains("checksum")) -Message "Corrupt APFS read did not report checksum blocker"

    $apfsRepairImagePath = Join-Path $runRoot "generated-format-repaired.apfs"
    $apfsRepairReportPath = Join-Path $runRoot "generated-format-repair-report.json"
    $apfsRepairResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $apfsCorruptImagePath,
        "--output", $apfsRepairReportPath,
        "--apfs-repair-object-checksums", $apfsRepairImagePath,
        "--apfs-repair-read-file", "/proof.txt",
        "--apfs-repair-read-max-bytes", ([string]$apfsSeedText.Length))
    Assert-Condition -Condition ($apfsRepairResult.ExitCode -eq 0) -Message ("APFS checksum repair failed: " + ($apfsRepairResult.Output -join "`n"))
    Assert-Condition -Condition (Test-Path -LiteralPath $apfsRepairImagePath -PathType Leaf) -Message "APFS repair image missing"
    $apfsRepairReport = Get-Content -LiteralPath $apfsRepairReportPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($apfsRepairReport.status -eq "Passed") -Message "APFS repair report status mismatch"
    Assert-Condition -Condition ([uint64]$apfsRepairReport.repaired_checksum_blocks -eq [uint64]1) -Message "APFS repaired checksum block count mismatch"
    Assert-Condition -Condition ($apfsRepairReport.repaired_detection.file_system -eq "APFS") -Message "APFS repair detection mismatch"
    Assert-Condition -Condition ($apfsRepairReport.repaired_apfs_listing.status -eq "Passed") -Message "APFS repair listing failed"
    Assert-Condition -Condition ($apfsRepairReport.repaired_file_read.status -eq "Passed") -Message "APFS repair read-back failed"
    Assert-Condition -Condition ([int]$apfsRepairReport.repaired_file_read.bytes_read -eq $apfsSeedText.Length) -Message "APFS repair read-back byte count mismatch"
    Assert-Condition -Condition ($apfsRepairReport.repaired_file_read.sha256 -eq $apfsFormatReport.generated_seed_file_read.sha256) -Message "APFS repair read-back hash mismatch"

    $apfsCleanRepairReportPath = Join-Path $runRoot "generated-format-clean-repair-report.json"
    $apfsCleanRepairImagePath = Join-Path $runRoot "generated-format-clean-repair.apfs"
    $apfsCleanRepairResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $apfsFormatImagePath,
        "--output", $apfsCleanRepairReportPath,
        "--apfs-repair-object-checksums", $apfsCleanRepairImagePath)
    Assert-Condition -Condition ($apfsCleanRepairResult.ExitCode -ne 0) -Message "Clean APFS repair unexpectedly passed"
    $apfsCleanRepairReport = Get-Content -LiteralPath $apfsCleanRepairReportPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ((@($apfsCleanRepairReport.blockers) -join "`n").Contains("did not find")) -Message "Clean APFS repair blocker missing"

    $apfsFormatOverwriteReportPath = Join-Path $runRoot "generated-format-overwrite-report.json"
    $apfsFormatOverwriteResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--output", $apfsFormatOverwriteReportPath,
        "--apfs-build-format-image", $apfsFormatImagePath,
        "--apfs-format-size-bytes", "67108864",
        "--apfs-format-block-size", "4096",
        "--apfs-format-volume-name", "SAK Empty")
    Assert-Condition -Condition ($apfsFormatOverwriteResult.ExitCode -ne 0) -Message "APFS format overwrite unexpectedly passed"
    $apfsFormatOverwriteReport = Get-Content -LiteralPath $apfsFormatOverwriteReportPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($apfsFormatOverwriteReport.status -eq "Failed") -Message "APFS format overwrite status mismatch"
    Assert-Condition -Condition ((@($apfsFormatOverwriteReport.blockers) -join "`n").Contains("overwrite")) -Message "APFS format overwrite blocker missing"

    Write-Host "partition_filesystem_probe_certifier self-test passed: $runRoot"
    $global:LASTEXITCODE = 0
}
finally {
    Pop-Location
}

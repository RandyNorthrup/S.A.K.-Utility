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

# The certifier's own JSON report is the certification evidence, so it has to be read
# strictly. PowerShell coercion would otherwise let a malformed report satisfy these
# checks: a JSON boolean true compares equal to any nonempty string ("Passed", "APFS"),
# a missing field casts to 0 and satisfies an expected-zero check, and $null -eq $null
# makes two absent hashes compare equal.
function Read-CertifierReport {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "certifier report missing: $Path"
    }
    $raw = Get-Content -LiteralPath $Path -Raw
    if ([string]::IsNullOrWhiteSpace($raw)) {
        throw "certifier report is empty: $Path"
    }
    $parsed = @($raw | ConvertFrom-Json)
    if ($parsed.Count -ne 1 -or $parsed[0] -isnot [pscustomobject]) {
        throw "certifier report is not a single JSON object: $Path"
    }
    return $parsed[0]
}

function Get-ReportText {
    param($Value, [string]$Message)

    if ($Value -isnot [string]) {
        throw "$Message (expected a JSON string)"
    }
    return [string]$Value
}

function Assert-ReportText {
    param($Actual, [string]$Expected, [string]$Message)

    if ($Actual -isnot [string]) {
        throw "$Message (expected the JSON string '$Expected', got a non-string value)"
    }
    if ($Actual -cne $Expected) {
        throw "$Message (expected '$Expected', got '$Actual')"
    }
}

# Accepts a JSON number or the decimal-digit string form the tools emit for 64-bit
# counters. Rejects $null, booleans, arrays and any other text, so a missing field can no
# longer cast to 0 and satisfy an expected-zero assertion.
function Get-ReportNumber {
    param($Value, [string]$Message)

    if ($null -eq $Value -or $Value -is [bool] -or $Value -is [System.Array]) {
        throw "$Message (expected a number, got no usable value)"
    }
    if ($Value -is [string]) {
        if ($Value -notmatch '^[0-9]+$') {
            throw "$Message (expected a decimal number, got '$Value')"
        }
        return [decimal]$Value
    }
    if ($Value -isnot [int] -and $Value -isnot [long] -and $Value -isnot [decimal] -and
        $Value -isnot [double]) {
        throw "$Message (expected a number, got $($Value.GetType().Name))"
    }
    return [decimal]$Value
}

function Assert-ReportNumber {
    param($Actual, [decimal]$Expected, [string]$Message)

    $value = Get-ReportNumber $Actual $Message
    if ($value -ne $Expected) {
        throw "$Message (expected $Expected, got $value)"
    }
}

function Assert-ReportNumberGreater {
    param($Actual, $Floor, [string]$Message)

    $value = Get-ReportNumber $Actual $Message
    $floorValue = Get-ReportNumber $Floor $Message
    if ($value -le $floorValue) {
        throw "$Message (expected greater than $floorValue, got $value)"
    }
}

# A hash is only evidence when it really is a hash: two absent fields would otherwise
# compare equal and pass a "hashes match" check.
function Get-ReportSha256 {
    param($Value, [string]$Message)

    if ($Value -isnot [string] -or $Value -notmatch '^[0-9a-f]{64}$') {
        throw "$Message (expected a 64-character SHA-256 hex string)"
    }
    return [string]$Value
}

function Assert-ReportSha256Equals {
    param($Actual, $Expected, [string]$Message)

    $actualHash = Get-ReportSha256 $Actual $Message
    $expectedHash = Get-ReportSha256 $Expected $Message
    if ($actualHash -ne $expectedHash) {
        throw "$Message (expected $expectedHash, got $actualHash)"
    }
}

function Assert-ReportSha256Differs {
    param($Actual, $Expected, [string]$Message)

    $actualHash = Get-ReportSha256 $Actual $Message
    $expectedHash = Get-ReportSha256 $Expected $Message
    if ($actualHash -eq $expectedHash) {
        throw "$Message (both hashes are $actualHash)"
    }
}

# The resolved path is executed, so it must be the tool this self-test certifies and
# nothing else: any other existing executable would otherwise be run in its place.
function Assert-CertifierBinary {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "certifier path is not a file: $Path"
    }
    if ([System.IO.Path]::GetFileName($Path) -ne "partition_filesystem_probe_certifier.exe") {
        throw "certifier path must name partition_filesystem_probe_certifier.exe: $Path"
    }
    return $Path
}

function Resolve-CertifierPath {
    param([string]$Path)

    if (-not [string]::IsNullOrEmpty($Path)) {
        if ([string]::IsNullOrWhiteSpace($Path)) {
            throw "-CertifierPath was whitespace. Pass a real path or omit it entirely."
        }
        $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
        return (Assert-CertifierBinary $resolved.Path)
    }

    $candidate = Join-Path $ProjectRoot "build\Release\partition_filesystem_probe_certifier.exe"
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        throw "partition_filesystem_probe_certifier.exe was not found. Build target partition_filesystem_probe_certifier first."
    }
    return (Assert-CertifierBinary (Resolve-Path -LiteralPath $candidate).Path)
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

    # $LASTEXITCODE is cleared to $null rather than 0 so a tool that never launches cannot
    # leave a success code behind; "Continue" only stops a nonzero exit from terminating
    # the run, it does not make a launch failure look like a clean exit.
    $global:LASTEXITCODE = $null
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $output = & $Path @Arguments 2>&1 | ForEach-Object { $_.ToString() }
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    if ($null -eq $LASTEXITCODE) {
        throw ("certifier did not launch: " + $Path + "`n" + (@($output) -join "`n"))
    }
    return [pscustomobject]@{
        ExitCode = $LASTEXITCODE
        Output = @($output)
    }
}

Push-Location $ProjectRoot
try {
    $resolvedCertifier = Resolve-CertifierPath -Path $CertifierPath
    if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
        throw "-OutputRoot must name a directory; an empty value would drop fixtures into the project root."
    }
    $resolvedOutputRoot = if ([System.IO.Path]::IsPathRooted($OutputRoot)) {
        $OutputRoot
    }
    else {
        Join-Path $ProjectRoot $OutputRoot
    }
    New-Item -ItemType Directory -Path $resolvedOutputRoot -Force | Out-Null
    $resolvedOutputRoot = (Resolve-Path -LiteralPath $resolvedOutputRoot).Path
    if ((Get-Item -LiteralPath $resolvedOutputRoot -Force).Attributes.HasFlag([System.IO.FileAttributes]::ReparsePoint)) {
        throw "-OutputRoot must not be a reparse point: $resolvedOutputRoot"
    }
    # Created without -Force: destructive-write fixtures go in a directory this run owns.
    # A pre-existing (or substituted) run root fails closed instead of being reused.
    $runRoot = Join-Path $resolvedOutputRoot `
        ("run-" + (Get-Date -Format "yyyyMMdd-HHmmss") + "-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $runRoot | Out-Null

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

    $report = Read-CertifierReport $reportPath
    $fixtureLength = (Get-Item -LiteralPath $fixturePath).Length
    Assert-ReportText $report.status "Passed" "offset probe status mismatch"
    Assert-ReportText $report.detected_file_system "ext2" "offset probe filesystem mismatch"
    Assert-ReportNumber $report.input_offset_bytes $offsetBytes "offset probe JSON offset mismatch"
    Assert-ReportNumber $report.input_size_bytes ($fixtureLength - $offsetBytes) "offset probe input size mismatch"
    Assert-ReportNumber $report.total_bytes 4194304 "offset probe total bytes mismatch"
    Assert-ReportNumber $report.free_bytes 2097152 "offset probe free bytes mismatch"
    Assert-Condition -Condition ((@($report.details) -join "`n").Contains("Volume label: SAK_OFFSET_TEST")) -Message "offset probe details missing volume label"

    $invalidReportPath = Join-Path $runRoot "invalid-offset-report.json"
    $invalidResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $fixturePath,
        "--input-offset-bytes", "not-a-number",
        "--output", $invalidReportPath,
        "--expect", "ext2")
    Assert-Condition -Condition ($invalidResult.ExitCode -ne 0) -Message "invalid offset unexpectedly passed"
    Assert-Condition -Condition (Test-Path -LiteralPath $invalidReportPath -PathType Leaf) -Message "invalid offset report missing"
    $invalidReport = Read-CertifierReport $invalidReportPath
    Assert-ReportText $invalidReport.status "Failed" "invalid offset report status mismatch"
    Assert-Condition -Condition ((Get-ReportText $invalidReport.error "invalid offset report missing error").Contains("--input-offset-bytes")) -Message "invalid offset report missing error"

    $invalidHfsMaxReportPath = Join-Path $runRoot "invalid-hfs-max-report.json"
    $invalidHfsMaxResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $fixturePath,
        "--output", $invalidHfsMaxReportPath,
        "--expect", "ext2",
        "--hfs-read-max-bytes", "not-a-number")
    Assert-Condition -Condition ($invalidHfsMaxResult.ExitCode -ne 0) -Message "invalid HFS max bytes unexpectedly passed"
    Assert-Condition -Condition (Test-Path -LiteralPath $invalidHfsMaxReportPath -PathType Leaf) -Message "invalid HFS max bytes report missing"
    $invalidHfsMaxReport = Read-CertifierReport $invalidHfsMaxReportPath
    Assert-ReportText $invalidHfsMaxReport.status "Failed" "invalid HFS max bytes report status mismatch"
    Assert-Condition -Condition ((Get-ReportText $invalidHfsMaxReport.error "invalid HFS max bytes report missing error").Contains("--hfs-read-max-bytes")) -Message "invalid HFS max bytes report missing error"

    $invalidHfsAttributeIdReportPath = Join-Path $runRoot "invalid-hfs-attribute-id-report.json"
    $invalidHfsAttributeIdResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $fixturePath,
        "--output", $invalidHfsAttributeIdReportPath,
        "--expect", "ext2",
        "--hfs-read-attribute-file-id", "not-a-number",
        "--hfs-read-attribute-name", "com.apple.FinderInfo")
    Assert-Condition -Condition ($invalidHfsAttributeIdResult.ExitCode -ne 0) -Message "invalid HFS attribute ID unexpectedly passed"
    Assert-Condition -Condition (Test-Path -LiteralPath $invalidHfsAttributeIdReportPath -PathType Leaf) -Message "invalid HFS attribute ID report missing"
    $invalidHfsAttributeIdReport = Read-CertifierReport $invalidHfsAttributeIdReportPath
    Assert-ReportText $invalidHfsAttributeIdReport.status "Failed" "invalid HFS attribute ID report status mismatch"
    Assert-Condition -Condition ((Get-ReportText $invalidHfsAttributeIdReport.error "invalid HFS attribute ID report missing error").Contains("--hfs-read-attribute-file-id")) -Message "invalid HFS attribute ID report missing error"

    $invalidApfsMaxReportPath = Join-Path $runRoot "invalid-apfs-max-report.json"
    $invalidApfsMaxResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $fixturePath,
        "--output", $invalidApfsMaxReportPath,
        "--expect", "ext2",
        "--apfs-read-max-bytes", "not-a-number")
    Assert-Condition -Condition ($invalidApfsMaxResult.ExitCode -ne 0) -Message "invalid APFS max bytes unexpectedly passed"
    Assert-Condition -Condition (Test-Path -LiteralPath $invalidApfsMaxReportPath -PathType Leaf) -Message "invalid APFS max bytes report missing"
    $invalidApfsMaxReport = Read-CertifierReport $invalidApfsMaxReportPath
    Assert-ReportText $invalidApfsMaxReport.status "Failed" "invalid APFS max bytes report status mismatch"
    Assert-Condition -Condition ((Get-ReportText $invalidApfsMaxReport.error "invalid APFS max bytes report missing error").Contains("--apfs-read-max-bytes")) -Message "invalid APFS max bytes report missing error"

    $invalidApfsExportMaxReportPath = Join-Path $runRoot "invalid-apfs-export-max-report.json"
    $invalidApfsExportMaxResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $fixturePath,
        "--output", $invalidApfsExportMaxReportPath,
        "--expect", "ext2",
        "--apfs-export-max-entries", "not-a-number")
    Assert-Condition -Condition ($invalidApfsExportMaxResult.ExitCode -ne 0) -Message "invalid APFS export max unexpectedly passed"
    Assert-Condition -Condition (Test-Path -LiteralPath $invalidApfsExportMaxReportPath -PathType Leaf) -Message "invalid APFS export max report missing"
    $invalidApfsExportMaxReport = Read-CertifierReport $invalidApfsExportMaxReportPath
    Assert-ReportText $invalidApfsExportMaxReport.status "Failed" "invalid APFS export max report status mismatch"
    Assert-Condition -Condition ((Get-ReportText $invalidApfsExportMaxReport.error "invalid APFS export max report missing error").Contains("--apfs-export-max-entries")) -Message "invalid APFS export max report missing error"

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
    $wrongHfsReport = Read-CertifierReport $wrongHfsReportPath
    $wrongHfsBlockers = (@($wrongHfsReport.blockers) + @($wrongHfsReport.hfs_operation_blockers)) -join "`n"
    Assert-ReportText $wrongHfsReport.status "Failed" "wrong HFS operation report status mismatch"
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
    $wrongApfsReport = Read-CertifierReport $wrongApfsReportPath
    $wrongApfsBlockers = (@($wrongApfsReport.blockers) + @($wrongApfsReport.apfs_operation_blockers)) -join "`n"
    Assert-ReportText $wrongApfsReport.status "Failed" "wrong APFS operation report status mismatch"
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
    $apfsFormatReport = Read-CertifierReport $apfsFormatReportPath
    Assert-ReportText $apfsFormatReport.status "Passed" "APFS format report status mismatch"
    Assert-ReportText $apfsFormatReport.generated_detection.file_system "APFS" "APFS format detection mismatch"
    Assert-ReportNumberGreater $apfsFormatReport.generated_detection.free_bytes 0 "APFS format free bytes missing"
    Assert-Condition -Condition ((@($apfsFormatReport.generated_detection.details) -join "`n").Contains("APFS free bytes")) -Message "APFS format spaceman details missing"
    Assert-ReportNumber $apfsFormatReport.target_container_bytes 67108864 "APFS format target size mismatch"
    Assert-ReportNumber $apfsFormatReport.block_size_bytes 4096 "APFS format block size mismatch"
    Assert-ReportText $apfsFormatReport.volume_name "SAK Empty" "APFS format volume name mismatch"
    Assert-ReportText $apfsFormatReport.generated_apfs_listing.status "Passed" "APFS generated root listing failed"
    Assert-ReportText $apfsFormatReport.generated_apfs_listing.volume_name "SAK Empty" "APFS generated root listing volume mismatch"
    Assert-ReportNumber $apfsFormatReport.generated_apfs_listing.entry_count 1 "APFS generated root listing seed count mismatch"
    Assert-ReportText $apfsFormatReport.generated_seed_file_read.status "Passed" "APFS generated seed read failed"
    Assert-ReportNumber $apfsFormatReport.generated_seed_file_read.bytes_read $apfsSeedText.Length "APFS generated seed byte count mismatch"
    Assert-Condition -Condition (@($apfsFormatReport.plan_steps).Count -ge 6) -Message "APFS format plan steps missing"
    [void](Get-ReportSha256 $apfsFormatReport.image_sha256 "APFS format image hash missing")
    # Independent of the report: the image the tool claims it built is on disk at the size
    # it was asked for.
    Assert-Condition -Condition ((Get-Item -LiteralPath $apfsFormatImagePath).Length -eq 67108864) -Message "APFS format image size mismatch on disk"

    $apfsEmptyImagePath = Join-Path $runRoot "generated-empty.apfs"
    $apfsEmptyReportPath = Join-Path $runRoot "generated-empty-report.json"
    $apfsEmptyResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--output", $apfsEmptyReportPath,
        "--apfs-build-format-image", $apfsEmptyImagePath,
        "--apfs-format-size-bytes", "67108864",
        "--apfs-format-block-size", "4096",
        "--apfs-format-volume-name", "SAK Empty")
    Assert-Condition -Condition ($apfsEmptyResult.ExitCode -eq 0) -Message ("APFS empty format image build failed: " + ($apfsEmptyResult.Output -join "`n"))
    $apfsEmptyReport = Read-CertifierReport $apfsEmptyReportPath
    Assert-ReportText $apfsEmptyReport.status "Passed" "APFS empty format report status mismatch"
    Assert-ReportNumber $apfsEmptyReport.generated_apfs_listing.entry_count 0 "APFS empty root listing mismatch"
    Assert-Condition -Condition ((Get-Item -LiteralPath $apfsEmptyImagePath).Length -eq 67108864) -Message "APFS empty format image size mismatch on disk"

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
    $apfsExistingReport = Read-CertifierReport $apfsExistingReportPath
    Assert-ReportText $apfsExistingReport.status "Passed" "APFS existing-target format report status mismatch"
    Assert-ReportText $apfsExistingReport.operation "APFS existing-target format" "APFS existing-target operation mismatch"
    Assert-ReportText $apfsExistingReport.generated_detection.file_system "APFS" "APFS existing-target detection mismatch"
    Assert-ReportText $apfsExistingReport.generated_apfs_listing.volume_name "SAK Existing" "APFS existing-target volume name mismatch"
    Assert-ReportNumber $apfsExistingReport.generated_apfs_listing.entry_count 0 "APFS existing-target root listing mismatch"

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
    $apfsExistingBlockedReport = Read-CertifierReport $apfsExistingBlockedReportPath
    Assert-ReportText $apfsExistingBlockedReport.status "Failed" "APFS existing-target blocked status mismatch"
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
    $apfsWriteReport = Read-CertifierReport $apfsWriteReportPath
    Assert-ReportText $apfsWriteReport.status "Passed" "APFS write report status mismatch"
    Assert-ReportText $apfsWriteReport.operation "APFS image-only root-file write" "APFS empty-root write operation mismatch"
    Assert-ReportNumberGreater $apfsWriteReport.new_xid $apfsWriteReport.previous_xid "APFS write transaction id did not advance"
    Assert-ReportText $apfsWriteReport.written_detection.file_system "APFS" "APFS write detection mismatch"
    Assert-ReportText $apfsWriteReport.written_apfs_listing.status "Passed" "APFS write listing failed"
    Assert-ReportNumber $apfsWriteReport.written_apfs_listing.entry_count 1 "APFS write root listing seed count mismatch"
    Assert-ReportText $apfsWriteReport.written_file_read.status "Passed" "APFS write read-back failed"
    Assert-ReportNumber $apfsWriteReport.written_file_read.bytes_read $apfsSeedText.Length "APFS write read-back byte count mismatch"
    Assert-ReportSha256Equals $apfsWriteReport.written_file_read.sha256 $apfsFormatReport.generated_seed_file_read.sha256 "APFS write read-back hash mismatch"

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
    $apfsNonEmptyWriteReport = Read-CertifierReport $apfsNonEmptyWriteReportPath
    Assert-ReportText $apfsNonEmptyWriteReport.status "Passed" "APFS non-empty write status mismatch"
    Assert-ReportText $apfsNonEmptyWriteReport.operation "APFS image-only root-file write" "APFS non-empty add operation mismatch"
    Assert-ReportNumber $apfsNonEmptyWriteReport.written_apfs_listing.entry_count 2 "APFS non-empty write root count mismatch"
    Assert-ReportNumber $apfsNonEmptyWriteReport.written_file_read.bytes_read $apfsSecondSeedText.Length "APFS non-empty write target byte count mismatch"

    $apfsPreservedReadReportPath = Join-Path $runRoot "generated-non-empty-preserved-read-report.json"
    $apfsPreservedReadResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $apfsNonEmptyWriteImagePath,
        "--output", $apfsPreservedReadReportPath,
        "--expect", "APFS",
        "--apfs-read-file", "/proof.txt",
        "--apfs-read-max-bytes", ([string]$apfsSeedText.Length))
    Assert-Condition -Condition ($apfsPreservedReadResult.ExitCode -eq 0) -Message ("APFS preserved read failed: " + ($apfsPreservedReadResult.Output -join "`n"))
    $apfsPreservedReadReport = Read-CertifierReport $apfsPreservedReadReportPath
    Assert-ReportText $apfsPreservedReadReport.apfs_read_file.status "Passed" "APFS preserved read status mismatch"
    Assert-ReportSha256Equals $apfsPreservedReadReport.apfs_read_file.sha256 $apfsFormatReport.generated_seed_file_read.sha256 "APFS preserved read hash mismatch"

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
    $apfsReplaceReport = Read-CertifierReport $apfsReplaceReportPath
    Assert-ReportText $apfsReplaceReport.status "Passed" "APFS replace write status mismatch"
    Assert-ReportText $apfsReplaceReport.operation "APFS image-only root-file write" "APFS replace operation mismatch"
    Assert-ReportNumber $apfsReplaceReport.written_apfs_listing.entry_count 2 "APFS replace root count mismatch"
    Assert-ReportNumber $apfsReplaceReport.written_file_read.bytes_read $apfsReplacementText.Length "APFS replace target byte count mismatch"
    Assert-ReportSha256Differs $apfsReplaceReport.written_file_read.sha256 $apfsFormatReport.generated_seed_file_read.sha256 "APFS replace hash did not change"

    $apfsReplacePreservedReportPath = Join-Path $runRoot "generated-replace-preserved-read-report.json"
    $apfsReplacePreservedResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $apfsReplaceImagePath,
        "--output", $apfsReplacePreservedReportPath,
        "--expect", "APFS",
        "--apfs-read-file", "/other.txt",
        "--apfs-read-max-bytes", ([string]$apfsSecondSeedText.Length))
    Assert-Condition -Condition ($apfsReplacePreservedResult.ExitCode -eq 0) -Message ("APFS replace preserved read failed: " + ($apfsReplacePreservedResult.Output -join "`n"))
    $apfsReplacePreservedReport = Read-CertifierReport $apfsReplacePreservedReportPath
    Assert-ReportText $apfsReplacePreservedReport.apfs_read_file.status "Passed" "APFS replace preserved read status mismatch"
    Assert-ReportSha256Equals $apfsReplacePreservedReport.apfs_read_file.sha256 $apfsNonEmptyWriteReport.written_file_read.sha256 "APFS replace preserved read hash mismatch"

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
    $apfsPatchReport = Read-CertifierReport $apfsPatchReportPath
    Assert-ReportText $apfsPatchReport.status "Passed" "APFS patch status mismatch"
    Assert-ReportText $apfsPatchReport.operation "APFS image-only root-file patch" "APFS patch operation mismatch"
    Assert-ReportNumber $apfsPatchReport.patch_offset_bytes $apfsPatchOffset "APFS patch offset mismatch"
    Assert-ReportText $apfsPatchReport.patched_file_read.status "Passed" "APFS patch read-back failed"
    Assert-ReportNumber $apfsPatchReport.patched_file_read.bytes_read $apfsReplacementText.Length "APFS patch file size changed"
    Assert-ReportSha256Differs $apfsPatchReport.patched_file_read.sha256 $apfsReplaceReport.written_file_read.sha256 "APFS patch hash did not change"

    $apfsDeleteReportPath = Join-Path $runRoot "generated-delete-report.json"
    $apfsDeleteImagePath = Join-Path $runRoot "generated-delete.apfs"
    $apfsDeleteResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $apfsPatchImagePath,
        "--output", $apfsDeleteReportPath,
        "--apfs-delete-root-file-image", $apfsDeleteImagePath,
        "--apfs-delete-root-file-name", "other.txt")
    Assert-Condition -Condition ($apfsDeleteResult.ExitCode -eq 0) -Message ("APFS delete failed: " + ($apfsDeleteResult.Output -join "`n"))
    $apfsDeleteReport = Read-CertifierReport $apfsDeleteReportPath
    Assert-ReportText $apfsDeleteReport.status "Passed" "APFS delete status mismatch"
    Assert-ReportText $apfsDeleteReport.operation "APFS image-only root-file delete" "APFS delete operation mismatch"
    Assert-ReportNumber $apfsDeleteReport.deleted_apfs_listing.entry_count 1 "APFS delete root count mismatch"
    Assert-ReportText $apfsDeleteReport.deleted_file_negative_read.status "Passed" "APFS delete negative read mismatch"

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
    $apfsRawPatchBlockedReport = Read-CertifierReport $apfsRawPatchBlockedReportPath
    Assert-ReportText $apfsRawPatchBlockedReport.status "Failed" "APFS raw patch blocker status mismatch"
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
    $apfsRawDeleteBlockedReport = Read-CertifierReport $apfsRawDeleteBlockedReportPath
    Assert-ReportText $apfsRawDeleteBlockedReport.status "Failed" "APFS raw delete blocker status mismatch"
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
    $apfsCorruptReadReport = Read-CertifierReport $apfsCorruptReadReportPath
    Assert-ReportText $apfsCorruptReadReport.status "Failed" "Corrupt APFS read report status mismatch"
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
    $apfsRepairReport = Read-CertifierReport $apfsRepairReportPath
    Assert-ReportText $apfsRepairReport.status "Passed" "APFS repair report status mismatch"
    Assert-ReportNumber $apfsRepairReport.repaired_checksum_blocks 1 "APFS repaired checksum block count mismatch"
    Assert-ReportText $apfsRepairReport.repaired_detection.file_system "APFS" "APFS repair detection mismatch"
    Assert-ReportText $apfsRepairReport.repaired_apfs_listing.status "Passed" "APFS repair listing failed"
    Assert-ReportText $apfsRepairReport.repaired_file_read.status "Passed" "APFS repair read-back failed"
    Assert-ReportNumber $apfsRepairReport.repaired_file_read.bytes_read $apfsSeedText.Length "APFS repair read-back byte count mismatch"
    Assert-ReportSha256Equals $apfsRepairReport.repaired_file_read.sha256 $apfsFormatReport.generated_seed_file_read.sha256 "APFS repair read-back hash mismatch"

    $apfsCleanRepairReportPath = Join-Path $runRoot "generated-format-clean-repair-report.json"
    $apfsCleanRepairImagePath = Join-Path $runRoot "generated-format-clean-repair.apfs"
    $apfsCleanRepairResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--input", $apfsFormatImagePath,
        "--output", $apfsCleanRepairReportPath,
        "--apfs-repair-object-checksums", $apfsCleanRepairImagePath)
    Assert-Condition -Condition ($apfsCleanRepairResult.ExitCode -ne 0) -Message "Clean APFS repair unexpectedly passed"
    $apfsCleanRepairReport = Read-CertifierReport $apfsCleanRepairReportPath
    Assert-Condition -Condition ((@($apfsCleanRepairReport.blockers) -join "`n").Contains("did not find")) -Message "Clean APFS repair blocker missing"

    $apfsFormatOverwriteReportPath = Join-Path $runRoot "generated-format-overwrite-report.json"
    $apfsFormatOverwriteResult = Invoke-Certifier -Path $resolvedCertifier -Arguments @(
        "--output", $apfsFormatOverwriteReportPath,
        "--apfs-build-format-image", $apfsFormatImagePath,
        "--apfs-format-size-bytes", "67108864",
        "--apfs-format-block-size", "4096",
        "--apfs-format-volume-name", "SAK Empty")
    Assert-Condition -Condition ($apfsFormatOverwriteResult.ExitCode -ne 0) -Message "APFS format overwrite unexpectedly passed"
    $apfsFormatOverwriteReport = Read-CertifierReport $apfsFormatOverwriteReportPath
    Assert-ReportText $apfsFormatOverwriteReport.status "Failed" "APFS format overwrite status mismatch"
    Assert-Condition -Condition ((@($apfsFormatOverwriteReport.blockers) -join "`n").Contains("overwrite")) -Message "APFS format overwrite blocker missing"

    Write-Host "partition_filesystem_probe_certifier self-test passed: $runRoot"
    $global:LASTEXITCODE = 0
}
finally {
    Pop-Location
}

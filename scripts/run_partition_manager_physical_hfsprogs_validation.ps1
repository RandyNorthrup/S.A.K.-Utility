<#
.SYNOPSIS
    Runs destructive physical HFS+/HFSX tool proof on an expendable partition.

.DESCRIPTION
    Formats one explicitly selected non-boot external partition with bundled
    newfs_hfs, validates and repairs with bundled fsck_hfs, verifies S.A.K. raw
    HFS detection with partition_filesystem_probe_certifier, and writes JSON
    evidence. The selected partition contents are destroyed.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [int]$DiskNumber,
    [int]$PartitionNumber = -1,
    [string]$ExpectedSerialNumber = "",
    [string]$ExpectedFriendlyNamePattern = "",
    [ValidateSet("HFS+", "HFSX")] [string[]]$FileSystems = @("HFS+", "HFSX", "HFS+"),
    [string]$ProjectRoot = "",
    [string]$EvidenceRoot = "artifacts\partition-manager-certification\vm-lab\external-evidence\external.hfsprogs-physical-destructive",
    [string]$ReportPath = "",
    [string]$CertifierPath = "",
    [switch]$AllowInternalDisk,
    [switch]$AllowLargeUnpinnedDisk,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$LargeDiskGuardBytes = 64GB
$SparseCopyBufferBytes = 4MB
$StaleSignatureClearBytes = 16MB
$AppleHfsGptType = "{48465300-0000-11aa-aa11-00306543ecac}"

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
}

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function ConvertTo-PlainText {
    param([object[]]$Value)
    return (($Value | ForEach-Object {
        if ($null -eq $_) { "" } else { $_.ToString() }
    }) -join "`n").Trim()
}

function Resolve-ProjectPath {
    param([Parameter(Mandatory = $true)] [string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return Join-Path $ProjectRoot $Path
}

function Resolve-Certifier {
    if (-not [string]::IsNullOrWhiteSpace($CertifierPath)) {
        return (Resolve-Path -LiteralPath $CertifierPath -ErrorAction Stop).Path
    }
    $candidate = Join-Path $ProjectRoot "build\Release\partition_filesystem_probe_certifier.exe"
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        throw "partition_filesystem_probe_certifier.exe was not found. Build target partition_filesystem_probe_certifier first."
    }
    return (Resolve-Path -LiteralPath $candidate).Path
}

function Get-ToolRecord {
    param(
        [Parameter(Mandatory = $true)] [object]$Manifest,
        [Parameter(Mandatory = $true)] [string]$ToolId,
        [Parameter(Mandatory = $true)] [string]$Operation,
        [Parameter(Mandatory = $true)] [string]$FileSystem
    )

    $toolFileSystem = if ($FileSystem -eq "HFSX") { "hfsx" } else { "hfs+" }
    $matches = @($Manifest.tools | Where-Object {
        $_.id -eq $ToolId -and
        @($_.operations) -contains $Operation -and
        @($_.file_systems) -contains $toolFileSystem
    })
    if ($matches.Count -ne 1) {
        throw "Manifest does not approve exactly one $ToolId tool for $Operation/$FileSystem."
    }
    return $matches[0]
}

function Resolve-ApprovedTool {
    param(
        [Parameter(Mandatory = $true)] [object]$Manifest,
        [Parameter(Mandatory = $true)] [string]$ToolId,
        [Parameter(Mandatory = $true)] [string]$Operation,
        [Parameter(Mandatory = $true)] [string]$FileSystem
    )

    $tool = Get-ToolRecord -Manifest $Manifest -ToolId $ToolId -Operation $Operation -FileSystem $FileSystem
    $path = Join-Path (Join-Path $ProjectRoot "tools\filesystem") ([string]$tool.relative_path)
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        $path = Join-Path $ProjectRoot ("build\Release\tools\filesystem\" + [string]$tool.relative_path)
    }
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Approved tool not found: $($tool.relative_path)"
    }
    $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    $expectedHash = ([string]$tool.binary_sha256).ToLowerInvariant()
    if ($actualHash -ne $expectedHash) {
        throw "Approved tool hash mismatch for $ToolId."
    }
    return [pscustomobject]@{
        id = $tool.id
        operation = $Operation
        file_system = $FileSystem
        path = (Resolve-Path -LiteralPath $path).Path
        expected_sha256 = $expectedHash
        actual_sha256 = $actualHash
    }
}

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string]$FilePath,
        [string[]]$Arguments = @(),
        [int[]]$AcceptedExitCodes = @(0)
    )

    $started = Get-Date
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $FilePath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }

    $record = [pscustomobject]@{
        name = $Name
        file_path = $FilePath
        arguments = $Arguments
        exit_code = [int]$exitCode
        duration_seconds = [Math]::Round(((Get-Date) - $started).TotalSeconds, 3)
        output = ConvertTo-PlainText -Value $output
    }
    $Script:Commands.Add($record) | Out-Null
    if ($AcceptedExitCodes -notcontains [int]$exitCode) {
        throw "$Name failed with exit code $exitCode. $($record.output)"
    }
    return $record
}

function ConvertTo-HfsToolTargetPath {
    param([Parameter(Mandatory = $true)] [string]$TargetPath)

    $trimmed = $TargetPath.Trim()
    if ($trimmed.StartsWith('\\?\') -or $trimmed.StartsWith('\\.\')) {
        return $trimmed.Replace('\', '/')
    }
    return $trimmed
}

function New-SparseImageFile {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [uint64]$SizeBytes
    )

    Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    New-Item -ItemType File -Path $Path -Force | Out-Null
    $sparseOutput = fsutil sparse setflag $Path 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to mark staged HFS image sparse. $((ConvertTo-PlainText -Value $sparseOutput))"
    }

    $stream = [System.IO.File]::Open($Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::ReadWrite,
        [System.IO.FileShare]::None)
    try {
        $stream.SetLength([int64]$SizeBytes)
    }
    finally {
        $stream.Dispose()
    }
}

function Get-SparseAllocatedRanges {
    param([Parameter(Mandatory = $true)] [string]$Path)

    $output = fsutil sparse queryrange $Path 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to query sparse ranges for staged HFS image. $((ConvertTo-PlainText -Value $output))"
    }

    $ranges = @()
    foreach ($line in $output) {
        if ($line -match 'Offset:\s*(0x[0-9a-fA-F]+)\s+Length:\s*(0x[0-9a-fA-F]+)') {
            $ranges += [pscustomobject]@{
                offset = [Convert]::ToUInt64($Matches[1].Substring(2), 16)
                length = [Convert]::ToUInt64($Matches[2].Substring(2), 16)
            }
        }
    }
    if ($ranges.Count -eq 0) {
        throw "No allocated sparse ranges found after staged HFS format."
    }
    return $ranges
}

function Write-ZeroRange {
    param(
        [Parameter(Mandatory = $true)] [System.IO.FileStream]$TargetStream,
        [Parameter(Mandatory = $true)] [uint64]$Offset,
        [Parameter(Mandatory = $true)] [uint64]$Length
    )

    $zero = New-Object byte[] ([int][Math]::Min([uint64]$SparseCopyBufferBytes, $Length))
    [void]$TargetStream.Seek([int64]$Offset, [System.IO.SeekOrigin]::Begin)
    $remaining = $Length
    while ($remaining -gt 0) {
        $chunk = [int][Math]::Min([uint64]$zero.Length, $remaining)
        $TargetStream.Write($zero, 0, $chunk)
        $remaining -= [uint64]$chunk
    }
}

function Copy-FileRange {
    param(
        [Parameter(Mandatory = $true)] [System.IO.FileStream]$SourceStream,
        [Parameter(Mandatory = $true)] [System.IO.FileStream]$TargetStream,
        [Parameter(Mandatory = $true)] [uint64]$Offset,
        [Parameter(Mandatory = $true)] [uint64]$Length
    )

    $buffer = New-Object byte[] ([int]$SparseCopyBufferBytes)
    [void]$SourceStream.Seek([int64]$Offset, [System.IO.SeekOrigin]::Begin)
    [void]$TargetStream.Seek([int64]$Offset, [System.IO.SeekOrigin]::Begin)
    $remaining = $Length
    while ($remaining -gt 0) {
        $chunk = [int][Math]::Min([uint64]$buffer.Length, $remaining)
        $read = $SourceStream.Read($buffer, 0, $chunk)
        if ($read -le 0) {
            throw "Staged HFS image read ended before requested range copy completed."
        }
        $TargetStream.Write($buffer, 0, $read)
        $remaining -= [uint64]$read
    }
}

function Copy-SparseImageToRawTarget {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string]$ImagePath,
        [Parameter(Mandatory = $true)] [string]$RawTarget,
        [Parameter(Mandatory = $true)] [uint64]$TargetSizeBytes
    )

    $started = Get-Date
    $ranges = @(Get-SparseAllocatedRanges -Path $ImagePath | Sort-Object offset)
    $source = [System.IO.File]::Open($ImagePath,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite)
    $target = [System.IO.File]::Open($RawTarget,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::ReadWrite,
        [System.IO.FileShare]::ReadWrite)
    $copiedBytes = [uint64]0
    $zeroedHoleBytes = [uint64]0
    $clippedRanges = 0
    try {
        $cursor = [uint64]0
        foreach ($range in $ranges) {
            if ([uint64]$range.offset -ge $TargetSizeBytes) {
                $clippedRanges += 1
                continue
            }
            if ([uint64]$range.offset -gt $cursor) {
                $holeLength = [uint64]$range.offset - $cursor
                Write-ZeroRange -TargetStream $target -Offset $cursor -Length $holeLength
                $zeroedHoleBytes += $holeLength
                $cursor = [uint64]$range.offset
            }
            $rangeLength = [uint64]$range.length
            $maxLength = $TargetSizeBytes - [uint64]$range.offset
            if ($rangeLength -gt $maxLength) {
                $rangeLength = $maxLength
                $clippedRanges += 1
            }
            if ($rangeLength -eq 0) {
                continue
            }
            Copy-FileRange -SourceStream $source -TargetStream $target -Offset $range.offset -Length $rangeLength
            $copiedBytes += $rangeLength
            $rangeEnd = [uint64]$range.offset + $rangeLength
            if ($rangeEnd -gt $cursor) {
                $cursor = $rangeEnd
            }
        }
        if ($cursor -lt $TargetSizeBytes) {
            $holeLength = $TargetSizeBytes - $cursor
            Write-ZeroRange -TargetStream $target -Offset $cursor -Length $holeLength
            $zeroedHoleBytes += $holeLength
        }
        $target.Flush()
    }
    finally {
        $source.Dispose()
        $target.Dispose()
    }

    $record = [pscustomobject]@{
        name = $Name
        file_path = "System.IO.FileStream"
        arguments = @($ImagePath, $RawTarget)
        exit_code = 0
        duration_seconds = [Math]::Round(((Get-Date) - $started).TotalSeconds, 3)
        output = "Copied $($ranges.Count) sparse range(s), $copiedBytes allocated byte(s); clipped $clippedRanges range(s); zeroed $zeroedHoleBytes sparse-hole byte(s)."
    }
    $Script:Commands.Add($record) | Out-Null
    return $record
}

function Get-DiskSnapshot {
    param([Parameter(Mandatory = $true)] [int]$Number)

    $disk = Get-Disk -Number $Number -ErrorAction Stop
    $partitions = @()
    foreach ($partition in @(Get-Partition -DiskNumber $Number -ErrorAction SilentlyContinue | Sort-Object Offset)) {
        $volume = $partition | Get-Volume -ErrorAction SilentlyContinue
        $partitions += [pscustomobject]@{
            partition_number = [int]$partition.PartitionNumber
            drive_letter = if ($partition.DriveLetter) { [string]$partition.DriveLetter } else { "" }
            type = [string]$partition.Type
            gpt_type = [string]$partition.GptType
            offset_bytes = [string][uint64]$partition.Offset
            size_bytes = [string][uint64]$partition.Size
            file_system = if ($volume) { [string]$volume.FileSystem } else { "" }
            label = if ($volume) { [string]$volume.FileSystemLabel } else { "" }
        }
    }
    return [pscustomobject]@{
        disk_number = [int]$disk.Number
        friendly_name = [string]$disk.FriendlyName
        serial_number = [string]$disk.SerialNumber
        bus_type = [string]$disk.BusType
        partition_style = [string]$disk.PartitionStyle
        size_bytes = [string][uint64]$disk.Size
        is_boot = [bool]$disk.IsBoot
        is_system = [bool]$disk.IsSystem
        is_read_only = [bool]$disk.IsReadOnly
        partitions = $partitions
    }
}

function Assert-PhysicalTarget {
    param([Parameter(Mandatory = $true)] [object]$Disk)

    if ($Disk.IsBoot -or $Disk.IsSystem) {
        throw "Refusing to mutate boot/system disk $DiskNumber."
    }
    if ($Disk.IsReadOnly) {
        throw "Refusing to mutate read-only disk $DiskNumber."
    }
    if (-not $AllowInternalDisk -and "$($Disk.BusType)" -ne "USB") {
        throw "Refusing to mutate non-USB disk $DiskNumber without -AllowInternalDisk. BusType=$($Disk.BusType)"
    }
    if ([uint64]$Disk.Size -gt $LargeDiskGuardBytes -and
        [string]::IsNullOrWhiteSpace($ExpectedSerialNumber) -and
        [string]::IsNullOrWhiteSpace($ExpectedFriendlyNamePattern) -and
        -not $AllowLargeUnpinnedDisk) {
        throw "Disk $DiskNumber is larger than $LargeDiskGuardBytes bytes; provide serial/friendly-name guard or -AllowLargeUnpinnedDisk."
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedSerialNumber) -and
        "$($Disk.SerialNumber)" -ne $ExpectedSerialNumber) {
        throw "Disk serial guard mismatch. Expected '$ExpectedSerialNumber', found '$($Disk.SerialNumber)'."
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedFriendlyNamePattern) -and
        "$($Disk.FriendlyName)" -notmatch $ExpectedFriendlyNamePattern) {
        throw "Disk friendly-name guard mismatch. Pattern '$ExpectedFriendlyNamePattern', found '$($Disk.FriendlyName)'."
    }
}

function Select-HfsPartition {
    if ($PartitionNumber -gt 0) {
        return Get-Partition -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber -ErrorAction Stop
    }
    $candidates = @(Get-Partition -DiskNumber $DiskNumber -ErrorAction Stop | Where-Object {
        "$($_.GptType)".Equals($AppleHfsGptType, [StringComparison]::OrdinalIgnoreCase)
    })
    if ($candidates.Count -ne 1) {
        throw "Expected exactly one Apple HFS partition on disk $DiskNumber; found $($candidates.Count). Pass -PartitionNumber."
    }
    return $candidates[0]
}

function New-ReportBase {
    param([string]$OutputPath)

    [ordered]@{
        schema_version = 1
        gate_id = "external.hfsprogs-physical-destructive"
        status = "Failed"
        started_at_utc = (Get-Date).ToUniversalTime().ToString("o")
        finished_at_utc = ""
        output_root = $OutputPath
        destructive = $true
        writes_target_media = $true
        disk = $null
        partition = $null
        before = $null
        after = $null
        tools = @()
        commands = @()
        cases = @()
        artifacts = @()
        error = ""
    }
}

if (-not $Force) {
    throw "Pass -Force after confirming Disk $DiskNumber Partition $PartitionNumber is expendable."
}
if (-not (Test-IsAdmin)) {
    throw "Run from an elevated PowerShell session or use the local launcher."
}

$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
$outputRoot = Resolve-ProjectPath -Path $EvidenceRoot
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = Join-Path $outputRoot "report.json"
} else {
    $ReportPath = Resolve-ProjectPath -Path $ReportPath
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ReportPath) | Out-Null
}

$Script:Commands = [System.Collections.Generic.List[object]]::new()
$report = New-ReportBase -OutputPath $outputRoot

try {
    $disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
    Assert-PhysicalTarget -Disk $disk
    $partition = Select-HfsPartition
    if ($partition.IsBoot -or $partition.IsSystem) {
        throw "Refusing to mutate boot/system partition $DiskNumber/$($partition.PartitionNumber)."
    }

    $rawTarget = "\\?\GLOBALROOT\Device\Harddisk$DiskNumber\Partition$($partition.PartitionNumber)"
    $manifestPath = Join-Path $ProjectRoot "tools\filesystem\manifest.json"
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $certifier = Resolve-Certifier
    $newfs = Resolve-ApprovedTool -Manifest $manifest -ToolId "newfs_hfs" -Operation "format" -FileSystem "HFS+"
    $fsck = Resolve-ApprovedTool -Manifest $manifest -ToolId "fsck_hfs" -Operation "repair" -FileSystem "HFS+"

    $report.disk = Get-DiskSnapshot -Number $DiskNumber
    $report.partition = [pscustomobject]@{
        disk_number = $DiskNumber
        partition_number = [int]$partition.PartitionNumber
        raw_target = $rawTarget
        gpt_type = [string]$partition.GptType
        size_bytes = [string][uint64]$partition.Size
        offset_bytes = [string][uint64]$partition.Offset
    }
    $report.before = Get-DiskSnapshot -Number $DiskNumber
    $report.tools = @($newfs, $fsck, [pscustomobject]@{
            id = "partition_filesystem_probe_certifier"
            operation = "probe"
            file_system = "hfs"
            path = $certifier
            actual_sha256 = (Get-FileHash -LiteralPath $certifier -Algorithm SHA256).Hash.ToLowerInvariant()
        })

    $index = 0
    foreach ($fileSystem in $FileSystems) {
        $index += 1
        $safeName = if ($fileSystem -eq "HFSX") { "hfsx" } else { "hfsplus" }
        $label = if ($fileSystem -eq "HFSX") { "SAKPHY_HFSX" } else { "SAKPHY_HFS" }
        $probePath = Join-Path $outputRoot ("{0:00}-{1}.probe.json" -f $index, $safeName)
        $imagePath = Join-Path $outputRoot ("{0:00}-{1}.img" -f $index, $safeName)
        $newfsArgs = @()
        if ($fileSystem -eq "HFSX") {
            $newfsArgs += "-s"
        }
        $newfsArgs += @("-v", $label, $imagePath)

        try {
            New-SparseImageFile -Path $imagePath -SizeBytes ([uint64]$partition.Size)
            $format = Invoke-NativeCommand -Name "newfs_hfs-$safeName" -FilePath $newfs.path -Arguments $newfsArgs
            $initialCheckCodes = if ($fileSystem -eq "HFSX") { @(0, 8) } else { @(0) }
            $initialCheck = Invoke-NativeCommand -Name "fsck_hfs-initial-$safeName" -FilePath $fsck.path -Arguments @("-n", "-f", $imagePath) -AcceptedExitCodes $initialCheckCodes
            $repairExitCodes = if ($fileSystem -eq "HFSX") { @(0, 8) } else { @(0) }
            $repair = Invoke-NativeCommand -Name "fsck_hfs-repair-$safeName" -FilePath $fsck.path -Arguments @("-p", "-f", $imagePath) -AcceptedExitCodes $repairExitCodes
            $finalCheck = Invoke-NativeCommand -Name "fsck_hfs-final-$safeName" -FilePath $fsck.path -Arguments @("-n", "-f", $imagePath) -AcceptedExitCodes $initialCheckCodes
            $extraRepair = $null
            if ($fileSystem -eq "HFSX" -and [int]$finalCheck.exit_code -ne 0) {
                $extraRepair = Invoke-NativeCommand -Name "fsck_hfs-repair2-$safeName" -FilePath $fsck.path -Arguments @("-p", "-f", $imagePath)
                $finalCheck = Invoke-NativeCommand -Name "fsck_hfs-final2-$safeName" -FilePath $fsck.path -Arguments @("-n", "-f", $imagePath)
            }
            $copy = Copy-SparseImageToRawTarget -Name "copy-staged-hfs-$safeName" -ImagePath $imagePath -RawTarget $rawTarget -TargetSizeBytes ([uint64]$partition.Size)
        }
        finally {
            Remove-Item -LiteralPath $imagePath -Force -ErrorAction SilentlyContinue
        }
        $probe = Invoke-NativeCommand -Name "sak-probe-$safeName" -FilePath $certifier -Arguments @(
            "--input", $rawTarget,
            "--output", $probePath,
            "--expect", $fileSystem,
            "--hfs-check"
        )

        $probeReport = Get-Content -LiteralPath $probePath -Raw | ConvertFrom-Json
        if ($probeReport.detected_file_system -ne $fileSystem) {
            throw "Probe detected '$($probeReport.detected_file_system)' after $fileSystem format."
        }
        if ($probeReport.hfs_check.status -ne "Passed") {
            throw "HFS consistency check did not pass after $fileSystem format."
        }

        $report.cases += [pscustomobject]@{
            index = $index
            file_system = $fileSystem
            raw_target = $rawTarget
            staged_image_path = $imagePath
            initial_check_exit_code = [int]$initialCheck.exit_code
            final_check_exit_code = [int]$finalCheck.exit_code
            detected_file_system = [string]$probeReport.detected_file_system
            probe_report = $probePath
            status = "Passed"
            format_command = $format.name
            repair_command = @($repair.name, $(if ($extraRepair) { $extraRepair.name } else { $null })) | Where-Object { $_ }
            copy_command = $copy.name
            probe_command = $probe.name
        }
        $report.artifacts += $probePath
    }

    $report.after = Get-DiskSnapshot -Number $DiskNumber
    $report.status = "Passed"
}
catch {
    $report.error = $_.Exception.Message
    throw
}
finally {
    $report.finished_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    $report.commands = @($Script:Commands)
    $report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $ReportPath -Encoding UTF8
}

Write-Host "Physical HFS tool validation passed on disk $DiskNumber partition $($report.partition.partition_number)."
Write-Host "Report: $ReportPath"

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
# Partition types this gate must never overwrite, including when -PartitionNumber
# names the partition explicitly.
$ForbiddenGptTypes = @(
    "{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}",
    "{e3c9e316-0b5c-4db8-817d-f92df00215ae}",
    "{de94bba4-06d1-4d40-a16a-bfd50179d6ac}"
)
# The gate id below is only earned when every one of these is exercised.
$RequiredFileSystems = @("HFS+", "HFSX")
$ToolRelativeRoot = "tools\filesystem"
$ToolBuildRelativeRoot = "build\Release\tools\filesystem"

if ([string]::IsNullOrWhiteSpace($env:SystemRoot)) {
    throw "SystemRoot is not set; refusing to resolve fsutil by bare command name."
}
$FsutilPath = Join-Path $env:SystemRoot "System32\fsutil.exe"
if (-not (Test-Path -LiteralPath $FsutilPath -PathType Leaf)) {
    throw "fsutil.exe was not found at $FsutilPath."
}

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

function Assert-PathUnderRoot {
    param(
        [Parameter(Mandatory = $true)] [string]$Root,
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [string]$What
    )

    $rootFull = [System.IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $pathFull = [System.IO.Path]::GetFullPath($Path)
    if (-not $pathFull.StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$What must stay under '$Root'. Refusing '$pathFull'."
    }
    return $pathFull
}

function Assert-NotReparsePoint {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [string]$What
    )

    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$What '$Path' is a reparse point; refusing to write evidence through a link."
    }
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
        $explicit = (Resolve-Path -LiteralPath $CertifierPath -ErrorAction Stop).Path
        if (-not (Test-Path -LiteralPath $explicit -PathType Leaf)) {
            throw "-CertifierPath must name an existing file: $CertifierPath"
        }
        if ([System.IO.Path]::GetExtension($explicit).ToLowerInvariant() -ne ".exe") {
            throw "-CertifierPath must name an .exe. Refusing '$explicit'."
        }
        return (Assert-PathUnderRoot -Root $ProjectRoot -Path $explicit -What "-CertifierPath")
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
    $approved = @($Manifest.tools | Where-Object {
        $_.id -eq $ToolId -and
        @($_.operations) -contains $Operation -and
        @($_.file_systems) -contains $toolFileSystem
    })
    if ($approved.Count -ne 1) {
        throw "Manifest does not approve exactly one $ToolId tool for $Operation/$FileSystem."
    }
    return $approved[0]
}

function Assert-ManifestRelativePath {
    param(
        [Parameter(Mandatory = $true)] [string]$Relative,
        [Parameter(Mandatory = $true)] [string]$ToolId
    )

    if ([string]::IsNullOrWhiteSpace($Relative)) {
        throw "Manifest entry $ToolId has an empty relative_path."
    }
    if ([System.IO.Path]::IsPathRooted($Relative) -or
        $Relative.Contains(":") -or
        (@($Relative -split '[\\/]') -contains "..")) {
        throw "Manifest entry $ToolId has an out-of-tree relative_path '$Relative'."
    }
}

function Get-ApprovedToolRoot {
    param(
        [Parameter(Mandatory = $true)] [string]$Relative,
        [Parameter(Mandatory = $true)] [string]$ToolId
    )

    Assert-ManifestRelativePath -Relative $Relative -ToolId $ToolId
    $canonicalRoot = Join-Path $ProjectRoot $ToolRelativeRoot
    $buildRoot = Join-Path $ProjectRoot $ToolBuildRelativeRoot
    $canonicalPath = Assert-PathUnderRoot -Root $canonicalRoot -Path (Join-Path $canonicalRoot $Relative) -What "Manifest path for $ToolId"
    $buildPath = Assert-PathUnderRoot -Root $buildRoot -Path (Join-Path $buildRoot $Relative) -What "Manifest path for $ToolId"
    $canonicalExists = Test-Path -LiteralPath $canonicalPath -PathType Leaf
    $buildExists = Test-Path -LiteralPath $buildPath -PathType Leaf
    if ($canonicalExists -and $buildExists) {
        $canonicalHash = (Get-FileHash -LiteralPath $canonicalPath -Algorithm SHA256).Hash.ToLowerInvariant()
        $buildHash = (Get-FileHash -LiteralPath $buildPath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($canonicalHash -ne $buildHash) {
            throw "Approved file '$Relative' differs between $ToolRelativeRoot and $ToolBuildRelativeRoot; refusing to guess which one is authoritative."
        }
    }
    if ($canonicalExists) {
        return $canonicalRoot
    }
    if ($buildExists) {
        return $buildRoot
    }
    throw "Approved tool not found: $Relative"
}

function Assert-ApprovedRuntimeFiles {
    param(
        [Parameter(Mandatory = $true)] [object]$Tool,
        [Parameter(Mandatory = $true)] [string]$ResolvedRoot
    )

    $canonicalRoot = Join-Path $ProjectRoot $ToolRelativeRoot
    foreach ($runtimeFile in @($Tool.runtime_files)) {
        if ($null -eq $runtimeFile) {
            continue
        }
        $relative = [string]$runtimeFile.relative_path
        Assert-ManifestRelativePath -Relative $relative -ToolId ([string]$Tool.id)
        $expected = ([string]$runtimeFile.sha256).ToLowerInvariant()
        if ($expected -notmatch '^[0-9a-f]{64}$') {
            throw "Manifest runtime file '$relative' has no valid sha256."
        }
        # Provenance is verified in the checked-in tool tree; the loadable
        # companions are verified again where the executable actually runs.
        $roots = @($canonicalRoot)
        if ($ResolvedRoot -ne $canonicalRoot -and
            [System.IO.Path]::GetExtension($relative).ToLowerInvariant() -eq ".dll") {
            $roots += $ResolvedRoot
        }
        foreach ($root in $roots) {
            $path = Assert-PathUnderRoot -Root $root -Path (Join-Path $root $relative) -What "Manifest runtime path"
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
                throw "Approved runtime file not found: $relative"
            }
            $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($actual -ne $expected) {
                throw "Approved runtime file hash mismatch for $relative."
            }
        }
    }
}

function Resolve-ApprovedTool {
    param(
        [Parameter(Mandatory = $true)] [object]$Manifest,
        [Parameter(Mandatory = $true)] [string]$ToolId,
        [Parameter(Mandatory = $true)] [string]$Operation,
        [Parameter(Mandatory = $true)] [string]$FileSystem
    )

    $tool = Get-ToolRecord -Manifest $Manifest -ToolId $ToolId -Operation $Operation -FileSystem $FileSystem
    $relative = [string]$tool.relative_path
    $root = Get-ApprovedToolRoot -Relative $relative -ToolId $ToolId
    $path = Join-Path $root $relative
    $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    $expectedHash = ([string]$tool.binary_sha256).ToLowerInvariant()
    if ($expectedHash -notmatch '^[0-9a-f]{64}$') {
        throw "Manifest entry $ToolId has no valid binary_sha256."
    }
    if ($actualHash -ne $expectedHash) {
        throw "Approved tool hash mismatch for $ToolId."
    }
    Assert-ApprovedRuntimeFiles -Tool $tool -ResolvedRoot $root
    return [pscustomobject]@{
        id = $tool.id
        operation = $Operation
        file_system = $FileSystem
        path = (Resolve-Path -LiteralPath $path).Path
        resolved_root = $root
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
    # A launch that never runs must not inherit the previous command's status.
    $Global:LASTEXITCODE = $null
    $output = @()
    $exitCode = $null
    $launchError = ""
    try {
        $output = & $FilePath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    catch {
        $launchError = $_.Exception.Message
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    if (-not [string]::IsNullOrWhiteSpace($launchError)) {
        throw "$Name failed to launch '$FilePath'. $launchError"
    }
    if ($null -eq $exitCode) {
        throw "$Name produced no exit code for '$FilePath'; treating the launch as failed. $((ConvertTo-PlainText -Value $output))"
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
    Invoke-NativeCommand -Name ("fsutil-sparse-setflag-" + [System.IO.Path]::GetFileName($Path)) `
        -FilePath $FsutilPath -Arguments @("sparse", "setflag", $Path) | Out-Null

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

    $record = Invoke-NativeCommand -Name ("fsutil-sparse-queryrange-" + [System.IO.Path]::GetFileName($Path)) `
        -FilePath $FsutilPath -Arguments @("sparse", "queryrange", $Path)

    $ranges = @()
    foreach ($line in ($record.output -split "`n")) {
        if ($line -match 'Offset:\s*(0x[0-9a-fA-F]+)\s+Length:\s*(0x[0-9a-fA-F]+)') {
            $offset = [Convert]::ToUInt64($Matches[1].Substring(2), 16)
            $length = [Convert]::ToUInt64($Matches[2].Substring(2), 16)
            if ($length -eq 0) {
                throw "fsutil reported a zero-length sparse range at offset $offset for the staged HFS image."
            }
            if ($offset -gt ([uint64]::MaxValue - $length)) {
                throw "fsutil reported an overflowing sparse range at offset $offset for the staged HFS image."
            }
            $ranges += [pscustomobject]@{
                offset = $offset
                length = $length
            }
        }
        elseif ($line -match 'Offset:|Length:') {
            throw "Unparsable sparse range line from fsutil: '$($line.Trim())'."
        }
    }
    if ($ranges.Count -eq 0) {
        throw "No allocated sparse ranges found after staged HFS format."
    }
    $sorted = @($ranges | Sort-Object offset)
    $previousEnd = [uint64]0
    foreach ($range in $sorted) {
        if ([uint64]$range.offset -lt $previousEnd) {
            throw "fsutil reported overlapping sparse ranges for the staged HFS image."
        }
        $previousEnd = [uint64]$range.offset + [uint64]$range.length
    }
    return $sorted
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
    # Deny concurrent writers on both ends: a foreign write during the raw copy
    # would corrupt the target and void the evidence.
    $source = [System.IO.File]::Open($ImagePath,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::Read)
    $target = [System.IO.File]::Open($RawTarget,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::ReadWrite,
        [System.IO.FileShare]::Read)
    $copiedBytes = [uint64]0
    $zeroedHoleBytes = [uint64]0
    try {
        $cursor = [uint64]0
        foreach ($range in $ranges) {
            $rangeOffset = [uint64]$range.offset
            $rangeLength = [uint64]$range.length
            if ($rangeOffset -ge $TargetSizeBytes -or
                $rangeLength -gt ($TargetSizeBytes - $rangeOffset)) {
                throw "Staged HFS image range (offset $rangeOffset, length $rangeLength) does not fit raw target size $TargetSizeBytes; refusing a partial copy."
            }
            if ($rangeOffset -gt $cursor) {
                $holeLength = $rangeOffset - $cursor
                Write-ZeroRange -TargetStream $target -Offset $cursor -Length $holeLength
                $zeroedHoleBytes += $holeLength
                $cursor = $rangeOffset
            }
            Copy-FileRange -SourceStream $source -TargetStream $target -Offset $rangeOffset -Length $rangeLength
            $copiedBytes += $rangeLength
            $rangeEnd = $rangeOffset + $rangeLength
            if ($rangeEnd -gt $cursor) {
                $cursor = $rangeEnd
            }
        }
        if ($cursor -lt $TargetSizeBytes) {
            $holeLength = $TargetSizeBytes - $cursor
            Write-ZeroRange -TargetStream $target -Offset $cursor -Length $holeLength
            $zeroedHoleBytes += $holeLength
        }
        $target.Flush($true)
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
        output = "Copied $($ranges.Count) sparse range(s), $copiedBytes allocated byte(s); zeroed $zeroedHoleBytes sparse-hole byte(s)."
    }
    $Script:Commands.Add($record) | Out-Null
    return $record
}

function Get-DiskSnapshot {
    param([Parameter(Mandatory = $true)] [int]$Number)

    $disk = Get-Disk -Number $Number -ErrorAction Stop
    $partitions = @()
    $partitionList = @()
    try {
        $partitionList = @(Get-Partition -DiskNumber $Number -ErrorAction Stop | Sort-Object Offset)
    }
    catch {
        throw "Failed to enumerate partitions on disk ${Number}: $($_.Exception.Message)"
    }
    foreach ($partition in $partitionList) {
        $volume = $null
        $volumeError = ""
        try {
            $volume = $partition | Get-Volume -ErrorAction Stop
        }
        catch {
            # A partition with no Windows-recognized volume is expected here, but
            # the reason is recorded instead of being silently dropped.
            $volumeError = $_.Exception.Message
        }
        $partitions += [pscustomobject]@{
            partition_number = [int]$partition.PartitionNumber
            drive_letter = if ($partition.DriveLetter) { [string]$partition.DriveLetter } else { "" }
            type = [string]$partition.Type
            gpt_type = [string]$partition.GptType
            offset_bytes = [string][uint64]$partition.Offset
            size_bytes = [string][uint64]$partition.Size
            file_system = if ($volume) { [string]$volume.FileSystem } else { "" }
            label = if ($volume) { [string]$volume.FileSystemLabel } else { "" }
            volume_query_error = $volumeError
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
    if (-not [string]::IsNullOrWhiteSpace($ExpectedFriendlyNamePattern) -and
        "" -match $ExpectedFriendlyNamePattern) {
        throw "ExpectedFriendlyNamePattern '$ExpectedFriendlyNamePattern' matches any disk; provide a constraining pattern."
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

function Assert-PartitionSelectable {
    param([Parameter(Mandatory = $true)] [object]$Partition)

    if ($Partition.IsBoot -or $Partition.IsSystem) {
        throw "Refusing to mutate boot/system partition $DiskNumber/$($Partition.PartitionNumber)."
    }
    $gptType = "$($Partition.GptType)"
    foreach ($forbidden in $ForbiddenGptTypes) {
        if ($gptType.Equals($forbidden, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to mutate partition $DiskNumber/$($Partition.PartitionNumber) with reserved GPT type $gptType."
        }
    }
}

function Select-HfsPartition {
    if ($PartitionNumber -gt 0) {
        $explicit = Get-Partition -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber -ErrorAction Stop
        Assert-PartitionSelectable -Partition $explicit
        return $explicit
    }
    $candidates = @(Get-Partition -DiskNumber $DiskNumber -ErrorAction Stop | Where-Object {
        "$($_.GptType)".Equals($AppleHfsGptType, [StringComparison]::OrdinalIgnoreCase)
    })
    if ($candidates.Count -ne 1) {
        throw "Expected exactly one Apple HFS partition on disk $DiskNumber; found $($candidates.Count). Pass -PartitionNumber."
    }
    Assert-PartitionSelectable -Partition $candidates[0]
    return $candidates[0]
}

function Assert-TargetIdentityUnchanged {
    param(
        [Parameter(Mandatory = $true)] [object]$PinnedDisk,
        [Parameter(Mandatory = $true)] [object]$PinnedPartition
    )

    $currentDisk = Get-Disk -Number $DiskNumber -ErrorAction Stop
    Assert-PhysicalTarget -Disk $currentDisk
    if ("$($currentDisk.SerialNumber)" -ne "$($PinnedDisk.SerialNumber)" -or
        "$($currentDisk.FriendlyName)" -ne "$($PinnedDisk.FriendlyName)" -or
        "$($currentDisk.UniqueId)" -ne "$($PinnedDisk.UniqueId)" -or
        [uint64]$currentDisk.Size -ne [uint64]$PinnedDisk.Size) {
        throw "Disk $DiskNumber identity changed after selection; refusing to write."
    }
    $currentPartition = Get-Partition -DiskNumber $DiskNumber `
        -PartitionNumber ([int]$PinnedPartition.PartitionNumber) -ErrorAction Stop
    Assert-PartitionSelectable -Partition $currentPartition
    if ([uint64]$currentPartition.Offset -ne [uint64]$PinnedPartition.Offset -or
        [uint64]$currentPartition.Size -ne [uint64]$PinnedPartition.Size -or
        "$($currentPartition.GptType)" -ne "$($PinnedPartition.GptType)") {
        throw "Partition $DiskNumber/$($PinnedPartition.PartitionNumber) changed after selection; refusing to write."
    }
}

function New-ReportBase {
    param(
        [string]$OutputPath,
        [string[]]$RequestedFileSystems
    )

    [ordered]@{
        schema_version = 1
        gate_id = "external.hfsprogs-physical-destructive"
        status = "Failed"
        started_at_utc = (Get-Date).ToUniversalTime().ToString("o")
        finished_at_utc = ""
        output_root = $OutputPath
        destructive = $true
        writes_target_media = $true
        file_systems = @($RequestedFileSystems)
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

$FileSystems = @($FileSystems | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
if ($FileSystems.Count -eq 0) {
    throw "-FileSystems is empty; the gate cannot pass without executing a case."
}
foreach ($requiredFileSystem in $RequiredFileSystems) {
    if (-not (@($FileSystems) -contains $requiredFileSystem)) {
        throw "Gate external.hfsprogs-physical-destructive requires $($RequiredFileSystems -join ' and ') coverage; got '$($FileSystems -join ', ')'."
    }
}

$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
$outputRoot = Assert-PathUnderRoot -Root $ProjectRoot -Path (Resolve-ProjectPath -Path $EvidenceRoot) -What "-EvidenceRoot"
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$outputRoot = (Resolve-Path -LiteralPath $outputRoot).Path
Assert-NotReparsePoint -Path $outputRoot -What "-EvidenceRoot"
if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = Join-Path $outputRoot "report.json"
} else {
    $ReportPath = Assert-PathUnderRoot -Root $ProjectRoot -Path (Resolve-ProjectPath -Path $ReportPath) -What "-ReportPath"
    $reportParent = Split-Path -Parent $ReportPath
    if ([string]::IsNullOrWhiteSpace($reportParent)) {
        throw "-ReportPath must name a file inside a directory."
    }
    New-Item -ItemType Directory -Force -Path $reportParent | Out-Null
    Assert-NotReparsePoint -Path $reportParent -What "-ReportPath directory"
}
if (Test-Path -LiteralPath $ReportPath) {
    if (-not (Test-Path -LiteralPath $ReportPath -PathType Leaf)) {
        throw "Report path '$ReportPath' is not a file."
    }
    Remove-Item -LiteralPath $ReportPath -Force
}

$Script:Commands = [System.Collections.Generic.List[object]]::new()
$report = New-ReportBase -OutputPath $outputRoot -RequestedFileSystems $FileSystems
# Publish the Failed base immediately: a run killed before `finally` must not leave
# an older Passed report standing at the canonical gate path.
$report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $ReportPath -Encoding UTF8

try {
    $disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
    Assert-PhysicalTarget -Disk $disk
    $partition = Select-HfsPartition
    Assert-PartitionSelectable -Partition $partition

    $rawTarget = "\\?\GLOBALROOT\Device\Harddisk$DiskNumber\Partition$($partition.PartitionNumber)"
    $manifestPath = Join-Path $ProjectRoot "tools\filesystem\manifest.json"
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $certifier = Resolve-Certifier

    # Every operation this gate performs is approved per file system before any
    # media is touched: format, read-only check and repair, for HFS+ and HFSX.
    $approvedTools = [ordered]@{}
    foreach ($toolFileSystem in $RequiredFileSystems) {
        $approvedTools["format:$toolFileSystem"] = Resolve-ApprovedTool -Manifest $manifest -ToolId "newfs_hfs" -Operation "format" -FileSystem $toolFileSystem
        $approvedTools["check:$toolFileSystem"] = Resolve-ApprovedTool -Manifest $manifest -ToolId "fsck_hfs" -Operation "check-read-only" -FileSystem $toolFileSystem
        $approvedTools["repair:$toolFileSystem"] = Resolve-ApprovedTool -Manifest $manifest -ToolId "fsck_hfs" -Operation "repair" -FileSystem $toolFileSystem
    }

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
    $report.tools = @($approvedTools.Values) + @([pscustomobject]@{
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
        $newfs = $approvedTools["format:$fileSystem"]
        $fsckCheck = $approvedTools["check:$fileSystem"]
        $fsckRepair = $approvedTools["repair:$fileSystem"]
        $newfsArgs = @()
        if ($fileSystem -eq "HFSX") {
            $newfsArgs += "-s"
        }
        $newfsArgs += @("-v", $label, $imagePath)

        # Deterministic evidence path: a stale probe from an earlier run must never
        # be consumed as this run's result.
        if (Test-Path -LiteralPath $probePath) {
            Remove-Item -LiteralPath $probePath -Force
        }

        try {
            New-SparseImageFile -Path $imagePath -SizeBytes ([uint64]$partition.Size)
            $format = Invoke-NativeCommand -Name "newfs_hfs-$safeName" -FilePath $newfs.path -Arguments $newfsArgs
            $initialCheckCodes = if ($fileSystem -eq "HFSX") { @(0, 8) } else { @(0) }
            $initialCheck = Invoke-NativeCommand -Name "fsck_hfs-initial-$safeName" -FilePath $fsckCheck.path -Arguments @("-n", "-f", $imagePath) -AcceptedExitCodes $initialCheckCodes
            $repairExitCodes = if ($fileSystem -eq "HFSX") { @(0, 8) } else { @(0) }
            $repair = Invoke-NativeCommand -Name "fsck_hfs-repair-$safeName" -FilePath $fsckRepair.path -Arguments @("-p", "-f", $imagePath) -AcceptedExitCodes $repairExitCodes
            $finalCheck = Invoke-NativeCommand -Name "fsck_hfs-final-$safeName" -FilePath $fsckCheck.path -Arguments @("-n", "-f", $imagePath) -AcceptedExitCodes $initialCheckCodes
            $extraRepair = $null
            if ($fileSystem -eq "HFSX" -and [int]$finalCheck.exit_code -ne 0) {
                $extraRepair = Invoke-NativeCommand -Name "fsck_hfs-repair2-$safeName" -FilePath $fsckRepair.path -Arguments @("-p", "-f", $imagePath)
                $finalCheck = Invoke-NativeCommand -Name "fsck_hfs-final2-$safeName" -FilePath $fsckCheck.path -Arguments @("-n", "-f", $imagePath)
            }
            if ([int]$finalCheck.exit_code -ne 0) {
                throw "fsck_hfs final check returned $($finalCheck.exit_code) for $fileSystem; refusing to certify a volume that is not clean."
            }
            # The pinned target is re-verified immediately before the raw write.
            Assert-TargetIdentityUnchanged -PinnedDisk $disk -PinnedPartition $partition
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
        if (-not (Test-Path -LiteralPath $probePath -PathType Leaf)) {
            throw "Probe $($probe.name) did not write $probePath."
        }

        $probeReport = Get-Content -LiteralPath $probePath -Raw | ConvertFrom-Json
        if ($null -eq $probeReport -or $probeReport -is [System.Array]) {
            throw "Probe report $probePath is not a single JSON object."
        }
        if ($probeReport.status -isnot [string] -or [string]$probeReport.status -ne "Passed") {
            throw "Probe report $probePath status is '$($probeReport.status)' after $fileSystem format."
        }
        if ($probeReport.detected_file_system -isnot [string]) {
            throw "Probe report $probePath has a malformed detected_file_system value."
        }
        if ([string]$probeReport.detected_file_system -ne $fileSystem) {
            throw "Probe detected '$($probeReport.detected_file_system)' after $fileSystem format."
        }
        if ($null -eq $probeReport.hfs_check -or $probeReport.hfs_check.status -isnot [string]) {
            throw "Probe report $probePath has no HFS consistency check result."
        }
        if ([string]$probeReport.hfs_check.status -ne "Passed") {
            throw "HFS consistency check did not pass after $fileSystem format."
        }

        $report.cases += [pscustomobject]@{
            index = $index
            file_system = $fileSystem
            raw_target = $rawTarget
            staged_image_path = $imagePath
            initial_check_exit_code = [int]$initialCheck.exit_code
            final_check_exit_code = [int]$finalCheck.exit_code
            required_second_repair = ($null -ne $extraRepair)
            detected_file_system = [string]$probeReport.detected_file_system
            probe_input_size_bytes = [string]$probeReport.input_size_bytes
            probe_report = $probePath
            status = "Passed"
            format_command = $format.name
            format_tool_sha256 = $newfs.actual_sha256
            check_tool_sha256 = $fsckCheck.actual_sha256
            repair_tool_sha256 = $fsckRepair.actual_sha256
            repair_command = @($repair.name, $(if ($extraRepair) { $extraRepair.name } else { $null })) | Where-Object { $_ }
            copy_command = $copy.name
            probe_command = $probe.name
        }
        $report.artifacts += $probePath
    }

    $report.after = Get-DiskSnapshot -Number $DiskNumber
    Assert-TargetIdentityUnchanged -PinnedDisk $disk -PinnedPartition $partition
    if ($report.after.friendly_name -ne $report.before.friendly_name -or
        $report.after.serial_number -ne $report.before.serial_number -or
        $report.after.size_bytes -ne $report.before.size_bytes) {
        throw "Disk identity changed between the before and after evidence snapshots."
    }
    if (@($report.cases).Count -ne $FileSystems.Count) {
        throw "Recorded $(@($report.cases).Count) case(s) for $($FileSystems.Count) requested file system(s)."
    }
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

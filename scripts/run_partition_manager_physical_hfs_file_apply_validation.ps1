<#
.SYNOPSIS
    Runs destructive physical HFS+ staged file-mutation proof on an expendable partition.

.DESCRIPTION
    Seeds a selected non-boot Apple HFS GPT partition with the S.A.K. HFS writer
    fixture, stages the raw partition into a sparse image, mutates an existing
    file data fork, resource fork, inline attribute, catalog rename, and secure-delete file/folder
    operations with sak_hfs_writer_cli, repairs/checks the staged image with bundled
    fsck_hfs, copies changed sparse ranges back to the same raw partition, then verifies
    read-back and secure-delete absence through the S.A.K. probe certifier. The selected
    partition contents are destroyed.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [int]$DiskNumber,
    [Parameter(Mandatory = $true)] [int]$PartitionNumber,
    [string]$ExpectedSerialNumber = "",
    [string]$ExpectedFriendlyNamePattern = "",
    [string]$ProjectRoot = "",
    [string]$EvidenceRoot = "artifacts\partition-manager-certification\vm-lab\external-evidence\external.hfs-file-apply-physical",
    [string]$ReportPath = "",
    [string]$CertifierPath = "",
    [string]$HfsWriterCliPath = "",
    [switch]$AllowInternalDisk,
    [switch]$AllowLargeUnpinnedDisk,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$LargeDiskGuardBytes = 64GB
$CopyBufferBytes = 4MB
$StaleSignatureClearBytes = 16MB
$AppleHfsGptType = "{48465300-0000-11aa-aa11-00306543ecac}"

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
}

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
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

function Resolve-BuildTool {
    param([string]$ExplicitPath, [string]$FileName, [string]$Description)
    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        return (Resolve-Path -LiteralPath $ExplicitPath -ErrorAction Stop).Path
    }
    $candidate = Join-Path $ProjectRoot "build\Release\$FileName"
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        throw "$Description was not found. Build Release target $FileName first."
    }
    return (Resolve-Path -LiteralPath $candidate).Path
}

function Resolve-ApprovedFsckHfs {
    $manifestPath = Join-Path $ProjectRoot "tools\filesystem\manifest.json"
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $matches = @($manifest.tools | Where-Object {
        $_.id -eq "fsck_hfs" -and
        @($_.operations) -contains "repair" -and
        @($_.file_systems) -contains "hfs+"
    })
    if ($matches.Count -ne 1) {
        throw "Manifest does not approve exactly one fsck_hfs repair tool for HFS+."
    }
    $tool = $matches[0]
    $path = Join-Path (Join-Path $ProjectRoot "tools\filesystem") ([string]$tool.relative_path)
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        $path = Join-Path $ProjectRoot ("build\Release\tools\filesystem\" + [string]$tool.relative_path)
    }
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Approved fsck_hfs tool not found: $($tool.relative_path)"
    }
    $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    $expectedHash = ([string]$tool.binary_sha256).ToLowerInvariant()
    if ($actualHash -ne $expectedHash) {
        throw "Approved fsck_hfs hash mismatch."
    }
    return [pscustomobject]@{
        id = $tool.id
        operation = "repair"
        file_system = "HFS+"
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

function Write-Json {
    param([string]$Path, [object]$Value)
    $Value | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Read-Json {
    param([string]$Path)
    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
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

function New-SparseImageFile {
    param([string]$Path, [uint64]$SizeBytes)
    Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    New-Item -ItemType File -Path $Path -Force | Out-Null
    $sparseOutput = fsutil sparse setflag $Path 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to mark staged HFS image sparse. $(ConvertTo-PlainText -Value $sparseOutput)"
    }
    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    try {
        $stream.SetLength([int64]$SizeBytes)
    }
    finally {
        $stream.Dispose()
    }
}

function Read-BigEndianUInt16 {
    param([byte[]]$Buffer, [int]$Offset)
    return [uint16]((([uint16]$Buffer[$Offset]) -shl 8) -bor ([uint16]$Buffer[$Offset + 1]))
}

function Read-BigEndianUInt32 {
    param([byte[]]$Buffer, [int]$Offset)
    return [uint32]((([uint32]$Buffer[$Offset]) -shl 24) -bor
        (([uint32]$Buffer[$Offset + 1]) -shl 16) -bor
        (([uint32]$Buffer[$Offset + 2]) -shl 8) -bor
        ([uint32]$Buffer[$Offset + 3]))
}

function Test-PowerOfTwoUInt32 {
    param([uint32]$Value)
    return $Value -ne 0 -and (($Value -band ($Value - 1)) -eq 0)
}

function Get-HfsStagingSizeFromHeader {
    param(
        [System.IO.FileStream]$Stream,
        [uint64]$HeaderOffset,
        [uint64]$VolumeOffset,
        [uint64]$PartitionSizeBytes
    )
    if ($HeaderOffset -gt [uint64]([int64]::MaxValue - 512)) {
        throw "HFS volume header offset overflow."
    }
    [void]$Stream.Seek([int64]$HeaderOffset, [System.IO.SeekOrigin]::Begin)
    $header = New-Object byte[] 512
    $read = $Stream.Read($header, 0, $header.Length)
    if ($read -ne $header.Length) {
        throw "Unable to read complete HFS volume header."
    }
    $signature = [System.Text.Encoding]::ASCII.GetString($header, 0, 2)
    if ($signature -ne "H+" -and $signature -ne "HX") {
        return $null
    }
    $blockSize = Read-BigEndianUInt32 -Buffer $header -Offset 40
    $totalBlocks = Read-BigEndianUInt32 -Buffer $header -Offset 44
    if ($blockSize -lt 512 -or $blockSize -gt 1048576 -or
        -not (Test-PowerOfTwoUInt32 -Value $blockSize) -or $totalBlocks -eq 0) {
        throw "Invalid HFS volume geometry in volume header."
    }
    if ([uint64]$blockSize -gt ([uint64]::MaxValue / [uint64]$totalBlocks)) {
        throw "HFS volume geometry overflows 64-bit size."
    }
    $volumeBytes = [uint64]$blockSize * [uint64]$totalBlocks
    if ($VolumeOffset -gt ([uint64]::MaxValue - $volumeBytes)) {
        throw "HFS wrapped volume size overflows 64-bit size."
    }
    $stagingBytes = $VolumeOffset + $volumeBytes
    if ($stagingBytes -lt 2048 -or $stagingBytes -gt $PartitionSizeBytes) {
        throw "HFS logical staging size $stagingBytes is outside partition size $PartitionSizeBytes."
    }
    return [pscustomobject]@{
        size = [uint64]$stagingBytes
        signature = $signature
        block_size = [uint64]$blockSize
        total_blocks = [uint64]$totalBlocks
        volume_offset = [uint64]$VolumeOffset
    }
}

function Get-HfsVolumeStagingSize {
    param([string]$RawTarget, [uint64]$PartitionSizeBytes)
    if ($PartitionSizeBytes -lt 2048) {
        throw "HFS partition is too small."
    }
    $stream = [System.IO.File]::Open($RawTarget, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
        $direct = Get-HfsStagingSizeFromHeader -Stream $stream -HeaderOffset 1024 -VolumeOffset 0 -PartitionSizeBytes $PartitionSizeBytes
        if ($null -ne $direct) {
            return $direct
        }
        [void]$stream.Seek(1024, [System.IO.SeekOrigin]::Begin)
        $mdb = New-Object byte[] 512
        $read = $stream.Read($mdb, 0, $mdb.Length)
        if ($read -ne $mdb.Length) {
            throw "Unable to read HFS wrapper header."
        }
        $mdbSignature = [System.Text.Encoding]::ASCII.GetString($mdb, 0, 2)
        $embeddedSignature = [System.Text.Encoding]::ASCII.GetString($mdb, 0x7c, 2)
        if ($mdbSignature -eq "BD" -and ($embeddedSignature -eq "H+" -or $embeddedSignature -eq "HX")) {
            $allocationBlockSize = Read-BigEndianUInt32 -Buffer $mdb -Offset 0x14
            $allocationStartSector = Read-BigEndianUInt16 -Buffer $mdb -Offset 0x1c
            $extentStartBlock = Read-BigEndianUInt16 -Buffer $mdb -Offset 0x7e
            $extentBlockCount = Read-BigEndianUInt16 -Buffer $mdb -Offset 0x80
            if ($extentBlockCount -eq 0 -or $allocationBlockSize -lt 512 -or
                -not (Test-PowerOfTwoUInt32 -Value $allocationBlockSize)) {
                throw "Invalid HFS wrapper geometry."
            }
            $volumeOffset = ([uint64]$allocationStartSector * 512) + ([uint64]$extentStartBlock * [uint64]$allocationBlockSize)
            $wrapped = Get-HfsStagingSizeFromHeader -Stream $stream -HeaderOffset ($volumeOffset + 1024) -VolumeOffset $volumeOffset -PartitionSizeBytes $PartitionSizeBytes
            if ($null -eq $wrapped) {
                throw "HFS wrapper points to missing HFS+ volume header."
            }
            return $wrapped
        }
        throw "Unable to locate a direct or wrapped HFS+/HFSX volume header on selected raw target."
    }
    finally {
        $stream.Dispose()
    }
}

function Test-ZeroBuffer {
    param([byte[]]$Buffer, [int]$Count)
    for ($i = 0; $i -lt $Count; $i += 1) {
        if ($Buffer[$i] -ne 0) {
            return $false
        }
    }
    return $true
}

function Get-SparseAllocatedRanges {
    param([string]$Path)
    $output = fsutil sparse queryrange $Path 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to query sparse ranges. $(ConvertTo-PlainText -Value $output)"
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
        throw "No allocated sparse ranges found."
    }
    return $ranges
}

function Write-ZeroRange {
    param([System.IO.FileStream]$TargetStream, [uint64]$Offset, [uint64]$Length)
    if ($Length -eq 0) {
        return
    }
    $zero = New-Object byte[] ([int][Math]::Min([uint64]$CopyBufferBytes, $Length))
    [void]$TargetStream.Seek([int64]$Offset, [System.IO.SeekOrigin]::Begin)
    $remaining = $Length
    while ($remaining -gt 0) {
        $chunk = [int][Math]::Min([uint64]$zero.Length, $remaining)
        $TargetStream.Write($zero, 0, $chunk)
        $remaining -= [uint64]$chunk
    }
}

function Copy-FileRange {
    param([System.IO.FileStream]$SourceStream, [System.IO.FileStream]$TargetStream, [uint64]$Offset, [uint64]$Length)
    $buffer = New-Object byte[] ([int]$CopyBufferBytes)
    [void]$SourceStream.Seek([int64]$Offset, [System.IO.SeekOrigin]::Begin)
    [void]$TargetStream.Seek([int64]$Offset, [System.IO.SeekOrigin]::Begin)
    $remaining = $Length
    while ($remaining -gt 0) {
        $chunk = [int][Math]::Min([uint64]$buffer.Length, $remaining)
        $read = $SourceStream.Read($buffer, 0, $chunk)
        if ($read -le 0) {
            throw "Range copy ended before requested bytes were copied."
        }
        $TargetStream.Write($buffer, 0, $read)
        $remaining -= [uint64]$read
    }
}

function Copy-RawTargetToSparseImage {
    param([string]$RawTarget, [string]$ImagePath, [uint64]$SizeBytes)
    $started = Get-Date
    $source = [System.IO.File]::Open($RawTarget, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    $target = [System.IO.File]::Open($ImagePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::ReadWrite)
    $buffer = New-Object byte[] ([int]$CopyBufferBytes)
    $copiedBytes = [uint64]0
    try {
        $remaining = $SizeBytes
        while ($remaining -gt 0) {
            $chunk = [int][Math]::Min([uint64]$buffer.Length, $remaining)
            $read = $source.Read($buffer, 0, $chunk)
            if ($read -le 0) {
                throw "Raw HFS target read ended before staged copy completed."
            }
            if (-not (Test-ZeroBuffer -Buffer $buffer -Count $read)) {
                $target.Write($buffer, 0, $read)
                $copiedBytes += [uint64]$read
            }
            else {
                [void]$target.Seek([int64]$read, [System.IO.SeekOrigin]::Current)
            }
            $remaining -= [uint64]$read
        }
        $target.Flush()
    }
    finally {
        $source.Dispose()
        $target.Dispose()
    }
    $record = [pscustomobject]@{
        name = "stage-raw-hfs-target"
        file_path = "System.IO.FileStream"
        arguments = @($RawTarget, $ImagePath)
        exit_code = 0
        duration_seconds = [Math]::Round(((Get-Date) - $started).TotalSeconds, 3)
        output = "Copied raw target into sparse image; nonzero bytes observed: $copiedBytes"
    }
    $Script:Commands.Add($record) | Out-Null
    return $record
}

function Copy-SparseImageToRawTarget {
    param([string]$ImagePath, [string]$RawTarget, [uint64]$TargetSizeBytes)
    $started = Get-Date
    $ranges = @(Get-SparseAllocatedRanges -Path $ImagePath)
    $source = [System.IO.File]::Open($ImagePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    $target = [System.IO.File]::Open($RawTarget, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::ReadWrite)
    $copiedBytes = [uint64]0
    $clippedRanges = 0
    try {
        $edgeBytes = [uint64][Math]::Min([uint64]$StaleSignatureClearBytes, $TargetSizeBytes)
        Write-ZeroRange -TargetStream $target -Offset 0 -Length $edgeBytes
        if ($TargetSizeBytes -gt $edgeBytes) {
            Write-ZeroRange -TargetStream $target -Offset ($TargetSizeBytes - $edgeBytes) -Length $edgeBytes
        }
        foreach ($range in $ranges) {
            if ([uint64]$range.offset -ge $TargetSizeBytes) {
                $clippedRanges += 1
                continue
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
        }
        $target.Flush()
    }
    finally {
        $source.Dispose()
        $target.Dispose()
    }
    $record = [pscustomobject]@{
        name = "copy-staged-hfs-image-back"
        file_path = "System.IO.FileStream"
        arguments = @($ImagePath, $RawTarget)
        exit_code = 0
        duration_seconds = [Math]::Round(((Get-Date) - $started).TotalSeconds, 3)
        output = "Copied $($ranges.Count) sparse range(s), $copiedBytes allocated byte(s); clipped $clippedRanges range(s)."
    }
    $Script:Commands.Add($record) | Out-Null
    return $record
}

function Seed-HfsFixtureToRawTarget {
    param([string]$FixturePath, [string]$RawTarget, [uint64]$TargetSizeBytes)
    $started = Get-Date
    $fixtureBytes = [System.IO.File]::ReadAllBytes($FixturePath)
    Assert-Condition -Condition ([uint64]$fixtureBytes.Length -lt $TargetSizeBytes) -Message "Fixture image is larger than target partition."
    $target = [System.IO.File]::Open($RawTarget, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::ReadWrite)
    try {
        $edgeBytes = [uint64][Math]::Min([uint64]$StaleSignatureClearBytes, $TargetSizeBytes)
        Write-ZeroRange -TargetStream $target -Offset 0 -Length $edgeBytes
        if ($TargetSizeBytes -gt $edgeBytes) {
            Write-ZeroRange -TargetStream $target -Offset ($TargetSizeBytes - $edgeBytes) -Length $edgeBytes
        }
        [void]$target.Seek(0, [System.IO.SeekOrigin]::Begin)
        $target.Write($fixtureBytes, 0, $fixtureBytes.Length)
        $tailClear = [uint64][Math]::Min([uint64]$StaleSignatureClearBytes, $TargetSizeBytes - [uint64]$fixtureBytes.Length)
        Write-ZeroRange -TargetStream $target -Offset ([uint64]$fixtureBytes.Length) -Length $tailClear
        $target.Flush()
    }
    finally {
        $target.Dispose()
    }
    $record = [pscustomobject]@{
        name = "seed-hfs-writer-fixture-to-raw"
        file_path = "System.IO.FileStream"
        arguments = @($FixturePath, $RawTarget)
        exit_code = 0
        duration_seconds = [Math]::Round(((Get-Date) - $started).TotalSeconds, 3)
        output = "Seeded $($fixtureBytes.Length) byte HFS writer fixture and cleared target edges."
    }
    $Script:Commands.Add($record) | Out-Null
    return $record
}

function Get-DiskSnapshot {
    param([int]$Number)
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
    param([object]$Disk, [object]$Partition)
    if ($Disk.IsBoot -or $Disk.IsSystem) {
        throw "Refusing to mutate boot/system disk $DiskNumber."
    }
    if ($Disk.IsReadOnly) {
        throw "Refusing to mutate read-only disk $DiskNumber."
    }
    if ($Partition.IsBoot -or $Partition.IsSystem) {
        throw "Refusing to mutate boot/system partition $DiskNumber/$PartitionNumber."
    }
    if ("$($Partition.GptType)" -ne $AppleHfsGptType) {
        throw "Target partition is not Apple HFS GPT type. Found $($Partition.GptType)."
    }
    if (-not $AllowInternalDisk -and "$($Disk.BusType)" -ne "USB") {
        throw "Refusing non-USB disk without -AllowInternalDisk. BusType=$($Disk.BusType)"
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

function New-ReportBase {
    param([string]$OutputPath)
    [ordered]@{
        schema_version = 1
        gate_id = "external.hfs-file-apply-physical"
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
        seed = $null
        staged_mutation = $null
        read_back = $null
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
    $partition = Get-Partition -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber -ErrorAction Stop
    Assert-PhysicalTarget -Disk $disk -Partition $partition

    $rawTarget = "\\?\GLOBALROOT\Device\Harddisk$DiskNumber\Partition$PartitionNumber"
    $certifier = Resolve-BuildTool -ExplicitPath $CertifierPath -FileName "partition_filesystem_probe_certifier.exe" -Description "partition_filesystem_probe_certifier.exe"
    $hfsCli = Resolve-BuildTool -ExplicitPath $HfsWriterCliPath -FileName "sak_hfs_writer_cli.exe" -Description "sak_hfs_writer_cli.exe"
    $fsck = Resolve-ApprovedFsckHfs

    $runRoot = Join-Path $outputRoot ("run-" + (Get-Date).ToUniversalTime().ToString("yyyyMMdd-HHmmss"))
    New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
    $fixturePath = Join-Path $runRoot "seed-hfs-writer-fixture.img"
    $fixtureReportPath = Join-Path $runRoot "seed-fixture.json"
    $seedVerifyPath = Join-Path $runRoot "seed-readback.json"
    $stagedImagePath = Join-Path $runRoot "staged-hfs-file-apply.img"
    $dataPayloadPath = Join-Path $runRoot "data-payload.bin"
    $resourcePayloadPath = Join-Path $runRoot "resource-payload.bin"
    $attributePayloadPath = Join-Path $runRoot "attribute-payload.bin"
    $dataReportPath = Join-Path $runRoot "hfs-data-replace.json"
    $resourceReportPath = Join-Path $runRoot "hfs-resource-replace.json"
    $attributeReportPath = Join-Path $runRoot "hfs-inline-attribute-replace.json"
    $secureFilePayloadPath = Join-Path $runRoot "secure-file-payload.bin"
    $secureFolderPayloadPath = Join-Path $runRoot "secure-folder-payload.bin"
    $secureFileCreateReportPath = Join-Path $runRoot "hfs-secure-file-create.json"
    $secureFileRenameReportPath = Join-Path $runRoot "hfs-secure-file-rename.json"
    $secureFileDeleteReportPath = Join-Path $runRoot "hfs-secure-file-delete.json"
    $secureFolderCreateReportPath = Join-Path $runRoot "hfs-secure-folder-create.json"
    $secureFolderFileCreateReportPath = Join-Path $runRoot "hfs-secure-folder-file-create.json"
    $secureFolderTreeDeleteReportPath = Join-Path $runRoot "hfs-secure-folder-tree-delete.json"
    $finalReadBackPath = Join-Path $runRoot "final-readback.json"

    $dataBytes = [System.Text.Encoding]::ASCII.GetBytes("SAK physical staged HFS data proof`n")
    $resourceBytes = [System.Text.Encoding]::ASCII.GetBytes("SAK physical staged HFS resource proof`n")
    $attributeBytes = [System.Text.Encoding]::ASCII.GetBytes("finder-phys-ok")
    $secureFileBytes = [System.Text.Encoding]::ASCII.GetBytes("SAK physical HFS secure delete proof`n")
    $secureFolderBytes = [System.Text.Encoding]::ASCII.GetBytes("SAK physical HFS secure folder delete proof`n")
    [System.IO.File]::WriteAllBytes($dataPayloadPath, $dataBytes)
    [System.IO.File]::WriteAllBytes($resourcePayloadPath, $resourceBytes)
    [System.IO.File]::WriteAllBytes($attributePayloadPath, $attributeBytes)
    [System.IO.File]::WriteAllBytes($secureFilePayloadPath, $secureFileBytes)
    [System.IO.File]::WriteAllBytes($secureFolderPayloadPath, $secureFolderBytes)
    $expectedDataHash = Get-Sha256HexFromBytes -Bytes $dataBytes
    $expectedResourceHash = Get-Sha256HexFromBytes -Bytes $resourceBytes
    $expectedAttributeHash = Get-Sha256HexFromBytes -Bytes $attributeBytes

    $report.disk = Get-DiskSnapshot -Number $DiskNumber
    $report.partition = [pscustomobject]@{
        disk_number = $DiskNumber
        partition_number = $PartitionNumber
        raw_target = $rawTarget
        gpt_type = [string]$partition.GptType
        size_bytes = [string][uint64]$partition.Size
        offset_bytes = [string][uint64]$partition.Offset
        staging_size_bytes = ""
        staging_geometry = $null
    }
    $report.before = Get-DiskSnapshot -Number $DiskNumber
    $report.tools = @(
        [pscustomobject]@{
            id = "partition_filesystem_probe_certifier"
            path = $certifier
            actual_sha256 = (Get-FileHash -LiteralPath $certifier -Algorithm SHA256).Hash.ToLowerInvariant()
        },
        [pscustomobject]@{
            id = "sak_hfs_writer_cli"
            path = $hfsCli
            actual_sha256 = (Get-FileHash -LiteralPath $hfsCli -Algorithm SHA256).Hash.ToLowerInvariant()
        },
        $fsck
    )

    $fixtureBuild = Invoke-NativeCommand -Name "build-hfs-writer-fixture" -FilePath $certifier -Arguments @(
        "--hfs-build-writer-fixture", $fixturePath,
        "--output", $fixtureReportPath)
    $fixtureReport = Read-Json -Path $fixtureReportPath
    Assert-Condition -Condition ($fixtureReport.status -eq "Passed") -Message "HFS writer fixture build report failed."

    $seed = Seed-HfsFixtureToRawTarget -FixturePath $fixturePath -RawTarget $rawTarget -TargetSizeBytes ([uint64]$partition.Size)
    $seedVerify = Invoke-NativeCommand -Name "verify-seeded-hfs-fixture" -FilePath $certifier -Arguments @(
        "--input", $rawTarget,
        "--output", $seedVerifyPath,
        "--expect", "HFS+",
        "--hfs-check",
        "--hfs-read-file", "/hello.txt",
        "--hfs-read-resource-fork", "/hello.txt",
        "--hfs-read-attribute-file-id", "17",
        "--hfs-read-attribute-name", "com.apple.FinderInfo",
        "--hfs-read-max-bytes", "4096")
    $seedVerifyReport = Read-Json -Path $seedVerifyPath
    Assert-Condition -Condition ($seedVerifyReport.status -eq "Passed") -Message "Seeded HFS fixture did not verify on raw target."

    $stagingGeometry = Get-HfsVolumeStagingSize -RawTarget $rawTarget -PartitionSizeBytes ([uint64]$partition.Size)
    $stagingSizeBytes = [uint64]$stagingGeometry.size
    $report.partition.staging_size_bytes = [string]$stagingSizeBytes
    $report.partition.staging_geometry = $stagingGeometry
    New-SparseImageFile -Path $stagedImagePath -SizeBytes $stagingSizeBytes
    $stage = Copy-RawTargetToSparseImage -RawTarget $rawTarget -ImagePath $stagedImagePath -SizeBytes $stagingSizeBytes

    $dataWrite = Invoke-NativeCommand -Name "hfs-data-fork-replace-staged" -FilePath $hfsCli -Arguments @(
        "replace-image",
        "--target", $stagedImagePath,
        "--hfs-path", "/hello.txt",
        "--payload-file", $dataPayloadPath,
        "--confirm-target",
        "--allow-journaled-volume",
        "--evidence-id", "external.hfs-file-apply-physical.data-fork",
        "--output-json", $dataReportPath)
    $resourceWrite = Invoke-NativeCommand -Name "hfs-resource-fork-replace-staged" -FilePath $hfsCli -Arguments @(
        "replace-resource-fork-image",
        "--target", $stagedImagePath,
        "--hfs-path", "/hello.txt",
        "--payload-file", $resourcePayloadPath,
        "--confirm-target",
        "--allow-journaled-volume",
        "--evidence-id", "external.hfs-file-apply-physical.resource-fork",
        "--output-json", $resourceReportPath)
    $attributeWrite = Invoke-NativeCommand -Name "hfs-inline-attribute-replace-staged" -FilePath $hfsCli -Arguments @(
        "replace-inline-attribute-image",
        "--target", $stagedImagePath,
        "--file-id", "17",
        "--attribute-name", "com.apple.FinderInfo",
        "--payload-file", $attributePayloadPath,
        "--confirm-target",
        "--allow-journaled-volume",
        "--evidence-id", "external.hfs-file-apply-physical.inline-attribute",
        "--output-json", $attributeReportPath)
    $secureFileCreate = Invoke-NativeCommand -Name "hfs-secure-delete-file-create-staged" -FilePath $hfsCli -Arguments @(
        "create-file-image",
        "--target", $stagedImagePath,
        "--hfs-path", "/secure-delete.txt",
        "--payload-file", $secureFilePayloadPath,
        "--confirm-target",
        "--allow-journaled-volume",
        "--evidence-id", "external.hfs-file-apply-physical.secure-file-create",
        "--output-json", $secureFileCreateReportPath)
    $secureFileRename = Invoke-NativeCommand -Name "hfs-secure-delete-file-rename-staged" -FilePath $hfsCli -Arguments @(
        "rename-catalog-entry-image",
        "--target", $stagedImagePath,
        "--hfs-path", "/secure-delete.txt",
        "--destination-hfs-path", "/secure-delete-renamed.txt",
        "--confirm-target",
        "--allow-journaled-volume",
        "--evidence-id", "external.hfs-file-apply-physical.secure-file-rename",
        "--output-json", $secureFileRenameReportPath)
    $secureFileDelete = Invoke-NativeCommand -Name "hfs-secure-delete-file-staged" -FilePath $hfsCli -Arguments @(
        "delete-file-image",
        "--target", $stagedImagePath,
        "--hfs-path", "/secure-delete-renamed.txt",
        "--confirm-target",
        "--allow-journaled-volume",
        "--secure-wipe-released-blocks",
        "--evidence-id", "external.hfs-file-apply-physical.secure-file-delete",
        "--output-json", $secureFileDeleteReportPath)
    $secureFolderCreate = Invoke-NativeCommand -Name "hfs-secure-delete-folder-create-staged" -FilePath $hfsCli -Arguments @(
        "create-empty-folder-image",
        "--target", $stagedImagePath,
        "--hfs-path", "/Secure Delete",
        "--confirm-target",
        "--allow-journaled-volume",
        "--evidence-id", "external.hfs-file-apply-physical.secure-folder-create",
        "--output-json", $secureFolderCreateReportPath)
    $secureFolderFileCreate = Invoke-NativeCommand -Name "hfs-secure-delete-folder-file-create-staged" -FilePath $hfsCli -Arguments @(
        "create-file-image",
        "--target", $stagedImagePath,
        "--hfs-path", "/Secure Delete/nested.txt",
        "--payload-file", $secureFolderPayloadPath,
        "--confirm-target",
        "--allow-journaled-volume",
        "--evidence-id", "external.hfs-file-apply-physical.secure-folder-file-create",
        "--output-json", $secureFolderFileCreateReportPath)
    $secureFolderTreeDelete = Invoke-NativeCommand -Name "hfs-secure-delete-folder-tree-staged" -FilePath $hfsCli -Arguments @(
        "delete-folder-tree-image",
        "--target", $stagedImagePath,
        "--hfs-path", "/Secure Delete",
        "--confirm-target",
        "--allow-journaled-volume",
        "--secure-wipe-released-blocks",
        "--evidence-id", "external.hfs-file-apply-physical.secure-folder-tree-delete",
        "--output-json", $secureFolderTreeDeleteReportPath)

    $dataReport = Read-Json -Path $dataReportPath
    $resourceReport = Read-Json -Path $resourceReportPath
    $attributeReport = Read-Json -Path $attributeReportPath
    $secureFileCreateReport = Read-Json -Path $secureFileCreateReportPath
    $secureFileRenameReport = Read-Json -Path $secureFileRenameReportPath
    $secureFileDeleteReport = Read-Json -Path $secureFileDeleteReportPath
    $secureFolderCreateReport = Read-Json -Path $secureFolderCreateReportPath
    $secureFolderFileCreateReport = Read-Json -Path $secureFolderFileCreateReportPath
    $secureFolderTreeDeleteReport = Read-Json -Path $secureFolderTreeDeleteReportPath
    Assert-Condition -Condition ([bool]$dataReport.ok) -Message "Data-fork staged mutation failed."
    Assert-Condition -Condition ([bool]$resourceReport.ok) -Message "Resource-fork staged mutation failed."
    Assert-Condition -Condition ([bool]$attributeReport.ok) -Message "Inline-attribute staged mutation failed."
    Assert-Condition -Condition ([bool]$secureFileCreateReport.ok) -Message "Secure-delete file create staged mutation failed."
    Assert-Condition -Condition ([bool]$secureFileRenameReport.ok) -Message "Secure-delete file rename staged mutation failed."
    Assert-Condition -Condition ([bool]$secureFileDeleteReport.ok) -Message "Secure-delete file staged mutation failed."
    Assert-Condition -Condition ([bool]$secureFolderCreateReport.ok) -Message "Secure-delete folder create staged mutation failed."
    Assert-Condition -Condition ([bool]$secureFolderFileCreateReport.ok) -Message "Secure-delete folder file create staged mutation failed."
    Assert-Condition -Condition ([bool]$secureFolderTreeDeleteReport.ok) -Message "Secure-delete folder-tree staged mutation failed."
    Assert-Condition -Condition ($dataReport.after_sha256 -eq $expectedDataHash) -Message "Data-fork staged hash mismatch."
    Assert-Condition -Condition ($resourceReport.after_sha256 -eq $expectedResourceHash) -Message "Resource-fork staged hash mismatch."
    Assert-Condition -Condition ($attributeReport.after_sha256 -eq $expectedAttributeHash) -Message "Inline-attribute staged hash mismatch."
    Assert-Condition -Condition ($secureFileRenameReport.destination_hfs_path -eq "/secure-delete-renamed.txt") -Message "Secure-delete file rename destination mismatch."
    Assert-Condition -Condition (([string]::Join(" ", @($secureFileRenameReport.warnings))).Contains("renamed")) -Message "Secure-delete file rename report did not prove catalog rename."
    Assert-Condition -Condition (([string]::Join(" ", @($secureFileDeleteReport.warnings))).Contains("zeroing released allocated blocks")) -Message "Secure file delete report did not prove released-block zeroing."
    Assert-Condition -Condition (([string]::Join(" ", @($secureFolderTreeDeleteReport.warnings))).Contains("zeroing released file blocks")) -Message "Secure folder-tree delete report did not prove released-block zeroing."

    $repair = Invoke-NativeCommand -Name "fsck_hfs-staged-repair" -FilePath $fsck.path -Arguments @("-p", "-f", $stagedImagePath) -AcceptedExitCodes @(0, 8)
    $finalCheck = Invoke-NativeCommand -Name "fsck_hfs-staged-final-check" -FilePath $fsck.path -Arguments @("-n", "-f", $stagedImagePath) -AcceptedExitCodes @(0, 8)
    $copyBack = Copy-SparseImageToRawTarget -ImagePath $stagedImagePath -RawTarget $rawTarget -TargetSizeBytes $stagingSizeBytes
    Remove-Item -LiteralPath $stagedImagePath -Force -ErrorAction SilentlyContinue

    $finalReadBack = Invoke-NativeCommand -Name "verify-hfs-file-apply-raw-readback" -FilePath $certifier -Arguments @(
        "--input", $rawTarget,
        "--output", $finalReadBackPath,
        "--expect", "HFS+",
        "--hfs-check",
        "--hfs-list-path", "/",
        "--hfs-read-file", "/hello.txt",
        "--hfs-read-resource-fork", "/hello.txt",
        "--hfs-read-attribute-file-id", "17",
        "--hfs-read-attribute-name", "com.apple.FinderInfo",
        "--hfs-read-max-bytes", "4096")
    $readBackReport = Read-Json -Path $finalReadBackPath
    Assert-Condition -Condition ($readBackReport.status -eq "Passed") -Message "Final raw read-back report failed."
    Assert-Condition -Condition ($readBackReport.hfs_read_file.sha256 -eq $expectedDataHash) -Message "Final raw data-fork hash mismatch."
    Assert-Condition -Condition ($readBackReport.hfs_read_resource_fork.sha256 -eq $expectedResourceHash) -Message "Final raw resource-fork hash mismatch."
    Assert-Condition -Condition ($readBackReport.hfs_read_attribute.sha256 -eq $expectedAttributeHash) -Message "Final raw inline-attribute hash mismatch."
    $rootNames = @($readBackReport.hfs_listing.entries | ForEach-Object { [string]$_.name })
    Assert-Condition -Condition ($rootNames -notcontains "secure-delete.txt") -Message "Secure-deleted file still appears in final HFS root listing."
    Assert-Condition -Condition ($rootNames -notcontains "secure-delete-renamed.txt") -Message "Renamed secure-deleted file still appears in final HFS root listing."
    Assert-Condition -Condition ($rootNames -notcontains "Secure Delete") -Message "Secure-deleted folder tree still appears in final HFS root listing."

    $report.seed = [pscustomobject]@{
        fixture_report = $fixtureReportPath
        seed_verify_report = $seedVerifyPath
        seed_command = $seed.name
        verify_command = $seedVerify.name
    }
    $report.staged_mutation = [pscustomobject]@{
        stage_command = $stage.name
        data_report = $dataReportPath
        resource_report = $resourceReportPath
        attribute_report = $attributeReportPath
        secure_file_create_report = $secureFileCreateReportPath
        secure_file_rename_report = $secureFileRenameReportPath
        secure_file_delete_report = $secureFileDeleteReportPath
        secure_folder_create_report = $secureFolderCreateReportPath
        secure_folder_file_create_report = $secureFolderFileCreateReportPath
        secure_folder_tree_delete_report = $secureFolderTreeDeleteReportPath
        data_command = $dataWrite.name
        resource_command = $resourceWrite.name
        attribute_command = $attributeWrite.name
        secure_file_create_command = $secureFileCreate.name
        secure_file_rename_command = $secureFileRename.name
        secure_file_delete_command = $secureFileDelete.name
        secure_folder_create_command = $secureFolderCreate.name
        secure_folder_file_create_command = $secureFolderFileCreate.name
        secure_folder_tree_delete_command = $secureFolderTreeDelete.name
        secure_file_delete_zeroed_released_blocks = $true
        secure_folder_tree_delete_zeroed_released_blocks = $true
        repair_command = $repair.name
        final_check_command = $finalCheck.name
        copy_back_command = $copyBack.name
        expected_data_sha256 = $expectedDataHash
        expected_resource_sha256 = $expectedResourceHash
        expected_attribute_sha256 = $expectedAttributeHash
    }
    $report.read_back = [pscustomobject]@{
        final_report = $finalReadBackPath
        verify_command = $finalReadBack.name
        data_sha256 = [string]$readBackReport.hfs_read_file.sha256
        resource_sha256 = [string]$readBackReport.hfs_read_resource_fork.sha256
        attribute_sha256 = [string]$readBackReport.hfs_read_attribute.sha256
        root_listing_entries = $rootNames
        secure_delete_targets_absent = $true
    }
    $report.artifacts = @($fixtureReportPath, $seedVerifyPath, $dataReportPath, $resourceReportPath, $attributeReportPath, $secureFileCreateReportPath, $secureFileRenameReportPath, $secureFileDeleteReportPath, $secureFolderCreateReportPath, $secureFolderFileCreateReportPath, $secureFolderTreeDeleteReportPath, $finalReadBackPath)

    # --- Phase 2: real newfs_hfs volume with extents-overflow growth and
    # --- catalog root-leaf split proven on the physical raw target.
    $newfsHfs = Join-Path $ProjectRoot "tools\filesystem\hfsprogs\newfs_hfs.exe"
    Assert-Condition -Condition (Test-Path -LiteralPath $newfsHfs -PathType Leaf) -Message "Bundled newfs_hfs not found."
    $realVolumePath = Join-Path $runRoot "phase2-newfs-volume.img"
    $phase2Stream = [System.IO.File]::Create($realVolumePath)
    $phase2Stream.SetLength(64MB)
    $phase2Stream.Close()
    $phase2Format = Invoke-NativeCommand -Name "phase2-newfs-format" -FilePath $newfsHfs -Arguments @("-v", "SAKPHYS", $realVolumePath)

    function Invoke-Phase2Cli {
        param([string]$Name, [string[]]$CliArguments, [string]$CliReportPath)
        $null = Invoke-NativeCommand -Name $Name -FilePath $hfsCli -Arguments $CliArguments -AcceptedExitCodes @(0, 1)
        $cliReport = Read-Json -Path $CliReportPath
        Assert-Condition -Condition ([bool]$cliReport.ok) -Message "$Name failed: $(@($cliReport.blockers) -join '; ')"
        return $cliReport
    }

    $phase2FragPayloadPath = Join-Path $runRoot "phase2-frag-payload.bin"
    [System.IO.File]::WriteAllBytes($phase2FragPayloadPath, [byte[]](,[byte]70 * 4096))
    foreach ($index in 1..8) {
        $null = Invoke-Phase2Cli -Name "phase2-create-frag-$index" -CliArguments @(
            "create-file-image", "--target", $realVolumePath,
            "--hfs-path", "/frag-$index.bin", "--payload-file", $phase2FragPayloadPath,
            "--confirm-target", "--evidence-id", "external.hfs-file-apply-physical.phase2",
            "--output-json", (Join-Path $runRoot "phase2-create-frag-$index.json")) `
            -CliReportPath (Join-Path $runRoot "phase2-create-frag-$index.json")
    }
    foreach ($index in @(1, 3, 5, 7)) {
        $null = Invoke-Phase2Cli -Name "phase2-delete-frag-$index" -CliArguments @(
            "delete-file-image", "--target", $realVolumePath,
            "--hfs-path", "/frag-$index.bin",
            "--confirm-target", "--evidence-id", "external.hfs-file-apply-physical.phase2",
            "--output-json", (Join-Path $runRoot "phase2-delete-frag-$index.json")) `
            -CliReportPath (Join-Path $runRoot "phase2-delete-frag-$index.json")
    }
    $phase2Stage1Path = Join-Path $runRoot "phase2-grow-stage1.bin"
    [System.IO.File]::WriteAllBytes($phase2Stage1Path, [byte[]](,[byte]71 * (5 * 4096 + 10)))
    $null = Invoke-Phase2Cli -Name "phase2-grow-stage1" -CliArguments @(
        "grow-image", "--target", $realVolumePath,
        "--hfs-path", "/frag-2.bin", "--payload-file", $phase2Stage1Path,
        "--confirm-target", "--evidence-id", "external.hfs-file-apply-physical.phase2",
        "--output-json", (Join-Path $runRoot "phase2-grow-stage1.json")) `
        -CliReportPath (Join-Path $runRoot "phase2-grow-stage1.json")
    foreach ($index in @(4, 6)) {
        $null = Invoke-Phase2Cli -Name "phase2-delete2-frag-$index" -CliArguments @(
            "delete-file-image", "--target", $realVolumePath,
            "--hfs-path", "/frag-$index.bin",
            "--confirm-target", "--evidence-id", "external.hfs-file-apply-physical.phase2",
            "--output-json", (Join-Path $runRoot "phase2-delete2-frag-$index.json")) `
            -CliReportPath (Join-Path $runRoot "phase2-delete2-frag-$index.json")
    }
    $phase2Stage2Path = Join-Path $runRoot "phase2-grow-stage2.bin"
    $phase2Stage2Bytes = [byte[]]::new(11 * 4096 + 50)
    for ($i = 0; $i -lt $phase2Stage2Bytes.Length; $i++) {
        $phase2Stage2Bytes[$i] = [byte](($i * 11 + 5) % 251)
    }
    [System.IO.File]::WriteAllBytes($phase2Stage2Path, $phase2Stage2Bytes)
    $phase2OverflowHash = (Get-FileHash -LiteralPath $phase2Stage2Path -Algorithm SHA256).Hash.ToLowerInvariant()
    $phase2OverflowReport = Invoke-Phase2Cli -Name "phase2-grow-overflow" -CliArguments @(
        "grow-image", "--target", $realVolumePath,
        "--hfs-path", "/frag-2.bin", "--payload-file", $phase2Stage2Path,
        "--confirm-target", "--evidence-id", "external.hfs-file-apply-physical.phase2-overflow",
        "--output-json", (Join-Path $runRoot "phase2-grow-overflow.json")) `
        -CliReportPath (Join-Path $runRoot "phase2-grow-overflow.json")
    Assert-Condition -Condition (([string]::Join(" ", @($phase2OverflowReport.warnings))).Contains("extents-overflow record")) `
        -Message "Phase-2 overflow growth did not create extents-overflow records."
    $phase2ImageFsck = Invoke-NativeCommand -Name "phase2-fsck-after-overflow" -FilePath $fsck.path -Arguments @("-n", "-f", $realVolumePath)

    $phase2Seed = Seed-HfsFixtureToRawTarget -FixturePath $realVolumePath -RawTarget $rawTarget -TargetSizeBytes ([uint64]$partition.Size)
    $phase2RawVerifyPath = Join-Path $runRoot "phase2-raw-overflow-readback.json"
    $null = Invoke-NativeCommand -Name "phase2-verify-raw-overflow" -FilePath $certifier -Arguments @(
        "--input", $rawTarget,
        "--output", $phase2RawVerifyPath,
        "--expect", "HFS+",
        "--hfs-check",
        "--hfs-read-file", "/frag-2.bin",
        "--hfs-read-max-bytes", "16777216")
    $phase2RawVerify = Read-Json -Path $phase2RawVerifyPath
    Assert-Condition -Condition ($phase2RawVerify.status -eq "Passed") -Message "Phase-2 raw overflow verification failed."
    Assert-Condition -Condition ($phase2RawVerify.hfs_read_file.sha256 -eq $phase2OverflowHash) -Message "Phase-2 raw overflow-file hash mismatch."

    $phase2StagedPath = Join-Path $runRoot "phase2-staged.img"
    $phase2Geometry = Get-HfsVolumeStagingSize -RawTarget $rawTarget -PartitionSizeBytes ([uint64]$partition.Size)
    New-SparseImageFile -Path $phase2StagedPath -SizeBytes ([uint64]$phase2Geometry.size)
    $null = Copy-RawTargetToSparseImage -RawTarget $rawTarget -ImagePath $phase2StagedPath -SizeBytes ([uint64]$phase2Geometry.size)
    $phase2DeleteReport = Invoke-Phase2Cli -Name "phase2-staged-overflow-delete" -CliArguments @(
        "delete-file-image", "--target", $phase2StagedPath,
        "--hfs-path", "/frag-2.bin",
        "--confirm-target", "--evidence-id", "external.hfs-file-apply-physical.phase2-overflow-delete",
        "--output-json", (Join-Path $runRoot "phase2-staged-overflow-delete.json")) `
        -CliReportPath (Join-Path $runRoot "phase2-staged-overflow-delete.json")
    Assert-Condition -Condition (([string]::Join(" ", @($phase2DeleteReport.warnings))).Contains("extents-overflow records for the deleted file were removed")) `
        -Message "Phase-2 staged overflow delete did not remove extents-overflow records."

    $phase2SplitSeen = $false
    foreach ($index in 1..24) {
        $splitReportPath = Join-Path $runRoot ("phase2-split-create-{0:d2}.json" -f $index)
        $splitReport = Invoke-Phase2Cli -Name ("phase2-split-create-{0:d2}" -f $index) -CliArguments @(
            "create-empty-file-image", "--target", $phase2StagedPath,
            "--hfs-path", ("/split-{0:d2}.txt" -f $index),
            "--confirm-target", "--evidence-id", "external.hfs-file-apply-physical.phase2-split",
            "--output-json", $splitReportPath) `
            -CliReportPath $splitReportPath
        if (([string]::Join(" ", @($splitReport.warnings))).Contains("split into two leaf nodes")) {
            $phase2SplitSeen = $true
            break
        }
    }
    Assert-Condition -Condition $phase2SplitSeen -Message "Phase-2 catalog root-leaf split did not trigger."
    $phase2StagedRepair = Invoke-NativeCommand -Name "phase2-fsck-staged-repair" -FilePath $fsck.path -Arguments @("-p", "-f", $phase2StagedPath)
    $phase2StagedCheck = Invoke-NativeCommand -Name "phase2-fsck-staged-final" -FilePath $fsck.path -Arguments @("-n", "-f", $phase2StagedPath)
    $null = Copy-SparseImageToRawTarget -ImagePath $phase2StagedPath -RawTarget $rawTarget -TargetSizeBytes ([uint64]$phase2Geometry.size)
    Remove-Item -LiteralPath $phase2StagedPath -Force -ErrorAction SilentlyContinue

    # Depth-2 staged mutation on the now-split raw volume: create, attribute
    # create, and delete against the two-level catalog with fsck_hfs proof.
    $phase3StagedPath = Join-Path $runRoot "phase3-staged.img"
    New-SparseImageFile -Path $phase3StagedPath -SizeBytes ([uint64]$phase2Geometry.size)
    $null = Copy-RawTargetToSparseImage -RawTarget $rawTarget -ImagePath $phase3StagedPath -SizeBytes ([uint64]$phase2Geometry.size)
    $phase3CreateReport = Invoke-Phase2Cli -Name "phase3-depth2-create" -CliArguments @(
        "create-file-image", "--target", $phase3StagedPath,
        "--hfs-path", "/depth2-proof.bin", "--payload-file", $phase2FragPayloadPath,
        "--confirm-target", "--evidence-id", "external.hfs-file-apply-physical.phase3-create",
        "--output-json", (Join-Path $runRoot "phase3-depth2-create.json")) `
        -CliReportPath (Join-Path $runRoot "phase3-depth2-create.json")
    $phase3AttrReport = Invoke-Phase2Cli -Name "phase3-depth2-attr-create" -CliArguments @(
        "create-inline-attribute-image", "--target", $phase3StagedPath,
        "--file-id", $phase3CreateReport.catalog_id,
        "--attribute-name", "org.sak.physical-proof",
        "--payload-file", $phase2FragPayloadPath,
        "--confirm-target", "--evidence-id", "external.hfs-file-apply-physical.phase3-attr",
        "--output-json", (Join-Path $runRoot "phase3-depth2-attr-create.json")) `
        -CliReportPath (Join-Path $runRoot "phase3-depth2-attr-create.json")
    $null = Invoke-Phase2Cli -Name "phase3-depth2-delete" -CliArguments @(
        "delete-empty-file-image", "--target", $phase3StagedPath,
        "--hfs-path", "/split-02.txt",
        "--confirm-target", "--evidence-id", "external.hfs-file-apply-physical.phase3-delete",
        "--output-json", (Join-Path $runRoot "phase3-depth2-delete.json")) `
        -CliReportPath (Join-Path $runRoot "phase3-depth2-delete.json")

    # Wave-2 raw proofs: fork-backed attribute create/delete, depth-3 bulk
    # create with one delete, and the journal-replay blocker on this
    # non-journaled volume. fsck validates the staged volume afterwards.
    $phase3ForkAttrReport = Invoke-Phase2Cli -Name "phase3-fork-attr-create" -CliArguments @(
        "create-fork-attribute-image", "--target", $phase3StagedPath,
        "--file-id", $phase3CreateReport.catalog_id,
        "--attribute-name", "org.sak.fork-physical",
        "--payload-file", $phase2FragPayloadPath,
        "--confirm-target", "--evidence-id", "external.hfs-file-apply-physical.phase3-fork-attr",
        "--output-json", (Join-Path $runRoot "phase3-fork-attr-create.json")) `
        -CliReportPath (Join-Path $runRoot "phase3-fork-attr-create.json")
    Assert-Condition -Condition ([bool]$phase3ForkAttrReport.ok) -Message "Phase-3 fork attribute create failed."
    $phase3ForkAttrDelete = Invoke-Phase2Cli -Name "phase3-fork-attr-delete" -CliArguments @(
        "delete-attribute-image", "--target", $phase3StagedPath,
        "--file-id", $phase3CreateReport.catalog_id,
        "--attribute-name", "org.sak.fork-physical",
        "--confirm-target", "--evidence-id", "external.hfs-file-apply-physical.phase3-attr-delete",
        "--output-json", (Join-Path $runRoot "phase3-fork-attr-delete.json")) `
        -CliReportPath (Join-Path $runRoot "phase3-fork-attr-delete.json")
    Assert-Condition -Condition ([bool]$phase3ForkAttrDelete.ok) -Message "Phase-3 fork attribute delete failed."
    $phase3Depth3Report = Invoke-Phase2Cli -Name "phase3-depth3-create" -CliArguments @(
        "create-empty-files-image", "--target", $phase3StagedPath,
        "--hfs-path", "/d3raw", "--file-count", "120", "--name-pad", "220",
        "--confirm-target", "--evidence-id", "external.hfs-file-apply-physical.phase3-depth3",
        "--output-json", (Join-Path $runRoot "phase3-depth3-create.json")) `
        -CliReportPath (Join-Path $runRoot "phase3-depth3-create.json")
    Assert-Condition -Condition ([bool]$phase3Depth3Report.ok) -Message "Phase-3 depth-3 bulk create failed."
    $phase3Depth3Pad = "x" * 220
    $null = Invoke-Phase2Cli -Name "phase3-depth3-delete" -CliArguments @(
        "delete-empty-file-image", "--target", $phase3StagedPath,
        "--hfs-path", "/d3raw-0030-$phase3Depth3Pad.txt",
        "--confirm-target", "--evidence-id", "external.hfs-file-apply-physical.phase3-depth3-delete",
        "--output-json", (Join-Path $runRoot "phase3-depth3-delete.json")) `
        -CliReportPath (Join-Path $runRoot "phase3-depth3-delete.json")
    $phase3JournalBlockedPath = Join-Path $runRoot "phase3-journal-blocked.json"
    $null = Invoke-NativeCommand -Name "phase3-journal-blocked" -FilePath $hfsCli -AcceptedExitCodes @(1) -Arguments @(
        "replay-journal-image", "--target", $phase3StagedPath,
        "--confirm-target", "--allow-journaled-volume",
        "--evidence-id", "external.hfs-file-apply-physical.phase3-journal-blocked",
        "--output-json", $phase3JournalBlockedPath)
    $phase3JournalBlocked = Read-Json -Path $phase3JournalBlockedPath
    Assert-Condition -Condition ((@($phase3JournalBlocked.blockers) -join " ").Contains("not journaled")) -Message "Phase-3 journal replay did not report the non-journaled blocker."

    $phase3Repair = Invoke-NativeCommand -Name "phase3-fsck-staged-repair" -FilePath $fsck.path -Arguments @("-p", "-f", $phase3StagedPath)
    $phase3Check = Invoke-NativeCommand -Name "phase3-fsck-staged-final" -FilePath $fsck.path -Arguments @("-n", "-f", $phase3StagedPath)
    $null = Copy-SparseImageToRawTarget -ImagePath $phase3StagedPath -RawTarget $rawTarget -TargetSizeBytes ([uint64]$phase2Geometry.size)
    Remove-Item -LiteralPath $phase3StagedPath -Force -ErrorAction SilentlyContinue

    $phase2FinalPath = Join-Path $runRoot "phase2-raw-final-readback.json"
    $null = Invoke-NativeCommand -Name "phase2-verify-raw-final" -FilePath $certifier -Arguments @(
        "--input", $rawTarget,
        "--output", $phase2FinalPath,
        "--expect", "HFS+",
        "--hfs-check",
        "--hfs-list-path", "/",
        "--hfs-read-file", "/depth2-proof.bin",
        "--hfs-read-attribute-file-id", $phase3CreateReport.catalog_id,
        "--hfs-read-attribute-name", "org.sak.physical-proof",
        "--hfs-read-max-bytes", "1048576")
    $phase2Final = Read-Json -Path $phase2FinalPath
    Assert-Condition -Condition ($phase2Final.status -eq "Passed") -Message "Phase-2 final raw read-back failed."
    $phase2FragHash = (Get-FileHash -LiteralPath $phase2FragPayloadPath -Algorithm SHA256).Hash.ToLowerInvariant()
    Assert-Condition -Condition ($phase2Final.hfs_read_file.sha256 -eq $phase2FragHash) -Message "Phase-3 depth-2 created file hash mismatch on raw target."
    Assert-Condition -Condition ($phase2Final.hfs_read_attribute.sha256 -eq $phase2FragHash) -Message "Phase-3 created attribute hash mismatch on raw target."
    $phase2RootNames = @($phase2Final.hfs_listing.entries | ForEach-Object { [string]$_.name })
    Assert-Condition -Condition ($phase2RootNames -notcontains "frag-2.bin") -Message "Phase-2 overflow-deleted file still appears on raw target."
    Assert-Condition -Condition ($phase2RootNames -notcontains "split-02.txt") -Message "Phase-3 depth-2 deleted file still appears on raw target."
    Assert-Condition -Condition ($phase2RootNames -contains "split-01.txt") -Message "Phase-2 split-created file missing from raw target listing."
    Assert-Condition -Condition ([bool]$phase3AttrReport.ok) -Message "Phase-3 attribute create report missing."
    Assert-Condition -Condition ($phase3Repair.exit_code -eq 0 -and $phase3Check.exit_code -eq 0) -Message "Phase-3 staged fsck did not pass."

    $report | Add-Member -NotePropertyName "broad_growth_and_split" -NotePropertyValue ([pscustomobject]@{
        newfs_format_command = $phase2Format.name
        overflow_growth_report = (Join-Path $runRoot "phase2-grow-overflow.json")
        overflow_payload_sha256 = $phase2OverflowHash
        image_fsck_command = $phase2ImageFsck.name
        raw_overflow_readback_report = $phase2RawVerifyPath
        staged_overflow_delete_report = (Join-Path $runRoot "phase2-staged-overflow-delete.json")
        catalog_split_triggered = $phase2SplitSeen
        staged_repair_command = $phase2StagedRepair.name
        staged_final_check_command = $phase2StagedCheck.name
        raw_final_readback_report = $phase2FinalPath
        seed_command = $phase2Seed.name
    })
    $report.artifacts += @($phase2RawVerifyPath, $phase2FinalPath)

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
    Write-Json -Path $ReportPath -Value $report
}

Write-Host "Physical HFS File Apply validation passed on disk $DiskNumber partition $PartitionNumber."
Write-Host "Report: $ReportPath"

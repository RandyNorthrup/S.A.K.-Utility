<#
.SYNOPSIS
    Runs destructive physical cross-filesystem proof on an expendable external disk.

.DESCRIPTION
    Wipes one explicitly selected non-boot disk, formats ext2/ext3/ext4 through
    the approved bundled e2fsprogs manifest, verifies read-only repair/probe
    paths after format/grow/shrink, writes an original Linux swap header, and
    emits JSON evidence.

    This script is intentionally separate from the VM-only runners. It allows
    large physical media only when the caller pins disk identity with serial or
    friendly-name guards, and it refuses boot/system disks.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [int]$DiskNumber,
    [string]$ExpectedSerialNumber = "",
    [string]$ExpectedFriendlyNamePattern = "",
    [ValidateSet("ext2", "ext3", "ext4")] [string[]]$FileSystems = @("ext2", "ext3", "ext4"),
    [string]$ProjectRoot = "",
    [string]$EvidenceRoot = "artifacts\partition-manager-certification\vm-lab\external-evidence\external.cross-filesystem-physical-destructive",
    [string]$ReportPath = "",
    [string]$CertifierPath = "",
    [uint64]$InitialSizeBytes = 512MB,
    [uint64]$GrowByBytes = 256MB,
    [uint64]$ShrinkByBytes = 128MB,
    [uint64]$LinuxSwapSizeBytes = 128MB,
    [ValidateSet(4096, 8192, 16384, 65536)] [int]$LinuxSwapPageSizeBytes = 4096,
    [string]$LinuxSwapLabel = "SAKPHY_SWAP",
    [switch]$AllowInternalDisk,
    [switch]$AllowLargeUnpinnedDisk,
    [switch]$NoCleanup,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$LinuxSwapGptType = "{0657FD6D-A4AB-43C4-84E5-0933C84B4F4F}"
$LargeDiskGuardBytes = 64GB

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

function Get-ToolRecord {
    param(
        [Parameter(Mandatory = $true)] [object]$Manifest,
        [Parameter(Mandatory = $true)] [string]$ToolId,
        [Parameter(Mandatory = $true)] [string]$Operation,
        [Parameter(Mandatory = $true)] [string]$FileSystem
    )

    $matches = @($Manifest.tools | Where-Object {
        $_.id -eq $ToolId -and
        @($_.operations) -contains $Operation -and
        @($_.file_systems) -contains $FileSystem
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
        is_offline = [bool]$disk.IsOffline
        is_read_only = [bool]$disk.IsReadOnly
        partitions = $partitions
    }
}

function Assert-DisposablePhysicalDisk {
    param([Parameter(Mandatory = $true)] [int]$Number)

    if (-not $Force) {
        throw "Pass -Force after confirming DiskNumber is expendable physical media."
    }
    if ($Number -le 0) {
        throw "Refusing disk ${Number}: disk 0 and negative disk numbers are not disposable targets."
    }

    $disk = Get-Disk -Number $Number -ErrorAction Stop
    if ($disk.IsBoot -or $disk.IsSystem) {
        throw "Refusing disk ${Number}: boot/system disk."
    }
    if (-not $AllowInternalDisk -and "$($disk.BusType)" -ne "USB") {
        throw "Refusing disk ${Number}: expected USB external media, got BusType=$($disk.BusType)."
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedSerialNumber) -and
        "$($disk.SerialNumber)" -ne $ExpectedSerialNumber) {
        throw "Refusing disk ${Number}: serial mismatch. Expected '$ExpectedSerialNumber', got '$($disk.SerialNumber)'."
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedFriendlyNamePattern) -and
        "$($disk.FriendlyName)" -notmatch $ExpectedFriendlyNamePattern) {
        throw "Refusing disk ${Number}: friendly-name mismatch. Pattern '$ExpectedFriendlyNamePattern', got '$($disk.FriendlyName)'."
    }
    if ([uint64]$disk.Size -gt [uint64]$LargeDiskGuardBytes -and
        [string]::IsNullOrWhiteSpace($ExpectedSerialNumber) -and
        [string]::IsNullOrWhiteSpace($ExpectedFriendlyNamePattern) -and
        -not $AllowLargeUnpinnedDisk) {
        throw "Refusing large disk ${Number}: pass ExpectedSerialNumber or ExpectedFriendlyNamePattern."
    }
    if ($disk.IsOffline) {
        Set-Disk -Number $Number -IsOffline $false
    }
    if ($disk.IsReadOnly) {
        Set-Disk -Number $Number -IsReadOnly $false
    }
}

function Clear-TargetDisk {
    param([Parameter(Mandatory = $true)] [int]$Number)

    Assert-DisposablePhysicalDisk -Number $Number
    $disk = Get-Disk -Number $Number -ErrorAction Stop
    $partitions = @(Get-Partition -DiskNumber $Number -ErrorAction SilentlyContinue)
    if ($disk.PartitionStyle -eq "RAW" -and $partitions.Count -eq 0) {
        return "Disk $Number already RAW."
    }
    Clear-Disk -Number $Number -RemoveData -RemoveOEM -Confirm:$false -ErrorAction Stop
    return "Disk $Number cleared to RAW."
}

function Initialize-TestPartition {
    param(
        [Parameter(Mandatory = $true)] [int]$Number,
        [Parameter(Mandatory = $true)] [uint64]$SizeBytes
    )

    Initialize-Disk -Number $Number -PartitionStyle GPT -ErrorAction Stop
    $partition = New-Partition -DiskNumber $Number -Size $SizeBytes -ErrorAction Stop
    return Get-Partition -DiskNumber $Number -PartitionNumber $partition.PartitionNumber -ErrorAction Stop
}

function Invoke-CertifierProbe {
    param(
        [Parameter(Mandatory = $true)] [string]$Certifier,
        [Parameter(Mandatory = $true)] [string]$TargetPath,
        [Parameter(Mandatory = $true)] [string]$ExpectedFileSystem,
        [Parameter(Mandatory = $true)] [string]$ReportPath,
        [uint64]$InputSizeBytes = 0
    )

    $args = @("--input", $TargetPath, "--output", $ReportPath, "--expect", $ExpectedFileSystem, "--require-sane")
    if ($InputSizeBytes -gt 0) {
        $args += @("--input-size-bytes", ([string]$InputSizeBytes))
    }
    Invoke-NativeCommand -Name "sak-probe-$ExpectedFileSystem" -FilePath $Certifier -Arguments $args | Out-Null
    $probe = Get-Content -LiteralPath $ReportPath -Raw | ConvertFrom-Json
    if ($probe.status -ne "Passed") {
        throw "S.A.K. probe failed for $ExpectedFileSystem on $TargetPath."
    }
    return $probe
}

function Invoke-ExtScenario {
    param(
        [Parameter(Mandatory = $true)] [string]$FileSystem,
        [Parameter(Mandatory = $true)] [object]$Tools,
        [Parameter(Mandatory = $true)] [string]$Certifier,
        [Parameter(Mandatory = $true)] [string]$RunRoot
    )

    Clear-TargetDisk -Number $DiskNumber | Out-Null
    $partition = Initialize-TestPartition -Number $DiskNumber -SizeBytes $InitialSizeBytes
    $rawTarget = "\\?\GLOBALROOT\Device\Harddisk$DiskNumber\Partition$($partition.PartitionNumber)"
    $label = ("SAK{0}PHY" -f $FileSystem.ToUpperInvariant())

    Invoke-NativeCommand -Name "mke2fs-format-$FileSystem" -FilePath $Tools.mke2fs.path -Arguments @("-q", "-t", $FileSystem, "-F", "-L", $label, $rawTarget) | Out-Null
    Invoke-NativeCommand -Name "e2fsck-repair-clean-$FileSystem" -FilePath $Tools.e2fsck.path -Arguments @("-p", "-f", $rawTarget) -AcceptedExitCodes @(0, 1) | Out-Null
    Invoke-NativeCommand -Name "e2fsck-readonly-after-format-$FileSystem" -FilePath $Tools.e2fsck.path -Arguments @("-n", "-f", $rawTarget) | Out-Null
    $formatProbePath = Join-Path $RunRoot "$FileSystem-after-format-probe.json"
    $formatProbe = Invoke-CertifierProbe -Certifier $Certifier -TargetPath $rawTarget -ExpectedFileSystem $FileSystem -ReportPath $formatProbePath -InputSizeBytes ([uint64]$partition.Size)

    $grownSize = [uint64]$partition.Size + [uint64]$GrowByBytes
    Resize-Partition -DiskNumber $DiskNumber -PartitionNumber $partition.PartitionNumber -Size $grownSize -ErrorAction Stop
    Update-HostStorageCache -ErrorAction SilentlyContinue
    Invoke-NativeCommand -Name "resize2fs-grow-$FileSystem" -FilePath $Tools.resize2fs.path -Arguments @($rawTarget) | Out-Null
    Invoke-NativeCommand -Name "e2fsck-readonly-after-grow-$FileSystem" -FilePath $Tools.e2fsck.path -Arguments @("-n", "-f", $rawTarget) | Out-Null
    $growProbePath = Join-Path $RunRoot "$FileSystem-after-grow-probe.json"
    $growProbe = Invoke-CertifierProbe -Certifier $Certifier -TargetPath $rawTarget -ExpectedFileSystem $FileSystem -ReportPath $growProbePath -InputSizeBytes $grownSize

    if ($ShrinkByBytes -le 0 -or $ShrinkByBytes -ge $grownSize) {
        throw "ShrinkByBytes must be smaller than grown size."
    }
    $shrunkSize = [uint64]($grownSize - [uint64]$ShrinkByBytes)
    if ($shrunkSize -le $InitialSizeBytes) {
        throw "ShrinkByBytes must leave shrunk size larger than initial size."
    }
    $shrunkKilobytes = [uint64][Math]::Floor([double]$shrunkSize / 1KB)
    Invoke-NativeCommand -Name "e2fsck-repair-before-shrink-$FileSystem" -FilePath $Tools.e2fsck.path -Arguments @("-p", "-f", $rawTarget) -AcceptedExitCodes @(0, 1) | Out-Null
    Invoke-NativeCommand -Name "resize2fs-shrink-$FileSystem" -FilePath $Tools.resize2fs.path -Arguments @("-p", $rawTarget, "$($shrunkKilobytes)K") | Out-Null
    Resize-Partition -DiskNumber $DiskNumber -PartitionNumber $partition.PartitionNumber -Size $shrunkSize -ErrorAction Stop
    Update-HostStorageCache -ErrorAction SilentlyContinue
    Invoke-NativeCommand -Name "e2fsck-readonly-after-shrink-$FileSystem" -FilePath $Tools.e2fsck.path -Arguments @("-n", "-f", $rawTarget) | Out-Null
    $shrinkProbePath = Join-Path $RunRoot "$FileSystem-after-shrink-probe.json"
    $shrinkProbe = Invoke-CertifierProbe -Certifier $Certifier -TargetPath $rawTarget -ExpectedFileSystem $FileSystem -ReportPath $shrinkProbePath -InputSizeBytes $shrunkSize

    return [pscustomobject]@{
        file_system = $FileSystem
        partition_number = [int]$partition.PartitionNumber
        raw_target = $rawTarget
        initial_size_bytes = [string][uint64]$InitialSizeBytes
        grown_size_bytes = [string][uint64]$grownSize
        shrunk_size_bytes = [string][uint64]$shrunkSize
        probes = @(
            [pscustomobject]@{ stage = "after-format"; report_path = $formatProbePath; detected_file_system = $formatProbe.detected_file_system },
            [pscustomobject]@{ stage = "after-grow"; report_path = $growProbePath; detected_file_system = $growProbe.detected_file_system },
            [pscustomobject]@{ stage = "after-shrink"; report_path = $shrinkProbePath; detected_file_system = $shrinkProbe.detected_file_system }
        )
    }
}

function Write-UInt32Le {
    param(
        [Parameter(Mandatory = $true)] [byte[]]$Buffer,
        [Parameter(Mandatory = $true)] [int]$Offset,
        [Parameter(Mandatory = $true)] [uint32]$Value
    )

    [Array]::Copy([BitConverter]::GetBytes($Value), 0, $Buffer, $Offset, 4)
}

function Read-UInt32Le {
    param(
        [Parameter(Mandatory = $true)] [byte[]]$Buffer,
        [Parameter(Mandatory = $true)] [int]$Offset
    )

    return [BitConverter]::ToUInt32($Buffer, $Offset)
}

function ConvertTo-UuidText {
    param([Parameter(Mandatory = $true)] [byte[]]$Bytes)

    $hex = ($Bytes | ForEach-Object { $_.ToString("x2") }) -join ""
    return "{0}-{1}-{2}-{3}-{4}" -f $hex.Substring(0, 8),
        $hex.Substring(8, 4),
        $hex.Substring(12, 4),
        $hex.Substring(16, 4),
        $hex.Substring(20, 12)
}

function New-LinuxSwapHeader {
    param(
        [Parameter(Mandatory = $true)] [uint64]$PartitionSizeBytes,
        [Parameter(Mandatory = $true)] [int]$PageSize,
        [Parameter(Mandatory = $true)] [string]$VolumeLabel
    )

    $pageCount = [uint64][Math]::Floor([double]$PartitionSizeBytes / [double]$PageSize)
    if ($pageCount -lt 2) {
        throw "Linux swap partition is too small."
    }
    if ($pageCount -gt [uint64]4294967295) {
        throw "Linux swap page count exceeds v1 header limit."
    }

    $label = $VolumeLabel.Trim()
    if ($label.Length -gt 16) {
        $label = $label.Substring(0, 16)
    }
    $header = New-Object byte[] $PageSize
    Write-UInt32Le -Buffer $header -Offset 1024 -Value ([uint32]1)
    Write-UInt32Le -Buffer $header -Offset 1028 -Value ([uint32]($pageCount - 1))
    Write-UInt32Le -Buffer $header -Offset 1032 -Value ([uint32]0)

    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    $uuidBytes = New-Object byte[] 16
    try {
        $rng.GetBytes($uuidBytes)
    }
    finally {
        $rng.Dispose()
    }
    [Array]::Copy($uuidBytes, 0, $header, 1036, 16)

    $labelBytes = [System.Text.Encoding]::ASCII.GetBytes($label)
    if ($labelBytes.Length -gt 0) {
        [Array]::Copy($labelBytes, 0, $header, 1052, [Math]::Min($labelBytes.Length, 16))
    }
    $signatureBytes = [System.Text.Encoding]::ASCII.GetBytes("SWAPSPACE2")
    [Array]::Copy($signatureBytes, 0, $header, $PageSize - 10, 10)

    return [pscustomobject]@{
        bytes = $header
        page_size_bytes = $PageSize
        page_count = [string]$pageCount
        last_page = [string]([uint64]$pageCount - 1)
        label = $label
        uuid = ConvertTo-UuidText -Bytes $uuidBytes
    }
}

function Read-RawBytes {
    param(
        [Parameter(Mandatory = $true)] [string]$TargetPath,
        [Parameter(Mandatory = $true)] [int]$Length
    )

    $buffer = New-Object byte[] $Length
    $stream = [System.IO.File]::Open($TargetPath,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite)
    try {
        $read = $stream.Read($buffer, 0, $buffer.Length)
        if ($read -lt $Length) {
            throw "Raw read was short."
        }
    }
    finally {
        $stream.Dispose()
    }
    return $buffer
}

function Write-RawBytes {
    param(
        [Parameter(Mandatory = $true)] [string]$TargetPath,
        [Parameter(Mandatory = $true)] [byte[]]$Bytes
    )

    $stream = [System.IO.File]::Open($TargetPath,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::ReadWrite,
        [System.IO.FileShare]::ReadWrite)
    try {
        [void]$stream.Seek(0, [System.IO.SeekOrigin]::Begin)
        $stream.Write($Bytes, 0, $Bytes.Length)
        $stream.Flush()
    }
    finally {
        $stream.Dispose()
    }
}

function Test-LinuxSwapHeader {
    param(
        [Parameter(Mandatory = $true)] [byte[]]$Header,
        [Parameter(Mandatory = $true)] [object]$Expected
    )

    $signature = [System.Text.Encoding]::ASCII.GetString($Header, [int]$Expected.page_size_bytes - 10, 10)
    $version = Read-UInt32Le -Buffer $Header -Offset 1024
    $lastPage = Read-UInt32Le -Buffer $Header -Offset 1028
    $label = [System.Text.Encoding]::ASCII.GetString($Header, 1052, 16).TrimEnd([char]0)
    if ($signature -ne "SWAPSPACE2") {
        throw "Linux swap signature verification failed."
    }
    if ($version -ne 1) {
        throw "Linux swap version verification failed."
    }
    if ([string]$lastPage -ne [string]$Expected.last_page) {
        throw "Linux swap last-page verification failed."
    }
    if ($label -ne [string]$Expected.label) {
        throw "Linux swap label verification failed."
    }
    return [pscustomobject]@{
        signature = $signature
        version = [uint32]$version
        last_page = [string]$lastPage
        label = $label
    }
}

function Invoke-LinuxSwapScenario {
    param(
        [Parameter(Mandatory = $true)] [string]$Certifier,
        [Parameter(Mandatory = $true)] [string]$RunRoot
    )

    Clear-TargetDisk -Number $DiskNumber | Out-Null
    $partition = Initialize-TestPartition -Number $DiskNumber -SizeBytes $LinuxSwapSizeBytes
    Set-Partition -DiskNumber $DiskNumber -PartitionNumber $partition.PartitionNumber -GptType $LinuxSwapGptType -ErrorAction Stop
    $partition = Get-Partition -DiskNumber $DiskNumber -PartitionNumber $partition.PartitionNumber -ErrorAction Stop
    $rawTarget = "\\?\GLOBALROOT\Device\Harddisk$DiskNumber\Partition$($partition.PartitionNumber)"

    $header = New-LinuxSwapHeader -PartitionSizeBytes ([uint64]$partition.Size) -PageSize $LinuxSwapPageSizeBytes -VolumeLabel $LinuxSwapLabel
    Write-RawBytes -TargetPath $rawTarget -Bytes $header.bytes
    $verification = Test-LinuxSwapHeader -Header (Read-RawBytes -TargetPath $rawTarget -Length $LinuxSwapPageSizeBytes) -Expected $header
    $probePath = Join-Path $RunRoot "linux-swap-probe.json"
    $probe = Invoke-CertifierProbe -Certifier $Certifier -TargetPath $rawTarget -ExpectedFileSystem "Linux swap" -ReportPath $probePath -InputSizeBytes ([uint64]$partition.Size)

    return [pscustomobject]@{
        file_system = "Linux swap"
        partition_number = [int]$partition.PartitionNumber
        partition_gpt_type = $LinuxSwapGptType
        raw_target = $rawTarget
        size_bytes = [string][uint64]$partition.Size
        header = [pscustomobject]@{
            page_size_bytes = $header.page_size_bytes
            page_count = $header.page_count
            last_page = $header.last_page
            label = $header.label
            uuid = $header.uuid
        }
        verification = $verification
        probe = [pscustomobject]@{
            report_path = $probePath
            detected_file_system = $probe.detected_file_system
        }
    }
}

$Script:Commands = [System.Collections.Generic.List[object]]::new()
$started = (Get-Date).ToUniversalTime().ToString("o")
$resolvedEvidenceRoot = Resolve-ProjectPath -Path $EvidenceRoot
if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = Join-Path $resolvedEvidenceRoot "report.json"
}
else {
    $ReportPath = Resolve-ProjectPath -Path $ReportPath
}
$runRoot = Join-Path $resolvedEvidenceRoot ("run-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
New-Item -ItemType Directory -Path (Split-Path -Parent $ReportPath) -Force | Out-Null

$status = "Failed"
$errorText = ""
$cleanup = ""
$before = $null
$after = $null
$toolHashes = @()
$extResults = @()
$swapResult = $null
$cleanupAllowed = $false
$transcriptStarted = $false
try {
    Start-Transcript -Path (Join-Path $runRoot "physical-cross-filesystem-destructive.log") -Force | Out-Null
    $transcriptStarted = $true
}
catch {
    $transcriptStarted = $false
}

try {
    Push-Location $ProjectRoot
    if (-not (Test-IsAdmin)) {
        throw "Run this script from elevated PowerShell."
    }

    $certifier = Resolve-Certifier
    Assert-DisposablePhysicalDisk -Number $DiskNumber
    $cleanupAllowed = $true
    $before = Get-DiskSnapshot -Number $DiskNumber

    $manifestPath = Join-Path $ProjectRoot "tools\filesystem\manifest.json"
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    foreach ($fileSystem in $FileSystems) {
        $tools = [pscustomobject]@{
            mke2fs = Resolve-ApprovedTool -Manifest $manifest -ToolId "mke2fs" -Operation "format" -FileSystem $fileSystem
            e2fsck = Resolve-ApprovedTool -Manifest $manifest -ToolId "e2fsck" -Operation "repair" -FileSystem $fileSystem
            resize2fs = Resolve-ApprovedTool -Manifest $manifest -ToolId "resize2fs" -Operation "resize" -FileSystem $fileSystem
        }
        $toolHashes += @($tools.mke2fs, $tools.e2fsck, $tools.resize2fs)
        $extResults += Invoke-ExtScenario -FileSystem $fileSystem -Tools $tools -Certifier $certifier -RunRoot $runRoot
    }
    $swapResult = Invoke-LinuxSwapScenario -Certifier $certifier -RunRoot $runRoot

    $after = Get-DiskSnapshot -Number $DiskNumber
    $cleanup = if ($NoCleanup) { "Cleanup skipped by -NoCleanup." } else { Clear-TargetDisk -Number $DiskNumber }
    $status = "Passed"
}
catch {
    $status = "Failed"
    $errorText = ConvertTo-PlainText -Value @($_)
    try {
        if (-not $NoCleanup -and $cleanupAllowed) {
            $cleanup = Clear-TargetDisk -Number $DiskNumber
        }
        elseif (-not $cleanupAllowed) {
            $cleanup = "Cleanup not attempted because admin and disk identity guards did not complete."
        }
    }
    catch {
        $cleanup = "Cleanup failed: $(ConvertTo-PlainText -Value @($_))"
    }
}
finally {
    try {
        Pop-Location
    }
    catch {
    }
    if ($transcriptStarted) {
        Stop-Transcript | Out-Null
    }
}

$report = [pscustomobject]@{
    schema_version = 1
    gate_id = "external.cross-filesystem-physical-destructive"
    status = $status
    destructive = $true
    writes_target_media = $true
    started_at_utc = $started
    finished_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    computer_name = $env:COMPUTERNAME
    user = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    disk_number = $DiskNumber
    expected_serial_number = $ExpectedSerialNumber
    expected_friendly_name_pattern = $ExpectedFriendlyNamePattern
    file_systems = @($FileSystems)
    before_layout = $before
    ext_results = @($extResults)
    linux_swap_result = $swapResult
    after_layout = $after
    tool_hashes = @($toolHashes | Sort-Object id, file_system, operation -Unique)
    commands = @($Script:Commands.ToArray())
    cleanup = $cleanup
    error = $errorText
}
$report | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $ReportPath -Encoding UTF8
$report | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $runRoot "report.json") -Encoding UTF8

if ($status -ne "Passed") {
    throw "Physical cross-filesystem destructive validation failed. Report: $ReportPath`n$errorText"
}

Write-Host "Physical cross-filesystem destructive validation passed on disk $DiskNumber."
Write-Host "Report: $ReportPath"

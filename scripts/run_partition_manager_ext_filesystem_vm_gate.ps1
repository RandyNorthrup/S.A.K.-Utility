<#
.SYNOPSIS
    Runs destructive ext filesystem write proof inside the Partition Manager VM.

.DESCRIPTION
    Intended to run elevated inside SAK-PM-Lab-Win11. Mutates only one small
    disposable VirtualBox data disk, verifies bundled e2fsprogs hashes against
    tools\filesystem\manifest.json, formats an ext4 filesystem on a raw
    partition target, runs the repair command path, grows the partition, runs
    resize2fs, shrinks the filesystem before shrinking the partition, rechecks
    the filesystem, and emits JSON evidence.
#>

[CmdletBinding()]
param(
    [string]$ProjectRoot = "\\vboxsvr\sakrepo",
    [string]$EvidenceRoot = "\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\external-evidence\external.ext-filesystem-write",
    [string]$ReportPath = "",
    [int]$DiskNumber = 1,
    [string]$FileSystem = "ext4",
    [uint64]$InitialSizeBytes = 512MB,
    [uint64]$GrowByBytes = 256MB,
    [uint64]$ShrinkByBytes = 128MB,
    [switch]$NoCleanup,
    [switch]$Force,
    [switch]$AllowNonVirtualBoxGuest
)

$ErrorActionPreference = "Stop"

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-IsVirtualBoxGuest {
    try {
        $system = Get-CimInstance -ClassName Win32_ComputerSystem -ErrorAction Stop
        $bios = Get-CimInstance -ClassName Win32_BIOS -ErrorAction Stop
        $identityText = @(
            [string]$system.Manufacturer,
            [string]$system.Model,
            [string]$bios.Manufacturer,
            [string]$bios.SMBIOSBIOSVersion,
            [string]$bios.SerialNumber
        ) -join " "
        return ($identityText -match "VirtualBox|innotek")
    }
    catch {
        return $false
    }
}

function Assert-VirtualBoxLabGuest {
    if (-not (Test-IsVirtualBoxGuest)) {
        throw "Refusing destructive ext filesystem VM gate outside a VirtualBox guest. Run the VM launcher inside SAK-PM-Lab-Win11, or pass -AllowNonVirtualBoxGuest only for a disposable non-VirtualBox lab VM."
    }
}

function ConvertTo-PlainText {
    param([object[]]$Value)
    return (($Value | ForEach-Object {
        if ($null -eq $_) { "" } else { $_.ToString() }
    }) -join "`n").Trim()
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
    $relativePath = [string]$tool.relative_path
    $path = Join-Path (Join-Path $ProjectRoot "tools\filesystem") $relativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        $path = Join-Path $ProjectRoot ("build\Release\tools\filesystem\" + $relativePath)
    }
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Approved tool not found: $relativePath"
    }
    $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne ([string]$tool.binary_sha256).ToLowerInvariant()) {
        throw "Approved tool hash mismatch for $ToolId."
    }
    return [pscustomobject]@{
        id = $tool.id
        path = $path
        expected_sha256 = ([string]$tool.binary_sha256).ToLowerInvariant()
        actual_sha256 = $actualHash
    }
}

function Get-DiskSnapshot {
    param([Parameter(Mandatory = $true)] [int]$Number)

    $disk = Get-Disk -Number $Number -ErrorAction Stop
    $partitions = @()
    foreach ($partition in @(Get-Partition -DiskNumber $Number -ErrorAction SilentlyContinue | Sort-Object Offset)) {
        $partitions += [pscustomobject]@{
            partition_number = [int]$partition.PartitionNumber
            drive_letter = if ($partition.DriveLetter) { [string]$partition.DriveLetter } else { "" }
            type = [string]$partition.Type
            offset_bytes = [uint64]$partition.Offset
            size_bytes = [uint64]$partition.Size
        }
    }
    return [pscustomobject]@{
        disk_number = [int]$disk.Number
        friendly_name = [string]$disk.FriendlyName
        serial_number = [string]$disk.SerialNumber
        bus_type = [string]$disk.BusType
        partition_style = [string]$disk.PartitionStyle
        size_bytes = [uint64]$disk.Size
        is_boot = [bool]$disk.IsBoot
        is_system = [bool]$disk.IsSystem
        partitions = $partitions
    }
}

function Assert-DisposableDisk {
    param([Parameter(Mandatory = $true)] [int]$Number)

    if ($Number -le 0) {
        throw "Refusing disk ${Number}: disk 0 and negative disk numbers are not disposable targets."
    }
    $disk = Get-Disk -Number $Number -ErrorAction Stop
    if ($disk.IsBoot -or $disk.IsSystem) {
        throw "Refusing disk ${Number}: boot/system disk."
    }
    if ($disk.Size -gt 12GB) {
        throw "Refusing disk ${Number}: expected small disposable disk, got $($disk.Size) bytes."
    }
    if ($disk.FriendlyName -notmatch "VBOX|Virtual|NVMe|HARDDISK") {
        throw "Refusing disk ${Number}: not recognized as disposable VirtualBox media."
    }
    if ($disk.IsOffline) {
        Set-Disk -Number $Number -IsOffline $false
    }
    if ($disk.IsReadOnly) {
        Set-Disk -Number $Number -IsReadOnly $false
    }
}

function Clear-DisposableDisk {
    param([Parameter(Mandatory = $true)] [int]$Number)

    Assert-DisposableDisk -Number $Number
    $disk = Get-Disk -Number $Number -ErrorAction Stop
    $partitions = @(Get-Partition -DiskNumber $Number -ErrorAction SilentlyContinue)
    if ($disk.PartitionStyle -eq "RAW" -and $partitions.Count -eq 0) {
        return "Disk $Number already RAW."
    }
    Clear-Disk -Number $Number -RemoveData -RemoveOEM -Confirm:$false -ErrorAction Stop
    return "Disk $Number cleared to RAW."
}

$Script:Commands = [System.Collections.Generic.List[object]]::new()
$started = (Get-Date).ToUniversalTime().ToString("o")
if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = Join-Path $EvidenceRoot "report.json"
}
$runRoot = Join-Path $EvidenceRoot ("run-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
New-Item -ItemType Directory -Path (Split-Path -Parent $ReportPath) -Force | Out-Null

$cleanup = ""
$rawTarget = ""
$before = $null
$after = $null
$toolHashes = @()
$status = "Failed"
$errorText = ""
$transcriptStarted = $false
try {
    Start-Transcript -Path (Join-Path $runRoot "ext-filesystem-vm-gate.log") -Force | Out-Null
    $transcriptStarted = $true
}
catch {
    $transcriptStarted = $false
}

try {
    if (-not (Test-IsAdmin)) {
        throw "Run this script from elevated PowerShell inside the VM."
    }
    if (-not $Force) {
        throw "Pass -Force after confirming DiskNumber is a disposable VM data disk."
    }
    if (-not $AllowNonVirtualBoxGuest) {
        Assert-VirtualBoxLabGuest
    }
    if (@("ext2", "ext3", "ext4") -notcontains $FileSystem.ToLowerInvariant()) {
        throw "FileSystem must be ext2, ext3, or ext4."
    }

    $manifestPath = Join-Path $ProjectRoot "tools\filesystem\manifest.json"
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $mke2fs = Resolve-ApprovedTool -Manifest $manifest -ToolId "mke2fs" -Operation "format" -FileSystem $FileSystem
    $e2fsck = Resolve-ApprovedTool -Manifest $manifest -ToolId "e2fsck" -Operation "repair" -FileSystem $FileSystem
    $resize2fs = Resolve-ApprovedTool -Manifest $manifest -ToolId "resize2fs" -Operation "resize" -FileSystem $FileSystem
    $toolHashes = @($mke2fs, $e2fsck, $resize2fs)

    Assert-DisposableDisk -Number $DiskNumber
    $before = Get-DiskSnapshot -Number $DiskNumber
    Clear-DisposableDisk -Number $DiskNumber | Out-Null
    Initialize-Disk -Number $DiskNumber -PartitionStyle GPT
    $partition = New-Partition -DiskNumber $DiskNumber -Size $InitialSizeBytes
    $partition = Get-Partition -DiskNumber $DiskNumber -PartitionNumber $partition.PartitionNumber -ErrorAction Stop
    $rawTarget = "\\?\GLOBALROOT\Device\Harddisk$DiskNumber\Partition$($partition.PartitionNumber)"

    Invoke-NativeCommand -Name "mke2fs-format-$FileSystem" -FilePath $mke2fs.path -Arguments @("-q", "-t", $FileSystem, "-F", "-L", "SAKEXTVM", $rawTarget) | Out-Null
    Invoke-NativeCommand -Name "e2fsck-repair-clean-$FileSystem" -FilePath $e2fsck.path -Arguments @("-p", "-f", $rawTarget) -AcceptedExitCodes @(0, 1) | Out-Null
    Invoke-NativeCommand -Name "e2fsck-readonly-after-format-$FileSystem" -FilePath $e2fsck.path -Arguments @("-n", "-f", $rawTarget) | Out-Null

    $grownSize = [uint64]$partition.Size + [uint64]$GrowByBytes
    Resize-Partition -DiskNumber $DiskNumber -PartitionNumber $partition.PartitionNumber -Size $grownSize
    Update-HostStorageCache -ErrorAction SilentlyContinue
    Invoke-NativeCommand -Name "resize2fs-grow-$FileSystem" -FilePath $resize2fs.path -Arguments @($rawTarget) | Out-Null
    Invoke-NativeCommand -Name "e2fsck-readonly-after-grow-$FileSystem" -FilePath $e2fsck.path -Arguments @("-n", "-f", $rawTarget) | Out-Null

    if ($ShrinkByBytes -le 0 -or $ShrinkByBytes -ge $grownSize) {
        throw "ShrinkByBytes must be smaller than the grown partition size."
    }
    $shrunkSize = [uint64]($grownSize - [uint64]$ShrinkByBytes)
    if ($shrunkSize -le $InitialSizeBytes) {
        throw "ShrinkByBytes must leave the shrunk partition larger than the initial partition."
    }
    $shrunkKilobytes = [uint64][Math]::Floor([double]$shrunkSize / 1KB)
    Invoke-NativeCommand -Name "e2fsck-repair-before-shrink-$FileSystem" -FilePath $e2fsck.path -Arguments @("-p", "-f", $rawTarget) -AcceptedExitCodes @(0, 1) | Out-Null
    Invoke-NativeCommand -Name "resize2fs-shrink-$FileSystem" -FilePath $resize2fs.path -Arguments @("-p", $rawTarget, "$($shrunkKilobytes)K") | Out-Null
    Resize-Partition -DiskNumber $DiskNumber -PartitionNumber $partition.PartitionNumber -Size $shrunkSize
    Update-HostStorageCache -ErrorAction SilentlyContinue
    Invoke-NativeCommand -Name "e2fsck-readonly-after-shrink-$FileSystem" -FilePath $e2fsck.path -Arguments @("-n", "-f", $rawTarget) | Out-Null

    $after = Get-DiskSnapshot -Number $DiskNumber
    $cleanup = if ($NoCleanup) { "Cleanup skipped by -NoCleanup." } else { Clear-DisposableDisk -Number $DiskNumber }
    $status = "Passed"
    $errorText = ""
}
catch {
    $status = "Failed"
    $errorText = ConvertTo-PlainText -Value @($_)
    try {
        if (-not $NoCleanup) {
            $cleanup = Clear-DisposableDisk -Number $DiskNumber
        }
    }
    catch {
        $cleanup = "Cleanup failed: $(ConvertTo-PlainText -Value @($_))"
    }
}
finally {
    if ($transcriptStarted) {
        Stop-Transcript | Out-Null
    }
}

$report = [pscustomobject]@{
    schema_version = 1
    gate_id = "external.ext-filesystem-write"
    status = $status
    started_at_utc = $started
    finished_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    vm_id = $env:COMPUTERNAME
    user = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    virtualbox_guest = Test-IsVirtualBoxGuest
    allow_non_virtualbox_guest = [bool]$AllowNonVirtualBoxGuest
    disk_number = $DiskNumber
    file_system = $FileSystem
    raw_target = $rawTarget
    before_layout = $before
    after_layout = $after
    tool_hashes = $toolHashes
    commands = @($Script:Commands.ToArray())
    cleanup = $cleanup
    error = $errorText
}
$report | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $ReportPath -Encoding UTF8
$report | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $runRoot "report.json") -Encoding UTF8

if ($status -ne "Passed") {
    throw "Ext filesystem VM gate failed. Report: $ReportPath`n$errorText"
}

Write-Host "Ext filesystem VM gate passed: $FileSystem on disposable disk $DiskNumber"
Write-Host "Report: $ReportPath"

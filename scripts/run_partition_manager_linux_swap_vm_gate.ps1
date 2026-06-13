<#
.SYNOPSIS
    Runs destructive Linux swap raw-partition proof inside the Partition Manager VM.

.DESCRIPTION
    Intended to run elevated inside SAK-PM-Lab-Win11. Mutates only one small
    disposable VirtualBox data disk, creates a GPT partition, writes original
    SWAPSPACE2 v1 metadata to the raw partition target, verifies the header by
    rereading the raw target, verifies S.A.K. raw detection with
    partition_filesystem_probe_certifier.exe, clears the disk, and emits JSON
    evidence. This script uses no runtime Linux/WSL dependency.
#>

[CmdletBinding()]
param(
    [string]$ProjectRoot = "\\vboxsvr\sakrepo",
    [string]$EvidenceRoot = "\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\external-evidence\external.linux-swap-format-vm",
    [string]$ReportPath = "",
    [int]$DiskNumber = 1,
    [uint64]$InitialSizeBytes = 128MB,
    [ValidateSet(4096, 8192, 16384, 65536)] [int]$PageSizeBytes = 4096,
    [string]$Label = "SAKSWAPVM",
    [string]$CertifierPath = "",
    [switch]$NoCleanup,
    [switch]$Force,
    [switch]$AllowNonVirtualBoxGuest
)

$ErrorActionPreference = "Stop"

$LinuxSwapGptType = "{0657FD6D-A4AB-43C4-84E5-0933C84B4F4F}"

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
        throw "Refusing destructive Linux swap VM gate outside a VirtualBox guest. Run the VM launcher inside SAK-PM-Lab-Win11, or pass -AllowNonVirtualBoxGuest only for a disposable non-VirtualBox lab VM."
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

function Resolve-Certifier {
    if (-not [string]::IsNullOrWhiteSpace($CertifierPath)) {
        return (Resolve-Path -LiteralPath $CertifierPath).Path
    }
    $candidate = Join-Path $ProjectRoot "build\Release\partition_filesystem_probe_certifier.exe"
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        return (Resolve-Path -LiteralPath $candidate).Path
    }
    throw "partition_filesystem_probe_certifier.exe was not found under ProjectRoot build\Release."
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
            gpt_type = [string]$partition.GptType
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

function Write-UInt32Le {
    param(
        [Parameter(Mandatory = $true)] [byte[]]$Buffer,
        [Parameter(Mandatory = $true)] [int]$Offset,
        [Parameter(Mandatory = $true)] [uint32]$Value
    )

    $bytes = [BitConverter]::GetBytes($Value)
    [Array]::Copy($bytes, 0, $Buffer, $Offset, 4)
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

    if ($Bytes.Length -ne 16) {
        throw "UUID requires exactly 16 bytes."
    }
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

function Write-RawHeader {
    param(
        [Parameter(Mandatory = $true)] [string]$TargetPath,
        [Parameter(Mandatory = $true)] [byte[]]$Header
    )

    $stream = [System.IO.File]::Open($TargetPath,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::ReadWrite,
        [System.IO.FileShare]::ReadWrite)
    try {
        [void]$stream.Seek(0, [System.IO.SeekOrigin]::Begin)
        $stream.Write($Header, 0, $Header.Length)
        $stream.Flush()
    }
    finally {
        $stream.Dispose()
    }
}

function Read-RawHeader {
    param(
        [Parameter(Mandatory = $true)] [string]$TargetPath,
        [Parameter(Mandatory = $true)] [int]$PageSize
    )

    $buffer = New-Object byte[] $PageSize
    $stream = [System.IO.File]::Open($TargetPath,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite)
    try {
        $read = $stream.Read($buffer, 0, $buffer.Length)
        if ($read -lt $PageSize) {
            throw "Linux swap header verify read was short."
        }
    }
    finally {
        $stream.Dispose()
    }
    return $buffer
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
$header = $null
$verification = $null
$probe = $null
$probePath = Join-Path $runRoot "linux-swap-raw-probe-report.json"
$status = "Failed"
$errorText = ""
$transcriptStarted = $false
try {
    Start-Transcript -Path (Join-Path $runRoot "linux-swap-vm-gate.log") -Force | Out-Null
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

    $certifier = Resolve-Certifier
    Assert-DisposableDisk -Number $DiskNumber
    $before = Get-DiskSnapshot -Number $DiskNumber
    Clear-DisposableDisk -Number $DiskNumber | Out-Null
    Initialize-Disk -Number $DiskNumber -PartitionStyle GPT
    $partition = New-Partition -DiskNumber $DiskNumber -Size $InitialSizeBytes
    Set-Partition -DiskNumber $DiskNumber -PartitionNumber $partition.PartitionNumber -GptType $LinuxSwapGptType
    $partition = Get-Partition -DiskNumber $DiskNumber -PartitionNumber $partition.PartitionNumber -ErrorAction Stop
    $rawTarget = "\\?\GLOBALROOT\Device\Harddisk$DiskNumber\Partition$($partition.PartitionNumber)"

    $header = New-LinuxSwapHeader -PartitionSizeBytes ([uint64]$partition.Size) -PageSize $PageSizeBytes -VolumeLabel $Label
    Write-RawHeader -TargetPath $rawTarget -Header $header.bytes
    $verification = Test-LinuxSwapHeader -Header (Read-RawHeader -TargetPath $rawTarget -PageSize $PageSizeBytes) -Expected $header

    Invoke-NativeCommand -Name "sak-probe-certifier-linux-swap-raw" `
        -FilePath $certifier `
        -Arguments @(
            "--input", $rawTarget,
            "--output", $probePath,
            "--expect", "Linux swap",
            "--input-size-bytes", ([string][uint64]$partition.Size)
        ) | Out-Null
    $probe = Get-Content -LiteralPath $probePath -Raw | ConvertFrom-Json
    if ($probe.status -ne "Passed") {
        throw "S.A.K. raw Linux swap probe failed."
    }

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
    gate_id = "external.linux-swap-format-vm"
    status = $status
    started_at_utc = $started
    finished_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    vm_id = $env:COMPUTERNAME
    user = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    virtualbox_guest = Test-IsVirtualBoxGuest
    allow_non_virtualbox_guest = [bool]$AllowNonVirtualBoxGuest
    disk_number = $DiskNumber
    partition_gpt_type = $LinuxSwapGptType
    raw_target = $rawTarget
    before_layout = $before
    after_layout = $after
    header = if ($header) {
        [pscustomobject]@{
            page_size_bytes = $header.page_size_bytes
            page_count = $header.page_count
            last_page = $header.last_page
            label = $header.label
            uuid = $header.uuid
        }
    } else { $null }
    verification = $verification
    probe_report_path = $probePath
    probe = $probe
    commands = @($Script:Commands.ToArray())
    cleanup = $cleanup
    error = $errorText
}
$report | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $ReportPath -Encoding UTF8
$report | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $runRoot "report.json") -Encoding UTF8

if ($status -ne "Passed") {
    throw "Linux swap VM gate failed. Report: $ReportPath`n$errorText"
}

Write-Host "Linux swap VM gate passed on disposable disk $DiskNumber"
Write-Host "Report: $ReportPath"

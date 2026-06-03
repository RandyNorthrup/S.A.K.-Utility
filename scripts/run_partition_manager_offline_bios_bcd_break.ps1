<#
.SYNOPSIS
    Breaks BIOS/MBR boot files on an attached disposable offline Windows disk.
#>

[CmdletBinding()]
param(
    [int]$TargetDiskNumber = -1,
    [string]$OutputRoot = "\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\offline-bios-break",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$script:TranscriptStarted = $false
$script:RunRoot = ""
$script:Actions = @()

trap {
    if (-not [string]::IsNullOrWhiteSpace($script:RunRoot)) {
        [pscustomobject]@{
            tool = "partition-manager-offline-bios-bcd-break"
            status = "Failed"
            created_utc = (Get-Date).ToUniversalTime().ToString("o")
            error = $_.Exception.Message
            position = $_.InvocationInfo.PositionMessage
            stack = $_.ScriptStackTrace
            actions = $script:Actions
        } | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $script:RunRoot "offline-bios-break-error.json") -Encoding UTF8
    }
    if ($script:TranscriptStarted) {
        Stop-Transcript | Out-Null
        $script:TranscriptStarted = $false
    }
    break
}

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Add-Action {
    param([string]$Name, [string]$Detail)
    $script:Actions += [pscustomobject]@{
        timestamp = (Get-Date).ToString("o")
        name = $Name
        detail = $Detail
    }
}

function Get-AvailableDriveLetter {
    $used = @(Get-Volume -ErrorAction SilentlyContinue | Where-Object DriveLetter | ForEach-Object { [string]$_.DriveLetter })
    foreach ($letter in @("W", "T", "R", "S", "U", "V", "X", "Y", "Z", "P", "Q", "L", "M", "N")) {
        if ($used -notcontains $letter) {
            return $letter
        }
    }
    throw "No available drive letter."
}

function Invoke-NativeCommand {
    param([string]$Name, [string]$FilePath, [string[]]$Arguments = @())
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $FilePath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    Add-Action -Name $Name -Detail ((($output | ForEach-Object { $_.ToString() }) -join "`n").Trim())
    if ($exitCode -ne 0) {
        throw "$Name failed with exit code $exitCode."
    }
}

function Invoke-DiskPartScript {
    param([string]$Name, [string[]]$Lines)
    $scriptPath = Join-Path $script:RunRoot ("diskpart-" + [guid]::NewGuid().ToString("N") + ".txt")
    Set-Content -LiteralPath $scriptPath -Value ($Lines -join [Environment]::NewLine) -Encoding ASCII
    Invoke-NativeCommand -Name $Name -FilePath "diskpart.exe" -Arguments @("/s", $scriptPath)
}

function Get-DiskSnapshot {
    param([int]$DiskNumber)
    $disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
    $partitions = @()
    foreach ($partition in @(Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue | Sort-Object Offset)) {
        $volume = $null
        if ($partition.DriveLetter) {
            $volume = Get-Volume -DriveLetter $partition.DriveLetter -ErrorAction SilentlyContinue
        }
        $partitions += [pscustomobject]@{
            partition_number = [int]$partition.PartitionNumber
            drive_letter = if ($partition.DriveLetter) { [string]$partition.DriveLetter } else { "" }
            type = [string]$partition.Type
            offset_bytes = [uint64]$partition.Offset
            size_bytes = [uint64]$partition.Size
            file_system = if ($null -ne $volume) { [string]$volume.FileSystem } else { "" }
            label = if ($null -ne $volume) { [string]$volume.FileSystemLabel } else { "" }
        }
    }
    return [pscustomobject]@{
        disk_number = [int]$disk.Number
        friendly_name = [string]$disk.FriendlyName
        serial_number = [string]$disk.SerialNumber
        unique_id = [string]$disk.UniqueId
        bus_type = [string]$disk.BusType
        partition_style = [string]$disk.PartitionStyle
        size_bytes = [uint64]$disk.Size
        is_boot = [bool]$disk.IsBoot
        is_system = [bool]$disk.IsSystem
        is_offline = [bool]$disk.IsOffline
        is_read_only = [bool]$disk.IsReadOnly
        partitions = $partitions
    }
}

if (-not (Test-IsAdmin)) {
    throw "Run elevated inside the VM."
}
if (-not $Force) {
    throw "Pass -Force after confirming the attached disk is the disposable BIOS target."
}

$script:RunRoot = Join-Path $OutputRoot (Get-Date -Format "yyyyMMdd-HHmmss")
New-Item -ItemType Directory -Path $script:RunRoot -Force | Out-Null
Start-Transcript -Path (Join-Path $script:RunRoot "offline-bios-break-transcript.log") -Force | Out-Null
$script:TranscriptStarted = $true

$candidateDisks = @(Get-Disk | Where-Object {
    -not $_.IsBoot -and -not $_.IsSystem -and
    $_.PartitionStyle -eq "MBR" -and
    $_.Size -gt 60GB -and $_.Size -lt 120GB
} | Sort-Object Number)
if ($TargetDiskNumber -lt 0) {
    if ($candidateDisks.Count -ne 1) {
        throw "Expected exactly one non-system MBR 60-120GB target disk, found $($candidateDisks.Count)."
    }
    $TargetDiskNumber = [int]$candidateDisks[0].Number
}

$before = Get-DiskSnapshot -DiskNumber $TargetDiskNumber
if ($before.is_boot -or $before.is_system -or $before.partition_style -ne "MBR") {
    throw "Refusing non-disposable or non-MBR disk $TargetDiskNumber."
}

Invoke-DiskPartScript -Name "prepare-target-disk" -Lines @(
    "select disk $TargetDiskNumber",
    "attributes disk clear readonly",
    "online disk noerr",
    "select partition 1",
    "active",
    "detail disk",
    "list partition",
    "list volume"
)
Update-HostStorageCache

$windowsPartition = Get-Partition -DiskNumber $TargetDiskNumber |
    Where-Object { [string]$_.Type -match "IFS|Basic|Microsoft Basic Data" -and $_.Size -gt 20GB } |
    Sort-Object Size -Descending |
    Select-Object -First 1
if ($null -eq $windowsPartition) {
    throw "No Windows-sized partition found on disk $TargetDiskNumber."
}

$windowsLetter = if ($windowsPartition.DriveLetter) { [string]$windowsPartition.DriveLetter } else { Get-AvailableDriveLetter }
if (-not $windowsPartition.DriveLetter) {
    try {
        Add-PartitionAccessPath -DiskNumber $TargetDiskNumber -PartitionNumber $windowsPartition.PartitionNumber -AccessPath "${windowsLetter}:"
    }
    catch {
        Invoke-DiskPartScript -Name "assign-windows-letter" -Lines @(
            "select disk $TargetDiskNumber",
            "select partition $($windowsPartition.PartitionNumber)",
            "assign letter=$windowsLetter"
        )
    }
}

$targetRoot = "${windowsLetter}:"
if (-not (Test-Path -LiteralPath "$targetRoot\Windows" -PathType Container)) {
    throw "Windows directory not found at $targetRoot\Windows."
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$bcdPath = "$targetRoot\Boot\BCD"
$bcdBackupPath = "$targetRoot\Boot\BCD.sak-broken-$stamp"
if (-not (Test-Path -LiteralPath $bcdPath -PathType Leaf)) {
    throw "BCD store not found at $bcdPath."
}
Copy-Item -LiteralPath $bcdPath -Destination $bcdBackupPath -Force
Remove-Item -LiteralPath $bcdPath -Force
Add-Action -Name "remove-bios-bcd" -Detail "Backed up $bcdPath to $bcdBackupPath, then removed active BCD."

$after = Get-DiskSnapshot -DiskNumber $TargetDiskNumber
$actionsPath = Join-Path $script:RunRoot "offline-bios-break-actions.json"
$script:Actions | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $actionsPath -Encoding UTF8

$report = [ordered]@{
    tool = "partition-manager-offline-bios-bcd-break"
    schema_version = 1
    created_utc = (Get-Date).ToUniversalTime().ToString("o")
    vm_id = $env:COMPUTERNAME
    target_disk_number = $TargetDiskNumber
    target_drive = $targetRoot
    before_layout = $before
    after_layout = $after
    bcd_path = $bcdPath
    bcd_backup_path = $bcdBackupPath
    bcd_removed = -not (Test-Path -LiteralPath $bcdPath -PathType Leaf)
    bootmgr_exists = (Test-Path -LiteralPath "$targetRoot\bootmgr" -PathType Leaf)
    actions_path = $actionsPath
}
$report | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath (Join-Path $script:RunRoot "offline-bios-break-report.json") -Encoding UTF8
if ($script:TranscriptStarted) {
    Stop-Transcript | Out-Null
    $script:TranscriptStarted = $false
}

Write-Host "Offline BIOS break complete: $(Join-Path $script:RunRoot "offline-bios-break-report.json")"

<#
.SYNOPSIS
    Runs mbr2gpt validate/convert on the disposable BIOS boot VM system disk.
#>

[CmdletBinding()]
param(
    [int]$DiskNumber = 0,
    [string]$OutputRoot = "\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\system-mbr2gpt",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$script:TranscriptStarted = $false
$script:RunRoot = ""
$script:Commands = @()

trap {
    if (-not [string]::IsNullOrWhiteSpace($script:RunRoot)) {
        [pscustomobject]@{
            tool = "partition-manager-system-mbr2gpt-external-gate"
            status = "Failed"
            created_utc = (Get-Date).ToUniversalTime().ToString("o")
            error = $_.Exception.Message
            position = $_.InvocationInfo.PositionMessage
            stack = $_.ScriptStackTrace
            commands = $script:Commands
        } | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $script:RunRoot "system-mbr2gpt-error.json") -Encoding UTF8
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

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string]$FilePath,
        [string[]]$Arguments = @()
    )

    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $started = Get-Date
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
        exit_code = $exitCode
        duration_seconds = [Math]::Round(((Get-Date) - $started).TotalSeconds, 3)
        output = (($output | ForEach-Object { $_.ToString() }) -join "`n").Trim()
    }
    $script:Commands += $record
    if ($exitCode -ne 0) {
        throw "$Name failed with exit code $exitCode. $($record.output)"
    }
    return $record
}

function Get-DiskSnapshot {
    param([int]$Number)
    $disk = Get-Disk -Number $Number -ErrorAction Stop
    $partitions = @()
    foreach ($partition in @(Get-Partition -DiskNumber $Number -ErrorAction SilentlyContinue | Sort-Object Offset)) {
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
    throw "Pass -Force after confirming this is the disposable BIOS boot VM."
}

$script:RunRoot = Join-Path $OutputRoot (Get-Date -Format "yyyyMMdd-HHmmss")
New-Item -ItemType Directory -Path $script:RunRoot -Force | Out-Null
Start-Transcript -Path (Join-Path $script:RunRoot "system-mbr2gpt-transcript.log") -Force | Out-Null
$script:TranscriptStarted = $true

$before = Get-DiskSnapshot -Number $DiskNumber
if (-not $before.is_boot -or -not $before.is_system -or $before.partition_style -ne "MBR") {
    throw "Refusing disk $DiskNumber. Expected disposable boot/system MBR disk."
}
if ($before.size_bytes -lt 60GB -or $before.size_bytes -gt 120GB) {
    throw "Refusing disk $DiskNumber size $($before.size_bytes); expected disposable 60-120GB VM OS disk."
}

$validate = Invoke-NativeCommand -Name "mbr2gpt-validate" -FilePath "mbr2gpt.exe" -Arguments @("/validate", "/disk:$DiskNumber", "/allowFullOS")
$convert = Invoke-NativeCommand -Name "mbr2gpt-convert" -FilePath "mbr2gpt.exe" -Arguments @("/convert", "/disk:$DiskNumber", "/allowFullOS")
Update-HostStorageCache
Start-Sleep -Seconds 2
$after = Get-DiskSnapshot -Number $DiskNumber

$commandsPath = Join-Path $script:RunRoot "system-mbr2gpt-commands.json"
$script:Commands | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $commandsPath -Encoding UTF8

$report = [ordered]@{
    tool = "partition-manager-system-mbr2gpt-external-gate"
    schema_version = 1
    created_utc = (Get-Date).ToUniversalTime().ToString("o")
    vm_id = $env:COMPUTERNAME
    disk_number = $DiskNumber
    before_partition_style = $before.partition_style
    after_partition_style = $after.partition_style
    before_layout = $before
    after_layout = $after
    mbr2gpt_validate_exit_code = $validate.exit_code
    mbr2gpt_convert_exit_code = $convert.exit_code
    commands_path = $commandsPath
}
$report | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath (Join-Path $script:RunRoot "system-mbr2gpt-report.json") -Encoding UTF8
if ($script:TranscriptStarted) {
    Stop-Transcript | Out-Null
    $script:TranscriptStarted = $false
}

Write-Host "System MBR2GPT complete: $(Join-Path $script:RunRoot "system-mbr2gpt-report.json")"

<#
.SYNOPSIS
    Runs live File Management APFS/HFS certification against expendable media.

.DESCRIPTION
    Selects a non-boot GPT disk with APFS and HFS+ partitions, then runs the
    File Management live certifier against raw partition aliases. The certifier
    writes target media only when -Destructive is set.
#>

[CmdletBinding()]
param(
    [int]$DiskNumber = -1,
    [int]$ApfsPartitionNumber = -1,
    [int]$HfsPartitionNumber = -1,
    [string]$CertifierPath = "",
    [string]$OutputRoot = "artifacts\file-management-live-certification",
    [string]$ExpectedSerialNumber = "",
    [string]$ExpectedFriendlyNamePattern = "",
    [int]$MaxDepth = 8,
    [int]$MaxDirectories = 512,
    [int]$MaxEntries = 1024,
    [uint64]$ReadMaxBytes = 1048576,
    [int]$WorkerTimeoutMs = 180000,
    [switch]$Destructive,
    [switch]$SkipApfs,
    [switch]$SkipHfs,
    [switch]$AllowBootOrSystemDisk,
    [switch]$NoWait
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$AppleHfsGptType = "{48465300-0000-11aa-aa11-00306543ecac}"
$AppleApfsGptType = "{7c3457ef-0000-11aa-aa11-00306543ecac}"

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]$identity
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Add-Arg {
    param([System.Collections.Generic.List[string]]$ArgumentValues, [string]$Name, [string]$Value)
    if (-not [string]::IsNullOrWhiteSpace($Value)) {
        $ArgumentValues.Add($Name)
        $ArgumentValues.Add($Value)
    }
}

function ConvertTo-QuotedProcessArgument {
    param([string]$Value)
    if ($null -eq $Value) {
        return '""'
    }
    $escaped = $Value -replace '`', '``' -replace '"', '`"'
    return '"' + $escaped + '"'
}

function ConvertTo-ProcessArgumentString {
    param([string[]]$ArgumentValues)
    return (($ArgumentValues | ForEach-Object { ConvertTo-QuotedProcessArgument -Value $_ }) -join " ")
}

function Resolve-CertifierPath {
    param([string]$Path)

    if (-not [string]::IsNullOrWhiteSpace($Path)) {
        return (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    }
    $candidate = Join-Path $ProjectRoot "build\Release\file_management_live_certifier.exe"
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        throw "file_management_live_certifier.exe was not found. Build target file_management_live_certifier first."
    }
    return (Resolve-Path -LiteralPath $candidate).Path
}

function ConvertTo-AbsolutePath {
    param([string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return Join-Path $ProjectRoot $Path
}

function Assert-SafeDisk {
    param([Parameter(Mandatory = $true)] [object]$Disk)
    if ($AllowBootOrSystemDisk) {
        return
    }
    if ($Disk.IsBoot -or $Disk.IsSystem) {
        throw "Refusing to certify boot/system disk $($Disk.Number)."
    }
}

function Select-TargetDisk {
    $allDisks = @(Get-Disk | Sort-Object Number)
    if ($DiskNumber -ge 0) {
        $disk = $allDisks | Where-Object { $_.Number -eq $DiskNumber } | Select-Object -First 1
        if (-not $disk) {
            throw "Disk $DiskNumber was not found."
        }
        Assert-SafeDisk -Disk $disk
        return $disk
    }

    $candidates = @($allDisks | Where-Object {
            $parts = @(Get-Partition -DiskNumber $_.Number -ErrorAction SilentlyContinue)
            -not $_.IsBoot -and
            -not $_.IsSystem -and
            $_.PartitionStyle -eq "GPT" -and
            @($parts | Where-Object { "$($_.GptType)".ToLowerInvariant() -eq $AppleApfsGptType }).Count -gt 0 -and
            @($parts | Where-Object { "$($_.GptType)".ToLowerInvariant() -eq $AppleHfsGptType }).Count -gt 0
        })
    if ($candidates.Count -ne 1) {
        throw "Expected exactly one non-boot GPT disk with APFS and HFS+ partitions; found $($candidates.Count). Pass -DiskNumber."
    }
    return $candidates[0]
}

function Assert-ExpectedDiskIdentity {
    param([Parameter(Mandatory = $true)] [object]$Disk)

    if (-not [string]::IsNullOrWhiteSpace($ExpectedSerialNumber) -and
        "$($Disk.SerialNumber)" -ne $ExpectedSerialNumber) {
        throw "Disk $($Disk.Number) serial mismatch. Expected '$ExpectedSerialNumber', got '$($Disk.SerialNumber)'."
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedFriendlyNamePattern) -and
        "$($Disk.FriendlyName)" -notlike $ExpectedFriendlyNamePattern) {
        throw "Disk $($Disk.Number) name mismatch. Expected pattern '$ExpectedFriendlyNamePattern', got '$($Disk.FriendlyName)'."
    }
}

if (-not (Test-IsAdmin)) {
    $argsList = [System.Collections.Generic.List[string]]::new()
    $argsList.Add("-NoProfile")
    $argsList.Add("-ExecutionPolicy")
    $argsList.Add("Bypass")
    $argsList.Add("-File")
    $argsList.Add($PSCommandPath)
    if ($DiskNumber -ge 0) {
        $argsList.Add("-DiskNumber")
        $argsList.Add([string]$DiskNumber)
    }
    if ($ApfsPartitionNumber -ge 0) {
        $argsList.Add("-ApfsPartitionNumber")
        $argsList.Add([string]$ApfsPartitionNumber)
    }
    if ($HfsPartitionNumber -ge 0) {
        $argsList.Add("-HfsPartitionNumber")
        $argsList.Add([string]$HfsPartitionNumber)
    }
    Add-Arg -ArgumentValues $argsList -Name "-CertifierPath" -Value $CertifierPath
    Add-Arg -ArgumentValues $argsList -Name "-OutputRoot" -Value (ConvertTo-AbsolutePath -Path $OutputRoot)
    Add-Arg -ArgumentValues $argsList -Name "-ExpectedSerialNumber" -Value $ExpectedSerialNumber
    Add-Arg -ArgumentValues $argsList -Name "-ExpectedFriendlyNamePattern" -Value $ExpectedFriendlyNamePattern
    $argsList.Add("-MaxDepth")
    $argsList.Add([string]$MaxDepth)
    $argsList.Add("-MaxDirectories")
    $argsList.Add([string]$MaxDirectories)
    $argsList.Add("-MaxEntries")
    $argsList.Add([string]$MaxEntries)
    $argsList.Add("-ReadMaxBytes")
    $argsList.Add([string]$ReadMaxBytes)
    $argsList.Add("-WorkerTimeoutMs")
    $argsList.Add([string]$WorkerTimeoutMs)
    if ($Destructive) {
        $argsList.Add("-Destructive")
    }
    if ($SkipApfs) {
        $argsList.Add("-SkipApfs")
    }
    if ($SkipHfs) {
        $argsList.Add("-SkipHfs")
    }
    if ($AllowBootOrSystemDisk) {
        $argsList.Add("-AllowBootOrSystemDisk")
    }

    $startInfo = @{
        FilePath = "powershell.exe"
        ArgumentList = (ConvertTo-ProcessArgumentString -ArgumentValues $argsList.ToArray())
        Verb = "RunAs"
    }
    if ($NoWait) {
        Start-Process @startInfo
        exit 0
    }
    $startInfo["PassThru"] = $true
    $process = Start-Process @startInfo
    $process.WaitForExit()
    exit $process.ExitCode
}

$certifier = Resolve-CertifierPath -Path $CertifierPath
$output = ConvertTo-AbsolutePath -Path $OutputRoot
New-Item -ItemType Directory -Force -Path $output | Out-Null

$disk = Select-TargetDisk
Assert-ExpectedDiskIdentity -Disk $disk
if ($SkipApfs -and $SkipHfs) {
    throw "At least one target family must be enabled."
}

$parts = @(Get-Partition -DiskNumber $disk.Number | Sort-Object PartitionNumber)
$apfs = $null
$hfs = $null
if (-not $SkipApfs) {
    if ($ApfsPartitionNumber -ge 0) {
        $apfs = $parts | Where-Object {
            $_.PartitionNumber -eq $ApfsPartitionNumber -and
            "$($_.GptType)".ToLowerInvariant() -eq $AppleApfsGptType
        } | Select-Object -First 1
    }
    else {
        $apfs = @($parts | Where-Object { "$($_.GptType)".ToLowerInvariant() -eq $AppleApfsGptType }) | Select-Object -First 1
    }
}
if (-not $SkipHfs) {
    if ($HfsPartitionNumber -ge 0) {
        $hfs = $parts | Where-Object {
            $_.PartitionNumber -eq $HfsPartitionNumber -and
            "$($_.GptType)".ToLowerInvariant() -eq $AppleHfsGptType
        } | Select-Object -First 1
    }
    else {
        $hfs = @($parts | Where-Object { "$($_.GptType)".ToLowerInvariant() -eq $AppleHfsGptType }) | Select-Object -First 1
    }
}
if (-not $SkipApfs -and -not $apfs) {
    throw "Disk $($disk.Number) has no APFS partition."
}
if (-not $SkipHfs -and -not $hfs) {
    throw "Disk $($disk.Number) has no HFS+ partition."
}
if ($Destructive -and $apfs) {
    $maxGeneratedSingleChunkBytes = [uint64]134217728
    if ([uint64]$apfs.Size -gt $maxGeneratedSingleChunkBytes) {
        throw "APFS destructive live certification currently requires a 64-128 MiB S.A.K.-generated one-spaceman-chunk APFS partition; selected partition is $($apfs.Size) bytes."
    }
}

$inventoryPath = Join-Path $output "target-inventory.json"
$inventory = [ordered]@{
    disk = [ordered]@{
        number = $disk.Number
        friendly_name = "$($disk.FriendlyName)"
        serial_number = "$($disk.SerialNumber)"
        bus_type = "$($disk.BusType)"
        size_bytes = "$($disk.Size)"
    }
}
if ($apfs) {
    $inventory.apfs_partition = [ordered]@{
        number = $apfs.PartitionNumber
        size_bytes = "$($apfs.Size)"
        offset_bytes = "$($apfs.Offset)"
        target = "\\?\GLOBALROOT\Device\Harddisk$($disk.Number)\Partition$($apfs.PartitionNumber)"
    }
}
if ($hfs) {
    $inventory.hfs_partition = [ordered]@{
        number = $hfs.PartitionNumber
        size_bytes = "$($hfs.Size)"
        offset_bytes = "$($hfs.Offset)"
        target = "\\?\GLOBALROOT\Device\Harddisk$($disk.Number)\Partition$($hfs.PartitionNumber)"
    }
}
$inventory | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $inventoryPath -Encoding UTF8

$reportPath = Join-Path $output "file-management-live-certification.json"
$certifierArgs = @(
    "--output", $reportPath,
    "--max-depth", [string]$MaxDepth,
    "--max-directories", [string]$MaxDirectories,
    "--max-entries", [string]$MaxEntries,
    "--read-max-bytes", [string]$ReadMaxBytes,
    "--worker-timeout-ms", [string]$WorkerTimeoutMs
)
if ($Destructive) {
    $certifierArgs += "--destructive"
}
if ($apfs) {
    $certifierArgs += @(
        "--target", "APFS=\\?\GLOBALROOT\Device\Harddisk$($disk.Number)\Partition$($apfs.PartitionNumber)",
        "--target-size", "APFS=$($apfs.Size)"
    )
}
if ($hfs) {
    $certifierArgs += @(
        "--target", "HFS+=\\?\GLOBALROOT\Device\Harddisk$($disk.Number)\Partition$($hfs.PartitionNumber)",
        "--target-size", "HFS+=$($hfs.Size)"
    )
}

$previousErrorActionPreference = $ErrorActionPreference
try {
    $ErrorActionPreference = "Continue"
    $certifierOutput = & $certifier @certifierArgs 2>&1
    $exitCode = $LASTEXITCODE
}
finally {
    $ErrorActionPreference = $previousErrorActionPreference
}
foreach ($line in $certifierOutput) {
    Write-Host "$line"
}
$status = if (Test-Path -LiteralPath $reportPath -PathType Leaf) {
    (Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json).status
}
else {
    "MissingReport"
}

Write-Host "File Management live certification status: $status"
Write-Host "Report: $reportPath"
Write-Host "Inventory: $inventoryPath"
exit $exitCode

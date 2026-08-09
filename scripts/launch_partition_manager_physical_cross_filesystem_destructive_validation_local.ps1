<#
.SYNOPSIS
    Launches the physical cross-filesystem destructive validation with proper UAC auth.

.DESCRIPTION
    Starts an elevated PowerShell process via Windows runas/UAC and waits for
    completion. No keyboard automation is used. The elevated child performs all
    destructive work and writes JSON evidence. The runner is pinned by hash, the
    resolved disk identity is shown before the UAC prompt, the elevated console
    is captured to a log, and success requires fresh evidence on disk.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [ValidateRange(0, 4095)] [int]$DiskNumber,
    [string]$ExpectedSerialNumber = "",
    [string]$ExpectedFriendlyNamePattern = "",
    [ValidateSet("ext2", "ext3", "ext4")] [string[]]$FileSystems = @("ext2", "ext3", "ext4"),
    [string]$EvidenceRoot = "artifacts\partition-manager-certification\vm-lab\external-evidence\external.cross-filesystem-physical-destructive",
    [switch]$AllowInternalDisk,
    [switch]$AllowLargeUnpinnedDisk,
    [switch]$NoCleanup,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$runner = Join-Path $ProjectRoot "scripts\run_partition_manager_physical_cross_filesystem_destructive_validation.ps1"
if (-not (Test-Path -LiteralPath $runner -PathType Leaf)) {
    throw "Runner not found: $runner"
}
if (-not $Force) {
    throw "Pass -Force after confirming DiskNumber is expendable physical media."
}
if ($FileSystems.Count -eq 0) {
    throw "FileSystems is empty. Pass at least one of ext2, ext3, ext4; an empty set would certify nothing."
}
if ($PSBoundParameters.ContainsKey("ExpectedSerialNumber") -and [string]::IsNullOrWhiteSpace($ExpectedSerialNumber)) {
    throw "ExpectedSerialNumber was supplied but is blank. Pass the real serial or omit the parameter."
}
if ($PSBoundParameters.ContainsKey("ExpectedFriendlyNamePattern") -and [string]::IsNullOrWhiteSpace($ExpectedFriendlyNamePattern)) {
    throw "ExpectedFriendlyNamePattern was supplied but is blank. Pass the real pattern or omit the parameter."
}
if ([string]::IsNullOrWhiteSpace($EvidenceRoot)) {
    throw "EvidenceRoot is empty. Pass a path inside the project root."
}

function Quote-PowerShellLiteral {
    param([Parameter(Mandatory = $true)] [AllowEmptyString()] [string]$Value)
    return "'" + ($Value -replace "'", "''") + "'"
}

function Resolve-ContainedEvidenceRoot {
    param(
        [Parameter(Mandatory = $true)] [string]$Root,
        [Parameter(Mandatory = $true)] [string]$Relative
    )
    $candidate = $Relative
    if (-not [System.IO.Path]::IsPathRooted($candidate)) {
        $candidate = Join-Path $Root $Relative
    }
    $full = [System.IO.Path]::GetFullPath($candidate)
    $prefix = [System.IO.Path]::GetFullPath($Root).TrimEnd([System.IO.Path]::DirectorySeparatorChar) +
    [System.IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "EvidenceRoot must stay inside the project root $Root but resolved to $full."
    }
    return $full
}

function Get-ElevatedLogTail {
    param([Parameter(Mandatory = $true)] [string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return " No elevated console log was written to $Path."
    }
    $tail = (Get-Content -LiteralPath $Path -Tail 40 -ErrorAction Stop) -join "`n"
    if ([string]::IsNullOrWhiteSpace($tail)) {
        return " Elevated console log $Path is empty."
    }
    return "`nElevated console output (tail):`n$tail"
}

function Assert-FreshEvidence {
    param(
        [Parameter(Mandatory = $true)] [string]$Root,
        [Parameter(Mandatory = $true)] [datetime]$SinceUtc
    )
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        throw "Elevated run reported success but the evidence root is missing: $Root"
    }
    $cutoff = $SinceUtc.AddMinutes(-2)
    $fresh = @(Get-ChildItem -LiteralPath $Root -Recurse -File -ErrorAction Stop |
            Where-Object { $_.LastWriteTimeUtc -ge $cutoff })
    if ($fresh.Count -eq 0) {
        throw "Elevated run reported success but wrote no evidence under $Root after $($SinceUtc.ToString('o'))."
    }
}

$evidenceFullPath = Resolve-ContainedEvidenceRoot -Root $ProjectRoot -Relative $EvidenceRoot

# Resolve the target before the UAC prompt so the operator confirms a real device,
# not just a disk number. The elevated runner re-checks these pins itself.
$disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
Write-Host "Target disk $DiskNumber : $($disk.FriendlyName) serial '$($disk.SerialNumber)' bus $($disk.BusType) size $($disk.Size) bytes style $($disk.PartitionStyle)."
if ($AllowInternalDisk) {
    Write-Host "WARNING: -AllowInternalDisk disables the internal-disk guard in the elevated runner."
}
if ($AllowLargeUnpinnedDisk) {
    Write-Host "WARNING: -AllowLargeUnpinnedDisk disables the large-unpinned-disk guard in the elevated runner."
}

$logRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("sak-cross-filesystem-launch-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $logRoot | Out-Null
$childLog = Join-Path $logRoot "elevated-console.log"

$fileSystemsLiteral = "@(" + (($FileSystems | ForEach-Object { Quote-PowerShellLiteral $_ }) -join ", ") + ")"
$runnerArgs = "-DiskNumber $DiskNumber -ProjectRoot $(Quote-PowerShellLiteral $ProjectRoot) -EvidenceRoot $(Quote-PowerShellLiteral $EvidenceRoot) -FileSystems $fileSystemsLiteral -Force"
if (-not [string]::IsNullOrWhiteSpace($ExpectedSerialNumber)) {
    $runnerArgs += " -ExpectedSerialNumber $(Quote-PowerShellLiteral $ExpectedSerialNumber)"
}
if (-not [string]::IsNullOrWhiteSpace($ExpectedFriendlyNamePattern)) {
    $runnerArgs += " -ExpectedFriendlyNamePattern $(Quote-PowerShellLiteral $ExpectedFriendlyNamePattern)"
}
if ($AllowInternalDisk) {
    $runnerArgs += " -AllowInternalDisk"
}
if ($AllowLargeUnpinnedDisk) {
    $runnerArgs += " -AllowLargeUnpinnedDisk"
}
if ($NoCleanup) {
    $runnerArgs += " -NoCleanup"
}

# Pin the runner bytes: an existence check here says nothing about what the elevated
# process opens a moment later, so the child re-hashes before it runs anything.
$runnerHash = (Get-FileHash -LiteralPath $runner -Algorithm SHA256).Hash
$command = @(
    '$ErrorActionPreference = "Stop"',
    "`$expectedHash = $(Quote-PowerShellLiteral $runnerHash)",
    "`$actualHash = (Get-FileHash -LiteralPath $(Quote-PowerShellLiteral $runner) -Algorithm SHA256).Hash",
    "if (`$actualHash -ne `$expectedHash) { throw ('Runner changed between launch and elevation: ' + $(Quote-PowerShellLiteral $runner)) }",
    "try {",
    "    & $(Quote-PowerShellLiteral $runner) $runnerArgs *>&1 | Tee-Object -FilePath $(Quote-PowerShellLiteral $childLog)",
    "}",
    "catch {",
    "    (`$_ | Out-String) | Add-Content -LiteralPath $(Quote-PowerShellLiteral $childLog)",
    "    throw",
    "}",
    "if (`$LASTEXITCODE) { exit `$LASTEXITCODE }"
) -join "`n"
$encodedCommand = [Convert]::ToBase64String([System.Text.Encoding]::Unicode.GetBytes($command))

$powershell = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
if (-not (Test-Path -LiteralPath $powershell -PathType Leaf)) {
    throw "Windows PowerShell was not found at its System32 path: $powershell"
}

Write-Host "Launching elevated destructive physical cross-filesystem validation for disk $DiskNumber."
Write-Host "Approve the Windows UAC prompt to continue."
$launchUtc = (Get-Date).ToUniversalTime()
$process = Start-Process -FilePath $powershell -Verb RunAs -Wait -PassThru -ArgumentList @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-EncodedCommand", $encodedCommand
)
Write-Host "Elevated process exited with code $($process.ExitCode)."
if ($process.ExitCode -ne 0) {
    throw "Elevated physical cross-filesystem validation exited with code $($process.ExitCode).$(Get-ElevatedLogTail -Path $childLog)"
}
Assert-FreshEvidence -Root $evidenceFullPath -SinceUtc $launchUtc
Write-Host "Elevated console log: $childLog"

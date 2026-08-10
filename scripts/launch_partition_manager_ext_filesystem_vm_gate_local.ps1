<#
.SYNOPSIS
    Stages and launches the ext filesystem VM write gate inside SAK-PM-Lab-Win11.

.DESCRIPTION
    Run from the non-elevated VM desktop with the shared repo mounted as
    \\vboxsvr\sakrepo. It copies the elevated runner locally, prompts UAC, runs
    the destructive ext write proof on a disposable VM data disk, and writes
    report.json evidence back to the shared repository.
#>

[CmdletBinding()]
param(
    [string]$SharedRoot = "\\vboxsvr\sakrepo",
    [string]$StageRoot = (Join-Path $env:PUBLIC ("sak-ext-filesystem-vm-gate-" + (Get-Date -Format "yyyyMMddHHmmss") + "-" + [guid]::NewGuid().ToString("N").Substring(0, 8))),
    [int]$DiskNumber = 1,
    [ValidateSet("ext2", "ext3", "ext4")] [string]$FileSystem = "ext4",
    [switch]$NoCleanup
)

$ErrorActionPreference = "Stop"

function Assert-QualifiedPath {
    param(
        [Parameter(Mandatory = $true)] [AllowEmptyString()] [string]$Path,
        [Parameter(Mandatory = $true)] [string]$Name
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "$Name must be a non-empty path."
    }
    if ($Path -match '["*?\[\]]') {
        throw "$Name contains characters that cannot be quoted or staged safely: $Path"
    }
    if ($Path -notmatch '^([A-Za-z]:[\\/]|\\\\[^\\])') {
        throw "$Name must be a fully qualified local or UNC path: $Path"
    }
}

function Assert-NoReparsePoint {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [string]$Name
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -eq [System.IO.FileAttributes]::ReparsePoint) {
        throw "$Name is a reparse point and cannot be trusted for elevated staging: $Path"
    }
}

function Get-TrustedPowerShellPath {
    if ([string]::IsNullOrWhiteSpace($env:SystemRoot)) {
        throw "SystemRoot is not set; refusing to elevate an unqualified powershell.exe."
    }
    $trusted = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
    if (-not (Test-Path -LiteralPath $trusted -PathType Leaf)) {
        throw "Trusted Windows PowerShell not found: $trusted"
    }
    return $trusted
}

function Assert-PassedReport {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [string]$Name
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Name was not produced: $Path"
    }
    $text = Get-Content -LiteralPath $Path -Raw
    if ([string]::IsNullOrWhiteSpace($text)) {
        throw "$Name is empty: $Path"
    }
    $report = $text | ConvertFrom-Json
    if ($report.status -ne "Passed") {
        throw "$Name did not report a passing status ('$($report.status)'): $Path"
    }
}

Assert-QualifiedPath -Path $SharedRoot -Name "SharedRoot"
Assert-QualifiedPath -Path $StageRoot -Name "StageRoot"

Write-Host "Ext VM launcher starting. StageRoot=$StageRoot"
if (-not (Test-Path -LiteralPath $SharedRoot -PathType Container)) {
    throw "Shared repo root not found: $SharedRoot"
}
Write-Host "Shared root ready: $SharedRoot"

$sharedRunner = Join-Path $SharedRoot "scripts\run_partition_manager_ext_filesystem_vm_gate.ps1"
if (-not (Test-Path -LiteralPath $sharedRunner -PathType Leaf)) {
    throw "Ext filesystem VM gate runner not found: $sharedRunner"
}

$sharedEvidenceRoot = Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\external-evidence\external.ext-filesystem-write"
$sharedReport = Join-Path $sharedEvidenceRoot "report.json"
$localRunner = Join-Path $StageRoot "run_partition_manager_ext_filesystem_vm_gate.ps1"
$localProjectRoot = Join-Path $StageRoot "repo"
$localEvidenceRoot = Join-Path $StageRoot "external.ext-filesystem-write"
$localReport = Join-Path $localEvidenceRoot "report.json"

Assert-NoReparsePoint -Path $StageRoot -Name "StageRoot"
New-Item -ItemType Directory -Path $StageRoot -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $localProjectRoot "tools") -Force | Out-Null
New-Item -ItemType Directory -Path $localEvidenceRoot -Force | Out-Null
New-Item -ItemType Directory -Path $sharedEvidenceRoot -Force | Out-Null
Write-Host "Stage directories ready."
Copy-Item -LiteralPath $sharedRunner -Destination $localRunner -Force
Write-Host "Runner staged."
$localFilesystemTools = Join-Path $localProjectRoot "tools\filesystem"
New-Item -ItemType Directory -Path $localFilesystemTools -Force | Out-Null
Copy-Item `
    -LiteralPath (Join-Path $SharedRoot "tools\filesystem\manifest.json") `
    -Destination $localFilesystemTools `
    -Force
Write-Host "Manifest staged."
Copy-Item `
    -LiteralPath (Join-Path $SharedRoot "tools\filesystem\README.md") `
    -Destination $localFilesystemTools `
    -Force
Write-Host "Filesystem README staged."
$localE2fsprogs = Join-Path $localFilesystemTools "e2fsprogs"
New-Item -ItemType Directory -Path $localE2fsprogs -Force | Out-Null
foreach ($item in @(
    "e2fsck.exe",
    "mke2fs.exe",
    "resize2fs.exe",
    "e2fsprogs-1.47.4.tar.xz",
    "e2fsprogs-1.47.4-sak-mingw.patch",
    "NOTICE",
    "BUILD-NOTES.md"
)) {
    Write-Host "Staging e2fsprogs file: $item"
    Copy-Item `
        -LiteralPath (Join-Path (Join-Path $SharedRoot "tools\filesystem\e2fsprogs") $item) `
        -Destination $localE2fsprogs `
        -Force
}
Write-Host "Filesystem tools staged."

$argumentList = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", "`"$localRunner`"",
    "-ProjectRoot", "`"$localProjectRoot`"",
    "-EvidenceRoot", "`"$localEvidenceRoot`"",
    "-ReportPath", "`"$localReport`"",
    "-DiskNumber", $DiskNumber,
    "-FileSystem", "`"$FileSystem`"",
    "-Force"
)
if ($NoCleanup) {
    $argumentList += "-NoCleanup"
}

$trustedPowerShell = Get-TrustedPowerShellPath
$process = Start-Process `
    -FilePath $trustedPowerShell `
    -Verb RunAs `
    -Wait `
    -PassThru `
    -ArgumentList $argumentList
Write-Host "Elevated process exited with code $($process.ExitCode)."

if (Test-Path -LiteralPath $localEvidenceRoot -PathType Container) {
    Write-Host "Copying evidence back."
    Copy-Item -Path (Join-Path $localEvidenceRoot "*") -Destination $sharedEvidenceRoot -Recurse -Force
}
if (Test-Path -LiteralPath $localReport -PathType Leaf) {
    Copy-Item -LiteralPath $localReport -Destination $sharedReport -Force
}

if ($process.ExitCode -ne 0) {
    throw "Ext filesystem VM gate exited with code $($process.ExitCode). Evidence copied when available: $sharedEvidenceRoot"
}

Assert-PassedReport -Path $sharedReport -Name "Ext filesystem VM gate report"

Write-Host "Ext filesystem VM gate completed. Report: $sharedReport"

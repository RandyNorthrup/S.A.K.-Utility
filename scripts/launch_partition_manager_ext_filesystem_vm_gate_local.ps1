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
    [string]$StageRoot = (Join-Path $env:PUBLIC ("sak-ext-filesystem-vm-gate-" + (Get-Date -Format "yyyyMMddHHmmss"))),
    [int]$DiskNumber = 1,
    [string]$FileSystem = "ext4",
    [switch]$NoCleanup
)

$ErrorActionPreference = "Stop"

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

$process = Start-Process `
    -FilePath "powershell.exe" `
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

if (-not (Test-Path -LiteralPath $sharedReport -PathType Leaf)) {
    throw "Ext filesystem VM gate did not produce report: $sharedReport"
}

Write-Host "Ext filesystem VM gate completed. Report: $sharedReport"

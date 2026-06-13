<#
.SYNOPSIS
    Stages and launches the Linux swap VM raw-format gate inside SAK-PM-Lab-Win11.

.DESCRIPTION
    Run from the non-elevated VM desktop with the shared repo mounted as
    \\vboxsvr\sakrepo. It copies the elevated runner and S.A.K. probe certifier
    locally, prompts UAC, runs destructive Linux swap raw-partition proof on a
    disposable VM data disk, and writes report.json evidence back to the shared
    repository.
#>

[CmdletBinding()]
param(
    [string]$SharedRoot = "\\vboxsvr\sakrepo",
    [string]$StageRoot = (Join-Path $env:PUBLIC ("sak-linux-swap-vm-gate-" + (Get-Date -Format "yyyyMMddHHmmss"))),
    [int]$DiskNumber = 1,
    [ValidateSet(4096, 8192, 16384, 65536)] [int]$PageSizeBytes = 4096,
    [string]$Label = "SAKSWAPVM",
    [switch]$NoCleanup
)

$ErrorActionPreference = "Stop"

function Copy-OptionalPattern {
    param(
        [Parameter(Mandatory = $true)] [string]$SourceDirectory,
        [Parameter(Mandatory = $true)] [string]$Pattern,
        [Parameter(Mandatory = $true)] [string]$DestinationDirectory
    )

    Get-ChildItem -LiteralPath $SourceDirectory -Filter $Pattern -File -ErrorAction SilentlyContinue |
        ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $DestinationDirectory -Force
            Write-Host "Staged runtime file: $($_.Name)"
        }
}

Write-Host "Linux swap VM launcher starting. StageRoot=$StageRoot"
if (-not (Test-Path -LiteralPath $SharedRoot -PathType Container)) {
    throw "Shared repo root not found: $SharedRoot"
}
Write-Host "Shared root ready: $SharedRoot"

$sharedRunner = Join-Path $SharedRoot "scripts\run_partition_manager_linux_swap_vm_gate.ps1"
if (-not (Test-Path -LiteralPath $sharedRunner -PathType Leaf)) {
    throw "Linux swap VM gate runner not found: $sharedRunner"
}

$sharedBuild = Join-Path $SharedRoot "build\Release"
$sharedCertifier = Join-Path $sharedBuild "partition_filesystem_probe_certifier.exe"
if (-not (Test-Path -LiteralPath $sharedCertifier -PathType Leaf)) {
    throw "S.A.K. probe certifier not found: $sharedCertifier"
}

$sharedEvidenceRoot = Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\external-evidence\external.linux-swap-format-vm"
$sharedReport = Join-Path $sharedEvidenceRoot "report.json"
$localRunner = Join-Path $StageRoot "run_partition_manager_linux_swap_vm_gate.ps1"
$localProjectRoot = Join-Path $StageRoot "repo"
$localBuild = Join-Path $localProjectRoot "build\Release"
$localEvidenceRoot = Join-Path $StageRoot "external.linux-swap-format-vm"
$localReport = Join-Path $localEvidenceRoot "report.json"

New-Item -ItemType Directory -Path $StageRoot -Force | Out-Null
New-Item -ItemType Directory -Path $localBuild -Force | Out-Null
New-Item -ItemType Directory -Path $localEvidenceRoot -Force | Out-Null
New-Item -ItemType Directory -Path $sharedEvidenceRoot -Force | Out-Null
Write-Host "Stage directories ready."

Copy-Item -LiteralPath $sharedRunner -Destination $localRunner -Force
Copy-Item -LiteralPath $sharedCertifier -Destination $localBuild -Force
Copy-OptionalPattern -SourceDirectory $sharedBuild -Pattern "Qt6*.dll" -DestinationDirectory $localBuild
Copy-OptionalPattern -SourceDirectory $sharedBuild -Pattern "msvcp*.dll" -DestinationDirectory $localBuild
Copy-OptionalPattern -SourceDirectory $sharedBuild -Pattern "vcruntime*.dll" -DestinationDirectory $localBuild
Copy-OptionalPattern -SourceDirectory $sharedBuild -Pattern "concrt*.dll" -DestinationDirectory $localBuild
Write-Host "Runner and certifier runtime staged."

$argumentList = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", "`"$localRunner`"",
    "-ProjectRoot", "`"$localProjectRoot`"",
    "-EvidenceRoot", "`"$localEvidenceRoot`"",
    "-ReportPath", "`"$localReport`"",
    "-DiskNumber", $DiskNumber,
    "-PageSizeBytes", $PageSizeBytes,
    "-Label", "`"$Label`"",
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
    throw "Linux swap VM gate exited with code $($process.ExitCode). Evidence copied when available: $sharedEvidenceRoot"
}

if (-not (Test-Path -LiteralPath $sharedReport -PathType Leaf)) {
    throw "Linux swap VM gate did not produce report: $sharedReport"
}

Write-Host "Linux swap VM gate completed. Report: $sharedReport"

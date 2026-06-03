<#
.SYNOPSIS
    Stages and launches offline BIOS/MBR fixture build elevated inside the VM.
#>

[CmdletBinding()]
param(
    [string]$SharedRoot = "\\vboxsvr\sakrepo",
    [string]$StageRoot = "$env:PUBLIC\sak-bios-mbr-fixture",
    [string]$OutputRoot = "\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\bios-mbr-fixture"
)

$ErrorActionPreference = "Stop"

New-Item -ItemType Directory -Path $StageRoot -Force | Out-Null
$localScript = Join-Path $StageRoot "run_partition_manager_offline_bios_mbr_fixture_build.ps1"
Copy-Item -LiteralPath (Join-Path $SharedRoot "scripts\run_partition_manager_offline_bios_mbr_fixture_build.ps1") -Destination $localScript -Force

$argumentList = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", "`"$localScript`"",
    "-OutputRoot", "`"$OutputRoot`"",
    "-ResumeExistingTarget",
    "-Force"
)

$process = Start-Process -FilePath powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList $argumentList
if ($process.ExitCode -ne 0) {
    throw "Elevated BIOS/MBR fixture build exited with code $($process.ExitCode)."
}

Write-Host "Elevated BIOS/MBR fixture build completed."

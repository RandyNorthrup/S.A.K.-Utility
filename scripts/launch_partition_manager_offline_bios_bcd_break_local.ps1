<#
.SYNOPSIS
    Stages and launches offline BIOS BCD break elevated inside the VM.
#>

[CmdletBinding()]
param(
    [string]$SharedRoot = "\\vboxsvr\sakrepo",
    [string]$StageRoot = "$env:PUBLIC\sak-offline-bios-break",
    [string]$OutputRoot = "\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\offline-bios-break"
)

$ErrorActionPreference = "Stop"

New-Item -ItemType Directory -Path $StageRoot -Force | Out-Null
$localScript = Join-Path $StageRoot "run_partition_manager_offline_bios_bcd_break.ps1"
Copy-Item -LiteralPath (Join-Path $SharedRoot "scripts\run_partition_manager_offline_bios_bcd_break.ps1") -Destination $localScript -Force

$argumentList = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", "`"$localScript`"",
    "-OutputRoot", "`"$OutputRoot`"",
    "-Force"
)

$process = Start-Process -FilePath powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList $argumentList
if ($process.ExitCode -ne 0) {
    throw "Elevated offline BIOS break exited with code $($process.ExitCode)."
}

Write-Host "Elevated offline BIOS break completed."

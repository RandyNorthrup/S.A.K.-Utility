<#
.SYNOPSIS
    Stages and launches system MBR2GPT gate elevated inside the VM.
#>

[CmdletBinding()]
param(
    [string]$SharedRoot = "\\vboxsvr\sakrepo",
    [string]$StageRoot = "$env:PUBLIC\sak-system-mbr2gpt",
    [string]$OutputRoot = "\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\system-mbr2gpt"
)

$ErrorActionPreference = "Stop"

New-Item -ItemType Directory -Path $StageRoot -Force | Out-Null
$localScript = Join-Path $StageRoot "run_partition_manager_system_mbr2gpt_external_gate.ps1"
Copy-Item -LiteralPath (Join-Path $SharedRoot "scripts\run_partition_manager_system_mbr2gpt_external_gate.ps1") -Destination $localScript -Force

$argumentList = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", "`"$localScript`"",
    "-OutputRoot", "`"$OutputRoot`"",
    "-Force"
)

$process = Start-Process -FilePath powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList $argumentList
if ($process.ExitCode -ne 0) {
    throw "Elevated system MBR2GPT gate exited with code $($process.ExitCode)."
}

Write-Host "Elevated system MBR2GPT gate completed."

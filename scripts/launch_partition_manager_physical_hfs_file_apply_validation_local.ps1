<#
.SYNOPSIS
    Launches physical HFS File Apply destructive validation with UAC auth.

.DESCRIPTION
    Starts an elevated PowerShell process via Windows runas/UAC and waits for
    completion. No keyboard automation is used. The elevated child seeds and
    mutates the selected expendable HFS partition, then writes JSON evidence.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [int]$DiskNumber,
    [Parameter(Mandatory = $true)] [int]$PartitionNumber,
    [string]$ExpectedSerialNumber = "",
    [string]$ExpectedFriendlyNamePattern = "",
    [string]$EvidenceRoot = "artifacts\partition-manager-certification\vm-lab\external-evidence\external.hfs-file-apply-physical",
    [switch]$AllowInternalDisk,
    [switch]$AllowLargeUnpinnedDisk,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$runner = Join-Path $ProjectRoot "scripts\run_partition_manager_physical_hfs_file_apply_validation.ps1"
if (-not (Test-Path -LiteralPath $runner -PathType Leaf)) {
    throw "Runner not found: $runner"
}
if (-not $Force) {
    throw "Pass -Force after confirming the selected physical HFS partition is expendable."
}

function Quote-PowerShellLiteral {
    param([Parameter(Mandatory = $true)] [string]$Value)
    return "'" + ($Value -replace "'", "''") + "'"
}

$command = @(
    '$ErrorActionPreference = "Stop"',
    "& $(Quote-PowerShellLiteral $runner) -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber -ProjectRoot $(Quote-PowerShellLiteral $ProjectRoot) -EvidenceRoot $(Quote-PowerShellLiteral $EvidenceRoot) -Force"
) -join "`n"
if (-not [string]::IsNullOrWhiteSpace($ExpectedSerialNumber)) {
    $command += " -ExpectedSerialNumber $(Quote-PowerShellLiteral $ExpectedSerialNumber)"
}
if (-not [string]::IsNullOrWhiteSpace($ExpectedFriendlyNamePattern)) {
    $command += " -ExpectedFriendlyNamePattern $(Quote-PowerShellLiteral $ExpectedFriendlyNamePattern)"
}
if ($AllowInternalDisk) {
    $command += " -AllowInternalDisk"
}
if ($AllowLargeUnpinnedDisk) {
    $command += " -AllowLargeUnpinnedDisk"
}
$encodedCommand = [Convert]::ToBase64String([System.Text.Encoding]::Unicode.GetBytes($command))

Write-Host "Launching elevated physical HFS File Apply validation for disk $DiskNumber partition $PartitionNumber."
Write-Host "Approve the Windows UAC prompt to continue."
$process = Start-Process -FilePath powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-EncodedCommand", $encodedCommand
)
Write-Host "Elevated process exited with code $($process.ExitCode)."
if ($process.ExitCode -ne 0) {
    throw "Elevated physical HFS File Apply validation exited with code $($process.ExitCode)."
}

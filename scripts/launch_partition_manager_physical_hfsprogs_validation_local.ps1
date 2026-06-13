<#
.SYNOPSIS
    Launches physical HFS+/HFSX destructive validation with UAC auth.

.DESCRIPTION
    Starts an elevated PowerShell process via Windows runas/UAC and waits for
    completion. No keyboard automation is used. The elevated child formats the
    selected expendable partition and writes JSON evidence.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [int]$DiskNumber,
    [int]$PartitionNumber = -1,
    [string]$ExpectedSerialNumber = "",
    [string]$ExpectedFriendlyNamePattern = "",
    [ValidateSet("HFS+", "HFSX")] [string[]]$FileSystems = @("HFS+", "HFSX", "HFS+"),
    [string]$EvidenceRoot = "artifacts\partition-manager-certification\vm-lab\external-evidence\external.hfsprogs-physical-destructive",
    [switch]$AllowInternalDisk,
    [switch]$AllowLargeUnpinnedDisk,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$runner = Join-Path $ProjectRoot "scripts\run_partition_manager_physical_hfsprogs_validation.ps1"
if (-not (Test-Path -LiteralPath $runner -PathType Leaf)) {
    throw "Runner not found: $runner"
}
if (-not $Force) {
    throw "Pass -Force after confirming the selected physical partition is expendable."
}

function Quote-PowerShellLiteral {
    param([Parameter(Mandatory = $true)] [string]$Value)
    return "'" + ($Value -replace "'", "''") + "'"
}

$fileSystemsLiteral = "@(" + (($FileSystems | ForEach-Object { Quote-PowerShellLiteral $_ }) -join ", ") + ")"
$command = @(
    '$ErrorActionPreference = "Stop"',
    "& $(Quote-PowerShellLiteral $runner) -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber -ProjectRoot $(Quote-PowerShellLiteral $ProjectRoot) -EvidenceRoot $(Quote-PowerShellLiteral $EvidenceRoot) -FileSystems $fileSystemsLiteral -Force"
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

Write-Host "Launching elevated physical HFS tool validation for disk $DiskNumber partition $PartitionNumber."
Write-Host "Approve the Windows UAC prompt to continue."
$process = Start-Process -FilePath powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-EncodedCommand", $encodedCommand
)
Write-Host "Elevated process exited with code $($process.ExitCode)."
if ($process.ExitCode -ne 0) {
    throw "Elevated physical HFS tool validation exited with code $($process.ExitCode)."
}

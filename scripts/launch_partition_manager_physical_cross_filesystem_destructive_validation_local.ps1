<#
.SYNOPSIS
    Launches the physical cross-filesystem destructive validation with proper UAC auth.

.DESCRIPTION
    Starts an elevated PowerShell process via Windows runas/UAC and waits for
    completion. No keyboard automation is used. The elevated child performs all
    destructive work and writes JSON evidence.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [int]$DiskNumber,
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

function Quote-PowerShellLiteral {
    param([Parameter(Mandatory = $true)] [string]$Value)
    return "'" + ($Value -replace "'", "''") + "'"
}

$fileSystemsLiteral = "@(" + (($FileSystems | ForEach-Object { Quote-PowerShellLiteral $_ }) -join ", ") + ")"
$command = @(
    '$ErrorActionPreference = "Stop"',
    "& $(Quote-PowerShellLiteral $runner) -DiskNumber $DiskNumber -ProjectRoot $(Quote-PowerShellLiteral $ProjectRoot) -EvidenceRoot $(Quote-PowerShellLiteral $EvidenceRoot) -FileSystems $fileSystemsLiteral -Force"
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
if ($NoCleanup) {
    $command += " -NoCleanup"
}
$encodedCommand = [Convert]::ToBase64String([System.Text.Encoding]::Unicode.GetBytes($command))

Write-Host "Launching elevated destructive physical cross-filesystem validation for disk $DiskNumber."
Write-Host "Approve the Windows UAC prompt to continue."
$process = Start-Process -FilePath powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-EncodedCommand", $encodedCommand
)
Write-Host "Elevated process exited with code $($process.ExitCode)."
if ($process.ExitCode -ne 0) {
    throw "Elevated physical cross-filesystem validation exited with code $($process.ExitCode)."
}

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
    [Parameter(Mandatory = $true)] [ValidatePattern('^\d+$')] [string]$DiskNumber,
    [Parameter(Mandatory = $true)] [ValidatePattern('^\d+$')] [string]$PartitionNumber,
    [string]$ExpectedSerialNumber = "",
    [string]$ExpectedFriendlyNamePattern = "",
    [string]$EvidenceRoot = "artifacts\partition-manager-certification\vm-lab\external-evidence\external.hfs-file-apply-physical",
    [switch]$AllowInternalDisk,
    [switch]$AllowLargeUnpinnedDisk,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

# EvidenceRoot is resolved by the elevated child against the project tree. Reject a rooted
# (absolute/UNC/drive-qualified) path or any ".." traversal so privileged evidence writes cannot be
# routed outside the approved artifact tree (or trigger outbound authentication to a UNC host).
if ([string]::IsNullOrWhiteSpace($EvidenceRoot)) {
    throw "EvidenceRoot must not be empty."
}
if ([System.IO.Path]::IsPathRooted($EvidenceRoot)) {
    throw "EvidenceRoot must be a project-relative path, not a rooted/UNC path: $EvidenceRoot"
}
if ($EvidenceRoot -split '[\\/]' | Where-Object { $_ -eq '..' }) {
    throw "EvidenceRoot must not contain '..' path traversal: $EvidenceRoot"
}

# Volatile disk/partition numbers alone must not authorize erasing an internal or oversized disk.
# Require an immutable identity pin (serial or friendly-name) whenever a safety override is set, and
# reject a friendly-name pattern too broad to identify a single device.
$hasSerialPin = -not [string]::IsNullOrWhiteSpace($ExpectedSerialNumber)
$hasNamePin = -not [string]::IsNullOrWhiteSpace($ExpectedFriendlyNamePattern)
if ($hasNamePin -and ($ExpectedFriendlyNamePattern.Trim() -match '^\.[\*\+]?$')) {
    throw "ExpectedFriendlyNamePattern is too broad to identify a device: $ExpectedFriendlyNamePattern"
}
if (($AllowInternalDisk -or $AllowLargeUnpinnedDisk) -and -not ($hasSerialPin -or $hasNamePin)) {
    throw "Provide -ExpectedSerialNumber or -ExpectedFriendlyNamePattern when using -AllowInternalDisk or -AllowLargeUnpinnedDisk."
}

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$runner = Join-Path $ProjectRoot "scripts\run_partition_manager_physical_hfs_file_apply_validation.ps1"
if (-not (Test-Path -LiteralPath $runner -PathType Leaf)) {
    throw "Runner not found: $runner"
}
if (-not $Force) {
    throw "Pass -Force after confirming the selected physical HFS partition is expendable."
}
# Pin the runner content and re-verify it inside the elevated child, and pin the host PowerShell to
# System32 so an attacker-controlled PATH or current directory cannot substitute the elevated binary.
$expectedHash = (Get-FileHash -LiteralPath $runner -Algorithm SHA256).Hash
$powershell = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
if (-not (Test-Path -LiteralPath $powershell -PathType Leaf)) {
    throw "System PowerShell not found: $powershell"
}

function Quote-PowerShellLiteral {
    param([Parameter(Mandatory = $true)] [string]$Value)
    return "'" + ($Value -replace "'", "''") + "'"
}

$command = @(
    '$ErrorActionPreference = "Stop"',
    "`$runner = $(Quote-PowerShellLiteral $runner)",
    "`$expected = $(Quote-PowerShellLiteral $expectedHash)",
    'if ((Get-FileHash -LiteralPath $runner -Algorithm SHA256).Hash -ne $expected) { throw "Runner hash mismatch (possible tampering): $runner" }',
    "& `$runner -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber -ProjectRoot $(Quote-PowerShellLiteral $ProjectRoot) -EvidenceRoot $(Quote-PowerShellLiteral $EvidenceRoot) -Force"
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
$process = Start-Process -FilePath $powershell -Verb RunAs -Wait -PassThru -ArgumentList @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-EncodedCommand", $encodedCommand
)
Write-Host "Elevated process exited with code $($process.ExitCode)."
if ($process.ExitCode -ne 0) {
    throw "Elevated physical HFS File Apply validation exited with code $($process.ExitCode)."
}

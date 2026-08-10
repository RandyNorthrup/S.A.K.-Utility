<#
.SYNOPSIS
    Stages and launches offline UEFI bcdboot repair elevated inside the VM.
#>

[CmdletBinding()]
param(
    [ValidateNotNullOrEmpty()]
    [string]$SharedRoot = "\\vboxsvr\sakrepo",
    [ValidateNotNullOrEmpty()]
    [string]$StageRoot = "$env:PUBLIC\sak-offline-uefi-repair",
    [ValidateNotNullOrEmpty()]
    [string]$OutputRoot = "\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\offline-uefi-repair"
)

$ErrorActionPreference = "Stop"

# Fail closed on ambient/relative inputs: every path that crosses the UAC boundary must be an
# explicit rooted path, and none may carry a double quote (which would otherwise break out of the
# elevated command line no matter how it is quoted).
foreach ($pair in @{ SharedRoot = $SharedRoot; StageRoot = $StageRoot; OutputRoot = $OutputRoot }.GetEnumerator()) {
    if ([string]::IsNullOrWhiteSpace($pair.Value)) {
        throw "$($pair.Key) must not be empty."
    }
    if (-not [System.IO.Path]::IsPathRooted($pair.Value)) {
        throw "$($pair.Key) must be a rooted path: $($pair.Value)"
    }
    if ($pair.Value.Contains('"')) {
        throw "$($pair.Key) must not contain a double-quote character."
    }
}

$sourceRunner = Join-Path $SharedRoot "scripts\run_partition_manager_offline_uefi_bcdboot_repair.ps1"
if (-not (Test-Path -LiteralPath $sourceRunner -PathType Leaf)) {
    throw "Runner not found in shared root: $sourceRunner"
}
# Pin the runner content copied from the (lab-trusted) shared root so the elevated child can prove
# the staged copy was not swapped between the copy and elevated execution.
$expectedHash = (Get-FileHash -LiteralPath $sourceRunner -Algorithm SHA256).Hash

# Reject a staging root that is a reparse point (symlink/junction) -- it could redirect the copy
# and the later elevated read to an attacker-chosen location.
if (Test-Path -LiteralPath $StageRoot) {
    $stageItem = Get-Item -LiteralPath $StageRoot -Force
    if ($stageItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
        throw "Stage root is a reparse point (possible redirection): $StageRoot"
    }
}
New-Item -ItemType Directory -Path $StageRoot -Force | Out-Null
$localScript = Join-Path $StageRoot "run_partition_manager_offline_uefi_bcdboot_repair.ps1"
Copy-Item -LiteralPath $sourceRunner -Destination $localScript -Force

# Pin the host PowerShell to its System32 location so an attacker-controlled PATH or current
# directory cannot substitute the binary that is elevated.
$powershell = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
if (-not (Test-Path -LiteralPath $powershell -PathType Leaf)) {
    throw "System PowerShell not found: $powershell"
}

function Quote-PowerShellLiteral {
    param([Parameter(Mandatory = $true)] [AllowEmptyString()] [string]$Value)
    return "'" + ($Value -replace "'", "''") + "'"
}

# Run the elevated child from an encoded script so no argument can be injected through quote
# breakout, and re-verify the staged runner's SHA-256 inside the elevated process before invoking
# it (closes the world-writable-staging swap window).
$command = @(
    '$ErrorActionPreference = "Stop"',
    "`$staged = $(Quote-PowerShellLiteral $localScript)",
    "`$expected = $(Quote-PowerShellLiteral $expectedHash)",
    'if ((Get-FileHash -LiteralPath $staged -Algorithm SHA256).Hash -ne $expected) { throw "Staged runner hash mismatch (possible tampering): $staged" }',
    "& `$staged -OutputRoot $(Quote-PowerShellLiteral $OutputRoot) -Force"
) -join "`n"
$encodedCommand = [Convert]::ToBase64String([System.Text.Encoding]::Unicode.GetBytes($command))

$process = Start-Process -FilePath $powershell -Verb RunAs -Wait -PassThru -ArgumentList @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-EncodedCommand", $encodedCommand
)
if ($null -eq $process) {
    throw "Failed to start elevated offline UEFI repair process."
}
if ($process.ExitCode -ne 0) {
    throw "Elevated offline UEFI repair exited with code $($process.ExitCode)."
}

Write-Host "Elevated offline UEFI repair completed."

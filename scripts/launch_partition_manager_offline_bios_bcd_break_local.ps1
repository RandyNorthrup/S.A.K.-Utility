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

function Assert-QualifiedPath {
    param(
        [Parameter(Mandatory = $true)] [AllowEmptyString()] [string]$Path,
        [Parameter(Mandatory = $true)] [string]$Name
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "$Name must be a non-empty path."
    }
    if ($Path -match '["*?\[\]]') {
        throw "$Name contains characters that cannot be quoted or staged safely: $Path"
    }
    if ($Path -notmatch '^([A-Za-z]:[\\/]|\\\\[^\\])') {
        throw "$Name must be a fully qualified local or UNC path: $Path"
    }
}

function Assert-NoReparsePoint {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [string]$Name
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -eq [System.IO.FileAttributes]::ReparsePoint) {
        throw "$Name is a reparse point and cannot be trusted for elevated staging: $Path"
    }
}

function Get-TrustedPowerShellPath {
    if ([string]::IsNullOrWhiteSpace($env:SystemRoot)) {
        throw "SystemRoot is not set; refusing to elevate an unqualified powershell.exe."
    }
    $trusted = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
    if (-not (Test-Path -LiteralPath $trusted -PathType Leaf)) {
        throw "Trusted Windows PowerShell not found: $trusted"
    }
    return $trusted
}

Assert-QualifiedPath -Path $SharedRoot -Name "SharedRoot"
Assert-QualifiedPath -Path $StageRoot -Name "StageRoot"
Assert-QualifiedPath -Path $OutputRoot -Name "OutputRoot"

Assert-NoReparsePoint -Path $StageRoot -Name "StageRoot"
New-Item -ItemType Directory -Path $StageRoot -Force | Out-Null
$localScript = Join-Path $StageRoot "run_partition_manager_offline_bios_bcd_break.ps1"
Assert-NoReparsePoint -Path $localScript -Name "Staged runner"
Copy-Item -LiteralPath (Join-Path $SharedRoot "scripts\run_partition_manager_offline_bios_bcd_break.ps1") -Destination $localScript -Force
if (-not (Test-Path -LiteralPath $localScript -PathType Leaf)) {
    throw "Runner was not staged: $localScript"
}
Assert-NoReparsePoint -Path $localScript -Name "Staged runner"

$argumentList = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", "`"$localScript`"",
    "-OutputRoot", "`"$OutputRoot`"",
    "-Force"
)

$trustedPowerShell = Get-TrustedPowerShellPath
$launchStart = Get-Date
$process = Start-Process -FilePath $trustedPowerShell -Verb RunAs -Wait -PassThru -ArgumentList $argumentList
if ($process.ExitCode -ne 0) {
    throw "Elevated offline BIOS break exited with code $($process.ExitCode)."
}

$freshReport = @(Get-ChildItem -LiteralPath $OutputRoot -Recurse -Filter "offline-bios-break-report.json" -File -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -ge $launchStart } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1)
if ($freshReport.Count -eq 0) {
    throw "Elevated offline BIOS break exited 0 but produced no fresh report under $OutputRoot."
}
$reportData = Get-Content -LiteralPath $freshReport[0].FullName -Raw | ConvertFrom-Json
if (-not $reportData.bcd_removed) {
    throw "Offline BIOS break report does not confirm BCD removal: $($freshReport[0].FullName)"
}

Write-Host "Elevated offline BIOS break completed. Report: $($freshReport[0].FullName)"

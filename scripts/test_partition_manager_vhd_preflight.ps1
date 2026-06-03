<#
.SYNOPSIS
    Emits a read-only preflight report for Partition Manager disposable-VHD certification.

.DESCRIPTION
    Checks whether the current shell and host look ready to run the destructive
    disposable-VHD matrix. This script does not create, attach, detach, format,
    wipe, or mutate any disk. Use -RequireReady only when preparing to start the
    elevated VHD certification run.
#>

[CmdletBinding()]
param(
    [string]$OutputRoot = "artifacts\partition-manager-certification",
    [int]$VhdSizeMB = 768,
    [string]$OutputPath = "",
    [switch]$RequireAdministrator,
    [switch]$RequireReady,
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$MinimumVhdSizeMB = 512
$CandidateDriveLetters = @("T", "U", "V", "W", "X", "Y", "Z")
$RequiredCommands = @(
    "diskpart.exe",
    "convert.exe",
    "Get-Disk",
    "Set-Disk",
    "Initialize-Disk",
    "New-Partition",
    "Resize-Partition",
    "Remove-Partition",
    "Format-Volume",
    "Get-Partition",
    "Add-PartitionAccessPath",
    "Get-Volume",
    "Get-DiskImage",
    "Mount-DiskImage",
    "Dismount-DiskImage"
)

function Resolve-ProjectPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return (Join-Path $ProjectRoot $Path)
}

function Test-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Add-Blocker {
    param(
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[string]]$Blockers,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    $Blockers.Add($Message)
}

function Add-Warning {
    param(
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[string]]$Warnings,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    $Warnings.Add($Message)
}

Push-Location $ProjectRoot
try {
    $matrixPath = Join-Path $ProjectRoot "docs\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
    $matrix = Get-Content -LiteralPath $matrixPath -Raw | ConvertFrom-Json
    if ($matrix.schema_version -ne 1) {
        throw "Unsupported Partition Manager certification matrix schema_version: $($matrix.schema_version)"
    }

    $blockers = [System.Collections.Generic.List[string]]::new()
    $warnings = [System.Collections.Generic.List[string]]::new()
    $isAdministrator = Test-Administrator
    if ($RequireAdministrator -and -not $isAdministrator) {
        Add-Blocker -Blockers $blockers -Message "Administrator shell required for VHD attach and disk mutation."
    }

    if ($VhdSizeMB -lt $MinimumVhdSizeMB) {
        Add-Blocker -Blockers $blockers -Message "VhdSizeMB must be at least $MinimumVhdSizeMB."
    }

    $commandChecks = @()
    foreach ($command in $RequiredCommands) {
        $found = $null -ne (Get-Command $command -ErrorAction SilentlyContinue)
        if (-not $found) {
            Add-Blocker -Blockers $blockers -Message "Required command unavailable: $command"
        }
        $commandChecks += [ordered]@{
            name = $command
            available = $found
        }
    }

    $usedLetters = @(Get-Volume -ErrorAction SilentlyContinue |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_.DriveLetter) } |
        ForEach-Object { $_.DriveLetter.ToString().ToUpperInvariant() })
    $freeLetters = @($CandidateDriveLetters | Where-Object { $usedLetters -notcontains $_ })
    if ($freeLetters.Count -lt 2) {
        Add-Blocker -Blockers $blockers -Message "At least two temporary drive letters from T-Z should be free for multi-VHD scenarios."
    }

    $resolvedOutputRoot = Resolve-ProjectPath -Path $OutputRoot
    $outputParent = if (Test-Path -LiteralPath $resolvedOutputRoot) {
        $resolvedOutputRoot
    }
    else {
        Split-Path -Parent $resolvedOutputRoot
    }
    if ([string]::IsNullOrWhiteSpace($outputParent)) {
        $outputParent = $ProjectRoot
    }

    $driveInfo = $null
    $freeBytes = 0L
    try {
        $fullOutputParent = [System.IO.Path]::GetFullPath($outputParent)
        $driveInfo = [System.IO.DriveInfo]::new(([System.IO.Path]::GetPathRoot($fullOutputParent)))
        $freeBytes = [int64]$driveInfo.AvailableFreeSpace
    }
    catch {
        Add-Warning -Warnings $warnings -Message "Could not determine free space for output root: $resolvedOutputRoot"
    }

    $estimatedBytes = [int64]([Math]::Max($VhdSizeMB, $MinimumVhdSizeMB)) * 1MB * 4
    if ($freeBytes -gt 0 -and $freeBytes -lt $estimatedBytes) {
        Add-Blocker -Blockers $blockers -Message "Output volume free space is below conservative VHD matrix estimate."
    }

    $report = [ordered]@{
        tool = "partition-manager-vhd-preflight"
        schema_version = 1
        generated_utc = (Get-Date).ToUniversalTime().ToString("o")
        project_root = $ProjectRoot
        output_root = $resolvedOutputRoot
        require_administrator = [bool]$RequireAdministrator
        require_ready = [bool]$RequireReady
        administrator = $isAdministrator
        vhd_size_mb = [Math]::Max($VhdSizeMB, $MinimumVhdSizeMB)
        minimum_vhd_size_mb = $MinimumVhdSizeMB
        vhd_scenarios = @($matrix.vhd_scenarios).Count
        external_gates = @($matrix.external_gates).Count
        required_commands = $commandChecks
        free_drive_letters = $freeLetters
        estimated_workspace_bytes = $estimatedBytes
        available_workspace_bytes = $freeBytes
        blockers = @($blockers)
        warnings = @($warnings)
        ready_for_vhd_certification = ($blockers.Count -eq 0)
        next_command = "powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run_partition_manager_destructive_certification.ps1 -RunVhdDataDiskMatrix -RelaunchElevated -RequireVhdDataDiskEvidence"
    }

    $json = $report | ConvertTo-Json -Depth 8
    if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
        $resolvedOutputPath = Resolve-ProjectPath -Path $OutputPath
        $parent = Split-Path -Parent $resolvedOutputPath
        if (-not [string]::IsNullOrWhiteSpace($parent)) {
            New-Item -ItemType Directory -Path $parent -Force | Out-Null
        }
        $json | Set-Content -LiteralPath $resolvedOutputPath -Encoding UTF8
    }

    if (-not $Quiet) {
        Write-Host "Partition Manager VHD preflight ready: $($report.ready_for_vhd_certification)"
        Write-Output $json
    }

    if ($RequireReady -and -not $report.ready_for_vhd_certification) {
        Write-Host "Partition Manager VHD preflight blockers: $($blockers -join '; ')" -ForegroundColor Red
        exit 2
    }
}
finally {
    Pop-Location
}

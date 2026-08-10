<#
.SYNOPSIS
    Verifies a Partition Manager disposable-VHD preflight report.
#>

[CmdletBinding()]
param(
    [string]$ReportPath = "artifacts\partition-manager-certification\readiness\vhd-preflight.json"
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
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

function Assert-Condition {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

Push-Location $ProjectRoot
try {
    $matrixPath = Join-Path $ProjectRoot "docs\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
    $matrix = Get-Content -LiteralPath $matrixPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($matrix.schema_version -eq 1) -Message "Unsupported Partition Manager certification matrix schema_version: $($matrix.schema_version)"

    $resolvedReportPath = Resolve-Path -LiteralPath (Resolve-ProjectPath -Path $ReportPath) -ErrorAction Stop
    $report = Get-Content -LiteralPath $resolvedReportPath.Path -Raw | ConvertFrom-Json

    Assert-Condition -Condition ($report.tool -eq "partition-manager-vhd-preflight") -Message "Unexpected VHD preflight tool"
    Assert-Condition -Condition ($report.schema_version -eq 1) -Message "Unexpected VHD preflight schema_version"
    Assert-Condition -Condition ([int]$report.minimum_vhd_size_mb -eq 512) -Message "Unexpected minimum VHD size"
    Assert-Condition -Condition ([int]$report.vhd_size_mb -ge [int]$report.minimum_vhd_size_mb) -Message "VHD size below minimum"
    Assert-Condition -Condition ([int]$report.vhd_scenarios -eq @($matrix.vhd_scenarios).Count) -Message "VHD scenario count mismatch"
    Assert-Condition -Condition ([int]$report.external_gates -eq @($matrix.external_gates).Count) -Message "External gate count mismatch"
    # Gate booleans must be real JSON booleans. A [bool] cast of the JSON string "false"
    # is $true in PowerShell, which would let a not-ready report enter the ready path or a
    # missing/"false" administrator flag bypass enforcement -- fail closed on any non-boolean.
    Assert-Condition -Condition ($report.ready_for_vhd_certification -is [bool]) -Message "ready_for_vhd_certification must be a JSON boolean"
    Assert-Condition -Condition ($report.require_administrator -is [bool]) -Message "require_administrator must be a JSON boolean"
    Assert-Condition -Condition ($report.administrator -is [bool]) -Message "administrator must be a JSON boolean"
    Assert-Condition -Condition ($report.next_command -like "*run_partition_manager_destructive_certification.ps1*") -Message "Missing destructive certification command"

    $actualCommands = @($report.required_commands | ForEach-Object { $_.name.ToString() })
    foreach ($command in $RequiredCommands) {
        Assert-Condition -Condition ($actualCommands -contains $command) -Message "VHD preflight missing command check: $command"
    }

    foreach ($commandCheck in @($report.required_commands)) {
        Assert-Condition -Condition ($null -ne $commandCheck.PSObject.Properties["available"]) -Message "Command check missing availability: $($commandCheck.name)"
        Assert-Condition -Condition ($commandCheck.available -is [bool]) -Message "Command availability must be a JSON boolean: $($commandCheck.name)"
    }

    # Filter out nulls so an omitted/null "blockers" does not masquerade as one blocker
    # via the PowerShell quirk @($null).Count -eq 1.
    $reportBlockers = @($report.blockers | Where-Object { $null -ne $_ } | ForEach-Object { $_.ToString() })

    if ($report.require_administrator -and -not $report.administrator) {
        Assert-Condition -Condition (@($reportBlockers | Where-Object { $_ -like "*Administrator shell required*" }).Count -gt 0) -Message "Non-admin required preflight missing administrator blocker"
        Assert-Condition -Condition (-not $report.ready_for_vhd_certification) -Message "Non-admin required preflight must not be ready"
    }

    if ($report.ready_for_vhd_certification) {
        Assert-Condition -Condition ($reportBlockers.Count -eq 0) -Message "Ready preflight has blockers"
        foreach ($commandCheck in @($report.required_commands)) {
            Assert-Condition -Condition ($commandCheck.available -eq $true) -Message "Ready preflight lists an unavailable required command: $($commandCheck.name)"
        }
    }
    else {
        Assert-Condition -Condition ($reportBlockers.Count -gt 0) -Message "Not-ready preflight missing blockers"
    }

    Write-Host "Partition Manager VHD preflight report passed: ready=$($report.ready_for_vhd_certification), blockers=$($reportBlockers.Count)."
}
finally {
    Pop-Location
}

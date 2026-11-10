<#
.SYNOPSIS
    Validates the machine-readable Apple filesystem capability owner.

.DESCRIPTION
    R5-IDX-7 found several prose copies of the HFS+/APFS capability list drifting in both
    directions. This gate makes certification/APPLE_FILESYSTEM_CAPABILITY_MATRIX.json the
    status owner and validates every expected row and its evidence contract. It reads only the
    JSON. It deliberately does not require ignored local artifact paths to exist on a clean
    runner; the evidence identifiers are historical records, while their status and shape are
    checked here.
#>

[CmdletBinding()]
param(
    [string]$ProjectRoot = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
}
$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
Set-Location $ProjectRoot

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-CertifiedRecord {
    param([object]$Record, [string]$Label)
    Assert-Condition ($Record.status -eq "certified") "$Label must be certified."
    Assert-Condition (([string]$Record.certified_on) -match '^\d{4}-\d{2}-\d{2}$') `
        "$Label must carry an ISO certified_on date."
    $refs = @($Record.evidence_refs)
    Assert-Condition ($refs.Count -gt 0) "$Label must carry at least one evidence reference."
    $allowedTypes = @("apple_tool_artifact", "apple_tool_report", "external_report", "physical_gate", "unit_test")
    foreach ($ref in $refs) {
        Assert-Condition ($allowedTypes -contains [string]$ref.type) `
            "$Label has unsupported evidence type '$($ref.type)'."
        Assert-Condition (-not [string]::IsNullOrWhiteSpace([string]$ref.id)) `
            "$Label has a blank evidence id."
    }
}

$matrixRelative = "certification/APPLE_FILESYSTEM_CAPABILITY_MATRIX.json"
$matrixPath = Join-Path $ProjectRoot $matrixRelative
Assert-Condition (Test-Path -LiteralPath $matrixPath -PathType Leaf) `
    "Apple filesystem capability matrix is missing: $matrixRelative"
$matrix = Get-Content -LiteralPath $matrixPath -Raw | ConvertFrom-Json
Assert-Condition ($matrix.schema_version -eq 1) "Apple capability matrix schema_version must be 1."
Assert-Condition ($matrix.matrix_id -eq "apple-filesystem-driver-capabilities") `
    "Apple capability matrix_id is invalid."

$expectedIds = @("H-a", "H-b", "H-c", "H-d", "H-e", "H-f", "H-g", "H-h",
                 "A-a", "A-b", "A-c", "A-d", "A-e", "A-f", "A-g", "A-h", "A-i")
$capabilities = @($matrix.capabilities)
$actualIds = @($capabilities | ForEach-Object { [string]$_.id })
Assert-Condition ($actualIds.Count -eq $expectedIds.Count) `
    "Apple capability matrix must contain exactly $($expectedIds.Count) capability rows."
Assert-Condition (($actualIds | Sort-Object -Unique).Count -eq $actualIds.Count) `
    "Apple capability matrix contains duplicate capability ids."
foreach ($id in $expectedIds) {
    Assert-Condition ($actualIds -contains $id) "Apple capability matrix is missing row $id."
}

foreach ($capability in $capabilities) {
    $id = [string]$capability.id
    Assert-Condition (-not [string]::IsNullOrWhiteSpace([string]$capability.name)) `
        "Capability $id has a blank name."
    $expectedFs = if ($id.StartsWith("H-")) { "HFS+" } else { "APFS" }
    Assert-Condition ($capability.filesystem -eq $expectedFs) `
        "Capability $id filesystem must be $expectedFs."
    if ($id -eq "A-i") {
        Assert-Condition ($capability.status -eq "design_decision") `
            "A-i must remain a design_decision until a certified Fusion rig result exists."
        Assert-Condition (-not [string]::IsNullOrWhiteSpace([string]$capability.reason)) `
            "A-i design decision requires a reason."
        Assert-Condition (-not [string]::IsNullOrWhiteSpace([string]$capability.fail_closed_behavior)) `
            "A-i design decision requires explicit fail_closed_behavior."
        Assert-Condition ([string]::IsNullOrWhiteSpace([string]$capability.certified_on)) `
            "A-i must not carry a certified_on date while it is a design decision."
    } else {
        Assert-CertifiedRecord $capability "Capability $id"
    }
}

$trackGates = @($matrix.track_gates)
$expectedTrackIds = @("apfs-full-driver", "hfs-full-driver")
Assert-Condition ($trackGates.Count -eq 2) "Apple capability matrix must contain two track gates."
foreach ($id in $expectedTrackIds) {
    $matches = @($trackGates | Where-Object { $_.id -eq $id })
    Assert-Condition ($matches.Count -eq 1) "Apple capability matrix track gate $id is missing or duplicated."
    Assert-CertifiedRecord $matches[0] "Track gate $id"
}

# The owner document is provenance (the archived plan that holds the history), not a status
# copy. Status is rendered from this JSON into the generated block in
# docs/PARTITION_MANAGER_CERTIFICATION.md by check_partition_manager_release_claims.ps1; no
# prose is parsed here (owner ruling 2026-10-24 retired the doc-citation and doc-row checks).
$ownerDocument = [string]$matrix.owner_document
Assert-Condition ($ownerDocument -eq "docs/archive/APFS_HFS_FULL_DRIVER_WRITE_PLAN.md") `
    "Apple capability owner_document must be docs/archive/APFS_HFS_FULL_DRIVER_WRITE_PLAN.md."

Write-Host ("Apple filesystem capability matrix passed: {0} capabilities, {1} track gates." -f `
    $capabilities.Count, $trackGates.Count)
exit 0

<#
.SYNOPSIS
    Verifies a Partition Manager certification artifact bundle manifest.
#>

[CmdletBinding()]
param(
    [string]$BundlePath = "artifacts\partition-manager-certification\readiness\certification-artifact-bundle.json"
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$RequiredKinds = @(
    "certification_status",
    "certification_report",
    "certification_gap_report_json",
    "certification_gap_report_markdown",
    "vhd_preflight",
    "external_evidence_manifest",
    "external_evidence_checklist"
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

function Read-JsonFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    return Get-Content -LiteralPath $resolved.Path -Raw | ConvertFrom-Json
}

Push-Location $ProjectRoot
try {
    $resolvedBundlePath = Resolve-Path -LiteralPath (Resolve-ProjectPath -Path $BundlePath) -ErrorAction Stop
    $bundle = Read-JsonFile -Path $resolvedBundlePath.Path
    Assert-Condition -Condition ($bundle.tool -eq "partition-manager-certification-artifact-bundle") -Message "Unexpected certification bundle tool"
    Assert-Condition -Condition ($bundle.schema_version -eq 1) -Message "Unexpected certification bundle schema_version"

    $artifacts = @($bundle.artifacts)
    foreach ($kind in $RequiredKinds) {
        $matches = @($artifacts | Where-Object { $_.kind -eq $kind })
        Assert-Condition -Condition ($matches.Count -eq 1) -Message "Certification bundle missing or duplicate artifact kind: $kind"
    }

    foreach ($artifact in $artifacts) {
        $resolvedArtifact = Resolve-Path -LiteralPath $artifact.path -ErrorAction Stop
        $item = Get-Item -LiteralPath $resolvedArtifact.Path -ErrorAction Stop
        $hash = (Get-FileHash -LiteralPath $resolvedArtifact.Path -Algorithm SHA256).Hash
        Assert-Condition -Condition ($artifact.sha256 -eq $hash) -Message "Certification bundle hash mismatch for $($artifact.kind)"
        Assert-Condition -Condition ([int64]$artifact.bytes -eq [int64]$item.Length) -Message "Certification bundle size mismatch for $($artifact.kind)"
    }

    $statusPath = @($artifacts | Where-Object { $_.kind -eq "certification_status" })[0].path
    $gapPath = @($artifacts | Where-Object { $_.kind -eq "certification_gap_report_json" })[0].path
    $preflightPath = @($artifacts | Where-Object { $_.kind -eq "vhd_preflight" })[0].path
    $status = Read-JsonFile -Path $statusPath
    $gap = Read-JsonFile -Path $gapPath
    $preflight = Read-JsonFile -Path $preflightPath

    Assert-Condition -Condition ($bundle.claim_level -eq $status.claim_level) -Message "Certification bundle claim level mismatch"
    Assert-Condition -Condition ($bundle.source_report_path -eq $status.report.path) -Message "Certification bundle source report mismatch"
    Assert-Condition -Condition ($bundle.external_manifest_path -eq $status.external_gates.manifest_path) -Message "Certification bundle external manifest mismatch"
    Assert-Condition -Condition ([int]$bundle.summary.vhd_data_disk.passed -eq [int]$status.vhd_data_disk.passed) -Message "Certification bundle VHD passed mismatch"
    Assert-Condition -Condition ([int]$bundle.summary.vhd_data_disk.required -eq [int]$status.vhd_data_disk.required) -Message "Certification bundle VHD required mismatch"
    Assert-Condition -Condition ([int]$bundle.summary.vhd_data_disk.incomplete -eq @($status.vhd_data_disk.incomplete_ids).Count) -Message "Certification bundle VHD incomplete mismatch"
    Assert-Condition -Condition ([int]$bundle.summary.external_gates.passed -eq [int]$status.external_gates.passed) -Message "Certification bundle external passed mismatch"
    Assert-Condition -Condition ([int]$bundle.summary.external_gates.required -eq [int]$status.external_gates.required) -Message "Certification bundle external required mismatch"
    Assert-Condition -Condition ([int]$bundle.summary.external_gates.incomplete -eq @($status.external_gates.incomplete_ids).Count) -Message "Certification bundle external incomplete mismatch"
    Assert-Condition -Condition ([int]$bundle.summary.gap_report.gaps -eq @($gap.gaps).Count) -Message "Certification bundle gap count mismatch"
    Assert-Condition -Condition ($bundle.summary.gap_report.claim_level -eq $gap.claim_level) -Message "Certification bundle gap claim level mismatch"
    Assert-Condition -Condition ([bool]$bundle.summary.vhd_preflight.ready -eq [bool]$preflight.ready_for_vhd_certification) -Message "Certification bundle VHD preflight ready mismatch"
    Assert-Condition -Condition ([bool]$bundle.summary.vhd_preflight.administrator -eq [bool]$preflight.administrator) -Message "Certification bundle VHD preflight administrator mismatch"
    Assert-Condition -Condition (@($bundle.summary.vhd_preflight.blockers).Count -eq @($preflight.blockers).Count) -Message "Certification bundle VHD preflight blocker count mismatch"

    Write-Host "Partition Manager certification artifact bundle passed: $($artifacts.Count) artifacts verified."
}
finally {
    Pop-Location
}

<#
.SYNOPSIS
    Works out what Partition Manager's certification evidence supports, and how to say it.

.DESCRIPTION
    Reads certification/PARTITION_MANAGER_CERTIFICATION_MATRIX.json and the one run of evidence the
    index at -EvidenceRoot names, checks each row with partition_manager_row_evidence.ps1, and
    computes the claim level, what each row ran on, and the words the site may use.

    Older evidence in the repository is not selectable: only the index is read, and it names one
    run. It stays in git as history.

    The levels, each saying what ran on what:
      FailedEvidence            a row in the selected run failed.
      CodeCompleteOnly          not every disposable-VHD row is accepted.
      VhdDataDiskCertified      every disposable-VHD row, not every other row. Virtual disks in a VM.
      VirtualMachinesValidated  every row, with at least one that needs a real device run on a
                                virtual one. Nothing here may read as a statement about hardware.
      NamedDevicesValidated     every row, with each that needs a real device run on one, named.
                                It says those devices, and no others.
#>

[CmdletBinding()]
param(
    [string]$EvidenceRoot = 'artifacts\partition-manager-certification\evidence\current',
    [string]$CodeDigest = '',
    [string]$MatrixPath = '',
    [string]$OutputPath = '',
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'partition_manager_row_evidence.ps1')
. (Join-Path $PSScriptRoot 'partition_manager_claim_wording.ps1')

$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($MatrixPath)) {
    $MatrixPath = Join-Path $projectRoot 'certification\PARTITION_MANAGER_CERTIFICATION_MATRIX.json'
}
$matrix = Get-Content -LiteralPath $MatrixPath -Raw | ConvertFrom-Json
if ($matrix.schema_version -ne 2) {
    throw "Unsupported Partition Manager certification matrix schema_version: $($matrix.schema_version)"
}

function Resolve-StatusPath {
    param([string]$Path)
    if ([IO.Path]::IsPathRooted($Path)) { return $Path }
    return (Join-Path $projectRoot $Path)
}

if ([string]::IsNullOrWhiteSpace($CodeDigest)) {
    $described = & python (Join-Path $PSScriptRoot 'partition_manager_code_digest.py') --root $projectRoot
    if ($LASTEXITCODE -ne 0) { throw 'The code digest could not be worked out' }
    $CodeDigest = ($described | ConvertFrom-Json).code_digest
}

$evidencePath = Resolve-StatusPath -Path $EvidenceRoot
$indexPath = Join-Path $evidencePath 'index.json'
$index = $null
$indexReasons = @()
if (Test-Path -LiteralPath $indexPath -PathType Leaf) {
    $index = Get-Content -LiteralPath $indexPath -Raw | ConvertFrom-Json
    $indexReasons = @(Test-PmEvidenceIndex -Index $index -EvidenceRoot $evidencePath -Matrix $matrix)
}

function Get-RowVerdict {
    param([object]$Spec)
    $rowDir = Join-Path $evidencePath "$($Spec.id)"
    $listed = $null -ne $index -and $indexReasons.Count -eq 0 -and
        @($index.rows | Where-Object { "$($_.id)" -ceq "$($Spec.id)" }).Count -eq 1
    $physicalClaim = [bool]$Spec.ran_on.physical_claim
    if (-not $listed) {
        $why = if ($null -eq $index) { 'no evidence has been selected' } elseif ($indexReasons.Count -gt 0) { $indexReasons -join '; ' } else { 'the selected run has no evidence for this row' }
        return [ordered]@{ id = "$($Spec.id)"; name = "$($Spec.name)"; accepted = $false; failed = $false; physical_claim = $physicalClaim; reasons = @($why); ran_on = $null }
    }
    $reasons = @(Test-PmRowEvidence -Spec $Spec -RowDir $rowDir -CurrentCodeDigest $CodeDigest)
    $report = Get-Content -LiteralPath (Join-Path $rowDir 'report.json') -Raw | ConvertFrom-Json
    return [ordered]@{
        id = "$($Spec.id)"
        name = "$($Spec.name)"
        accepted = ($reasons.Count -eq 0)
        failed = ("$($report.status)" -ceq 'Failed')
        physical_claim = $physicalClaim
        reasons = @($reasons)
        ran_on = (Get-PmRanOn -RowDir $rowDir)
    }
}

$vhdRows = @($matrix.vhd_scenarios | ForEach-Object { Get-RowVerdict -Spec $_ })
$externalRows = @($matrix.external_gates | ForEach-Object { Get-RowVerdict -Spec $_ })
$allRows = @($vhdRows + $externalRows)
$vhdIncomplete = @($vhdRows | Where-Object { -not $_.accepted } | ForEach-Object { $_.id })
$externalIncomplete = @($externalRows | Where-Object { -not $_.accepted } | ForEach-Object { $_.id })
$failedIds = @($allRows | Where-Object { $_.failed } | ForEach-Object { $_.id })
$virtualWhereItMatters = @($allRows | Where-Object { $_.accepted -and $_.physical_claim -and "$($_.ran_on.class)" -cne 'physical' } | ForEach-Object { $_.id })

$claimLevel = if ($failedIds.Count -gt 0) { 'FailedEvidence' }
elseif ($vhdIncomplete.Count -gt 0) { 'CodeCompleteOnly' }
elseif ($externalIncomplete.Count -gt 0) { 'VhdDataDiskCertified' }
elseif ($virtualWhereItMatters.Count -gt 0) { 'VirtualMachinesValidated' }
else { 'NamedDevicesValidated' }

$incompleteReasons = [ordered]@{}
foreach ($row in @($allRows | Where-Object { -not $_.accepted })) { $incompleteReasons[$row.id] = @($row.reasons) }

$version = (Get-Content -LiteralPath (Join-Path $projectRoot 'VERSION') -Raw).Trim()
$appleMatrix = Get-Content -LiteralPath (Join-Path $projectRoot 'certification\APPLE_FILESYSTEM_CAPABILITY_MATRIX.json') -Raw | ConvertFrom-Json

$summary = [ordered]@{
    tool = 'partition-manager-certification-status'
    schema_version = 2
    generated_utc = (Get-Date).ToUniversalTime().ToString('o')
    claim_level = $claimLevel
    code_digest = $CodeDigest
    evidence_root = "$EvidenceRoot".Replace('\', '/')
    evidence_commit = if ($null -ne $index) { "$($index.code_commit)" } else { '' }
    evidence_completed_utc = if ($null -ne $index) { "$($index.completed_utc)" } else { '' }
    vhd_data_disk = [ordered]@{ passed = $vhdRows.Count - $vhdIncomplete.Count; required = $vhdRows.Count; incomplete_ids = @($vhdIncomplete) }
    external_gates = [ordered]@{ passed = $externalRows.Count - $externalIncomplete.Count; required = $externalRows.Count; incomplete_ids = @($externalIncomplete) }
    failed_ids = @($failedIds)
    virtual_where_a_device_matters = @($virtualWhereItMatters)
    incomplete_reasons = $incompleteReasons
    rows = @($allRows | ForEach-Object { [ordered]@{ id = $_.id; accepted = $_.accepted; ran_on = $_.ran_on } })
    site_wording = (Get-PmSiteWording -ClaimLevel $claimLevel -Version $version -Rows $allRows -VhdRowCount $vhdRows.Count -AppleMatrix $appleMatrix)
}

$json = $summary | ConvertTo-Json -Depth 12
if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
    $resolved = Resolve-StatusPath -Path $OutputPath
    $parent = Split-Path -Parent $resolved
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [IO.File]::WriteAllText($resolved, $json)
}
if (-not $Quiet) {
    Write-Host "Partition Manager certification claim level: $claimLevel"
    Write-Output $json
}

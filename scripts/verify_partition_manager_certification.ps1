<#
.SYNOPSIS
    Fails unless the selected Partition Manager evidence holds, row by row.

.DESCRIPTION
    Uses the same guards the claim level does (partition_manager_row_evidence.ps1) and stops on any
    row of the selected run that does not hold. -RequireVhdDataDiskEvidence and
    -RequireExternalGateEvidence turn a missing row into a failure as well.

    It reads the source digest, not a binary hash, so a clean clone can run it: the digest covers
    every code file, CRLF read as LF, and the same commit gives the same digest wherever it is
    checked out. It also checks that the commit the evidence names is in this checkout's history,
    so evidence cannot be carried over from a tree that merely hashes alike.
#>

[CmdletBinding()]
param(
    [string]$EvidenceRoot = 'artifacts\partition-manager-certification\evidence\current',
    [switch]$RequireVhdDataDiskEvidence,
    [switch]$RequireExternalGateEvidence
)

$ErrorActionPreference = 'Stop'
$status = & (Join-Path $PSScriptRoot 'get_partition_manager_certification_status.ps1') -EvidenceRoot $EvidenceRoot -Quiet:$false | Select-Object -Skip 0 | Out-String
$parsed = $status.Substring($status.IndexOf('{')) | ConvertFrom-Json

$broken = @()
foreach ($property in $parsed.incomplete_reasons.PSObject.Properties) {
    $why = @($property.Value)
    if ($why.Count -eq 1 -and "$($why[0])" -match '^(no evidence has been selected|the selected run has no evidence for this row)$') { continue }
    $broken += "$($property.Name): $($why -join '; ')"
}
if ($broken.Count -gt 0) { throw "Partition Manager evidence does not hold:`n$($broken -join "`n")" }

if ($parsed.evidence_commit) {
    $root = Split-Path -Parent $PSScriptRoot
    & git -C $root merge-base --is-ancestor $parsed.evidence_commit HEAD 2>$null
    if ($LASTEXITCODE -ne 0) { throw "The evidence names commit $($parsed.evidence_commit), which is not in this checkout's history." }
}
if ($RequireVhdDataDiskEvidence -and $parsed.vhd_data_disk.passed -ne $parsed.vhd_data_disk.required) {
    throw "Disposable-VHD evidence is incomplete: $(@($parsed.vhd_data_disk.incomplete_ids) -join ', ')"
}
if ($RequireExternalGateEvidence -and $parsed.external_gates.passed -ne $parsed.external_gates.required) {
    throw "VM and device evidence is incomplete: $(@($parsed.external_gates.incomplete_ids) -join ', ')"
}
Write-Host "Partition Manager evidence holds for claim level $($parsed.claim_level): $($parsed.vhd_data_disk.passed)/$($parsed.vhd_data_disk.required) virtual disk rows, $($parsed.external_gates.passed)/$($parsed.external_gates.required) other rows."

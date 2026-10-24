<#
.SYNOPSIS
    Generates, or checks, the certification state block in docs/PARTITION_MANAGER_CERTIFICATION.md.

.DESCRIPTION
    certification/*.json and the claim level computed from it by
    get_partition_manager_certification_status.ps1 are the only certification truth (AGENTS.md,
    "Where truth lives"). This script renders the block between the BEGIN/END GENERATED markers
    under "## Current Certification State" from three inputs and nothing else:

      - the computed status JSON (claim level, accepted counts, incomplete IDs),
      - certification/PARTITION_MANAGER_CERTIFICATION_MATRIX.json (every scenario ID and name),
      - certification/APPLE_FILESYSTEM_CAPABILITY_MATRIX.json (every capability row and status).

    No word inside the block is hand-written, so an operation the evidence does not certify
    cannot be shown there as certified: the claim level, every accepted/not-accepted cell and
    every caveat sentence is a function of the status, and the status is validated against the
    matrix before anything is rendered. Prose outside the block is history and is not parsed.

    Default mode checks: it renders the block and fails if the document differs (the T3 gate).
    -Update rewrites the block in place. Both modes refuse a status that contradicts the matrix
    or its own incomplete sets before rendering, so an invalid status can never be written.

    This replaced (2026-10-24, owner ruling) a regex scan of README, changelog, plan,
    certification and readiness prose for unsupported wording, which tested a document's
    phrasing rather than the evidence.
#>

[CmdletBinding()]
param(
    [string]$StatusPath = "",
    [string]$CertificationRoot = "artifacts\partition-manager-certification\vhd-strict",
    [string]$ExternalEvidenceManifest = "",
    [string]$DocumentPath = "docs\PARTITION_MANAGER_CERTIFICATION.md",
    [switch]$Update
)

$ErrorActionPreference = "Stop"

$Script:BeginMarker = '<!-- BEGIN GENERATED certification-state: do not edit by hand. Regenerate with ' +
    'scripts/check_partition_manager_release_claims.ps1 -Update -->'
$Script:EndMarker = '<!-- END GENERATED certification-state -->'
$Script:KnownClaimLevels = @("FailedEvidence", "CodeCompleteOnly", "VhdDataDiskCertified", "HardwareCertified")

function Resolve-ProjectRoot {
    return Split-Path -Parent (Split-Path -Parent $PSCommandPath)
}

function Read-JsonFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    return Get-Content -LiteralPath $resolved.Path -Raw | ConvertFrom-Json
}

function Resolve-StatusPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ProjectRoot
    )

    if (-not [string]::IsNullOrWhiteSpace($StatusPath)) {
        return (Resolve-Path -LiteralPath $StatusPath -ErrorAction Stop).Path
    }

    # External evidence is an explicit authority choice, never inferred from old files.
    $statusOutputPath = Join-Path $ProjectRoot "artifacts\partition-manager-certification\claim-check-status.json"
    $arguments = @{
        CertificationRoot = $CertificationRoot
        OutputPath = $statusOutputPath
        Quiet = $true
    }
    if (-not [string]::IsNullOrWhiteSpace($ExternalEvidenceManifest)) {
        $arguments.ExternalEvidenceManifest = $ExternalEvidenceManifest
    }

    # Delete any prior status file first so a generator run that exits without writing
    # (or that leaves an older file in place) cannot make the checker consume a stale
    # claim level. The existence check afterward fails closed if nothing was written.
    if (Test-Path -LiteralPath $statusOutputPath) {
        Remove-Item -LiteralPath $statusOutputPath -Force
    }

    & (Join-Path $ProjectRoot "scripts\get_partition_manager_certification_status.ps1") @arguments
    if (-not $?) {
        throw "Could not compute Partition Manager certification status"
    }
    if (-not (Test-Path -LiteralPath $statusOutputPath -PathType Leaf)) {
        throw "Partition Manager certification status was not generated at $statusOutputPath"
    }
    return $statusOutputPath
}

function Assert-StatusHeader {
    param([Parameter(Mandatory = $true)][object]$Status)
    if ($Status.tool -ne "partition-manager-certification-status") {
        throw "Unexpected Partition Manager certification status tool: $($Status.tool)"
    }
    if (($Status.schema_version -isnot [int] -and $Status.schema_version -isnot [long]) -or $Status.schema_version -ne 1) {
        throw 'Unsupported Partition Manager certification status schema_version'
    }
    # Fail closed on an unrecognized or wrong-typed claim_level (an array/number, or a forged
    # value). These are the exact levels emitted by get_partition_manager_certification_status.ps1.
    if ($Status.claim_level -isnot [string] -or $Script:KnownClaimLevels -cnotcontains $Status.claim_level) {
        throw "Partition Manager certification status has an unrecognized claim_level: $($Status.claim_level)"
    }
}

function Get-ClaimEvidenceGroup {
    param([object]$Group, [string[]]$ExpectedIds, [string]$Name)
    if ($null -eq $Group -or $ExpectedIds.Count -eq 0 -or
        @($ExpectedIds | Sort-Object -Unique).Count -ne $ExpectedIds.Count) {
        throw "Invalid certification status or matrix group: $Name"
    }
    foreach ($field in @('passed', 'required')) {
        $property = $Group.PSObject.Properties[$field]
        if ($null -eq $property -or
            ($property.Value -isnot [int] -and $property.Value -isnot [long]) -or $property.Value -lt 0) {
            throw "Certification status $Name $field must be a nonnegative integer"
        }
    }
    if ($Group.required -ne $ExpectedIds.Count -or $Group.passed -gt $Group.required) {
        throw "Certification status $Name counts do not match the certification matrix"
    }
    $incomplete = $Group.PSObject.Properties['incomplete_ids']
    if ($null -eq $incomplete -or $incomplete.Value -isnot [Array]) {
        throw "Certification status $Name incomplete_ids must be an array"
    }
    $ids = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($id in $incomplete.Value) {
        if ($id -isnot [string] -or $ExpectedIds -cnotcontains $id -or -not $ids.Add($id)) {
            throw "Certification status $Name contains an unknown or duplicate incomplete ID"
        }
    }
    if ($Group.passed + $ids.Count -ne $Group.required) {
        throw "Certification status $Name passed plus incomplete does not equal required"
    }
    # Keep matrix order so the rendered block is deterministic.
    $ordered = @($ExpectedIds | Where-Object { $ids.Contains($_) })
    return [pscustomobject]@{ passed = $Group.passed; required = $Group.required; incomplete_ids = $ordered }
}

function Get-ValidatedState {
    param(
        [Parameter(Mandatory = $true)][object]$Status,
        [Parameter(Mandatory = $true)][object]$Matrix
    )
    Assert-StatusHeader $Status
    if (($Matrix.schema_version -isnot [int] -and $Matrix.schema_version -isnot [long]) -or $Matrix.schema_version -ne 1) {
        throw 'Unsupported Partition Manager certification matrix schema_version'
    }
    $vhdRows = @($Matrix.vhd_scenarios)
    $externalRows = @($Matrix.external_gates)
    $vhd = Get-ClaimEvidenceGroup $Status.vhd_data_disk @($vhdRows | ForEach-Object { [string]$_.id }) 'VHD'
    $external = Get-ClaimEvidenceGroup $Status.external_gates @($externalRows | ForEach-Object { [string]$_.id }) 'external'
    $hasVhdGaps = @($vhd.incomplete_ids).Count -gt 0
    $hasExternalGaps = @($external.incomplete_ids).Count -gt 0
    $level = [string]$Status.claim_level
    if (($level -ceq 'HardwareCertified' -and ($hasVhdGaps -or $hasExternalGaps)) -or
        ($level -ceq 'VhdDataDiskCertified' -and ($hasVhdGaps -or -not $hasExternalGaps)) -or
        ($level -ceq 'CodeCompleteOnly' -and -not $hasVhdGaps)) {
        throw 'Certification status claim level contradicts its incomplete evidence sets'
    }
    return [pscustomobject]@{
        level = $level
        vhd = $vhd
        external = $external
        vhd_rows = $vhdRows
        external_rows = $externalRows
        has_vhd_gaps = $hasVhdGaps
        has_external_gaps = $hasExternalGaps
    }
}

function Format-IdList {
    param([AllowEmptyCollection()][string[]]$Ids)
    if (@($Ids).Count -eq 0) { return 'none' }
    return (@($Ids) -join ', ')
}

function Get-CaveatLines {
    param([Parameter(Mandatory = $true)][object]$State)
    if ($State.level -ceq 'HardwareCertified') {
        return @('Every VHD scenario and every external VM/hardware/lab gate passed verification on the',
            'selected evidence.')
    }
    $reasons = @()
    if ($State.level -ceq 'FailedEvidence') { $reasons += 'evidence verification failed' }
    if ($State.has_vhd_gaps) { $reasons += 'VHD evidence is incomplete' }
    if ($State.has_external_gaps) { $reasons += 'VM/hardware/lab evidence is incomplete' }
    return @(
        ('On this evidence, destructive certification remains partial because ' + ($reasons -join ' and ') + '.'),
        'Do not mark the Partition Manager destructive paths fully certified until those reports exist.'
    )
}

function Get-ScenarioTableLines {
    param([object[]]$Rows, [string[]]$IncompleteIds, [string]$Kind)
    $lines = @("| $Kind | Name | Accepted on this evidence |", '|---|---|---|')
    foreach ($row in $Rows) {
        $accepted = if ($IncompleteIds -ccontains [string]$row.id) { 'no' } else { 'yes' }
        $lines += ('| `{0}` | {1} | {2} |' -f $row.id, $row.name, $accepted)
    }
    return $lines
}

function Get-AppleCapabilityLines {
    param([Parameter(Mandatory = $true)][object]$AppleMatrix)
    if (($AppleMatrix.schema_version -isnot [int] -and $AppleMatrix.schema_version -isnot [long]) -or
        $AppleMatrix.schema_version -ne 1) {
        throw 'Unsupported Apple filesystem capability matrix schema_version'
    }
    $lines = @('| Capability | Filesystem | Name | Status |', '|---|---|---|---|')
    foreach ($row in @($AppleMatrix.capabilities)) {
        $status = switch -CaseSensitive ([string]$row.status) {
            'certified' { "certified $($row.certified_on)" }
            'design_decision' { 'design decision: out of scope, fails closed' }
            default { throw "Apple capability $($row.id) has an unrenderable status: $($row.status)" }
        }
        $lines += ('| `{0}` | {1} | {2} | {3} |' -f $row.id, $row.filesystem, $row.name, $status)
    }
    return $lines
}

function Get-GeneratedBlock {
    param(
        [Parameter(Mandatory = $true)][object]$State,
        [Parameter(Mandatory = $true)][object]$AppleMatrix
    )
    $lines = @($Script:BeginMarker, '')
    $lines += ('Current claim level: `' + $State.level + '`.')
    $lines += "Accepted VHD evidence: $($State.vhd.passed)/$($State.vhd.required)."
    $lines += "Accepted external evidence: $($State.external.passed)/$($State.external.required)."
    $lines += "Incomplete VHD scenarios: $(Format-IdList $State.vhd.incomplete_ids)."
    $lines += "Incomplete external gates: $(Format-IdList $State.external.incomplete_ids)."
    $lines += ''
    $lines += Get-CaveatLines $State
    $lines += ''
    $lines += 'Disposable-VHD scenarios (`certification/PARTITION_MANAGER_CERTIFICATION_MATRIX.json`):'
    $lines += ''
    $lines += Get-ScenarioTableLines $State.vhd_rows $State.vhd.incomplete_ids 'Scenario'
    $lines += ''
    $lines += 'External VM/hardware/lab gates (same matrix):'
    $lines += ''
    $lines += Get-ScenarioTableLines $State.external_rows $State.external.incomplete_ids 'Gate'
    $lines += ''
    $lines += 'Apple filesystem driver capabilities (`certification/APPLE_FILESYSTEM_CAPABILITY_MATRIX.json`):'
    $lines += ''
    $lines += Get-AppleCapabilityLines $AppleMatrix
    $lines += ''
    $lines += $Script:EndMarker
    return $lines
}

function Get-BlockBounds {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string[]]$Lines)
    $headings = @(0..($Lines.Count - 1) | Where-Object { $Lines[$_].Trim() -ceq '## Current Certification State' })
    if ($headings.Count -ne 1) {
        throw 'Expected one Current Certification State heading in the certification document'
    }
    $begins = @(0..($Lines.Count - 1) | Where-Object { $Lines[$_] -ceq $Script:BeginMarker })
    $ends = @(0..($Lines.Count - 1) | Where-Object { $Lines[$_] -ceq $Script:EndMarker })
    if ($begins.Count -ne 1 -or $ends.Count -ne 1) {
        throw 'Expected exactly one BEGIN and one END generated certification-state marker'
    }
    $begin = $begins[0]
    $end = $ends[0]
    $nextHeading = @(($headings[0] + 1)..($Lines.Count - 1) | Where-Object { $Lines[$_] -match '^\s*#{1,6}\s' } |
        Select-Object -First 1)
    if ($begin -le $headings[0] -or $end -le $begin -or
        ($nextHeading.Count -eq 1 -and $end -gt $nextHeading[0])) {
        throw 'Generated certification-state markers must sit inside the Current Certification State section'
    }
    return [pscustomobject]@{ begin = $begin; end = $end }
}

$projectRoot = Resolve-ProjectRoot
Push-Location $projectRoot
try {
    $documentFullPath = [IO.Path]::GetFullPath((Join-Path $projectRoot $DocumentPath))
    $rootBoundary = [IO.Path]::GetFullPath($projectRoot).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $documentFullPath.StartsWith($rootBoundary, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Certification document escapes repository root: $DocumentPath"
    }
    if (-not (Test-Path -LiteralPath $documentFullPath -PathType Leaf)) {
        throw "Certification document missing: $DocumentPath"
    }

    $status = Read-JsonFile -Path (Resolve-StatusPath -ProjectRoot $projectRoot)
    $matrix = Read-JsonFile -Path (Join-Path $projectRoot 'certification/PARTITION_MANAGER_CERTIFICATION_MATRIX.json')
    $appleMatrix = Read-JsonFile -Path (Join-Path $projectRoot 'certification/APPLE_FILESYSTEM_CAPABILITY_MATRIX.json')
    $state = Get-ValidatedState -Status $status -Matrix $matrix
    $generated = @(Get-GeneratedBlock -State $state -AppleMatrix $appleMatrix)

    $raw = [IO.File]::ReadAllText($documentFullPath)
    $newline = if ($raw.Contains("`r`n")) { "`r`n" } else { "`n" }
    $lines = @($raw -split "\r?\n")
    $bounds = Get-BlockBounds -Lines $lines
    $current = @($lines[$bounds.begin..$bounds.end])

    if ($Update) {
        $before = if ($bounds.begin -gt 0) { @($lines[0..($bounds.begin - 1)]) } else { @() }
        $after = if ($bounds.end -lt $lines.Count - 1) { @($lines[($bounds.end + 1)..($lines.Count - 1)]) } else { @() }
        $text = (@($before) + $generated + @($after)) -join $newline
        [IO.File]::WriteAllText($documentFullPath, $text, [Text.UTF8Encoding]::new($false))
        Write-Host "Partition Manager certification state block written for claim level: $($state.level)"
        return
    }

    $mismatch = -1
    $count = [Math]::Max($current.Count, $generated.Count)
    for ($index = 0; $index -lt $count; $index++) {
        $have = if ($index -lt $current.Count) { $current[$index] } else { '<missing>' }
        $want = if ($index -lt $generated.Count) { $generated[$index] } else { '<missing>' }
        if ($have -cne $want) { $mismatch = $index; break }
    }
    if ($mismatch -ge 0) {
        $have = if ($mismatch -lt $current.Count) { $current[$mismatch] } else { '<missing>' }
        $want = if ($mismatch -lt $generated.Count) { $generated[$mismatch] } else { '<missing>' }
        throw ("Generated certification state block in $DocumentPath is stale at block line " +
            "$($mismatch + 1): found '$have', expected '$want'. Regenerate with " +
            "scripts/check_partition_manager_release_claims.ps1 -Update.")
    }
    Write-Host "Partition Manager certification state block is current for claim level: $($state.level)"
}
finally {
    Pop-Location
}

<#
.SYNOPSIS
    Verifies Partition Manager release/documentation certification claims match evidence.

.DESCRIPTION
    This non-destructive gate reads the machine-readable certification status and
    scans release-facing documentation for unsupported certification claims. It
    also binds the authoritative current-state block to accepted evidence counts
    and exact incomplete gate IDs. Historical sections cannot satisfy current
    caveats, and completed gate groups are not described as still incomplete.
#>

[CmdletBinding()]
param(
    [string]$StatusPath = "",
    [string]$CertificationRoot = "artifacts\partition-manager-certification\vhd-strict",
    [string]$ExternalEvidenceManifest = "",
    [string[]]$ClaimFiles = @(
        "README.md",
        "CHANGELOG.md",
        "docs\archive\PARTITION_MANAGER_PANEL_PLAN.md",
        "docs\PARTITION_MANAGER_CERTIFICATION.md",
        "docs\RELEASE_READINESS.md"
    )
)

$ErrorActionPreference = "Stop"

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

function Get-ClaimLines {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ProjectRoot,
        [Parameter(Mandatory = $true)]
        [string[]]$Files
    )

    $claimLines = @()
    $rootBoundary = [System.IO.Path]::GetFullPath($ProjectRoot).TrimEnd('\', '/') +
        [System.IO.Path]::DirectorySeparatorChar
    foreach ($relativePath in $Files) {
        $path = Join-Path $ProjectRoot $relativePath
        # Fail closed on a claim file that escapes the repo root (e.g. a "..\" override):
        # an out-of-tree file could satisfy the caveat scan while the real release docs
        # go unchecked.
        $fullPath = [System.IO.Path]::GetFullPath($path)
        if (-not $fullPath.StartsWith($rootBoundary, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Partition Manager claim file escapes repository root: $relativePath"
        }
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Partition Manager claim file missing: $relativePath"
        }

        $lineNumber = 0
        foreach ($line in Get-Content -LiteralPath $path) {
            ++$lineNumber
            $claimLines += [pscustomobject]@{
                path = $relativePath
                line_number = $lineNumber
                text = $line
            }
        }
    }
    return $claimLines
}

function Test-AllowedCertificationContext {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Line
    )

    $allowedPattern =
        "(?i)(not|until|before|requires?|remain(?:s|ing)?|blocked|partial|gate|external|claim levels?|claim[-_ ]level|claim[-_ ]wording|emits|counts only|deterministic|do not mark|can say|when|without|incomplete|CodeCompleteOnly|certification matrix|certification-status|vhd_data_disk_certified|hardware_certified|every disposable-VHD)"
    return $Line -match $allowedPattern
}

function Assert-NoUnsupportedClaims {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$ClaimLines,
        [Parameter(Mandatory = $true)]
        [string]$ClaimLevel
    )

    $forbiddenPatterns = @()
    # VHD-certified wording is only supportable once the VHD scenarios pass (claim level
    # VhdDataDiskCertified, or the higher HardwareCertified). Forbid it at every other
    # level -- CodeCompleteOnly, FailedEvidence, or anything unexpected -- so a non-VHD
    # status can never sit beside a VHD-certified claim (fail closed).
    if ($ClaimLevel -ne "VhdDataDiskCertified" -and $ClaimLevel -ne "HardwareCertified") {
        $forbiddenPatterns += "(?i)\bVhdDataDiskCertified\b"
        $forbiddenPatterns += "(?i)\bVHD[- ]certified\b"
    }
    if ($ClaimLevel -ne "HardwareCertified") {
        $forbiddenPatterns += "(?i)\bHardwareCertified\b"
        $forbiddenPatterns += "(?i)\bhardware[- ]certified\b"
        $forbiddenPatterns += "(?i)\bfully certified\b"
    }

    foreach ($line in $ClaimLines) {
        foreach ($pattern in $forbiddenPatterns) {
            if ($line.text -match $pattern -and -not (Test-AllowedCertificationContext -Line $line.text)) {
                throw "Unsupported Partition Manager certification claim at $($line.path):$($line.line_number): $($line.text)"
            }
        }
    }
}

function Get-CurrentCertificationClaimLines {
    param(
        [Parameter(Mandatory = $true)][object[]]$ClaimLines
    )
    $document = @($ClaimLines | Where-Object {
        $_.path.Replace('\', '/') -ceq 'docs/PARTITION_MANAGER_CERTIFICATION.md'
    })
    $headings = @($document | Where-Object { $_.text.Trim() -ceq '## Current Certification State' })
    if ($headings.Count -ne 1) {
        throw 'Expected one Current Certification State heading in docs/PARTITION_MANAGER_CERTIFICATION.md'
    }
    $current = [Collections.Generic.List[object]]::new()
    foreach ($line in $document) {
        if ($line.line_number -le $headings[0].line_number) { continue }
        if ($line.text -match '^\s*#{1,6}\s') { break }
        $current.Add($line)
    }
    return $current.ToArray()
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
    return [pscustomobject]@{ passed = $Group.passed; required = $Group.required; incomplete_ids = @($ids) }
}

function Get-CurrentClaimValue {
    param([object[]]$Lines, [string]$Label)
    $prefix = $Label + ':'
    $values = @($Lines | ForEach-Object {
        $text = ($_.text -replace '\s+', ' ').Trim()
        if ($text.StartsWith($prefix, [StringComparison]::Ordinal)) {
            $text.Substring($prefix.Length).Trim().TrimEnd('.').Replace('`', '')
        }
    })
    if ($values.Count -ne 1) { throw "Current certification state requires exactly one '$Label' line" }
    return $values[0]
}

function Assert-CurrentClaimValue {
    param([object[]]$Lines, [string]$Label, [string]$Expected)
    $actual = Get-CurrentClaimValue $Lines $Label
    if ($actual -cne $Expected) {
        throw "Current certification state mismatch for '$Label': expected '$Expected', found '$actual'"
    }
}

function Assert-CurrentIncompleteIds {
    param([object[]]$Lines, [string]$Label, [AllowEmptyCollection()][string[]]$Expected)
    $value = Get-CurrentClaimValue $Lines $Label
    $actual = if ($value -ceq 'none') { @() } else { @($value -split ',' | ForEach-Object { $_.Trim() }) }
    if (@($actual).Count -ne $Expected.Count -or
        @($actual | Sort-Object -Unique).Count -ne @($actual).Count) {
        throw "Current certification state incomplete-ID set mismatch for '$Label'"
    }
    foreach ($id in $actual) {
        if ($Expected -cnotcontains $id) { throw "Current certification state incomplete-ID set mismatch for '$Label'" }
    }
}

function Assert-RequiredCaveats {
    param(
        [Parameter(Mandatory = $true)][object[]]$ClaimLines,
        [Parameter(Mandatory = $true)][object]$Status,
        [Parameter(Mandatory = $true)][object]$Matrix
    )
    $current = @(Get-CurrentCertificationClaimLines $ClaimLines)
    $vhd = Get-ClaimEvidenceGroup $Status.vhd_data_disk @($Matrix.vhd_scenarios | ForEach-Object { $_.id }) 'VHD'
    $external = Get-ClaimEvidenceGroup $Status.external_gates @($Matrix.external_gates | ForEach-Object { $_.id }) 'external'
    $hasVhdGaps = @($vhd.incomplete_ids).Count -gt 0
    $hasExternalGaps = @($external.incomplete_ids).Count -gt 0
    if (($Status.claim_level -ceq 'HardwareCertified' -and ($hasVhdGaps -or $hasExternalGaps)) -or
        ($Status.claim_level -ceq 'VhdDataDiskCertified' -and ($hasVhdGaps -or -not $hasExternalGaps)) -or
        ($Status.claim_level -ceq 'CodeCompleteOnly' -and -not $hasVhdGaps)) {
        throw 'Certification status claim level contradicts its incomplete evidence sets'
    }
    Assert-CurrentClaimValue $current 'Current claim level' $Status.claim_level
    Assert-CurrentClaimValue $current 'Accepted VHD evidence' "$($vhd.passed)/$($vhd.required)"
    Assert-CurrentClaimValue $current 'Accepted external evidence' "$($external.passed)/$($external.required)"
    Assert-CurrentIncompleteIds $current 'Incomplete VHD scenarios' @($vhd.incomplete_ids)
    Assert-CurrentIncompleteIds $current 'Incomplete external gates' @($external.incomplete_ids)

    $allText = (($current | ForEach-Object { $_.text }) -join "`n") -replace '\s+', ' '
    $required = @()
    if ($Status.claim_level -cne 'HardwareCertified') {
        $required = @(
            'destructive certification remains partial',
            'Do not mark the Partition Manager destructive paths fully certified until those reports exist'
        )
        if ($hasVhdGaps) { $required += 'VHD evidence is incomplete' }
        if ($hasExternalGaps) { $required += 'VM/hardware/lab evidence is incomplete' }
        if ($Status.claim_level -ceq 'FailedEvidence') { $required += 'evidence verification failed' }
    }
    $forbidden = @()
    if (-not $hasVhdGaps) {
        $forbidden += @('VHD evidence is incomplete', 'VHD or VM/hardware/lab evidence is incomplete')
    }
    if (-not $hasExternalGaps) {
        $forbidden += @('still require external VM/hardware/lab evidence',
            'External VM/hardware/lab gates remain incomplete',
            'VM/hardware/lab evidence is incomplete')
    }
    if ($Status.claim_level -ceq 'HardwareCertified') {
        $forbidden += @(
            'destructive VM certification remains a release gate', 'hardware certification remains blocked',
            'claims still wait on the full external VM/hardware/lab matrix', 'remaining 12 external VM/hardware/lab gates',
            'destructive certification remains partial', 'VHD evidence is incomplete',
            'VM/hardware/lab evidence is incomplete',
            'Do not mark the Partition Manager destructive paths fully certified until those reports exist'
        )
    }
    foreach ($claim in $current) {
        if ($claim.text -match '^\s*-\s*`[A-Za-z]+`\s*-') { continue }
        $line = $claim.text -replace '\s+', ' '
        foreach ($phrase in $forbidden) {
            if ($line.Contains($phrase)) {
                throw "Stale Partition Manager certification blocker in current state: $($claim.path):$($claim.line_number): $phrase"
            }
        }
    }
    foreach ($phrase in $required) {
        if (-not $allText.Contains($phrase)) {
            throw "Partition Manager certification caveat missing from current state: $phrase"
        }
    }
}

$projectRoot = Resolve-ProjectRoot
Push-Location $projectRoot
try {
    $resolvedStatusPath = Resolve-StatusPath -ProjectRoot $projectRoot
    $status = Read-JsonFile -Path $resolvedStatusPath
    if ($status.tool -ne "partition-manager-certification-status") {
        throw "Unexpected Partition Manager certification status tool: $($status.tool)"
    }
    if (($status.schema_version -isnot [int] -and $status.schema_version -isnot [long]) -or $status.schema_version -ne 1) {
        throw 'Unsupported Partition Manager certification status schema_version'
    }
    if ([string]::IsNullOrWhiteSpace($status.claim_level)) {
        throw "Partition Manager certification status missing claim_level"
    }
    # Fail closed on an unrecognized or wrong-typed claim_level (an array/number, or a
    # forged value): an unknown level would otherwise slip past the enum-specific
    # forbidden-claim and caveat checks below. These are the exact levels emitted by
    # get_partition_manager_certification_status.ps1.
    $knownClaimLevels = @("FailedEvidence", "CodeCompleteOnly", "VhdDataDiskCertified", "HardwareCertified")
    if ($status.claim_level -isnot [string] -or $knownClaimLevels -notcontains $status.claim_level) {
        throw "Partition Manager certification status has an unrecognized claim_level: $($status.claim_level)"
    }

    $claimLines = @(Get-ClaimLines -ProjectRoot $projectRoot -Files $ClaimFiles)
    $matrix = Read-JsonFile -Path (Join-Path $projectRoot 'certification/PARTITION_MANAGER_CERTIFICATION_MATRIX.json')
    if (($matrix.schema_version -isnot [int] -and $matrix.schema_version -isnot [long]) -or $matrix.schema_version -ne 1) {
        throw 'Unsupported Partition Manager certification matrix schema_version'
    }
    Assert-NoUnsupportedClaims -ClaimLines $claimLines -ClaimLevel $status.claim_level
    Assert-RequiredCaveats -ClaimLines $claimLines -Status $status -Matrix $matrix

    Write-Host "Partition Manager release claims passed for claim level: $($status.claim_level)"
}
finally {
    Pop-Location
}

<#
.SYNOPSIS
    Summarizes Partition Manager certification claim level.

.DESCRIPTION
    Reads a Partition Manager certification report plus an optional external
    VM/hardware/lab evidence manifest and emits a machine-readable claim level.
    This does not replace strict verification; it makes release-note wording
    deterministic.
#>

[CmdletBinding()]
param(
    [string]$ReportPath = "",
    [string]$CertificationRoot = "artifacts\partition-manager-certification",
    [string]$ExternalEvidenceManifest = "",
    [string]$OutputPath = "",
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"

function Resolve-ProjectRoot {
    return Split-Path -Parent (Split-Path -Parent $PSCommandPath)
}

function Read-CertificationMatrix {
    $matrixPath = Join-Path (Resolve-ProjectRoot) "docs\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
    $matrix = Get-Content -LiteralPath $matrixPath -Raw | ConvertFrom-Json
    if ($matrix.schema_version -ne 1) {
        throw "Unsupported Partition Manager certification matrix schema_version: $($matrix.schema_version)"
    }
    return $matrix
}

$CertificationMatrix = Read-CertificationMatrix
$ExpectedVhdScenarioIds = @($CertificationMatrix.vhd_scenarios | ForEach-Object { $_.id })
$ExpectedExternalGateIds = @($CertificationMatrix.external_gates | ForEach-Object { $_.id })

function Resolve-LatestCertificationReport {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    $resolvedRoot = Resolve-Path -LiteralPath $Root -ErrorAction Stop
    $reports = @(Get-ChildItem -LiteralPath $resolvedRoot.Path -Recurse -Filter "partition-manager-certification-report.json" |
        Sort-Object LastWriteTimeUtc -Descending)
    if ($reports.Count -eq 0) {
        throw "No Partition Manager certification reports found under $($resolvedRoot.Path)"
    }

    return $reports[0].FullName
}

function Read-JsonFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    return Get-Content -LiteralPath $resolved.Path -Raw | ConvertFrom-Json
}

function Test-EvidenceReference {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Result,
        [Parameter(Mandatory = $true)]
        [string]$ManifestDirectory,
        [Parameter(Mandatory = $true)]
        [string]$ProjectRoot
    )

    $hasPath = -not [string]::IsNullOrWhiteSpace($Result.evidence_path)
    $hasUrl = -not [string]::IsNullOrWhiteSpace($Result.evidence_url)
    if (-not $hasPath -and -not $hasUrl) {
        return $false
    }

    if ($hasPath) {
        $candidatePaths = @()
        if ([System.IO.Path]::IsPathRooted($Result.evidence_path)) {
            $candidatePaths += $Result.evidence_path
        }
        else {
            $candidatePaths += Join-Path $ProjectRoot $Result.evidence_path
            $candidatePaths += Join-Path $ManifestDirectory $Result.evidence_path
        }

        foreach ($candidate in $candidatePaths) {
            if (Test-Path -LiteralPath $candidate) {
                return $true
            }
        }
    }

    if ($hasUrl) {
        $uri = $null
        $validUri = [System.Uri]::TryCreate($Result.evidence_url, [System.UriKind]::Absolute, [ref]$uri)
        return ($validUri -and ($uri.Scheme -eq "https" -or $uri.Scheme -eq "http"))
    }

    return $false
}

function Get-ResultById {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Results,
        [Parameter(Mandatory = $true)]
        [string]$Id
    )

    return @($Results | Where-Object { $_.id -eq $Id })
}

function Get-ScenarioSpec {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Id
    )

    $matches = @($CertificationMatrix.vhd_scenarios + $CertificationMatrix.external_gates |
        Where-Object { $_.id -eq $Id })
    if ($matches.Count -eq 0) {
        return $null
    }
    return $matches[0]
}

function Test-ObjectPropertyExists {
    param(
        [object]$Object,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    if ($null -eq $Object) {
        return $false
    }
    return $null -ne $Object.PSObject.Properties[$Name]
}

function Test-EvidenceValuePresent {
    param(
        [object]$Value
    )

    if ($null -eq $Value) {
        return $false
    }
    if ($Value -is [string]) {
        return -not [string]::IsNullOrWhiteSpace($Value)
    }
    if ($Value -is [System.Array]) {
        return $Value.Count -gt 0
    }
    if ($Value -is [pscustomobject]) {
        return @($Value.PSObject.Properties).Count -gt 0
    }

    return $true
}

function Test-EvidencePropertyComplete {
    param(
        [object]$Object,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    if (-not (Test-ObjectPropertyExists -Object $Object -Name $Name)) {
        return $false
    }
    return Test-EvidenceValuePresent -Value $Object.PSObject.Properties[$Name].Value
}

function ConvertTo-EvidenceValueStrings {
    param(
        [object]$Value
    )

    if ($null -eq $Value) {
        return @()
    }
    if ($Value -is [System.Array]) {
        return @($Value | ForEach-Object { $_.ToString() })
    }
    return @($Value.ToString())
}

function Get-EvidenceValueRequirement {
    param(
        [Parameter(Mandatory = $true)]
        [object]$ScenarioSpec,
        [Parameter(Mandatory = $true)]
        [string]$Key
    )

    if ($null -eq $ScenarioSpec.PSObject.Properties["required_evidence_values"]) {
        return $null
    }
    $property = $ScenarioSpec.required_evidence_values.PSObject.Properties[$Key]
    if ($null -eq $property) {
        return $null
    }
    return $property.Value
}

function Test-EvidenceValueRequirement {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Evidence,
        [Parameter(Mandatory = $true)]
        [string]$Key,
        [Parameter(Mandatory = $true)]
        [object]$Requirement
    )

    if (-not (Test-ObjectPropertyExists -Object $Evidence -Name $Key)) {
        return $false
    }

    $actualValues = ConvertTo-EvidenceValueStrings -Value $Evidence.PSObject.Properties[$Key].Value
    if ($null -ne $Requirement.PSObject.Properties["allowed_values"]) {
        $allowedValues = @($Requirement.allowed_values | ForEach-Object { $_.ToString() })
        if (@($actualValues | Where-Object { $allowedValues -contains $_ }).Count -eq 0) {
            return $false
        }
    }
    if ($null -ne $Requirement.PSObject.Properties["contains_all"]) {
        $requiredValues = @($Requirement.contains_all | ForEach-Object { $_.ToString() })
        foreach ($requiredValue in $requiredValues) {
            if ($actualValues -notcontains $requiredValue) {
                return $false
            }
        }
    }

    return $true
}

function Test-ScenarioContractComplete {
    param(
        [object]$Result
    )

    $spec = Get-ScenarioSpec -Id $Result.id
    if ($null -eq $spec -or $Result.name -ne $spec.name) {
        return $false
    }

    $actualKeys = @($Result.required_evidence_keys | ForEach-Object { $_.ToString() })
    foreach ($key in @($spec.required_evidence_keys)) {
        if ($actualKeys -notcontains $key) {
            return $false
        }
    }

    $actualSafety = @($Result.safety_contract | ForEach-Object { $_.ToString() })
    foreach ($item in @($spec.safety_contract)) {
        if ($actualSafety -notcontains $item) {
            return $false
        }
    }

    if ($null -ne $spec.PSObject.Properties["required_evidence_values"]) {
        if ($null -eq $Result.PSObject.Properties["required_evidence_values"] -or $null -eq $Result.required_evidence_values) {
            return $false
        }
        foreach ($requirementProperty in @($spec.required_evidence_values.PSObject.Properties)) {
            $actualProperty = $Result.required_evidence_values.PSObject.Properties[$requirementProperty.Name]
            if ($null -eq $actualProperty) {
                return $false
            }
            if ($null -ne $requirementProperty.Value.PSObject.Properties["allowed_values"]) {
                $actualAllowed = @($actualProperty.Value.allowed_values | ForEach-Object { $_.ToString() })
                foreach ($allowedValue in @($requirementProperty.Value.allowed_values)) {
                    if ($actualAllowed -notcontains $allowedValue.ToString()) {
                        return $false
                    }
                }
            }
            if ($null -ne $requirementProperty.Value.PSObject.Properties["contains_all"]) {
                $actualRequired = @($actualProperty.Value.contains_all | ForEach-Object { $_.ToString() })
                foreach ($requiredValue in @($requirementProperty.Value.contains_all)) {
                    if ($actualRequired -notcontains $requiredValue.ToString()) {
                        return $false
                    }
                }
            }
        }
    }

    return $true
}

function Test-VhdEvidenceComplete {
    param(
        [object]$Result
    )

    if (-not (Test-ScenarioContractComplete -Result $Result)) {
        return $false
    }
    $spec = Get-ScenarioSpec -Id $Result.id
    foreach ($key in @($spec.required_evidence_keys)) {
        if (-not (Test-EvidencePropertyComplete -Object $Result.evidence -Name $key)) {
            return $false
        }
        $requirement = Get-EvidenceValueRequirement -ScenarioSpec $spec -Key $key
        if ($null -ne $requirement -and -not (Test-EvidenceValueRequirement -Evidence $Result.evidence -Key $key -Requirement $requirement)) {
            return $false
        }
    }
    return $true
}

function Test-ExternalEvidenceComplete {
    param(
        [object]$Result,
        [Parameter(Mandatory = $true)]
        [string]$ManifestDirectory,
        [Parameter(Mandatory = $true)]
        [string]$ProjectRoot
    )

    if (-not (Test-ScenarioContractComplete -Result $Result)) {
        return $false
    }
    if (-not (Test-EvidenceReference -Result $Result -ManifestDirectory $ManifestDirectory -ProjectRoot $ProjectRoot)) {
        return $false
    }

    $spec = Get-ScenarioSpec -Id $Result.id
    foreach ($key in @($spec.required_evidence_keys)) {
        if (-not (Test-EvidencePropertyComplete -Object $Result.evidence -Name $key)) {
            return $false
        }
        $requirement = Get-EvidenceValueRequirement -ScenarioSpec $spec -Key $key
        if ($null -ne $requirement -and -not (Test-EvidenceValueRequirement -Evidence $Result.evidence -Key $key -Requirement $requirement)) {
            return $false
        }
    }
    return $true
}

$projectRoot = Resolve-ProjectRoot
Push-Location $projectRoot
try {
    if ([string]::IsNullOrWhiteSpace($ReportPath)) {
        $ReportPath = Resolve-LatestCertificationReport -Root (Join-Path $projectRoot $CertificationRoot)
    }

    $resolvedReportPath = (Resolve-Path -LiteralPath $ReportPath -ErrorAction Stop).Path
    $report = Read-JsonFile -Path $resolvedReportPath
    $reportResults = @($report.results)
    $failedIds = @($reportResults | Where-Object { $_.status -eq "Failed" } | ForEach-Object { $_.id })

    $passedVhdIds = @()
    $incompleteVhdIds = @()
    foreach ($id in $ExpectedVhdScenarioIds) {
        $matches = @(Get-ResultById -Results $reportResults -Id $id)
        if ($matches.Count -eq 1 -and $matches[0].status -eq "Passed" -and
            (Test-VhdEvidenceComplete -Result $matches[0])) {
            $passedVhdIds += $id
        }
        else {
            $incompleteVhdIds += $id
        }
    }

    $externalManifestPath = ""
    $passedExternalIds = @()
    $incompleteExternalIds = @($ExpectedExternalGateIds)
    if (-not [string]::IsNullOrWhiteSpace($ExternalEvidenceManifest)) {
        $externalManifestPath = (Resolve-Path -LiteralPath $ExternalEvidenceManifest -ErrorAction Stop).Path
        $manifest = Read-JsonFile -Path $externalManifestPath
        $manifestDirectory = Split-Path -Parent $externalManifestPath
        $externalResults = @($manifest.results)
        $passedExternalIds = @()
        $incompleteExternalIds = @()
        foreach ($id in $ExpectedExternalGateIds) {
            $matches = @(Get-ResultById -Results $externalResults -Id $id)
            if ($matches.Count -eq 1 -and $matches[0].status -eq "Passed" -and
                (Test-ExternalEvidenceComplete -Result $matches[0] -ManifestDirectory $manifestDirectory -ProjectRoot $projectRoot)) {
                $passedExternalIds += $id
            }
            else {
                $incompleteExternalIds += $id
            }
        }
    }

    $vhdCertified = ($incompleteVhdIds.Count -eq 0)
    $hardwareCertified = ($vhdCertified -and $incompleteExternalIds.Count -eq 0)
    $claimLevel = if ($failedIds.Count -gt 0 -or $report.status -eq "Failed") {
        "FailedEvidence"
    }
    elseif ($hardwareCertified) {
        "HardwareCertified"
    }
    elseif ($vhdCertified) {
        "VhdDataDiskCertified"
    }
    else {
        "CodeCompleteOnly"
    }

    $summary = [ordered]@{
        tool = "partition-manager-certification-status"
        schema_version = 1
        generated_utc = (Get-Date).ToUniversalTime().ToString("o")
        claim_level = $claimLevel
        claims = [ordered]@{
            code_complete_automated = ($claimLevel -ne "FailedEvidence")
            vhd_data_disk_certified = $vhdCertified
            hardware_certified = $hardwareCertified
        }
        report = [ordered]@{
            path = $resolvedReportPath
            status = $report.status
            failed_ids = $failedIds
        }
        vhd_data_disk = [ordered]@{
            passed = $passedVhdIds.Count
            required = $ExpectedVhdScenarioIds.Count
            incomplete_ids = $incompleteVhdIds
        }
        external_gates = [ordered]@{
            manifest_path = $externalManifestPath
            passed = $passedExternalIds.Count
            required = $ExpectedExternalGateIds.Count
            incomplete_ids = $incompleteExternalIds
        }
    }

    $json = $summary | ConvertTo-Json -Depth 8
    if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
        $resolvedOutputPath = if ([System.IO.Path]::IsPathRooted($OutputPath)) {
            $OutputPath
        }
        else {
            Join-Path $projectRoot $OutputPath
        }
        $parent = Split-Path -Parent $resolvedOutputPath
        if (-not [string]::IsNullOrWhiteSpace($parent)) {
            New-Item -ItemType Directory -Path $parent -Force | Out-Null
        }
        $json | Set-Content -LiteralPath $resolvedOutputPath -Encoding UTF8
    }

    if (-not $Quiet) {
        Write-Host "Partition Manager certification claim level: $claimLevel"
        Write-Output $json
    }
}
finally {
    Pop-Location
}

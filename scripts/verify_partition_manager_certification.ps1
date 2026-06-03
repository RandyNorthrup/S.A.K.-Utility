<#
.SYNOPSIS
    Verifies Partition Manager certification report evidence.

.DESCRIPTION
    This verifier keeps code-complete evidence separate from destructive
    disposable-VHD evidence and external VM/hardware/lab evidence. Default mode
    validates report shape and failed scenarios. Strict switches turn skipped
    VHD or missing external evidence into release-blocking failures.
#>

[CmdletBinding()]
param(
    [string]$ReportPath = "",
    [string]$CertificationRoot = "artifacts\partition-manager-certification",
    [string]$ExternalEvidenceManifest = "",
    [switch]$RequireVhdDataDiskEvidence,
    [switch]$RequireExternalGateEvidence
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

function Assert-IsoTimestamp {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Value,
        [Parameter(Mandatory = $true)]
        [string]$FieldName
    )

    $parsed = [datetime]::MinValue
    $styles = [System.Globalization.DateTimeStyles]::RoundtripKind
    $valid = [datetime]::TryParse($Value,
        [System.Globalization.CultureInfo]::InvariantCulture,
        $styles,
        [ref]$parsed)
    Assert-Condition -Condition $valid -Message "Invalid timestamp for $FieldName`: $Value"
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
        throw "Certification matrix missing scenario: $Id"
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

function Assert-EvidenceValuePresent {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Evidence,
        [Parameter(Mandatory = $true)]
        [string]$Key,
        [Parameter(Mandatory = $true)]
        [string]$MessagePrefix
    )

    $property = $Evidence.PSObject.Properties[$Key]
    Assert-Condition -Condition ($null -ne $property) -Message "$MessagePrefix missing evidence key: $Key"
    Assert-Condition -Condition (Test-EvidenceValuePresent -Value $property.Value) -Message "$MessagePrefix missing evidence value: $Key"
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

function Assert-EvidenceValueRequirement {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Evidence,
        [Parameter(Mandatory = $true)]
        [string]$Key,
        [Parameter(Mandatory = $true)]
        [object]$Requirement,
        [Parameter(Mandatory = $true)]
        [string]$MessagePrefix
    )

    $property = $Evidence.PSObject.Properties[$Key]
    Assert-Condition -Condition ($null -ne $property) -Message "$MessagePrefix missing evidence key: $Key"
    $actualValues = ConvertTo-EvidenceValueStrings -Value $property.Value

    if ($null -ne $Requirement.PSObject.Properties["allowed_values"]) {
        $allowedValues = @($Requirement.allowed_values | ForEach-Object { $_.ToString() })
        $matched = @($actualValues | Where-Object { $allowedValues -contains $_ }).Count -gt 0
        Assert-Condition -Condition $matched -Message "$MessagePrefix evidence value for $Key must be one of: $($allowedValues -join ', ')"
    }

    if ($null -ne $Requirement.PSObject.Properties["contains_all"]) {
        $requiredValues = @($Requirement.contains_all | ForEach-Object { $_.ToString() })
        foreach ($requiredValue in $requiredValues) {
            Assert-Condition -Condition ($actualValues -contains $requiredValue) -Message "$MessagePrefix evidence value for $Key missing required value: $requiredValue"
        }
    }
}

function Assert-StringArrayContains {
    param(
        [object[]]$Actual,
        [string[]]$Expected,
        [Parameter(Mandatory = $true)]
        [string]$MessagePrefix
    )

    $actualStrings = @($Actual | ForEach-Object { $_.ToString() })
    foreach ($expectedValue in $Expected) {
        Assert-Condition -Condition ($actualStrings -contains $expectedValue) -Message "$MessagePrefix missing '$expectedValue'"
    }
}

function Assert-ResultContract {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Result
    )

    $spec = Get-ScenarioSpec -Id $Result.id
    Assert-Condition -Condition ($Result.name -eq $spec.name) -Message "Scenario name mismatch for $($Result.id)"
    Assert-StringArrayContains -Actual @($Result.required_evidence_keys) -Expected @($spec.required_evidence_keys) -MessagePrefix "Required evidence contract for $($Result.id)"
    Assert-StringArrayContains -Actual @($Result.safety_contract) -Expected @($spec.safety_contract) -MessagePrefix "Safety contract for $($Result.id)"
    if ($null -ne $spec.PSObject.Properties["required_evidence_values"]) {
        Assert-Condition -Condition ($null -ne $Result.PSObject.Properties["required_evidence_values"] -and $null -ne $Result.required_evidence_values) -Message "Required evidence value contract missing for $($Result.id)"
        foreach ($requirementProperty in @($spec.required_evidence_values.PSObject.Properties)) {
            $actualProperty = $Result.required_evidence_values.PSObject.Properties[$requirementProperty.Name]
            Assert-Condition -Condition ($null -ne $actualProperty) -Message "Required evidence value contract for $($Result.id) missing '$($requirementProperty.Name)'"
            if ($null -ne $requirementProperty.Value.PSObject.Properties["allowed_values"]) {
                Assert-StringArrayContains -Actual @($actualProperty.Value.allowed_values) -Expected @($requirementProperty.Value.allowed_values) -MessagePrefix "Allowed evidence values for $($Result.id) $($requirementProperty.Name)"
            }
            if ($null -ne $requirementProperty.Value.PSObject.Properties["contains_all"]) {
                Assert-StringArrayContains -Actual @($actualProperty.Value.contains_all) -Expected @($requirementProperty.Value.contains_all) -MessagePrefix "Required evidence values for $($Result.id) $($requirementProperty.Name)"
            }
        }
    }
}

function Assert-PassedVhdEvidencePayload {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Result
    )

    $spec = Get-ScenarioSpec -Id $Result.id
    Assert-Condition -Condition ($null -ne $Result.evidence) -Message "Passed VHD scenario missing evidence payload: $($Result.id)"
    foreach ($key in @($spec.required_evidence_keys)) {
        Assert-EvidenceValuePresent -Evidence $Result.evidence -Key $key -MessagePrefix "Passed VHD scenario $($Result.id)"
        $requirement = Get-EvidenceValueRequirement -ScenarioSpec $spec -Key $key
        if ($null -ne $requirement) {
            Assert-EvidenceValueRequirement -Evidence $Result.evidence -Key $key -Requirement $requirement -MessagePrefix "Passed VHD scenario $($Result.id)"
        }
    }
}

function Assert-PassedExternalEvidencePayload {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Result
    )

    $spec = Get-ScenarioSpec -Id $Result.id
    Assert-Condition -Condition ($null -ne $Result.evidence) -Message "Passed external scenario missing evidence payload: $($Result.id)"
    foreach ($key in @($spec.required_evidence_keys)) {
        Assert-EvidenceValuePresent -Evidence $Result.evidence -Key $key -MessagePrefix "Passed external scenario $($Result.id)"
        $requirement = Get-EvidenceValueRequirement -ScenarioSpec $spec -Key $key
        if ($null -ne $requirement) {
            Assert-EvidenceValueRequirement -Evidence $Result.evidence -Key $key -Requirement $requirement -MessagePrefix "Passed external scenario $($Result.id)"
        }
    }
}

function Assert-ScenarioIdsPresent {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Results,
        [Parameter(Mandatory = $true)]
        [string[]]$ExpectedIds,
        [Parameter(Mandatory = $true)]
        [string]$GroupName
    )

    foreach ($id in $ExpectedIds) {
        $matches = @(Get-ResultById -Results $Results -Id $id)
        Assert-Condition -Condition ($matches.Count -eq 1) -Message "$GroupName scenario missing or duplicated: $id"
    }
}

function Assert-NoUnexpectedScenarioIds {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Results,
        [Parameter(Mandatory = $true)]
        [string[]]$ExpectedIds,
        [Parameter(Mandatory = $true)]
        [string]$GroupName
    )

    $unexpected = @($Results | Where-Object { $ExpectedIds -notcontains $_.id })
    if ($unexpected.Count -gt 0) {
        $ids = ($unexpected | ForEach-Object { $_.id }) -join ", "
        throw "$GroupName has unexpected scenario ID(s): $ids"
    }
}

function Assert-ResultMetadata {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Results,
        [Parameter(Mandatory = $true)]
        [string]$GroupName
    )

    foreach ($result in $Results) {
        Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace($result.id)) -Message "$GroupName result missing ID"
        Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace($result.name)) -Message "$GroupName result missing name: $($result.id)"
        Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace($result.status)) -Message "$GroupName result missing status: $($result.id)"
        Assert-ResultContract -Result $result
        if ($null -ne $result.completed_utc) {
            Assert-IsoTimestamp -Value $result.completed_utc -FieldName "$GroupName completed_utc for $($result.id)"
        }
    }
}

function Assert-ReportMetadata {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Report
    )

    Assert-Condition -Condition ($Report.schema_version -eq 1) -Message "Unexpected certification report schema_version: $($Report.schema_version)"
    Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace($Report.status)) -Message "Certification report missing status"
    Assert-IsoTimestamp -Value $Report.started_utc -FieldName "started_utc"
    Assert-IsoTimestamp -Value $Report.completed_utc -FieldName "completed_utc"
    Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace($Report.project_root)) -Message "Certification report missing project_root"
    Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace($Report.output_root)) -Message "Certification report missing output_root"
    Assert-Condition -Condition ($null -ne $Report.summary) -Message "Certification report missing summary"
}

function Assert-SummaryCounts {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Report,
        [Parameter(Mandatory = $true)]
        [object[]]$Results
    )

    $passed = @($Results | Where-Object { $_.status -eq "Passed" }).Count
    $failed = @($Results | Where-Object { $_.status -eq "Failed" }).Count
    $skipped = @($Results | Where-Object { $_.status -eq "Skipped" }).Count
    $external = @($Results | Where-Object { $_.status -eq "ExternalGate" }).Count

    Assert-Condition -Condition ([int]$Report.summary.passed -eq $passed) -Message "Passed summary count mismatch"
    Assert-Condition -Condition ([int]$Report.summary.failed -eq $failed) -Message "Failed summary count mismatch"
    Assert-Condition -Condition ([int]$Report.summary.skipped -eq $skipped) -Message "Skipped summary count mismatch"
    Assert-Condition -Condition ([int]$Report.summary.external_gates -eq $external) -Message "External gate summary count mismatch"
}

function Assert-NoFailedScenarios {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Results
    )

    $failed = @($Results | Where-Object { $_.status -eq "Failed" })
    if ($failed.Count -gt 0) {
        $ids = ($failed | ForEach-Object { $_.id }) -join ", "
        throw "Failed certification scenario(s): $ids"
    }
}

function Assert-VhdEvidence {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Report,
        [Parameter(Mandatory = $true)]
        [object[]]$Results
    )

    Assert-Condition -Condition ([bool]$Report.run_vhd_data_disk_matrix) -Message "VHD data-disk matrix was not requested"
    $notPassed = @($Results | Where-Object { $_.id -like "vhd.*" -and $_.status -ne "Passed" })
    if ($notPassed.Count -gt 0) {
        $details = ($notPassed | ForEach-Object { "$($_.id)=$($_.status)" }) -join ", "
        throw "VHD data-disk evidence incomplete: $details"
    }
}

function Assert-ExternalEvidenceManifest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ManifestPath,
        [bool]$RequirePassed = $false
    )

    $resolvedManifest = Resolve-Path -LiteralPath $ManifestPath -ErrorAction Stop
    $manifest = Read-JsonFile -Path $resolvedManifest.Path
    $manifestDirectory = Split-Path -Parent $resolvedManifest.Path
    Assert-Condition -Condition ($manifest.tool -eq "partition-manager-external-certification") -Message "Unexpected external evidence manifest tool"
    Assert-Condition -Condition ($manifest.schema_version -eq 1) -Message "Unexpected external evidence schema_version: $($manifest.schema_version)"
    Assert-IsoTimestamp -Value $manifest.created_utc -FieldName "external manifest created_utc"
    $results = @($manifest.results)
    Assert-Condition -Condition ($results.Count -gt 0) -Message "External evidence manifest has no results"
    Assert-ScenarioIdsPresent -Results $results -ExpectedIds $ExpectedExternalGateIds -GroupName "External"
    Assert-NoUnexpectedScenarioIds -Results $results -ExpectedIds $ExpectedExternalGateIds -GroupName "External evidence manifest"
    Assert-ResultMetadata -Results $results -GroupName "External evidence"

    $validExternalStatuses = @("Passed", "Pending", "Failed", "Blocked", "NotRun")
    foreach ($result in $results) {
        Assert-Condition -Condition ($validExternalStatuses -contains $result.status) -Message "Unexpected external evidence status for $($result.id): $($result.status)"
    }

    foreach ($id in $ExpectedExternalGateIds) {
        $result = @(Get-ResultById -Results $results -Id $id)[0]
        if ($RequirePassed -and $result.status -ne "Passed") {
            throw "External evidence incomplete: $id=$($result.status)"
        }
        if ($result.status -eq "Passed") {
            Assert-ExternalEvidenceReference -Result $result -ManifestDirectory $manifestDirectory
            Assert-PassedExternalEvidencePayload -Result $result
        }
    }
}

function Assert-ExternalEvidenceReference {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Result,
        [Parameter(Mandatory = $true)]
        [string]$ManifestDirectory
    )

    $hasPath = -not [string]::IsNullOrWhiteSpace($Result.evidence_path)
    $hasUrl = -not [string]::IsNullOrWhiteSpace($Result.evidence_url)
    if (-not $hasPath -and -not $hasUrl) {
        throw "External evidence missing artifact reference: $($Result.id)"
    }

    if ($hasPath) {
        $candidatePaths = @()
        if ([System.IO.Path]::IsPathRooted($Result.evidence_path)) {
            $candidatePaths += $Result.evidence_path
        }
        else {
            $candidatePaths += Join-Path (Resolve-ProjectRoot) $Result.evidence_path
            $candidatePaths += Join-Path $ManifestDirectory $Result.evidence_path
        }

        $exists = $false
        foreach ($candidate in $candidatePaths) {
            if (Test-Path -LiteralPath $candidate) {
                $exists = $true
                break
            }
        }
        Assert-Condition -Condition $exists -Message "External evidence path not found for $($Result.id): $($Result.evidence_path)"
    }

    if ($hasUrl) {
        $uri = $null
        $validUri = [System.Uri]::TryCreate($Result.evidence_url, [System.UriKind]::Absolute, [ref]$uri)
        Assert-Condition -Condition ($validUri -and ($uri.Scheme -eq "https" -or $uri.Scheme -eq "http")) -Message "External evidence URL is invalid for $($Result.id): $($Result.evidence_url)"
    }
}

$projectRoot = Resolve-ProjectRoot
Push-Location $projectRoot
try {
    if ([string]::IsNullOrWhiteSpace($ReportPath)) {
        $ReportPath = Resolve-LatestCertificationReport -Root (Join-Path $projectRoot $CertificationRoot)
    }

    $report = Read-JsonFile -Path $ReportPath
    Assert-Condition -Condition ($report.tool -eq "partition-manager-destructive-certification") -Message "Unexpected certification report tool"
    Assert-ReportMetadata -Report $report

    $validReportStatuses = @("Passed", "Partial", "NotRun", "Incomplete", "Failed")
    Assert-Condition -Condition ($validReportStatuses -contains $report.status) -Message "Unexpected certification report status: $($report.status)"

    $results = @($report.results)
    Assert-Condition -Condition ($results.Count -gt 0) -Message "Certification report has no results"
    $vhdResults = @($results | Where-Object { $_.id -like "vhd.*" })
    $currentExternalGateResults = @($results | Where-Object { $ExpectedExternalGateIds -contains $_.id })
    $unexpectedNonExternalResults = @($results | Where-Object {
            $_.id -notlike "vhd.*" -and $_.id -notlike "external.*"
        })

    Assert-ScenarioIdsPresent -Results $vhdResults -ExpectedIds $ExpectedVhdScenarioIds -GroupName "VHD"
    Assert-NoUnexpectedScenarioIds -Results $vhdResults -ExpectedIds $ExpectedVhdScenarioIds -GroupName "VHD"
    if ($unexpectedNonExternalResults.Count -gt 0) {
        $ids = ($unexpectedNonExternalResults | ForEach-Object { $_.id }) -join ", "
        throw "Certification report has unexpected non-external scenario ID(s): $ids"
    }
    Assert-ResultMetadata -Results ($vhdResults + $currentExternalGateResults) -GroupName "Certification report"

    $validScenarioStatuses = @("Passed", "Failed", "Skipped", "ExternalGate")
    foreach ($result in $results) {
        Assert-Condition -Condition ($validScenarioStatuses -contains $result.status) -Message "Unexpected scenario status for $($result.id): $($result.status)"
        if ($result.id -like "vhd.*" -and $result.status -eq "Passed") {
            Assert-PassedVhdEvidencePayload -Result $result
        }
    }

    Assert-SummaryCounts -Report $report -Results $results
    Assert-NoFailedScenarios -Results $results

    if ($RequireVhdDataDiskEvidence) {
        Assert-VhdEvidence -Report $report -Results $results
    }

    if ($RequireExternalGateEvidence) {
        Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace($ExternalEvidenceManifest)) -Message "External evidence manifest required"
        Assert-ExternalEvidenceManifest -ManifestPath $ExternalEvidenceManifest -RequirePassed $true
    }
    elseif (-not [string]::IsNullOrWhiteSpace($ExternalEvidenceManifest)) {
        Assert-ExternalEvidenceManifest -ManifestPath $ExternalEvidenceManifest
    }

    Write-Host "Partition Manager certification verification passed: $ReportPath"
}
finally {
    Pop-Location
}

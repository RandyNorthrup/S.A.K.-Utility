<#
.SYNOPSIS
    Verifies a Partition Manager certification artifact bundle manifest.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$BundlePath
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

# Every path in the manifest is untrusted bundle content. Reject UNC, device, and
# reparse-point targets so a forged manifest cannot point verification at a remote
# share (outbound authentication as the caller) or at an aliased file.
function Assert-LocalFilePath {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [AllowNull()]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Label
    )

    Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace($Path)) -Message "Certification bundle $Label path is empty"
    Assert-Condition -Condition ($Path -notmatch "[\x00-\x1f]") -Message "Certification bundle $Label path contains control characters"
    Assert-Condition -Condition ($Path -notmatch '^[\\/]{2}') -Message "Certification bundle $Label path is a UNC or device path: $Path"

    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    Assert-Condition -Condition ($resolved.Path -notmatch '^[\\/]{2}') -Message "Certification bundle $Label path resolves outside the local file system: $($resolved.Path)"

    $item = Get-Item -LiteralPath $resolved.Path -Force -ErrorAction Stop
    Assert-Condition -Condition ($item -is [System.IO.FileInfo]) -Message "Certification bundle $Label path is not a file: $($resolved.Path)"
    Assert-Condition -Condition ((($item.Attributes) -band [System.IO.FileAttributes]::ReparsePoint) -eq 0) -Message "Certification bundle $Label path is a reparse point: $($resolved.Path)"
    return $item
}

# JSON members are read through the typed accessors below so that a missing member,
# a wrong-typed member, or a numeric/boolean string can never coerce into a value
# that satisfies a closed-world check.
function Get-RequiredValue {
    param(
        $Object,
        [Parameter(Mandatory = $true)]
        [string]$MemberPath
    )

    $current = $Object
    foreach ($segment in $MemberPath.Split(".")) {
        Assert-Condition -Condition ($null -ne $current) -Message "Certification bundle member is missing: $MemberPath"
        Assert-Condition -Condition (@($current.PSObject.Properties.Name) -contains $segment) -Message "Certification bundle member is missing: $MemberPath"
        $current = $current.$segment
    }
    return , $current
}

function Get-RequiredInt64 {
    param(
        $Object,
        [Parameter(Mandatory = $true)]
        [string]$MemberPath
    )

    $value = Get-RequiredValue -Object $Object -MemberPath $MemberPath
    $isIntegral = ($value -is [int]) -or ($value -is [long]) -or ($value -is [int16]) -or ($value -is [byte])
    Assert-Condition -Condition $isIntegral -Message "Certification bundle member is not a JSON integer: $MemberPath"
    return [int64]$value
}

function Get-RequiredBool {
    param(
        $Object,
        [Parameter(Mandatory = $true)]
        [string]$MemberPath
    )

    $value = Get-RequiredValue -Object $Object -MemberPath $MemberPath
    Assert-Condition -Condition ($value -is [bool]) -Message "Certification bundle member is not a JSON boolean: $MemberPath"
    return [bool]$value
}

function Get-RequiredString {
    param(
        $Object,
        [Parameter(Mandatory = $true)]
        [string]$MemberPath
    )

    $value = Get-RequiredValue -Object $Object -MemberPath $MemberPath
    Assert-Condition -Condition ($value -is [string]) -Message "Certification bundle member is not a JSON string: $MemberPath"
    Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace($value)) -Message "Certification bundle member is empty: $MemberPath"
    return [string]$value
}

function Get-RequiredArrayCount {
    param(
        $Object,
        [Parameter(Mandatory = $true)]
        [string]$MemberPath
    )

    $value = Get-RequiredValue -Object $Object -MemberPath $MemberPath
    Assert-Condition -Condition ($value -is [System.Array]) -Message "Certification bundle member is not a JSON array: $MemberPath"
    return [int64]@($value).Count
}

function Read-JsonFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Label
    )

    $item = Assert-LocalFilePath -Path $Path -Label $Label
    return Get-Content -LiteralPath $item.FullName -Raw | ConvertFrom-Json
}

function Get-ArtifactPath {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [object[]]$Artifacts,
        [Parameter(Mandatory = $true)]
        [string]$Kind
    )

    $entries = @($Artifacts | Where-Object { ($_.kind -is [string]) -and ($_.kind -eq $Kind) })
    Assert-Condition -Condition ($entries.Count -eq 1) -Message "Certification bundle missing or duplicate artifact kind: $Kind"
    return (Get-RequiredString -Object $entries[0] -MemberPath "path")
}

function Get-CanonicalPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    return (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
}

Push-Location $ProjectRoot
try {
    $bundleItem = Assert-LocalFilePath -Path (Resolve-ProjectPath -Path $BundlePath) -Label "manifest"
    $bundle = Read-JsonFile -Path $bundleItem.FullName -Label "manifest"
    Assert-Condition -Condition ((Get-RequiredString -Object $bundle -MemberPath "tool") -eq "partition-manager-certification-artifact-bundle") -Message "Unexpected certification bundle tool"
    Assert-Condition -Condition ((Get-RequiredInt64 -Object $bundle -MemberPath "schema_version") -eq 1) -Message "Unexpected certification bundle schema_version"

    Assert-Condition -Condition (@($bundle.PSObject.Properties.Name) -contains "artifacts") -Message "Certification bundle member is missing: artifacts"
    Assert-Condition -Condition ($bundle.artifacts -is [System.Array]) -Message "Certification bundle member is not a JSON array: artifacts"
    $artifacts = @($bundle.artifacts)
    Assert-Condition -Condition ($artifacts.Count -gt 0) -Message "Certification bundle declares no artifacts"

    # A non-scalar kind would satisfy every required-kind filter at once, so one
    # forged entry could stand in as all seven artifacts.
    foreach ($artifact in $artifacts) {
        Assert-Condition -Condition ($artifact.kind -is [string]) -Message "Certification bundle artifact kind is not a scalar string"
        Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace($artifact.kind)) -Message "Certification bundle artifact kind is empty"
    }

    foreach ($kind in $RequiredKinds) {
        $kindMatches = @($artifacts | Where-Object { ($_.kind -is [string]) -and ($_.kind -eq $kind) })
        Assert-Condition -Condition ($kindMatches.Count -eq 1) -Message "Certification bundle missing or duplicate artifact kind: $kind"
    }

    foreach ($artifact in $artifacts) {
        $artifactPath = Get-RequiredString -Object $artifact -MemberPath "path"
        $declaredHash = Get-RequiredString -Object $artifact -MemberPath "sha256"
        Assert-Condition -Condition ($declaredHash -match '^[0-9A-Fa-f]{64}$') -Message "Certification bundle declares a malformed SHA-256 for $($artifact.kind)"
        $item = Assert-LocalFilePath -Path $artifactPath -Label "artifact '$($artifact.kind)'"
        $hash = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash
        Assert-Condition -Condition ($declaredHash -eq $hash) -Message "Certification bundle hash mismatch for $($artifact.kind)"
        Assert-Condition -Condition ((Get-RequiredInt64 -Object $artifact -MemberPath "bytes") -eq [int64]$item.Length) -Message "Certification bundle size mismatch for $($artifact.kind)"
    }

    $statusPath = Get-ArtifactPath -Artifacts $artifacts -Kind "certification_status"
    $reportPath = Get-ArtifactPath -Artifacts $artifacts -Kind "certification_report"
    $manifestPath = Get-ArtifactPath -Artifacts $artifacts -Kind "external_evidence_manifest"
    $gapPath = Get-ArtifactPath -Artifacts $artifacts -Kind "certification_gap_report_json"
    $preflightPath = Get-ArtifactPath -Artifacts $artifacts -Kind "vhd_preflight"

    # Bind the manifest's own claims to the artifacts it actually hashed, so an
    # unrelated file cannot be labelled and hashed as the required evidence.
    Assert-Condition -Condition ((Get-CanonicalPath -Path (Get-RequiredString -Object $bundle -MemberPath "status_path")) -eq (Get-CanonicalPath -Path $statusPath)) -Message "Certification bundle status path is not the hashed certification_status artifact"
    Assert-Condition -Condition ((Get-CanonicalPath -Path (Get-RequiredString -Object $bundle -MemberPath "source_report_path")) -eq (Get-CanonicalPath -Path $reportPath)) -Message "Certification bundle source report path is not the hashed certification_report artifact"
    Assert-Condition -Condition ((Get-CanonicalPath -Path (Get-RequiredString -Object $bundle -MemberPath "external_manifest_path")) -eq (Get-CanonicalPath -Path $manifestPath)) -Message "Certification bundle external manifest path is not the hashed external_evidence_manifest artifact"

    $status = Read-JsonFile -Path $statusPath -Label "certification status"
    $gap = Read-JsonFile -Path $gapPath -Label "gap report"
    $preflight = Read-JsonFile -Path $preflightPath -Label "VHD preflight"

    Assert-Condition -Condition ((Get-RequiredString -Object $bundle -MemberPath "claim_level") -eq (Get-RequiredString -Object $status -MemberPath "claim_level")) -Message "Certification bundle claim level mismatch"
    Assert-Condition -Condition ((Get-CanonicalPath -Path (Get-RequiredString -Object $bundle -MemberPath "source_report_path")) -eq (Get-CanonicalPath -Path (Get-RequiredString -Object $status -MemberPath "report.path"))) -Message "Certification bundle source report mismatch"
    Assert-Condition -Condition ((Get-CanonicalPath -Path (Get-RequiredString -Object $bundle -MemberPath "external_manifest_path")) -eq (Get-CanonicalPath -Path (Get-RequiredString -Object $status -MemberPath "external_gates.manifest_path"))) -Message "Certification bundle external manifest mismatch"
    Assert-Condition -Condition ((Get-RequiredInt64 -Object $bundle -MemberPath "summary.vhd_data_disk.passed") -eq (Get-RequiredInt64 -Object $status -MemberPath "vhd_data_disk.passed")) -Message "Certification bundle VHD passed mismatch"
    Assert-Condition -Condition ((Get-RequiredInt64 -Object $bundle -MemberPath "summary.vhd_data_disk.required") -eq (Get-RequiredInt64 -Object $status -MemberPath "vhd_data_disk.required")) -Message "Certification bundle VHD required mismatch"
    Assert-Condition -Condition ((Get-RequiredInt64 -Object $bundle -MemberPath "summary.vhd_data_disk.incomplete") -eq (Get-RequiredArrayCount -Object $status -MemberPath "vhd_data_disk.incomplete_ids")) -Message "Certification bundle VHD incomplete mismatch"
    Assert-Condition -Condition ((Get-RequiredInt64 -Object $bundle -MemberPath "summary.external_gates.passed") -eq (Get-RequiredInt64 -Object $status -MemberPath "external_gates.passed")) -Message "Certification bundle external passed mismatch"
    Assert-Condition -Condition ((Get-RequiredInt64 -Object $bundle -MemberPath "summary.external_gates.required") -eq (Get-RequiredInt64 -Object $status -MemberPath "external_gates.required")) -Message "Certification bundle external required mismatch"
    Assert-Condition -Condition ((Get-RequiredInt64 -Object $bundle -MemberPath "summary.external_gates.incomplete") -eq (Get-RequiredArrayCount -Object $status -MemberPath "external_gates.incomplete_ids")) -Message "Certification bundle external incomplete mismatch"
    Assert-Condition -Condition ((Get-RequiredInt64 -Object $bundle -MemberPath "summary.gap_report.gaps") -eq (Get-RequiredArrayCount -Object $gap -MemberPath "gaps")) -Message "Certification bundle gap count mismatch"
    Assert-Condition -Condition ((Get-RequiredString -Object $bundle -MemberPath "summary.gap_report.claim_level") -eq (Get-RequiredString -Object $gap -MemberPath "claim_level")) -Message "Certification bundle gap claim level mismatch"
    Assert-Condition -Condition ((Get-RequiredBool -Object $bundle -MemberPath "summary.vhd_preflight.ready") -eq (Get-RequiredBool -Object $preflight -MemberPath "ready_for_vhd_certification")) -Message "Certification bundle VHD preflight ready mismatch"
    Assert-Condition -Condition ((Get-RequiredBool -Object $bundle -MemberPath "summary.vhd_preflight.administrator") -eq (Get-RequiredBool -Object $preflight -MemberPath "administrator")) -Message "Certification bundle VHD preflight administrator mismatch"
    Assert-Condition -Condition ((Get-RequiredArrayCount -Object $bundle -MemberPath "summary.vhd_preflight.blockers") -eq (Get-RequiredArrayCount -Object $preflight -MemberPath "blockers")) -Message "Certification bundle VHD preflight blocker count mismatch"

    Write-Host "Partition Manager certification artifact bundle passed: $($artifacts.Count) artifacts verified."
}
finally {
    Pop-Location
}

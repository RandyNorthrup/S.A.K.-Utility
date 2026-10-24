<#
.SYNOPSIS
    Self-tests Partition Manager certification verifier and claim-level tools.

.DESCRIPTION
    Builds synthetic certification reports and external evidence manifests under
    build/test_results by default. No VHDs are created and no disks are mutated.
.PARAMETER ReportSelectionOnly
    Runs only recorded-report selection fixtures, before storage-preflight and other checks.
    The registered CTest leaves this switch unset and includes every certification-tool test.
.PARAMETER ControllerEvidenceOnly
    Runs only synthetic controller-evidence consumer fixtures; no executable or device is used.
.PARAMETER EvidenceValuesOnly
    Runs scalar/array evidence-value parity fixtures for the verifier and status consumer.
.PARAMETER EvidenceReferencesOnly
    Runs HTTPS/local-file reference parity fixtures directly against both consumers' functions.
.PARAMETER ReleaseClaimsOnly
    Runs synthetic current-state release-claim fixtures without storage or controller preparation.
.PARAMETER ReleaseEvidenceSelectionOnly
    Runs executable evidence-authority selectors with recording children; no storage tools run.
.PARAMETER ReleaseEvidenceCase
    Selects one evidence-authority case for an isolated production guard drill.
.PARAMETER ReleaseEvidenceScriptDirectory
    Explicit production-script copy directory for evidence-authority mutation drills.
.PARAMETER ControllerEvidenceCase
    Selects one controller fixture for isolated negative guard drills.
.PARAMETER ControllerConsumerDirectory
    Explicit disposable consumer-copy directory for source-mutation drills. Default is scripts/.
.PARAMETER FatConversionOnly
    Runs converter capture and mocked VHD cleanup safety tests. Never calls a storage tool.
.PARAMETER FatConversionCase
    Selects one converter case for independent production guard drills.
.PARAMETER FatConversionScriptDirectory
    Explicit production-script copy directory for converter guard drills.
.PARAMETER FatConversionExitWorker
    Internal child-only mode proving the real matrix failure exit after mocked conversion uncertainty.
#>

[CmdletBinding()]
param(
    [string]$OutputRoot = "build\test_results\partition-manager-certification-tools",
    [switch]$ReportSelectionOnly,
    [switch]$ControllerEvidenceOnly,
    [switch]$EvidenceValuesOnly,
    [switch]$EvidenceReferencesOnly,
    [switch]$ReleaseClaimsOnly,
    [switch]$ReleaseEvidenceSelectionOnly,
    [string]$ReleaseEvidenceCase = '',
    [string]$ReleaseEvidenceScriptDirectory = '',
    [string]$ControllerEvidenceCase = '',
    [string]$ControllerConsumerDirectory = '',
    [switch]$FatConversionOnly,
    [string]$FatConversionCase = '',
    [string]$FatConversionScriptDirectory = '',
    [switch]$FatConversionExitWorker
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
if ($ControllerConsumerDirectory -eq '') { $ControllerConsumerDirectory = $PSScriptRoot }
$CertificationMatrix = Get-Content -LiteralPath (Join-Path $ProjectRoot "certification\PARTITION_MANAGER_CERTIFICATION_MATRIX.json") -Raw | ConvertFrom-Json
if ($CertificationMatrix.schema_version -ne 1) {
    throw "Unsupported Partition Manager certification matrix schema_version: $($CertificationMatrix.schema_version)"
}
$ExpectedVhdScenarioIds = @($CertificationMatrix.vhd_scenarios | ForEach-Object { $_.id })
$ExpectedExternalGateIds = @($CertificationMatrix.external_gates | ForEach-Object { $_.id })
$CertificationSelectorCallers = @(
    "verify_partition_manager_certification.ps1",
    "get_partition_manager_certification_status.ps1",
    "run_partition_manager_vhd_certification_strict.ps1",
    "run_partition_manager_hardware_certification_strict.ps1"
)

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
    if ($matches.Count -gt 1) {
        throw "Certification matrix has duplicate scenario ID (self-test would silently select the first): $Id"
    }
    return $matches[0]
}

function New-SyntheticEvidence {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Id
    )

    if ($Id -eq 'vhd.image-restore') { return New-SyntheticControllerEvidence }
    $spec = Get-ScenarioSpec -Id $Id
    $evidence = [ordered]@{}
    foreach ($key in @($spec.required_evidence_keys)) {
        if ($null -ne $spec.PSObject.Properties["required_evidence_values"]) {
            $requirement = $spec.required_evidence_values.PSObject.Properties[$key]
            if ($null -ne $requirement) {
                if ($null -ne $requirement.Value.PSObject.Properties["allowed_values"]) {
                    $evidence[$key] = @($requirement.Value.allowed_values)[0]
                    continue
                }
                if ($null -ne $requirement.Value.PSObject.Properties["contains_all"]) {
                    $evidence[$key] = @($requirement.Value.contains_all)
                    continue
                }
            }
        }
        $evidence[$key] = switch ($key) {
            "sentinel_verified" { $true }
            "target_overwritten" { $true }
            "signature_verified" { $true }
            "outside_marker_preserved" { $true }
            "remaining_partitions" { 0 }
            "partition_count" { 1 }
            "mbr2gpt_validate_exit_code" { 0 }
            "mbr2gpt_convert_exit_code" { 0 }
            default { "synthetic-$key" }
        }
    }
    return [pscustomobject]$evidence
}

function New-ScenarioResult {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Id,
        [Parameter(Mandatory = $true)]
        [string]$Status
    )

    $spec = Get-ScenarioSpec -Id $Id
    $requiredEvidenceValues = if ($null -eq $spec.PSObject.Properties["required_evidence_values"]) {
        $null
    }
    else {
        $spec.required_evidence_values
    }
    [ordered]@{
        id = $Id
        name = $spec.name
        status = $Status
        message = ""
        required_evidence_keys = @($spec.required_evidence_keys)
        required_evidence_values = $requiredEvidenceValues
        safety_contract = @($spec.safety_contract)
        evidence = if ($Status -eq "Passed" -and $Id.StartsWith("vhd.")) {
            New-SyntheticEvidence -Id $Id
        }
        else {
            $null
        }
        completed_utc = (Get-Date).ToUniversalTime().ToString("o")
    }
}

function Write-CertificationReport {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$ReportStatus,
        [Parameter(Mandatory = $true)]
        [hashtable]$VhdStatuses
    )

    $results = @()
    foreach ($id in $ExpectedVhdScenarioIds) {
        $results += New-ScenarioResult -Id $id -Status $VhdStatuses[$id]
    }
    foreach ($id in $ExpectedExternalGateIds) {
        $results += New-ScenarioResult -Id $id -Status "ExternalGate"
    }

    $report = [ordered]@{
        tool = "partition-manager-destructive-certification"
        schema_version = 1
        status = $ReportStatus
        started_utc = (Get-Date).ToUniversalTime().ToString("o")
        completed_utc = (Get-Date).ToUniversalTime().ToString("o")
        host = $env:COMPUTERNAME
        synthetic_test_fixture = $true
        project_root = $ProjectRoot
        output_root = Split-Path -Parent $Path
        certification_matrix = "certification/PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
        run_vhd_data_disk_matrix = ($VhdStatuses.Values -contains "Passed")
        elevated_relaunch_requested = $false
        require_vhd_data_disk_evidence = $false
        keep_vhd = $false
        vhd_size_mb = 768
        administrator = $false
        summary = [ordered]@{
            passed = @($results | Where-Object { $_.status -eq "Passed" }).Count
            failed = @($results | Where-Object { $_.status -eq "Failed" }).Count
            skipped = @($results | Where-Object { $_.status -eq "Skipped" }).Count
            external_gates = @($results | Where-Object { $_.status -eq "ExternalGate" }).Count
        }
        results = $results
    }

    $parent = Split-Path -Parent $Path
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Write-ExternalManifest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Status,
        [string]$EvidencePath = "",
        [string]$EvidenceUrl = ""
    )

    $manifest = [ordered]@{
        tool = "partition-manager-external-certification"
        schema_version = 1
        created_utc = (Get-Date).ToUniversalTime().ToString("o")
        project_root = $ProjectRoot
        certification_matrix = "certification/PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
        results = @($ExpectedExternalGateIds | ForEach-Object {
                $spec = Get-ScenarioSpec -Id $_
                [ordered]@{
                    id = $_
                    name = $spec.name
                    status = $Status
                    required_evidence_keys = @($spec.required_evidence_keys)
                    required_evidence_values = if ($null -eq $spec.PSObject.Properties["required_evidence_values"]) {
                        $null
                    }
                    else {
                        $spec.required_evidence_values
                    }
                    safety_contract = @($spec.safety_contract)
                    evidence = if ($Status -eq "Passed") {
                        New-SyntheticEvidence -Id $_
                    }
                    else {
                        $null
                    }
                    evidence_path = $EvidencePath
                    evidence_url = $EvidenceUrl
                    notes = "synthetic certification-tool self-test"
                }
            })
    }

    $parent = Split-Path -Parent $Path
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function New-VhdStatusMap {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Status
    )

    $map = @{}
    foreach ($id in $ExpectedVhdScenarioIds) {
        $map[$id] = $Status
    }
    return $map
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

function Assert-NoCertificationFailureSilences {
    $sharedStorageRunners = @(
        "scripts\run_partition_manager_destructive_certification.ps1",
        "scripts\run_partition_manager_cluster_size_external_gate.ps1",
        "scripts\run_partition_manager_bitlocker_mutation_external_gate.ps1",
        "scripts\run_partition_manager_file_recovery_external_gate.ps1",
        "scripts\run_partition_manager_hdd_defrag_external_gate.ps1",
        "scripts\run_partition_manager_ext_filesystem_vm_gate.ps1",
        "scripts\run_partition_manager_linux_swap_vm_gate.ps1",
        "scripts\run_partition_manager_physical_apple_probe_validation.ps1",
        "scripts\run_partition_manager_physical_hfs_file_apply_validation.ps1",
        "scripts\run_partition_manager_physical_cross_filesystem_destructive_validation.ps1"
    )
    foreach ($relativePath in $sharedStorageRunners) {
        $path = Join-Path $ProjectRoot $relativePath
        $text = Get-Content -LiteralPath $path -Raw -ErrorAction Stop
        Assert-Condition `
            -Condition $text.Contains("partition_manager_storage.ps1") `
            -Message "Certification script bypasses the shared storage helper: $relativePath"
    }

    $partitionScripts = @(
        Get-ChildItem -LiteralPath (Join-Path $ProjectRoot "scripts") `
            -Filter "run_partition_manager_*.ps1" -File -ErrorAction Stop
        Get-ChildItem -LiteralPath (Join-Path $ProjectRoot "scripts") `
            -Filter "launch_partition_manager_*.ps1" -File -ErrorAction Stop
    ) | Sort-Object FullName -Unique
    foreach ($script in $partitionScripts) {
        $text = Get-Content -LiteralPath $script.FullName -Raw -ErrorAction Stop
        Assert-Condition `
            -Condition (-not $text.Contains("-ErrorAction SilentlyContinue")) `
            -Message "Partition Manager script hides a failure with SilentlyContinue: $($script.Name)"
    }

    $storageHelperPath = Join-Path $ProjectRoot "scripts\partition_manager_storage.ps1"
    $storageHelperText = Get-Content -LiteralPath $storageHelperPath -Raw -ErrorAction Stop
    Assert-Condition `
        -Condition (-not $storageHelperText.Contains("-ErrorAction SilentlyContinue")) `
        -Message "Shared Partition Manager storage helper hides a failure with SilentlyContinue"

    $extLauncherText = Get-Content -LiteralPath (Join-Path $ProjectRoot `
        "scripts\launch_partition_manager_ext_filesystem_vm_gate_local.ps1") -Raw -ErrorAction Stop
    Assert-Condition `
        -Condition ($extLauncherText.Contains("sharedStorageHelper") -and `
                    $extLauncherText.Contains("localStorageHelper")) `
        -Message "Ext VM launcher does not stage the shared storage helper"
    $swapLauncherText = Get-Content -LiteralPath (Join-Path $ProjectRoot `
        "scripts\launch_partition_manager_linux_swap_vm_gate_local.ps1") -Raw -ErrorAction Stop
    Assert-Condition `
        -Condition ($swapLauncherText.Contains("sharedStorageHelper") -and `
                    $swapLauncherText.Contains("localStorageHelper")) `
        -Message "Linux swap VM launcher does not stage the shared storage helper"
    $physicalLauncherText = Get-Content -LiteralPath (Join-Path $ProjectRoot `
        "scripts\launch_partition_manager_physical_cross_filesystem_destructive_validation_local.ps1") `
        -Raw -ErrorAction Stop
    Assert-Condition `
        -Condition ($physicalLauncherText.Contains("storageHelperHash") -and `
                    $physicalLauncherText.Contains("actualHelperHash")) `
        -Message "Physical launcher does not pin the shared storage helper across elevation"
    foreach ($relativePath in @(
            "scripts\launch_partition_manager_offline_bios_bcd_break_local.ps1",
            "scripts\launch_partition_manager_offline_bios_bcdboot_repair_local.ps1",
            "scripts\launch_partition_manager_offline_bios_mbr_fixture_build_local.ps1",
            "scripts\launch_partition_manager_offline_uefi_bcd_break_local.ps1",
            "scripts\launch_partition_manager_offline_uefi_bcdboot_repair_local.ps1")) {
        $launcherText = Get-Content -LiteralPath (Join-Path $ProjectRoot $relativePath) `
            -Raw -ErrorAction Stop
        Assert-Condition `
            -Condition $launcherText.Contains("partition_manager_storage.ps1") `
            -Message "Offline launcher does not stage the shared storage helper: $relativePath"
    }

    $strictHardwarePath = Join-Path $ProjectRoot "scripts\run_partition_manager_hardware_certification_strict.ps1"
    $strictHardwareText = Get-Content -LiteralPath $strictHardwarePath -Raw -ErrorAction Stop
    Assert-Condition `
        -Condition (-not $strictHardwareText.Contains("SkipExternalLabPackageCheck")) `
        -Message "Strict hardware certification still exposes a mandatory lab-package bypass"
}

function Test-PartitionManagerStorageHelpers {
    $helperPath = Join-Path $ProjectRoot "scripts\partition_manager_storage.ps1"
    & {
        param([string]$Path)

        . $Path
        $script:partitionMode = "present"
        $script:volumeMode = "present"
        $script:imageAttached = $false
        $script:dismountCount = 0
        $script:dismountFails = $false

        function New-StorageTestError {
            param([System.Management.Automation.ErrorCategory]$Category)
            return [System.Management.Automation.ErrorRecord]::new(
                [InvalidOperationException]::new("injected storage failure"),
                "InjectedStorageFailure",
                $Category,
                $null)
        }
        function Get-Partition {
            [CmdletBinding()]
            param([int]$DiskNumber, [string]$DriveLetter)
            if ($script:partitionMode -eq "present") {
                return [pscustomobject]@{
                    DiskNumber = if ($PSBoundParameters.ContainsKey("DiskNumber")) { $DiskNumber } else { 7 }
                    PartitionNumber = 1
                    DriveLetter = $DriveLetter
                }
            }
            if ($script:partitionMode -eq "missing") {
                throw (New-StorageTestError -Category ObjectNotFound)
            }
            throw (New-StorageTestError -Category InvalidOperation)
        }
        function Get-Volume {
            [CmdletBinding()]
            param([Parameter(ValueFromPipeline = $true)] [object]$Partition,
                  [string]$DriveLetter)
            process {
                if ($script:volumeMode -eq "present") {
                    return [pscustomobject]@{
                        FileSystem = "NTFS"
                        Partition = $Partition
                        DriveLetter = $DriveLetter
                    }
                }
                if ($script:volumeMode -eq "missing") {
                    throw (New-StorageTestError -Category ObjectNotFound)
                }
                throw (New-StorageTestError -Category InvalidOperation)
            }
        }
        function Get-DiskImage {
            [CmdletBinding()]
            param([string]$ImagePath)
            return [pscustomobject]@{ Attached = $script:imageAttached; ImagePath = $ImagePath }
        }
        function Dismount-DiskImage {
            [CmdletBinding()]
            param([string]$ImagePath)
            if ($script:dismountFails) {
                throw (New-StorageTestError -Category InvalidOperation)
            }
            $script:dismountCount++
        }

        Assert-Condition -Condition (@(Get-SakDiskPartitions -DiskNumber 7).Count -eq 1) `
            -Message "storage helper did not return a present partition"
        $script:partitionMode = "missing"
        Assert-Condition -Condition (@(Get-SakDiskPartitions -DiskNumber 7).Count -eq 0) `
            -Message "storage helper did not classify ObjectNotFound as an empty partition set"
        $script:partitionMode = "failure"
        $partitionFailedClosed = $false
        try { Get-SakDiskPartitions -DiskNumber 7 | Out-Null } catch { $partitionFailedClosed = $true }
        Assert-Condition -Condition $partitionFailedClosed `
            -Message "storage helper swallowed a non-ObjectNotFound partition failure"
        $script:partitionMode = "present"
        Assert-Condition -Condition (@(Get-SakPartitionsByDriveLetter -DriveLetter "T").Count -eq 1) `
            -Message "storage helper did not return a drive-letter partition"
        $script:partitionMode = "missing"
        Assert-Condition -Condition (@(Get-SakPartitionsByDriveLetter -DriveLetter "T").Count -eq 0) `
            -Message "storage helper did not classify a missing drive-letter partition"
        $script:partitionMode = "failure"
        $driveLetterFailedClosed = $false
        try { Get-SakPartitionsByDriveLetter -DriveLetter "T" | Out-Null } `
        catch { $driveLetterFailedClosed = $true }
        Assert-Condition -Condition $driveLetterFailedClosed `
            -Message "storage helper swallowed a drive-letter partition failure"

        $partition = [pscustomobject]@{ PartitionNumber = 1 }
        Assert-Condition -Condition ((Get-SakPartitionVolume -Partition $partition).FileSystem -eq "NTFS") `
            -Message "storage helper did not return a present volume"
        $script:volumeMode = "missing"
        Assert-Condition -Condition ($null -eq (Get-SakPartitionVolume -Partition $partition)) `
            -Message "storage helper did not classify ObjectNotFound as an absent volume"
        $script:volumeMode = "failure"
        $volumeFailedClosed = $false
        try { Get-SakPartitionVolume -Partition $partition | Out-Null } catch { $volumeFailedClosed = $true }
        Assert-Condition -Condition $volumeFailedClosed `
            -Message "storage helper swallowed a non-ObjectNotFound volume failure"
        $script:volumeMode = "present"
        Assert-Condition -Condition ((Get-SakVolumeByDriveLetter -DriveLetter "T").FileSystem -eq "NTFS") `
            -Message "storage helper did not return a drive-letter volume"
        $script:volumeMode = "missing"
        Assert-Condition -Condition ($null -eq (Get-SakVolumeByDriveLetter -DriveLetter "T")) `
            -Message "storage helper did not classify a missing drive-letter volume"
        $script:volumeMode = "failure"
        $driveVolumeFailedClosed = $false
        try { Get-SakVolumeByDriveLetter -DriveLetter "T" | Out-Null } `
        catch { $driveVolumeFailedClosed = $true }
        Assert-Condition -Condition $driveVolumeFailedClosed `
            -Message "storage helper swallowed a drive-letter volume failure"

        Dismount-SakDiskImageIfAttached -Path "C:\lab.vhdx"
        Assert-Condition -Condition ($script:dismountCount -eq 0) `
            -Message "storage helper dismounted an image that was already detached"
        $script:imageAttached = $true
        Dismount-SakDiskImageIfAttached -Path "C:\lab.vhdx"
        Assert-Condition -Condition ($script:dismountCount -eq 1) `
            -Message "storage helper did not dismount an attached image"
        $script:dismountFails = $true
        $dismountFailedClosed = $false
        try { Dismount-SakDiskImageIfAttached -Path "C:\lab.vhdx" } catch { $dismountFailedClosed = $true }
        Assert-Condition -Condition $dismountFailedClosed `
            -Message "storage helper swallowed a dismount failure"
    } $helperPath
}

function Assert-ClaimLevel {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ReportPath,
        [string]$ManifestPath = "",
        [Parameter(Mandatory = $true)]
        [string]$ExpectedClaimLevel,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $statusPath = Write-ClaimStatus `
        -ReportPath $ReportPath `
        -ManifestPath $ManifestPath `
        -Name $Name
    $summary = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($summary.claim_level -eq $ExpectedClaimLevel) -Message "$Name expected $ExpectedClaimLevel, got $($summary.claim_level)"
}

function Write-ClaimStatus {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ReportPath,
        [string]$ManifestPath = "",
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $statusPath = Join-Path $Script:RunRoot "$Name-status.json"
    $arguments = @{
        ReportPath = $ReportPath
        OutputPath = $statusPath
        Quiet = $true
    }
    foreach ($key in $Script:ControllerEvidenceArguments.Keys) {
        $arguments[$key] = $Script:ControllerEvidenceArguments[$key]
    }
    if (-not [string]::IsNullOrWhiteSpace($ManifestPath)) {
        $arguments.ExternalEvidenceManifest = $ManifestPath
    }

    & (Join-Path $ControllerConsumerDirectory 'get_partition_manager_certification_status.ps1') @arguments
    return $statusPath
}

function Assert-ToolFails {
    param(
        [Parameter(Mandatory = $true)]
        [scriptblock]$Body,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    try {
        & $Body
        throw "$Name did not fail"
    }
    catch {
        if ($_.Exception.Message -eq "$Name did not fail") {
            throw
        }
        # A negative test must fail because the tool REJECTED bad input, not
        # because it could not be found, could not bind its parameters, or could
        # not be parsed. Those pass the assertion without ever reaching the
        # validation under test, so re-surface them as real failures.
        if (($_.Exception -is [System.Management.Automation.CommandNotFoundException]) -or
            ($_.Exception -is [System.Management.Automation.ParameterBindingException]) -or
            ($_.Exception -is [System.Management.Automation.ParseException])) {
            throw "$Name failed before reaching its validation ($($_.Exception.GetType().Name)): $($_.Exception.Message)"
        }
    }
}

function Set-FirstRequiredEvidenceValue {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Result,
        [Parameter(Mandatory = $true)]
        [object]$Value
    )

    $key = @((Get-ScenarioSpec -Id $Result.id).required_evidence_keys)[0]
    $property = $Result.evidence.PSObject.Properties[$key]
    if ($null -eq $property) {
        throw "Synthetic result missing required evidence key: $($Result.id) $key"
    }
    $property.Value = $Value
}

function Set-ObjectProperty {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Object,
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [object]$Value
    )

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        $Object | Add-Member -NotePropertyName $Name -NotePropertyValue $Value
    }
    else {
        $property.Value = $Value
    }
}

function ConvertTo-ProjectRelativePath {
    param([Parameter(Mandatory = $true)] [string]$Path)

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $projectRootFull = [System.IO.Path]::GetFullPath($ProjectRoot)
    if (-not $projectRootFull.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $projectRootFull += [System.IO.Path]::DirectorySeparatorChar
    }
    if ($fullPath.StartsWith($projectRootFull, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $fullPath.Substring($projectRootFull.Length)
    }
    return $fullPath
}

function Write-SelectionReport {
    param([string]$Path, [string]$StartedUtc, [string]$CompletedUtc)

    Write-CertificationReport -Path $Path -ReportStatus "NotRun" `
        -VhdStatuses (New-VhdStatusMap -Status "Skipped")
    $report = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    $report.started_utc = $StartedUtc
    $report.completed_utc = $CompletedUtc
    if ([string]::IsNullOrEmpty($StartedUtc)) { $report.PSObject.Properties.Remove("started_utc") }
    if ([string]::IsNullOrEmpty($CompletedUtc)) { $report.PSObject.Properties.Remove("completed_utc") }
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Test-RecordedCertificationReportOrder {
    param([string]$Root)

    $older = Join-Path $Root "older\partition-manager-certification-report.json"
    $newer = Join-Path $Root "newer\partition-manager-certification-report.json"
    Write-SelectionReport -Path $older -StartedUtc "2026-01-01T00:00:00Z" `
        -CompletedUtc "2026-01-01T00:01:00Z"
    Write-SelectionReport -Path $newer -StartedUtc "2026-01-02T00:00:00Z" `
        -CompletedUtc "2026-01-02T00:01:00Z"
    (Get-Item -LiteralPath $older).LastWriteTimeUtc = [datetime]"2026-01-04T00:00:00Z"
    (Get-Item -LiteralPath $newer).LastWriteTimeUtc = [datetime]"2026-01-03T00:00:00Z"
    foreach ($rootArgument in @((ConvertTo-ProjectRelativePath -Path $Root), $Root)) {
        $verification = @(& (Join-Path $ProjectRoot "scripts\verify_partition_manager_certification.ps1") `
            -CertificationRoot $rootArgument 6>&1)
        $verificationText = (@($verification | ForEach-Object { $_.ToString() }) -join "`n")
        Assert-Condition -Condition ($verificationText.Contains($newer)) `
            -Message "Verifier did not choose the newest recorded run despite reversed mtimes"
        $statusPath = Join-Path $Root "selected-status.json"
        & (Join-Path $ProjectRoot "scripts\get_partition_manager_certification_status.ps1") `
            -CertificationRoot $rootArgument -OutputPath $statusPath -Quiet
        $status = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
        Assert-Condition -Condition ($status.report.path -eq $newer) `
            -Message "Status tool did not choose the same newest recorded run despite reversed mtimes"
    }
}

function Invoke-CertificationCallerSelector {
    param([string]$Caller, [string]$Root)

    $path = Join-Path $ProjectRoot "scripts\$Caller"
    $tokens = $null
    $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$parseErrors)
    Assert-Condition -Condition ($parseErrors.Count -eq 0) -Message "Caller does not parse: $Caller"
    $copies = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq "Resolve-LatestCertificationReport"
    }, $true))
    Assert-Condition -Condition ($copies.Count -eq 0) -Message "Caller retains a selector copy: $Caller"
    $imports = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
            $node.InvocationOperator -eq [Management.Automation.Language.TokenKind]::Dot -and
            $node.Extent.Text -eq '. (Join-Path $PSScriptRoot "partition_manager_certification_report.ps1")'
    }, $true))
    Assert-Condition -Condition ($imports.Count -eq 1) -Message "Caller must import the shared selector: $Caller"
    $calls = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
            $node.GetCommandName() -eq "Resolve-LatestCertificationReport"
    }, $true))
    Assert-Condition -Condition ($calls.Count -eq 1) -Message "Caller must select through the shared seam: $Caller"
    $callText = $calls[0].Extent.Text
    Assert-Condition -Condition ($callText -match '^Resolve-LatestCertificationReport\s+-Root\s+\$(CertificationRoot|resolvedCertificationRoot|resolvedOutputRoot)$') `
        -Message "Selector test refuses to execute anything except the bounded Root call: $Caller"
    # Execute only that source-derived selector call. Strict VHD/hardware workflows never run.
    return & {
        param([scriptblock]$Selector, [string]$FixtureRoot, [string]$Helper)
        . $Helper
        $CertificationRoot = $FixtureRoot
        $resolvedCertificationRoot = $FixtureRoot
        $resolvedOutputRoot = $FixtureRoot
        & $Selector
    } ([scriptblock]::Create($callText)) $Root `
        (Join-Path $ProjectRoot "scripts\partition_manager_certification_report.ps1")
}

function Assert-AllCertificationSelectors {
    param([string]$Root, [string]$ExpectedPath)

    foreach ($caller in $CertificationSelectorCallers) {
        $actual = Invoke-CertificationCallerSelector -Caller $caller -Root $Root
        Assert-Condition -Condition ($actual -eq $ExpectedPath) `
            -Message "$caller chose $actual instead of the newest recorded run $ExpectedPath"
    }
}

function Assert-CertificationSelectorsRefuseTime {
    param([string]$Root)

    foreach ($caller in $CertificationSelectorCallers) {
        $refused = $false
        try {
            Invoke-CertificationCallerSelector -Caller $caller -Root $Root | Out-Null
        } catch {
            if ($_.Exception.Message -notlike "*no readable completed_utc/started_utc*") { throw }
            $refused = $true
        }
        Assert-Condition -Condition $refused -Message "$caller accepted an unreadable report time"
    }
}

function Test-CertificationTimestampPolicy {
    param([string]$Root)

    $first = Join-Path $Root "first\partition-manager-certification-report.json"
    $second = Join-Path $Root "second\partition-manager-certification-report.json"
    Write-SelectionReport -Path $first -StartedUtc "2026-01-01T00:00:00Z" `
        -CompletedUtc "2026-01-04T00:00:00Z"
    Write-SelectionReport -Path $second -StartedUtc "2026-01-02T00:00:00Z" `
        -CompletedUtc "2026-01-03T00:00:00Z"
    (Get-Item -LiteralPath $first).LastWriteTimeUtc = [datetime]"2026-01-05T00:00:00Z"
    (Get-Item -LiteralPath $second).LastWriteTimeUtc = [datetime]"2026-01-06T00:00:00Z"
    Assert-AllCertificationSelectors -Root $Root -ExpectedPath $first
    Write-SelectionReport -Path $second -StartedUtc "2026-01-05T00:00:00Z" -CompletedUtc ""
    Assert-AllCertificationSelectors -Root $Root -ExpectedPath $second
    Write-SelectionReport -Path $first -StartedUtc "2026-01-01T00:00:00Z" `
        -CompletedUtc "2026-01-02T04:00:00+02:00"
    Write-SelectionReport -Path $second -StartedUtc "2026-01-01T00:00:00Z" `
        -CompletedUtc "2026-01-02T01:00:00Z"
    Assert-AllCertificationSelectors -Root $Root -ExpectedPath $first
}

function Test-UnreadableCertificationReportTime {
    param([string]$Root)

    $report = Join-Path $Root "unreadable\partition-manager-certification-report.json"
    Write-SelectionReport -Path $report -StartedUtc "2026-01-01T00:00:00Z" `
        -CompletedUtc "invalid-completion-time"
    $invalidCompletion = Get-Content -LiteralPath $report -Raw
    foreach ($payload in @($invalidCompletion,
                           $invalidCompletion.Replace('"invalid-completion-time"', '0'),
                           $invalidCompletion.Replace('"invalid-completion-time"', 'false'),
                           '{invalid-json')) {
        Set-Content -LiteralPath $report -Value $payload -Encoding UTF8
        Assert-CertificationSelectorsRefuseTime -Root $Root
    }
    Write-SelectionReport -Path $report -StartedUtc "2026-01-01T00:00:00Z" `
        -CompletedUtc "2026-01-02T00:00:00Z"
    $locked = [IO.File]::Open($report, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
    try { Assert-CertificationSelectorsRefuseTime -Root $Root } finally { $locked.Dispose() }
}

function Test-MissingCertificationReportTime {
    param([string]$Root)

    $report = Join-Path $Root "missing\partition-manager-certification-report.json"
    Write-SelectionReport -Path $report -StartedUtc "" -CompletedUtc ""
    Assert-CertificationSelectorsRefuseTime -Root $Root
    foreach ($name in @("verify_partition_manager_certification.ps1",
                         "get_partition_manager_certification_status.ps1")) {
        $refused = $false
        try {
            & (Join-Path $ProjectRoot "scripts\$name") `
                -CertificationRoot (ConvertTo-ProjectRelativePath -Path $Root) | Out-Null
        } catch {
            if ($_.Exception.Message -notlike "*no readable completed_utc/started_utc*") {
                throw "$name failed for an unrelated reason: $($_.Exception.Message)"
            }
            $refused = $true
        }
        Assert-Condition -Condition $refused -Message "$name accepted a report without recorded time"
    }
}

function Assert-CertificationDefaultRoot {
    param([string]$ScriptName, [string]$ParameterName, [string]$Expected)

    $scriptPath = Join-Path $ProjectRoot "scripts\$ScriptName"
    $tokens = $null
    $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile(
        $scriptPath, [ref]$tokens, [ref]$parseErrors)
    Assert-Condition ($parseErrors.Count -eq 0) "$ScriptName does not parse"
    $parameters = @($ast.ParamBlock.Parameters | Where-Object {
        $_.Name.VariablePath.UserPath -ceq $ParameterName
    })
    Assert-Condition ($parameters.Count -eq 1 -and
        $parameters[0].DefaultValue -is
            [Management.Automation.Language.StringConstantExpressionAst] -and
        $parameters[0].DefaultValue.Value -ceq $Expected) `
        "$ScriptName defaults $ParameterName outside its owned evidence tree"
}

function Test-CertificationReportSelection {
    Assert-CertificationDefaultRoot 'verify_partition_manager_certification.ps1' `
        'CertificationRoot' 'artifacts\partition-manager-certification\vhd-strict'
    Assert-CertificationDefaultRoot 'get_partition_manager_certification_status.ps1' `
        'CertificationRoot' 'artifacts\partition-manager-certification\vhd-strict'
    Assert-CertificationDefaultRoot 'test_partition_manager_certification_tools.ps1' `
        'OutputRoot' 'build\test_results\partition-manager-certification-tools'
    $runRootFull = [IO.Path]::GetFullPath($Script:RunRoot).TrimEnd('\', '/') +
        [IO.Path]::DirectorySeparatorChar
    $fixtureRoot = [IO.Path]::GetFullPath((Join-Path $Script:RunRoot `
        ("report-selection-" + [guid]::NewGuid().ToString("N"))))
    if (-not $fixtureRoot.StartsWith($runRootFull, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Report-selection fixture escaped its owned output root"
    }
    New-Item -ItemType Directory -Path $fixtureRoot -ErrorAction Stop | Out-Null
    try {
        $failures = @()
        foreach ($case in @("RecordedCertificationReportOrder", "MissingCertificationReportTime",
                             "CertificationTimestampPolicy", "UnreadableCertificationReportTime")) {
            try {
                & "Test-$case" -Root (Join-Path $fixtureRoot $case)
            } catch {
                $failures += "$case`: $($_.Exception.Message)"
            }
        }
        Assert-Condition -Condition ($failures.Count -eq 0) `
            -Message ("Report-selection failures: " + ($failures -join "`n"))
    } finally {
        # Only this newly created disposable fixture tree is removed. Never touch real reports.
        if (-not $fixtureRoot.StartsWith($runRootFull, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing report-selection fixture cleanup outside its output root"
        }
        Remove-Item -LiteralPath $fixtureRoot -Recurse -Force -ErrorAction Stop
    }
}

function Initialize-SyntheticControllerEvidence {
    $fixtureRoot = Join-Path $Script:RunRoot 'synthetic-controller-proof'
    $Script:ControllerFixture = & (Join-Path $ProjectRoot 'tests/unit/test_partition_manager_controller_evidence.ps1') `
        -FixtureRoot $fixtureRoot -FixtureProjectRoot $ProjectRoot
    $Script:ControllerEvidenceArguments = @{
        ControllerCertifierPath = $Script:ControllerFixture.Arguments.ExpectedBinaryPath
        ControllerSourceManifestPath = $Script:ControllerFixture.Arguments.SourceManifestPath
    }
}

function New-SyntheticControllerEvidence {
    $native = $Script:ControllerFixture.Report
    return [pscustomobject]@{
        source_image = $native.source_image.Replace('/', '\'); target_image = $native.target_after.vhd_path
        target_disk_number = 19; pre_restore_marker = $native.pre_restore_marker
        sentinel_verified = $true; target_overwritten = $true; independent_verification = $true
        execution_engine = 'PartitionManagerController'; controller_executed = $true; worker_descendants_finished = $true
        controller_run_id = $native.run_id; controller_report = 'synthetic-controller-proof/report.json'
        controller_report_sha256 = (Get-FileHash -LiteralPath $Script:ControllerFixture.Arguments.ReportPath -Algorithm SHA256).Hash
        source_digest = $native.source_digest; certifier_binary_sha256 = $native.certifier_binary_sha256
        create_image_operation_id = $native.operations[0].operation_id
        restore_image_operation_id = $native.operations[1].operation_id
        source_image_sha256 = $native.source_image_sha256; sentinel_sha256 = $native.sentinel_sha256
    }
}

function Invoke-SyntheticCertificationVerifier {
    param([string]$ReportPath, [string]$ExternalEvidenceManifest = '',
        [switch]$RequireVhdDataDiskEvidence, [switch]$RequireExternalGateEvidence)
    $arguments = @{
        ReportPath = $ReportPath; ExternalEvidenceManifest = $ExternalEvidenceManifest
        RequireVhdDataDiskEvidence = $RequireVhdDataDiskEvidence; RequireExternalGateEvidence = $RequireExternalGateEvidence
    }
    foreach ($key in $Script:ControllerEvidenceArguments.Keys) {
        $arguments[$key] = $Script:ControllerEvidenceArguments[$key]
    }
    & (Join-Path $ControllerConsumerDirectory 'verify_partition_manager_certification.ps1') @arguments
}

function Add-ControllerConsumerCase {
    param([string]$Name, [scriptblock]$Change, [string]$Reason = '', [object]$Data = $null)
    $Script:ControllerConsumerCases.Add([pscustomobject]@{ Name = $Name; Change = $Change; Reason = $Reason; Data = $Data })
}

function Register-ControllerConsumerCases {
    $Script:ControllerConsumerCases = [Collections.Generic.List[object]]::new()
    Add-ControllerConsumerCase current { param($c) }
    Add-ControllerConsumerCase windows-path-case { param($c)
        $c.Evidence.source_image = $c.Evidence.source_image.ToUpperInvariant().Replace('\', '/')
        $c.Evidence.target_image = $c.Evidence.target_image.ToUpperInvariant().Replace('\', '/')
    }
    Add-ControllerConsumerCase old-copy-only { param($c)
        foreach ($field in @('execution_engine', 'controller_executed', 'worker_descendants_finished',
                            'controller_report', 'controller_report_sha256', 'controller_run_id',
                            'source_digest', 'certifier_binary_sha256', 'create_image_operation_id',
                            'restore_image_operation_id', 'source_image_sha256', 'sentinel_sha256', 'independent_verification')) {
            $c.Evidence.PSObject.Properties.Remove($field)
        }
    } 'execution_engine'
    Add-ControllerConsumerCase helper-validation-only { param($c) $c.Native.validation_only = $true } 'validation_only'
    Add-ControllerConsumerCase helper-not-executed { param($c) $c.Native.controller_executed = $false } 'controller_executed'
    Add-ControllerConsumerCase helper-descendants-uncertain { param($c) $c.Native.worker_descendants_finished = $false } 'worker_descendants_finished'
    Add-ControllerConsumerCase helper-operation-failed { param($c) $c.Native.operations[1].execution.success = $false } 'execution success'
    Add-ControllerConsumerCase helper-job-still-active { param($c) $c.Native.job_quiescence.last_active_processes = '1' } 'remaining job processes'
    Add-ControllerConsumerCase changed-selected-binary { param($c)
        [IO.File]::WriteAllText($c.BinaryPath, 'changed fake binary; never executed')
    } 'Expected binary hash mismatch'
    Add-ControllerConsumerCase wrong-independent-binary { param($c)
        $other = Join-Path $Script:RunRoot 'wrong-synthetic.exe'
        [IO.File]::WriteAllText($other, 'independently selected wrong fake binary; never executed')
        $Script:ControllerEvidenceArguments.ControllerCertifierPath = $other
    } 'Expected binary hash mismatch'
    Add-ControllerConsumerCase raw-sidecar-crlf { param($c)
        [IO.File]::WriteAllText($c.ManifestPath, $c.ManifestText.Replace("`n", "`r`n"), $c.Utf8)
    } 'Source digest does not match'
    Add-ControllerConsumerCase source-row-stale { param($c)
        $records = @($c.ManifestText.TrimEnd("`n").Split("`n"))
        $records[0] = $records[0].Split(':')[0] + ':' + ('0' * 64)
        [IO.File]::WriteAllText($c.ManifestPath, ($records -join "`n") + "`n", $c.Utf8)
        $c.Native.source_digest = (Get-FileHash -LiteralPath $c.ManifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
        $c.Evidence.source_digest = $c.Native.source_digest
    } 'Source changed'
    Add-ControllerConsumerCase header-omitted { param($c)
        $records = @($c.ManifestText.TrimEnd("`n").Split("`n"))
        $omitted = @($records | Where-Object { $_.StartsWith('include/sak/') })[0]
        $records = @($records | Where-Object { $_ -cne $omitted })
        [IO.File]::WriteAllText($c.ManifestPath, ($records -join "`n") + "`n", $c.Utf8)
        $c.Native.source_digest = (Get-FileHash -LiteralPath $c.ManifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
        $c.Evidence.source_digest = $c.Native.source_digest
    } 'Current public header omitted'
    foreach ($field in @('controller_executed', 'worker_descendants_finished', 'independent_verification',
                        'sentinel_verified', 'target_overwritten')) {
        Add-ControllerConsumerCase "scenario-false-$field" { param($c, $data)
            $c.Evidence.$data = $false
        } "scenario $field" $field
    }
    foreach ($field in @('source_digest', 'certifier_binary_sha256', 'source_image_sha256', 'sentinel_sha256')) {
        Add-ControllerConsumerCase "scenario-mismatch-$field" { param($c, $data)
            $c.Evidence.$data = ('9' * 64)
        } "Scenario $field binding mismatch" $field
    }
    Add-ControllerConsumerCase report-byte-tamper { param($c) $c.PreserveClaimedReportHash = $true } 'controller_report_sha256 binding mismatch'
    foreach ($field in @('controller_run_id', 'create_image_operation_id', 'restore_image_operation_id')) {
        Add-ControllerConsumerCase "scenario-mismatch-$field" { param($c, $data)
            $c.Evidence.$data = [guid]::NewGuid().ToString('D')
        } 'scenario' $field
    }
    Add-ControllerConsumerCase scenario-mismatch-source_image { param($c)
        $c.Evidence.source_image = 'C:\SAK-Cert\fixture\unrelated.img'
    } 'source image binding mismatch'
    Add-ControllerConsumerCase scenario-mismatch-target_image { param($c)
        $c.Evidence.target_image = 'C:\SAK-Cert\fixture\unrelated.vhdx'
    } 'target image binding mismatch'
    Add-ControllerConsumerCase absolute-report-reference { param($c) $c.Evidence.controller_report = $c.NativePath } 'portable controller report reference'
    Add-ControllerConsumerCase traversal-report-reference { param($c)
        $c.Evidence.controller_report = 'synthetic-controller-proof/../synthetic-controller-proof/report.json'
    } 'remain inside its evidence directory'
    Add-ControllerConsumerCase reattached-number-string { param($c) $c.Evidence.target_disk_number = '19' } 'reattached disk number is invalid'
}

function Test-ControllerConsumerCase {
    param([object]$Definition)
    $fixture = $Script:ControllerFixture
    $outerPath = Join-Path $Script:RunRoot ('controller-' + $Definition.Name + '.json')
    Write-CertificationReport -Path $outerPath -ReportStatus Partial -VhdStatuses (New-VhdStatusMap Passed)
    $outer = Get-Content -LiteralPath $outerPath -Raw | ConvertFrom-Json
    $row = @($outer.results | Where-Object { $_.id -eq 'vhd.image-restore' })[0]
    $context = [pscustomobject]@{
        Evidence = $row.evidence; Native = ($fixture.Report | ConvertTo-Json -Depth 20 | ConvertFrom-Json)
        NativePath = $fixture.Arguments.ReportPath; ManifestPath = $fixture.Arguments.SourceManifestPath
        BinaryPath = $fixture.Arguments.ExpectedBinaryPath; PreserveClaimedReportHash = $false
        ManifestText = [IO.File]::ReadAllText($fixture.Arguments.SourceManifestPath)
        Utf8 = [Text.UTF8Encoding]::new($false, $true)
    }
    $binaryBytes = [IO.File]::ReadAllBytes($context.BinaryPath)
    $nativeBytes = [IO.File]::ReadAllBytes($context.NativePath)
    try {
        & $Definition.Change $context $Definition.Data
        # Keep envelope hash current while mutating native outcome guards. The byte-tamper case
        # deliberately leaves the envelope at its old digest to prove binding of parsed bytes.
        $nativeText = ($context.Native | ConvertTo-Json -Depth 20) + "`n"
        [IO.File]::WriteAllText($context.NativePath, $nativeText, $context.Utf8)
        if (-not $context.PreserveClaimedReportHash -and $null -ne $row.evidence.PSObject.Properties['controller_report_sha256']) {
            $row.evidence.controller_report_sha256 = (Get-FileHash -LiteralPath $context.NativePath -Algorithm SHA256).Hash
        }
        $outer | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $outerPath -Encoding UTF8
        $failure = $null
        try { Invoke-SyntheticCertificationVerifier -ReportPath $outerPath -RequireVhdDataDiskEvidence }
        catch { $failure = $_.Exception.Message }
        $statusPath = Write-ClaimStatus -ReportPath $outerPath -Name ('controller-' + $Definition.Name)
        $status = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
        if ($Definition.Reason -eq '') {
            Assert-Condition ($null -eq $failure) "Synthetic controller positive refused: $failure"
            Assert-Condition ($status.claim_level -eq 'VhdDataDiskCertified') 'Synthetic controller positive status was not admitted'
        } else {
            Assert-Condition ($null -ne $failure) "Consumer accepted unsafe controller evidence: verifier $($Definition.Name)"
            Assert-Condition ($failure.Contains($Definition.Reason)) "Wrong controller verifier refusal: $failure"
            Assert-Condition ($status.claim_level -eq 'CodeCompleteOnly') "Consumer accepted unsafe controller evidence: status $($Definition.Name)"
            $reason = $status.vhd_data_disk.incomplete_reasons.'vhd.image-restore'
            Assert-Condition ($reason -is [string] -and $reason.Contains($Definition.Reason)) "Controller status hid refusal: $reason"
        }
        Write-Host "PASS synthetic controller consumer $($Definition.Name)"
    } finally {
        [IO.File]::WriteAllBytes($context.BinaryPath, $binaryBytes)
        [IO.File]::WriteAllBytes($context.NativePath, $nativeBytes)
        [IO.File]::WriteAllText($context.ManifestPath, $context.ManifestText, $context.Utf8)
        $Script:ControllerEvidenceArguments.ControllerCertifierPath = $context.BinaryPath
    }
}

function Test-ControllerEvidenceConsumers {
    Register-ControllerConsumerCases
    $selected = @($Script:ControllerConsumerCases | Where-Object { $ControllerEvidenceCase -eq '' -or $_.Name -ceq $ControllerEvidenceCase })
    Assert-Condition ($selected.Count -gt 0) 'Unknown controller consumer fixture'
    foreach ($definition in $selected) { Test-ControllerConsumerCase $definition }
    Write-Host "Synthetic controller consumer fixtures: $($selected.Count) passed; not release certification"
}

function Get-CertificationReferenceFunctions {
    param([string]$ScriptName, [string[]]$Names)
    $path = Join-Path $ControllerConsumerDirectory $ScriptName
    $tokens = $null
    $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$parseErrors)
    Assert-Condition ($parseErrors.Count -eq 0) "Reference consumer parse failure: $ScriptName"
    $definitions = foreach ($name in $Names) {
        $matches = @($ast.FindAll({ param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name
        }, $true))
        Assert-Condition ($matches.Count -eq 1) "Missing/duplicate reference function: $ScriptName $name"
        $matches[0].Extent.Text
    }
    return $definitions -join "`n"
}

function Test-CertificationEvidenceReferences {
    $getter = Get-CertificationReferenceFunctions 'get_partition_manager_certification_status.ps1' @('Test-EvidenceReference')
    $verifier = Get-CertificationReferenceFunctions 'verify_partition_manager_certification.ps1' `
        @('Assert-Condition', 'Assert-ExternalEvidenceReference', 'Resolve-ProjectRoot')
    $artifact = Join-Path $Script:RunRoot 'synthetic-reference-artifact.txt'
    [IO.File]::WriteAllText($artifact, 'Synthetic reference fixture; never fetched or executed')
    $missing = Join-Path $Script:RunRoot 'missing-reference-artifact.txt'
    Assert-Condition (-not (Test-Path -LiteralPath $missing)) 'Negative reference fixture unexpectedly exists'
    $cases = @(
        @{ Name = 'https'; Path = ''; Url = 'https://example.com/proof'; Passed = $true },
        @{ Name = 'local-file'; Path = $artifact; Url = ''; Passed = $true },
        @{ Name = 'file-and-https'; Path = $artifact; Url = 'https://example.com/proof'; Passed = $true },
        @{ Name = 'http'; Path = ''; Url = 'http://example.com/proof'; Passed = $false; Error = 'URL is invalid' },
        @{ Name = 'file-and-http'; Path = $artifact; Url = 'http://example.com/proof'; Passed = $false; Error = 'URL is invalid' },
        @{ Name = 'missing-file-and-https'; Path = $missing; Url = 'https://example.com/proof'; Passed = $false; Error = 'path not found' }
    )
    foreach ($definition in $cases) {
        $row = [pscustomobject]@{ id = 'external.boot-repair-bios'; evidence_path = $definition.Path; evidence_url = $definition.Url }
        $outcome = & {
            param($getterText, $verifierText, $result, $directory)
            # Only the selected, source-derived reference functions run. No storage workflow,
            # helper executable, artifact URL fetch, or certification report generation occurs.
            . ([scriptblock]::Create($getterText))
            . ([scriptblock]::Create($verifierText))
            $accepted = Test-EvidenceReference -Result $result -ManifestDirectory $directory -ProjectRoot $directory
            $failure = $null
            try { Assert-ExternalEvidenceReference -Result $result -ManifestDirectory $directory }
            catch { $failure = $_.Exception.Message }
            [pscustomobject]@{ Accepted = $accepted; Failure = $failure }
        } $getter $verifier $row $Script:RunRoot
        if ($definition.Passed) {
            Assert-Condition ($outcome.Accepted -eq $true -and $null -eq $outcome.Failure) `
                "Valid evidence reference refused: $($definition.Name) $($outcome.Failure)"
        } else {
            Assert-Condition ($outcome.Accepted -eq $false) "Evidence reference getter accepted unsafe input: $($definition.Name)"
            Assert-Condition ($outcome.Failure -is [string] -and $outcome.Failure.Contains($definition.Error)) `
                "Evidence reference verifier lost real refusal: $($outcome.Failure)"
        }
        Write-Host "PASS synthetic evidence reference $($definition.Name)"
    }
}

function Test-CertificationAllowedValueArrays {
    $reportPath = Join-Path $Script:RunRoot 'allowed-values-report.json'
    Write-CertificationReport -Path $reportPath -ReportStatus Passed -VhdStatuses (New-VhdStatusMap Passed)
    $artifact = Join-Path $Script:RunRoot 'synthetic-values-artifact.txt'
    [IO.File]::WriteAllText($artifact, 'Synthetic consumer fixture only; not external certification')
    $gateId = 'external.boot-repair-bios'
    $allowed = @((Get-ScenarioSpec $gateId).required_evidence_values.boot_mode.allowed_values)
    Assert-Condition ($allowed.Count -gt 1) 'Allowed-values fixture requires the BIOS scalar/array contract'
    $cases = @(
        @{ Name = 'scalar'; Value = $allowed[0]; Passed = $true },
        @{ Name = 'array'; Value = $allowed; Passed = $true },
        @{ Name = 'mixed'; Value = @($allowed[0], 'wrong-mode'); Passed = $false },
        @{ Name = 'empty'; Value = @(); Passed = $false },
        @{ Name = 'wrong-scalar'; Value = 'wrong-mode'; Passed = $false }
    )
    foreach ($definition in $cases) {
        $manifestPath = Join-Path $Script:RunRoot ('allowed-values-' + $definition.Name + '.json')
        Write-ExternalManifest -Path $manifestPath -Status Passed -EvidencePath $artifact
        $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        $gate = @($manifest.results | Where-Object { $_.id -eq $gateId })[0]
        $gate.evidence.boot_mode = $definition.Value
        $manifest | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
        $statusPath = Write-ClaimStatus -ReportPath $reportPath -ManifestPath $manifestPath -Name ('allowed-values-' + $definition.Name)
        $status = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
        $failure = $null
        try {
            Invoke-SyntheticCertificationVerifier -ReportPath $reportPath -ExternalEvidenceManifest $manifestPath `
                -RequireVhdDataDiskEvidence -RequireExternalGateEvidence
        } catch { $failure = $_.Exception.Message }
        if ($definition.Passed) {
            Assert-Condition ($status.claim_level -eq 'HardwareCertified' -and $null -eq $failure) `
                "Legitimate allowed-values $($definition.Name) control refused: $failure"
        } else {
            Assert-Condition ($status.claim_level -eq 'VhdDataDiskCertified') `
                "Allowed-value status accepted invalid evidence: $($definition.Name)"
            Assert-Condition ($null -ne $failure -and $failure.Contains('boot_mode')) `
                "Allowed-value verifier failed for unrelated reason: $failure"
        }
        Write-Host "PASS synthetic allowed-values $($definition.Name)"
    }
}

function New-ReleaseClaimFixtureStatus {
    param([string]$ClaimLevel, [string[]]$VhdIds = @(), [string[]]$ExternalIds = @())
    return [pscustomobject]@{
        tool = 'partition-manager-certification-status'; schema_version = 1; claim_level = $ClaimLevel
        vhd_data_disk = [pscustomobject]@{
            passed = $ExpectedVhdScenarioIds.Count - $VhdIds.Count
            required = $ExpectedVhdScenarioIds.Count; incomplete_ids = @($VhdIds)
        }
        external_gates = [pscustomobject]@{
            passed = $ExpectedExternalGateIds.Count - $ExternalIds.Count
            required = $ExpectedExternalGateIds.Count; incomplete_ids = @($ExternalIds)
        }
    }
}

# The document skeleton every case starts from: the heading, an EMPTY generated block, and
# history prose. The checker must fill the block only from the status, and a check against an
# un-generated block must fail as stale.
$Script:ReleaseClaimBegin = '<!-- BEGIN GENERATED certification-state: do not edit by hand. Regenerate with ' +
    'scripts/check_partition_manager_release_claims.ps1 -Update -->'
$Script:ReleaseClaimEnd = '<!-- END GENERATED certification-state -->'

function New-ReleaseClaimFixtureDocument {
    return (@('# Partition Manager Certification', '', '## Current Certification State', '',
        $Script:ReleaseClaimBegin, $Script:ReleaseClaimEnd, '',
        'Narrative history; no gate parses this paragraph.', '',
        '### Historical June 2026 checkpoints',
        'Historical claim level: `CodeCompleteOnly`; destructive certification remains partial in that historical record.'
    ) -join "`n") + "`n"
}

function Invoke-ReleaseClaimChecker {
    param([string]$Fixture, [string]$StatusPath, [switch]$Update)
    $hostPath = (Get-Process -Id $PID).Path
    $arguments = @('-NoProfile', '-NonInteractive', '-File',
        (Join-Path $Fixture 'scripts/check_partition_manager_release_claims.ps1'), '-StatusPath', $StatusPath)
    if ($Update) { $arguments += '-Update' }
    return Invoke-CapturedProcess -FilePath $hostPath -Arguments $arguments -TimeoutSeconds 30
}

function Get-ReleaseClaimBlock {
    param([string]$Text)
    $begin = $Text.IndexOf($Script:ReleaseClaimBegin, [StringComparison]::Ordinal)
    $end = $Text.IndexOf($Script:ReleaseClaimEnd, [StringComparison]::Ordinal)
    Assert-Condition ($begin -ge 0 -and $end -gt $begin) 'Release-claim fixture lost its generated markers'
    return $Text.Substring($begin, $end - $begin)
}

function Invoke-ReleaseClaimFixture {
    param([string]$Fixture, [hashtable]$Case)
    $statusPath = Join-Path $Fixture 'status.json'
    $documentPath = Join-Path $Fixture 'docs/PARTITION_MANAGER_CERTIFICATION.md'
    [IO.File]::WriteAllText($statusPath, ($Case.status | ConvertTo-Json -Depth 8))
    $document = if ($Case.ContainsKey('document')) { $Case.document } else { New-ReleaseClaimFixtureDocument }
    [IO.File]::WriteAllText($documentPath, $document)
    if ($Case.ContainsKey('generate')) {
        [IO.File]::WriteAllText($statusPath, ($Case.generate | ConvertTo-Json -Depth 8))
        $generated = Invoke-ReleaseClaimChecker $Fixture $statusPath -Update
        Assert-Condition (-not $generated.timed_out -and $generated.exit_code -eq 0) (
            "Release-claim fixture could not generate its block: $($Case.name): $($generated.stdout)$($generated.stderr)")
        [IO.File]::WriteAllText($statusPath, ($Case.status | ConvertTo-Json -Depth 8))
    }
    if ($Case.ContainsKey('tamper')) {
        $text = [IO.File]::ReadAllText($documentPath)
        Assert-Condition ($text.Contains($Case.tamper[0])) "Release-claim tamper target missing: $($Case.name)"
        [IO.File]::WriteAllText($documentPath, $text.Replace($Case.tamper[0], $Case.tamper[1]))
    }
    $before = [IO.File]::ReadAllText($documentPath)
    $result = Invoke-ReleaseClaimChecker $Fixture $statusPath -Update:($Case.ContainsKey('update'))
    $output = $result.stdout + $result.stderr
    $after = [IO.File]::ReadAllText($documentPath)
    if ($Case.ContainsKey('error')) {
        Assert-Condition (-not $result.timed_out -and $result.exit_code -ne 0 -and $output.Contains($Case.error)) (
            "Release-claim negative fixture failed: $($Case.name): $output")
        Assert-Condition ($after -ceq $before) "Refused release-claim run changed the document: $($Case.name)"
    } else {
        Assert-Condition (-not $result.timed_out -and $result.exit_code -eq 0) (
            "Release-claim positive fixture failed: $($Case.name): $output")
        $block = Get-ReleaseClaimBlock $after
        foreach ($phrase in @(if ($Case.ContainsKey('contains')) { $Case['contains'] })) {
            Assert-Condition ($block.Contains($phrase)) "Generated block missing '$phrase': $($Case.name)"
        }
        foreach ($phrase in @(if ($Case.ContainsKey('absent')) { $Case['absent'] })) {
            Assert-Condition (-not $block.Contains($phrase)) "Generated block must not contain '$phrase': $($Case.name)"
        }
        $outsideBefore = $before.Replace((Get-ReleaseClaimBlock $before), '')
        $outsideAfter = $after.Replace($block, '')
        Assert-Condition ($outsideAfter -ceq $outsideBefore) "Release-claim update changed prose outside the block: $($Case.name)"
        if ($Case.ContainsKey('update')) {
            $check = Invoke-ReleaseClaimChecker $Fixture $statusPath
            Assert-Condition (-not $check.timed_out -and $check.exit_code -eq 0) (
                "Freshly generated block did not pass its own check: $($Case.name): $($check.stdout)$($check.stderr)")
        }
    }
    Write-Host "PASS release-claim state: $($Case.name)"
}

function Test-ReleaseClaimCurrentState {
    param([string]$CheckerText = '', [string]$CaseName = '')
    . (Join-Path $ProjectRoot 'scripts/process_capture.ps1')
    $fixture = Join-Path $Script:RunRoot ('release-claims-' + [guid]::NewGuid().ToString('N'))
    foreach ($directory in @('scripts', 'certification', 'docs')) {
        New-Item -ItemType Directory -Path (Join-Path $fixture $directory) -ErrorAction Stop | Out-Null
    }
    if ([string]::IsNullOrEmpty($CheckerText)) {
        $CheckerText = Get-Content -LiteralPath (Join-Path $ProjectRoot 'scripts/check_partition_manager_release_claims.ps1') -Raw
    }
    [IO.File]::WriteAllText((Join-Path $fixture 'scripts/check_partition_manager_release_claims.ps1'), $CheckerText)
    foreach ($matrixName in @('PARTITION_MANAGER_CERTIFICATION_MATRIX.json', 'APPLE_FILESYSTEM_CAPABILITY_MATRIX.json')) {
        Copy-Item -LiteralPath (Join-Path $ProjectRoot "certification/$matrixName") -Destination (Join-Path $fixture 'certification')
    }
    $restore = $CertificationMatrix.vhd_scenarios | Where-Object { $_.id -eq 'vhd.image-restore' }
    $restoreNo = '| `vhd.image-restore` | ' + $restore.name + ' | no |'
    $restoreYes = '| `vhd.image-restore` | ' + $restore.name + ' | yes |'
    $refusal = 'Do not mark the Partition Manager destructive paths fully certified until those reports exist.'
    $partial = New-ReleaseClaimFixtureStatus 'CodeCompleteOnly' @('vhd.image-restore')
    $external = New-ReleaseClaimFixtureStatus 'VhdDataDiskCertified' @() @('external.hardware-wipe')
    $both = New-ReleaseClaimFixtureStatus 'CodeCompleteOnly' @('vhd.image-restore') @('external.hardware-wipe')
    $hardware = New-ReleaseClaimFixtureStatus 'HardwareCertified'
    $failed = New-ReleaseClaimFixtureStatus 'FailedEvidence' @('vhd.image-restore')
    $badCounts = $partial | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    $badCounts.vhd_data_disk.required = 0
    $badType = $partial | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    $badType.vhd_data_disk.required = [string]$badType.vhd_data_disk.required
    $forgedCount = $partial | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    $forgedCount.vhd_data_disk.required++
    $forgedCount.vhd_data_disk.passed++
    $badSum = $partial | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    $badSum.vhd_data_disk.passed--
    $badIds = $partial | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    $badIds.vhd_data_disk.incomplete_ids = $null
    $badSchema = $partial | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    $badSchema.schema_version = '1'
    $unknownLevel = New-ReleaseClaimFixtureStatus 'Certified' @('vhd.image-restore')
    $unknown = New-ReleaseClaimFixtureStatus 'CodeCompleteOnly' @('vhd.unknown-case')
    $duplicate = New-ReleaseClaimFixtureStatus 'CodeCompleteOnly' @('vhd.image-restore', 'vhd.image-restore')
    $forgedHardware = New-ReleaseClaimFixtureStatus 'HardwareCertified' @('vhd.image-restore')
    $forgedVhd = New-ReleaseClaimFixtureStatus 'VhdDataDiskCertified' @('vhd.image-restore') @('external.hardware-wipe')
    $skeleton = New-ReleaseClaimFixtureDocument
    $cases = @(
        @{ name = 'generate one VHD gap'; status = $partial; update = $true
           contains = @('Current claim level: `CodeCompleteOnly`.', "Accepted VHD evidence: $($ExpectedVhdScenarioIds.Count - 1)/$($ExpectedVhdScenarioIds.Count).", $restoreNo,
               'remains partial because VHD evidence is incomplete.', $refusal, '| `A-i` | APFS |')
           absent = @($restoreYes, 'VM/hardware/lab evidence is incomplete') },
        @{ name = 'generate external gap only'; status = $external; update = $true
           contains = @('Current claim level: `VhdDataDiskCertified`.', $restoreYes,
               '| `external.hardware-wipe` | Non-system hardware wipe on disposable physical disk | no |',
               'remains partial because VM/hardware/lab evidence is incomplete.', $refusal)
           absent = @('VHD evidence is incomplete') },
        @{ name = 'generate VHD and external gaps'; status = $both; update = $true
           contains = @('because VHD evidence is incomplete and VM/hardware/lab evidence is incomplete.', $refusal) },
        @{ name = 'generate hardware certified'; status = $hardware; update = $true
           contains = @('Current claim level: `HardwareCertified`.', 'Incomplete VHD scenarios: none.',
               'Incomplete external gates: none.')
           absent = @(' | no |', 'remains partial', $refusal) },
        @{ name = 'generate failed evidence'; status = $failed; update = $true
           contains = @('Current claim level: `FailedEvidence`.', 'because evidence verification failed and VHD evidence is incomplete.', $refusal) },
        @{ name = 'regenerate over a stale block'; status = $partial; generate = $hardware; update = $true
           contains = @('Current claim level: `CodeCompleteOnly`.', $restoreNo); absent = @('`HardwareCertified`') },
        @{ name = 'current block passes check'; status = $partial; generate = $partial },
        @{ name = 'never generated block is stale'; status = $partial; document = $skeleton; error = 'is stale' },
        @{ name = 'block from another status is stale'; status = $partial; generate = $hardware; error = 'is stale' },
        @{ name = 'hand-raised claim level is stale'; status = $partial; generate = $partial
           tamper = @('Current claim level: `CodeCompleteOnly`.', 'Current claim level: `HardwareCertified`.'); error = 'is stale' },
        @{ name = 'hand-accepted scenario is stale'; status = $partial; generate = $partial
           tamper = @($restoreNo, $restoreYes); error = 'is stale' },
        @{ name = 'dropped refusal is stale'; status = $partial; generate = $partial
           tamper = @($refusal, ''); error = 'is stale' },
        @{ name = 'missing markers'; status = $partial; update = $true
           document = $skeleton.Replace($Script:ReleaseClaimBegin + "`n", ''); error = 'Expected exactly one BEGIN and one END' },
        @{ name = 'duplicate begin marker'; status = $partial
           document = $skeleton.Replace($Script:ReleaseClaimEnd, $Script:ReleaseClaimBegin + "`n" + $Script:ReleaseClaimEnd); error = 'Expected exactly one BEGIN and one END' },
        @{ name = 'markers outside current section'; status = $partial; update = $true
           document = $skeleton.Replace($Script:ReleaseClaimBegin + "`n" + $Script:ReleaseClaimEnd + "`n", '') + $Script:ReleaseClaimBegin + "`n" + $Script:ReleaseClaimEnd + "`n"
           error = 'must sit inside the Current Certification State section' },
        @{ name = 'missing current-state heading'; status = $partial; update = $true
           document = $skeleton.Replace('## Current Certification State', '## Historical Only'); error = 'Expected one Current Certification State heading' },
        @{ name = 'duplicate current-state heading'; status = $partial
           document = $skeleton + "`n## Current Certification State`n"; error = 'Expected one Current Certification State heading' },
        @{ name = 'unknown claim level'; status = $unknownLevel; update = $true; error = 'unrecognized claim_level' },
        @{ name = 'unknown incomplete ID'; status = $unknown; update = $true; error = 'unknown or duplicate incomplete ID' },
        @{ name = 'duplicate incomplete ID'; status = $duplicate; update = $true; error = 'unknown or duplicate incomplete ID' },
        @{ name = 'zero required count'; status = $badCounts; update = $true; error = 'counts do not match the certification matrix' },
        @{ name = 'count must be integer'; status = $badType; update = $true; error = 'required must be a nonnegative integer' },
        @{ name = 'forged matrix denominator'; status = $forgedCount; update = $true; error = 'counts do not match the certification matrix' },
        @{ name = 'inconsistent passed and incomplete sum'; status = $badSum; update = $true; error = 'passed plus incomplete does not equal required' },
        @{ name = 'missing incomplete array'; status = $badIds; update = $true; error = 'incomplete_ids must be an array' },
        @{ name = 'wrong status schema'; status = $badSchema; update = $true; error = 'Unsupported Partition Manager certification status schema_version' },
        @{ name = 'hardware claim has a VHD gap'; status = $forgedHardware; update = $true; error = 'claim level contradicts its incomplete evidence sets' },
        @{ name = 'VHD claim has a VHD gap'; status = $forgedVhd; update = $true; error = 'claim level contradicts its incomplete evidence sets' },
        @{ name = 'invalid status refused in check mode'; status = $forgedHardware; generate = $partial; error = 'claim level contradicts its incomplete evidence sets' }
    )
    if ($CaseName -ne '') {
        $cases = @($cases | Where-Object { $_.name -ceq $CaseName })
        Assert-Condition ($cases.Count -eq 1) "Unknown release-claim fixture: $CaseName"
    }
    foreach ($case in $cases) { Invoke-ReleaseClaimFixture $fixture $case }
    Write-Host "PASS release-claim generated-block fixtures: $($cases.Count) cases"
}

function Get-ReleaseEvidenceSelectorText {
    param([string]$Directory, [string]$Kind)
    $name = if ($Kind -eq 'claim') { 'check_partition_manager_release_claims.ps1' } else { 'check_release_readiness.ps1' }
    $tokens = $null
    $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $Directory $name), [ref]$tokens, [ref]$parseErrors)
    Assert-Condition ($parseErrors.Count -eq 0) "Evidence selector does not parse: $name"
    $functionNames = if ($Kind -eq 'claim') { @('Resolve-StatusPath') } else { @('Assert-ContainedProjectPath', 'Assert-PartitionEvidenceMode', 'Invoke-ReleaseReadinessScript') }
    $parts = @($ast.ParamBlock.Extent.Text, '$ErrorActionPreference = "Stop"', '$ProjectRoot = $PSScriptRoot')
    foreach ($functionName in $functionNames) {
        $definitions = @($ast.FindAll({ param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $functionName
        }, $true))
        # A removed mode guard must fail the behavioral no-write cases, not make an
        # original-source drill stop on a missing extraction helper before it executes.
        if ($functionName -eq 'Assert-PartitionEvidenceMode' -and $definitions.Count -eq 0) { continue }
        Assert-Condition ($definitions.Count -eq 1) "Expected one production selector function: $functionName"
        $parts += $definitions[0].Extent.Text
    }
    if ($Kind -eq 'claim') {
        $parts += 'Resolve-StatusPath -ProjectRoot $ProjectRoot'
        return $parts -join [Environment]::NewLine
    }
    $tries = @($ast.EndBlock.Statements | Where-Object { $_ -is [Management.Automation.Language.TryStatementAst] })
    Assert-Condition ($tries.Count -eq 1) 'Expected one aggregate readiness body'
    $statements = @($tries[0].Body.Statements)
    $start = @($statements | Where-Object { $_ -is [Management.Automation.Language.AssignmentStatementAst] -and $_.Left.Extent.Text -ceq '$requirePartitionVhdEvidenceStrict' })
    $end = @($statements | Where-Object { $_ -is [Management.Automation.Language.AssignmentStatementAst] -and $_.Left.Extent.Text -ceq '$partitionGapReportPath' })
    Assert-Condition ($start.Count -eq 1 -and $end.Count -eq 1 -and $end[0].Extent.StartOffset -gt $start[0].Extent.StartOffset) 'Aggregate evidence selector boundaries missing or ambiguous'
    # Run the actual contiguous production statements, including input checks, scaffold,
    # plan-only decision, verification and status generation. Only child scripts are fixtures.
    foreach ($guardName in @('Assert-ContainedProjectPath', 'Assert-PartitionEvidenceMode')) {
        $guards = @($ast.EndBlock.Statements | Where-Object {
            $_ -is [Management.Automation.Language.PipelineAst] -and $_.PipelineElements.Count -eq 1 -and
            $_.PipelineElements[0] -is [Management.Automation.Language.CommandAst] -and
            $_.PipelineElements[0].GetCommandName() -ceq $guardName
        })
        if ($guardName -eq 'Assert-PartitionEvidenceMode' -and $guards.Count -eq 0) { continue }
        Assert-Condition ($guards.Count -eq 1 -and $guards[0].Extent.StartOffset -lt $tries[0].Extent.StartOffset) "Evidence guard must run before aggregate child checks: $guardName"
        $parts += $guards[0].Extent.Text
    }
    $parts += $ast.Extent.Text.Substring($start[0].Extent.StartOffset, $end[0].Extent.StartOffset - $start[0].Extent.StartOffset)
    return $parts -join [Environment]::NewLine
}

function New-ReleaseEvidenceSelectorFixture {
    param([string]$Directory, [string]$Kind)
    $fixture = Join-Path $Script:RunRoot ('evidence-selection-' + [guid]::NewGuid().ToString('N'))
    foreach ($relative in @('scripts', 'artifacts/partition-manager-certification/readiness',
            'artifacts/partition-manager-certification/vhd-strict', 'artifacts/partition-manager-certification/vm-lab', 'chosen')) {
        New-Item -ItemType Directory -Path (Join-Path $fixture $relative) -ErrorAction Stop | Out-Null
    }
    foreach ($relative in @('artifacts/partition-manager-certification/vm-lab/external-evidence.imported.json',
            'artifacts/partition-manager-certification/vm-lab/external-evidence.imported.checklist.md',
            'chosen/manifest.json', 'chosen/checklist.md', 'chosen/status.json',
            'artifacts/partition-manager-certification/vhd-strict/partition-manager-certification-report.json')) {
        [IO.File]::WriteAllText((Join-Path $fixture $relative), 'caller-owned synthetic evidence')
    }
    $childText = @'
[CmdletBinding()]
param([string]$CertificationRoot, [string]$ExternalEvidenceManifest, [string]$OutputPath,
    [string]$ChecklistPath, [string]$EvidenceRoot, [string]$ManifestPath, [string]$ReportPath,
    [string]$OutputRoot, [switch]$CreateEvidenceDirectories, [switch]$Force, [switch]$Quiet,
    [switch]$RequireAdministrator, [switch]$RequireVhdDataDiskEvidence, [switch]$RequireExternalGateEvidence)
$ErrorActionPreference = 'Stop'
$fixtureRoot = Split-Path -Parent $PSScriptRoot
function Resolve-FixtureWritePath {
    param([string]$Path)
    $root = [IO.Path]::GetFullPath($fixtureRoot).TrimEnd('\') + '\'
    $candidate = if ([IO.Path]::IsPathRooted($Path)) { $Path } else { Join-Path $fixtureRoot $Path }
    $full = [IO.Path]::GetFullPath($candidate)
    if (-not $full.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Fixture write leaves its unique root: $Path"
    }
    return $full
}
# Validate every child output before even writing the call receipt. .NET does not
# follow Push-Location; every actual write below uses this absolute fixture binding.
foreach ($target in @($OutputPath, $ChecklistPath, $OutputRoot, $EvidenceRoot)) {
    if ($target) { $null = Resolve-FixtureWritePath $target }
}
$name = Split-Path -Leaf $PSCommandPath
[IO.File]::WriteAllText((Resolve-FixtureWritePath ($name + '.call.json')), ($PSBoundParameters | ConvertTo-Json -Depth 5))
if ($name -eq 'get_partition_manager_certification_status.ps1') {
    if ($ExternalEvidenceManifest -and -not (Test-Path -LiteralPath $ExternalEvidenceManifest -PathType Leaf)) {
        throw 'Selected explicit external evidence is missing'
    }
    [IO.File]::WriteAllText((Resolve-FixtureWritePath $OutputPath), 'generated synthetic status')
}
if ($name -eq 'new_partition_manager_external_evidence_manifest.ps1') {
    [IO.File]::WriteAllText((Resolve-FixtureWritePath $OutputPath), 'synthetic pending scaffold')
    [IO.File]::WriteAllText((Resolve-FixtureWritePath $ChecklistPath), 'synthetic pending checklist')
}
if ($name -eq 'run_partition_manager_destructive_certification.ps1') {
    [IO.File]::WriteAllText((Resolve-FixtureWritePath (Join-Path $OutputRoot 'plan-only-write.txt')), 'synthetic plan-only report write')
}
$global:LASTEXITCODE = 0
'@
    foreach ($name in @('get_partition_manager_certification_status.ps1', 'new_partition_manager_external_evidence_manifest.ps1',
            'check_partition_manager_external_checklist.ps1', 'check_partition_manager_external_lab_package.ps1',
            'test_partition_manager_vhd_preflight.ps1', 'check_partition_manager_vhd_preflight_report.ps1',
            'run_partition_manager_destructive_certification.ps1', 'verify_partition_manager_certification.ps1')) {
        [IO.File]::WriteAllText((Join-Path $fixture "scripts/$name"), $childText)
    }
    [IO.File]::WriteAllText((Join-Path $fixture 'selector.ps1'), (Get-ReleaseEvidenceSelectorText $Directory $Kind))
    return $fixture
}

function Get-ReleaseEvidenceSelectionCases {
    $scratch = 'artifacts\partition-manager-certification\readiness'
    $strict = 'artifacts\partition-manager-certification\vhd-strict'
    return @(
        @{ name = 'claim-default'; kind = 'claim'; args = @{}; root = $strict; manifest = '' },
        @{ name = 'claim-explicit-scratch'; kind = 'claim'; args = @{ CertificationRoot = $scratch }; root = $scratch; manifest = '' },
        @{ name = 'claim-explicit-root'; kind = 'claim'; args = @{ CertificationRoot = 'chosen' }; root = 'chosen'; manifest = '' },
        @{ name = 'claim-explicit-manifest'; kind = 'claim'; args = @{ ExternalEvidenceManifest = 'chosen/manifest.json' }; root = $strict; manifest = 'chosen/manifest.json' },
        @{ name = 'claim-status-precedence'; kind = 'claim'; args = @{ StatusPath = 'chosen/status.json'; CertificationRoot = 'missing'; ExternalEvidenceManifest = 'missing.json' }; status = $true },
        @{ name = 'claim-missing-status'; kind = 'claim'; args = @{ StatusPath = 'missing-status.json' }; error = 'Cannot find path' },
        @{ name = 'claim-missing-manifest'; kind = 'claim'; args = @{ ExternalEvidenceManifest = 'missing.json' }; error = 'Selected explicit external evidence is missing'; selectedMissing = $true },
        @{ name = 'readiness-default'; kind = 'readiness'; args = @{}; root = $scratch; manifest = "$scratch\external-evidence.template.json"; checklist = "$scratch\external-evidence.checklist.md"; vhd = $false; external = $false; plan = $true },
        @{ name = 'readiness-explicit-root'; kind = 'readiness'; args = @{ PartitionCertificationRoot = 'chosen' }; root = 'chosen'; manifest = 'chosen\external-evidence.template.json'; checklist = 'chosen\external-evidence.checklist.md'; vhd = $false; external = $false; plan = $true },
        @{ name = 'readiness-explicit-checklist'; kind = 'readiness'; args = @{ PartitionExternalEvidenceChecklist = 'chosen/checklist.md' }; root = $scratch; manifest = "$scratch\external-evidence.template.json"; checklist = 'chosen/checklist.md'; vhd = $false; external = $false; plan = $true },
        @{ name = 'readiness-explicit-manifest'; kind = 'readiness'; args = @{ PartitionExternalEvidenceManifest = 'chosen/manifest.json' }; root = $scratch; manifest = 'chosen/manifest.json'; checklist = ''; vhd = $false; external = $false; plan = $true },
        @{ name = 'readiness-strict-vhd'; kind = 'readiness'; args = @{ PartitionCertificationRoot = $strict; RequirePartitionVhdEvidence = $true }; root = $strict; manifest = "$strict\external-evidence.template.json"; checklist = "$strict\external-evidence.checklist.md"; vhd = $true; external = $false; plan = $false },
        @{ name = 'readiness-refuse-strict-plan'; kind = 'readiness'; args = @{ PartitionCertificationRoot = $strict }; error = 'Plan-only readiness cannot write the strict certification root'; noWrites = $true },
        @{ name = 'readiness-refuse-strict-slashes'; kind = 'readiness'; args = @{ PartitionCertificationRoot = 'artifacts/partition-manager-certification/vhd-strict/' }; error = 'Plan-only readiness cannot write the strict certification root'; noWrites = $true },
        @{ name = 'readiness-refuse-strict-dot'; kind = 'readiness'; args = @{ PartitionCertificationRoot = 'artifacts\partition-manager-certification\vhd-strict\.' }; error = 'Plan-only readiness cannot write the strict certification root'; noWrites = $true },
        @{ name = 'readiness-refuse-strict-case'; kind = 'readiness'; args = @{ PartitionCertificationRoot = 'ARTIFACTS\PARTITION-MANAGER-CERTIFICATION\VHD-STRICT' }; error = 'Plan-only readiness cannot write the strict certification root'; noWrites = $true },
        @{ name = 'readiness-refuse-strict-absolute'; kind = 'readiness'; args = @{ PartitionCertificationRoot = $strict }; absoluteRoot = $true; error = 'Plan-only readiness cannot write the strict certification root'; noWrites = $true },
        @{ name = 'readiness-strict-external'; kind = 'readiness'; args = @{ PartitionCertificationRoot = 'chosen'; PartitionExternalEvidenceManifest = 'chosen/manifest.json'; PartitionExternalEvidenceChecklist = 'chosen/checklist.md'; RequirePartitionExternalEvidence = $true }; root = 'chosen'; manifest = 'chosen/manifest.json'; checklist = 'chosen/checklist.md'; vhd = $true; external = $true; plan = $false },
        @{ name = 'readiness-missing-manifest'; kind = 'readiness'; args = @{ PartitionExternalEvidenceManifest = 'missing.json' }; error = 'Partition Manager external evidence manifest missing' },
        @{ name = 'readiness-missing-checklist'; kind = 'readiness'; args = @{ PartitionExternalEvidenceManifest = 'chosen/manifest.json'; PartitionExternalEvidenceChecklist = 'missing.md' }; error = 'Partition Manager external evidence checklist missing' },
        @{ name = 'readiness-required-checklist'; kind = 'readiness'; args = @{ PartitionExternalEvidenceManifest = 'chosen/manifest.json'; RequirePartitionExternalEvidence = $true }; error = 'Partition Manager external evidence checklist required' }
    )
}

function Assert-ReleaseEvidenceSelectionResult {
    param([string]$Fixture, [hashtable]$Case, [object[]]$Output)
    $statusCall = Join-Path $Fixture 'get_partition_manager_certification_status.ps1.call.json'
    if ($Case.ContainsKey('status')) {
        Assert-Condition (-not (Test-Path -LiteralPath $statusCall)) 'Explicit status must bypass evidence generation'
        Assert-Condition ($Output[-1] -ceq (Join-Path $Fixture 'chosen/status.json')) 'Explicit status path changed'
        return
    }
    Assert-Condition (Test-Path -LiteralPath $statusCall -PathType Leaf) 'Status generation was not reached'
    $status = Get-Content -LiteralPath $statusCall -Raw | ConvertFrom-Json
    Assert-Condition ($status.CertificationRoot -ceq $Case.root) "Evidence authority root changed: $($Case.name)"
    Assert-Condition ([string]$status.ExternalEvidenceManifest -ceq $Case.manifest) "External evidence was selected without authority or explicit input changed: $($Case.name)"
    if ($Case.kind -eq 'claim') { return }
    $verification = Get-Content -LiteralPath (Join-Path $Fixture 'verify_partition_manager_certification.ps1.call.json') -Raw | ConvertFrom-Json
    Assert-Condition ($verification.CertificationRoot -ceq $Case.root -and $verification.ExternalEvidenceManifest -ceq $Case.manifest) 'Verification and status evidence differ'
    Assert-Condition ([bool]$verification.RequireVhdDataDiskEvidence -eq $Case.vhd) "VHD requirement changed: $($Case.name)"
    Assert-Condition ([bool]$verification.RequireExternalGateEvidence -eq $Case.external) "External requirement changed: $($Case.name)"
    $planCall = Join-Path $Fixture 'run_partition_manager_destructive_certification.ps1.call.json'
    Assert-Condition ((Test-Path -LiteralPath $planCall) -eq $Case.plan) "Plan-only routing changed: $($Case.name)"
    $strictWrite = Join-Path $Fixture 'artifacts/partition-manager-certification/vhd-strict/plan-only-write.txt'
    Assert-Condition (-not (Test-Path -LiteralPath $strictWrite)) 'Plan-only report contaminated strict evidence root'
    if ($Case.plan) {
        $plan = Get-Content -LiteralPath $planCall -Raw | ConvertFrom-Json
        Assert-Condition ($plan.OutputRoot -ceq $Case.root) 'Plan-only write root changed'
        Assert-Condition (Test-Path -LiteralPath (Join-Path $Fixture ($Case.root + '/plan-only-write.txt'))) 'Plan-only child did not perform its observable fixture write'
    }
    $checklistCall = Join-Path $Fixture 'check_partition_manager_external_checklist.ps1.call.json'
    if ($Case.checklist -eq '') {
        Assert-Condition (-not (Test-Path -LiteralPath $checklistCall)) 'Unrequested external checklist was selected'
    } else {
        $checklist = Get-Content -LiteralPath $checklistCall -Raw | ConvertFrom-Json
        Assert-Condition ($checklist.ChecklistPath -ceq $Case.checklist -and $checklist.ManifestPath -ceq $Case.manifest) 'Explicit checklist or manifest changed'
    }
}

function Test-ReleaseEvidenceSelection {
    param([string]$Directory = '', [string]$CaseName = '')
    if ($Directory -eq '') { $Directory = $PSScriptRoot }
    if ($CaseName -eq '' -or $CaseName -eq 'fixture-write-isolation') {
        $fixture = New-ReleaseEvidenceSelectorFixture $Directory 'claim'
        $outside = Join-Path $Script:RunRoot ('outside-canary-' + [guid]::NewGuid().ToString('N') + '.txt')
        [IO.File]::WriteAllText($outside, 'owned canary must remain unchanged')
        $failure = $null
        try {
            & (Join-Path $fixture 'scripts/get_partition_manager_certification_status.ps1') -OutputPath $outside
        } catch { $failure = $_ }
        Assert-Condition ($null -ne $failure -and $failure.ToString().Contains('Fixture write leaves its unique root')) 'Fixture did not reject an outside output before writing'
        Assert-Condition ([IO.File]::ReadAllText($outside) -ceq 'owned canary must remain unchanged') 'Fixture changed the outside canary'
        Assert-Condition (-not (Test-Path -LiteralPath (Join-Path $fixture 'get_partition_manager_certification_status.ps1.call.json'))) 'Fixture wrote a call receipt before validating every output'
        Write-Host 'PASS release-evidence selection: fixture-write-isolation'
        if ($CaseName -eq 'fixture-write-isolation') { return }
    }
    $cases = @(Get-ReleaseEvidenceSelectionCases)
    if ($CaseName -ne '') {
        $cases = @($cases | Where-Object { $_.name -ceq $CaseName })
        Assert-Condition ($cases.Count -eq 1) "Unknown release-evidence fixture: $CaseName"
    }
    foreach ($case in $cases) {
        $fixture = New-ReleaseEvidenceSelectorFixture $Directory $case.kind
        Push-Location $fixture
        try {
            $failure = $null
            $output = @()
            $arguments = $case.args.Clone()
            if ($case.ContainsKey('absoluteRoot')) { $arguments.PartitionCertificationRoot = Join-Path $fixture $arguments.PartitionCertificationRoot }
            try { $output = @(& (Join-Path $fixture 'selector.ps1') @arguments) } catch { $failure = $_ }
            if ($case.ContainsKey('error')) {
                Assert-Condition ($null -ne $failure -and $failure.ToString().Contains($case.error)) "Evidence selector did not refuse intended input: $($case.name): $failure"
                if ($case.ContainsKey('selectedMissing')) {
                    $call = Get-Content -LiteralPath (Join-Path $fixture 'get_partition_manager_certification_status.ps1.call.json') -Raw | ConvertFrom-Json
                    Assert-Condition ($call.ExternalEvidenceManifest -ceq 'missing.json') 'Missing explicit manifest was replaced'
                }
                if ($case.ContainsKey('noWrites')) {
                    $receipts = @(Get-ChildItem -LiteralPath $fixture -Filter '*.call.json' -File -Recurse)
                    Assert-Condition ($receipts.Count -eq 0) 'Unsafe strict-root mode reached a child before refusal'
                    $strict = Join-Path $fixture 'artifacts/partition-manager-certification/vhd-strict'
                    $strictFiles = @(Get-ChildItem -LiteralPath $strict -File -Recurse)
                    Assert-Condition ($strictFiles.Count -eq 1 -and $strictFiles[0].Name -ceq 'partition-manager-certification-report.json') 'Unsafe strict-root mode wrote an artifact before refusal'
                    Assert-Condition ([IO.File]::ReadAllText($strictFiles[0].FullName) -ceq 'caller-owned synthetic evidence') 'Strict report canary changed'
                }
            } else {
                Assert-Condition ($null -eq $failure) "Evidence selector failed unexpectedly: $($case.name): $failure"
                Assert-ReleaseEvidenceSelectionResult $fixture $case $output
            }
        } finally { Pop-Location }
        Write-Host "PASS release-evidence selection: $($case.name)"
    }
    $count = $cases.Count
    if ($CaseName -eq '') { $count++ }
    Write-Host "PASS release-evidence selection fixtures: $count cases"
}

function Get-FatConversionFunctions {
    param([string]$Directory)
    if ($Directory -eq '') { $Directory = $PSScriptRoot }
    $path = Join-Path $Directory 'run_partition_manager_destructive_certification.ps1'
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors)
    Assert-Condition ($errors.Count -eq 0) 'Converter production script does not parse'
    $parts = @()
    foreach ($name in @('Invoke-FatVolumeConversion', 'Invoke-FatToNtfsScenario', 'Invoke-VhdScenario',
            'Assert-CertificationCondition', 'Complete-ControllerRestoreCleanup', 'Invoke-VhdImageRestoreScenario')) {
        $matches = @($ast.FindAll({ param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name
        }, $true))
        Assert-Condition ($matches.Count -eq 1) "Missing/duplicate converter function: $name"
        $parts += $matches[0].Extent.Text
    }
    $constants = @($ast.EndBlock.Statements | Where-Object {
        $_ -is [Management.Automation.Language.AssignmentStatementAst] -and
        $_.Left.Extent.Text -ceq '$FatConversionTimeoutSeconds'
    })
    # Old-source drills must reach behavior, not fail because the new constant is absent.
    foreach ($constant in $constants) { $parts += $constant.Extent.Text }
    return $parts -join [Environment]::NewLine
}

function Test-FatConversionScenario {
    param([string]$Definitions, [string]$CaseName, [switch]$ReturnResults, [switch]$FailAfterUnknownStop)
    & {
        param($text, $case, $returnResults, $failAfterUnknownStop)
        . ([scriptblock]::Create($text))
        $observed = @{ cleanup = 0; volume = 0; conversion = 0; next = 0; results = @() }
        $oldStop = Get-Variable -Scope Script | Where-Object { $_.Name -ieq 'VhdSafetyStopReason' }
        $oldStopValue = if ($null -ne $oldStop) { $oldStop.Value } else { $null }
        $Script:VhdSafetyStopReason = ''
        $RunVhdDataDiskMatrix = $true
        $VhdScenarioId = ''
        $ConvertPartitionBytes = 256MB
        function New-DisposableVhdDisk { @{ path = 'synthetic-owned-fixture.vhdx'; disk_number = 731 } }
        function Initialize-Disk { if ($case -ceq 'scenario-prelaunch') { throw 'fixture preparation failed' } }
        function New-Partition { [pscustomobject]@{ PartitionNumber = 3 } }
        function Get-CertificationDriveLetter { 'Q' }
        function Format-Volume { }
        function Get-Volume {
            $observed.volume++
            [pscustomobject]@{ FileSystem = $(if ($case -ceq 'scenario-verification-failure') { 'FAT32' } else { 'NTFS' }) }
        }
        function Remove-DisposableVhdDisk {
            param($Vhd)
            Assert-Condition ($Vhd.disk_number -eq 731) 'Cleanup target is not the owned fixture'
            $observed.cleanup++
            if ($case -in @('scenario-dual-failure', 'scenario-cleanup-failure')) { throw 'fixture cleanup failed' }
        }
        function Invoke-FatVolumeConversion {
            param($DriveLetter, $VolumeLabel, $State)
            $observed.conversion++
            Assert-Condition ($DriveLetter -ceq 'Q' -and $VolumeLabel -ceq 'SAKFAT') 'Scenario converter arguments changed'
            if ($case -ceq 'scenario-unknown') {
                if ($null -ne $State) { $State.cleanup_allowed = $false }
                throw 'fixture capture completion unknown'
            }
            if ($case -in @('scenario-native-failure', 'scenario-dual-failure')) { throw 'fixture native exit 37' }
        }
        function Test-Administrator { $true }
        function Assert-EvidenceContract { }
        function New-CertificationResult {
            param($Id, $Name, $Status, $Message, $Evidence)
            [pscustomobject]@{ id = $Id; status = $Status; message = $Message; evidence = $Evidence }
        }
        function Add-CertificationResult { param($Result) $observed.results += $Result }
        try {
            Invoke-VhdScenario 'vhd.fat32-to-ntfs' 'synthetic conversion' ${function:Invoke-FatToNtfsScenario}
            Invoke-VhdScenario 'vhd.quick-partition' 'synthetic next scenario' { $observed.next++; @{} }
            $first = $observed.results[0]
            if ($case -ceq 'scenario-success') {
                Assert-Condition ($first.status -ceq 'Passed' -and $first.evidence.final_file_system -ceq 'NTFS') 'Successful conversion evidence missing'
                Assert-Condition ($observed.volume -eq 1) 'Successful conversion did not verify filesystem'
            } else {
                $expectedVolumeChecks = if ($case -in @('scenario-verification-failure', 'scenario-cleanup-failure')) { 1 } else { 0 }
                Assert-Condition ($first.status -ceq 'Failed' -and $observed.volume -eq $expectedVolumeChecks) 'Failed conversion became success or continued volume verification'
            }
            if ($case -ceq 'scenario-unknown') {
                Assert-Condition ($observed.cleanup -eq 0) 'Unknown conversion worker authorized fixture cleanup'
                Assert-Condition ($observed.next -eq 0 -and $observed.results[1].status -ceq 'Skipped') 'Unknown conversion worker authorized another destructive scenario'
                Assert-Condition ($first.message.Contains('synthetic-owned-fixture.vhdx') -and $first.message.Contains('fixture capture completion unknown')) 'Preservation error lost original failure or fixture identity'
                Assert-Condition ($first.evidence.fat_conversion.cleanup_allowed -eq $false) 'Preservation evidence missing'
                Assert-Condition (-not $failAfterUnknownStop) 'Injected FAT fixture assertion failure'
            } else {
                Assert-Condition ($observed.cleanup -eq 1) 'Completed or prelaunch failure did not clean owned fixture exactly once'
                Assert-Condition ($observed.next -eq 1) 'Known completed conversion unexpectedly blocked remaining scenarios'
            }
            if ($case -ceq 'scenario-prelaunch') { Assert-Condition ($observed.conversion -eq 0) 'Prelaunch failure still launched conversion' }
            if ($case -ceq 'scenario-dual-failure') {
                Assert-Condition ($first.message.Contains('fixture native exit 37') -and $first.message.Contains('fixture cleanup failed')) 'Cleanup failure masked primary converter failure'
            }
            if ($case -ceq 'scenario-verification-failure') { Assert-Condition ($first.message.Contains('FAT32 to NTFS verification failed')) 'Filesystem verification failure was not propagated' }
            if ($case -ceq 'scenario-cleanup-failure') { Assert-Condition ($first.message.Contains('fixture cleanup failed')) 'Cleanup-only failure became a passing scenario' }
        } finally {
            if ($null -eq $oldStop) { Remove-Variable VhdSafetyStopReason -Scope Script }
            else { $Script:VhdSafetyStopReason = $oldStopValue }
        }
        if ($returnResults) { return ,$observed.results }
    } $Definitions $CaseName $ReturnResults $FailAfterUnknownStop
}

function Test-ControllerCleanupSafety {
    param([string]$Definitions, [string]$CaseName, [switch]$FailAfterUnknownStop)
    & {
        param($text, $case, $failAfterUnknownStop)
        . ([scriptblock]::Create($text))
        $observed = @{ cleanup = @(); next = 0; verification = 0; results = @() }
        $oldStop = Get-Variable -Scope Script | Where-Object { $_.Name -ieq 'VhdSafetyStopReason' }
        $oldStopValue = if ($null -ne $oldStop) { $oldStop.Value } else { $null }
        $Script:VhdSafetyStopReason = ''
        $RunVhdDataDiskMatrix = $true
        $VhdScenarioId = ''
        $VhdSizeMB = 768
        function Resolve-ControllerCertifier { @{ path = 'synthetic-certifier-never-executed' } }
        function New-ControllerRestoreDirectory { Join-Path $Script:RunRoot 'synthetic-controller-not-created' }
        function New-DisposableVhdDisk { param($Name) @{ path = "$Name.vhdx"; name = $Name } }
        function Initialize-ControllerRestoreVolume { 'synthetic-volume-marker' }
        function Invoke-ControllerRestore {
            param($State)
            if ($case -ceq 'controller-unknown') {
                $State.cleanup_allowed = $false
                throw 'fixture controller worker completion unknown'
            }
            $State.cleanup_allowed = $true
            $State.worker_descendants_finished = $true
            if ($case -ceq 'controller-known-failure') { throw 'fixture completed controller failure' }
        }
        function Test-ControllerRestoredTarget { $observed.verification++; @{ synthetic_verified = $true } }
        function Remove-DisposableVhdDisk {
            param($Vhd)
            Assert-Condition ($Vhd.name -in @('image-restore-target', 'image-restore-source')) 'Controller cleanup reached an unowned fixture'
            $observed.cleanup += $Vhd.name
            if ($case -ceq 'controller-cleanup-failure' -and $Vhd.name -ceq 'image-restore-target') {
                throw 'fixture controller target cleanup failed'
            }
        }
        function Test-Administrator { $true }
        function Assert-EvidenceContract { }
        function New-CertificationResult {
            param($Id, $Name, $Status, $Message, $Evidence)
            [pscustomobject]@{ id = $Id; status = $Status; message = $Message; evidence = $Evidence }
        }
        function Add-CertificationResult { param($Result) $observed.results += $Result }
        try {
            if ($case -ceq 'controller-existing-stop') {
                $Script:VhdSafetyStopReason = 'fixture earlier unverified worker'
                $state = @{ cleanup_allowed = $true; source = @{ name = 'image-restore-source' }; target = @{ name = 'image-restore-target' } }
                $cleanupErrors = Complete-ControllerRestoreCleanup $state
                Invoke-VhdScenario 'vhd.partition-clone-region' 'synthetic subsequent scenario' { $observed.next++; @{} }
                Assert-Condition ($cleanupErrors.Count -eq 0 -and ($observed.cleanup -join '|') -ceq 'image-restore-target|image-restore-source') 'Confirmed cleanup failed while earlier safety stop was active'
                Assert-Condition ($observed.next -eq 0 -and $observed.results[0].status -ceq 'Skipped' -and $Script:VhdSafetyStopReason -ceq 'fixture earlier unverified worker') 'Confirmed cleanup cleared earlier matrix safety stop'
                return
            }
            Invoke-VhdScenario 'vhd.image-restore' 'synthetic controller restore' ${function:Invoke-VhdImageRestoreScenario}
            Invoke-VhdScenario 'vhd.partition-clone-region' 'synthetic subsequent scenario' { $observed.next++; @{} }
            Assert-Condition ($observed.results.Count -eq 2) 'Controller fixture did not record both scenario outcomes'
            $first = $observed.results[0]
            if ($case -ceq 'controller-unknown') {
                Assert-Condition ($first.status -ceq 'Failed' -and $observed.verification -eq 0) 'Unknown controller completion became success or reached verification'
                Assert-Condition ($observed.cleanup.Count -eq 0) 'Unknown controller completion authorized VHD cleanup'
                Assert-Condition ($observed.next -eq 0 -and $observed.results[1].status -ceq 'Skipped') 'Unknown controller completion authorized another destructive scenario'
                $details = $first.evidence.controller_restore
                Assert-Condition ($details.source_vhd -ceq 'image-restore-source.vhdx' -and $details.target_vhd -ceq 'image-restore-target.vhdx') 'Controller preservation lost owned VHD identities'
                Assert-Condition ($null -eq $details.worker_descendants_finished -and $details.cleanup_errors.Count -eq 0) 'Controller unknown-state evidence changed'
                Assert-Condition ($first.message.Contains('fixture controller worker completion unknown') -and $first.message.Contains($details.report_path)) 'Controller preservation masked original failure/report identity'
                Assert-Condition ($observed.results[1].message.Contains($details.preservation_reason)) 'Controller safety stop lost preservation reason'
                Assert-Condition (-not $failAfterUnknownStop) 'Injected controller fixture assertion failure'
            } else {
                Assert-Condition (($observed.cleanup -join '|') -ceq 'image-restore-target|image-restore-source') 'Confirmed controller completion did not clean both fixtures in order'
                Assert-Condition ($observed.next -eq 1 -and $observed.results[1].status -ceq 'Passed') 'Confirmed controller completion incorrectly halted later scenarios'
                if ($case -ceq 'controller-known-success') {
                    Assert-Condition ($first.status -ceq 'Passed' -and $observed.verification -eq 1) 'Known controller success lost verification/result'
                } else {
                    $message = if ($case -ceq 'controller-known-failure') { 'fixture completed controller failure' } else { 'fixture controller target cleanup failed' }
                    Assert-Condition ($first.status -ceq 'Failed' -and $first.message.Contains($message)) 'Known controller failure lost failure evidence'
                }
            }
        } finally {
            if ($null -eq $oldStop) { Remove-Variable VhdSafetyStopReason -Scope Script }
            else { $Script:VhdSafetyStopReason = $oldStopValue }
        }
    } $Definitions $CaseName $FailAfterUnknownStop
}

function Test-CertificationFixtureStopIsolation {
    param([string]$Definitions)
    $prior = Get-Variable -Scope Script | Where-Object { $_.Name -ieq 'VhdSafetyStopReason' }
    $priorValue = if ($null -ne $prior) { $prior.Value } else { $null }
    $fixtures = @(
        @{ name = 'FAT'; command = 'Test-FatConversionScenario'; case = 'scenario-unknown'; failure = 'Injected FAT fixture assertion failure' },
        @{ name = 'controller'; command = 'Test-ControllerCleanupSafety'; case = 'controller-unknown'; failure = 'Injected controller fixture assertion failure' }
    )
    $variableName = 'vHdSaFeTyStOpReAsOn'
    $expectedValue = 'earlier mixed-case run stop'
    $variants = @(
        @{ exists = $false; assertion_failure = $false }, @{ exists = $false; assertion_failure = $true },
        @{ exists = $true; assertion_failure = $false }, @{ exists = $true; assertion_failure = $true }
    )
    try {
        foreach ($fixture in $fixtures) {
            foreach ($variant in $variants) {
                if ($null -ne (Get-Variable -Scope Script | Where-Object { $_.Name -ieq 'VhdSafetyStopReason' })) {
                    Remove-Variable VhdSafetyStopReason -Scope Script
                }
                if ($variant.exists) { Set-Variable -Name $variableName -Value $expectedValue -Scope Script }
                $failure = $null
                try {
                    & $fixture.command -Definitions $Definitions -CaseName $fixture.case -FailAfterUnknownStop:$variant.assertion_failure
                } catch { $failure = $_ }
                $context = "$($fixture.name), exists=$($variant.exists), assertion_failure=$($variant.assertion_failure)"
                if ($variant.assertion_failure) {
                    Assert-Condition ($null -ne $failure -and $failure.Exception.Message -ceq $fixture.failure) "Fixture did not exercise intended assertion failure: $context"
                } else { Assert-Condition ($null -eq $failure) "Fixture isolation baseline failed: $context`: $failure" }
                $after = Get-Variable -Scope Script | Where-Object { $_.Name -ieq 'VhdSafetyStopReason' }
                Assert-Condition (($null -ne $after) -eq $variant.exists) "Fixture run-stop presence was not restored: $context"
                if ($variant.exists) {
                    Assert-Condition ($after.Value -ceq $expectedValue -and $after.Name -ceq $variableName) "Fixture run-stop value/casing was not restored: $context"
                }
            }
        }
    } finally {
        if ($null -ne (Get-Variable -Scope Script | Where-Object { $_.Name -ieq 'VhdSafetyStopReason' })) {
            Remove-Variable VhdSafetyStopReason -Scope Script
        }
        if ($null -ne $prior) { Set-Variable -Name $prior.Name -Value $priorValue -Scope Script }
    }
    Write-Host 'PASS fixture stop isolation: two real helpers, absent/mixed-case state, normal/assertion exits'
}

function Test-FatConversionContract {
    param([string]$Definitions, [string]$CaseName)
    & {
        param($text, $case)
        . ([scriptblock]::Create($text))
        $observed = @{ resolved = 0; launched = 0 }
        function Resolve-SystemToolPath {
            param($Name)
            Assert-Condition ($Name -ceq 'convert.exe') 'Converter no longer resolves the system executable'
            $observed.resolved++
            'synthetic-bound-converter.exe'
        }
        function Invoke-CapturedProcess {
            param($FilePath, $Arguments, $InputText, $TimeoutSeconds)
            $observed.launched++
            Assert-Condition ($FilePath -ceq 'synthetic-bound-converter.exe') 'Converter selected a different executable'
            Assert-Condition (($Arguments -join '|') -ceq 'Q:|/FS:NTFS|/NoSecurity|/X') 'Converter argument contract changed'
            Assert-Condition ($InputText -ceq ("SAKFAT" + [Environment]::NewLine)) 'Converter input omitted exactly one label newline'
            Assert-Condition ($TimeoutSeconds -eq 120) 'Converter finite named timeout was omitted or changed'
            if ($case -ceq 'capture-throw') { throw 'fixture capture failure' }
            [pscustomobject]@{ timed_out = ($case -ceq 'timeout-result'); exit_code = $(if ($case -ceq 'native-failure') { 37 } else { 0 }); stdout = 'fixture stdout'; stderr = 'fixture stderr' }
        }
        $letters = if ($case -ceq 'invalid-drive') { @('q', "Q`n", "Q`r", 'QQ', 'Q:', ' Q', 'Q ', 'Q:/X', '1', ([string][char]0xFF31)) } else { @('Q') }
        foreach ($letter in $letters) {
            $state = @{ cleanup_allowed = $true }
            $failure = $null
            $output = $null
            try { $output = Invoke-FatVolumeConversion -DriveLetter $letter -VolumeLabel 'SAKFAT' -State $state }
            catch { $failure = $_ }
            if ($case -ceq 'invalid-drive') {
                Assert-Condition ($null -ne $failure -and $failure.ToString().Contains('Invalid conversion drive letter')) 'Malformed drive letter reached converter admission'
                Assert-Condition ($observed.resolved -eq 0 -and $observed.launched -eq 0 -and $state.cleanup_allowed) 'Malformed drive input reached resolution/launch or changed cleanup authority'
            } elseif ($case -ceq 'contract-success') {
                Assert-Condition ($null -eq $failure) "Converter contract failed: $failure"
                Assert-Condition ($output -ceq ('fixture stdout' + [Environment]::NewLine + 'fixture stderr')) 'Converter dropped stdout or stderr'
                Assert-Condition $state.cleanup_allowed 'Successful converter did not restore cleanup authority'
            } else {
                $expected = switch ($case) { 'capture-throw' { 'fixture capture failure' }; 'timeout-result' { 'conversion deadline' }; 'native-failure' { 'exit code 37' } }
                Assert-Condition ($null -ne $failure -and $failure.ToString().Contains($expected)) "Converter did not propagate $case`: $failure"
                Assert-Condition ($state.cleanup_allowed -eq ($case -ceq 'native-failure')) 'Converter restored cleanup authority without verified completion'
                if ($case -ne 'capture-throw') { Assert-Condition ($failure.ToString().Contains('fixture stdout') -and $failure.ToString().Contains('fixture stderr')) 'Converter failure dropped output evidence' }
            }
        }
    } $Definitions $CaseName
}

function Test-FatConversion {
    param([string]$Directory = '', [string]$CaseName = '')
    $definitions = Get-FatConversionFunctions $Directory
    $cases = @('contract-success', 'invalid-drive', 'capture-throw', 'timeout-result', 'native-failure',
        'scenario-success', 'scenario-prelaunch', 'scenario-native-failure', 'scenario-unknown', 'scenario-dual-failure',
        'scenario-verification-failure', 'scenario-cleanup-failure',
        'controller-unknown', 'controller-known-success', 'controller-known-failure', 'controller-cleanup-failure', 'controller-existing-stop',
        'native-success', 'native-failure-output', 'native-timeout', 'native-stdin-deadline', 'native-safety-stop-exit', 'fixture-stop-isolation')
    if ($CaseName -ne '') { $cases = @($cases | Where-Object { $_ -ceq $CaseName }) }
    Assert-Condition ($cases.Count -gt 0) "Unknown converter fixture: $CaseName"
    $converter = $null
    if (@($cases | Where-Object { $_ -in @('native-success', 'native-failure-output', 'native-timeout', 'native-stdin-deadline') }).Count -gt 0) {
        $converter = New-FatConversionExecutableFixture
    }
    foreach ($case in $cases) {
        if ($case.StartsWith('scenario-')) { Test-FatConversionScenario $definitions $case }
        elseif ($case.StartsWith('controller-')) { Test-ControllerCleanupSafety $definitions $case }
        elseif ($case -ceq 'fixture-stop-isolation') { Test-CertificationFixtureStopIsolation $definitions }
        elseif ($case -ceq 'native-safety-stop-exit') { Test-FatConversionFailureExit $Directory }
        elseif ($case.StartsWith('native-') -and $case -cne 'native-failure') { Test-FatConversionNative $definitions $case $converter }
        else { Test-FatConversionContract $definitions $case }
        Write-Host "PASS FAT converter: $case"
    }
    Write-Host "PASS FAT converter fixtures: $($cases.Count) cases; no storage tools executed"
}

function Invoke-FatConversionExitWorker {
    param([string]$Directory)
    if ($Directory -eq '') { $Directory = $PSScriptRoot }
    $definitions = Get-FatConversionFunctions $Directory
    $Script:Results = Test-FatConversionScenario $definitions 'scenario-unknown' -ReturnResults
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $Directory 'run_partition_manager_destructive_certification.ps1'), [ref]$tokens, [ref]$errors)
    Assert-Condition ($errors.Count -eq 0) 'Matrix completion source does not parse'
    $tries = @($ast.EndBlock.Statements | Where-Object { $_ -is [Management.Automation.Language.TryStatementAst] })
    Assert-Condition ($tries.Count -eq 1) 'Expected one production matrix body'
    $statements = @($tries[0].Body.Statements)
    $counts = @($statements | Where-Object {
        $_ -is [Management.Automation.Language.AssignmentStatementAst] -and
        $_.Left.Extent.Text -in @('$failed', '$passed', '$skipped', '$external', '$skippedVhd', '$status')
    })
    Assert-Condition ($counts.Count -eq 6) 'Matrix summary/claim assignment selection changed'
    $failureExit = @($statements | Where-Object {
        $_ -is [Management.Automation.Language.IfStatementAst] -and
        $_.Clauses[0].Item1.Extent.Text -ceq '$failed.Count -gt 0'
    })
    Assert-Condition ($failureExit.Count -eq 1) 'Matrix failure exit selection changed'
    $RequireVhdDataDiskEvidence = $true
    . ([scriptblock]::Create(($counts.Extent.Text -join [Environment]::NewLine)))
    Assert-Condition ($status -ceq 'Failed' -and $failed.Count -eq 1 -and $skipped.Count -eq 1) 'Unknown worker did not produce failed matrix status'
    [pscustomobject]@{ status = $status; results = $Script:Results } | ConvertTo-Json -Depth 6 |
        Set-Content -LiteralPath (Join-Path $Script:RunRoot 'fat-unknown-matrix.json') -Encoding UTF8
    Write-Host "Observed converter matrix status: $status"
    # Execute the real terminal branch in a child host; an exit removed by a drill
    # must return normally and be rejected by the parent, not kill the test runner.
    . ([scriptblock]::Create($failureExit[0].Extent.Text))
}

function Test-FatConversionFailureExit {
    param([string]$Directory)
    . (Join-Path $PSScriptRoot 'process_capture.ps1')
    $shell = (Get-Process -Id $PID).Path
    $output = Join-Path $Script:RunRoot ('fat-exit-child-' + [guid]::NewGuid().ToString('N'))
    $arguments = @('-NoProfile', '-NonInteractive', '-File', $PSCommandPath,
        '-FatConversionExitWorker', '-OutputRoot', $output)
    if ($Directory -ne '') { $arguments += @('-FatConversionScriptDirectory', $Directory) }
    $captured = Invoke-CapturedProcess -FilePath $shell -Arguments $arguments -TimeoutSeconds 30
    $reports = @(Get-ChildItem -LiteralPath $output -Filter 'fat-unknown-matrix.json' -File -Recurse)
    Assert-Condition ($reports.Count -eq 1) "Matrix child did not write exactly one observed report: $($captured.stderr)"
    $report = Get-Content -LiteralPath $reports[0].FullName -Raw | ConvertFrom-Json
    Assert-Condition ($report.status -ceq 'Failed' -and $report.results[0].status -ceq 'Failed' -and $report.results[1].status -ceq 'Skipped') 'Matrix child lost failed/skipped outcomes'
    Assert-Condition (-not $captured.timed_out -and $captured.exit_code -eq 1 -and $captured.stdout.Contains('Observed converter matrix status: Failed')) 'Unknown conversion worker did not propagate the real matrix nonzero exit'
}

function New-FatConversionExecutableFixture {
    $directory = Join-Path $Script:RunRoot ('fat-converter-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $directory -ErrorAction Stop | Out-Null
    $source = Join-Path $directory 'converter.cs'
    $executable = Join-Path $directory 'synthetic-converter.exe'
    # This program only observes its arguments, pipes and own process identity. It has
    # no volume/disk APIs. Independent expiry bounds old-production deadlock experiments.
    $program = @'
using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading;
public static class SyntheticConverter {
    public static int Main(string[] args) {
        string root = AppDomain.CurrentDomain.BaseDirectory;
        string mode = File.ReadAllText(Path.Combine(root, "mode.txt"));
        Process self = Process.GetCurrentProcess();
        File.WriteAllLines(Path.Combine(root, "started.txt"), new [] {
            self.Id.ToString(), self.StartTime.ToUniversalTime().Ticks.ToString() });
        Thread expiry = new Thread(delegate() {
            Thread.Sleep(8000);
            File.WriteAllText(Path.Combine(root, "expired.txt"), "Independent fixture expiry");
            Environment.Exit(91);
        });
        expiry.IsBackground = true;
        expiry.Start();
        File.WriteAllLines(Path.Combine(root, "arguments.txt"), args);
        if (mode == "native-stdin-deadline") Thread.Sleep(Timeout.Infinite);
        string input = Console.In.ReadToEnd();
        File.WriteAllText(Path.Combine(root, "stdin.txt"), input, new UTF8Encoding(false));
        if (mode == "native-timeout") Thread.Sleep(Timeout.Infinite);
        Console.Error.Write(new string('E', 131072) + ":stderr-finished");
        Console.Error.Flush();
        Console.Out.Write("stdout-finished");
        Console.Out.Flush();
        return mode == "native-failure-output" ? 37 : 0;
    }
}
'@
    [IO.File]::WriteAllText($source, $program, [Text.UTF8Encoding]::new($false))
    $compiler = Join-Path ([Environment]::GetFolderPath('Windows')) 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
    Assert-Condition (Test-Path -LiteralPath $compiler -PathType Leaf) 'Windows framework fixture compiler missing'
    . (Join-Path $PSScriptRoot 'process_capture.ps1')
    $compiled = Invoke-CapturedProcess -FilePath $compiler -Arguments @('/nologo', '/warnaserror+', '/target:exe', "/out:$executable", $source) -TimeoutSeconds 30
    Assert-Condition (-not $compiled.timed_out -and $compiled.exit_code -eq 0 -and (Test-Path -LiteralPath $executable -PathType Leaf)) "Converter fixture compilation failed: $($compiled.stdout) $($compiled.stderr)"
    return @{ path = $executable; hash = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash; directory = $directory }
}

function Assert-FatFixtureProcessStopped {
    param([string]$Directory)
    $identity = [IO.File]::ReadAllLines((Join-Path $Directory 'started.txt'))
    Assert-Condition ($identity.Count -eq 2) 'Synthetic converter process identity was not recorded'
    $owned = $null
    try { $owned = [Diagnostics.Process]::GetProcessById([int]$identity[0]) }
    catch [ArgumentException] { return }
    try {
        Assert-Condition ($owned.HasExited -or $owned.StartTime.ToUniversalTime().Ticks -ne [long]$identity[1]) 'Bound synthetic converter still running after capture returned'
    } finally { $owned.Dispose() }
}

function Test-FatConversionNative {
    param([string]$Definitions, [string]$CaseName, [hashtable]$Converter)
    $directory = Join-Path $Converter.directory $CaseName
    New-Item -ItemType Directory -Path $directory -ErrorAction Stop | Out-Null
    $executable = Join-Path $directory 'synthetic-converter.exe'
    Copy-Item -LiteralPath $Converter.path -Destination $executable -ErrorAction Stop
    [IO.File]::WriteAllText((Join-Path $directory 'mode.txt'), $CaseName)
    Assert-Condition ((Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash -ceq $Converter.hash) 'Synthetic converter binding changed before launch'
    $observation = & {
        param($text, $case, $program)
        . (Join-Path $PSScriptRoot 'process_capture.ps1')
        . ([scriptblock]::Create($text))
        $captureBody = ${function:Invoke-CapturedProcess}
        function Invoke-CapturedProcess {
            param($FilePath, $Arguments, $InputText, $TimeoutSeconds)
            Assert-Condition ($FilePath -ceq $program) 'Native converter fixture refused an unbound executable'
            & $captureBody @PSBoundParameters
        }
        function Resolve-SystemToolPath {
            param($Name)
            Assert-Condition ($Name -ceq 'convert.exe') 'Native fixture received a different tool request'
            return $program
        }
        # A one-second fixture budget exercises the same production timeout argument;
        # the pure contract case independently pins the real 120-second finite budget.
        $FatConversionTimeoutSeconds = 1
        $state = @{ cleanup_allowed = $true }
        $label = if ($case -ceq 'native-stdin-deadline') { 'L' * 1MB } else { 'SAKFAT' }
        $failure = $null
        $output = $null
        $conversionArguments = @{ DriveLetter = 'Q'; VolumeLabel = $label }
        # The historical converter predates cleanup-state reporting. Exercise that
        # actual implementation without failing on the newly added argument first.
        if ((Get-Command Invoke-FatVolumeConversion).Parameters.ContainsKey('State')) {
            $conversionArguments.State = $state
        }
        $watch = [Diagnostics.Stopwatch]::StartNew()
        try { $output = Invoke-FatVolumeConversion @conversionArguments }
        catch { $failure = $_.Exception.Message }
        $watch.Stop()
        [pscustomobject]@{ output = $output; failure = $failure; elapsed_ms = $watch.ElapsedMilliseconds; cleanup_allowed = $state.cleanup_allowed }
    } $Definitions $CaseName $executable
    $observation | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $directory 'observation.json') -Encoding UTF8
    Assert-FatFixtureProcessStopped $directory
    Assert-Condition ((Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash -ceq $Converter.hash) 'Synthetic converter changed during execution'
    $arguments = [IO.File]::ReadAllLines((Join-Path $directory 'arguments.txt'))
    Assert-Condition (($arguments -join '|') -ceq 'Q:|/FS:NTFS|/NoSecurity|/X') 'Native converter did not observe preserved arguments'
    if ($CaseName -cne 'native-stdin-deadline') {
        Assert-Condition ([IO.File]::ReadAllText((Join-Path $directory 'stdin.txt')) -ceq ('SAKFAT' + [Environment]::NewLine)) 'Native converter did not receive exact label newline and EOF'
    }
    if ($CaseName -in @('native-timeout', 'native-stdin-deadline')) {
        Assert-Condition ($null -ne $observation.failure -and $observation.failure -match '(?i)deadline|timed-out') "Native converter did not report its bounded deadline: $($observation.failure)"
        Assert-Condition (-not $observation.cleanup_allowed) 'Uncertain native conversion authorized cleanup'
        Assert-Condition (-not (Test-Path -LiteralPath (Join-Path $directory 'expired.txt'))) 'Converter waited for independent fixture expiry instead of its own deadline'
    } else {
        $expected = 'stdout-finished' + [Environment]::NewLine + ('E' * 131072) + ':stderr-finished'
        if ($CaseName -ceq 'native-success') {
            Assert-Condition ($null -eq $observation.failure -and $observation.output -ceq $expected) "Native converter concurrent stdout/stderr capture failed: $($observation.failure)"
        } else {
            Assert-Condition ($observation.failure -ceq ("convert.exe failed with exit code 37`n" + $expected)) 'Native converter failure lost exit code or either output stream'
        }
        Assert-Condition $observation.cleanup_allowed 'Normally completed native converter did not restore cleanup authority'
    }
    Write-Host "Native converter observation: $CaseName, $($observation.elapsed_ms) ms, $directory"
}

function Write-ExternalEvidenceReports {
    param(
        [Parameter(Mandatory = $true)]
        [string]$EvidenceRoot,
        [Parameter(Mandatory = $true)]
        [string]$ArtifactPath
    )

    foreach ($id in $ExpectedExternalGateIds) {
        $gateDirectory = Join-Path $EvidenceRoot $id
        $templatePath = Join-Path $gateDirectory "report.template.json"
        $reportPath = Join-Path $gateDirectory "report.json"
        $report = Get-Content -LiteralPath $templatePath -Raw | ConvertFrom-Json
        Set-ObjectProperty -Object $report -Name "status" -Value "Passed"
        Set-ObjectProperty -Object $report -Name "completed_utc" -Value (Get-Date).ToUniversalTime().ToString("o")
        Set-ObjectProperty -Object $report -Name "evidence" -Value (New-SyntheticEvidence -Id $id)
        Set-ObjectProperty -Object $report -Name "artifacts" -Value @($ArtifactPath)
        Set-ObjectProperty -Object $report -Name "verification_summary" -Value "synthetic external report import self-test"
        Set-ObjectProperty -Object $report -Name "operator_notes" -Value "synthetic report"
        $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportPath -Encoding UTF8
    }
}

$Script:RunPassed = $false
Push-Location $ProjectRoot
try {
    $resolvedOutputRoot = if ([System.IO.Path]::IsPathRooted($OutputRoot)) {
        $OutputRoot
    }
    else {
        Join-Path $ProjectRoot $OutputRoot
    }
    # Bounded retention: a passing run removes its own folder; each run first caps the folders
    # earlier failed runs left behind. The name stays run-<timestamp>: fixture paths under it
    # already sit near MAX_PATH (a .NET Framework converter fixture's .exe.config crossed 260
    # characters when a GUID suffix was tried). Without -Force, a second run started in the
    # same second fails closed instead of sharing, and later deleting, this run's folder.
    . (Join-Path $PSScriptRoot "run_folder_retention.ps1")
    $runFolderPattern = '^run-\d{8}-\d{6}$'
    $null = Limit-RetainedTestRunFolders -OutputRoot $resolvedOutputRoot -NamePattern $runFolderPattern
    $Script:RunRoot = Join-Path $resolvedOutputRoot ("run-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
    New-Item -ItemType Directory -Path $Script:RunRoot -ErrorAction Stop | Out-Null

    # The exit worker is a child whose expected outcome is a failure exit; its parent owns and
    # removes the enclosing run folder, so the worker never marks its own run as passed.
    if ($FatConversionExitWorker) { Invoke-FatConversionExitWorker $FatConversionScriptDirectory; return }
    if ($FatConversionOnly) { Test-FatConversion $FatConversionScriptDirectory $FatConversionCase; $Script:RunPassed = $true; return }
    if ($ReleaseEvidenceSelectionOnly) { Test-ReleaseEvidenceSelection $ReleaseEvidenceScriptDirectory $ReleaseEvidenceCase; $Script:RunPassed = $true; return }
    if ($ReleaseClaimsOnly) { Test-ReleaseClaimCurrentState; Test-ReleaseEvidenceSelection; $Script:RunPassed = $true; return }
    Test-CertificationReportSelection
    if ($ReportSelectionOnly) {
        Write-Host "Partition Manager report-selection tests passed."
        $Script:RunPassed = $true
        return
    }
    Test-CertificationEvidenceReferences
    if ($EvidenceReferencesOnly) { $Script:RunPassed = $true; return }
    Initialize-SyntheticControllerEvidence
    if ($EvidenceValuesOnly) { Test-CertificationAllowedValueArrays; $Script:RunPassed = $true; return }
    Test-ControllerEvidenceConsumers
    if ($ControllerEvidenceOnly) { $Script:RunPassed = $true; return }
    Test-CertificationAllowedValueArrays
    Test-ReleaseClaimCurrentState
    Test-ReleaseEvidenceSelection
    Test-FatConversion
    Assert-NoCertificationFailureSilences
    Test-PartitionManagerStorageHelpers

    $vhdPreflightPath = Join-Path $Script:RunRoot "vhd-preflight.json"
    & scripts\test_partition_manager_vhd_preflight.ps1 `
        -OutputRoot $Script:RunRoot `
        -OutputPath $vhdPreflightPath `
        -RequireAdministrator `
        -Quiet
    Assert-Condition -Condition (Test-Path -LiteralPath $vhdPreflightPath) -Message "VHD preflight report was not generated"
    & scripts\check_partition_manager_vhd_preflight_report.ps1 -ReportPath $vhdPreflightPath

    $scaffoldManifest = Join-Path $Script:RunRoot "generated-external-scaffold.json"
    $scaffoldChecklist = Join-Path $Script:RunRoot "generated-external-checklist.md"
    $scaffoldEvidenceRoot = Join-Path $Script:RunRoot "generated-external-evidence"
    & scripts\new_partition_manager_external_evidence_manifest.ps1 `
        -OutputPath $scaffoldManifest `
        -ChecklistPath $scaffoldChecklist `
        -EvidenceRoot $scaffoldEvidenceRoot `
        -CreateEvidenceDirectories `
        -Force
    Assert-Condition -Condition (Test-Path -LiteralPath $scaffoldManifest) -Message "external scaffold manifest was not generated"
    Assert-Condition -Condition (Test-Path -LiteralPath $scaffoldChecklist) -Message "external scaffold checklist was not generated"
    Assert-Condition -Condition (Test-Path -LiteralPath $scaffoldEvidenceRoot -PathType Container) -Message "external scaffold evidence package was not generated"
    $generatedManifest = Get-Content -LiteralPath $scaffoldManifest -Raw | ConvertFrom-Json
    Assert-Condition -Condition (@($generatedManifest.results).Count -eq $ExpectedExternalGateIds.Count) -Message "external scaffold manifest gate count mismatch"
    $generatedChecklist = Get-Content -LiteralPath $scaffoldChecklist -Raw
    foreach ($id in $ExpectedExternalGateIds) {
        Assert-Condition -Condition ($generatedChecklist.Contains($id)) -Message "external scaffold checklist missing gate ID: $id"
        $gate = $generatedManifest.results | Where-Object { $_.id -eq $id }
        Assert-Condition -Condition ($null -ne $gate) -Message "external scaffold manifest missing gate: $id"
        foreach ($key in @((Get-ScenarioSpec -Id $id).required_evidence_keys)) {
            Assert-Condition -Condition ($null -ne $gate.evidence.PSObject.Properties[$key]) -Message "external scaffold manifest missing evidence key: $id $key"
            Assert-Condition -Condition ($generatedChecklist.Contains($key)) -Message "external scaffold checklist missing evidence key: $id $key"
        }
        foreach ($contract in @((Get-ScenarioSpec -Id $id).safety_contract)) {
            Assert-Condition -Condition ($generatedChecklist.Contains($contract)) -Message "external scaffold checklist missing safety contract: $id $contract"
        }
        $spec = Get-ScenarioSpec -Id $id
        if ($null -ne $spec.PSObject.Properties["required_evidence_values"]) {
            Assert-Condition -Condition ($null -ne $gate.required_evidence_values) -Message "external scaffold manifest missing required evidence value contract: $id"
            foreach ($requirementProperty in @($spec.required_evidence_values.PSObject.Properties)) {
                Assert-Condition -Condition ($null -ne $gate.required_evidence_values.PSObject.Properties[$requirementProperty.Name]) -Message "external scaffold manifest missing required evidence value key: $id $($requirementProperty.Name)"
                foreach ($allowedValue in @($requirementProperty.Value.allowed_values)) {
                    Assert-Condition -Condition ($generatedChecklist.Contains($allowedValue.ToString())) -Message "external scaffold checklist missing allowed evidence value: $id $allowedValue"
                }
            }
        }
    }
    & scripts\check_partition_manager_external_checklist.ps1 -ChecklistPath $scaffoldChecklist -ManifestPath $scaffoldManifest
    & scripts\check_partition_manager_external_lab_package.ps1 -EvidenceRoot $scaffoldEvidenceRoot -ManifestPath $scaffoldManifest

    Assert-ToolFails -Name "missing lab package verifier" -Body {
        & scripts\check_partition_manager_external_lab_package.ps1 -EvidenceRoot (Join-Path $Script:RunRoot "missing-external-evidence") -ManifestPath $scaffoldManifest
    }

    $badLabManifest = Join-Path $Script:RunRoot "bad-external-lab-scaffold.json"
    $badLabChecklist = Join-Path $Script:RunRoot "bad-external-lab-checklist.md"
    $badLabRoot = Join-Path $Script:RunRoot "bad-external-lab-evidence"
    & scripts\new_partition_manager_external_evidence_manifest.ps1 `
        -OutputPath $badLabManifest `
        -ChecklistPath $badLabChecklist `
        -EvidenceRoot $badLabRoot `
        -CreateEvidenceDirectories `
        -Force
    $badLabReadme = Join-Path $badLabRoot "$($ExpectedExternalGateIds[0])\README.md"
    $badLabText = (Get-Content -LiteralPath $badLabReadme -Raw).Replace(
        "Fill every manifest evidence key with a non-empty value.",
        "Fill stale placeholder values.")
    $badLabText | Set-Content -LiteralPath $badLabReadme -Encoding UTF8
    Assert-ToolFails -Name "stale lab package verifier" -Body {
        & scripts\check_partition_manager_external_lab_package.ps1 -EvidenceRoot $badLabRoot -ManifestPath $badLabManifest
    }

    $firstExternalGate = Get-ScenarioSpec -Id $ExpectedExternalGateIds[0]
    $missingChecklistGate = Join-Path $Script:RunRoot "external-checklist-missing-gate.md"
    Copy-Item -LiteralPath $scaffoldChecklist -Destination $missingChecklistGate -Force
    $missingChecklistGateText = (Get-Content -LiteralPath $missingChecklistGate -Raw).Replace(
        "## $($firstExternalGate.id) - $($firstExternalGate.name)",
        "## external.stale-placeholder - Stale placeholder gate")
    $missingChecklistGateText | Set-Content -LiteralPath $missingChecklistGate -Encoding UTF8
    Assert-ToolFails -Name "missing checklist gate verifier" -Body {
        & scripts\check_partition_manager_external_checklist.ps1 -ChecklistPath $missingChecklistGate -ManifestPath $scaffoldManifest
    }

    $missingChecklistEvidence = Join-Path $Script:RunRoot "external-checklist-missing-evidence.md"
    Copy-Item -LiteralPath $scaffoldChecklist -Destination $missingChecklistEvidence -Force
    $firstExternalEvidenceKey = @($firstExternalGate.required_evidence_keys)[0]
    $missingChecklistEvidenceText = (Get-Content -LiteralPath $missingChecklistEvidence -Raw).Replace(
        "| ``$firstExternalEvidenceKey`` |  |  |",
        "| ``stale_$firstExternalEvidenceKey`` |  |  |")
    $missingChecklistEvidenceText | Set-Content -LiteralPath $missingChecklistEvidence -Encoding UTF8
    Assert-ToolFails -Name "missing checklist evidence verifier" -Body {
        & scripts\check_partition_manager_external_checklist.ps1 -ChecklistPath $missingChecklistEvidence -ManifestPath $scaffoldManifest
    }

    $valueChecklistGateId = @($ExpectedExternalGateIds | Where-Object {
            $null -ne (Get-ScenarioSpec -Id $_).PSObject.Properties["required_evidence_values"]
        } | Select-Object -First 1)[0]
    $valueChecklistGate = Get-ScenarioSpec -Id $valueChecklistGateId
    $valueChecklistRequirement = @($valueChecklistGate.required_evidence_values.PSObject.Properties)[0]
    $valueChecklistAllowed = @($valueChecklistRequirement.Value.allowed_values)[0].ToString()
    $badChecklistValue = Join-Path $Script:RunRoot "external-checklist-bad-required-value.md"
    Copy-Item -LiteralPath $scaffoldChecklist -Destination $badChecklistValue -Force
    $badChecklistValueText = (Get-Content -LiteralPath $badChecklistValue -Raw).Replace(
        "Allowed: $valueChecklistAllowed",
        "Allowed: stale-mode")
    $badChecklistValueText | Set-Content -LiteralPath $badChecklistValue -Encoding UTF8
    Assert-ToolFails -Name "bad checklist required value verifier" -Body {
        & scripts\check_partition_manager_external_checklist.ps1 -ChecklistPath $badChecklistValue -ManifestPath $scaffoldManifest
    }

    $pendingManifest = Join-Path $Script:RunRoot "external-pending.json"
    Write-ExternalManifest -Path $pendingManifest -Status "Pending"

    $evidenceFile = Join-Path $Script:RunRoot "external-evidence.txt"
    Set-Content -LiteralPath $evidenceFile -Value "synthetic external evidence" -Encoding ASCII
    $passedManifest = Join-Path $Script:RunRoot "external-passed.json"
    Write-ExternalManifest -Path $passedManifest -Status "Passed" -EvidencePath $evidenceFile
    Write-ExternalEvidenceReports -EvidenceRoot $scaffoldEvidenceRoot -ArtifactPath $evidenceFile
    $importedFromReportsManifest = Join-Path $Script:RunRoot "external-imported-from-reports.json"
    & scripts\update_partition_manager_external_manifest_from_reports.ps1 `
        -ManifestPath $scaffoldManifest `
        -EvidenceRoot $scaffoldEvidenceRoot `
        -OutputPath $importedFromReportsManifest `
        -RequireAllReports `
        -Force `
        -Quiet
    $importedFromReports = Get-Content -LiteralPath $importedFromReportsManifest -Raw | ConvertFrom-Json
    Assert-Condition -Condition ([int]$importedFromReports.imported_report_count -eq $ExpectedExternalGateIds.Count) -Message "external report import count mismatch"
    foreach ($result in @($importedFromReports.results)) {
        Assert-Condition -Condition ($result.status -eq "Passed") -Message "external report import did not mark result passed: $($result.id)"
        Assert-Condition -Condition ($result.evidence_path -like "*$($result.id)*report.json") -Message "external report import did not point evidence_path at report.json: $($result.id)"
    }

    $staleImportManifest = Join-Path $Script:RunRoot "stale-external-report-import.json"
    $staleImportChecklist = Join-Path $Script:RunRoot "stale-external-report-import.checklist.md"
    $staleImportedManifest = Join-Path $Script:RunRoot "stale-external-report-import.imported.json"
    $staleImportedChecklist = Join-Path $Script:RunRoot "stale-external-report-import.imported.checklist.md"
    Copy-Item -LiteralPath $scaffoldManifest -Destination $staleImportManifest -Force
    Copy-Item -LiteralPath $scaffoldChecklist -Destination $staleImportChecklist -Force
    $staleGate = Get-ScenarioSpec -Id "external.allocate-free-space"
    $staleManifest = Get-Content -LiteralPath $staleImportManifest -Raw | ConvertFrom-Json
    $staleResult = @($staleManifest.results | Where-Object { $_.id -eq $staleGate.id })[0]
    $staleResult.name = "Allocate Free Space donor-volume move/extend proof"
    $staleManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $staleImportManifest -Encoding UTF8
    $staleChecklistText = (Get-Content -LiteralPath $staleImportChecklist -Raw).Replace(
        "## $($staleGate.id) - $($staleGate.name)",
        "## $($staleGate.id) - Allocate Free Space donor-volume move/extend proof")
    $staleChecklistText | Set-Content -LiteralPath $staleImportChecklist -Encoding UTF8
    & scripts\update_partition_manager_external_manifest_from_reports.ps1 `
        -ManifestPath $staleImportManifest `
        -EvidenceRoot $scaffoldEvidenceRoot `
        -OutputPath $staleImportedManifest `
        -ChecklistPath $staleImportChecklist `
        -OutputChecklistPath $staleImportedChecklist `
        -RequireAllReports `
        -Force `
        -Quiet
    $normalizedImport = Get-Content -LiteralPath $staleImportedManifest -Raw | ConvertFrom-Json
    $normalizedResult = @($normalizedImport.results | Where-Object { $_.id -eq $staleGate.id })[0]
    Assert-Condition -Condition ($normalizedResult.name -eq $staleGate.name) -Message "external report import did not normalize stale result name"
    $staleImportedManifestRelative = ConvertTo-ProjectRelativePath -Path $staleImportedManifest
    & scripts\check_partition_manager_external_checklist.ps1 -ChecklistPath $staleImportedChecklist -ManifestPath $staleImportedManifestRelative
    $normalizedChecklistText = Get-Content -LiteralPath $staleImportedChecklist -Raw
    Assert-Condition -Condition ($normalizedChecklistText.Contains("## $($staleGate.id) - $($staleGate.name)")) -Message "external report import did not normalize stale checklist heading"

    $badImportManifest = Join-Path $Script:RunRoot "bad-external-report-import.json"
    $badImportChecklist = Join-Path $Script:RunRoot "bad-external-report-import.checklist.md"
    $badImportRoot = Join-Path $Script:RunRoot "bad-external-report-import"
    & scripts\new_partition_manager_external_evidence_manifest.ps1 `
        -OutputPath $badImportManifest `
        -ChecklistPath $badImportChecklist `
        -EvidenceRoot $badImportRoot `
        -CreateEvidenceDirectories `
        -Force
    $badReportGateId = $ExpectedExternalGateIds[0]
    $badReportTemplate = Join-Path $badImportRoot "$badReportGateId\report.template.json"
    $badReportPath = Join-Path $badImportRoot "$badReportGateId\report.json"
    $badReport = Get-Content -LiteralPath $badReportTemplate -Raw | ConvertFrom-Json
    Set-ObjectProperty -Object $badReport -Name "status" -Value "Passed"
    Set-ObjectProperty -Object $badReport -Name "evidence" -Value (New-SyntheticEvidence -Id $badReportGateId)
    Set-ObjectProperty -Object $badReport -Name "artifacts" -Value @($evidenceFile)
    Set-ObjectProperty -Object $badReport -Name "verification_summary" -Value "synthetic bad report"
    $badReportFirstKey = @((Get-ScenarioSpec -Id $badReportGateId).required_evidence_keys)[0]
    $badReport.evidence.PSObject.Properties[$badReportFirstKey].Value = ""
    $badReport | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $badReportPath -Encoding UTF8
    Assert-ToolFails -Name "bad external report import" -Body {
        & scripts\update_partition_manager_external_manifest_from_reports.ps1 `
            -ManifestPath $badImportManifest `
            -EvidenceRoot $badImportRoot `
            -OutputPath (Join-Path $Script:RunRoot "bad-external-report-import-output.json") `
            -Force `
            -Quiet
    }

    $badUrlManifest = Join-Path $Script:RunRoot "external-bad-url.json"
    Write-ExternalManifest -Path $badUrlManifest -Status "Passed" -EvidenceUrl "not-a-url"

    $badRequiredValueManifest = Join-Path $Script:RunRoot "external-bad-required-value.json"
    Copy-Item -LiteralPath $passedManifest -Destination $badRequiredValueManifest -Force
    $badRequiredValue = Get-Content -LiteralPath $badRequiredValueManifest -Raw | ConvertFrom-Json
    $valueConstrainedGateId = @($ExpectedExternalGateIds | Where-Object {
            $null -ne (Get-ScenarioSpec -Id $_).PSObject.Properties["required_evidence_values"]
        } | Select-Object -First 1)[0]
    $valueConstrainedResult = @($badRequiredValue.results | Where-Object { $_.id -eq $valueConstrainedGateId })[0]
    $valueRequirementKey = @((Get-ScenarioSpec -Id $valueConstrainedGateId).required_evidence_values.PSObject.Properties)[0].Name
    $valueConstrainedResult.evidence.PSObject.Properties[$valueRequirementKey].Value = "wrong-mode"
    $badRequiredValue | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $badRequiredValueManifest -Encoding UTF8

    $missingExternalEvidenceManifest = Join-Path $Script:RunRoot "external-missing-evidence.json"
    Write-ExternalManifest -Path $missingExternalEvidenceManifest -Status "Passed" -EvidencePath $evidenceFile
    $badExternalEvidence = Get-Content -LiteralPath $missingExternalEvidenceManifest -Raw | ConvertFrom-Json
    $badExternalEvidence.results[0].evidence = $null
    $badExternalEvidence | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $missingExternalEvidenceManifest -Encoding UTF8

    $codeCompleteReport = Join-Path $Script:RunRoot "code-complete-report.json"
    Write-CertificationReport -Path $codeCompleteReport -ReportStatus "NotRun" -VhdStatuses (New-VhdStatusMap -Status "Skipped")
    Assert-ClaimLevel -ReportPath $codeCompleteReport -ManifestPath $pendingManifest -ExpectedClaimLevel "CodeCompleteOnly" -Name "code-complete"
    $codeCompleteGapStatus = Write-ClaimStatus -ReportPath $codeCompleteReport -ManifestPath $pendingManifest -Name "code-complete-gap"
    $codeCompleteGapJson = Join-Path $Script:RunRoot "code-complete-gap-report.json"
    $codeCompleteGapMarkdown = Join-Path $Script:RunRoot "code-complete-gap-report.md"
    & scripts\new_partition_manager_certification_gap_report.ps1 `
        -StatusPath $codeCompleteGapStatus `
        -OutputPath $codeCompleteGapJson `
        -MarkdownPath $codeCompleteGapMarkdown `
        -Force `
        -Quiet
    & scripts\check_partition_manager_certification_gap_report.ps1 `
        -StatusPath $codeCompleteGapStatus `
        -GapReportPath $codeCompleteGapJson `
        -MarkdownPath $codeCompleteGapMarkdown
    $codeCompleteBundleJson = Join-Path $Script:RunRoot "code-complete-certification-bundle.json"
    & scripts\new_partition_manager_certification_bundle.ps1 `
        -CertificationRoot $Script:RunRoot `
        -StatusPath $codeCompleteGapStatus `
        -GapReportPath $codeCompleteGapJson `
        -GapMarkdownPath $codeCompleteGapMarkdown `
        -VhdPreflightPath $vhdPreflightPath `
        -ExternalEvidenceManifest $pendingManifest `
        -ExternalEvidenceChecklist $scaffoldChecklist `
        -OutputPath $codeCompleteBundleJson `
        -Force `
        -Quiet
    & scripts\check_partition_manager_certification_bundle.ps1 -BundlePath $codeCompleteBundleJson
    $codeCompleteGap = Get-Content -LiteralPath $codeCompleteGapJson -Raw | ConvertFrom-Json
    Assert-Condition -Condition (@($codeCompleteGap.gaps).Count -eq ($ExpectedVhdScenarioIds.Count + $ExpectedExternalGateIds.Count)) -Message "code-complete gap count mismatch"
    Assert-Condition -Condition ((@($codeCompleteGap.gaps | ForEach-Object { $_.id }) -contains "vhd.image-restore") -and (@($codeCompleteGap.gaps | ForEach-Object { $_.id }) -contains "external.hardware-wipe")) -Message "code-complete gap report missing expected terminal gates"
    $codeCompleteGapCommands = (@($codeCompleteGap.next_commands) -join "`n")
    Assert-Condition -Condition (-not $codeCompleteGapCommands.Contains("-ExecutionPolicy Bypass")) -Message "code-complete gap report recommends a blanket execution-policy bypass"
    Assert-Condition -Condition ($codeCompleteGapCommands.Contains("run_partition_manager_vhd_certification_strict.ps1")) -Message "code-complete gap report missing strict VHD next command"
    Assert-Condition -Condition ($codeCompleteGapCommands.Contains("update_partition_manager_external_manifest_from_reports.ps1")) -Message "code-complete gap report missing external report import next command"
    Assert-Condition -Condition ($codeCompleteGapCommands.Contains("run_partition_manager_hardware_certification_strict.ps1")) -Message "code-complete gap report missing strict hardware next command"
    Assert-Condition -Condition (-not $codeCompleteGapCommands.Contains("run_partition_manager_destructive_certification.ps1")) -Message "code-complete gap report contains stale direct destructive harness command"

    $vhdReport = Join-Path $Script:RunRoot "vhd-report.json"
    Write-CertificationReport -Path $vhdReport -ReportStatus "Partial" -VhdStatuses (New-VhdStatusMap -Status "Passed")
    Assert-ClaimLevel -ReportPath $vhdReport -ManifestPath $pendingManifest -ExpectedClaimLevel "VhdDataDiskCertified" -Name "vhd"
    $vhdGapStatus = Write-ClaimStatus -ReportPath $vhdReport -ManifestPath $pendingManifest -Name "vhd-gap"
    $vhdGapJson = Join-Path $Script:RunRoot "vhd-gap-report.json"
    $vhdGapMarkdown = Join-Path $Script:RunRoot "vhd-gap-report.md"
    & scripts\new_partition_manager_certification_gap_report.ps1 `
        -StatusPath $vhdGapStatus `
        -OutputPath $vhdGapJson `
        -MarkdownPath $vhdGapMarkdown `
        -Force `
        -Quiet
    & scripts\check_partition_manager_certification_gap_report.ps1 `
        -StatusPath $vhdGapStatus `
        -GapReportPath $vhdGapJson `
        -MarkdownPath $vhdGapMarkdown
    $vhdGap = Get-Content -LiteralPath $vhdGapJson -Raw | ConvertFrom-Json
    $vhdGapCommands = (@($vhdGap.next_commands) -join "`n")
    Assert-Condition -Condition (@($vhdGap.gaps).Count -eq $ExpectedExternalGateIds.Count) -Message "vhd-certified gap count mismatch"
    Assert-Condition -Condition (-not $vhdGapCommands.Contains("run_partition_manager_vhd_certification_strict.ps1")) -Message "vhd-certified gap report should not ask to rerun VHD"
    Assert-Condition -Condition ($vhdGapCommands.Contains("new_partition_manager_external_evidence_manifest.ps1")) -Message "vhd-certified gap report missing external scaffold command"
    Assert-Condition -Condition ($vhdGapCommands.Contains("update_partition_manager_external_manifest_from_reports.ps1")) -Message "vhd-certified gap report missing external report import command"
    Assert-Condition -Condition ($vhdGapCommands.Contains("run_partition_manager_hardware_certification_strict.ps1")) -Message "vhd-certified gap report missing strict hardware next command"

    $legacyExternalPlaceholderReport = Join-Path $Script:RunRoot "legacy-external-placeholder-report.json"
    Copy-Item -LiteralPath $vhdReport -Destination $legacyExternalPlaceholderReport -Force
    $legacyReport = Get-Content -LiteralPath $legacyExternalPlaceholderReport -Raw | ConvertFrom-Json
    $legacyBootResult = @($legacyReport.results | Where-Object { $_.id -eq "external.boot-repair-uefi" })[0]
    $legacyBootResult.id = "external.boot-repair"
    $legacyBootResult.name = "UEFI and BIOS boot repair with intentionally broken BCD"
    $legacyBootResult.required_evidence_values = $null
    $legacyBootResult.safety_contract = @("disposable_boot_vm_only", "pre_repair_snapshot", "commands_logged", "post_repair_boot_verified")
    $legacyReport.results = @($legacyReport.results | Where-Object { $_.id -ne "external.boot-repair-bios" })
    $legacyReport.summary.external_gates = [int]$legacyReport.summary.external_gates - 1
    $legacyReport | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $legacyExternalPlaceholderReport -Encoding UTF8

    $hardwareReport = Join-Path $Script:RunRoot "hardware-report.json"
    Write-CertificationReport -Path $hardwareReport -ReportStatus "Passed" -VhdStatuses (New-VhdStatusMap -Status "Passed")
    Assert-ClaimLevel -ReportPath $hardwareReport -ManifestPath $passedManifest -ExpectedClaimLevel "HardwareCertified" -Name "hardware"
    Assert-ClaimLevel -ReportPath $hardwareReport -ManifestPath $importedFromReportsManifest -ExpectedClaimLevel "HardwareCertified" -Name "hardware-imported-reports"
    Invoke-SyntheticCertificationVerifier `
        -ReportPath $hardwareReport `
        -ExternalEvidenceManifest $importedFromReportsManifest `
        -RequireVhdDataDiskEvidence `
        -RequireExternalGateEvidence
    $hardwareStrictScaffold = Join-Path $Script:RunRoot "hardware-strict-external-evidence.json"
    $hardwareStrictChecklist = Join-Path $Script:RunRoot "hardware-strict-external-evidence.checklist.md"
    $hardwareStrictEvidenceRoot = Join-Path $Script:RunRoot "hardware-strict-external-evidence"
    & scripts\new_partition_manager_external_evidence_manifest.ps1 `
        -OutputPath $hardwareStrictScaffold `
        -ChecklistPath $hardwareStrictChecklist `
        -EvidenceRoot $hardwareStrictEvidenceRoot `
        -CreateEvidenceDirectories `
        -Force
    $hardwareStrictManifest = Get-Content -LiteralPath $hardwareStrictScaffold -Raw | ConvertFrom-Json
    foreach ($result in @($hardwareStrictManifest.results)) {
        $result.status = "Passed"
        $result.evidence = New-SyntheticEvidence -Id $result.id
        $result.evidence_path = $evidenceFile
        $result.notes = "synthetic strict hardware certification self-test"
    }
    $hardwareStrictManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $hardwareStrictScaffold -Encoding UTF8
    & scripts\run_partition_manager_hardware_certification_strict.ps1 `
        @ControllerEvidenceArguments `
        -CertificationRoot $Script:RunRoot `
        -ReportPath $hardwareReport `
        -ExternalEvidenceManifest $hardwareStrictScaffold `
        -ExternalEvidenceChecklist $hardwareStrictChecklist `
        -ExternalEvidenceRoot $hardwareStrictEvidenceRoot `
        -OutputRoot (Join-Path $Script:RunRoot "hardware-strict-output")
    Assert-ToolFails -Name "strict hardware pending evidence verifier" -Body {
        & scripts\run_partition_manager_hardware_certification_strict.ps1 `
            @ControllerEvidenceArguments `
            -CertificationRoot $Script:RunRoot `
            -ReportPath $hardwareReport `
            -ExternalEvidenceManifest $pendingManifest `
            -ExternalEvidenceChecklist $scaffoldChecklist `
            -ExternalEvidenceRoot $scaffoldEvidenceRoot `
            -OutputRoot (Join-Path $Script:RunRoot "hardware-strict-pending-output")
    }
    $hardwareGapStatus = Write-ClaimStatus -ReportPath $hardwareReport -ManifestPath $passedManifest -Name "hardware-gap"
    $hardwareGapJson = Join-Path $Script:RunRoot "hardware-gap-report.json"
    $hardwareGapMarkdown = Join-Path $Script:RunRoot "hardware-gap-report.md"
    & scripts\new_partition_manager_certification_gap_report.ps1 `
        -StatusPath $hardwareGapStatus `
        -OutputPath $hardwareGapJson `
        -MarkdownPath $hardwareGapMarkdown `
        -Force `
        -Quiet
    & scripts\check_partition_manager_certification_gap_report.ps1 `
        -StatusPath $hardwareGapStatus `
        -GapReportPath $hardwareGapJson `
        -MarkdownPath $hardwareGapMarkdown
    $hardwareBundleJson = Join-Path $Script:RunRoot "hardware-certification-bundle.json"
    & scripts\new_partition_manager_certification_bundle.ps1 `
        -CertificationRoot $Script:RunRoot `
        -StatusPath $hardwareGapStatus `
        -GapReportPath $hardwareGapJson `
        -GapMarkdownPath $hardwareGapMarkdown `
        -VhdPreflightPath $vhdPreflightPath `
        -ExternalEvidenceManifest $passedManifest `
        -ExternalEvidenceChecklist $scaffoldChecklist `
        -OutputPath $hardwareBundleJson `
        -Force `
        -Quiet
    & scripts\check_partition_manager_certification_bundle.ps1 -BundlePath $hardwareBundleJson
    $hardwareGap = Get-Content -LiteralPath $hardwareGapJson -Raw | ConvertFrom-Json
    Assert-Condition -Condition (@($hardwareGap.gaps).Count -eq 0) -Message "hardware gap report should have no gaps"

    $failedStatus = New-VhdStatusMap -Status "Passed"
    $failedStatus[$ExpectedVhdScenarioIds[0]] = "Failed"
    $failedReport = Join-Path $Script:RunRoot "failed-report.json"
    Write-CertificationReport -Path $failedReport -ReportStatus "Failed" -VhdStatuses $failedStatus
    Assert-ClaimLevel -ReportPath $failedReport -ManifestPath $passedManifest -ExpectedClaimLevel "FailedEvidence" -Name "failed"

    Invoke-SyntheticCertificationVerifier -ReportPath $codeCompleteReport -ExternalEvidenceManifest $pendingManifest
    Invoke-SyntheticCertificationVerifier -ReportPath $legacyExternalPlaceholderReport -RequireVhdDataDiskEvidence
    Invoke-SyntheticCertificationVerifier -ReportPath $hardwareReport -ExternalEvidenceManifest $passedManifest -RequireVhdDataDiskEvidence -RequireExternalGateEvidence

    $extraIdReport = Join-Path $Script:RunRoot "extra-id-report.json"
    Copy-Item -LiteralPath $codeCompleteReport -Destination $extraIdReport -Force
    $badReport = Get-Content -LiteralPath $extraIdReport -Raw | ConvertFrom-Json
    $extra = $badReport.results[0].PSObject.Copy()
    $extra.id = "vhd.unexpected-extra"
    $badReport.results += $extra
    $badReport.summary.skipped = [int]$badReport.summary.skipped + 1
    $badReport | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $extraIdReport -Encoding UTF8
    Assert-ToolFails -Name "unexpected ID verifier" -Body {
        Invoke-SyntheticCertificationVerifier -ReportPath $extraIdReport
    }

    $missingEvidenceReport = Join-Path $Script:RunRoot "missing-evidence-report.json"
    Copy-Item -LiteralPath $hardwareReport -Destination $missingEvidenceReport -Force
    $badEvidence = Get-Content -LiteralPath $missingEvidenceReport -Raw | ConvertFrom-Json
    $badEvidence.results[0].evidence = $null
    $badEvidence | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $missingEvidenceReport -Encoding UTF8
    Assert-ToolFails -Name "missing evidence verifier" -Body {
        Invoke-SyntheticCertificationVerifier -ReportPath $missingEvidenceReport -RequireVhdDataDiskEvidence
    }

    $blankEvidenceReport = Join-Path $Script:RunRoot "blank-evidence-report.json"
    Copy-Item -LiteralPath $hardwareReport -Destination $blankEvidenceReport -Force
    $blankEvidence = Get-Content -LiteralPath $blankEvidenceReport -Raw | ConvertFrom-Json
    Set-FirstRequiredEvidenceValue -Result $blankEvidence.results[0] -Value ""
    $blankEvidence | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $blankEvidenceReport -Encoding UTF8
    Assert-ClaimLevel -ReportPath $blankEvidenceReport -ManifestPath $pendingManifest -ExpectedClaimLevel "CodeCompleteOnly" -Name "blank-vhd-evidence"
    Assert-ToolFails -Name "blank evidence verifier" -Body {
        Invoke-SyntheticCertificationVerifier -ReportPath $blankEvidenceReport -RequireVhdDataDiskEvidence
    }

    Assert-ToolFails -Name "bad URL verifier" -Body {
        Invoke-SyntheticCertificationVerifier -ReportPath $hardwareReport -ExternalEvidenceManifest $badUrlManifest -RequireExternalGateEvidence
    }

    Assert-ClaimLevel -ReportPath $hardwareReport -ManifestPath $badRequiredValueManifest -ExpectedClaimLevel "VhdDataDiskCertified" -Name "bad-required-external-value"
    Assert-ToolFails -Name "bad required evidence value verifier" -Body {
        Invoke-SyntheticCertificationVerifier -ReportPath $hardwareReport -ExternalEvidenceManifest $badRequiredValueManifest -RequireExternalGateEvidence
    }

    Assert-ToolFails -Name "missing external evidence verifier" -Body {
        Invoke-SyntheticCertificationVerifier -ReportPath $hardwareReport -ExternalEvidenceManifest $missingExternalEvidenceManifest -RequireExternalGateEvidence
    }

    $blankExternalEvidenceManifest = Join-Path $Script:RunRoot "external-blank-evidence.json"
    Copy-Item -LiteralPath $passedManifest -Destination $blankExternalEvidenceManifest -Force
    $blankExternalEvidence = Get-Content -LiteralPath $blankExternalEvidenceManifest -Raw | ConvertFrom-Json
    Set-FirstRequiredEvidenceValue -Result $blankExternalEvidence.results[0] -Value ""
    $blankExternalEvidence | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $blankExternalEvidenceManifest -Encoding UTF8
    Assert-ClaimLevel -ReportPath $hardwareReport -ManifestPath $blankExternalEvidenceManifest -ExpectedClaimLevel "VhdDataDiskCertified" -Name "blank-external-evidence"
    Assert-ToolFails -Name "blank external evidence verifier" -Body {
        Invoke-SyntheticCertificationVerifier -ReportPath $hardwareReport -ExternalEvidenceManifest $blankExternalEvidenceManifest -RequireExternalGateEvidence
    }

    Write-Host "Partition Manager certification tool self-test passed: $Script:RunRoot"
    $Script:RunPassed = $true
}
finally {
    Pop-Location
    if ($Script:RunPassed) {
        Remove-TestRunFolder -RunFolder $Script:RunRoot -OutputRoot $resolvedOutputRoot -NamePattern $runFolderPattern
        Write-Host "Removed the passing run folder $Script:RunRoot"
    }
}

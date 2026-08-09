<#
.SYNOPSIS
    Runs aggregate release-readiness checks for S.A.K. Utility.
#>

param(
    [string]$PackageRoot = "",
    [switch]$RequireSignedPackage,
    [string]$PartitionCertificationRoot = "artifacts\partition-manager-certification\readiness",
    [string]$PartitionExternalEvidenceManifest = "",
    [string]$PartitionExternalEvidenceChecklist = "",
    [switch]$RequirePartitionVhdEvidence,
    [switch]$RequirePartitionExternalEvidence
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)

if ($RequireSignedPackage -and [string]::IsNullOrWhiteSpace($PackageRoot)) {
    throw "-RequireSignedPackage requires -PackageRoot: there is no package to verify signatures for."
}

function Assert-ContainedProjectPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Label
    )

    # The certification root drives -Force writes. Resolve it and refuse anything that leaves
    # the repository (absolute path, traversal) or is reached through a junction/symlink, so
    # generated evidence cannot be redirected outside repository artifact storage.
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "$Label must not be empty"
    }
    $root = [System.IO.Path]::GetFullPath($ProjectRoot).TrimEnd('\')
    $candidate = if ([System.IO.Path]::IsPathRooted($Path)) { $Path } else { Join-Path $root $Path }
    $full = [System.IO.Path]::GetFullPath($candidate).TrimEnd('\')
    if ($full.Length -le $root.Length -or
        -not $full.StartsWith($root + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label must stay inside the repository: $Path"
    }
    if (Test-Path -LiteralPath $full) {
        $item = Get-Item -LiteralPath $full -Force
        $reparse = [System.IO.FileAttributes]::ReparsePoint
        if (($item.Attributes -band $reparse) -eq $reparse) {
            throw "$Label resolves through a reparse point: $full"
        }
    }
}

Assert-ContainedProjectPath -Path $PartitionCertificationRoot -Label "-PartitionCertificationRoot"

function Invoke-IsolatedPowerShellScript {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ScriptPath
    )

    $hostPath = (Get-Process -Id $PID).Path
    & $hostPath -NoProfile -ExecutionPolicy Bypass -File $ScriptPath
    if ($LASTEXITCODE -ne 0) {
        throw "Release readiness check failed: $ScriptPath"
    }
}

function Invoke-ReleaseReadinessScript {
    param(
        [Parameter(Mandatory = $true)]
        [scriptblock]$Body,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $global:LASTEXITCODE = 0
    & $Body
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "Release readiness check failed: $Name"
    }
}

Push-Location $ProjectRoot
try {
    $version = (Get-Content -LiteralPath "VERSION" -Raw).Trim()
    $cmake = Get-Content -LiteralPath "CMakeLists.txt" -Raw
    if ($cmake -notmatch "project\(SAK_Utility\s+VERSION\s+$([regex]::Escape($version))\b") {
        throw "VERSION ($version) does not match CMake project version"
    }

    foreach ($requiredScript in @(
            "scripts/scan_secrets.ps1",
            "scripts/check_blocking_patterns.ps1",
            "scripts/check_gui_style_tokens.ps1",
            "scripts/check_gui_stylesheet_literals.ps1",
            "scripts/check_gui_magic_numbers.ps1",
            "scripts/check_magic_numbers.py",
            "scripts/check_accessibility_patterns.ps1",
            "scripts/check_partition_manager_certification_matrix_integrity.ps1",
            "scripts/check_partition_manager_commercial_gate_matrix.ps1",
            "scripts/check_partition_manager_external_checklist.ps1",
            "scripts/check_partition_manager_external_lab_package.ps1",
            "scripts/check_partition_manager_certification_gap_report.ps1",
            "scripts/check_partition_manager_certification_bundle.ps1",
            "scripts/check_partition_manager_vhd_preflight_report.ps1",
            "scripts/check_partition_manager_feature_matrix.ps1",
            "scripts/check_partition_filesystem_tool_manifest.ps1",
            "scripts/test_partition_filesystem_probe_certifier.ps1",
            "scripts/run_partition_manager_hfsprogs_image_validation.ps1",
            "scripts/launch_partition_manager_physical_hfsprogs_validation_local.ps1",
            "scripts/run_partition_manager_physical_hfsprogs_validation.ps1",
            "scripts/launch_partition_manager_physical_hfs_file_apply_validation_local.ps1",
            "scripts/run_partition_manager_physical_hfs_file_apply_validation.ps1",
            "scripts/check_partition_manager_release_claims.ps1",
            "scripts/check_powershell_syntax.ps1",
            "scripts/check_logged_message_boxes.ps1",
            "scripts/run_lizard.py",
            "scripts/check_third_party_licenses.ps1",
            "scripts/get_partition_manager_certification_status.ps1",
            "scripts/new_partition_manager_certification_bundle.ps1",
            "scripts/new_partition_manager_certification_gap_report.ps1",
            "scripts/new_partition_manager_external_evidence_manifest.ps1",
            "scripts/run_partition_manager_allocate_free_space_external_gate.ps1",
            "scripts/run_partition_manager_bitlocker_mutation_external_gate.ps1",
            "scripts/run_partition_manager_cluster_size_external_gate.ps1",
            "scripts/run_partition_manager_destructive_certification.ps1",
            "scripts/launch_partition_manager_ext_filesystem_vm_gate_local.ps1",
            "scripts/run_partition_manager_ext_filesystem_vm_gate.ps1",
            "scripts/run_partition_manager_ext_linux_validation.ps1",
            "scripts/run_partition_manager_file_recovery_external_gate.ps1",
            "scripts/run_partition_manager_hdd_defrag_external_gate.ps1",
            "scripts/launch_partition_manager_hdd_defrag_external_gate_local.ps1",
            "scripts/run_partition_manager_linux_metadata_validation.ps1",
            "scripts/run_partition_manager_linux_swap_format_validation.ps1",
            "scripts/launch_partition_manager_linux_swap_vm_gate_local.ps1",
            "scripts/run_partition_manager_linux_swap_vm_gate.ps1",
            "scripts/run_partition_manager_physical_apple_probe_validation.ps1",
            "scripts/launch_partition_manager_physical_cross_filesystem_destructive_validation_local.ps1",
            "scripts/run_partition_manager_physical_cross_filesystem_destructive_validation.ps1",
            "scripts/run_partition_manager_hardware_certification_strict.ps1",
            "scripts/run_partition_manager_vhd_certification_strict.ps1",
            "scripts/test_partition_manager_certification_tools.ps1",
            "scripts/test_partition_manager_vhd_preflight.ps1",
            "scripts/update_partition_manager_external_manifest_from_reports.ps1",
            "scripts/verify_partition_manager_certification.ps1",
            "scripts/verify_portable_release_smoke.ps1",
            "scripts/run_portable_e2e_smoke.ps1",
            "scripts/stage_portable_release.ps1",
            "scripts/create_signing_catalog.ps1",
            "scripts/verify_authenticode_signatures.ps1",
            "scripts/create_release_archive.ps1")) {
        if (-not (Test-Path -LiteralPath $requiredScript -PathType Leaf)) {
            throw "Required release script missing: $requiredScript"
        }
    }

    Invoke-ReleaseReadinessScript -Name "scripts/scan_secrets.ps1" -Body {
        # No -SkipExternalTools on the release gate: gitleaks/trufflehog run whenever they are
        # installed, so readiness cannot pass on regex coverage alone.
        & scripts/scan_secrets.ps1
    }
    & scripts/check_blocking_patterns.ps1
    $syntaxFiles = @(git ls-files "*.ps1")
    if ($LASTEXITCODE -ne 0) {
        throw "Release readiness check failed: git ls-files could not enumerate PowerShell files (exit $LASTEXITCODE)"
    }
    if ($syntaxFiles.Count -eq 0) {
        throw "Release readiness check failed: git ls-files returned no PowerShell files"
    }
    $syntaxFiles += "scripts/get_partition_manager_certification_status.ps1"
    $syntaxFiles += "scripts/new_partition_manager_external_evidence_manifest.ps1"
    $syntaxFiles += "scripts/check_partition_manager_certification_matrix_integrity.ps1"
    $syntaxFiles += "scripts/check_partition_manager_commercial_gate_matrix.ps1"
    $syntaxFiles += "scripts/check_partition_manager_external_checklist.ps1"
    $syntaxFiles += "scripts/check_partition_manager_external_lab_package.ps1"
    $syntaxFiles += "scripts/check_partition_manager_certification_gap_report.ps1"
    $syntaxFiles += "scripts/check_partition_manager_certification_bundle.ps1"
    $syntaxFiles += "scripts/check_partition_manager_vhd_preflight_report.ps1"
    $syntaxFiles += "scripts/check_partition_manager_feature_matrix.ps1"
    $syntaxFiles += "scripts/check_partition_filesystem_tool_manifest.ps1"
    $syntaxFiles += "scripts/test_partition_filesystem_probe_certifier.ps1"
    $syntaxFiles += "scripts/run_partition_manager_hfsprogs_image_validation.ps1"
    $syntaxFiles += "scripts/launch_partition_manager_physical_hfsprogs_validation_local.ps1"
    $syntaxFiles += "scripts/run_partition_manager_physical_hfsprogs_validation.ps1"
    $syntaxFiles += "scripts/launch_partition_manager_physical_hfs_file_apply_validation_local.ps1"
    $syntaxFiles += "scripts/run_partition_manager_physical_hfs_file_apply_validation.ps1"
    $syntaxFiles += "scripts/check_partition_manager_release_claims.ps1"
    $syntaxFiles += "scripts/run_partition_manager_allocate_free_space_external_gate.ps1"
    $syntaxFiles += "scripts/run_partition_manager_bitlocker_mutation_external_gate.ps1"
    $syntaxFiles += "scripts/run_partition_manager_cluster_size_external_gate.ps1"
    $syntaxFiles += "scripts/run_partition_manager_destructive_certification.ps1"
    $syntaxFiles += "scripts/launch_partition_manager_ext_filesystem_vm_gate_local.ps1"
    $syntaxFiles += "scripts/run_partition_manager_ext_filesystem_vm_gate.ps1"
    $syntaxFiles += "scripts/run_partition_manager_ext_linux_validation.ps1"
    $syntaxFiles += "scripts/run_partition_manager_file_recovery_external_gate.ps1"
    $syntaxFiles += "scripts/run_partition_manager_hdd_defrag_external_gate.ps1"
    $syntaxFiles += "scripts/launch_partition_manager_hdd_defrag_external_gate_local.ps1"
    $syntaxFiles += "scripts/run_partition_manager_linux_metadata_validation.ps1"
    $syntaxFiles += "scripts/run_partition_manager_linux_swap_format_validation.ps1"
    $syntaxFiles += "scripts/launch_partition_manager_linux_swap_vm_gate_local.ps1"
    $syntaxFiles += "scripts/run_partition_manager_linux_swap_vm_gate.ps1"
    $syntaxFiles += "scripts/run_partition_manager_physical_apple_probe_validation.ps1"
    $syntaxFiles += "scripts/launch_partition_manager_physical_cross_filesystem_destructive_validation_local.ps1"
    $syntaxFiles += "scripts/run_partition_manager_physical_cross_filesystem_destructive_validation.ps1"
    $syntaxFiles += "scripts/run_partition_manager_hardware_certification_strict.ps1"
    $syntaxFiles += "scripts/run_partition_manager_vhd_certification_strict.ps1"
    $syntaxFiles += "scripts/test_partition_manager_certification_tools.ps1"
    $syntaxFiles += "scripts/test_partition_manager_vhd_preflight.ps1"
    $syntaxFiles += "scripts/update_partition_manager_external_manifest_from_reports.ps1"
    $syntaxFiles += "scripts/new_partition_manager_certification_bundle.ps1"
    $syntaxFiles += "scripts/new_partition_manager_certification_gap_report.ps1"
    $syntaxFiles += "scripts/verify_partition_manager_certification.ps1"
    $syntaxFiles = @($syntaxFiles | Sort-Object -Unique)
    Invoke-ReleaseReadinessScript -Name "scripts/check_powershell_syntax.ps1" -Body {
        & scripts/check_powershell_syntax.ps1 -Files $syntaxFiles
    }
    Invoke-IsolatedPowerShellScript -ScriptPath (Join-Path $ProjectRoot "scripts/check_accessibility_patterns.ps1")
    & scripts/check_gui_style_tokens.ps1
    & scripts/check_gui_stylesheet_literals.ps1
    & scripts/check_gui_magic_numbers.ps1
    python scripts/check_magic_numbers.py
    if ($LASTEXITCODE -ne 0) {
        throw "Release readiness check failed: scripts/check_magic_numbers.py"
    }
    & scripts/check_logged_message_boxes.ps1
    python scripts/run_lizard.py
    if ($LASTEXITCODE -ne 0) {
        throw "Release readiness check failed: scripts/run_lizard.py"
    }
    & scripts/check_third_party_licenses.ps1
    & scripts/check_partition_filesystem_tool_manifest.ps1
    Invoke-ReleaseReadinessScript -Name "scripts/test_partition_filesystem_probe_certifier.ps1" -Body {
        & scripts/test_partition_filesystem_probe_certifier.ps1
    }
    Invoke-ReleaseReadinessScript -Name "scripts/run_partition_manager_hfsprogs_image_validation.ps1" -Body {
        & scripts/run_partition_manager_hfsprogs_image_validation.ps1
    }
    & scripts/check_qrc_resources.ps1
    Invoke-ReleaseReadinessScript -Name "scripts/check_partition_manager_certification_matrix_integrity.ps1" -Body {
        & scripts/check_partition_manager_certification_matrix_integrity.ps1
    }
    Invoke-ReleaseReadinessScript -Name "scripts/check_partition_manager_commercial_gate_matrix.ps1" -Body {
        & scripts/check_partition_manager_commercial_gate_matrix.ps1
    }
    Invoke-ReleaseReadinessScript -Name "scripts/check_partition_manager_feature_matrix.ps1" -Body {
        & scripts/check_partition_manager_feature_matrix.ps1
    }
    $requirePartitionVhdEvidenceStrict =
        [bool]$RequirePartitionVhdEvidence -or [bool]$RequirePartitionExternalEvidence
    $requirePartitionExternalEvidenceStrict = [bool]$RequirePartitionExternalEvidence
    $partitionCertificationRoot = $PartitionCertificationRoot
    $partitionExternalEvidenceManifest = $PartitionExternalEvidenceManifest
    $partitionExternalEvidenceChecklist = $PartitionExternalEvidenceChecklist
    # Auto-detection only fills in evidence the caller supplied none of. An explicitly passed
    # checklist (or manifest, or non-default root) is never replaced by auto-detected evidence.
    if ($PartitionCertificationRoot -eq "artifacts\partition-manager-certification\readiness" -and
        [string]::IsNullOrWhiteSpace($PartitionExternalEvidenceManifest) -and
        [string]::IsNullOrWhiteSpace($PartitionExternalEvidenceChecklist)) {
        $strictRoot = "artifacts\partition-manager-certification\vhd-strict"
        $strictManifest = "artifacts\partition-manager-certification\vm-lab\external-evidence.imported.json"
        $strictChecklist = "artifacts\partition-manager-certification\vm-lab\external-evidence.imported.checklist.md"
        if ((Test-Path -LiteralPath (Join-Path $ProjectRoot $strictRoot) -PathType Container) -and
            (Test-Path -LiteralPath (Join-Path $ProjectRoot $strictManifest) -PathType Leaf) -and
            (Test-Path -LiteralPath (Join-Path $ProjectRoot $strictChecklist) -PathType Leaf)) {
            $partitionCertificationRoot = $strictRoot
            $partitionExternalEvidenceManifest = $strictManifest
            $partitionExternalEvidenceChecklist = $strictChecklist
            $requirePartitionVhdEvidenceStrict = $true
            $requirePartitionExternalEvidenceStrict = $true
            Write-Host "Using auto-detected strict Partition Manager evidence under $partitionCertificationRoot"
        }
    }
    $partitionExternalEvidencePath = if ([string]::IsNullOrWhiteSpace($partitionExternalEvidenceManifest)) {
        Join-Path $partitionCertificationRoot "external-evidence.template.json"
    }
    else {
        $partitionExternalEvidenceManifest
    }
    $partitionExternalChecklistPath = if (-not [string]::IsNullOrWhiteSpace($partitionExternalEvidenceChecklist)) {
        $partitionExternalEvidenceChecklist
    }
    elseif ([string]::IsNullOrWhiteSpace($partitionExternalEvidenceManifest)) {
        Join-Path $partitionCertificationRoot "external-evidence.checklist.md"
    }
    else {
        ""
    }
    $partitionExternalEvidenceRoot = Join-Path $partitionCertificationRoot "external-evidence"
    if ([string]::IsNullOrWhiteSpace($partitionExternalEvidenceManifest)) {
        Invoke-ReleaseReadinessScript -Name "scripts/new_partition_manager_external_evidence_manifest.ps1" -Body {
            & scripts/new_partition_manager_external_evidence_manifest.ps1 `
                -OutputPath $partitionExternalEvidencePath `
                -ChecklistPath $partitionExternalChecklistPath `
                -EvidenceRoot $partitionExternalEvidenceRoot `
                -CreateEvidenceDirectories `
                -Force
        }
    }
    elseif (-not (Test-Path -LiteralPath $partitionExternalEvidencePath -PathType Leaf)) {
        throw "Partition Manager external evidence manifest missing: $partitionExternalEvidencePath"
    }
    if (-not [string]::IsNullOrWhiteSpace($partitionExternalChecklistPath)) {
        if (-not (Test-Path -LiteralPath $partitionExternalChecklistPath -PathType Leaf)) {
            throw "Partition Manager external evidence checklist missing: $partitionExternalChecklistPath"
        }
        Invoke-ReleaseReadinessScript -Name "scripts/check_partition_manager_external_checklist.ps1" -Body {
            & scripts/check_partition_manager_external_checklist.ps1 `
                -ChecklistPath $partitionExternalChecklistPath `
                -ManifestPath $partitionExternalEvidencePath
        }
    }
    elseif ($requirePartitionExternalEvidenceStrict) {
        throw "Partition Manager external evidence checklist required when -RequirePartitionExternalEvidence is used. Pass -PartitionExternalEvidenceChecklist with a matrix-verified lab checklist."
    }
    if ([string]::IsNullOrWhiteSpace($partitionExternalEvidenceManifest)) {
        Invoke-ReleaseReadinessScript -Name "scripts/check_partition_manager_external_lab_package.ps1" -Body {
            & scripts/check_partition_manager_external_lab_package.ps1 `
                -EvidenceRoot $partitionExternalEvidenceRoot `
                -ManifestPath $partitionExternalEvidencePath
        }
    }
    $partitionVhdPreflightPath = Join-Path $partitionCertificationRoot "vhd-preflight.json"
    Invoke-ReleaseReadinessScript -Name "scripts/test_partition_manager_vhd_preflight.ps1" -Body {
        & scripts/test_partition_manager_vhd_preflight.ps1 `
            -OutputRoot $partitionCertificationRoot `
            -OutputPath $partitionVhdPreflightPath `
            -RequireAdministrator `
            -Quiet
    }
    Invoke-ReleaseReadinessScript -Name "scripts/check_partition_manager_vhd_preflight_report.ps1" -Body {
        & scripts/check_partition_manager_vhd_preflight_report.ps1 `
            -ReportPath $partitionVhdPreflightPath
    }
    if (-not $requirePartitionVhdEvidenceStrict) {
        Invoke-ReleaseReadinessScript -Name "scripts/run_partition_manager_destructive_certification.ps1" -Body {
            & scripts/run_partition_manager_destructive_certification.ps1 -OutputRoot $partitionCertificationRoot
        }
    }
    else {
        Write-Host "Using existing Partition Manager certification evidence under $partitionCertificationRoot"
    }
    Invoke-ReleaseReadinessScript -Name "scripts/verify_partition_manager_certification.ps1" -Body {
        $verifyArguments = @{
            CertificationRoot = $partitionCertificationRoot
            ExternalEvidenceManifest = $partitionExternalEvidencePath
        }
        if ($requirePartitionVhdEvidenceStrict) {
            $verifyArguments.RequireVhdDataDiskEvidence = $true
        }
        if ($requirePartitionExternalEvidenceStrict) {
            $verifyArguments.RequireExternalGateEvidence = $true
        }
        & scripts/verify_partition_manager_certification.ps1 @verifyArguments
    }
    Invoke-ReleaseReadinessScript -Name "scripts/get_partition_manager_certification_status.ps1" -Body {
        & scripts/get_partition_manager_certification_status.ps1 `
            -CertificationRoot $partitionCertificationRoot `
            -ExternalEvidenceManifest $partitionExternalEvidencePath `
            -OutputPath (Join-Path $partitionCertificationRoot "certification-status.json") `
            -Quiet
    }
    $partitionGapReportPath = Join-Path $partitionCertificationRoot "certification-gap-report.json"
    $partitionGapMarkdownPath = Join-Path $partitionCertificationRoot "certification-gap-report.md"
    Invoke-ReleaseReadinessScript -Name "scripts/new_partition_manager_certification_gap_report.ps1" -Body {
        & scripts/new_partition_manager_certification_gap_report.ps1 `
            -StatusPath (Join-Path $partitionCertificationRoot "certification-status.json") `
            -OutputPath $partitionGapReportPath `
            -MarkdownPath $partitionGapMarkdownPath `
            -Force `
            -Quiet
    }
    Invoke-ReleaseReadinessScript -Name "scripts/check_partition_manager_certification_gap_report.ps1" -Body {
        & scripts/check_partition_manager_certification_gap_report.ps1 `
            -StatusPath (Join-Path $partitionCertificationRoot "certification-status.json") `
            -GapReportPath $partitionGapReportPath `
            -MarkdownPath $partitionGapMarkdownPath
    }
    $partitionBundlePath = Join-Path $partitionCertificationRoot "certification-artifact-bundle.json"
    Invoke-ReleaseReadinessScript -Name "scripts/new_partition_manager_certification_bundle.ps1" -Body {
        & scripts/new_partition_manager_certification_bundle.ps1 `
            -CertificationRoot $partitionCertificationRoot `
            -StatusPath (Join-Path $partitionCertificationRoot "certification-status.json") `
            -GapReportPath $partitionGapReportPath `
            -GapMarkdownPath $partitionGapMarkdownPath `
            -VhdPreflightPath $partitionVhdPreflightPath `
            -ExternalEvidenceManifest $partitionExternalEvidencePath `
            -ExternalEvidenceChecklist $partitionExternalChecklistPath `
            -OutputPath $partitionBundlePath `
            -Force `
            -Quiet
    }
    Invoke-ReleaseReadinessScript -Name "scripts/check_partition_manager_certification_bundle.ps1" -Body {
        & scripts/check_partition_manager_certification_bundle.ps1 `
            -BundlePath $partitionBundlePath
    }
    Invoke-ReleaseReadinessScript -Name "scripts/check_partition_manager_release_claims.ps1" -Body {
        & scripts/check_partition_manager_release_claims.ps1 `
            -StatusPath (Join-Path $partitionCertificationRoot "certification-status.json")
    }
    Invoke-ReleaseReadinessScript -Name "scripts/test_partition_manager_certification_tools.ps1" -Body {
        & scripts/test_partition_manager_certification_tools.ps1
    }

    if (-not [string]::IsNullOrWhiteSpace($PackageRoot)) {
        $package = Resolve-Path -LiteralPath $PackageRoot
        if ($RequireSignedPackage) {
            # Trust is established BEFORE any package binary runs: the smoke checks launch
            # sak_utility.exe out of this directory, so an unsigned or tampered package must be
            # rejected first rather than after it has already executed.
            Invoke-ReleaseReadinessScript -Name "scripts/verify_authenticode_signatures.ps1" -Body {
                & scripts/verify_authenticode_signatures.ps1 -RootDir $package.Path
            }
        }
        Invoke-ReleaseReadinessScript -Name "scripts/verify_portable_release_smoke.ps1" -Body {
            & scripts/verify_portable_release_smoke.ps1 -PackageRoot $package.Path -RepoRoot $ProjectRoot
        }
        Invoke-ReleaseReadinessScript -Name "scripts/run_portable_e2e_smoke.ps1" -Body {
            & scripts/run_portable_e2e_smoke.ps1 -PackageRoot $package.Path
        }
    }

    Write-Host "Release readiness checks passed."
}
finally {
    Pop-Location
}

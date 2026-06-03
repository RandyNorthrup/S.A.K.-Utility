<#
.SYNOPSIS
    Verifies Partition Manager commercial-parity feature claims map to code, UI, tests, and docs.

.DESCRIPTION
    This is a static release gate. It does not mutate disks. It prevents broad
    AOMEI/MiniTool parity claims from drifting away from concrete implementation
    hooks, regression tests, and documentation.
#>

[CmdletBinding()]
param(
    [string]$ProjectRoot = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent $PSScriptRoot
}

function Get-RepoText {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RelativePath
    )

    $path = Join-Path $ProjectRoot $RelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Feature matrix file missing: $RelativePath"
    }
    return Get-Content -LiteralPath $path -Raw
}

function Assert-Contains {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RelativePath,
        [Parameter(Mandatory = $true)]
        [string[]]$Patterns,
        [Parameter(Mandatory = $true)]
        [string]$FeatureId,
        [Parameter(Mandatory = $true)]
        [string]$EvidenceKind
    )

    $text = Get-RepoText -RelativePath $RelativePath
    foreach ($pattern in $Patterns) {
        if (-not $text.Contains($pattern)) {
            throw "Partition Manager feature '$FeatureId' missing $EvidenceKind evidence '$pattern' in $RelativePath"
        }
    }
}

function Assert-Feature {
    param(
        [Parameter(Mandatory = $true)]
        [hashtable]$Feature
    )

    foreach ($group in $Feature.evidence) {
        Assert-Contains `
            -RelativePath $group.path `
            -Patterns $group.patterns `
            -FeatureId $Feature.id `
            -EvidenceKind $group.kind
    }
}

$features = @(
    @{
        id = "basic-partition-operations"
        evidence = @(
            @{
                kind = "operation type"
                path = "include\sak\partition_manager_types.h"
                patterns = @("Create,", "Delete,", "Format,", "SetDriveLetter,", "SetPartitionLabel,")
            },
            @{
                kind = "UI action"
                path = "src\gui\partition_manager_panel.cpp"
                patterns = @("Create Partition", "Delete Partition", "Format Partition", "Change Drive Letter", "Change Label")
            },
            @{
                kind = "test"
                path = "tests\unit\test_partition_manager_core.cpp"
                patterns = @("scriptBuilder_createRespectsWizardPayload", "scriptBuilder_formatsByPartitionIdentity", "scriptBuilder_setsPartitionLabelByMountedDriveLetter")
            },
            @{
                kind = "documentation"
                path = "docs\PARTITION_MANAGER_PANEL_PLAN.md"
                patterns = @("Basic partition operations", "Create, delete, format, label, assign/remove drive letter")
            }
        )
    },
    @{
        id = "resize-extend-move"
        evidence = @(
            @{
                kind = "operation type"
                path = "include\sak\partition_manager_types.h"
                patterns = @("Resize,", "MovePartition,")
            },
            @{
                kind = "UI action"
                path = "src\gui\partition_manager_panel.cpp"
                patterns = @("Resize/Move Partition", "Extend Partition Wizard", "Move partition start", "Offline move will back up")
            },
            @{
                kind = "test"
                path = "tests\unit\test_partition_manager_core.cpp"
                patterns = @("safetyValidator_allowsResizeIntoAdjacentFreeSpace", "scriptBuilder_buildsOfflineMoveAndMetadataScripts", "safetyValidator_allowsConfirmedOfflineRebuildOperations")
            },
            @{
                kind = "documentation"
                path = "docs\PARTITION_MANAGER_PANEL_PLAN.md"
                patterns = @("Resize operations", "MovePartition")
            }
        )
    },
    @{
        id = "mbr-gpt-conversion"
        evidence = @(
            @{
                kind = "operation type"
                path = "include\sak\partition_manager_types.h"
                patterns = @("ConvertPartitionStyle,")
            },
            @{
                kind = "script builder"
                path = "src\core\partition_script_builder.cpp"
                patterns = @("mbr2gpt.exe /validate", "Initialize-Disk -Number %1 -PartitionStyle %2")
            },
            @{
                kind = "test"
                path = "tests\unit\test_partition_manager_core.cpp"
                patterns = @("scriptBuilder_buildsMbr2GptScript", "safetyValidator_blocksUnsafeSystemStyleConversion")
            },
            @{
                kind = "documentation"
                path = "docs\PARTITION_MANAGER_PANEL_PLAN.md"
                patterns = @("MBR/GPT conversion", "Safe system-disk MBR-to-GPT")
            }
        )
    },
    @{
        id = "merge-split"
        evidence = @(
            @{
                kind = "operation type"
                path = "include\sak\partition_manager_types.h"
                patterns = @("Merge,", "Split,")
            },
            @{
                kind = "script builder"
                path = "src\core\partition_script_builder.cpp"
                patterns = @("robocopy.exe", "Split Disk %1 Partition %2")
            },
            @{
                kind = "test"
                path = "tests\unit\test_partition_manager_core.cpp"
                patterns = @("scriptBuilder_buildsMergeScript", "safetyValidator_blocksResizeBelowUsedBytes")
            },
            @{
                kind = "documentation"
                path = "docs\PARTITION_MANAGER_PANEL_PLAN.md"
                patterns = @("Partition merge and split", "Merge And Split")
            }
        )
    },
    @{
        id = "file-system-conversion"
        evidence = @(
            @{
                kind = "operation type"
                path = "include\sak\partition_manager_types.h"
                patterns = @("ConvertFileSystem,")
            },
            @{
                kind = "script builder"
                path = "src\core\partition_script_builder.cpp"
                patterns = @("convert.exe %1: /FS:NTFS /NoSecurity")
            },
            @{
                kind = "test"
                path = "tests\unit\test_partition_manager_core.cpp"
                patterns = @("safetyValidator_blocksUnsupportedFileSystemConversion")
            },
            @{
                kind = "documentation"
                path = "docs\PARTITION_MANAGER_PANEL_PLAN.md"
                patterns = @("File system conversion", "FAT/FAT32 to NTFS")
            }
        )
    },
    @{
        id = "clone-image-restore"
        evidence = @(
            @{
                kind = "operation type"
                path = "include\sak\partition_manager_types.h"
                patterns = @("CloneDisk,", "ClonePartition,", "CreateImage,", "RestoreImage,")
            },
            @{
                kind = "UI wizard"
                path = "src\gui\partition_manager_panel.cpp"
                patterns = @("Copy Disk Wizard", "Copy Partition Wizard", "Create Disk Image", "Restore Disk Image")
            },
            @{
                kind = "test"
                path = "tests\unit\test_partition_manager_core.cpp"
                patterns = @("scriptBuilder_buildsCloneVerificationScript", "scriptBuilder_buildsOffsetPartitionCloneScript", "safetyValidator_requiresCloneOverwriteConfirmation", "safetyValidator_requiresPartitionRegionCloneConfirmation", "safetyValidator_blocksUnsafePayloadTargetDisk")
            },
            @{
                kind = "documentation"
                path = "docs\PARTITION_MANAGER_PANEL_PLAN.md"
                patterns = @("Disk cloning and imaging", "Disk-to-disk clone, partition clone, image backup, image restore, and verification")
            },
            @{
                kind = "certification matrix"
                path = "docs\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
                patterns = @("vhd.image-clone", "vhd.image-restore", "target_overwritten", "vhd.partition-clone-region", "outside_marker_preserved")
            }
        )
    },
    @{
        id = "os-migration-boot-repair"
        evidence = @(
            @{
                kind = "operation type"
                path = "include\sak\partition_manager_types.h"
                patterns = @("MigrateOs,", "RepairBoot,")
            },
            @{
                kind = "script builder"
                path = "src\core\partition_script_builder.cpp"
                patterns = @("SAK OS migration boot validation", "bcdboot.exe %2 /s %1: /f UEFI", "reagentc.exe /info")
            },
            @{
                kind = "test"
                path = "tests\unit\test_partition_manager_core.cpp"
                patterns = @("scriptBuilder_buildsOsMigrationBootValidationScript")
            },
            @{
                kind = "documentation"
                path = "docs\PARTITION_MANAGER_PANEL_PLAN.md"
                patterns = @("OS migration tools", "Boot repair utilities")
            }
        )
    },
    @{
        id = "smart-ssd-optimization"
        evidence = @(
            @{
                kind = "inventory type"
                path = "include\sak\partition_manager_types.h"
                patterns = @("smart_summary", "temperature_celsius", "wear_percent", "OptimizeSsd,")
            },
            @{
                kind = "script builder"
                path = "src\core\partition_script_builder.cpp"
                patterns = @("fsutil behavior query DisableDeleteNotify", "Optimize-Volume -DriveLetter %1 -ReTrim -Verbose")
            },
            @{
                kind = "test"
                path = "tests\unit\test_partition_manager_core.cpp"
                patterns = @("SmartSummary", "TemperatureCelsius", "WearPercent", "scriptBuilder_buildsAdvancedParityScripts")
            },
            @{
                kind = "documentation"
                path = "docs\PARTITION_MANAGER_PANEL_PLAN.md"
                patterns = @("S.M.A.R.T. monitoring", "SSD optimization")
            }
        )
    },
    @{
        id = "secure-wipe"
        evidence = @(
            @{
                kind = "operation type"
                path = "include\sak\partition_manager_types.h"
                patterns = @("WipePartition,", "WipeDisk,", "WipeFreeSpace,")
            },
            @{
                kind = "script builder"
                path = "src\core\partition_script_builder.cpp"
                patterns = @("cipher.exe /w:%1:\\", "Clear-Disk -Number %1 -RemoveData -RemoveOEM")
            },
            @{
                kind = "test"
                path = "tests\unit\test_partition_manager_core.cpp"
                patterns = @("scriptBuilder_buildsClearLevelDiskWipeScript", "Optimize-Volume -DriveLetter", "safetyValidator_blocksUnsafeParityOperations")
            },
            @{
                kind = "documentation"
                path = "docs\PARTITION_MANAGER_PANEL_PLAN.md"
                patterns = @("Secure data wiping", "SSD Secure Erase queues Windows ReTrim")
            }
        )
    },
    @{
        id = "commercial-utility-actions"
        evidence = @(
            @{
                kind = "UI action"
                path = "src\gui\partition_manager_panel.cpp"
                patterns = @("Quick Partition", "Data Recovery", "raw volume/device path", "FileRecoveryEngine::scanOfflineImage", "Allocate Free Space", "Allocate Free Space To", "Back up adjacent donor", "MovePartition", "Space Analyzer", "Largest Files", "File Types", "Copy Path", "Disk Benchmark", "Manage BitLocker", "BitLocker status table", "Copy BitLocker commands", "Disk Defrag", "Disk optimization status table", "Copy Optimize commands", "SSD Secure Erase", "SSD Secure Erase readiness table", "Queue SSD Secure Erase", "Make Bootable Media")
            },
            @{
                kind = "data recovery engine"
                path = "src\core\file_recovery_engine.cpp"
                patterns = @("scanOfflineImage", "restoreCandidates", "PNG image", "JPEG image", "PDF document", "source_not_mutated")
            },
            @{
                kind = "panel test"
                path = "tests\unit\test_partition_manager_panel.cpp"
                patterns = @("Quick Partition", "Data Recovery", "image and raw path recovery", "allocateFreeSpaceQueuesAdjacentDonorOperation", "unallocatedAllocateFreeSpaceQueuesAdjacentEngines", "formerCommercialCompatibilityActionsQueueDirectEngines", "Allocate free space amount", "Space Analyzer", "Largest Files", "File Types", "Copy Path", "Disk Benchmark", "Manage BitLocker", "manageBitLockerShowsStatusDialog", "Disk Defrag", "diskDefragShowsOptimizeDialog", "SSD Secure Erase", "ssdSecureEraseShowsQueueDialog", "Make Bootable Media")
            },
            @{
                kind = "core test"
                path = "tests\unit\test_partition_manager_core.cpp"
                patterns = @("fileRecoveryEngine_scansAndRestoresOfflineImage", "scanOfflineImage", "restoreCandidates", "source_not_mutated", "scriptBuilder_buildsAllocateFreeSpaceScript", "safetyValidator_blocksUnsafeAllocateFreeSpacePayloads")
            },
            @{
                kind = "documentation"
                path = "docs\PARTITION_MANAGER_PANEL_PLAN.md"
                patterns = @("Commercial utility parity", "AOMEI/MiniTool expose Quick Partition", "Data Recovery", "Allocate Free Space queues adjacent donor", "Allocate Free Space To queues adjacent unallocated space", "image and raw volume/device file carving", "raw volume/device paths", "Largest Files", "File Types", "Copy Path", "BitLocker status", "defrag/ReTrim command guidance", "SSD Secure Erase shows disk identity")
            },
            @{
                kind = "certification matrix"
                path = "docs\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
                patterns = @("external.file-level-data-recovery", "deleted_fixture_name", "restore_to_separate_destination", "external.allocate-free-space", "external.ssd-retrim", "purge_warning_visible")
            }
        )
    },
    @{
        id = "pending-apply-review-context-menu"
        evidence = @(
            @{
                kind = "UI action"
                path = "src\gui\partition_manager_panel.cpp"
                patterns = @("Apply", "Dry Run", "Undo", "Redo", "Discard", "customContextMenuRequested", "addDiskContextMenuActions", "addPartitionContextMenuActions")
            },
            @{
                kind = "panel test"
                path = "tests\unit\test_partition_manager_panel.cpp"
                patterns = @("partitionTableUsesAomeiListChrome", "contextMenuPolicy", "sidebarIsFixedAndHasNoRedundantPreviewBox", "diskMapUsesCompactSpacing", "finalApplyReviewContainsLayoutDiff")
            },
            @{
                kind = "disk map chrome"
                path = "src\gui\partition_manager_panel.cpp"
                patterns = @("DiskMapRowFrame", "partitionActionTextLink", "kDiskMapOuterMargin = 1", "kDiskMapRowSpacing = 2", "kDiskMapSegmentSpacing = 2", "GPT/Primary", "RAID5", 'setProperty("outerColorRole"', 'setProperty("innerColorRole"', 'setProperty("selected"')
            },
            @{
                kind = "documentation"
                path = "docs\PARTITION_MANAGER_PANEL_PLAN.md"
                patterns = @("Pending operations", "right-click menu")
            }
        )
    },
    @{
        id = "certification-gates"
        evidence = @(
            @{
                kind = "readiness script"
                path = "scripts\check_release_readiness.ps1"
                patterns = @("run_partition_manager_destructive_certification.ps1", "run_partition_manager_vhd_certification_strict.ps1", "run_partition_manager_hardware_certification_strict.ps1", "verify_partition_manager_certification.ps1", "check_partition_manager_certification_matrix_integrity.ps1", "check_partition_manager_commercial_gate_matrix.ps1", "check_partition_manager_external_checklist.ps1", "check_partition_manager_external_lab_package.ps1", "new_partition_manager_certification_gap_report.ps1", "check_partition_manager_certification_gap_report.ps1", "certification-gap-report.md", "new_partition_manager_certification_bundle.ps1", "check_partition_manager_certification_bundle.ps1", "certification-artifact-bundle.json", "test_partition_manager_vhd_preflight.ps1", "check_partition_manager_vhd_preflight_report.ps1", "vhd-preflight.json", "CreateEvidenceDirectories", "external-evidence", "PartitionExternalEvidenceChecklist", "external-evidence.checklist.md", "check_partition_manager_release_claims.ps1", "test_partition_manager_certification_tools.ps1")
            },
            @{
                kind = "certification verifier"
                path = "scripts\verify_partition_manager_certification.ps1"
                patterns = @("RequireVhdDataDiskEvidence", "RequireExternalGateEvidence", "PARTITION_MANAGER_CERTIFICATION_MATRIX.json", "Assert-PassedVhdEvidencePayload", "Assert-PassedExternalEvidencePayload", "Assert-EvidenceValuePresent")
            },
            @{
                kind = "certification status"
                path = "scripts\get_partition_manager_certification_status.ps1"
                patterns = @("Test-VhdEvidenceComplete", "Test-ExternalEvidenceComplete", "Test-EvidenceValuePresent", "claim_level")
            },
            @{
                kind = "external evidence scaffold"
                path = "scripts\new_partition_manager_external_evidence_manifest.ps1"
                patterns = @("Write-GateReportTemplate", "report.template.json", "partition-manager-external-evidence-report", "suggested_evidence_path", "required_evidence_values")
            },
            @{
                kind = "external lab package verifier"
                path = "scripts\check_partition_manager_external_lab_package.ps1"
                patterns = @("report.template.json", "partition-manager-external-evidence-report", "Report template evidence keys mismatch", "Report template required evidence values mismatch")
            },
            @{
                kind = "external report importer"
                path = "scripts\update_partition_manager_external_manifest_from_reports.ps1"
                patterns = @("partition-manager-external-evidence-report", "RequireAllReports", "required_evidence_keys", "safety_contract", "required_evidence_values", "evidence_path")
            },
            @{
                kind = "certification gap report"
                path = "scripts\new_partition_manager_certification_gap_report.ps1"
                patterns = @("certification-gap-report", "required_evidence_keys", "safety_contract", "next_commands", "run_partition_manager_vhd_certification_strict.ps1", "run_partition_manager_hardware_certification_strict.ps1")
            },
            @{
                kind = "certification gap verifier"
                path = "scripts\check_partition_manager_certification_gap_report.ps1"
                patterns = @("Gap count mismatch", "required_evidence_keys", "safety_contract", "required evidence values", "Gap Markdown missing")
            },
            @{
                kind = "VHD preflight"
                path = "scripts\test_partition_manager_vhd_preflight.ps1"
                patterns = @("partition-manager-vhd-preflight", "RequireAdministrator", "required_commands", "free_drive_letters", "ready_for_vhd_certification", "run_partition_manager_destructive_certification.ps1")
            },
            @{
                kind = "VHD preflight verifier"
                path = "scripts\check_partition_manager_vhd_preflight_report.ps1"
                patterns = @("partition-manager-vhd-preflight", "VHD scenario count mismatch", "External gate count mismatch", "Non-admin required preflight missing administrator blocker")
            },
            @{
                kind = "strict VHD orchestrator"
                path = "scripts\run_partition_manager_vhd_certification_strict.ps1"
                patterns = @("Invoke-ElevatedRelaunch", "Verb RunAs", "RequireReady", "RunVhdDataDiskMatrix", "RequireVhdDataDiskEvidence", "VhdDataDiskCertified", "certification-artifact-bundle.json")
            },
            @{
                kind = "strict hardware orchestrator"
                path = "scripts\run_partition_manager_hardware_certification_strict.ps1"
                patterns = @("RequireVhdDataDiskEvidence", "RequireExternalGateEvidence", "HardwareCertified", "check_partition_manager_external_checklist.ps1", "check_partition_manager_external_lab_package.ps1", "hardware-certification-status.json", "hardware-certification-artifact-bundle.json")
            },
            @{
                kind = "certification artifact bundle"
                path = "scripts\new_partition_manager_certification_bundle.ps1"
                patterns = @("partition-manager-certification-artifact-bundle", "certification_status", "certification_report", "external_evidence_manifest", "external_evidence_checklist", "Get-FileHash")
            },
            @{
                kind = "certification artifact bundle verifier"
                path = "scripts\check_partition_manager_certification_bundle.ps1"
                patterns = @("partition-manager-certification-artifact-bundle", "Certification bundle hash mismatch", "Certification bundle claim level mismatch", "Certification bundle VHD preflight ready mismatch")
            },
            @{
                kind = "certification matrix"
                path = "docs\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
                patterns = @("required_evidence_keys", "safety_contract", "vhd.create-format-resize-delete", "external.file-level-data-recovery", "external.allocate-free-space", "external.partition-move", "external.cluster-size-change", "external.primary-logical-conversion", "external.volume-serial-number", "external.dynamic-to-basic", "external.hdd-defrag-execution", "external.ssd-secure-erase", "external.bitlocker-mutation", "external.hardware-wipe")
            },
            @{
                kind = "documentation"
                path = "docs\PARTITION_MANAGER_CERTIFICATION.md"
                patterns = @("Disposable VHD data-disk matrix", "External Gates")
            }
        )
    }
)

Push-Location $ProjectRoot
try {
    foreach ($feature in $features) {
        Assert-Feature -Feature $feature
    }

    Write-Host "Partition Manager feature matrix passed: $($features.Count) feature groups verified."
}
finally {
    Pop-Location
}

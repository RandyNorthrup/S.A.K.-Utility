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

    # A missing or misspelled 'evidence' key would make the foreach iterate zero times and the
    # feature pass with no checks at all. Fail closed instead of silently verifying nothing.
    if (($null -eq $Feature.evidence) -or (@($Feature.evidence).Count -eq 0)) {
        throw "Partition Manager feature '$($Feature.id)' has no evidence groups"
    }
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
                patterns = @("Resize/Move Partition", "Extend Partition", "Move partition start", "Offline move will back up")
            },
            @{
                kind = "test"
                path = "tests\unit\test_partition_manager_core.cpp"
                patterns = @("safetyValidator_allowsResizeIntoAdjacentFreeSpace", "scriptBuilder_buildsOfflineMoveAndMetadataScripts", "safetyValidator_allowsConfirmedOfflineRebuildOperations")
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
                patterns = @("convert.exe %1: /FS:NTFS /NoSecurity /X")
            },
            @{
                kind = "test"
                path = "tests\unit\test_partition_manager_core.cpp"
                patterns = @("safetyValidator_blocksUnsupportedFileSystemConversion")
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
                kind = "certification matrix"
                path = "certification\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
                patterns = @("vhd.image-clone", "vhd.image-restore", "every_other_sector_matches", "vhd.partition-clone-region", "the_marker_after_the_region_is_kept")
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
                patterns = @('bcdboot.exe %1 /s \"${sakBootLetter}:\" /f UEFI', "reagentc.exe /info")
            },
            @{
                kind = "script builder"
                path = "src\core\partition_scripts\os_migration.ps1"
                patterns = @("function Set-SakTargetBoot")
            },
            @{
                kind = "script builder"
                path = "src\core\partition_scripts\target_boot_files.ps1"
                patterns = @("function Invoke-SakTargetBootFiles")
            },
            @{
                kind = "test"
                path = "tests\unit\test_partition_os_migration.cpp"
                patterns = @("scriptRefusesEverythingBeforeTheFirstWrite")
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
            }
        )
    },
    @{
        id = "commercial-utility-actions"
        evidence = @(
            @{
                kind = "UI action"
                path = "src\gui\partition_manager_panel.cpp"
                patterns = @("Quick Partition", "Data Recovery", "raw volume/device path", "FileRecoveryEngine::scanOfflineImage", "Allocate Free Space", "Allocate Free Space To", "Back up adjacent donor", "MovePartition", "Space Analyzer", "Largest Files", "File Types", "Copy Path", "Disk Benchmark", "Manage BitLocker", "BitLocker status table", "Copy BitLocker commands", "Disk Defrag", "Disk optimization status table", "Copy Optimize commands", "Erase SSD (TRIM + Overwrite)", "Erase SSD readiness table", "Queue SSD erase", "Make Bootable Media")
            },
            @{
                kind = "data recovery engine"
                path = "src\core\file_recovery_engine.cpp"
                patterns = @("scanOfflineImage", "restoreCandidates", "PNG image", "JPEG image", "PDF document", "source_not_mutated")
            },
            @{
                kind = "panel test"
                path = "tests\unit\test_partition_manager_panel.cpp"
                patterns = @("Quick Partition", "Data Recovery", "image and raw path recovery", "allocateFreeSpaceQueuesAdjacentDonorOperation", "unallocatedAllocateFreeSpaceQueuesAdjacentEngines", "formerCommercialCompatibilityActionsQueueDirectEngines", "Allocate free space amount", "Space Analyzer", "Largest Files", "File Types", "Copy Path", "Disk Benchmark", "Manage BitLocker", "manageBitLockerShowsStatusDialog", "Disk Defrag", "diskDefragShowsOptimizeDialog", "Erase SSD (TRIM + Overwrite)", "ssdSecureEraseShowsQueueDialog", "Make Bootable Media")
            },
            @{
                kind = "core test"
                path = "tests\unit\test_partition_manager_core.cpp"
                patterns = @("fileRecoveryEngine_scansAndRestoresOfflineImage", "scanOfflineImage", "restoreCandidates", "source_not_mutated", "scriptBuilder_buildsAllocateFreeSpaceScript", "safetyValidator_blocksUnsafeAllocateFreeSpacePayloads")
            },
            @{
                kind = "certification matrix"
                path = "certification\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
                patterns = @("external.file-level-data-recovery", "every_fixture_was_recovered", "external.allocate-free-space", "external.ssd-retrim", "the_app_warned_this_is_not_a_purge")
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
            }
        )
    },
    @{
        id = "certification-gates"
        evidence = @(
            @{
                kind = "readiness script"
                path = "scripts\check_release_readiness.ps1"
                patterns = @("verify_partition_manager_certification.ps1", "check_partition_manager_release_claims.ps1", "test_partition_manager_row_evidence.ps1", "test_partition_manager_certification_status.ps1")
            },
            @{
                kind = "certification verifier"
                path = "scripts\verify_partition_manager_certification.ps1"
                patterns = @("RequireVhdDataDiskEvidence", "RequireExternalGateEvidence", "get_partition_manager_certification_status.ps1", "merge-base --is-ancestor")
            },
            @{
                kind = "certification status"
                path = "scripts\get_partition_manager_certification_status.ps1"
                patterns = @("claim_level", "VirtualMachinesValidated", "NamedDevicesValidated", "Test-PmRowEvidence", "Test-PmEvidenceIndex", "site_wording")
            },
            @{
                kind = "evidence guards"
                path = "scripts\partition_manager_row_evidence.ps1"
                patterns = @("Test-PmRowEvidence", "Test-PmSnapshotsAgree", "Test-PmStep", "Test-PmMedia", "Test-PmEvidenceIndex")
            },
            @{
                kind = "independent readback"
                path = "scripts\partition_manager_readback.ps1"
                patterns = @("ConvertFrom-RbPartitionTable", "Get-RbRangeSha256", "Get-RbRangeDifferences", "Find-RbMarkerAnywhere", "Assert-RbDisposableMachine")
            },
            @{
                kind = "code digest"
                path = "scripts\partition_manager_code_digest.py"
                patterns = @("EXCLUDED_PREFIXES", "EXCLUDED_SUFFIXES")
            },
            @{
                kind = "step client"
                path = "tests\certification\partition_manager_scenario_step.cpp"
                patterns = @("runScenarioStep", "requestBuilders", "PartitionManagerController", "elevated-helper")
            },
            @{
                kind = "row orchestrator"
                path = "scripts\run_partition_manager_app_certification.ps1"
                patterns = @("Invoke-CertStep", "Assert-RbDisposableMachine", "New-PmRowReport")
            },
            @{
                kind = "final run"
                path = "scripts\run_partition_manager_final_certification.ps1"
                patterns = @("code_dirty", "select_partition_manager_evidence.ps1", "check_partition_manager_release_claims.ps1")
            },
            @{
                kind = "certification matrix"
                path = "certification\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
                patterns = @("ran_on", "not_proved", "vhd.create-format-resize-delete", "external.file-level-data-recovery", "external.allocate-free-space", "external.partition-move", "external.cluster-size-change", "external.primary-logical-conversion", "external.volume-serial-number", "external.dynamic-to-basic", "external.hdd-defrag-execution", "external.ssd-secure-erase", "external.bitlocker-mutation")
            }
        )
    }
)

# Pin the number of parity features this gate covers. Dropping (or duplicating) a feature block
# above changes $features.Count; without this check a removed feature would still yield a passing
# run over the reduced set. Update this count deliberately when features are added or removed.
$expectedFeatureCount = 12
if ($features.Count -ne $expectedFeatureCount) {
    throw ("Partition Manager feature matrix expected $expectedFeatureCount feature groups but " +
        "found $($features.Count); a feature was added or removed without updating this gate.")
}

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

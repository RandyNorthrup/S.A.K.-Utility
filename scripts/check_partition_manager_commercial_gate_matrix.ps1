<#
.SYNOPSIS
    Verifies Partition Manager destructive/commercial-parity features map to certification evidence.

.DESCRIPTION
    This static release gate prevents AOMEI/MiniTool-style destructive features from
    drifting away from their execution code, UI, tests, and
    certification/PARTITION_MANAGER_CERTIFICATION_MATRIX.json evidence IDs. It reads code and
    the matrix only: documentation prose is not evidence (owner ruling 2026-10-24), and the
    certification document's status block is generated from the matrix instead.
#>

[CmdletBinding()]
param(
    [string]$ProjectRoot = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent $PSScriptRoot
}
# Canonicalize to an absolute path once, before any file is read or Push-Location runs.
# A relative -ProjectRoot would otherwise resolve differently before and after
# Push-Location (the matrix and the evidence files would be read from different trees).
# Resolve-Path also fails closed when the root does not exist.
$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot -ErrorAction Stop).Path

function Get-RepoText {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RelativePath
    )

    $path = Join-Path $ProjectRoot $RelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Commercial gate file missing: $RelativePath"
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
        [string]$GateName
    )

    $text = Get-RepoText -RelativePath $RelativePath
    foreach ($pattern in $Patterns) {
        if (-not $text.Contains($pattern)) {
            throw "Partition Manager commercial gate '$GateName' missing evidence '$pattern' in $RelativePath"
        }
    }
}

$matrixPath = Join-Path $ProjectRoot "certification\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
$matrix = Get-Content -LiteralPath $matrixPath -Raw | ConvertFrom-Json
if ($matrix.schema_version -ne 1) {
    throw "Unsupported Partition Manager certification matrix schema_version: $($matrix.schema_version)"
}

$scenarioIds = @($matrix.vhd_scenarios | ForEach-Object { $_.id })
$scenarioIds += @($matrix.external_gates | ForEach-Object { $_.id })
$scenarioIds = @($scenarioIds | Sort-Object -Unique)

function Assert-MatrixIds {
    param(
        [Parameter(Mandatory = $true)]
        [string]$GateName,
        [Parameter(Mandatory = $true)]
        [string[]]$Ids
    )

    foreach ($id in $Ids) {
        if ($scenarioIds -notcontains $id) {
            throw "Partition Manager commercial gate '$GateName' references missing certification matrix ID: $id"
        }
    }
}

$uiPath = "src\gui\partition_manager_panel.cpp"
$scriptBuilderPath = "src\core\partition_script_builder.cpp"
$safetyValidatorPath = "src\core\partition_safety_validator.cpp"
$panelTestPath = "tests\unit\test_partition_manager_panel.cpp"
$coreTestPath = "tests\unit\test_partition_manager_core.cpp"

$gates = @(
    @{
        name = "Image clone and restore overwrite"
        ids = @("vhd.image-clone", "vhd.image-restore")
        evidence = @(
            @{ path = $uiPath; patterns = @("Create Disk Image", "Restore Disk Image") }
        )
    },
    @{
        name = "OS migration reboot proof"
        ids = @("external.os-migration-reboot")
        evidence = @(
            @{ path = $uiPath; patterns = @("Migrate OS to SSD/HDD Wizard") },
            @{ path = $scriptBuilderPath; patterns = @("SAK OS migration boot validation", "BIOS/UEFI boot order") }
        )
    },
    @{
        name = "Boot repair proof"
        ids = @("external.boot-repair-uefi", "external.boot-repair-bios")
        evidence = @(
            @{ path = $uiPath; patterns = @("Rebuild MBR / Boot Repair", "onRepairBoot") },
            @{ path = $scriptBuilderPath; patterns = @("bcdboot.exe %2 /s %1: /f UEFI", "reagentc.exe /info") }
        )
    },
    @{
        name = "Allocate Free Space"
        ids = @("external.allocate-free-space")
        evidence = @(
            @{ path = $uiPath; patterns = @("Allocate Free Space", "Allocate Free Space To", "onAllocateFreeSpace", "Back up adjacent donor") },
            @{ path = $scriptBuilderPath; patterns = @("buildAllocateFreeSpaceScript", "sak-allocate-backup", 'New-Partition -DiskNumber %1 -Size $donorRemainingBytes') },
            @{ path = $coreTestPath; patterns = @("scriptBuilder_buildsAllocateFreeSpaceScript", "safetyValidator_blocksUnsafeAllocateFreeSpacePayloads") },
            @{ path = $panelTestPath; patterns = @("allocateFreeSpaceQueuesAdjacentDonorOperation", "unallocatedAllocateFreeSpaceQueuesAdjacentEngines", "Allocate free space amount") }
        )
    },
    @{
        name = "Partition start move"
        ids = @("external.partition-move")
        evidence = @(
            @{ path = $uiPath; patterns = @("Move partition start", "Offline move will back up", "MovePartition") },
            @{ path = $scriptBuilderPath; patterns = @("buildMovePartitionScript", "Remove-Partition -DiskNumber %1", 'New-Partition -DiskNumber %7 -Size $targetSize -Offset ', '$targetOffset -DriveLetter $drive') },
            @{ path = $coreTestPath; patterns = @("scriptBuilder_buildsOfflineMoveAndMetadataScripts", "safetyValidator_allowsConfirmedOfflineRebuildOperations") },
            @{ path = $panelTestPath; patterns = @("unallocatedAllocateFreeSpaceQueuesAdjacentEngines", "Move Partition") }
        )
    },
    @{
        name = "Existing-volume cluster-size change"
        ids = @("external.cluster-size-change")
        evidence = @(
            @{ path = $uiPath; patterns = @("Change Cluster Size", "Back up, reformat with selected cluster size", "onChangeClusterSize") },
            @{ path = $scriptBuilderPath; patterns = @("buildChangeClusterSizeScript", "Get-SakFileManifest") },
            @{ path = $panelTestPath; patterns = @("changeClusterSizeQueuesVerifiedReformatOperation", "Target cluster size") }
        )
    },
    @{
        name = "Primary/logical conversion"
        ids = @("external.primary-logical-conversion")
        evidence = @(
            @{ path = $uiPath; patterns = @("Convert Primary/Logical", "onConvertPrimaryLogical", "Primary logical backup directory") },
            @{ path = $scriptBuilderPath; patterns = @("buildConvertPrimaryLogicalScript", "create partition logical", "Assert-SakManifestMatch") },
            @{ path = $coreTestPath; patterns = @("ConvertPrimaryLogical", "safetyValidator_allowsConfirmedOfflineRebuildOperations") },
            @{ path = $panelTestPath; patterns = @("formerCommercialCompatibilityActionsQueueDirectEngines", "Primary logical backup directory") }
        )
    },
    @{
        name = "Volume serial-number change"
        ids = @("external.volume-serial-number")
        evidence = @(
            @{ path = $uiPath; patterns = @("Reformat for New Serial Number", "onChangeVolumeSerialNumber", "Volume serial backup directory") },
            @{ path = $scriptBuilderPath; patterns = @("buildChangeVolumeSerialNumberScript", "Get-SakVolumeSerial", 'Format-Volume -DriveLetter $drive') },
            @{ path = $coreTestPath; patterns = @("ChangeVolumeSerialNumber", "Volume serial changed") },
            @{ path = $panelTestPath; patterns = @("formerCommercialCompatibilityActionsQueueDirectEngines", "Volume serial backup directory") }
        )
    },
    @{
        name = "Dynamic disk to basic"
        ids = @("external.dynamic-to-basic")
        evidence = @(
            @{ path = $uiPath; patterns = @("Convert Dynamic Disk to Basic", "onConvertDynamicDiskToBasic", "Dynamic to basic backup directory") },
            @{ path = $scriptBuilderPath; patterns = @("buildConvertDynamicDiskToBasicScript", "delete volume override", "convert basic") },
            @{ path = $coreTestPath; patterns = @("ConvertDynamicDiskToBasic", "safetyValidator_allowsConfirmedOfflineRebuildOperations") },
            @{ path = $panelTestPath; patterns = @("formerCommercialCompatibilityActionsQueueDirectEngines", "Dynamic to basic backup directory") }
        )
    },
    @{
        name = "Direct HDD defrag execution"
        ids = @("external.hdd-defrag-execution")
        evidence = @(
            @{ path = $uiPath; patterns = @("Disk Defrag", "Queue HDD defrag through cancellable elevated Apply") },
            @{ path = $panelTestPath; patterns = @("Disk Defrag", "Queue HDD defrag") }
        )
    },
    @{
        name = "SSD ReTrim and purge warning"
        ids = @("external.ssd-retrim")
        evidence = @(
            @{ path = $uiPath; patterns = @("TRIM SSD Volume", "Optimize-Volume") }
        )
    },
    @{
        name = "BitLocker status and mutation"
        ids = @("external.bitlocker", "external.bitlocker-mutation")
        evidence = @(
            @{ path = $uiPath; patterns = @("Manage BitLocker", "Queue BitLocker unlock", "BitLockerUnlock") },
            # Unlock is done via the in-process Unlock-BitLocker cmdlet, NOT manage-bde.exe -unlock,
            # so the recovery password never lands on a child-process command line (see
            # partition_script_builder.cpp buildBitLockerScript security note).
            @{ path = $scriptBuilderPath; patterns = @("Unlock-BitLocker", "manage-bde.exe -protectors -disable", "manage-bde.exe -protectors -enable") },
            @{ path = $panelTestPath; patterns = @("Manage BitLocker", "manageBitLockerShowsStatusDialog") }
        )
    },
    @{
        name = "System MBR-to-GPT"
        ids = @("external.system-mbr2gpt")
        evidence = @(
            @{ path = $scriptBuilderPath; patterns = @("mbr2gpt.exe /validate", "mbr2gpt.exe /convert") }
        )
    },
    @{
        name = "USB removable destructive proof"
        ids = @("external.usb-removable")
        evidence = @(
            @{ path = $safetyValidatorPath; patterns = @("disk.is_removable", "Target disk is removable") }
        )
    },
    @{
        name = "Physical hardware wipe"
        ids = @("external.hardware-wipe")
        # This gate maps capability scope to its evidence ID. Current certification
        # levels belong to the status/claim verifier, never a hard-coded promotion sentence.
        evidence = @(
            @{ path = $scriptBuilderPath; patterns = @("Clear-Disk -Number %1 -RemoveData -RemoveOEM") }
        )
    }
)

Push-Location $ProjectRoot
try {
    $mappedIds = New-Object System.Collections.Generic.HashSet[string]
    foreach ($gate in $gates) {
        Assert-MatrixIds -GateName $gate.name -Ids $gate.ids
        foreach ($id in $gate.ids) {
            [void]$mappedIds.Add($id)
        }
        foreach ($evidence in $gate.evidence) {
            Assert-Contains -RelativePath $evidence.path -Patterns $evidence.patterns -GateName $gate.name
        }
    }

    Write-Host "Partition Manager commercial/destructive feature matrix passed: $($gates.Count) feature groups mapped to $($mappedIds.Count) certification IDs."
}
finally {
    Pop-Location
}

<#
.SYNOPSIS
    Verifies Partition Manager destructive/commercial-parity features map to certification evidence.

.DESCRIPTION
    This static release gate prevents AOMEI/MiniTool-style destructive feature
    claims from drifting away from execution code, tests, documentation, and
    docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json evidence IDs.
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

$matrixPath = Join-Path $ProjectRoot "docs\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
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
$planPath = "docs\PARTITION_MANAGER_PANEL_PLAN.md"
$certificationPath = "docs\PARTITION_MANAGER_CERTIFICATION.md"
$auditPath = "docs\PRODUCTION_GRADE_AUDIT.md"
$readmePath = "README.md"
$changelogPath = "CHANGELOG.md"
$panelTestPath = "tests\unit\test_partition_manager_panel.cpp"
$coreTestPath = "tests\unit\test_partition_manager_core.cpp"

$gates = @(
    @{
        name = "Image clone and restore overwrite"
        ids = @("vhd.image-clone", "vhd.image-restore")
        evidence = @(
            @{ path = $uiPath; patterns = @("Create Disk Image", "Restore Disk Image") },
            @{ path = $planPath; patterns = @("offline VHD image clone", "offline VHD image restore") },
            @{ path = $certificationPath; patterns = @("Offline VHD image clone", "Offline VHD image restore") }
        )
    },
    @{
        name = "OS migration reboot proof"
        ids = @("external.os-migration-reboot")
        evidence = @(
            @{ path = $uiPath; patterns = @("Migrate OS to SSD/HDD Wizard") },
            @{ path = $scriptBuilderPath; patterns = @("SAK OS migration boot validation", "BIOS/UEFI boot order") },
            @{ path = $certificationPath; patterns = @("OS migration target boot and firmware-order proof") }
        )
    },
    @{
        name = "Boot repair proof"
        ids = @("external.boot-repair-uefi", "external.boot-repair-bios")
        evidence = @(
            @{ path = $uiPath; patterns = @("Rebuild MBR / Boot Repair", "onRepairBoot") },
            @{ path = $scriptBuilderPath; patterns = @("bcdboot.exe %2 /s %1: /f UEFI", "reagentc.exe /info") },
            @{ path = $certificationPath; patterns = @("UEFI boot repair", "BIOS/MBR boot repair") }
        )
    },
    @{
        name = "Allocate Free Space"
        ids = @("external.allocate-free-space")
        evidence = @(
            @{ path = $uiPath; patterns = @("Allocate Free Space", "Allocate Free Space To", "onAllocateFreeSpace", "Back up adjacent donor") },
            @{ path = $scriptBuilderPath; patterns = @("buildAllocateFreeSpaceScript", "sak-allocate-backup", 'New-Partition -DiskNumber %1 -Size $donorRemainingBytes') },
            @{ path = $coreTestPath; patterns = @("scriptBuilder_buildsAllocateFreeSpaceScript", "safetyValidator_blocksUnsafeAllocateFreeSpacePayloads") },
            @{ path = $panelTestPath; patterns = @("allocateFreeSpaceQueuesAdjacentDonorOperation", "unallocatedAllocateFreeSpaceQueuesAdjacentEngines", "Allocate free space amount") },
            @{ path = $certificationPath; patterns = @("Allocate Free Space adjacent donor-volume backup/delete/extend/recreate/restore proof") }
        )
    },
    @{
        name = "Partition start move"
        ids = @("external.partition-move")
        evidence = @(
            @{ path = $uiPath; patterns = @("Move partition start", "Offline move will back up", "MovePartition") },
            @{ path = $scriptBuilderPath; patterns = @("buildMovePartitionScript", "Remove-Partition -DiskNumber %1", 'New-Partition -DiskNumber %7 -Size $targetSize -Offset ', '$targetOffset -DriveLetter $drive') },
            @{ path = $coreTestPath; patterns = @("scriptBuilder_buildsOfflineMoveAndMetadataScripts", "safetyValidator_allowsConfirmedOfflineRebuildOperations") },
            @{ path = $panelTestPath; patterns = @("unallocatedAllocateFreeSpaceQueuesAdjacentEngines", "Move Partition") },
            @{ path = $planPath; patterns = @("start-move", "MovePartition") },
            @{ path = $certificationPath; patterns = @("Offline partition start-move proof") }
        )
    },
    @{
        name = "Existing-volume cluster-size change"
        ids = @("external.cluster-size-change")
        evidence = @(
            @{ path = $uiPath; patterns = @("Change Cluster Size", "Back up, reformat with selected cluster size", "onChangeClusterSize") },
            @{ path = $scriptBuilderPath; patterns = @("buildChangeClusterSizeScript", "Get-SakFileManifest") },
            @{ path = $panelTestPath; patterns = @("changeClusterSizeQueuesVerifiedReformatOperation", "Target cluster size") },
            @{ path = $certificationPath; patterns = @("Existing-volume cluster-size change proof") }
        )
    },
    @{
        name = "Primary/logical conversion"
        ids = @("external.primary-logical-conversion")
        evidence = @(
            @{ path = $uiPath; patterns = @("Convert Primary/Logical", "onConvertPrimaryLogical", "Primary logical backup directory") },
            @{ path = $scriptBuilderPath; patterns = @("buildConvertPrimaryLogicalScript", "create partition logical", "Assert-SakManifestMatch") },
            @{ path = $coreTestPath; patterns = @("ConvertPrimaryLogical", "safetyValidator_allowsConfirmedOfflineRebuildOperations") },
            @{ path = $panelTestPath; patterns = @("formerCommercialCompatibilityActionsQueueDirectEngines", "Primary logical backup directory") },
            @{ path = $certificationPath; patterns = @("Primary/logical conversion on a disposable MBR disk") }
        )
    },
    @{
        name = "Volume serial-number change"
        ids = @("external.volume-serial-number")
        evidence = @(
            @{ path = $uiPath; patterns = @("Change Serial Number", "onChangeVolumeSerialNumber", "Volume serial backup directory") },
            @{ path = $scriptBuilderPath; patterns = @("buildChangeVolumeSerialNumberScript", "Get-SakVolumeSerial", 'Format-Volume -DriveLetter $drive') },
            @{ path = $coreTestPath; patterns = @("ChangeVolumeSerialNumber", "Volume serial changed") },
            @{ path = $panelTestPath; patterns = @("formerCommercialCompatibilityActionsQueueDirectEngines", "Volume serial backup directory") },
            @{ path = $certificationPath; patterns = @("Volume serial-number metadata mutation proof") }
        )
    },
    @{
        name = "Dynamic disk to basic"
        ids = @("external.dynamic-to-basic")
        evidence = @(
            @{ path = $uiPath; patterns = @("Convert Dynamic Disk to Basic", "onConvertDynamicDiskToBasic", "Dynamic to basic backup directory") },
            @{ path = $scriptBuilderPath; patterns = @("buildConvertDynamicDiskToBasicScript", "delete volume override", "convert basic") },
            @{ path = $coreTestPath; patterns = @("ConvertDynamicDiskToBasic", "safetyValidator_allowsConfirmedOfflineRebuildOperations") },
            @{ path = $panelTestPath; patterns = @("formerCommercialCompatibilityActionsQueueDirectEngines", "Dynamic to basic backup directory") },
            @{ path = $certificationPath; patterns = @("Dynamic disk to basic conversion proof") }
        )
    },
    @{
        name = "Direct HDD defrag execution"
        ids = @("external.hdd-defrag-execution")
        evidence = @(
            @{ path = $uiPath; patterns = @("Disk Defrag", "Queue HDD defrag through cancellable elevated Apply") },
            @{ path = $panelTestPath; patterns = @("Disk Defrag", "Queue HDD defrag") },
            @{ path = $certificationPath; patterns = @("Direct in-app HDD defrag execution proof") }
        )
    },
    @{
        name = "SSD ReTrim and purge warning"
        ids = @("external.ssd-retrim")
        evidence = @(
            @{ path = $uiPath; patterns = @("Attach evidence to external.ssd-retrim", "Optimize-Volume") },
            @{ path = $planPath; patterns = @("ReTrim", "purge warning") },
            @{ path = $certificationPath; patterns = @("SSD/NVMe ReTrim and vendor purge warning proof") }
        )
    },
    @{
        name = "BitLocker status and mutation"
        ids = @("external.bitlocker", "external.bitlocker-mutation")
        evidence = @(
            @{ path = $uiPath; patterns = @("Manage BitLocker", "Queue BitLocker unlock", "BitLockerUnlock") },
            @{ path = $scriptBuilderPath; patterns = @("manage-bde.exe -unlock", "manage-bde.exe -protectors -disable", "manage-bde.exe -protectors -enable") },
            @{ path = $panelTestPath; patterns = @("Manage BitLocker", "manageBitLockerShowsStatusDialog") },
            @{ path = $certificationPath; patterns = @("BitLocker locked/unlocked data-volume blocker proof", "In-app BitLocker unlock/suspend/resume mutation proof") }
        )
    },
    @{
        name = "System MBR-to-GPT"
        ids = @("external.system-mbr2gpt")
        evidence = @(
            @{ path = $scriptBuilderPath; patterns = @("mbr2gpt.exe /validate", "mbr2gpt.exe /convert") },
            @{ path = $planPath; patterns = @("Safe system-disk MBR-to-GPT", "System-critical") },
            @{ path = $certificationPath; patterns = @("System MBR-to-GPT conversion in a disposable boot VM") }
        )
    },
    @{
        name = "USB removable destructive proof"
        ids = @("external.usb-removable")
        evidence = @(
            @{ path = $readmePath; patterns = @("removable", "USB") },
            @{ path = $planPath; patterns = @("USB removable drive") },
            @{ path = $certificationPath; patterns = @("USB removable disk destructive operation proof") }
        )
    },
    @{
        name = "Physical hardware wipe"
        ids = @("external.hardware-wipe")
        evidence = @(
            @{ path = $scriptBuilderPath; patterns = @("Clear-Disk -Number %1 -RemoveData -RemoveOEM") },
            @{ path = $auditPath; patterns = @("physical wipe proof is imported in the 18/18 external VM/hardware/lab evidence set") },
            @{ path = $certificationPath; patterns = @("Non-system hardware wipe on disposable physical disk") }
        )
    },
    @{
        name = "Release-facing certification boundary"
        ids = @("external.allocate-free-space", "external.partition-move", "external.volume-serial-number", "external.dynamic-to-basic")
        evidence = @(
            @{ path = $readmePath; patterns = @("Move partition start", "volume serial-number") },
            @{ path = $changelogPath; patterns = @("MovePartition", "direct queued UI paths") },
            @{ path = $auditPath; patterns = @("MovePartition", "primary/logical conversion", "volume serial-number regeneration", "dynamic-to-basic conversion") }
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

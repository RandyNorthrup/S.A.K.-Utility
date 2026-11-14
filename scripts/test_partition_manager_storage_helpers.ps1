<#
.SYNOPSIS
    Proves the shared storage helper's answers, and that Partition Manager's run scripts hide no failure.

.DESCRIPTION
    The helper's Windows commands are replaced with stand-ins that report a present item, a missing one,
    or a failure, so it touches no disk. The rest reads script text.
#>

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)

function Assert-Condition {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )
    if (-not $Condition) { throw $Message }
}

function Assert-NoCertificationFailureSilences {
    $sharedStorageRunners = @(
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
    Assert-Condition -Condition ($partitionScripts.Count -gt 0) -Message 'No Partition Manager script was found to check'
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

function Test-E2fsckOutputRule {
    # e2fsck -n exits 0 after answering no to "Abort?" (operation proof, 2026-11-11): the
    # validation scripts read its output as the app does, and every e2fsck they run goes through it.
    . (Join-Path $ProjectRoot 'scripts\partition_manager_storage.ps1')
    $cut = @(
        'e2fsck 1.47.1 (20-May-2024)',
        'The filesystem size (according to the superblock) is 131072 blocks',
        'The physical size of the device is 65536 blocks',
        'Either the superblock or the partition table is likely to be corrupt!',
        'Abort? no',
        '',
        'Pass 1: Checking inodes, blocks, and sizes'
    ) -join "`n"
    Assert-Condition -Condition ([bool](Get-SakE2fsckOutputProblem -Output $cut)) `
        -Message 'e2fsck output that asked to abort passed'
    foreach ($line in @('Either the superblock or the partition table is likely to be corrupt!',
                        'The physical size of the device is 65536 blocks',
                        'Abort? no')) {
        Assert-Condition -Condition ([bool](Get-SakE2fsckOutputProblem -Output "Pass 1`n$line`nPass 2")) `
            -Message "e2fsck output with only '$line' passed"
    }
    $clean = "e2fsck 1.47.1 (20-May-2024)`nPass 5: Checking group summary information`n" +
        'SAK_EXT4: 11/65536 files (0.0% non-contiguous), 8859/262144 blocks'
    Assert-Condition -Condition (-not (Get-SakE2fsckOutputProblem -Output $clean)) `
        -Message 'a clean e2fsck run was refused'
    $passed = [pscustomobject]@{ name = 'e2fsck-clean'; exit_code = 0; output = $clean } | Assert-SakE2fsckPassed
    Assert-Condition -Condition ($passed.name -eq 'e2fsck-clean') -Message 'Assert-SakE2fsckPassed did not hand the run on'
    $refused = $false
    try {
        [pscustomobject]@{ name = 'e2fsck-cut'; exit_code = 0; output = $cut } | Assert-SakE2fsckPassed | Out-Null
    } catch { $refused = $_.Exception.Message -match 'e2fsck-cut: exit code 0, but' }
    Assert-Condition -Condition $refused -Message 'Assert-SakE2fsckPassed let exit code 0 with "Abort? no" pass'

    foreach ($relativePath in @('scripts\run_partition_manager_ext_filesystem_vm_gate.ps1',
                                'scripts\run_partition_manager_physical_cross_filesystem_destructive_validation.ps1')) {
        $calls = @(Get-Content -LiteralPath (Join-Path $ProjectRoot $relativePath) -ErrorAction Stop |
            Where-Object { $_ -match 'Invoke-NativeCommand -Name "e2fsck-' })
        Assert-Condition -Condition ($calls.Count -ge 5) -Message "$relativePath runs fewer e2fsck checks than expected"
        $unread = @($calls | Where-Object { $_ -notmatch '\| Assert-SakE2fsckPassed' })
        Assert-Condition -Condition ($unread.Count -eq 0) `
            -Message "$relativePath reads only the exit code of: $($unread -join ' / ')"
    }
}

Assert-NoCertificationFailureSilences
Test-PartitionManagerStorageHelpers
Test-E2fsckOutputRule
Write-Host 'Partition Manager storage helper and run scripts: all checks held.'

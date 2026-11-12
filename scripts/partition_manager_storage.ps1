<#
.SYNOPSIS
    Shared fail-closed storage query and VHD cleanup helpers for Partition Manager certification.

.DESCRIPTION
    Storage cmdlets report an empty RAW-disk partition set and an absent partition volume as
    ObjectNotFound errors on some Windows builds. These helpers classify only that exact category
    as an expected absence. Every other query or cleanup failure remains terminating.
#>

function Get-SakDiskPartitions {
    param([Parameter(Mandatory = $true)] [int]$DiskNumber)

    try {
        return @(Get-Partition -DiskNumber $DiskNumber -ErrorAction Stop)
    }
    catch {
        if ($_.CategoryInfo.Category -eq [System.Management.Automation.ErrorCategory]::ObjectNotFound) {
            return @()
        }
        throw
    }
}

function Get-SakPartitionVolume {
    param([Parameter(Mandatory = $true)] [object]$Partition)

    try {
        return $Partition | Get-Volume -ErrorAction Stop
    }
    catch {
        if ($_.CategoryInfo.Category -eq [System.Management.Automation.ErrorCategory]::ObjectNotFound) {
            return $null
        }
        throw
    }
}

function Get-SakPartitionsByDriveLetter {
    param([Parameter(Mandatory = $true)] [string]$DriveLetter)

    try {
        return @(Get-Partition -DriveLetter $DriveLetter -ErrorAction Stop)
    }
    catch {
        if ($_.CategoryInfo.Category -eq [System.Management.Automation.ErrorCategory]::ObjectNotFound) {
            return @()
        }
        throw
    }
}

function Get-SakVolumeByDriveLetter {
    param([Parameter(Mandatory = $true)] [string]$DriveLetter)

    try {
        return Get-Volume -DriveLetter $DriveLetter -ErrorAction Stop
    }
    catch {
        if ($_.CategoryInfo.Category -eq [System.Management.Automation.ErrorCategory]::ObjectNotFound) {
            return $null
        }
        throw
    }
}

function Dismount-SakDiskImageIfAttached {
    param([Parameter(Mandatory = $true)] [string]$Path)

    $image = Get-DiskImage -ImagePath $Path -ErrorAction Stop
    if ($image.Attached) {
        Dismount-DiskImage -ImagePath $Path -ErrorAction Stop | Out-Null
    }
}

function Get-SakE2fsckOutputProblem {
    <#
    .SYNOPSIS
        Why e2fsck's output says the file system did not pass whatever its exit code, or ''.
    .DESCRIPTION
        A check (-n) answers no to every question and exits 0 even when e2fsck asked to abort: on
        a 128 MiB ext4 cut to a 64 MiB partition it printed "The physical size of the device is
        ... blocks", "... the partition table is likely to be corrupt!" and "Abort? no", and
        exited 0 (operation proof, 2026-11-11; the app's own rule since then,
        PartitionFileSystemToolRunner::e2fsckProblem). Any of the three is a failure.
    #>
    param([AllowEmptyString()] [string]$Output)

    if ($Output -match '(?im)likely to be corrupt') {
        return 'e2fsck says the superblock or the partition table is likely corrupt'
    }
    if ($Output -match '(?im)physical size of the device') {
        return 'e2fsck says the file system is larger than its device'
    }
    if ($Output -match '(?im)^\s*Abort\? no\b') {
        return 'e2fsck asked to stop ("Abort?") and went on only because a check answers no'
    }
    return ''
}

function Assert-SakE2fsckPassed {
    <#
    .SYNOPSIS
        Throws when an e2fsck run's output says the file system did not pass (see
        Get-SakE2fsckOutputProblem); returns the run otherwise. The exit code is the caller's.
    #>
    param([Parameter(Mandatory = $true, ValueFromPipeline = $true)] [object]$Run)

    process {
        $problem = Get-SakE2fsckOutputProblem -Output ([string]$Run.output)
        if ($problem) { throw "$($Run.name): exit code $($Run.exit_code), but $problem. $($Run.output)" }
        $Run
    }
}

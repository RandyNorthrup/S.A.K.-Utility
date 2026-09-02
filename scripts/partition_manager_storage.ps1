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

<#
.SYNOPSIS
    The exact words the site may use for a Partition Manager certification result.

.DESCRIPTION
    A function of the computed status and of nothing else, so the words cannot say more than the
    evidence. A device is named only from what a row that ran on it reported. Public text does not
    say "certified", and of Apple's file systems it says only that writes were validated with Apple's
    own tools.
#>

Set-StrictMode -Version 2.0

$Script:PmWordingBackUp = 'Back up before changing a disk.'

function Get-PmDeviceDescription {
    # "Seagate Expansion Desk 8 TB USB hard drive", from what Windows reported.
    param([Parameter(Mandatory = $true)] [object]$RanOn)
    $bytes = [decimal]"$($RanOn.size_bytes)"
    $size = if ($bytes -ge 1000000000000) { "$([Math]::Round($bytes / 1000000000000)) TB" } else { "$([Math]::Round($bytes / 1000000000)) GB" }
    $kind = switch -CaseSensitive ("$($RanOn.media_type)") {
        'HDD' { 'hard drive' }
        'SSD' { 'SSD' }
        default { 'disk' }
    }
    return "$($RanOn.model) $size $($RanOn.bus_type) $kind"
}

function Get-PmNamedDevices {
    param([AllowEmptyCollection()] [object[]]$Rows)
    $devices = @()
    foreach ($row in $Rows) {
        if ($row.accepted -and $null -ne $row.ran_on -and "$($row.ran_on.class)" -ceq 'physical') {
            $devices += (Get-PmDeviceDescription -RanOn $row.ran_on)
        }
    }
    return @($devices | Sort-Object -Unique)
}

function Get-PmRowsOnDevices {
    param([AllowEmptyCollection()] [object[]]$Rows)
    return @($Rows | Where-Object { $_.accepted -and $null -ne $_.ran_on -and "$($_.ran_on.class)" -ceq 'physical' })
}

function Format-PmList {
    param([AllowEmptyCollection()] [string[]]$Items)
    if ($Items.Count -le 1) { return ($Items -join '') }
    if ($Items.Count -eq 2) { return "$($Items[0]) and $($Items[1])" }
    return (($Items[0..($Items.Count - 2)] -join ', ') + ", and $($Items[-1])")
}

function Get-PmNumberWord {
    param([int]$Number)
    $words = @('zero', 'one', 'two', 'three', 'four', 'five', 'six', 'seven', 'eight', 'nine', 'ten', 'eleven', 'twelve')
    if ($Number -lt $words.Count) { return $words[$Number] }
    return "$Number"
}

function Get-PmDeviceSentences {
    # Says what ran on real devices, and what that leaves out.
    param([AllowEmptyCollection()] [object[]]$Rows)
    $devices = @(Get-PmNamedDevices -Rows $Rows)
    if ($devices.Count -eq 0) { return @() }
    $sentences = @("Some operations were also tested on one $(Format-PmList -Items @($devices | ForEach-Object { $_ }))." -replace ', and ', ', and one ' -replace ' and (?!one )', ' and one ')
    $buses = @(Get-PmRowsOnDevices -Rows $Rows | ForEach-Object { "$($_.ran_on.bus_type)" })
    $notTested = @('SATA', 'NVMe' | Where-Object { $buses -cnotcontains $_ })
    $sentences += if ($notTested.Count -gt 0) { "Internal $($notTested -join ' and ') drives were not tested, and results on other devices may differ." } else { 'Results on other devices may differ.' }
    $erase = @(Get-PmRowsOnDevices -Rows $Rows | Where-Object { $_.id -ceq 'external.ssd-secure-erase' })
    if ($erase.Count -gt 0) {
        $sentences += 'SSD erase is TRIM plus overwrite, not an ATA or NVMe sanitize.'
    }
    return $sentences
}

function Get-PmAppleSentence {
    param([AllowNull()] [object]$AppleMatrix)
    if ($null -eq $AppleMatrix) { return @() }
    $done = @($AppleMatrix.capabilities | Where-Object { "$($_.status)" -ceq 'certified' })
    if ($done.Count -eq 0) { return @() }
    return @("Writes to Apple file systems were validated with Apple's own tools.")
}

function Get-PmSiteWording {
    <#
    .SYNOPSIS
        The words for a claim level. Throws for a level it has no words for.
    #>
    param(
        [Parameter(Mandatory = $true)] [string]$ClaimLevel,
        [Parameter(Mandatory = $true)] [string]$Version,
        [AllowEmptyCollection()] [object[]]$Rows = @(),
        [int]$VhdRowCount = 12,
        [AllowNull()] [object]$AppleMatrix = $null
    )
    $onDevices = @(Get-PmNamedDevices -Rows $Rows)
    $sentences = switch -CaseSensitive ($ClaimLevel) {
        'FailedEvidence' { @("On the v$Version code, a disk scenario failed its check, so Partition Manager carries no validation claim for this version.") }
        'CodeCompleteOnly' { @("On the v$Version code, Partition Manager has automated tests, and its disk operations have not been validated on disposable disks.") }
        'VhdDataDiskCertified' {
            $text = @("On the v$Version code, $(Get-PmNumberWord -Number $VhdRowCount) disk scenarios ran through the app's own Partition Manager controller on disposable virtual disks in a Windows 11 VM.")
            if ($onDevices.Count -eq 0) { $text += 'Nothing was tested on real hardware.' } else { $text += (Get-PmDeviceSentences -Rows $Rows) }
            $text
        }
        'VirtualMachinesValidated' {
            $text = @("On the v$Version code, Partition Manager operations, including boot repair, MBR-to-GPT conversion and OS migration, ran through the app on disposable virtual machines and virtual disks.")
            if ($onDevices.Count -eq 0) { $text += 'Real SSD, NVMe, USB and hard-disk behaviour was not tested.' } else { $text += (Get-PmDeviceSentences -Rows $Rows) }
            $text
        }
        'NamedDevicesValidated' {
            @("On the v$Version code, Partition Manager operations, including boot repair, MBR-to-GPT conversion and OS migration, ran through the app on disposable virtual machines and virtual disks.") + (Get-PmDeviceSentences -Rows $Rows)
        }
        default { throw "There are no words for the claim level $ClaimLevel" }
    }
    return ((@($sentences) + (Get-PmAppleSentence -AppleMatrix $AppleMatrix) + @($Script:PmWordingBackUp)) -join ' ')
}

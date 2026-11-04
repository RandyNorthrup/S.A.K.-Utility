<#
.SYNOPSIS
    Runs groups of Partition Manager certification rows on the rig, in the order they need.

.DESCRIPTION
    A group is what can run together. Most are a list of rows in the guest. The boot groups are a
    chain, because a row's disk is the one the row before it left: migrate a Windows, start it alone
    to prove it boots, break it, prove that stopped it starting, repair it through the app, and start
    it again. What only the host can see joins the row's readback before the report is final.

    Trial evidence belongs in scratch. The final run writes to a folder that
    select_partition_manager_evidence.ps1 then selects as current. It changes no disk of the machine
    it runs on, and it stops at the first group that fails.

.PARAMETER Group
    vhd      The VHD rows, and the wipe on a throwaway VDI, because every VHDX reports thin.
    data     Data-volume rows, on VHDs in the guest. The dynamic disk row runs on a throwaway VDI,
             because Windows will not make a virtual disk file dynamic.
    boot-uefi  OS migration, then UEFI boot repair on what it wrote.
    boot-bios  A BIOS install built for the purpose, BIOS boot repair, then system MBR-to-GPT.
    ssd-virtual  The SSD rows on a VDI the guest is told does not rotate and can discard.
    hdd-virtual  The defrag row on a plain VDI.
    usb-virtual  The USB row on a VDI behind a virtual USB controller, with no host device involved.
    ssd-device, hdd-device    The device rows on the pinned device.
    data-device  Data-volume rows, on small partitions of the pinned hard drive. No whole-disk pass.

    With -PcFile the device groups run on the physical PC that file names, on drives pinned there. The
    other groups need things only the VM rig has: throwaway disks it can attach, and a VM of its own to
    start a disk in. They stop rather than run somewhere else.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('vhd', 'data', 'boot-uefi', 'boot-bios', 'ssd-virtual', 'hdd-virtual', 'usb-virtual', 'ssd-device', 'hdd-device', 'data-device')]
    [string]$Group,
    [Parameter(Mandatory = $true)] [string]$EvidenceDir,
    [string[]]$Rows = @(),
    [string]$CredFile = 'temp\vm-auth\sak-capture-technician.json',
    [string]$StageDir = '',
    [string]$PinFile = '',
    [string]$PcFile = '',
    [string]$Vm = 'Windows 11',
    [int]$RowMinutes = 240
)

$ErrorActionPreference = 'Stop'
# Under pwsh -File a list arrives as one comma-joined string.
$Rows = @($Rows | ForEach-Object { "$_" -split ',' } | Where-Object { $_ })
$Rig = Join-Path $PSScriptRoot 'run_partition_manager_certification_rig.ps1'
. (Join-Path $PSScriptRoot 'partition_manager_row_evidence.ps1')
$Script:MiB = [uint64]1048576

function Invoke-Rig {
    param([Parameter(Mandatory = $true)] [string]$Action, [hashtable]$More = @{})
    $arguments = @{ Action = $Action; CredFile = $CredFile; Vm = $Vm; EvidenceDir = $EvidenceDir; JobMinutes = $RowMinutes }
    # A step may say more, or say otherwise.
    foreach ($key in $More.Keys) { $arguments[$key] = $More[$key] }
    if ($PcFile) { $arguments.PcFile = $PcFile }
    & pwsh -NoProfile -File $Rig @(ConvertTo-RigArguments -Arguments $arguments)
    if ($LASTEXITCODE -ne 0) { throw "The rig stopped on $Action with exit code $LASTEXITCODE" }
}

function ConvertTo-RigArguments {
    param([hashtable]$Arguments)
    $list = @()
    foreach ($key in $Arguments.Keys) {
        $value = $Arguments[$key]
        if ($value -is [switch] -or $value -is [bool]) {
            if ($value) { $list += "-$key" }
        } elseif ($value -is [array]) {
            $list += "-$key"
            $list += ($value -join ',')
        } else {
            $list += "-$key"
            $list += "$value"
        }
    }
    return $list
}

function Say {
    param([string]$Text)
    "=== [$(Get-Date -Format HH:mm:ss)] $Text"
}

function Assert-RowPassed {
    param([string]$Id)
    $path = Join-Path (Join-Path $EvidenceDir $Id) 'report.json'
    if (-not (Test-Path -LiteralPath $path)) { throw "$Id wrote no report" }
    $report = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    if ("$($report.status)" -cne 'Passed') { throw "$Id failed: $($report.failure)" }
}

function Get-Proof {
    # A disk started alone, in a VM of its own, seen from inside.
    param([string]$Disk, [string]$Name, [string]$Firmware, [string]$RowDir, [string]$MarkerPath = '', [switch]$ExpectNoBoot)
    $output = Join-Path $RowDir "$Name.json"
    $more = @{ Disk = $Disk; ProofName = "sakcert-$Name"; Firmware = $Firmware; ProofOutput = $output; MarkerPath = $MarkerPath }
    if ($ExpectNoBoot) { $more.ExpectNoBoot = $true }
    # The rig's own output is for reading. Left in this function's output, it would become the result.
    Invoke-Rig -Action 'BootProof' -More $more | ForEach-Object { Write-Host "$_" }
    return (Get-Content -LiteralPath $output -Raw | ConvertFrom-Json)
}

function Invoke-GuestRows {
    param([string[]]$Ids, [hashtable]$More = @{})
    Invoke-Rig -Action 'Rows' -More (@{ Rows = $Ids } + $More)
}

function Invoke-RowsOnThrowawayDisk {
    # A VDI made for these rows, attached the way they need with the VM off, and gone afterwards.
    param([string[]]$Ids, [string]$Disk, [long]$SizeMiB, [string]$DiskBus)
    Invoke-Rig -Action 'NewDisk' -More @{ Disk = $Disk; SizeMiB = $SizeMiB }
    Invoke-Rig -Action 'Off'
    Invoke-Rig -Action 'AttachDisk' -More @{ Disk = $Disk; Port = 2; DiskBus = $DiskBus }
    Invoke-Rig -Action 'On'
    try {
        Invoke-GuestRows -Ids $Ids -More @{ DataDiskBytes = [uint64]$SizeMiB * $Script:MiB }
    } finally {
        Invoke-Rig -Action 'Off'
        Invoke-Rig -Action 'DetachDisk' -More @{ Port = 2; DiskBus = $DiskBus }
        Invoke-Rig -Action 'DeleteDisk' -More @{ Disk = $Disk }
        Invoke-Rig -Action 'On'
    }
}

function Invoke-Migration {
    param([string]$Disk, [long]$SizeMiB)
    $bytes = [uint64]$SizeMiB * $Script:MiB
    Invoke-Rig -Action 'NewDisk' -More @{ Disk = $Disk; SizeMiB = $SizeMiB }
    Invoke-Rig -Action 'Off'
    Invoke-Rig -Action 'AttachDisk' -More @{ Disk = $Disk; Port = 2 }
    Invoke-Rig -Action 'On'
    Invoke-GuestRows -Ids @('external.os-migration-reboot') -More @{ DataDiskBytes = $bytes }
    Assert-RowPassed -Id 'external.os-migration-reboot'
    Invoke-Rig -Action 'Off'
    Invoke-Rig -Action 'DetachDisk' -More @{ Port = 2 }
    $row = Join-Path $EvidenceDir 'external.os-migration-reboot'
    $proof = Get-Proof -Disk $Disk -Name 'migrated' -Firmware 'efi' -RowDir $row -MarkerPath 'C:\SAKCert\source-marker.txt'
    $independent = Get-Content -LiteralPath (Join-Path $row 'independent.json') -Raw | ConvertFrom-Json
    $proof | Add-Member -NotePropertyName source_marker_present -NotePropertyValue ("$($proof.source_marker)" -ceq "$($independent.source_marker)") -Force
    Add-PmRowHostEvidence -RowDir $row -Values @{ boot = $proof } -ExtraFiles @('migrated.json')
}

function Invoke-BootRepairRow {
    <#
    .SYNOPSIS
        Proves a Windows disk starts, breaks it, proves it no longer starts, repairs it through the
        app from another Windows, and proves it starts again.
    #>
    param([string]$Id, [string]$Disk, [uint64]$Bytes, [string]$Firmware)
    $row = Join-Path $EvidenceDir $Id
    New-Item -ItemType Directory -Force -Path $row | Out-Null
    $hold = Join-Path ([IO.Path]::GetTempPath()) "sakcert-proofs-$PID"
    New-Item -ItemType Directory -Force -Path $hold | Out-Null
    Say "$Id : it starts as it is"
    $before = Get-Proof -Disk $Disk -Name "$Firmware-before" -Firmware $Firmware -RowDir $hold
    Invoke-Rig -Action 'AttachDisk' -More @{ Disk = $Disk; Port = 2 }
    Invoke-Rig -Action 'On'
    Say "$Id : break it"
    Invoke-GuestRows -Ids @($Id) -More @{ Phase = 'break'; DataDiskBytes = $Bytes }
    Invoke-Rig -Action 'Off'
    Invoke-Rig -Action 'DetachDisk' -More @{ Port = 2 }
    Say "$Id : it must not start"
    $broken = Get-Proof -Disk $Disk -Name "$Firmware-broken" -Firmware $Firmware -RowDir $hold -ExpectNoBoot
    Invoke-Rig -Action 'AttachDisk' -More @{ Disk = $Disk; Port = 2 }
    Invoke-Rig -Action 'On'
    Say "$Id : repair it through the app"
    Invoke-GuestRows -Ids @($Id) -More @{ Phase = 'repair'; DataDiskBytes = $Bytes }
    Assert-RowPassed -Id $Id
    Invoke-Rig -Action 'Off'
    Invoke-Rig -Action 'DetachDisk' -More @{ Port = 2 }
    Say "$Id : it starts again"
    $after = Get-Proof -Disk $Disk -Name "$Firmware-after" -Firmware $Firmware -RowDir $hold
    Copy-Item -LiteralPath (Join-Path $hold "$Firmware-before.json"), (Join-Path $hold "$Firmware-broken.json"), (Join-Path $hold "$Firmware-after.json") -Destination $row
    Add-PmRowHostEvidence -RowDir $row -Values @{ boot_before = $before; boot_broken = $broken; boot_after = $after } `
        -ExtraFiles @("$Firmware-before.json", "$Firmware-broken.json", "$Firmware-after.json")
    Remove-Item -LiteralPath $hold -Recurse -Force
}

function Invoke-SystemMbr2Gpt {
    # The row runs inside the VM that boots from the disk it converts. Only the host can then turn
    # on EFI, which is the moment that says whether the conversion left a Windows that starts.
    param([string]$Disk)
    $id = 'external.system-mbr2gpt'
    $name = 'sakcert-mbr2gpt'
    Invoke-Rig -Action 'VmCreate' -More @{ Disk = $Disk; ProofName = $name; Firmware = 'bios' }
    try {
        Invoke-Rig -Action 'Deploy' -More @{ Vm = $name; StageDir = $StageDir }
        Invoke-Rig -Action 'Rows' -More @{ Vm = $name; Rows = @($id) }
        Assert-RowPassed -Id $id
        Invoke-Rig -Action 'Off' -More @{ Vm = $name }
        Invoke-Rig -Action 'VmFirmware' -More @{ ProofName = $name; Firmware = 'efi' }
        $row = Join-Path $EvidenceDir $id
        $output = Join-Path $row 'boot-after-conversion.json'
        Invoke-Rig -Action 'VmFacts' -More @{ ProofName = $name; ProofOutput = $output }
        $facts = Get-Content -LiteralPath $output -Raw | ConvertFrom-Json
        Add-PmRowHostEvidence -RowDir $row -Values @{ boot = $facts } -ExtraFiles @('boot-after-conversion.json')
    } finally {
        Invoke-Rig -Action 'VmRemove' -More @{ ProofName = $name }
    }
}

function Invoke-BuildBiosDisk {
    param([string]$Disk, [long]$SizeMiB, [long]$ScratchMiB)
    Invoke-Rig -Action 'NewDisk' -More @{ Disk = $Disk; SizeMiB = $SizeMiB }
    Invoke-Rig -Action 'NewDisk' -More @{ Disk = "$Disk-scratch"; SizeMiB = $ScratchMiB }
    Invoke-Rig -Action 'Off'
    Invoke-Rig -Action 'AttachDisk' -More @{ Disk = $Disk; Port = 2 }
    Invoke-Rig -Action 'AttachDisk' -More @{ Disk = "$Disk-scratch"; Port = 3 }
    Invoke-Rig -Action 'On'
    $job = Join-Path ([IO.Path]::GetTempPath()) "sakcert-bios-$PID.ps1"
    [IO.File]::WriteAllText($job, "& 'C:\SAKCert\scripts\partition_manager_certification_bios_fixture.ps1' -TargetBytes $([uint64]$SizeMiB * $Script:MiB) -ScratchBytes $([uint64]$ScratchMiB * $Script:MiB)`n")
    Invoke-Rig -Action 'GuestJob' -More @{ JobFile = $job; JobMinutes = 180 }
    Remove-Item -LiteralPath $job
    Invoke-Rig -Action 'Off'
    Invoke-Rig -Action 'DetachDisk' -More @{ Port = 2 }
    Invoke-Rig -Action 'DetachDisk' -More @{ Port = 3 }
    Invoke-Rig -Action 'DeleteDisk' -More @{ Disk = "$Disk-scratch" }
}

$VhdRows = @('vhd.create-format-resize-delete', 'vhd.fat32-to-ntfs', 'vhd.quick-partition', 'vhd.quick-partition-custom',
    'vhd.quick-partition-mbr', 'vhd.adjacent-extend', 'vhd.recovered-partition-restore', 'vhd.empty-style-conversion',
    'vhd.image-clone', 'vhd.image-restore', 'vhd.partition-clone-region')
$DataRows = @('external.file-level-data-recovery', 'external.bitlocker', 'external.bitlocker-mutation', 'external.allocate-free-space',
    'external.partition-move', 'external.cluster-size-change', 'external.primary-logical-conversion',
    'external.volume-serial-number')

if ($PcFile -and @('ssd-device', 'hdd-device', 'data-device') -cnotcontains $Group) { throw "The group $Group runs on the VM rig. Only the device groups run on a physical PC" }
if ($Group -clike '*-device' -and -not $PinFile) { throw "The group $Group needs -PinFile" }
New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null
switch ($Group) {
    'vhd' {
        $wanted = if ($Rows.Count) { $Rows } else { $VhdRows + 'vhd.clear-disk-wipe' }
        $onVhd = @($wanted | Where-Object { $_ -cne 'vhd.clear-disk-wipe' })
        if ($onVhd.Count) { Invoke-GuestRows -Ids $onVhd }
        if ($wanted -ccontains 'vhd.clear-disk-wipe') {
            Invoke-RowsOnThrowawayDisk -Ids @('vhd.clear-disk-wipe') -Disk 'cert-wipe' -SizeMiB 1024 -DiskBus 'Sata'
        }
    }
    'data' {
        $wanted = if ($Rows.Count) { $Rows } else { $DataRows + 'external.dynamic-to-basic' }
        $onVhd = @($wanted | Where-Object { $_ -cne 'external.dynamic-to-basic' })
        if ($onVhd.Count) { Invoke-GuestRows -Ids $onVhd }
        if ($wanted -ccontains 'external.dynamic-to-basic') {
            Invoke-RowsOnThrowawayDisk -Ids @('external.dynamic-to-basic') -Disk 'cert-dynamic' -SizeMiB 1024 -DiskBus 'Sata'
        }
    }
    'boot-uefi' {
        $disk = 'cert-uefi'
        Say 'OS migration, through the app'
        Invoke-Migration -Disk $disk -SizeMiB 327680
        Say 'UEFI boot repair of what it wrote'
        Invoke-BootRepairRow -Id 'external.boot-repair-uefi' -Disk $disk -Bytes ([uint64]327680 * $Script:MiB) -Firmware 'efi'
        Invoke-Rig -Action 'DeleteDisk' -More @{ Disk = $disk }
    }
    'boot-bios' {
        $disk = 'cert-bios'
        Say 'A BIOS install to start from'
        Invoke-BuildBiosDisk -Disk $disk -SizeMiB 102400 -ScratchMiB 40960
        Say 'BIOS boot repair'
        Invoke-BootRepairRow -Id 'external.boot-repair-bios' -Disk $disk -Bytes ([uint64]102400 * $Script:MiB) -Firmware 'bios'
        Say 'System MBR to GPT, on the Windows that disk runs'
        Invoke-SystemMbr2Gpt -Disk $disk
        Invoke-Rig -Action 'DeleteDisk' -More @{ Disk = $disk }
    }
    'ssd-virtual' {
        Invoke-RowsOnThrowawayDisk -Ids $(if ($Rows.Count) { $Rows } else { @('external.ssd-retrim', 'external.ssd-secure-erase', 'external.hardware-wipe') }) `
            -Disk 'cert-ssd' -SizeMiB 8192 -DiskBus 'SataSolidState'
    }
    'hdd-virtual' {
        Invoke-RowsOnThrowawayDisk -Ids $(if ($Rows.Count) { $Rows } else { @('external.hdd-defrag-execution') }) -Disk 'cert-hdd' -SizeMiB 32768 -DiskBus 'Sata'
    }
    'usb-virtual' {
        Invoke-RowsOnThrowawayDisk -Ids $(if ($Rows.Count) { $Rows } else { @('external.usb-removable') }) -Disk 'cert-usb' -SizeMiB 8192 -DiskBus 'Usb'
    }
    'ssd-device' { Invoke-GuestRows -Ids $(if ($Rows.Count) { $Rows } else { @('external.ssd-retrim', 'external.ssd-secure-erase', 'external.hardware-wipe') }) -More @{ PinFile = $PinFile } }
    'hdd-device' { Invoke-GuestRows -Ids $(if ($Rows.Count) { $Rows } else { @('external.usb-removable', 'external.hdd-defrag-execution') }) -More @{ PinFile = $PinFile } }
    'data-device' {
        Invoke-GuestRows -Ids $(if ($Rows.Count) { $Rows } else { @('external.allocate-free-space', 'external.partition-move', 'external.cluster-size-change', 'external.volume-serial-number') }) `
            -More @{ PinFile = $PinFile; DataRowsOnDevice = $true }
    }
}
Say "$Group done"

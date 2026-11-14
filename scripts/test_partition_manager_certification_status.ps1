<#
.SYNOPSIS
    Proves what the computed claim level says for what evidence, and the words it gives the site.

.DESCRIPTION
    Runs the real status, verify and claims scripts over synthetic evidence in a temporary folder,
    against a small matrix of its own. Touches no disk.
#>

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'partition_manager_row_evidence.ps1')
. (Join-Path $PSScriptRoot 'partition_manager_claim_wording.ps1')

$Script:Failures = New-Object System.Collections.Generic.List[string]
$Script:Passes = 0
$Script:Root = Join-Path ([IO.Path]::GetTempPath()) "sak-cert-status-test-$PID"
$Script:Digest = '0' * 64
$Script:Commit = '1' * 40
$Script:ClientHash = '2' * 64
$Script:HelperHash = '3' * 64

function Assert-That {
    param([bool]$Condition, [string]$Name, [string]$Detail = '')
    if ($Condition) { $Script:Passes++ } else { $Script:Failures.Add("$Name $Detail") }
}

function New-RowSpec {
    param([string]$Id, [string[]]$Accepted, [bool]$PhysicalClaim = $false, [hashtable]$Requirement = $null)
    $ranOn = [ordered]@{ accepted = @($Accepted); physical_claim = $PhysicalClaim }
    if ($null -ne $Requirement) { $ranOn.physical_requirement = $Requirement }
    return [ordered]@{
        id = $Id; name = "Row $Id"; ran_on = $ranOn; client = 'unelevated'
        steps = @([ordered]@{ id = 'one'; operation = 'Create'; expect = 'success' })
        checks = @([ordered]@{ contract = 'worked'; path = 'worked'; is_true = $true })
        not_proved = 'Whatever this fixture leaves out.'
    }
}

$Script:MatrixPath = Join-Path $Script:Root 'matrix.json'
$Script:Specs = [ordered]@{
    schema_version = 2
    vhd_scenarios = @((New-RowSpec -Id 'vhd.a' -Accepted @('virtual_disk')))
    external_gates = @(
        (New-RowSpec -Id 'external.boot' -Accepted @('virtual_machine')),
        (New-RowSpec -Id 'external.ssd-secure-erase' -Accepted @('virtual_machine', 'physical') -PhysicalClaim $true -Requirement @{ media_type = @('SSD') }),
        (New-RowSpec -Id 'external.hdd' -Accepted @('virtual_machine', 'physical') -PhysicalClaim $true -Requirement @{ media_type = @('HDD') }),
        (New-RowSpec -Id 'external.defrag' -Accepted @('physical') -PhysicalClaim $true -Requirement @{ media_type = @('HDD') })
    )
}

function Write-Row {
    param([string]$RunDir, [string]$Id, [hashtable]$Media, [string]$Status = 'Passed', [bool]$Worked = $true, [string]$Digest = $Script:Digest)
    $dir = Join-Path $RunDir $Id
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    [IO.File]::WriteAllText((Join-Path $dir 'one.client.json'), (ConvertTo-Json -Depth 8 -InputObject ([ordered]@{
                    tool = 'partition-manager-scenario-step'; scenario_id = $Id; run_id = 'run'; status = 'Passed'; expectation_met = $true
                    execution_engine = 'PartitionManagerController'; route = 'elevated-helper'; helper_connected = $true; code_digest = $Digest
                    client = [ordered]@{ file_name = 'sak_utility.exe'; elevated = $false; sha256 = $Script:ClientHash }
                    execution = [ordered]@{ success = $true; dry_run = $false; cancelled = $false; timed_out = $false }
                })))
    [IO.File]::WriteAllText((Join-Path $dir 'independent.json'), (ConvertTo-Json -InputObject ([ordered]@{ worked = $Worked })))
    $records = @((Get-PmFileRecord -Root $dir -Relative 'independent.json'), (Get-PmFileRecord -Root $dir -Relative 'one.client.json'))
    [IO.File]::WriteAllText((Join-Path $dir 'report.json'), (ConvertTo-Json -Depth 12 -InputObject ([ordered]@{
                    tool = 'partition-manager-row-report'; schema_version = 1; scenario_id = $Id; status = $Status; failure = ''; run_id = 'run'
                    started_utc = '2026-11-04T10:00:00.0000000Z'; completed_utc = '2026-11-04T10:05:00.0000000Z'
                    code = [ordered]@{ digest = $Digest; commit = $Script:Commit; dirty = $false }
                    binaries = [ordered]@{ client_sha256 = $Script:ClientHash; helper_sha256 = $Script:HelperHash; app_sha256 = '4' * 64 }
                    media = $Media
                    steps = @([ordered]@{ id = 'one'; operation = 'Create'; expect = 'success'; report = 'one.client.json'
                            observed = [ordered]@{ client_token_elevated = $false; helper_token_elevated = $true; helper_sha256 = $Script:HelperHash } })
                    independent_file = 'independent.json'; artifacts = $records
                })))
}

function Write-Index {
    param([string]$RunDir, [string[]]$Ids)
    $rows = @($Ids | ForEach-Object {
            [ordered]@{ id = $_; report_sha256 = (Get-FileHash -LiteralPath (Join-Path (Join-Path $RunDir $_) 'report.json') -Algorithm SHA256).Hash.ToLowerInvariant() }
        })
    [IO.File]::WriteAllText((Join-Path $RunDir 'index.json'), (ConvertTo-Json -Depth 6 -InputObject ([ordered]@{
                    tool = 'partition-manager-evidence-index'; schema_version = 1; code_digest = $Script:Digest; code_commit = $Script:Commit
                    completed_utc = '2026-11-04T10:05:00.0000000Z'; rows = $rows
                })))
}

function Get-Status {
    param([string]$RunDir)
    $output = Join-Path $RunDir 'status.json'
    & (Join-Path $PSScriptRoot 'get_partition_manager_certification_status.ps1') -EvidenceRoot $RunDir -CodeDigest $Script:Digest -MatrixPath $Script:MatrixPath -OutputPath $output -Quiet
    return (Get-Content -LiteralPath $output -Raw | ConvertFrom-Json)
}

$virtualDisk = @{ class = 'virtual_disk'; kind = 'VHDX in a VM'; model = 'Msft Virtual Disk'; bus_type = 'File Backed Virtual'; media_type = 'Unspecified'; size_bytes = '1073741824' }
$virtualMachine = @{ class = 'virtual_machine'; kind = 'VDI'; model = 'VBOX HARDDISK'; bus_type = 'SATA'; media_type = 'SSD'; size_bytes = '1073741824' }
$virtualHdd = @{ class = 'virtual_machine'; kind = 'VDI'; model = 'VBOX HARDDISK'; bus_type = 'SATA'; media_type = 'HDD'; size_bytes = '1073741824' }
$nvme = @{ class = 'physical'; kind = 'USB passthrough'; model = 'NS-PCNVMEHDE(-C)'; bus_type = 'USB'; media_type = 'SSD'; size_bytes = '256060514304' }
$seagate = @{ class = 'physical'; kind = 'USB passthrough'; model = 'Seagate Expansion Desk'; bus_type = 'USB'; media_type = 'HDD'; size_bytes = '8001563221504' }
$internalSsd = @{ class = 'physical'; kind = 'NVMe inside a physical PC'; model = 'Fast Drive'; bus_type = 'NVMe'; media_type = 'SSD'; size_bytes = '512110190592' }
$internalHdd = @{ class = 'physical'; kind = 'SATA inside a physical PC'; model = 'Slow Drive'; bus_type = 'SATA'; media_type = 'HDD'; size_bytes = '1000204886016' }

New-Item -ItemType Directory -Force -Path $Script:Root | Out-Null
[IO.File]::WriteAllText($Script:MatrixPath, ($Script:Specs | ConvertTo-Json -Depth 12))
$version = (Get-Content -LiteralPath (Join-Path (Split-Path -Parent $PSScriptRoot) 'VERSION') -Raw).Trim()
# The repository's Apple matrix: HFS+ and APFS capabilities certified 2026-06-14 to 2026-08-31.
$apple = "Separately, on earlier code (June to August 2026), S.A.K.'s HFS+ and APFS write engine was validated with Apple's own tools."
try {
    # No evidence at all, which is what a clean clone has before the final run.
    $none = Join-Path $Script:Root 'none'
    New-Item -ItemType Directory -Force -Path $none | Out-Null
    $status = Get-Status -RunDir $none
    Assert-That ($status.claim_level -ceq 'CodeCompleteOnly') 'No evidence is code complete only' $status.claim_level
    Assert-That ($status.vhd_data_disk.passed -eq 0 -and $status.external_gates.passed -eq 0) 'No evidence counts nothing'
    Assert-That ($status.site_wording -match 'have not been validated') 'No evidence says so' $status.site_wording

    # A report sitting in the folder that the index does not name is not selected.
    $unlisted = Join-Path $Script:Root 'unlisted'
    Write-Row -RunDir $unlisted -Id 'vhd.a' -Media $virtualDisk
    Assert-That ((Get-Status -RunDir $unlisted).claim_level -ceq 'CodeCompleteOnly') 'A row with no index is not selected'

    # Virtual disks only.
    $vhd = Join-Path $Script:Root 'vhd'
    Write-Row -RunDir $vhd -Id 'vhd.a' -Media $virtualDisk
    Write-Index -RunDir $vhd -Ids @('vhd.a')
    $status = Get-Status -RunDir $vhd
    Assert-That ($status.claim_level -ceq 'VhdDataDiskCertified') 'Every VHD row is the VHD level' $status.claim_level
    Assert-That ($status.site_wording -ceq "On the v$version code, one disk scenarios ran through the app's own Partition Manager controller on disposable virtual disks in a Windows 11 VM. Nothing was tested on real hardware. $apple Back up before changing a disk.") 'The VHD words' $status.site_wording
    $row = @($status.rows | Where-Object { $_.id -ceq 'vhd.a' })[0]
    Assert-That ($row.ran_on.class -ceq 'virtual_disk' -and $row.ran_on.bus_type -ceq 'File Backed Virtual') 'A row says what it ran on'

    # Every row, on virtual media: this must not read as hardware.
    $vm = Join-Path $Script:Root 'vm'
    Write-Row -RunDir $vm -Id 'vhd.a' -Media $virtualDisk
    Write-Row -RunDir $vm -Id 'external.boot' -Media $virtualMachine
    Write-Row -RunDir $vm -Id 'external.ssd-secure-erase' -Media $virtualMachine
    Write-Row -RunDir $vm -Id 'external.hdd' -Media $virtualHdd
    Write-Index -RunDir $vm -Ids @('vhd.a', 'external.boot', 'external.ssd-secure-erase', 'external.hdd')
    $status = Get-Status -RunDir $vm
    Assert-That ($status.claim_level -ceq 'VirtualMachinesValidated') 'VM-only evidence is the VM level' $status.claim_level
    Assert-That ($status.claim_level -cnotmatch 'Hardware') 'VM-only evidence never reads as hardware'
    Assert-That ($status.site_wording -ceq "On the v$version code, Partition Manager operations, including boot repair, MBR-to-GPT conversion and OS migration, ran through the app on disposable virtual machines and virtual disks. Real SSD, NVMe, USB and hard-disk behaviour was not tested. $apple Back up before changing a disk.") 'The VM words' $status.site_wording
    Assert-That (@($status.virtual_where_a_device_matters).Count -eq 3) 'It lists which rows need a real device'
    Assert-That (@($status.external_gates.incomplete_ids).Count -eq 1 -and @($status.external_gates.incomplete_ids)[0] -ceq 'external.defrag') 'A row only a device can run is still listed as not run' "$($status.external_gates.incomplete_ids)"

    # That row on a virtual disk is refused, and does not hold back or raise the level.
    $defragInVm = Join-Path $Script:Root 'defrag-in-vm'
    Write-Row -RunDir $defragInVm -Id 'vhd.a' -Media $virtualDisk
    Write-Row -RunDir $defragInVm -Id 'external.boot' -Media $virtualMachine
    Write-Row -RunDir $defragInVm -Id 'external.ssd-secure-erase' -Media $virtualMachine
    Write-Row -RunDir $defragInVm -Id 'external.hdd' -Media $virtualHdd
    Write-Row -RunDir $defragInVm -Id 'external.defrag' -Media $virtualHdd
    Write-Index -RunDir $defragInVm -Ids @('vhd.a', 'external.boot', 'external.ssd-secure-erase', 'external.hdd', 'external.defrag')
    $status = Get-Status -RunDir $defragInVm
    Assert-That ($status.claim_level -ceq 'VirtualMachinesValidated') 'A device-only row on a VM stays the VM level' $status.claim_level
    Assert-That (@($status.external_gates.incomplete_ids) -ccontains 'external.defrag') 'and it is not accepted'

    # A row a VM could run, missing, is not the VM level, whatever the device-only row did.
    $vmGap = Join-Path $Script:Root 'vm-gap'
    Write-Row -RunDir $vmGap -Id 'vhd.a' -Media $virtualDisk
    Write-Row -RunDir $vmGap -Id 'external.ssd-secure-erase' -Media $virtualMachine
    Write-Row -RunDir $vmGap -Id 'external.hdd' -Media $virtualHdd
    Write-Row -RunDir $vmGap -Id 'external.defrag' -Media $seagate
    Write-Index -RunDir $vmGap -Ids @('vhd.a', 'external.ssd-secure-erase', 'external.hdd', 'external.defrag')
    Assert-That ((Get-Status -RunDir $vmGap).claim_level -ceq 'VhdDataDiskCertified') 'A row a VM could run, missing, stays the VHD level'

    # One device of the two: still the VM level, with that device named and no other.
    $some = Join-Path $Script:Root 'some'
    Write-Row -RunDir $some -Id 'vhd.a' -Media $virtualDisk
    Write-Row -RunDir $some -Id 'external.boot' -Media $virtualMachine
    Write-Row -RunDir $some -Id 'external.ssd-secure-erase' -Media $nvme
    Write-Row -RunDir $some -Id 'external.hdd' -Media $virtualHdd
    Write-Index -RunDir $some -Ids @('vhd.a', 'external.boot', 'external.ssd-secure-erase', 'external.hdd')
    $status = Get-Status -RunDir $some
    Assert-That ($status.claim_level -ceq 'VirtualMachinesValidated') 'One device of two is still the VM level' $status.claim_level
    Assert-That ($status.site_wording -match 'NS-PCNVMEHDE\(-C\) 256 GB USB SSD') 'It names the device that ran' $status.site_wording
    Assert-That ($status.site_wording -notmatch 'Seagate') 'It names no device that did not run' $status.site_wording
    Assert-That ($status.site_wording -notmatch 'hard-disk behaviour was not tested') 'It does not say nothing real ran' $status.site_wording

    # Both devices.
    $all = Join-Path $Script:Root 'all'
    Write-Row -RunDir $all -Id 'vhd.a' -Media $virtualDisk
    Write-Row -RunDir $all -Id 'external.boot' -Media $virtualMachine
    Write-Row -RunDir $all -Id 'external.ssd-secure-erase' -Media $nvme
    Write-Row -RunDir $all -Id 'external.hdd' -Media $seagate
    Write-Row -RunDir $all -Id 'external.defrag' -Media $seagate
    Write-Index -RunDir $all -Ids @('vhd.a', 'external.boot', 'external.ssd-secure-erase', 'external.hdd', 'external.defrag')
    $status = Get-Status -RunDir $all
    Assert-That ($status.claim_level -ceq 'NamedDevicesValidated') 'Each device row on its device is the named level' $status.claim_level
    Assert-That ($status.site_wording -ceq "On the v$version code, Partition Manager operations, including boot repair, MBR-to-GPT conversion and OS migration, ran through the app on disposable virtual machines and virtual disks. Some operations were also tested on one NS-PCNVMEHDE(-C) 256 GB USB SSD and one Seagate Expansion Desk 8 TB USB hard drive. Internal SATA and NVMe drives were not tested, and results on other devices may differ. SSD erase is TRIM plus overwrite, not an ATA or NVMe sanitize. $apple Back up before changing a disk.") 'The device words' $status.site_wording
    Assert-That ($status.site_wording -notmatch '(?i)certif') 'The words never say certified'
    Assert-That ($status.site_wording -notmatch 'Apple-certified') 'The words never say Apple-certified'

    # A drive inside a PC is said to have been tested, and what was not is still said.
    $nvmeInside = Join-Path $Script:Root 'nvme-inside'
    Write-Row -RunDir $nvmeInside -Id 'vhd.a' -Media $virtualDisk
    Write-Row -RunDir $nvmeInside -Id 'external.boot' -Media $virtualMachine
    Write-Row -RunDir $nvmeInside -Id 'external.ssd-secure-erase' -Media $internalSsd
    Write-Row -RunDir $nvmeInside -Id 'external.hdd' -Media $seagate
    Write-Index -RunDir $nvmeInside -Ids @('vhd.a', 'external.boot', 'external.ssd-secure-erase', 'external.hdd')
    $status = Get-Status -RunDir $nvmeInside
    Assert-That ($status.site_wording -cmatch 'one Fast Drive 512 GB NVMe SSD') 'It names the drive inside a PC' $status.site_wording
    Assert-That ($status.site_wording -cmatch 'Internal SATA drives were not tested') 'It says SATA was not tested' $status.site_wording
    Assert-That ($status.site_wording -cnotmatch 'NVMe drives were not tested') 'It does not say NVMe was not tested' $status.site_wording

    $bothInside = Join-Path $Script:Root 'both-inside'
    Write-Row -RunDir $bothInside -Id 'vhd.a' -Media $virtualDisk
    Write-Row -RunDir $bothInside -Id 'external.boot' -Media $virtualMachine
    Write-Row -RunDir $bothInside -Id 'external.ssd-secure-erase' -Media $internalSsd
    Write-Row -RunDir $bothInside -Id 'external.hdd' -Media $internalHdd
    Write-Row -RunDir $bothInside -Id 'external.defrag' -Media $internalHdd
    Write-Index -RunDir $bothInside -Ids @('vhd.a', 'external.boot', 'external.ssd-secure-erase', 'external.hdd', 'external.defrag')
    $status = Get-Status -RunDir $bothInside
    Assert-That ($status.site_wording -cnotmatch 'were not tested') 'With both tested, it does not say either was not' $status.site_wording
    Assert-That ($status.site_wording -cmatch 'Results on other devices may differ\.') 'It still says other devices may differ' $status.site_wording

    # The Apple sentence: its months and file systems come from the matrix, and a certified
    # capability with no date stops the wording rather than guess one (2026-11-11).
    $hfsJune = [pscustomobject]@{ capabilities = @(
            [pscustomobject]@{ id = 'H-a'; filesystem = 'HFS+'; status = 'certified'; certified_on = '2026-06-14' },
            [pscustomobject]@{ id = 'H-b'; filesystem = 'HFS+'; status = 'certified'; certified_on = '2026-06-27' },
            [pscustomobject]@{ id = 'A-a'; filesystem = 'APFS'; status = 'design_decision' }) }
    $words = Get-PmAppleSentence -AppleMatrix $hfsJune
    Assert-That ($words -ceq "Separately, on earlier code (June 2026), S.A.K.'s HFS+ write engine was validated with Apple's own tools.") 'One month, one file system' $words
    $acrossYears = [pscustomobject]@{ capabilities = @(
            [pscustomobject]@{ id = 'A-a'; filesystem = 'APFS'; status = 'certified'; certified_on = '2026-01-05' },
            [pscustomobject]@{ id = 'H-a'; filesystem = 'HFS+'; status = 'certified'; certified_on = '2025-12-30' }) }
    $words = Get-PmAppleSentence -AppleMatrix $acrossYears
    Assert-That ($words -ceq "Separately, on earlier code (December 2025 to January 2026), S.A.K.'s HFS+ and APFS write engine was validated with Apple's own tools.") 'Across a year' $words
    Assert-That (@(Get-PmAppleSentence -AppleMatrix ([pscustomobject]@{ capabilities = @($hfsJune.capabilities[2]) })).Count -eq 0) 'Nothing certified says nothing'
    $threw = $false
    try { Get-PmAppleSentence -AppleMatrix ([pscustomobject]@{ capabilities = @([pscustomobject]@{ id = 'H-a'; filesystem = 'HFS+'; status = 'certified' }) }) | Out-Null } catch { $threw = "$($_.Exception.Message)" -match 'no certified_on date' }
    Assert-That $threw 'A certified capability with no date stops the wording'

    # A claim level with no words stops, rather than saying something.
    $threw = $false
    try { Get-PmSiteWording -ClaimLevel 'HardwareCertified' -Version $version | Out-Null } catch { $threw = "$($_.Exception.Message)" -match 'no words' }
    Assert-That $threw 'A level with no words stops'

    # A device of the wrong kind stays out.
    $wrong = Join-Path $Script:Root 'wrong'
    Write-Row -RunDir $wrong -Id 'vhd.a' -Media $virtualDisk
    Write-Row -RunDir $wrong -Id 'external.boot' -Media $virtualMachine
    Write-Row -RunDir $wrong -Id 'external.ssd-secure-erase' -Media $seagate
    Write-Row -RunDir $wrong -Id 'external.hdd' -Media $seagate
    Write-Index -RunDir $wrong -Ids @('vhd.a', 'external.boot', 'external.ssd-secure-erase', 'external.hdd')
    $status = Get-Status -RunDir $wrong
    Assert-That ($status.claim_level -ceq 'VhdDataDiskCertified') 'A hard drive cannot pass for an SSD' $status.claim_level
    Assert-That (@($status.external_gates.incomplete_ids) -ccontains 'external.ssd-secure-erase') 'It says which row'

    # A device whose only row was refused is not named: nothing was shown to work on it.
    $refused = Join-Path $Script:Root 'refused'
    Write-Row -RunDir $refused -Id 'vhd.a' -Media $virtualDisk
    Write-Row -RunDir $refused -Id 'external.boot' -Media $virtualMachine
    Write-Row -RunDir $refused -Id 'external.hdd' -Media $nvme
    Write-Index -RunDir $refused -Ids @('vhd.a', 'external.boot', 'external.hdd')
    $status = Get-Status -RunDir $refused
    Assert-That ($status.site_wording -notmatch 'NS-PCNVMEHDE') 'A refused row names no device' $status.site_wording

    # A row that failed, a row that is stale, a report changed after selection.
    $failed = Join-Path $Script:Root 'failed'
    Write-Row -RunDir $failed -Id 'vhd.a' -Media $virtualDisk
    Write-Row -RunDir $failed -Id 'external.boot' -Media $virtualMachine -Status 'Failed'
    Write-Index -RunDir $failed -Ids @('vhd.a', 'external.boot')
    Assert-That ((Get-Status -RunDir $failed).claim_level -ceq 'FailedEvidence') 'A failed row fails the level'

    $stale = Join-Path $Script:Root 'stale'
    Write-Row -RunDir $stale -Id 'vhd.a' -Media $virtualDisk -Digest ('9' * 64)
    Write-Index -RunDir $stale -Ids @('vhd.a')
    $status = Get-Status -RunDir $stale
    Assert-That ($status.claim_level -ceq 'CodeCompleteOnly') 'Evidence for other code does not count' $status.claim_level
    Assert-That ("$($status.incomplete_reasons.'vhd.a')" -match 'for other code') 'It says why'

    $false1 = Join-Path $Script:Root 'false'
    Write-Row -RunDir $false1 -Id 'vhd.a' -Media $virtualDisk -Worked $false
    Write-Index -RunDir $false1 -Ids @('vhd.a')
    Assert-That ((Get-Status -RunDir $false1).claim_level -ceq 'CodeCompleteOnly') 'A false readback does not count, though no field is blank'

    $changed = Join-Path $Script:Root 'changed'
    Write-Row -RunDir $changed -Id 'vhd.a' -Media $virtualDisk
    Write-Index -RunDir $changed -Ids @('vhd.a')
    Add-Content -LiteralPath (Join-Path $changed 'vhd.a\report.json') -Value ' '
    $status = Get-Status -RunDir $changed
    Assert-That ($status.claim_level -ceq 'CodeCompleteOnly') 'A report changed after selection does not count' $status.claim_level
    Assert-That ("$($status.incomplete_reasons.'vhd.a')" -match 'changed after this run was selected') 'It says why'

    # Selecting a run: what it takes, and what it refuses.
    function Invoke-Select {
        param([string]$RunDir, [string[]]$DenyNames = @())
        $target = Join-Path $Script:Root "selected-$([guid]::NewGuid().ToString('N'))"
        try {
            & (Join-Path $PSScriptRoot 'select_partition_manager_evidence.ps1') -RunDir $RunDir -OutputRoot $target -CodeDigest $Script:Digest -MatrixPath $Script:MatrixPath -DenyNames $DenyNames | Out-Null
            return [pscustomobject]@{ error = ''; root = $target }
        } catch { return [pscustomobject]@{ error = "$($_.Exception.Message)"; root = $target } }
    }
    $noDevice = Join-Path $Script:Root 'select-no-device'
    Write-Row -RunDir $noDevice -Id 'vhd.a' -Media $virtualDisk
    Write-Row -RunDir $noDevice -Id 'external.boot' -Media $virtualMachine
    Write-Row -RunDir $noDevice -Id 'external.ssd-secure-erase' -Media $virtualMachine
    Write-Row -RunDir $noDevice -Id 'external.hdd' -Media $virtualHdd
    $picked = Invoke-Select -RunDir $noDevice
    Assert-That ($picked.error -ceq '') 'A run with no device can be selected, without the row only a device can run' $picked.error
    if ($picked.error -ceq '') {
        $status = Get-Status -RunDir $picked.root
        Assert-That ($status.claim_level -ceq 'VirtualMachinesValidated') 'and selecting it gives the VM level' $status.claim_level
        Assert-That (Test-Path -LiteralPath (Join-Path $picked.root 'vhd.a\report.json')) 'and it copies the rows'
        $index = Get-Content -LiteralPath (Join-Path $picked.root 'index.json') -Raw | ConvertFrom-Json
        Assert-That (@($index.rows | Where-Object { $_.id -ceq 'external.defrag' }).Count -eq 0) 'and the index does not name that row'
    }
    $missing = Join-Path $Script:Root 'select-missing'
    Write-Row -RunDir $missing -Id 'vhd.a' -Media $virtualDisk
    Write-Row -RunDir $missing -Id 'external.ssd-secure-erase' -Media $virtualMachine
    Write-Row -RunDir $missing -Id 'external.hdd' -Media $virtualHdd
    Assert-That ((Invoke-Select -RunDir $missing).error -match 'has no evidence for external\.boot') 'A row a VM could run, missing, stops selection'

    $doesNotHold = Join-Path $Script:Root 'select-does-not-hold'
    Write-Row -RunDir $doesNotHold -Id 'vhd.a' -Media $virtualDisk -Worked $false
    Write-Row -RunDir $doesNotHold -Id 'external.boot' -Media $virtualMachine
    Write-Row -RunDir $doesNotHold -Id 'external.ssd-secure-erase' -Media $virtualMachine
    Write-Row -RunDir $doesNotHold -Id 'external.hdd' -Media $virtualHdd
    Assert-That ((Invoke-Select -RunDir $doesNotHold).error -match 'vhd\.a does not hold') 'A row that does not hold stops selection'

    Assert-That ((Invoke-Select -RunDir $noDevice -DenyNames @('run')).error -match 'holds a private name') 'A private name in the evidence stops selection'
    $otherCommit = Join-Path $Script:Root 'select-other-commit'
    Write-Row -RunDir $otherCommit -Id 'vhd.a' -Media $virtualDisk
    Write-Row -RunDir $otherCommit -Id 'external.boot' -Media $virtualMachine
    Write-Row -RunDir $otherCommit -Id 'external.ssd-secure-erase' -Media $virtualMachine
    Write-Row -RunDir $otherCommit -Id 'external.hdd' -Media $virtualHdd
    $saved = $Script:Commit
    $Script:Commit = '5' * 40
    Write-Row -RunDir $otherCommit -Id 'external.boot' -Media $virtualMachine
    $Script:Commit = $saved
    Assert-That ((Invoke-Select -RunDir $otherCommit).error -match 'more than one commit') 'Rows from two commits stop selection'

    $unknownIndex = Join-Path $Script:Root 'unknown'
    Write-Row -RunDir $unknownIndex -Id 'vhd.a' -Media $virtualDisk
    Write-Index -RunDir $unknownIndex -Ids @('vhd.a')
    $index = Get-Content -LiteralPath (Join-Path $unknownIndex 'index.json') -Raw | ConvertFrom-Json
    $index.tool = 'partition-manager-external-evidence-report'
    [IO.File]::WriteAllText((Join-Path $unknownIndex 'index.json'), ($index | ConvertTo-Json -Depth 6))
    Assert-That ((Get-Status -RunDir $unknownIndex).claim_level -ceq 'CodeCompleteOnly') 'An index of another kind selects nothing'
} finally {
    Remove-Item -LiteralPath $Script:Root -Recurse -Force -ErrorAction SilentlyContinue
}

if ($Script:Failures.Count -gt 0) {
    $Script:Failures | ForEach-Object { Write-Host "FAIL $_" }
    throw "$($Script:Failures.Count) status check(s) failed, $($Script:Passes) held"
}
Write-Host "Partition Manager certification status: $($Script:Passes) checks held."

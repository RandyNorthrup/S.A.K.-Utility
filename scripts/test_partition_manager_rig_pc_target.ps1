<#
.SYNOPSIS
    Proves how the certification rig reaches a physical PC, and what it refuses, with no PC.

.DESCRIPTION
    Runs the real rig and groups scripts with stand-ins for ssh and scp first on the PATH, which record
    what they were asked and answer from a fixture. Nothing reaches a network or a disk, and the only
    thing read from this machine is its id, to prove the rig refuses to name it. No row runs here: the
    rows' own machine check is run once, and must stop on this machine before it reads a disk.
#>

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$Script:Rig = Join-Path $PSScriptRoot 'run_partition_manager_certification_rig.ps1'
$Script:Groups = Join-Path $PSScriptRoot 'run_partition_manager_certification_groups.ps1'
$Script:Root = Join-Path ([IO.Path]::GetTempPath()) "sak-rig-pc-test-$PID"
$Script:Failures = New-Object System.Collections.Generic.List[string]
$Script:Passes = 0
$Script:Calls = Join-Path $Script:Root 'calls.log'
$Script:DiskFile = Join-Path $Script:Root 'disks.json'

function Assert-That {
    param([bool]$Condition, [string]$Name, [string]$Detail = '')
    if ($Condition) { $Script:Passes++ } else { $Script:Failures.Add("$Name $Detail") }
}

function Write-StandIns {
    # A .cmd earlier on the PATH wins over ssh.exe. It hands every word to one script in this process's PowerShell.
    $bin = Join-Path $Script:Root 'bin'
    New-Item -ItemType Directory -Force -Path $bin | Out-Null
    $hostPath = (Get-Process -Id $PID).Path
    foreach ($tool in 'ssh', 'scp') {
        [IO.File]::WriteAllText((Join-Path $bin "$tool.cmd"), "@echo off`r`n`"$hostPath`" -NoProfile -NonInteractive -File `"%~dp0standin.ps1`" $tool %*`r`nexit /b %ERRORLEVEL%`r`n")
    }
    [IO.File]::WriteAllText((Join-Path $bin 'standin.ps1'), @'
$tool = $args[0]
$words = @($args | Select-Object -Skip 1)
$decoded = ''
$index = [Array]::IndexOf($words, '-EncodedCommand')
$command = ($words | Where-Object { "$_" -match 'EncodedCommand' }) -join ' '
if ($command -match 'EncodedCommand\s+(\S+)') { $decoded = [Text.Encoding]::Unicode.GetString([Convert]::FromBase64String($Matches[1])) }
elseif ($index -ge 0) { $decoded = [Text.Encoding]::Unicode.GetString([Convert]::FromBase64String($words[$index + 1])) }
Add-Content -LiteralPath $env:SAK_TEST_CALLS -Value "$tool $($words -join ' ') :: $decoded"
if ($env:SAK_TEST_FAIL -eq '1') { [Console]::Error.WriteLine('stand-in refuses'); exit 7 }
if ($tool -eq 'scp' -and $env:SAK_TEST_ZIP -and "$($words[-1])" -notmatch ':C:/') { Copy-Item -LiteralPath $env:SAK_TEST_ZIP -Destination (Join-Path $words[-1] ((Split-Path -Leaf "$($words[-2])"))) }
if ($tool -eq 'ssh') {
    if ($decoded -match 'quser') {
        if ($env:SAK_TEST_NO_CONSOLE -eq '1') { 'No User exists for *' } else { ' technician            console             1  Active      none' }
    } elseif ($decoded -match 'done\.txt.*DONE') { 'DONE' }
    elseif ($decoded -match "Get-Content -LiteralPath '[^']*\.log'") { if ($env:SAK_TEST_JOB_LOG) { $env:SAK_TEST_JOB_LOG -split '\|' } else { 'exit 0' } }
    elseif ($decoded -match 'ConvertTo-Json') { Get-Content -LiteralPath $env:SAK_TEST_DISKS -Raw }
}
exit 0
'@)
    return $bin
}

function Invoke-Script {
    param([string]$Path, [string[]]$Arguments, [hashtable]$Environment = @{})
    if (Test-Path -LiteralPath $Script:Calls) { Remove-Item -LiteralPath $Script:Calls }
    Set-Content -LiteralPath $Script:Calls -Value '' -NoNewline
    $saved = $env:PATH
    $env:PATH = "$Script:Bin;$saved"
    $env:SAK_TEST_CALLS = $Script:Calls
    $env:SAK_TEST_DISKS = $Script:DiskFile
    foreach ($key in $Environment.Keys) { Set-Item -Path "env:$key" -Value $Environment[$key] }
    try {
        $output = & (Get-Process -Id $PID).Path -NoProfile -NonInteractive -File $Path @Arguments 2>&1
        $code = $LASTEXITCODE
    } finally {
        $env:PATH = $saved
        foreach ($key in $Environment.Keys) { Remove-Item -Path "env:$key" }
    }
    return [pscustomobject]@{ code = $code; text = (@($output | ForEach-Object { "$_" }) -join "`n"); calls = [IO.File]::ReadAllText($Script:Calls) }
}

function New-PcFile {
    param([string]$Name, [hashtable]$Fields)
    $path = Join-Path $Script:Root "$Name.json"
    [IO.File]::WriteAllText($path, ($Fields | ConvertTo-Json))
    return $path
}

try {
    New-Item -ItemType Directory -Force -Path $Script:Root | Out-Null
    $Script:Bin = Write-StandIns
    $uuid = '4C4C4544-0044-3510-8052-B4C04F4A3132'
    $good = @{ host = 'Technician@10.1.2.3'; key = 'C:\keys\pc-a'; machine_uuid = $uuid; confirmation = "DISPOSABLE Dell Inc. OptiPlex 7040 $uuid"; port = '2222'; jump = 'user@mac' }
    $pc = New-PcFile -Name 'good' -Fields $good

    # It reaches the PC it was told to, by the key it was told to, and never the VM's Mac.
    $on = Invoke-Script -Path $Script:Rig -Arguments @('-Action', 'On', '-PcFile', $pc, '-WaitMinutes', '0')
    Assert-That ($on.code -eq 0 -and $on.text -match 'signed in: Technician@10\.1\.2\.3') 'On finds someone signed in' $on.text
    Assert-That ($on.calls -cmatch 'ssh -n -i C:\\keys\\pc-a ') 'ssh uses the named key' $on.calls
    Assert-That ($on.calls -cmatch '-J user@mac') 'ssh goes through the named jump host' $on.calls
    Assert-That ($on.calls -cmatch '-p 2222 Technician@10\.1\.2\.3') 'ssh uses the named port and host' $on.calls
    Assert-That ($on.calls -cmatch '-o BatchMode=yes') 'ssh cannot stop to ask for a password' $on.calls
    Assert-That ($on.calls -cmatch '-o StrictHostKeyChecking=accept-new') 'A PC host key is trusted once' $on.calls
    Assert-That ($on.calls -cmatch 'UserKnownHostsFile=\S*good\.json\.known_hosts') 'and kept beside the PC file, not in this user''s own list' $on.calls

    # Nobody signed in stops it: the elevated task would quietly not run.
    $alone = Invoke-Script -Path $Script:Rig -Arguments @('-Action', 'On', '-PcFile', $pc, '-WaitMinutes', '0') -Environment @{ SAK_TEST_NO_CONSOLE = '1' }
    Assert-That ($alone.code -ne 0 -and $alone.text -match 'no one signed in') 'A PC with nobody signed in stops it' $alone.text

    # The VM groups need what only the VM rig has.
    $alone = Invoke-Script -Path $Script:Groups -Arguments @('-Group', 'vhd', '-EvidenceDir', (Join-Path $Script:Root 'ev'), '-PcFile', $pc)
    Assert-That ($alone.code -ne 0 -and $alone.text -match 'runs on the VM rig') 'A VM group stops on a physical PC' $alone.text
    Assert-That ($alone.calls -ceq '') 'and reaches nothing' $alone.calls
    foreach ($group in 'data', 'boot-uefi', 'boot-bios', 'ssd-virtual', 'usb-virtual') {
        $stopped = Invoke-Script -Path $Script:Groups -Arguments @('-Group', $group, '-EvidenceDir', (Join-Path $Script:Root 'ev'), '-PcFile', $pc)
        Assert-That ($stopped.code -ne 0 -and $stopped.text -match 'runs on the VM rig' -and $stopped.calls -ceq '') "The group $group stops on a physical PC" $stopped.text
    }
    $noPins = Invoke-Script -Path $Script:Groups -Arguments @('-Group', 'ssd-device', '-EvidenceDir', (Join-Path $Script:Root 'ev'), '-PcFile', $pc)
    Assert-That ($noPins.code -ne 0 -and $noPins.text -match 'needs -PinFile' -and $noPins.calls -ceq '') 'A device group with no pins stops' $noPins.text

    # What only a VM has.
    foreach ($action in 'Off', 'NewDisk', 'AttachDisk', 'DetachDisk', 'DeleteDisk', 'BootProof', 'Snapshot', 'Restore', 'VmCreate', 'UsbOn', 'RootWrapper') {
        $refused = Invoke-Script -Path $Script:Rig -Arguments @('-Action', $action, '-PcFile', $pc, '-Disk', 'x', '-SizeMiB', '1')
        Assert-That ($refused.code -ne 0 -and $refused.text -match 'needs a VM' -and $refused.calls -ceq '') "$action stops on a physical PC" $refused.text
    }

    # A PC file that cannot be trusted stops before anything is reached.
    $here = "$((Get-CimInstance -ClassName Win32_ComputerSystemProduct).UUID)".Trim()
    $thisMachine = New-PcFile -Name 'here' -Fields @{ host = $good.host; key = $good.key; machine_uuid = $here.ToLowerInvariant(); confirmation = $good.confirmation }
    $refused = Invoke-Script -Path $Script:Rig -Arguments @('-Action', 'On', '-PcFile', $thisMachine)
    Assert-That ($refused.code -ne 0 -and $refused.text -match 'names the machine this is running on' -and $refused.calls -ceq '') 'A PC file naming this machine stops' $refused.text
    foreach ($field in 'host', 'key', 'machine_uuid', 'confirmation') {
        $fields = @{ host = $good.host; key = $good.key; machine_uuid = $good.machine_uuid; confirmation = $good.confirmation }
        $fields.Remove($field)
        $missing = Invoke-Script -Path $Script:Rig -Arguments @('-Action', 'On', '-PcFile', (New-PcFile -Name "no-$field" -Fields $fields))
        Assert-That ($missing.code -ne 0 -and $missing.text -match "has no $field" -and $missing.calls -ceq '') "A PC file with no $field stops" $missing.text
    }
    $prepare = Invoke-Script -Path $Script:Rig -Arguments @('-Action', 'PcPrepare')
    Assert-That ($prepare.code -ne 0 -and $prepare.text -match 'needs -PcFile') 'PcPrepare with no PC stops' $prepare.text

    # A failure on the PC is a failure here.
    $failed = Invoke-Script -Path $Script:Rig -Arguments @('-Action', 'GuestJob', '-PcFile', $pc, '-JobFile', $Script:Calls) -Environment @{ SAK_TEST_FAIL = '1' }
    Assert-That ($failed.code -ne 0 -and $failed.text -match 'stand-in refuses|failed with exit code') 'A refused command stops the rig' $failed.text

    $refusedSsh = Invoke-Script -Path $Script:Rig -Arguments @('-Action', 'WritePins', '-PcFile', $pc, '-PinFile', (Join-Path $Script:Root 'never.json'), '-PinSpec', 'hdd=Other|1') -Environment @{ SAK_TEST_FAIL = '1' }
    Assert-That ($refusedSsh.code -ne 0 -and $refusedSsh.text -match 'failed with exit code 7') 'A refused ssh command says so' $refusedSsh.text

    # A job goes there and back through scp and ssh, with no Mac and no VirtualBox involved.
    $jobFile = Join-Path $Script:Root 'job.ps1'
    [IO.File]::WriteAllText($jobFile, "'job'`n")
    $job = Invoke-Script -Path $Script:Rig -Arguments @('-Action', 'GuestJob', '-PcFile', $pc, '-JobFile', $jobFile, '-JobMinutes', '1')
    Assert-That ($job.code -eq 0) 'A job runs' $job.text
    Assert-That ($job.calls -cmatch 'scp -q -i C:\\keys\\pc-a .* -P 2222 .*job\.ps1 .*Technician@10\.1\.2\.3:C:/SAKCert/jobs/') 'scp sends the job with the capital -P port to a forward-slash folder' $job.calls
    Assert-That ($job.calls -cmatch 'schtasks\.exe /Run /TN SAKCaptureElevated') 'The job runs in the elevated task, not this sign-in' $job.calls
    Assert-That ($job.calls -cnotmatch 'VBoxManage|guestcontrol') 'No VirtualBox is involved' $job.calls

    # A job that failed by its log stops the step: it threw, it ran past its time, it exited nonzero,
    # or it left no exit line. The BIOS install of 2026-11-08 failed this way and the run went on.
    foreach ($case in @(
            @{ log = 'start|JOB STOPPED: DISM capture failed with exit code 21 at 45'; minutes = '1'; want = 'JOB STOPPED: DISM capture failed' },
            @{ log = 'something|exit 21'; minutes = '1'; want = 'it exited 21' },
            @{ log = 'output with no end'; minutes = '1'; want = 'it left no exit line' },
            @{ log = 'exit 0'; minutes = '0'; want = 'NOT DONE after 0 minutes' })) {
        $bad = Invoke-Script -Path $Script:Rig -Arguments @('-Action', 'GuestJob', '-PcFile', $pc, '-JobFile', $jobFile, '-JobMinutes', $case.minutes) -Environment @{ SAK_TEST_JOB_LOG = $case.log }
        Assert-That ($bad.code -ne 0 -and $bad.text -match ('failed: ' + [regex]::Escape($case.want))) "A guest job whose log says '$($case.want)' fails its step" $bad.text
    }
    $good = Invoke-Script -Path $Script:Rig -Arguments @('-Action', 'GuestJob', '-PcFile', $pc, '-JobFile', $jobFile, '-JobMinutes', '1') -Environment @{ SAK_TEST_JOB_LOG = 'all fine|exit 0' }
    Assert-That ($good.code -eq 0) 'A guest job that exited 0 passes' $good.text
    $rigText = Get-Content -LiteralPath $Script:Rig -Raw
    $factsAt = $rigText.IndexOf('function Get-RigBootFacts')
    Assert-That ($factsAt -ge 0 -and $rigText.IndexOf("Remove-Item -LiteralPath '`$Script:GuestRoot\jobs\boot-facts.json'", $factsAt) -gt $factsAt) 'Boot facts from an earlier start of the disk go before a new start is read' ''

    # A run brings back only its own rows, so it cannot overwrite what the host added to another.
    $evidence = Join-Path $Script:Root 'evidence'
    $other = Join-Path $evidence 'vhd.other'
    New-Item -ItemType Directory -Force -Path $other | Out-Null
    Set-Content -LiteralPath (Join-Path $other 'independent.json') -Value 'what the host added'
    $rowFile = Join-Path $Script:Root 'row-zip\vhd.mine\report.json'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $rowFile) | Out-Null
    Set-Content -LiteralPath $rowFile -Value 'from the guest'
    $rowZip = Join-Path $Script:Root 'row.zip'
    Compress-Archive -Path (Split-Path -Parent $rowFile) -DestinationPath $rowZip
    $rows = Invoke-Script -Path $Script:Rig -Arguments @('-Action', 'Rows', '-PcFile', $pc, '-Rows', 'vhd.mine', '-EvidenceDir', $evidence, '-JobMinutes', '1') -Environment @{ SAK_TEST_ZIP = $rowZip }
    Assert-That ($rows.code -eq 0) 'A row runs' $rows.text
    Assert-That ($rows.calls -cmatch "Compress-Archive -Path @\(@\('C:\\SAKCert\\evidence\\vhd\.mine'\) \| Where-Object \{ Test-Path -LiteralPath \`$_ \}\)") 'It asks for that row alone, if it ran' $rows.calls
    Assert-That ((Get-Content -LiteralPath (Join-Path $other 'independent.json') -Raw).Trim() -ceq 'what the host added') 'Another row is left as it was'
    Assert-That (Test-Path -LiteralPath (Join-Path $evidence 'vhd.mine\report.json')) 'The row came back'
    Assert-That (-not (Test-Path -LiteralPath (Join-Path $evidence 'sakcert-evidence-*.zip'))) 'Its archive is gone'
    # A row that failed exits 1, and the group reads that from its report; the rows come back.
    Remove-Item -LiteralPath (Join-Path $evidence 'vhd.mine') -Recurse -Force
    $failedRow = Invoke-Script -Path $Script:Rig -Arguments @('-Action', 'Rows', '-PcFile', $pc, '-Rows', 'vhd.mine', '-EvidenceDir', $evidence, '-JobMinutes', '1') -Environment @{ SAK_TEST_ZIP = $rowZip; SAK_TEST_JOB_LOG = 'ROW vhd.mine: Failed|exit 1' }
    Assert-That ($failedRow.code -eq 0 -and (Test-Path -LiteralPath (Join-Path $evidence 'vhd.mine\report.json'))) 'Rows that exit 1 for a failed row still come back, for the group to judge' $failedRow.text
    # Rows that stopped bring back what they wrote, and then fail.
    Remove-Item -LiteralPath (Join-Path $evidence 'vhd.mine') -Recurse -Force
    $stoppedRows = Invoke-Script -Path $Script:Rig -Arguments @('-Action', 'Rows', '-PcFile', $pc, '-Rows', 'vhd.mine', '-EvidenceDir', $evidence, '-JobMinutes', '1') -Environment @{ SAK_TEST_ZIP = $rowZip; SAK_TEST_JOB_LOG = 'JOB STOPPED: the rows threw at 9' }
    Assert-That ($stoppedRows.code -ne 0 -and $stoppedRows.text -match 'the rows threw') 'Rows that stopped fail the step' $stoppedRows.text
    Assert-That (Test-Path -LiteralPath (Join-Path $evidence 'vhd.mine\report.json')) 'and what they wrote still comes back' ''
    # The BIOS fixture captures from an offline clone, read-only, not a shadow copy anything can delete.
    $fixture = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'partition_manager_certification_bios_fixture.ps1') -Raw
    $groupsText = Get-Content -LiteralPath $Script:Groups -Raw
    Assert-That ($fixture -notmatch 'SnapshotSet' -and $fixture -match 'attributes disk set readonly' -and $fixture -match '\$raw = "\\Device\\Harddisk') 'The BIOS capture reads an offline clone, read-only, through its own device' ''
    $cloned = $groupsText.IndexOf("'CloneSystemDisk'")
    Assert-That ($cloned -gt $groupsText.IndexOf("Invoke-Rig -Action 'Off'", $groupsText.IndexOf('function Invoke-BuildBiosDisk')) -and $cloned -lt $groupsText.IndexOf("'AttachDisk' -More @{ Disk = `"`$Disk-source`"")) 'The clone is made with the VM off, before it is attached' ''
    Assert-That ($groupsText.Contains("'DeleteDisk' -More @{ Disk = `"`$Disk-source`" }")) 'and deleted afterwards' ''
    $cloneOnPc = Invoke-Script -Path $Script:Rig -Arguments @('-Action', 'CloneSystemDisk', '-PcFile', $pc, '-Disk', 'x')
    Assert-That ($cloneOnPc.code -ne 0 -and $cloneOnPc.text -match 'needs a VM' -and $cloneOnPc.calls -ceq '') 'Cloning a system disk stops on a physical PC' $cloneOnPc.text

    # A group stops at a row that failed, and says which.
    $pinStub = Join-Path $Script:Root 'group-pins.json'
    Set-Content -LiteralPath $pinStub -Value '{}'
    foreach ($case in @(@{ status = 'Passed'; failure = '' }, @{ status = 'Failed'; failure = 'the drill failed it' })) {
        $groupRow = Join-Path $Script:Root "group-$($case.status)\external.ssd-retrim\report.json"
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $groupRow) | Out-Null
        Set-Content -LiteralPath $groupRow -Value (@{ status = $case.status; failure = $case.failure } | ConvertTo-Json)
        $groupZip = Join-Path $Script:Root "group-$($case.status).zip"
        Compress-Archive -Path (Split-Path -Parent $groupRow) -DestinationPath $groupZip
        $groupEvidence = Join-Path $Script:Root "group-evidence-$($case.status)"
        $group = Invoke-Script -Path $Script:Groups -Arguments @('-Group', 'ssd-device', '-Rows', 'external.ssd-retrim', '-EvidenceDir', $groupEvidence, '-PcFile', $pc, '-PinFile', $pinStub, '-RowMinutes', '1') -Environment @{ SAK_TEST_ZIP = $groupZip }
        if ($case.status -eq 'Passed') {
            Assert-That ($group.code -eq 0) 'A group whose rows passed goes on' $group.text
        } else {
            Assert-That ($group.code -ne 0 -and $group.text -match 'external\.ssd-retrim failed: the drill failed it') 'A group stops at the row that failed, and names it' $group.text
        }
    }
    $rowsScript = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'run_partition_manager_app_certification.ps1') -Raw
    Assert-That ($rowsScript -match "if \(\`$report\.status -ne 'Passed'\) \{ break \}") 'The rows in the guest stop at the first that fails' ''

    # Pins come from what the PC reports.
    $disks = @(
        [ordered]@{ number = 0; model = 'Boot Drive'; size_bytes = '512110190592'; serial_number = 'SECRET-BOOT'; bus_type = 'NVMe'; unique_id = 'u0'; boot = $true; system = $true },
        [ordered]@{ number = 1; model = 'Expansion Desk'; size_bytes = '8001563221504'; serial_number = 'SECRET-HDD'; bus_type = 'USB'; unique_id = 'u1'; boot = $false; system = $false },
        [ordered]@{ number = 2; model = 'Twin'; size_bytes = '1000'; serial_number = 'SECRET-A'; bus_type = 'USB'; unique_id = 'u2'; boot = $false; system = $false },
        [ordered]@{ number = 3; model = 'Twin'; size_bytes = '1000'; serial_number = 'SECRET-B'; bus_type = 'USB'; unique_id = 'u3'; boot = $false; system = $false }
    )
    [IO.File]::WriteAllText($Script:DiskFile, ($disks | ConvertTo-Json -Compress))
    $pinPath = Join-Path $Script:Root 'pins.json'
    $pins = Invoke-Script -Path $Script:Rig -Arguments @('-Action', 'WritePins', '-PcFile', $pc, '-PinFile', $pinPath, '-PinSpec', 'hdd=Expansion Desk|8001563221504')
    Assert-That ($pins.code -eq 0) 'Pins are written' $pins.text
    Assert-That ($pins.text -notmatch 'SECRET') 'No serial number is printed' $pins.text
    $written = Get-Content -LiteralPath $pinPath -Raw | ConvertFrom-Json
    Assert-That ($written.hdd.confirmation -ceq 'CERTIFY Expansion Desk SECRET-HDD 8001563221504' -and $written.hdd.bus_type -ceq 'USB') 'The pin holds what the PC reported' ($written | ConvertTo-Json)
    $bootPin = Invoke-Script -Path $Script:Rig -Arguments @('-Action', 'WritePins', '-PcFile', $pc, '-PinFile', $pinPath, '-PinSpec', 'ssd=Boot Drive|512110190592')
    Assert-That ($bootPin.code -ne 0 -and $bootPin.text -match 'runs the Windows on that machine') 'The disk that runs that Windows cannot be pinned' $bootPin.text
    $twins = Invoke-Script -Path $Script:Rig -Arguments @('-Action', 'WritePins', '-PcFile', $pc, '-PinFile', $pinPath, '-PinSpec', 'hdd=Twin|1000')
    Assert-That ($twins.code -ne 0 -and $twins.text -match 'found 2') 'Two matching disks stop it' $twins.text
    $none = Invoke-Script -Path $Script:Rig -Arguments @('-Action', 'WritePins', '-PcFile', $pc, '-PinFile', $pinPath, '-PinSpec', 'hdd=Other|1')
    Assert-That ($none.code -ne 0 -and $none.text -match 'found 0') 'No matching disk stops it' $none.text
    $shape = Invoke-Script -Path $Script:Rig -Arguments @('-Action', 'WritePins', '-PcFile', $pc, '-PinFile', $pinPath, '-PinSpec', 'boot=Boot Drive|512110190592')
    Assert-That ($shape.code -ne 0 -and $shape.text -match 'A pin is role') 'A role that is not one stops it' $shape.text

    # The final run never reuses a trial's disk.
    $finalText = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'run_partition_manager_final_certification.ps1') -Raw
    Assert-That ($finalText -notmatch 'ReuseBiosDisk') 'The final run builds its BIOS install' ''

    # What the final run sends where. -PlanOnly reaches nothing.
    $final = Join-Path $PSScriptRoot 'run_partition_manager_final_certification.ps1'
    $onPc = Invoke-Script -Path $final -Arguments @('-ReleaseDir', 'x', '-PinFile', 'pins.json', '-PcFile', $pc, '-PlanOnly')
    Assert-That ($onPc.code -eq 0 -and $onPc.calls -ceq '') 'The plan reaches nothing' $onPc.text
    foreach ($group in 'ssd-device', 'hdd-device', 'data-device') {
        Assert-That ($onPc.text -cmatch "(?m)^$group on physical PC, pinned") "$group runs on the physical PC" $onPc.text
    }
    foreach ($group in 'vhd', 'data', 'boot-uefi', 'boot-bios') {
        Assert-That ($onPc.text -cmatch "(?m)^$group on VM rig:") "$group stays on the VM rig" $onPc.text
    }
    Assert-That ($onPc.text -cnotmatch 'virtual on') 'No virtual device group runs when devices are named' $onPc.text
    $dataLine = @($onPc.text -split "`n" | Where-Object { $_ -cmatch '^data on' })[0]
    Assert-That ($dataLine -cnotmatch 'allocate-free-space|partition-move|cluster-size-change|volume-serial-number') 'The rows for the hard drive do not also run in the VM' $dataLine
    Assert-That ($dataLine -cmatch 'dynamic-to-basic' -and $dataLine -cmatch 'file-level-data-recovery') 'The other data rows stay in the VM' $dataLine
    $deviceLine = @($onPc.text -split "`n" | Where-Object { $_ -cmatch '^data-device on' })[0]
    Assert-That ($deviceLine -cmatch 'allocate-free-space' -and $deviceLine -cmatch 'partition-move' -and $deviceLine -cmatch 'cluster-size-change' -and $deviceLine -cmatch 'volume-serial-number') 'The rows for the hard drive run on it' $deviceLine
    $noDevices = Invoke-Script -Path $final -Arguments @('-ReleaseDir', 'x', '-PlanOnly')
    foreach ($group in 'ssd-virtual', 'usb-virtual') {
        Assert-That ($noDevices.text -cmatch "(?m)^$group on VM rig:") "$group runs when no device is named" $noDevices.text
    }
    Assert-That ($noDevices.text -cnotmatch '-device on|physical PC') 'No device group runs when no device is named' $noDevices.text
    $pcNoPins = Invoke-Script -Path $final -Arguments @('-ReleaseDir', 'x', '-PcFile', $pc, '-PlanOnly')
    Assert-That ($pcNoPins.code -ne 0 -and $pcNoPins.text -match 'needs -PinFile') 'A PC with no pins stops the final run' $pcNoPins.text

    # The rows themselves stop on this machine, or on any that nobody named as disposable.
    . (Join-Path $PSScriptRoot 'partition_manager_readback.ps1')
    if (-not (Test-RbInVirtualBox -Facts (Get-RbMachineFacts))) {
        $rows = Invoke-Script -Path (Join-Path $PSScriptRoot 'run_partition_manager_app_certification.ps1') -Arguments @('-Rows', 'vhd.none', '-StageRoot', $Script:Root, '-OutputRoot', (Join-Path $Script:Root 'rows'), '-WorkRoot', (Join-Path $Script:Root 'work'))
        Assert-That ($rows.code -ne 0 -and $rows.text -match 'runs only in a VirtualBox') 'The rows stop on this machine' $rows.text
        Assert-That (-not (Test-Path -LiteralPath (Join-Path $Script:Root 'rows'))) 'before they make anything' $rows.text
    }
} finally {
    if (Test-Path -LiteralPath $Script:Root) { Remove-Item -LiteralPath $Script:Root -Recurse -Force }
}

if ($Script:Failures.Count -gt 0) {
    $Script:Failures | ForEach-Object { Write-Host "FAIL $_" }
    throw "$($Script:Failures.Count) rig check(s) failed, $($Script:Passes) held"
}
Write-Host "Certification rig, physical PC target: $($Script:Passes) checks held."

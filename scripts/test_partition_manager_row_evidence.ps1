<#
.SYNOPSIS
    Proves what scripts/partition_manager_row_evidence.ps1 refuses.

.DESCRIPTION
    Builds one row's evidence that passes, from real table bytes, then breaks it one way at a time.
    Each break keeps every file's hash correct, so it reaches the guard it is aimed at rather than
    tripping the hash check first. Touches no disk and runs nothing but this script.
#>

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'partition_manager_row_evidence.ps1')

$Script:Failures = New-Object System.Collections.Generic.List[string]
$Script:Passes = 0
$Script:Digest = 'a' * 64
$Script:Commit = 'b' * 40
$Script:ClientHash = 'c' * 64
$Script:HelperHash = 'd' * 64
$Script:Root = Join-Path ([IO.Path]::GetTempPath()) "sak-row-evidence-test-$PID"
$Script:MiB = [uint64]1048576

function Get-Spec {
    return ([ordered]@{
            id = 'vhd.fixture'
            name = 'A fixture row'
            ran_on = [ordered]@{ accepted = @('virtual_disk'); physical_claim = $false }
            client = 'unelevated'
            steps = @([ordered]@{ id = 'one'; operation = 'Create'; expect = 'success' })
            checks = @(
                [ordered]@{ contract = 'size_is_exact'; path = 'snapshots.after.data_partitions[0].size_bytes'; equals = 256 * $Script:MiB },
                [ordered]@{ contract = 'one_partition'; path = 'snapshots.after.data_partition_count'; equals = 1 },
                [ordered]@{ contract = 'grew'; path = 'snapshots.after.data_partitions[0].size_bytes'; minus_path = 'before_bytes'; equals = 128 * $Script:MiB },
                [ordered]@{ contract = 'files_same'; path = 'files_after.digest'; equals_path = 'files_before.digest' },
                [ordered]@{ contract = 'really_changed'; path = 'serial_after'; differs_from_path = 'serial_before' },
                [ordered]@{ contract = 'enough'; path = 'files_after.file_count'; at_least = 3 },
                [ordered]@{ contract = 'scanned'; path = 'scan_ok'; is_true = $true },
                [ordered]@{ contract = 'not_dirty'; path = 'dirty'; is_false = $true },
                [ordered]@{ contract = 'says_so'; path = 'said'; contains = 'BitLocker' }
            )
        } | ConvertTo-Json -Depth 8 | ConvertFrom-Json)
}

function New-GptRaw {
    param([uint64]$FirstLba, [uint64]$LastLba)
    $sector = New-Object byte[] 512
    $sector[450] = 0xEE
    [BitConverter]::GetBytes([uint32]1).CopyTo($sector, 454)
    [BitConverter]::GetBytes([uint32]::MaxValue).CopyTo($sector, 458)
    $sector[510] = 0x55
    $sector[511] = 0xAA
    $header = New-Object byte[] 512
    [Text.Encoding]::ASCII.GetBytes('EFI PART').CopyTo($header, 0)
    [BitConverter]::GetBytes([uint64]2).CopyTo($header, 72)
    [BitConverter]::GetBytes([uint32]4).CopyTo($header, 80)
    [BitConverter]::GetBytes([uint32]128).CopyTo($header, 84)
    ([Guid]'11111111-2222-3333-4444-555555555555').ToByteArray().CopyTo($header, 56)
    $entries = New-Object byte[] (4 * 128)
    ([Guid]'ebd0a0a2-b9e5-4433-87c0-68b6b72699c7').ToByteArray().CopyTo($entries, 0)
    ([Guid]'aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee').ToByteArray().CopyTo($entries, 16)
    [BitConverter]::GetBytes($FirstLba).CopyTo($entries, 32)
    [BitConverter]::GetBytes($LastLba).CopyTo($entries, 40)
    return [pscustomobject]@{
        lba0_hex = ConvertTo-RbHex -Bytes $sector
        gpt_header_hex = ConvertTo-RbHex -Bytes $header
        gpt_entries_hex = ConvertTo-RbHex -Bytes $entries
    }
}

function New-Snapshot {
    param([uint64]$SizeBytes)
    $raw = New-GptRaw -FirstLba 2048 -LastLba (2048 + ($SizeBytes / 512) - 1)
    $table = ConvertFrom-RbPartitionTable -Raw $raw
    return Add-RbDerivedFields -Snapshot ([pscustomobject]@{ sector_bytes = 512; raw = $raw; table = $table })
}

function Save-Json {
    param([string]$Path, [object]$Value)
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 32))
}

function New-Evidence {
    # A row that passes. Each test starts from a fresh copy.
    param([string]$Dir)
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    $runId = '12345678-1234-1234-1234-123456789012'
    Save-Json (Join-Path $Dir 'one.client.json') ([ordered]@{
            tool = 'partition-manager-scenario-step'; schema_version = 1; scenario_id = 'vhd.fixture'; step = 'one'; operation = 'Create'
            run_id = $runId; expect = 'success'; status = 'Passed'; expectation_met = $true; execution_engine = 'PartitionManagerController'
            route = 'elevated-helper'; helper_connected = $true; code_digest = $Script:Digest; code_commit = $Script:Commit; code_dirty = $false
            client = [ordered]@{ file_name = 'sak_utility.exe'; elevated = $false; sha256 = $Script:ClientHash; helper_sha256 = $Script:HelperHash }
            execution = [ordered]@{ success = $true; dry_run = $false; cancelled = $false; timed_out = $false; message = 'ok'; steps = @() }
        })
    Save-Json (Join-Path $Dir 'independent.json') ([ordered]@{
            snapshots = [ordered]@{ after = (New-Snapshot -SizeBytes (256 * $Script:MiB)) }
            before_bytes = 128 * $Script:MiB
            files_before = [ordered]@{ digest = 'f1'; file_count = 5 }
            files_after = [ordered]@{ digest = 'f1'; file_count = 5 }
            serial_before = 'AAAA'; serial_after = 'BBBB'; scan_ok = $true; dirty = $false; said = 'Refused: BitLocker is locked'
        })
    $report = [ordered]@{
        tool = 'partition-manager-row-report'; schema_version = 1; scenario_id = 'vhd.fixture'; status = 'Passed'; failure = ''; run_id = $runId
        started_utc = '2026-11-04T10:00:00.0000000Z'; completed_utc = '2026-11-04T10:05:00.0000000Z'
        code = [ordered]@{ digest = $Script:Digest; commit = $Script:Commit; dirty = $false }
        binaries = [ordered]@{ client_sha256 = $Script:ClientHash; helper_sha256 = $Script:HelperHash; app_sha256 = 'e' * 64 }
        rig = [ordered]@{ machine_model = 'VirtualBox' }
        media = [ordered]@{ class = 'virtual_disk'; kind = 'VHDX in a VM'; model = 'Msft Virtual Disk'; bus_type = 'File Backed Virtual'; media_type = 'Unspecified'; size_bytes = '1073741824'; serial_sha256 = 'x' }
        steps = @([ordered]@{
                id = 'one'; operation = 'Create'; expect = 'success'; report = 'one.client.json'
                observed = [ordered]@{ exit_code = 0; client_pid = 10; client_token_elevated = $false; helper_pid = 11; helper_token_elevated = $true; helper_sha256 = $Script:HelperHash }
            })
        independent_file = 'independent.json'
        artifacts = @()
    }
    Save-Json (Join-Path $Dir 'report.json') $report
    Update-Hashes -Dir $Dir
}

function Update-Hashes {
    param([string]$Dir)
    $report = Get-Content -LiteralPath (Join-Path $Dir 'report.json') -Raw | ConvertFrom-Json
    $records = @()
    foreach ($name in 'independent.json', 'one.client.json') {
        if (Test-Path -LiteralPath (Join-Path $Dir $name)) { $records += (Get-PmFileRecord -Root $Dir -Relative $name) }
    }
    $report.artifacts = @($records)
    Save-Json (Join-Path $Dir 'report.json') $report
}

function Edit-Json {
    param([string]$Path, [scriptblock]$Change)
    $value = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    & $Change $value
    Save-Json $Path $value
}

function Assert-Refused {
    <#
    .SYNOPSIS
        Breaks a fresh copy of the evidence one way, and wants one reason that names the break.
    #>
    param([string]$Name, [string]$Expect, [scriptblock]$Break, [object]$Spec = $null, [switch]$KeepHashes)
    $dir = Join-Path $Script:Root ([Guid]::NewGuid().ToString('N'))
    New-Evidence -Dir $dir
    & $Break $dir
    if (-not $KeepHashes) { Update-Hashes -Dir $dir }
    $use = if ($null -ne $Spec) { $Spec } else { Get-Spec }
    $reasons = @(Test-PmRowEvidence -Spec $use -RowDir $dir -CurrentCodeDigest $Script:Digest)
    if ($reasons.Count -eq 0) {
        $Script:Failures.Add("$Name was accepted")
    } elseif (@($reasons | Where-Object { $_ -match $Expect }).Count -eq 0) {
        $Script:Failures.Add("$Name was refused for another reason: $($reasons -join ' / ')")
    } else {
        $Script:Passes++
    }
    Remove-Item -LiteralPath $dir -Recurse -Force
}

function Set-Report {
    param([string]$Dir, [scriptblock]$Change)
    Edit-Json (Join-Path $Dir 'report.json') $Change
}

function Set-Client {
    param([string]$Dir, [scriptblock]$Change)
    Edit-Json (Join-Path $Dir 'one.client.json') $Change
}

function Set-Read {
    param([string]$Dir, [scriptblock]$Change)
    Edit-Json (Join-Path $Dir 'independent.json') $Change
}

New-Item -ItemType Directory -Force -Path $Script:Root | Out-Null
try {
    # What the parsers read from real bytes.
    $gpt = ConvertFrom-RbPartitionTable -Raw (New-GptRaw -FirstLba 2048 -LastLba 4095)
    if ($gpt.style -cne 'GPT' -or @($gpt.partitions).Count -ne 1 -or [uint64]$gpt.partitions[0].offset_bytes -ne $Script:MiB -or [uint64]$gpt.partitions[0].size_bytes -ne $Script:MiB) {
        $Script:Failures.Add('A GPT table was misread')
    } else { $Script:Passes++ }
    $mbrSector = New-Object byte[] 512
    $mbrSector[446] = 0x80
    $mbrSector[450] = 0x07
    [BitConverter]::GetBytes([uint32]2048).CopyTo($mbrSector, 454)
    [BitConverter]::GetBytes([uint32]2048).CopyTo($mbrSector, 458)
    $mbrSector[466] = 0x05
    [BitConverter]::GetBytes([uint32]4096).CopyTo($mbrSector, 470)
    [BitConverter]::GetBytes([uint32]4096).CopyTo($mbrSector, 474)
    $mbrSector[510] = 0x55
    $mbrSector[511] = 0xAA
    $ebr = New-Object byte[] 512
    $ebr[450] = 0x07
    [BitConverter]::GetBytes([uint32]2048).CopyTo($ebr, 454)
    [BitConverter]::GetBytes([uint32]2048).CopyTo($ebr, 458)
    $ebr[510] = 0x55
    $ebr[511] = 0xAA
    $mbr = ConvertFrom-RbPartitionTable -Raw ([pscustomobject]@{ lba0_hex = (ConvertTo-RbHex -Bytes $mbrSector); ebr = @([pscustomobject]@{ lba = 4096; hex = (ConvertTo-RbHex -Bytes $ebr) }) })
    $layouts = (@($mbr.partitions) | ForEach-Object { $_.layout }) -join ','
    if ($mbr.style -cne 'MBR' -or $layouts -cne 'primary,extended,logical') {
        $Script:Failures.Add("An MBR table was misread: $layouts")
    } else { $Script:Passes++ }
    $logical = @($mbr.partitions)[2]
    if ([uint64]$logical.offset_bytes -ne ((4096 + 2048) * 512)) { $Script:Failures.Add('A logical partition was placed wrongly') } else { $Script:Passes++ }
    $blank = ConvertFrom-RbPartitionTable -Raw ([pscustomobject]@{ lba0_hex = ('00' * 512) })
    if ($blank.style -cne 'RAW') { $Script:Failures.Add('A blank disk was read as partitioned') } else { $Script:Passes++ }

    # Which machine may run the rows at all.
    $vbox = [pscustomobject]@{ manufacturer = 'innotek GmbH'; model = 'VirtualBox'; uuid = '6D68F30A-E2E3-40EA-8BB1-B2F3AB9B36F2' }
    $pc = [pscustomobject]@{ manufacturer = 'Dell Inc.'; model = 'OptiPlex 7040'; uuid = '4C4C4544-0044-3510-8052-B4C04F4A3132' }
    $words = "DISPOSABLE Dell Inc. OptiPlex 7040 $($pc.uuid)"
    $named = [pscustomobject]@{ machine_uuid = $pc.uuid; confirmation = $words }
    $machineCases = @(
        @{ name = 'a VirtualBox guest needs no approval'; facts = $vbox; approval = $null; want = '' },
        @{ name = 'a PC named as disposable may run'; facts = $pc; approval = $named; want = '' },
        @{ name = 'a lower-case id in the approval names the same machine'; facts = $pc; approval = [pscustomobject]@{ machine_uuid = $pc.uuid.ToLowerInvariant(); confirmation = $words }; want = '' },
        @{ name = 'any other machine, with no approval, stops'; facts = $pc; approval = $null; want = 'runs only in a VirtualBox' },
        @{ name = 'an approval for another machine stops'; facts = $pc; approval = [pscustomobject]@{ machine_uuid = $vbox.uuid; confirmation = $words }; want = 'names another one' },
        @{ name = 'words for another machine stop'; facts = $pc; approval = [pscustomobject]@{ machine_uuid = $pc.uuid; confirmation = "DISPOSABLE Dell Inc. OptiPlex 9020 $($pc.uuid)" }; want = 'do not name this machine' },
        @{ name = 'a machine that only calls itself VirtualBox stops'; facts = [pscustomobject]@{ manufacturer = 'Dell Inc.'; model = 'VirtualBox'; uuid = $pc.uuid }; approval = $null; want = 'runs only in a VirtualBox' },
        @{ name = 'a board with a blank id cannot be named'; facts = [pscustomobject]@{ manufacturer = 'Dell Inc.'; model = 'OptiPlex 7040'; uuid = 'FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF' }; approval = [pscustomobject]@{ machine_uuid = 'FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF'; confirmation = 'DISPOSABLE Dell Inc. OptiPlex 7040 FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF' }; want = 'no id of its own' },
        @{ name = 'a board with a zero id cannot be named'; facts = [pscustomobject]@{ manufacturer = 'Dell Inc.'; model = 'OptiPlex 7040'; uuid = '00000000-0000-0000-0000-000000000000' }; approval = [pscustomobject]@{ machine_uuid = '00000000-0000-0000-0000-000000000000'; confirmation = 'DISPOSABLE Dell Inc. OptiPlex 7040 00000000-0000-0000-0000-000000000000' }; want = 'no id of its own' },
        @{ name = 'an id that is not one cannot be named'; facts = [pscustomobject]@{ manufacturer = 'Dell Inc.'; model = 'OptiPlex 7040'; uuid = 'TO BE FILLED BY O.E.M.' }; approval = [pscustomobject]@{ machine_uuid = 'TO BE FILLED BY O.E.M.'; confirmation = 'DISPOSABLE Dell Inc. OptiPlex 7040 TO BE FILLED BY O.E.M.' }; want = 'no id of its own' }
    )
    foreach ($case in $machineCases) {
        $why = Get-RbMachineRefusal -Facts $case.facts -Approval $case.approval
        $held = if ($case.want) { $why.Contains($case.want) } else { $why -ceq '' }
        if ($held) { $Script:Passes++ } else { $Script:Failures.Add("Machine: $($case.name): '$why'") }
    }

    # The row that passes.
    $good = Join-Path $Script:Root 'good'
    New-Evidence -Dir $good
    $reasons = @(Test-PmRowEvidence -Spec (Get-Spec) -RowDir $good -CurrentCodeDigest $Script:Digest)
    if ($reasons.Count -ne 0) { $Script:Failures.Add("Good evidence was refused: $($reasons -join ' / ')") } else { $Script:Passes++ }
    $ranOn = Get-PmRanOn -RowDir $good
    if ($ranOn.class -cne 'virtual_disk' -or $ranOn.model -cne 'Msft Virtual Disk' -or $ranOn.Contains('serial_sha256')) { $Script:Failures.Add('What a row ran on was misreported') } else { $Script:Passes++ }

    # The report itself.
    Assert-Refused 'no report' 'no report' { param($d) Remove-Item (Join-Path $d 'report.json'); New-Item -ItemType File (Join-Path $d 'x') | Out-Null } -KeepHashes
    Assert-Refused 'a hand-written report of another shape' 'not a row report' { param($d) Set-Report $d { param($r) $r.tool = 'partition-manager-external-evidence-report' } } -KeepHashes
    Assert-Refused 'another schema' 'another schema' { param($d) Set-Report $d { param($r) $r.schema_version = 9 } } -KeepHashes
    Assert-Refused 'another row' 'is for another row' { param($d) Set-Report $d { param($r) $r.scenario_id = 'vhd.other' } } -KeepHashes
    Assert-Refused 'a failed row' 'did not pass' { param($d) Set-Report $d { param($r) $r.status = 'Failed' } } -KeepHashes

    # Bound to the code.
    Assert-Refused 'other code' 'for other code' { param($d) Set-Report $d { param($r) $r.code.digest = 'f' * 64 } }
    Assert-Refused 'a dirty tree' 'uncommitted' { param($d) Set-Report $d { param($r) $r.code.dirty = $true } }
    Assert-Refused 'dirty given as a word' 'uncommitted' { param($d) Set-Report $d { param($r) $r.code.dirty = 'false' } }
    Assert-Refused 'no real commit' 'no real commit' { param($d) Set-Report $d { param($r) $r.code.commit = 'unavailable' } }
    Assert-Refused 'ends before it starts' 'before it starts' { param($d) Set-Report $d { param($r) $r.completed_utc = '2026-11-04T09:00:00.0000000Z' } }
    Assert-Refused 'ends in the future' 'in the future' { param($d) Set-Report $d { param($r) $r.completed_utc = '2099-01-01T00:00:00.0000000Z' } }
    Assert-Refused 'no readable run time' 'no readable run time' { param($d) Set-Report $d { param($r) $r.started_utc = 'yesterday' } }

    $outside = @(Test-PmRowEvidence -Spec (Get-Spec) -RowDir $good -CurrentCodeDigest $Script:Digest -CodeCommitsAllowed @('e' * 40))
    if (@($outside | Where-Object { $_ -match 'outside this release' }).Count -eq 0) { $Script:Failures.Add("A commit outside the release was accepted: $($outside -join ' / ')") } else { $Script:Passes++ }
    $inside = @(Test-PmRowEvidence -Spec (Get-Spec) -RowDir $good -CurrentCodeDigest $Script:Digest -CodeCommitsAllowed @($Script:Commit))
    if ($inside.Count -ne 0) { $Script:Failures.Add("The release's own commit was refused: $($inside -join ' / ')") } else { $Script:Passes++ }

    # Its files.
    Assert-Refused 'a file changed after the row wrote it' 'changed after' { param($d) Add-Content -LiteralPath (Join-Path $d 'independent.json') -Value ' ' } -KeepHashes
    Assert-Refused 'a listed file missing' 'missing' { param($d) Set-Report $d { param($r) $r.artifacts += [pscustomobject]@{ path = 'gone.json'; sha256 = 'a'; bytes = 1 } } } -KeepHashes
    Assert-Refused 'a file outside the row' 'outside the row' { param($d) Set-Report $d { param($r) $r.artifacts += [pscustomobject]@{ path = '..\..\x.json'; sha256 = 'a'; bytes = 1 } } } -KeepHashes
    Assert-Refused 'a URL for evidence' 'outside the row' { param($d) Set-Report $d { param($r) $r.artifacts += [pscustomobject]@{ path = 'https://example.invalid/x'; sha256 = 'a'; bytes = 1 } } } -KeepHashes
    Assert-Refused 'a step report not hashed' 'hashed files' { param($d) Set-Report $d { param($r) $r.artifacts = @($r.artifacts | Where-Object { $_.path -ne 'one.client.json' }) } } -KeepHashes
    Assert-Refused 'a readback not hashed' 'no readback' { param($d) Set-Report $d { param($r) $r.artifacts = @($r.artifacts | Where-Object { $_.path -ne 'independent.json' }) } } -KeepHashes

    Assert-Refused 'a file too large to be a report' 'too large' { param($d)
        $big = Join-Path $d 'big.json'
        $stream = [IO.File]::Create($big)
        $stream.SetLength(16MB + 1)
        $stream.Dispose()
        Set-Report $d { param($r) $r.artifacts += [pscustomobject]@{ path = 'big.json'; sha256 = (Get-FileHash -LiteralPath $big -Algorithm SHA256).Hash.ToLowerInvariant(); bytes = 16MB + 1 } }
    } -KeepHashes
    Assert-Refused 'a step report gone' 'has no report' { param($d)
        Remove-Item -LiteralPath (Join-Path $d 'one.client.json')
    } -KeepHashes

    # What says a drive spins: Windows' own word, or a rate only when Windows has none.
    $spins = [pscustomobject]@{ contract = 'drive_reports_it_spins'; path = 'media_type'; spins_by_rotation_path = 'rotation_rate_rpm' }
    $spinCases = @(
        @{ name = 'Windows says HDD, and no rate is needed'; media = 'HDD'; rate = 0; want = '' },
        @{ name = 'Windows says Unspecified, and the drive reports a rate'; media = 'Unspecified'; rate = 5425; want = '' },
        @{ name = 'Windows says Unspecified, and the drive reports no rate'; media = 'Unspecified'; rate = 0; want = 'no rotation rate' },
        @{ name = 'Windows says SSD, and a rate does not overrule it'; media = 'SSD'; rate = 5425; want = "Windows reports 'SSD'" },
        @{ name = 'Windows gives no answer at all, and a rate does not stand in for one'; media = ''; rate = 5425; want = "Windows reports ''" }
    )
    foreach ($case in $spinCases) {
        $why = Test-PmCheck -Check $spins -Independent ([pscustomobject]@{ media_type = $case.media; rotation_rate_rpm = $case.rate })
        $held = if ($case.want) { "$why".Contains($case.want) } else { "$why" -ceq '' }
        if ($held) { $Script:Passes++ } else { $Script:Failures.Add("Spins: $($case.name): '$why'") }
    }
    $noRate = Test-PmCheck -Check $spins -Independent ([pscustomobject]@{ media_type = 'Unspecified' })
    if ("$noRate".Contains('no rotation rate')) { $Script:Passes++ } else { $Script:Failures.Add("Spins: a readback with no rate at all: '$noRate'") }

    # What it ran on.
    Assert-Refused 'nothing about the media' 'what it ran on' { param($d) Set-Report $d { param($r) $r.media = $null } }
    Assert-Refused 'a class the matrix does not accept' 'matrix accepts' { param($d) Set-Report $d { param($r) $r.media.class = 'virtual_machine' } }
    Assert-Refused 'no device name' 'name its device' { param($d) Set-Report $d { param($r) $r.media.model = '' } }
    Assert-Refused 'no bus' 'name its device' { param($d) Set-Report $d { param($r) $r.media.bus_type = '' } }
    Assert-Refused 'a virtual row with a real device name' 'reported as virtual_disk' { param($d) Set-Report $d { param($r) $r.media.model = 'Seagate Expansion Desk' } }
    $physicalSpec = Get-Spec
    $physicalSpec.ran_on.accepted = @('physical')
    $physicalSpec.ran_on | Add-Member -NotePropertyName physical_requirement -NotePropertyValue ([pscustomobject]@{ media_type = @('SSD') })
    Assert-Refused 'a physical claim on a VirtualBox disk' 'is not physical' { param($d) Set-Report $d { param($r) $r.media.class = 'physical'; $r.media.model = 'VBOX HARDDISK'; $r.media.media_type = 'SSD' } } -Spec $physicalSpec
    Assert-Refused 'a physical claim on a virtual disk file' 'is not physical' { param($d) Set-Report $d { param($r) $r.media.class = 'physical'; $r.media.media_type = 'SSD' } } -Spec $physicalSpec
    Assert-Refused 'a physical device of the wrong kind' 'wants media_type' { param($d) Set-Report $d { param($r) $r.media.class = 'physical'; $r.media.model = 'Seagate Expansion Desk'; $r.media.media_type = 'HDD' } } -Spec $physicalSpec

    # Its steps.
    Assert-Refused 'too few steps' 'ran 0 steps' { param($d) Set-Report $d { param($r) $r.steps = @() } }
    Assert-Refused 'a step of another name' 'matrix names one' { param($d) Set-Report $d { param($r) $r.steps[0].id = 'two' } }
    Assert-Refused 'another operation' 'matrix asks for' { param($d) Set-Report $d { param($r) $r.steps[0].operation = 'Delete' } }
    Assert-Refused 'another expectation' 'matrix asks for' { param($d) Set-Report $d { param($r) $r.steps[0].expect = 'refused' } }
    Assert-Refused 'a step report from a shell script' 'not from the step client' { param($d) Set-Client $d { param($c) $c.tool = 'a-shell-script' } }
    Assert-Refused 'a step report from another run' 'another run' { param($d) Set-Client $d { param($c) $c.run_id = 'another' } }
    Assert-Refused 'a step report from another row' 'another row' { param($d) Set-Client $d { param($c) $c.scenario_id = 'vhd.other' } }
    Assert-Refused 'a step that did not pass' 'did not do what it expected' { param($d) Set-Client $d { param($c) $c.status = 'Failed' } }
    Assert-Refused 'a step that did not meet its expectation' 'did not do what it expected' { param($d) Set-Client $d { param($c) $c.expectation_met = $false } }
    Assert-Refused 'a step built from other code' 'built from other code' { param($d) Set-Client $d { param($c) $c.code_digest = 'f' * 64 } }
    Assert-Refused 'a step that skipped the controller' 'through the controller' { param($d) Set-Client $d { param($c) $c.execution_engine = 'powershell' } }
    Assert-Refused 'a step under another name' 'another name' { param($d) Set-Client $d { param($c) $c.client.file_name = 'partition_manager_live_certifier.exe' } }
    Assert-Refused 'a step from another binary' 'another client binary' { param($d) Set-Client $d { param($c) $c.client.sha256 = 'f' * 64 } }
    Assert-Refused 'an elevated client' 'ran elevated' { param($d) Set-Client $d { param($c) $c.client.elevated = $true } }
    Assert-Refused 'Windows saw an elevated client' 'standard-user client' { param($d) Set-Report $d { param($r) $r.steps[0].observed.client_token_elevated = $true } }
    Assert-Refused 'nothing observed of the client' 'standard-user client' { param($d) Set-Report $d { param($r) $r.steps[0].observed = [pscustomobject]@{ exit_code = 0 } } }
    Assert-Refused 'another route' 'did not use the helper' { param($d) Set-Client $d { param($c) $c.route = 'local-powershell' } }
    Assert-Refused 'the helper never connected' 'never connected' { param($d) Set-Client $d { param($c) $c.helper_connected = $false } }
    Assert-Refused 'Windows saw no elevated helper' 'elevated helper' { param($d) Set-Report $d { param($r) $r.steps[0].observed.helper_token_elevated = $false } }
    Assert-Refused 'another helper binary' 'another binary' { param($d) Set-Report $d { param($r) $r.steps[0].observed.helper_sha256 = 'f' * 64 } }
    Assert-Refused 'Apply did not succeed' 'did not succeed' { param($d) Set-Client $d { param($c) $c.execution.success = $false } }
    Assert-Refused 'a dry run' 'dry run' { param($d) Set-Client $d { param($c) $c.execution.dry_run = $true } }
    Assert-Refused 'a cancelled run' 'cancelled' { param($d) Set-Client $d { param($c) $c.execution.cancelled = $true } }
    $elevatedSpec = Get-Spec
    $elevatedSpec.client = 'elevated'
    Assert-Refused 'an elevated-path row that was not elevated' 'runs as administrator' { param($d) Set-Client $d { param($c) } } -Spec $elevatedSpec

    # What was read back. The summaries must agree with the bytes.
    Assert-Refused 'a table summary edited to pass' 'does not match its raw bytes' { param($d) Set-Read $d { param($i) $i.snapshots.after.table.partitions[0].size_bytes = 999 } }
    Assert-Refused 'raw bytes that are not hex' 'cannot be read' { param($d) Set-Read $d { param($i) $i.snapshots.after.raw.lba0_hex = 'nope' } }
    Assert-Refused 'a derived field edited to pass' 'one_partition' { param($d) Set-Read $d { param($i) $i.snapshots.after.table.partitions = @(); $i.snapshots.after.raw.gpt_entries_hex = ('00' * 512); $i.snapshots.after.data_partition_count = 1 } }

    # Each kind of check, holding its value false rather than blank.
    Assert-Refused 'equals' 'size_is_exact' { param($d) Set-Read $d { param($i) $i.snapshots.after = (New-Snapshot -SizeBytes (128 * $Script:MiB)) } }
    Assert-Refused 'a difference' 'grew' { param($d) Set-Read $d { param($i) $i.before_bytes = 1 } }
    Assert-Refused 'equals_path' 'files_same' { param($d) Set-Read $d { param($i) $i.files_after.digest = 'f2' } }
    Assert-Refused 'differs_from_path' 'really_changed' { param($d) Set-Read $d { param($i) $i.serial_after = 'AAAA' } }
    Assert-Refused 'at_least' 'enough' { param($d) Set-Read $d { param($i) $i.files_after.file_count = 2 } }
    Assert-Refused 'is_true given false' 'scanned' { param($d) Set-Read $d { param($i) $i.scan_ok = $false } }
    Assert-Refused 'is_true given a word' 'scanned' { param($d) Set-Read $d { param($i) $i.scan_ok = 'true' } }
    Assert-Refused 'is_false given true' 'not_dirty' { param($d) Set-Read $d { param($i) $i.dirty = $true } }
    Assert-Refused 'contains' 'says_so' { param($d) Set-Read $d { param($i) $i.said = 'some other reason' } }
    Assert-Refused 'a value nobody read back' 'nothing was read back' { param($d) Set-Read $d { param($i) $i.PSObject.Properties.Remove('scan_ok') } }
    Assert-Refused 'a difference with nothing to take away' 'nothing was read back at before_bytes' { param($d) Set-Read $d { param($i) $i.PSObject.Properties.Remove('before_bytes') } }
    Assert-Refused 'equals_path with nothing to compare' 'nothing was read back at files_before' { param($d) Set-Read $d { param($i) $i.PSObject.Properties.Remove('files_before') } }
    Assert-Refused 'differs_from_path with nothing to compare' 'nothing was read back at serial_before' { param($d) Set-Read $d { param($i) $i.PSObject.Properties.Remove('serial_before') } }
    $unknown = Get-Spec
    $unknown.checks = @([pscustomobject]@{ contract = 'odd'; path = 'dirty'; matches_nothing = 1 })
    Assert-Refused 'a test the verifier does not know' 'no test this verifier knows' { param($d) } -Spec $unknown

    # The index that selects a run: an older run in the repository is never picked up by accident.
    $evidenceRoot = Join-Path $Script:Root 'run'
    Copy-Item -LiteralPath $good -Destination (Join-Path $evidenceRoot 'vhd.fixture') -Recurse -Force
    $matrix = [pscustomobject]@{ vhd_scenarios = @((Get-Spec)); external_gates = @() }
    $reportHash = (Get-FileHash -LiteralPath (Join-Path $evidenceRoot 'vhd.fixture\report.json') -Algorithm SHA256).Hash.ToLowerInvariant()
    function New-Index {
        return [pscustomobject]@{
            tool = 'partition-manager-evidence-index'; schema_version = 1; code_digest = $Script:Digest; code_commit = $Script:Commit
            rows = @([pscustomobject]@{ id = 'vhd.fixture'; report_sha256 = $reportHash })
        }
    }
    function Assert-IndexRefused {
        param([string]$Name, [string]$Expect, [scriptblock]$Break)
        $index = New-Index
        & $Break $index
        $why = @(Test-PmEvidenceIndex -Index $index -EvidenceRoot $evidenceRoot -Matrix $matrix)
        if (@($why | Where-Object { $_ -match $Expect }).Count -eq 0) { $Script:Failures.Add("$Name was not refused for that reason: $($why -join ' / ')") } else { $Script:Passes++ }
    }
    $indexGood = @(Test-PmEvidenceIndex -Index (New-Index) -EvidenceRoot $evidenceRoot -Matrix $matrix)
    if ($indexGood.Count -ne 0) { $Script:Failures.Add("A good evidence index was refused: $($indexGood -join ' / ')") } else { $Script:Passes++ }
    Assert-IndexRefused 'an index of another shape' 'not one this verifier reads' { param($i) $i.tool = 'something-else' }
    Assert-IndexRefused 'an index of another schema' 'not one this verifier reads' { param($i) $i.schema_version = 2 }
    Assert-IndexRefused 'an index with no digest' 'names no code digest' { param($i) $i.code_digest = 'x' }
    Assert-IndexRefused 'an index with no commit' 'names no commit' { param($i) $i.code_commit = 'x' }
    Assert-IndexRefused 'an index naming a row nobody asked for' 'unknown row' { param($i) $i.rows = @([pscustomobject]@{ id = 'vhd.nobody'; report_sha256 = 'x' }) }
    Assert-IndexRefused 'an index naming a row twice' 'twice' { param($i) $i.rows = @($i.rows[0], $i.rows[0]) }
    Assert-IndexRefused 'a report changed after it was selected' 'changed after this run was selected' { param($i) $i.rows[0].report_sha256 = 'f' * 64 }
    Remove-Item -LiteralPath (Join-Path $evidenceRoot 'vhd.fixture\report.json')
    Assert-IndexRefused 'an index naming a report that is gone' 'report is missing' { param($i) }
    # A step refused at the queue, before any Apply (vhd.clear-disk-wipe's refused_on_thin since
    # 9623ad18 moved Wipe Disk's thin-disk refusal there): no execution, and the helper never
    # connected. The evidence check accepts it for a 'refused' step, and a matrix that still asks for
    # 'failed' (an Apply that failed) names the mismatch, as on the 1db87df4 recertification.
    function New-QueueRefusalEvidence {
        param([string]$Dir)
        New-Evidence -Dir $Dir
        Set-Client $Dir {
            param($c)
            $c.expect = 'refused'
            $c.PSObject.Properties.Remove('execution')
            $c.PSObject.Properties.Remove('helper_connected')
            $c | Add-Member -NotePropertyName 'helper_connected_after_scan' -NotePropertyValue $false
            $c | Add-Member -NotePropertyName 'refused_at' -NotePropertyValue 'queue'
            $c | Add-Member -NotePropertyName 'queue_refusal_reasons' -NotePropertyValue @('Wipe Disk needs a full format, and Windows does not allow one on a thin-provisioned disk, such as a virtual disk file. Nothing was changed.')
        }
        Set-Report $Dir {
            param($r)
            $r.steps[0].expect = 'refused'
            $r.steps[0].observed.helper_token_elevated = $false
            $r.steps[0].observed.helper_sha256 = ''
        }
        Update-Hashes -Dir $Dir
    }
    $refusedSpec = Get-Spec
    $refusedSpec.steps[0].expect = 'refused'
    $queueRefusal = Join-Path $Script:Root 'queue-refusal'
    New-QueueRefusalEvidence -Dir $queueRefusal
    $queueReasons = @(Test-PmRowEvidence -Spec $refusedSpec -RowDir $queueRefusal -CurrentCodeDigest $Script:Digest)
    if ($queueReasons.Count -ne 0) { $Script:Failures.Add("A queue-time refusal for a 'refused' step was refused: $($queueReasons -join ' / ')") } else { $Script:Passes++ }
    $failedSpec = Get-Spec
    $failedSpec.steps[0].expect = 'failed'
    $stale = @(Test-PmRowEvidence -Spec $failedSpec -RowDir $queueRefusal -CurrentCodeDigest $Script:Digest)
    if (@($stale | Where-Object { $_ -match "expecting refused, and the matrix asks for Create expecting failed" }).Count -ne 1) { $Script:Failures.Add("A row that refused where the matrix asks for a failure was not named: $($stale -join ' / ')") } else { $Script:Passes++ }
    Assert-Refused 'a refused step whose client failed' 'did not do what it expected' {
        param($d)
        New-QueueRefusalEvidence -Dir $d
        Set-Client $d { param($c) $c.status = 'Failed'; $c.expectation_met = $false }
    } -Spec $refusedSpec

    # Every step a row body names, with its -Expect (Invoke-CertStep's default is 'success'), expects
    # what the matrix says: Test-PmStep compares the two at every run, so a row whose body and matrix
    # disagree can only fail. Read from the files themselves, so a change to either is seen here.
    $pmMatrixPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'certification\PARTITION_MANAGER_CERTIFICATION_MATRIX.json'
    $pmMatrix = Get-Content -LiteralPath $pmMatrixPath -Raw | ConvertFrom-Json
    $matrixExpect = @{}
    foreach ($row in @($pmMatrix.vhd_scenarios) + @($pmMatrix.external_gates)) {
        foreach ($step in @($row.steps)) { $matrixExpect["$($row.id)/$($step.id)"] = "$($step.expect)" }
    }
    $bodiesText = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'partition_manager_certification_row_bodies.ps1') -Raw
    $bodyStarts = @([regex]::Matches($bodiesText, "\`$Script:CertRowBodies\['(?<id>[^']+)'\]\s*=\s*\{"))
    $compared = @{}
    for ($index = 0; $index -lt $bodyStarts.Count; $index++) {
        $start = $bodyStarts[$index].Index
        $end = if ($index + 1 -lt $bodyStarts.Count) { $bodyStarts[$index + 1].Index } else { $bodiesText.Length }
        $rowId = $bodyStarts[$index].Groups['id'].Value
        $body = $bodiesText.Substring($start, $end - $start)
        foreach ($call in [regex]::Matches($body, "Invoke-CertStep\s+-Step\s+'(?<step>[^']+)'")) {
            $key = "$rowId/$($call.Groups['step'].Value)"
            if (-not $matrixExpect.ContainsKey($key)) { continue }
            # The call's own text: to the end of its line, and on past lines its brackets leave open
            # (external.bitlocker's -Expect follows a -Choices table several lines long).
            $depth = 0
            $stop = $body.Length
            for ($at = $call.Index; $at -lt $body.Length; $at++) {
                $char = $body[$at]
                if ($char -eq '{' -or $char -eq '(') { $depth++ }
                elseif ($char -eq '}' -or $char -eq ')') { $depth--; if ($depth -lt 0) { $stop = $at; break } }
                elseif ($char -eq "`n" -and $depth -eq 0) { $stop = $at; break }
            }
            $said = [regex]::Match($body.Substring($call.Index, $stop - $call.Index), "-Expect\s+'(?<expect>[a-z]+)'")
            $bodyExpect = if ($said.Success) { $said.Groups['expect'].Value } else { 'success' }
            $compared[$key] = $bodyExpect
            if ($bodyExpect -cne $matrixExpect[$key]) { $Script:Failures.Add("$key expects $bodyExpect in its row body, and the matrix asks for $($matrixExpect[$key])") } else { $Script:Passes++ }
        }
    }
    if ($compared.Count -lt 20) { $Script:Failures.Add("Only $($compared.Count) row-body steps were compared with the matrix") } else { $Script:Passes++ }
    if ($compared['vhd.clear-disk-wipe/refused_on_thin'] -cne 'refused' -or $matrixExpect['vhd.clear-disk-wipe/refused_on_thin'] -cne 'refused') {
        $Script:Failures.Add("vhd.clear-disk-wipe/refused_on_thin is not a refusal in both its row body ($($compared['vhd.clear-disk-wipe/refused_on_thin'])) and the matrix ($($matrixExpect['vhd.clear-disk-wipe/refused_on_thin']))")
    } else { $Script:Passes++ }

} finally {
    Remove-Item -LiteralPath $Script:Root -Recurse -Force -ErrorAction SilentlyContinue
}

# Job limits (fix round S, H1). The rehearsal's 256 GB NVMe: two passes at 15 MiB/s are 543 minutes, and
# 40 more is 583, rounded up to 600. A 1 TB drive needs a longer group and a longer pass than 6 hours.
$Script:Rehearsal = [uint64]256060514304
$Script:Terabyte = [uint64]1000204886016
function Assert-Limit { param($Actual, $Expected, [string]$Name) if ($Actual -eq $Expected) { $Script:Passes++ } else { $Script:Failures.Add("$Name : $Actual, not $Expected") } }
Assert-Limit (Get-PmGroupJobMinutes -Group 'ssd-device' -SsdBytes $Script:Rehearsal) 600 'ssd-device on the rehearsal drive'
Assert-Limit (Get-PmGroupJobMinutes -Group 'ssd-device' -SsdBytes $Script:Terabyte) 2160 'ssd-device on a 1 TB drive'
Assert-Limit (Get-PmGroupJobMinutes -Group 'hdd-device') 60 'hdd-device'
Assert-Limit (Get-PmGroupJobMinutes -Group 'data-device') 60 'data-device'
Assert-Limit (Get-PmGroupJobMinutes -Group 'vhd') 240 'a VM group'
Assert-Limit (Get-PmGroupJobMinutes -Group 'boot-bios') 240 'another VM group'
try { $null = Get-PmGroupJobMinutes -Group 'ssd-device'; $Script:Failures.Add('ssd-device with no drive size was given a limit') } catch { if ("$_" -match 'pinned drive') { $Script:Passes++ } else { $Script:Failures.Add("ssd-device refused for another reason: $_") } }
Assert-Limit (Get-PmWholeDiskPassSeconds -SizeBytes $Script:Rehearsal) 21600 'one pass on the rehearsal drive keeps its 6 hours'
Assert-Limit (Get-PmWholeDiskPassSeconds -SizeBytes ([uint64]8192 * $Script:MiB)) 21600 'one pass on the 8 GB VDI keeps its 6 hours'
Assert-Limit (Get-PmWholeDiskPassSeconds -SizeBytes $Script:Terabyte) 65392 'one pass on a 1 TB drive'

# The operation proof reruns certification rows from an uncommitted build and so drops the
# release-binding reasons, "built from other code" among them, in its own driver only
# (Get-OpsCertVerdict). The certification's verdicts, the evidence selector and the status, must
# take Test-PmRowEvidence's reasons whole: neither may name a reason it ignores.
foreach ($certification in 'select_partition_manager_evidence.ps1', 'get_partition_manager_certification_status.ps1') {
    $text = Get-Content -LiteralPath (Join-Path $PSScriptRoot $certification) -Raw
    if ($text -match 'built from other code|is for other code|uncommitted changes|names no real commit|Get-OpsCertVerdict') { $Script:Failures.Add("$certification drops a release-binding reason") } else { $Script:Passes++ }
}

if ($Script:Failures.Count -gt 0) {
    $Script:Failures | ForEach-Object { Write-Host "FAIL $_" }
    throw "$($Script:Failures.Count) evidence guard(s) failed, $($Script:Passes) held"
}
Write-Host "Partition Manager row evidence: $($Script:Passes) guards held."

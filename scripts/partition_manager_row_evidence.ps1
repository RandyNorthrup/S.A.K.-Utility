<#
.SYNOPSIS
    Writes and checks one Partition Manager certification row's evidence.

.DESCRIPTION
    A row is accepted on what a separate tool read back, not on the application's word and not on
    the presence of a field. Test-PmRowEvidence holds each guard in one place, and both the status
    script and the strict verifier use it:

      - the report and every file it lists exist, and their bytes hash to what the report says;
      - the report is bound to the code being released: the same code digest, a clean tree, a real
        commit, a run time that makes sense;
      - each step the matrix asks for ran, in order, through the real controller, under the token and
        through the helper Windows says it did;
      - what the row ran on is what the matrix accepts for it, and a device claimed as physical is
        not a virtual one;
      - every partition table summary matches the raw bytes stored beside it, parsed again here;
      - every check the matrix names holds over the readback.

    Nothing is signed. What stops a hand-edited report is that its numbers have to agree with raw
    bytes and with every step's report, and that the whole set is committed: an honest mistake or a
    stale run fails here. Someone forging all of it on purpose would not be stopped.
#>

Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'partition_manager_readback.ps1')

$Script:PmRowTool = 'partition-manager-row-report'
$Script:PmStepTool = 'partition-manager-scenario-step'
$Script:PmNotDataGptTypes = @(
    'e3c9e316-0b5c-4db8-817d-f92df00215ae', # Microsoft reserved
    'c12a7328-f81f-11d2-ba4b-00a0c93ec93b', # EFI system
    '5808c8aa-7e8f-42e0-85d2-e1e90434cfb3', # LDM metadata
    'af9b60a0-1431-4f62-bc68-3311714a69ad'  # LDM data
)
$Script:PmDynamicGptTypes = @('5808c8aa-7e8f-42e0-85d2-e1e90434cfb3', 'af9b60a0-1431-4f62-bc68-3311714a69ad')
$Script:PmVirtualDeviceNames = 'VBOX|VIRTUAL|QEMU|VMWARE|MSFT VIRTUAL'
$Script:PmMaximumArtifactBytes = 16MB

function Add-RbDerivedFields {
    <#
    .SYNOPSIS
        What a snapshot's table means, worked out from the table. The verifier works it out again.
    #>
    param([Parameter(Mandatory = $true)] [object]$Snapshot)
    $all = @($Snapshot.table.partitions)
    $data = @($all | Where-Object {
            $layout = if ($null -ne $_.PSObject.Properties['layout']) { $_.layout } else { '' }
            $type = "$($_.type)"
            $layout -ne 'extended' -and $Script:PmNotDataGptTypes -notcontains $type -and $type -ne '42' -and $type -ne 'EE'
        })
    $layouts = @($all | ForEach-Object { if ($null -ne $_.PSObject.Properties['layout']) { $_.layout } else { '' } })
    $dynamic = @($all | Where-Object { $Script:PmDynamicGptTypes -contains "$($_.type)" -or "$($_.type)" -eq '42' })
    foreach ($pair in @(
            @('data_partitions', @($data)),
            @('data_partition_count', $data.Count),
            @('logical_partition_count', @($layouts | Where-Object { $_ -eq 'logical' }).Count),
            @('extended_partition_count', @($layouts | Where-Object { $_ -eq 'extended' }).Count),
            @('dynamic_entry_count', $dynamic.Count))) {
        $Snapshot | Add-Member -NotePropertyName $pair[0] -NotePropertyValue $pair[1] -Force
    }
    return $Snapshot
}

function Get-PmFileRecord {
    param([Parameter(Mandatory = $true)] [string]$Root, [Parameter(Mandatory = $true)] [string]$Relative)
    $full = Join-Path $Root $Relative
    $hash = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToLowerInvariant()
    return [ordered]@{ path = $Relative.Replace('\', '/'); sha256 = $hash; bytes = [uint64](Get-Item -LiteralPath $full).Length }
}

function New-PmRowReport {
    <#
    .SYNOPSIS
        Writes a row's report.json, listing and hashing what it read back and each step's report.
    #>
    param(
        [Parameter(Mandatory = $true)] [string]$RowId,
        [Parameter(Mandatory = $true)] [string]$RowDir,
        [Parameter(Mandatory = $true)] [string]$RunId,
        [Parameter(Mandatory = $true)] [string]$Status,
        [AllowEmptyString()] [string]$Failure = '',
        [Parameter(Mandatory = $true)] [string]$StartedUtc,
        [Parameter(Mandatory = $true)] [object]$Receipt,
        [Parameter(Mandatory = $true)] [object]$Rig,
        [AllowNull()] [object]$Media,
        [AllowEmptyCollection()] [object[]]$Steps = @(),
        [AllowNull()] [object]$Independent
    )
    $independentName = 'independent.json'
    $independentPath = Join-Path $RowDir $independentName
    [IO.File]::WriteAllText($independentPath, (ConvertTo-Json -InputObject $Independent -Depth 32))
    $records = @(Get-PmFileRecord -Root $RowDir -Relative $independentName)
    foreach ($step in $Steps) {
        if (Test-Path -LiteralPath (Join-Path $RowDir $step.report)) { $records += Get-PmFileRecord -Root $RowDir -Relative $step.report }
    }
    $report = [ordered]@{
        tool = $Script:PmRowTool
        schema_version = 1
        scenario_id = $RowId
        status = $Status
        failure = $Failure
        run_id = $RunId
        started_utc = $StartedUtc
        completed_utc = (Get-Date).ToUniversalTime().ToString('o')
        code = [ordered]@{ digest = $Receipt.code_digest; commit = $Receipt.code_commit; dirty = $Receipt.code_dirty }
        binaries = [ordered]@{ client_sha256 = $Receipt.client_sha256; helper_sha256 = $Receipt.helper_sha256; app_sha256 = $Receipt.app_sha256 }
        rig = $Rig
        media = $Media
        steps = @($Steps)
        independent_file = $independentName
        artifacts = $records
    }
    [IO.File]::WriteAllText((Join-Path $RowDir 'report.json'), (ConvertTo-Json -InputObject $report -Depth 16))
    return [pscustomobject]$report
}

function Add-PmRowHostEvidence {
    <#
    .SYNOPSIS
        Adds what only the host could see to a row's readback, such as whether a disk started in a
        VM of its own, and hashes the files that came with it.
    #>
    param(
        [Parameter(Mandatory = $true)] [string]$RowDir,
        [Parameter(Mandatory = $true)] [hashtable]$Values,
        [string[]]$ExtraFiles = @()
    )
    $independentPath = Join-Path $RowDir 'independent.json'
    $independent = Get-Content -LiteralPath $independentPath -Raw | ConvertFrom-Json
    foreach ($key in $Values.Keys) {
        $independent | Add-Member -NotePropertyName $key -NotePropertyValue $Values[$key] -Force
    }
    [IO.File]::WriteAllText($independentPath, (ConvertTo-Json -InputObject $independent -Depth 32))
    $reportPath = Join-Path $RowDir 'report.json'
    $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    $names = @(@($report.artifacts) | ForEach-Object { "$($_.path)" }) + $ExtraFiles | Sort-Object -Unique
    $report.artifacts = @($names | ForEach-Object { [pscustomobject](Get-PmFileRecord -Root $RowDir -Relative $_) })
    $report.completed_utc = (Get-Date).ToUniversalTime().ToString('o')
    [IO.File]::WriteAllText($reportPath, (ConvertTo-Json -InputObject $report -Depth 16))
}

function Get-PmProperty {
    param([AllowNull()] [object]$Object, [Parameter(Mandatory = $true)] [string]$Name)
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Resolve-PmPath {
    <#
    .SYNOPSIS
        Follows a.b[0].c through parsed JSON. An unknown step yields a value nobody can satisfy.
    #>
    param([Parameter(Mandatory = $true)] [object]$Root, [Parameter(Mandatory = $true)] [string]$Path)
    $current = $Root
    foreach ($part in $Path.Split('.')) {
        if ($part -match '^(?<name>[^\[]+)\[(?<index>\d+)\]$') {
            $property = if ($null -ne $current) { $current.PSObject.Properties[$Matches['name']] } else { $null }
            if ($null -eq $property) { return [pscustomobject]@{ found = $false; value = $null } }
            $list = @($property.Value)
            $index = [int]$Matches['index']
            if ($index -ge $list.Count) { return [pscustomobject]@{ found = $false; value = $null } }
            $current = $list[$index]
        } else {
            $property = if ($null -ne $current) { $current.PSObject.Properties[$part] } else { $null }
            if ($null -eq $property) { return [pscustomobject]@{ found = $false; value = $null } }
            $current = $property.Value
        }
    }
    return [pscustomobject]@{ found = $true; value = $current }
}

function Test-PmValueSame {
    # Sizes travel as strings or numbers, and a boolean is not a word.
    param([AllowNull()] [object]$Left, [AllowNull()] [object]$Right)
    if ($null -eq $Left -or $null -eq $Right) { return $false }
    if ($Left -is [bool] -or $Right -is [bool]) { return ($Left -is [bool]) -and ($Right -is [bool]) -and ($Left -eq $Right) }
    return "$Left" -ceq "$Right"
}

function Test-PmCheck {
    <#
    .SYNOPSIS
        One promise the matrix names, over a row's readback. Returns why it failed, or nothing.
    #>
    param([Parameter(Mandatory = $true)] [object]$Check, [Parameter(Mandatory = $true)] [object]$Independent)
    $resolved = Resolve-PmPath -Root $Independent -Path $Check.path
    if (-not $resolved.found) { return "$($Check.contract): nothing was read back at $($Check.path)" }
    $value = $resolved.value
    $names = @($Check.PSObject.Properties | ForEach-Object { $_.Name })
    if ($names -contains 'minus_path') {
        $other = Resolve-PmPath -Root $Independent -Path $Check.minus_path
        if (-not $other.found) { return "$($Check.contract): nothing was read back at $($Check.minus_path)" }
        $value = [decimal]"$value" - [decimal]"$($other.value)"
    }
    if ($names -contains 'equals') {
        if (-not (Test-PmValueSame -Left $value -Right $Check.equals)) { return "$($Check.contract): $($Check.path) is '$value', not '$($Check.equals)'" }
        return ''
    }
    if ($names -contains 'equals_path') {
        $other = Resolve-PmPath -Root $Independent -Path $Check.equals_path
        if (-not $other.found) { return "$($Check.contract): nothing was read back at $($Check.equals_path)" }
        if (-not (Test-PmValueSame -Left $value -Right $other.value)) { return "$($Check.contract): $($Check.path) is '$value', and $($Check.equals_path) is '$($other.value)'" }
        return ''
    }
    if ($names -contains 'differs_from_path') {
        $other = Resolve-PmPath -Root $Independent -Path $Check.differs_from_path
        if (-not $other.found) { return "$($Check.contract): nothing was read back at $($Check.differs_from_path)" }
        if ($null -eq $value -or (Test-PmValueSame -Left $value -Right $other.value)) { return "$($Check.contract): $($Check.path) did not change from '$value'" }
        return ''
    }
    if ($names -contains 'at_least') {
        $number = [decimal]0
        if (-not [decimal]::TryParse("$value", [ref]$number) -or $number -lt [decimal]$Check.at_least) { return "$($Check.contract): $($Check.path) is '$value', under $($Check.at_least)" }
        return ''
    }
    if ($names -contains 'is_true') {
        if ($value -isnot [bool] -or -not $value) { return "$($Check.contract): $($Check.path) is '$value', not true" }
        return ''
    }
    if ($names -contains 'is_false') {
        if ($value -isnot [bool] -or $value) { return "$($Check.contract): $($Check.path) is '$value', not false" }
        return ''
    }
    if ($names -contains 'contains') {
        if ("$value" -cnotmatch [regex]::Escape("$($Check.contains)")) { return "$($Check.contract): $($Check.path) does not say '$($Check.contains)'" }
        return ''
    }
    return "$($Check.contract): the matrix names no test this verifier knows"
}

function Find-PmSnapshots {
    # Every object in a readback that stores raw table bytes beside a parsed table.
    param([AllowNull()] [object]$Node, [string]$Path = '')
    if ($null -eq $Node -or $Node -is [string] -or $Node -is [ValueType]) { return }
    if ($Node -is [System.Collections.IEnumerable] -and $Node -isnot [pscustomobject]) {
        $index = 0
        foreach ($item in $Node) { Find-PmSnapshots -Node $item -Path "$Path[$index]"; $index++ }
        return
    }
    if ($null -ne $Node.PSObject.Properties['raw'] -and $null -ne $Node.PSObject.Properties['table']) {
        [pscustomobject]@{ path = $Path; node = $Node }
        return
    }
    foreach ($property in $Node.PSObject.Properties) {
        Find-PmSnapshots -Node $property.Value -Path "$Path.$($property.Name)"
    }
}

function Test-PmSnapshotsAgree {
    <#
    .SYNOPSIS
        Parses each stored raw table again, and works the data partitions out from that.
    #>
    param([Parameter(Mandatory = $true)] [object]$Independent)
    $reasons = @()
    foreach ($found in @(Find-PmSnapshots -Node $Independent)) {
        $snapshot = $found.node
        try {
            $sectorBytes = [int](Get-PmProperty $snapshot 'sector_bytes')
            if ($sectorBytes -le 0) { $sectorBytes = 512 }
            $parsed = ConvertFrom-RbPartitionTable -Raw $snapshot.raw -SectorBytes $sectorBytes
            $want = ($parsed | ConvertTo-Json -Depth 8 -Compress)
            $have = ($snapshot.table | ConvertTo-Json -Depth 8 -Compress)
            if ($want -cne $have) { $reasons += "$($found.path): the table summary does not match its raw bytes" ; continue }
            # Replace what the snapshot says it derived, so a check reads only the bytes' answer.
            Add-RbDerivedFields -Snapshot $snapshot | Out-Null
        } catch {
            $reasons += "$($found.path): the raw table cannot be read: $($_.Exception.Message)"
        }
    }
    return $reasons
}

function Test-PmStep {
    param(
        [Parameter(Mandatory = $true)] [object]$Spec,
        [AllowNull()] [object]$Step,
        [Parameter(Mandatory = $true)] [string]$RowDir,
        [Parameter(Mandatory = $true)] [object]$Report,
        [Parameter(Mandatory = $true)] [string]$RowClient
    )
    if ($null -eq $Step) { return "step $($Spec.id) did not run" }
    if ("$($Step.operation)" -cne "$($Spec.operation)" -or "$($Step.expect)" -cne "$($Spec.expect)") {
        return "step $($Spec.id) ran $($Step.operation) expecting $($Step.expect), and the matrix asks for $($Spec.operation) expecting $($Spec.expect)"
    }
    $path = Join-Path $RowDir "$($Step.report)"
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return "step $($Spec.id) has no report" }
    $client = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    if ("$($client.tool)" -cne $Script:PmStepTool) { return "step $($Spec.id) report is not from the step client" }
    if ("$($client.run_id)" -cne "$($Report.run_id)") { return "step $($Spec.id) belongs to another run" }
    if ("$($client.scenario_id)" -cne "$($Report.scenario_id)") { return "step $($Spec.id) belongs to another row" }
    if ("$($client.status)" -cne 'Passed' -or $client.expectation_met -isnot [bool] -or -not $client.expectation_met) { return "step $($Spec.id) did not do what it expected: $(Get-PmProperty $client 'error')" }
    if ("$($client.code_digest)" -cne "$($Report.code.digest)") { return "step $($Spec.id) was built from other code" }
    if ("$($client.execution_engine)" -cne 'PartitionManagerController') { return "step $($Spec.id) did not go through the controller" }
    if ("$($client.client.file_name)" -cne 'sak_utility.exe') { return "step $($Spec.id) ran under another name, which the helper would not accept" }
    if ("$($client.client.sha256)" -cne "$($Report.binaries.client_sha256)") { return "step $($Spec.id) ran another client binary" }
    $observed = Get-PmProperty $Step 'observed'
    if ($RowClient -ceq 'unelevated') {
        if ($client.client.elevated -isnot [bool] -or $client.client.elevated) { return "step $($Spec.id) ran elevated, and the app does not" }
        $clientToken = Get-PmProperty $observed 'client_token_elevated'
        if ($clientToken -isnot [bool] -or $clientToken) { return "step $($Spec.id): Windows did not report a standard-user client" }
        if ("$($client.route)" -cne 'elevated-helper') { return "step $($Spec.id) did not use the helper" }
        # A refusal comes from the checks, before Apply, and so before the helper has any work.
        if ("$($Spec.expect)" -cne 'refused') {
            $connected = Get-PmProperty $client 'helper_connected'
            if ($connected -isnot [bool] -or -not $connected) { return "step $($Spec.id): the helper never connected" }
            $helperToken = Get-PmProperty $observed 'helper_token_elevated'
            if ($helperToken -isnot [bool] -or -not $helperToken) { return "step $($Spec.id): Windows did not report an elevated helper for this client" }
            if ("$(Get-PmProperty $observed 'helper_sha256')" -cne "$($Report.binaries.helper_sha256)") { return "step $($Spec.id): the helper Windows saw is another binary" }
        }
    } elseif ($client.client.elevated -isnot [bool] -or -not $client.client.elevated) {
        return "step $($Spec.id): this row's real path runs as administrator, and it did not"
    }
    if ("$($Spec.expect)" -eq 'success') {
        $execution = Get-PmProperty $client 'execution'
        if ($RowClient -ceq 'unelevated' -and ($null -eq $execution -or $execution.success -isnot [bool] -or -not $execution.success)) { return "step $($Spec.id): Apply did not succeed" }
        if ($null -ne $execution -and ($execution.dry_run -or $execution.cancelled -or $execution.timed_out)) { return "step $($Spec.id): Apply was a dry run, was cancelled, or timed out" }
    }
    return ''
}

function Test-PmMedia {
    param([Parameter(Mandatory = $true)] [object]$Spec, [AllowNull()] [object]$Media)
    if ($null -eq $Media) { return 'the row does not say what it ran on' }
    $class = "$(Get-PmProperty $Media 'class')"
    if (@($Spec.ran_on.accepted) -cnotcontains $class) { return "the row ran on $class, and the matrix accepts $(@($Spec.ran_on.accepted) -join ', ')" }
    $model = "$(Get-PmProperty $Media 'model')"
    $bus = "$(Get-PmProperty $Media 'bus_type')"
    if ([string]::IsNullOrWhiteSpace($model) -or [string]::IsNullOrWhiteSpace($bus)) { return 'the row does not name its device and bus' }
    if ($class -ceq 'physical') {
        if ($model.ToUpperInvariant() -match $Script:PmVirtualDeviceNames) { return "a device called $model is not physical" }
        $requirement = Get-PmProperty $Spec.ran_on 'physical_requirement'
        if ($null -ne $requirement) {
            foreach ($field in $requirement.PSObject.Properties) {
                $have = "$(Get-PmProperty $Media $field.Name)"
                if (@($field.Value) -cnotcontains $have) { return "the matrix wants $($field.Name) to be $(@($field.Value) -join ' or '), and this device reports '$have'" }
            }
        }
    } elseif ($model.ToUpperInvariant() -notmatch $Script:PmVirtualDeviceNames) {
        return "a device called $model was reported as $class"
    }
    return ''
}

function Test-PmRowEvidence {
    <#
    .SYNOPSIS
        The reasons a row's evidence cannot be accepted. None means it is accepted.
    #>
    param(
        [Parameter(Mandatory = $true)] [object]$Spec,
        [Parameter(Mandatory = $true)] [string]$RowDir,
        [Parameter(Mandatory = $true)] [string]$CurrentCodeDigest,
        [string[]]$CodeCommitsAllowed = @()
    )
    $reportPath = Join-Path $RowDir 'report.json'
    if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) { return @('no report.json') }
    try { $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json }
    catch { return @("report.json is not JSON: $($_.Exception.Message)") }

    if ("$(Get-PmProperty $report 'tool')" -cne $Script:PmRowTool) { return @('report.json is not a row report') }
    if ((Get-PmProperty $report 'schema_version') -ne 1) { return @('report.json has another schema') }
    if ("$(Get-PmProperty $report 'scenario_id')" -cne "$($Spec.id)") { return @('report.json is for another row') }
    if ("$(Get-PmProperty $report 'status')" -cne 'Passed') { return @("the row did not pass: $(Get-PmProperty $report 'failure')") }

    $reasons = @()
    $code = Get-PmProperty $report 'code'
    if ("$(Get-PmProperty $code 'digest')" -cne $CurrentCodeDigest) { $reasons += "the evidence is for other code: $(Get-PmProperty $code 'digest'), and this tree is $CurrentCodeDigest" }
    if ((Get-PmProperty $code 'dirty') -isnot [bool] -or (Get-PmProperty $code 'dirty')) { $reasons += 'the evidence comes from a tree with uncommitted changes' }
    if ("$(Get-PmProperty $code 'commit')" -cnotmatch '^[0-9a-f]{40}$') { $reasons += 'the evidence names no real commit' }
    if ($CodeCommitsAllowed.Count -gt 0 -and $CodeCommitsAllowed -cnotcontains "$(Get-PmProperty $code 'commit')") { $reasons += 'the evidence names a commit outside this release' }

    try {
        $started = [DateTimeOffset]::Parse("$(Get-PmProperty $report 'started_utc')", [Globalization.CultureInfo]::InvariantCulture)
        $completed = [DateTimeOffset]::Parse("$(Get-PmProperty $report 'completed_utc')", [Globalization.CultureInfo]::InvariantCulture)
        if ($completed -lt $started) { $reasons += 'the row ends before it starts' }
        if ($completed -gt [DateTimeOffset]::UtcNow.AddDays(1)) { $reasons += 'the row ends in the future' }
    } catch { $reasons += 'the row has no readable run time' }

    $artifacts = @(Get-PmProperty $report 'artifacts')
    foreach ($artifact in $artifacts) {
        $relative = "$(Get-PmProperty $artifact 'path')"
        if ($relative -match '(^|[\\/])\.\.([\\/]|$)' -or [IO.Path]::IsPathRooted($relative) -or $relative -match '://') { $reasons += "the artifact $relative is outside the row"; continue }
        $path = Join-Path $RowDir $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { $reasons += "the artifact $relative is missing"; continue }
        if ((Get-Item -LiteralPath $path).Length -gt $Script:PmMaximumArtifactBytes) { $reasons += "the artifact $relative is too large to be a report"; continue }
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne "$(Get-PmProperty $artifact 'sha256')") { $reasons += "the artifact $relative changed after the row wrote it" }
    }
    $listed = @($artifacts | ForEach-Object { "$(Get-PmProperty $_ 'path')" })

    $mediaReason = Test-PmMedia -Spec $Spec -Media (Get-PmProperty $report 'media')
    if ($mediaReason) { $reasons += $mediaReason }

    $steps = @(Get-PmProperty $report 'steps')
    $specSteps = @($Spec.steps)
    if ($steps.Count -ne $specSteps.Count) { $reasons += "the row ran $($steps.Count) steps, and the matrix names $($specSteps.Count)" }
    for ($index = 0; $index -lt $specSteps.Count; $index++) {
        $step = if ($index -lt $steps.Count) { $steps[$index] } else { $null }
        if ($null -ne $step -and "$($step.id)" -cne "$($specSteps[$index].id)") { $reasons += "step $($index + 1) is $($step.id), and the matrix names $($specSteps[$index].id)"; continue }
        if ($null -ne $step -and $listed -cnotcontains "$($step.report)") { $reasons += "step $($step.id)'s report is not among the row's hashed files" }
        $stepReason = Test-PmStep -Spec $specSteps[$index] -Step $step -RowDir $RowDir -Report $report -RowClient "$($Spec.client)"
        if ($stepReason) { $reasons += $stepReason }
    }

    $independentName = "$(Get-PmProperty $report 'independent_file')"
    if ($listed -cnotcontains $independentName) { return $reasons + @('the row lists no readback among its hashed files') }
    $independentPath = Join-Path $RowDir $independentName
    if (-not (Test-Path -LiteralPath $independentPath -PathType Leaf)) { return $reasons + @('the row has no readback') }
    $independent = Get-Content -LiteralPath $independentPath -Raw | ConvertFrom-Json
    $reasons += @(Test-PmSnapshotsAgree -Independent $independent)
    foreach ($check in @($Spec.checks)) {
        $why = Test-PmCheck -Check $check -Independent $independent
        if ($why) { $reasons += $why }
    }
    return $reasons
}

function Test-PmEvidenceIndex {
    <#
    .SYNOPSIS
        The reasons the index naming the current run cannot be trusted. It is the only way evidence
        gets selected, so an older run sitting in the repository is never picked up by accident.
    #>
    param(
        [Parameter(Mandatory = $true)] [object]$Index,
        [Parameter(Mandatory = $true)] [string]$EvidenceRoot,
        [Parameter(Mandatory = $true)] [object]$Matrix
    )
    if ("$(Get-PmProperty $Index 'tool')" -cne 'partition-manager-evidence-index' -or (Get-PmProperty $Index 'schema_version') -ne 1) {
        return @('the evidence index is not one this verifier reads')
    }
    $reasons = @()
    if ("$(Get-PmProperty $Index 'code_digest')" -cnotmatch '^[0-9a-f]{64}$') { $reasons += 'the evidence index names no code digest' }
    if ("$(Get-PmProperty $Index 'code_commit')" -cnotmatch '^[0-9a-f]{40}$') { $reasons += 'the evidence index names no commit' }
    $known = @(@($Matrix.vhd_scenarios) + @($Matrix.external_gates) | ForEach-Object { "$($_.id)" })
    $seen = @{}
    foreach ($row in @(Get-PmProperty $Index 'rows')) {
        $id = "$(Get-PmProperty $row 'id')"
        if ($known -cnotcontains $id) { $reasons += "the evidence index names an unknown row: $id"; continue }
        if ($seen.ContainsKey($id)) { $reasons += "the evidence index names $id twice"; continue }
        $seen[$id] = $true
        $path = Join-Path (Join-Path $EvidenceRoot $id) 'report.json'
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { $reasons += "the evidence index names $id, and its report is missing"; continue }
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne "$(Get-PmProperty $row 'report_sha256')") {
            $reasons += "$id changed after this run was selected"
        }
    }
    return $reasons
}

function Get-PmRanOn {
    <#
    .SYNOPSIS
        What a row ran on, for the status and the words on the site. Serial numbers stay out.
    #>
    param([Parameter(Mandatory = $true)] [string]$RowDir)
    $report = Get-Content -LiteralPath (Join-Path $RowDir 'report.json') -Raw | ConvertFrom-Json
    $media = Get-PmProperty $report 'media'
    return [ordered]@{
        class = "$(Get-PmProperty $media 'class')"
        kind = "$(Get-PmProperty $media 'kind')"
        model = "$(Get-PmProperty $media 'model')"
        bus_type = "$(Get-PmProperty $media 'bus_type')"
        media_type = "$(Get-PmProperty $media 'media_type')"
        size_bytes = "$(Get-PmProperty $media 'size_bytes')"
    }
}

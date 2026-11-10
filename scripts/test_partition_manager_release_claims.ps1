<#
.SYNOPSIS
    Proves what check_partition_manager_release_claims.ps1 writes and what it refuses.

.DESCRIPTION
    Runs the real checker in a child process over synthetic status files and a fixture copy of the
    certification matrices, in a temporary folder. It reads no evidence and touches no disk. A status
    it refuses must leave the document as it was.
#>

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'process_capture.ps1')

$Script:ProjectRoot = Split-Path -Parent $PSScriptRoot
$Script:Failures = New-Object System.Collections.Generic.List[string]
$Script:Passes = 0
$Script:Root = Join-Path ([IO.Path]::GetTempPath()) "sak-release-claims-test-$PID"
$Script:Begin = '<!-- BEGIN GENERATED certification-state: do not edit by hand. Regenerate with ' +
    'scripts/check_partition_manager_release_claims.ps1 -Update -->'
$Script:End = '<!-- END GENERATED certification-state -->'
$Script:Words = 'Words the site may use. Back up before changing a disk.'

$matrix = Get-Content -LiteralPath (Join-Path $Script:ProjectRoot 'certification\PARTITION_MANAGER_CERTIFICATION_MATRIX.json') -Raw | ConvertFrom-Json
$Script:VhdIds = @($matrix.vhd_scenarios | ForEach-Object { "$($_.id)" })
$Script:ExternalIds = @($matrix.external_gates | ForEach-Object { "$($_.id)" })
$Script:FirstVhd = @($matrix.vhd_scenarios)[0]
$Script:FirstExternal = @($matrix.external_gates)[0]

function New-Status {
    param([string]$Level, [string[]]$VhdGaps = @(), [string[]]$ExternalGaps = @(), [string[]]$Virtual = @(), [object[]]$Rows = @())
    return [pscustomobject]@{
        tool = 'partition-manager-certification-status'; schema_version = 2; claim_level = $Level
        vhd_data_disk = [pscustomobject]@{ passed = $Script:VhdIds.Count - $VhdGaps.Count; required = $Script:VhdIds.Count; incomplete_ids = @($VhdGaps) }
        external_gates = [pscustomobject]@{ passed = $Script:ExternalIds.Count - $ExternalGaps.Count; required = $Script:ExternalIds.Count; incomplete_ids = @($ExternalGaps) }
        virtual_where_a_device_matters = @($Virtual)
        site_wording = $Script:Words
        rows = @($Rows)
    }
}

function Copy-Status {
    param([object]$Status)
    return ($Status | ConvertTo-Json -Depth 8 | ConvertFrom-Json)
}

function New-Skeleton {
    return (@('# Partition Manager Certification', '', '## Current Certification State', '',
            $Script:Begin, $Script:End, '', 'Narrative history; no gate parses this paragraph.', '',
            '### Historical checkpoints', 'Historical claim level: `CodeCompleteOnly` in that historical record.') -join "`n") + "`n"
}

function Invoke-Checker {
    param([string]$Fixture, [string]$StatusPath, [switch]$Update)
    $arguments = @('-NoProfile', '-NonInteractive', '-File', (Join-Path $Fixture 'scripts\check_partition_manager_release_claims.ps1'), '-StatusPath', $StatusPath)
    if ($Update) { $arguments += '-Update' }
    return (Invoke-CapturedProcess -FilePath (Get-Process -Id $PID).Path -Arguments $arguments -TimeoutSeconds 60)
}

function Get-Block {
    param([string]$Text)
    $begin = $Text.IndexOf($Script:Begin, [StringComparison]::Ordinal)
    $end = $Text.IndexOf($Script:End, [StringComparison]::Ordinal)
    if ($begin -lt 0 -or $end -le $begin) { throw 'The fixture lost its generated markers' }
    return $Text.Substring($begin, $end - $begin)
}

function Invoke-Case {
    param([string]$Fixture, [hashtable]$Case)
    $name = $Case.name
    $statusPath = Join-Path $Fixture 'status.json'
    $documentPath = Join-Path $Fixture 'docs\PARTITION_MANAGER_CERTIFICATION.md'
    $document = if ($Case.ContainsKey('document')) { $Case.document } else { New-Skeleton }
    [IO.File]::WriteAllText($documentPath, $document)
    if ($Case.ContainsKey('generate')) {
        [IO.File]::WriteAllText($statusPath, ($Case.generate | ConvertTo-Json -Depth 8))
        $made = Invoke-Checker $Fixture $statusPath -Update
        if ($made.timed_out -or $made.exit_code -ne 0) { $Script:Failures.Add("$name could not generate: $($made.stdout)$($made.stderr)"); return }
    }
    [IO.File]::WriteAllText($statusPath, ($Case.status | ConvertTo-Json -Depth 8))
    if ($Case.ContainsKey('tamper')) {
        $text = [IO.File]::ReadAllText($documentPath)
        if (-not $text.Contains($Case.tamper[0])) { $Script:Failures.Add("$name has nothing to tamper with"); return }
        [IO.File]::WriteAllText($documentPath, $text.Replace($Case.tamper[0], $Case.tamper[1]))
    }
    $before = [IO.File]::ReadAllText($documentPath)
    $result = Invoke-Checker $Fixture $statusPath -Update:($Case.ContainsKey('update'))
    $output = "$($result.stdout)$($result.stderr)"
    $after = [IO.File]::ReadAllText($documentPath)
    if ($Case.ContainsKey('error')) {
        if ($result.timed_out -or $result.exit_code -eq 0 -or -not $output.Contains($Case.error)) { $Script:Failures.Add("$name was not refused as '$($Case.error)': $output"); return }
        if ($after -cne $before) { $Script:Failures.Add("$name was refused but changed the document"); return }
    } else {
        if ($result.timed_out -or $result.exit_code -ne 0) { $Script:Failures.Add("$name failed: $output"); return }
        $block = Get-Block $after
        foreach ($phrase in @($Case['contains'])) {
            if ($phrase -and -not $block.Contains($phrase)) { $Script:Failures.Add("$name is missing '$phrase'"); return }
        }
        foreach ($phrase in @($Case['absent'])) {
            if ($phrase -and $block.Contains($phrase)) { $Script:Failures.Add("$name must not say '$phrase'"); return }
        }
        if ($after.Replace($block, '') -cne $before.Replace((Get-Block $before), '')) { $Script:Failures.Add("$name changed text outside the block"); return }
        if ($Case.ContainsKey('update')) {
            $check = Invoke-Checker $Fixture $statusPath
            if ($check.timed_out -or $check.exit_code -ne 0) { $Script:Failures.Add("$name did not pass its own check: $($check.stdout)$($check.stderr)"); return }
        }
    }
    $Script:Passes++
}

$fixture = $Script:Root
foreach ($directory in 'scripts', 'certification', 'docs') { New-Item -ItemType Directory -Force -Path (Join-Path $fixture $directory) | Out-Null }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'check_partition_manager_release_claims.ps1') -Destination (Join-Path $fixture 'scripts')
foreach ($name in 'PARTITION_MANAGER_CERTIFICATION_MATRIX.json', 'APPLE_FILESYSTEM_CAPABILITY_MATRIX.json') {
    Copy-Item -LiteralPath (Join-Path $Script:ProjectRoot "certification\$name") -Destination (Join-Path $fixture 'certification')
}

$vhdId = "$($Script:FirstVhd.id)"
$externalId = "$($Script:FirstExternal.id)"
$physicalOnlyId = 'external.hdd-defrag-execution'
$vhdNo = "| ``$vhdId`` | $($Script:FirstVhd.name) | no |"
$vhdYes = "| ``$vhdId`` | $($Script:FirstVhd.name) | yes |"
$refusal = 'Do not mark the Partition Manager destructive paths fully certified until those reports exist.'
$device = [pscustomobject]@{ id = $externalId; accepted = $true; ran_on = [pscustomobject]@{ class = 'physical'; model = 'Some Drive'; bus_type = 'USB' } }

$partial = New-Status 'CodeCompleteOnly' -VhdGaps @($vhdId)
$externalGap = New-Status 'VhdDataDiskCertified' -ExternalGaps @($externalId)
$both = New-Status 'CodeCompleteOnly' -VhdGaps @($vhdId) -ExternalGaps @($externalId)
$failed = New-Status 'FailedEvidence' -VhdGaps @($vhdId)
$virtual = New-Status 'VirtualMachinesValidated' -Virtual @($externalId)
$named = New-Status 'NamedDevicesValidated' -Rows @($device)

$badCounts = Copy-Status $partial; $badCounts.vhd_data_disk.required = 0
$badType = Copy-Status $partial; $badType.vhd_data_disk.required = "$($badType.vhd_data_disk.required)"
$forgedCount = Copy-Status $partial; $forgedCount.vhd_data_disk.required++; $forgedCount.vhd_data_disk.passed++
$badSum = Copy-Status $partial; $badSum.vhd_data_disk.passed--
$badIds = Copy-Status $partial; $badIds.vhd_data_disk.incomplete_ids = $null
$badSchema = Copy-Status $partial; $badSchema.schema_version = 1
$noWords = Copy-Status $named; $noWords.site_wording = ''
$saysCertified = Copy-Status $named; $saysCertified.site_wording = 'Certified on real hardware.'

$skeleton = New-Skeleton
$cases = @(
    @{ name = 'one VHD gap'; status = $partial; update = $true
        contains = @('Current claim level: `CodeCompleteOnly`.', "Accepted VHD evidence: $($Script:VhdIds.Count - 1)/$($Script:VhdIds.Count).", $vhdNo, 'remains partial because VHD evidence is incomplete.', $refusal, '| `A-i` | APFS |', "> $($Script:Words)")
        absent = @($vhdYes, 'VM/hardware/lab evidence is incomplete') },
    @{ name = 'external gap only'; status = $externalGap; update = $true
        contains = @('Current claim level: `VhdDataDiskCertified`.', $vhdYes, 'remains partial because VM/hardware/lab evidence is incomplete.', $refusal)
        absent = @('VHD evidence is incomplete') },
    @{ name = 'both gaps'; status = $both; update = $true
        contains = @('because VHD evidence is incomplete and VM/hardware/lab evidence is incomplete.', $refusal) },
    @{ name = 'failed evidence'; status = $failed; update = $true
        contains = @('Current claim level: `FailedEvidence`.', 'because evidence verification failed and VHD evidence is incomplete.', $refusal) },
    @{ name = 'virtual machines only'; status = $virtual; update = $true
        contains = @('Current claim level: `VirtualMachinesValidated`.', 'Nothing here is a statement about hardware.', 'Incomplete VHD scenarios: none.', 'nothing yet')
        absent = @(' | no |', 'remains partial', $refusal) },
    @{ name = 'virtual machines with only a device-only row missing'; status = (New-Status 'VirtualMachinesValidated' -ExternalGaps @($physicalOnlyId) -Virtual @($physicalOnlyId)); update = $true
        contains = @('Current claim level: `VirtualMachinesValidated`.', "Incomplete external gates: $physicalOnlyId.", 'Nothing here is a statement about hardware.')
        absent = @('remains partial', $refusal) },
    @{ name = 'named devices with a device-only row missing'; status = (New-Status 'NamedDevicesValidated' -ExternalGaps @($physicalOnlyId)); update = $true; error = 'contradicts its incomplete evidence sets' },
    @{ name = 'the VHD level with only a device-only row missing'; status = (New-Status 'VhdDataDiskCertified' -ExternalGaps @($physicalOnlyId)); update = $true; error = 'contradicts its incomplete evidence sets' },
    @{ name = 'named devices'; status = $named; update = $true
        contains = @('Current claim level: `NamedDevicesValidated`.', 'That says those devices, and no others.', 'physical: Some Drive, USB', 'Incomplete external gates: none.')
        absent = @(' | no |', 'remains partial', $refusal) },
    @{ name = 'regenerate over a stale block'; status = $partial; generate = $named; update = $true
        contains = @('Current claim level: `CodeCompleteOnly`.', $vhdNo); absent = @('`NamedDevicesValidated`') },
    @{ name = 'a current block passes'; status = $partial; generate = $partial },
    @{ name = 'a block never generated is stale'; status = $partial; document = $skeleton; error = 'is stale' },
    @{ name = 'a block from another status is stale'; status = $partial; generate = $named; error = 'is stale' },
    @{ name = 'a hand-raised level is stale'; status = $partial; generate = $partial
        tamper = @('Current claim level: `CodeCompleteOnly`.', 'Current claim level: `NamedDevicesValidated`.'); error = 'is stale' },
    @{ name = 'a hand-accepted row is stale'; status = $partial; generate = $partial; tamper = @($vhdNo, $vhdYes); error = 'is stale' },
    @{ name = 'a dropped refusal is stale'; status = $partial; generate = $partial; tamper = @($refusal, ''); error = 'is stale' },
    @{ name = 'hand-written words are stale'; status = $named; generate = $named; tamper = @($Script:Words, 'Tested on every drive.'); error = 'is stale' },
    @{ name = 'missing markers'; status = $partial; update = $true; document = $skeleton.Replace($Script:Begin + "`n", ''); error = 'Expected exactly one BEGIN and one END' },
    @{ name = 'a second begin marker'; status = $partial; document = $skeleton.Replace($Script:End, $Script:Begin + "`n" + $Script:End); error = 'Expected exactly one BEGIN and one END' },
    @{ name = 'markers outside the section'; status = $partial; update = $true
        document = $skeleton.Replace($Script:Begin + "`n" + $Script:End + "`n", '') + $Script:Begin + "`n" + $Script:End + "`n"; error = 'must sit inside the Current Certification State section' },
    @{ name = 'no state heading'; status = $partial; update = $true; document = $skeleton.Replace('## Current Certification State', '## Historical Only'); error = 'Expected one Current Certification State heading' },
    @{ name = 'two state headings'; status = $partial; document = $skeleton + "`n## Current Certification State`n"; error = 'Expected one Current Certification State heading' },
    @{ name = 'the old hardware level'; status = (New-Status 'HardwareCertified'); update = $true; error = 'unrecognized claim_level' },
    @{ name = 'an unknown level'; status = (New-Status 'Certified' -VhdGaps @($vhdId)); update = $true; error = 'unrecognized claim_level' },
    @{ name = 'an unknown row'; status = (New-Status 'CodeCompleteOnly' -VhdGaps @('vhd.unknown-case')); update = $true; error = 'unknown or duplicate incomplete ID' },
    @{ name = 'a row twice'; status = (New-Status 'CodeCompleteOnly' -VhdGaps @($vhdId, $vhdId)); update = $true; error = 'unknown or duplicate incomplete ID' },
    @{ name = 'zero required'; status = $badCounts; update = $true; error = 'counts do not match the certification matrix' },
    @{ name = 'a count as text'; status = $badType; update = $true; error = 'required must be a nonnegative integer' },
    @{ name = 'a forged denominator'; status = $forgedCount; update = $true; error = 'counts do not match the certification matrix' },
    @{ name = 'passed and incomplete disagree'; status = $badSum; update = $true; error = 'passed plus incomplete does not equal required' },
    @{ name = 'no incomplete list'; status = $badIds; update = $true; error = 'incomplete_ids must be an array' },
    @{ name = 'another status schema'; status = $badSchema; update = $true; error = 'Unsupported Partition Manager certification status schema_version' },
    @{ name = 'named devices with a gap'; status = (New-Status 'NamedDevicesValidated' -VhdGaps @($vhdId)); update = $true; error = 'contradicts its incomplete evidence sets' },
    @{ name = 'virtual machines with a gap'; status = (New-Status 'VirtualMachinesValidated' -ExternalGaps @($externalId) -Virtual @($externalId)); update = $true; error = 'contradicts its incomplete evidence sets' },
    @{ name = 'the VHD level with a VHD gap'; status = (New-Status 'VhdDataDiskCertified' -VhdGaps @($vhdId) -ExternalGaps @($externalId)); update = $true; error = 'contradicts its incomplete evidence sets' },
    @{ name = 'the VHD level with no external gap'; status = (New-Status 'VhdDataDiskCertified'); update = $true; error = 'contradicts its incomplete evidence sets' },
    @{ name = 'code complete with no VHD gap'; status = (New-Status 'CodeCompleteOnly'); update = $true; error = 'contradicts its incomplete evidence sets' },
    @{ name = 'named devices on a virtual one'; status = (New-Status 'NamedDevicesValidated' -Virtual @($externalId)); update = $true; error = 'contradicts what its rows ran on' },
    @{ name = 'virtual machines with no virtual row'; status = (New-Status 'VirtualMachinesValidated'); update = $true; error = 'contradicts what its rows ran on' },
    @{ name = 'an invalid status in check mode'; status = (New-Status 'NamedDevicesValidated' -VhdGaps @($vhdId)); generate = $partial; error = 'contradicts its incomplete evidence sets' },
    @{ name = 'no words'; status = $noWords; update = $true; error = 'has no words for the site' },
    @{ name = 'words that say certified'; status = $saysCertified; update = $true; error = 'may not say certified' }
)

try {
    foreach ($case in $cases) { Invoke-Case -Fixture $fixture -Case $case }

    # A capability the document has no words for stops the block.
    $applePath = Join-Path $fixture 'certification\APPLE_FILESYSTEM_CAPABILITY_MATRIX.json'
    $apple = Get-Content -LiteralPath $applePath -Raw | ConvertFrom-Json
    @($apple.capabilities)[0].status = 'mostly'
    [IO.File]::WriteAllText($applePath, ($apple | ConvertTo-Json -Depth 8))
    Invoke-Case -Fixture $fixture -Case @{ name = 'an Apple capability with no words'; status = $partial; update = $true; error = 'unrenderable status' }
} finally {
    if (Test-Path -LiteralPath $Script:Root) { Remove-Item -LiteralPath $Script:Root -Recurse -Force }
}

if ($Script:Failures.Count -gt 0) {
    $Script:Failures | ForEach-Object { Write-Host "FAIL $_" }
    throw "$($Script:Failures.Count) release-claim check(s) failed, $($Script:Passes) held"
}
Write-Host "Partition Manager release claims: $($Script:Passes) checks held."

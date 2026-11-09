<#
.SYNOPSIS
    Proves how the final run's chains run, with a stand-in for the group runner, and that the final run
    really starts them.

.DESCRIPTION
    run_partition_manager_certification_chains.ps1 runs the VM rig's groups and the physical PC's device
    groups as two chains at the same time (fix round S, H2; owner ruling 2026-11-06). The stand-in records
    when each group ran and what it was given, and fails or dawdles where the environment says.

    The final run itself is then run for real, without -PlanOnly, from a copy beside stand-ins for the rig,
    the chains script, selection, the claim check and python: it must start the chains script through
    -File with both chains, and select and update the claim only after they have passed. Nothing reaches
    a VM, a PC or a disk.
#>

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$Script:Chains = Join-Path $PSScriptRoot 'run_partition_manager_certification_chains.ps1'
$Script:Root = Join-Path ([IO.Path]::GetTempPath()) "sak-chains-test-$PID"
$Script:Failures = New-Object System.Collections.Generic.List[string]
$Script:Passes = 0
$Script:VmGroups = @('vhd', 'data', 'boot-uefi')
$Script:PcGroups = @('ssd-device', 'hdd-device', 'data-device')

function Assert-That {
    param([bool]$Condition, [string]$Name, [string]$Detail = '')
    if ($Condition) { $Script:Passes++ } else { $Script:Failures.Add("$Name $Detail") }
}

function Write-StandIn {
    $path = Join-Path $Script:Root 'groups-standin.ps1'
    [IO.File]::WriteAllText($path, @'
param([string]$Group, [string]$EvidenceDir, [string]$Rows = '', [string]$CredFile = '', [string]$StageDir = '', [string]$PinFile = '', [string]$PcFile = '')
$mark = Join-Path $EvidenceDir "ran-$Group.txt"
Set-Content -LiteralPath $mark -Value "start $([DateTime]::UtcNow.Ticks)", "pid $PID", "pin '$PinFile'", "pc '$PcFile'", "rows '$Rows'"
if ($env:SAK_TEST_SLOW -and (($env:SAK_TEST_SLOW -split ',') -contains $Group)) { Start-Sleep -Milliseconds 2500 }
"group $Group says hello"
Add-Content -LiteralPath $mark -Value "end $([DateTime]::UtcNow.Ticks)"
if ($env:SAK_TEST_FAIL_GROUP -eq $Group) { [Console]::Error.WriteLine("group $Group failed on purpose"); exit 3 }
exit 0
'@)
    return $path
}

function Invoke-Chains {
    param([string]$Name, [hashtable]$Environment = @{})
    $evidence = Join-Path $Script:Root $Name
    New-Item -ItemType Directory -Force -Path $evidence | Out-Null
    $plan = @(
        [ordered]@{ name = 'vm-rig'; steps = @($Script:VmGroups | ForEach-Object { [ordered]@{ group = $_ } }) },
        [ordered]@{ name = 'physical-pc'; steps = @($Script:PcGroups | ForEach-Object { [ordered]@{ group = $_; device = $true; rows = @('external.one', 'external.two') } }) }
    )
    $planFile = Join-Path $Script:Root "$Name.json"
    ConvertTo-Json -Depth 6 -InputObject $plan | Set-Content -LiteralPath $planFile
    foreach ($key in $Environment.Keys) { Set-Item -Path "env:$key" -Value $Environment[$key] }
    try {
        $output = & (Get-Process -Id $PID).Path -NoProfile -NonInteractive -File $Script:Chains -PlanFile $planFile -EvidenceDir $evidence `
            -GroupsScript $Script:StandIn -PinFile 'C:\pins.json' -PcFile 'C:\pc.json' 2>&1
        $code = $LASTEXITCODE
    } finally {
        foreach ($key in $Environment.Keys) { Remove-Item -Path "env:$key" }
    }
    return [pscustomobject]@{ code = $code; text = (@($output | ForEach-Object { "$_" }) -join "`n"); evidence = $evidence }
}

function Get-Ran {
    param([string]$Evidence, [string]$Group)
    $mark = Join-Path $Evidence "ran-$Group.txt"
    if (-not (Test-Path -LiteralPath $mark)) { return $null }
    $lines = Get-Content -LiteralPath $mark
    $value = { param($key) (@($lines | Where-Object { $_ -like "$key *" })[0] -split ' ', 2)[1] }
    return [pscustomobject]@{ start = [long](& $value 'start'); end = [long](& $value 'end'); pid = (& $value 'pid'); pin = (& $value 'pin'); pc = (& $value 'pc'); rows = (& $value 'rows') }
}

# The final run's own launch. On 2026-11-09 it wrote its plan over the chains script's path ($chains is
# $Chains) and started nothing; only -PlanOnly had ever been run. So the real script runs here, copied
# beside stand-ins for every sibling it calls through $PSScriptRoot, and for python, which works out the
# digest. Each stand-in writes one line to calls.log beside it.
$Script:FinalScript = Join-Path $PSScriptRoot 'run_partition_manager_final_certification.ps1'
$Script:LaunchDigest = '5ab0' * 16
$Script:LaunchCommit = '0123456789abcdef0123456789abcdef01234567'

$Script:LaunchStandIns = @{
    'run_partition_manager_certification_rig.ps1'    = @'
param([string]$Action, [string]$CredFile = '', [string]$ReleaseDir = '', [string]$StageDir = '', [string]$PcFile = '')
$ErrorActionPreference = 'Stop'
Add-Content -LiteralPath (Join-Path $PSScriptRoot 'calls.log') -Value "rig $Action pc='$PcFile'"
if ($Action -eq 'Stage') {
    New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
    [IO.File]::WriteAllText((Join-Path $StageDir 'sak_utility.exe'), 'MZ step client built from DIGEST')
}
exit 0
'@
    'run_partition_manager_certification_chains.ps1' = @'
param([string]$PlanFile, [string]$EvidenceDir, [string]$CredFile = '', [string]$StageDir = '', [string]$PinFile = '', [string]$PcFile = '', [string]$GroupsScript = '', [string]$Chain = '')
$ErrorActionPreference = 'Stop'
$log = Join-Path $PSScriptRoot 'calls.log'
$plan = @(Get-Content -LiteralPath $PlanFile -Raw | ConvertFrom-Json)
$said = @($plan | ForEach-Object { "$($_.name)=$(@($_.steps | ForEach-Object { $_.group }) -join ',')" }) -join ';'
Add-Content -LiteralPath $log -Value "chains start", "chains script $PSCommandPath", "chains command line $([Environment]::CommandLine)"
Add-Content -LiteralPath $log -Value "chains plan=$PlanFile evidence=$EvidenceDir cred=$CredFile stage=$StageDir pin=$PinFile pc=$PcFile", "chains groups $said"
$exitCode = [int](Get-Content -LiteralPath (Join-Path $PSScriptRoot 'chains-exit.txt') -Raw)
Start-Sleep -Milliseconds 300
Add-Content -LiteralPath $log -Value "chains end $exitCode"
exit $exitCode
'@
    'select_partition_manager_evidence.ps1'          = @'
param([string]$RunDir, [string[]]$DenyNames, [string]$CodeDigest)
Add-Content -LiteralPath (Join-Path $PSScriptRoot 'calls.log') -Value "select run=$RunDir digest=$CodeDigest deny=$($DenyNames -join ',')"
'@
    'check_partition_manager_release_claims.ps1'     = @'
param([switch]$Update)
Add-Content -LiteralPath (Join-Path $PSScriptRoot 'calls.log') -Value "claims update=$Update"
'@
}

function Invoke-FinalLaunch {
    # Runs a copy of the real final script, without -PlanOnly, where the stand-in chains exit with $ChainsExit.
    param([string]$Name, [int]$ChainsExit)
    $root = Join-Path $Script:Root $Name
    $scripts = Join-Path $root 'scripts'
    $bin = Join-Path $root 'bin'
    New-Item -ItemType Directory -Force -Path $scripts, $bin | Out-Null
    Copy-Item -LiteralPath $Script:FinalScript -Destination $scripts
    foreach ($standIn in $Script:LaunchStandIns.GetEnumerator()) {
        [IO.File]::WriteAllText((Join-Path $scripts $standIn.Key), $standIn.Value.Replace('DIGEST', $Script:LaunchDigest))
    }
    Set-Content -LiteralPath (Join-Path $scripts 'chains-exit.txt') -Value "$ChainsExit"
    $digestJson = "{`"code_digest`": `"$($Script:LaunchDigest)`", `"code_commit`": `"$($Script:LaunchCommit)`", `"code_dirty`": false}"
    Set-Content -LiteralPath (Join-Path $bin 'python.cmd') -Value "@echo python %* >>`"$scripts\calls.log`"", "@echo $digestJson"
    $files = [ordered]@{
        cred = @{ path = Join-Path $root 'cred.json'; json = '{"denyNames": ["HOST-OF-RIG"]}' }
        pins = @{ path = Join-Path $root 'pins.json'; json = '{"ssd": {}, "hdd": {}}' }
        pc   = @{ path = Join-Path $root 'pc.json'; json = '{"host": "pc.invalid", "deny_names": ["PC-ITSELF"]}' }
    }
    foreach ($file in $files.Values) { Set-Content -LiteralPath $file.path -Value $file.json }
    $path = $env:PATH
    $env:PATH = "$bin;$path"
    try {
        $output = & (Get-Process -Id $PID).Path -NoProfile -NonInteractive -File (Join-Path $scripts 'run_partition_manager_final_certification.ps1') `
            -ReleaseDir (Join-Path $root 'Release') -CredFile $files.cred.path -PinFile $files.pins.path -PcFile $files.pc.path 2>&1
        $code = $LASTEXITCODE
    } finally {
        $env:PATH = $path
    }
    $log = Join-Path $scripts 'calls.log'
    return [pscustomobject]@{
        code      = $code
        text      = (@($output | ForEach-Object { "$_" }) -join "`n")
        calls     = @(if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log })
        scripts   = $scripts
        root      = $root
        evidence  = Join-Path $root "temp\partition-manager-final\runs\$($Script:LaunchCommit.Substring(0, 12))"
        cred      = $files.cred.path
        pins      = $files.pins.path
        pc        = $files.pc.path
    }
}

function Get-CallIndex {
    # Where in calls.log the first line starting with $Prefix is, or -1.
    param($Launch, [string]$Prefix)
    for ($index = 0; $index -lt $Launch.calls.Count; $index++) {
        if ($Launch.calls[$index].StartsWith($Prefix, [StringComparison]::Ordinal)) { return $index }
    }
    return -1
}

function Get-CallLine {
    param($Launch, [string]$Prefix)
    $index = Get-CallIndex -Launch $Launch -Prefix $Prefix
    if ($index -lt 0) { return '' }
    return $Launch.calls[$index]
}

New-Item -ItemType Directory -Force -Path $Script:Root | Out-Null
try {
    $Script:StandIn = Write-StandIn

    # Both chains pass, at the same time, each in its own process with its own log.
    $both = Invoke-Chains -Name 'both' -Environment @{ SAK_TEST_SLOW = 'vhd,ssd-device' }
    Assert-That ($both.code -eq 0) 'Two chains that pass make a run that passes' $both.text
    foreach ($group in $Script:VmGroups + $Script:PcGroups) {
        Assert-That ($null -ne (Get-Ran -Evidence $both.evidence -Group $group)) "$group ran" $both.text
    }
    $vhd = Get-Ran -Evidence $both.evidence -Group 'vhd'
    $ssd = Get-Ran -Evidence $both.evidence -Group 'ssd-device'
    Assert-That ($vhd.start -lt $ssd.end -and $ssd.start -lt $vhd.end) 'The two chains run at the same time' "vhd $($vhd.start)-$($vhd.end), ssd $($ssd.start)-$($ssd.end)"
    $vmLog = Join-Path $both.evidence 'chain-vm-rig.log'
    $pcLog = Join-Path $both.evidence 'chain-physical-pc.log'
    Assert-That ((Test-Path -LiteralPath $vmLog) -and (Test-Path -LiteralPath $pcLog)) 'Each chain has its own log' $both.text
    $vmText = Get-Content -LiteralPath $vmLog -Raw
    $pcText = Get-Content -LiteralPath $pcLog -Raw
    Assert-That ($vmText -match 'group vhd says hello' -and $vmText -notmatch 'ssd-device says') 'A chain logs its own groups only' $vmText
    Assert-That ($pcText -match 'group ssd-device says hello' -and $pcText -notmatch 'vhd says') 'and so does the other' $pcText
    Assert-That ($both.text -match 'Chain vm-rig: passed' -and $both.text -match 'Chain physical-pc: passed') 'Both results are reported' $both.text
    # Within a chain, in order.
    $data = Get-Ran -Evidence $both.evidence -Group 'data'
    $uefi = Get-Ran -Evidence $both.evidence -Group 'boot-uefi'
    Assert-That ($vhd.end -le $data.start -and $data.end -le $uefi.start) 'A chain runs its groups in order' ''
    Assert-That ($ssd.pin -ceq "'C:\pins.json'" -and $ssd.pc -ceq "'C:\pc.json'" -and $ssd.rows -ceq "'external.one,external.two'") 'A device group gets the pins, the PC and its rows' "$($ssd.pin) $($ssd.pc) $($ssd.rows)"
    Assert-That ($vhd.pin -ceq "''" -and $vhd.pc -ceq "''") 'A VM group gets neither' "$($vhd.pin) $($vhd.pc)"

    # The VM chain fails: it stops there, the PC chain runs on to its end, and both results are reported.
    $vmFails = Invoke-Chains -Name 'vm-fails' -Environment @{ SAK_TEST_FAIL_GROUP = 'data'; SAK_TEST_SLOW = 'hdd-device' }
    Assert-That ($vmFails.code -ne 0) 'A chain that fails fails the run' $vmFails.text
    Assert-That ($null -eq (Get-Ran -Evidence $vmFails.evidence -Group 'boot-uefi')) 'The failed chain stops at the group that failed' $vmFails.text
    foreach ($group in $Script:PcGroups) {
        Assert-That ($null -ne (Get-Ran -Evidence $vmFails.evidence -Group $group)) "The other chain still runs $group" $vmFails.text
    }
    Assert-That ($vmFails.text -match 'Chain vm-rig: FAILED, CHAIN FAILED AT data \(exit 3\)' -and $vmFails.text -match 'Chain physical-pc: passed') 'Both results are reported, the failure by group' $vmFails.text
    Assert-That ((Get-Content -LiteralPath (Join-Path $vmFails.evidence 'chain-vm-rig.log') -Raw) -match 'group data failed on purpose') 'The failed group''s own words are in its chain''s log' ''

    # The PC chain fails at its first group: the VM chain runs all of its own.
    $pcFails = Invoke-Chains -Name 'pc-fails' -Environment @{ SAK_TEST_FAIL_GROUP = 'ssd-device' }
    Assert-That ($pcFails.code -ne 0) 'A device chain that fails fails the run' $pcFails.text
    Assert-That ($null -eq (Get-Ran -Evidence $pcFails.evidence -Group 'hdd-device')) 'It stops at its first failure' $pcFails.text
    foreach ($group in $Script:VmGroups) {
        Assert-That ($null -ne (Get-Ran -Evidence $pcFails.evidence -Group $group)) "The VM chain still runs $group" $pcFails.text
    }
    Assert-That ($pcFails.text -match 'Chain physical-pc: FAILED, CHAIN FAILED AT ssd-device' -and $pcFails.text -match 'Chain vm-rig: passed') 'Both results are reported' $pcFails.text

    # The final run, launched for real: it starts the chains script through -File, with both chains.
    $launch = Invoke-FinalLaunch -Name 'launch-passes' -ChainsExit 0
    $calls = $launch.calls -join "`n"
    $chainsScript = Join-Path $launch.scripts 'run_partition_manager_certification_chains.ps1'
    Assert-That ($launch.code -eq 0) 'A final run whose chains pass passes' "$($launch.text)`n$calls"
    Assert-That ((Get-CallLine -Launch $launch -Prefix 'chains script ') -ieq "chains script $chainsScript") 'The final run starts the chains script itself' "$($launch.text)`n$calls"
    $commandLine = Get-CallLine -Launch $launch -Prefix 'chains command line '
    Assert-That ($commandLine -match "-File\s+`"?$([regex]::Escape($chainsScript))`"?\s") 'It starts it through -File' $commandLine
    $stage = Join-Path $launch.root 'temp\partition-manager-final\stage'
    $expected = "chains plan=$(Join-Path $launch.evidence 'chains.json') evidence=$($launch.evidence) cred=$($launch.cred) stage=$stage pin=$($launch.pins) pc=$($launch.pc)"
    Assert-That ((Get-CallLine -Launch $launch -Prefix 'chains plan=') -ieq $expected) 'It gives it the plan, the evidence, the credentials, the stage, the pins and the PC' "$expected`n$calls"
    Assert-That ((Get-CallLine -Launch $launch -Prefix 'chains groups ') -ceq 'chains groups vm-rig=vhd,data,boot-uefi,boot-bios;physical-pc=ssd-device,hdd-device,data-device') 'The plan holds both chains, each with its groups in order' $calls
    foreach ($step in @("rig Stage pc=''", "rig Deploy pc=''", "rig Deploy pc='$($launch.pc)'", "rig On pc='$($launch.pc)'")) {
        $at = Get-CallIndex -Launch $launch -Prefix $step
        Assert-That ($at -ge 0 -and $at -lt (Get-CallIndex -Launch $launch -Prefix 'chains start')) "It runs $step before the chains" $calls
    }
    # Selection and the claim update come after both chains have ended, in that order.
    $endAt = Get-CallIndex -Launch $launch -Prefix 'chains end 0'
    $selectAt = Get-CallIndex -Launch $launch -Prefix 'select '
    $claimsAt = Get-CallIndex -Launch $launch -Prefix 'claims update=True'
    Assert-That ($endAt -ge 0 -and $selectAt -gt $endAt -and $claimsAt -gt $selectAt) 'Selection follows the chains, and the claim update follows selection' $calls
    $select = Get-CallLine -Launch $launch -Prefix 'select '
    Assert-That ($select -ieq "select run=$($launch.evidence) digest=$($Script:LaunchDigest) deny=HOST-OF-RIG,PC-ITSELF") 'Selection reads this run, at this digest, with every name to deny' $select

    # A chain fails: nothing is selected and the claim is not touched.
    $failed = Invoke-FinalLaunch -Name 'launch-fails' -ChainsExit 1
    $failedCalls = $failed.calls -join "`n"
    Assert-That ($failed.code -ne 0 -and $failed.text -match 'A chain failed') 'A final run whose chain fails fails' "$($failed.text)`n$failedCalls"
    Assert-That ((Get-CallIndex -Launch $failed -Prefix 'chains end 1') -ge 0) 'Its chains ran' $failedCalls
    Assert-That ((Get-CallIndex -Launch $failed -Prefix 'select ') -lt 0 -and (Get-CallIndex -Launch $failed -Prefix 'claims ') -lt 0) 'Nothing is selected and the claim is not updated' $failedCalls
} finally {
    Remove-Item -LiteralPath $Script:Root -Recurse -Force -ErrorAction SilentlyContinue
}

if ($Script:Failures.Count -gt 0) {
    $Script:Failures | ForEach-Object { Write-Host "FAIL $_" }
    throw "$($Script:Failures.Count) chain check(s) failed, $($Script:Passes) held"
}
Write-Host "Certification chains: $($Script:Passes) checks held."

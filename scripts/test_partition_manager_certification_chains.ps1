<#
.SYNOPSIS
    Proves how the final run's chains run, with a stand-in for the group runner.

.DESCRIPTION
    run_partition_manager_certification_chains.ps1 runs the VM rig's groups and the physical PC's device
    groups as two chains at the same time (fix round S, H2; owner ruling 2026-11-06). The stand-in records
    when each group ran and what it was given, and fails or dawdles where the environment says. Nothing
    reaches a VM, a PC or a disk.
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

    # The final run selects nothing unless every chain passed, and only after the chains have ended.
    $final = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'run_partition_manager_final_certification.ps1') -Raw
    $chainsAt = $final.IndexOf('& pwsh @arguments')
    $throwAt = $final.IndexOf('if ($LASTEXITCODE -ne 0) { throw "A chain failed', $chainsAt)
    $selectAt = $final.IndexOf('select_partition_manager_evidence.ps1')
    Assert-That ($chainsAt -gt 0 -and $throwAt -gt $chainsAt -and $selectAt -gt $throwAt) 'Selection follows the chains, and a failed chain stops it' ''
} finally {
    Remove-Item -LiteralPath $Script:Root -Recurse -Force -ErrorAction SilentlyContinue
}

if ($Script:Failures.Count -gt 0) {
    $Script:Failures | ForEach-Object { Write-Host "FAIL $_" }
    throw "$($Script:Failures.Count) chain check(s) failed, $($Script:Passes) held"
}
Write-Host "Certification chains: $($Script:Passes) checks held."

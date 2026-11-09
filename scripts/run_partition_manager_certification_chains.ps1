<#
.SYNOPSIS
    Runs certification groups as chains: the groups of one chain in order, every chain at the same time.

.DESCRIPTION
    Owner ruling 2026-11-06: use the VM and testpc1 in parallel. The final run's VM groups and its physical
    PC's device groups need different machines, so they run as two chains at once, each in its own pwsh with
    its own log beside the evidence (chain-<name>.log). A chain stops at its first group that fails. The other
    chain runs on to its end, and both results are reported; the run fails if either chain failed. Every group
    writes its rows into the one run folder, each in a folder of its own, and each process names its temporary
    files by its own PID, so the chains share none.

.PARAMETER PlanFile
    JSON: an array of chains, each { name, steps: [ { group, rows, device } ] }.

.PARAMETER Chain
    Runs that one chain here, in this process. The final run leaves it empty, and each chain is started with it.

.PARAMETER GroupsScript
    The group runner each step calls. A test points it at a stand-in.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$PlanFile,
    [Parameter(Mandatory = $true)] [string]$EvidenceDir,
    [string]$CredFile = '',
    [string]$StageDir = '',
    [string]$PinFile = '',
    [string]$PcFile = '',
    [string]$GroupsScript = '',
    [string]$Chain = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'process_capture.ps1')
if (-not $GroupsScript) { $GroupsScript = Join-Path $PSScriptRoot 'run_partition_manager_certification_groups.ps1' }
$Script:Plan = @(Get-Content -LiteralPath $PlanFile -Raw | ConvertFrom-Json)
$Script:Shell = (Get-Process -Id $PID).Path

function Get-ChainLog {
    param([string]$Name)
    return (Join-Path $EvidenceDir "chain-$Name.log")
}

function Write-ChainLog {
    param([string]$Log, [string]$Text)
    Add-Content -LiteralPath $Log -Value "=== [$(Get-Date -Format HH:mm:ss)] $Text"
}

function Get-StepArguments {
    param($Step)
    $arguments = @('-NoProfile', '-NonInteractive', '-File', $GroupsScript, '-Group', "$($Step.group)", '-EvidenceDir', $EvidenceDir)
    if ($CredFile) { $arguments += @('-CredFile', $CredFile) }
    if ($StageDir) { $arguments += @('-StageDir', $StageDir) }
    if (@($Step.rows).Count -gt 0 -and $Step.rows) { $arguments += @('-Rows', (@($Step.rows) -join ',')) }
    if ($Step.device) {
        $arguments += @('-PinFile', $PinFile)
        if ($PcFile) { $arguments += @('-PcFile', $PcFile) }
    }
    return $arguments
}

function Invoke-OneChain {
    # This process is the chain: its groups in order, stopping at the first that fails.
    param($Entry)
    $log = Get-ChainLog -Name "$($Entry.name)"
    foreach ($step in @($Entry.steps)) {
        Write-ChainLog -Log $log -Text "Group $($step.group)"
        $arguments = Get-StepArguments -Step $step
        & $Script:Shell @arguments *>> $log
        $code = $LASTEXITCODE
        if ($code -ne 0) {
            Write-ChainLog -Log $log -Text "CHAIN FAILED AT $($step.group) (exit $code)"
            exit 1
        }
    }
    Write-ChainLog -Log $log -Text 'CHAIN PASSED'
    exit 0
}

function Start-Chain {
    # Not redirected: the chain writes its own log, and an unredirected child's exit code is reliable.
    param($Entry)
    $log = Get-ChainLog -Name "$($Entry.name)"
    Set-Content -LiteralPath $log -Value "chain $($Entry.name): $((@($Entry.steps) | ForEach-Object { "$($_.group)" }) -join ', ')"
    $arguments = @('-NoProfile', '-NonInteractive', '-File', $PSCommandPath, '-PlanFile', $PlanFile, '-EvidenceDir', $EvidenceDir, '-Chain', "$($Entry.name)", '-GroupsScript', $GroupsScript)
    foreach ($pair in @(@('-CredFile', $CredFile), @('-StageDir', $StageDir), @('-PinFile', $PinFile), @('-PcFile', $PcFile))) {
        if ($pair[1]) { $arguments += $pair }
    }
    $info = [Diagnostics.ProcessStartInfo]::new($Script:Shell)
    $info.Arguments = Join-ProcessArguments $arguments
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    return [pscustomobject]@{ name = "$($Entry.name)"; log = $log; process = [Diagnostics.Process]::Start($info) }
}

if ($Chain) {
    $entry = @($Script:Plan | Where-Object { "$($_.name)" -ceq $Chain })
    if ($entry.Count -ne 1) { throw "The plan has no chain named $Chain" }
    Invoke-OneChain -Entry $entry[0]
}

New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null
$started = Get-Date
$running = @(foreach ($entry in $Script:Plan) { Start-Chain -Entry $entry })
"=== [$(Get-Date -Format HH:mm:ss)] Started $($running.Count) chain(s): $(($running | ForEach-Object { $_.name }) -join ', ')"
# Every chain runs to its own end: a chain that fails does not stop the others.
foreach ($run in $running) { $run.process.WaitForExit() }
$failed = 0
foreach ($run in $running) {
    $code = $run.process.ExitCode
    $last = @(Get-Content -LiteralPath $run.log | Where-Object { $_ -match 'CHAIN (PASSED|FAILED)' } | Select-Object -Last 1)
    $said = if ($last.Count) { $last[0] -replace '^=== \[[^\]]*\] ', '' } else { 'it wrote no result' }
    if ($code -ne 0) { $failed++ }
    "Chain $($run.name): $(if ($code -eq 0) { 'passed' } else { "FAILED, $said" }) (exit $code). Log: $($run.log)"
}
"=== [$(Get-Date -Format HH:mm:ss)] All chains ended after $([int]((Get-Date) - $started).TotalMinutes) minutes"
if ($failed -gt 0) { exit 1 }
exit 0

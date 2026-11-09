<#
.SYNOPSIS
    The final Partition Manager certification run: every row, on one frozen commit.

.DESCRIPTION
    Refuses a tree with uncommitted code, or binaries not built from it. Stages those binaries,
    runs every group, checks every row, selects the run as current, and regenerates the state block.
    The groups run as chains (run_partition_manager_certification_chains.ps1): with -PcFile, the VM
    rig's groups and the physical PC's device groups are two chains that run at the same time (owner,
    2026-11-06); otherwise one chain. A chain stops at its first group that fails, the other runs on to
    its end, and both results are reported. Selection and the state block run only once both have
    ended, and only when both passed. It writes evidence and that block, and commits
    nothing: docs and evidence may change after a freeze, code may not.

    Without -PinFile the device rows run on virtual disks, except the one only a real hard drive can
    run, and the claim cannot rise past VirtualMachinesValidated. With -PinFile they run on the pinned devices, and only their rows
    touch those devices. Make the pin file with the rig's WritePins just before the run, from the
    machine that holds them. The steps are in docs/PARTITION_MANAGER_CERTIFICATION.md.

    With -PcFile as well, those devices are on the physical PC that file names, reached over SSH. The
    other rows still run on the VM rig, because they need its throwaway disks and its VMs.

    Run it from the host, with the VM signed in. It changes no disk of this machine.

.PARAMETER ReleaseDir
    The build's Release folder, built from this commit.

.PARAMETER PinFile
    The pinned devices, by model, size, serial and typed confirmation. Holds serial numbers, so it
    stays out of the repository.

.PARAMETER PcFile
    The physical PC that holds the pinned devices: host, key, machine_uuid, confirmation, and optional
    port, jump and deny_names. Prepare it once with the rig's PcPrepare. It stays out of the repository.

.PARAMETER RunName
    A name for this run's evidence folder. It defaults to the commit.

.PARAMETER PlanOnly
    Prints what would run on what, and stops before it reads the tree or reaches any machine.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$ReleaseDir,
    [string]$CredFile = 'temp\vm-auth\sak-capture-technician.json',
    [string]$PinFile = '',
    [string]$PcFile = '',
    [string]$RunName = '',
    [string]$WorkDir = 'temp\partition-manager-final',
    [switch]$PlanOnly
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$Rig = Join-Path $PSScriptRoot 'run_partition_manager_certification_rig.ps1'
$Chains = Join-Path $PSScriptRoot 'run_partition_manager_certification_chains.ps1'

function Resolve-FinalPath {
    param([string]$Path)
    if ([IO.Path]::IsPathRooted($Path)) { return $Path }
    return (Join-Path $ProjectRoot $Path)
}

function Say {
    param([string]$Text)
    "=== [$(Get-Date -Format HH:mm:ss)] $Text"
}

if ($PcFile -and -not $PinFile) { throw '-PcFile needs -PinFile: it says where the pinned devices are' }

# The data rows the owner named for the hard drive run there. The rest need a virtual disk or a VDI.
$deviceDataRows = @('external.allocate-free-space', 'external.partition-move', 'external.cluster-size-change', 'external.volume-serial-number')
$plan = @(@{ group = 'vhd' })
if ($PinFile) {
    $plan += @{ group = 'data'; rows = @('external.file-level-data-recovery', 'external.bitlocker', 'external.bitlocker-mutation', 'external.primary-logical-conversion', 'external.dynamic-to-basic') }
    $plan += @(@{ group = 'boot-uefi' }, @{ group = 'boot-bios' }, @{ group = 'ssd-device'; device = $true }, @{ group = 'hdd-device'; device = $true }, @{ group = 'data-device'; device = $true; rows = $deviceDataRows })
} else {
    $plan += @(@{ group = 'data' }, @{ group = 'boot-uefi' }, @{ group = 'boot-bios' }, @{ group = 'ssd-virtual' }, @{ group = 'usb-virtual' })
}
# The device groups need the physical PC and the others the VM rig, so with a PC they are two chains.
# @(): a lone chain must stay a list, or Count counts its keys.
$chains = @(if ($PcFile) {
    @([ordered]@{ name = 'vm-rig'; steps = @($plan | Where-Object { -not $_.device }) },
        [ordered]@{ name = 'physical-pc'; steps = @($plan | Where-Object { $_.device }) })
} else {
    @([ordered]@{ name = 'vm-rig'; steps = $plan })
})
if ($PlanOnly) {
    foreach ($step in $plan) {
        $where = if ($step.device -and $PcFile) { 'physical PC' } else { 'VM rig' }
        "$($step.group) on $where$(if ($step.device) { ', pinned' }): $(if ($step.rows) { $step.rows -join ',' } else { 'every row' })"
    }
    foreach ($chain in $chains) { "Chain $($chain.name): $((@($chain.steps) | ForEach-Object { $_.group }) -join ', ')" }
    if ($chains.Count -gt 1) { 'The chains run at the same time. Selection and the state block wait for both.' }
    return
}

$code = (& python (Join-Path $PSScriptRoot 'partition_manager_code_digest.py') --root $ProjectRoot) | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw 'The code digest could not be worked out' }
if ($code.code_dirty) { throw 'The tree has uncommitted code. Commit it, and build again, before a final run' }

$work = Resolve-FinalPath $WorkDir
$stage = Join-Path $work 'stage'
$runName = if ($RunName) { $RunName } else { "$($code.code_commit)".Substring(0, 12) }
$evidence = Join-Path (Join-Path $work 'runs') $runName
$credentials = Resolve-FinalPath $CredFile
New-Item -ItemType Directory -Force -Path $evidence | Out-Null

Say 'Stage the binaries of this commit'
& pwsh -NoProfile -File $Rig -Action Stage -CredFile $credentials -ReleaseDir (Resolve-FinalPath $ReleaseDir) -StageDir $stage
if ($LASTEXITCODE -ne 0) { throw 'Staging failed' }
# The digest is compiled into the step client. Read it from the file: this PC does not run it.
$client = [IO.File]::ReadAllText((Join-Path $stage 'sak_utility.exe'), [Text.Encoding]::Latin1)
if (-not $client.Contains("$($code.code_digest)")) {
    throw "The step client was not built from this tree, $($code.code_digest). Build this commit first"
}
& pwsh -NoProfile -File $Rig -Action Deploy -CredFile $credentials -StageDir $stage
if ($LASTEXITCODE -ne 0) { throw 'Deploying to the VM failed' }

$pc = ''
$deny = @()
if ($PcFile) {
    $pc = Resolve-FinalPath $PcFile
    & pwsh -NoProfile -File $Rig -Action Deploy -PcFile $pc -StageDir $stage
    if ($LASTEXITCODE -ne 0) { throw 'Deploying to the physical PC failed' }
    & pwsh -NoProfile -File $Rig -Action On -PcFile $pc
    if ($LASTEXITCODE -ne 0) { throw 'The physical PC is not ready' }
    $deny = @((Get-Content -LiteralPath $pc -Raw | ConvertFrom-Json).deny_names | Where-Object { $_ })
}

$pins = ''
if ($PinFile) {
    $pins = Resolve-FinalPath $PinFile
    if (-not (Test-Path -LiteralPath $pins)) { throw "No pin file at $pins" }
}

$started = Get-Date
$planFile = Join-Path $evidence 'chains.json'
ConvertTo-Json -Depth 6 -InputObject @($chains) | Set-Content -LiteralPath $planFile -Encoding UTF8
Say "Chains: $(($chains | ForEach-Object { "$($_.name) ($((@($_.steps) | ForEach-Object { $_.group }) -join ', '))" }) -join '; ')"
$arguments = @('-NoProfile', '-File', $Chains, '-PlanFile', $planFile, '-EvidenceDir', $evidence, '-CredFile', $credentials, '-StageDir', $stage)
if ($pins) { $arguments += @('-PinFile', $pins) }
if ($pc) { $arguments += @('-PcFile', $pc) }
& pwsh @arguments
if ($LASTEXITCODE -ne 0) { throw "A chain failed; both results are above, each chain's log is in $evidence, and nothing was selected" }
Say "All groups ran in $([int]((Get-Date) - $started).TotalMinutes) minutes"

Say 'Check every row, and select this run as current'
$cred = Get-Content -LiteralPath $credentials -Raw | ConvertFrom-Json
& (Join-Path $PSScriptRoot 'select_partition_manager_evidence.ps1') -RunDir $evidence -DenyNames (@($cred.denyNames) + $deny) -CodeDigest $code.code_digest

Say 'Regenerate the state block'
& (Join-Path $PSScriptRoot 'check_partition_manager_release_claims.ps1') -Update

Say 'Done. Read the claim and its words in docs/PARTITION_MANAGER_CERTIFICATION.md, commit the evidence and the block, and tag'

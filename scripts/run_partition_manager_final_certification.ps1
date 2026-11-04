<#
.SYNOPSIS
    The final Partition Manager certification run: every row, on one frozen commit.

.DESCRIPTION
    Refuses a tree with uncommitted code, or binaries not built from it. Stages those binaries,
    runs every group in order, stops at the first group that fails, checks every row, selects the
    run as current, and regenerates the state block. It writes evidence and that block, and commits
    nothing: docs and evidence may change after a freeze, code may not.

    Without -PinFile the device rows run on virtual disks, and the claim cannot rise past
    VirtualMachinesValidated. With -PinFile they run on the pinned devices, and only their rows
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
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$ReleaseDir,
    [string]$CredFile = 'temp\vm-auth\sak-capture-technician.json',
    [string]$PinFile = '',
    [string]$PcFile = '',
    [string]$RunName = '',
    [string]$WorkDir = 'temp\partition-manager-final'
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$Rig = Join-Path $PSScriptRoot 'run_partition_manager_certification_rig.ps1'
$Groups = Join-Path $PSScriptRoot 'run_partition_manager_certification_groups.ps1'

function Resolve-FinalPath {
    param([string]$Path)
    if ([IO.Path]::IsPathRooted($Path)) { return $Path }
    return (Join-Path $ProjectRoot $Path)
}

function Say {
    param([string]$Text)
    "=== [$(Get-Date -Format HH:mm:ss)] $Text"
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

if ($PcFile -and -not $PinFile) { throw '-PcFile needs -PinFile: it says where the pinned devices are' }
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

# The data rows the owner named for the hard drive run there. The rest need a virtual disk or a VDI.
$deviceDataRows = @('external.allocate-free-space', 'external.partition-move', 'external.cluster-size-change', 'external.volume-serial-number')
$plan = @(@{ group = 'vhd' })
if ($PinFile) {
    $pins = Resolve-FinalPath $PinFile
    if (-not (Test-Path -LiteralPath $pins)) { throw "No pin file at $pins" }
    $plan += @{ group = 'data'; rows = @('external.file-level-data-recovery', 'external.bitlocker', 'external.bitlocker-mutation', 'external.primary-logical-conversion', 'external.dynamic-to-basic') }
    $plan += @(@{ group = 'boot-uefi' }, @{ group = 'boot-bios' }, @{ group = 'ssd-device'; device = $true }, @{ group = 'hdd-device'; device = $true }, @{ group = 'data-device'; device = $true; rows = $deviceDataRows })
} else {
    $pins = ''
    $plan += @(@{ group = 'data' }, @{ group = 'boot-uefi' }, @{ group = 'boot-bios' }, @{ group = 'ssd-virtual' }, @{ group = 'hdd-virtual' }, @{ group = 'usb-virtual' })
}

$started = Get-Date
foreach ($step in $plan) {
    $group = $step.group
    Say "Group $group"
    $arguments = @('-NoProfile', '-File', $Groups, '-Group', $group, '-EvidenceDir', $evidence, '-CredFile', $credentials, '-StageDir', $stage)
    if ($step.rows) { $arguments += @('-Rows', ($step.rows -join ',')) }
    if ($step.device) {
        $arguments += @('-PinFile', $pins)
        if ($pc) { $arguments += @('-PcFile', $pc) }
    }
    & pwsh @arguments
    if ($LASTEXITCODE -ne 0) { throw "Group $group failed. Its evidence is in $evidence" }
}
Say "All groups ran in $([int]((Get-Date) - $started).TotalMinutes) minutes"

Say 'Check every row, and select this run as current'
$cred = Get-Content -LiteralPath $credentials -Raw | ConvertFrom-Json
& (Join-Path $PSScriptRoot 'select_partition_manager_evidence.ps1') -RunDir $evidence -DenyNames (@($cred.denyNames) + $deny) -CodeDigest $code.code_digest

Say 'Regenerate the state block'
& (Join-Path $PSScriptRoot 'check_partition_manager_release_claims.ps1') -Update

Say 'Done. Read the claim and its words in docs/PARTITION_MANAGER_CERTIFICATION.md, commit the evidence and the block, and tag'

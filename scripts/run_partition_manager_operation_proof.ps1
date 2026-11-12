<#
.SYNOPSIS
    Runs the Partition Manager operation proof on one machine, with one command, from a workstation
    that changes no disk.

.DESCRIPTION
    Owner ruling 2026-11-11: every operation a person can reach in Partition Manager, and every other
    feature that runs a generated script elevated, runs end to end through the app before a release,
    and a separate tool reads back what it did. scripts/partition_manager_operation_proof_plan.psd1
    lists the rows, where each runs and what reads it back; -PlanOnly prints that and stops.

    The rows are scripts/partition_manager_operation_proof_rows.ps1, run by the certification runner
    (run_partition_manager_app_certification.ps1 -OperationProof) on the machine, through the step
    client and the real elevated helper. What this adds to the certification machinery is kept apart
    from it: rows are named ops.*, which the certification matrix never names; evidence goes under
    -EvidenceDir, which the evidence selector never reads; and the claim level never sees it. A
    certification row named here (vhd.* or external.*) runs again as a regression in the same way and
    is judged by the matrix's own checks, without the release binding a pre-merge build cannot meet.

    On testpc2 it stages the package and the step client on the spare NVMe, runs the rows in the
    signed-in session through a scheduled task of its own, brings the evidence and the image copies
    back, reads each image again with Linux's own e2fsprogs and apfsprogs through WSL on this
    workstation (files only, under -EvidenceDir), and writes the results table.

    Actions:
      Run       Preflight, stage, run, fetch, verify and report. The default.
      Claim     Writes this run's lines to the PC's claim file, after reading every other claim.
      Release   Removes this run's lines from the PC's claim file.
      Report    Rebuilds the results table from evidence already fetched.

.PARAMETER Machine
    testpc2 (virtual disk files on its spare NVMe) or vm (the VirtualBox guest; not yet driven here).
.PARAMETER PackageZip
    A release package zip. Its helper, tools and DLLs are what runs.
.PARAMETER PackageSha256
    The package's SHA-256, checked here and again on the PC.
.PARAMETER ClientExe
    partition_manager_live_certifier.exe from the same build as the package: the step client.
.PARAMETER ProbeExe
    partition_filesystem_probe_certifier.exe from the same build.
.PARAMETER BuildLabel
    What the build is, in words, for the results table (for example lane2-b24).
.PARAMETER MachineFile
    testpc2: a JSON file outside the repository naming the PC and the one drive this may write,
    because serial numbers and machine ids stay out of it:
    { "name": "TESTPC2", "uuid": "...", "spare_model": "...", "spare_serial": "...", "spare_bytes": "..." }
    It goes to the PC beside the work folder too, where every row checks it again.
.PARAMETER VmCredFile
    vm: the guest's credentials file (AGENTS.local.md says where it is). Never in the repository.
#>

[CmdletBinding()]
param(
    [ValidateSet('Run', 'Claim', 'Release', 'Report')] [string]$Action = 'Run',
    [ValidateSet('testpc2', 'vm')] [string]$Machine = 'testpc2',
    [string[]]$Rows = @(),
    [string]$PackageZip = '',
    [string]$PackageSha256 = '',
    [string]$ClientExe = '',
    [string]$ProbeExe = '',
    # vm: a build's Release folder (step client, helper, DLLs, smartctl), staged by the rig.
    [string]$ReleaseDir = '',
    [string]$VmSnapshot = 'pre-round8',
    [string]$VmCredFile = 'temp\vm-auth\sak-capture-technician.json',
    [string]$MachineFile = 'E:\SAK-Utility-Test-Scratch\opsproof\testpc2.machine.json',
    [string]$BuildLabel = '',
    [string]$EvidenceDir = '',
    [string]$ResultsPath = 'E:\SAK-Utility-Test-Scratch\opsproof\results.md',
    [string]$SshHost = 'testpc2',
    [string]$RemoteRoot = 'W:\opsproof',
    [int]$JobMinutes = 600,
    [string]$ClaimUntil = '',
    [switch]$SkipHostChecks,
    # vm: start the BIOS rows from the BIOS install an earlier run left on the rig (over an hour to build).
    [switch]$ReuseBiosDisk,
    # Also reads each HFS+ and APFS image with Apple's own fsck on the rig's Mac (read-only attach).
    [switch]$MacChecks,
    [switch]$PlanOnly
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
# Under pwsh -File a list arrives as one comma-joined string.
$Rows = @($Rows | ForEach-Object { "$_" -split ',' } | Where-Object { $_ })
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$Plan = Import-PowerShellDataFile -LiteralPath (Join-Path $PSScriptRoot 'partition_manager_operation_proof_plan.psd1')
if (-not [IO.Path]::IsPathRooted($VmCredFile)) { $VmCredFile = Join-Path $ProjectRoot $VmCredFile }
$Script:Pc = $null
$Script:ClaimTag = 'opsproof'
$Script:RunnerTask = 'SAKOpsProofRunner'

function Get-OpsPlanRows {
    param([string]$For = '')
    return @($Plan.Rows | Where-Object { -not $For -or $_.Machine -eq $For })
}

function Get-OpsBodyIds {
    # Every row id a body is written for: ops.* in the operation-proof rows, and the certification rows.
    $ids = @()
    foreach ($file in 'partition_manager_operation_proof_rows.ps1', 'partition_manager_certification_row_bodies.ps1') {
        $text = Get-Content -LiteralPath (Join-Path $PSScriptRoot $file) -Raw
        $ids += [regex]::Matches($text, "Add-OpsRow '([^']+)'") | ForEach-Object { $_.Groups[1].Value }
        $ids += [regex]::Matches($text, "CertRowBodies\['([^']+)'\]\s*=") | ForEach-Object { $_.Groups[1].Value }
    }
    return @($ids | Sort-Object -Unique)
}

function Assert-OpsPlanConsistent {
    $bodies = Get-OpsBodyIds
    $problems = @()
    foreach ($row in $Plan.Rows) {
        if ($row.Pending) { continue }
        if ($bodies -cnotcontains $row.Id) { $problems += "plan row $($row.Id) has no body" }
    }
    foreach ($id in @($bodies | Where-Object { $_ -like 'ops.*' })) {
        if (@($Plan.Rows | Where-Object { $_.Id -ceq $id }).Count -ne 1) { $problems += "body $id is not in the plan once" }
    }
    if ($problems.Count -gt 0) { throw "The operation-proof plan and its rows disagree: $($problems -join '; ')" }
}

function Show-OpsPlan {
    $lines = @('Operation proof plan', '')
    foreach ($group in ($Plan.Rows | Group-Object Machine)) {
        $lines += "== $($group.Name) ($($group.Count) rows)"
        foreach ($row in $group.Group) {
            $state = if ($row.Pending) { "PENDING: $($row.Pending)" } else { 'ready' }
            $lines += ('  {0,-40} {1} [{2}]' -f $row.Id, "$($row.Operation): $($row.Variant)", $state)
            $lines += "      reads back: $($row.Reads)"
        }
        $lines += ''
    }
    $lines += '== not runnable'
    foreach ($item in $Plan.NotRunnable) { $lines += "  $($item.Operation): $($item.Reason)" }
    return $lines
}

Assert-OpsPlanConsistent
if ($PlanOnly) { Show-OpsPlan; exit 0 }

function Invoke-OpsVmRun {
    <#
    .SYNOPSIS
        The VM rows: staged and deployed by the rig, run by the groups runner's ops-vm group, which
        restarts the guest, attaches throwaway disks and starts a migrated disk alone. Afterwards
        the guest goes back to its snapshot and is powered off, whatever happened.
    #>
    param([string[]]$RowIds, [string]$LocalDir)
    if (-not $ReleaseDir -or -not (Test-Path -LiteralPath (Join-Path $ReleaseDir 'partition_manager_live_certifier.exe'))) { throw 'The VM rows need -ReleaseDir with the step client in it' }
    $rig = Join-Path $PSScriptRoot 'run_partition_manager_certification_rig.ps1'
    $groups = Join-Path $PSScriptRoot 'run_partition_manager_certification_groups.ps1'
    $stage = Join-Path (Split-Path -Parent $ResultsPath) "vm-stage-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
    $failure = $null
    try {
        & pwsh -NoProfile -File $rig -Action Stage -ReleaseDir $ReleaseDir -StageDir $stage -CredFile $VmCredFile
        if ($LASTEXITCODE -ne 0) { throw 'The rig could not stage the build' }
        & pwsh -NoProfile -File $rig -Action On -CredFile $VmCredFile
        if ($LASTEXITCODE -ne 0) { throw 'The VM did not start and sign in' }
        & pwsh -NoProfile -File $rig -Action Deploy -StageDir $stage -CredFile $VmCredFile
        if ($LASTEXITCODE -ne 0) { throw 'The rig could not deploy to the VM' }
        & pwsh -NoProfile -File $groups -Group 'ops-vm' -Rows ($RowIds -join ',') -EvidenceDir $LocalDir -StageDir $stage -CredFile $VmCredFile @(if ($ReuseBiosDisk) { '-ReuseBiosDisk' })
        if ($LASTEXITCODE -ne 0) { $failure = "the ops-vm group exited $LASTEXITCODE" }
    } finally {
        & pwsh -NoProfile -File $rig -Action Off -CredFile $VmCredFile
        & pwsh -NoProfile -File $rig -Action Restore -SnapshotName $VmSnapshot -CredFile $VmCredFile
    }
    if ($failure) { Write-Warning $failure }
}

# --- The PC ---------------------------------------------------------------------------------------

function Read-OpsMachineFile {
    # The PC and its one writable drive, from a file a person wrote, outside the repository.
    if (-not (Test-Path -LiteralPath $MachineFile -PathType Leaf)) { throw "testpc2 runs need -MachineFile: $MachineFile is not there" }
    $machine = Get-Content -LiteralPath $MachineFile -Raw | ConvertFrom-Json
    foreach ($field in 'name', 'uuid', 'spare_model', 'spare_serial', 'spare_bytes') {
        if ([string]::IsNullOrWhiteSpace("$($machine.$field)")) { throw "$MachineFile has no $field" }
    }
    $here = "$((Get-CimInstance -ClassName Win32_ComputerSystemProduct).UUID)".Trim().ToUpperInvariant()
    if ("$($machine.uuid)".ToUpperInvariant() -ceq $here) { throw "$MachineFile names the machine this runs on" }
    return @{ Name = "$($machine.name)"; Uuid = "$($machine.uuid)".ToUpperInvariant(); SpareModel = "$($machine.spare_model)"; SpareSerial = "$($machine.spare_serial)"; SpareBytes = "$($machine.spare_bytes)" }
}

if ($Machine -eq 'testpc2' -and $Action -ne 'Report') { $Script:Pc = Read-OpsMachineFile }

$Script:Scratch = Join-Path ([IO.Path]::GetTempPath()) "opsproof-$PID"
New-Item -ItemType Directory -Force -Path $Script:Scratch | Out-Null

function Invoke-OpsSsh {
    param([string[]]$Arguments)
    $output = & ssh -o BatchMode=yes -o ConnectTimeout=20 -o ServerAliveInterval=30 $SshHost @Arguments 2>&1
    $code = $LASTEXITCODE
    $text = @($output | ForEach-Object { "$_" } | Where-Object { $_ -notmatch '^\*\* ' })
    return [pscustomobject]@{ code = $code; text = $text }
}

function Invoke-OpsRemote {
    <#
    .SYNOPSIS
        Runs a PowerShell script on the PC: sent as a file, because a multi-line -Command loses lines.
    #>
    param([Parameter(Mandatory = $true)] [string]$Code, [switch]$AllowFailure)
    $name = "remote-$([Guid]::NewGuid().ToString('N').Substring(0, 8)).ps1"
    $local = Join-Path $Script:Scratch $name
    # The script is read whole before it runs, so it removes its own file first.
    $guard = "Remove-Item -LiteralPath `$PSCommandPath -ErrorAction SilentlyContinue`n`$ErrorActionPreference = 'Stop'`n`$ProgressPreference = 'SilentlyContinue'`nif (`$env:COMPUTERNAME -ne '$($Script:Pc.Name)') { throw 'This is not $($Script:Pc.Name)' }`n"
    [IO.File]::WriteAllText($local, $guard + $Code)
    $remoteDir = "$RemoteRoot\jobs"
    $made = Invoke-OpsSsh -Arguments @("powershell -NoProfile -Command `"New-Item -ItemType Directory -Force -Path '$remoteDir' | Out-Null`"")
    if ($made.code -ne 0) { throw "Could not reach $SshHost`: $($made.text -join ' ')" }
    & scp -q -o BatchMode=yes $local "${SshHost}:$($remoteDir.Replace('\', '/'))/$name" 2>&1 | Where-Object { "$_" -notmatch '^\*\* ' } | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Could not copy $name to $SshHost" }
    $result = Invoke-OpsSsh -Arguments @("powershell -NoProfile -ExecutionPolicy Bypass -File `"$remoteDir\$name`"")
    Remove-Item -LiteralPath $local -Force
    if ($result.code -ne 0 -and -not $AllowFailure) { throw "The script on $SshHost failed ($($result.code)): $($result.text -join ' | ')" }
    return $result.text
}

function Send-OpsFile {
    param([string]$Local, [string]$RemoteDir)
    & scp -q -o BatchMode=yes $Local "${SshHost}:$($RemoteDir.Replace('\', '/'))/" 2>&1 | Where-Object { "$_" -notmatch '^\*\* ' } | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Could not copy $Local to $SshHost" }
}

function Get-OpsClaimLines {
    $until = if ($ClaimUntil) { $ClaimUntil } else { (Get-Date).AddHours(12).ToString('yyyy-MM-dd HH:mm') }
    $start = (Get-Date).ToString('yyyy-MM-dd HH:mm')
    return @(
        "drive $($Script:Pc.SpareSerial) $Script:ClaimTag (S.A.K. Utility operation proof): virtual disk files under $RemoteRoot only; start $start; expected end $until",
        "console $Script:ClaimTag (S.A.K. Utility operation proof): the step client runs hidden in Technician's session via scheduled tasks; start $start; expected end $until",
        "cpu $Script:ClaimTag (S.A.K. Utility operation proof): rows and image copies; start $start; expected end $until"
    )
}

function Invoke-OpsPreflight {
    # The PC is the one named, nobody else holds what this needs, and the spare is where it was.
    $code = @"
`$uuid = "`$((Get-CimInstance Win32_ComputerSystemProduct).UUID)".Trim().ToUpperInvariant()
if (`$uuid -ne '$($Script:Pc.Uuid)') { throw "This PC's id is `$uuid" }
`$session = (quser 2>&1 | Out-String)
if (`$session -notmatch '(?i)technician\s+console') { throw "Technician is not signed in at the console: `$session" }
`$conflicts = @()
foreach (`$file in @(Get-ChildItem -LiteralPath 'C:\TESTLAB_CLAIMS' -Filter '*.txt' -ErrorAction SilentlyContinue)) {
    foreach (`$line in @(Get-Content -LiteralPath `$file.FullName)) {
        if (-not `$line.Trim() -or `$line -match ' $Script:ClaimTag ') { continue }
        if (`$line -match '^(drive\s+$($Script:Pc.SpareSerial)|console|reboot)') { `$conflicts += "`$(`$file.Name): `$line" }
    }
}
if (`$conflicts.Count -gt 0) { throw "Another claim holds what this run needs: `$(`$conflicts -join ' || ')" }
`$volume = Get-Partition -DriveLetter '$($RemoteRoot.Substring(0, 1))' -ErrorAction Stop
`$disk = Get-Disk -Number `$volume.DiskNumber
`$serial = "`$(`$disk.SerialNumber)".Trim().TrimEnd('.')
if ("`$(`$disk.FriendlyName)" -cne '$($Script:Pc.SpareModel)' -or `$serial -cne '$($Script:Pc.SpareSerial)' -or "`$(`$disk.Size)" -ne '$($Script:Pc.SpareBytes)') {
    throw "$RemoteRoot is on `$(`$disk.FriendlyName) `$serial `$(`$disk.Size), not the spare"
}
"PIN spare: `$(`$disk.FriendlyName) `$serial `$(`$disk.Size) disk `$(`$disk.Number)"
"FREE: `$([math]::Round((Get-Volume -DriveLetter '$($RemoteRoot.Substring(0, 1))').SizeRemaining / 1GB, 1)) GB"
"BOOT: `$((Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o'))"
"@
    return Invoke-OpsRemote -Code $code
}

function Set-OpsClaim {
    param([switch]$Remove)
    $lines = if ($Remove) { @() } else { Get-OpsClaimLines }
    $encoded = ($lines | ForEach-Object { "'" + $_.Replace("'", "''") + "'" }) -join ','
    $code = @"
New-Item -ItemType Directory -Force -Path 'C:\TESTLAB_CLAIMS' | Out-Null
`$path = 'C:\TESTLAB_CLAIMS\SAK-Utility.txt'
`$kept = @(if (Test-Path -LiteralPath `$path) { Get-Content -LiteralPath `$path | Where-Object { `$_ -notmatch ' $Script:ClaimTag ' } })
`$mine = @($encoded)
`$all = @(`$kept + `$mine | Where-Object { `$_ })
if (`$all.Count -eq 0) { if (Test-Path -LiteralPath `$path) { Remove-Item -LiteralPath `$path } } else { Set-Content -LiteralPath `$path -Value `$all -Encoding ASCII }
if (Test-Path -LiteralPath `$path) { Get-Content -LiteralPath `$path } else { '(no claim file)' }
"@
    return Invoke-OpsRemote -Code $code
}

function Get-OpsSha256 {
    param([string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Invoke-OpsStage {
    <#
    .SYNOPSIS
        Puts the package, the step client in place of sak_utility.exe, the probe and the scripts on the
        PC, each checked by hash on both sides.
    #>
    foreach ($required in 'PackageZip', 'PackageSha256', 'ClientExe', 'ProbeExe', 'BuildLabel') {
        if (-not (Get-Variable -Name $required -ValueOnly)) { throw "Run needs -$required" }
    }
    $zipHash = Get-OpsSha256 $PackageZip
    if ($zipHash -ne $PackageSha256.ToLowerInvariant()) { throw "$PackageZip is $zipHash, not $PackageSha256" }
    $clientHash = Get-OpsSha256 $ClientExe
    $probeHash = Get-OpsSha256 $ProbeExe
    $stageName = "stage-$($zipHash.Substring(0, 12))-$($clientHash.Substring(0, 12))"
    $incoming = "$RemoteRoot\incoming"
    Invoke-OpsRemote -Code "New-Item -ItemType Directory -Force -Path '$incoming', '$RemoteRoot\scripts' | Out-Null; Get-ChildItem -LiteralPath '$RemoteRoot\scripts' -File | Remove-Item -Force" | Out-Null
    # Every row reads the PC's name and its one writable drive from here, beside the work folder.
    $machineCopy = Join-Path $Script:Scratch 'machine.json'
    Copy-Item -LiteralPath $MachineFile -Destination $machineCopy
    Send-OpsFile -Local $machineCopy -RemoteDir $RemoteRoot
    # A file already there with the same hash is not sent again; the PC checks every hash below anyway.
    $there = Invoke-OpsRemote -Code "Get-ChildItem -LiteralPath '$incoming' -File | ForEach-Object { '{0} {1}' -f `$_.Name, (Get-FileHash -LiteralPath `$_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }"
    foreach ($pair in @(@($PackageZip, $zipHash), @($ClientExe, $clientHash), @($ProbeExe, $probeHash))) {
        if ($there -ccontains "$(Split-Path -Leaf $pair[0]) $($pair[1])") { continue }
        Send-OpsFile -Local $pair[0] -RemoteDir $incoming
    }
    $scripts = @('run_partition_manager_app_certification.ps1', 'partition_manager_certification_row_bodies.ps1', 'partition_manager_readback.ps1',
        'partition_manager_row_evidence.ps1', 'partition_manager_certification_bios_fixture.ps1', 'partition_manager_operation_proof_rows.ps1',
        'partition_manager_operation_proof_checks.ps1')
    foreach ($script in $scripts) { Send-OpsFile -Local (Join-Path $PSScriptRoot $script) -RemoteDir "$RemoteRoot\scripts" }
    $zipLeaf = Split-Path -Leaf $PackageZip
    $code = @"
`$incoming = '$incoming'
`$zip = Join-Path `$incoming '$zipLeaf'
if ((Get-FileHash -LiteralPath `$zip -Algorithm SHA256).Hash.ToLowerInvariant() -ne '$zipHash') { throw 'The package changed on the way' }
foreach (`$pair in @(@('$(Split-Path -Leaf $ClientExe)', '$clientHash'), @('$(Split-Path -Leaf $ProbeExe)', '$probeHash'))) {
    if ((Get-FileHash -LiteralPath (Join-Path `$incoming `$pair[0]) -Algorithm SHA256).Hash.ToLowerInvariant() -ne `$pair[1]) { throw "`$(`$pair[0]) changed on the way" }
}
`$stage = '$RemoteRoot\$stageName'
if (Test-Path -LiteralPath `$stage) { Remove-Item -LiteralPath `$stage -Recurse -Force }
if (Test-Path -LiteralPath "`$stage.readers") { Remove-Item -LiteralPath "`$stage.readers" -Recurse -Force }
`$unpack = Join-Path `$incoming 'unpack'
if (Test-Path -LiteralPath `$unpack) { Remove-Item -LiteralPath `$unpack -Recurse -Force }
Expand-Archive -LiteralPath `$zip -DestinationPath `$unpack -Force
`$app = @(Get-ChildItem -LiteralPath `$unpack -Recurse -Filter 'sak_utility.exe')
if (`$app.Count -ne 1) { throw "The package holds `$(`$app.Count) sak_utility.exe" }
Move-Item -LiteralPath `$app[0].DirectoryName -Destination `$stage
`$appHash = (Get-FileHash -LiteralPath (Join-Path `$stage 'sak_utility.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
Copy-Item -LiteralPath (Join-Path `$incoming '$(Split-Path -Leaf $ClientExe)') -Destination (Join-Path `$stage 'sak_utility.exe') -Force
# The probe is a reader, not part of the app: the app's elevation check refuses an unknown program
# in its folder, so the probe lives beside the stage with its own copy of the DLLs it loads.
`$readers = "`$stage.readers"
if (Test-Path -LiteralPath `$readers) { Remove-Item -LiteralPath `$readers -Recurse -Force }
New-Item -ItemType Directory -Force -Path `$readers | Out-Null
Copy-Item -LiteralPath (Join-Path `$incoming '$(Split-Path -Leaf $ProbeExe)') -Destination `$readers -Force
Copy-Item -Path (Join-Path `$stage '*.dll') -Destination `$readers -Force
`$receipt = [ordered]@{
    code_digest = 'see each step report: the step client names the code it was built from'
    code_commit = '$BuildLabel'
    code_dirty = `$true
    client_sha256 = '$clientHash'
    helper_sha256 = (Get-FileHash -LiteralPath (Join-Path `$stage 'sak_elevated_helper.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
    app_sha256 = `$appHash
    package_sha256 = '$zipHash'
    probe_sha256 = '$probeHash'
    staged_utc = (Get-Date).ToUniversalTime().ToString('o')
}
[IO.File]::WriteAllText((Join-Path `$stage 'build-receipt.json'), (`$receipt | ConvertTo-Json))
Remove-Item -LiteralPath `$unpack -Recurse -Force -ErrorAction SilentlyContinue
"STAGE `$stage"
`$receipt | ConvertTo-Json
"@
    Invoke-OpsRemote -Code $code | Write-Host
    return "$RemoteRoot\$stageName"
}

function Invoke-OpsRows {
    <#
    .SYNOPSIS
        Runs the rows through the certification runner, elevated, in Technician's signed-in session,
        through a scheduled task of this suite's own, and waits for it to finish.
    #>
    param([string]$Stage, [string]$RunId, [string[]]$RowIds)
    $output = "$RemoteRoot\evidence\$RunId"
    $rowList = ($RowIds | ForEach-Object { "'$_'" }) -join ','
    $job = @"
& '$RemoteRoot\scripts\run_partition_manager_app_certification.ps1' -OperationProof -Rows $rowList -StageRoot '$Stage' -OutputRoot '$output' -WorkRoot '$RemoteRoot\work'
"@
    $stub = @"
`$r = '$RemoteRoot\jobs'
'started' | Set-Content -LiteralPath "`$r\runner-started.txt"
try {
    & "`$r\runner-job.ps1" *> "`$r\runner-$RunId.log"
    "exit `$LASTEXITCODE" | Add-Content -LiteralPath "`$r\runner-$RunId.log"
} catch {
    "JOB STOPPED: `$(`$_.Exception.Message) at `$(`$_.InvocationInfo.ScriptLineNumber)" | Add-Content -LiteralPath "`$r\runner-$RunId.log"
} finally { 'done' | Set-Content -LiteralPath "`$r\runner-done.txt" }
"@
    $code = @"
`$r = '$RemoteRoot\jobs'
New-Item -ItemType Directory -Force -Path `$r, '$RemoteRoot\work', '$output' | Out-Null
foreach (`$f in 'runner-started.txt', 'runner-done.txt') { if (Test-Path "`$r\`$f") { Remove-Item "`$r\`$f" } }
[IO.File]::WriteAllText("`$r\runner-job.ps1", @'
$job
'@)
[IO.File]::WriteAllText("`$r\runner-stub.ps1", @'
$stub
'@)
`$action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ('-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + "`$r\runner-stub.ps1" + '"')
`$principal = New-ScheduledTaskPrincipal -UserId 'Technician' -LogonType Interactive -RunLevel Highest
`$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Hours 24) -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName '$Script:RunnerTask' -Action `$action -Principal `$principal -Settings `$settings -Force | Out-Null
`$started = schtasks.exe /Run /TN '$Script:RunnerTask' 2>&1
"RUN `$started"
"@
    Invoke-OpsRemote -Code $code | Write-Host
    $deadline = (Get-Date).AddMinutes($JobMinutes)
    $startBy = (Get-Date).AddMinutes(3)
    $seen = 0
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 30
        $state = Invoke-OpsRemote -AllowFailure -Code @"
`$r = '$RemoteRoot\jobs'
`$log = "`$r\runner-$RunId.log"
if (Test-Path "`$r\runner-done.txt") { 'STATE DONE' } elseif (Test-Path "`$r\runner-started.txt") { 'STATE RUNNING' } else { 'STATE WAITING' }
if (Test-Path `$log) { Get-Content -LiteralPath `$log | Select-Object -Skip $seen }
"@
        $lines = @($state | Where-Object { $_ -notmatch '^STATE ' })
        $lines | ForEach-Object { Write-Host "  | $_" }
        $seen += $lines.Count
        if ($state -contains 'STATE DONE') { return $output }
        if ($state -contains 'STATE WAITING' -and (Get-Date) -gt $startBy) { throw 'The runner task did not start: is Technician signed in?' }
    }
    throw "The rows did not finish within $JobMinutes minutes"
}

function Get-OpsEvidence {
    param([string]$RemoteOutput, [string]$LocalDir)
    New-Item -ItemType Directory -Force -Path $LocalDir | Out-Null
    # The runner's log first: when the run stopped before any row, it is all there is.
    & scp -q -o BatchMode=yes "${SshHost}:$($RemoteRoot.Replace('\', '/'))/jobs/runner-$(Split-Path -Leaf $RemoteOutput).log" $LocalDir 2>&1 | Out-Null
    $listed = Invoke-OpsRemote -Code "@(Get-ChildItem -LiteralPath '$RemoteOutput' -ErrorAction SilentlyContinue).Count"
    if ([int]"$($listed | Select-Object -Last 1)" -eq 0) { throw "The run wrote no evidence under $RemoteOutput; see $LocalDir" }
    & scp -q -r -o BatchMode=yes "${SshHost}:$($RemoteOutput.Replace('\', '/'))/*" $LocalDir 2>&1 | Where-Object { "$_" -notmatch '^\*\* ' } | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Could not bring the evidence back from $RemoteOutput" }
}

# --- Host checks: Linux's own tools, on the image copies only ---------------------------------------

function ConvertTo-OpsWslPath {
    param([string]$Path)
    $full = [IO.Path]::GetFullPath($Path)
    return '/mnt/' + $full.Substring(0, 1).ToLowerInvariant() + $full.Substring(2).Replace('\', '/')
}

function Invoke-OpsWsl {
    param([string[]]$Arguments)
    $output = & wsl.exe -d archlinux -u root -- @Arguments 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $lines = @($output | Where-Object { $_.Trim() } | Select-Object -Last 15)
    return [pscustomobject]@{ command = ($Arguments -join ' '); exit_code = [int]$code; tail = ($lines -join ' | ') }
}

function Invoke-OpsMacCheck {
    <#
    .SYNOPSIS
        Apple's own fsck_apfs -n or fsck_hfs -n -f on a copy of an image, attached read-only on the
        rig's Mac (-MacChecks). The copy stays in ~/opsproof-readonly on the Mac: removing an image
        there is the owner's to do (AGENTS.local.md), so the report lists them.
    #>
    param([string]$Path, [string]$Kind)
    $cred = Get-Content -LiteralPath $VmCredFile -Raw | ConvertFrom-Json
    $name = Split-Path -Leaf $Path
    $ssh = @('-i', "$($cred.macKey)", '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=20')
    & ssh @ssh "$($cred.macHost)" 'mkdir -p ~/opsproof-readonly' 2>&1 | Out-Null
    & scp -q @ssh $Path "$($cred.macHost):opsproof-readonly/$name" 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { return [ordered]@{ name = "mac: $name reached the Mac"; passed = $false; detail = '' } }
    $tool = if ($Kind -eq 'apfs') { 'fsck_apfs -n' } else { 'fsck_hfs -n -f' }
    $raw = if ($Kind -eq 'apfs') { '$dev' } else { '${dev/disk/rdisk}' }
    $script = "dev=`$(hdiutil attach -nomount -readonly -imagekey diskimage-class=CRawDiskImage ~/opsproof-readonly/$name | head -1 | awk '{print `$1}'); $tool $raw; code=`$?; hdiutil detach `$dev >/dev/null 2>&1; echo FSCK_EXIT=`$code"
    $output = @(& ssh @ssh "$($cred.macHost)" $script 2>&1 | ForEach-Object { "$_" })
    $exit = if (($output -join "`n") -match 'FSCK_EXIT=(\d+)') { [int]$Matches[1] } else { -1 }
    return [ordered]@{ name = "mac: Apple $tool on $name exits 0"; passed = ($exit -eq 0); detail = (@($output | Select-Object -Last 8) -join ' | ') }
}

function Invoke-OpsHostChecks {
    <#
    .SYNOPSIS
        Reads every image a row copied off the PC with Linux's e2fsprogs, apfsprogs and util-linux, and
        writes host-checks.json beside the row's report. Files only; no disk of this workstation.
    #>
    param([string]$LocalDir)
    foreach ($rowDir in @(Get-ChildItem -LiteralPath $LocalDir -Directory | Where-Object { $_.Name -ne 'images' })) {
        $independentPath = Join-Path $rowDir.FullName 'independent.json'
        if (-not (Test-Path -LiteralPath $independentPath)) { continue }
        $independent = Get-Content -LiteralPath $independentPath -Raw | ConvertFrom-Json
        $images = @(if ($independent.PSObject.Properties['images']) { $independent.images } else { @() })
        if ($images.Count -eq 0) { continue }
        $checks = @()
        foreach ($image in $images) {
            $path = Join-Path $LocalDir "images\$($image.name)"
            if (-not (Test-Path -LiteralPath $path)) { $checks += [ordered]@{ name = "host: $($image.name) came back"; passed = $false; detail = '' }; continue }
            $hashOk = (Get-OpsSha256 $path) -eq "$($image.sha256)"
            $checks += [ordered]@{ name = "host: $($image.name) is the copy the row hashed"; passed = $hashOk; detail = '' }
            $wsl = ConvertTo-OpsWslPath $path
            switch -Regex ("$($image.expect)") {
                '^ext[234]$' {
                    $fsck = Invoke-OpsWsl @('e2fsck', '-fn', $wsl)
                    $checks += [ordered]@{ name = "host: Linux e2fsck -fn $($image.name) exits 0"; passed = ($fsck.exit_code -eq 0); detail = $fsck }
                    $dump = Invoke-OpsWsl @('sh', '-c', "dumpe2fs -h '$wsl' 2>/dev/null | grep -E 'features|volume name|Block count|Block size|state'")
                    $features = if ($dump.tail -match 'Filesystem features:\s*([^|]+)') { $Matches[1].Trim() } else { '' }
                    $kind = if (-not $features) { '' } elseif ($features -match '\bextents?\b') { 'ext4' } elseif ($features -match '\bhas_journal\b') { 'ext3' } else { 'ext2' }
                    $checks += [ordered]@{ name = "host: Linux dumpe2fs reads $($image.name) as $($image.expect)"; passed = ($kind -eq "$($image.expect)"); detail = $dump.tail }
                }
                '^apfs$' {
                    $apfsck = Invoke-OpsWsl @('/root/apfsprogs/apfsck/apfsck', '-cw', $wsl)
                    $checks += [ordered]@{ name = "host: apfsck -cw $($image.name) exits 0 with nothing to say"; passed = ($apfsck.exit_code -eq 0 -and -not $apfsck.tail); detail = $apfsck }
                }
                default { }
            }
            if ($MacChecks -and "$($image.expect)" -match '^(apfs|hfs\+|hfsplus|hfsx)$') { $checks += Invoke-OpsMacCheck -Path $path -Kind "$($image.expect)" }
        }
        [IO.File]::WriteAllText((Join-Path $rowDir.FullName 'host-checks.json'), (ConvertTo-Json -InputObject @($checks) -Depth 8))
    }
}

# --- Results -----------------------------------------------------------------------------------------

function Get-OpsCertVerdict {
    # A certification row run again: the matrix's own checks, without the release binding that a
    # build from an uncommitted tree cannot meet (evidence for that comes from the final run).
    param([string]$RowDir, [string]$Id)
    . (Join-Path $PSScriptRoot 'partition_manager_row_evidence.ps1')
    $matrix = Get-Content -LiteralPath (Join-Path $ProjectRoot 'certification\PARTITION_MANAGER_CERTIFICATION_MATRIX.json') -Raw | ConvertFrom-Json
    $spec = @(@($matrix.vhd_scenarios) + @($matrix.external_gates) | Where-Object { $_.id -ceq $Id })
    if ($spec.Count -ne 1) { return @("the matrix has no row $Id") }
    $report = Get-Content -LiteralPath (Join-Path $RowDir 'report.json') -Raw | ConvertFrom-Json
    $reasons = @(Test-PmRowEvidence -Spec $spec[0] -RowDir $RowDir -CurrentCodeDigest "$($report.code.digest)")
    # Release binding (which code, which commit, a clean tree) is the final run's to meet.
    return @($reasons | Where-Object { $_ -notmatch 'uncommitted changes|names no real commit|is for other code|outside this release' })
}

function Get-OpsRowSummary {
    param([string]$RowDir, [object]$PlanRow)
    $report = Get-Content -LiteralPath (Join-Path $RowDir 'report.json') -Raw | ConvertFrom-Json
    $scripts = @(); $helper = @(); $build = ''
    foreach ($step in @($report.steps)) {
        $path = Join-Path $RowDir "$($step.report)"
        if (-not (Test-Path -LiteralPath $path)) { $helper += "$($step.id): no report"; continue }
        $client = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
        if (-not $build) { $build = "$($client.code_commit) $(if ($client.code_dirty) { '+dirty ' })digest $("$($client.code_digest)".Substring(0, [Math]::Min(12, "$($client.code_digest)".Length)))" }
        foreach ($op in @($client.queued_operations)) { if ($op.script_sha256) { $scripts += "$($step.id):$("$($op.script_sha256)".Substring(0, 12))" } }
        $exec = $client.execution
        $said = if ($client.status -eq 'Passed') { 'ok' } else { "FAILED: $("$($client.error)".Replace('|', '/'))" }
        if ($exec -and -not $exec.success -and $client.status -eq 'Passed') { $said = "expected failure: $("$($exec.message)".Replace('|', '/'))" }
        $helper += "$($step.id) $said"
    }
    $checks = @(); $failed = @()
    $independentPath = Join-Path $RowDir 'independent.json'
    if (Test-Path -LiteralPath $independentPath) {
        $independent = Get-Content -LiteralPath $independentPath -Raw | ConvertFrom-Json
        if ($independent.PSObject.Properties['checks']) { $checks += @($independent.checks) }
    }
    $hostPath = Join-Path $RowDir 'host-checks.json'
    if (Test-Path -LiteralPath $hostPath) { $checks += @(Get-Content -LiteralPath $hostPath -Raw | ConvertFrom-Json) }
    $failed = @($checks | Where-Object { -not $_.passed } | ForEach-Object { "$($_.name)" })
    $certReasons = @()
    if ($PlanRow.Id -notlike 'ops.*' -and $report.status -eq 'Passed') { $certReasons = @(Get-OpsCertVerdict -RowDir $RowDir -Id $PlanRow.Id) }
    $verdict = if ($report.status -ne 'Passed') { 'FAIL' } elseif ($failed.Count -gt 0 -or $certReasons.Count -gt 0) { 'FAIL' } elseif ($checks.Count -eq 0 -and $PlanRow.Id -like 'ops.*') { 'FAIL' } else { 'PASS' }
    $verifier = if ($PlanRow.Id -like 'ops.*') { "$($checks.Count - $failed.Count)/$($checks.Count) checks" } else { "matrix: $(if ($certReasons.Count) { $certReasons -join '; ' } else { 'all checks hold' })" }
    if ($failed.Count) { $verifier += "; failed: $($failed -join '; ')" }
    if ($report.status -ne 'Passed') { $verifier += "; row stopped: $("$($report.failure)".Replace('|', '/'))" }
    return [pscustomobject]@{ id = $PlanRow.Id; operation = $PlanRow.Operation; variant = $PlanRow.Variant; build = $build; scripts = ($scripts -join ' '); helper = ($helper -join '; '); verifier = $verifier; verdict = $verdict; run = (Split-Path -Leaf (Split-Path -Parent $RowDir)) }
}

function Write-OpsResults {
    <#
    .SYNOPSIS
        One line per plan row, from the newest run that has evidence for it under the opsproof folder.
    #>
    $root = Split-Path -Parent $ResultsPath
    $runs = @(Get-ChildItem -LiteralPath (Join-Path $root 'runs') -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending)
    $lines = @('# S.A.K. Utility operation proof: results', '',
        "Generated $(Get-Date -Format 'yyyy-MM-dd HH:mm') by scripts/run_partition_manager_operation_proof.ps1 from the evidence under $root\runs.",
        'Each row is the newest run that has evidence for it. PASS means every step did what the row expected and every check a separate tool made held.',
        '', '| Row | Operation | Variant | Machine | Build | Script SHA-256 (step:first 12) | Helper result | Verifier | Verdict | Run |', '|---|---|---|---|---|---|---|---|---|---|')
    foreach ($row in $Plan.Rows) {
        $found = $null
        foreach ($run in $runs) { $dir = Join-Path $run.FullName $row.Id; if (Test-Path -LiteralPath (Join-Path $dir 'report.json')) { $found = $dir; break } }
        if ($found) {
            $s = Get-OpsRowSummary -RowDir $found -PlanRow $row
            $lines += "| $($s.id) | $($s.operation) | $($s.variant) | $($row.Machine) | $($s.build) | $($s.scripts) | $($s.helper) | $($s.verifier) | **$($s.verdict)** | $($s.run) |"
        } else {
            $why = if ($row.Pending) { "NOT RUN: $($row.Pending)" } else { 'NOT RUN' }
            $lines += "| $($row.Id) | $($row.Operation) | $($row.Variant) | $($row.Machine) | | | | | $why | |"
        }
    }
    $lines += '', '## Not runnable', ''
    foreach ($item in $Plan.NotRunnable) { $lines += "- $($item.Operation): $($item.Reason)" }
    [IO.File]::WriteAllText($ResultsPath, (($lines -join "`n") + "`n"))
    return $ResultsPath
}

# --- Main ------------------------------------------------------------------------------------------

try {
    switch ($Action) {
        'Claim' { Invoke-OpsPreflight | Write-Host; Set-OpsClaim | Write-Host }
        'Release' { Set-OpsClaim -Remove | Write-Host }
        'Report' { Write-OpsResults }
        'Run' {
            $wanted = if ($Rows.Count) { $Rows } else { @(Get-OpsPlanRows -For $Machine | Where-Object { -not $_.Pending } | ForEach-Object { $_.Id }) }
            foreach ($id in $wanted) {
                $row = @($Plan.Rows | Where-Object { $_.Id -ceq $id })
                if ($row.Count -ne 1 -or $row[0].Machine -ne $Machine -or $row[0].Pending) { throw "$id is not a ready $Machine row in the plan" }
            }
            $runId = "run-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
            $local = if ($EvidenceDir) { $EvidenceDir } else { Join-Path (Split-Path -Parent $ResultsPath) "runs\$runId" }
            if ($Machine -eq 'vm') {
                Invoke-OpsVmRun -RowIds $wanted -LocalDir $local
                Write-OpsResults
                break
            }
            Invoke-OpsPreflight | Write-Host
            $stage = Invoke-OpsStage
            $remote = Invoke-OpsRows -Stage $stage -RunId $runId -RowIds $wanted
            Get-OpsEvidence -RemoteOutput $remote -LocalDir $local
            if (-not $SkipHostChecks) { Invoke-OpsHostChecks -LocalDir $local }
            Write-OpsResults
        }
    }
} finally {
    Remove-Item -LiteralPath $Script:Scratch -Recurse -Force -ErrorAction SilentlyContinue
}

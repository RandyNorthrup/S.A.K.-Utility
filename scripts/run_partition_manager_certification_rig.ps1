<#
.SYNOPSIS
    Drives Partition Manager certification on the rig, from a workstation that changes no disk.

.DESCRIPTION
    The rig is a Mac running VirtualBox, with a disposable Windows VM. This script stages a build,
    copies it into the guest, starts rows there, and runs what only the host can do: attaching
    throwaway disks, starting a target in a VM of its own to prove it boots, and passing a real USB
    device through. Every disk-changing step runs in the guest or on the Mac. Nothing here touches a
    disk of the machine it runs on.

    It takes no secret. The guest's password and the Mac's address come from a credentials file
    outside the repository, named by -CredFile. Run it under PowerShell 7.

    Actions:
      Stage        Copies the step client (as sak_utility.exe) and the real helper to -StageDir and
                   writes build-receipt.json: the code digest, the commit, and each binary's hash.
      Deploy       Puts the stage and the certification scripts in the guest.
      On / Off     Starts the VM and signs in, or shuts it down.
      Snapshot / Restore
      NewDisk / AttachDisk / DetachDisk / DeleteDisk
      PcPrepare    With -PcFile, makes a physical PC ready: checks it is the machine the file names,
                   that UAC is on and someone is signed in, writes the approval the rows look for,
                   registers the elevated task, and lists its disks. Run once per PC.
      WritePins    Writes -PinFile from -PinSpec, reading the disks on the VM or PC. Refuses a disk that
                   runs that Windows, and prints no serial number.
      RootWrapper / RootTidy  VirtualBox on macOS needs root to capture a USB device. With a sudoers
                   rule for VBoxManage and VBoxHeadless, RootWrapper writes a small script that runs
                   VBoxManage as root with this user's VirtualBox folder, and setting
                   $env:SAK_RIG_VBOX to its path sends every action here through it. Root then writes
                   some of the VM's files. RootTidy gives this user those files back.
                   Throwaway VDIs in this rig's work folder. Attaching needs the VM off.
      Rows         Runs -Rows in the guest and brings their evidence to -EvidenceDir.
      BootProof    Starts -Disk alone in a throwaway VM, signs in, checks it from inside, writes JSON.
      VmCreate / VmFirmware / VmRemove / VmFacts
                   The same throwaway VM kept across steps, for a row that runs inside it.
      UsbList / UsbOn / UsbOff
                   What the Mac sees on USB, and passing the two named devices through to the VM.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Stage', 'Deploy', 'On', 'Off', 'Snapshot', 'Restore', 'NewDisk', 'AttachDisk', 'DetachDisk', 'DeleteDisk',
        'Rows', 'BootProof', 'VmCreate', 'VmFirmware', 'VmRemove', 'VmFacts', 'UsbList', 'UsbOn', 'UsbOff', 'GuestJob',
        'RootWrapper', 'RootTidy', 'PcPrepare', 'WritePins', 'CloneSystemDisk')]
    [string]$Action,
    [string]$CredFile = 'temp\vm-auth\sak-capture-technician.json',
    [string]$Vm = 'Windows 11',
    [string]$MacWork = 'sak-cert',
    [string]$ReleaseDir = 'build\Release',
    [string]$StageDir = '',
    [string]$EvidenceDir = '',
    [string[]]$Rows = @(),
    [string]$Disk = '',
    [long]$SizeMiB = 0,
    [int]$Port = 2,
    [ValidateSet('Sata', 'SataSolidState', 'Usb')] [string]$DiskBus = 'Sata',
    [string]$SnapshotName = '',
    [string]$Phase = '',
    [uint64]$DataDiskBytes = 0,
    [string]$PinFile = '',
    [switch]$DataRowsOnDevice,
    # Runs -Rows as operation-proof rows (scripts/run_partition_manager_operation_proof.ps1).
    [switch]$OperationProof,
    [ValidateSet('efi', 'bios')] [string]$Firmware = 'efi',
    [string]$ProofName = '',
    [string]$ProofOutput = '',
    [string]$MarkerPath = '',
    [int]$WaitMinutes = 20,
    [switch]$ExpectNoBoot,
    [string]$JobFile = '',
    [int]$JobMinutes = 240,
    [string]$PcFile = '',
    [string]$PinSpec = ''
)

$ErrorActionPreference = 'Stop'
# Under pwsh -File a list arrives as one comma-joined string: it binds to the first element only.
$Rows = @($Rows | ForEach-Object { "$_" -split ',' } | Where-Object { $_ })
. (Join-Path $PSScriptRoot 'partition_manager_rig_sign_in.ps1')
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$Script:GuestRoot = 'C:\SAKCert'
$Script:GuestEvidence = 'C:\SAKCert\evidence'
$Script:VBox = if ($env:SAK_RIG_VBOX) { $env:SAK_RIG_VBOX } else { '/Applications/VirtualBox.app/Contents/MacOS/VBoxManage' }
$Script:GuestPowerShell = 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe'
$Script:ThrowawayVm = '^sakcert-[a-z0-9-]+$'

function Remove-RigFile {
    param([string]$Path)
    if (Test-Path -LiteralPath $Path) { Remove-Item -LiteralPath $Path -Recurse -Force }
}

function Resolve-RigPath {
    param([string]$Path)
    if ([IO.Path]::IsPathRooted($Path)) { return $Path }
    return (Join-Path $ProjectRoot $Path)
}

function Read-RigPc {
    <#
    .SYNOPSIS
        The physical PC this run may change, from a file a person wrote. It holds the machine's id and
        the words that name it, and stays out of the repository. Refuses the machine this runs on.
    #>
    $pc = [IO.File]::ReadAllText((Resolve-RigPath $PcFile)) | ConvertFrom-Json
    foreach ($field in 'host', 'key', 'machine_uuid', 'confirmation') {
        if ([string]::IsNullOrWhiteSpace("$($pc.$field)")) { throw "$PcFile has no $field" }
    }
    $here = "$((Get-CimInstance -ClassName Win32_ComputerSystemProduct).UUID)".Trim().ToUpperInvariant()
    if ("$($pc.machine_uuid)".ToUpperInvariant() -ceq $here) { throw "$PcFile names the machine this is running on. Certification does not change this machine" }
    return $pc
}

$Script:Pc = if ($PcFile) { Read-RigPc } else { $null }
$Script:PcActions = @('Stage', 'Deploy', 'On', 'Rows', 'GuestJob', 'PcPrepare', 'WritePins')
if ($null -ne $Script:Pc -and $Script:PcActions -cnotcontains $Action) { throw "$Action needs a VM, and -PcFile names a physical PC" }
if ($Action -ceq 'PcPrepare' -and $null -eq $Script:Pc) { throw 'PcPrepare needs -PcFile' }
$Script:Cred = if ($Action -ne 'Stage' -and $null -eq $Script:Pc) { [IO.File]::ReadAllText((Resolve-RigPath $CredFile)) | ConvertFrom-Json } else { $null }

function Get-PcSshOptions {
    # What ssh and scp share. -P on scp and -p on ssh are the only difference, so the port is apart.
    # The PC's host key is kept beside the PC file: trusted the first time, and held to after that.
    $options = @('-i', "$($Script:Pc.key)", '-o', 'ConnectTimeout=15', '-o', 'BatchMode=yes', '-o', 'ServerAliveInterval=30',
        '-o', 'StrictHostKeyChecking=accept-new', '-o', "UserKnownHostsFile=$((Resolve-RigPath $PcFile).Replace('\', '/')).known_hosts")
    if ("$($Script:Pc.jump)") { $options += @('-J', "$($Script:Pc.jump)") }
    return $options
}

function Invoke-PcPS {
    # Key sign-in gives an administrator its elevated token, in a session of its own.
    param([string]$Code)
    $prefix = "`$ProgressPreference='SilentlyContinue';`$ErrorActionPreference='Stop';"
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($prefix + $Code + "`n;exit 0"))
    $port = if ("$($Script:Pc.port)") { @('-p', "$($Script:Pc.port)") } else { @() }
    $output = & ssh -n @(Get-PcSshOptions) @port "$($Script:Pc.host)" "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand $encoded" 2>&1
    $exitCode = $LASTEXITCODE
    $text = @($output | ForEach-Object { "$_" })
    if ($exitCode -ne 0) { throw "The command on $($Script:Pc.host) failed with exit code ${exitCode}: $($text -join ' | ')" }
    return $text
}

function Get-PcScpTarget {
    # scp reads a Windows path after the host, with forward slashes.
    param([string]$Path)
    return "$($Script:Pc.host):$($Path.Replace('\', '/'))"
}

function Get-PcScpPort {
    if ("$($Script:Pc.port)") { return @('-P', "$($Script:Pc.port)") }
    return @()
}

function Wait-RigPc {
    # The elevated task runs in a signed-in session, and quietly does not run without one.
    $deadline = (Get-Date).AddMinutes($WaitMinutes)
    $seen = ''
    do {
        try {
            $seen = (Invoke-PcPS -Code "(quser 2>&1 | Out-String)") -join ' '
            if ($seen -match '(?i)\bconsole\b') { return "signed in: $($Script:Pc.host)" }
        } catch { $seen = $_.Exception.Message }
        if ((Get-Date) -lt $deadline) { Start-Sleep -Seconds 15 }
    } while ((Get-Date) -lt $deadline)
    throw "$($Script:Pc.host) has no one signed in at its console: $seen"
}

function Get-RigSshArguments {
    return @('-i', $Script:Cred.macKey, '-o', 'ConnectTimeout=15', '-o', 'BatchMode=yes', $Script:Cred.macHost)
}

function Invoke-Mac {
    param([string]$Script, [string]$Stdin)
    $encoded = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(($Script -replace "`r", '')))
    $remote = "bash -c `"`$(echo $encoded | base64 -D)`""
    $ssh = Get-RigSshArguments
    if ($PSBoundParameters.ContainsKey('Stdin')) { $output = $Stdin | & ssh @ssh $remote 2>&1 }
    else { $output = & ssh -n @ssh $remote 2>&1 }
    $code = $LASTEXITCODE
    $text = @($output | ForEach-Object { "$_" })
    if ($code -ne 0) { throw "The command on the Mac failed with exit code ${code}: $($text -join ' | ')" }
    return $text
}

function Invoke-MacVBox {
    param([string]$Body)
    return Invoke-Mac -Script @"
set -u
V='$Script:VBox'
VM='$Vm'
W="`$HOME/$MacWork"
mkdir -p "`$W/disks" "`$W/shots" "`$W/push"
$Body
"@
}

function Invoke-GuestControl {
    param([string]$Body, [string]$VmName = $Vm)
    $script = @"
set -u
umask 077
mkdir -p "`$HOME/$MacWork"
PF="`$HOME/$MacWork/.pw.`$`$"
trap 'rm -f "`$PF"' EXIT
cat > "`$PF"
V='$Script:VBox'
VM='$VmName'
U='$($Script:Cred.user)'
run()  { "`$V" guestcontrol "`$VM" run --username "`$U" --passwordfile "`$PF" "`$@"; }
into() { "`$V" guestcontrol "`$VM" copyto --username "`$U" --passwordfile "`$PF" "`$@"; }
from() { "`$V" guestcontrol "`$VM" copyfrom --username "`$U" --passwordfile "`$PF" "`$@"; }
$Body
"@
    return Invoke-Mac -Script $script -Stdin $Script:Cred.pass
}

function ConvertTo-BashWord {
    param([string]$Text)
    return "'" + $Text.Replace("'", "'\''") + "'"
}

function Invoke-GuestPS {
    param([string]$Code, [int]$TimeoutMs = 600000, [string]$VmName = $Vm)
    if ($null -ne $Script:Pc) { return (Invoke-PcPS -Code $Code) }
    $prefix = "`$ProgressPreference='SilentlyContinue';`$ErrorActionPreference='Stop';"
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($prefix + $Code + "`n;exit 0"))
    $body = "run --exe $(ConvertTo-BashWord $Script:GuestPowerShell) --timeout $TimeoutMs --wait-stdout --wait-stderr -- -NoProfile -ExecutionPolicy Bypass -EncodedCommand $encoded"
    return Invoke-GuestControl -Body $body -VmName $VmName
}

function Send-ToMac {
    param([string]$LocalPath, [string]$RemoteName)
    & scp -q -i $Script:Cred.macKey -o ConnectTimeout=15 -o BatchMode=yes $LocalPath "$($Script:Cred.macHost):$MacWork/$RemoteName"
    if ($LASTEXITCODE -ne 0) { throw "scp of $LocalPath failed with exit code $LASTEXITCODE" }
}

function Send-ToGuest {
    param([string[]]$LocalPaths, [string]$GuestFolder, [string]$VmName = $Vm)
    if ($null -ne $Script:Pc) {
        & scp -q @(Get-PcSshOptions) @(Get-PcScpPort) @LocalPaths (Get-PcScpTarget -Path ($GuestFolder.TrimEnd('\') + '\'))
        if ($LASTEXITCODE -ne 0) { throw "scp to $($Script:Pc.host) failed with exit code $LASTEXITCODE" }
        return
    }
    Invoke-Mac "mkdir -p ~/$MacWork/push; rm -f ~/$MacWork/push/*" | Out-Null
    $names = @()
    foreach ($path in $LocalPaths) {
        $leaf = Split-Path -Leaf $path
        Send-ToMac -LocalPath $path -RemoteName "push/$leaf"
        $names += "`"`$HOME/$MacWork/push/$leaf`""
    }
    Invoke-GuestControl -Body "into --target-directory $(ConvertTo-BashWord ($GuestFolder.TrimEnd('\') + '\')) $($names -join ' ')" -VmName $VmName | Out-Null
    Invoke-Mac "rm -f ~/$MacWork/push/*" | Out-Null
}

function Get-FromGuest {
    param([string]$GuestPath, [string]$LocalFolder, [string]$VmName = $Vm)
    if ($null -ne $Script:Pc) {
        & scp -q @(Get-PcSshOptions) @(Get-PcScpPort) (Get-PcScpTarget -Path $GuestPath) $LocalFolder
        if ($LASTEXITCODE -ne 0) { throw "scp from $($Script:Pc.host) failed with exit code $LASTEXITCODE" }
        return
    }
    $leaf = Split-Path -Leaf $GuestPath
    Invoke-GuestControl -Body "from --target-directory `"`$HOME/$MacWork/`" $(ConvertTo-BashWord $GuestPath)" -VmName $VmName | Out-Null
    & scp -q -i $Script:Cred.macKey -o ConnectTimeout=15 -o BatchMode=yes "$($Script:Cred.macHost):$MacWork/$leaf" $LocalFolder
    if ($LASTEXITCODE -ne 0) { throw "scp back of $leaf failed with exit code $LASTEXITCODE" }
    Invoke-Mac "rm -f ~/$MacWork/$leaf" | Out-Null
}

function Get-VmState {
    param([string]$VmName = $Vm)
    return (Invoke-MacVBox "`"`$V`" showvminfo '$VmName' --machinereadable | grep '^VMState=' | cut -d'`"' -f2") -join ''
}

function Invoke-GuestLogin {
    # Signs in through the VM's own keyboard, so nothing about the session is special.
    param([string]$VmName = $Vm)
    Invoke-MacVBox "`"`$V`" controlvm '$VmName' setvideomodehint 1280 800 32 2>&1 | tail -1 || true" | Out-Null
    Start-Sleep -Seconds 3
    Invoke-MacVBox "`"`$V`" controlvm '$VmName' keyboardputscancode 39 b9" | Out-Null
    Start-Sleep -Seconds 8
    foreach ($chunk in ([regex]::Matches($Script:Cred.pass, '.{1,12}') | ForEach-Object { $_.Value })) {
        Invoke-Mac -Script "umask 077; mkdir -p `"`$HOME/$MacWork`"; F=`"`$HOME/$MacWork/.typed.`$`$`"; tr -d '\r\n' > `"`$F`"; '$Script:VBox' controlvm '$VmName' keyboardputfile `"`$F`"; rm -f `"`$F`"" -Stdin $chunk | Out-Null
        Start-Sleep -Milliseconds 400
    }
    Start-Sleep -Seconds 1
    Invoke-MacVBox "`"`$V`" controlvm '$VmName' keyboardputscancode 1c 9c" | Out-Null
    Start-Sleep -Seconds 25
}

function Invoke-RigVmSignIn {
    # Types the sign-in, and takes it as done only when Windows names who is signed in.
    param([string]$VmName = $Vm)
    $type = { Invoke-GuestLogin -VmName $VmName }
    $read = {
        $list = (Invoke-MacVBox "`"`$V`" guestproperty get '$VmName' /VirtualBox/GuestInfo/OS/LoggedInUsersList 2>&1 || true") -join ' '
        if ($list -match 'Value: (\S.*)') { $Matches[1].Trim() }
    }
    return (Invoke-RigSignIn -VmName $VmName -TypeSignIn $type -SignedInUsers $read)
}

function Start-RigVm {
    param([string]$VmName = $Vm)
    Invoke-MacVBox "`"`$V`" startvm '$VmName' --type headless 2>&1 | tail -1"
    $deadline = (Get-Date).AddMinutes(10)
    $ready = ''
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 15
        $ready = (Invoke-MacVBox "`"`$V`" guestproperty get '$VmName' /VirtualBox/GuestInfo/OS/NoLoggedInUsers 2>&1 || true") -join ''
        if ($ready -match 'Value: (true|false)') { break }
    }
    Start-Sleep -Seconds 15
    # The elevated task runs in a signed-in session, and quietly does not run without one. A
    # first boot of a changed machine can take the keystrokes before the sign-in screen is ready.
    "signed in: $(Invoke-RigVmSignIn -VmName $VmName)"
}

function Stop-RigVm {
    param([string]$VmName = $Vm)
    try { Invoke-GuestPS -Code "shutdown.exe /s /t 2 /f; 'asked'" -TimeoutMs 60000 -VmName $VmName | Out-Null } catch { "shutdown request: $($_.Exception.Message)" }
    $deadline = (Get-Date).AddMinutes(5)
    while ((Get-Date) -lt $deadline -and (Get-VmState -VmName $VmName) -ne 'poweroff') { Start-Sleep -Seconds 8 }
    if ((Get-VmState -VmName $VmName) -ne 'poweroff') {
        'forcing power off'
        Invoke-MacVBox "`"`$V`" controlvm '$VmName' poweroff 2>&1 | tail -1"
        Start-Sleep -Seconds 6
    }
    "state: $(Get-VmState -VmName $VmName)"
}

function Get-RigJobFailure {
    <#
    .SYNOPSIS
        What a guest job's log says went wrong, or nothing. A job that threw, ran past its time, or
        exited nonzero failed, and so did one that left no exit line: the step it was is not done.
        A run of rows may exit nonzero for a row that failed, because the group checks each row.
    #>
    param([AllowEmptyCollection()] [string[]]$Lines, [switch]$AllowNonZeroExit)
    $stopped = @($Lines | Where-Object { "$_" -match '^JOB STOPPED:' })
    if ($stopped.Count) { return "$($stopped[0])" }
    $late = @($Lines | Where-Object { "$_" -match '^NOT DONE after ' })
    if ($late.Count) { return "$($late[0])" }
    $exits = @($Lines | Where-Object { "$_" -match '^exit\b' })
    if ($exits.Count -eq 0) { return 'it left no exit line in its log' }
    if (-not $AllowNonZeroExit -and "$($exits[-1])" -match '^exit\s+(-?\d+)\s*$' -and [int]$Matches[1] -ne 0) {
        return "it exited $($Matches[1])"
    }
    return ''
}

function Invoke-GuestElevated {
    # The guest's elevated task runs a stub that runs the job, so a job is an ordinary script. Its
    # log is the output; a job that failed by its log stops the step, after the log is shown.
    param([string]$LocalScript, [int]$Minutes, [string]$VmName = $Vm, [switch]$AllowNonZeroExit)
    $leaf = Split-Path -Leaf $LocalScript
    $stub = Join-Path ([IO.Path]::GetTempPath()) "sakcert-stub-$PID.ps1"
    [IO.File]::WriteAllText($stub, @"
`$r = '$Script:GuestRoot\jobs'
if (Test-Path "`$r\done.txt") { Remove-Item "`$r\done.txt" }
'started' | Set-Content -LiteralPath "`$r\started.txt"
try {
    & "`$r\$leaf" *> "`$r\$leaf.log"
    "exit `$LASTEXITCODE" | Add-Content -LiteralPath "`$r\$leaf.log"
} catch {
    "JOB STOPPED: `$(`$_.Exception.Message) at `$(`$_.InvocationInfo.ScriptLineNumber)" | Add-Content -LiteralPath "`$r\$leaf.log"
} finally {
    'done' | Set-Content -LiteralPath "`$r\done.txt"
}
"@)
    try {
        Invoke-GuestPS -Code "New-Item -ItemType Directory -Force '$Script:GuestRoot\jobs' | Out-Null; if (Test-Path '$Script:GuestRoot\jobs\started.txt') { Remove-Item '$Script:GuestRoot\jobs\started.txt' }" -VmName $VmName | Out-Null
        Send-ToGuest -LocalPaths @($LocalScript, $stub) -GuestFolder "$Script:GuestRoot\jobs" -VmName $VmName
        $stubLeaf = Split-Path -Leaf $stub
        Invoke-GuestPS -Code "Copy-Item '$Script:GuestRoot\jobs\$stubLeaf' 'C:\SAKCapture\Capture-Shots.ps1' -Force; if (Test-Path '$Script:GuestRoot\jobs\done.txt') { Remove-Item '$Script:GuestRoot\jobs\done.txt' }; schtasks.exe /End /TN SAKCaptureElevated 2>&1 | Out-Null; `$r = schtasks.exe /Run /TN SAKCaptureElevated 2>&1; `$r -join ' '" -VmName $VmName
        $deadline = (Get-Date).AddMinutes($Minutes)
        $startBy = (Get-Date).AddMinutes(3)
        $state = ''
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Seconds 10
            $state = (Invoke-GuestPS -Code "if (Test-Path '$Script:GuestRoot\jobs\done.txt') { 'DONE' } elseif (Test-Path '$Script:GuestRoot\jobs\started.txt') { 'RUNNING' } else { 'WAITING' }" -VmName $VmName) -join ''
            if ($state -match 'DONE') { break }
            if ($state -match 'WAITING' -and (Get-Date) -gt $startBy) { throw "The guest's elevated task did not start the job: is anyone signed in?" }
        }
        $log = @(Invoke-GuestPS -Code "if (Test-Path -LiteralPath '$Script:GuestRoot\jobs\$leaf.log') { Get-Content -LiteralPath '$Script:GuestRoot\jobs\$leaf.log' }" -TimeoutMs 900000 -VmName $VmName)
        if ($state -notmatch 'DONE') { $log = @("NOT DONE after $Minutes minutes") + $log }
        $log
        $why = Get-RigJobFailure -Lines $log -AllowNonZeroExit:$AllowNonZeroExit
        if ($why) { throw "The guest job $leaf failed: $why" }
    } finally { Remove-RigFile -Path $stub }
}

function New-RigStage {
    $release = Resolve-RigPath $ReleaseDir
    $stage = Resolve-RigPath $StageDir
    if (-not (Test-Path -LiteralPath (Join-Path $release 'partition_manager_live_certifier.exe'))) { throw "No step client in $release" }
    Remove-RigFile -Path $stage
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    # The operation proof stages a whole release package, as on testpc2: since the security round
    # its scripts load sak_script_types.dll from the program's folder, which the list below leaves
    # out. No other program goes in: the app checks what is in its folder. A certification stage is
    # unchanged.
    if ($OperationProof) {
        Copy-Item -Path (Join-Path $release '*') -Destination $stage -Recurse -Force -Exclude 'partition_manager_live_certifier.exe', 'partition_filesystem_probe_certifier.exe'
    }
    Copy-Item (Join-Path $release 'partition_manager_live_certifier.exe') (Join-Path $stage 'sak_utility.exe') -Force
    Copy-Item (Join-Path $release 'sak_elevated_helper.exe') $stage
    foreach ($dll in 'Qt6Core', 'Qt6Gui', 'Qt6Network', 'Qt6Concurrent', 'Qt6Widgets', 'Qt6Xml', 'Qt6Svg', 'msvcp140', 'msvcp140_1', 'msvcp140_2', 'msvcp140_atomic_wait', 'vcruntime140', 'vcruntime140_1', 'concrt140', 'z', 'zlib1', 'bz2', 'liblzma', 'pcre2-16', 'double-conversion', 'icudt', 'icuin', 'icuuc', 'b2-1') {
        $source = Join-Path $release "$dll.dll"
        if (Test-Path -LiteralPath $source) { Copy-Item $source $stage }
    }
    # The app asks a drive Windows cannot name for its rotation rate, and the rows read it too.
    $smartctl = Join-Path $release 'tools\smartmontools\smartctl.exe'
    if (-not (Test-Path -LiteralPath $smartctl -PathType Leaf)) { throw "No smartctl in $release" }
    New-Item -ItemType Directory -Force -Path (Join-Path $stage 'tools\smartmontools') | Out-Null
    Copy-Item $smartctl (Join-Path $stage 'tools\smartmontools')
    $described = & python (Join-Path $ProjectRoot 'scripts\partition_manager_code_digest.py') --root $ProjectRoot | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0) { throw 'The code digest could not be worked out' }
    $receipt = [ordered]@{
        code_digest = $described.code_digest
        code_commit = $described.code_commit
        code_dirty = $described.code_dirty
        client_sha256 = (Get-FileHash -LiteralPath (Join-Path $stage 'sak_utility.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
        helper_sha256 = (Get-FileHash -LiteralPath (Join-Path $stage 'sak_elevated_helper.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
        app_sha256 = (Get-FileHash -LiteralPath (Join-Path $release 'sak_utility.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
        staged_utc = (Get-Date).ToUniversalTime().ToString('o')
    }
    [IO.File]::WriteAllText((Join-Path $stage 'build-receipt.json'), ($receipt | ConvertTo-Json))
    $receipt | ConvertTo-Json
}

function Send-RigStage {
    param([string]$VmName = $Vm)
    $stage = Resolve-RigPath $StageDir
    $zip = "$stage.zip"
    Remove-RigFile -Path $zip
    Compress-Archive -Path "$stage\*" -DestinationPath $zip
    Invoke-GuestPS -Code "New-Item -ItemType Directory -Force '$Script:GuestRoot\stage','$Script:GuestRoot\scripts','$Script:GuestRoot\jobs' | Out-Null; Remove-Item '$Script:GuestRoot\stage\*' -Recurse -Force" -VmName $VmName | Out-Null
    Send-ToGuest -LocalPaths @($zip) -GuestFolder $Script:GuestRoot -VmName $VmName
    Invoke-GuestPS -Code "Expand-Archive -LiteralPath '$Script:GuestRoot\$(Split-Path -Leaf $zip)' -DestinationPath '$Script:GuestRoot\stage' -Force; (Get-ChildItem '$Script:GuestRoot\stage').Count" -VmName $VmName
    $scripts = @('run_partition_manager_app_certification.ps1', 'partition_manager_certification_row_bodies.ps1', 'partition_manager_readback.ps1',
        'partition_manager_row_evidence.ps1', 'partition_manager_certification_bios_fixture.ps1',
        'partition_manager_operation_proof_rows.ps1', 'partition_manager_operation_proof_checks.ps1') |
        ForEach-Object { Join-Path $ProjectRoot "scripts\$_" }
    $scripts += (Join-Path $ProjectRoot 'src\core\partition_scripts\os_migration_native.cs')
    Send-ToGuest -LocalPaths $scripts -GuestFolder "$Script:GuestRoot\scripts" -VmName $VmName
}

function Invoke-RigRows {
    param([string]$VmName = $Vm)
    $job = Join-Path ([IO.Path]::GetTempPath()) "sakcert-rows-$PID.ps1"
    $rowList = ($Rows | ForEach-Object { "'$_'" }) -join ','
    $extra = ''
    if ($Phase) { $extra += " -Phase '$Phase'" }
    if ($DataDiskBytes -gt 0) { $extra += " -DataDiskBytes $DataDiskBytes" }
    if ($DataRowsOnDevice) { $extra += ' -DataRowsOnDevice' }
    if ($OperationProof) { $extra += ' -OperationProof' }
    if ($PinFile) {
        Send-ToGuest -LocalPaths @((Resolve-RigPath $PinFile)) -GuestFolder "$Script:GuestRoot\jobs" -VmName $VmName
        $extra += " -PhysicalPins '$Script:GuestRoot\jobs\$(Split-Path -Leaf $PinFile)'"
    }
    # In this process, not through powershell.exe -File: -File would bind only the first row.
    [IO.File]::WriteAllText($job, "& '$Script:GuestRoot\scripts\run_partition_manager_app_certification.ps1' -Rows $rowList -StageRoot '$Script:GuestRoot\stage' -OutputRoot '$Script:GuestEvidence'$extra`n")
    $failure = $null
    try {
        # A row that failed exits nonzero, and its group says so from its report. A run of rows that
        # stopped still brings back what it wrote, and then fails here.
        try {
            Invoke-GuestElevated -LocalScript $job -Minutes $JobMinutes -VmName $VmName -AllowNonZeroExit
        } catch {
            $failure = $_
        }
        $local = Resolve-RigPath $EvidenceDir
        New-Item -ItemType Directory -Force -Path $local | Out-Null
        # Only the rows asked for. Another row already here may hold what only the host could see, which
        # the guest's copy of it does not.
        $archive = "sakcert-evidence-$PID.zip"
        $wanted = ($Rows | ForEach-Object { "'$Script:GuestEvidence\$_'" }) -join ','
        Invoke-GuestPS -Code "if (Test-Path '$Script:GuestRoot\$archive') { Remove-Item '$Script:GuestRoot\$archive' }; Compress-Archive -Path @(@($wanted) | Where-Object { Test-Path -LiteralPath `$_ }) -DestinationPath '$Script:GuestRoot\$archive' -Force" -VmName $VmName | Out-Null
        Get-FromGuest -GuestPath "$Script:GuestRoot\$archive" -LocalFolder $local -VmName $VmName
        foreach ($row in $Rows) { Remove-RigFile -Path (Join-Path $local $row) }
        Expand-Archive -LiteralPath (Join-Path $local $archive) -DestinationPath $local -Force
        Remove-Item -LiteralPath (Join-Path $local $archive)
    } catch {
        if ($null -eq $failure) { throw }
        Write-Host "The rows' evidence could not be brought back: $($_.Exception.Message)"
    } finally { Remove-RigFile -Path $job }
    if ($null -ne $failure) { throw $failure }
}

function Get-RigShot {
    param([string]$VmName, [string]$Name, [string]$Folder)
    New-Item -ItemType Directory -Force -Path $Folder | Out-Null
    Invoke-MacVBox "`"`$V`" controlvm '$VmName' screenshotpng `"`$W/shots/$Name.png`"" | Out-Null
    & scp -q -i $Script:Cred.macKey -o ConnectTimeout=15 -o BatchMode=yes "$($Script:Cred.macHost):$MacWork/shots/$Name.png" (Join-Path $Folder "$Name.png")
    if ($LASTEXITCODE -ne 0) { throw 'The screenshot did not come back' }
    return (Join-Path $Folder "$Name.png")
}

function Stop-RigThrowawayVm {
    <#
    .SYNOPSIS
        Shuts a Windows that started down from inside, as a person would. Pulling the power leaves it
        recording a failed shutdown, and on BIOS the next start then stops at "Your PC couldn't turn
        off properly" until someone presses Enter. A VM that never started is powered off.
    #>
    param([string]$VmName)
    if ((Get-VmState -VmName $VmName) -ne 'running') { return 'not running' }
    try { Invoke-GuestPS -Code "shutdown.exe /s /t 2 /f; 'asked'" -TimeoutMs 60000 -VmName $VmName | Out-Null } catch { return 'no Windows to ask, so powered off' }
    $deadline = (Get-Date).AddMinutes(5)
    while ((Get-Date) -lt $deadline) {
        if ((Get-VmState -VmName $VmName) -eq 'poweroff') { return 'shut down from inside' }
        Start-Sleep -Seconds 6
    }
    return 'did not shut down within 5 minutes, so powered off'
}

function Remove-RigThrowawayVm {
    param([string]$VmName)
    "shutdown: $(Stop-RigThrowawayVm -VmName $VmName)"
    # unregistervm --delete also deletes a disk still attached, so it runs only once the disk is off.
    Invoke-MacVBox @"
"`$V" controlvm '$VmName' poweroff 2>/dev/null | tail -1
for i in 1 2 3 4 5 6 7 8 9 10; do s=`$("`$V" showvminfo '$VmName' --machinereadable | grep '^VMState=' | cut -d'"' -f2); [ "`$s" = poweroff ] && break; /bin/sleep 3; done
for j in 1 2 3 4 5 6; do "`$V" storageattach '$VmName' --storagectl SATA --port 0 --device 0 --medium none 2>/dev/null && break; /bin/sleep 3; done
if "`$V" showvminfo '$VmName' --machinereadable | grep -q '^"SATA-0-0"="none"'; then "`$V" unregistervm '$VmName' --delete 2>&1 | tail -1; else echo 'disk still attached: VM kept, so its disk is not deleted'; fi
"@
}

function New-RigThrowawayVm {
    if ($ProofName -notmatch $Script:ThrowawayVm) { throw 'Throwaway VMs are named sakcert-*' }
    Invoke-MacVBox @"
"`$V" list vms | grep -q '"$ProofName"' && { echo 'VM already exists'; exit 3; }
"`$V" createvm --name '$ProofName' --ostype Windows11_64 --register --basefolder "`$W/vms" | tail -1
"`$V" modifyvm '$ProofName' --firmware $Firmware --memory 6144 --cpus 4 --vram 128 --graphicscontroller vboxsvga --nic1 none --ioapic on --rtc-use-utc on --audio-enabled off --mouse usbtablet
"`$V" storagectl '$ProofName' --name SATA --add sata --controller IntelAhci --portcount 4
"`$V" storageattach '$ProofName' --storagectl SATA --port 0 --device 0 --type hdd --medium "`$W/disks/$Disk.vdi"
"`$V" startvm '$ProofName' --type headless | tail -1
"@
}

function Get-RigBootFacts {
    # Windows' own account of itself, from inside the VM.
    param([string]$VmName)
    $job = Join-Path ([IO.Path]::GetTempPath()) "sakcert-check-$PID.ps1"
    $marker = if ($MarkerPath) { "'$MarkerPath'" } else { '$null' }
    [IO.File]::WriteAllText($job, @"
`$marker = $marker
`$facts = [ordered]@{
    firmware = "`$env:firmware_type"
    system_drive = "`$env:SystemDrive"
    repair_scan = "`$(Repair-Volume -DriveLetter (`$env:SystemDrive.Substring(0,1)) -Scan)"
    disk_count = @(Get-Disk).Count
    source_marker = if (`$marker -and (Test-Path -LiteralPath `$marker)) { "`$(Get-Content -LiteralPath `$marker -Raw)".Trim() } else { '' }
}
`$facts | ConvertTo-Json | Set-Content -LiteralPath '$Script:GuestRoot\jobs\boot-facts.json' -Encoding UTF8
"@)
    # The disk carries the facts an earlier proof of it wrote. Those go first, so a facts job that
    # failed cannot leave them to be read as this start's.
    Invoke-GuestPS -Code "if (Test-Path -LiteralPath '$Script:GuestRoot\jobs\boot-facts.json') { Remove-Item -LiteralPath '$Script:GuestRoot\jobs\boot-facts.json' -Force }" -VmName $VmName | Out-Null
    try {
        Invoke-GuestElevated -LocalScript $job -Minutes 25 -VmName $VmName | Out-Null
    } finally { Remove-RigFile -Path $job }
    return (((Invoke-GuestPS -Code "Get-Content -LiteralPath '$Script:GuestRoot\jobs\boot-facts.json' -Raw" -VmName $VmName) -join "`n") | ConvertFrom-Json)
}

function Invoke-RigBootProof {
    <#
    .SYNOPSIS
        Starts a disk alone in a VM of its own, and says what Windows reports from inside it.
    #>
    New-RigThrowawayVm
    $shots = Join-Path (Split-Path -Parent (Resolve-RigPath $ProofOutput)) 'shots'
    $result = [ordered]@{ reached_sign_in = $false; firmware = ''; system_drive = ''; repair_scan = ''; only_disk_attached = $true; source_marker_present = $false; shot = '' }
    $deadline = (Get-Date).AddMinutes($WaitMinutes)
    $attempts = 0
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 30
        $attempts++
        $state = (Invoke-MacVBox "`"`$V`" guestproperty get '$ProofName' /VirtualBox/GuestInfo/OS/NoLoggedInUsers 2>&1 || true") -join ' '
        if ($state -match 'Value: true') { $result.reached_sign_in = $true; break }
        if ($ExpectNoBoot -and $attempts -ge 6) { break }
    }
    Start-Sleep -Seconds 20
    $result.shot = Split-Path -Leaf (Get-RigShot -VmName $ProofName -Name $ProofName -Folder $shots)
    try {
        if ($result.reached_sign_in) {
            Invoke-RigVmSignIn -VmName $ProofName | Out-Null
            $facts = Get-RigBootFacts -VmName $ProofName
            $result.firmware = "$($facts.firmware)"
            $result.system_drive = "$($facts.system_drive)"
            $result.repair_scan = "$($facts.repair_scan)"
            $result.only_disk_attached = ([int]$facts.disk_count -eq 1)
            $result.source_marker = "$($facts.source_marker)"
        }
    } finally {
        Remove-RigThrowawayVm -VmName $ProofName
    }
    [IO.File]::WriteAllText((Resolve-RigPath $ProofOutput), ($result | ConvertTo-Json))
    $result | ConvertTo-Json
}

function Invoke-RigPcPrepare {
    $job = Join-Path ([IO.Path]::GetTempPath()) "sakcert-pc-prepare-$PID.ps1"
    Invoke-PcPS -Code "New-Item -ItemType Directory -Force 'C:\SAKCapture','$Script:GuestRoot\stage','$Script:GuestRoot\scripts','$Script:GuestRoot\jobs','$Script:GuestRoot\evidence','$Script:GuestRoot\work' | Out-Null" | Out-Null
    Send-ToGuest -LocalPaths @((Join-Path $PSScriptRoot 'partition_manager_readback.ps1')) -GuestFolder "$Script:GuestRoot\scripts"
    $approval = [ordered]@{ machine_uuid = "$($Script:Pc.machine_uuid)"; confirmation = "$($Script:Pc.confirmation)" } | ConvertTo-Json -Compress
    [IO.File]::WriteAllText($job, @"
`$ErrorActionPreference = 'Stop'
. '$Script:GuestRoot\scripts\partition_manager_readback.ps1'
[IO.File]::WriteAllText('$Script:GuestRoot\disposable-machine.json', '$($approval.Replace("'", "''"))')
Assert-RbDisposableMachine -ApprovalPath '$Script:GuestRoot\disposable-machine.json'
`$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not `$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'This sign-in has no elevated token: sign in by key as an administrator' }
`$policy = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System'
if ((Get-ItemProperty -Path `$policy -Name EnableLUA).EnableLUA -ne 1) { throw 'UAC is off. Rows need a real standard-user token: turn it on and restart' }
Set-ItemProperty -Path `$policy -Name ConsentPromptBehaviorAdmin -Value 0 -Type DWord
Set-ItemProperty -Path `$policy -Name PromptOnSecureDesktop -Value 0 -Type DWord
if (-not (Test-Path -LiteralPath 'C:\SAKCapture\Capture-Shots.ps1')) { Set-Content -LiteralPath 'C:\SAKCapture\Capture-Shots.ps1' -Value '' }
`$action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\SAKCapture\Capture-Shots.ps1'
`$task = New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Highest
`$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Days 2) -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName SAKCaptureElevated -Action `$action -Principal `$task -Settings `$settings -Force | Out-Null
`$facts = Get-RbMachineFacts
`$os = Get-CimInstance -ClassName Win32_OperatingSystem
"`$(`$facts.manufacturer) `$(`$facts.model), firmware `$env:firmware_type, `$(`$os.Caption) build `$(`$os.BuildNumber)"
Get-Disk | ForEach-Object {
    `$disk = `$_
    `$physical = Get-PhysicalDisk | Where-Object { "`$(`$_.DeviceId)" -eq "`$(`$disk.Number)" } | Select-Object -First 1
    "disk `$(`$disk.Number): '`$(`$disk.FriendlyName)' `$([uint64]`$disk.Size) bytes, bus `$(`$disk.BusType), media '`$(`$physical.MediaType)', style `$(`$disk.PartitionStyle), boot `$(`$disk.IsBoot), system `$(`$disk.IsSystem)"
}
"@)
    try {
        Send-ToGuest -LocalPaths @($job) -GuestFolder "$Script:GuestRoot\jobs"
        Invoke-PcPS -Code "& '$Script:GuestRoot\jobs\$(Split-Path -Leaf $job)'"
    } finally { Remove-RigFile -Path $job }
    Wait-RigPc
}

function Get-RigPinFile {
    <#
    .SYNOPSIS
        Reads the disks on the target and writes the pin file. -PinSpec is role=model|bytes, separated
        by semicolons. A role needs exactly one match, and that disk must not run this Windows. Prints
        no serial number.
    #>
    if ([string]::IsNullOrWhiteSpace($PinSpec) -or [string]::IsNullOrWhiteSpace($PinFile)) { throw 'WritePins needs -PinSpec and -PinFile' }
    $raw = (Invoke-GuestPS -Code "@(Get-Disk | ForEach-Object { [pscustomobject]@{ number = [int]`$_.Number; model = `"`$(`$_.FriendlyName)`"; size_bytes = `"`$([uint64]`$_.Size)`"; serial_number = `"`$(`$_.SerialNumber)`".Trim(); bus_type = `"`$(`$_.BusType)`"; unique_id = `"`$(`$_.UniqueId)`".Trim(); boot = [bool]`$_.IsBoot; system = [bool]`$_.IsSystem } }) | ConvertTo-Json -Depth 3 -Compress") -join ''
    $disks = @($raw | ConvertFrom-Json)
    $pins = [ordered]@{}
    foreach ($entry in ($PinSpec -split ';' | Where-Object { $_ })) {
        if ($entry -notmatch '^(ssd|hdd|any)=(.+)\|(\d+)$') { throw "A pin is role=model|bytes, not '$entry'" }
        $role, $model, $bytes = $Matches[1], $Matches[2], $Matches[3]
        $found = @($disks | Where-Object { "$($_.model)" -ceq $model -and "$($_.size_bytes)" -ceq $bytes })
        if ($found.Count -ne 1) { throw "Expected one disk that is $model, $bytes bytes: found $($found.Count)" }
        $disk = $found[0]
        if ($disk.boot -or $disk.system) { throw "$model runs the Windows on that machine, and a pin is for a disk the rows may change" }
        $pins[$role] = [ordered]@{
            unique_id = $disk.unique_id; serial_number = $disk.serial_number; model = $disk.model; size_bytes = $disk.size_bytes; bus_type = $disk.bus_type
            confirmation = "CERTIFY $($disk.model) $($disk.serial_number) $($disk.size_bytes)"
        }
        "$role is disk $($disk.number): $($disk.model), $($disk.size_bytes) bytes, $($disk.bus_type)"
    }
    [IO.File]::WriteAllText((Resolve-RigPath $PinFile), ($pins | ConvertTo-Json -Depth 4))
}

function Get-UsbDevices {
    # The two devices the owner named, by what they report, and nothing else on this Mac.
    return @(
        [pscustomobject]@{ name = 'NVMe adapter'; vendor = '6655'; product = 'a583'; marker = 'NS-PCNVMEHDE'; size = '256.1 GB' },
        [pscustomobject]@{ name = 'Seagate'; vendor = '0bc2'; product = '331a'; marker = 'Expansion Desk'; size = '8.0 TB' }
    )
}

switch ($Action) {
    'Stage' { New-RigStage }
    'Deploy' { Send-RigStage }
    'On' { if ($null -ne $Script:Pc) { Wait-RigPc } else { Start-RigVm } }
    'PcPrepare' { Invoke-RigPcPrepare }
    'WritePins' { Get-RigPinFile }
    'Off' { Stop-RigVm }
    'Snapshot' { Invoke-MacVBox "`"`$V`" snapshot '$Vm' take '$SnapshotName' 2>&1 | tail -1; `"`$V`" snapshot '$Vm' list --machinereadable | grep -E '^CurrentSnapshotName'" }
    'Restore' { Invoke-MacVBox "`"`$V`" snapshot '$Vm' restore '$SnapshotName' 2>&1 | tail -1; `"`$V`" showvminfo '$Vm' --machinereadable | grep '^VMState='" }
    'NewDisk' {
        if ($Disk -notmatch '^[a-z0-9-]+$') { throw 'A disk name is lowercase letters, digits and dashes' }
        Invoke-MacVBox "`"`$V`" createmedium disk --filename `"`$W/disks/$Disk.vdi`" --size $SizeMiB --format VDI 2>&1 | tail -1"
    }
    'AttachDisk' {
        if ($Disk -notmatch '^[a-z0-9-]+$') { throw 'A disk name is lowercase letters, digits and dashes' }
        if ((Get-VmState) -notin 'poweroff', 'aborted') { throw 'Attach a disk only with the VM off: hot-plug leaves stale links' }
        if ($DiskBus -ceq 'Usb') {
            # A virtual USB mass-storage device: the guest's own USB stack carries it, with no host device involved.
            Invoke-MacVBox "`"`$V`" storagectl '$Vm' --name CertUsb --add usb --controller USB 2>&1 | tail -1; `"`$V`" storageattach '$Vm' --storagectl CertUsb --port 0 --device 0 --type hdd --medium `"`$W/disks/$Disk.vdi`" 2>&1 | tail -1; `"`$V`" showvminfo '$Vm' --machinereadable | grep -E '^`"CertUsb-0-0`"='"
        } else {
            $solidState = if ($DiskBus -ceq 'SataSolidState') { ' --nonrotational on --discard on' } else { '' }
            Invoke-MacVBox "`"`$V`" storageattach '$Vm' --storagectl SATA --port $Port --device 0 --type hdd --medium `"`$W/disks/$Disk.vdi`"$solidState 2>&1 | tail -1; `"`$V`" showvminfo '$Vm' --machinereadable | grep -E '^`"SATA-$Port-0`"='"
        }
    }
    'DetachDisk' {
        if ($DiskBus -ceq 'Usb') {
            Invoke-MacVBox "`"`$V`" storagectl '$Vm' --name CertUsb --remove 2>&1 | tail -1; `"`$V`" showvminfo '$Vm' --machinereadable | grep -cE '^`"CertUsb-0-0`"=' || true"
        } else {
            Invoke-MacVBox "`"`$V`" storageattach '$Vm' --storagectl SATA --port $Port --device 0 --medium none 2>&1 | tail -1; `"`$V`" showvminfo '$Vm' --machinereadable | grep -E '^`"SATA-$Port-0`"='"
        }
    }
    'CloneSystemDisk' {
        # A full copy of the VM's system disk as it is now, for a fixture to read offline.
        if ($Disk -notmatch '^[a-z0-9-]+$') { throw 'A disk name is lowercase letters, digits and dashes' }
        if ((Get-VmState) -notin 'poweroff', 'aborted') { throw 'Clone the system disk only with the VM off: a running Windows changes it while it is copied' }
        Invoke-MacVBox @"
S=`$("`$V" showvminfo '$Vm' --machinereadable | grep '^"SATA-ImageUUID-0-0"=' | cut -d'"' -f4)
[ -n "`$S" ] || { echo 'The VM has no system disk on SATA port 0'; exit 3; }
"`$V" clonemedium disk "`$S" "`$W/disks/$Disk.vdi" --format VDI 2>&1 | tail -1
[ -f "`$W/disks/$Disk.vdi" ] || { echo 'The clone was not written'; exit 4; }
"@
    }
    'DeleteDisk' {
        if ($Disk -notmatch '^[a-z0-9-]+$') { throw 'A disk name is lowercase letters, digits and dashes' }
        Invoke-MacVBox "`"`$V`" closemedium disk `"`$W/disks/$Disk.vdi`" --delete 2>&1 | tail -1; df -h `"`$HOME`" | tail -1"
    }
    'Rows' { Invoke-RigRows }
    'BootProof' { Invoke-RigBootProof }
    'VmCreate' {
        New-RigThrowawayVm
        $deadline = (Get-Date).AddMinutes($WaitMinutes)
        $state = ''
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Seconds 20
            $state = (Invoke-MacVBox "`"`$V`" guestproperty get '$ProofName' /VirtualBox/GuestInfo/OS/NoLoggedInUsers 2>&1 || true") -join ' '
            if ($state -match 'Value: true') { break }
        }
        if ($state -notmatch 'Value: true') { throw "$ProofName did not reach sign-in within $WaitMinutes minutes" }
        Start-Sleep -Seconds 15
        Invoke-RigVmSignIn -VmName $ProofName | Out-Null
    }
    'VmFirmware' {
        if ((Get-VmState -VmName $ProofName) -notin 'poweroff', 'aborted') { throw 'Firmware changes only with the VM off' }
        Invoke-MacVBox "`"`$V`" modifyvm '$ProofName' --firmware $Firmware 2>&1 | tail -1"
    }
    'VmRemove' { Remove-RigThrowawayVm -VmName $ProofName }
    'VmFacts' {
        $result = [ordered]@{ reached_sign_in = $false; firmware = ''; system_drive = ''; repair_scan = '' }
        Invoke-MacVBox "`"`$V`" startvm '$ProofName' --type headless | tail -1"
        $deadline = (Get-Date).AddMinutes($WaitMinutes)
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Seconds 30
            $state = (Invoke-MacVBox "`"`$V`" guestproperty get '$ProofName' /VirtualBox/GuestInfo/OS/NoLoggedInUsers 2>&1 || true") -join ' '
            if ($state -match 'Value: true') { $result.reached_sign_in = $true; break }
        }
        if ($result.reached_sign_in) {
            Start-Sleep -Seconds 15
            Invoke-RigVmSignIn -VmName $ProofName | Out-Null
            $facts = Get-RigBootFacts -VmName $ProofName
            $result.firmware = "$($facts.firmware)"
            $result.system_drive = "$($facts.system_drive)"
            $result.repair_scan = "$($facts.repair_scan)"
        }
        [IO.File]::WriteAllText((Resolve-RigPath $ProofOutput), ($result | ConvertTo-Json))
        $result | ConvertTo-Json
    }
    'GuestJob' { Invoke-GuestElevated -LocalScript (Resolve-RigPath $JobFile) -Minutes $JobMinutes }
    'RootWrapper' {
        # umask 022: what root copies out of the guest has to be readable by this user afterwards.
        Invoke-MacVBox @"
cat > "`$W/vbox-root.sh" <<'WRAP'
#!/bin/bash
umask 022
exec sudo -n VBOX_USER_HOME="`$HOME/Library/VirtualBox" /Applications/VirtualBox.app/Contents/MacOS/VBoxManage "`$@"
WRAP
chmod 755 "`$W/vbox-root.sh"
echo "`$W/vbox-root.sh"
"@
    }
    'RootTidy' {
        # A file root replaced in a folder this user owns can be replaced back without root.
        Invoke-Mac -Script @"
set -u
for d in "`$HOME/VirtualBox VMs/$Vm" "`$HOME/Library/VirtualBox" "`$HOME/$MacWork"; do
  [ -d "`$d" ] || continue
  find "`$d" -type f -user root -print0 | while IFS= read -r -d '' f; do
    if cp -p "`$f" "`$f.sak-tidy" 2>/dev/null && mv -f "`$f.sak-tidy" "`$f"; then echo "gave back: `$f"; else rm -f "`$f.sak-tidy"; echo "could not give back: `$f"; fi
  done
done
echo 'still root:'
for d in "`$HOME/VirtualBox VMs/$Vm" "`$HOME/Library/VirtualBox" "`$HOME/$MacWork"; do
  [ -d "`$d" ] && find "`$d" -user root
done
echo 'end'
"@
    }
    'UsbList' {
        Invoke-MacVBox "`"`$V`" list usbhost | grep -E 'VendorId|ProductId|Product:|Current State|SerialNumber|^$' ; diskutil list external physical; `"`$V`" showvminfo '$Vm' --machinereadable | grep -E '^(usb|ehci|xhci|USBFilter)'"
    }
    'UsbOn' {
        if ((Get-VmState) -notin 'poweroff', 'aborted') { throw 'USB controllers change only with the VM off' }
        $filters = @()
        $index = 0
        foreach ($device in (Get-UsbDevices)) {
            $filters += "`"`$V`" usbfilter add $index --target '$Vm' --name '$($device.name)' --vendorid $($device.vendor) --productid $($device.product) 2>&1 | tail -1"
            $index++
        }
        Invoke-MacVBox "`"`$V`" modifyvm '$Vm' --usbohci on --usbehci on --usbxhci on 2>&1 | tail -3`n$($filters -join "`n")`n`"`$V`" showvminfo '$Vm' --machinereadable | grep -E '^(usb|ehci|xhci|USBFilter)'"
    }
    'UsbOff' {
        if ((Get-VmState) -notin 'poweroff', 'aborted') { throw 'USB controllers change only with the VM off' }
        Invoke-MacVBox "`"`$V`" usbfilter remove 1 --target '$Vm' 2>&1 | tail -1; `"`$V`" usbfilter remove 0 --target '$Vm' 2>&1 | tail -1; `"`$V`" modifyvm '$Vm' --usbehci off --usbxhci off 2>&1 | tail -1; `"`$V`" showvminfo '$Vm' --machinereadable | grep -E '^(usb|ehci|xhci|USBFilter)'"
    }
}

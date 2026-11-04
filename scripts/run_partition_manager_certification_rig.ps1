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
        'RootWrapper', 'RootTidy')]
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
    [ValidateSet('efi', 'bios')] [string]$Firmware = 'efi',
    [string]$ProofName = '',
    [string]$ProofOutput = '',
    [string]$MarkerPath = '',
    [int]$WaitMinutes = 20,
    [switch]$ExpectNoBoot,
    [string]$JobFile = '',
    [int]$JobMinutes = 240
)

$ErrorActionPreference = 'Stop'
# Under pwsh -File a list arrives as one comma-joined string: it binds to the first element only.
$Rows = @($Rows | ForEach-Object { "$_" -split ',' } | Where-Object { $_ })
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$Script:GuestRoot = 'C:\SAKCert'
$Script:GuestEvidence = 'C:\SAKCert\evidence'
$Script:VBox = if ($env:SAK_RIG_VBOX) { $env:SAK_RIG_VBOX } else { '/Applications/VirtualBox.app/Contents/MacOS/VBoxManage' }
$Script:GuestPowerShell = 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe'
$Script:ThrowawayVm = '^sakcert-[a-z0-9-]+$'

function Resolve-RigPath {
    param([string]$Path)
    if ([IO.Path]::IsPathRooted($Path)) { return $Path }
    return (Join-Path $ProjectRoot $Path)
}

$Script:Cred = if ($Action -ne 'Stage') { [IO.File]::ReadAllText((Resolve-RigPath $CredFile)) | ConvertFrom-Json } else { $null }

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
    $users = ''
    for ($attempt = 0; $attempt -lt 4; $attempt++) {
        if ($attempt -gt 0 -or $ready -match 'Value: true') { Invoke-GuestLogin -VmName $VmName }
        $users = (Invoke-MacVBox "`"`$V`" guestproperty get '$VmName' /VirtualBox/GuestInfo/OS/LoggedInUsersList 2>&1 || true") -join ' '
        if ($users -match 'Value: \S') { break }
        Start-Sleep -Seconds 30
    }
    if ($users -notmatch 'Value: \S') { throw "$VmName did not sign in" }
    "signed in: $users"
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

function Invoke-GuestElevated {
    # The guest's elevated task runs a stub that runs the job, so a job is an ordinary script.
    param([string]$LocalScript, [int]$Minutes, [string]$VmName = $Vm)
    $leaf = Split-Path -Leaf $LocalScript
    $stub = Join-Path ([IO.Path]::GetTempPath()) "sakcert-stub-$PID.ps1"
    [IO.File]::WriteAllText($stub, @"
`$r = '$Script:GuestRoot\jobs'
Remove-Item "`$r\done.txt" -ErrorAction SilentlyContinue
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
        Invoke-GuestPS -Code "New-Item -ItemType Directory -Force '$Script:GuestRoot\jobs' | Out-Null; Remove-Item '$Script:GuestRoot\jobs\started.txt' -ErrorAction SilentlyContinue" -VmName $VmName | Out-Null
        Send-ToGuest -LocalPaths @($LocalScript, $stub) -GuestFolder "$Script:GuestRoot\jobs" -VmName $VmName
        $stubLeaf = Split-Path -Leaf $stub
        Invoke-GuestPS -Code "Copy-Item '$Script:GuestRoot\jobs\$stubLeaf' 'C:\SAKCapture\Capture-Shots.ps1' -Force; Remove-Item '$Script:GuestRoot\jobs\done.txt' -ErrorAction SilentlyContinue; schtasks.exe /End /TN SAKCaptureElevated 2>&1 | Out-Null; `$r = schtasks.exe /Run /TN SAKCaptureElevated 2>&1; `$r -join ' '" -VmName $VmName
        $deadline = (Get-Date).AddMinutes($Minutes)
        $startBy = (Get-Date).AddMinutes(3)
        $state = ''
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Seconds 10
            $state = (Invoke-GuestPS -Code "if (Test-Path '$Script:GuestRoot\jobs\done.txt') { 'DONE' } elseif (Test-Path '$Script:GuestRoot\jobs\started.txt') { 'RUNNING' } else { 'WAITING' }" -VmName $VmName) -join ''
            if ($state -match 'DONE') { break }
            if ($state -match 'WAITING' -and (Get-Date) -gt $startBy) { throw "The guest's elevated task did not start the job: is anyone signed in?" }
        }
        if ($state -notmatch 'DONE') { "NOT DONE after $Minutes minutes" }
        Invoke-GuestPS -Code "Get-Content -LiteralPath '$Script:GuestRoot\jobs\$leaf.log' -ErrorAction SilentlyContinue" -TimeoutMs 900000 -VmName $VmName
    } finally { Remove-Item -LiteralPath $stub -ErrorAction SilentlyContinue }
}

function New-RigStage {
    $release = Resolve-RigPath $ReleaseDir
    $stage = Resolve-RigPath $StageDir
    if (-not (Test-Path -LiteralPath (Join-Path $release 'partition_manager_live_certifier.exe'))) { throw "No step client in $release" }
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    Copy-Item (Join-Path $release 'partition_manager_live_certifier.exe') (Join-Path $stage 'sak_utility.exe')
    Copy-Item (Join-Path $release 'sak_elevated_helper.exe') $stage
    foreach ($dll in 'Qt6Core', 'Qt6Gui', 'Qt6Network', 'Qt6Concurrent', 'Qt6Widgets', 'Qt6Xml', 'Qt6Svg', 'msvcp140', 'msvcp140_1', 'msvcp140_2', 'msvcp140_atomic_wait', 'vcruntime140', 'vcruntime140_1', 'concrt140', 'z', 'zlib1', 'bz2', 'liblzma', 'pcre2-16', 'double-conversion', 'icudt', 'icuin', 'icuuc', 'b2-1') {
        $source = Join-Path $release "$dll.dll"
        if (Test-Path -LiteralPath $source) { Copy-Item $source $stage }
    }
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
    Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
    Compress-Archive -Path "$stage\*" -DestinationPath $zip
    Invoke-GuestPS -Code "New-Item -ItemType Directory -Force '$Script:GuestRoot\stage','$Script:GuestRoot\scripts','$Script:GuestRoot\jobs' | Out-Null; Remove-Item '$Script:GuestRoot\stage\*' -Recurse -Force -ErrorAction SilentlyContinue" -VmName $VmName | Out-Null
    Send-ToGuest -LocalPaths @($zip) -GuestFolder $Script:GuestRoot -VmName $VmName
    Invoke-GuestPS -Code "Expand-Archive -LiteralPath '$Script:GuestRoot\$(Split-Path -Leaf $zip)' -DestinationPath '$Script:GuestRoot\stage' -Force; (Get-ChildItem '$Script:GuestRoot\stage').Count" -VmName $VmName
    $scripts = @('run_partition_manager_app_certification.ps1', 'partition_manager_certification_row_bodies.ps1', 'partition_manager_readback.ps1',
        'partition_manager_row_evidence.ps1', 'partition_manager_certification_bios_fixture.ps1') |
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
    if ($PinFile) {
        Send-ToGuest -LocalPaths @((Resolve-RigPath $PinFile)) -GuestFolder "$Script:GuestRoot\jobs" -VmName $VmName
        $extra += " -PhysicalPins '$Script:GuestRoot\jobs\$(Split-Path -Leaf $PinFile)'"
    }
    # In this process, not through powershell.exe -File: -File would bind only the first row.
    [IO.File]::WriteAllText($job, "& '$Script:GuestRoot\scripts\run_partition_manager_app_certification.ps1' -Rows $rowList -StageRoot '$Script:GuestRoot\stage' -OutputRoot '$Script:GuestEvidence'$extra`n")
    try {
        Invoke-GuestElevated -LocalScript $job -Minutes $JobMinutes -VmName $VmName
        $local = Resolve-RigPath $EvidenceDir
        New-Item -ItemType Directory -Force -Path $local | Out-Null
        $archive = "sakcert-evidence-$PID.zip"
        Invoke-GuestPS -Code "Remove-Item '$Script:GuestRoot\$archive' -ErrorAction SilentlyContinue; Compress-Archive -Path '$Script:GuestEvidence\*' -DestinationPath '$Script:GuestRoot\$archive' -Force" -VmName $VmName | Out-Null
        Get-FromGuest -GuestPath "$Script:GuestRoot\$archive" -LocalFolder $local -VmName $VmName
        Expand-Archive -LiteralPath (Join-Path $local $archive) -DestinationPath $local -Force
        Remove-Item -LiteralPath (Join-Path $local $archive)
    } finally { Remove-Item -LiteralPath $job -ErrorAction SilentlyContinue }
}

function Get-RigShot {
    param([string]$VmName, [string]$Name, [string]$Folder)
    New-Item -ItemType Directory -Force -Path $Folder | Out-Null
    Invoke-MacVBox "`"`$V`" controlvm '$VmName' screenshotpng `"`$W/shots/$Name.png`"" | Out-Null
    & scp -q -i $Script:Cred.macKey -o ConnectTimeout=15 -o BatchMode=yes "$($Script:Cred.macHost):$MacWork/shots/$Name.png" (Join-Path $Folder "$Name.png")
    if ($LASTEXITCODE -ne 0) { throw 'The screenshot did not come back' }
    return (Join-Path $Folder "$Name.png")
}

function Remove-RigThrowawayVm {
    param([string]$VmName)
    # unregistervm --delete also deletes a disk still attached, so it runs only once the disk is off.
    Invoke-MacVBox @"
"`$V" controlvm '$VmName' poweroff 2>&1 | tail -1
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
    Invoke-GuestElevated -LocalScript $job -Minutes 25 -VmName $VmName | Out-Null
    Remove-Item -LiteralPath $job -ErrorAction SilentlyContinue
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
            Invoke-GuestLogin -VmName $ProofName
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
    'On' { Start-RigVm }
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
        Invoke-GuestLogin -VmName $ProofName
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
            Invoke-GuestLogin -VmName $ProofName
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

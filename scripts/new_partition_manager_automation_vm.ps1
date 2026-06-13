<#
.SYNOPSIS
    Creates a dedicated Windows VM for noninteractive Partition Manager gates.

.DESCRIPTION
    Builds a new VirtualBox Windows 11 lab VM with a real guest password file,
    shared repo folder, Guest Additions, disposable data disks, and install-time
    UAC admin-filtering policy changes. The resulting VM is intentionally a lab
    automation machine: guestcontrol runs as the configured local admin account
    with a high token, so destructive VM gates do not depend on UAC keypresses.

    This script creates or replaces only the named VirtualBox VM. It does not
    mutate host disks or the existing SAK-PM-Lab-Win11 VM unless that exact VM
    name is supplied.
#>

[CmdletBinding()]
param(
    [string]$VmName = "SAK-PM-Automation-Win11",
    [string]$IsoPath = "",
    [string]$VBoxManage = "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe",
    [string]$GuestUser = "saklab",
    [string]$GuestHostname = "sak-pm-auto.local",
    [string]$PasswordFile = "",
    [string]$SharedFolderName = "sakrepo",
    [string]$SharedFolderPath = "",
    [int]$MemoryMb = 8192,
    [int]$CpuCount = 4,
    [int]$SystemDiskMb = 81920,
    [int]$DataDiskMb = 4096,
    [int]$ImageIndex = 6,
    [int]$InstallTimeoutMinutes = 90,
    [switch]$ForceRecreate,
    [switch]$StartInstall
)

$ErrorActionPreference = "Stop"
$script:SecretRedactions = New-Object System.Collections.Generic.List[string]

function Hide-Secrets {
    param([object[]]$Lines)

    $redacted = New-Object System.Collections.Generic.List[string]
    foreach ($line in $Lines) {
        $text = [string]$line
        foreach ($secret in $script:SecretRedactions) {
            if ($secret) {
                $text = $text.Replace($secret, "[redacted]")
            }
        }
        $redacted.Add($text)
    }
    return $redacted
}

function Invoke-VBox {
    param(
        [Parameter(Mandatory = $true)] [string[]]$Arguments,
        [switch]$AllowFailure
    )

    $oldErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $VBoxManage @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldErrorActionPreference
    }
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        $safeOutput = Hide-Secrets -Lines $output
        throw "VBoxManage failed with exit code ${exitCode}: $($safeOutput -join "`n")"
    }
    return [pscustomobject]@{
        exit_code = $exitCode
        output = (Hide-Secrets -Lines $output)
    }
}

function Test-HostIsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function ConvertTo-SafeFilePart {
    param([Parameter(Mandatory = $true)] [string]$Value)
    return ($Value -replace "[^A-Za-z0-9_.-]", "_")
}

function Resolve-WindowsIso {
    param([string]$ExplicitIso)

    if ($ExplicitIso) {
        if (-not (Test-Path -LiteralPath $ExplicitIso -PathType Leaf)) {
            throw "Windows ISO not found: $ExplicitIso"
        }
        return (Resolve-Path -LiteralPath $ExplicitIso).Path
    }

    $downloadIsoDir = Join-Path $env:USERPROFILE "Downloads\iso"
    $candidates = @(
        (Join-Path $downloadIsoDir "Win11_25H2_English_x64.iso"),
        (Join-Path $downloadIsoDir "Windows.iso"),
        (Join-Path $downloadIsoDir "Windows11old.iso")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "No Windows ISO found. Pass -IsoPath."
}

function New-PasswordFile {
    param(
        [Parameter(Mandatory = $true)] [string]$Path
    )

    $alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789!@#$%^*-_+="
    $bytes = New-Object byte[] 28
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $rng.GetBytes($bytes)
    }
    finally {
        $rng.Dispose()
    }
    $chars = for ($i = 0; $i -lt $bytes.Length; $i++) {
        $alphabet[$bytes[$i] % $alphabet.Length]
    }
    $password = -join $chars
    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
    Set-Content -LiteralPath $Path -Value $password -NoNewline -Encoding ascii
    try {
        & icacls.exe $Path /inheritance:r /grant:r "$env:USERNAME`:F" /grant:r "SYSTEM:F" | Out-Null
    }
    catch {
        Write-Warning "Unable to tighten ACLs on password file ${Path}: $($_.Exception.Message)"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Resolve-PasswordFile {
    param(
        [string]$ExplicitPasswordFile,
        [Parameter(Mandatory = $true)] [string]$MachineName,
        [Parameter(Mandatory = $true)] [string]$UserName
    )

    if ($ExplicitPasswordFile) {
        if (-not (Test-Path -LiteralPath $ExplicitPasswordFile -PathType Leaf)) {
            return New-PasswordFile -Path $ExplicitPasswordFile
        }
        return (Resolve-Path -LiteralPath $ExplicitPasswordFile).Path
    }

    $authDir = Join-Path (Get-Location) "temp\vm-auth"
    $safeVm = ConvertTo-SafeFilePart -Value $MachineName
    $safeUser = ConvertTo-SafeFilePart -Value $UserName
    $defaultPath = Join-Path $authDir "${safeVm}-${safeUser}.password"
    if (Test-Path -LiteralPath $defaultPath -PathType Leaf) {
        return (Resolve-Path -LiteralPath $defaultPath).Path
    }
    return New-PasswordFile -Path $defaultPath
}

function New-PostInstallTemplate {
    param(
        [Parameter(Mandatory = $true)] [string]$Path
    )

    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
    $content = @'
@echo off
setlocal EnableExtensions
set MY_LOG_FILE=C:\sak-automation-postinstall.log
echo *** SAK automation post-install started >> %MY_LOG_FILE%

set MY_VBOX_ADDITIONS=
for %%D in (D E F G H I) do (
    if exist %%D:\VBoxWindowsAdditions.exe set MY_VBOX_ADDITIONS=%%D:
    if exist %%D:\vboxadditions\VBoxWindowsAdditions.exe set MY_VBOX_ADDITIONS=%%D:\vboxadditions
)

if not "%MY_VBOX_ADDITIONS%"=="" (
    echo *** Installing Guest Additions from %MY_VBOX_ADDITIONS% >> %MY_LOG_FILE%
    if exist %MY_VBOX_ADDITIONS%\cert\VBoxCertUtil.exe (
        %MY_VBOX_ADDITIONS%\cert\VBoxCertUtil.exe add-trusted-publisher %MY_VBOX_ADDITIONS%\cert\vbox*.cer --root %MY_VBOX_ADDITIONS%\cert\vbox*.cer >> %MY_LOG_FILE% 2>&1
        echo *** VBoxCertUtil ERRORLEVEL: %ERRORLEVEL% >> %MY_LOG_FILE%
    )
    %MY_VBOX_ADDITIONS%\VBoxWindowsAdditions.exe /S >> %MY_LOG_FILE% 2>&1
    echo *** VBoxWindowsAdditions ERRORLEVEL: %ERRORLEVEL% >> %MY_LOG_FILE%
) else (
    echo *** Guest Additions media not found >> %MY_LOG_FILE%
)

echo *** Applying automation auth policy >> %MY_LOG_FILE%
reg add HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System /v EnableLUA /t REG_DWORD /d 0 /f >> %MY_LOG_FILE% 2>&1
reg add HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System /v ConsentPromptBehaviorAdmin /t REG_DWORD /d 0 /f >> %MY_LOG_FILE% 2>&1
reg add HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System /v PromptOnSecureDesktop /t REG_DWORD /d 0 /f >> %MY_LOG_FILE% 2>&1
reg add HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System /v LocalAccountTokenFilterPolicy /t REG_DWORD /d 1 /f >> %MY_LOG_FILE% 2>&1

echo *** Rebooting after automation policy >> %MY_LOG_FILE%
shutdown /r /t 20 /c "SAK VM automation policy applied" >> %MY_LOG_FILE% 2>&1
echo *** SAK automation post-install done >> %MY_LOG_FILE%
endlocal
'@
    Set-Content -LiteralPath $Path -Value $content -Encoding ascii
    return (Resolve-Path -LiteralPath $Path).Path
}

function Test-VmExists {
    param([Parameter(Mandatory = $true)] [string]$MachineName)

    $result = Invoke-VBox -Arguments @("showvminfo", $MachineName, "--machinereadable") -AllowFailure
    return $result.exit_code -eq 0
}

function Remove-ExistingVm {
    param([Parameter(Mandatory = $true)] [string]$MachineName)

    $info = Invoke-VBox -Arguments @("showvminfo", $MachineName, "--machinereadable") -AllowFailure
    if ($info.exit_code -ne 0) {
        return
    }
    if (($info.output -join "`n") -match 'VMState="running"') {
        Invoke-VBox -Arguments @("controlvm", $MachineName, "poweroff") -AllowFailure | Out-Null
        Start-Sleep -Seconds 3
    }
    Invoke-VBox -Arguments @("unregistervm", $MachineName, "--delete") | Out-Null
}

function Wait-GuestDirectAdmin {
    param(
        [Parameter(Mandatory = $true)] [string]$MachineName,
        [Parameter(Mandatory = $true)] [string]$UserName,
        [Parameter(Mandatory = $true)] [string]$GuestPasswordFile,
        [Parameter(Mandatory = $true)] [int]$TimeoutMinutes
    )

    $deadline = (Get-Date).AddMinutes($TimeoutMinutes)
    $lastError = ""
    while ((Get-Date) -lt $deadline) {
        $wait = Invoke-VBox -Arguments @(
            "guestcontrol", $MachineName, "waitrunlevel", "desktop", "--timeout", "60000"
        ) -AllowFailure
        if ($wait.exit_code -ne 0) {
            $lastError = ($wait.output -join "`n")
            Start-Sleep -Seconds 10
            continue
        }

        $adminProbeCommand = @"
`$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (`$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { 'admin=true' } else { 'admin=false' }
"@
        $probe = Invoke-VBox -Arguments @(
            "guestcontrol", $MachineName, "run",
            "--username", $UserName,
            "--passwordfile", $GuestPasswordFile,
            "--exe", "C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe",
            "--",
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-Command", $adminProbeCommand
        ) -AllowFailure
        $probeText = $probe.output -join "`n"
        if ($probe.exit_code -eq 0 -and $probeText -match "admin=true") {
            return [pscustomobject]@{
                direct_admin = $true
                probe = "admin=true"
            }
        }
        $lastError = $probeText
        Start-Sleep -Seconds 15
    }

    throw "VM $MachineName did not reach direct-admin guestcontrol state within $TimeoutMinutes minutes. Last error: $lastError"
}

if (-not (Test-Path -LiteralPath $VBoxManage -PathType Leaf)) {
    throw "VBoxManage not found: $VBoxManage"
}

$iso = Resolve-WindowsIso -ExplicitIso $IsoPath
if (-not $SharedFolderPath) {
    $SharedFolderPath = (Get-Location).Path
}
$SharedFolderPath = (Resolve-Path -LiteralPath $SharedFolderPath).Path
$guestPasswordFile = Resolve-PasswordFile -ExplicitPasswordFile $PasswordFile -MachineName $VmName -UserName $GuestUser
$script:SecretRedactions.Add((Get-Content -LiteralPath $guestPasswordFile -Raw))
$postInstallTemplate = New-PostInstallTemplate -Path (Join-Path (Get-Location) ("temp\vm-auth\{0}-postinstall.cmd" -f (ConvertTo-SafeFilePart -Value $VmName)))

if ((Test-VmExists -MachineName $VmName)) {
    if (-not $ForceRecreate) {
        throw "VM already exists: $VmName. Pass -ForceRecreate to replace it."
    }
    Remove-ExistingVm -MachineName $VmName
}

$baseFolder = Join-Path $env:USERPROFILE "VirtualBox VMs"
$vmFolder = Join-Path $baseFolder $VmName
New-Item -ItemType Directory -Path $vmFolder -Force | Out-Null

Invoke-VBox -Arguments @("createvm", "--name", $VmName, "--ostype", "Windows11_64", "--basefolder", $baseFolder, "--register") | Out-Null
Invoke-VBox -Arguments @(
    "modifyvm", $VmName,
    "--memory", ([string]$MemoryMb),
    "--cpus", ([string]$CpuCount),
    "--firmware", "efi",
    "--graphicscontroller", "vboxsvga",
    "--vram", "128",
    "--nic1", "nat",
    "--clipboard", "bidirectional",
    "--boot1", "dvd",
    "--boot2", "disk"
) | Out-Null
Invoke-VBox -Arguments @("modifyvm", $VmName, "--tpm-type", "2.0") -AllowFailure | Out-Null

Invoke-VBox -Arguments @("storagectl", $VmName, "--name", "SATA", "--add", "sata", "--controller", "IntelAhci", "--portcount", "8") | Out-Null

$systemDisk = Join-Path $vmFolder "system.vdi"
$dataDiskA = Join-Path $vmFolder "data-a.vdi"
$dataDiskB = Join-Path $vmFolder "data-b.vdi"
Invoke-VBox -Arguments @("createmedium", "disk", "--filename", $systemDisk, "--size", ([string]$SystemDiskMb), "--format", "VDI") | Out-Null
Invoke-VBox -Arguments @("createmedium", "disk", "--filename", $dataDiskA, "--size", ([string]$DataDiskMb), "--format", "VDI") | Out-Null
Invoke-VBox -Arguments @("createmedium", "disk", "--filename", $dataDiskB, "--size", ([string]$DataDiskMb), "--format", "VDI") | Out-Null
Invoke-VBox -Arguments @("storageattach", $VmName, "--storagectl", "SATA", "--port", "0", "--device", "0", "--type", "hdd", "--medium", $systemDisk) | Out-Null
Invoke-VBox -Arguments @("storageattach", $VmName, "--storagectl", "SATA", "--port", "1", "--device", "0", "--type", "hdd", "--medium", $dataDiskA) | Out-Null
Invoke-VBox -Arguments @("storageattach", $VmName, "--storagectl", "SATA", "--port", "2", "--device", "0", "--type", "hdd", "--medium", $dataDiskB) | Out-Null

Invoke-VBox -Arguments @("sharedfolder", "add", $VmName, "--name", $SharedFolderName, "--hostpath", $SharedFolderPath, "--automount") | Out-Null

$unattendedArgs = @(
    "unattended", "install", $VmName,
    "--iso=$iso",
    "--user=$GuestUser",
    "--user-password-file=$guestPasswordFile",
    "--admin-password-file=$guestPasswordFile",
    "--full-user-name=SAKLab",
    "--hostname=$GuestHostname",
    "--locale=en_US",
    "--country=US",
    "--time-zone=America/Los_Angeles",
    "--key=VK7JG-NPHTM-C97JM-9MPGT-3V66T",
    "--image-index=$ImageIndex",
    "--install-additions",
    "--additions-iso=C:\Program Files\Oracle\VirtualBox\VBoxGuestAdditions.iso",
    "--post-install-template=$postInstallTemplate",
    "--start-vm=$(if ($StartInstall) { 'headless' } else { 'none' })"
)
Invoke-VBox -Arguments $unattendedArgs | Out-Null

$reportRoot = Join-Path (Get-Location) "artifacts\partition-manager-certification\vm-lab\automation-vm"
New-Item -ItemType Directory -Path $reportRoot -Force | Out-Null
$reportPath = Join-Path $reportRoot ("{0}-{1}.json" -f (ConvertTo-SafeFilePart -Value $VmName), (Get-Date -Format "yyyyMMddHHmmss"))

$directAdmin = $false
if ($StartInstall) {
    $result = Wait-GuestDirectAdmin -MachineName $VmName -UserName $GuestUser -GuestPasswordFile $guestPasswordFile -TimeoutMinutes $InstallTimeoutMinutes
    $directAdmin = [bool]$result.direct_admin
}

$report = [pscustomobject]@{
    schema_version = 1
    vm_name = $VmName
    created_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    iso_path = $iso
    guest_user = $GuestUser
    guest_hostname = $GuestHostname
    password_file_used = $true
    password_file_path_redacted = $true
    password_value_redacted = $true
    shared_folder_name = $SharedFolderName
    shared_folder_path = $SharedFolderPath
    start_install = [bool]$StartInstall
    direct_admin_guestcontrol_verified = $directAdmin
    host_is_admin = Test-HostIsAdmin
    automation_policy = [pscustomobject]@{
        enable_lua = 0
        consent_prompt_behavior_admin = 0
        prompt_on_secure_desktop = 0
        local_account_token_filter_policy = 1
    }
}
$report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $reportPath -Encoding UTF8

Write-Host "Automation VM configured: $VmName"
Write-Host "Report: $reportPath"
if ($StartInstall) {
    Write-Host "Direct admin guestcontrol verified: $directAdmin"
} else {
    Write-Host "Install media prepared. Start with: VBoxManage startvm `"$VmName`" --type headless"
}

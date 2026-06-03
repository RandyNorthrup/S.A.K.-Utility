<#
.SYNOPSIS
    Proves OS migration target boot in a disposable VirtualBox VM.

.DESCRIPTION
    Clones the current SAK-PM-Lab-Win11 boot disk to a new target disk,
    boots a target-only EFI VM from the clone, captures Guest Additions
    heartbeat/screenshot evidence, and writes external.os-migration-reboot.
#>

[CmdletBinding()]
param(
    [string]$SourceVmName = "SAK-PM-Lab-Win11",
    [string]$TargetVmName = ("SAK-PM-OS-Migration-Target-" + (Get-Date -Format "yyyyMMdd-HHmmss")),
    [string]$EvidenceRoot = "artifacts\partition-manager-certification\vm-lab\external-evidence",
    [string]$RunRoot = "artifacts\partition-manager-certification\vm-lab\os-migration",
    [string]$VBoxManage = "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe",
    [string]$VirtualBoxBaseFolder = (Join-Path $env:USERPROFILE "VirtualBox VMs"),
    [int]$BootTimeoutSeconds = 600,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $VBoxManage -PathType Leaf)) {
    throw "VBoxManage not found: $VBoxManage"
}

function Invoke-VBox {
    param(
        [Parameter(Mandatory = $true)] [string[]]$Arguments,
        [Parameter(Mandatory = $true)] [string]$LogPath,
        [switch]$AllowFailure
    )

    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $VBoxManage @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    $record = [pscustomobject]@{
        command = "VBoxManage " + ($Arguments -join " ")
        exit_code = $exitCode
        output = (($output | ForEach-Object { $_.ToString() }) -join "`n")
    }
    $record | ConvertTo-Json -Depth 8 | Add-Content -LiteralPath $LogPath -Encoding UTF8
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        throw "VBoxManage failed: $($record.command)`n$($record.output)"
    }
    return $record
}

function Get-MachineValue {
    param(
        [Parameter(Mandatory = $true)] [string[]]$Info,
        [Parameter(Mandatory = $true)] [string]$Key
    )
    $line = @($Info | Where-Object { $_ -match "^$([regex]::Escape($Key))=" } | Select-Object -First 1)
    if ($line.Count -eq 0) {
        return ""
    }
    return (($line[0] -split "=", 2)[1]).Trim('"')
}

function ConvertTo-ProjectRelativePath {
    param([Parameter(Mandatory = $true)] [string]$Path)
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $fullRoot = [System.IO.Path]::GetFullPath((Get-Location).Path)
    if (-not $fullRoot.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $fullRoot += [System.IO.Path]::DirectorySeparatorChar
    }
    if ($fullPath.StartsWith($fullRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        return ($fullPath.Substring($fullRoot.Length) -replace "\\", "/")
    }
    return $fullPath
}

function Write-JsonFile {
    param(
        [Parameter(Mandatory = $true)] [object]$Value,
        [Parameter(Mandatory = $true)] [string]$Path
    )
    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    $Value | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $Path -Encoding UTF8
}

if (-not $Force) {
    throw "Pass -Force after confirming the target VM and disk are disposable."
}

$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$runPath = Join-Path $RunRoot $timestamp
$gateDir = Join-Path $EvidenceRoot "external.os-migration-reboot"
$commandLog = Join-Path $runPath "vbox-commands.jsonl"
New-Item -ItemType Directory -Path $runPath -Force | Out-Null
New-Item -ItemType Directory -Path $gateDir -Force | Out-Null

$sourceInfo = & $VBoxManage showvminfo $SourceVmName --machinereadable
$sourceState = Get-MachineValue -Info $sourceInfo -Key "VMState"
if ($sourceState -ne "poweroff") {
    throw "Source VM must be powered off before cloning. Current state: $sourceState"
}
$sourceDisk = Get-MachineValue -Info $sourceInfo -Key '"SATA-0-0"'
if ([string]::IsNullOrWhiteSpace($sourceDisk) -or -not (Test-Path -LiteralPath $sourceDisk -PathType Leaf)) {
    throw "Could not resolve source boot disk from $SourceVmName."
}

$existingTarget = & $VBoxManage list vms | Select-String ('"' + [regex]::Escape($TargetVmName) + '"')
if ($existingTarget) {
    throw "Target VM already exists: $TargetVmName"
}

$targetVmDir = Join-Path $VirtualBoxBaseFolder $TargetVmName
$targetDisk = Join-Path $targetVmDir "os-migration-target.vdi"

$sourceInfo | Set-Content -LiteralPath (Join-Path $runPath "source-showvminfo.txt") -Encoding UTF8
(& $VBoxManage showmediuminfo disk $sourceDisk) | Set-Content -LiteralPath (Join-Path $runPath "source-showmediuminfo.txt") -Encoding UTF8

Invoke-VBox -Arguments @("createvm", "--name", $TargetVmName, "--ostype", "Windows11_64", "--basefolder", $VirtualBoxBaseFolder, "--register") -LogPath $commandLog | Out-Null
Invoke-VBox -Arguments @("modifyvm", $TargetVmName, "--memory", "4096", "--cpus", "2", "--firmware", "efi", "--graphicscontroller", "vboxsvga", "--vram", "128", "--nic1", "nat", "--boot1", "disk", "--boot2", "none", "--boot3", "none", "--boot4", "none") -LogPath $commandLog | Out-Null
Invoke-VBox -Arguments @("storagectl", $TargetVmName, "--name", "SATA", "--add", "sata", "--controller", "IntelAhci", "--portcount", "4", "--bootable", "on") -LogPath $commandLog | Out-Null
Invoke-VBox -Arguments @("clonemedium", "disk", $sourceDisk, $targetDisk, "--format", "VDI") -LogPath $commandLog | Out-Null
Invoke-VBox -Arguments @("storageattach", $TargetVmName, "--storagectl", "SATA", "--port", "0", "--device", "0", "--type", "hdd", "--medium", $targetDisk) -LogPath $commandLog | Out-Null

(& $VBoxManage showmediuminfo disk $targetDisk) | Set-Content -LiteralPath (Join-Path $runPath "target-showmediuminfo.txt") -Encoding UTF8
(& $VBoxManage showvminfo $TargetVmName --machinereadable) | Set-Content -LiteralPath (Join-Path $runPath "target-showvminfo-before-boot.txt") -Encoding UTF8

Invoke-VBox -Arguments @("startvm", $TargetVmName, "--type", "headless") -LogPath $commandLog | Out-Null

$pollRows = @()
$booted = $false
$deadline = (Get-Date).AddSeconds($BootTimeoutSeconds)
$shotIndex = 0
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 20
    $shotIndex++
    $targetInfo = & $VBoxManage showvminfo $TargetVmName --machinereadable
    $state = Get-MachineValue -Info $targetInfo -Key "VMState"
    $guestRunLevel = Get-MachineValue -Info $targetInfo -Key "GuestAdditionsRunLevel"
    $videoMode = Get-MachineValue -Info $targetInfo -Key "VideoMode"
    $product = Get-MachineValue -Info $targetInfo -Key "GuestInfo/OS/Product"
    $screenshot = Join-Path $runPath ("target-boot-{0}.png" -f $shotIndex)
    $screenshotStatus = "Captured"
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $screenshotOutput = & $VBoxManage controlvm $TargetVmName screenshotpng $screenshot 2>&1
        if ($LASTEXITCODE -ne 0) {
            $screenshotStatus = (($screenshotOutput | ForEach-Object { $_.ToString() }) -join "`n")
        }
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    $pollRows += [pscustomobject]@{
        timestamp = (Get-Date).ToString("o")
        vm_state = $state
        guest_additions_run_level = $guestRunLevel
        video_mode = $videoMode
        guest_product = $product
        screenshot = ConvertTo-ProjectRelativePath -Path $screenshot
        screenshot_status = $screenshotStatus
    }
    if ($state -eq "running" -and $guestRunLevel -eq "3") {
        $booted = $true
        break
    }
}

$pollPath = Join-Path $runPath "target-boot-poll.json"
Write-JsonFile -Value $pollRows -Path $pollPath
(& $VBoxManage showvminfo $TargetVmName --machinereadable) | Set-Content -LiteralPath (Join-Path $runPath "target-showvminfo-after-boot.txt") -Encoding UTF8

Invoke-VBox -Arguments @("controlvm", $TargetVmName, "acpipowerbutton") -LogPath $commandLog -AllowFailure | Out-Null
Start-Sleep -Seconds 15
$finalState = Get-MachineValue -Info (& $VBoxManage showvminfo $TargetVmName --machinereadable) -Key "VMState"
if ($finalState -eq "running") {
    Invoke-VBox -Arguments @("controlvm", $TargetVmName, "poweroff") -LogPath $commandLog -AllowFailure | Out-Null
}

$artifacts = @(
    "source-showvminfo.txt",
    "source-showmediuminfo.txt",
    "target-showmediuminfo.txt",
    "target-showvminfo-before-boot.txt",
    "target-showvminfo-after-boot.txt",
    "target-boot-poll.json",
    "vbox-commands.jsonl"
) | ForEach-Object { ConvertTo-ProjectRelativePath -Path (Join-Path $runPath $_) }

$latestPoll = @($pollRows)[@($pollRows).Count - 1]
$report = [ordered]@{
    tool = "partition-manager-external-evidence-report"
    schema_version = 1
    gate_id = "external.os-migration-reboot"
    gate_name = "OS migration target boot and firmware-order proof"
    status = if ($booted) { "Passed" } else { "Failed" }
    created_utc = (Get-Date).ToUniversalTime().ToString("o")
    manifest = "artifacts/partition-manager-certification/vm-lab/external-evidence.imported.json"
    certification_matrix = "docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
    required_evidence_keys = @(
        "source_disk_id",
        "target_disk_id",
        "clone_verification_mode",
        "boot_validation_output",
        "firmware_boot_order",
        "target_boot_result"
    )
    safety_contract = @(
        "disposable_vm_or_lab_disk",
        "target_overwrite_confirmed",
        "source_target_identity_recorded",
        "target_boot_verified"
    )
    evidence = [ordered]@{
        source_disk_id = "$SourceVmName SATA-0-0 $sourceDisk"
        target_disk_id = "$TargetVmName SATA-0-0 $targetDisk"
        clone_verification_mode = "VBoxManage clonemedium disk from powered-off source boot medium to disposable VDI, then target-only EFI VM booted from cloned disk."
        boot_validation_output = "GuestAdditionsRunLevel=$($latestPoll.guest_additions_run_level); VideoMode=$($latestPoll.video_mode); GuestProduct=$($latestPoll.guest_product); poll_artifact=$(ConvertTo-ProjectRelativePath -Path $pollPath)"
        firmware_boot_order = "Target VM firmware=EFI, boot1=disk, boot2/3/4=none; target-showvminfo artifacts capture full order."
        target_boot_result = if ($booted) { "Passed: target-only cloned VM reached running state with Guest Additions run level 3." } else { "Failed: target cloned VM did not reach Guest Additions run level 3 before timeout." }
    }
    artifacts = $artifacts
    suggested_evidence_path = "artifacts/partition-manager-certification/vm-lab/external-evidence/external.os-migration-reboot/report.json"
    verification_summary = if ($booted) {
        "Disposable VirtualBox target VM $TargetVmName booted from a cloned source OS disk with EFI firmware and reached Guest Additions run level 3, proving target-only post-migration boot."
    }
    else {
        "Disposable VirtualBox target VM $TargetVmName did not reach Guest Additions run level 3 within $BootTimeoutSeconds seconds."
    }
    operator_notes = "Target VM/disk are disposable OS-migration proof artifacts; source VM stayed powered off during clone."
}

$reportFileName = if ($booted) { "report.json" } else { "report.failed.json" }
$reportPath = Join-Path $gateDir $reportFileName
foreach ($stale in @("report.json", "report.failed.json")) {
    $stalePath = Join-Path $gateDir $stale
    if (Test-Path -LiteralPath $stalePath -PathType Leaf) {
        Remove-Item -LiteralPath $stalePath -Force
    }
}
Write-JsonFile -Value $report -Path $reportPath

if (-not $booted) {
    throw "OS migration target boot failed; see $reportPath"
}

Write-Host "OS migration external gate passed: $reportPath"

<#
.SYNOPSIS
    Launches Partition Manager destructive VM gates from the Windows host.

.DESCRIPTION
    This host-side helper solves the repeatable VM auth path for
    SAK-PM-Lab-Win11. It resolves a real guest password file, validates
    VirtualBox guestcontrol, requires a direct administrator token, starts the
    gate runner from the shared repo, then waits for fresh evidence to appear
    in the host workspace.

    No password value is written to stdout, stderr, JSON reports, or process
    arguments. If a password file is not supplied, the helper uses a cached
    temp file or extracts the password from the VM's unattended install XML.
#>

[CmdletBinding()]
param(
    [ValidateSet("linux-swap", "ext-filesystem", "hdd-defrag")]
    [string]$Gate = "linux-swap",

    [string]$VmName = "SAK-PM-Lab-Win11",
    [string]$GuestUser = "saklab",
    [string]$GuestDomain = "",
    [string]$PasswordFile = "",
    [string]$SharedRoot = "\\vboxsvr\sakrepo",
    [string]$VBoxManage = "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe",
    [ValidateRange(1, 128)]
    [int]$DiskNumber = 1,
    [ValidateSet("ext2", "ext3", "ext4")]
    [string]$FileSystem = "ext4",
    [ValidateSet(4096, 8192, 16384, 65536)]
    [int]$PageSizeBytes = 4096,
    [string]$Label = "SAKSWAPVM",
    [int]$TimeoutSeconds = 900,
    [int]$GuestReadyTimeoutSeconds = 600,
    [int]$UacDelaySeconds = 5,
    [int]$UacAcceptAttempts = 6,
    [int]$UacAcceptIntervalSeconds = 2,
    [switch]$NoCleanup,
    [switch]$AllowUacKeypressFallback
)

$ErrorActionPreference = "Stop"

# Pin every host-side path (credentials, reports, screenshots, evidence) to the repository that
# actually contains this script rather than the ambient working directory, which an unrelated
# launch location could otherwise turn into an implicit trust root for secrets and evidence.
$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $RepoRoot -or -not (Test-Path -LiteralPath $RepoRoot -PathType Container)) {
    throw "Unable to resolve repository root from script location."
}

function ConvertTo-SafeFilePart {
    param([Parameter(Mandatory = $true)] [string]$Value)
    return ($Value -replace "[^A-Za-z0-9_.-]", "_")
}

function Get-VBoxMachineInfo {
    param(
        [Parameter(Mandatory = $true)] [string]$VBoxManagePath,
        [Parameter(Mandatory = $true)] [string]$MachineName
    )

    $raw = & $VBoxManagePath showvminfo $MachineName --machinereadable 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "VBoxManage showvminfo failed for ${MachineName}: $($raw -join "`n")"
    }

    $info = @{}
    foreach ($line in $raw) {
        if ($line -match '^([^=]+)="(.*)"$') {
            $info[$matches[1]] = ($matches[2] -replace '\\"', '"')
        }
    }
    return $info
}

function Resolve-GuestPasswordFile {
    param(
        [Parameter(Mandatory = $true)] [string]$VBoxManagePath,
        [Parameter(Mandatory = $true)] [string]$MachineName,
        [Parameter(Mandatory = $true)] [string]$UserName,
        [string]$ExplicitPasswordFile
    )

    # An EXPLICIT -PasswordFile or the SAK_VM_GUEST_PASSWORD_FILE override is a deliberate
    # credential selection: if it is set but does not resolve to a real file, FAIL CLOSED rather
    # than silently falling through to a cached/unattended password (a different identity).
    if ($ExplicitPasswordFile) {
        if (-not (Test-Path -LiteralPath $ExplicitPasswordFile -PathType Leaf)) {
            throw "Explicit -PasswordFile does not exist: $ExplicitPasswordFile"
        }
        return (Resolve-Path -LiteralPath $ExplicitPasswordFile).Path
    }
    if ($env:SAK_VM_GUEST_PASSWORD_FILE) {
        if (-not (Test-Path -LiteralPath $env:SAK_VM_GUEST_PASSWORD_FILE -PathType Leaf)) {
            throw "SAK_VM_GUEST_PASSWORD_FILE is set but does not exist: $env:SAK_VM_GUEST_PASSWORD_FILE"
        }
        return (Resolve-Path -LiteralPath $env:SAK_VM_GUEST_PASSWORD_FILE).Path
    }

    $candidateFiles = New-Object System.Collections.Generic.List[string]
    $authDir = Join-Path $RepoRoot "temp\vm-auth"
    New-Item -ItemType Directory -Path $authDir -Force | Out-Null
    $safeVm = ConvertTo-SafeFilePart -Value $MachineName
    $safeUser = ConvertTo-SafeFilePart -Value $UserName
    $cachedPasswordFile = Join-Path $authDir "${safeVm}-${safeUser}.password"
    $legacyPasswordFile = Join-Path $authDir "${safeUser}.password"
    $candidateFiles.Add($cachedPasswordFile)
    $candidateFiles.Add($legacyPasswordFile)

    foreach ($candidate in ($candidateFiles | Select-Object -Unique)) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    $info = Get-VBoxMachineInfo -VBoxManagePath $VBoxManagePath -MachineName $MachineName
    if (-not $info.ContainsKey("CfgFile")) {
        throw "Unable to locate VM config file for $MachineName."
    }

    $vmConfig = $info["CfgFile"] -replace "\\\\", "\"
    $vmRoot = Split-Path -Parent $vmConfig
    $unattendedXml = Get-ChildItem -LiteralPath $vmRoot -Filter "*autounattend.xml" -File |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $unattendedXml) {
        throw "Guest password file not supplied and no unattended XML found next to $vmConfig."
    }

    [xml]$xml = Get-Content -LiteralPath $unattendedXml.FullName -Raw
    $passwords = New-Object System.Collections.Generic.List[string]
    foreach ($account in $xml.SelectNodes("//*[local-name()='LocalAccount']")) {
        $name = $account.SelectSingleNode("*[local-name()='Name']")
        $value = $account.SelectSingleNode("*[local-name()='Password']/*[local-name()='Value']")
        if ($name -and $value -and $name.InnerText -ieq $UserName -and $value.InnerText) {
            $passwords.Add($value.InnerText)
        }
    }

    if ($passwords.Count -eq 0) {
        $autoUser = $xml.SelectSingleNode("//*[local-name()='AutoLogon']/*[local-name()='Username']")
        $autoPassword = $xml.SelectSingleNode("//*[local-name()='AutoLogon']/*[local-name()='Password']/*[local-name()='Value']")
        if ($autoUser -and $autoPassword -and $autoUser.InnerText -ieq $UserName -and $autoPassword.InnerText) {
            $passwords.Add($autoPassword.InnerText)
        }
    }

    if ($passwords.Count -eq 0) {
        throw "No unattended password entry found for guest user $UserName."
    }

    $distinctPasswords = @($passwords | Select-Object -Unique)
    if ($distinctPasswords.Count -gt 1) {
        throw "Multiple distinct unattended passwords found for guest user ${UserName}; refusing to guess which one is correct."
    }

    Set-Content -LiteralPath $cachedPasswordFile -Value $distinctPasswords[0] -NoNewline -Encoding ascii
    # Harden the freshly written plaintext secret and FAIL CLOSED if the ACL restriction does not
    # take: a broadly readable password file left on disk is worse than no cached file at all.
    & icacls.exe $cachedPasswordFile /inheritance:r /grant:r "$env:USERNAME`:F" /grant:r "SYSTEM:F" | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Remove-Item -LiteralPath $cachedPasswordFile -Force -ErrorAction SilentlyContinue
        throw "Failed to restrict ACLs on cached password file '${cachedPasswordFile}'; refusing to leave an unprotected plaintext secret on disk."
    }
    return (Resolve-Path -LiteralPath $cachedPasswordFile).Path
}

function Invoke-VBoxGuestControl {
    param(
        [Parameter(Mandatory = $true)] [string[]]$Arguments,
        [switch]$AllowFailure
    )

    $output = & $VBoxManage @Arguments 2>&1
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        throw "VBoxManage failed with exit code ${exitCode}: $($output -join "`n")"
    }
    return [pscustomobject]@{
        exit_code = $exitCode
        output = $output
    }
}

function Wait-GuestDesktopReady {
    param([Parameter(Mandatory = $true)] [int]$ReadyTimeoutSeconds)

    $deadline = (Get-Date).AddSeconds($ReadyTimeoutSeconds)
    $lastError = ""
    while ((Get-Date) -lt $deadline) {
        $result = Invoke-VBoxGuestControl `
            -Arguments @("guestcontrol", $VmName, "waitrunlevel", "desktop", "--timeout", "30000") `
            -AllowFailure
        if ($result.exit_code -eq 0) {
            return
        }
        $lastError = ($result.output -join "`n")
        Start-Sleep -Seconds 5
    }
    throw "Guest desktop did not become ready within $ReadyTimeoutSeconds seconds. Last error: $lastError"
}

function Get-GateConfiguration {
    param([Parameter(Mandatory = $true)] [string]$GateName)

    $artifactRoot = Join-Path $RepoRoot "artifacts\partition-manager-certification\vm-lab"
    switch ($GateName) {
        "linux-swap" {
            return [pscustomobject]@{
                launcher = (Join-Path $SharedRoot "scripts\launch_partition_manager_linux_swap_vm_gate_local.ps1")
                runner = (Join-Path $SharedRoot "scripts\run_partition_manager_linux_swap_vm_gate.ps1")
                report = (Join-Path $artifactRoot "external-evidence\external.linux-swap-format-vm\report.json")
                evidenceRoot = (Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\external-evidence\external.linux-swap-format-vm")
                arguments = @(
                    "-DiskNumber", $DiskNumber,
                    "-PageSizeBytes", $PageSizeBytes,
                    "-Label", $Label
                )
                directArguments = @(
                    "-ProjectRoot", $SharedRoot,
                    "-EvidenceRoot", (Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\external-evidence\external.linux-swap-format-vm"),
                    "-DiskNumber", $DiskNumber,
                    "-PageSizeBytes", $PageSizeBytes,
                    "-Label", $Label,
                    "-Force"
                )
            }
        }
        "ext-filesystem" {
            return [pscustomobject]@{
                launcher = (Join-Path $SharedRoot "scripts\launch_partition_manager_ext_filesystem_vm_gate_local.ps1")
                runner = (Join-Path $SharedRoot "scripts\run_partition_manager_ext_filesystem_vm_gate.ps1")
                report = (Join-Path $artifactRoot "external-evidence\external.ext-filesystem-write\report.json")
                evidenceRoot = (Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\external-evidence\external.ext-filesystem-write")
                arguments = @(
                    "-DiskNumber", $DiskNumber,
                    "-FileSystem", $FileSystem
                )
                directArguments = @(
                    "-ProjectRoot", $SharedRoot,
                    "-EvidenceRoot", (Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\external-evidence\external.ext-filesystem-write"),
                    "-DiskNumber", $DiskNumber,
                    "-FileSystem", $FileSystem,
                    "-Force"
                )
            }
        }
        "hdd-defrag" {
            return [pscustomobject]@{
                launcher = (Join-Path $SharedRoot "scripts\launch_partition_manager_hdd_defrag_external_gate_local.ps1")
                runner = (Join-Path $SharedRoot "scripts\run_partition_manager_hdd_defrag_external_gate.ps1")
                report = (Join-Path $artifactRoot "external-evidence\external.hdd-defrag-execution\report.json")
                evidenceRoot = (Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\external-evidence\external.hdd-defrag-execution")
                arguments = @(
                    "-TargetDiskNumber", $DiskNumber
                )
                directArguments = @(
                    "-TargetDiskNumber", $DiskNumber,
                    "-EvidenceDir", (Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\external-evidence\external.hdd-defrag-execution"),
                    "-GuestReportPath", (Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\external-hdd-defrag-guest-report.json"),
                    "-Force"
                )
            }
        }
    }
}

if (-not (Test-Path -LiteralPath $VBoxManage -PathType Leaf)) {
    throw "VBoxManage not found: $VBoxManage"
}

$gateConfig = Get-GateConfiguration -GateName $Gate
if ($NoCleanup) {
    $gateConfig.arguments += "-NoCleanup"
    $gateConfig.directArguments += "-NoCleanup"
}

$reportPath = $gateConfig.report
$reportDir = Split-Path -Parent $reportPath
New-Item -ItemType Directory -Path $reportDir -Force | Out-Null

$hostReportDir = Join-Path $RepoRoot "artifacts\partition-manager-certification\vm-lab\host-launch"
New-Item -ItemType Directory -Path $hostReportDir -Force | Out-Null
$hostReportPath = Join-Path $hostReportDir ("{0}-{1}.json" -f $Gate, (Get-Date -Format "yyyyMMddHHmmss"))
$screenshotDir = Join-Path $hostReportDir ("screenshots-{0}-{1}" -f $Gate, (Get-Date -Format "yyyyMMddHHmmss"))
New-Item -ItemType Directory -Path $screenshotDir -Force | Out-Null

$startedAt = Get-Date
$passwordFilePath = Resolve-GuestPasswordFile `
    -VBoxManagePath $VBoxManage `
    -MachineName $VmName `
    -UserName $GuestUser `
    -ExplicitPasswordFile $PasswordFile

$vmInfo = Get-VBoxMachineInfo -VBoxManagePath $VBoxManage -MachineName $VmName
if ($vmInfo["VMState"] -ne "running") {
    Invoke-VBoxGuestControl -Arguments @("startvm", $VmName, "--type", "headless") | Out-Null
}

Wait-GuestDesktopReady -ReadyTimeoutSeconds $GuestReadyTimeoutSeconds

$authArgs = @("guestcontrol", $VmName, "run", "--username", $GuestUser, "--passwordfile", $passwordFilePath)
if ($GuestDomain) {
    $authArgs += @("--domain", $GuestDomain)
}
$authArgs += @("--exe", "C:\Windows\System32\cmd.exe", "--", "/c", "whoami")
$authProbe = Invoke-VBoxGuestControl -Arguments $authArgs
$whoami = ($authProbe.output | Select-Object -First 1)
if (-not $whoami) {
    throw "Guest auth probe returned no user identity."
}

$adminProbeCommand = @"
`$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (`$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { 'admin=true' } else { 'admin=false' }
"@
$adminProbeArguments = @(
    "guestcontrol", $VmName, "run",
    "--username", $GuestUser,
    "--passwordfile", $passwordFilePath,
    "--exe", "C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe",
    "--",
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-Command", $adminProbeCommand
)
if ($GuestDomain) {
    $adminProbeArguments = @(
        "guestcontrol", $VmName, "run",
        "--username", $GuestUser,
        "--passwordfile", $passwordFilePath,
        "--domain", $GuestDomain,
        "--exe", "C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe",
        "--",
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-Command", $adminProbeCommand
    )
}
$adminProbe = Invoke-VBoxGuestControl -Arguments $adminProbeArguments
# Authorize direct destructive mode only on an EXACT 'admin=true' line from the probe, never a
# substring found anywhere in the guest output (which could carry the token incidentally).
$directAdmin = (@($adminProbe.output | ForEach-Object { "$_".Trim() }) -contains "admin=true")

$uacAcceptedAt = $null
$launchMode = "direct-admin"

function Write-HostLaunchReport {
    param(
        [Parameter(Mandatory = $true)] [bool]$Completed,
        [string]$ReportStatus,
        [string]$ReportGateId,
        [string]$FailureReason = ""
    )

    $hostReport = [pscustomobject]@{
        schema_version = 1
        gate = $Gate
        vm_name = $VmName
        guest_user = $GuestUser
        guest_identity = $whoami
        launch_mode = $launchMode
        direct_admin_token = $directAdmin
        started_at = $startedAt.ToUniversalTime().ToString("o")
        uac_accept_sent_at = if ($uacAcceptedAt) { $uacAcceptedAt.ToUniversalTime().ToString("o") } else { $null }
        finished_at = (Get-Date).ToUniversalTime().ToString("o")
        completed = $Completed
        report_path = $reportPath
        report_status = $ReportStatus
        report_gate_id = $ReportGateId
        failure_reason = $FailureReason
        password_file_used = $true
        password_file_path_redacted = $true
        password_value_redacted = $true
        screenshot_dir = $screenshotDir
    }
    $hostReport | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $hostReportPath -Encoding UTF8
}

if ($directAdmin) {
    if (Test-Path -LiteralPath $reportPath -PathType Leaf) {
        Remove-Item -LiteralPath $reportPath -Force
    }
    $runnerArguments = @(
        "guestcontrol", $VmName, "run",
        "--username", $GuestUser,
        "--passwordfile", $passwordFilePath,
        "--timeout", ([string]($TimeoutSeconds * 1000)),
        "--exe", "C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe",
        "--",
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", $gateConfig.runner
    )
    if ($GuestDomain) {
        $runnerArguments = @(
            "guestcontrol", $VmName, "run",
            "--username", $GuestUser,
            "--passwordfile", $passwordFilePath,
            "--domain", $GuestDomain,
            "--timeout", ([string]($TimeoutSeconds * 1000)),
            "--exe", "C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe",
            "--",
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-File", $gateConfig.runner
        )
    }
    $runnerArguments += $gateConfig.directArguments
    Invoke-VBoxGuestControl -Arguments $runnerArguments | Out-Null
}
elseif ($AllowUacKeypressFallback) {
    $launchMode = "uac-keypress-fallback"
    if (Test-Path -LiteralPath $reportPath -PathType Leaf) {
        Remove-Item -LiteralPath $reportPath -Force
    }
    $launcherArguments = @(
        "guestcontrol", $VmName, "start",
        "--username", $GuestUser,
        "--passwordfile", $passwordFilePath
    )
    if ($GuestDomain) {
        $launcherArguments += @("--domain", $GuestDomain)
    }
    $launcherArguments += @(
        "--exe", "C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe",
        "--",
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", $gateConfig.launcher
    )
    $launcherArguments += $gateConfig.arguments

    Invoke-VBoxGuestControl -Arguments $launcherArguments | Out-Null

    Start-Sleep -Seconds $UacDelaySeconds
    for ($attempt = 1; $attempt -le $UacAcceptAttempts; $attempt++) {
        $screenshot = Join-Path $screenshotDir ("uac-attempt-{0}.png" -f $attempt)
        Invoke-VBoxGuestControl -Arguments @("controlvm", $VmName, "screenshotpng", $screenshot) -AllowFailure | Out-Null
        Invoke-VBoxGuestControl -Arguments @("controlvm", $VmName, "keyboardputscancode", "38", "15", "95", "b8") | Out-Null
        if (-not $uacAcceptedAt) {
            $uacAcceptedAt = Get-Date
        }
        Start-Sleep -Seconds $UacAcceptIntervalSeconds
        if (Test-Path -LiteralPath $reportPath -PathType Leaf) {
            break
        }
    }
}
else {
    $launchMode = "auth-preflight-failed"
    Write-HostLaunchReport `
        -Completed $false `
        -ReportStatus "auth-preflight-failed" `
        -ReportGateId $null `
        -FailureReason "Guestcontrol auth works, but the token is filtered and not elevated."
    throw "Guestcontrol auth works for $whoami, but the token is not elevated. Configure a dedicated automation VM/account with UAC admin filtering disabled, or rerun with -AllowUacKeypressFallback only for manual emergency use."
}

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
while ((Get-Date) -lt $deadline) {
    if (Test-Path -LiteralPath $reportPath -PathType Leaf) {
        break
    }
    Start-Sleep -Seconds 5
}

$completed = Test-Path -LiteralPath $reportPath -PathType Leaf
$reportFresh = $true
if ($completed) {
    # Bind the report to THIS run: a report older than the run start is stale evidence from a
    # prior or concurrent run and must not certify this one.
    $reportFresh = ((Get-Item -LiteralPath $reportPath).LastWriteTimeUtc -ge $startedAt.ToUniversalTime())
}
$reportStatus = $null
$reportGateId = $null
if ($completed -and $reportFresh) {
    try {
        $reportJson = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
        $reportStatus = $reportJson.status
        $reportGateId = $reportJson.gate_id
    }
    catch {
        $reportStatus = "unreadable: $($_.Exception.Message)"
    }
}

$finalScreenshot = Join-Path $screenshotDir "final.png"
Invoke-VBoxGuestControl -Arguments @("controlvm", $VmName, "screenshotpng", $finalScreenshot) -AllowFailure | Out-Null
Invoke-VBoxGuestControl -Arguments @("guestcontrol", $VmName, "closesession", "--all") -AllowFailure | Out-Null

$hostCompleted = ($completed -and $reportFresh)
$hostFailureReason = ""
if (-not $completed) {
    $hostFailureReason = "No report produced within $TimeoutSeconds seconds."
}
elseif (-not $reportFresh) {
    $hostFailureReason = "Report predates run start; stale evidence refused."
}
elseif ($reportStatus -ne "passed") {
    $hostFailureReason = "Report status is '$reportStatus' (expected 'passed')."
}

Write-HostLaunchReport -Completed $hostCompleted -ReportStatus $reportStatus -ReportGateId $reportGateId -FailureReason $hostFailureReason

if (-not $completed) {
    throw "VM gate $Gate did not produce report within $TimeoutSeconds seconds. Host report: $hostReportPath"
}
if (-not $reportFresh) {
    throw "VM gate $Gate report is stale (predates run start). Host report: $hostReportPath"
}
# Fail closed on ANY status that is not exactly 'passed' -- a missing, empty, or null status
# must never print completion.
if ($reportStatus -ne "passed") {
    throw "VM gate $Gate report status is '${reportStatus}' (expected 'passed'). Host report: $hostReportPath"
}

Write-Host "VM gate $Gate completed. Report: $reportPath"
Write-Host "Host launch report: $hostReportPath"

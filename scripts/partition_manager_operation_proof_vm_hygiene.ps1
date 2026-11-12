<#
.SYNOPSIS
    Quietens the VirtualBox guest after its snapshot is restored, before any operation-proof row:
    the same hygiene the certification applied on 2026-11-09 (docs/RELEASE_READINESS.md, the third
    stop). Runs elevated in the guest through the rig's GuestJob.

.DESCRIPTION
    pre-round8 has Windows Update on. On 2026-11-12 it installed a cumulative update during the
    Image-route migration row, its own snapshots filled the shadow storage, VSS deleted the
    migration's snapshot mid-capture (volsnap 33, DISM exit 433), and the guest restarted twice.
    This turns off automatic updates, the Store's downloads, the update and Edge-update services and
    tasks, and the System Restore task; TrustedInstaller stays on Manual, since a DISM or servicing
    step may need it. It also reports the kernel dump settings. Nothing here outlives the next
    restore of the snapshot, and the snapshot itself is never changed.
#>
$ErrorActionPreference = 'Continue'
function Write-Hygiene([string]$Text) { "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $Text" }

foreach ($policy in @(
        @{ Path = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU'; Name = 'NoAutoUpdate'; Value = 1 },
        @{ Path = 'HKLM:\SOFTWARE\Policies\Microsoft\WindowsStore'; Name = 'AutoDownload'; Value = 2 })) {
    New-Item -Path $policy.Path -Force | Out-Null
    Set-ItemProperty -Path $policy.Path -Name $policy.Name -Value $policy.Value -Type DWord
    Write-Hygiene "policy $($policy.Path) $($policy.Name)=$((Get-ItemProperty -Path $policy.Path).($policy.Name))"
}
foreach ($name in 'wuauserv', 'UsoSvc', 'InstallService', 'edgeupdate', 'edgeupdatem', 'TrustedInstaller') {
    $service = Get-Service -Name $name -ErrorAction SilentlyContinue
    if (-not $service) { Write-Hygiene "service ${name}: not present"; continue }
    Stop-Service -Name $name -Force -ErrorAction SilentlyContinue
    $start = if ($name -eq 'TrustedInstaller') { 'Manual' } else { 'Disabled' }
    Set-Service -Name $name -StartupType $start -ErrorAction SilentlyContinue
    $service = Get-Service -Name $name
    Write-Hygiene "service ${name}: status $($service.Status), start $($service.StartType)"
}
$tasks = @(Get-ScheduledTask -TaskPath '\Microsoft\Windows\SystemRestore\' -TaskName 'SR' -ErrorAction SilentlyContinue) +
    @(Get-ScheduledTask -ErrorAction SilentlyContinue | Where-Object { $_.TaskName -like 'MicrosoftEdgeUpdate*' })
foreach ($task in $tasks) {
    if ($task.State -eq 'Running') { Stop-ScheduledTask -TaskPath $task.TaskPath -TaskName $task.TaskName -ErrorAction SilentlyContinue }
    $done = Disable-ScheduledTask -TaskPath $task.TaskPath -TaskName $task.TaskName -ErrorAction SilentlyContinue
    Write-Hygiene "task $($task.TaskPath)$($task.TaskName): $(if ($done) { $done.State } else { 'not changed' })"
}
$crash = Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl'
Write-Hygiene "crash dumps: CrashDumpEnabled=$($crash.CrashDumpEnabled) AutoReboot=$($crash.AutoReboot) DedicatedDumpFile=$($crash.DedicatedDumpFile)"
Write-Hygiene "shadow storage: $((& vssadmin.exe list shadowstorage 2>&1 | Select-String 'Maximum Shadow Copy Storage space') -join ' ')"

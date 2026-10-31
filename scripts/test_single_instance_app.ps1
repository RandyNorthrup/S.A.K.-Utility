<#
.SYNOPSIS
    The real application keeps one instance per data folder, and an elevated relaunch waits for
    the instance that launched it instead of bringing it forward.

.DESCRIPTION
    Runs the application this build produced against its own data folder (build\Release\data),
    offscreen so no window reaches the desktop:
      1. A plain launch (A) stays open as the instance for the folder.
      2. A second launch (B, a startup smoke) finds A, asks it to come forward and exits 0
         without starting: no SAK_STARTUP_SMOKE_OK, an "already running" line on stderr, A alive.
      3. A launch carrying --elevated-relaunch (C, what Run as Admin starts) neither starts nor
         exits while A runs; once A ends, C becomes the instance and its smoke completes.
    The UAC prompt itself cannot run unattended; C carries the same argument the elevated copy
    receives, so the handoff it exercises is the one Run as Admin uses.
#>
[CmdletBinding()]
param(
    # The application the configured build produced ($<TARGET_FILE:sak_utility>).
    [string]$ExePath = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
. (Join-Path $ProjectRoot 'scripts\assert_built_target.ps1')
. (Join-Path $ProjectRoot 'scripts\process_capture.ps1')
$exe = Assert-FreshBuiltTarget -Path $ExePath
$exeDir = Split-Path -Parent $exe
$env:QT_QPA_PLATFORM = 'offscreen'

$readyTimeoutSeconds = 90
$launchTimeoutSeconds = 90
$heldMilliseconds = 3000
$logDirectory = Join-Path $exeDir 'data\logs'

function Start-App([string[]]$Arguments) {
    $info = [System.Diagnostics.ProcessStartInfo]::new($exe)
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.WorkingDirectory = $exeDir
    $process = [System.Diagnostics.Process]::Start($info)
    return [pscustomobject]@{
        process = $process
        stdout = $process.StandardOutput.ReadToEndAsync()
        stderr = $process.StandardError.ReadToEndAsync()
    }
}

function Get-SessionLogNames {
    if (-not (Test-Path -LiteralPath $logDirectory -PathType Container)) { return @() }
    return @(Get-ChildItem -LiteralPath $logDirectory -Filter 'sak_*.log' -File | ForEach-Object Name)
}

# The app opens its session log only after it has claimed the instance for its data folder
# (runApplication: claimSingleInstance, then the logger), so A's new log file is the sign that A
# is the instance. Its info lines are buffered, so the file's content is not waited for.
function Wait-InstanceClaimed($App, [string[]]$LogsBefore) {
    $known = [Collections.Generic.HashSet[string]]::new([string[]]$LogsBefore)
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    while ($watch.Elapsed.TotalSeconds -lt $readyTimeoutSeconds) {
        if ($App.process.HasExited) {
            throw "The first instance exited ($($App.process.ExitCode)) before it started: $($App.stderr.Result)"
        }
        if (@(Get-SessionLogNames | Where-Object { -not $known.Contains($_) }).Count -gt 0) { return }
        Start-Sleep -Milliseconds 200
    }
    throw "The first instance did not start within $readyTimeoutSeconds s."
}

function Stop-App($App) {
    if ($null -ne $App -and -not $App.process.HasExited) {
        $App.process.Kill($true)
        [void]$App.process.WaitForExit(30000)
    }
}

$first = $null
$relaunch = $null
try {
    $logsBefore = @(Get-SessionLogNames)
    $first = Start-App @('--no-splash')
    Wait-InstanceClaimed $first $logsBefore

    $second = Invoke-CapturedProcess -FilePath $exe -Arguments @('--smoke-test', '--no-splash') `
        -WorkingDirectory $exeDir -TimeoutSeconds $launchTimeoutSeconds
    if ($second.timed_out) { throw 'A second launch did not exit while the first instance ran.' }
    if ($second.exit_code -ne 0) {
        throw "A second launch exited $($second.exit_code): $($second.stderr)"
    }
    if ($second.stdout -match 'SAK_STARTUP_SMOKE_OK') {
        throw 'A second launch started its own instance beside the running one.'
    }
    if ($second.stderr -notmatch 'already running') {
        throw "A second launch did not say the application is already running: $($second.stderr)"
    }
    if ($first.process.HasExited) {
        throw "The first instance exited ($($first.process.ExitCode)) when a second launch asked it forward."
    }

    $relaunch = Start-App @('--smoke-test', '--no-splash', '--elevated-relaunch')
    if ($relaunch.process.WaitForExit($heldMilliseconds)) {
        throw ("An elevated relaunch ended while the instance that launched it still ran " +
            "(exit $($relaunch.process.ExitCode)): $($relaunch.stderr.Result)")
    }
    Stop-App $first
    if (-not $relaunch.process.WaitForExit($launchTimeoutSeconds * 1000)) {
        throw 'An elevated relaunch did not start once the instance that launched it ended.'
    }
    $relaunch.process.WaitForExit()
    if ($relaunch.process.ExitCode -ne 0 -or $relaunch.stdout.Result -notmatch 'SAK_STARTUP_SMOKE_OK') {
        throw ("An elevated relaunch did not run once its launcher ended (exit " +
            "$($relaunch.process.ExitCode)): $($relaunch.stdout.Result) $($relaunch.stderr.Result)")
    }
}
finally {
    Stop-App $relaunch
    Stop-App $first
}

Write-Host ('Single-instance test passed: a second launch brought the running instance forward ' +
    'and exited; an elevated relaunch waited for its launcher and then started.')
exit 0

# Performance budget (G23-3): boot the app headless via its --smoke-test path and fail
# if cold startup exceeds a wall-clock budget. This catches a startup regression like
# the DirectWrite font-database one that pushed startup from ~1s to 31s and was noticed
# only by a human using the app -- no gate saw it. Normal offscreen smoke startup is ~2s.
#
# Fails closed: a missing exe, a smoke run that never prints SAK_STARTUP_SMOKE_OK, a
# non-zero exit, or an over-budget / hung run are all hard failures.
param(
    [ValidateRange(1, 2147483647)]
    [int]$BudgetMs = 15000,
    [ValidateRange(1, 3600)]
    [int]$HardTimeoutSec = 60,
    [string]$ExePath
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$processCaptureScript = Join-Path $PSScriptRoot "process_capture.ps1"
if (-not (Test-Path -LiteralPath $processCaptureScript -PathType Leaf)) {
    throw "Startup budget captured-process helper not found: $processCaptureScript"
}
. $processCaptureScript

if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $ExePath = Join-Path $root "build\Release\sak_utility.exe"
}
if (-not (Test-Path -LiteralPath $ExePath -PathType Leaf)) {
    throw "Startup budget: executable not found at ${ExePath}; build before running this gate."
}

$env:QT_QPA_PLATFORM = "offscreen"
$env:SAK_STARTUP_SMOKE_CI_HEADLESS = "1"

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$result = Invoke-CapturedProcess -FilePath $ExePath -Arguments @("--smoke-test", "--no-splash") `
    -WorkingDirectory (Split-Path -Parent $ExePath) -TimeoutSeconds $HardTimeoutSec
$sw.Stop()

if ($result.timed_out) {
    throw "Startup budget: smoke run did not exit within ${HardTimeoutSec}s (hung startup)."
}
$elapsedMs = [int]$sw.Elapsed.TotalMilliseconds

if ($result.exit_code -ne 0) {
    throw ("Startup budget: smoke run exited $($result.exit_code) (expected 0). " +
        "Stdout:`n$($result.stdout)`nStderr:`n$($result.stderr)")
}
if ($result.stdout -notmatch "SAK_STARTUP_SMOKE_OK") {
    throw ("Startup budget: smoke run did not report SAK_STARTUP_SMOKE_OK. " +
        "Stdout:`n$($result.stdout)`nStderr:`n$($result.stderr)")
}
if ($elapsedMs -gt $BudgetMs) {
    throw "Startup budget EXCEEDED: cold smoke startup took ${elapsedMs} ms > ${BudgetMs} ms budget (a startup regression)."
}

Write-Host "Startup budget passed: cold smoke startup ${elapsedMs} ms (budget ${BudgetMs} ms)."
exit 0

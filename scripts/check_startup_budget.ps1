# Performance budget (G23-3): boot the app headless via its --smoke-test path and fail
# if cold startup exceeds a wall-clock budget. This catches a startup regression like
# the DirectWrite font-database one that pushed startup from ~1s to 31s and was noticed
# only by a human using the app -- no gate saw it. Normal offscreen smoke startup is ~2s.
#
# Fails closed: a missing exe, a smoke run that never prints SAK_STARTUP_SMOKE_OK, a
# non-zero exit, or an over-budget / hung run are all hard failures.
param(
    [int]$BudgetMs = 15000,
    [int]$HardTimeoutSec = 60,
    [string]$ExePath
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $ExePath = Join-Path $root "build\Release\sak_utility.exe"
}
if (-not (Test-Path -LiteralPath $ExePath -PathType Leaf)) {
    throw "Startup budget: executable not found at ${ExePath}; build before running this gate."
}

$env:QT_QPA_PLATFORM = "offscreen"
$env:SAK_STARTUP_SMOKE_CI_HEADLESS = "1"

$outFile = Join-Path ([System.IO.Path]::GetTempPath()) "sak_startup_smoke.out"
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$proc = Start-Process -FilePath $ExePath -ArgumentList @("--smoke-test", "--no-splash") `
    -NoNewWindow -PassThru -RedirectStandardOutput $outFile
# Touch .Handle so $proc.ExitCode is populated after exit (Start-Process -PassThru quirk).
$null = $proc.Handle
if (-not $proc.WaitForExit($HardTimeoutSec * 1000)) {
    try { $proc.Kill() } catch {}
    throw "Startup budget: smoke run did not exit within ${HardTimeoutSec}s (hung startup)."
}
$sw.Stop()
$elapsedMs = [int]$sw.Elapsed.TotalMilliseconds
$stdout = if (Test-Path -LiteralPath $outFile) { Get-Content -LiteralPath $outFile -Raw } else { "" }

if ($proc.ExitCode -ne 0) {
    throw "Startup budget: smoke run exited ${($proc.ExitCode)} (expected 0). Output:`n$stdout"
}
if ($stdout -notmatch "SAK_STARTUP_SMOKE_OK") {
    throw "Startup budget: smoke run did not report SAK_STARTUP_SMOKE_OK. Output:`n$stdout"
}
if ($elapsedMs -gt $BudgetMs) {
    throw "Startup budget EXCEEDED: cold smoke startup took ${elapsedMs} ms > ${BudgetMs} ms budget (a startup regression)."
}

Write-Host "Startup budget passed: cold smoke startup ${elapsedMs} ms (budget ${BudgetMs} ms)."
exit 0

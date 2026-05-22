<#
.SYNOPSIS
    Runs a deterministic startup smoke test against a portable package folder.

.DESCRIPTION
    Launches sak_utility.exe with --smoke-test so the real application
    initializes Qt, portable paths, logging, and the main window, then exits
    automatically. This catches missing Qt plugins and startup regressions that
    static package checks cannot see.
#>

param(
    [string]$PackageRoot = "build\Release",
    [int]$TimeoutSeconds = 30
)

$ErrorActionPreference = "Stop"

$root = Resolve-Path -LiteralPath $PackageRoot
$exe = Join-Path $root.Path "sak_utility.exe"
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "sak_utility.exe not found under package root: $($root.Path)"
}

$startedAt = Get-Date
$runtimeRelativePaths = @("data\ai_sessions", "data\temp", "data\logs", "data\config", "_logs")
$preexistingRuntimePaths = @{}
foreach ($relativePath in $runtimeRelativePaths) {
    $runtimePath = Join-Path $root.Path $relativePath
    $preexistingRuntimePaths[$relativePath] = Test-Path -LiteralPath $runtimePath
}
$stdout = Join-Path $env:TEMP ("sak_startup_smoke_{0}.out.txt" -f [guid]::NewGuid().ToString("N"))
$stderr = Join-Path $env:TEMP ("sak_startup_smoke_{0}.err.txt" -f [guid]::NewGuid().ToString("N"))

try {
    $process = Start-Process `
        -FilePath $exe `
        -ArgumentList @("--smoke-test", "--no-splash") `
        -WorkingDirectory $root.Path `
        -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr `
        -WindowStyle Hidden `
        -PassThru

    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        if ($IsWindows -or $env:OS -eq "Windows_NT") {
            & taskkill.exe /PID $process.Id /T /F | Out-Null
        } else {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
        throw "Startup smoke timed out after $TimeoutSeconds seconds"
    }

    # Ensure redirected output drains and refresh best-effort exit metadata.
    $process.WaitForExit()
    $process.Refresh()

    $outText = if (Test-Path -LiteralPath $stdout) { Get-Content -LiteralPath $stdout -Raw } else { "" }
    $errText = if (Test-Path -LiteralPath $stderr) { Get-Content -LiteralPath $stderr -Raw } else { "" }
    if ($null -ne $process.ExitCode -and $process.ExitCode -ne 0) {
        throw "Startup smoke failed with exit code $($process.ExitCode)`nSTDOUT:`n$outText`nSTDERR:`n$errText"
    }

    $logsDir = Join-Path $root.Path "data\logs"
    if (-not (Test-Path -LiteralPath $logsDir -PathType Container)) {
        throw "Startup smoke did not create portable log directory: $logsDir"
    }

    $recentLog = Get-ChildItem -LiteralPath $logsDir -File -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $startedAt.AddSeconds(-5) } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $recentLog) {
        throw "Startup smoke did not create or update a recent log file under $logsDir"
    }
    $recentLogText = Get-Content -LiteralPath $recentLog.FullName -Raw
    if ($recentLogText -notmatch "Application shutting down with exit code: 0") {
        throw "Startup smoke log did not record clean shutdown: $($recentLog.FullName)`nSTDOUT:`n$outText`nSTDERR:`n$errText"
    }

    foreach ($relativePath in $runtimeRelativePaths) {
        $runtimePath = Join-Path $root.Path $relativePath
        if (-not $preexistingRuntimePaths[$relativePath] -and (Test-Path -LiteralPath $runtimePath)) {
            Remove-Item -LiteralPath $runtimePath -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    Write-Host "Portable startup E2E smoke passed: $($root.Path)"
}
finally {
    Remove-Item -LiteralPath $stdout, $stderr -Force -ErrorAction SilentlyContinue
}

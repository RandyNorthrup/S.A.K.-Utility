<#
.SYNOPSIS
    Runs a deterministic startup smoke test against a portable package folder.

.DESCRIPTION
    Launches sak_utility.exe with --smoke-test so the real application
    initializes Qt, portable paths, logging, and the full main-window object
    tree, then exits automatically. In CI, the app skips showing the native
    window and exits immediately after the clean startup/shutdown marker to
    avoid hosted-runner Qt/widget teardown false negatives while still catching
    missing Qt plugins, bundle issues, and startup regressions that static
    package checks cannot see.
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

function Get-RecentSmokeLogText {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RootPath,
        [Parameter(Mandatory = $true)]
        [datetime]$StartedAt
    )

    $logsDir = Join-Path $RootPath "data\logs"
    if (-not (Test-Path -LiteralPath $logsDir -PathType Container)) {
        return "No portable log directory found: $logsDir"
    }

    $recentLogs = Get-ChildItem -LiteralPath $logsDir -File -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $StartedAt.AddSeconds(-10) } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 3
    if (-not $recentLogs) {
        return "No recent portable logs found under $logsDir"
    }

    $chunks = foreach ($log in $recentLogs) {
        $text = Get-Content -LiteralPath $log.FullName -Tail 120 -ErrorAction SilentlyContinue |
            Out-String
        "=== $($log.FullName) ===`n$text"
    }
    return ($chunks -join "`n")
}

try {
    # The child inherits THIS process's environment. Every key below is applied
    # deterministically: a non-null value is SET, and a $null value is CLEARED (not skipped),
    # so an inherited value can never silently change what the smoke run exercises.
    #   SAK_STARTUP_SMOKE_HEADLESS - "1" only under CI; cleared otherwise so a stale local
    #     value cannot select the weaker headless path in a normal developer run.
    #   QT_PLUGIN_PATH / QT_QPA_PLATFORM_PLUGIN_PATH - cleared so a host Qt installation cannot
    #     supply plugins the portable bundle is missing (which would let a broken bundle pass).
    $headlessValue = if ($env:GITHUB_ACTIONS -eq "true" -or $env:CI -eq "true") { "1" } else { $null }
    $childEnvironment = [ordered]@{
        "SAK_STARTUP_SMOKE_HEADLESS"  = $headlessValue
        "QT_PLUGIN_PATH"              = $null
        "QT_QPA_PLATFORM_PLUGIN_PATH" = $null
    }
    $previousEnvironment = @{}
    foreach ($entry in $childEnvironment.GetEnumerator()) {
        $previousEnvironment[$entry.Key] = [Environment]::GetEnvironmentVariable($entry.Key, "Process")
        # A $null value clears the variable for the child; a non-null value sets it. Either
        # way the child's value is deterministic rather than inherited from the caller.
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, "Process")
    }

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
            # System32-qualified so an attacker-controlled PATH cannot substitute taskkill.
            & "$env:SystemRoot\System32\taskkill.exe" /PID $process.Id /T /F | Out-Null
        } else {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
        # Confirm the child actually died before surfacing the timeout: a wedged smoke process
        # must not keep running (and possibly mutating state) after the gate returns.
        if (-not $process.WaitForExit(5000)) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            $process.WaitForExit(5000) | Out-Null
        }
        $liveness = if ($process.HasExited) { "" } else {
            " (WARNING: smoke process $($process.Id) did not terminate)"
        }
        $recentLogText = Get-RecentSmokeLogText -RootPath $root.Path -StartedAt $startedAt
        throw "Startup smoke timed out after $TimeoutSeconds seconds$liveness`nRECENT LOGS:`n$recentLogText"
    }

    # Ensure redirected output drains and refresh best-effort exit metadata.
    $process.WaitForExit()
    $process.Refresh()

    $outText = if (Test-Path -LiteralPath $stdout) { Get-Content -LiteralPath $stdout -Raw } else { "" }
    $errText = if (Test-Path -LiteralPath $stderr) { Get-Content -LiteralPath $stderr -Raw } else { "" }
    # Fail closed if the exit code cannot be read: an unconfirmable exit is not a clean exit.
    if ($null -eq $process.ExitCode) {
        $recentLogText = Get-RecentSmokeLogText -RootPath $root.Path -StartedAt $startedAt
        throw "Startup smoke could not read the process exit code (cannot confirm a clean exit)`nSTDOUT:`n$outText`nSTDERR:`n$errText`nRECENT LOGS:`n$recentLogText"
    }
    if ($process.ExitCode -ne 0) {
        $recentLogText = Get-RecentSmokeLogText -RootPath $root.Path -StartedAt $startedAt
        throw "Startup smoke failed with exit code $($process.ExitCode)`nSTDOUT:`n$outText`nSTDERR:`n$errText`nRECENT LOGS:`n$recentLogText"
    }

    $logsDir = Join-Path $root.Path "data\logs"
    if (-not (Test-Path -LiteralPath $logsDir -PathType Container)) {
        throw "Startup smoke did not create portable log directory: $logsDir"
    }

    $recentLog = Get-ChildItem -LiteralPath $logsDir -Filter "sak_*.log" -File -ErrorAction SilentlyContinue |
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

    # Restore the package to its pre-run state by removing ONLY the runtime directories this
    # run created. Two fail-closed guards: (1) refuse to recurse through a reparse point
    # (junction/symlink), because Remove-Item -Recurse can delete the link TARGET's contents,
    # so a planted junction must not delete data elsewhere; (2) surface any removal failure
    # instead of silently leaving the package non-pristine for later verification steps.
    $cleanupErrors = @()
    foreach ($relativePath in $runtimeRelativePaths) {
        $runtimePath = Join-Path $root.Path $relativePath
        if ($preexistingRuntimePaths[$relativePath] -or -not (Test-Path -LiteralPath $runtimePath)) {
            continue
        }
        $item = Get-Item -LiteralPath $runtimePath -Force -ErrorAction SilentlyContinue
        if ($null -ne $item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            $cleanupErrors += "refused to delete a reparse point: $runtimePath"
            continue
        }
        try {
            Remove-Item -LiteralPath $runtimePath -Recurse -Force -ErrorAction Stop
        } catch {
            $cleanupErrors += "could not remove ${runtimePath}: $($_.Exception.Message)"
        }
    }
    if ($cleanupErrors.Count -gt 0) {
        throw "Startup smoke passed but runtime cleanup failed (package left non-pristine):`n$($cleanupErrors -join "`n")"
    }

    Write-Host "Portable startup E2E smoke passed: $($root.Path)"
}
finally {
    if ($previousEnvironment) {
        foreach ($entry in $previousEnvironment.GetEnumerator()) {
            [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, "Process")
        }
    }
    Remove-Item -LiteralPath $stdout, $stderr -Force -ErrorAction SilentlyContinue
}

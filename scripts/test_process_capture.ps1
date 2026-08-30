<#
.SYNOPSIS
    Self-tests scripts/process_capture.ps1 under the current PowerShell host.
#>

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "process_capture.ps1")

function Assert-Equal($Actual, $Expected, [string]$Message) {
    if ($Actual -ne $Expected) {
        throw "$Message (expected '$Expected', got '$Actual')"
    }
}

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) {
        throw $Message
    }
}

$shell = [System.Diagnostics.Process]::GetCurrentProcess().MainModule.FileName

$captured = Invoke-CapturedProcess -FilePath $shell -Arguments @(
    "-NoProfile",
    "-NonInteractive",
    "-Command",
    "[Console]::Out.Write('stdout marker'); " +
        "[Console]::Error.Write('stderr marker'); exit 7"
)
Assert-Equal $captured.exit_code 7 "Non-zero child exit code was not captured"
Assert-Equal $captured.stdout "stdout marker" "Standard output was not captured"
Assert-Equal $captured.stderr "stderr marker" "Standard error was not captured"
Assert-True (-not $captured.timed_out) "Completed child was reported as timed out"

$allowedRejected = $false
try {
    Invoke-CapturedProcess -FilePath $shell -Arguments @(
        "-NoProfile", "-NonInteractive", "-Command", "exit 7"
    ) -AllowedExitCodes @(0) | Out-Null
} catch {
    $allowedRejected = $_.Exception.Message -match 'Command failed \(7\)'
}
Assert-True $allowedRejected "Disallowed exit code did not fail closed"

$largeOutput = Invoke-CapturedProcess -FilePath $shell -Arguments @(
    "-NoProfile",
    "-NonInteractive",
    "-Command",
    "[Console]::Out.Write(('O' * 200000)); " +
        "[Console]::Error.Write(('E' * 200000)); exit 0"
) -AllowedExitCodes @(0)
Assert-Equal $largeOutput.stdout.Length 200000 "Large standard output was truncated"
Assert-Equal $largeOutput.stderr.Length 200000 "Large standard error was truncated"

$timed = Invoke-CapturedProcess -FilePath $shell -Arguments @(
    "-NoProfile", "-NonInteractive", "-Command", "Start-Sleep -Seconds 5"
) -TimeoutSeconds 1
Assert-True $timed.timed_out "Timeout did not terminate and flag the child"

Write-Host "Process capture tests passed under $($PSVersionTable.PSVersion)."

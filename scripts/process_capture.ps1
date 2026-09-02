<#
.SYNOPSIS
    Cross-PowerShell native process capture with bounded waits and reliable exit codes.

.DESCRIPTION
    Uses System.Diagnostics.Process directly instead of Start-Process -PassThru. Windows
    PowerShell 5.1 can leave Start-Process.ExitCode null after redirected execution even when
    the child has exited. Standard output and error are drained asynchronously so a child that
    fills both pipes cannot deadlock the parent before WaitForExit.
#>

function ConvertTo-WindowsProcessArgument([string]$Argument) {
    if ($null -eq $Argument) {
        return '""'
    }
    if ($Argument.Length -gt 0 -and $Argument -notmatch '[\s"]') {
        return $Argument
    }

    $builder = [System.Text.StringBuilder]::new()
    [void]$builder.Append('"')
    $backslashes = 0
    foreach ($char in $Argument.ToCharArray()) {
        if ($char -eq '\') {
            $backslashes += 1
            continue
        }
        if ($char -eq '"') {
            [void]$builder.Append(('\' * (($backslashes * 2) + 1)))
            [void]$builder.Append('"')
            $backslashes = 0
            continue
        }
        if ($backslashes -gt 0) {
            [void]$builder.Append(('\' * $backslashes))
            $backslashes = 0
        }
        [void]$builder.Append($char)
    }
    if ($backslashes -gt 0) {
        [void]$builder.Append(('\' * ($backslashes * 2)))
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Join-ProcessArguments([string[]]$Arguments) {
    return ($Arguments | ForEach-Object {
            ConvertTo-WindowsProcessArgument -Argument $_
        }) -join ' '
}

function Stop-CapturedProcessTree([System.Diagnostics.Process]$Process) {
    if ($Process.HasExited) {
        return
    }
    $taskkillPath = Join-Path ([System.Environment]::SystemDirectory) 'taskkill.exe'
    if (-not (Test-Path -LiteralPath $taskkillPath -PathType Leaf)) {
        throw "Cannot terminate timed-out process tree; taskkill.exe is missing: $taskkillPath"
    }
    $terminationOutput = @(& $taskkillPath /PID ([string]$Process.Id) /T /F 2>&1)
    $terminationExitCode = $LASTEXITCODE
    $Process.Refresh()
    if ($terminationExitCode -ne 0 -and -not $Process.HasExited) {
        throw "Failed to terminate timed-out process tree $($Process.Id) " +
            "(taskkill exit $terminationExitCode): $($terminationOutput -join ' ')"
    }
}

function Invoke-CapturedProcess {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory = "",
        [ValidateRange(0, 2147483)]
        [int]$TimeoutSeconds = 0,
        [int[]]$AllowedExitCodes = @(),
        [AllowNull()][string]$InputText = $null
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = Join-ProcessArguments -Arguments $Arguments
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $hasInput = $null -ne $InputText
    $startInfo.RedirectStandardInput = $hasInput
    if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory)) {
        $startInfo.WorkingDirectory = $WorkingDirectory
    }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Process did not start: $FilePath"
    }

    try {
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if ($hasInput) {
            $process.StandardInput.Write($InputText)
            $process.StandardInput.Close()
        }
        $timedOut = $false
        if ($TimeoutSeconds -gt 0) {
            $timedOut = -not $process.WaitForExit($TimeoutSeconds * 1000)
        } else {
            $process.WaitForExit()
        }

        if ($timedOut) {
            Stop-CapturedProcessTree -Process $process
            if (-not $process.WaitForExit(5000)) {
                throw "Timed-out process did not terminate: $FilePath (pid $($process.Id))"
            }
        }

        # Parameterless WaitForExit completes asynchronous stream handling on .NET Framework.
        $process.WaitForExit()
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        $result = [pscustomobject]@{
            file = $FilePath
            args = $Arguments
            exit_code = $process.ExitCode
            stdout = $stdout.Trim()
            stderr = $stderr.Trim()
            timed_out = $timedOut
        }
        if (-not $timedOut -and $AllowedExitCodes.Count -gt 0 -and
            $AllowedExitCodes -notcontains $result.exit_code) {
            throw "Command failed ($($result.exit_code)): $FilePath $($Arguments -join ' ')`n$stderr"
        }
        return $result
    }
    finally {
        $process.Dispose()
    }
}

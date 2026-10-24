<#
.SYNOPSIS
    Cross-PowerShell native process capture with bounded waits and reliable exit codes.

.DESCRIPTION
    Uses System.Diagnostics.Process directly instead of Start-Process -PassThru. Windows
    PowerShell 5.1 can leave Start-Process.ExitCode null after redirected execution even when
    the child has exited. Owned cancellable I/O threads drain both pipes and write stdin.
    A finite post-launch deadline covers input, process exit and output completion. Timeout
    cleanup has one additional five-second budget. Zero preserves unlimited execution time,
    not unlimited output: each stream has a finite character budget and any worker failure
    interrupts every execution wait. Output overflow is an error, never successful truncation.
    Standard input is always redirected: supplied text is followed by EOF; omitted, null or
    empty input sends immediate EOF. No parent-console input is inherited.
    Optional environment overrides affect only the child: null removes a name, an empty
    string remains present-empty, and other strings replace inherited values.
    Parent exit is not proof that arbitrary background descendants are quiescent.
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

function Get-CaptureIoType {
    # ProcessStartInfo pipes use synchronous handles on .NET Framework. A timed
    # Task.Wait alone cannot cancel ReadToEndAsync's blocked thread. Own each I/O
    # thread and cancel its synchronous request; never abort a managed thread.
    $source = @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Microsoft.Win32.SafeHandles;
[assembly: DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
namespace SakCapture {
    public sealed class __TYPE__ : IDisposable {
        private const uint ThreadTerminate = 0x0001;
        private const int NoPendingRequest = 1168;
        private const int ChunkCharacters = 4096;
        public const int MaximumOutputCharacters = 16 * 1024 * 1024;
        private readonly StreamReader reader;
        private readonly StreamWriter writer;
        private readonly string input;
        private readonly int outputLimit;
        private readonly string streamName;
        private readonly Thread thread;
        private SafeWaitHandle nativeThread;
        private int cancelled;
        private bool disposed;
        private string output;
        private Exception failure;
        [DllImport("kernel32.dll", SetLastError=true)]
        private static extern SafeWaitHandle OpenThread(uint access, bool inherit, uint id);
        [DllImport("kernel32.dll")]
        private static extern uint GetCurrentThreadId();
        [DllImport("kernel32.dll", SetLastError=true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CancelSynchronousIo(SafeWaitHandle handle);
        private __TYPE__(StreamReader source, StreamWriter destination, string text, int limit, string name) {
            reader = source;
            writer = destination;
            input = text;
            outputLimit = limit;
            streamName = name;
            try {
                thread = new Thread(Run);
                thread.IsBackground = true;
                thread.Start();
            } catch (Exception startError) {
                try {
                    if (reader != null) reader.Dispose();
                    if (writer != null) writer.Dispose();
                } catch (Exception cleanupError) {
                    throw new AggregateException("Capture I/O startup and cleanup failed", startError, cleanupError);
                }
                throw;
            }
        }
        public static __TYPE__ Read(StreamReader source, int limit, string name) {
            if (source == null) throw new ArgumentNullException("source");
            if (limit < 1 || limit > MaximumOutputCharacters)
                throw new ArgumentOutOfRangeException("limit", "Capture output budget is outside its finite range");
            return new __TYPE__(source, null, null, limit, name);
        }
        public static __TYPE__ Write(StreamWriter destination, string text) {
            if (destination == null) throw new ArgumentNullException("destination");
            if (text == null) throw new ArgumentNullException("text");
            return new __TYPE__(null, destination, text, 0, null);
        }
        private void Run() {
            try {
                SafeWaitHandle handle = OpenThread(ThreadTerminate, false, GetCurrentThreadId());
                if (handle.IsInvalid) {
                    int error = Marshal.GetLastWin32Error();
                    handle.Dispose();
                    throw new Win32Exception(error, "Open owned capture I/O thread failed");
                }
                Interlocked.Exchange(ref nativeThread, handle);
                if (IsCancellationRequested) return;
                if (reader != null) ReadChunks();
                else WriteChunks();
            } catch (Exception error) { failure = error; }
        }
        private bool IsCancellationRequested {
            get { return Interlocked.CompareExchange(ref cancelled, 0, 0) != 0; }
        }
        private void ReadChunks() {
            char[] buffer = new char[ChunkCharacters];
            StringBuilder text = new StringBuilder(Math.Min(ChunkCharacters, outputLimit), outputLimit);
            while (!IsCancellationRequested) {
                int count = reader.Read(buffer, 0, buffer.Length);
                if (count == 0) break;
                if (count > outputLimit - text.Length)
                    throw new IOException("Capture " + streamName + " exceeded output limit of " + outputLimit + " characters");
                text.Append(buffer, 0, count);
            }
            output = text.ToString();
        }
        private void WriteChunks() {
            char[] buffer = new char[ChunkCharacters];
            int offset = 0;
            while (offset < input.Length && !IsCancellationRequested) {
                int count = Math.Min(buffer.Length, input.Length - offset);
                input.CopyTo(offset, buffer, 0, count);
                writer.Write(buffer, 0, count);
                offset += count;
            }
            if (!IsCancellationRequested) { writer.Flush(); writer.Close(); }
        }
        public bool IsCompleted { get { return !thread.IsAlive; } }
        private static bool StreamClosed(Stream stream) {
            if (stream == null) return true;
            try {
                FileStream file = stream as FileStream;
                if (file != null) return file.SafeFileHandle.IsClosed;
                System.IO.Pipes.PipeStream pipe = stream as System.IO.Pipes.PipeStream;
                if (pipe != null) return pipe.SafePipeHandle.IsClosed;
                MemoryStream memory = stream as MemoryStream;
                if (memory != null) return !memory.CanRead && !memory.CanWrite;
                return false;
            } catch (ObjectDisposedException) {
                // Some CLR stream implementations refuse even a handle query
                // after disposal. That specific state is also terminal.
                return true;
            }
        }
        public bool IsDisposed {
            get {
                return disposed && (nativeThread == null || nativeThread.IsClosed)
                    && (reader == null || StreamClosed(reader.BaseStream))
                    && (writer == null || StreamClosed(writer.BaseStream));
            }
        }
        // A completed Join publishes the worker's failure to polling callers.
        public Exception Failure { get { return thread.Join(0) ? failure : null; } }
        public bool Wait(int milliseconds) { return thread.Join(milliseconds); }
        public string Result() {
            if (!IsCompleted) throw new InvalidOperationException("Capture I/O is still running");
            if (failure != null) throw new IOException("Capture I/O failed", failure);
            if (Interlocked.CompareExchange(ref cancelled, 0, 0) != 0)
                throw new OperationCanceledException("Capture I/O was cancelled");
            return output;
        }
        public void RequestCancellation() {
            Interlocked.Exchange(ref cancelled, 1);
            SafeWaitHandle handle = Interlocked.CompareExchange(ref nativeThread, null, null);
            if (handle == null || IsCompleted) return;
            if (!CancelSynchronousIo(handle)) {
                int error = Marshal.GetLastWin32Error();
                // No request may exist between reads or before the first read.
                // The bounded owner loop repeats cancellation until Join proves exit.
                if (error != NoPendingRequest)
                    throw new Win32Exception(error, "Cancel owned capture I/O failed");
            }
        }
        public void Dispose() {
            if (disposed) return;
            if (thread.IsAlive) throw new InvalidOperationException("Cannot dispose live capture I/O");
            try {
                if (reader != null) reader.Dispose();
                if (writer != null) {
                    // An aborted writer closes its owned stream without asking
                    // StreamWriter to flush again after that stream is closed.
                    if (IsCancellationRequested || failure != null) writer.BaseStream.Dispose();
                    else writer.Dispose();
                }
            } finally {
                if (nativeThread != null) nativeThread.Dispose();
                disposed = true;
            }
        }
    }
}
'@
    $hash = [Security.Cryptography.SHA256]::Create()
    try { $digest = [BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($source))).Replace('-', '') }
    finally { $hash.Dispose() }
    # Exact source determines type identity. Re-importing an edited module must
    # not silently reuse a previously loaded native helper implementation.
    $name = 'Io_' + $digest
    $typeName = 'SakCapture.' + $name
    $type = $typeName -as [type]
    if ($null -eq $type) {
        Add-Type -TypeDefinition $source.Replace('__TYPE__', $name) -ErrorAction Stop -WarningAction Stop
        $type = $typeName -as [type]
    }
    if ($null -eq $type) { throw 'Capture I/O type did not load' }
    return $type
}

function Get-CaptureRemainingMilliseconds($Watch, [long]$Deadline) {
    if ($Deadline -lt 0) { return -1 }
    return [int][Math]::Max(0, $Deadline - $Watch.ElapsedMilliseconds)
}

function Assert-CaptureIoWorkers([object[]]$Workers) {
    foreach ($worker in $Workers) {
        if ($null -ne $worker -and $null -ne $worker.Failure) {
            throw [IO.IOException]::new('Capture I/O worker failed: ' + $worker.Failure.Message, $worker.Failure)
        }
    }
}

function Get-CaptureWaitSlice($Watch, [long]$Deadline) {
    $pollMilliseconds = 50
    $remaining = Get-CaptureRemainingMilliseconds $Watch $Deadline
    if ($remaining -lt 0) { return $pollMilliseconds }
    return [Math]::Min($remaining, $pollMilliseconds)
}

function Wait-CaptureIo($Worker, $Watch, [long]$Deadline, [string]$Stage, [object[]]$Workers = @()) {
    do {
        Assert-CaptureIoWorkers $Workers
        $slice = Get-CaptureWaitSlice $Watch $Deadline
        if ($slice -eq 0) { throw [TimeoutException]::new("Capture deadline expired during $Stage") }
        $completed = $Worker.Wait($slice)
        Assert-CaptureIoWorkers $Workers
    } while (-not $completed)
    return $Worker.Result()
}

function Wait-CaptureProcess($Process, $Watch, [long]$Deadline, [object[]]$Workers) {
    do {
        Assert-CaptureIoWorkers $Workers
        $slice = Get-CaptureWaitSlice $Watch $Deadline
        if ($slice -eq 0) { return $false }
        $completed = $Process.WaitForExit($slice)
        Assert-CaptureIoWorkers $Workers
    } while (-not $completed)
    return $true
}

function Close-CaptureIo([object[]]$Workers, $Watch, [long]$Deadline) {
    $failures = [Collections.Generic.List[string]]::new()
    $pending = @($Workers | Where-Object { $null -ne $_ -and -not $_.IsCompleted })
    while ($pending.Count -gt 0) {
        foreach ($worker in $pending) {
            try { $worker.RequestCancellation() }
            catch { $failures.Add($_.Exception.Message) }
        }
        $remaining = Get-CaptureRemainingMilliseconds $Watch $Deadline
        if ($remaining -eq 0) { break }
        [void]$pending[0].Wait([Math]::Min(10, $remaining))
        $pending = @($pending | Where-Object { -not $_.IsCompleted })
    }
    foreach ($worker in @($Workers | Where-Object { $null -ne $_ })) {
        if (-not $worker.Wait(0)) { $failures.Add('Owned capture I/O did not stop within cleanup budget'); continue }
        if ($null -ne $worker.Failure) { $failures.Add($worker.Failure.ToString()) }
        try { $worker.Dispose() } catch { $failures.Add($_.Exception.Message) }
    }
    if ($failures.Count -gt 0) { throw ('Capture I/O cleanup: ' + ($failures -join '; ')) }
}

function Stop-CapturedProcessTree([System.Diagnostics.Process]$Process, $BudgetWatch = $null, [long]$Deadline = -1) {
    if ($Process.HasExited) {
        throw "Cannot verify descendant termination: process $($Process.Id) exited before requested tree cleanup"
    }
    $taskkillPath = Join-Path ([System.Environment]::SystemDirectory) 'taskkill.exe'
    if (-not (Test-Path -LiteralPath $taskkillPath -PathType Leaf)) {
        throw "Cannot terminate timed-out process tree; taskkill.exe is missing: $taskkillPath"
    }
    $ioType = Get-CaptureIoType
    $termination = [Diagnostics.Process]::new()
    $termination.StartInfo = [Diagnostics.ProcessStartInfo]::new()
    $termination.StartInfo.FileName = $taskkillPath
    $termination.StartInfo.Arguments = "/PID $($Process.Id) /T /F"
    $termination.StartInfo.UseShellExecute = $false
    $termination.StartInfo.CreateNoWindow = $true
    $termination.StartInfo.RedirectStandardOutput = $true
    $termination.StartInfo.RedirectStandardError = $true
    $watch = if ($null -eq $BudgetWatch) { [Diagnostics.Stopwatch]::StartNew() } else { $BudgetWatch }
    $stopDeadline = if ($Deadline -lt 0) { $watch.ElapsedMilliseconds + 5000 } else { $Deadline }
    $workers = @()
    $failure = $null
    $terminationStarted = $false
    $cleanupErrors = [Collections.Generic.List[string]]::new()
    try {
        if (-not $termination.Start()) { throw 'Process-tree termination utility did not start' }
        $terminationStarted = $true
        $workers += $ioType::Read($termination.StandardOutput, $ioType::MaximumOutputCharacters, 'termination stdout')
        $workers += $ioType::Read($termination.StandardError, $ioType::MaximumOutputCharacters, 'termination stderr')
        if (-not (Wait-CaptureProcess $termination $watch $stopDeadline $workers)) {
            $termination.Kill()
            throw 'Process-tree termination utility exceeded cleanup budget'
        }
        $output = Wait-CaptureIo $workers[0] $watch $stopDeadline 'termination stdout' $workers
        $errorText = Wait-CaptureIo $workers[1] $watch $stopDeadline 'termination stderr' $workers
        if ($termination.ExitCode -ne 0) {
            throw "Failed to terminate timed-out process tree $($Process.Id) (taskkill exit $($termination.ExitCode)): $output $errorText"
        }
    } catch { $failure = $_ }
    finally {
        if ($null -ne $failure -and $terminationStarted -and -not $termination.HasExited) {
            try {
                $termination.Kill()
                if (-not $termination.WaitForExit((Get-CaptureRemainingMilliseconds $watch $stopDeadline))) {
                    throw 'Process-tree termination utility did not exit during cleanup'
                }
            } catch { $cleanupErrors.Add($_.Exception.Message) }
        }
        try { Close-CaptureIo $workers $watch $stopDeadline }
        catch { $cleanupErrors.Add($_.Exception.Message) }
        try { $termination.Dispose() } catch { $cleanupErrors.Add($_.Exception.Message) }
    }
    if ($null -ne $failure) {
        if ($cleanupErrors.Count -gt 0) { throw [InvalidOperationException]::new(($failure.Exception.Message + '; ' + ($cleanupErrors -join '; ')), $failure.Exception) }
        throw $failure
    }
    if ($cleanupErrors.Count -gt 0) { throw ($cleanupErrors -join '; ') }
}

function Set-CaptureEnvironmentOverrides([Diagnostics.ProcessStartInfo]$StartInfo, [AllowNull()][object]$Overrides) {
    if ($null -eq $Overrides) { return }
    $maximumEntries = 64
    $maximumNameCharacters = 256
    $maximumValueCharacters = 16384
    $maximumOverrideCharacters = 32768
    if ($Overrides -isnot [Collections.IDictionary]) { throw 'Capture environment overrides must be a dictionary' }
    if ($Overrides.Count -gt $maximumEntries) { throw 'Capture environment override count exceeds limit' }
    $entries = @($Overrides.GetEnumerator())
    $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $characters = 0
    foreach ($entry in $entries) {
        if ($entry.Key -isnot [string] -or $entry.Key.Length -eq 0 -or
            $entry.Key.Length -gt $maximumNameCharacters -or $entry.Key.Contains('=') -or
            $entry.Key.IndexOf([char]0) -ge 0 -or -not $names.Add($entry.Key)) {
            throw 'Capture environment override name is invalid or duplicated'
        }
        if ($null -ne $entry.Value -and ($entry.Value -isnot [string] -or
                $entry.Value.Length -gt $maximumValueCharacters -or $entry.Value.IndexOf([char]0) -ge 0)) {
            throw 'Capture environment override value is invalid or exceeds limit'
        }
        $characters += $entry.Key.Length + 2
        if ($null -ne $entry.Value) { $characters += $entry.Value.Length }
        if ($characters -gt $maximumOverrideCharacters) { throw 'Capture environment overrides exceed total character limit' }
    }
    # Validation precedes every update and launch. Never mutate the caller's process
    # environment: .NET Framework cannot faithfully restore present-empty variables.
    foreach ($entry in $entries) {
        if ($null -eq $entry.Value) { $StartInfo.EnvironmentVariables.Remove($entry.Key) }
        else { $StartInfo.EnvironmentVariables[$entry.Key] = $entry.Value }
    }
}

function Start-CaptureProcess($Process) {
    # .NET Framework (Windows PowerShell 5.1) builds StandardInput from Console.InputEncoding
    # and enables AutoFlush inside Start(), which writes that encoding's preamble at once.
    # Under a UTF-8 console (chcp 65001) the encoding carries a byte-order mark, so an empty
    # input reached the child as three BOM bytes. Start with the same code page minus its
    # preamble; assigning an equal code page leaves the console itself unchanged. Untyped
    # parameter: fixtures override Start() on the PSObject and must keep that override.
    $inputEncoding = [Console]::InputEncoding
    if ($inputEncoding.GetPreamble().Length -eq 0) { return $Process.Start() }
    if ($inputEncoding -isnot [Text.UTF8Encoding]) {
        throw "Console input encoding carries an unsupported preamble: $($inputEncoding.WebName)"
    }
    [Console]::InputEncoding = [Text.UTF8Encoding]::new($false)
    try { return $Process.Start() } finally { [Console]::InputEncoding = $inputEncoding }
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
        [ValidateRange(1, 16777216)]
        [int]$MaxOutputCharacters = 16777216,
        [int[]]$AllowedExitCodes = @(),
        [AllowNull()][string]$InputText = '',
        [switch]$HiddenWindow,
        [AllowNull()][object]$EnvironmentOverrides = $null
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = Join-ProcessArguments -Arguments $Arguments
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    if ($HiddenWindow) {
        $startInfo.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    }
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.RedirectStandardInput = $true
    if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory)) {
        $startInfo.WorkingDirectory = $WorkingDirectory
    }
    Set-CaptureEnvironmentOverrides $startInfo $EnvironmentOverrides

    $ioType = Get-CaptureIoType
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $deadline = if ($TimeoutSeconds -gt 0) { [long]$TimeoutSeconds * 1000 } else { -1L }
    $cleanupMilliseconds = 5000
    $workers = @()
    $failure = $null
    $cleanupErrors = [Collections.Generic.List[string]]::new()
    $terminationAttempted = $false
    $cleanupDeadline = $null
    $result = $null
    $started = $false
    try {
        if (-not (Start-CaptureProcess $process)) { throw "Process did not start: $FilePath" }
        $started = $true
        $watch.Restart()
        $stdoutWorker = $ioType::Read($process.StandardOutput, $MaxOutputCharacters, 'stdout')
        $workers += $stdoutWorker
        $stderrWorker = $ioType::Read($process.StandardError, $MaxOutputCharacters, 'stderr')
        $workers += $stderrWorker
        $inputWorker = $ioType::Write($process.StandardInput, $InputText)
        $workers += $inputWorker
        [void](Wait-CaptureIo $inputWorker $watch $deadline 'stdin write/flush' $workers)
        $timedOut = -not (Wait-CaptureProcess $process $watch $deadline $workers)

        if ($timedOut) {
            $deadline = $watch.ElapsedMilliseconds + $cleanupMilliseconds
            $cleanupDeadline = $deadline
            $terminationAttempted = $true
            Stop-CapturedProcessTree -Process $process -BudgetWatch $watch -Deadline $cleanupDeadline
            if (-not (Wait-CaptureProcess $process $watch $deadline $workers)) {
                throw "Timed-out process did not terminate: $FilePath (pid $($process.Id))"
            }
        }

        $stdout = Wait-CaptureIo $stdoutWorker $watch $deadline 'stdout completion' $workers
        $stderr = Wait-CaptureIo $stderrWorker $watch $deadline 'stderr completion' $workers
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
    } catch { $failure = $_ }
    finally {
        if ($null -eq $cleanupDeadline) { $cleanupDeadline = $watch.ElapsedMilliseconds + $cleanupMilliseconds }
        if ($null -ne $failure -and $started -and -not $terminationAttempted -and -not $process.HasExited) {
            try { Stop-CapturedProcessTree -Process $process -BudgetWatch $watch -Deadline $cleanupDeadline }
            catch { $cleanupErrors.Add($_.Exception.Message) }
        }
        try { Close-CaptureIo $workers $watch $cleanupDeadline }
        catch { $cleanupErrors.Add($_.Exception.Message) }
        try { $process.Dispose() } catch { $cleanupErrors.Add($_.Exception.Message) }
    }
    if ($null -ne $failure) {
        if ($cleanupErrors.Count -gt 0) {
            throw [InvalidOperationException]::new(($failure.Exception.Message + '; ' + ($cleanupErrors -join '; ')), $failure.Exception)
        }
        throw $failure
    }
    if ($cleanupErrors.Count -gt 0) { throw ($cleanupErrors -join '; ') }
    return $result
}

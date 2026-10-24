<#
.SYNOPSIS
    Self-tests scripts/process_capture.ps1 under the current PowerShell host.
#>

param(
    [ValidateSet("all", "file-management", "allocate-free-space", "bitlocker",
        "os-migration-vbox", "os-migration-screenshot", "linux-metadata", "vm-data",
        "file-recovery", "ext-filesystem-vm", "offline-bios-bcd")]
    [string]$MigrationCase = "all",
    [string]$CaptureModulePath = '',
    [ValidateSet('', 'no-read', 'hold-stdout', 'hold-stderr', 'hold-stdin',
        'parent-exit-before-stop', 'parent-exit-before-taskkill')][string]$DeadlineCase = '',
    [switch]$DeadlineWorker,
    [switch]$CoreOnly,
    [switch]$OwnershipOnly,
    [switch]$DeadlineOnly,
    [switch]$OutputOnly,
    [string]$OutputCase = '',
    [switch]$StdinOnly,
    [ValidateSet('', 'omitted', 'null', 'empty', 'text')][string]$StdinCase = '',
    [switch]$EnvironmentOnly,
    [switch]$EnvironmentWorker,
    [string]$FixtureRoot = '',
    [string]$PythonPath = ''
)

$ErrorActionPreference = "Stop"

if ($CaptureModulePath -eq '') { $CaptureModulePath = Join-Path $PSScriptRoot 'process_capture.ps1' }
$CaptureModulePath = (Resolve-Path -LiteralPath $CaptureModulePath -ErrorAction Stop).Path
$captureSourceHash = (Get-FileHash -LiteralPath $CaptureModulePath -Algorithm SHA256).Hash
. $CaptureModulePath

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

function Assert-CaptureFixtureBinding([Diagnostics.Process]$Process, $Binding, [string]$Label) {
    $validPid = ($Binding.pid -is [int] -or $Binding.pid -is [long]) -and
        $Binding.pid -gt 0 -and $Binding.pid -le [int]::MaxValue
    $validCreated = $Binding.created -is [string] -and $Binding.created -match '^[1-9][0-9]{0,18}$'
    Assert-True ($validPid -and $validCreated) "$Label has no valid PID/generation receipt"
    [void]$Process.Handle
    Assert-True ($Process.Id -eq $Binding.pid -and
        $Process.StartTime.ToUniversalTime().ToFileTimeUtc().ToString() -ceq $Binding.created) (
        "$Label PID/generation does not match the held process handle"
    )
}

function Get-CaptureFunctionScriptBlock([string]$Name, [string]$Path) {
    $tokens = $null
    $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$parseErrors)
    Assert-True ($parseErrors.Count -eq 0) "Capture source did not parse: $Path"
    $definitions = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $Name
    }, $true))
    Assert-True ($definitions.Count -eq 1) "Capture function missing or ambiguous: $Name"
    $definition = $definitions[0]
    $body = $definition.Body.Extent.Text
    $text = $body.Substring(1, $body.Length - 2)
    if ($definition.Parameters.Count -gt 0) {
        $parameters = $definition.Parameters | ForEach-Object { $_.Extent.Text }
        $text = 'param(' + ($parameters -join ', ') + ')' + [Environment]::NewLine + $text
    }
    return [scriptblock]::Create($text)
}

function New-DeadlineTerminationProcess {
    $termination = [Diagnostics.Process]::new()
    $termination | Add-Member -MemberType ScriptMethod -Name Start -Force -Value {
        Assert-True (-not $script:DeadlineParentProcess.HasExited) 'Parent exited before taskkill launch barrier'
        [IO.File]::WriteAllText((Join-Path $FixtureRoot 'parent-release.ack'), 'release bound parent')
        Assert-True ($script:DeadlineParentProcess.WaitForExit(2000)) 'Parent did not exit before actual taskkill launch'
        $script:DeadlineRaceState.parent_exited_before_taskkill = $true
        return $this.psbase.Start()
    }
    $termination | Add-Member -MemberType ScriptMethod -Name WaitForExit -Force -Value {
        param($Milliseconds)
        $exited = $this.psbase.WaitForExit([int]$Milliseconds)
        if ($exited) { $script:DeadlineRaceState.taskkill_exit = $this.ExitCode }
        return $exited
    }
    return $termination
}

function Set-DeadlineCleanupObservers {
    $script:DeadlineDisposeFailures = @()
    $closeText = (Get-CaptureFunctionScriptBlock 'Close-CaptureIo' $CaptureModulePath).ToString()
    $dispose = 'try { $worker.Dispose() } catch { $failures.Add($_.Exception.Message) }'
    Assert-True ([regex]::Matches($closeText, [regex]::Escape($dispose)).Count -eq 1) 'I/O disposal observation seam is not unique'
    $closeText = $closeText.Replace($dispose,
        'try { $worker.Dispose() } catch { $script:DeadlineDisposeFailures += $_.Exception.ToString(); $failures.Add($_.Exception.Message) }')
    Set-Item -Path Function:script:Close-CaptureIo -Value ([scriptblock]::Create($closeText))
    if (-not $DeadlineCase.StartsWith('parent-exit-')) { return }
    $stop = Get-CaptureFunctionScriptBlock 'Stop-CapturedProcessTree' $CaptureModulePath
    if ($DeadlineCase -eq 'parent-exit-before-taskkill') {
        $old = '$termination = [Diagnostics.Process]::new()'
        $stopText = $stop.ToString()
        Assert-True ([regex]::Matches($stopText, [regex]::Escape($old)).Count -eq 1) 'Taskkill Process observation seam is not unique'
        $stop = [scriptblock]::Create($stopText.Replace($old, '$termination = New-DeadlineTerminationProcess'))
    }
    $script:DeadlineCanonicalStop = $stop
    Set-Item -Path Function:script:Stop-CapturedProcessTree -Value {
        param([Diagnostics.Process]$Process, $BudgetWatch = $null, [long]$Deadline = -1)
        $script:DeadlineRaceState.stop_called = $true
        $script:DeadlineRaceState.parent_exited_at_stop = $Process.HasExited
        & $script:DeadlineCanonicalStop -Process $Process -BudgetWatch $BudgetWatch -Deadline $Deadline
    }
}

if ($DeadlineWorker) {
    if ($DeadlineCase -eq '' -or $FixtureRoot -eq '' -or $PythonPath -eq '') { throw 'Incomplete deadline worker arguments' }
    $captureText = (Get-Command Invoke-CapturedProcess -CommandType Function).Definition
    $script:DeadlineOperationWatch = $null
    $script:DeadlineDisposeFailures = @()
    $script:DeadlineRaceState = @{ admitted = $false; stop_called = $false; parent_exited_at_stop = $false;
        parent_exited_before_taskkill = $false; taskkill_exit = $null }
    function New-DeadlineBoundProcess {
        $process = [Diagnostics.Process]::new()
        $script:DeadlineParentProcess = $process
        $process | Add-Member -MemberType ScriptMethod -Name Start -Force -Value {
            $started = $this.psbase.Start()
            if (-not $started) { return $false }
            $setup = [Diagnostics.Stopwatch]::StartNew()
            while (-not (Test-Path -LiteralPath (Join-Path $FixtureRoot 'capture-ready.ack'))) {
                if ($setup.Elapsed.TotalSeconds -ge 30) { throw 'Deadline fixture identity setup expired' }
                [Threading.Thread]::Sleep(10)
            }
            # Independent clock for the real post-launch contract. Compilation
            # and this explicit identity-setup barrier are separate setup work.
            $script:DeadlineOperationWatch = [Diagnostics.Stopwatch]::StartNew()
            return $true
        }
        if ($DeadlineCase.StartsWith('parent-exit-')) {
            $process | Add-Member -MemberType ScriptMethod -Name WaitForExit -Force -Value {
                param($Milliseconds)
                $exited = $this.psbase.WaitForExit([int]$Milliseconds)
                if (-not $exited -and -not $script:DeadlineRaceState.admitted -and
                    $script:DeadlineOperationWatch.ElapsedMilliseconds -ge 1000) {
                    Assert-True (-not $this.HasExited) 'Parent exited before the real timeout was observed'
                    $script:DeadlineRaceState.admitted = $true
                    if ($DeadlineCase -eq 'parent-exit-before-stop') {
                        [IO.File]::WriteAllText((Join-Path $FixtureRoot 'parent-release.ack'), 'release after real timeout')
                        Assert-True ($this.psbase.WaitForExit(2000)) 'Parent did not exit before canonical tree cleanup'
                    }
                }
                return $exited
            }
        }
        return $process
    }
    $construction = '$process = [System.Diagnostics.Process]::new()'
    if ([regex]::Matches($captureText, [regex]::Escape($construction)).Count -ne 1) { throw 'Deadline process boundary is not unique' }
    # The real Start call is unchanged. Bind fixture identities before returning
    # from Start and therefore before either old/new post-launch timeout begins.
    $captureText = $captureText.Replace($construction, '$process = New-DeadlineBoundProcess')
    $observeAt = 'try { $process.Dispose() } catch'
    $matches = [regex]::Matches($captureText, [regex]::Escape($observeAt)).Count
    if ($matches -gt 1) { throw 'Ambiguous capture observation boundary' }
    $script:DeadlineObservedIo = @()
    if ($matches -eq 1) {
        # Observe I/O ownership before Process.Dispose can incidentally close an
        # undisposed stdin stream and hide a missing worker cleanup operation.
        $captureText = $captureText.Replace($observeAt, '$script:DeadlineObservedIo = @($workers | ForEach-Object { @{completed=$_.IsCompleted;disposed=$_.IsDisposed} }); ' + $observeAt)
    }
    # Old-module drills may lack this observation boundary. They cannot pass the
    # worker-lifetime assertion; their intended deadline assertion must fail first.
    $capture = [scriptblock]::Create($captureText)
    $options = @{
        FilePath=$PythonPath; Arguments=@((Join-Path $PSScriptRoot '../tests/support/process_capture_deadline_fixture.py'),$DeadlineCase,$FixtureRoot)
        WorkingDirectory=$FixtureRoot; TimeoutSeconds=1
    }
    if ($DeadlineCase -in @('no-read', 'hold-stdin')) { $options.InputText = 'i' * (8 * 1024 * 1024) }
    if ($DeadlineCase -eq 'hold-stdin' -or $DeadlineCase.StartsWith('parent-exit-')) { Set-DeadlineCleanupObservers }
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $failure = $null; $captured = $null
    try { $captured = & $capture @options } catch { $failure = $_.Exception.ToString() }
    $watch.Stop()
    $observation = [ordered]@{
        elapsed_seconds=$(if ($null -ne $script:DeadlineOperationWatch) { $script:DeadlineOperationWatch.Elapsed.TotalSeconds } else { $watch.Elapsed.TotalSeconds })
        total_seconds=$watch.Elapsed.TotalSeconds; failure=$failure; captured=$captured
        io=$script:DeadlineObservedIo
        dispose_failures=@($script:DeadlineDisposeFailures)
        race=$script:DeadlineRaceState
    }
    $observation | ConvertTo-Json -Depth 6
    if ((Get-FileHash -LiteralPath $CaptureModulePath -Algorithm SHA256).Hash -cne $captureSourceHash) { throw 'Capture module changed during worker proof' }
    exit 0
}

function Test-CaptureIoDeadline([string]$Mode, [string]$ShellPath) {
    $base = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    $root = Join-Path $base ('sak-capture-deadline-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $root -ErrorAction Stop | Out-Null
    $python = (Get-Command python.exe -CommandType Application -TotalCount 1 -ErrorAction Stop).Source
    $worker = [Diagnostics.Process]::new()
    $worker.StartInfo = [Diagnostics.ProcessStartInfo]::new()
    $worker.StartInfo.FileName = $ShellPath
    $worker.StartInfo.Arguments = Join-ProcessArguments @('-NoProfile','-NonInteractive','-File',$PSCommandPath,
        '-CaptureModulePath',$CaptureModulePath,'-DeadlineCase',$Mode,'-DeadlineWorker','-FixtureRoot',$root,'-PythonPath',$python)
    $worker.StartInfo.UseShellExecute = $false
    $worker.StartInfo.CreateNoWindow = $true
    $worker.StartInfo.RedirectStandardOutput = $true
    $worker.StartInfo.RedirectStandardError = $true
    $worker.StartInfo.EnvironmentVariables.Remove('PSModulePath')
    $held = @{}; $bindings = @{}; $primaryFailure = $null
    $cleanupErrors = [Collections.Generic.List[string]]::new()
    $watch = [Diagnostics.Stopwatch]::StartNew()
    try {
        Assert-True $worker.Start() 'Deadline worker did not start'
        $held.worker=$worker
        $bindings.worker=@{pid=$worker.Id;created=$worker.StartTime.ToUniversalTime().ToFileTimeUtc().ToString()}
        Assert-CaptureFixtureBinding $worker $bindings.worker 'Deadline worker'
        $output = $worker.StandardOutput.ReadToEndAsync(); $errorOutput = $worker.StandardError.ReadToEndAsync()
        while (-not $worker.HasExited) {
            foreach ($role in @('parent','child')) {
                $path=Join-Path $root ($role+'.json')
                if ($held.ContainsKey($role) -or -not (Test-Path -LiteralPath $path)) { continue }
                $binding=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json
                $process=[Diagnostics.Process]::GetProcessById($binding.pid)
                try { Assert-CaptureFixtureBinding $process $binding "Deadline $role" }
                catch { $process.Dispose(); throw }
                $held[$role]=$process; $bindings[$role]=$binding
            }
            if ($held.ContainsKey('parent') -and $held.ContainsKey('child') -and -not (Test-Path -LiteralPath (Join-Path $root 'bound.ack'))) {
                $ack=[IO.File]::Open((Join-Path $root 'bound.ack'),[IO.FileMode]::CreateNew);$ack.Dispose()
            }
            if ($held.ContainsKey('parent') -and ($Mode -eq 'no-read' -or $held.ContainsKey('child')) -and
                -not (Test-Path -LiteralPath (Join-Path $root 'capture-ready.ack'))) {
                $ack=[IO.File]::Open((Join-Path $root 'capture-ready.ack'),[IO.FileMode]::CreateNew);$ack.Dispose()
            }
            Assert-True ($watch.Elapsed.TotalSeconds -lt 30) 'Deadline fixture outer watchdog expired'
            [Threading.Thread]::Sleep(10)
        }
        Assert-True ($output.Wait(1000) -and $errorOutput.Wait(1000)) 'Deadline fixture output did not complete'
        Assert-Equal $worker.ExitCode 0 ('Deadline worker setup failed: ' + $errorOutput.GetAwaiter().GetResult())
        $result=$output.GetAwaiter().GetResult()|ConvertFrom-Json
        $result|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $root 'observation.json') -Encoding ascii
        $stage=@{'no-read'='stdin write/flush';'hold-stdin'='stdin write/flush';'hold-stdout'='stdout completion';'hold-stderr'='stderr completion'}[$Mode]
        $expected = if ($Mode -eq 'parent-exit-before-stop') { 'Cannot verify descendant termination' }
        elseif ($Mode -eq 'parent-exit-before-taskkill') { 'Failed to terminate timed-out process tree' }
        else { "Capture deadline expired during $stage" }
        Assert-True ($result.failure -is [string] -and $result.failure.Contains($expected)) "Missing intended $Mode deadline refusal: $($result.failure)"
        if ($Mode.StartsWith('parent-exit-')) {
            Assert-True ($result.race.admitted -and $result.race.stop_called -and $null -eq $result.captured) 'Tree-cleanup refusal did not follow an actual admitted timeout'
            Assert-True ($result.race.parent_exited_at_stop -eq ($Mode -eq 'parent-exit-before-stop')) 'Parent exit occurred at the wrong cleanup boundary'
            if ($Mode -eq 'parent-exit-before-taskkill') {
                $validExit = ($result.race.taskkill_exit -is [int] -or $result.race.taskkill_exit -is [long]) -and
                    $result.race.taskkill_exit -gt 0 -and $result.race.taskkill_exit -le [int]::MaxValue
                Assert-True ($result.race.parent_exited_before_taskkill -and $validExit) 'Actual taskkill did not fail after the admitted parent exit'
            }
        }
        if ($Mode -eq 'hold-stdin') {
            Assert-True ($result.dispose_failures.Count -eq 0) ('Cancelled native writer disposal failed: ' + ($result.dispose_failures -join '; '))
        }
        Assert-True ($result.io.Count -ge 2) 'Deadline worker exposed no I/O lifetime evidence'
        foreach ($io in $result.io) { Assert-True ($io.completed -is [bool] -and $io.completed -and $io.disposed -is [bool] -and $io.disposed) 'Capture I/O worker was not stopped and disposed' }
        Assert-True ($result.elapsed_seconds -lt 6) "Finite $Mode capture exceeded execution plus cleanup allowance"
        Assert-True $held.ContainsKey('parent') 'Deadline fixture parent was not bound'
        Assert-True $held.parent.HasExited 'Capture left the owned stalled parent running'
        if ($Mode -ne 'no-read') {
            Assert-True $held.ContainsKey('child') 'Deadline fixture child was not bound'
            Assert-True (-not $held.child.HasExited) 'Capture unexpectedly killed the independently redirected background child'
        }
        Write-Host "Deadline $Mode refused in $($result.elapsed_seconds) seconds; owned I/O stopped/disposed."
    } catch { $primaryFailure = $_ }
    finally {
        foreach ($role in @('child','parent','worker')) {
            if (-not $held.ContainsKey($role)) { continue }
            $process=$held[$role]
            try {
                Assert-CaptureFixtureBinding $process $bindings[$role] "Deadline cleanup $role"
                if (-not $process.HasExited) { $process.Kill() }
                Assert-True ($process.WaitForExit(2000)) "Deadline fixture $role did not stop"
            } catch { $cleanupErrors.Add($_.Exception.Message) }
            finally { $process.Dispose() }
        }
        if ($cleanupErrors.Count -eq 0 -and $null -eq $primaryFailure) {
            try {
                Assert-True ([IO.Path]::GetDirectoryName($root) -ceq $base -and [IO.Path]::GetFileName($root) -cmatch '^sak-capture-deadline-[0-9a-f]{32}$') 'Unsafe deadline fixture root'
                foreach ($file in Get-ChildItem -LiteralPath $root -Force) {
                    Assert-True (-not $file.PSIsContainer -and -not $file.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)) 'Unexpected deadline fixture entry'
                    [IO.File]::Delete($file.FullName)
                }
                [IO.Directory]::Delete($root,$false)
            } catch { $cleanupErrors.Add($_.Exception.Message) }
        }
    }
    if ($null -ne $primaryFailure -or $cleanupErrors.Count -gt 0) {
        $message=if($null -ne $primaryFailure){$primaryFailure.Exception.Message}else{'Deadline fixture cleanup failed'}
        throw "$message; cleanup_errors=$($cleanupErrors.Count); $($cleanupErrors -join '; '); fixture=$root"
    }
}

function Test-CancelledWriterOwnership {
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Threading;
public sealed class CaptureFixtureWriteBarrier : MemoryStream {
    public readonly ManualResetEventSlim Entered = new ManualResetEventSlim(false);
    public readonly ManualResetEventSlim ReleaseWrite = new ManualResetEventSlim(false);
    public override void Write(byte[] buffer, int offset, int count) {
        Entered.Set();
        if (!ReleaseWrite.Wait(5000)) throw new TimeoutException("Writer fixture barrier expired");
        base.Write(buffer, offset, count);
    }
    protected override void Dispose(bool disposing) {
        if (disposing) { Entered.Dispose(); ReleaseWrite.Dispose(); }
        base.Dispose(disposing);
    }
}
'@ -ErrorAction Stop -WarningAction Stop
    $stream = [CaptureFixtureWriteBarrier]::new()
    $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false), 1024)
    $writer.AutoFlush = $true
    $type = Get-CaptureIoType
    $io = $type::Write($writer, ('w' * 8192))
    try {
        Assert-True ($stream.Entered.Wait(5000)) 'Cancelled-writer fixture never entered its real write'
        $io.RequestCancellation()
        $stream.ReleaseWrite.Set()
        Assert-True ($io.Wait(5000)) 'Cooperatively cancelled writer did not stop'
        # This managed barrier deliberately avoids broken-pipe auto-close, which
        # otherwise masks a missing writer disposal in the native timeout case.
        Assert-True $stream.CanWrite 'Writer fixture closed before ownership was checked'
        $io.Dispose()
        Assert-True ($io.IsDisposed -and -not $stream.CanWrite) 'Cancelled input worker did not close its stream'
    } finally {
        if (-not $io.IsCompleted) {
            $stream.ReleaseWrite.Set()
            $io.RequestCancellation()
            Assert-True ($io.Wait(5000)) 'Writer fixture cleanup did not stop the worker'
        }
        try { $io.Dispose() } finally { $stream.Dispose() }
    }
    Write-Host 'Cancelled writer ownership verified.'
}

function Test-CaptureOutputCase($Case) {
    $base = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    $root = Join-Path $base ('sak-capture-output-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $root -ErrorAction Stop | Out-Null
    $python = (Get-Command python.exe -CommandType Application -TotalCount 1 -ErrorAction Stop).Source
    $state = @{ held=@{}; bindings=@{}; watch=$null; io=@(); stages=@(); utility_io=@() }
    # Bound only the fixture receipt handshake. Python startup and host scanning can exceed
    # two seconds before it writes parent.json; production capture deadlines are unchanged.
    $identitySetupLimitSeconds = 10
    $failure = $null; $captured = $null; $primaryFailure = $null
    $cleanupErrors = [Collections.Generic.List[string]]::new()
    function New-OutputBoundProcess {
        $process = [Diagnostics.Process]::new()
        $process | Add-Member -MemberType ScriptMethod -Name Start -Force -Value {
            if (-not $this.psbase.Start()) { return $false }
            $held = [Diagnostics.Process]::GetProcessById($this.Id)
            $state.held.parent=$held
            $state.bindings.parent=@{pid=$this.Id;created=$this.StartTime.ToUniversalTime().ToFileTimeUtc().ToString()}
            Assert-CaptureFixtureBinding $held $state.bindings.parent 'Output parent'
            $setup=[Diagnostics.Stopwatch]::StartNew()
            $roles=if($Case.mode -eq 'output-hold-stdout'){@('parent','child')}else{@('parent')}
            foreach($role in $roles) {
                $path=Join-Path $root ($role+'.json')
                while(-not (Test-Path -LiteralPath $path)) {
                    Assert-True ($setup.Elapsed.TotalSeconds -lt $identitySetupLimitSeconds) 'Output fixture identity setup expired'
                    [Threading.Thread]::Sleep(5)
                }
                $binding=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json
                if(-not $state.held.ContainsKey($role)) {
                    $state.held[$role]=[Diagnostics.Process]::GetProcessById($binding.pid)
                    $state.bindings[$role]=$binding
                }
                Assert-CaptureFixtureBinding $state.held[$role] $binding "Output $role"
            }
            [IO.File]::WriteAllText((Join-Path $root 'output-ready.ack'),'identities bound')
            $state.watch=[Diagnostics.Stopwatch]::StartNew()
            return $true
        }
        return $process
    }
    $canonicalIoWait = Get-CaptureFunctionScriptBlock 'Wait-CaptureIo' $CaptureModulePath
    function Wait-CaptureIo($Worker,$Watch,[long]$Deadline,[string]$Stage,[object[]]$Workers=@()) {
        $state.stages += $Stage
        if($Case.mode -eq 'output-hold-stdout' -and $Stage -eq 'stdout completion') {
            [IO.File]::WriteAllText((Join-Path $root 'output-drain.ack'),'observing output wait')
        }
        if($Case.mode -eq 'output-block-stdin' -and $Stage -eq 'stdin write/flush') {
            [IO.File]::WriteAllText((Join-Path $root 'output-release.ack'),'observing blocked input wait')
        }
        & $canonicalIoWait $Worker $Watch $Deadline $Stage $Workers
    }
    $canonicalProcessWait = Get-CaptureFunctionScriptBlock 'Wait-CaptureProcess' $CaptureModulePath
    function Wait-CaptureProcess($Process,$Watch,[long]$Deadline,[object[]]$Workers) {
        if($Process.Id -eq $state.held.parent.Id) {
            $state.stages += 'process exit'
            if($Case.mode -eq 'output-wait') {
                [IO.File]::WriteAllText((Join-Path $root 'output-release.ack'),'observing parent exit wait')
            }
        }
        & $canonicalProcessWait $Process $Watch $Deadline $Workers
    }
    if($Case.ContainsKey('utility')) {
        # Only the fixture utility's real ProcessStartInfo target changes. The
        # production utility launch, I/O worker creation and waits remain intact.
        function New-OutputBoundTerminationProcess {
            $process=[Diagnostics.Process]::new()
            $process | Add-Member -MemberType ScriptMethod -Name Start -Force -Value {
                $utilityRoot=Join-Path $root 'utility'
                New-Item -ItemType Directory -Path $utilityRoot -ErrorAction Stop | Out-Null
                $stdoutCount=if($Case.utility -eq 'stdout'){16777217}else{0}
                $stderrCount=if($Case.utility -eq 'stderr'){16777217}else{0}
                $this.StartInfo.FileName=$python
                $utilityArgs=@((Join-Path $PSScriptRoot '../tests/support/process_capture_deadline_fixture.py'),
                    'output-wait',$utilityRoot,[string]$stdoutCount,[string]$stderrCount)
                if($Case.ContainsKey('utility_receipt_delay_ms')){$utilityArgs += [string]$Case.utility_receipt_delay_ms}
                $this.StartInfo.Arguments=Join-ProcessArguments $utilityArgs
                if(-not $this.psbase.Start()){return $false}
                $held=[Diagnostics.Process]::GetProcessById($this.Id)
                $state.held.utility=$held
                $state.bindings.utility=@{pid=$this.Id;created=$this.StartTime.ToUniversalTime().ToFileTimeUtc().ToString()}
                Assert-CaptureFixtureBinding $held $state.bindings.utility 'Output termination utility'
                $setup=[Diagnostics.Stopwatch]::StartNew()
                $receipt=Join-Path $utilityRoot 'parent.json'
                while(-not (Test-Path -LiteralPath $receipt)) {
                    Assert-True ($setup.Elapsed.TotalSeconds -lt $identitySetupLimitSeconds) 'Output utility identity setup expired'
                    [Threading.Thread]::Sleep(5)
                }
                Assert-CaptureFixtureBinding $held (Get-Content -LiteralPath $receipt -Raw|ConvertFrom-Json) 'Output utility receipt'
                [IO.File]::WriteAllText((Join-Path $utilityRoot 'output-ready.ack'),'utility bound')
                [IO.File]::WriteAllText((Join-Path $utilityRoot 'output-release.ack'),'utility wait admitted')
                return $true
            }
            return $process
        }
        $stopText=(Get-CaptureFunctionScriptBlock 'Stop-CapturedProcessTree' $CaptureModulePath).ToString()
        $utilityConstruction='$termination = [Diagnostics.Process]::new()'
        $utilityObservation='try { $termination.Dispose() } catch'
        foreach($seam in @($utilityConstruction,$utilityObservation)) {
            Assert-Equal ([regex]::Matches($stopText,[regex]::Escape($seam)).Count) 1 'Output utility observation seam is not unique'
        }
        $stopText=$stopText.Replace($utilityConstruction,'$termination = New-OutputBoundTerminationProcess')
        $stopText=$stopText.Replace($utilityObservation,'$state.utility_io = @($workers | ForEach-Object { @{completed=$_.IsCompleted;disposed=$_.IsDisposed} }); '+$utilityObservation)
        $canonicalOutputStop=[scriptblock]::Create($stopText)
        function Stop-CapturedProcessTree($Process,$BudgetWatch=$null,[long]$Deadline=-1) {
            & $canonicalOutputStop -Process $Process -BudgetWatch $BudgetWatch -Deadline $Deadline
        }
    }
    $captureText = (Get-Command Invoke-CapturedProcess -CommandType Function).Definition
    $construction='$process = [System.Diagnostics.Process]::new()'
    $observation='try { $process.Dispose() } catch'
    foreach($seam in @($construction,$observation)) {
        Assert-Equal ([regex]::Matches($captureText,[regex]::Escape($seam)).Count) 1 'Output fixture observation seam is not unique'
    }
    $captureText=$captureText.Replace($construction,'$process = New-OutputBoundProcess')
    $captureText=$captureText.Replace($observation,'$state.io = @($workers | ForEach-Object { @{completed=$_.IsCompleted;disposed=$_.IsDisposed} }); '+$observation)
    $options=@{FilePath=$python;Arguments=@((Join-Path $PSScriptRoot '../tests/support/process_capture_deadline_fixture.py'),
        $Case.mode,$root,[string]$Case.stdout,[string]$Case.stderr);TimeoutSeconds=$Case.timeout}
    if($Case.ContainsKey('receipt_delay_ms')){$options.Arguments += [string]$Case.receipt_delay_ms}
    if($Case.ContainsKey('limit')){$options.MaxOutputCharacters=$Case.limit}
    if($Case.mode -eq 'output-block-stdin'){$options.InputText='I' * 131072}
    try {
        try { $captured=& ([scriptblock]::Create($captureText)) @options }
        catch { $failure=$_.Exception.ToString() }
        if($Case.ContainsKey('invalid')) {
            Assert-True ($failure -match "Cannot validate argument on parameter 'MaxOutputCharacters'") "Invalid output budget was not refused: $($Case.name)"
            Assert-Equal $state.held.Count 0 'Invalid output budget launched a process'
        } else {
            Assert-True ($null -ne $state.watch) "Output probe did not reach the native launch boundary: $failure; held=$($state.held.Count)"
            if($Case.ContainsKey('overflow')) {
                $limit=if($Case.ContainsKey('limit')){$Case.limit}else{16777216}
                $expected="Capture $($Case.overflow) exceeded output limit of $limit characters"
                Assert-True ($null -ne $failure -and $failure.Contains($expected)) "Missing output overflow refusal: $($Case.name); $failure"
                Assert-True ($null -eq $captured) 'Output overflow returned a successful or truncated result'
            } else {
                Assert-True ($null -eq $failure) "Valid output budget failed: $($Case.name); $failure"
                Assert-Equal $captured.exit_code 0 'Exact-boundary output child failed'
                Assert-True (-not $captured.timed_out) 'Exact-boundary output timed out'
                Assert-Equal $captured.stdout.Length $Case.stdout 'Stdout boundary was truncated'
                Assert-Equal $captured.stderr.Length $Case.stderr 'Stderr boundary was truncated'
                Assert-True ($captured.stdout -cmatch '^O*$' -and $captured.stderr -cmatch '^E*$') 'Output boundary content was corrupted'
            }
            if($Case.ContainsKey('stage')) {
                Assert-True ($state.stages -contains $Case.stage) "Output fixture did not enter $($Case.stage); observed=$($state.stages -join ', ')"
            }
            # Subtract only the explicit test-child startup delay, not any production capture time.
            $fixtureDelaySeconds=if($Case.ContainsKey('utility_receipt_delay_ms')){$Case.utility_receipt_delay_ms / 1000}else{0}
            Assert-True (($state.watch.Elapsed.TotalSeconds - $fixtureDelaySeconds) -lt 3) "Output worker failure was not propagated promptly: $($Case.name)"
            Assert-True ($state.io.Count -ge 2) 'Output probe exposed no owned I/O state'
            foreach($io in $state.io){Assert-True ($io.completed -and $io.disposed) 'Output worker did not stop and dispose'}
            if($Case.ContainsKey('utility')) {
                Assert-True ($state.held.utility.HasExited -and $state.utility_io.Count -eq 2) 'Output termination utility was not stopped'
                foreach($io in $state.utility_io){Assert-True ($io.completed -and $io.disposed) 'Termination output worker did not stop and dispose'}
                Assert-True (-not $state.held.parent.HasExited) 'Fake termination utility unexpectedly terminated its parent target'
            } else {
                Assert-True $state.held.parent.HasExited 'Output capture left its owned parent running'
            }
            if($Case.mode -eq 'output-hold-stdout') {
                Assert-True (-not $state.held.child.HasExited) 'Output refusal killed a normal-completion background child'
            }
        }
        Write-Host "Output $($Case.name) verified; elapsed_seconds=$($state.watch.Elapsed.TotalSeconds)."
    } catch { $primaryFailure=$_ }
    finally {
        foreach($role in @('child','utility','parent')) {
            if(-not $state.held.ContainsKey($role)){continue}
            $process=$state.held[$role]
            try {
                Assert-CaptureFixtureBinding $process $state.bindings[$role] "Output cleanup $role"
                if(-not $process.HasExited){$process.Kill()}
                Assert-True ($process.WaitForExit(2000)) 'Output fixture process did not stop'
            } catch {$cleanupErrors.Add($_.Exception.Message)}
            finally {$process.Dispose()}
        }
        if($null -eq $primaryFailure -and $cleanupErrors.Count -eq 0) {
            try {
                Assert-True ([IO.Path]::GetDirectoryName($root) -ceq $base -and [IO.Path]::GetFileName($root) -cmatch '^sak-capture-output-[0-9a-f]{32}$') 'Unsafe output fixture root'
                $cleanupRoots=@($root)
                if($Case.ContainsKey('utility')){$cleanupRoots=@((Join-Path $root 'utility'),$root)}
                foreach($cleanupRoot in $cleanupRoots) {
                Assert-True ($cleanupRoot -ceq $root -or [IO.Path]::GetDirectoryName($cleanupRoot) -ceq $root) 'Unsafe output utility fixture root'
                $directory=Get-Item -LiteralPath $cleanupRoot -Force -ErrorAction Stop
                Assert-True (-not $directory.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)) 'Unexpected output fixture directory'
                foreach($file in Get-ChildItem -LiteralPath $cleanupRoot -Force) {
                    Assert-True (-not $file.PSIsContainer -and -not $file.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)) 'Unexpected output fixture entry'
                    [IO.File]::Delete($file.FullName)
                }
                [IO.Directory]::Delete($cleanupRoot,$false)
                }
            } catch {$cleanupErrors.Add($_.Exception.Message)}
        }
    }
    if($null -ne $primaryFailure -or $cleanupErrors.Count -gt 0) {
        throw "Output case $($Case.name) failed: $($primaryFailure.Exception.Message); cleanup_errors=$($cleanupErrors.Count); $($cleanupErrors -join '; '); fixture=$root"
    }
}

function Test-CaptureOutputLimits([string]$SelectedCase) {
    $cases=@(
        @{name='stdout-exact';stdout=8192;stderr=0;limit=8192},
        @{name='stderr-exact';stdout=0;stderr=8192;limit=8192},
        @{name='both-exact';stdout=8192;stderr=8192;limit=8192},
        @{name='stdout-over';stdout=8193;stderr=0;limit=8192;overflow='stdout'},
        @{name='stderr-over';stdout=0;stderr=8193;limit=8192;overflow='stderr'},
        @{name='both-over';stdout=8193;stderr=8193;limit=8192;overflow='stdout'},
        @{name='minimum-exact';stdout=1;stderr=1;limit=1},
        @{name='delayed-receipt';stdout=1;stderr=1;limit=1;receipt_delay_ms=3000},
        @{name='default-exact';stdout=16777216;stderr=0},
        @{name='default-over';stdout=0;stderr=16777217;overflow='stderr'},
        @{name='zero-budget';stdout=0;stderr=0;limit=0;invalid=$true},
        @{name='negative-budget';stdout=0;stderr=0;limit=-1;invalid=$true},
        @{name='above-maximum-budget';stdout=0;stderr=0;limit=16777217;invalid=$true}
    )
    foreach($stream in @('stdout','stderr')) {
        $cases+=@{name="termination-$stream-over";mode='output-wait';stdout=0;stderr=0;overflow="termination $stream";utility=$stream;timeout=1}
    }
    $cases+=@{name='termination-delayed-stdout-over';mode='output-wait';stdout=0;stderr=0;overflow='termination stdout';utility='stdout';utility_receipt_delay_ms=3000;timeout=1}
    foreach($timeout in @(10,0)) {
        foreach($stream in @('stdout','stderr')) {
            foreach($mode in @('output-wait','output-block-stdin')) {
                $case=@{name="$mode-$stream-$timeout";mode=$mode;stdout=0;stderr=0;limit=8192;overflow=$stream;timeout=$timeout}
                $case[$stream]=8193
                $case.stage=if($mode -eq 'output-wait'){'process exit'}else{'stdin write/flush'}
                $cases+=$case
            }
        }
        $cases+=@{name="output-drain-$timeout";mode='output-hold-stdout';stdout=0;stderr=0;limit=8192;overflow='stderr';timeout=$timeout;stage='stdout completion'}
    }
    $selected=@($cases | Where-Object {$SelectedCase -eq '' -or $_.name -ceq $SelectedCase})
    Assert-True ($selected.Count -gt 0) "Unknown output fixture case: $SelectedCase"
    foreach($case in $selected) {
        if(-not $case.ContainsKey('mode')){$case.mode='output-exit'}
        if(-not $case.ContainsKey('timeout')){$case.timeout=10}
        Test-CaptureOutputCase $case
    }
    Write-Host "Output budget cases passed: $($selected.Count)."
}

function Test-CaptureInputEof([string]$SelectedCase) {
    $captureText=(Get-Command Invoke-CapturedProcess -CommandType Function).Definition
    $boundary='$ioType = Get-CaptureIoType'
    Assert-Equal ([regex]::Matches($captureText,[regex]::Escape($boundary)).Count) 1 'Stdin setup boundary is not unique'
    # Run the actual parameter binding and ProcessStartInfo construction, stopping
    # before helper compilation or process creation. This proves redirection even
    # if the test host's own input happens to be an already-closed pipe.
    $setupText=$captureText.Substring(0,$captureText.IndexOf($boundary,[StringComparison]::Ordinal))
    $setup=[scriptblock]::Create($setupText + [Environment]::NewLine +
        'return [pscustomobject]@{info=$startInfo;input=$InputText;is_null=($null -eq $InputText)}')
    $shell=[Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
    $childCommand=@'
$text=[Console]::In.ReadToEnd()
[Console]::Out.Write('redirected=' + [Console]::IsInputRedirected + ';length=' + $text.Length + ';payload=' + $text)
'@
    $cases=@(
        @{name='omitted';expected=''},
        @{name='null';value=$null;expected=''},
        @{name='empty';value='';expected=''},
        @{name='text';value='stdin payload with spaces';expected='stdin payload with spaces'}
    )
    $selected=@($cases | Where-Object {$SelectedCase -eq '' -or $_.name -ieq $SelectedCase})
    Assert-True ($selected.Count -gt 0) "No stdin cases selected: $SelectedCase"
    foreach($case in $selected) {
        $options=@{FilePath=$shell;Arguments=@('-NoProfile','-NonInteractive','-Command',$childCommand);TimeoutSeconds=5}
        if($case.ContainsKey('value')){$options.InputText=$case.value}
        $observed=& $setup @options
        Assert-True (-not $observed.is_null -and $observed.input -ceq $case.expected) "Stdin binding changed for $($case.name)"
        Assert-True $observed.info.RedirectStandardInput "Stdin redirection disabled for $($case.name)"
        $result=Invoke-CapturedProcess @options
        Assert-True (-not $result.timed_out -and $result.exit_code -eq 0) "Stdin EOF missing for $($case.name)"
        Assert-Equal $result.stderr '' "Stdin child reported an error for $($case.name)"
        $expected='redirected=True;length='+$case.expected.Length+';payload='+$case.expected
        Assert-Equal $result.stdout $expected "Stdin EOF/payload changed for $($case.name)"
        Write-Host "Stdin $($case.name) binding, native redirection and EOF/payload verified."
    }
}

function Get-CaptureFixtureEnvironment {
    $all=[Environment]::GetEnvironmentVariables('Process')
    $facts=[ordered]@{}
    foreach($name in @('SAK_CAPTURE_ENV_REMOVE','SAK_CAPTURE_ENV_EMPTY','SAK_CAPTURE_ENV_REPLACE',
            'SAK_CAPTURE_ENV_INHERIT','SAK_CAPTURE_ENV_INHERIT_EMPTY','SAK_CAPTURE_ENV_ABSENT','SAK_CAPTURE_ENV_NEW')) {
        $facts[$name]=[ordered]@{present=$all.Contains($name);value=$all[$name]}
    }
    return ConvertTo-Json -InputObject $facts -Depth 4 -Compress
}

function Test-CaptureEnvironmentValidation {
    $captureText=(Get-Command Invoke-CapturedProcess -CommandType Function).Definition
    $construction='$process = [System.Diagnostics.Process]::new()'
    Assert-Equal ([regex]::Matches($captureText,[regex]::Escape($construction)).Count) 1 'Environment launch boundary ambiguous'
    $script:EnvironmentLaunchAttempts=0
    $capture=[scriptblock]::Create($captureText.Replace($construction,
        '$script:EnvironmentLaunchAttempts++; throw "Environment validation reached process creation"'))
    $duplicate=[Collections.Specialized.OrderedDictionary]::new()
    $duplicate.Add('duplicate','one');$duplicate.Add('DUPLICATE','two')
    $tooMany=@{};foreach($index in 1..65){$tooMany['KEY'+$index]=''}
    $secret='synthetic-private-value'
    $invalid=@(
        @{name='container';value=$secret;diagnostic='must be a dictionary'},
        @{name='empty-name';value=@{''=$secret};diagnostic='name is invalid'},
        @{name='name-type';value=@{7=$secret};diagnostic='name is invalid'},
        @{name='equals';value=@{'bad=name'=$secret};diagnostic='name is invalid'},
        @{name='name-nul';value=@{('bad'+[char]0+'name')=$secret};diagnostic='name is invalid'},
        @{name='name-bound';value=@{('N'*257)=$secret};diagnostic='name is invalid'},
        @{name='duplicate';value=$duplicate;diagnostic='name is invalid'},
        @{name='value-type';value=@{KEY=@($secret)};diagnostic='value is invalid'},
        @{name='value-nul';value=@{KEY=($secret+[char]0)};diagnostic='value is invalid'},
        @{name='value-bound';value=@{KEY=($secret+('x'*16385))};diagnostic='value is invalid'},
        @{name='count';value=$tooMany;diagnostic='count exceeds'},
        @{name='total';value=@{A=('x'*16384);B=('y'*16379)};diagnostic='total character limit'}
    )
    foreach($case in $invalid) {
        $failure=''
        try {& $capture -FilePath 'environment-validation-no-launch.exe' -EnvironmentOverrides $case.value|Out-Null}
        catch {$failure=$_.Exception.Message}
        Assert-True ($failure.Contains($case.diagnostic)) "Environment invalid $($case.name) was not refused"
        Assert-True (-not $failure.Contains($secret)) 'Environment refusal exposed an override value'
    }
    Assert-Equal $script:EnvironmentLaunchAttempts 0 'Invalid environment reached process creation'
    $valid64=@{};foreach($index in 1..64){$valid64['KEY'+$index]=''}
    foreach($valid in @($valid64,@{('N'*256)='v'},@{A=('x'*16384);B=('y'*16378)})) {
        $info=[Diagnostics.ProcessStartInfo]::new();$info.EnvironmentVariables.Clear()
        Set-CaptureEnvironmentOverrides $info $valid
        Assert-Equal $info.EnvironmentVariables.Count $valid.Count 'Environment exact bound was rejected or truncated'
        foreach($entry in $valid.GetEnumerator()){Assert-Equal $info.EnvironmentVariables[$entry.Key] $entry.Value 'Environment exact-bound value changed'}
    }
    Write-Host 'Environment validation: 12 prelaunch refusals and exact bounds passed; values not disclosed.'
}

function Test-CaptureEnvironmentWorker {
    $before=Get-CaptureFixtureEnvironment
    $expected=$before|ConvertFrom-Json
    Assert-True ($expected.SAK_CAPTURE_ENV_INHERIT_EMPTY.present -and $expected.SAK_CAPTURE_ENV_INHERIT_EMPTY.value -ceq '') 'Environment worker has no genuine present-empty precondition'
    $definition=Get-CaptureFunctionScriptBlock 'Get-CaptureFixtureEnvironment' $PSCommandPath
    $childCommand='function Get-CaptureFixtureEnvironment {'+$definition.ToString()+'}; Get-CaptureFixtureEnvironment; exit '
    $shell=[Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
    $overrides=[ordered]@{SAK_CAPTURE_ENV_REMOVE=$null;SAK_CAPTURE_ENV_EMPTY='';SAK_CAPTURE_ENV_REPLACE='replacement';
        SAK_CAPTURE_ENV_NEW='created';SAK_CAPTURE_ENV_ABSENT=$null}
    foreach($mode in @('omitted','null','empty-map','overrides','nonzero','allowed-error','launch-error')) {
        $options=@{FilePath=$shell;Arguments=@('-NoProfile','-NonInteractive','-Command',($childCommand+'0'));TimeoutSeconds=10}
        if($mode -eq 'null'){$options.EnvironmentOverrides=$null}
        elseif($mode -eq 'empty-map'){$options.EnvironmentOverrides=@{}}
        elseif($mode -ne 'omitted'){$options.EnvironmentOverrides=$overrides}
        if($mode -in @('nonzero','allowed-error')){$options.Arguments[-1]=$childCommand+'7'}
        if($mode -eq 'allowed-error'){$options.AllowedExitCodes=@(0)}
        if($mode -eq 'launch-error'){$options.FilePath=Join-Path $PSScriptRoot 'environment-fixture-must-not-exist.exe';Assert-True (-not(Test-Path $options.FilePath)) 'Missing launch fixture exists'}
        $result=$null;$failure=$null
        try {$result=Invoke-CapturedProcess @options}catch{$failure=$_.Exception}
        # Compare presence as well as value BEFORE child assertions. A mutation
        # writing parent state must fail this specific isolation contract first.
        Assert-Equal (Get-CaptureFixtureEnvironment) $before "Capture mutated parent environment during $mode"
        if($mode -eq 'allowed-error') {Assert-True ($null -ne $failure -and $failure.Message.Contains('Command failed (7)')) 'Environment disallowed-exit failure lost';continue}
        if($mode -eq 'launch-error') {Assert-True ($null -ne $failure) 'Environment launch failure was accepted';continue}
        Assert-True ($null -eq $failure -and -not $result.timed_out) "Environment $mode capture failed: $failure"
        Assert-Equal $result.exit_code $(if($mode -eq 'nonzero'){7}else{0}) 'Environment child exit changed'
        $actual=$result.stdout|ConvertFrom-Json
        if($mode -in @('omitted','null','empty-map')){Assert-Equal $result.stdout $before 'Omitted environment overrides changed inherited values';continue}
        Assert-True ($actual.SAK_CAPTURE_ENV_REPLACE.present -and $actual.SAK_CAPTURE_ENV_REPLACE.value -ceq 'replacement' -and
            $actual.SAK_CAPTURE_ENV_NEW.present -and $actual.SAK_CAPTURE_ENV_NEW.value -ceq 'created') 'Child environment forwarding failed'
        Assert-True (-not $actual.SAK_CAPTURE_ENV_REMOVE.present -and $null -eq $actual.SAK_CAPTURE_ENV_REMOVE.value) 'Child environment null removal failed'
        Assert-True ($actual.SAK_CAPTURE_ENV_EMPTY.present -and $actual.SAK_CAPTURE_ENV_EMPTY.value -ceq '') 'Child environment empty preservation failed'
        Assert-True ($actual.SAK_CAPTURE_ENV_INHERIT.present -and $actual.SAK_CAPTURE_ENV_INHERIT.value -ceq 'inherit-original' -and
            $actual.SAK_CAPTURE_ENV_INHERIT_EMPTY.present -and $actual.SAK_CAPTURE_ENV_INHERIT_EMPTY.value -ceq '' -and
            -not $actual.SAK_CAPTURE_ENV_ABSENT.present) 'Child environment unspecified inheritance changed'
    }
    Test-CaptureEnvironmentValidation
    Assert-Equal (Get-CaptureFixtureEnvironment) $before 'Environment validation changed parent state'
    Write-Host 'Environment worker: seven native inheritance/override/failure paths passed; parent presence/value unchanged.'
}

function Test-CaptureEnvironment {
    $shell=[Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
    $worker=[Diagnostics.Process]::new()
    $worker.StartInfo=[Diagnostics.ProcessStartInfo]::new()
    $worker.StartInfo.FileName=$shell
    $worker.StartInfo.Arguments=Join-ProcessArguments @('-NoProfile','-NonInteractive','-File',$PSCommandPath,
        '-EnvironmentWorker','-CaptureModulePath',$CaptureModulePath)
    $worker.StartInfo.UseShellExecute=$false;$worker.StartInfo.CreateNoWindow=$true
    $worker.StartInfo.RedirectStandardOutput=$true;$worker.StartInfo.RedirectStandardError=$true
    # This independent fixture setup is not the override implementation under
    # test. A failed parent-isolation drill can affect only this owned child.
    $worker.StartInfo.EnvironmentVariables.Remove('PSModulePath')
    foreach($pair in @(@('REMOVE','remove-original'),@('EMPTY','empty-original'),@('REPLACE','replace-original'),@('INHERIT','inherit-original'),@('INHERIT_EMPTY',''))){
        $worker.StartInfo.EnvironmentVariables['SAK_CAPTURE_ENV_'+$pair[0]]=$pair[1]
    }
    foreach($name in @('SAK_CAPTURE_ENV_ABSENT','SAK_CAPTURE_ENV_NEW')){$worker.StartInfo.EnvironmentVariables.Remove($name)}
    $before=Get-CaptureFixtureEnvironment
    $started=$false
    try {
        $started=$worker.Start();Assert-True $started 'Environment worker did not start'
        $output=$worker.StandardOutput.ReadToEndAsync();$errorOutput=$worker.StandardError.ReadToEndAsync()
        Assert-True ($worker.WaitForExit(30000)) 'Environment worker exceeded bounded fixture time'
        Assert-True ($output.Wait(1000) -and $errorOutput.Wait(1000)) 'Environment worker pipes did not complete'
        Assert-Equal $worker.ExitCode 0 ('Environment worker failed: '+$errorOutput.GetAwaiter().GetResult()+$output.GetAwaiter().GetResult())
        Write-Host $output.GetAwaiter().GetResult()
    } finally {
        if($started -and -not $worker.HasExited){$worker.Kill();Assert-True ($worker.WaitForExit(2000)) 'Environment worker cleanup failed'}
        $worker.Dispose()
    }
    Assert-Equal (Get-CaptureFixtureEnvironment) $before 'Environment fixture changed caller state'
}

if($EnvironmentWorker){Test-CaptureEnvironmentWorker;exit 0}
if($EnvironmentOnly){Test-CaptureEnvironment;Assert-Equal ((Get-FileHash $CaptureModulePath).Hash) $captureSourceHash 'Capture changed during environment proof';exit 0}

if($StdinOnly -or $StdinCase -ne '') {
    Test-CaptureInputEof $StdinCase
    Assert-Equal ((Get-FileHash -LiteralPath $CaptureModulePath -Algorithm SHA256).Hash) $captureSourceHash 'Capture changed during stdin proof'
    exit 0
}

if($OutputOnly -or $OutputCase -ne '') {
    Test-CaptureOutputLimits $OutputCase
    Assert-Equal ((Get-FileHash -LiteralPath $CaptureModulePath -Algorithm SHA256).Hash) $captureSourceHash 'Capture changed during output proof'
    exit 0
}

if ($OwnershipOnly) {
    Test-CancelledWriterOwnership
    if ((Get-FileHash -LiteralPath $CaptureModulePath -Algorithm SHA256).Hash -cne $captureSourceHash) { throw 'Capture module changed during ownership proof' }
    exit 0
}

if ($DeadlineCase -ne '') {
    Test-CaptureIoDeadline $DeadlineCase ([Diagnostics.Process]::GetCurrentProcess().MainModule.FileName)
    if ((Get-FileHash -LiteralPath $CaptureModulePath -Algorithm SHA256).Hash -cne $captureSourceHash) { throw 'Capture module changed during deadline proof' }
    exit 0
}

if ($DeadlineOnly) {
    $shellPath = [Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
    foreach ($mode in @('no-read','hold-stdout','hold-stderr','hold-stdin',
            'parent-exit-before-stop','parent-exit-before-taskkill')) {
        Test-CaptureIoDeadline $mode $shellPath
    }
    if ((Get-FileHash -LiteralPath $CaptureModulePath -Algorithm SHA256).Hash -cne $captureSourceHash) { throw 'Capture module changed during deadline proof' }
    exit 0
}

function Invoke-ReadinessBoundCaptureTree([string]$ShellPath) {
    $temporaryBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    $fixtureRoot = Join-Path $temporaryBase ('sak_capture_tree_' + [guid]::NewGuid().ToString('N'))
    Assert-True ($fixtureRoot.StartsWith($temporaryBase + '\', [StringComparison]::OrdinalIgnoreCase)) (
        'Capture-tree fixture escaped its temporary parent'
    )
    New-Item -ItemType Directory -Path $fixtureRoot -ErrorAction Stop | Out-Null
    $readyPath = Join-Path $fixtureRoot 'ready.json'
    $state = @{ parent = $null; child = $null; ready = $false; ready_watch = $null; receipt = $null; capture_watch = $null }
    $setupLimitSeconds = 30
    $captureLimitSeconds = 1
    $terminationGraceSeconds = 5
    # Independent cleanup uses on-disk source even when CaptureModulePath names a mutant.
    $canonicalStop = Get-CaptureFunctionScriptBlock 'Stop-CapturedProcessTree' (Join-Path $PSScriptRoot 'process_capture.ps1')

    function New-ReadinessBoundFixtureProcess {
        $process = [Diagnostics.Process]::new()
        $process | Add-Member -MemberType ScriptMethod -Name Start -Force -Value {
            $started = $this.psbase.Start()
            if ($started) {
                $state.parent = [Diagnostics.Process]::GetProcessById($this.Id)
                Assert-CaptureFixtureBinding $state.parent @{
                    pid = $this.Id; created = $this.StartTime.ToUniversalTime().ToFileTimeUtc().ToString()
                } 'Launched parent'
            }
            return $started
        }
        $process | Add-Member -MemberType ScriptMethod -Name WaitForExit -Force -Value {
            param($Milliseconds)
            if ($null -eq $Milliseconds) { $this.psbase.WaitForExit(); return }
            if (-not $state.ready) {
                # The private fixture's readiness barrier is not part of the
                # production I/O budget. Pause only while this barrier runs.
                $state.capture_watch.Stop()
                try {
                $setupWatch = [Diagnostics.Stopwatch]::StartNew()
                while (-not (Test-Path -LiteralPath $readyPath -PathType Leaf)) {
                    Assert-True (-not $this.HasExited) 'Capture fixture parent exited before its readiness receipt'
                    Assert-True ($setupWatch.Elapsed.TotalSeconds -lt $setupLimitSeconds) 'Capture fixture readiness exceeded 30 seconds'
                    [Threading.Thread]::Sleep(10)
                }
                $receipt = Get-Content -LiteralPath $readyPath -Raw -ErrorAction Stop | ConvertFrom-Json
                Assert-CaptureFixtureBinding $state.parent $receipt.parent 'Readiness parent'
                Assert-True (($receipt.child.pid -is [int] -or $receipt.child.pid -is [long]) -and
                    $receipt.child.pid -gt 0 -and $receipt.child.pid -le [int]::MaxValue) 'Readiness child has no positive PID'
                $state.child = [Diagnostics.Process]::GetProcessById($receipt.child.pid)
                Assert-CaptureFixtureBinding $state.child $receipt.child 'Readiness child'
                Assert-True (-not $state.child.HasExited) 'Readiness child was not running before timeout'
                $state.receipt = $receipt
                $state.ready = $true
                $state.ready_watch = [Diagnostics.Stopwatch]::StartNew()
                } finally { $state.capture_watch.Start() }
            }
            $exited = $this.psbase.WaitForExit([int]$Milliseconds)
            if ($exited -and $Milliseconds -eq ($terminationGraceSeconds * 1000)) {
                # Check the bound child before pipe draining can wait for a leaked
                # descendant's natural exit. Consume only the remaining original
                # execution/termination budget, never an additional grace period.
                $remaining = [Math]::Max(0, [Math]::Floor(
                    (($captureLimitSeconds + $terminationGraceSeconds) * 1000) -
                    $state.ready_watch.Elapsed.TotalMilliseconds))
                Assert-True ($state.child.WaitForExit([int]$remaining)) 'Owned descendant survived capture termination'
            }
            return $exited
        }
        return $process
    }

    $treeCommand = @'
Start-Sleep -Milliseconds 1500
$child = Start-Process -FilePath '__SHELL__' -ArgumentList @(
    '-NoProfile', '-NonInteractive', '-Command', 'Start-Sleep -Seconds 30'
) -PassThru -WindowStyle Hidden -RedirectStandardOutput '__OUT__' -RedirectStandardError '__ERR__'
$self = [Diagnostics.Process]::GetCurrentProcess()
$receipt = [ordered]@{
    parent = @{ pid = $PID; created = $self.StartTime.ToUniversalTime().ToFileTimeUtc().ToString() }
    child = @{ pid = $child.Id; created = $child.StartTime.ToUniversalTime().ToFileTimeUtc().ToString() }
}
$text = $receipt | ConvertTo-Json -Depth 4 -Compress
[IO.File]::WriteAllText('__READY__.tmp', $text)
[IO.File]::Move('__READY__.tmp', '__READY__')
Write-Output $text
Start-Sleep -Seconds 30
'@
    foreach ($replacement in @{
        '__SHELL__' = $ShellPath; '__OUT__' = (Join-Path $fixtureRoot 'child.out');
        '__ERR__' = (Join-Path $fixtureRoot 'child.err'); '__READY__' = $readyPath
    }.GetEnumerator()) { $treeCommand = $treeCommand.Replace($replacement.Key, $replacement.Value.Replace("'", "''")) }

    # Execute the exact current capture body. Only its Process construction boundary
    # supplies an instrumented REAL process: Start/Wait call the original CLR methods.
    # Bounded setup readiness is excluded from the unchanged one-second execution
    # timeout. No production timeout or process/capture implementation is replaced.
    $captureBody = (Get-Command Invoke-CapturedProcess -CommandType Function).Definition
    $construction = '$process = [System.Diagnostics.Process]::new()'
    Assert-True ([regex]::Matches($captureBody, [regex]::Escape($construction)).Count -eq 1) (
        'Capture process-construction boundary is not unique'
    )
    $watchConstruction = '$watch = [Diagnostics.Stopwatch]::StartNew()'
    Assert-True ([regex]::Matches($captureBody, [regex]::Escape($watchConstruction)).Count -eq 1) 'Capture clock boundary is not unique'
    $captureText = $captureBody.Replace($construction, '$process = New-ReadinessBoundFixtureProcess')
    $captureText = $captureText.Replace($watchConstruction, $watchConstruction + '; $state.capture_watch = $watch')
    $capture = [scriptblock]::Create($captureText)
    $primaryFailure = $null
    $cleanupErrors = [Collections.Generic.List[string]]::new()
    try {
        $timed = & $capture -FilePath $ShellPath -Arguments @('-NoProfile', '-NonInteractive', '-Command', $treeCommand) -TimeoutSeconds $captureLimitSeconds
        Assert-True $timed.timed_out 'Process-tree timeout was not reported'
        Assert-True $state.ready 'Process-tree timeout ran without bound readiness evidence'
        $output = $timed.stdout | ConvertFrom-Json
        Assert-CaptureFixtureBinding $state.parent $output.parent 'Captured parent'
        Assert-CaptureFixtureBinding $state.child $output.child 'Captured child'
        Assert-True ($state.child.WaitForExit($terminationGraceSeconds * 1000)) 'Owned descendant survived capture termination'
        Assert-True ($state.ready_watch.Elapsed.TotalSeconds -le ($captureLimitSeconds + $terminationGraceSeconds)) (
            'Owned descendant termination exceeded the unchanged execution/termination bounds'
        )
    } catch { $primaryFailure = $_ }
    finally {
        # Independent fixture cleanup uses only handles bound BEFORE the timeout,
        # including when a negative drill deliberately disables descendant killing.
        # The canonical on-disk stop body also cleans setup failures before the child
        # receipt was bound; an in-memory guard mutant cannot weaken this cleanup.
        if ($null -ne $state.parent -and -not $state.parent.HasExited) {
            try { & $canonicalStop -Process $state.parent }
            catch { $cleanupErrors.Add($_.Exception.Message) }
        }
        foreach ($process in @($state.child, $state.parent)) {
            if ($null -eq $process) { continue }
            try {
                if (-not $process.HasExited) { $process.Kill() }
                Assert-True ($process.WaitForExit($terminationGraceSeconds * 1000)) 'Owned fixture process did not terminate during cleanup'
            } catch { $cleanupErrors.Add($_.Exception.Message) }
            finally { $process.Dispose() }
        }
        if ($state.ready -and $cleanupErrors.Count -eq 0) {
            try {
                $resolvedFixture = [IO.Path]::GetFullPath($fixtureRoot)
                Assert-True ($resolvedFixture.StartsWith($temporaryBase + '\', [StringComparison]::OrdinalIgnoreCase)) 'Fixture cleanup escaped temporary parent'
                $directory = Get-Item -LiteralPath $resolvedFixture -Force -ErrorAction Stop
                Assert-True (-not ($directory.Attributes -band [IO.FileAttributes]::ReparsePoint)) 'Fixture cleanup refused a reparse directory'
                foreach ($file in @(Get-ChildItem -LiteralPath $resolvedFixture -File -Force -ErrorAction Stop)) {
                    Assert-True (-not ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) 'Fixture cleanup refused a reparse file'
                    [IO.File]::Delete($file.FullName)
                }
                [IO.Directory]::Delete($resolvedFixture, $false)
            } catch { $cleanupErrors.Add($_.Exception.Message) }
        }
    }
    if ($null -ne $primaryFailure -or $cleanupErrors.Count -gt 0) {
        $messages = @($cleanupErrors)
        if ($null -ne $primaryFailure) { $messages = @($primaryFailure.Exception.Message) + $messages }
        throw ('Capture-tree fixture failed: ' + ($messages -join '; ') +
            "; cleanup_errors=$($cleanupErrors.Count); fixture=$fixtureRoot")
    }
    Write-Host ('Process-tree capture verified; parent=' + $state.receipt.parent.pid + '/' +
        $state.receipt.parent.created + '; child=' + $state.receipt.child.pid + '/' +
        $state.receipt.child.created + '; execution_limit_seconds=' + $captureLimitSeconds +
        '; termination_grace_seconds=' + $terminationGraceSeconds + '; cleanup=complete')
}

function Assert-CaptureMigration {
    param(
        [string]$Name,
        [string]$RelativePath,
        [string[]]$RequiredPatterns,
        [string[]]$LegacyPatterns
    )

    $path = Join-Path (Split-Path -Parent $PSScriptRoot) $RelativePath
    $text = Get-Content -LiteralPath $path -Raw -ErrorAction Stop
    foreach ($pattern in $RequiredPatterns) {
        Assert-True $text.Contains($pattern) "$Name does not use shared capture contract: $pattern"
    }
    foreach ($pattern in $LegacyPatterns) {
        Assert-True (-not $text.Contains($pattern)) "$Name retained legacy capture: $pattern"
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

$stdinCaptured = Invoke-CapturedProcess -FilePath $shell -Arguments @(
    "-NoProfile", "-NonInteractive", "-Command", "[Console]::In.ReadToEnd()"
) -InputText "stdin marker" -AllowedExitCodes @(0)
Assert-Equal $stdinCaptured.stdout "stdin marker" "Standard input was not delivered"

$emptyInput = Invoke-CapturedProcess -FilePath $shell -Arguments @('-NoProfile','-NonInteractive','-Command',
    "'input-length=' + [Console]::In.ReadToEnd().Length") -InputText '' -TimeoutSeconds 10
Assert-Equal $emptyInput.stdout 'input-length=0' 'Empty input did not close stdin'
$boundaryInput = ('a' * 4095) + [char]0xd83d + [char]0xde00 + 'tail'
# Exercise the actual chunk writer with an explicitly configured UTF-8 encoder;
# changing only a child console's decoder would assume the parent's OEM encoding.
$memory = [IO.MemoryStream]::new()
$utf8 = [Text.UTF8Encoding]::new($false, $true)
$writer = [IO.StreamWriter]::new($memory, $utf8, 1024)
$writer.AutoFlush = $true
$ioType = Get-CaptureIoType
$boundaryWorker = $ioType::Write($writer, $boundaryInput)
try {
    Assert-True ($boundaryWorker.Wait(5000)) 'Chunk writer did not finish'
    [void]$boundaryWorker.Result()
    Assert-Equal ([Convert]::ToBase64String($memory.ToArray())) ([Convert]::ToBase64String($utf8.GetBytes($boundaryInput))) 'Input chunk boundary corrupted a Unicode surrogate pair'
} finally { $boundaryWorker.Dispose(); $memory.Dispose() }
$missingProgram = Join-Path $PSScriptRoot 'capture-fixture-must-not-exist.exe'
Assert-True (-not (Test-Path -LiteralPath $missingProgram)) 'Missing-launch fixture path unexpectedly exists'
$missingError = $null
try { Invoke-CapturedProcess -FilePath $missingProgram -TimeoutSeconds 1 | Out-Null }
catch { $missingError = $_.Exception }
while ($null -ne $missingError -and $missingError -isnot [ComponentModel.Win32Exception]) { $missingError = $missingError.InnerException }
Assert-True ($null -ne $missingError -and $missingError.NativeErrorCode -eq 2) 'Launch failure lost its actual Win32 cause'

$pipePayloadSize = 131072
$pipeInput = 'i' * $pipePayloadSize
$duplexCaptured = Invoke-CapturedProcess -FilePath $shell -Arguments @(
    "-NoProfile", "-NonInteractive", "-Command",
    "[Console]::Out.Write(('o' * $pipePayloadSize)); " +
        "[Console]::Out.Flush(); " +
        "[Console]::Out.Write([Console]::In.ReadToEnd())"
) -InputText $pipeInput -TimeoutSeconds 10 -AllowedExitCodes @(0)
Assert-True (-not $duplexCaptured.timed_out) "Duplex output/stdin child deadlocked"
Assert-Equal $duplexCaptured.stdout.Length ($pipePayloadSize * 2) (
    "Duplex output/stdin payload was truncated"
)

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

Invoke-ReadinessBoundCaptureTree -ShellPath $shell
if (-not $CoreOnly) {
    Test-CancelledWriterOwnership
    foreach ($mode in @('no-read','hold-stdout','hold-stderr','hold-stdin',
            'parent-exit-before-stop','parent-exit-before-taskkill')) { Test-CaptureIoDeadline $mode $shell }
    Test-CaptureOutputLimits ''
    Test-CaptureInputEof ''
    Test-CaptureEnvironment
}

$migrationCases = @(
    @{
        name = "file-management"
        path = "scripts/run_file_management_live_filesystem_certification.ps1"
        required = @(
            '. (Join-Path $PSScriptRoot "process_capture.ps1")',
            'Invoke-CapturedProcess -FilePath $certifier -Arguments $certifierArgs',
            '$certifierProcess.exit_code',
            '$certifierProcess.stdout',
            '$certifierProcess.stderr'
        )
        legacy = @(
            '$certifierOutput = & $certifier @certifierArgs 2>&1',
            '$ErrorActionPreference = "Continue"',
            'SilentlyContinue'
        )
    },
    @{
        name = "allocate-free-space"
        path = "scripts/run_partition_manager_allocate_free_space_external_gate.ps1"
        required = @(
            '. (Join-Path $PSScriptRoot "process_capture.ps1")',
            'Invoke-CapturedProcess -FilePath $FilePath -Arguments $Arguments',
            '$captured.exit_code',
            '$captured.stdout',
            '$captured.stderr'
        )
        legacy = @(
            '$output = & $FilePath @Arguments 2>&1',
            '$global:LASTEXITCODE = $null',
            '$ErrorActionPreference = "Continue"',
            'SilentlyContinue'
        )
    },
    @{
        name = "bitlocker"
        path = "scripts/run_partition_manager_bitlocker_mutation_external_gate.ps1"
        required = @(
            '. (Join-Path $PSScriptRoot "process_capture.ps1")',
            '$captured = Invoke-CapturedProcess -FilePath $FilePath -Arguments $Arguments',
            '$raw = Invoke-NativeCommandRaw',
            'ConvertTo-SanitizedCommandLog -Command $raw'
        )
        legacy = @(
            '$output = & $FilePath @Arguments 2>&1',
            '$ErrorActionPreference = "Continue"',
            'SilentlyContinue'
        )
    },
    @{
        name = "os-migration-vbox"
        path = "scripts/run_partition_manager_os_migration_external_gate.ps1"
        required = @(
            '. (Join-Path $PSScriptRoot "process_capture.ps1")',
            '$captured = Invoke-CapturedProcess -FilePath $VBoxManage -Arguments $Arguments',
            '$captured.exit_code',
            '$captured.stdout',
            '$captured.stderr'
        )
        legacy = @(
            '$output = & $VBoxManage @Arguments 2>&1',
            '$ErrorActionPreference = "Continue"',
            'SilentlyContinue'
        )
    },
    @{
        name = "os-migration-screenshot"
        path = "scripts/run_partition_manager_os_migration_external_gate.ps1"
        required = @(
            '$screenshotProcess = Invoke-CapturedProcess -FilePath $VBoxManage -Arguments @(',
            '$screenshotProcess.exit_code',
            '$screenshotProcess.stdout',
            '$screenshotProcess.stderr'
        )
        legacy = @(
            '$screenshotOutput = & $VBoxManage controlvm',
            '$ErrorActionPreference = "Continue"',
            'SilentlyContinue'
        )
    },
    @{
        name = "linux-metadata"
        path = "scripts/run_partition_manager_linux_metadata_validation.ps1"
        required = @(
            '. (Join-Path $PSScriptRoot "process_capture.ps1")',
            '$captured = Invoke-CapturedProcess -FilePath $FilePath -Arguments $Arguments',
            '$captured.exit_code',
            '$captured.stdout',
            '$captured.stderr'
        )
        legacy = @(
            '$output = & $FilePath @Arguments 2>&1',
            '$ErrorActionPreference = "Continue"',
            '$LASTEXITCODE',
            'SilentlyContinue'
        )
    },
    @{
        name = "vm-data"
        path = "scripts/run_partition_manager_vm_data_external_gates.ps1"
        required = @(
            '. (Join-Path $PSScriptRoot "process_capture.ps1")',
            '$captured = Invoke-CapturedProcess -FilePath $FilePath -Arguments $Arguments',
            '$captured.exit_code',
            '$captured.stdout',
            '$captured.stderr'
        )
        legacy = @(
            '$output = & $FilePath @Arguments 2>&1',
            '$ErrorActionPreference = "Continue"',
            '$LASTEXITCODE',
            'SilentlyContinue'
        )
    },
    @{
        name = "file-recovery"
        path = "scripts/run_partition_manager_file_recovery_external_gate.ps1"
        required = @(
            '. (Join-Path $PSScriptRoot "process_capture.ps1")',
            '$captured = Invoke-CapturedProcess -FilePath $FilePath -Arguments $Arguments',
            '$captured.exit_code',
            '$captured.stdout',
            '$captured.stderr',
            '$FsutilExe = Join-Path ([System.Environment]::SystemDirectory) "fsutil.exe"',
            '-FilePath $FsutilExe',
            'Failed to disable NTFS DisableDeleteNotify with modern fsutil syntax',
            'Failed to restore NTFS DisableDeleteNotify with modern fsutil syntax'
        )
        legacy = @(
            '$output = & $FilePath @Arguments 2>&1',
            '$ErrorActionPreference = "Continue"',
            '$LASTEXITCODE',
            'SilentlyContinue',
            '$disableDeleteNotifyLegacy',
            '$restoreDeleteNotifyLegacy',
            '@("behavior", "set", "DisableDeleteNotify", "1")',
            '@("behavior", "set", "DisableDeleteNotify", "0")',
            "Do not trust either set's exit code"
        )
    },
    @{
        name = "ext-filesystem-vm"
        path = "scripts/run_partition_manager_ext_filesystem_vm_gate.ps1"
        required = @(
            '. (Join-Path $PSScriptRoot "process_capture.ps1")',
            '$captured = Invoke-CapturedProcess -FilePath $FilePath -Arguments $Arguments',
            '$captured.exit_code',
            '$captured.stdout',
            '$captured.stderr'
        )
        legacy = @(
            '$output = & $FilePath @Arguments 2>&1',
            '$ErrorActionPreference = "Continue"',
            '$LASTEXITCODE',
            'SilentlyContinue'
        )
    },
    @{
        name = "offline-bios-bcd"
        path = "scripts/run_partition_manager_offline_bios_bcd_break.ps1"
        required = @(
            '. (Join-Path $PSScriptRoot "process_capture.ps1")',
            '$captured = Invoke-CapturedProcess -FilePath $FilePath -Arguments $Arguments',
            '$captured.exit_code',
            '$captured.stdout',
            '$captured.stderr'
        )
        legacy = @(
            '$output = & $FilePath @Arguments 2>&1',
            '$ErrorActionPreference = "Continue"',
            '$LASTEXITCODE',
            'SilentlyContinue'
        )
    }
)
foreach ($case in $migrationCases) {
    if ($MigrationCase -eq "all" -or $MigrationCase -eq $case.name) {
        Assert-CaptureMigration -Name $case.name -RelativePath $case.path `
            -RequiredPatterns $case.required -LegacyPatterns $case.legacy
    }
}
if ($MigrationCase -eq "all" -or $MigrationCase -eq "bitlocker") {
    Assert-CaptureMigration -Name "bitlocker-launcher" `
        -RelativePath "scripts/launch_partition_manager_bitlocker_mutation_external_gate_local.ps1" `
        -RequiredPatterns @(
            'run_partition_manager_bitlocker_mutation_external_gate.ps1',
            'partition_manager_storage.ps1',
            'process_capture.ps1'
        ) -LegacyPatterns @()
}
if ($MigrationCase -eq "all" -or $MigrationCase -eq "vm-data") {
    Assert-CaptureMigration -Name "vm-data-launcher" `
        -RelativePath "scripts/launch_partition_manager_vm_data_external_gates_local.ps1" `
        -RequiredPatterns @(
            'run_partition_manager_vm_data_external_gates.ps1',
            'process_capture.ps1'
        ) -LegacyPatterns @()
}
if ($MigrationCase -eq "all" -or $MigrationCase -eq "file-recovery") {
    Assert-CaptureMigration -Name "file-recovery-launcher" `
        -RelativePath "scripts/launch_partition_manager_file_recovery_external_gate_local.ps1" `
        -RequiredPatterns @(
            'run_partition_manager_file_recovery_external_gate.ps1',
            'partition_manager_storage.ps1',
            'process_capture.ps1',
            '$stagedStorageHash = Copy-VerifiedStagedFile',
            '$stagedCaptureHash = Copy-VerifiedStagedFile'
        ) -LegacyPatterns @()
}
if ($MigrationCase -eq "all" -or $MigrationCase -eq "ext-filesystem-vm") {
    Assert-CaptureMigration -Name "ext-filesystem-vm-launcher" `
        -RelativePath "scripts/launch_partition_manager_ext_filesystem_vm_gate_local.ps1" `
        -RequiredPatterns @(
            'run_partition_manager_ext_filesystem_vm_gate.ps1',
            'partition_manager_storage.ps1',
            'process_capture.ps1'
        ) -LegacyPatterns @()
}
if ($MigrationCase -eq "all" -or $MigrationCase -eq "offline-bios-bcd") {
    Assert-CaptureMigration -Name "offline-bios-bcd-launcher" `
        -RelativePath "scripts/launch_partition_manager_offline_bios_bcd_break_local.ps1" `
        -RequiredPatterns @(
            'run_partition_manager_offline_bios_bcd_break.ps1',
            'partition_manager_storage.ps1',
            'process_capture.ps1'
        ) -LegacyPatterns @()
}

if ((Get-FileHash -LiteralPath $CaptureModulePath -Algorithm SHA256).Hash -cne $captureSourceHash) { throw 'Capture module changed during full proof' }
Write-Host "Process capture tests passed under $($PSVersionTable.PSVersion); migration=$MigrationCase; source=$captureSourceHash."

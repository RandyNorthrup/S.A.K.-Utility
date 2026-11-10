<#
.SYNOPSIS
    Runs the packaged Windows application through its bounded startup smoke path.
.DESCRIPTION
    Native Windows is pinned locally. Explicit CI runs construct the window without
    showing it and are reported separately. Success requires the captured process,
    its stdout marker and one newly created ordinary application log to agree.
    Only newly created runtime paths are cleaned, after complete path preflight.
#>
[CmdletBinding()]
param(
    [ValidateNotNullOrEmpty()][string]$PackageRoot = "build\Release",
    [ValidateRange(1, 3600)][int]$TimeoutSeconds = 30
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'process_capture.ps1')
# Reuse only the existing ancestor guard. No owner/ACL-changing operation is invoked.
. (Join-Path $PSScriptRoot 'owner_only_staging_file.ps1')

function Get-PortableSmokeItem {
    param([string]$Path, [ValidateSet('File', 'Directory', 'Any')][string]$Kind = 'Any')
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if ($null -eq $item) { throw "Portable smoke metadata unavailable: $Path" }
    if (($Kind -eq 'File' -and $item.PSIsContainer) -or
        ($Kind -eq 'Directory' -and -not $item.PSIsContainer)) {
        throw "Portable smoke expected $Kind at $Path"
    }
    Assert-DirectStagingPath -Path $Path
    return $item
}

function Assert-PortableSmokeChild {
    param([string]$Path, [string]$Root)
    $full = [IO.Path]::GetFullPath($Path)
    $prefix = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Portable smoke path escaped its package: $Path"
    }
}

function Get-PortableSmokeTree {
    param([string]$Path, [string]$Root)
    $pending = [Collections.Generic.Stack[string]]::new()
    $items = [Collections.Generic.List[object]]::new()
    $pending.Push($Path)
    $maximumItems = 4096
    while ($pending.Count -gt 0) {
        $current = $pending.Pop()
        Assert-PortableSmokeChild $current $Root
        $item = Get-PortableSmokeItem $current
        $items.Add($item)
        if ($items.Count -gt $maximumItems) { throw 'Portable smoke runtime tree exceeds its item bound' }
        if ($item.PSIsContainer) {
            foreach ($child in @(Get-ChildItem -LiteralPath $current -Force -ErrorAction Stop)) {
                Assert-PortableSmokeChild $child.FullName $Root
                $pending.Push($child.FullName)
            }
        }
    }
    return $items.ToArray()
}

function Get-PortableSmokeLogs {
    param([string]$Root)
    $directory = Join-Path $Root 'data\logs'
    if (-not (Test-Path -LiteralPath $directory -ErrorAction Stop)) { return @() }
    [void](Get-PortableSmokeItem $directory 'Directory')
    return @(Get-ChildItem -LiteralPath $directory -Filter 'sak_*.log' -File -Force -ErrorAction Stop)
}

function Get-NewPortableSmokeLog {
    param([string]$Root, [Collections.Generic.HashSet[string]]$PreviousNames)
    $newLogs = @(Get-PortableSmokeLogs $Root | Where-Object { -not $PreviousNames.Contains($_.Name) })
    if ($newLogs.Count -ne 1) {
        throw "Portable smoke requires exactly one uniquely new application log; found $($newLogs.Count)"
    }
    $path = $newLogs[0].FullName
    Assert-PortableSmokeChild $path $Root
    [void](Get-PortableSmokeItem $path 'File')
    return Get-Content -LiteralPath $path -Raw -ErrorAction Stop
}

function Assert-PortableSmokeProcess {
    param([object]$Result)
    if ($Result.exit_code -isnot [int] -or $Result.exit_code -ne 0) {
        throw "Portable smoke process did not report integer exit zero: $($Result.exit_code)"
    }
    if ($Result.stdout -isnot [string] -or
        [regex]::Matches($Result.stdout, '(?m)^SAK_STARTUP_SMOKE_OK\r?$').Count -ne 1) {
        throw 'Portable smoke requires one exact SAK_STARTUP_SMOKE_OK stdout marker'
    }
    if ($Result.stderr -isnot [string]) {
        throw 'Portable smoke requires startup phase diagnostics on standard error'
    }
    $phases = @([regex]::Matches($Result.stderr,
            '(?m)^SAK_STARTUP_PHASE tick_ms=(\d+) phase=([a-z-]+)\r?$'))
    $expected = @('before-qt-app', 'qt-app-ready', 'app-initialized')
    if ($phases.Count -ne $expected.Count) {
        throw 'Portable smoke requires the three exact startup phase diagnostics'
    }
    [uint64]$previousTick = 0
    for ($index = 0; $index -lt $expected.Count; $index++) {
        if ($phases[$index].Groups[2].Value -cne $expected[$index]) {
            throw 'Portable smoke startup phase diagnostics are out of order'
        }
        $tick = [uint64]::Parse($phases[$index].Groups[1].Value,
            [Globalization.CultureInfo]::InvariantCulture)
        if ($tick -lt $previousTick) {
            throw 'Portable smoke startup phase timestamps moved backwards'
        }
        $previousTick = $tick
    }
}

function Assert-PortableSmokeLog {
    param([string]$Log, [bool]$CiMode)
    if ($Log -notmatch '(?m)Application shutting down with exit code: 0\s*$') {
        throw 'Portable smoke new log did not record clean shutdown'
    }
    if ([regex]::Matches($Log, '(?m)MainWindow startup: all tool panels materialized\s*$').Count -ne 1) {
        throw 'Portable smoke did not prove every lazy tool panel was constructed'
    }
    $headless = $Log.Contains('Startup smoke CI headless mode active; main window constructed but not shown')
    $displayed = $Log.Contains('Main window displayed - application ready')
    $initialized = $Log.Contains('Main window initialized - application ready')
    if (($CiMode -and (-not $headless -or -not $initialized -or $displayed)) -or
        (-not $CiMode -and ($headless -or $initialized -or -not $displayed))) {
        throw 'Portable smoke log does not match the explicit native/CI startup mode'
    }
}

function Remove-PortableSmokeRuntime {
    param([string]$Root, [hashtable]$Preexisting)
    $plan = [Collections.Generic.List[object]]::new()
    # Validate the COMPLETE new tree before the first deletion. A late unsafe node
    # must not cause partial cleanup of earlier runtime directories.
    foreach ($relative in ($Preexisting.Keys | Sort-Object)) {
        $path = Join-Path $Root $relative
        if ($Preexisting[$relative] -or -not (Test-Path -LiteralPath $path -ErrorAction Stop)) { continue }
        [void](Get-PortableSmokeItem $path 'Directory')
        if ($relative -eq 'data\crashes' -and
            @(Get-ChildItem -LiteralPath $path -Force -ErrorAction Stop).Count -gt 0) {
            throw 'Portable smoke found newly created crash evidence; all runtime evidence is retained'
        }
        foreach ($item in @(Get-PortableSmokeTree $path $Root)) { $plan.Add($item) }
    }
    $errors = [Collections.Generic.List[string]]::new()
    foreach ($item in ($plan | Sort-Object { $_.FullName.Length } -Descending)) {
        try {
            $kind = if ($item.PSIsContainer) { 'Directory' } else { 'File' }
            [void](Get-PortableSmokeItem $item.FullName $kind)
            # Nonrecursive CLR deletion cannot prompt or traverse newly inserted
            # children. A nonempty/raced directory fails; no recursive fallback.
            if ($item.PSIsContainer) { [IO.Directory]::Delete($item.FullName, $false) }
            else { [IO.File]::Delete($item.FullName) }
            if (Test-Path -LiteralPath $item.FullName -ErrorAction Stop) {
                throw "Cleanup left the path present: $($item.FullName)"
            }
        } catch { $errors.Add($_.Exception.Message) }
    }
    if ($errors.Count -gt 0) { throw ('Portable smoke runtime cleanup failed: ' + ($errors -join '; ')) }
}

function Get-PortableSmokeDiagnostics {
    param([string]$Root)
    $messages = [Collections.Generic.List[string]]::new()
    try {
        $logs = @(Get-PortableSmokeLogs $Root | Sort-Object LastWriteTime -Descending | Select-Object -First 3)
        foreach ($log in $logs) {
            try {
                Assert-PortableSmokeChild $log.FullName $Root
                [void](Get-PortableSmokeItem $log.FullName 'File')
                $text = Get-Content -LiteralPath $log.FullName -Tail 120 -ErrorAction Stop
                $messages.Add("$($log.Name): " + ($text -join [Environment]::NewLine))
            } catch { $messages.Add('Log diagnostic failed: ' + $_.Exception.Message) }
        }
    } catch { $messages.Add('Log enumeration diagnostic failed: ' + $_.Exception.Message) }
    return $messages.ToArray()
}

function Invoke-PortableStartupSmoke {
    param([string]$Path, [int]$LimitSeconds)
    $resolved = @(Resolve-Path -LiteralPath $Path -ErrorAction Stop)
    if ($resolved.Count -ne 1 -or $resolved[0].Provider.Name -ne 'FileSystem') {
        throw 'Portable smoke requires exactly one filesystem package directory'
    }
    $rootPath = $resolved[0].Path
    if ($rootPath.Length -gt 3) { $rootPath = $rootPath.TrimEnd('\') }
    [void](Get-PortableSmokeItem $rootPath 'Directory')
    $exe = Join-Path $rootPath 'sak_utility.exe'
    [void](Get-PortableSmokeItem $exe 'File')
    $preexisting = @{}
    foreach ($relative in @('data\ai_sessions', 'data\temp', 'data\logs', 'data\config', 'data\crashes', '_logs')) {
        $runtime = Join-Path $rootPath $relative
        Assert-DirectStagingPath $runtime
        $preexisting[$relative] = Test-Path -LiteralPath $runtime -ErrorAction Stop
        if ($preexisting[$relative]) {
            [void](Get-PortableSmokeItem $runtime 'Directory')
            [void]@(Get-PortableSmokeTree $runtime $rootPath)
        }
    }
    $previousNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($log in @(Get-PortableSmokeLogs $rootPath)) { [void]$previousNames.Add($log.Name) }
    $ciMode = [Environment]::GetEnvironmentVariable('GITHUB_ACTIONS', 'Process') -eq 'true' -or
        [Environment]::GetEnvironmentVariable('CI', 'Process') -eq 'true'
    $environment = [ordered]@{
        SAK_STARTUP_SMOKE_HEADLESS = $(if ($ciMode) { '1' } else { $null })
        SAK_STARTUP_SMOKE_CI_HEADLESS = $null
        QT_QPA_PLATFORM = 'windows'
        QT_PLUGIN_PATH = $null
        QT_QPA_PLATFORM_PLUGIN_PATH = $null
    }
    $failures = [Collections.Generic.List[string]]::new()
    $result = $null
    try {
        $launch = @{
            FilePath = $exe; Arguments = @('--smoke-test', '--no-splash')
            WorkingDirectory = $rootPath; TimeoutSeconds = $LimitSeconds; HiddenWindow = $true
            EnvironmentOverrides = $environment
        }
        $result = Invoke-CapturedProcess @launch
        if ($result.timed_out -isnot [bool]) {
            throw 'Portable smoke process did not report a boolean timeout result'
        }
        if ($result.timed_out) {
            throw "Portable smoke timed out (limit $LimitSeconds seconds)"
        }
        Assert-PortableSmokeProcess $result
        $logText = Get-NewPortableSmokeLog $rootPath $previousNames
        Assert-PortableSmokeLog $logText $ciMode
        Remove-PortableSmokeRuntime $rootPath $preexisting
    } catch {
        $failures.Add($_.Exception.Message)
        foreach ($diagnostic in @(Get-PortableSmokeDiagnostics $rootPath)) { $failures.Add($diagnostic) }
        if ($null -ne $result) {
            $failures.Add("STDOUT: $($result.stdout)")
            $failures.Add("STDERR: $($result.stderr)")
        }
    }
    if ($failures.Count -gt 0) { throw ($failures -join [Environment]::NewLine) }
    $mode = if ($ciMode) { 'CI constructed-window' } else { 'native Windows' }
    Write-Host "Portable startup E2E smoke passed ($mode): $rootPath"
}

Invoke-PortableStartupSmoke -Path $PackageRoot -LimitSeconds $TimeoutSeconds

# Proves what a test under run_ctest_no_dialogs.ps1 inherits, by running a test that reports it.
param()

$ErrorActionPreference = 'Stop'

$wrapper = Join-Path $PSScriptRoot 'run_ctest_no_dialogs.ps1'
$work = Join-Path ([IO.Path]::GetTempPath()) ("sak-no-dialogs-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null

# CI sets this for the whole suite. The wrapper's job is a local run, and refusing it is checked below.
$env:SAK_TEST_ALLOW_REAL_HARDWARE = $null

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "FAILED: $Message" }
}

# Runs the wrapper in a child shell, as a person would, and returns its exit code and output.
function Invoke-Wrapper([string]$TestDirectory) {
    $shell = (Get-Process -Id $PID).Path
    $output = & $shell -NoProfile -ExecutionPolicy Bypass -File $wrapper --test-dir $TestDirectory -V 2>&1
    return [pscustomobject]@{ Code = $LASTEXITCODE; Text = ($output | ForEach-Object { "$_" }) -join "`n" }
}

try {
    # The test asks Windows for its own error mode and prints it with the platform it was given.
    $reporter = Join-Path $work 'report.ps1'
    Set-Content -LiteralPath $reporter -Encoding ascii -Value @'
Add-Type -Namespace Probe -Name Mode -MemberDefinition '[DllImport("kernel32.dll")] public static extern uint GetErrorMode();'
"errormode=0x{0:X4} qpa={1}" -f [Probe.Mode]::GetErrorMode(), $env:QT_QPA_PLATFORM
'@
    $shellPath = ((Get-Process -Id $PID).Path -replace '\\', '/')
    $reporterPath = ($reporter -replace '\\', '/')

    $reports = Join-Path $work 'reports'
    New-Item -ItemType Directory -Path $reports | Out-Null
    Set-Content -LiteralPath (Join-Path $reports 'CTestTestfile.cmake') -Encoding ascii -Value `
        "add_test(report `"$shellPath`" -NoProfile -ExecutionPolicy Bypass -File `"$reporterPath`")"

    $failing = Join-Path $work 'failing'
    New-Item -ItemType Directory -Path $failing | Out-Null
    Set-Content -LiteralPath (Join-Path $failing 'CTestTestfile.cmake') -Encoding ascii -Value `
        "add_test(fails `"$shellPath`" -NoProfile -Command `"exit 3`")"

    # Windows' two flags that stop a missing DLL's box and a crash's box.
    $inherited = Invoke-Wrapper $reports
    Assert-True ($inherited.Code -eq 0) "a passing test should exit 0, got $($inherited.Code)"
    $match = [regex]::Match($inherited.Text, 'errormode=0x([0-9A-F]{4})')
    Assert-True $match.Success "the test reported no error mode: $($inherited.Text)"
    $mode = [Convert]::ToUInt32($match.Groups[1].Value, 16)
    Assert-True (($mode -band 0x3) -eq 0x3) "the test inherited error mode 0x$($match.Groups[1].Value), without both flags"

    Assert-True ($inherited.Text -match 'qpa=offscreen') 'QT_QPA_PLATFORM=offscreen did not reach the test'

    $failed = Invoke-Wrapper $failing
    Assert-True ($failed.Code -ne 0) 'a failing test should fail the run'

    # A local run must not stand the hardware guard down.
    $env:SAK_TEST_ALLOW_REAL_HARDWARE = '1'
    $refused = Invoke-Wrapper $reports
    $env:SAK_TEST_ALLOW_REAL_HARDWARE = $null
    Assert-True ($refused.Code -eq 2) "the variable should be refused with 2, got $($refused.Code)"
    Assert-True ($refused.Text -notmatch 'errormode=') 'the variable was refused only after the tests ran'

    'run_ctest_no_dialogs: all checks passed'
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

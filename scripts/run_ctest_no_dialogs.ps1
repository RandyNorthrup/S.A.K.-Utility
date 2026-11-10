<#
.SYNOPSIS
    Runs ctest so that a test cannot put a dialog or a window on this PC's screen.

.DESCRIPTION
    Use this for every local test run on a PC someone is working at. Arguments go to ctest as they
    are, and its exit code comes back.

    By default ctest resets the error mode its children inherit, whatever its own parent set: a
    probe under ctest read 0x0000 with the parent at 0x8003, so SetErrorMode in the parent does not
    reach the tests. With --interactive-debug-mode 0 the same probe reads 0x0003, so a test binary
    missing a DLL exits with 0xC0000135 at once, instead of waiting on a "System Error" box until
    someone closes it. That is what ctest's dashboard mode does, without a dashboard.

    QT_QPA_PLATFORM=offscreen is set for every test, because the suites that exercise the real
    desktop and clipboard otherwise open windows and write to the clipboard on the person's session.
    CI does not use this script: it keeps the native platform those suites exist to exercise.

    The CRT's own boxes are in-process, so no error mode reaches them. The hardware guard, which is
    linked into every native test binary, sends them to stderr.

.EXAMPLE
    scripts/run_ctest_no_dialogs.ps1 --test-dir build -C Release -j 6 --output-on-failure
#>

# No param block: PowerShell would try to bind ctest's own flags to it and refuse them.
$ErrorActionPreference = 'Stop'

if ($env:SAK_TEST_ALLOW_REAL_HARDWARE -eq '1') {
    [Console]::Error.WriteLine('SAK_TEST_ALLOW_REAL_HARDWARE is set. It belongs to CI runners, not to a local run.')
    exit 2
}

$env:QT_QPA_PLATFORM = 'offscreen'
& ctest --interactive-debug-mode 0 @args
exit $LASTEXITCODE

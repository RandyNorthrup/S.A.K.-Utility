$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $PSCommandPath
$ProjectRoot = Split-Path -Parent $ScriptRoot
. (Join-Path $ScriptRoot 'process_capture.ps1')

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Invoke-Script([string]$Path, [string[]]$Arguments) {
    $hostPath = (Get-Process -Id $PID).Path
    $allArguments = @(
        '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $Path
    ) + $Arguments
    return Invoke-CapturedProcess -FilePath $hostPath -Arguments $allArguments
}

$root = Join-Path ([System.IO.Path]::GetTempPath()) "sak-false-green-command-tests-$PID"
try {
    New-Item -ItemType Directory -Path (Join-Path $root 'build-tidy') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $root 'src') -Force | Out-Null
    $source = Join-Path $root 'src\sample.cpp'
    $sourceTwo = Join-Path $root 'src\sample_two.cpp'
    Set-Content -LiteralPath $source -Value 'int sample() { return 0; }' -Encoding ascii
    Set-Content -LiteralPath $sourceTwo -Value 'int sampleTwo() { return 0; }' -Encoding ascii
    $entry = @([ordered]@{
        directory = $root
        file = $source
        command = "cl.exe /c $source"
    }, [ordered]@{
        directory = $root
        file = $sourceTwo
        command = "cl.exe /c $sourceTwo"
    })
    [System.IO.File]::WriteAllText(
        (Join-Path $root 'build-tidy\compile_commands.json'),
        ($entry | ConvertTo-Json -Depth 4),
        [System.Text.UTF8Encoding]::new($false)
    )
    $clang = Join-Path $ScriptRoot 'run_clang_tidy.ps1'
    $base = @('-Root', $root, '-AutogenTarget', '', '-AnalyzerExitCodeForTesting')

    $failed = Invoke-Script $clang ($base + @('7'))
    Assert-True ($failed.exit_code -eq 7) (
        "Standalone analyzer tool failure was swallowed: exit=$($failed.exit_code) " +
        "stdout=$($failed.stdout) stderr=$($failed.stderr)"
    )

    $finding = Invoke-Script $clang ($base + @('1', '-AllowAnalyzerFindings'))
    Assert-True ($finding.exit_code -eq 0) 'Measurement mode rejected analyzer findings'

    $toolFailure = Invoke-Script $clang ($base + @('7', '-AllowAnalyzerFindings'))
    Assert-True ($toolFailure.exit_code -eq 7) 'Measurement mode swallowed analyzer tool failure'

    $clean = Invoke-Script $clang ($base + @('0'))
    Assert-True ($clean.exit_code -eq 0) 'Clean analyzer run failed'

    $review = Join-Path $ScriptRoot 'run_review_units.ps1'
    $reviewSource = Get-Content -Raw $review
    Assert-True ($reviewSource -match '\$codexResult = Invoke-BoundedReviewProcess') (
        'Codex review is not bound to shared stdin/timeout process capture'
    )
    Assert-True ($reviewSource -match '--expected-blob \$ExpectedBlob' -and
        $reviewSource -match '-ExpectedBlob \$afterBlob') (
        'Ledger mark is not bound to exact reviewed target bytes'
    )

    $ledgerRoot = Join-Path $root 'ledger-repo'
    New-Item -ItemType Directory -Path (Join-Path $ledgerRoot 'scripts') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $ledgerRoot 'src') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $ledgerRoot 'docs\review_briefs') -Force |
        Out-Null
    Copy-Item -LiteralPath (Join-Path $ScriptRoot 'review_ledger.py') -Destination (
        Join-Path $ledgerRoot 'scripts\review_ledger.py'
    )
    Copy-Item -LiteralPath $review -Destination (Join-Path $ledgerRoot 'scripts\run_review_units.ps1')
    Copy-Item -LiteralPath (Join-Path $ScriptRoot 'process_capture.ps1') -Destination (
        Join-Path $ledgerRoot 'scripts\process_capture.ps1'
    )
    $target = Join-Path $ledgerRoot 'src\sample.cpp'
    Set-Content -LiteralPath $target -Value 'int sample() { return 1; }' -Encoding ascii
    Set-Content -LiteralPath (Join-Path $ledgerRoot 'docs\review_briefs\sample.md') -Value (
        'NO FINDINGS'
    ) -Encoding ascii
    $successChild = Join-Path $ledgerRoot 'success-child.ps1'
    $timeoutChild = Join-Path $ledgerRoot 'timeout-child.ps1'
    $unsupportedChild = Join-Path $ledgerRoot 'unsupported-child.cmd'
    Set-Content -LiteralPath $successChild -Value @'
$inputText = [Console]::In.ReadToEnd()
if ($inputText -ne 'review stdin marker') { exit 8 }
Write-Output $inputText
'@ -Encoding ascii
    Set-Content -LiteralPath $timeoutChild -Value 'Start-Sleep -Seconds 5' -Encoding ascii
    Set-Content -LiteralPath $unsupportedChild -Value '@exit /b 0' -Encoding ascii

    $probeRoot = Join-Path $root 'batch-probe'
    $probeOutput = & python (Join-Path $ScriptRoot 'test_review_ledger_batch_hashing.py') `
        (Join-Path $ledgerRoot 'scripts\review_ledger.py') $probeRoot
    Assert-True ($LASTEXITCODE -eq 0 -and $probeOutput -match 'batch hashing probe passed') (
        'Review ledger did not hash only dirty tracked units'
    )
    Push-Location $ledgerRoot
    try {
        git init --quiet
        git config user.email test@example.invalid
        git config user.name 'Ledger Test'
        git config core.autocrlf true
        git add scripts/review_ledger.py scripts/run_review_units.ps1 scripts/process_capture.ps1 `
            src/sample.cpp docs/review_briefs/sample.md
        git commit --quiet -m seed

        $reviewCopy = Join-Path $ledgerRoot 'scripts\run_review_units.ps1'
        $markFailure = Invoke-Script $reviewCopy @(
            '-TestLedgerMarkOnly', '-LedgerMarkExitCodeForTesting', '5'
        )
        Assert-True ($markFailure.exit_code -ne 0 -and
            ($markFailure.stdout + $markFailure.stderr) -match 'mark failed with exit code 5') (
            'Ledger mark failure was swallowed'
        )
        $markSuccess = Invoke-Script $reviewCopy @(
            '-TestLedgerMarkOnly', '-LedgerMarkExitCodeForTesting', '0'
        )
        Assert-True ($markSuccess.exit_code -eq 0) 'Ledger mark success failed'

        $nextFailure = Invoke-Script $reviewCopy @(
            '-TestLedgerNextOnly', '-LedgerNextExitCodeForTesting', '6'
        )
        Assert-True ($nextFailure.exit_code -ne 0 -and
            ($nextFailure.stdout + $nextFailure.stderr) -match 'next failed with exit code 6') (
            'Ledger next failure was swallowed'
        )
        $nextSuccess = Invoke-Script $reviewCopy @(
            '-TestLedgerNextOnly', '-LedgerNextExitCodeForTesting', '0'
        )
        Assert-True ($nextSuccess.exit_code -eq 0) 'Ledger next success failed'

        $statusFailure = Invoke-Script $reviewCopy @(
            '-TestLedgerStatusOnly', '-LedgerStatusExitCodeForTesting', '7'
        )
        Assert-True ($statusFailure.exit_code -ne 0 -and
            ($statusFailure.stdout + $statusFailure.stderr) -match 'status failed with exit code 7') (
            'Ledger status failure was swallowed'
        )
        $statusSuccess = Invoke-Script $reviewCopy @(
            '-TestLedgerStatusOnly', '-LedgerStatusExitCodeForTesting', '0'
        )
        Assert-True ($statusSuccess.exit_code -eq 0) 'Ledger status success failed'

        Set-Content -LiteralPath $target -Value 'int sample() { return 2; }' -Encoding ascii
        $statusBefore = (git status --porcelain -- src/sample.cpp) -join "`n"
        $hashChanged = Invoke-Script $reviewCopy @(
            '-TestTargetHashOnly', '-TestTargetPath', 'src/sample.cpp',
            '-MutateTargetForTesting'
        )
        $statusAfter = (git status --porcelain -- src/sample.cpp) -join "`n"
        Assert-True ($statusBefore -eq $statusAfter -and $statusBefore -match '^ M ') (
            'Target mutation fixture did not preserve its already-dirty git-status row'
        )
        Assert-True ($hashChanged.exit_code -ne 0 -and
            ($hashChanged.stdout + $hashChanged.stderr) -match 'changed during review') (
            'Same-status-row target mutation was not detected'
        )
        Set-Content -LiteralPath $target -Value 'int sample() { return 2; }' -Encoding ascii

        $missingTarget = Invoke-Script $reviewCopy @(
            '-TestTargetHashOnly', '-TestTargetPath', 'src/missing.cpp'
        )
        Assert-True ($missingTarget.exit_code -ne 0 -and
            ($missingTarget.stdout + $missingTarget.stderr) -match 'missing or unreadable') (
            'Missing driver review target was accepted'
        )

        $captureSuccess = Invoke-Script $reviewCopy @(
            '-TestCodexCaptureOnly', '-TestCodexScriptPath', $successChild,
            '-TimeoutSeconds', '5'
        )
        Assert-True ($captureSuccess.exit_code -eq 0 -and
            $captureSuccess.stdout -match 'review stdin marker') (
            'Bounded Codex capture did not deliver stdin or report success'
        )
        $captureTimeout = Invoke-Script $reviewCopy @(
            '-TestCodexCaptureOnly', '-TestCodexScriptPath', $timeoutChild,
            '-TimeoutSeconds', '1'
        )
        Assert-True ($captureTimeout.exit_code -ne 0 -and
            ($captureTimeout.stdout + $captureTimeout.stderr) -match 'timed out after 1 seconds') (
            'Bounded Codex capture did not propagate timeout'
        )
        $unsupportedCommand = Invoke-Script $reviewCopy @(
            '-TestCodexCaptureOnly', '-TestCodexScriptPath', $unsupportedChild,
            '-TimeoutSeconds', '5'
        )
        Assert-True ($unsupportedCommand.exit_code -ne 0 -and
            ($unsupportedCommand.stdout + $unsupportedCommand.stderr) -match
                'Unsupported review command type') (
            'Unsupported Codex command type was accepted'
        )

        python scripts/review_ledger.py inventory | Out-Null
        $initialBlob = (git hash-object --path=src/sample.cpp -- src/sample.cpp).Trim()
        python scripts/review_ledger.py mark src/sample.cpp --brief docs/review_briefs/sample.md `
            --expected-blob $initialBlob | Out-Null
        Assert-True ($LASTEXITCODE -eq 0) 'Initial ledger mark failed'

        Set-Content -LiteralPath $target -Value 'int sample() { return 3; }' -Encoding ascii
        $next = python scripts/review_ledger.py next --count 5
        Assert-True ($LASTEXITCODE -eq 0 -and $next -match 'src/sample.cpp') (
            'Dirty reviewed target did not return to pending'
        )
        $pendingState = Get-Content -Raw scripts/review_ledger_state.json | ConvertFrom-Json
        Assert-True ($pendingState.units.'src/sample.cpp'.previous_review_blob -eq $initialBlob) (
            'Live next refresh destroyed prior review evidence'
        )

        python scripts/review_ledger.py mark src/sample.cpp --brief docs/review_briefs/sample.md `
            --expected-blob $initialBlob 2>&1 | Out-Null
        Assert-True ($LASTEXITCODE -ne 0) 'Ledger mark accepted bytes different from review hash'

        $expectedBlob = (git hash-object --path=src/sample.cpp -- src/sample.cpp).Trim()
        python scripts/review_ledger.py mark src/sample.cpp --brief docs/review_briefs/sample.md `
            --expected-blob $expectedBlob | Out-Null
        Assert-True ($LASTEXITCODE -eq 0) 'Working-tree ledger mark failed'
        $state = Get-Content -Raw scripts/review_ledger_state.json | ConvertFrom-Json
        Assert-True ($state.units.'src/sample.cpp'.reviewed_blob -eq $expectedBlob) (
            'Ledger mark did not record exact filtered working-tree bytes'
        )

        Set-Content -LiteralPath $target -Value 'int sample() { return 4; }' -Encoding ascii
        $status = python scripts/review_ledger.py status
        Assert-True ($LASTEXITCODE -eq 0 -and $status -match 'pending=1') (
            'Status did not reconcile dirty reviewed target'
        )
        python scripts/review_ledger.py next --count 5 | Out-Null
        $refreshedState = Get-Content -Raw scripts/review_ledger_state.json | ConvertFrom-Json
        Assert-True ($refreshedState.units.'src/sample.cpp'.previous_review_blob -eq $expectedBlob) (
            'Repeated live refresh destroyed prior review evidence'
        )

        Remove-Item -LiteralPath $target -Force -ErrorAction Stop
        python scripts/review_ledger.py status 2>&1 | Out-Null
        Assert-True ($LASTEXITCODE -ne 0) 'Missing tracked review target was accepted'

        $skipRoot = Join-Path $root 'skip-audit-repo'
        $skipScripts = Join-Path $skipRoot 'scripts'
        $skipTests = Join-Path $skipRoot 'tests'
        $skipResults = Join-Path $skipRoot 'build\test_results'
        New-Item -ItemType Directory -Path $skipScripts, $skipTests, $skipResults -Force |
            Out-Null
        Copy-Item -LiteralPath (Join-Path $ScriptRoot 'check_test_skips.ps1') -Destination (
            Join-Path $skipScripts 'check_test_skips.ps1'
        )
        Set-Content -LiteralPath (Join-Path $skipTests 'skip_site_inventory.txt') `
            -Value '# no static QSKIP sites in fixture' -Encoding ascii
        Set-Content -LiteralPath (Join-Path $skipTests 'skip_baseline.txt') -Value (
            'current_test::CurrentTests::external_optIn = external service is intentionally absent'
        ) -Encoding ascii
        $currentLog = Join-Path $skipResults 'current_test.txt'
        $removedLog = Join-Path $skipResults 'removed_test.txt'
        $qtLog = @'
********* Start testing of CurrentTests *********
SKIP   : CurrentTests::external_optIn() external service absent
Totals: 1 passed, 0 failed, 1 skipped, 0 blacklisted, 1ms
'@
        Set-Content -LiteralPath $currentLog -Value $qtLog -Encoding ascii
        Set-Content -LiteralPath $removedLog -Value ($qtLog.Replace('CurrentTests', 'RemovedTests')) `
            -Encoding ascii
        $registrationPath = Join-Path $skipRoot 'registration.json'
        $registration = [ordered]@{
            tests = @([ordered]@{
                    name = 'current_test'
                    command = @('current_test.exe', '-o', "$currentLog,txt", '-o', '-,txt')
                })
        }
        [IO.File]::WriteAllText(
            $registrationPath,
            ($registration | ConvertTo-Json -Depth 6),
            [Text.UTF8Encoding]::new($false)
        )
        $skipAudit = Join-Path $skipScripts 'check_test_skips.ps1'
        $skipClean = Invoke-Script $skipAudit @(
            '-ResultsDir', $skipResults, '-RegistrationJsonPath', $registrationPath
        )
        Assert-True ($skipClean.exit_code -eq 0 -and
            $skipClean.stdout -match 'stale removed-test logs ignored:\s*1') (
            'Skip audit counted a stale removed-test log'
        )
        Remove-Item -LiteralPath $currentLog -Force -ErrorAction Stop
        $skipMissing = Invoke-Script $skipAudit @(
            '-ResultsDir', $skipResults, '-RegistrationJsonPath', $registrationPath
        )
        Assert-True ($skipMissing.exit_code -ne 0 -and
            ($skipMissing.stdout + $skipMissing.stderr) -match 'REGISTERED QT TESTS WITH NO LOG') (
            'Skip audit accepted a missing current registered-test log'
        )
    }
    finally {
        Pop-Location
    }

    Write-Host "False-green command tests passed under $($PSVersionTable.PSVersion)."
}
finally {
    if (Test-Path -LiteralPath $root) {
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction Stop
    }
}

# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    Proves scripts/check_ctest_error_mode.ps1 finds every form of ctest call without
    --interactive-debug-mode 0, and allows only the nightly's documented exemption.

.DESCRIPTION
    The repository itself must pass. Then stand-in trees, one per case: each form of call the
    repository uses (a workflow's -FilePath ctest, an assignment, & $ctest.Source with its flag on
    a continued line, OpenCppCoverage's '--', 'ctest' list, Python's ["ctest", ...] across lines,
    -FilePath $ctest with -Arguments on the next line), with and without the flag; a ctest named
    only in a comment, a help block, a message string or a list of file names is not a call; the
    exemption is honored only in nightly.yml, only with its reasons, and only once; and a tree
    whose workflows call ctest nowhere fails, since the check would then prove nothing.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$checker = Join-Path $PSScriptRoot 'check_ctest_error_mode.ps1'
$repoRoot = Split-Path -Parent $PSScriptRoot
$hostPath = (Get-Process -Id $PID).Path

function Assert-Condition {
    param([Parameter(Mandatory = $true)] [bool]$Condition, [Parameter(Mandatory = $true)] [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Invoke-Checker {
    param([Parameter(Mandatory = $true)] [string]$Root)
    $output = @(& $hostPath -NoProfile -NonInteractive -File $checker -Root $Root 2>&1 | ForEach-Object { "$_" })
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Text = ($output -join "`n") }
}

$workflowWithFlag = @'
jobs:
  t:
    steps:
    - name: Test
      run: |
        & scripts/invoke_unelevated.ps1 -FilePath ctest -- --test-dir build -C Release `
          --interactive-debug-mode 0 -j 4 --output-on-failure
'@
$scratch = Join-Path ([IO.Path]::GetTempPath()) ("sak-ctest-error-mode-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null

# A stand-in tree: a workflow (the one above unless given) and files under scripts/.
function New-Tree {
    param([string]$Name, [hashtable]$Files, [string]$Workflow = $workflowWithFlag, [string]$WorkflowName = 'ci.yml')
    $tree = Join-Path $scratch $Name
    New-Item -ItemType Directory -Path (Join-Path $tree '.github/workflows'), (Join-Path $tree 'scripts') -Force | Out-Null
    if ($Workflow) { Set-Content -LiteralPath (Join-Path $tree ".github/workflows/$WorkflowName") -Value $Workflow }
    foreach ($relative in $Files.Keys) { Set-Content -LiteralPath (Join-Path $tree $relative) -Value $Files[$relative] }
    return $tree
}

function Assert-Verdict {
    param([string]$Name, [hashtable]$Files, [int]$Expected, [string]$Says = '', [string]$Workflow = $workflowWithFlag, [string]$WorkflowName = 'ci.yml')
    $result = Invoke-Checker (New-Tree -Name $Name -Files $Files -Workflow $Workflow -WorkflowName $WorkflowName)
    Assert-Condition ($result.ExitCode -eq $Expected) "${Name}: expected exit $Expected, got $($result.ExitCode): $($result.Text)"
    if ($Says) { Assert-Condition ($result.Text -match $Says) "${Name}: the output does not say /$Says/: $($result.Text)" }
    Write-Host "PASS $Name"
}

try {
    $real = Invoke-Checker $repoRoot
    Assert-Condition ($real.ExitCode -eq 0) "The repository fails its own check: $($real.Text)"
    Assert-Condition ($real.Text -match 'exempt, keeping its crash dumps: \.github/workflows/nightly\.yml:\d+') "The repository's one exemption is not the nightly's: $($real.Text)"
    Write-Host "PASS the repository: $($real.Text)"

    Assert-Verdict 'workflow-with-flag' @{} 0
    Assert-Verdict 'workflow-without-flag' @{} 1 'ci\.yml:6: ctest without --interactive-debug-mode 0' -Workflow ($workflowWithFlag -replace '--interactive-debug-mode 0 ', '')
    Assert-Verdict 'assignment-without-flag' @{ 'scripts/a.ps1' = '$json = ctest --test-dir b -C Release --show-only=json-v1' } 1 'scripts/a\.ps1:1:'
    Assert-Verdict 'assignment-with-flag' @{ 'scripts/a.ps1' = '$json = ctest --interactive-debug-mode 0 --test-dir b --show-only=json-v1' } 0
    Assert-Verdict 'plain-command-without-flag' @{ 'scripts/a.ps1' = "if (`$x) {`n    ctest --test-dir build -C Release`n}" } 1 'scripts/a\.ps1:2:'
    Assert-Verdict 'variable-continued-with-flag' @{ 'scripts/a.ps1' = "`$t = (& `$ctest.Source --test-dir `$b -C Release ```n    --interactive-debug-mode 0 --show-only=json-v1) -join ''" } 0
    Assert-Verdict 'variable-continued-without-flag' @{ 'scripts/a.ps1' = "`$t = (& `$ctest.Source --test-dir `$b -C Release ```n    --show-only=json-v1) -join ''" } 1 'scripts/a\.ps1:1:'
    Assert-Verdict 'filepath-variable-arguments-next-line' @{ 'scripts/a.ps1' = "`$r = Invoke-CapturedProcess -FilePath `$ctest -WorkingDirectory `$b ```n    -Arguments @('--interactive-debug-mode', '0', '-R', `$x)" } 0
    Assert-Verdict 'filepath-quoted-without-flag' @{ 'scripts/a.ps1' = "Invoke-LoggedCommand -Name x ```n    -FilePath `"ctest`" ```n    -Arguments @(`"--test-dir`", `$b)" } 1 'scripts/a\.ps1:2:'
    Assert-Verdict 'coverage-list-without-flag' @{ 'scripts/a.ps1' = "`$args = @(`n    '--', 'ctest', '--test-dir', `$b,`n    '--output-on-failure'`n)" } 1 'scripts/a\.ps1:2:'
    Assert-Verdict 'coverage-list-with-flag' @{ 'scripts/a.ps1' = "`$args = @(`n    '--', 'ctest', '--interactive-debug-mode', '0', '--test-dir', `$b`n)" } 0
    Assert-Verdict 'python-list-across-lines-with-flag' @{ 'scripts/a.py' = "result = _run([`"ctest`", `"--test-dir`", d,`n               `"--interactive-debug-mode`", `"0`"], 60, log)" } 0
    Assert-Verdict 'python-list-without-flag' @{ 'scripts/a.py' = "result = _run([`"ctest`", `"--test-dir`", d,`n               `"--show-only=json-v1`"], 60, log)" } 1 'scripts/a\.py:1:'
    Assert-Verdict 'flag-one-is-not-zero' @{ 'scripts/a.ps1' = 'ctest --interactive-debug-mode 1 --test-dir b' } 1 'scripts/a\.ps1:1:'
    Assert-Verdict 'not-calls' @{
        'scripts/a.ps1' = "<#`n.EXAMPLE`n    pwsh scripts/invoke_unelevated.ps1 -FilePath ctest -- --test-dir build`n#>`n# ctest --test-dir build`nWrite-Host 'Run the suite first (ctest --test-dir build -C Release).'`nthrow `"ctest --show-only exited 1`"`n`$tools = @('OpenCppCoverage.exe', 'ctest.exe', 'llvm-cov.exe')"
        'scripts/b.py' = "`"`"`"Runs the suite:`n    ctest --test-dir build`n`"`"`"`nprint('ok')"
    } 0

    $exemptReasons = "        # ctest-error-mode-exempt: error mode 0x0003 would stop WER writing this job's crash dump;`n        # a hang is stopped by each test's TIMEOUT, and check_program_imports ran first.`n"
    $exemptWorkflow = "jobs:`n  a:`n    steps:`n    - run: |`n$exemptReasons        & scripts/invoke_unelevated.ps1 -FilePath ctest -- --test-dir build-debug -C Debug`n"
    Assert-Verdict 'exemption-in-nightly' @{} 0 'exempt, keeping its crash dumps: \.github/workflows/nightly\.yml:7' -Workflow $exemptWorkflow -WorkflowName 'nightly.yml'
    Assert-Verdict 'exemption-in-ci' @{ '.github/workflows/nightly.yml' = $workflowWithFlag } 1 'allowed only in \.github/workflows/nightly\.yml' -Workflow $exemptWorkflow -WorkflowName 'ci.yml'
    Assert-Verdict 'exemption-without-reasons' @{} 1 'lacks: dump, TIMEOUT, check_program_imports' -Workflow ("jobs:`n  a:`n    steps:`n    - run: |`n        # ctest-error-mode-exempt: because`n        & scripts/invoke_unelevated.ps1 -FilePath ctest -- --test-dir b`n") -WorkflowName 'nightly.yml'
    Assert-Verdict 'two-exemptions' @{} 1 'more than one exempt ctest call' -Workflow ($exemptWorkflow + "    - run: |`n$exemptReasons        & scripts/invoke_unelevated.ps1 -FilePath ctest -- --test-dir b`n") -WorkflowName 'nightly.yml'
    Assert-Verdict 'exemption-in-a-script' @{ 'scripts/a.ps1' = "# ctest-error-mode-exempt: dump TIMEOUT check_program_imports`nctest --test-dir b" } 1 'scripts/a\.ps1:2: an exemption is allowed only'
    Assert-Verdict 'no-workflow-call' @{ 'scripts/a.ps1' = 'ctest --interactive-debug-mode 0 --test-dir b' } 1 'no ctest call found in \.github/' -Workflow "jobs:`n  a:`n    steps:`n    - run: echo hi`n"
} finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host 'check_ctest_error_mode: all checks passed'

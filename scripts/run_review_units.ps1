<#
.SYNOPSIS
    Run the next pending per-file review units through Codex and record them in the ledger.

.DESCRIPTION
    R5-LEDGER-1. This is the driver the remediation doc claimed existed. It does not exist
    anywhere in the repo's history, which is why "764 of 1098 units run" could never be resumed
    from or audited.

    For each pending unit (scripts/review_ledger.py picks them):
      1. Run Codex READ-ONLY at xhigh over that one file.
      2. Store the brief under docs/review_briefs/ so the finding survives in the tree -- the
         previous campaign kept none, so its findings cannot be re-audited at all.
      3. Mark the unit in the ledger with evidence=driver and filtered working-tree Git blob, so a
         later edit to that file automatically returns it to pending.

    Codex NEVER writes: the sandbox is bypassed (the codex-review skill records why), so
    read-only is enforced in the prompt, and target bytes are checked afterwards -- if reviewed
    file changes during a unit, driver STOPS rather than continuing to trust it.

    FINDINGS ARE LEADS, NOT VERDICTS. This driver deliberately does not fix anything and does not
    mark findings as confirmed. Every brief is verified by hand against the tree afterwards
    (R5-LEDGER-2); the first unit run this way produced 3 findings and all 3 were real, but the
    standing rule is still to verify before changing code.

.PARAMETER Count
    How many units to run. Each is a separate xhigh Codex call, so this is the budget dial.

.PARAMETER Group
    Restrict to one ledger group: src, include, tests, scripts, browser.

.EXAMPLE
    ./scripts/run_review_units.ps1 -Count 5 -Group src
#>
param(
    [int]$Count = 5,
    [string]$Group = "",
    [ValidateRange(1, 2147483)] [int]$TimeoutSeconds = 900,
    [switch]$TestLedgerMarkOnly,
    [ValidateRange(-1, 255)] [int]$LedgerMarkExitCodeForTesting = -1,
    [switch]$TestLedgerNextOnly,
    [ValidateRange(-1, 255)] [int]$LedgerNextExitCodeForTesting = -1,
    [switch]$TestLedgerStatusOnly,
    [ValidateRange(-1, 255)] [int]$LedgerStatusExitCodeForTesting = -1,
    [switch]$TestTargetHashOnly,
    [string]$TestTargetPath = '',
    [switch]$MutateTargetForTesting,
    [switch]$TestCodexCaptureOnly,
    [string]$TestCodexScriptPath = ''
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
Set-Location $RepoRoot
. (Join-Path $PSScriptRoot 'process_capture.ps1')

function Get-TargetBlob([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Review target is missing or unreadable: $Path"
    }
    $hashOutput = @(& git hash-object "--path=$Path" -- $Path)
    $hashExitCode = $LASTEXITCODE
    $blob = ($hashOutput -join '').Trim()
    if ($hashExitCode -ne 0 -or $blob -notmatch '^[0-9a-f]{40,64}$') {
        throw "Could not hash review target: $Path"
    }
    return $blob
}

function Invoke-BoundedReviewProcess(
    [string]$FilePath,
    [string[]]$Arguments,
    [string]$Prompt,
    [string]$Unit
) {
    $result = Invoke-CapturedProcess -FilePath $FilePath -Arguments $Arguments `
        -InputText $Prompt -TimeoutSeconds $TimeoutSeconds
    if ($result.timed_out) {
        throw "Codex review timed out after $TimeoutSeconds seconds for $Unit"
    }
    if ($result.exit_code -ne 0) {
        throw "Codex review failed for $Unit (exit $($result.exit_code))"
    }
    return $result
}

function Resolve-ReviewCommand([System.Management.Automation.CommandInfo]$CommandInfo) {
    if ($CommandInfo.CommandType -eq [System.Management.Automation.CommandTypes]::ExternalScript) {
        if ([System.IO.Path]::GetExtension($CommandInfo.Source) -ine '.ps1') {
            throw "Unsupported review command script: $($CommandInfo.Source)"
        }
        return [pscustomobject]@{
            FilePath = (Get-Process -Id $PID).Path
            PrefixArguments = @(
                '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
                '-File', $CommandInfo.Source
            )
        }
    }
    if ($CommandInfo.CommandType -eq [System.Management.Automation.CommandTypes]::Application -and
        [System.IO.Path]::GetExtension($CommandInfo.Source) -ieq '.exe') {
        return [pscustomobject]@{
            FilePath = $CommandInfo.Source
            PrefixArguments = @()
        }
    }
    throw "Unsupported review command type $($CommandInfo.CommandType): $($CommandInfo.Source)"
}

if ($TestTargetHashOnly) {
    $beforeBlob = Get-TargetBlob -Path $TestTargetPath
    if ($MutateTargetForTesting) {
        Add-Content -LiteralPath $TestTargetPath -Value 'mutation' -Encoding ascii
    }
    $afterBlob = Get-TargetBlob -Path $TestTargetPath
    if ($afterBlob -ne $beforeBlob) {
        throw "Review target changed during review: $TestTargetPath"
    }
    Write-Host 'Review target hash self-test passed.'
    return
}

if ($TestCodexCaptureOnly) {
    if (-not (Test-Path -LiteralPath $TestCodexScriptPath -PathType Leaf)) {
        throw "Test Codex script is missing or unreadable: $TestCodexScriptPath"
    }
    $testCommand = Get-Command $TestCodexScriptPath -ErrorAction Stop
    $resolvedTestCommand = Resolve-ReviewCommand -CommandInfo $testCommand
    $testResult = Invoke-BoundedReviewProcess -FilePath $resolvedTestCommand.FilePath `
        -Arguments $resolvedTestCommand.PrefixArguments -Prompt 'review stdin marker' `
        -Unit 'capture self-test'
    Write-Output $testResult.stdout
    return
}

function Invoke-LedgerMark([string]$Unit, [string]$Brief, [string]$ExpectedBlob) {
    if ($LedgerMarkExitCodeForTesting -ge 0) {
        $exitCode = $LedgerMarkExitCodeForTesting
    }
    else {
        python scripts/review_ledger.py mark $Unit --brief $Brief `
            --expected-blob $ExpectedBlob | Out-Null
        $exitCode = $LASTEXITCODE
    }
    if ($exitCode -ne 0) {
        throw "review_ledger.py mark failed with exit code $exitCode for $Unit"
    }
}

function Invoke-LedgerNext([string[]]$Arguments) {
    if ($LedgerNextExitCodeForTesting -ge 0) {
        $output = @('1 pending; next 1:', '  test/unit.cpp')
        $exitCode = $LedgerNextExitCodeForTesting
    }
    else {
        $output = @(python scripts/review_ledger.py @Arguments)
        $exitCode = $LASTEXITCODE
    }
    if ($exitCode -ne 0) {
        throw "review_ledger.py next failed with exit code $exitCode"
    }
    return $output
}

function Invoke-LedgerStatus {
    if ($LedgerStatusExitCodeForTesting -ge 0) {
        $output = @('ledger status self-test')
        $exitCode = $LedgerStatusExitCodeForTesting
    }
    else {
        $output = @(python scripts/review_ledger.py status)
        $exitCode = $LASTEXITCODE
    }
    if ($exitCode -ne 0) {
        throw "review_ledger.py status failed with exit code $exitCode"
    }
    return $output
}

if ($TestLedgerMarkOnly) {
    Invoke-LedgerMark -Unit 'test/unit.cpp' -Brief 'docs/review_briefs/test.md' `
        -ExpectedBlob ('0' * 40)
    Write-Host 'Ledger mark self-test passed.'
    return
}

if ($TestLedgerNextOnly) {
    Invoke-LedgerNext -Arguments @('next', '--count', '1') | Out-Null
    Write-Host 'Ledger next self-test passed.'
    return
}

if ($TestLedgerStatusOnly) {
    Invoke-LedgerStatus | Out-Null
    Write-Host 'Ledger status self-test passed.'
    return
}

$codexCmd = $null
try {
    $codexCmd = Get-Command codex -ErrorAction Stop
    $resolvedCodex = Resolve-ReviewCommand -CommandInfo $codexCmd
}
catch {
    Write-Error "REVIEW DRIVER FAILED: codex CLI not on PATH. npm install -g @openai/codex@latest"
    exit 1
}
$briefDir = Join-Path $RepoRoot "docs/review_briefs"
New-Item -ItemType Directory -Force -Path $briefDir | Out-Null

# Each review hashes its exact target immediately before and after Codex. This catches a second edit
# to an already-dirty file, which leaves the `git status --porcelain` row unchanged. Unrelated
# concurrent edits remain allowed because they cannot change the reviewed target's blob.

$ledgerArgs = @("next", "--count", "$Count")
if ($Group) { $ledgerArgs += @("--group", $Group) }
$pending = (Invoke-LedgerNext -Arguments $ledgerArgs) |
    Where-Object { $_ -match "^\s{2}\S" } | ForEach-Object { $_.Trim() }

if (-not $pending) {
    Write-Host "no pending units"
    exit 0
}

Write-Host "running $($pending.Count) unit(s) through Codex (xhigh, read-only)"
$done = 0
foreach ($unit in $pending) {
    $safeName = ($unit -replace "[\\/]", "_") -replace "\.[^.]+$", ""
    $briefRel = "docs/review_briefs/$safeName.md"
    Write-Host "  $unit ..."

    # One line of scope, one of goal, one of what to report, one enforcing read-only -- the shape
    # the codex-review skill settled on. NO FINDINGS is required for an empty result so that a
    # silent/empty brief is distinguishable from a clean file.
    $prompt = @"
Read-only review of $unit as it stands in the LOCAL working tree (there is no diff; do NOT rely
on any remote). Goal: find real defects in this file. Report ONLY: (1) bugs/correctness,
(2) anything missing versus the file's own stated contract, (3) quality (dead code, duplication,
error handling, naming). Cite file:line, rank by severity, and output ONLY the final findings
list with no reasoning. If there is nothing real to report, output exactly: NO FINDINGS.
Do NOT modify, create or delete any file; only read and report.
"@

    # Sampled here, one line before the call, so unrelated concurrent edits do not false-alarm.
    $beforeBlob = Get-TargetBlob -Path $unit
    $log = Join-Path $env:TEMP "codex_unit_$safeName.log"
    $codexArgs = @($resolvedCodex.PrefixArguments) + @(
        'exec', '-m', 'gpt-5.6-sol', '-c', 'model_reasoning_effort=xhigh',
        '--dangerously-bypass-approvals-and-sandbox', '--skip-git-repo-check', '-o', $briefRel)
    $codexResult = Invoke-BoundedReviewProcess -FilePath $resolvedCodex.FilePath `
        -Arguments $codexArgs -Prompt $prompt -Unit $unit
    @($codexResult.stdout, $codexResult.stderr) | Set-Content -LiteralPath $log

    if (-not (Test-Path $briefRel) -or -not (Get-Content $briefRel -Raw).Trim()) {
        Write-Host "    no brief produced -- see $log (usage limit? auth?). STOPPING."
        Write-Host "    A unit with no brief must NOT be marked reviewed."
        break
    }

    # Codex is instructed never to write. Verify target bytes rather than trusting that instruction.
    $afterBlob = Get-TargetBlob -Path $unit
    if ($afterBlob -ne $beforeBlob) {
        Write-Host "    WORKING TREE CHANGED during a read-only review. STOPPING."
        Write-Host "      target blob changed: $beforeBlob -> $afterBlob"
        Write-Host "    Codex is instructed never to write, so this is either that instruction"
        Write-Host "    failing -- which must be looked at -- or, far more likely, somebody"
        Write-Host "    editing the repo while the batch ran. DO NOT EDIT DURING A BATCH: the"
        Write-Host "    check cannot tell the two apart, and marking a unit reviewed while its"
        Write-Host "    file is in flux is exactly the unverifiable bookkeeping this ledger"
        Write-Host "    replaced. Nothing was marked; re-run when the tree is settled."
        break
    }

    Invoke-LedgerMark -Unit $unit -Brief $briefRel -ExpectedBlob $afterBlob
    $findings = (Get-Content $briefRel -Raw)
    $verdict = if ($findings -match "NO FINDINGS") { "no findings" } else { "findings recorded" }
    Write-Host "    $verdict -> $briefRel"
    $done++
}

Write-Host ""
Write-Host "$done unit(s) reviewed and recorded."
Write-Host "NEXT: verify each brief against the tree by hand before changing anything (LEDGER-2)."
Invoke-LedgerStatus | ForEach-Object { Write-Output $_ }

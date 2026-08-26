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
      3. Mark the unit in the ledger with evidence=driver and the file's git BLOB SHA, so a later
         edit to that file automatically returns it to pending.

    Codex NEVER writes: the sandbox is bypassed (the codex-review skill records why), so
    read-only is enforced in the prompt, and the run is checked afterwards -- if the working tree
    changed during a unit, the driver STOPS rather than continuing to trust it.

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
    [int]$TimeoutSeconds = 900
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
Set-Location $RepoRoot

$codexCmd = Get-Command codex -ErrorAction SilentlyContinue
if (-not $codexCmd) {
    Write-Error "REVIEW DRIVER FAILED: codex CLI not on PATH. npm install -g @openai/codex@latest"
    exit 1
}
$briefDir = Join-Path $RepoRoot "docs/review_briefs"
New-Item -ItemType Directory -Force -Path $briefDir | Out-Null

# The tree must be clean-ish going in, so "did Codex touch anything?" is answerable afterwards.
$baseline = (git status --porcelain) -join "`n"

$args = @("next", "--count", "$Count")
if ($Group) { $args += @("--group", $Group) }
$pending = (python scripts/review_ledger.py @args) |
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

    $log = Join-Path $env:TEMP "codex_unit_$safeName.log"
    $prompt | & $codexCmd.Source exec -m gpt-5.6-sol -c model_reasoning_effort=xhigh `
        --dangerously-bypass-approvals-and-sandbox --skip-git-repo-check `
        -o $briefRel *> $log

    if (-not (Test-Path $briefRel) -or -not (Get-Content $briefRel -Raw).Trim()) {
        Write-Host "    no brief produced -- see $log (usage limit? auth?). STOPPING."
        Write-Host "    A unit with no brief must NOT be marked reviewed."
        break
    }

    # Codex is instructed never to write. Verify rather than trust: anything new in the tree that
    # is not the brief itself means the run cannot be trusted, so stop and let a human look.
    $now = (git status --porcelain) -join "`n"
    $unexpected = ($now -split "`n" | Where-Object {
        $_ -and ($baseline -notmatch [regex]::Escape($_)) -and ($_ -notmatch "review_ledger_state|review_briefs")
    })
    if ($unexpected) {
        Write-Host "    WORKING TREE CHANGED during a read-only review. STOPPING."
        $unexpected | ForEach-Object { Write-Host "      $_" }
        break
    }

    python scripts/review_ledger.py mark $unit --brief $briefRel | Out-Null
    $findings = (Get-Content $briefRel -Raw)
    $verdict = if ($findings -match "NO FINDINGS") { "no findings" } else { "findings recorded" }
    Write-Host "    $verdict -> $briefRel"
    $done++
}

Write-Host ""
Write-Host "$done unit(s) reviewed and recorded."
Write-Host "NEXT: verify each brief against the tree by hand before changing anything (LEDGER-2)."
python scripts/review_ledger.py status

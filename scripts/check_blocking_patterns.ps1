<#
.SYNOPSIS
    Fails on nested event loops, UI pumping, and blocking waits that are not
    explicitly justified at the site.

.DESCRIPTION
    Two things can hang this application forever, and both are gated:

      1. Nested event loops and UI pumping. A QEventLoop object or a
         processEvents() call re-enters event delivery, so code that the caller
         believes is suspended can run again underneath it - destructors, timers
         and queued slots all fire mid-statement.
      2. UNBOUNDED blocking waits: wait() / waitForFinished() and friends called
         with no timeout. If the thread or future being waited on never
         completes, the caller is stuck with no way out. On the GUI thread that
         is a frozen window that never recovers.

    A BOUNDED wait - wait(kTimeoutMs) - is deliberately NOT gated. It cannot
    hang: it gives up and the caller decides what to do. Joining a thread you own
    before its state dies is mandatory C++ lifetime management, not a defect, and
    this codebase does it about 40 times; demanding a written justification for
    each would make the marker meaningless. Being honest about that limit matters
    more than pretending to cover it: a bounded wait on the GUI thread still
    freezes the window for its timeout, and this gate does not catch that.

    Neither gated form is always wrong. An async->sync bridge on a worker thread
    needs a local event loop, and some joins genuinely must not give up. So this
    is an OPT-IN gate rather than a ban: every match must carry a justification
    at the site saying why hanging forever beats giving up.

        thread.wait();  // SAK-ALLOW-BLOCKING: reason it must not give up

        // Some existing explanation of the teardown.
        // SAK-ALLOW-BLOCKING: reason it must not give up
        thread.wait();

    The marker exempts the line it sits on, or the statement directly below the
    contiguous // comment block containing it. A reason is mandatory - a bare
    marker fails.

    Deliberately NOT a per-file allowlist. A file-scoped pass hides every later
    violation added to that file, which is how the previous version of this
    check accumulated 25 blanket entries (2 of them long stale) while never
    actually running: it exited on the event-loop half first, so the wait half
    had never been evaluated and had 45 unreviewed matches behind it.

    Unused markers fail too. A marker whose line stopped matching is a stale
    claim about code that no longer exists, and left alone it would quietly
    pre-authorize whatever gets written there next.
#>

param(
    [string]$Root = "."
)

$ErrorActionPreference = "Stop"

# A QEventLoop OBJECT (local, member, reference or pointer) or a processEvents()
# call. Deliberately not the bare type name: `#include <QEventLoop>` and the
# QEventLoop::ExcludeUserInputEvents enum are not themselves nested loops, and
# flagging them would force markers onto lines that block nothing. One marker per
# loop object covers every exec() call on it.
$eventLoopPattern = 'QEventLoop\b\s*[&*]*\s*\w|processEvents\s*\('

# A join with NO timeout argument: wait(), waitForFinished(), waitForStarted()...
# The empty argument list is the whole point. wait(kTimeoutMs) is bounded, cannot
# hang, and is not gated.
$blockingWaitPattern = '\bwait\s*\(\s*\)|waitFor\w*\s*\(\s*\)'

$markerPattern = 'SAK-ALLOW-BLOCKING:'

function Invoke-Rg {
    param([string[]]$RgArgs)

    # Application only, invoked by resolved path: an alias, function or script
    # named rg could emit nothing and leave a stale 0/1 in $LASTEXITCODE, passing
    # the gate without ever searching. Pin the real native executable.
    $rg = @(Get-Command rg -CommandType Application -ErrorAction SilentlyContinue)[0]
    if (-not $rg) {
        throw "Required tool missing: rg (native executable)"
    }

    # Keep stderr: a non-zero rg failure has to report WHY, not just its code.
    $errFile = [System.IO.Path]::GetTempFileName()
    try {
        $output = & $rg.Source @RgArgs 2> $errFile
        $code = $LASTEXITCODE
        # Only 0 (matched) and 1 (no match) are success. Anything else - a crash
        # (negative NTSTATUS exit), an unreadable path, a killed process - must
        # fail closed, never be read as "no blocking patterns found".
        if ($code -ne 0 -and $code -ne 1) {
            $stderr = (Get-Content -LiteralPath $errFile -Raw)
            throw "rg failed with exit code ${code}: rg $($RgArgs -join ' ')`n$stderr"
        }
    } finally {
        Remove-Item -LiteralPath $errFile -Force -ErrorAction SilentlyContinue
    }
    return @($output)
}

# The part of a source line that is actually code: trailing // comment removed,
# then double-quoted string literals blanked. Without this the gate matches
# prose ("...freezing the GUI in the destructor's waitForFinished()") and data
# (QStringLiteral("waitFor") is a browser-contract JSON key), and demands a
# justification marker on lines that cannot block anything.
function Get-CodePortion {
    param([string]$Line)

    $code = [regex]::Replace($Line, '"(\\.|[^"\\])*"', '""')
    # Inline /* ... */ spans, e.g. `foo();  /* thread.wait() one day */`.
    $code = [regex]::Replace($code, '/\*.*?\*/', ' ')
    $commentAt = $code.IndexOf('//')
    if ($commentAt -ge 0) {
        $code = $code.Substring(0, $commentAt)
    }
    $trimmed = $code.TrimStart()
    # Continuation line of a multi-line /* */ or doc block. Known limit: a block
    # comment whose continuation lines do NOT lead with * is not detected, so a
    # gated pattern buried in one would still be reported. That errs toward asking
    # for a justification, never toward missing a real blocking call.
    if ($trimmed.StartsWith('*') -or $trimmed.StartsWith('/*')) {
        return ""
    }
    return $code
}

function Get-Violations {
    param(
        [string[]]$RgOutput,
        [hashtable]$FileLines,
        [hashtable]$UsedMarkers
    )

    $violations = @()
    foreach ($entry in $RgOutput) {
        if ($entry -notmatch '^(?<file>.+?):(?<line>\d+):(?<text>.*)$') {
            throw "Unparseable rg output line: $entry"
        }
        $file = $matches['file']
        $lineNo = [int]$matches['line']
        $text = $matches['text']

        $lines = $FileLines[$file]
        $justified = $false

        # Marker on the line itself.
        if ($text -match $markerPattern) {
            $UsedMarkers["${file}:${lineNo}"] = $true
            $justified = $true
        }

        # Or anywhere in the contiguous // comment block directly above the
        # statement. Walking the whole block (rather than just one line) lets the
        # reason live with the explanation it belongs to, and survives
        # clang-format reflowing a marker line past the 100-column limit - which
        # would otherwise push the code out of a strict one-line-above check and
        # fail the gate for a purely cosmetic reason. A blank line ends the block,
        # so the marker still has to sit against the code it justifies.
        for ($idx = $lineNo - 2; $idx -ge 0; $idx--) {
            $above = $lines[$idx]
            if ($above.TrimStart() -notmatch '^//') {
                break
            }
            if ($above -match $markerPattern) {
                $UsedMarkers["${file}:$($idx + 1)"] = $true
                $justified = $true
            }
        }

        if (-not $justified) {
            $violations += "${file}:${lineNo}: $($text.Trim())"
        }
    }
    return $violations
}

$repo = (Resolve-Path -LiteralPath $Root).Path
Push-Location $repo
try {
    $searchRoots = @("src", "include")

    $rawMatches = @()
    foreach ($pattern in @($eventLoopPattern, $blockingWaitPattern)) {
        $rawMatches += Invoke-Rg -RgArgs (@("--line-number", "--no-heading", $pattern) + $searchRoots)
    }

    # Cache each file once; the gate re-reads lines for marker lookups.
    $fileLines = @{}
    foreach ($entry in $rawMatches) {
        if ($entry -match '^(?<file>.+?):(?<line>\d+):') {
            $file = $matches['file']
            if (-not $fileLines.ContainsKey($file)) {
                $fileLines[$file] = @(Get-Content -LiteralPath $file)
            }
        }
    }

    # Drop matches that live in comments or string literals: they block nothing.
    $codeMatches = @()
    foreach ($entry in $rawMatches) {
        if ($entry -notmatch '^(?<file>.+?):(?<line>\d+):(?<text>.*)$') {
            throw "Unparseable rg output line: $entry"
        }
        $text = $matches['text']
        $code = Get-CodePortion -Line $text
        if (($code -match $eventLoopPattern) -or ($code -match $blockingWaitPattern)) {
            $codeMatches += $entry
        }
    }
    $codeMatches = @($codeMatches | Sort-Object -Unique)

    $usedMarkers = @{}
    $violations = Get-Violations -RgOutput $codeMatches -FileLines $fileLines -UsedMarkers $usedMarkers

    if ($violations.Count -gt 0) {
        Write-Error ("Unjustified blocking / nested-event-loop pattern ($($violations.Count)):`n" +
            ($violations -join "`n") +
            "`n`nEither remove the block, or justify it at the site with a trailing or preceding" +
            "`n    // SAK-ALLOW-BLOCKING: <why this one is correct>")
        exit 1
    }

    # Every marker in the tree must currently exempt something.
    $markerLines = Invoke-Rg -RgArgs (@("--line-number", "--no-heading", $markerPattern) + $searchRoots)
    $staleMarkers = @()
    $unreasoned = @()
    foreach ($entry in $markerLines) {
        if ($entry -notmatch '^(?<file>.+?):(?<line>\d+):(?<text>.*)$') {
            throw "Unparseable rg output line: $entry"
        }
        $file = $matches['file']
        $lineNo = [int]$matches['line']
        $text = $matches['text']

        if ($text -notmatch "$markerPattern\s*\S") {
            $unreasoned += "${file}:${lineNo}: $($text.Trim())"
            continue
        }
        if (-not $usedMarkers.ContainsKey("${file}:${lineNo}")) {
            $staleMarkers += "${file}:${lineNo}: $($text.Trim())"
        }
    }

    if ($unreasoned.Count -gt 0) {
        Write-Error ("SAK-ALLOW-BLOCKING marker with no reason ($($unreasoned.Count)):`n" +
            ($unreasoned -join "`n") +
            "`n`nThe reason is the entire point of the marker. State why the block is correct.")
        exit 1
    }

    if ($staleMarkers.Count -gt 0) {
        Write-Error ("Stale SAK-ALLOW-BLOCKING marker, exempts nothing ($($staleMarkers.Count)):`n" +
            ($staleMarkers -join "`n") +
            "`n`nThe code it justified is gone. Delete the marker - left in place it" +
            "`npre-authorizes whatever is written there next.")
        exit 1
    }

    Write-Host "Blocking-pattern check passed ($($codeMatches.Count) justified site(s))."
}
finally {
    Pop-Location
}

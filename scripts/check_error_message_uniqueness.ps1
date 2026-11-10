<#
.SYNOPSIS
    Enforces that user-facing failure messages are specific and unique.

.DESCRIPTION
    Static/non-mutating release gate. Field support is only tractable when an
    error message identifies the failure, so this gate scans first-party C++
    (src\** and include\**, excluding any third_party subtree) for the string
    literals that feed the error sinks this codebase uses and enforces two rules:

      (a) BANNED VAGUE PHRASE (always hard-fails): no message literal may contain
          "unknown error" (case-insensitive). A message that says only "unknown
          error" tells a technician nothing; name the actual failure instead.

      (b) NO NEW DUPLICATE MESSAGES: two DISTINCT failure sites must not emit the
          exact same non-trivial message literal, because a duplicate makes a
          field report ambiguous about which site failed. The codebase already
          carries many legacy duplicates (repeated precondition guards, shared
          filesystem-writer errors), so this half runs against a committed
          baseline allowlist: pre-existing duplicate texts are grandfathered and
          only NEW duplicates fail. Regenerate the baseline with -UpdateBaseline
          after an intentional, reviewed change.

    Error sinks recognized (the message is the first non-trivial string literal on
    the sink's code line): errorOccurred(...), setError(...), logError(...), and
    blockers list appends (<<, .append/.push_back/.prepend/.emplace_back).

    Fails closed: an unreadable source file, or a missing baseline on a normal
    run, is an error -- never a silent pass.
#>

[CmdletBinding()]
param(
    [string]$ProjectRoot = "",
    [string]$BaselinePath = "",
    [switch]$UpdateBaseline
)

$ErrorActionPreference = "Stop"

# A message shorter than this, or with fewer than the minimum distinct alphabetic
# words, is treated as trivial (a bare format string like "%1" or a fragment) and
# is not tracked for duplication. Two distinct sites are required to call a text a
# duplicate.
$script:MinMessageLength = 15
$script:MinDistinctWords = 3
$script:DuplicateSiteThreshold = 2
$script:BannedPhrase = "unknown error"

$script:LiteralRegex = [regex]'"(?:[^"\\]|\\.)*"'
$script:WordRegex = [regex]'[A-Za-z]{2,}'
$script:SinkRegex = [regex](
    'errorOccurred\s*\(' +
    '|\bsetError\s*\(' +
    '|\blogError\s*\(' +
    '|blockers[^;]*?(?:<<|\.append\s*\(|\.push_back\s*\(|\.prepend\s*\(|\.emplace_back\s*\()'
)

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent $PSScriptRoot
}
if ([string]::IsNullOrWhiteSpace($BaselinePath)) {
    $BaselinePath = Join-Path $PSScriptRoot "error_message_duplicate_baseline.json"
}

# Return the code portion of a line: everything before an unquoted "//" or "/*".
# Message literals never legitimately live in a comment, so stripping comments
# keeps a commented-out sink or phrase from tripping the gate.
function Get-CodePortion {
    param([string]$Line)

    $inString = $false
    for ($i = 0; $i -lt $Line.Length; $i++) {
        $ch = $Line[$i]
        if ($ch -eq '"') {
            $backslashes = 0
            $j = $i - 1
            while ($j -ge 0 -and $Line[$j] -eq '\') { $backslashes++; $j-- }
            if (($backslashes % 2) -eq 0) { $inString = -not $inString }
            continue
        }
        if (-not $inString -and $ch -eq '/' -and ($i + 1) -lt $Line.Length) {
            $next = $Line[$i + 1]
            if ($next -eq '/' -or $next -eq '*') { return $Line.Substring(0, $i) }
        }
    }
    return $Line
}

# Extract the inner text of every C++ double-quoted string literal in a code line.
function Get-StringLiterals {
    param([string]$Code)

    $out = @()
    foreach ($m in $script:LiteralRegex.Matches($Code)) {
        $out += $m.Value.Substring(1, $m.Value.Length - 2)
    }
    return $out
}

# A trackable duplicate candidate: long enough and with enough real words to be a
# human-readable message rather than a bare format string or fragment.
function Test-NonTrivialMessage {
    param([string]$Message)

    if ($Message.Length -lt $script:MinMessageLength) { return $false }
    return $script:WordRegex.Matches($Message).Count -ge $script:MinDistinctWords
}

function Get-ScanFiles {
    param([string[]]$Roots)

    $files = @()
    foreach ($root in $Roots) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        $files += Get-ChildItem -LiteralPath $root -Recurse -File `
                -Include *.cpp, *.cc, *.cxx, *.h, *.hpp, *.hxx -ErrorAction Stop |
            Where-Object { $_.FullName -notmatch '[\\/]third_party[\\/]' }
    }
    return , @($files | Sort-Object FullName -Unique)
}

$roots = @((Join-Path $ProjectRoot "src"), (Join-Path $ProjectRoot "include"))
$files = Get-ScanFiles -Roots $roots

$bannedViolations = @()
# message text -> ordinal-unique set of "relative_path:line" sites.
$messageSites = @{}

foreach ($file in $files) {
    $relative = $file.FullName.Substring($ProjectRoot.Length).TrimStart('\', '/')
    $lines = Get-Content -LiteralPath $file.FullName -ErrorAction Stop
    for ($n = 0; $n -lt $lines.Count; $n++) {
        $line = $lines[$n]
        if ($line.IndexOf('"') -lt 0) { continue }
        $code = Get-CodePortion -Line $line
        $literals = @(Get-StringLiterals -Code $code)
        if ($literals.Count -eq 0) { continue }

        foreach ($literal in $literals) {
            if ($literal.ToLowerInvariant().Contains($script:BannedPhrase)) {
                $bannedViolations += "${relative}:$($n + 1): `"$literal`""
            }
        }

        if (-not $script:SinkRegex.IsMatch($code)) { continue }
        foreach ($literal in $literals) {
            $message = $literal.Trim()
            if (-not (Test-NonTrivialMessage -Message $message)) { continue }
            if (-not $messageSites.ContainsKey($message)) {
                $messageSites[$message] =
                    New-Object "System.Collections.Generic.HashSet[string]" `
                        -ArgumentList @([StringComparer]::Ordinal)
            }
            [void]$messageSites[$message].Add("${relative}:$($n + 1)")
            break
        }
    }
}

# Banned phrase is unconditional -- it can never be baselined away.
if ($bannedViolations.Count -gt 0) {
    Write-Host "Error-message uniqueness FAILED: banned vague phrase '$($script:BannedPhrase)'"
    Write-Host "in a user-facing message literal. Name the actual failure instead:"
    $bannedViolations | Sort-Object | ForEach-Object { Write-Host "  $_" }
    exit 1
}

$currentDuplicates = New-Object "System.Collections.Generic.SortedSet[string]" `
    -ArgumentList @([StringComparer]::Ordinal)
foreach ($entry in $messageSites.GetEnumerator()) {
    if ($entry.Value.Count -ge $script:DuplicateSiteThreshold) {
        [void]$currentDuplicates.Add($entry.Key)
    }
}

if ($UpdateBaseline) {
    $payload = [ordered]@{
        schema_version = 1
        description = "Grandfathered duplicate error-message literals; new duplicates fail."
        allowed_duplicate_messages = @($currentDuplicates)
    }
    $json = $payload | ConvertTo-Json -Depth 4
    Set-Content -LiteralPath $BaselinePath -Value $json -Encoding ascii
    Write-Host "Baseline updated: $($currentDuplicates.Count) duplicate message(s) recorded."
    exit 0
}

if (-not (Test-Path -LiteralPath $BaselinePath -PathType Leaf)) {
    Write-Host "Error-message uniqueness FAILED: baseline missing at $BaselinePath."
    Write-Host "Establish it by running this script with the -UpdateBaseline switch."
    exit 1
}

$baselineRaw = Get-Content -LiteralPath $BaselinePath -Raw | ConvertFrom-Json
$allowed = New-Object "System.Collections.Generic.HashSet[string]" `
    -ArgumentList @([StringComparer]::Ordinal)
foreach ($msg in @($baselineRaw.allowed_duplicate_messages)) {
    [void]$allowed.Add([string]$msg)
}

$newDuplicates = @()
foreach ($message in $currentDuplicates) {
    if (-not $allowed.Contains($message)) {
        $sites = @($messageSites[$message]) | Sort-Object
        $newDuplicates += [pscustomobject]@{ Message = $message; Sites = $sites }
    }
}

if ($newDuplicates.Count -gt 0) {
    Write-Host "Error-message uniqueness FAILED: NEW duplicate message literal(s) emitted from"
    Write-Host "distinct failure sites. Disambiguate one side (add the operation or target), or"
    Write-Host "if this duplication is intentional run -UpdateBaseline to grandfather it:"
    foreach ($dup in $newDuplicates) {
        Write-Host "  `"$($dup.Message)`""
        $dup.Sites | ForEach-Object { Write-Host "      $_" }
    }
    exit 1
}

$summaryFormat = "Error-message uniqueness passed: {0} sink message(s), " +
    "{1} grandfathered duplicate(s), no banned phrase, no new duplicates ({2} files)."
Write-Host ($summaryFormat -f $messageSites.Count, $currentDuplicates.Count, $files.Count)
exit 0

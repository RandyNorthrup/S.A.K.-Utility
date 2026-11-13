# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    Fails when a workflow or a scripts/ runner calls ctest without --interactive-debug-mode 0.

.DESCRIPTION
    By default ctest gives its tests error mode 0, and a test program that cannot load a DLL then
    puts up a Windows error box and waits for an answer nobody gives: on the 9170b149 nightly
    (2026-11-12) two writer-CLI tests waited until the job's 330-minute limit. With
    --interactive-debug-mode 0 the tests get error mode 0x0003, and the same program exits at once
    with 0xC0000135.

    One call may go without it: the nightly's Debug + ASan test step, where error mode 0x0003 would
    also stop Windows Error Reporting writing the crash dumps that job collects (measured on testpc1,
    2026-11-13: no dump for an access violation or a fail-fast at 0x0003, one for each at 0). That
    call carries, in the comment lines directly above it, the marker "ctest-error-mode-exempt:"
    and why: the word "dump", and what guards it instead, "TIMEOUT" and "check_program_imports".
    An exemption anywhere else, or a second one, fails.

    A call is a ctest command (ctest ..., & ctest ..., x = ctest ...), a -FilePath naming ctest or
    a $ctest variable, & $ctest..., or an argument list with 'ctest' in it (OpenCppCoverage's
    "--, ctest", Python's ["ctest", ...]). Its arguments are the line and the lines it continues
    onto (a trailing backtick or comma, or brackets left open). Comments, PowerShell block comments
    and Python triple-quoted strings are not read.

.PARAMETER Root
    The repository root to check. Defaults to this script's repository.
#>
[CmdletBinding()]
param(
    [string]$Root = ''
)

$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }
$Root = (Resolve-Path -LiteralPath $Root).Path

$exemptionMarker = 'ctest-error-mode-exempt:'
$exemptionFile = '.github/workflows/nightly.yml'
$exemptionWords = @('dump', 'TIMEOUT', 'check_program_imports')
$flagPattern = '--interactive-debug-mode(?:[''"]\s*,\s*[''"]|\s+|=)0(?![0-9])'
# Read with string literals emptied (a message that names ctest is not a call)...
$commandPatterns = @(
    '(?:^|[;(=|{])\s*(?:&\s*)?ctest(?:\.exe)?\s+\S',
    '&\s*\$ctest\w*(?:\.Source)?\s'
)
# ...and as written, since these name ctest in quotes.
$quotedPatterns = @(
    '-FilePath\s+(?:[''"]ctest(?:\.exe)?[''"]|ctest(?:\.exe)?(?=\s|$)|\$ctest\w*)',
    # 'ctest' alone as a list element: OpenCppCoverage's '--', 'ctest' and Python's ["ctest", ...]
    # ('ctest.exe' in a list of file names is not a call).
    '[\[(,]\s*[''"]ctest[''"]\s*(?:,|\])'
)

# The file's lines with comments, PowerShell block comments and Python triple-quoted strings
# blanked, so that line numbers still match.
function Get-CodeLine {
    param([string]$Path, [string]$Kind)
    $lines = @(Get-Content -LiteralPath $Path)
    $code = New-Object string[] $lines.Count
    $inBlock = $false
    for ($index = 0; $index -lt $lines.Count; $index++) {
        $line = $lines[$index]
        if ($Kind -eq 'ps1') {
            if ($inBlock) {
                if ($line -match '#>') { $inBlock = $false }
                $line = ''
            } elseif ($line -match '^\s*<#') {
                $inBlock = -not ($line -match '#>')
                $line = ''
            }
        } elseif ($Kind -eq 'py') {
            $quotes = ([regex]::Matches($line, '"""|''''''')).Count
            if ($inBlock) {
                if ($quotes % 2 -eq 1) { $inBlock = $false }
                $line = ''
            } elseif ($quotes % 2 -eq 1) {
                $inBlock = $true
                $line = ''
            }
        }
        if ($line -match '^\s*#') { $line = '' }
        $code[$index] = $line
    }
    return , $code
}

# The comment lines directly above line $Index (blank lines between them skipped).
function Get-CommentAbove {
    param([string[]]$Lines, [int]$Index)
    $comments = @()
    for ($above = $Index - 1; $above -ge 0; $above--) {
        $text = $Lines[$above].Trim()
        if ($text -eq '') { continue }
        if (-not $text.StartsWith('#')) { break }
        $comments += $text
    }
    return , $comments
}

# The text of the call that starts on line $Index: that line and the lines it runs onto.
function Get-CallText {
    param([string[]]$Code, [int]$Index)
    $text = ''
    $depth = 0
    for ($next = $Index; $next -lt [Math]::Min($Code.Count, $Index + 12); $next++) {
        $line = $Code[$next]
        $text += ' ' + $line
        $depth += ([regex]::Matches($line, '[\[({]')).Count - ([regex]::Matches($line, '[\])}]')).Count
        $trimmed = $line.TrimEnd()
        if ($depth -le 0 -and -not ($trimmed.EndsWith('`') -or $trimmed.EndsWith(','))) { break }
    }
    return $text
}

$files = @()
foreach ($pattern in @('.github/workflows/*.yml', '.github/workflows/*.yaml', '.github/actions/*/action.yml',
        'scripts/*.ps1', 'scripts/*.psm1', 'scripts/*.py')) {
    $files += @(Get-ChildItem -Path (Join-Path $Root $pattern) -File -ErrorAction SilentlyContinue)
}
$self = @('check_ctest_error_mode.ps1', 'test_check_ctest_error_mode.ps1')
$problems = New-Object Collections.Generic.List[string]
$calls = 0
$workflowCalls = 0
$exempt = New-Object Collections.Generic.List[string]
foreach ($file in ($files | Sort-Object FullName -Unique)) {
    if ($self -contains $file.Name) { continue }
    $relative = $file.FullName.Substring($Root.Length + 1).Replace('\', '/')
    $kind = switch -regex ($file.Extension) { '^\.py$' { 'py' } '^\.ps(m)?1$' { 'ps1' } default { 'yml' } }
    $raw = @(Get-Content -LiteralPath $file.FullName)
    $code = Get-CodeLine -Path $file.FullName -Kind $kind
    for ($index = 0; $index -lt $code.Count; $index++) {
        $line = $code[$index]
        if (-not $line -or $line -cnotmatch 'ctest') { continue }
        $unquoted = [regex]::Replace($line, '"[^"]*"|''[^'']*''', '""')
        $isCall = $false
        foreach ($pattern in $commandPatterns) { if ($unquoted -cmatch $pattern) { $isCall = $true } }
        foreach ($pattern in $quotedPatterns) { if ($line -cmatch $pattern) { $isCall = $true } }
        if (-not $isCall) { continue }
        $calls++
        if ($relative.StartsWith('.github/')) { $workflowCalls++ }
        if ((Get-CallText -Code $code -Index $index) -cmatch $flagPattern) { continue }
        $where = "${relative}:$($index + 1)"
        $comments = (Get-CommentAbove -Lines $raw -Index $index) -join ' '
        if ($comments.Contains($exemptionMarker)) {
            $lacking = @($exemptionWords | Where-Object { -not $comments.Contains($_) })
            if ($relative -ne $exemptionFile) {
                $problems.Add("${where}: an exemption is allowed only in $exemptionFile's Debug + ASan test step")
            } elseif ($lacking.Count -gt 0) {
                $problems.Add("${where}: the exemption comment must say why and what guards the call instead; it lacks: $($lacking -join ', ')")
            } else {
                $exempt.Add($where)
            }
            continue
        }
        $problems.Add("${where}: ctest without --interactive-debug-mode 0, so a test that cannot load a DLL would wait on a Windows error box")
    }
}
if ($exempt.Count -gt 1) {
    $problems.Add("more than one exempt ctest call ($($exempt -join ', ')); only the nightly's Debug + ASan test step may be")
}
if ($workflowCalls -eq 0) {
    $problems.Add('no ctest call found in .github/: this check matched nothing, so it proves nothing')
}
if ($problems.Count -gt 0) {
    foreach ($problem in $problems) { [Console]::Error.WriteLine($problem) }
    exit 1
}
$exemptText = if ($exempt.Count -gt 0) { " (exempt, keeping its crash dumps: $($exempt -join ', '))" } else { '' }
Write-Host "$calls ctest calls pass --interactive-debug-mode 0$exemptText."
exit 0

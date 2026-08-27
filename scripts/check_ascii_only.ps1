<#
.SYNOPSIS
    Fails when a tracked text file contains a byte above 0x7F or a UTF-8 BOM.

.DESCRIPTION
    Repository sources and docs are plain 7-bit ASCII. The rule exists because non-ASCII
    text in this tree has repeatedly been silent damage rather than typography: an em dash
    that had been double-encoded through cp1252 sat in a comment for months, and a test
    named roundTrip_nonAsciiPassword was round-tripping mojibake instead of the multi-script
    password it claimed to exercise. Neither was visible in review, because a mangled
    codepoint renders as something that still looks like text.

    A BOM is rejected for the same reason it is invisible: it changes the first bytes of a
    file, so tools that read from offset zero (and diffs, and shell redirection) see a file
    that does not start where the text starts.

    When a glyph genuinely has to survive to a user -- a window title, a rendered report
    column -- keep the codepoint and spell it as a \u escape in the literal. The compiled
    output is unchanged and the source stays ASCII. CMakeLists.txt sets /utf-8, so a
    universal-character-name in a narrow literal is encoded as UTF-8 in the execution
    charset, which is what Qt's tr() expects.

    Two classes of path are excluded, and neither is a style exemption:

      Vendored third-party sources must stay byte-identical to upstream. Rewriting the
      copyright sign in someone else's license header, or an author's name, is not a
      formatting change -- it alters a notice the license requires be preserved.

      Captured certification evidence records what a live run actually produced. Editing a
      stored report after the fact edits the evidence, which is the one thing it exists to
      prevent.

    Anything not excluded and not a known binary extension is checked. That direction is
    deliberate: a new text file type is covered by default, and a new binary type fails
    loudly until it is named here, rather than slipping through an allowlist unnoticed.
#>

param(
    [string[]]$Files
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
Set-Location $ProjectRoot

# Byte-oriented formats: scanning them for "non-ASCII" is meaningless.
$BinaryExtensions = @(
    ".exe", ".dll", ".pdb", ".lib", ".obj", ".bin",
    ".png", ".ico", ".icns", ".jpg", ".jpeg", ".gif", ".bmp", ".svgz",
    ".pdf", ".crx", ".nupkg",
    ".zip", ".7z", ".gz", ".xz", ".tar",
    ".ttf", ".otf", ".woff", ".woff2",
    ".qm", ".mo"
)

# See the .DESCRIPTION block: upstream bytes, and evidence of live runs.
$ExcludedPrefixes = @(
    "tools/chocolatey/",
    "tools/uup/",
    "tools/iperf3/",
    "tools/smartmontools/",
    "artifacts/"
)

if (-not $Files -or $Files.Count -eq 0) {
    $Files = git ls-files
    # A whole-tree run that enumerates NOTHING must not report zero violations. If git fails
    # or returns an empty list, this gate would scan no bytes and print a clean pass -- the
    # same false-green shape the sibling PowerShell-syntax gate already refuses. Fail closed.
    if ($LASTEXITCODE -ne 0) {
        throw "git ls-files failed (exit $LASTEXITCODE); cannot enumerate tracked files."
    }
    if (-not $Files -or @($Files).Count -eq 0) {
        throw "git ls-files returned no tracked files; refusing to pass without scanning anything."
    }
}

$violations = @()

foreach ($file in $Files) {
    $normalized = $file -replace "\\", "/"

    $excluded = $false
    foreach ($prefix in $ExcludedPrefixes) {
        if ($normalized.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            $excluded = $true
            break
        }
    }
    if ($excluded) { continue }

    $extension = [System.IO.Path]::GetExtension($normalized)
    if ($BinaryExtensions -contains $extension.ToLowerInvariant()) { continue }

    if (-not (Test-Path -LiteralPath $normalized -PathType Leaf)) { continue }

    $bytes = [System.IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $normalized).Path)
    if ($bytes.Length -eq 0) { continue }

    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        $violations += [pscustomobject]@{
            File   = $normalized
            Line   = 1
            Column = 1
            Detail = "UTF-8 BOM"
        }
    }

    # Report the first offending byte on each line rather than every byte: a decorated
    # comment rule is hundreds of bytes and would bury the rest of the report.
    $line = 1
    $column = 1
    $reportedOnThisLine = $false
    foreach ($byte in $bytes) {
        if ($byte -eq 0x0A) {
            $line++
            $column = 1
            $reportedOnThisLine = $false
            continue
        }
        if ($byte -gt 0x7F -and -not $reportedOnThisLine) {
            $violations += [pscustomobject]@{
                File   = $normalized
                Line   = $line
                Column = $column
                Detail = ("byte 0x{0:X2}" -f $byte)
            }
            $reportedOnThisLine = $true
        }
        $column++
    }
}

if ($violations.Count -gt 0) {
    Write-Host ""
    Write-Host "Non-ASCII content is not allowed in tracked text files."
    Write-Host ""
    foreach ($violation in $violations) {
        Write-Host ("  {0}:{1}:{2}  {3}" -f $violation.File, $violation.Line, $violation.Column, $violation.Detail)
    }
    Write-Host ""
    Write-Host "Replace decoration with ASCII (-- for an em dash, -> for an arrow, <= for a"
    Write-Host "relational sign, | + - for box drawing). Where the glyph must reach a user,"
    Write-Host "keep the codepoint and write it as a \u escape inside the string literal."
    Write-Host ""
    Write-Error ("ASCII gate failed: {0} violation(s) in {1} file(s)." -f
        $violations.Count, ($violations | Select-Object -ExpandProperty File -Unique).Count)
    exit 1
}

Write-Host ("ASCII gate passed: {0} file(s) checked." -f $Files.Count)
exit 0

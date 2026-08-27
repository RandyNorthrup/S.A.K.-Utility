<#
.SYNOPSIS
    Local secret and machine-path scan for S.A.K. Utility.

.DESCRIPTION
    Fails on key-shaped literals and developer-machine paths in tracked files.
    If gitleaks or trufflehog are installed, also runs those tools.
#>

param(
    [switch]$SkipExternalTools
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
Set-Location $ProjectRoot

$WindowsUserPathRegex = "C:[\\/]+Users[\\/]+(?!Username\b|Public\b|Default\b|All Users\b)[^\\/\s""']+"

$Patterns = @(
    @{ Name = "OpenAI/API key"; Regex = "sk-(proj-)?[A-Za-z0-9_-]{20,}" },
    @{ Name = "Context7 API key"; Regex = "ctx7sk-[A-Za-z0-9-]{20,}" },
    @{ Name = "GitHub token"; Regex = "gh[pousr]_[A-Za-z0-9_]{30,}" },
    @{ Name = "AWS access key"; Regex = "(AKIA|ASIA)[0-9A-Z]{16}" },
    @{ Name = "Google API key"; Regex = "AIza[0-9A-Za-z_-]{35}" },
    @{ Name = "Slack token"; Regex = "xox[baprs]-[0-9A-Za-z-]{20,}" },
    @{ Name = "Stripe key"; Regex = "[rs]k_(live|test)_[0-9A-Za-z]{20,}" },
    @{ Name = "SendGrid key"; Regex = "SG\.[0-9A-Za-z_-]{20,}\.[0-9A-Za-z_-]{20,}" },
    @{ Name = "Private key block"; Regex = "-----BEGIN [A-Z ]*PRIVATE KEY-----" },
    @{ Name = "JWT"; Regex = "eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}" },
    @{ Name = "Bearer token"; Regex = "Bearer\s+[A-Za-z0-9._~+/-]{20,}" },
    @{ Name = "Developer user path"; Regex = $WindowsUserPathRegex }
)

$ExcludedPrefixes = @(
    "artifacts/",
    "build/",
    ".git/",
    "temp/",
    "_archived/"
)

function Convert-ToRepoPath {
    param([string]$Path)
    return ($Path -replace "\\", "/")
}

function Test-IsScannableFile {
    param([string]$Path)
    $repoPath = Convert-ToRepoPath $Path
    foreach ($prefix in $ExcludedPrefixes) {
        if ($repoPath.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            return $false
        }
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }
    $extension = [System.IO.Path]::GetExtension($Path).ToLowerInvariant()
    return $extension -notin @(".exe", ".dll", ".lib", ".pdb", ".png", ".jpg", ".jpeg", ".ico",
                               ".zip", ".7z", ".gz", ".bz2", ".xz", ".pdf")
}

# $ErrorActionPreference does not turn a native nonzero exit into a terminating error, so
# check it explicitly: a failed/wrong-directory git invocation must not yield an empty list
# that then reports "clean".
$trackedFiles = git ls-files -c -m -o --exclude-standard
if ($LASTEXITCODE -ne 0) {
    throw "git ls-files failed with exit code $LASTEXITCODE; cannot perform a complete secret scan."
}
$Files = $trackedFiles |
    ForEach-Object { Convert-ToRepoPath $_ } |
    Where-Object { Test-IsScannableFile $_ } |
    Sort-Object -Unique

$Findings = @()
foreach ($file in $Files) {
    # A tracked, scannable file that cannot be read must fail closed rather than be
    # silently skipped -- a suppressed read error could hide a secret.
    try {
        $text = Get-Content -LiteralPath $file -Raw -ErrorAction Stop
    }
    catch {
        throw "Secret scan cannot read tracked file '$file': $($_.Exception.Message)"
    }
    if ($null -eq $text) {
        # Genuinely empty file: nothing to scan.
        continue
    }
    foreach ($pattern in $Patterns) {
        if ($text -match $pattern.Regex) {
            $Findings += [pscustomobject]@{
                File = $file
                Rule = $pattern.Name
            }
        }
    }
}

if ($Findings.Count -gt 0) {
    Write-Host "Secret/path scan failed:" -ForegroundColor Red
    $Findings | Format-Table -AutoSize | Out-String | Write-Host
    exit 1
}

Write-Host "Regex secret/path scan clean." -ForegroundColor Green

if ($SkipExternalTools) {
    exit 0
}

$gitleaks = Get-Command gitleaks -CommandType Application -ErrorAction SilentlyContinue
if ($gitleaks) {
    & $gitleaks.Source detect --source . --config .gitleaks.toml --redact --verbose
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} else {
    # Fail closed (R5-G21-4): the external scan was requested (no -SkipExternalTools), so a
    # missing gitleaks is a gate that cannot run, not a pass. Install gitleaks, or pass
    # -SkipExternalTools (as pre-commit and CI do) to run only the regex/path scan.
    Write-Error "gitleaks is required for the external secret scan but was not found on PATH. Install gitleaks, or pass -SkipExternalTools to run only the regex/path scan."
    exit 1
}

$trufflehog = Get-Command trufflehog -CommandType Application -ErrorAction SilentlyContinue
if ($trufflehog) {
    # "file://." and NOT "file://$ProjectRoot". trufflehog mangles a Windows ABSOLUTE path in
    # every other form: backslashes get percent-encoded into an ssh:// URI with a bogus
    # hostname, and forward slashes get the drive letter prepended a second time. Both fail to
    # clone, so this arm of the gate could never run on this platform. It fails CLOSED --
    # blocking the push rather than skipping the scan -- which is why nothing surfaced it until
    # a push was first attempted. The script has already Set-Location'd to $ProjectRoot, so the
    # relative form names the same repository.
    & $trufflehog.Source git "file://." --only-verified --fail --no-update
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} else {
    # Fail closed (R5-G21-4): same rule as gitleaks above -- a requested external scan whose
    # tool is absent must fail loudly, never silently skip.
    Write-Error "trufflehog is required for the external secret scan but was not found on PATH. Install trufflehog, or pass -SkipExternalTools to run only the regex/path scan."
    exit 1
}

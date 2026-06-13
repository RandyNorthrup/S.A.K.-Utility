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

$Files = git ls-files -c -m -o --exclude-standard |
    ForEach-Object { Convert-ToRepoPath $_ } |
    Where-Object { Test-IsScannableFile $_ } |
    Sort-Object -Unique

$Findings = @()
foreach ($file in $Files) {
    $text = Get-Content -LiteralPath $file -Raw -ErrorAction SilentlyContinue
    if ($null -eq $text) {
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

$gitleaks = Get-Command gitleaks -ErrorAction SilentlyContinue
if ($gitleaks) {
    & $gitleaks.Source detect --source . --config .gitleaks.toml --redact --verbose
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} else {
    Write-Host "gitleaks not installed; skipped external gitleaks scan." -ForegroundColor Yellow
}

$trufflehog = Get-Command trufflehog -ErrorAction SilentlyContinue
if ($trufflehog) {
    & $trufflehog.Source git "file://$ProjectRoot" --only-verified --fail --no-update
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} else {
    Write-Host "trufflehog not installed; skipped external TruffleHog scan." -ForegroundColor Yellow
}

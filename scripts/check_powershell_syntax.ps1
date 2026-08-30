<#
.SYNOPSIS
    Parses PowerShell scripts and fails on syntax errors.

.DESCRIPTION
    This is intentionally syntax-only. It catches broken scripts quickly in
    pre-commit without executing installer, bundle, signing, or release logic.
#>

# PositionalBinding=$false + ValueFromRemainingArguments. With a plain [string[]]$Files this
# script received ONLY the first path pre-commit handed it and silently ignored the rest --
# proven here directly: a file with `function Broken( {` in second position exited 0 with
# "PowerShell syntax check passed." See the note in scripts/run_cppcheck.ps1 for the
# measurement behind this.
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Files
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
Set-Location $ProjectRoot

if (-not $Files -or $Files.Count -eq 0) {
    $Files = git ls-files "*.ps1"
    if ($LASTEXITCODE -ne 0) {
        throw "git ls-files failed (exit $LASTEXITCODE); cannot enumerate PowerShell scripts."
    }
    if (-not $Files -or @($Files).Count -eq 0) {
        throw "git ls-files returned no .ps1 files; refusing to pass without scanning anything."
    }
}

$failures = @()
foreach ($file in $Files) {
    if (-not $file.EndsWith(".ps1", [System.StringComparison]::OrdinalIgnoreCase)) {
        continue
    }
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        continue
    }

    $tokens = $null
    $errors = $null
    [System.Management.Automation.Language.Parser]::ParseFile(
        (Resolve-Path -LiteralPath $file).Path,
        [ref]$tokens,
        [ref]$errors
    ) | Out-Null

    foreach ($errorRecord in $errors) {
        $failures += [pscustomobject]@{
            File = $file
            Line = $errorRecord.Extent.StartLineNumber
            Column = $errorRecord.Extent.StartColumnNumber
            Message = $errorRecord.Message
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Host "PowerShell syntax check failed:" -ForegroundColor Red
    $failures | Format-Table -AutoSize | Out-String | Write-Host
    exit 1
}

Write-Host "PowerShell syntax check passed." -ForegroundColor Green

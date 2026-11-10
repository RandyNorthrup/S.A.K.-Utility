<#
.SYNOPSIS
    Fails when GUI code uses direct static QMessageBox dialogs instead of logged wrappers.
#>

param(
    [string]$Root = "."
)

$ErrorActionPreference = "Stop"

$repo = (Resolve-Path -LiteralPath $Root).Path
# Pin repository identity: a wrong -Root (or the "." default run from the wrong
# directory) must not scan an unrelated tree, find nothing, and pass falsely.
if (-not (Test-Path -LiteralPath (Join-Path $repo "src") -PathType Container) -or
    -not (Test-Path -LiteralPath (Join-Path $repo "include") -PathType Container)) {
    throw "check_logged_message_boxes: '$repo' is not a repo root (missing src/ or include/)."
}
Push-Location $repo
try {
    # --no-config / --no-ignore so a stray RIPGREP_CONFIG_PATH or ignore file cannot
    # suppress a match and turn a real violation into a false PASS. \s* tolerates
    # whitespace around :: so "QMessageBox :: warning" cannot slip through.
    # Pin the native rg (R5-G21-4): an alias/function named rg could emit nothing and leave a
    # stale exit code, passing the gate without searching.
    $rg = @(Get-Command rg -CommandType Application -ErrorAction SilentlyContinue)[0]
    if (-not $rg) {
        throw "Required tool missing: rg (native executable)"
    }
    $hits = & $rg.Source -n --no-config --no-ignore "QMessageBox\s*::\s*(warning|critical|information|question)" src include
    $violations = @($hits | Where-Object {
            $_ -notmatch '^include[\\/]+sak[\\/]+message_box_helpers\.h:'
        })
    if ($LASTEXITCODE -eq 0 -and $violations.Count -gt 0) {
        Write-Error "Direct static QMessageBox call found. Use sak::show*Logged helpers:`n$($violations -join "`n")"
        exit 1
    }
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 1) {
        throw "rg failed while scanning QMessageBox calls"
    }

    Write-Host "Logged message-box check passed."
}
finally {
    Pop-Location
}

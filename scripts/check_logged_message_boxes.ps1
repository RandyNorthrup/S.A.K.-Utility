<#
.SYNOPSIS
    Fails when GUI code uses direct static QMessageBox dialogs instead of logged wrappers.
#>

param(
    [string]$Root = "."
)

$ErrorActionPreference = "Stop"

$repo = (Resolve-Path -LiteralPath $Root).Path
Push-Location $repo
try {
    $matches = & rg -n "QMessageBox::(warning|critical|information|question)" src include
    $violations = @($matches | Where-Object {
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

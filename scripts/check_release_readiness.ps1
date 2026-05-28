<#
.SYNOPSIS
    Runs aggregate release-readiness checks for S.A.K. Utility.
#>

param(
    [string]$PackageRoot = "",
    [switch]$RequireSignedPackage
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)

function Invoke-IsolatedPowerShellScript {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ScriptPath
    )

    $hostPath = (Get-Process -Id $PID).Path
    & $hostPath -NoProfile -ExecutionPolicy Bypass -File $ScriptPath
    if ($LASTEXITCODE -ne 0) {
        throw "Release readiness check failed: $ScriptPath"
    }
}

Push-Location $ProjectRoot
try {
    $version = (Get-Content -LiteralPath "VERSION" -Raw).Trim()
    $cmake = Get-Content -LiteralPath "CMakeLists.txt" -Raw
    if ($cmake -notmatch "project\(SAK_Utility\s+VERSION\s+$([regex]::Escape($version))\b") {
        throw "VERSION ($version) does not match CMake project version"
    }

    foreach ($requiredScript in @(
            "scripts/scan_secrets.ps1",
            "scripts/check_blocking_patterns.ps1",
            "scripts/check_gui_style_tokens.ps1",
            "scripts/check_gui_stylesheet_literals.ps1",
            "scripts/check_gui_magic_numbers.ps1",
            "scripts/check_magic_numbers.py",
            "scripts/check_accessibility_patterns.ps1",
            "scripts/check_logged_message_boxes.ps1",
            "scripts/run_lizard.py",
            "scripts/check_third_party_licenses.ps1",
            "scripts/verify_portable_release_smoke.ps1",
            "scripts/run_portable_e2e_smoke.ps1",
            "scripts/stage_portable_release.ps1",
            "scripts/create_signing_catalog.ps1",
            "scripts/verify_authenticode_signatures.ps1",
            "scripts/create_release_archive.ps1")) {
        if (-not (Test-Path -LiteralPath $requiredScript -PathType Leaf)) {
            throw "Required release script missing: $requiredScript"
        }
    }

    & scripts/scan_secrets.ps1 -SkipExternalTools
    & scripts/check_blocking_patterns.ps1
    Invoke-IsolatedPowerShellScript -ScriptPath (Join-Path $ProjectRoot "scripts/check_accessibility_patterns.ps1")
    & scripts/check_gui_style_tokens.ps1
    & scripts/check_gui_stylesheet_literals.ps1
    & scripts/check_gui_magic_numbers.ps1
    python scripts/check_magic_numbers.py
    if ($LASTEXITCODE -ne 0) {
        throw "Release readiness check failed: scripts/check_magic_numbers.py"
    }
    & scripts/check_logged_message_boxes.ps1
    python scripts/run_lizard.py
    if ($LASTEXITCODE -ne 0) {
        throw "Release readiness check failed: scripts/run_lizard.py"
    }
    & scripts/check_third_party_licenses.ps1
    & scripts/check_qrc_resources.ps1

    if (-not [string]::IsNullOrWhiteSpace($PackageRoot)) {
        $package = Resolve-Path -LiteralPath $PackageRoot
        & scripts/verify_portable_release_smoke.ps1 -PackageRoot $package.Path -RepoRoot $ProjectRoot
        & scripts/run_portable_e2e_smoke.ps1 -PackageRoot $package.Path
        if ($RequireSignedPackage) {
            & scripts/verify_authenticode_signatures.ps1 -RootDir $package.Path
        }
    }

    Write-Host "Release readiness checks passed."
}
finally {
    Pop-Location
}

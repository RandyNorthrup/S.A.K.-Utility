<#
.SYNOPSIS
    Verifies third-party license documentation and bundled MCP runtime licenses.

.DESCRIPTION
    This is a release gate. It fails when required third-party documentation is
    missing, when remote MCP providers imply an embedded key, or when the
    win32-mcp-server runtime dependency inventory contains forbidden reciprocal
    or non-commercial licenses.
#>

param(
    [string]$Root = "."
)

$ErrorActionPreference = "Stop"

$repo = (Resolve-Path -LiteralPath $Root).Path
Push-Location $repo
try {
    $licensePath = "THIRD_PARTY_LICENSES.md"
    if (-not (Test-Path -LiteralPath $licensePath -PathType Leaf)) {
        throw "Missing $licensePath"
    }

    $licenseText = Get-Content -LiteralPath $licensePath -Raw
    $requiredSections = @(
        "Qt 6.5+",
        "qrcodegen",
        "win32-mcp-server",
        "Context7 MCP",
        "Microsoft Learn MCP",
        "aria2",
        "UUPMediaCreator",
        "wimlib / libwim",
        "zlib",
        "bzip2",
        "XZ Utils (liblzma)",
        "Chocolatey",
        "smartmontools",
        "iPerf3",
        "Icons8"
    )

    $missingSections = @()
    foreach ($section in $requiredSections) {
        $pattern = "(?m)^##\s+$([regex]::Escape($section))\s*$"
        if ($licenseText -notmatch $pattern) {
            $missingSections += $section
        }
    }
    if ($missingSections.Count -gt 0) {
        throw "Missing third-party license sections: $($missingSections -join ', ')"
    }

    foreach ($requiredPhrase in @(
            "read a Context7 API key",
            "no authentication is",
            "complete source code for aria2",
            "complete source code for smartmontools",
            "Attribution is provided")) {
        if (-not $licenseText.Contains($requiredPhrase)) {
            throw "License documentation missing required compliance phrase: $requiredPhrase"
        }
    }

    $providerFiles = @(
        "resources/ai/providers/providers.json",
        "data/ai/providers/providers.json"
    ) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }
    foreach ($providerFile in $providerFiles) {
        $providerText = Get-Content -LiteralPath $providerFile -Raw
        if ($providerText -match "ctx7sk-[A-Za-z0-9-]{20,}") {
            throw "Provider manifest contains a Context7 API key: $providerFile"
        }
        if ($providerText -match "sk-(proj-)?[A-Za-z0-9_-]{20,}") {
            throw "Provider manifest contains an OpenAI-shaped API key: $providerFile"
        }
    }

    $mcpInventory = "tools/mcp/win32-mcp-server/THIRD_PARTY_LICENSES.txt"
    if (Test-Path -LiteralPath "tools/mcp/win32-mcp-server/win32-mcp-server.exe" -PathType Leaf) {
        if (-not (Test-Path -LiteralPath $mcpInventory -PathType Leaf)) {
            throw "Bundled win32 MCP executable is missing runtime license inventory: $mcpInventory"
        }
    }

    if (Test-Path -LiteralPath $mcpInventory -PathType Leaf) {
        $forbiddenPattern = "(?i)\b(AGPL|LGPL|SSPL|Commons Clause|Non[- ]?Commercial|Proprietary)\b|(?<!MPL-)GPL"
        $forbiddenRows = @()
        foreach ($line in Get-Content -LiteralPath $mcpInventory) {
            if ($line -notmatch "^\S+\t\S+\t") {
                continue
            }
            $parts = $line -split "`t", 3
            if ($parts.Count -lt 3) {
                continue
            }
            $packageName = $parts[0]
            $license = $parts[2]
            if ($packageName -eq "Package" -or [string]::IsNullOrWhiteSpace($license)) {
                continue
            }
            if ($license -match $forbiddenPattern) {
                $forbiddenRows += "$packageName`t$license"
            }
        }
        if ($forbiddenRows.Count -gt 0) {
            throw "Forbidden win32 MCP runtime licenses found:`n$($forbiddenRows -join "`n")"
        }
    }

    Write-Host "Third-party license audit passed."
}
finally {
    Pop-Location
}

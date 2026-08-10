<#
.SYNOPSIS
    Verifies third-party license documentation and bundled MCP runtime licenses.

.DESCRIPTION
    This is a release gate. It fails when required third-party documentation is
    missing, when remote MCP providers imply an embedded key, or when an approved
    filesystem tool is undocumented.
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
        "Context7 MCP",
        "Microsoft Learn MCP",
        "Partition filesystem tools",
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
    if (@($providerFiles).Count -eq 0) {
        throw "No AI provider manifest found to scan for embedded API keys (expected resources/ai/providers/providers.json or data/ai/providers/providers.json)"
    }
    foreach ($providerFile in $providerFiles) {
        $providerText = Get-Content -LiteralPath $providerFile -Raw
        if ($providerText -match "ctx7sk-[A-Za-z0-9-]{20,}") {
            throw "Provider manifest contains a Context7 API key: $providerFile"
        }
        if ($providerText -match "sk-(proj-)?[A-Za-z0-9_-]{20,}") {
            throw "Provider manifest contains an OpenAI-shaped API key: $providerFile"
        }
    }

    $filesystemManifestPath = "tools/filesystem/manifest.json"
    if (-not (Test-Path -LiteralPath $filesystemManifestPath -PathType Leaf)) {
        throw "Missing bundled filesystem tool manifest: $filesystemManifestPath"
    }
    $filesystemManifest = Get-Content -LiteralPath $filesystemManifestPath -Raw | ConvertFrom-Json
    foreach ($tool in @($filesystemManifest.tools)) {
        $toolId = [string]$tool.id
        $displayName = [string]$tool.display_name
        $upstreamUrl = [string]$tool.upstream_url
        $license = [string]$tool.license
        # An approved tool with a blank required field must fail closed rather than be
        # silently skipped -- {}, {"tools":[{}]}, and null fields would otherwise pass.
        foreach ($field in @(
                @{ Name = "id"; Value = $toolId },
                @{ Name = "display_name"; Value = $displayName },
                @{ Name = "upstream_url"; Value = $upstreamUrl },
                @{ Name = "license"; Value = $license })) {
            if ([string]::IsNullOrWhiteSpace($field.Value)) {
                throw "Filesystem tool in $filesystemManifestPath has a blank required field '$($field.Name)'"
            }
        }
        foreach ($requiredPhrase in @($toolId, $displayName, $upstreamUrl, $license, "tools/filesystem")) {
            if (-not $licenseText.Contains($requiredPhrase)) {
                throw "Filesystem tool '$toolId' is approved in $filesystemManifestPath but missing from THIRD_PARTY_LICENSES.md: $requiredPhrase"
            }
        }
    }

    Write-Host "Third-party license audit passed."
}
finally {
    Pop-Location
}

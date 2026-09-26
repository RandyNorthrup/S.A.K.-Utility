<#
.SYNOPSIS
    Bundles the vendor AI agent runtimes used by the AI Assistant's
    subscription sign-in modes.

.DESCRIPTION
    Downloads pinned, unmodified vendor releases into tools/ai_agents/ so the
    portable build needs nothing installed on the customer PC:

      tools/ai_agents/codex/   OpenAI Codex app-server package (Apache-2.0)
      tools/ai_agents/claude/  Claude Code native executable (Anthropic terms)
      tools/ai_agents/gemini/  Gemini CLI bundle + private Node.js runtime
                               (only with -IncludeGemini)

    Every download is verified against the vendor's own published checksum
    (GitHub release digest, Claude Code release manifest, Node.js
    SHASUMS256.txt, npm integrity) and, for executables, a valid Authenticode
    signature from the expected publisher. A manifest with the SHA-256 of every
    bundled file is written to tools/ai_agents/manifest.json.

    Muse Code is NOT bundled: no redistribution grant has been published for
    the muse binary. S.A.K. uses a copy the technician installs with Meta's
    official installer instead.

    Binaries are never modified; Claude Code in particular must be shipped
    exactly as published (Anthropic legal-and-compliance conditions).

.PARAMETER Force
    Re-download runtimes that are already present.
#>

param(
    [string]$DestinationRoot = "tools/ai_agents",
    [string]$CodexVersion = "0.157.0",
    [string]$ClaudeCodeVersion = "2.1.274",
    [switch]$IncludeGemini,
    [string]$GeminiCliVersion = "0.61.0",
    [string]$NodeVersion = "24.21.0",
    [switch]$SkipSignatureCheck,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$dest = [System.IO.Path]::GetFullPath((Join-Path $root $DestinationRoot))
$temp = Join-Path ([System.IO.Path]::GetTempPath()) "sak_bundle_ai_agents"
New-Item -ItemType Directory -Force -Path $dest, $temp | Out-Null

$manifestEntries = @()

function Get-Download {
    param([string]$Uri, [string]$OutFile)
    Write-Host "  Downloading $Uri"
    Invoke-WebRequest -Uri $Uri -OutFile $OutFile -UseBasicParsing
}

function Assert-Hash {
    param([string]$Path, [string]$Algorithm, [string]$Expected)
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm $Algorithm).Hash.ToLowerInvariant()
    if ($actual -ne $Expected.ToLowerInvariant()) {
        throw "$Algorithm mismatch for $Path`n expected $Expected`n actual   $actual"
    }
    Write-Host "  Verified $Algorithm $([System.IO.Path]::GetFileName($Path))"
}

function Assert-Publisher {
    param([string]$Path, [string]$Publisher)
    if ($SkipSignatureCheck) {
        Write-Warning "Signature check skipped for $Path"
        return
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne "Valid" -or $signature.SignerCertificate.Subject -notmatch [regex]::Escape($Publisher)) {
        throw "Unexpected Authenticode signature on $Path ($($signature.Status); $($signature.SignerCertificate.Subject))"
    }
}

function Add-ManifestEntry {
    param([string]$Id, [string]$Version, [string]$Source, [string]$Directory)
    $files = Get-ChildItem -LiteralPath $Directory -Recurse -File | Sort-Object FullName | ForEach-Object {
        [ordered]@{
            path   = $_.FullName.Substring($dest.Length + 1).Replace("\", "/")
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    $script:manifestEntries += [ordered]@{
        id      = $Id
        version = $Version
        source  = $Source
        files   = @($files)
    }
}

function Test-Present {
    param([string]$Path)
    return (-not $Force) -and (Test-Path -LiteralPath $Path -PathType Leaf)
}

function Install-Codex {
    $target = Join-Path $dest "codex"
    $tag = "rust-v$CodexVersion"
    $asset = "codex-app-server-package-x86_64-pc-windows-msvc.tar.gz"
    if (-not (Test-Present (Join-Path $target "bin/codex-app-server.exe"))) {
        $headers = @{ "User-Agent" = "sak-utility-build" }
        if ($env:GITHUB_TOKEN) { $headers["Authorization"] = "Bearer $env:GITHUB_TOKEN" }
        $release = Invoke-RestMethod -Uri "https://api.github.com/repos/openai/codex/releases/tags/$tag" -Headers $headers -UseBasicParsing
        $entry = $release.assets | Where-Object { $_.name -eq $asset } | Select-Object -First 1
        if (-not $entry -or -not $entry.digest) {
            throw "Codex release $tag has no published digest for $asset"
        }
        $archive = Join-Path $temp $asset
        Get-Download -Uri $entry.browser_download_url -OutFile $archive
        Assert-Hash -Path $archive -Algorithm SHA256 -Expected ($entry.digest -replace "^sha256:", "")
        Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force -Path $target | Out-Null
        & tar -xzf $archive -C $target
        if ($LASTEXITCODE -ne 0) { throw "Failed to extract $asset" }
        foreach ($notice in @("LICENSE", "NOTICE")) {
            Get-Download -Uri "https://raw.githubusercontent.com/openai/codex/$tag/$notice" -OutFile (Join-Path $target $notice)
        }
    }
    $server = Join-Path $target "bin/codex-app-server.exe"
    if (-not (Test-Path -LiteralPath $server)) {
        throw "Codex package layout changed: $server not found"
    }
    Assert-Publisher -Path $server -Publisher "OpenAI"
    Add-ManifestEntry -Id "codex" -Version $CodexVersion -Source "https://github.com/openai/codex/releases/tag/$tag" -Directory $target
}

function Install-ClaudeCode {
    $target = Join-Path $dest "claude"
    $exe = Join-Path $target "claude.exe"
    $base = "https://downloads.claude.ai/claude-code-releases/$ClaudeCodeVersion"
    if (-not (Test-Present $exe)) {
        $manifest = Invoke-RestMethod -Uri "$base/manifest.json" -UseBasicParsing
        $platform = $manifest.platforms.'win32-x64'
        if (-not $platform -or -not $platform.checksum) {
            throw "Claude Code $ClaudeCodeVersion manifest has no win32-x64 checksum"
        }
        New-Item -ItemType Directory -Force -Path $target | Out-Null
        $download = Join-Path $temp "claude.exe"
        Get-Download -Uri "$base/win32-x64/claude.exe" -OutFile $download
        Assert-Hash -Path $download -Algorithm SHA256 -Expected $platform.checksum
        Move-Item -LiteralPath $download -Destination $exe -Force
        @(
            "Claude Code $ClaudeCodeVersion (unmodified native executable)",
            "(c) Anthropic PBC. All rights reserved.",
            "Use is subject to Anthropic's Commercial Terms and the conditions at",
            "https://code.claude.com/docs/en/legal-and-compliance",
            "Users sign in with their own Claude plan or API key through Claude Code itself."
        ) | Set-Content -LiteralPath (Join-Path $target "LICENSE.txt") -Encoding UTF8
    }
    Assert-Publisher -Path $exe -Publisher "Anthropic"
    Add-ManifestEntry -Id "claude" -Version $ClaudeCodeVersion -Source $base -Directory $target
}

function Install-Node {
    param([string]$Target)
    $node = Join-Path $Target "node.exe"
    if (Test-Present $node) { return }
    $name = "node-v$NodeVersion-win-x64"
    $sums = Invoke-RestMethod -Uri "https://nodejs.org/dist/v$NodeVersion/SHASUMS256.txt" -UseBasicParsing
    $line = ($sums -split "`n") | Where-Object { $_ -match "\s$([regex]::Escape($name)).zip$" } | Select-Object -First 1
    if (-not $line) { throw "No SHASUMS256 entry for $name.zip" }
    $archive = Join-Path $temp "$name.zip"
    Get-Download -Uri "https://nodejs.org/dist/v$NodeVersion/$name.zip" -OutFile $archive
    Assert-Hash -Path $archive -Algorithm SHA256 -Expected ($line -split "\s+")[0]
    $unpacked = Join-Path $temp $name
    Remove-Item -LiteralPath $unpacked -Recurse -Force -ErrorAction SilentlyContinue
    Expand-Archive -LiteralPath $archive -DestinationPath $temp -Force
    New-Item -ItemType Directory -Force -Path $Target | Out-Null
    Copy-Item -LiteralPath (Join-Path $unpacked "node.exe") -Destination $node -Force
    Copy-Item -LiteralPath (Join-Path $unpacked "LICENSE") -Destination (Join-Path $Target "LICENSE") -Force
}

function Install-GeminiCli {
    $target = Join-Path $dest "gemini"
    Install-Node -Target (Join-Path $target "node")
    Assert-Publisher -Path (Join-Path $target "node/node.exe") -Publisher "OpenJS Foundation"
    if (-not (Test-Present (Join-Path $target "bundle/gemini.js"))) {
        $meta = Invoke-RestMethod -Uri "https://registry.npmjs.org/@google/gemini-cli/$GeminiCliVersion" -UseBasicParsing
        $integrity = $meta.dist.integrity
        if ($integrity -notmatch "^sha512-") { throw "Gemini CLI $GeminiCliVersion has no sha512 integrity" }
        $archive = Join-Path $temp "gemini-cli-$GeminiCliVersion.tgz"
        Get-Download -Uri $meta.dist.tarball -OutFile $archive
        $expected = -join ([Convert]::FromBase64String($integrity.Substring(7)) | ForEach-Object { $_.ToString("x2") })
        Assert-Hash -Path $archive -Algorithm SHA512 -Expected $expected
        $unpacked = Join-Path $temp "gemini-cli"
        Remove-Item -LiteralPath $unpacked -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force -Path $unpacked | Out-Null
        & tar -xzf $archive -C $unpacked
        if ($LASTEXITCODE -ne 0) { throw "Failed to extract Gemini CLI" }
        Remove-Item -LiteralPath (Join-Path $target "bundle") -Recurse -Force -ErrorAction SilentlyContinue
        Copy-Item -LiteralPath (Join-Path $unpacked "package/bundle") -Destination (Join-Path $target "bundle") -Recurse -Force
        Copy-Item -LiteralPath (Join-Path $unpacked "package/LICENSE") -Destination (Join-Path $target "LICENSE") -Force
    }
    Add-ManifestEntry -Id "gemini" -Version "$GeminiCliVersion (node $NodeVersion)" -Source "https://www.npmjs.com/package/@google/gemini-cli" -Directory $target
}

Write-Host "=== AI agent runtimes ===" -ForegroundColor Cyan
Write-Host "Destination: $dest"
Install-Codex
Install-ClaudeCode
if ($IncludeGemini) {
    Install-GeminiCli
} else {
    Remove-Item -LiteralPath (Join-Path $dest "gemini") -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "  Gemini CLI skipped (pass -IncludeGemini to bundle it with a private Node.js)"
}

[ordered]@{
    generated_utc = (Get-Date).ToUniversalTime().ToString("o")
    note          = "Unmodified vendor runtimes. Muse Code is not bundled (no redistribution grant)."
    agents        = @($manifestEntries)
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $dest "manifest.json") -Encoding UTF8

Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "AI agent runtimes bundled." -ForegroundColor Green

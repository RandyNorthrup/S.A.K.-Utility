<#
.SYNOPSIS
    Re-pins the PACKAGE's filesystem tool manifest to the bytes release signing produced.

.DESCRIPTION
    tools/filesystem/manifest.json pins the SHA-256 of every bundled filesystem tool (e2fsprogs,
    hfsprogs and their runtime files), and the application refuses a tool whose bytes differ.
    Release signing adds an Authenticode signature to every unsigned .exe and .dll it ships, which
    changes those bytes. This rewrites the packaged copy of the manifest, never the repository's,
    so it names the signed files, and writes the mapping from each upstream hash to its signed hash
    for the release's assets. The repository manifest keeps the upstream hashes as provenance.

    Fails closed. The packaged manifest must still be byte-identical to the repository's. A file
    whose bytes changed must be a .exe or .dll with a Valid Authenticode signature whose signer is
    exactly the release publisher; any other change stops the release. Only hash values change in
    the manifest, and every rewritten entry is read back against the file it names.

    Release (T3) only: .github/workflows/release.yml runs it between signing and the check that
    asks the application's own validation about the package.
#>

[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackageRoot,

    [Parameter(Mandatory = $true)]
    [string]$RepositoryManifest,

    [Parameter(Mandatory = $true)]
    [string]$MappingPath,

    [Parameter(Mandatory = $true)]
    [string]$Version,

    [string]$ExpectedPublisher = "CN=Randy Northrup, O=Randy Northrup"
)

$ErrorActionPreference = "Stop"

$packageRootPath = (Resolve-Path -LiteralPath $PackageRoot).Path
$packageManifest = Join-Path $packageRootPath "tools\filesystem\manifest.json"
$toolsRoot = Split-Path -Parent $packageManifest
$repositoryManifestPath = (Resolve-Path -LiteralPath $RepositoryManifest).Path

$repositoryHash = (Get-FileHash -LiteralPath $repositoryManifestPath -Algorithm SHA256).Hash
$packagedHash = (Get-FileHash -LiteralPath $packageManifest -Algorithm SHA256).Hash
if ($repositoryHash -cne $packagedHash) {
    throw "The packaged filesystem tool manifest is not the repository's ($packagedHash, expected $repositoryHash); refusing to re-pin it."
}

$manifest = [IO.File]::ReadAllText($packageManifest) | ConvertFrom-Json

# Every pinned file once, by path. A file two tools share (hfsprogs' msys-2.0.dll) must carry one pin.
$pins = [ordered]@{}
foreach ($tool in @($manifest.tools)) {
    $entries = @([pscustomobject]@{ Path = [string]$tool.relative_path; Sha256 = [string]$tool.binary_sha256 })
    foreach ($runtimeFile in @($tool.runtime_files | Where-Object { $null -ne $_ })) {
        $entries += [pscustomobject]@{ Path = [string]$runtimeFile.relative_path; Sha256 = [string]$runtimeFile.sha256 }
    }
    foreach ($entry in $entries) {
        if ([string]::IsNullOrWhiteSpace($entry.Path) -or $entry.Sha256 -notmatch '^[0-9a-fA-F]{64}$') {
            throw "Tool '$($tool.id)' has an entry without a path or a SHA-256: '$($entry.Path)'"
        }
        $key = $entry.Path.Replace('\', '/').ToLowerInvariant()
        if ($pins.Contains($key)) {
            if ($pins[$key].Sha256 -ne $entry.Sha256.ToLowerInvariant()) {
                throw "Two pins disagree for $($entry.Path)"
            }
            $pins[$key].References++
            continue
        }
        $pins[$key] = [pscustomobject]@{
            Path       = $entry.Path
            Sha256     = $entry.Sha256.ToLowerInvariant()
            References = 1
        }
    }
}
if ($pins.Count -eq 0) {
    throw "The packaged filesystem tool manifest pins no files: $packageManifest"
}

$changed = @()
foreach ($pin in $pins.Values) {
    $file = Join-Path $toolsRoot $pin.Path
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Pinned filesystem tool file is missing from the package: $($pin.Path)"
    }
    $actual = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -eq $pin.Sha256) {
        continue
    }
    $extension = [IO.Path]::GetExtension($file).ToLowerInvariant()
    if ($extension -notin ".exe", ".dll") {
        throw "A pinned filesystem tool file changed and it is not a signed binary: $($pin.Path)"
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $file
    $signer = if ($signature.SignerCertificate) { $signature.SignerCertificate.Subject } else { "" }
    if ($signature.Status -ne "Valid" -or $signer -cne $ExpectedPublisher) {
        throw "A pinned filesystem tool binary changed without a valid release signature: $($pin.Path) (status $($signature.Status), signer '$signer')"
    }
    $changed += [pscustomobject]@{
        Path       = $pin.Path
        Upstream   = $pin.Sha256
        Signed     = $actual
        References = $pin.References
    }
}

$text = [IO.File]::ReadAllText($packageManifest)
foreach ($entry in $changed) {
    # A hash names one file's bytes, so it appears exactly once per manifest entry for that file.
    $occurrences = ([regex]::Matches($text, [regex]::Escape($entry.Upstream), 'IgnoreCase')).Count
    if ($occurrences -ne $entry.References) {
        throw "Expected $($entry.References) pin(s) of $($entry.Path) in the manifest, found $occurrences"
    }
    $text = [regex]::Replace($text, [regex]::Escape($entry.Upstream), $entry.Signed, 'IgnoreCase')
}
if ($changed.Count -gt 0) {
    [IO.File]::WriteAllText($packageManifest, $text, [Text.UTF8Encoding]::new($false))
}

# Read the rewritten manifest back: every pin must now name the bytes in the package.
$rewritten = [IO.File]::ReadAllText($packageManifest) | ConvertFrom-Json
foreach ($tool in @($rewritten.tools)) {
    $checks = @(@{ Path = [string]$tool.relative_path; Sha256 = [string]$tool.binary_sha256 })
    foreach ($runtimeFile in @($tool.runtime_files | Where-Object { $null -ne $_ })) {
        $checks += @{ Path = [string]$runtimeFile.relative_path; Sha256 = [string]$runtimeFile.sha256 }
    }
    foreach ($check in $checks) {
        $actual = (Get-FileHash -LiteralPath (Join-Path $toolsRoot $check.Path) -Algorithm SHA256).Hash
        if ($actual -ne $check.Sha256) {
            throw "Re-pinned manifest does not match the package for tool '$($tool.id)': $($check.Path)"
        }
    }
}

$lines = @(
    "S.A.K. Utility v${Version}: bundled filesystem tools, as pinned upstream and as shipped signed.",
    "",
    "Release signing signs every unsigned .exe and .dll in the package, which changes their bytes.",
    "tools/filesystem/manifest.json in the repository keeps the upstream hashes below; the packaged",
    "manifest carries the signed ones, because the application checks each tool against it.",
    "",
    "relative_path upstream_sha256 signed_sha256"
)
foreach ($entry in $changed) {
    $lines += "$($entry.Path) $($entry.Upstream) $($entry.Signed)"
}
if ($changed.Count -eq 0) {
    $lines += "(none: no pinned filesystem tool file changed when the package was signed)"
}
[IO.File]::WriteAllLines($MappingPath, [string[]]$lines, [Text.UTF8Encoding]::new($false))

Write-Host "Re-pinned $($changed.Count) signed filesystem tool file(s) in $packageManifest"
foreach ($entry in $changed) {
    Write-Host "  $($entry.Path): $($entry.Upstream) -> $($entry.Signed)"
}
Write-Host "Mapping written: $MappingPath"

# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Verifies every bundled Files-community icon against the upstream asset it
# claims to be derived from.
#
# The plan's icon import rules require that XAML vector path data reach Qt
# resources "only through a repeatable script or documented manual mapping; no
# untracked hand-copied path blobs". resources/icons/files/manifest.json records
# the mapping (upstream repository, commit, source XAML file, and resource key);
# this script is the repeatable half: it re-reads the upstream XAML and compares
# each Setter's OutlineIconData against the "d" attribute of the SVG S.A.K.
# ships.
#
# The upstream clone is NOT vendored into this repository, so this is an
# on-demand parity check run against a local checkout, not a pre-commit gate.
# Point -UpstreamRoot at a files-community/Files clone at the manifest's commit.
#
# Exit codes: 0 all icons match, 1 at least one mismatch or missing asset,
# 2 the upstream clone or manifest could not be read.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$UpstreamRoot,

    # Where to write the comparison report. Defaults to the certification
    # artifacts folder used by the rest of the File Explorer evidence.
    [string]$ReportPath = "artifacts/file-management-explorer-baseline/icon-parity-report.txt",

    [string]$ManifestPath = "resources/icons/files/manifest.json"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $UpstreamRoot -PathType Container)) {
    Write-Error "Upstream clone not found: $UpstreamRoot"
    exit 2
}
if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
    Write-Error "Icon manifest not found: $ManifestPath"
    exit 2
}

$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
$expectedCommit = $manifest.upstream.commit

# Report the commit the clone is actually on. A parity pass proves nothing about
# the recorded commit if the checkout has moved on, so this is stated in the
# report rather than assumed.
$actualCommit = '(not a git checkout)'
try {
    $actualCommit = (& git -C $UpstreamRoot rev-parse --short HEAD 2>$null)
    if ([string]::IsNullOrWhiteSpace($actualCommit)) { $actualCommit = '(not a git checkout)' }
} catch {
    $actualCommit = '(not a git checkout)'
}

$xamlCache = @{}
function Get-UpstreamXaml {
    param([string]$RelativePath)
    if (-not $xamlCache.ContainsKey($RelativePath)) {
        $full = Join-Path $UpstreamRoot $RelativePath
        if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
            $xamlCache[$RelativePath] = $null
        } else {
            $xamlCache[$RelativePath] = Get-Content -LiteralPath $full -Raw
        }
    }
    return $xamlCache[$RelativePath]
}

# The outline path for one x:Key. Scoped to that Style block so a later Style
# in the same file (e.g. the ".12" size variant) cannot answer for this key.
function Get-UpstreamOutlinePath {
    param([string]$RelativePath, [string]$Key)
    $text = Get-UpstreamXaml -RelativePath $RelativePath
    if ($null -eq $text) { return $null }
    $start = $text.IndexOf("x:Key=`"$Key`"")
    if ($start -lt 0) { return $null }
    $end = $text.IndexOf('</Style>', $start)
    if ($end -lt 0) { $end = $text.Length }
    $block = $text.Substring($start, $end - $start)
    $match = [regex]::Match($block, 'Property="OutlineIconData"\s+Value="([^"]*)"')
    if (-not $match.Success) { return $null }
    return $match.Groups[1].Value
}

function Get-ShippedPathData {
    param([string]$SvgPath)
    if (-not (Test-Path -LiteralPath $SvgPath -PathType Leaf)) { return $null }
    $svg = Get-Content -LiteralPath $SvgPath -Raw
    $match = [regex]::Match($svg, '\sd="([^"]*)"')
    if (-not $match.Success) { return $null }
    return $match.Groups[1].Value
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("Files icon parity report")
$lines.Add("manifest commit : $expectedCommit")
$lines.Add("clone HEAD      : $actualCommit")
$lines.Add("clone path      : $UpstreamRoot")
$lines.Add("")

$matched = 0
$failed = 0
foreach ($asset in $manifest.assets) {
    $shipped = Get-ShippedPathData -SvgPath $asset.file
    $upstream = Get-UpstreamOutlinePath -RelativePath $asset.source -Key $asset.upstream_key
    if ($null -eq $shipped) {
        $lines.Add(("MISSING-SVG  {0,-24} {1}" -f $asset.key, $asset.file))
        $failed++
    } elseif ($null -eq $upstream) {
        $lines.Add(("NO-UPSTREAM  {0,-24} {1} in {2}" -f $asset.key, $asset.upstream_key, $asset.source))
        $failed++
    } elseif ($shipped -eq $upstream) {
        $lines.Add(("MATCH        {0,-24} {1}" -f $asset.key, $asset.upstream_key))
        $matched++
    } else {
        $lines.Add(("MISMATCH     {0,-24} {1}" -f $asset.key, $asset.upstream_key))
        $lines.Add("    shipped : $shipped")
        $lines.Add("    upstream: $upstream")
        $failed++
    }
}

$lines.Add("")
$lines.Add("matched=$matched failed=$failed total=$($manifest.assets.Count)")

$reportDir = Split-Path -Parent $ReportPath
if ($reportDir -and -not (Test-Path -LiteralPath $reportDir -PathType Container)) {
    New-Item -ItemType Directory -Force -Path $reportDir | Out-Null
}
Set-Content -LiteralPath $ReportPath -Value ($lines -join "`n") -Encoding ascii

$lines | ForEach-Object { Write-Output $_ }

if ($failed -gt 0) {
    Write-Error "$failed icon(s) do not match the upstream Files assets they claim."
    exit 1
}
exit 0

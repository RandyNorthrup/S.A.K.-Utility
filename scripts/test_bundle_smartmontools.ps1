<#
.SYNOPSIS
    bundle_smartmontools.ps1 trusts a correctly pinned smartctl.exe, and a re-bundle changes no file.

.DESCRIPTION
    On 2026-11-10 the release workflow's first run found the script's check of an existing, pinned
    smartctl.exe failing on every fresh PowerShell 7 session: it read $LASTEXITCODE after a pipeline
    that Select-Object -First had cut short, which PowerShell 7 does not record. It then downloaded
    smartmontools again and rewrote the tracked tools/smartmontools/README.txt without its byte-order
    mark, which changed the tree the Partition Manager certification is bound to.

    Default (offline): a fresh session of this PowerShell runs the script against a copy of the tracked
    folder. It must report the bundle as present, download nothing, and rewrite no file.

    -Rebundle (downloads the pinned installer, about 1.5 MB): with -Force it bundles again into a copy of
    the tracked folder, and no tracked file may be rewritten.
#>
[CmdletBinding()]
param(
    [switch]$Rebundle
)

$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$script = Join-Path $repo 'scripts\bundle_smartmontools.ps1'
$tracked = Join-Path $repo 'tools\smartmontools'
$work = Join-Path ([IO.Path]::GetTempPath()) ('sak-bundle-smartmontools-' + [guid]::NewGuid().ToString('N'))
$destination = Join-Path $work 'smartmontools'

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "FAILED: $Message" }
}

function Get-TreeHashes([string]$Root) {
    $hashes = @{}
    foreach ($file in Get-ChildItem -LiteralPath $Root -File -Recurse) {
        $relative = $file.FullName.Substring($Root.Length + 1)
        $hashes[$relative] = '{0} {1}' -f (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash,
            $file.LastWriteTimeUtc.Ticks
    }
    return $hashes
}

New-Item -ItemType Directory -Path $work | Out-Null
try {
    Copy-Item -LiteralPath $tracked -Destination $destination -Recurse
    $before = Get-TreeHashes $destination
    Assert-True ($before.ContainsKey('smartctl.exe') -and $before.ContainsKey('README.txt')) 'the tracked folder holds smartctl.exe and README.txt'

    # A new process of the PowerShell running this test: the defect needed a session in which no
    # native command had run yet.
    $hostPath = (Get-Process -Id $PID).Path
    $arguments = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $script, '-DestinationPath', $destination)
    if ($Rebundle) { $arguments += '-Force' }
    $output = @(& $hostPath @arguments 2>&1 | ForEach-Object { "$_" })
    $exitCode = $LASTEXITCODE
    $text = $output -join "`n"
    Assert-True ($exitCode -eq 0) "bundle_smartmontools.ps1 exited $exitCode`n$text"

    if ($Rebundle) {
        Assert-True ($text -match 'SHA-256 matches pinned value') "the re-bundle downloaded and checked the pinned installer`n$text"
        Assert-True ($text -match 'Successfully bundled: smartctl ') "the re-bundled smartctl.exe reported its version`n$text"
    } else {
        Assert-True ($text -match 'smartctl already bundled: smartctl ') "a pinned smartctl.exe is accepted as bundled`n$text"
        Assert-True ($text -notmatch 're-downloading|Downloading smartmontools') "a pinned smartctl.exe is not downloaded again`n$text"
    }

    $after = Get-TreeHashes $destination
    foreach ($name in $before.Keys) {
        Assert-True ($after.ContainsKey($name)) "$name is still there"
        Assert-True ($after[$name] -eq $before[$name]) "$name is not rewritten (hash and write time were $($before[$name]), now $($after[$name]))"
    }
    $mode = if ($Rebundle) { 're-bundle with -Force' } else { 'existing pinned bundle' }
    Write-Host "PASS bundle_smartmontools.ps1 ($mode, $($PSVersionTable.PSEdition) $($PSVersionTable.PSVersion)): no tracked file is rewritten"
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

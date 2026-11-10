# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Re-pack (sign) the S.A.K. Utility browser-control extension into the committed CRX
# that the app force-installs on customer machines. Run this whenever anything under
# browser/extension changes so browser/dist/sak_browser_control.crx stays in sync.
#
# The signing key is NOT in the repo (a private key must never be committed). It lives
# outside the tree at %LOCALAPPDATA%\SAK\keys\sak_browser_control.signing-key.pem and
# MUST be backed up: losing it changes the extension id, which forces every installed
# machine to re-install under the new id. The public half of this key is pinned in
# browser/extension/manifest.json ("key"), so the unpacked-load id and the CRX id match.
#
# Usage:
#   ./pack-extension.ps1                       # sign with the default key -> browser/dist
#   ./pack-extension.ps1 -Key <path-to.pem>    # sign with an explicit key
#
# Expected extension id: ofodhfbipljnhenjjjpbdaglkjdphoec

[CmdletBinding()]
param(
    [string]$Chrome,
    [string]$Key,
    [string]$Out
)

$ErrorActionPreference = "Stop"

$ExpectedId = "ofodhfbipljnhenjjjpbdaglkjdphoec"
$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$SrcDir     = Join-Path $ScriptDir "extension"
$PackTimeoutMs = 120000

# Chrome derives an extension id from the SHA-256 of the signing key's DER
# SubjectPublicKeyInfo: the first 16 bytes, one character per nibble over 'a'..'p'.
# The installer force-lists a HARDCODED id (kBrowserExtensionId), so an artifact whose
# key derives any other id installs nowhere -- the id must be proven, never announced.
function ConvertTo-ExtensionId {
    param([byte[]]$PublicKeyDer)
    if ($null -eq $PublicKeyDer -or $PublicKeyDer.Length -lt 1) {
        throw "Public key is empty; cannot derive an extension id."
    }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { $digest = $sha.ComputeHash($PublicKeyDer) } finally { $sha.Dispose() }
    $id = ""
    foreach ($octet in $digest[0..15]) {
        $id += [char](97 + ($octet -shr 4))
        $id += [char](97 + ($octet -band 0x0F))
    }
    return $id
}

function Read-ProtoVarint {
    param([byte[]]$Buffer, [ref]$Position)
    $value = [long]0
    $shift = 0
    while ($Position.Value -lt $Buffer.Length) {
        $octet = $Buffer[$Position.Value]
        $Position.Value++
        $value = $value -bor (([long]($octet -band 0x7F)) -shl $shift)
        if (($octet -band 0x80) -eq 0) { return $value }
        $shift += 7
        if ($shift -gt 56) { throw "CRX3 header is malformed: oversized varint." }
    }
    throw "CRX3 header is malformed: truncated varint."
}

# CrxFileHeader and AsymmetricKeyProof carry only length-delimited fields, so any other
# wire type means these bytes are not a CRX3 header -- refuse rather than guess past it.
function Get-ProtoBytesField {
    param([byte[]]$Buffer, [int]$FieldNumber)
    $position = 0
    while ($position -lt $Buffer.Length) {
        $tag = Read-ProtoVarint -Buffer $Buffer -Position ([ref]$position)
        $field = [int]($tag -shr 3)
        if (([int]($tag -band 7)) -ne 2) {
            throw "CRX3 header field $field is not length-delimited; the file is not a CRX3."
        }
        $length = [long](Read-ProtoVarint -Buffer $Buffer -Position ([ref]$position))
        if ($length -lt 1 -or ($position + $length) -gt $Buffer.Length) {
            throw "CRX3 header field $field is empty or runs past the end of the header."
        }
        if ($field -eq $FieldNumber) { return [byte[]]$Buffer[$position..($position + $length - 1)] }
        $position += $length
    }
    throw "CRX3 header carries no field $FieldNumber."
}

# The id the artifact will actually install under. Field 2 of CrxFileHeader is the
# sha256_with_rsa proof and its field 1 is the DER public key Chrome signed with, so
# this reads the packed file itself rather than trusting the run that produced it.
function Get-CrxExtensionId {
    param([string]$Path)
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 16) {
        throw "CRX $Path is $($bytes.Length) bytes; too short to be a CRX3 file."
    }
    if ([System.Text.Encoding]::ASCII.GetString($bytes, 0, 4) -cne "Cr24") {
        throw "CRX $Path does not start with the Cr24 magic; it is not a CRX file."
    }
    $version = [long][BitConverter]::ToUInt32($bytes, 4)
    if ($version -ne 3) {
        throw "CRX $Path declares format version $version; only CRX3 can be verified."
    }
    $headerSize = [long][BitConverter]::ToUInt32($bytes, 8)
    if ($headerSize -lt 1 -or (12 + $headerSize) -ge $bytes.Length) {
        throw "CRX $Path declares a $headerSize-byte header that leaves no archive in $($bytes.Length) bytes."
    }
    $header = [byte[]]$bytes[12..(11 + $headerSize)]
    $proof = Get-ProtoBytesField -Buffer $header -FieldNumber 2
    return ConvertTo-ExtensionId -PublicKeyDer (Get-ProtoBytesField -Buffer $proof -FieldNumber 1)
}

# A CRX is a CRX3 header followed by a zip, and the zip's end-of-central-directory record
# is the LAST thing written. Requiring it -- and requiring the directory it points at to
# fit inside the file -- is what tells a complete artifact from an interrupted copy, which
# keeps the magic and the header and would otherwise verify.
function Assert-CrxArchiveComplete {
    param([string]$Path)
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $archiveOffset = 12 + [long][BitConverter]::ToUInt32($bytes, 8)
    $limit = [Math]::Max($archiveOffset, $bytes.Length - 65557)
    $eocd = -1
    for ($i = $bytes.Length - 22; $i -ge $limit; $i--) {
        if ([BitConverter]::ToUInt32($bytes, $i) -eq 0x06054B50) { $eocd = $i; break }
    }
    if ($eocd -lt 0) {
        throw "CRX $Path has no zip end-of-central-directory record; it is truncated."
    }
    $directorySize = [long][BitConverter]::ToUInt32($bytes, $eocd + 12)
    $directoryOffset = [long][BitConverter]::ToUInt32($bytes, $eocd + 16)
    if (($archiveOffset + $directoryOffset + $directorySize) -gt $eocd) {
        throw "CRX $Path zip central directory runs past its end record; it is truncated."
    }
}

# -Out is typed by hand. Resolve it against the shell's location (not the process CWD,
# which can differ) so a bare filename still has a parent directory to create and a rename
# target on the same volume, and reject a directory: copying onto one lands the file
# inside it under Chrome's own name and leaves the real artifact stale.
function Resolve-CrxOutputPath {
    param([string]$Path, [string]$BaseDirectory)
    $full = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($BaseDirectory, $Path))
    if (Test-Path -LiteralPath $full -PathType Container) {
        throw "-Out names an existing directory: $full (pass the full path of the .crx file)."
    }
    if ([System.IO.Path]::GetExtension($full) -ne ".crx") {
        throw "-Out must name a .crx file: $full"
    }
    return $full
}

# Chrome explains a pack failure on its own streams; that text is the only account of
# WHY the pack failed, so it is carried into the exception instead of being discarded.
# Quote one argument the way CreateProcess parses it back out, so a path containing spaces
# survives being flattened into Start-Process's single command line. A trailing backslash is
# the trap: "C:\dir\" would escape the closing quote and swallow the next argument, so runs
# of backslashes immediately before the closing quote are doubled.
function Format-NativeArgument {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value)
    if ($Value.Contains('"')) {
        # No argument this script builds contains a quote; one appearing means the caller
        # supplied something unexpected, and guessing at an escaping is how injection starts.
        throw "Refusing to pass an argument containing a double quote: $Value"
    }
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s]') {
        return $Value
    }
    $trailing = 0
    while ($trailing -lt $Value.Length -and $Value[$Value.Length - 1 - $trailing] -eq '\') {
        $trailing++
    }
    return '"' + $Value + ('\' * $trailing) + '"'
}

function Get-ProcessDiagnostics {
    param([string[]]$LogPaths)
    $parts = @()
    foreach ($logPath in $LogPaths) {
        if (Test-Path -LiteralPath $logPath -PathType Leaf) {
            $text = Get-Content -LiteralPath $logPath -Raw
            if ($text) { $parts += $text.Trim() }
        }
    }
    if ($parts.Count -eq 0) { return "(chrome wrote no output)" }
    return ($parts -join [System.Environment]::NewLine)
}

# Run Chrome's packer over StageDir and return the CRX it produced. Chrome names that output
# after the directory it packed and drops it beside it, so the path is derived from StageDir
# rather than assumed. Every way this run can fail -- a nonzero exit, a wedge, a silent
# no-output -- ends the script with Chrome's own account of it attached, because the alternative
# is copying a stale or absent artifact over the one customers install.
function Invoke-ChromePack {
    param(
        [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$ChromePath,
        [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$StageDir,
        [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$KeyPath,
        [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$WorkDir,
        [Parameter(Mandatory = $true)][ValidateRange(1, [int]::MaxValue)][int]$TimeoutMs)
    $packOut = Join-Path $WorkDir "pack-stdout.txt"
    $packErr = Join-Path $WorkDir "pack-stderr.txt"
    # -ArgumentList does NOT quote array elements: PowerShell joins them with a plain space
    # and hands the result to CreateProcess, so a stage or key path containing a space
    # arrives as two arguments and Chrome packs the wrong directory (or nothing). The
    # call-operator form quoted each element for us; since Start-Process is needed here for
    # the timeout and the redirects, the quoting has to be explicit.
    $packArgs = (@("--pack-extension=$StageDir", "--pack-extension-key=$KeyPath", "--no-message-box") |
        ForEach-Object { Format-NativeArgument -Value $_ }) -join " "
    $proc = Start-Process -FilePath $ChromePath -ArgumentList $packArgs -PassThru -NoNewWindow `
        -RedirectStandardOutput $packOut -RedirectStandardError $packErr
    # A wedged Chrome (profile lock, AV interference) must end the run, not own it.
    if (-not $proc.WaitForExit($TimeoutMs)) {
        # Kill($true), not Kill(): chrome.exe is a launcher that spawns its own child processes,
        # and killing only the one this script holds leaves that tree running -- still holding the
        # staging directory the cleanup below is about to try to delete, and still packing.
        $proc.Kill($true)
        $null = $proc.WaitForExit(5000)
        throw "chrome --pack-extension did not exit within $TimeoutMs ms; killed it."
    }
    if ($proc.ExitCode -ne 0) {
        throw ("chrome --pack-extension failed (exit $($proc.ExitCode)): " +
               (Get-ProcessDiagnostics -LogPaths @($packOut, $packErr)))
    }
    $producedCrx = Join-Path (Split-Path -Parent $StageDir) ((Split-Path -Leaf $StageDir) + ".crx")
    if (-not (Test-Path -LiteralPath $producedCrx -PathType Leaf)) {
        throw ("Chrome did not produce a CRX at ${producedCrx}: " +
               (Get-ProcessDiagnostics -LogPaths @($packOut, $packErr)))
    }
    return $producedCrx
}

if (-not $Key) {
    $Key = Join-Path $env:LOCALAPPDATA "SAK\keys\sak_browser_control.signing-key.pem"
}
# -LiteralPath -PathType Leaf: a wildcard or a directory would pass the plain form and
# then be handed to Chrome as a literal path it cannot open, blaming the wrong thing.
if (-not (Test-Path -LiteralPath $Key -PathType Leaf)) {
    throw "Signing key not found: $Key (restore your backed-up key, or pass -Key)."
}
if (-not $Out) {
    $Out = Join-Path $ScriptDir "dist\sak_browser_control.crx"
}
# .ProviderPath, not .Path: on a PowerShell-only drive (New-PSDrive -PSProvider FileSystem) the
# location reads back as "Work:\sub", which is a PowerShell path, not one System.IO can combine
# or open. ProviderPath is the same location expressed as the filesystem sees it.
$Out = Resolve-CrxOutputPath -Path $Out -BaseDirectory (Get-Location -PSProvider FileSystem).ProviderPath
if (-not $Chrome) {
    $candidates = @(
        "$env:ProgramFiles\Google\Chrome\Application\chrome.exe",
        "${env:ProgramFiles(x86)}\Google\Chrome\Application\chrome.exe",
        "$env:LOCALAPPDATA\Google\Chrome\Application\chrome.exe"
    )
    $Chrome = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
}
if (-not $Chrome -or -not (Test-Path -LiteralPath $Chrome -PathType Leaf)) {
    throw "chrome.exe not found; pass -Chrome <path>."
}

# An unpacked load takes its id from the manifest's pinned key. If that has drifted from
# the signing key, developers and customers run two different extensions.
$manifest = Get-Content -LiteralPath (Join-Path $SrcDir "manifest.json") -Raw | ConvertFrom-Json
if (-not $manifest.key) {
    throw "browser/extension/manifest.json has no pinned key; the unpacked-load id cannot match the CRX."
}
$manifestId = ConvertTo-ExtensionId -PublicKeyDer ([System.Convert]::FromBase64String($manifest.key))
if ($manifestId -ne $ExpectedId) {
    throw "manifest.json's pinned key derives extension id $manifestId, not $ExpectedId."
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("sak_ext_pack_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $work | Out-Null
$staged = $null

try {
    # Pack a COPY so Chrome's <dir>.crx / <dir>.pem outputs never land in the source tree.
    # Staging lives inside the try so a failed copy still hits the cleanup below.
    $stage = Join-Path $work "extension"
    Copy-Item -Recurse $SrcDir $stage
    $producedCrx = Invoke-ChromePack -ChromePath $Chrome -StageDir $stage -KeyPath $Key `
        -WorkDir $work -TimeoutMs $PackTimeoutMs
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Out) | Out-Null
    # Verify a sibling copy and rename it into place: the destination is then either the
    # previous known-good artifact or a fully written one that carries the pinned id --
    # never a truncated file or one signed with the wrong key.
    $staged = "$Out.new"
    Copy-Item -LiteralPath $producedCrx -Destination $staged -Force
    $packedId = Get-CrxExtensionId -Path $staged
    Assert-CrxArchiveComplete -Path $staged
    if ($packedId -ne $ExpectedId) {
        throw "Packed CRX derives extension id $packedId, not the pinned $ExpectedId (wrong signing key?)."
    }
    Move-Item -LiteralPath $staged -Destination $Out -Force
    $staged = $null
    Write-Host "Wrote $Out ($((Get-Item -LiteralPath $Out).Length) bytes)"
    Write-Host "Verified extension id: $packedId"
} finally {
    # Best effort by design: a throw from finally would replace the real failure above.
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    if ($staged -and (Test-Path -LiteralPath $staged -PathType Leaf)) {
        Remove-Item -LiteralPath $staged -Force -ErrorAction SilentlyContinue
    }
}

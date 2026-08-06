# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Register (or remove) the S.A.K. Utility browser-control native messaging host for
# Chrome on the current user. Chrome launches the host named here whenever the
# extension calls chrome.runtime.connectNative("com.sak.browsercontrol").
#
# Usage:
#   ./register_native_host.ps1 [-ExtensionId <id>] [-ExePath <path-to-sak_win32_mcp.exe>]
#   ./register_native_host.ps1 -Unregister
#
# ExtensionId defaults to the pinned production id; pass it only for an unpacked
# build with a different key, in which case the id is shown on chrome://extensions
# after you load the unpacked extension from browser/extension. ExePath defaults to
# the built Debug binary beside this repo; pass it explicitly for a Release/installed
# build.

[CmdletBinding(DefaultParameterSetName = "Register")]
param(
    # Defaults to the pinned production extension id (browser/extension/manifest.json "key").
    # Pass a different id only when testing an unpacked build with a different key.
    [Parameter(ParameterSetName = "Register")]
    [string]$ExtensionId = "ofodhfbipljnhenjjjpbdaglkjdphoec",

    # Validation runs only at binding time, so an omitted -ExePath still takes the
    # documented Debug default below while an explicitly-passed empty value fails
    # closed instead of being silently redirected to that default.
    [Parameter(ParameterSetName = "Register")]
    [ValidateNotNullOrEmpty()]
    [string]$ExePath,

    [Parameter(ParameterSetName = "Unregister", Mandatory = $true)]
    [switch]$Unregister,

    # Where the native-messaging-host key lives. Overridable for one reason: the guard test
    # must be able to register into a scratch key instead of saving, clobbering and restoring
    # the developer's own live Chrome registration -- a test that has to put back what it
    # broke leaves the machine broken whenever it is interrupted. Production never passes it.
    [ValidateNotNullOrEmpty()]
    [string]$RegistryRoot = "HKCU:\Software\Google\Chrome\NativeMessagingHosts"
)

$ErrorActionPreference = "Stop"
# A missing variable or property must be an error here, never a silent empty string
# that gets written into the manifest or the registry as if it were a real value.
Set-StrictMode -Version Latest

$HostName = "com.sak.browsercontrol"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ManifestPath = Join-Path $ScriptDir "$HostName.json"
# Per-user registration only. Constraining the override to HKCU: means a mistyped or
# injected root cannot turn this into a machine-wide write under HKLM.
if ($RegistryRoot -notmatch '^HKCU:\\') {
    throw "RegistryRoot must be under HKCU: (per-user registration), got: $RegistryRoot"
}
$RegKey = $RegistryRoot.TrimEnd('\') + "\" + $HostName

if ($Unregister) {
    if (Test-Path $RegKey) {
        Remove-Item -Path $RegKey -Force
        Write-Host "Removed registry key $RegKey"
    } else {
        Write-Host "Registry key $RegKey not present; nothing to remove."
    }
    return
}

# Resolve the host executable.
if (-not $ExePath) {
    $ExePath = Join-Path $ScriptDir "..\..\build\Debug\sak_win32_mcp.exe"
}
$ExePath = [System.IO.Path]::GetFullPath($ExePath)
# Chrome re-resolves this path on every launch, so it must name a local file this
# machine owns -- a UNC share would let a remote host supply the native host binary.
if ($ExePath.StartsWith("\\")) {
    throw "Host executable must be a local path, not a UNC/device path: $ExePath"
}
# A drive letter can itself BE a remote share ("net use Z: \\server\share"), so refusing the
# UNC prefix alone leaves the very thing that prefix was guarding against wide open. Classify
# the volume and accept only ones this machine owns. A root that cannot be classified is
# refused with them: "could not tell" is not "local".
$ExeRoot = [System.IO.Path]::GetPathRoot($ExePath)
$ExeDriveType = [System.IO.DriveType]::Unknown
try {
    $ExeDriveType = (New-Object System.IO.DriveInfo($ExeRoot)).DriveType
} catch {
    $ExeDriveType = [System.IO.DriveType]::Unknown
}
if ($ExeDriveType -ne [System.IO.DriveType]::Fixed -and
    $ExeDriveType -ne [System.IO.DriveType]::Removable) {
    throw ("Host executable must live on a local volume; '$ExeRoot' is a $ExeDriveType drive: " +
           $ExePath)
}
# -LiteralPath so a wildcard cannot satisfy the guard while the unexpanded literal
# is what gets written, and -PathType Leaf so a directory is not accepted as the
# executable. Otherwise a registration Chrome can never launch is reported as success.
if (-not (Test-Path -LiteralPath $ExePath -PathType Leaf)) {
    throw "Host executable not found: $ExePath (build sak_win32_mcp, or pass -ExePath)."
}

# Normalize the extension id (accept a bare id or a full chrome-extension:// origin).
$id = $ExtensionId.Trim()
$id = $id -replace "^chrome-extension://", ""
$id = $id.TrimEnd("/")
# -cnotmatch, not -notmatch: PowerShell's default matching is case-insensitive, which
# would admit an upper-case id and write a non-canonical origin into the manifest. The
# value that is validated has to be the exact value that is written.
if ($id -cnotmatch "^[a-p]{32}$") {
    throw "Extension id '$id' does not look like a 32-char lower-case Chrome extension id."
}

# Write the manifest with the concrete path + allowed origin. Native messaging
# manifests require forward or escaped-backslash paths; ConvertTo-Json escapes them.
$manifest = [ordered]@{
    name            = $HostName
    description     = "S.A.K. Utility browser-control native messaging host"
    path            = $ExePath
    type            = "stdio"
    allowed_origins = @("chrome-extension://$id/")
}
# Stage to a sibling and rename over the target only after the staged bytes parse:
# an interrupted or failed write must leave the previous good manifest intact rather
# than a truncated file Chrome refuses to parse.
$ManifestTemp = "$ManifestPath.tmp"
try {
    $manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $ManifestTemp -Encoding UTF8
    $null = Get-Content -Raw -LiteralPath $ManifestTemp | ConvertFrom-Json
    Move-Item -LiteralPath $ManifestTemp -Destination $ManifestPath -Force
} finally {
    # Cleanup must never replace the failure that brought us here. With $ErrorActionPreference
    # "Stop" a staging file that cannot be removed (still locked, denied) would throw from the
    # finally block and become the error the operator sees -- hiding the real one, which is the
    # write or the parse that actually failed. A leftover temp file is reported, not raised.
    try {
        if (Test-Path -LiteralPath $ManifestTemp) {
            Remove-Item -LiteralPath $ManifestTemp -Force
        }
    } catch {
        Write-Warning "Could not remove staging file ${ManifestTemp}: $($_.Exception.Message)"
    }
}
Write-Host "Wrote host manifest: $ManifestPath"

# Point Chrome at the manifest for this user.
New-Item -Path $RegKey -Force | Out-Null
Set-ItemProperty -Path $RegKey -Name "(Default)" -Value $ManifestPath

# Read back both writes before claiming success: the manifest has to hold the exact
# path and origin that were validated above, and the registry provider has to have
# resolved "(Default)" to the key's default value rather than a literally-named one.
$written = Get-Content -Raw -LiteralPath $ManifestPath | ConvertFrom-Json
$writtenOrigins = @($written.allowed_origins) -join ","
if ($written.path -ne $ExePath -or $writtenOrigins -ne "chrome-extension://$id/") {
    throw "Host manifest read-back mismatch in ${ManifestPath}: path='$($written.path)' allowed_origins='$writtenOrigins'"
}
if ((Get-ItemProperty -LiteralPath $RegKey).'(default)' -ne $ManifestPath) {
    throw "Registry default value did not take: $RegKey"
}
Write-Host "Registered $HostName -> $ManifestPath"
Write-Host "Reload the extension (or restart Chrome), then click the toolbar button to ping the host."

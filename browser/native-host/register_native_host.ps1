# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Register (or remove) the S.A.K. Utility browser-control native messaging host for
# Chrome on the current user. Chrome launches the host named here whenever the
# extension calls chrome.runtime.connectNative("com.sak.browsercontrol").
#
# Usage:
#   ./register_native_host.ps1 -ExtensionId <id> [-ExePath <path-to-sak_win32_mcp.exe>]
#   ./register_native_host.ps1 -Unregister
#
# The extension id is shown on chrome://extensions after you load the unpacked
# extension from browser/extension. ExePath defaults to the built Debug binary
# beside this repo; pass it explicitly for a Release/installed build.

[CmdletBinding(DefaultParameterSetName = "Register")]
param(
    [Parameter(ParameterSetName = "Register", Mandatory = $true)]
    [string]$ExtensionId,

    [Parameter(ParameterSetName = "Register")]
    [string]$ExePath,

    [Parameter(ParameterSetName = "Unregister", Mandatory = $true)]
    [switch]$Unregister
)

$ErrorActionPreference = "Stop"

$HostName = "com.sak.browsercontrol"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ManifestPath = Join-Path $ScriptDir "$HostName.json"
$RegKey = "HKCU:\Software\Google\Chrome\NativeMessagingHosts\$HostName"

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
if (-not (Test-Path $ExePath)) {
    throw "Host executable not found: $ExePath (build sak_win32_mcp, or pass -ExePath)."
}

# Normalize the extension id (accept a bare id or a full chrome-extension:// origin).
$id = $ExtensionId.Trim()
$id = $id -replace "^chrome-extension://", ""
$id = $id.TrimEnd("/")
if ($id -notmatch "^[a-p]{32}$") {
    throw "Extension id '$id' does not look like a 32-char Chrome extension id."
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
$manifest | ConvertTo-Json -Depth 4 | Set-Content -Path $ManifestPath -Encoding UTF8
Write-Host "Wrote host manifest: $ManifestPath"

# Point Chrome at the manifest for this user.
New-Item -Path $RegKey -Force | Out-Null
Set-ItemProperty -Path $RegKey -Name "(Default)" -Value $ManifestPath
Write-Host "Registered $HostName -> $ManifestPath"
Write-Host "Reload the extension (or restart Chrome), then click the toolbar button to ping the host."

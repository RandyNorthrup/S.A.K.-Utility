# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard tests for browser/native-host/register_native_host.ps1. Every case here is a
# malformed input that must FAIL CLOSED, or a write that must be verified before the
# script claims the host is registered.
#
# The script under test is copied into a scratch directory first, so the manifest it
# writes lands there instead of over the committed template, and it is pointed at a
# throwaway -RegistryRoot so the developer's own live Chrome registration is never
# touched. Saving and restoring the real key was the obvious alternative and is worse:
# an interrupted run would leave this machine's browser control unregistered.
#
# Usage: pwsh -NoProfile -File tests/unit/test_register_native_host.ps1
#        pwsh -NoProfile -File tests/unit/test_register_native_host.ps1 -ScriptUnderTest <path>

param(
    [ValidateNotNullOrEmpty()]
    [string]$ScriptUnderTest = (Join-Path $PSScriptRoot "..\..\browser\native-host\register_native_host.ps1")
)

$ErrorActionPreference = "Stop"

$script:Failures = 0
$RegRoot = "HKCU:\Software\SAK-Utility-Tests\NativeMessagingHosts-" + [guid]::NewGuid().ToString("N")
$RegKey = "$RegRoot\com.sak.browsercontrol"
$PinnedId = "ofodhfbipljnhenjjjpbdaglkjdphoec"

function Report([bool]$Ok, [string]$Name, [string]$Detail) {
    if ($Ok) {
        Write-Host "PASS $Name"
    } else {
        Write-Host "FAIL $Name : $Detail"
        $script:Failures++
    }
}

function Get-RegDefault([string]$Key) {
    if (-not (Test-Path -LiteralPath $Key)) { return $null }
    $props = Get-ItemProperty -LiteralPath $Key
    if ($null -eq $props.PSObject.Properties['(default)']) { return $null }
    return $props.'(default)'
}

# Run the script under test out-of-process so a terminating error is observable as a
# non-zero exit code rather than killing this harness.
# Every invocation carries the scratch -RegistryRoot, so no case in this file can reach
# the real Chrome key even if one is added later without thinking about it.
function Invoke-Script([string]$ScriptPath, [string[]]$ScriptArgs) {
    $all = @($ScriptArgs) + @("-RegistryRoot", $RegRoot)
    $out = & pwsh -NoProfile -NonInteractive -File $ScriptPath @all 2>&1
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = ($out | Out-String) }
}

# A guard has to reject for its OWN reason: an incidental "not found" further down the
# script would satisfy a bare exit-code check while the guard itself is still missing.
function Assert-FailsClosed([string]$Name, [string]$ScriptPath, [string[]]$ScriptArgs,
                            [string]$ExpectMatch) {
    $r = Invoke-Script $ScriptPath $ScriptArgs
    $ok = $r.ExitCode -ne 0 -and $r.Output -match $ExpectMatch
    Report $ok $Name "expected non-zero exit matching '$ExpectMatch', got $($r.ExitCode): $($r.Output)"
}

$Scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("sak_nh_test_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $Scratch -Force | Out-Null

try {
    # Mirror the repo layout two levels down, so the script's own
    # $ScriptDir\..\..\build\Debug default resolves INSIDE the scratch root and is
    # removed with it -- nothing this test creates may outlive it.
    $HostDir = Join-Path $Scratch "repo\browser\native-host"
    New-Item -ItemType Directory -Path $HostDir -Force | Out-Null
    $Copy = Join-Path $HostDir "register_native_host.ps1"
    Copy-Item -LiteralPath $ScriptUnderTest -Destination $Copy
    $ManifestPath = Join-Path $HostDir "com.sak.browsercontrol.json"

    # The default -ExePath is $ScriptDir\..\..\build\Debug\sak_win32_mcp.exe. Materialize
    # it so an empty -ExePath that gets coerced to the default would SUCCEED, which is
    # what makes the coercion observable instead of hiding behind a missing file.
    $DefaultExeDir = Join-Path $Scratch "repo\build\Debug"
    New-Item -ItemType Directory -Path $DefaultExeDir -Force | Out-Null
    $DefaultExe = Join-Path $DefaultExeDir "sak_win32_mcp.exe"
    Set-Content -LiteralPath $DefaultExe -Value "stub" -Encoding UTF8

    $RealExe = Join-Path $Scratch "sak_win32_mcp.exe"
    Set-Content -LiteralPath $RealExe -Value "stub" -Encoding UTF8
    $ExeDir = Join-Path $Scratch "exedir"
    New-Item -ItemType Directory -Path $ExeDir -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $ExeDir "sak_win32_mcp.exe") -Value "stub" -Encoding UTF8

    # F1: an explicitly-empty -ExePath is malformed input; it must not be redirected to
    # the Debug default.
    Assert-FailsClosed "empty_exepath_fails_closed" $Copy @("-ExePath", "") "null or empty"

    # F3: the guard must validate the exact literal that gets written -- a directory is
    # not an executable, and a wildcard is never expanded into the manifest.
    Assert-FailsClosed "directory_exepath_fails_closed" $Copy @("-ExePath", $ExeDir) `
        "Host executable not found"
    Assert-FailsClosed "wildcard_exepath_fails_closed" $Copy @("-ExePath", (Join-Path $ExeDir "*.exe")) `
        "Host executable not found"

    # F4: Chrome re-resolves the recorded path on every launch, so a non-local path must
    # not be recorded.
    Assert-FailsClosed "unc_exepath_fails_closed" $Copy @("-ExePath", "\\?\$RealExe") `
        "must be a local path"

    # F4b: the UNC prefix is not the only way to reach a remote binary -- "net use Z: \\host\s"
    # gives a drive letter that resolves off-machine. The guard therefore classifies the volume,
    # so a root this machine does not own is refused for THAT reason, ahead of the not-found
    # check. An unused drive letter is the same unclassifiable root as an unmapped share and
    # needs no network to exercise.
    $UsedLetters = @([System.IO.DriveInfo]::GetDrives() | ForEach-Object { $_.Name.Substring(0, 1) })
    $FreeLetter = @([char[]]"XYWVUT" | Where-Object { $UsedLetters -notcontains $_ }) |
        Select-Object -First 1
    if ($FreeLetter) {
        Assert-FailsClosed "nonlocal_drive_exepath_fails_closed" $Copy @(
            "-ExePath", "${FreeLetter}:\sak_win32_mcp.exe") "must live on a local volume"
    } else {
        Report $false "nonlocal_drive_exepath_fails_closed" `
            "no free drive letter among X,Y,W,V,U,T to test the volume guard with"
    }

    # F6: the id guard is case-sensitive; an upper-case id is not a canonical origin.
    Assert-FailsClosed "uppercase_extension_id_fails_closed" $Copy @(
        "-ExtensionId", $PinnedId.ToUpperInvariant(), "-ExePath", $RealExe) `
        "lower-case Chrome extension id"

    # F2: a write that fails part way must leave the previous good manifest intact. A
    # read-only staging sibling blocks the staged write; a script that writes straight
    # to the live manifest ignores it and overwrites the file instead.
    $Sentinel = '{"name":"prior-good"}'
    Set-Content -LiteralPath $ManifestPath -Value $Sentinel -Encoding UTF8
    $StagePath = "$ManifestPath.tmp"
    Set-Content -LiteralPath $StagePath -Value "blocked" -Encoding UTF8
    Set-ItemProperty -LiteralPath $StagePath -Name IsReadOnly -Value $true
    $r = Invoke-Script $Copy @("-ExePath", $RealExe)
    $survived = (Get-Content -Raw -LiteralPath $ManifestPath).Trim() -eq $Sentinel
    Report ($r.ExitCode -ne 0 -and $survived) "failed_write_leaves_prior_manifest_intact" `
        "exit=$($r.ExitCode) survived=$survived output=$($r.Output)"
    if (Test-Path -LiteralPath $StagePath) {
        Set-ItemProperty -LiteralPath $StagePath -Name IsReadOnly -Value $false
        Remove-Item -LiteralPath $StagePath -Force
    }
    Remove-Item -LiteralPath $ManifestPath -Force -ErrorAction SilentlyContinue

    # F2 + F7: the happy path must land a parseable manifest holding the exact values
    # that were validated, a registry default value that resolved, and no staging residue.
    $r = Invoke-Script $Copy @("-ExePath", $RealExe)
    Report ($r.ExitCode -eq 0) "happy_path_registers" "exit=$($r.ExitCode): $($r.Output)"
    if ($r.ExitCode -eq 0) {
        $written = Get-Content -Raw -LiteralPath $ManifestPath | ConvertFrom-Json
        Report ($written.path -eq $RealExe) "manifest_records_exact_path" "got '$($written.path)'"
        Report ((@($written.allowed_origins) -join ",") -eq "chrome-extension://$PinnedId/") `
            "manifest_records_canonical_origin" "got '$(@($written.allowed_origins) -join ",")'"
        Report ((Get-RegDefault $RegKey) -eq $ManifestPath) "registry_default_value_resolved" `
            "got '$(Get-RegDefault $RegKey)'"
        Report (-not (Test-Path -LiteralPath "$ManifestPath.tmp")) "no_staging_residue" `
            "staging file left behind"
    }

    # F8: strict mode must not break the documented removal path.
    $r = Invoke-Script $Copy @("-Unregister")
    Report ($r.ExitCode -eq 0 -and -not (Test-Path -LiteralPath $RegKey)) "unregister_removes_key" `
        "exit=$($r.ExitCode) keyPresent=$(Test-Path -LiteralPath $RegKey): $($r.Output)"
    $r = Invoke-Script $Copy @("-Unregister")
    Report ($r.ExitCode -eq 0) "unregister_is_idempotent" "exit=$($r.ExitCode): $($r.Output)"

    # F8/F9: hardening and documentation that has no observable runtime behavior today.
    $source = Get-Content -Raw -LiteralPath $ScriptUnderTest
    Report ($source -match "Set-StrictMode -Version Latest") "strict_mode_enabled" "not found"
    Report ($source -match "\[-ExtensionId <id>\]") "usage_marks_extensionid_optional" `
        "usage synopsis still shows -ExtensionId as required"
} finally {
    # The scratch root is this run's alone, so it is deleted outright -- there is no prior
    # state to put back, which is the point of using it.
    if (Test-Path -LiteralPath $RegRoot) {
        Remove-Item -LiteralPath $RegRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $Scratch -Recurse -Force -ErrorAction SilentlyContinue
}

if ($script:Failures -gt 0) {
    Write-Host "FAILED: $($script:Failures) case(s)"
    exit 1
}
Write-Host "All register_native_host guard cases passed."
exit 0

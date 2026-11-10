# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    Proves the signature verifier's publisher pin on real signed binaries.

.DESCRIPTION
    A signature cannot be faked for a test, and a mocked one proves nothing, so the fixtures are
    real Authenticode-signed files copied under first-party names: the bundled choco.exe, signed by
    Chocolatey Software, and a Windows system DLL, signed by Microsoft.
#>

[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$Verifier = Join-Path $ProjectRoot "scripts\verify_authenticode_signatures.ps1"
$ChocoSource = Join-Path $ProjectRoot "tools\chocolatey\choco.exe"
# This host, not a hard-coded edition: a child of the other edition inherits a PSModulePath that
# cannot load Get-AuthenticodeSignature.
$HostExecutable = (Get-Process -Id $PID).Path
$FirstParty = @(
    "sak_utility.exe",
    "sak_elevated_helper.exe",
    "sak_apfs_writer_cli.exe",
    "sak_hfs_writer_cli.exe"
)

function Get-Subject([string]$Path) {
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne "Valid") {
        throw "Fixture is not validly signed: $Path ($($signature.Status))"
    }
    return $signature.SignerCertificate.Subject
}

function New-PackageFixture {
    param([string[]]$Names = $FirstParty)
    $root = Join-Path ([IO.Path]::GetTempPath()) ("sak-signature-test-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $root | Out-Null
    foreach ($name in $Names) {
        Copy-Item -LiteralPath $ChocoSource -Destination (Join-Path $root $name)
    }
    return $root
}

# The verifier in its own process, as every caller runs it: only the exit code and text count.
function Invoke-Verifier {
    param([string]$Root, [string]$ExpectedPublisher = "")
    $arguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $Verifier, "-RootDir", $Root)
    if ($ExpectedPublisher) {
        $arguments += @("-ExpectedPublisher", $ExpectedPublisher)
    }
    $output = & $HostExecutable @arguments 2>&1 | Out-String
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output }
}

function Assert-Refused($Result, [string]$Expected, [string]$Case) {
    if ($Result.ExitCode -eq 0) {
        throw "${Case}: the verifier accepted this package.`n$($Result.Output)"
    }
    if ($Result.Output -notmatch [regex]::Escape($Expected)) {
        throw "${Case}: refused, but not because of '$Expected'.`n$($Result.Output)"
    }
}

function Assert-Accepted($Result, [string]$Case) {
    if ($Result.ExitCode -ne 0) {
        throw "${Case}: the verifier refused this package.`n$($Result.Output)"
    }
}

$chocoSubject = Get-Subject $ChocoSource
# Whichever of these this Windows has signed by someone other than Chocolatey.
$MicrosoftSource = @("ucrtbase.dll", "msvcp140.dll", "vcruntime140.dll", "ntdll.dll", "kernel32.dll") |
    ForEach-Object { Join-Path $env:windir "System32\$_" } |
    Where-Object {
        (Test-Path -LiteralPath $_ -PathType Leaf) -and
        (Get-AuthenticodeSignature -LiteralPath $_).Status -eq "Valid"
    } |
    Select-Object -First 1
if ([string]::IsNullOrEmpty($MicrosoftSource)) {
    throw "No validly signed system DLL found to use as the second publisher's fixture"
}
if ($chocoSubject -ceq (Get-Subject $MicrosoftSource)) {
    throw "The two fixtures must come from different publishers"
}

$fixtures = @()
try {
    # The default pin: first-party binaries signed by anyone else are refused, however valid.
    $root = New-PackageFixture
    $fixtures += $root
    Assert-Refused (Invoke-Verifier $root) "not signed by 'CN=Randy Northrup, O=Randy Northrup'" `
        "wrong publisher under the default pin"

    # The same files pass once the pin names their real signer: the status check is not the
    # reason for the refusal above.
    Assert-Accepted (Invoke-Verifier $root $chocoSubject) "first-party binaries with the pinned publisher"

    # A signer whose subject merely starts with the pin is not the publisher.
    $prefix = $chocoSubject.Substring(0, $chocoSubject.Length - 3)
    Assert-Refused (Invoke-Verifier $root $prefix) "not signed by" "subject must match exactly"

    # One first-party binary from another publisher is enough.
    $root = New-PackageFixture
    $fixtures += $root
    Copy-Item -LiteralPath $MicrosoftSource -Destination (Join-Path $root "sak_extra.dll")
    Assert-Refused (Invoke-Verifier $root $chocoSubject) "sak_extra.dll" "any sak_ binary is pinned"

    # Bundled third-party tools keep their own vendor's signature: choco.exe must stay
    # Chocolatey-signed, so a pin over the whole package could never pass.
    $root = New-PackageFixture
    $fixtures += $root
    New-Item -ItemType Directory -Path (Join-Path $root "tools") | Out-Null
    Copy-Item -LiteralPath $MicrosoftSource -Destination (Join-Path $root "tools\vendor.dll")
    Assert-Accepted (Invoke-Verifier $root $chocoSubject) "third-party binary with its own signature"

    # A tree without the package's binaries is not the package.
    $root = New-PackageFixture -Names @("sak_utility.exe")
    $fixtures += $root
    Assert-Refused (Invoke-Verifier $root $chocoSubject) "First-party binaries missing" "missing binaries"

    # Unsigned binaries are still refused.
    $root = New-PackageFixture
    $fixtures += $root
    [IO.File]::WriteAllBytes((Join-Path $root "unsigned.dll"), [byte[]](0x4D, 0x5A, 0, 0))
    Assert-Refused (Invoke-Verifier $root $chocoSubject) "Unsigned or invalid" "unsigned binary"
}
finally {
    foreach ($fixture in $fixtures) {
        Remove-Item -LiteralPath $fixture -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "Authenticode publisher pin tests passed."

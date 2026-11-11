# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    Proves sign-exe.ps1 checks WHO signed the dlib it runs and the exe it produced.

.DESCRIPTION
    sign-exe.ps1 accepted any valid signature on the Artifact Signing dlib, which signtool runs
    with signing rights, and any valid signature on the exe it had just signed. A signature cannot
    be faked for a test, so the fixtures are real signed files: the bundled choco.exe (Chocolatey
    Software) and a Windows system DLL (Microsoft Windows).

    1. The checks themselves (authenticode_signer.ps1), against those files.
    2. sign-exe.ps1 run for real in a sandbox, up to its dlib check: its own copy beside a stand-in
       sign-config.ps1 and a stand-in az, with a Chocolatey-signed file as the dlib. It must stop
       there. With a Microsoft-signed file as the dlib it must get past the check, to the step
       after it, which the sandbox makes fail (a folder where it writes metadata.json), so signtool
       never runs and nothing is signed.
    3. The exe check is reached only after a real signing, so its wiring is read from the script:
       the exe sign-exe.ps1 signed is checked against the publisher verify_authenticode_signatures.ps1
       requires of every packaged first-party binary.
#>

[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$Scripts = Join-Path $ProjectRoot "scripts"
. (Join-Path $Scripts "authenticode_signer.ps1")
$ChocoSource = Join-Path $ProjectRoot "tools\chocolatey\choco.exe"
$HostExecutable = (Get-Process -Id $PID).Path

function Assert-True([bool]$Condition, [string]$Case) {
    if (-not $Condition) {
        throw "FAILED: $Case"
    }
}

$MicrosoftSource = @("kernel32.dll", "ucrtbase.dll", "ntdll.dll") |
    ForEach-Object { Join-Path $env:windir "System32\$_" } |
    Where-Object {
        (Test-Path -LiteralPath $_ -PathType Leaf) -and
        ((Get-AuthenticodeSignature -LiteralPath $_).SignerCertificate.Subject -cin $MicrosoftSignerSubjects)
    } |
    Select-Object -First 1
Assert-True (-not [string]::IsNullOrEmpty($MicrosoftSource)) "a Microsoft-signed system DLL is available"
$chocoSubject = (Get-AuthenticodeSignature -LiteralPath $ChocoSource).SignerCertificate.Subject
Assert-True ($chocoSubject -like "*Chocolatey*") "choco.exe is Chocolatey-signed ($chocoSubject)"

$sandboxes = @()
try {
    # -- 1. The checks ----------------------------------------------------------------------
    Assert-True ($null -eq (Get-AuthenticodeSignerRefusal -LiteralPath $MicrosoftSource -AllowedSubjects $MicrosoftSignerSubjects)) `
        "a Microsoft-signed DLL passes as Microsoft"
    $refusal = Get-AuthenticodeSignerRefusal -LiteralPath $ChocoSource -AllowedSubjects $MicrosoftSignerSubjects
    Assert-True ($refusal -like "*is signed by '$chocoSubject'*") "a validly signed non-Microsoft file is refused: $refusal"
    $prefix = $MicrosoftSignerSubjects | ForEach-Object { $_.Substring(0, $_.Length - 1) }
    Assert-True ($null -ne (Get-AuthenticodeSignerRefusal -LiteralPath $MicrosoftSource -AllowedSubjects $prefix)) `
        "a subject that only starts with an allowed one is refused"
    $scratch = Join-Path ([IO.Path]::GetTempPath()) ("sak-signer-test-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $scratch | Out-Null
    $sandboxes += $scratch
    $unsigned = Join-Path $scratch "unsigned.dll"
    [IO.File]::WriteAllBytes($unsigned, [byte[]](0x4D, 0x5A, 0, 0))
    Assert-True ((Get-AuthenticodeSignerRefusal -LiteralPath $unsigned -AllowedSubjects $MicrosoftSignerSubjects) -like "*not validly signed*") `
        "an unsigned file is refused"

    $refusal = Get-SignedExeRefusal -LiteralPath $ChocoSource -Publisher $SakPublisherSubject
    Assert-True ($refusal -like "*not by $SakPublisherSubject*") "an exe signed by someone else is refused: $refusal"
    Assert-True ($null -eq (Get-SignedExeRefusal -LiteralPath $ChocoSource -Publisher $chocoSubject)) `
        "a timestamped exe signed by the expected publisher passes"

    # -- 2. sign-exe.ps1 stops at a dlib Microsoft did not sign --------------------------------
    function Invoke-SignExeSandbox([string]$DlibSource) {
        $root = Join-Path ([IO.Path]::GetTempPath()) ("sak-sign-exe-test-" + [guid]::NewGuid().ToString("N"))
        $script:sandboxes += $root
        $sandboxScripts = Join-Path $root "scripts"
        $dlibDir = Join-Path $root "build\artifact-signing-client\1.0.146\bin\x64"
        $stubs = Join-Path $root "stubs"
        New-Item -ItemType Directory -Path $sandboxScripts, $dlibDir, $stubs | Out-Null
        Copy-Item -LiteralPath (Join-Path $Scripts "sign-exe.ps1"), (Join-Path $Scripts "authenticode_signer.ps1") `
            -Destination $sandboxScripts
        Set-Content -LiteralPath (Join-Path $sandboxScripts "sign-config.ps1") `
            -Value '$SubscriptionId = "00000000-0000-0000-0000-000000000001"' -Encoding ASCII
        # The stand-in az: logged in, any subscription. Nothing reaches Azure.
        Set-Content -LiteralPath (Join-Path $stubs "az.cmd") -Value "@exit /b 0" -Encoding ASCII
        Copy-Item -LiteralPath $DlibSource -Destination (Join-Path $dlibDir "Azure.CodeSigning.Dlib.dll")
        # The step after the dlib check writes this file; a folder in its place stops the script
        # there, before signtool.
        New-Item -ItemType Directory -Path (Join-Path $root "build\artifact-signing-client\1.0.146\metadata.json") | Out-Null
        $target = Join-Path $root "target.exe"
        Copy-Item -LiteralPath $ChocoSource -Destination $target
        $before = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
        $savedPath = $env:Path
        # The child's error stream is its answer, read as text: under Windows PowerShell 5.1 a
        # redirected native error line would otherwise stop this script.
        $ErrorActionPreference = "Continue"
        try {
            $env:Path = "$stubs;$savedPath"
            $output = & $HostExecutable -NoProfile -NonInteractive -ExecutionPolicy Bypass `
                -File (Join-Path $sandboxScripts "sign-exe.ps1") -ExePath $target 2>&1 | Out-String
            $code = $LASTEXITCODE
        } finally {
            $env:Path = $savedPath
        }
        $after = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
        return [pscustomobject]@{ ExitCode = $code; Output = $output; Unchanged = ($before -eq $after) }
    }

    $foreign = Invoke-SignExeSandbox $ChocoSource
    Assert-True ($foreign.ExitCode -ne 0) "sign-exe.ps1 refuses a dlib Microsoft did not sign`n$($foreign.Output)"
    Assert-True ($foreign.Output -match [regex]::Escape("Refusing the Artifact Signing client")) `
        "the refusal is the dlib signer check`n$($foreign.Output)"
    Assert-True ($foreign.Output -notmatch "=== Signing executable ===") "signtool never ran`n$($foreign.Output)"
    Assert-True $foreign.Unchanged "the exe was not touched"

    $microsoft = Invoke-SignExeSandbox $MicrosoftSource
    Assert-True ($microsoft.Output -notmatch [regex]::Escape("Refusing the Artifact Signing client")) `
        "a Microsoft-signed dlib passes the check`n$($microsoft.Output)"
    Assert-True ($microsoft.Output -match "metadata\.json") `
        "sign-exe.ps1 went on to the step after the check`n$($microsoft.Output)"
    Assert-True ($microsoft.Output -notmatch "=== Signing executable ===") "signtool never ran`n$($microsoft.Output)"
    Assert-True ($microsoft.ExitCode -ne 0 -and $microsoft.Unchanged) "nothing was signed`n$($microsoft.Output)"

    # -- 3. The exe check: wired after signtool, against the packaged binaries' publisher ------
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $Scripts "sign-exe.ps1"), [ref]$tokens, [ref]$errors)
    Assert-True ($errors.Count -eq 0) "sign-exe.ps1 parses"
    $commands = @($ast.FindAll({ param($node) $node -is [Management.Automation.Language.CommandAst] }, $true))
    $signtool = @($commands | Where-Object { $_.Extent.Text -match '^\&\s*\$signtool\s+sign\b' })
    $exeCheck = @($commands | Where-Object {
            $_.GetCommandName() -eq "Get-SignedExeRefusal" -and
            $_.Extent.Text -match '-LiteralPath\s+\$ExePath\b' -and
            $_.Extent.Text -match '-Publisher\s+\$SakPublisherSubject\b'
        })
    Assert-True ($signtool.Count -eq 1) "sign-exe.ps1 runs signtool sign once"
    Assert-True ($exeCheck.Count -eq 1) "sign-exe.ps1 checks the signed exe's publisher"
    Assert-True ($exeCheck[0].Extent.StartOffset -gt $signtool[0].Extent.StartOffset) "the publisher is checked after signing"
    $verifier = [Management.Automation.Language.Parser]::ParseFile((Join-Path $Scripts "verify_authenticode_signatures.ps1"), [ref]$tokens, [ref]$errors)
    $pinned = $verifier.ParamBlock.Parameters | Where-Object { $_.Name.VariablePath.UserPath -eq "ExpectedPublisher" }
    Assert-True ($pinned.DefaultValue.Value -ceq $SakPublisherSubject) `
        "sign-exe.ps1 signs as the publisher the package verifier requires ($($pinned.DefaultValue.Value))"
}
finally {
    foreach ($sandbox in $sandboxes) {
        Remove-Item -LiteralPath $sandbox -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "sign-exe.ps1 signer checks passed."

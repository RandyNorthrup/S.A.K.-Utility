# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$verifier = Join-Path $ProjectRoot "scripts\verify_windows_runtime_dependencies.ps1"
$releaseRoot = Join-Path $ProjectRoot "build\Release"

function Assert-RuntimeContract {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Get-RuntimeAst {
    param([string]$Text)
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseInput($Text, [ref]$tokens, [ref]$errors)
    Assert-RuntimeContract ($errors.Count -eq 0) "Runtime verifier source does not parse"
    return $ast
}

function Test-RuntimePackageContracts {
    param([string]$Text, [string]$FixtureRoot)
    $ast = Get-RuntimeAst $Text
    foreach ($name in @("Test-IsSystemDll", "Assert-NoPackageSystemDlls", "Get-PeMachine")) {
        $nodes = @($ast.FindAll({ param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name
        }, $true))
        Assert-RuntimeContract ($nodes.Count -eq 1) "Runtime function missing: $name"
        . ([scriptblock]::Create($nodes[0].Extent.Text))
    }
    foreach ($name in @("icuuc.dll", "ICUIN.DLL", "icu.dll", "VirtDisk.dll", "Authz.dll", "Mpr.dll", "Ntdll.dll",
            "d3d9.dll", "d3d11.dll", "d3d12.dll", "dwrite.dll", "ncrypt.dll", "uiautomationcore.dll", "wtsapi32.dll",
            "imagehlp.dll", "msvcrt.dll", "rpcrt4.dll")) {
        Assert-RuntimeContract (Test-IsSystemDll $name) "Windows-provided DLL classified as private: $name"
    }
    foreach ($name in @("icuuc78.dll", "icuin78.dll", "icudt78.dll", "icuuc-custom.dll", "Qt6Core.dll",
            "d3d9-custom.dll", "d3d11-custom.dll", "d3d12core.dll", "dwrite-private.dll", "ncrypt-custom.dll",
            "uiautomationcore-custom.dll", "wtsapi32-helper.dll", "D3Dcompiler_47.dll", "imagehlp-custom.dll",
            "msvcrt-extra.dll", "msvcr100.dll", "rpcrt4-private.dll")) {
        Assert-RuntimeContract (-not (Test-IsSystemDll $name)) "Private runtime incorrectly classified as Windows: $name"
    }
    $directory = Join-Path $FixtureRoot ([guid]::NewGuid().ToString("D"))
    New-Item -ItemType Directory -Path $directory -ErrorAction Stop | Out-Null
    Assert-NoPackageSystemDlls $directory
    foreach ($name in @("icuuc.dll", "icuin.dll", "icu.dll", "VirtDisk.dll", "d3d9.dll", "d3d11.dll",
            "d3d12.dll", "dwrite.dll", "ncrypt.dll", "uiautomationcore.dll", "wtsapi32.dll", "imagehlp.dll",
            "msvcrt.dll", "rpcrt4.dll")) {
        $path = Join-Path $directory $name
        [IO.File]::WriteAllText($path, "ordinary OS-name test fixture")
        $before = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        $errorMessage = ""
        try { Assert-NoPackageSystemDlls $directory } catch { $errorMessage = $_.Exception.Message }
        Assert-RuntimeContract ($errorMessage.Contains("app-local Windows system DLL")) "Package accepted OS shim: $name"
        Assert-RuntimeContract ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ceq $before) "Package guard changed a file"
        Remove-Item -LiteralPath $path -Force -ErrorAction Stop
    }
    $hidden = Join-Path $directory "icuuc.dll"
    [IO.File]::WriteAllText($hidden, "hidden OS-name test fixture")
    [IO.File]::SetAttributes($hidden, [IO.FileAttributes]::Hidden)
    $errorMessage = ""
    try { Assert-NoPackageSystemDlls $directory } catch { $errorMessage = $_.Exception.Message }
    Assert-RuntimeContract ($errorMessage.Contains("app-local Windows system DLL")) "Package accepted hidden OS shim"
    [IO.File]::SetAttributes($hidden, [IO.FileAttributes]::Normal)
    Remove-Item -LiteralPath $hidden -Force -ErrorAction Stop

    # Exercise the actual dependency loop: a versioned private ICU import remains required.
    $loops = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.ForEachStatementAst] -and
            $node.Variable.Extent.Text -ceq '$dll' -and $node.Condition.Extent.Text -ceq '$dependencies'
    }, $true))
    Assert-RuntimeContract ($loops.Count -eq 1) "Expected one production dependency loop"
    $dependencyLoop = [scriptblock]::Create($loops[0].Extent.Text)
    $dependencies = @("icuuc78.dll")
    $root = [pscustomobject]@{ Path = $directory }
    $exeDir = $directory
    $exeMachine = 0x8664
    $missing = @()
    . $dependencyLoop
    Assert-RuntimeContract ($missing.Count -eq 1 -and $missing[0] -ceq "icuuc78.dll") "Missing private ICU dependency was ignored"
    $privatePath = Join-Path $directory "icuuc78.dll"
    $header = New-Object byte[] 128
    $header[0] = 0x4d
    $header[1] = 0x5a
    [BitConverter]::GetBytes([int]64).CopyTo($header, 60)
    $header[64] = 0x50
    $header[65] = 0x45
    $header[68] = 0x64
    $header[69] = 0x86
    [IO.File]::WriteAllBytes($privatePath, $header)
    $missing = @()
    . $dependencyLoop
    Assert-RuntimeContract ($missing.Count -eq 0) "Present x64 private ICU dependency was rejected"
    Assert-NoPackageSystemDlls $directory
    $header[69] = 0xaa
    [IO.File]::WriteAllBytes($privatePath, $header)
    $missing = @()
    . $dependencyLoop
    Assert-RuntimeContract ($missing.Count -eq 1 -and $missing[0].Contains("wrong architecture")) "ARM64 private ICU passed x64 architecture check"

    # Execute only the package-mode call site, never the verifier's native-tool main body.
    $guards = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.IfStatementAst] -and
            $node.Clauses[0].Item1.Extent.Text.Contains('$RequireNoSystemDlls')
    }, $true))
    Assert-RuntimeContract ($guards.Count -eq 1) "Strict package call site is missing"
    $guard = [scriptblock]::Create($guards[0].Extent.Text)
    [IO.File]::WriteAllText($hidden, "OS-name call-site fixture")
    $resolvedExe = Join-Path $directory "fixture.exe"
    $RequireNoSystemDlls = $false
    . $guard
    $RequireNoSystemDlls = $true
    $errorMessage = ""
    try { . $guard } catch { $errorMessage = $_.Exception.Message }
    Assert-RuntimeContract ($errorMessage.Contains("app-local Windows system DLL")) "Strict package mode did not refuse OS shim"
}

foreach ($required in @($verifier, (Join-Path $releaseRoot "sak_utility.exe"))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Runtime-dependency regression test input missing: $required"
    }
}

$fixtureRoot = Join-Path $ProjectRoot ("build\test_results\runtime_dependencies\" + [guid]::NewGuid().ToString("D"))
New-Item -ItemType Directory -Path $fixtureRoot -ErrorAction Stop | Out-Null
$classifier = Join-Path $ProjectRoot "scripts\windows_runtime_dlls.ps1"
$verifierText = (Get-Content -LiteralPath $verifier -Raw) + [Environment]::NewLine +
    (Get-Content -LiteralPath $classifier -Raw)
$beforeHash = (Get-FileHash -LiteralPath $verifier -Algorithm SHA256).Hash
$classifierBeforeHash = (Get-FileHash -LiteralPath $classifier -Algorithm SHA256).Hash
Test-RuntimePackageContracts $verifierText $fixtureRoot
Write-Host "PASS Windows ICU/package contracts and private ICU dependency loop"
$mutations = @(
    @{ name = "icuuc-system-classification"; from = '        "icuuc.dll",'; to = ''; expected = "classified as private" },
    @{ name = "icuin-system-classification"; from = '        "icuin.dll",'; to = ''; expected = "classified as private" },
    @{ name = "icu-system-classification"; from = '        "icu.dll",'; to = ''; expected = "classified as private" },
    @{ name = "d3d9-system-classification"; from = '        "d3d9.dll",'; to = ''; expected = "classified as private: d3d9.dll" },
    @{ name = "d3d11-system-classification"; from = '        "d3d11.dll",'; to = ''; expected = "classified as private: d3d11.dll" },
    @{ name = "d3d12-system-classification"; from = '        "d3d12.dll",'; to = ''; expected = "classified as private: d3d12.dll" },
    @{ name = "dwrite-system-classification"; from = '        "dwrite.dll",'; to = ''; expected = "classified as private: dwrite.dll" },
    @{ name = "ncrypt-system-classification"; from = '        "ncrypt.dll",'; to = ''; expected = "classified as private: ncrypt.dll" },
    @{ name = "uiautomation-system-classification"; from = '        "uiautomationcore.dll",'; to = ''; expected = "classified as private: uiautomationcore.dll" },
    @{ name = "wtsapi32-system-classification"; from = '        "wtsapi32.dll",'; to = ''; expected = "classified as private: wtsapi32.dll" },
    @{ name = "imagehlp-system-classification"; from = '        "imagehlp.dll",'; to = ''; expected = "classified as private: imagehlp.dll" },
    @{ name = "msvcrt-system-classification"; from = '        "msvcrt.dll",'; to = ''; expected = "classified as private: msvcrt.dll" },
    @{ name = "rpcrt4-system-classification"; from = '        "rpcrt4.dll",'; to = ''; expected = "classified as private: rpcrt4.dll" },
    @{ name = "private-icu-required"; from = 'return $systemDlls -contains $lower'; to = 'return ($systemDlls -contains $lower) -or $lower.StartsWith("icu")'; expected = "Private runtime incorrectly" },
    @{ name = "package-os-shim-refusal"; from = 'if ($leaked.Count -gt 0)'; to = 'if ($leaked.Count -lt 0)'; expected = "Package accepted OS shim" },
    @{ name = "hidden-os-shim-refusal"; from = '-Filter "*.dll" -File -Force -ErrorAction Stop'; to = '-Filter "*.dll" -File -ErrorAction Stop'; expected = "hidden OS shim" },
    @{ name = "strict-package-call-site"; from = 'if ($RequireNoSystemDlls)'; to = 'if ($RequireNoSystemDlls -and -not $RequireNoSystemDlls)'; expected = "Strict package mode" }
    @{ name = "pe-machine-high-byte"; from = '([int]$peHeader[4] -bor ([int]$peHeader[5] -shl 8))'; to = '($peHeader[4] -bor ($peHeader[5] -shl 8))'; expected = "Present x64 private ICU dependency was rejected" }
    @{ name = "same-low-byte-architecture"; from = 'elseif ($machine -ne $exeMachine)'; to = 'elseif (($machine -band 0xff) -ne ($exeMachine -band 0xff))'; expected = "ARM64 private ICU passed" }
)
foreach ($mutation in $mutations) {
    Assert-RuntimeContract ($verifierText.Contains($mutation.from)) "Mutation anchor missing: $($mutation.name)"
    $errorMessage = ""
    try { Test-RuntimePackageContracts ($verifierText.Replace($mutation.from, $mutation.to)) $fixtureRoot } catch { $errorMessage = $_.Exception.Message }
    Assert-RuntimeContract ($errorMessage.Contains($mutation.expected)) "Mutation survived or failed for an unrelated reason: $($mutation.name): $errorMessage"
    Write-Host "KILLED $($mutation.name)"
}
Assert-RuntimeContract ((Get-FileHash -LiteralPath $verifier -Algorithm SHA256).Hash -ceq $beforeHash) "Verifier source changed during in-memory drills"
Assert-RuntimeContract ((Get-FileHash -LiteralPath $classifier -Algorithm SHA256).Hash -ceq $classifierBeforeHash) "Classifier source changed during in-memory drills"

$hostPath = (Get-Process -Id $PID).Path
foreach ($executable in @("sak_utility.exe", "partition_manager_live_certifier.exe", "Qt6Core.dll")) {
    & $hostPath -NoProfile -NonInteractive -File $verifier `
        -RootDir $releaseRoot -PrimaryExe $executable
    $verifyExit = $LASTEXITCODE
    if ($verifyExit -ne 0) {
        throw "Runtime-dependency verifier rejected $executable (exit $verifyExit)."
    }
}

Write-Host "Runtime-dependency regression tests passed for app, live certifier, QtCore and 19 isolated OS/package guards."
exit 0

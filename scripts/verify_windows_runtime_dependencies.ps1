<#
.SYNOPSIS
    Verifies that a Windows app directory contains the non-system DLLs required
    by an executable.

.DESCRIPTION
    Uses dumpbin /DEPENDENTS to inspect imported DLLs. Windows/system DLLs are
    ignored. Every other imported DLL must exist next to the executable or in the
    package root. This catches release packaging mistakes such as shipping an EXE
    that imports z.dll without including z.dll in the ZIP.
#>

param(
    [Parameter(Mandatory = $true)]
    [string]$RootDir,

    [string]$PrimaryExe = "sak_utility.exe"
)

$ErrorActionPreference = "Stop"

function Find-Dumpbin {
    $fromPath = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($fromPath) {
        return $fromPath.Source
    }

    $roots = @(
        ${env:ProgramFiles},
        ${env:ProgramFiles(x86)}
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }

    $matches = @()
    foreach ($root in $roots) {
        $vsRoot = Join-Path $root "Microsoft Visual Studio\2022"
        if (Test-Path -LiteralPath $vsRoot -PathType Container) {
            $matches += Get-ChildItem -LiteralPath $vsRoot `
                -Filter dumpbin.exe `
                -Recurse `
                -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -like "*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" }
        }
    }

    if ($matches.Count -gt 0) {
        return ($matches | Sort-Object FullName -Descending | Select-Object -First 1).FullName
    }

    throw "dumpbin.exe was not found. Install Visual Studio Build Tools or run from a Developer PowerShell."
}

function Test-IsSystemDll {
    param([string]$Name)

    $lower = $Name.ToLowerInvariant()
    if ($lower -like "api-ms-win-*.dll" -or $lower -like "ext-ms-*.dll") {
        return $true
    }

    $systemDlls = @(
        "advapi32.dll",
        "bcrypt.dll",
        "comdlg32.dll",
        "crypt32.dll",
        "dnsapi.dll",
        "dwmapi.dll",
        "dxgi.dll",
        "gdi32.dll",
        "imm32.dll",
        "iphlpapi.dll",
        "kernel32.dll",
        "msimg32.dll",
        "netapi32.dll",
        "ole32.dll",
        "oleacc.dll",
        "oleaut32.dll",
        "powrprof.dll",
        "rstrtmgr.dll",
        "secur32.dll",
        "setupapi.dll",
        "shell32.dll",
        "shlwapi.dll",
        "user32.dll",
        "userenv.dll",
        "uxtheme.dll",
        "version.dll",
        "winhttp.dll",
        "winmm.dll",
        "wlanapi.dll",
        "ws2_32.dll"
    )

    return $systemDlls -contains $lower
}

$root = Resolve-Path -LiteralPath $RootDir
$exePath = Join-Path $root.Path $PrimaryExe
if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
    throw "Primary executable not found: $exePath"
}

$dumpbin = Find-Dumpbin
Write-Host "Verifying runtime dependencies for $PrimaryExe"
Write-Host "Using dumpbin: $dumpbin"

$output = & $dumpbin /DEPENDENTS $exePath 2>&1
if ($LASTEXITCODE -ne 0) {
    $text = $output | Out-String
    throw "dumpbin failed for $exePath`n$text"
}

$dependencies = @()
foreach ($line in $output) {
    if ($line -match '^\s*([A-Za-z0-9_.+-]+\.dll)\s*$') {
        $dependencies += $matches[1]
    }
}

$dependencies = $dependencies | Sort-Object -Unique
$missing = @()
$exeDir = Split-Path -Parent $exePath

foreach ($dll in $dependencies) {
    if (Test-IsSystemDll $dll) {
        continue
    }

    $nextToExe = Join-Path $exeDir $dll
    $inRoot = Join-Path $root.Path $dll
    if ((Test-Path -LiteralPath $nextToExe -PathType Leaf) -or
        (Test-Path -LiteralPath $inRoot -PathType Leaf)) {
        Write-Host "OK  $dll"
    } else {
        $missing += $dll
    }
}

if ($missing.Count -gt 0) {
    Write-Host ""
    Write-Host "Missing runtime DLL(s):" -ForegroundColor Red
    $missing | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 1
}

Write-Host "Runtime dependency verification passed."

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
    [ValidateNotNullOrEmpty()]
    [string]$RootDir,

    [ValidateNotNullOrEmpty()]
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

    # NOT $matches: that is a PowerShell automatic variable, rewritten by every -match
    # and every switch -regex in scope. Anything added here could be silently discarded
    # by an unrelated regex, and the search would then report dumpbin as not found.
    $dumpbinCandidates = @()
    foreach ($root in $roots) {
        $vsRoot = Join-Path $root "Microsoft Visual Studio\2022"
        if (Test-Path -LiteralPath $vsRoot -PathType Container) {
            $dumpbinCandidates += Get-ChildItem -LiteralPath $vsRoot `
                -Filter dumpbin.exe `
                -Recurse `
                -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -like "*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" }
        }
    }

    if ($dumpbinCandidates.Count -gt 0) {
        return ($dumpbinCandidates | Sort-Object FullName -Descending | Select-Object -First 1).FullName
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
        # Windows Trust Verification API, System32 only and never redistributable.
        # ChocolateyManager calls WinVerifyTrust to Authenticode-check the installer it
        # is about to run, which is what put this in the import table.
        "wintrust.dll",
        "wlanapi.dll",
        "ws2_32.dll"
    )

    return $systemDlls -contains $lower
}

function Get-PeMachine {
    param([Parameter(Mandatory = $true)][string]$Path)

    # Returns the PE machine word (e.g. 0x8664 for x64), or $null when the file is not a
    # readable PE image. Reads only the header bytes, so large DLLs stay cheap.
    $stream = $null
    try {
        $stream = [System.IO.File]::OpenRead($Path)
    } catch {
        return $null
    }
    try {
        $reader = New-Object System.IO.BinaryReader($stream)
        $dosHeader = $reader.ReadBytes(64)
        if ($dosHeader.Length -lt 64 -or $dosHeader[0] -ne 0x4D -or $dosHeader[1] -ne 0x5A) {
            return $null  # missing the 'MZ' signature
        }
        $peOffset = [System.BitConverter]::ToInt32($dosHeader, 60)  # e_lfanew lives at 0x3C
        if ($peOffset -lt 0 -or ($peOffset + 6) -gt $stream.Length) {
            return $null
        }
        $stream.Position = $peOffset
        $peHeader = $reader.ReadBytes(6)
        if ($peHeader.Length -lt 6 -or $peHeader[0] -ne 0x50 -or $peHeader[1] -ne 0x45 -or
            $peHeader[2] -ne 0 -or $peHeader[3] -ne 0) {
            return $null  # missing the 'PE\0\0' signature
        }
        return ($peHeader[4] -bor ($peHeader[5] -shl 8))  # IMAGE_FILE_HEADER.Machine
    } catch {
        return $null
    } finally {
        $stream.Dispose()
    }
}

$root = Resolve-Path -LiteralPath $RootDir
# PrimaryExe must name an artifact INSIDE the package root. Reject an absolute path or a
# parent-traversal so a crafted value cannot verify an executable outside the package.
if ([System.IO.Path]::IsPathRooted($PrimaryExe) -or $PrimaryExe -match '\.\.[\\/]') {
    throw "PrimaryExe must be a package-relative name without '..' or a drive letter: $PrimaryExe"
}
$exePath = Join-Path $root.Path $PrimaryExe
if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
    throw "Primary executable not found: $exePath"
}
$resolvedExe = (Resolve-Path -LiteralPath $exePath).Path
if (-not $resolvedExe.StartsWith($root.Path, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Primary executable resolves outside the package root: $resolvedExe"
}
$exeMachine = Get-PeMachine -Path $resolvedExe
if ($null -eq $exeMachine) {
    throw "Primary executable is not a readable PE image: $resolvedExe"
}

$dumpbin = Find-Dumpbin
Write-Host "Verifying runtime dependencies for $PrimaryExe"
Write-Host "Using dumpbin: $dumpbin"

$output = & $dumpbin /DEPENDENTS $resolvedExe 2>&1
if ($LASTEXITCODE -ne 0) {
    $text = $output | Out-String
    throw "dumpbin failed for $resolvedExe`n$text"
}

# Parse only WITHIN dumpbin's dependency blocks (regular AND delay-load), rather than scanning
# every line for anything DLL-shaped. Inside a block, accept any legal bare filename (spaces
# and Unicode allowed; path separators are not), and treat a non-blank line that is not a DLL
# name as a format change -> FAIL CLOSED instead of silently dropping a real dependency.
$dependencies = @()
$inSection = $false
$sawDepInSection = $false
foreach ($line in $output) {
    if ($line -match 'Image has the following( delay load)? dependencies:') {
        $inSection = $true
        $sawDepInSection = $false
        continue
    }
    if (-not $inSection) {
        continue
    }
    if ($line -match '^\s*$') {
        if ($sawDepInSection) {
            $inSection = $false
        }
        continue
    }
    if ($line -imatch '^\s+([^\\/:*?"<>|\r\n]+\.dll)\s*$') {
        $dependencies += $matches[1].Trim()
        $sawDepInSection = $true
        continue
    }
    throw "Unexpected line in dumpbin dependency section (format may have changed): $line"
}

$dependencies = $dependencies | Sort-Object -Unique
# A real PE executable always imports at least one DLL (kernel32). Zero parsed dependencies
# means the parse recognized nothing -- fail closed rather than declare a vacuous pass.
if ($dependencies.Count -eq 0) {
    throw "No imported DLLs were parsed from dumpbin output for $resolvedExe (unexpected format or empty result)"
}
$missing = @()
$exeDir = Split-Path -Parent $resolvedExe

foreach ($dll in $dependencies) {
    if (Test-IsSystemDll $dll) {
        continue
    }

    $nextToExe = Join-Path $exeDir $dll
    $inRoot = Join-Path $root.Path $dll
    $found = $null
    if (Test-Path -LiteralPath $nextToExe -PathType Leaf) {
        $found = $nextToExe
    } elseif (Test-Path -LiteralPath $inRoot -PathType Leaf) {
        $found = $inRoot
    }

    if ($null -eq $found) {
        $missing += $dll
        continue
    }

    # Existence alone is not enough: an empty, truncated, non-PE, or wrong-architecture file
    # with the right name would satisfy a bare existence check yet fail to load at runtime.
    # Require a real PE image whose machine matches the primary executable.
    $machine = Get-PeMachine -Path $found
    if ($null -eq $machine) {
        $missing += "$dll (present but not a valid PE image)"
    } elseif ($machine -ne $exeMachine) {
        $missing += ("$dll (present but wrong architecture: 0x{0:X} vs 0x{1:X})" -f $machine, $exeMachine)
    } else {
        Write-Host "OK  $dll"
    }
}

if ($missing.Count -gt 0) {
    Write-Host ""
    Write-Host "Missing runtime DLL(s):" -ForegroundColor Red
    $missing | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 1
}

Write-Host "Runtime dependency verification passed."

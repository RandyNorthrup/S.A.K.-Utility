# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    Proves scripts/check_program_imports.ps1 names an executable and the DLL it cannot load.

.DESCRIPTION
    With a real built program (sak_apfs_writer_cli, linked with /DEPENDENTLOADFLAG:0x1000) in a
    folder of its own: alone it fails, naming its zlib and Qt imports; with the DLLs it names
    copied in from its build folder it passes; with its zlib DLL removed again it fails, naming
    that DLL (zd.dll in a Debug build, as on the 9170b149 nightly). Then the resolution rules
    against a stand-in import table: an import beside the program is followed to its own imports
    and one missing there is named with the DLL that imports it, System32 and API sets resolve,
    and a folder with no executables fails rather than passing having checked nothing. Nothing is
    run but the checker.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$CliPath
)

$ErrorActionPreference = 'Stop'
$checker = Join-Path $PSScriptRoot 'check_program_imports.ps1'
$hostPath = (Get-Process -Id $PID).Path

function Assert-Condition {
    param([Parameter(Mandatory = $true)] [bool]$Condition, [Parameter(Mandatory = $true)] [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Invoke-Checker {
    param([Parameter(Mandatory = $true)] [string]$Directory)
    $output = @(& $hostPath -NoProfile -NonInteractive -File $checker -Directory $Directory 2>&1 | ForEach-Object { "$_" })
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Text = ($output -join "`n") }
}

Assert-Condition (Test-Path -LiteralPath $CliPath -PathType Leaf) "The writer CLI is not built: $CliPath"
$cliFolder = Split-Path -Parent $CliPath
$cliName = Split-Path -Leaf $CliPath
$root = Join-Path ([IO.Path]::GetTempPath()) ("sak-program-imports-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
try {
    # 1. The program alone: its own DLLs are not there.
    Copy-Item -LiteralPath $CliPath -Destination $root
    $alone = Invoke-Checker $root
    Assert-Condition ($alone.ExitCode -eq 1) "The checker passed a program with none of its DLLs (exit $($alone.ExitCode)): $($alone.Text)"
    Assert-Condition ($alone.Text -match "(?m)^$([regex]::Escape($cliName)) imports zd?\.dll, which is neither beside it") "The checker did not name the zlib import: $($alone.Text)"
    Assert-Condition ($alone.Text -match "$([regex]::Escape($cliName)) imports Qt6Cored?\.dll") "The checker did not name the Qt import: $($alone.Text)"
    Write-Host 'PASS a program without its DLLs is named with each one'

    # 2. Its DLLs copied in from its build folder, as many rounds as their own imports need.
    $result = $alone
    for ($round = 0; $round -lt 6 -and $result.ExitCode -ne 0; $round++) {
        $named = @([regex]::Matches($result.Text, 'imports (?<dll>[^ ,]+\.dll)') | ForEach-Object { $_.Groups['dll'].Value } | Sort-Object -Unique)
        Assert-Condition ($named.Count -gt 0) "The checker failed without naming a DLL: $($result.Text)"
        foreach ($dll in $named) {
            $source = Join-Path $cliFolder $dll
            Assert-Condition (Test-Path -LiteralPath $source -PathType Leaf) "The build folder lacks $dll, which $cliName needs: $source"
            Copy-Item -LiteralPath $source -Destination $root -Force
        }
        $result = Invoke-Checker $root
    }
    Assert-Condition ($result.ExitCode -eq 0) "The checker refused a program with every DLL it imports beside it: $($result.Text)"
    Assert-Condition ($result.Text -match 'Checked 1 executables in .*every import resolves') "The checker's pass did not say what it checked: $($result.Text)"
    Write-Host 'PASS a program with its DLLs beside it passes'

    # 3. The zlib DLL removed again: the case the nightly hit.
    $zlib = @(Get-ChildItem -LiteralPath $root -File | Where-Object { $_.Name -match '^zd?\.dll$' })
    Assert-Condition ($zlib.Count -eq 1) "Expected one zlib DLL beside the copy, found $($zlib.Count)"
    Remove-Item -LiteralPath $zlib[0].FullName -Force
    $missing = Invoke-Checker $root
    Assert-Condition ($missing.ExitCode -eq 1) "The checker passed with $($zlib[0].Name) gone: $($missing.Text)"
    Assert-Condition ($missing.Text -match "$([regex]::Escape($cliName)) imports $([regex]::Escape($zlib[0].Name)), which is neither beside it") "The checker did not name $($zlib[0].Name): $($missing.Text)"
    Write-Host "PASS $($zlib[0].Name) removed is named"

    # 4. The rules, against a stand-in import table (the checker's own functions, its file reader
    # replaced).
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($checker, [ref]$tokens, [ref]$errors)
    Assert-Condition ($errors.Count -eq 0) 'The checker does not parse'
    $finder = @($ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Find-MissingImport' }, $true))
    Assert-Condition ($finder.Count -eq 1) 'The checker has no Find-MissingImport'
    . ([scriptblock]::Create($finder[0].Extent.Text))
    $standIns = @{
        'app.exe' = @('beside.dll', 'kernel32.dll', 'api-ms-win-core-synch-l1-2-0.dll', 'BESIDE.DLL')
        'beside.dll' = @('kernel32.dll', 'gone.dll')
        'other.exe' = @('kernel32.dll')
    }
    function Get-PeImportName {
        param([string]$Path)
        return , $standIns[(Split-Path -Leaf $Path).ToLowerInvariant()]
    }
    $folder = Join-Path $root 'standin'
    $system = Join-Path $root 'standin-system32'
    New-Item -ItemType Directory -Path $folder, $system | Out-Null
    foreach ($name in 'app.exe', 'beside.dll', 'other.exe') { Set-Content -LiteralPath (Join-Path $folder $name) -Value '' }
    Set-Content -LiteralPath (Join-Path $system 'kernel32.dll') -Value ''
    # Not wrapped in @(): the function returns its list as one array, as the checker reads it.
    $found = Find-MissingImport -Folder $folder -Executables @('app.exe', 'other.exe') -SystemFolder $system
    Assert-Condition ($found.Count -eq 1) "Expected one missing import, found $($found.Count): $($found | ConvertTo-Json -Compress)"
    Assert-Condition ($found[0].Executable -eq 'app.exe' -and $found[0].Dll -eq 'gone.dll' -and $found[0].ImportedBy -eq 'beside.dll') "The missing import was not named with the DLL that imports it: $($found | ConvertTo-Json -Compress)"
    Set-Content -LiteralPath (Join-Path $system 'gone.dll') -Value ''
    $none = Find-MissingImport -Folder $folder -Executables @('app.exe', 'other.exe') -SystemFolder $system
    Assert-Condition ($none.Count -eq 0) "An import in System32 was not resolved: $($none | ConvertTo-Json -Compress)"
    Write-Host 'PASS imports beside a program are followed; System32 and API sets resolve'

    # 5. No executables: nothing checked is not a pass.
    $empty = Join-Path $root 'empty'
    New-Item -ItemType Directory -Path $empty | Out-Null
    $nothing = Invoke-Checker $empty
    Assert-Condition ($nothing.ExitCode -eq 1 -and $nothing.Text -match 'nothing was checked') "A folder with no executables passed: $($nothing.Text)"
    Write-Host 'PASS a folder with no executables fails'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host 'check_program_imports: all checks passed'

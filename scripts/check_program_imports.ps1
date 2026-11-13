# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    Fails when an executable in a build output folder imports a DLL that is neither beside it nor
    in System32, naming the executable and the DLL.

.DESCRIPTION
    sak_utility, the elevated helper and both writer CLIs are linked with /DEPENDENTLOADFLAG:0x1000:
    Windows resolves their imports only from their own folder and System32, never PATH. A Debug
    build that staged only the Release names of vcpkg's DLLs could not start them, and under ctest
    the missing import was a Windows error box that waited until the nightly's 330-minute limit
    (9170b149, 2026-11-12). The test executables sit in the same folder and are held to the same
    rule, so a missing import is found here, before any test runs, whatever error mode the tests
    get.

    Every .exe directly in the folder is read (its import and delay-import tables, straight from the
    file, so no dumpbin is needed), and so is every DLL beside it that it imports, transitively. An
    import resolves when the file is in the folder or in System32, or is an API set
    (api-ms-*, ext-ms-*). The bundled tools under tools\ are not the build's and are not read.

.PARAMETER Directory
    The build output folder: build\Release, or build\Debug for a Debug build.

.EXAMPLE
    scripts/check_program_imports.ps1 -Directory build/Release
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$Directory
)

$ErrorActionPreference = 'Stop'

# The names one PE file imports, its delay-loaded ones included, read from the file itself.
function Get-PeImportName {
    param([Parameter(Mandatory = $true)] [string]$Path)
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    try {
        $reader = New-Object IO.BinaryReader($stream)
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw "$Path is not a PE file (no MZ header)" }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadInt32()
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) { throw "$Path is not a PE file (no PE signature)" }
        $stream.Position = $peOffset + 6
        $sectionCount = $reader.ReadUInt16()
        $stream.Position = $peOffset + 20
        $optionalSize = $reader.ReadUInt16()
        $optionalStart = $peOffset + 24
        $stream.Position = $optionalStart
        $magic = $reader.ReadUInt16()
        if ($magic -eq 0x20B) {
            $stream.Position = $optionalStart + 24
            $imageBase = $reader.ReadUInt64()
            $directoryStart = $optionalStart + 112
        } elseif ($magic -eq 0x10B) {
            $stream.Position = $optionalStart + 28
            $imageBase = [uint64]$reader.ReadUInt32()
            $directoryStart = $optionalStart + 96
        } else {
            throw "$Path has an optional header this does not read (magic 0x$('{0:X}' -f $magic))"
        }
        $stream.Position = $directoryStart - 4
        $directoryCount = $reader.ReadUInt32()
        $sections = @(for ($index = 0; $index -lt $sectionCount; $index++) {
            $stream.Position = $optionalStart + $optionalSize + 40 * $index + 8
            $virtualSize = $reader.ReadUInt32()
            $virtualAddress = $reader.ReadUInt32()
            $rawSize = $reader.ReadUInt32()
            $rawPointer = $reader.ReadUInt32()
            [pscustomobject]@{ Address = $virtualAddress; Size = [Math]::Max($virtualSize, $rawSize); Raw = $rawPointer }
        })
        $toOffset = {
            param([uint64]$Rva)
            foreach ($section in $sections) {
                if ($Rva -ge $section.Address -and $Rva -lt $section.Address + $section.Size) {
                    return [int64]($Rva - $section.Address + $section.Raw)
                }
            }
            throw "$Path points outside its sections (RVA 0x$('{0:X}' -f $Rva))"
        }
        $readName = {
            param([uint64]$Rva)
            $stream.Position = & $toOffset $Rva
            $bytes = New-Object Collections.Generic.List[byte]
            for ($count = 0; $count -lt 260; $count++) {
                $next = $stream.ReadByte()
                if ($next -le 0) { break }
                $bytes.Add([byte]$next)
            }
            [Text.Encoding]::ASCII.GetString($bytes.ToArray())
        }
        $names = New-Object Collections.Generic.List[string]
        $tables = @(@{ Index = 1; Entry = 20; NameAt = 12; Delay = $false }, @{ Index = 13; Entry = 32; NameAt = 4; Delay = $true })
        foreach ($table in $tables) {
            if ($directoryCount -le $table.Index) { continue }
            $stream.Position = $directoryStart + 8 * $table.Index
            $tableRva = $reader.ReadUInt32()
            if ($tableRva -eq 0) { continue }
            $entryOffset = & $toOffset $tableRva
            for ($entry = 0; $entry -lt 4096; $entry++) {
                $stream.Position = $entryOffset + $table.Entry * $entry
                $attributes = $reader.ReadUInt32()
                $stream.Position = $entryOffset + $table.Entry * $entry + $table.NameAt
                $nameRva = [uint64]$reader.ReadUInt32()
                if ($nameRva -eq 0) { break }
                # An old-style delay descriptor (attribute bit 0 clear) holds addresses, not RVAs.
                if ($table.Delay -and ($attributes -band 1) -eq 0) { $nameRva -= $imageBase }
                $names.Add((& $readName $nameRva))
            }
        }
        return , $names.ToArray()
    } finally {
        $stream.Dispose()
    }
}

# Every import of each executable, transitively through the DLLs beside it, that resolves neither
# beside it nor in System32. Returns one record per missing DLL and the file that imports it.
function Find-MissingImport {
    param(
        [Parameter(Mandatory = $true)] [string]$Folder,
        [Parameter(Mandatory = $true)] [string[]]$Executables,
        [Parameter(Mandatory = $true)] [string]$SystemFolder
    )
    $importsOf = @{}
    $missing = New-Object Collections.Generic.List[object]
    foreach ($executable in $Executables) {
        $seen = @{}
        $queue = New-Object Collections.Generic.Queue[string]
        $queue.Enqueue($executable)
        while ($queue.Count -gt 0) {
            $file = $queue.Dequeue()
            $key = $file.ToLowerInvariant()
            if (-not $importsOf.ContainsKey($key)) { $importsOf[$key] = Get-PeImportName -Path (Join-Path $Folder $file) }
            foreach ($import in $importsOf[$key]) {
                $importKey = $import.ToLowerInvariant()
                if ($seen.ContainsKey($importKey)) { continue }
                $seen[$importKey] = $true
                if (Test-Path -LiteralPath (Join-Path $Folder $import) -PathType Leaf) {
                    $queue.Enqueue($import)
                } elseif ($importKey -match '^(api|ext)-ms-') {
                    continue
                } elseif (-not (Test-Path -LiteralPath (Join-Path $SystemFolder $import) -PathType Leaf)) {
                    $missing.Add([pscustomobject]@{ Executable = $executable; Dll = $import; ImportedBy = $file })
                }
            }
        }
    }
    return , $missing.ToArray()
}

$folder = (Resolve-Path -LiteralPath $Directory).Path
$executables = @(Get-ChildItem -LiteralPath $folder -Filter '*.exe' -File | Sort-Object Name | ForEach-Object { $_.Name })
if ($executables.Count -eq 0) {
    [Console]::Error.WriteLine("No executables in ${folder}: nothing was checked.")
    exit 1
}
$system = Join-Path $env:SystemRoot 'System32'
$found = Find-MissingImport -Folder $folder -Executables $executables -SystemFolder $system
foreach ($record in $found) {
    $through = if ($record.ImportedBy -ieq $record.Executable) { '' } else { " (imported by $($record.ImportedBy))" }
    [Console]::Error.WriteLine("$($record.Executable) imports $($record.Dll)$through, which is neither beside it in $folder nor in $system")
}
if ($found.Count -gt 0) {
    [Console]::Error.WriteLine("$($found.Count) missing import(s) in $($executables.Count) executables: these programs cannot start.")
    exit 1
}
Write-Host "Checked $($executables.Count) executables in ${folder}: every import resolves beside them or in System32."
exit 0

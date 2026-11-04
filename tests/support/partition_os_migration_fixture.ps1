# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

# Runs what can run of the Migrate OS and attached-disk repair scripts with no disk involved: parses
# them, compiles their C#, runs the copy code on ordinary files in -WorkFolder, and runs the pure
# functions on plain objects. It opens no device and never runs the scripts' main flow.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$MigrationScript,
    [Parameter(Mandatory = $true)] [string]$RepairScript,
    [Parameter(Mandatory = $true)] [string]$WorkFolder
)

$ErrorActionPreference = 'Stop'

function Assert-Fixture([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Get-ScriptFunctionTextOrNull($Ast, [string]$Name) {
    $found = @($Ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $Name }, $true))
    if ($found.Count -eq 1) { return $found[0].Extent.Text }
    return $null
}

function Get-ParsedScript([string]$Path) {
    $errors = $null
    $tokens = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
    Assert-Fixture ($errors.Count -eq 0) "$Path does not parse: $($errors | Select-Object -First 3)"
    return $ast
}

$migrationAst = Get-ParsedScript $MigrationScript
$repairAst = Get-ParsedScript $RepairScript
foreach ($name in 'Invoke-SakTargetBootFiles', 'Set-SakTargetDriveLetters', 'Find-SakAttachedWindows', 'Get-SakWindowsOwnLetter') {
    Assert-Fixture ($null -ne (Get-ScriptFunctionTextOrNull $repairAst $name)) "The repair script has no $name"
}

# The C# in the script, compiled by the compiler Windows PowerShell 5.1 has (C# 5).
$text = [IO.File]::ReadAllText($MigrationScript)
$match = [regex]::Match($text, "Add-Type -TypeDefinition @'\r?\n(.*?)\r?\n'@", 'Singleline')
Assert-Fixture $match.Success 'The migration script has no C# block'
Add-Type -TypeDefinition $match.Groups[1].Value

function Get-ScriptFunctionText($Ast, [string]$Name) {
    $found = @($Ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $Name }, $true))
    Assert-Fixture ($found.Count -eq 1) "Function $Name appears $($found.Count) times"
    return $found[0].Extent.Text
}

# --- the copy code, on files
$sourcePath = Join-Path $WorkFolder 'source.bin'
$targetPath = Join-Path $WorkFolder 'target.bin'
$bytes = New-Object byte[] (6 * 1048576)
(New-Object Random 11).NextBytes($bytes)
[IO.File]::WriteAllBytes($sourcePath, $bytes)
[IO.File]::WriteAllBytes($targetPath, (New-Object byte[] (8 * 1048576)))
$targetBase = [int64]1048576
$ranges = [int64[]]@(0, 1048576, (3 * 1048576), 2097152)
$source = [SakMigration.Io]::OpenRead($sourcePath)
$target = [SakMigration.Io]::OpenWrite($targetPath)
try {
    [SakMigration.Copy]::Run($source, 0, $target, $targetBase, $ranges, 'Fixture copy')
    [SakMigration.Copy]::Verify($source, 0, $target, $targetBase, $ranges, $true)
    [SakMigration.Copy]::Verify($source, 0, $target, $targetBase, $ranges, $false)
} finally {
    $source.Dispose()
    $target.Dispose()
}
$after = [IO.File]::ReadAllBytes($targetPath)
foreach ($pair in @(@(0, 1048576), @(3145728, 2097152))) {
    for ($i = 0; $i -lt $pair[1]; $i += 65536) {
        Assert-Fixture ($after[$targetBase + $pair[0] + $i] -eq $bytes[$pair[0] + $i]) 'A copied range differs'
    }
}
Assert-Fixture (($after[0..1048575] | Where-Object { $_ -ne 0 } | Select-Object -First 1) -eq $null) 'Copy wrote before the target base'
$gap = $after[(2 * 1048576)..(4 * 1048576 - 1)]
Assert-Fixture (($gap | Where-Object { $_ -ne 0 } | Select-Object -First 1) -eq $null) 'Copy wrote a range it was not given'

# --- a difference is reported, in full and in the sample
$after[$targetBase + 3 * 1048576 + 5] = [byte]($after[$targetBase + 3 * 1048576 + 5] -bxor 0xFF)
[IO.File]::WriteAllBytes($targetPath, $after)
foreach ($full in @($true, $false)) {
    $source = [SakMigration.Io]::OpenRead($sourcePath)
    $target = [SakMigration.Io]::OpenWrite($targetPath)
    $reported = $false
    try { [SakMigration.Copy]::Verify($source, 0, $target, $targetBase, $ranges, $full) } catch { $reported = "$_" -match 'difference' }
    finally { $source.Dispose(); $target.Dispose() }
    Assert-Fixture $reported "Verify (full=$full) did not report a changed byte"
}

# --- an ordinary file is not in the Compact state, and asking does not fail
Assert-Fixture (-not [SakMigration.Io]::IsExternallyBacked($sourcePath)) 'An ordinary file read as externally backed'

# --- what Windows reads to give a volume its drive letter
. ([scriptblock]::Create((Get-ScriptFunctionText $migrationAst 'Get-SakMountedDeviceValue')))
$gptGuid = '{92e38f2a-9b38-4f3d-b65e-8472ca9bbc3b}'
$value = Get-SakMountedDeviceValue ([pscustomobject]@{ PartitionStyle = 'GPT' }) ([pscustomobject]@{ Guid = $gptGuid; Offset = 1 })
Assert-Fixture ($value -ceq '444D494F3A49443A2A8FE392389B3D4FB65E8472CA9BBC3B') "GPT value is $value, as read from a real SYSTEM hive it is 444D494F3A49443A2A8FE392389B3D4FB65E8472CA9BBC3B"
$value = Get-SakMountedDeviceValue ([pscustomobject]@{ PartitionStyle = 'MBR'; Signature = [uint32]3311002703 }) ([pscustomobject]@{ Guid = $null; Offset = [uint64]524288000 })
Assert-Fixture ($value -ceq '4FE459C50000401F00000000') "MBR value is $value"

# --- a partition to be formatted starts hidden, or Windows offers on screen to format it
. ([scriptblock]::Create((Get-ScriptFunctionText $migrationAst 'New-SakLayoutEntry')))
$from = New-Object SakMigration.LayoutEntry
$from.GptType = [Guid]'ebd0a0a2-b9e5-4433-87c0-68b6b72699c7'
$from.MbrType = 7
$from.Name = 'Basic data partition'
$sakScheme = 'GPT'
$plain = New-SakLayoutEntry $from 1048576 2097152 $false
Assert-Fixture ($plain.Attributes -eq 0) 'A partition that keeps its bytes must keep its attributes'
Assert-Fixture ($plain.PartitionId -ne [Guid]::Empty) 'A target partition needs its own identity'
$other = New-SakLayoutEntry $from 1048576 2097152 $false
Assert-Fixture ($plain.PartitionId -ne $other.PartitionId) 'Two target partitions shared an identity'
$hidden = New-SakLayoutEntry $from 1048576 2097152 $true
Assert-Fixture ($hidden.Attributes -eq [uint64]'9223372036854775808') 'A raw GPT partition must carry the no-letter attribute'
$sakScheme = 'MBR'
$hiddenMbr = New-SakLayoutEntry $from 1048576 2097152 $true
Assert-Fixture ($hiddenMbr.MbrType -eq 0x17) 'A raw MBR partition must carry the hidden type'

Write-Output 'OS_MIGRATION_FIXTURE_PASSED'

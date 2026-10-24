# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    Regression test for scripts/run_folder_retention.ps1.

.DESCRIPTION
    The helper deletes directories, so its fences are the subject here: a passing run's folder
    is removed and a junction inside it is unlinked rather than followed; anything that is not
    a pattern-matching direct child of the output root, or that is a reparse point, is refused
    with its target intact; pruning keeps the newest folders, never touches a folder young
    enough to belong to a concurrent run, and ignores names outside the pattern. Registered for
    both PowerShell 7 and Windows PowerShell 5.1 because the two hosts run on different .NET
    directory-deletion implementations.
#>

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'run_folder_retention.ps1')

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Assert-Refused([scriptblock]$Body, [string]$Expected, [string]$Name) {
    $message = $null
    try { & $Body } catch { $message = $_.Exception.Message }
    Assert-True ($null -ne $message -and $message.Contains($Expected)) (
        "$Name was not refused with '$Expected': $message")
    Write-Host "PASS refusal: $Name"
}

function New-RunName { return 'run-' + [guid]::NewGuid().ToString('N') }

$pattern = '^run-[0-9a-f]{32}$'
$base = Join-Path ([IO.Path]::GetTempPath()) ('sak-run-retention-' + [guid]::NewGuid().ToString('N'))
$root = Join-Path $base 'output'
$outside = Join-Path $base 'outside'
New-Item -ItemType Directory -Path $root, $outside -ErrorAction Stop | Out-Null
$canary = Join-Path $outside 'canary.txt'
Set-Content -LiteralPath $canary -Value 'outside canary' -Encoding ascii
try {
    # A passing run's folder goes, and a junction inside it is unlinked, not followed.
    $run = Join-Path $root (New-RunName)
    New-Item -ItemType Directory -Path (Join-Path $run 'nested') -ErrorAction Stop | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $run 'nested\image.bin'), [byte[]]::new(4096))
    $readOnly = Join-Path $run 'nested\report.json'
    Set-Content -LiteralPath $readOnly -Value '{}' -Encoding ascii
    Set-ItemProperty -LiteralPath $readOnly -Name IsReadOnly -Value $true
    New-Item -ItemType Junction -Path (Join-Path $run 'escape') -Target $outside -ErrorAction Stop | Out-Null
    New-Item -ItemType Junction -Path (Join-Path $run 'nested\deep-escape') -Target $outside -ErrorAction Stop | Out-Null
    Remove-TestRunFolder -RunFolder $run -OutputRoot $root -NamePattern $pattern
    Assert-True (-not (Test-Path -LiteralPath $run)) 'Passing run folder survived removal'
    Assert-True ((Get-Content -LiteralPath $canary -Raw).Trim() -ceq 'outside canary') (
        'Removal followed a junction out of the run folder')
    Assert-True (@(Get-ChildItem -LiteralPath $outside -Force).Count -eq 1) (
        'Removal deleted something outside the run folder through a junction')
    Write-Host 'PASS passing run folder removed; read-only file cleared; junctions unlinked, target intact'

    $foreign = Join-Path $outside (New-RunName)
    New-Item -ItemType Directory -Path $foreign -ErrorAction Stop | Out-Null
    Assert-Refused { Remove-TestRunFolder -RunFolder $foreign -OutputRoot $root -NamePattern $pattern } `
        'not a direct child' 'folder outside the output root'
    Assert-Refused { Remove-TestRunFolder -RunFolder (Join-Path $root "..\outside\$(Split-Path -Leaf $foreign)") -OutputRoot $root -NamePattern $pattern } `
        'not a direct child' 'traversal out of the output root'
    $grandchild = Join-Path (Join-Path $root (New-RunName)) (New-RunName)
    New-Item -ItemType Directory -Path $grandchild -ErrorAction Stop | Out-Null
    Assert-Refused { Remove-TestRunFolder -RunFolder $grandchild -OutputRoot $root -NamePattern $pattern } `
        'not a direct child' 'nested run folder'
    Assert-Refused { Remove-TestRunFolder -RunFolder $root -OutputRoot $root -NamePattern $pattern } `
        'not a direct child' 'the output root itself'
    $misnamed = Join-Path $root 'keep-me'
    New-Item -ItemType Directory -Path $misnamed -ErrorAction Stop | Out-Null
    Assert-Refused { Remove-TestRunFolder -RunFolder $misnamed -OutputRoot $root -NamePattern $pattern } `
        'does not match the run-folder pattern' 'name outside the pattern'
    $linkedRun = Join-Path $root (New-RunName)
    New-Item -ItemType Junction -Path $linkedRun -Target $outside -ErrorAction Stop | Out-Null
    Assert-Refused { Remove-TestRunFolder -RunFolder $linkedRun -OutputRoot $root -NamePattern $pattern } `
        'is a reparse point' 'run folder that is a junction'
    Assert-Refused { Remove-TestRunFolder -RunFolder 'C:\run-00000000000000000000000000000000' -OutputRoot 'C:\' -NamePattern $pattern } `
        'refuses a drive root' 'drive root as the output root'
    $linkedRoot = Join-Path $base 'linked-output'
    New-Item -ItemType Junction -Path $linkedRoot -Target $root -ErrorAction Stop | Out-Null
    Assert-Refused { Limit-RetainedTestRunFolders -OutputRoot $linkedRoot -NamePattern $pattern -Keep 0 } `
        'reached through a reparse point' 'output root reached through a junction'
    foreach ($kept in @($foreign, $grandchild, $misnamed, $canary)) {
        Assert-True (Test-Path -LiteralPath $kept) "A refused removal still deleted $kept"
    }
    [IO.Directory]::Delete($linkedRun)
    [IO.Directory]::Delete((Split-Path -Parent $grandchild), $true)
    Write-Host 'PASS every refused target is intact'

    # Pruning keeps the newest folders, skips young ones and ignores names outside the pattern.
    $aged = @()
    foreach ($hoursAgo in 1..5) {
        $folder = Join-Path $root (New-RunName)
        New-Item -ItemType Directory -Path $folder -ErrorAction Stop | Out-Null
        (Get-Item -LiteralPath $folder).CreationTimeUtc = [DateTime]::UtcNow.AddHours(-$hoursAgo)
        $aged += $folder
    }
    $young = Join-Path $root (New-RunName)
    New-Item -ItemType Directory -Path $young -ErrorAction Stop | Out-Null
    $removed = @(Limit-RetainedTestRunFolders -OutputRoot $root -NamePattern $pattern -Keep 3)
    Assert-True ($removed.Count -eq 3) "Prune removed $($removed.Count) folders, expected 3"
    foreach ($folder in @($young, $aged[0], $aged[1], $misnamed)) {
        Assert-True (Test-Path -LiteralPath $folder) "Prune removed a folder it had to keep: $folder"
    }
    foreach ($folder in @($aged[2], $aged[3], $aged[4])) {
        Assert-True (-not (Test-Path -LiteralPath $folder)) "Prune kept a folder beyond the cap: $folder"
    }
    $removed = @(Limit-RetainedTestRunFolders -OutputRoot $root -NamePattern $pattern -Keep 0)
    Assert-True ((Test-Path -LiteralPath $young) -and $removed.Count -eq 2) (
        'Prune removed a folder young enough to belong to a concurrent run')
    Write-Host 'PASS prune keeps the newest, skips young folders, ignores foreign names'
}
finally {
    if (Test-Path -LiteralPath $base) {
        foreach ($link in @(Get-ChildItem -LiteralPath $base -Recurse -Force -Directory -ErrorAction SilentlyContinue |
                Where-Object { ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 })) {
            [IO.Directory]::Delete($link.FullName)
        }
        [IO.Directory]::Delete($base, $true)
    }
}
Write-Host "Run-folder retention tests passed under $($PSVersionTable.PSVersion)."

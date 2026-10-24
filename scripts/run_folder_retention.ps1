# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    Bounded retention for script-test run folders. Dot-source it.

.DESCRIPTION
    The certifier and writer-CLI script tests write one run folder per invocation under their
    -OutputRoot: about 1.3 GB for the APFS writer CLI, 0.7 GB for the probe certifier. Nothing
    removed them, and they once held more than 1 TB under build/test_results.

    A run folder is worth keeping only when its run failed: the images of a passing run prove
    nothing a rerun cannot. So a passing run deletes its own folder (Remove-TestRunFolder),
    and each new run first caps the folders left behind by earlier failed or killed runs to the
    newest few (Limit-RetainedTestRunFolders), so a failure loop cannot fill the disk either.

    Deletion is fenced: the folder must be a direct child of the resolved output root, its name
    must match the caller's run-folder pattern, neither it nor the root may be a reparse point,
    and the root may not be a drive root. Removal walks the tree itself and unlinks any junction
    or symlink it meets without descending into it. [IO.Directory]::Delete(path, $true) is not
    used: on both PowerShell hosts it refuses a folder that contains a junction with "access
    denied", which the registered test proved before this walk replaced it.
#>

$Script:RetainedFailedRunFolders = 3
# A folder this young may belong to a concurrent run of the same test; never prune it.
$Script:RunFolderPruneMinimumAgeMinutes = 30

# Resolve against the PowerShell location, not the process directory: [IO.Path]::GetFullPath
# alone resolves a relative path against the .NET current directory, which a script's
# Set-Location does not move (AGENTS.md, ".NET ignores the shell cwd").
function Resolve-RunFolderFullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    $provider = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
    return [IO.Path]::GetFullPath($provider).TrimEnd('\', '/')
}

function Resolve-RunFolderOutputRoot {
    param([Parameter(Mandatory = $true)][string]$OutputRoot)
    if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
        throw 'Run-folder retention needs a non-empty output root'
    }
    $full = Resolve-RunFolderFullPath $OutputRoot
    if ($full.Length -le 3 -or [string]::IsNullOrEmpty([IO.Path]::GetDirectoryName($full))) {
        throw "Run-folder retention refuses a drive root as the output root: $OutputRoot"
    }
    if (Test-Path -LiteralPath $full) {
        $item = Get-Item -LiteralPath $full -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Run-folder retention refuses an output root reached through a reparse point: $full"
        }
    }
    return $full
}

function Assert-RunFolderDeletable {
    param(
        [Parameter(Mandatory = $true)][string]$RunFolder,
        [Parameter(Mandatory = $true)][string]$OutputRoot,
        [Parameter(Mandatory = $true)][string]$NamePattern
    )
    $root = Resolve-RunFolderOutputRoot $OutputRoot
    $full = Resolve-RunFolderFullPath $RunFolder
    $parent = [IO.Path]::GetDirectoryName($full)
    if (-not [string]::Equals($parent, $root, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Run folder is not a direct child of its output root: $full"
    }
    $leaf = [IO.Path]::GetFileName($full)
    if ($leaf -cnotmatch $NamePattern) {
        throw "Run folder name does not match the run-folder pattern ${NamePattern}: $leaf"
    }
    $item = Get-Item -LiteralPath $full -Force -ErrorAction Stop
    if (-not $item.PSIsContainer) {
        throw "Run folder is not a directory: $full"
    }
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Run folder is a reparse point and will not be deleted: $full"
    }
    return $full
}

# Depth-first removal that never follows a link: a reparse point (junction or symlink) is
# unlinked where it stands, so nothing outside the run folder can be reached through it.
function Remove-DirectoryTreeWithoutFollowingLinks {
    param([Parameter(Mandatory = $true)][string]$Path)
    $directory = [IO.DirectoryInfo]::new($Path)
    foreach ($entry in @($directory.EnumerateFileSystemInfos())) {
        if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            if ($entry -is [IO.DirectoryInfo]) { [IO.Directory]::Delete($entry.FullName) }
            else { [IO.File]::Delete($entry.FullName) }
            continue
        }
        if ($entry -is [IO.DirectoryInfo]) {
            Remove-DirectoryTreeWithoutFollowingLinks $entry.FullName
            continue
        }
        if (($entry.Attributes -band [IO.FileAttributes]::ReadOnly) -ne 0) {
            $entry.Attributes = $entry.Attributes -band (-bnot [IO.FileAttributes]::ReadOnly)
        }
        $entry.Delete()
    }
    $directory.Delete()
}

function Remove-TestRunFolder {
    param(
        [Parameter(Mandatory = $true)][string]$RunFolder,
        [Parameter(Mandatory = $true)][string]$OutputRoot,
        [Parameter(Mandatory = $true)][string]$NamePattern
    )
    $full = Assert-RunFolderDeletable -RunFolder $RunFolder -OutputRoot $OutputRoot -NamePattern $NamePattern
    Remove-DirectoryTreeWithoutFollowingLinks $full
    if (Test-Path -LiteralPath $full) {
        throw "Passing run folder could not be removed: $full"
    }
}

function Limit-RetainedTestRunFolders {
    param(
        [Parameter(Mandatory = $true)][string]$OutputRoot,
        [Parameter(Mandatory = $true)][string]$NamePattern,
        [int]$Keep = $Script:RetainedFailedRunFolders,
        [int]$MinimumAgeMinutes = $Script:RunFolderPruneMinimumAgeMinutes
    )
    $root = Resolve-RunFolderOutputRoot $OutputRoot
    if (-not (Test-Path -LiteralPath $root -PathType Container)) { return @() }
    $folders = @(Get-ChildItem -LiteralPath $root -Directory -Force -ErrorAction Stop |
        Where-Object { $_.Name -cmatch $NamePattern } |
        Sort-Object CreationTimeUtc -Descending)
    $cutoff = [DateTime]::UtcNow.AddMinutes(-$MinimumAgeMinutes)
    $removed = @()
    foreach ($folder in @($folders | Select-Object -Skip $Keep)) {
        if ($folder.CreationTimeUtc -gt $cutoff) { continue }
        Remove-TestRunFolder -RunFolder $folder.FullName -OutputRoot $root -NamePattern $NamePattern
        $removed += $folder.FullName
    }
    return $removed
}

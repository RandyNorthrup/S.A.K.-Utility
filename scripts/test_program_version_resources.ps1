# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    Every program S.A.K. ships carries its Windows version resource.

.DESCRIPTION
    Explorer's Details tab and the UAC prompt show a program's FileDescription. In the v0.9.3-rc.2
    package only sak_utility.exe had a version resource, so the UAC prompt for the elevated helper
    showed its bare file name. Every sak_*.exe in the program folder must name S.A.K. Utility, its
    company, its own file name, a description of its own and the version from project(), as text and
    as the four numbers Windows keeps (the last always 0). The four programs a package carries must
    be there, so a folder without them does not pass, and a program added later is checked too.
#>

[CmdletBinding()]
param(
    # The folder the build puts the programs in, as the package carries them side by side.
    [Parameter(Mandatory = $true)]
    [string]$ProgramDir,

    # project()'s version, MAJOR.MINOR.PATCH.
    [Parameter(Mandatory = $true)]
    [string]$Version
)

$ErrorActionPreference = "Stop"

if ($Version -notmatch '^(\d+)\.(\d+)\.(\d+)$') {
    throw "Version is not MAJOR.MINOR.PATCH: '$Version'"
}
$expectedNumbers = "$($Matches[1]).$($Matches[2]).$($Matches[3]).0"

$required = @(
    "sak_utility.exe",
    "sak_elevated_helper.exe",
    "sak_apfs_writer_cli.exe",
    "sak_hfs_writer_cli.exe"
)
$programs = @(Get-ChildItem -LiteralPath $ProgramDir -Filter "sak_*.exe" -File)
$missing = @($required | Where-Object { @($programs.Name) -notcontains $_ })
if ($missing.Count -gt 0) {
    throw "Programs missing from ${ProgramDir}: $($missing -join ', ')"
}

$problems = @()
$descriptions = @{}
foreach ($program in $programs) {
    $info = (Get-Item -LiteralPath $program.FullName).VersionInfo
    $expected = [ordered]@{
        CompanyName      = "Randy Northrup"
        ProductName      = "S.A.K. Utility"
        FileVersion      = $Version
        ProductVersion   = $Version
        InternalName     = $program.BaseName
        OriginalFilename = $program.Name
    }
    foreach ($field in $expected.Keys) {
        if ([string]$info.$field -cne $expected[$field]) {
            $problems += "$($program.Name): $field is '$($info.$field)', expected '$($expected[$field])'"
        }
    }
    $fileNumbers = "$($info.FileMajorPart).$($info.FileMinorPart).$($info.FileBuildPart).$($info.FilePrivatePart)"
    $productNumbers = "$($info.ProductMajorPart).$($info.ProductMinorPart).$($info.ProductBuildPart).$($info.ProductPrivatePart)"
    if ($fileNumbers -ne $expectedNumbers -or $productNumbers -ne $expectedNumbers) {
        $problems += "$($program.Name): the numeric versions are $fileNumbers and $productNumbers, expected $expectedNumbers"
    }
    # The UAC prompt names the program by this, so it says what the program is and tells them apart.
    $description = [string]$info.FileDescription
    if ($description -cnotmatch '^S\.A\.K\. Utility - \S') {
        $problems += "$($program.Name): FileDescription is '$description', expected 'S.A.K. Utility - <what it is>'"
    } elseif ($descriptions.ContainsKey($description)) {
        $problems += "$($program.Name): FileDescription '$description' is also $($descriptions[$description])'s"
    } else {
        $descriptions[$description] = $program.Name
    }
}

if ($problems.Count -gt 0) {
    $problems | ForEach-Object { Write-Host $_ }
    throw "$($problems.Count) version resource problem(s) in $ProgramDir"
}
foreach ($program in $programs) {
    $info = (Get-Item -LiteralPath $program.FullName).VersionInfo
    Write-Host ("{0}: '{1}' {2}" -f $program.Name, $info.FileDescription, $info.FileVersion)
}
Write-Host "$($programs.Count) program(s) carry their version resource."

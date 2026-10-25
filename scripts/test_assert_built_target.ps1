<#
.SYNOPSIS
    Fixture tests for Assert-FreshBuiltTarget (scripts/assert_built_target.ps1).

.DESCRIPTION
    The startup-budget and runtime-dependency tests used to hard-code build\Release\sak_utility.exe
    and so could test another build's binary. They now take the target from CTest and refuse a
    missing, misplaced or stale one. Each refusal is driven here against an owned fixture
    directory, plus the one accepted case, so a guard that stopped refusing turns this red.
#>
$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
. (Join-Path $ProjectRoot "scripts\assert_built_target.ps1")

function Assert-Refused {
    param([scriptblock]$Call, [string]$Expected, [string]$Case)
    $message = ""
    try { & $Call | Out-Null } catch { $message = $_.Exception.Message }
    if (-not $message.Contains($Expected)) {
        throw "Guard did not refuse ($Case): expected '$Expected', got '$message'"
    }
    Write-Host "REFUSED $Case"
}

$root = Join-Path ([IO.Path]::GetTempPath()) ("sak-built-target-" + [guid]::NewGuid().ToString("N"))
$release = Join-Path $root "Release"
$other = Join-Path $root "Other"
New-Item -ItemType Directory -Path $release, $other -ErrorAction Stop | Out-Null
try {
    $exe = Join-Path $release "sak_utility.exe"
    $lib = Join-Path $release "sak_core.lib"
    [IO.File]::WriteAllBytes($exe, [byte[]](1, 2, 3))
    [IO.File]::WriteAllBytes($lib, [byte[]](4, 5, 6))
    $past = [DateTime]::UtcNow.AddMinutes(-10)
    (Get-Item -LiteralPath $lib).LastWriteTimeUtc = $past
    (Get-Item -LiteralPath $exe).LastWriteTimeUtc = $past.AddMinutes(1)

    $accepted = Assert-FreshBuiltTarget -Path $exe -ReleaseDir $release
    if ($accepted -ine (Get-Item -LiteralPath $exe).FullName) {
        throw "Guard did not return the accepted target's full path: $accepted"
    }
    Write-Host "ACCEPTED fresh target in its release directory"

    Assert-Refused { Assert-FreshBuiltTarget -Path "" } "path is empty" "empty path"
    Assert-Refused { Assert-FreshBuiltTarget -Path (Join-Path $release "missing.exe") } `
        "missing" "missing target"
    Assert-Refused { Assert-FreshBuiltTarget -Path $exe -ReleaseDir $other } `
        "is not in the release directory" "target outside the named release directory"

    (Get-Item -LiteralPath $lib).LastWriteTimeUtc = $past.AddMinutes(2)
    Assert-Refused { Assert-FreshBuiltTarget -Path $exe -ReleaseDir $release } "is stale" `
        "library rebuilt after the executable (failed relink)"

    Remove-Item -LiteralPath $lib -Force
    Assert-Refused { Assert-FreshBuiltTarget -Path $exe } "No product library" `
        "no library to show freshness"
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "Assert-FreshBuiltTarget refuses missing, misplaced and stale targets"
exit 0

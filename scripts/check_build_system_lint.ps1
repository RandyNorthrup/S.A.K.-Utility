# Build-system lint: catch dead CMake guards that silently disable code under the
# Visual Studio MULTI-CONFIG generator, where CMAKE_BUILD_TYPE is empty at configure
# time. This campaign found two by hand -- one had silently disabled AddressSanitizer
# across the whole project. A real (non-comment) "if(CMAKE_BUILD_TYPE STREQUAL ...)"
# guard is therefore dead here and must not return; config-specific logic must use a
# generator expression ($<CONFIG:Debug>) or CMAKE_CONFIGURATION_TYPES instead.
#
# Fails closed: an unreadable CMakeLists is an error, not a silent pass.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)

$cmakeFiles = @()
$cmakeFiles += Get-ChildItem -Path $root -Filter "CMakeLists.txt" -Recurse -File |
    Where-Object { $_.FullName -notmatch '[\\/](third_party|build|build-debug|out|vcpkg[_-])' }
$cmakeFiles += Get-ChildItem -Path (Join-Path $root "cmake") -Filter "*.cmake" -Recurse -File -ErrorAction SilentlyContinue

# A real guard: an "if (" ... "CMAKE_BUILD_TYPE" ... "STREQUAL" that is NOT inside a
# comment (a "#" before the "if" on the line means it is documentation, which is how
# the removed anti-patterns are memorialized).
$pattern = '^\s*if\s*\(\s*CMAKE_BUILD_TYPE\s+STREQUAL'
$violations = @()
foreach ($f in ($cmakeFiles | Sort-Object FullName -Unique)) {
    $lineNo = 0
    foreach ($line in (Get-Content -LiteralPath $f.FullName)) {
        $lineNo++
        if ($line -match $pattern) {
            $rel = $f.FullName.Substring($root.Length).TrimStart('\', '/')
            $violations += "${rel}:${lineNo}: $($line.Trim())"
        }
    }
}

if ($violations.Count -gt 0) {
    Write-Host "Build-system lint FAILED: dead CMAKE_BUILD_TYPE STREQUAL guard(s) --"
    Write-Host "these are inert under the Visual Studio multi-config generator (use a"
    Write-Host "generator expression instead):"
    $violations | ForEach-Object { Write-Host "  $_" }
    exit 1
}

Write-Host "Build-system lint passed: no dead CMAKE_BUILD_TYPE STREQUAL guards ($($cmakeFiles.Count) CMake files)."
exit 0

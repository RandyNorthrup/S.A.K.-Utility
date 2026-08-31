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

# CMAKE_CXX_CLANG_TIDY and CMAKE_CXX_CPPCHECK are copied into a target when that target is
# created. Setting either variable after add_library/add_executable makes an opt-in analysis build
# print ENABLED while instrumenting zero already-created targets. Enforce both assignments before
# the first target in the root build file.
$rootCmake = Join-Path $root "CMakeLists.txt"
$rootLines = @(Get-Content -LiteralPath $rootCmake)
$firstTargetLine = 0
for ($index = 0; $index -lt $rootLines.Count; $index++) {
    if ($rootLines[$index] -match '^\s*add_(?:library|executable)\s*\(') {
        $firstTargetLine = $index + 1
        break
    }
}
if ($firstTargetLine -eq 0) {
    $violations += "CMakeLists.txt: no add_library/add_executable target found"
}
foreach ($analyzerVariable in @("CMAKE_CXX_CLANG_TIDY", "CMAKE_CXX_CPPCHECK")) {
    $assignmentLine = 0
    for ($index = 0; $index -lt $rootLines.Count; $index++) {
        if ($rootLines[$index] -match ("^\s*set\s*\(\s*" + $analyzerVariable + "\b")) {
            $assignmentLine = $index + 1
            break
        }
    }
    if ($assignmentLine -eq 0) {
        $violations += "CMakeLists.txt: missing $analyzerVariable assignment"
    } elseif ($firstTargetLine -gt 0 -and $assignmentLine -gt $firstTargetLine) {
        $violations += ("CMakeLists.txt:{0}: {1} is assigned after first target at line {2}; " +
                        "build-integrated analysis would not instrument existing targets" -f
                        $assignmentLine, $analyzerVariable, $firstTargetLine)
    }
}

if ($violations.Count -gt 0) {
    Write-Host "Build-system lint FAILED: invalid CMake gate configuration --"
    $violations | ForEach-Object { Write-Host "  $_" }
    exit 1
}

Write-Host ("Build-system lint passed: no dead CMAKE_BUILD_TYPE guards and analyzer setup " +
           "precedes target creation ($($cmakeFiles.Count) CMake files).")
exit 0

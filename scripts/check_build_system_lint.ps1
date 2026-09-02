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
$forbiddenWarningSilences = @(
    "-Wno-unused-command-line-argument",
    "-Wno-missing-designated-field-initializers",
    "-Wno-trigraphs",
    "-Wno-unused-lambda-capture",
    "-Wno-unused-parameter",
    "-Wno-missing-field-initializers",
    "-Wno-unused-variable",
    "-Wno-unused-function"
)
$violations = @()
foreach ($f in ($cmakeFiles | Sort-Object FullName -Unique)) {
    $lineNo = 0
    foreach ($line in (Get-Content -LiteralPath $f.FullName)) {
        $lineNo++
        if ($line -match $pattern) {
            $rel = $f.FullName.Substring($root.Length).TrimStart('\', '/')
            $violations += "${rel}:${lineNo}: $($line.Trim())"
        }
        if ($line.TrimStart().StartsWith("#")) {
            continue
        }
        foreach ($silence in $forbiddenWarningSilences) {
            if ($line.Contains($silence)) {
                $rel = $f.FullName.Substring($root.Length).TrimStart('\', '/')
                $violations += ("${rel}:${lineNo}: forbidden coverage warning silence " +
                                "'$silence'; fix the diagnostic instead")
            }
        }
    }
}

# CMAKE_CXX_CLANG_TIDY and CMAKE_CXX_CPPCHECK are copied into a target when that target is
# created. Setting either variable after add_library/add_executable makes an opt-in analysis build
# print ENABLED while instrumenting zero already-created targets. Enforce both assignments before
# the first target in the root build file.
$rootCmake = Join-Path $root "CMakeLists.txt"
$rootLines = @(Get-Content -LiteralPath $rootCmake)
$rootText = $rootLines -join "`n"
if ($rootText -notmatch '(?ms)^if\s*\(\s*NOT\s+WIN32\s*\).*?message\s*\(\s*FATAL_ERROR.*?^endif\s*\(\s*\)') {
    $violations += ("CMakeLists.txt: missing fail-closed Windows-only configure boundary; " +
                    "the shipped application supports Windows 10/11 x64 only")
}
$unsupportedPlatformTokens = @(
    "PLATFORM_MACOS",
    "PLATFORM_LINUX",
    "SAK_PLATFORM_MACOS",
    "SAK_PLATFORM_LINUX",
    "MACOSX_BUNDLE",
    "macdeployqt"
)
for ($index = 0; $index -lt $rootLines.Count; $index++) {
    $trimmed = $rootLines[$index].TrimStart()
    if ($trimmed.StartsWith("#")) {
        continue
    }
    foreach ($token in $unsupportedPlatformTokens) {
        if ($rootLines[$index].Contains($token)) {
            $violations += (("CMakeLists.txt:{0}: unsupported application-platform token '{1}'; " +
                             "remove the dead branch instead of advertising non-Windows support") -f
                            ($index + 1), $token)
        }
    }
}

# Configure rejects non-Windows hosts before any target is created. These audited product modules
# therefore must contain only the shipped Windows implementation: platform branches, POSIX host-I/O
# shims, and Q_UNUSED placeholders are dead code that can drift away from the executable path.
$windowsOnlyProductFiles = @(
    "include/sak/elevated_pipe_server.h",
    "include/sak/elevation_broker.h",
    "include/sak/input_validator.h",
    "include/sak/leftover_scanner.h",
    "include/sak/secure_memory.h",
    "src/actions/backup_bitlocker_keys_action.cpp",
    "src/ai/ai_credential_store.cpp",
    "src/core/advanced_search_worker.cpp",
    "src/core/app_mutating_actions.cpp",
    "src/core/chocolatey_manager.cpp",
    "src/core/crash_reporter.cpp",
    "src/core/disk_benchmark_worker.cpp",
    "src/core/elevated_pipe_server.cpp",
    "src/core/elevation_broker.cpp",
    "src/core/encryption.cpp",
    "src/core/input_validator.cpp",
    "src/core/leftover_scanner.cpp",
    "src/core/partition_raw_device_io.cpp",
    "src/core/recycle_bin.cpp",
    "src/core/secure_memory.cpp",
    "src/core/stress_test_worker.cpp",
    "src/core/vulnerability_scanner.cpp",
    "src/core/wifi_profile_scanner.cpp",
    "src/core/windows_user_scanner.cpp",
    "src/tools/sak_apfs_writer_cli.cpp",
    "src/tools/sak_hfs_writer_cli.cpp"
)
$deadPlatformGuardPattern =
    '^\s*#\s*(?:if|ifdef|ifndef|elif)\b.*(?:_WIN32|Q_OS_WIN|SAK_PLATFORM_WINDOWS|_MSC_VER|__linux__|__APPLE__|SEEK_DATA|SEEK_HOLE)'
$deadHostCompatibilityTokens = @(
    "#include <sys/resource.h>",
    "#include <sys/stat.h>",
    "#include <sys/statvfs.h>",
    "#include <unistd.h>",
    "Q_UNUSED("
)
foreach ($relativePath in $windowsOnlyProductFiles) {
    $sourcePath = Join-Path $root $relativePath
    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
        $violations += "${relativePath}: audited Windows-only product file is missing"
        continue
    }
    $lineNo = 0
    foreach ($line in (Get-Content -LiteralPath $sourcePath)) {
        $lineNo++
        if ($line -match $deadPlatformGuardPattern) {
            $violations += ("${relativePath}:${lineNo}: dead application-platform guard; " +
                            "configure already rejects non-Windows targets")
        }
        foreach ($token in $deadHostCompatibilityTokens) {
            if ($line.Contains($token)) {
                $violations += ("${relativePath}:${lineNo}: dead host-compatibility token " +
                                "'$token' in Windows-only product module")
            }
        }
    }
}
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

Write-Host ("Build-system lint passed: Windows-only boundary enforced, no dead CMAKE_BUILD_TYPE " +
           "guards or forbidden coverage warning silences, and analyzer setup precedes target creation " +
           "($($cmakeFiles.Count) CMake files).")
exit 0

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

# Warning-silencing markers are forbidden across all first-party production C/C++ source. An
# unused callback parameter can be unnamed; a discarded value must be asserted, reported, or
# removed. File-wide third-party trees stay byte-identical to upstream and are outside this gate.
$productionSourceFiles = @()
foreach ($sourceRoot in @((Join-Path $root "src"), (Join-Path $root "include"))) {
    $productionSourceFiles += Get-ChildItem -LiteralPath $sourceRoot -Recurse -File |
        Where-Object {
            $_.Extension -in @(".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx") -and
            $_.FullName -notmatch '[\\/]third_party[\\/]'
        }
}
foreach ($sourceFile in ($productionSourceFiles | Sort-Object FullName -Unique)) {
    $relativePath = $sourceFile.FullName.Substring($root.Length).TrimStart('\', '/')
    $lineNo = 0
    foreach ($line in (Get-Content -LiteralPath $sourceFile.FullName)) {
        $lineNo++
        if ($line.TrimStart().StartsWith("//")) {
            continue
        }
        foreach ($marker in @("Q_UNUSED(", "[[maybe_unused]]")) {
            if ($line.Contains($marker)) {
                $violations += ("${relativePath}:${lineNo}: forbidden production warning " +
                                "silence '$marker'; remove the parameter/value or use it")
            }
        }
    }
}

# Test registration is part of the Windows-only product contract. A tracked test source or a
# required PowerShell-driven CTest must fail configure when unavailable, never disappear behind an
# EXISTS/TARGET/WIN32 guard or a warning that says it was skipped.
$testsCmakePath = Join-Path $root "tests/CMakeLists.txt"
$testsCmakeLines = @(Get-Content -LiteralPath $testsCmakePath)
$testsCmakeText = $testsCmakeLines -join "`n"
for ($index = 0; $index -lt $testsCmakeLines.Count; $index++) {
    $line = $testsCmakeLines[$index]
    if ($line.TrimStart().StartsWith("#")) { continue }
    if ($line -match '^\s*message\s*\(\s*WARNING.*\bskipp') {
        $violations += (("tests/CMakeLists.txt:{0}: test omission is a warning; fail configure " +
                         "instead of skipping the test") -f ($index + 1))
    }
    if ($line -match '^\s*if\s*\(\s*WIN32\s+AND\s+EXISTS.*[\\/]test_[^\"]+') {
        $violations += (("tests/CMakeLists.txt:{0}: tracked Windows test source is conditional; " +
                         "add it directly so a missing file fails configure") -f ($index + 1))
    }
    if ($line -match '^\s*if\s*\(\s*TARGET\s+\S+\s+AND\s+WIN32\s*\)') {
        $violations += (("tests/CMakeLists.txt:{0}: required CTest is conditional on a target; " +
                         "the target and test are mandatory on this Windows-only build") -f
                        ($index + 1))
    }
}

$requiredTrackedTestSources = @(
    "unit/test_win32_mcp_key_chord.cpp",
    "unit/test_win32_mcp_input_plan.cpp",
    "unit/test_backup_destination_guard.cpp",
    "unit/test_win32_mcp_server.cpp",
    "unit/test_browser_bridge_security.cpp",
    "unit/test_browser_bridge_pipe.cpp",
    "unit/test_browser_bridge_relay.cpp"
)
foreach ($relativePath in $requiredTrackedTestSources) {
    $trackedSourcePath = Join-Path (Join-Path $root "tests") $relativePath
    if (-not (Test-Path -LiteralPath $trackedSourcePath -PathType Leaf)) {
        $violations += "tests/${relativePath}: required tracked test source is missing"
    }
}

if ($testsCmakeText -notmatch
    '(?ms)find_program\s*\(\s*SAK_TEST_POWERSHELL_PATH\b.*?if\s*\(\s*NOT\s+SAK_TEST_POWERSHELL_PATH\b.*?\).*?message\s*\(\s*FATAL_ERROR') {
    $violations += ("tests/CMakeLists.txt: required PowerShell CTests lack a fail-closed " +
                    "find_program/FATAL_ERROR preflight")
}
foreach ($target in @("test_ai_assistant_panel_tool_dispatch", "test_browser_extension_installer")) {
    $definitionPattern = ('(?ms)target_compile_definitions\s*\(\s*' + [regex]::Escape($target) +
                          '\s+PRIVATE(?:(?!\)\s*$).)*\bSAK_WIN_REGISTRY_TESTS\b')
    if ($testsCmakeText -notmatch $definitionPattern) {
        $violations += ("tests/CMakeLists.txt: $target must define SAK_WIN_REGISTRY_TESTS " +
                        "unconditionally so AUTOMOC registers its Windows slots")
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
           "guards, coverage warning silences, or production unused-value silences; analyzer " +
           "setup precedes target creation ($($cmakeFiles.Count) CMake files).")
exit 0

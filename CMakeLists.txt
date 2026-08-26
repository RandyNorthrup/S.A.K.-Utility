cmake_minimum_required(VERSION 3.28 FATAL_ERROR)

# Load persistent build configuration (vcpkg, Qt, static runtime) - optional for CI
set(SAK_BUILD_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SAK_BuildConfig.cmake")
if(EXISTS "${SAK_BUILD_CONFIG}")
    include("${SAK_BUILD_CONFIG}")
    message(STATUS "Loaded build configuration from ${SAK_BUILD_CONFIG}")
else()
    message(STATUS "No build configuration found - using environment/command-line settings")
    # Default to dynamic CRT when no build config is present (CI environments)
    if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "MSVC runtime library")
    endif()
endif()

# Project definition
project(SAK_Utility
    VERSION 0.9.2.0
    DESCRIPTION "Swiss Army Knife Utility - PC technician's toolkit for Windows migration and maintenance"
    LANGUAGES C CXX
)# C++23 Standard - Strict compliance required
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Platform defines (apply to all targets)
if(WIN32)
    add_compile_definitions(
        WIN32_LEAN_AND_MEAN  # Exclude rarely-used Windows headers
        NOMINMAX             # Prevent Windows min/max macro conflicts
    )
endif()

# Export compile commands for IDE integration
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type default
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# Module path for custom CMake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Suppress optional Vulkan header discovery (not required for this app)
set(CMAKE_DISABLE_FIND_PACKAGE_WrapVulkanHeaders TRUE)

# Platform detection
if(WIN32)
    set(PLATFORM_WINDOWS TRUE)
    add_definitions(-DSAK_PLATFORM_WINDOWS)
elseif(APPLE)
    set(PLATFORM_MACOS TRUE)
    add_definitions(-DSAK_PLATFORM_MACOS)
elseif(UNIX)
    set(PLATFORM_LINUX TRUE)
    add_definitions(-DSAK_PLATFORM_LINUX)
endif()

# ============================================================================
# Compiler Configuration - STRICT STANDARDS
# ============================================================================

# MSVC Compiler Flags
if(MSVC)
    # Static Runtime Linking - Embed MSVC runtime (fixes MSVCP140.DLL, VCRUNTIME140.DLL, VCRUNTIME140_1.DLL errors)
    # NOTE: CMAKE_MSVC_RUNTIME_LIBRARY is now set in cmake/SAK_BuildConfig.cmake

    # Warning Level 4, Warnings as Errors, Strict Conformance
    add_compile_options(
        /W4                 # Warning level 4
        /WX                 # Treat warnings as errors
        /permissive-        # Strict C++ conformance
        /Zc:__cplusplus     # Enable correct __cplusplus macro
        /sdl                # Security Development Lifecycle checks
        /MP                 # Multi-processor compilation
        /Zc:inline          # Remove unreferenced COMDAT
        /Zc:preprocessor    # Conforming preprocessor
        /utf-8              # UTF-8 source and execution charset
        /external:anglebrackets  # Treat <> includes as external
        /external:W0        # Disable warnings for external headers
        # Static runtime is set via CMAKE_MSVC_RUNTIME_LIBRARY in cmake/SAK_BuildConfig.cmake
    )

    # Config-specific flags. Use generator expressions so Visual Studio
    # multi-config builds do not leak Release optimization flags into Debug.
    add_compile_options(
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/Ob2>
        $<$<CONFIG:Release>:/Oi>
        $<$<CONFIG:Release>:/Ot>
        $<$<CONFIG:Release>:/GL>
        $<$<CONFIG:Debug>:/Od>
        $<$<CONFIG:Debug>:/Zi>
        $<$<CONFIG:Debug>:/RTC1>
    )
    add_link_options(
        $<$<CONFIG:Release>:/LTCG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )

    # Security features
    add_compile_options(/guard:cf)  # Control Flow Guard
    add_link_options(/guard:cf /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA)

# Clang/GCC Compiler Flags
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    # Comprehensive warning flags
    add_compile_options(
        -Wall                   # Enable all warnings
        -Wextra                 # Extra warnings
        -Werror                 # Warnings as errors
        -Wpedantic              # Pedantic warnings
        -Wconversion            # Implicit conversions
        -Wshadow                # Variable shadowing
        -Wnon-virtual-dtor      # Non-virtual destructors
        -Wold-style-cast        # C-style casts
        -Wcast-align            # Alignment casts
        -Wunused                # Unused variables/functions
        -Woverloaded-virtual    # Overloaded virtuals
        -Wsign-conversion       # Sign conversions
        -Wnull-dereference      # Null pointer dereference
        -Wdouble-promotion      # Float to double promotion
        -Wformat=2              # Format string vulnerabilities
    )

    # Config-specific flags.
    #
    # -march=native bakes the BUILD host's instruction set into the image, so a
    # release compiled on a newer CPU dies with an illegal instruction on the
    # older technician and customer machines this tool exists to service. It
    # stays available for local benchmarking, but it has to be asked for: a
    # distributable build must never pick it up by accident.
    option(SAK_ENABLE_MARCH_NATIVE
        "Optimize Release builds for the build host's CPU (NOT distributable)" OFF)
    add_compile_options(
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Release>:-flto>
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Debug>:-g3>
    )
    if(SAK_ENABLE_MARCH_NATIVE)
        add_compile_options($<$<CONFIG:Release>:-march=native>)
        message(STATUS "-march=native:      ENABLED (the binary is NOT portable off this host)")
    endif()
    add_link_options($<$<CONFIG:Release>:-flto>)

    # Security features.
    #
    # _FORTIFY_SOURCE needs an optimizing compile to do anything at all; glibc
    # answers -O0 with "#warning _FORTIFY_SOURCE requires compiling with
    # optimization", and the -Werror above turns that into a hard error - so the
    # flag meant to harden Release would instead stop every Debug build. Apply it
    # only to the configurations where it has an effect.
    add_compile_options(-fstack-protector-strong)
    add_compile_options($<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=2>)
    # -z relro / -z now are ELF-only. Apple's linker rejects -z outright and the
    # MinGW/PE linkers ignore it, so emit them only where they mean something
    # rather than shipping flags whose fate depends on the linker in use.
    if(PLATFORM_LINUX)
        add_link_options(-Wl,-z,relro -Wl,-z,now)
    endif()
endif()

# ============================================================================
# Sanitizers -- Strictest Settings for Debug and Testing
# ============================================================================
#
# MSVC supports:  ASan only (/fsanitize=address)
# Clang/GCC:      ASan, UBSan, TSan (TSan mutually exclusive with ASan)
#
# Debug build:    cmake -B build -DCMAKE_BUILD_TYPE=Debug
# ASan + UBSan:   cmake -B build -DCMAKE_BUILD_TYPE=Debug (default)
# TSan only:      cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON -DENABLE_ASAN=OFF
#
# Runtime env vars (set before running the executable):
#   ASAN_OPTIONS=strict_string_checks=1:detect_stack_use_after_return=1:check_initialization_order=1:strict_init_order=1:detect_odr_violation=2:halt_on_error=1
#   UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
#   TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1
# ============================================================================

option(ENABLE_ASAN "Enable Address Sanitizer (Debug builds)" ON)
option(ENABLE_UBSAN "Enable Undefined Behavior Sanitizer (Clang/GCC Debug)" ON)
option(ENABLE_TSAN "Enable Thread Sanitizer (mutually exclusive with ASan)" OFF)

# ============================================================================
# BRANCH COVERAGE (R5-G14-16b)
# ============================================================================
# OpenCppCoverage (scripts/run_coverage.ps1) measures LINE coverage only, which
# cannot see an untaken arm that shares a line with a taken one. That blind spot
# is not theoretical: include/sak/mbox_transfer_decoder.h measures 100% LINE
# covered while four of its branches were never executed by the suite -- the
# bare-LF soft break, the hex pair's short-circuited second conjunct, and a
# whitespace arm of the base64 unwrap. Each was a real hole, found by hand.
#
# Source-based instrumentation reports those directly. It needs clang-cl (MSVC
# has no equivalent), so this is opt-in and OFF by default; configure a separate
# build tree with -T ClangCL -DENABLE_COVERAGE=ON and drive it with
# scripts/run_branch_coverage.ps1.
option(ENABLE_COVERAGE "Enable source-based branch coverage (clang-cl only)" OFF)

set(SAK_COVERAGE_STATE "OFF")
if(ENABLE_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # Silently building uninstrumented binaries would produce an empty report
        # that reads as "no uncovered branches" -- the worst possible failure for
        # a coverage gate. Refuse instead.
        message(FATAL_ERROR
            "ENABLE_COVERAGE=ON requires clang-cl: MSVC emits no source-based coverage "
            "mapping, so the build would produce uninstrumented binaries and an empty "
            "report that reads as full coverage. Configure the coverage tree with "
            "-T ClangCL, or leave ENABLE_COVERAGE=OFF.")
    endif()
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    add_link_options(-fprofile-instr-generate)
    # The warning set below is tuned for MSVC and carries /WX. clang-cl accepts the flags but
    # warns that a few are unused (/MP, /Zc:preprocessor, /external:anglebrackets), and /WX then
    # turns those notes into hard errors before a single file compiles. Silence ONLY that
    # diagnostic, and only in this opt-in tree: every real warning stays an error.
    add_compile_options(-Wno-unused-command-line-argument)
    # clang has warnings MSVC does not, and /WX makes each one fatal. These are toolchain
    # differences rather than defects, and this tree exists to MEASURE the suite, not to re-lint
    # it under a second compiler: a coverage run that cannot build reports nothing at all.
    #   * missing-designated-field-initializers: a C++20 designated initializer that omits a
    #     field, which is value-initialized by the standard (e.g. the root-folder record in
    #     partition_hfs_internal.h omitting .data_fork).
    # Everything else stays an error, so this does not turn the coverage build into a warning-free
    # fiction. Linting is the MSVC build's job and is unaffected.
    add_compile_options(-Wno-missing-designated-field-initializers)
    #   * missing-field-initializers / unused-variable / unused-function / unused-parameter /
    #     unused-lambda-capture / trigraphs: clang diagnoses these where MSVC does not. They are
    #     style differences between two compilers, not defects in code that MSVC builds warning
    #     free under /W4 /WX every commit.
    add_compile_options(
        -Wno-missing-field-initializers
        -Wno-unused-variable
        -Wno-unused-function
        -Wno-unused-parameter
        -Wno-unused-lambda-capture
        -Wno-trigraphs)
    set(SAK_COVERAGE_STATE "ENABLED (source-based, all configurations)")
    message(STATUS "Branch coverage:    ${SAK_COVERAGE_STATE}")
endif()

# Effective state, reported in the configuration summary below. The option
# values are requests; these say what the build will actually instrument.
set(SAK_ASAN_STATE "OFF")
set(SAK_UBSAN_STATE "OFF")
set(SAK_TSAN_STATE "OFF")

# Two mutually exclusive sanitizers were requested. Quietly dropping one leaves
# the build reporting coverage it does not have, so refuse to configure and make
# the caller say which one is wanted.
if(ENABLE_TSAN AND ENABLE_ASAN)
    message(FATAL_ERROR
        "ENABLE_TSAN and ENABLE_ASAN are mutually exclusive: one binary cannot carry "
        "both instrumentations. Configure with -DENABLE_TSAN=ON -DENABLE_ASAN=OFF for a "
        "TSan build, or leave ENABLE_TSAN=OFF for the default ASan+UBSan build.")
endif()

# The sanitizer flags MUST be selected per-configuration with a generator
# expression, never behind "if(CMAKE_BUILD_TYPE STREQUAL Debug)".
#
# This project is generated with "Visual Studio 17 2022", a MULTI-CONFIG
# generator: CMAKE_BUILD_TYPE is empty there and the configuration is chosen at
# build time by --config. The old guard therefore never fired, and
# /fsanitize=address was never applied to a single translation unit even though
# CMakeCache reported ENABLE_ASAN:BOOL=ON. Verified before the fix: 0 of 246
# generated .vcxproj files carried /fsanitize. A gate that reports healthy while
# analyzing nothing is worse than an absent one.
#
# $<CONFIG:Debug> is evaluated per configuration, so this is correct under both
# multi-config and single-config generators. Release is unaffected.
if(TRUE)
    if(MSVC)
        # MSVC Address Sanitizer - strictest configuration
        if(ENABLE_ASAN)
            add_compile_options($<$<CONFIG:Debug>:/fsanitize=address>)
            # /Zi for full debug info with ASan stack traces
            add_compile_options($<$<CONFIG:Debug>:/Zi>)
            # MSVC ASan is incompatible with incremental linking, so turn it off
            # for Debug only. Note that /RTC1 (CMake's Debug default, emitted as
            # BasicRuntimeChecks=EnableFastChecks) is NOT stripped: it was tested
            # against MSVC 14.44 and the two coexist, so removing it would be a
            # change made on an assumption rather than on evidence.
            add_link_options($<$<CONFIG:Debug>:/INCREMENTAL:NO>)
            # An ASan-instrumented binary loads clang_rt.asan_dynamic at startup.
            # Without it on the search path every Debug executable dies with
            # STATUS_DLL_NOT_FOUND (0xC0000135) BEFORE main, which looks like a
            # broken test rather than a missing runtime. Resolve it from the
            # compiler's own directory (it ships beside cl.exe) and fail closed
            # if it is not there, instead of producing binaries that cannot run.
            get_filename_component(SAK_MSVC_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
            set(SAK_ASAN_RUNTIME
                "${SAK_MSVC_BIN_DIR}/clang_rt.asan_dynamic-x86_64.dll")
            if(NOT EXISTS "${SAK_ASAN_RUNTIME}")
                message(FATAL_ERROR
                    "ENABLE_ASAN is ON but the ASan runtime was not found at "
                    "${SAK_ASAN_RUNTIME}. Install the 'C++ AddressSanitizer' "
                    "component of the Visual Studio installer, or configure with "
                    "-DENABLE_ASAN=OFF.")
            endif()
            set(SAK_ASAN_STATE "ENABLED for the Debug configuration")
            message(STATUS "Address Sanitizer:  ${SAK_ASAN_STATE}")
            message(STATUS "ASan runtime:       ${SAK_ASAN_RUNTIME}")
            # Stage it beside both Debug output roots at configure time, so every
            # Debug executable finds it without depending on the developer's PATH.
            # An instrumented binary that cannot load this DLL dies with
            # STATUS_DLL_NOT_FOUND before main, which reads as a broken test
            # rather than a missing runtime -- measured, not assumed.
            file(COPY "${SAK_ASAN_RUNTIME}" DESTINATION "${CMAKE_BINARY_DIR}/Debug")
            file(COPY "${SAK_ASAN_RUNTIME}" DESTINATION "${CMAKE_BINARY_DIR}/tests/Debug")
        endif()
        # Note: MSVC does not support UBSan or TSan.
        # Use clang-cl or GCC for UBSan/TSan coverage.
        # ENABLE_UBSAN is ON by default, so its value here is not a request from
        # anyone; report the effective state and carry on. ENABLE_TSAN defaults
        # OFF, so ON means somebody explicitly asked for coverage this toolchain
        # cannot give - that fails configure instead of printing SKIPPED and
        # then building an uninstrumented binary that looks tested.
        if(ENABLE_UBSAN)
            set(SAK_UBSAN_STATE "SKIPPED (MSVC has no UBSan; use clang-cl or GCC)")
            message(STATUS "UBSan:              ${SAK_UBSAN_STATE}")
        endif()
        if(ENABLE_TSAN)
            message(FATAL_ERROR
                "ENABLE_TSAN=ON but the compiler is MSVC, which has no ThreadSanitizer. "
                "Build the TSan configuration with clang-cl or GCC, or configure with "
                "-DENABLE_TSAN=OFF.")
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        # GCC/Clang Sanitizers - strictest flags. Same per-config rule as MSVC.
        if(ENABLE_ASAN)
            add_compile_options(
                $<$<CONFIG:Debug>:-fsanitize=address>
                $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
                $<$<CONFIG:Debug>:-fno-optimize-sibling-calls>  # Better stack traces
            )
            add_link_options($<$<CONFIG:Debug>:-fsanitize=address>)
            set(SAK_ASAN_STATE "ENABLED for the Debug configuration")
            message(STATUS "Address Sanitizer:  ${SAK_ASAN_STATE}")
        endif()

        if(ENABLE_UBSAN)
            add_compile_options(
                $<$<CONFIG:Debug>:-fsanitize=undefined>
                $<$<CONFIG:Debug>:-fsanitize=float-divide-by-zero>
                $<$<CONFIG:Debug>:-fsanitize=float-cast-overflow>
                $<$<CONFIG:Debug>:-fno-sanitize-recover=all>  # Strictest: abort on any UB
                $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
            )
            # -fsanitize=integer is a Clang-only group (unsigned-integer-overflow
            # and friends). GCC rejects the command line outright, so a GCC Debug
            # build - a configuration this branch explicitly claims to support -
            # would fail before compiling a single file. Add it where it exists,
            # and say plainly where it does not.
            if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
                add_compile_options($<$<CONFIG:Debug>:-fsanitize=integer>)
                set(SAK_UBSAN_STATE "ENABLED for Debug (strictest, no recovery, +integer)")
            else()
                set(SAK_UBSAN_STATE
                    "ENABLED for Debug (strictest, no recovery; -fsanitize=integer is Clang-only)")
            endif()
            add_link_options($<$<CONFIG:Debug>:-fsanitize=undefined>)
            message(STATUS "UBSan:              ${SAK_UBSAN_STATE}")
        endif()

        if(ENABLE_TSAN)
            add_compile_options(
                $<$<CONFIG:Debug>:-fsanitize=thread>
                $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
            )
            add_link_options($<$<CONFIG:Debug>:-fsanitize=thread>)
            set(SAK_TSAN_STATE "ENABLED for the Debug configuration")
            message(STATUS "Thread Sanitizer:   ${SAK_TSAN_STATE}")
        endif()
    endif()
endif()

# ============================================================================
# Dependencies
# ============================================================================

# Qt6 - Required. CorePrivate provides QZipReader/QZipWriter for the explorer
# zip compress/extract (no third-party archive library is vendored).
find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core
    CorePrivate
    Widgets
    Concurrent
    Network
    Xml
)

# Threads - Standard library threading
find_package(Threads REQUIRED)

# Compression libraries for image decompression
find_package(ZLIB REQUIRED)
find_package(BZip2 REQUIRED)
find_package(LibLZMA REQUIRED)

# ----------------------------------------------------------------------------
# C runtime coherence - fail configure, never suppress.
#
# Linking static-CRT (/MT) libraries into a dynamic-CRT (/MD) image puts two C
# runtimes in one process: two heaps, two copies of stdio, locale and errno
# state. Freeing in one what the other allocated corrupts the heap. MSVC reports
# it as LNK4098 and then resolves duplicate CRT symbols to whichever library the
# linker saw first, so the behaviour is not determined by anything in the source.
#
# This project shipped that mismatch. vcpkg was pinned to x64-windows-static
# (static CRT) while CMAKE_MSVC_RUNTIME_LIBRARY resolved to the dynamic CRT,
# because the file that selected a static runtime, cmake/SAK_BuildConfig.cmake,
# is gitignored and absent from every checkout including CI - so the "fallback"
# was in fact the only path anyone ever took. The conflict was then hidden by
# /NODEFAULTLIB suppressions rather than resolved.
#
# Qt fixes which side is correct: the msvc2022_64 Qt binaries are built against
# the dynamic CRT, so a Qt application must use the dynamic CRT too. That makes
# x64-windows-static-md (static libraries, dynamic CRT) the coherent triplet: it
# keeps the single-file linking of static libs without a second runtime.
# ----------------------------------------------------------------------------
if(MSVC)
    string(FIND "${CMAKE_MSVC_RUNTIME_LIBRARY}" "DLL" _sak_app_crt_dll)
    if(_sak_app_crt_dll EQUAL -1)
        set(_sak_app_crt "static")
    else()
        set(_sak_app_crt "dynamic")
    endif()

    get_target_property(_sak_qt_core_type Qt6::Core TYPE)
    if(_sak_qt_core_type STREQUAL "SHARED_LIBRARY" AND NOT _sak_app_crt STREQUAL "dynamic")
        message(FATAL_ERROR
            "CRT mismatch: Qt6::Core is a shared library built against the dynamic CRT, "
            "but CMAKE_MSVC_RUNTIME_LIBRARY is '${CMAKE_MSVC_RUNTIME_LIBRARY}' (static CRT). "
            "A Qt application must use the same runtime as the Qt binaries it loads. "
            "Set CMAKE_MSVC_RUNTIME_LIBRARY to MultiThreaded$<$<CONFIG:Debug>:Debug>DLL, or "
            "build against a static Qt.")
    endif()

    set(_sak_vcpkg_crt "not-used")
    if(DEFINED VCPKG_TARGET_TRIPLET)
        # Triplet CRT: a name ending in -static uses the static CRT. Plain
        # dynamic triplets and -static-md (static libs, dynamic CRT) do not.
        if("${VCPKG_TARGET_TRIPLET}" MATCHES "-static$")
            set(_sak_vcpkg_crt "static")
        else()
            set(_sak_vcpkg_crt "dynamic")
        endif()
        if(NOT _sak_app_crt STREQUAL _sak_vcpkg_crt)
            message(FATAL_ERROR
                "CRT mismatch: this project compiles against the ${_sak_app_crt} CRT "
                "(CMAKE_MSVC_RUNTIME_LIBRARY='${CMAKE_MSVC_RUNTIME_LIBRARY}') while vcpkg "
                "triplet '${VCPKG_TARGET_TRIPLET}' supplies libraries built against the "
                "${_sak_vcpkg_crt} CRT. Two C runtimes in one image is a heap-corruption "
                "hazard, not a warning to silence. Use -DVCPKG_TARGET_TRIPLET=x64-windows-static-md "
                "so the static vcpkg libraries use the same dynamic CRT as this project and Qt. "
                "Do NOT resolve this with /NODEFAULTLIB.")
        endif()
    endif()
    # Checking the triplet VARIABLE is not enough, and that was proven the hard
    # way: after switching VCPKG_TARGET_TRIPLET to x64-windows the check above
    # reported "dynamic / dynamic" while the linker still emitted LNK4098. CMake
    # caches the resolved library paths (ZLIB_LIBRARY_RELEASE and friends) as
    # absolute FILEPATHs, and changing the triplet does not re-resolve them, so
    # the build was still linking C:/vcpkg/installed/x64-windows-static/...
    #
    # A gate that reports healthy while the thing it guards is broken is the
    # exact failure this whole section exists to stop, so verify what is actually
    # going to be LINKED, not what was asked for.
    if(DEFINED VCPKG_TARGET_TRIPLET)
        foreach(_sak_lib_var
                ZLIB_LIBRARY_RELEASE ZLIB_LIBRARY_DEBUG
                BZIP2_LIBRARY_RELEASE BZIP2_LIBRARY_DEBUG
                LIBLZMA_LIBRARY_RELEASE LIBLZMA_LIBRARY_DEBUG)
            if(${_sak_lib_var} AND NOT "${${_sak_lib_var}}" MATCHES "/${VCPKG_TARGET_TRIPLET}/")
                message(FATAL_ERROR
                    "Stale resolved dependency: ${_sak_lib_var} is '${${_sak_lib_var}}', which is "
                    "not under the selected triplet '${VCPKG_TARGET_TRIPLET}'. CMake caches these "
                    "as absolute paths, so changing the triplet alone does NOT re-resolve them and "
                    "the build would keep linking the old libraries - including their CRT. Delete "
                    "the build directory (or at least CMakeCache.txt) and configure again.")
            endif()
        endforeach()
    endif()

    message(STATUS "CRT: ${_sak_app_crt} (app), ${_sak_vcpkg_crt} (vcpkg ${VCPKG_TARGET_TRIPLET})")
endif()

# Platform-specific libraries
if(PLATFORM_WINDOWS)
    # Windows-specific: Registry access, WMI, DXGI, performance counters,
    # networking (IP Helper, Winsock, DNS, WLAN, NetBIOS), COM (Firewall)
    set(PLATFORM_LIBS advapi32 crypt32 shell32 user32 dxgi pdh
        iphlpapi ws2_32 dnsapi wlanapi netapi32 ole32 oleaut32 dbghelp)
elseif(PLATFORM_MACOS)
    # macOS-specific: CoreFoundation, IOKit
    find_library(COREFOUNDATION_LIBRARY CoreFoundation REQUIRED)
    find_library(IOKIT_LIBRARY IOKit REQUIRED)
    set(PLATFORM_LIBS ${COREFOUNDATION_LIBRARY} ${IOKIT_LIBRARY})
endif()

# ============================================================================
# Project Configuration
# ============================================================================

# Resolve build type for multi-config generators (MSVC)
# CMAKE_BUILD_TYPE is empty on multi-config; use generator expression fallback
if(CMAKE_BUILD_TYPE)
    set(SAK_RESOLVED_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
else()
    set(SAK_RESOLVED_BUILD_TYPE "Release")
endif()

option(SAK_ENABLE_AI_ASSISTANT "Build the AI Assistant panel" ON)

# Version header generation
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/sak/version.h"
    @ONLY
)

# Include directories
include_directories(
    ${CMAKE_CURRENT_BINARY_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ============================================================================
# Source Files
# ============================================================================

# Core utility modules
set(CORE_SOURCES
    src/core/logger.cpp
    src/core/app_paths.cpp
    src/core/crash_reporter.cpp
    src/core/file_hash.cpp
    src/core/file_scanner.cpp
    src/core/path_utils.cpp
    src/core/config_manager.cpp
    src/core/input_validator.cpp
    src/core/secure_memory.cpp
    src/core/encryption.cpp
    src/core/backup_file_codec.cpp
    src/core/keep_awake.cpp
    src/core/elevation_manager.cpp
    src/core/bundled_tools_manager.cpp
    src/core/app_scanner.cpp
    src/core/chocolatey_manager.cpp
    src/core/vulnerability_scanner.cpp
    src/core/package_matcher.cpp
    src/core/migration_report.cpp
    src/core/app_installation_worker.cpp
    src/core/app_installation_busy.cpp
    src/core/user_data_manager.cpp
    src/core/user_profile_types.cpp
    src/core/smart_file_filter.cpp
    src/core/windows_user_scanner.cpp
    src/core/permission_manager.cpp
    src/core/user_profile_backup_worker.cpp
    src/core/user_profile_restore_worker.cpp
    src/core/user_profile_restore_selection.cpp
    src/core/drive_scanner.cpp
    src/core/image_source.cpp
    src/core/iso_analyzer.cpp
    src/core/flash_coordinator.cpp
    src/core/uup_dump_api.cpp
    src/core/uup_iso_builder.cpp
    src/core/windows_iso_downloader.cpp
    src/core/linux_distro_catalog.cpp
    src/core/linux_iso_downloader.cpp
    src/core/windows_usb_creator.cpp
    src/core/windows_usb_creator_extract.cpp
    src/core/drive_unmounter.cpp
    src/core/streaming_decompressor.cpp
    src/core/gzip_decompressor.cpp
    src/core/bzip2_decompressor.cpp
    src/core/xz_decompressor.cpp
    src/core/decompressor_factory.cpp
    src/core/quick_action.cpp
    src/core/quick_action_controller.cpp
    src/core/app_action_registry.cpp
    src/core/app_action_bridge.cpp
    src/core/app_action_service.cpp
    src/core/app_readonly_actions.cpp
    src/core/app_mutating_actions.cpp
    include/sak/app_action_service.h
    src/core/hardware_inventory_scanner.cpp
    src/core/smart_disk_analyzer.cpp
    src/core/cpu_benchmark_worker.cpp
    src/core/disk_benchmark_worker.cpp
    src/core/memory_benchmark_worker.cpp
    src/core/stress_test_worker.cpp
    src/core/thermal_monitor.cpp
    src/core/diagnostic_report_generator.cpp
    src/core/diagnostic_controller.cpp
    src/core/file_recovery_engine.cpp
    src/core/file_explorer_types.cpp
    src/core/file_explorer_layout_metrics.cpp
    src/core/file_explorer_grouping.cpp
    src/core/file_explorer_group_proxy_model.cpp
    src/core/file_explorer_properties_calc.cpp
    src/core/file_explorer_storage_history.cpp
    src/core/file_explorer_status_center.cpp
    src/core/file_explorer_transfer_worker.cpp
    src/core/file_explorer_archive_worker.cpp
    src/core/file_explorer_item_model.cpp
    src/core/file_explorer_sort_filter_model.cpp
    src/core/file_explorer_command_registry.cpp
    src/core/file_explorer_session_store.cpp
    src/core/file_explorer_tag_store.cpp
    src/core/file_explorer_archive_service.cpp
    include/sak/file_explorer_archive_service.h
    src/gui/file_explorer_properties_dialog.cpp
    include/sak/file_explorer_properties_dialog.h
    src/core/file_management_file_system.cpp
    src/core/apfs_crypto.cpp
    src/core/apfs_keybag.cpp
    src/core/partition_apfs_file_system_reader.cpp
    src/core/partition_apfs_writer.cpp
    src/core/partition_ext_file_system_reader.cpp
    src/core/partition_hfs_file_system_reader.cpp
    src/core/partition_hfs_file_system_writer.cpp
    src/core/partition_raw_device_io.cpp
    src/core/partition_manager_types.cpp
    src/core/partition_file_system_detector.cpp
    src/core/partition_file_system_registry.cpp
    src/core/partition_file_system_tool_manifest.cpp
    src/core/partition_file_system_tool_runner.cpp
    src/core/storage_inventory_worker.cpp
    src/core/partition_safety_validator.cpp
    src/core/partition_script_builder.cpp
    src/core/partition_operation_planner.cpp
    src/core/partition_operation_queue.cpp
    src/core/partition_executor.cpp
    src/core/partition_manager_controller.cpp
    src/core/partition_report_generator.cpp
    include/sak/logger.h
    include/sak/app_paths.h
    include/sak/crash_reporter.h
    include/sak/file_hash.h
    include/sak/file_scanner.h
    include/sak/path_utils.h
    include/sak/input_validator.h
    include/sak/secure_memory.h
    include/sak/encryption.h
    include/sak/keep_awake.h
    include/sak/elevation_manager.h
    include/sak/elevation_gate.h
    include/sak/elevation_tier.h
    include/sak/elevated_pipe_protocol.h
    include/sak/elevation_broker.h
    src/core/elevation_broker.cpp
    include/sak/elevated_pipe_server.h
    src/core/elevated_pipe_server.cpp
    include/sak/elevated_task_dispatcher.h
    src/core/elevated_task_dispatcher.cpp
    include/sak/bundled_tools_manager.h
    include/sak/error_codes.h
    include/sak/aligned_buffer.h
    include/sak/format_utils.h
    include/sak/widget_helpers.h
    include/sak/view_empty_state.h
    include/sak/config_manager.h
    include/sak/app_scanner.h
    include/sak/drive_scanner.h
    include/sak/image_source.h
    include/sak/iso_analyzer.h
    include/sak/flash_coordinator.h
    include/sak/uup_dump_api.h
    include/sak/uup_iso_builder.h
    include/sak/windows_iso_downloader.h
    include/sak/linux_distro_catalog.h
    include/sak/linux_iso_downloader.h
    include/sak/windows_usb_creator.h
    include/sak/drive_unmounter.h
    include/sak/streaming_decompressor.h
    include/sak/gzip_decompressor.h
    include/sak/bzip2_decompressor.h
    include/sak/xz_decompressor.h
    include/sak/decompressor_factory.h
    include/sak/chocolatey_manager.h
    include/sak/vulnerability_scanner.h
    include/sak/package_matcher.h
    include/sak/migration_report.h
    include/sak/app_installation_worker.h
    include/sak/user_data_manager.h
    include/sak/user_profile_types.h
    include/sak/smart_file_filter.h
    include/sak/windows_user_scanner.h
    include/sak/permission_manager.h
    include/sak/user_profile_backup_worker.h
    include/sak/user_profile_restore_worker.h
    include/sak/diagnostic_types.h
    include/sak/hardware_inventory_scanner.h
    include/sak/smart_disk_analyzer.h
    include/sak/cpu_benchmark_worker.h
    include/sak/disk_benchmark_worker.h
    include/sak/memory_benchmark_worker.h
    include/sak/stress_test_worker.h
    include/sak/thermal_monitor.h
    include/sak/diagnostic_report_generator.h
    include/sak/diagnostic_controller.h
    include/sak/file_recovery_engine.h
    include/sak/file_explorer_types.h
    include/sak/file_explorer_layout_metrics.h
    include/sak/file_explorer_grouping.h
    include/sak/file_explorer_group_proxy_model.h
    include/sak/file_explorer_properties_calc.h
    include/sak/file_explorer_storage_history.h
    include/sak/file_explorer_status_center.h
    include/sak/file_explorer_transfer_worker.h
    include/sak/file_explorer_archive_worker.h
    include/sak/file_explorer_item_model.h
    include/sak/file_explorer_sort_filter_model.h
    include/sak/file_explorer_command_registry.h
    include/sak/file_explorer_session_store.h
    include/sak/file_explorer_tag_store.h
    include/sak/file_management_file_system.h
    include/sak/partition_apfs_file_system_reader.h
    include/sak/partition_apfs_writer.h
    include/sak/partition_ext_file_system_reader.h
    include/sak/partition_hfs_file_system_reader.h
    include/sak/partition_hfs_internal.h
    include/sak/partition_raw_device_io.h
    include/sak/partition_manager_types.h
    include/sak/partition_file_system_detector.h
    include/sak/partition_file_system_registry.h
    include/sak/partition_file_system_tool_manifest.h
    include/sak/partition_file_system_tool_runner.h
    include/sak/storage_inventory_worker.h
    include/sak/partition_safety_validator.h
    include/sak/partition_script_builder.h
    include/sak/partition_operation_planner.h
    include/sak/partition_operation_queue.h
    include/sak/partition_executor.h
    include/sak/partition_manager_controller.h
    include/sak/partition_report_generator.h
    src/core/advanced_search_worker.cpp
    src/core/advanced_search_controller.cpp
    src/core/regex_pattern_library.cpp
    include/sak/advanced_search_types.h
    include/sak/advanced_search_worker.h
    include/sak/advanced_search_controller.h
    include/sak/regex_pattern_library.h
    # Advanced Uninstall core
    src/core/program_enumerator.cpp
    src/core/uninstall_worker.cpp
    src/core/leftover_scanner.cpp
    src/core/cleanup_worker.cpp
    src/core/recycle_bin.cpp
    include/sak/recycle_bin.h
    src/core/restore_point_manager.cpp
    src/core/registry_snapshot_engine.cpp
    src/core/advanced_uninstall_controller.cpp
    include/sak/advanced_uninstall_types.h
    include/sak/program_enumerator.h
    include/sak/uninstall_worker.h
    include/sak/leftover_scanner.h
    include/sak/cleanup_worker.h
    include/sak/restore_point_manager.h
    include/sak/registry_snapshot_engine.h
    include/sak/advanced_uninstall_controller.h
    # Network Diagnostic core
    src/core/network_adapter_inspector.cpp
    src/core/connectivity_tester.cpp
    src/core/dns_diagnostic_tool.cpp
    src/core/port_scanner.cpp
    src/core/bandwidth_tester.cpp
    src/core/wifi_analyzer.cpp
    src/core/wifi_profile_scanner.cpp
    src/core/wifi_setup_script.cpp
    src/core/active_connections_monitor.cpp
    src/core/firewall_rule_auditor.cpp
    src/core/network_share_browser.cpp
    src/core/network_diagnostic_report_generator.cpp
    src/core/network_diagnostic_controller.cpp
    src/core/ethernet_config_manager.cpp
    include/sak/network_diagnostic_types.h
    include/sak/network_adapter_inspector.h
    include/sak/connectivity_tester.h
    include/sak/dns_diagnostic_tool.h
    include/sak/port_scanner.h
    include/sak/bandwidth_tester.h
    include/sak/wifi_analyzer.h
    include/sak/wifi_profile_scanner.h
    include/sak/active_connections_monitor.h
    include/sak/firewall_rule_auditor.h
    include/sak/network_share_browser.h
    include/sak/network_diagnostic_report_generator.h
    include/sak/network_diagnostic_controller.h
    include/sak/ethernet_config_manager.h
    include/sak/action_constants.h
    include/sak/layout_constants.h
    include/sak/message_box_helpers.h
    include/sak/network_constants.h
    include/sak/report_style_constants.h
    include/sak/style_constants.h
    # Email & PST/OST Inspector core
    src/core/pst_parser.cpp
    src/core/mbox_parser.cpp
    src/core/email_search_worker.cpp
    src/core/email_export_worker.cpp
    src/core/email_folder_selection.cpp
    src/core/email_attachment_saver.cpp
    src/core/email_profile_manager.cpp
    src/core/email_report_generator.cpp
    src/core/email_inspector_controller.cpp
    include/sak/email_constants.h
    include/sak/email_types.h
    include/sak/pst_parser.h
    include/sak/mbox_parser.h
    include/sak/email_search_worker.h
    include/sak/email_export_worker.h
    include/sak/email_folder_selection.h
    include/sak/email_attachment_saver.h
    include/sak/email_view_ids.h
    include/sak/email_profile_manager.h
    include/sak/email_report_generator.h
    include/sak/email_inspector_controller.h
    # OST/PST Converter
    src/core/ost_converter_controller.cpp
    src/core/ost_conversion_worker.cpp
    src/core/eml_writer.cpp
    src/core/mbox_writer.cpp
    src/core/html_email_writer.cpp
    src/core/pdf_email_writer.cpp
    src/core/deleted_item_scanner.cpp
    src/core/conversion_report_generator.cpp
    include/sak/ost_converter_types.h
    include/sak/ost_converter_constants.h
    include/sak/ost_converter_controller.h
    include/sak/ost_conversion_worker.h
    include/sak/eml_writer.h
    include/sak/mbox_writer.h
    include/sak/html_email_writer.h
    include/sak/pdf_email_writer.h
    include/sak/deleted_item_scanner.h
    include/sak/conversion_report_generator.h
    # Offline Deployment (Chocolatey internalization)
    src/core/nuget_version_range.cpp
    src/core/nuget_dependency_resolver.cpp
    src/core/install_script_parser.cpp
    src/core/script_rewriter.cpp
    src/core/package_internalization_engine.cpp
    src/core/offline_deployment_worker.cpp
    src/core/package_list_manager.cpp
    include/sak/offline_deployment_constants.h
    include/sak/nuget_version_range.h
    include/sak/nuget_dependency_resolver.h
    include/sak/install_script_parser.h
    include/sak/script_rewriter.h
    include/sak/package_internalization_engine.h
    include/sak/offline_deployment_worker.h
    include/sak/package_list_manager.h
)

# Threading infrastructure
set(THREADING_SOURCES
    src/threading/worker_base.cpp
    src/threading/organizer_worker.cpp
    src/threading/duplicate_finder_worker.cpp
    src/threading/flash_worker.cpp
    src/threading/partition_apply_worker.cpp
    src/threading/network_probe_worker.cpp
    include/sak/worker_base.h
    include/sak/organizer_worker.h
    include/sak/duplicate_finder_worker.h
    include/sak/flash_worker.h
    include/sak/partition_apply_worker.h
    include/sak/network_probe_worker.h
)

# GUI sources
set(GUI_SOURCES
    src/gui/main_window.cpp
    src/gui/view_empty_state.cpp
    src/gui/user_migration_panel.cpp
    src/gui/organizer_panel.cpp
    src/gui/file_explorer_sidebar.cpp
    src/gui/file_explorer_breadcrumb.cpp
    src/gui/file_explorer_icon_registry.cpp
    src/gui/file_explorer_command_bar.cpp
    src/gui/file_explorer_omnibar.cpp
    src/gui/file_explorer_status_center_widget.cpp
    src/gui/file_explorer_details_view.cpp
    src/gui/file_explorer_name_delegate.cpp
    src/gui/file_explorer_pane.cpp
    src/gui/file_explorer_details_pane.cpp
    src/gui/file_explorer_style.cpp
    src/gui/file_management_explorer_panel.cpp
    src/gui/log_viewer.cpp
    include/sak/main_window.h
    include/sak/user_migration_panel.h
    include/sak/organizer_panel.h
    include/sak/file_explorer_sidebar.h
    include/sak/file_explorer_breadcrumb.h
    include/sak/file_explorer_icon_registry.h
    include/sak/file_explorer_command_bar.h
    include/sak/file_explorer_omnibar.h
    include/sak/file_explorer_status_center_widget.h
    include/sak/file_explorer_details_view.h
    include/sak/file_explorer_name_delegate.h
    include/sak/file_explorer_pane.h
    include/sak/file_explorer_details_pane.h
    include/sak/file_explorer_style.h
    include/sak/file_management_explorer_panel.h
    include/sak/log_viewer.h
    include/sak/settings_dialog.h
    src/gui/settings_dialog.cpp
    src/gui/app_installation_panel.cpp
    src/gui/app_installation_panel_actions.cpp
    src/gui/app_installation_panel_table.cpp
    include/sak/app_installation_panel.h
    src/gui/vulnerability_panel.cpp
    include/sak/vulnerability_panel.h
    src/gui/per_user_customization_dialog.cpp
    include/sak/per_user_customization_dialog.h
    src/gui/user_profile_backup_wizard.cpp
    src/gui/user_profile_backup_wizard_pages.cpp
    src/gui/user_profile_backup_wizard_execute.cpp
    include/sak/user_profile_backup_wizard.h
    src/gui/user_profile_restore_wizard.cpp
    src/gui/user_profile_restore_wizard_pages.cpp
    src/gui/user_profile_restore_wizard_execute.cpp
    include/sak/user_profile_restore_wizard.h
    src/gui/image_flasher_panel.cpp
    include/sak/image_flasher_panel.h
    src/gui/partition_manager_panel.cpp
    include/sak/partition_manager_panel.h
    src/gui/windows_iso_download_dialog.cpp
    include/sak/windows_iso_download_dialog.h
    src/gui/linux_iso_download_dialog.cpp
    include/sak/linux_iso_download_dialog.h
    src/gui/image_flasher_settings_dialog.cpp
    include/sak/image_flasher_settings_dialog.h
    src/gui/install_summary_dialog.cpp
    include/sak/install_summary_dialog.h
    src/gui/detachable_log_window.cpp
    include/sak/detachable_log_window.h
    src/gui/ribbon_tool_button.cpp
    include/sak/ribbon_tool_button.h
    src/gui/follow_scroll_controller.cpp
    include/sak/follow_scroll_controller.h
    src/gui/windows11_theme.cpp
    include/sak/windows11_theme.h
    src/gui/splash_screen.cpp
    include/sak/splash_screen.h
    include/sak/quick_action.h
    include/sak/quick_action_controller.h
    src/core/quick_action_result_io.cpp
    include/sak/quick_action_result_io.h
    src/core/process_runner.cpp
    include/sak/process_runner.h
    src/core/network_transfer_runner.cpp
    include/sak/network_transfer_runner.h
    src/gui/diagnostic_benchmark_panel.cpp
    src/gui/diagnostic_benchmark_panel_slots.cpp
    include/sak/diagnostic_benchmark_panel.h
    src/gui/info_button.cpp
    include/sak/info_button.h
    # WiFi Manager panel + bundled QR library
    src/gui/wifi_manager_panel.cpp
    include/sak/wifi_manager_panel.h
    src/third_party/qrcodegen/qrcodegen.cpp
    src/third_party/qrcodegen/qrcodegen.hpp
    src/gui/advanced_search_panel.cpp
    include/sak/advanced_search_panel.h
    # Advanced Uninstall panel
    src/gui/advanced_uninstall_panel.cpp
    src/gui/advanced_uninstall_panel_dialogs.cpp
    include/sak/advanced_uninstall_panel.h
    # Network Diagnostic panel
    src/gui/network_diagnostic_panel.cpp
    include/sak/network_diagnostic_panel.h
    # Email & PST/OST Inspector panel
    src/gui/email_inspector_panel.cpp
    include/sak/email_inspector_panel.h
    src/gui/email_file_scanner_dialog.cpp
    include/sak/email_file_scanner_dialog.h
    src/gui/email_contacts_dialog.cpp
    include/sak/email_contacts_dialog.h
    src/gui/email_calendar_dialog.cpp
    include/sak/email_calendar_dialog.h
    src/gui/email_attachments_browser_dialog.cpp
    include/sak/email_attachments_browser_dialog.h
    # OST/PST Converter panel
    src/gui/ost_converter_widget.cpp
    include/sak/ost_converter_widget.h
)

set(AI_SOURCES)
if(SAK_ENABLE_AI_ASSISTANT)
    list(APPEND AI_SOURCES
        src/ai/ai_app_action_planner.cpp
        src/ai/ai_credential_store.cpp
        src/ai/ai_secret_redaction.cpp
        src/ai/ai_cancellation_token.cpp
        src/ai/ai_command_guard.cpp
        src/ai/ai_command_tool_planner.cpp
        src/ai/ai_chat_title.cpp
        src/ai/ai_conversation_store.cpp
        src/ai/ai_execution_broker.cpp
        src/ai/ai_human_gate.cpp
        src/ai/ai_human_gate_store.cpp
        src/ai/ai_lease_manager.cpp
        src/ai/ai_mcp_http_client.cpp
        src/ai/ai_mcp_session_pool.cpp
        src/ai/ai_mcp_stdio_client.cpp
        src/ai/ai_mcp_stdio_session.cpp
        src/ai/ai_openai_model_client.cpp
        src/ai/ai_orchestrator.cpp
        src/ai/ai_offline_downloader_tool_runner.cpp
        src/ai/ai_package_tool_planner.cpp
        src/ai/ai_package_selection.cpp
        src/ai/ai_paths.cpp
        src/ai/ai_prompt_assembler.cpp
        src/ai/ai_provider_gateway.cpp
        src/ai/ai_provider_gateway_authorization.cpp
        src/ai/ai_provider_gateway_tool_runner.cpp
        src/ai/ai_provider_registry.cpp
        src/ai/ai_win32_gui_runner.cpp
        src/ai/ai_recovery_policy.cpp
        src/ai/ai_async_tool_runner.cpp
        src/ai/ai_skill.cpp
        src/ai/ai_skill_store.cpp
        src/ai/ai_run_state.cpp
        src/ai/ai_run_state_store.cpp
        src/ai/ai_subagent_runner.cpp
        src/ai/ai_subagent_tool_executor.cpp
        src/ai/ai_token_usage_tracker.cpp
        src/ai/ai_tool_call_router.cpp
        src/ai/ai_tool_dispatcher.cpp
        src/ai/ai_tool_health_ledger.cpp
        src/ai/ai_tool_loop_detector.cpp
        src/ai/ai_tool_policy.cpp
        src/ai/ai_tool_result_recorder.cpp
        src/ai/ai_tool_turn.cpp
        src/ai/ai_trace_store.cpp
        src/ai/ai_workflow_powershell_tool_runner.cpp
        src/ai/ai_workflow_clarifier.cpp
        src/ai/ai_workflow_placeholders.cpp
        src/ai/ai_workflow_store.cpp
        src/ai/ai_workflow_template.cpp
        src/ai/openai_responses_client.cpp
        src/gui/ai_transcript_view.cpp
        include/sak/ai/ai_app_action_planner.h
        include/sak/ai/ai_async_tool_runner.h
        include/sak/ai/ai_credential_store.h
        include/sak/ai/ai_cancellation_token.h
        include/sak/ai/ai_command_guard.h
        include/sak/ai/ai_command_tool_planner.h
        include/sak/ai/ai_chat_title.h
        include/sak/ai/ai_conversation_store.h
        include/sak/ai/ai_execution_broker.h
        include/sak/ai/ai_human_gate.h
        include/sak/ai/ai_human_gate_store.h
        include/sak/ai/ai_lease_manager.h
        include/sak/ai/ai_mcp_http_client.h
        include/sak/ai/ai_mcp_jsonrpc.h
        include/sak/ai/ai_mcp_session_pool.h
        include/sak/ai/ai_mcp_stdio_client.h
        include/sak/ai/ai_mcp_stdio_session.h
        include/sak/ai/ai_openai_model_client.h
        include/sak/ai/ai_orchestrator.h
        include/sak/ai/ai_offline_downloader_tool_runner.h
        include/sak/ai/ai_package_tool_planner.h
        include/sak/ai/ai_package_selection.h
        include/sak/ai/ai_paths.h
        include/sak/ai/ai_prompt_assembler.h
        include/sak/ai/ai_provider_gateway.h
        include/sak/ai/ai_provider_gateway_tool_runner.h
        include/sak/ai/ai_provider_registry.h
        include/sak/ai/ai_recovery_policy.h
        include/sak/ai/ai_skill.h
        include/sak/ai/ai_skill_store.h
        include/sak/ai/ai_run_state.h
        include/sak/ai/ai_run_state_store.h
        include/sak/ai/ai_subagent_runner.h
        include/sak/ai/ai_subagent_tool_executor.h
        include/sak/ai/ai_tool_loop_detector.h
        include/sak/ai/ai_token_usage_tracker.h
        include/sak/ai/ai_tool_call_router.h
        include/sak/ai/ai_tool_dispatcher.h
        include/sak/ai/ai_tool_health_ledger.h
        include/sak/ai/ai_tool_policy.h
        include/sak/ai/ai_tool_result_recorder.h
        include/sak/ai/ai_tool_turn.h
        include/sak/ai/ai_trace_store.h
        include/sak/ai/ai_workflow_placeholders.h
        include/sak/ai/ai_workflow_powershell_tool_runner.h
        include/sak/ai/ai_workflow_store.h
        include/sak/ai/ai_workflow_template.h
        include/sak/ai/openai_response_types.h
        include/sak/ai/openai_responses_client.h
        src/gui/ai_assistant_panel.cpp
        include/sak/ai_assistant_panel.h
        include/sak/ai_transcript_view.h
    )
endif()

# Quick Actions
set(QUICK_ACTIONS_SOURCES
    # System Optimization
    src/actions/optimize_power_settings_action.cpp

    # Maintenance
    src/actions/verify_system_files_action.cpp
    src/actions/check_disk_errors_action.cpp
    src/actions/reset_network_action.cpp

    # Troubleshooting
    src/actions/generate_system_report_action.cpp

    # Emergency Recovery
    src/actions/screenshot_settings_action.cpp
    src/actions/backup_bitlocker_keys_action.cpp

    # Headers for all actions
    include/sak/actions/optimize_power_settings_action.h
    include/sak/actions/verify_system_files_action.h
    include/sak/actions/check_disk_errors_action.h
    include/sak/actions/reset_network_action.h
    include/sak/actions/generate_system_report_action.h
    include/sak/actions/screenshot_settings_action.h
    include/sak/actions/backup_bitlocker_keys_action.h
)

# Main entry point
set(MAIN_SOURCE src/main.cpp)

# Windows resource file for icon and version info
if(WIN32)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/sak_utility.manifest.in"
        "${CMAKE_CURRENT_BINARY_DIR}/sak_utility.manifest"
        @ONLY
    )
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/sak_utility.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/sak_utility.rc"
        @ONLY
    )
    set(RESOURCE_FILES "${CMAKE_CURRENT_BINARY_DIR}/sak_utility.rc")
endif()

# Qt icon resources (processed by AUTORCC)
list(APPEND RESOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons.qrc")
if(SAK_ENABLE_AI_ASSISTANT)
    list(APPEND RESOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/ai.qrc")
endif()

# Combine all sources
set(ALL_SOURCES
    ${CORE_SOURCES}
    ${THREADING_SOURCES}
    ${GUI_SOURCES}
    ${AI_SOURCES}
    ${QUICK_ACTIONS_SOURCES}
    ${MAIN_SOURCE}
    ${RESOURCE_FILES}
)

# ============================================================================
# Vendored lzfse/lzvn reference library (Apple, BSD-3) for HFS+ decmpfs codecs
# ============================================================================

add_library(sak_lzfse STATIC
    third_party/lzfse/src/lzfse_decode.c
    third_party/lzfse/src/lzfse_decode_base.c
    third_party/lzfse/src/lzfse_encode.c
    third_party/lzfse/src/lzfse_encode_base.c
    third_party/lzfse/src/lzfse_fse.c
    third_party/lzfse/src/lzvn_decode_base.c
    third_party/lzfse/src/lzvn_encode_base.c
    third_party/lzfse/sak_lzvn_shim.c
)
target_include_directories(sak_lzfse PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lzfse/src
)
if(MSVC)
    target_compile_options(sak_lzfse PRIVATE /w)
else()
    target_compile_options(sak_lzfse PRIVATE -w)
endif()

# ============================================================================
# Executable Target
# ============================================================================

add_executable(sak_utility ${ALL_SOURCES})

# Browser-extension installer: drives the panel's Install/Uninstall button in-process.
# Has a non-Windows stub, so it compiles on every platform the panel does.
target_sources(sak_utility PRIVATE
    src/win32mcp/browser_extension_installer.cpp
    include/sak/win32mcp/browser_extension_installer.h
)

# The win32 MCP server + Chrome browser-control host are folded into this one app binary
# instead of a second exe: main() dispatches to the helper modes (env var / chrome-extension
# origin) before any GUI init. Windows-only (raw Win32 + Chrome DevTools Protocol).
if(PLATFORM_WINDOWS)
    target_sources(sak_utility PRIVATE
        src/win32mcp/win32_mcp_entry.cpp
        src/win32mcp/win32_mcp_dispatch.cpp
        src/win32mcp/win32_mcp_tools.cpp
        src/win32mcp/win32_mcp_desktop.cpp
        src/win32mcp/win32_mcp_dialog_choice.cpp
        src/win32mcp/win32_mcp_uia_ref.cpp
        src/win32mcp/win32_mcp_capture.cpp
        src/win32mcp/win32_mcp_geometry.cpp
        src/win32mcp/win32_mcp_ocr.cpp
        src/win32mcp/win32_mcp_text_match.cpp
        src/win32mcp/win32_mcp_json_clamp.cpp
        src/win32mcp/win32_mcp_watch.cpp
        src/win32mcp/win32_mcp_fingerprint.cpp
        src/win32mcp/win32_mcp_input.cpp
        src/win32mcp/win32_mcp_input_plan.cpp
        src/win32mcp/win32_mcp_key_chord.cpp
        src/win32mcp/browser_contract.cpp
        src/win32mcp/browser_bridge.cpp
        src/win32mcp/browser_bridge_security.cpp
        src/win32mcp/browser_bridge_pipe.cpp
        src/win32mcp/browser_bridge_relay.cpp
        src/win32mcp/browser_control.cpp
        src/win32mcp/native_messaging.cpp
        include/sak/win32mcp/win32_mcp_entry.h
        include/sak/win32mcp/win32_mcp_desktop.h
        include/sak/win32mcp/win32_mcp_capture.h
        include/sak/win32mcp/win32_mcp_ocr.h
        include/sak/win32mcp/win32_mcp_watch.h
        include/sak/win32mcp/win32_mcp_input.h
    )
    # shcore: GetDpiForMonitor (per-monitor DPI in list_monitors). gdi32/user32: screen capture
    # (BitBlt/PrintWindow/CreateDIBSection) in the desktop-control tools. runtimeobject: WinRT
    # activation (RoGetActivationFactory) for the Windows.Media.Ocr OCR tools.
    target_link_libraries(sak_utility PRIVATE shcore gdi32 user32 runtimeobject)
endif()

# Enable Qt MOC, UIC, and RCC
set_target_properties(sak_utility PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
)

target_compile_definitions(sak_utility PRIVATE
    SAK_ENABLE_AI_ASSISTANT=$<BOOL:${SAK_ENABLE_AI_ASSISTANT}>
)

# Link libraries
target_link_libraries(sak_utility PRIVATE
    Qt6::Core
    # QZipReader/QZipWriter (explorer zip compress/extract) live in QtCore's
    # private headers; no third-party archive library is vendored for this.
    Qt6::CorePrivate
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Network
    Qt6::Xml
    Threads::Threads
    ZLIB::ZLIB
    BZip2::BZip2
    LibLZMA::LibLZMA
    sak_lzfse
    ${PLATFORM_LIBS}
)

# The /NODEFAULTLIB CRT-conflict suppressions that used to sit here (and on
# sak_apfs_writer_cli, sak_hfs_writer_cli, sak_elevated_helper and
# test_ai_assistant_panel_tool_dispatch) are GONE. They existed to silence
# LNK4098 from mixing a static-CRT vcpkg triplet with the dynamic CRT - that is,
# to hide the very hazard the CRT coherence check above now refuses to configure.
# With the app CRT, the vcpkg triplet CRT and Qt all required to agree, there is
# no conflict left to suppress, and telling the linker to drop a CRT it needs is
# not something this build should be doing silently.

# Include directories
target_include_directories(sak_utility PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/qrcodegen
)

# ============================================================================
# APFS Writer CLI (Partition Manager raw/image apply bridge)
# ============================================================================

add_executable(sak_apfs_writer_cli
    src/tools/sak_apfs_writer_cli.cpp
    src/core/apfs_crypto.cpp
    src/core/apfs_keybag.cpp
    src/core/partition_apfs_file_system_reader.cpp
    src/core/partition_apfs_writer.cpp
    src/core/partition_file_system_detector.cpp
    src/core/partition_raw_device_io.cpp
    include/sak/apfs_crypto.h
    include/sak/apfs_keybag.h
    include/sak/partition_apfs_writer.h
    include/sak/partition_apfs_file_system_reader.h
    include/sak/partition_file_system_detector.h
    include/sak/partition_raw_device_io.h
)

set_target_properties(sak_apfs_writer_cli PROPERTIES
    AUTOMOC ON
    WIN32_EXECUTABLE FALSE
)

target_include_directories(sak_apfs_writer_cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/include
)

target_link_libraries(sak_apfs_writer_cli PRIVATE
    Qt6::Core
    sak_lzfse
    ${PLATFORM_LIBS}
)

target_compile_definitions(sak_apfs_writer_cli PRIVATE
    QT_NO_KEYWORDS
    QT_DEPRECATED_WARNINGS
    QT_DISABLE_DEPRECATED_BEFORE=0x060000
)

add_dependencies(sak_utility sak_apfs_writer_cli)

# ============================================================================
# HFS Writer CLI (Partition Manager image-only HFS apply bridge)
# ============================================================================

add_executable(sak_hfs_writer_cli
    src/tools/sak_hfs_writer_cli.cpp
    src/core/partition_hfs_file_system_reader.cpp
    src/core/partition_hfs_file_system_writer.cpp
    src/core/partition_raw_device_io.cpp
    include/sak/partition_hfs_file_system_reader.h
    include/sak/partition_hfs_internal.h
    include/sak/partition_raw_device_io.h
)

set_target_properties(sak_hfs_writer_cli PROPERTIES
    AUTOMOC ON
    WIN32_EXECUTABLE FALSE
)

target_include_directories(sak_hfs_writer_cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/include
)

target_link_libraries(sak_hfs_writer_cli PRIVATE
    Qt6::Core
    sak_lzfse
    ${PLATFORM_LIBS}
)

target_compile_definitions(sak_hfs_writer_cli PRIVATE
    QT_NO_KEYWORDS
    QT_DEPRECATED_WARNINGS
    QT_DISABLE_DEPRECATED_BEFORE=0x060000
)

add_dependencies(sak_utility sak_hfs_writer_cli)

# ============================================================================
# Native win32 MCP server (assistant computer + browser control)
# ============================================================================
# Formerly a standalone sak_win32_mcp.exe; now FOLDED into the app binary (its sources
# are compiled into sak_utility above and its entry points dispatched from main()). One
# binary serves GUI, the MCP server (SAK_WIN32_MCP_MODE env), and the Chrome native host
# (chrome-extension:// origin) -- so there is a single executable to ship and sign.

# ============================================================================
# Elevated Helper Executable (Phase 2 -- per-task elevation)
# ============================================================================

if(PLATFORM_WINDOWS)
    # Helper sources: action implementations + IPC layer + entry point
    set(HELPER_SOURCES
        src/elevated/elevated_helper_main.cpp
        src/core/app_paths.cpp
        src/core/elevated_pipe_server.cpp
        src/core/elevated_task_dispatcher.cpp
        src/core/logger.cpp
        src/core/quick_action.cpp
        src/core/process_runner.cpp
        src/core/permission_manager.cpp
        src/core/elevation_manager.cpp
        src/actions/verify_system_files_action.cpp
        src/actions/check_disk_errors_action.cpp
        src/actions/reset_network_action.cpp
        src/actions/backup_bitlocker_keys_action.cpp
        include/sak/elevated_pipe_protocol.h
        include/sak/app_paths.h
        include/sak/elevated_pipe_server.h
        include/sak/elevated_task_dispatcher.h
        include/sak/permission_manager.h
        include/sak/quick_action.h
        include/sak/actions/verify_system_files_action.h
        include/sak/actions/check_disk_errors_action.h
        include/sak/actions/reset_network_action.h
        include/sak/actions/backup_bitlocker_keys_action.h
    )

    # Resource file for embedding the requireAdministrator manifest
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/sak_elevated_helper.manifest"
        "${CMAKE_CURRENT_BINARY_DIR}/sak_elevated_helper.manifest"
        COPYONLY
    )

    # Generate a minimal .rc that embeds the manifest
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/sak_elevated_helper.rc"
        "#include <winuser.h>\nCREATEPROCESS_MANIFEST_RESOURCE_ID RT_MANIFEST \"sak_elevated_helper.manifest\"\n"
    )

    add_executable(sak_elevated_helper
        ${HELPER_SOURCES}
        "${CMAKE_CURRENT_BINARY_DIR}/sak_elevated_helper.rc"
    )

    set_target_properties(sak_elevated_helper PROPERTIES
        AUTOMOC ON
        WIN32_EXECUTABLE FALSE
    )

    target_include_directories(sak_elevated_helper PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/include
    )

    target_link_libraries(sak_elevated_helper PRIVATE
        Qt6::Core
        Qt6::Gui
        Threads::Threads
        ${PLATFORM_LIBS}
    )

    target_compile_definitions(sak_elevated_helper PRIVATE
        QT_NO_KEYWORDS
        QT_DEPRECATED_WARNINGS
        QT_DISABLE_DEPRECATED_BEFORE=0x060000
    )

    # Suppress linker-generated manifest -- we embed via RC
    if(MSVC)
        set_target_properties(sak_elevated_helper PROPERTIES
            LINK_FLAGS "/MANIFEST:NO"
        )
    endif()

    # Copy helper to main output directory so they sit side-by-side
    add_custom_command(TARGET sak_elevated_helper POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:sak_elevated_helper>
            $<TARGET_FILE_DIR:sak_utility>/sak_elevated_helper.exe
        COMMENT "Copying elevated helper next to sak_utility"
    )
    add_dependencies(sak_utility sak_elevated_helper)
endif()

# Check for bundled smartmontools (required for SMART diagnostics)
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/smartmontools/smartctl.exe")
    message(FATAL_ERROR "smartmontools not found! SMART disk diagnostics will be unavailable.\n"
                        "Run 'powershell -File scripts/bundle_smartmontools.ps1' to bundle smartctl.exe.")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/uup/aria2c.exe")
    message(FATAL_ERROR "aria2c not found! ISO downloads will be unavailable.\n"
                        "Run 'powershell -File scripts/bundle_uup_tools.ps1' to bundle UUP tools.")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/uup/uupmc/UUPMediaConverter.exe")
    message(FATAL_ERROR "UUP Media Creator not found! UUP-to-ISO conversion will be unavailable.\n"
                        "Run 'powershell -File scripts/bundle_uup_tools.ps1' to bundle UUP tools.")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/iperf3/iperf3.exe")
    message(FATAL_ERROR "iPerf3 not found! LAN bandwidth testing will be unavailable.\n"
                        "Run 'powershell -File scripts/bundle_iperf3.ps1' to bundle iperf3.exe.")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/chocolatey/choco.exe")
    message(FATAL_ERROR "Chocolatey not found! App installation features will be unavailable.\n"
                        "Run 'powershell -File scripts/bundle_chocolatey.ps1' to bundle choco.exe.")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/mcp/_build")
    message(FATAL_ERROR "Temporary MCP build files found under tools/mcp/_build. "
                        "Remove that directory before building so release packages stay portable.")
endif()

# The win32 desktop/browser automation provider is now the native sak_win32_mcp target
# (built below), not a bundled external binary, so nothing to require here.

# The signed browser-control CRX is committed (browser/dist/) and force-installed by the
# assistant's extension installer. Guard against a release silently shipping without it:
# if it is missing the install fails closed on every customer PC. Restore it from git, or
# re-sign with 'powershell -File browser/pack-extension.ps1'.
if(PLATFORM_WINDOWS AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/browser/dist/sak_browser_control.crx")
    message(FATAL_ERROR "browser/dist/sak_browser_control.crx not found. Restore it from git "
                        "or re-sign with 'powershell -File browser/pack-extension.ps1'.")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/filesystem/manifest.json")
    message(FATAL_ERROR "Partition filesystem tool manifest not bundled. "
                        "tools/filesystem/manifest.json is required even when no non-native "
                        "filesystem tools are approved yet.")
endif()

# Copy embedded tools directory to output
add_custom_command(TARGET sak_utility POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        "-DSAK_TOOLS_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tools"
        "-DSAK_TOOLS_DESTINATION=$<TARGET_FILE_DIR:sak_utility>/tools"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_tools_bundle.cmake"
    COMMENT "Copying bundled tools to output directory"
    VERBATIM
)

# The assistant reads its provider policy and its app-action recipes from these
# directories. A build that quietly omits them ships an AI panel running on a
# partial tool policy, which is exactly the kind of "configured but not really"
# state the rest of this file refuses to produce.
if(SAK_ENABLE_AI_ASSISTANT)
    foreach(_sak_ai_data_dir providers app_manifests)
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/ai/${_sak_ai_data_dir}")
            message(FATAL_ERROR
                "SAK_ENABLE_AI_ASSISTANT is ON but resources/ai/${_sak_ai_data_dir} is missing. "
                "The assistant loads its tool/provider policy from that directory, so the build "
                "would ship a partial security configuration. Restore it from git, or configure "
                "with -DSAK_ENABLE_AI_ASSISTANT=OFF.")
        endif()
    endforeach()
endif()

# Bundled AI policy data is staged by a target of its own, and that target runs
# on EVERY build. Two staleness hazards made the old POST_BUILD copies unsafe
# for security-relevant data:
#   - a POST_BUILD command only runs when its target relinks, so editing a
#     manifest and rebuilding left the OLD manifest sitting beside the exe;
#   - copy_directory never deletes, so a manifest removed from the source tree
#     kept shipping forever.
# The staging purges each destination first, so what ships is what is in the
# source tree - nothing older, nothing withdrawn.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/ai/providers")
    add_custom_target(sak_stage_ai_providers ALL
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_FILE_DIR:sak_utility>/data/ai/providers"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/ai/providers"
            "$<TARGET_FILE_DIR:sak_utility>/data/ai/providers"
        COMMAND ${CMAKE_COMMAND} -E rm -f
            "$<TARGET_FILE_DIR:sak_utility>/data/ai/providers/provider_secrets.example.json"
            "$<TARGET_FILE_DIR:sak_utility>/data/ai/providers/provider_secrets.local.json"
        COMMENT "Staging bundled AI provider manifests to output directory"
        VERBATIM
    )
    add_dependencies(sak_stage_ai_providers sak_utility)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/ai/app_manifests")
    add_custom_target(sak_stage_ai_app_manifests ALL
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_FILE_DIR:sak_utility>/data/ai/app_manifests"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/ai/app_manifests"
            "$<TARGET_FILE_DIR:sak_utility>/data/ai/app_manifests"
        COMMENT "Staging bundled AI app manifests to output directory"
        VERBATIM
    )
    add_dependencies(sak_stage_ai_app_manifests sak_utility)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/sak_splash.png")
    add_custom_command(TARGET sak_utility POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/sak_splash.png"
            "$<TARGET_FILE_DIR:sak_utility>/sak_splash.png"
        COMMENT "Copying splash image to output directory"
    )
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.ico")
    add_custom_command(TARGET sak_utility POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.ico"
            "$<TARGET_FILE_DIR:sak_utility>/icon.ico"
        COMMENT "Copying icon to output directory"
    )
endif()

# Signed browser-control extension CRX: the assistant force-installs this into Chrome.
# Copied next to the exe so BrowserExtensionInstaller finds it at <appDir>/<crx name>.
# Staged by an always-run target for the same reason as the AI manifests: a re-signed
# CRX must not wait for an unrelated relink before it reaches the output directory.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/browser/dist/sak_browser_control.crx")
    add_custom_target(sak_stage_browser_crx ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/browser/dist/sak_browser_control.crx"
            "$<TARGET_FILE_DIR:sak_utility>/sak_browser_control.crx"
        COMMENT "Staging browser-control extension CRX to output directory"
        VERBATIM
    )
    add_dependencies(sak_stage_browser_crx sak_utility)
endif()

# Compile definitions
target_compile_definitions(sak_utility PRIVATE
    QT_NO_KEYWORDS          # Avoid Qt keyword conflicts
    QT_DEPRECATED_WARNINGS  # Warn about deprecated Qt features
    QT_DISABLE_DEPRECATED_BEFORE=0x060000  # Disable deprecated Qt APIs
)

# Icon bootstrap from the parent directory (see the two uses below).
option(SAK_IMPORT_PARENT_ICON
    "Overwrite the tracked resources/ icon with ../icon.ico|.icns during configure" OFF)

# Windows-specific settings
if(PLATFORM_WINDOWS)
    # Set Windows subsystem. This has to be per-configuration, not
    # "if(CMAKE_BUILD_TYPE STREQUAL Release)": under the Visual Studio
    # multi-config generator CMAKE_BUILD_TYPE is empty and gets FORCEd to
    # Release at the top of this file, so that test was true even for a
    # --config Debug build and Debug lost its console. WIN32_EXECUTABLE accepts
    # generator expressions, which are evaluated per configuration.
    set_target_properties(sak_utility PROPERTIES
        WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>
    )

    # Manifest is embedded via the RC file; suppress linker-generated manifest to avoid conflict
    if(MSVC)
        set_target_properties(sak_utility PROPERTIES
            LINK_FLAGS "/MANIFEST:NO"
        )
    endif()

    # Icon bootstrap from the parent directory. resources/icon.ico is tracked, so
    # importing it unconditionally let an UNTRACKED file one level above the
    # repository root overwrite a committed resource during configure - the build
    # output would depend on whatever happens to sit outside the checkout. Keep
    # the bootstrap for a tree that has no icon yet, and require an explicit
    # opt-in to replace one that does.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../icon.ico"
       AND (SAK_IMPORT_PARENT_ICON OR NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.ico"))
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/../icon.ico"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.ico"
            COPYONLY
        )
        message(STATUS "Icon:               imported from ../icon.ico into resources/")
    endif()
endif()

# macOS-specific settings
if(PLATFORM_MACOS)
    set_target_properties(sak_utility PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "SAK Utility"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.sak.utility"
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    )

    # Icon bootstrap - same rule as the Windows .ico above: an untracked file
    # outside the checkout does not silently replace a committed resource.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../icon.icns"
       AND (SAK_IMPORT_PARENT_ICON OR NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.icns"))
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/../icon.icns"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.icns"
            COPYONLY
        )
        message(STATUS "Icon:               imported from ../icon.icns into resources/")
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.icns")
        set_target_properties(sak_utility PROPERTIES
            MACOSX_BUNDLE_ICON_FILE icon.icns
        )
    endif()
endif()

# ============================================================================
# Testing
# ============================================================================

option(BUILD_TESTING "Build tests" ON)

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

# ============================================================================
# Installation
# ============================================================================

# Everything the app resolves next to its own executable at run time has to be
# part of the install, not only of the build directory. sak_utility launches the
# elevated helper and the two writer CLIs by path, and refuses to run features
# whose bundled tool or policy file is absent - so an install tree holding only
# the GUI exe is a package that fails on the customer's PC, not a smaller one.
install(TARGETS sak_utility
    RUNTIME DESTINATION bin
    BUNDLE DESTINATION .
)

install(TARGETS sak_apfs_writer_cli sak_hfs_writer_cli
    RUNTIME DESTINATION bin
)

if(PLATFORM_WINDOWS)
    install(TARGETS sak_elevated_helper
        RUNTIME DESTINATION bin
    )
endif()

# Bundled technician tools (smartctl, aria2c, UUP media converter, iperf3,
# portable chocolatey, filesystem tool manifest). Their presence is already a
# configure-time FATAL_ERROR above; ship what was verified.
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/"
    DESTINATION bin/tools
)

# AI provider/app policy, minus the secret templates that must never ship.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/ai/providers")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources/ai/providers/"
        DESTINATION bin/data/ai/providers
        PATTERN "provider_secrets.example.json" EXCLUDE
        PATTERN "provider_secrets.local.json" EXCLUDE
    )
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/ai/app_manifests")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources/ai/app_manifests/"
        DESTINATION bin/data/ai/app_manifests
    )
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/sak_splash.png")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/sak_splash.png"
        DESTINATION bin
    )
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.ico")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.ico"
        DESTINATION bin
    )
endif()

# Ship the signed browser-control CRX beside the installed exe so the assistant's
# extension installer resolves it at <appDir>/sak_browser_control.crx on customer PCs.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/browser/dist/sak_browser_control.crx")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/browser/dist/sak_browser_control.crx"
        DESTINATION bin
    )
endif()

# Qt deployment
if(PLATFORM_WINDOWS)
    # Use windeployqt. Without it the output directory holds an executable that
    # cannot start (no Qt DLLs, no platform plugin), so a missing windeployqt is
    # a broken package, not a degraded one - fail configure instead of leaving a
    # build that only appears to have produced something shippable.
    find_program(WINDEPLOYQT windeployqt HINTS "${Qt6_DIR}/../../../bin")
    if(NOT WINDEPLOYQT)
        message(FATAL_ERROR
            "windeployqt was not found (searched PATH and '${Qt6_DIR}/../../../bin'). It is "
            "required to place the Qt runtime and platform plugins beside sak_utility.exe; "
            "without it the built and installed trees cannot run. Add the Qt bin directory to "
            "PATH or pass -DCMAKE_PREFIX_PATH pointing at the Qt installation.")
    endif()
    # No --release/--debug here on purpose: windeployqt reads the binary and
    # deploys the matching Qt build. Hard-coding --release deployed release Qt
    # DLLs next to a --config Debug executable, which links the debug ones.
    add_custom_command(TARGET sak_utility POST_BUILD
        COMMAND ${WINDEPLOYQT} --no-translations --no-compiler-runtime $<TARGET_FILE:sak_utility>
        COMMENT "Running windeployqt..."
    )
    # ... and the same for the install tree, which install(TARGETS) alone leaves
    # without any Qt runtime at all.
    install(CODE "
        execute_process(
            COMMAND \"${WINDEPLOYQT}\" --no-translations --no-compiler-runtime
                    \"\${CMAKE_INSTALL_PREFIX}/bin/sak_utility.exe\"
            RESULT_VARIABLE _sak_windeployqt_result)
        if(NOT _sak_windeployqt_result EQUAL 0)
            message(FATAL_ERROR
                \"windeployqt failed on the installed executable (result \${_sak_windeployqt_result}); \"
                \"the installed tree would be missing the Qt runtime.\")
        endif()
    ")

    # Bundle dynamic vcpkg runtime DLLs when using a dynamic triplet.
    # Each group lists the accepted file names for ONE library, separated by "|":
    # vcpkg has renamed these across versions and the alias that exists is the
    # one to ship.
    set(SAK_VCPKG_RUNTIME_DLL_GROUPS
        "z.dll|zlib1.dll"
        "bz2.dll|libbz2.dll"
        "liblzma.dll|lzma.dll"
    )
    # A triplet whose name contains "-static" supplies static libraries, so there
    # is no DLL to bundle and nothing to check. Any other triplet supplies import
    # libraries, and then the DLLs are load-bearing: shipping without them
    # produces an exe that dies at load time on the customer's PC.
    if(DEFINED VCPKG_TARGET_TRIPLET AND NOT "${VCPKG_TARGET_TRIPLET}" MATCHES "-static")
        set(_sak_vcpkg_dlls_required TRUE)
    else()
        set(_sak_vcpkg_dlls_required FALSE)
    endif()
    if(_sak_vcpkg_dlls_required AND NOT DEFINED VCPKG_INSTALLED_DIR)
        message(FATAL_ERROR
            "vcpkg triplet '${VCPKG_TARGET_TRIPLET}' supplies shared libraries but "
            "VCPKG_INSTALLED_DIR is not set, so zlib/bzip2/liblzma DLLs cannot be located "
            "or bundled. Configure with the vcpkg toolchain file so VCPKG_INSTALLED_DIR is "
            "defined, or use a -static triplet.")
    endif()
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        set(SAK_VCPKG_BIN "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
        foreach(group ${SAK_VCPKG_RUNTIME_DLL_GROUPS})
            string(REPLACE "|" ";" group_dlls "${group}")
            set(group_found FALSE)
            foreach(dll ${group_dlls})
                set(runtime_dll "${SAK_VCPKG_BIN}/${dll}")
                if(EXISTS "${runtime_dll}")
                    add_custom_command(TARGET sak_utility POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${runtime_dll}"
                            "$<TARGET_FILE_DIR:sak_utility>"
                        COMMENT "Bundling ${dll} from vcpkg..."
                    )
                    install(FILES "${runtime_dll}" DESTINATION bin)
                    set(group_found TRUE)
                endif()
            endforeach()
            if(_sak_vcpkg_dlls_required AND NOT group_found)
                message(FATAL_ERROR
                    "vcpkg triplet '${VCPKG_TARGET_TRIPLET}' is a shared-library triplet but none "
                    "of '${group}' was found in '${SAK_VCPKG_BIN}'. The app links that library, so "
                    "the package would ship without a DLL it loads at startup. Install the port for "
                    "this triplet, or build with -DVCPKG_TARGET_TRIPLET=x64-windows-static-md.")
            endif()
        endforeach()
    endif()

    # Bundle MSVC runtime DLLs (required by Qt DLLs)
    # Find and copy the runtime DLLs to the output directory
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)

    # This project is required to build against the dynamic CRT (see the CRT
    # coherence gate above), so msvcp140/vcruntime140 are not optional extras -
    # the app cannot start without them on a PC that has no VC redistributable
    # installed. Neither discovery path is allowed to end in a warning.
    if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        foreach(lib ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS})
            add_custom_command(TARGET sak_utility POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${lib}"
                    "$<TARGET_FILE_DIR:sak_utility>"
                COMMENT "Bundling ${lib}..."
            )
        endforeach()
        install(FILES ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION bin)
    else()
        # Fallback: manually locate MSVC runtime DLLs across installed VS editions.
        file(GLOB MSVC_VERSIONS
            "C:/Program Files/Microsoft Visual Studio/2022/*/VC/Redist/MSVC/*"
            "C:/Program Files (x86)/Microsoft Visual Studio/2022/*/VC/Redist/MSVC/*"
        )
        list(SORT MSVC_VERSIONS)
        list(REVERSE MSVC_VERSIONS)

        if(NOT MSVC_VERSIONS)
            message(FATAL_ERROR
                "MSVC runtime DLLs could not be located: InstallRequiredSystemLibraries found "
                "none and no VC redistributable directory exists under the Visual Studio 2022 "
                "install. This build links the dynamic CRT, so the packaged app would fail to "
                "start on any PC without the VC++ redistributable. Install the "
                "'C++ 2022 Redistributable MSMs / Redist' component of Visual Studio.")
        endif()

        list(GET MSVC_VERSIONS 0 LATEST_MSVC)
        set(MSVC_DLLS
            "${LATEST_MSVC}/x64/Microsoft.VC143.CRT/msvcp140.dll"
            "${LATEST_MSVC}/x64/Microsoft.VC143.CRT/vcruntime140.dll"
            "${LATEST_MSVC}/x64/Microsoft.VC143.CRT/vcruntime140_1.dll"
        )

        foreach(dll ${MSVC_DLLS})
            if(NOT EXISTS "${dll}")
                message(FATAL_ERROR
                    "Required MSVC runtime DLL '${dll}' is missing from the redistributable "
                    "directory. Bundling only part of the CRT ships an app that loads on the "
                    "build machine and fails on the customer's. Repair the Visual Studio "
                    "'Redist' component, or install a toolset whose redistributable is complete.")
            endif()
            add_custom_command(TARGET sak_utility POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${dll}"
                    "$<TARGET_FILE_DIR:sak_utility>"
                COMMENT "Bundling ${dll}..."
            )
        endforeach()
        install(FILES ${MSVC_DLLS} DESTINATION bin)
    endif()
elseif(PLATFORM_MACOS)
    # Use macdeployqt
    find_program(MACDEPLOYQT macdeployqt HINTS "${Qt6_DIR}/../../../bin")
    if(MACDEPLOYQT)
        add_custom_command(TARGET sak_utility POST_BUILD
            COMMAND ${MACDEPLOYQT} $<TARGET_BUNDLE_DIR:sak_utility>
            COMMENT "Running macdeployqt..."
        )
    endif()
endif()

# ============================================================================
# Code Signing (Azure Trusted Signing)
# ============================================================================

option(SAK_CODE_SIGN "Sign the executable after build using Azure Trusted Signing" OFF)

if(SAK_CODE_SIGN AND PLATFORM_WINDOWS)
    find_program(POWERSHELL_EXE pwsh HINTS "$ENV{ProgramFiles}/PowerShell/7")
    if(NOT POWERSHELL_EXE)
        find_program(POWERSHELL_EXE powershell)
    endif()

    set(SIGN_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/sign-exe.ps1")
    # Signing was asked for explicitly. Warning and then producing unsigned
    # binaries is the worst outcome available: the release looks signed to
    # whoever read the option and is not.
    if(NOT POWERSHELL_EXE)
        message(FATAL_ERROR
            "SAK_CODE_SIGN is ON but neither pwsh nor powershell was found, so nothing can be "
            "signed. Install PowerShell 7, or configure with -DSAK_CODE_SIGN=OFF.")
    endif()
    if(NOT EXISTS "${SIGN_SCRIPT}")
        message(FATAL_ERROR
            "SAK_CODE_SIGN is ON but the signing script '${SIGN_SCRIPT}' does not exist. "
            "Restore it from git, or configure with -DSAK_CODE_SIGN=OFF.")
    endif()

    # Every executable that ships gets signed, not just the GUI. The elevated
    # helper is the one that runs as administrator, and the two writer CLIs are
    # the ones that mutate raw disks -- exactly the binaries a customer machine
    # has the most reason to check the signature of.
    set(SAK_SIGN_TARGETS sak_utility sak_apfs_writer_cli sak_hfs_writer_cli)
    if(TARGET sak_elevated_helper)
        list(APPEND SAK_SIGN_TARGETS sak_elevated_helper)
    endif()
    foreach(sign_target ${SAK_SIGN_TARGETS})
        add_custom_command(TARGET ${sign_target} POST_BUILD
            COMMAND ${POWERSHELL_EXE} -ExecutionPolicy Bypass -File "${SIGN_SCRIPT}"
                -ExePath "$<TARGET_FILE:${sign_target}>"
            COMMENT "Signing ${sign_target} via Azure Trusted Signing..."
            VERBATIM
        )
    endforeach()
    message(STATUS "Code Signing:      ENABLED (Azure Trusted Signing) for ${SAK_SIGN_TARGETS}")
else()
    message(STATUS "Code Signing:      OFF (enable with -DSAK_CODE_SIGN=ON)")
endif()

# ============================================================================
# Static Analysis Integration -- Strictest Settings
# ============================================================================
#
# Enable during build:   cmake -B build -DENABLE_CLANG_TIDY=ON -DENABLE_CPPCHECK=ON
# Standalone scripts:    python scripts/run_lizard.py
#                        powershell scripts/run_cppcheck.ps1
# Pre-commit hooks:      pre-commit run --all-files
# ============================================================================

option(ENABLE_CLANG_TIDY "Enable clang-tidy analysis during build" OFF)

if(ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY clang-tidy)
    if(CLANG_TIDY)
        set(CMAKE_CXX_CLANG_TIDY
            ${CLANG_TIDY}
            --warnings-as-errors=*
            --header-filter=include/sak/.*\.h$
        )
        message(STATUS "clang-tidy:         ENABLED (warnings-as-errors)")
    else()
        # The analysis was requested. Continuing without it produces a build that
        # passed a check nobody ran.
        message(FATAL_ERROR
            "ENABLE_CLANG_TIDY is ON but clang-tidy was not found on PATH. Install it (LLVM, or "
            "the 'C++ Clang tools for Windows' Visual Studio component), or configure with "
            "-DENABLE_CLANG_TIDY=OFF.")
    endif()
endif()

option(ENABLE_CPPCHECK "Enable cppcheck analysis during build" OFF)

if(ENABLE_CPPCHECK)
    find_program(CPPCHECK cppcheck HINTS "C:/Program Files/Cppcheck")
    if(CPPCHECK)
        set(CMAKE_CXX_CPPCHECK
            ${CPPCHECK}
            --enable=all
            --check-level=exhaustive
            --inconclusive
            --std=c++23
            --error-exitcode=1
            --force
            --inline-suppr
            --platform=win64
            --quiet
            --suppressions-list=${CMAKE_CURRENT_SOURCE_DIR}/cppcheck_suppressions.txt
        )
        message(STATUS "cppcheck:           ENABLED (exhaustive, all checks, C++23)")
    else()
        # Same rule as clang-tidy above: a requested analysis either runs or the
        # configure fails; it never silently does not happen.
        message(FATAL_ERROR
            "ENABLE_CPPCHECK is ON but cppcheck was not found (searched PATH and "
            "'C:/Program Files/Cppcheck'). Install it from https://cppcheck.sourceforge.io/, "
            "or configure with -DENABLE_CPPCHECK=OFF.")
    endif()
endif()

# ---------------------------------------------------------------------------
# Custom analysis targets (run independently of build)
# ---------------------------------------------------------------------------

# Lizard complexity analysis
find_program(LIZARD_EXE lizard HINTS "${CMAKE_CURRENT_SOURCE_DIR}/.venv/Scripts")
if(LIZARD_EXE)
    add_custom_target(lizard
        COMMAND ${LIZARD_EXE}
            -l cpp
            -C10            # CCN threshold (strict)
            -L70            # Function length (TigerStyle)
            -a5             # Parameter count (strict)
            -w              # Warnings-only output
            -i 0            # Zero tolerance
            -x "*/third_party/*"
            -x "*/3rdparty/*"
            -x "*/build/*"
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running Lizard complexity analysis (CCN<=10, length<=70, params<=5)..."
        VERBATIM
    )
endif()

# Standalone analysis target.
#
# This used to announce a "full static analysis suite" while depending on lizard
# alone, and it vanished entirely when lizard was absent - so `--target analyze`
# either ran one tool under a name promising three, or reported an unknown
# target. Both read as "analysis is fine". Say what it runs, always define it,
# and make a missing tool stop the build instead of reporting nothing to fix.
#
# clang-tidy and cppcheck are compiler-integrated here, not standalone: they run
# on every translation unit via -DENABLE_CLANG_TIDY=ON / -DENABLE_CPPCHECK=ON,
# both of which now fail configure when their tool is missing.
find_program(PYTHON_EXE python3 python)
if(LIZARD_EXE)
    add_custom_target(analyze
        COMMENT "Running standalone analysis suite (lizard complexity)..."
        VERBATIM
    )
    add_dependencies(analyze lizard)
else()
    add_custom_target(analyze
        COMMAND ${CMAKE_COMMAND} -E echo
            "analyze: lizard was not found (searched PATH and .venv/Scripts). Install it with 'pip install lizard' and re-run cmake."
        COMMAND ${CMAKE_COMMAND} -E false
        COMMENT "Standalone analysis suite (unavailable)"
        VERBATIM
    )
endif()

# ============================================================================
# Summary
# ============================================================================

message(STATUS "")
message(STATUS "========================================")
message(STATUS "SAK Utility Configuration Summary")
message(STATUS "========================================")
message(STATUS "Version:           ${PROJECT_VERSION}")
if(CMAKE_CONFIGURATION_TYPES)
    # Multi-config generator: the configuration is chosen at build time by
    # --config, so printing CMAKE_BUILD_TYPE here would name one that is not
    # necessarily the one built.
    message(STATUS "Build Type:        chosen per build (--config): ${CMAKE_CONFIGURATION_TYPES}")
else()
    message(STATUS "Build Type:        ${CMAKE_BUILD_TYPE}")
endif()
message(STATUS "C++ Standard:      C++${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler:          ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Platform:          ${CMAKE_SYSTEM_NAME}")
message(STATUS "Qt Version:        ${Qt6_VERSION}")
message(STATUS "Build Testing:     ${BUILD_TESTING}")
# Report what is actually instrumented, not what was requested. An option that
# reads ON while nothing carries the instrumentation is the exact failure this
# file already had to fix once for /fsanitize=address.
message(STATUS "ASan:              ${SAK_ASAN_STATE}")
message(STATUS "UBSan:             ${SAK_UBSAN_STATE}")
message(STATUS "TSan:              ${SAK_TSAN_STATE}")
if(LIZARD_EXE)
    message(STATUS "Lizard:            AVAILABLE (cmake --build build --target lizard)")
else()
    message(STATUS "Lizard:            NOT FOUND (the 'analyze' target will fail until installed)")
endif()
if(CPPCHECK)
    message(STATUS "cppcheck:          AVAILABLE")
endif()
if(CLANG_TIDY)
    message(STATUS "clang-tidy:        AVAILABLE")
endif()
message(STATUS "========================================")
if(LIZARD_EXE)
    message(STATUS "Analysis targets:  lizard, analyze")
else()
    message(STATUS "Analysis targets:  analyze (fails: lizard not installed)")
endif()
message(STATUS "========================================")
message(STATUS "")

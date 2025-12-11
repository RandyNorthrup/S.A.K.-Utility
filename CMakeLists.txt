cmake_minimum_required(VERSION 3.28 FATAL_ERROR)

# Project definition
project(SAK_Utility
    VERSION 0.5.0
    DESCRIPTION "Swiss Army Knife Utility - PC technician's toolkit for Windows migration and maintenance"
    LANGUAGES CXX
)

# C++23 Standard - Strict compliance required
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for IDE integration
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type default
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# Module path for custom CMake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

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
    )
    
    # Release optimizations
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(/O2 /Ob2 /Oi /Ot /GL)
        add_link_options(/LTCG /OPT:REF /OPT:ICF)
    endif()
    
    # Debug settings
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(/Od /Zi /RTC1)
    endif()
    
    # Security features
    add_compile_options(/guard:cf)  # Control Flow Guard
    add_link_options(/guard:cf /DYNAMICBASE /NXCOMPAT)
    
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
    
    # Release optimizations
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O3 -march=native -flto)
        add_link_options(-flto)
    endif()
    
    # Debug settings
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-O0 -g3)
    endif()
    
    # Security features
    add_compile_options(-fstack-protector-strong -D_FORTIFY_SOURCE=2)
    add_link_options(-Wl,-z,relro -Wl,-z,now)
endif()

# ============================================================================
# Sanitizers - Mandatory for Debug and Testing
# ============================================================================

option(ENABLE_ASAN "Enable Address Sanitizer" ON)
option(ENABLE_UBSAN "Enable Undefined Behavior Sanitizer" ON)
option(ENABLE_TSAN "Enable Thread Sanitizer" OFF)  # Mutually exclusive with ASAN

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(ENABLE_ASAN AND NOT ENABLE_TSAN)
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
        message(STATUS "Address Sanitizer: ENABLED")
    endif()
    
    if(ENABLE_UBSAN)
        add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=undefined)
        message(STATUS "Undefined Behavior Sanitizer: ENABLED")
    endif()
    
    if(ENABLE_TSAN)
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
        message(STATUS "Thread Sanitizer: ENABLED")
    endif()
endif()

# ============================================================================
# Dependencies
# ============================================================================

# Qt6 - Required
find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core
    Widgets
    Concurrent
    Network
)

# Threads - Standard library threading
find_package(Threads REQUIRED)

# Compression libraries for image decompression
find_package(ZLIB REQUIRED)
find_package(BZip2 REQUIRED)
find_package(LibLZMA REQUIRED)

# Platform-specific libraries
if(PLATFORM_WINDOWS)
    # Windows-specific: Registry access, etc.
    set(PLATFORM_LIBS advapi32 shell32 user32)
elseif(PLATFORM_MACOS)
    # macOS-specific: CoreFoundation, IOKit
    find_library(COREFOUNDATION_LIBRARY CoreFoundation REQUIRED)
    find_library(IOKIT_LIBRARY IOKit REQUIRED)
    set(PLATFORM_LIBS ${COREFOUNDATION_LIBRARY} ${IOKIT_LIBRARY})
endif()

# ============================================================================
# Project Configuration
# ============================================================================

# Version header generation
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/sak/version.h"
    @ONLY
)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ============================================================================
# Source Files
# ============================================================================

# Core utility modules (will be C++23 modules)
# Phase 1: Basic infrastructure (COMPLETE)
# Phase 2: File operations (COMPLETE)
# Phase 4: Configuration management (COMPLETE)
# Phase 5: Input validation and security (COMPLETE)
set(CORE_SOURCES
    src/core/logger.cpp
    src/core/file_hash.cpp
    src/core/file_scanner.cpp
    src/core/path_utils.cpp
    src/core/platform_interface.cpp
    src/core/config_manager.cpp
    src/core/input_validator.cpp
    src/core/secure_memory.cpp
    src/core/encryption.cpp
    src/core/keep_awake.cpp
    src/core/elevation_manager.cpp
    src/core/app_scanner.cpp
    src/core/chocolatey_manager.cpp
    src/core/package_matcher.cpp
    src/core/migration_report.cpp
    src/core/app_migration_worker.cpp
    src/core/user_data_manager.cpp
    src/core/user_profile_types.cpp
    src/core/smart_file_filter.cpp
    src/core/windows_user_scanner.cpp
    src/core/permission_manager.cpp
    src/core/user_profile_backup_worker.cpp
    src/core/user_profile_restore_worker.cpp
    src/core/drive_scanner.cpp
    src/core/image_source.cpp
    src/core/flash_coordinator.cpp
    src/core/windows_iso_downloader.cpp
    src/core/drive_unmounter.cpp
    src/core/drive_lock.cpp
    src/core/image_writer.cpp
    src/core/streaming_decompressor.cpp
    src/core/gzip_decompressor.cpp
    src/core/bzip2_decompressor.cpp
    src/core/xz_decompressor.cpp
    src/core/decompressor_factory.cpp
    include/sak/config_manager.h
    include/sak/app_scanner.h
    include/sak/drive_scanner.h
    include/sak/image_source.h
    include/sak/flash_coordinator.h
    include/sak/windows_iso_downloader.h
    include/sak/drive_unmounter.h
    include/sak/drive_lock.h
    include/sak/image_writer.h
    include/sak/streaming_decompressor.h
    include/sak/gzip_decompressor.h
    include/sak/bzip2_decompressor.h
    include/sak/xz_decompressor.h
    include/sak/decompressor_factory.h
    include/sak/chocolatey_manager.h
    include/sak/package_matcher.h
    include/sak/migration_report.h
    include/sak/app_migration_worker.h
    include/sak/user_data_manager.h
    include/sak/user_profile_types.h
    include/sak/smart_file_filter.h
    include/sak/windows_user_scanner.h
    include/sak/permission_manager.h
    include/sak/user_profile_backup_worker.h
    include/sak/user_profile_restore_worker.h
    include/sak/user_profile_restore_wizard.h
)

# Platform abstraction
# Phase 2: Completed basic platform interface
set(PLATFORM_SOURCES
    # Phase 3+ files (not yet implemented):
    # src/platform/permission_manager.cpp
)

# Phase 3+ files (not yet implemented):
if(FALSE)
if(PLATFORM_WINDOWS)
    list(APPEND PLATFORM_SOURCES
        src/platform/platform_windows.cpp
        src/platform/registry_scanner.cpp
        src/platform/keep_awake_windows.cpp
    )
elseif(PLATFORM_MACOS)
    list(APPEND PLATFORM_SOURCES
        src/platform/platform_macos.cpp
        src/platform/plist_scanner.cpp
    )
endif()
endif() # End Phase 3+ conditional

# Threading infrastructure (Phase 3)
set(THREADING_SOURCES
    src/threading/worker_base.cpp
    src/threading/backup_worker.cpp
    src/threading/organizer_worker.cpp
    src/threading/duplicate_finder_worker.cpp
    src/threading/license_scanner_worker.cpp
    src/threading/flash_worker.cpp
    include/sak/worker_base.h
    include/sak/backup_worker.h
    include/sak/organizer_worker.h
    include/sak/duplicate_finder_worker.h
    include/sak/license_scanner_worker.h
    include/sak/flash_worker.h
)

# Feature modules (Phase 4)
set(FEATURE_SOURCES
    # src/features/backup_worker.cpp
    # src/features/backup_config.cpp
    # src/features/copy_operation.cpp
    # src/features/directory_organizer.cpp
    # src/features/file_classifier.cpp
    # src/features/duplicate_finder.cpp
    # src/features/duplicate_resolver.cpp
    # src/features/duplicate_report.cpp
    # src/features/license_scanner.cpp
    # src/features/license_detector.cpp
    # src/features/license_exporter.cpp
)

# GUI sources (Phase 2 Initial Implementation + Phase 4 Common Components)
set(GUI_SOURCES
    src/gui/main_window.cpp
    src/gui/backup_panel.cpp
    src/gui/organizer_panel.cpp
    src/gui/duplicate_finder_panel.cpp
    src/gui/license_scanner_panel.cpp
    src/gui/progress_dialog.cpp
    src/gui/about_dialog.cpp
    src/gui/log_viewer.cpp
    src/gui/settings_dialog.cpp
    src/gui/file_list_widget.cpp
    src/gui/undo_manager.cpp
    src/gui/undo_commands.cpp
    include/sak/main_window.h
    include/sak/backup_panel.h
    include/sak/organizer_panel.h
    include/sak/duplicate_finder_panel.h
    include/sak/license_scanner_panel.h
    include/sak/progress_dialog.h
    include/sak/about_dialog.h
    include/sak/log_viewer.h
    include/gui/settings_dialog.h
    include/gui/file_list_widget.h
    include/gui/undo_manager.h
    include/gui/undo_commands.h
    src/gui/backup_wizard.cpp
    src/gui/restore_wizard.cpp
    include/sak/backup_wizard.h
    include/sak/restore_wizard.h
    src/gui/app_migration_panel.cpp
    include/sak/app_migration_panel.h
    src/gui/per_user_customization_dialog.cpp
    include/sak/per_user_customization_dialog.h
    src/gui/user_profile_backup_wizard.cpp
    include/sak/user_profile_backup_wizard.h
    src/gui/user_profile_restore_wizard.cpp
    include/sak/user_profile_restore_wizard.h
    src/gui/image_flasher_panel.cpp
    include/sak/image_flasher_panel.h
    src/gui/windows_iso_download_dialog.cpp
    include/sak/windows_iso_download_dialog.h
    src/gui/image_flasher_settings_dialog.cpp
    include/sak/image_flasher_settings_dialog.h
)

# Main entry point
set(MAIN_SOURCE src/main.cpp)

# Windows resource file for icon
if(WIN32)
    set(RESOURCE_FILES resources/sak_utility.rc)
endif()

# Combine all sources
set(ALL_SOURCES
    ${CORE_SOURCES}
    ${PLATFORM_SOURCES}
    ${THREADING_SOURCES}
    ${FEATURE_SOURCES}
    ${GUI_SOURCES}
    ${MAIN_SOURCE}
    ${RESOURCE_FILES}
)

# ============================================================================
# Executable Target
# ============================================================================

add_executable(sak_utility ${ALL_SOURCES})

# Enable Qt MOC, UIC, and RCC
set_target_properties(sak_utility PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
)

# Link libraries
target_link_libraries(sak_utility PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Network
    Threads::Threads
    ZLIB::ZLIB
    BZip2::BZip2
    LibLZMA::LibLZMA
    ${PLATFORM_LIBS}
)

# Include directories
target_include_directories(sak_utility PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/include
)

# Copy embedded tools directory to output
add_custom_command(TARGET sak_utility POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
        $<TARGET_FILE_DIR:sak_utility>/tools
    COMMENT "Copying embedded Chocolatey tools to output directory"
)

# Compile definitions
target_compile_definitions(sak_utility PRIVATE
    QT_NO_KEYWORDS          # Avoid Qt keyword conflicts
    QT_DEPRECATED_WARNINGS  # Warn about deprecated Qt features
    QT_DISABLE_DEPRECATED_BEFORE=0x060000  # Disable deprecated Qt APIs
)

# Windows-specific settings
if(PLATFORM_WINDOWS)
    # Set Windows subsystem
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set_target_properties(sak_utility PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
    
    # Copy icon to resources
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../icon.ico")
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/../icon.ico"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.ico"
            COPYONLY
        )
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
    
    # Copy icon to resources
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../icon.icns")
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/../icon.icns"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.icns"
            COPYONLY
        )
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

install(TARGETS sak_utility
    RUNTIME DESTINATION bin
    BUNDLE DESTINATION .
)

# Qt deployment
if(PLATFORM_WINDOWS)
    # Use windeployqt
    find_program(WINDEPLOYQT windeployqt HINTS "${Qt6_DIR}/../../../bin")
    if(WINDEPLOYQT)
        add_custom_command(TARGET sak_utility POST_BUILD
            COMMAND ${WINDEPLOYQT} $<TARGET_FILE:sak_utility>
            COMMENT "Running windeployqt..."
        )
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
# Static Analysis Integration
# ============================================================================

option(ENABLE_CLANG_TIDY "Enable clang-tidy analysis" OFF)

if(ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY clang-tidy)
    if(CLANG_TIDY)
        set(CMAKE_CXX_CLANG_TIDY ${CLANG_TIDY} --warnings-as-errors=*)
        message(STATUS "clang-tidy: ENABLED")
    else()
        message(WARNING "clang-tidy not found")
    endif()
endif()

option(ENABLE_CPPCHECK "Enable cppcheck analysis" OFF)

if(ENABLE_CPPCHECK)
    find_program(CPPCHECK cppcheck)
    if(CPPCHECK)
        set(CMAKE_CXX_CPPCHECK ${CPPCHECK} --enable=all --error-exitcode=1 --suppress=missingInclude)
        message(STATUS "cppcheck: ENABLED")
    else()
        message(WARNING "cppcheck not found")
    endif()
endif()

# ============================================================================
# Documentation
# ============================================================================

option(BUILD_DOCUMENTATION "Build Doxygen documentation" OFF)

if(BUILD_DOCUMENTATION)
    find_package(Doxygen)
    if(DOXYGEN_FOUND)
        set(DOXYGEN_IN ${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile.in)
        set(DOXYGEN_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)
        
        configure_file(${DOXYGEN_IN} ${DOXYGEN_OUT} @ONLY)
        
        add_custom_target(docs ALL
            COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "Generating API documentation with Doxygen"
            VERBATIM
        )
    else()
        message(WARNING "Doxygen not found, documentation will not be built")
    endif()
endif()

# ============================================================================
# Test Executables (Application Migration Scanner)
# ============================================================================

# Test scanner - SCAN-FIRST APPROACH
add_executable(test_app_scanner
    tests/test_app_scanner.cpp
    src/core/app_scanner.cpp
)

target_link_libraries(test_app_scanner PRIVATE
    Qt6::Core
    ${PLATFORM_LIBS}
)

target_include_directories(test_app_scanner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Test: ChocolateyManager
add_executable(test_chocolatey_manager
    tests/test_chocolatey_manager.cpp
    src/core/chocolatey_manager.cpp
    include/sak/chocolatey_manager.h
)

set_target_properties(test_chocolatey_manager PROPERTIES
    AUTOMOC ON
)

target_link_libraries(test_chocolatey_manager PRIVATE
    Qt6::Core
    ${PLATFORM_LIBS}
)

target_include_directories(test_chocolatey_manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Test: Chocolatey Real Installation
add_executable(test_choco_install
    tests/test_choco_install.cpp
    src/core/chocolatey_manager.cpp
    include/sak/chocolatey_manager.h
)

set_target_properties(test_choco_install PROPERTIES
    AUTOMOC ON
)

target_link_libraries(test_choco_install PRIVATE
    Qt6::Core
    ${PLATFORM_LIBS}
)

target_include_directories(test_choco_install PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Test: Phase 1+2 Integration
add_executable(test_integration_phase1_2
    tests/test_integration_phase1_2.cpp
    src/core/app_scanner.cpp
    src/core/chocolatey_manager.cpp
    include/sak/app_scanner.h
    include/sak/chocolatey_manager.h
)

set_target_properties(test_integration_phase1_2 PROPERTIES
    AUTOMOC ON
)

target_link_libraries(test_integration_phase1_2 PRIVATE
    Qt6::Core
    ${PLATFORM_LIBS}
)

target_include_directories(test_integration_phase1_2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Test: PackageMatcher
add_executable(test_package_matcher
    tests/test_package_matcher.cpp
    src/core/app_scanner.cpp
    src/core/chocolatey_manager.cpp
    src/core/package_matcher.cpp
    include/sak/app_scanner.h
    include/sak/chocolatey_manager.h
    include/sak/package_matcher.h
)

set_target_properties(test_package_matcher PROPERTIES
    AUTOMOC ON
)

target_link_libraries(test_package_matcher PRIVATE
    Qt6::Core
    Qt6::Concurrent
    ${PLATFORM_LIBS}
)

target_include_directories(test_package_matcher PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Test executable: test_migration_report
add_executable(test_migration_report
    tests/test_migration_report.cpp
    src/core/app_scanner.cpp
    src/core/chocolatey_manager.cpp
    src/core/package_matcher.cpp
    src/core/migration_report.cpp
    include/sak/app_scanner.h
    include/sak/chocolatey_manager.h
    include/sak/package_matcher.h
    include/sak/migration_report.h
)

set_target_properties(test_migration_report PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
    AUTOMOC ON
)

target_link_libraries(test_migration_report PRIVATE
    Qt6::Core
    Qt6::Concurrent
    ${PLATFORM_LIBS}
)

target_include_directories(test_migration_report PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Test executable: test_app_migration_worker
add_executable(test_app_migration_worker
    tests/test_app_migration_worker.cpp
    src/core/app_scanner.cpp
    src/core/chocolatey_manager.cpp
    src/core/package_matcher.cpp
    src/core/migration_report.cpp
    src/core/app_migration_worker.cpp
    include/sak/app_scanner.h
    include/sak/chocolatey_manager.h
    include/sak/package_matcher.h
    include/sak/migration_report.h
    include/sak/app_migration_worker.h
)

set_target_properties(test_app_migration_worker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
    AUTOMOC ON
)

target_link_libraries(test_app_migration_worker PRIVATE
    Qt6::Core
    Qt6::Concurrent
    ${PLATFORM_LIBS}
)

target_include_directories(test_app_migration_worker PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ============================================================================
# Test 8: UserDataManager (backup/restore)
# ============================================================================
add_executable(test_user_data_manager
    tests/test_user_data_manager.cpp
    src/core/app_scanner.cpp
    src/core/chocolatey_manager.cpp
    src/core/package_matcher.cpp
    src/core/migration_report.cpp
    src/core/app_migration_worker.cpp
    src/core/user_data_manager.cpp
    src/core/encryption.cpp
    include/sak/app_scanner.h
    include/sak/chocolatey_manager.h
    include/sak/package_matcher.h
    include/sak/migration_report.h
    include/sak/app_migration_worker.h
    include/sak/user_data_manager.h
    include/sak/encryption.h
)

set_target_properties(test_user_data_manager PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
)

target_link_libraries(test_user_data_manager PRIVATE
    Qt6::Core
    Qt6::Concurrent
    ${PLATFORM_LIBS}
)

target_include_directories(test_user_data_manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ============================================================================
# Test 9: Backup Wizard (Phase 7)
# ============================================================================

add_executable(test_backup_wizard
    tests/test_backup_wizard.cpp
    src/core/user_data_manager.cpp
    src/core/encryption.cpp
    src/gui/backup_wizard.cpp
    include/sak/backup_wizard.h
    include/sak/user_data_manager.h
    include/sak/encryption.h
)

set_target_properties(test_backup_wizard PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
)

target_link_libraries(test_backup_wizard PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Concurrent
    ${PLATFORM_LIBS}
)

target_include_directories(test_backup_wizard PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ============================================================================
# Test 10: Restore Wizard (Phase 7)
# ============================================================================

add_executable(test_restore_wizard
    tests/test_restore_wizard.cpp
    src/core/user_data_manager.cpp
    src/core/encryption.cpp
    src/gui/restore_wizard.cpp
    include/sak/restore_wizard.h
    include/sak/user_data_manager.h
    include/sak/encryption.h
)

set_target_properties(test_restore_wizard PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
)

target_link_libraries(test_restore_wizard PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Concurrent
    ${PLATFORM_LIBS}
)

target_include_directories(test_restore_wizard PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ============================================================================
# Test 11: App Migration Panel (Phase 8)
# ============================================================================

add_executable(test_app_migration_panel
    tests/test_app_migration_panel.cpp
    src/gui/app_migration_panel.cpp
    src/core/app_scanner.cpp
    src/core/chocolatey_manager.cpp
    src/core/package_matcher.cpp
    src/core/migration_report.cpp
    src/core/app_migration_worker.cpp
    src/core/user_data_manager.cpp
    src/core/encryption.cpp
    src/gui/backup_wizard.cpp
    src/gui/restore_wizard.cpp
    include/sak/app_migration_panel.h
    include/sak/app_scanner.h
    include/sak/chocolatey_manager.h
    include/sak/package_matcher.h
    include/sak/migration_report.h
    include/sak/app_migration_worker.h
    include/sak/user_data_manager.h
    include/sak/backup_wizard.h
    include/sak/restore_wizard.h
)

set_target_properties(test_app_migration_panel PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
)

target_link_libraries(test_app_migration_panel PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Concurrent
    ${PLATFORM_LIBS}
)

target_include_directories(test_app_migration_panel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

message(STATUS "")
message(STATUS "Test executables:")
message(STATUS "  test_app_scanner - Run this first to scan installed applications")
message(STATUS "  test_chocolatey_manager - Test embedded Chocolatey functionality")
message(STATUS "  test_choco_install - Test real package installation with embedded Chocolatey")
message(STATUS "  test_integration_phase1_2 - Integration test: AppScanner + ChocolateyManager")
message(STATUS "  test_package_matcher - Test PackageMatcher with real scanned apps")
message(STATUS "  test_migration_report - Test MigrationReport export/import")
message(STATUS "  test_app_migration_worker - Test AppMigrationWorker background installation")
message(STATUS "  test_user_data_manager - Test UserDataManager backup/restore functionality")
message(STATUS "  test_backup_wizard - Test BackupWizard GUI component")
message(STATUS "  test_restore_wizard - Test RestoreWizard GUI component")
message(STATUS "  test_app_migration_panel - Test AppMigrationPanel GUI (Phase 8)")
message(STATUS "")

# ============================================================================
# Summary
# ============================================================================

message(STATUS "")
message(STATUS "========================================")
message(STATUS "SAK Utility Configuration Summary")
message(STATUS "========================================")
message(STATUS "Version:           ${PROJECT_VERSION}")
message(STATUS "Build Type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard:      C++${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler:          ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Platform:          ${CMAKE_SYSTEM_NAME}")
message(STATUS "Qt Version:        ${Qt6_VERSION}")
message(STATUS "Build Testing:     ${BUILD_TESTING}")
message(STATUS "Build Docs:        ${BUILD_DOCUMENTATION}")
message(STATUS "========================================")
message(STATUS "")

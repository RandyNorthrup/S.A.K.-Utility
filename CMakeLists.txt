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
    VERSION 0.9.0.9
    DESCRIPTION "Swiss Army Knife Utility - PC technician's toolkit for Windows migration and maintenance"
    LANGUAGES CXX
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
# Sanitizers — Strictest Settings for Debug and Testing
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

if(ENABLE_TSAN AND ENABLE_ASAN)
    message(WARNING "TSan and ASan are mutually exclusive. Disabling ASan in favor of TSan.")
    set(ENABLE_ASAN OFF)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(MSVC)
        # MSVC Address Sanitizer — strictest configuration
        if(ENABLE_ASAN)
            add_compile_options(/fsanitize=address)
            # /Zi for full debug info with ASan stack traces
            add_compile_options(/Zi)
            message(STATUS "Address Sanitizer:  ENABLED (MSVC /fsanitize=address)")
        endif()
        # Note: MSVC does not support UBSan or TSan.
        # Use clang-cl or GCC for UBSan/TSan coverage.
        if(ENABLE_UBSAN)
            message(STATUS "UBSan:              SKIPPED (MSVC does not support UBSan)")
        endif()
        if(ENABLE_TSAN)
            message(STATUS "TSan:               SKIPPED (MSVC does not support TSan)")
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        # GCC/Clang Sanitizers — strictest flags
        if(ENABLE_ASAN)
            add_compile_options(
                -fsanitize=address
                -fno-omit-frame-pointer
                -fno-optimize-sibling-calls   # Better stack traces
            )
            add_link_options(-fsanitize=address)
            message(STATUS "Address Sanitizer:  ENABLED")
        endif()

        if(ENABLE_UBSAN)
            add_compile_options(
                -fsanitize=undefined
                -fsanitize=float-divide-by-zero
                -fsanitize=float-cast-overflow
                -fsanitize=integer
                -fno-sanitize-recover=all     # Strictest: abort on any UB
                -fno-omit-frame-pointer
            )
            add_link_options(-fsanitize=undefined)
            message(STATUS "UBSan:              ENABLED (strictest — no recovery)")
        endif()

        if(ENABLE_TSAN)
            add_compile_options(
                -fsanitize=thread
                -fno-omit-frame-pointer
            )
            add_link_options(-fsanitize=thread)
            message(STATUS "Thread Sanitizer:   ENABLED")
        endif()
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
    Xml
)

# Threads - Standard library threading
find_package(Threads REQUIRED)

# Compression libraries for image decompression
find_package(ZLIB REQUIRED)
find_package(BZip2 REQUIRED)
find_package(LibLZMA REQUIRED)

# Platform-specific libraries
if(PLATFORM_WINDOWS)
    # Windows-specific: Registry access, WMI, DXGI, performance counters,
    # networking (IP Helper, Winsock, DNS, WLAN, NetBIOS), COM (Firewall)
    set(PLATFORM_LIBS advapi32 shell32 user32 dxgi pdh
        iphlpapi ws2_32 dnsapi wlanapi netapi32 ole32 oleaut32)
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
    src/core/file_hash.cpp
    src/core/file_scanner.cpp
    src/core/path_utils.cpp
    src/core/config_manager.cpp
    src/core/input_validator.cpp
    src/core/secure_memory.cpp
    src/core/encryption.cpp
    src/core/keep_awake.cpp
    src/core/elevation_manager.cpp
    src/core/bundled_tools_manager.cpp
    src/core/app_scanner.cpp
    src/core/chocolatey_manager.cpp
    src/core/package_matcher.cpp
    src/core/migration_report.cpp
    src/core/app_installation_worker.cpp
    src/core/user_data_manager.cpp
    src/core/user_profile_types.cpp
    src/core/smart_file_filter.cpp
    src/core/windows_user_scanner.cpp
    src/core/permission_manager.cpp
    src/core/user_profile_backup_worker.cpp
    src/core/user_profile_restore_worker.cpp
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
    src/core/hardware_inventory_scanner.cpp
    src/core/smart_disk_analyzer.cpp
    src/core/cpu_benchmark_worker.cpp
    src/core/disk_benchmark_worker.cpp
    src/core/memory_benchmark_worker.cpp
    src/core/stress_test_worker.cpp
    src/core/thermal_monitor.cpp
    src/core/diagnostic_report_generator.cpp
    src/core/diagnostic_controller.cpp
    include/sak/logger.h
    include/sak/file_hash.h
    include/sak/file_scanner.h
    include/sak/path_utils.h
    include/sak/input_validator.h
    include/sak/secure_memory.h
    include/sak/encryption.h
    include/sak/keep_awake.h
    include/sak/elevation_manager.h
    include/sak/bundled_tools_manager.h
    include/sak/error_codes.h
    include/sak/aligned_buffer.h
    include/sak/format_utils.h
    include/sak/widget_helpers.h
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
    include/sak/network_constants.h
    include/sak/style_constants.h
    # Email & PST/OST Inspector core
    src/core/pst_parser.cpp
    src/core/mbox_parser.cpp
    src/core/email_search_worker.cpp
    src/core/email_export_worker.cpp
    src/core/email_profile_manager.cpp
    src/core/email_report_generator.cpp
    src/core/email_inspector_controller.cpp
    include/sak/email_constants.h
    include/sak/email_types.h
    include/sak/pst_parser.h
    include/sak/mbox_parser.h
    include/sak/email_search_worker.h
    include/sak/email_export_worker.h
    include/sak/email_profile_manager.h
    include/sak/email_report_generator.h
    include/sak/email_inspector_controller.h
    # Offline Deployment (Chocolatey internalization)
    src/core/nuget_api_client.cpp
    src/core/install_script_parser.cpp
    src/core/script_rewriter.cpp
    src/core/package_internalization_engine.cpp
    src/core/offline_deployment_worker.cpp
    src/core/package_list_manager.cpp
    include/sak/offline_deployment_constants.h
    include/sak/nuget_api_client.h
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
    include/sak/worker_base.h
    include/sak/organizer_worker.h
    include/sak/duplicate_finder_worker.h
    include/sak/flash_worker.h
)

# GUI sources
set(GUI_SOURCES
    src/gui/main_window.cpp
    src/gui/user_migration_panel.cpp
    src/gui/organizer_panel.cpp
    src/gui/log_viewer.cpp
    include/sak/main_window.h
    include/sak/user_migration_panel.h
    include/sak/organizer_panel.h
    include/sak/log_viewer.h
    include/sak/settings_dialog.h
    src/gui/settings_dialog.cpp
    src/gui/app_installation_panel.cpp
    src/gui/app_installation_panel_actions.cpp
    src/gui/app_installation_panel_table.cpp
    include/sak/app_installation_panel.h
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
)

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

# Combine all sources
set(ALL_SOURCES
    ${CORE_SOURCES}
    ${THREADING_SOURCES}
    ${GUI_SOURCES}
    ${QUICK_ACTIONS_SOURCES}
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
    Qt6::Xml
    Threads::Threads
    ZLIB::ZLIB
    BZip2::BZip2
    LibLZMA::LibLZMA
    ${PLATFORM_LIBS}
)

# Suppress MSVCRT defaultlib conflicts when linking static runtime with Qt libs
# Only needed when static CRT (/MT) is used — suppressing MSVCRT with dynamic CRT causes LNK2001
if(MSVC)
    string(FIND "${CMAKE_MSVC_RUNTIME_LIBRARY}" "DLL" _sak_crt_dll_pos)
    if(_sak_crt_dll_pos EQUAL -1)
        # Static CRT: suppress conflicting dynamic CRT default libs
        target_link_options(sak_utility PRIVATE
            /NODEFAULTLIB:MSVCRT
            /NODEFAULTLIB:MSVCRTD
        )
    endif()
endif()

# Include directories
target_include_directories(sak_utility PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/qrcodegen
)

# Check for bundled smartmontools (required for SMART diagnostics)
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/smartmontools/smartctl.exe")
    message(WARNING "smartmontools not found! SMART disk diagnostics will be unavailable.\n"
                    "Run 'powershell -File scripts/bundle_smartmontools.ps1' to bundle smartctl.exe.")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/uup/aria2c.exe")
    message(WARNING "aria2c not found! ISO downloads will be unavailable.\n"
                    "Run 'powershell -File scripts/bundle_uup_tools.ps1' to bundle UUP tools.")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/uup/uupmc/UUPMediaConverter.exe")
    message(WARNING "UUP Media Creator not found! UUP-to-ISO conversion will be unavailable.\n"
                    "Run 'powershell -File scripts/bundle_uup_tools.ps1' to bundle UUP tools.")
endif()

# Copy embedded tools directory to output
add_custom_command(TARGET sak_utility POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
        $<TARGET_FILE_DIR:sak_utility>/tools
    COMMENT "Copying bundled tools to output directory"
)

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

    # Manifest is embedded via the RC file; suppress linker-generated manifest to avoid conflict
    if(MSVC)
        set_target_properties(sak_utility PROPERTIES
            LINK_FLAGS "/MANIFEST:NO"
        )
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
            COMMAND ${WINDEPLOYQT} --release --no-translations --no-compiler-runtime $<TARGET_FILE:sak_utility>
            COMMENT "Running windeployqt..."
        )
    endif()

    # Bundle MSVC runtime DLLs (required by Qt DLLs)
    # Find and copy the runtime DLLs to the output directory
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)

    if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        foreach(lib ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS})
            add_custom_command(TARGET sak_utility POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${lib}"
                    "$<TARGET_FILE_DIR:sak_utility>"
                COMMENT "Bundling ${lib}..."
            )
        endforeach()
    else()
        # Fallback: manually locate MSVC runtime DLLs
        set(MSVC_REDIST_DIR "C:/Program Files/Microsoft Visual Studio/2022/Preview/VC/Redist/MSVC")
        file(GLOB MSVC_VERSIONS "${MSVC_REDIST_DIR}/*")
        list(SORT MSVC_VERSIONS)
        list(REVERSE MSVC_VERSIONS)
        list(GET MSVC_VERSIONS 0 LATEST_MSVC)

        set(MSVC_DLLS
            "${LATEST_MSVC}/x64/Microsoft.VC143.CRT/msvcp140.dll"
            "${LATEST_MSVC}/x64/Microsoft.VC143.CRT/vcruntime140.dll"
            "${LATEST_MSVC}/x64/Microsoft.VC143.CRT/vcruntime140_1.dll"
        )

        foreach(dll ${MSVC_DLLS})
            if(EXISTS "${dll}")
                add_custom_command(TARGET sak_utility POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${dll}"
                        "$<TARGET_FILE_DIR:sak_utility>"
                    COMMENT "Bundling $(notdir ${dll})..."
                )
            endif()
        endforeach()
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
    if(POWERSHELL_EXE AND EXISTS "${SIGN_SCRIPT}")
        add_custom_command(TARGET sak_utility POST_BUILD
            COMMAND ${POWERSHELL_EXE} -ExecutionPolicy Bypass -File "${SIGN_SCRIPT}"
                -ExePath "$<TARGET_FILE:sak_utility>"
            COMMENT "Signing sak_utility.exe via Azure Trusted Signing..."
            VERBATIM
        )
        message(STATUS "Code Signing:      ENABLED (Azure Trusted Signing)")
    else()
        message(WARNING "Code signing enabled but PowerShell or sign-exe.ps1 not found")
    endif()
else()
    message(STATUS "Code Signing:      OFF (enable with -DSAK_CODE_SIGN=ON)")
endif()

# ============================================================================
# Static Analysis Integration — Strictest Settings
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
        message(WARNING "clang-tidy not found")
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
        message(WARNING "cppcheck not found — install from https://cppcheck.sourceforge.io/")
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

# Full static analysis target (runs all tools)
find_program(PYTHON_EXE python3 python)
if(LIZARD_EXE)
    add_custom_target(analyze
        COMMENT "Running full static analysis suite..."
        VERBATIM
    )
    add_dependencies(analyze lizard)
endif()

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
message(STATUS "ASan:              ${ENABLE_ASAN}")
message(STATUS "UBSan:             ${ENABLE_UBSAN}")
message(STATUS "TSan:              ${ENABLE_TSAN}")
if(LIZARD_EXE)
    message(STATUS "Lizard:            AVAILABLE (cmake --build build --target lizard)")
endif()
if(CPPCHECK)
    message(STATUS "cppcheck:          AVAILABLE")
endif()
if(CLANG_TIDY)
    message(STATUS "clang-tidy:        AVAILABLE")
endif()
message(STATUS "========================================")
message(STATUS "Analysis targets:  lizard, analyze")
message(STATUS "========================================")
message(STATUS "")

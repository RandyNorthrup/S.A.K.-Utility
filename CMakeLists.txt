cmake_minimum_required(VERSION 3.28 FATAL_ERROR)

# Load persistent build configuration (vcpkg, Qt, static runtime) - optional for CI
set(SAK_BUILD_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SAK_BuildConfig.cmake")
if(EXISTS "${SAK_BUILD_CONFIG}")
    include("${SAK_BUILD_CONFIG}")
    message(STATUS "Loaded build configuration from ${SAK_BUILD_CONFIG}")
else()
    message(STATUS "No build configuration found - using environment/command-line settings")
endif()

# Project definition
project(SAK_Utility 
    VERSION 0.5.7
    DESCRIPTION "Swiss Army Knife Utility - PC technician's toolkit for Windows migration and maintenance"
    LANGUAGES CXX
)# C++23 Standard - Strict compliance required
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
        /MT$<$<CONFIG:Debug>:d>  # Static runtime: /MT (Release) or /MTd (Debug)
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
    if(MSVC)
        # MSVC Address Sanitizer
        if(ENABLE_ASAN AND NOT ENABLE_TSAN)
            add_compile_options(/fsanitize=address)
            message(STATUS "Address Sanitizer: ENABLED (MSVC)")
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        # GCC/Clang Sanitizers
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
    src/core/app_migration_worker.cpp
    src/core/user_data_manager.cpp
    src/core/user_profile_types.cpp
    src/core/smart_file_filter.cpp
    src/core/windows_user_scanner.cpp
    src/core/permission_manager.cpp
    src/core/user_profile_backup_worker.cpp
    src/core/user_profile_restore_worker.cpp
    src/core/network_transfer_types.cpp
    src/core/network_transfer_protocol.cpp
    src/core/network_transfer_security.cpp
    src/core/peer_discovery_service.cpp
    src/core/network_connection_manager.cpp
    src/core/network_transfer_worker.cpp
    src/core/network_transfer_controller.cpp
    src/core/network_transfer_report.cpp
    src/core/orchestration_types.cpp
    src/core/destination_registry.cpp
    src/core/deployment_manager.cpp
    src/core/migration_orchestrator.cpp
    src/core/mapping_engine.cpp
    src/core/parallel_transfer_manager.cpp
    src/core/deployment_history.cpp
    src/core/deployment_summary_report.cpp
    src/core/assignment_queue_store.cpp
    src/core/orchestration_protocol.cpp
    src/core/orchestration_server.cpp
    src/core/orchestration_client.cpp
    src/core/orchestration_discovery_service.cpp
    src/core/drive_scanner.cpp
    src/core/image_source.cpp
    src/core/flash_coordinator.cpp
    src/core/uup_dump_api.cpp
    src/core/uup_iso_builder.cpp
    src/core/windows_iso_downloader.cpp
    src/core/linux_distro_catalog.cpp
    src/core/linux_iso_downloader.cpp
    src/core/windows_usb_creator.cpp
    src/core/drive_unmounter.cpp
    src/core/drive_lock.cpp
    src/core/streaming_decompressor.cpp
    src/core/gzip_decompressor.cpp
    src/core/bzip2_decompressor.cpp
    src/core/xz_decompressor.cpp
    src/core/decompressor_factory.cpp
    src/core/quick_action.cpp
    src/core/quick_action_controller.cpp
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
    include/sak/config_manager.h
    include/sak/app_scanner.h
    include/sak/drive_scanner.h
    include/sak/image_source.h
    include/sak/flash_coordinator.h
    include/sak/uup_dump_api.h
    include/sak/uup_iso_builder.h
    include/sak/windows_iso_downloader.h
    include/sak/linux_distro_catalog.h
    include/sak/linux_iso_downloader.h
    include/sak/windows_usb_creator.h
    include/sak/drive_unmounter.h
    include/sak/drive_lock.h
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
    include/sak/network_transfer_types.h
    include/sak/network_transfer_protocol.h
    include/sak/network_transfer_security.h
    include/sak/peer_discovery_service.h
    include/sak/network_connection_manager.h
    include/sak/network_transfer_worker.h
    include/sak/network_transfer_controller.h
    include/sak/network_transfer_report.h
    include/sak/orchestration_types.h
    include/sak/destination_registry.h
    include/sak/deployment_manager.h
    include/sak/migration_orchestrator.h
    include/sak/mapping_engine.h
    include/sak/parallel_transfer_manager.h
    include/sak/deployment_history.h
    include/sak/deployment_summary_report.h
    include/sak/assignment_queue_store.h
    include/sak/orchestration_protocol.h
    include/sak/orchestration_server.h
    include/sak/orchestration_server_interface.h
    include/sak/orchestration_client.h
    include/sak/orchestration_discovery_service.h
)

# Platform abstraction
set(PLATFORM_SOURCES
)

# Threading infrastructure
set(THREADING_SOURCES
    src/threading/worker_base.cpp
    src/threading/backup_worker.cpp
    src/threading/organizer_worker.cpp
    src/threading/duplicate_finder_worker.cpp
    src/threading/flash_worker.cpp
    include/sak/worker_base.h
    include/sak/backup_worker.h
    include/sak/organizer_worker.h
    include/sak/duplicate_finder_worker.h
    include/sak/flash_worker.h
)

# Feature modules
set(FEATURE_SOURCES
)

# GUI sources
set(GUI_SOURCES
    src/gui/main_window.cpp
    src/gui/backup_panel.cpp
    src/gui/organizer_panel.cpp
    src/gui/duplicate_finder_panel.cpp
    src/gui/progress_dialog.cpp
    src/gui/about_dialog.cpp
    src/gui/log_viewer.cpp
    src/gui/settings_dialog.cpp
    src/gui/file_list_widget.cpp
    include/sak/main_window.h
    include/sak/backup_panel.h
    include/sak/organizer_panel.h
    include/sak/duplicate_finder_panel.h
    include/sak/progress_dialog.h
    include/sak/about_dialog.h
    include/sak/log_viewer.h
    include/gui/settings_dialog.h
    include/gui/file_list_widget.h
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
    src/gui/linux_iso_download_dialog.cpp
    include/sak/linux_iso_download_dialog.h
    src/gui/image_flasher_settings_dialog.cpp
    include/sak/image_flasher_settings_dialog.h
    src/gui/quick_actions_panel.cpp
    include/sak/quick_actions_panel.h
    src/gui/network_transfer_panel.cpp
    include/sak/network_transfer_panel.h
    src/gui/windows11_theme.cpp
    include/gui/windows11_theme.h
    src/gui/splash_screen.cpp
    include/gui/splash_screen.h
    include/sak/quick_action.h
    include/sak/quick_action_controller.h
    src/core/quick_action_result_io.cpp
    include/sak/quick_action_result_io.h
    src/core/process_runner.cpp
    include/sak/process_runner.h
)

# Quick Actions
set(QUICK_ACTIONS_SOURCES
    # Factory
    src/actions/action_factory.cpp
    include/sak/actions/action_factory.h
    
    # System Optimization (8 actions)
    src/actions/disk_cleanup_action.cpp
    src/actions/clear_browser_cache_action.cpp
    src/actions/defragment_drives_action.cpp
    src/actions/clear_windows_update_cache_action.cpp
    src/actions/disable_startup_programs_action.cpp
    src/actions/clear_event_logs_action.cpp
    src/actions/optimize_power_settings_action.cpp
    src/actions/disable_visual_effects_action.cpp
    
    # Quick Backups (8 actions)
    src/actions/quickbooks_backup_action.cpp
    src/actions/browser_profile_backup_action.cpp
    src/actions/outlook_backup_action.cpp
    src/actions/sticky_notes_backup_action.cpp
    src/actions/saved_game_data_backup_action.cpp
    src/actions/tax_software_backup_action.cpp
    src/actions/photo_management_backup_action.cpp
    src/actions/development_configs_backup_action.cpp
    
    # Maintenance (8 actions)
    src/actions/check_disk_health_action.cpp
    src/actions/update_all_apps_action.cpp
    src/actions/windows_update_action.cpp
    src/actions/verify_system_files_action.cpp
    src/actions/check_disk_errors_action.cpp
    src/actions/rebuild_icon_cache_action.cpp
    src/actions/reset_network_action.cpp
    src/actions/clear_print_spooler_action.cpp
    
    # Troubleshooting (6 actions)
    src/actions/generate_system_report_action.cpp
    src/actions/check_bloatware_action.cpp
    src/actions/test_network_speed_action.cpp
    src/actions/scan_malware_action.cpp
    src/actions/repair_windows_store_action.cpp
    src/actions/fix_audio_issues_action.cpp
    
    # Emergency Recovery (8 actions)
    src/actions/backup_browser_data_action.cpp
    src/actions/backup_email_data_action.cpp
    src/actions/create_restore_point_action.cpp
    src/actions/export_registry_keys_action.cpp
    src/actions/backup_activation_keys_action.cpp
    src/actions/screenshot_settings_action.cpp
    src/actions/backup_desktop_wallpaper_action.cpp
    src/actions/backup_printer_settings_action.cpp
    src/actions/backup_bitlocker_keys_action.cpp
    
    # Headers for all actions
    include/sak/actions/disk_cleanup_action.h
    include/sak/actions/clear_browser_cache_action.h
    include/sak/actions/defragment_drives_action.h
    include/sak/actions/clear_windows_update_cache_action.h
    include/sak/actions/disable_startup_programs_action.h
    include/sak/actions/clear_event_logs_action.h
    include/sak/actions/optimize_power_settings_action.h
    include/sak/actions/disable_visual_effects_action.h
    include/sak/actions/quickbooks_backup_action.h
    include/sak/actions/browser_profile_backup_action.h
    include/sak/actions/outlook_backup_action.h
    include/sak/actions/sticky_notes_backup_action.h
    include/sak/actions/saved_game_data_backup_action.h
    include/sak/actions/tax_software_backup_action.h
    include/sak/actions/photo_management_backup_action.h
    include/sak/actions/development_configs_backup_action.h
    include/sak/actions/check_disk_health_action.h
    include/sak/actions/update_all_apps_action.h
    include/sak/actions/windows_update_action.h
    include/sak/actions/verify_system_files_action.h
    include/sak/actions/check_disk_errors_action.h
    include/sak/actions/rebuild_icon_cache_action.h
    include/sak/actions/reset_network_action.h
    include/sak/actions/clear_print_spooler_action.h
    include/sak/actions/generate_system_report_action.h
    include/sak/actions/check_bloatware_action.h
    include/sak/actions/test_network_speed_action.h
    include/sak/actions/scan_malware_action.h
    include/sak/actions/repair_windows_store_action.h
    include/sak/actions/fix_audio_issues_action.h
    include/sak/actions/backup_browser_data_action.h
    include/sak/actions/backup_email_data_action.h
    include/sak/actions/create_restore_point_action.h
    include/sak/actions/export_registry_keys_action.h
    include/sak/actions/backup_activation_keys_action.h
    include/sak/actions/screenshot_settings_action.h
    include/sak/actions/backup_desktop_wallpaper_action.h
    include/sak/actions/backup_printer_settings_action.h
    include/sak/actions/backup_bitlocker_keys_action.h
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
    Threads::Threads
    ZLIB::ZLIB
    BZip2::BZip2
    LibLZMA::LibLZMA
    ${PLATFORM_LIBS}
)

# Suppress MSVCRT defaultlib conflicts when linking static runtime with Qt libs
if(MSVC)
    target_link_options(sak_utility PRIVATE
        /NODEFAULTLIB:MSVCRT
        /NODEFAULTLIB:MSVCRTD
    )
endif()

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
    
    # Request Administrator privileges for disk operations (diskpart, Format-Volume)
    # This modifies the default Qt manifest to require elevation
    if(MSVC)
        set_target_properties(sak_utility PROPERTIES
            LINK_FLAGS "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\""
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

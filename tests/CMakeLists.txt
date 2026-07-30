# SAK Utility Test Suite
# Comprehensive testing configuration

cmake_minimum_required(VERSION 3.28)

# Enable testing
enable_testing()

# Find Qt Test module
find_package(Qt6 REQUIRED COMPONENTS Test Network Concurrent Widgets)

# Test output directory
set(TEST_OUTPUT_DIR "${CMAKE_BINARY_DIR}/test_results")
file(MAKE_DIRECTORY "${TEST_OUTPUT_DIR}")

# Put test executables next to sak_utility.exe so they share deployed DLLs.
# MSVC multi-config generators append the config name to RUNTIME_OUTPUT_DIRECTORY,
# which would put tests in build/Release/Release/ ??? away from the DLLs that
# windeployqt placed in build/Release/.  Using the config-specific property
# (RUNTIME_OUTPUT_DIRECTORY_RELEASE) avoids the extra subdirectory.
set(SAK_MAIN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/Release")

set(SAK_APP_PATHS_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/app_paths.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/app_paths.h"
)
set(SAK_PROCESS_RUNNER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/process_runner.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/process_runner.h"
)
set(SAK_NETWORK_TRANSFER_RUNNER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/network_transfer_runner.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/network_transfer_runner.h"
)

# windeployqt only analyses sak_utility.exe, which does not link Qt6::Test.
# Copy Qt6Test.dll so the test executables can actually load at runtime.
if(WIN32)
    get_target_property(_qt_test_dll Qt6::Test IMPORTED_LOCATION_RELEASE)
    if(NOT _qt_test_dll)
        get_target_property(_qt_test_dll Qt6::Test IMPORTED_LOCATION)
    endif()
    if(_qt_test_dll AND EXISTS "${_qt_test_dll}")
        file(COPY "${_qt_test_dll}" DESTINATION "${SAK_MAIN_OUTPUT_DIR}")
        message(STATUS "Deployed ${_qt_test_dll} -> ${SAK_MAIN_OUTPUT_DIR}")
    else()
        message(FATAL_ERROR "Qt6Test.dll not found - tests cannot run without it")
    endif()
    # windeployqt deploys only qwindows.dll; the GUI suites run on the
    # offscreen platform (QT_QPA_PLATFORM=offscreen) so no test window ever
    # appears on the developer desktop. Deploy that plugin too. This must be
    # loud on failure: the test environment forces the offscreen backend, so a
    # missing plugin aborts every GUI suite at startup (0xC0000409) with no
    # useful message.
    get_target_property(_qt_core_dll Qt6::Core IMPORTED_LOCATION_RELEASE)
    if(NOT _qt_core_dll)
        get_target_property(_qt_core_dll Qt6::Core IMPORTED_LOCATION)
    endif()
    set(_qt_offscreen "")
    if(_qt_core_dll)
        get_filename_component(_qt_bin_dir "${_qt_core_dll}" DIRECTORY)
        foreach(_candidate qoffscreen.dll qoffscreend.dll)
            if(EXISTS "${_qt_bin_dir}/../plugins/platforms/${_candidate}")
                set(_qt_offscreen "${_qt_bin_dir}/../plugins/platforms/${_candidate}")
                break()
            endif()
        endforeach()
    endif()
    if(_qt_offscreen)
        file(COPY "${_qt_offscreen}"
             DESTINATION "${SAK_MAIN_OUTPUT_DIR}/platforms")
        message(STATUS "Deployed ${_qt_offscreen} for headless GUI tests")
    else()
        message(FATAL_ERROR
            "qoffscreen.dll not found next to Qt6::Core - the GUI test suites "
            "force QT_QPA_PLATFORM=offscreen and would abort at startup")
    endif()
endif()

# ============================================================================
# Unit Tests - Core Components
# ============================================================================

set(UNIT_TESTS
    test_windows_iso_downloader
    test_linux_iso_downloader
)

foreach(test_name ${UNIT_TESTS})
    # Find source files for test
    file(GLOB test_sources
        "${CMAKE_CURRENT_SOURCE_DIR}/unit/${test_name}.cpp"
    )

    # Find corresponding implementation files
    string(REPLACE "test_" "" component_name "${test_name}")
    file(GLOB impl_sources
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/${component_name}.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/${component_name}.h"
    )
    list(APPEND impl_sources
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/logger.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/logger.h"
    )

    if(test_name STREQUAL "test_windows_iso_downloader")
        list(APPEND impl_sources
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/app_paths.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/app_paths.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/uup_dump_api.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/uup_dump_api.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/uup_iso_builder.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/uup_iso_builder.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/windows_iso_downloader.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/windows_iso_downloader.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/bundled_tools_manager.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/bundled_tools_manager.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/elevation_manager.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/elevation_manager.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/logger.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/logger.h"
        )
    endif()

    if(test_name STREQUAL "test_linux_iso_downloader")
        list(APPEND impl_sources
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/linux_iso_downloader.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/linux_iso_downloader.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/linux_distro_catalog.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/linux_distro_catalog.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/bundled_tools_manager.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/bundled_tools_manager.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/logger.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/logger.h"
        )
    endif()



    if(test_sources)
        add_executable(${test_name} ${test_sources} ${impl_sources})

        set_target_properties(${test_name} PROPERTIES
            AUTOMOC ON
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
        )

        set(extra_unit_libs "")
        if(test_name STREQUAL "test_windows_iso_downloader"
            OR test_name STREQUAL "test_linux_iso_downloader")
            list(APPEND extra_unit_libs Qt6::Network)
        endif()

        # ISO downloader needs Qt Widgets
        if(test_name STREQUAL "test_windows_iso_downloader")
            list(APPEND extra_unit_libs Qt6::Widgets)
        endif()

        # Linux ISO downloader needs Qt Concurrent for checksum verification
        if(test_name STREQUAL "test_linux_iso_downloader")
            list(APPEND extra_unit_libs Qt6::Concurrent)
        endif()

        target_link_libraries(${test_name} PRIVATE
            Qt6::Core
            Qt6::Test
            ${extra_unit_libs}
            ${PLATFORM_LIBS}
        )

        target_include_directories(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../include
        )

        # Add to CTest
        add_test(NAME ${test_name}
                 COMMAND ${test_name}
                 WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")

        message(STATUS "Added unit test: ${test_name}")
    endif()
endforeach()

# ============================================================================
# Unit Tests - Diagnostic & Benchmarking Panel
# ============================================================================

# test_worker_base ??? WorkerBase lifecycle, exception safety, cancellation
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_worker_base.cpp")
    add_executable(test_worker_base
        unit/test_worker_base.cpp
        ../src/threading/worker_base.cpp
        ../include/sak/worker_base.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_worker_base PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_worker_base PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_worker_base PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_worker_base COMMAND test_worker_base
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_worker_base")
endif()

# test_error_codes ??? to_string completeness
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_error_codes.cpp")
    add_executable(test_error_codes
        unit/test_error_codes.cpp
    )
    set_target_properties(test_error_codes PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_error_codes PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_error_codes PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_error_codes COMMAND test_error_codes
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_error_codes")
endif()

# test_diagnostic_types ??? struct defaults and value semantics
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_diagnostic_types.cpp")
    add_executable(test_diagnostic_types
        unit/test_diagnostic_types.cpp
    )
    set_target_properties(test_diagnostic_types PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_diagnostic_types PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_diagnostic_types PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_diagnostic_types COMMAND test_diagnostic_types
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_diagnostic_types")
endif()

# test_partition_manager_core - inventory parsing, safety, scripts, queue
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_partition_manager_core.cpp")
    add_executable(test_partition_manager_core
        unit/test_partition_manager_core.cpp
        ../src/core/file_recovery_engine.cpp
        ../src/core/apfs_crypto.cpp
        ../src/core/apfs_keybag.cpp
        ../src/core/partition_apfs_file_system_reader.cpp
        ../src/core/partition_apfs_writer.cpp
        ../src/core/partition_ext_file_system_reader.cpp
        ../src/core/partition_hfs_file_system_reader.cpp
        ../src/core/partition_hfs_file_system_writer.cpp
        ../src/core/partition_raw_device_io.cpp
        ../src/core/partition_manager_types.cpp
        ../src/core/partition_file_system_detector.cpp
        ../src/core/partition_file_system_registry.cpp
        ../src/core/partition_file_system_tool_manifest.cpp
        ../src/core/partition_file_system_tool_runner.cpp
        ../src/core/storage_inventory_worker.cpp
        ../src/core/elevation_broker.cpp
        ../src/core/partition_safety_validator.cpp
        ../src/core/partition_script_builder.cpp
        ../src/core/partition_operation_planner.cpp
        ../src/core/partition_operation_queue.cpp
        ${SAK_PROCESS_RUNNER_SOURCES}
        ../src/core/logger.cpp
        ../include/sak/partition_manager_types.h
        ../include/sak/partition_file_system_detector.h
        ../include/sak/partition_file_system_registry.h
        ../include/sak/partition_file_system_tool_manifest.h
        ../include/sak/partition_file_system_tool_runner.h
        ../include/sak/partition_apfs_file_system_reader.h
        ../include/sak/partition_apfs_writer.h
        ../include/sak/partition_ext_file_system_reader.h
        ../include/sak/partition_hfs_file_system_reader.h
        ../include/sak/partition_raw_device_io.h
        ../include/sak/elevation_broker.h
        ../include/sak/file_recovery_engine.h
        ../include/sak/storage_inventory_worker.h
        ../include/sak/partition_safety_validator.h
        ../include/sak/partition_script_builder.h
        ../include/sak/partition_operation_planner.h
        ../include/sak/partition_operation_queue.h
    )
    set_target_properties(test_partition_manager_core PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_partition_manager_core PRIVATE
        Qt6::Core Qt6::Test sak_lzfse ${PLATFORM_LIBS}
    )
    target_include_directories(test_partition_manager_core PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_partition_manager_core COMMAND test_partition_manager_core
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_partition_manager_core")
endif()

# partition_file_recovery_certifier - external lab helper for live-volume Data Recovery proof
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/certification/partition_file_recovery_certifier.cpp")
    add_executable(partition_file_recovery_certifier
        certification/partition_file_recovery_certifier.cpp
        ../src/core/file_recovery_engine.cpp
        ../include/sak/file_recovery_engine.h
    )
    set_target_properties(partition_file_recovery_certifier PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(partition_file_recovery_certifier PRIVATE
        Qt6::Core
    )
    target_include_directories(partition_file_recovery_certifier PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    message(STATUS "Added certification helper: partition_file_recovery_certifier")
endif()

# partition_filesystem_probe_certifier - external lab helper for raw filesystem metadata proof
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/certification/partition_filesystem_probe_certifier.cpp")
    add_executable(partition_filesystem_probe_certifier
        certification/partition_filesystem_probe_certifier.cpp
        ../src/core/apfs_crypto.cpp
        ../src/core/apfs_keybag.cpp
        ../src/core/partition_apfs_file_system_reader.cpp
        ../src/core/partition_apfs_writer.cpp
        ../src/core/partition_file_system_detector.cpp
        ../src/core/partition_hfs_file_system_reader.cpp
        ../src/core/partition_hfs_file_system_writer.cpp
        ../src/core/partition_raw_device_io.cpp
        ../include/sak/partition_file_system_detector.h
        ../include/sak/partition_apfs_file_system_reader.h
        ../include/sak/partition_apfs_writer.h
        ../include/sak/partition_hfs_file_system_reader.h
        ../include/sak/partition_raw_device_io.h
    )
    set_target_properties(partition_filesystem_probe_certifier PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(partition_filesystem_probe_certifier PRIVATE
        Qt6::Core sak_lzfse
    )
    target_include_directories(partition_filesystem_probe_certifier PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_partition_filesystem_probe_certifier.ps1")
        find_program(SAK_TEST_POWERSHELL_PATH
            NAMES pwsh.exe pwsh powershell.exe powershell
            HINTS
                "$ENV{ProgramFiles}/PowerShell/7"
                "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0")
        if(SAK_TEST_POWERSHELL_PATH)
            add_test(NAME test_partition_filesystem_probe_certifier
                     COMMAND "${SAK_TEST_POWERSHELL_PATH}"
                             -NoProfile
                             -ExecutionPolicy Bypass
                             -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_partition_filesystem_probe_certifier.ps1"
                             -CertifierPath "$<TARGET_FILE:partition_filesystem_probe_certifier>"
                             -OutputRoot "${TEST_OUTPUT_DIR}/partition_filesystem_probe_certifier"
                     WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")
            message(STATUS "Added certification helper test: test_partition_filesystem_probe_certifier")
        else()
            message(WARNING "PowerShell not found; skipping partition_filesystem_probe_certifier CTest hook")
        endif()
    endif()
    message(STATUS "Added certification helper: partition_filesystem_probe_certifier")
endif()

# file_management_live_certifier - live APFS/HFS File Management bridge proof
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/certification/file_management_live_certifier.cpp")
    add_executable(file_management_live_certifier
        certification/file_management_live_certifier.cpp
        ../src/core/advanced_search_worker.cpp
        ../src/core/file_hash.cpp
        ../src/core/file_management_file_system.cpp
        ../src/core/logger.cpp
        ../src/core/apfs_crypto.cpp
        ../src/core/apfs_keybag.cpp
        ../src/core/partition_apfs_file_system_reader.cpp
        ../src/core/partition_apfs_writer.cpp
        ../src/core/partition_ext_file_system_reader.cpp
        ../src/core/partition_file_system_detector.cpp
        ../src/core/partition_file_system_registry.cpp
        ../src/core/partition_hfs_file_system_reader.cpp
        ../src/core/partition_hfs_file_system_writer.cpp
        ../src/core/partition_raw_device_io.cpp
        ../src/threading/duplicate_finder_worker.cpp
        ../src/threading/worker_base.cpp
        ../include/sak/advanced_search_worker.h
        ../include/sak/duplicate_finder_worker.h
        ../include/sak/file_hash.h
        ../include/sak/file_management_file_system.h
        ../include/sak/partition_apfs_file_system_reader.h
        ../include/sak/partition_apfs_writer.h
        ../include/sak/partition_ext_file_system_reader.h
        ../include/sak/partition_file_system_detector.h
        ../include/sak/partition_file_system_registry.h
        ../include/sak/partition_hfs_file_system_reader.h
        ../include/sak/partition_raw_device_io.h
        ../include/sak/worker_base.h
    )
    set_target_properties(file_management_live_certifier PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(file_management_live_certifier PRIVATE
        Qt6::Core Qt6::Gui Qt6::Concurrent ZLIB::ZLIB sak_lzfse ${PLATFORM_LIBS}
    )
    target_include_directories(file_management_live_certifier PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    message(STATUS "Added certification helper: file_management_live_certifier")
endif()

if(TARGET file_management_live_certifier AND WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_file_management_live_certifier_gate.ps1")
    find_program(SAK_LIVE_CERT_TEST_POWERSHELL_PATH
        NAMES pwsh.exe pwsh powershell.exe powershell
        HINTS
            "$ENV{ProgramFiles}/PowerShell/7"
            "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0")
    if(SAK_LIVE_CERT_TEST_POWERSHELL_PATH)
        add_test(NAME test_file_management_live_certifier_gate
                 COMMAND "${SAK_LIVE_CERT_TEST_POWERSHELL_PATH}"
                         -NoProfile
                         -ExecutionPolicy Bypass
                         -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_file_management_live_certifier_gate.ps1"
                         -CertifierPath "$<TARGET_FILE:file_management_live_certifier>"
                         -OutputRoot "${TEST_OUTPUT_DIR}/file_management_live_certifier_gate"
                 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")
        message(STATUS "Added live-certifier gate self-test: test_file_management_live_certifier_gate")
    else()
        message(WARNING "PowerShell not found; skipping live-certifier gate self-test")
    endif()
endif()

if(TARGET sak_apfs_writer_cli AND WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_sak_apfs_writer_cli.ps1")
    find_program(SAK_APFS_CLI_TEST_POWERSHELL_PATH
        NAMES pwsh.exe pwsh powershell.exe powershell
        HINTS
            "$ENV{ProgramFiles}/PowerShell/7"
            "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0")
    if(SAK_APFS_CLI_TEST_POWERSHELL_PATH)
        add_test(NAME test_sak_apfs_writer_cli
                 COMMAND "${SAK_APFS_CLI_TEST_POWERSHELL_PATH}"
                         -NoProfile
                         -ExecutionPolicy Bypass
                         -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_sak_apfs_writer_cli.ps1"
                         -CliPath "$<TARGET_FILE:sak_apfs_writer_cli>"
                         -OutputRoot "${TEST_OUTPUT_DIR}/sak_apfs_writer_cli"
                 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")
        message(STATUS "Added APFS writer CLI self-test: test_sak_apfs_writer_cli")
    else()
        message(WARNING "PowerShell not found; skipping sak_apfs_writer_cli self-test")
    endif()
endif()

if(TARGET sak_hfs_writer_cli AND WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_sak_hfs_writer_cli.ps1")
    find_program(SAK_HFS_CLI_TEST_POWERSHELL_PATH
        NAMES pwsh.exe pwsh powershell.exe powershell
        HINTS
            "$ENV{ProgramFiles}/PowerShell/7"
            "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0")
    if(SAK_HFS_CLI_TEST_POWERSHELL_PATH)
        add_test(NAME test_sak_hfs_writer_cli
                 COMMAND "${SAK_HFS_CLI_TEST_POWERSHELL_PATH}"
                         -NoProfile
                         -ExecutionPolicy Bypass
                         -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_sak_hfs_writer_cli.ps1"
                         -CliPath "$<TARGET_FILE:sak_hfs_writer_cli>"
                         -CertifierPath "$<TARGET_FILE:partition_filesystem_probe_certifier>"
                         -OutputRoot "${TEST_OUTPUT_DIR}/sak_hfs_writer_cli"
                 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")
        message(STATUS "Added HFS writer CLI self-test: test_sak_hfs_writer_cli")
    else()
        message(WARNING "PowerShell not found; skipping sak_hfs_writer_cli self-test")
    endif()
endif()

# test_partition_manager_panel - GUI chrome guard for AOMEI-style layout
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_partition_manager_panel.cpp")
    add_executable(test_partition_manager_panel
        unit/test_partition_manager_panel.cpp
        ../src/gui/partition_manager_panel.cpp
        ../src/gui/detachable_log_window.cpp
        ../src/gui/follow_scroll_controller.cpp
        ../src/gui/ribbon_tool_button.cpp
        ../src/core/file_recovery_engine.cpp
        ../src/core/apfs_crypto.cpp
        ../src/core/apfs_keybag.cpp
        ../src/core/partition_apfs_file_system_reader.cpp
        ../src/core/partition_apfs_writer.cpp
        ../src/core/partition_ext_file_system_reader.cpp
        ../src/core/partition_hfs_file_system_reader.cpp
        ../src/core/partition_hfs_file_system_writer.cpp
        ../src/core/partition_raw_device_io.cpp
        ../src/core/partition_manager_controller.cpp
        ../src/core/partition_manager_types.cpp
        ../src/core/partition_file_system_detector.cpp
        ../src/core/partition_file_system_registry.cpp
        ../src/core/partition_file_system_tool_manifest.cpp
        ../src/core/partition_file_system_tool_runner.cpp
        ../src/core/storage_inventory_worker.cpp
        ../src/core/partition_safety_validator.cpp
        ../src/core/partition_script_builder.cpp
        ../src/core/partition_operation_planner.cpp
        ../src/core/partition_operation_queue.cpp
        ../src/core/partition_executor.cpp
        ../src/core/partition_report_generator.cpp
        ../src/core/config_manager.cpp
        ../src/core/app_paths.cpp
        ../src/core/elevation_broker.cpp
        ${SAK_PROCESS_RUNNER_SOURCES}
        ../src/core/logger.cpp
        ../resources/icons.qrc
        ../include/sak/partition_manager_panel.h
        ../include/sak/file_recovery_engine.h
        ../include/sak/partition_apfs_file_system_reader.h
        ../include/sak/partition_apfs_writer.h
        ../include/sak/partition_ext_file_system_reader.h
        ../include/sak/partition_hfs_file_system_reader.h
        ../include/sak/partition_raw_device_io.h
        ../include/sak/partition_file_system_detector.h
        ../include/sak/partition_file_system_registry.h
        ../include/sak/partition_file_system_tool_manifest.h
        ../include/sak/partition_file_system_tool_runner.h
        ../include/sak/partition_manager_controller.h
        ../include/sak/partition_executor.h
        ../include/sak/storage_inventory_worker.h
        ../include/sak/elevation_broker.h
        ../include/sak/detachable_log_window.h
        ../include/sak/config_manager.h
    )
    set_target_properties(test_partition_manager_panel PROPERTIES
        AUTOMOC ON
        AUTORCC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_partition_manager_panel PRIVATE
        Qt6::Core Qt6::Concurrent Qt6::Widgets Qt6::Test sak_lzfse ${PLATFORM_LIBS}
    )
    target_compile_definitions(test_partition_manager_panel PRIVATE
        SAK_PARTITION_MANAGER_PANEL_TEST_HOOKS
    )
    target_include_directories(test_partition_manager_panel PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_partition_manager_panel COMMAND test_partition_manager_panel
             -o test_partition_manager_panel.txt,txt
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_partition_manager_panel")
endif()

# test_wifi_manager_panel --- SSID injection safety in buildWindowsScript
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_wifi_manager_panel.cpp")
    add_executable(test_wifi_manager_panel
        unit/test_wifi_manager_panel.cpp
        ../src/gui/wifi_manager_panel.cpp
        ../src/gui/detachable_log_window.cpp
        ../src/gui/follow_scroll_controller.cpp
        ../src/third_party/qrcodegen/qrcodegen.cpp
        ${SAK_PROCESS_RUNNER_SOURCES}
        ../src/core/logger.cpp
        ../include/sak/wifi_manager_panel.h
        ../include/sak/detachable_log_window.h
    )
    set_target_properties(test_wifi_manager_panel PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_wifi_manager_panel PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_wifi_manager_panel PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/third_party/qrcodegen
    )
    add_test(NAME test_wifi_manager_panel COMMAND test_wifi_manager_panel
             -o test_wifi_manager_panel.txt,txt
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_wifi_manager_panel")
endif()

# test_file_explorer_breadcrumb --- path segmentation incl. UNC targets
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_file_explorer_breadcrumb.cpp")
    add_executable(test_file_explorer_breadcrumb
        unit/test_file_explorer_breadcrumb.cpp
        ../src/gui/file_explorer_breadcrumb.cpp
        ../src/gui/file_explorer_icon_registry.cpp
        ../include/sak/file_explorer_breadcrumb.h
    )
    set_target_properties(test_file_explorer_breadcrumb PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_file_explorer_breadcrumb PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_file_explorer_breadcrumb PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_file_explorer_breadcrumb COMMAND test_file_explorer_breadcrumb
             -o test_file_explorer_breadcrumb.txt,txt
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_file_explorer_breadcrumb")
endif()

# test_file_explorer_properties_calc --- size-walk completeness + hash labeling
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_file_explorer_properties_calc.cpp")
    add_executable(test_file_explorer_properties_calc
        unit/test_file_explorer_properties_calc.cpp
        ../src/core/file_explorer_properties_calc.cpp
        ../include/sak/file_explorer_properties_calc.h
    )
    set_target_properties(test_file_explorer_properties_calc PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_file_explorer_properties_calc PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_file_explorer_properties_calc PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_file_explorer_properties_calc COMMAND test_file_explorer_properties_calc
             -o test_file_explorer_properties_calc.txt,txt
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_file_explorer_properties_calc")
endif()

# test_iso_analyzer --- Windows-vs-Linux install-media classification (P12-09)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_iso_analyzer.cpp")
    add_executable(test_iso_analyzer
        unit/test_iso_analyzer.cpp
        ../src/core/iso_analyzer.cpp
        ../src/core/logger.cpp
        ../include/sak/iso_analyzer.h
    )
    set_target_properties(test_iso_analyzer PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_iso_analyzer PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_iso_analyzer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_iso_analyzer COMMAND test_iso_analyzer
             -o test_iso_analyzer.txt,txt
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_iso_analyzer")
endif()

# test_user_profile_restore_selection --- folder selection -> manifest (P12-25)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_user_profile_restore_selection.cpp")
    add_executable(test_user_profile_restore_selection
        unit/test_user_profile_restore_selection.cpp
        ../src/core/user_profile_restore_selection.cpp
        ../src/core/user_profile_types.cpp
        ../include/sak/user_profile_restore_selection.h
    )
    set_target_properties(test_user_profile_restore_selection PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_user_profile_restore_selection PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_user_profile_restore_selection PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_user_profile_restore_selection COMMAND test_user_profile_restore_selection
             -o test_user_profile_restore_selection.txt,txt
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_user_profile_restore_selection")
endif()

# test_diagnostic_controller ??? suite state machine, cancellation, error handling
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_diagnostic_controller.cpp")
    add_executable(test_diagnostic_controller
        unit/test_diagnostic_controller.cpp
        ../src/core/diagnostic_controller.cpp
        ../src/core/hardware_inventory_scanner.cpp
        ../src/core/smart_disk_analyzer.cpp
        ../src/core/cpu_benchmark_worker.cpp
        ../src/core/disk_benchmark_worker.cpp
        ../src/core/memory_benchmark_worker.cpp
        ../src/core/stress_test_worker.cpp
        ../src/core/thermal_monitor.cpp
        ../src/core/diagnostic_report_generator.cpp
        ../src/core/bundled_tools_manager.cpp
        ${SAK_PROCESS_RUNNER_SOURCES}
        ../src/threading/worker_base.cpp
        ../src/core/keep_awake.cpp
        ../src/core/logger.cpp
        ../include/sak/diagnostic_controller.h
        ../include/sak/hardware_inventory_scanner.h
        ../include/sak/smart_disk_analyzer.h
        ../include/sak/cpu_benchmark_worker.h
        ../include/sak/disk_benchmark_worker.h
        ../include/sak/memory_benchmark_worker.h
        ../include/sak/stress_test_worker.h
        ../include/sak/thermal_monitor.h
        ../include/sak/diagnostic_report_generator.h
        ../include/sak/bundled_tools_manager.h
        ../include/sak/worker_base.h
    )
    set_target_properties(test_diagnostic_controller PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_diagnostic_controller PRIVATE
        Qt6::Core Qt6::Test Qt6::Concurrent ZLIB::ZLIB ${PLATFORM_LIBS}
    )
    target_include_directories(test_diagnostic_controller PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_diagnostic_controller COMMAND test_diagnostic_controller
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_diagnostic_controller")
endif()

# test_hardware_inventory_scanner --- WMI parsing + volume-to-disk mapping
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_hardware_inventory_scanner.cpp")
    add_executable(test_hardware_inventory_scanner
        unit/test_hardware_inventory_scanner.cpp
        ../src/core/hardware_inventory_scanner.cpp
        ${SAK_PROCESS_RUNNER_SOURCES}
        ../src/core/logger.cpp
        ../include/sak/hardware_inventory_scanner.h
    )
    set_target_properties(test_hardware_inventory_scanner PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_hardware_inventory_scanner PRIVATE
        Qt6::Core Qt6::Test Qt6::Concurrent ZLIB::ZLIB ${PLATFORM_LIBS}
    )
    target_include_directories(test_hardware_inventory_scanner PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_hardware_inventory_scanner COMMAND test_hardware_inventory_scanner
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_hardware_inventory_scanner")
endif()

# test_thermal_monitor ??? timer behavior and polling
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_thermal_monitor.cpp")
    add_executable(test_thermal_monitor
        unit/test_thermal_monitor.cpp
        ../src/core/thermal_monitor.cpp
        ${SAK_PROCESS_RUNNER_SOURCES}
        ../include/sak/thermal_monitor.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_thermal_monitor PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_thermal_monitor PRIVATE
        Qt6::Core Qt6::Concurrent Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_thermal_monitor PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_thermal_monitor COMMAND test_thermal_monitor
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_thermal_monitor")
endif()

# test_diagnostic_report_generator ??? HTML, JSON, CSV report output
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_diagnostic_report_generator.cpp")
    add_executable(test_diagnostic_report_generator
        unit/test_diagnostic_report_generator.cpp
        ../src/core/diagnostic_report_generator.cpp
        ../include/sak/diagnostic_report_generator.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_diagnostic_report_generator PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_diagnostic_report_generator PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_diagnostic_report_generator PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_diagnostic_report_generator COMMAND test_diagnostic_report_generator
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_diagnostic_report_generator")
endif()

# test_cpu_benchmark_worker ??? construction, result defaults, signal types
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_cpu_benchmark_worker.cpp")
    add_executable(test_cpu_benchmark_worker
        unit/test_cpu_benchmark_worker.cpp
        ../src/core/cpu_benchmark_worker.cpp
        ../include/sak/cpu_benchmark_worker.h
        ../src/threading/worker_base.cpp
        ../include/sak/worker_base.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_cpu_benchmark_worker PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_cpu_benchmark_worker PRIVATE
        Qt6::Core Qt6::Concurrent Qt6::Test ZLIB::ZLIB ${PLATFORM_LIBS}
    )
    target_include_directories(test_cpu_benchmark_worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_cpu_benchmark_worker COMMAND test_cpu_benchmark_worker
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_cpu_benchmark_worker")
endif()

# test_disk_benchmark_worker ??? construction, config, result defaults
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_disk_benchmark_worker.cpp")
    add_executable(test_disk_benchmark_worker
        unit/test_disk_benchmark_worker.cpp
        ../src/core/disk_benchmark_worker.cpp
        ../include/sak/disk_benchmark_worker.h
        ../src/core/keep_awake.cpp
        ../include/sak/keep_awake.h
        ../src/threading/worker_base.cpp
        ../include/sak/worker_base.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_disk_benchmark_worker PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_disk_benchmark_worker PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_disk_benchmark_worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_disk_benchmark_worker COMMAND test_disk_benchmark_worker
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_disk_benchmark_worker")
endif()

# test_memory_benchmark_worker ??? construction, result defaults
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_memory_benchmark_worker.cpp")
    add_executable(test_memory_benchmark_worker
        unit/test_memory_benchmark_worker.cpp
        ../src/core/memory_benchmark_worker.cpp
        ../include/sak/memory_benchmark_worker.h
        ../src/threading/worker_base.cpp
        ../include/sak/worker_base.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_memory_benchmark_worker PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_memory_benchmark_worker PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_memory_benchmark_worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_memory_benchmark_worker COMMAND test_memory_benchmark_worker
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_memory_benchmark_worker")
endif()

# test_stress_test_worker ??? construction, config, result defaults
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_stress_test_worker.cpp")
    add_executable(test_stress_test_worker
        unit/test_stress_test_worker.cpp
        ../src/core/stress_test_worker.cpp
        ../include/sak/stress_test_worker.h
        ../src/core/thermal_monitor.cpp
        ${SAK_PROCESS_RUNNER_SOURCES}
        ../include/sak/thermal_monitor.h
        ../src/core/keep_awake.cpp
        ../include/sak/keep_awake.h
        ../src/threading/worker_base.cpp
        ../include/sak/worker_base.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_stress_test_worker PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_stress_test_worker PRIVATE
        Qt6::Core Qt6::Concurrent Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_stress_test_worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_stress_test_worker COMMAND test_stress_test_worker
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_stress_test_worker")
endif()

# ============================================================================
# Unit Tests - Phase 4: Comprehensive Coverage
# ============================================================================

# --- Tier 1: Security & Data Integrity ---

# test_encryption ??? AES-256-CBC round-trip, wrong password, corrupted ciphertext
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_encryption.cpp")
    add_executable(test_encryption
        unit/test_encryption.cpp
        ../src/core/encryption.cpp
        ../src/core/secure_memory.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_encryption PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_encryption PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_encryption PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_encryption COMMAND test_encryption
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_encryption")
endif()

# test_file_hash ??? MD5/SHA256, verify correct/incorrect, progress, cancellation
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_file_hash.cpp")
    add_executable(test_file_hash
        unit/test_file_hash.cpp
        ../src/core/file_hash.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_file_hash PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_file_hash PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_file_hash PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_file_hash COMMAND test_file_hash
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_file_hash")
endif()

# test_path_utils ??? safe path, traversal, pattern matching, directory size
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_path_utils.cpp")
    add_executable(test_path_utils
        unit/test_path_utils.cpp
        ../src/core/path_utils.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_path_utils PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_path_utils PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_path_utils PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_path_utils COMMAND test_path_utils
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_path_utils")
endif()

# test_secure_memory ??? wipe, secure_buffer, secure_allocator, secure random
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_secure_memory.cpp")
    add_executable(test_secure_memory
        unit/test_secure_memory.cpp
        ../src/core/secure_memory.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_secure_memory PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_secure_memory PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_secure_memory PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_secure_memory COMMAND test_secure_memory
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_secure_memory")
endif()

# test_input_validator ??? path/string/numeric validation, safe arithmetic, buffer validation
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_input_validator.cpp")
    add_executable(test_input_validator
        unit/test_input_validator.cpp
        ../src/core/input_validator.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_input_validator PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_input_validator PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_input_validator PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_input_validator COMMAND test_input_validator
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_input_validator")
endif()

# --- Tier 2: Core Business Logic ---

# test_decompressor_factory ??? format detection, isCompressed, factory create
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_decompressor_factory.cpp")
    add_executable(test_decompressor_factory
        unit/test_decompressor_factory.cpp
        ../src/core/decompressor_factory.cpp
        ../src/core/streaming_decompressor.cpp
        ../src/core/gzip_decompressor.cpp
        ../src/core/bzip2_decompressor.cpp
        ../src/core/xz_decompressor.cpp
        ../src/core/logger.cpp
        ../include/sak/streaming_decompressor.h
        ../include/sak/gzip_decompressor.h
        ../include/sak/bzip2_decompressor.h
        ../include/sak/xz_decompressor.h
    )
    set_target_properties(test_decompressor_factory PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_decompressor_factory PRIVATE
        Qt6::Core Qt6::Test ZLIB::ZLIB BZip2::BZip2 LibLZMA::LibLZMA ${PLATFORM_LIBS}
    )
    target_include_directories(test_decompressor_factory PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_decompressor_factory COMMAND test_decompressor_factory
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_decompressor_factory")
endif()

# test_config_manager ??? singleton, get/set, defaults, signals, typed accessors
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_config_manager.cpp")
    add_executable(test_config_manager
        unit/test_config_manager.cpp
        ../src/core/app_paths.cpp
        ../src/core/config_manager.cpp
        ../src/core/logger.cpp
        ../include/sak/app_paths.h
        ../include/sak/config_manager.h
    )
    set_target_properties(test_config_manager PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_config_manager PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_config_manager PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_config_manager COMMAND test_config_manager
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_config_manager")
endif()

# test_smart_file_filter ??? dangerous files, size limits, pattern/folder exclusion
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_smart_file_filter.cpp")
    add_executable(test_smart_file_filter
        unit/test_smart_file_filter.cpp
        ../src/core/smart_file_filter.cpp
        ../src/core/user_profile_types.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_smart_file_filter PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_smart_file_filter PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_smart_file_filter PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_smart_file_filter COMMAND test_smart_file_filter
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_smart_file_filter")
endif()

# test_file_scanner ??? scan, filters, cancellation, static convenience methods
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_file_scanner.cpp")
    add_executable(test_file_scanner
        unit/test_file_scanner.cpp
        ../src/core/file_scanner.cpp
        ../src/core/path_utils.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_file_scanner PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_file_scanner PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_file_scanner PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_file_scanner COMMAND test_file_scanner
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_file_scanner")
endif()

# test_file_explorer_types - File Explorer state and command registry
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_file_explorer_types.cpp")
    add_executable(test_file_explorer_types
        unit/test_file_explorer_types.cpp
        ../src/core/file_explorer_types.cpp
        ../src/core/file_explorer_layout_metrics.cpp
        ../src/core/file_explorer_storage_history.cpp
        ../src/core/file_explorer_status_center.cpp
        ../src/core/file_explorer_command_registry.cpp
        ../include/sak/file_explorer_types.h
        ../include/sak/file_explorer_layout_metrics.h
        ../include/sak/file_explorer_storage_history.h
        ../include/sak/file_explorer_status_center.h
        ../include/sak/file_explorer_command_registry.h
        ../include/sak/file_management_file_system.h
    )
    set_target_properties(test_file_explorer_types PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_file_explorer_types PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_file_explorer_types PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_file_explorer_types COMMAND test_file_explorer_types
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_file_explorer_types")
endif()

# test_file_explorer_session_store - tab session serialization round trip
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_file_explorer_session_store.cpp")
    add_executable(test_file_explorer_session_store
        unit/test_file_explorer_session_store.cpp
        ../src/core/file_explorer_types.cpp
        ../src/core/file_explorer_layout_metrics.cpp
        ../src/core/file_explorer_session_store.cpp
        ../include/sak/file_explorer_types.h
        ../include/sak/file_explorer_layout_metrics.h
        ../include/sak/file_explorer_session_store.h
        ../include/sak/file_management_file_system.h
    )
    set_target_properties(test_file_explorer_session_store PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_file_explorer_session_store PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_file_explorer_session_store PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_file_explorer_session_store COMMAND test_file_explorer_session_store
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_file_explorer_session_store")
endif()

# test_file_explorer_tag_store - app-level item tag persistence
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_file_explorer_tag_store.cpp")
    add_executable(test_file_explorer_tag_store
        unit/test_file_explorer_tag_store.cpp
        ../src/core/file_explorer_tag_store.cpp
        ../include/sak/file_explorer_tag_store.h
    )
    set_target_properties(test_file_explorer_tag_store PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_file_explorer_tag_store PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_file_explorer_tag_store PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_file_explorer_tag_store COMMAND test_file_explorer_tag_store
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_file_explorer_tag_store")
endif()

# test_file_explorer_item_model - File Explorer model/view rows, roles, and sorting
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_file_explorer_item_model.cpp")
    add_executable(test_file_explorer_item_model
        unit/test_file_explorer_item_model.cpp
        ../src/core/file_explorer_item_model.cpp
        ../src/core/file_explorer_sort_filter_model.cpp
        ../src/core/file_explorer_grouping.cpp
        ../src/core/file_explorer_group_proxy_model.cpp
        ../include/sak/file_explorer_item_model.h
        ../include/sak/file_explorer_sort_filter_model.h
        ../include/sak/file_explorer_grouping.h
        ../include/sak/file_explorer_group_proxy_model.h
        ../include/sak/file_management_file_system.h
    )
    set_target_properties(test_file_explorer_item_model PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_file_explorer_item_model PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_file_explorer_item_model PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_file_explorer_item_model COMMAND test_file_explorer_item_model
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_file_explorer_item_model")
endif()

# test_file_management_explorer_panel - File Explorer shell and command-menu UI
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_file_management_explorer_panel.cpp")
    add_executable(test_file_management_explorer_panel
        unit/test_file_management_explorer_panel.cpp
        ../src/gui/file_explorer_sidebar.cpp
        ../src/gui/file_explorer_breadcrumb.cpp
        ../src/gui/file_explorer_icon_registry.cpp
        ../src/gui/file_explorer_command_bar.cpp
        ../src/gui/file_explorer_omnibar.cpp
        ../src/gui/file_explorer_status_center_widget.cpp
        ../src/gui/file_explorer_details_view.cpp
        ../src/gui/file_explorer_name_delegate.cpp
        ../include/sak/file_explorer_name_delegate.h
        ../src/gui/file_explorer_pane.cpp
        ../src/gui/file_explorer_details_pane.cpp
        ../src/gui/file_explorer_style.cpp
        ../src/gui/file_management_explorer_panel.cpp
        ../src/core/file_explorer_types.cpp
        ../src/core/file_explorer_layout_metrics.cpp
        ../src/core/file_explorer_grouping.cpp
        ../src/core/file_explorer_group_proxy_model.cpp
        ../src/core/file_explorer_storage_history.cpp
        ../src/core/file_explorer_status_center.cpp
        ../src/core/file_explorer_transfer_worker.cpp
        ../src/core/file_explorer_archive_worker.cpp
        ../src/core/file_explorer_item_model.cpp
        ../src/core/file_explorer_sort_filter_model.cpp
        ../src/core/file_explorer_command_registry.cpp
        ../src/core/file_explorer_session_store.cpp
        ../src/core/file_explorer_tag_store.cpp
        ../src/core/file_explorer_archive_service.cpp
        ../src/gui/file_explorer_properties_dialog.cpp
        ../include/sak/file_explorer_properties_dialog.h
        ../src/core/file_management_file_system.cpp
        ../src/core/file_hash.cpp
        ../src/core/recycle_bin.cpp
        ../src/core/drive_unmounter.cpp
        ../include/sak/drive_unmounter.h
        ../src/core/advanced_search_worker.cpp
        ../src/threading/worker_base.cpp
        ../src/core/apfs_crypto.cpp
        ../src/core/apfs_keybag.cpp
        ../src/core/partition_apfs_file_system_reader.cpp
        ../src/core/partition_apfs_writer.cpp
        ../src/core/partition_ext_file_system_reader.cpp
        ../src/core/partition_hfs_file_system_reader.cpp
        ../src/core/partition_hfs_file_system_writer.cpp
        ../src/core/partition_raw_device_io.cpp
        ../src/core/partition_file_system_detector.cpp
        ../src/core/partition_file_system_registry.cpp
        ../src/core/partition_manager_types.cpp
        ../src/core/storage_inventory_worker.cpp
        ../src/core/elevation_broker.cpp
        ${SAK_PROCESS_RUNNER_SOURCES}
        ../src/core/logger.cpp
        ../resources/icons.qrc
        ../include/sak/file_explorer_sidebar.h
        ../include/sak/file_explorer_breadcrumb.h
        ../include/sak/file_explorer_icon_registry.h
        ../include/sak/file_explorer_command_bar.h
        ../include/sak/file_explorer_omnibar.h
        ../include/sak/file_explorer_status_center.h
        ../include/sak/file_explorer_status_center_widget.h
        ../include/sak/file_explorer_transfer_worker.h
        ../include/sak/file_explorer_archive_worker.h
        ../include/sak/file_explorer_details_view.h
        ../include/sak/file_explorer_pane.h
        ../include/sak/file_explorer_details_pane.h
        ../include/sak/file_management_explorer_panel.h
        ../include/sak/file_explorer_types.h
        ../include/sak/file_explorer_layout_metrics.h
        ../include/sak/file_explorer_grouping.h
        ../include/sak/file_explorer_group_proxy_model.h
        ../include/sak/file_explorer_item_model.h
        ../include/sak/file_explorer_sort_filter_model.h
        ../include/sak/file_explorer_command_registry.h
        ../include/sak/file_management_file_system.h
        ../include/sak/advanced_search_worker.h
        ../include/sak/worker_base.h
        ../include/sak/storage_inventory_worker.h
        ../include/sak/elevation_broker.h
    )
    set_target_properties(test_file_management_explorer_panel PROPERTIES
        AUTOMOC ON
        AUTORCC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_file_management_explorer_panel PRIVATE
        Qt6::Core Qt6::CorePrivate Qt6::Concurrent Qt6::Widgets Qt6::Test ZLIB::ZLIB sak_lzfse
        ${PLATFORM_LIBS}
    )
    target_include_directories(test_file_management_explorer_panel PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_file_management_explorer_panel COMMAND test_file_management_explorer_panel
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    # GUI suite runs on the offscreen platform: no windows appear on the
    # developer desktop and stray focus changes cannot disturb the tests.
    set_tests_properties(test_file_management_explorer_panel PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    message(STATUS "Added unit test: test_file_management_explorer_panel")
endif()

# test_file_management_file_system - File Management target bridge
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_file_management_file_system.cpp")
    add_executable(test_file_management_file_system
        unit/test_file_management_file_system.cpp
        ../src/core/file_management_file_system.cpp
        ../src/core/file_hash.cpp
        ../src/core/logger.cpp
        ../src/core/apfs_crypto.cpp
        ../src/core/apfs_keybag.cpp
        ../src/core/partition_apfs_file_system_reader.cpp
        ../src/core/partition_apfs_writer.cpp
        ../src/core/partition_ext_file_system_reader.cpp
        ../src/core/partition_hfs_file_system_reader.cpp
        ../src/core/partition_hfs_file_system_writer.cpp
        ../src/core/partition_raw_device_io.cpp
        ../src/core/partition_file_system_detector.cpp
        ../src/core/partition_file_system_registry.cpp
        ../include/sak/file_management_file_system.h
        ../include/sak/partition_apfs_file_system_reader.h
        ../include/sak/partition_apfs_writer.h
        ../include/sak/partition_ext_file_system_reader.h
        ../include/sak/partition_hfs_file_system_reader.h
        ../include/sak/partition_raw_device_io.h
        ../include/sak/partition_file_system_detector.h
        ../include/sak/partition_file_system_registry.h
    )
    set_target_properties(test_file_management_file_system PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_file_management_file_system PRIVATE
        Qt6::Core Qt6::Test ZLIB::ZLIB sak_lzfse ${PLATFORM_LIBS}
    )
    target_include_directories(test_file_management_file_system PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_file_management_file_system COMMAND test_file_management_file_system
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_file_management_file_system")
endif()

# test_process_runner ??? echo, exit code, stderr, timeout, cancellation, PowerShell
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_process_runner.cpp")
    add_executable(test_process_runner
        unit/test_process_runner.cpp
        ../src/core/process_runner.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_process_runner PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_process_runner PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_process_runner PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_process_runner COMMAND test_process_runner
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_process_runner")
endif()

# test_smart_disk_analyzer ??? SmartReport parsing, health assessment
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_token_usage_tracker.cpp")
    add_executable(test_ai_token_usage_tracker
        unit/test_ai_token_usage_tracker.cpp
        ../src/ai/ai_token_usage_tracker.cpp
        ../include/sak/ai/ai_token_usage_tracker.h
    )
    set_target_properties(test_ai_token_usage_tracker PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_token_usage_tracker PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_token_usage_tracker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_token_usage_tracker COMMAND test_ai_token_usage_tracker
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_token_usage_tracker")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_openai_responses_client.cpp")
    add_executable(test_openai_responses_client
        unit/test_openai_responses_client.cpp
        ../src/ai/openai_responses_client.cpp
        ../src/core/app_paths.cpp
        ../src/ai/ai_token_usage_tracker.cpp
        ../src/ai/ai_credential_store.cpp
        ../src/ai/ai_paths.cpp
        ../src/core/secure_memory.cpp
        ../src/core/logger.cpp
        ../include/sak/ai/openai_responses_client.h
        ../include/sak/app_paths.h
        ../include/sak/ai/ai_token_usage_tracker.h
        ../include/sak/ai/ai_credential_store.h
        ../include/sak/ai/ai_paths.h
        ../include/sak/secure_memory.h
    )
    set_target_properties(test_openai_responses_client PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_openai_responses_client PRIVATE
        Qt6::Core Qt6::Test Qt6::Network ${PLATFORM_LIBS}
    )
    target_include_directories(test_openai_responses_client PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_openai_responses_client COMMAND test_openai_responses_client
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_openai_responses_client")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_provider_registry.cpp")
    add_executable(test_ai_provider_registry
        unit/test_ai_provider_registry.cpp
        ../src/ai/ai_provider_registry.cpp
        ../include/sak/ai/ai_provider_registry.h
        ../resources/ai.qrc
    )
    set_target_properties(test_ai_provider_registry PROPERTIES
        AUTOMOC ON
        AUTORCC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_provider_registry PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_provider_registry PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_provider_registry COMMAND test_ai_provider_registry
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_provider_registry")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_prompt_assembler.cpp")
    add_executable(test_ai_prompt_assembler
        unit/test_ai_prompt_assembler.cpp
        ../src/ai/ai_prompt_assembler.cpp
        ../include/sak/ai/ai_prompt_assembler.h
    )
    set_target_properties(test_ai_prompt_assembler PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_prompt_assembler PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_prompt_assembler PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_prompt_assembler COMMAND test_ai_prompt_assembler
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_prompt_assembler")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_assistant_panel_tool_dispatch.cpp"
   AND SAK_ENABLE_AI_ASSISTANT)
    # The panel's off-GUI-thread tool dispatch is certified against the real
    # AiAssistantPanel, which pulls in the whole AI subsystem plus a few core/gui
    # utilities. Reuse the app's AI source group (absolutized -- the group paths
    # are root-relative) and add the panel's non-AI dependencies explicitly, so
    # this target stays in lock-step with the app without dragging in main_window
    # and the storage drivers.
    set(_panel_dispatch_test_srcs "")
    foreach(_s IN LISTS AI_SOURCES)
        list(APPEND _panel_dispatch_test_srcs "${CMAKE_SOURCE_DIR}/${_s}")
    endforeach()
    add_executable(test_ai_assistant_panel_tool_dispatch
        unit/test_ai_assistant_panel_tool_dispatch.cpp
        ${_panel_dispatch_test_srcs}
        ${CMAKE_SOURCE_DIR}/src/win32mcp/browser_extension_installer.cpp
        ${CMAKE_SOURCE_DIR}/src/core/app_paths.cpp
        ${CMAKE_SOURCE_DIR}/src/core/chocolatey_manager.cpp
        ${CMAKE_SOURCE_DIR}/src/core/package_list_manager.cpp
        ${CMAKE_SOURCE_DIR}/src/core/offline_deployment_worker.cpp
        ${CMAKE_SOURCE_DIR}/src/core/package_internalization_engine.cpp
        ${CMAKE_SOURCE_DIR}/src/core/install_script_parser.cpp
        ${CMAKE_SOURCE_DIR}/src/core/script_rewriter.cpp
        ${CMAKE_SOURCE_DIR}/src/core/network_transfer_runner.cpp
        ${CMAKE_SOURCE_DIR}/src/core/elevation_broker.cpp
        ${CMAKE_SOURCE_DIR}/src/core/elevation_manager.cpp
        ${CMAKE_SOURCE_DIR}/src/core/logger.cpp
        ${CMAKE_SOURCE_DIR}/src/core/process_runner.cpp
        # Headless app-action surface (sak_app_action tool) + the QuickActions it seeds.
        ${CMAKE_SOURCE_DIR}/src/core/app_action_registry.cpp
        ${CMAKE_SOURCE_DIR}/src/core/app_action_bridge.cpp
        ${CMAKE_SOURCE_DIR}/src/core/app_action_service.cpp
        ${CMAKE_SOURCE_DIR}/src/core/quick_action.cpp
        ${CMAKE_SOURCE_DIR}/src/core/quick_action_controller.cpp
        ${CMAKE_SOURCE_DIR}/src/core/quick_action_result_io.cpp
        ${CMAKE_SOURCE_DIR}/src/actions/verify_system_files_action.cpp
        ${CMAKE_SOURCE_DIR}/src/actions/check_disk_errors_action.cpp
        ${CMAKE_SOURCE_DIR}/src/actions/reset_network_action.cpp
        ${CMAKE_SOURCE_DIR}/src/actions/optimize_power_settings_action.cpp
        ${CMAKE_SOURCE_DIR}/src/actions/generate_system_report_action.cpp
        ${CMAKE_SOURCE_DIR}/src/actions/screenshot_settings_action.cpp
        ${CMAKE_SOURCE_DIR}/src/actions/backup_bitlocker_keys_action.cpp
        # W1a read-only app ops + the headless services they drive.
        ${CMAKE_SOURCE_DIR}/src/core/app_readonly_actions.cpp
        # backup.preview_app_data: UserDataManager discovery (scanForAppData/discoverAppDataPaths).
        # Links the whole class, which drags encryption.cpp (backup AES; bcrypt auto-links) even
        # though the op only calls the read-only discovery methods.
        ${CMAKE_SOURCE_DIR}/src/core/user_data_manager.cpp
        ${CMAKE_SOURCE_DIR}/include/sak/user_data_manager.h  # Q_OBJECT -> list header for AUTOMOC
        ${CMAKE_SOURCE_DIR}/src/core/encryption.cpp
        # Network read-only ops: adapter enumeration + DNS lookup engines (synchronous,
        # emit inline). GetAdaptersAddresses / DnsQuery_W -> need iphlpapi + dnsapi.
        ${CMAKE_SOURCE_DIR}/src/core/network_adapter_inspector.cpp
        ${CMAKE_SOURCE_DIR}/src/core/dns_diagnostic_tool.cpp
        # Wave-1 active network probes: ping/traceroute (ConnectivityTester, ICMP) and TCP
        # port scan (PortScanner, QTcpSocket) driven through NetworkProbeWorker (a QThread)
        # via AsyncActionInvocation. iphlpapi/ws2_32 auto-link via #pragma comment in the cpp.
        ${CMAKE_SOURCE_DIR}/src/core/connectivity_tester.cpp
        ${CMAKE_SOURCE_DIR}/src/core/port_scanner.cpp
        ${CMAKE_SOURCE_DIR}/src/threading/network_probe_worker.cpp
        # network.list_connections: active TCP/UDP enumeration + PID->process mapping
        # (GetExtendedTcpTable/UdpTable, iphlpapi -- already linked above).
        ${CMAKE_SOURCE_DIR}/src/core/active_connections_monitor.cpp
        # network.audit_firewall: INetFwPolicy2 COM enumeration + conflict/gap analysis
        # (ole32/oleaut32 auto-link via #pragma comment in the cpp).
        ${CMAKE_SOURCE_DIR}/src/core/firewall_rule_auditor.cpp
        # network.wifi_scan: Windows Native WiFi scan (wlanapi auto-link via #pragma comment).
        ${CMAKE_SOURCE_DIR}/src/core/wifi_analyzer.cpp
        # network.list_wifi_profiles: WifiProfileScanner (netsh wlan, no-shell runProcess) driven
        # directly. Enumeration is not unit-tested (needs a real WLAN service); the serialization
        # (names + security type only, no key material) + honesty channel are covered by the shape.
        ${CMAKE_SOURCE_DIR}/src/core/wifi_profile_scanner.cpp
        ${CMAKE_SOURCE_DIR}/src/core/storage_inventory_worker.cpp
        ${CMAKE_SOURCE_DIR}/src/core/partition_file_system_detector.cpp
        ${CMAKE_SOURCE_DIR}/src/core/partition_raw_device_io.cpp
        # W2e partition.preview_operation: the planner + safety validator + type
        # helpers are driven directly (pure, no disk I/O); no controller/executor.
        ${CMAKE_SOURCE_DIR}/src/core/partition_operation_planner.cpp
        ${CMAKE_SOURCE_DIR}/src/core/partition_safety_validator.cpp
        ${CMAKE_SOURCE_DIR}/src/core/partition_manager_types.cpp
        # W2f partition.apply_operation: the executor + script builder + tool runner +
        # elevation broker are link-only (the apply EXECUTION is not unit-tested -- needs
        # a real disk + admin; the pure guard resolvePartitionApplyTarget IS tested). The
        # script builder pulls only the tool runner, not the APFS/HFS writer libs.
        ${CMAKE_SOURCE_DIR}/src/core/partition_executor.cpp
        ${CMAKE_SOURCE_DIR}/src/threading/partition_apply_worker.cpp
        ${CMAKE_SOURCE_DIR}/src/core/partition_script_builder.cpp
        ${CMAKE_SOURCE_DIR}/src/core/partition_file_system_tool_runner.cpp
        ${CMAKE_SOURCE_DIR}/src/core/partition_file_system_tool_manifest.cpp
        ${CMAKE_SOURCE_DIR}/src/core/elevation_broker.cpp
        ${CMAKE_SOURCE_DIR}/src/core/process_runner.cpp
        ${CMAKE_SOURCE_DIR}/src/core/vulnerability_scanner.cpp
        ${CMAKE_SOURCE_DIR}/src/core/image_source.cpp
        ${CMAKE_SOURCE_DIR}/src/core/decompressor_factory.cpp
        ${CMAKE_SOURCE_DIR}/src/core/streaming_decompressor.cpp
        ${CMAKE_SOURCE_DIR}/src/core/file_hash.cpp
        ${CMAKE_SOURCE_DIR}/src/core/gzip_decompressor.cpp
        ${CMAKE_SOURCE_DIR}/src/core/bzip2_decompressor.cpp
        ${CMAKE_SOURCE_DIR}/src/core/xz_decompressor.cpp
        # W1b async read-only op: the search ENGINE (worker) is driven directly, so
        # the controller/regex-library/config-manager are intentionally not linked.
        ${CMAKE_SOURCE_DIR}/src/core/advanced_search_worker.cpp
        ${CMAKE_SOURCE_DIR}/src/threading/worker_base.cpp
        # W1c sync read-only diagnostics ops: the scanners are driven directly.
        ${CMAKE_SOURCE_DIR}/src/core/hardware_inventory_scanner.cpp
        ${CMAKE_SOURCE_DIR}/src/core/smart_disk_analyzer.cpp
        # diagnostics.read_temperatures: ThermalMonitor::pollOnce (static, QtConcurrent already
        # linked). Uses runProcess (bundled no-shell powershell poll).
        ${CMAKE_SOURCE_DIR}/src/core/thermal_monitor.cpp
        ${CMAKE_SOURCE_DIR}/src/core/bundled_tools_manager.cpp
        # W1c email.read_mbox: the MBOX parser (synchronous read API) is driven directly.
        ${CMAKE_SOURCE_DIR}/src/core/mbox_parser.cpp
        # email.search_mbox: EmailSearchWorker (synchronous searchMbox, emits inline) drives the
        # already-open MboxParser. The PST search path links but is never called by this op.
        ${CMAKE_SOURCE_DIR}/src/core/email_search_worker.cpp
        ${CMAKE_SOURCE_DIR}/include/sak/email_search_worker.h  # Q_OBJECT -> AUTOMOC
        # W2a email.export_mbox: the export worker (synchronous, signal-completes inline)
        # is driven directly. Its writer/parser chain must link even though the MBOX-EML
        # test path only exercises the EmlWriter.
        ${CMAKE_SOURCE_DIR}/src/core/app_mutating_actions.cpp
        # files.delete_to_recycle_bin: sendPathToRecycleBin (SHFileOperationW, shell32 via
        # PLATFORM_LIBS) moves one file/dir to the Recycle Bin.
        ${CMAKE_SOURCE_DIR}/src/core/recycle_bin.cpp
        # network.set_adapter_dhcp: EthernetConfigManager (netsh, no-shell runProcess) driven
        # directly. The DHCP EXECUTION is not unit-tested (needs admin + would change the host's
        # network); the adapter exact-match guard + gating are.
        ${CMAKE_SOURCE_DIR}/src/core/ethernet_config_manager.cpp
        # files.compress_zip / files.extract_zip: FileExplorerArchiveService (static, synchronous,
        # QZipReader/QZipWriter via Qt6::CorePrivate -- already linked). zip-slip/bomb hardening is
        # in the engine; the op-layer path guards + new/empty-dir + no-clobber are covered by tests.
        ${CMAKE_SOURCE_DIR}/src/core/file_explorer_archive_service.cpp
        ${CMAKE_SOURCE_DIR}/include/sak/file_explorer_archive_service.h
        ${CMAKE_SOURCE_DIR}/src/core/wifi_setup_script.cpp
        ${CMAKE_SOURCE_DIR}/src/core/ost_conversion_worker.cpp
        ${CMAKE_SOURCE_DIR}/include/sak/ost_conversion_worker.h
        ${CMAKE_SOURCE_DIR}/src/core/pst_writer.cpp
        ${CMAKE_SOURCE_DIR}/src/core/msg_writer.cpp
        ${CMAKE_SOURCE_DIR}/src/core/dbx_writer.cpp
        ${CMAKE_SOURCE_DIR}/src/core/mbox_writer.cpp
        ${CMAKE_SOURCE_DIR}/src/core/pst_splitter.cpp
        ${CMAKE_SOURCE_DIR}/src/core/email_export_worker.cpp
        ${CMAKE_SOURCE_DIR}/src/core/eml_writer.cpp
        ${CMAKE_SOURCE_DIR}/src/core/html_email_writer.cpp
        ${CMAKE_SOURCE_DIR}/src/core/pdf_email_writer.cpp
        ${CMAKE_SOURCE_DIR}/src/core/pst_parser.cpp
        # email.recover_deleted: DeletedItemScanner drives the (already-open, hardened) PstParser
        # directly (synchronous scan methods); its recursions ride the parser's depth-capped folder
        # tree. Enumeration not unit-tested (no hand-buildable PST fixture, like read_pst); path
        # guards + honesty (reachableReliable) + no-body serialization covered by shape.
        ${CMAKE_SOURCE_DIR}/src/core/deleted_item_scanner.cpp
        # email.list_profiles: EmailProfileManager discovery (registry + profile-dir scan,
        # synchronous, emits inline). Enumeration is env-dependent (real host mail clients);
        # the op-layer shape + honest empty-success + metadata-only serialization are certed.
        ${CMAKE_SOURCE_DIR}/src/core/email_profile_manager.cpp
        ${CMAKE_SOURCE_DIR}/src/core/email_attachment_saver.cpp
        # W2b organizer.organize_directory: the OrganizerWorker (a QThread) is driven
        # through the AsyncActionInvocation bridge, like the read-only search.
        ${CMAKE_SOURCE_DIR}/src/threading/organizer_worker.cpp
        ${CMAKE_SOURCE_DIR}/src/core/input_validator.cpp
        # organizer.find_duplicates: DuplicateFinderWorker (a QThread) driven through the
        # AsyncActionInvocation bridge like the search/organize workers. Its raw/image bridge path
        # is never taken (use_file_system_target=false), so the search stub below covers its two
        # FileManagementFileSystemBridge symbols and keeps the APFS/HFS driver out of the link.
        ${CMAKE_SOURCE_DIR}/src/threading/duplicate_finder_worker.cpp
        # files.scan_directory: file_scanner (synchronous std::filesystem walk, follow_symlinks off
        # + cycle detection) driven directly; path_utils provides its pattern matching.
        ${CMAKE_SOURCE_DIR}/src/core/file_scanner.cpp
        ${CMAKE_SOURCE_DIR}/src/core/path_utils.cpp
        # diagnostics.run_benchmark: the CPU + memory benchmark workers (WorkerBase QThreads, pure
        # in-memory compute) are driven directly through the AsyncActionInvocation bridge. The
        # benchmark EXECUTION is not re-run here (it is timing-sensitive and covered by the
        # dedicated worker unit tests); the op-layer registration + target routing + invalid-target
        # rejection are covered by the dispatch shape tests.
        ${CMAKE_SOURCE_DIR}/src/core/cpu_benchmark_worker.cpp
        ${CMAKE_SOURCE_DIR}/src/core/memory_benchmark_worker.cpp
        ${CMAKE_SOURCE_DIR}/include/sak/cpu_benchmark_worker.h
        ${CMAKE_SOURCE_DIR}/include/sak/memory_benchmark_worker.h
        # W-uninstall software.uninstall_uwp_app: the ProgramEnumerator (UWP-only sync resolve)
        # + UninstallWorker (UwpRemove mode) are driven directly. The uninstall EXECUTION is not
        # unit-tested (would remove a real app); the resolve + guard + gating are. The leftover/
        # snapshot/restore-point stages must link even though UwpRemove never runs them.
        ${CMAKE_SOURCE_DIR}/src/core/program_enumerator.cpp
        ${CMAKE_SOURCE_DIR}/src/core/uninstall_worker.cpp
        ${CMAKE_SOURCE_DIR}/src/core/leftover_scanner.cpp
        # software.clean_leftovers: CleanupWorker (WorkerBase QThread) deletes selected leftover
        # items. The DELETION is never unit-tested here (would remove real system objects); every
        # dispatch cert feeds an item that the leftover_cleanup_guard.h fail-closed screen REFUSES,
        # so the worker is never constructed/run and the suite has no destructive side effects.
        ${CMAKE_SOURCE_DIR}/src/core/cleanup_worker.cpp
        ${CMAKE_SOURCE_DIR}/include/sak/cleanup_worker.h
        ${CMAKE_SOURCE_DIR}/src/core/registry_snapshot_engine.cpp
        ${CMAKE_SOURCE_DIR}/src/core/restore_point_manager.cpp
        # imaging.scan_recoverable: FileRecoveryEngine (static signature-carver, pure Qt) reads an
        # offline image file read-only and returns candidate metadata.
        ${CMAKE_SOURCE_DIR}/src/core/file_recovery_engine.cpp
        # imaging.list_drives: DriveScanner::enumerateDrivesOnce (static, monitor-free physical-drive
        # enumeration via Win32 metadata IOCTLs; setupapi auto-links via #pragma comment in the cpp).
        ${CMAKE_SOURCE_DIR}/src/core/drive_scanner.cpp
        # system.list_users: WindowsUserScanner (NetUserEnum + profile/SID resolution) driven
        # directly, like the read-only diagnostics scanners.
        ${CMAKE_SOURCE_DIR}/src/core/windows_user_scanner.cpp
        # network.list_shares: NetworkShareBrowser (NetShareEnum) driven read-only (no write
        # probe), like the other SYNC network enumerators.
        ${CMAKE_SOURCE_DIR}/src/core/network_share_browser.cpp
        # imaging.analyze_iso: IsoAnalyzer::analyze (static, local file I/O) driven directly.
        ${CMAKE_SOURCE_DIR}/src/core/iso_analyzer.cpp
        # W2d imaging.flash_image: FlashCoordinator + its worker chain must link (the
        # flash EXECUTION is not unit-tested -- needs a real disk -- but the safety
        # guard resolveFlashTarget is tested pure).
        ${CMAKE_SOURCE_DIR}/src/core/flash_coordinator.cpp
        ${CMAKE_SOURCE_DIR}/src/threading/flash_worker.cpp
        ${CMAKE_SOURCE_DIR}/src/core/drive_unmounter.cpp
        ${CMAKE_SOURCE_DIR}/src/core/keep_awake.cpp
        # Stub the raw/image-filesystem bridge (search never uses it) so the whole
        # APFS/HFS+/ext driver stays out of this test link.
        ${CMAKE_CURRENT_SOURCE_DIR}/stubs/file_management_bridge_search_stub.cpp
        ${CMAKE_SOURCE_DIR}/src/gui/detachable_log_window.cpp
        ${CMAKE_SOURCE_DIR}/src/gui/follow_scroll_controller.cpp
        # Q_OBJECT headers listed explicitly for AUTOMOC (this project moc's
        # headers by listing, not by basename discovery).
        ${CMAKE_SOURCE_DIR}/include/sak/ai_assistant_panel.h
        ${CMAKE_SOURCE_DIR}/include/sak/ai_transcript_view.h
        ${CMAKE_SOURCE_DIR}/include/sak/chocolatey_manager.h
        ${CMAKE_SOURCE_DIR}/include/sak/detachable_log_window.h
        ${CMAKE_SOURCE_DIR}/include/sak/elevation_broker.h
        ${CMAKE_SOURCE_DIR}/include/sak/elevation_manager.h
        ${CMAKE_SOURCE_DIR}/include/sak/offline_deployment_worker.h
        ${CMAKE_SOURCE_DIR}/include/sak/package_internalization_engine.h
        ${CMAKE_SOURCE_DIR}/include/sak/app_action_service.h
        ${CMAKE_SOURCE_DIR}/include/sak/quick_action.h
        ${CMAKE_SOURCE_DIR}/include/sak/quick_action_controller.h
        ${CMAKE_SOURCE_DIR}/include/sak/actions/verify_system_files_action.h
        ${CMAKE_SOURCE_DIR}/include/sak/actions/check_disk_errors_action.h
        ${CMAKE_SOURCE_DIR}/include/sak/actions/reset_network_action.h
        ${CMAKE_SOURCE_DIR}/include/sak/actions/optimize_power_settings_action.h
        ${CMAKE_SOURCE_DIR}/include/sak/actions/generate_system_report_action.h
        ${CMAKE_SOURCE_DIR}/include/sak/actions/screenshot_settings_action.h
        ${CMAKE_SOURCE_DIR}/include/sak/actions/backup_bitlocker_keys_action.h
        ${CMAKE_SOURCE_DIR}/include/sak/storage_inventory_worker.h
        ${CMAKE_SOURCE_DIR}/include/sak/image_source.h
        ${CMAKE_SOURCE_DIR}/include/sak/streaming_decompressor.h
        ${CMAKE_SOURCE_DIR}/include/sak/gzip_decompressor.h
        ${CMAKE_SOURCE_DIR}/include/sak/bzip2_decompressor.h
        ${CMAKE_SOURCE_DIR}/include/sak/xz_decompressor.h
        ${CMAKE_SOURCE_DIR}/include/sak/advanced_search_worker.h
        ${CMAKE_SOURCE_DIR}/include/sak/worker_base.h
        ${CMAKE_SOURCE_DIR}/include/sak/hardware_inventory_scanner.h
        ${CMAKE_SOURCE_DIR}/include/sak/smart_disk_analyzer.h
        ${CMAKE_SOURCE_DIR}/include/sak/thermal_monitor.h
        ${CMAKE_SOURCE_DIR}/include/sak/mbox_parser.h
        ${CMAKE_SOURCE_DIR}/include/sak/email_export_worker.h
        ${CMAKE_SOURCE_DIR}/include/sak/pst_parser.h
        ${CMAKE_SOURCE_DIR}/include/sak/deleted_item_scanner.h
        ${CMAKE_SOURCE_DIR}/include/sak/email_profile_manager.h
        ${CMAKE_SOURCE_DIR}/include/sak/organizer_worker.h
        ${CMAKE_SOURCE_DIR}/include/sak/duplicate_finder_worker.h
        ${CMAKE_SOURCE_DIR}/include/sak/flash_coordinator.h
        ${CMAKE_SOURCE_DIR}/include/sak/flash_worker.h
        ${CMAKE_SOURCE_DIR}/include/sak/drive_scanner.h  # Q_OBJECT -> list header for AUTOMOC
        ${CMAKE_SOURCE_DIR}/include/sak/drive_unmounter.h
        ${CMAKE_SOURCE_DIR}/include/sak/partition_executor.h
        ${CMAKE_SOURCE_DIR}/include/sak/partition_apply_worker.h
        ${CMAKE_SOURCE_DIR}/include/sak/elevation_broker.h
        ${CMAKE_SOURCE_DIR}/include/sak/network_adapter_inspector.h
        ${CMAKE_SOURCE_DIR}/include/sak/dns_diagnostic_tool.h
        ${CMAKE_SOURCE_DIR}/include/sak/connectivity_tester.h
        ${CMAKE_SOURCE_DIR}/include/sak/port_scanner.h
        ${CMAKE_SOURCE_DIR}/include/sak/network_probe_worker.h
        ${CMAKE_SOURCE_DIR}/include/sak/active_connections_monitor.h
        ${CMAKE_SOURCE_DIR}/include/sak/firewall_rule_auditor.h
        ${CMAKE_SOURCE_DIR}/include/sak/wifi_analyzer.h
        ${CMAKE_SOURCE_DIR}/include/sak/wifi_profile_scanner.h
        ${CMAKE_SOURCE_DIR}/include/sak/program_enumerator.h
        ${CMAKE_SOURCE_DIR}/include/sak/uninstall_worker.h
        ${CMAKE_SOURCE_DIR}/include/sak/restore_point_manager.h
        ${CMAKE_SOURCE_DIR}/include/sak/windows_user_scanner.h
        ${CMAKE_SOURCE_DIR}/include/sak/network_share_browser.h
        ${CMAKE_SOURCE_DIR}/include/sak/ethernet_config_manager.h
    )
    set_target_properties(test_ai_assistant_panel_tool_dispatch PROPERTIES
        AUTOMOC ON
        AUTOUIC ON
        AUTORCC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_compile_definitions(test_ai_assistant_panel_tool_dispatch PRIVATE
        SAK_ENABLE_AI_ASSISTANT=$<BOOL:${SAK_ENABLE_AI_ASSISTANT}>
    )
    # AUTOMOC forwards target compile definitions to moc (not the builtin _WIN32), so the
    # Windows-only browser-extension button test slot registers with QtTest on Windows.
    if(WIN32)
        target_compile_definitions(test_ai_assistant_panel_tool_dispatch PRIVATE
            SAK_WIN_REGISTRY_TESTS)
    endif()
    target_link_libraries(test_ai_assistant_panel_tool_dispatch PRIVATE
        Qt6::Core Qt6::CorePrivate Qt6::Widgets Qt6::Concurrent Qt6::Network Qt6::Xml
        Qt6::Test Threads::Threads ZLIB::ZLIB BZip2::BZip2 LibLZMA::LibLZMA sak_lzfse
        ${PLATFORM_LIBS}
    )
    if(MSVC)
        string(FIND "${CMAKE_MSVC_RUNTIME_LIBRARY}" "DLL" _panel_test_crt_dll_pos)
        if(_panel_test_crt_dll_pos EQUAL -1)
            target_link_options(test_ai_assistant_panel_tool_dispatch PRIVATE
                /NODEFAULTLIB:MSVCRT /NODEFAULTLIB:MSVCRTD)
        else()
            target_link_options(test_ai_assistant_panel_tool_dispatch PRIVATE
                /NODEFAULTLIB:LIBCMT /NODEFAULTLIB:LIBCMTD)
        endif()
    endif()
    target_include_directories(test_ai_assistant_panel_tool_dispatch PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/third_party/qrcodegen
    )
    add_test(NAME test_ai_assistant_panel_tool_dispatch
             COMMAND test_ai_assistant_panel_tool_dispatch
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_assistant_panel_tool_dispatch")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_async_tool_runner.cpp")
    add_executable(test_ai_async_tool_runner
        unit/test_ai_async_tool_runner.cpp
        ../src/ai/ai_async_tool_runner.cpp
        ../include/sak/ai/ai_async_tool_runner.h
    )
    set_target_properties(test_ai_async_tool_runner PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_async_tool_runner PRIVATE
        Qt6::Core Qt6::Concurrent Qt6::Test
    )
    target_include_directories(test_ai_async_tool_runner PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_async_tool_runner COMMAND test_ai_async_tool_runner
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_async_tool_runner")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_tool_health_ledger.cpp")
    add_executable(test_ai_tool_health_ledger
        unit/test_ai_tool_health_ledger.cpp
        ../src/ai/ai_tool_health_ledger.cpp
        ../include/sak/ai/ai_tool_health_ledger.h
    )
    set_target_properties(test_ai_tool_health_ledger PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_tool_health_ledger PRIVATE
        Qt6::Core Qt6::Concurrent Qt6::Test
    )
    target_include_directories(test_ai_tool_health_ledger PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_tool_health_ledger COMMAND test_ai_tool_health_ledger
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_tool_health_ledger")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_follow_scroll_controller.cpp")
    add_executable(test_follow_scroll_controller
        unit/test_follow_scroll_controller.cpp
        ../src/gui/follow_scroll_controller.cpp
        ../include/sak/follow_scroll_controller.h
    )
    set_target_properties(test_follow_scroll_controller PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_follow_scroll_controller PRIVATE
        Qt6::Core Qt6::Widgets Qt6::Test
    )
    target_include_directories(test_follow_scroll_controller PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_follow_scroll_controller COMMAND test_follow_scroll_controller
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_follow_scroll_controller")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_splash_screen.cpp")
    add_executable(test_splash_screen
        unit/test_splash_screen.cpp
        ../src/gui/splash_screen.cpp
        ../include/sak/splash_screen.h
    )
    set_target_properties(test_splash_screen PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_splash_screen PRIVATE
        Qt6::Core Qt6::Widgets Qt6::Test
    )
    target_include_directories(test_splash_screen PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_splash_screen COMMAND test_splash_screen
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_splash_screen")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_transcript_view.cpp")
    add_executable(test_ai_transcript_view
        unit/test_ai_transcript_view.cpp
        ../src/gui/ai_transcript_view.cpp
        ../src/gui/follow_scroll_controller.cpp
        ../include/sak/ai_transcript_view.h
        ../include/sak/follow_scroll_controller.h
        ../resources/icons.qrc
    )
    set_target_properties(test_ai_transcript_view PROPERTIES
        AUTOMOC ON
        AUTORCC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_transcript_view PRIVATE
        Qt6::Core Qt6::Widgets Qt6::Test
    )
    target_include_directories(test_ai_transcript_view PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_transcript_view COMMAND test_ai_transcript_view
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_transcript_view")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_provider_gateway.cpp")
    add_executable(test_ai_provider_gateway
        unit/test_ai_provider_gateway.cpp
        ../src/ai/ai_provider_gateway.cpp
        ../src/ai/ai_provider_registry.cpp
        ../src/ai/ai_mcp_http_client.cpp
        ../src/ai/ai_mcp_session_pool.cpp
        ../src/ai/ai_mcp_stdio_client.cpp
        ../src/ai/ai_mcp_stdio_session.cpp
        ../include/sak/ai/ai_provider_gateway.h
        ../include/sak/ai/ai_provider_registry.h
        ../include/sak/ai/ai_mcp_http_client.h
        ../include/sak/ai/ai_mcp_session_pool.h
        ../include/sak/ai/ai_mcp_stdio_client.h
        ../include/sak/ai/ai_mcp_stdio_session.h
    )
    set_target_properties(test_ai_provider_gateway PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_provider_gateway PRIVATE
        Qt6::Core Qt6::Network Qt6::Test
    )
    target_include_directories(test_ai_provider_gateway PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_provider_gateway COMMAND test_ai_provider_gateway
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_provider_gateway")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_app_action_planner.cpp")
    add_executable(test_ai_app_action_planner
        unit/test_ai_app_action_planner.cpp
        ../src/ai/ai_app_action_planner.cpp
        ../src/ai/ai_command_guard.cpp
        ../src/ai/ai_command_tool_planner.cpp
        ../src/ai/ai_execution_broker.cpp
        ../src/ai/ai_tool_policy.cpp
        ../src/ai/ai_credential_store.cpp
        ../src/ai/ai_paths.cpp
        ../src/core/app_paths.cpp
        ../src/core/secure_memory.cpp
        ../include/sak/ai/ai_app_action_planner.h
        ../include/sak/ai/ai_command_guard.h
        ../include/sak/ai/ai_command_tool_planner.h
        ../include/sak/ai/ai_execution_broker.h
        ../include/sak/ai/ai_tool_policy.h
        ../include/sak/ai/ai_credential_store.h
        ../include/sak/ai/ai_paths.h
        ../include/sak/app_paths.h
        ../include/sak/secure_memory.h
    )
    set_target_properties(test_ai_app_action_planner PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_app_action_planner PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_ai_app_action_planner PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_app_action_planner COMMAND test_ai_app_action_planner
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_app_action_planner")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_win32_gui_runner.cpp")
    add_executable(test_ai_win32_gui_runner
        unit/test_ai_win32_gui_runner.cpp
        ../src/ai/ai_win32_gui_runner.cpp
        ../include/sak/ai/ai_win32_gui_runner.h
    )
    set_target_properties(test_ai_win32_gui_runner PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_win32_gui_runner PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_ai_win32_gui_runner PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_win32_gui_runner COMMAND test_ai_win32_gui_runner
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_win32_gui_runner")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_command_guard.cpp")
    add_executable(test_ai_command_guard
        unit/test_ai_command_guard.cpp
        ../src/ai/ai_command_guard.cpp
        ../include/sak/ai/ai_command_guard.h
    )
    set_target_properties(test_ai_command_guard PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_command_guard PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_command_guard PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_command_guard COMMAND test_ai_command_guard
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_command_guard")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_command_tool_planner.cpp")
    add_executable(test_ai_command_tool_planner
        unit/test_ai_command_tool_planner.cpp
        ../src/ai/ai_command_tool_planner.cpp
        ../src/ai/ai_command_guard.cpp
        ../src/ai/ai_execution_broker.cpp
        ../src/ai/ai_tool_policy.cpp
        ../src/ai/ai_credential_store.cpp
        ../src/ai/ai_paths.cpp
        ../src/core/app_paths.cpp
        ../src/core/secure_memory.cpp
        ../include/sak/ai/ai_command_tool_planner.h
        ../include/sak/ai/ai_command_guard.h
        ../include/sak/ai/ai_execution_broker.h
        ../include/sak/ai/ai_tool_policy.h
        ../include/sak/ai/ai_credential_store.h
        ../include/sak/ai/ai_paths.h
        ../include/sak/app_paths.h
        ../include/sak/secure_memory.h
    )
    set_target_properties(test_ai_command_tool_planner PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_command_tool_planner PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_ai_command_tool_planner PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_command_tool_planner COMMAND test_ai_command_tool_planner
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_command_tool_planner")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_tool_call_router.cpp")
    add_executable(test_ai_tool_call_router
        unit/test_ai_tool_call_router.cpp
        ../src/ai/ai_tool_call_router.cpp
        ../include/sak/ai/ai_tool_call_router.h
        ../include/sak/ai/openai_response_types.h
    )
    set_target_properties(test_ai_tool_call_router PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_tool_call_router PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_tool_call_router PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_tool_call_router COMMAND test_ai_tool_call_router
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_tool_call_router")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_mcp_http_client.cpp")
    add_executable(test_ai_mcp_http_client
        unit/test_ai_mcp_http_client.cpp
        ../src/ai/ai_mcp_http_client.cpp
        ../include/sak/ai/ai_mcp_http_client.h
    )
    set_target_properties(test_ai_mcp_http_client PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_mcp_http_client PRIVATE
        Qt6::Core Qt6::Network Qt6::Test
    )
    target_include_directories(test_ai_mcp_http_client PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_mcp_http_client COMMAND test_ai_mcp_http_client
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_mcp_http_client")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_mcp_stdio_client.cpp")
    add_executable(test_ai_mcp_stdio_client
        unit/test_ai_mcp_stdio_client.cpp
        ../src/ai/ai_mcp_stdio_client.cpp
        ../include/sak/ai/ai_mcp_stdio_client.h
    )
    set_target_properties(test_ai_mcp_stdio_client PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_mcp_stdio_client PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_mcp_stdio_client PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_mcp_stdio_client COMMAND test_ai_mcp_stdio_client
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_mcp_stdio_client")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_mcp_stdio_session.cpp")
    add_executable(test_ai_mcp_stdio_session
        unit/test_ai_mcp_stdio_session.cpp
        ../src/ai/ai_mcp_stdio_session.cpp
        ../include/sak/ai/ai_mcp_stdio_session.h
    )
    set_target_properties(test_ai_mcp_stdio_session PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_mcp_stdio_session PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_mcp_stdio_session PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_mcp_stdio_session COMMAND test_ai_mcp_stdio_session
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_mcp_stdio_session")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_mcp_session_pool.cpp")
    add_executable(test_ai_mcp_session_pool
        unit/test_ai_mcp_session_pool.cpp
        ../src/ai/ai_mcp_session_pool.cpp
        ../src/ai/ai_mcp_stdio_session.cpp
        ../include/sak/ai/ai_mcp_session_pool.h
        ../include/sak/ai/ai_mcp_stdio_session.h
    )
    set_target_properties(test_ai_mcp_session_pool PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_mcp_session_pool PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_mcp_session_pool PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_mcp_session_pool COMMAND test_ai_mcp_session_pool
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_mcp_session_pool")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_app_action_registry.cpp")
    add_executable(test_app_action_registry
        unit/test_app_action_registry.cpp
        ../src/core/app_action_registry.cpp
        ../include/sak/app_action_registry.h
    )
    set_target_properties(test_app_action_registry PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_app_action_registry PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_app_action_registry PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_app_action_registry COMMAND test_app_action_registry
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_app_action_registry")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_app_action_service.cpp")
    add_executable(test_app_action_service
        unit/test_app_action_service.cpp
        ../src/core/app_action_bridge.cpp
        ../src/core/app_action_registry.cpp
        ../src/core/quick_action.cpp
        ../src/core/quick_action_controller.cpp
        ../src/core/app_paths.cpp
        ../src/core/elevation_broker.cpp
        ../src/core/elevation_manager.cpp
        ../src/core/logger.cpp
        ../src/core/quick_action_result_io.cpp
        ../include/sak/app_action_service.h
        ../include/sak/app_action_registry.h
        ../include/sak/quick_action.h
        ../include/sak/quick_action_controller.h
        ../include/sak/elevation_broker.h
        ../include/sak/elevation_manager.h
    )
    set_target_properties(test_app_action_service PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_app_action_service PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test advapi32 shell32
    )
    target_include_directories(test_app_action_service PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_app_action_service COMMAND test_app_action_service
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_app_action_service")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_conversation_store.cpp")
    add_executable(test_ai_conversation_store
        unit/test_ai_conversation_store.cpp
        ../src/core/app_paths.cpp
        ../src/ai/ai_conversation_store.cpp
        ../src/ai/ai_credential_store.cpp
        ../src/ai/ai_paths.cpp
        ../src/ai/ai_token_usage_tracker.cpp
        ../include/sak/ai/ai_conversation_store.h
        ../include/sak/app_paths.h
        ../include/sak/ai/ai_paths.h
        ../include/sak/ai/ai_token_usage_tracker.h
    )
    set_target_properties(test_ai_conversation_store PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_conversation_store PRIVATE
        Qt6::Core Qt6::Test Crypt32
    )
    target_include_directories(test_ai_conversation_store PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_conversation_store COMMAND test_ai_conversation_store
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_conversation_store")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_chat_title.cpp")
    add_executable(test_ai_chat_title
        unit/test_ai_chat_title.cpp
        ../src/ai/ai_chat_title.cpp
        ../include/sak/ai/ai_chat_title.h
    )
    set_target_properties(test_ai_chat_title PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_chat_title PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_chat_title PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_chat_title COMMAND test_ai_chat_title
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_chat_title")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_execution_broker.cpp")
    add_executable(test_ai_execution_broker
        unit/test_ai_execution_broker.cpp
        ../src/core/app_paths.cpp
        ../src/ai/ai_execution_broker.cpp
        ../src/ai/ai_credential_store.cpp
        ../src/ai/ai_paths.cpp
        ../src/core/secure_memory.cpp
        ../include/sak/ai/ai_execution_broker.h
        ../include/sak/app_paths.h
        ../include/sak/ai/ai_credential_store.h
        ../include/sak/ai/ai_paths.h
        ../include/sak/secure_memory.h
    )
    set_target_properties(test_ai_execution_broker PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_execution_broker PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_ai_execution_broker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_execution_broker COMMAND test_ai_execution_broker
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_execution_broker")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_cancellation_token.cpp")
    add_executable(test_ai_cancellation_token
        unit/test_ai_cancellation_token.cpp
        ../src/ai/ai_cancellation_token.cpp
        ../include/sak/ai/ai_cancellation_token.h
    )
    set_target_properties(test_ai_cancellation_token PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_cancellation_token PRIVATE
        Qt6::Core Qt6::Concurrent Qt6::Test
    )
    target_include_directories(test_ai_cancellation_token PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_cancellation_token COMMAND test_ai_cancellation_token
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_cancellation_token")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_run_state.cpp")
    add_executable(test_ai_run_state
        unit/test_ai_run_state.cpp
        ../src/ai/ai_human_gate.cpp
        ../src/ai/ai_run_state.cpp
        ../include/sak/ai/ai_human_gate.h
        ../include/sak/ai/ai_run_state.h
    )
    set_target_properties(test_ai_run_state PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_run_state PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_run_state PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_run_state COMMAND test_ai_run_state
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_run_state")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_human_gate_store.cpp")
    add_executable(test_ai_human_gate_store
        unit/test_ai_human_gate_store.cpp
        ../src/ai/ai_human_gate.cpp
        ../src/ai/ai_human_gate_store.cpp
        ../include/sak/ai/ai_human_gate.h
        ../include/sak/ai/ai_human_gate_store.h
    )
    set_target_properties(test_ai_human_gate_store PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_human_gate_store PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_human_gate_store PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_human_gate_store COMMAND test_ai_human_gate_store
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_human_gate_store")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_recovery_policy.cpp")
    add_executable(test_ai_recovery_policy
        unit/test_ai_recovery_policy.cpp
        ../src/ai/ai_recovery_policy.cpp
        ../include/sak/ai/ai_recovery_policy.h
    )
    set_target_properties(test_ai_recovery_policy PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_recovery_policy PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_recovery_policy PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_recovery_policy COMMAND test_ai_recovery_policy
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_recovery_policy")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_package_selection.cpp")
    add_executable(test_ai_package_selection
        unit/test_ai_package_selection.cpp
        ../src/ai/ai_package_selection.cpp
        ../include/sak/ai/ai_package_selection.h
    )
    set_target_properties(test_ai_package_selection PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_package_selection PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_package_selection PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_package_selection COMMAND test_ai_package_selection
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_package_selection")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_workflow_clarifier.cpp")
    add_executable(test_ai_workflow_clarifier
        unit/test_ai_workflow_clarifier.cpp
        ../src/ai/ai_workflow_clarifier.cpp
        ../src/ai/ai_workflow_template.cpp
    )
    set_target_properties(test_ai_workflow_clarifier PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_workflow_clarifier PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_workflow_clarifier PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_workflow_clarifier COMMAND test_ai_workflow_clarifier
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_workflow_clarifier")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_trace_store.cpp")
    add_executable(test_ai_trace_store
        unit/test_ai_trace_store.cpp
        ../src/ai/ai_trace_store.cpp
        ../src/ai/ai_credential_store.cpp
        ../src/ai/ai_paths.cpp
        ../src/ai/ai_token_usage_tracker.cpp
        ../src/core/app_paths.cpp
        ../src/core/logger.cpp
        ../src/core/secure_memory.cpp
        ../include/sak/ai/ai_trace_store.h
        ../include/sak/ai/ai_credential_store.h
        ../include/sak/ai/ai_paths.h
        ../include/sak/ai/ai_token_usage_tracker.h
        ../include/sak/app_paths.h
        ../include/sak/secure_memory.h
    )
    set_target_properties(test_ai_trace_store PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_trace_store PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_ai_trace_store PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_trace_store COMMAND test_ai_trace_store
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_trace_store")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_tool_policy.cpp")
    add_executable(test_ai_tool_policy
        unit/test_ai_tool_policy.cpp
        ../src/ai/ai_tool_policy.cpp
        ../include/sak/ai/ai_tool_policy.h
    )
    set_target_properties(test_ai_tool_policy PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_tool_policy PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_tool_policy PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_tool_policy COMMAND test_ai_tool_policy
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_tool_policy")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_skill_store.cpp")
    add_executable(test_ai_skill_store
        unit/test_ai_skill_store.cpp
        ../src/ai/ai_skill.cpp
        ../src/ai/ai_skill_store.cpp
        ../src/ai/ai_paths.cpp
        ../src/core/app_paths.cpp
        ../include/sak/ai/ai_skill.h
        ../include/sak/ai/ai_skill_store.h
        ${CMAKE_SOURCE_DIR}/resources/ai.qrc
    )
    set_target_properties(test_ai_skill_store PROPERTIES
        AUTOMOC ON
        AUTORCC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_skill_store PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_skill_store PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_skill_store COMMAND test_ai_skill_store
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_skill_store")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_lease_manager.cpp")
    add_executable(test_ai_lease_manager
        unit/test_ai_lease_manager.cpp
        ../src/ai/ai_lease_manager.cpp
        ../include/sak/ai/ai_lease_manager.h
    )
    set_target_properties(test_ai_lease_manager PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_lease_manager PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_lease_manager PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_lease_manager COMMAND test_ai_lease_manager
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_lease_manager")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_subagent_tool_executor.cpp")
    add_executable(test_ai_subagent_tool_executor
        unit/test_ai_subagent_tool_executor.cpp
        ../src/ai/ai_subagent_tool_executor.cpp
        ../src/ai/ai_cancellation_token.cpp
        ../include/sak/ai/ai_subagent_tool_executor.h
        ../include/sak/ai/ai_subagent_runner.h
    )
    set_target_properties(test_ai_subagent_tool_executor PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_subagent_tool_executor PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_subagent_tool_executor PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_subagent_tool_executor COMMAND test_ai_subagent_tool_executor
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_subagent_tool_executor")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_tool_loop_detector.cpp")
    add_executable(test_ai_tool_loop_detector
        unit/test_ai_tool_loop_detector.cpp
        ../src/ai/ai_tool_loop_detector.cpp
        ../include/sak/ai/ai_tool_loop_detector.h
    )
    set_target_properties(test_ai_tool_loop_detector PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_tool_loop_detector PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_tool_loop_detector PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_tool_loop_detector COMMAND test_ai_tool_loop_detector
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_tool_loop_detector")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_package_tool_planner.cpp")
    add_executable(test_ai_package_tool_planner
        unit/test_ai_package_tool_planner.cpp
        ../src/ai/ai_package_tool_planner.cpp
        ../include/sak/ai/ai_package_tool_planner.h
    )
    set_target_properties(test_ai_package_tool_planner PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_package_tool_planner PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_package_tool_planner PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_package_tool_planner COMMAND test_ai_package_tool_planner
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_package_tool_planner")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_offline_downloader_tool_runner.cpp")
    add_executable(test_ai_offline_downloader_tool_runner
        unit/test_ai_offline_downloader_tool_runner.cpp
        ../src/ai/ai_offline_downloader_tool_runner.cpp
        ../include/sak/ai/ai_offline_downloader_tool_runner.h
    )
    set_target_properties(test_ai_offline_downloader_tool_runner PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_offline_downloader_tool_runner PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_offline_downloader_tool_runner PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_offline_downloader_tool_runner
             COMMAND test_ai_offline_downloader_tool_runner
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_offline_downloader_tool_runner")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_workflow_powershell_tool_runner.cpp")
    add_executable(test_ai_workflow_powershell_tool_runner
        unit/test_ai_workflow_powershell_tool_runner.cpp
        ../src/ai/ai_workflow_powershell_tool_runner.cpp
        ../src/ai/ai_command_guard.cpp
        ../src/ai/ai_execution_broker.cpp
        ../src/ai/ai_credential_store.cpp
        ../src/ai/ai_paths.cpp
        ../src/core/app_paths.cpp
        ../src/core/secure_memory.cpp
        ../include/sak/ai/ai_workflow_powershell_tool_runner.h
        ../include/sak/ai/ai_command_guard.h
        ../include/sak/ai/ai_execution_broker.h
        ../include/sak/ai/ai_credential_store.h
        ../include/sak/ai/ai_paths.h
        ../include/sak/app_paths.h
        ../include/sak/secure_memory.h
    )
    set_target_properties(test_ai_workflow_powershell_tool_runner PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_workflow_powershell_tool_runner PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_ai_workflow_powershell_tool_runner PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_workflow_powershell_tool_runner
             COMMAND test_ai_workflow_powershell_tool_runner
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_workflow_powershell_tool_runner")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_tool_dispatcher.cpp")
    add_executable(test_ai_tool_dispatcher
        unit/test_ai_tool_dispatcher.cpp
        ../src/ai/ai_lease_manager.cpp
        ../src/ai/ai_tool_dispatcher.cpp
        ../src/ai/ai_tool_health_ledger.cpp
        ../src/ai/ai_tool_policy.cpp
        ../include/sak/ai/ai_lease_manager.h
        ../include/sak/ai/ai_tool_dispatcher.h
        ../include/sak/ai/ai_tool_health_ledger.h
        ../include/sak/ai/ai_tool_policy.h
    )
    set_target_properties(test_ai_tool_dispatcher PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_tool_dispatcher PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_tool_dispatcher PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_tool_dispatcher COMMAND test_ai_tool_dispatcher
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_tool_dispatcher")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_tool_turn.cpp")
    add_executable(test_ai_tool_turn
        unit/test_ai_tool_turn.cpp
        ../src/ai/ai_tool_turn.cpp
        ../include/sak/ai/ai_tool_turn.h
        ../include/sak/ai/openai_response_types.h
    )
    set_target_properties(test_ai_tool_turn PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_tool_turn PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_tool_turn PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_tool_turn COMMAND test_ai_tool_turn
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_tool_turn")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_tool_result_recorder.cpp")
    add_executable(test_ai_tool_result_recorder
        unit/test_ai_tool_result_recorder.cpp
        ../src/ai/ai_conversation_store.cpp
        ../src/ai/ai_credential_store.cpp
        ../src/ai/ai_paths.cpp
        ../src/ai/ai_tool_result_recorder.cpp
        ../src/core/app_paths.cpp
        ../include/sak/ai/ai_conversation_store.h
        ../include/sak/ai/ai_credential_store.h
        ../include/sak/ai/ai_paths.h
        ../include/sak/ai/ai_tool_result_recorder.h
        ../include/sak/app_paths.h
    )
    set_target_properties(test_ai_tool_result_recorder PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_tool_result_recorder PRIVATE
        Qt6::Core Qt6::Test crypt32
    )
    target_include_directories(test_ai_tool_result_recorder PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_tool_result_recorder COMMAND test_ai_tool_result_recorder
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_tool_result_recorder")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_run_state_store.cpp")
    add_executable(test_ai_run_state_store
        unit/test_ai_run_state_store.cpp
        ../src/ai/ai_human_gate.cpp
        ../src/ai/ai_run_state.cpp
        ../src/ai/ai_run_state_store.cpp
        ../include/sak/ai/ai_human_gate.h
        ../include/sak/ai/ai_run_state.h
        ../include/sak/ai/ai_run_state_store.h
    )
    set_target_properties(test_ai_run_state_store PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_run_state_store PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_run_state_store PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_run_state_store COMMAND test_ai_run_state_store
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_run_state_store")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_subagent_runner.cpp")
    add_executable(test_ai_subagent_runner
        unit/test_ai_subagent_runner.cpp
        ../src/ai/ai_cancellation_token.cpp
        ../src/ai/ai_subagent_runner.cpp
        ../src/ai/ai_token_usage_tracker.cpp
        ../src/ai/ai_tool_policy.cpp
        ../include/sak/ai/ai_cancellation_token.h
        ../include/sak/ai/ai_subagent_runner.h
        ../include/sak/ai/ai_token_usage_tracker.h
        ../include/sak/ai/ai_tool_policy.h
    )
    set_target_properties(test_ai_subagent_runner PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_subagent_runner PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_subagent_runner PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_subagent_runner COMMAND test_ai_subagent_runner
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_subagent_runner")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_orchestrator.cpp")
    add_executable(test_ai_orchestrator
        unit/test_ai_orchestrator.cpp
        ../src/ai/ai_cancellation_token.cpp
        ../src/ai/ai_human_gate.cpp
        ../src/ai/ai_orchestrator.cpp
        ../src/ai/ai_recovery_policy.cpp
        ../src/ai/ai_run_state.cpp
        ../src/ai/ai_subagent_runner.cpp
        ../src/ai/ai_token_usage_tracker.cpp
        ../src/ai/ai_tool_policy.cpp
        ../src/ai/ai_workflow_placeholders.cpp
        ../src/ai/ai_workflow_template.cpp
        ../include/sak/ai/ai_cancellation_token.h
        ../include/sak/ai/ai_human_gate.h
        ../include/sak/ai/ai_orchestrator.h
        ../include/sak/ai/ai_recovery_policy.h
        ../include/sak/ai/ai_run_state.h
        ../include/sak/ai/ai_subagent_runner.h
        ../include/sak/ai/ai_token_usage_tracker.h
        ../include/sak/ai/ai_tool_policy.h
        ../include/sak/ai/ai_workflow_placeholders.h
        ../include/sak/ai/ai_workflow_template.h
    )

    add_executable(test_ai_workflow_evals
        unit/test_ai_workflow_evals.cpp
        ../src/ai/ai_cancellation_token.cpp
        ../src/ai/ai_human_gate.cpp
        ../src/ai/ai_orchestrator.cpp
        ../src/ai/ai_recovery_policy.cpp
        ../src/ai/ai_run_state.cpp
        ../src/ai/ai_subagent_runner.cpp
        ../src/ai/ai_token_usage_tracker.cpp
        ../src/ai/ai_tool_policy.cpp
        ../src/ai/ai_workflow_placeholders.cpp
        ../src/ai/ai_workflow_template.cpp
        ../include/sak/ai/ai_cancellation_token.h
        ../include/sak/ai/ai_human_gate.h
        ../include/sak/ai/ai_orchestrator.h
        ../include/sak/ai/ai_recovery_policy.h
        ../include/sak/ai/ai_run_state.h
        ../include/sak/ai/ai_subagent_runner.h
        ../include/sak/ai/ai_token_usage_tracker.h
        ../include/sak/ai/ai_tool_policy.h
        ../include/sak/ai/ai_workflow_placeholders.h
        ../include/sak/ai/ai_workflow_template.h
    )
    set_target_properties(test_ai_workflow_evals PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_workflow_evals PRIVATE Qt6::Core Qt6::Test)
    target_include_directories(test_ai_workflow_evals PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_workflow_evals COMMAND test_ai_workflow_evals
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_workflow_evals")
    set_target_properties(test_ai_orchestrator PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_orchestrator PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_orchestrator PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_orchestrator COMMAND test_ai_orchestrator
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_orchestrator")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ai_workflow_store.cpp")
    add_executable(test_ai_workflow_store
        unit/test_ai_workflow_store.cpp
        ../src/core/app_paths.cpp
        ../src/ai/ai_paths.cpp
        ../src/ai/ai_workflow_store.cpp
        ../src/ai/ai_workflow_template.cpp
        ../include/sak/ai/ai_paths.h
        ../include/sak/app_paths.h
        ../include/sak/ai/ai_workflow_store.h
        ../include/sak/ai/ai_workflow_template.h
        ../resources/ai.qrc
    )
    set_target_properties(test_ai_workflow_store PROPERTIES
        AUTOMOC ON
        AUTORCC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ai_workflow_store PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_ai_workflow_store PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ai_workflow_store COMMAND test_ai_workflow_store
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_workflow_store")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_smart_disk_analyzer.cpp")
    add_executable(test_smart_disk_analyzer
        unit/test_smart_disk_analyzer.cpp
        ../src/core/smart_disk_analyzer.cpp
        ../src/core/bundled_tools_manager.cpp
        ${SAK_PROCESS_RUNNER_SOURCES}
        ../src/core/logger.cpp
        ../include/sak/smart_disk_analyzer.h
        ../include/sak/bundled_tools_manager.h
    )
    set_target_properties(test_smart_disk_analyzer PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_smart_disk_analyzer PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_smart_disk_analyzer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_smart_disk_analyzer COMMAND test_smart_disk_analyzer
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_smart_disk_analyzer")
endif()

# test_package_matcher ??? mapping CRUD, export/import, findMatch
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_package_matcher.cpp")
    add_executable(test_package_matcher
        unit/test_package_matcher.cpp
        ../src/core/package_matcher.cpp
        ../src/core/app_scanner.cpp
        ../src/core/bundled_tools_manager.cpp
        ../src/core/chocolatey_manager.cpp
        ${SAK_PROCESS_RUNNER_SOURCES}
        ../src/core/logger.cpp
        ../include/sak/package_matcher.h
        ../include/sak/chocolatey_manager.h
    )
    set_target_properties(test_package_matcher PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_package_matcher PRIVATE
        Qt6::Core Qt6::Test Qt6::Concurrent ${PLATFORM_LIBS}
    )
    target_include_directories(test_package_matcher PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_package_matcher COMMAND test_package_matcher
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_package_matcher")
endif()

# --- Tier 3: Hardware & System ---

# test_drive_scanner ??? DriveInfo struct, getDrives, lifecycle
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_drive_scanner.cpp")
    add_executable(test_drive_scanner
        unit/test_drive_scanner.cpp
        ../src/core/drive_scanner.cpp
        ../src/core/logger.cpp
        ../include/sak/drive_scanner.h
    )
    set_target_properties(test_drive_scanner PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_drive_scanner PRIVATE
        Qt6::Core Qt6::Concurrent Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_drive_scanner PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_drive_scanner COMMAND test_drive_scanner
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_drive_scanner")
endif()

# test_permission_manager ??? canModifyPermissions, getOwner, SDDL, stripPermissions
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_permission_manager.cpp")
    add_executable(test_permission_manager
        unit/test_permission_manager.cpp
        ../src/core/permission_manager.cpp
        ../src/core/elevation_manager.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_permission_manager PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_permission_manager PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_permission_manager PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_permission_manager COMMAND test_permission_manager
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_permission_manager")
endif()

# test_elevation_manager ??? isElevated, canElevate, error messages
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_elevation_manager.cpp")
    add_executable(test_elevation_manager
        unit/test_elevation_manager.cpp
        ../src/core/elevation_manager.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_elevation_manager PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_elevation_manager PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_elevation_manager PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_elevation_manager COMMAND test_elevation_manager
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_elevation_manager")
endif()

# test_elevation_tier ??? tier classification, feature lookup, table integrity
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_elevation_tier.cpp")
    add_executable(test_elevation_tier
        unit/test_elevation_tier.cpp
    )
    set_target_properties(test_elevation_tier PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_elevation_tier PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_elevation_tier PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_elevation_tier COMMAND test_elevation_tier
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_elevation_tier")
endif()

# test_elevated_pipe_protocol ??? wire protocol framing, parsing, pipe name generation
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_elevated_pipe_protocol.cpp")
    add_executable(test_elevated_pipe_protocol
        unit/test_elevated_pipe_protocol.cpp
    )
    set_target_properties(test_elevated_pipe_protocol PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_elevated_pipe_protocol PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_elevated_pipe_protocol PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_elevated_pipe_protocol COMMAND test_elevated_pipe_protocol
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_elevated_pipe_protocol")
endif()

# test_elevated_task_dispatcher ??? allowlist, dispatch, exception safety, callbacks
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_elevated_task_dispatcher.cpp")
    add_executable(test_elevated_task_dispatcher
        unit/test_elevated_task_dispatcher.cpp
        ../src/core/elevated_task_dispatcher.cpp
        ../include/sak/elevated_task_dispatcher.h
        ../src/core/logger.cpp
        ../include/sak/logger.h
    )
    set_target_properties(test_elevated_task_dispatcher PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_elevated_task_dispatcher PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_elevated_task_dispatcher PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_elevated_task_dispatcher COMMAND test_elevated_task_dispatcher
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_elevated_task_dispatcher")
endif()

# test_mixed_tier_operations ??? Phase 3 mixed-tier elevation: PermissionManager
# expected overloads, canReadPath, backup worker signal, task handlers, thermal
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_mixed_tier_operations.cpp")
    add_executable(test_mixed_tier_operations
        unit/test_mixed_tier_operations.cpp
        ../src/core/elevated_task_dispatcher.cpp
        ../src/core/thermal_monitor.cpp
        ../src/core/permission_manager.cpp
        ../src/core/elevation_manager.cpp
        ../src/core/user_profile_backup_worker.cpp
        ../src/core/user_profile_types.cpp
        ../src/core/smart_file_filter.cpp
        ../src/core/path_utils.cpp
        ${SAK_PROCESS_RUNNER_SOURCES}
        ../src/core/logger.cpp
        ../include/sak/elevated_task_dispatcher.h
        ../include/sak/thermal_monitor.h
        ../include/sak/permission_manager.h
        ../include/sak/user_profile_backup_worker.h
        ../include/sak/user_profile_types.h
    )
    set_target_properties(test_mixed_tier_operations PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_mixed_tier_operations PRIVATE
        Qt6::Core Qt6::Concurrent Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_mixed_tier_operations PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_mixed_tier_operations COMMAND test_mixed_tier_operations
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_mixed_tier_operations")
endif()

# test_elevation_ux ??? Phase 4 shield icon extraction, elevation banner, style tokens
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_elevation_ux.cpp")
    add_executable(test_elevation_ux
        unit/test_elevation_ux.cpp
        ../src/core/elevation_manager.cpp
        ../src/core/logger.cpp
        ../include/sak/elevation_manager.h
        ../include/sak/shield_icon.h
        ../include/sak/elevation_banner.h
        ../resources/icons.qrc
    )
    set_target_properties(test_elevation_ux PROPERTIES
        AUTOMOC ON
        AUTORCC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_elevation_ux PRIVATE
        Qt6::Core Qt6::Widgets Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_elevation_ux PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_elevation_ux COMMAND test_elevation_ux
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_elevation_ux")
endif()

# test_elevation_hardening ??? Phase 5 legacy removal, tier coverage, gate, allowlist, IPC
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_elevation_hardening.cpp")
    add_executable(test_elevation_hardening
        unit/test_elevation_hardening.cpp
        ../src/core/elevation_manager.cpp
        ../src/core/elevated_task_dispatcher.cpp
        ../src/core/logger.cpp
        ../include/sak/elevation_tier.h
        ../include/sak/elevation_gate.h
        ../include/sak/elevated_pipe_protocol.h
        ../include/sak/elevated_task_dispatcher.h
    )
    set_target_properties(test_elevation_hardening PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_elevation_hardening PRIVATE
        Qt6::Core Qt6::Widgets Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_elevation_hardening PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_elevation_hardening COMMAND test_elevation_hardening
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_elevation_hardening")
endif()

# --- Tier 4: Streaming/IO ---

# test_streaming_decompressor ??? gzip/bzip2/xz lifecycle, byte tracking
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_streaming_decompressor.cpp")
    add_executable(test_streaming_decompressor
        unit/test_streaming_decompressor.cpp
        ../src/core/streaming_decompressor.cpp
        ../src/core/decompressor_factory.cpp
        ../src/core/gzip_decompressor.cpp
        ../src/core/bzip2_decompressor.cpp
        ../src/core/xz_decompressor.cpp
        ../src/core/logger.cpp
        ../include/sak/streaming_decompressor.h
        ../include/sak/gzip_decompressor.h
        ../include/sak/bzip2_decompressor.h
        ../include/sak/xz_decompressor.h
    )
    set_target_properties(test_streaming_decompressor PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_streaming_decompressor PRIVATE
        Qt6::Core Qt6::Test ZLIB::ZLIB BZip2::BZip2 LibLZMA::LibLZMA ${PLATFORM_LIBS}
    )
    target_include_directories(test_streaming_decompressor PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_streaming_decompressor COMMAND test_streaming_decompressor
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_streaming_decompressor")
endif()

# test_logger ??? initialization, level filtering, file output, thread safety
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_logger.cpp")
    add_executable(test_logger
        unit/test_logger.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_logger PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_logger PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_logger PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_logger COMMAND test_logger
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_logger")
endif()

# ============================================================================
# Unit Tests - Offline Deployment
# ============================================================================

# test_install_script_parser ??? PowerShell install script parsing
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_install_script_parser.cpp")
    add_executable(test_install_script_parser
        unit/test_install_script_parser.cpp
        ../src/core/install_script_parser.cpp
        ../include/sak/install_script_parser.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_install_script_parser PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_install_script_parser PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_install_script_parser PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_install_script_parser COMMAND test_install_script_parser
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_install_script_parser")
endif()

# test_script_rewriter ??? URL replacement for offline deployment
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_script_rewriter.cpp")
    add_executable(test_script_rewriter
        unit/test_script_rewriter.cpp
        ../src/core/script_rewriter.cpp
        ../src/core/install_script_parser.cpp
        ../include/sak/script_rewriter.h
        ../include/sak/install_script_parser.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_script_rewriter PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_script_rewriter PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_script_rewriter PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_script_rewriter COMMAND test_script_rewriter
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_script_rewriter")
endif()

# test_package_list_manager ??? curated package lists and presets
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_package_list_manager.cpp")
    add_executable(test_package_list_manager
        unit/test_package_list_manager.cpp
        ../src/core/package_list_manager.cpp
        ../include/sak/package_list_manager.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_package_list_manager PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_package_list_manager PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_package_list_manager PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_package_list_manager COMMAND test_package_list_manager
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_package_list_manager")
endif()

# test_vulnerability_scanner - vulnerability source parsing and product matching
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_vulnerability_scanner.cpp")
    add_executable(test_vulnerability_scanner
        unit/test_vulnerability_scanner.cpp
        ../src/core/vulnerability_scanner.cpp
        ../src/core/app_paths.cpp
        ../include/sak/vulnerability_scanner.h
        ../include/sak/app_paths.h
        ${SAK_NETWORK_TRANSFER_RUNNER_SOURCES}
    )
    set_target_properties(test_vulnerability_scanner PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_vulnerability_scanner PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test Qt6::Network ${PLATFORM_LIBS}
    )
    target_include_directories(test_vulnerability_scanner PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_vulnerability_scanner COMMAND test_vulnerability_scanner
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_vulnerability_scanner")
endif()

# test_nuget_api_client ??? OData XML parsing, construction, metadata structs
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_nuget_api_client.cpp")
    add_executable(test_nuget_api_client
        unit/test_nuget_api_client.cpp
        ../src/core/nuget_api_client.cpp
        ../include/sak/nuget_api_client.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_nuget_api_client PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_nuget_api_client PRIVATE
        Qt6::Core Qt6::Test Qt6::Network Qt6::Xml ${PLATFORM_LIBS}
    )
    target_include_directories(test_nuget_api_client PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_nuget_api_client COMMAND test_nuget_api_client
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_nuget_api_client")
endif()

# test_offline_package_search ??? offline deploy search workflow, dedup, data flow
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_offline_package_search.cpp")
    add_executable(test_offline_package_search
        unit/test_offline_package_search.cpp
        ../src/core/chocolatey_manager.cpp
        ../include/sak/chocolatey_manager.h
        ../src/core/package_list_manager.cpp
        ../include/sak/package_list_manager.h
        ${SAK_PROCESS_RUNNER_SOURCES}
        ../src/core/logger.cpp
    )
    set_target_properties(test_offline_package_search PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_offline_package_search PRIVATE
        Qt6::Core Qt6::Test Qt6::Network Qt6::Xml Qt6::Widgets ${PLATFORM_LIBS}
    )
    target_include_directories(test_offline_package_search PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_offline_package_search COMMAND test_offline_package_search
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_offline_package_search")
endif()

# ============================================================================
# Integration Tests
# ============================================================================

# Integration test: UUP Conversion Pipeline
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_uup_conversion_pipeline.cpp")
    add_executable(test_uup_conversion_pipeline
        integration/test_uup_conversion_pipeline.cpp
    )

    set_target_properties(test_uup_conversion_pipeline PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )

    target_link_libraries(test_uup_conversion_pipeline PRIVATE
        Qt6::Core
        Qt6::Test
        Qt6::Widgets
        ${PLATFORM_LIBS}
    )

    target_include_directories(test_uup_conversion_pipeline PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )

    add_test(NAME test_uup_conversion_pipeline
             COMMAND test_uup_conversion_pipeline
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")

    message(STATUS "Added integration test: test_uup_conversion_pipeline")
endif()

# ============================================================================
# Unit Tests - Quick Actions
# ============================================================================

# test_action_factory ??? Action metadata, category distribution, initial state
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/actions/test_action_factory.cpp")
    add_executable(test_action_factory
        unit/actions/test_action_factory.cpp

        # Base class
        ../src/core/quick_action.cpp
        ../include/sak/quick_action.h

        # Dependencies used by actions
        ../src/core/process_runner.cpp
        ../include/sak/process_runner.h
        ../src/core/logger.cpp
        ../include/sak/logger.h
        ../src/core/bundled_tools_manager.cpp
        ../include/sak/bundled_tools_manager.h
        ../src/core/path_utils.cpp
        ../include/sak/path_utils.h
        ../src/core/windows_user_scanner.cpp
        ../include/sak/windows_user_scanner.h
        ../src/core/user_profile_types.cpp
        ../include/sak/user_profile_types.h
        ../src/core/chocolatey_manager.cpp
        ../include/sak/chocolatey_manager.h

        # System Optimization actions
        ../src/actions/optimize_power_settings_action.cpp
        ../include/sak/actions/optimize_power_settings_action.h

        # Maintenance actions
        ../src/actions/verify_system_files_action.cpp
        ../include/sak/actions/verify_system_files_action.h
        ../src/actions/check_disk_errors_action.cpp
        ../include/sak/actions/check_disk_errors_action.h
        ../src/actions/reset_network_action.cpp
        ../include/sak/actions/reset_network_action.h

        # Troubleshooting actions
        ../src/actions/generate_system_report_action.cpp
        ../include/sak/actions/generate_system_report_action.h

        # Emergency Recovery actions
        ../src/actions/screenshot_settings_action.cpp
        ../include/sak/actions/screenshot_settings_action.h
        ../src/actions/backup_bitlocker_keys_action.cpp
        ../include/sak/actions/backup_bitlocker_keys_action.h
    )

    set_target_properties(test_action_factory PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )

    target_link_libraries(test_action_factory PRIVATE
        Qt6::Core
        Qt6::Test
        Qt6::Widgets
        ${PLATFORM_LIBS}
    )

    target_include_directories(test_action_factory PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )

    add_test(NAME test_action_factory
             COMMAND test_action_factory
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")

    message(STATUS "Added unit test: test_action_factory")
endif()

# ============================================================================
# Unit Tests - Enterprise Hardening Sprint (TST-02, TST-04, TST-05, TST-10, TST-11)
# ============================================================================

# test_flash_types ??? FlashProgress arithmetic, FlashResult checks (TST-02)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_flash_types.cpp")
    add_executable(test_flash_types
        unit/test_flash_types.cpp
    )
    set_target_properties(test_flash_types PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_flash_types PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_flash_types PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_flash_types COMMAND test_flash_types
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_flash_types")
endif()

# test_quick_action ??? QuickAction static helpers and base class (TST-10)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_quick_action.cpp")
    add_executable(test_quick_action
        unit/test_quick_action.cpp
        ../src/core/quick_action.cpp
        ../include/sak/quick_action.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_quick_action PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_quick_action PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_quick_action PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_quick_action COMMAND test_quick_action
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_quick_action")
endif()

# test_user_profile_types ??? Serialization round-trips (TST-05)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_user_profile_types.cpp")
    add_executable(test_user_profile_types
        unit/test_user_profile_types.cpp
        ../src/core/user_profile_types.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_user_profile_types PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_user_profile_types PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_user_profile_types PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_user_profile_types COMMAND test_user_profile_types
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_user_profile_types")
endif()

# test_user_data_manager ??? Checksums, data locations, size calc (TST-04)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_user_data_manager.cpp")
    add_executable(test_user_data_manager
        unit/test_user_data_manager.cpp
        ../src/core/user_data_manager.cpp
        ../include/sak/user_data_manager.h
        ../src/core/encryption.cpp
        ../src/core/secure_memory.cpp
        ${SAK_PROCESS_RUNNER_SOURCES}
        ../src/core/logger.cpp
    )
    set_target_properties(test_user_data_manager PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_user_data_manager PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_user_data_manager PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_user_data_manager COMMAND test_user_data_manager
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_user_data_manager")
endif()

# test_duplicate_finder_worker ??? Duplicate detection with temp files (TST-11)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_duplicate_finder_worker.cpp")
    add_executable(test_duplicate_finder_worker
        unit/test_duplicate_finder_worker.cpp
        ../src/core/file_management_file_system.cpp
        ../src/core/apfs_crypto.cpp
        ../src/core/apfs_keybag.cpp
        ../src/core/partition_apfs_file_system_reader.cpp
        ../src/core/partition_apfs_writer.cpp
        ../src/core/partition_ext_file_system_reader.cpp
        ../src/core/partition_hfs_file_system_reader.cpp
        ../src/core/partition_hfs_file_system_writer.cpp
        ../src/core/partition_raw_device_io.cpp
        ../src/core/partition_file_system_detector.cpp
        ../src/core/partition_file_system_registry.cpp
        ../src/threading/duplicate_finder_worker.cpp
        ../include/sak/file_management_file_system.h
        ../include/sak/duplicate_finder_worker.h
        ../include/sak/partition_apfs_file_system_reader.h
        ../include/sak/partition_apfs_writer.h
        ../include/sak/partition_ext_file_system_reader.h
        ../include/sak/partition_hfs_file_system_reader.h
        ../include/sak/partition_raw_device_io.h
        ../include/sak/partition_file_system_detector.h
        ../include/sak/partition_file_system_registry.h
        ../src/threading/worker_base.cpp
        ../include/sak/worker_base.h
        ../src/core/file_hash.cpp
        ../include/sak/file_hash.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_duplicate_finder_worker PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_duplicate_finder_worker PRIVATE
        Qt6::Core Qt6::Test Qt6::Concurrent ZLIB::ZLIB sak_lzfse ${PLATFORM_LIBS}
    )
    target_include_directories(test_duplicate_finder_worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_duplicate_finder_worker COMMAND test_duplicate_finder_worker
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_duplicate_finder_worker")
endif()

# test_organizer_worker ??? File categorization and undo (TST-11)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_organizer_worker.cpp")
    add_executable(test_organizer_worker
        unit/test_organizer_worker.cpp
        ../src/threading/organizer_worker.cpp
        ../include/sak/organizer_worker.h
        ../src/threading/worker_base.cpp
        ../include/sak/worker_base.h
        ../src/core/logger.cpp
        ../src/core/input_validator.cpp
    )
    set_target_properties(test_organizer_worker PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_organizer_worker PRIVATE
        Qt6::Core Qt6::Test Qt6::Concurrent ${PLATFORM_LIBS}
    )
    target_include_directories(test_organizer_worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_organizer_worker COMMAND test_organizer_worker
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_organizer_worker")
endif()

# test_app_installation_worker ??? Job filtering, stats, state management (TST-03)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_app_installation_worker.cpp")
    add_executable(test_app_installation_worker
        unit/test_app_installation_worker.cpp
        ../src/core/app_installation_worker.cpp
        ../include/sak/app_installation_worker.h
        ../src/core/chocolatey_manager.cpp
        ../include/sak/chocolatey_manager.h
        ../src/core/migration_report.cpp
        ../include/sak/migration_report.h
        ../src/core/app_scanner.cpp
        ../include/sak/app_scanner.h
        ../src/core/bundled_tools_manager.cpp
        ../src/core/package_matcher.cpp
        ../include/sak/package_matcher.h
        ${SAK_PROCESS_RUNNER_SOURCES}
        ../src/core/logger.cpp
    )
    set_target_properties(test_app_installation_worker PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_app_installation_worker PRIVATE
        Qt6::Core Qt6::Test Qt6::Concurrent Qt6::Network ${PLATFORM_LIBS}
    )
    target_include_directories(test_app_installation_worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_app_installation_worker COMMAND test_app_installation_worker
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_app_installation_worker")
endif()

# test_user_profile_restore_worker ??? Restore flow, conflict resolution, signals (TST-06)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_user_profile_restore_worker.cpp")
    add_executable(test_user_profile_restore_worker
        unit/test_user_profile_restore_worker.cpp
        ../src/core/user_profile_restore_worker.cpp
        ../include/sak/user_profile_restore_worker.h
        ../src/core/user_profile_types.cpp
        ../include/sak/user_profile_types.h
        ../src/core/smart_file_filter.cpp
        ../include/sak/smart_file_filter.h
        ../src/core/permission_manager.cpp
        ../include/sak/permission_manager.h
        ../src/core/elevation_manager.cpp
        ../include/sak/elevation_manager.h
        ../src/core/windows_user_scanner.cpp
        ../include/sak/windows_user_scanner.h
        ../src/core/path_utils.cpp
        ../include/sak/path_utils.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_user_profile_restore_worker PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_user_profile_restore_worker PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_user_profile_restore_worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_user_profile_restore_worker COMMAND test_user_profile_restore_worker
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_user_profile_restore_worker")
endif()

# test_windows_usb_creator ??? Input validation, BUG-09 disk number injection (TST-07)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_windows_usb_creator.cpp")
    add_executable(test_windows_usb_creator
        unit/test_windows_usb_creator.cpp
        ../src/core/windows_usb_creator.cpp
        ../src/core/windows_usb_creator_extract.cpp
        ../include/sak/windows_usb_creator.h
        ${SAK_PROCESS_RUNNER_SOURCES}
        ../src/core/logger.cpp
    )
    set_target_properties(test_windows_usb_creator PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_windows_usb_creator PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_windows_usb_creator PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_windows_usb_creator COMMAND test_windows_usb_creator
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_windows_usb_creator")
endif()

# test_flash_worker ??? Structs, validation, early-exit paths, cancellation (TST-09)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_flash_worker.cpp")
    add_executable(test_flash_worker
        unit/test_flash_worker.cpp
        ../src/threading/flash_worker.cpp
        ../src/threading/worker_base.cpp
        ../src/core/image_source.cpp
        ../src/core/streaming_decompressor.cpp
        ../src/core/decompressor_factory.cpp
        ../src/core/gzip_decompressor.cpp
        ../src/core/bzip2_decompressor.cpp
        ../src/core/xz_decompressor.cpp
        ../src/core/keep_awake.cpp
        ../src/core/logger.cpp
        ../include/sak/flash_worker.h
        ../include/sak/worker_base.h
        ../include/sak/image_source.h
        ../include/sak/streaming_decompressor.h
        ../include/sak/decompressor_factory.h
        ../include/sak/gzip_decompressor.h
        ../include/sak/bzip2_decompressor.h
        ../include/sak/xz_decompressor.h
        ../include/sak/keep_awake.h
    )
    set_target_properties(test_flash_worker PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_flash_worker PRIVATE
        Qt6::Core Qt6::Test ZLIB::ZLIB BZip2::BZip2 LibLZMA::LibLZMA ${PLATFORM_LIBS}
    )
    target_compile_definitions(test_flash_worker PRIVATE
        QT_DISABLE_DEPRECATED_BEFORE=0x060000
    )
    target_include_directories(test_flash_worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_flash_worker COMMAND test_flash_worker
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_flash_worker")
endif()

# ============================================================================
# Unit Tests - Phase 8: Expanded Test Coverage
# ============================================================================

# test_quick_action_result_io ??? enum ??? string round-trips, file write/read
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_quick_action_result_io.cpp")
    add_executable(test_quick_action_result_io
        unit/test_quick_action_result_io.cpp
        ../src/core/quick_action_result_io.cpp
        ../include/sak/quick_action_result_io.h
        ../src/core/quick_action.cpp
        ../include/sak/quick_action.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_quick_action_result_io PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_quick_action_result_io PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_quick_action_result_io PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_quick_action_result_io COMMAND test_quick_action_result_io
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_quick_action_result_io")
endif()

# test_keep_awake -- KeepAwake refcount / guard lifetime (P06-08 overlap)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_keep_awake.cpp")
    add_executable(test_keep_awake
        unit/test_keep_awake.cpp
        ../src/core/keep_awake.cpp
        ../include/sak/keep_awake.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_keep_awake PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_keep_awake PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_keep_awake PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_keep_awake COMMAND test_keep_awake
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_keep_awake")
endif()

# test_quick_action_controller ??? registration, lookup, category filtering
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_quick_action_controller.cpp")
    add_executable(test_quick_action_controller
        unit/test_quick_action_controller.cpp
        ../src/core/app_paths.cpp
        ../src/core/quick_action_controller.cpp
        ../include/sak/app_paths.h
        ../include/sak/quick_action_controller.h
        ../src/core/quick_action.cpp
        ../include/sak/quick_action.h
        ../src/core/quick_action_result_io.cpp
        ../include/sak/quick_action_result_io.h
        ../src/core/elevation_manager.cpp
        ../include/sak/elevation_manager.h
        ../src/core/elevation_broker.cpp
        ../include/sak/elevation_broker.h
        ../src/core/elevated_pipe_server.cpp
        ../include/sak/elevated_pipe_server.h
        ../src/core/elevated_task_dispatcher.cpp
        ../include/sak/elevated_task_dispatcher.h
        ../src/core/logger.cpp
        ../include/sak/logger.h
    )
    set_target_properties(test_quick_action_controller PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_quick_action_controller PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_quick_action_controller PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_quick_action_controller COMMAND test_quick_action_controller
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_quick_action_controller")
endif()

# test_migration_report ??? entry CRUD, selection, counts, distribution
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_migration_report.cpp")
    add_executable(test_migration_report
        unit/test_migration_report.cpp
        ../src/core/migration_report.cpp
        ../include/sak/migration_report.h
        ../src/core/app_scanner.cpp
        ../include/sak/app_scanner.h
        ../src/core/bundled_tools_manager.cpp
        ../src/core/package_matcher.cpp
        ../include/sak/package_matcher.h
        ../src/core/chocolatey_manager.cpp
        ../include/sak/chocolatey_manager.h
        ${SAK_PROCESS_RUNNER_SOURCES}
        ../src/core/logger.cpp
    )
    set_target_properties(test_migration_report PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_migration_report PRIVATE
        Qt6::Core Qt6::Test Qt6::Concurrent Qt6::Network ${PLATFORM_LIBS}
    )
    target_include_directories(test_migration_report PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_migration_report COMMAND test_migration_report
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_migration_report")
endif()

# test_linux_distro_catalog ??? catalog integrity, category filtering, resolution
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_linux_distro_catalog.cpp")
    add_executable(test_linux_distro_catalog
        unit/test_linux_distro_catalog.cpp
        ../src/core/linux_distro_catalog.cpp
        ../include/sak/linux_distro_catalog.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_linux_distro_catalog PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_linux_distro_catalog PRIVATE
        Qt6::Core Qt6::Test Qt6::Network ${PLATFORM_LIBS}
    )
    target_include_directories(test_linux_distro_catalog PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_linux_distro_catalog COMMAND test_linux_distro_catalog
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_linux_distro_catalog")
endif()

# test_flash_coordinator ??? defaults, config, guards, cancel safety
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_flash_coordinator.cpp")
    add_executable(test_flash_coordinator
        unit/test_flash_coordinator.cpp
        ../src/core/flash_coordinator.cpp
        ../include/sak/flash_coordinator.h
        ../src/threading/flash_worker.cpp
        ../include/sak/flash_worker.h
        ../src/threading/worker_base.cpp
        ../include/sak/worker_base.h
        ../src/core/image_source.cpp
        ../include/sak/image_source.h
        ../src/core/drive_unmounter.cpp
        ../include/sak/drive_unmounter.h
        ../src/core/drive_scanner.cpp
        ../include/sak/drive_scanner.h
        ../src/core/input_validator.cpp
        ../src/core/streaming_decompressor.cpp
        ../include/sak/streaming_decompressor.h
        ../src/core/decompressor_factory.cpp
        ../src/core/gzip_decompressor.cpp
        ../include/sak/gzip_decompressor.h
        ../src/core/bzip2_decompressor.cpp
        ../include/sak/bzip2_decompressor.h
        ../src/core/xz_decompressor.cpp
        ../include/sak/xz_decompressor.h
        ../src/core/keep_awake.cpp
        ../include/sak/keep_awake.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_flash_coordinator PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_flash_coordinator PRIVATE
        Qt6::Core Qt6::Concurrent Qt6::Test ZLIB::ZLIB BZip2::BZip2 LibLZMA::LibLZMA ${PLATFORM_LIBS}
    )
    target_compile_definitions(test_flash_coordinator PRIVATE
        QT_DISABLE_DEPRECATED_BEFORE=0x060000
    )
    target_include_directories(test_flash_coordinator PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_flash_coordinator COMMAND test_flash_coordinator
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_flash_coordinator")
endif()

# test_user_profile_backup_worker ??? construction, initial state, cancel
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_user_profile_backup_worker.cpp")
    add_executable(test_user_profile_backup_worker
        unit/test_user_profile_backup_worker.cpp
        ../src/core/user_profile_backup_worker.cpp
        ../include/sak/user_profile_backup_worker.h
        ../src/core/user_profile_types.cpp
        ../include/sak/user_profile_types.h
        ../src/core/smart_file_filter.cpp
        ../include/sak/smart_file_filter.h
        ../src/core/permission_manager.cpp
        ../include/sak/permission_manager.h
        ../src/core/elevation_manager.cpp
        ../include/sak/elevation_manager.h
        ../src/core/path_utils.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_user_profile_backup_worker PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_user_profile_backup_worker PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_user_profile_backup_worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_user_profile_backup_worker COMMAND test_user_profile_backup_worker
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_user_profile_backup_worker")
endif()

# test_uup_iso_builder ??? phase defaults, cancel, weight constants
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_uup_iso_builder.cpp")
    add_executable(test_uup_iso_builder
        unit/test_uup_iso_builder.cpp
        ../src/core/app_paths.cpp
        ../src/core/uup_iso_builder.cpp
        ../include/sak/app_paths.h
        ../include/sak/uup_iso_builder.h
        ../src/core/uup_dump_api.cpp
        ../include/sak/uup_dump_api.h
        ../src/core/bundled_tools_manager.cpp
        ../include/sak/bundled_tools_manager.h
        ../src/core/elevation_manager.cpp
        ../include/sak/elevation_manager.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_uup_iso_builder PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_uup_iso_builder PRIVATE
        Qt6::Core Qt6::Test Qt6::Network ${PLATFORM_LIBS}
    )
    target_include_directories(test_uup_iso_builder PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_uup_iso_builder COMMAND test_uup_iso_builder
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_uup_iso_builder")
endif()

# test_all_actions_metadata ??? per-action expected values, cancel behavior
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/actions/test_all_actions_metadata.cpp")
    add_executable(test_all_actions_metadata
        unit/actions/test_all_actions_metadata.cpp

        # Base class
        ../src/core/quick_action.cpp
        ../include/sak/quick_action.h

        # Dependencies used by actions
        ../src/core/process_runner.cpp
        ../include/sak/process_runner.h
        ../src/core/logger.cpp
        ../include/sak/logger.h
        ../src/core/bundled_tools_manager.cpp
        ../include/sak/bundled_tools_manager.h
        ../src/core/path_utils.cpp
        ../include/sak/path_utils.h
        ../src/core/windows_user_scanner.cpp
        ../include/sak/windows_user_scanner.h
        ../src/core/user_profile_types.cpp
        ../include/sak/user_profile_types.h
        ../src/core/chocolatey_manager.cpp
        ../include/sak/chocolatey_manager.h

        # System Optimization actions
        ../src/actions/optimize_power_settings_action.cpp
        ../include/sak/actions/optimize_power_settings_action.h

        # Maintenance actions
        ../src/actions/verify_system_files_action.cpp
        ../include/sak/actions/verify_system_files_action.h
        ../src/actions/check_disk_errors_action.cpp
        ../include/sak/actions/check_disk_errors_action.h
        ../src/actions/reset_network_action.cpp
        ../include/sak/actions/reset_network_action.h

        # Troubleshooting actions
        ../src/actions/generate_system_report_action.cpp
        ../include/sak/actions/generate_system_report_action.h

        # Emergency Recovery actions
        ../src/actions/screenshot_settings_action.cpp
        ../include/sak/actions/screenshot_settings_action.h
        ../src/actions/backup_bitlocker_keys_action.cpp
        ../include/sak/actions/backup_bitlocker_keys_action.h
    )

    set_target_properties(test_all_actions_metadata PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_all_actions_metadata PRIVATE
        Qt6::Core Qt6::Test Qt6::Widgets ${PLATFORM_LIBS}
    )
    target_include_directories(test_all_actions_metadata PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_all_actions_metadata COMMAND test_all_actions_metadata
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_all_actions_metadata")
endif()

# ============================================================================
# Unit Tests - Advanced Search Panel
# ============================================================================

# test_advanced_search_types ??? struct defaults, extension sets, value semantics
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_advanced_search_types.cpp")
    add_executable(test_advanced_search_types
        unit/test_advanced_search_types.cpp
    )
    set_target_properties(test_advanced_search_types PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_advanced_search_types PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_advanced_search_types PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_advanced_search_types COMMAND test_advanced_search_types
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_advanced_search_types")
endif()

# test_regex_pattern_library ??? built-in/custom patterns, enable/disable, CRUD, combined
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_regex_pattern_library.cpp")
    add_executable(test_regex_pattern_library
        unit/test_regex_pattern_library.cpp
        ../src/core/regex_pattern_library.cpp
        ../src/core/app_paths.cpp
        ../src/core/logger.cpp
        ../include/sak/app_paths.h
        ../include/sak/regex_pattern_library.h
    )
    set_target_properties(test_regex_pattern_library PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_regex_pattern_library PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_regex_pattern_library PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_regex_pattern_library COMMAND test_regex_pattern_library
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_regex_pattern_library")
endif()

# test_advanced_search_worker ??? regex compilation, text/metadata/archive/binary search
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_advanced_search_worker.cpp")
    add_executable(test_advanced_search_worker
        unit/test_advanced_search_worker.cpp
        ../src/core/file_management_file_system.cpp
        ../src/core/apfs_crypto.cpp
        ../src/core/apfs_keybag.cpp
        ../src/core/partition_apfs_file_system_reader.cpp
        ../src/core/partition_apfs_writer.cpp
        ../src/core/partition_ext_file_system_reader.cpp
        ../src/core/partition_hfs_file_system_reader.cpp
        ../src/core/partition_hfs_file_system_writer.cpp
        ../src/core/partition_raw_device_io.cpp
        ../src/core/partition_file_system_detector.cpp
        ../src/core/partition_file_system_registry.cpp
        ../src/core/advanced_search_worker.cpp
        ../src/core/file_hash.cpp
        ../src/threading/worker_base.cpp
        ../src/core/logger.cpp
        ../include/sak/file_management_file_system.h
        ../include/sak/advanced_search_worker.h
        ../include/sak/partition_apfs_file_system_reader.h
        ../include/sak/partition_apfs_writer.h
        ../include/sak/partition_ext_file_system_reader.h
        ../include/sak/partition_hfs_file_system_reader.h
        ../include/sak/partition_raw_device_io.h
        ../include/sak/partition_file_system_detector.h
        ../include/sak/partition_file_system_registry.h
        ../include/sak/worker_base.h
    )
    set_target_properties(test_advanced_search_worker PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_advanced_search_worker PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test ZLIB::ZLIB sak_lzfse
    )
    target_include_directories(test_advanced_search_worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_advanced_search_worker COMMAND test_advanced_search_worker
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_advanced_search_worker")
endif()

# test_advanced_search_controller ??? state machine, history, preferences, lifecycle
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_advanced_search_controller.cpp")
    add_executable(test_advanced_search_controller
        unit/test_advanced_search_controller.cpp
        ../src/core/file_management_file_system.cpp
        ../src/core/apfs_crypto.cpp
        ../src/core/apfs_keybag.cpp
        ../src/core/partition_apfs_file_system_reader.cpp
        ../src/core/partition_apfs_writer.cpp
        ../src/core/partition_ext_file_system_reader.cpp
        ../src/core/partition_hfs_file_system_reader.cpp
        ../src/core/partition_hfs_file_system_writer.cpp
        ../src/core/partition_raw_device_io.cpp
        ../src/core/partition_file_system_detector.cpp
        ../src/core/partition_file_system_registry.cpp
        ../src/core/advanced_search_controller.cpp
        ../src/core/advanced_search_worker.cpp
        ../src/core/file_hash.cpp
        ../src/core/regex_pattern_library.cpp
        ../src/core/config_manager.cpp
        ${SAK_APP_PATHS_SOURCES}
        ../src/threading/worker_base.cpp
        ../src/core/logger.cpp
        ../include/sak/file_management_file_system.h
        ../include/sak/advanced_search_controller.h
        ../include/sak/advanced_search_worker.h
        ../include/sak/partition_apfs_file_system_reader.h
        ../include/sak/partition_apfs_writer.h
        ../include/sak/partition_ext_file_system_reader.h
        ../include/sak/partition_hfs_file_system_reader.h
        ../include/sak/partition_raw_device_io.h
        ../include/sak/partition_file_system_detector.h
        ../include/sak/partition_file_system_registry.h
        ../include/sak/regex_pattern_library.h
        ../include/sak/config_manager.h
        ../include/sak/worker_base.h
    )
    set_target_properties(test_advanced_search_controller PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_advanced_search_controller PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test ZLIB::ZLIB sak_lzfse
    )
    target_include_directories(test_advanced_search_controller PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_advanced_search_controller COMMAND test_advanced_search_controller
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_advanced_search_controller")
endif()

# ============================================================================
# Unit Tests - Advanced Uninstall Panel
# ============================================================================

# test_leftover_cleanup_guard --- fail-closed target screens behind software.clean_leftovers
# (header-only guard; pure, no filesystem/registry mutation).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_leftover_cleanup_guard.cpp")
    add_executable(test_leftover_cleanup_guard
        unit/test_leftover_cleanup_guard.cpp
    )
    set_target_properties(test_leftover_cleanup_guard PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_leftover_cleanup_guard PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_leftover_cleanup_guard PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_leftover_cleanup_guard COMMAND test_leftover_cleanup_guard
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_leftover_cleanup_guard")
endif()

# test_win32_mcp_server --- native win32 MCP server: JSON-RPC dispatch + tool catalog/invocation
# (Windows only; shcore for GetDpiForMonitor in list_monitors).
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_win32_mcp_server.cpp")
    # The dispatch now references BrowserControl (browser_* tools merged into
    # tools/list and routed on tools/call), so the server test links the browser
    # facade and its transitive transport/session/contract deps.
    add_executable(test_win32_mcp_server
        unit/test_win32_mcp_server.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/win32_mcp_dispatch.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/win32_mcp_tools.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/win32_mcp_desktop.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/win32_mcp_capture.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/win32_mcp_ocr.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/win32_mcp_watch.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/win32_mcp_input.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/browser_extension_installer.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/browser_control.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/browser_bridge.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/browser_contract.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/browser_bridge_pipe.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/browser_bridge_security.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/native_messaging.cpp
        ${CMAKE_SOURCE_DIR}/include/sak/win32mcp/win32_mcp_dispatch.h
        ${CMAKE_SOURCE_DIR}/include/sak/win32mcp/win32_mcp_tools.h
        ${CMAKE_SOURCE_DIR}/include/sak/win32mcp/browser_control.h
    )
    set_target_properties(test_win32_mcp_server PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_win32_mcp_server PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test ${PLATFORM_LIBS} shcore gdi32 user32 runtimeobject
    )
    target_include_directories(test_win32_mcp_server PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_win32_mcp_server COMMAND test_win32_mcp_server
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_win32_mcp_server")
endif()

# test_browser_contract --- pure DOM-snapshot renderer + browser tool catalog + tool->extension
# command translation. OS-agnostic (Qt Core only, no windows.h), so it builds and runs on any host.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_browser_contract.cpp")
    add_executable(test_browser_contract
        unit/test_browser_contract.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/browser_contract.cpp
        ${CMAKE_SOURCE_DIR}/include/sak/win32mcp/browser_contract.h
    )
    set_target_properties(test_browser_contract PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_browser_contract PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_browser_contract PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_browser_contract COMMAND test_browser_contract
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_browser_contract")
endif()

# test_native_messaging --- Chrome native-messaging frame codec + host message handler
# (ping->pong handshake). OS-agnostic (Qt Core only), so it builds and runs on any host.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_native_messaging.cpp")
    add_executable(test_native_messaging
        unit/test_native_messaging.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/native_messaging.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/win32_mcp_native_host.cpp
        ${CMAKE_SOURCE_DIR}/include/sak/win32mcp/native_messaging.h
        ${CMAKE_SOURCE_DIR}/include/sak/win32mcp/win32_mcp_native_host.h
    )
    set_target_properties(test_native_messaging PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_native_messaging PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_native_messaging PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_native_messaging COMMAND test_native_messaging
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_native_messaging")
endif()

# test_browser_bridge --- the MCP-side browser session state machine (id minting,
# single-winner reply matching, snapshot->ref_index, session/stale lifecycle).
# OS-agnostic (Qt Core only); links browser_bridge + browser_contract.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_browser_bridge.cpp")
    add_executable(test_browser_bridge
        unit/test_browser_bridge.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/browser_bridge.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/browser_contract.cpp
        ${CMAKE_SOURCE_DIR}/include/sak/win32mcp/browser_bridge.h
        ${CMAKE_SOURCE_DIR}/include/sak/win32mcp/browser_contract.h
    )
    set_target_properties(test_browser_bridge PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_browser_bridge PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_browser_bridge PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_browser_bridge COMMAND test_browser_bridge
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_browser_bridge")
endif()

# test_browser_extension_installer --- headless Chrome force-install policy + native-host
# registration. Registry tests run only on Windows (guarded in the .cpp) against a throwaway
# HKCU\Software\SAK_Test subkey; the identity guard (manifest key -> pinned id/version) runs
# everywhere. SAK_REPO_ROOT lets the identity test read the committed manifest.json.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_browser_extension_installer.cpp")
    add_executable(test_browser_extension_installer
        unit/test_browser_extension_installer.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/browser_extension_installer.cpp
        ${CMAKE_SOURCE_DIR}/include/sak/win32mcp/browser_extension_installer.h
    )
    set_target_properties(test_browser_extension_installer PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_browser_extension_installer PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_browser_extension_installer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    target_compile_definitions(test_browser_extension_installer PRIVATE
        SAK_REPO_ROOT="${CMAKE_SOURCE_DIR}"
    )
    # AUTOMOC forwards target compile definitions to moc (unlike the builtin _WIN32),
    # so the Windows-only registry test slots register with QtTest on Windows.
    if(WIN32)
        target_compile_definitions(test_browser_extension_installer PRIVATE SAK_WIN_REGISTRY_TESTS)
    endif()
    add_test(NAME test_browser_extension_installer COMMAND test_browser_extension_installer
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_browser_extension_installer")
endif()

# test_browser_bridge_security --- the load-bearing pipe security + discovery primitives
# (current-user-SID DACL + Medium NO_WRITE_UP label, per-user/session nonce pipe name,
# rendezvous record). Windows only (Win32 SID/SDDL APIs); links advapi32 via PLATFORM_LIBS.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_browser_bridge_security.cpp")
    add_executable(test_browser_bridge_security
        unit/test_browser_bridge_security.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/browser_bridge_security.cpp
        ${CMAKE_SOURCE_DIR}/include/sak/win32mcp/browser_bridge_security.h
    )
    set_target_properties(test_browser_bridge_security PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_browser_bridge_security PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_browser_bridge_security PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_browser_bridge_security COMMAND test_browser_bridge_security
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_browser_bridge_security")
endif()

# test_browser_bridge_pipe --- the bridge pipe SERVER (hardened create, accept thread,
# overlapped IO + deadline, hello/welcome handshake, peer verify) exercised end-to-end
# by an in-process client. Windows only; links the security + native-messaging codecs.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_browser_bridge_pipe.cpp")
    add_executable(test_browser_bridge_pipe
        unit/test_browser_bridge_pipe.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/browser_bridge_pipe.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/browser_bridge_security.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/native_messaging.cpp
        ${CMAKE_SOURCE_DIR}/include/sak/win32mcp/browser_bridge_pipe.h
    )
    set_target_properties(test_browser_bridge_pipe PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_browser_bridge_pipe PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_browser_bridge_pipe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_browser_bridge_pipe COMMAND test_browser_bridge_pipe
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_browser_bridge_pipe")
endif()

# test_browser_bridge_relay --- the Chrome-side RELAY (rendezvous discovery + server
# code-identity verify + hello/welcome handshake + id-transparent frame pump) and the
# MCP-side BrowserControl facade, exercised end-to-end against a real in-process pipe
# server with a fake extension. Windows only; links relay + control + transport +
# session + contract.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_browser_bridge_relay.cpp")
    add_executable(test_browser_bridge_relay
        unit/test_browser_bridge_relay.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/browser_bridge_relay.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/browser_control.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/browser_bridge.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/browser_contract.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/browser_bridge_pipe.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/browser_bridge_security.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/native_messaging.cpp
        ${CMAKE_SOURCE_DIR}/include/sak/win32mcp/browser_bridge_relay.h
        ${CMAKE_SOURCE_DIR}/include/sak/win32mcp/browser_control.h
    )
    set_target_properties(test_browser_bridge_relay PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_browser_bridge_relay PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_browser_bridge_relay PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_browser_bridge_relay COMMAND test_browser_bridge_relay
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_browser_bridge_relay")
endif()

# test_advanced_uninstall_types ??? struct defaults, enums, value semantics
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_advanced_uninstall_types.cpp")
    add_executable(test_advanced_uninstall_types
        unit/test_advanced_uninstall_types.cpp
    )
    set_target_properties(test_advanced_uninstall_types PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_advanced_uninstall_types PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test
    )
    target_include_directories(test_advanced_uninstall_types PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_advanced_uninstall_types COMMAND test_advanced_uninstall_types
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_advanced_uninstall_types")
endif()

# test_advanced_uninstall_controller ??? state machine, queue, settings, input validation
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_advanced_uninstall_controller.cpp")
    add_executable(test_advanced_uninstall_controller
        unit/test_advanced_uninstall_controller.cpp
        ../src/core/advanced_uninstall_controller.cpp
        ../src/core/program_enumerator.cpp
        ../src/core/uninstall_worker.cpp
        ../src/core/leftover_scanner.cpp
        ../src/core/cleanup_worker.cpp
        ../src/core/recycle_bin.cpp
        ../src/core/registry_snapshot_engine.cpp
        ../src/core/restore_point_manager.cpp
        ../src/core/elevation_manager.cpp
        ../src/core/config_manager.cpp
        ${SAK_APP_PATHS_SOURCES}
        ${SAK_PROCESS_RUNNER_SOURCES}
        ../src/threading/worker_base.cpp
        ../src/core/logger.cpp
        ../include/sak/advanced_uninstall_controller.h
        ../include/sak/program_enumerator.h
        ../include/sak/uninstall_worker.h
        ../include/sak/leftover_scanner.h
        ../include/sak/cleanup_worker.h
        ../include/sak/registry_snapshot_engine.h
        ../include/sak/restore_point_manager.h
        ../include/sak/config_manager.h
        ../include/sak/worker_base.h
    )
    set_target_properties(test_advanced_uninstall_controller PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_advanced_uninstall_controller PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_advanced_uninstall_controller PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_advanced_uninstall_controller COMMAND test_advanced_uninstall_controller
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_advanced_uninstall_controller")
endif()

# test_leftover_scanner ??? pattern building, risk classification, scan levels, cancellation
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_leftover_scanner.cpp")
    add_executable(test_leftover_scanner
        unit/test_leftover_scanner.cpp
        ../src/core/leftover_scanner.cpp
        ../src/core/registry_snapshot_engine.cpp
        ${SAK_PROCESS_RUNNER_SOURCES}
        ../src/core/logger.cpp
        ../include/sak/leftover_scanner.h
        ../include/sak/registry_snapshot_engine.h
    )
    set_target_properties(test_leftover_scanner PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_leftover_scanner PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_leftover_scanner PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_leftover_scanner COMMAND test_leftover_scanner
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_leftover_scanner")
endif()

# test_registry_snapshot_engine ??? diff logic, pattern matching, capture smoke test
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_registry_snapshot_engine.cpp")
    add_executable(test_registry_snapshot_engine
        unit/test_registry_snapshot_engine.cpp
        ../src/core/registry_snapshot_engine.cpp
        ../src/core/logger.cpp
        ../include/sak/registry_snapshot_engine.h
    )
    set_target_properties(test_registry_snapshot_engine PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_registry_snapshot_engine PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_registry_snapshot_engine PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_registry_snapshot_engine COMMAND test_registry_snapshot_engine
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_registry_snapshot_engine")
endif()

# ============================================================================
# Network Diagnostic Panel Tests
# ============================================================================

# test_network_diagnostic_types ??? value/move semantics, enums, compile-time invariants
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_network_diagnostic_types.cpp")
    add_executable(test_network_diagnostic_types
        unit/test_network_diagnostic_types.cpp
    )
    set_target_properties(test_network_diagnostic_types PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_network_diagnostic_types PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_network_diagnostic_types PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_network_diagnostic_types COMMAND test_network_diagnostic_types
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_network_diagnostic_types")
endif()

# test_network_diagnostic_utils ??? WiFi freq conversion, vendor lookup, port presets, DNS lists
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_network_diagnostic_utils.cpp")
    add_executable(test_network_diagnostic_utils
        unit/test_network_diagnostic_utils.cpp
        ../src/core/wifi_analyzer.cpp
        ../src/core/port_scanner.cpp
        ../src/core/dns_diagnostic_tool.cpp
        ${SAK_PROCESS_RUNNER_SOURCES}
        ../src/core/logger.cpp
        ../include/sak/wifi_analyzer.h
        ../include/sak/port_scanner.h
        ../include/sak/dns_diagnostic_tool.h
    )
    set_target_properties(test_network_diagnostic_utils PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_network_diagnostic_utils PRIVATE
        Qt6::Core Qt6::Network Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_network_diagnostic_utils PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_network_diagnostic_utils COMMAND test_network_diagnostic_utils
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_network_diagnostic_utils")
endif()

# test_network_diagnostic_report ??? HTML/JSON report generation, sections, metadata
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_network_diagnostic_report.cpp")
    add_executable(test_network_diagnostic_report
        unit/test_network_diagnostic_report.cpp
        ../src/core/network_diagnostic_report_generator.cpp
        ../include/sak/network_diagnostic_report_generator.h
    )
    set_target_properties(test_network_diagnostic_report PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_network_diagnostic_report PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_network_diagnostic_report PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_network_diagnostic_report COMMAND test_network_diagnostic_report
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_network_diagnostic_report")
endif()

# ============================================================================
# Unit Tests - Utility & Deduplication
# ============================================================================

# test_format_utils ??? sak::formatBytes() display formatting
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_format_utils.cpp")
    add_executable(test_format_utils
        unit/test_format_utils.cpp
    )
    set_target_properties(test_format_utils PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_format_utils PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_format_utils PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_format_utils COMMAND test_format_utils
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_format_utils")
endif()

# test_wifi_profile_scanner ??? WiFi profile name/security parsing
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_wifi_profile_scanner.cpp")
    add_executable(test_wifi_profile_scanner
        unit/test_wifi_profile_scanner.cpp
        ../src/core/wifi_profile_scanner.cpp
        ../src/core/user_profile_types.cpp
        ${SAK_PROCESS_RUNNER_SOURCES}
        ../src/core/logger.cpp
    )
    set_target_properties(test_wifi_profile_scanner PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_wifi_profile_scanner PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_wifi_profile_scanner PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_wifi_profile_scanner COMMAND test_wifi_profile_scanner
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_wifi_profile_scanner")
endif()

# ============================================================================
# Unit Tests - Email & PST/OST Inspector Panel
# ============================================================================

# test_email_types ??? struct defaults, enum values, constants validation
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_email_types.cpp")
    add_executable(test_email_types
        unit/test_email_types.cpp
    )
    set_target_properties(test_email_types PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_email_types PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_email_types PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_email_types COMMAND test_email_types
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_email_types")
endif()

# test_pst_parser ??? header validation, magic, encryption, lifecycle
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_pst_parser.cpp")
    add_executable(test_pst_parser
        unit/test_pst_parser.cpp
        ../src/core/pst_parser.cpp
        ../src/core/logger.cpp
        ../include/sak/pst_parser.h
    )
    set_target_properties(test_pst_parser PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_pst_parser PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_pst_parser PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_pst_parser COMMAND test_pst_parser
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_pst_parser")
endif()

# test_ost_integration ??? opens real OST/PST files and exercises full parse pipeline
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_ost_integration.cpp")
    add_executable(test_ost_integration
        integration/test_ost_integration.cpp
        ../src/core/email_export_worker.cpp
        ../src/core/eml_writer.cpp
        ../src/core/html_email_writer.cpp
        ../src/core/pdf_email_writer.cpp
        ../src/core/email_attachment_saver.cpp
        ../src/core/mbox_parser.cpp
        ../src/core/pst_parser.cpp
        ../src/core/logger.cpp
        ../include/sak/email_export_worker.h
        ../include/sak/eml_writer.h
        ../include/sak/html_email_writer.h
        ../include/sak/pdf_email_writer.h
        ../include/sak/mbox_parser.h
        ../include/sak/pst_parser.h
    )
    set_target_properties(test_ost_integration PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ost_integration PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_ost_integration PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ost_integration COMMAND test_ost_integration
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added integration test: test_ost_integration")
endif()

# test_mbox_parser ??? indexing, message loading, From line detection
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_mbox_parser.cpp")
    add_executable(test_mbox_parser
        unit/test_mbox_parser.cpp
        ../src/core/mbox_parser.cpp
        ../src/core/logger.cpp
        ../include/sak/mbox_parser.h
    )
    set_target_properties(test_mbox_parser PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_mbox_parser PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_mbox_parser PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_mbox_parser COMMAND test_mbox_parser
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_mbox_parser")
endif()

# test_email_search_worker ??? criteria, cancel, search hit structure
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_email_search_worker.cpp")
    add_executable(test_email_search_worker
        unit/test_email_search_worker.cpp
        ../src/core/email_search_worker.cpp
        ../src/core/pst_parser.cpp
        ../src/core/mbox_parser.cpp
        ../src/core/logger.cpp
        ../include/sak/email_search_worker.h
        ../include/sak/pst_parser.h
        ../include/sak/mbox_parser.h
    )
    set_target_properties(test_email_search_worker PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_email_search_worker PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_email_search_worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_email_search_worker COMMAND test_email_search_worker
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_email_search_worker")
endif()

# test_email_export_worker ??? config, result, format coverage
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_email_export_worker.cpp")
    add_executable(test_email_export_worker
        unit/test_email_export_worker.cpp
        ../src/core/email_export_worker.cpp
        ../src/core/eml_writer.cpp
        ../src/core/html_email_writer.cpp
        ../src/core/email_attachment_saver.cpp
        ../src/core/pdf_email_writer.cpp
        ../src/core/pst_parser.cpp
        ../src/core/mbox_parser.cpp
        ../src/core/logger.cpp
        ../include/sak/email_export_worker.h
        ../include/sak/eml_writer.h
        ../include/sak/html_email_writer.h
        ../include/sak/pdf_email_writer.h
        ../include/sak/pst_parser.h
        ../include/sak/mbox_parser.h
    )
    set_target_properties(test_email_export_worker PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_email_export_worker PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_email_export_worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_email_export_worker COMMAND test_email_export_worker
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_email_export_worker")
endif()

# test_email_profile_manager ??? discovery, backup/restore, linked files
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_email_profile_manager.cpp")
    add_executable(test_email_profile_manager
        unit/test_email_profile_manager.cpp
        ../src/core/email_profile_manager.cpp
        ${SAK_PROCESS_RUNNER_SOURCES}
        ../src/core/logger.cpp
        ../include/sak/email_profile_manager.h
    )
    set_target_properties(test_email_profile_manager PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_email_profile_manager PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_email_profile_manager PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_email_profile_manager COMMAND test_email_profile_manager
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_email_profile_manager")
endif()

# test_wifi_setup_script --- injection-safe WLAN .cmd builder
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_wifi_setup_script.cpp")
    add_executable(test_wifi_setup_script
        unit/test_wifi_setup_script.cpp
        ../src/core/wifi_setup_script.cpp
        ../src/core/process_runner.cpp
        ../src/core/logger.cpp
        ../include/sak/wifi_setup_script.h
    )
    set_target_properties(test_wifi_setup_script PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_wifi_setup_script PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_wifi_setup_script PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_wifi_setup_script COMMAND test_wifi_setup_script
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_wifi_setup_script")
endif()

# test_deadline_canceller --- header-only deadline/cancel monitor (single-winner finish/timeout race)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_deadline_canceller.cpp")
    add_executable(test_deadline_canceller
        unit/test_deadline_canceller.cpp
        ../include/sak/deadline_canceller.h
    )
    set_target_properties(test_deadline_canceller PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_deadline_canceller PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_deadline_canceller PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_deadline_canceller COMMAND test_deadline_canceller
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_deadline_canceller")
endif()

# test_email_attachment_saver --- short-write + dedupe integrity
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_email_attachment_saver.cpp")
    add_executable(test_email_attachment_saver
        unit/test_email_attachment_saver.cpp
        ../src/core/email_attachment_saver.cpp
        ../src/core/logger.cpp
        ../include/sak/email_attachment_saver.h
    )
    set_target_properties(test_email_attachment_saver PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_email_attachment_saver PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_email_attachment_saver PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_email_attachment_saver COMMAND test_email_attachment_saver
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_email_attachment_saver")
endif()

# test_email_report_generator ??? HTML, JSON, CSV output validation
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_email_report_generator.cpp")
    add_executable(test_email_report_generator
        unit/test_email_report_generator.cpp
        ../src/core/email_report_generator.cpp
        ../src/core/logger.cpp
        ../include/sak/email_report_generator.h
    )
    set_target_properties(test_email_report_generator PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_email_report_generator PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_email_report_generator PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_email_report_generator COMMAND test_email_report_generator
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_email_report_generator")
endif()

# test_offline_package_builder ??? builds the Office PC offline deployment bundle
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_offline_package_builder.cpp")
    add_executable(test_offline_package_builder
        integration/test_offline_package_builder.cpp
        ../src/core/offline_deployment_worker.cpp
        ../src/core/package_internalization_engine.cpp
        ../src/core/install_script_parser.cpp
        ../src/core/script_rewriter.cpp
        ../src/core/nuget_api_client.cpp
        ${SAK_PROCESS_RUNNER_SOURCES}
        ${SAK_NETWORK_TRANSFER_RUNNER_SOURCES}
        ../src/core/logger.cpp
        ../include/sak/offline_deployment_worker.h
        ../include/sak/package_internalization_engine.h
        ../include/sak/install_script_parser.h
        ../include/sak/script_rewriter.h
        ../include/sak/nuget_api_client.h
    )
    set_target_properties(test_offline_package_builder PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_offline_package_builder PRIVATE
        Qt6::Core Qt6::Test Qt6::Network Qt6::Xml Qt6::Concurrent ${PLATFORM_LIBS}
    )
    target_include_directories(test_offline_package_builder PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_offline_package_builder
             COMMAND test_offline_package_builder
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    # This integration test intentionally downloads and repacks real package
    # assets, so the default 300s CTest timeout creates false failures when
    # upstream package/CDN latency spikes.
    set_tests_properties(test_offline_package_builder PROPERTIES TIMEOUT 900)
    message(STATUS "Added integration test: test_offline_package_builder")
endif()

# test_ost_converter_types ??? type defaults, constants, enum coverage
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ost_converter_types.cpp")
    add_executable(test_ost_converter_types
        unit/test_ost_converter_types.cpp
        ../include/sak/ost_converter_types.h
        ../include/sak/ost_converter_constants.h
    )
    set_target_properties(test_ost_converter_types PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ost_converter_types PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_ost_converter_types PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ost_converter_types COMMAND test_ost_converter_types
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ost_converter_types")
endif()

# test_eml_writer ??? RFC 5322 output, filename sanitization, collision handling
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_eml_writer.cpp")
    add_executable(test_eml_writer
        unit/test_eml_writer.cpp
        ../src/core/eml_writer.cpp
        ../src/core/logger.cpp
        ../include/sak/eml_writer.h
        ../include/sak/email_types.h
    )
    set_target_properties(test_eml_writer PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_eml_writer PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_eml_writer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_eml_writer COMMAND test_eml_writer
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_eml_writer")
endif()

# test_ost_converter_controller ??? queue management, duplicate detection
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ost_converter_controller.cpp")
    add_executable(test_ost_converter_controller
        unit/test_ost_converter_controller.cpp
        ../src/core/ost_converter_controller.cpp
        ../src/core/ost_conversion_worker.cpp
        ../src/core/eml_writer.cpp
        ../src/core/pst_writer.cpp
        ../src/core/pst_splitter.cpp
        ../src/core/msg_writer.cpp
        ../src/core/mbox_writer.cpp
        ../src/core/dbx_writer.cpp
        ../src/core/html_email_writer.cpp
        ../src/core/email_attachment_saver.cpp
        ../src/core/pdf_email_writer.cpp
        ../src/core/deleted_item_scanner.cpp
        ../src/core/imap_uploader.cpp
        ../src/core/conversion_report_generator.cpp
        ../src/core/pst_parser.cpp
        ../src/core/logger.cpp
        ../include/sak/ost_converter_controller.h
        ../include/sak/ost_conversion_worker.h
        ../include/sak/eml_writer.h
        ../include/sak/deleted_item_scanner.h
        ../include/sak/imap_uploader.h
        ../include/sak/pst_parser.h
    )
    set_target_properties(test_ost_converter_controller PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_ost_converter_controller PRIVATE
        Qt6::Core Qt6::Gui Qt6::Network Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_ost_converter_controller PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ost_converter_controller COMMAND test_ost_converter_controller
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ost_converter_controller")
endif()

# test_pst_writer ??? PST binary creation, folder hierarchy, magic bytes
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_pst_writer.cpp")
    add_executable(test_pst_writer
        unit/test_pst_writer.cpp
        ../src/core/pst_writer.cpp
        ../src/core/logger.cpp
        ../include/sak/pst_writer.h
        ../include/sak/email_types.h
    )
    set_target_properties(test_pst_writer PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_pst_writer PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_pst_writer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_pst_writer COMMAND test_pst_writer
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_pst_writer")
endif()

# test_pst_splitter ??? volume rotation, single volume, error cases
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_pst_splitter.cpp")
    add_executable(test_pst_splitter
        unit/test_pst_splitter.cpp
        ../src/core/pst_splitter.cpp
        ../src/core/pst_writer.cpp
        ../src/core/logger.cpp
        ../include/sak/pst_splitter.h
        ../include/sak/pst_writer.h
        ../include/sak/email_types.h
    )
    set_target_properties(test_pst_splitter PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_pst_splitter PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_pst_splitter PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_pst_splitter COMMAND test_pst_splitter
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_pst_splitter")
endif()

# test_msg_writer ??? OLE2 compound file, MAPI properties, magic bytes
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_msg_writer.cpp")
    add_executable(test_msg_writer
        unit/test_msg_writer.cpp
        ../src/core/msg_writer.cpp
        ../src/core/logger.cpp
        ../include/sak/msg_writer.h
        ../include/sak/email_types.h
    )
    set_target_properties(test_msg_writer PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_msg_writer PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_msg_writer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_msg_writer COMMAND test_msg_writer
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_msg_writer")
endif()

# test_mbox_writer ??? Unix mbox format, From_ escaping, per-folder
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_mbox_writer.cpp")
    add_executable(test_mbox_writer
        unit/test_mbox_writer.cpp
        ../src/core/mbox_writer.cpp
        ../src/core/eml_writer.cpp
        ../src/core/logger.cpp
        ../include/sak/mbox_writer.h
        ../include/sak/eml_writer.h
        ../include/sak/email_types.h
    )
    set_target_properties(test_mbox_writer PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_mbox_writer PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_mbox_writer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_mbox_writer COMMAND test_mbox_writer
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_mbox_writer")
endif()

# test_dbx_writer ??? Outlook Express DBX, folder magic bytes
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_dbx_writer.cpp")
    add_executable(test_dbx_writer
        unit/test_dbx_writer.cpp
        ../src/core/dbx_writer.cpp
        ../src/core/eml_writer.cpp
        ../src/core/logger.cpp
        ../include/sak/dbx_writer.h
        ../include/sak/eml_writer.h
        ../include/sak/email_types.h
    )
    set_target_properties(test_dbx_writer PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_dbx_writer PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_dbx_writer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_dbx_writer COMMAND test_dbx_writer
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_dbx_writer")
endif()

# test_html_email_writer ??? styled HTML output, data URIs
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_html_email_writer.cpp")
    add_executable(test_html_email_writer
        unit/test_html_email_writer.cpp
        ../src/core/html_email_writer.cpp
        ../src/core/email_attachment_saver.cpp
        ../src/core/logger.cpp
        ../include/sak/html_email_writer.h
        ../include/sak/email_types.h
    )
    set_target_properties(test_html_email_writer PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_html_email_writer PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_html_email_writer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_html_email_writer COMMAND test_html_email_writer
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_html_email_writer")
endif()

# test_pdf_email_writer ??? QPdfWriter output, PDF header
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_pdf_email_writer.cpp")
    add_executable(test_pdf_email_writer
        unit/test_pdf_email_writer.cpp
        ../src/core/pdf_email_writer.cpp
        ../src/core/logger.cpp
        ../include/sak/pdf_email_writer.h
        ../include/sak/email_types.h
    )
    set_target_properties(test_pdf_email_writer PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_pdf_email_writer PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_pdf_email_writer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_pdf_email_writer COMMAND test_pdf_email_writer
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_pdf_email_writer")
endif()

# test_conversion_report_generator ??? HTML report, CSV manifest
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_conversion_report_generator.cpp")
    add_executable(test_conversion_report_generator
        unit/test_conversion_report_generator.cpp
        ../src/core/conversion_report_generator.cpp
        ../src/core/logger.cpp
        ../include/sak/conversion_report_generator.h
        ../include/sak/ost_converter_types.h
        ../include/sak/email_types.h
    )
    set_target_properties(test_conversion_report_generator PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_conversion_report_generator PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_conversion_report_generator PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_conversion_report_generator COMMAND test_conversion_report_generator
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_conversion_report_generator")
endif()

# ============================================================================
# Test Targets
# ============================================================================

# Run all tests
add_custom_target(run_all_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure --verbose
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Running all tests..."
)

# Run unit tests only
add_custom_target(run_unit_tests
    COMMAND ${CMAKE_CTEST_COMMAND} -R "^test_" -E "_workflow|_pipeline" --output-on-failure
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Running unit tests..."
)

# Run integration tests only
add_custom_target(run_integration_tests
    COMMAND ${CMAKE_CTEST_COMMAND} -R "test_(backup|migration)_workflow|test_uup_conversion_pipeline|test_ost_integration|test_offline_package_builder" --output-on-failure
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Running integration tests..."
)

# Generate test report
add_custom_target(test_report
    COMMAND ${CMAKE_CTEST_COMMAND} --output-junit "${TEST_OUTPUT_DIR}/test_results.xml"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Generating test report..."
)

message(STATUS "")
message(STATUS "Test Targets:")
message(STATUS "  run_all_tests - Run all unit and integration tests")
message(STATUS "  run_unit_tests - Run unit tests only")
message(STATUS "  run_integration_tests - Run integration tests only")
message(STATUS "  test_report - Generate JUnit XML test report")
message(STATUS "")
message(STATUS "Test output directory: ${TEST_OUTPUT_DIR}")

# Project-wide convention (see root CMakeLists.txt): the shared core sources are written
# under QT_NO_KEYWORDS - identifiers like `slots` are used as ordinary variable names, so
# any target that compiles those sources directly must define it too. The GUI/app targets
# that use the Qt `signals`/`slots`/`emit` keywords are NOT in this list and keep the
# default. Every test target below compiles src/core/partition_apfs_writer.cpp (and peers)
# directly and uses Q_SLOTS/Q_EMIT, so QT_NO_KEYWORDS is safe for each.
foreach(_sak_core_test_target
        test_partition_manager_core
        partition_filesystem_probe_certifier
        file_management_live_certifier
        test_partition_manager_panel
        test_file_management_explorer_panel
        test_file_management_file_system
        test_duplicate_finder_worker
        test_advanced_search_worker
        test_advanced_search_controller)
    if(TARGET ${_sak_core_test_target})
        target_compile_definitions(${_sak_core_test_target} PRIVATE QT_NO_KEYWORDS)
    endif()
endforeach()

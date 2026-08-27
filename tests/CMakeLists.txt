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
# which would put tests in build/Release/Release/ - away from the DLLs that
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
                 -o "${TEST_OUTPUT_DIR}/${test_name}.txt,txt" -o -,txt
                 WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")

        message(STATUS "Added unit test: ${test_name}")
    endif()
endforeach()

# ============================================================================
# Unit Tests - Diagnostic & Benchmarking Panel
# ============================================================================

# test_worker_base - WorkerBase lifecycle, exception safety, cancellation
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
         -o "${TEST_OUTPUT_DIR}/test_worker_base.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_worker_base")

# test_error_codes - to_string completeness
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
         -o "${TEST_OUTPUT_DIR}/test_error_codes.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_error_codes")

# test_ai_model_catalog - restricted OpenAI model catalog (GPT-5.6 only)
add_executable(test_ai_model_catalog
    unit/test_ai_model_catalog.cpp
)
set_target_properties(test_ai_model_catalog PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_model_catalog PRIVATE
    Qt6::Core Qt6::Test
)
target_include_directories(test_ai_model_catalog PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_ai_model_catalog COMMAND test_ai_model_catalog
         -o "${TEST_OUTPUT_DIR}/test_ai_model_catalog.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_model_catalog")

# test_restore_point_manager - System Restore enablement (B5-10 fail-closed)
add_executable(test_restore_point_manager
    unit/test_restore_point_manager.cpp
    ../src/core/restore_point_manager.cpp
    ../src/core/elevation_manager.cpp
    ../src/core/process_runner.cpp
    ../src/core/logger.cpp
    ../include/sak/restore_point_manager.h
)
set_target_properties(test_restore_point_manager PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_restore_point_manager PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_restore_point_manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_restore_point_manager COMMAND test_restore_point_manager
         -o "${TEST_OUTPUT_DIR}/test_restore_point_manager.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_restore_point_manager")

# test_diagnostic_types - struct defaults and value semantics
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
         -o "${TEST_OUTPUT_DIR}/test_diagnostic_types.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_diagnostic_types")

# test_partition_manager_core - inventory parsing, safety, scripts, queue
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
    ../src/core/partition_executor.cpp
    ../src/core/partition_operation_planner.cpp
    ../src/core/partition_operation_queue.cpp
    ${SAK_PROCESS_RUNNER_SOURCES}
    ../src/core/logger.cpp
    ../include/sak/partition_executor.h
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
         -o "${TEST_OUTPUT_DIR}/test_partition_manager_core.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_partition_manager_core")

# partition_file_recovery_certifier - external lab helper for live-volume Data Recovery proof
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

# partition_filesystem_probe_certifier - external lab helper for raw filesystem metadata proof
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
if(WIN32)
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

# file_management_live_certifier - live APFS/HFS File Management bridge proof
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

# flash_live_certifier - LIVE, DESTRUCTIVE raw-disk cert of the Batch 4 FlashWorker
# memory-safety fixes. Built only on Windows; run manually + elevated against a
# disposable removable USB (it hard-refuses non-USB / system / oversized disks).
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/certification/flash_live_certifier.cpp")
    add_executable(flash_live_certifier
        certification/flash_live_certifier.cpp
        ../src/threading/flash_worker.cpp
        ../src/core/drive_scanner.cpp
        ../include/sak/drive_scanner.h
        ../src/threading/worker_base.cpp
        ../src/core/image_source.cpp
        ../src/core/decompressor_factory.cpp
        ../src/core/streaming_decompressor.cpp
        ../src/core/gzip_decompressor.cpp
        ../src/core/bzip2_decompressor.cpp
        ../src/core/xz_decompressor.cpp
        ../src/core/file_hash.cpp
        ../src/core/keep_awake.cpp
        ../src/core/logger.cpp
        ../include/sak/flash_worker.h
        ../include/sak/image_source.h
        ../include/sak/worker_base.h
        ../include/sak/streaming_decompressor.h
        ../include/sak/decompressor_factory.h
        ../include/sak/gzip_decompressor.h
        ../include/sak/bzip2_decompressor.h
        ../include/sak/xz_decompressor.h
    )
    set_target_properties(flash_live_certifier PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(flash_live_certifier PRIVATE
        Qt6::Core Qt6::Concurrent ZLIB::ZLIB BZip2::BZip2 LibLZMA::LibLZMA sak_lzfse ${PLATFORM_LIBS}
    )
    target_include_directories(flash_live_certifier PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    message(STATUS "Added certification helper: flash_live_certifier")
endif()

if(TARGET file_management_live_certifier AND WIN32)
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

if(TARGET sak_apfs_writer_cli AND WIN32)
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

if(TARGET sak_hfs_writer_cli AND WIN32)
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
add_executable(test_partition_manager_panel
    unit/test_partition_manager_panel.cpp
    ../src/gui/partition_manager_panel.cpp
    ../src/gui/view_empty_state.cpp
    ../include/sak/view_empty_state.h
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
         -o "${TEST_OUTPUT_DIR}/test_partition_manager_panel.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_partition_manager_panel")

# test_wifi_manager_panel --- SSID injection safety in buildWindowsScript
add_executable(test_wifi_manager_panel
    unit/test_wifi_manager_panel.cpp
    ../src/gui/wifi_manager_panel.cpp
    ../src/gui/detachable_log_window.cpp
    ../src/gui/follow_scroll_controller.cpp
    ../src/third_party/qrcodegen/qrcodegen.cpp
    ${SAK_PROCESS_RUNNER_SOURCES}
    ../src/core/logger.cpp
    ../src/core/wifi_setup_script.cpp
    # The panel now scans through the shared headless scanner instead of its own netsh parse,
    # so the target links the real thing rather than a second copy of the same logic.
    ../src/core/wifi_profile_scanner.cpp
    ../src/core/user_profile_types.cpp
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
         -o "${TEST_OUTPUT_DIR}/test_wifi_manager_panel.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_wifi_manager_panel")

# test_file_explorer_breadcrumb --- path segmentation incl. UNC targets
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
         -o "${TEST_OUTPUT_DIR}/test_file_explorer_breadcrumb.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_file_explorer_breadcrumb")

# test_file_explorer_properties_calc --- size-walk completeness + hash labeling
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
         -o "${TEST_OUTPUT_DIR}/test_file_explorer_properties_calc.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_file_explorer_properties_calc")

# test_iso_analyzer --- Windows-vs-Linux install-media classification (P12-09)
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
         -o "${TEST_OUTPUT_DIR}/test_iso_analyzer.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_iso_analyzer")

# test_user_profile_restore_selection --- folder selection -> manifest (P12-25)
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
         -o "${TEST_OUTPUT_DIR}/test_user_profile_restore_selection.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_user_profile_restore_selection")

# test_diagnostic_controller - suite state machine, cancellation, error handling
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
         -o "${TEST_OUTPUT_DIR}/test_diagnostic_controller.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_diagnostic_controller")

# test_hardware_inventory_scanner --- WMI parsing + volume-to-disk mapping
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
         -o "${TEST_OUTPUT_DIR}/test_hardware_inventory_scanner.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_hardware_inventory_scanner")

# test_thermal_monitor - timer behavior and polling
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
         -o "${TEST_OUTPUT_DIR}/test_thermal_monitor.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_thermal_monitor")

# test_diagnostic_report_generator - HTML, JSON, CSV report output
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
         -o "${TEST_OUTPUT_DIR}/test_diagnostic_report_generator.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_diagnostic_report_generator")

# test_cpu_benchmark_worker - construction, result defaults, signal types
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
         -o "${TEST_OUTPUT_DIR}/test_cpu_benchmark_worker.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_cpu_benchmark_worker")

# test_disk_benchmark_worker - construction, config, result defaults
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
         -o "${TEST_OUTPUT_DIR}/test_disk_benchmark_worker.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_disk_benchmark_worker")

# test_memory_benchmark_worker - construction, result defaults
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
         -o "${TEST_OUTPUT_DIR}/test_memory_benchmark_worker.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_memory_benchmark_worker")

# test_stress_test_worker - construction, config, result defaults
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
         -o "${TEST_OUTPUT_DIR}/test_stress_test_worker.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_stress_test_worker")

# ============================================================================
# Unit Tests - Phase 4: Comprehensive Coverage
# ============================================================================

# --- Tier 1: Security & Data Integrity ---

# test_encryption - AES-256-CBC round-trip, wrong password, corrupted ciphertext
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
         -o "${TEST_OUTPUT_DIR}/test_encryption.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_encryption")

# test_backup_file_codec - streaming per-file compression/encryption container
add_executable(test_backup_file_codec
    unit/test_backup_file_codec.cpp
    ../src/core/backup_file_codec.cpp
    ../src/core/encryption.cpp
    ../src/core/secure_memory.cpp
    ../src/core/logger.cpp
)
set_target_properties(test_backup_file_codec PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_backup_file_codec PRIVATE
    Qt6::Core Qt6::Test ZLIB::ZLIB ${PLATFORM_LIBS}
)
target_include_directories(test_backup_file_codec PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_backup_file_codec COMMAND test_backup_file_codec
         -o "${TEST_OUTPUT_DIR}/test_backup_file_codec.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_backup_file_codec")

# test_file_hash - MD5/SHA256, verify correct/incorrect, progress, cancellation
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
         -o "${TEST_OUTPUT_DIR}/test_file_hash.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_file_hash")

# test_path_utils - safe path, traversal, pattern matching, directory size
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
         -o "${TEST_OUTPUT_DIR}/test_path_utils.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_path_utils")

# test_secure_memory - wipe, secure_buffer, secure_allocator, secure random
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
         -o "${TEST_OUTPUT_DIR}/test_secure_memory.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_secure_memory")

# test_input_validator - path/string/numeric validation, safe arithmetic, buffer validation
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
         -o "${TEST_OUTPUT_DIR}/test_input_validator.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_input_validator")

# --- Tier 2: Core Business Logic ---

# test_decompressor_factory - format detection, isCompressed, factory create
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
         -o "${TEST_OUTPUT_DIR}/test_decompressor_factory.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_decompressor_factory")

# test_config_manager - singleton, get/set, defaults, signals, typed accessors
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
         -o "${TEST_OUTPUT_DIR}/test_config_manager.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_config_manager")

# test_config_schema_versioning - R5-G23-5: persisted schema_version; current
# round-trip, older-version migrate (no data loss), newer-version preserve+unhealthy
add_executable(test_config_schema_versioning
    unit/test_config_schema_versioning.cpp
    ../src/core/app_paths.cpp
    ../src/core/config_manager.cpp
    ../src/core/logger.cpp
    ../include/sak/app_paths.h
    ../include/sak/config_manager.h
)
set_target_properties(test_config_schema_versioning PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_config_schema_versioning PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_config_schema_versioning PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_config_schema_versioning COMMAND test_config_schema_versioning
         -o "${TEST_OUTPUT_DIR}/test_config_schema_versioning.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_config_schema_versioning")

# test_smart_file_filter - dangerous files, size limits, pattern/folder exclusion
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
         -o "${TEST_OUTPUT_DIR}/test_smart_file_filter.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_smart_file_filter")

# test_recycle_bin -- B8-23 guard: empty/wildcard/NUL paths refused before the
# SHFileOperationW shell call (wildcards would expand into a mass recycle).
add_executable(test_recycle_bin
    unit/test_recycle_bin.cpp
    ../src/core/recycle_bin.cpp
)
set_target_properties(test_recycle_bin PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_recycle_bin PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_recycle_bin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_recycle_bin COMMAND test_recycle_bin
         -o "${TEST_OUTPUT_DIR}/test_recycle_bin.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_recycle_bin")

# test_firewall_rule_auditor -- B9-11 pure conflict/port-overlap seams plus the
# live-firewall enumeration integration checks.
add_executable(test_firewall_rule_auditor
    unit/test_firewall_rule_auditor.cpp
    ../src/core/firewall_rule_auditor.cpp
    ../include/sak/firewall_rule_auditor.h  # Q_OBJECT -> AUTOMOC
)
set_target_properties(test_firewall_rule_auditor PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_firewall_rule_auditor PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_firewall_rule_auditor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_firewall_rule_auditor COMMAND test_firewall_rule_auditor
         -o "${TEST_OUTPUT_DIR}/test_firewall_rule_auditor.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_firewall_rule_auditor")

# test_ethernet_config_manager -- snapshot serialization + B9-12 atomic saveToFile
# round-trip. (The netsh-driven restore/DHCP path is not unit-tested.)
add_executable(test_ethernet_config_manager
    unit/test_ethernet_config_manager.cpp
    ../src/core/ethernet_config_manager.cpp
    ../include/sak/ethernet_config_manager.h  # Q_OBJECT -> AUTOMOC
    ../src/core/process_runner.cpp
    # EthernetConfigInfo::parseNetIpConfigJson -- the language-neutral adapter scan the manager
    # now shares with the backup wizard instead of scraping localized netsh labels.
    ../src/core/user_profile_types.cpp
)
set_target_properties(test_ethernet_config_manager PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ethernet_config_manager PRIVATE
    Qt6::Core Qt6::Network Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_ethernet_config_manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_ethernet_config_manager COMMAND test_ethernet_config_manager
         -o "${TEST_OUTPUT_DIR}/test_ethernet_config_manager.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ethernet_config_manager")

# test_file_scanner - scan, filters, cancellation, static convenience methods
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
         -o "${TEST_OUTPUT_DIR}/test_file_scanner.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_file_scanner")

# test_file_explorer_types - File Explorer state and command registry
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
         -o "${TEST_OUTPUT_DIR}/test_file_explorer_types.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_file_explorer_types")

# test_file_explorer_session_store - tab session serialization round trip
add_executable(test_file_explorer_session_store
    unit/test_file_explorer_session_store.cpp
    ../src/core/file_explorer_types.cpp
    ../src/core/file_explorer_layout_metrics.cpp
    ../src/core/file_explorer_session_store.cpp
    # The session store now surfaces its own failures instead of swallowing them, which pulls
    # in the logger; carry it the way the helper-built unit tests do.
    ../src/core/logger.cpp
    ../include/sak/logger.h
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
         -o "${TEST_OUTPUT_DIR}/test_file_explorer_session_store.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_file_explorer_session_store")

# test_file_explorer_tag_store - app-level item tag persistence
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
         -o "${TEST_OUTPUT_DIR}/test_file_explorer_tag_store.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_file_explorer_tag_store")

# test_file_explorer_item_model - File Explorer model/view rows, roles, and sorting
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
         -o "${TEST_OUTPUT_DIR}/test_file_explorer_item_model.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_file_explorer_item_model")

# test_file_management_explorer_panel - File Explorer shell and command-menu UI
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
    ../src/core/file_explorer_properties_calc.cpp
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
         -o "${TEST_OUTPUT_DIR}/test_file_management_explorer_panel.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
# GUI suite runs on the offscreen platform: no windows appear on the
# developer desktop and stray focus changes cannot disturb the tests.
set_tests_properties(test_file_management_explorer_panel PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
message(STATUS "Added unit test: test_file_management_explorer_panel")

# test_file_management_file_system - File Management target bridge
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
         -o "${TEST_OUTPUT_DIR}/test_file_management_file_system.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_file_management_file_system")

# test_process_runner - echo, exit code, stderr, timeout, cancellation, PowerShell
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
         -o "${TEST_OUTPUT_DIR}/test_process_runner.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_process_runner")

# test_resource_soak - R5-G23-10 long-session handle/GDI/memory-growth soak
add_executable(test_resource_soak
    unit/test_resource_soak.cpp
    ../src/core/process_runner.cpp
    ../src/core/logger.cpp
)
set_target_properties(test_resource_soak PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_resource_soak PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_resource_soak PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_resource_soak COMMAND test_resource_soak
         -o "${TEST_OUTPUT_DIR}/test_resource_soak.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_resource_soak")

# test_smart_disk_analyzer - SmartReport parsing, health assessment
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
         -o "${TEST_OUTPUT_DIR}/test_ai_token_usage_tracker.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_token_usage_tracker")

add_executable(test_openai_responses_client
    unit/test_openai_responses_client.cpp
    ../src/ai/openai_responses_client.cpp
    ../src/core/app_paths.cpp
    ../src/ai/ai_token_usage_tracker.cpp
    ../src/ai/ai_credential_store.cpp
    ../src/ai/ai_secret_redaction.cpp
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
         -o "${TEST_OUTPUT_DIR}/test_openai_responses_client.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_openai_responses_client")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_provider_registry.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_provider_registry")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_prompt_assembler.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_prompt_assembler")

if(SAK_ENABLE_AI_ASSISTANT)
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
        ${CMAKE_SOURCE_DIR}/src/core/nuget_dependency_resolver.cpp
        ${CMAKE_SOURCE_DIR}/src/core/nuget_version_range.cpp
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
        # network.enable_adapter / disable_adapter / rename_adapter: same shape -- the netsh
        # EXECUTION needs admin and would change the host's network, so it is not unit-tested
        # here, but the ops link against the pure seam (name validation, resolution, argument
        # vectors) which test_network_adapter_admin covers directly.
        ${CMAKE_SOURCE_DIR}/src/core/network_adapter_admin.cpp
        # EthernetConfigManager now maps its capture through
        # EthernetConfigInfo::parseNetIpConfigJson (language-neutral, replacing the
        # localized netsh scrape), so its translation unit is required here too.
        ${CMAKE_SOURCE_DIR}/src/core/user_profile_types.cpp
        # files.compress_zip / files.extract_zip: FileExplorerArchiveService (static, synchronous,
        # QZipReader/QZipWriter via Qt6::CorePrivate -- already linked). zip-slip/bomb hardening is
        # in the engine; the op-layer path guards + new/empty-dir + no-clobber are covered by tests.
        ${CMAKE_SOURCE_DIR}/src/core/file_explorer_archive_service.cpp
        ${CMAKE_SOURCE_DIR}/include/sak/file_explorer_archive_service.h
        ${CMAKE_SOURCE_DIR}/src/core/wifi_setup_script.cpp
        ${CMAKE_SOURCE_DIR}/src/core/ost_conversion_worker.cpp
        ${CMAKE_SOURCE_DIR}/include/sak/ost_conversion_worker.h
        ${CMAKE_SOURCE_DIR}/src/core/mbox_writer.cpp
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
    # No /NODEFAULTLIB CRT suppression here either; see the note in the root
    # CMakeLists. The CRT coherence check makes the conflict impossible to
    # configure rather than something to silence per target.
    target_include_directories(test_ai_assistant_panel_tool_dispatch PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/third_party/qrcodegen
    )
    add_test(NAME test_ai_assistant_panel_tool_dispatch
             COMMAND test_ai_assistant_panel_tool_dispatch
             -o "${TEST_OUTPUT_DIR}/test_ai_assistant_panel_tool_dispatch.txt,txt" -o -,txt
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_ai_assistant_panel_tool_dispatch")
endif()

add_executable(test_ai_async_tool_runner
    unit/test_ai_async_tool_runner.cpp
    ../src/ai/ai_async_tool_runner.cpp
    ../include/sak/ai/ai_async_tool_runner.h
    # The runner logs the exception it swallows so a thrown-away tool failure is not
    # silent; that pulls in the logger.
    ../src/core/logger.cpp
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
         -o "${TEST_OUTPUT_DIR}/test_ai_async_tool_runner.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_async_tool_runner")

add_executable(test_ai_tool_health_ledger
    unit/test_ai_tool_health_ledger.cpp
    ../src/ai/ai_tool_health_ledger.cpp
    ../include/sak/ai/ai_tool_health_ledger.h
    # The ledger now reports its own failures instead of swallowing them, which pulls in the
    # logger; carry it the same way the helper-built unit tests do.
    ../src/core/logger.cpp
    ../include/sak/logger.h
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
         -o "${TEST_OUTPUT_DIR}/test_ai_tool_health_ledger.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_tool_health_ledger")

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
         -o "${TEST_OUTPUT_DIR}/test_follow_scroll_controller.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_follow_scroll_controller")

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
         -o "${TEST_OUTPUT_DIR}/test_splash_screen.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_splash_screen")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_transcript_view.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_transcript_view")

add_executable(test_view_empty_state
    unit/test_view_empty_state.cpp
    ../src/gui/view_empty_state.cpp
    ../include/sak/view_empty_state.h
)
set_target_properties(test_view_empty_state PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_view_empty_state PRIVATE
    Qt6::Core Qt6::Widgets Qt6::Test
)
target_include_directories(test_view_empty_state PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_view_empty_state COMMAND test_view_empty_state
         -o "${TEST_OUTPUT_DIR}/test_view_empty_state.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_view_empty_state")

add_executable(test_crash_reporter
    unit/test_crash_reporter.cpp
    ../src/core/crash_reporter.cpp
    ../include/sak/crash_reporter.h
)
set_target_properties(test_crash_reporter PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_crash_reporter PRIVATE
    Qt6::Core Qt6::Test
)
if(WIN32)
    target_link_libraries(test_crash_reporter PRIVATE dbghelp)
endif()
target_include_directories(test_crash_reporter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_crash_reporter COMMAND test_crash_reporter
         -o "${TEST_OUTPUT_DIR}/test_crash_reporter.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_crash_reporter")

# test_fuzz_email_sanitizer - deterministic mutation-fuzz of the untrusted-email
# HTML sanitizer (G14-6/G14-12). Header-only sanitizer + reusable fuzz core; no
# extra link deps beyond QtCore/QtTest.
add_executable(test_fuzz_email_sanitizer
    unit/test_fuzz_email_sanitizer.cpp
    ../include/sak/email_html_sanitizer.h
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_email_sanitizer PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_email_sanitizer PRIVATE
    Qt6::Core Qt6::Test
)
target_include_directories(test_fuzz_email_sanitizer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_fuzz_email_sanitizer COMMAND test_fuzz_email_sanitizer
         -o "${TEST_OUTPUT_DIR}/test_fuzz_email_sanitizer.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_fuzz_email_sanitizer")

# test_fuzz_pst_parser - deterministic mutation-fuzz of the PST/OST binary parser
# through the real open() path (G14-5). Mirrors test_pst_parser's link set.
add_executable(test_fuzz_pst_parser
    unit/test_fuzz_pst_parser.cpp
    ../src/core/pst_parser.cpp
    ../src/core/logger.cpp
    ../include/sak/pst_parser.h
    fuzz/fuzz_harness.h
    support/pst_fixture.h
)
set_target_properties(test_fuzz_pst_parser PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_pst_parser PRIVATE
    Qt6::Core Qt6::Gui Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_fuzz_pst_parser PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_fuzz_pst_parser COMMAND test_fuzz_pst_parser
         -o "${TEST_OUTPUT_DIR}/test_fuzz_pst_parser.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_fuzz_pst_parser")

# test_fuzz_pst_structure - structure-aware mutation-fuzz of the PST LTP accept path
# (G14-5 depth): mutates one region body of the openable store and re-stamps its trailer
# so the file stays integral, driving the deep BTree/heap/property code the plain fuzz
# (which breaks a CRC and rejects at the first gate) cannot reach. Same link set.
add_executable(test_fuzz_pst_structure
    unit/test_fuzz_pst_structure.cpp
    ../src/core/pst_parser.cpp
    ../src/core/logger.cpp
    ../include/sak/pst_parser.h
    fuzz/fuzz_harness.h
    support/pst_fixture.h
)
set_target_properties(test_fuzz_pst_structure PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_pst_structure PRIVATE
    Qt6::Core Qt6::Gui Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_fuzz_pst_structure PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_fuzz_pst_structure COMMAND test_fuzz_pst_structure
         -o "${TEST_OUTPUT_DIR}/test_fuzz_pst_structure.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_fuzz_pst_structure")

# test_fuzz_decompressor - deterministic mutation-fuzz of the streaming archive decompressors
# (gzip / bzip2 / xz) through the real DecompressorFactory -> open() -> read() pipeline (G14-10).
# Asserts no crash/hang, that the decompression-bomb cap holds, and that terminal failure sticks.
# Mirrors test_streaming_decompressor's link set.
add_executable(test_fuzz_decompressor
    unit/test_fuzz_decompressor.cpp
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
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_decompressor PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_decompressor PRIVATE
    Qt6::Core Qt6::Test ZLIB::ZLIB BZip2::BZip2 LibLZMA::LibLZMA ${PLATFORM_LIBS}
)
target_include_directories(test_fuzz_decompressor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_fuzz_decompressor COMMAND test_fuzz_decompressor
         -o "${TEST_OUTPUT_DIR}/test_fuzz_decompressor.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_fuzz_decompressor")

# test_fuzz_mcp_framing - deterministic mutation-fuzz of the two byte-framed control-
# bridge transports (G14-12): Chrome native-messaging frames (parseFrame, needs
# native_messaging.cpp) and the MCP stdio JSON-RPC line (parseJsonLine, header-only).
add_executable(test_fuzz_mcp_framing
    unit/test_fuzz_mcp_framing.cpp
    ../src/win32mcp/native_messaging.cpp
    ../include/sak/win32mcp/native_messaging.h
    ../include/sak/ai/ai_mcp_jsonrpc.h
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_mcp_framing PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_mcp_framing PRIVATE
    Qt6::Core Qt6::Test
)
target_include_directories(test_fuzz_mcp_framing PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_fuzz_mcp_framing COMMAND test_fuzz_mcp_framing
         -o "${TEST_OUTPUT_DIR}/test_fuzz_mcp_framing.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_fuzz_mcp_framing")

# test_fuzz_command_classifier - property-fuzz of the AI command risk classifier
# (G14-18): a catastrophic command stays catastrophic under every shell-transparent
# obfuscation (intra-word carets/backticks, exe suffixes, case), and a benign command
# never becomes catastrophic. Guards the "fo^rmat C:" shipped-bug class.
add_executable(test_fuzz_command_classifier
    unit/test_fuzz_command_classifier.cpp
    ../src/ai/ai_tool_policy.cpp
    ../include/sak/ai/ai_tool_policy.h
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_command_classifier PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_command_classifier PRIVATE
    Qt6::Core Qt6::Test
)
target_include_directories(test_fuzz_command_classifier PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_fuzz_command_classifier COMMAND test_fuzz_command_classifier
         -o "${TEST_OUTPUT_DIR}/test_fuzz_command_classifier.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_fuzz_command_classifier")

# test_fuzz_mbox_headers - deterministic mutation-fuzz of the RFC 5322 header parser
# (G14-6). Header-only pure seam (sak/mbox_header_parser.h), no extra link deps.
add_executable(test_fuzz_mbox_headers
    unit/test_fuzz_mbox_headers.cpp
    ../include/sak/mbox_header_parser.h
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_mbox_headers PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_mbox_headers PRIVATE
    Qt6::Core Qt6::Test
)
target_include_directories(test_fuzz_mbox_headers PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_fuzz_mbox_headers COMMAND test_fuzz_mbox_headers
         -o "${TEST_OUTPUT_DIR}/test_fuzz_mbox_headers.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_fuzz_mbox_headers")

# test_fuzz_mbox_transfer_decoder - deterministic mutation-fuzz of the MIME
# Content-Transfer-Encoding decoders (G14-6): strict base64 (fail-closed) and lenient
# quoted-printable. Header-only pure seam (sak/mbox_transfer_decoder.h), no extra link.
add_executable(test_fuzz_mbox_transfer_decoder
    unit/test_fuzz_mbox_transfer_decoder.cpp
    ../include/sak/mbox_transfer_decoder.h
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_mbox_transfer_decoder PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_mbox_transfer_decoder PRIVATE
    Qt6::Core Qt6::Test
)
target_include_directories(test_fuzz_mbox_transfer_decoder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_fuzz_mbox_transfer_decoder COMMAND test_fuzz_mbox_transfer_decoder
         -o "${TEST_OUTPUT_DIR}/test_fuzz_mbox_transfer_decoder.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_fuzz_mbox_transfer_decoder")

# test_fuzz_iso_analyzer - deterministic mutation-fuzz of the ISO 9660 / El Torito / UDF
# volume-descriptor parser via the IsoAnalyzer::analyzeDevice byte-in seam (G14).
add_executable(test_fuzz_iso_analyzer
    unit/test_fuzz_iso_analyzer.cpp
    ../src/core/iso_analyzer.cpp
    ../src/core/logger.cpp
    ../include/sak/iso_analyzer.h
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_iso_analyzer PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_iso_analyzer PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_fuzz_iso_analyzer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_fuzz_iso_analyzer COMMAND test_fuzz_iso_analyzer
         -o "${TEST_OUTPUT_DIR}/test_fuzz_iso_analyzer.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_fuzz_iso_analyzer")

# test_fuzz_ext_reader - deterministic mutation-fuzz of the read-only ext2/3/4 file browser
# (G14-9) through the real listDirectoryFromImage() / readFileFromImage() entry points. Asserts
# no crash/hang, fail-closed-with-a-blocker, bounded listings, and bounded reads. Reuses the
# shared ext image builder in tests/support/ext_fixture.h.
add_executable(test_fuzz_ext_reader
    unit/test_fuzz_ext_reader.cpp
    ../src/core/partition_ext_file_system_reader.cpp
    ../src/core/partition_raw_device_io.cpp
    ../include/sak/partition_ext_file_system_reader.h
    ../include/sak/partition_raw_device_io.h
    ../include/sak/partition_export_containment.h
    support/byte_writer.h
    support/ext_fixture.h
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_ext_reader PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_ext_reader PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_fuzz_ext_reader PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_fuzz_ext_reader COMMAND test_fuzz_ext_reader
         -o "${TEST_OUTPUT_DIR}/test_fuzz_ext_reader.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_fuzz_ext_reader")

# test_fuzz_fs_detector - deterministic mutation-fuzz of the raw file-system signature detector,
# the untrusted-bytes gate that runs before any reader (FAT/exFAT/NTFS/ext/HFS+/APFS/...). Asserts
# no crash/hang across the three call shapes, determinism (pure function), and non-empty family.
add_executable(test_fuzz_fs_detector
    unit/test_fuzz_fs_detector.cpp
    ../src/core/partition_file_system_detector.cpp
    ../src/core/partition_raw_device_io.cpp
    ../include/sak/partition_file_system_detector.h
    ../include/sak/partition_raw_device_io.h
    support/byte_writer.h
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_fs_detector PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_fs_detector PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_fuzz_fs_detector PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_fuzz_fs_detector COMMAND test_fuzz_fs_detector
         -o "${TEST_OUTPUT_DIR}/test_fuzz_fs_detector.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_fuzz_fs_detector")

# test_fuzz_install_script - deterministic mutation-fuzz of the Chocolatey install-script parser.
# The script text comes from third-party packages and the parser is regex-heavy, so this asserts
# no crash/hang (ReDoS), determinism, and non-negative resource line numbers over mutated scripts.
add_executable(test_fuzz_install_script
    unit/test_fuzz_install_script.cpp
    ../src/core/install_script_parser.cpp
    ../src/core/logger.cpp
    ../include/sak/install_script_parser.h
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_install_script PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_install_script PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_fuzz_install_script PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_fuzz_install_script COMMAND test_fuzz_install_script
         -o "${TEST_OUTPUT_DIR}/test_fuzz_install_script.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_fuzz_install_script")

# test_fuzz_hfs_reader - deterministic mutation-fuzz of the read-only HFS+ file browser (G14-8)
# through the real listDirectoryFromImage() / readFileFromImage() entry points. Asserts no
# crash/hang, fail-closed-with-a-blocker, bounded listings, and bounded reads. Reuses the shared
# HFS image builder in tests/support/hfs_fixture.h.
add_executable(test_fuzz_hfs_reader
    unit/test_fuzz_hfs_reader.cpp
    ../src/core/partition_hfs_file_system_reader.cpp
    ../src/core/partition_raw_device_io.cpp
    ../include/sak/partition_hfs_file_system_reader.h
    ../include/sak/partition_raw_device_io.h
    ../include/sak/partition_export_containment.h
    ../include/sak/partition_hfs_internal.h
    support/byte_writer.h
    support/hfs_fixture.h
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_hfs_reader PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_hfs_reader PRIVATE
    Qt6::Core Qt6::Test sak_lzfse ${PLATFORM_LIBS}
)
target_include_directories(test_fuzz_hfs_reader PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_fuzz_hfs_reader COMMAND test_fuzz_hfs_reader
         -o "${TEST_OUTPUT_DIR}/test_fuzz_hfs_reader.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_fuzz_hfs_reader")

# test_fuzz_apfs_reader - deterministic mutation-fuzz of the read-only APFS container reader
# (G14-7). An APFS container has a 64 MiB floor, so instead of whole-buffer mutation the harness
# builds one genuine walkable container with the real writer and overlays a mutated front window
# (nx_superblock + checkpoint + object map) onto its head each iteration. Asserts no crash/hang,
# fail-closed-with-a-blocker, bounded listings, and bounded reads.
add_executable(test_fuzz_apfs_reader
    unit/test_fuzz_apfs_reader.cpp
    ../src/core/partition_apfs_file_system_reader.cpp
    ../src/core/partition_apfs_writer.cpp
    ../src/core/apfs_crypto.cpp
    ../src/core/apfs_keybag.cpp
    ../src/core/partition_file_system_detector.cpp
    ../src/core/partition_raw_device_io.cpp
    ../include/sak/partition_apfs_file_system_reader.h
    ../include/sak/partition_apfs_writer.h
    ../include/sak/partition_raw_device_io.h
    ../include/sak/partition_export_containment.h
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_apfs_reader PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_apfs_reader PRIVATE
    Qt6::Core Qt6::Test sak_lzfse ${PLATFORM_LIBS}
)
target_include_directories(test_fuzz_apfs_reader PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_fuzz_apfs_reader COMMAND test_fuzz_apfs_reader
         -o "${TEST_OUTPUT_DIR}/test_fuzz_apfs_reader.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_fuzz_apfs_reader")

# test_fuzz_nuget_version_range - deterministic mutation-fuzz of the NuGet version / version-range
# parsers, which consume version strings from third-party package metadata. Asserts no crash/hang,
# determinism, the fail-closed contract (an invalid range satisfies nothing), and that
# selectHighestSatisfying only returns a satisfying version.
add_executable(test_fuzz_nuget_version_range
    unit/test_fuzz_nuget_version_range.cpp
    ../src/core/nuget_version_range.cpp
    ../include/sak/nuget_version_range.h
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_nuget_version_range PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_nuget_version_range PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_fuzz_nuget_version_range PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_fuzz_nuget_version_range COMMAND test_fuzz_nuget_version_range
         -o "${TEST_OUTPUT_DIR}/test_fuzz_nuget_version_range.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_fuzz_nuget_version_range")

# test_fuzz_backup_codec - deterministic mutation-fuzz of the backup-file container decoder. The
# container carries a magic header, a zlib layer, and an AES-GCM layer; the decoder is fail-closed
# (staged plaintext is renamed into place only after the tag verifies). Asserts no crash/hang, that
# the metadata classify never crashes, that a rejected container leaves no destination file, and
# determinism. Mirrors test_backup_file_codec's link set.
add_executable(test_fuzz_backup_codec
    unit/test_fuzz_backup_codec.cpp
    ../src/core/backup_file_codec.cpp
    ../src/core/encryption.cpp
    ../src/core/secure_memory.cpp
    ../src/core/logger.cpp
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_backup_codec PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_backup_codec PRIVATE
    Qt6::Core Qt6::Test ZLIB::ZLIB ${PLATFORM_LIBS}
)
target_include_directories(test_fuzz_backup_codec PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_fuzz_backup_codec COMMAND test_fuzz_backup_codec
         -o "${TEST_OUTPUT_DIR}/test_fuzz_backup_codec.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_fuzz_backup_codec")

# test_fuzz_uup_manifest_guard - deterministic mutation-fuzz of the UUP-dump aria2 manifest-entry
# guard. The UUP dump JSON API is a third-party service; its fileName/url/sha1 fields are serialized
# into an aria2 input manifest, so isSafeAria2FileEntry()/isValidSha1() are the injection boundary.
# Asserts no crash/hang, determinism, that the guard agrees with an independent oracle, and above all
# that an oracle-unsafe field is NEVER accepted. Mirrors test_uup_dump_api's link set.
add_executable(test_fuzz_uup_manifest_guard
    unit/test_fuzz_uup_manifest_guard.cpp
    ../src/core/uup_dump_api.cpp
    ../include/sak/uup_dump_api.h
    ../src/core/logger.cpp
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_uup_manifest_guard PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_uup_manifest_guard PRIVATE
    Qt6::Core Qt6::Test Qt6::Network ${PLATFORM_LIBS}
)
target_include_directories(test_fuzz_uup_manifest_guard PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_fuzz_uup_manifest_guard COMMAND test_fuzz_uup_manifest_guard
         -o "${TEST_OUTPUT_DIR}/test_fuzz_uup_manifest_guard.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_fuzz_uup_manifest_guard")

# test_fuzz_smart_report - deterministic mutation-fuzz of the smartctl JSON health-report parser.
# smartctl output for a failing/corrupt drive is untrusted input; the parser's central contract is
# fail-closed (a data-less payload must resolve to Unknown health, never read as a clean drive).
# parseAndAssessForTesting() runs the whole parse->assess->recommend pipeline over raw bytes. Asserts
# no crash/hang, determinism, the Unknown-IFF-no-signal equivalence, that FAILED is always Critical,
# and that a recommendation is always produced. Mirrors test_smart_disk_analyzer's link set.
add_executable(test_fuzz_smart_report
    unit/test_fuzz_smart_report.cpp
    ../src/core/smart_disk_analyzer.cpp
    ../src/core/bundled_tools_manager.cpp
    ${SAK_PROCESS_RUNNER_SOURCES}
    ../src/core/logger.cpp
    ../include/sak/smart_disk_analyzer.h
    ../include/sak/bundled_tools_manager.h
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_smart_report PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_smart_report PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_fuzz_smart_report PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_fuzz_smart_report COMMAND test_fuzz_smart_report
         -o "${TEST_OUTPUT_DIR}/test_fuzz_smart_report.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_fuzz_smart_report")

# test_fuzz_ai_response - deterministic mutation-fuzz of the OpenAI Responses reply parser. The HTTP
# response body is untrusted, and the function_calls parseResponseObject extracts are dispatched to
# real tools, so the contract is fail-closed: a broken tool call poisons the whole response, a
# terminal/non-success status yields nothing, and bad JSON / an API-error envelope yields nothing.
# Asserts no crash/hang, determinism, that every returned call is structurally complete, and an
# independent oracle that a must-be-empty response never surfaces a usable field. Mirrors
# test_openai_responses_client's link set.
add_executable(test_fuzz_ai_response
    unit/test_fuzz_ai_response.cpp
    ../src/ai/openai_responses_client.cpp
    ../src/core/app_paths.cpp
    ../src/ai/ai_token_usage_tracker.cpp
    ../src/ai/ai_credential_store.cpp
    ../src/ai/ai_secret_redaction.cpp
    ../src/ai/ai_paths.cpp
    ../src/core/secure_memory.cpp
    ../src/core/logger.cpp
    ../include/sak/ai/openai_responses_client.h
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_ai_response PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_ai_response PRIVATE
    Qt6::Core Qt6::Test Qt6::Network ${PLATFORM_LIBS}
)
target_include_directories(test_fuzz_ai_response PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_fuzz_ai_response COMMAND test_fuzz_ai_response
         -o "${TEST_OUTPUT_DIR}/test_fuzz_ai_response.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_fuzz_ai_response")

# test_fuzz_mbox_container - deterministic mutation-fuzz of the MBOX container splitter + full read
# pipeline. The header parser and MIME transfer decoder are already fuzzed as pure seams; this covers
# the part that only exists behind an open file: the "From " separator scan (buildMessageIndex/
# isFromLine), the per-message boundary math (readRawMessage), and the recursive MIME walk with its
# part/depth caps. Writes each mutated input to a temp file and drives open->index->read. Asserts no
# crash/hang, determinism, open-implies-at-least-one-message, and well-formed reads. Mirrors
# test_mbox_parser's link set.
add_executable(test_fuzz_mbox_container
    unit/test_fuzz_mbox_container.cpp
    ../src/core/mbox_parser.cpp
    ../src/core/logger.cpp
    ../include/sak/mbox_parser.h
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_mbox_container PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_mbox_container PRIVATE
    Qt6::Core Qt6::Concurrent Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_fuzz_mbox_container PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_fuzz_mbox_container COMMAND test_fuzz_mbox_container
         -o "${TEST_OUTPUT_DIR}/test_fuzz_mbox_container.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_fuzz_mbox_container")

add_executable(test_ai_provider_gateway
    unit/test_ai_provider_gateway.cpp
    ../src/ai/ai_provider_gateway.cpp
    ../src/ai/ai_provider_gateway_authorization.cpp
    ../src/ai/ai_provider_registry.cpp
    ../src/ai/ai_mcp_http_client.cpp
    ../src/ai/ai_mcp_session_pool.cpp
    ../src/ai/ai_mcp_stdio_client.cpp
    ../src/ai/ai_mcp_stdio_session.cpp
    ../include/sak/ai/ai_provider_gateway.h
    ../include/sak/ai/ai_provider_gateway_authorization.h
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
         -o "${TEST_OUTPUT_DIR}/test_ai_provider_gateway.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_provider_gateway")

add_executable(test_ai_app_action_planner
    unit/test_ai_app_action_planner.cpp
    ../src/ai/ai_app_action_planner.cpp
    ../src/ai/ai_command_guard.cpp
    ../src/ai/ai_command_tool_planner.cpp
    ../src/ai/ai_execution_broker.cpp
    # ai_execution_broker resolves interpreters through sak::systemPowerShellPath.
    ${SAK_PROCESS_RUNNER_SOURCES}
    ../src/ai/ai_tool_policy.cpp
    ../src/ai/ai_credential_store.cpp
    ../src/ai/ai_secret_redaction.cpp
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
         -o "${TEST_OUTPUT_DIR}/test_ai_app_action_planner.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_app_action_planner")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_win32_gui_runner.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_win32_gui_runner")

# test_win32_mcp_dialog_choice --- pure, deterministic coverage of the dismiss_dialog button
# selection: affirmative ranking, never-Cancel safety, lone-nameless fallback, explicit override.
# No Win32/UIA -- the logic is a pure function seam, so it runs on any platform.
add_executable(test_win32_mcp_dialog_choice
    unit/test_win32_mcp_dialog_choice.cpp
    ../src/win32mcp/win32_mcp_dialog_choice.cpp
    ../include/sak/win32mcp/win32_mcp_dialog_choice.h
)
set_target_properties(test_win32_mcp_dialog_choice PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_win32_mcp_dialog_choice PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_win32_mcp_dialog_choice PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_win32_mcp_dialog_choice COMMAND test_win32_mcp_dialog_choice
         -o "${TEST_OUTPUT_DIR}/test_win32_mcp_dialog_choice.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_win32_mcp_dialog_choice")

# test_win32_mcp_text_match --- pure, deterministic coverage of the OCR whole-word matcher that
# click_text/find_text/wait_for_text rely on: whole-word (so "OK" != "cookies", "Scan" !=
# "Scanning"), consecutive same-line phrase runs, exact-over-substring + standalone-over-sentence
# ranking, and the opt-in contains fallback. No WinRT OCR -- the matcher is a pure function seam.
add_executable(test_win32_mcp_text_match
    unit/test_win32_mcp_text_match.cpp
    ../src/win32mcp/win32_mcp_text_match.cpp
    ../include/sak/win32mcp/win32_mcp_text_match.h
)
set_target_properties(test_win32_mcp_text_match PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_win32_mcp_text_match PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_win32_mcp_text_match PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_win32_mcp_text_match COMMAND test_win32_mcp_text_match
         -o "${TEST_OUTPUT_DIR}/test_win32_mcp_text_match.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_win32_mcp_text_match")

# test_win32_mcp_key_chord --- deterministic coverage of the send_keys chord parser: correct VK
# mapping, multiple modifiers, named/single keys, and refusal of an unknown modifier or main key so
# the assistant never injects an unintended keystroke. Windows-only (the parser maps to Win32 VKs).
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_win32_mcp_key_chord.cpp")
    add_executable(test_win32_mcp_key_chord
        unit/test_win32_mcp_key_chord.cpp
        ../src/win32mcp/win32_mcp_key_chord.cpp
        ../include/sak/win32mcp/win32_mcp_key_chord.h
    )
    set_target_properties(test_win32_mcp_key_chord PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_win32_mcp_key_chord PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_win32_mcp_key_chord PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_win32_mcp_key_chord COMMAND test_win32_mcp_key_chord
             -o "${TEST_OUTPUT_DIR}/test_win32_mcp_key_chord.txt,txt" -o -,txt
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_win32_mcp_key_chord")
endif()

# test_win32_mcp_input_plan --- deterministic coverage of the SendInput decision logic: absolute-
# coordinate mapping, virtual-screen bounds check (reject a click the OS would clamp to a desktop
# edge), mouse-button mapping, and the type_text keystroke plan (CRLF collapse, surrogate pairs).
# Windows-only (maps to Win32 MOUSEEVENTF_*/VK_* constants).
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_win32_mcp_input_plan.cpp")
    add_executable(test_win32_mcp_input_plan
        unit/test_win32_mcp_input_plan.cpp
        ../src/win32mcp/win32_mcp_input_plan.cpp
        ../include/sak/win32mcp/win32_mcp_input_plan.h
    )
    set_target_properties(test_win32_mcp_input_plan PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_win32_mcp_input_plan PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_win32_mcp_input_plan PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_win32_mcp_input_plan COMMAND test_win32_mcp_input_plan
             -o "${TEST_OUTPUT_DIR}/test_win32_mcp_input_plan.txt,txt" -o -,txt
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_win32_mcp_input_plan")
endif()

# test_win32_mcp_uia_ref --- deterministic coverage of the UIA ref-drift guard that protects
# uia_click_control / uia_get_control_value / dismiss_dialog from acting on a stale ref: identity is
# role + name + bounding-rect origin, and out-of-range against either walk fails closed. Pure logic.
add_executable(test_win32_mcp_uia_ref
    unit/test_win32_mcp_uia_ref.cpp
    ../src/win32mcp/win32_mcp_uia_ref.cpp
    ../include/sak/win32mcp/win32_mcp_uia_ref.h
)
set_target_properties(test_win32_mcp_uia_ref PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_win32_mcp_uia_ref PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_win32_mcp_uia_ref PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_win32_mcp_uia_ref COMMAND test_win32_mcp_uia_ref
         -o "${TEST_OUTPUT_DIR}/test_win32_mcp_uia_ref.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_win32_mcp_uia_ref")

# test_win32_mcp_fingerprint --- deterministic coverage of the visual-fingerprint oracle behind
# compare_screenshots + wait_for_idle: grid sampling with a degenerate-buffer guard, the per-cell
# tolerance boundary, the changed-ratio boundary, and the signed->unsigned gray round-trip. Pure.
add_executable(test_win32_mcp_fingerprint
    unit/test_win32_mcp_fingerprint.cpp
    ../src/win32mcp/win32_mcp_fingerprint.cpp
    ../include/sak/win32mcp/win32_mcp_fingerprint.h
)
set_target_properties(test_win32_mcp_fingerprint PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_win32_mcp_fingerprint PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_win32_mcp_fingerprint PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_win32_mcp_fingerprint COMMAND test_win32_mcp_fingerprint
         -o "${TEST_OUTPUT_DIR}/test_win32_mcp_fingerprint.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_win32_mcp_fingerprint")

# test_win32_mcp_json_clamp --- deterministic coverage of the wait-loop ms clamp: default fallback,
# floor/cap clamping, fractional truncation, and the regression where a huge JSON number must pin to
# the cap instead of overflowing the narrowing cast to the floor. Pure.
add_executable(test_win32_mcp_json_clamp
    unit/test_win32_mcp_json_clamp.cpp
    ../src/win32mcp/win32_mcp_json_clamp.cpp
    ../include/sak/win32mcp/win32_mcp_json_clamp.h
)
set_target_properties(test_win32_mcp_json_clamp PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_win32_mcp_json_clamp PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_win32_mcp_json_clamp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_win32_mcp_json_clamp COMMAND test_win32_mcp_json_clamp
         -o "${TEST_OUTPUT_DIR}/test_win32_mcp_json_clamp.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_win32_mcp_json_clamp")

# test_win32_mcp_geometry --- deterministic coverage of the capture/OCR geometry math: downscale
# sizing (aspect + edge floor), capture-rect validation (empty / oversize guard), inverse-scale,
# and word-box mapping back to absolute screen pixels (a rounding/scale slip mislocates click_text).
add_executable(test_win32_mcp_geometry
    unit/test_win32_mcp_geometry.cpp
    ../src/win32mcp/win32_mcp_geometry.cpp
    ../include/sak/win32mcp/win32_mcp_geometry.h
)
set_target_properties(test_win32_mcp_geometry PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_win32_mcp_geometry PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_win32_mcp_geometry PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_win32_mcp_geometry COMMAND test_win32_mcp_geometry
         -o "${TEST_OUTPUT_DIR}/test_win32_mcp_geometry.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_win32_mcp_geometry")

# test_ai_credential_store - redactSecrets, the last line before a key reaches a log or a
# transcript. It had no test of its own until R5-LEDGER found two leaks in it.
add_executable(test_ai_credential_store
    unit/test_ai_credential_store.cpp
    ../src/ai/ai_credential_store.cpp
    ../src/ai/ai_secret_redaction.cpp
    ../src/ai/ai_paths.cpp
    ../src/core/app_paths.cpp
    ../src/core/secure_memory.cpp
    ../src/core/logger.cpp
    ../include/sak/ai/ai_credential_store.h
)
set_target_properties(test_ai_credential_store PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_credential_store PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_ai_credential_store PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_ai_credential_store COMMAND test_ai_credential_store
         -o "${TEST_OUTPUT_DIR}/test_ai_credential_store.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_credential_store")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_command_guard.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_command_guard")

add_executable(test_ai_command_tool_planner
    unit/test_ai_command_tool_planner.cpp
    ../src/ai/ai_command_tool_planner.cpp
    ../src/ai/ai_command_guard.cpp
    ../src/ai/ai_execution_broker.cpp
    # ai_execution_broker resolves interpreters through sak::systemPowerShellPath.
    ${SAK_PROCESS_RUNNER_SOURCES}
    ../src/ai/ai_tool_policy.cpp
    ../src/ai/ai_credential_store.cpp
    ../src/ai/ai_secret_redaction.cpp
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
         -o "${TEST_OUTPUT_DIR}/test_ai_command_tool_planner.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_command_tool_planner")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_tool_call_router.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_tool_call_router")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_mcp_http_client.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_mcp_http_client")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_mcp_stdio_client.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_mcp_stdio_client")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_mcp_stdio_session.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_mcp_stdio_session")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_mcp_session_pool.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_mcp_session_pool")

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
         -o "${TEST_OUTPUT_DIR}/test_app_action_registry.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_app_action_registry")

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
         -o "${TEST_OUTPUT_DIR}/test_app_action_service.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_app_action_service")

add_executable(test_ai_conversation_store
    unit/test_ai_conversation_store.cpp
    ../src/core/app_paths.cpp
    ../src/ai/ai_conversation_store.cpp
    ../src/ai/ai_credential_store.cpp
    ../src/ai/ai_secret_redaction.cpp
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
         -o "${TEST_OUTPUT_DIR}/test_ai_conversation_store.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_conversation_store")

add_executable(test_ai_chat_title
    unit/test_ai_chat_title.cpp
    ../src/ai/ai_chat_title.cpp
    ../src/ai/ai_secret_redaction.cpp
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
         -o "${TEST_OUTPUT_DIR}/test_ai_chat_title.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_chat_title")

add_executable(test_ai_execution_broker
    unit/test_ai_execution_broker.cpp
    ../src/core/app_paths.cpp
    ../src/ai/ai_execution_broker.cpp
    ../src/ai/ai_credential_store.cpp
    ../src/ai/ai_secret_redaction.cpp
    ../src/ai/ai_paths.cpp
    ../src/core/secure_memory.cpp
    ${SAK_PROCESS_RUNNER_SOURCES}
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
         -o "${TEST_OUTPUT_DIR}/test_ai_execution_broker.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_execution_broker")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_cancellation_token.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_cancellation_token")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_run_state.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_run_state")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_human_gate_store.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_human_gate_store")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_recovery_policy.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_recovery_policy")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_package_selection.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_package_selection")

add_executable(test_ai_workflow_clarifier
    unit/test_ai_workflow_clarifier.cpp
    ../src/ai/ai_workflow_clarifier.cpp
    ../src/ai/ai_workflow_template.cpp
    ../src/ai/ai_workflow_placeholders.cpp
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
         -o "${TEST_OUTPUT_DIR}/test_ai_workflow_clarifier.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_workflow_clarifier")

add_executable(test_ai_trace_store
    unit/test_ai_trace_store.cpp
    ../src/ai/ai_trace_store.cpp
    ../src/ai/ai_credential_store.cpp
    ../src/ai/ai_secret_redaction.cpp
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
         -o "${TEST_OUTPUT_DIR}/test_ai_trace_store.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_trace_store")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_tool_policy.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_tool_policy")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_skill_store.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_skill_store")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_lease_manager.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_lease_manager")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_subagent_tool_executor.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_subagent_tool_executor")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_tool_loop_detector.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_tool_loop_detector")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_package_tool_planner.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_package_tool_planner")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_offline_downloader_tool_runner.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_offline_downloader_tool_runner")

add_executable(test_ai_workflow_powershell_tool_runner
    unit/test_ai_workflow_powershell_tool_runner.cpp
    ../src/ai/ai_workflow_powershell_tool_runner.cpp
    ../src/ai/ai_workflow_placeholders.cpp
    ../src/ai/ai_command_guard.cpp
    ../src/ai/ai_execution_broker.cpp
    # ai_execution_broker resolves interpreters through sak::systemPowerShellPath.
    ${SAK_PROCESS_RUNNER_SOURCES}
    ../src/ai/ai_credential_store.cpp
    ../src/ai/ai_secret_redaction.cpp
    ../src/ai/ai_paths.cpp
    ../src/core/app_paths.cpp
    ../src/core/secure_memory.cpp
    ../include/sak/ai/ai_workflow_powershell_tool_runner.h
    ../include/sak/ai/ai_workflow_placeholders.h
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
         -o "${TEST_OUTPUT_DIR}/test_ai_workflow_powershell_tool_runner.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_workflow_powershell_tool_runner")

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
    # Pulled in by the health ledger, which now surfaces its own failures.
    ../src/core/logger.cpp
    ../include/sak/logger.h
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
         -o "${TEST_OUTPUT_DIR}/test_ai_tool_dispatcher.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_tool_dispatcher")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_tool_turn.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_tool_turn")

add_executable(test_ai_tool_result_recorder
    unit/test_ai_tool_result_recorder.cpp
    ../src/ai/ai_conversation_store.cpp
    ../src/ai/ai_credential_store.cpp
    ../src/ai/ai_secret_redaction.cpp
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
         -o "${TEST_OUTPUT_DIR}/test_ai_tool_result_recorder.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_tool_result_recorder")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_run_state_store.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_run_state_store")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_subagent_runner.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_subagent_runner")

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
         -o "${TEST_OUTPUT_DIR}/test_ai_workflow_evals.txt,txt" -o -,txt
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
         -o "${TEST_OUTPUT_DIR}/test_ai_orchestrator.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_orchestrator")

add_executable(test_ai_workflow_store
    unit/test_ai_workflow_store.cpp
    ../src/core/app_paths.cpp
    ../src/ai/ai_paths.cpp
    ../src/ai/ai_workflow_store.cpp
    ../src/ai/ai_workflow_template.cpp
    ../src/ai/ai_workflow_placeholders.cpp
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
         -o "${TEST_OUTPUT_DIR}/test_ai_workflow_store.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_workflow_store")

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
         -o "${TEST_OUTPUT_DIR}/test_smart_disk_analyzer.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_smart_disk_analyzer")

# test_package_matcher - mapping CRUD, export/import, findMatch
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
         -o "${TEST_OUTPUT_DIR}/test_package_matcher.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_package_matcher")

# --- Tier 3: Hardware & System ---

# test_drive_scanner - DriveInfo struct, getDrives, lifecycle
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
         -o "${TEST_OUTPUT_DIR}/test_drive_scanner.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_drive_scanner")

# test_permission_manager - getOwner, SDDL, stripPermissions, elevation
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
         -o "${TEST_OUTPUT_DIR}/test_permission_manager.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_permission_manager")

# test_elevation_manager - isElevated, canElevate, error messages
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
         -o "${TEST_OUTPUT_DIR}/test_elevation_manager.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_elevation_manager")

# test_elevation_tier - tier classification, feature lookup, table integrity
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
         -o "${TEST_OUTPUT_DIR}/test_elevation_tier.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_elevation_tier")

# test_elevated_pipe_protocol - wire protocol framing, parsing, pipe name generation
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
         -o "${TEST_OUTPUT_DIR}/test_elevated_pipe_protocol.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_elevated_pipe_protocol")

# test_elevated_task_dispatcher - allowlist, dispatch, exception safety, callbacks
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
         -o "${TEST_OUTPUT_DIR}/test_elevated_task_dispatcher.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_elevated_task_dispatcher")

# test_mixed_tier_operations - Phase 3 mixed-tier elevation: PermissionManager
# expected overloads, canReadPath, backup worker signal, task handlers
add_executable(test_mixed_tier_operations
    unit/test_mixed_tier_operations.cpp
    ../src/core/elevated_task_dispatcher.cpp
    ../src/core/permission_manager.cpp
    ../src/core/elevation_manager.cpp
    ../src/core/user_profile_backup_worker.cpp
    ../src/core/user_profile_types.cpp
    ../src/core/smart_file_filter.cpp
    ../src/core/path_utils.cpp
    ${SAK_PROCESS_RUNNER_SOURCES}
    ../src/core/logger.cpp
    ../src/core/backup_file_codec.cpp
    ../src/core/encryption.cpp
    ../src/core/secure_memory.cpp
    ../include/sak/elevated_task_dispatcher.h
    ../include/sak/permission_manager.h
    ../include/sak/user_profile_backup_worker.h
    ../include/sak/user_profile_types.h
)
set_target_properties(test_mixed_tier_operations PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_mixed_tier_operations PRIVATE
    Qt6::Core Qt6::Concurrent Qt6::Test ZLIB::ZLIB ${PLATFORM_LIBS}
)
target_include_directories(test_mixed_tier_operations PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_mixed_tier_operations COMMAND test_mixed_tier_operations
         -o "${TEST_OUTPUT_DIR}/test_mixed_tier_operations.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_mixed_tier_operations")

# test_elevation_ux - Phase 4 shield icon extraction, elevation banner, style tokens
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
         -o "${TEST_OUTPUT_DIR}/test_elevation_ux.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_elevation_ux")

# test_elevation_hardening - Phase 5 legacy removal, tier coverage, gate, allowlist, IPC
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
         -o "${TEST_OUTPUT_DIR}/test_elevation_hardening.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_elevation_hardening")

# --- Tier 4: Streaming/IO ---

# test_streaming_decompressor - gzip/bzip2/xz lifecycle, byte tracking
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
         -o "${TEST_OUTPUT_DIR}/test_streaming_decompressor.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_streaming_decompressor")

# test_logger - initialization, level filtering, file output, thread safety
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
         -o "${TEST_OUTPUT_DIR}/test_logger.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_logger")

# ============================================================================
# Unit Tests - Offline Deployment
# ============================================================================

# test_install_script_parser - PowerShell install script parsing
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
         -o "${TEST_OUTPUT_DIR}/test_install_script_parser.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_install_script_parser")

# test_script_rewriter - URL replacement for offline deployment
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
         -o "${TEST_OUTPUT_DIR}/test_script_rewriter.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_script_rewriter")

# test_package_list_manager - curated package lists and presets
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
         -o "${TEST_OUTPUT_DIR}/test_package_list_manager.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_package_list_manager")

# test_vulnerability_scanner - vulnerability source parsing and product matching
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
         -o "${TEST_OUTPUT_DIR}/test_vulnerability_scanner.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_vulnerability_scanner")

# test_nuget_version_range --- pure NuGet version + version-range primitives (B10-25)
add_executable(test_nuget_version_range
    unit/test_nuget_version_range.cpp
    ../src/core/nuget_version_range.cpp
    ../include/sak/nuget_version_range.h
)
set_target_properties(test_nuget_version_range PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_nuget_version_range PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_nuget_version_range PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_nuget_version_range COMMAND test_nuget_version_range
         -o "${TEST_OUTPUT_DIR}/test_nuget_version_range.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_nuget_version_range")

# test_nuget_dependency_resolver --- transitive resolution e2e (B10-25/B10-28)
add_executable(test_nuget_dependency_resolver
    unit/test_nuget_dependency_resolver.cpp
    ../src/core/nuget_dependency_resolver.cpp
    ../src/core/nuget_version_range.cpp
    ../include/sak/nuget_dependency_resolver.h
    ../include/sak/nuget_version_range.h
)
set_target_properties(test_nuget_dependency_resolver PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_nuget_dependency_resolver PRIVATE
    Qt6::Core Qt6::Test Qt6::Xml ${PLATFORM_LIBS}
)
target_include_directories(test_nuget_dependency_resolver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_nuget_dependency_resolver COMMAND test_nuget_dependency_resolver
         -o "${TEST_OUTPUT_DIR}/test_nuget_dependency_resolver.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_nuget_dependency_resolver")

# test_offline_package_search - offline deploy search workflow, dedup, data flow
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
         -o "${TEST_OUTPUT_DIR}/test_offline_package_search.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_offline_package_search")

# ============================================================================
# Integration Tests
# ============================================================================

# Integration test: UUP Conversion Pipeline
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
         -o "${TEST_OUTPUT_DIR}/test_uup_conversion_pipeline.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")

message(STATUS "Added integration test: test_uup_conversion_pipeline")

# ============================================================================
# Unit Tests - Quick Actions
# ============================================================================

# test_action_factory - Action metadata, category distribution, initial state
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
         -o "${TEST_OUTPUT_DIR}/test_action_factory.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")

message(STATUS "Added unit test: test_action_factory")

# test_backup_bitlocker_keys_action -- B6 fail-closed seams (recovery-key gate,
# unique backup dir, key-protector parse-failure signal, password accounting)
add_executable(test_backup_bitlocker_keys_action
    unit/actions/test_backup_bitlocker_keys_action.cpp

    # Base class
    ../src/core/quick_action.cpp
    ../include/sak/quick_action.h

    # Dependencies used by the action
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

    # Action under test
    ../src/actions/backup_bitlocker_keys_action.cpp
    ../include/sak/actions/backup_bitlocker_keys_action.h
)

set_target_properties(test_backup_bitlocker_keys_action PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)

target_link_libraries(test_backup_bitlocker_keys_action PRIVATE
    Qt6::Core
    Qt6::Test
    Qt6::Widgets
    ${PLATFORM_LIBS}
)

target_include_directories(test_backup_bitlocker_keys_action PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

add_test(NAME test_backup_bitlocker_keys_action
         COMMAND test_backup_bitlocker_keys_action
         -o "${TEST_OUTPUT_DIR}/test_backup_bitlocker_keys_action.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")

message(STATUS "Added unit test: test_backup_bitlocker_keys_action")

# test_verify_system_files_action -- B6-07/08 pure seams (DISM corruption
# detection must not fire on a clean machine; SFC temp filename is unique)
add_executable(test_verify_system_files_action
    unit/actions/test_verify_system_files_action.cpp

    # Base class
    ../src/core/quick_action.cpp
    ../include/sak/quick_action.h

    # Dependencies used by the action
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

    # Action under test
    ../src/actions/verify_system_files_action.cpp
    ../include/sak/actions/verify_system_files_action.h
)

set_target_properties(test_verify_system_files_action PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)

target_link_libraries(test_verify_system_files_action PRIVATE
    Qt6::Core
    Qt6::Test
    Qt6::Widgets
    ${PLATFORM_LIBS}
)

target_include_directories(test_verify_system_files_action PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

add_test(NAME test_verify_system_files_action
         COMMAND test_verify_system_files_action
         -o "${TEST_OUTPUT_DIR}/test_verify_system_files_action.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")

message(STATUS "Added unit test: test_verify_system_files_action")

# test_check_disk_errors_action -- B6-09/10 pure seams (check is read-only; the
# outcome aggregates every drive, not just one that succeeded)
add_executable(test_check_disk_errors_action
    unit/actions/test_check_disk_errors_action.cpp

    # Base class
    ../src/core/quick_action.cpp
    ../include/sak/quick_action.h

    # Dependencies used by the action
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

    # Action under test
    ../src/actions/check_disk_errors_action.cpp
    ../include/sak/actions/check_disk_errors_action.h
)

set_target_properties(test_check_disk_errors_action PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)

target_link_libraries(test_check_disk_errors_action PRIVATE
    Qt6::Core
    Qt6::Test
    Qt6::Widgets
    ${PLATFORM_LIBS}
)

target_include_directories(test_check_disk_errors_action PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

add_test(NAME test_check_disk_errors_action
         COMMAND test_check_disk_errors_action
         -o "${TEST_OUTPUT_DIR}/test_check_disk_errors_action.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")

message(STATUS "Added unit test: test_check_disk_errors_action")

# test_reset_network_action -- B6-12/13 pure seams (adapter reset fails closed;
# a timed-out / non-zero step counts as a failure for the final verdict)
add_executable(test_reset_network_action
    unit/actions/test_reset_network_action.cpp

    # Base class
    ../src/core/quick_action.cpp
    ../include/sak/quick_action.h

    # Dependencies used by the action
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

    # Action under test
    ../src/actions/reset_network_action.cpp
    ../include/sak/actions/reset_network_action.h
)

set_target_properties(test_reset_network_action PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)

target_link_libraries(test_reset_network_action PRIVATE
    Qt6::Core
    Qt6::Test
    Qt6::Widgets
    ${PLATFORM_LIBS}
)

target_include_directories(test_reset_network_action PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

add_test(NAME test_reset_network_action
         COMMAND test_reset_network_action
         -o "${TEST_OUTPUT_DIR}/test_reset_network_action.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")

message(STATUS "Added unit test: test_reset_network_action")

# test_generate_system_report_action -- B6-15 pure seams (a failed collector is
# surfaced; a saved-but-empty report is not reported as success)
add_executable(test_generate_system_report_action
    unit/actions/test_generate_system_report_action.cpp

    # Base class
    ../src/core/quick_action.cpp
    ../include/sak/quick_action.h

    # Dependencies used by the action
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

    # Action under test
    ../src/actions/generate_system_report_action.cpp
    ../include/sak/actions/generate_system_report_action.h
)

set_target_properties(test_generate_system_report_action PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)

target_link_libraries(test_generate_system_report_action PRIVATE
    Qt6::Core
    Qt6::Test
    Qt6::Widgets
    ${PLATFORM_LIBS}
)

target_include_directories(test_generate_system_report_action PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

add_test(NAME test_generate_system_report_action
         COMMAND test_generate_system_report_action
         -o "${TEST_OUTPUT_DIR}/test_generate_system_report_action.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")

message(STATUS "Added unit test: test_generate_system_report_action")

# test_screenshot_settings_action -- B6-17 report seams (write failure surfaced;
# report path advertised only when the file was actually written)
add_executable(test_screenshot_settings_action
    unit/actions/test_screenshot_settings_action.cpp

    ../src/core/quick_action.cpp
    ../include/sak/quick_action.h
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

    ../src/actions/screenshot_settings_action.cpp
    ../include/sak/actions/screenshot_settings_action.h
)

set_target_properties(test_screenshot_settings_action PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)

target_link_libraries(test_screenshot_settings_action PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    Qt6::Widgets
    ${PLATFORM_LIBS}
)

target_include_directories(test_screenshot_settings_action PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

add_test(NAME test_screenshot_settings_action
         COMMAND test_screenshot_settings_action
         -o "${TEST_OUTPUT_DIR}/test_screenshot_settings_action.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")

message(STATUS "Added unit test: test_screenshot_settings_action")

# test_optimize_power_settings_action -- B6-18 pure seam (already-optimized is
# decided by GUID, not a name substring)
add_executable(test_optimize_power_settings_action
    unit/actions/test_optimize_power_settings_action.cpp

    ../src/core/quick_action.cpp
    ../include/sak/quick_action.h
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

    ../src/actions/optimize_power_settings_action.cpp
    ../include/sak/actions/optimize_power_settings_action.h
)

set_target_properties(test_optimize_power_settings_action PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)

target_link_libraries(test_optimize_power_settings_action PRIVATE
    Qt6::Core
    Qt6::Test
    Qt6::Widgets
    ${PLATFORM_LIBS}
)

target_include_directories(test_optimize_power_settings_action PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

add_test(NAME test_optimize_power_settings_action
         COMMAND test_optimize_power_settings_action
         -o "${TEST_OUTPUT_DIR}/test_optimize_power_settings_action.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")

message(STATUS "Added unit test: test_optimize_power_settings_action")

# test_file_explorer_archive_service -- B6-19 exclusive-create (compress does not
# clobber an existing output; compress/extract round trip still works)
add_executable(test_file_explorer_archive_service
    unit/test_file_explorer_archive_service.cpp
    ../src/core/file_explorer_archive_service.cpp
    ../include/sak/file_explorer_archive_service.h
)

set_target_properties(test_file_explorer_archive_service PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)

target_link_libraries(test_file_explorer_archive_service PRIVATE
    Qt6::Core
    Qt6::CorePrivate
    Qt6::Test
    ${PLATFORM_LIBS}
)

target_include_directories(test_file_explorer_archive_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

add_test(NAME test_file_explorer_archive_service
         COMMAND test_file_explorer_archive_service
         -o "${TEST_OUTPUT_DIR}/test_file_explorer_archive_service.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")

message(STATUS "Added unit test: test_file_explorer_archive_service")

# ============================================================================
# Unit Tests - Enterprise Hardening Sprint (TST-02, TST-04, TST-05, TST-10, TST-11)
# ============================================================================

# test_flash_types - FlashProgress arithmetic, FlashResult checks (TST-02)
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
         -o "${TEST_OUTPUT_DIR}/test_flash_types.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_flash_types")

# test_image_flasher_panel - the guards on entering a destructive write: the Flash
# button must require BOTH an image and a target, and a rejected image must select
# nothing. Runs offscreen.
add_executable(test_image_flasher_panel
    unit/test_image_flasher_panel.cpp
    ../src/gui/image_flasher_panel.cpp
    ../src/gui/view_empty_state.cpp
    ../include/sak/view_empty_state.h
    ../src/gui/image_flasher_settings_dialog.cpp
    ../src/gui/windows_iso_download_dialog.cpp
    ../src/gui/linux_iso_download_dialog.cpp
    ../src/gui/detachable_log_window.cpp
    ../src/gui/follow_scroll_controller.cpp
    ../src/gui/info_button.cpp
    ../src/core/flash_coordinator.cpp
    ../src/threading/flash_worker.cpp
    ../src/threading/worker_base.cpp
    ../src/core/image_source.cpp
    ../src/core/drive_unmounter.cpp
    ../src/core/drive_scanner.cpp
    ../src/core/streaming_decompressor.cpp
    ../src/core/decompressor_factory.cpp
    ../src/core/gzip_decompressor.cpp
    ../src/core/bzip2_decompressor.cpp
    ../src/core/xz_decompressor.cpp
    ../src/core/keep_awake.cpp
    ../src/core/iso_analyzer.cpp
    ../src/core/config_manager.cpp
    ../src/core/app_paths.cpp
    ../src/core/windows_iso_downloader.cpp
    ../src/core/linux_iso_downloader.cpp
    ../src/core/linux_distro_catalog.cpp
    ../src/core/uup_iso_builder.cpp
    ../src/core/uup_dump_api.cpp
    ../src/core/windows_usb_creator.cpp
    ../src/core/windows_usb_creator_extract.cpp
    ../src/core/bundled_tools_manager.cpp
    ../src/core/elevation_manager.cpp
    ../src/core/input_validator.cpp
    ${SAK_PROCESS_RUNNER_SOURCES}
    ../src/core/logger.cpp
    # Listed so AUTOMOC generates the meta-objects for these Q_OBJECT classes.
    ../include/sak/image_flasher_panel.h
    ../include/sak/image_flasher_settings_dialog.h
    ../include/sak/windows_iso_download_dialog.h
    ../include/sak/linux_iso_download_dialog.h
    ../include/sak/detachable_log_window.h
    ../include/sak/follow_scroll_controller.h
    ../include/sak/info_button.h
    ../include/sak/flash_coordinator.h
    ../include/sak/flash_worker.h
    ../include/sak/worker_base.h
    ../include/sak/image_source.h
    ../include/sak/drive_unmounter.h
    ../include/sak/drive_scanner.h
    ../include/sak/streaming_decompressor.h
    ../include/sak/gzip_decompressor.h
    ../include/sak/bzip2_decompressor.h
    ../include/sak/xz_decompressor.h
    ../include/sak/uup_dump_api.h
    ../include/sak/linux_distro_catalog.h
    ../include/sak/bundled_tools_manager.h
    ../include/sak/iso_analyzer.h
    ../include/sak/config_manager.h
    ../include/sak/windows_iso_downloader.h
    ../include/sak/linux_iso_downloader.h
    ../include/sak/uup_iso_builder.h
    ../include/sak/windows_usb_creator.h
    ../include/sak/elevation_manager.h
)
set_target_properties(test_image_flasher_panel PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_image_flasher_panel PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Test
    ZLIB::ZLIB BZip2::BZip2 LibLZMA::LibLZMA ${PLATFORM_LIBS}
)
target_include_directories(test_image_flasher_panel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_image_flasher_panel COMMAND test_image_flasher_panel
         -o "${TEST_OUTPUT_DIR}/test_image_flasher_panel.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
set_tests_properties(test_image_flasher_panel PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
message(STATUS "Added unit test: test_image_flasher_panel")

# test_flasher_policy - pure decisions behind the Image Flasher settings:
# the large-drive warning threshold and the concurrent-write ceiling.
add_executable(test_flasher_policy
    unit/test_flasher_policy.cpp
    ../include/sak/flasher_policy.h
)
set_target_properties(test_flasher_policy PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_flasher_policy PRIVATE
    Qt6::Core Qt6::Test
)
target_include_directories(test_flasher_policy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_flasher_policy COMMAND test_flasher_policy
         -o "${TEST_OUTPUT_DIR}/test_flasher_policy.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_flasher_policy")

# test_quick_action - QuickAction static helpers and base class (TST-10)
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
         -o "${TEST_OUTPUT_DIR}/test_quick_action.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_quick_action")

# test_user_profile_types - Serialization round-trips (TST-05)
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
         -o "${TEST_OUTPUT_DIR}/test_user_profile_types.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_user_profile_types")

# test_user_data_manager - Checksums, data locations, size calc (TST-04)
add_executable(test_user_data_manager
    unit/test_user_data_manager.cpp
    ../src/core/user_data_manager.cpp
    ../src/core/cleanup_worker.cpp
    ../include/sak/cleanup_worker.h
    ../src/threading/worker_base.cpp
    ../include/sak/worker_base.h
    ../src/core/recycle_bin.cpp
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
    Qt6::Core Qt6::Gui Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_user_data_manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_user_data_manager COMMAND test_user_data_manager
         -o "${TEST_OUTPUT_DIR}/test_user_data_manager.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_user_data_manager")

# test_duplicate_finder_worker - Duplicate detection with temp files (TST-11)
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
         -o "${TEST_OUTPUT_DIR}/test_duplicate_finder_worker.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_duplicate_finder_worker")

# test_organizer_worker - File categorization and undo (TST-11)
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
         -o "${TEST_OUTPUT_DIR}/test_organizer_worker.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_organizer_worker")

# test_app_installation_worker - Job filtering, stats, state management (TST-03)
add_executable(test_app_installation_worker
    unit/test_app_installation_worker.cpp
    ../src/core/app_installation_worker.cpp
    ../include/sak/app_installation_worker.h
    ../src/core/nuget_version_range.cpp
    ../include/sak/nuget_version_range.h
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
         -o "${TEST_OUTPUT_DIR}/test_app_installation_worker.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_app_installation_worker")

# test_app_installation_busy - pure predicate seam for the App Installation panel's single
# in-flight authority: the online Chocolatey install path and the offline deployment path are
# mutually exclusive, so one predicate answers for both entry points and both control groups.
add_executable(test_app_installation_busy
    unit/test_app_installation_busy.cpp
    ../src/core/app_installation_busy.cpp
    ../include/sak/app_installation_busy.h
)
set_target_properties(test_app_installation_busy PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_app_installation_busy PRIVATE
    Qt6::Core Qt6::Test
)
target_include_directories(test_app_installation_busy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_app_installation_busy COMMAND test_app_installation_busy
         -o "${TEST_OUTPUT_DIR}/test_app_installation_busy.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_app_installation_busy")

# test_user_profile_restore_worker - Restore flow, conflict resolution, signals (TST-06)
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
    ../src/core/process_runner.cpp
    ../include/sak/process_runner.h
    ../src/core/logger.cpp
    ../src/core/backup_file_codec.cpp
    ../include/sak/backup_file_codec.h
    ../src/core/encryption.cpp
    ../src/core/secure_memory.cpp
)
set_target_properties(test_user_profile_restore_worker PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_user_profile_restore_worker PRIVATE
    Qt6::Core Qt6::Test ZLIB::ZLIB ${PLATFORM_LIBS}
)
target_include_directories(test_user_profile_restore_worker PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_user_profile_restore_worker COMMAND test_user_profile_restore_worker
         -o "${TEST_OUTPUT_DIR}/test_user_profile_restore_worker.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_user_profile_restore_worker")

# test_windows_usb_creator - Input validation, BUG-09 disk number injection (TST-07)
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
         -o "${TEST_OUTPUT_DIR}/test_windows_usb_creator.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_windows_usb_creator")

# test_flash_worker - Structs, validation, early-exit paths, cancellation (TST-09)
add_executable(test_flash_worker
    unit/test_flash_worker.cpp
    ../src/threading/flash_worker.cpp
    ../src/core/drive_scanner.cpp
    ../include/sak/drive_scanner.h
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
    Qt6::Core Qt6::Concurrent Qt6::Test ZLIB::ZLIB BZip2::BZip2 LibLZMA::LibLZMA ${PLATFORM_LIBS}
)
target_compile_definitions(test_flash_worker PRIVATE
    QT_DISABLE_DEPRECATED_BEFORE=0x060000
)
target_include_directories(test_flash_worker PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_flash_worker COMMAND test_flash_worker
         -o "${TEST_OUTPUT_DIR}/test_flash_worker.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_flash_worker")

# ============================================================================
# Unit Tests - Phase 8: Expanded Test Coverage
# ============================================================================

# test_quick_action_result_io - enum to string round-trips, file write/read
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
         -o "${TEST_OUTPUT_DIR}/test_quick_action_result_io.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_quick_action_result_io")

# test_keep_awake -- KeepAwake refcount / guard lifetime (P06-08 overlap)
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
         -o "${TEST_OUTPUT_DIR}/test_keep_awake.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_keep_awake")

# test_quick_action_controller - registration, lookup, category filtering
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
         -o "${TEST_OUTPUT_DIR}/test_quick_action_controller.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_quick_action_controller")

# test_migration_report - entry CRUD, selection, counts, distribution
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
         -o "${TEST_OUTPUT_DIR}/test_migration_report.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_migration_report")

# test_linux_distro_catalog - catalog integrity, category filtering, resolution
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
         -o "${TEST_OUTPUT_DIR}/test_linux_distro_catalog.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_linux_distro_catalog")

# test_flash_coordinator - defaults, config, guards, cancel safety
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
         -o "${TEST_OUTPUT_DIR}/test_flash_coordinator.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_flash_coordinator")

# test_user_profile_backup_worker - construction, initial state, cancel
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
    ../src/core/backup_file_codec.cpp
    ../src/core/encryption.cpp
    ../src/core/secure_memory.cpp
)
set_target_properties(test_user_profile_backup_worker PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_user_profile_backup_worker PRIVATE
    Qt6::Core Qt6::Test ZLIB::ZLIB ${PLATFORM_LIBS}
)
target_include_directories(test_user_profile_backup_worker PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_user_profile_backup_worker COMMAND test_user_profile_backup_worker
         -o "${TEST_OUTPUT_DIR}/test_user_profile_backup_worker.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_user_profile_backup_worker")

# test_uup_dump_api --- channel/ring maps, entry safety, SHA-1 validation (B10-20)
add_executable(test_uup_dump_api
    unit/test_uup_dump_api.cpp
    ../src/core/uup_dump_api.cpp
    ../include/sak/uup_dump_api.h
    ../src/core/logger.cpp
)
set_target_properties(test_uup_dump_api PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_uup_dump_api PRIVATE
    Qt6::Core Qt6::Test Qt6::Network ${PLATFORM_LIBS}
)
target_include_directories(test_uup_dump_api PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_uup_dump_api COMMAND test_uup_dump_api
         -o "${TEST_OUTPUT_DIR}/test_uup_dump_api.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_uup_dump_api")

# test_uup_iso_builder - phase defaults, cancel, weight constants
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
         -o "${TEST_OUTPUT_DIR}/test_uup_iso_builder.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_uup_iso_builder")

# test_all_actions_metadata - per-action expected values, cancel behavior
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
         -o "${TEST_OUTPUT_DIR}/test_all_actions_metadata.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_all_actions_metadata")

# ============================================================================
# Unit Tests - Advanced Search Panel
# ============================================================================

# test_advanced_search_types - struct defaults, extension sets, value semantics
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
         -o "${TEST_OUTPUT_DIR}/test_advanced_search_types.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_advanced_search_types")

# test_regex_pattern_library - built-in/custom patterns, enable/disable, CRUD, combined
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
         -o "${TEST_OUTPUT_DIR}/test_regex_pattern_library.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_regex_pattern_library")

# test_advanced_search_worker - regex compilation, text/metadata/archive/binary search
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
         -o "${TEST_OUTPUT_DIR}/test_advanced_search_worker.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_advanced_search_worker")

# test_advanced_search_controller - state machine, history, preferences, lifecycle
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
         -o "${TEST_OUTPUT_DIR}/test_advanced_search_controller.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_advanced_search_controller")

# ============================================================================
# Unit Tests - Advanced Uninstall Panel
# ============================================================================

# test_email_html_sanitizer --- the two policies that make an attacker-authored message body safe
# to render in-process: the active-content sanitizer shared with the HTML/PDF export writers, and
# the data:-only resource whitelist the message viewers enforce via loadResource(). Asserted
# against a realistic hostile newsletter, including a control that ordinary formatting (bold,
# lists, tables, inline colors) survives. Pure string/QUrl logic, no display.
add_executable(test_email_html_sanitizer
    unit/test_email_html_sanitizer.cpp
)
set_target_properties(test_email_html_sanitizer PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_email_html_sanitizer PRIVATE
    Qt6::Core Qt6::Test
)
target_include_directories(test_email_html_sanitizer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_email_html_sanitizer COMMAND test_email_html_sanitizer
         -o "${TEST_OUTPUT_DIR}/test_email_html_sanitizer.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_email_html_sanitizer")

# test_rich_text_safety --- the shared seam that neutralizes untrusted text before it reaches a Qt
# rich-text sink (tooltips and QTextEdit::append have no plain-text mode). Pure string logic:
# markup in a registry program name, a file name off a raw image, or AI output must come out as
# literal characters, and the wrapper -- not the value -- must be what selects rich text.
add_executable(test_rich_text_safety
    unit/test_rich_text_safety.cpp
)
set_target_properties(test_rich_text_safety PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_rich_text_safety PRIVATE
    Qt6::Core Qt6::Test
)
target_include_directories(test_rich_text_safety PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_rich_text_safety COMMAND test_rich_text_safety
         -o "${TEST_OUTPUT_DIR}/test_rich_text_safety.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_rich_text_safety")

# test_apfs_free_queue_guard --- F25: an APFS free-queue record schedules ghost blocks for reuse,
# so a record covering live container metadata hands the allocator the checkpoint ring or the
# internal pool and the next allocation destroys them. The guard is pure interval arithmetic, and
# it was written WRONG three times -- each a false-close that rejected a legitimate commit, each
# caught only by a full build plus the container round-trip tests. Pinned here from both sides so
# the next attempt fails in seconds instead.
add_executable(test_apfs_free_queue_guard
    unit/test_apfs_free_queue_guard.cpp
)
set_target_properties(test_apfs_free_queue_guard PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_apfs_free_queue_guard PRIVATE
    Qt6::Core Qt6::Test
)
target_include_directories(test_apfs_free_queue_guard PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_apfs_free_queue_guard COMMAND test_apfs_free_queue_guard
         -o "${TEST_OUTPUT_DIR}/test_apfs_free_queue_guard.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_apfs_free_queue_guard")

# test_backup_destination_guard --- fail-closed screen for the user-profile backup destination:
# whitespace-only text, relative paths, and folders that overlap a selected source profile are
# refused outright (header-only guard; pure string/path logic, no filesystem access). Windows-only
# because absoluteness and case-insensitive path comparison are platform behavior.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_backup_destination_guard.cpp")
    add_executable(test_backup_destination_guard
        unit/test_backup_destination_guard.cpp
    )
    set_target_properties(test_backup_destination_guard PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_backup_destination_guard PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_backup_destination_guard PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_backup_destination_guard COMMAND test_backup_destination_guard
             -o "${TEST_OUTPUT_DIR}/test_backup_destination_guard.txt,txt" -o -,txt
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_backup_destination_guard")
endif()

# test_leftover_cleanup_guard --- fail-closed target screens behind software.clean_leftovers
# (header-only guard; pure, no filesystem/registry mutation).
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
         -o "${TEST_OUTPUT_DIR}/test_leftover_cleanup_guard.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_leftover_cleanup_guard")

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
        ${CMAKE_SOURCE_DIR}/src/win32mcp/win32_mcp_dialog_choice.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/win32_mcp_uia_ref.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/win32_mcp_capture.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/win32_mcp_geometry.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/win32_mcp_ocr.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/win32_mcp_text_match.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/win32_mcp_json_clamp.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/win32_mcp_watch.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/win32_mcp_fingerprint.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/win32_mcp_input.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/win32_mcp_input_plan.cpp
        ${CMAKE_SOURCE_DIR}/src/win32mcp/win32_mcp_key_chord.cpp
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
             -o "${TEST_OUTPUT_DIR}/test_win32_mcp_server.txt,txt" -o -,txt
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_win32_mcp_server")
endif()

# test_browser_contract --- pure DOM-snapshot renderer + browser tool catalog + tool->extension
# command translation. OS-agnostic (Qt Core only, no windows.h), so it builds and runs on any host.
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
         -o "${TEST_OUTPUT_DIR}/test_browser_contract.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_browser_contract")

# test_native_messaging --- Chrome native-messaging frame codec + host message handler
# (ping->pong handshake). OS-agnostic (Qt Core only), so it builds and runs on any host.
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
         -o "${TEST_OUTPUT_DIR}/test_native_messaging.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_native_messaging")

# test_browser_bridge --- the MCP-side browser session state machine (id minting,
# single-winner reply matching, snapshot->ref_index, session/stale lifecycle).
# OS-agnostic (Qt Core only); links browser_bridge + browser_contract.
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
         -o "${TEST_OUTPUT_DIR}/test_browser_bridge.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_browser_bridge")

# test_browser_extension_installer --- headless Chrome force-install policy + native-host
# registration. Registry tests run only on Windows (guarded in the .cpp) against a throwaway
# HKCU\Software\SAK_Test subkey; the identity guard (manifest key -> pinned id/version) runs
# everywhere. SAK_REPO_ROOT lets the identity test read the committed manifest.json.
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
         -o "${TEST_OUTPUT_DIR}/test_browser_extension_installer.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_browser_extension_installer")

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
             -o "${TEST_OUTPUT_DIR}/test_browser_bridge_security.txt,txt" -o -,txt
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
             -o "${TEST_OUTPUT_DIR}/test_browser_bridge_pipe.txt,txt" -o -,txt
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
             -o "${TEST_OUTPUT_DIR}/test_browser_bridge_relay.txt,txt" -o -,txt
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_browser_bridge_relay")
endif()

# test_advanced_uninstall_types - struct defaults, enums, value semantics
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
         -o "${TEST_OUTPUT_DIR}/test_advanced_uninstall_types.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_advanced_uninstall_types")

# test_windows_user_scanner --- profile/folder scanning + per-folder sizing (B10-31)
add_executable(test_windows_user_scanner
    unit/test_windows_user_scanner.cpp
    ../src/core/windows_user_scanner.cpp
    ../src/core/logger.cpp
    ../include/sak/windows_user_scanner.h
)
set_target_properties(test_windows_user_scanner PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_windows_user_scanner PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_windows_user_scanner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_windows_user_scanner COMMAND test_windows_user_scanner
         -o "${TEST_OUTPUT_DIR}/test_windows_user_scanner.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_windows_user_scanner")

# test_app_scanner --- registry hive enumeration robustness (B10-31)
add_executable(test_app_scanner
    unit/test_app_scanner.cpp
    ../src/core/app_scanner.cpp
    ../src/core/bundled_tools_manager.cpp
    ../src/core/chocolatey_manager.cpp
    ${SAK_PROCESS_RUNNER_SOURCES}
    ../src/core/logger.cpp
    ../include/sak/app_scanner.h
    ../include/sak/chocolatey_manager.h
)
set_target_properties(test_app_scanner PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_app_scanner PRIVATE
    Qt6::Core Qt6::Gui Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_app_scanner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_app_scanner COMMAND test_app_scanner
         -o "${TEST_OUTPUT_DIR}/test_app_scanner.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_app_scanner")

# test_chocolatey_manager --- struct defaults, parse, input validation (B10-31)
add_executable(test_chocolatey_manager
    unit/test_chocolatey_manager.cpp
    ../src/core/chocolatey_manager.cpp
    ${SAK_PROCESS_RUNNER_SOURCES}
    ../src/core/logger.cpp
    ../include/sak/chocolatey_manager.h
)
set_target_properties(test_chocolatey_manager PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_chocolatey_manager PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_chocolatey_manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_chocolatey_manager COMMAND test_chocolatey_manager
         -o "${TEST_OUTPUT_DIR}/test_chocolatey_manager.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_chocolatey_manager")

# test_cleanup_worker --- construction, enum coverage, recycle-fallback surfacing (B10-23)
add_executable(test_cleanup_worker
    unit/test_cleanup_worker.cpp
    ../src/core/cleanup_worker.cpp
    ../src/core/recycle_bin.cpp
    ${SAK_PROCESS_RUNNER_SOURCES}
    ../src/threading/worker_base.cpp
    ../src/core/logger.cpp
    ../include/sak/cleanup_worker.h
    ../include/sak/worker_base.h
)
set_target_properties(test_cleanup_worker PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_cleanup_worker PRIVATE
    Qt6::Core Qt6::Gui Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_cleanup_worker PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_cleanup_worker COMMAND test_cleanup_worker
         -o "${TEST_OUTPUT_DIR}/test_cleanup_worker.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_cleanup_worker")

# test_advanced_uninstall_controller - state machine, queue, settings, input validation
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
         -o "${TEST_OUTPUT_DIR}/test_advanced_uninstall_controller.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_advanced_uninstall_controller")

# test_leftover_scanner - pattern building, risk classification, scan levels, cancellation
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
         -o "${TEST_OUTPUT_DIR}/test_leftover_scanner.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_leftover_scanner")

# test_program_enumerator --- enumeration signals (generation token), bloatware/orphan detection
add_executable(test_program_enumerator
    unit/test_program_enumerator.cpp
    ../src/core/program_enumerator.cpp
    ${SAK_PROCESS_RUNNER_SOURCES}
    ../src/core/logger.cpp
    ../include/sak/program_enumerator.h
)
set_target_properties(test_program_enumerator PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_program_enumerator PRIVATE
    Qt6::Core Qt6::Gui Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_program_enumerator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_program_enumerator COMMAND test_program_enumerator
         -o "${TEST_OUTPUT_DIR}/test_program_enumerator.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_program_enumerator")

# test_registry_snapshot_engine - diff logic, pattern matching, capture smoke test
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
         -o "${TEST_OUTPUT_DIR}/test_registry_snapshot_engine.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_registry_snapshot_engine")

# ============================================================================
# Network Diagnostic Panel Tests
# ============================================================================

# test_network_diagnostic_types - value/move semantics, enums, compile-time invariants
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
         -o "${TEST_OUTPUT_DIR}/test_network_diagnostic_types.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_network_diagnostic_types")

# test_network_diagnostic_utils - WiFi freq conversion, vendor lookup, port presets, DNS lists
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
         -o "${TEST_OUTPUT_DIR}/test_network_diagnostic_utils.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_network_diagnostic_utils")

# test_network_share_browser -- SMB share discovery: value shape plus the fail-closed split
# between an authoritative discoveryComplete and a partial/failed discoveryFailed.
add_executable(test_network_share_browser
    unit/test_network_share_browser.cpp
    ../src/core/network_share_browser.cpp
    ../include/sak/network_share_browser.h
)
set_target_properties(test_network_share_browser PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_network_share_browser PRIVATE
    Qt6::Core Qt6::Network Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_network_share_browser PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_network_share_browser COMMAND test_network_share_browser
         -o "${TEST_OUTPUT_DIR}/test_network_share_browser.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_network_share_browser")

# ============================================================================
# Previously unwired unit tests
#
# The suites below existed under unit/ and were listed in tests/README.md, but
# had no target in this file, so they never built and never ran. They are wired
# with the same per-target idiom as the rest of the file. Grouped here (rather
# than scattered by subject) so the set stays auditable: a test file with no
# target is invisible, and this block is the record of the ones that were.
# ============================================================================

# test_active_connections_monitor -- MonitorConfig/ConnectionInfo defaults, start/stop
# lifecycle, and the live TCP/UDP table snapshot (iphlpapi, no network traffic).
add_executable(test_active_connections_monitor
    unit/test_active_connections_monitor.cpp
    ../src/core/active_connections_monitor.cpp
    ../include/sak/active_connections_monitor.h
)
set_target_properties(test_active_connections_monitor PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_active_connections_monitor PRIVATE
    Qt6::Core Qt6::Network Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_active_connections_monitor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_active_connections_monitor COMMAND test_active_connections_monitor
         -o "${TEST_OUTPUT_DIR}/test_active_connections_monitor.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_active_connections_monitor")

# test_bundled_tools_manager -- singleton identity, bundled tool/script/module path
# composition, and existence probes for absent entries.
add_executable(test_bundled_tools_manager
    unit/test_bundled_tools_manager.cpp
    ../src/core/bundled_tools_manager.cpp
    ../include/sak/bundled_tools_manager.h
)
set_target_properties(test_bundled_tools_manager PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_bundled_tools_manager PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_bundled_tools_manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_bundled_tools_manager COMMAND test_bundled_tools_manager
         -o "${TEST_OUTPUT_DIR}/test_bundled_tools_manager.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_bundled_tools_manager")

# test_drive_unmounter -- construction, error channel, and the read-only volume query
# for a physical drive number that cannot exist (no unmount is ever attempted).
add_executable(test_drive_unmounter
    unit/test_drive_unmounter.cpp
    ../src/core/drive_unmounter.cpp
    ../src/core/logger.cpp
    ../include/sak/drive_unmounter.h
)
set_target_properties(test_drive_unmounter PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_drive_unmounter PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_drive_unmounter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_drive_unmounter COMMAND test_drive_unmounter
         -o "${TEST_OUTPUT_DIR}/test_drive_unmounter.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_drive_unmounter")

# test_image_source -- ImageMetadata validity, extension format detection, and the
# DecompressorFactory-backed isCompressed classification (streamable vs not).
add_executable(test_image_source
    unit/test_image_source.cpp
    ../src/core/image_source.cpp
    ../src/core/decompressor_factory.cpp
    ../src/core/streaming_decompressor.cpp
    ../src/core/gzip_decompressor.cpp
    ../src/core/bzip2_decompressor.cpp
    ../src/core/xz_decompressor.cpp
    ../src/core/file_hash.cpp
    ../src/core/logger.cpp
    ../include/sak/image_source.h
    ../include/sak/streaming_decompressor.h
    ../include/sak/gzip_decompressor.h
    ../include/sak/bzip2_decompressor.h
    ../include/sak/xz_decompressor.h
)
set_target_properties(test_image_source PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_image_source PRIVATE
    Qt6::Core Qt6::Test ZLIB::ZLIB BZip2::BZip2 LibLZMA::LibLZMA ${PLATFORM_LIBS}
)
target_include_directories(test_image_source PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_image_source COMMAND test_image_source
         -o "${TEST_OUTPUT_DIR}/test_image_source.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_image_source")

# test_network_adapter_inspector -- the pure formatting seams (link speed, MAC bytes),
# which are the parts that need no adapter present.
add_executable(test_network_adapter_inspector
    unit/test_network_adapter_inspector.cpp
    ../src/core/network_adapter_inspector.cpp
    ../src/core/logger.cpp
    ../include/sak/network_adapter_inspector.h
)
set_target_properties(test_network_adapter_inspector PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_network_adapter_inspector PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_network_adapter_inspector PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_network_adapter_inspector COMMAND test_network_adapter_inspector
         -o "${TEST_OUTPUT_DIR}/test_network_adapter_inspector.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_network_adapter_inspector")

# test_network_diagnostic_controller -- idle state, operation bookkeeping, and the
# fail-closed input guards (unknown report format, out-of-range LAN transfer bounds).
# Links the whole tool set the controller owns; none of the tests start a tool.
add_executable(test_network_diagnostic_controller
    unit/test_network_diagnostic_controller.cpp
    ../src/core/network_diagnostic_controller.cpp
    ../src/core/active_connections_monitor.cpp
    ../src/core/bandwidth_tester.cpp
    ../src/core/connectivity_tester.cpp
    ../src/core/dns_diagnostic_tool.cpp
    ../src/core/ethernet_config_manager.cpp
    # EthernetConfigInfo::parseNetIpConfigJson -- see the ethernet manager's locale fix.
    ../src/core/user_profile_types.cpp
    ../src/core/firewall_rule_auditor.cpp
    ../src/core/network_adapter_inspector.cpp
    ../src/core/network_diagnostic_report_generator.cpp
    ../src/core/network_share_browser.cpp
    ../src/core/port_scanner.cpp
    ../src/core/wifi_analyzer.cpp
    ${SAK_NETWORK_TRANSFER_RUNNER_SOURCES}
    ${SAK_PROCESS_RUNNER_SOURCES}
    ../src/core/logger.cpp
    ../include/sak/network_diagnostic_controller.h
    ../include/sak/active_connections_monitor.h
    ../include/sak/bandwidth_tester.h
    ../include/sak/connectivity_tester.h
    ../include/sak/dns_diagnostic_tool.h
    ../include/sak/ethernet_config_manager.h
    ../include/sak/firewall_rule_auditor.h
    ../include/sak/network_adapter_inspector.h
    ../include/sak/network_diagnostic_report_generator.h
    ../include/sak/network_share_browser.h
    ../include/sak/port_scanner.h
    ../include/sak/wifi_analyzer.h
)
set_target_properties(test_network_diagnostic_controller PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_network_diagnostic_controller PRIVATE
    Qt6::Core Qt6::Network Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_network_diagnostic_controller PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_network_diagnostic_controller COMMAND test_network_diagnostic_controller
         -o "${TEST_OUTPUT_DIR}/test_network_diagnostic_controller.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_network_diagnostic_controller")

# test_port_scanner -- well-known service naming, the scan presets, and ScanConfig
# defaults. No socket is opened by any of these.
add_executable(test_port_scanner
    unit/test_port_scanner.cpp
    ../src/core/port_scanner.cpp
    ../include/sak/port_scanner.h
)
set_target_properties(test_port_scanner PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_port_scanner PRIVATE
    Qt6::Core Qt6::Network Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_port_scanner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_port_scanner COMMAND test_port_scanner
         -o "${TEST_OUTPUT_DIR}/test_port_scanner.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_port_scanner")

# test_uninstall_worker -- construction, report/value defaults, enum ordering, and the
# native-uninstall exit-code classifier (3010 reboot-required is a success).
add_executable(test_uninstall_worker
    unit/test_uninstall_worker.cpp
    ../src/core/uninstall_worker.cpp
    ../src/core/leftover_scanner.cpp
    ../src/core/registry_snapshot_engine.cpp
    ../src/core/restore_point_manager.cpp
    ../src/core/elevation_manager.cpp
    ${SAK_PROCESS_RUNNER_SOURCES}
    ../src/threading/worker_base.cpp
    ../src/core/logger.cpp
    ../include/sak/uninstall_worker.h
    ../include/sak/leftover_scanner.h
    ../include/sak/registry_snapshot_engine.h
    ../include/sak/restore_point_manager.h
    ../include/sak/worker_base.h
)
set_target_properties(test_uninstall_worker PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
# Qt6::Gui is REQUIRED, not incidental: advanced_uninstall_types.h includes <QImage> for the
# program icon, so ProgramInfo cannot compile without it. Mirrors test_advanced_uninstall_-
# controller, which links the same sources. QGuiApplication opens no window here.
target_link_libraries(test_uninstall_worker PRIVATE
    Qt6::Core Qt6::Gui Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_uninstall_worker PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_uninstall_worker COMMAND test_uninstall_worker
         -o "${TEST_OUTPUT_DIR}/test_uninstall_worker.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_uninstall_worker")

# test_wifi_analyzer -- freq/channel/band helpers, channel utilization, and the
# B9-13 per-BSSID security seam (deriveBssSecurity: RSN/WPA IE + Privacy bit).
add_executable(test_wifi_analyzer
    unit/test_wifi_analyzer.cpp
    ../src/core/wifi_analyzer.cpp
    ../src/core/logger.cpp
    ../include/sak/wifi_analyzer.h
)
set_target_properties(test_wifi_analyzer PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_wifi_analyzer PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_wifi_analyzer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_wifi_analyzer COMMAND test_wifi_analyzer
         -o "${TEST_OUTPUT_DIR}/test_wifi_analyzer.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_wifi_analyzer")

# test_bandwidth_tester -- iPerf config, server lifecycle guards, and the B9-16
# per-instance firewall-rule-name seam (composeFirewallRuleName uniqueness).
add_executable(test_bandwidth_tester
    unit/test_bandwidth_tester.cpp
    ../src/core/bandwidth_tester.cpp
    ${SAK_NETWORK_TRANSFER_RUNNER_SOURCES}
    ${SAK_PROCESS_RUNNER_SOURCES}
    ../src/core/logger.cpp
    ../include/sak/bandwidth_tester.h
)
set_target_properties(test_bandwidth_tester PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_bandwidth_tester PRIVATE
    Qt6::Core Qt6::Network Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_bandwidth_tester PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_bandwidth_tester COMMAND test_bandwidth_tester
         -o "${TEST_OUTPUT_DIR}/test_bandwidth_tester.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_bandwidth_tester")

# test_connectivity_tester -- ping/traceroute/MTR config structs, a live localhost
# ping, and the B9-17 sanitizeConfig bounds seams (clamp count/ttl/packet/etc.).
add_executable(test_connectivity_tester
    unit/test_connectivity_tester.cpp
    ../src/core/connectivity_tester.cpp
    ../include/sak/connectivity_tester.h
)
set_target_properties(test_connectivity_tester PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_connectivity_tester PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_connectivity_tester PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_connectivity_tester COMMAND test_connectivity_tester
         -o "${TEST_OUTPUT_DIR}/test_connectivity_tester.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_connectivity_tester")

# test_dns_diagnostic_tool -- well-known servers, supported record types, and the
# B9-18 seams (record-type validation, numeric-IPv4 guard, order-insensitive compare).
add_executable(test_dns_diagnostic_tool
    unit/test_dns_diagnostic_tool.cpp
    ../src/core/dns_diagnostic_tool.cpp
    ${SAK_PROCESS_RUNNER_SOURCES}
    ../include/sak/dns_diagnostic_tool.h
)
set_target_properties(test_dns_diagnostic_tool PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_dns_diagnostic_tool PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_dns_diagnostic_tool PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_dns_diagnostic_tool COMMAND test_dns_diagnostic_tool
         -o "${TEST_OUTPUT_DIR}/test_dns_diagnostic_tool.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_dns_diagnostic_tool")

# test_network_diagnostic_report - HTML/JSON report generation, sections, metadata
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
         -o "${TEST_OUTPUT_DIR}/test_network_diagnostic_report.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_network_diagnostic_report")

# ============================================================================
# Unit Tests - Utility & Deduplication
# ============================================================================

# test_format_utils - sak::formatBytes() display formatting
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
         -o "${TEST_OUTPUT_DIR}/test_format_utils.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_format_utils")

# test_wifi_profile_scanner - WiFi profile name/security parsing
# test_network_adapter_admin --- the pure half of enable/disable/rename (R5-IDX-19c):
# new-name validation, resolution against the system's own adapter list, and the exact netsh
# argument vectors. No adapter is touched, so this runs anywhere.
add_executable(test_network_adapter_admin
    unit/test_network_adapter_admin.cpp
    ../src/core/network_adapter_admin.cpp
    ${SAK_PROCESS_RUNNER_SOURCES}
    ../src/core/logger.cpp
    ../include/sak/network_adapter_admin.h
)
set_target_properties(test_network_adapter_admin PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_network_adapter_admin PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_network_adapter_admin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_network_adapter_admin COMMAND test_network_adapter_admin
         -o "${TEST_OUTPUT_DIR}/test_network_adapter_admin.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_network_adapter_admin")

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
         -o "${TEST_OUTPUT_DIR}/test_wifi_profile_scanner.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_wifi_profile_scanner")

# ============================================================================
# Unit Tests - Email & PST/OST Inspector Panel
# ============================================================================

# test_email_types - struct defaults, enum values, constants validation
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
         -o "${TEST_OUTPUT_DIR}/test_email_types.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_email_types")

# test_pst_parser - header validation, magic, encryption, lifecycle
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
         -o "${TEST_OUTPUT_DIR}/test_pst_parser.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_pst_parser")

# test_ost_integration - opens real OST/PST files and exercises full parse pipeline
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
         -o "${TEST_OUTPUT_DIR}/test_ost_integration.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added integration test: test_ost_integration")

# test_mbox_parser - indexing, message loading, From line detection
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
    Qt6::Core Qt6::Concurrent Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_mbox_parser PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_mbox_parser COMMAND test_mbox_parser
         -o "${TEST_OUTPUT_DIR}/test_mbox_parser.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_mbox_parser")

# test_email_search_worker - criteria, cancel, search hit structure
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
         -o "${TEST_OUTPUT_DIR}/test_email_search_worker.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_email_search_worker")

# test_email_export_worker - config, result, format coverage
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
         -o "${TEST_OUTPUT_DIR}/test_email_export_worker.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_email_export_worker")

# test_email_profile_manager - discovery, backup/restore, linked files
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
         -o "${TEST_OUTPUT_DIR}/test_email_profile_manager.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_email_profile_manager")

# test_wifi_setup_script --- injection-safe WLAN .cmd builder
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
         -o "${TEST_OUTPUT_DIR}/test_wifi_setup_script.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_wifi_setup_script")

# test_deadline_canceller --- header-only deadline/cancel monitor (single-winner finish/timeout race)
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
         -o "${TEST_OUTPUT_DIR}/test_deadline_canceller.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_deadline_canceller")

# test_email_attachment_saver --- short-write + dedupe integrity
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
         -o "${TEST_OUTPUT_DIR}/test_email_attachment_saver.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_email_attachment_saver")

# test_email_view_ids --- fail-closed decoding of view-item folder/message ids
add_executable(test_email_view_ids
    unit/test_email_view_ids.cpp
    ../include/sak/email_view_ids.h
)
set_target_properties(test_email_view_ids PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_email_view_ids PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_email_view_ids PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_email_view_ids COMMAND test_email_view_ids
         -o "${TEST_OUTPUT_DIR}/test_email_view_ids.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_email_view_ids")

# test_email_report_generator - HTML, JSON, CSV output validation
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
         -o "${TEST_OUTPUT_DIR}/test_email_report_generator.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_email_report_generator")

# test_package_internalization_engine -- B10-11 pure isSafePackageComponent seam
# (package id / version cannot escape the work dir). No network at runtime.
add_executable(test_package_internalization_engine
    unit/test_package_internalization_engine.cpp
    ../src/core/package_internalization_engine.cpp
    ../src/core/install_script_parser.cpp
    ../src/core/script_rewriter.cpp
    ../src/core/nuget_dependency_resolver.cpp
    ../src/core/nuget_version_range.cpp
    ${SAK_PROCESS_RUNNER_SOURCES}
    ${SAK_NETWORK_TRANSFER_RUNNER_SOURCES}
    ../src/core/logger.cpp
    ../include/sak/package_internalization_engine.h
    ../include/sak/install_script_parser.h
    ../include/sak/script_rewriter.h
    ../include/sak/nuget_dependency_resolver.h
    ../include/sak/nuget_version_range.h
)
set_target_properties(test_package_internalization_engine PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_package_internalization_engine PRIVATE
    Qt6::Core Qt6::Network Qt6::Xml Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_package_internalization_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_package_internalization_engine
         COMMAND test_package_internalization_engine
         -o "${TEST_OUTPUT_DIR}/test_package_internalization_engine.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_package_internalization_engine")

# test_offline_deployment_worker --- pure path-safety seams (B10-12/13/14)
add_executable(test_offline_deployment_worker
    unit/test_offline_deployment_worker.cpp
    ../src/core/offline_deployment_worker.cpp
    ../src/core/package_internalization_engine.cpp
    ../src/core/install_script_parser.cpp
    ../src/core/script_rewriter.cpp
    ../src/core/nuget_dependency_resolver.cpp
    ../src/core/nuget_version_range.cpp
    ../src/core/bundled_tools_manager.cpp
    ${SAK_PROCESS_RUNNER_SOURCES}
    ${SAK_NETWORK_TRANSFER_RUNNER_SOURCES}
    ../src/core/logger.cpp
    ../include/sak/offline_deployment_worker.h
    ../include/sak/package_internalization_engine.h
    ../include/sak/install_script_parser.h
    ../include/sak/script_rewriter.h
    ../include/sak/nuget_dependency_resolver.h
    ../include/sak/nuget_version_range.h
)
set_target_properties(test_offline_deployment_worker PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_offline_deployment_worker PRIVATE
    Qt6::Core Qt6::Test Qt6::Network Qt6::Xml Qt6::Concurrent ${PLATFORM_LIBS}
)
target_include_directories(test_offline_deployment_worker PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_offline_deployment_worker
         COMMAND test_offline_deployment_worker
         -o "${TEST_OUTPUT_DIR}/test_offline_deployment_worker.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_offline_deployment_worker")

# test_offline_package_builder - builds the Office PC offline deployment bundle
add_executable(test_offline_package_builder
    integration/test_offline_package_builder.cpp
    ../src/core/offline_deployment_worker.cpp
    ../src/core/package_internalization_engine.cpp
    ../src/core/install_script_parser.cpp
    ../src/core/script_rewriter.cpp
    ../src/core/nuget_dependency_resolver.cpp
    ../src/core/nuget_version_range.cpp
    ../src/core/bundled_tools_manager.cpp
    ${SAK_PROCESS_RUNNER_SOURCES}
    ${SAK_NETWORK_TRANSFER_RUNNER_SOURCES}
    ../src/core/logger.cpp
    ../include/sak/offline_deployment_worker.h
    ../include/sak/package_internalization_engine.h
    ../include/sak/install_script_parser.h
    ../include/sak/script_rewriter.h
    ../include/sak/nuget_dependency_resolver.h
    ../include/sak/nuget_version_range.h
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
         -o "${TEST_OUTPUT_DIR}/test_offline_package_builder.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
# This integration test intentionally downloads and repacks real package
# assets, so the default 300s CTest timeout creates false failures when
# upstream package/CDN latency spikes.
set_tests_properties(test_offline_package_builder PROPERTIES TIMEOUT 900)
message(STATUS "Added integration test: test_offline_package_builder")

# test_ost_converter_types - type defaults, constants, enum coverage
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
         -o "${TEST_OUTPUT_DIR}/test_ost_converter_types.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ost_converter_types")

# test_email_folder_selection - which folders are MAIL folders, shared by the Email
# Inspector tree and its "Export ALL Mail Folders" whole-store export.
add_executable(test_email_folder_selection
    unit/test_email_folder_selection.cpp
    ../src/core/email_folder_selection.cpp
    ../include/sak/email_folder_selection.h
)
set_target_properties(test_email_folder_selection PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_email_folder_selection PRIVATE
    Qt6::Core Qt6::Gui Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_email_folder_selection PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_email_folder_selection COMMAND test_email_folder_selection
         -o "${TEST_OUTPUT_DIR}/test_email_folder_selection.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_email_folder_selection")

# test_eml_writer - RFC 5322 output, filename sanitization, collision handling
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
         -o "${TEST_OUTPUT_DIR}/test_eml_writer.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_eml_writer")

# test_ost_converter_controller - queue management, duplicate detection
add_executable(test_ost_converter_controller
    unit/test_ost_converter_controller.cpp
    ../src/core/ost_converter_controller.cpp
    ../src/core/ost_conversion_worker.cpp
    ../src/core/eml_writer.cpp
    ../src/core/mbox_writer.cpp
    ../src/core/html_email_writer.cpp
    ../src/core/email_attachment_saver.cpp
    ../src/core/pdf_email_writer.cpp
    ../src/core/deleted_item_scanner.cpp
    ../src/core/conversion_report_generator.cpp
    ../src/core/pst_parser.cpp
    ../src/core/logger.cpp
    ../include/sak/ost_converter_controller.h
    ../include/sak/ost_conversion_worker.h
    ../include/sak/eml_writer.h
    ../include/sak/deleted_item_scanner.h
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
         -o "${TEST_OUTPUT_DIR}/test_ost_converter_controller.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ost_converter_controller")

# test_mbox_writer - Unix mbox format, From_ escaping, per-folder
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
         -o "${TEST_OUTPUT_DIR}/test_mbox_writer.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_mbox_writer")

# test_html_email_writer - styled HTML output, data URIs
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
         -o "${TEST_OUTPUT_DIR}/test_html_email_writer.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_html_email_writer")

# test_pdf_email_writer - QPdfWriter output, PDF header
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
         -o "${TEST_OUTPUT_DIR}/test_pdf_email_writer.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_pdf_email_writer")

# test_conversion_report_generator - HTML report, CSV manifest
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
         -o "${TEST_OUTPUT_DIR}/test_conversion_report_generator.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_conversion_report_generator")

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
        test_advanced_search_controller
        test_fuzz_apfs_reader)
    if(TARGET ${_sak_core_test_target})
        target_compile_definitions(${_sak_core_test_target} PRIVATE QT_NO_KEYWORDS)
    endif()
endforeach()

# ============================================================================
# ============================================================================
# PowerShell guard tests
# ============================================================================
#
# browser/pack-extension.ps1 and browser/native-host/register_native_host.ps1 are
# PowerShell, so their guards cannot be exercised from a QtTest binary. Registering them
# here is what makes those guards evidence: the test-registration gate only scans
# test_*.cpp, so a test_*.ps1 left unregistered would sit in the tree looking like
# coverage while never running once -- the precise failure that gate was built to stop.
#
# Both are hermetic: each copies the script under test into a scratch directory and, for
# the native host, passes a throwaway -RegistryRoot, so neither touches the developer's
# Chrome installation or the committed manifest.
if(PLATFORM_WINDOWS)
    find_program(SAK_PWSH_EXECUTABLE NAMES pwsh)
    if(NOT SAK_PWSH_EXECUTABLE)
        message(FATAL_ERROR
                "pwsh was not found, so the PowerShell guard tests could not run. "
                "Refusing to configure a test suite that silently drops them.")
    endif()
    foreach(_sak_ps_test test_pack_extension test_register_native_host)
        add_test(NAME ${_sak_ps_test}
                 COMMAND "${SAK_PWSH_EXECUTABLE}" -NoProfile -NonInteractive
                         -File "${CMAKE_CURRENT_SOURCE_DIR}/unit/${_sak_ps_test}.ps1")
        set_tests_properties(${_sak_ps_test} PROPERTIES TIMEOUT 300)
    endforeach()

    # scripts/test_partition_manager_certification_tools.ps1 self-tests the certification
    # verifiers against synthetic reports -- it creates no VHDs and mutates no disks. It
    # was reachable ONLY from scripts/check_release_readiness.ps1, which runs in the
    # release workflow, and CI has not run for 776 commits (R5-G21-7). An 800-line self
    # test that executes once per release is not a gate, so it runs in the suite now.
    #
    # -OutputRoot is redirected into the build tree. Its default writes synthetic reports
    # under artifacts/ in the working copy, which would dirty the repo on every ctest run
    # and mix generated fixtures in with the real captured certification evidence.
    add_test(NAME test_partition_manager_certification_tools
             COMMAND "${SAK_PWSH_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_partition_manager_certification_tools.ps1"
                     -OutputRoot "${TEST_OUTPUT_DIR}/partition-manager-certification-tools")
    set_tests_properties(test_partition_manager_certification_tools PROPERTIES TIMEOUT 600)
endif()

# The browser-control service worker is ~3300 lines of privileged JavaScript that drives a
# real user's browser over CDP and parses page-controlled data. No C/C++ gate can see it, so
# until this suite existed its correctness rested entirely on review (R5-G21-9). node runs
# the worker AS SHIPPED under a stubbed chrome and exercises its pure decision functions.
#
# A missing node is FATAL rather than a skip: silently dropping the only gate that has ever
# covered that file is exactly the failure R5-G16 documents nine times over.
find_program(SAK_NODE_EXECUTABLE NAMES node)
if(NOT SAK_NODE_EXECUTABLE)
    message(FATAL_ERROR
            "node was not found, so the browser-extension unit suite could not run. "
            "Refusing to configure a test suite that silently drops it.")
endif()
add_test(NAME test_browser_extension_pure
         COMMAND "${SAK_NODE_EXECUTABLE}" --test
                 "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_browser_extension_pure.mjs")
set_tests_properties(test_browser_extension_pure PROPERTIES TIMEOUT 120)

# ============================================================================
# Debug / sanitizer run environment
# ============================================================================
#
# Release test binaries find their Qt DLLs because the packaging step stages
# them next to the executables. The Debug configuration has no such step, so a
# Debug test binary imports Qt6Cored.dll / Qt6Testd.dll and dies with
# STATUS_DLL_NOT_FOUND (0xC0000135) before main -- which looks like a crashing
# test rather than a missing runtime. That is exactly the failure mode that let
# the dead ASan gate hide, so it is fixed here rather than documented as a
# developer setup step.
#
# ENVIRONMENT_MODIFICATION prepends rather than replaces, so the rest of PATH
# survives. Applied to every registered test in this directory, so a new test
# target inherits it automatically and cannot regress.
if(WIN32)
    get_target_property(_sak_qt_core_location Qt6::Core LOCATION)
    if(_sak_qt_core_location)
        get_filename_component(_sak_qt_bin_dir "${_sak_qt_core_location}" DIRECTORY)

        # The vcpkg bin directory is needed for the SAME reason, and only became
        # needed when the build moved to the dynamic x64-windows triplet: zlib1,
        # bz2 and liblzma are now DLLs rather than static libraries. sak_utility
        # gets them staged beside it by a POST_BUILD step, but test executables
        # live in a different directory and had no such step, so every test would
        # have died with STATUS_DLL_NOT_FOUND. Staged here so the fix covers all
        # tests at once instead of per target.
        set(_sak_test_path_dirs "${_sak_qt_bin_dir}")
        if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
            set(_sak_vcpkg_bin_dir "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
            if(EXISTS "${_sak_vcpkg_bin_dir}")
                list(APPEND _sak_test_path_dirs "${_sak_vcpkg_bin_dir}")
            endif()
        endif()

        get_property(_sak_registered_tests DIRECTORY PROPERTY TESTS)
        foreach(_sak_test IN LISTS _sak_registered_tests)
            foreach(_sak_path_dir IN LISTS _sak_test_path_dirs)
                set_property(TEST ${_sak_test} APPEND PROPERTY ENVIRONMENT_MODIFICATION
                             "PATH=path_list_prepend:${_sak_path_dir}")
            endforeach()
        endforeach()
        list(LENGTH _sak_registered_tests _sak_test_count)
        message(STATUS "Test PATH staging: ${_sak_test_count} tests get ${_sak_test_path_dirs}")
    else()
        message(FATAL_ERROR
                "Could not resolve the Qt6::Core location, so test binaries would "
                "not find their Qt DLLs. Refusing to configure a test suite that "
                "cannot run.")
    endif()
endif()

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

# Product code a test exercises comes from the static product libraries declared in the root
# CMakeLists.txt (sak_foundation and the layers above it): a test target lists its own test
# sources, fixtures and fakes, and links the libraries. A library carries its own dependencies, so
# a new product source reaches every test that needs it without editing any test target. The
# source lists below are for the certification helpers only: they compile their production
# sources directly so the source fingerprint they embed binds every object they are built from.
# Two test targets also compile a file directly, deliberately: test_partition_manager_controller
# and test_partition_manager_panel build the controller (and the panel) with
# SAK_PARTITION_MANAGER_PANEL_TEST_HOOKS, which adds members to those classes, so they cannot
# take the library's unhooked objects for them.
set(SAK_PROCESS_RUNNER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/process_runner.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/process_runner.h"
)
set(SAK_WINDOWS_PROCESS_JOB_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/windows_process_job.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/windows_process_job.h"
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

    if(test_sources)
        add_executable(${test_name} ${test_sources})

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
            sak_core
            sak_foundation
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
)
set_target_properties(test_worker_base PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_worker_base PRIVATE
    sak_foundation
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
)
set_target_properties(test_restore_point_manager PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_restore_point_manager PRIVATE
    sak_core sak_foundation
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

# Production runtime the app-driven external certification helper compiles directly (see the
# note at the top of this file); the core tests link the same code from the product libraries.
# No test substitute or test-only compile definition belongs in this source list.
set(SAK_PARTITION_CORE_RUNTIME_SOURCES
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

# test_partition_manager_core - inventory parsing, safety, scripts, queue
add_executable(test_partition_manager_core
    unit/test_partition_manager_core.cpp
)
set_target_properties(test_partition_manager_core PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_partition_manager_core PRIVATE
    sak_core sak_partition_fs sak_foundation
    Qt6::Core Qt6::Test ZLIB::ZLIB sak_lzfse ${PLATFORM_LIBS}
)
target_include_directories(test_partition_manager_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_compile_definitions(test_partition_manager_core PRIVATE
    SAK_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
add_test(NAME test_partition_manager_core COMMAND test_partition_manager_core
         -o "${TEST_OUTPUT_DIR}/test_partition_manager_core.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
# About 3x the measured 201 s (Wave 69 serial run). COST makes a parallel run start the
# longest test first instead of leaving it as the tail.
set_tests_properties(test_partition_manager_core PROPERTIES TIMEOUT 600 COST 201)
message(STATUS "Added unit test: test_partition_manager_core")

# The five Create Image lock runtime shapes each launch the full PowerShell guard
# fixture. Register each data row independently so one slow row cannot consume
# the aggregate core test's deadline or conceal which shape failed.
add_executable(test_partition_image_volume_lock
    unit/test_partition_image_volume_lock.cpp
)
set_target_properties(test_partition_image_volume_lock PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_partition_image_volume_lock PRIVATE
    sak_core sak_partition_fs sak_foundation
    Qt6::Core Qt6::Test ZLIB::ZLIB sak_lzfse ${PLATFORM_LIBS}
)
target_include_directories(test_partition_image_volume_lock PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_compile_definitions(test_partition_image_volume_lock PRIVATE QT_NO_KEYWORDS)
add_test(NAME test_partition_image_volume_lock_whole_disk
         COMMAND test_partition_image_volume_lock
                 scriptBuilder_createImageVolumeLocksRuntime:whole-disk
                 -o "${TEST_OUTPUT_DIR}/test_partition_image_volume_lock_whole_disk.txt,txt"
                 -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
add_test(NAME test_partition_image_volume_lock_selected_partition
         COMMAND test_partition_image_volume_lock
                 scriptBuilder_createImageVolumeLocksRuntime:selected-partition
                 -o "${TEST_OUTPUT_DIR}/test_partition_image_volume_lock_selected_partition.txt,txt"
                 -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
add_test(NAME test_partition_image_volume_lock_selected_mbr_unpinned_guid
         COMMAND test_partition_image_volume_lock
                 scriptBuilder_createImageVolumeLocksRuntime:selected-mbr-unpinned-guid
                 -o "${TEST_OUTPUT_DIR}/test_partition_image_volume_lock_selected_mbr_unpinned_guid.txt,txt"
                 -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
add_test(NAME test_partition_image_volume_lock_offline_whole_disk
         COMMAND test_partition_image_volume_lock
                 scriptBuilder_createImageVolumeLocksRuntime:offline-whole-disk
                 -o "${TEST_OUTPUT_DIR}/test_partition_image_volume_lock_offline_whole_disk.txt,txt"
                 -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
add_test(NAME test_partition_image_volume_lock_offline_selected_partition
         COMMAND test_partition_image_volume_lock
                 scriptBuilder_createImageVolumeLocksRuntime:offline-selected-partition
                 -o "${TEST_OUTPUT_DIR}/test_partition_image_volume_lock_offline_selected_partition.txt,txt"
                 -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
set_tests_properties(
    test_partition_image_volume_lock_whole_disk
    test_partition_image_volume_lock_selected_partition
    test_partition_image_volume_lock_selected_mbr_unpinned_guid
    test_partition_image_volume_lock_offline_whole_disk
    test_partition_image_volume_lock_offline_selected_partition
    PROPERTIES TIMEOUT 210
)
message(STATUS "Added five Create Image source-lock runtime tests")

# test_partition_manager_controller - async preflight/apply/verification state machine
add_executable(test_partition_manager_controller
    unit/test_partition_manager_controller.cpp
    ../src/core/partition_manager_controller.cpp
    ../include/sak/partition_manager_controller.h
)
set_target_properties(test_partition_manager_controller PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_partition_manager_controller PRIVATE
    sak_core sak_partition_fs sak_foundation
    Qt6::Core Qt6::Concurrent Qt6::Test ZLIB::ZLIB sak_lzfse ${PLATFORM_LIBS}
)
target_include_directories(test_partition_manager_controller PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_compile_definitions(test_partition_manager_controller PRIVATE
    SAK_PARTITION_MANAGER_PANEL_TEST_HOOKS
)
add_test(NAME test_partition_manager_controller COMMAND test_partition_manager_controller
         -o "${TEST_OUTPUT_DIR}/test_partition_manager_controller.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_partition_manager_controller")

# App-driven external restore proof uses production runtime, never the controller unit test's
# substitute executor. This helper is not an unattended destructive CTest.
set(SAK_PARTITION_LIVE_CERTIFIER_SOURCES
    certification/partition_manager_live_certifier.cpp
    ${SAK_PARTITION_CORE_RUNTIME_SOURCES}
    ${SAK_WINDOWS_PROCESS_JOB_SOURCES}
    ../src/core/elevation_manager.cpp
    ../src/core/file_hash.cpp
    ../src/core/partition_manager_controller.cpp
    ../src/core/partition_report_generator.cpp
    ../include/sak/elevation_manager.h
    ../include/sak/file_hash.h
    ../include/sak/partition_manager_controller.h
)
add_executable(partition_manager_live_certifier ${SAK_PARTITION_LIVE_CERTIFIER_SOURCES})
set_target_properties(partition_manager_live_certifier PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(partition_manager_live_certifier PRIVATE
    Qt6::Core Qt6::CorePrivate Qt6::Concurrent ZLIB::ZLIB sak_lzfse ${PLATFORM_LIBS} virtdisk
)
target_include_directories(partition_manager_live_certifier PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

# Conservatively bind the complete first-party public-header tree. This includes transitive
# partition dependencies; an unrelated public-header change may require a fresh certification,
# but no undeclared header edit may silently reuse an earlier source stamp.
file(GLOB_RECURSE SAK_PARTITION_CERTIFICATION_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/include/sak/*.h"
    "${CMAKE_SOURCE_DIR}/include/sak/*.hpp"
)
# The linked codec archive contains both upstream C and the separately compiled first-party
# shim. Derive their files from the actual targets, not a second hand-maintained source list.
get_target_property(SAK_PARTITION_CODEC_SOURCES sak_lzfse SOURCES)
get_target_property(SAK_PARTITION_CODEC_SOURCE_DIR sak_lzfse SOURCE_DIR)
get_target_property(SAK_PARTITION_SHIM_SOURCES sak_lzfse_shim SOURCES)
list(REMOVE_ITEM SAK_PARTITION_CODEC_SOURCES "$<TARGET_OBJECTS:sak_lzfse_shim>")
set(SAK_PARTITION_CODEC_INPUTS)
foreach(codec_source IN LISTS SAK_PARTITION_CODEC_SOURCES SAK_PARTITION_SHIM_SOURCES)
    if(codec_source MATCHES "\\$<")
        message(FATAL_ERROR "Unbound conditional codec source: ${codec_source}")
    endif()
    file(REAL_PATH "${codec_source}" codec_path BASE_DIRECTORY "${SAK_PARTITION_CODEC_SOURCE_DIR}")
    list(APPEND SAK_PARTITION_CODEC_INPUTS "${codec_path}")
endforeach()
file(GLOB_RECURSE SAK_PARTITION_CODEC_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/third_party/lzfse/src/*.h"
)
include("${CMAKE_SOURCE_DIR}/cmake/SakSourceFingerprint.cmake")
sak_compute_source_fingerprint(SAK_PARTITION_CERTIFIER_SOURCE_SHA256 "${CMAKE_SOURCE_DIR}"
    ${SAK_PARTITION_LIVE_CERTIFIER_SOURCES}
    ${SAK_PARTITION_CERTIFICATION_HEADERS}
    ${SAK_PARTITION_CODEC_INPUTS}
    ${SAK_PARTITION_CODEC_HEADERS}
    "${CMAKE_SOURCE_DIR}/CMakeLists.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
    "${CMAKE_SOURCE_DIR}/cmake/SakSourceFingerprint.cmake"
    "${CMAKE_SOURCE_DIR}/scripts/verify_source_fingerprint.py"
)
# Only the reporting entrypoint consumes the stamp. A changed stamp must not needlessly
# rebuild every linked runtime object whose own source and compile settings are unchanged.
set_property(SOURCE certification/partition_manager_live_certifier.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS
    SAK_PARTITION_CERTIFIER_SOURCE_SHA256="${SAK_PARTITION_CERTIFIER_SOURCE_SHA256}"
)
# Retain the exact digest preimage beside the executable. Verification can rehash these source
# inputs and compare the manifest digest with the stamp reported by the executed helper.
set(SAK_PARTITION_CERTIFIER_SOURCE_MANIFEST
    "${CMAKE_CURRENT_BINARY_DIR}/partition_manager_live_certifier.sources.txt"
)
file(GENERATE OUTPUT "${SAK_PARTITION_CERTIFIER_SOURCE_MANIFEST}"
    CONTENT "${SAK_PARTITION_CERTIFIER_SOURCE_SHA256_RECORDS}"
    NEWLINE_STYLE LF
)
sak_add_source_fingerprint_guard(partition_manager_live_certifier "${CMAKE_SOURCE_DIR}"
    "${SAK_PARTITION_CERTIFIER_SOURCE_MANIFEST}" "${SAK_PARTITION_CERTIFIER_SOURCE_SHA256}"
    sak_lzfse sak_lzfse_shim
)
add_custom_command(TARGET partition_manager_live_certifier POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SAK_PARTITION_CERTIFIER_SOURCE_MANIFEST}"
        "$<TARGET_FILE_DIR:partition_manager_live_certifier>/partition_manager_live_certifier.sources.txt"
    VERBATIM
)
message(STATUS "Added app-driven certification helper: partition_manager_live_certifier")

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
find_program(SAK_TEST_POWERSHELL_PATH
    NAMES pwsh.exe pwsh powershell.exe powershell
    HINTS
        "$ENV{ProgramFiles}/PowerShell/7"
        "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0")
if(NOT SAK_TEST_POWERSHELL_PATH OR NOT EXISTS "${SAK_TEST_POWERSHELL_PATH}")
    message(FATAL_ERROR
            "PowerShell is required for registered filesystem certification and writer tests; "
            "refusing to configure a suite that silently omits them.")
endif()
add_test(NAME test_partition_filesystem_probe_certifier
         COMMAND "${SAK_TEST_POWERSHELL_PATH}"
                 -NoProfile
                 -NonInteractive
                 -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_partition_filesystem_probe_certifier.ps1"
                 -CertifierPath "$<TARGET_FILE:partition_filesystem_probe_certifier>"
                 -OutputRoot "${TEST_OUTPUT_DIR}/partition_filesystem_probe_certifier"
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")
message(STATUS "Added certification helper test: test_partition_filesystem_probe_certifier")
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

add_test(NAME test_file_management_live_certifier_gate
         COMMAND "${SAK_TEST_POWERSHELL_PATH}"
                 -NoProfile
                 -NonInteractive
                 -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_file_management_live_certifier_gate.ps1"
                 -CertifierPath "$<TARGET_FILE:file_management_live_certifier>"
                 -OutputRoot "${TEST_OUTPUT_DIR}/file_management_live_certifier_gate"
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")
message(STATUS "Added live-certifier gate self-test: test_file_management_live_certifier_gate")

add_test(NAME test_sak_apfs_writer_cli
         COMMAND "${SAK_TEST_POWERSHELL_PATH}"
                 -NoProfile
                 -NonInteractive
                 -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_sak_apfs_writer_cli.ps1"
                 -CliPath "$<TARGET_FILE:sak_apfs_writer_cli>"
                 -OutputRoot "${TEST_OUTPUT_DIR}/sak_apfs_writer_cli"
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")
message(STATUS "Added APFS writer CLI self-test: test_sak_apfs_writer_cli")

add_test(NAME test_sak_hfs_writer_cli
         COMMAND "${SAK_TEST_POWERSHELL_PATH}"
                 -NoProfile
                 -NonInteractive
                 -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_sak_hfs_writer_cli.ps1"
                 -CliPath "$<TARGET_FILE:sak_hfs_writer_cli>"
                 -CertifierPath "$<TARGET_FILE:partition_filesystem_probe_certifier>"
                 -OutputRoot "${TEST_OUTPUT_DIR}/sak_hfs_writer_cli"
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")
message(STATUS "Added HFS writer CLI self-test: test_sak_hfs_writer_cli")

# test_partition_manager_panel - GUI chrome guard for AOMEI-style layout
add_executable(test_partition_manager_panel
    unit/test_partition_manager_panel.cpp
    ../src/gui/partition_manager_panel.cpp
    ../src/core/partition_manager_controller.cpp
    ../resources/icons.qrc
    ../include/sak/partition_manager_panel.h
    ../include/sak/partition_manager_controller.h
)
set_target_properties(test_partition_manager_panel PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_partition_manager_panel PRIVATE
    sak_gui sak_core sak_partition_fs sak_foundation
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
set_tests_properties(test_partition_manager_panel PROPERTIES
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)
message(STATUS "Added unit test: test_partition_manager_panel")

# test_wifi_manager_panel --- SSID injection safety in buildWindowsScript
add_executable(test_wifi_manager_panel
    unit/test_wifi_manager_panel.cpp
)
set_target_properties(test_wifi_manager_panel PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_wifi_manager_panel PRIVATE
    sak_gui sak_core sak_foundation
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
)
set_target_properties(test_file_explorer_breadcrumb PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_file_explorer_breadcrumb PRIVATE
    sak_gui
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
)
set_target_properties(test_file_explorer_properties_calc PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_file_explorer_properties_calc PRIVATE
    sak_core
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
)
set_target_properties(test_iso_analyzer PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_iso_analyzer PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_user_profile_restore_selection PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_user_profile_restore_selection PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_diagnostic_controller PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_diagnostic_controller PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_hardware_inventory_scanner PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_hardware_inventory_scanner PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_thermal_monitor PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_thermal_monitor PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_diagnostic_report_generator PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_diagnostic_report_generator PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_cpu_benchmark_worker PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_cpu_benchmark_worker PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_disk_benchmark_worker PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_disk_benchmark_worker PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_memory_benchmark_worker PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_memory_benchmark_worker PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_stress_test_worker PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_stress_test_worker PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_encryption PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_encryption PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_backup_file_codec PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_backup_file_codec PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_file_hash PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_file_hash PRIVATE
    sak_foundation
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
)
set_target_properties(test_path_utils PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_path_utils PRIVATE
    sak_foundation
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
)
set_target_properties(test_secure_memory PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_secure_memory PRIVATE
    sak_foundation
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
)
set_target_properties(test_input_validator PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_input_validator PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_decompressor_factory PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_decompressor_factory PRIVATE
    sak_partition_fs sak_foundation
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
)
set_target_properties(test_config_manager PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_config_manager PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_config_schema_versioning PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_config_schema_versioning PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_smart_file_filter PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_smart_file_filter PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_recycle_bin PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_recycle_bin PRIVATE
    sak_core
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
)
set_target_properties(test_firewall_rule_auditor PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_firewall_rule_auditor PRIVATE
    sak_core
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
)
set_target_properties(test_ethernet_config_manager PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ethernet_config_manager PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_file_scanner PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_file_scanner PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_file_explorer_types PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_file_explorer_types PRIVATE
    sak_core
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
)
set_target_properties(test_file_explorer_session_store PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_file_explorer_session_store PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_file_explorer_tag_store PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_file_explorer_tag_store PRIVATE
    sak_core
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
)
set_target_properties(test_file_explorer_item_model PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_file_explorer_item_model PRIVATE
    sak_core
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
    ../resources/icons.qrc
)
set_target_properties(test_file_management_explorer_panel PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_file_management_explorer_panel PRIVATE
    sak_gui sak_core sak_partition_fs sak_foundation
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
)
set_target_properties(test_file_management_file_system PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_file_management_file_system PRIVATE
    sak_core sak_partition_fs sak_foundation
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
)
set_target_properties(test_process_runner PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_process_runner PRIVATE
    sak_foundation
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_process_runner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_process_runner COMMAND test_process_runner
         -o "${TEST_OUTPUT_DIR}/test_process_runner.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_process_runner")

# test_windows_process_job - atomic startup containment and checked Job cleanup
add_executable(test_windows_process_job
    unit/test_windows_process_job.cpp
)
set_target_properties(test_windows_process_job PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_windows_process_job PRIVATE
    sak_foundation
    Qt6::Core Qt6::CorePrivate Qt6::Test
)
target_include_directories(test_windows_process_job PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_windows_process_job COMMAND test_windows_process_job
         -o "${TEST_OUTPUT_DIR}/test_windows_process_job.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
set_tests_properties(test_windows_process_job PROPERTIES TIMEOUT 60)
message(STATUS "Added unit test: test_windows_process_job")

# test_resource_soak - R5-G23-10 long-session handle/GDI/memory-growth soak
add_executable(test_resource_soak
    unit/test_resource_soak.cpp
)
set_target_properties(test_resource_soak PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_resource_soak PRIVATE
    sak_foundation
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
)
set_target_properties(test_ai_token_usage_tracker PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_token_usage_tracker PRIVATE
    sak_ai_core
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
)
set_target_properties(test_openai_responses_client PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_openai_responses_client PRIVATE
    sak_ai_core sak_foundation
    Qt6::Core Qt6::Test Qt6::Network ${PLATFORM_LIBS}
)
target_include_directories(test_openai_responses_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_openai_responses_client COMMAND test_openai_responses_client
         -o "${TEST_OUTPUT_DIR}/test_openai_responses_client.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_openai_responses_client")

add_executable(test_ai_openai_model_client
    unit/test_ai_openai_model_client.cpp
)
set_target_properties(test_ai_openai_model_client PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_openai_model_client PRIVATE
    sak_ai_core
    Qt6::Core Qt6::Test Qt6::Network ${PLATFORM_LIBS}
)
target_include_directories(test_ai_openai_model_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_ai_openai_model_client COMMAND test_ai_openai_model_client
         -o "${TEST_OUTPUT_DIR}/test_ai_openai_model_client.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_openai_model_client")

add_executable(test_ai_provider_registry
    unit/test_ai_provider_registry.cpp
    ../resources/ai.qrc
)
set_target_properties(test_ai_provider_registry PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_provider_registry PRIVATE
    sak_ai_core
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
)
set_target_properties(test_ai_prompt_assembler PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_prompt_assembler PRIVATE
    sak_ai_core
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
    # The panel's off-GUI-thread tool dispatch is certified against the real AiAssistantPanel
    # and the headless app-action surface it drives, linked from the product libraries.
    # Coverage scope: the read-only ops and the async workers (search, organize, duplicate
    # finding, network probes, benchmarks) run through the AsyncActionInvocation bridge. Ops that
    # need admin or would change the host -- partition apply, image flash, adapter DHCP and
    # enable/disable/rename, UWP uninstall -- are covered through their pure guards and gating and
    # are never executed. Every software.clean_leftovers item fed here is one the
    # leftover_cleanup_guard.h screen refuses, so CleanupWorker never runs and the suite has no
    # destructive side effect. Host-dependent enumerations (mail profiles, WLAN profiles, PST
    # contents) are covered by their op-layer shape, and the benchmark execution itself is
    # covered by the dedicated worker tests.
    add_executable(test_ai_assistant_panel_tool_dispatch
        unit/test_ai_assistant_panel_tool_dispatch.cpp
        ${CMAKE_SOURCE_DIR}/resources/ai.qrc
        ${CMAKE_SOURCE_DIR}/resources/icons.qrc
        # Stub the raw/image-filesystem bridge so the whole APFS/HFS+/ext driver stays out of
        # this test link: search and duplicate finding run with use_file_system_target=false,
        # so their bridge path is never taken.
        ${CMAKE_CURRENT_SOURCE_DIR}/stubs/file_management_bridge_search_stub.cpp
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
    # AUTOMOC forwards target compile definitions to moc (not the builtin _WIN32). The product
    # configures only on Windows, so register the browser-extension button slot unconditionally.
    target_compile_definitions(test_ai_assistant_panel_tool_dispatch PRIVATE
        SAK_WIN_REGISTRY_TESTS)
    target_link_libraries(test_ai_assistant_panel_tool_dispatch PRIVATE
        sak_gui sak_ai_core sak_win32mcp sak_core sak_partition_fs sak_foundation
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
    set_tests_properties(test_ai_assistant_panel_tool_dispatch PROPERTIES TIMEOUT 300)
    message(STATUS "Added unit test: test_ai_assistant_panel_tool_dispatch")
endif()

add_executable(test_ai_async_tool_runner
    unit/test_ai_async_tool_runner.cpp
)
set_target_properties(test_ai_async_tool_runner PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_async_tool_runner PRIVATE
    sak_ai_core sak_foundation
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
)
set_target_properties(test_ai_tool_health_ledger PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_tool_health_ledger PRIVATE
    sak_ai_core sak_foundation
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
)
set_target_properties(test_follow_scroll_controller PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_follow_scroll_controller PRIVATE
    sak_gui
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
)
set_target_properties(test_splash_screen PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_splash_screen PRIVATE
    sak_gui
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
    ../resources/icons.qrc
)
set_target_properties(test_ai_transcript_view PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_transcript_view PRIVATE
    sak_gui
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
)
set_target_properties(test_view_empty_state PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_view_empty_state PRIVATE
    sak_gui
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
)
set_target_properties(test_crash_reporter PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_crash_reporter PRIVATE
    sak_core
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
    fuzz/fuzz_harness.h
    support/pst_fixture.h
)
set_target_properties(test_fuzz_pst_parser PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_pst_parser PRIVATE
    sak_core sak_foundation
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
    fuzz/fuzz_harness.h
    support/pst_fixture.h
)
set_target_properties(test_fuzz_pst_structure PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_pst_structure PRIVATE
    sak_core sak_foundation
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
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_decompressor PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_decompressor PRIVATE
    sak_partition_fs sak_foundation
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
    ../include/sak/ai/ai_mcp_jsonrpc.h
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_mcp_framing PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_mcp_framing PRIVATE
    sak_win32mcp
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
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_command_classifier PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_command_classifier PRIVATE
    sak_ai_core
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
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_iso_analyzer PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_iso_analyzer PRIVATE
    sak_core sak_foundation
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
    sak_partition_fs
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
    support/byte_writer.h
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_fs_detector PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_fs_detector PRIVATE
    sak_partition_fs
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
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_install_script PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_install_script PRIVATE
    sak_core sak_foundation
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
    ../include/sak/partition_export_containment.h
    support/byte_writer.h
    support/hfs_fixture.h
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_hfs_reader PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_hfs_reader PRIVATE
    sak_partition_fs
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
    ../include/sak/partition_export_containment.h
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_apfs_reader PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_apfs_reader PRIVATE
    sak_partition_fs
    Qt6::Core Qt6::Test sak_lzfse ${PLATFORM_LIBS}
)
target_include_directories(test_fuzz_apfs_reader PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_fuzz_apfs_reader COMMAND test_fuzz_apfs_reader
         -o "${TEST_OUTPUT_DIR}/test_fuzz_apfs_reader.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
# About 3x the measured 25 s (Wave 69 serial run) so a hang fails in minutes, not at the
# 1500 s CTest default.
set_tests_properties(test_fuzz_apfs_reader PROPERTIES TIMEOUT 75)
message(STATUS "Added unit test: test_fuzz_apfs_reader")

# test_fuzz_nuget_version_range - deterministic mutation-fuzz of the NuGet version / version-range
# parsers, which consume version strings from third-party package metadata. Asserts no crash/hang,
# determinism, the fail-closed contract (an invalid range satisfies nothing), and that
# selectHighestSatisfying only returns a satisfying version.
add_executable(test_fuzz_nuget_version_range
    unit/test_fuzz_nuget_version_range.cpp
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_nuget_version_range PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_nuget_version_range PRIVATE
    sak_core
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
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_backup_codec PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_backup_codec PRIVATE
    sak_core sak_foundation
    Qt6::Core Qt6::Test ZLIB::ZLIB ${PLATFORM_LIBS}
)
target_include_directories(test_fuzz_backup_codec PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_fuzz_backup_codec COMMAND test_fuzz_backup_codec
         -o "${TEST_OUTPUT_DIR}/test_fuzz_backup_codec.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
# About 3x the measured 127 s (Wave 69 serial run). Over the 60 s T2 fuzz budget, so it is
# labelled fuzz-extended (T4) in the label block at the end of this file.
set_tests_properties(test_fuzz_backup_codec PROPERTIES TIMEOUT 380)
message(STATUS "Added unit test: test_fuzz_backup_codec")

# test_fuzz_uup_manifest_guard - deterministic mutation-fuzz of the UUP-dump aria2 manifest-entry
# guard. The UUP dump JSON API is a third-party service; its fileName/url/sha1 fields are serialized
# into an aria2 input manifest, so isSafeAria2FileEntry()/isValidSha1() are the injection boundary.
# Asserts no crash/hang, determinism, that the guard agrees with an independent oracle, and above all
# that an oracle-unsafe field is NEVER accepted. Mirrors test_uup_dump_api's link set.
add_executable(test_fuzz_uup_manifest_guard
    unit/test_fuzz_uup_manifest_guard.cpp
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_uup_manifest_guard PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_uup_manifest_guard PRIVATE
    sak_core sak_foundation
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
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_smart_report PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_smart_report PRIVATE
    sak_core sak_foundation
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
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_ai_response PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_ai_response PRIVATE
    sak_ai_core sak_foundation
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
    fuzz/fuzz_harness.h
)
set_target_properties(test_fuzz_mbox_container PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_fuzz_mbox_container PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_ai_provider_gateway PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_provider_gateway PRIVATE
    sak_ai_core sak_foundation
    Qt6::Core Qt6::CorePrivate Qt6::Network Qt6::Test
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
)
set_target_properties(test_ai_app_action_planner PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_app_action_planner PRIVATE
    sak_ai_core sak_foundation
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
)
set_target_properties(test_ai_win32_gui_runner PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_win32_gui_runner PRIVATE
    sak_ai_core
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
)
set_target_properties(test_win32_mcp_dialog_choice PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_win32_mcp_dialog_choice PRIVATE
    sak_win32mcp
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
)
set_target_properties(test_win32_mcp_text_match PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_win32_mcp_text_match PRIVATE
    sak_win32mcp
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
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_win32_mcp_key_chord.cpp")
    message(FATAL_ERROR "Missing tracked test source: unit/test_win32_mcp_key_chord.cpp")
endif()
    add_executable(test_win32_mcp_key_chord
        unit/test_win32_mcp_key_chord.cpp
    )
    set_target_properties(test_win32_mcp_key_chord PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_win32_mcp_key_chord PRIVATE
        sak_win32mcp
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_win32_mcp_key_chord PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_win32_mcp_key_chord COMMAND test_win32_mcp_key_chord
             -o "${TEST_OUTPUT_DIR}/test_win32_mcp_key_chord.txt,txt" -o -,txt
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_win32_mcp_key_chord")

# test_win32_mcp_input_plan --- deterministic coverage of the SendInput decision logic: absolute-
# coordinate mapping, virtual-screen bounds check (reject a click the OS would clamp to a desktop
# edge), mouse-button mapping, and the type_text keystroke plan (CRLF collapse, surrogate pairs).
# Windows-only (maps to Win32 MOUSEEVENTF_*/VK_* constants).
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_win32_mcp_input_plan.cpp")
    message(FATAL_ERROR "Missing tracked test source: unit/test_win32_mcp_input_plan.cpp")
endif()
    add_executable(test_win32_mcp_input_plan
        unit/test_win32_mcp_input_plan.cpp
    )
    set_target_properties(test_win32_mcp_input_plan PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_win32_mcp_input_plan PRIVATE
        sak_win32mcp
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_win32_mcp_input_plan PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_win32_mcp_input_plan COMMAND test_win32_mcp_input_plan
             -o "${TEST_OUTPUT_DIR}/test_win32_mcp_input_plan.txt,txt" -o -,txt
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_win32_mcp_input_plan")

# test_win32_mcp_uia_ref --- deterministic coverage of the UIA ref-drift guard that protects
# uia_click_control / uia_get_control_value / dismiss_dialog from acting on a stale ref: identity is
# role + name + bounding-rect origin, and out-of-range against either walk fails closed. Pure logic.
add_executable(test_win32_mcp_uia_ref
    unit/test_win32_mcp_uia_ref.cpp
)
set_target_properties(test_win32_mcp_uia_ref PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_win32_mcp_uia_ref PRIVATE
    sak_win32mcp
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
)
set_target_properties(test_win32_mcp_fingerprint PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_win32_mcp_fingerprint PRIVATE
    sak_win32mcp
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
)
set_target_properties(test_win32_mcp_json_clamp PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_win32_mcp_json_clamp PRIVATE
    sak_win32mcp
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
)
set_target_properties(test_win32_mcp_geometry PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_win32_mcp_geometry PRIVATE
    sak_win32mcp
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
)
set_target_properties(test_ai_credential_store PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_credential_store PRIVATE
    sak_ai_core sak_foundation
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
)
set_target_properties(test_ai_command_guard PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_command_guard PRIVATE
    sak_ai_core
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
)
set_target_properties(test_ai_command_tool_planner PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_command_tool_planner PRIVATE
    sak_ai_core sak_foundation
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
    ../include/sak/ai/openai_response_types.h
)
set_target_properties(test_ai_tool_call_router PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_tool_call_router PRIVATE
    sak_ai_core
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
)
set_target_properties(test_ai_mcp_http_client PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_mcp_http_client PRIVATE
    sak_ai_core
    Qt6::Core Qt6::Network Qt6::Test
)
target_include_directories(test_ai_mcp_http_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_ai_mcp_http_client COMMAND test_ai_mcp_http_client
         -o "${TEST_OUTPUT_DIR}/test_ai_mcp_http_client.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_ai_mcp_http_client")

add_executable(test_ai_mcp_stdio_session
    unit/test_ai_mcp_stdio_session.cpp
    support/ai_mcp_native_fixture.h
)
set_target_properties(test_ai_mcp_stdio_session PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_mcp_stdio_session PRIVATE
    sak_ai_core sak_foundation
    Qt6::Core Qt6::CorePrivate Qt6::Test
)
target_include_directories(test_ai_mcp_stdio_session PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_dependencies(test_ai_mcp_stdio_session sak_utility)
add_test(NAME test_ai_mcp_stdio_session COMMAND test_ai_mcp_stdio_session
         -o "${TEST_OUTPUT_DIR}/test_ai_mcp_stdio_session.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
set_tests_properties(test_ai_mcp_stdio_session PROPERTIES TIMEOUT 60)
message(STATUS "Added unit test: test_ai_mcp_stdio_session")

add_executable(test_ai_mcp_session_pool
    unit/test_ai_mcp_session_pool.cpp
    support/ai_mcp_native_fixture.h
)
set_target_properties(test_ai_mcp_session_pool PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_mcp_session_pool PRIVATE
    sak_ai_core sak_foundation
    Qt6::Core Qt6::CorePrivate Qt6::Test
)
target_include_directories(test_ai_mcp_session_pool PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_dependencies(test_ai_mcp_session_pool sak_utility)
add_test(NAME test_ai_mcp_session_pool COMMAND test_ai_mcp_session_pool
         -o "${TEST_OUTPUT_DIR}/test_ai_mcp_session_pool.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
set_tests_properties(test_ai_mcp_session_pool PROPERTIES TIMEOUT 60)
message(STATUS "Added unit test: test_ai_mcp_session_pool")

add_executable(test_app_action_registry
    unit/test_app_action_registry.cpp
)
set_target_properties(test_app_action_registry PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_app_action_registry PRIVATE
    sak_core
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
)
set_target_properties(test_app_action_service PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_app_action_service PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_ai_conversation_store PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_conversation_store PRIVATE
    sak_ai_core sak_foundation
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
)
set_target_properties(test_ai_chat_title PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_chat_title PRIVATE
    sak_ai_core
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
)
set_target_properties(test_ai_execution_broker PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_execution_broker PRIVATE
    sak_ai_core sak_foundation
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
)
set_target_properties(test_ai_cancellation_token PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_cancellation_token PRIVATE
    sak_ai_core
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
)
set_target_properties(test_ai_run_state PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_run_state PRIVATE
    sak_ai_core
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
)
set_target_properties(test_ai_human_gate_store PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_human_gate_store PRIVATE
    sak_ai_core
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
)
set_target_properties(test_ai_recovery_policy PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_recovery_policy PRIVATE
    sak_ai_core
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
)
set_target_properties(test_ai_package_selection PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_package_selection PRIVATE
    sak_ai_core
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
)
set_target_properties(test_ai_workflow_clarifier PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_workflow_clarifier PRIVATE
    sak_ai_core
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
)
set_target_properties(test_ai_trace_store PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_trace_store PRIVATE
    sak_ai_core sak_foundation
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
)
set_target_properties(test_ai_tool_policy PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_tool_policy PRIVATE
    sak_ai_core
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
    ${CMAKE_SOURCE_DIR}/resources/ai.qrc
    fixtures/ai_store_crlf/ai_store_crlf.qrc
)
set_target_properties(test_ai_skill_store PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_skill_store PRIVATE
    sak_ai_core sak_foundation
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
)
set_target_properties(test_ai_lease_manager PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_lease_manager PRIVATE
    sak_ai_core
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
)
set_target_properties(test_ai_subagent_tool_executor PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_subagent_tool_executor PRIVATE
    sak_ai_core sak_foundation
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
)
set_target_properties(test_ai_tool_loop_detector PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_tool_loop_detector PRIVATE
    sak_ai_core
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
)
set_target_properties(test_ai_package_tool_planner PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_package_tool_planner PRIVATE
    sak_ai_core
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
)
set_target_properties(test_ai_offline_downloader_tool_runner PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_offline_downloader_tool_runner PRIVATE
    sak_ai_core
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
)
set_target_properties(test_ai_workflow_powershell_tool_runner PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_workflow_powershell_tool_runner PRIVATE
    sak_ai_core sak_foundation
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
)
set_target_properties(test_ai_tool_dispatcher PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_tool_dispatcher PRIVATE
    sak_ai_core sak_foundation
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
    ../include/sak/ai/openai_response_types.h
)
set_target_properties(test_ai_tool_turn PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_tool_turn PRIVATE
    sak_ai_core
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
)
set_target_properties(test_ai_tool_result_recorder PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_tool_result_recorder PRIVATE
    sak_ai_core sak_foundation
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
)
set_target_properties(test_ai_run_state_store PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_run_state_store PRIVATE
    sak_ai_core
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
)
set_target_properties(test_ai_subagent_runner PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_subagent_runner PRIVATE
    sak_ai_core
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
)

add_executable(test_ai_workflow_evals
    unit/test_ai_workflow_evals.cpp
)
set_target_properties(test_ai_workflow_evals PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_workflow_evals PRIVATE sak_ai_core Qt6::Core Qt6::Test)
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
    sak_ai_core
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
    ../resources/ai.qrc
    fixtures/ai_store_crlf/ai_store_crlf.qrc
)
set_target_properties(test_ai_workflow_store PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ai_workflow_store PRIVATE
    sak_ai_core sak_foundation
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
)
set_target_properties(test_smart_disk_analyzer PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_smart_disk_analyzer PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_package_matcher PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_package_matcher PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_drive_scanner PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_drive_scanner PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_permission_manager PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_permission_manager PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_elevation_manager PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_elevation_manager PRIVATE
    sak_foundation
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
)
set_target_properties(test_elevated_task_dispatcher PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_elevated_task_dispatcher PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_mixed_tier_operations PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_mixed_tier_operations PRIVATE
    sak_core sak_foundation
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
    sak_foundation
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
    ../include/sak/elevation_tier.h
    ../include/sak/elevation_gate.h
    ../include/sak/elevated_pipe_protocol.h
)
set_target_properties(test_elevation_hardening PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_elevation_hardening PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_streaming_decompressor PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_streaming_decompressor PRIVATE
    sak_partition_fs sak_foundation
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
)
set_target_properties(test_logger PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_logger PRIVATE
    sak_foundation
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
)
set_target_properties(test_install_script_parser PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_install_script_parser PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_script_rewriter PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_script_rewriter PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_package_list_manager PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_package_list_manager PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_vulnerability_scanner PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_vulnerability_scanner PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_nuget_version_range PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_nuget_version_range PRIVATE
    sak_core
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
)
set_target_properties(test_nuget_dependency_resolver PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_nuget_dependency_resolver PRIVATE
    sak_core
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
)
set_target_properties(test_offline_package_search PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_offline_package_search PRIVATE
    sak_core sak_foundation
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
)

set_target_properties(test_action_factory PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)

target_link_libraries(test_action_factory PRIVATE
    sak_core sak_foundation
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
)

set_target_properties(test_backup_bitlocker_keys_action PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)

target_link_libraries(test_backup_bitlocker_keys_action PRIVATE
    sak_core sak_foundation
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
)

set_target_properties(test_verify_system_files_action PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)

target_link_libraries(test_verify_system_files_action PRIVATE
    sak_core sak_foundation
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
)

set_target_properties(test_check_disk_errors_action PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)

target_link_libraries(test_check_disk_errors_action PRIVATE
    sak_core sak_foundation
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
)

set_target_properties(test_reset_network_action PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)

target_link_libraries(test_reset_network_action PRIVATE
    sak_core sak_foundation
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
)

set_target_properties(test_generate_system_report_action PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)

target_link_libraries(test_generate_system_report_action PRIVATE
    sak_core sak_foundation
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
)

set_target_properties(test_screenshot_settings_action PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)

target_link_libraries(test_screenshot_settings_action PRIVATE
    sak_core sak_foundation
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
)

set_target_properties(test_optimize_power_settings_action PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)

target_link_libraries(test_optimize_power_settings_action PRIVATE
    sak_core sak_foundation
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
)

set_target_properties(test_file_explorer_archive_service PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)

target_link_libraries(test_file_explorer_archive_service PRIVATE
    sak_core
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
    ../resources/icons.qrc
)
set_target_properties(test_image_flasher_panel PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_image_flasher_panel PRIVATE
    sak_gui sak_core sak_partition_fs sak_foundation
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
)
set_target_properties(test_quick_action PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_quick_action PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_user_profile_types PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_user_profile_types PRIVATE
    sak_foundation
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
)
set_target_properties(test_user_data_manager PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_user_data_manager PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_duplicate_finder_worker PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_duplicate_finder_worker PRIVATE
    sak_core sak_partition_fs sak_foundation
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
)
set_target_properties(test_organizer_worker PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_organizer_worker PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_app_installation_worker PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_app_installation_worker PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_app_installation_busy PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_app_installation_busy PRIVATE
    sak_core
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
)
set_target_properties(test_user_profile_restore_worker PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_user_profile_restore_worker PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_windows_usb_creator PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_windows_usb_creator PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_flash_worker PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_flash_worker PRIVATE
    sak_core sak_partition_fs sak_foundation
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
)
set_target_properties(test_quick_action_result_io PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_quick_action_result_io PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_keep_awake PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_keep_awake PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_quick_action_controller PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_quick_action_controller PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_migration_report PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_migration_report PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_linux_distro_catalog PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_linux_distro_catalog PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_flash_coordinator PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_flash_coordinator PRIVATE
    sak_core sak_partition_fs sak_foundation
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
)
set_target_properties(test_user_profile_backup_worker PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_user_profile_backup_worker PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_uup_dump_api PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_uup_dump_api PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_uup_iso_builder PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_uup_iso_builder PRIVATE
    sak_core sak_foundation
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
)

set_target_properties(test_all_actions_metadata PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_all_actions_metadata PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_regex_pattern_library PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_regex_pattern_library PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_advanced_search_worker PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_advanced_search_worker PRIVATE
    sak_core sak_partition_fs sak_foundation
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
)
set_target_properties(test_advanced_search_controller PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_advanced_search_controller PRIVATE
    sak_core sak_partition_fs sak_foundation
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
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_backup_destination_guard.cpp")
    message(FATAL_ERROR "Missing tracked test source: unit/test_backup_destination_guard.cpp")
endif()
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
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_win32_mcp_server.cpp")
    message(FATAL_ERROR "Missing tracked test source: unit/test_win32_mcp_server.cpp")
endif()
    # The dispatch now references BrowserControl (browser_* tools merged into
    # tools/list and routed on tools/call), so the server test links the browser
    # facade and its transitive transport/session/contract deps.
    add_executable(test_win32_mcp_server
        unit/test_win32_mcp_server.cpp
    )
    set_target_properties(test_win32_mcp_server PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_win32_mcp_server PRIVATE
        sak_win32mcp
        Qt6::Core Qt6::Gui Qt6::Test ${PLATFORM_LIBS} shcore gdi32 user32 runtimeobject
    )
    target_include_directories(test_win32_mcp_server PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_win32_mcp_server COMMAND test_win32_mcp_server
             -o "${TEST_OUTPUT_DIR}/test_win32_mcp_server.txt,txt" -o -,txt
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_win32_mcp_server")

# test_browser_contract --- pure DOM-snapshot renderer + browser tool catalog + tool->extension
# command translation. OS-agnostic (Qt Core only, no windows.h), so it builds and runs on any host.
add_executable(test_browser_contract
    unit/test_browser_contract.cpp
)
set_target_properties(test_browser_contract PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_browser_contract PRIVATE
    sak_win32mcp
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
    ${CMAKE_SOURCE_DIR}/src/win32mcp/win32_mcp_native_host.cpp
    ${CMAKE_SOURCE_DIR}/include/sak/win32mcp/win32_mcp_native_host.h
)
set_target_properties(test_native_messaging PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_native_messaging PRIVATE
    sak_win32mcp
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
)
set_target_properties(test_browser_bridge PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_browser_bridge PRIVATE
    sak_win32mcp
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
)
set_target_properties(test_browser_extension_installer PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_browser_extension_installer PRIVATE
    sak_win32mcp
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_browser_extension_installer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
# AUTOMOC forwards target compile definitions to moc (unlike the builtin _WIN32). The product
# configures only on Windows, so register the registry slots unconditionally.
target_compile_definitions(test_browser_extension_installer PRIVATE SAK_WIN_REGISTRY_TESTS)
add_test(NAME test_browser_extension_installer COMMAND test_browser_extension_installer
         -o "${TEST_OUTPUT_DIR}/test_browser_extension_installer.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_browser_extension_installer")

# test_browser_bridge_security --- the load-bearing pipe security + discovery primitives
# (current-user-SID DACL + Medium NO_WRITE_UP label, per-user/session nonce pipe name,
# rendezvous record). Windows only (Win32 SID/SDDL APIs); links advapi32 via PLATFORM_LIBS.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_browser_bridge_security.cpp")
    message(FATAL_ERROR "Missing tracked test source: unit/test_browser_bridge_security.cpp")
endif()
    add_executable(test_browser_bridge_security
        unit/test_browser_bridge_security.cpp
    )
    set_target_properties(test_browser_bridge_security PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_browser_bridge_security PRIVATE
        sak_win32mcp
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_browser_bridge_security PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_browser_bridge_security COMMAND test_browser_bridge_security
             -o "${TEST_OUTPUT_DIR}/test_browser_bridge_security.txt,txt" -o -,txt
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_browser_bridge_security")

# test_browser_bridge_pipe --- the bridge pipe SERVER (hardened create, accept thread,
# overlapped IO + deadline, hello/welcome handshake, peer verify) exercised end-to-end
# by an in-process client. Windows only; links the security + native-messaging codecs.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_browser_bridge_pipe.cpp")
    message(FATAL_ERROR "Missing tracked test source: unit/test_browser_bridge_pipe.cpp")
endif()
    add_executable(test_browser_bridge_pipe
        unit/test_browser_bridge_pipe.cpp
    )
    set_target_properties(test_browser_bridge_pipe PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_browser_bridge_pipe PRIVATE
        sak_win32mcp
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_browser_bridge_pipe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_browser_bridge_pipe COMMAND test_browser_bridge_pipe
             -o "${TEST_OUTPUT_DIR}/test_browser_bridge_pipe.txt,txt" -o -,txt
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_browser_bridge_pipe")

# test_browser_bridge_relay --- the Chrome-side RELAY (rendezvous discovery + server
# code-identity verify + hello/welcome handshake + id-transparent frame pump) and the
# MCP-side BrowserControl facade, exercised end-to-end against a real in-process pipe
# server with a fake extension. Windows only; links relay + control + transport +
# session + contract.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_browser_bridge_relay.cpp")
    message(FATAL_ERROR "Missing tracked test source: unit/test_browser_bridge_relay.cpp")
endif()
    add_executable(test_browser_bridge_relay
        unit/test_browser_bridge_relay.cpp
    )
    set_target_properties(test_browser_bridge_relay PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
    )
    target_link_libraries(test_browser_bridge_relay PRIVATE
        sak_win32mcp
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_browser_bridge_relay PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_browser_bridge_relay COMMAND test_browser_bridge_relay
             -o "${TEST_OUTPUT_DIR}/test_browser_bridge_relay.txt,txt" -o -,txt
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_browser_bridge_relay")

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
)
set_target_properties(test_windows_user_scanner PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_windows_user_scanner PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_app_scanner PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_app_scanner PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_chocolatey_manager PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_chocolatey_manager PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_cleanup_worker PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_cleanup_worker PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_advanced_uninstall_controller PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_advanced_uninstall_controller PRIVATE
    sak_core sak_foundation
    Qt6::Core Qt6::Gui Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_advanced_uninstall_controller PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_advanced_uninstall_controller COMMAND test_advanced_uninstall_controller
         -o "${TEST_OUTPUT_DIR}/test_advanced_uninstall_controller.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_advanced_uninstall_controller")

# Source-inspection tests read committed source through the configured source root.
# Executable-relative ../.. breaks in nested build trees and can silently read build/src.
foreach(_sak_source_root_test IN ITEMS
        test_partition_manager_panel
        test_process_runner
        test_uup_dump_api
        test_cleanup_worker
        test_advanced_uninstall_controller
        test_browser_extension_installer
        test_windows_usb_creator)
    target_compile_definitions(${_sak_source_root_test} PRIVATE
        SAK_REPO_ROOT="${CMAKE_SOURCE_DIR}")
endforeach()

# test_leftover_scanner - pattern building, risk classification, scan levels, cancellation
add_executable(test_leftover_scanner
    unit/test_leftover_scanner.cpp
)
set_target_properties(test_leftover_scanner PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_leftover_scanner PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_program_enumerator PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_program_enumerator PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_registry_snapshot_engine PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_registry_snapshot_engine PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_network_diagnostic_utils PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_network_diagnostic_utils PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_network_share_browser PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_network_share_browser PRIVATE
    sak_core
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
)
set_target_properties(test_active_connections_monitor PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_active_connections_monitor PRIVATE
    sak_core
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
)
set_target_properties(test_bundled_tools_manager PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_bundled_tools_manager PRIVATE
    sak_foundation
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
)
set_target_properties(test_drive_unmounter PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_drive_unmounter PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_image_source PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_image_source PRIVATE
    sak_core sak_partition_fs sak_foundation
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
)
set_target_properties(test_network_adapter_inspector PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_network_adapter_inspector PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_network_diagnostic_controller PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_network_diagnostic_controller PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_port_scanner PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_port_scanner PRIVATE
    sak_core
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
)
set_target_properties(test_uninstall_worker PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
# Qt6::Gui is REQUIRED, not incidental: advanced_uninstall_types.h includes <QImage> for the
# program icon, so ProgramInfo cannot compile without it. Mirrors test_advanced_uninstall_-
# controller, which links the same sources. QGuiApplication opens no window here.
target_link_libraries(test_uninstall_worker PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_wifi_analyzer PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_wifi_analyzer PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_bandwidth_tester PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_bandwidth_tester PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_connectivity_tester PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_connectivity_tester PRIVATE
    sak_core
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
)
set_target_properties(test_dns_diagnostic_tool PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_dns_diagnostic_tool PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_network_diagnostic_report PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_network_diagnostic_report PRIVATE
    sak_core
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
# test_csv_escape --- the single CSV cell writer. Four writers existed and two of them carried no
# formula guard, on paths that export externally-sourced text (advisory feeds, WiFi SSIDs).
# Header-only and pure, so this needs no production source.
add_executable(test_csv_escape
    unit/test_csv_escape.cpp
    ../include/sak/csv_escape.h
)
set_target_properties(test_csv_escape PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_csv_escape PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_csv_escape PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_csv_escape COMMAND test_csv_escape
         -o "${TEST_OUTPUT_DIR}/test_csv_escape.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_csv_escape")

# test_windows_path_policy --- the shared screening dialect in windows_path_policy.h. It gates two
# real authorities and had NO direct unit test: literalLocalPathTrusted screens a registry string
# any user can write under HKCU before the uninstaller launches it ELEVATED or deletes a directory
# RECURSIVELY, and isSafeChildName screens a name about to become a file or folder in a chosen
# directory. Header-only and pure, so this needs no production source.
add_executable(test_windows_path_policy
    unit/test_windows_path_policy.cpp
    ../include/sak/windows_path_policy.h
)
set_target_properties(test_windows_path_policy PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_windows_path_policy PRIVATE
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_windows_path_policy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_windows_path_policy COMMAND test_windows_path_policy
         -o "${TEST_OUTPUT_DIR}/test_windows_path_policy.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_windows_path_policy")

# test_network_adapter_admin --- the pure half of enable/disable/rename (R5-IDX-19c):
# new-name validation, resolution against the system's own adapter list, and the exact netsh
# argument vectors. No adapter is touched, so this runs anywhere.
add_executable(test_network_adapter_admin
    unit/test_network_adapter_admin.cpp
)
set_target_properties(test_network_adapter_admin PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_network_adapter_admin PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_wifi_profile_scanner PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_wifi_profile_scanner PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_pst_parser PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_pst_parser PRIVATE
    sak_core sak_foundation
    Qt6::Core Qt6::Gui Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_pst_parser PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_pst_parser COMMAND test_pst_parser
         -o "${TEST_OUTPUT_DIR}/test_pst_parser.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_pst_parser")

# test_ost_integration - opens generated OST/PST fixtures through full parse/export pipeline
add_executable(test_ost_integration
    integration/test_ost_integration.cpp
)
set_target_properties(test_ost_integration PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ost_integration PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_mbox_parser PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_mbox_parser PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_email_search_worker PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_email_search_worker PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_email_export_worker PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_email_export_worker PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_email_profile_manager PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_email_profile_manager PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_wifi_setup_script PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_wifi_setup_script PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_email_attachment_saver PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_email_attachment_saver PRIVATE
    sak_core sak_foundation
    Qt6::Core Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_email_attachment_saver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_email_attachment_saver COMMAND test_email_attachment_saver
         -o "${TEST_OUTPUT_DIR}/test_email_attachment_saver.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_email_attachment_saver")

# test_email_attachments_browser_dialog --- exact folder/item failure correlation and save errors
add_executable(test_email_attachments_browser_dialog
    unit/test_email_attachments_browser_dialog.cpp
)
set_target_properties(test_email_attachments_browser_dialog PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_email_attachments_browser_dialog PRIVATE
    sak_gui sak_core sak_foundation
    Qt6::Core Qt6::Concurrent Qt6::Gui Qt6::Widgets Qt6::Test ${PLATFORM_LIBS}
)
target_include_directories(test_email_attachments_browser_dialog PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME test_email_attachments_browser_dialog COMMAND test_email_attachments_browser_dialog
         -o "${TEST_OUTPUT_DIR}/test_email_attachments_browser_dialog.txt,txt" -o -,txt
         WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
message(STATUS "Added unit test: test_email_attachments_browser_dialog")

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
)
set_target_properties(test_email_report_generator PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_email_report_generator PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_package_internalization_engine PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_package_internalization_engine PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_offline_deployment_worker PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_offline_deployment_worker PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_offline_package_builder PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_offline_package_builder PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_email_folder_selection PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_email_folder_selection PRIVATE
    sak_core
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
    ../include/sak/email_types.h
)
set_target_properties(test_eml_writer PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_eml_writer PRIVATE
    sak_core sak_foundation
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
)
set_target_properties(test_ost_converter_controller PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_ost_converter_controller PRIVATE
    sak_core sak_foundation
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
    ../include/sak/email_types.h
)
set_target_properties(test_mbox_writer PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_mbox_writer PRIVATE
    sak_core sak_foundation
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
    ../include/sak/email_types.h
)
set_target_properties(test_html_email_writer PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_html_email_writer PRIVATE
    sak_core sak_foundation
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
    ../include/sak/email_types.h
)
set_target_properties(test_pdf_email_writer PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_pdf_email_writer PRIVATE
    sak_core sak_foundation
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
    ../include/sak/ost_converter_types.h
    ../include/sak/email_types.h
)
set_target_properties(test_conversion_report_generator PROPERTIES
    AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SAK_MAIN_OUTPUT_DIR}"
)
target_link_libraries(test_conversion_report_generator PRIVATE
    sak_core sak_foundation
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
        partition_manager_live_certifier
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
    add_test(NAME test_partition_manager_live_certifier
             COMMAND "${SAK_PWSH_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_SOURCE_DIR}/scripts/test_partition_manager_live_certifier.ps1"
                     -CertifierPath "$<TARGET_FILE:partition_manager_live_certifier>"
                     -OutputRoot "${TEST_OUTPUT_DIR}/partition_manager_live_certifier")
    set_tests_properties(test_partition_manager_live_certifier PROPERTIES TIMEOUT 120)

    add_test(NAME test_partition_manager_controller_evidence
             COMMAND "${SAK_PWSH_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_partition_manager_controller_evidence.ps1")
    set_tests_properties(test_partition_manager_controller_evidence PROPERTIES TIMEOUT 120)

    add_test(NAME test_partition_manager_controller_restore_routing
             COMMAND "${SAK_PWSH_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_SOURCE_DIR}/scripts/test_partition_manager_controller_restore_routing.ps1"
                     -RunGuardDrills)
    set_tests_properties(test_partition_manager_controller_restore_routing PROPERTIES TIMEOUT 60)

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
    # -OutputRoot is explicit in the build tree. Its default also stays under
    # build/test_results so standalone self-tests cannot mix synthetic fixtures
    # with captured certification evidence under artifacts/.
    add_test(NAME test_partition_manager_certification_tools
             COMMAND "${SAK_PWSH_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_partition_manager_certification_tools.ps1"
                     -OutputRoot "${TEST_OUTPUT_DIR}/partition-manager-certification-tools")
    set_tests_properties(test_partition_manager_certification_tools PROPERTIES TIMEOUT 600)

    # Start-Process with redirected output can leave ExitCode null under Windows PowerShell
    # 5.1, and sequential pipe reads can deadlock when both streams fill. This hermetic test
    # drives the shared ProcessStartInfo seam through exit 0/7, large dual-stream output,
    # argument quoting, and a bounded timeout. It also pins the named certification runners to
    # that seam and rejects their old scoped capture blocks. Keep each bounded group separate
    # under pwsh and Windows PowerShell 5.1: the old aggregate crossed its 60-second deadline
    # at different groups on repeated runs, hiding the remaining assertions.
    add_test(NAME test_process_capture
             COMMAND "${SAK_PWSH_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_process_capture.ps1"
                     -CoreOnly)
    set_tests_properties(test_process_capture PROPERTIES TIMEOUT 60)

    # CTest is a native parent: unlike a direct PowerShell-to-PowerShell launch, it retains
    # the caller's PowerShell 7 module path. Clear only that inherited variable at each
    # Windows PowerShell launch so 5.1 initializes its own standard-module search paths.
    set(SAK_WINDOWS_POWERSHELL_EXECUTABLE
        "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/powershell.exe")
    if(NOT EXISTS "${SAK_WINDOWS_POWERSHELL_EXECUTABLE}")
        message(FATAL_ERROR
                "Windows PowerShell 5.1 was not found, so its redirected-process exit-code "
                "regression test cannot run.")
    endif()
    add_test(NAME test_process_capture_windows_powershell
             COMMAND "${CMAKE_COMMAND}" -E env --unset=PSModulePath
                     "${SAK_WINDOWS_POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_process_capture.ps1"
                     -CoreOnly)
    set_tests_properties(test_process_capture_windows_powershell PROPERTIES TIMEOUT 60)

    add_test(NAME test_process_capture_ownership
             COMMAND "${SAK_PWSH_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_process_capture.ps1"
                     -OwnershipOnly)
    add_test(NAME test_process_capture_ownership_windows_powershell
             COMMAND "${CMAKE_COMMAND}" -E env --unset=PSModulePath
                     "${SAK_WINDOWS_POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_process_capture.ps1"
                     -OwnershipOnly)
    add_test(NAME test_process_capture_deadline
             COMMAND "${SAK_PWSH_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_process_capture.ps1"
                     -DeadlineOnly)
    add_test(NAME test_process_capture_deadline_windows_powershell
             COMMAND "${CMAKE_COMMAND}" -E env --unset=PSModulePath
                     "${SAK_WINDOWS_POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_process_capture.ps1"
                     -DeadlineOnly)
    add_test(NAME test_process_capture_output
             COMMAND "${SAK_PWSH_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_process_capture.ps1"
                     -OutputOnly)
    add_test(NAME test_process_capture_output_windows_powershell
             COMMAND "${CMAKE_COMMAND}" -E env --unset=PSModulePath
                     "${SAK_WINDOWS_POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_process_capture.ps1"
                     -OutputOnly)
    add_test(NAME test_process_capture_stdin
             COMMAND "${SAK_PWSH_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_process_capture.ps1"
                     -StdinOnly)
    add_test(NAME test_process_capture_stdin_windows_powershell
             COMMAND "${CMAKE_COMMAND}" -E env --unset=PSModulePath
                     "${SAK_WINDOWS_POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_process_capture.ps1"
                     -StdinOnly)
    add_test(NAME test_process_capture_environment
             COMMAND "${SAK_PWSH_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_process_capture.ps1"
                     -EnvironmentOnly)
    add_test(NAME test_process_capture_environment_windows_powershell
             COMMAND "${CMAKE_COMMAND}" -E env --unset=PSModulePath
                     "${SAK_WINDOWS_POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_process_capture.ps1"
                     -EnvironmentOnly)
    set_tests_properties(
        test_process_capture_ownership
        test_process_capture_ownership_windows_powershell
        test_process_capture_deadline
        test_process_capture_deadline_windows_powershell
        test_process_capture_output
        test_process_capture_output_windows_powershell
        test_process_capture_stdin
        test_process_capture_stdin_windows_powershell
        test_process_capture_environment
        test_process_capture_environment_windows_powershell
        PROPERTIES TIMEOUT 60)

    # Coverage reports are evidence only when every child/tool succeeds and every output is fresh.
    # Fake native tools make each refusal deterministic without needing an instrumented build.
    add_test(NAME test_coverage_runners
             COMMAND "${SAK_PWSH_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_coverage_runners.ps1")
    set_tests_properties(test_coverage_runners PROPERTIES TIMEOUT 300)

    add_test(NAME test_coverage_runners_windows_powershell
             COMMAND "${CMAKE_COMMAND}" -E env --unset=PSModulePath
                     "${SAK_WINDOWS_POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_coverage_runners.ps1")
    set_tests_properties(test_coverage_runners_windows_powershell PROPERTIES TIMEOUT 300)

    add_test(NAME test_new_partition_manager_automation_vm_security
             COMMAND "${SAK_PWSH_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_new_partition_manager_automation_vm_security.ps1")
    set_tests_properties(test_new_partition_manager_automation_vm_security PROPERTIES TIMEOUT 120)

    add_test(NAME test_new_partition_manager_automation_vm_security_windows_powershell
             COMMAND "${CMAKE_COMMAND}" -E env --unset=PSModulePath
                     "${SAK_WINDOWS_POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_new_partition_manager_automation_vm_security.ps1")
    set_tests_properties(
        test_new_partition_manager_automation_vm_security_windows_powershell PROPERTIES TIMEOUT 120)

    add_test(NAME test_false_green_command_propagation
             COMMAND "${SAK_PWSH_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_false_green_command_propagation.ps1")
    set_tests_properties(test_false_green_command_propagation PROPERTIES TIMEOUT 60)

    add_test(NAME test_false_green_command_propagation_windows_powershell
             COMMAND "${CMAKE_COMMAND}" -E env --unset=PSModulePath
                     "${SAK_WINDOWS_POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_false_green_command_propagation.ps1")
    set_tests_properties(
        test_false_green_command_propagation_windows_powershell PROPERTIES TIMEOUT 60)

    add_test(NAME test_startup_budget
             COMMAND "${SAK_PWSH_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_startup_budget.ps1")
    set_tests_properties(test_startup_budget PROPERTIES TIMEOUT 180)

    add_test(NAME test_windows_runtime_dependencies
             COMMAND "${SAK_PWSH_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_windows_runtime_dependencies.ps1")
    set_tests_properties(test_windows_runtime_dependencies PROPERTIES TIMEOUT 120)

    # scripts/run_folder_retention.ps1 deletes the run folders of passing certifier and CLI
    # image tests (they once held over 1 TB under build/test_results). It deletes directories,
    # so its fences are tested under both hosts: they delete through different .NET runtimes.
    add_test(NAME test_run_folder_retention
             COMMAND "${SAK_PWSH_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_run_folder_retention.ps1")
    add_test(NAME test_run_folder_retention_windows_powershell
             COMMAND "${CMAKE_COMMAND}" -E env --unset=PSModulePath
                     "${SAK_WINDOWS_POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_run_folder_retention.ps1")
    set_tests_properties(test_run_folder_retention test_run_folder_retention_windows_powershell
                         PROPERTIES TIMEOUT 60)

    add_test(NAME test_stage_portable_release
             COMMAND "${SAK_PWSH_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_stage_portable_release.ps1"
                     -BuildDirectory "${CMAKE_BINARY_DIR}" -SkipArchiveContracts)
    set_tests_properties(test_stage_portable_release PROPERTIES TIMEOUT 240)

    add_test(NAME test_release_archive_recovery
             COMMAND "${SAK_PWSH_EXECUTABLE}" -NoProfile -NonInteractive
                     -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test_stage_portable_release.ps1"
                     -BuildDirectory "${CMAKE_BINARY_DIR}" -ArchiveOnly)
    set_tests_properties(test_release_archive_recovery PROPERTIES TIMEOUT 180)

    find_program(SAK_PYTHON_EXECUTABLE NAMES python3 python)
    if(NOT SAK_PYTHON_EXECUTABLE)
        message(FATAL_ERROR
                "Python was not found, so the lizard JavaScript-baseline gate test cannot run.")
    endif()
    add_test(NAME test_run_lizard_js_baseline
             COMMAND "${SAK_PYTHON_EXECUTABLE}"
                     "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_run_lizard_js_baseline.py")
    set_tests_properties(test_run_lizard_js_baseline PROPERTIES TIMEOUT 60)

    add_test(NAME test_mutation_runner
             COMMAND "${SAK_PYTHON_EXECUTABLE}"
                     "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_mutation_runner.py")
    set_tests_properties(test_mutation_runner PROPERTIES TIMEOUT 60)

    add_test(NAME test_source_fingerprint
             COMMAND "${SAK_PYTHON_EXECUTABLE}"
                     "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_source_fingerprint.py")
    set_tests_properties(test_source_fingerprint PROPERTIES TIMEOUT 180)

    add_test(NAME test_cppcheck_qt_slot_classifier
             COMMAND "${SAK_PYTHON_EXECUTABLE}"
                     "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_cppcheck_qt_slot_classifier.py")
    set_tests_properties(test_cppcheck_qt_slot_classifier PROPERTIES TIMEOUT 60)

    add_test(NAME test_clang_tidy_naming_gate
             COMMAND "${SAK_PYTHON_EXECUTABLE}"
                     "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_clang_tidy_naming_gate.py")
    set_tests_properties(test_clang_tidy_naming_gate PROPERTIES TIMEOUT 60)

    add_test(NAME test_apple_filesystem_capability_matrix
             COMMAND "${SAK_PYTHON_EXECUTABLE}"
                     "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_apple_filesystem_capability_matrix.py")
    set_tests_properties(test_apple_filesystem_capability_matrix PROPERTIES TIMEOUT 60)
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

# ProcessRunner now owns one atomic Windows Job. Every target compiling its
# implementation needs the same compiled Job owner; direct Job-source targets
# already provide it and must not link a second copy. Derive this from actual
# target sources so new consumers cannot silently omit containment at link time.
# Tests take ProcessRunner from sak_foundation, which archives the Job with it, so
# this only acts on a target that compiles process_runner.cpp itself.
get_property(_sak_runner_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
set(_sak_runner_job_link_count 0)
foreach(_sak_runner_target IN LISTS _sak_runner_targets)
    get_target_property(_sak_runner_sources "${_sak_runner_target}" SOURCES)
    if(NOT _sak_runner_sources)
        continue()
    endif()
    set(_sak_has_runner_source FALSE)
    set(_sak_has_direct_job_source FALSE)
    foreach(_sak_runner_source IN LISTS _sak_runner_sources)
        get_filename_component(_sak_runner_basename "${_sak_runner_source}" NAME)
        if(_sak_runner_basename STREQUAL "process_runner.cpp")
            set(_sak_has_runner_source TRUE)
        elseif(_sak_runner_basename STREQUAL "windows_process_job.cpp")
            set(_sak_has_direct_job_source TRUE)
        endif()
    endforeach()
    if(_sak_has_runner_source AND NOT _sak_has_direct_job_source)
        target_link_libraries("${_sak_runner_target}" PRIVATE sak_windows_process_job)
        math(EXPR _sak_runner_job_link_count "${_sak_runner_job_link_count} + 1")
    endif()
endforeach()
message(STATUS "ProcessRunner Windows Job linked to ${_sak_runner_job_link_count} test targets")

# ============================================================================
# CTest labels, resource locks and serial runs (owner ruling 2026-10-24)
# ============================================================================
#
# Every registered test carries exactly one tier label; tests/README.md maps each label to its
# tier (AGENTS.md "The gate"). A native QtTest executable defaults to `product`. A script test
# must be listed by name below, and configure FAILS for a test that is neither a native test
# target nor listed, and for a listed name that is not registered: a script test that silently
# lands in a tier nobody runs is the dead-test failure the test-registration gate exists to stop.
set(_sak_label_lists
    "product:_sak_labels_product"
    "release-tooling:_sak_labels_release_tooling"
    "tooling:_sak_labels_tooling"
    "cert:_sak_labels_cert"
    "tooling-extended:_sak_labels_tooling_extended"
    "mutation:_sak_labels_mutation"
    "coverage:_sak_labels_coverage"
    "tidy:_sak_labels_tidy"
    "cert-tooling:_sak_labels_cert_tooling"
    "fuzz-extended:_sak_labels_fuzz_extended")
# Product scripts and the per-row registrations of one native executable.
set(_sak_labels_product
    test_browser_extension_pure test_pack_extension test_register_native_host
    test_sak_apfs_writer_cli test_sak_hfs_writer_cli
    test_partition_image_volume_lock_whole_disk
    test_partition_image_volume_lock_selected_partition
    test_partition_image_volume_lock_selected_mbr_unpinned_guid
    test_partition_image_volume_lock_offline_whole_disk
    test_partition_image_volume_lock_offline_selected_partition)
set(_sak_labels_release_tooling test_startup_budget test_windows_runtime_dependencies)
set(_sak_labels_tooling
    test_process_capture test_process_capture_windows_powershell
    test_run_lizard_js_baseline test_cppcheck_qt_slot_classifier
    test_run_folder_retention test_run_folder_retention_windows_powershell)
set(_sak_labels_cert test_apple_filesystem_capability_matrix)
set(_sak_labels_tooling_extended
    test_process_capture_ownership test_process_capture_ownership_windows_powershell
    test_process_capture_deadline test_process_capture_deadline_windows_powershell
    test_process_capture_output test_process_capture_output_windows_powershell
    test_process_capture_stdin test_process_capture_stdin_windows_powershell
    test_process_capture_environment test_process_capture_environment_windows_powershell
    test_false_green_command_propagation test_false_green_command_propagation_windows_powershell
    test_stage_portable_release test_release_archive_recovery)
set(_sak_labels_mutation test_mutation_runner)
set(_sak_labels_coverage test_coverage_runners test_coverage_runners_windows_powershell)
set(_sak_labels_tidy test_clang_tidy_naming_gate)
set(_sak_labels_cert_tooling
    test_source_fingerprint test_partition_manager_certification_tools
    test_partition_manager_controller_evidence test_partition_manager_controller_restore_routing
    test_partition_manager_live_certifier test_partition_filesystem_probe_certifier
    test_file_management_live_certifier_gate
    test_new_partition_manager_automation_vm_security
    test_new_partition_manager_automation_vm_security_windows_powershell)
# Fuzz suites are product tests that also carry `fuzz`. One measured over the 60-second T2 fuzz
# budget (about 127 s serially), so it runs in T4 under fuzz-extended instead of product.
set(_sak_labels_fuzz_extended test_fuzz_backup_codec)

# RESOURCE_LOCK groups. Each names the shared resource that makes its members unsafe to overlap.
#   portable_data_root: every test executable lives in the one Release output directory, and
#     app_paths::dataRoot() is <exe dir>/data, so these share data/config/Utility.ini,
#     data/logs and data/ai_sessions (writers measured by diffing that tree around each test
#     run alone; the panels read the config the writers reset). The staging, archive and
#     startup-budget scripts read that same tree.
#   system_clipboard / desktop_session: the real Windows clipboard and interactive desktop.
#   image_scratch: SAK_TEST_IMAGE_SCRATCH_ROOT, where the core suite builds 17 GB sparse
#     images. It is the only consumer today; the lock names the owner for the next one.
set(_sak_lock_portable_data_root
    test_config_manager test_config_schema_versioning test_regex_pattern_library
    test_advanced_search_controller test_advanced_uninstall_controller
    test_ai_assistant_panel_tool_dispatch test_app_action_service test_quick_action_controller
    test_image_flasher_panel test_network_diagnostic_controller test_partition_manager_panel
    test_startup_budget test_stage_portable_release test_release_archive_recovery)
set(_sak_lock_system_clipboard test_ai_transcript_view test_win32_mcp_server)
set(_sak_lock_desktop_session test_win32_mcp_server test_follow_scroll_controller)
set(_sak_lock_image_scratch test_partition_manager_core)
# Timing-sensitive: wall-clock budgets, process deadlines, handle-growth soaks and child
# process sessions whose bounds a fully loaded machine can exceed. Each runs alone.
# The last two are machine-saturating rather than delicate: source_fingerprint drives real
# VS2022 configure/build fixtures and archive recovery zips and re-verifies the whole package.
# Alone they take about 110 s and 76 s; started together with the other heavy tests by a
# -j 8 run, both crossed their 180 s limits (2026-10-24). The limits stay; the overlap goes.
set(_sak_run_serial
    test_process_capture_deadline test_process_capture_deadline_windows_powershell
    test_startup_budget test_windows_process_job test_resource_soak
    test_ai_mcp_stdio_session test_ai_mcp_session_pool
    test_source_fingerprint test_release_archive_recovery)

get_property(_sak_label_tests DIRECTORY PROPERTY TESTS)
foreach(_sak_pair IN LISTS _sak_label_lists)
    string(REPLACE ":" ";" _sak_pair_parts "${_sak_pair}")
    list(GET _sak_pair_parts 1 _sak_list_var)
    foreach(_sak_listed IN LISTS ${_sak_list_var})
        if(NOT _sak_listed IN_LIST _sak_label_tests)
            message(FATAL_ERROR "Label list ${_sak_list_var} names unregistered test ${_sak_listed}")
        endif()
    endforeach()
endforeach()
foreach(_sak_lock_list _sak_lock_portable_data_root _sak_lock_system_clipboard
        _sak_lock_desktop_session _sak_lock_image_scratch _sak_run_serial)
    foreach(_sak_listed IN LISTS ${_sak_lock_list})
        if(NOT _sak_listed IN_LIST _sak_label_tests)
            message(FATAL_ERROR "${_sak_lock_list} names unregistered test ${_sak_listed}")
        endif()
    endforeach()
endforeach()

set(_sak_offscreen_count 0)
foreach(_sak_test IN LISTS _sak_label_tests)
    set(_sak_test_label "")
    foreach(_sak_pair IN LISTS _sak_label_lists)
        string(REPLACE ":" ";" _sak_pair_parts "${_sak_pair}")
        list(GET _sak_pair_parts 0 _sak_candidate_label)
        list(GET _sak_pair_parts 1 _sak_list_var)
        if(_sak_test IN_LIST ${_sak_list_var})
            if(_sak_test_label)
                message(FATAL_ERROR "${_sak_test} is listed under two labels")
            endif()
            set(_sak_test_label "${_sak_candidate_label}")
        endif()
    endforeach()
    if(NOT _sak_test_label)
        if(NOT TARGET ${_sak_test})
            message(FATAL_ERROR
                    "${_sak_test} is not a native test target and has no label. Add it to the "
                    "label list for its tier in tests/CMakeLists.txt.")
        endif()
        set(_sak_test_label product)
    endif()
    set(_sak_test_labels "${_sak_test_label}")
    if(_sak_test MATCHES "^test_fuzz_")
        list(APPEND _sak_test_labels fuzz)
    endif()
    set_property(TEST ${_sak_test} PROPERTY LABELS ${_sak_test_labels})

    foreach(_sak_lock portable_data_root system_clipboard desktop_session image_scratch)
        if(_sak_test IN_LIST _sak_lock_${_sak_lock})
            set_property(TEST ${_sak_test} APPEND PROPERTY RESOURCE_LOCK ${_sak_lock})
        endif()
    endforeach()
    if(_sak_test IN_LIST _sak_run_serial)
        set_property(TEST ${_sak_test} PROPERTY RUN_SERIAL TRUE)
    endif()

    # Native GUI suites run on the offscreen platform by default: no window reaches the
    # developer desktop and a parallel neighbour cannot steal focus from it. The real desktop
    # and clipboard tests keep the native platform, which is what they exist to exercise.
    if(TARGET ${_sak_test} AND _sak_test_label STREQUAL "product"
       AND NOT _sak_test IN_LIST _sak_lock_desktop_session
       AND NOT _sak_test IN_LIST _sak_lock_system_clipboard)
        get_target_property(_sak_test_links ${_sak_test} LINK_LIBRARIES)
        if("Qt6::Widgets" IN_LIST _sak_test_links OR "Qt6::Gui" IN_LIST _sak_test_links)
            get_test_property(${_sak_test} ENVIRONMENT _sak_test_env)
            if(NOT _sak_test_env MATCHES "QT_QPA_PLATFORM=")
                set_property(TEST ${_sak_test} APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
            endif()
            math(EXPR _sak_offscreen_count "${_sak_offscreen_count} + 1")
        endif()
    endif()
endforeach()
list(LENGTH _sak_label_tests _sak_label_test_count)
message(STATUS "Test labels: ${_sak_label_test_count} tests labelled; "
               "${_sak_offscreen_count} native GUI suites on the offscreen platform")

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

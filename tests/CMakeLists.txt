# SAK Utility Test Suite
# Comprehensive testing configuration

cmake_minimum_required(VERSION 3.28)

# Enable testing
enable_testing()

# Find Qt Test module
find_package(Qt6 REQUIRED COMPONENTS Test Network Concurrent)

# Test output directory
set(TEST_OUTPUT_DIR "${CMAKE_BINARY_DIR}/test_results")
file(MAKE_DIRECTORY "${TEST_OUTPUT_DIR}")

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
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/uup_dump_api.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/uup_dump_api.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/uup_iso_builder.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/uup_iso_builder.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/windows_iso_downloader.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/windows_iso_downloader.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/bundled_tools_manager.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/bundled_tools_manager.h"
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
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_worker_base — WorkerBase lifecycle, exception safety, cancellation
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_worker_base.cpp")
    add_executable(test_worker_base
        unit/test_worker_base.cpp
        ../src/threading/worker_base.cpp
        ../include/sak/worker_base.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_worker_base PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_error_codes — to_string completeness
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_error_codes.cpp")
    add_executable(test_error_codes
        unit/test_error_codes.cpp
    )
    set_target_properties(test_error_codes PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_diagnostic_types — struct defaults and value semantics
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_diagnostic_types.cpp")
    add_executable(test_diagnostic_types
        unit/test_diagnostic_types.cpp
    )
    set_target_properties(test_diagnostic_types PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_diagnostic_controller — suite state machine, cancellation, error handling
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
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_thermal_monitor — timer behavior and polling
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_thermal_monitor.cpp")
    add_executable(test_thermal_monitor
        unit/test_thermal_monitor.cpp
        ../src/core/thermal_monitor.cpp
        ../include/sak/thermal_monitor.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_thermal_monitor PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_diagnostic_report_generator — HTML, JSON, CSV report output
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_diagnostic_report_generator.cpp")
    add_executable(test_diagnostic_report_generator
        unit/test_diagnostic_report_generator.cpp
        ../src/core/diagnostic_report_generator.cpp
        ../include/sak/diagnostic_report_generator.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_diagnostic_report_generator PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_cpu_benchmark_worker — construction, result defaults, signal types
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
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_disk_benchmark_worker — construction, config, result defaults
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
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_memory_benchmark_worker — construction, result defaults
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
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_stress_test_worker — construction, config, result defaults
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_stress_test_worker.cpp")
    add_executable(test_stress_test_worker
        unit/test_stress_test_worker.cpp
        ../src/core/stress_test_worker.cpp
        ../include/sak/stress_test_worker.h
        ../src/core/thermal_monitor.cpp
        ../include/sak/thermal_monitor.h
        ../src/core/keep_awake.cpp
        ../include/sak/keep_awake.h
        ../src/threading/worker_base.cpp
        ../include/sak/worker_base.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_stress_test_worker PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_encryption — AES-256-CBC round-trip, wrong password, corrupted ciphertext
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_encryption.cpp")
    add_executable(test_encryption
        unit/test_encryption.cpp
        ../src/core/encryption.cpp
        ../src/core/secure_memory.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_encryption PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_file_hash — MD5/SHA256, verify correct/incorrect, progress, cancellation
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_file_hash.cpp")
    add_executable(test_file_hash
        unit/test_file_hash.cpp
        ../src/core/file_hash.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_file_hash PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_path_utils — safe path, traversal, pattern matching, directory size
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_path_utils.cpp")
    add_executable(test_path_utils
        unit/test_path_utils.cpp
        ../src/core/path_utils.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_path_utils PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_secure_memory — wipe, secure_buffer, secure_allocator, secure random
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_secure_memory.cpp")
    add_executable(test_secure_memory
        unit/test_secure_memory.cpp
        ../src/core/secure_memory.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_secure_memory PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_input_validator — path/string/numeric validation, safe arithmetic, buffer validation
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_input_validator.cpp")
    add_executable(test_input_validator
        unit/test_input_validator.cpp
        ../src/core/input_validator.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_input_validator PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_decompressor_factory — format detection, isCompressed, factory create
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
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_config_manager — singleton, get/set, defaults, signals, typed accessors
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_config_manager.cpp")
    add_executable(test_config_manager
        unit/test_config_manager.cpp
        ../src/core/config_manager.cpp
        ../src/core/logger.cpp
        ../include/sak/config_manager.h
    )
    set_target_properties(test_config_manager PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_smart_file_filter — dangerous files, size limits, pattern/folder exclusion
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_smart_file_filter.cpp")
    add_executable(test_smart_file_filter
        unit/test_smart_file_filter.cpp
        ../src/core/smart_file_filter.cpp
        ../src/core/user_profile_types.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_smart_file_filter PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_file_scanner — scan, filters, cancellation, static convenience methods
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_file_scanner.cpp")
    add_executable(test_file_scanner
        unit/test_file_scanner.cpp
        ../src/core/file_scanner.cpp
        ../src/core/path_utils.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_file_scanner PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_process_runner — echo, exit code, stderr, timeout, cancellation, PowerShell
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_process_runner.cpp")
    add_executable(test_process_runner
        unit/test_process_runner.cpp
        ../src/core/process_runner.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_process_runner PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_smart_disk_analyzer — SmartReport parsing, health assessment
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_smart_disk_analyzer.cpp")
    add_executable(test_smart_disk_analyzer
        unit/test_smart_disk_analyzer.cpp
        ../src/core/smart_disk_analyzer.cpp
        ../src/core/bundled_tools_manager.cpp
        ../src/core/logger.cpp
        ../include/sak/smart_disk_analyzer.h
        ../include/sak/bundled_tools_manager.h
    )
    set_target_properties(test_smart_disk_analyzer PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_package_matcher — mapping CRUD, export/import, findMatch
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_package_matcher.cpp")
    add_executable(test_package_matcher
        unit/test_package_matcher.cpp
        ../src/core/package_matcher.cpp
        ../src/core/app_scanner.cpp
        ../src/core/bundled_tools_manager.cpp
        ../src/core/chocolatey_manager.cpp
        ../src/core/logger.cpp
        ../include/sak/package_matcher.h
        ../include/sak/chocolatey_manager.h
    )
    set_target_properties(test_package_matcher PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_drive_scanner — DriveInfo struct, getDrives, lifecycle
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_drive_scanner.cpp")
    add_executable(test_drive_scanner
        unit/test_drive_scanner.cpp
        ../src/core/drive_scanner.cpp
        ../src/core/logger.cpp
        ../include/sak/drive_scanner.h
    )
    set_target_properties(test_drive_scanner PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
    )
    target_link_libraries(test_drive_scanner PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_drive_scanner PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_drive_scanner COMMAND test_drive_scanner
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_drive_scanner")
endif()

# test_permission_manager — canModifyPermissions, getOwner, SDDL, stripPermissions
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_permission_manager.cpp")
    add_executable(test_permission_manager
        unit/test_permission_manager.cpp
        ../src/core/permission_manager.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_permission_manager PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_elevation_manager — isElevated, canElevate, error messages
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_elevation_manager.cpp")
    add_executable(test_elevation_manager
        unit/test_elevation_manager.cpp
        ../src/core/elevation_manager.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_elevation_manager PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# --- Tier 4: Streaming/IO ---

# test_streaming_decompressor — gzip/bzip2/xz lifecycle, byte tracking
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
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_logger — initialization, level filtering, file output, thread safety
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_logger.cpp")
    add_executable(test_logger
        unit/test_logger.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_logger PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_install_script_parser — PowerShell install script parsing
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_install_script_parser.cpp")
    add_executable(test_install_script_parser
        unit/test_install_script_parser.cpp
        ../src/core/install_script_parser.cpp
        ../include/sak/install_script_parser.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_install_script_parser PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_script_rewriter — URL replacement for offline deployment
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
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_package_list_manager — curated package lists and presets
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_package_list_manager.cpp")
    add_executable(test_package_list_manager
        unit/test_package_list_manager.cpp
        ../src/core/package_list_manager.cpp
        ../include/sak/package_list_manager.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_package_list_manager PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_nuget_api_client — OData XML parsing, construction, metadata structs
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_nuget_api_client.cpp")
    add_executable(test_nuget_api_client
        unit/test_nuget_api_client.cpp
        ../src/core/nuget_api_client.cpp
        ../include/sak/nuget_api_client.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_nuget_api_client PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_action_factory — Factory completeness, metadata, category distribution, initial state
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/actions/test_action_factory.cpp")
    add_executable(test_action_factory
        unit/actions/test_action_factory.cpp

        # Factory
        ../src/actions/action_factory.cpp
        ../include/sak/actions/action_factory.h

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
        ../src/actions/disk_cleanup_action.cpp
        ../include/sak/actions/disk_cleanup_action.h
        ../src/actions/clear_browser_cache_action.cpp
        ../include/sak/actions/clear_browser_cache_action.h
        ../src/actions/defragment_drives_action.cpp
        ../include/sak/actions/defragment_drives_action.h
        ../src/actions/clear_windows_update_cache_action.cpp
        ../include/sak/actions/clear_windows_update_cache_action.h
        ../src/actions/disable_startup_programs_action.cpp
        ../include/sak/actions/disable_startup_programs_action.h
        ../src/actions/clear_event_logs_action.cpp
        ../include/sak/actions/clear_event_logs_action.h
        ../src/actions/optimize_power_settings_action.cpp
        ../include/sak/actions/optimize_power_settings_action.h
        ../src/actions/disable_visual_effects_action.cpp
        ../include/sak/actions/disable_visual_effects_action.h

        # Quick Backup actions
        ../src/actions/quickbooks_backup_action.cpp
        ../include/sak/actions/quickbooks_backup_action.h
        ../src/actions/browser_profile_backup_action.cpp
        ../include/sak/actions/browser_profile_backup_action.h
        ../src/actions/outlook_backup_action.cpp
        ../include/sak/actions/outlook_backup_action.h
        ../src/actions/sticky_notes_backup_action.cpp
        ../include/sak/actions/sticky_notes_backup_action.h
        ../src/actions/saved_game_data_backup_action.cpp
        ../include/sak/actions/saved_game_data_backup_action.h
        ../src/actions/tax_software_backup_action.cpp
        ../include/sak/actions/tax_software_backup_action.h
        ../src/actions/photo_management_backup_action.cpp
        ../include/sak/actions/photo_management_backup_action.h
        ../src/actions/development_configs_backup_action.cpp
        ../include/sak/actions/development_configs_backup_action.h
        ../src/actions/backup_known_networks_action.cpp
        ../include/sak/actions/backup_known_networks_action.h

        # Maintenance actions
        ../src/actions/update_all_apps_action.cpp
        ../include/sak/actions/update_all_apps_action.h
        ../src/actions/windows_update_action.cpp
        ../include/sak/actions/windows_update_action.h
        ../src/actions/verify_system_files_action.cpp
        ../include/sak/actions/verify_system_files_action.h
        ../src/actions/check_disk_errors_action.cpp
        ../include/sak/actions/check_disk_errors_action.h
        ../src/actions/rebuild_icon_cache_action.cpp
        ../include/sak/actions/rebuild_icon_cache_action.h
        ../src/actions/reset_network_action.cpp
        ../include/sak/actions/reset_network_action.h
        ../src/actions/clear_print_spooler_action.cpp
        ../include/sak/actions/clear_print_spooler_action.h

        # Troubleshooting actions
        ../src/actions/generate_system_report_action.cpp
        ../include/sak/actions/generate_system_report_action.h
        ../src/actions/check_bloatware_action.cpp
        ../include/sak/actions/check_bloatware_action.h
        ../src/actions/test_network_speed_action.cpp
        ../include/sak/actions/test_network_speed_action.h
        ../src/actions/scan_malware_action.cpp
        ../include/sak/actions/scan_malware_action.h
        ../src/actions/repair_windows_store_action.cpp
        ../include/sak/actions/repair_windows_store_action.h
        ../src/actions/fix_audio_issues_action.cpp
        ../include/sak/actions/fix_audio_issues_action.h

        # Emergency Recovery actions
        ../src/actions/create_restore_point_action.cpp
        ../include/sak/actions/create_restore_point_action.h
        ../src/actions/export_registry_keys_action.cpp
        ../include/sak/actions/export_registry_keys_action.h
        ../src/actions/screenshot_settings_action.cpp
        ../include/sak/actions/screenshot_settings_action.h
        ../src/actions/backup_desktop_wallpaper_action.cpp
        ../include/sak/actions/backup_desktop_wallpaper_action.h
        ../src/actions/backup_printer_settings_action.cpp
        ../include/sak/actions/backup_printer_settings_action.h
        ../src/actions/backup_bitlocker_keys_action.cpp
        ../include/sak/actions/backup_bitlocker_keys_action.h
    )

    set_target_properties(test_action_factory PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_flash_types — FlashProgress arithmetic, FlashResult checks (TST-02)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_flash_types.cpp")
    add_executable(test_flash_types
        unit/test_flash_types.cpp
    )
    set_target_properties(test_flash_types PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_quick_action — QuickAction static helpers and base class (TST-10)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_quick_action.cpp")
    add_executable(test_quick_action
        unit/test_quick_action.cpp
        ../src/core/quick_action.cpp
        ../include/sak/quick_action.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_quick_action PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_user_profile_types — Serialization round-trips (TST-05)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_user_profile_types.cpp")
    add_executable(test_user_profile_types
        unit/test_user_profile_types.cpp
        ../src/core/user_profile_types.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_user_profile_types PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_user_data_manager — Checksums, data locations, size calc (TST-04)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_user_data_manager.cpp")
    add_executable(test_user_data_manager
        unit/test_user_data_manager.cpp
        ../src/core/user_data_manager.cpp
        ../include/sak/user_data_manager.h
        ../src/core/encryption.cpp
        ../src/core/secure_memory.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_user_data_manager PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_duplicate_finder_worker — Duplicate detection with temp files (TST-11)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_duplicate_finder_worker.cpp")
    add_executable(test_duplicate_finder_worker
        unit/test_duplicate_finder_worker.cpp
        ../src/threading/duplicate_finder_worker.cpp
        ../include/sak/duplicate_finder_worker.h
        ../src/threading/worker_base.cpp
        ../include/sak/worker_base.h
        ../src/core/file_hash.cpp
        ../include/sak/file_hash.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_duplicate_finder_worker PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
    )
    target_link_libraries(test_duplicate_finder_worker PRIVATE
        Qt6::Core Qt6::Test Qt6::Concurrent ${PLATFORM_LIBS}
    )
    target_include_directories(test_duplicate_finder_worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_duplicate_finder_worker COMMAND test_duplicate_finder_worker
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_duplicate_finder_worker")
endif()

# test_organizer_worker — File categorization and undo (TST-11)
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
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_app_installation_worker — Job filtering, stats, state management (TST-03)
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
        ../src/core/logger.cpp
    )
    set_target_properties(test_app_installation_worker PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_user_profile_restore_worker — Restore flow, conflict resolution, signals (TST-06)
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
        ../src/core/windows_user_scanner.cpp
        ../include/sak/windows_user_scanner.h
        ../src/core/path_utils.cpp
        ../include/sak/path_utils.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_user_profile_restore_worker PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_windows_usb_creator — Input validation, BUG-09 disk number injection (TST-07)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_windows_usb_creator.cpp")
    add_executable(test_windows_usb_creator
        unit/test_windows_usb_creator.cpp
        ../src/core/windows_usb_creator.cpp
        ../src/core/windows_usb_creator_extract.cpp
        ../include/sak/windows_usb_creator.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_windows_usb_creator PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_flash_worker — Structs, validation, early-exit paths, cancellation (TST-09)
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
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_quick_action_result_io — enum ↔ string round-trips, file write/read
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
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_quick_action_controller — registration, lookup, category filtering
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_quick_action_controller.cpp")
    add_executable(test_quick_action_controller
        unit/test_quick_action_controller.cpp
        ../src/core/quick_action_controller.cpp
        ../include/sak/quick_action_controller.h
        ../src/core/quick_action.cpp
        ../include/sak/quick_action.h
        ../src/core/quick_action_result_io.cpp
        ../include/sak/quick_action_result_io.h
        ../src/core/elevation_manager.cpp
        ../include/sak/elevation_manager.h
        ../src/core/logger.cpp
        ../include/sak/logger.h
    )
    set_target_properties(test_quick_action_controller PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_migration_report — entry CRUD, selection, counts, distribution
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
        ../src/core/logger.cpp
    )
    set_target_properties(test_migration_report PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_linux_distro_catalog — catalog integrity, category filtering, resolution
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_linux_distro_catalog.cpp")
    add_executable(test_linux_distro_catalog
        unit/test_linux_distro_catalog.cpp
        ../src/core/linux_distro_catalog.cpp
        ../include/sak/linux_distro_catalog.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_linux_distro_catalog PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_flash_coordinator — defaults, config, guards, cancel safety
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
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
    )
    target_link_libraries(test_flash_coordinator PRIVATE
        Qt6::Core Qt6::Test ZLIB::ZLIB BZip2::BZip2 LibLZMA::LibLZMA ${PLATFORM_LIBS}
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

# test_user_profile_backup_worker — construction, initial state, cancel
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
        ../src/core/path_utils.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_user_profile_backup_worker PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_uup_iso_builder — phase defaults, cancel, weight constants
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_uup_iso_builder.cpp")
    add_executable(test_uup_iso_builder
        unit/test_uup_iso_builder.cpp
        ../src/core/uup_iso_builder.cpp
        ../include/sak/uup_iso_builder.h
        ../src/core/uup_dump_api.cpp
        ../include/sak/uup_dump_api.h
        ../src/core/bundled_tools_manager.cpp
        ../include/sak/bundled_tools_manager.h
        ../src/core/logger.cpp
    )
    set_target_properties(test_uup_iso_builder PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_all_actions_metadata — per-action expected values, cancel behavior
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/actions/test_all_actions_metadata.cpp")
    add_executable(test_all_actions_metadata
        unit/actions/test_all_actions_metadata.cpp

        # Factory
        ../src/actions/action_factory.cpp
        ../include/sak/actions/action_factory.h

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
        ../src/actions/disk_cleanup_action.cpp
        ../include/sak/actions/disk_cleanup_action.h
        ../src/actions/clear_browser_cache_action.cpp
        ../include/sak/actions/clear_browser_cache_action.h
        ../src/actions/defragment_drives_action.cpp
        ../include/sak/actions/defragment_drives_action.h
        ../src/actions/clear_windows_update_cache_action.cpp
        ../include/sak/actions/clear_windows_update_cache_action.h
        ../src/actions/disable_startup_programs_action.cpp
        ../include/sak/actions/disable_startup_programs_action.h
        ../src/actions/clear_event_logs_action.cpp
        ../include/sak/actions/clear_event_logs_action.h
        ../src/actions/optimize_power_settings_action.cpp
        ../include/sak/actions/optimize_power_settings_action.h
        ../src/actions/disable_visual_effects_action.cpp
        ../include/sak/actions/disable_visual_effects_action.h

        # Quick Backup actions
        ../src/actions/quickbooks_backup_action.cpp
        ../include/sak/actions/quickbooks_backup_action.h
        ../src/actions/browser_profile_backup_action.cpp
        ../include/sak/actions/browser_profile_backup_action.h
        ../src/actions/outlook_backup_action.cpp
        ../include/sak/actions/outlook_backup_action.h
        ../src/actions/sticky_notes_backup_action.cpp
        ../include/sak/actions/sticky_notes_backup_action.h
        ../src/actions/saved_game_data_backup_action.cpp
        ../include/sak/actions/saved_game_data_backup_action.h
        ../src/actions/tax_software_backup_action.cpp
        ../include/sak/actions/tax_software_backup_action.h
        ../src/actions/photo_management_backup_action.cpp
        ../include/sak/actions/photo_management_backup_action.h
        ../src/actions/development_configs_backup_action.cpp
        ../include/sak/actions/development_configs_backup_action.h
        ../src/actions/backup_known_networks_action.cpp
        ../include/sak/actions/backup_known_networks_action.h

        # Maintenance actions
        ../src/actions/update_all_apps_action.cpp
        ../include/sak/actions/update_all_apps_action.h
        ../src/actions/windows_update_action.cpp
        ../include/sak/actions/windows_update_action.h
        ../src/actions/verify_system_files_action.cpp
        ../include/sak/actions/verify_system_files_action.h
        ../src/actions/check_disk_errors_action.cpp
        ../include/sak/actions/check_disk_errors_action.h
        ../src/actions/rebuild_icon_cache_action.cpp
        ../include/sak/actions/rebuild_icon_cache_action.h
        ../src/actions/reset_network_action.cpp
        ../include/sak/actions/reset_network_action.h
        ../src/actions/clear_print_spooler_action.cpp
        ../include/sak/actions/clear_print_spooler_action.h

        # Troubleshooting actions
        ../src/actions/generate_system_report_action.cpp
        ../include/sak/actions/generate_system_report_action.h
        ../src/actions/check_bloatware_action.cpp
        ../include/sak/actions/check_bloatware_action.h
        ../src/actions/test_network_speed_action.cpp
        ../include/sak/actions/test_network_speed_action.h
        ../src/actions/scan_malware_action.cpp
        ../include/sak/actions/scan_malware_action.h
        ../src/actions/repair_windows_store_action.cpp
        ../include/sak/actions/repair_windows_store_action.h
        ../src/actions/fix_audio_issues_action.cpp
        ../include/sak/actions/fix_audio_issues_action.h

        # Emergency Recovery actions
        ../src/actions/create_restore_point_action.cpp
        ../include/sak/actions/create_restore_point_action.h
        ../src/actions/export_registry_keys_action.cpp
        ../include/sak/actions/export_registry_keys_action.h
        ../src/actions/screenshot_settings_action.cpp
        ../include/sak/actions/screenshot_settings_action.h
        ../src/actions/backup_desktop_wallpaper_action.cpp
        ../include/sak/actions/backup_desktop_wallpaper_action.h
        ../src/actions/backup_printer_settings_action.cpp
        ../include/sak/actions/backup_printer_settings_action.h
        ../src/actions/backup_bitlocker_keys_action.cpp
        ../include/sak/actions/backup_bitlocker_keys_action.h
    )

    set_target_properties(test_all_actions_metadata PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_advanced_search_types — struct defaults, extension sets, value semantics
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_advanced_search_types.cpp")
    add_executable(test_advanced_search_types
        unit/test_advanced_search_types.cpp
    )
    set_target_properties(test_advanced_search_types PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_regex_pattern_library — built-in/custom patterns, enable/disable, CRUD, combined
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_regex_pattern_library.cpp")
    add_executable(test_regex_pattern_library
        unit/test_regex_pattern_library.cpp
        ../src/core/regex_pattern_library.cpp
        ../src/core/logger.cpp
        ../include/sak/regex_pattern_library.h
    )
    set_target_properties(test_regex_pattern_library PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
    )
    target_link_libraries(test_regex_pattern_library PRIVATE
        Qt6::Core Qt6::Test
    )
    target_include_directories(test_regex_pattern_library PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_regex_pattern_library COMMAND test_regex_pattern_library
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_regex_pattern_library")
endif()

# test_advanced_search_worker — regex compilation, text/metadata/archive/binary search
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_advanced_search_worker.cpp")
    add_executable(test_advanced_search_worker
        unit/test_advanced_search_worker.cpp
        ../src/core/advanced_search_worker.cpp
        ../src/threading/worker_base.cpp
        ../src/core/logger.cpp
        ../include/sak/advanced_search_worker.h
        ../include/sak/worker_base.h
    )
    set_target_properties(test_advanced_search_worker PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
    )
    target_link_libraries(test_advanced_search_worker PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test ZLIB::ZLIB
    )
    target_include_directories(test_advanced_search_worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_advanced_search_worker COMMAND test_advanced_search_worker
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_advanced_search_worker")
endif()

# test_advanced_search_controller — state machine, history, preferences, lifecycle
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_advanced_search_controller.cpp")
    add_executable(test_advanced_search_controller
        unit/test_advanced_search_controller.cpp
        ../src/core/advanced_search_controller.cpp
        ../src/core/advanced_search_worker.cpp
        ../src/core/regex_pattern_library.cpp
        ../src/core/config_manager.cpp
        ../src/threading/worker_base.cpp
        ../src/core/logger.cpp
        ../include/sak/advanced_search_controller.h
        ../include/sak/advanced_search_worker.h
        ../include/sak/regex_pattern_library.h
        ../include/sak/config_manager.h
        ../include/sak/worker_base.h
    )
    set_target_properties(test_advanced_search_controller PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
    )
    target_link_libraries(test_advanced_search_controller PRIVATE
        Qt6::Core Qt6::Gui Qt6::Test ZLIB::ZLIB
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

# test_advanced_uninstall_types — struct defaults, enums, value semantics
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_advanced_uninstall_types.cpp")
    add_executable(test_advanced_uninstall_types
        unit/test_advanced_uninstall_types.cpp
    )
    set_target_properties(test_advanced_uninstall_types PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_advanced_uninstall_controller — state machine, queue, settings, input validation
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_advanced_uninstall_controller.cpp")
    add_executable(test_advanced_uninstall_controller
        unit/test_advanced_uninstall_controller.cpp
        ../src/core/advanced_uninstall_controller.cpp
        ../src/core/program_enumerator.cpp
        ../src/core/uninstall_worker.cpp
        ../src/core/leftover_scanner.cpp
        ../src/core/cleanup_worker.cpp
        ../src/core/registry_snapshot_engine.cpp
        ../src/core/restore_point_manager.cpp
        ../src/core/elevation_manager.cpp
        ../src/core/config_manager.cpp
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
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_leftover_scanner — pattern building, risk classification, scan levels, cancellation
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_leftover_scanner.cpp")
    add_executable(test_leftover_scanner
        unit/test_leftover_scanner.cpp
        ../src/core/leftover_scanner.cpp
        ../src/core/registry_snapshot_engine.cpp
        ../src/core/logger.cpp
        ../include/sak/leftover_scanner.h
        ../include/sak/registry_snapshot_engine.h
    )
    set_target_properties(test_leftover_scanner PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_registry_snapshot_engine — diff logic, pattern matching, capture smoke test
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_registry_snapshot_engine.cpp")
    add_executable(test_registry_snapshot_engine
        unit/test_registry_snapshot_engine.cpp
        ../src/core/registry_snapshot_engine.cpp
        ../src/core/logger.cpp
        ../include/sak/registry_snapshot_engine.h
    )
    set_target_properties(test_registry_snapshot_engine PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_network_diagnostic_types — value/move semantics, enums, compile-time invariants
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_network_diagnostic_types.cpp")
    add_executable(test_network_diagnostic_types
        unit/test_network_diagnostic_types.cpp
    )
    set_target_properties(test_network_diagnostic_types PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_network_diagnostic_utils — WiFi freq conversion, vendor lookup, port presets, DNS lists
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_network_diagnostic_utils.cpp")
    add_executable(test_network_diagnostic_utils
        unit/test_network_diagnostic_utils.cpp
        ../src/core/wifi_analyzer.cpp
        ../src/core/port_scanner.cpp
        ../src/core/dns_diagnostic_tool.cpp
        ../include/sak/wifi_analyzer.h
        ../include/sak/port_scanner.h
        ../include/sak/dns_diagnostic_tool.h
    )
    set_target_properties(test_network_diagnostic_utils PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_network_diagnostic_report — HTML/JSON report generation, sections, metadata
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_network_diagnostic_report.cpp")
    add_executable(test_network_diagnostic_report
        unit/test_network_diagnostic_report.cpp
        ../src/core/network_diagnostic_report_generator.cpp
        ../include/sak/network_diagnostic_report_generator.h
    )
    set_target_properties(test_network_diagnostic_report PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_format_utils — sak::formatBytes() display formatting
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_format_utils.cpp")
    add_executable(test_format_utils
        unit/test_format_utils.cpp
    )
    set_target_properties(test_format_utils PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_wifi_profile_scanner — WiFi profile name/security parsing
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_wifi_profile_scanner.cpp")
    add_executable(test_wifi_profile_scanner
        unit/test_wifi_profile_scanner.cpp
        ../src/core/wifi_profile_scanner.cpp
        ../src/core/user_profile_types.cpp
        ../src/core/logger.cpp
    )
    set_target_properties(test_wifi_profile_scanner PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_email_types — struct defaults, enum values, constants validation
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_email_types.cpp")
    add_executable(test_email_types
        unit/test_email_types.cpp
    )
    set_target_properties(test_email_types PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_pst_parser — header validation, magic, encryption, lifecycle
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_pst_parser.cpp")
    add_executable(test_pst_parser
        unit/test_pst_parser.cpp
        ../src/core/pst_parser.cpp
        ../src/core/logger.cpp
        ../include/sak/pst_parser.h
    )
    set_target_properties(test_pst_parser PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
    )
    target_link_libraries(test_pst_parser PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_pst_parser PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_pst_parser COMMAND test_pst_parser
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_pst_parser")
endif()

# test_ost_integration — opens a real OST file and exercises full parse pipeline
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_ost_integration.cpp")
    add_executable(test_ost_integration
        integration/test_ost_integration.cpp
        ../src/core/pst_parser.cpp
        ../src/core/logger.cpp
        ../include/sak/pst_parser.h
    )
    set_target_properties(test_ost_integration PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
    )
    target_link_libraries(test_ost_integration PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_ost_integration PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_ost_integration COMMAND test_ost_integration
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added integration test: test_ost_integration")
endif()

# test_mbox_parser — indexing, message loading, From line detection
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_mbox_parser.cpp")
    add_executable(test_mbox_parser
        unit/test_mbox_parser.cpp
        ../src/core/mbox_parser.cpp
        ../src/core/logger.cpp
        ../include/sak/mbox_parser.h
    )
    set_target_properties(test_mbox_parser PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_email_search_worker — criteria, cancel, search hit structure
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
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
    )
    target_link_libraries(test_email_search_worker PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_email_search_worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_email_search_worker COMMAND test_email_search_worker
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_email_search_worker")
endif()

# test_email_export_worker — config, result, format coverage
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_email_export_worker.cpp")
    add_executable(test_email_export_worker
        unit/test_email_export_worker.cpp
        ../src/core/email_export_worker.cpp
        ../src/core/pst_parser.cpp
        ../src/core/mbox_parser.cpp
        ../src/core/logger.cpp
        ../include/sak/email_export_worker.h
        ../include/sak/pst_parser.h
        ../include/sak/mbox_parser.h
    )
    set_target_properties(test_email_export_worker PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
    )
    target_link_libraries(test_email_export_worker PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_email_export_worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_email_export_worker COMMAND test_email_export_worker
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_email_export_worker")
endif()

# test_email_profile_manager — discovery, backup/restore, linked files
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_email_profile_manager.cpp")
    add_executable(test_email_profile_manager
        unit/test_email_profile_manager.cpp
        ../src/core/email_profile_manager.cpp
        ../src/core/logger.cpp
        ../include/sak/email_profile_manager.h
    )
    set_target_properties(test_email_profile_manager PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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

# test_email_report_generator — HTML, JSON, CSV output validation
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_email_report_generator.cpp")
    add_executable(test_email_report_generator
        unit/test_email_report_generator.cpp
        ../src/core/email_report_generator.cpp
        ../src/core/logger.cpp
        ../include/sak/email_report_generator.h
    )
    set_target_properties(test_email_report_generator PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
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
    COMMAND ${CMAKE_CTEST_COMMAND} -R "test_(backup|migration)_workflow|test_uup_conversion_pipeline|test_ost_integration" --output-on-failure
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

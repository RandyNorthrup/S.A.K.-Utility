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

# All fake tests have been removed
# Previous tests were not testing real functionality (e.g., QVERIFY(42 >= 0))
# TODO: Write real tests that verify actual behavior

set(UNIT_TESTS
    test_transfer_protocol
    test_transfer_types
    test_transfer_security
    test_network_connection
    test_peer_discovery
    test_destination_registry
    test_orchestration_protocol
    test_migration_orchestrator
    test_orchestration_types
    test_orchestration_client
    test_orchestration_discovery_service
    test_mapping_engine
    test_parallel_transfer_manager
    test_deployment_history
    test_assignment_queue_store
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

    if(test_name STREQUAL "test_transfer_protocol")
        list(APPEND impl_sources
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/network_transfer_protocol.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/network_transfer_protocol.h"
        )
    endif()

    if(test_name STREQUAL "test_transfer_types")
        list(APPEND impl_sources
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/network_transfer_types.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/network_transfer_types.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/user_profile_types.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/user_profile_types.h"
        )
    endif()

    if(test_name STREQUAL "test_transfer_security")
        list(APPEND impl_sources
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/network_transfer_security.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/network_transfer_security.h"
        )
    endif()

    if(test_name STREQUAL "test_network_connection")
        list(APPEND impl_sources
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/network_connection_manager.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/network_connection_manager.h"
        )
    endif()

    if(test_name STREQUAL "test_peer_discovery")
        list(APPEND impl_sources
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/peer_discovery_service.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/peer_discovery_service.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/network_transfer_types.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/network_transfer_types.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/user_profile_types.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/user_profile_types.h"
        )
    endif()

    if(test_name STREQUAL "test_assignment_queue_store")
        list(APPEND impl_sources
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/orchestration_types.cpp"
        )
    endif()

    if(test_name STREQUAL "test_mapping_engine")
        list(APPEND impl_sources
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/destination_registry.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/orchestration_types.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/destination_registry.h"
        )
    endif()

    if(test_name STREQUAL "test_migration_orchestrator")
        list(APPEND impl_sources
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/deployment_manager.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/destination_registry.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/mapping_engine.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/orchestration_protocol.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/orchestration_types.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/orchestration_server.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/orchestration_discovery_service.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/deployment_manager.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/destination_registry.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/mapping_engine.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/orchestration_server_interface.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/orchestration_server.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/orchestration_discovery_service.h"
        )
    endif()

    if(test_name STREQUAL "test_orchestration_protocol")
        list(APPEND impl_sources
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/orchestration_protocol.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/orchestration_protocol.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/orchestration_types.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/orchestration_types.h"
        )
    endif()

    if(test_name STREQUAL "test_orchestration_client")
        list(APPEND impl_sources
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/orchestration_protocol.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/orchestration_types.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/orchestration_protocol.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/orchestration_types.h"
        )
    endif()

    if(test_name STREQUAL "test_orchestration_discovery_service")
        list(APPEND impl_sources
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/orchestration_types.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/orchestration_types.h"
        )
    endif()

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
        if(test_name STREQUAL "test_network_connection"
            OR test_name STREQUAL "test_peer_discovery"
            OR test_name STREQUAL "test_orchestration_discovery_service"
            OR test_name STREQUAL "test_orchestration_client"
            OR test_name STREQUAL "test_orchestration_protocol"
            OR test_name STREQUAL "test_migration_orchestrator"
            OR test_name STREQUAL "test_transfer_protocol"
            OR test_name STREQUAL "test_transfer_types"
            OR test_name STREQUAL "test_windows_iso_downloader"
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

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_parallel_transfer_manager_stress.cpp")
    add_executable(test_parallel_transfer_manager_stress
        unit/test_parallel_transfer_manager_stress.cpp
        ../src/core/parallel_transfer_manager.cpp
        ../src/core/logger.cpp
        ../include/sak/parallel_transfer_manager.h
    )

    set_target_properties(test_parallel_transfer_manager_stress PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
    )

    target_link_libraries(test_parallel_transfer_manager_stress PRIVATE
        Qt6::Core
        Qt6::Test
        ${PLATFORM_LIBS}
    )

    target_include_directories(test_parallel_transfer_manager_stress PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )

    add_test(NAME test_parallel_transfer_manager_stress
             COMMAND test_parallel_transfer_manager_stress
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")

    message(STATUS "Added unit test: test_parallel_transfer_manager_stress")
endif()

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
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
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

# test_deployment_manager — FIFO queue, signals, readiness check
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_deployment_manager.cpp")
    add_executable(test_deployment_manager
        unit/test_deployment_manager.cpp
        ../src/core/deployment_manager.cpp
        ../src/core/orchestration_types.cpp
        ../src/core/logger.cpp
        ../include/sak/deployment_manager.h
    )
    set_target_properties(test_deployment_manager PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
    )
    target_link_libraries(test_deployment_manager PRIVATE
        Qt6::Core Qt6::Test ${PLATFORM_LIBS}
    )
    target_include_directories(test_deployment_manager PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME test_deployment_manager COMMAND test_deployment_manager
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    message(STATUS "Added unit test: test_deployment_manager")
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
# Integration Tests
# ============================================================================

# Integration test: Network Transfer Workflow
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_network_transfer_workflow.cpp")
    add_executable(test_network_transfer_workflow
        integration/test_network_transfer_workflow.cpp
        ../src/core/network_transfer_controller.cpp
        ../src/core/network_transfer_worker.cpp
        ../src/core/network_connection_manager.cpp
        ../src/core/peer_discovery_service.cpp
        ../src/core/orchestration_client.cpp
        ../src/core/orchestration_discovery_service.cpp
        ../src/core/orchestration_protocol.cpp
        ../src/core/orchestration_types.cpp
        ../src/core/network_transfer_protocol.cpp
        ../src/core/network_transfer_types.cpp
        ../src/core/network_transfer_security.cpp
        ../src/core/user_profile_types.cpp
        ../src/core/permission_manager.cpp
        ../src/core/file_hash.cpp
        ../src/core/path_utils.cpp
        ../src/core/logger.cpp
        ../include/sak/network_transfer_controller.h
        ../include/sak/network_transfer_worker.h
        ../include/sak/network_connection_manager.h
        ../include/sak/peer_discovery_service.h
        ../include/sak/orchestration_client.h
        ../include/sak/orchestration_discovery_service.h
        ../include/sak/orchestration_protocol.h
        ../include/sak/orchestration_types.h
        ../include/sak/network_transfer_protocol.h
        ../include/sak/network_transfer_types.h
        ../include/sak/network_transfer_security.h
        ../include/sak/user_profile_types.h
    )

    set_target_properties(test_network_transfer_workflow PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
    )

    target_link_libraries(test_network_transfer_workflow PRIVATE
        Qt6::Core
        Qt6::Network
        Qt6::Test
        Qt6::Concurrent
        ${PLATFORM_LIBS}
    )

    target_include_directories(test_network_transfer_workflow PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )

    add_test(NAME test_network_transfer_workflow
             COMMAND test_network_transfer_workflow
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")

    message(STATUS "Added integration test: test_network_transfer_workflow")
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
    COMMAND ${CMAKE_CTEST_COMMAND} -R "test_(path_utils|logger|config_manager|file_scanner|encryption)" --output-on-failure
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Running unit tests..."
)

# Run integration tests only
add_custom_target(run_integration_tests
    COMMAND ${CMAKE_CTEST_COMMAND} -R "test_(backup|migration|network_transfer)_workflow" --output-on-failure
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

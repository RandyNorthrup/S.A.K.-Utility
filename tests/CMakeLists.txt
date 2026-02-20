# SAK Utility Test Suite
# Comprehensive testing configuration

cmake_minimum_required(VERSION 3.28)

# Enable testing
enable_testing()

# Find Qt Test module
find_package(Qt6 REQUIRED COMPONENTS Test Network)

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
            OR test_name STREQUAL "test_windows_iso_downloader")
            list(APPEND extra_unit_libs Qt6::Network)
        endif()
        
        # ISO downloader needs Qt Widgets
        if(test_name STREQUAL "test_windows_iso_downloader")
            list(APPEND extra_unit_libs Qt6::Widgets)
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
# Unit Tests - Action Classes
# ============================================================================

set(ACTION_TESTS
    # No tests yet
)

# ============================================================================
# Unit Tests - Utilities and Device Operations
# ============================================================================

set(UTILITY_TESTS
    # No tests yet
)

# ============================================================================
# Unit Tests - Worker Classes
# ============================================================================

set(WORKER_TESTS
    # No tests yet
)

foreach(test_name ${WORKER_TESTS})
    file(GLOB test_sources 
        "${CMAKE_CURRENT_SOURCE_DIR}/unit/${test_name}.cpp"
    )
    
    string(REPLACE "test_" "" worker_name "${test_name}")
    file(GLOB impl_sources
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/workers/${worker_name}.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/workers/${worker_name}.h"
    )
    
    if(test_sources)
        add_executable(${test_name} ${test_sources} ${impl_sources})
        
        set_target_properties(${test_name} PROPERTIES
            AUTOMOC ON
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
        )
        
        target_link_libraries(${test_name} PRIVATE
            Qt6::Core
            Qt6::Test
            Qt6::Concurrent
            ${PLATFORM_LIBS}
        )
        
        target_include_directories(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../include
        )
        
        add_test(NAME ${test_name} 
                 COMMAND ${test_name}
                 WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
        
        message(STATUS "Added worker test: ${test_name}")
    endif()
endforeach()

foreach(test_name ${ACTION_TESTS})
    file(GLOB test_sources 
        "${CMAKE_CURRENT_SOURCE_DIR}/unit/actions/${test_name}.cpp"
    )
    
    string(REPLACE "test_" "" action_name "${test_name}")
    file(GLOB impl_sources
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/actions/${action_name}.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/actions/${action_name}.h"
    )
    
    # Add common dependencies
    list(APPEND impl_sources
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/logger.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/logger.h"
    )

    if(test_name STREQUAL "test_assignment_queue_store")
        list(APPEND impl_sources
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/orchestration_types.cpp"
        )
    endif()

    if(test_name STREQUAL "test_mapping_engine")
        list(APPEND impl_sources
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/destination_registry.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/orchestration_types.cpp"
        )
    endif()
    
    if(test_sources)
        add_executable(${test_name} ${test_sources} ${impl_sources})
        
        set_target_properties(${test_name} PROPERTIES
            AUTOMOC ON
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
        )
        
        target_link_libraries(${test_name} PRIVATE
            Qt6::Core
            Qt6::Test
            ${PLATFORM_LIBS}
        )
        
        target_include_directories(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../include
        )
        
        add_test(NAME ${test_name} 
                 COMMAND ${test_name}
                 WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
        
        message(STATUS "Added action test: ${test_name}")
    endif()
endforeach()

foreach(test_name ${UTILITY_TESTS})
    file(GLOB test_sources 
        "${CMAKE_CURRENT_SOURCE_DIR}/unit/${test_name}.cpp"
    )
    
    string(REPLACE "test_" "" util_name "${test_name}")
    
    # Build source file list based on utility type
    set(impl_sources "")
    
    if(util_name STREQUAL "action_factory")
        file(GLOB impl_sources
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/actions/action_factory.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/actions/action_factory.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/actions/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/actions/*.h"
        )
    elseif(util_name STREQUAL "quick_action_controller")
        file(GLOB impl_sources
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/quick_action_controller.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/quick_action_controller.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/actions/action_factory.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/actions/*.cpp"
        )
    elseif(util_name STREQUAL "drive_scanner")
        file(GLOB impl_sources
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/drive_scanner.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/drive_scanner.h"
        )
    endif()
    
    # Add common dependencies
    list(APPEND impl_sources
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/logger.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../include/sak/logger.h"
    )
    
    if(test_sources)
        add_executable(${test_name} ${test_sources} ${impl_sources})
        
        set_target_properties(${test_name} PROPERTIES
            AUTOMOC ON
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
        )
        
        target_link_libraries(${test_name} PRIVATE
            Qt6::Core
            Qt6::Test
            Qt6::Concurrent
            ${PLATFORM_LIBS}
        )
        
        target_include_directories(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../include
        )
        
        add_test(NAME ${test_name} 
                 COMMAND ${test_name}
                 WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
        
        message(STATUS "Added utility test: ${test_name}")
    endif()
endforeach()

# ============================================================================
# Integration Tests
# ============================================================================

# Integration test: Backup Workflow
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_backup_workflow.cpp")
    add_executable(test_backup_workflow
        integration/test_backup_workflow.cpp
        ../src/gui/backup_wizard.cpp
        ../src/gui/restore_wizard.cpp
        ../src/core/user_data_manager.cpp
        ../src/core/logger.cpp
        ../src/actions/backup_browser_data_action.cpp
    )

    set_target_properties(test_backup_workflow PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
    )

    target_link_libraries(test_backup_workflow PRIVATE
        Qt6::Core
        Qt6::Widgets
        Qt6::Test
        Qt6::Concurrent
        ${PLATFORM_LIBS}
    )

    target_include_directories(test_backup_workflow PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )

    add_test(NAME test_backup_workflow 
             COMMAND test_backup_workflow
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    
    message(STATUS "Added integration test: test_backup_workflow")
endif()

# Integration test: Migration Workflow
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_migration_workflow.cpp")
    add_executable(test_migration_workflow
        integration/test_migration_workflow.cpp
        ../src/core/app_scanner.cpp
        ../src/core/chocolatey_manager.cpp
        ../src/core/package_matcher.cpp
        ../src/core/migration_report.cpp
        ../src/core/logger.cpp
    )

    set_target_properties(test_migration_workflow PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
    )

    target_link_libraries(test_migration_workflow PRIVATE
        Qt6::Core
        Qt6::Test
        Qt6::Concurrent
        ${PLATFORM_LIBS}
    )

    target_include_directories(test_migration_workflow PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )

    add_test(NAME test_migration_workflow 
             COMMAND test_migration_workflow
             WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
    
    message(STATUS "Added integration test: test_migration_workflow")
endif()

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

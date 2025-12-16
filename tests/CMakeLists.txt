# SAK Utility Test Suite
# Comprehensive testing configuration

cmake_minimum_required(VERSION 3.28)

# Enable testing
enable_testing()

# Find Qt Test module
find_package(Qt6 REQUIRED COMPONENTS Test)

# Test output directory
set(TEST_OUTPUT_DIR "${CMAKE_BINARY_DIR}/test_results")
file(MAKE_DIRECTORY "${TEST_OUTPUT_DIR}")

# ============================================================================
# Unit Tests - Core Components
# ============================================================================

set(UNIT_TESTS
    test_path_utils
    test_logger
    test_config_manager
    test_file_scanner
    test_encryption
    test_app_scanner
    test_chocolatey_manager
    test_package_matcher
    test_migration_report
    test_file_hash
    test_user_data_manager
    test_input_validator
    test_secure_memory
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
        
        # Add to CTest
        add_test(NAME ${test_name} 
                 COMMAND ${test_name}
                 WORKING_DIRECTORY "${TEST_OUTPUT_DIR}")
        
        message(STATUS "Added unit test: ${test_name}")
    endif()
endforeach()

# ============================================================================
# Unit Tests - Action Classes
# ============================================================================

set(ACTION_TESTS
    test_disk_cleanup_action
    test_backup_browser_data_action
    test_clear_browser_cache_action
    test_check_disk_health_action
    test_optimize_power_settings_action
    test_clear_event_logs_action
    test_windows_update_action
    test_check_disk_errors_action
    test_check_bloatware_action
    test_backup_activation_keys_action
    test_sticky_notes_backup_action
    test_generate_system_report_action
    test_defragment_drives_action
    test_create_restore_point_action
    test_disable_startup_programs_action
    test_clear_windows_update_cache_action
    test_rebuild_icon_cache_action
    test_disable_visual_effects_action
    test_clear_print_spooler_action
    test_fix_audio_issues_action
    test_reset_network_action
)

# ============================================================================
# Unit Tests - Worker Classes
# ============================================================================

set(WORKER_TESTS
    test_app_migration_worker
    test_backup_worker
    test_user_profile_backup_worker
    test_user_profile_restore_worker
    test_flash_worker
    test_duplicate_finder_worker
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
    COMMAND ${CMAKE_CTEST_COMMAND} -R "test_(backup|migration)_workflow" --output-on-failure
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

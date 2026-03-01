# SAK Utility — Test Suite

## Overview

Comprehensive test suite for SAK Utility using the **Qt Test** framework. Tests cover core utilities, security, networking, orchestration, diagnostics, ISO pipelines, deployment, and quick action validation.

## Structure

```
tests/
├── unit/                          # Unit tests for individual components
│   ├── actions/                   # Quick action validation tests
│   │   └── test_action_factory.cpp
│   ├── test_assignment_queue_store.cpp
│   ├── test_config_manager.cpp
│   ├── test_decompressor_factory.cpp
│   ├── test_deployment_history.cpp
│   ├── test_deployment_manager.cpp
│   ├── test_destination_registry.cpp
│   ├── test_diagnostic_controller.cpp
│   ├── test_diagnostic_report_generator.cpp
│   ├── test_diagnostic_types.cpp
│   ├── test_drive_scanner.cpp
│   ├── test_elevation_manager.cpp
│   ├── test_encryption.cpp
│   ├── test_error_codes.cpp
│   ├── test_file_hash.cpp
│   ├── test_file_scanner.cpp
│   ├── test_input_validator.cpp
│   ├── test_linux_iso_downloader.cpp
│   ├── test_logger.cpp
│   ├── test_mapping_engine.cpp
│   ├── test_migration_orchestrator.cpp
│   ├── test_network_connection.cpp
│   ├── test_orchestration_client.cpp
│   ├── test_orchestration_discovery_service.cpp
│   ├── test_orchestration_protocol.cpp
│   ├── test_orchestration_types.cpp
│   ├── test_package_matcher.cpp
│   ├── test_parallel_transfer_manager.cpp
│   ├── test_parallel_transfer_manager_stress.cpp
│   ├── test_path_utils.cpp
│   ├── test_peer_discovery.cpp
│   ├── test_permission_manager.cpp
│   ├── test_process_runner.cpp
│   ├── test_secure_memory.cpp
│   ├── test_smart_disk_analyzer.cpp
│   ├── test_smart_file_filter.cpp
│   ├── test_streaming_decompressor.cpp
│   ├── test_thermal_monitor.cpp
│   ├── test_transfer_protocol.cpp
│   ├── test_transfer_security.cpp
│   ├── test_transfer_types.cpp
│   ├── test_windows_iso_downloader.cpp
│   └── test_worker_base.cpp
├── integration/                   # End-to-end workflow tests
│   ├── test_network_transfer_workflow.cpp
│   └── test_uup_conversion_pipeline.cpp
├── CMakeLists.txt                 # Test build configuration
└── README.md                      # This file
```

## Running Tests

### Build and Run All

```powershell
cmake --build build --config Release --target RUN_TESTS
```

### Using CTest

```powershell
cd build
ctest --build-config Release --output-on-failure

# Pattern matching
ctest -R "test_encryption"

# Verbose output
ctest -VV
```

### Custom Targets

```powershell
# All tests
cmake --build build --target run_all_tests

# Unit tests only
cmake --build build --target run_unit_tests

# Integration tests only
cmake --build build --target run_integration_tests
```

### Run a Single Test

```powershell
.\build\Release\test_path_utils.exe
.\build\Release\test_path_utils.exe -v2   # verbose
```

## Test Categories

### Core Utilities
| Test | Module Under Test | Coverage |
|---|---|---|
| test_path_utils | `path_utils` | Path validation, normalization, sanitization, env-var expansion, safe-delete checks |
| test_logger | `logger` | Log levels, formatting, file rotation, thread safety |
| test_config_manager | `config_manager` | Get/set values, persistence, portable mode, type conversions |
| test_file_scanner | `file_scanner` | Recursive scan, extension/size/date filters, cancellation |
| test_file_hash | `file_hash` | MD5/SHA-256 hashing, large-file streaming, hash comparison |
| test_process_runner | `process_runner` | Process execution, stdout/stderr capture, timeout, exit codes |
| test_error_codes | `error_codes` | Error code enum values, category strings, display messages |
| test_input_validator | `input_validator` | Path, IP, port, Chocolatey name, and URL validation |
| test_worker_base | `worker_base` | Thread lifecycle, cancellation, progress signals |

### Security & Encryption
| Test | Module Under Test | Coverage |
|---|---|---|
| test_encryption | `encryption` | AES-256-CBC encrypt/decrypt, wrong password, Unicode, file encryption, IV randomness |
| test_secure_memory | `secure_memory` | SecureString/SecureBuffer zeroing, VirtualLock, random generation |
| test_transfer_security | `network_transfer_security` | AES-256-GCM per-chunk, PBKDF2 key derivation, challenge/response auth |

### Network Transfer & Orchestration
| Test | Module Under Test | Coverage |
|---|---|---|
| test_transfer_types | `network_transfer_types` | Transfer state, file metadata, chunk structures |
| test_transfer_protocol | `network_transfer_protocol` | Protocol message serialization, versioning, handshake |
| test_peer_discovery | `peer_discovery_service` | UDP broadcast, peer registration, timeout expiry |
| test_network_connection | `network_connection_manager` | Connection lifecycle, reconnection, error handling |
| test_parallel_transfer_manager | `parallel_transfer_manager` | Concurrent transfers, queue management, bandwidth limits |
| test_parallel_transfer_manager_stress | `parallel_transfer_manager` | High-concurrency stress, thread safety under load |
| test_orchestration_types | `orchestration_types` | Job state, assignment, deployment plan structures |
| test_orchestration_protocol | `orchestration_protocol` | Multi-PC protocol messages, serialization roundtrip |
| test_orchestration_client | `orchestration_client` | Client connection, job acceptance, status reporting |
| test_orchestration_discovery_service | `orchestration_discovery_service` | Orchestrator advertisement, client discovery |

### Deployment & Migration
| Test | Module Under Test | Coverage |
|---|---|---|
| test_destination_registry | `destination_registry` | PC registration, capacity tracking, availability |
| test_deployment_manager | `deployment_manager` | Job creation, assignment, progress tracking |
| test_deployment_history | `deployment_history` | History persistence, query, cleanup |
| test_assignment_queue_store | `assignment_queue_store` | Queue serialization, ordering, deduplication |
| test_mapping_engine | `mapping_engine` | Source→destination path mapping, conflict resolution |
| test_migration_orchestrator | `migration_orchestrator` | End-to-end migration planning, rollback |
| test_package_matcher | `package_matcher` | App-to-Chocolatey matching, confidence scoring |
| test_permission_manager | `permission_manager` | NTFS ACL read/write, permission preservation |
| test_smart_file_filter | `smart_file_filter` | Category-based filtering, exclusion patterns, size thresholds |
| test_elevation_manager | `elevation_manager` | Admin privilege detection, UAC status |

### Diagnostics & Benchmarking
| Test | Module Under Test | Coverage |
|---|---|---|
| test_diagnostic_types | `diagnostic_types` | Hardware info structs, SMART data, benchmark results |
| test_diagnostic_controller | `diagnostic_controller` | Full-suite orchestration, step sequencing, cancellation |
| test_diagnostic_report_generator | `diagnostic_report_generator` | HTML/JSON/CSV generation, data formatting |
| test_smart_disk_analyzer | `smart_disk_analyzer` | smartctl output parsing, health classification, attribute extraction |
| test_thermal_monitor | `thermal_monitor` | Temperature polling, threshold alerts, auto-abort |

### ISO & Image Handling
| Test | Module Under Test | Coverage |
|---|---|---|
| test_windows_iso_downloader | `windows_iso_downloader` | UUP API, download URL validation, HTTP-allow for Microsoft CDN |
| test_linux_iso_downloader | `linux_iso_downloader` | Distro catalog, GitHub URL resolution, aria2c integration |
| test_drive_scanner | `drive_scanner` | Physical drive enumeration, USB detection, formatting info |
| test_streaming_decompressor | `streaming_decompressor` | Streaming gzip/bzip2/xz decompression, error handling |
| test_decompressor_factory | `decompressor_factory` | Format detection, correct decompressor selection |

### Quick Actions
| Test | Module Under Test | Coverage |
|---|---|---|
| test_action_factory | `action_factory` | Factory completeness, metadata validity (name, description, category), initial state, no duplicates, all categories populated |

### Integration Tests
| Test | Workflow | Coverage |
|---|---|---|
| test_network_transfer_workflow | Network Transfer | End-to-end peer discovery → connect → transfer → verify |
| test_uup_conversion_pipeline | UUP-to-ISO | Converter process attachment, config parsing, missing files, stdin close, exit handling |

## Writing New Tests

### Template

```cpp
#include "sak/my_component.h"
#include <QTest>

class TestMyComponent : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();    // Before all tests
    void cleanupTestCase(); // After all tests
    void init();            // Before each test
    void cleanup();         // After each test

    void testSomething() {
        sak::MyComponent c;
        QVERIFY(c.isValid());
        QCOMPARE(c.value(), expected);
    }
};

QTEST_MAIN(TestMyComponent)
#include "test_my_component.moc"
```

### Registering in CMakeLists.txt

Add a guarded block in `tests/CMakeLists.txt`:

```cmake
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_my_component.cpp")
    add_executable(test_my_component unit/test_my_component.cpp)
    set_target_properties(test_my_component PROPERTIES AUTOMOC ON)
    target_link_libraries(test_my_component PRIVATE
        Qt6::Test Qt6::Core
        # ... add modules your test needs
    )
    add_test(NAME test_my_component COMMAND test_my_component)
endif()
```

## Best Practices

- **Isolation:** Use `QTemporaryDir` for file operations — auto-cleaned after test
- **No hardcoded counts:** Validate structure and invariants, not exact numbers
- **One concept per test:** Keep tests focused and named descriptively
- **Fast:** Target < 1 second per test (use mocks for slow operations)
- **Edge cases:** Test empty input, max values, boundary conditions, error paths

## QTest Quick Reference

```cpp
QVERIFY(condition)                       // Assert true
QVERIFY2(condition, "message")           // Assert with message
QCOMPARE(actual, expected)               // Assert equality
QSKIP("reason")                          // Skip test

QSignalSpy spy(&obj, &Obj::signal);      // Signal monitoring
QCOMPARE(spy.count(), 1);
```

---

**Test Framework:** Qt Test  
**Platform:** Windows 10/11

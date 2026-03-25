# SAK Utility — Test Suite

## Overview

Comprehensive test suite for SAK Utility using the **Qt Test** framework with **91 registered CTest tests** across **117 test files**. Tests cover core utilities, security, networking, orchestration, diagnostics, ISO pipelines, deployment, email inspection, and quick action validation.

## Structure

```
tests/
├── unit/                                  # Unit tests (112 files)
│   ├── actions/                           # Quick action validation tests
│   │   ├── test_action_factory.cpp
│   │   └── test_all_actions_metadata.cpp
│   ├── test_active_connections_monitor.cpp
│   ├── test_advanced_search_controller.cpp
│   ├── test_advanced_search_types.cpp
│   ├── test_advanced_search_worker.cpp
│   ├── test_advanced_uninstall_controller.cpp
│   ├── test_advanced_uninstall_types.cpp
│   ├── test_app_installation_worker.cpp
│   ├── test_app_scanner.cpp
│   ├── test_assignment_queue_store.cpp
│   ├── test_bandwidth_tester.cpp
│   ├── test_bundled_tools_manager.cpp
│   ├── test_chocolatey_manager.cpp
│   ├── test_cleanup_worker.cpp
│   ├── test_config_manager.cpp
│   ├── test_connectivity_tester.cpp
│   ├── test_cpu_benchmark_worker.cpp
│   ├── test_decompressor_factory.cpp
│   ├── test_deployment_history.cpp
│   ├── test_deployment_manager.cpp
│   ├── test_deployment_summary_report.cpp
│   ├── test_destination_registry.cpp
│   ├── test_diagnostic_controller.cpp
│   ├── test_diagnostic_report_generator.cpp
│   ├── test_diagnostic_types.cpp
│   ├── test_disk_benchmark_worker.cpp
│   ├── test_dns_diagnostic_tool.cpp
│   ├── test_drive_scanner.cpp
│   ├── test_drive_unmounter.cpp
│   ├── test_duplicate_finder_worker.cpp
│   ├── test_elevation_manager.cpp
│   ├── test_email_export_worker.cpp
│   ├── test_email_profile_manager.cpp
│   ├── test_email_report_generator.cpp
│   ├── test_email_search_worker.cpp
│   ├── test_email_types.cpp
│   ├── test_encryption.cpp
│   ├── test_error_codes.cpp
│   ├── test_ethernet_config_manager.cpp
│   ├── test_file_hash.cpp
│   ├── test_file_scanner.cpp
│   ├── test_firewall_rule_auditor.cpp
│   ├── test_flash_coordinator.cpp
│   ├── test_flash_types.cpp
│   ├── test_flash_worker.cpp
│   ├── test_format_utils.cpp
│   ├── test_hardware_inventory_scanner.cpp
│   ├── test_image_source.cpp
│   ├── test_input_validator.cpp
│   ├── test_keep_awake.cpp
│   ├── test_leftover_scanner.cpp
│   ├── test_linux_distro_catalog.cpp
│   ├── test_linux_iso_downloader.cpp
│   ├── test_logger.cpp
│   ├── test_mapping_engine.cpp
│   ├── test_mbox_parser.cpp
│   ├── test_memory_benchmark_worker.cpp
│   ├── test_migration_orchestrator.cpp
│   ├── test_migration_report.cpp
│   ├── test_network_adapter_inspector.cpp
│   ├── test_network_connection.cpp
│   ├── test_network_diagnostic_controller.cpp
│   ├── test_network_diagnostic_report.cpp
│   ├── test_network_diagnostic_types.cpp
│   ├── test_network_diagnostic_utils.cpp
│   ├── test_network_share_browser.cpp
│   ├── test_network_transfer_controller.cpp
│   ├── test_network_transfer_report.cpp
│   ├── test_network_transfer_worker.cpp
│   ├── test_orchestration_client.cpp
│   ├── test_orchestration_discovery_service.cpp
│   ├── test_orchestration_protocol.cpp
│   ├── test_orchestration_server.cpp
│   ├── test_orchestration_types.cpp
│   ├── test_organizer_worker.cpp
│   ├── test_package_matcher.cpp
│   ├── test_parallel_transfer_manager.cpp
│   ├── test_parallel_transfer_manager_stress.cpp
│   ├── test_path_utils.cpp
│   ├── test_peer_discovery.cpp
│   ├── test_permission_manager.cpp
│   ├── test_port_scanner.cpp
│   ├── test_process_runner.cpp
│   ├── test_program_enumerator.cpp
│   ├── test_pst_parser.cpp
│   ├── test_quick_action.cpp
│   ├── test_quick_action_controller.cpp
│   ├── test_quick_action_result_io.cpp
│   ├── test_regex_pattern_library.cpp
│   ├── test_registry_snapshot_engine.cpp
│   ├── test_restore_point_manager.cpp
│   ├── test_secure_memory.cpp
│   ├── test_smart_disk_analyzer.cpp
│   ├── test_smart_file_filter.cpp
│   ├── test_streaming_decompressor.cpp
│   ├── test_stress_test_worker.cpp
│   ├── test_thermal_monitor.cpp
│   ├── test_transfer_protocol.cpp
│   ├── test_transfer_security.cpp
│   ├── test_transfer_types.cpp
│   ├── test_uninstall_worker.cpp
│   ├── test_user_data_manager.cpp
│   ├── test_user_profile_backup_worker.cpp
│   ├── test_user_profile_restore_worker.cpp
│   ├── test_user_profile_types.cpp
│   ├── test_uup_dump_api.cpp
│   ├── test_uup_iso_builder.cpp
│   ├── test_wifi_analyzer.cpp
│   ├── test_wifi_profile_scanner.cpp
│   ├── test_windows_iso_downloader.cpp
│   ├── test_windows_usb_creator.cpp
│   ├── test_windows_user_scanner.cpp
│   └── test_worker_base.cpp
├── integration/                           # End-to-end workflow tests (3 files)
│   ├── test_network_transfer_workflow.cpp
│   ├── test_ost_integration.cpp
│   └── test_uup_conversion_pipeline.cpp
├── CMakeLists.txt                         # Test build configuration
└── README.md                              # This file
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
| test_format_utils | `format_utils` | Number/size/duration formatting helpers |
| test_bundled_tools_manager | `bundled_tools_manager` | Tool detection, path resolution, version checks |
| test_keep_awake | `keep_awake` | System sleep prevention, cleanup on destruction |

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
| test_network_transfer_controller | `network_transfer_controller` | Transfer orchestration, state machine, error recovery |
| test_network_transfer_worker | `network_transfer_worker` | File chunking, send/receive, integrity verification |
| test_network_transfer_report | `network_transfer_report` | Transfer report generation, statistics formatting |
| test_parallel_transfer_manager | `parallel_transfer_manager` | Concurrent transfers, queue management, bandwidth limits |
| test_parallel_transfer_manager_stress | `parallel_transfer_manager` | High-concurrency stress, thread safety under load |
| test_orchestration_types | `orchestration_types` | Job state, assignment, deployment plan structures |
| test_orchestration_protocol | `orchestration_protocol` | Multi-PC protocol messages, serialization roundtrip |
| test_orchestration_client | `orchestration_client` | Client connection, job acceptance, status reporting |
| test_orchestration_server | `orchestration_server` | Server lifecycle, client management, job dispatch |
| test_orchestration_discovery_service | `orchestration_discovery_service` | Orchestrator advertisement, client discovery |
| test_connectivity_tester | `connectivity_tester` | Network reachability, timeout handling |

### Deployment & Migration
| Test | Module Under Test | Coverage |
|---|---|---|
| test_destination_registry | `destination_registry` | PC registration, capacity tracking, availability |
| test_deployment_manager | `deployment_manager` | Job creation, assignment, progress tracking |
| test_deployment_history | `deployment_history` | History persistence, query, cleanup |
| test_deployment_summary_report | `deployment_summary_report` | Deployment report generation, formatting |
| test_assignment_queue_store | `assignment_queue_store` | Queue serialization, ordering, deduplication |
| test_mapping_engine | `mapping_engine` | Source→destination path mapping, conflict resolution |
| test_migration_orchestrator | `migration_orchestrator` | End-to-end migration planning, rollback |
| test_migration_report | `migration_report` | Migration report generation |
| test_package_matcher | `package_matcher` | App-to-Chocolatey matching, confidence scoring |
| test_chocolatey_manager | `chocolatey_manager` | Package install, update, version queries |
| test_app_scanner | `app_scanner` | Installed app enumeration from registry |
| test_app_installation_worker | `app_installation_worker` | Package installation workflow, retry, error handling |
| test_permission_manager | `permission_manager` | NTFS ACL read/write, permission preservation |
| test_smart_file_filter | `smart_file_filter` | Category-based filtering, exclusion patterns, size thresholds |
| test_elevation_manager | `elevation_manager` | Admin privilege detection, UAC status |
| test_user_data_manager | `user_data_manager` | User data discovery and categorization |
| test_user_profile_types | `user_profile_types` | Profile data structures, serialization |
| test_user_profile_backup_worker | `user_profile_backup_worker` | Backup workflow, compression, encryption |
| test_user_profile_restore_worker | `user_profile_restore_worker` | Restore workflow, permission handling, merge |
| test_windows_user_scanner | `windows_user_scanner` | Windows user profile enumeration |
| test_cleanup_worker | `cleanup_worker` | Temp file cleanup, cache clearing |
| test_restore_point_manager | `restore_point_manager` | System restore point creation |

### Diagnostics & Benchmarking
| Test | Module Under Test | Coverage |
|---|---|---|
| test_diagnostic_types | `diagnostic_types` | Hardware info structs, SMART data, benchmark results |
| test_diagnostic_controller | `diagnostic_controller` | Full-suite orchestration, step sequencing, cancellation |
| test_diagnostic_report_generator | `diagnostic_report_generator` | HTML/JSON/CSV generation, data formatting |
| test_smart_disk_analyzer | `smart_disk_analyzer` | smartctl output parsing, health classification, attribute extraction |
| test_thermal_monitor | `thermal_monitor` | Temperature polling, threshold alerts, auto-abort |
| test_hardware_inventory_scanner | `hardware_inventory_scanner` | CPU, memory, GPU, OS info collection |
| test_cpu_benchmark_worker | `cpu_benchmark_worker` | Single/multi-thread scoring, matrix multiply |
| test_disk_benchmark_worker | `disk_benchmark_worker` | Sequential/random I/O, IOPS measurement |
| test_memory_benchmark_worker | `memory_benchmark_worker` | Read/write bandwidth, latency |
| test_stress_test_worker | `stress_test_worker` | CPU/memory/disk stress, error detection |

### Network Diagnostics
| Test | Module Under Test | Coverage |
|---|---|---|
| test_network_diagnostic_types | `network_diagnostic_types` | Diagnostic result structures |
| test_network_diagnostic_utils | `network_diagnostic_utils` | IP parsing, format helpers |
| test_network_diagnostic_controller | `network_diagnostic_controller` | Tool orchestration, result aggregation |
| test_network_diagnostic_report | `network_diagnostic_report` | HTML/JSON report generation |
| test_network_adapter_inspector | `network_adapter_inspector` | Adapter enumeration, config parsing |
| test_ethernet_config_manager | `ethernet_config_manager` | Ethernet backup/restore, JSON persistence |
| test_dns_diagnostic_tool | `dns_diagnostic_tool` | DNS queries, record type parsing |
| test_port_scanner | `port_scanner` | TCP port scanning, banner grabbing |
| test_bandwidth_tester | `bandwidth_tester` | iPerf3/HTTP speed testing |
| test_active_connections_monitor | `active_connections_monitor` | TCP/UDP connection listing |
| test_firewall_rule_auditor | `firewall_rule_auditor` | Firewall rule enumeration, conflict detection |
| test_network_share_browser | `network_share_browser` | SMB share discovery |
| test_wifi_analyzer | `wifi_analyzer` | WiFi network scanning, signal analysis |

### ISO & Image Handling
| Test | Module Under Test | Coverage |
|---|---|---|
| test_windows_iso_downloader | `windows_iso_downloader` | UUP API, download URL validation, HTTP-allow for Microsoft CDN |
| test_linux_iso_downloader | `linux_iso_downloader` | Distro catalog, GitHub URL resolution, aria2c integration |
| test_linux_distro_catalog | `linux_distro_catalog` | Distro metadata, URL patterns, version detection |
| test_uup_dump_api | `uup_dump_api` | UUP Dump API parsing, edition/language selection |
| test_uup_iso_builder | `uup_iso_builder` | UUP-to-ISO conversion, converter process management |
| test_drive_scanner | `drive_scanner` | Physical drive enumeration, USB detection, formatting info |
| test_drive_unmounter | `drive_unmounter` | Volume unmount, lock/unlock |
| test_flash_types | `flash_types` | Flash operation data structures |
| test_flash_worker | `flash_worker` | Raw sector write, verification, decompression |
| test_flash_coordinator | `flash_coordinator` | Multi-drive flash orchestration |
| test_image_source | `image_source` | Image file detection, format identification |
| test_windows_usb_creator | `windows_usb_creator` | NTFS format, ISO extraction, boot sector |
| test_streaming_decompressor | `streaming_decompressor` | Streaming gzip/bzip2/xz decompression, error handling |
| test_decompressor_factory | `decompressor_factory` | Format detection, correct decompressor selection |
| test_wifi_profile_scanner | `wifi_profile_scanner` | WiFi profile enumeration, export |

### Advanced Search
| Test | Module Under Test | Coverage |
|---|---|---|
| test_advanced_search_types | `advanced_search_types` | Search result structures, match types |
| test_advanced_search_controller | `advanced_search_controller` | Search orchestration, result aggregation |
| test_advanced_search_worker | `advanced_search_worker` | Content search, regex, binary/hex, metadata, archive |
| test_regex_pattern_library | `regex_pattern_library` | Built-in patterns, custom pattern persistence |

### Advanced Uninstall
| Test | Module Under Test | Coverage |
|---|---|---|
| test_advanced_uninstall_types | `advanced_uninstall_types` | Program info, leftover structures, risk levels |
| test_advanced_uninstall_controller | `advanced_uninstall_controller` | Uninstall orchestration, batch queue |
| test_leftover_scanner | `leftover_scanner` | File/registry/service leftover detection |
| test_registry_snapshot_engine | `registry_snapshot_engine` | Pre/post snapshot, diff computation |
| test_program_enumerator | `program_enumerator` | Win32/UWP/provisioned app enumeration |
| test_uninstall_worker | `uninstall_worker` | Uninstall execution, cleanup |

### Directory Organizer
| Test | Module Under Test | Coverage |
|---|---|---|
| test_organizer_worker | `organizer_worker` | File organization by category, collision handling |
| test_duplicate_finder_worker | `duplicate_finder_worker` | MD5 hashing, duplicate detection, parallel scan |

### Email Inspector
| Test | Module Under Test | Coverage |
|---|---|---|
| test_email_types | `email_types` | Email data structures, item types, MAPI properties |
| test_pst_parser | `pst_parser` | PST file parsing, folder tree, item detail, attachments |
| test_mbox_parser | `mbox_parser` | MBOX parsing, message extraction, header parsing |
| test_email_search_worker | `email_search_worker` | Full-text search, field filtering |
| test_email_export_worker | `email_export_worker` | EML/CSV/VCF/ICS export, attachment extraction |
| test_email_profile_manager | `email_profile_manager` | Email client profile discovery, backup/restore |
| test_email_report_generator | `email_report_generator` | Email analysis report generation |

### Quick Actions
| Test | Module Under Test | Coverage |
|---|---|---|
| test_action_factory | `action_factory` | Factory completeness, metadata validity (name, description, category), initial state, no duplicates, all categories populated |
| test_all_actions_metadata | `action_factory` | Validates every registered action has complete metadata |
| test_quick_action | `quick_action` | Base action interface, state transitions |
| test_quick_action_controller | `quick_action_controller` | Action execution, progress, cancellation |
| test_quick_action_result_io | `quick_action_result_io` | Result serialization, history persistence |

### Integration Tests
| Test | Workflow | Coverage |
|---|---|---|
| test_network_transfer_workflow | Network Transfer | End-to-end peer discovery → connect → transfer → verify |
| test_ost_integration | Email Inspector | OST file opening, folder tree, item loading |
| test_uup_conversion_pipeline | UUP-to-ISO | UUPMediaConverter exe existence, process attachment (no fork), help output, missing UUP dir, empty UUP dir |

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
**Registered Tests:** 91 (CTest)
**Test Files:** 117 (112 unit + 2 action + 3 integration)
**Platform:** Windows 10/11

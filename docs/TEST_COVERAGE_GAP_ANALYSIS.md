# Test Coverage Gap Analysis — S.A.K. Utility

**Generated:** 2026-02-25  
**Scope:** All production code under `src/` and `include/` vs. test files under `tests/`

---

## Summary

| Category | Count |
|----------|-------|
| Total production source files (`src/`) | **95** |
| Total header files (`include/`) | **104** |
| Unique classes/modules with dedicated unit tests | **23** |
| Integration test files | **2** |
| Fully tested modules | **20** |
| Partially tested modules | **5** |
| Not tested (testable) | **46** |
| Not testable (GUI/hardware/platform) | **24** |

**Estimated unit test coverage of testable code: ~34%**

---

## 1. Complete Production File Test Status Matrix

### Legend
- ✅ **Fully Tested** — Dedicated test file with meaningful method-level tests
- 🟡 **Partially Tested** — Tested indirectly or only superficial coverage
- ❌ **Not Tested** — No test coverage at all
- 🔘 **Not Testable** — GUI widget, `main.cpp`, or platform-specific code requiring hardware

---

### `src/core/` — Core Business Logic (75 files)

| # | Source File | Header | Test Status | Test File | Notes |
|---|-----------|--------|-------------|-----------|-------|
| 1 | `assignment_queue_store.cpp` | `assignment_queue_store.h` | ✅ Fully | `test_assignment_queue_store.cpp` | 1 test (save/load roundtrip) |
| 2 | `deployment_history.cpp` | `deployment_history.h` | ✅ Fully | `test_deployment_history.cpp` | 2 tests (append/load, CSV export) |
| 3 | `destination_registry.cpp` | `destination_registry.h` | ✅ Fully | `test_destination_registry.cpp` | 2 tests (register/update, readiness) |
| 4 | `diagnostic_controller.cpp` | `diagnostic_controller.h` | ✅ Fully | `test_diagnostic_controller.cpp` | 5 tests (state, cancel, access) |
| 5 | `diagnostic_report_generator.cpp` | `diagnostic_report_generator.h` | ✅ Fully | `test_diagnostic_report_generator.cpp` | 7 tests (HTML/JSON/CSV generation, content validation) |
| 6 | `mapping_engine.cpp` | `mapping_engine.h` | ✅ Fully | `test_mapping_engine.cpp` | 3 tests (selection strategies, persistence) |
| 7 | `migration_orchestrator.cpp` | `migration_orchestrator.h` | ✅ Fully | `test_migration_orchestrator.cpp` | 7 tests (health, deployment, progress, assignment queuing) |
| 8 | `network_connection_manager.cpp` | `network_connection_manager.h` | 🟡 Partial | `test_network_connection.cpp` | 1 test only (loopback connect) |
| 9 | `network_transfer_protocol.cpp` | `network_transfer_protocol.h` | ✅ Fully | `test_transfer_protocol.cpp` | 1 test (encode/decode roundtrip) |
| 10 | `network_transfer_security.cpp` | `network_transfer_security.h` | ✅ Fully | `test_transfer_security.cpp` | 1 test (AES-GCM roundtrip) |
| 11 | `network_transfer_types.cpp` | `network_transfer_types.h` | ✅ Fully | `test_transfer_types.cpp` | 1 test (serialization roundtrip) |
| 12 | `orchestration_client.cpp` | `orchestration_client.h` | ✅ Fully | `test_orchestration_client.cpp` | 3 tests (assignment receive, control, reconnect) |
| 13 | `orchestration_discovery_service.cpp` | `orchestration_discovery_service.h` | ✅ Fully | `test_orchestration_discovery_service.cpp` | 2 tests (discovery response, announcement) |
| 14 | `orchestration_protocol.cpp` | `orchestration_protocol.h` | ✅ Fully | `test_orchestration_protocol.cpp` | 1 test (encode/decode roundtrip) |
| 15 | `orchestration_types.cpp` | `orchestration_types.h` | ✅ Fully | `test_orchestration_types.cpp` | 3 tests (assignment/progress/completion serialization) |
| 16 | `parallel_transfer_manager.cpp` | `parallel_transfer_manager.h` | ✅ Fully | `test_parallel_transfer_manager.cpp` + `_stress.cpp` | 8 tests total (concurrency, cancel, priority, retry, bandwidth) |
| 17 | `peer_discovery_service.cpp` | `peer_discovery_service.h` | ✅ Fully | `test_peer_discovery.cpp` | 1 test (peer discovery) |
| 18 | `thermal_monitor.cpp` | `thermal_monitor.h` | ✅ Fully | `test_thermal_monitor.cpp` | 5 tests (state, start/stop, poll, clear, timer) |
| 19 | `windows_iso_downloader.cpp` | `windows_iso_downloader.h` | ✅ Fully | `test_windows_iso_downloader.cpp` | 5 tests (architectures, channels, fetch, cancel, display names) |
| 20 | `network_transfer_controller.cpp` | `network_transfer_controller.h` | 🟡 Partial | integration `test_network_transfer_workflow.cpp` | 5 integration tests (encrypted transfer, multi-file, resume, throttle) — no unit tests |
| 21 | `app_migration_worker.cpp` | `app_migration_worker.h` | ❌ Not Tested | — | Migration business logic |
| 22 | `app_scanner.cpp` | `app_scanner.h` | ❌ Not Tested | — | App discovery/scanning |
| 23 | `bundled_tools_manager.cpp` | `bundled_tools_manager.h` | ❌ Not Tested | — | Tool bundling management |
| 24 | `bzip2_decompressor.cpp` | `bzip2_decompressor.h` | ❌ Not Tested | — | Decompression logic |
| 25 | `chocolatey_manager.cpp` | `chocolatey_manager.h` | ❌ Not Tested | — | Package management |
| 26 | `config_manager.cpp` | `config_manager.h` | ❌ Not Tested | — | **20+ methods**, settings persistence |
| 27 | `cpu_benchmark_worker.cpp` | `cpu_benchmark_worker.h` | ❌ Not Tested | — | Benchmark worker |
| 28 | `decompressor_factory.cpp` | `decompressor_factory.h` | ❌ Not Tested | — | Factory pattern, testable |
| 29 | `deployment_manager.cpp` | `deployment_manager.h` | ❌ Not Tested | — | Queue management, 8 methods |
| 30 | `deployment_summary_report.cpp` | `deployment_summary_report.h` | ❌ Not Tested | — | Report generation |
| 31 | `disk_benchmark_worker.cpp` | `disk_benchmark_worker.h` | ❌ Not Tested | — | Benchmark worker |
| 32 | `drive_lock.cpp` | `drive_lock.h` | ❌ Not Tested | — | Platform-specific |
| 33 | `drive_scanner.cpp` | `drive_scanner.h` | ❌ Not Tested | — | 15+ methods, hardware-dependent |
| 34 | `drive_unmounter.cpp` | `drive_unmounter.h` | ❌ Not Tested | — | Platform-specific |
| 35 | `elevation_manager.cpp` | `elevation_manager.h` | ❌ Not Tested | — | Windows privilege escalation |
| 36 | `encryption.cpp` | `encryption.h` | ❌ Not Tested | — | **CRITICAL: AES-256 encrypt/decrypt, PBKDF2 key derivation** |
| 37 | `file_hash.cpp` | `file_hash.h` | 🟡 Partial | integration only | Used in integration test but no dedicated unit tests. 8 methods. |
| 38 | `file_scanner.cpp` | `file_scanner.h` | ❌ Not Tested | — | File traversal, 7 methods |
| 39 | `flash_coordinator.cpp` | `flash_coordinator.h` | ❌ Not Tested | — | Disk flashing coordination |
| 40 | `gzip_decompressor.cpp` | `gzip_decompressor.h` | ❌ Not Tested | — | Decompression logic |
| 41 | `hardware_inventory_scanner.cpp` | `hardware_inventory_scanner.h` | ❌ Not Tested | — | Hardware enumeration |
| 42 | `image_source.cpp` | `image_source.h` | ❌ Not Tested | — | Image source abstraction |
| 43 | `input_validator.cpp` | `input_validator.h` | ❌ Not Tested | — | **CRITICAL: 19 validation methods, path traversal protection** |
| 44 | `keep_awake.cpp` | `keep_awake.h` | ❌ Not Tested | — | Platform-specific |
| 45 | `linux_distro_catalog.cpp` | `linux_distro_catalog.h` | ❌ Not Tested | — | Catalog data |
| 46 | `linux_iso_downloader.cpp` | `linux_iso_downloader.h` | ❌ Not Tested | — | Network download logic |
| 47 | `logger.cpp` | `logger.h` | ❌ Not Tested | — | 15 methods, singleton |
| 48 | `memory_benchmark_worker.cpp` | `memory_benchmark_worker.h` | ❌ Not Tested | — | Benchmark worker |
| 49 | `migration_report.cpp` | `migration_report.h` | ❌ Not Tested | — | Report generation |
| 50 | `network_transfer_report.cpp` | `network_transfer_report.h` | ❌ Not Tested | — | Transfer reporting |
| 51 | `network_transfer_worker.cpp` | `network_transfer_worker.h` | ❌ Not Tested | — | Transfer execution |
| 52 | `orchestration_server.cpp` | `orchestration_server.h` | ❌ Not Tested | — | Server-side orchestration |
| 53 | `package_matcher.cpp` | `package_matcher.h` | ❌ Not Tested | — | Package matching logic |
| 54 | `path_utils.cpp` | `path_utils.h` | ❌ Not Tested | — | **CRITICAL: 20+ path manipulation/security methods** |
| 55 | `permission_manager.cpp` | `permission_manager.h` | ❌ Not Tested | — | Permission management |
| 56 | `process_runner.cpp` | `process_runner.h` | ❌ Not Tested | — | Process execution |
| 57 | `quick_action.cpp` | `quick_action.h` | ❌ Not Tested | — | Action state management |
| 58 | `quick_action_controller.cpp` | `quick_action_controller.h` | ❌ Not Tested | — | 20 methods, orchestrates actions |
| 59 | `quick_action_result_io.cpp` | `quick_action_result_io.h` | ❌ Not Tested | — | Result serialization |
| 60 | `secure_memory.cpp` | `secure_memory.h` | ❌ Not Tested | — | **CRITICAL: Secure random gen, memory locking** |
| 61 | `smart_disk_analyzer.cpp` | `smart_disk_analyzer.h` | ❌ Not Tested | — | 13 methods, SMART parsing logic |
| 62 | `smart_file_filter.cpp` | `smart_file_filter.h` | ❌ Not Tested | — | 11 methods, filtering logic |
| 63 | `streaming_decompressor.cpp` | `streaming_decompressor.h` | ❌ Not Tested | — | Streaming decompression |
| 64 | `stress_test_worker.cpp` | `stress_test_worker.h` | ❌ Not Tested | — | Stress testing |
| 65 | `user_data_manager.cpp` | `user_data_manager.h` | ❌ Not Tested | — | **20+ methods, backup/restore logic** |
| 66 | `user_profile_backup_worker.cpp` | `user_profile_backup_worker.h` | ❌ Not Tested | — | Profile backup |
| 67 | `user_profile_restore_worker.cpp` | `user_profile_restore_worker.h` | ❌ Not Tested | — | Profile restore |
| 68 | `user_profile_types.cpp` | `user_profile_types.h` | ❌ Not Tested | — | Data types |
| 69 | `uup_dump_api.cpp` | `uup_dump_api.h` | 🟡 Partial | via `test_windows_iso_downloader.cpp` | Included in ISO downloader tests but not directly tested |
| 70 | `uup_iso_builder.cpp` | `uup_iso_builder.h` | ❌ Not Tested | — | ISO build logic |
| 71 | `windows_usb_creator.cpp` | `windows_usb_creator.h` | ❌ Not Tested | — | USB creation (platform) |
| 72 | `windows_user_scanner.cpp` | `windows_user_scanner.h` | ❌ Not Tested | — | Windows user enum |
| 73 | `xz_decompressor.cpp` | `xz_decompressor.h` | ❌ Not Tested | — | Decompression logic |

### `src/threading/` — Worker Threads (5 files)

| # | Source File | Header | Test Status | Notes |
|---|-----------|--------|-------------|-------|
| 74 | `worker_base.cpp` | `worker_base.h` | ✅ Fully | 9 tests (execution, exceptions, cancellation, progress) |
| 75 | `backup_worker.cpp` | `backup_worker.h` | ❌ Not Tested | Backup thread logic |
| 76 | `duplicate_finder_worker.cpp` | `duplicate_finder_worker.h` | ❌ Not Tested | Duplicate detection |
| 77 | `flash_worker.cpp` | `flash_worker.h` | ❌ Not Tested | Flash writing |
| 78 | `organizer_worker.cpp` | `organizer_worker.h` | ❌ Not Tested | File organization |

### `src/actions/` — Quick Actions (40 files)

| # | Source File | Header | Test Status | Notes |
|---|-----------|--------|-------------|-------|
| 79 | `action_factory.cpp` | `action_factory.h` | ❌ Not Tested | Factory for all actions |
| 80 | `backup_activation_keys_action.cpp` | `backup_activation_keys_action.h` | ❌ Not Tested | System action |
| 81 | `backup_bitlocker_keys_action.cpp` | `backup_bitlocker_keys_action.h` | ❌ Not Tested | Security-sensitive action |
| 82 | `backup_browser_data_action.cpp` | `backup_browser_data_action.h` | ❌ Not Tested | Data backup action |
| 83 | `backup_desktop_wallpaper_action.cpp` | `backup_desktop_wallpaper_action.h` | ❌ Not Tested | Simple backup |
| 84 | `backup_email_data_action.cpp` | `backup_email_data_action.h` | ❌ Not Tested | Email backup |
| 85 | `backup_printer_settings_action.cpp` | `backup_printer_settings_action.h` | ❌ Not Tested | Printer settings |
| 86 | `browser_profile_backup_action.cpp` | `browser_profile_backup_action.h` | ❌ Not Tested | Browser backup |
| 87 | `check_bloatware_action.cpp` | `check_bloatware_action.h` | ❌ Not Tested | Bloatware detection |
| 88 | `check_disk_errors_action.cpp` | `check_disk_errors_action.h` | ❌ Not Tested | Disk checks |
| 89 | `check_disk_health_action.cpp` | `check_disk_health_action.h` | ❌ Not Tested | Disk health |
| 90 | `clear_browser_cache_action.cpp` | `clear_browser_cache_action.h` | ❌ Not Tested | Cache clearing |
| 91 | `clear_event_logs_action.cpp` | `clear_event_logs_action.h` | ❌ Not Tested | Log clearing |
| 92 | `clear_print_spooler_action.cpp` | `clear_print_spooler_action.h` | ❌ Not Tested | Spooler management |
| 93 | `clear_windows_update_cache_action.cpp` | `clear_windows_update_cache_action.h` | ❌ Not Tested | Cache clearing |
| 94 | `create_restore_point_action.cpp` | `create_restore_point_action.h` | ❌ Not Tested | System restore |
| 95 | `defragment_drives_action.cpp` | `defragment_drives_action.h` | ❌ Not Tested | Disk defrag |
| 96 | `development_configs_backup_action.cpp` | `development_configs_backup_action.h` | ❌ Not Tested | Dev config backup |
| 97 | `disable_startup_programs_action.cpp` | `disable_startup_programs_action.h` | ❌ Not Tested | Startup management |
| 98 | `disable_visual_effects_action.cpp` | `disable_visual_effects_action.h` | ❌ Not Tested | Visual effects |
| 99 | `disk_cleanup_action.cpp` | `disk_cleanup_action.h` | ❌ Not Tested | Disk cleanup |
| 100 | `export_registry_keys_action.cpp` | `export_registry_keys_action.h` | ❌ Not Tested | Registry export |
| 101 | `fix_audio_issues_action.cpp` | `fix_audio_issues_action.h` | ❌ Not Tested | Audio repair |
| 102 | `generate_system_report_action.cpp` | `generate_system_report_action.h` | ❌ Not Tested | System report |
| 103 | `optimize_power_settings_action.cpp` | `optimize_power_settings_action.h` | ❌ Not Tested | Power settings |
| 104 | `outlook_backup_action.cpp` | `outlook_backup_action.h` | ❌ Not Tested | Outlook backup |
| 105 | `photo_management_backup_action.cpp` | `photo_management_backup_action.h` | ❌ Not Tested | Photo backup |
| 106 | `quickbooks_backup_action.cpp` | `quickbooks_backup_action.h` | ❌ Not Tested | QB backup |
| 107 | `rebuild_icon_cache_action.cpp` | `rebuild_icon_cache_action.h` | ❌ Not Tested | Icon cache |
| 108 | `repair_windows_store_action.cpp` | `repair_windows_store_action.h` | ❌ Not Tested | Store repair |
| 109 | `reset_network_action.cpp` | `reset_network_action.h` | ❌ Not Tested | Network reset |
| 110 | `saved_game_data_backup_action.cpp` | `saved_game_data_backup_action.h` | ❌ Not Tested | Game data backup |
| 111 | `scan_malware_action.cpp` | `scan_malware_action.h` | ❌ Not Tested | Malware scanning |
| 112 | `screenshot_settings_action.cpp` | `screenshot_settings_action.h` | ❌ Not Tested | Screenshot settings |
| 113 | `sticky_notes_backup_action.cpp` | `sticky_notes_backup_action.h` | ❌ Not Tested | Sticky notes |
| 114 | `tax_software_backup_action.cpp` | `tax_software_backup_action.h` | ❌ Not Tested | Tax software |
| 115 | `test_network_speed_action.cpp` | `test_network_speed_action.h` | ❌ Not Tested | Speed test |
| 116 | `update_all_apps_action.cpp` | `update_all_apps_action.h` | ❌ Not Tested | App updates |
| 117 | `verify_system_files_action.cpp` | `verify_system_files_action.h` | ❌ Not Tested | SFC verification |
| 118 | `windows_update_action.cpp` | `windows_update_action.h` | ❌ Not Tested | Windows Update |

### `src/gui/` — GUI Widgets (24 files)

| # | Source File | Header | Test Status | Notes |
|---|-----------|--------|-------------|-------|
| 119 | `main_window.cpp` | `main_window.h` | 🔘 Not Testable | Main UI container |
| 120 | `about_dialog.cpp` | `about_dialog.h` | 🔘 Not Testable | Dialog widget |
| 121 | `app_migration_panel.cpp` | `app_migration_panel.h` | 🔘 Not Testable | GUI panel |
| 122 | `backup_panel.cpp` | `backup_panel.h` | 🔘 Not Testable | GUI panel |
| 123 | `backup_wizard.cpp` | `backup_wizard.h` | 🔘 Not Testable | Wizard widget |
| 124 | `diagnostic_benchmark_panel.cpp` | `diagnostic_benchmark_panel.h` | 🔘 Not Testable | GUI panel |
| 125 | `duplicate_finder_panel.cpp` | `duplicate_finder_panel.h` | 🔘 Not Testable | GUI panel |
| 126 | `file_list_widget.cpp` | `file_list_widget.h` | 🔘 Not Testable | Widget |
| 127 | `image_flasher_panel.cpp` | `image_flasher_panel.h` | 🔘 Not Testable | GUI panel |
| 128 | `image_flasher_settings_dialog.cpp` | `image_flasher_settings_dialog.h` | 🔘 Not Testable | Dialog |
| 129 | `linux_iso_download_dialog.cpp` | `linux_iso_download_dialog.h` | 🔘 Not Testable | Dialog |
| 130 | `log_viewer.cpp` | `log_viewer.h` | 🔘 Not Testable | Viewer widget |
| 131 | `network_transfer_panel.cpp` | `network_transfer_panel.h` | 🔘 Not Testable | GUI panel |
| 132 | `organizer_panel.cpp` | `organizer_panel.h` | 🔘 Not Testable | GUI panel |
| 133 | `per_user_customization_dialog.cpp` | `per_user_customization_dialog.h` | 🔘 Not Testable | Dialog |
| 134 | `progress_dialog.cpp` | `progress_dialog.h` | 🔘 Not Testable | Dialog |
| 135 | `quick_actions_panel.cpp` | `quick_actions_panel.h` | 🔘 Not Testable | GUI panel |
| 136 | `restore_wizard.cpp` | `restore_wizard.h` | 🔘 Not Testable | Wizard widget |
| 137 | `settings_dialog.cpp` | `settings_dialog.h` | 🔘 Not Testable | Dialog |
| 138 | `splash_screen.cpp` | `splash_screen.h` | 🔘 Not Testable | Splash widget |
| 139 | `user_profile_backup_wizard.cpp` | `user_profile_backup_wizard.h` | 🔘 Not Testable | Wizard widget |
| 140 | `user_profile_restore_wizard.cpp` | `user_profile_restore_wizard.h` | 🔘 Not Testable | Wizard widget |
| 141 | `windows11_theme.cpp` | `windows11_theme.h` | 🔘 Not Testable | Theme/styling |
| 142 | `windows_iso_download_dialog.cpp` | `windows_iso_download_dialog.h` | 🔘 Not Testable | Dialog |

### Other

| # | Source File | Test Status | Notes |
|---|-----------|-------------|-------|
| 143 | `main.cpp` | 🔘 Not Testable | Application entry point |

### Header-only (`include/sak/`)

| Header | Test Status | Notes |
|--------|-------------|-------|
| `error_codes.h` | ✅ Fully | `test_error_codes.cpp` — 4 tests |
| `diagnostic_types.h` | ✅ Fully | `test_diagnostic_types.cpp` — 12 tests |
| `aligned_buffer.h` | ❌ Not Tested | Buffer alignment utility |
| `orchestration_server_interface.h` | 🟡 Partial | Used as mock in migration_orchestrator test |

---

## 2. Most Critical Untested Files

Ranked by security impact, data integrity risk, and business logic importance:

### 🔴 CRITICAL — Security & Data Integrity

| Priority | File | Methods | Risk |
|----------|------|---------|------|
| **1** | `encryption.cpp` | 4 public + internal helpers | **AES-256 encryption/decryption, PBKDF2 key derivation.** A bug here silently corrupts or exposes user data. |
| **2** | `input_validator.cpp` | 19 methods | **Path traversal protection, injection prevention, UTF-8 validation.** Security boundary — untested validators are a vulnerability. |
| **3** | `secure_memory.cpp` | 3 functions | **Secure random generation, memory locking.** Cryptographic primitives need verified correctness. |
| **4** | `path_utils.cpp` | 20+ methods | **Path normalization, safe path checking, traversal detection.** Missteps cause data loss or security bypass. |
| **5** | `file_hash.cpp` | 8 methods | **MD5/SHA256 hash calculation, file verification.** Integrity checking — only tested indirectly via integration. |

### 🟠 HIGH — Core Business Logic

| Priority | File | Methods | Risk |
|----------|------|---------|------|
| **6** | `config_manager.cpp` | 20+ methods | Central settings manager (singleton). Bugs affect all features. |
| **7** | `user_data_manager.cpp` | 20+ methods | Backup/restore of user data, archive creation/extraction. Data loss risk. |
| **8** | `quick_action_controller.cpp` | 20 methods | Orchestrates all quick actions — scan, execute, cancel. |
| **9** | `deployment_manager.cpp` | 8 methods | Deployment queue management with readiness checks. |
| **10** | `smart_disk_analyzer.cpp` | 13 methods | SMART data parsing, health assessment, recommendations. Parse logic is very testable. |
| **11** | `smart_file_filter.cpp` | 11 methods | File filtering rules, regex compilation, exclusion logic. Pure logic, highly testable. |
| **12** | `file_scanner.cpp` | 7 methods | Directory traversal, file discovery. Core to backup/organize features. |
| **13** | `decompressor_factory.cpp` | Factory | Creates appropriate decompressor — classic testable factory pattern. |
| **14** | `deployment_summary_report.cpp` | Report gen | Report generation is easily testable. |
| **15** | `action_factory.cpp` | Factory | Creates all 40 quick actions — factory pattern, testable. |

### 🟡 MEDIUM — Workers & Utilities

| Priority | File | Methods | Risk |
|----------|------|---------|------|
| **16** | `logger.cpp` | 15 methods | Logging framework (rotation, levels, timestamps). |
| **17** | `package_matcher.cpp` | Matching | Package name matching logic. |
| **18** | `app_scanner.cpp` | Scanning | Application discovery. |
| **19** | `backup_worker.cpp` | Worker | Backup execution thread. |
| **20** | `duplicate_finder_worker.cpp` | Worker | Duplicate detection thread. |

---

## 3. Recommendations

### Immediate Priority (Security — must have tests)

1. **`encryption.cpp`** — Test encrypt/decrypt roundtrips with known test vectors, verify PBKDF2 produces expected keys, test edge cases (empty data, max size, wrong password).

2. **`input_validator.cpp`** — Test all path traversal sequences (`../`, `..\\`, encoded variants), null bytes, control chars, UTF-8 validation, boundary conditions for buffer/disk/memory validation.

3. **`path_utils.cpp`** — Test `isSafePath()`, `normalize()`, `makeAbsolute()`, `makeRelative()`, wildcard matching, safe filename generation. These are pure functions — trivially testable.

4. **`secure_memory.cpp`** — Test random generation produces output, verify memory lock/unlock calls succeed.

5. **`file_hash.cpp`** — Promote from integration-only to dedicated unit tests with known hash values for test files.

### High Priority (Core Logic)

6. **`config_manager.cpp`** — Test get/set/clear/reset operations, defaults initialization, thread safety. Singleton makes this slightly harder — consider constructor injection.

7. **`smart_disk_analyzer.cpp`** — The parsing methods (`parseSmartctlOutput`, `parseSataAttributes`, `parseNvmeHealth`, `assessHealth`, `generateRecommendations`) can be tested with captured JSON output. No hardware needed for parse logic.

8. **`smart_file_filter.cpp`** — Pure filtering logic with regex. Create test fixtures with various file types and verify include/exclude decisions.

9. **`user_data_manager.cpp`** — Test archive creation/extraction with temp directories, backup listing, checksum comparison. Use `QTemporaryDir` for isolation.

10. **`deployment_manager.cpp`** — Test queue operations, priority ordering, readiness checks. All in-memory, easy to test.

### Medium Priority (Factory & Infrastructure)

11. **`action_factory.cpp`** / **`decompressor_factory.cpp`** — Verify correct types are created for each input.

12. **`quick_action_controller.cpp`** — Test action registration, scan workflow, execution workflow, cancellation.

13. **`logger.cpp`** — Test log rotation, level filtering, file creation.

### Lower Priority (Largely Not Testable Without Hardware)

- **Actions (`src/actions/`)** — The 40 quick actions are mostly thin wrappers around Windows system commands (registry, services, etc.). Testing them requires mocking `ProcessRunner` or the Windows API. Consider adding an interface/mock for `ProcessRunner` to enable action testing.
- **Drive scanner, drive lock, drive unmounter, elevation manager, keep awake** — Require real hardware or privileged operations.
- **GUI widgets** — Standard "not unit testable" category unless converted to MVP/MVVM with testable view models.

### Architectural Recommendations

1. **Inject dependencies** into `ConfigManager`, `QuickActionController`, and action classes rather than using singletons and direct process calls. This enables unit testing.

2. **Extract parsing logic** from hardware-dependent classes (e.g., `SmartDiskAnalyzer::parseSmartctlOutput` can be tested independently of `runSmartctl`).

3. **Create a `ProcessRunner` interface** so actions can be tested with a mock that verifies correct commands are invoked without actually executing them.

4. **Add test helpers** — The project already uses `QTemporaryDir` in some tests. Standardize a test fixture base class for file-system tests.

---

## Existing Test Quality Notes

The **existing 23 test files are well-structured** using QtTest with proper signal spies, mock objects (e.g., `FakeOrchestrationServer`), and meaningful assertions. The parallel transfer manager tests are particularly thorough with both unit and stress tests.

**Gaps in existing tests:**
- `test_assignment_queue_store.cpp` — Only 1 test (save/load). Missing: empty state, corruption handling, concurrent access.
- `test_network_connection.cpp` — Only 1 test (loopback). Missing: timeout handling, error cases, reconnection.
- `test_transfer_protocol.cpp`, `test_transfer_security.cpp`, `test_transfer_types.cpp` — Each has only 1 test. Consider adding edge cases and error scenarios.

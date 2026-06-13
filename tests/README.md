# SAK Utility — Test Suite

## Overview

Comprehensive test suite for SAK Utility using the **Qt Test** framework with **140 registered CTest tests** across **164 C++ test source files** plus script-backed helper tests. Tests cover AI assistant clients, chat-title generation, workflow orchestration, tool execution, cancellation, run state, trace storage, core utilities, security, elevation (tier classification, IPC protocol, task dispatcher, mixed-tier operations, UX, hardening), diagnostics, partition management, ISO pipelines, deployment, email inspection, splash sizing, partition filesystem probe certification, and quick action validation.

## Structure

```
tests/
├── unit/                                  # Unit tests (158 C++ files, including actions/)
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
│   ├── test_bandwidth_tester.cpp
│   ├── test_bundled_tools_manager.cpp
│   ├── test_chocolatey_manager.cpp
│   ├── test_cleanup_worker.cpp
│   ├── test_config_manager.cpp
│   ├── test_connectivity_tester.cpp
│   ├── test_cpu_benchmark_worker.cpp
│   ├── test_decompressor_factory.cpp
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
│   ├── test_input_validator.cpp    ├── test_install_script_parser.cpp│   ├── test_keep_awake.cpp
│   ├── test_leftover_scanner.cpp
│   ├── test_linux_distro_catalog.cpp
│   ├── test_linux_iso_downloader.cpp
│   ├── test_logger.cpp
│   ├── test_mbox_parser.cpp
│   ├── test_memory_benchmark_worker.cpp
│   ├── test_migration_report.cpp
│   ├── test_network_adapter_inspector.cpp
│   ├── test_network_diagnostic_controller.cpp
│   ├── test_network_diagnostic_report.cpp
│   ├── test_network_diagnostic_types.cpp
│   ├── test_network_diagnostic_utils.cpp
│   ├── test_network_share_browser.cpp
    ├── test_nuget_api_client.cpp
    ├── test_organizer_worker.cpp
    ├── test_package_list_manager.cpp
│   ├── test_package_matcher.cpp
│   ├── test_path_utils.cpp
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
│   ├── test_restore_point_manager.cpp    ├── test_script_rewriter.cpp│   ├── test_secure_memory.cpp
│   ├── test_smart_disk_analyzer.cpp
│   ├── test_smart_file_filter.cpp
│   ├── test_streaming_decompressor.cpp
│   ├── test_stress_test_worker.cpp
│   ├── test_thermal_monitor.cpp
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
│   ├── test_offline_package_builder.cpp
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
| test_connectivity_tester | `connectivity_tester` | Network connectivity detection, ping, unreachable hosts |

### Security & Encryption
| Test | Module Under Test | Coverage |
|---|---|---|
| test_encryption | `encryption` | AES-256-CBC encrypt/decrypt, wrong password, Unicode, file encryption, IV randomness |
| test_secure_memory | `secure_memory` | SecureString/SecureBuffer zeroing, VirtualLock, random generation |

### Deployment & Migration
| Test | Module Under Test | Coverage |
|---|---|---|
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
| test_package_list_manager | `package_list_manager` | Curated package list management, list generation |
| test_install_script_parser | `install_script_parser` | Installation script parsing and validation |
| test_nuget_api_client | `nuget_api_client` | NuGet API queries, package metadata retrieval |
| test_script_rewriter | `script_rewriter` | Script modification and rewriting logic |

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
| test_file_management_file_system | `FileManagementFileSystemBridge` | Mounted/raw/image target capability mapping, non-native organizer blockers, ext/HFS+/APFS reader routing, explicit HFS+ mutation blockers/options, and APFS write gating to 64-128 MiB one-spaceman-chunk generated targets |
| test_file_explorer_types | `FileExplorer*` types and `FileExplorerCommandRegistry` | Files-like explorer target IDs, explicit-ID enforcement, item capability mapping, local/raw path normalization, pane history, selection summaries, command metadata, feature gates, layout-picker blocker gates, target-type command-state coverage, and raw write blockers |
| test_file_explorer_item_model | `FileExplorerItemModel`, `FileExplorerSortFilterModel` | Model/view row/column roles, entry paths and item metadata roles, attribute summary roles, folder-first source/proxy sorting, proxy filtering, size sorting, and clear/reset behavior |
| test_file_management_explorer_panel | `FileManagementExplorerPanel`, `FileExplorerPane`, `FileExplorerDetailsView` | Files-like shell creation, grouped sidebar target selection, command-bar disabled blockers, functional View/layout picker without milestone labels, details/list/grid/cards/adaptive switching, Columns current/child-preview surfaces, shared selection survival, per-target/path view persistence, hidden-items and file-extension toggles, current-folder filter, command palette entry points, omnibar path load, status strip, persistent detail panes, loading/empty/error pane states, responsive collapse, extended selection, details column persistence, directory activation route, table/sidebar context menu actions, baseline screenshot capture, and shortcut smoke coverage |

### Partition Manager
| Test | Module Under Test | Coverage |
|---|---|---|
| test_partition_manager_core | `partition_manager_*` | Inventory parsing, resilient volume lookup script coverage, hidden EFI/MSR file-system fallback labels, file-system support registry coverage, read-only raw signature detection for ext2/ext3/ext4 with superblock metadata, XFS/Btrfs read-only superblock metadata and sanity warning coverage, APFS read-only container/checkpoint ring/object-map/volume-OID metadata, visible object-map tree anchors, visible referenced root/non-root B-tree node headers, root `btree_info_t` metadata, bounded volume-superblock candidate metadata, referenced-object-header preflight, APFS metadata object-checksum rejection in the read-only browser, APFS browse/extract/export capability registry coverage, fail-closed APFS writer certification preflight coverage including oversized generated-layout rejection and malformed spaceman chunk/CIB sanity warnings, APFS object checksum calculation/stamping/verification, operation-specific image-only file/format/repair/resize/volume-label mutation plan shapes, safe target-path normalization/traversal blockers, raw APFS format/write/patch/delete/directory/volume-label/repair blocker coverage for missing raw paths, missing confirmation, and image-only options, APFS generated/minimal layout guard rejection for malformed generated media and unsupported APFS layouts, APFS generated root-file write/patch/delete, empty root-directory create/delete, root-directory child-file write/patch/delete, and volume-label Apply script generation and safety gates, APFS image-only format scratch-image generation with APFS detection/free-space/root-listing proof and 9000-byte seed-file read-back proof, APFS generated-image volume-label change proof with old/new-label read-back, APFS generated-image root-file add/replace/byte-range patch/delete proof with bounded existing root-file preservation, multi-block read-back, negative read-back, and empty-root-after-delete coverage, APFS generated-image empty root-directory create/delete proof with empty-listing and absence validation, APFS generated-image root-directory child-file create/read/patch/delete proof with non-empty directory delete blocking and empty-directory-after-delete validation, APFS generated-image checksum-repair proof with corrupt-before/repaired-after browse and read-back, and sanity warning coverage, XFS/Btrfs/APFS metadata consistency capability registry coverage, ext2/ext3/ext4 read-only directory/file browsing, selected-file extraction, symlink target display/sidecar export, and bounded recursive directory export for direct, indirect, and extent-mapped data, HFS+ catalog consistency checks, HFS+ attributes B-tree key scans plus structured fork/inline metadata reporting, HFS+ catalog browsing, selected data/resource-fork extraction, selected inline/fork-backed attribute-value extraction, bounded recursive directory export with `.rsrc` sidecars, catalog/data/resource/attribute extents-overflow resolution, image-only same-size HFS+ data-fork overwrite with fail-closed gates, initial-extent and extents-overflow write coverage, data/resource-fork allocated-block grow/shrink replacement with catalog logical-size updates, stale-tail zeroing, inline-attribute replacement with record-capacity checks, fork-backed attribute replacement within existing allocated blocks, bounded initial-extent data/resource/fork-backed-attribute allocation growth with bitmap/counter read-back, broad allocation-growth blocking, unsupported inline/broad attribute-growth blocking, and read-back hashes, Linux swap header metadata and original SWAPSPACE2 format script generation, direct and HFS-wrapper HFS+, HFSX, and unknown data, filesystem tool-manifest approval/blocker coverage, manifest-gated read-only checker command generation and approval resolution, ext e2fsprogs format/repair/resize command generation with destructive confirmation blockers, HFS+/HFSX `newfs_hfs` format and `fsck_hfs` check/repair command generation, HFS staged selected-raw-partition data-fork/resource-fork/inline/fork-backed-attribute mutation script generation and safety blockers, APFS generated create/format/repair script generation through `sak_apfs_writer_cli.exe`, APFS GPT type validation, manifest-gated ext/HFS format/repair/grow/shrink scripts, non-native write safety gates including exact selected raw partition target matching, ext shrink usage-metadata blocker, native ext format rejection, safety blockers, Create Image read-only risk and unsafe destination blockers, resize/merge/MBR2GPT script generation, resize adjacent-free-space/no-op/below-used-space/start-move/donor-space blockers, recovery write-back acknowledgement/bounds blockers and scripts, offline-image and raw-source Data Recovery scan/restore with source-hash verification, clone verification scripts, offset partition-clone scripts, create selected-region offset scripts, create/format/change cluster-size scripts, create partition type scripts, partition-clone region overwrite confirmation and unsafe physical-target blockers, PowerShell escaping, redo-stack state, queue layout-hash guard |
| test_partition_manager_panel | `PartitionManagerPanel` | AOMEI-style panel chrome, stateful Scan Disks/Refresh Disks inventory control, status-bar inventory summary, selection-gated sidebar actions, enabled HFS+ read-only catalog check action, enabled Inspect Non-Windows File System metadata report for detected non-native volumes, disabled unsupported HFS+/APFS/XFS/Btrfs Windows-native check/resize/cluster/label paths with active Non-Windows alternatives, generated-layout-gated APFS File write dialog and queue wiring including root-file, root-directory, and root-directory child-file modes, HFS File staged mutation dialog and queue wiring including bounded allocation-growth mode, bounded create-file payload mode, no-payload empty-file/folder create/delete, single-leaf catalog rename/move destination-path mode, allocated-file delete, secure delete toggle, bounded folder-tree delete, and fork-backed attribute mode toggling with non-HFS disabled gating, XFS sanity metadata surfacing in File System tooltip, Properties, Inspect, and Check Non-Windows File System original metadata consistency report, Browse Non-Windows File System ext, HFS+, and APFS enablement including ext symlink target display and HFS+ selected attribute-value extraction, confirmed ext format/repair/grow/shrink, Linux swap format, APFS generated format, and generated APFS repair queue wiring with fixed selected raw partition targets for queued repair, fixed-width sidebar, compact icon text-link actions, no redundant sidebar preview box, fixed-size Icons8 SVG ribbon icon resources with disabled SVG mode mappings including the dedicated Dry Run icon, compact unframed 1 px disk-map gutter, full commercial legend roles, rendered legend swatches and inner bars matched to meaningful colors for GPT/Primary, Logical, Simple, Spanned, Striped, Mirrored, RAID5, and dark-gray Unallocated segments, rendered neutral outer disk-map shells, selected partition highlights, whole-disk row highlights, mouse/right-click disk-map target selection synchronized with table selection, redo enabled only after Undo creates a redoable action, equal-height disk tiles and partition segments, rounded disk row/tile containers, final Apply before/after diff, table and disk-map context menus without ribbon/Pending Operations commands, Properties including raw file-system metadata, Explore, Data Recovery image/raw-source entry point, Quick Partition preset-capable entry point, Extend Partition Wizard, Allocate Free Space adjacent-donor backup/delete/extend/recreate/restore queue path, unallocated Allocate Free Space To resize/move queue paths, Change Cluster Size queued backup/reformat/restore dialog, primary/logical conversion queue dialog, volume serial-number queue dialog, dynamic-to-basic queue dialog, Space Analyzer tree/largest-file/file-type views with Open/Explore/Copy Path actions, Disk Benchmark, Manage BitLocker status dialog, SSD Secure Erase readiness and typed queue-confirmation dialog, Make Bootable Media actions, create-dialog handle synchronization and direct preview-bar drag, disk separators, resizable disk map, and borderless no-row-number partition table |
| test_sak_apfs_writer_cli | `sak_apfs_writer_cli.exe` | Production APFS helper self-test covering generated APFS image format, generated-image volume-label change with old/new-label read-back, generated-image root-file create/replace/byte-range patch/delete with payload/read-back or deleted-file hashes, generated-image empty root-directory create/delete with empty-listing and absence validation, generated-image root-directory child-file write/patch/delete with child read-back and empty-directory-after-delete validation, missing-delete and non-empty-directory-delete blocking, intentional metadata checksum corruption, generated-layout checksum repair, JSON output, and raw write/patch/delete/directory/child-file/volume-label command refusal for normal file paths. |
| test_sak_hfs_writer_cli | `sak_hfs_writer_cli.exe` | Production HFS helper self-test covering JSON output, structured fail-closed rejection for non-HFS media and missing confirmation, successful same-size helper overwrite, data-fork allocation growth plus allocated-block grow/shrink/truncate, resource-fork allocation growth plus replace/truncate, inline-attribute replacement, fork-backed attribute replacement and bounded allocation growth, constrained empty-file create/read/list/delete, non-empty file create/read/list/delete, single-leaf catalog rename with list/read-back proof, empty-folder create/list/delete, secure released-block wipe on delete, distinct before/after hashes, and certifier read-back hash proof. |
| partition_filesystem_probe_certifier | `PartitionFileSystemDetector`, `PartitionHfsFileSystemReader`, `PartitionApfsFileSystemReader`, `openFileOrRawDeviceReadOnly` | Certification helper for Linux/VM/physical metadata proof. It reads a raw image or Windows raw partition alias, emits JSON with detected file system, source, total/free bytes, details, and blockers, can require expected file-system plus no metadata sanity warnings, can build a tiny HFS+ writer fixture for helper certification, can add HFS+ root/list/read/check/attribute proof plus APFS root/list/read/export proof for physical Apple media, can build a generated image-only APFS format scratch image with multi-block seed-file read-back proof, can add, replace, or delete one bounded root regular file in a generated APFS scratch image while preserving existing bounded root regular files, can add or delete one bounded regular child file inside a supported generated APFS root directory, and can repair recognized APFS metadata object checksums in a separate generated scratch image. The self-test covers offset reads, invalid offset rejection, invalid HFS read cap and attribute file-ID rejection, invalid APFS export cap rejection, HFS/APFS-operation blocker exit codes on wrong input, generated APFS detection/root-listing/seed-file read-back, generated APFS empty-root write/read-back, non-empty generated-root add, existing-file replace, generated-root child-file write/read/delete, generated-root delete, preserved-file read-back, corrupt-checksum browse failure, checksum repair read-back, clean-image repair blocking, and overwrite blocking. |

`test_partition_manager_core` also covers in-place APFS existing-image format
with destructive wipe confirmation, stale edge zeroing, post-format APFS
detection, requested volume-name listing, empty-root browse proof, production
helper script generation, and fake raw target fail-closed blockers before any
raw open. `test_sak_apfs_writer_cli` covers the production helper for generated
format/repair, root-file mutation, empty root-directory mutation,
root-directory child-file mutation, plus raw-command
refusal for normal file paths. The certifier
self-test covers the same existing-target format lane for file-backed images.

Destructive Partition Manager certification is intentionally outside CTest. Release readiness runs `scripts/check_partition_manager_certification_matrix_integrity.ps1`, `scripts/check_partition_manager_commercial_gate_matrix.ps1`, `scripts/check_partition_manager_external_checklist.ps1`, `scripts/check_partition_manager_external_lab_package.ps1`, `scripts/check_partition_manager_vhd_preflight_report.ps1`, `scripts/check_partition_manager_certification_gap_report.ps1`, `scripts/check_partition_manager_certification_bundle.ps1`, and `scripts/check_partition_manager_feature_matrix.ps1` first so matrix IDs, evidence/value contracts, certification docs, strict VHD and strict hardware orchestrator coverage, generated lab-checklist and per-gate lab-package coverage, current VHD preflight blockers, current incomplete-gate reporting, status-aware next commands, hashed artifact-bundle integrity, visible blocker dialogs, and the commercial-parity feature list stay mapped to implementation, UI, tests, docs, `docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json`, release-claim wording, and certification evidence. It then runs the certification harness in plan-only mode, creates an external-evidence JSON scaffold, Markdown lab checklist, and per-gate lab package with required evidence payload placeholders plus `report.template.json` evidence-report scaffolds, verifies report/manifest/checklist/lab-package shape plus matrix-backed required evidence keys and safety contracts, writes a read-only VHD preflight report, writes a certification-status summary that counts only complete non-empty VHD and external payload values, writes and verifies JSON/Markdown certification-gap reports, writes and verifies a hashed certification-artifact bundle, checks release-facing docs with `scripts/check_partition_manager_release_claims.ps1`, and runs `scripts/test_partition_manager_certification_tools.ps1` without mutating disks. The self-test covers claim levels, VHD preflight, completed per-gate `report.json` import into the external manifest, malformed evidence, generated-checklist/lab-package/report-template acceptance, strict hardware handoff acceptance/rejection, stale-checklist/lab-package gate/evidence/value rejection, status-aware gap-report generation, and artifact-bundle verification. Use `scripts/run_partition_manager_vhd_certification_strict.ps1 -RelaunchElevated` for the one-command disposable-VHD evidence handoff, including raw-region partition clone and image restore gates, then scaffold external VM/hardware/lab evidence with `scripts/new_partition_manager_external_evidence_manifest.ps1 -CreateEvidenceDirectories`, fill each gate `report.json`, import with `scripts/update_partition_manager_external_manifest_from_reports.ps1 -RequireAllReports`, verify the imported manifest with `scripts/run_partition_manager_hardware_certification_strict.ps1`, run `scripts/get_partition_manager_certification_status.ps1`, and complete the external gates in `docs/PARTITION_MANAGER_CERTIFICATION.md`.

Optional destructive filesystem lab proof is also outside CTest. Use
`scripts/launch_partition_manager_physical_cross_filesystem_destructive_validation_local.ps1`
only against pinned expendable external media; it UAC-elevates, validates the
target identity, proves ext2/ext3/ext4 format/repair/grow/shrink plus Linux swap
raw detection, writes JSON evidence, and clears the disk back to RAW.
Non-mutating HFS tool proof is part of release readiness through
`scripts/run_partition_manager_hfsprogs_image_validation.ps1`; it creates
disposable HFS+/HFSX images, formats/checks/repairs them with bundled
`newfs_hfs` and `fsck_hfs` including HFSX repair retry handling, and verifies S.A.K. HFS detection/consistency with
`partition_filesystem_probe_certifier.exe`.
Physical HFS tool proof is destructive and remains outside CTest; it uses sparse
staging for raw partition writes and should be run with
`scripts/launch_partition_manager_physical_hfsprogs_validation_local.ps1` only
against a pinned expendable external partition.
Physical HFS File Apply proof is destructive and remains outside CTest; it uses
HFS logical-size sparse staging for selected raw-partition data-fork,
resource-fork, catalog rename, inline-attribute, fork-backed-attribute replacement, and bounded
fork-backed-attribute growth mutation and should be run with
`scripts/launch_partition_manager_physical_hfs_file_apply_validation_local.ps1`
only against a pinned expendable external HFS partition. Current evidence lives
at
`artifacts/partition-manager-certification/vm-lab/external-evidence/external.hfs-file-apply-physical/report.json`.
Physical APFS production-helper proof is destructive and remains outside CTest.
The previous pinned JMicron 51 GB generated-layout evidence at
`artifacts/file-management-live-certification/disk3-apfs-raw-format/report.json`
is now Windows-side-only: macOS recovery kernel validation rejected that large
generated spaceman geometry. Current APFS raw-format Windows proof passed on
the 128 MiB JMicron slice at
`artifacts/file-management-live-certification/disk2-apfs-128mb-raw-format/report.json`;
Apple-native validation of that small slice remains pending until the recovery
VM validates it.
File Management live proof is destructive and remains outside CTest; current
evidence covers APFS File Explorer create/write/read/duplicate-search/
advanced-search/delete/delete-folder on JMicron Disk 2 Partition 2 at
`artifacts/file-management-live-certification/disk2-apfs-script-after-fix/file-management-live-certification.json`
and HFS+ File Explorer create/write/read/duplicate-search/advanced-search/
rename/delete/delete-folder on Best Buy Disk 3 Partition 3 at
`artifacts/file-management-live-certification/disk3-hfs-script-after-fix/file-management-live-certification.json`.
Larger APFS targets remain read-only for File Management until multi-CIB
spaceman support is implemented and Apple-validated.

### Email Inspector
| Test | Module Under Test | Coverage |
|---|---|---|
| test_email_types | `email_types` | Email data structures, item types, MAPI properties |
| test_pst_parser | `pst_parser` | PST/OST header parsing, legacy Unicode page sizing, compressible PST decode, folder tree, item detail, attachments |
| test_mbox_parser | `mbox_parser` | MBOX parsing, message extraction, header parsing |
| test_email_search_worker | `email_search_worker` | Full-text search, field filtering |
| test_email_export_worker | `email_export_worker` | EML/CSV/VCF/ICS export, attachment extraction |
| test_email_profile_manager | `email_profile_manager` | Email client profile discovery, backup/restore |
| test_email_report_generator | `email_report_generator` | Email analysis report generation |

### Quick Actions
| Test | Module Under Test | Coverage |
|---|---|---|
| test_action_factory | `quick_action` | Action creation completeness, metadata validity (name, description, category), initial state, no duplicates, all categories populated |
| test_all_actions_metadata | `quick_action` | Validates every registered action has complete metadata |
| test_quick_action | `quick_action` | Base action interface, state transitions |
| test_quick_action_controller | `quick_action_controller` | Action execution, progress, cancellation |
| test_quick_action_result_io | `quick_action_result_io` | Result serialization, history persistence |

### Integration Tests
| Test | Workflow | Coverage |
|---|---|---|
| test_ost_integration | Email Inspector | Real OST/PST file opening, folder tree, item loading, item detail |
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
**Registered Tests:** 140 (CTest)
**Test Files:** 164 C++ test source files plus script-backed helper tests
**Platform:** Windows 10/11

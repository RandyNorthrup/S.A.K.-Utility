# SAK Utility -- Test Suite

## Overview

The suite uses the **Qt Test** framework for native tests, plus PowerShell, Python and node script
tests that CTest registers and runs the same way. Tests cover AI assistant clients, chat-title
generation, workflow orchestration, tool execution, cancellation, run state, trace storage, core
utilities, security, elevation (tier classification, IPC protocol, task dispatcher, mixed-tier
operations, UX, hardening), diagnostics, partition management, ISO pipelines, deployment, email
inspection, splash sizing, partition filesystem probe certification, and quick action validation.

This README describes tests; it does not count them. The CTest registration is the only count
(`ctest -N`), and the `test-registration` hook refuses a test source that nothing registers.

## Structure

```
tests/
+-- unit/            # Unit tests, one test_<component>.cpp per component
|   +-- actions/     # Quick action validation tests
+-- integration/     # End-to-end workflow tests
+-- fuzz/            # Shared fuzz harness used by the test_fuzz_* suites
+-- certification/   # Certifier helpers driven by script tests and VM/hardware runs
+-- fixtures/        # Static fixtures and compressed foreign Apple images
+-- support/         # Shared fixture builders (byte writers, HFS/ext/MCP fixtures)
+-- stubs/           # Link stubs for narrowly scoped test targets
+-- tsan/            # Separate ThreadSanitizer build for portable threaded units (WSL clang)
+-- CMakeLists.txt   # Test build configuration and CTest registration
+-- README.md        # This file
```

## Running Tests

### Tests and this PC's drives

A test may not reach the real drives of the PC it runs on, and on a person's PC it cannot.
`support/test_hardware_guard.cpp` is linked into every native `test_*` binary. Unless the environment
sets `SAK_TEST_ALLOW_REAL_HARDWARE=1`, which only the CI workflows do, it refuses these and prints one
`SAK TEST HARDWARE GUARD` line for each:

- opening a physical drive, or a volume other than the system drive's, from product code or Qt, and
  opening the system drive's volume for write. The line ends with the access mask and share mode the
  open asked for, so a run's logs can be counted for write requests;
- starting a process whose command line would query a drive, scan or repair Windows, change the
  network or the firewall, or run the application;
- asking for elevation, which is how the application reaches the helper that reads raw disks.

The refused call fails with access denied, as it can on any PC, so a panel that lists drives as it
starts carries on with none. Give a test that needs an answer a fake (`ScopedProcessFaultInjector` for
a process), or start it with `SAK_SKIP_UNLESS_REAL_HARDWARE_ALLOWED()` if only the real thing proves
it. The PowerShell tests that run the application exit 77 without the variable, which CTest reports as
skipped. The APFS and HFS writer CLI tests and the live certifier test leave out, without it, the checks
that aim those programs at this PC's own disk to prove they refuse: the guard is not linked into them. `test_hardware_guard` drills each route with paths and commands that are harmless if the
guard is broken. A script too long for a command line reads itself from standard input, where the guard cannot read it, so the guard refuses that form outright.

### Labels and tiers

Every registered test carries exactly one tier label (set in the label block at the end of
`tests/CMakeLists.txt`; configure fails for an unlabelled script test). Gate tiers are defined in
`AGENTS.md`, "The gate".

| Label | Tier | What it holds |
|---|---|---|
| `product` | T2 | Native QtTest suites (the default) and the product scripts: browser extension, pack/register, the Create Image lock rows, the APFS/HFS writer CLIs, the release-binary scan for partition/filesystem test hooks and the retired SAK_DEV_SOURCE_TREE override |
| `release-tooling` | T2 | Startup budget, runtime-DLL audit and signature publisher regressions |
| `tooling` | T2 | Core process-capture contract, lizard JS baseline, cppcheck Qt-slot classifier, run-folder retention fences |
| `cert` | T2 | Apple filesystem capability matrix gate (JSON only); Partition Manager row evidence guards, claim level and site wording, code digest, and whether the matrix is what its generator writes |
| `tooling-extended` | T4 | Process-capture ownership/deadline/output/stdin/environment scopes, false-green harness, portable staging and archive recovery |
| `mutation` | T4 | Mutation runner regressions |
| `coverage` | T4 | Coverage runner regressions |
| `tidy` | T4 | clang-tidy naming gate regressions |
| `cert-tooling` | T4 | Certification-tool self-tests: verifiers, controller evidence/routing, live and probe certifiers, source fingerprint, automation-VM security |
| `fuzz-extended` | T4 | Fuzz suites over the 60-second T2 fuzz budget (`test_fuzz_backup_codec`) |

`fuzz` is an extra label on every `test_fuzz_*` suite, alongside its tier label.

Parallel runs are safe by construction, not by luck. Tests that share a resource carry a
`RESOURCE_LOCK`: `portable_data_root` (every test executable lives in the one Release directory,
and `app_paths::dataRoot()` is that directory plus `data`), `system_clipboard`,
`desktop_session`, and `image_scratch` (`SAK_TEST_IMAGE_SCRATCH_ROOT`). Wall-clock and deadline
tests are `RUN_SERIAL`, and so are the two tests that saturate the machine on their own (the
source-fingerprint build fixtures and portable archive recovery). Native GUI suites run on the offscreen platform unless they exist to
exercise the real desktop or clipboard. If a test fails only under `-j`, fix its isolation (a
lock, a serial run, its own data root), never its assertion.

```powershell
# T2 tests, as CI runs them
ctest --test-dir build -C Release -j 8 --output-on-failure -L "^(product|release-tooling|tooling|cert)$"

# T4 tests
ctest --test-dir build -C Release -j 8 --output-on-failure -L "^(tooling-extended|mutation|coverage|tidy|cert-tooling|fuzz-extended)$"

# Everything, or one label, or the tests a change can reach
ctest --test-dir build -C Release -j 8 --output-on-failure
ctest --test-dir build -C Release -j 8 -L fuzz
ctest --test-dir build -C Release -R "test_encryption"

# What a label selects, without running it
ctest --test-dir build -C Release -N -L product
```

On a PC someone is working at, run `scripts/run_ctest_no_dialogs.ps1` with the same arguments in place of
`ctest`, so no test can put a dialog or a window on their screen.

Run the suite from a standard (non-elevated) shell. The app runs as a standard user and elevates
per task, and several suites assert exactly that (an ordinary PowerShell child is not elevated, a
new file belongs to the user rather than `BUILTIN\Administrators`, the AI policy sees an
unelevated host). CI runners are elevated, so CI runs CTest through
`scripts/invoke_unelevated.ps1`, which gives the command a standard-user token; use it the same
way from an elevated shell.

Image tests need a disk with room for sparse multi-gigabyte images: point `TEMP`, `TMP` and
`SAK_TEST_IMAGE_SCRATCH_ROOT` at it for the one command rather than persistently.

The certifier and writer-CLI script tests (APFS/HFS writer CLIs, filesystem probe certifier,
certification tools, live certifier) delete their run folder under `build/.../test_results` when
they pass and keep a failed run for diagnosis; each run first caps leftover failed runs at the
newest three (`scripts/run_folder_retention.ps1`). Those folders once held over 1 TB.

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

Live-test policy: paid OpenAI, UUP dump, Linux ISO network, and Chocolatey bundle tests keep
explicit environment-variable opt-outs. Once requested, missing credentials, timeouts, API errors,
empty results, and service failures fail; dependency failure never becomes a passing skip.

The Win32 MCP session and pool suites are ordinary bounded process-transport tests. They launch
the built `sak_utility.exe` in its real headless MCP mode, give each child a temporary
`LOCALAPPDATA`, and issue only `health_check` plus catalog discovery. They verify persistent
process reuse and read-only/full-policy isolation without driving the desktop, clipboard, browser,
registry or network. Full application and live browser/desktop behavior remain separate E2E scope.
Pool cleanup regressions use owned fake servers, held process-generation handles and per-instance
native faults to check combined error reporting, retained ownership, blocked replacement, partial
cleanup and explicit recovery. A fake tool that never answers proves a cancelled tools/call kills
its server through the Job and returns well inside the request timeout, at both session and pool
level, and that the pool then reopens the slot. The AI panel dispatch suite covers the panel side:
Stop flips the cancel check those calls poll, and releasing a panel whose tool will not stop
keeps it alive, hidden and unparented, until the tool returns. The shared Windows Job suite covers startup attributes, real
descendant containment and cleanup-failure ownership under a 60-second CTest deadline. Explicit
fake-child modes use an independent 10-second lifetime guard, including while blocked on stdin.
The shared `process_capture.ps1` self-test runs core, ownership, deadline,
output, stdin and environment scopes as separate 60-second CTests under both
PowerShell 7 and Windows PowerShell 5.1. The previous aggregate reached its
60-second limit at different subcases on two runs; splitting preserves every
assertion while making a slow group identifiable without loosening a limit.
Controlled three-second Python receipt delays separately exercise the parent
and termination-utility setup handshakes; neither changes production deadlines.

## Test Categories

### Core Utilities
| Test | Module Under Test | Coverage |
|---|---|---|
| test_partition_os_migration | `partition_os_migration`, `src/core/partition_scripts` | What Migrate OS plans for each target, what refuses it, and the order of the script's steps, with no disk involved. Its fixture parses the scripts, compiles their C#, and runs the copy code and pure functions on ordinary files. The scripts' real work runs only in the VM. |
| test_elevated_helper_connection | `ElevationBroker`, `sak_elevated_helper` | The real helper, started the real way, runs a real task over the real pipe. It runs only in an elevated process: ctest skips it, and an elevated CI step of its own runs it and fails if it skipped. Nothing else connects the two ends, which is how a path comparison stopped every administrator task for three months. |
| test_partition_image_volume_lock (five CTests) | `PartitionScriptBuilder`, Create Image source locks | Each disk/selected-partition, pinned/unpinned GUID, and offline capture shape executes the full generated PowerShell lock fixture and its guard drills in a separately bounded CTest. No row is dropped from the former aggregate core test. |
| test_path_utils | `path_utils` | Path validation, normalization, sanitization, env-var expansion, safe-delete checks |
| test_logger | `logger` | Log levels, formatting, file rotation, thread safety; old session logs pruned at initialize (newest 30, 200 MB) |
| test_config_manager | `config_manager` | Get/set values, persistence, portable mode, type conversions; retired settings dropped on load (only when present); UI state in data/config/ui_state.ini (no registry QSettings in src/), no data-root fallback, per-session child temp |
| test_file_scanner | `file_scanner` | Recursive scan, extension/size/date filters, cancellation, controlled junction-cycle fixtures, and injected canonical-resolution failure |
| test_file_hash | `file_hash` | MD5/SHA-256 hashing, large-file streaming, hash comparison |
| test_hardware_guard | `support/test_hardware_guard` | Every route to a real drive is refused, ordinary work is not, and it stands down only when told |
| test_accessibility_audit | `accessibility_audit` | What the audit holds a widget to; the main window and every dialog it can build pass |
| test_process_runner | `process_runner` | Process execution, stdout/stderr capture, timeout, exit codes, held-generation Job cleanup on cancellation, timeout, normal-exit survivors and callback exceptions, unprovable cleanup reported as a failed run with the live process and armed Job in custody, detached-tool fault injection, and five GUI detached-launch failure guards; ChildProcessPolicy::WaitForJobEmpty lets a relaunched child finish, stops and reports a family that outlives the wait, honours cancel, and the uninstaller/Chocolatey callers request it |
| test_windows_process_job | `windows_process_job` | Native preparation failures (including a refused close after one), startup modifier preservation, real child/descendant containment, retained handles after cleanup failures, and isolated destruction of an unproved Job that reports and closes it, kill-on-close ending a live member, without crashing |
| test_resource_soak | `process_runner` | Long-session handle/GDI/USER/memory-growth soak over the real launchers |
| test_error_codes | `error_codes` | Error code enum values, category strings, display messages |
| test_input_validator | `input_validator` | Path, IP, port, Chocolatey name, and URL validation |
| test_worker_base | `worker_base`, `thread_custody` | Thread lifecycle, bounded readiness before cancellation, derived-member-safe destructor joins, progress signals; a worker or plain QThread that overruns its stop deadline is retained in custody (signals severed, never terminated) and destroyed once it runs to completion; a running worker destroyed directly stops the process (child probe) instead of being terminated |
| test_running_work_exit_guard | `running_operations`, `RunningWorkExitGuard`, `MainWindow` | Closing or relaunching as administrator with registered work running asks first; declining leaves it running; going ahead cancels it, shows progress and proceeds only once it has stopped; a registration ends with its owner; the main window's close consults the guard |
| test_format_utils | `format_utils` | Number/size/duration formatting helpers |
| test_bundled_tools_manager | `bundled_tools_manager` | Tool detection, path resolution, version checks |
| test_keep_awake | `keep_awake` | System sleep prevention, cleanup on destruction |
| test_connectivity_tester | `connectivity_tester` | Network connectivity detection, ping, unreachable hosts |

### Security & Encryption
| Test | Module Under Test | Coverage |
|---|---|---|
| test_encryption | `encryption` | AES-256-CBC encrypt/decrypt, wrong password, Unicode, file encryption, IV randomness |
| test_secure_memory | `secure_memory` | Memory wiping |

### Elevation & Privilege
| Test | Module Under Test | Coverage |
|---|---|---|
| test_elevation_ux | `elevation_banner`, `shield_icon` | Injected elevated/standard banner decisions, deterministic layout/content, and required Windows shield rendering |
| test_elevation_hardening | `elevation_gate` | Pure elevated/standard gate preflight, IPC, and dispatcher hardening |
| test_mixed_tier_operations | `permission_manager`, `user_profile_backup_worker` | Pure ownership-elevation refusal, ACL operations, path accessibility, and elevated task dispatch |

### Deployment & Migration
| Test | Module Under Test | Coverage |
|---|---|---|
| test_migration_report | `migration_report` | The install queue's entries, with bounds-checked access |
| test_email_inspector_panel | `email_inspector_panel` | Search scope, fields searched, and hits listed in the item table |
| test_main_window_help | `main_window` | Where the Help tab's links go and what the About tab claims |
| test_package_matcher | `package_matcher` | App-to-Chocolatey matching on real display names |
| test_user_profile_app_restore_flow | backup and restore wizard pages | Scan to restore: real install offers, one install per package, elevation gate |
| test_chocolatey_manager | `chocolatey_manager` | Package install, update, version queries |
| test_app_scanner | `app_scanner` | Installed app enumeration from registry |
| test_app_installation_worker | `app_installation_worker` | Package installation workflow, retry, error handling |
| test_permission_manager | `permission_manager` | NTFS ACL read/write, controlled junction/hard-link refusal, and pure primary-group apply planning |
| test_smart_file_filter | `smart_file_filter` | Category-based filtering, exclusion patterns, size thresholds |
| test_elevation_manager | `elevation_manager` | Admin privilege detection, UAC status; a declined UAC prompt needs no failure notice |
| test_user_profile_types | `user_profile_types` | Profile data structures, serialization, and the fields deliberately kept OUT of it (a WiFi profile's plaintext key must never reach a manifest) |
| test_user_profile_backup_worker | `user_profile_backup_worker` | Backup workflow, compression, encryption |
| test_user_profile_restore_worker | `user_profile_restore_worker` | Restore workflow, permission handling, merge |
| test_windows_user_scanner | `windows_user_scanner` | Windows user profile enumeration and deterministic standard-folder fixture sizing |
| test_cleanup_worker | `cleanup_worker` | Temp file cleanup, cache clearing |
| test_restore_point_manager | `restore_point_manager` | System restore point creation; the once-per-24h refusal reuses a restore point from the last 24 h (named), and fails closed without one |
| test_package_list_manager | `package_list_manager` | Curated package list management, list generation |
| test_install_script_parser | `install_script_parser` | Installation script parsing and validation |
| test_nuget_version_range | `nuget_version_range` | NuGet SemVer parse/compare + version-range grammar |
| test_nuget_dependency_resolver | `nuget_dependency_resolver` | Transitive dependency closure resolution (in-memory feed e2e) |
| test_script_rewriter | `script_rewriter` | Script modification and rewriting logic |

### Diagnostics & Benchmarking
| Test | Module Under Test | Coverage |
|---|---|---|
| test_diagnostic_types | `diagnostic_types` | Hardware info structs, SMART data, benchmark results |
| test_diagnostic_controller | `diagnostic_controller` | Full-suite orchestration, step sequencing, cancellation |
| test_diagnostic_report_generator | `diagnostic_report_generator` | HTML/JSON/CSV generation, data formatting |
| test_smart_disk_analyzer | `smart_disk_analyzer` | smartctl output parsing, health classification, attribute extraction |
| test_thermal_monitor | `thermal_monitor` | Temperature polling (own 5 s interval constant), threshold alerts, auto-abort, a missing sensor warned about once |
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
| test_network_adapter_admin | `network_adapter_admin` | New-name validation, exact-match resolution against the system's adapter list, the IPv4/DNS/DHCP netsh argument vectors, and the strict dotted-IPv4 check |
| test_csv_escape | `csv_escape` | The single CSV cell writer: RFC 4180 quoting plus the CWE-1236 formula guard, across delimiters |
| test_windows_path_policy | `windows_path_policy` | The shared untrusted-path screening dialect: safe single-child names (no `..`, `.`, separators or `:`), literal path segments, drive-qualified roots |
| test_ethernet_config_manager | `ethernet_config_manager` | Ethernet backup/restore, JSON persistence, and injected friendly/system-name MAC lookup |
| test_dns_diagnostic_tool | `dns_diagnostic_tool` | DNS queries, record type parsing |
| test_port_scanner | `port_scanner` | TCP port scanning, banner grabbing |
| test_bandwidth_tester | `bandwidth_tester` | iPerf3/HTTP speed testing; stale iPerf3 firewall-rule sweep (owner PID) and the server running inside its own Job |
| test_active_connections_monitor | `active_connections_monitor` | TCP/UDP connection listing |
| test_firewall_rule_auditor | `firewall_rule_auditor` | Firewall rule enumeration, conflict detection |
| test_network_share_browser | `network_share_browser` | SMB share discovery |
| test_wifi_analyzer | `wifi_analyzer` | WiFi network scanning, signal analysis |

### ISO & Image Handling
| Test | Module Under Test | Coverage |
|---|---|---|
| test_windows_iso_downloader | `windows_iso_downloader` | Opt-in UUP API, download URL validation, HTTP-allow for Microsoft CDN; post-opt-in failures are hard failures |
| test_linux_iso_downloader | `linux_iso_downloader` | Distro catalog, GitHub URL resolution, aria2c integration, and opt-in live GitHub/Ubuntu reachability that hard-fails after opt-in |
| test_linux_distro_catalog | `linux_distro_catalog` | Required distro metadata, URL patterns, version detection |
| test_uup_dump_api | `uup_dump_api` | UUP Dump API parsing, edition/language selection |
| test_uup_iso_builder | `uup_iso_builder` | UUP-to-ISO conversion, converter process management |
| test_drive_scanner | `drive_scanner` | Physical drive enumeration, USB detection, formatting info, disk/volume device-notification rescans with a debounce and no periodic timer |
| test_image_flasher_panel | `ImageFlasherPanel` | Real panel Flash-button gating, rejected-image state, smoke and audit modes running the same start-up drive scan users get, and linked production SVG resources; no raw-disk write |
| test_diagnostic_benchmark_panel | `DiagnosticBenchmarkPanel` | The thermal readout polls only while the panel is shown (stops on hide, resumes on show) and behaves the same in interactive, startup-smoke and accessibility-audit modes; the System Inventory Report asks for a folder (starting in Documents), runs its real read-only collectors, writes there and names the saved file, and a cancelled folder choice runs nothing |
| test_detachable_log_window | `DetachableLogWindow` | The anchored log stays inside the screen work area beside a maximized main window, opens without taking focus, ignores refills while hidden, and shows lines literally |
| test_advanced_uninstall_panel | `AdvancedUninstallPanel` | Install-folder size measured only on demand (Measure button, off the GUI thread), cached per folder, never offered for a remote folder |
| test_network_diagnostic_panel | `NetworkDiagnosticPanel` | The start-up adapter scan runs in interactive, startup-smoke and accessibility-audit modes, off the GUI thread, and an immediate teardown joins it |
| test_email_file_scanner_dialog | `EmailFileScannerDialog` | The scanner looks in every writable drive's Backups and Email Backups folders, the system drive's included |
| test_quick_action_panels | `UserMigrationPanel`, `DiagnosticBenchmarkPanel`, `NetworkDiagnosticPanel`, `QuickActionController` | Each panel's quick-action flow driven through a real controller with fake actions and a fake elevated-helper broker (`quick_action_fakes.h`; nothing touches the system): Screenshot Settings and the System Inventory Report ask for a folder starting in Documents and name what they saved, BitLocker Key Backup asks for a folder and hands it to the helper, Verify System Files and Reset Network are confirmed (a No runs nothing), a cancelled folder choice runs nothing, and closing the window cancels a running elevated action through the helper; with all three panels on one controller, each reacts only to its own runs, a click while another caller's run is going says it is queued, and a queued run writes into the folder its own caller chose |
| test_app_installation_panel | `AppInstallationPanel` | Signal connections and the bundled Chocolatey initialization run in interactive, startup-smoke and accessibility-audit modes (a Search click reaches the handler, which finds Chocolatey initialized) |
| test_window_input | `MainWindow`, `wheel_policy` | Inside the File Explorer its own Ctrl+1..9, Ctrl+Tab and Ctrl+Shift+1..6 bindings act (the main window's identical application-wide shortcuts step aside) and outside it the main window's act; an unfocused combo box, spin box or slider and any tab bar leave the wheel to the scrolling view; a focused combo steps; the wheel never moves keyboard focus |
| test_app_theme | `MainWindow`, `windows11_theme`, `ConfigManager`, `InfoButton` | The one theme setting (ui/theme: System by default and for an unknown value, saved by the status bar's Theme menu); System follows the Windows app theme live, Light and Dark ignore it; no per-panel Dark switch remains; with every tab of the real window built and the theme switched live, no widget keeps a colour of the other theme in its own style sheet (SAK_THEME_SCREENSHOT_DIR saves each tab as PNG); no widget carries a graphics effect, the info popup is outlined by its 1px border, and each visible group box and card draws a 1px border in both themes; every tool tab has one Log switch at the page's left margin under the page at the capture size, 1680x900 and the minimum size; App Management sub-tabs keep a margin in their pane; no widget text uses an em dash or " -- "; AI workspace labels paint no fill; a selected tab shows its icon's own drawing, reading 3:1 on the selected tab's fill in both themes (SAK_THEME_SCREENSHOT_DIR also saves the tab bar with its first tab selected); switching tabs clears the last tab's status |
| test_theme_widget_states | `windows11_theme`, `style_constants`, `color_constants`, `widget_helpers`, File Explorer style | Offscreen on the Windows 11 style with the real theme: checked boxes tick, radios dot, item-view boxes match; selected input text, disabled inputs, placeholders, focus frames, one selection shade with no gaps, spin-box height, transparent plain containers, one border and fill for group boxes, cards and panes, link buttons, submenu arrows, File Explorer chrome states, header weight, page title size, rich-text rules; in dark plain buttons keep their fill, progress bars and disabled buttons suit the theme; WCAG AA for text tokens, button and tab gradients and check box outlines in both themes |
| test_single_instance_guard | `SingleInstanceGuard`, `ElevationManager`, `MainWindow` | One instance per data folder whatever its spelling (case, trailing separator, a junction); another folder runs side by side; a second launch makes the running instance emit activationRequested; an elevated relaunch waits for its launcher and times out with a reason while it runs; the relaunch carries --elevated-relaunch; an asked window is restored from minimized and a never-shown one stays hidden |
| test_plural_placeholders | every string literal under `src/` and `include/` | No message the application can build says "file(s)", "match(es)" or "director(ies)": counts go through `sak::countOf`, labels say what they mean (UI review S24); `http(s)` is a URL scheme and allowed |
| test_single_instance_app | `scripts/test_single_instance_app.ps1`, `sak_utility` | The real app: a second launch on the same data folder exits 0 without starting and says the app is already running while the first stays up; a launch with --elevated-relaunch neither starts nor exits until the first instance ends, then completes its startup smoke |
| test_screenshot_settings_action | `ScreenshotSettingsAction` | Report seams; only a Settings window this run opened is closed, found by EnumWindows, with no taskkill/tasklist/fixed sleeps |
| test_backup_bitlocker_keys_action | `BackupBitlockerKeysAction` | Key-gate, backup-location screening, and refusal of a destination on a BitLocker volume whose key is in the backup |
| test_verify_authenticode_signatures | `scripts/verify_authenticode_signatures.ps1` | The publisher pin on real signed binaries; bundled tools keep their vendor's signature |
| test_stage_portable_release | `scripts/stage_portable_release.ps1` | Portable staging contract, including that the package ships no portable.ini |
| test_drive_unmounter | `drive_unmounter` | Volume unmount, lock/unlock |
| test_flash_types | `flash_types` | Flash operation data structures |
| test_flash_worker | `flash_worker` | Raw sector write, verification, decompression |
| test_flash_coordinator | `flash_coordinator` | Multi-drive flash orchestration |
| test_image_source | `image_source` | Image file detection, format identification |
| test_windows_usb_creator | `windows_usb_creator` | NTFS format, ISO extraction, boot flag set once, no bcdboot step, bounded wait for the new volume instead of fixed sleeps |
| test_streaming_decompressor | `streaming_decompressor` | Streaming gzip/bzip2/xz decompression, error handling |
| test_decompressor_factory | `decompressor_factory` | Format detection, correct decompressor selection |
| test_wifi_profile_scanner | `wifi_profile_scanner` | WLANProfile parsing (security type, hidden flag, plaintext key only when `protected` is false), and the refusal to retain XML alongside plaintext keys |

### Advanced Search
| Test | Module Under Test | Coverage |
|---|---|---|
| test_advanced_search_types | `advanced_search_types` | Search result structures, match types |
| test_advanced_search_controller | `advanced_search_controller` | Search orchestration, result aggregation |
| test_advanced_search_worker | `advanced_search_worker` | Content search, regex, binary/hex, safe default file-link filtering, synthetic EXIF metadata matrix, archive |
| test_regex_pattern_library | `regex_pattern_library` | Built-in patterns, custom pattern persistence |

### Advanced Uninstall
| Test | Module Under Test | Coverage |
|---|---|---|
| test_advanced_uninstall_types | `advanced_uninstall_types` | Program info, leftover structures, risk levels |
| test_advanced_uninstall_controller | `advanced_uninstall_controller` | Uninstall orchestration, batch queue; the batch restore point is made off the GUI thread and a cancel during it stops the batch |
| test_leftover_scanner | `leftover_scanner` | File/registry/service leftover detection; the protected system folders follow this machine's own Windows, Program Files, ProgramData and Users folders (known-folder API), subtree included |
| test_registry_snapshot_engine | `registry_snapshot_engine` | Pre/post snapshot, diff computation |
| test_program_enumerator | `program_enumerator` | Win32/UWP/provisioned app enumeration |
| test_uninstall_worker | `uninstall_worker` | Uninstall execution, cleanup |

### Directory Organizer
| Test | Module Under Test | Coverage |
|---|---|---|
| test_organizer_worker | `organizer_worker` | File organization by category, collision handling |
| test_duplicate_finder_worker | `duplicate_finder_worker` | MD5 hashing, duplicate detection, parallel scan |
| test_file_management_file_system | `FileManagementFileSystemBridge` | Mounted/raw/image target capability mapping, non-native organizer blockers, ext/HFS+/APFS reader routing, explicit HFS+ mutation blockers/options, APFS write gating by known container size (superblock-derived for raw partitions; S.A.K.-generated and real Apple foreign media both write-enabled in range, unknown-size/out-of-range read-only with exact blockers), file-system-specific safety notes, one-spelling-per-target file-system display naming, `copyFileToHost` byte-exact local copy with SHA-256 and atomic fail-closed destination handling, and recursive `exportDirectoryToHost` folder copy-out |
| test_file_explorer_types | `FileExplorer*` types and `FileExplorerCommandRegistry` | Files-like explorer target IDs, explicit-ID enforcement, item capability mapping, local/raw path normalization, pane history, selection summaries, command metadata, feature gates, layout-picker blocker gates, target-type command-state coverage, raw write blockers, and the full Files LayoutSizeKindHelper metric tables (row heights, grid widths, and per-layout icon sizes, each asserted smaller than its own row) |
| test_file_explorer_item_model | `FileExplorerItemModel`, `FileExplorerSortFilterModel` | Model/view row/column roles, entry paths and item metadata roles, attribute summary roles, folder-first source/proxy sorting, proxy filtering, size sorting, clear/reset behavior, tag-filter proxy restriction, and the injected-provider Tags column/role |
| test_file_management_explorer_panel | `FileManagementExplorerPanel`, `FileExplorerPane`, `FileExplorerDetailsView` | Files-like shell creation, grouped sidebar target selection, command-bar disabled blockers, functional View/layout picker without milestone labels, details/list/grid/cards/adaptive switching, Columns current/child-preview surfaces, shared selection survival, per-target/path view persistence, hidden-items and file-extension toggles, current-folder filter, command palette entry points + grouped headers, omnibar path load, richer omnibar search dialog (target badge, history seed, result list, routing buttons), status strip, persistent detail panes, loading/empty/error pane states, responsive collapse, extended selection, details column persistence, directory activation route, table/sidebar context menu actions, clipboard Copy/Paste local round trip, dual-pane stack orientation + tab-session round trip, stale-favorite removal, sidebar-rebuild navigation preservation, favorites/recent persistence, Tags-column repaint, evidence-report target matching, baseline screenshot capture, and shortcut smoke coverage. Visual/density lanes: the persistent preview showing a real local text file's own contents; the Safety pane's Write/Read/Browse verdict and blocker text for ext4, HFS+, in-range APFS, out-of-range APFS, XFS and Btrfs (manual image targets added through the real Add Raw/Image dialog); Open In Second Pane listing the selected folder in pane B while pane A stays put; commands acting on the active pane only, including activation transferring on a press in empty space and the pane losing focus dropping its selection; icons and shell ground following the application palette in both themes (ink luminance compared per icon, rendered pixels compared for the shell); command-bar icon/label consistency including one shared icon size across QPushButton and QToolButton controls; status labels having the content-box width their own text needs; single-line sidebar rows with the detail in the tooltip; the details view never showing or hiding a column inside the header's layout pass (the splitter's narrow-then-wide resizes and the header menu's hide in a narrow pane, forced in Release); and every panel instance using an injected `QTemporaryDir` local target instead of host-mounted-volume discovery, with zero host-target `QSKIP` paths. |

### Partition Manager
| Test | Module Under Test | Coverage |
|---|---|---|
| test_partition_manager_core | `partition_manager_*` | Genuine Apple 2-chunk and 135-chunk/2-CIB fixture reconstruction, foreign scattered-internal-pool COW rotation and live-metadata exclusion, chained insert/patch/replace/delete read-back, foreign multi-CIB shrink, inventory parsing, resilient volume lookup script coverage, hidden EFI/MSR file-system fallback labels, file-system support registry coverage, read-only raw signature detection for ext2/ext3/ext4 with superblock metadata, XFS/Btrfs read-only superblock metadata and sanity warning coverage, APFS read-only container/checkpoint ring/object-map/volume-OID metadata, visible object-map tree anchors, visible referenced root/non-root B-tree node headers, root `btree_info_t` metadata, bounded volume-superblock candidate metadata, referenced-object-header preflight, APFS metadata object-checksum rejection in the read-only browser, APFS browse/extract/export capability registry coverage, fail-closed APFS writer certification preflight coverage including oversized generated-layout rejection and malformed spaceman chunk/CIB sanity warnings, APFS object checksum calculation/stamping/verification, operation-specific image-only file/format/repair/resize/volume-label mutation plan shapes, safe target-path normalization/traversal blockers, raw APFS format/write/patch/delete/directory/volume-label/repair blocker coverage for missing raw paths, missing confirmation, and image-only options, APFS generated/minimal layout guard rejection for malformed generated media and unsupported APFS layouts, APFS generated root-file write/patch/delete, empty root-directory create/delete, root-directory child-file write/patch/delete, and volume-label Apply script generation and safety gates, APFS image-only format scratch-image generation with APFS detection/free-space/root-listing proof and 9000-byte seed-file read-back proof, APFS generated-image volume-label change proof with old/new-label read-back, APFS generated-image root-file add/replace/byte-range patch/delete proof with bounded existing root-file preservation, multi-block read-back, negative read-back, and empty-root-after-delete coverage, APFS generated-image empty root-directory create/delete proof with empty-listing and absence validation, APFS generated-image root-directory child-file create/read/patch/delete proof with non-empty directory delete blocking and empty-directory-after-delete validation, APFS generated-image checksum-repair proof with corrupt-before/repaired-after browse and read-back, and sanity warning coverage, XFS/Btrfs/APFS metadata consistency capability registry coverage, ext2/ext3/ext4 read-only directory/file browsing, selected-file extraction, symlink target display/sidecar export, and bounded recursive directory export for direct, indirect, and extent-mapped data, HFS+ catalog consistency checks, HFS+ attributes B-tree key scans plus structured fork/inline metadata reporting, HFS+ catalog browsing, selected data/resource-fork extraction, selected inline/fork-backed attribute-value extraction, bounded recursive directory export with `.rsrc` sidecars, catalog/data/resource/attribute extents-overflow resolution, image-only same-size HFS+ data-fork overwrite with fail-closed gates, initial-extent and extents-overflow write coverage, data/resource-fork allocated-block grow/shrink replacement with catalog logical-size updates, stale-tail zeroing, inline-attribute replacement with record-capacity checks, fork-backed attribute replacement within existing allocated blocks, bounded initial-extent data/resource/fork-backed-attribute allocation growth with bitmap/counter read-back, broad allocation-growth blocking, unsupported inline/broad attribute-growth blocking, and read-back hashes, Linux swap header metadata and original SWAPSPACE2 format script generation, direct and HFS-wrapper HFS+, HFSX, and unknown data, filesystem tool-manifest approval/blocker coverage, manifest-gated read-only checker command generation and approval resolution, ext e2fsprogs format/repair/resize command generation with destructive confirmation blockers, HFS+/HFSX `newfs_hfs` format and `fsck_hfs` check/repair command generation, HFS staged selected-raw-partition data-fork/resource-fork/inline/fork-backed-attribute mutation script generation and safety blockers, APFS generated create/format/repair script generation through `sak_apfs_writer_cli.exe`, APFS GPT type validation, manifest-gated ext/HFS format/repair/grow/shrink scripts, non-native write safety gates including exact selected raw partition target matching, ext shrink usage-metadata blocker, native ext format rejection, safety blockers, Create Image read-only risk and unsafe destination blockers, resize/merge/MBR2GPT script generation, resize adjacent-free-space/no-op/below-used-space/start-move/donor-space blockers, recovery write-back acknowledgement/bounds blockers and scripts, offline-image and raw-source Data Recovery scan/restore with source-hash verification, clone verification scripts, offset partition-clone scripts, create selected-region offset scripts, create/format/change cluster-size scripts, create partition type scripts, partition-clone region overwrite confirmation and unsafe physical-target blockers, PowerShell escaping, redo-stack state, queue layout-hash guard |
| test_partition_manager_panel | `PartitionManagerPanel` | AOMEI-style panel chrome, stateful Scan Disks/Refresh Disks inventory control, status-bar inventory summary, selection-gated sidebar actions, enabled HFS+ read-only catalog check action, enabled Inspect Non-Windows File System metadata report for detected non-native volumes, disabled unsupported HFS+/APFS/XFS/Btrfs Windows-native check/resize/cluster/label paths with active Non-Windows alternatives, generated-layout-gated APFS Container action dialog and queue wiring (snapshot-create mode with resize/name-field visibility toggling; the former per-file APFS/HFS mutation dialogs moved to the File Management explorer and are covered by its bridge and panel suites), XFS sanity metadata surfacing in File System tooltip, Properties, Inspect, and Check Non-Windows File System metadata report labeled as last-disk-scan results, Browse Non-Windows File System ext, HFS+, and APFS enablement including ext symlink target display and HFS+ selected attribute-value extraction, confirmed ext format/repair/grow/shrink, Linux swap format, APFS generated format, and generated APFS repair queue wiring with fixed selected raw partition targets for queued repair, fixed-width sidebar, compact icon text-link actions, no redundant sidebar preview box, fixed-size Icons8 SVG ribbon icon resources with disabled SVG mode mappings including the dedicated Dry Run icon, compact unframed 1 px disk-map gutter, full commercial legend roles, rendered legend swatches and inner bars matched to meaningful colors for GPT/Primary, Logical, Simple, Spanned, Striped, Mirrored, RAID5, and dark-gray Unallocated segments, rendered neutral outer disk-map shells, selected partition highlights, whole-disk row highlights, mouse/right-click disk-map target selection synchronized with table selection, redo enabled only after Undo creates a redoable action, equal-height disk tiles and partition segments, rounded disk row/tile containers, final Apply before/after diff, table and disk-map context menus without ribbon/Pending Operations commands, Properties including raw file-system metadata, Explore, Data Recovery image/raw-source entry point, Quick Partition preset-capable entry point, Extend Partition Wizard, Allocate Free Space adjacent-donor backup/delete/extend/recreate/restore queue path, unallocated Allocate Free Space To resize/move queue paths, Change Cluster Size queued backup/reformat/restore dialog, primary/logical conversion queue dialog, volume serial-number queue dialog, dynamic-to-basic queue dialog, Space Analyzer tree/largest-file/file-type views with Open/Explore/Copy Path actions, Disk Benchmark, Manage BitLocker status dialog, SSD erase (TRIM + overwrite) readiness and typed queue-confirmation dialog with no internal checklist, run-now Check File System (per partition) and disk-level-only Scan File Systems that never enter the queue, honest action names, Make Bootable Media actions, create-dialog handle synchronization and direct preview-bar drag, disk separators, resizable disk map, and borderless no-row-number partition table |
| test_partition_manager_controller | `PartitionManagerController` | Async cancellable refresh/apply-preflight/apply-verification state machine, fresh same-hash safety revalidation, failed/partial post-scan downgrade with queue retention, reentrant admission ownership, cancellable filesystem checks and teardown joins, authoritative inventory states, dry-run/undo/elevation transitions, and atomic 256-operation queue capacity |
| test_sak_apfs_writer_cli | `sak_apfs_writer_cli.exe` | Production APFS helper self-test covering exact-size image grow then shrink, generated APFS image format, generated-image volume-label change with old/new-label read-back, generated-image root-file create/replace/byte-range patch/delete with payload/read-back or deleted-file hashes, generated-image empty root-directory create/delete with empty-listing and absence validation, generated-image root-directory child-file write/patch/delete with child read-back and empty-directory-after-delete validation, missing-delete and non-empty-directory-delete blocking, intentional metadata checksum corruption, generated-layout checksum repair, JSON output, and raw write/patch/delete/directory/child-file/volume-label command refusal for normal file paths. |
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

The foreign multi-CIB APFS case in `test_partition_manager_core` expands two
18,043,854,848-byte disk images. Set `SAK_TEST_IMAGE_SCRATCH_ROOT` to an
existing scratch directory on a storage drive with at least 40 GB free before
running it; an explicit but unusable directory fails the test. The test uses
the system temporary directory only when the variable is unset. Set `TEMP`
and `TMP` to that same directory for the complete CTest run so other scratch
images also stay on the storage drive. Sparse allocation is attempted when
supported, but the test does not require it (for example, on exFAT). On slow
non-sparse storage the case can exceed Qt Test's default 300000 ms function
watchdog; set `QTEST_FUNCTION_TIMEOUT=900000` for that run without changing
any test assertion. The measured local exFAT case took 518182 ms.
The `rawDeviceIo_copyFileSparsePreservesContentAndSucceeds` row also checks
that a sparse-capable volume produces a sparse destination and a non-sparse
volume produces a byte-identical dense destination. Run that row on each
filesystem to exercise both production copy strategies; a pass on NTFS alone
does not prove exFAT behavior.

Destructive Partition Manager certification is intentionally outside CTest: every row changes a disk, and runs in the disposable VM. See `docs/PARTITION_MANAGER_CERTIFICATION.md`. CTest runs what checks its evidence, on synthetic files: `test_partition_manager_row_evidence`, `test_partition_manager_certification_status`, `test_partition_manager_release_claims`, `test_partition_manager_rig_pc_target`, `test_partition_manager_code_digest` and `test_partition_manager_matrix_generated`.

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
2026-07-12 command-route certification extended this to foreign media and HFS+:
the destructive command route passed on a 7.45 TB real Apple-created APFS
container (apfsck -cw EXIT 0 clean) and a 256 MiB Apple-HFS partition (fsck_hfs
clean), and ext4 read-only copy-out plus XFS/Btrfs metadata-only blockers passed
on WSL-built images. Evidence under
`artifacts/file-management-live-certification/{disk2-foreign-apfs-destructive,disk1-hfs-file-management-cert,ro-ext-xfs-btrfs-20260712}/`.
APFS writes are gated by a known container size in the certified engine range
(superblock-derived for raw partitions); unknown-size or out-of-range targets
stay read-only with exact blockers.

### Email Inspector
| Test | Module Under Test | Coverage |
|---|---|---|
| test_email_types | `email_types` | Email data structures, item types, MAPI properties |
| test_pst_parser | `pst_parser` | PST/OST header parsing, generated PST/OST/NST store matrix, legacy Unicode page sizing, compressible PST decode, folder tree, item detail, attachments |
| test_mbox_parser | `mbox_parser` | MBOX parsing, message extraction, header parsing |
| test_email_search_worker | `email_search_worker` | Full-text search, field filtering |
| test_email_export_worker | `email_export_worker` | EML/CSV/VCF/ICS export, attachment extraction |
| test_pst_recurrence | `pst_recurrence` | Repeating appointments: every pattern, end, and occurrence list; time zones; what cannot be read is said, never guessed |
| test_email_ics_writer | `email_ics_writer` | RFC 5545 output: attendees, RRULE, EXDATE, RECURRENCE-ID, VTIMEZONE, folding, injection |
| test_mbox_python_reader | `ost_conversion_worker`, `mbox_writer` | A converted mailbox read back by Python's mailbox module, an independent reader |
| test_email_attachments_browser_dialog | `EmailInspectorController`, `EmailAttachmentsBrowserDialog` | Exact folder-page and item-detail failure identities, wrong-ID refusal, matching failure advancement, plain-text accessible generic error reporting without scan-state mutation, and attachment-specific plus generic double-signal save accounting |
| test_email_profile_manager | `email_profile_manager` | Email client profile discovery, backup/restore, and controlled junction destination confinement |
| test_email_report_generator | `email_report_generator` | Email analysis report generation |

### Quick Actions
| Test | Module Under Test | Coverage |
|---|---|---|
| test_action_factory | `quick_action` | Action creation completeness, metadata validity (name, description, category), initial state, no duplicates, all categories populated |
| test_all_actions_metadata | `quick_action` | Validates every registered action has complete metadata |
| test_quick_action | `quick_action` | Base action interface, state transitions |
| test_quick_action_controller | `quick_action_controller` | The application's one controller: each run carries its own output folder and owner (a queued run writes where its own caller chose), a request while another runs is queued and announced, a refused request finishes at once, a caller's cancel touches only its own runs, every elevated run shares the one broker the controller owns (faked through the SAK_CORE_TEST_HOOKS factory) and gets its own folder, an unsafe folder is never handed to the helper, destroying the controller cancels the elevated task and shuts the helper down, and teardown during a stuck run retains the thread and its action in custody instead of blocking |

### Integration Tests
| Test | Workflow | Coverage |
|---|---|---|
| test_ost_integration | Email Inspector | Generated OST/PST fixture opening, folder tree, item loading, item detail, and export |
| test_uup_conversion_pipeline | UUP-to-ISO | UUPMediaConverter exe existence, process attachment (no fork), help output, missing UUP dir, empty UUP dir |
| test_offline_package_builder | Offline deployment | Opt-in Chocolatey dependency closure and package bundle; service/CDN failure hard-fails after opt-in |

### Mutation Runner Regression Tests

`test_mutation_runner` registers the Python runner tests with CTest. Its cases exercise
assertion kills, survivor refusal, compilation and restoration failure, missing/empty test
reports, real CTest timeout, Windows child-tree cleanup, unconfirmed termination refusal, and
retention of fresh Qt assertion logs, exclusive admission, process-instance binding, job-contained
children, and complete hashed snapshot validation before restoration. A timeout or compiler failure never counts as mutation
coverage. Panel modal callbacks run offscreen, close through scoped ownership on failed
assertions, and report watchdog expiry as failure.
LF-authored catalog needles adapt to LF or CRLF source without changing source
bytes; mixed or bare-CR line endings fail closed. Validator and runner share
that rule, and a real copied mutation restores original CRLF bytes exactly.
Windows Job cleanup pins live member process handles before termination and
waits for those exact objects to signal; an accounting count of zero alone is
not treated as descendant-exit proof. Negative tests cover incomplete member
lists, foreign process binding, unsignaled members and failed enumeration.

`test_source_fingerprint` runs real Windows VS2022 CMake configure/build fixtures. Its cases
verify path/content binding, deterministic duplicate handling, header-triggered regeneration,
rename invalidation, and refusal of missing, empty or out-of-root inputs. Certification source
stamps must change before a helper rebuild when its declared inputs change. Native archive/object
fixtures cover same-size backdated C/C++/header edits, uppercase extensions, and Windows junction
escape/retarget refusal; the gate verifies rebuilt code output, not just a new digest string.

`test_partition_manager_live_certifier` runs only non-destructive native CLI/admission contracts.
`test_partition_manager_live_certifier` also pins the step client's own checks, with `--self-test`. It performs no disk write.
Actual controller-driven CreateImage/RestoreImage remains a separate approved-VM certification.

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
        sak_core           # the product library that holds the code under test
        Qt6::Test Qt6::Core
        # ... add modules your test needs
    )
    add_test(NAME test_my_component COMMAND test_my_component)
endif()
```

Product code comes from the static product libraries the root `CMakeLists.txt` declares
(`sak_foundation`, `sak_partition_fs`, `sak_core`, `sak_secret_redaction`, `sak_win32mcp`,
`sak_ai_core`, `sak_gui`), so a test target lists only its own test sources, fixtures and fakes.
Do not add a `src/` file to a test
target: a new product source goes into its library's list and reaches every test from there. The
exceptions are a target that must compile a product file with a test-only definition that changes
a class (the two Partition Manager hook tests), the two suites that drive partition/filesystem test seams compiled only under `SAK_PARTITION_FS_TEST_HOOKS` (`test_partition_manager_core` and `test_file_management_file_system`; see `SAK_PARTITION_FS_TEST_HOOK_SOURCES`), and the certification helpers, which compile their
sources directly so their embedded source fingerprint covers every object they contain.

## Best Practices

- **Isolation:** Use `QTemporaryDir` for file operations -- auto-cleaned after test
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

`QSKIP` is reserved for real environment or certification interlocks, never an unsupported
application-platform branch. `tests/skip_site_inventory.txt` holds every static site by file and
exact count, while `tests/skip_baseline.txt` holds the functions that skipped in the latest full
run. Check
dormant/static coverage with
`powershell scripts/check_test_skips.ps1 -StaticOnly`; the full skip audit also requires fresh
per-test logs from complete CTest. It reads CTest's JSON registration to require every current Qt
log and exclude stale logs from removed tests, so a stale success cannot inflate coverage and a
missing current log cannot disappear from the audit.

PowerShell gate regressions run under both PowerShell 7 and Windows PowerShell 5.1. The
false-green command harness runs once per host and covers fixture ownership, inherited execution
policy, secret-scan token boundaries, wrapped-target registration, portable smoke contracts,
standalone analyzer exit propagation, and the skip audit's stale/missing log handling. Process
capture tests independently pin output, error, exit-code, standard-input, simultaneous large
input/output pipes, and timeout behavior.
Portable staging and archive-recovery fixtures are separate CTests. The
archive scope hard-kills isolated child publishers at publication boundaries,
tests prior/candidate pair reconciliation and tamper refusal, and keeps its
own 180-second limit; the staging scope retains its 240-second limit.
---

**Test Framework:** Qt Test, plus PowerShell, Python and node script tests registered in CTest
**Registered Tests:** `ctest --test-dir <build> -C Release -N` (the registration is the only count)
**Platform:** Windows 10/11

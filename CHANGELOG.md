# Changelog

All notable changes to S.A.K. Utility are documented in this file.

The format is loosely based on [Keep a Changelog](https://keepachangelog.com/).

---

## v0.9.1.2

- **Email panel performance & stability** — Eliminated GUI freezes and a rescan-mid-loop crash class in the email file scanner by moving its synchronous filesystem traversal off the GUI thread onto `QtConcurrent::run`/`QFutureWatcher`; removed both `QApplication::processEvents()` reentrancy band-aids. Batched row population in the file scanner, attachments browser, and contacts dialog under `setUpdatesEnabled(false)` + `setSortingEnabled(false)` + `QSignalBlocker` to cut per-row repaint cost on large mailboxes. Added a 150 ms debounce timer to the contacts search line-edit and a 50 ms debounce timer to the inspector preview so that CID attachment arrivals and remote-image downloads coalesce into a single `QTextBrowser::setHtml()` call instead of one parse per image. Linked `Qt6::Gui` into `test_pst_parser`, `test_ost_integration`, `test_email_search_worker`, and `test_email_export_worker` to match the parser's `QTextDocument` dependency.
- **Assertion audit (two passes)** — Removed and corrected ~30 misplaced or wrong `Q_ASSERT` calls across core workers and quick actions: tautological status checks immediately after `setStatus()`, asserts on pointers fired before `make_unique` allocation, asserts on optional user-input fields (technician name, ticket number, firewall conflicts), and duplicate `.empty()`/`.isEmpty()` pairs on the same container. Files touched include `config_manager`, `bandwidth_tester`, `wifi_analyzer`, `cpu_benchmark_worker`, `flash_worker`, `email_search_worker`, `email_export_worker`, `ost_conversion_worker`, `user_profile_backup_worker`, `user_profile_restore_worker`, `network_diagnostic_report_generator`, and 7 quick-action implementations.
- **Public-API hardening** — Crash-risk asserts on user-supplied input replaced with `logWarning` + safe-default return in `ConfigManager` getters/setters, `WifiAnalyzer::setScanInterval`, `FlashWorker::setBufferSize`, and `ScreenshotSettingsAction` URI/process-name handling.
- **Optimization** — Extracted duplicated `QStorageInfo` filter loop in `CheckDiskErrorsAction` into a file-local helper with `.reserve()`. Added `qsizetype` loop counter and `log_lines.reserve()` in `BackupBitlockerKeysAction`. Added `[[nodiscard]]` to 8 pure query getters across `app_installation_worker`, `chocolatey_manager`, `detachable_log_window`, `flash_coordinator`, `user_profile_backup_wizard`, and `windows_iso_downloader`.
- **Repo hygiene** — Removed previously-committed `_archived/network_transfer/` (74 files of legacy peer-to-peer transfer code) from version control and added `/_archived/` to `.gitignore`. Added `*.ost` and `cppcheck_errors.txt` ignores.
- **Build quality** — Clean MSVC `/W4 /WX` build, 93 automated tests passing.

## v0.9.1.1

- **Comprehensive documentation accuracy audit** — Verified all documentation, in-app About dialog, GitHub Actions workflow, and third-party license files against the actual codebase. Corrected stale file counts (167 headers, 157 source files, 93 CTest tests across 117 test files), updated Qt modules (added Xml), and refreshed all feature descriptions.
- **OST Converter documented** — Added full OST Converter feature section to README (8 output formats, multi-threaded conversion, deleted item recovery, PST splitting, IMAP upload). Updated About dialog, workflow release body, and plan doc status to reflect implementation.
- **Stale references cleaned** — Removed 7-Zip from About dialog credits and license tab (removed in v0.9.0). Removed scrapped System Tools tab plan from Roadmap. Removed Quick Actions standalone section from release body (panel removed in v0.9.0.6).
- **Missing credits added** — Added iPerf3 (BSD 3-Clause) and Icons8 to About dialog credits. Added iPerf3 full license to THIRD_PARTY_LICENSES.md. Added iPerf3 to README dependencies table and acknowledgments.
- **Workflow hardened** — Added Qt6Xml.dll to required DLL verification. Added iPerf3 presence check to build verification step. Updated release body with current feature descriptions, per-task elevation language, and per-panel settings.
- **Build quality** — Clean MSVC `/W4 /WX` build, 93 automated tests (all passing), 157 source files, 167 headers.

---

## v0.9.1.0

- **Per-task elevation** — Replaced the global `requireAdministrator` manifest with `asInvoker`. The app now runs as a standard user and elevates only for specific tasks that require it, via an elevated helper process communicating over Named Pipes IPC. Tasks drop back to standard privileges once complete.
- **Elevation tier system** — Introduced `ElevationTier` (Standard/Elevated/Mixed) and `FeatureId` enums classifying 40+ features. Consolidated three duplicate `isRunningAsAdmin()` implementations into `ElevationManager::isElevated()`.
- **Elevated helper process** — New `sak_elevated_helper.exe` with `requireAdministrator` manifest, connected via Named Pipes with a custom framed protocol (`[4-byte LE length][1-byte type][UTF-8 JSON]`). Security: random nonce in pipe name, parent PID validation, DACL restrictions, task allowlist, single connection.
- **Elevation broker** — `ElevationBroker` client manages launching the helper, sending task requests, receiving results, and handling cancellation/timeout. Integrated into `QuickActionController` for seamless elevated action execution.
- **Elevation gates** — Reusable `showElevationGate()` dialog prompts users before elevated operations (Image Flasher, UUP ISO, App Installation). Users can restart elevated or decline.
- **Mixed-tier operations** — Backup worker path-access detection (`canReadPath()`), `PermissionManager` `std::expected` overloads, 5 new helper task handlers (TakeOwnership, StripPermissions, SetStandardPermissions, BackupFile, ReadThermalData), `ThermalMonitor::hasCpuTemperature()`.
- **Status bar elevation indicator** — Icons8 Flat Color SVG icons (shield for admin, lock for standard user) with text label and tooltip. "Run as Admin" button to restart elevated when needed.
- **Elevation info banners** — Informational banner on Image Flasher and App Installation panels indicating that some operations may prompt for elevation.
- **Legacy cleanup** — Removed `runElevatedQuickAction()`, `requestAdminElevation()`, CLI argument parsing (`--run-quick-action`), and associated Win32 headers. No more ShellExecuteEx-based self-re-launch.
- **New error codes** — `elevation_required`, `elevation_failed`, `elevation_denied`, `elevation_timeout`, `helper_connection_failed`, `helper_crashed`, `task_not_allowed`.
- **New tests** — 6 new test files with 139 test methods covering elevation tiers, IPC protocol, task dispatcher, mixed-tier operations, UX components, and hardening verification.
- **Comprehensive documentation accuracy audit** — Verified all in-app About panel feature descriptions, wizard welcome pages, README highlights, CHANGELOG entries, and build script claims against the actual codebase. All feature descriptions confirmed accurate. Corrected header file count (142 headers including action headers), corrected README test count reference (74 automated tests).
- **Build quality** — Clean MSVC `/W4 /WX` build, 93 automated tests (all passing), 157 source files, 167 headers.

---

## v0.9.0.9

- **Email inspector HTML/text slider toggle** — Replaced the plain-text/HTML toggle button with a `LogToggleSwitch` slider, repositioned to the bottom row alongside the log toggle for a cleaner layout.
- **Open containing folder in file scanner** — Added "Open Containing Folder" button to the email file scanner dialog, enabling quick navigation to the directory of any discovered mailbox file.
- **Network diagnostics right-click context menus** — Added extensive context menus to all 9 tables (Ping, Traceroute, MTR, DNS, Port Scan, WiFi, Connections, Firewall, Shares) with tab-specific actions (copy IP, traceroute to hop, ping remote, open share in Explorer) and common actions (copy selected/all rows, export CSV, clear).
- **Ping DNS resolution fix** — Added `WSAStartup`/`WSACleanup` to `ConnectivityTester` — `getaddrinfo()` requires Winsock initialization, which was missing. IP-based pings worked but domain name resolution silently failed.
- **SMART report discrepancy fixes** — Updated HTML, JSON, and CSV diagnostic reports to include Serial Number, Interface Type, and NVMe Wear Level columns, matching the data shown in the SMART panel.
- **Documentation audit** — Renamed "Email Tool" to "Email Tools" across all docs (README, About HTML, CHANGELOG, OST Converter plan), corrected Settings section (per-panel settings, not global menu), updated file counts.
- **Build quality** — Clean MSVC `/W4 /WX` build, 74 automated tests (all passing), 138 source files, 135 headers.

---

## v0.9.0.8

- **Offline deployment — Direct Download mode** — Downloads the actual installer binary (EXE/MSI) for each package instead of just the .nupkg wrapper. Supports three package patterns: URL-based downloads (`Install-ChocolateyPackage`), embedded-binary packages (`Install-ChocolateyInstallPackage` with EXE/MSI files inside the nupkg's `tools/` directory), and meta-packages (follows `.install` dependency chain via nuspec parsing). Primary installer selection ensures only the main resource is downloaded per package, avoiding patches and secondary resources. Tested with Office PC preset (10 packages, all succeeding).
- **Install script parser improvements** — Splatting regex now matches any variable name (not just `$packageArgs`) with line-anchored closing brace to handle `${variable}` syntax in URLs. `extractHashtableValue` uses two-pass parsing (quoted first, unquoted fallback). `resolveVariables` handles both `$varName` and `${varName}` references.
- **TigerStyle reframed** — Updated copilot-instructions.md to position TigerStyle as a guiding philosophy and best-practice target rather than a hard blocker. Release hooks (zero build warnings, zero build errors, all tests passing) are the hard gates.
- **Documentation accuracy audit** — Corrected file counts across all documentation (135 headers, 138 source files, 99 test files, 75 CTest tests), updated CONTRIBUTING.md TigerStyle section, refreshed tests/README.md structure.
- **Build quality** — Clean MSVC `/W4 /WX` build, 75 automated tests (all passing), 138 source files, 135 headers.

## v0.9.0.7

- **File Converter tab plan** — Comprehensive implementation plan for universal offline file conversion (documents, images, audio, video, spreadsheets, PDFs) with batch processing. New tab planned for the File Management panel.
- **System Tools tab plan** — Comprehensive implementation plan for a centralized launcher of 48+ built-in Windows admin/diagnostic utilities with categorized grid, search, favorites, and availability detection. New tab planned for the Benchmark & Diagnostics panel. *(Later scrapped — plan removed.)*
- **OST Converter tab plan** — Comprehensive implementation plan for multi-threaded bulk OST/PST conversion to PST, EML, MSG, MBOX, and DBX formats with IMAP cloud upload, deleted/corrupt item recovery, metadata preservation, and PST splitting. New tab planned for the Email Tools panel. *(Fully implemented — see OST Converter in Email Tools.)*
- **Documentation accuracy audit** — Corrected header count (134), test file count (96), removed stale Network encryption security claim (feature was removed in v0.9.0.6), updated roadmap with planned features.
- **Build quality** — Clean MSVC `/W4 /WX` build, 74 automated tests (all passing), 137 source files, 134 headers.

## v0.9.0.6

- **Quick Actions panel removed** — Distributed 7 quick actions into their destination panels: Screenshot Settings and BitLocker Key Backup moved to Backup and Restore, Optimize Power Settings / Verify System Files / Check Disk Errors / Generate System Report moved to Benchmark and Diagnostics, Reset Network Settings moved to Network Management. Removed the standalone Quick Actions panel, ActionFactory, and 5 unused actions. All quick actions are now accessible directly from the panel where they are most relevant.
- **Network Transfer panel removed** — Removed the entire Network Transfer feature from the codebase. All source files, tests, and documentation references have been cleaned. Code archived for potential future reuse.
- **Offline deployment enhancement** — Application Management panel now supports offline deployment via exported JSON application lists.
- **Build quality** — Clean MSVC `/W4 /WX` build, 74 automated tests (all passing), 137 source files, 134 headers.

## v0.9.0.5

- **Documentation overhaul** — Complete rewrite of README feature sections to match actual panel organization: Application Management (App Installation + Advanced Uninstall), File Management (File Organizer + Duplicate Finder + Advanced Search), Network Management (Network Diagnostics + Network Adapters + WiFi Manager), Email Tools. Removed stale License Key Scanner references. Fixed Settings section (single Backup tab, not four). Corrected Quick Actions category assignments.
- **In-app About tab rewrite** — About tab HTML updated to reflect all current features: Migration & Backup, Quick Actions, Diagnostics & Benchmarking, File Management, Application Management, Imaging, Network Management, and Email & Data Forensics.
- **Release notes modernization** — GitHub release body rewritten with full per-feature descriptions instead of bullet summaries. Removed stale code signing and installation sections.
- **Build quality** — Clean MSVC `/W4 /WX` build, 91 automated tests (all passing), 186 source files, 151 headers.

## v0.9.0.4

## v0.9.0.3

- **Codebase-wide assertion audit** — Removed ~170+ incorrect assertions left behind by the scrapped TigerStyle linter across 22 source files. Eliminated three dangerous patterns: assert-before-create (asserting a member exists at the top of the function that creates it), contradictory assert+guard (asserting non-null then immediately null-checking), and incorrect state assertions (asserting non-empty on legitimately empty data). 1,487 legitimate assertions verified and retained.
- **Error logging completeness** — Added ~15 missing `sak::logWarning`/`sak::logError` calls alongside `QMessageBox` displays across backup wizards, restore wizards, organizer panel, and settings dialogs.
- **Silent error elimination** — Fixed 7 unchecked `QFile::remove()` calls in critical data paths (encryption cleanup, transfer checksum failures, conflict resolution) and 1 unchecked `waitForFinished()` in service removal.
- **Build quality** — Clean MSVC `/W4 /WX` build, 91 automated tests (all passing).

## v0.9.0.2

- **Comprehensive cppcheck cleanup** — Resolved all actionable cppcheck findings across the codebase with `--enable=all --check-level=exhaustive --inconclusive`.
- **Non-ASCII encoding fixes** — Fixed encoding in ~140 source files for clean cross-platform compilation.
- **Static correctness** — Added `static` to 28 functions that don't use member state (`functionStatic`).
- **STL algorithm adoption** — Converted ~43 raw loops to STL algorithms: `std::any_of`, `std::find_if`, `std::for_each`, `std::generate`, `std::accumulate`, `std::count_if`, `std::copy_if`, `std::transform` (`useStlAlgorithm`).
- **Const correctness** — Added `const` qualifier to 6 member functions: `searchMatch`, `trySearchMatch`, `assessSataAttributeHealth`, `assessNvmeHealth`, `groupByHash`, `checkStop` (`functionConst`).
- **Dead code removal** — Removed unused `SmartThreshold::name` field (`unusedStructMember`).
- **Lizard compliance** — All functions meet CCN < 11 and parameter count < 6.
- **Build quality** — Clean MSVC `/W4 /WX` build, 91 automated tests (all passing).

## v0.9.0.1

- **ISO 9660 analyzer** — New `IsoAnalyzer` class parses ISO Primary Volume Descriptors for volume label, publisher, preparer, application, dates, and file count. Automatic Linux distro identification across 74 patterns (Ubuntu, Fedora, Arch, Manjaro, Kali, Pop!_OS, Proxmox, Clonezilla, GParted, Ventoy, FreeBSD, SteamOS, and more) with desktop environment detection. Searches volume label, publisher, preparer, and application metadata fields to maximize detection across different mastering workflows.
- **Tabbed diagnostics panel** — Diagnostic & Benchmark panel restructured into tabbed interface for better organization and navigation.
- **Image flasher ISO info** — Unified ISO information display with size and format rows integrated into the ISO info group box. Info panel always shown when an image is selected.
- **Icons8 SVG icons** — Added 6 new SVG icons (benchmark, duplicate, settings help, source, destination, orchestrator) for improved visual consistency across panels.
- **Build quality** — Clean MSVC `/W4 /WX` build, 91 automated tests (all passing).

## v0.9.0

- **UUP converter rewrite** — Rewrote `UupIsoBuilder` to drive the bundled `UUPMediaConverter.exe` directly via QProcess, replacing the ConvertConfig.ini generation and batch-script pipeline introduced in v0.6.2. Removed 7-Zip dependency, retry/fallback logic, and broken cancel path. Added `classifyConverterFailure()` for structured error diagnostics and converter output analysis.
- **Code quality audit** — Assertion-density audit, function length / complexity refactoring, magic number extraction, nesting depth reduction across the codebase.
- **Updated licenses & credits** — THIRD_PARTY_LICENSES.md, README acknowledgments, and about dialog updated to reference UUPMediaCreator/OSTooling and bundled wimlib/libwim.
- **Build quality** — Clean MSVC `/W4 /WX` build, 91 automated tests (all passing).

## v0.8.8

- **Error handling hardening** — Clarified best-effort banner probe in port scanner.
- **Magic number elimination** — Extracted `kTimeoutThreadShutdownMs`, `kTimeoutThreadTerminateMs`, `kTimeoutWorkerResetMs` for worker lifecycle, and `kFlashBufferSize`, `kVerifySampleMax`, `kVerifyBlockSize` for flash operations. All bare numeric literals replaced with named constants.
- **Build quality** — Clean MSVC `/W4 /WX` build, 91 automated tests (all passing).

## v0.8.5

- **Enterprise-grade Directory Organizer** — Merged duplicate finder into the organizer panel. Added confirmation dialogs before destructive operations (file count + collision strategy warning), category validation (empty mapping / duplicate name detection), Reset to Defaults button, cross-operation locking (disables organizer widgets during dedup and vice versa), duplicate directory prevention, and scrollable results dialogs for large output.
- **Parallel duplicate hashing** — Dedup settings now expose parallel hashing toggle and thread count spinner with auto-detected ideal thread count display.
- **Codebase security hardening** — 20+ files fixed: `findChild` null checks, hardcoded paths replaced with environment variables (`%SystemRoot%`, `%ProgramFiles(x86)%`), `QProcess::waitForStarted` timeout checks, insecure temp files migrated to `QTemporaryFile`, thread-safe `setError()` in USB creator (29 assignments migrated), `SetFilePointer` → `SetFilePointerEx` for large-disk correctness.
- **UI robustness** — `setFixedSize` → `setMinimumSize` across 3 dialogs for DPI scaling, centralized style constants (`style_constants.h`), tooltip event filter parent fix.
- **Build quality** — Clean MSVC `/W4 /WX` build, 76 automated tests (all passing).

## v0.8.1

- **Network Diagnostics UI polish** — Horizontal 4-column adapter detail layout (Identity, Addressing, Gateway/DNS, Status) replaces single stacked label for better space utilization. Tightened adapter section margins to maximize table visibility. Removed unused report generation section from the Network Diagnostics panel (reports remain in PC Diagnostics). Speed column widened, adapter detail area compacted, QSplitter layout for adapters/tools split.
- **UI refinements** — Style constants centralized (`style_constants.h`), keyboard shortcut improvements, accessible names audit, and layout fixes across multiple panels.

## v0.8.0

- **Advanced Uninstall panel** — Deep application removal with leftover scanning and cleanup. Enumerates Win32, UWP, and provisioned apps. Three uninstall modes: standard, forced (registry-only), and batch queue. Post-uninstall leftover scanner detects orphaned files, folders, registry keys, services, scheduled tasks, firewall rules, startup entries, and shell extensions across three depth levels (Safe / Moderate / Advanced). Risk-level color coding per item. Settings modal with recycle bin deletion, auto restore point, default scan level, and select-all preferences. Locked files are automatically scheduled for removal on reboot via `MoveFileExW`. Registry snapshot engine captures before/after diffs.
- **Network Diagnostics panel** — 10-tool network diagnostic suite: Ping, Traceroute, MTR, DNS Lookup, Port Scanner, Bandwidth (iPerf3 + HTTP), WiFi Analyzer, Active Connections, Firewall Auditor, and Network Share Browser. Adapter inspector with full IPv4/IPv6 config, DHCP, driver details, and traffic stats. Ethernet settings backup/restore to JSON for cross-machine portability. HTML and JSON report generation with technician metadata.
- **76 automated tests** — Full coverage including Advanced Uninstall (types, controller, leftover scanner, registry snapshot engine) and Network Diagnostics (types, utils, report generation).

## v0.7.0

- **Advanced Search panel** — Enterprise-grade grep-style file content search with a three-panel interface (file explorer, results tree, preview pane). Supports text, regex, image metadata (EXIF/GPS), file metadata (PDF/Office/audio/video), archive content (ZIP/EPUB with deflate decompression), and binary/hex search modes. Includes a regex pattern library with 8 built-in patterns and custom user-defined patterns with JSON persistence.
- **Modernized About panel** — Rebuilt as a multi-tab view (About, License, Credits, System) with styled HTML, full dependency attribution (12 third-party components), and live runtime info. Removed dead `AboutDialog` class.
- **Unified UI polish** — Glass-effect 3-stop rgba button gradients, transparent widget backgrounds eliminating patchy gray/white artifacts, harmonized margins and border-radii via style tokens, centered quick-action labels, consistent UUP progress bar height.
- **TigerStyle philosophy (ongoing)** — Code quality tooling (Lizard complexity, cppcheck) and a compliance plan are in place. Current status is tracked and addressed as part of the enterprise hardening work.

## v0.6.3

- **Code cleanup** — Removed legacy `BackupWorker`, `BackupWizard`, and `RestoreWizard` classes that had been superseded by the user-profile wizard suite.
- **Renamed panels for clarity** — `BackupPanel` → `UserMigrationPanel` (`user_migration_panel.h/.cpp`); `WifiQrPanel` → `WifiManagerPanel` (`wifi_manager_panel.h/.cpp`). All internal member names and CMake source lists updated.
- **Dead code pruned** — Removed unused member variables and stale includes from `browser_profile_backup_action.h` left over from a previous refactor.
- **Accurate tech-stack docs** — `THIRD_PARTY_LICENSES.md` now includes the bundled `qrcodegen` library (MIT, Project Nayuki) and corrects the Qt module list. Backup Known Networks action added, WiFi Manager feature section added.

## v0.6.2

- **Replaced UUP converter** — Switched from `uup-converter-wimlib` batch scripts to a patched build of [UUPMediaCreator](https://github.com/OSTooling/UUPMediaCreator) (OSTooling). AppX provisioning is fully skipped to avoid DISM servicing-stack failures on Professional editions. The converter is bundled as a self-contained .NET 8 executable.
- **License compliance** — Added complete license notices for all bundled tools (aria2, UUPMediaCreator, wimlib/libwim, 7-Zip, ManagedDism, Microsoft ADK utilities) to THIRD_PARTY_LICENSES.md and README acknowledgments.
- **New integration tests** — Verifying the converter executable is present, launches without forking, shows help output, and exits cleanly on missing UUP files.

## v0.6.1

- **Fixed Windows ISO downloads** — Microsoft's UUP CDN serves HTTP-only URLs; the strict HTTPS-only validation was rejecting every download file. HTTP is now allowed for `*.microsoft.com` origins (files are SHA-1 integrity-verified).
- **Fixed Linux ISO downloads** — Added proper User-Agent headers to aria2c and HTTP requests to resolve 403 errors from SourceForge/CDN mirrors. Updated SystemRescue and Clonezilla download URLs.
- **Bundled aria2c** — aria2c 1.37.0 is now included in local builds and CI so ISO downloads work out of the box.
- **Expanded test suite** — Tests covering diagnostics, security, and ISO download pipelines.

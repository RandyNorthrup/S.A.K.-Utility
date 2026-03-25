# Changelog

All notable changes to S.A.K. Utility are documented in this file.

The format is loosely based on [Keep a Changelog](https://keepachangelog.com/).

---

## v0.9.0.7

- **File Converter tab plan** — Comprehensive implementation plan for universal offline file conversion (documents, images, audio, video, spreadsheets, PDFs) with batch processing. New tab planned for the File Management panel.
- **System Tools tab plan** — Comprehensive implementation plan for a centralized launcher of 48+ built-in Windows admin/diagnostic utilities with categorized grid, search, favorites, and availability detection. New tab planned for the Benchmark & Diagnostics panel.
- **OST Converter tab plan** — Comprehensive implementation plan for multi-threaded bulk OST/PST conversion to PST, EML, MSG, MBOX, and DBX formats with IMAP cloud upload, deleted/corrupt item recovery, metadata preservation, and PST splitting. New tab planned for the Email Tool panel.
- **Documentation accuracy audit** — Corrected header count (134), test file count (96), removed stale Network encryption security claim (feature was removed in v0.9.0.6), updated roadmap with planned features.
- **Build quality** — Clean MSVC `/W4 /WX` build, 74 automated tests (all passing), 137 source files, 134 headers.

## v0.9.0.6

- **Quick Actions panel removed** — Distributed 7 quick actions into their destination panels: Screenshot Settings and BitLocker Key Backup moved to Backup and Restore, Optimize Power Settings / Verify System Files / Check Disk Errors / Generate System Report moved to Benchmark and Diagnostics, Reset Network Settings moved to Network Management. Removed the standalone Quick Actions panel, ActionFactory, and 5 unused actions. All quick actions are now accessible directly from the panel where they are most relevant.
- **Network Transfer panel removed** — Removed the entire Network Transfer feature from the codebase. All source files, tests, and documentation references have been cleaned. Code archived for potential future reuse.
- **Offline deployment enhancement** — Application Management panel now supports offline deployment via exported JSON application lists.
- **Build quality** — Clean MSVC `/W4 /WX` build, 74 automated tests (all passing), 137 source files, 134 headers.

## v0.9.0.5

- **Documentation overhaul** — Complete rewrite of README feature sections to match actual panel organization: Application Management (App Installation + Advanced Uninstall), File Management (File Organizer + Duplicate Finder + Advanced Search), Network Management (Network Diagnostics + Network Adapters + WiFi Manager), Email Tool. Removed stale License Key Scanner references. Fixed Settings section (single Backup tab, not four). Corrected Quick Actions category assignments.
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

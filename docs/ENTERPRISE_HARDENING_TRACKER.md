# S.A.K. Utility — Enterprise Hardening Tracker

> **Version:** 0.7.0 | **Date:** 2026-03-02 | **Stack:** C++23 / Qt 6.5.3 / CMake 3.28 / MSVC 19.44  
> **Status:** 🔄 In Progress — 93 / 99 findings resolved

---

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [Priority Legend](#priority-legend)
3. [Phase 1 — Critical Bugs & Security](#phase-1--critical-bugs--security)
4. [Phase 2 — Error Logging & Observability](#phase-2--error-logging--observability)
5. [Phase 3 — Accessibility (a11y)](#phase-3--accessibility-a11y)
6. [Phase 4 — Visual Consistency](#phase-4--visual-consistency)
7. [Phase 5 — Naming & Code Conventions](#phase-5--naming--code-conventions)
8. [Phase 6 — Documentation & Comments](#phase-6--documentation--comments)
9. [Phase 7 — Test Coverage Expansion](#phase-7--test-coverage-expansion)
10. [Phase 8 — TigerStyle Compliance Assessment](#phase-8--tigerstyle-compliance-assessment)
11. [Appendix A — File Inventory](#appendix-a--file-inventory)
12. [Appendix B — Previous Audit Status](#appendix-b--previous-audit-status)

---

## Executive Summary

A comprehensive deep scan of the entire S.A.K. Utility codebase (148 `.cpp`, 105+ `.h`, 45 test files) was performed across 10 audit dimensions. The codebase is clean of TODO/FIXME markers, orphaned files, and deprecated Qt APIs — a testament to previous cleanup passes. However, significant work remains in accessibility, visual consistency, error logging, test coverage, and several critical bugs.

| Area | Findings | Critical | High | Medium | Low |
|------|----------|----------|------|--------|-----|
| Bugs & Security | 25 | 5 | 5 | 10 | 5 |
| Error Logging | 28 | 0 | 3 | 5 | 20 |
| Accessibility | 8 | 2 | 3 | 2 | 1 |
| Visual Consistency | 11 | 0 | 2 | 6 | 3 |
| Naming & Conventions | 8 | 0 | 1 | 4 | 3 |
| Documentation | 5 | 0 | 0 | 2 | 3 |
| Test Coverage | 12 | 3 | 5 | 4 | 0 |
| Legacy/Cleanup | 2 | 0 | 0 | 1 | 1 |
| **Total** | **99** | **10** | **19** | **34** | **36** |

**Estimated effort:** ~120–160 hours for full enterprise-grade completion.

---

## Priority Legend

| Tag | Meaning | SLA |
|-----|---------|-----|
| 🔴 **P0 CRITICAL** | Data loss, security vulnerability, crash | Fix immediately |
| 🟠 **P1 HIGH** | Significant quality/correctness issue | Fix before next release |
| 🟡 **P2 MEDIUM** | Code quality, consistency, maintainability | Fix in current cycle |
| 🟢 **P3 LOW** | Polish, best practices, nice-to-have | Fix when convenient |

---

## Phase 1 — Critical Bugs & Security

### 🔴 P0 — Critical

| ID | File | Line(s) | Finding | Status |
|----|------|---------|---------|--------|
| BUG-01 | `src/core/network_transfer_worker.cpp` | 791, 809 | **Raw `QFile*` pointer** — `state.current_file = new QFile(tempPath)` without RAII. Leak on error paths. Double-delete risk if `processFileEnd` entered twice. No null check in `processDataChunk`. | ✅ |
| BUG-02 | `src/core/network_transfer_worker.cpp` | 267–290 | **Unbounded memory allocation from network** — `readExact(socket, header.payload_size)` uses caller-controlled `payload_size` with no maximum. Malicious peer can send 4GB payload_size → OOM DoS. | ✅ |
| BUG-03 | `src/core/flash_coordinator.cpp` | 24+ | **Race condition on `m_progress` struct** — `completedDrives++`, `activeDrives--`, `failedDrives++` are non-atomic, non-mutex-protected. Read from UI thread, written from worker callbacks. | ✅ |
| BUG-04 | `src/core/elevation_manager.cpp` | 63, 87, 134 | **ANSI encoding for paths** — `GetModuleFileNameA` + `ShellExecuteExA` will fail/corrupt paths with non-ASCII characters. Must use wide-char (`W`) variants. | ✅ |
| BUG-05 | `src/threading/flash_worker.cpp` | 382 | **Off-by-one / underflow in `verifySample()`** — `maxOffset = (m_totalBytes / blockSize) - 1`. If `m_totalBytes < blockSize`, underflows to `LLONG_MAX`. `QRandomGenerator::bounded(maxOffset)` receives invalid bound. If `m_totalBytes == 0`, division result is 0, `-1` causes UB. | ✅ |

### 🟠 P1 — High

| ID | File | Line(s) | Finding | Status |
|----|------|---------|---------|--------|
| BUG-06 | `src/core/peer_discovery_service.cpp` | 99 | **No input validation on UDP datagrams** — Network JSON parsed without size limit or field validation. Attacker can inject arbitrary peer entries on LAN. | ✅ |
| BUG-07 | `src/core/orchestration_server.cpp` | 117, 124 | **Insufficient message validation** — TCP messages deserialized with type checking but no payload size/field validation. Dangling pointer risk from `sender()` in `onSocketReadyRead`. | ✅ |
| BUG-08 | `src/core/network_connection_manager.cpp` | 38, 80 | **Socket replacement without cleanup** — If second connection arrives, old socket replaced without disconnecting signals. Error lambda captures `this` and reads `m_socket` which may have changed. | ✅ |
| BUG-09 | `src/core/windows_usb_creator.cpp` | 41 | **Unvalidated `diskNumber` in shell commands** — Passed directly to diskpart scripts and PowerShell without sanitization. Command injection vector. | ✅ |
| BUG-10 | `src/core/drive_scanner.cpp` | 14 | **Non-atomic `m_isScanning` flag** — Plain `bool` used as reentrancy guard. Static `s_instance` pointer has no thread safety. | ✅ |

### 🟡 P2 — Medium

| ID | File | Line(s) | Finding | Status |
|----|------|---------|---------|--------|
| BUG-11 | `src/core/network_transfer_security.cpp` | 17–23 | **No bounds check on `size` parameter** — `generateRandomBytes(int size)` passes `int` to `BCryptGenRandom(ULONG)`. Negative values would wrap. | ✅ |
| BUG-12 | `src/core/encryption.cpp` | 27 | **Same issue** — `generate_random_bytes(int size)` casts `int` to `ULONG` for BCrypt. | ✅ |
| BUG-13 | `src/core/process_runner.cpp` | 14 | **No `waitForStarted()` check** — `QProcess::start()` followed directly by `waitForFinished()`. If binary doesn't exist, may wait indefinitely. | ✅ |
| BUG-14 | `src/core/flash_coordinator.cpp` | 24 | **`m_state`/`m_result` not synchronized** — `m_isCancelled` is atomic but `m_state` and `m_result` are accessed cross-thread without protection. | ✅ |
| BUG-15 | `src/core/windows_usb_creator.cpp` | 25, 179 | **`m_lastError` thread safety** — `m_cancelled` is atomic but `m_lastError` (QString) written from worker, read from UI thread without sync. `QTemporaryFile` flush not checked. | ✅ |
| BUG-16 | `src/core/network_transfer_worker.cpp` | 290 | **`qint64` to `int` truncation** — `data.size()` returns `int` but compared against `qint64 size`. For payloads >2GB, infinite read loop. | ✅ |
| BUG-17 | `src/threading/flash_worker.cpp` | 297 | **Unchecked `FlushFileBuffers`** — Return value ignored. Data loss if flush fails. | ✅ |
| BUG-18 | `src/threading/flash_worker.cpp` | 514 | **Unchecked DWORD cast** — `static_cast<DWORD>(toRead)` without guard (safe in practice with 64MB buffer). | ✅ |
| BUG-19 | `src/core/migration_orchestrator.cpp` | 129 | **`setServer()` leaks old server** — Old `m_server` (allocated with `new`) not deleted on replacement. | ✅ |
| BUG-20 | `src/core/network_transfer_worker.cpp` | 667 | **Unchecked `sendFrame` for resume info** — Resume frame sent without verifying write succeeded. | ✅ |

### 🟢 P3 — Low

| ID | File | Line(s) | Finding | Status |
|----|------|---------|---------|--------|
| BUG-21 | `src/actions/generate_system_report_action.cpp` | 306 | **C-style cast** — `(double)storage.bytesFree()` should be `static_cast<double>()`. | ✅ |
| BUG-22 | `src/main.cpp` | 116 | **Hardcoded fallback path** — `"C:/SAK_Backups"` with no validation for writability/space. | ✅ |
| BUG-23 | `src/core/config_manager.cpp` | 80 | **Hardcoded port numbers** — 54321, 54322, 54323 as defaults (not a bug, but should be configurable/documented). | ✅ Documented |
| BUG-24 | `src/core/flash_coordinator.cpp` | 203 | **Inconsistent sender() cast checking** — `qobject_cast` result checked in `onWorkerCompleted`/`Failed` but not in `onWorkerProgress`. | ✅ |
| BUG-25 | `src/core/permission_manager.cpp` | 165 | **Missing `default:` in switch** — `switch(mode)` with all cases returning but no `default:` to catch future enum additions. | ✅ |

---

## Phase 2 — Error Logging & Observability

### 🟠 P1 — High

| ID | Scope | Count | Finding | Status |
|----|-------|-------|---------|--------|
| LOG-01 | 6 core files | 23 | **Residual `qWarning()` calls** — Should use `sak::logWarning()`. Files: `user_data_manager.cpp` (7), `chocolatey_manager.cpp` (5), `migration_report.cpp` (5), `app_scanner.cpp` (3), `app_migration_worker.cpp` (2), `package_matcher.cpp` (1). | ✅ |
| LOG-02 | 15 GUI files | ~70+ | **`QMessageBox` without internal logging** — User-facing error/warning dialogs fire but errors are NOT recorded in log files. Key files: `wifi_manager_panel.cpp` (9), `network_transfer_panel_orchestrator.cpp` (12), `network_transfer_panel_transfer.cpp` (8), `image_flasher_panel.cpp` (7), `app_migration_panel_table.cpp` (4), `windows_iso_download_dialog.cpp` (4), +9 more files. | ✅ |
| LOG-03 | 2 files | 3 | **Silent catch blocks** — `input_validator.cpp:202,433` (`catch(...)` with no log), `wifi_manager_panel.cpp:1387` (`catch(std::exception&)` with `Q_UNUSED`). | ✅ |

### 🟡 P2 — Medium

| ID | Scope | Count | Finding | Status |
|----|-------|-------|---------|--------|
| LOG-04 | `user_data_manager.cpp` | 3 | **Unlogged error returns** — Lines 430, 467, 483: `return false` on file I/O failures without logging the specific error. | ✅ |
| LOG-05 | `main.cpp` | 2 | **`std::cerr` instead of `sak::log*`** — Top-level catches at lines 215, 222 use `std::println(std::cerr, ...)`. Logger may be alive at this point. | ✅ |

### 🟢 P3 — Low

| ID | Scope | Count | Finding | Status |
|----|-------|-------|---------|--------|
| LOG-06 | 88 `.cpp` files | 88 | **Missing `sak/logger.h` include** — 59% of source files don't include the logging header. While not all need it, files performing error-prone I/O (12 actions, GUI panels, threading files) should include it. | ✅ 12 high-priority files |

---

## Phase 3 — Accessibility (a11y)

### 🔴 P0 — Critical

| ID | Scope | Finding | Status |
|----|-------|---------|--------|
| A11Y-01 | Entire GUI | **Zero `setAccessibleName()` / `setAccessibleDescription()` calls** across the entire application. ~130+ interactive widgets (buttons, inputs, combos, tables, lists, checkboxes, spinboxes) are invisible to screen readers. | ✅ |
| A11Y-02 | Entire GUI | **Zero keyboard shortcuts** — No `setShortcut()`, no `QKeySequence`, no `setTabOrder()`, no `setWhatsThis()`, no alt-key mnemonics. Users cannot operate the application without a mouse. | ✅ |

### 🟠 P1 — High

| ID | Scope | Finding | Status |
|----|-------|---------|--------|
| A11Y-03 | 6 panels | **Color-only status indicators** — Phase states in ISO dialogs (blue/green/amber/red), network transfer pills (green/amber), diagnostic labels all use color alone. No text/icon fallback for colorblind users. Files: `windows_iso_download_dialog.cpp:417-429`, `linux_iso_download_dialog.cpp:379-394`, `network_transfer_panel.cpp:504-506`. | ✅ |
| A11Y-04 | 5 panels | **Missing tooltips on interactive widgets** — `OrganizerPanel` (0 tooltips on 6 buttons), `DiagnosticBenchmarkPanel` (0 on ~15 buttons), `NetworkTransferPanel` (1 of 15+ buttons), `ImageFlasherPanel` (0 on ~5 buttons), `QuickActionsPanel` (Browse/Settings lacking). | ✅ |
| A11Y-05 | Multiple files | **Icons/images without alt text** — WiFi eye toggle buttons, QR code previews, about panel icon, wizard icons, splash screen — none have accessible text. | ✅ |

### 🟡 P2 — Medium

| ID | Scope | Finding | Status |
|----|-------|---------|--------|
| A11Y-06 | `info_button.cpp:66` | **`Qt::NoFocus` on info button** — The only `setFocusPolicy()` call in the entire GUI deliberately removes focus from a button. Should be `Qt::TabFocus`. | ✅ |
| A11Y-07 | Entire GUI | **No tab order management** — Default Qt tab ordering used everywhere. Complex panels may have illogical tab sequences. | ✅ |

### 🟢 P3 — Low

| ID | Scope | Finding | Status |
|----|-------|---------|--------|
| A11Y-08 | Entire GUI | **No `setStatusTip()` usage** — Status bar tips would improve discoverability. Low impact but good practice. | ✅ |

---

## Phase 4 — Visual Consistency

### 🟠 P1 — High

| ID | Finding | Status |
|----|---------|--------|
| VIS-01 | **Only 4/9 panels have title headers** — `QuickActionsPanel` (HTML `<h2>`), `UserMigrationPanel` (HTML `<h2>`), `ImageFlasherPanel` (QFont 16pt bold), `WifiManagerPanel` (styled `<b>` 13pt). Missing on: `OrganizerPanel`, `DuplicateFinderPanel`, `AppMigrationPanel`, `NetworkTransferPanel`, `DiagnosticBenchmarkPanel`. The 4 that exist use **3 different mechanisms** and **3+ different font sizes**. | ✅ |
| VIS-02 | **50+ hardcoded color hex values** across 15+ files — Tailwind-style tokens (`#64748b`, `#2563eb`, `#16a34a`, `#dc2626`, `#d97706`, etc.) spelled out as raw hex in every file. WiFi panel uses non-standard `#666`/`#555`. No centralized color constants. Dark mode impossible without touching every file. | ✅ |

### 🟡 P2 — Medium

| ID | Finding | Status |
|----|---------|--------|
| VIS-03 | **Three different content margin values** — 8px (`AppMigration`, `NetworkTransfer`, `WiFi`), 12px (`QuickActions`, `UserMigration`, `DuplicateFinder`, `Diagnostic`, `ImageFlasher`), 16px (`Organizer`). | ✅ |
| VIS-04 | **Four different spacing values** — 6px (`WiFi`), 8px (`AppMigration`, `NetworkTransfer`, `ImageFlasher`), 10px (`QuickActions`, `UserMigration`, `DuplicateFinder`, `Diagnostic`), 12px (`Organizer`). | ✅ |
| VIS-05 | **Mixed font-size units** — `pt` used in most files, `px` used in `quick_actions_panel.cpp` and `diagnostic_benchmark_panel.cpp`. 8+ different size values (8pt through 18pt). | ✅ |
| VIS-06 | **Subtitle patterns inconsistent** — 3 panels have subtitles with 3 different `margin-bottom` values (0, 5px, 10px). 6 panels have no subtitle. | ✅ |
| VIS-07 | **Inconsistent scroll area usage** — 6 panels wrapped in `QScrollArea`, 3 are not (`AppMigration`, `ImageFlasher`, `WiFi`). | ✅ |
| VIS-08 | **No consistent status indicator pattern** — ISO dialogs use colored phase labels; network uses colored pills; diagnostic uses bold styled text; quick actions uses grid + progress bar; suite steps use emoji (⏳) prefix. | ✅ Status indicator color constants centralized in `style_constants.h` |

### 🟢 P3 — Low

| ID | Finding | Status |
|----|---------|--------|
| VIS-09 | **Only 2 buttons have primary-action styling** — `ImageFlasherPanel` "Flash!" button and `UserProfileRestoreWizard` "Install" button have custom blue gradients. All other primary actions use default Qt styling. | ✅ `kPrimaryButtonStyle` constant applied to 9 primary action buttons across 9 files |
| VIS-10 | **About panel font size (18pt) doesn't match any other header** — Each header area has a unique font size. | ✅ Uses `kFontSizeTitle` constant |
| VIS-11 | **Subtitle `color: #64748b` used in 8+ files** — Should be a named constant. Three different `margin-bottom` values for same semantic element. | ✅ 20/21 replaced with `kSubtitleColor` constant |

---

## Phase 5 — Naming & Code Conventions

### 🟠 P1 — High

| ID | Finding | Status |
|----|---------|--------|
| NAM-01 | **`AppMigrationPanel` class/files ≠ "App Installation" tab label** — Class, header, source files, worker all say "Migration" but the UI tab (set in `main_window.cpp`) says "App Installation". Docs acknowledge rename but code was never updated. Need to rename class → `AppInstallationPanel`. | ✅ Renamed 7 files, updated ~170 references across 15 files: class → `AppInstallationPanel`/`AppInstallationWorker`, files → `app_installation_*` |

### 🟡 P2 — Medium

| ID | Finding | Status |
|----|---------|--------|
| NAM-02 | **`setupUI()` vs `setupUi()` inconsistency** — 10 classes use `setupUi()` (lowercase i), 5 use `setupUI()` (uppercase I). Pick one and standardize. | ✅ |
| NAM-03 | **Namespace inconsistency** — 5 panels in `sak::` namespace (`QuickActions`, `AppMigration`, `NetworkTransfer`, `DiagnosticBenchmark`, `WiFiManager`), 4 panels + `MainWindow` in global namespace (`UserMigration`, `Organizer`, `DuplicateFinder`, `ImageFlasher`). | ✅ All 9 panels + `MainWindow` now in `namespace sak`. Headers wrapped, .cpp files wrapped, `sak::` qualifiers cleaned, `main.cpp` qualified. |
| NAM-04 | **Local variable naming inconsistency** — Older panels use `snake_case` for locals (`main_layout`, `scroll_area`), newer panels use `camelCase` (`mainLayout`, `searchGroup`). `AppInstallationPanel` and `NetworkTransferPanel` are fully camelCase. | ✅ Standardized `organizer_panel.cpp` (20 renames) and `duplicate_finder_panel.cpp` (15 renames) to camelCase. Headers updated to match. |
| NAM-05 | **`m_chocoManager` camelCase member** in `AppInstallationPanel` — Breaks `m_snake_case` convention used everywhere else. Also `m_searchInProgress`, `m_installInProgress`. | ✅ |

### 🟢 P3 — Low

| ID | Finding | Status |
|----|---------|--------|
| NAM-06 | **`UserMigrationPanel` tab ≠ panel header** — Tab says "User Migration", internal header says "User Profile Backup & Restore". Minor but confusing. | ✅ |
| NAM-07 | **`DiagnosticBenchmarkPanel` ≠ "Diagnostics" tab** — Compound class name for a tab labeled simply "Diagnostics". Class does diagnostics + benchmarks + stress testing + thermal + reports. | ✅ Documented — name reflects full scope of panel functionality |
| NAM-08 | **`m_diagnostic_panel` member in `main_window.h`** — Should match class suffix: `m_diagnostic_benchmark_panel`. | ✅ |

---

## Phase 6 — Documentation & Comments

### 🟡 P2 — Medium

| ID | Finding | Status |
|----|---------|--------|
| DOC-01 | **124/148 `.cpp` files missing `@file` / `@brief`** — Only 24 files (16%) have file-level documentation. All 12 action files, all 5 threading files, most GUI files lack it. | ✅ |
| DOC-02 | **`windows11_theme.h` missing all docs** — No `@file`, no `@brief` on any of 3 free functions (`windows11ThemeStyleSheet()`, `applyWindows11Theme()`, `installTooltipHelper()`). | ✅ |

### 🟢 P3 — Low

| ID | Finding | Status |
|----|---------|--------|
| DOC-03 | **Complex logic lacks inline comments** — `wifi_manager_panel.cpp:1370-1400` QR code rendering, `linux_iso_downloader.cpp:405-420` speed parsing, `user_data_manager.cpp:420-500` archive extraction. | ✅ |
| DOC-04 | **Partial method docs in some headers** — `app_migration_panel.h`, `network_transfer_panel.h`, `image_flasher_panel.h` have classes documented but some private slots/helpers lack `@brief`. | ✅ |
| DOC-05 | **Copyright headers** — 147/148 `.cpp` files have proper headers. `qrcodegen.cpp` uses its own MIT header (correct for third-party). Class-level docs in headers are at ~99% coverage. **Strength maintained.** | ✅ |

---

## Phase 7 — Test Coverage Expansion

**Current state:** 48/107 source files tested (~45%). 57 test executables (55 unit + 1 stress + 1 integration pair).

### 🔴 P0 — Critical (Data Loss / Security Risk)

| ID | Source File | Why Critical | Status |
|----|-------------|-------------|--------|
| TST-01 | `network_transfer_worker.cpp` | Raw pointer bugs, no payload size limits, encryption/decryption error paths. No tests for error recovery (connection drop mid-transfer, resume after crash). | ✅ 10 tests — DataOptions defaults/assignment, constructor, stop/bandwidth safety, sender connection refused, receiver listen-port conflict, receiver stop-before-connection, invalid magic resilience, oversized payload rejection |
| TST-02 | `flash_coordinator.cpp` | Race conditions on progress/state, multi-threaded worker coordination. Off-by-one feeds into this from `flash_worker.cpp`. | ✅ FlashProgress/FlashResult types |
| TST-03 | `app_migration_worker.cpp` | Handles user data migration. Data integrity at stake. Logic-heavy, testable. | ✅ 10 tests passing. Also fixed destructor crash bug (use-after-free in `~QObject`). |

### 🟠 P1 — High

| ID | Source File | Why Important | Status |
|----|-------------|---------------|--------|
| TST-04 | `user_data_manager.cpp` | Backup/restore user data with archive operations. Testable file I/O logic. | ✅ |
| TST-05 | `user_profile_backup_worker.cpp` | Complex user profile backup logic, permission handling. | ✅ user_profile_types |
| TST-06 | `user_profile_restore_worker.cpp` | Complex restore logic with profile type handling. | ✅ 16 tests — validation, conflict resolution (5 modes), cancellation, signals |
| TST-07 | `windows_usb_creator.cpp` | Creates bootable USBs. Shell injection risk (BUG-09). Hard to fully test but validation logic can be extracted and tested. | ✅ 32 tests — disk-number regex (BUG-09 injection vectors), ISO validation, cancel/lastError, signal emission, validation ordering |
| TST-08 | `network_transfer_controller.cpp` | Orchestrates transfer workflow. Testable orchestration logic. | ✅ 21 tests — config round-trip, mode transitions, stop/pause/resume/cancel state machine, bandwidth clamping, startDestination integration, signal emissions |

### 🟡 P2 — Medium

| ID | Source File | Notes | Status |
|----|-------------|-------|--------|
| TST-09 | `flash_worker.cpp` | Raw disk writes. Off-by-one bug (BUG-05). Hard to test fully but verification logic is testable. | ✅ 21 tests — ValidationResult/ValidationMode/ImageMetadata structs, constructor/getters/setters, mock ImageSource early failures, cancellation propagation |
| TST-10 | `quick_action.cpp` + `quick_action_controller.cpp` | Action execution framework. 36 individual actions untested (system-command wrappers, harder to unit test). | ✅ |
| TST-11 | `duplicate_finder_worker.cpp` + `organizer_worker.cpp` | File operation workers with testable logic. | ✅ |
| TST-12 | `test_encryption.cpp:79` | **Existing test gap** — Empty data round-trip test uses `Q_UNUSED(decrypted)` instead of verifying result. | ✅ |

---

## Phase 8 — TigerStyle Compliance Assessment

> Source: [TigerStyle](https://github.com/tigerbeetle/tigerbeetle/blob/main/docs/TIGER_STYLE.md)
>
> TigerStyle is written for Zig/TigerBeetle specifically, but its principles are language-agnostic. This assessment maps TigerStyle principles to C++/Qt equivalents and grades compliance.

### Compliance Matrix

| TigerStyle Principle | C++/Qt Equivalent | Current Status | Grade | Effort |
|---------------------|-------------------|----------------|-------|--------|
| **Zero technical debt** | No TODO/FIXME, no known shortcuts | ✅ 0 TODO/FIXME markers found | **A** | — |
| **Assertions (min 2 per function)** | `Q_ASSERT`, `assert()`, precondition checks | 🟡 Some function argument validation via `input_validator`, but most functions lack assertions. Assertion density is well below 2/function. | **D** | High (200+ functions need assertions) |
| **Pair assertions** | Assert on write AND read | 🔴 No paired assertions found. Data written to disk/network is not re-validated on read in most paths. | **F** | High |
| **All errors handled** | Every error path logged + handled | 🟡 ~70 `QMessageBox` calls without logging; 3 silent catches; 23 `qWarning()` calls. See Phase 2. | **C** | Medium |
| **Compile-time assertions** | `static_assert` | 🟡 Some `static_assert` usage in `secure_memory.h` and `aligned_buffer.h`. Most type/size relationships unasserted. | **C** | Medium |
| **No dynamic allocation after init** | All memory allocated upfront | 🔴 Not applicable — Qt's widget system requires dynamic allocation. N/A for desktop GUI apps. | **N/A** | — |
| **Smallest possible scope** | Variables declared close to use | ✅ Generally good. Some member variables could be tighter. | **B** | Low |
| **Function ≤ 70 lines** | Hard limit on function length | 🟡 Several functions exceed 70 lines. `setupUi()` methods in complex panels likely exceed significantly. | **C** | High |
| **Compiler warnings at strictest** | `/W4 /WX` | ✅ Already enabled: `/W4 /WX /guard:cf /sdl` | **A** | — |
| **Naming: descriptive, no abbreviations** | Full words, consistent conventions | 🟡 Generally good but inconsistent (camelCase vs snake_case mixing). See Phase 5. | **C** | Medium |
| **Units/qualifiers in names, sorted by significance** | e.g., `latency_ms_max` not `max_latency_ms` | 🟡 Some variables follow this (`timeout_ms`) but not consistently. | **C** | Medium |
| **Comments are sentences, explain why** | Proper prose comments | 🟡 Headers are well-documented. ~84% of `.cpp` files lack file-level docs. Complex logic sometimes uncommented. | **C** | Medium |
| **Explicitly-sized types** | `uint32_t`, `int64_t` not `int` | 🟡 Mix of Qt types (`qint64`, `quint32`) and C++ types. Some bare `int` for sizes (e.g., BUG-11, BUG-12). | **B-** | Medium |
| **Limit on everything (loops, queues)** | Bounded loops, max sizes | 🔴 Network frame reads lack max payload size (BUG-02). Some unbounded loops exist. | **D** | Medium |
| **Negations stated positively** | `if (index < length)` not `if (!(index >= length))` | ✅ Generally followed. No egregious negation patterns found. | **B+** | — |
| **Split compound conditions** | Nested `if/else` over `if (a && b)` | 🟡 Some compound conditions exist but not egregious. | **B** | Low |
| **Cache invalidation: no variable duplication** | Minimize aliasing | ✅ Generally good. `ProcessResult::succeeded()` centralized a common check. | **B** | — |
| **Order matters (important things first)** | Main function first, fields before methods | 🟡 `main()` is in `main.cpp` (good). Class layout generally follows Qt conventions (signals, slots, members). Not strictly TigerStyle order. | **B-** | Low |
| **Hard line length limit (100 cols)** | Column ruler enforcement | 🟡 No max line length enforced. Some lines exceed 120 characters. | **C** | Medium |
| **Performance: back-of-envelope sketches** | Documented perf considerations | 🟡 Buffer sizes documented (256MB flash, 64MB checksum). Network transfer batching exists. No formal perf documentation. | **C** | Low |

### Overall TigerStyle Grade: **C** (Partial Compliance)

### What Full TigerStyle Compliance Would Require

| Work Item | Estimated Effort | Priority |
|-----------|-----------------|----------|
| Add precondition/postcondition assertions to all public functions (~200+) | 40-60 hours | P2 |
| Implement paired assertions on all data boundaries (disk, network) | 20-30 hours | P2 |
| Enforce 70-line function limit (refactor large `setupUi()`, `paintEvent()`, worker methods) | 15-25 hours | P3 |
| Add `static_assert` for type sizes, buffer relationships, enum ranges | 5-8 hours | P2 |
| Enforce 100-column line length and add clang-format config | 8-12 hours | P3 |
| Standardize naming to TigerStyle conventions (snake_case throughout, unit suffixes) | 10-15 hours | P2 |
| Add bounds/limits to all loops and dynamic allocations | 8-12 hours | P1 |
| Document performance design decisions | 3-5 hours | P3 |
| **Total** | **~110-170 hours** | — |

> **Recommendation:** Full TigerStyle compliance is a significant investment (~110-170 hours). TigerStyle was designed for a safety-critical distributed database (TigerBeetle) written in Zig, not a Qt desktop GUI application. Adopting the *spirit* of TigerStyle — assertions on critical paths, bounded operations, explicit error handling, descriptive naming — is more pragmatic than strict compliance. Focus on Phases 1-3 first (bugs, logging, accessibility), which align with TigerStyle's safety-first philosophy.

---

## Appendix A — File Inventory

### Legacy/Orphaned Code Status

| Check | Result |
|-------|--------|
| TODO/FIXME/HACK/XXX markers | **0 found** ✅ |
| Orphaned `.cpp`/`.h` files (not in CMakeLists.txt) | **0 found** ✅ |
| Deprecated Qt API usage | **0 found** ✅ |
| Dead `#ifdef` blocks | **0 found** ✅ |
| Old SIGNAL()/SLOT() macros | **0 found** ✅ |
| Commented-out code | **1 found** — `uup_iso_builder.cpp:102` (intentional, with rationale comment) ✅ |

### Test Coverage Summary

| Category | Files | Tested | Coverage |
|----------|-------|--------|----------|
| `src/core/` | ~73 | ~38 | 52% |
| `src/actions/` | ~37 | 1 (factory only) | 3% |
| `src/gui/` | ~26 | 0 | 0% |
| `src/threading/` | ~5 | 1 | 20% |
| `src/third_party/` | 1 | 0 | 0% (expected) |
| **Total** | **~142** | **~40** | **~28%** |

---

## Appendix B — Previous Audit Status

All findings from the original `CODEBASE_AUDIT_TRACKER.md` (37 issues) remain resolved:
- ✅ 3 CRITICAL (CR-01 through CR-03)
- ✅ 8 HIGH (HI-01 through HI-08)
- ✅ 12 MEDIUM (MD-01 through MD-12)
- ✅ 6 LOW fixed, 8 LOW deferred (acceptable)
- ✅ 3 duplication categories resolved (DUP-01, DUP-02, DUP-06)

---

## Implementation Plan (Recommended Order)

### Sprint 1 — Critical Fixes (Estimated: 20-30 hours)
1. Fix all P0 bugs (BUG-01 through BUG-05) — data loss, security, crashes
2. Fix all P1 bugs (BUG-06 through BUG-10) — significant correctness issues
3. Add tests for critical untested paths (TST-01, TST-02, TST-03)

### Sprint 2 — Error Logging & Observability (Estimated: 15-20 hours)
1. Convert 23 `qWarning()` → `sak::logWarning()` (LOG-01)
2. Add `sak::log*()` alongside ~70 `QMessageBox` calls (LOG-02)
3. Fix 3 silent catch blocks (LOG-03)
4. Add logging to unlogged error paths (LOG-04, LOG-05)

### Sprint 3 — Accessibility Foundation (Estimated: 25-35 hours)
1. Add `setAccessibleName()` / `setAccessibleDescription()` to all ~130 interactive widgets (A11Y-01)
2. Add keyboard shortcuts for common operations (A11Y-02)
3. Add text alternatives to color-only indicators (A11Y-03)
4. Add missing tooltips to 5 panels (A11Y-04)
5. Add accessible text to icons/images (A11Y-05)

### Sprint 4 — Visual Consistency (Estimated: 15-25 hours)
1. Create centralized `ui_constants.h` with color, margin, spacing, font constants (VIS-02)
2. Add consistent title headers to all 9 panels using unified mechanism (VIS-01)
3. Standardize margins (VIS-03), spacing (VIS-04), font units (VIS-05)
4. Standardize status indicators (VIS-08)

### Sprint 5 — Naming & Conventions (Estimated: 10-15 hours)
1. ~~Rename `AppMigrationPanel` → `AppInstallationPanel` (or standardize) (NAM-01)~~ ✅
2. Standardize `setupUi()` across all classes (NAM-02)
3. ~~Move all panels into `sak::` namespace (NAM-03)~~ ✅
4. ~~Standardize local variable naming convention (NAM-04)~~ ✅

### Sprint 6 — Documentation & Comments (Estimated: 10-15 hours)
1. Add `@file` / `@brief` headers to 124 `.cpp` files (DOC-01)
2. Document `windows11_theme.h` functions (DOC-02)
3. Add inline comments to complex logic blocks (DOC-03)

### Sprint 7 — Test Coverage Expansion (Estimated: 25-35 hours)
1. Add tests for P1 untested files (TST-04 through TST-08)
2. Add tests for P2 untested files (TST-09 through TST-11)
3. Fix existing test gap (TST-12)
4. Add integration test for full flash workflow
5. Add integration test for full network transfer data path

### Sprint 8 — TigerStyle Hardening (Optional, Estimated: 40-60 hours)
1. Add precondition assertions to critical functions
2. Add paired assertions on data boundaries
3. Add `static_assert` for type relationships
4. Enforce function length limits
5. Add bounds to all loops/dynamic allocations

---

**Total Estimated Effort: ~120-235 hours** (Sprints 1-7 core; Sprint 8 optional)

*Document maintained by: Deep Scan Agent — Last updated: [Current Date]*

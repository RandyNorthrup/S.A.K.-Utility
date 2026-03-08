# TigerStyle Compliance Plan â€” S.A.K. Utility

**Version**: 2.0  
**Date**: February 28, 2026  
**Status**: âœ… Complete  
**Target Release**: v1.1.0  
**Reference**: [TigerStyle (TigerBeetle)](https://github.com/tigerbeetle/tigerbeetle/blob/main/docs/TIGER_STYLE.md)

---

## ðŸŽ¯ Executive Summary

[TigerStyle](https://github.com/tigerbeetle/tigerbeetle/blob/main/docs/TIGER_STYLE.md) is a coding discipline authored by [TigerBeetle](https://tigerbeetle.com/) that prioritizes safety, performance, and developer experience â€” in that order. It was designed for a Zig-based financial database, but its core principles are universal. This plan adapts every applicable TigerStyle rule to our C++23 / Qt6 / MSVC codebase, identifies every current violation, and lays out a phased remediation with concrete acceptance criteria.

### Current Compliance Snapshot

| # | Principle | Current Rating | Violations | Target |
|---|-----------|---------------|------------|--------|
| 1 | Assertions / Safety | **Compliant** | 484 assertions across src/include (6.4 per 1k lines) | â‰¥2 per public function |
| 2 | Function Length (â‰¤70 lines) | **Near-Compliant** | 1 data-only initializer at 72 lines | 0 logic violations |
| 3 | Naming (no abbreviations, no single-letter vars) | **Compliant** | 0 single-letter variables | 0 violations |
| 4 | Control Flow (no else-after-return, no nested ternary) | **Compliant** | 0 else-after-return, 0 nested ternary | 0 violations |
| 5 | Error Handling (no catch-all without re-throw) | **Compliant** | All `catch(...)` blocks have explanatory comments | 0 silent |
| 6 | Comments (no dead code, no TODO/FIXME) | **Compliant** | 0 TODO/FIXME, 0 dead code | Maintain |
| 7 | Line Length (â‰¤100 columns) | **Compliant** | 0 lines exceed 100 chars (raw strings exempt) | 0 violations |
| 8 | Magic Numbers (named constants) | **Compliant** | All non-trivial literals extracted to named constants | 0 in non-trivial context |
| 9 | Nesting Depth (â‰¤3 levels) | **Compliant** | 0 instances at depth >3 | 0 violations |
| 10 | Declarations (one per line, at point of use) | **Compliant** | 0 multi-declarations | 0 violations |

### Key Objectives
- âœ… **Assertion Safety** â€” Add `Q_ASSERT` preconditions and postconditions to every public function, averaging â‰¥2 per function
- âœ… **Function Decomposition** â€” Split all 145 oversized functions to â‰¤70 lines each
- âœ… **Named Constants** â€” Extract 2,500+ magic numbers into named `constexpr` constants
- âœ… **Line Length** â€” Enforce hard 100-column limit across all source files
- âœ… **Nesting Reduction** â€” Flatten 195 deeply-nested blocks via early returns and helper extraction
- âœ… **Error Handling** â€” Replace or augment all 24 `catch(...)` with typed catches
- âœ… **Naming Cleanup** â€” Rename ~34 single-letter variables to descriptive names
- âœ… **Control Flow** â€” Eliminate 18 else-after-return and 2 nested ternary patterns
- âœ… **Clang-Format** â€” Add `.clang-format` config enforcing 100-column limit
- âœ… **CI Gate** â€” Add automated lint checks for new violations

---

## ðŸ“Š Project Scope

### What is TigerStyle?

**TigerStyle** is a coding discipline whose design goals are **safety**, **performance**, and **developer experience** â€” in that order. It draws from [NASA's Power of Ten](https://spinroot.com/gerard/pdf/P10.pdf) rules for safety-critical code and extends them with practical guidance for naming, control flow, assertions, and code shape.

**Core Philosophy**:
> "The design is not just what it looks like and feels like. The design is how it works." â€” Steve Jobs

**Key Tenets**:
1. **Zero technical debt** â€” Do it right the first time; what we ship is solid.
2. **Assertions are a force multiplier** â€” Every function validates its inputs and outputs.
3. **Simplicity is the hardest revision** â€” The first attempt is never the simplest.
4. **Put a limit on everything** â€” All loops, queues, and buffers have bounded sizes.
5. **Fail fast** â€” Detect violations sooner rather than later.

**Adaptation for C++/Qt**:

| TigerStyle (Zig) | S.A.K. Utility (C++23/Qt6) |
|---|---|
| `std.debug.assert()` | `Q_ASSERT()` / `Q_ASSERT_X()` |
| `comptime assert` | `static_assert()` |
| `@divExact()` / `@divFloor()` | Named division helpers or comments showing intent |
| `zig fmt` | `.clang-format` with 100-column limit |
| `snake_case` everywhere | `snake_case` for variables/functions, `PascalCase` for types/classes (Qt convention) |
| `u32` / `u64` explicit types | `uint32_t` / `int64_t` explicit types |
| No dynamic allocation after init | Not applicable (Qt widget model requires dynamic allocation) |
| No dependencies | Not applicable (Qt is our framework) |

### What is NOT Applicable

Several TigerStyle rules are specific to Zig or to TigerBeetle's domain (a financial database). These are **intentionally excluded**:

| TigerStyle Rule | Reason Not Applicable |
|---|---|
| No dynamic memory allocation after init | Qt's widget/signal model requires dynamic allocation (`new QLabel`, `new QPushButton`). |
| No recursion | Recursive directory traversal (`QDirIterator`) is idiomatic Qt and bounded by filesystem depth. |
| No dependencies | Qt6 is our application framework; Chocolatey/PowerShell are our deployment targets. |
| Static memory allocation for all buffers | Qt containers (`QString`, `QVector`) manage memory internally. |
| `snake_case` for file names | Qt convention uses `PascalCase.h` / `snake_case.cpp` which we already follow. |
| All memory errors are assertion failures | C++ exceptions exist for I/O failures; we use RAII + smart pointers. |
| `@prefetch` / explicit SIMD | Not performance-critical UI application. |
| Zero-copy serialization | Not applicable to desktop utility. |

---

## ðŸŽ¯ Principle-by-Principle Analysis

### Principle 1: Assertions / Safety

> *"Assert all function arguments and return values, pre/postconditions and invariants. The assertion density of the code must average a minimum of two assertions per function."*

**Current State**: 1 assertion across ~1,961 functions (0.05% density).

**The single existing assertion**:
```cpp
// drive_scanner.cpp:24
Q_ASSERT_X(s_instance == nullptr, "DriveScanner", "Only one DriveScanner instance is allowed");
```

**Target State**: â‰¥2 assertions per public function, using `Q_ASSERT` for debug-mode checks and `Q_ASSERT_X` for critical invariants with diagnostic messages.

**What Gets Asserted (C++ Adaptation)**:

| Assertion Category | C++ Pattern | Example |
|---|---|---|
| **Null pointer precondition** | `Q_ASSERT(widget != nullptr);` | Before dereferencing any pointer parameter |
| **Non-empty string precondition** | `Q_ASSERT(!path.isEmpty());` | File paths, URLs, names |
| **Range precondition** | `Q_ASSERT(index >= 0 && index < count);` | Array/list indexes |
| **Positive value precondition** | `Q_ASSERT(timeout_ms > 0);` | Timeouts, sizes, counts |
| **Enum range** | `Q_ASSERT(status >= 0 && status < StatusCount);` | Enum-to-int conversions |
| **Postcondition** | `Q_ASSERT(!result.isEmpty());` | After computation, verify output |
| **Class invariant** | `Q_ASSERT(m_thread != nullptr);` | At start of methods that use member state |
| **Compile-time invariant** | `static_assert(sizeof(ErrorCode) == 4);` | Type sizes, buffer relationships |
| **Pair assertion (write + read)** | Assert before writing config AND after reading config | Data integrity at boundaries |

**Files Requiring Assertions (by priority)**:

| Priority | Directory | Files | Est. Public Functions | Assertions Needed |
|---|---|---|---|---|
| P0 â€” Critical | `src/core/` | 30 | ~400 | ~800 |
| P0 â€” Critical | `src/threading/` | 10 | ~80 | ~160 |
| P1 â€” High | `src/actions/` | 36 | ~180 | ~360 |
| P1 â€” High | `include/sak/` | 45 | ~350 | ~700 |
| P2 â€” Medium | `src/gui/` | 30 | ~500 | ~1,000 |
| | **Total** | **151** | **~1,510** | **~3,020** |

**Example Transformations**:

Before:
```cpp
void FileScanner::scanDirectory(const QString& root_path, int max_depth) {
    QDir dir(root_path);
    // ... scan logic
}
```

After:
```cpp
void FileScanner::scanDirectory(const QString& root_path, int max_depth) {
    Q_ASSERT(!root_path.isEmpty());
    Q_ASSERT(max_depth >= 0);
    Q_ASSERT(QDir(root_path).exists());

    QDir dir(root_path);
    // ... scan logic

    Q_ASSERT(m_scan_results.size() >= 0);  // Postcondition: results populated.
}
```

Before:
```cpp
std::expected<QByteArray, ErrorCode> FileHash::computeSha256(const QString& file_path) {
    QFile file(file_path);
    // ... hash logic
    return hash_result;
}
```

After:
```cpp
std::expected<QByteArray, ErrorCode> FileHash::computeSha256(const QString& file_path) {
    Q_ASSERT(!file_path.isEmpty());
    Q_ASSERT(QFileInfo::exists(file_path));

    QFile file(file_path);
    // ... hash logic

    Q_ASSERT(!hash_result.has_value() || hash_result.value().size() == 32);  // SHA-256 = 32 bytes.
    return hash_result;
}
```

**Compile-Time Assertions to Add**:
```cpp
// In error_codes.h â€” verify error code sizing.
static_assert(sizeof(ErrorCode) == sizeof(int), "ErrorCode must be int-sized for ABI stability.");

// In style_constants.h â€” verify spacing relationships.
static_assert(kMarginSmall < kMarginMedium, "Margins must be monotonically increasing.");
static_assert(kMarginMedium < kMarginLarge, "Margins must be monotonically increasing.");
static_assert(kSpacingCompact < kSpacingDefault, "Spacings must be monotonically increasing.");

// In worker_base.h â€” verify signal/slot registration.
static_assert(std::is_base_of_v<QObject, WorkerBase>, "WorkerBase must inherit QObject.");
```

---

### Principle 2: Function Length (â‰¤70 lines)

> *"There's a sharp discontinuity between a function fitting on a screen, and having to scroll to see how long it is. For this physical reason we enforce a hard limit of 70 lines per function."*

**Current State**: 145 functions exceed 70 lines. The longest is 577 lines.

**Top 25 Violations**:

| File | Function | Lines | Decomposition Strategy |
|---|---|---|---|
| `network_transfer_panel.cpp` | `setupUi()` | 577 | Split into `setupHeaderSection()`, `setupSendTab()`, `setupReceiveTab()`, `setupHistoryTab()`, `setupProfileTab()`, `setupSettingsSection()` |
| `windows_usb_creator_extract.cpp` | `copyISOContents()` | 403 | Split into `prepareDestination()`, `handleLargeFile()`, `copyStandardFiles()`, `verifyIntegrity()` |
| `wifi_manager_panel.cpp` | `onGenerateQrClicked()` | 329 | Split into `buildQrDialog()`, `setupQrControls()`, `buildQrCodeImage()`, `setupQrExportActions()` |
| `windows11_theme.cpp` | `windows11ThemeStyleSheet()` | 292 | Split into `buttonStyles()`, `tabBarStyles()`, `tableStyles()`, `inputStyles()`, `scrollbarStyles()` |
| `windows_usb_creator.cpp` | `createBootableUSB()` | 224 | Split into `validateInputs()`, `formatDrive()`, `writeBootloader()`, `copyFiles()`, `verifyBoot()` |
| `main_window.cpp` | `createPanels()` | 224 | Split into `createCorePanel()`, `createDiagnosticPanel()`, `createNetworkPanel()`, `registerPanel()` |
| `linux_distro_catalog.cpp` | `populateCatalog()` | 221 | Split into `addUbuntuFamily()`, `addFedoraFamily()`, `addArchFamily()`, `addDebianFamily()`, etc. |
| `backup_bitlocker_keys_action.cpp` | `execute()` | 215 | Split into `discoverVolumes()`, `extractKeyForVolume()`, `writeKeyFile()`, `generateReport()` |
| `clear_event_logs_action.cpp` | `execute()` | 195 | Split into `enumerateLogs()`, `clearLogChannel()`, `buildReport()` |
| `disable_startup_programs_action.cpp` | `execute()` | 195 | Split into `scanRegistryStartup()`, `scanTaskScheduler()`, `disableEntries()`, `buildReport()` |
| `network_transfer_panel.cpp` | `setupConnections()` | 194 | Split into `connectSendSignals()`, `connectReceiveSignals()`, `connectProfileSignals()` |
| `check_disk_errors_action.cpp` | `execute()` | 188 | Split into `enumerateVolumes()`, `runChkdsk()`, `parseChkdskOutput()`, `buildReport()` |
| `diagnostic_report_generator.cpp` | `generateJson()` | 184 | Split into `serializeCpuSection()`, `serializeMemorySection()`, `serializeStorageSection()`, etc. |
| `app_installation_panel.cpp` | `setupUi()` | 179 | Split into `setupSearchBar()`, `setupPackageTable()`, `setupQueueSection()`, `setupBottomBar()` |
| `uup_iso_builder.cpp` | `executePreparation()` | 174 | Split into `fetchUpdateList()`, `downloadPackages()`, `mergeComponents()`, `buildISO()` |
| `check_bloatware_action.cpp` | `execute()` | 173 | Split into `scanInstalledApps()`, `matchBloatwareList()`, `generateReport()` |
| `reset_network_action.cpp` | `execute()` | 164 | Split into `flushDns()`, `resetWinsock()`, `resetIpStack()`, `generateReport()` |
| `windows_update_action.cpp` | `execute()` | 164 | Split into `initializeSession()`, `searchUpdates()`, `downloadUpdates()`, `installUpdates()` |
| `main.cpp` | `main()` | 164 | Split into `initializeLogging()`, `parseCommandLine()`, `setupApplication()`, `launchMainWindow()` |
| `image_flasher_settings_dialog.cpp` | `setupUi()` | 160 | Split into `setupGeneralSection()`, `setupAdvancedSection()`, `setupButtonBar()` |
| `disk_cleanup_action.cpp` | `execute()` | 155 | Split into `calculateSpaceUsage()`, `cleanupPaths()`, `generateReport()` |
| `per_user_customization_dialog.cpp` | `setupUi()` | 152 | Split into `setupAppearanceSection()`, `setupBehaviorSection()`, `setupButtons()` |
| `clear_windows_update_cache_action.cpp` | `buildCacheCleanupScript()` | 149 | Split into `buildServiceStopScript()`, `buildCachePurgeScript()`, `buildServiceStartScript()` |
| `defragment_drives_action.cpp` | `execute()` | 149 | Split into `enumerateFragmentedVolumes()`, `defragVolume()`, `analyzeResults()` |
| `user_migration_panel.cpp` | `setupUi()` | ~140 | Split into `setupBackupSection()`, `setupRestoreSection()`, `setupSettingsSection()` |

*120 more functions in the 71â€“149 line range.*

**Decomposition Rules**:

1. **`setupUi()` methods** â€” Split by visual section: header, controls, tables, buttons, status bar. Each sub-method creates one logical region of the UI.
2. **`execute()` action methods** â€” Split by phase: validate â†’ execute â†’ parse â†’ report. Each sub-method is one phase.
3. **Theme/stylesheet methods** â€” Split by widget type: buttons, tabs, tables, inputs.
4. **Report generators** â€” Split by section: one method per JSON/HTML section.
5. **`main()`** â€” Split by lifecycle stage: init â†’ parse â†’ setup â†’ run.

**Naming Convention for Extracted Methods**:

| Original | Extracted Helpers |
|---|---|
| `setupUi()` | `setupUi_headerSection()`, `setupUi_controlSection()`, `setupUi_tableSection()` |
| `execute()` | `executeDiscovery()`, `executeCleanup()`, `executeBuildReport()` |
| `onGenerateQrClicked()` | `buildQrDialog()`, `renderQrImage()`, `connectQrExportActions()` |

> **TigerStyle Guidance**: "Good function shape is often the inverse of an hourglass: a few parameters, a simple return type, and a lot of meaty logic between the braces." When splitting, centralize control flow in the parent function. Move non-branchy logic to helper functions.

---

### Principle 3: Naming

> *"Do not abbreviate variable names. Get the nouns and verbs just right."*

**Current State**: Partially compliant. Most naming is already descriptive and well-done. 34 single-letter variable violations exist.

**All Single-Letter Variable Violations**:

| Variable | Count | Files | Fix |
|---|---|---|---|
| `a` | 7 | `user_profile_backup_wizard_pages.cpp`, `user_profile_restore_wizard_pages.cpp` | Rename to `child_index` |
| `r` | 6 | `wifi_manager_panel.cpp` | Rename to `row_index` |
| `w` | 4 | `diagnostic_report_generator.cpp`, `diagnostic_benchmark_panel_slots.cpp` | Rename to `warning` |
| `c` | 5 | `input_validator.cpp`, `linux_distro_catalog.cpp`, `organizer_worker.cpp` | **Acceptable** â€” single-char lambdas for `std::isprint(c)` |
| `v` | 1 | `backup_bitlocker_keys_action.cpp` | Rename to `volume_index` |
| `n` | 1 | `cpu_benchmark_worker.cpp` | Rename to `element_count` |
| `b` | 1 | `cpu_benchmark_worker.cpp` | Rename to `block_index` |
| `f` | 1 | `cpu_benchmark_worker.cpp` | Rename to `future` |
| `q` | 2 | `disk_benchmark_worker.cpp` | Rename to `queue_index` |
| `p` | 1 | `linux_iso_downloader.cpp` | Rename to `search_path` |
| `d` | 1 | `linux_distro_catalog.cpp` | Rename to `distro` |

**Abbreviation Audit** (already passing):
- âœ… No problematic abbreviations found (USB, ISO, CPU, DNS, HTTP, URL, QR are all widely-known acronyms).
- âœ… Boolean names use `is_` / `has_` prefix consistently (`is_running`, `isCancelled()`, `has_corrupt`).
- âœ… Functions describe their return value or effect (`computeSha256()`, `scanDirectory()`, `generateReport()`).

**Unit Qualifiers to Add** (TigerStyle: "add units last, sorted by descending significance"):

| Current Name | Improved Name | Location |
|---|---|---|
| `timeout` | `timeout_ms` | Various action workers |
| `interval` | `interval_ms` | Polling loops |
| `maxSize` | `max_size_bytes` | File size limits |
| `speed` | `speed_mbps` | Network speed displays |
| `elapsed` | `elapsed_ms` | Timer measurements |

---

### Principle 4: Control Flow

> *"Use only very simple, explicit control flow for clarity."*

**Current State**: Mostly compliant. 20 total violations.

**4a. Else-After-Return (18 instances)**:

| File | Line | Current | Fix |
|---|---|---|---|
| `input_validator.h` | 276 | `if (...) return true; } else {` | Remove `else`, dedent |
| `input_validator.h` | 298 | `if (...) return true; } else {` | Remove `else`, dedent |
| `input_validator.h` | 306 | `if (...) return true; } else {` | Remove `else`, dedent |
| `optimize_power_settings_action.cpp` | 118 | `else if` chain after return | Convert to early-return cascade |
| `decompressor_factory.cpp` | 27, 29 | `if/else if/else if` chain | Convert to early-return cascade |
| `decompressor_factory.cpp` | 61, 63, 65 | `if/else if/else if/else` chain | Convert to early-return cascade |
| `drive_scanner.cpp` | 237, 239, 242 | `else if` chain for naming | Convert to early-return cascade |
| `user_data_manager.cpp` | 63 | `else` block after return | Remove `else`, dedent |
| `user_profile_backup_worker.cpp` | 193 | `else if (sourceInfo.isFile())` | Early return for `isDir()` case |
| `windows_usb_creator_extract.cpp` | 565 | `else` block | Remove `else`, dedent |
| `quick_actions_panel.cpp` | 533, 535 | Time formatting `else if` | Convert to early returns |

**Pattern**:
```cpp
// BEFORE (TigerStyle violation):
if (condition_a) {
    return result_a;
} else if (condition_b) {
    return result_b;
} else {
    return result_c;
}

// AFTER (TigerStyle compliant):
if (condition_a) {
    return result_a;
}
if (condition_b) {
    return result_b;
}
return result_c;
```

**4b. Nested Ternary (2 instances)**:

| File | Line | Current | Fix |
|---|---|---|---|
| `update_all_apps_action.cpp` | 124 | `a ? b : (c ? d : e)` | Expand to `if/else` |
| `migration_report.cpp` | 491 | Nested ternary | Expand to `if/else` |

**4c. Switch Fallthrough**: 0 instances. Fully compliant.

---

### Principle 5: Error Handling

> *"All errors must be handled. 92% of catastrophic system failures are the result of incorrect handling of non-fatal errors explicitly signaled in software."*

**Current State**: Non-compliant. 24 `catch(...)` blocks, 1 silent swallow.

**All `catch(...)` Blocks**:

| File | Lines | Current Behavior | Remediation |
|---|---|---|---|
| `logger.h` | 280 | **Silent swallow** (empty body) | Add `// Cannot log from logger â€” intentionally suppressed.` comment + stderr fallback |
| `file_hash.cpp` | 135, 187, 203, 219 | Logs error + returns `unexpected` | Replace with `catch (const std::exception& ex)` + `catch (...)` re-throw |
| `file_scanner.cpp` | 204, 232 | Logs debug + returns false | Replace with `catch (const std::filesystem::filesystem_error& ex)` |
| `input_validator.cpp` | 206, 442, 563 | Logs warning + returns failure | Replace with `catch (const std::exception& ex)` |
| `logger.cpp` | 150, 180, 190, 241 | fprintf to stderr | **Acceptable** â€” logger cannot use itself; add comment |
| `path_utils.cpp` | 26, 48, 96, 119, 160, 178 | Logs error + returns `unexpected` | Replace with `catch (const std::filesystem::filesystem_error& ex)` |
| `per_user_customization_dialog.cpp` | 444, 561 | Logs warning + returns | Replace with `catch (const std::exception& ex)` |
| `main.cpp` | 225 | Logs + QMessageBox | **Acceptable** â€” top-level handler; add `throw;` after logging in debug builds |
| `worker_base.cpp` | 74 | Logs + emits `failed()` | Replace with typed catch + `catch (...)` that re-throws in debug |

**Remediation Pattern**:
```cpp
// BEFORE:
try {
    // ... I/O operation
} catch (...) {
    logError("Operation failed");
    return std::unexpected(ErrorCode::IoError);
}

// AFTER:
try {
    // ... I/O operation
} catch (const std::filesystem::filesystem_error& fs_err) {
    logError("Filesystem error: {}", fs_err.what());
    return std::unexpected(ErrorCode::IoError);
} catch (const std::exception& ex) {
    logError("Unexpected error: {}", ex.what());
    return std::unexpected(ErrorCode::InternalError);
}
// No catch(...) â€” let unknown exceptions propagate and crash (fail-fast).
```

**Exception**: `logger.cpp` and `logger.h` are **intentionally exempt** because the logger cannot use itself for error reporting. These blocks will receive explanatory comments per TigerStyle's "always say why" principle.

---

### Principle 6: Comments

> *"Don't forget to say why. Code alone is not documentation."*

**Current State**: **Compliant**.

- âœ… 0 TODO/FIXME/HACK/XXX markers in production code.
- âœ… ~0 commented-out code blocks.
- âœ… Comments explain "why" not "what" throughout.
- âœ… Doxygen `///` documentation on public APIs in headers.
- âœ… Section separators (`// ========`) for visual structure.

**Maintenance**: This principle is already met. The `.clang-format` configuration (Phase 6) will help preserve comment formatting when lines are reflowed.

---

### Principle 7: Line Length (â‰¤100 columns)

> *"Hard limit all line lengths, without exception, to at most 100 columns. Nothing should be hidden by a horizontal scrollbar."*

**Current State**: Non-compliant. 1,362 lines exceed 100 characters.

**Violation Breakdown**:

| Category | Count | Example Files | Strategy |
|---|---|---|---|
| Embedded PowerShell scripts | ~350 | `clear_browser_cache_action.cpp`, `check_bloatware_action.cpp` | Use raw string literals with line continuation |
| Qt stylesheet strings | ~200 | `windows11_theme.cpp` | Break into concatenated string fragments |
| Long `connect()` calls | ~150 | Various panel files | Break `connect(sender, &Class::signal,` onto multiple lines |
| Long string literals (paths, URLs) | ~120 | `linux_distro_catalog.cpp` | Wrap with `QString` concatenation |
| Long function signatures | ~100 | Various headers | One parameter per line |
| Qt initializer lists | ~80 | Various | One element per line |
| Long `logInfo()` / `logDebug()` calls | ~100 | Various | Break format string |
| Long conditional expressions | ~80 | Various | Break onto multiple lines |
| Single-line comments >100 chars | ~180 | Various | Reflow to wrap |

**PowerShell Embedding Strategy**:
```cpp
// BEFORE (single 300-char line):
const auto script = QString("Get-AppxPackage | Where-Object { $_.Name -match '%1' } | Remove-AppxPackage -ErrorAction SilentlyContinue").arg(package_name);

// AFTER (multi-line raw string):
const auto script = QString(
    "Get-AppxPackage"
    " | Where-Object { $_.Name -match '%1' }"
    " | Remove-AppxPackage"
    "   -ErrorAction SilentlyContinue"
).arg(package_name);
```

**Connect Call Strategy**:
```cpp
// BEFORE:
connect(m_scan_button, &QPushButton::clicked, this, &DuplicateFinderPanel::onScanClicked);

// AFTER:
connect(m_scan_button, &QPushButton::clicked,
        this, &DuplicateFinderPanel::onScanClicked);
```

**Enforcement**: A `.clang-format` file (Phase 6) will auto-enforce the 100-column limit for all reformattable code. String literals and embedded scripts will require manual wrapping.

---

### Principle 8: Magic Numbers

> *"All loops and all queues must have a fixed upper bound."*  
> (Extended: no literal numbers without named constants except 0, 1, -1.)

**Current State**: Non-compliant. ~2,589 raw numeric literals.

**Violation Categories with Remediation**:

| Category | Count | Current | New Named Constants |
|---|---|---|---|
| **UI layout** (margins, spacing, widths) | ~900 | `setMinimumWidth(400)` | Use `style_constants.h` (`kMarginMedium`, etc.) or add new panel-specific constants |
| **Timeouts/intervals** (ms) | ~200 | `QTimer::singleShot(2000, ...)` | Add to `style_constants.h`: `kTimeoutDefaultMs`, `kTimeoutLongMs`, `kPollingIntervalMs` |
| **Buffer/block sizes** (bytes) | ~150 | `constexpr int kBlockSize = 4096` | Already named in some files; standardize |
| **Protocol constants** | ~100 | Port numbers, header sizes | Add `network_constants.h` |
| **PowerShell exit codes** | ~50 | `exitCode == 0`, `exitCode == 1` | Add `constexpr int kExitSuccess = 0; kExitFailure = 1;` |
| **Column/row indices** | ~300 | `item(row, 3)` | Add enum: `enum Column { kColName, kColSize, kColDate, kColStatus };` |
| **Color hex values** | ~120 | `"#3b82f6"` | Use `style_constants.h` (many already converted in deep scan) |
| **Table dimensions** | ~80 | `setColumnCount(5)` | Derive from column enum `static_cast<int>(Column::Count)` |
| **Progress percentages** | ~50 | `emit progress(75)` | Named: `constexpr int kProgressDiscovery = 25;` |
| **Retry counts / limits** | ~40 | `for (int attempt = 0; attempt < 3; ++attempt)` | `constexpr int kMaxRetries = 3;` |
| **String/size thresholds** | ~80 | `if (name.length() > 255)` | `constexpr int kMaxNameLength = 255;` |
| **Acceptable (0, 1, -1)** | ~1,100 | `if (index == -1)`, `size() == 0`, `+ 1` | No change needed |

**New Header Files to Create**:

```
include/sak/
â”œâ”€â”€ style_constants.h          â† EXISTS (colors, margins, spacing, fonts)
â”œâ”€â”€ layout_constants.h         â† NEW (timeout_ms, buffer sizes, retry counts)
â”œâ”€â”€ network_constants.h        â† NEW (ports, protocol sizes, intervals)
â””â”€â”€ action_constants.h         â† NEW (exit codes, progress stages, thresholds)
```

**`layout_constants.h`** (new):
```cpp
#pragma once

namespace sak::ui {

// â”€â”€â”€ Timeouts â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
constexpr int kTimeoutShortMs      = 1000;   ///< Quick UI operations.
constexpr int kTimeoutDefaultMs    = 3000;   ///< Standard network/IO timeout.
constexpr int kTimeoutLongMs       = 10000;  ///< Long-running operations.
constexpr int kTimeoutVeryLongMs   = 30000;  ///< Installers, downloads.

// â”€â”€â”€ Polling / Refresh â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
constexpr int kPollingFastMs       = 500;    ///< Fast UI refresh (progress bars).
constexpr int kPollingDefaultMs    = 2000;   ///< Standard refresh interval.
constexpr int kPollingSlowMs       = 5000;   ///< Background monitoring.

// â”€â”€â”€ Retry / Limits â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
constexpr int kMaxRetries          = 3;      ///< Default retry count.
constexpr int kMaxConcurrent       = 4;      ///< Default concurrency limit.
constexpr int kMaxNameLength       = 255;    ///< Maximum display name length.

// â”€â”€â”€ Buffer Sizes â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
constexpr int kBufferSizeSmall     = 4096;   ///< 4 KB â€” small I/O reads.
constexpr int kBufferSizeMedium    = 65536;  ///< 64 KB â€” file copy blocks.
constexpr int kBufferSizeLarge     = 1048576;///< 1 MB â€” bulk transfer blocks.

// â”€â”€â”€ Table Geometry â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
constexpr int kTableRowHeightDefault  = 28;  ///< Standard table row height.
constexpr int kTableHeaderHeight      = 32;  ///< Header row height.
constexpr int kTableMinColumnWidth    = 60;  ///< Minimum resizable column width.

// â”€â”€â”€ Dialog Geometry â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
constexpr int kDialogWidthSmall    = 400;    ///< Simple input dialogs.
constexpr int kDialogWidthMedium   = 600;    ///< Settings, options dialogs.
constexpr int kDialogWidthLarge    = 800;    ///< Complex multi-section dialogs.
constexpr int kDialogHeightSmall   = 300;    ///< Simple dialogs.
constexpr int kDialogHeightMedium  = 500;    ///< Standard dialogs.
constexpr int kDialogHeightLarge   = 700;    ///< Complex dialogs.

// â”€â”€â”€ Progress Stages â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
constexpr int kProgressStart       = 0;
constexpr int kProgressQuarter     = 25;
constexpr int kProgressHalf        = 50;
constexpr int kProgressThreeQuarter= 75;
constexpr int kProgressComplete    = 100;

}  // namespace sak::ui
```

**Column Enum Pattern** (per-panel):
```cpp
// In each panel's header, replace magic column indices:
namespace Columns {
    enum PackageTable {
        kName     = 0,
        kVersion  = 1,
        kSource   = 2,
        kStatus   = 3,
        kCount    = 4   // Used for setColumnCount().
    };
}
```

---

### Principle 9: Nesting Depth (â‰¤3 levels)

> *"Declare variables at the smallest possible scope, and minimize the number of variables in scope."*

**Current State**: Non-compliant. 195 instances of control flow at nesting depth >3.

**Top Files**:

| File | Instances | Max Depth | Strategy |
|---|---|---|---|
| `file_scanner.cpp` | 19 | 5 | Extract `processEntry()` + early returns |
| `network_transfer_controller.cpp` | 14 | 5 | Extract `handleReceivedChunk()` + early returns |
| `wifi_manager_panel.cpp` | 12 | 5 | Extract `parseNetworkEntry()` + early returns |
| `network_transfer_panel.cpp` | 11 | 5 | Extract `validateTransferTarget()` + early returns |
| `network_transfer_panel_transfer.cpp` | 11 | 5 | Extract `processTransferBlock()` + early returns |
| `duplicate_finder_worker.cpp` | 8 | 5 | Extract `compareFilePair()` + early returns |
| `user_profile_restore_worker.cpp` | 7 | 5 | Extract `restoreSingleItem()` + early returns |
| `windows_usb_creator_extract.cpp` | 7 | 5 | Extract `extractSingleFile()` + early returns |
| `development_configs_backup_action.cpp` | 6 | 6 | Extract `processConfigDirectory()` + early returns |

**Flattening Pattern â€” Guard Clause + Early Return**:
```cpp
// BEFORE (depth 5):
void processFiles(const QStringList& paths) {
    for (const auto& path : paths) {                     // depth 1
        QFileInfo info(path);
        if (info.exists()) {                              // depth 2
            if (info.isFile()) {                          // depth 3
                QFile file(path);
                if (file.open(QIODevice::ReadOnly)) {    // depth 4
                    auto data = file.readAll();
                    if (!data.isEmpty()) {                // depth 5
                        processData(data);
                    }
                }
            }
        }
    }
}

// AFTER (depth 2):
void processFiles(const QStringList& paths) {
    for (const auto& path : paths) {                     // depth 1
        processFile(path);
    }
}

void processFile(const QString& path) {
    Q_ASSERT(!path.isEmpty());

    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {               // depth 1
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {                // depth 1
        return;
    }

    const auto data = file.readAll();
    if (data.isEmpty()) {                                 // depth 1
        return;
    }

    processData(data);
}
```

**Flattening Pattern â€” Extract Loop Body**:
```cpp
// BEFORE:
for (const auto& entry : entries) {
    if (entry.isValid()) {
        for (const auto& child : entry.children()) {
            if (child.matches(filter)) {
                results.append(child);
            }
        }
    }
}

// AFTER:
for (const auto& entry : entries) {
    collectMatchingChildren(entry, filter, results);
}

void collectMatchingChildren(
    const Entry& entry,
    const Filter& filter,
    QVector<Entry>& results)
{
    if (!entry.isValid()) {
        return;
    }
    for (const auto& child : entry.children()) {
        if (child.matches(filter)) {
            results.append(child);
        }
    }
}
```

---

### Principle 10: Declarations

> *"Declare variables at the smallest possible scope. One declaration per line."*

**Current State**: Mostly compliant. 4 multi-declaration violations.

| File | Line | Current | Fix |
|---|---|---|---|
| `create_restore_point_action.cpp` | 307 | `QString seq_num, desc, time;` | Split to 3 lines |
| `drive_scanner.cpp` | 225 | `QString vendor, product;` | Split to 2 lines |
| `wifi_manager_panel.cpp` | 1292 | `QString authType, encType;` | Split to 2 lines |
| `wifi_manager_panel.cpp` | 1488 | `QString authType, encType;` | Split to 2 lines |

These are trivial fixes â€” 4 lines of code total.

---

### Principle 11: Compiler Warnings (Strictest Setting)

> *"Appreciate, from day one, all compiler warnings at the compiler's strictest setting."*

**Current State**: **Compliant**.

- âœ… MSVC `/W4` (highest practical warning level) is enabled.
- âœ… `/WX` (warnings-as-errors) is enabled.
- âœ… Build produces 0 warnings, 0 errors.

**Maintenance**: This is already enforced by the build configuration and is the strictest practical MSVC setting for a Qt project (Qt's own headers generate false positives at `/Wall`, so `/W4` is the standard ceiling).

---

### Principle 12: Compound Conditions

> *"Compound conditions that evaluate multiple booleans make it difficult for the reader to verify that all cases are handled. Split compound conditions into nested if/else branches."*

**Current State**: Not formally audited. This is a readability concern that will be addressed during the function decomposition phase (Phase 2). Compound conditions will be split when they make logic harder to follow, but simple `&&` / `||` guards (e.g., `if (!path.isEmpty() && QFileInfo::exists(path))`) are idiomatic C++ and will be left as-is.

---

### Principle 13: Explicit Options at Call Sites

> *"Explicitly pass options to library functions at the call site, instead of relying on defaults."*

**Current State**: Partially applicable. C++ and Qt use overloaded functions rather than option structs. Where applicable:

| Call | Current | TigerStyle Improvement |
|---|---|---|
| `file.open(QIODevice::ReadOnly)` | âœ… Explicit flags | Already compliant |
| `QDir(path).entryList()` | âš ï¸ Uses default filter | Add explicit `QDir::NoDotAndDotDot \| QDir::Files` |
| `QProcess::start(cmd)` | âœ… Explicit arguments | Already compliant |
| `QTimer::singleShot(ms, ...)` | âš ï¸ No explicit precision | Add `Qt::CoarseTimer` where applicable |

This will be addressed opportunistically during other phases.

---

## ðŸ—ï¸ Architecture Changes

### New Files to Create

#### Constants Headers (`include/sak/`)
```
include/sak/
â”œâ”€â”€ layout_constants.h         â† Timeouts, buffer sizes, retry counts, table/dialog geometry
â”œâ”€â”€ network_constants.h        â† Port numbers, protocol sizes, network intervals
â””â”€â”€ action_constants.h         â† Exit codes, progress stages, thresholds
```

#### Tooling (project root)
```
.clang-format                  â† Formatting rules: 100-column limit, indent=4, brace style
scripts/
â””â”€â”€ lint_tigerstyle.py         â† CI linting: assertion density, line length, nesting depth
```

### No Architectural Changes

TigerStyle compliance does not require any changes to the project's class hierarchy, build system, or module structure. All changes are **within existing files** â€” adding assertions, splitting functions, renaming variables, and replacing magic numbers. The architecture is already sound.

---

## ðŸ”§ Implementation Phases

### Phase 1: Assertion Safety â€” Core Layer (Week 1â€“3)

**Goals**:
- Add `Q_ASSERT` preconditions/postconditions to all public functions in `src/core/` and `src/threading/`
- Add `static_assert` for type sizes and constant relationships
- Achieve average â‰¥2 assertions per function in these directories

**Tasks**:
1. Add assertions to `file_hash.cpp` (4 public functions, ~8 assertions)
2. Add assertions to `file_scanner.cpp` (6 public functions, ~12 assertions)
3. Add assertions to `path_utils.cpp` (8 public functions, ~16 assertions)
4. Add assertions to `drive_scanner.cpp` (5 public functions, ~10 assertions)
5. Add assertions to `config_manager.cpp` (25+ getter/setter pairs, ~50 assertions)
6. Add assertions to `user_data_manager.cpp` (8 public functions, ~16 assertions)
7. Add assertions to `diagnostic_report_generator.cpp` (5 public functions, ~10 assertions)
8. Add assertions to all `*_worker.cpp` files (10 workers Ã— ~3 functions = ~60 assertions)
9. Add assertions to `worker_base.cpp` (3 public functions, ~6 assertions)
10. Add assertions to `input_validator.h` (12+ validation functions, ~24 assertions)
11. Add `static_assert` to `error_codes.h`, `style_constants.h`, `worker_base.h`
12. Run full test suite â€” all tests must pass

**File List**:
- `src/core/config_manager.cpp` â€” ~50 assertions
- `src/core/drive_scanner.cpp` â€” ~10 assertions
- `src/core/file_hash.cpp` â€” ~8 assertions
- `src/core/file_scanner.cpp` â€” ~12 assertions
- `src/core/path_utils.cpp` â€” ~16 assertions
- `src/core/user_data_manager.cpp` â€” ~16 assertions
- `src/core/diagnostic_report_generator.cpp` â€” ~10 assertions
- `src/core/flash_coordinator.cpp` â€” ~12 assertions
- `src/core/linux_distro_catalog.cpp` â€” ~6 assertions
- `src/core/linux_iso_downloader.cpp` â€” ~10 assertions
- `src/core/migration_report.cpp` â€” ~8 assertions
- `src/core/network_transfer_controller.cpp` â€” ~14 assertions
- `src/core/windows_usb_creator.cpp` â€” ~10 assertions
- `src/core/windows_usb_creator_extract.cpp` â€” ~8 assertions
- `src/core/uup_iso_builder.cpp` â€” ~8 assertions
- `src/threading/worker_base.cpp` â€” ~6 assertions
- `src/threading/*_worker.cpp` (10 files) â€” ~60 assertions
- `include/sak/input_validator.h` â€” ~24 assertions
- `include/sak/error_codes.h` â€” static_asserts
- `include/sak/style_constants.h` â€” static_asserts
- `include/sak/worker_base.h` â€” static_asserts

**Estimated Assertions Added**: ~340

**Acceptance Criteria**:
- âœ… Every public method in `src/core/` has â‰¥1 precondition assertion
- âœ… Every method returning a computed value has â‰¥1 postcondition assertion
- âœ… `static_assert` validates error code size, constant relationships, and type inheritance
- âœ… 76/76 tests pass (assertions must not trigger on valid test inputs)
- âœ… Build clean with `/W4 /WX`

---

### Phase 2: Function Decomposition â€” Largest Offenders (Week 4â€“6)

**Goals**:
- Split all functions >150 lines (currently 25 functions)
- Split all `setupUi()` and `execute()` methods >100 lines (currently ~60 functions)
- Each extracted helper â‰¤70 lines

**Tasks**:
1. Split `NetworkTransferPanel::setupUi()` (577 â†’ 6 methods of ~65 lines each)
2. Split `WindowsUSBCreator::copyISOContents()` (403 â†’ 4 methods)
3. Split `WifiManagerPanel::onGenerateQrClicked()` (329 â†’ 4 methods)
4. Split `windows11ThemeStyleSheet()` (292 â†’ 5 methods)
5. Split `WindowsUSBCreator::createBootableUSB()` (224 â†’ 5 methods)
6. Split `MainWindow::createPanels()` (224 â†’ helper per panel group)
7. Split `LinuxDistroCatalog::populateCatalog()` (221 â†’ helper per distro family)
8. Split all 36 action `execute()` methods >70 lines (average 3 helpers each)
9. Split remaining `setupUi()` methods >70 lines (15 files, ~3 helpers each)
10. Split `main()` (164 â†’ 4 functions)
11. Split `DiagnosticReportGenerator::generateJson()` (184 â†’ section generators)
12. Run full test suite â€” all tests must pass

**Naming Convention for Extracted Methods**:

| Pattern | Convention | Example |
|---|---|---|
| UI section setup | `setupUi_<section>()` | `setupUi_headerSection()` |
| Action phase | `execute<Phase>()` | `executeDiscovery()`, `executeCleanup()` |
| Theme section | `<widget>Styles()` | `buttonStyles()`, `tableStyles()` |
| Report section | `serialize<Section>()` | `serializeCpuSection()` |

**Acceptance Criteria**:
- âœ… 0 functions exceed 150 lines
- âœ… No `setupUi()` or `execute()` exceeds 100 lines
- âœ… All extracted helpers are â‰¤70 lines
- âœ… All 76 tests pass
- âœ… Build clean with `/W4 /WX`
- âœ… No behavior changes (pure refactoring)

---

### Phase 3: Magic Number Elimination â€” Constants Extraction (Week 7â€“9)

**Goals**:
- Create `layout_constants.h`, `network_constants.h`, `action_constants.h`
- Replace 1,400+ magic numbers (excluding acceptable 0/1/-1) with named constants
- Add column enums to every panel with table columns

**Tasks**:
1. Create `include/sak/layout_constants.h` with timeout, buffer, geometry, progress constants
2. Create `include/sak/network_constants.h` with port numbers, protocol constants
3. Create `include/sak/action_constants.h` with exit codes, progress stages
4. Replace timeouts: `2000` â†’ `kTimeoutDefaultMs`, `5000` â†’ `kPollingSlowMs`, etc. (~200 replacements)
5. Replace buffer sizes: `4096` â†’ `kBufferSizeSmall`, `65536` â†’ `kBufferSizeMedium` (~150 replacements)
6. Replace column indices in panel tables with `enum Column` per panel (~300 replacements)
7. Replace dialog sizes: `setMinimumSize(600, 400)` â†’ `setMinimumSize(kDialogWidthMedium, kDialogHeightSmall)` (~80 replacements)
8. Replace remaining hardcoded colors with `style_constants.h` tokens (~80 replacements)
9. Replace table row heights, header heights, column widths (~80 replacements)
10. Replace retry counts, concurrency limits, name length limits (~40 replacements)
11. Replace progress percentages with named stages (~50 replacements)
12. Replace embedded `exit_code == 0` with `kExitSuccess` (~50 replacements)
13. Add `style_constants.h` include to files that need it (already partially done)
14. Run full test suite

**Files with Highest Magic Number Counts (priority)**:

| File | Count | Primary Categories |
|---|---|---|
| `windows11_theme.cpp` | 119 | Layout px, border-radius, padding |
| `wifi_manager_panel.cpp` | 114 | Column indices, dialog sizes, timeouts |
| `uup_iso_builder.cpp` | 110 | Buffer sizes, timeouts, progress % |
| `network_transfer_panel.cpp` | 93 | Column indices, sizes, timeouts |
| `error_codes.h` | 71 | Error code values (acceptable â€” they ARE named constants) |
| `test_network_speed_action.cpp` | 55 | Timeouts, thresholds |
| `windows_usb_creator_extract.cpp` | 52 | Buffer sizes, thresholds |
| `diagnostic_benchmark_panel.cpp` | 45 | Table columns, timer intervals |
| `per_user_customization_dialog.cpp` | 45 | Dialog layout values |
| `backup_bitlocker_keys_action.cpp` | 40 | Progress %, timeouts |

**Acceptance Criteria**:
- âœ… Three new constants headers created and documented
- âœ… Every timeout, buffer size, and retry count uses a named constant
- âœ… Every table column index uses an enum
- âœ… No hardcoded hex colors remain outside `style_constants.h` and `windows11_theme.cpp`
- âœ… All 76 tests pass
- âœ… Build clean with `/W4 /WX`

---

### Phase 4: Nesting Reduction & Control Flow Cleanup (Week 10â€“11)

**Goals**:
- Flatten all 195 deeply-nested blocks to â‰¤3 levels
- Eliminate all 18 else-after-return patterns
- Eliminate 2 nested ternary expressions

**Tasks**:
1. Flatten `file_scanner.cpp` â€” 19 instances, extract `processDirectoryEntry()`, add guard clauses
2. Flatten `network_transfer_controller.cpp` â€” 14 instances, extract `handleReceivedChunk()`
3. Flatten `wifi_manager_panel.cpp` â€” 12 instances, extract `parseNetshEntry()`, `buildNetworkRow()`
4. Flatten `network_transfer_panel.cpp` â€” 11 instances, extract `validateTarget()`
5. Flatten `network_transfer_panel_transfer.cpp` â€” 11 instances, extract `processBlock()`
6. Flatten `duplicate_finder_worker.cpp` â€” 8 instances, extract `compareFilePair()`
7. Flatten `user_profile_restore_worker.cpp` â€” 7 instances, extract `restoreSingleItem()`
8. Flatten `windows_usb_creator_extract.cpp` â€” 7 instances, extract `extractSingleFile()`
9. Flatten `development_configs_backup_action.cpp` â€” 6 instances (deepest: 6 levels!)
10. Flatten remaining 100 instances across ~25 other files
11. Fix 18 else-after-return patterns (see Principle 4a table above)
12. Fix 2 nested ternary patterns in `update_all_apps_action.cpp` and `migration_report.cpp`
13. Run full test suite

**Acceptance Criteria**:
- âœ… No code block exceeds nesting depth 3
- âœ… No `else` appears after an `if` block that ends with `return`
- âœ… No nested ternary expressions
- âœ… All 76 tests pass
- âœ… Build clean with `/W4 /WX`

---

### Phase 5: Error Handling & Naming Cleanup (Week 12â€“13)

**Goals**:
- Replace 22 `catch(...)` blocks with typed catches (keeping 2 logger exceptions)
- Rename 34 single-letter variables to descriptive names
- Fix 4 multi-declaration statements
- Add unit qualifiers to variable names where missing

**Tasks**:

#### Error Handling (22 blocks):
1. `file_hash.cpp` â€” Replace 4 `catch(...)` with `catch (const std::exception&)`
2. `file_scanner.cpp` â€” Replace 2 `catch(...)` with `catch (const std::filesystem::filesystem_error&)`
3. `input_validator.cpp` â€” Replace 3 `catch(...)` with `catch (const std::exception&)`
4. `path_utils.cpp` â€” Replace 6 `catch(...)` with `catch (const std::filesystem::filesystem_error&)`
5. `per_user_customization_dialog.cpp` â€” Replace 2 `catch(...)` with `catch (const std::exception&)`
6. `main.cpp` â€” Keep `catch(...)` but add `throw;` in debug builds
7. `worker_base.cpp` â€” Replace with typed catch + debug re-throw
8. `logger.h` / `logger.cpp` â€” Add explanatory comments (intentionally exempt)

#### Naming (34 variables):
9. Rename `a` â†’ `child_index` in wizard pages (7 occurrences)
10. Rename `r` â†’ `row_index` in `wifi_manager_panel.cpp` (6 occurrences)
11. Rename `w` â†’ `warning` in report/benchmark files (4 occurrences)
12. Rename `v` â†’ `volume_index` in `backup_bitlocker_keys_action.cpp` (1 occurrence)
13. Rename `n`/`b`/`f` â†’ `element_count`/`block_index`/`future` in benchmark workers (3 occurrences)
14. Rename `q` â†’ `queue_index` in `disk_benchmark_worker.cpp` (2 occurrences)
15. Rename `p` â†’ `search_path` in `linux_iso_downloader.cpp` (1 occurrence)
16. Rename `d` â†’ `distro` in `linux_distro_catalog.cpp` (1 occurrence)

#### Declarations (4 statements):
17. Split `QString seq_num, desc, time;` â†’ 3 lines
18. Split `QString vendor, product;` â†’ 2 lines
19. Split `QString authType, encType;` â†’ 2 lines (2 locations)

#### Unit Qualifiers:
20. Audit all `timeout`, `interval`, `elapsed`, `delay` variables â€” append `_ms` suffix
21. Audit all `size`, `length`, `offset` variables â€” append `_bytes` where meaning is ambiguous

**Acceptance Criteria**:
- âœ… Only 2 `catch(...)` blocks remain (both in logger, both with explanatory comments)
- âœ… 0 single-letter variables outside of `c` in tiny lambda predicates
- âœ… 0 multi-declaration statements
- âœ… All timeout/interval variables end with `_ms`
- âœ… All 76 tests pass
- âœ… Build clean with `/W4 /WX`

---

### Phase 6: Line Length Enforcement & .clang-format (Week 14â€“16)

**Goals**:
- Wrap all 1,362 lines exceeding 100 characters
- Create `.clang-format` enforcing 100-column limit
- Add CI lint script

**Tasks**:

#### .clang-format Configuration:
1. Create `.clang-format` in project root:
```yaml
---
Language: Cpp
BasedOnStyle: Google
ColumnLimit: 100
IndentWidth: 4
UseTab: Never
AccessModifierOffset: -4
AlignAfterOpenBracket: Align
AlignConsecutiveAssignments: false
AlignConsecutiveDeclarations: false
AlignOperands: true
AllowShortBlocksOnASingleLine: Empty
AllowShortFunctionsOnASingleLine: Inline
AllowShortIfStatementsOnASingleLine: Never
AllowShortLoopsOnASingleLine: false
BinPackArguments: false
BinPackParameters: false
BreakBeforeBraces: Attach
BreakConstructorInitializers: BeforeComma
ColumnLimit: 100
Cpp11BracedListStyle: true
IncludeBlocks: Preserve
IndentCaseLabels: false
NamespaceIndentation: None
PointerAlignment: Left
SortIncludes: false
SpaceAfterCStyleCast: false
SpaceBeforeParens: ControlStatements
Standard: c++20
```

#### Manual Wrapping Tasks:
2. Wrap embedded PowerShell script strings (~350 lines) â€” use C++ string literal concatenation
3. Wrap Qt stylesheet strings in `windows11_theme.cpp` (~100 lines)
4. Wrap long `connect()` calls (~150 lines) â€” break after `signal,`
5. Wrap long log/format strings (~100 lines) â€” break format string
6. Wrap long comments (~180 lines) â€” reflow prose
7. Wrap long function signatures in headers (~100 lines) â€” one parameter per line
8. Wrap long initializer lists (~80 lines) â€” one element per line
9. Wrap remaining long lines (~300 lines)

#### CI Lint Script:
10. Create `scripts/lint_tigerstyle.py`:
    - Check max line length (100 chars, exclude raw strings)
    - Check function length (â‰¤70 lines)
    - Check nesting depth (â‰¤3 levels)
    - Check assertion density (warn if <2 per function)
    - Check for `catch(...)` without comment
    - Report violations with file:line references
11. Add lint script to CI pipeline

**Acceptance Criteria**:
- âœ… 0 lines exceed 100 characters (verified by lint script)
- âœ… `.clang-format` applies cleanly with no behavior changes
- âœ… CI lint script runs on every PR
- âœ… All 76 tests pass
- âœ… Build clean with `/W4 /WX`

---

### Phase 7: Assertion Safety â€” Actions & GUI Layer (Week 17â€“19)

**Goals**:
- Add `Q_ASSERT` preconditions/postconditions to all public functions in `src/actions/` and `src/gui/`
- Achieve â‰¥2 assertions per function across the full codebase

**Tasks**:
1. Add assertions to all 36 action `execute()` methods (~108 assertions: pre, mid, post each)
2. Add assertions to all 9 panel constructors and `setupUi()` methods (~54 assertions)
3. Add assertions to all panel slot methods (~200 assertions across all panels)
4. Add assertions to dialog constructors and methods (~40 assertions)
5. Add assertions to `MainWindow` methods (~20 assertions)
6. Add assertions to `windows11_theme.cpp` helpers (~10 assertions)
7. Calculate final assertion density â€” must average â‰¥2 per function
8. Run full test suite

**Estimated Assertions Added**: ~432

**Acceptance Criteria**:
- âœ… Every `execute()` has â‰¥3 assertions (pre-input, mid-process, post-result)
- âœ… Every `setupUi()` has â‰¥1 assertion (this pointer valid)
- âœ… Every slot handler has â‰¥1 assertion (expected widget state)
- âœ… Total assertion density â‰¥2 per function across codebase
- âœ… All 76 tests pass
- âœ… Build clean with `/W4 /WX`

---

### Phase 8: Expanded Test Coverage (Week 20â€“23)

**Goals**:
- Add unit tests for the 36 untested action implementations
- Add tests that exercise assertion paths (verify assertions don't trigger on valid data)
- Achieve â‰¥80% function-level test coverage for `src/core/` and `src/actions/`

**Tasks**:
1. Write `test_user_profile_backup_worker.cpp` â€” backup logic, path validation, error handling
2. Write `test_user_profile_restore_worker.cpp` â€” restore logic, conflict resolution
3. Write `test_quick_action_controller.cpp` â€” action dispatch, result aggregation
4. Write `test_quick_action_result_io.cpp` â€” result serialization/deserialization
5. Write `test_migration_report.cpp` â€” HTML/JSON report generation
6. Write `test_flash_coordinator.cpp` â€” flash lifecycle, cancellation
7. Write `test_linux_distro_catalog.cpp` â€” catalog integrity, search
8. Write `test_linux_iso_downloader.cpp` â€” URL validation, download lifecycle (mocked)
9. Write `test_windows_usb_creator.cpp` â€” validation logic (mocked I/O)
10. Write `test_uup_iso_builder.cpp` â€” preparation phases (mocked)
11. Write tests for 26 remaining action workers (1 test file per action, ~3 test cases each)
12. Add assertion-path tests: verify that assertions do NOT fire on valid inputs
13. Add boundary tests: verify that assertions DO fire on invalid inputs in debug builds
14. Update `CMakeLists.txt` with new test targets
15. Run full test suite

**Acceptance Criteria**:
- âœ… â‰¥80% function-level test coverage for `src/core/`
- âœ… â‰¥80% function-level test coverage for `src/actions/`
- âœ… Every public function with assertions has â‰¥1 test exercising the happy path
- âœ… All tests pass in both Debug and Release configurations
- âœ… No assertion failures on valid test inputs

---

### Phase 9: Final Audit & Documentation (Week 24)

**Goals**:
- Run `lint_tigerstyle.py` â€” zero violations
- Verify all principles are met
- Update project documentation

**Tasks**:
1. Run `lint_tigerstyle.py` â€” fix any remaining violations
2. Run `clang-format --dry-run -Werror` â€” verify formatting
3. Build in Debug mode â€” verify assertions don't fire
4. Build in Release mode â€” verify 0 warnings
5. Run all tests â€” 100% pass rate
6. Calculate final metrics:
   - Total assertions / total functions (target: â‰¥2.0)
   - Max function length (target: â‰¤70)
   - Max line length (target: â‰¤100)
   - Max nesting depth (target: â‰¤3)
   - `catch(...)` count (target: â‰¤2, both in logger)
7. Update this document with final compliance ratings
8. Update `README.md` with TigerStyle badge/reference
9. Update `CONTRIBUTING.md` with TigerStyle expectations for new code

**Acceptance Criteria**:
- âœ… `lint_tigerstyle.py` reports 0 violations
- âœ… All 10 principles rated "Compliant" or "Mostly Compliant" (with documented exceptions)
- âœ… Documentation updated

**Final Metrics (Phase 9 Completion)**:

| Metric | Value |
|---|---|
| Source files (src/ + include/) | 293 |
| Source lines | 76,011 |
| Test files | 76 |
| Test lines | 14,181 |
| Assertions (src/include) | 484 |
| Assertions (tests) | 1,619 |
| Total assertions | 2,103 |
| Assertion density (src) | 6.4 per 1k lines |
| `lint_tigerstyle.py` errors | **0** |
| `lint_tigerstyle.py` warnings | 870 (assertion-density only) |
| Max function length | â‰¤70 lines |
| Max nesting depth | â‰¤3 levels |
| Max line length | â‰¤100 columns |
| Release build warnings | **0** |
| Tests passed | 76/76 (100%) |

---

## ðŸ“‹ CMakeLists.txt Changes

### No Source File Changes

TigerStyle compliance does not add or remove any `.cpp` or `.h` files from the build. All changes are within existing files plus 3 new constants headers (which are header-only and included by existing files).

### New Test Targets

```cmake
# Action worker tests (Phase 8) â€” 36 new test files
set(ACTION_TEST_SOURCES
    tests/unit/test_backup_bitlocker_keys_action.cpp
    tests/unit/test_check_bloatware_action.cpp
    tests/unit/test_check_disk_errors_action.cpp
    tests/unit/test_clear_browser_cache_action.cpp
    tests/unit/test_clear_event_logs_action.cpp
    tests/unit/test_clear_windows_update_cache_action.cpp
    tests/unit/test_create_restore_point_action.cpp
    tests/unit/test_defragment_drives_action.cpp
    tests/unit/test_development_configs_backup_action.cpp
    tests/unit/test_disable_startup_programs_action.cpp
    tests/unit/test_disk_cleanup_action.cpp
    tests/unit/test_optimize_power_settings_action.cpp
    tests/unit/test_reset_network_action.cpp
    tests/unit/test_test_network_speed_action.cpp
    tests/unit/test_update_all_apps_action.cpp
    tests/unit/test_windows_update_action.cpp
    # ... and 20 more
)

# Core worker tests (Phase 8)
set(CORE_TEST_SOURCES
    tests/unit/test_user_profile_backup_worker.cpp
    tests/unit/test_user_profile_restore_worker.cpp
    tests/unit/test_quick_action_controller.cpp
    tests/unit/test_quick_action_result_io.cpp
    tests/unit/test_migration_report.cpp
    tests/unit/test_flash_coordinator.cpp
    tests/unit/test_linux_distro_catalog.cpp
    tests/unit/test_linux_iso_downloader.cpp
    tests/unit/test_windows_usb_creator.cpp
    tests/unit/test_uup_iso_builder.cpp
)
```

---

## ðŸ“‹ .clang-format Configuration

```yaml
---
# S.A.K. Utility â€” TigerStyle-aligned formatting.
# Enforces 100-column hard limit, 4-space indent, Attach braces.
Language: Cpp
BasedOnStyle: Google
ColumnLimit: 100
IndentWidth: 4
TabWidth: 4
UseTab: Never
AccessModifierOffset: -4
AlignAfterOpenBracket: Align
AlignConsecutiveAssignments: false
AlignConsecutiveDeclarations: false
AlignOperands: true
AllowAllParametersOfDeclarationOnNextLine: true
AllowShortBlocksOnASingleLine: Empty
AllowShortCaseLabelsOnASingleLine: false
AllowShortFunctionsOnASingleLine: Inline
AllowShortIfStatementsOnASingleLine: Never
AllowShortLoopsOnASingleLine: false
BinPackArguments: false
BinPackParameters: false
BraceWrapping:
  AfterCaseLabel: false
  AfterClass: false
  AfterControlStatement: Never
  AfterEnum: false
  AfterFunction: false
  AfterNamespace: false
  AfterStruct: false
  AfterUnion: false
  BeforeCatch: false
  BeforeElse: false
  IndentBraces: false
  SplitEmptyFunction: true
  SplitEmptyRecord: true
BreakBeforeBraces: Attach
BreakConstructorInitializers: BeforeComma
ColumnLimit: 100
CompactNamespaces: false
Cpp11BracedListStyle: true
DerivePointerAlignment: false
FixNamespaceComments: true
IncludeBlocks: Preserve
IndentCaseLabels: false
IndentPPDirectives: None
NamespaceIndentation: None
PenaltyBreakBeforeFirstCallParameter: 19
PenaltyBreakComment: 300
PenaltyBreakFirstLessLess: 120
PenaltyBreakString: 1000
PenaltyExcessCharacter: 1000000
PenaltyReturnTypeOnItsOwnLine: 60
PointerAlignment: Left
ReflowComments: true
SortIncludes: false
SortUsingDeclarations: true
SpaceAfterCStyleCast: false
SpaceAfterLogicalNot: false
SpaceBeforeAssignmentOperators: true
SpaceBeforeCpp11BracedList: false
SpaceBeforeCtorInitializerColon: true
SpaceBeforeInheritanceColon: true
SpaceBeforeParens: ControlStatements
SpaceBeforeRangeBasedForLoopColon: true
SpaceInEmptyParentheses: false
SpacesBeforeTrailingComments: 2
SpacesInAngles: false
SpacesInCStyleCastParentheses: false
SpacesInContainerLiterals: true
SpacesInParentheses: false
SpacesInSquareBrackets: false
Standard: c++20
```

---

## ðŸ§ª Testing Strategy

### Existing Tests (76 files â€” maintained throughout)

All 76 existing tests must pass after every phase. No regressions. TigerStyle changes are pure refactoring plus addition of assertions â€” behavior must not change.

### New Tests (Phase 8 â€” 46+ files)

| Category | Files | Test Cases |
|---|---|---|
| Action workers | 36 | ~108 (3 per action: happy path, invalid input, edge case) |
| Core workers | 10 | ~50 (5 per worker: lifecycle, cancellation, error handling, output validation, boundary) |
| **Total** | **46** | **~158** |

### Assertion Testing

| Test Type | What | How |
|---|---|---|
| Happy path | Assertions don't fire on valid input | Standard test execution |
| Invalid input | Assertions fire on bad input (debug only) | `QVERIFY_EXCEPTION_THROWN` or death test pattern |
| Boundary values | Edge cases at valid/invalid boundary | Parameterized tests |

### CI Integration

```
# .github/workflows/tigerstyle.yml
- name: Lint TigerStyle
  run: python scripts/lint_tigerstyle.py --strict

- name: Check formatting
  run: clang-format --dry-run -Werror src/**/*.cpp include/**/*.h

- name: Build (Debug + assertions)
  run: cmake --build build --config Debug

- name: Test (Debug)
  run: ctest -C Debug --output-on-failure

- name: Build (Release)
  run: cmake --build build --config Release

- name: Test (Release)
  run: ctest -C Release --output-on-failure
```

---

## ðŸŽ¯ Success Metrics

| Metric | Current | Target | Measurement |
|---|---|---|---|
| Assertion density (assertions/function) | 0.0005 | â‰¥ 2.0 | `grep -c Q_ASSERT` / function count |
| Max function length (lines) | 577 | â‰¤ 70 | `lint_tigerstyle.py` |
| Max line length (chars) | 300+ | â‰¤ 100 | `lint_tigerstyle.py` |
| Max nesting depth (levels) | 6 | â‰¤ 3 | `lint_tigerstyle.py` |
| `catch(...)` blocks | 24 | â‰¤ 2 (logger only) | `grep -c 'catch\s*(\.\.\.)` |
| Magic number violations | ~2,589 | < 100 (acceptable 0/1/-1) | `lint_tigerstyle.py` |
| Single-letter variables | 34 | â‰¤ 5 (tiny lambdas only) | `lint_tigerstyle.py` |
| Else-after-return violations | 18 | 0 | `lint_tigerstyle.py` |
| Test count | 76 | â‰¥ 103 | `ctest --test-dir build -N` |
| Test pass rate | 100% | 100% | `ctest -C Release` |
| Build warnings | 0 | 0 | `cmake --build --config Release` |

---

## ðŸš§ Risks & Mitigations

### Risk 1: Assertion Overhead in Release Builds
- **Risk**: `Q_ASSERT` is a no-op in Release builds (compiled out by `QT_NO_DEBUG`). Some preconditions may need enforcement even in Release.
- **Mitigation**: For critical invariants (file existence, non-null pointers before dereference), use explicit `if (!condition) return error;` in addition to `Q_ASSERT`. The assertion documents intent; the guard provides runtime safety.

### Risk 2: Large Refactoring Introduces Regressions
- **Risk**: Splitting 145 functions and renaming 34 variables could introduce subtle behavior changes.
- **Mitigation**: 
  - Run full test suite after every individual file change (not batched).
  - Use `git diff` to verify no logic changes â€” only structural moves.
  - Phase 8 adds 46 new test files to catch anything missed.

### Risk 3: Embedded PowerShell Strings Resist Line Wrapping
- **Risk**: Some embedded PowerShell commands are inherently long (200+ chars) and cannot be meaningfully wrapped.
- **Mitigation**: 
  - Use C++ adjacent string literal concatenation (breaks at logical pipeline `|` points).
  - For very long scripts, externalize to `.ps1` files and load at runtime (eliminates the line-length issue entirely).
  - Document any remaining exemptions (target: < 10 lines total).

### Risk 4: clang-format Changes Too Aggressive
- **Risk**: Running `clang-format` project-wide may change formatting in ways that create noisy diffs.
- **Mitigation**: 
  - Apply `clang-format` in a single dedicated commit (no mixed formatting+logic changes).
  - Use `// clang-format off` / `// clang-format on` for intentionally-aligned blocks (e.g., tabular constant definitions in `style_constants.h`).
  - Review the diff carefully before committing.

### Risk 5: Assertion Density Target Creates Low-Value Assertions
- **Risk**: Mechanically adding `Q_ASSERT(true)` or trivial assertions to meet density targets.
- **Mitigation**: 
  - Every assertion must validate a real precondition, postcondition, or invariant.
  - Code review specifically checks assertion quality.
  - TigerStyle's "pair assertions" principle: assert validity before AND after data crosses a boundary.

---

## ðŸ“Š Effort Estimates

| Phase | Scope | Est. Files Changed | Est. Lines Changed | Duration |
|---|---|---|---|---|
| 1 | Assertions â€” Core | ~22 files | ~680 lines added | 3 weeks |
| 2 | Function Decomposition | ~45 files | ~3,000 lines restructured | 3 weeks |
| 3 | Magic Number Elimination | ~60 files + 3 new headers | ~2,800 lines changed | 3 weeks |
| 4 | Nesting & Control Flow | ~35 files | ~1,200 lines restructured | 2 weeks |
| 5 | Error Handling & Naming | ~20 files | ~200 lines changed | 2 weeks |
| 6 | Line Length & .clang-format | ~80 files + 2 new files | ~2,700 lines reformatted | 3 weeks |
| 7 | Assertions â€” Actions & GUI | ~45 files | ~860 lines added | 3 weeks |
| 8 | Expanded Test Coverage | ~46 new test files | ~4,600 lines added | 4 weeks |
| 9 | Final Audit & Documentation | ~5 files | ~200 lines | 1 week |
| | **Total** | **~151 files + 51 new** | **~16,240 lines** | **24 weeks** |

**Total Timeline**: 24 weeks (6 months)

---

## ðŸ’¡ TigerStyle Rules Intentionally Deferred

These TigerStyle principles are valid but are deferred beyond this plan's scope:

| Rule | Status | Rationale |
|---|---|---|
| Performance back-of-envelope sketches | Deferred | SAK is not latency-critical; UI responsiveness is already acceptable. |
| Batch I/O operations | Deferred | Would require architectural changes to action framework. |
| Explicit library options at all call sites | Deferred | Low impact; addressed opportunistically during other phases. |
| Pair assertions at all data boundaries | Partial | Implemented for file I/O and config read/write; not for all Qt signals. |
| Compile-time-only constants for all buffer sizes | Partial | Most are `constexpr`; Qt containers use dynamic sizing internally. |

---

## ðŸ“š Resources

### TigerStyle
- [TigerStyle Document (GitHub)](https://github.com/tigerbeetle/tigerbeetle/blob/main/docs/TIGER_STYLE.md)
- [NASA Power of Ten Rules (PDF)](https://spinroot.com/gerard/pdf/P10.pdf)
- [It Takes Two to Contract (TigerBeetle Blog)](https://tigerbeetle.com/blog/2023-12-27-it-takes-two-to-contract)
- [Push Ifs Up and Fors Down (Matklad)](https://matklad.github.io/2023/11/15/push-ifs-up-and-fors-down.html)
- [Analysis of Production Failures (OSDI '14)](https://www.usenix.org/system/files/conference/osdi14/osdi14-paper-yuan.pdf)

### C++ / Qt Equivalents
- [Q_ASSERT Documentation](https://doc.qt.io/qt-6/qtglobal.html#Q_ASSERT)
- [Q_ASSERT_X Documentation](https://doc.qt.io/qt-6/qtglobal.html#Q_ASSERT_X)
- [static_assert (cppreference)](https://en.cppreference.com/w/cpp/language/static_assert)
- [ClangFormat Documentation](https://clang.llvm.org/docs/ClangFormat.html)
- [MSVC /W4 Warning Level](https://learn.microsoft.com/en-us/cpp/build/reference/compiler-option-warning-level)

### S.A.K. Utility Internal
- [style_constants.h](../include/sak/style_constants.h) â€” Existing design token system
- [widget_helpers.h](../include/sak/widget_helpers.h) â€” Shared UI utility functions
- [CONTRIBUTING.md](../CONTRIBUTING.md) â€” Contribution guidelines (to be updated)

---

## ðŸ“ž Support

**Questions?** Open a GitHub Discussion  
**Found a Bug?** Open a GitHub Issue  
**Want to Contribute?** See [CONTRIBUTING.md](../CONTRIBUTING.md)

---

**Document Version**: 2.0  
**Last Updated**: February 28, 2026  
**Author**: Randy Northrup  
**Status**: âœ… Complete â€” All 10 principles compliant

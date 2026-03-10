# SAK Utility — Code Duplication Audit

> **Generated:** 2026-02-28  
> **Scope:** Comprehensive deep scan for code duplication and duplicity  
> **Codebase:** C++23 / Qt6

---

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [DUP-01: formatFileSize / formatSize / formatBytes — 8+ Implementations](#dup-01-file-size-formatting)
3. [DUP-02: statusColor / progressColor / applyStatusColors — Exact Copy-Paste](#dup-02-status-color-helpers)
4. [DUP-03: copyFileWithProgress — Near-Duplicate Implementations](#dup-03-copy-file-with-progress)
5. [DUP-04: calculateDirectorySize — Identical Wrapper Functions](#dup-04-calculate-directory-size)
6. [DUP-05: PowerShell Get-DirectorySize / Format-Bytes — Duplicated Embedded Scripts](#dup-05-powershell-helper-functions)
7. [DUP-06: Process Result Checking Pattern — Repeated Boilerplate](#dup-06-process-result-checking)
8. [DUP-07: Backup Action Execution Pattern — Structural Clones](#dup-07-backup-action-pattern)
9. [DUP-08: Log Box Construction — Manual Box-Drawing Everywhere](#dup-08-log-box-construction)
10. [DUP-09: Inline Size Formatting — Ad-hoc GB/MB/KB Conversions](#dup-09-inline-size-formatting)
11. [DUP-10: isProcessRunning / tasklist Pattern — Repeated Process Detection](#dup-10-process-detection)
12. [DUP-11: WindowsUserScanner Instantiation — Repeated Scanner Creation](#dup-11-user-scanner-instantiation)
13. [Estimated Impact Summary](#estimated-impact-summary)

---

## Executive Summary

The codebase contains **11 distinct categories** of actionable code duplication, totaling an estimated **600–800 lines** of redundant code. The most severe duplication is in **file size formatting** (8+ independent implementations of the same function) and **the backup action execution pattern** (6+ near-identical copy/scan/backup flows). Most duplications are in `src/actions/` and `src/gui/`.

**Key finding:** While the project has excellent base-class abstractions (`QuickAction`, `WorkerBase`, `StreamingDecompressor`), several utility functions that should have been centralized were instead reimplemented locally in each file that needed them.

---

## DUP-01: File Size Formatting {#dup-01-file-size-formatting}

**Severity:** HIGH — 8+ independent implementations of the same function  
**Type:** Near-duplicate (same logic, minor formatting differences)  
**Estimated duplicate lines:** ~80 lines

### Instances

| # | Location | Function Name | Signature | Notes |
|---|----------|---------------|-----------|-------|
| 1 | [quick_action.cpp](../src/core/quick_action.cpp#L84) | `QuickAction::formatFileSize` | `QString(qint64)` | Uses `1073741824LL` literal constants, 2 decimal places |
| 2 | [linux_iso_downloader.cpp](../src/core/linux_iso_downloader.cpp#L33) | `formatSize` (anon namespace) | `QString(qint64)` | Uses `1024 * 1024` expressions, 1 decimal place |
| 3 | [image_flasher_panel.cpp](../src/gui/image_flasher_panel.cpp#L817) | `ImageFlasherPanel::formatFileSize` | `QString(qint64) const` | Identical to #2 |
| 4 | [linux_iso_download_dialog.cpp](../src/gui/linux_iso_download_dialog.cpp#L509) | `LinuxISODownloadDialog::formatSize` | `QString(qint64)` | Like #2 but adds `<=0` → "Unknown" |
| 5 | [quick_actions_panel.cpp](../src/gui/quick_actions_panel.cpp#L512) | `QuickActionsPanel::formatBytes` | `QString(qint64)` | Uses named constants (`kb`, `mb`, `gb`), KB shows no decimals |
| 6 | [network_transfer_panel.cpp](../src/gui/network_transfer_panel.cpp#L80) | `formatBytes` (anon namespace) | `QString(qint64)` | Compact, no "bytes" case |
| 7 | [network_transfer_panel_transfer.cpp](../src/gui/network_transfer_panel_transfer.cpp#L73) | `formatBytes` (anon namespace) | `QString(qint64)` | **Exact copy** of #6 |
| 8 | [network_transfer_panel_orchestrator.cpp](../src/gui/network_transfer_panel_orchestrator.cpp#L63) | `formatBytes` (anon namespace) | `QString(qint64)` | **Exact copy** of #6 |
| 9 | [diagnostic_benchmark_panel.cpp](../src/gui/diagnostic_benchmark_panel.cpp#L656) | `DiagnosticBenchmarkPanel::formatBytes` | `QString(uint64_t)` | Uses loop + array approach, handles TB |
| 10 | [diagnostic_report_generator.cpp](../src/core/diagnostic_report_generator.cpp#L724) | `DiagnosticReportGenerator::formatBytes` | `QString(uint64_t)` | Linear if-chain, handles TB |

### Recommendation

Extract to a single free function in `path_utils.h` / `path_utils.cpp` (or a new `string_utils.h`):

```cpp
namespace sak {
QString formatBytes(qint64 bytes);       // for Qt file sizes
QString formatBytes(uint64_t bytes);     // for hardware sizes (TB support)
}
```

All 10 call sites would replace their local implementation with `sak::formatBytes()`. The `QuickAction::formatFileSize` base class method can become a thin inline wrapper or be deprecated in favor of the free function.

---

## DUP-02: statusColor / progressColor / applyStatusColors {#dup-02-status-color-helpers}

**Severity:** HIGH — Exact copy-paste across 2 files (3 functions each)  
**Type:** Exact duplicate  
**Estimated duplicate lines:** ~40 lines

### Instances

| # | Location | Functions |
|---|----------|-----------|
| 1 | [network_transfer_panel.cpp](../src/gui/network_transfer_panel.cpp#L89-L121) | `statusColor()`, `progressColor()`, `applyStatusColors()` |
| 2 | [network_transfer_panel_orchestrator.cpp](../src/gui/network_transfer_panel_orchestrator.cpp#L72-L104) | `statusColor()`, `progressColor()`, `applyStatusColors()` — **identical** |

Both are in anonymous namespaces. The function bodies, color values (`QColor(56,142,60)`, `QColor(198,40,40)`, `QColor(245,124,0)`, `QColor(97,97,97)`), string matching logic, and signatures are character-for-character identical.

### Recommendation

Move to a shared header (e.g., `sak/gui_utils.h` or `sak/network_transfer_panel.h`) as `inline` functions, or into a `gui_utils.cpp` compilation unit. These three functions are tightly coupled and should live together.

---

## DUP-03: copyFileWithProgress {#dup-03-copy-file-with-progress}

**Severity:** MEDIUM — 2 near-duplicate implementations  
**Type:** Near-duplicate (same logic, minor differences)  
**Estimated duplicate lines:** ~50 lines

### Instances

| # | Location | Lines | Notes |
|---|----------|-------|-------|
| 1 | [outlook_backup_action.cpp](../src/actions/outlook_backup_action.cpp#L209-L241) | 33 lines | Uses `char buffer[64*1024]`, no cancellation check, no timestamp preservation |
| 2 | [quickbooks_backup_action.cpp](../src/actions/quickbooks_backup_action.cpp#L335-L384) | 50 lines | Uses `QByteArray`, has cancellation check, preserves timestamps |

Both open source → open dest → loop read/write 64KB chunks → emit progress → close.

### Recommendation

Move to `QuickAction` base class as a protected method `copyFileWithProgress(source, dest, preserveTimestamps=true)` with cancellation support. Both subclasses then call the inherited method. The QuickBooks version is more complete and should serve as the canonical implementation.

---

## DUP-04: calculateDirectorySize {#dup-04-calculate-directory-size}

**Severity:** MEDIUM — 3 identical wrapper implementations  
**Type:** Exact duplicate  
**Estimated duplicate lines:** ~30 lines

### Instances

| # | Location | Lines | Implementation |
|---|----------|-------|----------------|
| 1 | [clear_windows_update_cache_action.cpp](../src/actions/clear_windows_update_cache_action.cpp#L30-L38) | 9 lines | Wraps `path_utils::getDirectorySizeAndCount()` |
| 2 | [disk_cleanup_action.cpp](../src/actions/disk_cleanup_action.cpp#L386-L394) | 9 lines | **Identical** wrapper of same function |
| 3 | [per_user_customization_dialog.cpp](../src/gui/per_user_customization_dialog.cpp#L545-L572) | 28 lines | Different — uses recursive `QDir` approach instead of `path_utils` |

Instances 1 and 2 are **character-for-character identical**:
```cpp
qint64 ClassName::calculateDirectorySize(const QString& path, int& file_count) {
    file_count = 0;
    auto result = path_utils::getDirectorySizeAndCount(path.toStdWString());
    if (!result) { return 0; }
    file_count = static_cast<int>(result->file_count);
    return static_cast<qint64>(result->total_bytes);
}
```

### Recommendation

Move to `QuickAction` base class as a protected method since both call sites are action subclasses. Instance 3 in the dialog should also be migrated to use `path_utils::getDirectorySizeAndCount()` for consistency.

---

## DUP-05: PowerShell Get-DirectorySize / Format-Bytes {#dup-05-powershell-helper-functions}

**Severity:** MEDIUM — Identical PS helper functions embedded in C++ string literals  
**Type:** Near-duplicate  
**Estimated duplicate lines:** ~40 lines

### Instances

**`Get-DirectorySize` function:**

| # | Location | Lines |
|---|----------|-------|
| 1 | [clear_windows_update_cache_action.cpp](../src/actions/clear_windows_update_cache_action.cpp#L152-L158) | 7 lines |
| 2 | [clear_browser_cache_action.cpp](../src/actions/clear_browser_cache_action.cpp#L180-L188) | 8 lines |

Both define the same PS function: `Get-ChildItem -Recurse -File | Measure-Object -Property Length -Sum`. Minor formatting differences only.

**`Format-Bytes` function:**

| # | Location | Lines |
|---|----------|-------|
| 1 | [clear_windows_update_cache_action.cpp](../src/actions/clear_windows_update_cache_action.cpp#L143-L149) | 6 lines |
| 2 | [clear_browser_cache_action.cpp](../src/actions/clear_browser_cache_action.cpp#L191-L197) | 6 lines |

Identical formatting logic: `$Bytes -ge 1GB → '{0:N2} GB'`, etc.

### Recommendation

Create a PowerShell script template helper (e.g., `ps_helpers.h`) with `constexpr` or `const QString` variables for common PS function definitions that can be prepended to scripts:

```cpp
namespace sak::ps {
    inline const QString kFormatBytes = "function Format-Bytes { ... }\n";
    inline const QString kGetDirectorySize = "function Get-DirectorySize { ... }\n";
}
```

---

## DUP-06: Process Result Checking Pattern {#dup-06-process-result-checking}

**Severity:** LOW-MEDIUM — Repeated ~2-line boilerplate, 30+ instances  
**Type:** Pattern duplication  
**Estimated duplicate lines:** ~60 lines

### The Pattern

```cpp
if (proc.timed_out || proc.exit_code != 0) {
    // handle error
}
```

This exact expression appears 30+ times across action files:

- [reset_network_action.cpp](../src/actions/reset_network_action.cpp) — 6 instances (lines 19, 27, 36, 45, 50, 58)
- [screenshot_settings_action.cpp](../src/actions/screenshot_settings_action.cpp) — 3 instances (lines 81, 171, 202)
- [scan_malware_action.cpp](../src/actions/scan_malware_action.cpp) — 1 instance (line 18)
- [windows_update_action.cpp](../src/actions/windows_update_action.cpp) — 1 instance (line 20)
- [clear_print_spooler_action.cpp](../src/actions/clear_print_spooler_action.cpp) — 2 instances (lines 31, 53)
- [disable_visual_effects_action.cpp](../src/actions/disable_visual_effects_action.cpp) — 2 instances (lines 45, 136)
- [export_registry_keys_action.cpp](../src/actions/export_registry_keys_action.cpp) — 1 instance (line 22)
- [backup_bitlocker_keys_action.cpp](../src/actions/backup_bitlocker_keys_action.cpp) — 2 instances (lines 133, 266)
- Plus 10+ more across other action files

A variant `!proc.timed_out && proc.exit_code == 0` (as a return expression) appears in:
- [repair_windows_store_action.cpp](../src/actions/repair_windows_store_action.cpp) — 4 instances (lines 66, 79, 96, 115)
- [rebuild_icon_cache_action.cpp](../src/actions/rebuild_icon_cache_action.cpp#L175)
- [optimize_power_settings_action.cpp](../src/actions/optimize_power_settings_action.cpp#L73)
- [backup_printer_settings_action.cpp](../src/actions/backup_printer_settings_action.cpp#L37)

### Recommendation

Add a convenience method to `ProcessResult`:

```cpp
struct ProcessResult {
    // ...existing members...
    [[nodiscard]] bool succeeded() const {
        return !timed_out && exit_code == 0;
    }
};
```

Then replace `if (proc.timed_out || proc.exit_code != 0)` → `if (!proc.succeeded())` and `return !proc.timed_out && proc.exit_code == 0` → `return proc.succeeded()`.

---

## DUP-07: Backup Action Execution Pattern {#dup-07-backup-action-pattern}

**Severity:** HIGH — 6+ action classes follow nearly identical scan→execute→copy→report flow  
**Type:** Structural near-duplicate  
**Estimated duplicate lines:** ~200 lines

### The Pattern

All backup actions follow this identical structure (~30-40 lines each):

```
1. Check cancellation → emitCancelledResult()
2. Scan for files using WindowsUserScanner + QDirIterator
3. If nothing found → emitFailedResult()
4. Create backup directory: QDir(m_backup_location + "/XxxBackup").mkpath(".")
5. Loop files:
   a. Check cancellation
   b. Emit progress with percentage
   c. sanitizePathForBackup() on source directory
   d. QFile::copy() to target
   e. Accumulate bytes_copied/files_copied
6. Build ExecutionResult: success, message with formatFileSize(), output_path
7. finishWithResult()
```

### Instances

| # | Action Class | File | Key Differences |
|---|-------------|------|-----------------|
| 1 | `OutlookBackupAction` | [outlook_backup_action.cpp](../src/actions/outlook_backup_action.cpp#L130-L200) | Checks isOutlookRunning(), filters *.pst/*.ost |
| 2 | `QuickBooksBackupAction` | [quickbooks_backup_action.cpp](../src/actions/quickbooks_backup_action.cpp#L170-L260) | Checks isQBRunning(), skips open files |
| 3 | `BrowserProfileBackupAction` | [browser_profile_backup_action.cpp](../src/actions/browser_profile_backup_action.cpp#L45-L140) | Copies entire profile directories |
| 4 | `DevelopmentConfigsBackupAction` | [development_configs_backup_action.cpp](../src/actions/development_configs_backup_action.cpp#L150-L240) | Scans multiple config types |
| 5 | `TaxSoftwareBackupAction` | [tax_software_backup_action.cpp](../src/actions/tax_software_backup_action.cpp#L205-L295) | Scans TurboTax/HRBlock/TaxAct |
| 6 | `SavedGameDataBackupAction` | [saved_game_data_backup_action.cpp](../src/actions/saved_game_data_backup_action.cpp#L140-L230) | Scans Steam/Epic/GOG/MyGames |
| 7 | `PhotoManagementBackupAction` | [photo_management_backup_action.cpp](../src/actions/photo_management_backup_action.cpp#L100-L206) | Scans Lightroom/Photoshop catalogs |

### Recommendation

Create a template method in `QuickAction` or a `BackupAction` intermediate base class:

```cpp
class BackupAction : public QuickAction {
protected:
    struct BackupFileInfo {
        QString path;
        QString filename;
        qint64 size;
    };

    // Subclasses implement these
    virtual QString backupSubdirectory() const = 0;
    virtual QVector<BackupFileInfo> discoverFiles() = 0;
    virtual bool isApplicationRunning() { return false; }

    // Template method handles the common flow
    void executeBackup();
};
```

This would eliminate ~200 lines of near-identical code and ensure consistent behavior (timestamp preservation, collision handling, progress reporting).

---

## DUP-08: Log Box Construction {#dup-08-log-box-construction}

**Severity:** MEDIUM — Manual box-drawing instead of using `formatLogBox()`  
**Type:** Missed reuse opportunity  
**Estimated duplicate lines:** ~100 lines

### Context

`QuickAction::formatLogBox()` at [quick_action.cpp](../src/core/quick_action.cpp#L98-L125) provides a standard box-drawing utility. However, **13+ action files** manually construct their own box-drawing strings with `╔═══`, `╠═══`, `╚═══`, `║` characters instead of using this utility.

### Files with manual box-drawing

| File | Approx Lines of Box Code |
|------|-------------------------|
| [update_all_apps_action.cpp](../src/actions/update_all_apps_action.cpp) | ~40 lines |
| [screenshot_settings_action.cpp](../src/actions/screenshot_settings_action.cpp#L225-L252) | ~25 lines |
| [repair_windows_store_action.cpp](../src/actions/repair_windows_store_action.cpp#L177-L228) | ~30 lines |
| [rebuild_icon_cache_action.cpp](../src/actions/rebuild_icon_cache_action.cpp#L220-L262) | ~25 lines |
| [optimize_power_settings_action.cpp](../src/actions/optimize_power_settings_action.cpp#L158-L195) | ~20 lines |
| [fix_audio_issues_action.cpp](../src/actions/fix_audio_issues_action.cpp#L160-L200) | ~25 lines |
| [disable_startup_programs_action.cpp](../src/actions/disable_startup_programs_action.cpp#L163-L220) | ~30 lines |
| [disable_visual_effects_action.cpp](../src/actions/disable_visual_effects_action.cpp#L82-L100) | ~15 lines |
| [create_restore_point_action.cpp](../src/actions/create_restore_point_action.cpp#L169-L210) | ~30 lines |
| [clear_windows_update_cache_action.cpp](../src/actions/clear_windows_update_cache_action.cpp#L324-L385) | ~30 lines |
| [clear_print_spooler_action.cpp](../src/actions/clear_print_spooler_action.cpp#L272-L305) | ~25 lines |
| [clear_event_logs_action.cpp](../src/actions/clear_event_logs_action.cpp#L216-L260) | ~25 lines |
| [clear_browser_cache_action.cpp](../src/actions/clear_browser_cache_action.cpp#L326-L383) | ~30 lines |
| [check_bloatware_action.cpp](../src/actions/check_bloatware_action.cpp#L177-L250) | ~40 lines |

**Problem:** Each file uses slightly different box widths (64 chars vs 68 chars vs 76 chars), making the output visually inconsistent. The base class `formatLogBox()` exists but is rarely used.

### Recommendation

Enhance `QuickAction::formatLogBox()` to support variable-width boxes and key-value pairs, then migrate all action log reports to use it. This would eliminate ~100 lines of manual box-drawing and ensure visual consistency.

---

## DUP-09: Inline Size Formatting {#dup-09-inline-size-formatting}

**Severity:** LOW — Ad-hoc `bytes / (1024.0 * 1024.0)` conversions  
**Type:** Inline duplication  
**Estimated duplicate lines:** ~30 lines

### Instances

[per_user_customization_dialog.cpp](../src/gui/per_user_customization_dialog.cpp) has the same size→string conversion logic **3 separate times** (lines 237-246, 501-509, 529-537):

```cpp
double sizeMB = totalSize / (1024.0 * 1024.0);
if (sizeMB >= 1024) {
    sizeStr = QString("%1 GB").arg(sizeMB / 1024.0, 0, 'f', 2);
} else if (totalSize >= 1024) {
    sizeStr = QString("%1 MB").arg(sizeMB, 0, 'f', 1);
} else {
    sizeStr = QString("%1 KB").arg(totalSize / 1024.0, 0, 'f', 1);
}
```

Additional ad-hoc conversions in:
- [user_profile_backup_wizard_pages.cpp](../src/gui/user_profile_backup_wizard_pages.cpp#L164): `totalSize / (1024.0 * 1024.0 * 1024.0)`
- [user_profile_restore_wizard.cpp](../src/gui/user_profile_restore_wizard.cpp#L150): `manifest.total_backup_size_bytes / (1024.0 * 1024.0 * 1024.0)`
- [organizer_panel.cpp](../src/gui/organizer_panel.cpp): wasted-space formatting inline

### Recommendation

All of these should call the centralized `formatBytes()` function from DUP-01's recommendation.

---

## DUP-10: Process Detection (isProcessRunning / tasklist) {#dup-10-process-detection}

**Severity:** LOW — Same 3-line pattern repeated  
**Type:** Near-duplicate  
**Estimated duplicate lines:** ~20 lines

### Instances

| # | Location | Process Checked |
|---|----------|----------------|
| 1 | [outlook_backup_action.cpp](../src/actions/outlook_backup_action.cpp#L204-L207) | `OUTLOOK.EXE` |
| 2 | [outlook_backup_action.cpp](../src/actions/outlook_backup_action.cpp#L75) | `OUTLOOK.EXE` (same file, called twice) |
| 3 | [quickbooks_backup_action.cpp](../src/actions/quickbooks_backup_action.cpp#L156-L163) | `QBW32.EXE` / `QBW64.EXE` |
| 4 | [screenshot_settings_action.cpp](../src/actions/screenshot_settings_action.cpp#L278-L283) | Generic process name via `QProcess` |

Pattern:
```cpp
ProcessResult proc = runProcess("tasklist", QStringList() << "/FI" << "IMAGENAME eq XXX.EXE", 3000);
return proc.std_out.contains("XXX.EXE", Qt::CaseInsensitive);
```

### Recommendation

Add `isProcessRunning(const QString& processName)` to `QuickAction` or as a free function in `process_runner.h`:

```cpp
bool isProcessRunning(const QString& imageName);
```

---

## DUP-11: WindowsUserScanner Instantiation {#dup-11-user-scanner-instantiation}

**Severity:** LOW — Repeated instantiation pattern  
**Type:** Pattern duplication (minor)  
**Estimated duplicate lines:** ~15 lines

### Instances

The pattern `WindowsUserScanner scanner; QVector<UserProfile> users = scanner.scanUsers();` appears in **10+ action files**:

- [outlook_backup_action.cpp](../src/actions/outlook_backup_action.cpp#L23-L24) (appears twice: scan + execute)
- [quickbooks_backup_action.cpp](../src/actions/quickbooks_backup_action.cpp#L275-L276)
- [browser_profile_backup_action.cpp](../src/actions/browser_profile_backup_action.cpp#L57)
- [development_configs_backup_action.cpp](../src/actions/development_configs_backup_action.cpp#L152)
- [saved_game_data_backup_action.cpp](../src/actions/saved_game_data_backup_action.cpp#L143)
- [sticky_notes_backup_action.cpp](../src/actions/sticky_notes_backup_action.cpp#L22)
- [tax_software_backup_action.cpp](../src/actions/tax_software_backup_action.cpp#L208)
- [photo_management_backup_action.cpp](../src/actions/photo_management_backup_action.cpp#L105)
- [backup_desktop_wallpaper_action.cpp](../src/actions/backup_desktop_wallpaper_action.cpp#L50)

Each creates a new `WindowsUserScanner` instance and scans. The scan results don't change between calls within the same action, yet some actions (like OutlookBackupAction) scan twice — once in `scan()` and again in `execute()`.

### Recommendation

For the `BackupAction` intermediary (DUP-07), cache user profiles in the base class:

```cpp
class BackupAction : public QuickAction {
protected:
    QVector<UserProfile> cachedUsers();  // lazy-init, scan once
private:
    std::optional<QVector<UserProfile>> m_cached_users;
};
```

---

## Estimated Impact Summary

| ID | Category | Duplicate Lines | Severity | Effort to Fix |
|----|----------|----------------|----------|---------------|
| DUP-01 | formatFileSize/formatBytes | ~80 | HIGH | Low — extract function |
| DUP-02 | statusColor/applyStatusColors | ~40 | HIGH | Low — move to shared header |
| DUP-03 | copyFileWithProgress | ~50 | MEDIUM | Low — lift to base class |
| DUP-04 | calculateDirectorySize | ~30 | MEDIUM | Low — lift to base class |
| DUP-05 | PS Get-DirectorySize/Format-Bytes | ~40 | MEDIUM | Low — extract PS constants |
| DUP-06 | proc.timed_out check | ~60 | LOW-MED | Low — add `succeeded()` method |
| DUP-07 | Backup action template | ~200 | HIGH | Medium — create base class |
| DUP-08 | Manual box-drawing | ~100 | MEDIUM | Medium — enhance formatLogBox() |
| DUP-09 | Inline size math | ~30 | LOW | Low — replace with formatBytes() |
| DUP-10 | tasklist process check | ~20 | LOW | Low — extract utility function |
| DUP-11 | Scanner instantiation | ~15 | LOW | Low — cache in base class |
| | **TOTAL** | **~665** | | |

### Recommended Refactoring Priority

1. **DUP-01** (formatBytes) — Highest ROI. One function replaces 10 implementations. Touch once, fix everywhere.
2. **DUP-02** (statusColor) — Trivial exact copy-paste elimination.
3. **DUP-06** (ProcessResult::succeeded) — Simple struct method addition, 30+ simplified call sites.
4. **DUP-07** (BackupAction base class) — Largest impact but requires most design work. Would also resolve DUP-03, DUP-04, DUP-10, and DUP-11.
5. **DUP-05** (PS helpers) — Quick win for embedded script maintenance.
6. **DUP-08** (Log boxes) — Gradual migration, can be done per-action.
7. **DUP-09** (Inline math) — Automatically resolved once DUP-01 is done.

### Items NOT Flagged (Justified Patterns)

- **Decompressor classes** (bzip2, gzip, xz): These follow the Template Method pattern via `StreamingDecompressor` base class. Each implements different library APIs (BZ2, zlib, lzma). This is proper polymorphism, not duplication.
- **Action `scan()` / `execute()` virtual methods**: Each has genuinely different logic. Only the backup subset (DUP-07) has structural duplication.
- **`emitCancelledResult()` / `emitFailedResult()` / `finishWithResult()` calls**: These are calls to the base class, not duplicated implementations. The base class correctly provides these utilities.
- **Constructor boilerplate** (member initializer lists): Natural and idiomatic.
- **Benchmark workers** (CPU, Memory, Disk): Each tests fundamentally different hardware with different APIs. No actionable duplication.

# Advanced Uninstall Panel - Comprehensive Implementation Plan

**Version**: 1.0  
**Date**: March 2, 2026  
**Status**: ✅ Complete  
**Completed in**: v0.8.5  
**Inspiration**: [Revo Uninstaller Pro](https://www.revouninstaller.com/) feature set, adapted for SAK technician workflow

---

## 🎯 Executive Summary

The Advanced Uninstall Panel provides Revo Uninstaller-style deep application removal directly within S.A.K. Utility. It goes far beyond the standard Windows "Add/Remove Programs" — after running an application's built-in uninstaller, it performs multi-level leftover scanning to find and remove orphaned files, folders, and registry entries that the native uninstaller missed. The panel also supports forced uninstall (for programs with broken or missing uninstallers), UWP/Microsoft Store app removal, batch uninstallation, system restore point creation, and comprehensive reporting.

### Key Objectives
- ✅ **Program Enumeration** — List all installed Win32 programs (Registry) and UWP/Store apps (AppX) with rich metadata
- ✅ **Standard Uninstall** — Execute the application's built-in uninstaller, then scan for leftovers
- ✅ **Leftover Scanner** — Multi-level scanning for orphaned files, folders, and registry keys post-uninstall
- ✅ **Forced Uninstall** — Remove remnants of programs with broken/missing uninstallers or already-uninstalled programs
- ✅ **UWP App Removal** — Remove Microsoft Store apps and provisioned packages (bloatware)
- ✅ **Batch Uninstall** — Queue and sequentially uninstall multiple programs
- ✅ **System Restore Points** — Automatically create restore points before uninstallation for safety
- ✅ **Registry Snapshot & Diff** — Capture registry state before/after uninstall to identify leftover keys
- ✅ **Detailed Reporting** — Generate reports of what was found and removed, exportable to file
- ✅ **Safety First** — Confirmation dialogs, protected system paths, restore point integration, dry-run preview

---

## 📊 Project Scope

### What is Advanced Uninstall?

**Advanced Uninstall** is a power-user uninstallation tool for Windows that ensures complete program removal. Standard Windows uninstallers frequently leave behind:

- **Orphaned files and folders** in `Program Files`, `AppData`, `ProgramData`, temp directories
- **Registry entries** in `HKLM\SOFTWARE`, `HKCU\Software`, `HKLM\SYSTEM\CurrentControlSet\Services`
- **Scheduled tasks** created by the application
- **Windows Firewall rules** added during installation
- **Startup entries** in registry Run/RunOnce keys and Start Menu Startup folder
- **Shell extensions** and context menu handlers

Advanced Uninstall addresses all of these by performing structured scanning after the built-in uninstaller completes.

### How It Differs from Existing SAK Functionality

| Feature | Existing SAK | Advanced Uninstall Panel |
|---------|-------------|--------------------------|
| Program enumeration | `AppScanner` (read-only) | Full enumeration + metadata + size calculation |
| Chocolatey uninstall | `ChocolateyManager::uninstallPackage()` (backend only, no UI) | N/A (this panel handles Win32/UWP directly) |
| Bloatware detection | `CheckBloatwareAction` (report only, no removal) | Full UWP removal with confirmation UI |
| Registry scanning | `AppScanner::scanRegistry()` (read-only) | Post-uninstall diff scanning + cleanup |
| Leftover file removal | None | Multi-level file/folder scanning with safety checks |
| Restore points | None | Automatic creation before uninstallation |
| Batch operation | `AppInstallationPanel` (batch install) | Batch uninstall with queue management |

### Reusable SAK Components

The following existing components will be leveraged:

- **`AppScanner`** — Existing registry + AppX enumeration (`scanRegistry()`, `scanAppX()`, `scanChocolatey()`)
- **`WorkerBase`** — Background thread execution pattern
- **`LogToggleSwitch` + `DetachableLogWindow`** — Log output infrastructure
- **`ConfigManager`** — Settings persistence
- **`style_constants.h`** — UI theme compliance (colors, margins, fonts, button styles)
- **`CheckBloatwareAction`** — Bloatware pattern database (~40 known patterns)

---

## 🎯 Use Cases

### 1. **Complete Application Removal**
**Scenario**: Technician needs to fully remove a stubborn antivirus or VPN client that left files and registry entries behind after its built-in uninstaller ran.

**Workflow**:
1. Open Advanced Uninstall Panel
2. Search for the application in the program list (filter/search box)
3. Select the application → Right-click → "Uninstall"
4. SAK creates a system restore point (automatic safety measure)
5. SAK takes a registry snapshot (before state)
6. SAK launches the application's built-in uninstaller
7. User completes the native uninstall wizard
8. SAK detects uninstaller completion → prompts "Scan for leftovers?"
9. User selects scan level: Safe / Moderate / Advanced
10. Scanner finds: 47 registry keys, 23 files, 8 folders
11. Results tree shows all leftovers with color-coded safety indicators
12. User reviews, unchecks any items to keep, clicks "Delete Selected"
13. SAK removes all checked items and logs everything

**Benefits**:
- Guaranteed clean removal — no hidden files or registry pollution
- Restore point provides safety net
- Technician controls exactly what gets deleted

---

### 2. **Forced Uninstall of Broken Program**
**Scenario**: Program's uninstaller is corrupted or missing — "Add/Remove Programs" shows the entry but clicking Uninstall does nothing or errors.

**Workflow**:
1. Open Advanced Uninstall Panel
2. Select the broken program entry
3. Right-click → "Forced Uninstall"
4. SAK creates a restore point
5. SAK scans for all traces:
   - Registry entries matching the program name/publisher
   - Files in Program Files, AppData, ProgramData matching install path patterns
   - Services registered by the application
   - Scheduled tasks containing the program name
   - Firewall rules referencing the program
6. Results tree shows all discovered traces
7. User reviews findings, confirms deletion
8. SAK removes all traces and cleans up the broken registry entry

**Benefits**:
- Handles the #1 most frustrating Windows maintenance issue
- No need for manual registry editing or folder hunting
- Comprehensive scan covers all common leftover locations

---

### 3. **Bulk Bloatware Removal on New PC**
**Scenario**: New OEM laptop arrived with 30+ pre-installed apps (Candy Crush, McAfee trial, HP bloatware, Xbox apps, etc.).

**Workflow**:
1. Open Advanced Uninstall Panel
2. Click "UWP Apps" tab to see all Microsoft Store apps
3. Click "Show Bloatware Only" filter (uses `CheckBloatwareAction` pattern database)
4. 18 bloatware apps highlighted with safety classification
5. Select all → "Batch Uninstall"
6. Confirmation dialog shows total estimated space savings
7. SAK creates one restore point, then removes all selected UWP apps
8. Progress bar shows completion: "Removed 18/18 apps — Recovered 2.4 GB"

**Benefits**:
- New PC setup reduced from 30 minutes of manual clicking to 2 minutes
- Known-safe bloatware patterns prevent accidental removal of system apps
- Single restore point covers entire batch operation

---

### 4. **Pre-Migration Cleanup**
**Scenario**: Before migrating a user profile to a new PC, technician needs to uninstall deprecated software that won't be needed on the new machine.

**Workflow**:
1. Open Advanced Uninstall Panel
2. Sort program list by "Install Date" to see oldest installations
3. Filter by publisher to find all "Adobe" or "Autodesk" entries
4. Select deprecated versions → Add to uninstall queue
5. Review queue → "Start Batch Uninstall"
6. SAK processes queue sequentially, running each native uninstaller + leftover scan
7. Export final report as text file for documentation

**Benefits**:
- Clean profile means faster backup and restore via Backup & Restore panel
- Removes registry bloat that slows down profile migration
- Report serves as documentation for IT records

---

### 5. **Orphaned Entry Cleanup**
**Scenario**: "Add/Remove Programs" shows entries for programs that were already uninstalled or manually deleted, creating clutter.

**Workflow**:
1. Open Advanced Uninstall Panel
2. Click "Detect Orphaned Entries" tool
3. SAK scans all registry uninstall entries and checks if:
   - Install location directory still exists
   - Uninstall executable still exists
   - Program files are actually present
4. Results show 7 orphaned entries with "Install Path Missing" indicator
5. Select all → "Remove Entries" (only removes registry entries, no file operations)

**Benefits**:
- Cleans up "ghost" entries in Add/Remove Programs
- Safe operation — only removes registry stubs, cannot affect running software
- Reduces confusion when managing installed programs

---

### 6. **Service and Startup Cleanup**
**Scenario**: After uninstalling a program, its Windows service and startup entry remain, consuming boot time and resources.

**Workflow**:
1. Uninstall program via Standard Uninstall (Use Case 1)
2. During leftover scan, SAK's "Advanced" scan level detects:
   - Orphaned Windows service with `ImagePath` pointing to deleted executable
   - Startup registry entry in `HKCU\...\Run` referencing the uninstalled program
   - Scheduled task that would have been run by the program
3. User selects these items → "Delete Selected"
4. SAK stops and removes the orphaned service, deletes the Run entry, removes the scheduled task
5. Next reboot is cleaner — no error popups from missing executables

**Benefits**:
- Eliminates post-uninstall "file not found" errors at boot
- Reduces boot time by removing dead startup entries
- Service cleanup prevents resource leaks

---

## 🏗️ Architecture Overview

### Component Hierarchy

```
AdvancedUninstallPanel (QWidget) — Main UI panel
├─ AdvancedUninstallController (QObject)
│  ├─ State: Idle / Scanning / Uninstalling / LeftoverScanning / Cleaning
│  ├─ Manages: All worker threads
│  ├─ Coordinates: Uninstall → Leftover Scan → Cleanup pipeline
│  └─ Persists: Settings, uninstall history, queue state
│
├─ ProgramEnumerator (QObject) [Worker Thread]
│  ├─ Win32 Registry enumeration (HKLM + HKCU + WOW6432Node)
│  ├─ UWP/AppX package enumeration (PowerShell)
│  ├─ Program size calculation (install directory)
│  ├─ Icon extraction from executable
│  ├─ Orphaned entry detection (install path missing)
│  └─ Output: QVector<ProgramInfo>
│
├─ UninstallWorker (WorkerBase) [Worker Thread]
│  ├─ System restore point creation (WMI / PowerShell)
│  ├─ Registry snapshot (before state)
│  ├─ Native uninstaller execution (CreateProcess / ShellExecute)
│  ├─ Uninstaller completion detection (process exit monitoring)
│  ├─ MSI uninstall via msiexec.exe /x {GUID}
│  ├─ UWP removal via Remove-AppxPackage
│  ├─ Provisioned UWP removal via Remove-AppxProvisionedPackage
│  └─ Output: UninstallResult (success/failed/cancelled)
│
├─ LeftoverScanner (WorkerBase) [Worker Thread]
│  ├─ Three scan levels: Safe / Moderate / Advanced
│  ├─ File System Scanning
│  │  ├─ Program Files / Program Files (x86)
│  │  ├─ %AppData% (Roaming + Local + LocalLow)
│  │  ├─ %ProgramData%
│  │  ├─ %TEMP% / Windows\Temp
│  │  ├─ Start Menu\Programs
│  │  ├─ Desktop shortcuts
│  │  └─ Common Files
│  │
│  ├─ Registry Scanning
│  │  ├─ HKLM\SOFTWARE (and WOW6432Node)
│  │  ├─ HKCU\Software
│  │  ├─ HKLM\SYSTEM\CurrentControlSet\Services
│  │  ├─ HKCU\...\Run and RunOnce
│  │  ├─ HKLM\...\Run and RunOnce
│  │  ├─ HKCR (file associations, shell extensions)
│  │  ├─ HKLM\SOFTWARE\Classes\CLSID
│  │  └─ Registry diff (before/after snapshot comparison)
│  │
│  ├─ System Object Scanning (Advanced level only)
│  │  ├─ Windows Services (sc query / registry)
│  │  ├─ Scheduled Tasks (schtasks /query)
│  │  ├─ Windows Firewall rules (netsh advfirewall)
│  │  └─ Shell extensions (HKCR\*\shellex)
│  │
│  ├─ Safety Classification
│  │  ├─ Protected system paths list
│  │  ├─ Shared component detection (reference counting)
│  │  ├─ Color-coded risk: Green (safe) / Yellow (review) / Red (risky)
│  │  └─ Default selection: only Green items pre-checked
│  │
│  └─ Output: QVector<LeftoverItem>
│
├─ CleanupWorker (WorkerBase) [Worker Thread]
│  ├─ File/folder deletion with error handling
│  ├─ Registry key/value deletion
│  ├─ Service removal (sc delete)
│  ├─ Scheduled task removal (schtasks /delete)
│  ├─ Firewall rule removal (netsh)
│  ├─ Rollback on failure (best-effort)
│  └─ Output: CleanupReport
│
└─ RestorePointManager (QObject)
   ├─ Create restore point via WMI (SystemRestore class)
   ├─ Enumerate existing restore points
   ├─ Check if System Restore is enabled
   └─ Estimated restore point creation time
```

---

## 🛠️ Technical Specifications

### Core Data Structures

```cpp
/// @brief Comprehensive information about an installed program
struct ProgramInfo {
    // Identity
    QString displayName;            ///< Application display name
    QString publisher;              ///< Publisher/vendor
    QString displayVersion;         ///< Installed version string
    QString installDate;            ///< Installation date (YYYYMMDD or localized)

    // Paths
    QString installLocation;        ///< Installation directory
    QString uninstallString;        ///< Uninstall command line
    QString quietUninstallString;   ///< Silent uninstall command (if available)
    QString modifyPath;             ///< Modify/repair command
    QString displayIcon;            ///< Path to icon resource

    // Registry
    QString registryKeyPath;        ///< Full registry key path (for forced uninstall)

    // Metadata
    qint64 estimatedSizeKB = 0;     ///< Estimated size from registry (in KB)
    qint64 actualSizeBytes = 0;     ///< Calculated actual disk usage (bytes)

    // Classification
    enum class Source {
        RegistryHKLM,               ///< HKEY_LOCAL_MACHINE\...\Uninstall
        RegistryHKLM_WOW64,         ///< HKLM\...\WOW6432Node\...\Uninstall
        RegistryHKCU,               ///< HKEY_CURRENT_USER\...\Uninstall
        UWP,                        ///< Microsoft Store / AppX package
        Provisioned                 ///< Provisioned UWP (all-users)
    };
    Source source = Source::RegistryHKLM;

    // UWP-specific
    QString packageFamilyName;      ///< UWP package family name (for removal)
    QString packageFullName;        ///< UWP full package name

    // Status
    bool isSystemComponent = false; ///< WindowsInstaller SystemComponent flag
    bool isOrphaned = false;        ///< Install directory missing or uninstaller gone
    bool isBloatware = false;       ///< Matched bloatware pattern database

    // Icon cache
    QIcon cachedIcon;               ///< Extracted program icon
};

/// @brief Scan level for leftover detection
enum class ScanLevel {
    Safe,       ///< Only obvious leftovers in known locations (fast, safe)
    Moderate,   ///< Extended scanning with pattern matching (recommended)
    Advanced    ///< Deep scan including services, tasks, firewall, shell extensions
};

/// @brief A single leftover item found after uninstallation
struct LeftoverItem {
    enum class Type {
        File,
        Folder,
        RegistryKey,
        RegistryValue,
        Service,
        ScheduledTask,
        FirewallRule,
        StartupEntry,
        ShellExtension
    };

    enum class RiskLevel {
        Safe,       ///< Green — clearly belongs to the uninstalled app
        Review,     ///< Yellow — likely belongs, but shared component possible
        Risky       ///< Red — may be shared or system-related
    };

    Type type;
    RiskLevel risk = RiskLevel::Safe;
    QString path;               ///< File path or registry key path
    QString description;        ///< Human-readable description
    qint64 sizeBytes = 0;       ///< Size for files/folders; 0 for registry
    bool selected = false;      ///< User selection state (Safe = pre-selected)

    // Registry-specific
    QString registryValueName;  ///< Non-empty for RegistryValue type
    QString registryValueData;  ///< Display data for the value
};

/// @brief Result of uninstall + leftover scan + cleanup pipeline
struct UninstallReport {
    QString programName;
    QString programVersion;
    QString programPublisher;

    // Timing
    QDateTime startTime;
    QDateTime endTime;

    // Restore
    bool restorePointCreated = false;
    QString restorePointName;

    // Uninstall phase
    enum class UninstallResult {
        Success,
        Failed,
        Cancelled,
        Skipped         ///< Forced uninstall — no native uninstaller run
    };
    UninstallResult uninstallResult = UninstallResult::Success;
    int nativeExitCode = 0;

    // Leftover scan phase
    ScanLevel scanLevel = ScanLevel::Moderate;
    QVector<LeftoverItem> foundLeftovers;

    // Cleanup phase
    int filesDeleted = 0;
    int foldersDeleted = 0;
    int registryKeysDeleted = 0;
    int registryValuesDeleted = 0;
    int servicesRemoved = 0;
    int tasksRemoved = 0;
    int firewallRulesRemoved = 0;
    int startupEntriesRemoved = 0;
    int failedDeletions = 0;
    qint64 totalSpaceRecovered = 0;

    QStringList errorLog;       ///< Errors encountered during cleanup
};

/// @brief Batch uninstall queue item
struct UninstallQueueItem {
    ProgramInfo program;
    ScanLevel scanLevel = ScanLevel::Moderate;
    bool autoCleanSafeLeftovers = true;  ///< Auto-delete green items

    // Post-process state
    enum class Status {
        Queued,
        InProgress,
        Completed,
        Failed,
        Cancelled
    };
    Status status = Status::Queued;
    UninstallReport report;
};
```

### Program Enumerator

**Purpose**: Enumerate all installed programs from all sources with rich metadata.

```cpp
class ProgramEnumerator : public QObject {
    Q_OBJECT
public:
    explicit ProgramEnumerator(QObject* parent = nullptr);
    ~ProgramEnumerator() override;

    ProgramEnumerator(const ProgramEnumerator&) = delete;
    ProgramEnumerator& operator=(const ProgramEnumerator&) = delete;
    ProgramEnumerator(ProgramEnumerator&&) = delete;
    ProgramEnumerator& operator=(ProgramEnumerator&&) = delete;

    /// @brief Start async enumeration of all installed programs
    void enumerateAll();

    /// @brief Get the last enumeration result (cached)
    [[nodiscard]] QVector<ProgramInfo> programs() const;

    /// @brief Detect orphaned entries (install path or uninstaller missing)
    void detectOrphaned(QVector<ProgramInfo>& programs);

    /// @brief Mark known bloatware using pattern database
    void markBloatware(QVector<ProgramInfo>& programs);

    /// @brief Calculate actual disk usage for a program's install directory
    [[nodiscard]] static qint64 calculateDirSize(const QString& path);

Q_SIGNALS:
    void enumerationStarted();
    void enumerationProgress(int current, int total);
    void enumerationFinished(QVector<ProgramInfo> programs);
    void enumerationFailed(const QString& error);

private:
    /// @brief Scan Win32 programs from registry
    QVector<ProgramInfo> scanRegistryPrograms();

    /// @brief Scan single registry hive
    QVector<ProgramInfo> scanRegistryHive(HKEY hive, const QString& subkey,
                                           ProgramInfo::Source source);

    /// @brief Read a single registry string value
    [[nodiscard]] QString readRegString(HKEY key, const wchar_t* valueName);

    /// @brief Read a single registry DWORD value
    [[nodiscard]] DWORD readRegDword(HKEY key, const wchar_t* valueName);

    /// @brief Scan UWP/AppX packages via PowerShell
    QVector<ProgramInfo> scanUwpPackages();

    /// @brief Scan provisioned (all-users) UWP packages
    QVector<ProgramInfo> scanProvisionedPackages();

    /// @brief Extract icon from executable file
    [[nodiscard]] static QIcon extractIcon(const QString& path);

    /// @brief Check if entry is a system component (filter out)
    [[nodiscard]] static bool isSystemComponent(HKEY key);

    // Registry paths
    static constexpr const wchar_t* kUninstallKey64 =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    static constexpr const wchar_t* kUninstallKeyWow64 =
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

    QVector<ProgramInfo> m_cachedPrograms;
};
```

**Registry Value Mapping** (per uninstall subkey):

| Registry Value | ProgramInfo Field | Notes |
|----------------|-------------------|-------|
| `DisplayName` | `displayName` | Required — skip entry if missing |
| `DisplayVersion` | `displayVersion` | Optional |
| `Publisher` | `publisher` | Optional |
| `InstallDate` | `installDate` | YYYYMMDD format |
| `InstallLocation` | `installLocation` | May be empty |
| `UninstallString` | `uninstallString` | Required for standard uninstall |
| `QuietUninstallString` | `quietUninstallString` | For silent uninstall |
| `ModifyPath` | `modifyPath` | Modify/repair command |
| `DisplayIcon` | `displayIcon` | Icon resource path |
| `EstimatedSize` | `estimatedSizeKB` | Size in KB (DWORD) |
| `SystemComponent` | `isSystemComponent` | If 1 → filter from default view |
| `WindowsInstaller` | — | If 1 → use `msiexec /x` for uninstall |

### Uninstall Worker

**Purpose**: Execute the uninstall pipeline on a background thread.

```cpp
class UninstallWorker : public WorkerBase {
    Q_OBJECT
public:
    /// @brief Uninstall mode
    enum class Mode {
        Standard,       ///< Run native uninstaller + leftover scan
        ForcedUninstall,///< Skip native uninstaller, scan + remove all traces
        UwpRemove,      ///< Remove UWP package via PowerShell
        RegistryOnly    ///< Only remove the registry uninstall entry (orphaned cleanup)
    };

    explicit UninstallWorker(const ProgramInfo& program, Mode mode,
                             ScanLevel scanLevel, QObject* parent = nullptr);
    ~UninstallWorker() override = default;

    UninstallWorker(const UninstallWorker&) = delete;
    UninstallWorker& operator=(const UninstallWorker&) = delete;
    UninstallWorker(UninstallWorker&&) = delete;
    UninstallWorker& operator=(UninstallWorker&&) = delete;

Q_SIGNALS:
    /// @brief Native uninstaller has been launched — waiting for completion
    void nativeUninstallerStarted(const QString& programName);

    /// @brief Native uninstaller completed
    void nativeUninstallerFinished(int exitCode);

    /// @brief Registry snapshot captured (before state)
    void registrySnapshotCaptured();

    /// @brief Restore point created
    void restorePointCreated(const QString& name);

    /// @brief Leftover scan started
    void leftoverScanStarted(ScanLevel level);

    /// @brief Leftover scan progress
    void leftoverScanProgress(const QString& currentPath, int found);

    /// @brief Leftover scan complete
    void leftoverScanFinished(QVector<LeftoverItem> leftovers);

    /// @brief Full uninstall pipeline complete
    void uninstallComplete(UninstallReport report);

protected:
    auto execute() -> std::expected<void, sak::error_code> override;

private:
    ProgramInfo m_program;
    Mode m_mode;
    ScanLevel m_scanLevel;

    // Pipeline stages
    [[nodiscard]] bool createRestorePoint();
    [[nodiscard]] bool captureRegistrySnapshot();
    [[nodiscard]] bool runNativeUninstaller();
    [[nodiscard]] QVector<LeftoverItem> scanLeftovers();
    [[nodiscard]] bool removeUwpPackage();
    [[nodiscard]] bool removeRegistryEntry();

    // Registry snapshot data
    QSet<QString> m_registrySnapshotBefore;

    // Helpers
    [[nodiscard]] bool isMsiInstaller() const;
    [[nodiscard]] QString buildMsiUninstallCommand() const;
    [[nodiscard]] QString extractGuidFromUninstallString() const;
};
```

**Execute Implementation Sketch**:

```cpp
auto UninstallWorker::execute() -> std::expected<void, sak::error_code> {
    UninstallReport report;
    report.programName = m_program.displayName;
    report.programVersion = m_program.displayVersion;
    report.programPublisher = m_program.publisher;
    report.startTime = QDateTime::currentDateTime();
    report.scanLevel = m_scanLevel;

    // Phase 1: Create restore point
    reportProgress(0, 100, "Creating system restore point...");
    if (createRestorePoint()) {
        report.restorePointCreated = true;
        report.restorePointName = QString("SAK: Before uninstall %1").arg(m_program.displayName);
        emit restorePointCreated(report.restorePointName);
    }

    if (checkStop()) return {};

    // Phase 2: Handle by mode
    switch (m_mode) {
    case Mode::Standard: {
        // 2a: Capture registry snapshot (before state)
        reportProgress(10, 100, "Capturing registry snapshot...");
        captureRegistrySnapshot();
        emit registrySnapshotCaptured();

        if (checkStop()) return {};

        // 2b: Run native uninstaller and wait for exit
        reportProgress(20, 100, "Running native uninstaller...");
        emit nativeUninstallerStarted(m_program.displayName);

        if (!runNativeUninstaller()) {
            report.uninstallResult = UninstallReport::UninstallResult::Failed;
            report.endTime = QDateTime::currentDateTime();
            emit uninstallComplete(report);
            return std::unexpected(sak::error_code::operation_failed);
        }

        report.uninstallResult = UninstallReport::UninstallResult::Success;
        emit nativeUninstallerFinished(report.nativeExitCode);
        break;
    }

    case Mode::ForcedUninstall:
        report.uninstallResult = UninstallReport::UninstallResult::Skipped;
        captureRegistrySnapshot();  // Still capture for diff, even without native uninstall
        break;

    case Mode::UwpRemove:
        reportProgress(20, 100, "Removing UWP package...");
        if (!removeUwpPackage()) {
            report.uninstallResult = UninstallReport::UninstallResult::Failed;
            report.endTime = QDateTime::currentDateTime();
            emit uninstallComplete(report);
            return std::unexpected(sak::error_code::operation_failed);
        }
        report.uninstallResult = UninstallReport::UninstallResult::Success;
        report.endTime = QDateTime::currentDateTime();
        emit uninstallComplete(report);
        return {};  // UWP — no leftover scan needed

    case Mode::RegistryOnly:
        reportProgress(50, 100, "Removing orphaned registry entry...");
        if (!removeRegistryEntry()) {
            return std::unexpected(sak::error_code::operation_failed);
        }
        report.uninstallResult = UninstallReport::UninstallResult::Success;
        report.registryKeysDeleted = 1;
        report.endTime = QDateTime::currentDateTime();
        emit uninstallComplete(report);
        return {};
    }

    if (checkStop()) return {};

    // Phase 3: Leftover scanning
    reportProgress(40, 100, "Scanning for leftovers...");
    emit leftoverScanStarted(m_scanLevel);

    auto leftovers = scanLeftovers();
    report.foundLeftovers = leftovers;

    emit leftoverScanFinished(leftovers);

    report.endTime = QDateTime::currentDateTime();
    emit uninstallComplete(report);

    return {};
}
```

### Leftover Scanner

**Purpose**: Multi-level scanning for orphaned files, folders, and registry entries.

```cpp
class LeftoverScanner {
public:
    explicit LeftoverScanner(const ProgramInfo& program, ScanLevel level);

    /// @brief Run the full leftover scan
    /// @param registrySnapshotBefore Registry keys captured before uninstall
    /// @param stopRequested Atomic flag for cancellation
    /// @param progressCallback Called with (currentPath, foundCount)
    [[nodiscard]] QVector<LeftoverItem> scan(
        const QSet<QString>& registrySnapshotBefore,
        const std::atomic<bool>& stopRequested,
        std::function<void(const QString&, int)> progressCallback = {});

private:
    ProgramInfo m_program;
    ScanLevel m_level;

    // Search patterns derived from program info
    QStringList m_namePatterns;     ///< Program name variations for matching
    QStringList m_publisherPatterns;///< Publisher name variations
    QString m_installDirName;       ///< Last component of install path

    /// @brief Generate search patterns from program info
    void buildSearchPatterns();

    // File system scanning
    QVector<LeftoverItem> scanFileSystem();
    QVector<LeftoverItem> scanDirectory(const QString& basePath,
                                         LeftoverItem::RiskLevel defaultRisk);

    // Registry scanning
    QVector<LeftoverItem> scanRegistry();
    QVector<LeftoverItem> scanRegistryHive(HKEY hive, const QString& subkey,
                                            const QString& hiveName);
    QVector<LeftoverItem> diffRegistry(const QSet<QString>& snapshotBefore);

    // System object scanning (Advanced level)
    QVector<LeftoverItem> scanServices();
    QVector<LeftoverItem> scanScheduledTasks();
    QVector<LeftoverItem> scanFirewallRules();
    QVector<LeftoverItem> scanStartupEntries();

    // Safety classification
    LeftoverItem::RiskLevel classifyRisk(const QString& path,
                                          LeftoverItem::Type type) const;
    bool isProtectedPath(const QString& path) const;
    bool matchesProgram(const QString& name) const;

    // Protected system paths that should NEVER be deleted
    static const QStringList kProtectedPaths;

    /// @brief Calculate directory size recursively
    [[nodiscard]] static qint64 calculateSize(const QString& path);
};
```

**Scan Locations by Level**:

| Location | Safe | Moderate | Advanced |
|----------|------|----------|----------|
| `%ProgramFiles%\<app>` | ✅ | ✅ | ✅ |
| `%ProgramFiles(x86)%\<app>` | ✅ | ✅ | ✅ |
| `%AppData%\<app>` | ✅ | ✅ | ✅ |
| `%LocalAppData%\<app>` | ✅ | ✅ | ✅ |
| `%ProgramData%\<app>` | ✅ | ✅ | ✅ |
| `%TEMP%\<app>*` | ✅ | ✅ | ✅ |
| Start Menu shortcuts | ✅ | ✅ | ✅ |
| Desktop shortcuts | ✅ | ✅ | ✅ |
| `HKCU\Software\<publisher>` | — | ✅ | ✅ |
| `HKLM\SOFTWARE\<publisher>` | — | ✅ | ✅ |
| `HKCR\<app>.*` (file assoc.) | — | ✅ | ✅ |
| Registry diff (before/after) | — | ✅ | ✅ |
| `HKLM\...\Services\<app>` | — | — | ✅ |
| Scheduled Tasks | — | — | ✅ |
| Firewall Rules | — | — | ✅ |
| Startup entries (Run/RunOnce) | — | — | ✅ |
| Shell extensions (CLSID) | — | — | ✅ |
| `%CommonFiles%\<publisher>` | — | — | ✅ |

**Protected Path List** (never auto-selected, always marked Red):

```cpp
const QStringList LeftoverScanner::kProtectedPaths = {
    "C:\\Windows",
    "C:\\Windows\\System32",
    "C:\\Windows\\SysWOW64",
    "C:\\Windows\\WinSxS",
    "C:\\Program Files\\Common Files\\Microsoft Shared",
    "C:\\Program Files\\Windows",
    "C:\\ProgramData\\Microsoft",
    "C:\\Users\\Default",
    "HKLM\\SOFTWARE\\Microsoft\\Windows",
    "HKLM\\SOFTWARE\\Microsoft\\Windows NT",
    "HKLM\\SYSTEM\\CurrentControlSet\\Control",
    "HKLM\\SYSTEM\\CurrentControlSet\\Enum",
    "HKCR\\CLSID\\{00000000-",  // System CLSIDs
};
```

### Cleanup Worker

**Purpose**: Delete selected leftover items safely on a background thread.

```cpp
class CleanupWorker : public WorkerBase {
    Q_OBJECT
public:
    explicit CleanupWorker(const QVector<LeftoverItem>& selectedItems,
                           QObject* parent = nullptr);
    ~CleanupWorker() override = default;

    CleanupWorker(const CleanupWorker&) = delete;
    CleanupWorker& operator=(const CleanupWorker&) = delete;
    CleanupWorker(CleanupWorker&&) = delete;
    CleanupWorker& operator=(CleanupWorker&&) = delete;

Q_SIGNALS:
    void itemCleaned(const QString& path, bool success);
    void cleanupComplete(int succeeded, int failed, qint64 bytesRecovered);

protected:
    auto execute() -> std::expected<void, sak::error_code> override;

private:
    QVector<LeftoverItem> m_items;

    [[nodiscard]] bool deleteFile(const QString& path);
    [[nodiscard]] bool deleteFolder(const QString& path);
    [[nodiscard]] bool deleteRegistryKey(const QString& fullKeyPath);
    [[nodiscard]] bool deleteRegistryValue(const QString& keyPath, const QString& valueName);
    [[nodiscard]] bool removeService(const QString& serviceName);
    [[nodiscard]] bool removeScheduledTask(const QString& taskName);
    [[nodiscard]] bool removeFirewallRule(const QString& ruleName);
};
```

### Restore Point Manager

**Purpose**: Create system restore points and check System Restore availability.

```cpp
class RestorePointManager : public QObject {
    Q_OBJECT
public:
    explicit RestorePointManager(QObject* parent = nullptr);

    /// @brief Check if System Restore is enabled on the system drive
    [[nodiscard]] bool isSystemRestoreEnabled() const;

    /// @brief Create a system restore point
    /// @param description Description for the restore point
    /// @return true if created successfully
    [[nodiscard]] bool createRestorePoint(const QString& description);

    /// @brief Get list of existing restore points
    [[nodiscard]] QVector<QPair<QDateTime, QString>> listRestorePoints() const;

Q_SIGNALS:
    void restorePointCreated(const QString& description);
    void restorePointFailed(const QString& error);
};
```

**Implementation via PowerShell** (most reliable on Windows 10/11):

```cpp
bool RestorePointManager::createRestorePoint(const QString& description) {
    // PowerShell command to create restore point
    // Requires elevation (SAK typically runs as admin for technician tasks)
    QProcess powershell;
    powershell.setProgram("powershell.exe");
    powershell.setArguments({
        "-NoProfile", "-NonInteractive", "-Command",
        QString("Checkpoint-Computer -Description '%1' -RestorePointType 'APPLICATION_UNINSTALL'")
            .arg(description.left(64))  // Max 64 chars for description
    });
    powershell.start();

    // Wait up to 60 seconds for restore point creation
    if (!powershell.waitForFinished(60000)) {
        emit restorePointFailed("Timeout creating restore point");
        return false;
    }

    if (powershell.exitCode() != 0) {
        const QString err = QString::fromUtf8(powershell.readAllStandardError());
        emit restorePointFailed(err);
        return false;
    }

    emit restorePointCreated(description);
    return true;
}

bool RestorePointManager::isSystemRestoreEnabled() const {
    QProcess powershell;
    powershell.setProgram("powershell.exe");
    powershell.setArguments({
        "-NoProfile", "-NonInteractive", "-Command",
        "(Get-ComputerRestorePoint -ErrorAction SilentlyContinue) -ne $null; "
        "$?"
    });
    powershell.start();
    powershell.waitForFinished(10000);

    // If it returns without error, System Restore is enabled
    return powershell.exitCode() == 0;
}
```

### Registry Snapshot Engine

**Purpose**: Capture registry key inventory before uninstall, then diff after to find new/changed keys.

```cpp
class RegistrySnapshotEngine {
public:
    /// @brief Capture a snapshot of registry keys under monitored paths
    /// @return Set of full key paths (e.g., "HKLM\SOFTWARE\CompanyName\Product")
    [[nodiscard]] static QSet<QString> captureSnapshot();

    /// @brief Diff two snapshots to find removed keys (leftover candidates)
    /// @param before Snapshot before uninstall
    /// @param after Snapshot after uninstall  
    /// @return Keys present in 'before' but not in 'after' = removed by uninstaller (expected).
    ///         Keys present in 'after' but not in 'before' = added during uninstall (rare, suspicious).
    ///         Keys present in both but unchanged = potential leftovers (need pattern matching).
    [[nodiscard]] static QVector<LeftoverItem> diffSnapshots(
        const QSet<QString>& before, const QSet<QString>& after,
        const QStringList& programNamePatterns);

private:
    /// @brief Enumerate all subkeys under a registry path
    static void enumerateKeys(HKEY hive, const QString& subkey,
                               const QString& hiveName, QSet<QString>& output,
                               int maxDepth = 3);

    // Monitored paths for snapshot
    static const QStringList kMonitoredPaths;
};
```

**Monitored Registry Paths**:
```cpp
const QStringList RegistrySnapshotEngine::kMonitoredPaths = {
    // HKLM paths
    "HKLM\\SOFTWARE",
    "HKLM\\SOFTWARE\\WOW6432Node",
    "HKLM\\SYSTEM\\CurrentControlSet\\Services",

    // HKCU paths
    "HKCU\\Software",

    // HKCR paths (file associations, shell extensions)
    "HKCR",
};
```

---

## 🎨 User Interface Design

### Advanced Uninstall Panel Layout

The panel uses a toolbar + split view: program list on top, leftover results on bottom (when scanning).

```
┌──────────────────────────────────────────────────────────────────────┐
│ Advanced Uninstall Panel                                             │
├──────────────────────────────────────────────────────────────────────┤
│                                                                      │
│  ┌── TOOLBAR ───────────────────────────────────────────────────┐   │
│  │                                                               │   │
│  │  Search: [________________🔍]   View: [All Programs ▼]       │   │
│  │                                                               │   │
│  │  [🔄 Refresh]  [Uninstall]  [Forced Uninstall]  [Batch...]  │   │
│  │                                                               │   │
│  └───────────────────────────────────────────────────────────────┘   │
│                                                                      │
│  ┌── PROGRAM LIST ──────────────────────────────────────────────┐   │
│  │                                                               │   │
│  │  ☐ │ Icon │ Name              │ Publisher    │ Version │ Size │   │
│  │  ──┼──────┼───────────────────┼──────────────┼─────────┼──────│   │
│  │  ☐ │ 🔷  │ Google Chrome      │ Google LLC   │ 131.0   │ 1.2G│   │
│  │  ☐ │ 🔷  │ Mozilla Firefox    │ Mozilla      │ 134.0   │ 498M│   │
│  │  ☐ │ 🔷  │ Visual Studio Code │ Microsoft    │ 1.96    │ 412M│   │
│  │  ☑ │ ⚠️  │ Norton Security    │ NortonLifeL │ 22.24   │ 2.1G│   │
│  │  ☐ │ 🔷  │ 7-Zip              │ Igor Pavlov  │ 24.09   │  5M │   │
│  │  ☐ │ 🔷  │ VLC media player   │ VideoLAN     │ 3.0.21  │ 189M│   │
│  │  ☐ │ 💀  │ BonziBuddy        │ BONZI.COM    │ 4.0     │  ??? │   │
│  │  ...                                                          │   │
│  │                                                               │   │
│  │  [ 142 programs ] [ 32.4 GB total ]                          │   │
│  │                                                               │   │
│  └───────────────────────────────────────────────────────────────┘   │
│                                                                      │
│  ┌── LEFTOVER RESULTS (shown after scan) ───────────────────────┐   │
│  │                                                               │   │
│  │  Leftovers for "Norton Security" — Scan: Moderate             │   │
│  │  Found: 47 registry keys, 23 files, 8 folders (312 MB)       │   │
│  │                                                               │   │
│  │  ☑ │ Risk │ Type     │ Path                          │  Size │   │
│  │  ──┼──────┼──────────┼───────────────────────────────┼───────│   │
│  │  ☑ │ 🟢  │ Folder   │ C:\ProgramData\Norton         │ 245MB │   │
│  │  ☑ │ 🟢  │ Folder   │ C:\Users\...\AppData\Norton   │  42MB │   │
│  │  ☑ │ 🟢  │ File     │ C:\Users\...\Desktop\Norton.lnk│   1K │   │
│  │  ☑ │ 🟢  │ RegKey   │ HKLM\SOFTWARE\Norton           │    — │   │
│  │  ☑ │ 🟢  │ RegKey   │ HKCU\Software\Norton           │    — │   │
│  │  ☐ │ 🟡  │ RegKey   │ HKLM\SOFTWARE\Symantec         │    — │   │
│  │  ☐ │ 🔴  │ Service  │ NortonSecurity (stopped)        │    — │   │
│  │  ...                                                          │   │
│  │                                                               │   │
│  │  [Select All Safe] [Deselect All] [Delete Selected (298 MB)] │   │
│  │                                                               │   │
│  └───────────────────────────────────────────────────────────────┘   │
│                                                                      │
│  ┌── STATUS BAR ────────────────────────────────────────────────┐   │
│  │ Ready — 142 programs loaded                     [█████████] │   │
│  └───────────────────────────────────────────────────────────────┘   │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘
```

### Toolbar Detail

```
┌────────────────────────────────────────────────────────────────────┐
│                                                                    │
│  Search: [___________________________🔍]                           │
│                                                                    │
│  View: [All Programs ▼]   Sort: [Name A-Z ▼]                     │
│         ├ All Programs                                             │
│         ├ Win32 Only                                               │
│         ├ UWP/Store Apps                                           │
│         ├ Bloatware Only                                           │
│         └ Orphaned Entries                                         │
│                                                                    │
│  [🔄 Refresh] [Uninstall] [Forced Uninstall] [Batch Uninstall]   │
│                                                                    │
└────────────────────────────────────────────────────────────────────┘
```

### Uninstall Confirmation Dialog

```
┌──────────────────────────────────────────────┐
│ Confirm Uninstall                       [X]  │
├──────────────────────────────────────────────┤
│                                              │
│  ⚠️ You are about to uninstall:              │
│                                              │
│  Norton Security v22.24.5                    │
│  Publisher: NortonLifeLock Inc.               │
│  Size: 2.1 GB                                │
│                                              │
│  Scan Level:                                 │
│  ○ Safe       (fast, known locations only)   │
│  ● Moderate   (recommended)                  │
│  ○ Advanced   (deep scan, slower)            │
│                                              │
│  ☑ Create system restore point first         │
│  ☑ Auto-clean safe (green) leftovers         │
│                                              │
│          [Uninstall]  [Cancel]               │
│                                              │
└──────────────────────────────────────────────┘
```

### Forced Uninstall Dialog

```
┌──────────────────────────────────────────────┐
│ Forced Uninstall                        [X]  │
├──────────────────────────────────────────────┤
│                                              │
│  ⚠️ Forced Uninstall will scan for ALL       │
│  traces of this program and remove them      │
│  without running the native uninstaller.     │
│                                              │
│  Program: Old Antivirus Pro v3.1             │
│                                              │
│  This is useful when:                        │
│  • The uninstaller is missing or broken      │
│  • The program was already partially removed │
│  • Add/Remove Programs shows a ghost entry   │
│                                              │
│  ☑ Create system restore point first         │
│                                              │
│  Scan Level:                                 │
│  ○ Moderate  (recommended)                   │
│  ● Advanced  (thorough, includes services)   │
│                                              │
│      [Force Uninstall]  [Cancel]             │
│                                              │
└──────────────────────────────────────────────┘
```

### Batch Uninstall Dialog

```
┌──────────────────────────────────────────────┐
│ Batch Uninstall Queue                   [X]  │
├──────────────────────────────────────────────┤
│                                              │
│  Queue (3 programs):                         │
│                                              │
│  1. Norton Security v22.24      [Remove]     │
│  2. Candy Crush Saga (UWP)      [Remove]     │
│  3. HP Support Assistant v9.0   [Remove]     │
│                                              │
│  Total estimated size: 3.8 GB                │
│                                              │
│  Settings:                                   │
│  Scan Level: [Moderate ▼]                    │
│  ☑ Create one restore point before start     │
│  ☑ Auto-clean safe leftovers                 │
│  ☐ Generate report file when complete        │
│                                              │
│      [Start Batch Uninstall]  [Cancel]       │
│                                              │
└──────────────────────────────────────────────┘
```

### Uninstall Progress View

```
┌──────────────────────────────────────────────┐
│ Uninstalling: Norton Security           [X]  │
├──────────────────────────────────────────────┤
│                                              │
│  ┌──────────────────────────────────────┐    │
│  │ ████████████████░░░░░░░░░░░░░░  45% │    │
│  └──────────────────────────────────────┘    │
│                                              │
│  Phase: Scanning for leftovers...            │
│  Scanning: HKLM\SOFTWARE\Symantec            │
│  Found: 32 leftover items                    │
│                                              │
│  ✅ 1. Created restore point                 │
│  ✅ 2. Captured registry snapshot            │
│  ✅ 3. Ran native uninstaller (exit: 0)      │
│  ⏳ 4. Scanning for leftovers...             │
│  ○  5. Review and clean leftovers            │
│                                              │
│                          [Cancel]            │
│                                              │
└──────────────────────────────────────────────┘
```

### Context Menu (Right-click on program)

```
┌──────────────────────────────────┐
│ Uninstall                        │
│ Forced Uninstall                 │
│ ─────────────────────────────── │
│ Add to Batch Queue               │
│ ─────────────────────────────── │
│ Open Install Location            │
│ Copy Program Name                │
│ Copy Uninstall Command           │
│ ─────────────────────────────── │
│ Properties...                    │
│ ─────────────────────────────── │
│ Remove Registry Entry Only       │  (shown only for orphaned items)
└──────────────────────────────────┘
```

### Program Properties Dialog

```
┌──────────────────────────────────────────────┐
│ Program Properties                      [X]  │
├──────────────────────────────────────────────┤
│                                              │
│  Name:           Norton Security             │
│  Version:        22.24.5.23                  │
│  Publisher:       NortonLifeLock Inc.         │
│  Install Date:   2024-08-15                  │
│  Install Path:   C:\Program Files\Norton     │
│  Registry Size:  2,150,400 KB                │
│  Actual Size:    2.1 GB (calculated)         │
│                                              │
│  Source:         Registry (HKLM 64-bit)      │
│  Registry Key:   SOFTWARE\Microsoft\Win...   │
│                                              │
│  Uninstall:      "C:\Program Files\Nor...    │
│  Silent:         "C:\Program Files\Nor...    │
│  Modify:         "C:\Program Files\Nor...    │
│                                              │
│  Status: ● Installed   ☐ System Component    │
│                                              │
│                             [Close]          │
│                                              │
└──────────────────────────────────────────────┘
```

---

## 📂 File Structure

### New Files to Create

#### Headers (`include/sak/`)

```
advanced_uninstall_panel.h               # Main UI panel (toolbar + tables + dialogs)
advanced_uninstall_controller.h          # Orchestrates uninstall pipeline
advanced_uninstall_types.h               # Shared types (ProgramInfo, LeftoverItem, etc.)
program_enumerator.h                     # Registry + AppX program enumeration
uninstall_worker.h                       # WorkerBase subclass for uninstall execution
leftover_scanner.h                       # Multi-level leftover detection
cleanup_worker.h                         # WorkerBase subclass for leftover deletion
restore_point_manager.h                  # System Restore point creation
registry_snapshot_engine.h               # Registry snapshot capture and diff
```

#### Implementation (`src/`)

```
gui/advanced_uninstall_panel.cpp         # Main UI, toolbar, program table, leftover table
gui/advanced_uninstall_panel_dialogs.cpp # Confirmation, forced uninstall, batch, properties dialogs
core/advanced_uninstall_controller.cpp   # Pipeline orchestration, queue management, history
core/program_enumerator.cpp              # Win32 + WOW64 + HKCU + UWP enumeration
core/uninstall_worker.cpp                # Native uninstaller execution, MSI handling
core/leftover_scanner.cpp                # File system + registry + services scanning
core/cleanup_worker.cpp                  # Delete files, folders, registry, services, tasks
core/restore_point_manager.cpp           # PowerShell-based restore point management
core/registry_snapshot_engine.cpp        # RegEnumKeyEx-based snapshot + diff
```

#### Tests (`tests/unit/`)

```
test_program_enumerator.cpp              # Registry enumeration, UWP listing
test_leftover_scanner.cpp                # Leftover detection logic, safety classification
test_registry_snapshot_engine.cpp        # Snapshot capture and diff
test_restore_point_manager.cpp           # Restore point availability check
test_advanced_uninstall_integration.cpp  # End-to-end pipeline (with mock program)
```

---

## 🔧 Third-Party Dependencies

### No new external dependencies required

All functionality is implemented using existing capabilities:

| Capability | API / Library | Status |
|------------|---------------|--------|
| Registry access | Win32 `RegOpenKeyExW` / `RegEnumKeyExW` / `RegQueryValueExW` | Already used in `AppScanner` |
| Process execution | `QProcess` / Win32 `CreateProcessW` | Already used throughout |
| PowerShell commands | `QProcess` → `powershell.exe` | Already used in actions |
| File system operations | `QFile` / `QDir` / `QDirIterator` | Qt 6.5 Core |
| Icon extraction | `SHGetFileInfoW` or `ExtractIconExW` | Win32 Shell API (already linked) |
| Service management | `sc.exe` via `QProcess` | System command |
| Scheduled tasks | `schtasks.exe` via `QProcess` | System command |
| Firewall rules | `netsh advfirewall` via `QProcess` | System command |
| UWP removal | `Remove-AppxPackage` via PowerShell | System command |
| Restore points | `Checkpoint-Computer` via PowerShell | System command |
| WMI queries | PowerShell `Get-WmiObject` / `Get-CimInstance` | System command |
| Table views | `QTableWidget` / `QTreeWidget` | Qt 6.5 Widgets |
| Worker threads | `WorkerBase` (SAK) | Already in codebase |
| Settings | `ConfigManager` (SAK) | Already in codebase |
| Logging | `LogToggleSwitch` + `DetachableLogWindow` (SAK) | Already in codebase |
| Style | `style_constants.h` (SAK) | Already in codebase |

This panel is **zero new dependencies** — it uses only Win32 APIs, PowerShell, and existing Qt/SAK infrastructure.

---

## 🔧 Implementation Phases

### Phase 1: Program Enumeration & UI Shell (Week 1-3)

**Goals**:
- Complete program enumeration from all sources
- Main panel UI with program table
- Search, filter, and sort functionality
- Icon extraction and display

**Tasks**:
1. Define all data structures in `advanced_uninstall_types.h`
2. Implement `ProgramEnumerator` class
   - Win32 registry scanning (HKLM 64-bit, HKLM WOW64, HKCU)
   - Read all registry values from the Uninstall key per program
   - Filter out `SystemComponent = 1` entries
   - UWP/AppX enumeration via `Get-AppxPackage` PowerShell
   - Provisioned package enumeration via `Get-AppxProvisionedPackage`
   - Icon extraction using `SHGetFileInfoW` or `ExtractIconExW`
   - Actual disk size calculation for install directories
   - Orphaned entry detection (install path missing, uninstaller missing)
   - Bloatware marking using patterns from `CheckBloatwareAction`
3. Create `AdvancedUninstallPanel` UI shell
   - Search bar with real-time filtering (`QLineEdit` with `textChanged`)
   - View filter dropdown (All / Win32 / UWP / Bloatware / Orphaned)
   - Sort by Name / Publisher / Version / Size / Install Date
   - `QTableWidget` with columns: Check, Icon, Name, Publisher, Version, Size, Date
   - Right-click context menu
   - Program properties dialog
   - Status bar showing count and total size
4. Wire `ProgramEnumerator` to panel with loading indicator
5. Add panel to `MainWindow::createToolPanels()`
6. Style compliance with `style_constants.h`
7. Write `test_program_enumerator.cpp`

**Acceptance Criteria**:
- ✅ All Win32 programs from HKLM, HKLM WOW64, and HKCU enumerated
- ✅ UWP apps listed with package names
- ✅ Program icons displayed in table
- ✅ Search filters by name, publisher, version in real-time
- ✅ View presets filter correctly (bloatware, orphaned, etc.)
- ✅ All sort modes work (name, publisher, size, date)
- ✅ Orphaned entries detected and flagged
- ✅ Panel matches SAK theme (colors, margins, fonts, button styles)
- ✅ Properties dialog shows all program metadata

---

### Phase 2: Standard Uninstall & Restore Points (Week 4-6)

**Goals**:
- System restore point creation
- Native uninstaller execution and monitoring
- MSI-based uninstall support
- UWP app removal

**Tasks**:
1. Implement `RestorePointManager`
   - Check System Restore enabled state
   - Create restore point via `Checkpoint-Computer`
   - List existing restore points
   - Handle non-admin scenarios (warn user if not elevated)
2. Implement `UninstallWorker` (Standard mode)
   - Parse uninstall string (handle quoted paths, arguments)
   - Detect MSI installers (contains `MsiExec` or has `WindowsInstaller = 1`)
   - Build `msiexec.exe /x {GUID}` command for MSI programs
   - Launch uninstaller via `QProcess` / `CreateProcessW`
   - Monitor process for exit (non-blocking wait)
   - Capture exit code
   - Handle silent uninstall option (use `QuietUninstallString` if available)
3. Implement UWP removal
   - `Remove-AppxPackage -Package <fullname>` for per-user apps
   - `Remove-AppxProvisionedPackage -Online -PackageName <name>` for provisioned
4. Implement registry-only removal (for orphaned entries)
   - `RegDeleteKeyExW` to remove the uninstall subkey
5. Create confirmation dialog UI
   - Scan level selector (Safe / Moderate / Advanced)
   - Restore point checkbox
   - Auto-clean safe leftovers checkbox
6. Create uninstall progress view UI
   - Phase checklist (restore → snapshot → uninstall → scan → clean)
   - Progress bar
   - Current activity label
7. Wire signals from `UninstallWorker` to UI updates
8. Write `test_restore_point_manager.cpp`

**Acceptance Criteria**:
- ✅ Restore point created successfully before uninstall
- ✅ Native uninstallers launch and run correctly
- ✅ MSI-based programs uninstall via `msiexec /x`
- ✅ UWP apps removed via PowerShell
- ✅ Orphaned registry entries removable
- ✅ Exit code captured and reported
- ✅ Progress UI updates in real-time
- ✅ Cancel stops the operation cleanly

---

### Phase 3: Leftover Scanner (Week 7-10)

**Goals**:
- Multi-level leftover scanning (Safe / Moderate / Advanced)
- Registry snapshot and diff engine
- File system scanning with pattern matching
- Safety classification (Green / Yellow / Red)

**Tasks**:
1. Implement `RegistrySnapshotEngine`
   - `captureSnapshot()` — enumerate all keys in monitored paths (depth-limited)
   - `diffSnapshots()` — find keys present in both snapshots (leftovers)
   - Performance: limit depth to 3 levels, skip large hives
2. Implement `LeftoverScanner`
   - Build search patterns from program name, publisher, install directory name
   - **Safe level**: Scan only obvious locations (Program Files, AppData, ProgramData, Start Menu, Desktop)
   - **Moderate level**: Add registry scanning (HKCU\Software, HKLM\SOFTWARE, HKCR), file associations
   - **Advanced level**: Add services, scheduled tasks, firewall rules, startup entries, shell extensions
   - Pattern matching: case-insensitive substring match on directory/key names
   - Directory size calculation for file/folder items
   - Safety classification:
     - Green (Safe): Exact name match in expected location
     - Yellow (Review): Partial match or shared publisher directory
     - Red (Risky): System path proximity or common shared component
   - Protected path enforcement (never mark system paths as safe)
3. Create leftover results UI
   - `QTableWidget` with columns: Check, Risk, Type, Path, Size
   - Color-coded risk indicators (green/yellow/red circles)
   - "Select All Safe" / "Deselect All" buttons
   - "Delete Selected" button with totalable size display
   - Expandable detail on click (shows full path, registry value data)
4. Wire leftover scanner to uninstall pipeline
   - After native uninstaller completes → auto-launch leftover scan
   - Prompt user to select scan level if not pre-configured
5. Write `test_leftover_scanner.cpp`
6. Write `test_registry_snapshot_engine.cpp`

**Acceptance Criteria**:
- ✅ Registry snapshot captures keys in < 5 seconds
- ✅ Registry diff identifies leftover keys accurately
- ✅ File system scan finds orphaned directories in all expected locations
- ✅ Safe scan level runs in < 10 seconds
- ✅ Moderate scan level runs in < 30 seconds
- ✅ Advanced scan level runs in < 60 seconds
- ✅ Safety classification correctly grades items (Green/Yellow/Red)
- ✅ Protected paths never auto-selected
- ✅ Services, tasks, and firewall rules detected at Advanced level
- ✅ Results table displays correctly with risk colors

---

### Phase 4: Cleanup & Forced Uninstall (Week 11-13)

**Goals**:
- Delete selected leftover items safely
- Forced uninstall mode (no native uninstaller)
- Error handling and partial failure recovery

**Tasks**:
1. Implement `CleanupWorker`
   - File deletion: `QFile::remove()` with error handling
   - Folder deletion: `QDir::removeRecursively()` with locked file detection
   - Registry key deletion: `RegDeleteKeyExW` with backup
   - Registry value deletion: `RegDeleteValueW`
   - Service removal: `sc stop <service>` then `sc delete <service>`
   - Scheduled task removal: `schtasks /delete /tn <name> /f`
   - Firewall rule removal: `netsh advfirewall firewall delete rule name="<name>"`
   - Progress reporting per item
   - Error logging for failed items (continue on failure, don't abort)
   - Summary: succeeded / failed / bytes recovered
2. Implement Forced Uninstall mode
   - Skip native uninstaller entirely
   - Run leftover scanner with broader pattern matching
   - Include the program's uninstall registry key itself as a leftover item
   - Support "Forced Uninstall" for programs where uninstall string is missing/broken
3. Create forced uninstall confirmation dialog
4. Implement batch uninstall
   - Queue management (add, remove, reorder)
   - Batch uninstall dialog UI
   - Sequential processing: for each queued program, run full pipeline
   - Single restore point for entire batch
   - Progress: "Uninstalling 3/5: Norton Security..."
   - Stop on failure option vs. continue
5. Create `UninstallReport` generation
   - Summary of what was found and removed
   - Export to text file option
6. Write `test_advanced_uninstall_integration.cpp`

**Acceptance Criteria**:
- ✅ File and folder deletion works with error handling
- ✅ Registry keys and values deleted correctly
- ✅ Services stopped and removed
- ✅ Scheduled tasks removed
- ✅ Locked file detection (report, don't crash)
- ✅ Forced uninstall finds traces without native uninstaller
- ✅ Batch uninstall processes queue sequentially
- ✅ Report generated with accurate counts and sizes

---

### Phase 5: Controller & Integration (Week 14-15)

**Goals**:
- Full pipeline orchestration
- Settings persistence
- Uninstall history
- MainWindow integration

**Tasks**:
1. Implement `AdvancedUninstallController`
   - State machine: Idle → Scanning → Uninstalling → LeftoverScanning → Cleaning → Idle
   - Pipeline coordination: connect uninstall → leftover scan → cleanup
   - Uninstall history: save reports to JSON for review
   - Queue persistence: save/load batch queue
2. ConfigManager integration
   - Default scan level
   - Auto-create restore points (on/off)
   - Auto-clean safe leftovers (on/off)
   - Show system components (on/off)
   - Sort order and column widths
3. Add panel to `MainWindow::createToolPanels()`
   - Add `#include "sak/advanced_uninstall_panel.h"`
   - Add `std::unique_ptr<AdvancedUninstallPanel>` member
   - Register tab with tooltip and keyboard shortcut
4. Add all new source files to `CMakeLists.txt`
5. Wire `statusMessage` and `progressUpdate` signals
6. Connect `LogToggleSwitch` for log output
7. Test full build (0 errors, 0 warnings)
8. Run all existing tests (65+ must pass)

**Acceptance Criteria**:
- ✅ Full pipeline works end-to-end (uninstall → scan → review → clean)
- ✅ Settings persist across app restarts
- ✅ Panel appears in MainWindow tab bar
- ✅ Keyboard shortcut works
- ✅ Log output appears in detachable log window
- ✅ Build succeeds locally and on CI
- ✅ All existing tests pass

---

### Phase 6: Testing & Polish (Week 16-18)

**Goals**:
- Comprehensive test suite
- Edge case handling
- Performance optimization
- Documentation

**Tasks**:
1. Write unit tests for all components
   - `test_program_enumerator.cpp` — registry scanning, UWP listing, orphan detection
   - `test_leftover_scanner.cpp` — all scan levels, pattern matching, safety classification
   - `test_registry_snapshot_engine.cpp` — snapshot capture, diff accuracy
   - `test_restore_point_manager.cpp` — availability check
   - `test_advanced_uninstall_integration.cpp` — end-to-end with mock program
2. Test edge cases:
   - Programs with no uninstall string
   - Programs with broken uninstall strings (file not found)
   - Programs with Unicode names (Chinese, Arabic, Japanese)
   - Very long install paths
   - Programs installed for all users vs. current user only
   - MSI installers with missing GUID
   - UWP apps with dependencies
   - Registry permissions denied (non-admin)
   - Service running with elevated privileges
   - File locked by running process
   - System Restore disabled
   - Extremely large program (> 10 GB)
   - Programs with shared components (e.g., Visual C++ Redistributable)
3. Performance testing:
   - Enumeration of 200+ programs in < 5 seconds
   - Registry snapshot capture in < 5 seconds
   - Safe leftover scan in < 10 seconds
   - Full leftover table render with 500 items
4. Accessibility:
   - Keyboard navigation through all UI elements
   - Screen reader labels on all interactive elements
   - High contrast theme compatibility
5. TigerStyle compliance scan (static_asserts, naming conventions)
6. Update `README.md` changelog
7. Update `THIRD_PARTY_LICENSES.md` (no new deps, but document Win32 API usage)
8. Update `docs/ENTERPRISE_HARDENING_TRACKER.md`
9. Update `docs/CODEBASE_AUDIT_TRACKER.md`

**Acceptance Criteria**:
- ✅ All new unit tests pass
- ✅ All existing 65+ tests still pass
- ✅ 0 compiler warnings
- ✅ TigerStyle lint: 0 errors
- ✅ Program enumeration in < 5 seconds
- ✅ UI responsive during all operations (< 100ms frame time)
- ✅ No crashes on any edge case
- ✅ CI build passes
- ✅ Documentation updated

---

**Total Timeline**: 18 weeks (4.5 months)

---

## 📋 CMakeLists.txt Changes

### New Source Files
```cmake
# Add to CORE_SOURCES:
src/core/advanced_uninstall_controller.cpp
src/core/program_enumerator.cpp
src/core/uninstall_worker.cpp
src/core/leftover_scanner.cpp
src/core/cleanup_worker.cpp
src/core/restore_point_manager.cpp
src/core/registry_snapshot_engine.cpp
include/sak/advanced_uninstall_controller.h
include/sak/advanced_uninstall_types.h
include/sak/program_enumerator.h
include/sak/uninstall_worker.h
include/sak/leftover_scanner.h
include/sak/cleanup_worker.h
include/sak/restore_point_manager.h
include/sak/registry_snapshot_engine.h

# Add to GUI_SOURCES:
src/gui/advanced_uninstall_panel.cpp
src/gui/advanced_uninstall_panel_dialogs.cpp
include/sak/advanced_uninstall_panel.h

# Win32 libraries (should already be linked):
# Advapi32.lib    (Registry APIs)
# Shell32.lib     (SHGetFileInfoW, icon extraction)
# Msi.lib         (MsiEnumProducts — optional, for enhanced MSI handling)
```

### No vcpkg Changes Required
Zero new external dependencies — this panel uses only Win32 APIs, PowerShell, and existing Qt/SAK infrastructure.

---

## 📋 Configuration & Settings

### ConfigManager Extensions

```cpp
// Advanced Uninstall Settings

// Defaults
ScanLevel getAdvUninstallDefaultScanLevel() const;
void setAdvUninstallDefaultScanLevel(ScanLevel level);

bool getAdvUninstallAutoRestorePoint() const;
void setAdvUninstallAutoRestorePoint(bool enabled);

bool getAdvUninstallAutoCleanSafe() const;
void setAdvUninstallAutoCleanSafe(bool enabled);

bool getAdvUninstallShowSystemComponents() const;
void setAdvUninstallShowSystemComponents(bool show);

// UI state
QByteArray getAdvUninstallColumnWidths() const;
void setAdvUninstallColumnWidths(const QByteArray& state);

int getAdvUninstallSortColumn() const;
void setAdvUninstallSortColumn(int column);

Qt::SortOrder getAdvUninstallSortOrder() const;
void setAdvUninstallSortOrder(Qt::SortOrder order);

int getAdvUninstallViewFilter() const;
void setAdvUninstallViewFilter(int filter);
```

**Default Values**:
```cpp
advuninstall/default_scan_level = 1          // Moderate
advuninstall/auto_restore_point = true
advuninstall/auto_clean_safe = true
advuninstall/show_system_components = false
advuninstall/sort_column = 0                 // Name
advuninstall/sort_order = 0                  // Ascending
advuninstall/view_filter = 0                 // All Programs
```

---

## 🧪 Testing Strategy

### Unit Tests

**test_program_enumerator.cpp**:
- Registry scanning returns non-empty results on a dev machine
- Programs have display names (no blank names)
- Orphaned detection flags entries with missing install paths
- System components filtered by default
- UWP packages enumerated (at least system apps on any Windows 10/11 machine)
- Duplicate entries from HKLM + HKCU handled correctly

**test_leftover_scanner.cpp**:
- Build search patterns from program name "Norton Security" → ["norton", "security", "nortonsecurity"]
- Build search patterns from publisher "NortonLifeLock" → ["nortonlifelock", "norton"]
- Pattern matching is case-insensitive
- Protected path list prevents system path selection
- Safety classification: exact match in ProgramFiles → Green
- Safety classification: publisher directory in registry → Yellow
- Safety classification: system path proximity → Red
- Safe level scans fewer locations than Moderate
- Moderate level scans fewer than Advanced

**test_registry_snapshot_engine.cpp**:
- Snapshot capture returns non-empty set on Windows
- Diff of identical snapshots returns no changes
- Diff detects new keys added between snapshots
- Snapshot depth limit prevents excessive enumeration

**test_restore_point_manager.cpp**:
- `isSystemRestoreEnabled()` returns a boolean without error
- (Cannot safely test restore point creation in CI)

**test_advanced_uninstall_integration.cpp**:
- Create a mock test program (registry entry only) → enumerate → verify found
- Forced uninstall the mock program → verify registry entry removed
- Create mock leftover files in temp directory → scan → verify detected
- Clean mock leftovers → verify files deleted

### Manual Testing

1. **Standard uninstall** of a real program (7-Zip recommended for safe testing)
2. **View all programs** — verify count matches "Apps & Features" approximately
3. **Search and filter** — verify real-time filtering works
4. **Forced uninstall** on a manually deleted program
5. **UWP removal** of a bloatware app (Candy Crush or similar)
6. **Batch uninstall** of 3+ programs sequentially
7. **Restore point creation** — verify visible in System Restore UI
8. **Leftover scan levels** — compare Safe vs. Moderate vs. Advanced results
9. **Safety classification** — verify green/yellow/red items make sense
10. **Cancel during operation** — verify clean abort
11. **Non-admin scenario** — verify graceful handling when elevation needed
12. **Properties dialog** — verify all metadata displayed correctly

---

## 🚧 Limitations & Challenges

### Technical Limitations

**Administrator Privileges Required**:
- ⚠️ Many operations require elevation: HKLM registry writes, service removal, restore points, system programs
- ⚠️ SAK is typically run as admin by technicians, but non-admin mode should gracefully degrade
- **Mitigation**: Check elevation at panel load. Disable "Uninstall" for HKLM programs if not admin. Show "Run as Administrator" prompt.

**Registry Snapshot Performance**:
- ⚠️ Full registry enumeration under HKLM\SOFTWARE can take 5-15 seconds with thousands of keys
- ⚠️ Deep recursion into HKCR can be very slow (millions of CLSID entries)
- **Mitigation**: Limit depth to 3 levels. Skip known-large hives (HKCR\CLSID, HKCR\WOW6432Node). Cache snapshot for reuse within same session.

**Shared Component Risk**:
- ⚠️ Some programs share DLLs (Visual C++ Redistributables), registry keys (COM servers), or AppData folders
- ⚠️ Removing shared components can break other programs
- **Mitigation**: Reference counting where possible. Yellow/Red risk classification. Default to NOT selecting shared items. Include "Review" hint in description.

**Incomplete Leftover Detection**:
- ⚠️ Not all programs follow standard installation patterns — some scatter files across unusual locations
- ⚠️ Installers written in NSIS, Inno Setup, InstallShield all use different layout conventions
- **Mitigation**: Pattern matching is heuristic, not perfect. The three scan levels provide increasing thoroughness. Forced Uninstall with Advanced scan covers the most ground.

**Windows Installer (MSI) Complexity**:
- ⚠️ MSI uninstall via `msiexec /x {GUID}` may prompt for source media or require network access
- ⚠️ Some MSI installs are per-machine while others are per-user
- **Mitigation**: Support both `/x` (interactive) and `/qn` (silent) modes. Let user choose.

**System Restore Point Rate Limiting**:
- ⚠️ Windows throttles restore point creation — only one per 24 hours on some configurations
- ⚠️ Some enterprise policies disable System Restore entirely
- **Mitigation**: Check availability before offering. If throttled, warn user but proceed without restore point. Always offer the option but make it non-blocking.

**Locked Files During Cleanup**:
- ⚠️ Some application files may remain locked by running services or background processes
- **Mitigation**: Detect lock via `ERROR_SHARING_VIOLATION`. Report locked files in output. Suggest "schedule for deletion on reboot" as future enhancement.

---

## 🎯 Success Metrics

| Metric | Target | Importance |
|--------|--------|------------|
| Program enumeration time | < 5 seconds (200 programs) | Critical |
| Search/filter response | < 50 ms (real-time) | Critical |
| Restore point creation | < 60 seconds | High |
| Registry snapshot capture | < 5 seconds | High |
| Safe level leftover scan | < 10 seconds | High |
| Moderate level leftover scan | < 30 seconds | High |
| Advanced level leftover scan | < 60 seconds | Medium |
| Native uninstaller launch | < 2 seconds | High |
| Leftover cleanup (100 items) | < 30 seconds | High |
| UI frame time during scan | < 100 ms (responsive) | Critical |
| Panel tab switch | < 100 ms | High |
| Program icon extraction | < 1 second per program | Medium |
| Batch uninstall overhead per program | < 5 seconds (excluding native uninstaller) | Medium |
| Memory usage during scan | < 100 MB | Medium |
| False positive rate (leftovers) | < 5% of found items | High |
| Missed leftover rate | < 20% (Moderate level) | Medium |

---

## 🔒 Security Considerations

### Privilege Management
- Panel checks for administrator elevation at startup
- Non-admin mode: Can view programs and UWP apps, but cannot uninstall HKLM programs or create restore points
- HKCU programs can be uninstalled without elevation
- UWP per-user apps can be removed without elevation
- All elevated operations use existing process context (no UAC re-prompts if already elevated)

### Registry Safety
- Never modify `HKLM\SOFTWARE\Microsoft\Windows` or `HKLM\SYSTEM\CurrentControlSet\Control`
- Protected path list prevents accidental system damage
- Registry operations use `KEY_READ` for scanning, `KEY_WRITE` only during cleanup
- Registry values backed up in report before deletion (logged, not restored automatically)

### File System Safety
- Never delete files under `C:\Windows`, `C:\Windows\System32`, `C:\Windows\SysWOW64`
- Never delete `C:\Users\Default` or `C:\Users\Public` directories
- Only delete files/folders that match the program's name patterns
- File deletion uses `QFile::remove()` (moves to recycle bin is not default — permanent delete)

### Process Execution Safety
- Native uninstaller strings sanitized before execution (prevent command injection)
- Only execute uninstall strings from registry (trusted source)
- PowerShell commands use `-NoProfile -NonInteractive` for predictable behavior
- Process execution timeout prevents zombie processes

### Data Privacy
- Uninstall reports may contain program names and paths — stored locally only
- No telemetry or network communication
- Search history and queue state stored in local `ConfigManager` only

---

## 💡 Future Enhancements (Post-v0.9.0)

### v1.0.0 - Advanced Features
- **Real-Time Installation Monitor** — Watch filesystem and registry changes during any program install to create exact uninstall logs
- **Installation Logs Database** — Pre-built knowledge of what specific programs install, for more accurate leftover detection
- **Reboot-Pending Deletions** — Schedule locked files for deletion on next reboot via `MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT)`
- **Export/Import Program List** — Save installed program inventory for documentation or comparison

### v1.1.0 - Enhanced Cleanup
- **Browser Extension Management** — List and remove browser extensions (Chrome, Firefox, Edge)
- **Startup Manager** — Dedicated view for all startup entries (registry Run keys, Startup folder, scheduled tasks)
- **Service Manager** — Dedicated view for all Windows services with start/stop/disable/delete
- **Context Menu Cleanup** — Find and remove orphaned shell context menu entries

### v1.2.0 - Intelligence
- **Community Leftover Database** — Crowdsourced database of known leftover patterns per application
- **Smart Scan** — ML-based classification of leftover risk levels
- **Undo Cleanup** — Full rollback of cleaned items using backup data
- **Comparison View** — Compare installed programs between two machines (source/target for migration)

---

## 📚 Resources

### Official Documentation
- [Windows Registry - Uninstall Key](https://learn.microsoft.com/en-us/windows/win32/msi/uninstall-registry-key) — Microsoft's reference for the Uninstall registry structure
- [RegOpenKeyExW](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regopenkeyexw) — Registry key opening API
- [RegEnumKeyExW](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regenumkeyexw) — Registry key enumeration API
- [RegDeleteKeyExW](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regdeletekeyexw) — Registry key deletion API
- [MsiEnumProducts](https://learn.microsoft.com/en-us/windows/win32/api/msi/nf-msi-msienumproductsa) — Windows Installer product enumeration
- [Get-AppxPackage](https://learn.microsoft.com/en-us/powershell/module/appx/get-appxpackage) — UWP package enumeration
- [Remove-AppxPackage](https://learn.microsoft.com/en-us/powershell/module/appx/remove-appxpackage) — UWP package removal
- [Checkpoint-Computer](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.management/checkpoint-computer) — System Restore point creation
- [SHGetFileInfoW](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shgetfileinfow) — Icon extraction
- [QProcess](https://doc.qt.io/qt-6/qprocess.html) — External process execution

### SAK Codebase Reference
- `include/sak/worker_base.h` — Base class for worker threads
- `include/sak/app_scanner.h` — Existing registry enumeration (reusable patterns)
- `src/core/app_scanner.cpp` — Win32 registry access patterns
- `include/sak/style_constants.h` — UI theme constants (colors, margins, fonts, buttons)
- `src/actions/check_bloatware_action.cpp` — Bloatware pattern database (~40 patterns)
- `include/sak/chocolatey_manager.h` — `uninstallPackage()` backend method
- `src/gui/main_window.cpp` — Panel registration pattern
- `include/sak/config_manager.h` — Settings persistence

### Inspiration
- [Revo Uninstaller](https://www.revouninstaller.com/) — Feature reference (leftover scanning, forced uninstall, hunter mode)
- [Geek Uninstaller](https://geekuninstaller.com/) — Lightweight forced uninstall reference
- [IObit Uninstaller](https://www.iobit.com/en/advanceduninstaller.php) — Batch uninstall and bundleware detection reference

---

## 📞 Support

**Questions?** Open a GitHub Discussion  
**Found a Bug?** Open a GitHub Issue  
**Want to Contribute?** See [CONTRIBUTING.md](CONTRIBUTING.md)

---

**Document Version**: 1.0  
**Last Updated**: March 2, 2026  
**Author**: Randy Northrup  
**Status**: ✅ Ready for Implementation

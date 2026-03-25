# System Tools Tab — Comprehensive Implementation Plan

**Version**: 1.0  
**Date**: March 25, 2026  
**Status**: 📋 Planned  
**Parent Panel**: Benchmark & Diagnostics (DiagnosticBenchmarkPanel)  
**Tab Position**: Tab 2 (after Diagnostics and Benchmarks)

---

## 🎯 Executive Summary

The System Tools tab adds a centralized shortcut launcher for built-in Windows
administrative and diagnostic utilities directly within the Benchmark & Diagnostics
panel. Technicians can quickly access dozens of commonly needed system tools —
organized into logical categories with descriptions and tooltips — without hunting
through Start Menu, Run dialogs, or Control Panel applets. The tab provides a clean,
searchable grid of tool cards that launch the corresponding Windows executable with a
single click.

### Key Objectives
- **Categorized Tool Grid** — System tools organized into logical groups (System, Disk, Network, Security, etc.) with icons, names, descriptions, and rich tooltips
- **One-Click Launch** — Each tool card launches the corresponding Windows executable via `QProcess::startDetached`
- **Search/Filter** — Real-time filter bar to find tools by name, description, or category
- **Availability Detection** — Gray out / badge tools that are unavailable on the current Windows edition or not installed
- **Favorites** — Technicians can star frequently-used tools for quick access at the top
- **Tooltip Details** — Hover any tool to see: what it does, the executable path, required privileges, and the Windows editions that include it
- **Category Collapse** — Expand/collapse categories to reduce visual clutter
- **Zero Dependencies** — Uses only Windows built-in executables; nothing to download or install

---

## 📊 Project Scope

### What is the System Tools Tab?

The System Tools tab is a **launcher panel** — it does not implement any system tool
functionality itself. Instead, it provides a well-organized, searchable catalog of
Windows built-in utilities that technicians use regularly during PC setup, maintenance,
troubleshooting, and repair.

**Problem it solves**: Technicians working on unfamiliar machines waste time locating
tools through nested Start Menu folders, typing executable names in Run dialogs, or
navigating deep Control Panel paths. Many technicians don't know about useful tools
that Windows ships (e.g., `resmon.exe`, `msinfo32.exe`, `mdsched.exe`). This tab puts
every relevant tool one click away with context about what each tool does.

**Launcher Workflow**:
1. Technician opens Benchmark & Diagnostics → System Tools tab
2. Browses categories or types in the search bar
3. Clicks a tool card → tool launches as a separate process
4. Tool runs independently; SAK Utility remains responsive
5. Optionally stars frequently-used tools for "Favorites" category at top

**Key Constraints**:
- **Launch only** — SAK does not embed or wrap these tools; it starts them as separate processes
- **No elevation proxy** — Tools that require admin run under SAK's existing elevated context (SAK runs as admin via manifest)
- **No data capture** — SAK does not intercept or parse output from launched tools
- **Portable awareness** — Tool paths are resolved dynamically via `%SystemRoot%` and registry; never hardcoded to `C:\Windows`

---

## 🎯 Use Cases

### 1. **New Machine Setup — Checking System Configuration**
**Scenario**: Technician is setting up a new workstation and needs to verify hardware, BIOS, drivers, and Windows activation.

**Workflow**:
1. Open Benchmark & Diagnostics → System Tools
2. Click "System Information" → `msinfo32.exe` opens showing full hardware/software inventory
3. Click "Device Manager" → verify all drivers installed (no yellow triangles)
4. Click "DirectX Diagnostic Tool" → verify GPU driver and DirectX feature level
5. Click "Windows Activation" → verify license status

**Benefits**:
- All verification tools accessible from one place
- No hunting through Settings, Control Panel, and Run dialog
- Tooltips remind technician what each tool shows

---

### 2. **Slow Machine — Performance Troubleshooting**
**Scenario**: Customer complains "it's really slow." Technician needs to identify the bottleneck.

**Workflow**:
1. Open System Tools tab
2. Click "Resource Monitor" → see real-time CPU, memory, disk, and network per-process
3. Click "Task Manager" → identify high-CPU/memory processes
4. Click "Performance Monitor" → check disk queue length and page faults
5. Click "Reliability Monitor" → check recent crashes and failures
6. Click "Event Viewer" → search for error events around the time of slowness

**Benefits**:
- Systematic performance investigation in logical order
- Category grouping suggests which tools to try
- Descriptions explain what each tool measures

---

### 3. **Disk Issues — Storage Troubleshooting**
**Scenario**: Customer's disk is full, or partition needs resizing, or drive health is suspect.

**Workflow**:
1. Open System Tools tab → Disk & Storage category
2. Click "Disk Management" → see all partitions, unallocated space, health status
3. Click "Disk Cleanup" → remove temp files, Windows Update cleanup, thumbnails
4. Click "Defragment and Optimize Drives" → check fragmentation on HDD, TRIM on SSD
5. Click "Storage Sense Settings" → configure automatic cleanup policies

**Benefits**:
- All disk tools in one category
- Descriptions clarify which tool to use for which problem
- Avoids common mistake of defragmenting an SSD (tooltip warns)

---

### 4. **Security Audit — Hardening a Workstation**
**Scenario**: Technician is hardening a workstation for a security-conscious customer.

**Workflow**:
1. Open System Tools tab → Security category
2. Click "Windows Security" → verify antivirus, firewall, device security
3. Click "Local Security Policy" → check password policy, audit policy, user rights
4. Click "Windows Firewall with Advanced Security" → review inbound/outbound rules
5. Click "Credential Manager" → audit stored credentials
6. Click "Local Group Policy Editor" → verify group policies applied

**Benefits**:
- Complete security tool inventory in one place
- Tooltips note which tools require Pro/Enterprise editions
- Edition availability badge prevents confusion on Home SKUs

---

### 5. **Network Issues — Quick Access to Windows Network Tools**
**Scenario**: Technician needs Windows-native network tools (not SAK's built-in diagnostics).

**Workflow**:
1. Open System Tools tab → Network category
2. Click "Network Connections" → open ncpa.cpl for adapter properties
3. Click "Network and Sharing Center" → check network profile, sharing settings
4. Click "Remote Desktop Connection" → quickly RDP to another machine
5. Click "Windows Firewall" → check if firewall is blocking

**Benefits**:
- Complements SAK's Network Diagnostics panel with native Windows tools
- Quick access to adapter property pages (change IP, DNS, etc.)
- RDP shortcut saves time compared to Start Menu search

---

### 6. **System Recovery — Repair and Maintenance**
**Scenario**: System has stability issues — technician needs to check integrity and repair.

**Workflow**:
1. Open System Tools tab → System Maintenance category
2. Click "System File Checker" → note: tooltip explains this runs `sfc /scannow` in CMD
3. Click "Memory Diagnostic" → schedule RAM test on next reboot
4. Click "System Restore" → check for restore points
5. Click "Recovery Options" → access reset/advanced startup options
6. Click "Reliability Monitor" → review failure history timeline

**Benefits**:
- Recovery tools grouped logically
- Tooltips explain which tools require reboot
- Descriptions help technician choose the right repair tool

---

## 🏗️ Architecture Overview

### Component Hierarchy

```
DiagnosticBenchmarkPanel (existing)
├─ Tab 0: Diagnostics (existing)
├─ Tab 1: Benchmarks (existing)
└─ Tab 2: System Tools (NEW)
   │
   ├─ SystemToolsWidget (QWidget)
   │  ├─ Search/Filter: QLineEdit with real-time filtering
   │  ├─ Favorites Section: Starred tools (persisted in QSettings)
   │  └─ Category Sections: Collapsible QGroupBox per category
   │     └─ Tool Cards: Clickable widgets in a QGridLayout / FlowLayout
   │        ├─ Icon (32×32 or 24×24 SVG)
   │        ├─ Name (bold label)
   │        ├─ Description (1-line gray text)
   │        └─ Availability badge (if not present on this edition)
   │
   ├─ SystemToolCatalog (QObject) [Data Layer]
   │  ├─ Builds the complete list of tools with metadata
   │  ├─ Detects tool availability (file existence + edition check)
   │  ├─ Manages favorites (load/save from QSettings)
   │  └─ Provides filtered views (by category, search term, favorites)
   │
   └─ SystemToolLauncher (static utility)
      ├─ Resolves executable path via %SystemRoot% / registry
      ├─ Launches via QProcess::startDetached()
      ├─ Logs launch events via sak::logInfo()
      └─ Handles MMC snap-ins (.msc) and Control Panel applets (.cpl)
```

### Integration with DiagnosticBenchmarkPanel

The System Tools tab plugs into the existing tab widget:

```cpp
// In DiagnosticBenchmarkPanel::setupUi() — after existing tabs
m_tabs->addTab(createSystemToolsTab(), tr("System Tools"));

// In the currentChanged lambda — add entry for index 2
{":/icons/icons/system_tools.svg", "System Tools",
 "Launch built-in Windows system utilities"}
```

The `SystemToolsWidget` is self-contained: it owns the tool catalog and all UI
elements. The only integration point with `DiagnosticBenchmarkPanel` is the shared
`statusMessage` signal (to show "Launched Device Manager" in the status bar).

---

## 🛠️ Technical Specifications

### Tool Catalog Data Structure

```cpp
/// @brief Metadata for a single Windows system tool
struct SystemToolEntry {
    QString id;               // Unique key: "device_manager", "disk_management"
    QString name;             // Display name: "Device Manager"
    QString description;      // 1-line: "View and manage hardware devices and drivers"
    QString tooltip;          // Rich tooltip (multi-line, detailed)
    QString category;         // "System Information", "Disk & Storage", etc.
    QString executable;       // "devmgmt.msc", "diskmgmt.msc", "resmon.exe"
    QStringList arguments;    // Optional args (usually empty)
    QString iconResource;     // ":/icons/icons/tool_device_manager.svg"
    LaunchMethod launchMethod;// Exe, MscSnapin, ControlPanelApplet, ShellExecute
    WindowsEdition minEdition;// Home, Pro, Enterprise (for availability badge)
    bool requiresElevation;   // True if needs admin (most do — SAK already runs elevated)
    bool availableOnSystem;   // Detected at runtime: does the executable exist?
};

/// @brief How to launch the tool
enum class LaunchMethod {
    Executable,        // Direct .exe launch via QProcess::startDetached
    MscSnapin,         // MMC snap-in (.msc) launched via mmc.exe
    ControlPanelApplet,// Control Panel applet (.cpl) launched via control.exe
    ShellExecute,      // ms-settings: URI or shell:::{GUID} via QDesktopServices
    CommandPrompt      // Launches cmd.exe /k <command> (for CLI tools like sfc)
};

/// @brief Windows edition for availability detection
enum class WindowsEdition {
    AllEditions,       // Available on Home, Pro, Enterprise, Education
    ProAndAbove,       // Requires Pro, Enterprise, or Education
    EnterpriseOnly     // Requires Enterprise or Education
};
```

### Tool Categories and Complete Catalog

#### Category 1: System Information & Configuration

| Tool | Executable | Launch Method | Description | Min Edition |
|------|-----------|---------------|-------------|-------------|
| System Information | `msinfo32.exe` | Executable | Detailed hardware, software, and environment info | All |
| DirectX Diagnostic Tool | `dxdiag.exe` | Executable | Display, sound, and input device diagnostics | All |
| Device Manager | `devmgmt.msc` | MscSnapin | View and manage hardware devices and drivers | All |
| System Properties | `sysdm.cpl` | ControlPanelApplet | Computer name, domain, hardware profiles, advanced settings | All |
| System Configuration (MSConfig) | `msconfig.exe` | Executable | Boot options, startup services, diagnostic startup | All |
| About Your PC | `ms-settings:about` | ShellExecute | Windows edition, device specs, rename PC | All |
| Environment Variables | `rundll32.exe sysdm.cpl,EditEnvironmentVariables` | Executable | View and edit system/user environment variables | All |
| Windows Version (winver) | `winver.exe` | Executable | Quick Windows build number and edition display | All |
| Registry Editor | `regedit.exe` | Executable | Browse and edit the Windows registry | All |

#### Category 2: Performance & Monitoring

| Tool | Executable | Launch Method | Description | Min Edition |
|------|-----------|---------------|-------------|-------------|
| Task Manager | `taskmgr.exe` | Executable | Process, performance, startup, services overview | All |
| Resource Monitor | `resmon.exe` | Executable | Real-time CPU, memory, disk, and network per-process | All |
| Performance Monitor | `perfmon.exe` | Executable | System performance counters, data collector sets | All |
| Reliability Monitor | `perfmon.exe /rel` | Executable | System stability timeline with failure history | All |
| Event Viewer | `eventvwr.msc` | MscSnapin | Windows event logs (Application, System, Security) | All |

#### Category 3: Disk & Storage

| Tool | Executable | Launch Method | Description | Min Edition |
|------|-----------|---------------|-------------|-------------|
| Disk Management | `diskmgmt.msc` | MscSnapin | Partition, format, resize, and manage disk volumes | All |
| Disk Cleanup | `cleanmgr.exe` | Executable | Remove temporary files, Windows Update cleanup | All |
| Defragment and Optimize Drives | `dfrgui.exe` | Executable | Defragment HDDs, send TRIM to SSDs, schedule optimization | All |
| Storage Sense Settings | `ms-settings:storagepolicies` | ShellExecute | Configure automatic disk cleanup policies | All |
| iSCSI Initiator | `iscsicpl.exe` | Executable | Configure iSCSI target connections | All |

#### Category 4: Network & Connectivity

| Tool | Executable | Launch Method | Description | Min Edition |
|------|-----------|---------------|-------------|-------------|
| Network Connections | `ncpa.cpl` | ControlPanelApplet | Adapter properties, IP config, enable/disable adapters | All |
| Network and Sharing Center | `control.exe /name Microsoft.NetworkAndSharingCenter` | Executable | Network profile, sharing settings, adapter settings | All |
| Windows Firewall with Advanced Security | `wf.msc` | MscSnapin | Inbound/outbound firewall rules, connection security | All |
| Remote Desktop Connection | `mstsc.exe` | Executable | Connect to remote computers via RDP | All |
| Internet Options | `inetcpl.cpl` | ControlPanelApplet | Proxy settings, security zones, certificates, connections | All |

#### Category 5: Security & User Management

| Tool | Executable | Launch Method | Description | Min Edition |
|------|-----------|---------------|-------------|-------------|
| Windows Security | `ms-settings:windowsdefender` | ShellExecute | Antivirus, firewall, device security, app control | All |
| Local Security Policy | `secpol.msc` | MscSnapin | Password policy, audit policy, user rights assignments | Pro+ |
| Local Users and Groups | `lusrmgr.msc` | MscSnapin | Manage local user accounts and group memberships | Pro+ |
| Credential Manager | `control.exe /name Microsoft.CredentialManager` | Executable | Manage stored Windows and web credentials | All |
| Certificate Manager | `certmgr.msc` | MscSnapin | View and manage user and machine certificates | All |
| Local Group Policy Editor | `gpedit.msc` | MscSnapin | Configure local group policies | Pro+ |
| Computer Management | `compmgmt.msc` | MscSnapin | Unified console: users, disks, services, events, devices | All |
| BitLocker Drive Encryption | `control.exe /name Microsoft.BitLockerDriveEncryption` | Executable | Manage BitLocker encryption on drives | Pro+ |
| Windows Update | `ms-settings:windowsupdate` | ShellExecute | Check and install Windows updates | All |

#### Category 6: System Maintenance & Recovery

| Tool | Executable | Launch Method | Description | Min Edition |
|------|-----------|---------------|-------------|-------------|
| System Restore | `rstrui.exe` | Executable | Restore system to a previous restore point | All |
| Memory Diagnostic | `mdsched.exe` | Executable | Schedule RAM test on next reboot | All |
| Recovery Options | `ms-settings:recovery` | ShellExecute | Reset PC, advanced startup, go back to previous version | All |
| Startup Repair (via Settings) | `ms-settings:recovery` | ShellExecute | Access advanced startup for repair options | All |
| Programs and Features | `appwiz.cpl` | ControlPanelApplet | Classic add/remove programs and Windows features | All |
| Turn Windows Features On/Off | `optionalfeatures.exe` | Executable | Enable/disable Windows optional features (Hyper-V, WSL, etc.) | All |

#### Category 7: Developer & Power User Tools

| Tool | Executable | Launch Method | Description | Min Edition |
|------|-----------|---------------|-------------|-------------|
| PowerShell (Admin) | `powershell.exe` | Executable | Windows PowerShell with admin privileges | All |
| Command Prompt (Admin) | `cmd.exe` | Executable | Classic command-line interpreter with admin privileges | All |
| Windows Terminal | `wt.exe` | Executable | Modern terminal with tabs, profiles, and GPU rendering | All |
| Component Services (DCOM) | `dcomcnfg.exe` | Executable | COM+ applications, DCOM configuration | All |
| Services | `services.msc` | MscSnapin | Start, stop, and configure Windows services | All |
| Shared Folders | `fsmgmt.msc` | MscSnapin | View open files, active sessions, and shared folders | All |
| Task Scheduler | `taskschd.msc` | MscSnapin | Create and manage scheduled tasks | All |
| Hyper-V Manager | `virtmgmt.msc` | MscSnapin | Manage virtual machines (Hyper-V) | Pro+ |
| ODBC Data Sources | `odbcad32.exe` | Executable | Configure ODBC database connections | All |
| Print Management | `printmanagement.msc` | MscSnapin | Manage printers, drivers, and print queues | Pro+ |

---

### Tool Availability Detection

```cpp
/// @brief Check if a system tool exists on this machine
[[nodiscard]] bool isToolAvailable(const SystemToolEntry& tool)
{
    switch (tool.launchMethod) {
    case LaunchMethod::Executable: {
        // Resolve via %SystemRoot% (e.g., C:\Windows\System32\resmon.exe)
        const QString sys32 =
            QDir::toNativeSeparators(
                QString::fromLocal8Bit(qgetenv("SystemRoot"))
                + "\\System32\\");
        return QFile::exists(sys32 + tool.executable);
    }
    case LaunchMethod::MscSnapin: {
        // .msc files live in System32
        const QString sys32 =
            QDir::toNativeSeparators(
                QString::fromLocal8Bit(qgetenv("SystemRoot"))
                + "\\System32\\");
        return QFile::exists(sys32 + tool.executable);
    }
    case LaunchMethod::ControlPanelApplet: {
        const QString sys32 =
            QDir::toNativeSeparators(
                QString::fromLocal8Bit(qgetenv("SystemRoot"))
                + "\\System32\\");
        return QFile::exists(sys32 + tool.executable);
    }
    case LaunchMethod::ShellExecute:
        // ms-settings: URIs are always "available" on Windows 10+
        return true;
    case LaunchMethod::CommandPrompt:
        return true; // cmd.exe is always present
    }
    return false;
}
```

### Windows Edition Detection

```cpp
/// @brief Detect the current Windows edition
[[nodiscard]] WindowsEdition detectWindowsEdition()
{
    QSettings registry(
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
        QSettings::NativeFormat);
    const QString edition =
        registry.value("EditionID").toString().toLower();

    if (edition.contains("enterprise") || edition.contains("education")) {
        return WindowsEdition::EnterpriseOnly;
    }
    if (edition.contains("professional") || edition.contains("pro")) {
        return WindowsEdition::ProAndAbove;
    }
    return WindowsEdition::AllEditions;
}

/// @brief Check if a tool is supported on the current edition
[[nodiscard]] bool isEditionSupported(WindowsEdition tool_min,
                                      WindowsEdition current)
{
    // Higher enum value = more restrictive edition
    return static_cast<int>(current) >= static_cast<int>(tool_min);
}
```

### Tool Launcher

```cpp
/// @brief Launch a system tool as a detached process
/// @return true if the process started successfully
[[nodiscard]] bool launchTool(const SystemToolEntry& tool)
{
    Q_ASSERT(!tool.executable.isEmpty());

    const QString sys_root =
        QString::fromLocal8Bit(qgetenv("SystemRoot"));
    const QString sys32 = sys_root + "\\System32\\";

    bool started = false;

    switch (tool.launchMethod) {
    case LaunchMethod::Executable: {
        const QString full_path = sys32 + tool.executable;
        started = QProcess::startDetached(full_path, tool.arguments);
        break;
    }
    case LaunchMethod::MscSnapin: {
        const QString mmc_path = sys32 + "mmc.exe";
        const QString snap_path = sys32 + tool.executable;
        started = QProcess::startDetached(mmc_path, {snap_path});
        break;
    }
    case LaunchMethod::ControlPanelApplet: {
        const QString control_path = sys32 + "control.exe";
        started = QProcess::startDetached(control_path, {tool.executable});
        break;
    }
    case LaunchMethod::ShellExecute: {
        // ms-settings: and shell::: URIs
        started = QDesktopServices::openUrl(QUrl(tool.executable));
        break;
    }
    case LaunchMethod::CommandPrompt: {
        const QString cmd_path = sys32 + "cmd.exe";
        QStringList args = {"/k"};
        args.append(tool.arguments);
        started = QProcess::startDetached(cmd_path, args);
        break;
    }
    }

    if (started) {
        sak::logInfo("Launched system tool: {} ({})",
                     tool.name.toStdString(),
                     tool.executable.toStdString());
    } else {
        sak::logError("Failed to launch system tool: {} ({})",
                      tool.name.toStdString(),
                      tool.executable.toStdString());
    }

    return started;
}
```

### Favorites Persistence

```cpp
/// @brief Save favorited tool IDs to QSettings
void saveFavorites(const QStringList& favorite_ids)
{
    QSettings settings;
    settings.beginGroup("SystemTools");
    settings.setValue("favorites", favorite_ids);
    settings.endGroup();
}

/// @brief Load favorited tool IDs from QSettings
[[nodiscard]] QStringList loadFavorites()
{
    QSettings settings;
    settings.beginGroup("SystemTools");
    const QStringList favorites =
        settings.value("favorites").toStringList();
    settings.endGroup();
    return favorites;
}
```

### Search/Filter Logic

```cpp
/// @brief Filter tools by search query (matches name, description, category)
[[nodiscard]] QVector<const SystemToolEntry*> filterTools(
    const QVector<SystemToolEntry>& catalog,
    const QString& query)
{
    QVector<const SystemToolEntry*> results;

    if (query.isEmpty()) {
        results.reserve(catalog.size());
        for (const auto& tool : catalog) {
            results.append(&tool);
        }
        return results;
    }

    const QString lower_query = query.toLower();

    for (const auto& tool : catalog) {
        if (tool.name.toLower().contains(lower_query)
            || tool.description.toLower().contains(lower_query)
            || tool.category.toLower().contains(lower_query)
            || tool.executable.toLower().contains(lower_query)) {
            results.append(&tool);
        }
    }

    return results;
}
```

---

## 🎨 User Interface Design

### System Tools Tab Layout

```
┌──────────────────────────────────────────────────────────────────────────┐
│  Benchmark & Diagnostics                                                 │
│  [Diagnostics] [Benchmarks] [System Tools]                   ← tab bar  │
├──────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  🔍 [Search tools...________________________]   [Show unavailable ☐]    │
│                                                                          │
│  ┌── ⭐ FAVORITES ──────────────────────────────────────────────────┐   │
│  │                                                                    │   │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐            │   │
│  │  │ 🖥️ Device    │  │ 📊 Resource  │  │ 💽 Disk      │            │   │
│  │  │   Manager    │  │   Monitor    │  │   Management │            │   │
│  │  │ Manage HW    │  │ Real-time    │  │ Partitions   │            │   │
│  │  │ devices...   │  │ usage...     │  │ & volumes... │            │   │
│  │  └──────────────┘  └──────────────┘  └──────────────┘            │   │
│  │                                                                    │   │
│  └────────────────────────────────────────────────────────────────────┘   │
│                                                                          │
│  ┌── 🖥️ SYSTEM INFORMATION & CONFIGURATION ─────────────────────────┐   │
│  │                                                                    │   │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐            │   │
│  │  │ ℹ️ System    │  │ 🎮 DirectX   │  │ 🖥️ Device    │            │   │
│  │  │   Information│  │   Diagnostic │  │   Manager    │            │   │
│  │  │ Hardware &   │  │ Display &    │  │ View & manage│            │   │
│  │  │ software...  │  │ sound diag.. │  │ HW devices.. │            │   │
│  │  └──────────────┘  └──────────────┘  └──────────────┘            │   │
│  │                                                                    │   │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐            │   │
│  │  │ ⚙️ System    │  │ 🔧 MSConfig  │  │ 📄 Registry  │            │   │
│  │  │   Properties │  │              │  │   Editor     │            │   │
│  │  │ Name, domain │  │ Boot config  │  │ Browse &     │            │   │
│  │  │ HW profiles  │  │ & services.. │  │ edit keys... │            │   │
│  │  └──────────────┘  └──────────────┘  └──────────────┘            │   │
│  │                                                                    │   │
│  └────────────────────────────────────────────────────────────────────┘   │
│                                                                          │
│  ┌── 📈 PERFORMANCE & MONITORING ────────────────────────────────────┐   │
│  │  ...                                                               │   │
│  └────────────────────────────────────────────────────────────────────┘   │
│                                                                          │
│  ┌── 💽 DISK & STORAGE ──────────────────────────────────────────────┐   │
│  │  ...                                                               │   │
│  └────────────────────────────────────────────────────────────────────┘   │
│                                                                          │
│  (continues with remaining categories, all scrollable)                   │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

### Tool Card Widget Design

Each tool card is a custom `QFrame`-based widget:

```
┌──────────────────────────────────┐
│  [icon 24×24]  Tool Name     [⭐]│   ← star toggles favorite
│               Short description  │
│               of what it does    │
│                            [Pro] │   ← edition badge (if not All)
└──────────────────────────────────┘
```

**States**:
- **Normal**: White background, subtle border
- **Hover**: Light blue background, highlight border
- **Pressed**: Darker blue momentary flash
- **Unavailable**: Grayed out, "Not available" overlay, dimmed icon
- **Favorite**: Gold star icon visible

**Tooltip** (appears on hover with 500ms delay):

```
┌─────────────────────────────────────────────────┐
│  Device Manager                                  │
│                                                   │
│  View and manage hardware devices and drivers.    │
│  Check for driver issues (yellow triangle),       │
│  update drivers, disable/enable devices, and      │
│  scan for hardware changes.                       │
│                                                   │
│  Executable: devmgmt.msc (MMC Snap-in)           │
│  Location:   %SystemRoot%\System32\              │
│  Requires:   Administrator                        │
│  Available:  All Windows editions                 │
└─────────────────────────────────────────────────┘
```

### Search Behavior

- Filters in real-time as the user types (debounced 150ms)
- Matches against: tool name, description, category name, executable name
- When filter is active, empty categories are hidden
- Clear button (×) in the search field resets filter
- Result count shown: "Showing 5 of 48 tools"

### Category Collapse Behavior

- Categories have a clickable header that toggles expand/collapse
- Collapse state persisted in QSettings per category
- "Expand All" / "Collapse All" buttons in the toolbar
- Favorites section is always expanded (cannot collapse)

---

## 📂 File Structure

### New Files to Create

#### Headers (`include/sak/`)

```
system_tools_widget.h             # Main tab widget (search, favorites, categories)
system_tool_catalog.h             # Tool data catalog + availability + favorites
system_tool_card.h                # Individual tool card widget
system_tool_types.h               # SystemToolEntry, LaunchMethod, WindowsEdition enums
system_tool_launcher.h            # Static launch helper (QProcess, ShellExecute, etc.)
```

#### Implementation (`src/`)

```
gui/system_tools_widget.cpp       # Tab UI: search bar, scroll area, category sections
core/system_tool_catalog.cpp      # Build catalog, detect availability, filter, favorites
gui/system_tool_card.cpp          # Card widget: icon + name + desc + star + hover states
core/system_tool_launcher.cpp     # Launch logic per LaunchMethod
```

#### Tests (`tests/unit/`)

```
test_system_tool_catalog.cpp      # Catalog building, filtering, favorites, availability
test_system_tool_launcher.cpp     # Launch method resolution, path building
```

#### Icons (`resources/icons/`)

New icons needed for the tab and tool categories (sourced from Icons8 or Windows
shell resources — see Icon Strategy below):

```
icons/system_tools_tab.svg        # Tab icon for the "System Tools" tab
icons/tool_system_info.svg        # Category: System Information
icons/tool_performance.svg        # Category: Performance & Monitoring
icons/tool_disk.svg               # Category: Disk & Storage
icons/tool_network.svg            # Category: Network & Connectivity
icons/tool_security.svg           # Category: Security & User Management
icons/tool_maintenance.svg        # Category: System Maintenance & Recovery
icons/tool_developer.svg          # Category: Developer & Power User Tools
icons/tool_favorite.svg           # Star/favorite icon
icons/tool_unavailable.svg        # Overlay badge for unavailable tools
```

Individual tool icons: Where possible, extract and use the actual Windows shell
icons from the executable files at runtime (see Icon Strategy section). For tools
where runtime extraction fails or for a consistent fallback, use Icons8 SVGs.

---

## 🎨 Icon Strategy

### Option A: Runtime Icon Extraction from Windows (Preferred)

Windows executables, `.msc` files, and `.cpl` files contain embedded icons. We can
extract them at runtime for a native look:

```cpp
/// @brief Extract the icon from a Windows executable at runtime
[[nodiscard]] QIcon extractWindowsIcon(const QString& executable_path)
{
    // Use Qt's file icon provider — works for .exe, .msc, .cpl
    QFileIconProvider provider;
    QFileInfo file_info(executable_path);
    return provider.icon(file_info);
}
```

For higher-quality extraction:

```cpp
/// @brief Extract high-resolution icon from a Windows executable
[[nodiscard]] QPixmap extractHighResIcon(const QString& path, int size)
{
    HICON icon_handle = nullptr;
    // ExtractIconExW gets the large icon from the executable's resources
    const int count = ExtractIconExW(
        reinterpret_cast<LPCWSTR>(path.utf16()),
        0,             // Icon index 0 (first icon)
        &icon_handle,  // Large icon
        nullptr,       // Small icon (not needed)
        1);            // Extract 1 icon

    if (count <= 0 || icon_handle == nullptr) {
        return {};
    }

    QPixmap pixmap = QPixmap::fromImage(
        QImage::fromHICON(icon_handle));
    DestroyIcon(icon_handle);

    return pixmap.scaled(size, size, Qt::KeepAspectRatio,
                         Qt::SmoothTransformation);
}
```

**Advantages**: Authentic Windows look, automatically matches OS theme, no bundled
icon files needed.

**Disadvantages**: Icons may vary between Windows versions, some `.msc` files don't
have custom icons, icon quality may be low (16×16 or 32×32 only).

### Option B: Icons8 SVGs (Consistent Fallback)

Use Icons8's "Fluent" or "Windows 11" style for consistent, high-quality icons
that match the SAK Utility's existing icon set. Icons8 MCP can search and
download SVGs directly.

**Query suggestions for Icons8**:
- "system information" → System Info category icon
- "device manager" / "hardware" → Device Manager
- "performance monitor" / "speedometer" → Performance category
- "hard drive" / "disk" → Disk category
- "network" / "ethernet" → Network category (reuse `panel_network.svg`)
- "security shield" → Security category
- "wrench" / "maintenance" → Maintenance category
- "terminal" / "command line" → Developer category
- "star" / "favorite" → Favorites toggle
- "settings" / "gear" → Generic system tool

### Option C: Hybrid Approach (Recommended)

1. **Category icons**: Use Icons8 SVGs (bundled in resources) for consistent styling
2. **Tool icons**: Attempt runtime extraction from Windows executable → fallback to
   a generic category icon if extraction fails
3. **Tab icon**: Use Icons8 SVG (bundled) matching the Fluent design language

```cpp
/// @brief Get the icon for a system tool — prefer native, fallback to bundled
[[nodiscard]] QIcon getToolIcon(const SystemToolEntry& tool)
{
    // Try native Windows icon first
    const QString sys32 =
        QString::fromLocal8Bit(qgetenv("SystemRoot")) + "\\System32\\";
    const QIcon native_icon = extractWindowsIcon(sys32 + tool.executable);

    if (!native_icon.isNull()) {
        return native_icon;
    }

    // Fallback to bundled SVG resource
    if (!tool.iconResource.isEmpty()) {
        return QIcon(tool.iconResource);
    }

    // Final fallback: generic category icon
    return QIcon(":/icons/icons/system_tools_tab.svg");
}
```

---

## 🔧 Implementation Phases

### Phase 1: Core Data Layer + Basic UI
**Scope**: Tool catalog, availability detection, basic grid layout

**Tasks**:
1. Create `system_tool_types.h` with `SystemToolEntry`, `LaunchMethod`, `WindowsEdition`
2. Create `system_tool_catalog.h/.cpp` with full tool catalog (48 entries)
3. Implement availability detection (`isToolAvailable`, `detectWindowsEdition`)
4. Create `system_tool_launcher.h/.cpp` with all 5 launch methods
5. Create `system_tool_card.h/.cpp` with basic card widget (icon + name + description)
6. Create `system_tools_widget.h/.cpp` with scrollable category grid
7. Add tab to `DiagnosticBenchmarkPanel::setupUi()`
8. Write unit tests for catalog building and filtering

**Deliverables**:
- All tools visible in categorized grid
- Clicking a tool launches it
- Unavailable tools grayed out

### Phase 2: Polish + Features
**Scope**: Search, favorites, tooltips, icon strategy

**Tasks**:
1. Add search bar with real-time filtering
2. Implement favorites (star toggle, QSettings persistence, Favorites section)
3. Add rich tooltips with executable path, description, edition info
4. Implement category expand/collapse with persistence
5. Add edition badge for Pro+/Enterprise tools
6. Implement icon loading (hybrid: native Windows icon + Icons8 fallback)
7. Connect `statusMessage` signal for "Launched X" feedback
8. Write unit tests for favorites persistence and search filtering

**Deliverables**:
- Fully functional search
- Persistent favorites
- Professional tooltips and visual polish
- Native Windows icons where available

### Phase 3: Icons + Final Polish
**Scope**: Icons8 SVG procurement, visual consistency, accessibility

**Tasks**:
1. Use Icons8 MCP to find and download category icons (Fluent/Windows 11 style)
2. Add icon resources to `icons.qrc`
3. Tune card sizing, spacing, and hover animations
4. Add keyboard navigation (arrow keys between cards, Enter to launch)
5. Add "Expand All" / "Collapse All" buttons
6. Add result count to search ("Showing N of M tools")
7. Test on Windows 10 and Windows 11 (different tool availability)
8. Write integration test for live tool launch (localhost-safe tools only)

**Deliverables**:
- Polished, production-ready tab
- All icons consistent with SAK design language
- Keyboard accessible

---

## 📋 CMakeLists.txt Changes

### New Source Files

```cmake
# In the main SAK_SOURCES list, add:

# System Tools Tab
src/gui/system_tools_widget.cpp
src/gui/system_tool_card.cpp
src/core/system_tool_catalog.cpp
src/core/system_tool_launcher.cpp
```

### New Header Files

```cmake
# In the main SAK_HEADERS list, add:

# System Tools Tab
include/sak/system_tools_widget.h
include/sak/system_tool_card.h
include/sak/system_tool_catalog.h
include/sak/system_tool_launcher.h
include/sak/system_tool_types.h
```

### New Test Files

```cmake
# In the test section, add:

sak_add_test(test_system_tool_catalog
    tests/unit/test_system_tool_catalog.cpp)

sak_add_test(test_system_tool_launcher
    tests/unit/test_system_tool_launcher.cpp)
```

### Link Dependencies

No new library dependencies. The implementation uses only:
- **Qt Core** (QProcess, QSettings, QDir, QFile) — already linked
- **Qt Widgets** (QFrame, QGridLayout, QGroupBox, QLineEdit) — already linked
- **Windows API** (ExtractIconExW, Shell32.lib) — already linked via Qt platform plugin

---

## 📋 Configuration & Settings

### QSettings Keys

```
SystemTools/favorites       = QStringList    # List of favorite tool IDs
SystemTools/collapsed       = QStringList    # List of collapsed category names
SystemTools/showUnavailable = bool           # Show/hide unavailable tools (default: true)
SystemTools/lastSearch      = QString        # Restore last search query (optional)
```

### Constants

```cpp
// In include/sak/system_tools_constants.h

namespace sak::system_tools {

/// Card dimensions
constexpr int kCardMinWidth = 180;
constexpr int kCardMaxWidth = 240;
constexpr int kCardHeight = 80;
constexpr int kCardIconSize = 24;
constexpr int kCardSpacing = 8;

/// Grid layout
constexpr int kGridColumns = 3;          // Cards per row (adjusts with resize)
constexpr int kCategorySpacing = 12;     // Space between category groups

/// Search
constexpr int kSearchDebounceMs = 150;   // Debounce delay for search input
constexpr int kMaxSearchResults = 100;   // Safety bound

/// Tooltips
constexpr int kTooltipDelayMs = 500;     // Hover delay before tooltip appears
constexpr int kTooltipMaxWidth = 350;    // Max tooltip width in pixels

/// Total tool count
constexpr int kTotalToolCount = 48;      // Total tools in catalog

} // namespace sak::system_tools
```

---

## 🧪 Testing Strategy

### Unit Tests

**test_system_tool_catalog.cpp**:
- Catalog builds with expected tool count (48)
- All categories populated (7 categories)
- Each tool has non-empty: id, name, description, tooltip, category, executable
- No duplicate tool IDs
- Filter by name returns correct tools
- Filter by category returns correct tools
- Filter by executable name returns correct tools
- Empty filter returns all tools
- Case-insensitive search
- Favorites: add, remove, persist, load
- Availability detection: existing tool returns true
- Availability detection: nonexistent tool returns false
- Edition detection: returns valid enum value
- Edition support check: Pro+ tool reports correctly on Home vs Pro

**test_system_tool_launcher.cpp**:
- Executable launch method resolves correct path
- MscSnapin launch method builds `mmc.exe <snap-in>` command
- ControlPanelApplet builds `control.exe <applet>` command
- ShellExecute method constructs valid URL
- CommandPrompt method builds `cmd.exe /k <args>` command
- Path resolution uses `%SystemRoot%` (not hardcoded `C:\Windows`)
- Launch failure returns false and logs error
- Launch success returns true and logs info

### Integration Tests

**test_system_tools_integration.cpp** (optional, manual):
- Live launch of `winver.exe` (safe, non-destructive, all editions)
- Verify process starts via `QProcess::startDetached` return value
- Catalog availability scan on the current machine
- Favorites round-trip: save → reload → compare

### Manual Test Matrix

| Test Case | Expected Result |
|-----------|----------------|
| Open System Tools tab | All categories visible, tools populated |
| Click "System Information" | `msinfo32.exe` launches |
| Click "Device Manager" | `devmgmt.msc` opens in MMC |
| Click "Disk Cleanup" | `cleanmgr.exe` launches |
| Click "Windows Security" | Settings app opens to Windows Security |
| Click unavailable tool (e.g., gpedit on Home) | Tool is grayed out, click does nothing |
| Type "disk" in search | Only disk-related tools shown |
| Clear search | All tools visible again |
| Star a tool | Tool appears in Favorites section |
| Unstar a tool | Tool removed from Favorites |
| Close and reopen SAK | Favorites persist |
| Collapse a category | Category collapses, state persists on reopen |
| Test on Windows 10 Home | Pro+ tools show "Requires Pro" badge |
| Test on Windows 11 Pro | All tools available |

---

## 🚧 Limitations & Challenges

### Technical Limitations

**Windows Edition Detection**:
- ⚠️ `EditionID` registry key is the primary source but can be empty on some insider builds
- **Mitigation**: Fall back to `ProductName` parsing; default to `AllEditions` if uncertain

**Icon Extraction Quality**:
- ⚠️ Some `.msc` files have no custom icon (default MMC icon)
- ⚠️ `.cpl` icons may be low resolution (32×32 max)
- ⚠️ `ExtractIconExW` does not work on `ms-settings:` URIs
- **Mitigation**: Hybrid approach with Icons8 SVG fallback for consistent quality

**Tool Path Variability**:
- ⚠️ Some tools may be in different locations on Server editions
- ⚠️ `wt.exe` (Windows Terminal) may not be installed by default
- **Mitigation**: Dynamic path resolution via `%SystemRoot%`; check `App Paths` registry key as fallback; mark missing tools as unavailable

**UAC and Elevation**:
- ✅ SAK runs as admin (manifest), so elevated tools launch without UAC prompt
- ⚠️ If user somehow runs SAK without admin, elevated tools will trigger UAC
- **Mitigation**: No special handling needed — Windows handles UAC natively

**Process Lifecycle**:
- ⚠️ Launched tools are independent processes — SAK cannot track if they are still open
- ⚠️ Some tools (like Task Manager) may already be running
- **Mitigation**: Use `startDetached` (fire and forget); no process tracking needed

### Known Quirks

**`cleanmgr.exe` on Windows 11**:
- Windows 11 redirects `cleanmgr.exe` to the new Storage Sense in some builds
- Tooltip should note this behavior

**`gpedit.msc` on Home Edition**:
- Not included in Windows Home — but can be manually installed
- Show as unavailable by default; tooltip mentions manual install option

**`mstsc.exe` (Remote Desktop)**:
- Client is available on all editions; Remote Desktop *server* requires Pro+
- Tooltip should clarify this distinction

---

## 🎯 Success Metrics

| Metric | Target | Importance |
|--------|--------|------------|
| Tool catalog completeness | 48 tools across 7 categories | High |
| Tool launch success rate | 100% for available tools | Critical |
| Availability detection accuracy | 95%+ (edge cases on insider builds) | High |
| Search response time | < 50ms for 48 entries | High |
| Tab load time | < 200ms (all cards rendered) | High |
| Icon load time | < 500ms (async OS icon extraction) | Medium |
| Favorites persist correctly | 100% round-trip fidelity | High |
| Category collapse persist | 100% round-trip fidelity | Medium |
| Memory overhead | < 5 MB (icons cached) | Medium |

---

## 🔒 Security Considerations

### Process Launch Safety
- Tool executables are resolved from `%SystemRoot%\System32\` only — never from user-supplied paths
- No user input is passed to `QProcess::startDetached` as arguments (tools launch with no args or fixed args)
- `ms-settings:` URIs are hardcoded in the catalog — never constructed from user input
- Registry Editor (`regedit.exe`) and Group Policy Editor (`gpedit.msc`) are powerful tools — no additional warning needed since SAK already runs with admin privileges

### Catalog Integrity
- The tool catalog is compiled into the executable (not loaded from an external file)
- No external JSON/XML configuration that could be tampered with
- Tool entries are `constexpr`-friendly — defined in source code

### Shell Execute Safety
- `QDesktopServices::openUrl()` is used only with hardcoded `ms-settings:` URIs
- No user-controlled URLs are passed through this path

---

## 💡 Future Enhancements (Post-v1.0)

### v1.1 - Extended Catalog
- **Server Tools**: Add tools specific to Windows Server (DHCP Manager, DNS Manager, AD Users & Computers, etc.)
- **Third-Party Tool Detection**: Detect and offer shortcuts to popular third-party tools if installed (Sysinternals, Wireshark, etc.)
- **Custom Tool Entries**: Allow technicians to add custom tool shortcuts

### v1.2 - Enhanced Features
- **Tool History**: Track recently used tools for quick re-access
- **Tool Recommendations**: Based on the current diagnostic context, suggest relevant tools
- **Tool Chaining**: Define workflows (e.g., "Security Audit" runs a sequence of tools)
- **Dark/Light Icon Sets**: Auto-switch icons based on SAK theme
- **Sysinternals Integration**: If Sysinternals Suite is detected in PATH or `tools/`, offer shortcuts to Process Explorer, Autoruns, TCPView, etc.

---

## 📚 Resources

### Official Documentation
- [QProcess::startDetached](https://doc.qt.io/qt-6/qprocess.html#startDetached)
- [QDesktopServices::openUrl](https://doc.qt.io/qt-6/qdesktopservices.html#openUrl)
- [ExtractIconExW](https://learn.microsoft.com/windows/win32/api/shellapi/nf-shellapi-extracticonexw)
- [QFileIconProvider](https://doc.qt.io/qt-6/qfileiconprovider.html)
- [ms-settings: URI scheme](https://learn.microsoft.com/windows/uwp/launch-resume/launch-settings-app)
- [Windows System32 tools reference](https://learn.microsoft.com/windows-server/administration/windows-commands/windows-commands)
- [MMC Snap-in reference](https://learn.microsoft.com/troubleshoot/windows-server/system-management-components/what-is-microsoft-management-console)

### Icons8 MCP Queries (for Phase 3)
- Search: "system tools", style: Fluent/Windows 11
- Search: "wrench", "gear", "settings" for generic tool icons
- Search: "hard drive", "disk" for storage category
- Search: "shield", "security" for security category
- Search: "speedometer", "performance" for performance category
- Search: "terminal", "command line" for developer category
- Search: "network", "ethernet" for network category
- Search: "star", "favorite" for favorites toggle
- Search: "maintenance", "repair" for maintenance category
- Search: "information", "about" for system info category

---

## 📞 Support

**Questions?** Open a GitHub Discussion  
**Found a Bug?** Open a GitHub Issue  
**Want to Contribute?** See [CONTRIBUTING.md](../CONTRIBUTING.md)

---

**Document Version**: 1.0  
**Last Updated**: March 25, 2026  
**Author**: Randy Northrup  
**Status**: 📋 Planned — Ready for Implementation

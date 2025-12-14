# Quick Actions Panel - Comprehensive Implementation Plan

**Version**: 1.0  
**Date**: December 14, 2025  
**Status**: ✅ **Phase 1 Complete - Infrastructure & Initial Actions Implemented**  
**Target Release**: v0.6.0

---

## ✅ Implementation Status

### Completed (December 14, 2025)
- ✅ **Core Infrastructure** - QuickAction base class, ActionController, QuickActionsPanel UI
- ✅ **Threading & Admin Elevation** - Worker threads, UAC integration, privilege checking
- ✅ **Initial Actions** - DiskCleanupAction, QuickBooksBackupAction, ClearBrowserCacheAction
- ✅ **MainWindow Integration** - Added as first tab in application
- ✅ **Build Success** - Zero compilation errors, fully functional executable

### Files Created (11 files, 2,500+ lines)
- `include/sak/quick_action.h` + `src/core/quick_action.cpp` (289 lines)
- `include/sak/quick_action_controller.h` + `src/core/quick_action_controller.cpp` (545 lines)
- `include/sak/quick_actions_panel.h` + `src/gui/quick_actions_panel.cpp` (884 lines)
- `include/sak/actions/disk_cleanup_action.h` + `src/actions/disk_cleanup_action.cpp` (467 lines)
- `include/sak/actions/quickbooks_backup_action.h` + `src/actions/quickbooks_backup_action.cpp` (368 lines)
- `include/sak/actions/clear_browser_cache_action.h` + `src/actions/clear_browser_cache_action.cpp` (365 lines)

---

## 🎯 Executive Summary

The Quick Actions Panel provides one-click access to common PC technician tasks, system optimizations, and targeted backup operations. This panel is designed for speed and efficiency, enabling technicians to perform routine maintenance and emergency data protection without navigating complex wizards.

### Key Objectives
- ✅ **One-Click Operations** - No wizards, immediate execution with smart defaults
- ✅ **Common Tasks** - Frequently performed technician operations
- ✅ **Smart Backups** - Targeted backups for critical application data
- ✅ **System Optimization** - Safe cleanup and performance improvements
- ✅ **Emergency Recovery** - Quick data protection before repairs
- ✅ **Progress Visibility** - Real-time feedback for all operations

---

## 📊 Project Scope

### Quick Actions Categories

#### 1. 🚀 System Optimization
**Goal**: Safe performance improvements with one click

**Actions**:
- ✅ **Disk Cleanup** - Clear Windows temp files, downloads, recycle bin *(IMPLEMENTED)*
- ✅ **Clear Browser Caches** - Chrome, Firefox, Edge cached data *(IMPLEMENTED)*
- **Defragment Drives** - Schedule or run defrag on HDDs (skip SSDs)
- **Clear Windows Update Cache** - Free space from old updates
- **Disable Startup Programs** - Show list of high-impact startup items
- **Clear Event Logs** - Archive and clear Application/System logs
- **Optimize Power Settings** - Switch to High Performance mode
- **Disable Visual Effects** - Reduce animations for performance

#### 2. 💾 Quick Backups
**Goal**: Targeted backups for critical application data

**Actions**:
- ✅ **QuickBooks Company Files** - Backup .QBW, .QBB, .TLG files *(IMPLEMENTED)*
- **Browser Profiles** - Chrome, Firefox, Edge profiles + bookmarks + passwords
- **Email Data (Outlook)** - PST/OST files + account settings
- **Sticky Notes** - Windows Sticky Notes database
- **Desktop & Documents** - Fast backup of common folders
- **Saved Game Data** - Steam, Epic Games, GOG saves
- **Tax Software Data** - TurboTax, H&R Block data files
- **Photo Management** - Lightroom catalogs, Photoshop settings
- **Development Configs** - Git configs, SSH keys, IDE settings

#### 3. 🛠️ Maintenance Tasks
**Goal**: Routine maintenance operations

**Actions**:
- **Check Disk Health** - SMART status for all drives
- **Update All Apps (Chocolatey)** - `choco upgrade all -y`
- **Windows Update Check** - Trigger update scan
- **Verify System Files** - Run SFC /scannow
- **Check Disk Errors** - Run CHKDSK /scan
- **Rebuild Icon Cache** - Fix broken/missing icons
- **Reset Network Adapters** - Flush DNS, reset TCP/IP stack
- **Clear Print Spooler** - Stop service, clear jobs, restart

#### 4. 🔧 Troubleshooting
**Goal**: Quick diagnostic and repair actions

**Actions**:
- **Generate System Report** - Hardware, software, drivers (HTML report)
- **Export Installed Apps** - List of all installed programs (CSV/JSON)
- **Check for Bloatware** - Detect common bloatware/adware
- **Test Network Speed** - Built-in speed test (local & internet)
- **Scan for Malware** - Windows Defender quick scan
- **Check Driver Updates** - List outdated drivers
- **Repair Windows Store** - Reset Windows Store cache
- **Fix Audio Issues** - Restart audio services, rebuild cache

#### 5. 🚨 Emergency Recovery
**Goal**: Fast data protection before system work

**Actions**:
- **Emergency User Backup** - Backup current user profile (Documents, Desktop, Pictures only)
- **Backup Browser Data** - All browsers + passwords + bookmarks
- **Backup Email** - Outlook PST/OST files
- **Create System Restore Point** - Force immediate restore point
- **Export Registry Hive** - Backup HKCU and HKLM to .reg files
- **Backup Activation Keys** - Scan and export all found license keys
- **Screenshot All Settings** - Auto-capture screenshots of common settings screens

---

## 🎨 User Interface Design

### Quick Actions Panel Layout

```
┌─────────────────────────────────────────────────────────────────────┐
│ Quick Actions Panel                                      [? Help]   │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌─────────────────── 🚀 SYSTEM OPTIMIZATION ──────────────────┐  │
│  │                                                               │  │
│  │  [🗑️ Disk Cleanup]    [🌐 Clear Browser Cache]              │  │
│  │  Frees: ~2.3 GB      Frees: ~450 MB                          │  │
│  │                                                               │  │
│  │  [💨 Disable Heavy Startup Programs]  [📊 Optimize Power]   │  │
│  │  Impact: 15 programs                 Mode: Balanced → High   │  │
│  │                                                               │  │
│  │  [✨ Disable Visual Effects]  [🗂️ Clear Event Logs]         │  │
│  │  Speed boost: 10-15%          Size: 128 MB                   │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌────────────────────── 💾 QUICK BACKUPS ──────────────────────┐  │
│  │                                                               │  │
│  │  [💼 QuickBooks Files]  [🌐 Browser Profiles]                │  │
│  │  Found: 3 company files  Browsers: 2 (Chrome, Edge)          │  │
│  │  Size: 156 MB           Size: 234 MB                         │  │
│  │                                                               │  │
│  │  [📧 Outlook Email]  [📝 Sticky Notes]  [🎮 Game Saves]     │  │
│  │  2 PST files         Notes: 12          Steam: 45 games      │  │
│  │  Size: 2.4 GB        Size: 2 MB         Size: 890 MB         │  │
│  │                                                               │  │
│  │  [🖼️ Desktop & Documents]  [💰 Tax Software Data]           │  │
│  │  Size: 5.2 GB              TurboTax 2024: 45 MB              │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌──────────────────── 🛠️ MAINTENANCE TASKS ────────────────────┐  │
│  │                                                               │  │
│  │  [💿 Check Disk Health]  [⬆️ Update All Apps]               │  │
│  │  SMART: All OK           12 updates available                │  │
│  │                                                               │  │
│  │  [🔍 Verify System Files]  [🌐 Reset Network]               │  │
│  │  SFC /scannow              Flush DNS + reset TCP/IP          │  │
│  │                                                               │  │
│  │  [🖨️ Clear Print Spooler]  [🎨 Rebuild Icon Cache]         │  │
│  │  3 stuck jobs              Fix broken icons                  │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌───────────────────── 🔧 TROUBLESHOOTING ─────────────────────┐  │
│  │                                                               │  │
│  │  [📋 Generate System Report]  [🚫 Check for Bloatware]      │  │
│  │  HTML report with specs      Found: 7 suspicious programs    │  │
│  │                                                               │  │
│  │  [🌐 Test Network Speed]  [🛡️ Quick Malware Scan]          │  │
│  │  Download/Upload/Ping     Windows Defender                   │  │
│  │                                                               │  │
│  │  [🔌 Check Driver Updates]  [🏪 Repair Windows Store]       │  │
│  │  3 outdated drivers         Reset wsreset                    │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌────────────────── 🚨 EMERGENCY RECOVERY ─────────────────────┐  │
│  │                                                               │  │
│  │  [⚡ EMERGENCY USER BACKUP]  [🔑 BACKUP ACTIVATION KEYS]    │  │
│  │  Current user only           Registry scan for licenses      │  │
│  │  Destination: D:\Emergency\  Export to keys.txt              │  │
│  │                                                               │  │
│  │  [💾 CREATE RESTORE POINT]  [📸 SCREENSHOT SETTINGS]        │  │
│  │  Force immediate restore     Auto-capture 20+ screens        │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌────────────────────── ⚙️ SETTINGS ───────────────────────────┐  │
│  │                                                               │  │
│  │  Quick Backup Location: [D:\SAK_QuickBackups\] [Browse...]  │  │
│  │  ☑ Confirm before executing    ☑ Show notifications         │  │
│  │  ☑ Create log for all actions  ☐ Auto-compress backups      │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌──────────────────── 📊 LAST OPERATION ───────────────────────┐  │
│  │                                                               │  │
│  │  Action: QuickBooks Backup                                   │  │
│  │  Progress: ████████████████████████  100%                    │  │
│  │  Status: ✅ Success - 3 files backed up (156 MB)             │  │
│  │  Location: D:\SAK_QuickBackups\QuickBooks_2025-12-13_143022  │  │
│  │  Duration: 2.3 seconds                                       │  │
│  │                                                               │  │
│  │  [📁 Open Backup Folder]  [📋 View Log]                      │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### Progress Dialog

When an action is clicked, show a non-modal progress dialog:

```
┌──────────────────────────────────────────┐
│ Clearing Browser Caches...              │
├──────────────────────────────────────────┤
│                                          │
│  Current: Chrome cache                  │
│  Progress: ████████░░░░░░░░░  50%      │
│                                          │
│  Files deleted: 4,523                   │
│  Space freed: 234 MB                    │
│                                          │
│  [Cancel]                               │
│                                          │
└──────────────────────────────────────────┘
```

---

## 🏗️ Architecture Overview

### Component Hierarchy

```
QuickActionsPanel (QWidget)
├─ SystemOptimizationSection (QGroupBox)
│  ├─ DiskCleanupAction
│  ├─ ClearBrowserCacheAction
│  ├─ DisableStartupProgramsAction
│  └─ ...
│
├─ QuickBackupsSection (QGroupBox)
│  ├─ QuickBooksBackupAction
│  ├─ BrowserProfileBackupAction
│  ├─ OutlookBackupAction
│  └─ ...
│
├─ MaintenanceSection (QGroupBox)
│  ├─ CheckDiskHealthAction
│  ├─ UpdateAllAppsAction
│  └─ ...
│
├─ TroubleshootingSection (QGroupBox)
│  ├─ GenerateSystemReportAction
│  ├─ CheckBloatwareAction
│  └─ ...
│
├─ EmergencyRecoverySection (QGroupBox)
│  ├─ EmergencyUserBackupAction
│  ├─ BackupActivationKeysAction
│  └─ ...
│
├─ QuickActionController (QObject)
│  ├─ Orchestrates all actions
│  ├─ Manages action queue
│  ├─ Handles confirmations
│  └─ Logs all operations
│
└─ QuickActionWorkers (QThread subclasses)
   ├─ DiskCleanupWorker
   ├─ BackupWorker (generic, reusable)
   ├─ SystemMaintenanceWorker
   └─ ...
```

### Action Base Class

All actions inherit from `QuickAction` base class:

```cpp
class QuickAction : public QObject {
    Q_OBJECT
public:
    enum ActionCategory {
        SystemOptimization,
        QuickBackup,
        Maintenance,
        Troubleshooting,
        EmergencyRecovery
    };
    
    enum ActionStatus {
        Idle,
        Scanning,      // Detecting what to backup/clean
        Running,       // Executing action
        Success,
        Failed,
        Cancelled
    };
    
    virtual QString name() const = 0;
    virtual QString description() const = 0;
    virtual ActionCategory category() const = 0;
    virtual QIcon icon() const = 0;
    
    // Pre-scan to show size/count before execution
    virtual void scan() = 0;
    virtual QString scanSummary() const = 0; // e.g., "Frees: 2.3 GB"
    
    // Execute the action
    virtual void execute() = 0;
    
    // Check if action is applicable (e.g., QuickBooks installed)
    virtual bool isApplicable() const = 0;
    
    // Requires admin privileges?
    virtual bool requiresElevation() const = 0;
    
    // Estimated duration (for UX)
    virtual int estimatedDurationSeconds() const = 0;
    
Q_SIGNALS:
    void scanStarted();
    void scanComplete(QString summary);
    void executionStarted();
    void progressUpdated(int percent, QString currentTask);
    void executionComplete(ActionStatus status, QString message);
    void errorOccurred(QString error);
};
```

---

## 📋 Detailed Action Specifications

### 1. System Optimization Actions

#### Disk Cleanup Action

**Purpose**: Free disk space by removing temporary files

**Implementation**:
```cpp
class DiskCleanupAction : public QuickAction {
public:
    void scan() override {
        // Scan for cleanable files
        scanTempFiles();         // %TEMP%, C:\Windows\Temp
        scanDownloads();         // Downloads folder (files older than 30 days)
        scanRecycleBin();        // Recycle bin
        scanThumbnailCache();    // Thumbnail cache
        scanErrorReports();      // Windows Error Reporting files
        
        m_totalSize = calculateTotalSize();
    }
    
    void execute() override {
        // Delete with progress updates
        deleteFiles(m_tempFiles);
        deleteFiles(m_oldDownloads);
        emptyRecycleBin();
        clearThumbnailCache();
        deleteErrorReports();
        
        emit executionComplete(Success, 
            QString("Freed %1 of disk space").arg(formatSize(m_totalSize)));
    }
};
```

**Settings**:
- Include Downloads folder: Yes/No
- Age threshold for Downloads: 30 days (configurable)
- Include Windows Update cleanup: Yes/No

---

#### Clear Browser Caches Action

**Purpose**: Clear cached data from all browsers

**Browsers Supported**:
- Google Chrome
- Microsoft Edge
- Mozilla Firefox
- Opera
- Brave

**Implementation**:
```cpp
class ClearBrowserCacheAction : public QuickAction {
public:
    void scan() override {
        // Detect installed browsers
        detectInstalledBrowsers();
        
        // Calculate cache sizes
        for (auto& browser : m_browsers) {
            browser.cacheSize = calculateCacheSize(browser.cachePath);
        }
        
        m_totalSize = sumCacheSizes();
    }
    
    void execute() override {
        // Close browsers first
        for (auto& browser : m_browsers) {
            closeProcesses(browser.processNames); // chrome.exe, firefox.exe, etc.
        }
        
        // Clear caches
        for (auto& browser : m_browsers) {
            clearCache(browser.cachePath);
            clearCookies(browser.cookiesPath);     // Optional
            clearHistory(browser.historyPath);     // Optional
        }
        
        emit executionComplete(Success, 
            QString("Cleared %1 from %2 browsers")
                .arg(formatSize(m_totalSize))
                .arg(m_browsers.size()));
    }
};
```

**Cache Locations**:
```cpp
// Chrome
%LOCALAPPDATA%\Google\Chrome\User Data\Default\Cache
%LOCALAPPDATA%\Google\Chrome\User Data\Default\Code Cache

// Edge
%LOCALAPPDATA%\Microsoft\Edge\User Data\Default\Cache

// Firefox
%APPDATA%\Mozilla\Firefox\Profiles\*.default-release\cache2
```

**Settings**:
- Clear cookies: Yes/No
- Clear history: Yes/No
- Clear saved passwords: No (default, dangerous)

---

#### Disable Startup Programs Action

**Purpose**: Show high-impact startup programs for user to disable

**Implementation**:
```cpp
class DisableStartupProgramsAction : public QuickAction {
public:
    struct StartupProgram {
        QString name;
        QString path;
        QString location; // Registry, Startup folder, Task Scheduler
        bool enabled;
        QString impact;   // High, Medium, Low
    };
    
    void scan() override {
        // Scan registry keys
        scanRegistryKey(HKCU, "Software\\Microsoft\\Windows\\CurrentVersion\\Run");
        scanRegistryKey(HKLM, "Software\\Microsoft\\Windows\\CurrentVersion\\Run");
        
        // Scan startup folder
        scanStartupFolder("%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\Startup");
        scanStartupFolder("%PROGRAMDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\Startup");
        
        // Scan Task Scheduler (tasks set to run at logon)
        scanScheduledTasks();
        
        // Estimate impact (based on file size, known programs)
        estimateImpact();
    }
    
    void execute() override {
        // Show dialog with list of programs
        auto dialog = new StartupProgramsDialog(m_programs);
        if (dialog->exec() == QDialog::Accepted) {
            auto disabledPrograms = dialog->getDisabledPrograms();
            
            for (auto& program : disabledPrograms) {
                disableStartupProgram(program);
            }
            
            emit executionComplete(Success,
                QString("Disabled %1 startup programs").arg(disabledPrograms.size()));
        }
    }
};
```

**Impact Heuristics**:
- **High Impact**: File size > 50 MB, known heavy programs (Adobe Creative Cloud, Steam, Discord)
- **Medium Impact**: File size 10-50 MB, productivity tools
- **Low Impact**: File size < 10 MB, system utilities

---

### 2. Quick Backup Actions

#### QuickBooks Backup Action

**Purpose**: Backup QuickBooks company files, backups, and transaction logs

**Files Backed Up**:
```cpp
// QuickBooks Desktop file locations
C:\Users\Public\Documents\Intuit\QuickBooks\Company Files\*.QBW    // Company files
C:\Users\Public\Documents\Intuit\QuickBooks\Company Files\*.QBB    // Backup files
C:\Users\Public\Documents\Intuit\QuickBooks\Company Files\*.TLG    // Transaction logs
C:\Users\Public\Documents\Intuit\QuickBooks\Company Files\*.ND     // Network data
C:\Users\Public\Documents\Intuit\QuickBooks\Company Files\*.QBM    // Portable files

// QuickBooks Online (no local files)
// QuickBooks settings
%APPDATA%\Intuit\QuickBooks\*.ini
```

**Implementation**:
```cpp
class QuickBooksBackupAction : public QuickAction {
public:
    void scan() override {
        // Search common locations
        QStringList searchPaths = {
            "C:\\Users\\Public\\Documents\\Intuit\\QuickBooks\\Company Files",
            "C:\\Users\\Public\\Public Documents\\Intuit\\QuickBooks\\Company Files",
            QDir::homePath() + "\\Documents\\Intuit\\QuickBooks\\Company Files"
        };
        
        for (const auto& path : searchPaths) {
            findQuickBooksFiles(path);
        }
        
        // Check if QuickBooks is running
        m_isQuickBooksRunning = isProcessRunning("QBW32.exe") || 
                                 isProcessRunning("QBW64.exe");
        
        m_totalSize = calculateTotalSize(m_foundFiles);
    }
    
    void execute() override {
        if (m_isQuickBooksRunning) {
            auto reply = QMessageBox::question(nullptr, "QuickBooks Running",
                "QuickBooks is currently running. Close it before backing up?",
                QMessageBox::Yes | QMessageBox::No);
            
            if (reply == QMessageBox::Yes) {
                terminateProcess("QBW32.exe");
                terminateProcess("QBW64.exe");
                QThread::sleep(2); // Wait for graceful shutdown
            } else {
                emit executionComplete(Cancelled, "Backup cancelled by user");
                return;
            }
        }
        
        // Create timestamped backup folder
        QString backupPath = QString("%1/QuickBooks_%2")
            .arg(ConfigManager::instance().getQuickBackupLocation())
            .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd_HHmmss"));
        
        QDir().mkpath(backupPath);
        
        // Copy files with progress
        for (const auto& file : m_foundFiles) {
            copyFileWithProgress(file, backupPath);
        }
        
        // Create backup manifest
        createManifest(backupPath, m_foundFiles);
        
        emit executionComplete(Success,
            QString("Backed up %1 QuickBooks files (%2) to %3")
                .arg(m_foundFiles.size())
                .arg(formatSize(m_totalSize))
                .arg(backupPath));
    }
};
```

**Backup Manifest** (JSON):
```json
{
  "backup_type": "QuickBooks",
  "timestamp": "2025-12-13T14:30:22Z",
  "hostname": "WORK-PC-01",
  "username": "Accountant",
  "files": [
    {
      "original_path": "C:\\Users\\Public\\Documents\\Intuit\\QuickBooks\\Company Files\\MyCompany.QBW",
      "filename": "MyCompany.QBW",
      "size": 156234567,
      "modified": "2025-12-13T10:15:00Z",
      "sha256": "abc123..."
    }
  ],
  "total_size": 156234567,
  "quickbooks_version": "QuickBooks Desktop 2024"
}
```

---

#### Browser Profile Backup Action

**Purpose**: Backup browser profiles including bookmarks, passwords, and settings

**Data Backed Up**:

**Chrome/Edge**:
```cpp
// Profile locations
%LOCALAPPDATA%\Google\Chrome\User Data\Default\
%LOCALAPPDATA%\Microsoft\Edge\User Data\Default\

// Critical files
Bookmarks                    // Bookmarks
Login Data                   // Saved passwords (encrypted)
Preferences                  // Settings
History                      // Browsing history
Web Data                     // Autofill data
Extensions\*\*\*             // Installed extensions
```

**Firefox**:
```cpp
// Profile location
%APPDATA%\Mozilla\Firefox\Profiles\*.default-release\

// Critical files
places.sqlite                // Bookmarks + history
logins.json                  // Saved passwords (encrypted)
key4.db                      // Master password encryption key
prefs.js                     // Settings
extensions.json              // Installed extensions
```

**Implementation**:
```cpp
class BrowserProfileBackupAction : public QuickAction {
public:
    void scan() override {
        // Detect browsers and profiles
        detectChrome();
        detectEdge();
        detectFirefox();
        
        m_totalSize = calculateTotalSize();
    }
    
    void execute() override {
        // Close browsers
        closeAllBrowsers();
        
        // Create backup folder
        QString backupPath = QString("%1/BrowserProfiles_%2")
            .arg(ConfigManager::instance().getQuickBackupLocation())
            .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd_HHmmss"));
        
        // Backup each browser
        for (const auto& browser : m_browsers) {
            QString browserBackupPath = QString("%1/%2").arg(backupPath, browser.name);
            QDir().mkpath(browserBackupPath);
            
            // Copy critical files only (not cache)
            for (const auto& file : browser.criticalFiles) {
                copyFileWithProgress(file, browserBackupPath);
            }
        }
        
        // Create manifest
        createManifest(backupPath);
        
        emit executionComplete(Success,
            QString("Backed up %1 browser profiles (%2)")
                .arg(m_browsers.size())
                .arg(formatSize(m_totalSize)));
    }
};
```

**Security Note**: Browser passwords are encrypted with Windows DPAPI. Backups can only be restored on the same Windows user account or with the master password.

---

#### Outlook Email Backup Action

**Purpose**: Backup Outlook PST/OST files and account settings

**Files Backed Up**:
```cpp
// PST files (Personal Folders)
// OST files (Offline Folders)
// Account settings

// Default locations
%LOCALAPPDATA%\Microsoft\Outlook\*.pst
%LOCALAPPDATA%\Microsoft\Outlook\*.ost
%APPDATA%\Microsoft\Outlook\*.xml         // Account settings

// Old Outlook versions
%USERPROFILE%\Documents\Outlook Files\*.pst
```

**Implementation**:
```cpp
class OutlookBackupAction : public QuickAction {
public:
    void scan() override {
        // Search for PST/OST files
        findOutlookDataFiles();
        
        // Check if Outlook is running
        m_isOutlookRunning = isProcessRunning("OUTLOOK.EXE");
        
        // Get account settings
        exportAccountSettings();
        
        m_totalSize = calculateTotalSize();
    }
    
    void execute() override {
        if (m_isOutlookRunning) {
            auto reply = QMessageBox::warning(nullptr, "Outlook Running",
                "Outlook is currently running. Data files may be locked.\n"
                "Close Outlook before backing up?",
                QMessageBox::Yes | QMessageBox::No);
            
            if (reply == QMessageBox::Yes) {
                terminateProcess("OUTLOOK.EXE");
                QThread::sleep(5); // Outlook takes time to close
            }
        }
        
        // Create backup folder
        QString backupPath = QString("%1/Outlook_%2")
            .arg(ConfigManager::instance().getQuickBackupLocation())
            .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd_HHmmss"));
        
        QDir().mkpath(backupPath);
        
        // Copy PST/OST files
        for (const auto& file : m_dataFiles) {
            copyFileWithProgress(file, backupPath);
        }
        
        // Export account settings
        exportRegistryKey("HKCU\\Software\\Microsoft\\Office\\16.0\\Outlook\\Profiles",
                          QString("%1/OutlookProfiles.reg").arg(backupPath));
        
        emit executionComplete(Success,
            QString("Backed up %1 Outlook data files (%2)")
                .arg(m_dataFiles.size())
                .arg(formatSize(m_totalSize)));
    }
};
```

**Warning**: OST files are synchronized with Exchange/Office 365. They can be rebuilt. PST files are local-only and critical.

---

### 3. Maintenance Actions

#### Check Disk Health Action

**Purpose**: Check SMART status for all drives

**Implementation**:
```cpp
class CheckDiskHealthAction : public QuickAction {
public:
    struct DriveHealth {
        QString driveLetter;
        QString model;
        QString serial;
        qint64 totalSize;
        QString smartStatus;     // OK, Warning, Critical
        int temperature;         // Celsius
        int powerOnHours;
        int reallocatedSectors;
        bool isSSD;
    };
    
    void scan() override {
        // Query WMI for drive info
        queryWMI("SELECT * FROM Win32_DiskDrive");
        
        // Get SMART attributes via WMI
        queryWMI("SELECT * FROM MSStorageDriver_FailurePredictStatus");
        queryWMI("SELECT * FROM MSStorageDriver_FailurePredictData");
        
        // Parse SMART attributes
        parseSmartData();
    }
    
    void execute() override {
        // Generate HTML report
        QString report = generateHealthReport();
        
        // Save report
        QString reportPath = QString("%1/DiskHealth_%2.html")
            .arg(ConfigManager::instance().getQuickBackupLocation())
            .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd_HHmmss"));
        
        QFile file(reportPath);
        file.open(QIODevice::WriteOnly);
        file.write(report.toUtf8());
        file.close();
        
        // Show dialog with results
        auto dialog = new DiskHealthDialog(m_drives);
        dialog->exec();
        
        emit executionComplete(Success, "Disk health check complete");
    }
};
```

**SMART Attributes Monitored**:
- **Reallocated Sectors Count** (ID 5) - Critical
- **Current Pending Sector Count** (ID 197) - Warning
- **Uncorrectable Sector Count** (ID 198) - Critical
- **Temperature** (ID 194) - Informational
- **Power-On Hours** (ID 9) - Informational

**Health Status Logic**:
- ✅ **OK**: All critical attributes within normal range
- ⚠️ **Warning**: Some non-critical issues (high temp, pending sectors)
- ❌ **Critical**: Reallocated or uncorrectable sectors > 0

---

#### Update All Apps (Chocolatey) Action

**Purpose**: Update all Chocolatey packages to latest versions

**Implementation**:
```cpp
class UpdateAllAppsAction : public QuickAction {
public:
    void scan() override {
        // Get list of outdated packages
        auto result = ChocolateyManager::instance().runCommand("outdated", {});
        
        // Parse output
        parseOutdatedPackages(result.output);
        
        m_updateCount = m_outdatedPackages.size();
    }
    
    void execute() override {
        if (m_updateCount == 0) {
            emit executionComplete(Success, "All packages are up to date");
            return;
        }
        
        // Show confirmation with package list
        auto dialog = new UpdateConfirmationDialog(m_outdatedPackages);
        if (dialog->exec() != QDialog::Accepted) {
            emit executionComplete(Cancelled, "Update cancelled by user");
            return;
        }
        
        // Run: choco upgrade all -y
        auto result = ChocolateyManager::instance().upgradeAll();
        
        if (result.success) {
            emit executionComplete(Success,
                QString("Updated %1 packages").arg(m_updateCount));
        } else {
            emit executionComplete(Failed,
                QString("Update failed: %1").arg(result.error));
        }
    }
};
```

**Confirmation Dialog** shows:
- Package name
- Current version → New version
- Size of download
- Total estimated time

---

### 4. Troubleshooting Actions

#### Generate System Report Action

**Purpose**: Create comprehensive HTML report of system configuration

**Report Sections**:

1. **Hardware Summary**:
   - CPU: Model, cores, threads, frequency
   - RAM: Total, available, speed, configuration
   - GPU: Model, VRAM, driver version
   - Storage: Drives, capacity, free space, health

2. **Operating System**:
   - Windows version, build, edition
   - Installation date
   - Last update date
   - Activation status

3. **Installed Software**:
   - All installed programs (from registry)
   - Chocolatey packages
   - Windows Store apps

4. **Drivers**:
   - All installed drivers
   - Driver versions
   - Outdated drivers (highlighted)

5. **Network Configuration**:
   - IP addresses (all adapters)
   - DNS servers
   - Gateway
   - MAC addresses

6. **Startup Programs**:
   - All startup items
   - Impact (High/Medium/Low)

7. **Event Log Summary**:
   - Critical errors (last 7 days)
   - Warnings (last 7 days)

**Implementation**:
```cpp
class GenerateSystemReportAction : public QuickAction {
public:
    void execute() override {
        // Collect data from WMI
        collectHardwareInfo();
        collectOSInfo();
        collectInstalledSoftware();
        collectDriverInfo();
        collectNetworkInfo();
        collectStartupPrograms();
        collectEventLogSummary();
        
        // Generate HTML report
        QString html = generateHTMLReport();
        
        // Save report
        QString reportPath = QString("%1/SystemReport_%2_%3.html")
            .arg(ConfigManager::instance().getQuickBackupLocation())
            .arg(QHostInfo::localHostName())
            .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd_HHmmss"));
        
        QFile file(reportPath);
        file.open(QIODevice::WriteOnly);
        file.write(html.toUtf8());
        file.close();
        
        // Open in browser
        QDesktopServices::openUrl(QUrl::fromLocalFile(reportPath));
        
        emit executionComplete(Success,
            QString("System report saved to %1").arg(reportPath));
    }
};
```

**Report Template** (HTML with embedded CSS):
```html
<!DOCTYPE html>
<html>
<head>
    <style>
        body { font-family: 'Segoe UI', sans-serif; margin: 20px; }
        h1 { color: #2c3e50; border-bottom: 2px solid #3498db; }
        h2 { color: #34495e; margin-top: 30px; }
        table { border-collapse: collapse; width: 100%; margin: 10px 0; }
        th { background: #3498db; color: white; padding: 10px; text-align: left; }
        td { padding: 8px; border-bottom: 1px solid #ddd; }
        .warning { color: #e74c3c; font-weight: bold; }
        .ok { color: #27ae60; }
    </style>
</head>
<body>
    <h1>System Report - {HOSTNAME}</h1>
    <p>Generated: {TIMESTAMP}</p>
    
    <h2>Hardware Summary</h2>
    <table>
        <tr><th>Component</th><th>Details</th></tr>
        <tr><td>CPU</td><td>{CPU_INFO}</td></tr>
        <tr><td>RAM</td><td>{RAM_INFO}</td></tr>
        <!-- ... -->
    </table>
    
    <!-- More sections... -->
</body>
</html>
```

---

#### Check for Bloatware Action

**Purpose**: Detect common bloatware and adware

**Bloatware Database** (JSON):
```json
{
  "bloatware_signatures": [
    {
      "name": "PC Accelerate Pro",
      "type": "Fake Optimizer",
      "severity": "high",
      "detection_methods": [
        "registry:HKLM\\Software\\PCAccelerate",
        "file:%ProgramFiles%\\PCAccelerate\\*",
        "process:PCAccelerate.exe"
      ]
    },
    {
      "name": "Ask Toolbar",
      "type": "Browser Hijacker",
      "severity": "medium",
      "detection_methods": [
        "browser_extension:chrome:ask_toolbar",
        "registry:HKCU\\Software\\Ask.com"
      ]
    },
    {
      "name": "McAfee WebAdvisor",
      "type": "Bundled Software",
      "severity": "low",
      "detection_methods": [
        "program:McAfee WebAdvisor"
      ]
    }
  ]
}
```

**Common Bloatware Categories**:
- Fake optimizers (PC Accelerate, Driver Booster, etc.)
- Browser hijackers (Ask Toolbar, Babylon Toolbar, etc.)
- Adware (Conduit, Mindspark, etc.)
- Bundled software (McAfee, Norton trials, etc.)
- OEM bloatware (HP Sure Click, Dell SupportAssist, etc.)

**Implementation**:
```cpp
class CheckBloatwareAction : public QuickAction {
public:
    void scan() override {
        // Load bloatware database
        loadBloatwareDatabase();
        
        // Scan installed programs
        scanInstalledPrograms();
        
        // Scan browser extensions
        scanBrowserExtensions();
        
        // Scan startup items
        scanStartupItems();
        
        // Match against database
        matchBloatware();
    }
    
    void execute() override {
        if (m_foundBloatware.isEmpty()) {
            QMessageBox::information(nullptr, "Bloatware Check",
                "No bloatware detected. System looks clean!");
            emit executionComplete(Success, "No bloatware found");
            return;
        }
        
        // Show dialog with found bloatware
        auto dialog = new BloatwareRemovalDialog(m_foundBloatware);
        if (dialog->exec() == QDialog::Accepted) {
            auto itemsToRemove = dialog->getSelectedItems();
            
            // Uninstall selected bloatware
            for (const auto& item : itemsToRemove) {
                uninstallProgram(item);
            }
            
            emit executionComplete(Success,
                QString("Removed %1 bloatware items").arg(itemsToRemove.size()));
        }
    }
};
```

---

### 5. Emergency Recovery Actions

#### Emergency User Backup Action

**Purpose**: Fast backup of current user's critical data

**Data Backed Up** (current user only):
- Documents folder
- Desktop folder
- Pictures folder
- Downloads folder (optional)
- Browser bookmarks + passwords
- Email PST files (if found)
- Sticky Notes

**NOT Backed Up** (to save time):
- Videos folder (usually large)
- Music folder (usually large)
- AppData (except browser profiles)
- Hidden files/folders

**Implementation**:
```cpp
class EmergencyUserBackupAction : public QuickAction {
public:
    void scan() override {
        QString username = qEnvironmentVariable("USERNAME");
        QString userProfile = qEnvironmentVariable("USERPROFILE");
        
        // Calculate sizes
        m_documentsSize = calculateFolderSize(userProfile + "/Documents");
        m_desktopSize = calculateFolderSize(userProfile + "/Desktop");
        m_picturesSize = calculateFolderSize(userProfile + "/Pictures");
        
        // Find browser profiles
        findBrowserProfiles();
        
        // Find email files
        findOutlookFiles();
        
        m_totalSize = m_documentsSize + m_desktopSize + 
                      m_picturesSize + m_browserSize + m_emailSize;
    }
    
    void execute() override {
        // Create emergency backup folder
        QString backupPath = QString("%1/EMERGENCY_%2_%3")
            .arg(ConfigManager::instance().getQuickBackupLocation())
            .arg(qEnvironmentVariable("USERNAME"))
            .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd_HHmmss"));
        
        QDir().mkpath(backupPath);
        
        // Show progress dialog
        auto progress = new QProgressDialog(
            "Emergency backup in progress...", 
            "Cancel", 0, 100);
        progress->setWindowModality(Qt::WindowModal);
        progress->show();
        
        // Copy folders with progress
        copyFolderWithProgress(QDir::homePath() + "/Documents", 
                               backupPath + "/Documents", progress);
        copyFolderWithProgress(QDir::homePath() + "/Desktop",
                               backupPath + "/Desktop", progress);
        copyFolderWithProgress(QDir::homePath() + "/Pictures",
                               backupPath + "/Pictures", progress);
        
        // Copy browser profiles
        copyBrowserProfiles(backupPath + "/BrowserProfiles");
        
        // Copy email files
        copyOutlookFiles(backupPath + "/Email");
        
        // Create manifest
        createManifest(backupPath);
        
        progress->close();
        
        QMessageBox::information(nullptr, "Emergency Backup Complete",
            QString("Emergency backup complete!\n\n"
                    "Location: %1\n"
                    "Size: %2\n"
                    "Duration: %3")
                .arg(backupPath)
                .arg(formatSize(m_totalSize))
                .arg(formatDuration(m_elapsedSeconds)));
        
        emit executionComplete(Success,
            QString("Emergency backup saved to %1").arg(backupPath));
    }
};
```

**Target Speed**: Complete in < 5 minutes for typical user (5-10 GB)

---

#### Create System Restore Point Action

**Purpose**: Force immediate Windows System Restore point

**Implementation**:
```cpp
class CreateRestorePointAction : public QuickAction {
public:
    void execute() override {
        // Check if System Restore is enabled
        if (!isSystemRestoreEnabled()) {
            QMessageBox::warning(nullptr, "System Restore Disabled",
                "System Restore is disabled on this PC.\n"
                "Enable it in System Properties?");
            // TODO: Offer to enable System Restore
            emit executionComplete(Failed, "System Restore is disabled");
            return;
        }
        
        // Create restore point via WMI
        // COM object: "SystemRestore.CreateRestorePoint"
        QString description = QString("SAK Utility - Manual Restore Point - %1")
            .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm"));
        
        bool success = createRestorePointWMI(description);
        
        if (success) {
            QMessageBox::information(nullptr, "Restore Point Created",
                QString("System restore point created successfully:\n\n%1")
                    .arg(description));
            emit executionComplete(Success, "Restore point created");
        } else {
            emit executionComplete(Failed, "Failed to create restore point");
        }
    }
    
private:
    bool createRestorePointWMI(const QString& description) {
        // PowerShell command:
        // Checkpoint-Computer -Description "SAK Utility" -RestorePointType MODIFY_SETTINGS
        
        QProcess process;
        process.start("powershell.exe", QStringList() 
            << "-NoProfile" 
            << "-Command"
            << QString("Checkpoint-Computer -Description '%1' -RestorePointType MODIFY_SETTINGS")
               .arg(description));
        
        process.waitForFinished(60000); // 60 second timeout
        
        return process.exitCode() == 0;
    }
};
```

**Note**: Windows limits System Restore points to 1 per 24 hours by default. Override with registry edit if needed.

---

## 📂 File Structure

### New Files to Create

#### Headers (`include/sak/`)

```
quick_actions_panel.h              # Main UI panel
quick_action_base.h                # Base class for all actions
quick_action_controller.h          # Orchestrates actions, manages queue

// System Optimization Actions
disk_cleanup_action.h
clear_browser_cache_action.h
disable_startup_programs_action.h
optimize_power_action.h

// Quick Backup Actions
quickbooks_backup_action.h
browser_profile_backup_action.h
outlook_backup_action.h
sticky_notes_backup_action.h
game_saves_backup_action.h

// Maintenance Actions
check_disk_health_action.h
update_all_apps_action.h
verify_system_files_action.h
reset_network_action.h
clear_print_spooler_action.h

// Troubleshooting Actions
generate_system_report_action.h
check_bloatware_action.h
test_network_speed_action.h

// Emergency Recovery Actions
emergency_user_backup_action.h
create_restore_point_action.h
backup_activation_keys_action.h
screenshot_settings_action.h
```

#### Implementation (`src/`)

```
gui/quick_actions_panel.cpp
core/quick_action_base.cpp
core/quick_action_controller.cpp

// Actions
core/actions/disk_cleanup_action.cpp
core/actions/clear_browser_cache_action.cpp
core/actions/quickbooks_backup_action.cpp
core/actions/browser_profile_backup_action.cpp
// ... (all other actions)
```

#### Resources

```
resources/icons/quick_actions/
├─ disk_cleanup.svg
├─ browser_cache.svg
├─ quickbooks.svg
├─ browser_profile.svg
├─ email.svg
├─ emergency_backup.svg
└─ ...
```

---

## 🔧 Implementation Phases

### Phase 1: Foundation (Week 1-2)

**Goals**:
- Create UI skeleton
- Implement base classes
- 3-5 sample actions

**Tasks**:
1. Create `QuickActionsPanel` UI with collapsible sections
2. Create `QuickAction` base class with signals
3. Create `QuickActionController` for orchestration
4. Implement 3 sample actions:
   - Disk Cleanup
   - QuickBooks Backup
   - Emergency User Backup
5. Add progress tracking
6. Add logging
7. Write unit tests for base classes

**Acceptance Criteria**:
- ✅ UI displays action buttons
- ✅ Actions execute with progress
- ✅ Results shown to user
- ✅ Logs created

---

### Phase 2: System Optimization (Week 3)

**Tasks**:
1. Implement `ClearBrowserCacheAction`
2. Implement `DisableStartupProgramsAction`
3. Implement `OptimizePowerAction`
4. Implement `DisableVisualEffectsAction`
5. Write tests for each action

**Acceptance Criteria**:
- ✅ All 4 actions working
- ✅ Size/impact shown before execution
- ✅ Confirmation dialogs shown

---

### Phase 3: Quick Backups (Week 4-5)

**Tasks**:
1. Implement `BrowserProfileBackupAction`
2. Implement `OutlookBackupAction`
3. Implement `StickyNotesBackupAction`
4. Implement `GameSavesBackupAction`
5. Implement `TaxSoftwareBackupAction`
6. Create backup manifest format (JSON)
7. Write tests

**Acceptance Criteria**:
- ✅ All 5 backup actions working
- ✅ Manifests created
- ✅ Backups compressed (optional)

---

### Phase 4: Maintenance & Troubleshooting (Week 6)

**Tasks**:
1. Implement `CheckDiskHealthAction` (SMART)
2. Implement `UpdateAllAppsAction` (Chocolatey)
3. Implement `VerifySystemFilesAction` (SFC)
4. Implement `ResetNetworkAction`
5. Implement `GenerateSystemReportAction`
6. Implement `CheckBloatwareAction`
7. Write tests

**Acceptance Criteria**:
- ✅ All actions working
- ✅ HTML reports generated
- ✅ SMART data parsed correctly

---

### Phase 5: Emergency Recovery & Polish (Week 7-8)

**Tasks**:
1. Implement `CreateRestorePointAction`
2. Implement `BackupActivationKeysAction` (reuse License Scanner)
3. Implement `ScreenshotSettingsAction`
4. Add action queue (run multiple actions sequentially)
5. Add keyboard shortcuts (Ctrl+1, Ctrl+2, etc.)
6. Error handling improvements
7. User documentation
8. Update README.md

**Acceptance Criteria**:
- ✅ All actions implemented
- ✅ Robust error handling
- ✅ Documentation complete

---

## 📋 Configuration & Settings

### ConfigManager Extensions

Add to `config_manager.h/cpp`:

```cpp
// Quick Actions Settings
QString getQuickBackupLocation() const;
void setQuickBackupLocation(const QString& location);

bool getQuickActionsConfirmBeforeExecute() const;
void setQuickActionsConfirmBeforeExecute(bool confirm);

bool getQuickActionsShowNotifications() const;
void setQuickActionsShowNotifications(bool show);

bool getQuickActionsCreateLogs() const;
void setQuickActionsCreateLogs(bool create);

bool getQuickActionsAutoCompressBackups() const;
void setQuickActionsAutoCompressBackups(bool compress);

// Disk Cleanup Settings
bool getDiskCleanupIncludeDownloads() const;
void setDiskCleanupIncludeDownloads(bool include);

int getDiskCleanupDownloadsAgeDays() const;
void setDiskCleanupDownloadsAgeDays(int days);

// Browser Cache Settings
bool getClearBrowserCacheIncludeCookies() const;
void setClearBrowserCacheIncludeCookies(bool include);

bool getClearBrowserCacheIncludeHistory() const;
void setClearBrowserCacheIncludeHistory(bool include);

// QuickBooks Settings
QStringList getQuickBooksCustomLocations() const;
void setQuickBooksCustomLocations(const QStringList& locations);

// Emergency Backup Settings
bool getEmergencyBackupIncludeDownloads() const;
void setEmergencyBackupIncludeDownloads(bool include);
```

**Default Values**:
```cpp
quick_actions/backup_location = "D:\\SAK_QuickBackups"
quick_actions/confirm_before_execute = true
quick_actions/show_notifications = true
quick_actions/create_logs = true
quick_actions/auto_compress_backups = false

disk_cleanup/include_downloads = false
disk_cleanup/downloads_age_days = 30

clear_browser_cache/include_cookies = false
clear_browser_cache/include_history = false

emergency_backup/include_downloads = false
```

---

## 🧪 Testing Strategy

### Unit Tests

**test_quick_action_base.cpp**:
- Signal emission
- Status transitions
- Progress updates

**test_disk_cleanup_action.cpp**:
- File scanning
- Size calculation
- File deletion (mock filesystem)

**test_quickbooks_backup_action.cpp**:
- File detection
- Process detection
- Backup creation

### Integration Tests

**test_quick_actions_integration.cpp**:
- Full action execution
- Progress tracking
- Error handling
- Manifest creation

### Manual Testing

1. **Disk Cleanup**:
   - Create temp files
   - Run cleanup
   - Verify deletion

2. **Browser Backups**:
   - Create test profiles
   - Run backup
   - Verify all files copied

3. **Emergency Backup**:
   - Time execution
   - Verify < 5 minutes for 10 GB

---

## 🎯 Success Metrics

| Action | Target Duration | Target Success Rate |
|--------|-----------------|---------------------|
| Disk Cleanup | < 30 seconds | 99% |
| QuickBooks Backup | < 10 seconds | 95% |
| Browser Profile Backup | < 1 minute | 95% |
| Emergency User Backup | < 5 minutes | 90% |
| System Report | < 2 minutes | 99% |

---

## 🚧 Risks & Mitigation

| Risk | Impact | Mitigation |
|------|--------|------------|
| **Deleting wrong files** | Critical | Confirmation dialogs, dry-run mode, logs |
| **Locked files (Outlook PST)** | Medium | Detect running processes, prompt to close |
| **Disk full during backup** | High | Check free space before starting |
| **QuickBooks file not found** | Low | Search multiple locations, allow custom paths |

---

## 📅 Timeline

**Total**: 8 weeks

| Week | Phase | Deliverables |
|------|-------|--------------|
| 1-2 | Foundation | UI, base classes, 3 sample actions |
| 3 | System Optimization | 4 optimization actions |
| 4-5 | Quick Backups | 5 backup actions |
| 6 | Maintenance & Troubleshooting | 6 actions + reports |
| 7-8 | Emergency & Polish | 3 emergency actions + docs |

**Target Release**: v0.6.0 (Q1 2026)

---

**Document Version**: 1.0  
**Last Updated**: December 13, 2025  
**Author**: Randy Northrup  
**Status**: ✅ Ready for Implementation

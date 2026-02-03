# Windows PE Bootable Version - Comprehensive Implementation Plan

**Version**: 1.0  
**Date**: December 13, 2025  
**Status**: ❌ Scrapped / Archived  
**Target Release**: N/A

**Decision (Feb 1, 2026):** This plan is no longer in scope and has been archived. PXE Boot Panel is the active focus.

---

## 🎯 Executive Summary

A bootable Windows PE version of S.A.K. Utility enables PC technicians to perform data recovery, user profile migrations, and system diagnostics without booting into the installed operating system. This is critical for scenarios where Windows won't boot, hardware is being replaced, or a clean offline environment is needed.

### Key Objectives
- ✅ **Offline Operations** - Work without booting Windows
- ✅ **Data Recovery** - Access files when OS won't boot
- ✅ **Hardware-Independent** - Boot on any PC via USB
- ✅ **Full Feature Set** - Most SAK features available in PE
- ✅ **Driver Support** - Network, storage, USB drivers included
- ✅ **Quick Boot** - < 2 minute boot time from USB

---

## 📊 Project Scope

### What is Windows PE?

**Windows Preinstallation Environment (WinPE)** is a lightweight version of Windows designed for:
- Deployment and recovery
- Hardware-independent booting
- Network support
- Storage driver loading
- Running Windows applications (with limitations)

**Current Version**: Windows PE 11 (based on Windows 11)  
**Previous Versions**: WinPE 10 (Windows 10), WinPE 8.1, etc.

**Size**: ~400-500 MB base image (can grow to 1-2 GB with drivers and tools)

---

## 🎯 Use Cases

### 1. **Data Recovery**
**Scenario**: Customer's Windows won't boot due to corrupt system files

**Workflow**:
1. Boot from SAK USB drive
2. Launch SAK Utility in WinPE
3. Access user profiles on dead OS
4. Backup Documents, Desktop, Pictures to external drive
5. Optionally wipe drive and reinstall Windows

**Features Used**:
- User Profile Backup (offline mode)
- File Scanner
- Disk Health Check
- SMART monitoring

---

### 2. **PC Replacement**
**Scenario**: Migrating user from old PC to new PC (both working)

**Workflow**:
1. Boot old PC from SAK USB
2. Scan user profiles and applications
3. Backup to network share or external drive
4. Boot new PC from SAK USB (or boot normally)
5. Restore user profiles
6. Install applications via Chocolatey

**Features Used**:
- User Profile Backup
- Application Scanner
- Network Transfer (future)
- Chocolatey installation

---

### 3. **Offline Diagnostics**
**Scenario**: Customer reports slow PC, malware suspected

**Workflow**:
1. Boot from SAK USB (clean environment)
2. Run offline malware scan
3. Check disk health (SMART)
4. Scan for bloatware
5. Generate system report
6. Export license keys (before reinstall)

**Features Used**:
- Disk Health Check
- Bloatware Scanner
- License Key Scanner
- System Report Generator

---

### 4. **Drive Cloning**
**Scenario**: Upgrading HDD to SSD before OS won't boot anymore

**Workflow**:
1. Boot from SAK USB
2. Connect new SSD
3. Clone entire drive (partition table + data)
4. Verify clone integrity
5. Swap drives

**Features Used**:
- Drive Scanner
- Image Flasher (modified for disk-to-disk cloning)
- SHA-256 verification

---

### 5. **Emergency Network Backup**
**Scenario**: PC is about to die, need to backup critical data over network

**Workflow**:
1. Boot from SAK USB
2. Connect to network (Ethernet or WiFi)
3. Map network share (NAS, file server)
4. Backup user profiles to network
5. Verify backup integrity

**Features Used**:
- Network drivers
- User Profile Backup
- Network file operations

---

## 🏗️ Technical Architecture

### WinPE Boot Structure

```
SAK_WinPE_USB/
├─ Boot/
│  ├─ boot.wim              # WinPE boot image (400 MB)
│  ├─ BCD                   # Boot Configuration Data
│  └─ bootmgr               # Windows Boot Manager
│
├─ EFI/
│  ├─ Boot/
│  │  └─ bootx64.efi        # UEFI boot loader
│  └─ Microsoft/
│     └─ Boot/
│        ├─ BCD             # UEFI Boot Configuration
│        └─ bootmgfw.efi    # UEFI boot manager
│
├─ SAK_Utility/
│  ├─ sak_utility.exe       # Main application
│  ├─ Qt6Core.dll           # Qt dependencies
│  ├─ Qt6Gui.dll
│  ├─ Qt6Widgets.dll
│  ├─ Qt6Network.dll
│  ├─ platforms/            # Qt platform plugin
│  │  └─ qwindows.dll
│  ├─ tools/
│  │  └─ chocolatey/        # Embedded Chocolatey
│  └─ drivers/              # Extra drivers (optional)
│     ├─ network/
│     ├─ storage/
│     └─ usb/
│
├─ startnet.cmd             # Auto-start script
└─ winpeshl.ini             # PE shell configuration
```

---

## 🛠️ Building the WinPE Image

### Prerequisites

**Required Software**:
- **Windows ADK** (Assessment and Deployment Kit)
  - Download: https://learn.microsoft.com/windows-hardware/get-started/adk-install
  - Size: ~2-5 GB
  - Components needed:
    - Deployment Tools
    - Windows Preinstallation Environment (Windows PE)
    
- **Windows ADK PE Add-on** (if separate)
  - Matches ADK version

**Build Environment**:
- Windows 10/11 x64
- Administrator privileges
- 20+ GB free disk space
- USB drive 8+ GB (16 GB recommended)

---

### Build Process

#### Step 1: Install Windows ADK

```powershell
# Download and install Windows ADK
# https://go.microsoft.com/fwlink/?linkid=2271337

# Install ADK with required features
adksetup.exe /quiet /features OptionId.DeploymentTools OptionId.WindowsPreinstallationEnvironment

# Install WinPE add-on (if separate)
adkwinpesetup.exe /quiet /features OptionId.WindowsPreinstallationEnvironment
```

---

#### Step 2: Create WinPE Working Directory

```powershell
# Open "Deployment and Imaging Tools Environment" as Administrator

# Create working directory
copype amd64 C:\WinPE_SAK

# Directory structure created:
# C:\WinPE_SAK\
#   ├─ fwfiles\        # Firmware files (UEFI)
#   ├─ media\          # Final bootable media
#   │  ├─ Boot\
#   │  ├─ EFI\
#   │  └─ sources\
#   │     └─ boot.wim  # The main WinPE image
#   └─ mount\          # Mount point for editing boot.wim
```

---

#### Step 3: Mount WinPE Image for Customization

```powershell
# Mount the boot.wim for editing
Dism /Mount-Wim /WimFile:C:\WinPE_SAK\media\sources\boot.wim /Index:1 /MountDir:C:\WinPE_SAK\mount

# Image is now mounted at C:\WinPE_SAK\mount
# This is a temporary Windows file system we can modify
```

---

#### Step 4: Add Optional Components (Drivers, Features)

**Network Support** (Critical for network backups):
```powershell
# Add WinPE-WMI (Windows Management Instrumentation)
Dism /Image:C:\WinPE_SAK\mount /Add-Package /PackagePath:"C:\Program Files (x86)\Windows Kits\10\Assessment and Deployment Kit\Windows Preinstallation Environment\amd64\WinPE_OCs\WinPE-WMI.cab"

# Add WinPE-NetFx (required for some .NET apps)
Dism /Image:C:\WinPE_SAK\mount /Add-Package /PackagePath:"C:\Program Files (x86)\Windows Kits\10\Assessment and Deployment Kit\Windows Preinstallation Environment\amd64\WinPE_OCs\WinPE-NetFx.cab"

# Add WinPE-Scripting (PowerShell support)
Dism /Image:C:\WinPE_SAK\mount /Add-Package /PackagePath:"C:\Program Files (x86)\Windows Kits\10\Assessment and Deployment Kit\Windows Preinstallation Environment\amd64\WinPE_OCs\WinPE-Scripting.cab"

# Add WinPE-PowerShell (full PowerShell)
Dism /Image:C:\WinPE_SAK\mount /Add-Package /PackagePath:"C:\Program Files (x86)\Windows Kits\10\Assessment and Deployment Kit\Windows Preinstallation Environment\amd64\WinPE_OCs\WinPE-PowerShell.cab"

# Add WinPE-StorageWMI (disk management)
Dism /Image:C:\WinPE_SAK\mount /Add-Package /PackagePath:"C:\Program Files (x86)\Windows Kits\10\Assessment and Deployment Kit\Windows Preinstallation Environment\amd64\WinPE_OCs\WinPE-StorageWMI.cab"

# Add WinPE-DismCmdlets (DISM PowerShell cmdlets)
Dism /Image:C:\WinPE_SAK\mount /Add-Package /PackagePath:"C:\Program Files (x86)\Windows Kits\10\Assessment and Deployment Kit\Windows Preinstallation Environment\amd64\WinPE_OCs\WinPE-DismCmdlets.cab"
```

**Graphics and GUI Support**:
```powershell
# Add WinPE-HTA (HTML Applications - for custom GUI)
Dism /Image:C:\WinPE_SAK\mount /Add-Package /PackagePath:"C:\Program Files (x86)\Windows Kits\10\Assessment and Deployment Kit\Windows Preinstallation Environment\amd64\WinPE_OCs\WinPE-HTA.cab"
```

---

#### Step 5: Add Third-Party Drivers

**Network Drivers** (Intel, Realtek, etc.):
```powershell
# Add driver recursively (searches subdirectories)
Dism /Image:C:\WinPE_SAK\mount /Add-Driver /Driver:C:\Drivers\Network /Recurse

# Common driver sources:
# - Dell: https://www.dell.com/support/home/en-us/drivers/driversdetails
# - HP: https://support.hp.com/us-en/drivers
# - Intel: https://downloadcenter.intel.com/
# - Realtek: https://www.realtek.com/en/downloads
```

**Storage Drivers** (NVMe, RAID, etc.):
```powershell
Dism /Image:C:\WinPE_SAK\mount /Add-Driver /Driver:C:\Drivers\Storage /Recurse
```

**USB Drivers** (if needed):
```powershell
Dism /Image:C:\WinPE_SAK\mount /Add-Driver /Driver:C:\Drivers\USB /Recurse
```

---

#### Step 6: Copy SAK Utility Files

```powershell
# Create SAK directory in WinPE
New-Item -Path C:\WinPE_SAK\mount\SAK_Utility -ItemType Directory

# Copy SAK executable and dependencies
Copy-Item -Path "C:\Users\Randy\Github\S.A.K.-Utility\build\Release\*" `
          -Destination C:\WinPE_SAK\mount\SAK_Utility `
          -Recurse

# Copy Qt DLLs (already in Release folder from windeployqt)
# Copy Chocolatey tools
Copy-Item -Path "C:\Users\Randy\Github\S.A.K.-Utility\tools\chocolatey" `
          -Destination C:\WinPE_SAK\mount\SAK_Utility\tools\chocolatey `
          -Recurse

# Copy any additional resources
Copy-Item -Path "C:\Users\Randy\Github\S.A.K.-Utility\resources" `
          -Destination C:\WinPE_SAK\mount\SAK_Utility\resources `
          -Recurse
```

---

#### Step 7: Create Auto-Start Script

**startnet.cmd** (runs automatically when WinPE boots):
```batch
@echo off
REM SAK Utility WinPE Auto-Start Script

echo.
echo ========================================
echo   S.A.K. Utility - Windows PE Edition
echo   Version 0.9.0
echo ========================================
echo.

REM Initialize network (DHCP)
echo Initializing network...
wpeinit

REM Wait for network initialization
timeout /t 10

REM Set high-performance power plan
powercfg /s 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c

REM Assign drive letters to all volumes
echo Assigning drive letters...
mountvol /E

REM Show available drives
echo.
echo Available drives:
wmic logicaldisk get name,description,size,freespace

REM Launch SAK Utility
echo.
echo Launching S.A.K. Utility...
X:\SAK_Utility\sak_utility.exe

REM If SAK exits, open command prompt
echo.
echo SAK Utility exited. Opening command prompt...
cmd.exe
```

**Copy startnet.cmd**:
```powershell
Copy-Item -Path C:\path\to\startnet.cmd `
          -Destination C:\WinPE_SAK\mount\Windows\System32\startnet.cmd `
          -Force
```

---

#### Step 8: Customize WinPE Shell (Optional)

**winpeshl.ini** (controls what runs at boot):
```ini
[LaunchApps]
%SYSTEMROOT%\System32\startnet.cmd
```

**Copy winpeshl.ini**:
```powershell
Copy-Item -Path C:\path\to\winpeshl.ini `
          -Destination C:\WinPE_SAK\mount\Windows\System32\winpeshl.ini `
          -Force
```

---

#### Step 9: Set WinPE Scratch Space (RAM Disk)

```powershell
# Increase scratch space for large operations (default 32 MB)
Dism /Image:C:\WinPE_SAK\mount /Set-ScratchSpace:512
```

---

#### Step 10: Optimize and Clean Up Image

```powershell
# Remove unnecessary files to reduce size
Dism /Image:C:\WinPE_SAK\mount /Cleanup-Image /StartComponentCleanup

# Reduce image size by removing superseded components
Dism /Image:C:\WinPE_SAK\mount /Cleanup-Image /StartComponentCleanup /ResetBase
```

---

#### Step 11: Unmount and Commit Changes

```powershell
# Unmount and save changes
Dism /Unmount-Wim /MountDir:C:\WinPE_SAK\mount /Commit

# Image is now saved at: C:\WinPE_SAK\media\sources\boot.wim
```

---

#### Step 12: Create Bootable USB Drive

**Option A: Using MakeWinPEMedia (Recommended)**:
```powershell
# Insert USB drive (will be formatted - backup data first!)
# Assuming USB drive is F:

# Create bootable USB (UEFI + BIOS support)
MakeWinPEMedia /UFD C:\WinPE_SAK\media F:

# This command:
# - Formats F: as FAT32
# - Copies all files from media\ to F:
# - Makes it bootable for both UEFI and legacy BIOS
```

**Option B: Manual Method**:
```powershell
# Format USB as FAT32
Format-Volume -DriveLetter F -FileSystem FAT32 -NewFileSystemLabel "SAK_WinPE"

# Copy all files
Copy-Item -Path C:\WinPE_SAK\media\* -Destination F:\ -Recurse

# Make bootable (BIOS)
bootsect /nt60 F: /mbr

# UEFI boot already supported via EFI folder
```

---

#### Step 13: Test the USB Drive

```powershell
# Boot from USB on test machine
# - Access BIOS/UEFI boot menu (usually F12, F8, or Del)
# - Select USB drive
# - Wait for WinPE to boot (30-90 seconds)
# - SAK Utility should auto-launch
```

---

## 🎨 WinPE-Specific Features

### Feature Availability Matrix

| Feature | WinPE Support | Notes |
|---------|---------------|-------|
| **User Profile Backup** | ✅ Full | Access offline profiles |
| **Application Scanner** | ✅ Full | Scan registry of offline Windows |
| **Package Matcher** | ✅ Full | No changes needed |
| **Chocolatey Install** | ⚠️ Limited | Can install to target OS, not PE |
| **User Profile Restore** | ✅ Full | Restore to offline profiles |
| **Directory Organizer** | ✅ Full | No changes needed |
| **Duplicate Finder** | ✅ Full | No changes needed |
| **License Key Scanner** | ✅ Full | Scan offline registry |
| **Image Flasher** | ✅ Full | Flash ISOs to USB (bootable USB creator) |
| **Network Transfer** | ✅ Full | Requires network drivers |
| **Quick Actions** | ⚠️ Mixed | Some actions require running OS |
| **Keep Awake** | ❌ N/A | Not needed in PE |

---

### WinPE Detection

Add to `platform_interface.h`:

```cpp
class PlatformInterface {
public:
    enum BootEnvironment {
        StandardWindows,
        WindowsPE,
        WindowsRE,      // Recovery Environment
        SafeMode
    };
    
    static BootEnvironment getBootEnvironment();
    static bool isWindowsPE();
    static QString getWindowsVersion();    // Returns "PE" if in WinPE
};
```

**Implementation**:
```cpp
bool PlatformInterface::isWindowsPE() {
    // Method 1: Check registry
    QSettings registry("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\MiniNT",
                       QSettings::NativeFormat);
    if (registry.contains(".")) {
        return true;  // MiniNT key exists only in WinPE
    }
    
    // Method 2: Check system root
    QString systemRoot = qEnvironmentVariable("SystemRoot");
    if (systemRoot.startsWith("X:\\", Qt::CaseInsensitive)) {
        return true;  // WinPE runs from X: (RAM drive)
    }
    
    // Method 3: Check Windows PE registry value
    QSettings winpe("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                    QSettings::NativeFormat);
    QString productName = winpe.value("ProductName").toString();
    if (productName.contains("Windows PE", Qt::CaseInsensitive)) {
        return true;
    }
    
    return false;
}
```

---

### Offline Registry Access

**Challenge**: Need to access registry of installed Windows (e.g., C:\Windows\System32\config\SOFTWARE)

**Solution**: Mount offline registry hives

```cpp
class OfflineRegistryManager {
public:
    /**
     * @brief Load offline registry hive
     * @param hivePath Path to hive file (e.g., C:\Windows\System32\config\SOFTWARE)
     * @param mountPoint Registry key to mount under (e.g., HKLM\OFFLINE_SOFTWARE)
     * @return true if successful
     */
    static bool loadHive(const QString& hivePath, const QString& mountPoint);
    
    /**
     * @brief Unload offline registry hive
     * @param mountPoint Registry key to unload
     * @return true if successful
     */
    static bool unloadHive(const QString& mountPoint);
    
    /**
     * @brief Get list of Windows installations on all drives
     * @return List of Windows directories (e.g., C:\Windows, D:\Windows)
     */
    static QStringList findWindowsInstallations();
};
```

**Implementation**:
```cpp
bool OfflineRegistryManager::loadHive(const QString& hivePath, const QString& mountPoint) {
    // Use reg.exe to load hive
    QProcess process;
    process.start("reg.exe", QStringList() 
        << "load"
        << mountPoint
        << QDir::toNativeSeparators(hivePath));
    
    process.waitForFinished();
    return process.exitCode() == 0;
}

bool OfflineRegistryManager::unloadHive(const QString& mountPoint) {
    QProcess process;
    process.start("reg.exe", QStringList() << "unload" << mountPoint);
    process.waitForFinished();
    return process.exitCode() == 0;
}

QStringList OfflineRegistryManager::findWindowsInstallations() {
    QStringList installations;
    
    // Scan all drives
    for (const QStorageInfo& storage : QStorageInfo::mountedVolumes()) {
        if (!storage.isValid() || storage.isReadOnly()) continue;
        
        QString windowsDir = storage.rootPath() + "/Windows";
        QFileInfo systemHive(windowsDir + "/System32/config/SYSTEM");
        
        if (systemHive.exists()) {
            installations.append(windowsDir);
        }
    }
    
    return installations;
}
```

**Usage in AppScanner**:
```cpp
void AppScanner::scanInstalledApplicationsPE() {
    // Find Windows installations
    QStringList installations = OfflineRegistryManager::findWindowsInstallations();
    
    if (installations.isEmpty()) {
        emit errorOccurred("No Windows installations found");
        return;
    }
    
    // Let user choose if multiple installations
    QString selectedWindows = installations.first();
    if (installations.size() > 1) {
        // Show dialog to select installation
        selectedWindows = showInstallationSelectionDialog(installations);
    }
    
    // Load SOFTWARE hive
    QString softwareHive = selectedWindows + "/System32/config/SOFTWARE";
    OfflineRegistryManager::loadHive(softwareHive, "HKLM\\OFFLINE_SOFTWARE");
    
    // Scan registry (using HKLM\OFFLINE_SOFTWARE instead of HKLM\SOFTWARE)
    scanRegistryKey("HKLM\\OFFLINE_SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall");
    scanRegistryKey("HKLM\\OFFLINE_SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall");
    
    // Unload hive
    OfflineRegistryManager::unloadHive("HKLM\\OFFLINE_SOFTWARE");
}
```

---

### Drive Detection in WinPE

**Challenge**: Drive letters may not be assigned automatically

**Solution**: Enumerate all volumes and assign letters

```cpp
class WinPEDriveManager {
public:
    /**
     * @brief Assign drive letters to all volumes
     */
    static void assignAllDriveLetters();
    
    /**
     * @brief Get list of all volumes with details
     */
    static QVector<VolumeInfo> enumerateVolumes();
};
```

**Implementation**:
```cpp
void WinPEDriveManager::assignAllDriveLetters() {
    // Run mountvol /E to enable automatic mounting
    QProcess::execute("mountvol.exe", QStringList() << "/E");
    
    // Alternatively, use diskpart script:
    QString diskpartScript = 
        "rescan\n"
        "list volume\n"
        "exit\n";
    
    QTemporaryFile scriptFile;
    scriptFile.open();
    scriptFile.write(diskpartScript.toUtf8());
    scriptFile.close();
    
    QProcess::execute("diskpart.exe", QStringList() << "/s" << scriptFile.fileName());
}
```

---

### Network Configuration in WinPE

**Challenge**: Network drivers must be loaded manually

**Solution**: wpeinit and netsh commands

```cpp
class WinPENetworkManager {
public:
    /**
     * @brief Initialize network (DHCP)
     */
    static bool initializeNetwork();
    
    /**
     * @brief Configure static IP
     */
    static bool configureStaticIP(const QString& ip, const QString& subnet, const QString& gateway);
    
    /**
     * @brief Get network adapter status
     */
    static QVector<NetworkAdapterInfo> getAdapters();
};
```

**Implementation**:
```cpp
bool WinPENetworkManager::initializeNetwork() {
    // Run wpeinit (loads network drivers and gets DHCP)
    QProcess process;
    process.start("wpeinit.exe");
    process.waitForFinished(30000);  // 30 second timeout
    
    return process.exitCode() == 0;
}

bool WinPENetworkManager::configureStaticIP(const QString& ip, 
                                             const QString& subnet, 
                                             const QString& gateway) {
    // Use netsh to configure static IP
    QProcess::execute("netsh", QStringList() 
        << "interface" << "ip" << "set" << "address"
        << "name=\"Ethernet\"" 
        << "source=static"
        << QString("addr=%1").arg(ip)
        << QString("mask=%1").arg(subnet)
        << QString("gateway=%1").arg(gateway));
    
    return true;
}
```

---

## 🎨 WinPE-Specific UI

### Startup Wizard

When SAK launches in WinPE, show a wizard:

```
┌────────────────────────────────────────────────┐
│ S.A.K. Utility - Windows PE Edition           │
├────────────────────────────────────────────────┤
│                                                │
│  Welcome to S.A.K. Utility Windows PE!        │
│                                                │
│  Running in: Windows PE 11 (Offline Mode)     │
│  Boot Drive: X:\ (RAM Disk)                   │
│                                                │
│  ┌──────────────────────────────────────────┐ │
│  │ Available Windows Installations:         │ │
│  │                                          │ │
│  │ ○ C:\Windows (Windows 11 Pro)           │ │
│  │   Size: 45 GB  |  Users: 3              │ │
│  │                                          │ │
│  │ ○ D:\Windows (Windows 10 Home)          │ │
│  │   Size: 35 GB  |  Users: 1              │ │
│  │                                          │ │
│  │ ○ None (Data Recovery Only)             │ │
│  └──────────────────────────────────────────┘ │
│                                                │
│  Network Status: ✅ Connected (DHCP)          │
│  IP: 192.168.1.100                            │
│                                                │
│  [Configure Network...]                       │
│                                                │
│  [Continue]  [Exit to Command Prompt]         │
│                                                │
└────────────────────────────────────────────────┘
```

---

### WinPE Indicator

Show persistent indicator that we're in WinPE:

```
┌────────────────────────────────────────────────────┐
│ S.A.K. Utility  |  🟢 WinPE Mode (Offline)        │
├────────────────────────────────────────────────────┤
│                                                    │
│  Target OS: C:\Windows (Windows 11 Pro)           │
│  Users: John, Sarah, Admin                        │
│                                                    │
│  [🔄 Change Target OS]                             │
│                                                    │
```

---

## 📦 Distribution

### Option 1: Pre-Built ISO

**Files**:
- `SAK_Utility_WinPE_v0.9.0.iso` (800 MB - 1.5 GB)
- SHA256SUMS.txt

**Usage**:
```powershell
# Burn to DVD (rare nowadays)
# OR flash to USB using Rufus, Etcher, or built-in tools

# Using Rufus:
# 1. Download Rufus: https://rufus.ie/
# 2. Insert USB drive (8+ GB)
# 3. Select SAK_Utility_WinPE_v0.9.0.iso
# 4. Partition scheme: GPT (UEFI) or MBR (BIOS)
# 5. Click Start

# Using Windows built-in tool:
$IsoPath = "C:\Downloads\SAK_Utility_WinPE_v0.9.0.iso"
$UsbDrive = "F:"

# Mount ISO
$mountResult = Mount-DiskImage -ImagePath $IsoPath -PassThru
$driveLetter = ($mountResult | Get-Volume).DriveLetter

# Copy files to USB
Copy-Item -Path "$driveLetter`:\*" -Destination "$UsbDrive\" -Recurse

# Unmount ISO
Dismount-DiskImage -ImagePath $IsoPath

# Make USB bootable
bootsect /nt60 $UsbDrive /mbr
```

---

### Option 2: WinPE Builder Tool

**Concept**: Ship a GUI tool that builds the WinPE image for the user

**Why?**
- Reduces download size (user provides Windows ADK)
- Always uses latest WinPE version
- User can customize drivers
- Avoids redistribution licensing issues

**SAK_WinPE_Builder.exe**:
```
┌─────────────────────────────────────────────────────┐
│ S.A.K. Utility - WinPE Builder                     │
├─────────────────────────────────────────────────────┤
│                                                     │
│  Step 1: Prerequisites                             │
│  ☑ Windows ADK Installed                           │
│  ☑ WinPE Add-on Installed                          │
│  ☑ USB Drive Connected (16 GB)                     │
│                                                     │
│  Step 2: Customization                             │
│  ☑ Include network drivers                         │
│  ☑ Include storage drivers (NVMe, RAID)            │
│  ☐ Include WiFi drivers (optional)                 │
│  ☐ Add custom drivers folder                       │
│                                                     │
│  Step 3: Build                                     │
│  Target: F:\ (USB Drive - 14.9 GB free)            │
│  Estimated time: 15-20 minutes                     │
│                                                     │
│  [Build WinPE USB]                                 │
│                                                     │
│  Progress: ████████████░░░░░░░░░░  60%            │
│  Current: Copying Qt dependencies...               │
│                                                     │
└─────────────────────────────────────────────────────┘
```

**Implementation**:
```cpp
class WinPEBuilder : public QObject {
    Q_OBJECT
public:
    void build(const QString& usbDrive, const WinPEBuildOptions& options);
    
Q_SIGNALS:
    void progressUpdated(int percent, QString message);
    void buildComplete(bool success, QString message);
    
private:
    void checkPrerequisites();
    void createWorkingDirectory();
    void mountBootWim();
    void addOptionalComponents();
    void addDrivers(const QStringList& driverPaths);
    void copySAKFiles();
    void createStartupScript();
    void unmountBootWim();
    void createBootableUSB(const QString& usbDrive);
};
```

---

### Option 3: Hybrid Approach

**Ship both**:
1. Pre-built ISO for quick use (no ADK needed)
2. Builder tool for customization

---

## 🧪 Testing Strategy

### Test Environments

1. **Virtual Machines**:
   - VirtualBox (set to boot from ISO)
   - VMware Workstation
   - Hyper-V

2. **Physical Hardware**:
   - UEFI system (modern PC)
   - Legacy BIOS system (older PC)
   - Laptop with WiFi (test WiFi drivers)
   - PC with NVMe SSD (test NVMe drivers)

### Test Scenarios

**Boot Test**:
```
1. Insert USB drive
2. Boot from USB (BIOS/UEFI menu)
3. Verify WinPE boots in < 2 minutes
4. Verify SAK auto-launches
5. Verify network initializes (DHCP)
6. Verify drives are assigned letters
```

**Feature Tests** (in WinPE):
```
1. User Profile Backup
   - Detect offline Windows installation
   - Scan users (C:\Users\*)
   - Backup to external drive
   - Verify files copied

2. Application Scanner
   - Load offline registry hive
   - Scan installed apps
   - Export to JSON
   - Unload registry hive

3. License Key Scanner
   - Load offline registry
   - Scan for license keys
   - Export to file

4. Network Backup
   - Connect to network share (\\NAS\Backups)
   - Backup user profile to network
   - Verify transfer
```

---

## 📋 Feature Adaptations for WinPE

### User Profile Backup

**Changes Needed**:
- Detect offline Windows installations
- Let user select which Windows to backup
- Load offline registry to enumerate users
- Access profile folders directly (C:\Users\John\Documents, etc.)

**Modified Workflow**:
```cpp
void UserProfileBackupWizard::initializeWinPEMode() {
    // Find Windows installations
    m_installations = OfflineRegistryManager::findWindowsInstallations();
    
    // Show installation selection page (new page)
    auto installPage = new InstallationSelectionPage(m_installations);
    setPage(Page_SelectInstallation, installPage);
    
    // After installation selected, enumerate users
    QString selectedWindows = installPage->getSelectedInstallation();
    
    // Enumerate users from C:\Users\* folders
    QDir usersDir(selectedWindows + "/../Users");
    for (const QString& userFolder : usersDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        UserProfile profile;
        profile.username = userFolder;
        profile.profilePath = usersDir.absoluteFilePath(userFolder);
        profile.fullName = getUserFullName(profile.profilePath);  // From NTUSER.DAT
        m_scannedUsers.append(profile);
    }
}
```

---

### Application Scanner

**Changes Needed**:
- Load offline SOFTWARE registry hive
- Scan for installed apps in offline hive
- Unload hive when done

**Modified Workflow**:
```cpp
void AppScanner::scanWinPE(const QString& windowsPath) {
    QString softwareHive = windowsPath + "/System32/config/SOFTWARE";
    
    // Load hive
    if (!OfflineRegistryManager::loadHive(softwareHive, "HKLM\\OFFLINE_SOFTWARE")) {
        emit errorOccurred("Failed to load SOFTWARE hive");
        return;
    }
    
    // Scan offline hive
    scanRegistryKeyOffline("HKLM\\OFFLINE_SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall");
    scanRegistryKeyOffline("HKLM\\OFFLINE_SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall");
    
    // Unload hive
    OfflineRegistryManager::unloadHive("HKLM\\OFFLINE_SOFTWARE");
}
```

---

### Chocolatey Manager

**Challenge**: Can't install packages to running WinPE (X:\)

**Solution**: Install to target OS (C:\)

```cpp
void ChocolateyManager::installPackageWinPE(const QString& packageName, 
                                             const QString& targetWindowsPath) {
    // Determine target path
    QString programData = targetWindowsPath + "/../ProgramData/chocolatey";
    
    // Set environment variables to target offline Windows
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("ChocolateyInstall", programData);
    
    // Run choco install with custom install directory
    // Packages will install to C:\Program Files, C:\ProgramData, etc.
    // on the offline Windows installation
    
    QProcess process;
    process.setProcessEnvironment(env);
    process.start(m_chocoPath, QStringList() 
        << "install" 
        << packageName 
        << "-y"
        << "--force"
        << QString("--installArgs='/DIR=%1\\Program Files\\%2'")
           .arg(targetWindowsPath.left(2))  // C:
           .arg(packageName));
    
    process.waitForFinished(-1);
}
```

**Note**: This is complex and may not work for all packages. Better to install after booting into the restored OS.

---

## 🚧 Limitations in WinPE

### What Doesn't Work

1. **Windows Services**: Many background services don't run in WinPE
2. **Windows Store Apps**: Can't install/run UWP apps
3. **Some Drivers**: May need to add drivers manually to WinPE image
4. **Persistent Changes**: WinPE runs from RAM; changes lost on reboot
5. **Limited RAM**: 512 MB default scratch space (configurable)
6. **No Hibernation**: WinPE doesn't support sleep/hibernate
7. **Limited .NET**: Only basic .NET Framework (no .NET Core/5+)

### Workarounds

**Persistent Storage**:
- Save all data to external drives or network shares
- Don't save anything to X:\ (RAM disk)

**Driver Issues**:
- Build multiple versions: WinPE with Intel drivers, WinPE with AMD drivers, etc.
- Or build custom WinPE with driver pack (e.g., Dell driver pack)

**RAM Limitations**:
- Increase scratch space: `/Set-ScratchSpace:1024` (1 GB)
- Close applications when done to free RAM

---

## 📅 Implementation Timeline

### Phase 1: Research & Setup (Week 1-2)

**Tasks**:
1. Install Windows ADK and WinPE add-on
2. Build basic WinPE image (no customization)
3. Test boot on physical hardware
4. Test boot on virtual machines (VirtualBox, VMware)
5. Document build process

**Deliverables**:
- Working vanilla WinPE image
- Build documentation

---

### Phase 2: SAK Integration (Week 3-4)

**Tasks**:
1. Add Qt dependencies to WinPE image
2. Copy SAK executable and DLLs
3. Create startnet.cmd auto-launch script
4. Test SAK launch in WinPE
5. Implement WinPE detection (`PlatformInterface::isWindowsPE()`)
6. Add WinPE startup wizard

**Deliverables**:
- SAK launches in WinPE
- WinPE mode detected and indicated in UI

---

### Phase 3: Offline Registry Access (Week 5-6)

**Tasks**:
1. Implement `OfflineRegistryManager` class
2. Modify `AppScanner` for offline registry
3. Modify `LicenseScanner` for offline registry
4. Implement Windows installation detection
5. Add installation selection dialog
6. Write tests

**Deliverables**:
- App scanning works in WinPE
- License scanning works in WinPE

---

### Phase 4: User Profile Backup in WinPE (Week 7-8)

**Tasks**:
1. Modify `UserProfileBackupWizard` for offline mode
2. Implement offline user enumeration (C:\Users\*)
3. Implement offline profile access
4. Test large backups (10+ GB)
5. Test backup to external drive
6. Test backup to network share

**Deliverables**:
- User profile backup works in WinPE
- Network backups work

---

### Phase 5: Driver Support (Week 9-10)

**Tasks**:
1. Research common network driver requirements
2. Add Intel network drivers to WinPE
3. Add Realtek network drivers to WinPE
4. Add NVMe storage drivers
5. Add RAID controller drivers (optional)
6. Test on various hardware

**Deliverables**:
- Network works on 90%+ of hardware
- Storage works on 95%+ of hardware

---

### Phase 6: WinPE Builder Tool (Week 11-13)

**Tasks**:
1. Design WinPE Builder GUI
2. Implement ADK prerequisite checking
3. Implement automated build process
4. Implement USB creation
5. Add driver import functionality
6. Write user documentation
7. Test on clean Windows installations

**Deliverables**:
- Working WinPE Builder tool
- User can build custom WinPE USB in 20 minutes

---

### Phase 7: Testing & Polish (Week 14-15)

**Tasks**:
1. Test on 10+ different PCs (UEFI and BIOS)
2. Test on laptops with WiFi
3. Test on NVMe systems
4. Performance optimization (boot time)
5. Documentation (README, troubleshooting guide)
6. Create pre-built ISO for download

**Deliverables**:
- Pre-built ISO ready for release
- Comprehensive documentation
- Tested on diverse hardware

---

**Total Timeline**: 15 weeks (4 months)  
**Target Release**: v0.9.0 (Q3 2026)

---

## 🎯 Success Metrics

| Metric | Target | Importance |
|--------|--------|------------|
| Boot time | < 2 minutes | High |
| Network compatibility | 90%+ hardware | High |
| Storage compatibility | 95%+ hardware | Critical |
| ISO size | < 1.5 GB | Medium |
| RAM usage | < 2 GB | Medium |
| Feature parity | 80%+ features work | High |

---

## 📚 Resources

### Official Documentation

- [Windows ADK Download](https://learn.microsoft.com/windows-hardware/get-started/adk-install)
- [WinPE Documentation](https://learn.microsoft.com/windows-hardware/manufacture/desktop/winpe-intro)
- [DISM Reference](https://learn.microsoft.com/windows-hardware/manufacture/desktop/dism-reference)
- [WinPE Optional Components](https://learn.microsoft.com/windows-hardware/manufacture/desktop/winpe-add-packages--optional-components-reference)

### Community Resources

- [WinPE Boot from USB](https://www.tenforums.com/tutorials/95419-create-bootable-usb-flash-drive-install-windows-10-a.html)
- [Offline Registry Editing](https://superuser.com/questions/165116/how-to-edit-offline-registry)
- [Adding Drivers to WinPE](https://learn.microsoft.com/windows-hardware/manufacture/desktop/add-device-drivers-to-windows-during-windows-setup)

---

## 💡 Future Enhancements (Post-v0.9)

### v1.0 - Advanced Features

**Remote Management**:
- Boot WinPE on client PC
- Technician controls remotely via VNC/RDP
- Automated backup jobs over network

**Automated Deployment**:
- PXE boot support (boot from network)
- Mass deployment to 100+ PCs
- Unattended backup scripts

**Additional Tools**:
- Partition editor (resize, delete, create)
- Boot loader repair (BCDEdit wrapper)
- Password reset tool
- File recovery (undelete)

---

## 🔒 Security Considerations

**WinPE Security**:
- ⚠️ **WinPE has no login** - Anyone can boot and access data
- ⚠️ **BitLocker drives are locked** - Need recovery key to access
- ⚠️ **UEFI Secure Boot** - May need to disable to boot custom WinPE

**Mitigation**:
- Encrypt USB drive with BitLocker To Go (optional)
- Use strong BIOS password
- Physical security (keep USB in safe location)

**BitLocker Support**:
```cpp
class BitLockerManager {
public:
    /**
     * @brief Check if drive is BitLocker-encrypted
     */
    static bool isBitLockerEncrypted(const QString& drive);
    
    /**
     * @brief Unlock BitLocker drive with recovery key
     */
    static bool unlockDrive(const QString& drive, const QString& recoveryKey);
};
```

---

## 📞 Support

**Questions?** Open a GitHub Discussion  
**Found a Bug?** Open a GitHub Issue  
**Want to Contribute?** See [CONTRIBUTING.md](CONTRIBUTING.md)

---

**Document Version**: 1.0  
**Last Updated**: December 13, 2025  
**Author**: Randy Northrup  
**Status**: ✅ Ready for Implementation

---

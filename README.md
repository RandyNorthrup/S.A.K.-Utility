# S.A.K. Utility

**Swiss Army Knife Utility** — A comprehensive Windows desktop toolkit for PC technicians, IT support professionals, and system administrators. Consolidates migration, maintenance, recovery, imaging, and deployment workflows into a single portable application.

[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://isocpp.org/)
[![Qt 6.5.3](https://img.shields.io/badge/Qt-6.5.3-green.svg)](https://www.qt.io/)
[![Windows](https://img.shields.io/badge/Platform-Windows%2010%2F11-blue.svg)](https://www.microsoft.com/windows)
[![Build Status](https://img.shields.io/github/actions/workflow/status/RandyNorthrup/S.A.K.-Utility/build-release.yml?branch=main)](https://github.com/RandyNorthrup/S.A.K.-Utility/actions)

---

## Table of Contents

- [About](#about)
- [Key Features](#key-features)
- [System Requirements](#system-requirements)
- [Quick Start](#quick-start)
- [Feature Documentation](#feature-documentation)
  - [Quick Actions](#quick-actions)
  - [User Profile Backup & Restore](#user-profile-backup--restore)
  - [Application Migration](#application-migration)
  - [Network Transfer](#network-transfer)
  - [Directory Organizer](#directory-organizer)
  - [Duplicate Finder](#duplicate-finder)
  - [Image Flasher](#image-flasher)
- [Security & Encryption](#security--encryption)
- [Building from Source](#building-from-source)
- [Configuration](#configuration)
- [Contributing](#contributing)
- [License](#license)
- [Author](#author)

---

## About

S.A.K. Utility is a **portable, production-ready** Windows application designed for technicians who need reliable, repeatable workflows for PC migration, system maintenance, data recovery, and deployment automation. Built with modern **C++23** and **Qt 6.5.3**, it provides enterprise-grade features with technician-friendly UIs.

**Current Version:** 0.5.6

**Platform:** Windows 10/11 (x64)

**Author:** Randy Northrup

**Architecture:** Multi-threaded C++23 with Qt6 GUI framework, Windows BCrypt cryptography, embedded Chocolatey package manager, and portable operation (no installation required).

---

## Key Features

### ✨ Highlights

- **100% Portable** — No installer. Run from USB drive or network share. Settings stored locally when `portable.ini` is present.
- **Embedded Tools** — Bundled Chocolatey package manager eliminates system dependencies.
- **Enterprise Security** — AES-256-GCM encryption using Windows BCrypt API (FIPS 140-2 compliant).
- **Multi-Threaded** — Background workers prevent UI freezes during long operations.
- **Comprehensive Logging** — Detailed operation logs with timestamps for audit trails.
- **Modern UI** — Windows 11-themed interface with rounded corners, custom splash screen, and responsive layouts.

### 🎯 Primary Use Cases

1. **PC Migration** — Transfer user profiles, applications, and data between machines
2. **System Maintenance** — One-click cleanup, updates, and health checks
3. **Data Recovery** — Emergency backups of critical files (QuickBooks, browsers, email)
4. **Deployment Automation** — Orchestrate multi-PC migrations via LAN
5. **Bootable Media** — Create Windows installation USB drives
6. **File Management** — Organize and deduplicate large file collections

---

## System Requirements

### Minimum Requirements

- **Operating System:** Windows 10 (1809) or Windows 11
- **Processor:** x64 (64-bit) Intel/AMD CPU
- **Memory:** 4 GB RAM
- **Disk Space:** 500 MB for application + variable for operations
- **Network:** Ethernet or Wi-Fi for Network Transfer features
- **Permissions:** Administrator privileges (required for disk operations, system maintenance)

### Build Requirements (Developers Only)

- **Visual Studio 2022** (v17+) with C++ Desktop Development workload
- **CMake** 3.28 or later
- **Qt 6.5.3** MSVC 2019 64-bit toolchain
- **vcpkg** (for dependency management)
- **Git** for version control

---

## Quick Start

### For Users

1. **Download** the latest release from [Releases](https://github.com/RandyNorthrup/S.A.K.-Utility/releases)
2. **Extract** the archive to a folder (e.g., `C:\Tools\SAK-Utility`)
3. **Run** `sak_utility.exe` (accepts administrator elevation prompt)
4. *(Optional)* Create an empty `portable.ini` file next to the executable for portable settings mode

### For Developers

```powershell
# Clone repository
git clone https://github.com/RandyNorthrup/S.A.K.-Utility.git
cd S.A.K.-Utility

# Configure build
cmake -B build -G "Visual Studio 17 2022" -A x64

# Build Release
cmake --build build --config Release --parallel

# Run executable
.\build\Release\sak_utility.exe
```

See [BUILD.md](BUILD.md) for comprehensive build instructions.

---

## Feature Documentation

### Quick Actions

**One-click maintenance and recovery operations** organized into five categories with real-time progress tracking and detailed logging.

#### Categories

##### 1. System Optimization (8 Actions)

- **Disk Cleanup** — Removes temporary files, browser caches, recycle bin, Windows Update cache, thumbnails using `cleanmgr.exe` with comprehensive profile configuration
- **Clear Browser Cache** — Clears cache for Chrome, Edge, Firefox, Brave, Vivaldi, Opera (all user profiles)
- **Defragment Drives** — Optimizes HDDs and trims SSDs using `defrag.exe` with media type detection
- **Clear Windows Update Cache** — Stops Windows Update service, clears `SoftwareDistribution` folder, restarts service
- **Disable Startup Programs** — Identifies and disables unnecessary startup items via Task Manager and registry
- **Clear Event Logs** — Archives and clears Windows Event Viewer logs (Application, System, Security, Setup)
- **Optimize Power Settings** — Applies High Performance or Ultimate Performance power plan
- **Disable Visual Effects** — Reduces animations for faster performance on older hardware

##### 2. Quick Backups (8 Actions)

- **QuickBooks Backup** — Backs up .QBW company files, .QBB backups, .TLG transaction logs, .ND network data from common locations
- **Browser Profile Backup** — Backs up Chrome/Edge/Firefox bookmarks, passwords (encrypted), history, extensions, preferences
- **Outlook Backup** — Backs up .PST/.OST files, account settings, signatures, rules, contacts
- **Sticky Notes Backup** — Backs up Windows Sticky Notes database (`plum.sqlite`)
- **Saved Game Data Backup** — Backs up Steam, Epic Games, Origin, Xbox Game Pass save files
- **Tax Software Backup** — Backs up TurboTax, H&R Block, TaxAct data files
- **Photo Management Backup** — Backs up Adobe Lightroom catalogs, Photoshop preferences
- **Development Configs Backup** — Backs up VS Code settings, Git config, SSH keys, environment variables

##### 3. Maintenance (8 Actions)

- **Check Disk Health** — Queries SMART data via PowerShell `Get-PhysicalDisk` and `Get-StorageReliabilityCounter` (temperature, wear, errors, power-on hours)
- **Update All Apps** — Updates packages via WinGet and Chocolatey with detailed reporting
- **Windows Update** — Triggers Windows Update via `UsoClient` (Update Session Orchestrator)
- **Verify System Files** — Runs `sfc /scannow` and `DISM /RestoreHealth` to repair corrupt system files
- **Check Disk Errors** — Runs `chkdsk` with bad sector scan and repair
- **Rebuild Icon Cache** — Deletes `IconCache.db` and restarts Explorer to fix broken icons
- **Reset Network** — Flushes DNS, resets TCP/IP stack, Winsock catalog, firewall, and network adapters
- **Clear Print Spooler** — Stops spooler service, deletes stuck print jobs, restarts service

##### 4. Troubleshooting (6 Actions)

- **Generate System Report** — Collects 100+ properties: OS details, hardware specs, storage info, drivers, event logs, installed programs (HTML output)
- **Check for Bloatware** — Scans for known bloatware and pre-installed OEM software
- **Test Network Speed** — Measures download/upload speeds and latency
- **Scan for Malware** — Runs Windows Defender quick scan via PowerShell `Start-MpScan`
- **Repair Windows Store** — Runs `WSReset.exe` and re-registers Store apps via PowerShell
- **Fix Audio Issues** — Restarts Windows Audio service and rebuilds audio device enumeration

##### 5. Emergency Recovery (8 Actions)

- **Backup Browser Data** — Emergency backup of all browser profiles (bookmarks, passwords, history) for all users
- **Backup Email Data** — Emergency backup of Outlook .PST files, Thunderbird profiles, Windows Mail
- **Create Restore Point** — Creates Windows System Restore checkpoint via WMI
- **Export Registry Keys** — Exports critical registry hives (HKLM\Software, HKCU, etc.)
- **Backup Activation Keys** — Extracts Windows and Office product keys from registry
- **Screenshot Settings** — Captures screenshots of Windows Settings panels for documentation
- **Backup Desktop Wallpaper** — Backs up current wallpaper and theme files
- **Backup Printer Settings** — Exports printer drivers, queues, and port configurations

#### Technical Details

- **Architecture:** Base class `QuickAction` with scan/execute lifecycle
- **Threading:** Actions run on worker threads via `QuickActionController`
- **Scanning:** Pre-execution scan estimates size, file count, duration, and displays warnings
- **Execution:** Real-time progress updates (0-100%), detailed logging, cancellation support
- **Results:** Structured output with success/failure status, bytes processed, files affected, duration, output path
- **Backup Location:** Configurable (default: `C:\SAK_Backups`)
- **Process Execution:** Uses `ProcessRunner` wrapper for PowerShell, CMD, and executable invocation

---

### User Profile Backup & Restore

**Comprehensive wizards** for backing up and restoring Windows user profiles with intelligent filtering, encryption, and permission preservation.

#### Backup Wizard (6 Pages)

##### Page 1: Welcome & Instructions
Introduction to the backup process with overview of capabilities.

##### Page 2: Scan & Select Users
- Scans all Windows user profiles via `WindowsUserScanner`
- Displays users with profile paths and estimated sizes
- Multi-select with checkbox UI
- Filters out system accounts (DefaultAppPool, SYSTEM, LOCAL SERVICE)

##### Page 3: Customize Per-User Data
- Per-user customization dialog for each selected profile
- Folder selection: Desktop, Documents, Pictures, Videos, Music, Downloads, AppData
- Browser data: Chrome, Edge, Firefox profiles (bookmarks, passwords, history)
- Email data: Outlook PST files, Thunderbird profiles
- Office documents, OneNote notebooks, game saves

##### Page 4: Smart Filter Configuration
- **Dangerous Files:** Excludes locked system files (`NTUSER.DAT`, `ntuser.dat.LOG`, `UsrClass.dat`)
- **Exclude Patterns:** Configurable regex patterns (e.g., `*.tmp`, `*.lock`, `~*`)
- **Exclude Folders:** Cache, Temp, Temporary Internet Files, Cookies, GPUCache
- **Size Limits:** Skip files larger than threshold (default: 4 GB)
- **Cache Detection:** Automatically identifies and excludes browser/app caches

##### Page 5: Backup Settings
- **Destination Path:** Browse for backup location
- **Compression:** Level 0-9 (0=none, 3=fast, 6=balanced, 9=maximum)
- **Encryption:** AES-256 password protection via Windows BCrypt
- **Permission Mode:** Preserve NTFS ACLs or apply default permissions
- **Manifest Generation:** Creates JSON manifest with metadata

##### Page 6: Execution & Progress
- Multi-threaded backup worker (`UserProfileBackupWorker`)
- Real-time progress: current file, bytes copied, files processed, estimated time remaining
- Detailed log viewer with timestamps
- Cancellation support with cleanup
- Final summary: success/failure, total bytes, file count, duration, backup path

#### Restore Wizard (6 Pages)

##### Page 1: Welcome
Instructions for restore process.

##### Page 2: Select Backup
- Browse for backup manifest file (`.json`)
- Displays backup metadata: creation date, source PC, users included, total size
- Validates backup integrity

##### Page 3: Map Users
- Drag-and-drop UI for mapping source users to destination users
- Auto-mapping based on username matches
- Create new users or map to existing accounts

##### Page 4: Merge Options
- **Collision Strategy:** Skip, Overwrite, Rename, Prompt
- **Preserve Permissions:** Restore original NTFS ACLs
- **Restore Hidden Files:** Include hidden/system files
- **Validate Checksums:** Verify file integrity during restore

##### Page 5: Review & Confirm
- Summary of restore plan
- Warnings for overwrite operations
- Estimated disk space required

##### Page 6: Execution & Progress
- Multi-threaded restore worker (`UserProfileRestoreWorker`)
- Real-time progress tracking
- Detailed logging
- Permission restoration via `PermissionManager`
- Final summary with restore report

#### Technical Implementation

- **User Scanning:** `WindowsUserScanner` queries registry `HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList`
- **Smart Filtering:** `SmartFileFilter` applies rules to exclude temp/cache/system files
- **Encryption:** AES-256-GCM via `encryption.cpp` (BCrypt API)
- **Compression:** zlib integration (gzip format)
- **Permissions:** `PermissionManager` preserves/restores NTFS ACLs using Windows API
- **Manifest Format:** JSON with user profiles, folder mappings, file lists, checksums, timestamps
- **Threading:** QThread-based workers with signal/slot communication for UI updates

---

### Application Migration

**Scan installed applications, match to Chocolatey packages, backup user data, install on target system, and restore data.**

#### Workflow

##### Step 1: Scan Installed Applications
- Queries Windows Registry:
  - `HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall`
  - `HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall` (32-bit on 64-bit)
  - `HKEY_CURRENT_USER\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall` (per-user)
- Extracts: DisplayName, DisplayVersion, Publisher, InstallLocation, UninstallString
- Filters out Windows components, KB updates, drivers

##### Step 2: Match to Chocolatey Packages
- Uses `PackageMatcher` with embedded `package_mappings.json` (700+ mappings)
- **Match Confidence Levels:**
  - **High:** Exact name match or known mapping (e.g., "Google Chrome" → `googlechrome`)
  - **Medium:** Publisher match or partial name match (e.g., "Adobe Acrobat" → `adobereader`)
  - **Low:** Fuzzy string match (Levenshtein distance)
  - **None/Manual:** No automatic match found

##### Step 3: Backup Application Data (Optional)
- Launches `BackupWizard` to back up user-specific app data
- Common locations: `%APPDATA%`, `%LOCALAPPDATA%`, `Documents`, user profile folders
- Examples: browser profiles, email data, game saves, IDE settings

##### Step 4: Generate Migration Report
- JSON format with application list, Chocolatey packages, match confidence, version locks
- Portable format for transfer to target machine

##### Step 5: Install Packages on Target
- Uses embedded Chocolatey (`ChocolateyManager`)
- **Retry Logic:** 3 attempts per package with exponential backoff
- **Version Locking:** Install specific versions if required (e.g., `googlechrome --version 120.0.6099.130`)
- **Concurrency:** Sequential installation to avoid conflicts
- **Detailed Logging:** Installation output, success/failure, error messages

##### Step 6: Restore Application Data
- Launches `RestoreWizard` to restore backed-up app data
- Maps source user paths to target user paths
- Preserves permissions and folder structures

#### UI Features

- **Table View:** Sortable, filterable table with columns:
  - Select (checkbox)
  - Application Name
  - Version
  - Publisher
  - Chocolatey Package
  - Match Confidence
  - Version Lock (checkbox)
  - Status (Pending/Installing/Installed/Failed)
  - Progress (0-100%)
- **Toolbar Actions:**
  - Scan Apps
  - Match Packages
  - Backup Data
  - Install
  - Restore Data
  - Export Report
  - Load Report
  - Refresh
- **Filters:**
  - Search by name, publisher, or package
  - Filter by confidence level (All/High/Medium/Low/None)
- **Selection Tools:**
  - Select All
  - Select None
  - Select Matched (only apps with Chocolatey packages)

#### Technical Implementation

- **App Scanner:** `AppScanner` queries registry via Windows API
- **Package Matcher:** `PackageMatcher` with fuzzy string matching algorithms
- **Chocolatey Manager:** `ChocolateyManager` wraps embedded `choco.exe` (portable distribution in `tools/chocolatey/`)
- **Migration Report:** JSON serialization via Qt JSON classes
- **Worker Thread:** `AppMigrationWorker` handles async package installation
- **Error Handling:** Retry logic, timeout handling, detailed error messages

---

### Network Transfer

**Secure LAN-based transfer of user profiles** with three modes: Source (Send), Destination (Receive), and Orchestrator (Multi-PC Deployment).

#### Mode 1: Source (Send)

##### Configuration
1. **Scan Users:** Select Windows user profiles to transfer
2. **Customize Data:** Per-user folder selection (Desktop, Documents, etc.)
3. **Discover Peers:** UDP broadcast discovery on port 54321
4. **Connection:** Manual IP entry or select from discovered destinations
5. **Security:**
   - Passphrase (required for encryption)
   - AES-256-GCM encryption toggle
   - Compression toggle (gzip)
   - Resume capability
6. **Bandwidth:** Limit transfer speed (KB/s)
7. **Permissions:** Preserve, Apply Default, or Strip permissions

##### Transfer Process
1. Build manifest (user list, folder mappings, file list, total size)
2. Establish TCP connection (port 54322 for control)
3. Send authentication challenge (PBKDF2-derived key from passphrase)
4. Send manifest for destination approval
5. Wait for approval/rejection
6. Open data connection (port 54323)
7. Transfer files in chunks (64 KB default, configurable)
8. AES-256-GCM encryption per chunk (if enabled)
9. Resume support (partial file ranges persisted)
10. Integrity verification (SHA-256 per file)
11. Generate transfer report (JSON)

#### Mode 2: Destination (Receive)

##### Configuration
1. **Destination Base Path:** Root folder for incoming profiles (e.g., `D:\UserMigration`)
2. **Passphrase:** Must match source passphrase for decryption
3. **Auto-Approve:** Automatically approve transfers (skip manual review)
4. **Apply Restore:** Automatically restore permissions after transfer

##### Receive Process
1. Start listening on port 54322 (TCP)
2. Receive manifest from source
3. Display manifest for review (users, size, file count)
4. Approve or reject transfer
5. Receive encrypted chunks
6. Decrypt chunks (AES-256-GCM)
7. Write to disk with progress tracking
8. Verify checksums
9. Restore permissions (if enabled)
10. Generate receive report

##### Orchestrator Integration
- Connect to orchestrator server (configurable host/port)
- Register as available destination
- Receive deployment assignments
- Display active assignment details
- Queue multiple assignments
- Report progress to orchestrator

#### Mode 3: Orchestrator (Deploy)

**Centralized deployment manager** for multi-PC migrations.

##### Server Setup
1. Start orchestrator server (default port 54322)
2. Server listens for destination registrations
3. Discovery service broadcasts on port 54321

##### Source Configuration
1. Scan users on source PC
2. Select users for deployment
3. Configure global settings (bandwidth, concurrency)

##### Destination Registration
- Destinations connect to orchestrator
- Registration includes: hostname, IP, available space, capabilities
- Health checks every 10 seconds

##### Mapping Configuration

###### Mapping Types
- **One-to-Many:** Single source user → multiple destinations
- **Many-to-Many:** Multiple source users → multiple destinations (auto-assigned)
- **Custom Rules:** Drag-and-drop user-to-destination assignments

###### Mapping Strategies
- **Round Robin:** Distribute users evenly across destinations
- **Least Busy:** Assign to destination with fewest active jobs
- **Most Space:** Assign to destination with most free disk space
- **Custom Priority:** Manual priority order

##### Deployment Execution

###### Concurrency Control
- **Max Concurrent Jobs:** Global limit (e.g., 5 simultaneous transfers)
- **Global Bandwidth Limit:** Total bandwidth cap (MB/s)
- **Per-Job Bandwidth Limit:** Individual job cap (MB/s)

###### Job Management
- **Pause/Resume:** Individual jobs or entire deployment
- **Retry:** Failed jobs with exponential backoff
- **Cancel:** Abort jobs with cleanup
- **Priority:** Adjust job priority in queue

###### Progress Tracking
- **Job Table:** Columns for Source User, Destination, Status, Progress (0-100%), Speed, ETA
- **Aggregate Progress:** Overall completion percentage
- **Destination Table:** Real-time status of each registered destination
- **History Table:** Completed jobs with timestamps, durations, success/failure

##### Deployment Templates
- **Save Template:** Export deployment configuration (mapping rules, settings)
- **Load Template:** Import saved configuration for repeated deployments

##### Deployment History
- **Persistent Storage:** SQLite database (`deployment_history.db`)
- **Recovery:** Resume interrupted deployments from last known state
- **Exports:**
  - CSV: Summary of all deployments
  - PDF: Detailed deployment report with charts

#### Security Architecture

##### Encryption
- **Algorithm:** AES-256-GCM (Galois/Counter Mode)
- **Key Derivation:** PBKDF2 with SHA-256 (100,000 iterations, 32-byte salt)
- **Implementation:** Windows BCrypt API (FIPS 140-2 compliant)
- **Per-Chunk Encryption:** Each 64 KB chunk encrypted independently
- **Authentication:** 16-byte GCM tag per chunk for integrity verification

##### Authentication
- **Challenge/Response:** Destination sends random 32-byte nonce, source responds with HMAC-SHA256(nonce, derived_key)
- **Pre-Shared Passphrase:** User-provided password (minimum 8 characters recommended)

##### Network Protocol
- **Discovery:** UDP broadcast (unencrypted, hostname/IP/capabilities only)
- **Control Channel:** TCP port 54322 (encrypted manifest, commands)
- **Data Channel:** TCP port 54323 (encrypted file chunks)

#### Technical Implementation

- **Controllers:**
  - `NetworkTransferController` (Source/Destination logic)
  - `MigrationOrchestrator` (Orchestrator server)
  - `ParallelTransferManager` (Concurrent job scheduling)
- **Discovery:** `OrchestrationDiscoveryService` (UDP broadcast/listen)
- **Server:** `OrchestrationServer` (TCP server for orchestrator)
- **Client:** `OrchestrationClient` (TCP client for destinations)
- **Security:** `TransferSecurityManager` (AES-256-GCM encryption)
- **Resume:** `AssignmentQueueStore` (SQLite persistence)
- **Workers:** `NetworkTransferWorker` (threaded file transfer)

#### Resume Capability

- **Partial Files:** Store resume ranges in manifest
- **Checkpoint:** Save progress every 1 MB
- **Recovery:** Detect incomplete transfers, resume from last checkpoint
- **Validation:** Verify partial file integrity before resume

#### Reporting

- **Source Report:** Saved to `%USERPROFILE%\Documents\SAK\TransferReports\transfer_YYYYMMDD_HHMMSS.json`
- **Destination Report:** Saved to `<DestinationBase>\TransferReports\receive_YYYYMMDD_HHMMSS.json`
- **Orchestrator Summary:** Deployment history with job details, success rates, total bytes transferred

---

### Directory Organizer

**Organize files by extension** into categorized subdirectories with preview mode and collision handling.

#### Features

- **Category Mapping:** Define categories with extensions (e.g., Images: `jpg,png,gif,svg`)
- **Default Categories:** Documents, Images, Videos, Audio, Archives, Code, Spreadsheets, Executables
- **Custom Categories:** Add/remove categories via table UI
- **Collision Strategies:**
  - **Rename:** Append number suffix (e.g., `file (1).txt`)
  - **Skip:** Leave original file in place
  - **Overwrite:** Replace existing file (with confirmation)
- **Preview Mode:** Show what would happen without moving files
- **Recursive Scanning:** Organize files in subdirectories
- **Progress Tracking:** Real-time file count and current file path

#### Workflow

1. **Select Target Directory:** Browse for folder to organize
2. **Configure Categories:** Edit extension mappings in table
3. **Choose Collision Strategy:** Select how to handle duplicates
4. **Preview:** Click Preview to see proposed changes (no files moved)
5. **Execute:** Click Execute to perform organization
6. **Log Review:** View detailed log of operations

#### Technical Implementation

- **Worker Thread:** `OrganizerWorker` runs in background
- **File Classifier:** Extension-based categorization with case-insensitive matching
- **Collision Detection:** Pre-scan target directories for conflicts
- **Undo Support:** Integration with `UndoManager` for rollback (via undo stack)

---

### Duplicate Finder

**Hash-based duplicate detection** with size filtering and recursive scanning.

#### Features

- **Scanning:**
  - Add multiple directories to scan
  - Recursive subdirectory scanning
  - Minimum file size filter (KB)
- **Detection:**
  - SHA-256 hash comparison (via `FileHash` utility)
  - Fast pre-filter by file size (only hash files with same size)
  - Skip system files and locked files
- **Results:**
  - Summary: duplicate count, wasted space (bytes)
  - Detailed log: duplicate sets with file paths and sizes
- **Actions:**
  - Delete duplicates (keep oldest or newest)
  - Move to folder
  - Create hardlinks (Windows NTFS only)

#### Workflow

1. **Add Directories:** Click Add Directory, browse for folders
2. **Set Options:**
   - Minimum size (skip small files)
   - Recursive scan toggle
3. **Scan:** Click Scan to start detection
4. **Review Results:** View summary and detailed duplicate sets
5. **Take Action:** Delete, move, or link duplicates (future enhancement)

#### Technical Implementation

- **Worker Thread:** `DuplicateFinderWorker` (non-blocking UI)
- **Hashing:** `FileHash::sha256()` using Windows CryptoAPI
- **Performance:** Size-based grouping before hashing (reduces hash operations)

---

### Image Flasher

**Create bootable USB drives** from disk images with Windows 11 ISO download support.

#### Supported Image Formats

- **ISO:** CD/DVD image (most common)
- **IMG:** Raw disk image
- **WIC:** Windows Imaging Component format
- **ZIP, GZ, BZ2, XZ:** Compressed images (auto-decompression)
- **DMG:** Apple disk image (read-only)
- **DSK:** Generic disk image

#### Features

##### Image Selection
- **Browse for Image:** Select local image file
- **Download Windows 11:** Direct download from Microsoft servers via `WindowsISODownloader`
  - Multiple languages supported
  - x64 and ARM64 architectures
  - ~5.5 GB download with progress tracking
  - Automatic integrity verification

##### Drive Selection
- **Auto-Detection:** Scans for removable USB drives and SD cards via `DriveScanner`
- **Safety Checks:** System drive protection (C: drive cannot be selected)
- **Multi-Select:** Flash to multiple drives simultaneously (parallel writing)
- **Drive Info:** Shows capacity, file system, label, partition count

##### Flash Process (4-Page Wizard)

###### Page 1: Image Selection
- Select image file or download Windows 11
- Display image info: size, format, filename

###### Page 2: Drive Selection
- Table of available drives
- Multi-select checkboxes
- Refresh button to rescan drives
- Safety warnings for data loss

###### Page 3: Flash Progress
- Real-time progress: writing, verifying
- Speed (MB/s), ETA, bytes written
- Per-drive progress bars (if multi-drive)

###### Page 4: Completion
- Success/failure summary
- Drive letters of completed flashes
- Option to eject drives safely

#### Windows USB Creator Workflow

For Windows ISO files, uses specialized `WindowsUSBCreator`:

##### Step 1: Format Drive as NTFS
- Uses PowerShell `Format-Volume` cmdlet
- NTFS required for files >4 GB (e.g., `install.wim`)

##### Step 2: Extract ISO Contents
- Mounts ISO via PowerShell `Mount-DiskImage`
- Copies all files to USB drive
- Unmounts ISO after extraction

##### Step 3: Make Bootable
- Applies boot sector via `bootsect.exe /nt60 <drive>: /force /mbr`
- Creates UEFI boot structure

##### Step 4: Verify
- Checks for critical files: `setup.exe`, `sources\boot.wim`, `bootmgr`, `sources\install.wim` or `install.esd`
- Validates NTFS file system

##### Step 5: Eject Safely
- Flushes buffers
- Dismounts volume
- Safe to remove drive

#### Generic Image Writer

For non-Windows images (Linux, etc.):

1. **Unmount Drive:** `DriveUnmounter` safely unmounts volume
2. **Lock Drive:** `DriveLock` obtains exclusive access
3. **Write Sectors:** `ImageWriter` writes raw bytes to disk
4. **Decompress on-the-fly:** `StreamingDecompressor` handles compressed images (gzip, bzip2, xz)
5. **Verify:** SHA-512 checksum validation (optional)

#### Technical Implementation

- **Components:**
  - `DriveScanner`: Detects removable drives via Windows API
  - `FlashCoordinator`: Orchestrates flash workflow
  - `WindowsISODownloader`: Downloads ISOs from Microsoft Media Creation Tool API
  - `WindowsUSBCreator`: Creates bootable Windows USB drives
  - `ImageWriter`: Raw disk I/O via Windows API
  - `StreamingDecompressor`: On-the-fly decompression
  - `DriveUnmounter`: Safe volume unmounting
  - `DriveLock`: Exclusive drive access
- **Decompression:**
  - `GzipDecompressor`: GZIP format (zlib)
  - `Bzip2Decompressor`: BZIP2 format
  - `XzDecompressor`: XZ/LZMA format
  - `DecompressorFactory`: Auto-detects format from header
- **Threading:** Background workers for download, extraction, writing, verification

---

## Security & Encryption

### Windows BCrypt API

All cryptographic operations use **Windows BCrypt API** for FIPS 140-2 compliance.

#### AES-256 Encryption

- **Algorithm:** AES-256-CBC (Cipher Block Chaining) for file encryption
- **Algorithm:** AES-256-GCM (Galois/Counter Mode) for network transfer
- **Key Derivation:** PBKDF2 with SHA-256
  - 100,000 iterations (configurable)
  - 32-byte salt (randomly generated per operation)
  - 32-byte key output (256 bits)
- **Implementation:** `encryption.cpp` via BCrypt API
- **Functions:**
  - `encrypt_data()`: Encrypt QByteArray with password
  - `decrypt_data()`: Decrypt QByteArray with password
  - `encrypt_file()`: In-place file encryption
  - `decrypt_file()`: In-place file decryption

#### Network Transfer Security

- **Chunk Encryption:** Each 64 KB chunk encrypted with AES-256-GCM
- **Authentication:** 16-byte GCM tag per chunk for integrity
- **Nonce:** 12-byte random IV per chunk (never reused)
- **Additional Authenticated Data (AAD):** Chunk sequence number to prevent replay attacks
- **Key Exchange:** Pre-shared passphrase (no key exchange protocol, assumes secure band communication)

#### Secure Memory

- **Implementation:** `secure_memory.cpp`
- **Features:**
  - `SecureString`: Overwrite memory on destruction (prevent memory dumps)
  - `SecureBuffer`: Zero-fill on deallocation
  - Page locking (prevent swapping to disk)

### Input Validation

- **Implementation:** `input_validator.cpp`
- **Validations:**
  - Path sanitization (prevent directory traversal)
  - Filename validation (Windows-safe characters)
  - IP address validation
  - Port number validation
  - Package name validation (Chocolatey format)
  - Version string validation (SemVer)

### Elevation Management

- **Implementation:** `elevation_manager.cpp`
- **UAC Handling:** Application manifest requests `requireAdministrator`
- **Runtime Checks:** Verify administrator privileges before disk operations

---

## Building from Source

### Prerequisites

1. **Install Visual Studio 2022** (Community, Professional, or Enterprise)
   - Workload: Desktop development with C++
   - Components: MSVC v143, Windows 10/11 SDK, CMake tools

2. **Install Qt 6.5.3**
   ```powershell
   # Download Qt Online Installer from https://www.qt.io/download-qt-installer
   # Install Qt 6.5.3 with MSVC 2019 64-bit component
   ```

3. **Install CMake 3.28+**
   ```powershell
   winget install Kitware.CMake
   ```

4. **Install vcpkg** (optional, for dependencies)
   ```powershell
   git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
   cd C:\vcpkg
   .\bootstrap-vcpkg.bat
   .\vcpkg integrate install
   ```

### Build Steps

#### Option 1: Visual Studio GUI

1. Open `CMakeLists.txt` in Visual Studio 2022
2. Configure CMake (automatic)
3. Build → Build Solution (F7)
4. Output: `build\Release\sak_utility.exe`

#### Option 2: Command Line

```powershell
# Clone repository
git clone https://github.com/RandyNorthrup/S.A.K.-Utility.git
cd S.A.K.-Utility

# Configure (generates Visual Studio solution)
cmake -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.5.3/msvc2019_64"

# Build Release
cmake --build build --config Release --parallel

# Build Debug
cmake --build build --config Debug --parallel

# Run
.\build\Release\sak_utility.exe
```

### Build Configuration

Persistent settings stored in `cmake/SAK_BuildConfig.cmake`:

```cmake
# vcpkg integration
set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake")
set(VCPKG_TARGET_TRIPLET "x64-windows-static")

# Qt path
set(CMAKE_PREFIX_PATH "C:/Qt/6.5.3/msvc2019_64")

# Static runtime (no DLL dependencies)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
```

### Compiler Flags

- **C++ Standard:** C++23 (`/std:c++latest`)
- **Warning Level:** W4 (all warnings)
- **Warnings as Errors:** `/WX`
- **Conformance:** `/permissive-` (strict standards compliance)
- **Security:** `/sdl` (Security Development Lifecycle checks), `/guard:cf` (Control Flow Guard)
- **Optimizations (Release):** `/O2` (maximize speed), `/GL` (whole program optimization), `/LTCG` (link-time code generation)

### Dependencies

Managed via vcpkg and Qt:

- **Qt 6.5.3:** Core, Widgets, Concurrent, Network
- **zlib:** Compression (gzip)
- **bzip2:** Compression (bzip2)
- **liblzma:** Compression (xz/LZMA)
- **Windows SDK:** BCrypt, WinHTTP, Shell API

### Embedded Tools

Pre-bundled in `tools/` directory:

- **Chocolatey:** Portable distribution (copied to build output)

### Testing

```powershell
# Enable tests in CMake
cmake -B build -DBUILD_TESTING=ON

# Build tests
cmake --build build --config Release --target RUN_TESTS

# Run specific test suite
.\build\tests\Release\test_encryption.exe
.\build\tests\Release\test_network_transfer.exe
```

Test suites:
- Unit tests: `tests/unit/` (59 files)
- Integration tests: `tests/integration/` (15 files)

### Verification Script

```powershell
.\scripts\verify_build.ps1

# Options:
# -FullClean    : Clean build directory before build
# -SkipBuild    : Only check files, don't build
```

Checks:
- Executable presence
- DLL dependencies (Qt6Core.dll, Qt6Widgets.dll, etc.)
- Resource files (splash screen, icon)
- Embedded tools (Chocolatey)

---

## Configuration

### Portable Mode

Create `portable.ini` (empty file) next to `sak_utility.exe`:

```ini
# Empty file enables portable mode
# Settings stored in sak_utility.ini (same directory)
# Logs stored in _logs\ subdirectory
```

### Normal Mode (No portable.ini)

- Settings: `%APPDATA%\SAK_Utility\config.ini`
- Logs: `%APPDATA%\SAK_Utility\_logs\`

### Configuration File Format

`config.ini` or `sak_utility.ini`:

```ini
[QuickActions]
backup_location=C:/SAK_Backups
confirm_before_execute=true
show_notifications=true
enable_logging=true
compress_backups=true

[NetworkTransfer]
discovery_port=54321
control_port=54322
data_port=54323
encryption_enabled=true
compression_enabled=true
chunk_size=65536

[Application]
theme=windows11
window_geometry=@ByteArray(...)
window_state=@ByteArray(...)
```

### Environment Variables

Optional environment overrides:

- `SAK_CHOCO_PATH`: Custom Chocolatey installation path
- `SAK_LOG_LEVEL`: Logging level (DEBUG, INFO, WARNING, ERROR)
- `SAK_CONFIG_PATH`: Custom configuration file path

---

## Contributing

We welcome contributions! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

### Development Workflow

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Make changes following coding standards
4. Write/update tests
5. Commit with conventional commit messages (`feat:`, `fix:`, `docs:`, etc.)
6. Push to your fork
7. Open a Pull Request

### Coding Standards

- **C++ Style:** Follow Google C++ Style Guide with Qt naming conventions
- **Naming:**
  - Classes: `PascalCase` (e.g., `NetworkTransferController`)
  - Methods: `camelCase` (e.g., `executeAction()`)
  - Private members: `m_` prefix (e.g., `m_controller`)
  - Signals: `camelCase` (e.g., `statusChanged`)
  - Slots: `on` prefix (e.g., `onButtonClicked()`)
- **Documentation:** Doxygen-style comments for public APIs
- **Error Handling:** Use `std::expected` (C++23) for error propagation
- **Threading:** Use Qt threading primitives (QThread, QtConcurrent)
- **Memory Management:** Prefer smart pointers (`std::unique_ptr`, `std::shared_ptr`)

### Testing Requirements

- Unit tests for core logic
- Integration tests for workflows
- Manual testing on Windows 10 and Windows 11
- Administrator permission testing

### Commit Message Format

```
<type>(<scope>): <subject>

<body>

<footer>
```

Types: `feat`, `fix`, `docs`, `style`, `refactor`, `test`, `chore`

Example:
```
feat(network): Add pause/resume support for orchestrator deployments

Implement pause and resume functionality for individual jobs and entire
deployments in the orchestrator mode. Jobs can now be paused mid-transfer
and resumed without data loss.

Closes #123
```

---

## License

This project is licensed under the **GNU General Public License v2.0** - see the [LICENSE](LICENSE) file for details.

### Summary

- ✅ Commercial use allowed
- ✅ Modification allowed
- ✅ Distribution allowed
- ✅ Private use allowed
- ⚠️ Must disclose source code
- ⚠️ Must include original license
- ⚠️ Same license for derivatives
- ❌ No warranty provided

---

## Author

**Randy Northrup**

- GitHub: [@RandyNorthrup](https://github.com/RandyNorthrup)
- Project: [S.A.K.-Utility](https://github.com/RandyNorthrup/S.A.K.-Utility)

---

## Acknowledgments

- **Qt Framework:** Cross-platform UI framework (https://www.qt.io/)
- **Chocolatey:** Windows package manager (https://chocolatey.org/)
- **Microsoft:** Windows BCrypt API, PowerShell, Windows SDK
- **Open Source Community:** vcpkg, CMake, zlib, bzip2, liblzma

---

## Roadmap

### Planned Features (v0.6.0+)

- [ ] PXE boot server for network imaging
- [ ] WinPE bootable environment creation
- [ ] Cloud backup integration (Azure, AWS S3, Google Cloud)
- [ ] Active Directory user migration
- [ ] Group Policy backup/restore
- [ ] BitLocker recovery key export
- [ ] Driver backup/restore
- [ ] Registry comparison and merge
- [ ] Automated testing framework
- [ ] Multi-language support (i18n)

### Known Issues

- See [GitHub Issues](https://github.com/RandyNorthrup/S.A.K.-Utility/issues)

---

## Support

- **Documentation:** See `docs/` folder
- **Issues:** [GitHub Issues](https://github.com/RandyNorthrup/S.A.K.-Utility/issues)
- **Discussions:** [GitHub Discussions](https://github.com/RandyNorthrup/S.A.K.-Utility/discussions)

---

## Changelog

See [VERSION_MANAGEMENT.md](VERSION_MANAGEMENT.md) for detailed version history.

### Version 0.5.6 (Current)

- ✨ 39 Quick Actions across 5 categories
- ✨ User Profile Backup & Restore wizards (6 pages each)
- ✨ Application Migration with embedded Chocolatey
- ✨ Network Transfer with Orchestrator mode
- ✨ Image Flasher with Windows 11 ISO download
- ✨ Windows 11 themed UI with splash screen
- ✨ AES-256 encryption via BCrypt API
- ✨ Smart file filtering
- ✨ Comprehensive logging system
- 🐛 Fixed UI overlapping issues
- 🐛 Fixed scrollbar styling
- 🐛 Fixed CMake build configuration

---

**Last Updated:** February 2, 2026

**Project Status:** Active Development

**Build Status:** ![Passing](https://img.shields.io/badge/build-passing-brightgreen)

**Code Quality:** ![A+](https://img.shields.io/badge/code%20quality-A%2B-brightgreen)

**Documentation:** ![Comprehensive](https://img.shields.io/badge/documentation-comprehensive-blue)

---

*This README provides a complete overview of S.A.K. Utility. For detailed technical documentation, see individual files in the `docs/` directory.*

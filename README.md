# S.A.K. Utility

**Swiss Army Knife Utility** - Windows PC technician's toolkit for system migration, user profile backups, and file management.

[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://isocpp.org/)
[![Qt 6.5.3](https://img.shields.io/badge/Qt-6.5.3-green.svg)](https://www.qt.io/)
[![Windows](https://img.shields.io/badge/Platform-Windows%2010%2F11-blue.svg)](https://www.microsoft.com/windows)

---

## Overview

S.A.K. Utility is a **Windows-only** desktop application designed for **PC technicians** who need to migrate systems, backup user profiles, and manage files during PC repairs, upgrades, and replacements. Built with modern C++23 and Qt6, it provides professional-grade tools through an intuitive tabbed interface.

**Target Users**: PC technicians, computer repair shops, IT support specialists  
**Version**: 0.5.5  
**Release Date**: December 11, 2025  
**Platform**: Windows 10/11 x64 only  
**Package Size**: ~21.5 MB (includes Qt runtime, vcpkg libraries, embedded Chocolatey)

---

## Core Features

### 1. Application Migration
Automate software installation on new or refreshed Windows systems using embedded Chocolatey.

**Capabilities:**
- **Application Scanning**: Windows Registry + AppX packages via PowerShell `Get-AppxPackage`
  - Registry sources: `HKLM\Software\...\Uninstall` (32-bit and 64-bit), `HKCU\Software\...\Uninstall`
  - Detects: App name, version, publisher, install location
- **Intelligent Package Matching**: 3-tier matching system
  - **Exact Match**: 42 pre-defined common app mappings (e.g., "7-Zip" → `7zip`, "Google Chrome" → `googlechrome`)
  - **Fuzzy Match**: Levenshtein distance + Jaro-Winkler similarity algorithms
  - **Chocolatey Search API**: Real-time package availability verification
- **Confidence Scoring**: High/Medium/Low/Manual/Unknown ratings for match quality
- **Version Locking**: Pin specific app versions for reproducible deployments
### 2. User Profile Backup & Restore
Comprehensive Windows user profile backup with intelligent **6-page wizards** and enterprise-grade encryption.

#### **User Profile Backup Wizard - 6 Pages**

**Page 1: Welcome & Instructions**
- Introduction to backup process, safety warnings, documentation links

**Page 2: Scan & Select Users**
- **Windows User Detection**: Uses `NetUserEnum` API to enumerate all local users
- **Displayed Info**: Username, SID (Security Identifier), Profile Path (`C:\Users\Username`), Estimated Size, Current User indicator
- **Actions**: Scan Users, Select All, Select None, real-time progress bar
- **Summary**: X users selected, Y GB total

**Page 3: Customize Per-User Data**
- **Per-User Customization Dialog**: `per_user_customization_dialog.cpp/h`
- **11 Folder Types Supported**:
  - Documents, Desktop, Pictures, Videos, Music, Downloads
  - AppData\Roaming, AppData\Local
  - Favorites, Start Menu, Custom paths
- **Folder Options**: Enable/disable each folder, include patterns (`["*"]` or specific files), exclude patterns, calculated size and file count per folder

**Page 4: Smart Filter Configuration**
- **Size Limits** (Optional): Max single file size (2 GB default), Max folder size (50 GB warning threshold)
- **Dangerous Files Excluded**: `NTUSER.DAT`, `UsrClass.dat`, `NTUSER.DAT.LOG*`, `*.lock`, `hiberfil.sys`, `pagefile.sys`
- **Excluded Folders**: `AppData\Local\Temp`, browser caches (`INetCache`, `Temporary Internet Files`)
- **Pattern Exclusions**: `*.tmp`, `*.temp`, `*.cache`, `thumbs.db`, `desktop.ini`
- **Actions**: Reset to Defaults, View Dangerous Files List
- **Implementation**: `smart_file_filter.cpp` - OWASP-compliant path validation

**Page 5: Backup Settings**
- **Compression**: 4 levels (None, Fast, Balanced, Maximum) using PowerShell `Compress-Archive` → ZIP format
- **Encryption**: AES-256-CBC via Windows BCrypt API (FIPS 140-2 compliant)
  - **Key Derivation**: PBKDF2 with 100,000 iterations
  - **Salt**: 32 bytes (random per backup)
  - **IV**: 16 bytes (random per backup)
  - **Format**: `[Salt(32)][IV(16)][EncryptedData]`
  - Password confirmation with strength validation
- **Verification**: SHA256 checksum for integrity (stored in manifest file)
- **Destination**: Browse for backup location, automatic manifest generation

**Page 6: Execution & Progress**
- **Progress Tracking**: Overall progress bar (%), current file progress, current operation label, speed (MB/s), elapsed time
### 3. Directory Organizer
Automatically organize messy directories by file extension with intelligent categorization.

**Features:**
- **Extension-Based Sorting**: Moves files into category folders
- **6 Default Categories**:
  - **Images**: `.jpg`, `.jpeg`, `.png`, `.gif`, `.bmp`, `.svg`
  - **Documents**: `.pdf`, `.doc`, `.docx`, `.txt`, `.odt`
  - **Videos**: `.mp4`, `.avi`, `.mkv`, `.mov`, `.wmv`
  - **Audio**: `.mp3`, `.wav`, `.flac`, `.m4a`, `.ogg`
  - **Archives**: `.zip`, `.rar`, `.7z`, `.tar`, `.gz`
  - **Executables**: `.exe`, `.msi`, `.bat`, `.sh`
- **Customization**: Add/remove categories, custom extension mappings
- **Collision Handling**: Rename (add suffix), Skip (leave existing), Overwrite (replace)
- **Recursive Processing**: Scan subdirectories option
- **Preview Mode**: Dry run before committing changes
- **Detailed Logging**: Action-by-action report

**Technical Components:**
- `organizer_panel.cpp/h` - GUI panel
- `organizer_worker.cpp/h` - Background thread for file operationsource machine, backup date, users in backup, total size, encryption status

**Page 2: User Mapping**
- **Auto-Detect Destination Users**: Scans local users via `NetUserEnum` API
- **3 Merge Modes**:
  - **ReplaceDestination**: Overwrite destination user's files
  - **MergeIntoDestination**: Combine files with conflict resolution
  - **CreateNewUser**: Create new user profile
- **Mapping Table**: Source Username → Destination Username (dropdown), Source/Dest SID, Merge Mode dropdown
- **Actions**: Auto-Map (matches by username), Manual mapping per row

**Page 3: Merge Configuration**
### 6. Image Flasher
Create bootable USB drives with **4-page wizard**, supporting multiple image formats and streaming decompression.

#### **4-Page Wizard Interface**

**Page 1: Select Image**
- **Image Sources**:
  - Local file: ISO, IMG, WIC, ZIP, GZIP (.gz), BZIP2 (.bz2), XZ (.xz), DMG (read-only), DSK
  - Download Windows ISO: Opens `windows_iso_download_dialog.cpp`
- **Image Info Display**: Path, Size, Format, Compression type (if applicable)

**Page 2: Select Target Drives**
- **Drive List**: Name, Size, Bus Type (USB, SATA, NVMe, SD), Mount Points
- **Multi-Select**: Flash to multiple drives simultaneously (parallel writing)
- **System Drive Protection**: System/boot drives grayed out, cannot select
- **Safety Options**: "Show All Drives" checkbox (includes non-removable when enabled)
- **Summary**: "X drives selected"

**Page 3: Flash Progress**
- **Overall Progress Bar**: 0-100%
- **State Label**: "Validating → Unmounting → Decompressing → Flashing → Verifying"
- **Details**: "Writing to \\.\PhysicalDrive1 (SanDisk Ultra)"
- **Speed**: Current write speed (MB/s)
- **Per-Drive Status**: Active drives count, failed drives count, completed drives count
- **Cancel Button**: Graceful abort with cleanup

**Page 4: Completion**
- **Success Message**: "Successfully flashed X drives"
- **Failed Drives List**: Error details for each failure
- **Verification Results**: SHA-512 checksums (if verification enabled)
- **Actions**: "Flash Another" (restart wizard), "Done" (close panel)

#### **Supported Image Formats**

| Format | Extension | Description | Decompression |
|--------|-----------|-------------|---------------|
| ISO | `.iso` | ISO 9660 CD/DVD image | None (direct write) |
| IMG | `.img` | Raw disk image | None (direct write) |
| WIC | `.wic` | Windows Imaging Format | None (direct write) |
| ZIP | `.zip` | ZIP archive containing image | Built-in Qt |
| GZIP | `.gz` | GZIP compressed | zlib (streaming) |
| BZIP2 | `.bz2` | BZIP2 compressed | libbz2 (streaming) |
| XZ | `.xz` | XZ/LZMA compressed | liblzma (streaming) |
| DMG | `.dmg` | Apple Disk Image | Read-only support |
| DSK | `.dsk` | Generic disk image | None (direct write) |

#### **Windows ISO Downloader**
### 4. Duplicate File Finder
Find and manage duplicate files using cryptographic hashing.

**Capabilities:**
- **MD5-Based Detection**: True byte-level duplicate detection (not just filename matching)
- **Streaming Hashing**: Memory-efficient for large files (1 MB chunks via `file_hash.cpp`)
- **Multi-Threaded Scanning**: Parallel hashing for large directories using QtConcurrent
- **Minimum Size Filter**: Skip tiny files (configurable threshold in KB)
- **Recursive Scanning**: Option to scan subdirectories
- **3 Actions**: Delete duplicates (keeps one copy), Move to folder, Report only (export list)
### 5. License Key Scanner
Attempt to locate software license keys stored in Windows Registry (best-effort tool).

**Features:**
- **Registry Scanning**: Searches `HKLM` and `HKCU` with pattern matching (keywords: "key", "serial", "license", "product", "activation")
- **Filesystem Scanning**: Detects license files (`.lic`, `.key`, `.license`, `.reg`)
- **Multi-Threaded**: Parallel registry traversal for performance
- **UAC Elevation**: Automatic elevation prompt when registry access requires admin
- **Export**: Save findings to CSV/TXT file
- **Additional Paths**: Add custom directories to scan

**Scan Options:**
- Scan Registry (HKLM, HKCU)
- Scan Filesystem (Program Files, ProgramData, etc.)
- Scan system licenses (Windows activation, Office)
- Custom paths (line edit)

**Limitations:**
- **Only finds registry-stored keys** - Many modern apps use:
  - Cloud-based licensing (keys stored on vendor servers)
  - Hardware tokens or dongles
  - Encrypted credential storage (Windows Credential Manager, KeyChain)
- **Cannot decrypt protected keys** - Encrypted keys cannot be recovered
- **False Positives**: May find unrelated registry values
- **Best-effort basis only** - No guarantees

**Technical Components:**
- `license_scanner_panel.cpp/h` - GUI panel with result table
- `license_scanner_worker.cpp/h` - Background registry/filesystem scanning thread
- `elevation_manager.cpp/h` - UAC integration for admin accesses, `QueryDosDevice` API, `GetDriveType`, `DeviceIoControl` for geometry
- **Drive Information Collected**:
  - Device path (`\\.\PhysicalDrive1`)
  - Name, Size (bytes), Block Size (512 or 4096)
  - System drive flag, Removable flag, Read-only flag
  - Bus type (USB, SATA, NVMe, SD)
  - Mount points (e.g., `["E:\", "F:\"]`), Volume label
- **Continuous Monitoring**: `DriveScanner` runs in background thread, emits `drivesUpdated` signal on drive insertion/removal

**Safety Features**:
- **System Drive Protection**: Prevents selecting boot drive (grayed out in UI)
- **Write-Protection Detection**: Warns if drive is read-only
- **Size Validation**: Ensures target drive is large enough for image
- **Confirmation Dialog**: Shows all drives that will be erased, requires explicit confirmation

#### **Flash Operation Workflow**

**Flash Coordinator** (`flash_coordinator.cpp` - 249 lines):

**5 Flash States**:
1. **Validating**: Check image file exists, validate drives, verify size compatibility
2. **Unmounting**: Unmount all volumes on target drives (`drive_unmounter.cpp`)
3. **Decompressing**: Stream decompress (if needed) - **NO TEMPORARY FILES** (key feature)
4. **Flashing**: Parallel writes to multiple drives
   - **Buffer**: 64 MB per buffer
   - **Pipeline**: 16 buffers for read-ahead (1 GB total pipeline)
   - **Parallelism**: Writes to all selected drives simultaneously
5. **Verifying**: SHA-512 checksum per drive (optional, can be disabled in settings)

**Progress Tracking**:
- State enum, Percentage (0-100), Bytes written (total across all drives)
- Total bytes, Speed (MB/s), Active/failed/completed drive counts

**Streaming Decompression** (NO TEMP FILES):
- **Key Feature**: Decompresses on-the-fly without creating temporary files
- **Libraries**: zlib (gzip), libbz2 (bzip2), liblzma (xz)
- **Buffer Management**: 64 MB chunks, 16-buffer pipeline for continuous streaming
- **Memory-Efficient**: Only buffers in RAM, never writes full decompressed file to disk
- **Implementation**: `streaming_decompressor.cpp/h` (base), `gzip_decompressor.cpp`, `bzip2_decompressor.cpp`, `xz_decompressor.cpp`

**Use Case:** Create bootable Windows installation USB drives for PC repairs and fresh installations, flash Raspberry Pi images, create Linux live USBs.

## Recent Changes (v0.5.5)

### Documentation Overhaul
- ✅ **Comprehensive README rewrite** with accurate feature descriptions from codebase scan
- ✅ **User Profile Wizards**: Documented all 6 pages (Backup) + 6 pages (Restore) with detailed workflows
- ✅ **Image Flasher**: Added 4-page wizard breakdown, 9 supported formats, streaming decompression details
- ✅ **Application Migration**: Documented 42 pre-defined mappings, fuzzy matching algorithms (Levenshtein + Jaro-Winkler)
- ✅ **Technical Architecture**: Added backend systems (logging, encryption, threading, input validation)
- ✅ **Corrected Inaccuracies**: SHA-512 verification (was incorrectly stated as SHA-256), 5 main tabs (was stated as 4)

### Build & Packaging Improvements (from v0.5.4)
- ✅ Added all Chocolatey binaries to repository (22 executables/DLLs, 67 MB)
- ✅ Fixed compression DLL packaging (`liblzma.dll` instead of incorrect `lzma.dll`)
- ✅ Implemented build caching for vcpkg and CMake (7+ min → 2-4 min builds)
- ✅ Automated GUI test skipping in CI environment
- ✅ Fixed package directory structure (proper cleanup prevents errors)

### Security & Verification
- ✅ Added SHA256 hash generation for all releases
- ✅ Included `SHA256SUMS.txt` in release assets
- ✅ Added Windows Defender guidance in documentation
- ✅ Transparent automated builds via GitHub Actions
**Technical Architecture:**
- **Encryption**: Windows BCrypt API (FIPS 140-2 compliant) - `encryption.cpp` (339 lines)
- **Hashing**: `QCryptographicHash` for SHA256 integrity verification - `file_hash.cpp`
- **User Scanning**: `windows_user_scanner.cpp` - NetUserEnum API wrapper
- **Orchestration**: `user_data_manager.cpp` - Coordinates backup/restore operations
- **Threading**: `user_profile_backup_worker.cpp/h` (multi-threaded), `user_profile_restore_worker.cpp/h`
- **Validation**: `input_validator.cpp` (345 lines) - OWASP-compliant path traversal prevention
**Restore Wizard:**
- User mapping and conflict resolution
- Merge mode configuration (skip, overwrite, rename)
- Permission settings preservation
- **Automatic decryption**: Supports encrypted backups with password
- Real-time progress tracking with detailed logging

**Technical Details:**
- Uses `QCryptographicHash` for SHA256 integrity verification
- AES-256-CBC encryption via Windows BCrypt API (FIPS 140-2 compliant)
- 32-byte salt + 16-byte IV per encryption for maximum security
- Supports registry hive backup (NTUSER.DAT, UsrClass.dat)
- Handles Windows ACL preservation (Access Control Lists)
- Multi-threaded file operations for performance

### 3. Directory Organizer
Automatically organize messy directories by file extension.

**Features:**
- Extension-based file sorting into folders
- Automatic collision handling (rename, skip, overwrite)
- Recursive directory processing
- Preview mode before committing changes
- Detailed action logging

### 4. Duplicate File Finder
Find and manage duplicate files using MD5 hashing.

**Capabilities:**
- MD5-based duplicate detection (true byte-level duplicates)
- Multi-threaded scanning for large directories
- Configurable actions: delete, move, or report
- Safe by default: always keeps one copy
- Memory-efficient streaming for large files

### 5. License Key Scanner
Attempt to locate software license keys stored in Windows Registry.

**Features:**
- Windows Registry scanning with pattern matching
- Multi-threaded registry traversal
- Automatic UAC elevation when needed
- Export results to file

**Limitations:**
- Only finds registry-stored keys (many modern apps use cloud/encrypted storage)
- Cannot decrypt protected keys
- Best-effort basis only

### 6. Image Flasher
Create bootable USB drives from Windows ISO files with automatic decompression.

**Capabilities:**
- Windows ISO downloader with language and edition selection
- Automatic drive detection with safety checks (prevents system drive selection)
- Streaming decompression support: gzip (.gz), bzip2 (.bz2), xz/lzma (.xz)
- Direct write to USB drives with progress tracking
- Verification after write (optional)
- Safe by default: System drives cannot be selected

**Use Case:** Create bootable Windows installation USB drives for PC repairs and fresh installations.

### GUI Architecture

**Main Window** (`main_window.cpp` - 180 lines):
- **5 Main Tabs**:
  1. **Backup** - Launch point for User Profile Backup/Restore wizards
  2. **Directory Organizer** - File organization by extension
  3. **Duplicate Finder** - MD5-based duplicate detection
  4. **License Scanner** - Registry key extraction
  5. **App Migration** - Application installation automation
- **Menu Bar**:
  - **File**: Exit
  - **Edit**: Undo, Redo, Settings
  - **Help**: About
- **Status Bar**: Hidden (individual panels have their own status displays)

**Wizards**:
- **User Profile Backup Wizard**: 6 pages (`user_profile_backup_wizard.cpp` - 1090+ lines)
  - Page 1: Welcome & Instructions
  - Page 2: Scan & Select Users
  - Page 3: Customize Per-User Data
  - Page 4: Smart Filter Configuration
  - Page 5: Backup Settings (Compression, Encryption, Verification)
  - Page 6: Execution & Progress
- **User Profile Restore Wizard**: 6 pages (`user_profile_restore_wizard.cpp` - 1081 lines)
  - Page 1: Welcome & Select Backup
  - Page 2: User Mapping (Source → Destination)
  - Page 3: Merge Configuration (Conflict Resolution)
  - Page 4: Folder Selection
### Technology Stack

| Component | Version | Purpose |
|-----------|---------|---------|
| **C++ Standard** | C++23 | Modern language features (`std::expected`, `std::stop_token`, `std::format`, `std::source_location`) |
| **GUI Framework** | Qt 6.5.3 | Cross-platform GUI library (Windows-only deployment) |
| **Build System** | CMake 3.28+ | Project configuration and build orchestration |
| **Compiler** | MSVC 19.44+ | Visual Studio 2022 toolchain with strict C++23 compliance |
| **Threading** | QtConcurrent + `std::stop_token` | Background task management with modern cancellation |
| **Cryptography** | Windows BCrypt API (FIPS 140-2) + QCryptographicHash | AES-256-CBC encryption, MD5/SHA256/SHA512 hashing |
| **Compression** | vcpkg (zlib, bzip2, liblzma) | gzip, bzip2, xz/lzma streaming decompression (no temp files) |
| **Package Manager** | Chocolatey (embedded 67 MB) | Fully embedded in `tools/chocolatey/` - no system install |
| **Windows APIs** | NetUserEnum, BCrypt, advapi32, DeviceIoControl | User enumeration, encryption, registry, drive access |
| **Error Handling** | `std::expected<T, error_code>` | Modern C++23 error propagation (100+ error codes) |
| **Input Validation** | OWASP-compliant (`input_validator.cpp`) | Path traversal prevention, injection attack prevention |
| **Logging** | Async file logging (`logger.cpp` - 286 lines) | Thread-safe, structured ISO 8601 timestamps, auto-rotation |
| **CI/CD** | GitHub Actions | Automated builds, vcpkg/CMake caching (2-4 min builds), release creation |er_settings_dialog.cpp/h`)
- **Windows ISO Download Dialog**: Language, architecture selection (`windows_iso_download_dialog.cpp` - 228 lines)
- **Per-User Customization Dialog**: Folder selection per user (`per_user_customization_dialog.cpp/h`)
- **Progress Dialog**: Generic progress display (`progress_dialog.cpp/h`)
- **About Dialog**: Credits, version, system info (`about_dialog.cpp/h`)
- **Log Viewer**: Detailed logging display (`log_viewer.cpp/h`)
### Core Application
- `sak_utility.exe` - Main application (1.33 MB)
- `portable.ini` - Enables portable mode (0 bytes, just presence matters)

### Qt 6.5.3 Runtime
- Qt6Core.dll, Qt6Gui.dll, Qt6Widgets.dll, Qt6Network.dll
- Platform plugins: `platforms/qwindows.dll`
- Style plugins: `styles/qwindowsvistastyle.dll`
- Image format plugins: `imageformats/` (PNG, JPEG, etc.)
- TLS plugins for HTTPS support

### Compression Libraries (vcpkg)
- `zlib1.dll` - gzip compression/decompression
- `bz2.dll` - bzip2 compression/decompression  
- `liblzma.dll` - xz/lzma compression/decompression

### Embedded Chocolatey (67 MB)
- `tools/chocolatey/choco.exe` - Main Chocolatey executable
- `tools/chocolatey/bin/` - Helper tools (7z.exe, wget.exe, shimgen.exe, checksum.exe)
- `tools/chocolatey/helpers/` - PowerShell modules
- `tools/chocolatey/lib/` - Package cache (initially empty)
- **No system installation required** - completely self-contained

### Documentation
- `README.md` - Full documentation
- `LICENSE` - GPL v2 license
- `CONTRIBUTING.md` - Contribution guidelines (if present)

**Total Package Size**: ~21.5 MB compressed, ~95 MB extracted

---

## Recent Changes (v0.5.4)

### Build & Packaging Improvements
- ✅ Added all Chocolatey binaries to repository (22 executables/DLLs)
- ✅ Fixed compression DLL packaging (`liblzma.dll` instead of incorrect `lzma.dll`)
- ✅ Implemented build caching for vcpkg and CMake (7+ min → 2-4 min builds)
- ✅ Automated GUI test skipping in CI environment
- ✅ Fixed package directory structure (proper cleanup prevents errors)

### Security & Verification
- ✅ Added SHA256 hash generation for all releases
- ✅ Included `SHA256SUMS.txt` in release assets
- ✅ Added Windows Defender guidance in documentation
- ✅ Transparent automated builds via GitHub Actions

### Developer Experience
- ✅ GitHub Actions workflow with proper permissions
- ✅ Automatic release creation on version tags
- ✅ Comprehensive error handling in build pipeline

---

## Technical Architecture

### Technology Stack

| Component | Version | Purpose |
|-----------|---------|---------|
| **C++ Standard** | C++23 | Modern language features, strict compliance |
| **GUI Framework** | Qt 6.5.3 | Cross-platform GUI library (Windows-only deployment) |
| **Build System** | CMake 3.28+ | Project configuration and build orchestration |
| **Compiler** | MSVC 19.44+ | Visual Studio 2022 toolchain (MSVC 2022) |
| **Threading** | QtConcurrent | Background task management |
| **Cryptography** | Windows BCrypt + QCryptographicHash | AES-256-CBC encryption, MD5/SHA256 hashing |
| **Compression** | vcpkg (zlib, bzip2, liblzma) | gzip, bzip2, xz/lzma decompression support |
| **Package Manager** | Chocolatey (embedded) | Fully embedded in `tools/chocolatey/` - no system install |
| **CI/CD** | GitHub Actions | Automated builds, caching, release creation |

### Code Structure

```
S.A.K.-Utility/
├── src/
│   ├── core/                          # Business logic (35+ files)
│   │   ├── app_scanner.cpp             # Windows app detection (Registry + filesystem)
│   │   ├── chocolatey_manager.cpp      # Embedded Chocolatey integration
│   │   ├── package_matcher.cpp         # Fuzzy app-to-package matching
│   │   ├── file_hash.cpp               # MD5/SHA256 via QCryptographicHash
│   │   ├── user_data_manager.cpp       # User profile backup/restore orchestration
│   │   ├── windows_user_scanner.cpp    # NetUserEnum API wrapper
│   │   ├── permission_manager.cpp      # Windows ACL handling
│   │   ├── smart_file_filter.cpp       # Intelligent file exclusion rules
│   │   ├── migration_report.cpp        # Report generation (JSON/CSV/HTML)
│   │   ├── encryption.cpp              # AES-256-CBC encryption (339 lines)
│   │   ├── drive_scanner.cpp           # USB drive detection and safety checks
│   │   ├── windows_iso_downloader.cpp  # Windows ISO download API
│   │   ├── image_writer.cpp            # Buffered USB writing
│   │   ├── flash_coordinator.cpp       # Flash operation orchestration
│   │   ├── streaming_decompressor.cpp  # Base decompression interface
│   │   ├── gzip_decompressor.cpp       # gzip decompression
│   │   ├── bzip2_decompressor.cpp      # bzip2 decompression
│   │   ├── xz_decompressor.cpp         # xz/lzma decompression
│   │   ├── keep_awake.cpp              # Prevent system sleep (no GUI)
│   │   └── logger.cpp                  # Async file logging
│   │
│   ├── gui/                            # Qt interface (21+ files)
│   │   ├── main_window.cpp             # 6-tab main window
│   │   ├── backup_panel.cpp            # Launch point for profile wizards
│   │   ├── user_profile_backup_wizard.cpp    # 6-page backup wizard (1090+ lines)
│   │   ├── user_profile_restore_wizard.cpp   # 6-page restore wizard (1081 lines)
│   │   ├── app_migration_panel.cpp     # Application migration GUI (921 lines)
│   │   ├── image_flasher_panel.cpp     # Image flasher GUI (600+ lines)
│   │   ├── windows_iso_download_dialog.cpp   # Windows ISO downloader dialog
│   │   ├── organizer_panel.cpp         # Directory organizer GUI
│   │   ├── duplicate_finder_panel.cpp  # Duplicate finder GUI
│   │   ├── license_scanner_panel.cpp   # License scanner GUI
│   │   ├── settings_dialog.cpp         # Multi-tab settings
│   │   └── about_dialog.cpp            # About/credits/system info
│   │
│   ├── threading/                      # Background workers
│   │   └── flash_worker.cpp            # Threaded USB flash operations
│   │
│   └── main.cpp                        # Entry point, logger initialization
│
├── include/sak/                        # Public headers (50+ files)
├── tests/                              # Unit tests (11 test programs)
├── tools/chocolatey/                   # Embedded Chocolatey binaries (67 MB)
├── resources/                          # Icons and Qt resources
└── CMakeLists.txt                      # Build configuration
```

### GUI Architecture

**Main Window** (`main_window.cpp`):
- 5 tabs: Backup, Directory Organizer, Duplicate Finder, License Scanner, App Migration
- Menu bar: File (Exit), Edit (Undo/Redo/Settings), Help (About)
- Status bar hidden (panels have individual status bars)

**Wizards**:
- User Profile Backup Wizard: 6 pages (Welcome, Select Users, Customize Data, Smart Filters, Settings, Execute)
- User Profile Restore Wizard: 6 pages (Welcome, User Mapping, Merge Config, Folder Selection, Permissions, Execute)
- Backup Wizard: 4 pages (general backup, not user-profile-specific)
- Restore Wizard: 4 pages (general restore, not user-profile-specific)

### Portable Mode

S.A.K. Utility supports **true portable operation**:

**How to Enable:**
1. Create an empty file named `portable.ini` in the same directory as `sak_utility.exe`
2. Launch the application
3. All settings will be stored in `settings.ini` (same directory) instead of Windows Registry

**Portable Features:**
- No registry access when `portable.ini` is detected
- Dynamic path resolution using `%SystemDrive%` and `%ProgramData%` environment variables
- Settings stored in local INI file (`QSettings` with `IniFormat`)
- Copy entire folder to USB drive or network location - it just works
- Chocolatey cache stays in `tools/chocolatey/lib/` (portable packages)

---

## Backend Systems & Infrastructure

### **Logging System** (`logger.cpp` - 286 lines)

**Features:**
- **Thread-Safe**: Mutex protection for multi-threaded writes
- **Structured Output**: ISO 8601 timestamps (`2025-12-11T14:32:15.123Z`)
- **5 Severity Levels**: Debug, Info, Warning, Error, Critical
- **Automatic Log Rotation**: When log file exceeds size threshold
- **Async File Writing**: Non-blocking I/O for performance
- **Console Output**: Configurable (enabled in debug builds)

**Log Location**: `%LOCALAPPDATA%\SAK\Utility\logs\sak_utility.log`

### **Encryption System** (`encryption.cpp` - 339 lines)

**Algorithm**: AES-256-CBC (FIPS 140-2 compliant via Windows BCrypt API)

**Key Derivation**: PBKDF2 with SHA-256
- **Iterations**: 100,000 (OWASP recommended minimum)
- **Key Size**: 32 bytes (256 bits)
- **IV Size**: 16 bytes (128 bits - AES block size)
- **Salt Size**: 32 bytes (random per encryption)

**Format**: `[Salt(32)][IV(16)][EncryptedData]`

**Security Features:**
- Random salt per encryption (prevents rainbow tables)
- Random IV per encryption (prevents pattern analysis)
- PBKDF2 key stretching (slows brute-force attacks)
- Windows BCrypt API (FIPS 140-2 certified)

### **File Hashing** (`file_hash.cpp/h`)

**Algorithms Supported:**
- **MD5**: Fast, less secure (used for duplicate detection)
- **SHA256**: Slower, more secure (used for integrity verification)
- **SHA512**: Slowest, most secure (used for Image Flasher verification)

**Features:**
- **Chunked Reading**: 1 MB default (memory-efficient for large files)
- **Progress Callback**: Updates UI during long operations
- **Cancellation**: Via `std::atomic<bool>` flag
- **Qt Integration**: Uses `QCryptographicHash` (no external dependencies)

### **Error Handling** (`error_handling.cpp/h` - 259 lines)

**Modern C++23 Pattern**: `std::expected<T, error_code>`

**Error Categories (100+ error codes)**:
- File system errors (1-99), I/O errors (100-199), Hash/verification errors (200-299)
- Configuration errors (300-399), Platform errors (400-499), Threading errors (500-599)
- Memory errors (600-699), Scanner/organizer errors (700-799), Network errors (800-899)
- Security/validation errors (850-899), Generic errors (900-999)

### **Keep Awake System** (`keep_awake.cpp/h`)

**Purpose**: Prevent system sleep during long operations (backup, restore, flash)

**Implementation**: `SetThreadExecutionState` Windows API with RAII guard pattern

**Status**: Backend exists, **no GUI checkbox yet** (planned for v0.6)

---

## Installation

### Option 1: Pre-built Binary (Recommended)

1. Download the latest release from [Releases](https://github.com/RandyNorthrup/S.A.K.-Utility/releases)
2. Extract to your preferred location (e.g., `C:\Tools\SAK-Utility\`)
3. Run `sak_utility.exe`
4. **(Optional)** Create `portable.ini` for portable mode

### Windows Defender / SmartScreen Warnings

**Why does this happen?**
- The application is not code-signed (signing certificates cost $300+/year)
- Windows Defender may flag new executables without established reputation
- This is a **false positive** - the source code is fully open and auditable

**Solutions:**

1. **Add Windows Defender Exclusion (Recommended):**
   ```powershell
   # Run as Administrator
   Add-MpPreference -ExclusionPath "C:\Tools\SAK-Utility"
   ```
   Or manually: Windows Security → Virus & threat protection → Manage settings → Exclusions → Add folder

2. **SmartScreen Warning:**
   - Click "More info" → "Run anyway"
   - The warning will disappear as the app builds reputation

3. **Verify Download Integrity:**
   - Check SHA256 hash matches the release notes
   - All releases are built automatically via GitHub Actions (transparent build process)

**Why trust this software?**
- ✅ **100% Open Source** - All code is public and auditable on GitHub
- ✅ **Automated Builds** - Built via GitHub Actions, no manual tampering
- ✅ **Active Development** - Regular updates and community contributions
- ✅ **No Telemetry** - Zero data collection, fully offline capable

### Option 2: Build from Source

**Prerequisites:**
- Windows 10/11 x64
- Visual Studio 2022 (Community Edition is free)
  - Workload: "Desktop development with C++"
- CMake 3.28 or higher
- Qt 6.5.3 (msvc2019_64 kit)
- Git

**Build Commands:**
```powershell
# Clone repository
git clone https://github.com/RandyNorthrup/S.A.K.-Utility.git
cd S.A.K.-Utility

# Configure with CMake
cmake -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.5.3/msvc2019_64"

# Build Release
cmake --build build --config Release

# Executable will be in build\Release\sak_utility.exe
```

**Troubleshooting:**
- **Qt not found**: Verify `-DCMAKE_PREFIX_PATH` points to your Qt installation
- **Missing DLLs**: Run from `build\Release\` directory (windeployqt runs automatically during build)
- **Build errors**: Ensure Visual Studio 2022 with C++23 support is installed

---

## Usage

### Application Migration

1. Launch S.A.K. Utility and switch to **App Migration** tab
2. Click **"Scan Applications"** (may prompt for administrator elevation)
3. Wait for scan to complete (scans Windows Registry and filesystem)
4. Click **"Match Packages"** to find Chocolatey equivalents
5. Select apps to install by checking boxes in the **Select** column
6. **(Optional)** Check **"Lock Version"** column to pin specific versions
7. Click **"Install Selected"** (requires administrator privileges)
8. Monitor real-time installation progress in the log window
9. Use **File → Generate Report** to export migration plan (JSON/CSV/HTML)

**Import Migration Plan:**
- Use **File → Import** to load a previously exported migration plan
- Useful for installing the same apps on multiple machines

### User Profile Backup

1. Switch to **Backup** tab
2. Click **"Backup User Profiles..."** button
3. **Welcome Page**: Read overview, click **Next**
4. **Select Users**: Check users to backup (detected via NetUserEnum API)
5. **Customize Data**: Select folders for each user (Documents, Desktop, Pictures, Videos, Music, Downloads, AppData)
   - Click **"Customize"** for per-user folder selection
6. **Smart Filters**: Review exclusion rules (temp files, caches, thumbnails, browser caches)
7. **Settings**: Choose destination folder, enable compression (optional), enable verification (SHA256)
8. **Execute**: Monitor progress with detailed logging, file/byte progress bars
9. Click **Finish** when complete - backup manifest saved as JSON

### User Profile Restore

1. Switch to **Backup** tab
2. Click **"Restore User Profiles..."** button
3. **Welcome Page**: Read overview, click **Next**
4. **Select Backup**: Choose backup folder (reads manifest.json)
5. **User Mapping**: Map backup users to current system users
6. **Merge Config**: Choose merge mode (skip existing, overwrite, rename)
7. **Folder Selection**: Choose which folders to restore
8. **Permissions**: Configure ACL preservation settings
9. **Execute**: Monitor restore progress with detailed logging
10. Click **Finish** when complete

### Directory Organizer

1. Switch to **Directory Organizer** tab
2. Click **Browse** to select directory to organize
3. Choose options:
   - **Sort by extension**: Creates folders like `Images/`, `Documents/`, etc.
   - **Recursive**: Process subdirectories
   - **Collision handling**: Skip, rename, or overwrite
4. Click **"Organize"** to execute
5. Monitor progress in log window

### Duplicate File Finder

1. Switch to **Duplicate Finder** tab
2. Click **Browse** to select directory to scan
3. Click **"Find Duplicates"** to start scan
4. Wait for MD5 hashing to complete (multi-threaded)
5. Review results grouped by hash
6. Select action:
   - **Delete**: Remove duplicates (keeps one copy)
   - **Move**: Move to separate folder
   - **Report**: Export list to file
7. Click **"Execute"** to perform action

### License Key Scanner

1. Switch to **License Scanner** tab
2. Click **"Scan Registry"**
3. Allow UAC elevation when prompted (required for registry access)
4. Wait for scan to complete (searches for patterns like "key", "serial", "license")
5. Review potential keys in results table
6. Click **"Export"** to save findings to file

**Warning**: This is a best-effort tool. Modern applications often use:
- Cloud-based licensing (keys stored on vendor servers)
- Hardware tokens or dongles
- Encrypted credential storage
- Windows Credential Manager

---

## Compiler Flags & Code Quality

### MSVC (Visual Studio 2022)

```cmake
/W4                     # Warning level 4 (highest)
/WX                     # Treat warnings as errors
/permissive-            # Strict C++ conformance (no Microsoft extensions)
/Zc:__cplusplus         # Correct __cplusplus macro value
/sdl                    # Security Development Lifecycle checks
/MP                     # Multi-processor compilation
/Zc:inline              # Remove unreferenced COMDAT
/Zc:preprocessor        # Conforming C++23 preprocessor
/utf-8                  # UTF-8 source and execution charset
/guard:cf               # Control Flow Guard (security)
/O2                     # Maximum optimization (Release)
/LTCG                   # Link-Time Code Generation (Release)
```

### Code Standards

- **C++23 strict compliance** - No compiler extensions
- **Zero warnings** - All warnings treated as errors
- **RAII everywhere** - No manual memory management
- **Qt `QT_NO_KEYWORDS` mode** - Uses `Q_EMIT`, `Q_SIGNALS`, `Q_SLOTS` to avoid macro conflicts
- **Modern error handling** - Uses `std::expected` for error propagation
- **Type safety** - No C-style casts, `std::optional` for nullable values

---

## Testing

**11 test executables** are built and located in `build/Release/Release/`:

| Test | Size | Purpose |
|------|------|---------|
| `test_app_scanner.exe` | 30 KB | Windows app detection (Registry + filesystem) |
| `test_chocolatey_manager.exe` | 47 KB | Embedded Chocolatey integration |
| `test_choco_install.exe` | 41.5 KB | Real package installation testing |
| `test_integration_phase1_2.exe` | 57.5 KB | Integration tests for phases 1-2 |
| `test_package_matcher.exe` | 136 KB | Fuzzy app-to-package matching |
| `test_user_data_manager.exe` | 105 KB | User profile backup/restore core |
| `test_backup_wizard.exe` | 123.5 KB | General backup wizard GUI |
| `test_restore_wizard.exe` | 128.5 KB | General restore wizard GUI |
| `test_migration_report.exe` | 166 KB | Report generation (JSON/CSV/HTML) |
| `test_app_migration_worker.exe` | 167 KB | Background installation worker |
| `test_app_migration_panel.exe` | 330.5 KB | Application migration panel GUI |

**Total**: 11 test executables + 1 main application (`sak_utility.exe` - 1.13 MB)

**Test Coverage:**
- **Core Logic**: Unit tests for scanner, matcher, report generator
- **Integration**: Scanner + Chocolatey, Worker + Manager
- **GUI**: Wizard page flows, panel interactions
- **Real Operations**: Actual Chocolatey package installation

**CI/CD** (GitHub Actions):
- Automated builds on push/PR to main branch
- **vcpkg caching**: `installed/`, `packages/`, `buildtrees/` directories
- **CMake caching**: `build/` directory (excludes artifacts)
- **Build time**: 2-4 minutes (down from 7+ minutes without caching)
- **GUI test skipping**: 3 tests skipped in CI environment (test_app_migration_panel, test_backup_wizard, test_restore_wizard)
- **Automatic release creation**: On `v*` tags (e.g., `v0.5.5`)
- **SHA256 hash generation**: For exe and ZIP in `SHA256SUMS.txt`

**Note**: CTest integration is pending. Tests must be run manually from `build/Release/Release/` directory.

---

## Limitations & Known Issues

### Platform Support

- **Windows 10/11 x64**: Fully supported and tested
- **macOS**: Not supported (code stubs exist but are non-functional)
- **Linux**: Not supported (code stubs exist but are non-functional)

**Why Windows-Only?**
- Chocolatey is Windows-exclusive
- Windows Registry scanning (License Scanner, App Scanner)
- NetUserEnum API for user detection (Backup/Restore wizards)
- Windows ACL/permissions handling (PermissionManager)
- NTFS filesystem features

### Feature Limitations

**License Scanner:**
- Only finds registry-stored keys
- Cannot decrypt encrypted keys
- Many modern apps don't store keys in registry (cloud licensing, hardware tokens)

**Application Migration:**
- Requires administrator privileges for Chocolatey operations
- Only works with apps that have Chocolatey packages
- Some apps may require manual configuration after installation

**User Profile Backup:**
- Not a full system image backup
- No incremental backup support (full backup only)
- No scheduling (manual execution only)
- No built-in compression (files copied as-is, optional ZIP coming in v0.6)

**Keep-Awake Feature:**
- Backend class exists (`keep_awake.cpp`) but **no GUI checkbox**
- System can still sleep during long operations
- Planned for v0.6

---

## Roadmap

### Version 0.6 (Next Release)
- Keep-Awake GUI checkbox
- CTest integration for automated test discovery
- Backup compression support (ZIP/7z)
- Dark mode theme
- Enhanced logging viewer with filtering

### Version 0.7
- Incremental backup support
- Backup scheduling via Windows Task Scheduler
- Settings persistence improvements
- ACL preservation in all backup modes

### Version 1.0 (Stable Release)
- Full ACL backup/restore verification
- Comprehensive documentation
- Code signing certificate
- Installer package (MSI)

### Future Considerations
- Network backup capabilities
- Cloud backup integration (OneDrive, Google Drive)
- Plugin system for extensibility
- macOS support (requires significant development)
- Linux support (requires significant development)

---

## License

**GNU General Public License v2.0**

Copyright (C) 2025 Randy Northrup

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

See [LICENSE](LICENSE) file for full text.

### Third-Party Licenses

- **Qt Framework 6.5.3**: LGPL v3
  - Used for: GUI, threading, networking, cryptography
  - Website: https://www.qt.io/
  
- **Chocolatey**: Apache 2.0
  - Used for: Windows package management (embedded)
  - Website: https://chocolatey.org/

---

## Credits

**Development:**
- Lead Developer: Randy Northrup
- Original Python Version: Proof of concept (archived in `python_archive/`)

**Third-Party Components:**
- Qt Framework - Cross-platform GUI library with cryptographic functions
- Chocolatey - Windows package manager (fully embedded, no external installation)
- Microsoft Windows API - User enumeration (NetUserEnum), Registry access, ACL management

**Special Thanks:**
- C++ and Qt communities for documentation and support
- Microsoft for Windows API documentation
- Chocolatey community for package database

---

## Support & Contributions

- **Issues**: [GitHub Issues](https://github.com/RandyNorthrup/S.A.K.-Utility/issues)
- **Discussions**: [GitHub Discussions](https://github.com/RandyNorthrup/S.A.K.-Utility/discussions)
- **Source Code**: [GitHub Repository](https://github.com/RandyNorthrup/S.A.K.-Utility)

**Contributing:**
See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

Areas needing help:
- macOS/Linux implementations (significant effort required)
- Additional Chocolatey package mappings
- UI/UX improvements
- Documentation
- Testing on various Windows configurations (Win10 Home, Pro, Enterprise)

---

**Built with C++23 and Qt6 | Windows 10/11 x64**

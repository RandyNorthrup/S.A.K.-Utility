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
**Version**: 0.5.1  
**Release Date**: December 11, 2025  
**Platform**: Windows 10/11 x64 only  
**Build Size**: 1.33 MB executable

---

## Core Features

### 1. Application Migration
Automate software installation on new or refreshed Windows systems using embedded Chocolatey.

**Capabilities:**
- Scan installed applications (Windows Registry + filesystem detection)
- Automatic Chocolatey package matching with confidence scoring
- Interactive selection with version locking
- Batch installation with real-time progress tracking
- Export/import migration plans (JSON, CSV, HTML formats)
- **No external dependencies** - Chocolatey is fully embedded

**Use Case:** Migrate from old PC to new PC by scanning apps on the old machine, exporting a migration plan, then importing and installing on the new machine.

### 2. User Profile Backup & Restore
Comprehensive Windows user profile backup with intelligent 6-page wizards and enterprise-grade encryption.

**Backup Wizard:**
- Automatic Windows user detection via NetUserEnum API
- Selective folder backup (Documents, Desktop, Pictures, Videos, Music, Downloads, AppData)
- Per-user customization dialogs for granular control
- Smart file filtering (skip temp files, caches, registry hives)
- **Compression support**: 4 levels (None, Fast, Balanced, Maximum) using PowerShell Compress-Archive
- **AES-256-CBC encryption**: Enterprise-grade encryption with PBKDF2 key derivation (100,000 iterations)
- Multi-threaded backup with SHA256 verification
- Backup manifest generation with metadata
- Password protection with confirmation validation

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

**Technical Details:**
- Integrates with Microsoft's Windows ISO download API
- Multi-threaded decompression for performance
- Buffered writing with cancellation support
- Drive locking during write operations to prevent corruption

---

## Technical Architecture

### Technology Stack

| Component | Version | Purpose |
|-----------|---------|---------|
| **C++ Standard** | C++23 | Modern language features, strict compliance |
| **GUI Framework** | Qt 6.5.3 | Cross-platform GUI library (Windows-only deployment) |
| **Build System** | CMake 3.28+ | Project configuration and build orchestration |
| **Compiler** | MSVC 19.44+ | Visual Studio 2022 toolchain |
| **Threading** | QtConcurrent | Background task management |
| **Cryptography** | Windows BCrypt + QCryptographicHash | AES-256-CBC encryption, MD5/SHA256 hashing |
| **Package Manager** | Chocolatey | Embedded in `tools/chocolatey/` directory |

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
- Settings stored in local INI file (QSettings with IniFormat)
- Copy entire folder to USB drive or network location - it just works

---

## Installation

### Option 1: Pre-built Binary (Recommended)

1. Download the latest release from [Releases](https://github.com/RandyNorthrup/S.A.K.-Utility/releases)
2. Extract to your preferred location (e.g., `C:\Tools\SAK-Utility\`)
3. Run `sak_utility.exe`
4. **(Optional)** Create `portable.ini` for portable mode

**Note**: Windows SmartScreen may show a warning for unsigned executables. Click "More info" → "Run anyway".

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

12 test executables are built and located in `build/Release/Release/`:

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
| `sak_utility.exe` | 1.13 MB | Main application |

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

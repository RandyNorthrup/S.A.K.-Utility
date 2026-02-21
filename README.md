# S.A.K. Utility

<div align="center">

**Swiss Army Knife Utility** — A portable Windows toolkit for PC technicians, IT pros, and sysadmins.

[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://isocpp.org/)
[![Qt 6.5.3](https://img.shields.io/badge/Qt-6.5.3-41cd52.svg)](https://www.qt.io/)
[![Windows 10/11](https://img.shields.io/badge/Windows-10%20%7C%2011-0078d4.svg)](https://www.microsoft.com/windows)
[![Build](https://github.com/RandyNorthrup/S.A.K.-Utility/actions/workflows/build-release.yml/badge.svg)](https://github.com/RandyNorthrup/S.A.K.-Utility/actions)

Migration · Maintenance · Recovery · Imaging · Deployment — one portable EXE.

</div>

---

## Highlights

| | |
|---|---|
| **100 % Portable** | No installer. Drop on a USB stick and go. `portable.ini` enables local settings. |
| **39 Quick Actions** | One-click system optimization, backups, maintenance, troubleshooting, and recovery. |
| **User Profile Backup & Restore** | 6-page wizards with smart filtering, AES-256 encryption, and NTFS permission handling. |
| **Application Migration** | Scan installed apps, match to Chocolatey packages, bulk-install on a new PC. |
| **Network Transfer** | Peer-to-peer LAN migration with AES-256-GCM, resume, and multi-PC orchestrator mode. |
| **Image Flasher** | Flash ISOs/IMGs to USB. Download Windows ISOs directly from Microsoft. |
| **BitLocker Key Backup** | Export recovery keys from all encrypted volumes with restricted-permission files. |
| **Modern UI** | Windows 11-style rounded corners, custom splash screen, and responsive layouts. |

---

## Table of Contents

- [System Requirements](#system-requirements)
- [Quick Start](#quick-start)
- [Features](#features)
  - [Quick Actions](#quick-actions)
  - [User Profile Backup & Restore](#user-profile-backup--restore)
  - [Application Migration](#application-migration)
  - [Network Transfer](#network-transfer)
  - [Image Flasher](#image-flasher)
  - [Directory Organizer](#directory-organizer)
  - [Duplicate Finder](#duplicate-finder)
- [Security](#security)
- [Building from Source](#building-from-source)
- [Configuration](#configuration)
- [Contributing](#contributing)
- [License](#license)
- [Acknowledgments](#acknowledgments)

---

## System Requirements

| | Minimum |
|---|---|
| **OS** | Windows 10 (1809) or Windows 11 |
| **CPU** | x64 Intel or AMD |
| **RAM** | 4 GB |
| **Disk** | ~500 MB + working space |
| **Network** | Ethernet / Wi-Fi (for Network Transfer) |
| **Privileges** | Administrator (most features) |

---

## Quick Start

```text
1. Download the latest ZIP from Releases.
2. Extract anywhere (USB drive, desktop, network share).
3. Run sak_utility.exe and accept the UAC prompt.
```

> **SmartScreen / Defender note:** The EXE is not code-signed (certificate costs >`$`300/yr).
> Click **More info -> Run anyway**, or add a Defender exclusion:
> `Add-MpPreference -ExclusionPath "C:\path\to\SAK-Utility"`

---

## Features

### Quick Actions

**39 one-click operations** organized into five categories with real-time progress and detailed logging.

<details>
<summary><strong>System Optimization (8)</strong></summary>

| Action | Description |
|---|---|
| Disk Cleanup | Temp files, browser caches, recycle bin, thumbnails |
| Clear Browser Cache | Chrome, Edge, Firefox, Brave, Vivaldi, Opera — all profiles |
| Defragment Drives | HDD optimization and SSD TRIM via `defrag.exe` |
| Clear Windows Update Cache | Stops WU service, clears `SoftwareDistribution`, restarts |
| Disable Startup Programs | Identifies and disables unnecessary startup items |
| Clear Event Logs | Archives and clears Application, System, Security, Setup logs |
| Optimize Power Settings | Applies High Performance / Ultimate Performance plan |
| Disable Visual Effects | Reduces animations for snappier UX on older hardware |

</details>

<details>
<summary><strong>Quick Backups (8)</strong></summary>

| Action | Description |
|---|---|
| QuickBooks Backup | .QBW, .QBB, .TLG, .ND files from common locations |
| Browser Profile Backup | Bookmarks, passwords (encrypted), history, extensions |
| Outlook Backup | .PST/.OST, signatures, rules, contacts |
| Sticky Notes Backup | `plum.sqlite` database |
| Saved Game Data | Steam, Epic, Origin, Xbox Game Pass save files |
| Tax Software Backup | TurboTax, H&R Block, TaxAct data |
| Photo Management Backup | Lightroom catalogs, Photoshop preferences |
| Dev Configs Backup | VS Code settings, Git config, SSH keys, env vars |

</details>

<details>
<summary><strong>Maintenance (8)</strong></summary>

| Action | Description |
|---|---|
| Check Disk Health | SMART data via `Get-PhysicalDisk` (temp, wear, errors) |
| Update All Apps | Runs WinGet + Chocolatey updates with reporting |
| Windows Update | Triggers via UsoClient |
| Verify System Files | `sfc /scannow` + `DISM /RestoreHealth` |
| Check Disk Errors | `chkdsk` with bad-sector scan |
| Rebuild Icon Cache | Deletes `IconCache.db`, restarts Explorer |
| Reset Network | Flushes DNS, resets TCP/IP, Winsock, firewall, adapters |
| Clear Print Spooler | Stops spooler, deletes stuck jobs, restarts |

</details>

<details>
<summary><strong>Troubleshooting (6)</strong></summary>

| Action | Description |
|---|---|
| Generate System Report | 100+ properties: OS, hardware, drivers, event logs, programs (HTML) |
| Check for Bloatware | Scans for known bloatware and OEM software |
| Test Network Speed | Download/upload speed and latency |
| Scan for Malware | Windows Defender quick scan via `Start-MpScan` |
| Repair Windows Store | `WSReset.exe` + re-register Store apps |
| Fix Audio Issues | Restarts Windows Audio service, rebuilds device enumeration |

</details>

<details>
<summary><strong>Emergency Recovery (9)</strong></summary>

| Action | Description |
|---|---|
| Backup Browser Data | Emergency backup of all browser profiles for all users |
| Backup Email Data | Outlook .PST, Thunderbird profiles, Windows Mail |
| Create Restore Point | System Restore checkpoint via WMI |
| Export Registry Keys | Critical registry hives (HKLM\Software, HKCU, etc.) |
| Backup Activation Keys | Windows and Office product keys from registry |
| Screenshot Settings | Captures screenshots of Windows Settings panels |
| Backup Desktop Wallpaper | Saves current wallpaper and theme files |
| Backup Printer Settings | Printer drivers, queues, and port configurations |
| Backup BitLocker Keys | Recovery keys for all encrypted volumes (restricted permissions) |

</details>

---

### User Profile Backup & Restore

Two 6-page wizards for comprehensive user-profile migration with smart filtering, encryption, and NTFS permission handling.

**Backup Wizard**
1. Welcome
2. Scan & select user profiles (auto-excludes system accounts)
3. Per-user data customization (Desktop, Documents, AppData, browsers, email …)
4. Smart filter config (exclude temp/cache, size limits, locked-file detection)
5. Settings (destination, compression 0-9, AES-256 encryption, permission mode)
6. Execution with real-time progress, log viewer, and cancellation support

**Restore Wizard**
1. Welcome
2. Select backup manifest (`.json`)
3. Map source users to destination users (auto-map or drag-and-drop)
4. Merge options (skip / overwrite / rename, permission handling, checksums)
5. Review & confirm
6. Execution with progress, logging, and restore report

---

### Application Migration

Scan installed apps, match them to Chocolatey packages, and bulk-install on a new PC.

1. **Scan** — Queries HKLM/HKCU Uninstall registry keys; extracts name, version, publisher.
2. **Match** — `PackageMatcher` with 42 curated mappings (high/medium/low/manual confidence).
3. **Backup** — Optional data backup via `BackupWizard` (browser profiles, IDE settings, etc.).
4. **Export** — JSON migration report portable to the target machine.
5. **Install** — Embedded Chocolatey with retry logic (3 attempts, exponential backoff).
6. **Restore** — `RestoreWizard` maps source paths to target paths and restores data.

---

### Network Transfer

Secure peer-to-peer LAN transfer with three modes.

| Mode | Description |
|---|---|
| **Source** | Scan users, discover peers (UDP 54321), connect, send encrypted data |
| **Destination** | Listen for incoming transfers, approve/reject, receive + restore |
| **Orchestrator** | Centralized multi-PC deployment — mapping strategies, concurrency, job queues |

**Security:** AES-256-GCM per chunk (64 KB), PBKDF2 key derivation (100 k iterations), challenge/response authentication, SHA-256 integrity per file.

**Resume:** Checkpoint every 1 MB, partial-file tracking, integrity validation before resume.

---

### Image Flasher

Create bootable USB drives from disk images.

- **Formats:** ISO, IMG, WIC, ZIP, GZ, BZ2, XZ, DMG, DSK
- **Windows ISO download** directly from Microsoft (multiple languages, x64/ARM64)
- **Windows USB Creator** — NTFS format, ISO extraction, `bootsect.exe` boot sector, UEFI structure, verification of critical files
- **Generic writer** — Raw sector I/O with streaming decompression
- **Safety:** System-drive protection, multi-select for parallel flash

---

### Directory Organizer

Organize files by extension into categorized subdirectories.

- Default categories: Documents, Images, Videos, Audio, Archives, Code, Spreadsheets, Executables
- Collision handling: rename, skip, or overwrite
- Preview mode — see what would happen without moving anything

---

### Duplicate Finder

SHA-256 hash-based duplicate detection with size pre-filtering.

- Multi-directory recursive scan
- Minimum-size filter to skip small files
- Summary: duplicate count, wasted space

---

## Security

| Layer | Implementation |
|---|---|
| **File encryption** | AES-256-CBC via Windows BCrypt (PBKDF2, 100 k iterations, 32-byte salt) |
| **Network encryption** | AES-256-GCM via Windows BCrypt (12-byte nonce, 16-byte GCM tag per chunk) |
| **Secure memory** | `SecureString` / `SecureBuffer` — zero-fill on destruction, page locking |
| **Input validation** | Path sanitization, IP/port validation, Chocolatey name format checks |
| **Elevation** | UAC manifest (`requireAdministrator`), runtime privilege verification |

All crypto uses the **Windows BCrypt API** (FIPS 140-2 validated provider).

---

## Building from Source

### Prerequisites

| Tool | Version |
|---|---|
| Visual Studio 2022 | v17+ with _Desktop development with C++_ |
| CMake | 3.28+ |
| Qt | 6.5.3 MSVC 2019 64-bit |
| vcpkg | Latest (for zlib, bzip2, liblzma) |

### Build

```powershell
git clone https://github.com/RandyNorthrup/S.A.K.-Utility.git
cd S.A.K.-Utility

# Configure
cmake -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.5.3/msvc2019_64"

# Build
cmake --build build --config Release --parallel

# Run
.\build\Release\sak_utility.exe
```

See [BUILD.md](BUILD.md) for full build instructions, compiler flags, and verification steps.

### Dependencies

| Library | License | Purpose |
|---|---|---|
| [Qt 6.5.3](https://www.qt.io/) | LGPL v3 | UI framework (Core, Widgets, Concurrent, Network) |
| [zlib](https://www.zlib.net/) | zlib License | gzip compression |
| [bzip2](https://sourceware.org/bzip2/) | BSD-style | bzip2 compression |
| [liblzma](https://tukaani.org/xz/) | 0BSD / Public Domain | xz/LZMA compression |
| [Chocolatey](https://chocolatey.org/) | Apache 2.0 | Embedded package manager |
| Windows BCrypt | OS component | AES-256, PBKDF2, SHA-256 |

Full license texts: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)

### Testing

```powershell
cmake --build build --config Release --target RUN_TESTS
```

17 unit tests, 2 integration tests.

---

## Configuration

### Portable Mode

Place an empty `portable.ini` next to `sak_utility.exe`. Settings and logs are stored alongside the executable.

### Normal Mode

Settings: `%APPDATA%\SAK_Utility\config.ini`
Logs: `%APPDATA%\SAK_Utility\_logs\`

### Key Settings

```ini
[QuickActions]
backup_location=C:/SAK_Backups

[NetworkTransfer]
discovery_port=54321
control_port=54322
data_port=54323
encryption_enabled=true
```

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for coding standards, commit conventions, and PR workflow.

**Quick summary:**

1. Fork -> feature branch -> PR
2. C++23, camelCase methods, `m_` member prefix
3. Conventional commits (`feat:`, `fix:`, `docs:`, etc.)
4. Tests for core logic

---

## Roadmap

- [ ] PXE boot server for network imaging
- [ ] WinPE bootable environment creation
- [ ] Cloud backup integration (Azure, AWS S3, Google Cloud)
- [ ] Active Directory user migration
- [ ] Group Policy backup/restore
- [ ] Driver backup/restore
- [ ] Registry comparison and merge
- [ ] Multi-language support (i18n)

---

## License

This project is licensed under the **MIT License** — see [LICENSE](LICENSE) for the full text.

Third-party dependency licenses are documented in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

---

## Acknowledgments

- [**Qt**](https://www.qt.io/) — Cross-platform UI framework (LGPL v3)
- [**Chocolatey**](https://chocolatey.org/) — Windows package manager (Apache 2.0)
- [**zlib**](https://www.zlib.net/) — Compression library (zlib License)
- [**bzip2**](https://sourceware.org/bzip2/) — Compression library (BSD-style)
- [**XZ Utils**](https://tukaani.org/xz/) — LZMA compression (0BSD / Public Domain)
- [**vcpkg**](https://vcpkg.io/) — C++ dependency manager (MIT)
- [**CMake**](https://cmake.org/) — Build system (BSD 3-Clause)
- **Microsoft** — Windows BCrypt API, PowerShell, Windows SDK

---

## Author

**Randy Northrup**
[GitHub](https://github.com/RandyNorthrup) · [S.A.K.-Utility](https://github.com/RandyNorthrup/S.A.K.-Utility)

---

## Support

- [Issues](https://github.com/RandyNorthrup/S.A.K.-Utility/issues)
- [Discussions](https://github.com/RandyNorthrup/S.A.K.-Utility/discussions)
- [Changelog](VERSION_MANAGEMENT.md)

# S.A.K. Utility

<div align="center">

**Swiss Army Knife Utility** — A portable Windows toolkit for PC technicians, IT pros, and sysadmins.

[![License: AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://isocpp.org/)
[![Qt 6.5+](https://img.shields.io/badge/Qt-6.5%2B-41cd52.svg)](https://www.qt.io/)
[![Windows 10/11](https://img.shields.io/badge/Windows-10%20%7C%2011-0078d4.svg)](https://www.microsoft.com/windows)
[![Build](https://github.com/RandyNorthrup/S.A.K.-Utility/actions/workflows/build-release.yml/badge.svg)](https://github.com/RandyNorthrup/S.A.K.-Utility/actions)
[![Version](https://img.shields.io/badge/Version-0.7.0-orange.svg)](VERSION)
[![TigerStyle](https://img.shields.io/badge/code%20style-TigerStyle-f80.svg)](docs/TIGERSTYLE_COMPLIANCE_PLAN.md)

Migration · Maintenance · Recovery · Imaging · Deployment — one portable EXE.

</div>

---

## What's New in v0.7.0

- **Advanced Search panel** — Enterprise-grade grep-style file content search with a three-panel interface (file explorer, results tree, preview pane). Supports text, regex, image metadata (EXIF/GPS), file metadata (PDF/Office/audio/video), archive content (ZIP/EPUB with deflate decompression), and binary/hex search modes. Includes a regex pattern library with 8 built-in patterns and custom user-defined patterns with JSON persistence.
- **Modernized About panel** — Rebuilt as a multi-tab view (About, License, Credits, System) with styled HTML, full dependency attribution (12 third-party components), and live runtime info. Removed dead `AboutDialog` class.
- **Unified UI polish** — Glass-effect 3-stop rgba button gradients, transparent widget backgrounds eliminating patchy gray/white artifacts, harmonized margins and border-radii via style tokens, centered quick-action labels, consistent UUP progress bar height.
- **TigerStyle Phase 9 compliance** — Lint errors reduced from 52 → 0 across the codebase; 0 compiler warnings in Release builds.
- **69 automated tests** — Comprehensive test suite covering all major subsystems including Advanced Search (types, regex library, worker, controller).

### v0.6.3

- **Code cleanup** — Removed legacy `BackupWorker`, `BackupWizard`, and `RestoreWizard` classes that had been superseded by the user-profile wizard suite.
- **Renamed panels for clarity** — `BackupPanel` → `UserMigrationPanel` (`user_migration_panel.h/.cpp`); `WifiQrPanel` → `WifiManagerPanel` (`wifi_manager_panel.h/.cpp`). All internal member names and CMake source lists updated.
- **Dead code pruned** — Removed unused member variables and stale includes from `browser_profile_backup_action.h` left over from a previous refactor.
- **Accurate tech-stack docs** — `THIRD_PARTY_LICENSES.md` now includes the bundled `qrcodegen` library (MIT, Project Nayuki) and corrects the Qt module list. Backup Known Networks action added, WiFi Manager feature section added.

### v0.6.2

- **Fixed UUP-to-ISO conversion** — Root cause: `convert-UUP.cmd` re-launches itself through PowerShell to disable QuickEdit mode, which detaches from the tracked process and causes the app to report instant success with no ISO output. Fix passes `-qedit -elevated` flags to prevent both the QuickEdit re-launch and UAC self-elevation from orphaning the process. Also corrected ConvertConfig.ini option names, added admin-privilege check, closes stdin to prevent hanging, and sets `AutoExit=1`.
- **License compliance** — Added complete license notices for all bundled tools (aria2, wimlib, 7-Zip, uup-converter-wimlib, ManagedDism, Microsoft ADK utilities) to THIRD_PARTY_LICENSES.md and README acknowledgments.
- **New integration tests** — Verifying the converter process stays attached, reads config correctly, handles missing files, and exits cleanly on closed stdin.

### v0.6.1

- **Fixed Windows ISO downloads** — Microsoft's UUP CDN serves HTTP-only URLs; the strict HTTPS-only validation was rejecting every download file. HTTP is now allowed for `*.microsoft.com` origins (files are SHA-1 integrity-verified).
- **Fixed Linux ISO downloads** — Added proper User-Agent headers to aria2c and HTTP requests to resolve 403 errors from SourceForge/CDN mirrors. Updated SystemRescue and Clonezilla download URLs.
- **Bundled aria2c** — aria2c 1.37.0 is now included in local builds and CI so ISO downloads work out of the box.
- **Expanded test suite** — Tests covering network transfer, orchestration, diagnostics, security, and ISO download pipelines.

---

## Highlights

| | |
|---|---|
| **100 % Portable** | No installer. Drop on a USB stick and go. |
| **Quick Actions** | One-click system optimization, backups, maintenance, troubleshooting, and recovery. |
| **User Profile Backup & Restore** | Step-by-step wizards with smart filtering, AES-256 encryption, and NTFS permission handling. |
| **Application Migration** | Scan installed apps, match to Chocolatey packages, bulk-install on a new PC. |
| **Diagnostics & Benchmarking** | SMART disk health, CPU/disk/memory benchmarks, stress testing, thermal monitoring, HTML/JSON/CSV reports. |
| **Network Transfer** | Peer-to-peer LAN migration with AES-256-GCM, resume, and multi-PC orchestrator mode. |
| **Image Flasher** | Flash ISOs/IMGs to USB. Download Windows and Linux ISOs directly. |
| **Advanced Search** | Grep-style file content search with regex, metadata, archive, and binary/hex modes across directory trees. |
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
  - [Diagnostics & Benchmarking](#diagnostics--benchmarking)
  - [Image Flasher](#image-flasher)
  - [Directory Organizer](#directory-organizer)
  - [Duplicate Finder](#duplicate-finder)
  - [WiFi Manager](#wifi-manager)
  - [Advanced Search](#advanced-search)
  - [Settings](#settings)
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

> **Code Signed:** Releases are digitally signed via [Azure Trusted Signing](https://learn.microsoft.com/en-us/azure/trusted-signing/).
> Windows SmartScreen and Defender should recognize the signature automatically.
> Right-click `sak_utility.exe` → Properties → Digital Signatures to verify.

---

## Features

### Quick Actions

One-click operations organized into five categories with real-time progress and detailed logging.

<details>
<summary><strong>System Optimization</strong></summary>

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
<summary><strong>Quick Backups</strong></summary>

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
| Backup Known Networks | Saved WiFi profiles exported via `netsh wlan export` |
| Backup Desktop Wallpaper | Saves current wallpaper and theme files |
| Backup Printer Settings | Printer drivers, queues, and port configurations |

</details>

<details>
<summary><strong>Maintenance</strong></summary>

| Action | Description |
|---|---|
| Update All Apps | Runs WinGet + Chocolatey updates with reporting |
| Windows Update | Triggers via UsoClient |
| Verify System Files | `sfc /scannow` + `DISM /RestoreHealth` |
| Check Disk Errors | `chkdsk` with bad-sector scan |
| Rebuild Icon Cache | Deletes `IconCache.db`, restarts Explorer |
| Reset Network | Flushes DNS, resets TCP/IP, Winsock, firewall, adapters |
| Clear Print Spooler | Stops spooler, deletes stuck jobs, restarts |

</details>

<details>
<summary><strong>Troubleshooting</strong></summary>

| Action | Description |
|---|---|
| Generate System Report | Comprehensive report: OS, hardware, storage, network, drivers, event logs, installed programs (HTML) |
| Check for Bloatware | Scans for known bloatware and OEM software |
| Test Network Speed | Download/upload speed and latency |
| Scan for Malware | Windows Defender quick scan via `Start-MpScan` |
| Repair Windows Store | `WSReset.exe` + re-register Store apps |
| Fix Audio Issues | Restarts Windows Audio service, rebuilds device enumeration |

</details>

<details>
<summary><strong>Emergency Recovery</strong></summary>

| Action | Description |
|---|---|
| Create Restore Point | System Restore checkpoint via WMI |
| Export Registry Keys | Critical registry hives (HKLM\Software, HKCU, etc.) |
| Screenshot Settings | Captures screenshots of Windows Settings panels |
| Backup BitLocker Keys | Recovery keys for all encrypted volumes (restricted permissions) |

</details>

---

### User Profile Backup & Restore

Guided wizards for comprehensive user-profile migration with smart filtering, AES-256 encryption, and NTFS permission handling.

**Backup Wizard** — Scan and select user profiles, customize per-user data categories (Desktop, Documents, AppData, browsers, email, and more), configure filters, compression, and encryption, then execute with real-time progress and cancellation support.

**Restore Wizard** — Map source users to destination users (auto-map or manual), configure merge behavior, select data categories, handle NTFS permissions, and restore with detailed logging.

---

### Application Migration

Scan installed apps, match them to Chocolatey packages, and bulk-install on a new PC.

1. **Scan** — Queries HKLM/HKCU Uninstall registry keys; extracts name, version, publisher.
2. **Match** — `PackageMatcher` with curated mappings (high/medium/low/manual confidence).
3. **Backup** — Optional data backup via `UserProfileBackupWizard` (browser profiles, IDE settings, etc.).
4. **Export** — JSON migration report portable to the target machine.
5. **Install** — Embedded Chocolatey with retry logic and exponential backoff.
6. **Restore** — `UserProfileRestoreWizard` maps source paths to target paths and restores data.

---

### Network Transfer

Secure peer-to-peer LAN transfer with three modes.

| Mode | Description |
|---|---|
| **Source** | Scan users, discover peers via UDP broadcast, connect, send encrypted data |
| **Destination** | Listen for incoming transfers, approve/reject, receive + restore |
| **Orchestrator** | Centralized multi-PC deployment — mapping strategies, concurrency, job queues |

**Security:** AES-256-GCM encryption per chunk, PBKDF2 key derivation, challenge/response authentication, SHA-256 integrity verification per file.

**Resume:** Periodic checkpointing, partial-file tracking, integrity validation before resume.

---

### Diagnostics & Benchmarking

Comprehensive hardware diagnostics, performance benchmarking, and stability testing with bundled [smartmontools](https://www.smartmontools.org/) for SMART analysis.

**Hardware Inventory**
- CPU: model, cores, threads, base/max clock
- Memory: total capacity, slot usage
- Storage: model, capacity, interface (SATA/NVMe/USB), media type
- GPU: model, VRAM, driver version
- OS: name, build number

**SMART Disk Health**
- Queries all physical drives via bundled `smartctl.exe` (smartmontools 7.4)
- Health status (Healthy / Warning / Critical / Failed)
- Temperature, power-on hours, raw attributes
- Attribute-level detail with threshold monitoring

**Benchmarks**

| Benchmark | Metrics |
|---|---|
| **CPU** | Single-thread and multi-thread scores, matrix multiply, compression throughput, prime computation |
| **Disk** | Sequential read/write, random 4K IOPS (read and write), queue-depth scoring |
| **Memory** | Read/write bandwidth, random-access latency |

**Stress Testing**
- Configurable duration
- CPU stress (all-core compute + floating-point)
- Memory stress (pattern write/verify for ECC error detection)
- Disk stress (sustained sequential I/O with direct writes)
- Real-time thermal monitoring with auto-abort at configurable limit
- Error-count abort thresholds

**Full Suite Mode**
- One-click sequential run: Hardware Scan → SMART Analysis → CPU Benchmark → Disk Benchmark → Memory Benchmark → Stress Test → Report Generation
- Step-by-step progress with live status

**Report Export**
- **HTML** — Styled report with hardware summary, SMART health, benchmark scores, and stress test results
- **JSON** — Machine-readable structured data for automation
- **CSV** — RFC 4180 compliant, importable into Excel or data pipelines

---

### Image Flasher

Create bootable USB drives from disk images.

- **Formats:** ISO, IMG, WIC, ZIP, GZ, BZ2, XZ, DMG, DSK
- **Windows ISO download** directly from Microsoft via UUP Dump API (multiple editions, languages, x64/ARM64)
- **Linux ISO download** with built-in distro catalog:
  - Ubuntu Desktop, Ubuntu Server, Linux Mint Cinnamon
  - Kali Linux
  - SystemRescue, Clonezilla, GParted Live, ShredOS
  - Ventoy (multi-boot USB creator)
- **Windows USB Creator** — NTFS format, ISO extraction, `bootsect.exe` boot sector, UEFI structure, verification of critical files
- **Generic writer** — Raw sector I/O with streaming decompression (bootable for Linux and other ISOs)
- **Safety:** System-drive protection, multi-select for parallel flash

---

### Directory Organizer

Organize files by extension into categorized subdirectories.

- Default categories: Images, Documents, Audio, Video, Archives, Code
- Custom categories with user-defined extensions
- Collision handling: rename, skip, or overwrite
- Preview mode — see what would happen without moving anything

---

### Duplicate Finder

MD5 hash-based duplicate detection with minimum-size filtering.

- Multi-directory recursive scan
- Minimum-size filter to skip small files
- Summary: duplicate count, wasted space

---

### WiFi Manager

Generate and manage WiFi network QR codes and network configuration scripts.

- Enter one or more networks (SSID, password, security type, hidden flag)
- **QR code generation** via bundled qrcodegen library — scannable WIFI: URI payload
- **Bulk network table** — save/load multiple networks as JSON
- **Export** — Windows netsh `.cmd` script (per network or bulk), macOS `.mobileconfig` plist
- **Scan networks** — detect nearby SSIDs and pre-fill the form
- **Connect with phone** — display full-screen QR for easy mobile scanning
- High error correction level for reliable scanning even with partial occlusion

---

### Advanced Search

Enterprise-grade grep-style file content search with a three-panel interface: file explorer, results tree, and preview pane.

- **Text content search** — Line-by-line search with configurable context window (0–10 lines before/after)
- **Regex support** — Full QRegularExpression engine with case-sensitive, whole-word, and multiline options
- **Regex Pattern Library** — 8 built-in patterns (emails, URLs, IPv4, phone numbers, dates, hex, numbers, words) plus custom user-defined patterns with JSON persistence
- **Binary/hex search** — Search binary files for text or hex patterns with hex-context display
- **Image metadata search** — Search EXIF/GPS metadata in image files
- **File metadata search** — Search metadata in PDF, Office, audio/video files
- **Archive search** — Search inside ZIP/EPUB archives with deflate decompression (handles real-world .docx, .xlsx, .pptx, .odt, .epub)
- **File explorer** — Drive and directory navigation with lazy-loading tree, context menus
- **Results tree** — Grouped by file with 8 sort modes (path, match count, file size, date modified)
- **Preview pane** — Monospace file preview with yellow/orange match highlighting, previous/next navigation with wrapping, line numbers with `>>>` match indicators
- **Search history** — Last 50 searches persisted via ConfigManager

---

### Settings

Global application settings accessible from the **Edit → Settings** menu (`Ctrl+,`):

| Tab | Options |
|---|---|
| **General** | Theme, startup behavior, logging preferences |
| **Backup** | Default backup location, Quick Actions backup settings (location, confirmations, notifications, logging, compression) |
| **Duplicate Finder** | Default minimum file size, recursive scan defaults |
| **Advanced** | Network Transfer toggle, experimental features |

---

## Security

| Layer | Implementation |
|---|---|
| **File encryption** | AES-256-CBC via Windows BCrypt with PBKDF2 key derivation |
| **Network encryption** | AES-256-GCM via Windows BCrypt with PBKDF2 key derivation, per-chunk authentication tags |
| **Secure memory** | `SecureString` / `SecureBuffer` — zero-fill on destruction, page locking via `VirtualLock` |
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
| Qt | 6.5+ MSVC 2019 64-bit |
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

# Bundle smartmontools (required for SMART diagnostics)
powershell -ExecutionPolicy Bypass -File scripts/bundle_smartmontools.ps1

# Run
.\build\Release\Release\sak_utility.exe
```

### Code Signing (Optional)

Local builds can be signed via Azure Trusted Signing:

```powershell
# One-time: log in to Azure
az login

# Sign after building
powershell -ExecutionPolicy Bypass -File scripts/sign-exe.ps1

# Or enable automatic signing on every build:
cmake -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.5.3/msvc2019_64" `
  -DSAK_CODE_SIGN=ON
cmake --build build --config Release
```

Requires Azure CLI and access to the Azure Trusted Signing account. CI builds (GitHub Actions) are signed automatically.

### Dependencies

| Library | License | Purpose |
|---|---|---|
| [Qt 6.5+](https://www.qt.io/) | LGPL v3 | UI framework (Core, Widgets, Concurrent, Network; Gui linked transitively via Widgets) |
| [zlib](https://www.zlib.net/) | zlib License | Compression and ZIP archive decompression (deflate) |
| [bzip2](https://sourceware.org/bzip2/) | BSD-style | bzip2 compression |
| [liblzma](https://tukaani.org/xz/) | 0BSD / Public Domain | xz/LZMA compression |
| [qrcodegen](https://www.nayuki.io/page/qr-code-generator-library) | MIT | QR code generation (bundled source) |
| [smartmontools](https://www.smartmontools.org/) | GPLv2 | SMART disk health analysis (bundled `smartctl.exe`) |
| [Chocolatey](https://chocolatey.org/) | Apache 2.0 | Embedded package manager |
| Windows BCrypt | OS component | AES-256, PBKDF2, SHA-256 |

Full license texts: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)

### Testing

```powershell
cmake --build build --config Release --target RUN_TESTS
```

Unit and integration tests covering Advanced Search, network transfer, orchestration, diagnostics, security, encryption, configuration, ISO download, and quick action factory validation.

---

## Configuration

Settings are stored at `%APPDATA%\SAK\Utility\` in INI format.

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

This project is licensed under the **GNU Affero General Public License v3.0 (AGPL-3.0)** — see [LICENSE](LICENSE) for the full text.

Third-party dependency licenses are documented in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

---

## Acknowledgments

- [**Qt**](https://www.qt.io/) — Cross-platform UI framework (LGPL v3)
- [**qrcodegen**](https://www.nayuki.io/page/qr-code-generator-library) — QR code generator by Project Nayuki (MIT)
- [**aria2**](https://aria2.github.io/) — Multi-connection download manager (GPLv2)
- [**wimlib**](https://wimlib.net/) — WIM image library by Eric Biggers (LGPL v3)
- [**7-Zip**](https://www.7-zip.org/) — Archive tool by Igor Pavlov (LGPL v2.1)
- [**uup-converter-wimlib**](https://github.com/abbodi1406) — UUP-to-ISO converter by abbodi1406
- [**smartmontools**](https://www.smartmontools.org/) — SMART disk diagnostics (GPLv2)
- [**Chocolatey**](https://chocolatey.org/) — Windows package manager (Apache 2.0)
- [**zlib**](https://www.zlib.net/) — Compression library (zlib License)
- [**bzip2**](https://sourceware.org/bzip2/) — Compression library (BSD-style)
- [**XZ Utils**](https://tukaani.org/xz/) — LZMA compression (0BSD / Public Domain)
- [**vcpkg**](https://vcpkg.io/) — C++ dependency manager (MIT)
- [**CMake**](https://cmake.org/) — Build system (BSD 3-Clause)
- **Microsoft** — Windows BCrypt API, PowerShell, Windows SDK, ADK tools (cdimage, imagex, bcdedit, cabarc)

---

## Author

**Randy Northrup**
[GitHub](https://github.com/RandyNorthrup) · [S.A.K.-Utility](https://github.com/RandyNorthrup/S.A.K.-Utility)

---

## Support

- [Issues](https://github.com/RandyNorthrup/S.A.K.-Utility/issues)
- [Discussions](https://github.com/RandyNorthrup/S.A.K.-Utility/discussions)
- [Releases](https://github.com/RandyNorthrup/S.A.K.-Utility/releases)

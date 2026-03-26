# S.A.K. Utility

<div align="center">

**Swiss Army Knife Utility** — A portable Windows toolkit for PC technicians, IT pros, and sysadmins.

[![License: AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://isocpp.org/)
[![Qt 6.5+](https://img.shields.io/badge/Qt-6.5%2B-41cd52.svg)](https://www.qt.io/)
[![Windows 10/11](https://img.shields.io/badge/Windows-10%20%7C%2011-0078d4.svg)](https://www.microsoft.com/windows)
[![Build](https://github.com/RandyNorthrup/S.A.K.-Utility/actions/workflows/build-release.yml/badge.svg)](https://github.com/RandyNorthrup/S.A.K.-Utility/actions)
[![Version](https://img.shields.io/badge/Version-0.9.0.8-orange.svg)](VERSION)

Migration · Maintenance · Recovery · Imaging · Deployment — one portable EXE.

</div>

---

## Changelog

See [CHANGELOG.md](CHANGELOG.md) for the full version history.

**Latest: v0.9.0.8** — Offline deployment Direct Download mode with embedded-binary package support, install script parser improvements, meta-package dependency resolution, documentation audit.

---

## Highlights

| | |
|---|---|
| **100 % Portable** | No installer. Drop on a USB stick and go. |
| **Backup and Restore** | Step-by-step wizards with smart filtering, AES-256 encryption, NTFS permission handling, plus integrated screenshot settings and BitLocker key backup. |
| **Diagnostics & Benchmarking** | SMART disk health, CPU/disk/memory benchmarks, stress testing, thermal monitoring, system maintenance tools, HTML/JSON/CSV reports. |
| **Image Flasher** | Flash ISOs/IMGs to USB. Download Windows and Linux ISOs directly. |
| **File Management** | Organize files by extension, find duplicates with parallel hashing, and grep-style content search with regex, metadata, archive, and binary/hex modes. |
| **Application Management** | Scan installed apps, match to Chocolatey packages, bulk-install on a new PC. Offline deployment with direct installer downloads. Deep application removal with leftover scanning and registry snapshot diffs. |
| **Network Management** | Diagnostic suite (ping, traceroute, MTR, DNS, port scan, bandwidth, WiFi, connections, firewall, shares), adapter inspector with ethernet backup/restore and network reset, WiFi QR code manager. |
| **Email Tool** | Browse PST, OST, and MBOX email archives. Search, export (EML/CSV/VCF/ICS), contacts, calendar (month/week/day), attachments browser — no Outlook required. |
| **Modern UI** | Windows 11-style rounded corners, custom splash screen, and responsive layouts. |

---

## Table of Contents

- [System Requirements](#system-requirements)
- [Quick Start](#quick-start)
- [Features](#features)
  - [Backup and Restore](#backup-and-restore)
  - [Application Management](#application-management)
  - [Benchmark and Diagnostics](#benchmark-and-diagnostics)
  - [Image Flasher](#image-flasher)
  - [File Management](#file-management)
  - [Network Management](#network-management)
  - [Email Tool](#email-tool)
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

### Backup and Restore

Guided wizards for comprehensive user-profile migration with smart filtering, AES-256 encryption, and NTFS permission handling.

**Backup Wizard** — Scan and select user profiles, customize per-user data categories (Desktop, Documents, AppData, browsers, email, and more), configure filters, compression, and encryption, then execute with real-time progress and cancellation support.

**Restore Wizard** — Map source users to destination users (auto-map or manual), configure merge behavior, select data categories, handle NTFS permissions, and restore with detailed logging.

**Quick Tools** — Integrated one-click actions:

| Tool | Description |
|---|---|
| Screenshot Settings | Captures screenshots of Windows Settings panels for documentation |
| BitLocker Key Backup | Exports BitLocker recovery keys for all encrypted volumes |

---

### Application Management

Two subtabs — **App Installation** and **Advanced Uninstall** — under one panel.

#### App Installation

Scan installed apps, match them to Chocolatey packages, and bulk-install on a new PC.

1. **Scan** — Queries HKLM/HKCU Uninstall registry keys; extracts name, version, publisher.
2. **Match** — `PackageMatcher` with curated mappings (high/medium/low/manual confidence).
3. **Backup** — Optional data backup via `UserProfileBackupWizard` (browser profiles, IDE settings, etc.).
4. **Export** — JSON migration report portable to the target machine.
5. **Install** — Embedded Chocolatey with retry logic and exponential backoff.
6. **Restore** — `UserProfileRestoreWizard` maps source paths to target paths and restores data.

**Offline Deploy** — Build deployment bundles for air-gapped or bandwidth-limited environments:

| Mode | Description |
|---|---|
| **Build Bundle** | Downloads .nupkg packages with internalized dependencies for offline Chocolatey install |
| **Direct Download** | Downloads the actual installer binaries (EXE/MSI) directly — supports URL-based, embedded-binary, and meta-package patterns |

Includes preset package lists (Office PC, Developer, Media, etc.) and curated package catalogs with search.

#### Advanced Uninstall

Deep application removal with leftover scanning, cleanup, and system protection.

**Program Enumeration**
- Win32 apps (HKLM, HKLM\WOW6432Node, HKCU Uninstall registry keys)
- UWP / Microsoft Store apps
- Windows provisioned apps
- View filters: All, Win32 Only, UWP Only, Bloatware Only, Orphaned Only

**Uninstall Modes**

| Mode | Description |
|---|---|
| **Standard** | Runs the native uninstaller with optional auto-cleanup of detected leftovers |
| **Forced** | Bypasses the native uninstaller — removes registry entries and leftover artifacts directly |
| **Batch** | Queue multiple programs and process sequentially with full logging |

**Leftover Scanner**
- Three depth levels: **Safe** (common paths/registry), **Moderate** (broader pattern matching), **Advanced** (deep scan with registry diff)
- Detects: files, folders, registry keys/values, services, scheduled tasks, firewall rules, startup entries, shell extensions
- Risk-level color coding: Safe (green), Review (yellow), Risky (red)
- Registry Snapshot Engine — captures pre/post uninstall diffs to detect leftover changes

**Cleanup & Deletion**
- Select All, Select Safe Only, Deselect All, or manual per-item selection
- Recycle Bin support — files routed via `SHFileOperationW` with `FOF_ALLOWUNDO` (registry and service entries are always permanent)
- Locked file handling — files that cannot be removed are automatically scheduled for deletion on reboot via `MoveFileExW` with `MOVEFILE_DELAY_UNTIL_REBOOT`
- User notification of reboot-pending items

**Uninstall Settings**

| Group | Options |
|---|---|
| **Leftover Selection** | Select all leftovers by default vs. safe only |
| **Deletion Behavior** | Send deleted files to Recycle Bin (toggle) |
| **System Protection** | Auto-create restore point before uninstall |
| **Default Scan Level** | Safe / Moderate / Advanced |
| **Display** | Show system components in the program list |

**Context Menu** — Uninstall, Forced Uninstall, Add to Queue, Open Install Location, Copy Program Name, Copy Uninstall Command, Show Properties, Remove Registry Entry

---

### Benchmark and Diagnostics

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

**System Maintenance** — Integrated one-click actions:

| Tool | Description |
|---|---|
| Optimize Power Settings | Applies High Performance / Ultimate Performance plan |
| Verify System Files | `sfc /scannow` + `DISM /RestoreHealth` |
| Check Disk Errors | `chkdsk` with bad-sector scan |
| Generate System Report | Comprehensive report: OS, hardware, storage, network, drivers, event logs, installed programs (HTML) |

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

### File Management

Three subtabs — **File Organizer**, **Duplicate Finder**, and **Advanced Search** — under one panel.

#### File Organizer

Organize files by extension into configurable categories.

- Default categories: Images, Documents, Audio, Video, Archives, Code — fully customizable with user-defined extensions
- Collision handling: rename, skip, or overwrite
- Preview mode — see what would happen without moving anything
- Confirmation dialog before destructive operations (shows file count and collision strategy)
- Category validation — detects empty mappings and duplicate category names
- Reset to Defaults — one-click restore of built-in category mappings

#### Duplicate Finder

Detect duplicate files via content-based hashing.

- MD5 hash-based duplicate detection with configurable minimum-size filtering
- Multi-directory recursive scan with duplicate directory prevention
- Parallel hashing with configurable thread count (auto-detects ideal thread count)
- Summary: duplicate count, wasted space, scrollable results for large scans

**Cross-Operation Safety**
- Organizer and duplicate finder share mutual locking — running one disables the other to prevent conflicts
- Cancel support for both operations

#### Advanced Search

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

### Network Management

Three-tab network panel covering diagnostics, adapter configuration, and WiFi credential management.

#### Network Diagnostics

10-tool diagnostic suite with report generation.

| Tool | Capabilities |
|---|---|
| **Ping** | Configurable count, timeout, interval, packet size; per-reply stats with min/max/avg/jitter aggregation |
| **Traceroute** | Max hops, hostname resolution; per-hop RTT table |
| **MTR** | Continuous ping + traceroute with cycle-by-cycle stats: loss %, avg/best/worst/jitter per hop |
| **DNS Lookup** | Query by record type, custom DNS server, reverse lookup, multi-server comparison, flush DNS cache |
| **Port Scanner** | Preset port lists + custom ranges, concurrent scanning, banner grabbing, timeout control |
| **Bandwidth** | iPerf3 client + server mode; HTTP speed test; bidirectional and multi-stream testing |
| **WiFi Analyzer** | Scan nearby networks with signal/channel/security; continuous scanning; channel utilization analysis |
| **Active Connections** | TCP/UDP connection monitor with process info, auto-refresh, process/port filters |
| **Firewall Auditor** | Enumerate all Windows Firewall rules; detect conflicts and coverage gaps; filter by direction/action |
| **Network Share Browser** | Discover SMB shares on a host; test access permissions |

**Report Generation**
- **HTML** — Styled report with all cached diagnostic results and adapter info
- **JSON** — Machine-readable structured data
- Technician name, ticket number, and notes metadata fields

#### Network Adapters

Adapter inspector with ethernet backup, restore, and network reset.

- Enumerates all network adapters (Ethernet, WiFi, Virtual)
- Full configuration display: IPv4/IPv6 addresses, subnet masks, gateways, DHCP status, DNS servers, MAC address, driver info, link speed, traffic statistics
- Copy adapter configuration to clipboard
- Capture adapter IP/DNS/gateway settings to a portable JSON file
- Restore settings to the same or a different adapter on any machine via `netsh` commands
- Supports DHCP and static configurations, primary and secondary DNS servers
- Cross-machine portability — back up on one PC, restore on another
- **Reset Network Settings** — One-click reset: flushes DNS, resets TCP/IP stack, Winsock, firewall, and adapters

#### WiFi Manager

WiFi credential manager with QR code generator and cross-platform network profile export.

**Network Details** — Enter Location label, SSID, Password (show/hide toggle), Security type (WPA/WPA2/WPA3, WEP, Open), and Hidden network flag.

**Saved Networks Table** — Store multiple WiFi configs with search/filter, inline editing, and tri-state select-all. Save/load entire table to/from JSON.

**QR Code Generation**
- Single-network QR for immediate phone/tablet connection
- Batch QR export wizard (PNG/PDF/JPG/BMP) with optional location header banner
- WiFi QR payload format with HIGH error correction (30%)

**Script & Profile Export**
- Generate Windows `.cmd` batch scripts using `netsh` (single or batch)
- Add selected networks directly to Windows known WiFi profiles via netsh WLAN profile XML
- Generate macOS `.mobileconfig` plist files (single or multiple networks per profile)

**Windows Integration** — Scan existing Windows known profiles and import them into the table for backup/management.

---

### Email Tool

Browse, search, and export data from Outlook PST/OST archives and MBOX mailboxes — no Outlook or MAPI libraries required.

**Supported Formats**
- **PST** — Outlook Personal Storage (Unicode and ANSI)
- **OST** — Outlook Offline Storage
- **MBOX** — RFC 4155 (Thunderbird, Apple Mail, Linux mail clients)

**File Scanner** — Automatically discovers PST/OST/MBOX files in common locations (user home, desktop, recent paths); select which to open.

**Folder Tree** — Navigable hierarchy with typed icons (Inbox, Sent Items, Drafts, Deleted Items, Junk Email, Calendar, Contacts, etc.)

**Item List** — Sortable table with Subject, From, Date, Size, Type, and attachment indicator.

**Preview Pane** — Four tabs:

| Tab | Content |
|---|---|
| **Content** | HTML/plain-text email body; contact details; task descriptions; sticky note text |
| **Headers** | RFC 5322 message headers (monospace) |
| **Properties** | MAPI property names and values for forensics/analysis |
| **Attachments** | File list with individual or batch save |

**Search** — Full-text search across subjects, bodies, senders, recipients, and attachment names. Filter by item type, date range, has-attachment, and folder scope.

**Contacts Dialog** — Searchable address book with sortable columns (name, email, company, phone) and export to VCF or CSV.

**Calendar Dialog** — Three view modes (month, week, day) with event highlighting, half-hour grid lines, and column separators. Navigate by date, click events to view details, and export to ICS or CSV.

**Attachments Browser** — Scans all emails in the mail file and presents every attachment in a searchable, filterable list. Type filter (images, documents, archives, etc.), filename search, and right-click context menu with Save Attachment, View Containing Email (navigates to the source message), and Copy Filename.

**Export Formats**

| Format | Use Case |
|---|---|
| **EML** | RFC 5322 email files (Outlook, Thunderbird compatible) |
| **CSV** | Emails, contacts, calendar, or tasks as spreadsheet data |
| **VCF** | vCard 3.0 contact files |
| **ICS** | iCalendar appointments/events |
| **TXT** | Plain-text sticky notes |
| **Attachments** | Batch extract with optional filtering and inline-image skip |

---

### Settings

Global application settings accessible from the **Edit → Settings** menu (`Ctrl+,`):

| Tab | Options |
|---|---|
| **Backup** | Default backup location, confirmation prompts, notification preferences, logging, compression |

---

## Security

| Layer | Implementation |
|---|---|
| **File encryption** | AES-256-CBC via Windows BCrypt with PBKDF2 key derivation |
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

75 unit and integration tests across 99 test files covering Advanced Search, Advanced Uninstall (types, controller, leftover scanner, registry snapshot engine), Network Diagnostics (types, utils, report generation), Email Inspector (PST/OST parsing, MBOX parsing, email types, search, export, profile manager, report generator), Offline Deployment (install script parsing, NuGet API, script rewriting, package builder), diagnostics, security, encryption, configuration, ISO download, and quick action validation.

---

## Configuration

Settings are stored at `%APPDATA%\SAK\Utility\` in INI format.

### Key Settings

```ini
[QuickActions]
backup_location=C:/SAK_Backups
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

### Planned Features (detailed plans in `docs/`)

- **File Converter Tab** — Universal offline file conversion for documents, images, audio, video, spreadsheets, and PDFs with batch processing and quality controls. Adds a new tab to the File Management panel. See [FILE_CONVERTER_TAB_PLAN.md](docs/FILE_CONVERTER_TAB_PLAN.md).
- **System Tools Tab** — Centralized launcher for 48+ built-in Windows administrative and diagnostic utilities, organized into searchable categories with one-click launch, favorites, and availability detection. Adds a new tab to the Benchmark & Diagnostics panel. See [SYSTEM_TOOLS_TAB_PLAN.md](docs/SYSTEM_TOOLS_TAB_PLAN.md).
- **OST Converter Tab** — Multi-threaded bulk OST/PST conversion to PST, EML, MSG, MBOX, and DBX formats with IMAP cloud upload (Office 365, Gmail, Yahoo), deleted/corrupt item recovery, metadata preservation, and PST splitting. Adds a new tab to the Email Tool panel. See [OST_CONVERTER_TAB_PLAN.md](docs/OST_CONVERTER_TAB_PLAN.md).

---

## License

This project is licensed under the **GNU Affero General Public License v3.0 (AGPL-3.0)** — see [LICENSE](LICENSE) for the full text.

Third-party dependency licenses are documented in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

---

## Acknowledgments

- [**Qt**](https://www.qt.io/) — Cross-platform UI framework (LGPL v3)
- [**qrcodegen**](https://www.nayuki.io/page/qr-code-generator-library) — QR code generator by Project Nayuki (MIT)
- [**aria2**](https://aria2.github.io/) — Multi-connection download manager (GPLv2)
- [**UUPMediaCreator**](https://github.com/OSTooling/UUPMediaCreator) — UUP-to-ISO converter by OSTooling (MIT)
- [**wimlib / libwim**](https://wimlib.net/) — WIM image library by Eric Biggers (LGPL v3, bundled with UUPMediaConverter)
- [**smartmontools**](https://www.smartmontools.org/) — SMART disk diagnostics (GPLv2)
- [**Icons8**](https://icons8.com/) — UI icons (Icons8 Free License with attribution)
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

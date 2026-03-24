# S.A.K. Utility

<div align="center">

**Swiss Army Knife Utility** — A portable Windows toolkit for PC technicians, IT pros, and sysadmins.

[![License: AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://isocpp.org/)
[![Qt 6.5+](https://img.shields.io/badge/Qt-6.5%2B-41cd52.svg)](https://www.qt.io/)
[![Windows 10/11](https://img.shields.io/badge/Windows-10%20%7C%2011-0078d4.svg)](https://www.microsoft.com/windows)
[![Build](https://github.com/RandyNorthrup/S.A.K.-Utility/actions/workflows/build-release.yml/badge.svg)](https://github.com/RandyNorthrup/S.A.K.-Utility/actions)
[![Version](https://img.shields.io/badge/Version-0.9.0.4-orange.svg)](VERSION)

Migration · Maintenance · Recovery · Imaging · Deployment — one portable EXE.

</div>

---

## What's New in v0.9.0.4

- **Email Inspector panel** — Full-featured PST/OST/MBOX email browser with folder tree navigation, item list, HTML/plain-text preview, MAPI property inspector, and attachment viewer. File scanner auto-discovers email archives in common locations. Export to EML, CSV, VCF, ICS, and bulk attachment extraction. Contacts dialog, calendar dialog with event highlighting, and full-text search across all item fields — no Outlook or MAPI libraries required.
- **Modern email client layout** — Ribbon toolbar with Icons8 Windows 11 Filled icons, side-by-side splitter layout (folder tree | item list | preview), typed folder icons (Inbox, Sent, Drafts, Deleted, Junk, Starred), and responsive preview pane sizing.
- **PST/OST parser improvements** — Fixed 8-byte property types (PT_SYSTIME, PT_INT64, PT_FLOAT64) truncated to 4 bytes, fixed HTML body display for PT_BINARY properties, added ~50 MAPI property name mappings.
- **Documentation overhaul** — README updated with Email Inspector feature section, corrected test counts (91), modernized CONTRIBUTING.md (Qt Test examples, CTest commands, corrected naming conventions and include order), updated THIRD_PARTY_LICENSES.md with full Icons8 icon inventory.
- **Build quality** — Clean MSVC `/W4 /WX` build, 91 automated tests (all passing).

### v0.9.0.3

- **Codebase-wide assertion audit** — Removed ~170+ incorrect assertions left behind by the scrapped TigerStyle linter across 22 source files. Eliminated three dangerous patterns: assert-before-create (asserting a member exists at the top of the function that creates it), contradictory assert+guard (asserting non-null then immediately null-checking), and incorrect state assertions (asserting non-empty on legitimately empty data). 1,487 legitimate assertions verified and retained.
- **Error logging completeness** — Added ~15 missing `sak::logWarning`/`sak::logError` calls alongside `QMessageBox` displays across backup wizards, restore wizards, organizer panel, and settings dialogs.
- **Silent error elimination** — Fixed 7 unchecked `QFile::remove()` calls in critical data paths (encryption cleanup, transfer checksum failures, conflict resolution) and 1 unchecked `waitForFinished()` in service removal.
- **Build quality** — Clean MSVC `/W4 /WX` build, 91 automated tests (all passing).

### v0.9.0.2

- **Comprehensive cppcheck cleanup** — Resolved all actionable cppcheck findings across the codebase with `--enable=all --check-level=exhaustive --inconclusive`.
- **Non-ASCII encoding fixes** — Fixed encoding in ~140 source files for clean cross-platform compilation.
- **Static correctness** — Added `static` to 28 functions that don't use member state (`functionStatic`).
- **STL algorithm adoption** — Converted ~43 raw loops to STL algorithms: `std::any_of`, `std::find_if`, `std::for_each`, `std::generate`, `std::accumulate`, `std::count_if`, `std::copy_if`, `std::transform` (`useStlAlgorithm`).
- **Const correctness** — Added `const` qualifier to 6 member functions: `searchMatch`, `trySearchMatch`, `assessSataAttributeHealth`, `assessNvmeHealth`, `groupByHash`, `checkStop` (`functionConst`).
- **Dead code removal** — Removed unused `SmartThreshold::name` field (`unusedStructMember`).
- **Lizard compliance** — All functions meet CCN < 11 and parameter count < 6.
- **Build quality** — Clean MSVC `/W4 /WX` build, 91 automated tests (all passing).

### v0.9.0.1

- **ISO 9660 analyzer** — New `IsoAnalyzer` class parses ISO Primary Volume Descriptors for volume label, publisher, preparer, application, dates, and file count. Automatic Linux distro identification across 74 patterns (Ubuntu, Fedora, Arch, Manjaro, Kali, Pop!_OS, Proxmox, Clonezilla, GParted, Ventoy, FreeBSD, SteamOS, and more) with desktop environment detection. Searches volume label, publisher, preparer, and application metadata fields to maximize detection across different mastering workflows.
- **Tabbed diagnostics panel** — Diagnostic & Benchmark panel restructured into tabbed interface for better organization and navigation.
- **Network transfer mode cards** — Replaced mode combo box with visual portrait-style card selector (Source, Destination, Orchestrator) with dedicated SVG icons matching the backup/restore panel pattern.
- **Image flasher ISO info** — Unified ISO information display with size and format rows integrated into the ISO info group box. Info panel always shown when an image is selected.
- **Network settings simplified** — Removed redundant "Enabled" master switch from network transfer settings dialog.
- **Icons8 SVG icons** — Added 6 new SVG icons (benchmark, duplicate, settings help, source, destination, orchestrator) for improved visual consistency across panels.
- **Build quality** — Clean MSVC `/W4 /WX` build, 91 automated tests (all passing).

### v0.9.0

- **UUP converter rewrite** — Rewrote `UupIsoBuilder` to drive the bundled `UUPMediaConverter.exe` directly via QProcess, replacing the ConvertConfig.ini generation and batch-script pipeline introduced in v0.6.2. Removed 7-Zip dependency, retry/fallback logic, and broken cancel path. Added `classifyConverterFailure()` for structured error diagnostics and converter output analysis.
- **Code quality audit** — Assertion-density audit, function length / complexity refactoring, magic number extraction, nesting depth reduction across the codebase.
- **Updated licenses & credits** — THIRD_PARTY_LICENSES.md, README acknowledgments, and about dialog updated to reference UUPMediaCreator/OSTooling and bundled wimlib/libwim.
- **Build quality** — Clean MSVC `/W4 /WX` build, 91 automated tests (all passing).

### v0.8.8

- **Network transfer reliability fix** — Fixed `sendFrame()` using `socket->flush()` instead of `waitForBytesWritten()`, causing spurious transfer failures under TCP backpressure during unthrottled resume transfers. Added stop-request guard to sender retry loop to prevent pointless retries on cancelled transfers.
- **Error handling hardening** — Added `sak::logError()` to transfer report write failures, clarified best-effort banner probe in port scanner.
- **Magic number elimination** — Extracted `kTimeoutThreadShutdownMs`, `kTimeoutThreadTerminateMs`, `kTimeoutWorkerResetMs` for worker lifecycle, and `kFlashBufferSize`, `kVerifySampleMax`, `kVerifyBlockSize` for flash operations. All bare numeric literals replaced with named constants.
- **Build quality** — Clean MSVC `/W4 /WX` build, 91 automated tests (all passing).

### v0.8.5

- **Enterprise-grade Directory Organizer** — Merged duplicate finder into the organizer panel. Added confirmation dialogs before destructive operations (file count + collision strategy warning), category validation (empty mapping / duplicate name detection), Reset to Defaults button, cross-operation locking (disables organizer widgets during dedup and vice versa), duplicate directory prevention, and scrollable results dialogs for large output.
- **Parallel duplicate hashing** — Dedup settings now expose parallel hashing toggle and thread count spinner with auto-detected ideal thread count display.
- **Codebase security hardening** — 20+ files fixed: `findChild` null checks, hardcoded paths replaced with environment variables (`%SystemRoot%`, `%ProgramFiles(x86)%`), `QProcess::waitForStarted` timeout checks, insecure temp files migrated to `QTemporaryFile`, thread-safe `setError()` in USB creator (29 assignments migrated), `SetFilePointer` → `SetFilePointerEx` for large-disk correctness.
- **UI robustness** — Network Transfer panel re-enabled, `setFixedSize` → `setMinimumSize` across 3 dialogs for DPI scaling, centralized style constants (`style_constants.h`), tooltip event filter parent fix, dead lambda cleanup in network settings.
- **Build quality** — Clean MSVC `/W4 /WX` build, 76 automated tests (all passing).

### v0.8.1

- **Network Diagnostics UI polish** — Horizontal 4-column adapter detail layout (Identity, Addressing, Gateway/DNS, Status) replaces single stacked label for better space utilization. Tightened adapter section margins to maximize table visibility. Removed unused report generation section from the Network Diagnostics panel (reports remain in PC Diagnostics). Speed column widened, adapter detail area compacted, QSplitter layout for adapters/tools split.
- **UI refinements** — Style constants centralized (`style_constants.h`), keyboard shortcut improvements, accessible names audit, and layout fixes across multiple panels.

### v0.8.0

- **Advanced Uninstall panel** — Deep application removal with leftover scanning and cleanup. Enumerates Win32, UWP, and provisioned apps. Three uninstall modes: standard, forced (registry-only), and batch queue. Post-uninstall leftover scanner detects orphaned files, folders, registry keys, services, scheduled tasks, firewall rules, startup entries, and shell extensions across three depth levels (Safe / Moderate / Advanced). Risk-level color coding per item. Settings modal with recycle bin deletion, auto restore point, default scan level, and select-all preferences. Locked files are automatically scheduled for removal on reboot via `MoveFileExW`. Registry snapshot engine captures before/after diffs.
- **Network Diagnostics panel** — 10-tool network diagnostic suite: Ping, Traceroute, MTR, DNS Lookup, Port Scanner, Bandwidth (iPerf3 + HTTP), WiFi Analyzer, Active Connections, Firewall Auditor, and Network Share Browser. Adapter inspector with full IPv4/IPv6 config, DHCP, driver details, and traffic stats. Ethernet settings backup/restore to JSON for cross-machine portability. HTML and JSON report generation with technician metadata.
- **76 automated tests** — Full coverage including Advanced Uninstall (types, controller, leftover scanner, registry snapshot engine) and Network Diagnostics (types, utils, report generation).

### v0.7.0

- **Advanced Search panel** — Enterprise-grade grep-style file content search with a three-panel interface (file explorer, results tree, preview pane). Supports text, regex, image metadata (EXIF/GPS), file metadata (PDF/Office/audio/video), archive content (ZIP/EPUB with deflate decompression), and binary/hex search modes. Includes a regex pattern library with 8 built-in patterns and custom user-defined patterns with JSON persistence.
- **Modernized About panel** — Rebuilt as a multi-tab view (About, License, Credits, System) with styled HTML, full dependency attribution (12 third-party components), and live runtime info. Removed dead `AboutDialog` class.
- **Unified UI polish** — Glass-effect 3-stop rgba button gradients, transparent widget backgrounds eliminating patchy gray/white artifacts, harmonized margins and border-radii via style tokens, centered quick-action labels, consistent UUP progress bar height.
- **TigerStyle philosophy (ongoing)** — Code quality tooling (Lizard complexity, cppcheck) and a compliance plan are in place. Current status is tracked and addressed as part of the enterprise hardening work.

### v0.6.3

- **Code cleanup** — Removed legacy `BackupWorker`, `BackupWizard`, and `RestoreWizard` classes that had been superseded by the user-profile wizard suite.
- **Renamed panels for clarity** — `BackupPanel` → `UserMigrationPanel` (`user_migration_panel.h/.cpp`); `WifiQrPanel` → `WifiManagerPanel` (`wifi_manager_panel.h/.cpp`). All internal member names and CMake source lists updated.
- **Dead code pruned** — Removed unused member variables and stale includes from `browser_profile_backup_action.h` left over from a previous refactor.
- **Accurate tech-stack docs** — `THIRD_PARTY_LICENSES.md` now includes the bundled `qrcodegen` library (MIT, Project Nayuki) and corrects the Qt module list. Backup Known Networks action added, WiFi Manager feature section added.

### v0.6.2

- **Replaced UUP converter** — Switched from `uup-converter-wimlib` batch scripts to a patched build of [UUPMediaCreator](https://github.com/OSTooling/UUPMediaCreator) (OSTooling). AppX provisioning is fully skipped to avoid DISM servicing-stack failures on Professional editions. The converter is bundled as a self-contained .NET 8 executable.
- **License compliance** — Added complete license notices for all bundled tools (aria2, UUPMediaCreator, wimlib/libwim, 7-Zip, ManagedDism, Microsoft ADK utilities) to THIRD_PARTY_LICENSES.md and README acknowledgments.
- **New integration tests** — Verifying the converter executable is present, launches without forking, shows help output, and exits cleanly on missing UUP files.

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
| **Directory Organizer** | Organize files by extension with duplicate detection, parallel hashing, category validation, and cross-operation safety. |
| **Advanced Uninstall** | Deep application removal with leftover scanning, recycle bin support, locked-file reboot scheduling, and registry snapshot diffs. |
| **Network Diagnostics** | 10-tool diagnostic suite (ping, traceroute, MTR, DNS, port scan, bandwidth, WiFi, connections, firewall, shares) with ethernet backup/restore and report export. |
| **Email Inspector** | Browse PST, OST, and MBOX email archives. Search, export (EML/CSV/VCF/ICS), view contacts, calendar, attachments — no Outlook required. |
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
  - [Directory Organizer & Duplicate Finder](#directory-organizer--duplicate-finder)
  - [WiFi Manager](#wifi-manager)
  - [Advanced Search](#advanced-search)
  - [Advanced Uninstall](#advanced-uninstall)
  - [Network Diagnostics](#network-diagnostics)
  - [Email Inspector](#email-inspector)
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

### Directory Organizer & Duplicate Finder

Unified panel for organizing files by extension and detecting duplicates via MD5 hashing.

**File Organization**
- Default categories: Images, Documents, Audio, Video, Archives, Code — fully customizable with user-defined extensions
- Collision handling: rename, skip, or overwrite
- Preview mode — see what would happen without moving anything
- Confirmation dialog before destructive operations (shows file count and collision strategy)
- Category validation — detects empty mappings and duplicate category names
- Reset to Defaults — one-click restore of built-in category mappings

**Duplicate Detection**
- MD5 hash-based duplicate detection with configurable minimum-size filtering
- Multi-directory recursive scan with duplicate directory prevention
- Parallel hashing with configurable thread count (auto-detects ideal thread count)
- Summary: duplicate count, wasted space, scrollable results for large scans

**Cross-Operation Safety**
- Organizer and duplicate finder share mutual locking — running one disables the other to prevent conflicts
- Cancel support for both operations

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

### Advanced Uninstall

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

**Settings Modal**

| Group | Options |
|---|---|
| **Leftover Selection** | Select all leftovers by default vs. safe only |
| **Deletion Behavior** | Send deleted files to Recycle Bin (toggle) |
| **System Protection** | Auto-create restore point before uninstall |
| **Default Scan Level** | Safe / Moderate / Advanced |
| **Display** | Show system components in the program list |

**Context Menu** — Uninstall, Forced Uninstall, Add to Queue, Open Install Location, Copy Program Name, Copy Uninstall Command, Show Properties, Remove Registry Entry

---

### Network Diagnostics

10-tool network diagnostic suite with adapter inspector, ethernet backup/restore, and report generation.

**Adapter Inspector**
- Enumerates all network adapters (Ethernet, WiFi, Virtual)
- Full configuration display: IPv4/IPv6 addresses, subnet masks, gateways, DHCP status, DNS servers, MAC address, driver info, link speed, traffic statistics
- Copy adapter configuration to clipboard

**Ethernet Backup & Restore**
- Capture adapter IP/DNS/gateway settings to a portable JSON file
- Restore settings to the same or a different adapter on any machine via `netsh` commands
- Supports DHCP and static configurations, primary and secondary DNS servers
- Cross-machine portability — back up on one PC, restore on another

**Diagnostic Tools**

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

---

### Email Inspector

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

**Contacts Dialog** — Searchable address book with sortable columns and export to VCF or CSV.

**Calendar Dialog** — Calendar widget with event highlighting, date-based event list, and export to ICS or CSV.

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
| **General** | Theme, startup behavior, logging preferences |
| **Backup** | Default backup location, Quick Actions backup settings (location, confirmations, notifications, logging, compression) |
| **Organizer** | Default minimum file size for dedup, recursive scan defaults, parallel hashing |
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

91 unit and integration tests covering Advanced Search, Advanced Uninstall (types, controller, leftover scanner, registry snapshot engine), Network Diagnostics (types, utils, report generation), Email Inspector (PST/OST parsing, MBOX parsing, email types, search, export, profile manager, report generator), network transfer, orchestration, diagnostics, security, encryption, configuration, ISO download, and quick action factory validation.

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

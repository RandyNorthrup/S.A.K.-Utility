# S.A.K. Utility

<div align="center">

**Swiss Army Knife Utility** - A portable Windows toolkit for PC technicians, IT pros, and sysadmins.

[![License: AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://isocpp.org/)
[![Qt 6.5+](https://img.shields.io/badge/Qt-6.5%2B-41cd52.svg)](https://www.qt.io/)
[![Windows 10/11](https://img.shields.io/badge/Windows-10%20%7C%2011-0078d4.svg)](https://www.microsoft.com/windows)
[![Build](https://github.com/RandyNorthrup/S.A.K.-Utility/actions/workflows/build-release.yml/badge.svg)](https://github.com/RandyNorthrup/S.A.K.-Utility/actions)
[![Version](https://img.shields.io/badge/Version-0.9.1.9-orange.svg)](VERSION)
[![Tests](https://img.shields.io/badge/Tests-133%20passing-brightgreen.svg)](tests/)

Migration - Maintenance - Recovery - Imaging - Deployment - one portable toolkit.

</div>

---

## Changelog

See [CHANGELOG.md](CHANGELOG.md) for the full version history.

**Latest: v0.9.1.9** - AI Assistant production hardening release with exact context-window usage, workflow-selection role preview, task-specific technician roles, first-prompt role inference, chat title/rename/resume continuity, result-bubble copy, Enter-to-send, background-agent telemetry, live OpenAI smoke coverage, and the hardware-certified Partition Manager scope from v0.9.1.8.

---

## Highlights

| | |
|---|---|
| **Portable ZIP** | No installer. Extract the release package and run the app; bundled Qt/plugins/tools travel with it. |
| **AI Assistant** | Codex-style AI workspace for technician chat, PC actions, multi-agent workflows, context attachments, reports, and artifacts. |
| **Backup and Restore** | Step-by-step wizards with smart filtering, AES-256 encryption, NTFS permission handling, plus integrated screenshot settings and BitLocker key backup. |
| **Diagnostics & Benchmarking** | SMART disk health, CPU/disk/memory benchmarks, stress testing, thermal monitoring, system maintenance tools, HTML/JSON/CSV reports. |
| **Partition Manager** | AOMEI/MiniTool-style disk layout workspace with stateful Scan Disks/Refresh Disks inventory, resilient EFI/MSR and read-only raw-signature file-system labels, ribbon actions, fixed-width left operation pane, selection-aware action enabling, Pending Operations queue, final Apply before/after layout review, compact proportional disk map, table and disk-map right-click action menus with safety reasons, queued partition operations, safety preflights, BitLocker/dirty-bit and storage reliability data, dry-run scripts, MBR/GPT conversion, clone/image, partition recovery scan, image/raw-path Data Recovery, OS migration, boot repair, SSD optimization, wipe tools, Quick Partition, Extend Partition Wizard, adjacent-donor Allocate Free Space, unallocated Allocate Free Space To, Move partition start, primary/logical conversion, volume serial-number reformat/restore, one-volume dynamic-to-basic conversion, Explore, Space Analyzer, Disk Benchmark, in-app BitLocker status with Windows management launch, in-app defrag/ReTrim guidance with HDD-only queued defrag through cancellable Apply, SSD Secure Erase ReTrim plus clear-level wipe queueing, and bootable-media launchers. |
| **Image Flasher** | Flash ISOs/IMGs to USB. Build Windows ISOs from Microsoft UUP payloads and download Linux ISOs from the curated catalog. |
| **File Management** | Organize mounted files by extension, find duplicates across mounted or supported raw/image file-system targets, browse supported file systems in a native explorer with full driver-level certified HFS+/HFSX and APFS write actions, and run grep-style content search with regex, metadata, archive, and binary/hex modes. |
| **Application Management** | Scan installed apps, match to Chocolatey packages, bulk-install on a new PC. Offline deployment with direct installer downloads. Deep application removal and vulnerability checks across CISA KEV, NVD, GitHub Advisories, and OSV. |
| **Network Management** | Diagnostic suite (ping, traceroute, MTR, DNS, port scan, bandwidth, WiFi, connections, firewall, shares), adapter inspector with ethernet backup/restore and network reset, WiFi QR code manager. |
| **Email Tools** | Browse PST, OST, and MBOX email archives. Search, checkbox-select, export (HTML/TXT/EML/PDF/CSV/VCF/ICS), contacts, calendar (month/week/day), attachments browser - no Outlook required. Multi-threaded OST/PST converter with 8 output formats including IMAP cloud upload. |
| **Modern UI** | Windows 11-style rounded corners, light/dark themes, fixed 300x300 px custom splash screen, shared Icons8 control icons, and responsive layouts. |

---

## Table of Contents

- [System Requirements](#system-requirements)
- [Quick Start](#quick-start)
- [Features](#features)
  - [AI Assistant](#ai-assistant)
  - [Backup and Restore](#backup-and-restore)
  - [Application Management](#application-management)
  - [Benchmark and Diagnostics](#benchmark-and-diagnostics)
  - [Partition Manager](#partition-manager)
  - [Image Flasher](#image-flasher)
  - [File Management](#file-management)
  - [Network Management](#network-management)
  - [Email Tools](#email-tools)
  - [OST Converter](#ost-converter)
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
| **Privileges** | Standard user; elevates per-task via UAC when needed |

---

## Quick Start

```text
1. Download the latest ZIP from Releases.
2. Extract anywhere (USB drive, desktop, network share).
3. Run sak_utility.exe - runs as a standard user; individual features prompt for elevation when needed.
```

> **Code Signing:** Official release artifacts should be signed by the release workflow when Azure Trusted Signing credentials are available. Local and manual builds may be unsigned.
> Authenticode signature validity and SmartScreen reputation are separate; a valid signature can still show a reputation warning on new releases.
> Right-click `sak_utility.exe` ? Properties ? Digital Signatures to verify.

---

## Features

### AI Assistant

The AI Assistant panel is the first tab in S.A.K. Utility when `SAK_ENABLE_AI_ASSISTANT` is enabled. It provides a Codex-style technician workspace that can explain SAK capabilities, recommend workflows, collect evidence, run approved PC actions, and produce readable handoff reports.

**Chat Workspace**
- Modern chat layout with right-aligned prompt bubbles and left-aligned assistant results
- Assistant, workflow, tool, and system result bubbles include a copy icon that copies only that bubble's redacted contents
- Expand/collapse support for long results and tool output
- Enter submits, Ctrl+Enter inserts a new line, and Up/Down at the input start/end cycles prompt history
- Session picker with new chat and rename controls
- New Chat closes the current chat workspace immediately, clears the transcript/composer, assigns a draft `AI Session` name, and auto-renames from the first prompt or attached workflow while keeping manual rename available after the chat is saved
- Header/status integration for API key state, access mode, activity, workflow progress, background subagent counts, exact `Ctx: x/y` context usage in the composer beside Send, and token usage
- Details panel for active workflow/run state; artifacts button opens the current session artifact directory when content exists

**OpenAI Integration**
- User-supplied OpenAI API key with encrypted app-local storage under the portable `data/credentials/` directory
- Load/clear key workflow with visible loaded/not-loaded status
- Model list retrieval and selectable chat model
- OpenAI Responses API client with persisted response-chain continuity across reopened chats, usage parsing, strict function-tool schemas, and citation parsing
- Privacy-preserving `safety_identifier` values derived from local AI session IDs for model requests
- Token usage meter for input, cached input, output, reasoning, and total token counts
- Secret redaction for API keys, bearer tokens, GitHub tokens, AWS keys, Google keys, Slack tokens, Stripe keys, and common `password=`/`secret=`/`token=` assignments

**Context and Instructions**
- Attach screenshots, documents, and other files as session context
- Add Markdown instruction files as explicit instruction context
- Color-coded context and instruction chips with remove controls
- Per-session memory and transcript persistence under the portable `data/ai_sessions/` directory
- Session-specific artifact directories so logs, downloads, screenshots, and reports do not pollute other sessions

**Local Tools and PC Actions**
- `run_powershell` for Windows automation, with optional elevation through SAK's existing per-task UAC helper
- `run_cmd` for non-admin cmd.exe tasks
- `run_process` for non-admin direct executable launches with explicit arguments
- `take_screenshot` to capture the primary display into session artifacts
- `download_file` for HTTPS downloads with SHA-256 evidence
- Built-in `sak_package_manager` tool for app search, install, uninstall, upgrade, installed-version checks, and outdated-package checks before raw package-manager or vendor-web commands
- Built-in `sak_offline_downloader` tool for offline installer downloads, direct installer retrieval, offline Chocolatey bundle creation, and offline bundle installation
- Human approval prompts for risky or destructive actions, with restore-point offers when rollback may be needed
- Stop/cancel handling across model calls, local tools, workflows, and subagents

**Workflow Roles and Workflows**
- Workflow-selected technician roles so the right prompt stance previews immediately when a workflow is selected, then stays with the workflow after Add
- First-prompt role inference when no workflow is selected, with explicit user role changes such as "act as a report writer" honored for later turns
- The plain Session role text is populated from the workflow catalog so user-added workflow roles appear without editing panel code
- The assistant is aware of the workflow catalog and can describe what each workflow does, when to use it, required inputs, risk level, verification, reporting, cleanup, and expected artifacts
- Multi-agent workflow orchestration with a main overseer, specialized subagents, shared session memory, phase tracking, cancellation, recovery policy, and human handoff when needed
- Codex-inspired workflow policy keeps the main chat as overseer, runs workflow-declared read-only subagents in parallel with per-subagent OpenAI client isolation, serializes mutating phases, and summarizes subagent evidence instead of flooding the transcript with raw logs
- Workflow resources can include prompts, instruction files, skills, required software, tool phases, troubleshooting guidance, verification steps, cleanup, and reporting requirements

**Built-in Technician Workflows**

| Workflow | Purpose |
|---|---|
| Full PC Health Check | Collects system, storage, security, performance, and event-log evidence and summarizes device health |
| Drive Health Deep Check | Performs read-only drive/volume/SMART-style diagnostics with secondary Windows storage evidence when counters are unavailable |
| Windows Update Repair | Diagnoses update failures, checks services/logs/component store state, applies approved repairs, and verifies update health |
| Network Connectivity Repair | Collects adapter/DNS/route/connectivity evidence, applies safe network repairs, and verifies connectivity |
| BSOD Investigation | Collects crash, driver, event-log, and system evidence for stop-code analysis |
| Printer Troubleshooting | Checks spooler, printer queue, driver, port, and connectivity state |
| Startup Performance Triage | Reviews startup apps, services, boot symptoms, and high-impact processes |
| Security Advisory Check | Reviews security posture and relevant advisories without making destructive changes |
| Disk Space Cleanup Triage | Finds reclaimable space with read-only evidence first, then recommends safe cleanup actions |
| Browser Issue Cleanup | Diagnoses browser performance, extension, cache, profile, and policy issues before recommending cleanup |
| Driver and Device Troubleshooting | Collects device/driver evidence, checks rollback/update options, and verifies device health after approved changes |
| Laptop Battery Health | Reviews battery report, power settings, charging symptoms, and practical replacement/optimization guidance |
| User Profile and Login Repair | Diagnoses profile load, shell, registry, and sign-in issues with rollback-aware repair guidance |
| Windows Search Index Repair | Checks indexer state, catalog health, service state, and approved rebuild paths |
| Audio Device Troubleshooting | Diagnoses playback/recording devices, driver state, service health, and approved audio resets |
| Time Sync Repair | Checks time service, NTP source, skew, event logs, and approved synchronization repairs |
| Malware and Virus Removal | Runs a technician-guided malware triage/removal flow with evidence capture, approvals, verification, and cleanup |
| PC Cleanup, Bloatware, and Adware Removal | Identifies unwanted software/startup clutter, removes approved items, verifies system state, and reports changes |
| Approved Bloatware/Adware Removal | Removes a user-approved list with explicit verification and rollback-aware reporting |
| Clean Uninstall | Uses SAK uninstall capabilities and leftover review for deeper application removal |
| Install App Now | Uses SAK package management first, then verifies installation |
| Download Offline Installer | Uses SAK offline downloader first and records installer paths/checksums |
| Build Offline Deployment Bundle | Builds offline Chocolatey/deployment bundles with evidence and manifest output |
| Technician Tool-Assisted Task | Structured workflow for using downloaded or bundled tools, then cleaning up after the job |
| Technician Service Report | Converts session findings, evidence, actions, and verification into an easy-to-read customer/technician report |

**Reports and Artifacts**
- Report generation is manual and enabled only after reportable actions/results exist
- User can choose report output location and format: HTML, Markdown, or plain text
- Reports are technician handoffs: styled HTML leads with an executive summary, evidence snapshot, prioritized findings, risks/evidence gaps, and next steps; raw transcript excerpts are kept in an appendix
- Reports evaluate the session transcript, evidence, workflow results, tool outputs, findings, risks, actions taken, verification status, and remaining recommendations
- Reports include a link to the containing artifact directory when possible

---

### Backup and Restore

Guided wizards for comprehensive user-profile migration with smart filtering, AES-256 encryption, and NTFS permission handling.

**Backup Wizard** - Scan and select user profiles, customize per-user data categories (Desktop, Documents, AppData, browsers, email, and more), configure filters, compression, and encryption, then execute with real-time progress and cancellation support.

**Restore Wizard** - Map source users to destination users (auto-map or manual), configure merge behavior, select data categories, handle NTFS permissions, and restore with detailed logging.

**Quick Tools** - Integrated one-click actions:

| Tool | Description |
|---|---|
| Screenshot Settings | Captures screenshots of Windows Settings panels for documentation |
| BitLocker Key Backup | Exports BitLocker recovery keys for all encrypted volumes |

---

### Application Management

Three subtabs - **App Installation**, **Advanced Uninstall**, and **Vulnerability Scanner** - under one panel.

#### App Installation

Scan installed apps, match them to Chocolatey packages, and bulk-install on a new PC.

1. **Scan** - Queries HKLM/HKCU Uninstall registry keys; extracts name, version, publisher.
2. **Match** - `PackageMatcher` with curated mappings (high/medium/low/manual confidence).
3. **Backup** - Optional data backup via `UserProfileBackupWizard` (browser profiles, IDE settings, etc.).
4. **Export** - JSON migration report portable to the target machine.
5. **Install** - Embedded Chocolatey with retry logic and exponential backoff.
6. **Restore** - `UserProfileRestoreWizard` maps source paths to target paths and restores data.

**Offline Deploy** - Build deployment bundles for air-gapped or bandwidth-limited environments:

| Mode | Description |
|---|---|
| **Build Bundle** | Downloads .nupkg packages with internalized dependencies for offline Chocolatey install |
| **Direct Download** | Downloads the actual installer binaries (EXE/MSI) directly - supports URL-based, embedded-binary, and meta-package patterns |

Includes preset package lists (Office PC, Developer, Media, etc.) and curated package catalogs with search.

#### Vulnerability Scanner

Checks installed software and packages against current vulnerability intelligence sources.

| Source | Role |
|---|---|
| **CISA KEV** | First-pass exploited-vulnerability matching for installed software |
| **NVD** | CVE detail enrichment, CVSS severity, affected CPE/product evidence, and broader priority-app lookup |
| **GitHub Security Advisories** | Reviewed open-source package advisories by package/ecosystem |
| **OSV** | Open-source vulnerability lookup across npm, PyPI, Maven, NuGet, Go, crates.io, Packagist, and RubyGems |

Findings show critical status, active exploitation, affected products, installed app/version, patch recommendation, published date, confidence, details, references, and CSV/JSON export.

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
| **Forced** | Bypasses the native uninstaller - removes registry entries and leftover artifacts directly |
| **Batch** | Queue multiple programs and process sequentially with full logging |

**Leftover Scanner**
- Three depth levels: **Safe** (common paths/registry), **Moderate** (broader pattern matching), **Advanced** (deep scan with registry diff)
- Detects: files, folders, registry keys/values, services, scheduled tasks, firewall rules, startup entries, shell extensions
- Risk-level color coding: Safe (green), Review (yellow), Risky (red)
- Registry Snapshot Engine - captures pre/post uninstall diffs to detect leftover changes

**Cleanup & Deletion**
- Select All, Select Safe Only, Deselect All, or manual per-item selection
- Recycle Bin support - files routed via `SHFileOperationW` with `FOF_ALLOWUNDO` (registry and service entries are always permanent)
- Locked file handling - files that cannot be removed are automatically scheduled for deletion on reboot via `MoveFileExW` with `MOVEFILE_DELAY_UNTIL_REBOOT`
- User notification of reboot-pending items

**Uninstall Settings**

| Group | Options |
|---|---|
| **Leftover Selection** | Select all leftovers by default vs. safe only |
| **Deletion Behavior** | Send deleted files to Recycle Bin (toggle) |
| **System Protection** | Auto-create restore point before uninstall |
| **Default Scan Level** | Safe / Moderate / Advanced |
| **Display** | Show system components in the program list |

**Context Menu** - Uninstall, Forced Uninstall, Add to Queue, Open Install Location, Copy Program Name, Copy Uninstall Command, Show Properties, Remove Registry Entry

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
- Queries all physical drives via bundled `smartctl.exe` (smartmontools 7.5)
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

**System Maintenance** - Integrated one-click actions:

| Tool | Description |
|---|---|
| Optimize Power Settings | Applies High Performance / Ultimate Performance plan |
| Verify System Files | `sfc /scannow` + `DISM /RestoreHealth` |
| Check Disk Errors | `chkdsk` with bad-sector scan |
| Generate System Report | Comprehensive report: OS, hardware, storage, network, drivers, event logs, installed programs (HTML) |

**Full Suite Mode**
- One-click sequential run: Hardware Scan ? SMART Analysis ? CPU Benchmark ? Disk Benchmark ? Memory Benchmark ? Stress Test ? Report Generation
- Step-by-step progress with live status

**Report Export**
- **HTML** - Styled report with hardware summary, SMART health, benchmark scores, and stress test results
- **JSON** - Machine-readable structured data for automation
- **CSV** - RFC 4180 compliant, importable into Excel or data pipelines

---

### Partition Manager

Modern disk and partition workspace for technician-safe Windows storage work. The panel appears before Image Flasher with a familiar AOMEI/MiniTool-style ribbon, stateful Scan Disks/Refresh Disks inventory button, fixed-width left Actions and Wizards pane with compact icon text links, scrollable Partition Operations group, Pending Operations queue, partition table, unframed compact proportional disk map, and bottom legend. It uses a queued-apply model: scan disks on demand, select a disk, partition, or free-space region, add operations to the pending queue, dry-run scripts, then apply only after the final review with before/after layout diff.

**Inventory and Layout**
- On-demand async read-only disk/partition/volume inventory from Windows Storage cmdlets so opening the panel does not start a storage scan; the button starts as Scan Disks, becomes Refresh Disks after inventory loads, disk-count/layout summaries go to the main status bar, hidden EFI/MSR partitions get deterministic file-system labels when Windows does not return a mapped volume, and partitions with no Windows label get a small read-only raw signature probe for NTFS, exFAT, FAT12/16/32, ext2/ext3/ext4, XFS, Btrfs, Linux swap, direct or HFS-wrapper HFS+, HFSX, and APFS. If the normal process cannot open the physical drive read-only, the scan retries through the allowlisted elevated `ReadPartitionProbe` helper task; ext, Linux swap, XFS, Btrfs, APFS, and HFS raw metadata plus lightweight sanity notes are shown in File System tooltips and Properties
- Resizable rounded disk map with neutral partition shells, type-colored inner usage bars for GPT/Primary, Logical, Simple, Spanned, Striped, Mirrored, RAID5, and dark-gray Unallocated roles, selection-only highlight outlines, whole-disk row highlight when a disk is selected, a 1 px outer gutter, compact row gaps, rounded disk row/tile containers, plus AOMEI-style borderless partition table with no row-number gutter, disk separator rules, final Apply before/after layout diff, and health, operational, temperature, wear, and error counters where Windows exposes storage reliability data
- Flags for system, boot, EFI, MSR, recovery, BitLocker, read-only, removable, dynamic disk, and Storage Spaces states
- Layout hash guard so queued operations cannot apply after the disk layout changes

**Operations**
- Create, delete, format, assign/change drive letter, set partition label, resize, split, and limited adjacent partition merge
- Create, format, resize, and Move partition start use target-identity Add-to-Queue dialogs with warning text, direct draggable preview-bar handles, synchronized slider/numeric controls, graphical create/resize/move size previews, free-space-before placement, adjacent-free-space extend bounds, partition-type/label/file-system/cluster-size/drive-letter/full-format controls, and no immediate execution; Extend Partition Wizard routes adjacent-free-space extension through that same queued resize path, MovePartition queues offline backup/delete/recreate-at-offset/restore/hash-verify scripts, and donor-space resize still uses the adjacent-donor Allocate Free Space engine instead of unsafe hidden reshuffles
- MBR/GPT conversion with `mbr2gpt.exe` for system MBR-to-GPT and empty-data-disk conversion safety rules
- FAT/FAT32 to NTFS conversion
- Disk/partition clone, image create/restore, partition recovery scan, recovered-partition write-back candidates, OS migration copy plan, and boot repair commands
- Copy Disk, Copy Partition, Partition Recovery, and Migrate OS use source/target/options/review wizard flows; disk and OS copy plans include graphical source/target layout preview, keep/fit copy behavior, source/target sizing, known undersized-target blocking, explicit target overwrite confirmation before Apply, sample/full byte verification scripts, and OS migration boot-validation output for the operation report. Copy Partition can target an image/custom path or a visual unallocated region, carries target disk/offset/size into the clone script, blocks undersized regions, requires overwrite confirmation for raw-device/region writes, and validates the payload destination disk before Apply. Partition Recovery can queue a candidate restore from byte offset, size, and partition type ID; Apply requires acknowledgement, non-overlap checks, disk-bound checks, and target disk safety validation.
- Change Cluster Size queues a destructive backup, reformat with explicit allocation unit, restore, and SHA-256 file-manifest verification path; the UI requires an off-volume backup directory and explicit reformat confirmation before the operation can apply.
- SSD ReTrim/optimization and clear-level wipe paths for free space, partitions, and non-system disks, with warnings when SSD purge/secure-erase semantics require vendor tooling
- Selection-aware sidebar buttons plus table and disk-map right-click menus expose supported disk, partition, and unallocated actions backed by queued operation generation, Explore, safety blocker reasons where validation fails, plus a copyable Properties dialog with disk, partition, volume, SMART, flag, and free-space details
- Advanced Windows-supported actions include file-system checks, surface tests, hide/unhide, active/inactive, partition type ID changes, initialize-disk, and delete-all-partitions
- Cross-filesystem support is tracked in [docs/PARTITION_MANAGER_CROSS_FILESYSTEM_PLAN.md](docs/PARTITION_MANAGER_CROSS_FILESYSTEM_PLAN.md) and the driver-capability matrix is owned by [docs/APFS_HFS_FULL_DRIVER_WRITE_PLAN.md](docs/APFS_HFS_FULL_DRIVER_WRITE_PLAN.md): no runtime installs, no driver dependency, bundled/open-source or original-code tools only, and all write/repair paths route through the Partition Manager queue, safety validators, Apply review, and certification harness. ext2/ext3/ext4 supports detection, browse/extract/export, read-only `e2fsck`, confirmed create/format/repair, confirmed grow, and confirmed same-start shrink through bundled e2fsprogs. Linux swap supports read-only metadata plus confirmed original SWAPSPACE2 create/format. HFS+/HFSX is full driver-level write-certified (H1-H8 - Apple `fsck_hfs` + macOS-kernel RW mount, with physical-USB destructive/crash/rollback at the H8 gate): read+write catalog/attribute/resource-fork workflows; streaming catalog, attributes, and extents-overflow B-trees of arbitrary depth/width with split + underflow merge/rebalance; data/resource-fork and inline/fork-backed-attribute write plus attribute overflow records; the fragmenting allocator with multi-leaf extents; hard-links, symlinks, and complex/hard-linked delete; little- and big-endian journal replay; the embedded HFS-wrapper write edge; and decmpfs read+write of all types through `sak_hfs_writer_cli.exe`, plus sparse-staged create/format/repair through bundled hfsprogs. APFS is full driver-level write-certified (A1-A8 - Apple `fsck_apfs` + macOS-kernel mount, with physical-USB destructive/crash/rollback at the A8 gate): read-only metadata/browse/extract/export plus multi-CIB/CAB create/format and checksum repair to a 32 TiB cap, in-place crash-safe COW mutation of files and directories (write/patch/delete/rename/cross-directory move/object-id-preserving patch), snapshots (create/delete/revert), multi-volume containers, inline zlib compression, credential-gated FileVault encryption, and file clones / sparse files / hard-links / xattr-ACL / in-chunk container resize through queue/apply via `sak_apfs_writer_cli.exe`, gated to S.A.K. generated-layout containers with explicit confirmation. Remaining fail-closed by design: APFS Fusion/Tier2 multi-device (out of scope, no rig), encryption without the user credential, sealed-system-volume writes without a typed seal-invalidation confirmation, APFS container shrink and chunk-adding grow (documented follow-ons), arbitrary non-generated Apple-media mutation at the Apply layer, and XFS/Btrfs writes plus deep XFS/Btrfs tool checks (read-only metadata stays the shipped scope).

**Safety**
- Destructive operations queue first and execute only through cancellable Apply
- Apply opens a final operation/risk/layout review before elevated execution
- System/boot/EFI/MSR/recovery partition mutation is blocked in v1
- Dynamic disks and Storage Spaces block normal writes; a one mounted simple-volume dynamic disk can queue dynamic-to-basic backup/delete/convert/recreate/restore verification.
- Space Analyzer runs a read-only async mounted-volume usage scan with Tree View, Largest Files, and File Types tabs, sortable size/percent/path or count results, and safe right-click Open, Explore, and Copy Path actions.
- Disk Benchmark opens the existing Benchmark and Diagnostics panel; Make Bootable Media opens Image Flasher; Manage BitLocker shows BitLocker status with copyable external commands and Windows management launch; Disk Defrag shows media-aware defrag/ReTrim command guidance, queues HDD defrag only for reported HDD media, blocks SSD/NVMe defrag, and executes through cancellable elevated Apply.
- Quick Partition queues same-style equal-size or custom size/label full-disk layouts for non-system basic disks, including reusable saved presets; GPT layouts allow up to 9 partitions and MBR layouts are capped at four data partitions with Windows extended/logical container behavior recorded during certification; existing partitions are deleted only after Apply review.
- Data Recovery can scan image files or raw volume/device paths read-only for PNG, JPEG, and PDF signatures, review recoverable candidates, and restore them to a separate destination while verifying the source hash stays unchanged.
- Allocate Free Space is code-complete for an immediately adjacent mounted donor partition: it queues donor backup, donor delete, target extend, donor recreate, restore, SHA-256 manifest comparison, and repair scans. Allocate Free Space To on selected unallocated space now queues either online resize into following free space or MovePartition backup/delete/recreate/restore when moving the following partition backward into preceding free space.
- Primary/logical conversion, volume serial-number changes, and one-volume dynamic-disk-to-basic conversion now queue direct destructive backup/rebuild/reformat/restore/verify scripts with off-volume backup and explicit confirmation requirements.
- Disk clone, OS migration, and Copy Partition region targets block known targets smaller than the source before Apply and again in the generated script
- Copy Partition raw-device and unallocated-region targets require explicit overwrite confirmation, payload target-disk identity checks, target size checks, and script-level raw-write safeguards before Apply
- Recovered-partition write-back candidates require typed candidate identity, explicit acknowledgement, non-overlap checks, disk-bound checks, and target disk safety validation before Apply can run
- OS migration writes post-copy target disk scheme, EFI/active partition validation, and firmware boot-order guidance into the execution report
- FAT/FAT32-to-NTFS conversion, split, merge, and create-size requests are preflighted before Apply
- Resize requests are preflighted for required target size, no-op targets, shrink-below-used-space requests, and extend targets that exceed contiguous free space immediately after the partition; the queued layout preview consumes or creates free-space spans for resize operations.
- Move partition start payloads use the `MovePartition` backup/recreate/restore engine. Legacy `Resize` payloads that try to smuggle start offsets or donor partition numbers are still blocked so only the explicit MovePartition and Allocate Free Space engines can perform those workflows.
- Create, format, and Change Cluster Size allocation-unit choices are limited to supported `Format-Volume -AllocationUnitSize` values; unsupported payloads are rejected before script execution
- Create partition type choices are limited to Windows-supported GPT Basic Data, GPT Recovery, GPT EFI System, MBR IFS/NTFS, and MBR FAT32 payloads; incompatible file-system/type combinations are blocked before Apply
- Create honors the selected unallocated region by carrying a relative placement offset into `New-Partition -Offset`, then blocks any size/offset pair that does not fit inside that exact region
- Dry Run shows generated scripts without changing disks
- HTML/JSON operation reports include before/after layout hashes, warnings, steps, stdout/stderr, and result status
- Destructive certification uses a strict disposable-VHD handoff first, then a non-mutating strict hardware handoff that verifies the external VM/hardware/lab manifest, checklist, and evidence package before any hardware-certified release claim is allowed

---

### Image Flasher

Create bootable USB drives from disk images.

- **Formats:** ISO, IMG, WIC, ZIP, GZ, BZ2, XZ, DMG, DSK
- **Windows ISO download** from Microsoft UUP payloads via the UUP Dump API and bundled UUP-to-ISO tooling (multiple editions, languages, x64/ARM64)
- **Linux ISO download** with built-in distro catalog:
  - Ubuntu Desktop, Ubuntu Server, Fedora Workstation, Debian Live GNOME, Arch Linux, Linux Mint Cinnamon
  - Kali Linux
  - SystemRescue, Clonezilla, GParted Live, ShredOS
  - Ventoy (multi-boot USB creator)
- **Windows USB Creator** - NTFS format, ISO extraction, `bootsect.exe` boot sector, UEFI structure, verification of critical files
- **Generic writer** - Raw sector I/O with streaming decompression (bootable for Linux and other ISOs)
- **Safety:** System-drive protection, multi-select for parallel flash

---

### File Management

Current File Management has four subtabs: **File Organizer**, **Duplicate Finder**, **File Explorer**, and **Advanced Search**.

The File Management target layer recognizes mounted Windows-native volumes plus disk/partition inventory targets and manual raw/image targets. ext2/ext3/ext4, HFS+/HFSX, and APFS browse and read through the same original S.A.K. readers used by Partition Manager. Generic organizer moves stay limited to local/mounted file APIs; raw/non-native writes are exposed only as explicit File Explorer create/write/rename/delete actions where certified. HFS+/HFSX and APFS writes run the full driver-level write engines (HFS+ H1-H8, APFS A1-A8, Apple-certified - see [docs/APFS_HFS_FULL_DRIVER_WRITE_PLAN.md](docs/APFS_HFS_FULL_DRIVER_WRITE_PLAN.md)); APFS writes are gated to S.A.K. generated-layout containers (multi-CIB/CAB to a 32 TiB cap) with explicit confirmation, while arbitrary non-generated Apple media, Fusion/Tier2 multi-device, and unprovided-credential encrypted volumes stay fail-closed.


#### File Organizer

Organize files by extension into configurable categories.

- Default categories: Images, Documents, Audio, Video, Archives, Code - fully customizable with user-defined extensions
- Collision handling: rename, skip, or overwrite
- Preview mode - see what would happen without moving anything
- Confirmation dialog before destructive operations (shows file count and collision strategy)
- Category validation - detects empty mappings and duplicate category names
- Reset to Defaults - one-click restore of built-in category mappings

#### Duplicate Finder

Detect duplicate files via content-based hashing.

- MD5 hash-based duplicate detection with configurable minimum-size filtering
- Multi-directory recursive scan with duplicate directory prevention
- Parallel hashing with configurable thread count (auto-detects ideal thread count)
- Read-only duplicate scans for supported raw/image ext2/ext3/ext4, HFS+/HFSX, and APFS targets
- Summary: duplicate count, wasted space, scrollable results for large scans

**Cross-Operation Safety**
- Organizer and duplicate finder share mutual locking - running one disables the other to prevent conflicts
- Cancel support for both operations

#### File Explorer

- Sidebar target navigation for mounted volumes, scanned disk/partition targets, and manual raw/image targets
- Native Qt shell inspired by Files-style explorer workflows: grouped sidebar navigation, command bar, functional View/layout picker, back/forward/up navigation, editable path omnibar, current-folder filter, command palette, details/list/grid/cards/adaptive view modes, context menus, keyboard shortcuts, bottom status strip, copy path, text preview, and persistent preview/properties/safety/evidence pane
- Dedicated shell widgets for sidebar, command bar, omnibar, main pane, and details pane keep the File Explorer layout modular for deeper multi-level Columns polish, tabs, dual-pane, and transfer work
- View/layout picker switches Details/List/Grid/Cards/Adaptive views, exposes the current Columns surface, controls item size, hidden items, and file-extension display, and persists view settings per target/path through the new File Explorer settings path only; unsupported future commands such as tabs and dual pane remain visible with product-grade blocker text
- Details view uses a sort/filter proxy, folder-first sorting, persisted column widths/order, loading/empty/error state text, stale-result discard, and asynchronous directory listing so large folders do not block the UI thread
- Favorites, recent targets, last target restore, target properties, sidebar collapse, details-pane collapse, narrow-width collapse behavior, and shared command-registry blockers for toolbar/context/shortcut routes
- Read-only browsing for supported ext2/ext3/ext4, HFS+/HFSX, and APFS targets through the shared File Management bridge
- Explicit create folder, write file, rename, and single/multi-item delete actions for supported HFS+/HFSX targets and generated-layout APFS targets (full driver-level write, multi-CIB/CAB to a 32 TiB cap), with raw/non-native safety blockers surfaced in the panel
- Clear blockers for file systems without directory browsing support, such as current XFS/Btrfs metadata-only coverage
- Files-like UI/UX parity is tracked in [docs/FILE_MANAGEMENT_EXPLORER_FILES_LIKE_PLAN.md](docs/FILE_MANAGEMENT_EXPLORER_FILES_LIKE_PLAN.md); release wording stays "Files-inspired" until deeper multi-level Columns polish, tabs, dual pane, richer search, and live raw-device certification are implemented and tested. Cloud drives, FTP, Git integration, and third-party integrations are explicitly out of scope for the current milestone.

#### Advanced Search

Enterprise-grade grep-style file content search with a three-panel interface: file explorer, results tree, and preview pane.

- **Text content search** - Line-by-line search with configurable context window (0-10 lines before/after)
- **Regex support** - Full QRegularExpression engine with case-sensitive, whole-word, and multiline options
- **Regex Pattern Library** - 8 built-in patterns (emails, URLs, IPv4, phone numbers, dates, hex, numbers, words) plus custom user-defined patterns with JSON persistence
- **Binary/hex search** - Search binary files for text or hex patterns with hex-context display
- **Image metadata search** - Search EXIF/GPS metadata in image files
- **File metadata search** - Search metadata in PDF, Office, audio/video files
- **Archive search** - Search inside ZIP/EPUB archives with deflate decompression (handles real-world .docx, .xlsx, .pptx, .odt, .epub)
- **File explorer** - Drive and directory navigation with lazy-loading tree, context menus
- **Results tree** - Grouped by file with 8 sort modes (path, match count, file size, date modified)
- **Preview pane** - Monospace file preview with yellow/orange match highlighting, previous/next navigation with wrapping, line numbers with `>>>` match indicators
- **File-system targets** - Search mounted volumes plus supported raw/image ext2/ext3/ext4, HFS+/HFSX, and APFS file systems
- **Search history** - Last 50 searches persisted via ConfigManager

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
- **HTML** - Styled report with all cached diagnostic results and adapter info
- **JSON** - Machine-readable structured data
- Technician name, ticket number, and notes metadata fields

#### Network Adapters

Adapter inspector with ethernet backup, restore, and network reset.

- Enumerates all network adapters (Ethernet, WiFi, Virtual)
- Full configuration display: IPv4/IPv6 addresses, subnet masks, gateways, DHCP status, DNS servers, MAC address, driver info, link speed, traffic statistics
- Copy adapter configuration to clipboard
- Capture adapter IP/DNS/gateway settings to a portable JSON file
- Restore settings to the same or a different adapter on any machine via `netsh` commands
- Supports DHCP and static configurations, primary and secondary DNS servers
- Cross-machine portability - back up on one PC, restore on another
- **Reset Network Settings** - One-click reset: flushes DNS, resets TCP/IP stack, Winsock, firewall, and adapters

#### WiFi Manager

WiFi credential manager with QR code generator and cross-platform network profile export.

**Network Details** - Enter Location label, SSID, Password (show/hide toggle), Security type (WPA/WPA2/WPA3, WEP, Open), and Hidden network flag.

**Saved Networks Table** - Store multiple WiFi configs with search/filter, inline editing, and tri-state select-all. Save/load entire table to/from JSON.

**QR Code Generation**
- Single-network QR for immediate phone/tablet connection
- Batch QR export wizard (PNG/PDF/JPG/BMP) with optional location header banner
- WiFi QR payload format with HIGH error correction (30%)

**Script & Profile Export**
- Generate Windows `.cmd` batch scripts using `netsh` (single or batch)
- Add selected networks directly to Windows known WiFi profiles via netsh WLAN profile XML
- Generate macOS `.mobileconfig` plist files (single or multiple networks per profile)

**Windows Integration** - Scan existing Windows known profiles and import them into the table for backup/management.

---

### Email Tools

Browse, search, and export data from Outlook PST/OST archives and MBOX mailboxes - no Outlook or MAPI libraries required.

**Supported Formats**
- **PST** - Outlook Personal Storage (Unicode and ANSI)
- **OST** - Outlook Offline Storage
- **MBOX** - RFC 4155 (Thunderbird, Apple Mail, Linux mail clients)

**File Scanner** - Automatically discovers PST/OST/MBOX files in common locations (user home, desktop, recent paths); select which to open.

**Folder Tree** - Navigable hierarchy with typed icons (Inbox, Sent Items, Drafts, Deleted Items, Junk Email, Calendar, Contacts, etc.)

**Item List** - Sortable table with checkbox multi-select, Subject, From, Date, Size, and Type. Use `Save Selected Email` beside the preview `Images` toggle to export checked messages.

**Preview Pane** - Four tabs:

| Tab | Content |
|---|---|
| **Content** | HTML/plain-text email body; contact details; task descriptions; sticky note text |
| **Headers** | RFC 5322 message headers (monospace) |
| **Properties** | MAPI property names and values for forensics/analysis |
| **Attachments** | File list with individual or batch save |

**Search** - Full-text search across subjects, bodies, senders, recipients, and attachment names. Filter by item type, date range, has-attachment, and folder scope.

**Contacts Dialog** - Searchable address book with sortable columns (name, email, company, phone) and export to VCF or CSV.

**Calendar Dialog** - Three view modes (month, week, day) with event highlighting, half-hour grid lines, and column separators. Navigate by date, click events to view details, and export to ICS or CSV.

**Attachments Browser** - Scans all emails in the mail file and presents every attachment in a searchable, filterable list with checkbox selection. Type filter (images, documents, archives, etc.), filename search, and right-click context menu with Save Attachment, View Containing Email (navigates to the source message), and Copy Filename.

**Export Formats**

| Format | Use Case |
|---|---|
| **EML** | RFC 5322 email files (Outlook, Thunderbird compatible) |
| **HTML** | Styled standalone email pages with attachments saved beside each message |
| **PDF** | Rendered email archives with attachments saved beside each message |
| **CSV** | Emails, contacts, calendar, or tasks as spreadsheet data |
| **VCF** | vCard 3.0 contact files |
| **ICS** | iCalendar appointments/events |
| **TXT** | Plain-text emails or sticky notes with attachments saved beside each email |
| **Attachments** | Batch extract with optional filtering and inline-image skip |

---

### OST Converter

Multi-threaded bulk OST/PST conversion engine integrated as a second tab in the Email Tools panel.

**Supported Output Formats**

| Format | Description |
|---|---|
| **PST** | Outlook Personal Storage (Unicode) |
| **EML** | RFC 5322 email files |
| **MSG** | Outlook message files |
| **MBOX** | Unix mailbox format |
| **DBX** | Outlook Express format |
| **HTML** | Styled HTML email archives |
| **PDF** | PDF email archives |
| **IMAP Upload** | Direct upload to IMAP servers (Office 365, Gmail, Yahoo) |

**Key Features**
- Multi-threaded conversion with configurable worker count (1-8 threads)
- Deleted item recovery (soft and hard delete scanning)
- PST splitting for large archives with configurable size limits
- IMAP cloud upload with PLAIN, LOGIN, and XOAUTH2 authentication
- Advanced filtering by date range, folder, sender, and subject
- Corruption handling with automatic recovery
- Metadata preservation across all output formats
- Detailed conversion reporting

---

### Settings

Settings are configured per-panel - each panel that needs configuration provides its own settings dialog or inline controls.

| Panel | Settings |
|---|---|
| **Backup and Restore** | Default backup location, confirmation prompts, notification preferences, logging, compression |
| **Advanced Uninstall** | Leftover selection defaults, deletion behavior, system protection (restore points), scan level, system component visibility |
| **Image Flasher** | Flash settings, verification options |

---

## Security

| Layer | Implementation |
|---|---|
| **File encryption** | AES-256-CBC via Windows BCrypt with PBKDF2 key derivation |
| **Secure memory** | `SecureString` / `SecureBuffer` - zero-fill on destruction, page locking via `VirtualLock` |
| **Input validation** | Path sanitization, IP/port validation, Chocolatey name format checks |
| **Elevation** | Per-task elevation via elevated helper process with Named Pipes IPC; `asInvoker` manifest with on-demand UAC prompts |
| **AI credential storage** | OpenAI API key encrypted with Windows DPAPI and stored in the portable app `data/credentials/` directory with owner-only file permissions |
| **AI evidence safety** | AI session data, transcripts, logs, downloads, screenshots, reports, and artifacts stay under portable app `data/ai_sessions/`; model-bound command output is capped and secrets are redacted |

Cryptographic operations use Windows DPAPI and BCrypt APIs where implemented. Windows FIPS validation depends on the active Windows cryptographic modules and system policy, so this project does not claim independent FIPS certification.

---

## Building from Source

### Prerequisites

| Tool | Version |
|---|---|
| Visual Studio 2022 | v17+ with _Desktop development with C++_ |
| CMake | 3.28+ |
| Qt | 6.5+ for MSVC x64 |
| vcpkg | Current supported bootstrap (for zlib, bzip2, liblzma) |

Release CI currently builds with Qt 6.10.3 on the latest aqt-installable MSVC 2022 x64 Qt package while the source minimum remains Qt 6.5+.
Set `Qt6_DIR` or replace `$env:Qt6_DIR` below with your installed Qt path, for example
`C:/Qt/<version>/<msvc-x64-kit>`.

### Build

```powershell
git clone https://github.com/RandyNorthrup/S.A.K.-Utility.git
cd S.A.K.-Utility

# Optional: refresh bundled tools after clone or upstream tool updates.
# Run these before configure if a required tools/ subtree is missing.
powershell -ExecutionPolicy Bypass -File scripts/bundle_smartmontools.ps1
powershell -ExecutionPolicy Bypass -File scripts/bundle_iperf3.ps1
powershell -ExecutionPolicy Bypass -File scripts/bundle_chocolatey.ps1
powershell -ExecutionPolicy Bypass -File scripts/bundle_uup_tools.ps1

# Configure
cmake -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH="$env:Qt6_DIR"

# Optional: explicitly toggle the AI Assistant panel
# -DSAK_ENABLE_AI_ASSISTANT=ON  # default

# Build
cmake --build build --config Release --parallel

# Run
.\build\Release\sak_utility.exe
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
  -DCMAKE_PREFIX_PATH="$env:Qt6_DIR" `
  -DSAK_CODE_SIGN=ON
cmake --build build --config Release
```

Requires Azure CLI and access to the Azure Trusted Signing account. CI builds sign release artifacts only when the signing credentials are configured.

### Release Readiness Gates

Release candidates must pass the aggregate readiness gate before publication:

```powershell
$version = (Get-Content VERSION -Raw).Trim()
$packageName = "SAK-Utility-v$version"
powershell -ExecutionPolicy Bypass -File scripts/stage_portable_release.ps1 `
  -BuildDir build\Release `
  -PackageName $packageName
powershell -ExecutionPolicy Bypass -File scripts/create_release_archive.ps1 `
  -BuildDir build\Release `
  -PackageName $packageName
$extract = "build\Release\clean-readiness-extract"
Remove-Item -Recurse -Force $extract -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $extract | Out-Null
Expand-Archive -LiteralPath "build\Release\$packageName-Windows-x64.zip" -DestinationPath $extract -Force
powershell -ExecutionPolicy Bypass -File scripts/check_release_readiness.ps1 `
  -PackageRoot $extract `
  -RequireSignedPackage
```

For unsigned local preflight builds, omit `-RequireSignedPackage`; signed package validation remains required before publication.

This verifies secret scanning, blocking-pattern rules, accessibility coverage,
PowerShell syntax, style-token/style-literal/layout constants, global magic numbers, logged dialog
usage, Lizard limits, third-party license documentation, Qt resource manifests,
Partition Manager certification-matrix integrity, commercial/destructive feature mapping, VHD preflight reporting, certification-gap reporting, certification-artifact bundling, feature-matrix coverage, portable package contents, startup E2E smoke, and Authenticode signatures. See
[docs/RELEASE_READINESS.md](docs/RELEASE_READINESS.md) and
[docs/SECURITY_THREAT_MODEL.md](docs/SECURITY_THREAT_MODEL.md).

Partition Manager destructive storage paths have a separate certification harness:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_partition_manager_destructive_certification.ps1 `
  -RunVhdDataDiskMatrix
```

For the one-command strict VHD handoff, use:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_partition_manager_vhd_certification_strict.ps1 `
  -RelaunchElevated
```

That harness uses disposable VHD media only and covers create/format/resize/delete,
FAT32-to-NTFS conversion, Quick Partition GPT equal/custom and MBR four-data-partition layouts, adjacent Extend Wizard growth,
recovered-partition write-back, empty GPT/MBR conversion, clear-disk wipe, and
offline VHD image clone, image restore overwrite, and partition-clone raw-region evidence. The shared certification matrix in
`docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json` defines required evidence keys
and safety contracts; the matrix-integrity guard, external JSON scaffold,
Markdown lab checklist verifier, external lab package verifier, per-gate
`report.json` importer, read-only VHD preflight report, certification-gap report,
hashed certification-artifact bundle, certification verifier, and claim-level
script reject incomplete,
wrong-mode, or blank passed VHD or external evidence payload values. VM-lab evidence
currently has all 18/18 external VM/hardware/lab gates passed, including
System MBR-to-GPT, OS migration reboot proof, UEFI/BIOS boot repair, removable
USB, SSD/NVMe, SSD secure erase, non-adjacent partition move, primary/logical,
volume serial-number, dynamic-to-basic, and physical wipe cases. Strict
hardware-certified readiness requires both `-PartitionExternalEvidenceManifest`
and `-PartitionExternalEvidenceChecklist`. See
[docs/PARTITION_MANAGER_CERTIFICATION.md](docs/PARTITION_MANAGER_CERTIFICATION.md).

### Dependencies

| Library | License | Purpose |
|---|---|---|
| [Qt 6.5+](https://www.qt.io/) | LGPL v3 | UI framework (Core, Widgets, Concurrent, Network, Xml; Gui linked transitively via Widgets) |
| [zlib](https://www.zlib.net/) | zlib License | Compression and ZIP archive decompression (deflate) |
| [bzip2](https://sourceware.org/bzip2/) | BSD-style | bzip2 compression |
| [liblzma](https://tukaani.org/xz/) | 0BSD / Public Domain | xz/LZMA compression |
| [qrcodegen](https://www.nayuki.io/page/qr-code-generator-library) | MIT | QR code generation (bundled source) |
| [win32-mcp-server](https://github.com/RandyNorthrup/win32-mcp-server) | MIT | Bundled portable MCP server for manifest-gated Windows desktop observation/automation |
| [Context7 MCP](https://github.com/upstash/context7) | MIT | Remote documentation MCP provider; no bundled code; no app API key |
| [Microsoft Learn MCP](https://learn.microsoft.com/en-us/training/support/mcp) | Microsoft Learn Terms | Remote Microsoft documentation MCP provider; no bundled code; no app API key |
| [aria2](https://aria2.github.io/) | GPLv2 | Multi-connection downloader bundled for ISO/UUP payload downloads |
| [UUPMediaCreator](https://github.com/OSTooling/UUPMediaCreator) | MIT | Bundled UUP-to-ISO conversion tooling |
| [wimlib / libwim](https://wimlib.net/) | LGPL v3 | WIM image library bundled with the UUP conversion tooling |
| [smartmontools 7.5](https://www.smartmontools.org/) | GPLv2 | SMART disk health analysis (bundled `smartctl.exe`) |
| [iPerf3 3.21](https://iperf.fr/) | BSD 3-Clause | LAN bandwidth testing (bundled `iperf3.exe`) |
| [Chocolatey 2.7.2](https://chocolatey.org/) | Apache 2.0 | Embedded portable package manager |
| [Icons8](https://icons8.com/) | Icons8 Free License | UI icons with attribution |
| Windows BCrypt | OS component | AES-256, PBKDF2, SHA-256 |

Full license texts: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)

### Testing

```powershell
cmake --build build --config Release --target RUN_TESTS
```

Automated tests cover AI assistant clients, MCP HTTP parsing, provider registry, workflow store/evals, orchestration, subagents, tool policy/dispatch, execution broker, cancellation, run state, trace store, credential redaction, Advanced Search, Advanced Uninstall (types, controller, leftover scanner, registry snapshot engine), Network Diagnostics (types, utils, report generation), Partition Manager (inventory parsing, safety blockers, script generation, layout-hash queue guards, panel chrome, direct destructive queue dialogs, disk-map resizing, and feature-matrix release gating), Email Inspector (PST/OST parsing, MBOX parsing, email types, search, export, profile manager, report generator), OST Converter (types, controller, PST splitter, integration), Offline Deployment (install script parsing, NuGet API, script rewriting, package builder), Elevation (tier classification, IPC protocol, task dispatcher, mixed-tier operations, UX components, hardening), diagnostics, security, encryption, configuration, ISO download, and quick action validation.

---

## Configuration

Portable ZIP builds store settings at `<app>/data/config/Utility.ini` in INI format.
Packaged builds use the OS app-local data directory when Windows package identity is present.

AI Assistant sessions and credentials are intentionally portable-app local:

| Path | Purpose |
|---|---|
| `<app>/data/credentials/openai_api_key.dpapi.json` | Encrypted OpenAI API key |
| `<app>/data/ai_sessions/` | Session manifests, transcripts, usage, memory, run state, artifacts, downloads, screenshots, and reports |
| `<app>/data/ai/providers/` | Portable AI provider registry overrides |
| `<app>/data/ai/app_manifests/` | Portable app-control manifests for scanner/tool workflows |
| `<app>/data/ai/workflows/` | User-added workflow templates |

OpenAI API calls include a `safety_identifier` derived from the local AI session
ID hash. The raw session ID and user identity are not sent for this field.

The provider gateway exposes Microsoft Learn and Context7 public documentation
lookups through HTTP MCP, plus bundled Win32 MCP calls through the portable
`tools/mcp/win32-mcp-server/win32-mcp-server.exe`. Win32 MCP access follows the
selected AI access mode: observation tools are read-only, interactive tools use
the interactive profile, and high-risk tools require the existing high-risk
handling path. Supported app-manifest actions can run through the same gateway;
for example, the Microsoft Defender manifest exposes quick/full scan and
definition update actions while SUPERAntiSpyware scan actions remain manual
until a validated non-interactive workflow exists.

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

- **File Converter Tab** - Universal offline file conversion for documents, images, audio, video, spreadsheets, and PDFs with batch processing and quality controls. Adds a new tab to the File Management panel. See [FILE_CONVERTER_TAB_PLAN.md](docs/FILE_CONVERTER_TAB_PLAN.md).
- **macOS Bootable USB** - Create macOS bootable USB drives from macOS installer images. See [MACOS_BOOTABLE_USB_PLAN.md](docs/MACOS_BOOTABLE_USB_PLAN.md).

---

## License

This project is licensed under the **GNU Affero General Public License v3.0 (AGPL-3.0)** - see [LICENSE](LICENSE) for the full text.

Third-party dependency licenses are documented in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

---

## Acknowledgments

- [**Qt**](https://www.qt.io/) - Cross-platform UI framework (LGPL v3)
- [**qrcodegen**](https://www.nayuki.io/page/qr-code-generator-library) - QR code generator by Project Nayuki (MIT)
- [**win32-mcp-server**](https://github.com/RandyNorthrup/win32-mcp-server) - Portable Windows automation MCP server (MIT)
- [**Context7 MCP**](https://github.com/upstash/context7) - Remote code documentation MCP provider (MIT source; no app API key required)
- [**Microsoft Learn MCP**](https://learn.microsoft.com/en-us/training/support/mcp) - Remote Microsoft documentation MCP provider
- [**aria2**](https://aria2.github.io/) - Multi-connection download manager (GPLv2)
- [**UUPMediaCreator**](https://github.com/OSTooling/UUPMediaCreator) - UUP-to-ISO converter by OSTooling (MIT)
- [**wimlib / libwim**](https://wimlib.net/) - WIM image library by Eric Biggers (LGPL v3, bundled with UUPMediaConverter)
- [**smartmontools**](https://www.smartmontools.org/) - SMART disk diagnostics (GPLv2)
- [**iPerf3**](https://iperf.fr/) - LAN bandwidth testing (BSD 3-Clause)
- [**Icons8**](https://icons8.com/) - UI icons (Icons8 Free License with attribution)
- [**Chocolatey**](https://chocolatey.org/) - Windows package manager (Apache 2.0)
- [**zlib**](https://www.zlib.net/) - Compression library (zlib License)
- [**bzip2**](https://sourceware.org/bzip2/) - Compression library (BSD-style)
- [**XZ Utils**](https://tukaani.org/xz/) - LZMA compression (0BSD / Public Domain)
- [**vcpkg**](https://vcpkg.io/) - C++ dependency manager (MIT)
- [**CMake**](https://cmake.org/) - Build system (BSD 3-Clause)
- **Microsoft** - Windows BCrypt API, PowerShell, Windows SDK, ADK tools (cdimage, imagex, bcdedit, cabarc)

---

## Author

**Randy Northrup**
[GitHub](https://github.com/RandyNorthrup) - [S.A.K.-Utility](https://github.com/RandyNorthrup/S.A.K.-Utility)

---

## Support

- [Issues](https://github.com/RandyNorthrup/S.A.K.-Utility/issues)
- [Discussions](https://github.com/RandyNorthrup/S.A.K.-Utility/discussions)
- [Releases](https://github.com/RandyNorthrup/S.A.K.-Utility/releases)

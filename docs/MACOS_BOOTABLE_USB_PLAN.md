# macOS Bootable USB Creator — Comprehensive Implementation Plan

**Version**: 1.0
**Date**: April 2, 2026
**Status**: 📋 Planned
**Target Release**: TBD

---

## 🎯 Executive Summary

The macOS Bootable USB Creator extends the existing Image Flasher panel to support
downloading Apple macOS installer images and writing fully-offline bootable macOS
installer USB drives — entirely from Windows. This gives PC technicians the ability
to prepare macOS installer media on the same machine they use for Windows and Linux,
without needing access to a Mac. The feature mirrors the existing Linux ISO download
workflow: browse a catalog, download, and flash to USB.

### Key Objectives

- [ ] **macOS Installer Catalog** — Parse Apple's public sucatalog to discover
      available macOS installer versions (Big Sur through latest)
- [ ] **macOS DMG Downloader** — Download `InstallAssistant.pkg` / full installer
      DMGs via aria2c with progress, checksum verification, and resume support
- [ ] **DMG Analyzer** — Read UDIF trailer and HFS+ volume header from `.dmg` files
      to populate the Image Details panel (OS name, version, size, boot status)
- [ ] **DMG Extraction** — Convert Apple UDIF DMG to raw image via bundled `dmg2img.exe`
- [ ] **HFS+ Bootable USB Writer** — Partition the target drive with a GPT layout
      containing an EFI System Partition (FAT32) and an Apple HFS+ partition, then
      write the macOS installer content using bundled `hfsplus.exe`
      (from mozilla/libdmg-hfsplus)
- [ ] **End-to-End Offline Boot** — The resulting USB boots a Mac directly into the
      macOS installer with no internet required on the target machine
- [ ] **Image Flasher Integration** — Add a 4th download card ("macOS") to the
      Image Flasher panel, following the same UX patterns as the Linux card
- [ ] **Tests** — Unit tests for catalog parsing, DMG analysis, GPT layout, and
      HFS+ write orchestration

---

## 📊 Project Scope

### What is macOS Bootable USB Creation?

Creating a bootable macOS installer USB from Windows is a multi-step process that
historically required either a Mac or commercial software (e.g., TransMac). The
challenge is that macOS installer media uses Apple's HFS+ filesystem, which Windows
cannot natively read or write.

**End-to-End Workflow**:
1. Query Apple's public software update catalog (sucatalog) to discover available
   macOS installer versions and their download URLs
2. Download the full macOS installer image (typically 12–14 GB)
3. Analyze the DMG to extract metadata (macOS version, build, boot type)
4. Partition the target USB drive with a GUID Partition Table (GPT):
   - 200 MB EFI System Partition (FAT32) for UEFI boot
   - Remaining space as Apple HFS+ partition
5. Extract the DMG contents and write them to the HFS+ partition using
   mozilla/libdmg-hfsplus tooling
6. The resulting USB boots any compatible Mac directly into the macOS installer

**Key External Tools (bundled as standalone .exe, invoked via QProcess)**:

| Tool | Source | License | Purpose |
|------|--------|---------|---------|
| `dmg2img.exe` | dmg2img project | GPL-2.0 | Convert UDIF DMG → raw IMG |
| `hfsplus.exe` | mozilla/libdmg-hfsplus | GPL-3.0 | Read/write HFS+ volumes, extract/inject files |
| `dmg.exe` | mozilla/libdmg-hfsplus | GPL-3.0 | Parse/extract DMG container metadata |
| `aria2c.exe` | aria2 (already bundled) | GPL-2.0 | Multi-connection download with resume |

> **License strategy**: All GPL-licensed tools are bundled as standalone executables
> in `tools/` and invoked via `QProcess`, identical to how `aria2c.exe` and
> `iperf3.exe` are already bundled. No GPL code is linked into the main SAK binary.
> This preserves SAK's own license terms.

**Key Data Sources**:
- **Apple sucatalog** — Public XML plist at
  `swscan.apple.com/content/catalogs/others/index-{version-chain}.merged-1.sucatalog`
  Lists all macOS installers with download URLs and integrity info
- **gibMacOS** (MIT, 7k stars) — Reference implementation for sucatalog parsing;
  documents the catalog URL patterns and product key structure
- **UDIF format** — Apple's Universal Disk Image Format; the DMG trailer at EOF
  contains an XML plist with partition map, block sizes, and checksums
- **HFS+ specification** — Apple's Hierarchical File System Plus; volume header at
  offset 1024 contains volume name, creation date, block count, and catalog info
- **GPT specification** — GUID Partition Table (UEFI standard); well-documented
  binary layout with Apple HFS+ partition type GUID
  `48465300-0000-11AA-AA11-00306543ECAC`

---

## 🎯 Use Cases

### 1. **Technician Creates macOS Installer USB at the Bench**

**Scenario**: A client brings in a MacBook with a corrupted macOS installation.
The technician has no Mac available but needs to create a bootable macOS installer
USB to reinstall the OS.

**Workflow**:
1. Open Image Flasher panel → click **macOS** download card
2. macOS Catalog Dialog appears, showing available versions:
   - macOS Sequoia 15.4 (latest)
   - macOS Sonoma 14.7.4
   - macOS Ventura 13.7.4
   - macOS Monterey 12.7.6
3. Select "macOS Sonoma 14.7.4" (matches the client's hardware)
4. Click Download → aria2c downloads the ~13 GB installer with progress bar
5. Download completes → checksum verified → auto-selects as the active image
6. **Image Details panel** shows: "macOS Sonoma 14.7.4", "Build 23H420",
   "Apple Disk Image", "12.9 GB", "UEFI Bootable"
7. Select target USB drive (≥16 GB) → click Flash
8. SAK partitions the drive (GPT + EFI + HFS+), extracts the DMG, writes content
9. Hand the USB to the client or boot the MacBook from it
10. macOS installer launches — **fully offline, no internet required**

**Benefits**:
- No Mac required to create macOS installer media
- Same workflow as creating Linux USB — consistent UX
- Full offline install — works even if the MacBook has no WiFi/Ethernet

---

### 2. **Loading a User-Provided DMG File**

**Scenario**: A technician already has a macOS DMG file (obtained previously or
from a client) and wants to identify it and flash it to USB.

**Workflow**:
1. Open Image Flasher panel → click **Select Image File**
2. Choose a `.dmg` file from disk
3. **DMG Analyzer** reads the file's UDIF trailer and HFS+ volume header
4. **Image Details panel** populates:
   - **OS**: "macOS Ventura 13.6.1" (detected from volume name / SystemVersion.plist)
   - **Architecture**: "Universal (x86_64 + ARM64)" or "ARM64"
   - **Format**: "Apple Disk Image (UDIF compressed)"
   - **Size**: "12.1 GB" (file size) / "14.3 GB" (uncompressed)
   - **Boot**: "UEFI Bootable" (EFI partition detected in DMG partition map)
   - **Filesystem**: "HFS+"
   - **Volume Label**: "Install macOS Ventura"
   - **Creation Date**: "2023-10-25"
5. Technician confirms details → selects drive → flashes

**Benefits**:
- DMG files are no longer "black boxes" — full metadata visible before flashing
- Same info density as ISO files (OS, version, arch, boot type, size)
- Prevents flashing the wrong image to the wrong drive

---

### 3. **Identifying an Unknown DMG**

**Scenario**: A technician finds several `.dmg` files on a network share with
cryptic names and needs to identify which macOS version each contains.

**Workflow**:
1. Open Image Flasher → Select Image File → pick first DMG
2. Image Details immediately shows: "macOS Monterey 12.7.6", "Build 21H1320",
   "ARM64", "11.8 GB"
3. Repeat for other files — each one is analyzed and identified in seconds
4. Technician now knows exactly which DMG to use

**Benefits**:
- Instant identification without mounting or extracting
- Works for any Apple UDIF DMG, not just macOS installers
- Volume label and build number provide definitive identification

---

### 4. **Multi-Mac Deployment**

**Scenario**: IT department needs to reimage 10 MacBooks before reassigning them.
They need multiple macOS installer USBs.

**Workflow**:
1. Download macOS Sonoma once via the macOS catalog
2. Insert first USB → flash → repeat with remaining USBs
3. Each flash takes ~15–20 minutes (limited by USB write speed)
4. The downloaded DMG is cached — no need to re-download

**Benefits**:
- Download once, flash many
- Existing multi-drive support in Flash Coordinator could enable parallel writes
- No Mac needed in the imaging room

---

## 🏗️ Architecture Overview

### Component Hierarchy

```
ImageFlasherPanel (existing QWidget)
├── [Existing] Download Cards: Microsoft Direct | UUP Download | Linux
├── [NEW] Download Card: macOS
│   └── Opens MacOSInstallerDialog
│
├── [Existing] Select Image File button
│   └── .dmg files already accepted in file dialog filter
│
├── [Existing] Image Details Group (m_isoInfoGroup)
│   ├── m_infoOsLabel        ← NEW: "macOS Sonoma 14.7.4"
│   ├── m_infoArchLabel       ← NEW: "ARM64" / "Universal"
│   ├── m_infoSizeLabel       ← existing
│   ├── m_infoFormatLabel     ← NEW: "Apple Disk Image (UDIF compressed)"
│   ├── m_infoBootLabel       ← NEW: "UEFI Bootable"
│   ├── m_infoFilesysLabel    ← NEW: "HFS+"
│   ├── m_infoVolLabel        ← NEW: "Install macOS Ventura"
│   ├── m_infoPublisherLabel  ← NEW: "Apple Inc."
│   ├── m_infoDateLabel       ← NEW: creation date from HFS+ volume header
│   └── m_infoEditionsLabel   ← (unused for macOS)
│
├── [Existing] Drive Selection (DriveScanner)
├── [Existing] Flash Coordinator → Flash Worker
│   └── [MODIFIED] New flash pipeline for macOS DMGs:
│       DMG → dmg2img → raw IMG → GPT partition → HFS+ write
│
└── [NEW] macOS-specific components:
    ├── MacOSCatalogParser (QObject)
    │   ├── Fetches and parses Apple sucatalog XML plist
    │   ├── Extracts: product key, version, build, download URLs, sizes
    │   ├── Filters to full macOS installer packages only
    │   └── Output: QVector<MacOSInstallerInfo>
    │
    ├── MacOSInstallerDialog (QDialog)
    │   ├── Version list with macOS name, version, build, size
    │   ├── Download button → delegates to MacOSDownloader
    │   ├── Progress bar, speed, ETA (mirrors LinuxISODownloadDialog)
    │   └── Checksum verification status
    │
    ├── MacOSDownloader (QObject)
    │   ├── Uses aria2c (already bundled) for multi-connection download
    │   ├── Phase state machine: Idle → Resolving → Downloading →
    │   │   Verifying → Completed/Failed
    │   ├── Supports resume on interrupt
    │   └── Output: path to downloaded DMG/PKG file
    │
    ├── DmgAnalyzer (QObject) [NEW — image identification]
    │   ├── Reads UDIF trailer (last 512 bytes) for DMG metadata
    │   ├── Parses XML plist from DMG resource fork:
    │   │   - Partition map (number/type of partitions)
    │   │   - Block sizes and checksums
    │   │   - Compression type (zlib, bzip2, lzfse, raw)
    │   ├── Reads HFS+ volume header (offset 1024 within HFS+ partition):
    │   │   - Volume name → "Install macOS Ventura"
    │   │   - Creation/modification dates
    │   │   - Total blocks × block size → uncompressed volume size
    │   ├── macOS version detection:
    │   │   - Volume name pattern matching ("Install macOS <Name>")
    │   │   - Maps name → version (Sequoia=15, Sonoma=14, Ventura=13, ...)
    │   ├── Architecture detection:
    │   │   - Post-Big Sur (11+): Universal or ARM64
    │   │   - Pre-Big Sur: x86_64
    │   ├── Boot detection:
    │   │   - Checks partition map for EFI System Partition entry
    │   │   - All macOS installer DMGs are UEFI bootable
    │   ├── Returns: DmgInfo struct (see Data Structures below)
    │   └── Integrates with populateIsoInfo() in ImageFlasherPanel
    │
    └── MacOSFlashWorker (QObject) [Worker Thread]
        ├── Step 1: DMG → raw IMG via dmg2img.exe (QProcess)
        ├── Step 2: Wipe target drive, create GPT layout:
        │   ├── Protective MBR (LBA 0)
        │   ├── GPT Header (LBA 1)
        │   ├── Partition Entry 1: EFI System Partition
        │   │   - Type GUID: C12A7328-F81F-11D2-BA4B-00A0C93EC93B
        │   │   - Size: 200 MB, formatted FAT32
        │   ├── Partition Entry 2: Apple HFS+
        │   │   - Type GUID: 48465300-0000-11AA-AA11-00306543ECAC
        │   │   - Remaining space
        │   └── Backup GPT Header (last LBA)
        ├── Step 3: Format HFS+ partition via hfsplus.exe (QProcess)
        ├── Step 4: Extract DMG contents to HFS+ partition via hfsplus.exe
        ├── Step 5: Write boot files to EFI partition
        ├── Progress signals throughout (reuses FlashProgress struct)
        ├── Cancellation support at each step boundary
        └── Cleanup of temp raw IMG on completion/failure
```

---

## 🛠️ Technical Specifications

### DMG Analyzer (Image Identification)

**Purpose**: Read and parse Apple DMG files to populate the Image Details panel,
giving DMG files the same level of identification as ISO files currently receive.

**Why This Matters**: Currently, selecting a `.dmg` file in the Image Flasher shows
only the filename, file size, and "Apple Disk Image" — no OS name, version,
architecture, boot type, filesystem, volume label, or creation date. The
`IsoAnalyzer` reads ISO 9660 Primary Volume Descriptors at sector 16, which
doesn't exist in DMG files. A dedicated `DmgAnalyzer` is needed to read the
UDIF and HFS+ structures specific to Apple disk images.

**Data Structures**:
```cpp
struct DmgPartitionEntry {
    QString name;           // "disk image (Apple_HFS : 3)"
    QString type;           // "Apple_HFS", "Apple_Free", "EFI"
    qint64 sectorOffset;
    qint64 sectorCount;
};

struct DmgInfo {
    // Identity
    QString os_name;            // "macOS Sonoma 14.7.4"
    QString os_version;         // "14.7.4"
    QString build_number;       // "23H420"
    QString architecture;       // "ARM64", "x86_64", "Universal"
    QString os_family;          // "macOS" (always)

    // DMG Metadata (from UDIF trailer + XML plist)
    QString volume_label;       // "Install macOS Ventura"
    QString compression_type;   // "zlib", "bzip2", "lzfse", "raw"
    qint64 compressed_size;     // File size on disk
    qint64 uncompressed_size;   // Actual data size when extracted
    QVector<DmgPartitionEntry> partitions;
    QString checksum_type;      // "CRC32" or "MD5" from UDIF
    QString checksum_value;

    // HFS+ Volume Header
    QString filesystem;         // "HFS+"
    QDateTime creation_date;    // From HFS+ volume header
    QDateTime modification_date;
    qint64 total_blocks;
    qint64 block_size;          // Typically 4096

    // Boot Info
    bool is_bootable;           // EFI partition present in partition map
    QString boot_type;          // "UEFI" (always for macOS)

    // Validation
    bool is_valid_dmg;          // UDIF magic found
    bool is_macos_installer;    // Volume name matches "Install macOS *"
    QString error_message;      // If parsing failed
};
```

**UDIF Trailer Format** (last 512 bytes of DMG file):
```
Offset  Size  Field
0       4     Magic ("koly" = 0x6B6F6C79)
4       4     Version (4)
8       4     Header size (512)
12      4     Flags
16      8     Running data fork offset
24      8     Data fork offset
32      8     Data fork length
40      8     Resource fork offset
48      8     Resource fork length
...
216     4     XML plist offset (from file start)
224     8     XML plist length
...
```

**Implementation Approach**:
```cpp
class DmgAnalyzer {
public:
    // Static, thread-safe, no global state (mirrors IsoAnalyzer pattern)
    static DmgInfo analyze(const QString& file_path);

private:
    // Step 1: Read last 512 bytes, validate "koly" magic
    static bool readUdifTrailer(QFile& file, DmgInfo& info);

    // Step 2: Seek to XML plist offset, parse partition map
    static bool parseResourceFork(QFile& file, qint64 offset,
                                  qint64 length, DmgInfo& info);

    // Step 3: Locate HFS+ partition in partition map,
    //         read volume header at partition_offset + 1024
    static bool readHfsPlusVolumeHeader(QFile& file, DmgInfo& info);

    // Step 4: Pattern-match volume name to macOS version
    static void identifyMacOS(DmgInfo& info);

    // macOS name → version mapping
    // "Sequoia" → 15, "Sonoma" → 14, "Ventura" → 13,
    // "Monterey" → 12, "Big Sur" → 11, "Catalina" → 10.15,
    // "Mojave" → 10.14, "High Sierra" → 10.13, "Sierra" → 10.12,
    // "El Capitan" → 10.11, "Yosemite" → 10.10
    static constexpr std::array kMacOSVersions = { ... };
};
```

**Integration with Image Flasher Panel**:

The existing `populateIsoInfo()` method currently calls `IsoAnalyzer::analyze()`
and populates 10 UI labels. For DMG files, the flow becomes:

```
populateIsoInfo(imagePath)
    ├── if format == ISO/IMG → IsoAnalyzer::analyze()  [existing]
    └── if format == DMG     → DmgAnalyzer::analyze()  [NEW]
            ├── m_infoOsLabel      = dmgInfo.os_name
            ├── m_infoArchLabel    = dmgInfo.architecture
            ├── m_infoSizeLabel    = formatFileSize(compressed) +
            │                        " (" + formatFileSize(uncompressed) + " uncompressed)"
            ├── m_infoFormatLabel  = "Apple Disk Image (UDIF " + compression_type + ")"
            ├── m_infoBootLabel    = dmgInfo.boot_type (if is_bootable)
            ├── m_infoFilesysLabel = dmgInfo.filesystem
            ├── m_infoVolLabel     = dmgInfo.volume_label
            ├── m_infoPublisherLabel = "Apple Inc."
            ├── m_infoDateLabel    = dmgInfo.creation_date
            └── m_infoEditionsLabel = "" (unused for macOS)
```

**Validation on Load**:
- Check file size ≥ 512 bytes (minimum to read UDIF trailer)
- Validate "koly" magic (0x6B6F6C79) at trailer offset 0
- Validate XML plist offset + length falls within file bounds
- If HFS+ volume header found, validate signature (0x482B = "H+")
- If any step fails, still show available partial info (filename, size, "Apple Disk Image")
  with a warning: "DMG metadata could not be fully read"

---

### macOS Catalog Parser

**Purpose**: Fetch and parse Apple's public software update catalog to discover
available macOS full installer downloads.

**Data Structures**:
```cpp
struct MacOSInstallerInfo {
    // Identity
    QString product_key;        // "072-14059" (Apple's product identifier)
    QString name;               // "macOS Sonoma"
    QString version;            // "14.7.4"
    QString build;              // "23H420"

    // Download
    QUrl download_url;          // URL to InstallAssistant.pkg or full DMG
    qint64 download_size;       // Bytes
    QString integrity_hash;     // Chunklist or digest from catalog
    QString package_type;       // "InstallAssistant.pkg" or "SharedSupport.dmg"

    // Compatibility
    QString min_macos_version;  // Minimum macOS to run the installer (not relevant for USB)
    QStringList supported_board_ids;  // Hardware compatibility (informational)

    // Display
    QString display_version;    // "macOS Sonoma 14.7.4 (23H420)"
    QDate post_date;            // When Apple published this version
};
```

**Catalog URL Pattern**:
```
https://swscan.apple.com/content/catalogs/others/
    index-15-14-13-12-10.16-10.15-10.14-10.13-10.12-10.11-10.10-10.9
    -mountainlion-lion-snowleopard-leopard.merged-1.sucatalog
```

The version chain in the URL extends as Apple releases new major versions.
The catalog is an XML plist containing thousands of software update entries.
Full macOS installers are identified by the presence of `InstallAssistant.pkg`
or specific `SharedSupport.dmg` entries in the package list.

**Implementation**:
```cpp
class MacOSCatalogParser : public QObject {
    Q_OBJECT
public:
    explicit MacOSCatalogParser(QObject* parent = nullptr);

    void fetchCatalog();
    void parseCatalogFromFile(const QString& cached_path);

Q_SIGNALS:
    void catalogReady(QVector<MacOSInstallerInfo> installers);
    void fetchProgress(qint64 bytesReceived, qint64 bytesTotal);
    void errorOccurred(QString error);

private:
    // Download the ~8 MB catalog XML
    void downloadCatalog();

    // Parse XML plist, filter to full installer products only
    QVector<MacOSInstallerInfo> parseSucatalog(const QByteArray& data);

    // Identify full installers by package names and distribution keys
    bool isFullInstaller(const QVariantMap& product);

    // Extract version, build, name from distribution .dist file
    MacOSInstallerInfo parseDistribution(const QString& dist_url,
                                         const QVariantMap& product);
};
```

**Filtering Strategy**:

The sucatalog contains thousands of entries (app updates, security patches, etc.).
Full macOS installers are identified by:
1. Product packages contain `InstallAssistant.pkg` (Big Sur+)
2. Or product contains `SharedSupport.dmg` + `BaseSystem.dmg` (older)
3. The distribution `.dist` file (also linked in catalog) contains
   `<key>SU_TITLE</key>` with "macOS" in the value

**Cache Strategy**:
- Cache the parsed catalog in `temp/macos_catalog_cache.json`
- Cache TTL: 24 hours (Apple updates the catalog infrequently)
- User can force-refresh via button in the dialog

---

### macOS Installer Dialog

**Purpose**: Present discovered macOS installers in a user-friendly selection
dialog, handle download with progress, mirror the UX of `LinuxISODownloadDialog`.

**UI Layout**:
```
┌─────────────────────────────────────────────────────────┐
│  Download macOS Installer                          [X]  │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  Available macOS Versions:                              │
│  ┌─────────────────────────────────────────────────┐   │
│  │ Name              Version   Build    Size       │   │
│  ├─────────────────────────────────────────────────┤   │
│  │ macOS Sequoia     15.4      24E248   13.8 GB    │   │
│  │ macOS Sonoma      14.7.4    23H420   13.2 GB    │   │
│  │ macOS Ventura     13.7.4    22H313   12.1 GB    │   │
│  │ macOS Monterey    12.7.6    21H1320  12.3 GB    │   │
│  └─────────────────────────────────────────────────┘   │
│                                                         │
│  [Refresh Catalog]                                      │
│                                                         │
│  Download Progress:                                     │
│  ████████████████████░░░░░░░░░░  68%                   │
│  8.97 GB / 13.2 GB    45.2 MB/s    ETA: 1m 34s        │
│                                                         │
│  Status: Downloading InstallAssistant.pkg...            │
│                                                         │
│  [Download]  [Cancel]  [Close]                          │
└─────────────────────────────────────────────────────────┘
```

**Implementation**:
```cpp
class MacOSInstallerDialog : public QDialog {
    Q_OBJECT
public:
    explicit MacOSInstallerDialog(QWidget* parent = nullptr);

    QString selectedImagePath() const;

Q_SIGNALS:
    void installerDownloaded(QString dmg_path);

private Q_SLOTS:
    void onCatalogReady(QVector<MacOSInstallerInfo> installers);
    void onDownloadClicked();
    void onCancelClicked();
    void onDownloadProgress(qint64 received, qint64 total, double speed_mbps);
    void onDownloadFinished(QString file_path);
    void onRefreshCatalog();

private:
    MacOSCatalogParser* m_catalog_parser;
    MacOSDownloader* m_downloader;

    QTableWidget* m_version_table;
    QProgressBar* m_progress_bar;
    QLabel* m_status_label;
    QLabel* m_speed_label;
    QPushButton* m_download_button;
    QPushButton* m_cancel_button;
    QPushButton* m_refresh_button;
};
```

---

### macOS Downloader

**Purpose**: Download macOS installer packages via aria2c with multi-connection
support, resume capability, and integrity verification.

**Implementation**:
```cpp
class MacOSDownloader : public QObject {
    Q_OBJECT
public:
    enum class Phase {
        Idle,
        ResolvingVersion,
        Downloading,
        VerifyingChecksum,
        ExtractingPkg,      // If InstallAssistant.pkg, extract DMG from it
        Completed,
        Failed
    };

    explicit MacOSDownloader(QObject* parent = nullptr);

    void download(const MacOSInstallerInfo& installer,
                  const QString& output_dir);
    void cancel();
    Phase currentPhase() const;

Q_SIGNALS:
    void phaseChanged(Phase phase);
    void downloadProgress(qint64 received, qint64 total, double speed_mbps);
    void downloadFinished(QString dmg_path);
    void errorOccurred(QString error);

private:
    void startAria2Download(const QUrl& url, const QString& output_path);
    void verifyChecksum(const QString& file_path, const QString& expected);
    void extractDmgFromPkg(const QString& pkg_path);

    QProcess* m_aria2_process = nullptr;
    std::atomic<bool> m_cancelled{false};
    Phase m_phase = Phase::Idle;
};
```

**PKG Extraction Note**:

Big Sur and later provide `InstallAssistant.pkg` rather than a bare DMG.
This is a flat xar archive containing `SharedSupport.dmg` (the actual bootable
content). The downloader must extract the DMG from the PKG:
- Option A: Use `7z.exe` (already common) to extract xar archives
- Option B: Parse the xar TOC (XML) and extract the payload directly
- Option C: Download `SharedSupport.dmg` directly if the catalog provides
  a separate URL for it (some catalog entries do)

The preferred approach will be determined during implementation based on which
catalog entries are available.

---

### macOS Flash Worker

**Purpose**: Orchestrate the multi-step process of writing a macOS bootable USB
from a DMG file on Windows.

**Flash Pipeline**:
```
Step 1: DMG → Raw IMG (dmg2img.exe)
    Input:  /path/to/InstallMacOS.dmg
    Output: /temp/InstallMacOS.img
    Time:   ~2–5 minutes (depends on DMG compression)

Step 2: Create GPT Partition Layout (diskpart or Windows API)
    ├── Clean the target disk
    ├── Convert to GPT
    ├── Create Partition 1: EFI System (200 MB, FAT32)
    │   Type GUID: C12A7328-F81F-11D2-BA4B-00A0C93EC93B
    │   Formatted FAT32, labeled "EFI"
    ├── Create Partition 2: Apple HFS+
    │   Type GUID: 48465300-0000-11AA-AA11-00306543ECAC
    │   Remaining space, raw (formatted in Step 3)
    └── Write backup GPT at end of disk

Step 3: Format HFS+ Partition (hfsplus.exe)
    hfsplus.exe \\.\PhysicalDriveN:offset:size mkfs "Install macOS Sonoma"

Step 4: Write Installer Content (hfsplus.exe)
    hfsplus.exe \\.\PhysicalDriveN:offset:size addall /temp/extracted_content/

Step 5: Write EFI Boot Files
    Copy boot.efi and related files to EFI partition (standard FAT32 write)

Step 6: Cleanup
    Delete temporary raw IMG and extracted content
```

**Implementation**:
```cpp
class MacOSFlashWorker : public QObject {
    Q_OBJECT
public:
    struct FlashConfig {
        QString dmg_path;           // Source DMG file
        QString physical_drive;     // "\\.\PhysicalDrive3"
        qint64 drive_size_bytes;    // Total drive capacity
        QString temp_dir;           // For intermediate files
        bool verify_after_write;    // SHA verification
    };

    explicit MacOSFlashWorker(QObject* parent = nullptr);

    void flash(const FlashConfig& config);
    void cancel();

Q_SIGNALS:
    void progressUpdated(FlashProgress progress);
    void stepChanged(QString step_description);
    void flashFinished(bool success);
    void errorOccurred(QString error);

private Q_SLOTS:
    void onDmg2ImgFinished(int exit_code);
    void onPartitioningFinished(int exit_code);
    void onHfsPlusFormatFinished(int exit_code);
    void onHfsPlusWriteFinished(int exit_code);

private:
    enum class Step {
        ExtractingDmg,
        Partitioning,
        FormattingHfsPlus,
        WritingContent,
        WritingEfi,
        Verifying,
        Cleanup,
        Done
    };

    void advanceToStep(Step step);
    void extractDmg();
    void createGptLayout();
    void formatHfsPlusPartition();
    void writeInstallerContent();
    void writeEfiPartition();
    void cleanup();

    FlashConfig m_config;
    Step m_current_step = Step::ExtractingDmg;
    QProcess* m_active_process = nullptr;
    std::atomic<bool> m_cancelled{false};

    // Partition layout calculated during Step 2
    qint64 m_efi_partition_offset = 0;
    qint64 m_efi_partition_size = 0;
    qint64 m_hfs_partition_offset = 0;
    qint64 m_hfs_partition_size = 0;
};
```

**GPT Layout Constants**:
```cpp
namespace macos_constants {

// Partition type GUIDs
constexpr char kEfiSystemPartitionGuid[] =
    "C12A7328-F81F-11D2-BA4B-00A0C93EC93B";
constexpr char kAppleHfsPlusGuid[] =
    "48465300-0000-11AA-AA11-00306543ECAC";

// Sizes
constexpr qint64 kEfiPartitionSize = 200 * 1024 * 1024;  // 200 MB
constexpr qint64 kMinUsbDriveSize = 16LL * 1024 * 1024 * 1024;  // 16 GB
constexpr qint64 kGptHeaderSectors = 34;  // LBA 0-33 (MBR + header + entries)
constexpr qint64 kSectorSize = 512;

// DMG extraction
constexpr int kDmg2ImgTimeoutMs = 600000;  // 10 minutes
constexpr int kHfsPlusFormatTimeoutMs = 120000;  // 2 minutes
constexpr int kHfsPlusWriteTimeoutMs = 3600000;  // 60 minutes

// Catalog
constexpr int kCatalogCacheTtlHours = 24;
constexpr int kCatalogDownloadTimeoutMs = 60000;  // 1 minute

}  // namespace macos_constants
```

**Drive Access Strategy**:

On Windows, raw drive access requires administrator privileges (already handled
by SAK's elevation system). The worker will:

1. Use `diskpart` via QProcess for GPT creation (safest, well-tested path):
   ```
   select disk N
   clean
   convert gpt
   create partition efi size=200
   format fs=fat32 quick label="EFI"
   create partition primary
   ```
   Note: diskpart cannot format HFS+ — that's where hfsplus.exe takes over.

2. After diskpart creates the raw partition, use `hfsplus.exe` to format it
   as HFS+ and write the installer content.

3. For the EFI partition, use standard Win32 file APIs (CreateFile on the
   volume, write boot.efi and supporting files).

---

### Bundled Tool Management

**Purpose**: Manage the lifecycle of external GPL tools (check existence, validate
integrity, report missing tools).

**Tools Directory Layout**:
```
tools/
├── dmg2img/
│   ├── dmg2img.exe         (~200 KB)
│   └── LICENSE.txt          (GPL-2.0)
├── hfsplus/
│   ├── hfsplus.exe          (~300 KB)
│   ├── dmg.exe              (~250 KB)
│   └── LICENSE.txt          (GPL-3.0)
├── aria2c/                  (already exists)
│   └── aria2c.exe
├── iperf3/                  (already exists)
│   └── iperf3.exe
└── ...
```

**Build/Bundle Scripts**:

New PowerShell scripts will be added to `scripts/` to build the tools from source:
- `scripts/bundle_dmg2img.ps1` — Clone, build with CMake, copy exe
- `scripts/bundle_hfsplus.ps1` — Clone mozilla/libdmg-hfsplus, build with CMake,
  copy hfsplus.exe and dmg.exe

These mirror the existing `bundle_chocolatey.ps1`, `bundle_iperf3.ps1`, etc.

**Runtime Tool Validation**:
```cpp
class MacOSToolValidator {
public:
    struct ToolStatus {
        bool dmg2img_available;
        bool hfsplus_available;
        bool dmg_tool_available;
        QString dmg2img_path;
        QString hfsplus_path;
        QString dmg_tool_path;
        QString error_message;  // If any tool is missing
    };

    static ToolStatus validateTools();
    static QString findTool(const QString& name);
};
```

The macOS download card and flash worker will check tool availability on startup
and disable themselves with a clear message if tools are missing:
"macOS USB creation requires dmg2img.exe and hfsplus.exe. Run
scripts/bundle_hfsplus.ps1 to build them."

---

## 📋 Implementation Phases

### Phase 1: DMG Analyzer & Image Identification

**Goal**: When a user loads a `.dmg` file, the Image Details panel shows full
metadata — matching the information density of ISO files.

**Files to Create**:
| File | Purpose |
|------|---------|
| `include/sak/dmg_analyzer.h` | DmgInfo struct, DmgAnalyzer static class |
| `src/core/dmg_analyzer.cpp` | UDIF trailer parsing, HFS+ volume header reading, macOS identification |
| `tests/unit/test_dmg_analyzer.cpp` | Unit tests with synthetic DMG headers |

**Files to Modify**:
| File | Change |
|------|--------|
| `src/gui/image_flasher_panel.cpp` | `populateIsoInfo()` — branch on DMG format, call `DmgAnalyzer::analyze()`, populate labels |
| `include/sak/image_source.h` | Add `uncompressedSize` population for DMG in metadata |
| `CMakeLists.txt` | Add new source files |

**Key Implementation Details**:
- `DmgAnalyzer::analyze()` is a static method, thread-safe, no global state
  (same pattern as `IsoAnalyzer::analyze()`)
- Reads only the UDIF trailer (512 bytes at EOF) + XML plist + HFS+ volume
  header (1024 bytes) — does not decompress or mount the image
- Fast: should complete in < 100ms even for 14 GB DMGs (seeks, not sequential read)
- Graceful degradation: if any parsing step fails, returns partial info

**Tests**:
- `testValidMacOSDmg()` — synthetic DMG trailer + HFS+ header → correct OS/version
- `testNonDmgFile()` — ISO file → `is_valid_dmg = false`
- `testCorruptTrailer()` — invalid magic → graceful error, partial info
- `testVersionDetection()` — volume names "Install macOS Sequoia" through
  "Install OS X El Capitan" → correct version mapping
- `testCompressedVsRaw()` — detect compression type from UDIF flags

**Acceptance Criteria**:
- [ ] Loading a macOS DMG shows OS name, version, architecture, format,
      boot type, filesystem, volume label, and creation date
- [ ] Loading a non-DMG file still works exactly as before (no regression)
- [ ] Loading a corrupt/truncated DMG shows partial info + warning
- [ ] All 10 Image Details labels are populated for valid macOS DMGs

---

### Phase 2: macOS Catalog & Download

**Goal**: Users can browse available macOS installer versions and download them
directly, mirroring the Linux ISO download experience.

**Files to Create**:
| File | Purpose |
|------|---------|
| `include/sak/macos_catalog_parser.h` | MacOSInstallerInfo struct, catalog parser |
| `src/core/macos_catalog_parser.cpp` | Sucatalog fetch, XML plist parsing, installer filtering |
| `include/sak/macos_downloader.h` | Download manager with phase state machine |
| `src/core/macos_downloader.cpp` | aria2c integration, checksum verification, PKG extraction |
| `include/sak/macos_installer_dialog.h` | Download dialog UI |
| `src/gui/macos_installer_dialog.cpp` | Version table, progress bar, download/cancel |
| `tests/unit/test_macos_catalog_parser.cpp` | Catalog parsing tests with sample XML |
| `tests/unit/test_macos_downloader.cpp` | Download phase machine tests |

**Files to Modify**:
| File | Change |
|------|--------|
| `src/gui/image_flasher_panel.cpp` | Add 4th download card ("macOS"), wire up dialog |
| `include/sak/image_flasher_panel.h` | Add macOS card member, slot for dialog result |
| `CMakeLists.txt` | Add new source files |

**Key Implementation Details**:
- Catalog is ~8 MB XML plist — download async, parse in background thread
- Cache parsed results in `temp/macos_catalog_cache.json` (24-hour TTL)
- Filter catalog to full installer products only (skip delta updates, security
  patches, Safari updates, etc.)
- Download uses aria2c with 8 connections (large files benefit from parallelism)
- Post-download: verify integrity using chunklist or digest from catalog
- If downloaded file is `.pkg`, extract the DMG payload from it

**Tests**:
- `testParseSucatalog()` — sample XML fragment → correct MacOSInstallerInfo list
- `testFilterFullInstallers()` — mixed catalog → only full installers returned
- `testCatalogCache()` — verify cache write/read/expiry behavior
- `testVersionSorting()` — installers sorted newest-first
- `testPhaseTransitions()` — downloader state machine validation
- `testCancelDuringDownload()` — cancel propagates to aria2c process

**Acceptance Criteria**:
- [ ] macOS download card appears in Image Flasher panel
- [ ] Clicking it opens dialog with available macOS versions
- [ ] Download shows progress, speed, ETA (same quality as Linux dialog)
- [ ] Completed download auto-selects as the active image
- [ ] Downloaded DMG passes through DMG Analyzer → full Image Details shown
- [ ] Cancel works at any point during download
- [ ] Catalog refreshes on demand and caches results

---

### Phase 3: Tool Bundling & Build Infrastructure

**Goal**: Build and bundle `dmg2img.exe` and `hfsplus.exe` from source, with
validation at runtime.

**Files to Create**:
| File | Purpose |
|------|---------|
| `scripts/bundle_dmg2img.ps1` | Clone + build dmg2img from source |
| `scripts/bundle_hfsplus.ps1` | Clone + build mozilla/libdmg-hfsplus from source |
| `include/sak/macos_tool_validator.h` | Tool presence/integrity checking |
| `src/core/macos_tool_validator.cpp` | Locate tools, verify they execute |
| `tools/dmg2img/LICENSE.txt` | GPL-2.0 license text |
| `tools/hfsplus/LICENSE.txt` | GPL-3.0 license text |
| `tests/unit/test_macos_tool_validator.cpp` | Tool validation tests |

**Files to Modify**:
| File | Change |
|------|--------|
| `THIRD_PARTY_LICENSES.md` | Add dmg2img and libdmg-hfsplus entries |
| `CMakeLists.txt` | Add new source files |

**Key Implementation Details**:
- `bundle_hfsplus.ps1` steps:
  1. `git clone https://github.com/mozilla/libdmg-hfsplus.git`
  2. `cmake -B build -G "Visual Studio 17 2022" -A x64`
  3. `cmake --build build --config Release`
  4. Copy `build/Release/hfsplus.exe` and `build/Release/dmg.exe` to `tools/hfsplus/`
- `bundle_dmg2img.ps1` steps:
  1. Clone dmg2img source
  2. Build with MSVC (small C project, zlib dependency)
  3. Copy `dmg2img.exe` to `tools/dmg2img/`
- `MacOSToolValidator::validateTools()` checks:
  - File exists at expected path
  - File executes without error (`--version` or `--help` flag)
  - Returns structured status for UI to display

**Tests**:
- `testToolPathResolution()` — finds tools in expected locations
- `testMissingToolDetection()` — graceful error when tools absent
- `testToolExecution()` — validates tools respond to version check

**Acceptance Criteria**:
- [ ] `bundle_hfsplus.ps1` builds both `hfsplus.exe` and `dmg.exe` from source
- [ ] `bundle_dmg2img.ps1` builds `dmg2img.exe` from source
- [ ] Tools are placed in `tools/` subdirectories with license files
- [ ] `THIRD_PARTY_LICENSES.md` updated with proper attribution
- [ ] Runtime validation reports missing tools with actionable message
- [ ] macOS features gracefully disable when tools are missing

---

### Phase 4: macOS Bootable USB Flash Worker

**Goal**: Write a fully-offline bootable macOS installer USB from a DMG file,
using the bundled tools to handle GPT partitioning and HFS+ filesystem creation.

**Files to Create**:
| File | Purpose |
|------|---------|
| `include/sak/macos_flash_worker.h` | Flash worker with step-based pipeline |
| `src/threading/macos_flash_worker.cpp` | DMG extraction, GPT layout, HFS+ write |
| `include/sak/macos_constants.h` | GUIDs, sizes, timeouts for macOS operations |
| `tests/unit/test_macos_flash_worker.cpp` | Pipeline step tests, cancellation tests |

**Files to Modify**:
| File | Change |
|------|--------|
| `src/core/flash_coordinator.cpp` | Route DMG images to MacOSFlashWorker instead of generic FlashWorker |
| `include/sak/flash_coordinator.h` | Add DMG routing logic |
| `CMakeLists.txt` | Add new source files |

**Key Implementation Details**:
- Uses `diskpart` via QProcess for GPT creation (well-tested, admin path)
- Uses `hfsplus.exe` via QProcess for HFS+ format and file injection
- Uses `dmg2img.exe` via QProcess for DMG → raw conversion
- Each step emits progress signals compatible with existing FlashProgress struct
- Cancellation checks between each step (all steps are discrete QProcess calls)
- Temp directory for intermediate raw IMG cleaned up on success or failure
- Requires admin elevation (already handled by SAK's existing elevation system)

**Pipeline Error Handling**:
- `dmg2img` fails → report "DMG extraction failed" + stderr output
- `diskpart` fails → report partition error, drive may be in unknown state
- `hfsplus` fails → report HFS+ error + specific step that failed
- Timeout on any step → kill process, report timeout, clean up
- Cancel at any step → kill active QProcess, clean temp files

**Tests**:
- `testFlashConfigValidation()` — invalid drive path, missing DMG, drive too small
- `testStepProgression()` — mock QProcess outputs, verify state machine advances
- `testCancelDuringExtraction()` — cancel while dmg2img running
- `testCancelDuringPartitioning()` — cancel while diskpart running
- `testTempFileCleanup()` — verify intermediate files removed after flash
- `testMinimumDriveSize()` — reject drives < 16 GB for macOS

**Acceptance Criteria**:
- [ ] DMG file → fully bootable macOS USB in a single click
- [ ] Progress visible at each pipeline step
- [ ] Cancel works at any point, cleans up temp files
- [ ] Drives < 16 GB rejected with clear message
- [ ] Flash Coordinator automatically routes DMG to MacOSFlashWorker
- [ ] Existing ISO/IMG flash paths unaffected (no regression)

---

### Phase 5: Integration & Polish

**Goal**: End-to-end workflow testing, UI polish, edge case handling.

**Tasks**:
| Task | Details |
|------|---------|
| End-to-end flow test | Catalog → download → analyze → flash → boot on Mac |
| Error message polish | User-friendly messages for every failure path |
| Download resume | Verify aria2c resume works for interrupted 13 GB downloads |
| Drag-and-drop DMG | Verify DMG files dropped on panel trigger analyzer |
| Multi-drive flash | Test flashing same DMG to multiple USBs (if feasible) |
| Dark theme | Verify macOS dialog and card render correctly in dark mode |
| Documentation | Update README, add macOS section to user guide |
| Keyboard shortcuts | Ensure dialog is fully keyboard-navigable |

**Files to Modify**:
| File | Change |
|------|--------|
| `README.md` | Add macOS bootable USB to feature list |
| `CHANGELOG.md` | Document new feature |
| `resources/icons/` | Add macOS/Apple icon for download card |

**Acceptance Criteria**:
- [ ] Complete download → flash → boot cycle verified on real hardware
- [ ] All new code compiles with zero warnings (`/W4 /WX /permissive- /sdl`)
- [ ] All existing tests still pass (no regression)
- [ ] All new tests pass (36 tests across 5 test files)
- [ ] `pre-commit run --all-files` passes cleanly (all 10 hooks)
- [ ] No function exceeds CCN 10 or 5 parameters (Lizard)
- [ ] clang-format produces zero diffs on all new `.cpp`/`.h` files
- [ ] cppcheck reports zero findings on all new source files
- [ ] UI is consistent with existing Linux/Windows download cards

---

## 📁 File Inventory Summary

### New Files (20 files)

| File | Type |
|------|------|
| `include/sak/dmg_analyzer.h` | Header |
| `src/core/dmg_analyzer.cpp` | Source |
| `include/sak/macos_catalog_parser.h` | Header |
| `src/core/macos_catalog_parser.cpp` | Source |
| `include/sak/macos_downloader.h` | Header |
| `src/core/macos_downloader.cpp` | Source |
| `include/sak/macos_installer_dialog.h` | Header |
| `src/gui/macos_installer_dialog.cpp` | Source |
| `include/sak/macos_tool_validator.h` | Header |
| `src/core/macos_tool_validator.cpp` | Source |
| `include/sak/macos_flash_worker.h` | Header |
| `src/threading/macos_flash_worker.cpp` | Source |
| `include/sak/macos_constants.h` | Header |
| `scripts/bundle_dmg2img.ps1` | Build script |
| `scripts/bundle_hfsplus.ps1` | Build script |
| `tests/unit/test_dmg_analyzer.cpp` | Test (10 tests) |
| `tests/unit/test_macos_catalog_parser.cpp` | Test (8 tests) |
| `tests/unit/test_macos_downloader.cpp` | Test (6 tests) |
| `tests/unit/test_macos_tool_validator.cpp` | Test (4 tests) |
| `tests/unit/test_macos_flash_worker.cpp` | Test (8 tests) |

### Modified Files (8 files)

| File | Change |
|------|--------|
| `src/gui/image_flasher_panel.cpp` | Add macOS card, DMG analyzer branch in populateIsoInfo() |
| `include/sak/image_flasher_panel.h` | Add macOS card member, macOS dialog slot |
| `src/core/flash_coordinator.cpp` | Route DMG format to MacOSFlashWorker |
| `include/sak/flash_coordinator.h` | Add DMG routing |
| `include/sak/image_source.h` | Enhance DMG metadata population |
| `CMakeLists.txt` | Add all new source/header/test files |
| `THIRD_PARTY_LICENSES.md` | Add dmg2img + libdmg-hfsplus attribution |
| `README.md` | Add macOS feature documentation |

---

## ✅ Code Quality Gates & Pre-Commit Requirements

All new code in this feature **must** pass every quality gate on the first commit
attempt. This section documents the exact requirements so implementation gets it
right the first time — no rework loops.

### Hard Gates (Block Commits — Zero Tolerance)

| Gate | Tool | Threshold | How It's Enforced |
|------|------|-----------|-------------------|
| **Build warnings** | MSVC (`/W4 /WX /permissive- /sdl`) | **0 warnings** | Compiler rejects build |
| **Code formatting** | clang-format (`--dry-run -Werror`) | 100-col limit, Google style | Pre-commit hook |
| **Cyclomatic complexity** | Lizard | **CCN ≤ 10** per function | Pre-commit hook blocks |
| **Function parameters** | Lizard | **≤ 5 parameters** per function | Pre-commit hook blocks |
| **Static analysis** | cppcheck (`--enable=all --check-level=exhaustive --std=c++23`) | All findings resolved or suppressed | Pre-commit hook blocks |
| **All tests pass** | CTest (Qt Test) | **100% pass rate** | CI gate + pre-commit |
| **Trailing whitespace** | pre-commit built-in | None allowed | Pre-commit hook |
| **End-of-file newline** | pre-commit built-in | Required | Pre-commit hook |
| **Large files** | pre-commit built-in | **< 4 KB** per added file | Pre-commit hook |

### Soft Guidelines (Advisory — Strive For, Don't Block)

| Guideline | Threshold | Notes |
|-----------|-----------|-------|
| Function length | ≤ 70 lines | Lizard prints warning, does NOT block |
| Nesting depth | ≤ 3 levels | TigerStyle best practice |
| Named constants | No magic numbers | 0, 1, −1 are acceptable bare literals |
| Single-letter variables | Avoid | Except tiny lambda predicates |

### Pre-Commit Hook Pipeline (`.pre-commit-config.yaml`)

Runs with `fail_fast: true` — stops on first failure. All 10 hooks:

1. `trailing-whitespace` — strip trailing spaces
2. `end-of-file-fixer` — ensure newline at EOF
3. `check-yaml` — valid YAML syntax
4. `check-json` — valid JSON syntax
5. `check-added-large-files` — reject files > 4 KB
6. `check-merge-conflict` — detect `<<<<<<<` markers
7. `check-case-conflict` — filename case collisions
8. `clang-format` — `clang-format.exe --dry-run -Werror` on all `.cpp`/`.h` files
9. `lizard-complexity` — `python scripts/run_lizard.py` (CCN ≤ 10, params ≤ 5)
10. `cppcheck` — `powershell scripts/run_cppcheck.ps1` (exhaustive analysis)

### Coding Constraints for Implementation

Every function in new source files must be written to satisfy these constraints
from the start. Key design implications:

**CCN ≤ 10 (cyclomatic complexity)**:
- `DmgAnalyzer::analyze()` must NOT be a single monolithic function. Split into
  discrete steps: `readUdifTrailer()`, `parseResourceFork()`,
  `readHfsPlusVolumeHeader()`, `identifyMacOS()` — each with CCN ≤ 10.
- `MacOSCatalogParser::parseSucatalog()` must delegate filtering and version
  extraction to helper functions, not inline all logic.
- `MacOSFlashWorker` step methods (`extractDmg()`, `createGptLayout()`,
  `formatHfsPlusPartition()`, etc.) are already separate — keep them that way.
- Pattern: if a function has > 3 `if/else` branches or a `switch` with > 8 cases,
  extract a helper or use a lookup table.

**≤ 5 parameters per function**:
- Use config structs (`FlashConfig`, `PingConfig` pattern) instead of long
  parameter lists. The plan already does this for `MacOSFlashWorker::FlashConfig`.
- `DmgAnalyzer::analyze()` takes only 1 parameter (file path).
- `MacOSDownloader::download()` takes 2 parameters (installer info + output dir).
- If a helper needs more context, pass a struct or `this` pointer.

**100-character line limit**:
- Break long string literals with `QStringLiteral()` concatenation.
- Break long call chains across lines.
- Use `const auto&` for long type names.

**clang-format include ordering**:
```cpp
// 1. Corresponding header
#include "sak/dmg_analyzer.h"
// 2. Project headers
#include "sak/macos_constants.h"
// 3. Qt headers
#include <QFile>
#include <QXmlStreamReader>
// 4. C++ STL headers
#include <array>
#include <cstdint>
// 5. Windows headers (if needed)
#include <windows.h>
```

**cppcheck compliance**:
- Use `Q_ASSERT()` for debug-mode preconditions, not raw `assert()`.
- Use `static_cast<>` not C-style casts.
- Initialize all member variables in constructors or class declaration.
- Check all `QProcess::waitFor*()` return values.
- Every `QFile::open()` result must be checked.

**MSVC `/W4 /WX` compliance**:
- No unused variables or parameters — use `[[maybe_unused]]` if needed.
- No signed/unsigned comparison — use explicit casts or matching types.
- No unreachable code after `return`.
- No implicit narrowing conversions — use `static_cast` or `gsl::narrow_cast`.
- No missing `default:` in `switch` on enums.

### clang-tidy Checks (Optional but Recommended)

If `ENABLE_CLANG_TIDY=ON` is set, all warnings become errors. Key checks:
- `bugprone-*` — dangling references, incorrect round-of-half, etc.
- `modernize-*` — use `auto`, range-for, `nullptr`, `override`
- `performance-*` — move semantics, unnecessary copies
- `readability-*` — naming conventions, redundant control flow
- `concurrency-*` — thread safety
- 10 checks disabled for Qt compatibility (e.g., `modernize-use-trailing-return-type`)

---

## 🧪 Comprehensive Test Requirements

Every phase includes tests. Every public function gets coverage. This section
defines the complete test matrix.

### Test File Inventory

| Test File | Tests | Phase |
|-----------|-------|-------|
| `tests/unit/test_dmg_analyzer.cpp` | 10 tests | Phase 1 |
| `tests/unit/test_macos_catalog_parser.cpp` | 8 tests | Phase 2 |
| `tests/unit/test_macos_downloader.cpp` | 6 tests | Phase 2 |
| `tests/unit/test_macos_tool_validator.cpp` | 4 tests | Phase 3 |
| `tests/unit/test_macos_flash_worker.cpp` | 8 tests | Phase 4 |
| **Total** | **36 tests** | |

### Phase 1 Tests: DMG Analyzer (`test_dmg_analyzer.cpp`)

```cpp
class TestDmgAnalyzer : public QObject {
    Q_OBJECT
private Q_SLOTS:
    // Happy path
    void testValidMacOSDmg();          // Synthetic UDIF trailer + HFS+ header
                                        // → correct os_name, version, volume_label
    void testUncompressedDmg();         // Raw (uncompressed) DMG → correct sizes
    void testZlibCompressedDmg();       // zlib-compressed DMG → compression_type "zlib"

    // macOS version identification
    void testVersionDetection_data();   // Data-driven: name→version mapping
    void testVersionDetection();        // "Install macOS Sequoia" → 15.x,
                                        // through "Install OS X El Capitan" → 10.11

    // Architecture detection
    void testArchitectureDetection();   // Big Sur+ → "Universal", pre-Big Sur → "x86_64"

    // Error/edge cases
    void testNonDmgFile();              // ISO file → is_valid_dmg == false
    void testCorruptUdifTrailer();      // Bad magic → graceful error, partial info
    void testTruncatedFile();           // File < 512 bytes → is_valid_dmg == false
    void testEmptyFile();               // 0-byte file → is_valid_dmg == false
};
```

**Test Data Strategy**: Create synthetic binary blobs in test setup with valid
UDIF trailers and HFS+ volume headers. No real DMG files needed (avoids 12 GB
test fixtures and copyright concerns).

### Phase 2 Tests: Catalog Parser (`test_macos_catalog_parser.cpp`)

```cpp
class TestMacOSCatalogParser : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void testParseSucatalog();          // Sample XML → correct installer list
    void testFilterFullInstallers();    // Mixed catalog → only full installers
    void testFilterDeltaUpdates();      // Delta updates excluded
    void testVersionSorting();          // Newest version first
    void testEmptyCatalog();            // Empty XML → empty list, no crash
    void testMalformedXml();            // Invalid XML → error signal, no crash
    void testCatalogCacheWrite();       // Parsed results → JSON cache file
    void testCatalogCacheExpiry();      // Cache older than 24h → re-fetch
};
```

### Phase 2 Tests: Downloader (`test_macos_downloader.cpp`)

```cpp
class TestMacOSDownloader : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void testPhaseTransitions();        // Idle → Downloading → Verifying → Completed
    void testCancelDuringIdle();        // Cancel before start → no-op
    void testCancelDuringDownload();    // Cancel → aria2c process killed
    void testInvalidUrl();              // Bad URL → Failed phase + error signal
    void testMissingAria2c();           // aria2c not found → error signal
    void testChecksumMismatch();        // Bad checksum → Failed phase + error
};
```

### Phase 3 Tests: Tool Validator (`test_macos_tool_validator.cpp`)

```cpp
class TestMacOSToolValidator : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void testToolPathResolution();      // Tools in expected locations → found
    void testMissingToolDetection();    // Absent tool → clear error message
    void testPortableMode();            // Portable mode → tools relative to exe
    void testInstalledMode();            // Installed → tools in program dir
};
```

### Phase 4 Tests: Flash Worker (`test_macos_flash_worker.cpp`)

```cpp
class TestMacOSFlashWorker : public QObject {
    Q_OBJECT
private Q_SLOTS:
    // Config validation
    void testValidConfig();             // Valid config → accepted
    void testMissingDmgPath();          // Empty DMG path → rejected
    void testDriveTooSmall();           // < 16 GB → rejected with message
    void testInvalidDrivePath();        // Bad drive path → rejected

    // Pipeline steps (mock QProcess)
    void testStepProgression();         // Each step completes → advances to next
    void testCancelDuringExtraction();  // Cancel mid-dmg2img → process killed
    void testCancelDuringPartitioning();// Cancel mid-diskpart → process killed
    void testTempFileCleanup();         // After flash → temp files deleted
};
```

### Test Compliance with Quality Gates

All test files must also pass pre-commit hooks:
- Test functions themselves must have CCN ≤ 10
- Data-driven tests use `_data()` / `QFETCH` pattern to avoid large test functions
- Test helper functions keep parameter count ≤ 5
- Synthetic binary blob creation extracted into helper functions (e.g.,
  `createSyntheticUdifTrailer()`, `createSyntheticHfsPlusHeader()`)
- All tests formatted with clang-format before commit
- `QCOMPARE`, `QVERIFY`, `QVERIFY2` preferred over raw assertions

### Running Tests

```powershell
# Build
cmake --build build --config Release

# Run all tests (existing + new)
ctest --test-dir build -C Release --output-on-failure

# Run only macOS-related tests
ctest --test-dir build -C Release -R "macos|dmg" --output-on-failure

# Run single test
.\build\Release\test_dmg_analyzer.exe -v2
```

---

## ⚠️ Risks & Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Apple changes sucatalog URL/format | Catalog parsing breaks | Version-chain URL is stable for 10+ years; plist format unchanged. gibMacOS (7k stars) depends on it. Monitor for changes. |
| libdmg-hfsplus doesn't build on MSVC | No HFS+ tooling | Library uses standard C with CMake. mozilla fork actively maintained (last release April 2025). Fallback: cross-compile with MinGW. |
| UDIF DMG format variations | Analyzer fails on some DMGs | Handle known compression types (zlib, bzip2, lzfse, raw). Graceful degradation for unknown types. |
| Large download sizes (~13 GB) | Slow/failed downloads | aria2c handles resume natively. Multi-connection download. Progress saving. |
| HFS+ write corruption | Non-bootable USB | hfsplus.exe is "well-proven" per mozilla docs. Verify with read-back after write. Test on real Mac hardware. |
| diskpart requires elevation | Flash fails without admin | SAK already has elevation handling. Validate elevation before starting flash pipeline. |
| Big Sur+ PKG wrapping | Extra extraction step | Detect PKG vs bare DMG. Extract DMG from PKG using xar/7z. |
| Temp disk space (~28 GB) | Extraction fails | Check available temp space before starting (DMG + raw IMG). Warn user if insufficient. |

---

## 🔑 Key Technical Decisions

1. **Bundle tools as standalone .exe** — Avoids GPL license contamination. Same
   proven pattern as aria2c and iperf3.

2. **DMG Analyzer reads binary headers directly** — No external tool dependency
   for identification. Pure C++ reading UDIF trailer and HFS+ volume header.
   Fast (< 100ms) and works even if dmg2img/hfsplus aren't installed.

3. **diskpart for GPT creation** — More reliable than raw sector writes for
   partition table creation. Well-tested on all Windows versions. Already
   requires the same admin elevation SAK uses.

4. **Separate MacOSFlashWorker** — The existing FlashWorker does raw sector
   copies. macOS requires a multi-tool pipeline (dmg2img → diskpart → hfsplus).
   Separate class keeps responsibilities clear.

5. **Catalog caching** — The sucatalog is ~8 MB and changes infrequently.
   24-hour cache avoids unnecessary re-downloads.

6. **APFS not required** — All macOS installer USB media uses HFS+. APFS is
   only used by the macOS installer itself when writing to the target Mac's
   internal drive. This simplifies the toolchain significantly.

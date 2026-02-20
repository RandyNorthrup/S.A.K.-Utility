# Linux ISO Downloader — Implementation Plan

## Overview

Add a "Download Linux ISO" capability to the Image Flasher panel, complementing
the existing "Download Windows 11" feature. Targets IT technicians who need
bootable USB drives for diagnostics, recovery, disk wiping, partitioning, and
general-purpose Linux installations.

## Distro Catalog

| Distro | Category | Source Type | Download Pattern | Checksum |
|---|---|---|---|---|
| Ubuntu Desktop LTS | General Purpose | Direct URL | `https://releases.ubuntu.com/{codename}/ubuntu-{ver}-desktop-amd64.iso` | SHA256SUMS file in same directory |
| Ubuntu Server LTS | General Purpose | Direct URL | `https://releases.ubuntu.com/{codename}/ubuntu-{ver}-live-server-amd64.iso` | SHA256SUMS file in same directory |
| Linux Mint Cinnamon | General Purpose | Mirror URL | `https://mirrors.kernel.org/linuxmint/stable/{ver}/linuxmint-{ver}-cinnamon-64bit.iso` | `sha256sum.txt` in same directory |
| Kali Linux | Security/Pen-Test | Direct URL | `https://cdimage.kali.org/kali-{ver}/kali-linux-{ver}-installer-amd64.iso` | SHA256SUMS in same directory |
| SystemRescue | System Recovery | Direct URL | `https://fastly-cdn.system-rescue.org/systemrescue-{ver}-amd64.iso` | `.sha256` sidecar file |
| Clonezilla Live | Disk Cloning | SourceForge | `https://sourceforge.net/projects/clonezilla/files/clonezilla_live_stable/{ver}/clonezilla-live-{ver}-amd64.iso/download` | Checksums on download page |
| GParted Live | Partitioning | SourceForge | `https://sourceforge.net/projects/gparted/files/gparted-live-stable/{ver}/gparted-live-{ver}-amd64.iso/download` | Checksums on download page |
| ShredOS | Secure Erase | GitHub Releases | `https://api.github.com/repos/PartialVolume/shredos.x86_64/releases/latest` → parse `.iso` asset | SHA1 sidecar in release assets |
| Ventoy | Multi-Boot USB | GitHub Releases | `https://api.github.com/repos/ventoy/Ventoy/releases/latest` → `ventoy-{ver}-livecd.iso` | SHA256 in release body |
| Memtest86+ | Memory Test | GitHub Releases | `https://api.github.com/repos/memtest86plus/memtest86plus/releases/latest` → `.iso` asset | SHA256 in release assets |

## Architecture

### Pattern: Matches existing Windows ISO downloader

```
LinuxDistroCatalog  ─→  LinuxISODownloader  ─→  LinuxISODownloadDialog
  (data + version        (orchestrator:           (Qt wizard dialog:
   discovery via          download via aria2c,      pick distro/variant,
   GitHub API /           verify checksum,          save location,
   URL templates)         report progress)          progress display)
```

### New Files

| File | Type | Purpose |
|---|---|---|
| `include/sak/linux_distro_catalog.h` | Header | Distro metadata structs, catalog with URL patterns, GitHub API version checking |
| `src/core/linux_distro_catalog.cpp` | Impl | Catalog population, GitHub API calls for latest versions, URL generation |
| `include/sak/linux_iso_downloader.h` | Header | Orchestrator: catalog → aria2c download → SHA256 verify |
| `src/core/linux_iso_downloader.cpp` | Impl | Download pipeline with progress reporting |
| `include/sak/linux_iso_download_dialog.h` | Header | Dialog UI: distro picker, variant selector, progress |
| `src/gui/linux_iso_download_dialog.cpp` | Impl | Full wizard dialog implementation |

### Modified Files

| File | Change |
|---|---|
| `include/sak/image_flasher_panel.h` | Add `m_downloadLinuxButton`, `onDownloadLinuxClicked()`, forward-declare `LinuxISODownloader` |
| `src/gui/image_flasher_panel.cpp` | Add button to Step 1 page, wire click handler, create downloader |
| `CMakeLists.txt` | Add 4 new source files to `CORE_SOURCES` and `GUI_SOURCES` |

## Download Engine

Uses bundled `aria2c` (already available in `tools/uup/aria2c.exe`) for
high-speed multi-connection downloads. Unlike the Windows UUP flow which downloads
dozens of small files and then converts them, Linux ISOs are single large files
downloaded directly.

### aria2c Parameters for Single-File ISO Download

```
--max-connection-per-server=16
--split=16
--min-split-size=1M
--file-allocation=none
--disk-cache=64M
--connect-timeout=10
--timeout=60
--max-tries=5
--retry-wait=3
--lowest-speed-limit=50K
--check-certificate=true
--auto-file-renaming=false
--allow-overwrite=true
--console-log-level=error
--summary-interval=1
```

### SHA256 Verification

After download completes, the orchestrator:
1. Fetches the checksum file from the distro's checksum URL
2. Parses the expected SHA256 for the downloaded filename
3. Computes SHA256 of the downloaded file using `QCryptographicHash`
4. Compares and reports pass/fail

## Version Discovery Strategy

### GitHub-hosted distros (ShredOS, Ventoy, Memtest86+)

1. `GET https://api.github.com/repos/{owner}/{repo}/releases/latest`
2. Parse `tag_name` for version
3. Parse `assets[]` for `.iso` download URL and size
4. Rate limit: GitHub allows 60 requests/hour unauthenticated — sufficient

### Direct URL distros (Ubuntu, Kali, SystemRescue)

Ship with known-good latest versions in the catalog. The catalog struct includes
a `latestKnownVersion` field. Users see the version; no dynamic scraping needed
for these as the URL patterns are stable.

### SourceForge distros (Clonezilla, GParted)

SourceForge redirects to mirrors automatically. Ship with known-good versions.
The `/download` suffix on SourceForge URLs triggers the mirror redirect.

## UI Design

### LinuxISODownloadDialog Layout

```
┌─ Download Linux ISO ──────────────────────────────────────┐
│                                                            │
│  ┌─ Select Distribution ────────────────────────────────┐  │
│  │ Category: [All ▼]                                    │  │
│  │ ┌──────────────────────────────────────────────────┐  │  │
│  │ │ 🖥 Ubuntu Desktop 24.04.4 LTS (Noble Numbat)    │  │  │
│  │ │ 🖧 Ubuntu Server 24.04.4 LTS                    │  │  │
│  │ │ 🌿 Linux Mint 22.3 Cinnamon                     │  │  │
│  │ │ 🔒 Kali Linux 2025.4                            │  │  │
│  │ │ 🔧 SystemRescue 12.03                           │  │  │
│  │ │ 💾 Clonezilla Live 3.3.0-33                     │  │  │
│  │ │ 📊 GParted Live 1.8.0-2                         │  │  │
│  │ │ 🗑 ShredOS v2025.11 (nwipe 0.40)                │  │  │
│  │ │ 📀 Ventoy 1.1.10 LiveCD                         │  │  │
│  │ │ 🧪 Memtest86+                                   │  │  │
│  │ └──────────────────────────────────────────────────┘  │  │
│  │ Description: [selected distro description here]      │  │
│  └──────────────────────────────────────────────────────┘  │
│                                                            │
│  ┌─ Save Location ─────────────────────────────────────┐  │
│  │ [C:\Users\...\Downloads\ubuntu-24.04.4...]  [Browse]│  │
│  └──────────────────────────────────────────────────────┘  │
│                                                            │
│  ┌─ Progress ───────────────────────────────────────────┐  │
│  │ Status: Downloading ubuntu-24.04.4-desktop...       │  │
│  │ Phase:  Downloading ISO                              │  │
│  │ [████████████████████░░░░░░░░░░░░░░░░] 52%          │  │
│  │ 3.2 GB / 6.2 GB                         45.2 MB/s  │  │
│  └──────────────────────────────────────────────────────┘  │
│                                                            │
│                    [Download]  [Cancel]  [Close]           │
└────────────────────────────────────────────────────────────┘
```

### Category Filter Options

- All
- General Purpose (Ubuntu, Linux Mint)
- Security & Pen-Testing (Kali Linux)
- System Recovery (SystemRescue)
- Disk Tools (Clonezilla, GParted, ShredOS)
- Utilities (Ventoy, Memtest86+)

## Error Handling

- Network timeout: Retry with exponential backoff (handled by aria2c `--max-tries`)
- SHA256 mismatch: Delete file, show error with option to re-download
- GitHub API rate limit: Fall back to hardcoded version URLs
- SourceForge mirror failure: aria2c retries automatically
- Disk full: Detect via aria2c exit code, report to user
- Cancel: Kill aria2c process, clean up partial `.aria2` control files

## Thread Safety

- All network operations run in QProcess (aria2c) — non-blocking
- SHA256 verification runs in `QtConcurrent::run()` off the UI thread
- Progress polling via QTimer (1-second interval, same as Windows ISO downloader)
- All UI updates via Qt signal/slot (thread-safe by design)

## Testing

The existing test infrastructure can be extended. Key scenarios:
- Catalog population and URL generation (unit test)
- GitHub API response parsing (unit test with mock JSON)
- aria2c argument construction (unit test)
- SHA256 verification (unit test with known hash)
- Full download flow (integration test, network required)

## Estimated Scope

- `linux_distro_catalog.h/cpp`: ~300 lines
- `linux_iso_downloader.h/cpp`: ~400 lines
- `linux_iso_download_dialog.h/cpp`: ~500 lines
- `image_flasher_panel` modifications: ~30 lines
- `CMakeLists.txt` modifications: ~6 lines
- **Total: ~1,230 lines**


# OS Downloader Implementation Checklist

This document provides a detailed implementation plan for adding Windows 10, Ubuntu, Debian, Raspberry Pi OS, and Linux Mint ISO downloaders to the Image Flasher Panel.

## Overview

Currently implemented: **Windows 11 ISO Downloader** only  
To be implemented: 5 additional OS downloaders

## Priority Order (Easiest to Hardest)

1. **Ubuntu** - Simple direct download links, official JSON API
2. **Linux Mint** - Direct download links from website
3. **Raspberry Pi OS** - Simple download structure
4. **Debian** - Multiple mirrors, more complex
5. **Windows 10** - Similar to Windows 11 but different API endpoints

---

## 1. Ubuntu Desktop/Server Downloader

**Difficulty**: ⭐⭐ (Easy - Direct downloads, JSON API available)  
**Priority**: HIGH  
**Estimated Time**: 4-6 hours

### Research Phase
- [ ] Test Ubuntu releases API endpoint: `http://releases.ubuntu.com/`
- [ ] Test direct download patterns: `http://releases.ubuntu.com/{version}/ubuntu-{version}-desktop-amd64.iso`
- [ ] Verify LTS versions available: 24.04, 22.04, 20.04
- [ ] Check for SHA256SUMS files for verification
- [ ] Test download mirrors for reliability

### Architecture Design
- [ ] Create `UbuntuISODownloader` class inheriting from `QObject`
- [ ] Design similar API to `WindowsISODownloader` for consistency
- [ ] Define `UbuntuEdition` enum: `Desktop`, `Server`, `DesktopMinimal`
- [ ] Define `UbuntuArchitecture` enum: `AMD64`, `ARM64`
- [ ] Plan signal/slot interface matching existing pattern

### Header File: `include/sak/ubuntu_iso_downloader.h`
- [ ] Class declaration with QObject inheritance
- [ ] Public methods:
  - [ ] `void fetchAvailableReleases()` - Get LTS and current releases
  - [ ] `void requestDownloadInfo(QString version, UbuntuEdition edition, UbuntuArchitecture arch)` - Get download URL and checksum
  - [ ] `void downloadISO(QString url, QString savePath)` - Download with progress
  - [ ] `void cancel()` - Cancel operation
  - [ ] `QStringList getAvailableReleases() const` - Return cached releases
  - [ ] `QStringList getAvailableEditions() const` - Return editions
  - [ ] `QStringList getAvailableArchitectures() const` - Return architectures
- [ ] Signals:
  - [ ] `void releasesFetched(QStringList releases)` - Available versions
  - [ ] `void downloadInfoReceived(QString url, QString checksum)` - Download details
  - [ ] `void downloadProgress(qint64 received, qint64 total, double speedMBps)` - Progress updates
  - [ ] `void downloadComplete(QString filePath, qint64 fileSize)` - Success
  - [ ] `void downloadError(QString error)` - Error occurred
  - [ ] `void statusMessage(QString message)` - Status updates
  - [ ] `void checksumVerified(bool valid)` - Checksum verification result
- [ ] Private members:
  - [ ] `QNetworkAccessManager* m_networkManager`
  - [ ] `QNetworkReply* m_currentReply`
  - [ ] `QFile* m_downloadFile`
  - [ ] `QStringList m_availableReleases`
  - [ ] `QString m_expectedChecksum`
  - [ ] Progress tracking variables
- [ ] Private methods:
  - [ ] `QStringList parseReleases(QString html)` - Parse releases page
  - [ ] `QString buildDownloadUrl(QString version, UbuntuEdition, UbuntuArchitecture)` - Construct URL
  - [ ] `void verifyChecksum(QString filePath)` - SHA256 verification
  - [ ] Network slot handlers

### Implementation File: `src/core/ubuntu_iso_downloader.cpp`
- [ ] Constructor: Initialize network manager
- [ ] `fetchAvailableReleases()`:
  - [ ] Fetch `http://releases.ubuntu.com/`
  - [ ] Parse HTML for version directories (24.04, 22.04, 20.04, etc.)
  - [ ] Filter for LTS releases and latest stable
  - [ ] Emit `releasesFetched` signal
- [ ] `requestDownloadInfo()`:
  - [ ] Build download URL: `http://releases.ubuntu.com/{version}/ubuntu-{version}-{edition}-{arch}.iso`
  - [ ] Fetch SHA256SUMS file from same directory
  - [ ] Parse checksum for the specific ISO
  - [ ] Emit `downloadInfoReceived` signal
- [ ] `downloadISO()`:
  - [ ] Create QNetworkRequest with URL
  - [ ] Open QFile for writing
  - [ ] Connect progress signals
  - [ ] Start download
  - [ ] Track speed and ETA
- [ ] `verifyChecksum()`:
  - [ ] Calculate SHA256 of downloaded file
  - [ ] Compare with expected checksum
  - [ ] Emit `checksumVerified` signal
- [ ] Slot handlers for network events
- [ ] Error handling and retry logic

### UI Integration in `image_flasher_panel.cpp`
- [ ] Add "Download Ubuntu" button to image selection page
- [ ] Create Ubuntu download dialog:
  - [ ] Version dropdown (LTS: 24.04, 22.04, 20.04 + latest)
  - [ ] Edition dropdown (Desktop, Server, Desktop Minimal)
  - [ ] Architecture dropdown (AMD64, ARM64)
  - [ ] Download location selector
  - [ ] Progress bar
  - [ ] Status label
  - [ ] Cancel button
- [ ] Connect signals:
  - [ ] `releasesFetched` → Populate version dropdown
  - [ ] `downloadProgress` → Update progress bar
  - [ ] `downloadComplete` → Close dialog, load ISO
  - [ ] `downloadError` → Show error message
  - [ ] `checksumVerified` → Show verification result
- [ ] Connect button click to show dialog
- [ ] Handle download completion (auto-select downloaded ISO)

### Testing
- [ ] Unit tests for URL construction
- [ ] Test with Ubuntu 24.04 LTS Desktop AMD64
- [ ] Test with Ubuntu 22.04 LTS Server AMD64
- [ ] Test checksum verification
- [ ] Test download cancellation
- [ ] Test network error handling
- [ ] Test download resume (if implemented)
- [ ] Integration test: Full download → Flash workflow

### Documentation
- [ ] Add class documentation to header
- [ ] Document Ubuntu releases structure
- [ ] Add example usage to class comments
- [ ] Update IMAGE_FLASHER_PANEL_PLAN.md to mark as complete

---

## 2. Linux Mint Downloader

**Difficulty**: ⭐⭐ (Easy - Direct downloads from website)  
**Priority**: MEDIUM  
**Estimated Time**: 3-5 hours

### Research Phase
- [ ] Explore Linux Mint download page: `https://www.linuxmint.com/download.php`
- [ ] Test download patterns for editions (Cinnamon, MATE, Xfce)
- [ ] Check mirror structure and availability
- [ ] Verify SHA256 checksum availability
- [ ] Test torrent availability (optional feature)

### Architecture Design
- [ ] Create `LinuxMintISODownloader` class
- [ ] Define `LinuxMintEdition` enum: `Cinnamon`, `MATE`, `Xfce`, `LMDE` (Debian Edition)
- [ ] Design simple API (fewer steps than Windows, similar to Ubuntu)

### Header File: `include/sak/linuxmint_iso_downloader.h`
- [ ] Class declaration
- [ ] Public methods:
  - [ ] `void fetchAvailableVersions()` - Current and previous releases
  - [ ] `void fetchDownloadMirrors(QString version, LinuxMintEdition edition)` - Get mirror list
  - [ ] `void requestDownloadInfo(QString version, LinuxMintEdition edition, QString mirror)` - Get URL
  - [ ] `void downloadISO(QString url, QString savePath)`
  - [ ] `void cancel()`
  - [ ] Getter methods for cached data
- [ ] Signals (similar to Ubuntu pattern)
- [ ] Private members and methods

### Implementation File: `src/core/linuxmint_iso_downloader.cpp`
- [ ] Parse Linux Mint website for available versions
- [ ] Parse edition pages for download links
- [ ] Build download URLs (typically: `https://mirrors.layeronline.com/linuxmint/stable/{version}/linuxmint-{version}-{edition}-64bit.iso`)
- [ ] Fetch and parse SHA256 checksums
- [ ] Implement download with progress
- [ ] Implement checksum verification

### UI Integration
- [ ] Add "Download Linux Mint" button
- [ ] Create download dialog:
  - [ ] Version dropdown (Latest, Previous)
  - [ ] Edition dropdown (Cinnamon, MATE, Xfce)
  - [ ] Mirror selection (optional: auto-select fastest)
  - [ ] Download controls
- [ ] Signal/slot connections
- [ ] Auto-load downloaded ISO

### Testing
- [ ] Test with Linux Mint 21.3 Cinnamon
- [ ] Test mirror selection
- [ ] Test checksum verification
- [ ] Test error handling

### Documentation
- [ ] Class documentation
- [ ] Update plan document

---

## 3. Raspberry Pi OS Downloader

**Difficulty**: ⭐⭐⭐ (Medium - JSON API available but complex variants)  
**Priority**: MEDIUM  
**Estimated Time**: 5-7 hours

### Research Phase
- [ ] Explore Raspberry Pi downloads API: `https://downloads.raspberrypi.org/`
- [ ] Test API endpoints for OS list
- [ ] Check available variants:
  - [ ] Raspberry Pi OS Lite (32-bit)
  - [ ] Raspberry Pi OS Lite (64-bit)
  - [ ] Raspberry Pi OS Desktop (32-bit)
  - [ ] Raspberry Pi OS Desktop (64-bit)
  - [ ] Raspberry Pi OS Full (32-bit)
  - [ ] Raspberry Pi OS Full (64-bit)
- [ ] Test direct download URLs
- [ ] Check for SHA256/SHA1 checksums
- [ ] Test XZ compressed images (common for RPi OS)

### Architecture Design
- [ ] Create `RaspberryPiOSDownloader` class
- [ ] Define `RPiOSVariant` enum: `Lite`, `Desktop`, `Full`
- [ ] Define `RPiOSArchitecture` enum: `ARM32`, `ARM64`
- [ ] Handle `.img.xz` format (compressed images)

### Header File: `include/sak/raspberrypi_os_downloader.h`
- [ ] Class declaration
- [ ] Public methods:
  - [ ] `void fetchAvailableImages()` - Get all variants
  - [ ] `void requestDownloadInfo(RPiOSVariant variant, RPiOSArchitecture arch)`
  - [ ] `void downloadImage(QString url, QString savePath)`
  - [ ] `void cancel()`
  - [ ] Getters for variants and architectures
- [ ] Signals (standard pattern)
- [ ] Private members for API parsing

### Implementation File: `src/core/raspberrypi_os_downloader.cpp`
- [ ] Parse Raspberry Pi downloads API (JSON or HTML)
- [ ] Build download URLs
- [ ] Handle XZ decompression (may require on-the-fly decompression or separate step)
- [ ] Implement checksum verification
- [ ] Download with progress tracking

### UI Integration
- [ ] Add "Download Raspberry Pi OS" button
- [ ] Create download dialog:
  - [ ] Variant dropdown (Lite, Desktop, Full)
  - [ ] Architecture dropdown (32-bit, 64-bit)
  - [ ] Note about XZ compression and auto-extraction
  - [ ] Download controls
- [ ] Handle XZ decompression UI feedback
- [ ] Auto-load extracted image

### Testing
- [ ] Test with Raspberry Pi OS Lite 64-bit
- [ ] Test XZ decompression
- [ ] Test checksum verification
- [ ] Test with different variants

### Documentation
- [ ] Class documentation
- [ ] Document XZ handling
- [ ] Update plan

---

## 4. Debian ISO Downloader

**Difficulty**: ⭐⭐⭐⭐ (Medium-Hard - Multiple mirrors, complex structure)  
**Priority**: LOW  
**Estimated Time**: 6-8 hours

### Research Phase
- [ ] Explore Debian download structure: `https://www.debian.org/CD/http-ftp/`
- [ ] Test mirror list API: `https://www.debian.org/mirror/list`
- [ ] Understand release structure:
  - [ ] Stable (current: Debian 12 "Bookworm")
  - [ ] Testing
  - [ ] Unstable (Sid)
- [ ] Check netinst vs full ISO options
- [ ] Test architecture options (amd64, arm64, i386, etc.)
- [ ] Verify jigdo/torrent alternatives
- [ ] Test SHA512/SHA256 checksum files

### Architecture Design
- [ ] Create `DebianISODownloader` class
- [ ] Define `DebianRelease` enum: `Stable`, `Testing`, `Unstable`
- [ ] Define `DebianISOType` enum: `NetInst`, `DVD1`, `DVD2`, `DVD3`, `Complete`
- [ ] Define `DebianArchitecture` enum: `AMD64`, `ARM64`, `I386`, `ARMEL`, `ARMHF`, etc.
- [ ] Implement mirror selection logic

### Header File: `include/sak/debian_iso_downloader.h`
- [ ] Class declaration
- [ ] Public methods:
  - [ ] `void fetchAvailableReleases()` - Get Stable/Testing/Unstable info
  - [ ] `void fetchMirrors(QString countryCode)` - Get nearby mirrors
  - [ ] `void requestDownloadInfo(DebianRelease release, DebianISOType type, DebianArchitecture arch, QString mirror)`
  - [ ] `void downloadISO(QString url, QString savePath)`
  - [ ] `void cancel()`
  - [ ] Mirror management methods
- [ ] Signals (standard pattern + mirror selection)
- [ ] Private members for mirror list and release info

### Implementation File: `src/core/debian_iso_downloader.cpp`
- [ ] Parse Debian release information
- [ ] Parse mirror list (XML/JSON)
- [ ] Implement mirror selection:
  - [ ] Geographic proximity (based on IP or user selection)
  - [ ] Automatic fastest mirror detection (optional)
- [ ] Build download URLs (complex path structure)
- [ ] Parse SHA512SUMS files
- [ ] Download with progress
- [ ] Checksum verification (SHA512)

### UI Integration
- [ ] Add "Download Debian" button
- [ ] Create download dialog:
  - [ ] Release dropdown (Stable, Testing, Unstable)
  - [ ] ISO Type dropdown (Network Installer, DVD 1, Full Set)
  - [ ] Architecture dropdown (amd64, arm64, i386)
  - [ ] Mirror selection (auto-detect or manual)
  - [ ] Download controls
- [ ] Advanced options:
  - [ ] Choose specific DVD number for multi-disc releases
  - [ ] Torrent option (if implementing torrent support)
- [ ] Signal/slot connections

### Testing
- [ ] Test with Debian 12 Stable amd64 netinst
- [ ] Test mirror selection
- [ ] Test SHA512 verification
- [ ] Test different ISO types
- [ ] Test error handling

### Documentation
- [ ] Class documentation
- [ ] Document Debian release structure
- [ ] Document mirror selection algorithm
- [ ] Update plan

---

## 5. Windows 10 ISO Downloader

**Difficulty**: ⭐⭐⭐⭐⭐ (Hard - Similar to Windows 11 API complexity)  
**Priority**: HIGH (but complex)  
**Estimated Time**: 8-12 hours

### Research Phase
- [ ] Test Windows 10 Media Creation Tool API: `https://www.microsoft.com/software-download/windows10`
- [ ] Compare with Windows 11 implementation (likely very similar)
- [ ] Identify session ID mechanism
- [ ] Test language and edition selection
- [ ] Test architecture options (x64, x86)
- [ ] Check download URL expiration (24 hours like Win11)
- [ ] Test for different Windows 10 editions (Home, Pro, Education)

### Architecture Design
- [ ] Option A: Extend `WindowsISODownloader` to support both Win10 and Win11
  - [ ] Add `WindowsVersion` enum: `Windows10`, `Windows11`
  - [ ] Modify existing methods to accept version parameter
  - [ ] Refactor common code
- [ ] Option B: Create separate `Windows10ISODownloader` class
  - [ ] Duplicate structure but different endpoints
  - [ ] Easier to maintain separate logic
- [ ] **Recommendation**: Option A (extend existing class for code reuse)

### Header File Modifications: `include/sak/windows_iso_downloader.h`
- [ ] Add `WindowsVersion` enum
- [ ] Modify method signatures:
  - [ ] `void fetchProductPage(WindowsVersion version)`
  - [ ] `void fetchAvailableLanguages(WindowsVersion version)`
  - [ ] etc.
- [ ] Add Windows 10 specific constants:
  - [ ] Windows 10 product page URL
  - [ ] Windows 10 API endpoints
- [ ] Add methods:
  - [ ] `void setWindowsVersion(WindowsVersion version)`
  - [ ] `WindowsVersion getWindowsVersion() const`

### Implementation File Modifications: `src/core/windows_iso_downloader.cpp`
- [ ] Add Windows 10 constants:
  - [ ] `PRODUCT_PAGE_URL_WIN10 = "https://www.microsoft.com/software-download/windows10"`
  - [ ] API endpoints for Windows 10
- [ ] Modify `fetchProductPage()`:
  - [ ] Use different URL based on version
  - [ ] Parse session ID (may differ from Win11)
- [ ] Modify `fetchAvailableLanguages()`:
  - [ ] Different API endpoint for Win10
  - [ ] Parse response (JSON structure may differ)
- [ ] Modify `requestDownloadUrl()`:
  - [ ] Different API endpoint for Win10
  - [ ] Handle edition selection (Home, Pro, Education)
- [ ] Test for differences in:
  - [ ] Session ID format
  - [ ] API response structures
  - [ ] Download URL format
  - [ ] Expiration handling
- [ ] Add support for x86 architecture (Windows 10 has 32-bit, Win11 doesn't)

### UI Integration in `image_flasher_panel.cpp`
- [ ] Modify "Download Windows 11" button to "Download Windows ISO"
- [ ] Update dialog to support version selection:
  - [ ] Version dropdown (Windows 10, Windows 11)
  - [ ] Edition dropdown (if Windows 10: Home/Pro/Education)
  - [ ] Language dropdown
  - [ ] Architecture dropdown (Win10: x86/x64/ARM64, Win11: x64/ARM64)
- [ ] Dynamic UI based on version:
  - [ ] Show edition selector only for Windows 10
  - [ ] Show x86 option only for Windows 10
- [ ] Handle download workflow for both versions

### Testing
- [ ] Test Windows 10 product page fetching
- [ ] Test Windows 10 language list
- [ ] Test Windows 10 download URL request
- [ ] Test Windows 10 ISO download
- [ ] Test edition selection (Home, Pro, Education)
- [ ] Test x86 and x64 downloads
- [ ] Compare behavior with Windows 11 flow
- [ ] Test error handling
- [ ] Test URL expiration and re-request

### Documentation
- [ ] Update class documentation to mention both versions
- [ ] Document differences between Win10 and Win11 APIs
- [ ] Add examples for both versions
- [ ] Update plan document

---

## Shared Infrastructure Components

These components will be used by multiple downloaders:

### 1. Download Progress Dialog Widget
- [ ] Create `ISODownloadDialog` reusable widget
- [ ] Features:
  - [ ] Title customization (per OS)
  - [ ] Progress bar with percentage
  - [ ] Speed display (MB/s)
  - [ ] ETA calculation
  - [ ] Status messages
  - [ ] Cancel button
  - [ ] Checksum verification progress
- [ ] File: `include/sak/iso_download_dialog.h`, `src/gui/iso_download_dialog.cpp`

### 2. Checksum Verification Utility
- [ ] Create `ChecksumVerifier` utility class
- [ ] Support multiple algorithms:
  - [ ] SHA256 (Ubuntu, Linux Mint, Raspberry Pi OS)
  - [ ] SHA512 (Debian)
  - [ ] SHA1 (legacy, if needed)
- [ ] Streaming calculation for large files
- [ ] Progress reporting
- [ ] File: `include/sak/checksum_verifier.h`, `src/core/checksum_verifier.cpp`

### 3. Download Manager Base Class (Optional)
- [ ] Create `ISODownloaderBase` abstract base class
- [ ] Common functionality:
  - [ ] Network manager setup
  - [ ] Download with progress
  - [ ] Cancellation handling
  - [ ] Error handling
  - [ ] Retry logic
  - [ ] Speed calculation
- [ ] Derived classes implement:
  - [ ] OS-specific API parsing
  - [ ] URL construction
  - [ ] Metadata extraction
- [ ] File: `include/sak/iso_downloader_base.h`, `src/core/iso_downloader_base.cpp`

### 4. Mirror Selection Utility (for Debian, possibly Ubuntu)
- [ ] Create `MirrorSelector` utility
- [ ] Features:
  - [ ] Geographic location detection (IP-based)
  - [ ] Ping multiple mirrors
  - [ ] Select fastest mirror
  - [ ] Fallback to alternative mirrors on failure
- [ ] File: `include/sak/mirror_selector.h`, `src/core/mirror_selector.cpp`

---

## UI/UX Enhancements

### Image Selection Page Updates
- [ ] Reorganize download buttons into a group
- [ ] Add icons for each OS downloader
- [ ] Group buttons by category:
  - [ ] **Windows**: Windows 10, Windows 11
  - [ ] **Linux Desktop**: Ubuntu Desktop, Linux Mint, Debian Desktop
  - [ ] **Linux Server**: Ubuntu Server, Debian Server
  - [ ] **Embedded**: Raspberry Pi OS
- [ ] Add "More OS Downloads..." button for future expansion
- [ ] Add tooltips explaining each OS option

### Download History/Queue (Future Enhancement)
- [ ] Track downloaded ISOs in settings
- [ ] Show recently downloaded ISOs for quick re-selection
- [ ] Download queue for multiple simultaneous downloads
- [ ] Resume interrupted downloads

---

## Build System Updates

### CMakeLists.txt
- [ ] Add new source files:
  ```cmake
  src/core/ubuntu_iso_downloader.cpp
  src/core/linuxmint_iso_downloader.cpp
  src/core/raspberrypi_os_downloader.cpp
  src/core/debian_iso_downloader.cpp
  ```
- [ ] Add new header files to installation
- [ ] Add new UI components if created

### Header Installation
- [ ] Ensure new headers are in `include/sak/` directory
- [ ] Update install targets in CMakeLists.txt

---

## Testing Strategy

### Unit Tests
- [ ] Create test suite for each downloader:
  - [ ] `test_ubuntu_downloader.cpp`
  - [ ] `test_linuxmint_downloader.cpp`
  - [ ] `test_raspberrypi_downloader.cpp`
  - [ ] `test_debian_downloader.cpp`
  - [ ] `test_windows10_downloader.cpp`
- [ ] Test cases:
  - [ ] URL construction
  - [ ] API response parsing
  - [ ] Checksum calculation
  - [ ] Error handling
  - [ ] Cancellation

### Integration Tests
- [ ] End-to-end download tests (use small test ISOs if available)
- [ ] Test full workflow: Select OS → Download → Verify → Flash
- [ ] Test multiple simultaneous downloads
- [ ] Test network interruption recovery

### Manual Testing Checklist
- [ ] Download at least one ISO from each OS
- [ ] Verify checksum matches official checksums
- [ ] Flash downloaded ISO to test drive
- [ ] Boot from flashed drive to verify integrity
- [ ] Test cancellation during download
- [ ] Test network error scenarios
- [ ] Test with slow internet connection

---

## Documentation

### User Documentation
- [ ] Add "Downloading OS Images" section to user manual
- [ ] Create table of supported OS downloads with descriptions
- [ ] Add screenshots of download dialogs
- [ ] Explain checksum verification importance
- [ ] Document download locations and file management

### Developer Documentation
- [ ] Document OS downloader architecture
- [ ] Add sequence diagrams for each downloader workflow
- [ ] Document API endpoints and authentication mechanisms
- [ ] Create contribution guide for adding new OS downloaders
- [ ] Document testing procedures

### Plan Document Updates
- [ ] Mark implemented items as complete in `IMAGE_FLASHER_PANEL_PLAN.md`
- [ ] Add implementation notes
- [ ] Update architecture diagrams
- [ ] Add "Lessons Learned" section

---

## Rollout Plan

### Phase 1: Foundation (Week 1)
- [ ] Implement shared infrastructure (ChecksumVerifier, ISODownloadDialog)
- [ ] Create ISODownloaderBase abstract class
- [ ] Set up testing framework
- [ ] Update UI structure for multiple OS buttons

### Phase 2: Easy Implementations (Week 2)
- [ ] Implement Ubuntu downloader (high priority, easy)
- [ ] Implement Linux Mint downloader (medium priority, easy)
- [ ] Test and refine

### Phase 3: Medium Complexity (Week 3)
- [ ] Implement Raspberry Pi OS downloader (medium priority, medium complexity)
- [ ] Implement MirrorSelector utility
- [ ] Test and refine

### Phase 4: Complex Implementations (Week 4)
- [ ] Implement Debian downloader (low priority, high complexity)
- [ ] Extend WindowsISODownloader for Windows 10 (high priority, high complexity)
- [ ] Comprehensive testing

### Phase 5: Polish and Release (Week 5)
- [ ] UI/UX refinements
- [ ] Performance optimization
- [ ] Documentation completion
- [ ] Beta testing
- [ ] Bug fixes
- [ ] Release

---

## Success Criteria

- [ ] All 5 OS downloaders implemented and functional
- [ ] Checksum verification works for all downloads
- [ ] UI is intuitive and consistent across all downloaders
- [ ] Downloads complete successfully with progress tracking
- [ ] Error handling is robust (network errors, invalid selections, etc.)
- [ ] Downloaded ISOs can be successfully flashed using Image Flasher Panel
- [ ] All unit tests pass
- [ ] Integration tests pass
- [ ] Documentation is complete
- [ ] Code review completed
- [ ] No critical bugs in issue tracker

---

## Future Enhancements (Post-Implementation)

- [ ] Torrent support for faster downloads
- [ ] Automatic mirror selection with speed testing
- [ ] Download scheduling (off-peak hours)
- [ ] Bandwidth limiting
- [ ] Download queue management
- [ ] Resume support for all downloaders
- [ ] Additional OS support:
  - [ ] Fedora
  - [ ] Arch Linux
  - [ ] CentOS/Rocky Linux/AlmaLinux
  - [ ] FreeBSD
  - [ ] OpenSUSE
- [ ] Cloud storage integration (save ISOs to OneDrive, Google Drive)
- [ ] ISO library with metadata (release notes, system requirements)

---

## Notes

### Design Principles
1. **Consistency**: All downloaders should follow the same signal/slot pattern
2. **Reliability**: Implement retry logic and error recovery
3. **User Experience**: Provide clear feedback and progress information
4. **Security**: Always verify checksums before marking download as complete
5. **Performance**: Use streaming downloads and efficient checksum calculation
6. **Maintainability**: Use base classes and shared utilities to reduce code duplication

### API Rate Limiting Considerations
- Some APIs may have rate limits (especially Windows Media Creation Tool)
- Implement exponential backoff on failures
- Cache API responses where appropriate
- Respect server load by implementing reasonable timeouts

### Legal Considerations
- Ensure all downloads are from official sources
- Respect licensing terms of each OS
- Don't redistribute downloaded ISOs
- Include disclaimers about official sources

---

**Total Estimated Time**: 26-38 hours (spread across 5 weeks)  
**Complexity**: Medium-High  
**Risk**: Low (all downloads are from official sources with known APIs)  
**Value**: High (significantly enhances Image Flasher Panel functionality)

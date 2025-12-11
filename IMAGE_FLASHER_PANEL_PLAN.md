# Image Flasher Panel - Implementation Plan

## Overview
Implement a USB/SD card image flashing panel similar to Etcher (balena.io/etcher) for the S.A.K. Utility. This tool will allow PC technicians to flash OS images (ISO, IMG, WIC, etc.) to USB drives and SD cards safely.

---

## Core Features (Based on Etcher)

### 1. Image Selection
- [ ] Support for multiple image formats
  - [ ] ISO files
  - [ ] IMG files (raw disk images)
  - [ ] WIC files (Windows Imaging format)
  - [ ] ZIP compressed images
  - [ ] BZ2 compressed images
  - [ ] GZ compressed images
  - [ ] XZ compressed images
  - [ ] DMG files (Apple Disk Images)
  - [ ] DSK files
- [ ] Drag-and-drop support for image files
- [ ] File browser dialog for image selection
- [ ] Display image metadata (size, type, name)
- [ ] Image validation before flashing

### 2. Drive Selection
- [ ] Automatic detection of removable drives
  - [ ] USB flash drives
  - [ ] SD card readers
  - [ ] External USB drives
- [ ] Drive information display
  - [ ] Drive name/label
  - [ ] Drive size
  - [ ] Drive device path
  - [ ] Mount status
- [ ] Safety features
  - [ ] Exclude system drives from list
  - [ ] Warning for large drives (potential HDD/SSD)
  - [ ] Highlight system drive warnings in red/orange
  - [ ] Confirmation dialog before flashing system drives
- [ ] Multi-drive selection support
  - [ ] Flash to multiple drives simultaneously
  - [ ] Individual progress tracking per drive

### 3. Flash Process
- [ ] Pre-flash validation
  - [ ] Verify image file integrity
  - [ ] Check if drive has sufficient space
  - [ ] Compare image size vs drive size
- [ ] Unmount operation
  - [ ] Safely unmount all partitions on target drive(s)
  - [ ] Handle Windows drive locking
  - [ ] Retry logic for unmount failures
- [ ] Writing operation
  - [ ] Multi-threaded writing for performance
  - [ ] Decompression on-the-fly for compressed images
  - [ ] Block mapping optimization (skip unused ext partition space)
  - [ ] Configurable buffer sizes
- [ ] Progress tracking
  - [ ] Overall percentage complete
  - [ ] Bytes written / total bytes
  - [ ] Current write speed (MB/s)
  - [ ] ETA (estimated time remaining)
  - [ ] Status per device (when flashing multiple)
- [ ] Post-write validation
  - [ ] Verify written data matches source
  - [ ] CRC/SHA512 checksum verification
  - [ ] Detect potential corruption issues
  - [ ] Option to skip validation (for speed)

### 4. User Interface Components

#### Main Panel Layout
- [ ] Three-step wizard interface
  1. [ ] Select Image step (with icon)
  2. [ ] Select Drive step (with icon)
  3. [ ] Flash step (with progress button)
- [ ] Visual indicators for completed steps
- [ ] Disabled state for incomplete prerequisites

#### Image Selection Widget
- [ ] Large "Select Image" button with icon
- [ ] Display selected image details
  - [ ] File name (with ellipsis for long names)
  - [ ] File size (human-readable)
  - [ ] File icon/logo (if available)
- [ ] "Change" link to select different image

#### Drive Selection Widget
- [ ] "Select Drive" button
- [ ] Modal dialog with drive list
  - [ ] Drive icon (USB/SD card visual)
  - [ ] Drive name and description
  - [ ] Drive size
  - [ ] Warning indicators for system drives
  - [ ] Checkboxes for multi-selection
- [ ] Display selected drive(s) summary
- [ ] "Change" link to reopen drive selector

#### Flash Progress Widget
- [ ] Circular progress indicator
- [ ] Percentage display (0-100%)
- [ ] Status text
  - [ ] "Flashing..." during write
  - [ ] "Verifying..." during validation
  - [ ] "Decompressing..." if applicable
- [ ] Speed and ETA display
- [ ] Cancel button (during operation)
- [ ] Retry button (on failure)

#### Results Screen
- [ ] Success/failure indication
  - [ ] Green checkmark for success
  - [ ] Red X for failure
- [ ] Statistics
  - [ ] Total time elapsed
  - [ ] Average write speed
  - [ ] Successful devices count
  - [ ] Failed devices count
- [ ] Error details (if any)
  - [ ] Expandable error list
  - [ ] Error codes and descriptions
  - [ ] Device-specific errors
- [ ] "Flash Another" button to reset

### 5. Settings Integration
- [ ] Configurable options in Settings Dialog
  - [ ] Enable/disable post-write validation
  - [ ] Auto-unmount on completion
  - [ ] Notification preferences
  - [ ] Error reporting (anonymous analytics)
  - [ ] Trim ext partitions option (block mapping)
  - [ ] Decompress-first mode toggle

### 6. Safety Features
- [ ] Multiple confirmation levels
  - [ ] Warning for system drives
  - [ ] Warning for large drives (>32GB typical warning)
  - [ ] Confirmation before data destruction
- [ ] Drive protection
  - [ ] Prevent writing to source drive (if image is on USB)
  - [ ] Lock drive during operation
  - [ ] Prevent accidental cancellation
- [ ] Error recovery
  - [ ] Graceful handling of drive unplugging
  - [ ] Recovery from write errors
  - [ ] Cleanup of partial writes

---

## Technical Implementation

### Phase 1: Core Infrastructure (Week 1)

#### 1.1 Drive Detection System (Based on Etcher SDK Scanner Architecture)
- [ ] Create `DriveScanner` class (inspired by etcher-sdk's Scanner)
  - [ ] Event-driven architecture (QObject with signals/slots)
  - [ ] Windows: Use Win32 APIs + WMI for drive enumeration
  - [ ] Real-time drive monitoring using `RegisterDeviceNotification`
  - [ ] Support for BlockDevice adapter pattern
  - [ ] Emit signals: `driveAttached`, `driveDetached`, `scanReady`, `scanError`
- [ ] Create `DriveInfo` struct (inspired by etcher-sdk's BlockDevice)
  ```cpp
  struct DriveInfo {
      QString devicePath;      // \\.\PhysicalDrive# (e.g., \\.\PhysicalDrive1)
      QString device;          // Unique identifier
      QString name;            // Display name (e.g., "USB Drive")
      QString description;     // Full description (e.g., "SanDisk Ultra 32GB")
      quint64 size;           // Size in bytes
      quint64 blockSize;      // Sector/block size (usually 512 or 4096)
      bool isRemovable;       // USB/SD card
      bool isSystem;          // Contains OS (C: drive)
      bool isMounted;         // Has mounted partitions
      QStringList mountPoints; // Drive letters (e.g., ["D:", "E:"])
      bool isReadOnly;        // Write-protected
      QString busType;        // "USB", "SD", "SATA", etc.
  }
  ```
- [ ] Create `DriveAdapter` base class (adapter pattern from etcher-sdk)
  - [ ] `BlockDeviceAdapter` - for physical drives
  - [ ] Virtual method: `scan()` - enumerate drives
  - [ ] Virtual method: `isSystemDrive()` - safety check
  - [ ] Event emitter for attach/detach
- [ ] Create `DriveFilter` class
  - [ ] Filter out system drives (C: drive)
  - [ ] Filter out non-removable drives (optional)
  - [ ] Filter by minimum size
  - [ ] Sort drives by type and size

#### 1.2 Image Handler (Based on Etcher SDK SourceDestination)
- [ ] Create `ImageSource` base class (inspired by etcher-sdk's SourceDestination)
  - [ ] Abstract interface for all image types
  - [ ] Virtual methods: `open()`, `close()`, `read()`, `getMetadata()`
  - [ ] Support for streaming reads
  - [ ] Checksum calculation capability
- [ ] Create `ImageMetadata` struct
  ```cpp
  struct ImageMetadata {
      QString name;           // Display name
      QString path;           // Full file path
      ImageFormat format;     // File format enum
      quint64 size;          // Uncompressed size
      quint64 compressedSize;// Compressed size (if applicable)
      bool isCompressed;     // Requires decompression
      bool hasMBR;           // Has Master Boot Record
      int partitionCount;    // Number of partitions
      QString checksum;      // SHA-512 hash
  }
  ```
- [ ] Create `ImageFormat` enum
  ```cpp
  enum class ImageFormat {
      ISO,      // ISO 9660 disc image
      IMG,      // Raw disk image
      WIC,      // Windows Imaging format
      ZIP,      // ZIP archive
      GZ,       // Gzip compressed
      BZ2,      // Bzip2 compressed
      XZ,       // XZ compressed
      DMG,      // Apple Disk Image
      DSK,      // Generic disk image
      Unknown
  }
  ```
- [ ] Implement source type classes (etcher-sdk pattern)
  - [ ] `FileImageSource` - local file images
  - [ ] `CompressedImageSource` - handles compressed files
  - [ ] Wrapper pattern for decompression (zip/gz/bz2/xz)
- [ ] Format detection
  - [ ] Magic number checking (file signatures)
  - [ ] Extension-based fallback
  - [ ] Archive introspection for nested images

#### 1.3 Flash Engine Core (Based on Etcher SDK multiWrite)
- [ ] Create `FlashWorker` class (QThread-based, inspired by etcher-sdk's multiWrite)
  - [ ] State machine for flash process phases
  - [ ] Progress signaling (percentage, speed, ETA)
  - [ ] Multi-destination support (parallel writes)
  - [ ] Error handling per destination
  - [ ] Cancellation support with cleanup
  - [ ] Memory-efficient streaming
- [ ] Create `FlashState` enum (matches etcher-sdk progress types)
  ```cpp
  enum class FlashState {
      Idle,           // Not started
      Unmounting,     // Preparing drives
      Decompressing,  // Extracting compressed image
      Flashing,       // Writing data
      Verifying,      // Checking written data
      Completed,      // Success
      Failed,         // Error occurred
      Cancelled       // User cancelled
  }
  ```
- [ ] Implement `FlashProgress` struct (etcher-sdk MultiDestinationProgress pattern)
  ```cpp
  struct FlashProgress {
      FlashState state;          // Current phase
      int percentage;            // 0-100 overall progress
      quint64 bytesWritten;      // Bytes written so far
      quint64 totalBytes;        // Total bytes to write
      double speedMBps;          // Current write speed (MB/s)
      int etaSeconds;            // Estimated time remaining
      int active;                // Number of active writes
      int failed;                // Number of failed writes
      QString devicePath;        // Current device being written
      quint64 position;          // Current byte position
  }
  ```
- [ ] Create `WriteResult` struct (etcher-sdk pattern)
  ```cpp
  struct WriteResult {
      quint64 bytesWritten;
      ImageMetadata sourceMetadata;
      QMap<QString, QString> failures; // device -> error message
      int successful;
      int failed;
      double averageSpeed;
      qint64 duration;          // Total time in milliseconds
  }
  ```

## Implementation Status

### Phase 1: Core Infrastructure (Week 1) - ✅ COMPLETED

- [x] **DriveScanner** - Physical drive detection and hot-plug monitoring
  - File: `include/sak/drive_scanner.h`, `src/core/drive_scanner.cpp`
  - WMI-based drive enumeration
  - Windows device notification support
  - System drive detection
  - Removable drive filtering

- [x] **ImageSource** - Abstract image source with compression support
  - File: `include/sak/image_source.h`, `src/core/image_source.cpp`
  - FileImageSource for regular files
  - CompressedImageSource for streaming decompression (placeholder)
  - Format detection (ISO, IMG, WIC, ZIP, GZ, BZ2, XZ, DMG, DSK)
  - SHA-512 checksum calculation

- [x] **FlashWorker** - Worker thread for writing to drives
  - File: `include/sak/flash_worker.h`, `src/threading/flash_worker.cpp`
  - Sector-aligned writes with FILE_FLAG_NO_BUFFERING
  - Volume locking and dismounting
  - Progress tracking and speed calculation
  - Verification support (placeholder)
  - Inherits from WorkerBase with std::expected error handling

- [x] **FlashCoordinator** - Multi-drive flash orchestration
  - File: `include/sak/flash_coordinator.h`, `src/core/flash_coordinator.cpp`
  - Parallel writes to multiple drives
  - State machine (Idle, Validating, Unmounting, Flashing, Verifying, etc.)
  - Progress aggregation from multiple workers
  - Configurable verification and buffer settings

- [x] **WindowsISODownloader** - Download Windows 11 ISOs
  - File: `include/sak/windows_iso_downloader.h`, `src/core/windows_iso_downloader.cpp`
  - Microsoft Media Creation Tool API integration
  - Multi-step workflow (session, languages, download URL)
  - Progress tracking with speed calculation
  - Resume capability support

- [x] **ImageFlasherPanel** - Main UI panel
  - File: `include/sak/image_flasher_panel.h`, `src/gui/image_flasher_panel.cpp`
  - Four-page wizard interface (Image Selection, Drive Selection, Progress, Completion)
  - Image file selection with format detection
  - Drive list with system drive warnings
  - Real-time progress display
  - Confirmation dialogs

- [x] **CMakeLists.txt Updates**
  - Added all new source files to build
  - Added Qt6::Network dependency
  - Properly organized into CORE_SOURCES, THREADING_SOURCES, and GUI_SOURCES

### Next Steps

### Phase 2: Write Operations (Week 2) - ✅ COMPLETE

#### 2.1 Drive Preparation (Windows-Specific, Etcher SDK Patterns)
- [x] Create `DriveUnmounter` class
  - [x] Windows: Enumerate all volumes on the drive
  - [x] Use `DeleteVolumeMountPoint` Win32 API
  - [x] Use `FSCTL_LOCK_VOLUME` - exclusive access
  - [x] Use `FSCTL_DISMOUNT_VOLUME` - force unmount
  - [x] Handle locked volumes (retry with backoff)
  - [x] Close all file handles to the drive
  - [x] Exponential backoff: 100ms, 200ms, 400ms, 800ms, 1600ms
  - [x] Maximum retry attempts: 5
- [x] Create `DriveLock` class (RAII pattern)
  - [x] Constructor: Acquire exclusive lock
  - [x] Destructor: Release lock (exception-safe)
  - [x] Prevent Windows automount during operation
  - [x] Use `FSCTL_SET_PERSISTENT_VOLUME_STATE` to prevent auto-mount
  - [x] Restore auto-mount on completion

#### 2.2 Image Writing (Etcher SDK decompressThenFlash Pattern)
- [x] Create `ImageWriter` class (inspired by etcher-sdk's pipeSourceToDestinations)
  - [x] Raw sector-level writing using Win32 API
  - [x] Open drive handle: `CreateFile(\\.\PhysicalDrive#, GENERIC_WRITE, ...)`
  - [x] Set file pointer: `SetFilePointerEx` for large offsets
  - [x] Write operation: `WriteFile` with unbuffered I/O
  - [x] Sector-aligned writes (512 or 4096 bytes)
  - [x] Buffered I/O for performance (configurable buffer size)
  - [x] Block-level operations (not file-level)
  - [x] Flush buffers: `FlushFileBuffers` after writes
- [x] Implement write strategies (etcher-sdk decompressFirst pattern)
  - [x] **Direct write** for uncompressed images
    - Stream from source file → destination device
  - [x] **Decompress-then-write** for compressed images
    - Stream decompression on-the-fly (decompressFirst=false)
  - [ ] **Sparse file handling** (etcher-sdk SparseStream pattern) - Future enhancement
    - Skip unused blocks in ext2/ext3/ext4 partitions
    - Detect zero blocks and skip writing
    - Requires partition table parsing
- [x] Handle Windows-specific requirements
  - [x] Elevation/admin privileges required
  - [x] Run as SYSTEM or Administrator
  - [x] `FILE_FLAG_NO_BUFFERING` for direct I/O
  - [x] `FILE_FLAG_WRITE_THROUGH` to bypass cache
  - [x] Sector alignment: All operations must align to sector boundaries

#### 2.3 Multi-Write Support (Etcher SDK multiWrite Pattern)
- [x] Create `FlashCoordinator` class (inspired by etcher-sdk's multiWrite.pipeSourceToDestinations)
  - [x] Parallel writing to multiple drives simultaneously
  - [x] One QThread worker per destination drive
  - [x] Shared source image reading (single decompression)
  - [x] Per-drive progress tracking with aggregation
  - [x] Per-drive error handling (one failure doesn't stop others)
  - [x] Signals: `progressUpdated(devicePath, progress)`, `driveFailed(devicePath, error)`
- [x] Optimize thread pool usage (etcher-sdk numBuffers pattern)
  - [x] One worker thread per drive (up to CPU core count)
  - [x] Shared decompression cache (if applicable)
  - [x] Buffer size: Configurable (default 64MB per buffer)
  - [x] CPU management: Leave cores for UI

#### 2.4 Decompression Support (Etcher SDK Compression Sources)
- [x] Integrate compression libraries (etcher-sdk uses streaming decompression)
  - [x] **zlib** (for .gz files) - via vcpkg
  - [x] **libbz2** (for .bz2 files) - via vcpkg
  - [x] **liblzma** (for .xz files) - via vcpkg
  - [ ] **QuaZip** (for .zip files) - already in project (future enhancement)
- [x] Create `DecompressorFactory` (etcher-sdk CompressedSource pattern)
  - [x] Auto-detect compression type (magic numbers)
  - [x] Stream-based decompression (no temp files by default)
  - [x] Progress reporting during decompression phase
  - [x] Support chained compression (e.g., .tar.gz)
- [x] Implement `StreamingDecompressor` base class
  - [x] `read(buffer, size)` - decompress into buffer
  - [x] `bytesRead()` - track compressed bytes processed
  - [x] `bytesDecompressed()` - track uncompressed bytes produced
  - [x] Subclasses: `GzipDecompressor`, `Bzip2Decompressor`, `XzDecompressor`
- [x] Handle stream decompression (etcher-sdk pattern)
  - [x] Stream decompression during write (default)
    - Pros: No temp disk space needed
    - Cons: Slower, can't seek in compressed stream

### Phase 3: Verification System (Week 3) - ✅ COMPLETE

#### 3.1 Checksum Verification (Etcher SDK Pattern)
- [x] Integrated checksum calculation in ImageSource and FlashWorker
  - [x] SHA-512 hash calculation (primary method)
  - [x] Streaming hash calculation (memory-efficient)
  - [x] Use Qt's QCryptographicHash
- [x] Source image hashing (etcher-sdk caches source checksum)
  - [x] Calculate hash during initial write phase
  - [x] Cache hash in FlashWorker
  - [x] Reuse cached hash for verification
  - [x] Progress reporting during hash calculation
- [x] Target verification (etcher-sdk verify option)
  - [x] Read back written data from device
  - [x] Calculate checksum of written data
  - [x] Compare source vs target hashes
  - [x] Byte-by-byte comparison for sample mode
  - [x] Sample-based verification option (faster)

#### 3.2 Validation Features (Etcher SDK Validation Patterns)
- [x] Create `ValidationResult` struct
  ```cpp
  struct ValidationResult {
      bool passed;               // Overall success/failure
      QString sourceChecksum;    // Expected checksum (SHA-512)
      QString targetChecksum;    // Actual checksum from device
      QList<QString> errors;     // Detailed error messages
      qint64 mismatchOffset;     // First mismatch byte position (-1 if none)
      int corruptedBlocks;       // Number of blocks with errors
      double verificationSpeed;  // MB/s read speed during verify
  }
  ```
- [x] Implement validation strategies (etcher-sdk verify modes)
  - [x] **Full validation** (verify=true, default)
    - Read every byte back from device
    - Calculate complete checksum
    - Compare with source
    - Most reliable but slowest
  - [x] **Sample validation** (quick mode)
    - Read random sample blocks
    - Verify checksums of samples only
    - Faster but less thorough
    - Sample size: 100MB or 10% of image (whichever is smaller)
  - [x] **Skip validation** (verify=false)
    - User choice for speed
    - No verification performed
    - Flash only, no read-back
- [x] Handle verification errors (etcher-sdk error codes)
  - [x] Checksum mismatch detected
  - [x] Report specific block/sector of corruption
  - [x] Error reporting through ValidationResult
  - [ ] Drive removal detection during verification - Future enhancement
  - [ ] Bad sector detection and reporting - Future enhancement

### Phase 4: User Interface (Week 4) - ✅ COMPLETE

#### 4.1 Main Panel Widget
- [x] Create `ImageFlasherPanel` class (inherits QWidget)
  - [x] Four-page wizard interface (Image, Drive, Progress, Completion)
  - [x] Step indicator UI
  - [x] State management
- [x] File: `src/gui/image_flasher_panel.cpp`
- [x] Header: `include/sak/image_flasher_panel.h`

#### 4.2 Image Selection Widget
- [x] Image selection page with browse button
  - [x] Browse button with file dialog
  - [x] Image info display
  - [x] Windows 11 ISO downloader integration
- [x] Supported file filters
  ```cpp
  "Image Files (*.iso *.img *.wic *.zip *.gz *.bz2 *.xz *.dmg *.dsk);;All Files (*.*)"
  ```

#### 4.3 Drive Selector Dialog
- [x] Drive selection page
  - [x] QListWidget with drive information
  - [x] Multi-selection with checkboxes
  - [x] Warning badges for system drives
  - [x] Refresh button
  - [x] Real-time updates on plug/unplug via DriveScanner

#### 4.4 Flash Progress Widget
- [x] Flash progress page
  - [x] QProgressBar (horizontal)
  - [x] Status label (dynamic text)
  - [x] Speed/ETA labels
  - [x] Cancel button
  - [x] Per-device progress tracking

#### 4.5 Results Dialog
- [x] Completion page
  - [x] Success/failure banner
  - [x] Statistics display
  - [x] Flash Another button
  - [x] Close button

### Phase 5: Integration & Testing (Week 5) - 🔨 IN PROGRESS

#### 5.1 Main Window Integration
- [x] Add "Image Flasher" tab to `MainWindow`
- [x] Update `CMakeLists.txt` to include new files
- [x] Register panel in tab widget
- [ ] Add icon to resources (optional future enhancement)

#### 5.2 Settings Integration
- [ ] Add "Image Flasher" section to `SettingsDialog`
  - [ ] Validation toggle
  - [ ] Unmount on completion toggle
  - [ ] Notification preferences
  - [ ] Buffer size configuration
  - [ ] Ext partition trim toggle
- [ ] Save/load settings in `ConfigManager`

#### 5.3 Logging & Diagnostics
- [ ] Integrate with existing `Logger` class
- [ ] Log all flash operations
  - [ ] Image selected
  - [ ] Drives selected
  - [ ] Flash started
  - [ ] Progress updates (sampled)
  - [ ] Verification results
  - [ ] Errors and warnings
- [ ] Create flash session logs
  - [ ] Timestamp
  - [ ] Image hash
  - [ ] Drive info
  - [ ] Success/failure status

#### 5.4 Testing
- [ ] Unit tests (`tests/test_image_flasher.cpp`)
  - [ ] Drive detection
  - [ ] Image validation
  - [ ] Format detection
  - [ ] Hash calculation
- [ ] Integration tests
  - [ ] Mock drive operations
  - [ ] Simulated flash process
  - [ ] Error condition handling
- [ ] Manual testing checklist
  - [ ] Flash to USB drive
  - [ ] Flash to SD card
  - [ ] Multi-drive flash
  - [ ] Cancellation during flash
  - [ ] Drive unplug during flash
  - [ ] Compressed image flashing
  - [ ] Verification pass/fail scenarios
  - [ ] System drive warning
  - [ ] Large drive warning

---

## File Structure

### New Files to Create

```
include/sak/
├── image_flasher_panel.h           # Main panel widget
├── drive_scanner.h                 # Drive detection and monitoring
├── drive_info.h                    # Drive information struct
├── image_file.h                    # Image file handling
├── flash_worker.h                  # Flash operation worker thread
├── multi_write_coordinator.h       # Multi-drive flash coordinator
├── drive_unmounter.h               # Drive unmount operations
├── image_writer.h                  # Low-level write operations
├── checksum_calculator.h           # Hash/checksum calculation
├── decompressor_factory.h          # Decompression handler factory
└── flash_state.h                   # State structures and enums

src/gui/
├── image_flasher_panel.cpp         # Main panel implementation
├── image_selector_widget.cpp       # Image selection UI
├── drive_selector_dialog.cpp       # Drive selection modal
├── flash_progress_widget.cpp       # Progress display
└── flash_results_dialog.cpp        # Results screen

src/core/
├── drive_scanner.cpp               # Drive detection logic
├── image_file.cpp                  # Image handling
├── flash_worker.cpp                # Flash worker implementation
├── multi_write_coordinator.cpp     # Multi-write coordination
├── drive_unmounter.cpp             # Unmount operations
├── image_writer.cpp                # Write operations
├── checksum_calculator.cpp         # Checksum implementation
└── decompressor_factory.cpp        # Decompression logic

tests/
├── test_drive_scanner.cpp          # Drive detection tests
├── test_image_file.cpp             # Image handling tests
├── test_flash_worker.cpp           # Flash operation tests
├── test_checksum_calculator.cpp    # Checksum tests
└── test_image_flasher_integration.cpp  # Integration tests
```

---

## Dependencies & Libraries

### Required Libraries (via vcpkg)
- [ ] **zlib** - For .gz decompression
  ```powershell
  vcpkg install zlib:x64-windows
  ```
  - Used by etcher-sdk's GzipSource
  - Streaming decompression support
  
- [ ] **libbz2** - For .bz2 decompression
  ```powershell
  vcpkg install bzip2:x64-windows
  ```
  - Used by etcher-sdk's BZip2Source
  - Efficient compression for disk images
  
- [ ] **liblzma** - For .xz decompression
  ```powershell
  vcpkg install liblzma:x64-windows
  ```
  - Used by etcher-sdk's XzSource
  - Best compression ratio
  
- [ ] **QuaZip** - For .zip archives (already in project)
  - Already available in S.A.K. Utility
  - Qt-friendly ZIP library
  
- [ ] **OpenSSL** - For SHA-512 hashing
  ```powershell
  vcpkg install openssl:x64-windows
  ```
  - Etcher SDK uses SHA-512 for verification
  - Crypto++ alternative: `vcpkg install cryptopp:x64-windows`
  - Or use Qt's QCryptographicHash (built-in, slower but no dependency)

### Windows APIs Required (Etcher SDK Equivalents)
- [ ] **Drive Enumeration**
  - [ ] `FindFirstVolume` / `FindNextVolume` - Enumerate volumes
  - [ ] `GetVolumePathNamesForVolumeName` - Get mount points
  - [ ] `GetDiskFreeSpaceEx` - Check drive capacity
  - [ ] WMI Queries (via `wmic` or COM):
    - `SELECT * FROM Win32_DiskDrive`
    - `SELECT * FROM Win32_LogicalDisk`
    - `SELECT * FROM Win32_Volume`
  
- [ ] **Drive Operations** (etcher-sdk BlockDevice operations)
  - [ ] `CreateFile` - Open drive handle
    ```cpp
    HANDLE hDrive = CreateFile(
        "\\\\.\\PhysicalDrive1",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
        NULL
    );
    ```
  - [ ] `DeviceIoControl` - Drive control operations
    - `FSCTL_LOCK_VOLUME` - Lock drive for exclusive access
    - `FSCTL_UNLOCK_VOLUME` - Unlock drive
    - `FSCTL_DISMOUNT_VOLUME` - Dismount all partitions
    - `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX` - Get sector size
    - `IOCTL_STORAGE_GET_DEVICE_NUMBER` - Get device number
  - [ ] `WriteFile` - Write sectors to drive (unbuffered)
  - [ ] `ReadFile` - Read sectors for verification
  - [ ] `SetFilePointerEx` - Seek to position (64-bit offsets)
  - [ ] `FlushFileBuffers` - Ensure data written to disk
  
- [ ] **Drive Monitoring** (etcher-sdk Scanner events)
  - [ ] `RegisterDeviceNotification` - Register for device notifications
  - [ ] `DBT_DEVICEARRIVAL` - Drive plugged in
  - [ ] `DBT_DEVICEREMOVECOMPLETE` - Drive removed
  - [ ] `UnregisterDeviceNotification` - Cleanup on exit
  
- [ ] **Volume Management**
  - [ ] `DeleteVolumeMountPoint` - Remove drive letter
  - [ ] `SetVolumeMountPoint` - Restore drive letter
  - [ ] `GetVolumeInformation` - Get volume details

### Etcher SDK Architecture Insights for C++ Implementation

Based on the etcher-sdk research, here's how to map TypeScript patterns to C++:

1. **Source/Destination Pattern** (TypeScript → C++)
   ```typescript
   // Etcher SDK (TypeScript)
   const source = new sourceDestination.File({ path: '/path/to/image.img' });
   await source.open();
   const metadata = await source.getMetadata();
   ```
   ```cpp
   // S.A.K. Utility (C++)
   ImageSource* source = new FileImageSource("C:\\path\\to\\image.img");
   source->open();
   ImageMetadata metadata = source->getMetadata();
   ```

2. **Progress Callbacks** (TypeScript → C++)
   ```typescript
   // Etcher SDK
   onProgress: (progress) => {
       console.log(`${progress.type}: ${progress.percentage}%`);
       console.log(`Speed: ${progress.speed} bytes/s`);
   }
   ```
   ```cpp
   // S.A.K. Utility - Use Qt signals
   connect(flashWorker, &FlashWorker::progressUpdated, 
           this, [](const FlashProgress& progress) {
       qDebug() << progress.state << progress.percentage << "%";
       qDebug() << "Speed:" << progress.speedMBps << "MB/s";
   });
   ```

3. **Multi-Write Pattern** (TypeScript → C++)
   ```typescript
   // Etcher SDK
   const result = await multiWrite.pipeSourceToDestinations({
       source,
       destinations: [drive1, drive2],
       verify: true,
       numBuffers: 16,
       onProgress: progressHandler,
       onFail: failHandler
   });
   ```
   ```cpp
   // S.A.K. Utility
   MultiWriteCoordinator* coordinator = new MultiWriteCoordinator();
   coordinator->setSource(imageSource);
   coordinator->addDestination(drive1);
   coordinator->addDestination(drive2);
   coordinator->setVerifyEnabled(true);
   coordinator->setBufferCount(16);
   connect(coordinator, &MultiWriteCoordinator::progressUpdated, ...);
   connect(coordinator, &MultiWriteCoordinator::driveFailed, ...);
   WriteResult result = coordinator->execute();
   ```

4. **Scanner Event Pattern** (TypeScript → C++)
   ```typescript
   // Etcher SDK
   const scanner = new Scanner(adapters);
   scanner.on('attach', (drive) => { /* handle */ });
   scanner.on('detach', (drive) => { /* handle */ });
   await scanner.start();
   ```
   ```cpp
   // S.A.K. Utility - Qt signals
   DriveScanner* scanner = new DriveScanner(this);
   connect(scanner, &DriveScanner::driveAttached, this, &Panel::onDriveAttached);
   connect(scanner, &DriveScanner::driveDetached, this, &Panel::onDriveDetached);
   scanner->start();
   ```

---

## Configuration Settings

### Settings File Structure (`portable.ini` or registry)
```ini
[ImageFlasher]
validation_enabled=true
unmount_on_completion=true
auto_detect_compression=true
enable_notifications=true
error_reporting=false
trim_ext_partitions=true
decompress_first=false
buffer_size_mb=64
show_system_drive_warning=true
show_large_drive_warning=true
large_drive_threshold_gb=32
max_concurrent_writes=4
verification_sample_size_mb=100
```

---

## Security Considerations

### Elevation Requirements
- [ ] Require administrator privileges for:
  - [ ] Drive locking/unlocking
  - [ ] Direct disk access
  - [ ] Volume dismounting
- [ ] Use `ElevationManager` (already in project)
  - [ ] Prompt for elevation before flash
  - [ ] Spawn elevated child process if needed
  - [ ] Handle elevation denial gracefully

### Data Safety
- [ ] Multiple confirmation dialogs
- [ ] Clear visual warnings for destructive operations
- [ ] Prevent writes to system drive (C:)
- [ ] Log all operations for audit trail
- [ ] Atomic operations (complete or rollback)
- [ ] Verify source drive != target drive

---

## Error Handling

### Error Categories
1. **Pre-Flight Errors**
   - [ ] Image file not found
   - [ ] Image file unreadable
   - [ ] Unsupported format
   - [ ] Drive not detected
   - [ ] Drive too small for image
   - [ ] Insufficient permissions

2. **Operation Errors**
   - [ ] Drive unplugged during operation
   - [ ] Write failure (bad sectors)
   - [ ] Decompression failure
   - [ ] Out of memory
   - [ ] Timeout errors

3. **Verification Errors**
   - [ ] Checksum mismatch
   - [ ] Partial write detected
   - [ ] Read-back failure

### Error Dialog Display
- [ ] User-friendly error messages
- [ ] Technical details (expandable)
- [ ] Error code display
- [ ] Recovery suggestions
- [ ] Retry/Cancel options
- [ ] Save error log option

---

## Performance Optimizations

### Write Performance
- [ ] Large buffer sizes (configurable, default 64MB)
- [ ] Unbuffered I/O for direct disk access
- [ ] Sequential write patterns
- [ ] Memory-mapped files for source images
- [ ] Multi-threaded decompression

### Memory Management
- [ ] Stream-based processing (avoid loading entire image)
- [ ] Reusable buffer pools
- [ ] Compressed data caching
- [ ] Explicit memory cleanup

### Progress Updates
- [ ] Throttled updates (max 10 per second)
- [ ] Batched progress signals
- [ ] Background thread for UI updates

---

## Documentation

### User Documentation
- [ ] Add section to `README.md`
- [ ] Create user guide
  - [ ] How to select an image
  - [ ] How to identify the correct drive
  - [ ] Safety warnings
  - [ ] Troubleshooting common errors
- [ ] Add tooltips to all UI elements

### Developer Documentation
- [ ] Architecture overview document
- [ ] API documentation (Doxygen comments)
- [ ] Code examples for extending
- [ ] Testing guidelines

---

## Success Criteria

### Functional Requirements
- [ ] Successfully flash ISO/IMG/WIC files to USB/SD
- [ ] Support for compressed images (ZIP, GZ, BZ2, XZ)
- [ ] Automatic drive detection and refresh
- [ ] System drive protection with warnings
- [ ] Write verification with hash checking
- [ ] Multi-drive flashing capability
- [ ] Real-time progress and speed display
- [ ] Graceful error handling and recovery
- [ ] Admin elevation when required

### Performance Requirements
- [ ] Write speed: >= 20 MB/s on USB 2.0
- [ ] Write speed: >= 50 MB/s on USB 3.0
- [ ] UI remains responsive during operations
- [ ] Progress updates at least 5 times per second
- [ ] Memory usage < 500MB for single flash
- [ ] Support images up to 32GB

### Quality Requirements
- [ ] Zero data loss on cancellation
- [ ] Zero writes to unselected drives
- [ ] All errors logged to file
- [ ] 100% test coverage for core logic
- [ ] No crashes on unexpected conditions
- [ ] Clean resource cleanup on exit

---

## Timeline Estimate

| Phase | Duration | Tasks |
|-------|----------|-------|
| Phase 1: Core Infrastructure | 1 week | Drive detection, Image handling, Flash engine base |
| Phase 2: Write Operations | 1 week | Unmounting, Writing, Multi-write, Decompression |
| Phase 3: Verification System | 1 week | Checksums, Validation, Error detection |
| Phase 4: User Interface | 1 week | Panel, Dialogs, Widgets, Progress display |
| Phase 5: Integration & Testing | 1 week | Main window, Settings, Tests, Documentation |
| **Total** | **5 weeks** | **Complete Image Flasher feature** |

---

## Built-in Image Download Feature

### Download Latest Windows 11 ISO (High Priority Feature)
- [ ] Create "Download Windows 11" button in Image Selection widget
- [ ] Implement `WindowsISODownloader` class
  - [ ] Use Microsoft's official Windows 11 download API
  - [ ] Scrape download page: https://www.microsoft.com/software-download/windows11
  - [ ] Parse available editions (Home, Pro, Education)
  - [ ] Parse available languages
  - [ ] Get direct download link from Microsoft's servers
- [ ] Download method: **Microsoft Media Creation Tool API**
  - Use the same API that Media Creation Tool uses
  - Direct ISO download without running executable
  - Workflow:
    1. Request product download page
    2. Extract session ID and product edition ID
    3. Submit product selection (Windows 11)
    4. Get available languages
    5. Submit language selection
    6. Receive time-limited download URL (valid ~24 hours)
    7. Download ISO with progress tracking
  - Key endpoints:
    - Product page: `https://www.microsoft.com/en-us/software-download/windows11`
    - Download API: `https://www.microsoft.com/en-US/api/controls/contentinclude/html`
    - Parameters: `pageId`, `skuId`, `sessionId`, `productEditionId`, `language`
  - Response parsing:
    - HTML contains JavaScript with download URLs
    - Extract 64-bit and ARM64 download links
    - Parse expiration timestamp
  - Supported editions:
    - Windows 11 Home/Pro (consumer)
    - Windows 11 Education
    - Windows 11 Enterprise (requires licensing)

- [ ] Implementation details:
  ```cpp
  class WindowsISODownloader : public QObject {
  public:
      // Initialize with Media Creation Tool API
      WindowsISODownloader(QObject* parent = nullptr);
      
      // Step 1: Get session and product info from download page
      void fetchProductPage();
      
      // Step 2: Get available languages for Windows 11
      void fetchAvailableLanguages();
      
      // Step 3: Request download URL for selected language and arch
      void requestDownloadUrl(QString language, QString architecture);
      
      // Step 4: Download ISO with progress tracking
      void downloadISO(QString url, QString savePath);
      
      // Utility methods
      QStringList getAvailableLanguages() const;
      QStringList getAvailableArchitectures() const; // x64, ARM64
      QString parseDownloadUrl(QString html);
      bool isDownloadUrlExpired(QString url);
      
      // Cancel ongoing operations
      void cancel();
      
  signals:
      void productPageFetched(QString sessionId, QString productEditionId);
      void languagesFetched(QStringList languages);
      void downloadUrlReceived(QString url, QDateTime expiresAt);
      void downloadProgress(qint64 bytesReceived, qint64 bytesTotal, double speedMBps);
      void downloadComplete(QString filePath, QString checksum);
      void downloadError(QString error);
      void statusMessage(QString message);
      
  private:
      QNetworkAccessManager* m_networkManager;
      QString m_sessionId;
      QString m_productEditionId;
      QString m_skuId;
      QNetworkReply* m_currentReply;
      QFile* m_downloadFile;
      QTime m_downloadStartTime;
      qint64 m_lastBytesReceived;
  };
  ```

- [ ] UI Features:
  - [ ] "Download Windows 11" button (prominent placement)
  - [ ] Download dialog with options:
    - [ ] Language selector (auto-detect system language as default)
      - English (United States)
      - English (United Kingdom)
      - Spanish, French, German, Italian, Portuguese, Japanese, Chinese, etc.
    - [ ] Architecture selector (auto-detect system architecture)
      - 64-bit (x64) - default for most systems
      - ARM64 - for ARM-based PCs
    - [ ] Edition note (explains download is multi-edition)
    - [ ] Save location picker (default: Downloads folder)
    - [ ] Estimated download size: ~5.5 GB
  - [ ] Download progress dialog:
    - [ ] Overall progress bar (0-100%)
    - [ ] Status text: "Fetching download URL...", "Downloading...", "Verifying..."
    - [ ] Downloaded: X.X GB / 5.5 GB
    - [ ] Speed: XX.X MB/s
    - [ ] Time remaining: X minutes
    - [ ] Cancel button
  - [ ] Post-download actions:
    - [ ] "Flash Now" button - auto-select ISO and go to drive selection
    - [ ] "Open Folder" button - show downloaded file
    - [ ] "Flash Later" button - close dialog, ISO remains in list

- [ ] Technical implementation:
  - [ ] Use QNetworkAccessManager for all HTTP requests
  - [ ] Set proper User-Agent header (mimic Media Creation Tool)
    - `User-Agent: Windows-Update-Agent/10.0.10011.16384 Client-Protocol/1.40`
  - [ ] Handle cookies and session state
  - [ ] Parse HTML responses (use QRegularExpression or simple parsing)
  - [ ] Extract JSON data embedded in JavaScript
  - [ ] Timeout handling (30 seconds for API calls, no timeout for download)
  - [ ] Resume capability with HTTP Range headers (if connection drops)
  - [ ] No official SHA-256 from API (verify file size instead)
  - [ ] Download to temp file first: `{temp}/Win11_English_x64.tmp`
  - [ ] Rename on success: `{downloads}/Win11_English_x64.iso`
  - [ ] Handle download interruptions gracefully:
    - Save partial download
    - Resume from last position if URL still valid
    - Re-request URL if expired
  - [ ] Retry logic for network failures (3 attempts with exponential backoff)
  - [ ] Progress throttling (update UI max 10 times per second)
  - [ ] Bandwidth monitoring (calculate speed over 1-second window)

### Other Popular OS Downloads
- [ ] **Windows 10 ISO**
  - Similar implementation to Windows 11
  - Endpoint: https://www.microsoft.com/software-download/windows10

- [ ] **Ubuntu Desktop/Server**
  - Parse releases from: https://releases.ubuntu.com/
  - LTS versions: 24.04, 22.04, 20.04
  - Regular releases: Latest version
  - Direct download links available

- [ ] **Debian**
  - Official mirrors: https://www.debian.org/CD/http-ftp/
  - Stable, Testing, Unstable releases
  - Network installer vs full ISO

- [ ] **Raspberry Pi OS**
  - API: https://downloads.raspberrypi.org/
  - Raspberry Pi OS Lite, Desktop, Full
  - 32-bit and 64-bit variants

- [ ] **Linux Mint**
  - Download page: https://www.linuxmint.com/download.php
  - Editions: Cinnamon, MATE, Xfce
  - Direct download links

### Download Manager Integration
- [ ] Queue multiple downloads
- [ ] Download scheduling (off-peak hours)
- [ ] Bandwidth limiting
- [ ] Download history
- [ ] Automatic checksum verification
- [ ] "Quick Download" presets for common ISOs

## Future Enhancements (Post-v1.0)

- [ ] Support for Raspberry Pi USB boot mode (like Etcher)
- [ ] Network image sources (HTTP/HTTPS URLs) from custom servers
- [ ] Persistent USB (live Linux with persistence)
- [ ] Custom partition layouts
- [ ] Batch operations (queue multiple flashes)
- [ ] Scheduled flashing
- [ ] Remote flashing (network drives)
- [ ] Cloud image sources (AWS, Azure, Google Cloud)
- [ ] Torrent image downloads
- [ ] Image library/catalog with ratings
- [ ] Auto-update image database
- [ ] Ventoy multi-boot USB creation
- [ ] ISO to WIM conversion tools

---

## Notes

### Differences from Etcher
- **S.A.K. Utility is Windows-only** - No need for cross-platform abstractions
- **Focus on PC technician workflows** - Batch operations, logging, reporting
- **Integration with existing tools** - Combine with backup/restore, app migration
- **Simpler UI** - Match existing S.A.K. design language
- **No Electron** - Native Qt application = better performance

### Key Advantages
- **Faster startup** - Native app vs Electron
- **Lower memory usage** - No Chromium overhead
- **Better Windows integration** - Native APIs, UAC handling
- **Portable mode** - Works from USB drive
- **Technician features** - Logging, batch operations, reporting

---

## References

- **Etcher GitHub**: https://github.com/balena-io/etcher
- **Etcher SDK**: https://github.com/balena-io/etcher-sdk (TypeScript-based, reference only)
- **Windows Raw Disk I/O**: https://docs.microsoft.com/en-us/windows/win32/fileio/raw-disk-access
- **Qt Threading**: https://doc.qt.io/qt-6/thread-basics.html
- **SHA-512 in OpenSSL**: https://www.openssl.org/docs/man3.0/man3/SHA512.html

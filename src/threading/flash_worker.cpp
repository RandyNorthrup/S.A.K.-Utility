// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file flash_worker.cpp
/// @brief Implements the background worker thread for USB image flashing operations

#include "sak/flash_worker.h"

#include "sak/drive_scanner.h"
#include "sak/keep_awake.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QRandomGenerator>
#include <QtGlobal>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <vector>

#include <windows.h>

#include <winioctl.h>

#pragma comment(lib, "setupapi.lib")

namespace {
constexpr qint64 kFlashBufferSize = 64LL * 1024 * 1024;   // 64 MB
constexpr qint64 kVerifySampleMax = 100LL * 1024 * 1024;  // 100 MB
constexpr qint64 kVerifyBlockSize = 1024LL * 1024;        // 1 MB
constexpr qint64 kDeviceSectorSizeBytes = 512;
constexpr qint64 kPaddingCapacityGrowthLimit = 2;
constexpr qint64 kSampleSizeFractionDivisor = 10;
constexpr qint64 kProgressThrottleMs = sak::kTimerPollingFastMs;
constexpr qint64 kSpeedUpdateIntervalMs = sak::kMillisecondsPerSecond;

// Sector-aligned heap buffer for FILE_FLAG_NO_BUFFERING device I/O. Unbuffered
// reads/writes require the buffer's BASE ADDRESS to be a whole multiple of the
// volume's logical sector size; QByteArray only guarantees ~16-byte alignment,
// which fails on strict 4Kn drivers. _aligned_malloc gives sector alignment.
class AlignedBuffer {
public:
    AlignedBuffer() = default;
    ~AlignedBuffer() { reset(); }
    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;
    AlignedBuffer(AlignedBuffer&&) = delete;
    AlignedBuffer& operator=(AlignedBuffer&&) = delete;

    // alignment MUST be a power of two (real sector sizes 512/4096 are); a bogus
    // geometry makes _aligned_malloc fail and this returns false (fail closed).
    [[nodiscard]] bool allocate(qint64 bytes, qint64 alignment) {
        reset();
        if (bytes <= 0 || alignment <= 0) {
            return false;
        }
        m_data = static_cast<char*>(
            _aligned_malloc(static_cast<size_t>(bytes), static_cast<size_t>(alignment)));
        if (m_data == nullptr) {
            return false;
        }
        m_size = bytes;
        return true;
    }

    [[nodiscard]] char* data() { return m_data; }
    [[nodiscard]] qint64 size() const { return m_size; }

private:
    void reset() {
        if (m_data != nullptr) {
            _aligned_free(m_data);
            m_data = nullptr;
        }
        m_size = 0;
    }

    char* m_data = nullptr;
    qint64 m_size = 0;
};

// Read-back throughput for a verify pass: MB/s over the given elapsed window, or
// 0.0 when no time elapsed (avoids a divide-by-zero and a misleading value).
double verificationSpeedMBps(qint64 bytes, qint64 elapsed_ms) {
    if (elapsed_ms <= 0) {
        return 0.0;
    }
    return (static_cast<double>(bytes) / sak::kBytesPerMBf) /
           (static_cast<double>(elapsed_ms) / sak::kMillisecondsPerSecondF);
}

// Emit the standard flash-completed log line: the elapsed wall-clock window
// rendered as fractional seconds.
void logFlashCompletion(qint64 elapsed_ms) {
    sak::logInfo(QString("Flash completed in %1 seconds")
                     .arg(static_cast<double>(elapsed_ms) / sak::kMillisecondsPerSecondF)
                     .toStdString());
}

// Fail closed: if fewer than the intended sample blocks were read back and compared (all reads
// failed, a partial-read failure, or mid-loop cancellation), the flash cannot be called verified.
void markIncompleteVerification(sak::ValidationResult& result, int verified, int expected) {
    if (verified < expected) {
        result.passed = false;
        result.errors.append(
            QString("Sample verification incomplete: only %1 of %2 blocks were read back")
                .arg(verified)
                .arg(expected));
    }
}

// Pick up to @p num_samples DISTINCT block indices in [0, total_blocks). Sampling
// WITHOUT replacement makes the advertised sample count map to that many distinct
// blocks; a per-iteration bounded() draw samples WITH replacement, which can re-verify
// one block and silently under-cover the image. Returns fewer than num_samples only
// when the image holds fewer blocks than requested (then markIncompleteVerification
// fails the pass closed).
QList<qint64> pickDistinctBlockIndices(qint64 total_blocks, int num_samples) {
    QList<qint64> indices;
    if (total_blocks <= 0 || num_samples <= 0) {
        return indices;
    }
    // Cannot draw more distinct blocks than exist.
    const qint64 wanted = std::min<qint64>(num_samples, total_blocks);
    // Dense sample: enumerate every block and partially shuffle. Rejection sampling
    // below degrades toward the coupon-collector limit as wanted approaches
    // total_blocks; take this branch once the sample would cover at least half the
    // blocks (wanted >= total_blocks / kDenseSampleDivisor).
    constexpr qint64 kDenseSampleDivisor = 2;
    if (wanted * kDenseSampleDivisor >= total_blocks) {
        indices.reserve(total_blocks);
        for (qint64 block = 0; block < total_blocks; ++block) {
            indices.append(block);
        }
        for (qint64 i = 0; i < wanted; ++i) {
            const qint64 j = i + QRandomGenerator::global()->bounded(total_blocks - i);
            indices.swapItemsAt(i, j);
        }
        indices.resize(wanted);
        return indices;
    }
    // Sparse sample: rejection-sample distinct indices (expected draws ~ wanted).
    std::unordered_set<qint64> seen;
    seen.reserve(static_cast<size_t>(wanted));
    indices.reserve(wanted);
    while (static_cast<qint64>(indices.size()) < wanted) {
        const qint64 idx = QRandomGenerator::global()->bounded(total_blocks);
        if (seen.insert(idx).second) {
            indices.append(idx);
        }
    }
    return indices;
}

// Parse the physical-drive number from a "\\.\PhysicalDriveN" path; -1 if absent.
int parseTargetDriveNumber(const QString& path) {
    const QString prefix = QStringLiteral("PhysicalDrive");
    const int idx = static_cast<int>(path.lastIndexOf(prefix));
    if (idx < 0) {
        return -1;
    }
    bool ok = false;
    const int number = path.mid(idx + prefix.size()).toInt(&ok);
    return ok ? number : -1;
}

// Reads the physical-disk number the given OPEN device handle actually refers to
// (IOCTL_STORAGE_GET_DEVICE_NUMBER); -1 on a query failure, a short reply, or a
// non-disk device. Used to re-confirm, at write-handle-open time, that a
// "\\.\PhysicalDriveN" path still opens disk N: FlashCoordinator validated this
// earlier on a SEPARATE handle it has since closed, so a hot-plug or a DosDevice-
// alias remap in the interim could otherwise redirect the raw write to a different
// disk. Mirrors FlashCoordinator::queryHandleDriveNumber (same fail-closed guards).
int handleStorageDriveNumber(HANDLE handle) {
    STORAGE_DEVICE_NUMBER device_number = {};
    DWORD bytes_returned = 0;
    if (DeviceIoControl(handle,
                        IOCTL_STORAGE_GET_DEVICE_NUMBER,
                        nullptr,
                        0,
                        &device_number,
                        sizeof(device_number),
                        &bytes_returned,
                        nullptr) == 0) {
        return -1;
    }
    // A short "success" would leave the zero-initialized struct in place and let a
    // zeroed DeviceNumber pass for a real answer. Require the whole record back.
    if (bytes_returned < sizeof(device_number)) {
        return -1;
    }
    if (device_number.DeviceType != FILE_DEVICE_DISK) {
        return -1;
    }
    return static_cast<int>(device_number.DeviceNumber);
}

// Defense-in-depth OS-disk self-guard: does physical drive @p driveNumber back the
// running %WINDIR% volume? Returns true ONLY on a positive match. The authoritative
// fail-closed guard is FlashCoordinator; this independent check just ensures a
// FlashWorker constructed directly (bypassing the coordinator) still cannot raw-
// write the current system disk. An indeterminate probe returns false (proceed) so
// it never blocks a legitimate flash the coordinator already cleared.
bool physicalDriveBacksWindows(int drive_number) {
    if (drive_number < 0) {
        return false;
    }
    wchar_t win_dir[MAX_PATH] = {};
    const UINT len = GetWindowsDirectoryW(win_dir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH || win_dir[1] != L':') {
        return false;
    }
    const wchar_t volume_path[] = {L'\\', L'\\', L'.', L'\\', win_dir[0], L':', L'\0'};
    HANDLE h_vol = CreateFileW(
        volume_path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h_vol == INVALID_HANDLE_VALUE) {
        return false;
    }
    constexpr DWORD kMaxExtents = 16;
    const DWORD buf_size = sizeof(VOLUME_DISK_EXTENTS) + ((kMaxExtents - 1) * sizeof(DISK_EXTENT));
    std::vector<unsigned char> buffer(buf_size, 0);
    auto* extents = reinterpret_cast<VOLUME_DISK_EXTENTS*>(buffer.data());
    DWORD bytes_returned = 0;
    const BOOL ok = DeviceIoControl(h_vol,
                                    IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                                    nullptr,
                                    0,
                                    extents,
                                    buf_size,
                                    &bytes_returned,
                                    nullptr);
    CloseHandle(h_vol);
    if (ok == 0) {
        return false;
    }
    for (DWORD i = 0; i < extents->NumberOfDiskExtents && i < kMaxExtents; ++i) {
        if (static_cast<int>(extents->Extents[i].DiskNumber) == drive_number) {
            return true;
        }
    }
    return false;
}
}  // namespace

// Neither constructor input is asserted: both are caller-supplied, and both are rejected at
// runtime instead -- execute() refuses a null image source, and openDevice() fails the run on a
// device path it cannot open (an empty one included).
FlashWorker::FlashWorker(std::unique_ptr<ImageSource> image_source,
                         const QString& target_device,
                         QObject* parent)
    : WorkerBase(parent)
    , m_imageSource(std::move(image_source))
    , m_targetDevice(target_device)
    , m_deviceHandle(INVALID_HANDLE_VALUE)
    , m_bytesWritten(0)
    , m_totalBytes(0)
    , m_contentBytesWritten(0)
    , m_speedMBps(0.0)
    , m_bufferSize(kFlashBufferSize)
    , m_sectorSize(kDeviceSectorSizeBytes)
    , m_verificationEnabled(true)
    , m_validationMode(sak::ValidationMode::Full)
    , m_lastProgressUpdate(0)
    , m_lastSpeedUpdate(0)
    , m_lastSpeedBytes(0)
    , m_lastVerifyUpdate(0) {}

FlashWorker::~FlashWorker() {
    // Join the flash thread BEFORE closing the device or freeing the image source.
    // ~WorkerBase joins too late (after these members are gone), so on a coordinator
    // wait-timeout a still-running execute() would touch a closed handle / freed
    // source -> use-after-free and corrupted media. stopAndJoin() is idempotent.
    stopAndJoin();
    closeDevice();
}

void FlashWorker::setVerificationEnabled(bool enabled) {
    m_verificationEnabled = enabled;
}

void FlashWorker::setValidationMode(sak::ValidationMode mode) {
    m_validationMode = mode;
}

void FlashWorker::setBufferSize(qint64 size_bytes) {
    if (size_bytes <= 0) {
        sak::logWarning("FlashWorker::setBufferSize: ignoring non-positive size {}", size_bytes);
        return;
    }
    m_bufferSize = size_bytes;
}

auto FlashWorker::execute() -> std::expected<void, sak::error_code> {
    // Fail closed: the caller-supplied source is dereferenced by every stage below.
    if (!m_imageSource) {
        sak::logError("Refusing to flash: no image source");
        Q_EMIT error("No image source to flash");
        return std::unexpected(sak::error_code::invalid_argument);
    }

    const sak::KeepAwakeGuard keep_awake(sak::KeepAwake::PowerRequest::System,
                                         "Flashing disk image");
    sak::logInfo(QString("Starting flash to %1").arg(m_targetDevice).toStdString());

    QElapsedTimer timer;
    timer.start();

    // Open image source
    if (!m_imageSource->open()) {
        sak::logError("Failed to open image source");
        Q_EMIT error("Failed to open image source");
        return std::unexpected(sak::error_code::file_not_found);
    }

    m_totalBytes = m_imageSource->size();

    // Open target device
    if (!openDevice()) {
        sak::logError(QString("Failed to open device: %1").arg(m_targetDevice).toStdString());
        Q_EMIT error("Failed to open target device");
        cleanupFlashResources();
        return std::unexpected(sak::error_code::file_not_found);
    }

    // Defense in depth: refuse to raw-write the running OS disk even if this worker
    // was constructed directly, bypassing FlashCoordinator's authoritative guard.
    if (refuseIfTargetIsOsDisk()) {
        return std::unexpected(sak::error_code::validation_failed);
    }

    // Fail closed when the image cannot fit (oversized write would clobber the
    // whole device before the tail write fails at end-of-media).
    if (!ensureImageFitsTarget()) {
        return std::unexpected(sak::error_code::invalid_argument);
    }

    lockAndDismountBestEffort();

    // Write image. A user cancellation surfaces as WorkerBase::cancelled() (run() checks
    // stopRequested() before this returned code is ever read), so a failure that reaches
    // the failed() channel here is a genuine write I/O failure, not a cancellation --
    // report it as such rather than collapsing it into operation_cancelled.
    if (!writeImage()) {
        sak::logError("Failed to write image");
        Q_EMIT error("Failed to write image");
        cleanupFlashResources();
        return std::unexpected(sak::error_code::write_error);
    }

    Q_EMIT writeCompleted(m_bytesWritten.load(std::memory_order_relaxed));

    if (auto verified = runVerificationStage(); !verified) {
        return verified;
    }

    // Cleanup
    cleanupFlashResources();

    logFlashCompletion(timer.elapsed());

    return {};
}

void FlashWorker::cleanupFlashResources() {
    unlockVolume();
    closeDevice();
    m_imageSource->close();
}

auto FlashWorker::runVerificationStage() -> std::expected<void, sak::error_code> {
    if (!m_verificationEnabled || stopRequested()) {
        return {};
    }
    const sak::ValidationResult result = verifyImage();
    Q_EMIT verificationCompleted(result);
    if (!result.passed) {
        sak::logError("Verification failed");
        // Do NOT emit error() here -- verificationCompleted already carries the failure
        // info and the coordinator handles it via onWorkerCompleted(); a second signal
        // would double-count the drive as failed.
        cleanupFlashResources();
        // runVerificationStage() returns {} early when stopRequested(), so reaching here
        // means verification genuinely FAILED -- report verification_failed, not the
        // misleading operation_cancelled the old coarse mapping emitted.
        return std::unexpected(sak::error_code::verification_failed);
    }
    return {};
}

bool FlashWorker::refuseIfTargetIsOsDisk() {
    const int drive_number = parseTargetDriveNumber(m_targetDevice);
    // This worker raw-writes physical drives only ("\\.\PhysicalDriveN"). A target that is not a
    // parseable physical drive (e.g. a bare volume path like "\\.\C:") cannot be checked against
    // the OS disk, yet openDevice() may still have opened it -- so fail closed rather than proceed
    // to raw-write an unverifiable target. The coordinator only ever constructs physical-drive
    // targets, so this refuses only a direct, coordinator-bypassing misuse.
    if (drive_number < 0) {
        sak::logError(QString("Refusing raw write to %1: target is not a physical-drive path")
                          .arg(m_targetDevice)
                          .toStdString());
        Q_EMIT error("Refusing to write a non-physical-drive target");
        cleanupFlashResources();
        return true;
    }
    if (physicalDriveBacksWindows(drive_number)) {
        sak::logError(QString("Refusing raw write to %1: it backs the current OS disk")
                          .arg(m_targetDevice)
                          .toStdString());
        Q_EMIT error("Refusing to write the current OS disk");
        cleanupFlashResources();
        return true;
    }
    // Re-run the boot-disk guard on the just-opened target. FlashCoordinator cleared this
    // drive as non-boot at validation, but on a SEPARATE handle it has since closed; a
    // hot-remove + disk-number reassignment in the interim could repoint
    // "\\.\PhysicalDriveN" at a DIFFERENT physical disk (e.g. a split-boot ESP/boot disk)
    // that openDevice()'s device-number recheck cannot distinguish -- the new disk really
    // IS number N. Refuse on a POSITIVE boot-disk match. An Undetermined probe is NOT
    // treated as a refusal here: the coordinator has already offlined this cleared target,
    // which can make its volumes transiently unqueryable (-> Undetermined), so failing
    // closed on Undetermined would reject a legitimate flash the coordinator already
    // cleared. The coordinator remains the authoritative fail-closed guard (it refuses
    // Undetermined at validation, before any offlining).
    if (DriveScanner::physicalDriveBootProbe(drive_number) == sak::DiskProbe::Yes) {
        sak::logError(QString("Refusing raw write to %1: it now resolves to a Windows boot disk")
                          .arg(m_targetDevice)
                          .toStdString());
        Q_EMIT error("Refusing to write a Windows boot/system disk");
        cleanupFlashResources();
        return true;
    }
    return false;
}

bool FlashWorker::ensureImageFitsTarget() {
    // Fail closed: the device capacity MUST be known before writing. A previous
    // "capacity unknown -> write anyway, it fails at end-of-device" fallback would
    // let an oversized image overwrite the entire device before failing.
    const qint64 device_bytes = queryDeviceCapacity();
    if (device_bytes < 0) {
        sak::logError("Refusing to flash: could not determine target device capacity");
        Q_EMIT error("Could not determine the target device capacity");
        cleanupFlashResources();
        return false;
    }
    // Fail closed on an undetermined (<0) OR empty (0) image size. An undetermined
    // size (e.g. a compressed stream whose decompressed length is unknown) leaves
    // the capacity gate unable to bound the write. A zero-length image would write
    // nothing yet pass an empty-hash verify as a false "success" -- refuse it here
    // too (defense in depth; the coordinator also rejects it up front).
    if (m_totalBytes <= 0) {
        sak::logError("Refusing to flash: image size is unknown or zero");
        Q_EMIT error("Image size is unknown or empty");
        cleanupFlashResources();
        return false;
    }
    if (imageFitsDevice(m_totalBytes, device_bytes)) {
        return true;
    }
    sak::logError(QString("Image (%1 bytes) exceeds the target device capacity (%2 bytes)")
                      .arg(m_totalBytes)
                      .arg(device_bytes)
                      .toStdString());
    Q_EMIT error("Image is larger than the target device");
    cleanupFlashResources();
    return false;
}

void FlashWorker::lockAndDismountBestEffort() {
    // Belt-and-suspenders. Non-fatal by design: the target is a physical-drive
    // handle (no single volume to lock) and the AUTHORITATIVE, fail-closed
    // dismount already ran in FlashCoordinator::unmountVolumes(), which must
    // succeed before this worker is ever started.
    const bool locked = lockVolume();
    const bool dismounted = dismountVolume();
    if (!locked || !dismounted) {
        sak::logInfo("Proceeding with raw write; target volumes were unmounted by the coordinator");
    }
}

bool FlashWorker::openDevice() {
    m_deviceHandle = CreateFileW(reinterpret_cast<LPCWSTR>(m_targetDevice.utf16()),
                                 GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr,
                                 OPEN_EXISTING,
                                 FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
                                 nullptr);

    if (m_deviceHandle == INVALID_HANDLE_VALUE) {
        DWORD const last_error = GetLastError();
        sak::logError(QString("CreateFile failed with error %1").arg(last_error).toStdString());
        return false;
    }

    // Re-assert device identity on THIS write handle. FlashCoordinator proved the
    // target opened the disk its name claims, but on a SEPARATE handle it has since
    // closed, and workers are queued and started well after that check -- between the
    // two, a hot-plug or a DosDevice-alias remap could repoint "\\.\PhysicalDriveN" at
    // a DIFFERENT disk, redirecting the raw write. For a physical-drive path, require
    // the handle we are about to raw-write to really refer to disk N and fail closed on
    // any mismatch or unreadable identity (verify, do not assume). A non-physical path
    // names no disk number to check; refuseIfTargetIsOsDisk() fails that closed just
    // after openDevice() returns, so its dedicated branch stays live.
    const int parsed_number = parseTargetDriveNumber(m_targetDevice);
    if (parsed_number >= 0) {
        const int actual_number = handleStorageDriveNumber(m_deviceHandle);
        if (actual_number != parsed_number) {
            sak::logError(QString("Refusing raw write to %1: the opened device is disk %2, not %3 "
                                  "(hot-plug or alias remap after validation)")
                              .arg(m_targetDevice)
                              .arg(actual_number)
                              .arg(parsed_number)
                              .toStdString());
            closeDevice();
            return false;
        }
    }

    if (!queryDeviceSectorSize()) {
        closeDevice();
        return false;
    }
    return true;
}

bool FlashWorker::queryDeviceSectorSize() {
    // Fail closed: the logical sector size MUST be known before any sector-padded
    // write. A fallback to an assumed 512 bytes would let 4K-aligned writes onto a
    // 4Kn device silently overwrite most of it before the padded tail is rejected.
    m_sectorSize = 0;
    if (m_deviceHandle == INVALID_HANDLE_VALUE) {
        return false;
    }

    DISK_GEOMETRY geometry{};
    DWORD bytes_returned = 0;
    if ((DeviceIoControl(m_deviceHandle,
                         IOCTL_DISK_GET_DRIVE_GEOMETRY,
                         nullptr,
                         0,
                         &geometry,
                         sizeof(geometry),
                         &bytes_returned,
                         nullptr) != 0) &&
        geometry.BytesPerSector > 0) {
        m_sectorSize = static_cast<qint64>(geometry.BytesPerSector);
        sak::logInfo(
            QString("Target logical sector size: %1 bytes").arg(m_sectorSize).toStdString());
        return true;
    }
    sak::logError(QString("Could not query device sector size (error %1); refusing to flash")
                      .arg(GetLastError())
                      .toStdString());
    return false;
}

void FlashWorker::closeDevice() {
    if (m_deviceHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_deviceHandle);
        m_deviceHandle = INVALID_HANDLE_VALUE;
    }
}

qint64 FlashWorker::queryDeviceCapacity() {
    if (m_deviceHandle == INVALID_HANDLE_VALUE) {
        return -1;
    }
    GET_LENGTH_INFORMATION length_info{};
    DWORD bytes_returned = 0;
    if (DeviceIoControl(m_deviceHandle,
                        IOCTL_DISK_GET_LENGTH_INFO,
                        nullptr,
                        0,
                        &length_info,
                        sizeof(length_info),
                        &bytes_returned,
                        nullptr) == 0) {
        sak::logWarning(QString("Could not query device capacity (error %1)")
                            .arg(GetLastError())
                            .toStdString());
        return -1;
    }
    return static_cast<qint64>(length_info.Length.QuadPart);
}

qint64 FlashWorker::alignUpToSectorSize(qint64 bytes, qint64 sector_size) {
    if (sector_size <= 0 || bytes < 0) {
        return -1;
    }
    if (bytes % sector_size == 0) {
        return bytes;
    }
    const qint64 sectors = bytes / sector_size;  // floor
    // Guard the (sectors + 1) * sectorSize multiply against qint64 overflow.
    if (sectors > (std::numeric_limits<qint64>::max() / sector_size) - 1) {
        return -1;
    }
    return (sectors + 1) * sector_size;
}

bool FlashWorker::imageFitsDevice(qint64 image_bytes, qint64 device_bytes) {
    // Fail closed on BOTH unknowns. An unqueryable capacity (deviceBytes < 0) or
    // an undetermined image size (imageBytes < 0, e.g. a compressed stream whose
    // decompressed length the decompressor cannot report) must NOT be treated as
    // "fits" -- that would let an oversized image overwrite the whole device
    // before failing at end-of-media. Both sizes must be KNOWN.
    if (device_bytes < 0 || image_bytes < 0) {
        return false;
    }
    return image_bytes <= device_bytes;
}

bool FlashWorker::lockVolume() {
    DWORD bytes_returned = 0;
    const bool locked =
        DeviceIoControl(
            m_deviceHandle, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &bytes_returned, nullptr) !=
        0;
    if (!locked) {
        // Non-fatal: a physical-drive handle has no single volume to lock, and
        // the coordinator already fail-closed dismounted every child volume.
        sak::logInfo("Volume lock not acquired (physical-drive target / already unmounted)");
    }
    return locked;
}

bool FlashWorker::unlockVolume() {
    DWORD bytes_returned = 0;
    // Report the real outcome instead of an unconditional true. Non-fatal on the
    // cleanup path (a physical-drive handle may hold no volume lock to release), but
    // the caller should not be told the unlock succeeded when it did not.
    const BOOL ok = DeviceIoControl(
        m_deviceHandle, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &bytes_returned, nullptr);
    if (ok == 0) {
        sak::logInfo("Volume unlock not performed (physical-drive target / no lock held)");
    }
    return ok != 0;
}

bool FlashWorker::dismountVolume() {
    DWORD bytes_returned = 0;
    const bool dismounted = DeviceIoControl(m_deviceHandle,
                                            FSCTL_DISMOUNT_VOLUME,
                                            nullptr,
                                            0,
                                            nullptr,
                                            0,
                                            &bytes_returned,
                                            nullptr) != 0;
    if (!dismounted) {
        // Non-fatal: see lockVolume(); the coordinator owns the authoritative,
        // fail-closed dismount performed before this worker started.
        sak::logInfo("Volume dismount not performed (physical-drive target / already unmounted)");
    }
    return dismounted;
}

bool FlashWorker::prepareSourceChecksum() {
    Q_ASSERT(m_imageSource);  // execute() refuses a null source before writeImage() runs.
    if (!m_verificationEnabled || !m_sourceChecksum.isEmpty()) {
        return true;
    }

    sak::logInfo("Calculating source checksum");
    m_sourceChecksum = m_imageSource->calculateChecksum();
    if (m_sourceChecksum.isEmpty()) {
        sak::logError("Failed to calculate source checksum");
        return false;
    }
    sak::logInfo(QString("Source checksum: %1").arg(m_sourceChecksum).toStdString());

    // Reopen source after checksum calculation
    m_imageSource->close();
    if (!m_imageSource->open()) {
        sak::logError("Failed to reopen image source");
        return false;
    }
    return true;
}

bool FlashWorker::padAlignedBuffer(char* data, qint64& bytes_read, qint64 capacity) const {
    // Sole caller writeImage() returns early when AlignedBuffer::allocate() fails, so data() is
    // never null here.
    Q_ASSERT(data != nullptr);
    const qint64 padded = alignUpToSectorSize(bytes_read, m_sectorSize);
    if (padded < 0 || padded > capacity) {
        sak::logError(QString("Invalid sector padding: %1 -> %2 (capacity %3)")
                          .arg(bytes_read)
                          .arg(padded)
                          .arg(capacity)
                          .toStdString());
        return false;
    }
    if (padded > bytes_read) {
        std::memset(data + bytes_read, 0, static_cast<size_t>(padded - bytes_read));
        bytes_read = padded;
    }
    return true;
}

bool FlashWorker::padToSectorSize(QByteArray& buffer, qint64& bytes_read, qint64 sector_size) {
    // Fail closed on bogus geometry rather than dividing by zero / under-padding.
    if (sector_size <= 0) {
        sak::logError(
            QString("Invalid sector size for padding: %1").arg(sector_size).toStdString());
        return false;
    }
    // Fail closed on a negative valid-byte count. A negative bytesRead would make the
    // zero-fill loop below write buffer[i] at NEGATIVE indices (out-of-bounds/UB), and
    // -100 % 512 != 0 would even reach that loop. A valid count is never negative.
    if (bytes_read < 0) {
        sak::logError(
            QString("Invalid negative bytesRead for padding: %1").arg(bytes_read).toStdString());
        return false;
    }
    if (bytes_read % sector_size == 0) {
        return true;
    }

    const qint64 padded_size = ((bytes_read / sector_size) + 1) * sector_size;

    // Validate padded size is reasonable
    if (padded_size > buffer.capacity() * kPaddingCapacityGrowthLimit || padded_size < 0) {
        sak::logError(QString("Invalid padded size calculated: %1").arg(padded_size).toStdString());
        return false;
    }

    try {
        buffer.resize(padded_size);
    } catch (const std::bad_alloc&) {
        sak::logError("Failed to allocate padding buffer - out of memory");
        return false;
    }

    // Verify resize succeeded
    if (buffer.size() != padded_size) {
        sak::logError(QString("Buffer resize failed: expected %1, got %2")
                          .arg(padded_size)
                          .arg(buffer.size())
                          .toStdString());
        return false;
    }

    // Zero out padding
    for (qint64 i = bytes_read; i < padded_size; ++i) {
        buffer[i] = 0;
    }
    bytes_read = padded_size;
    return true;
}

bool FlashWorker::writeImage() {
    Q_ASSERT(m_imageSource);  // execute() refuses a null source before reaching this stage.
    sak::logInfo("Writing image");

    if (!prepareSourceChecksum()) {
        return false;
    }

    // Sector-aligned base address: FILE_FLAG_NO_BUFFERING WriteFile requires it.
    AlignedBuffer buffer;
    if (!buffer.allocate(m_bufferSize, m_sectorSize)) {
        sak::logError("Failed to allocate sector-aligned write buffer");
        return false;
    }
    m_bytesWritten = 0;
    m_contentBytesWritten = 0;

    QElapsedTimer speed_timer;
    speed_timer.start();
    m_lastSpeedUpdate = 0;
    m_lastSpeedBytes = 0;

    while (!m_imageSource->atEnd() && !stopRequested()) {
        // Bound the read by the ACTUAL allocation (buffer.size()), not m_bufferSize: the two are
        // equal here, but reading against the live member would write past the buffer if a setter
        // changed m_bufferSize after allocation. Reading into buffer.size() can never overflow it.
        qint64 bytes_read = m_imageSource->read(buffer.data(), buffer.size());
        if (bytes_read < 0) {
            sak::logError("Failed to read from image source");
            return false;
        }

        if (bytes_read == 0) {
            break;
        }

        // The meaningful (decompressed) content length, before sector padding.
        // Full verify hashes exactly this many bytes back from the device.
        m_contentBytesWritten += bytes_read;

        if (!padAlignedBuffer(buffer.data(), bytes_read, buffer.size())) {
            return false;
        }

        if (!writeChunk(buffer.data(), bytes_read)) {
            return false;
        }

        // Update progress
        const qint64 written_total = m_bytesWritten.load(std::memory_order_relaxed);
        updateProgress(written_total);
        updateSpeed(written_total);
    }

    return finalizeWrite();
}

bool FlashWorker::finalizeWrite() {
    // Flush buffers and check for failure to prevent silent data loss.
    if (FlushFileBuffers(m_deviceHandle) == 0) {
        DWORD const flush_error = GetLastError();
        sak::logError(
            QString("FlushFileBuffers failed with error %1").arg(flush_error).toStdString());
        return false;
    }

    const qint64 written = m_bytesWritten.load(std::memory_order_relaxed);
    sak::logInfo(QString("Wrote %1 bytes").arg(written).toStdString());

    // Fail closed on a truncated source. Compare the DECOMPRESSED CONTENT length
    // (m_contentBytesWritten, before sector padding) against the expected size --
    // NOT m_bytesWritten, which includes up to sectorSize-1 padding bytes and would
    // mask a sub-sector tail truncation (a source shrunk by <512 bytes mid-write
    // would still pad up to >= m_totalBytes and pass).
    const qint64 content = m_contentBytesWritten;
    if (!stopRequested() && content < m_totalBytes) {
        sak::logError(
            QString("Incomplete write: wrote %1 of %2 expected content bytes (source ended early)")
                .arg(content)
                .arg(m_totalBytes)
                .toStdString());
        return false;
    }
    return !stopRequested();
}

bool FlashWorker::writeChunk(const char* buffer, qint64 bytes_read) {
    // Sole caller writeImage() passes its AlignedBuffer, allocated (non-null) or it returned.
    Q_ASSERT(buffer != nullptr);
    // Guard against qint64 -> DWORD truncation
    if (bytes_read > static_cast<qint64>(MAXDWORD)) {
        sak::logError("Write size exceeds DWORD range");
        return false;
    }

    DWORD bytes_written_this_time = 0;
    if (WriteFile(m_deviceHandle,
                  buffer,
                  static_cast<DWORD>(bytes_read),
                  &bytes_written_this_time,
                  nullptr) == 0) {
        DWORD const last_error = GetLastError();
        sak::logError(QString("WriteFile failed with error %1").arg(last_error).toStdString());
        return false;
    }

    // Fail closed on a short write: WriteFile can succeed yet write fewer bytes (device full,
    // failing sector). Accepting it drops the chunk's tail and misaligns every later write.
    if (bytes_written_this_time != static_cast<DWORD>(bytes_read)) {
        sak::logError(QString("Short write: wrote %1 of %2 bytes")
                          .arg(bytes_written_this_time)
                          .arg(bytes_read)
                          .toStdString());
        return false;
    }

    m_bytesWritten += bytes_written_this_time;
    return true;
}

sak::ValidationResult FlashWorker::verifyImage() {
    sak::ValidationResult result;

    // Skip validation mode
    if (m_validationMode == sak::ValidationMode::Skip) {
        sak::logInfo("Verification skipped (skip mode)");
        result.passed = true;
        result.sourceChecksum = m_sourceChecksum;
        return result;
    }

    // Full validation mode
    if (m_validationMode == sak::ValidationMode::Full) {
        return verifyFull();
    }

    // Sample validation mode
    return verifySample();
}

sak::ValidationResult FlashWorker::verifyFull() {
    sak::logInfo("Starting full verification");

    sak::ValidationResult result;
    result.sourceChecksum = m_sourceChecksum;

    QElapsedTimer timer;
    timer.start();

    // Hash the DECOMPRESSED written length, not size(): the source checksum
    // covers the decompressed bytes, while the device also holds sector-padding
    // zeros. Hashing m_totalBytes (== size(), the compressed length for a
    // compressed source) would never match. m_contentBytesWritten is exactly the
    // meaningful prefix the source checksum was computed over.
    const qint64 verify_len = m_contentBytesWritten;
    const QString target_checksum = calculateChecksum(m_deviceHandle, verify_len);
    if (target_checksum.isEmpty()) {
        result.passed = false;
        result.errors.append("Failed to calculate target checksum");
        return result;
    }

    result.targetChecksum = target_checksum;

    // Compare checksums
    if (result.sourceChecksum != result.targetChecksum) {
        result.passed = false;
        result.errors.append(QString("Checksum mismatch - Source: %1, Target: %2")
                                 .arg(result.sourceChecksum)
                                 .arg(result.targetChecksum));
        sak::logError(QString("Checksum mismatch - Source: %1, Target: %2")
                          .arg(result.sourceChecksum)
                          .arg(result.targetChecksum)
                          .toStdString());
    } else {
        result.passed = true;
        sak::logInfo("Verification passed - checksums match");
    }

    result.verificationSpeed = verificationSpeedMBps(verify_len, timer.elapsed());

    return result;
}

sak::ValidationResult FlashWorker::verifySample() {
    Q_ASSERT(m_imageSource);  // execute() refuses a null source before the verify stage runs.
    sak::logInfo("Starting sample verification");

    sak::ValidationResult result;
    result.sourceChecksum = m_sourceChecksum;

    // Sample size: 100MB or 10% of image, whichever is smaller
    const qint64 sample_size = qMin(kVerifySampleMax, m_totalBytes / kSampleSizeFractionDivisor);
    const qint64 block_size = kVerifyBlockSize;
    int num_samples = static_cast<int>(sample_size / block_size);

    num_samples = std::max(num_samples, 1);

    sak::logInfo(QString("Verifying %1 sample blocks (%2 MB)")
                     .arg(num_samples)
                     .arg(sample_size / sak::kBytesPerMB)
                     .toStdString());

    QElapsedTimer timer;
    timer.start();

    QByteArray source_buffer(block_size, 0);
    // Device read-back buffer needs a sector-aligned base for NO_BUFFERING.
    AlignedBuffer target_buffer;
    if (!target_buffer.allocate(block_size, m_sectorSize)) {
        result.passed = false;
        result.errors.append("Failed to allocate sector-aligned verify buffer");
        return result;
    }

    // Reopen source for reading
    m_imageSource->close();
    if (!m_imageSource->open()) {
        result.passed = false;
        result.errors.append("Failed to reopen image source for verification");
        return result;
    }

    result.passed = true;

    // Images smaller than one sample block: compare the whole image rather than
    // auto-passing with zero bytes read back (fail-closed).
    const qint64 total_blocks = m_totalBytes / block_size;
    if (total_blocks < 1) {
        verifySmallImage(result);
        return result;
    }

    const int samples_verified = verifySampleBlocks(result,
                                                    {.num_samples = num_samples,
                                                     .block_size = block_size,
                                                     .total_blocks = total_blocks,
                                                     .sample_size = sample_size},
                                                    source_buffer.data(),
                                                    target_buffer.data());

    markIncompleteVerification(result, samples_verified, num_samples);
    result.verificationSpeed = verificationSpeedMBps(sample_size, timer.elapsed());

    sak::logInfo(QString("Sample verification complete - %1/%2 blocks verified, %3 mismatches")
                     .arg(samples_verified)
                     .arg(num_samples)
                     .arg(result.corruptedBlocks)
                     .toStdString());

    return result;
}

bool FlashWorker::compareDeviceBlock(sak::ValidationResult& result,
                                     qint64 offset_bytes,
                                     qint64 compare_len,
                                     const char* source_data,
                                     char* target_data) {
    // Both buffers come from verifySample() via verifySampleBlocks(): a sized QByteArray and an
    // AlignedBuffer whose failed allocation returns before either is passed on.
    Q_ASSERT(source_data != nullptr);
    Q_ASSERT(target_data != nullptr);
    LARGE_INTEGER li;
    li.QuadPart = offset_bytes;
    if (SetFilePointerEx(m_deviceHandle, li, nullptr, FILE_BEGIN) == 0) {
        result.errors.append(QString("Failed to seek target to offset %1").arg(offset_bytes));
        return false;
    }

    DWORD bytes_read_from_device = 0;
    if (ReadFile(m_deviceHandle,
                 target_data,
                 static_cast<DWORD>(compare_len),
                 &bytes_read_from_device,
                 nullptr) == 0) {
        result.errors.append(QString("Failed to read from device at offset %1").arg(offset_bytes));
        return false;
    }

    // Fail closed on a short device read: fewer bytes back than we compare means
    // the target buffer tail is stale, so this block cannot count as verified.
    if (static_cast<qint64>(bytes_read_from_device) != compare_len) {
        result.passed = false;
        result.errors.append(QString("Short device read at offset %1: %2 of %3 bytes")
                                 .arg(offset_bytes)
                                 .arg(bytes_read_from_device)
                                 .arg(compare_len));
        return false;
    }

    if (memcmp(source_data, target_data, static_cast<size_t>(compare_len)) != 0) {
        result.passed = false;
        result.mismatchOffset = offset_bytes;
        result.corruptedBlocks++;
        result.errors.append(QString("Data mismatch at offset %1").arg(offset_bytes));
        sak::logError(QString("Data mismatch at offset %1").arg(offset_bytes).toStdString());
    }
    return true;
}

int FlashWorker::verifySampleBlocks(sak::ValidationResult& result,
                                    const VerifyBlocksConfig& config,
                                    char* source_data,
                                    char* target_data) {
    // Sole caller verifySample() passes its sized source QByteArray and an AlignedBuffer it
    // returned early on if the allocation failed.
    Q_ASSERT(source_data != nullptr);
    Q_ASSERT(target_data != nullptr);
    int samples_verified = 0;

    // Draw the block indices WITHOUT replacement so the advertised sample count maps to
    // that many DISTINCT blocks; a per-iteration bounded() draw could re-verify one block
    // and under-cover the image. num_samples <= total_blocks here (verifySample() caps the
    // sample at 10% of the image), so this yields exactly num_samples distinct indices.
    const QList<qint64> block_indices = pickDistinctBlockIndices(config.total_blocks,
                                                                 config.num_samples);

    for (const qint64 block_index : block_indices) {
        if (stopRequested()) {
            break;
        }
        const qint64 offset_bytes = block_index * config.block_size;

        if (!m_imageSource->seek(offset_bytes)) {
            result.errors.append(QString("Failed to seek source to offset %1").arg(offset_bytes));
            continue;
        }

        // Compare only the bytes actually read. A full-block sample offset always
        // returns block_size here, keeping the device read a whole sector multiple.
        const qint64 compare_len = m_imageSource->read(source_data, config.block_size);
        if (compare_len <= 0) {
            // Seek/read failure or premature EOF; leave unverified
            // (markIncompleteVerification fails closed if too few blocks read back).
            continue;
        }

        if (!compareDeviceBlock(result, offset_bytes, compare_len, source_data, target_data)) {
            continue;
        }

        samples_verified++;
        updateVerificationProgress(samples_verified * config.block_size, config.sample_size);
    }

    return samples_verified;
}

void FlashWorker::verifySmallImage(sak::ValidationResult& result) {
    Q_ASSERT(m_imageSource);  // execute() refuses a null source before the verify stage runs.
    const qint64 len = m_totalBytes;
    if (len <= 0) {
        result.passed = false;
        result.errors.append("Image has no data to verify");
        return;
    }

    // Device reads use FILE_FLAG_NO_BUFFERING: the read length must be a whole
    // multiple of the sector size. The written image was sector-padded, so read
    // back the aligned length and compare only the meaningful prefix.
    const qint64 aligned_len = alignUpToSectorSize(len, m_sectorSize);
    if (aligned_len < 0) {
        result.passed = false;
        result.errors.append("Invalid sector geometry for small-image verification");
        return;
    }

    QByteArray source_buffer(len, 0);
    if (m_imageSource->read(source_buffer.data(), len) != len) {
        result.passed = false;
        result.errors.append("Failed to read full source for small-image verification");
        return;
    }

    LARGE_INTEGER li;
    li.QuadPart = 0;
    if (SetFilePointerEx(m_deviceHandle, li, nullptr, FILE_BEGIN) == 0) {
        result.passed = false;
        result.errors.append("Failed to seek device for small-image verification");
        return;
    }

    // Sector-aligned base address for the NO_BUFFERING device read-back.
    AlignedBuffer target_buffer;
    if (!target_buffer.allocate(aligned_len, m_sectorSize)) {
        result.passed = false;
        result.errors.append("Failed to allocate sector-aligned verify buffer");
        return;
    }
    DWORD bytes_read_from_device = 0;
    if ((ReadFile(m_deviceHandle,
                  target_buffer.data(),
                  static_cast<DWORD>(aligned_len),
                  &bytes_read_from_device,
                  nullptr) == 0) ||
        static_cast<qint64>(bytes_read_from_device) < len) {
        result.passed = false;
        result.errors.append("Failed to read full device for small-image verification");
        return;
    }

    if (memcmp(source_buffer.data(), target_buffer.data(), static_cast<size_t>(len)) != 0) {
        result.passed = false;
        result.mismatchOffset = 0;
        result.corruptedBlocks++;
        result.errors.append("Data mismatch in small-image verification");
        sak::logError("Data mismatch in small-image verification");
    }
    // On a byte-for-byte match, result.passed keeps the caller's true value.
}

QString FlashWorker::calculateChecksum(HANDLE handle, qint64 size) {
    // Sole caller verifyFull() passes m_contentBytesWritten, which writeImage() zeroes and then
    // only ever adds positive read counts to.
    Q_ASSERT(size >= 0);
    sak::logInfo("Calculating device checksum");

    // Seek to beginning
    LARGE_INTEGER li;
    li.QuadPart = 0;
    if (SetFilePointerEx(handle, li, nullptr, FILE_BEGIN) == 0) {
        sak::logError("Failed to seek to beginning for checksum");
        return QString();
    }

    // Sector-aligned base address for FILE_FLAG_NO_BUFFERING ReadFile.
    AlignedBuffer buffer;
    if (!buffer.allocate(kFlashBufferSize, m_sectorSize)) {
        sak::logError("Failed to allocate sector-aligned verify buffer");
        return QString();
    }

    QCryptographicHash hash(QCryptographicHash::Sha512);
    qint64 total_read = 0;
    m_lastVerifyUpdate = 0;

    while (total_read < size && !stopRequested()) {
        const qint64 remaining = size - total_read;
        const qint64 to_read = qMin(static_cast<qint64>(kFlashBufferSize), remaining);
        // NO_BUFFERING: the read LENGTH must also be a whole sector multiple. Read
        // the aligned length back (the tail sectors hold write-time padding) and
        // hash only the meaningful prefix so the digest matches the source.
        const qint64 aligned_read = alignUpToSectorSize(to_read, m_sectorSize);
        if (aligned_read < 0 || aligned_read > buffer.size()) {
            sak::logError("Invalid sector-aligned verify read length");
            return QString();
        }

        DWORD bytes_read = 0;
        if (ReadFile(
                handle, buffer.data(), static_cast<DWORD>(aligned_read), &bytes_read, nullptr) ==
            0) {
            sak::logError(
                QString("ReadFile failed with error %1").arg(GetLastError()).toStdString());
            return QString();
        }

        if (bytes_read == 0) {
            break;
        }

        const qint64 meaningful = qMin(remaining, static_cast<qint64>(bytes_read));
        hash.addData(QByteArrayView(buffer.data(), static_cast<qsizetype>(meaningful)));
        total_read += meaningful;
        updateVerificationProgress(total_read, size);
    }

    if (stopRequested()) {
        sak::logWarning("Checksum calculation cancelled");
        return QString();
    }

    QString result = QString(hash.result().toHex());
    sak::logInfo(QString("Device checksum: %1").arg(result).toStdString());
    return result;
}

void FlashWorker::updateVerificationProgress(qint64 bytes_verified, qint64 total_bytes) {
    // Both callers (verifySampleBlocks, calculateChecksum) pass non-negative running counters and
    // a total derived from m_totalBytes, which ensureImageFitsTarget() refuses to flash unless
    // it is positive.
    Q_ASSERT(bytes_verified >= 0);
    Q_ASSERT(total_bytes >= 0);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // Throttle updates to once per 100ms
    if (now - m_lastVerifyUpdate < kProgressThrottleMs) {
        return;
    }

    m_lastVerifyUpdate = now;

    double percentage = 0.0;
    if (total_bytes > 0) {
        percentage = (static_cast<double>(bytes_verified) / static_cast<double>(total_bytes)) *
                     sak::kPercentMaxF;
    }

    Q_EMIT verificationProgress(percentage, bytes_verified);
}

void FlashWorker::updateProgress(qint64 bytes_written) {
    // Sole caller writeImage() passes m_bytesWritten, which it zeroes and then only ever
    // increases by a successful WriteFile count.
    Q_ASSERT(bytes_written >= 0);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // Throttle updates to once per 100ms
    if (now - m_lastProgressUpdate < kProgressThrottleMs) {
        return;
    }

    m_lastProgressUpdate = now;

    double percentage = 0.0;
    if (m_totalBytes > 0) {
        percentage = (static_cast<double>(bytes_written) / static_cast<double>(m_totalBytes)) *
                     sak::kPercentMaxF;
    }

    Q_EMIT progressUpdated(percentage, bytes_written);
}

void FlashWorker::updateSpeed(qint64 bytes_written) {
    // Same m_bytesWritten counter updateProgress() is given, from the same call site.
    Q_ASSERT(bytes_written >= 0);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // Calculate speed every second
    if (now - m_lastSpeedUpdate < kSpeedUpdateIntervalMs) {
        return;
    }

    const qint64 bytes_delta = bytes_written - m_lastSpeedBytes;
    const qint64 time_delta = now - m_lastSpeedUpdate;

    if (time_delta > 0) {
        const double bytes_per_ms = static_cast<double>(bytes_delta) /
                                    static_cast<double>(time_delta);
        m_speedMBps = (bytes_per_ms * sak::kMillisecondsPerSecondF) / sak::kBytesPerMBf;
    }

    m_lastSpeedUpdate = now;
    m_lastSpeedBytes = m_bytesWritten.load(std::memory_order_relaxed);
}

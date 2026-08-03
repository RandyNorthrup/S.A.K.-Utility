// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file flash_worker.cpp
/// @brief Implements the background worker thread for USB image flashing operations

#include "sak/flash_worker.h"

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

#include <cstdlib>
#include <cstring>
#include <limits>

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
    return (bytes / sak::kBytesPerMBf) / (elapsed_ms / sak::kMillisecondsPerSecondF);
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
}  // namespace

FlashWorker::FlashWorker(std::unique_ptr<ImageSource> imageSource,
                         const QString& targetDevice,
                         QObject* parent)
    : WorkerBase(parent)
    , m_imageSource(std::move(imageSource))
    , m_targetDevice(targetDevice)
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
    , m_lastVerifyUpdate(0) {
    Q_ASSERT_X(m_imageSource != nullptr, "FlashWorker", "imageSource must not be null");
    Q_ASSERT_X(!m_targetDevice.isEmpty(), "FlashWorker", "targetDevice must not be empty");
}

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

void FlashWorker::setBufferSize(qint64 sizeBytes) {
    if (sizeBytes <= 0) {
        sak::logWarning("FlashWorker::setBufferSize: ignoring non-positive size {}", sizeBytes);
        return;
    }
    m_bufferSize = sizeBytes;
}

auto FlashWorker::execute() -> std::expected<void, sak::error_code> {
    sak::KeepAwakeGuard keep_awake(sak::KeepAwake::PowerRequest::System, "Flashing disk image");
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

    // Fail closed when the image cannot fit (oversized write would clobber the
    // whole device before the tail write fails at end-of-media).
    if (!ensureImageFitsTarget()) {
        return std::unexpected(sak::error_code::invalid_argument);
    }

    lockAndDismountBestEffort();

    // Write image
    if (!writeImage()) {
        sak::logError("Failed to write image");
        Q_EMIT error("Failed to write image");
        cleanupFlashResources();
        return std::unexpected(sak::error_code::operation_cancelled);
    }

    Q_EMIT writeCompleted(m_bytesWritten.load(std::memory_order_relaxed));

    // Verify if enabled
    if (m_verificationEnabled && !stopRequested()) {
        sak::ValidationResult result = verifyImage();
        Q_EMIT verificationCompleted(result);

        if (!result.passed) {
            sak::logError("Verification failed");
            // Don't emit error() here -- verificationCompleted already carries
            // the failure info, and the coordinator handles it via
            // onWorkerCompleted(). Emitting error() would double-count.
            cleanupFlashResources();
            return std::unexpected(sak::error_code::operation_cancelled);
        }
    }

    // Cleanup
    cleanupFlashResources();

    qint64 elapsed_ms = timer.elapsed();
    sak::logInfo(QString("Flash completed in %1 seconds")
                     .arg(elapsed_ms / sak::kMillisecondsPerSecondF)
                     .toStdString());

    return {};
}

void FlashWorker::cleanupFlashResources() {
    unlockVolume();
    closeDevice();
    m_imageSource->close();
}

bool FlashWorker::ensureImageFitsTarget() {
    // Fail closed: the device capacity MUST be known before writing. A previous
    // "capacity unknown -> write anyway, it fails at end-of-device" fallback would
    // let an oversized image overwrite the entire device before failing.
    const qint64 deviceBytes = queryDeviceCapacity();
    if (deviceBytes < 0) {
        sak::logError("Refusing to flash: could not determine target device capacity");
        Q_EMIT error("Could not determine the target device capacity");
        cleanupFlashResources();
        return false;
    }
    // Fail closed on an undetermined image size (e.g. a compressed stream whose
    // decompressed length is unknown): without a known size the capacity gate
    // cannot bound the write, so refuse rather than risk clobbering the device.
    if (m_totalBytes < 0) {
        sak::logError("Refusing to flash: could not determine the image size");
        Q_EMIT error("Could not determine the image size");
        cleanupFlashResources();
        return false;
    }
    if (imageFitsDevice(m_totalBytes, deviceBytes)) {
        return true;
    }
    sak::logError(QString("Image (%1 bytes) exceeds the target device capacity (%2 bytes)")
                      .arg(m_totalBytes)
                      .arg(deviceBytes)
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
        DWORD last_error = GetLastError();
        sak::logError(QString("CreateFile failed with error %1").arg(last_error).toStdString());
        return false;
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
    DWORD bytesReturned = 0;
    if (DeviceIoControl(m_deviceHandle,
                        IOCTL_DISK_GET_DRIVE_GEOMETRY,
                        nullptr,
                        0,
                        &geometry,
                        sizeof(geometry),
                        &bytesReturned,
                        nullptr) &&
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
    GET_LENGTH_INFORMATION lengthInfo{};
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(m_deviceHandle,
                         IOCTL_DISK_GET_LENGTH_INFO,
                         nullptr,
                         0,
                         &lengthInfo,
                         sizeof(lengthInfo),
                         &bytesReturned,
                         nullptr)) {
        sak::logWarning(QString("Could not query device capacity (error %1)")
                            .arg(GetLastError())
                            .toStdString());
        return -1;
    }
    return static_cast<qint64>(lengthInfo.Length.QuadPart);
}

qint64 FlashWorker::alignUpToSectorSize(qint64 bytes, qint64 sectorSize) {
    if (sectorSize <= 0 || bytes < 0) {
        return -1;
    }
    if (bytes % sectorSize == 0) {
        return bytes;
    }
    const qint64 sectors = bytes / sectorSize;  // floor
    // Guard the (sectors + 1) * sectorSize multiply against qint64 overflow.
    if (sectors > (std::numeric_limits<qint64>::max() / sectorSize) - 1) {
        return -1;
    }
    return (sectors + 1) * sectorSize;
}

bool FlashWorker::imageFitsDevice(qint64 imageBytes, qint64 deviceBytes) {
    // Fail closed on BOTH unknowns. An unqueryable capacity (deviceBytes < 0) or
    // an undetermined image size (imageBytes < 0, e.g. a compressed stream whose
    // decompressed length the decompressor cannot report) must NOT be treated as
    // "fits" -- that would let an oversized image overwrite the whole device
    // before failing at end-of-media. Both sizes must be KNOWN.
    if (deviceBytes < 0 || imageBytes < 0) {
        return false;
    }
    return imageBytes <= deviceBytes;
}

bool FlashWorker::lockVolume() {
    DWORD bytesReturned = 0;
    const bool locked = DeviceIoControl(
        m_deviceHandle, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &bytesReturned, nullptr);
    if (!locked) {
        // Non-fatal: a physical-drive handle has no single volume to lock, and
        // the coordinator already fail-closed dismounted every child volume.
        sak::logInfo("Volume lock not acquired (physical-drive target / already unmounted)");
    }
    return locked;
}

bool FlashWorker::unlockVolume() {
    DWORD bytesReturned = 0;
    DeviceIoControl(
        m_deviceHandle, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &bytesReturned, nullptr);
    return true;
}

bool FlashWorker::dismountVolume() {
    DWORD bytesReturned = 0;
    const bool dismounted = DeviceIoControl(
        m_deviceHandle, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &bytesReturned, nullptr);
    if (!dismounted) {
        // Non-fatal: see lockVolume(); the coordinator owns the authoritative,
        // fail-closed dismount performed before this worker started.
        sak::logInfo("Volume dismount not performed (physical-drive target / already unmounted)");
    }
    return dismounted;
}

bool FlashWorker::prepareSourceChecksum() {
    Q_ASSERT(m_imageSource);
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

bool FlashWorker::padAlignedBuffer(char* data, qint64& bytesRead, qint64 capacity) const {
    Q_ASSERT(data != nullptr);
    const qint64 padded = alignUpToSectorSize(bytesRead, m_sectorSize);
    if (padded < 0 || padded > capacity) {
        sak::logError(QString("Invalid sector padding: %1 -> %2 (capacity %3)")
                          .arg(bytesRead)
                          .arg(padded)
                          .arg(capacity)
                          .toStdString());
        return false;
    }
    if (padded > bytesRead) {
        std::memset(data + bytesRead, 0, static_cast<size_t>(padded - bytesRead));
        bytesRead = padded;
    }
    return true;
}

bool FlashWorker::padToSectorSize(QByteArray& buffer, qint64& bytesRead, qint64 sectorSize) {
    Q_ASSERT(!buffer.isEmpty());
    // Fail closed on bogus geometry rather than dividing by zero / under-padding.
    if (sectorSize <= 0) {
        sak::logError(QString("Invalid sector size for padding: %1").arg(sectorSize).toStdString());
        return false;
    }
    if (bytesRead % sectorSize == 0) {
        return true;
    }

    qint64 paddedSize = ((bytesRead / sectorSize) + 1) * sectorSize;

    // Validate padded size is reasonable
    if (paddedSize > buffer.capacity() * kPaddingCapacityGrowthLimit || paddedSize < 0) {
        sak::logError(QString("Invalid padded size calculated: %1").arg(paddedSize).toStdString());
        return false;
    }

    try {
        buffer.resize(paddedSize);
    } catch (const std::bad_alloc&) {
        sak::logError("Failed to allocate padding buffer - out of memory");
        return false;
    }

    // Verify resize succeeded
    if (buffer.size() != paddedSize) {
        sak::logError(QString("Buffer resize failed: expected %1, got %2")
                          .arg(paddedSize)
                          .arg(buffer.size())
                          .toStdString());
        return false;
    }

    // Zero out padding
    for (qint64 i = bytesRead; i < paddedSize; ++i) {
        buffer[i] = 0;
    }
    bytesRead = paddedSize;
    return true;
}

bool FlashWorker::writeImage() {
    Q_ASSERT(m_imageSource);
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

    QElapsedTimer speedTimer;
    speedTimer.start();
    m_lastSpeedUpdate = 0;
    m_lastSpeedBytes = 0;

    while (!m_imageSource->atEnd() && !stopRequested()) {
        qint64 bytesRead = m_imageSource->read(buffer.data(), m_bufferSize);
        if (bytesRead < 0) {
            sak::logError("Failed to read from image source");
            return false;
        }

        if (bytesRead == 0) {
            break;
        }

        // The meaningful (decompressed) content length, before sector padding.
        // Full verify hashes exactly this many bytes back from the device.
        m_contentBytesWritten += bytesRead;

        if (!padAlignedBuffer(buffer.data(), bytesRead, buffer.size())) {
            return false;
        }

        if (!writeChunk(buffer.data(), bytesRead)) {
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
    if (!FlushFileBuffers(m_deviceHandle)) {
        DWORD flushError = GetLastError();
        sak::logError(
            QString("FlushFileBuffers failed with error %1").arg(flushError).toStdString());
        return false;
    }

    const qint64 written = m_bytesWritten.load(std::memory_order_relaxed);
    sak::logInfo(QString("Wrote %1 bytes").arg(written).toStdString());

    // Fail closed on a truncated source: a premature zero-length read breaks the
    // write loop; without this length check a short or corrupt image would report
    // success. Padding only ever grows the tail, so a complete write has written
    // >= m_totalBytes.
    if (!stopRequested() && written < m_totalBytes) {
        sak::logError(
            QString("Incomplete write: wrote %1 of %2 expected bytes (source ended early)")
                .arg(written)
                .arg(m_totalBytes)
                .toStdString());
        return false;
    }
    return !stopRequested();
}

bool FlashWorker::writeChunk(const char* buffer, qint64 bytesRead) {
    Q_ASSERT(buffer != nullptr);
    // Guard against qint64 -> DWORD truncation
    if (bytesRead > static_cast<qint64>(MAXDWORD)) {
        sak::logError("Write size exceeds DWORD range");
        return false;
    }

    DWORD bytesWrittenThisTime = 0;
    if (!WriteFile(m_deviceHandle,
                   buffer,
                   static_cast<DWORD>(bytesRead),
                   &bytesWrittenThisTime,
                   nullptr)) {
        DWORD last_error = GetLastError();
        sak::logError(QString("WriteFile failed with error %1").arg(last_error).toStdString());
        return false;
    }

    // Fail closed on a short write: WriteFile can succeed yet write fewer bytes (device full,
    // failing sector). Accepting it drops the chunk's tail and misaligns every later write.
    if (bytesWrittenThisTime != static_cast<DWORD>(bytesRead)) {
        sak::logError(QString("Short write: wrote %1 of %2 bytes")
                          .arg(bytesWrittenThisTime)
                          .arg(bytesRead)
                          .toStdString());
        return false;
    }

    m_bytesWritten += bytesWrittenThisTime;
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
    const qint64 verifyLen = m_contentBytesWritten;
    QString targetChecksum = calculateChecksum(m_deviceHandle, verifyLen);
    if (targetChecksum.isEmpty()) {
        result.passed = false;
        result.errors.append("Failed to calculate target checksum");
        return result;
    }

    result.targetChecksum = targetChecksum;

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

    result.verificationSpeed = verificationSpeedMBps(verifyLen, timer.elapsed());

    return result;
}

sak::ValidationResult FlashWorker::verifySample() {
    Q_ASSERT(m_imageSource);
    sak::logInfo("Starting sample verification");

    sak::ValidationResult result;
    result.sourceChecksum = m_sourceChecksum;

    // Sample size: 100MB or 10% of image, whichever is smaller
    qint64 sampleSize = qMin(kVerifySampleMax, m_totalBytes / kSampleSizeFractionDivisor);
    qint64 blockSize = kVerifyBlockSize;
    int numSamples = static_cast<int>(sampleSize / blockSize);

    if (numSamples < 1) {
        numSamples = 1;
    }

    sak::logInfo(QString("Verifying %1 sample blocks (%2 MB)")
                     .arg(numSamples)
                     .arg(sampleSize / sak::kBytesPerMB)
                     .toStdString());

    QElapsedTimer timer;
    timer.start();

    QByteArray sourceBuffer(blockSize, 0);
    // Device read-back buffer needs a sector-aligned base for NO_BUFFERING.
    AlignedBuffer targetBuffer;
    if (!targetBuffer.allocate(blockSize, m_sectorSize)) {
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
    const qint64 totalBlocks = m_totalBytes / blockSize;
    if (totalBlocks < 1) {
        verifySmallImage(result);
        return result;
    }

    int samplesVerified = verifySampleBlocks(result,
                                             {numSamples, blockSize, totalBlocks, sampleSize},
                                             sourceBuffer.data(),
                                             targetBuffer.data());

    markIncompleteVerification(result, samplesVerified, numSamples);
    result.verificationSpeed = verificationSpeedMBps(sampleSize, timer.elapsed());

    sak::logInfo(QString("Sample verification complete - %1/%2 blocks verified, %3 mismatches")
                     .arg(samplesVerified)
                     .arg(numSamples)
                     .arg(result.corruptedBlocks)
                     .toStdString());

    return result;
}

bool FlashWorker::compareDeviceBlock(sak::ValidationResult& result,
                                     qint64 offset_bytes,
                                     qint64 compareLen,
                                     const char* sourceData,
                                     char* targetData) {
    Q_ASSERT(sourceData != nullptr);
    Q_ASSERT(targetData != nullptr);
    LARGE_INTEGER li;
    li.QuadPart = offset_bytes;
    if (!SetFilePointerEx(m_deviceHandle, li, nullptr, FILE_BEGIN)) {
        result.errors.append(QString("Failed to seek target to offset %1").arg(offset_bytes));
        return false;
    }

    DWORD bytesReadFromDevice = 0;
    if (!ReadFile(m_deviceHandle,
                  targetData,
                  static_cast<DWORD>(compareLen),
                  &bytesReadFromDevice,
                  nullptr)) {
        result.errors.append(QString("Failed to read from device at offset %1").arg(offset_bytes));
        return false;
    }

    // Fail closed on a short device read: fewer bytes back than we compare means
    // the target buffer tail is stale, so this block cannot count as verified.
    if (static_cast<qint64>(bytesReadFromDevice) != compareLen) {
        result.passed = false;
        result.errors.append(QString("Short device read at offset %1: %2 of %3 bytes")
                                 .arg(offset_bytes)
                                 .arg(bytesReadFromDevice)
                                 .arg(compareLen));
        return false;
    }

    if (memcmp(sourceData, targetData, static_cast<size_t>(compareLen)) != 0) {
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
                                    char* sourceData,
                                    char* targetData) {
    Q_ASSERT(sourceData != nullptr);
    Q_ASSERT(targetData != nullptr);
    int samplesVerified = 0;

    for (int i = 0; i < config.num_samples && !stopRequested(); ++i) {
        qint64 blockIndex = QRandomGenerator::global()->bounded(config.total_blocks);
        qint64 offset_bytes = blockIndex * config.block_size;

        if (!m_imageSource->seek(offset_bytes)) {
            result.errors.append(QString("Failed to seek source to offset %1").arg(offset_bytes));
            continue;
        }

        // Compare only the bytes actually read. A full-block sample offset always
        // returns block_size here, keeping the device read a whole sector multiple.
        const qint64 compareLen = m_imageSource->read(sourceData, config.block_size);
        if (compareLen <= 0) {
            // Seek/read failure or premature EOF; leave unverified
            // (markIncompleteVerification fails closed if too few blocks read back).
            continue;
        }

        if (!compareDeviceBlock(result, offset_bytes, compareLen, sourceData, targetData)) {
            continue;
        }

        samplesVerified++;
        updateVerificationProgress(samplesVerified * config.block_size, config.sample_size);
    }

    return samplesVerified;
}

void FlashWorker::verifySmallImage(sak::ValidationResult& result) {
    Q_ASSERT(m_imageSource);
    const qint64 len = m_totalBytes;
    if (len <= 0) {
        result.passed = false;
        result.errors.append("Image has no data to verify");
        return;
    }

    // Device reads use FILE_FLAG_NO_BUFFERING: the read length must be a whole
    // multiple of the sector size. The written image was sector-padded, so read
    // back the aligned length and compare only the meaningful prefix.
    const qint64 alignedLen = alignUpToSectorSize(len, m_sectorSize);
    if (alignedLen < 0) {
        result.passed = false;
        result.errors.append("Invalid sector geometry for small-image verification");
        return;
    }

    QByteArray sourceBuffer(len, 0);
    if (m_imageSource->read(sourceBuffer.data(), len) != len) {
        result.passed = false;
        result.errors.append("Failed to read full source for small-image verification");
        return;
    }

    LARGE_INTEGER li;
    li.QuadPart = 0;
    if (!SetFilePointerEx(m_deviceHandle, li, nullptr, FILE_BEGIN)) {
        result.passed = false;
        result.errors.append("Failed to seek device for small-image verification");
        return;
    }

    // Sector-aligned base address for the NO_BUFFERING device read-back.
    AlignedBuffer targetBuffer;
    if (!targetBuffer.allocate(alignedLen, m_sectorSize)) {
        result.passed = false;
        result.errors.append("Failed to allocate sector-aligned verify buffer");
        return;
    }
    DWORD bytesReadFromDevice = 0;
    if (!ReadFile(m_deviceHandle,
                  targetBuffer.data(),
                  static_cast<DWORD>(alignedLen),
                  &bytesReadFromDevice,
                  nullptr) ||
        static_cast<qint64>(bytesReadFromDevice) < len) {
        result.passed = false;
        result.errors.append("Failed to read full device for small-image verification");
        return;
    }

    if (memcmp(sourceBuffer.data(), targetBuffer.data(), static_cast<size_t>(len)) != 0) {
        result.passed = false;
        result.mismatchOffset = 0;
        result.corruptedBlocks++;
        result.errors.append("Data mismatch in small-image verification");
        sak::logError("Data mismatch in small-image verification");
    }
    // On a byte-for-byte match, result.passed keeps the caller's true value.
}

QString FlashWorker::calculateChecksum(HANDLE handle, qint64 size) {
    Q_ASSERT(size >= 0);
    sak::logInfo("Calculating device checksum");

    // Seek to beginning
    LARGE_INTEGER li;
    li.QuadPart = 0;
    if (!SetFilePointerEx(handle, li, nullptr, FILE_BEGIN)) {
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
    qint64 totalRead = 0;
    m_lastVerifyUpdate = 0;

    while (totalRead < size && !stopRequested()) {
        const qint64 remaining = size - totalRead;
        const qint64 toRead = qMin(static_cast<qint64>(kFlashBufferSize), remaining);
        // NO_BUFFERING: the read LENGTH must also be a whole sector multiple. Read
        // the aligned length back (the tail sectors hold write-time padding) and
        // hash only the meaningful prefix so the digest matches the source.
        const qint64 alignedRead = alignUpToSectorSize(toRead, m_sectorSize);
        if (alignedRead < 0 || alignedRead > buffer.size()) {
            sak::logError("Invalid sector-aligned verify read length");
            return QString();
        }

        DWORD bytesRead = 0;
        if (!ReadFile(
                handle, buffer.data(), static_cast<DWORD>(alignedRead), &bytesRead, nullptr)) {
            sak::logError(
                QString("ReadFile failed with error %1").arg(GetLastError()).toStdString());
            return QString();
        }

        if (bytesRead == 0) {
            break;
        }

        const qint64 meaningful = qMin(remaining, static_cast<qint64>(bytesRead));
        hash.addData(QByteArrayView(buffer.data(), static_cast<qsizetype>(meaningful)));
        totalRead += meaningful;
        updateVerificationProgress(totalRead, size);
    }

    if (stopRequested()) {
        sak::logWarning("Checksum calculation cancelled");
        return QString();
    }

    QString result = QString(hash.result().toHex());
    sak::logInfo(QString("Device checksum: %1").arg(result).toStdString());
    return result;
}

void FlashWorker::updateVerificationProgress(qint64 bytesVerified, qint64 totalBytes) {
    Q_ASSERT(bytesVerified >= 0);
    Q_ASSERT(totalBytes >= 0);
    qint64 now = QDateTime::currentMSecsSinceEpoch();

    // Throttle updates to once per 100ms
    if (now - m_lastVerifyUpdate < kProgressThrottleMs) {
        return;
    }

    m_lastVerifyUpdate = now;

    double percentage = 0.0;
    if (totalBytes > 0) {
        percentage = (static_cast<double>(bytesVerified) / totalBytes) * sak::kPercentMaxF;
    }

    Q_EMIT verificationProgress(percentage, bytesVerified);
}

void FlashWorker::updateProgress(qint64 bytesWritten) {
    Q_ASSERT(bytesWritten >= 0);
    qint64 now = QDateTime::currentMSecsSinceEpoch();

    // Throttle updates to once per 100ms
    if (now - m_lastProgressUpdate < kProgressThrottleMs) {
        return;
    }

    m_lastProgressUpdate = now;

    double percentage = 0.0;
    if (m_totalBytes > 0) {
        percentage = (static_cast<double>(bytesWritten) / m_totalBytes) * sak::kPercentMaxF;
    }

    Q_EMIT progressUpdated(percentage, bytesWritten);
}

void FlashWorker::updateSpeed(qint64 bytesWritten) {
    Q_ASSERT(bytesWritten >= 0);
    qint64 now = QDateTime::currentMSecsSinceEpoch();

    // Calculate speed every second
    if (now - m_lastSpeedUpdate < kSpeedUpdateIntervalMs) {
        return;
    }

    qint64 bytesDelta = bytesWritten - m_lastSpeedBytes;
    qint64 timeDelta = now - m_lastSpeedUpdate;

    if (timeDelta > 0) {
        double bytesPerMs = static_cast<double>(bytesDelta) / timeDelta;
        m_speedMBps = (bytesPerMs * sak::kMillisecondsPerSecondF) / sak::kBytesPerMBf;
    }

    m_lastSpeedUpdate = now;
    m_lastSpeedBytes = m_bytesWritten.load(std::memory_order_relaxed);
}

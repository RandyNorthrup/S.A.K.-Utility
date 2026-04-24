// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file flash_worker.cpp
/// @brief Implements the background worker thread for USB image flashing operations

#include "sak/flash_worker.h"

#include "sak/keep_awake.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QRandomGenerator>
#include <QtGlobal>

#include <windows.h>

#include <winioctl.h>

#pragma comment(lib, "setupapi.lib")

namespace {
constexpr qint64 kFlashBufferSize = 64LL * 1024 * 1024;   // 64 MB
constexpr qint64 kVerifySampleMax = 100LL * 1024 * 1024;  // 100 MB
constexpr qint64 kVerifyBlockSize = 1024LL * 1024;        // 1 MB
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
    , m_speedMBps(0.0)
    , m_bufferSize(kFlashBufferSize)
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

    // Lock and dismount volume (best-effort; non-critical if drive has no volumes)
    lockVolume();
    dismountVolume();

    // Write image
    if (!writeImage()) {
        sak::logError("Failed to write image");
        Q_EMIT error("Failed to write image");
        cleanupFlashResources();
        return std::unexpected(sak::error_code::operation_cancelled);
    }

    Q_EMIT writeCompleted(m_bytesWritten);

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
    sak::logInfo(QString("Flash completed in %1 seconds").arg(elapsed_ms / 1000.0).toStdString());

    return {};
}

void FlashWorker::cleanupFlashResources() {
    unlockVolume();
    closeDevice();
    m_imageSource->close();
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

    return true;
}

void FlashWorker::closeDevice() {
    if (m_deviceHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_deviceHandle);
        m_deviceHandle = INVALID_HANDLE_VALUE;
    }
}

bool FlashWorker::lockVolume() {
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(
            m_deviceHandle, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &bytesReturned, nullptr)) {
        sak::logWarning("Failed to lock volume (may not be mounted)");
        // Not a critical error - drive might not have volumes
    }
    return true;
}

bool FlashWorker::unlockVolume() {
    DWORD bytesReturned = 0;
    DeviceIoControl(
        m_deviceHandle, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &bytesReturned, nullptr);
    return true;
}

bool FlashWorker::dismountVolume() {
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(m_deviceHandle,
                         FSCTL_DISMOUNT_VOLUME,
                         nullptr,
                         0,
                         nullptr,
                         0,
                         &bytesReturned,
                         nullptr)) {
        sak::logWarning("Failed to dismount volume (may not be mounted)");
        // Not a critical error
    }
    return true;
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

bool FlashWorker::padBufferToSectorSize(QByteArray& buffer, qint64& bytesRead) {
    Q_ASSERT(!buffer.isEmpty());
    if (bytesRead % 512 == 0) {
        return true;
    }

    qint64 paddedSize = ((bytesRead / 512) + 1) * 512;

    // Validate padded size is reasonable
    if (paddedSize > buffer.capacity() * 2 || paddedSize < 0) {
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

    QByteArray buffer(m_bufferSize, 0);
    m_bytesWritten = 0;

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

        if (!padBufferToSectorSize(buffer, bytesRead)) {
            return false;
        }

        // Guard against qint64 -> DWORD truncation
        if (bytesRead > static_cast<qint64>(MAXDWORD)) {
            sak::logError("Write size exceeds DWORD range");
            return false;
        }

        DWORD bytesWrittenThisTime = 0;
        if (!WriteFile(m_deviceHandle,
                       buffer.data(),
                       static_cast<DWORD>(bytesRead),
                       &bytesWrittenThisTime,
                       nullptr)) {
            DWORD last_error = GetLastError();
            sak::logError(QString("WriteFile failed with error %1").arg(last_error).toStdString());
            return false;
        }

        m_bytesWritten += bytesWrittenThisTime;

        // Update progress
        updateProgress(m_bytesWritten);
        updateSpeed(m_bytesWritten);
    }

    // Flush buffers and check for failure to prevent silent data loss.
    if (!FlushFileBuffers(m_deviceHandle)) {
        DWORD flushError = GetLastError();
        sak::logError(
            QString("FlushFileBuffers failed with error %1").arg(flushError).toStdString());
        return false;
    }

    sak::logInfo(QString("Wrote %1 bytes").arg(m_bytesWritten).toStdString());
    return !stopRequested();
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

    // Calculate checksum of written data
    QString targetChecksum = calculateChecksum(m_deviceHandle, m_totalBytes);
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

    // Calculate speed
    qint64 elapsed_ms = timer.elapsed();
    if (elapsed_ms > 0) {
        result.verificationSpeed = (m_totalBytes / sak::kBytesPerMBf) / (elapsed_ms / 1000.0);
    }

    return result;
}

sak::ValidationResult FlashWorker::verifySample() {
    Q_ASSERT(m_imageSource);
    sak::logInfo("Starting sample verification");

    sak::ValidationResult result;
    result.sourceChecksum = m_sourceChecksum;

    // Sample size: 100MB or 10% of image, whichever is smaller
    qint64 sampleSize = qMin(kVerifySampleMax, m_totalBytes / 10);
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
    QByteArray targetBuffer(blockSize, 0);

    // Reopen source for reading
    m_imageSource->close();
    if (!m_imageSource->open()) {
        result.passed = false;
        result.errors.append("Failed to reopen image source for verification");
        return result;
    }

    result.passed = true;

    // Guard against images smaller than one block -- nothing to sample.
    const qint64 totalBlocks = m_totalBytes / blockSize;
    if (totalBlocks < 1) {
        sak::logWarning("Image too small for sample verification, skipping");
        return result;
    }

    int samplesVerified = verifySampleBlocks(
        result, {numSamples, blockSize, totalBlocks, sampleSize}, sourceBuffer, targetBuffer);

    // Calculate speed
    qint64 elapsed_ms = timer.elapsed();
    if (elapsed_ms > 0) {
        result.verificationSpeed = (sampleSize / sak::kBytesPerMBf) / (elapsed_ms / 1000.0);
    }

    sak::logInfo(QString("Sample verification complete - %1/%2 blocks verified, %3 mismatches")
                     .arg(samplesVerified)
                     .arg(numSamples)
                     .arg(result.corruptedBlocks)
                     .toStdString());

    return result;
}

int FlashWorker::verifySampleBlocks(sak::ValidationResult& result,
                                    const VerifyBlocksConfig& config,
                                    QByteArray& sourceBuffer,
                                    QByteArray& targetBuffer) {
    int samplesVerified = 0;

    for (int i = 0; i < config.num_samples && !stopRequested(); ++i) {
        qint64 blockIndex = QRandomGenerator::global()->bounded(config.total_blocks);
        qint64 offset_bytes = blockIndex * config.block_size;

        // Read from source
        if (!m_imageSource->seek(offset_bytes)) {
            result.errors.append(QString("Failed to seek source to offset %1").arg(offset_bytes));
            continue;
        }

        qint64 bytesRead = m_imageSource->read(sourceBuffer.data(), config.block_size);
        if (bytesRead != config.block_size) {
            if (bytesRead <= 0) {
                continue;
            }
            sourceBuffer.resize(bytesRead);
            targetBuffer.resize(bytesRead);
        }

        // Read from target device
        LARGE_INTEGER li;
        li.QuadPart = offset_bytes;
        if (!SetFilePointerEx(m_deviceHandle, li, nullptr, FILE_BEGIN)) {
            result.errors.append(QString("Failed to seek target to offset %1").arg(offset_bytes));
            continue;
        }

        DWORD bytesReadFromDevice = 0;
        if (!ReadFile(m_deviceHandle,
                      targetBuffer.data(),
                      static_cast<DWORD>(bytesRead),
                      &bytesReadFromDevice,
                      nullptr)) {
            result.errors.append(
                QString("Failed to read from device at offset %1").arg(offset_bytes));
            continue;
        }

        // Compare blocks
        if (memcmp(sourceBuffer.data(), targetBuffer.data(), bytesRead) != 0) {
            result.passed = false;
            result.mismatchOffset = offset_bytes;
            result.corruptedBlocks++;
            result.errors.append(QString("Data mismatch at offset %1").arg(offset_bytes));
            sak::logError(QString("Data mismatch at offset %1").arg(offset_bytes).toStdString());
        }

        samplesVerified++;
        updateVerificationProgress(samplesVerified * config.block_size, config.sample_size);
    }

    return samplesVerified;
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

    QCryptographicHash hash(QCryptographicHash::Sha512);

    const qint64 bufferSize = kFlashBufferSize;
    QByteArray buffer(bufferSize, 0);
    qint64 totalRead = 0;
    m_lastVerifyUpdate = 0;

    while (totalRead < size && !stopRequested()) {
        qint64 toRead = qMin(bufferSize, size - totalRead);

        DWORD bytesRead = 0;
        if (!ReadFile(handle, buffer.data(), static_cast<DWORD>(toRead), &bytesRead, nullptr)) {
            DWORD last_error = GetLastError();
            sak::logError(QString("ReadFile failed with error %1").arg(last_error).toStdString());
            return QString();
        }

        if (bytesRead == 0) {
            break;
        }

        hash.addData(buffer.data(), bytesRead);
        totalRead += bytesRead;

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
    if (now - m_lastVerifyUpdate < 100) {
        return;
    }

    m_lastVerifyUpdate = now;

    double percentage = 0.0;
    if (totalBytes > 0) {
        percentage = (static_cast<double>(bytesVerified) / totalBytes) * 100.0;
    }

    Q_EMIT verificationProgress(percentage, bytesVerified);
}

void FlashWorker::updateProgress(qint64 bytesWritten) {
    Q_ASSERT(bytesWritten >= 0);
    qint64 now = QDateTime::currentMSecsSinceEpoch();

    // Throttle updates to once per 100ms
    if (now - m_lastProgressUpdate < 100) {
        return;
    }

    m_lastProgressUpdate = now;

    double percentage = 0.0;
    if (m_totalBytes > 0) {
        percentage = (static_cast<double>(bytesWritten) / m_totalBytes) * 100.0;
    }

    Q_EMIT progressUpdated(percentage, bytesWritten);
}

void FlashWorker::updateSpeed(qint64 bytesWritten) {
    Q_ASSERT(bytesWritten >= 0);
    qint64 now = QDateTime::currentMSecsSinceEpoch();

    // Calculate speed every second
    if (now - m_lastSpeedUpdate < 1000) {
        return;
    }

    qint64 bytesDelta = bytesWritten - m_lastSpeedBytes;
    qint64 timeDelta = now - m_lastSpeedUpdate;

    if (timeDelta > 0) {
        double bytesPerMs = static_cast<double>(bytesDelta) / timeDelta;
        m_speedMBps = (bytesPerMs * 1000.0) / sak::kBytesPerMBf;
    }

    m_lastSpeedUpdate = now;
    m_lastSpeedBytes = m_bytesWritten;
}

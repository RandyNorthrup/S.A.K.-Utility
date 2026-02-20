// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/flash_worker.h"
#include "sak/logger.h"
#include <QObject>
#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QRandomGenerator>
#include <windows.h>
#include <winioctl.h>

#pragma comment(lib, "setupapi.lib")

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
    , m_bufferSize(64 * 1024 * 1024) // 64MB default
    , m_verificationEnabled(true)
    , m_validationMode(sak::ValidationMode::Full)
    , m_lastProgressUpdate(0)
    , m_lastSpeedUpdate(0)
    , m_lastSpeedBytes(0)
    , m_lastVerifyUpdate(0)
{
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
    m_bufferSize = sizeBytes;
}

auto FlashWorker::execute() -> std::expected<void, sak::error_code> {
    sak::log_info(QString("Starting flash to %1").arg(m_targetDevice).toStdString());
    
    QElapsedTimer timer;
    timer.start();
    
    // Open image source
    if (!m_imageSource->open()) {
        sak::log_error("Failed to open image source");
        Q_EMIT error("Failed to open image source");
        return std::unexpected(sak::error_code::file_not_found);
    }
    
    m_totalBytes = m_imageSource->size();
    
    // Open target device
    if (!openDevice()) {
        sak::log_error(QString("Failed to open device: %1").arg(m_targetDevice).toStdString());
        Q_EMIT error("Failed to open target device");
        m_imageSource->close();
        return std::unexpected(sak::error_code::file_not_found);
    }
    
    // Lock and dismount volume
    if (!lockVolume() || !dismountVolume()) {
        sak::log_error("Failed to prepare device for writing");
        Q_EMIT error("Failed to prepare device for writing");
        closeDevice();
        m_imageSource->close();
        return std::unexpected(sak::error_code::operation_cancelled);
    }
    
    // Write image
    if (!writeImage()) {
        sak::log_error("Failed to write image");
        Q_EMIT error("Failed to write image");
        unlockVolume();
        closeDevice();
        m_imageSource->close();
        return std::unexpected(sak::error_code::operation_cancelled);
    }
    
    Q_EMIT writeCompleted(m_bytesWritten);
    
    // Verify if enabled
    if (m_verificationEnabled && !stop_requested()) {
        sak::ValidationResult result = verifyImage();
        Q_EMIT verificationCompleted(result);
        
        if (!result.passed) {
            sak::log_error("Verification failed");
            Q_EMIT error(QString("Verification failed: %1").arg(
                result.errors.isEmpty() ? "Checksum mismatch" : result.errors.first()));
            unlockVolume();
            closeDevice();
            m_imageSource->close();
            return std::unexpected(sak::error_code::operation_cancelled);
        }
    }
    
    // Cleanup
    unlockVolume();
    closeDevice();
    m_imageSource->close();
    
    qint64 elapsed = timer.elapsed();
    sak::log_info(QString("Flash completed in %1 seconds").arg(elapsed / 1000.0).toStdString());
    
    return {};
}

bool FlashWorker::openDevice() {
    m_deviceHandle = CreateFileW(
        reinterpret_cast<LPCWSTR>(m_targetDevice.utf16()),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
        nullptr
    );
    
    if (m_deviceHandle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        sak::log_error(QString("CreateFile failed with error %1").arg(error).toStdString());
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
        m_deviceHandle,
        FSCTL_LOCK_VOLUME,
        nullptr, 0,
        nullptr, 0,
        &bytesReturned,
        nullptr))
    {
        sak::log_warning("Failed to lock volume (may not be mounted)");
        // Not a critical error - drive might not have volumes
    }
    return true;
}

bool FlashWorker::unlockVolume() {
    DWORD bytesReturned = 0;
    DeviceIoControl(
        m_deviceHandle,
        FSCTL_UNLOCK_VOLUME,
        nullptr, 0,
        nullptr, 0,
        &bytesReturned,
        nullptr
    );
    return true;
}

bool FlashWorker::dismountVolume() {
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(
        m_deviceHandle,
        FSCTL_DISMOUNT_VOLUME,
        nullptr, 0,
        nullptr, 0,
        &bytesReturned,
        nullptr))
    {
        sak::log_warning("Failed to dismount volume (may not be mounted)");
        // Not a critical error
    }
    return true;
}

bool FlashWorker::writeImage() {
    sak::log_info("Writing image");
    
    // Calculate source checksum if verification enabled
    if (m_verificationEnabled && m_sourceChecksum.isEmpty()) {
        sak::log_info("Calculating source checksum");
        m_sourceChecksum = m_imageSource->calculateChecksum();
        if (m_sourceChecksum.isEmpty()) {
            sak::log_error("Failed to calculate source checksum");
            return false;
        }
        sak::log_info(QString("Source checksum: %1").arg(m_sourceChecksum).toStdString());
        
        // Reopen source after checksum calculation
        m_imageSource->close();
        if (!m_imageSource->open()) {
            sak::log_error("Failed to reopen image source");
            return false;
        }
    }
    
    QByteArray buffer(m_bufferSize, 0);
    m_bytesWritten = 0;
    
    QElapsedTimer speedTimer;
    speedTimer.start();
    m_lastSpeedUpdate = 0;
    m_lastSpeedBytes = 0;
    
    while (!m_imageSource->atEnd() && !stop_requested()) {
        qint64 bytesRead = m_imageSource->read(buffer.data(), m_bufferSize);
        if (bytesRead < 0) {
            sak::log_error("Failed to read from image source");
            return false;
        }
        
        if (bytesRead == 0) {
            break;
        }
        
    // Pad to sector size if needed
    if (bytesRead % 512 != 0) {
        qint64 paddedSize = ((bytesRead / 512) + 1) * 512;
        
        // Validate padded size is reasonable
        if (paddedSize > buffer.capacity() * 2 || paddedSize < 0) {
            sak::log_error(QString("Invalid padded size calculated: %1").arg(paddedSize).toStdString());
            return false;
        }
        
        try {
            buffer.resize(paddedSize);
        } catch (const std::bad_alloc&) {
            sak::log_error("Failed to allocate padding buffer - out of memory");
            return false;
        }
        
        // Verify resize succeeded
        if (buffer.size() != paddedSize) {
            sak::log_error(QString("Buffer resize failed: expected %1, got %2")
                .arg(paddedSize).arg(buffer.size()).toStdString());
            return false;
        }
        
        // Zero out padding
        for (qint64 i = bytesRead; i < paddedSize; ++i) {
            buffer[i] = 0;
        }
        bytesRead = paddedSize;
    }
    
        // Guard against qint64 → DWORD truncation
        if (bytesRead > static_cast<qint64>(MAXDWORD)) {
            sak::log_error("Write size exceeds DWORD range");
            return false;
        }
        
        DWORD bytesWrittenThisTime = 0;
        if (!WriteFile(
            m_deviceHandle,
            buffer.data(),
            static_cast<DWORD>(bytesRead),
            &bytesWrittenThisTime,
            nullptr))
        {
            DWORD error = GetLastError();
            sak::log_error(QString("WriteFile failed with error %1").arg(error).toStdString());
            return false;
        }
        
        m_bytesWritten += bytesWrittenThisTime;
        
        // Update progress
        updateProgress(m_bytesWritten);
        updateSpeed(m_bytesWritten);
    }
    
    // Flush buffers
    FlushFileBuffers(m_deviceHandle);
    
    sak::log_info(QString("Wrote %1 bytes").arg(m_bytesWritten).toStdString());
    return !stop_requested();
}

sak::ValidationResult FlashWorker::verifyImage() {
    sak::ValidationResult result;
    
    // Skip validation mode
    if (m_validationMode == sak::ValidationMode::Skip) {
        sak::log_info("Verification skipped (skip mode)");
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
    sak::log_info("Starting full verification");
    
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
            .arg(result.sourceChecksum).arg(result.targetChecksum));
        sak::log_error(QString("Checksum mismatch - Source: %1, Target: %2")
            .arg(result.sourceChecksum).arg(result.targetChecksum).toStdString());
    } else {
        result.passed = true;
        sak::log_info("Verification passed - checksums match");
    }
    
    // Calculate speed
    qint64 elapsed = timer.elapsed();
    if (elapsed > 0) {
        result.verificationSpeed = (m_totalBytes / (1024.0 * 1024.0)) / (elapsed / 1000.0);
    }
    
    return result;
}

sak::ValidationResult FlashWorker::verifySample() {
    sak::log_info("Starting sample verification");
    
    sak::ValidationResult result;
    result.sourceChecksum = m_sourceChecksum;
    
    // Sample size: 100MB or 10% of image, whichever is smaller
    qint64 sampleSize = qMin(100LL * 1024 * 1024, m_totalBytes / 10);
    qint64 blockSize = 1024 * 1024; // 1MB blocks
    int numSamples = static_cast<int>(sampleSize / blockSize);
    
    if (numSamples < 1) {
        numSamples = 1;
    }
    
    sak::log_info(QString("Verifying %1 sample blocks (%2 MB)")
        .arg(numSamples).arg(sampleSize / (1024 * 1024)).toStdString());
    
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
    
    int samplesVerified = 0;
    result.passed = true;
    
    for (int i = 0; i < numSamples && !stop_requested(); ++i) {
        // Calculate random offset aligned to block boundary
        qint64 maxOffset = (m_totalBytes / blockSize) - 1;
        qint64 blockIndex = QRandomGenerator::global()->bounded(maxOffset);
        qint64 offset = blockIndex * blockSize;
        
        // Read from source
        if (!m_imageSource->seek(offset)) {
            result.errors.append(QString("Failed to seek source to offset %1").arg(offset));
            continue;
        }
        
        qint64 bytesRead = m_imageSource->read(sourceBuffer.data(), blockSize);
        if (bytesRead != blockSize) {
            // Might be at end of file
            if (bytesRead <= 0) {
                continue;
            }
            sourceBuffer.resize(bytesRead);
            targetBuffer.resize(bytesRead);
        }
        
        // Read from target device
        LARGE_INTEGER li;
        li.QuadPart = offset;
        if (!SetFilePointerEx(m_deviceHandle, li, nullptr, FILE_BEGIN)) {
            result.errors.append(QString("Failed to seek target to offset %1").arg(offset));
            continue;
        }
        
        DWORD bytesReadFromDevice = 0;
        if (!ReadFile(m_deviceHandle, targetBuffer.data(), 
                     static_cast<DWORD>(bytesRead), &bytesReadFromDevice, nullptr)) {
            result.errors.append(QString("Failed to read from device at offset %1").arg(offset));
            continue;
        }
        
        // Compare blocks
        if (memcmp(sourceBuffer.data(), targetBuffer.data(), bytesRead) != 0) {
            result.passed = false;
            result.mismatchOffset = offset;
            result.corruptedBlocks++;
            result.errors.append(QString("Data mismatch at offset %1").arg(offset));
            sak::log_error(QString("Data mismatch at offset %1").arg(offset).toStdString());
        }
        
        samplesVerified++;
        updateVerificationProgress(samplesVerified * blockSize, sampleSize);
    }
    
    // Calculate speed
    qint64 elapsed = timer.elapsed();
    if (elapsed > 0) {
        result.verificationSpeed = (sampleSize / (1024.0 * 1024.0)) / (elapsed / 1000.0);
    }
    
    sak::log_info(QString("Sample verification complete - %1/%2 blocks verified, %3 mismatches")
        .arg(samplesVerified).arg(numSamples).arg(result.corruptedBlocks).toStdString());
    
    return result;
}

QString FlashWorker::calculateChecksum(HANDLE handle, qint64 size) {
    sak::log_info("Calculating device checksum");
    
    // Seek to beginning
    LARGE_INTEGER li;
    li.QuadPart = 0;
    if (!SetFilePointerEx(handle, li, nullptr, FILE_BEGIN)) {
        sak::log_error("Failed to seek to beginning for checksum");
        return QString();
    }
    
    QCryptographicHash hash(QCryptographicHash::Sha512);
    
    const qint64 bufferSize = 64 * 1024 * 1024; // 64MB
    QByteArray buffer(bufferSize, 0);
    qint64 totalRead = 0;
    m_lastVerifyUpdate = 0;
    
    while (totalRead < size && !stop_requested()) {
        qint64 toRead = qMin(bufferSize, size - totalRead);
        
        DWORD bytesRead = 0;
        if (!ReadFile(handle, buffer.data(), static_cast<DWORD>(toRead), &bytesRead, nullptr)) {
            DWORD error = GetLastError();
            sak::log_error(QString("ReadFile failed with error %1").arg(error).toStdString());
            return QString();
        }
        
        if (bytesRead == 0) {
            break;
        }
        
        hash.addData(buffer.data(), bytesRead);
        totalRead += bytesRead;
        
        updateVerificationProgress(totalRead, size);
    }
    
    if (stop_requested()) {
        sak::log_warning("Checksum calculation cancelled");
        return QString();
    }
    
    QString result = QString(hash.result().toHex());
    sak::log_info(QString("Device checksum: %1").arg(result).toStdString());
    return result;
}

void FlashWorker::updateVerificationProgress(qint64 bytesVerified, qint64 totalBytes) {
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
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    
    // Calculate speed every second
    if (now - m_lastSpeedUpdate < 1000) {
        return;
    }
    
    qint64 bytesDelta = bytesWritten - m_lastSpeedBytes;
    qint64 timeDelta = now - m_lastSpeedUpdate;
    
    if (timeDelta > 0) {
        double bytesPerMs = static_cast<double>(bytesDelta) / timeDelta;
        m_speedMBps = (bytesPerMs * 1000.0) / (1024.0 * 1024.0);
    }
    
    m_lastSpeedUpdate = now;
    m_lastSpeedBytes = m_bytesWritten;
}











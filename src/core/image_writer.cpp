// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/image_writer.h"
#include "sak/logger.h"
#include <windows.h>
#include <winioctl.h>
#include <algorithm>

ImageWriter::ImageWriter(std::unique_ptr<ImageSource> source, int driveNumber, QObject* parent)
    : QObject(parent)
    , m_source(std::move(source))
    , m_driveNumber(driveNumber)
{
    m_targetPath = QString("\\\\.\\PhysicalDrive%1").arg(driveNumber);
    init();
}

ImageWriter::ImageWriter(std::unique_ptr<ImageSource> source, const QString& volumePath, QObject* parent)
    : QObject(parent)
    , m_source(std::move(source))
    , m_targetPath(volumePath)
    , m_driveNumber(-1)
{
    init();
}

ImageWriter::~ImageWriter() {
    if (m_isWriting) {
        cancel();
    }
}

void ImageWriter::init() {
    m_bufferSize = 64 * 1024 * 1024; // 64MB default
    m_sectorSize = 512;               // Will be updated when drive is opened
    m_progressInterval = 500;         // 500ms default
    m_isWriting = false;
    m_cancelled = false;
    m_lastProgressBytes = 0;

    m_progress.bytesWritten = 0;
    m_progress.totalBytes = 0;
    m_progress.percentage = 0.0;
    m_progress.speedMBps = 0.0;
    m_progress.etaSeconds = 0;
}

bool ImageWriter::write() {
    if (m_isWriting) {
        m_lastError = "Write already in progress";
        return false;
    }

    if (!m_source) {
        m_lastError = "No image source provided";
        Q_EMIT writeError(m_lastError);
        return false;
    }

    m_isWriting = true;
    m_cancelled = false;
    m_writeTimer.start();
    m_progressTimer.start();

    sak::log_info(QString("Starting write to %1").arg(m_targetPath).toStdString());

    // Open the image source
    if (!m_source->open()) {
        m_lastError = "Failed to open image source";
        sak::log_error(m_lastError.toStdString());
        Q_EMIT writeError(m_lastError);
        m_isWriting = false;
        return false;
    }

    // Get image metadata
    const sak::ImageMetadata& metadata = m_source->metadata();
    m_progress.totalBytes = metadata.size;

    sak::log_info(QString("Image size: %1 bytes").arg(m_progress.totalBytes).toStdString());

    // Acquire exclusive lock on drive
    DriveLock lock{m_targetPath, false};
    if (!lock.isLocked()) {
        m_lastError = QString("Failed to lock drive: %1").arg(lock.lastError());
        sak::log_error(m_lastError.toStdString());
        Q_EMIT writeError(m_lastError);
        m_source->close();
        m_isWriting = false;
        return false;
    }

    HANDLE driveHandle = lock.handle();

    // Get sector size
    m_sectorSize = getSectorSize(driveHandle);
    sak::log_info(QString("Sector size: %1 bytes").arg(m_sectorSize).toStdString());

    // Allocate aligned buffer
    qint64 alignedBufferSize = alignToSector(m_bufferSize, m_sectorSize);
    std::vector<char> buffer(alignedBufferSize);

    sak::log_info(QString("Using %1 MB buffer").arg(alignedBufferSize / (1024.0 * 1024.0)).toStdString());

    // Write loop
    qint64 totalWritten = 0;
    bool success = true;

    while (totalWritten < m_progress.totalBytes && !m_cancelled) {
        // Read from source
        qint64 bytesToRead = (std::min)(alignedBufferSize, m_progress.totalBytes - totalWritten);
        qint64 bytesRead = m_source->read(buffer.data(), bytesToRead);

        if (bytesRead < 0) {
            m_lastError = "Failed to read from image";
            sak::log_error(m_lastError.toStdString());
            Q_EMIT writeError(m_lastError);
            success = false;
            break;
        }

        if (bytesRead == 0) {
            // End of file
            break;
        }

        // Pad to sector alignment if needed
        qint64 alignedBytes = alignToSector(bytesRead, m_sectorSize);
        if (alignedBytes > bytesRead) {
            memset(buffer.data() + bytesRead, 0, alignedBytes - bytesRead);
        }

        // Write to drive
        if (!writeBuffer(driveHandle, buffer.data(), alignedBytes, totalWritten)) {
            success = false;
            break;
        }

        totalWritten += bytesRead;
        m_progress.bytesWritten = totalWritten;

        // Update progress
        updateProgress(false);
    }

    // Final progress update
    if (success && !m_cancelled) {
        m_progress.bytesWritten = totalWritten;
        updateProgress(true);

        // Flush all buffers
        sak::log_info(QString("Flushing buffers...").toStdString());
        if (!FlushFileBuffers(driveHandle)) {
            m_lastError = QString("Failed to flush buffers: error %1").arg(GetLastError());
            sak::log_error(m_lastError.toStdString());
            success = false;
        }
    }

    // Cleanup
    m_source->close();
    m_isWriting = false;

    // Emit completion signals
    if (m_cancelled) {
        sak::log_info(QString("Write cancelled").toStdString());
        Q_EMIT writeCancelled();
    } else if (success) {
        qint64 elapsed = m_writeTimer.elapsed();
        double avgSpeed = (totalWritten / (1024.0 * 1024.0)) / (elapsed / 1000.0);
        sak::log_info(QString("Write completed: %1 bytes in %2ms (avg %3 MB/s)")
            .arg(totalWritten).arg(elapsed).arg(avgSpeed, 0, 'f', 2).toStdString());
        Q_EMIT writeCompleted(totalWritten);
    } else {
        Q_EMIT writeError(m_lastError);
    }

    return success && !m_cancelled;
}

void ImageWriter::cancel() {
    if (m_isWriting) {
        sak::log_info(QString("Cancelling write operation").toStdString());
        m_cancelled = true;
    }
}

void ImageWriter::setBufferSize(qint64 sizeBytes) {
    if (!m_isWriting) {
        m_bufferSize = sizeBytes;
    }
}

void ImageWriter::setProgressInterval(int milliseconds) {
    m_progressInterval = milliseconds;
}

DWORD ImageWriter::getSectorSize(HANDLE driveHandle) {
    DISK_GEOMETRY_EX geometry = {};
    DWORD bytesReturned = 0;

    if (DeviceIoControl(
        driveHandle,
        IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
        nullptr,
        0,
        &geometry,
        sizeof(geometry),
        &bytesReturned,
        nullptr
    )) {
        return geometry.Geometry.BytesPerSector;
    }

    // Fallback: Try basic geometry
    DISK_GEOMETRY basicGeometry = {};
    if (DeviceIoControl(
        driveHandle,
        IOCTL_DISK_GET_DRIVE_GEOMETRY,
        nullptr,
        0,
        &basicGeometry,
        sizeof(basicGeometry),
        &bytesReturned,
        nullptr
    )) {
        return basicGeometry.BytesPerSector;
    }

    // Default to 512 bytes
    sak::log_warning(QString("Failed to get sector size, defaulting to 512 bytes").toStdString());
    return 512;
}

qint64 ImageWriter::alignToSector(qint64 value, DWORD alignment) {
    return ((value + alignment - 1) / alignment) * alignment;
}

void ImageWriter::updateProgress(bool force) {
    qint64 elapsed = m_progressTimer.elapsed();

    if (!force && elapsed < m_progressInterval) {
        return;
    }

    // Calculate speed
    qint64 bytesSinceLastUpdate = m_progress.bytesWritten - m_lastProgressBytes;
    if (elapsed > 0) {
        m_progress.speedMBps = (bytesSinceLastUpdate / (1024.0 * 1024.0)) / (elapsed / 1000.0);
    }

    // Calculate percentage
    if (m_progress.totalBytes > 0) {
        m_progress.percentage = (m_progress.bytesWritten * 100.0) / m_progress.totalBytes;
    }

    // Calculate ETA
    if (m_progress.speedMBps > 0) {
        qint64 remainingBytes = m_progress.totalBytes - m_progress.bytesWritten;
        double remainingMB = remainingBytes / (1024.0 * 1024.0);
        m_progress.etaSeconds = static_cast<int>(remainingMB / m_progress.speedMBps);
    }

    Q_EMIT progressUpdated(m_progress);

    m_lastProgressBytes = m_progress.bytesWritten;
    m_progressTimer.restart();
}

bool ImageWriter::writeBuffer(HANDLE driveHandle, const char* buffer, qint64 size, qint64 offset) {
    // Set file pointer
    LARGE_INTEGER liOffset;
    liOffset.QuadPart = offset;

    if (!SetFilePointerEx(driveHandle, liOffset, nullptr, FILE_BEGIN)) {
        m_lastError = QString("SetFilePointerEx failed: error %1").arg(GetLastError());
        sak::log_error(m_lastError.toStdString());
        return false;
    }

    // Write data
    DWORD bytesWritten = 0;
    if (!WriteFile(driveHandle, buffer, static_cast<DWORD>(size), &bytesWritten, nullptr)) {
        m_lastError = QString("WriteFile failed: error %1").arg(GetLastError());
        sak::log_error(m_lastError.toStdString());
        return false;
    }

    if (bytesWritten != static_cast<DWORD>(size)) {
        m_lastError = QString("Incomplete write: wrote %1 of %2 bytes")
            .arg(bytesWritten).arg(size);
        sak::log_error(m_lastError.toStdString());
        return false;
    }

    return true;
}

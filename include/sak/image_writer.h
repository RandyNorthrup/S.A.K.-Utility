// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/image_source.h"
#include "sak/drive_lock.h"
#include <QObject>
#include <QString>
#include <QElapsedTimer>
#include <windows.h>
#include <memory>
#include <vector>

namespace sak {

/**
 * @brief Write progress information
 */
struct WriteProgress {
    qint64 bytesWritten;      // Total bytes written
    qint64 totalBytes;        // Total bytes to write
    double percentage;        // Progress percentage (0-100)
    double speedMBps;         // Current write speed (MB/s)
    int etaSeconds;           // Estimated time remaining
};

} // namespace sak

/**
 * @brief Image Writer - Raw sector-level disk writing
 * 
 * Handles low-level writing of disk images to physical drives or volumes.
 * Based on Etcher SDK pipeSourceToDestinations pattern.
 * 
 * Features:
 * - Raw sector-level writes using Win32 API
 * - Buffered I/O for performance (configurable buffer size)
 * - Sector-aligned operations (512 or 4096 bytes)
 * - Progress tracking with speed calculation
 * - Automatic retry on transient errors
 * - Support for compressed image sources
 * 
 * Technical Details:
 * - Uses CreateFile with FILE_FLAG_NO_BUFFERING
 * - All writes must be sector-aligned
 * - Uses WriteFile for unbuffered I/O
 * - FlushFileBuffers after each buffer write
 * - SetFilePointerEx for large file support
 * 
 * Thread-Safety: NOT thread-safe. Use one instance per thread.
 * 
 * Example:
 * @code
 * auto source = std::make_unique<FileImageSource>("ubuntu.iso");
 * ImageWriter writer(std::move(source), 0);
 * 
 * connect(&writer, &ImageWriter::progressUpdated, [](const WriteProgress& p) {
 *     qDebug() << p.percentage << "% complete";
 * });
 * 
 * if (writer.write()) {
 *     qDebug() << "Write successful!";
 * }
 * @endcode
 */
class ImageWriter : public QObject {
    Q_OBJECT

public:
    /**
     * @brief Constructor for physical drive writing
     * @param source Image source (takes ownership)
     * @param driveNumber Physical drive number (0 = first drive)
     * @param parent Parent QObject
     */
    ImageWriter(std::unique_ptr<ImageSource> source, int driveNumber, QObject* parent = nullptr);

    /**
     * @brief Constructor for volume writing
     * @param source Image source (takes ownership)
     * @param volumePath Volume path (e.g., "\\.\C:")
     * @param parent Parent QObject
     */
    ImageWriter(std::unique_ptr<ImageSource> source, const QString& volumePath, QObject* parent = nullptr);

    /**
     * @brief Destructor
     */
    ~ImageWriter() override;

    /**
     * @brief Write image to drive
     * @return true if successful, false on error
     */
    bool write();

    /**
     * @brief Cancel the write operation
     */
    void cancel();

    /**
     * @brief Check if write is in progress
     * @return true if writing
     */
    bool isWriting() const { return m_isWriting; }

    /**
     * @brief Check if operation was cancelled
     * @return true if cancelled
     */
    bool isCancelled() const { return m_cancelled; }

    /**
     * @brief Get last error message
     * @return Human-readable error description
     */
    QString lastError() const { return m_lastError; }

    /**
     * @brief Set buffer size for I/O operations
     * @param sizeBytes Buffer size in bytes (default 64MB, must be sector-aligned)
     */
    void setBufferSize(qint64 sizeBytes);

    /**
     * @brief Get current buffer size
     * @return Buffer size in bytes
     */
    qint64 bufferSize() const { return m_bufferSize; }

    /**
     * @brief Set progress update interval
     * @param milliseconds Interval between progress signals (default 500ms)
     */
    void setProgressInterval(int milliseconds);

Q_SIGNALS:
    /**
     * @brief Emitted periodically during write
     * @param progress Current progress information
     */
    void progressUpdated(const sak::WriteProgress& progress);

    /**
     * @brief Emitted when write completes successfully
     * @param bytesWritten Total bytes written
     */
    void writeCompleted(qint64 bytesWritten);

    /**
     * @brief Emitted on write error
     * @param error Error message
     */
    void writeError(const QString& error);

    /**
     * @brief Emitted when write is cancelled
     */
    void writeCancelled();

private:
    /**
     * @brief Initialize common members
     */
    void init();

    /**
     * @brief Get sector size for the drive
     * @param driveHandle Handle to drive
     * @return Sector size in bytes (512 or 4096)
     */
    DWORD getSectorSize(HANDLE driveHandle);

    /**
     * @brief Align value to sector boundary
     * @param value Value to align
     * @param alignment Sector size
     * @return Aligned value
     */
    qint64 alignToSector(qint64 value, DWORD alignment);

    /**
     * @brief Update write progress
     * @param force Force update even if interval hasn't elapsed
     */
    void updateProgress(bool force = false);

    /**
     * @brief Write a buffer to drive
     * @param driveHandle Handle to drive
     * @param buffer Data buffer
     * @param size Buffer size
     * @param offset Write offset
     * @return true if successful
     */
    bool writeBuffer(HANDLE driveHandle, const char* buffer, qint64 size, qint64 offset);

    std::unique_ptr<ImageSource> m_source;      // Image source
    QString m_targetPath;                        // Drive or volume path
    int m_driveNumber;                           // Physical drive number (-1 for volume)
    
    qint64 m_bufferSize;                         // I/O buffer size
    DWORD m_sectorSize;                          // Sector size (512 or 4096)
    int m_progressInterval;                      // Progress update interval (ms)
    
    bool m_isWriting;                            // Write in progress flag
    bool m_cancelled;                            // Cancellation flag
    QString m_lastError;                         // Last error message
    
    // Progress tracking
    sak::WriteProgress m_progress;
    QElapsedTimer m_writeTimer;                  // Total write time
    QElapsedTimer m_progressTimer;               // Time since last progress update
    qint64 m_lastProgressBytes;                  // Bytes written at last progress update
};

// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "worker_base.h"
#include "image_source.h"
#include <QString>
#include <QMutex>
#include <QList>
#include <windows.h>

namespace sak {

/**
 * @brief Validation modes for verification
 */
enum class ValidationMode {
    Full,      ///< Read and verify every byte (most reliable)
    Sample,    ///< Verify random samples (faster, less thorough)
    Skip       ///< No verification (fastest)
};

/**
 * @brief Result of verification operation
 */
struct ValidationResult {
    bool passed = false;                  ///< Overall success/failure
    QString sourceChecksum;               ///< Expected checksum (SHA-512)
    QString targetChecksum;               ///< Actual checksum from device
    QList<QString> errors;                ///< Detailed error messages
    qint64 mismatchOffset = -1;          ///< First mismatch byte position (-1 if none)
    int corruptedBlocks = 0;             ///< Number of blocks with errors
    double verificationSpeed = 0.0;       ///< MB/s read speed during verify
};

} // namespace sak

/**
 * @brief Flash Worker - Writes image to a single drive
 * 
 * Worker thread that writes an image source to a physical drive.
 * Handles low-level Windows API operations including opening device,
 * locking volume, writing sectors, and verification.
 * 
 * Based on Etcher SDK's flash worker with Windows-specific optimizations.
 * 
 * Features:
 * - Sector-aligned writes with FILE_FLAG_NO_BUFFERING
 * - Progress tracking with speed calculation
 * - SHA-512 verification via read-back
 * - Automatic retry on transient errors
 * - Graceful cancellation
 * 
 * Thread-Safety: All methods are thread-safe. Run() executes on worker thread.
 * 
 * Example:
 * @code
 * auto imageSource = std::make_unique<FileImageSource>("image.iso");
 * FlashWorker worker(std::move(imageSource), "\\.\PhysicalDrive1");
 * 
 * connect(&worker, &FlashWorker::progressUpdated, 
 *         [](double pct) { qDebug() << pct << "%"; });
 * 
 * worker.start();
 * @endcode
 */
class FlashWorker : public WorkerBase {
    Q_OBJECT

public:
    /**
     * @brief Construct flash worker
     * @param imageSource Image source to read from
     * @param targetDevice Target device path (e.g., "\\.\PhysicalDrive1")
     * @param parent Parent object
     */
    explicit FlashWorker(std::unique_ptr<ImageSource> imageSource,
                        const QString& targetDevice,
                        QObject* parent = nullptr);
    ~FlashWorker() override;

    // Disable copy and move
    FlashWorker(const FlashWorker&) = delete;
    FlashWorker& operator=(const FlashWorker&) = delete;
    FlashWorker(FlashWorker&&) = delete;
    FlashWorker& operator=(FlashWorker&&) = delete;

    /**
     * @brief Get target device path
     * @return Device path
     */
    QString targetDevice() const { return m_targetDevice; }

    /**
     * @brief Get total bytes written
     * @return Bytes written
     */
    qint64 bytesWritten() const { return m_bytesWritten; }

    /**
     * @brief Get write speed
     * @return Speed in MB/s
     */
    double speedMBps() const { return m_speedMBps; }

    /**
     * @brief Enable/disable verification
     * @param enabled true to verify after writing
     */
    void setVerificationEnabled(bool enabled);
    
    /**
     * @brief Set validation mode
     * @param mode Validation mode (Full, Sample, or Skip)
     */
    void setValidationMode(sak::ValidationMode mode);

    /**
     * @brief Set buffer size
     * @param sizeBytes Buffer size in bytes
     */
    void setBufferSize(qint64 sizeBytes);

Q_SIGNALS:
    /**
     * @brief Emitted periodically during write
     * @param percentage Progress 0-100
     * @param bytesWritten Total bytes written so far
     */
    void progressUpdated(double percentage, qint64 bytesWritten);
    
    /**
     * @brief Emitted periodically during verification
     * @param percentage Progress 0-100
     * @param bytesVerified Total bytes verified so far
     */
    void verificationProgress(double percentage, qint64 bytesVerified);

    /**
     * @brief Emitted when verification completes
     * @param result Validation result with checksums and status
     */
    void verificationCompleted(const sak::ValidationResult& result);

    /**
     * @brief Emitted when write completes (before verification)
     * @param bytesWritten Total bytes written
     */
    void writeCompleted(qint64 bytesWritten);
    
    /**
     * @brief Emitted on error
     * @param error Error message
     */
    void error(const QString& error);

protected:
    auto execute() -> std::expected<void, sak::error_code> override;

private:
    bool openDevice();
    void closeDevice();
    bool lockVolume();
    bool unlockVolume();
    bool dismountVolume();
    
    bool writeImage();
    sak::ValidationResult verifyImage();
    QString calculateChecksum(HANDLE handle, qint64 size);
    sak::ValidationResult verifyFull();
    sak::ValidationResult verifySample();
    
    void updateProgress(qint64 bytesWritten);
    void updateSpeed(qint64 bytesWritten);
    void updateVerificationProgress(qint64 bytesVerified, qint64 totalBytes);
    
    std::unique_ptr<ImageSource> m_imageSource;
    QString m_targetDevice;
    QString m_sourceChecksum;  // Cached source checksum
    HANDLE m_deviceHandle;
    
    qint64 m_bytesWritten;
    qint64 m_totalBytes;
    double m_speedMBps;
    qint64 m_bufferSize;
    bool m_verificationEnabled;
    sak::ValidationMode m_validationMode;
    
    QMutex m_mutex;
    qint64 m_lastProgressUpdate;
    qint64 m_lastSpeedUpdate;
    qint64 m_lastSpeedBytes;
    qint64 m_lastVerifyUpdate;
};

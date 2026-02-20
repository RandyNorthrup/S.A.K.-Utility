// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "image_source.h"
#include "drive_scanner.h"
#include <QObject>
#include <QString>
#include <QStringList>
#include <memory>
#include <vector>
#include <atomic>

class FlashWorker;

namespace sak {

// Forward declarations
struct ValidationResult;

/**
 * @brief Flash operation state
 */
enum class FlashState {
    Idle,
    Validating,      // Validating image and drives
    Unmounting,      // Unmounting volumes
    Decompressing,   // Decompressing image (if needed)
    Flashing,        // Writing to drives
    Verifying,       // Verifying writes
    Completed,       // Successfully completed
    Failed,          // Failed with error
    Cancelled        // User cancelled
};

/**
 * @brief Progress information for flash operation
 */
struct FlashProgress {
    FlashState state;
    double percentage;         // Overall progress 0-100
    qint64 bytesWritten;      // Total bytes written across all drives
    qint64 totalBytes;        // Total bytes to write
    double speedMBps;         // Current write speed in MB/s
    int activeDrives;         // Number of drives currently being written
    int failedDrives;         // Number of drives that failed
    int completedDrives;      // Number of drives completed
    QString currentOperation; // Human-readable description
    
    double getOverallProgress() const {
        if (totalBytes == 0) return 0.0;
        return (static_cast<double>(bytesWritten) / totalBytes) * 100.0;
    }
};

/**
 * @brief Result of flash operation
 */
struct FlashResult {
    bool success;
    qint64 bytesWritten;
    double elapsedSeconds;
    QStringList successfulDrives;
    QStringList failedDrives;
    QStringList errorMessages;
    QString sourceChecksum;
    
    bool hasErrors() const { return !failedDrives.isEmpty(); }
    int totalDrives() const { return successfulDrives.size() + failedDrives.size(); }
};

} // namespace sak

/**
 * @brief Flash Coordinator - Orchestrates multi-drive flash operations
 * 
 * Manages the complete flash workflow including validation, unmounting,
 * decompression (if needed), writing, and verification. Supports writing
 * to multiple drives in parallel.
 * 
 * Based on Etcher SDK's multiWrite pattern with Windows-specific optimizations.
 * 
 * Features:
 * - Parallel writes to multiple drives
 * - Automatic decompression
 * - SHA-512 verification
 * - Progress tracking per drive and overall
 * - Automatic unmounting and remounting
 * - Error recovery and retry logic
 * - Memory-efficient buffering
 * 
 * Workflow:
 * 1. Validate image and target drives
 * 2. Unmount all target volumes
 * 3. Open image source (decompress if needed)
 * 4. Create flash workers for each drive
 * 5. Write image to all drives in parallel
 * 6. Verify each drive
 * 7. Report results
 * 
 * Thread-Safety: Methods can be called from any thread.
 * Signals are emitted on the calling thread.
 * 
 * Example:
 * @code
 * FlashCoordinator coordinator;
 * connect(&coordinator, &FlashCoordinator::progressUpdated, 
 *         [](const FlashProgress& progress) {
 *     qDebug() << "Progress:" << progress.percentage << "%";
 * });
 * 
 * coordinator.startFlash("C:/image.iso", {"\\.\PhysicalDrive1", "\\.\PhysicalDrive2"});
 * @endcode
 */
class FlashCoordinator : public QObject {
    Q_OBJECT

public:
    explicit FlashCoordinator(QObject* parent = nullptr);
    ~FlashCoordinator() override;

    // Disable copy and move
    FlashCoordinator(const FlashCoordinator&) = delete;
    FlashCoordinator& operator=(const FlashCoordinator&) = delete;
    FlashCoordinator(FlashCoordinator&&) = delete;
    FlashCoordinator& operator=(FlashCoordinator&&) = delete;

    /**
     * @brief Start flash operation
     * @param imagePath Path to image file
     * @param targetDrives List of device paths to write to
     * @return true if started successfully
     */
    bool startFlash(const QString& imagePath, const QStringList& targetDrives);

    /**
     * @brief Cancel ongoing flash operation
     */
    void cancel();

    /**
     * @brief Check if flash is in progress
     * @return true if currently flashing
     */
    bool isFlashing() const;

    /**
     * @brief Get current state
     * @return Current FlashState
     */
    sak::FlashState state() const;

    /**
     * @brief Get current progress
     * @return FlashProgress structure
     */
    sak::FlashProgress progress() const;

    /**
     * @brief Enable/disable verification
     * @param enabled true to verify after writing
     */
    void setVerificationEnabled(bool enabled);

    /**
     * @brief Get verification setting
     * @return true if verification is enabled
     */
    bool isVerificationEnabled() const;

    /**
     * @brief Set buffer size for reading/writing
     * @param sizeBytes Buffer size in bytes (default 64MB)
     */
    void setBufferSize(qint64 sizeBytes);

    /**
     * @brief Set number of buffers for pipeline
     * @param count Number of buffers (default 16)
     */
    void setBufferCount(int count);

Q_SIGNALS:
    /**
     * @brief Emitted when state changes
     * @param newState New state
     * @param message Human-readable message
     */
    void stateChanged(sak::FlashState newState, const QString& message);

    /**
     * @brief Emitted periodically during operation
     * @param progress Current progress information
     */
    void progressUpdated(const sak::FlashProgress& progress);

    /**
     * @brief Emitted when a drive completes successfully
     * @param devicePath Device path of completed drive
     * @param checksum Verification checksum (if verification enabled)
     */
    void driveCompleted(const QString& devicePath, const QString& checksum);

    /**
     * @brief Emitted when a drive fails
     * @param devicePath Device path of failed drive
     * @param error Error message
     */
    void driveFailed(const QString& devicePath, const QString& error);

    /**
     * @brief Emitted when entire operation completes
     * @param result Complete results
     */
    void flashCompleted(const sak::FlashResult& result);

    /**
     * @brief Emitted on error
     * @param error Error message
     */
    void flashError(const QString& error);

private Q_SLOTS:
    void onWorkerProgress(double percentage, qint64 bytesWritten);
    void onWorkerCompleted(const sak::ValidationResult& result);
    void onWorkerFailed(const QString& error);

private:
    bool validateTargets(const QStringList& targetDrives);
    bool unmountVolumes(const QStringList& targetDrives);
    void updateProgress();
    void cleanupWorkers();
    
    std::unique_ptr<ImageSource> m_imageSource;
    std::vector<std::unique_ptr<FlashWorker>> m_workers;
    
    sak::FlashState m_state;
    sak::FlashProgress m_progress;
    sak::FlashResult m_result;
    
    bool m_verificationEnabled;
    qint64 m_bufferSize;
    int m_bufferCount;
    std::atomic<bool> m_isCancelled;
    
    QStringList m_targetDrives;
    QString m_sourceChecksum;
};

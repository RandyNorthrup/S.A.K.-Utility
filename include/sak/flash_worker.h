// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "image_source.h"
#include "worker_base.h"

#include <QList>
#include <QMutex>
#include <QString>

#include <atomic>
#include <optional>

#include <windows.h>

namespace sak {

/**
 * @brief Validation modes for verification
 */
enum class ValidationMode {
    Full,    ///< Read and verify every byte (most reliable)
    Sample,  ///< Verify random samples (faster, less thorough)
    Skip     ///< No verification (fastest)
};

/// @brief Maps the persisted image-flasher validation setting to a ValidationMode.
///
/// The settings dialog stores "full" / "quick" / "none";
/// ConfigManager::getImageFlasherValidationMode documents "full" as the default. Returns
/// nullopt for anything else so the caller can report the unrecognized value instead of
/// silently choosing for the user. Pure and unit-testable.
[[nodiscard]] inline std::optional<ValidationMode> validationModeFromSetting(
    const QString& setting) {
    if (setting == QLatin1String("full")) {
        return ValidationMode::Full;
    }
    if (setting == QLatin1String("quick")) {
        return ValidationMode::Sample;
    }
    if (setting == QLatin1String("none")) {
        return ValidationMode::Skip;
    }
    return std::nullopt;
}

/**
 * @brief Result of verification operation
 */
struct ValidationResult {
    bool passed = false;             ///< Overall success/failure
    QString sourceChecksum;          ///< Expected checksum (SHA-512)
    QString targetChecksum;          ///< Actual checksum from device
    QList<QString> errors;           ///< Detailed error messages
    qint64 mismatchOffset = -1;      ///< First mismatch byte position (-1 if none)
    int corruptedBlocks = 0;         ///< Number of blocks with errors
    double verificationSpeed = 0.0;  ///< MB/s read speed during verify
};

}  // namespace sak

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
 * Thread-Safety: the progress GETTERS (bytesWritten(), speedMBps()) are atomic and
 * readable from any thread while execute() runs on the worker thread. The setters
 * (verification/validation-mode/buffer-size) must be configured BEFORE start().
 * FlashWorker performs no OS-disk guard itself beyond a defense-in-depth self-check;
 * the authoritative fail-closed guard is FlashCoordinator, which is the only intended
 * constructor of this class.
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
    qint64 bytesWritten() const { return m_bytesWritten.load(std::memory_order_relaxed); }

    /**
     * @brief Get write speed
     * @return Speed in MB/s
     */
    double speedMBps() const { return m_speedMBps.load(std::memory_order_relaxed); }

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

    /// @brief Zero-pad a buffer up to a whole multiple of the device sector size
    /// @param buffer Buffer to pad in place (grown to the padded size)
    /// @param bytesRead In/out valid byte count; set to the padded size on success
    /// @param sectorSize Device logical sector size (512 for 512e, 4096 for 4Kn)
    /// @return false on a bogus sector size or a failed/oversized allocation
    /// @note Unbuffered raw-device writes must be a whole multiple of the LOGICAL
    ///       sector size; a 512-only assumption fails every write on a 4Kn disk.
    [[nodiscard]] static bool padToSectorSize(QByteArray& buffer,
                                              qint64& bytesRead,
                                              qint64 sectorSize);

    /// @brief Round a byte count up to a whole multiple of the sector size.
    /// @param bytes Byte count to align (must be >= 0).
    /// @param sectorSize Device logical sector size (> 0).
    /// @return The aligned length, or -1 on a bogus sector size, a negative
    ///         input, or a multiply overflow.
    /// @note Unbuffered raw-device reads must be a whole multiple of the logical
    ///       sector size; a short read length is rejected by ReadFile.
    [[nodiscard]] static qint64 alignUpToSectorSize(qint64 bytes, qint64 sectorSize);

    /// @brief Does an image fit on the target device?
    /// @param imageBytes Total (uncompressed) image size, or < 0 when unknown.
    /// @param deviceBytes Device capacity in bytes, or < 0 when it cannot be
    ///        queried.
    /// @return true only when BOTH sizes are known and the image fits. Fails
    ///         closed (false) on an unknown capacity (deviceBytes < 0) or an
    ///         undetermined image size (imageBytes < 0) -- never assumes "fits".
    [[nodiscard]] static bool imageFitsDevice(qint64 imageBytes, qint64 deviceBytes);

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
    /// @brief Executes the flash operation on the worker thread
    /// @return void on success, or sak::error_code on failure
    auto execute() -> std::expected<void, sak::error_code> override;

private:
    bool openDevice();
    void closeDevice();
    void cleanupFlashResources();
    /// @brief Defense-in-depth OS-disk self-guard. If the target physical drive backs
    ///        the running %WINDIR% volume, emits error(), cleans up, and returns true
    ///        (refused). Returns false to proceed. The authoritative fail-closed guard
    ///        is FlashCoordinator; this only catches a direct, coordinator-bypassing
    ///        construction and never blocks a flash the coordinator already cleared.
    [[nodiscard]] bool refuseIfTargetIsOsDisk();
    /// @brief Run the post-write verification stage (if enabled and not cancelled),
    ///        emitting verificationCompleted. Returns an error (after cleanup) when
    ///        verification fails; {} when it passes or is skipped.
    [[nodiscard]] auto runVerificationStage() -> std::expected<void, sak::error_code>;
    /// @brief Fail-closed capacity gate: emits error()+cleans up and returns
    ///        false when the image is larger than the (known) device capacity.
    bool ensureImageFitsTarget();
    /// @brief Best-effort lock+dismount; non-fatal (coordinator owns the
    ///        authoritative fail-closed dismount before the worker starts).
    void lockAndDismountBestEffort();
    bool lockVolume();
    bool unlockVolume();
    bool dismountVolume();

    bool writeImage();
    /// @brief Flush + fail-closed completeness check after the write loop.
    bool finalizeWrite();
    bool writeChunk(const char* buffer, qint64 bytesRead);
    bool prepareSourceChecksum();
    /// @brief Query the target's logical sector size (IOCTL_DISK_GET_DRIVE_GEOMETRY)
    /// @return true and sets m_sectorSize on success; false (fail closed) if the
    ///         size cannot be determined -- no fallback to an assumed 512 bytes.
    [[nodiscard]] bool queryDeviceSectorSize();
    /// @brief Query the target device's total capacity (IOCTL_DISK_GET_LENGTH_INFO)
    /// @return Capacity in bytes, or -1 if it cannot be determined
    qint64 queryDeviceCapacity();
    /// @brief Zero-pad the tail of a sector-aligned I/O buffer up to a whole
    ///        multiple of the device sector size, in place (no realloc).
    /// @param data Buffer base (must have @p capacity bytes; sector-aligned).
    /// @param bytesRead In/out valid byte count; set to the padded size on success.
    /// @param capacity Total allocated bytes of @p data.
    /// @return false on bogus geometry, overflow, or padded size > capacity.
    [[nodiscard]] bool padAlignedBuffer(char* data, qint64& bytesRead, qint64 capacity) const;
    sak::ValidationResult verifyImage();
    /// @brief Whole-image compare for images smaller than one sample block
    /// @note Fail-closed: mutates @p result to passed=false on any read failure
    ///       or byte mismatch, rather than auto-passing with nothing compared.
    void verifySmallImage(sak::ValidationResult& result);
    QString calculateChecksum(HANDLE handle, qint64 size);
    sak::ValidationResult verifyFull();
    sak::ValidationResult verifySample();
    /// @brief Configuration for sample block verification
    struct VerifyBlocksConfig {
        int num_samples;
        qint64 block_size;
        qint64 total_blocks;
        qint64 sample_size;
    };

    /// @brief Verify individual sample blocks against the target device
    /// @return Number of blocks successfully verified
    int verifySampleBlocks(sak::ValidationResult& result,
                           const VerifyBlocksConfig& config,
                           char* sourceData,
                           char* targetData);
    /// @brief Read one block back from the device at @p offset_bytes and compare
    ///        it to @p sourceData. Fails closed on a short device read.
    /// @param targetData Sector-aligned read-back buffer (FILE_FLAG_NO_BUFFERING).
    /// @return true if the block was fully read back (counts as a verified
    ///         sample, match or mismatch); false if it must be skipped.
    bool compareDeviceBlock(sak::ValidationResult& result,
                            qint64 offset_bytes,
                            qint64 compareLen,
                            const char* sourceData,
                            char* targetData);

    void updateProgress(qint64 bytesWritten);
    void updateSpeed(qint64 bytesWritten);
    void updateVerificationProgress(qint64 bytesVerified, qint64 totalBytes);

    std::unique_ptr<ImageSource> m_imageSource;
    QString m_targetDevice;
    QString m_sourceChecksum;  // Cached source checksum
    HANDLE m_deviceHandle;

    std::atomic<qint64> m_bytesWritten;
    qint64 m_totalBytes;
    /// Decompressed (unpadded) content length actually written; the meaningful
    /// prefix a full verify must hash. Distinct from m_bytesWritten, which counts
    /// the sector-padding tail as well.
    qint64 m_contentBytesWritten;
    std::atomic<double> m_speedMBps;
    qint64 m_bufferSize;
    qint64 m_sectorSize;  ///< Target logical sector size; 512 until queryDeviceSectorSize()
    bool m_verificationEnabled;
    sak::ValidationMode m_validationMode;

    QMutex m_mutex;
    qint64 m_lastProgressUpdate;
    qint64 m_lastSpeedUpdate;
    qint64 m_lastSpeedBytes;
    qint64 m_lastVerifyUpdate;
};

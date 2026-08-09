// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/layout_constants.h"

#include "drive_scanner.h"
#include "image_source.h"

#include <QElapsedTimer>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>
#include <vector>

class FlashWorker;
class DriveUnmounter;

namespace sak {

// Forward declarations. ValidationMode is declared rather than included: it lives in
// flash_worker.h, which pulls in <windows.h>, and this header is included by GUI
// translation units that must not inherit that. A scoped enum has a known underlying type
// (int), so an opaque declaration is sufficient to hold one by value.
struct ValidationResult;
enum class ValidationMode;

/**
 * @brief Flash operation state
 */
enum class FlashState {
    Idle,
    Validating,     // Validating image and drives
    Unmounting,     // Unmounting volumes
    Decompressing,  // Decompressing image (if needed)
    Flashing,       // Writing to drives
    Verifying,      // Verifying writes
    Completed,      // Successfully completed
    Failed,         // Failed with error
    Cancelled       // User cancelled
};

/**
 * @brief Progress information for flash operation
 */
struct FlashProgress {
    FlashState state;
    double percentage;         // Overall progress 0-100
    qint64 bytesWritten;       // Total bytes written across all drives
    qint64 totalBytes;         // Total bytes to write
    double speedMBps;          // Current write speed in MB/s
    int activeDrives;          // Number of drives currently being written
    int failedDrives;          // Number of drives that failed
    int completedDrives;       // Number of drives completed
    QString currentOperation;  // Human-readable description

    double getOverallProgress() const {
        if (totalBytes == 0) {
            return 0.0;
        }
        return (static_cast<double>(bytesWritten) / totalBytes) * kPercentMaxF;
    }
};

/**
 * @brief Result of flash operation
 */
struct FlashResult {
    bool success = false;
    qint64 bytesWritten = 0;
    double elapsedSeconds = 0.0;
    QStringList successfulDrives;
    QStringList failedDrives;
    QStringList errorMessages;
    QString sourceChecksum;
    /// Drives the eject-on-completion setting actually ejected, and those it could
    /// not. Both stay empty when the setting is off. Reported separately from
    /// successfulDrives because a flash can succeed on a drive that then refuses to
    /// eject, and telling the user a drive is safe to unplug when it is not is the
    /// failure this reporting exists to prevent.
    QStringList ejectedDrives;
    QStringList ejectFailedDrives;
    /// Drives that could not be brought back online when the run ended. The unmount
    /// step sets a PERSISTENT OFFLINE attribute, so a drive left in this list needs a
    /// manual `diskpart online disk`. Reported rather than only logged: telling the
    /// user a run is finished while a disk is still offline is the failure this
    /// reporting exists to prevent.
    QStringList reonlineFailedDrives;
    /// True only when post-write verification actually ran for this run. With
    /// verification disabled a "successful" drive means the WRITE succeeded and
    /// nothing read it back, and the result must say so rather than imply a verify.
    bool verified = false;

    bool hasErrors() const { return !failedDrives.isEmpty(); }
    int totalDrives() const { return successfulDrives.size() + failedDrives.size(); }
};

}  // namespace sak

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
 * Thread-Safety: the state/progress GETTERS (state(), progress(), isFlashing())
 * are mutex-guarded and callable from any thread. startFlash()/cancel() drive the
 * run and are intended to be called from a single (GUI) thread; startFlash refuses
 * re-entry atomically while a run is active. Signals are emitted on the calling
 * thread.
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

    /// @brief Return the first target device path that appears more than once in
    ///        @p targetDrives (case-/whitespace-insensitive), or empty if all are
    ///        distinct. Two workers writing the SAME physical disk concurrently
    ///        interleave their raw writes and corrupt each other, so a duplicate is
    ///        rejected before flashing. Pure; unit-testable.
    [[nodiscard]] static QString firstDuplicateTarget(const QStringList& targetDrives);

    /// @brief Return the first target whose PhysicalDrive NUMBER already appeared
    ///        earlier in @p targetDrives, or empty when every parseable target names a
    ///        distinct disk. String equality is not device identity --
    ///        "\\.\PhysicalDrive1" and "\\?\PhysicalDrive1" are different strings for
    ///        the SAME disk and slip past firstDuplicateTarget(). Paths with no
    ///        parseable number are skipped; validateSingleTarget() refuses those
    ///        outright. Pure; unit-testable.
    [[nodiscard]] static QString firstDuplicatePhysicalDrive(const QStringList& targetDrives);

    /// @brief Parse the PhysicalDrive number from a device path such as
    ///        "\\.\PhysicalDriveN". Returns the number (>=0), or -1 when the path
    ///        contains no parseable PhysicalDrive index. Pure; unit-testable.
    [[nodiscard]] static int parsePhysicalDriveNumber(const QString& devicePath);

    /**
     * @brief Cancel ongoing flash operation
     */
    void cancel();

    /**
     * @brief Check if flash is in progress
     * @return true if currently flashing
     */
    [[nodiscard]] bool isFlashing() const;

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
    [[nodiscard]] bool isVerificationEnabled() const;

    /**
     * @brief Set buffer size for reading/writing
     * @param sizeBytes Buffer size in bytes (default 256MB)
     */
    void setBufferSize(qint64 sizeBytes);

    /**
     * @brief Set the post-write verification depth applied to every worker.
     * @param mode Full, Sample or Skip.
     *
     * Every worker this coordinator starts is given this mode. Without it the workers
     * silently kept their own Full default, so the Image Flasher settings dialog's
     * validation choice was collected, stored and then ignored.
     */
    void setValidationMode(sak::ValidationMode mode);

    /**
     * @brief Cap how many target drives are written at the same time.
     * @param maxWrites Ceiling on concurrent writes (>= 1).
     *
     * Drives beyond the ceiling are queued and started as earlier ones finish, so a
     * multi-drive run still writes every target -- it just does not contend for the
     * same bus. A value below 1 is refused (logged, ceiling unchanged) rather than
     * accepted as "write nothing".
     */
    void setMaxConcurrentWrites(int maxWrites);

    /// @brief Current concurrent-write ceiling.
    [[nodiscard]] int maxConcurrentWrites() const;

    /**
     * @brief Eject each successfully flashed drive when the run finishes.
     * @param eject true to eject, false to leave the drives mounted.
     *
     * Only SUCCESSFUL drives are ejected. A failed target is left mounted so it can
     * be inspected or re-flashed without being unplugged and re-inserted first. The
     * per-drive outcome is reported in FlashResult::ejectedDrives /
     * ejectFailedDrives, never assumed.
     */
    void setEjectOnCompletion(bool eject);

    /// @brief Whether successfully flashed drives are ejected when the run finishes.
    [[nodiscard]] bool ejectOnCompletion() const;

    /// @brief How many queued workers may start right now, given the ceiling, the
    ///        number already writing and the number still queued. Pure;
    ///        unit-testable. See sak::startableWorkerCount.
    [[nodiscard]] static int startableWorkerCount(int maxConcurrent, int running, int pending);

    /// @brief Eject every drive in @p result.successfulDrives, appending each device
    ///        path to result.ejectedDrives or result.ejectFailedDrives.
    ///
    /// Static and public because the Image Flasher has two writers: this coordinator
    /// for raw images, and WindowsUSBCreator for Windows installation media. Both
    /// honour the same eject-on-completion setting, and they share this so the two
    /// paths cannot drift into reporting removal differently.
    static void ejectCompletedDrives(sak::FlashResult& result);

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
    bool validateImagePath(const QString& imagePath);
    bool prepareImageSource(const QString& imagePath);
    bool unmountAndFlash(const QString& imagePath, const QStringList& targetDrives);

    bool validateTargets(const QStringList& targetDrives);
    /// @brief Validate one target device (accessible, valid disk, not the OS disk).
    /// @note Emits flashError() on rejection.
    bool validateSingleTarget(const QString& devicePath);
    /// @brief Fail-closed OS-disk guard: true only when devicePath is provably NOT
    ///        the running OS disk. Refuses (logs + emits flashError, returns false)
    ///        on an unparseable drive number, a system-volume disk, or an
    ///        indeterminate probe -- never assumes "safe".
    bool passesOsDiskGuard(const QString& devicePath);
    /// @brief Fail-closed boot/ESP-disk guard, independent of the %WINDIR% disk. True
    ///        only when the drive provably does NOT carry the Windows boot loader;
    ///        refuses on a boot disk OR an indeterminate probe. Protects a split-boot
    ///        ESP that lives on a different physical disk than \Windows.
    bool passesBootDiskGuard(const QString& devicePath, int driveNumber);
    /// @brief Stop a run that was cancelled during its synchronous phase: re-online
    ///        anything already taken offline, publish Cancelled and drop the image
    ///        handle. Returns true when the run was cancelled (caller must bail).
    bool abortIfCancelled(const QString& reason);
    bool unmountVolumes(const QStringList& targetDrives);
    /// @brief Roll back (clear OFFLINE on) drives already offlined this run when a
    ///        later target fails to unmount, so earlier targets are not stranded.
    /// @return The drives that refused to come back online (empty when all did).
    QStringList rollbackOfflinedDrives(const QStringList& offlinedPaths);
    /// @brief Roll back every drive offlined so far and SURFACE the ones that stayed
    ///        offline (flashError, not a log line) -- a stranded disk needs a manual
    ///        `diskpart online disk`. Leaves the still-offline drives recorded so a
    ///        later teardown tries again.
    void rollbackAndReport();
    /// @brief Clear the recorded offline set, bringing each of those drives back
    ///        online. Drives that refuse are returned AND kept recorded so the
    ///        destructor retries them.
    QStringList reonlineOfflinedDrives();
    /// @brief Atomically refuse re-entry and, if idle, claim the run by moving the
    ///        state to Validating under the lock. Returns false if a run is active OR
    ///        if a worker from a previous run is still writing.
    bool beginFlashClaim();
    /// @brief Publish a new coordinator state under m_mutex (m_state and the mirrored
    ///        m_progress.state are read from other threads by the locked getters, so
    ///        an unlocked write is a data race). Emit stateChanged AFTER this returns,
    ///        never while the lock is held (B10-07).
    void setState(sak::FlashState state);
    /// @brief Move the run to Failed, announce it, and release the coordinator's image
    ///        handle. @p errorMessage may be empty when the failing step already
    ///        emitted its own flashError.
    void failRun(const QString& stateMessage, const QString& errorMessage);
    /// @brief Close and drop the coordinator's own image source, so a run that ends
    ///        before the workers start does not leak the open handle.
    void releaseImageSource();
    /// @brief Zero the per-run progress/result/dedup state and start the run clock.
    ///        Runs BEFORE the first step that can fail, so a rejected image cannot
    ///        leave the previous run's totals on display.
    void resetRunState(const QStringList& targetDrives);
    /// @brief Fail-closed byte-total gate run BEFORE anything is taken offline: the
    ///        opened source must report a positive size that multiplies by the target
    ///        count without overflowing. Refuses (failRun) otherwise.
    bool recordTotalBytes(const QString& imagePath, qsizetype targetCount);
    /// @brief Move Flashing -> Verifying the first time a worker reports verification
    ///        progress, so the reported state matches what the run is actually doing.
    void noteVerificationStarted();
    void updateProgress();
    /// Record one drive's success/failure into m_progress + m_result. Call with
    /// m_mutex held. Returns the error message on failure (empty on success).
    QString recordDriveCompletion(const QString& devicePath, const sak::ValidationResult& result);
    /// Populate FlashResult::bytesWritten (summed across drives) + elapsedSeconds at
    /// finalize. Call with m_mutex held. (These were previously left at zero.)
    /// Builds one queued FlashWorker per target. On failure the partial queue is retired, the
    /// already-offline disks are put back, and the run fails closed.
    [[nodiscard]] bool buildWorkerQueue(const QString& imagePath, const QStringList& targetDrives);
    void finalizeResultMetrics();
    /// When this drive was the last one outstanding, settles the run's terminal state and
    /// copies out what the caller needs to emit AFTER releasing m_mutex. Returns false while
    /// drives remain. MUST be called with m_mutex held.
    [[nodiscard]] bool finalizeRunIfLastDrive(sak::FlashState* terminalState,
                                              sak::FlashResult* terminalResult,
                                              QString* terminalMessage);
    /// Emit the terminal stateChanged + flashCompleted signals and tear workers down.
    /// MUST be called with m_mutex released (it emits and cleanupWorkers() can wait()).
    void emitTerminalOutcome(sak::FlashState state,
                             const sak::FlashResult& result,
                             const QString& statusMessage);
    /// Clear the persistent OFFLINE attribute on every successfully flashed+verified
    /// drive so completed media is not left permanently offline (the attribute was
    /// set at unmount time and survives the flash). Best-effort per drive; MUST be
    /// called with m_mutex released (it opens the physical drive).
    /// @return The drives that could not be brought back online (empty when all did).
    QStringList reonlineDrives(const QStringList& drivePaths);
    void cleanupWorkers();
    /// Cooperatively stop, wait for and destroy @p workers (detaching any that refuse
    /// to stop rather than terminating a live raw write). Takes the vector by reference
    /// and empties it, so a caller can hand over a run's workers BEFORE emitting its
    /// terminal signals -- a handler that starts the next run then cannot have the new
    /// run's workers torn down underneath it. MUST be called with m_mutex released.
    void retireWorkers(std::vector<std::unique_ptr<FlashWorker>>& workers);
    /// Start as many queued workers as the concurrent-write ceiling currently
    /// allows. Called once the workers are built and again each time one finishes.
    /// Starts nothing once the run has been cancelled or has left Flashing/Verifying,
    /// so a cancel cannot be followed by a queued drive quietly starting to write.
    /// MUST be called with m_mutex released (it takes the lock itself and then
    /// starts threads outside it).
    void startPendingWorkers();
    /// Start each of @p workers and report the ones QThread refused to launch as failed
    /// drives, so a thread that never started cannot leave the run waiting forever for
    /// a terminal signal it can never send. MUST be called with m_mutex released.
    void startWorkers(const std::vector<FlashWorker*>& workers);
    /// Record every still-queued (never-started) worker as a failed drive so a
    /// cancelled run can still reach its finalize predicate. MUST be called with
    /// m_mutex released (it takes the lock itself).
    void markQueuedWorkersCancelled();
    /// Wire one worker's progress/completion/error/abort signals to the coordinator.
    void connectWorkerSignals(FlashWorker* worker);
    /// Record a drive failure and finalize the run if all drives are done. Deduped by
    /// @p worker so a drive that already reported a terminal outcome (FlashWorker::error
    /// or verificationCompleted) is not double-counted when its WorkerBase-level
    /// failed()/cancelled() also fires. Shared by onWorkerFailed and the WorkerBase
    /// terminal handlers (the latter catch an execute() exception that surfaces ONLY as
    /// WorkerBase::failed, which would otherwise never finalize -> hang).
    void onWorkerFailedFor(const FlashWorker* worker, const QString& error);

    std::unique_ptr<ImageSource> m_imageSource;
    std::vector<std::unique_ptr<FlashWorker>> m_workers;

    mutable QMutex m_mutex;                  ///< Guards m_state, m_progress, m_result
    sak::FlashState m_state;                 // Protected by m_mutex
    sak::FlashProgress m_progress;           // Protected by m_mutex
    sak::FlashResult m_result;               // Protected by m_mutex
    QSet<const QObject*> m_reportedWorkers;  ///< Workers that reported a terminal (m_mutex)

    bool m_verificationEnabled;
    qint64 m_bufferSize;
    int m_maxConcurrentWrites;
    bool m_ejectOnCompletion;
    /// Index of the first worker in m_workers that has not been started yet.
    /// Everything before it is running or finished. Protected by m_mutex.
    size_t m_nextWorkerIndex;
    // Initialized in the constructor, not here: ValidationMode is only forward-declared in
    // this header, so its enumerators are not nameable at this point.
    sak::ValidationMode m_validationMode;
    std::atomic<bool> m_isCancelled;

    QStringList m_targetDrives;
    /// Targets whose PERSISTENT OFFLINE attribute this run set and has not cleared
    /// yet. Written and read only on the coordinator (run-driving) thread, so it is
    /// not part of the m_mutex set; it exists so a teardown that never reaches
    /// emitTerminalOutcome (destructor, cancel during the synchronous phase) can still
    /// bring the disks back instead of stranding them offline.
    QStringList m_offlinedDrives;
    QElapsedTimer m_flashTimer;  ///< Wall-clock for FlashResult::elapsedSeconds
};

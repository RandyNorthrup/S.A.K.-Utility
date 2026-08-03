// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QFuture>
#include <QMutex>
#include <QObject>
#include <QPair>
#include <QQueue>
#include <QThread>
#include <QVector>
#include <QWaitCondition>

#include <atomic>
#include <memory>

// Include full definitions needed for shared_ptr usage
#include "sak/action_constants.h"
#include "sak/chocolatey_manager.h"
#include "sak/migration_report.h"
#include "sak/network_constants.h"

namespace sak {

/**
 * @brief Status of a migration job
 */
enum class MigrationStatus {
    Pending,     ///< Not yet started
    Queued,      ///< In queue waiting for worker thread
    Installing,  ///< Currently installing
    Success,     ///< Successfully installed
    Failed,      ///< Installation failed
    Skipped,     ///< Skipped by user
    Cancelled    ///< Cancelled by user
};

/**
 * @brief Installation job for a single package
 */
struct MigrationJob {
    int entryIndex{-1};    ///< Index in MigrationReport entries
    QString appName;       ///< Application display name
    QString packageId;     ///< Chocolatey package ID
    QString version;       ///< Requested version (empty = latest)
    MigrationStatus status = MigrationStatus::Pending;
    QString errorMessage;  ///< Error details if failed
    QDateTime startTime;   ///< When installation started
    QDateTime endTime;     ///< When installation completed
    int retryCount = 0;    ///< Number of retry attempts
};

/**
 * @brief Background worker thread for app migration
 *
 * Manages parallel installation of Chocolatey packages with:
 * - Queue-based job processing
 * - Pause/resume/cancel support
 * - Retry logic with exponential backoff
 * - Progress tracking with signals
 * - Thread-safe status updates
 */
class AppInstallationWorker : public QObject {
    Q_OBJECT

public:
    /**
     * @brief Construct worker with Chocolatey manager
     * @param chocoManager Chocolatey manager for installations
     * @param parent Parent QObject
     */
    explicit AppInstallationWorker(std::shared_ptr<ChocolateyManager> chocoManager,
                                   QObject* parent = nullptr);

    ~AppInstallationWorker();

    /// @brief Upper bound on install concurrency. Chocolatey holds a global lock,
    ///        so installs run serially regardless; this only caps the requested
    ///        value. 0 is the queue-only (dry-run) mode used by validation paths.
    static constexpr int kMaxInstallConcurrency = 8;

    /// @brief Clamp a requested concurrency to [0, kMaxInstallConcurrency]. Pure;
    ///        unit-testable. 0 stays valid (queue-without-installing); negatives
    ///        clamp to 0 and absurd values to the cap so scheduling never stalls
    ///        on an out-of-range request.
    [[nodiscard]] static int boundConcurrency(int requested);

    /// @brief Reason a SELECTED migration entry cannot be queued, or empty when it
    ///        is installable. Covers a missing choco package, an unavailable
    ///        package, and a version lock with no locked version (which would
    ///        otherwise silently install latest). Pure; unit-testable. Makes a skip
    ///        an explicit, reported outcome instead of a silent drop.
    [[nodiscard]] static QString migrationSkipReason(const MigrationReport::MigrationEntry& entry);

    /// @brief True iff @p candidate names @p target as a WHOLE word (case
    ///        insensitive), so an unrelated program whose name merely embeds the
    ///        target token does not falsely certify an install. Pure;
    ///        unit-testable.
    [[nodiscard]] static bool nameIndicatesApp(const QString& candidate, const QString& target);

    /**
     * @brief Start migration from report
     * @param report Migration report with selected entries
     * @param maxConcurrent Maximum concurrent installations (default: 2). A value of 0 queues

     * * jobs without launching installers, which is used by non-installing validation paths.
     *
     * @return Number of jobs queued
     */
    int startMigration(std::shared_ptr<MigrationReport> report,
                       int maxConcurrent = kMaxConcurrentTransfers);

    /**
     * @brief Pause migration
     * Completes current installations, then pauses
     */
    void pause();

    /**
     * @brief Resume paused migration
     */
    void resume();

    /**
     * @brief Cancel migration
     * Cancels queued jobs, waits for current to finish
     */
    void cancel();

    /**
     * @brief Check if migration is running
     */
    [[nodiscard]] bool isRunning() const;

    /**
     * @brief Check if migration is paused
     */
    [[nodiscard]] bool isPaused() const;

    /**
     * @brief Get current job statistics
     */
    struct Stats {
        int total = 0;       ///< Total jobs
        int pending = 0;     ///< Not started
        int queued = 0;      ///< In queue
        int installing = 0;  ///< Currently running
        int success = 0;     ///< Completed successfully
        int failed = 0;      ///< Failed
        int skipped = 0;     ///< Skipped
        int cancelled = 0;   ///< Cancelled
    };
    Stats getStats() const;

    /**
     * @brief Get current jobs
     * Thread-safe copy of job queue
     */
    QVector<MigrationJob> getJobs() const;

Q_SIGNALS:
    /**
     * @brief Emitted when migration starts
     * @param totalJobs Number of jobs to process
     */
    void migrationStarted(int totalJobs);

    /**
     * @brief Emitted when a job status changes
     * @param entryIndex Index in report entries
     * @param job Job details
     */
    void jobStatusChanged(int entryIndex, const MigrationJob& job);

    /**
     * @brief Emitted for job progress updates
     * @param entryIndex Index in report entries
     * @param message Progress message
     */
    void jobProgress(int entryIndex, const QString& message);

    /**
     * @brief Emitted when migration completes (all jobs done)
     * @param stats Final statistics
     */
    void migrationCompleted(const AppInstallationWorker::Stats& stats);

    /**
     * @brief Emitted when migration is paused
     */
    void migrationPaused();

    /**
     * @brief Emitted when migration is resumed
     */
    void migrationResumed();

    /**
     * @brief Emitted when migration is cancelled
     */
    void migrationCancelled();

private Q_SLOTS:
    /**
     * @brief Process job queue
     * Runs in worker thread
     */
    void processQueue();

    void onInstallStarted(const QString& packageId);
    void onInstallSuccess(const QString& packageId);
    void onInstallFailed(const QString& packageId, const QString& error);
    void onInstallRetrying(const QString& packageId, int attempt);

private:
    enum class QueueAction {
        Proceed,
        Wait,
        Finish
    };
    QueueAction checkQueueState();

    /// @brief Populate m_jobs/m_jobQueue from the report's selected entries and
    ///        record each skipped selected entry (reason) in the report. Caller
    ///        must hold m_mutex. Returns the (entryIndex, reason) pairs so the
    ///        caller can notify listeners after releasing the lock.
    QVector<QPair<int, QString>> buildJobQueue();

    /// @brief Build a queued job from an installable entry (version pinned only
    ///        when the entry locks a concrete version).
    static MigrationJob makeJob(int entryIndex, const MigrationReport::MigrationEntry& entry);

    /**
     * @brief Install a single package
     * @param job Job to process
     * @return True if successful
     */
    bool installPackage(int jobIndex, MigrationJob& job);

    /// @brief Decrement the active-job count and, on a retryable failure, back off
    ///        and re-enqueue the job -- unless a cancel landed during the backoff,
    ///        in which case the job is dropped rather than resurrected.
    void handleJobResult(int jobIndex, MigrationJob& job, bool success);

    /**
     * @brief Store a job snapshot after background-thread changes
     */
    void storeJobSnapshot(int index, const MigrationJob& job);

    /**
     * @brief Verify installation via multi-source check
     *
     * Checks Chocolatey output, Windows Registry, and AppX packages
     * to confirm the application was actually installed.
     *
     * @param job The migration job with app name and package ID
     * @param choco_result The result from Chocolatey install
     * @return True only if a source (choco count line, registry, or AppX)
     *         positively confirms the install; false when nothing confirms it
     *         (inconclusive is treated as unverified, not success).
     */
    bool verifyInstallation(const MigrationJob& job, const ChocolateyManager::Result& choco_result);

    /**
     * @brief Check if a newer or equal version is already installed
     *
     * Scans Windows Registry and AppX packages for the application.
     * If found with a version >= the requested version, the install
     * should be skipped.
     *
     * @param job The migration job with app name and version
     * @param[out] installed_version The version found on the system
     * @return True if a newer or equal version is already present
     */
    bool isNewerVersionInstalled(const MigrationJob& job, QString& installed_version);

    /**
     * @brief Update job status (thread-safe)
     */
    void updateJobStatus(int index, MigrationStatus status, const QString& error = QString());

    /**
     * @brief Check if should retry failed job
     */
    bool shouldRetry(const MigrationJob& job) const;

    /**
     * @brief Calculate retry delay (exponential backoff)
     * @param retryCount Number of retries so far
     * @return Delay in milliseconds
     */
    int getRetryDelay(int retryCount) const;

    std::shared_ptr<ChocolateyManager> m_chocoManager;
    std::shared_ptr<MigrationReport> m_report;

    QVector<MigrationJob> m_jobs;
    QQueue<int> m_jobQueue;  ///< Indices of pending jobs
    QFuture<void> m_processFuture;

    mutable QMutex m_mutex;          ///< Protects job data
    QWaitCondition m_waitCondition;  ///< For pause/resume

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_cancelled{false};
    int m_maxConcurrent = kMaxConcurrentTransfers;
    int m_activeJobs = 0;
};

}  // namespace sak

Q_DECLARE_METATYPE(sak::MigrationJob)
Q_DECLARE_METATYPE(sak::MigrationStatus)
Q_DECLARE_METATYPE(sak::AppInstallationWorker::Stats)

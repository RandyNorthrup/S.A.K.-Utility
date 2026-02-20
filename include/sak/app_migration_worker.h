#pragma once

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QQueue>
#include <QFuture>
#include <QDateTime>
#include <memory>

// Include full definitions needed for shared_ptr usage
#include "sak/chocolatey_manager.h"
#include "sak/migration_report.h"

namespace sak {

/**
 * @brief Status of a migration job
 */
enum class MigrationStatus {
    Pending,      ///< Not yet started
    Queued,       ///< In queue waiting for worker thread
    Installing,   ///< Currently installing
    Success,      ///< Successfully installed
    Failed,       ///< Installation failed
    Skipped,      ///< Skipped by user
    Cancelled     ///< Cancelled by user
};

/**
 * @brief Installation job for a single package
 */
struct MigrationJob {
    int entryIndex;                    ///< Index in MigrationReport entries
    QString appName;                   ///< Application display name
    QString packageId;                 ///< Chocolatey package ID
    QString version;                   ///< Requested version (empty = latest)
    MigrationStatus status = MigrationStatus::Pending;
    QString errorMessage;              ///< Error details if failed
    QDateTime startTime;               ///< When installation started
    QDateTime endTime;                 ///< When installation completed
    int retryCount = 0;                ///< Number of retry attempts
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
class AppMigrationWorker : public QObject {
    Q_OBJECT

public:
    /**
     * @brief Construct worker with Chocolatey manager
     * @param chocoManager Chocolatey manager for installations
     * @param parent Parent QObject
     */
    explicit AppMigrationWorker(std::shared_ptr<ChocolateyManager> chocoManager,
                                QObject* parent = nullptr);
    
    ~AppMigrationWorker();

    /**
     * @brief Start migration from report
     * @param report Migration report with selected entries
     * @param maxConcurrent Maximum concurrent installations (default: 2)
     * @return Number of jobs queued
     */
    int startMigration(std::shared_ptr<MigrationReport> report, int maxConcurrent = 2);

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
    bool isRunning() const;

    /**
     * @brief Check if migration is paused
     */
    bool isPaused() const;

    /**
     * @brief Get current job statistics
     */
    struct Stats {
        int total = 0;        ///< Total jobs
        int pending = 0;      ///< Not started
        int queued = 0;       ///< In queue
        int installing = 0;   ///< Currently running
        int success = 0;      ///< Completed successfully
        int failed = 0;       ///< Failed
        int skipped = 0;      ///< Skipped
        int cancelled = 0;    ///< Cancelled
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
    void migrationCompleted(const AppMigrationWorker::Stats& stats);

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

    /**
     * @brief Handle Chocolatey manager signals
     */
    void onInstallStarted(const QString& packageId);
    void onInstallSuccess(const QString& packageId);
    void onInstallFailed(const QString& packageId, const QString& error);
    void onInstallRetrying(const QString& packageId, int attempt);

private:
    /**
     * @brief Install a single package
     * @param job Job to process
     * @return True if successful
     */
    bool installPackage(MigrationJob& job);

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
    QQueue<int> m_jobQueue;              ///< Indices of pending jobs
    QFuture<void> m_processFuture;
    
    mutable QMutex m_mutex;              ///< Protects job data
    QWaitCondition m_waitCondition;      ///< For pause/resume
    
    bool m_running = false;
    bool m_paused = false;
    bool m_cancelled = false;
    int m_maxConcurrent = 2;
    int m_activeJobs = 0;
    
    static const int MAX_RETRIES = 3;
    static const int BASE_RETRY_DELAY_MS = 5000;  ///< 5 seconds
};

} // namespace sak


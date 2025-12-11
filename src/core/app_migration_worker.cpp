#include "sak/app_migration_worker.h"
#include "sak/chocolatey_manager.h"
#include "sak/migration_report.h"
#include "sak/app_scanner.h"
#include "sak/package_matcher.h"
#include <QDebug>
#include <QThread>
#include <QMetaObject>
#include <QTimer>

namespace sak {

AppMigrationWorker::AppMigrationWorker(std::shared_ptr<ChocolateyManager> chocoManager,
                                       QObject* parent)
    : QObject(parent)
    , m_chocoManager(chocoManager)
{
    // Connect to Chocolatey manager signals
    connect(m_chocoManager.get(), &ChocolateyManager::installStarted,
            this, &AppMigrationWorker::onInstallStarted);
    connect(m_chocoManager.get(), &ChocolateyManager::installSuccess,
            this, &AppMigrationWorker::onInstallSuccess);
    connect(m_chocoManager.get(), &ChocolateyManager::installFailed,
            this, &AppMigrationWorker::onInstallFailed);
    connect(m_chocoManager.get(), &ChocolateyManager::installRetrying,
            this, &AppMigrationWorker::onInstallRetrying);
}

AppMigrationWorker::~AppMigrationWorker() {
    if (m_workerThread) {
        m_workerThread->quit();
        m_workerThread->wait();
        delete m_workerThread;
    }
}

int AppMigrationWorker::startMigration(std::shared_ptr<MigrationReport> report, int maxConcurrent) {
    QMutexLocker locker(&m_mutex);
    
    if (m_running) {
        qWarning() << "[AppMigrationWorker] Migration already running";
        return 0;
    }
    
    m_report = report;
    m_maxConcurrent = qMax(1, maxConcurrent);
    m_running = true;
    m_paused = false;
    m_cancelled = false;
    m_activeJobs = 0;
    
    // Build job list from selected entries
    m_jobs.clear();
    m_jobQueue.clear();
    
    const auto& entries = m_report->getEntries();
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        
        // Only queue selected entries with matches
        if (entry.selected && !entry.choco_package.isEmpty()) {
            MigrationJob job;
            job.entryIndex = static_cast<int>(i);
            job.appName = entry.app_name;
            job.packageId = entry.choco_package;
            job.version = QString();  // Use latest version
            job.status = MigrationStatus::Queued;
            
            m_jobs.append(job);
            m_jobQueue.enqueue(m_jobs.size() - 1);
        }
    }
    
    int totalJobs = m_jobs.size();
    qDebug() << "[AppMigrationWorker] Starting migration with" << totalJobs << "jobs,"
             << m_maxConcurrent << "max concurrent";
    
    Q_EMIT migrationStarted(totalJobs);
    
    // Start worker thread
    if (!m_workerThread) {
        m_workerThread = new QThread(this);
        m_workerThread->start();
    }
    
    // Process queue in background
    QMetaObject::invokeMethod(this, "processQueue", Qt::QueuedConnection);
    
    return totalJobs;
}

void AppMigrationWorker::pause() {
    QMutexLocker locker(&m_mutex);
    
    if (!m_running || m_paused) {
        return;
    }
    
    m_paused = true;
    qDebug() << "[AppMigrationWorker] Migration paused";
    Q_EMIT migrationPaused();
}

void AppMigrationWorker::resume() {
    QMutexLocker locker(&m_mutex);
    
    if (!m_running || !m_paused) {
        return;
    }
    
    m_paused = false;
    qDebug() << "[AppMigrationWorker] Migration resumed";
    
    // Wake up worker thread
    m_waitCondition.wakeAll();
    Q_EMIT migrationResumed();
}

void AppMigrationWorker::cancel() {
    QMutexLocker locker(&m_mutex);
    
    if (!m_running) {
        return;
    }
    
    m_cancelled = true;
    m_paused = false;  // Unpause to allow cancellation
    
    // Cancel all queued jobs
    while (!m_jobQueue.isEmpty()) {
        int jobIndex = m_jobQueue.dequeue();
        m_jobs[jobIndex].status = MigrationStatus::Cancelled;
        
        // Update report entry
        if (m_report) {
            m_report->getEntry(m_jobs[jobIndex].entryIndex).status = "cancelled";
        }
        
        Q_EMIT jobStatusChanged(m_jobs[jobIndex].entryIndex, m_jobs[jobIndex]);
    }
    
    qDebug() << "[AppMigrationWorker] Migration cancelled";
    
    // Wake up worker thread
    m_waitCondition.wakeAll();
    Q_EMIT migrationCancelled();
}

bool AppMigrationWorker::isRunning() const {
    QMutexLocker locker(&m_mutex);
    return m_running;
}

bool AppMigrationWorker::isPaused() const {
    QMutexLocker locker(&m_mutex);
    return m_paused;
}

AppMigrationWorker::Stats AppMigrationWorker::getStats() const {
    QMutexLocker locker(&m_mutex);
    
    Stats stats;
    stats.total = m_jobs.size();
    
    for (const auto& job : m_jobs) {
        switch (job.status) {
            case MigrationStatus::Pending:    stats.pending++; break;
            case MigrationStatus::Queued:     stats.queued++; break;
            case MigrationStatus::Installing: stats.installing++; break;
            case MigrationStatus::Success:    stats.success++; break;
            case MigrationStatus::Failed:     stats.failed++; break;
            case MigrationStatus::Skipped:    stats.skipped++; break;
            case MigrationStatus::Cancelled:  stats.cancelled++; break;
        }
    }
    
    return stats;
}

QVector<MigrationJob> AppMigrationWorker::getJobs() const {
    QMutexLocker locker(&m_mutex);
    return m_jobs;
}

void AppMigrationWorker::processQueue() {
    while (true) {
        m_mutex.lock();
        
        // Check if should stop
        if (m_cancelled || (!m_running && m_activeJobs == 0)) {
            m_running = false;
            m_mutex.unlock();
            
            auto stats = getStats();
            Q_EMIT migrationCompleted(stats);
            break;
        }
        
        // Check if paused
        if (m_paused) {
            m_waitCondition.wait(&m_mutex);
            m_mutex.unlock();
            continue;
        }
        
        // Check if can start new job
        if (m_activeJobs >= m_maxConcurrent || m_jobQueue.isEmpty()) {
            if (m_activeJobs == 0 && m_jobQueue.isEmpty()) {
                // All jobs complete
                m_running = false;
                m_mutex.unlock();
                
                auto stats = getStats();
                qDebug() << "[AppMigrationWorker] Migration completed:"
                         << "Success:" << stats.success
                         << "Failed:" << stats.failed
                         << "Cancelled:" << stats.cancelled;
                Q_EMIT migrationCompleted(stats);
                break;
            }
            
            // Wait for active jobs to finish
            m_mutex.unlock();
            QThread::msleep(100);
            continue;
        }
        
        // Get next job
        int jobIndex = m_jobQueue.dequeue();
        MigrationJob& job = m_jobs[jobIndex];
        
        m_activeJobs++;
        m_mutex.unlock();
        
        // Install package
        bool success = installPackage(job);
        
        m_mutex.lock();
        m_activeJobs--;
        
        // Handle retry logic
        if (!success && shouldRetry(job)) {
            int delay = getRetryDelay(job.retryCount);
            qDebug() << "[AppMigrationWorker] Retrying" << job.packageId
                     << "in" << delay << "ms (attempt" << (job.retryCount + 1) << ")";
            
            m_mutex.unlock();
            QThread::msleep(delay);
            m_mutex.lock();
            
            job.retryCount++;
            m_jobQueue.enqueue(jobIndex);
        }
        
        m_mutex.unlock();
    }
}

bool AppMigrationWorker::installPackage(MigrationJob& job) {
    qDebug() << "[AppMigrationWorker] Installing" << job.packageId;
    
    // Update status to installing
    job.status = MigrationStatus::Installing;
    job.startTime = QDateTime::currentDateTime();
    Q_EMIT jobStatusChanged(job.entryIndex, job);
    Q_EMIT jobProgress(job.entryIndex, "Installing " + job.packageId + "...");
    
    // Install via Chocolatey
    ChocolateyManager::InstallConfig config;
    config.package_name = job.packageId;
    config.version = job.version;
    config.version_locked = !job.version.isEmpty();
    config.auto_confirm = true;
    config.force = false;
    config.allow_unofficial = false;
    
    auto result = m_chocoManager->installPackage(config);
    bool success = result.success;
    
    // Update status
    job.endTime = QDateTime::currentDateTime();
    
    if (success) {
        job.status = MigrationStatus::Success;
        Q_EMIT jobProgress(job.entryIndex, "Successfully installed " + job.packageId);
        qDebug() << "[AppMigrationWorker] Success:" << job.packageId;
    } else {
        job.status = MigrationStatus::Failed;
        job.errorMessage = result.error_message.isEmpty() ? "Installation failed" : result.error_message;
        Q_EMIT jobProgress(job.entryIndex, "Failed to install " + job.packageId);
        qWarning() << "[AppMigrationWorker] Failed:" << job.packageId << "-" << job.errorMessage;
    }
    
    Q_EMIT jobStatusChanged(job.entryIndex, job);
    
    return success;
}

void AppMigrationWorker::updateJobStatus(int index, MigrationStatus status, const QString& error) {
    QMutexLocker locker(&m_mutex);
    
    if (index < 0 || index >= m_jobs.size()) {
        return;
    }
    
    m_jobs[index].status = status;
    if (!error.isEmpty()) {
        m_jobs[index].errorMessage = error;
    }
    
    Q_EMIT jobStatusChanged(m_jobs[index].entryIndex, m_jobs[index]);
}

bool AppMigrationWorker::shouldRetry(const MigrationJob& job) const {
    return job.status == MigrationStatus::Failed && 
           job.retryCount < MAX_RETRIES &&
           !m_cancelled;
}

int AppMigrationWorker::getRetryDelay(int retryCount) const {
    // Exponential backoff: 5s, 10s, 20s
    return BASE_RETRY_DELAY_MS * (1 << retryCount);
}

void AppMigrationWorker::onInstallStarted(const QString& packageId) {
    Q_EMIT jobProgress(-1, "Starting installation of " + packageId);
}

void AppMigrationWorker::onInstallSuccess(const QString& packageId) {
    Q_EMIT jobProgress(-1, "Successfully installed " + packageId);
}

void AppMigrationWorker::onInstallFailed(const QString& packageId, const QString& error) {
    Q_EMIT jobProgress(-1, "Failed to install " + packageId + ": " + error);
}

void AppMigrationWorker::onInstallRetrying(const QString& packageId, int attempt) {
    Q_EMIT jobProgress(-1, QString("Retrying %1 (attempt %2)").arg(packageId).arg(attempt));
}

} // namespace sak

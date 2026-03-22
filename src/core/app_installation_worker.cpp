// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/app_installation_worker.h"

#include "sak/app_scanner.h"
#include "sak/chocolatey_manager.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/migration_report.h"
#include "sak/package_matcher.h"

#include <QMetaObject>
#include <QRegularExpression>
#include <QtConcurrent>
#include <QtGlobal>
#include <QThread>
#include <QTimer>
#include <QVersionNumber>

namespace sak {

namespace {

bool isVersionNewer(const QString& version, const QVersionNumber& requested) {
    if (version.isEmpty()) {
        return false;
    }
    QVersionNumber system_ver = QVersionNumber::fromString(version);
    return !system_ver.isNull() && system_ver >= requested;
}

}  // namespace

AppInstallationWorker::AppInstallationWorker(std::shared_ptr<ChocolateyManager> chocoManager,
                                             QObject* parent)
    : QObject(parent), m_chocoManager(chocoManager) {
    // Register custom types for cross-thread signal delivery
    qRegisterMetaType<sak::MigrationJob>("sak::MigrationJob");
    qRegisterMetaType<sak::MigrationStatus>("sak::MigrationStatus");
    qRegisterMetaType<sak::AppInstallationWorker::Stats>("sak::AppInstallationWorker::Stats");

    // Connect to Chocolatey manager signals
    connect(m_chocoManager.get(),
            &ChocolateyManager::installStarted,
            this,
            &AppInstallationWorker::onInstallStarted);
    connect(m_chocoManager.get(),
            &ChocolateyManager::installSuccess,
            this,
            &AppInstallationWorker::onInstallSuccess);
    connect(m_chocoManager.get(),
            &ChocolateyManager::installFailed,
            this,
            &AppInstallationWorker::onInstallFailed);
    connect(m_chocoManager.get(),
            &ChocolateyManager::installRetrying,
            this,
            &AppInstallationWorker::onInstallRetrying);
}

AppInstallationWorker::~AppInstallationWorker() {
    // Ensure background thread is stopped before destruction
    cancel();
    if (m_processFuture.isRunning()) {
        m_processFuture.waitForFinished();
    }
}

int AppInstallationWorker::startMigration(std::shared_ptr<MigrationReport> report,
                                          int maxConcurrent) {
    if (!report) {
        sak::logError("[AppInstallationWorker] startMigration: report must not be null");
        return 0;
    }

    int totalJobs;
    {
        QMutexLocker locker(&m_mutex);

        if (m_running) {
            sak::logWarning("[AppInstallationWorker] Installation already running");
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
        const size_t entry_count = entries.size();
        m_jobs.reserve(entry_count / 2);

        for (size_t i = 0; i < entry_count; ++i) {
            const auto& entry = entries[i];

            if (entry.selected && !entry.choco_package.isEmpty() && entry.available) {
                MigrationJob job;
                job.entryIndex = static_cast<int>(i);
                job.appName = entry.app_name;
                job.packageId = entry.choco_package;
                job.version = (entry.version_lock && !entry.locked_version.isEmpty())
                                  ? entry.locked_version
                                  : QString();
                job.status = MigrationStatus::Queued;

                m_jobs.append(job);
                m_jobQueue.enqueue(m_jobs.size() - 1);
            }
        }

        totalJobs = m_jobs.size();

        // Launch background processing (will wait for mutex release)
        m_processFuture = QtConcurrent::run([this]() { processQueue(); });
    }
    // Mutex released — safe to emit (handlers may call getStats())
    Q_EMIT migrationStarted(totalJobs);

    return totalJobs;
}

void AppInstallationWorker::pause() {
    QMutexLocker locker(&m_mutex);

    if (!m_running || m_paused) {
        return;
    }

    m_paused = true;
    Q_EMIT migrationPaused();
}

void AppInstallationWorker::resume() {
    QMutexLocker locker(&m_mutex);

    if (!m_running || !m_paused) {
        return;
    }

    m_paused = false;

    // Wake up worker thread
    m_waitCondition.wakeAll();
    Q_EMIT migrationResumed();
}

void AppInstallationWorker::cancel() {
    QVector<MigrationJob> cancelled_jobs;
    {
        QMutexLocker locker(&m_mutex);

        if (!m_running) {
            return;
        }

        m_cancelled = true;
        m_paused = false;

        while (!m_jobQueue.isEmpty()) {
            int jobIndex = m_jobQueue.dequeue();
            m_jobs[jobIndex].status = MigrationStatus::Cancelled;

            if (m_report) {
                m_report->getEntry(m_jobs[jobIndex].entryIndex).status = "cancelled";
            }
            cancelled_jobs.append(m_jobs[jobIndex]);
        }

        m_waitCondition.wakeAll();
    }
    // Emit outside the lock — handlers may call getStats()
    for (const auto& job : cancelled_jobs) {
        Q_EMIT jobStatusChanged(job.entryIndex, job);
    }
    Q_EMIT migrationCancelled();
}

bool AppInstallationWorker::isRunning() const {
    QMutexLocker locker(&m_mutex);
    return m_running;
}

bool AppInstallationWorker::isPaused() const {
    QMutexLocker locker(&m_mutex);
    return m_paused;
}

AppInstallationWorker::Stats AppInstallationWorker::getStats() const {
    QMutexLocker locker(&m_mutex);

    Stats stats;
    stats.total = m_jobs.size();

    for (const auto& job : m_jobs) {
        switch (job.status) {
        case MigrationStatus::Pending:
            stats.pending++;
            break;
        case MigrationStatus::Queued:
            stats.queued++;
            break;
        case MigrationStatus::Installing:
            stats.installing++;
            break;
        case MigrationStatus::Success:
            stats.success++;
            break;
        case MigrationStatus::Failed:
            stats.failed++;
            break;
        case MigrationStatus::Skipped:
            stats.skipped++;
            break;
        case MigrationStatus::Cancelled:
            stats.cancelled++;
            break;
        }
    }

    return stats;
}

QVector<MigrationJob> AppInstallationWorker::getJobs() const {
    QMutexLocker locker(&m_mutex);
    return m_jobs;
}

void AppInstallationWorker::processQueue() {
    while (true) {
        auto action = checkQueueState();
        if (action == QueueAction::Finish) {
            return;
        }
        if (action == QueueAction::Wait) {
            continue;
        }

        int jobIndex;
        {
            QMutexLocker locker(&m_mutex);
            jobIndex = m_jobQueue.dequeue();
            m_activeJobs++;
        }

        MigrationJob& job = m_jobs[jobIndex];
        bool success = installPackage(job);

        QMutexLocker locker(&m_mutex);
        m_activeJobs--;
        if (!success && shouldRetry(job)) {
            int delay_ms = getRetryDelay(job.retryCount);
            locker.unlock();
            QThread::msleep(delay_ms);

            QMutexLocker retryLocker(&m_mutex);
            job.retryCount++;
            m_jobQueue.enqueue(jobIndex);
        }
    }
}

AppInstallationWorker::QueueAction AppInstallationWorker::checkQueueState() {
    QMutexLocker locker(&m_mutex);

    if (m_cancelled || (!m_running && m_activeJobs == 0)) {
        m_running = false;
        locker.unlock();
        Q_EMIT migrationCompleted(getStats());
        return QueueAction::Finish;
    }

    if (m_paused) {
        m_waitCondition.wait(&m_mutex);
        return QueueAction::Wait;
    }

    if (m_activeJobs == 0 && m_jobQueue.isEmpty()) {
        m_running = false;
        locker.unlock();
        Q_EMIT migrationCompleted(getStats());
        return QueueAction::Finish;
    }

    if (m_activeJobs >= m_maxConcurrent || m_jobQueue.isEmpty()) {
        locker.unlock();
        QThread::msleep(sak::kTimerPollingFastMs);
        return QueueAction::Wait;
    }

    return QueueAction::Proceed;
}

bool AppInstallationWorker::installPackage(MigrationJob& job) {
    // Check if a newer or equal version is already installed
    QString installed_version;
    if (isNewerVersionInstalled(job, installed_version)) {
        job.status = MigrationStatus::Skipped;
        job.startTime = QDateTime::currentDateTime();
        job.endTime = job.startTime;
        QString message = QString("Skipped %1 — newer version %2 already installed")
                              .arg(job.appName, installed_version);
        job.errorMessage = message;
        Q_EMIT jobProgress(job.entryIndex, message);
        Q_EMIT jobStatusChanged(job.entryIndex, job);
        sak::logInfo("[AppInstallationWorker] {}", message.toStdString());
        return true;
    }

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
    config.force = true;
    config.allow_unofficial = false;

    auto result = m_chocoManager->installPackage(config);
    bool success = result.success;
    bool verification_failed = false;

    // Verify installation via multi-source check
    if (success) {
        verification_failed = !verifyInstallation(job, result);
        if (verification_failed) {
            success = false;
        }
    }

    // Update status
    job.endTime = QDateTime::currentDateTime();

    if (success) {
        job.status = MigrationStatus::Success;
        Q_EMIT jobProgress(job.entryIndex, "Successfully installed " + job.packageId);
    } else {
        job.status = MigrationStatus::Failed;
        if (verification_failed) {
            job.errorMessage = "Installation reported success but could not be verified";
        } else if (result.error_message.isEmpty()) {
            job.errorMessage = "Installation failed";
        } else {
            job.errorMessage = result.error_message;
        }
        Q_EMIT jobProgress(job.entryIndex, "Failed to install " + job.packageId);
        sak::logWarning("[AppInstallationWorker] Failed: {} - {}",
                        job.packageId.toStdString(),
                        job.errorMessage.toStdString());
    }

    Q_EMIT jobStatusChanged(job.entryIndex, job);

    return success;
}

// ======================================================================
// Installation Verification
// ======================================================================

bool AppInstallationWorker::verifyInstallation(const MigrationJob& job,
                                               const ChocolateyManager::Result& choco_result) {
    // Primary: parse Chocolatey output for definitive package count
    // Choco prints "Chocolatey installed X/Y packages." on completion
    static const QRegularExpression kPackageCountPattern(
        QStringLiteral("Chocolatey installed (\\d+)/(\\d+) packages"));

    auto count_match = kPackageCountPattern.match(choco_result.output);
    if (count_match.hasMatch()) {
        int installed = count_match.captured(1).toInt();
        if (installed > 0) {
            return true;
        }
        sak::logWarning("[AppInstallationWorker] Choco reports 0 packages for {}",
                        job.packageId.toStdString());
        return false;
    }

    // Fallback: choco output didn't contain package count line
    // Check system directly across multiple sources
    sak::logInfo("[AppInstallationWorker] Checking system for {} ({})",
                 job.appName.toStdString(),
                 job.packageId.toStdString());

    // Check Windows Registry (covers MSI/EXE installers)
    AppScanner scanner;
    for (const auto& app : scanner.scanRegistry()) {
        if (app.name.contains(job.appName, Qt::CaseInsensitive)) {
            sak::logInfo("[AppInstallationWorker] Verified via registry: {}",
                         app.name.toStdString());
            return true;
        }
    }

    // Check AppX/MSIX packages (covers Store/UWP apps like Teams)
    for (const auto& app : AppScanner::scanAppX()) {
        if (app.name.contains(job.appName, Qt::CaseInsensitive) ||
            app.name.contains(job.packageId, Qt::CaseInsensitive)) {
            sak::logInfo("[AppInstallationWorker] Verified via AppX: {}", app.name.toStdString());
            return true;
        }
    }

    // Could not verify — trust choco exit code, warn in logs
    sak::logWarning("[AppInstallationWorker] Could not independently verify {} ({})",
                    job.appName.toStdString(),
                    job.packageId.toStdString());
    return true;
}

// ======================================================================
// Pre-Install Version Check
// ======================================================================

bool AppInstallationWorker::isNewerVersionInstalled(const MigrationJob& job,
                                                    QString& installed_version) {
    installed_version.clear();

    QVersionNumber requested = QVersionNumber::fromString(job.version);
    if (requested.isNull()) {
        return false;
    }

    AppScanner scanner;
    for (const auto& app : scanner.scanRegistry()) {
        if (app.name.contains(job.appName, Qt::CaseInsensitive) &&
            isVersionNewer(app.version, requested)) {
            installed_version = app.version;
            return true;
        }
    }

    for (const auto& app : AppScanner::scanAppX()) {
        bool name_match = app.name.contains(job.appName, Qt::CaseInsensitive) ||
                          app.name.contains(job.packageId, Qt::CaseInsensitive);
        if (name_match && isVersionNewer(app.version, requested)) {
            installed_version = app.version;
            return true;
        }
    }

    return false;
}

void AppInstallationWorker::updateJobStatus(int index,
                                            MigrationStatus status,
                                            const QString& error) {
    MigrationJob job_copy;
    {
        QMutexLocker locker(&m_mutex);

        if (index < 0 || index >= m_jobs.size()) {
            return;
        }

        m_jobs[index].status = status;
        if (!error.isEmpty()) {
            m_jobs[index].errorMessage = error;
        }
        job_copy = m_jobs[index];
    }
    Q_EMIT jobStatusChanged(job_copy.entryIndex, job_copy);
}

bool AppInstallationWorker::shouldRetry(const MigrationJob& job) const {
    return job.status == MigrationStatus::Failed && job.retryCount < kRetryCountDefault &&
           !m_cancelled;
}

int AppInstallationWorker::getRetryDelay(int retryCount) const {
    // Exponential backoff: 5s, 10s, 20s
    return kRetryBackoffSlowMs * (1 << retryCount);
}

void AppInstallationWorker::onInstallStarted(const QString& packageId) {
    Q_EMIT jobProgress(-1, "Starting installation of " + packageId);
}

void AppInstallationWorker::onInstallSuccess(const QString& packageId) {
    Q_EMIT jobProgress(-1, "Successfully installed " + packageId);
}

void AppInstallationWorker::onInstallFailed(const QString& packageId, const QString& error) {
    Q_EMIT jobProgress(-1, "Failed to install " + packageId + ": " + error);
}

void AppInstallationWorker::onInstallRetrying(const QString& packageId, int attempt) {
    Q_EMIT jobProgress(-1, QString("Retrying %1 (attempt %2)").arg(packageId).arg(attempt));
}

}  // namespace sak

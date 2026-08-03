// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/app_installation_worker.h"

#include "sak/app_scanner.h"
#include "sak/chocolatey_manager.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/migration_report.h"
#include "sak/nuget_version_range.h"
#include "sak/package_matcher.h"

#include <QMetaObject>
#include <QRegularExpression>
#include <QtConcurrent>
#include <QtGlobal>
#include <QThread>
#include <QTimer>

namespace sak {

namespace {
constexpr size_t kSelectedJobReserveDivisor = 2;
}

namespace {

// SemVer-aware "installed satisfies requested". QVersionNumber ignores the
// prerelease tag, so it would treat an installed "1.0.0-beta" as >= a requested
// stable "1.0.0" and WRONGLY skip the install. NuGetVersion honors prerelease
// precedence (1.0.0-beta < 1.0.0), so a prerelease never satisfies a stable
// request. An unparseable installed version is not treated as newer (fail-open
// would mean silently skipping a needed install).
bool isVersionNewer(const QString& version, const NuGetVersion& requested) {
    const auto installed = NuGetVersion::parse(version);
    return installed.has_value() && installed->compare(requested) >= 0;
}

ChocolateyManager::InstallConfig makeInstallConfig(const MigrationJob& job) {
    ChocolateyManager::InstallConfig config;
    config.package_name = job.packageId;
    config.version = job.version;
    config.version_locked = !job.version.isEmpty();
    config.auto_confirm = true;
    config.force = true;
    config.allow_unofficial = false;
    return config;
}

}  // namespace

AppInstallationWorker::AppInstallationWorker(std::shared_ptr<ChocolateyManager> chocoManager,
                                             QObject* parent)
    : QObject(parent), m_chocoManager(chocoManager) {
    // Register custom types for cross-thread signal delivery
    qRegisterMetaType<sak::MigrationJob>("sak::MigrationJob");
    qRegisterMetaType<sak::MigrationStatus>("sak::MigrationStatus");
    qRegisterMetaType<sak::AppInstallationWorker::Stats>("sak::AppInstallationWorker::Stats");

    // Fail closed on a null manager: skip the signal wiring here and refuse to
    // start migrations later (startMigration guards), so a null shared_ptr can
    // never be dereferenced at install time.
    if (!m_chocoManager) {
        sak::logError("[AppInstallationWorker] constructed with a null ChocolateyManager");
        return;
    }

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

int AppInstallationWorker::boundConcurrency(int requested) {
    return qBound(0, requested, kMaxInstallConcurrency);
}

int AppInstallationWorker::startMigration(std::shared_ptr<MigrationReport> report,
                                          int maxConcurrent) {
    if (!report) {
        sak::logError("[AppInstallationWorker] startMigration: report must not be null");
        return 0;
    }
    if (!m_chocoManager) {
        sak::logError("[AppInstallationWorker] startMigration: ChocolateyManager is null");
        return 0;
    }

    int totalJobs;
    QVector<QPair<int, QString>> skipped;
    {
        QMutexLocker locker(&m_mutex);

        if (m_running) {
            sak::logWarning("[AppInstallationWorker] Installation already running");
            return 0;
        }

        m_report = report;
        m_maxConcurrent = boundConcurrency(maxConcurrent);
        m_running = true;
        m_paused = false;
        m_cancelled = false;
        m_activeJobs = 0;

        skipped = buildJobQueue();
        totalJobs = m_jobs.size();

        // Launch background processing (will wait for mutex release)
        m_processFuture = QtConcurrent::run([this]() { processQueue(); });
    }
    // Mutex released — safe to emit (handlers may call getStats())
    for (const auto& item : skipped) {
        Q_EMIT jobProgress(item.first, QStringLiteral("Skipped: ") + item.second);
    }
    Q_EMIT migrationStarted(totalJobs);

    return totalJobs;
}

QVector<QPair<int, QString>> AppInstallationWorker::buildJobQueue() {
    QVector<QPair<int, QString>> skipped;
    m_jobs.clear();
    m_jobQueue.clear();

    const auto& entries = m_report->getEntries();
    const size_t entry_count = entries.size();
    m_jobs.reserve(entry_count / kSelectedJobReserveDivisor);

    for (size_t i = 0; i < entry_count; ++i) {
        const auto& entry = entries[i];
        if (!entry.selected) {
            continue;
        }
        const QString reason = migrationSkipReason(entry);
        if (!reason.isEmpty()) {
            // Record the skip explicitly instead of silently dropping a selected
            // entry, so the user can see it was not migrated and why.
            m_report->getEntry(static_cast<int>(i)).status = "skipped";
            m_report->getEntry(static_cast<int>(i)).error_message = reason;
            skipped.append({static_cast<int>(i), reason});
            continue;
        }
        m_jobs.append(makeJob(static_cast<int>(i), entry));
        m_jobQueue.enqueue(m_jobs.size() - 1);
    }
    return skipped;
}

QString AppInstallationWorker::migrationSkipReason(const MigrationReport::MigrationEntry& entry) {
    if (entry.choco_package.isEmpty()) {
        return QStringLiteral("No matched Chocolatey package");
    }
    if (!entry.available) {
        return QStringLiteral("Package not available in the configured feed");
    }
    if (entry.version_lock && entry.locked_version.isEmpty()) {
        // Fail closed: a lock with no version would silently install latest.
        return QStringLiteral("Version lock requested but no locked version specified");
    }
    return {};
}

MigrationJob AppInstallationWorker::makeJob(int entryIndex,
                                            const MigrationReport::MigrationEntry& entry) {
    MigrationJob job;
    job.entryIndex = entryIndex;
    job.appName = entry.app_name;
    job.packageId = entry.choco_package;
    // Skip-reason has already rejected a lock with an empty version, so a locked
    // job here always carries a concrete version; unlocked installs use latest.
    job.version = entry.version_lock ? entry.locked_version : QString();
    job.status = MigrationStatus::Queued;
    return job;
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

        MigrationJob job;
        {
            QMutexLocker locker(&m_mutex);
            if (jobIndex < 0 || jobIndex >= m_jobs.size()) {
                m_activeJobs--;
                continue;
            }
            job = m_jobs[jobIndex];
        }

        bool success = installPackage(jobIndex, job);
        handleJobResult(jobIndex, job, success);
    }
}

void AppInstallationWorker::handleJobResult(int jobIndex, MigrationJob& job, bool success) {
    QMutexLocker locker(&m_mutex);
    m_activeJobs--;
    if (success || !shouldRetry(job)) {
        return;
    }
    const int delay_ms = getRetryDelay(job.retryCount);
    locker.unlock();
    QThread::msleep(delay_ms);

    QMutexLocker retryLocker(&m_mutex);
    // A cancel() may have landed during the backoff sleep and already drained the
    // queue; do not resurrect a job onto a cancelled run.
    if (m_cancelled) {
        return;
    }
    job.retryCount++;
    if (jobIndex >= 0 && jobIndex < m_jobs.size()) {
        m_jobs[jobIndex] = job;
    }
    m_jobQueue.enqueue(jobIndex);
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

    // Finish when there is no active job AND either the queue is drained or no
    // job can EVER launch (maxConcurrent==0 dry-run) -- otherwise the loop below
    // would busy-poll forever waiting for a slot that never opens.
    if (m_activeJobs == 0 && (m_jobQueue.isEmpty() || m_maxConcurrent <= 0)) {
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

bool AppInstallationWorker::installPackage(int jobIndex, MigrationJob& job) {
    // Check if a newer or equal version is already installed
    QString installed_version;
    if (isNewerVersionInstalled(job, installed_version)) {
        job.status = MigrationStatus::Skipped;
        job.startTime = QDateTime::currentDateTime();
        job.endTime = job.startTime;
        QString message = QString("Skipped %1 — newer version %2 already installed")
                              .arg(job.appName, installed_version);
        job.errorMessage = message;
        storeJobSnapshot(jobIndex, job);
        Q_EMIT jobProgress(job.entryIndex, message);
        Q_EMIT jobStatusChanged(job.entryIndex, job);
        sak::logInfo("[AppInstallationWorker] {}", message.toStdString());
        return true;
    }

    // Update status to installing
    job.status = MigrationStatus::Installing;
    job.startTime = QDateTime::currentDateTime();
    storeJobSnapshot(jobIndex, job);
    Q_EMIT jobStatusChanged(job.entryIndex, job);
    Q_EMIT jobProgress(job.entryIndex, "Installing " + job.packageId + "...");

    auto result = m_chocoManager->installPackage(makeInstallConfig(job));
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

    storeJobSnapshot(jobIndex, job);
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
        if (installed <= 0) {
            sak::logWarning("[AppInstallationWorker] Choco reports 0 packages for {}",
                            job.packageId.toStdString());
            return false;
        }
        // installed>0 alone could reflect a DEPENDENCY succeeding while the target
        // failed. Only certify when the target package id also appears in the
        // transcript; otherwise fall through to independent system verification.
        if (choco_result.output.contains(job.packageId, Qt::CaseInsensitive)) {
            return true;
        }
        sak::logWarning("[AppInstallationWorker] Count line lacks target {}; verifying via system",
                        job.packageId.toStdString());
    }

    // Fallback: choco output didn't contain package count line
    // Check system directly across multiple sources
    sak::logInfo("[AppInstallationWorker] Checking system for {} ({})",
                 job.appName.toStdString(),
                 job.packageId.toStdString());

    // Check Windows Registry (covers MSI/EXE installers). Whole-word match so an
    // unrelated program whose name merely embeds the target token cannot certify.
    AppScanner scanner;
    for (const auto& app : scanner.scanRegistry()) {
        if (nameIndicatesApp(app.name, job.appName)) {
            sak::logInfo("[AppInstallationWorker] Verified via registry: {}",
                         app.name.toStdString());
            return true;
        }
    }

    // Check AppX/MSIX packages (covers Store/UWP apps like Teams)
    for (const auto& app : AppScanner::scanAppX()) {
        if (nameIndicatesApp(app.name, job.appName) || nameIndicatesApp(app.name, job.packageId)) {
            sak::logInfo("[AppInstallationWorker] Verified via AppX: {}", app.name.toStdString());
            return true;
        }
    }

    // No source confirmed the install: choco printed no "installed X/Y" count line
    // AND neither the registry nor AppX shows the app. Do NOT certify it installed
    // on the exit code alone -- require positive confirmation (the caller reports
    // "reported success but could not be verified" so the user can check).
    sak::logWarning("[AppInstallationWorker] Could not independently verify {} ({})",
                    job.appName.toStdString(),
                    job.packageId.toStdString());
    return false;
}

// ======================================================================
// Pre-Install Version Check
// ======================================================================

bool AppInstallationWorker::isNewerVersionInstalled(const MigrationJob& job,
                                                    QString& installed_version) {
    installed_version.clear();

    const auto requested = NuGetVersion::parse(job.version);
    if (!requested.has_value()) {
        return false;
    }

    AppScanner scanner;
    for (const auto& app : scanner.scanRegistry()) {
        if (nameIndicatesApp(app.name, job.appName) && isVersionNewer(app.version, *requested)) {
            installed_version = app.version;
            return true;
        }
    }

    for (const auto& app : AppScanner::scanAppX()) {
        bool name_match = nameIndicatesApp(app.name, job.appName) ||
                          nameIndicatesApp(app.name, job.packageId);
        if (name_match && isVersionNewer(app.version, *requested)) {
            installed_version = app.version;
            return true;
        }
    }

    return false;
}

bool AppInstallationWorker::nameIndicatesApp(const QString& candidate, const QString& target) {
    if (candidate.isEmpty() || target.isEmpty()) {
        return false;
    }
    // Whole-word, case-insensitive: forbid an adjacent alphanumeric on either side
    // so "Notepad" does not match "Notepadster", yet "Google Chrome" still matches
    // "Google Chrome (64-bit)" and a punctuation-tailed id like "Notepad++" still
    // matches. Using alnum lookarounds (not \b) keeps targets that END in a
    // non-word char (++) working, where \b would spuriously fail.
    const QString pattern = QStringLiteral("(?<![A-Za-z0-9])") +
                            QRegularExpression::escape(target) + QStringLiteral("(?![A-Za-z0-9])");
    const QRegularExpression re(pattern, QRegularExpression::CaseInsensitiveOption);
    return re.match(candidate).hasMatch();
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

void AppInstallationWorker::storeJobSnapshot(int index, const MigrationJob& job) {
    QMutexLocker locker(&m_mutex);

    if (index < 0 || index >= m_jobs.size()) {
        return;
    }

    m_jobs[index] = job;
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

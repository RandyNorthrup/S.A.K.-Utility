#include "sak/parallel_transfer_manager.h"

#include <QUuid>
#include <QTimer>

namespace sak {

ParallelTransferManager::ParallelTransferManager(QObject* parent)
    : QObject(parent) {
    m_retryTimer = new QTimer(this);
    m_retryTimer->setInterval(500);
    connect(m_retryTimer, &QTimer::timeout, this, [this]() {
        startNextJobs();
    });
}

void ParallelTransferManager::startDeployment(const MappingEngine::DeploymentMapping& mapping) {
    reset();
    m_currentDeploymentId = mapping.deployment_id.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces)
                                                            : mapping.deployment_id;

    if (mapping.type == MappingEngine::MappingType::OneToMany) {
        if (mapping.sources.isEmpty()) {
            return;
        }
        for (const auto& destination : mapping.destinations) {
            enqueueJob(mapping.sources.first(), destination);
        }
    } else if (mapping.type == MappingEngine::MappingType::ManyToMany) {
        const int count = qMin(mapping.sources.size(), mapping.destinations.size());
        for (int i = 0; i < count; ++i) {
            enqueueJob(mapping.sources[i], mapping.destinations[i]);
        }
    } else {
        QMap<QString, DestinationPC> destinationMap;
        for (const auto& destination : mapping.destinations) {
            destinationMap.insert(destination.destination_id, destination);
        }
        for (const auto& source : mapping.sources) {
            const auto destinationId = mapping.custom_rules.value(source.username);
            if (!destinationId.isEmpty() && destinationMap.contains(destinationId)) {
                enqueueJob(source, destinationMap.value(destinationId));
            }
        }
    }

    Q_EMIT deploymentStarted(m_currentDeploymentId);
    startNextJobs();
}

void ParallelTransferManager::pauseDeployment() {
    m_deploymentPaused = true;
    for (const auto& jobId : m_activeJobs) {
        if (m_jobs.contains(jobId)) {
            m_jobs[jobId].status = "paused";
            Q_EMIT jobPauseRequested(jobId);
        }
    }
    updateRetryTimer();
}

void ParallelTransferManager::resumeDeployment() {
    m_deploymentPaused = false;
    for (const auto& jobId : m_activeJobs) {
        if (m_jobs.contains(jobId)) {
            m_jobs[jobId].status = "transferring";
            Q_EMIT jobResumeRequested(jobId);
        }
    }
    startNextJobs();
    rebalanceBandwidth();
}

void ParallelTransferManager::cancelDeployment() {
    for (const auto& jobId : m_activeJobs) {
        Q_EMIT jobCancelRequested(jobId);
    }

    for (const auto& jobId : m_queue) {
        if (m_jobs.contains(jobId)) {
            m_jobs[jobId].status = "canceled";
        }
    }

    m_activeJobs.clear();
    m_queue.clear();
    m_retrySchedule.clear();
    updateRetryTimer();
    updateDeploymentProgress();
    Q_EMIT deploymentComplete(m_currentDeploymentId, false);
}

void ParallelTransferManager::pauseJob(const QString& job_id) {
    if (m_jobs.contains(job_id)) {
        m_jobs[job_id].status = "paused";
        Q_EMIT jobPauseRequested(job_id);
    }
}

void ParallelTransferManager::resumeJob(const QString& job_id) {
    if (m_jobs.contains(job_id)) {
        m_jobs[job_id].status = "transferring";
        Q_EMIT jobResumeRequested(job_id);
    }
    startNextJobs();
}

void ParallelTransferManager::retryJob(const QString& job_id) {
    if (!m_jobs.contains(job_id)) {
        return;
    }

    auto& job = m_jobs[job_id];
    job.retry_count++;
    job.status = "retry_scheduled";
    job.error_message.clear();
    job.bytes_transferred = 0;
    job.updated_at = QDateTime::currentDateTimeUtc();

    m_failedJobs.remove(job_id);
    m_activeJobs.remove(job_id);

    const int delay = qMin(m_retryBaseMs * (1 << qMin(job.retry_count - 1, 6)), m_retryMaxMs);
    m_retrySchedule.insert(job_id, QDateTime::currentDateTimeUtc().addMSecs(delay));

    if (!m_queue.contains(job_id)) {
        m_queue.append(job_id);
    }

    updateRetryTimer();
}

void ParallelTransferManager::cancelJob(const QString& job_id) {
    if (!m_jobs.contains(job_id)) {
        return;
    }

    auto& job = m_jobs[job_id];
    job.status = "canceled";
    job.updated_at = QDateTime::currentDateTimeUtc();

    m_queue.removeAll(job_id);
    m_activeJobs.remove(job_id);
    m_failedJobs.insert(job_id);
    m_retrySchedule.remove(job_id);

    Q_EMIT jobCancelRequested(job_id);
    Q_EMIT jobCompleted(job_id, false, tr("Canceled"));
    updateDeploymentProgress();

    if (isDeploymentComplete()) {
        Q_EMIT deploymentComplete(m_currentDeploymentId, false);
    } else {
        startNextJobs();
    }
}

void ParallelTransferManager::setJobPriority(const QString& job_id, JobPriority priority) {
    if (!m_jobs.contains(job_id)) {
        return;
    }
    m_jobs[job_id].priority = priority;
    startNextJobs();
}

void ParallelTransferManager::setDefaultPriority(JobPriority priority) {
    m_defaultPriority = priority;
}

void ParallelTransferManager::setRetryBackoff(int base_ms, int max_ms) {
    m_retryBaseMs = qMax(100, base_ms);
    m_retryMaxMs = qMax(m_retryBaseMs, max_ms);
}

QVector<ParallelTransferManager::TransferJob> ParallelTransferManager::getActiveJobs() const {
    QVector<TransferJob> jobs;
    jobs.reserve(m_activeJobs.size());
    for (const auto& jobId : m_activeJobs) {
        if (m_jobs.contains(jobId)) {
            jobs.push_back(m_jobs.value(jobId));
        }
    }
    return jobs;
}

QVector<ParallelTransferManager::TransferJob> ParallelTransferManager::allJobs() const {
    return m_jobs.values().toVector();
}

ParallelTransferManager::TransferJob ParallelTransferManager::getJobStatus(const QString& job_id) const {
    return m_jobs.value(job_id);
}

int ParallelTransferManager::totalJobs() const {
    return m_jobs.size();
}

int ParallelTransferManager::completedJobs() const {
    return m_completedJobs.size();
}

int ParallelTransferManager::failedJobs() const {
    return m_failedJobs.size();
}

void ParallelTransferManager::setMaxConcurrentTransfers(int count) {
    m_maxConcurrent = qMax(1, count);
    startNextJobs();
}

void ParallelTransferManager::setGlobalBandwidthLimit(int mbps) {
    m_globalBandwidthLimitMbps = qMax(0, mbps);
}

void ParallelTransferManager::setPerJobBandwidthLimit(int mbps) {
    m_perJobBandwidthLimitMbps = qMax(0, mbps);
}

void ParallelTransferManager::updateJobProgress(const QString& job_id,
                                                int progress_percent,
                                                qint64 bytes_transferred,
                                                qint64 total_bytes,
                                                double speed_mbps,
                                                const QString& current_file) {
    if (!m_jobs.contains(job_id)) {
        return;
    }

    auto& job = m_jobs[job_id];
    job.status = "transferring";
    job.bytes_transferred = bytes_transferred;
    job.total_bytes = total_bytes;
    job.speed_mbps = speed_mbps;
    job.updated_at = QDateTime::currentDateTimeUtc();

    Q_UNUSED(current_file);

    Q_EMIT jobUpdated(job_id, progress_percent);
    updateDeploymentProgress();
}

void ParallelTransferManager::markJobComplete(const QString& job_id, bool success, const QString& error_message) {
    if (!m_jobs.contains(job_id)) {
        return;
    }

    auto& job = m_jobs[job_id];
    job.status = success ? "complete" : "failed";
    job.error_message = error_message;
    job.updated_at = QDateTime::currentDateTimeUtc();

    m_activeJobs.remove(job_id);
    m_retrySchedule.remove(job_id);
    if (success) {
        m_completedJobs.insert(job_id);
    } else {
        m_failedJobs.insert(job_id);
    }

    Q_EMIT jobCompleted(job_id, success, error_message);
    updateDeploymentProgress();

    if (isDeploymentComplete()) {
        Q_EMIT deploymentComplete(m_currentDeploymentId, m_failedJobs.isEmpty());
    } else {
        startNextJobs();
        rebalanceBandwidth();
    }
}

void ParallelTransferManager::reset() {
    m_jobs.clear();
    m_queue.clear();
    m_activeJobs.clear();
    m_completedJobs.clear();
    m_failedJobs.clear();
    m_retrySchedule.clear();
    m_deploymentPaused = false;
}

void ParallelTransferManager::enqueueJob(const MappingEngine::SourceProfile& source, const DestinationPC& destination) {
    TransferJob job;
    job.job_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job.source = source;
    job.destination = destination;
    job.total_bytes = source.profile_size_bytes;
    job.status = "queued";
    job.priority = m_defaultPriority;
    job.updated_at = QDateTime::currentDateTimeUtc();

    m_jobs.insert(job.job_id, job);
    m_queue.append(job.job_id);
}

void ParallelTransferManager::startNextJobs() {
    if (m_deploymentPaused) {
        return;
    }

    while (!m_queue.isEmpty() && m_activeJobs.size() < m_maxConcurrent) {
        const auto now = QDateTime::currentDateTimeUtc();
        int bestIndex = -1;
        int bestScore = -1;

        for (int i = 0; i < m_queue.size(); ++i) {
            const auto& jobId = m_queue.at(i);
            if (!m_jobs.contains(jobId)) {
                continue;
            }

            const auto& job = m_jobs.value(jobId);
            if (job.status == "canceled") {
                continue;
            }

            if (m_retrySchedule.contains(jobId) && m_retrySchedule.value(jobId) > now) {
                continue;
            }

            const int score = priorityScore(job.priority);
            if (score > bestScore) {
                bestScore = score;
                bestIndex = i;
            }
        }

        if (bestIndex < 0) {
            updateRetryTimer();
            break;
        }

        const auto jobId = m_queue.takeAt(bestIndex);
        if (!m_jobs.contains(jobId)) {
            continue;
        }

        auto& job = m_jobs[jobId];
        if (job.status == "canceled") {
            continue;
        }

        job.status = "transferring";
        job.started_at = QDateTime::currentDateTimeUtc();
        job.updated_at = job.started_at;
        m_activeJobs.insert(jobId);

        Q_EMIT jobStartRequested(jobId, job.source, job.destination);
        Q_EMIT jobStarted(jobId);
    }

    rebalanceBandwidth();
}

void ParallelTransferManager::deferredStartNextJobs() {
    startNextJobs();
    rebalanceBandwidth();
}

void ParallelTransferManager::updateDeploymentProgress() {
    const int totalJobs = m_jobs.size();
    const int completedJobs = m_completedJobs.size();
    Q_EMIT deploymentProgress(completedJobs, totalJobs);
}

bool ParallelTransferManager::isDeploymentComplete() const {
    const int totalJobs = m_jobs.size();
    return totalJobs > 0 && (m_completedJobs.size() + m_failedJobs.size()) >= totalJobs;
}

void ParallelTransferManager::updateRetryTimer() {
    if (m_retrySchedule.isEmpty()) {
        m_retryTimer->stop();
        return;
    }

    QDateTime next = QDateTime::currentDateTimeUtc().addDays(365);
    for (auto it = m_retrySchedule.constBegin(); it != m_retrySchedule.constEnd(); ++it) {
        if (it.value() < next) {
            next = it.value();
        }
    }

    const auto now = QDateTime::currentDateTimeUtc();
    const int delay = qMax(0, static_cast<int>(now.msecsTo(next)));
    m_retryTimer->setInterval(qMax(100, delay));
    if (!m_retryTimer->isActive()) {
        m_retryTimer->start();
    }
}

int ParallelTransferManager::priorityScore(JobPriority priority) const {
    return static_cast<int>(priority);
}

void ParallelTransferManager::rebalanceBandwidth() {
    if (m_globalBandwidthLimitMbps <= 0 || m_activeJobs.isEmpty()) {
        return;
    }

    const int activeCount = m_activeJobs.size();
    if (activeCount <= 0) {
        return;
    }

    const int totalKbps = m_globalBandwidthLimitMbps * 1024;
    const int perJobCapKbps = m_perJobBandwidthLimitMbps > 0
        ? m_perJobBandwidthLimitMbps * 1024
        : totalKbps;

    struct Allocation {
        QString jobId;
        int weight{1};
        int cap{0};
        int assigned{0};
    };

    QVector<Allocation> allocations;
    allocations.reserve(activeCount);

    int totalWeight = 0;
    for (const auto& jobId : m_activeJobs) {
        const auto job = m_jobs.value(jobId);
        const int weight = qMax(1, priorityScore(job.priority) + 1);
        allocations.push_back({jobId, weight, perJobCapKbps, 0});
        totalWeight += weight;
    }

    if (totalWeight <= 0) {
        totalWeight = activeCount;
        for (auto& alloc : allocations) {
            alloc.weight = 1;
        }
    }

    int assignedTotal = 0;
    for (auto& alloc : allocations) {
        const int desired = qMax(1, static_cast<int>((static_cast<long long>(totalKbps) * alloc.weight) / totalWeight));
        alloc.assigned = qMin(alloc.cap, desired);
        assignedTotal += alloc.assigned;
    }

    int remaining = totalKbps - assignedTotal;
    if (remaining > 0) {
        int iterations = 0;
        while (remaining > 0 && iterations < 1000) {
            int weightSum = 0;
            for (const auto& alloc : allocations) {
                if (alloc.assigned < alloc.cap) {
                    weightSum += alloc.weight;
                }
            }

            if (weightSum <= 0) {
                break;
            }

            bool progress = false;
            for (auto& alloc : allocations) {
                if (alloc.assigned >= alloc.cap) {
                    continue;
                }

                const int slice = qMax(1, static_cast<int>((static_cast<long long>(remaining) * alloc.weight) / weightSum));
                const int delta = qMin(slice, alloc.cap - alloc.assigned);
                if (delta > 0) {
                    alloc.assigned += delta;
                    remaining -= delta;
                    progress = true;
                    if (remaining <= 0) {
                        break;
                    }
                }
            }

            if (!progress) {
                break;
            }
            ++iterations;
        }
    }

    for (const auto& alloc : allocations) {
        Q_EMIT jobBandwidthUpdateRequested(alloc.jobId, alloc.assigned);
    }
}

} // namespace sak

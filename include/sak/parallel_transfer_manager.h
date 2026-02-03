#pragma once

#include <QObject>
#include <QDateTime>
#include <QMap>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QVector>

#include "sak/mapping_engine.h"

namespace sak {

class ParallelTransferManager : public QObject {
    Q_OBJECT

public:
    enum class JobPriority {
        Low = 0,
        Normal = 1,
        High = 2,
        Critical = 3
    };

    struct TransferJob {
        QString job_id;
        MappingEngine::SourceProfile source;
        DestinationPC destination;
        qint64 bytes_transferred{0};
        qint64 total_bytes{0};
        double speed_mbps{0.0};
        QString status{"queued"};
        int retry_count{0};
        JobPriority priority{JobPriority::Normal};
        QString error_message;
        QDateTime started_at;
        QDateTime updated_at;
    };

    explicit ParallelTransferManager(QObject* parent = nullptr);

    void startDeployment(const MappingEngine::DeploymentMapping& mapping);
    void pauseDeployment();
    void resumeDeployment();
    void cancelDeployment();

    void pauseJob(const QString& job_id);
    void resumeJob(const QString& job_id);
    void retryJob(const QString& job_id);
    void cancelJob(const QString& job_id);
    void setJobPriority(const QString& job_id, JobPriority priority);
    void setDefaultPriority(JobPriority priority);
    void setRetryBackoff(int base_ms, int max_ms);

    QVector<TransferJob> getActiveJobs() const;
    QVector<TransferJob> allJobs() const;
    TransferJob getJobStatus(const QString& job_id) const;
    int totalJobs() const;
    int completedJobs() const;
    int failedJobs() const;

    void setMaxConcurrentTransfers(int count);
    void setGlobalBandwidthLimit(int mbps);
    void setPerJobBandwidthLimit(int mbps);

    void updateJobProgress(const QString& job_id,
                           int progress_percent,
                           qint64 bytes_transferred,
                           qint64 total_bytes,
                           double speed_mbps,
                           const QString& current_file);
    void markJobComplete(const QString& job_id, bool success, const QString& error_message = QString());

Q_SIGNALS:
    void deploymentStarted(const QString& deployment_id);
    void deploymentProgress(int completedJobs, int totalJobs);
    void deploymentComplete(const QString& deployment_id, bool success);

    void jobStartRequested(const QString& job_id,
                           const MappingEngine::SourceProfile& source,
                           const DestinationPC& destination);
    void jobBandwidthUpdateRequested(const QString& job_id, int max_bandwidth_kbps);
    void jobPauseRequested(const QString& job_id);
    void jobResumeRequested(const QString& job_id);
    void jobCancelRequested(const QString& job_id);

    void jobStarted(const QString& job_id);
    void jobUpdated(const QString& job_id, int progress_percent);
    void jobCompleted(const QString& job_id, bool success, const QString& error_message);

private:
    void reset();
    void enqueueJob(const MappingEngine::SourceProfile& source, const DestinationPC& destination);
    void startNextJobs();
    void updateDeploymentProgress();
    bool isDeploymentComplete() const;
    void updateRetryTimer();
    int priorityScore(JobPriority priority) const;
    void rebalanceBandwidth();

    QString m_currentDeploymentId;
    bool m_deploymentPaused{false};

    QMap<QString, TransferJob> m_jobs;
    QList<QString> m_queue;
    QSet<QString> m_activeJobs;
    QSet<QString> m_completedJobs;
    QSet<QString> m_failedJobs;
    QMap<QString, QDateTime> m_retrySchedule;
    QTimer* m_retryTimer{nullptr};

    int m_maxConcurrent{10};
    int m_globalBandwidthLimitMbps{0};
    int m_perJobBandwidthLimitMbps{0};
    int m_retryBaseMs{2000};
    int m_retryMaxMs{60000};
    JobPriority m_defaultPriority{JobPriority::Normal};
};

} // namespace sak

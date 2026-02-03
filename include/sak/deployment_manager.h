#pragma once

#include <QObject>
#include <QQueue>

#include <functional>

#include "sak/orchestration_types.h"

namespace sak {

class DeploymentManager : public QObject {
    Q_OBJECT

public:
    explicit DeploymentManager(QObject* parent = nullptr);

    void enqueue(const DeploymentAssignment& assignment);
    void enqueueForDestination(const DeploymentAssignment& assignment,
                               const QString& destination_id,
                               qint64 required_free_bytes);
    bool hasPending() const;
    bool peek(DeploymentAssignment* assignment) const;
    DeploymentAssignment dequeue();
    int pendingCount() const;

Q_SIGNALS:
    void deploymentQueued(const DeploymentAssignment& assignment);
    void deploymentDequeued(const DeploymentAssignment& assignment);
    void deploymentRejected(const QString& destination_id, const QString& reason);

public:
    using readiness_check = std::function<bool(const QString& destination_id,
                                               qint64 required_free_bytes,
                                               QString* reason)>;
    void setReadinessCheck(readiness_check checker);

private:
    QQueue<DeploymentAssignment> m_queue;
    readiness_check m_readinessCheck;
};

} // namespace sak

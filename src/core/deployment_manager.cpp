#include "sak/deployment_manager.h"

namespace sak {

DeploymentManager::DeploymentManager(QObject* parent)
    : QObject(parent) {
}

void DeploymentManager::enqueue(const DeploymentAssignment& assignment) {
    m_queue.enqueue(assignment);
    Q_EMIT deploymentQueued(assignment);
}

void DeploymentManager::enqueueForDestination(const DeploymentAssignment& assignment,
                                              const QString& destination_id,
                                              qint64 required_free_bytes) {
    if (m_readinessCheck) {
        QString reason;
        if (!m_readinessCheck(destination_id, required_free_bytes, &reason)) {
            Q_EMIT deploymentRejected(destination_id, reason);
            return;
        }
    }

    enqueue(assignment);
}

bool DeploymentManager::hasPending() const {
    return !m_queue.isEmpty();
}

bool DeploymentManager::peek(DeploymentAssignment* assignment) const {
    if (!assignment || m_queue.isEmpty()) {
        return false;
    }

    *assignment = m_queue.head();
    return true;
}

DeploymentAssignment DeploymentManager::dequeue() {
    if (m_queue.isEmpty()) {
        return {};
    }
    auto assignment = m_queue.dequeue();
    Q_EMIT deploymentDequeued(assignment);
    return assignment;
}

int DeploymentManager::pendingCount() const {
    return m_queue.size();
}

void DeploymentManager::setReadinessCheck(readiness_check checker) {
    m_readinessCheck = std::move(checker);
}

} // namespace sak

#include "sak/migration_orchestrator.h"
#include "sak/orchestration_server.h"
#include "sak/orchestration_discovery_service.h"

namespace sak {

MigrationOrchestrator::MigrationOrchestrator(QObject* parent)
    : QObject(parent)
    , m_registry(new DestinationRegistry(this))
    , m_deploymentManager(new DeploymentManager(this))
    , m_server(new OrchestrationServer(this))
    , m_discovery(new OrchestrationDiscoveryService(this))
    , m_healthPollTimer(new QTimer(this))
{
    connect(m_registry, &DestinationRegistry::destinationRegistered, this, [this](const DestinationPC&) {
        tryAssignQueuedDeployments();
    });
    connect(m_registry, &DestinationRegistry::destinationUpdated, this, [this](const DestinationPC&) {
        tryAssignQueuedDeployments();
    });
    connect(m_registry, &DestinationRegistry::destinationRemoved, this, [this](const QString& destination_id) {
        m_activeDestinations.remove(destination_id);
        tryAssignQueuedDeployments();
    });

    connect(m_deploymentManager, &DeploymentManager::deploymentQueued, this, [this](const DeploymentAssignment& assignment) {
        Q_EMIT orchestratorStatus(tr("Deployment queued: %1").arg(assignment.deployment_id));
        Q_EMIT deploymentReady(assignment);
        tryAssignQueuedDeployments();
    });

    connect(m_deploymentManager, &DeploymentManager::deploymentRejected, this, [this](const QString& destination_id, const QString& reason) {
        Q_EMIT orchestratorStatus(tr("Deployment rejected for %1: %2").arg(destination_id, reason));
        Q_EMIT deploymentRejected(destination_id, reason);
    });

    m_deploymentManager->setReadinessCheck([this](const QString& destination_id,
                                                  qint64 required_free_bytes,
                                                  QString* reason) {
        return canAssignDeployment(destination_id, required_free_bytes, reason);
    });

    connect(m_server, &OrchestrationServerInterface::destinationRegistered, this, &MigrationOrchestrator::registerDestination);
    connect(m_server, &OrchestrationServerInterface::healthUpdated, this, &MigrationOrchestrator::updateHealth);
    connect(m_server, &OrchestrationServerInterface::statusMessage, this, &MigrationOrchestrator::orchestratorStatus);
    connect(m_server, &OrchestrationServerInterface::progressUpdated, this, [this](const DeploymentProgress& progress) {
        if (!progress.destination_id.isEmpty()) {
            m_progressByDestination.insert(progress.destination_id, progress);
        }
        Q_EMIT deploymentProgress(progress);

        const auto destinations = m_registry->destinations();
        const int total = destinations.size();
        if (total > 0) {
            int sum = 0;
            for (const auto& destination : destinations) {
                if (m_progressByDestination.contains(destination.destination_id)) {
                    sum += m_progressByDestination.value(destination.destination_id).progress_percent;
                }
            }
            const int percent = sum / total;
            Q_EMIT aggregateProgress(m_completedDestinations.size(), total, percent);
        }
    });
    connect(m_server, &OrchestrationServerInterface::deploymentCompleted, this, [this](const DeploymentCompletion& completion) {
        if (!completion.destination_id.isEmpty()) {
            m_completedDestinations.insert(completion.destination_id);
            m_activeDestinations.remove(completion.destination_id);
            auto progress = m_progressByDestination.value(completion.destination_id);
            progress.progress_percent = 100;
            m_progressByDestination.insert(completion.destination_id, progress);
        }
        Q_EMIT deploymentCompleted(completion);

        const auto destinations = m_registry->destinations();
        const int total = destinations.size();
        if (total > 0) {
            int sum = 0;
            for (const auto& destination : destinations) {
                if (m_progressByDestination.contains(destination.destination_id)) {
                    sum += m_progressByDestination.value(destination.destination_id).progress_percent;
                }
            }
            const int percent = sum / total;
            Q_EMIT aggregateProgress(m_completedDestinations.size(), total, percent);
        }

        if (!completion.destination_id.isEmpty()) {
            handleAssignmentCompletion(completion.destination_id);
        }
        tryAssignQueuedDeployments();
    });

    connect(m_discovery, &OrchestrationDiscoveryService::destinationDiscovered, this,
        [this](const DestinationPC& destination) {
            registerDestination(destination);
        });

    m_healthPollTimer->setInterval(10000);
    connect(m_healthPollTimer, &QTimer::timeout, this, [this]() {
        const auto items = m_registry->destinations();
        for (const auto& destination : items) {
            if (!destination.destination_id.isEmpty()) {
                m_server->sendHealthCheck(destination.destination_id);
            }
        }
    });
}

DestinationRegistry* MigrationOrchestrator::registry() const {
    return m_registry;
}

DeploymentManager* MigrationOrchestrator::deploymentManager() const {
    return m_deploymentManager;
}

OrchestrationServerInterface* MigrationOrchestrator::server() const {
    return m_server;
}

void MigrationOrchestrator::setServer(OrchestrationServerInterface* server) {
    if (!server || server == m_server) {
        return;
    }

    if (m_server) {
        m_server->disconnect(this);
    }

    m_server = server;
    connect(m_server, &OrchestrationServerInterface::destinationRegistered, this, &MigrationOrchestrator::registerDestination);
    connect(m_server, &OrchestrationServerInterface::healthUpdated, this, &MigrationOrchestrator::updateHealth);
    connect(m_server, &OrchestrationServerInterface::statusMessage, this, &MigrationOrchestrator::orchestratorStatus);
    connect(m_server, &OrchestrationServerInterface::progressUpdated, this, [this](const DeploymentProgress& progress) {
        if (!progress.destination_id.isEmpty()) {
            m_progressByDestination.insert(progress.destination_id, progress);
        }
        Q_EMIT deploymentProgress(progress);

        const auto destinations = m_registry->destinations();
        const int total = destinations.size();
        if (total > 0) {
            int sum = 0;
            for (const auto& destination : destinations) {
                if (m_progressByDestination.contains(destination.destination_id)) {
                    sum += m_progressByDestination.value(destination.destination_id).progress_percent;
                }
            }
            const int percent = sum / total;
            Q_EMIT aggregateProgress(m_completedDestinations.size(), total, percent);
        }
    });
    connect(m_server, &OrchestrationServerInterface::deploymentCompleted, this, [this](const DeploymentCompletion& completion) {
        if (!completion.destination_id.isEmpty()) {
            m_completedDestinations.insert(completion.destination_id);
            m_activeDestinations.remove(completion.destination_id);
            auto progress = m_progressByDestination.value(completion.destination_id);
            progress.progress_percent = 100;
            m_progressByDestination.insert(completion.destination_id, progress);
        }
        Q_EMIT deploymentCompleted(completion);

        const auto destinations = m_registry->destinations();
        const int total = destinations.size();
        if (total > 0) {
            int sum = 0;
            for (const auto& destination : destinations) {
                if (m_progressByDestination.contains(destination.destination_id)) {
                    sum += m_progressByDestination.value(destination.destination_id).progress_percent;
                }
            }
            const int percent = sum / total;
            Q_EMIT aggregateProgress(m_completedDestinations.size(), total, percent);
        }

        if (!completion.destination_id.isEmpty()) {
            handleAssignmentCompletion(completion.destination_id);
        }
        tryAssignQueuedDeployments();
    });

    tryAssignQueuedDeployments();
}

bool MigrationOrchestrator::startServer(quint16 port) {
    if (m_server->start(port)) {
        m_listenPort = port;
        return true;
    }
    return false;
}

void MigrationOrchestrator::stopServer() {
    m_server->stop();
    m_listenPort = 0;
}

void MigrationOrchestrator::requestHealthCheck(const QString& destination_id) {
    m_server->sendHealthCheck(destination_id);
}

void MigrationOrchestrator::startDiscovery(quint16 port) {
    if (!m_discovery) {
        return;
    }

    m_discovery->setPort(port);
    if (m_listenPort != 0) {
        m_discovery->setOrchestratorPort(m_listenPort);
    }
    m_discovery->startAsOrchestrator();
}

void MigrationOrchestrator::stopDiscovery() {
    if (m_discovery) {
        m_discovery->stop();
    }
}

void MigrationOrchestrator::startHealthPolling(int interval_ms) {
    m_healthPollTimer->setInterval(interval_ms);
    if (!m_healthPollTimer->isActive()) {
        m_healthPollTimer->start();
    }
}

void MigrationOrchestrator::stopHealthPolling() {
    if (m_healthPollTimer->isActive()) {
        m_healthPollTimer->stop();
    }
}

void MigrationOrchestrator::registerDestination(const DestinationPC& destination) {
    m_registry->registerDestination(destination);
    Q_EMIT orchestratorStatus(tr("Destination registered: %1").arg(destination.hostname));
}

void MigrationOrchestrator::updateHealth(const QString& destination_id, const DestinationHealth& health) {
    m_registry->updateHealth(destination_id, health);
}

void MigrationOrchestrator::queueDeployment(const DeploymentAssignment& assignment) {
    m_deploymentManager->enqueue(assignment);
}

void MigrationOrchestrator::enableAutoAssignment(bool enabled) {
    m_autoAssignmentEnabled = enabled;
    if (m_autoAssignmentEnabled) {
        tryAssignQueuedDeployments();
    }
}

bool MigrationOrchestrator::autoAssignmentEnabled() const {
    return m_autoAssignmentEnabled;
}

void MigrationOrchestrator::setMappingStrategy(MappingEngine::Strategy strategy) {
    m_mappingEngine.setStrategy(strategy);
    tryAssignQueuedDeployments();
}

MappingEngine::Strategy MigrationOrchestrator::mappingStrategy() const {
    return m_mappingEngine.strategy();
}

void MigrationOrchestrator::assignDeploymentToDestination(const QString& destination_id,
                                                         const DeploymentAssignment& assignment,
                                                         qint64 required_free_bytes) {
    QString reason;
    if (!canAssignDeployment(destination_id, required_free_bytes, &reason)) {
        Q_EMIT orchestratorStatus(tr("Deployment rejected for %1: %2").arg(destination_id, reason));
        Q_EMIT deploymentRejected(destination_id, reason);
        return;
    }

    if (m_activeDestinations.contains(destination_id)) {
        m_pendingAssignments[destination_id].enqueue(assignment);
        Q_EMIT orchestratorStatus(tr("Deployment queued for %1: %2")
                                    .arg(destination_id, assignment.deployment_id));
        return;
    }

    if (dispatchAssignment(destination_id, assignment)) {
        Q_EMIT orchestratorStatus(tr("Deployment assigned: %1 -> %2")
                                    .arg(assignment.deployment_id, destination_id));
    }
}

void MigrationOrchestrator::pauseAssignment(const QString& destination_id,
                                            const QString& deployment_id,
                                            const QString& job_id) {
    if (!m_server) {
        return;
    }
    m_server->sendAssignmentPause(destination_id, deployment_id, job_id);
    Q_EMIT orchestratorStatus(tr("Pause requested: %1").arg(job_id));
}

void MigrationOrchestrator::resumeAssignment(const QString& destination_id,
                                             const QString& deployment_id,
                                             const QString& job_id) {
    if (!m_server) {
        return;
    }
    m_server->sendAssignmentResume(destination_id, deployment_id, job_id);
    Q_EMIT orchestratorStatus(tr("Resume requested: %1").arg(job_id));
}

void MigrationOrchestrator::cancelAssignment(const QString& destination_id,
                                             const QString& deployment_id,
                                             const QString& job_id) {
    if (!m_server) {
        return;
    }
    m_server->sendAssignmentCancel(destination_id, deployment_id, job_id);
    Q_EMIT orchestratorStatus(tr("Cancel requested: %1").arg(job_id));
}

bool MigrationOrchestrator::canAssignDeployment(const QString& destination_id,
                                                qint64 required_free_bytes,
                                                QString* reason) const {
    const auto destinations = m_registry->destinations();
    for (const auto& destination : destinations) {
        if (destination.destination_id == destination_id) {
            return DestinationRegistry::checkReadiness(destination, required_free_bytes, reason);
        }
    }

    if (reason) {
        *reason = tr("Destination not found");
    }
    return false;
}

void MigrationOrchestrator::tryAssignQueuedDeployments() {
    if (!m_autoAssignmentEnabled || !m_server) {
        return;
    }

    DeploymentAssignment next;
    int safety = 0;
    while (m_deploymentManager->peek(&next) && safety++ < 1000) {
        const QString destination_id = selectDestinationFor(next, next.profile_size_bytes);
        if (destination_id.isEmpty()) {
            break;
        }

        m_deploymentManager->dequeue();
        if (m_activeDestinations.contains(destination_id)) {
            m_pendingAssignments[destination_id].enqueue(next);
            Q_EMIT orchestratorStatus(tr("Deployment queued for %1: %2")
                                        .arg(destination_id, next.deployment_id));
            continue;
        }

        if (dispatchAssignment(destination_id, next)) {
            Q_EMIT orchestratorStatus(tr("Deployment assigned: %1 -> %2")
                                        .arg(next.deployment_id, destination_id));
        }
    }
}

QString MigrationOrchestrator::selectDestinationFor(const DeploymentAssignment& assignment, qint64 required_free_bytes) {
    const auto destinations = m_registry->destinations();
    return m_mappingEngine.selectDestination(assignment, destinations, m_activeDestinations, required_free_bytes);
}

bool MigrationOrchestrator::dispatchAssignment(const QString& destination_id, const DeploymentAssignment& assignment) {
    if (!m_server || destination_id.isEmpty()) {
        return false;
    }

    m_activeDestinations.insert(destination_id);
    m_server->sendDeploymentAssignment(destination_id, assignment);
    return true;
}

void MigrationOrchestrator::handleAssignmentCompletion(const QString& destination_id) {
    if (destination_id.isEmpty()) {
        return;
    }

    if (!m_pendingAssignments.contains(destination_id)) {
        return;
    }

    auto& queue = m_pendingAssignments[destination_id];
    if (queue.isEmpty()) {
        m_pendingAssignments.remove(destination_id);
        return;
    }

    const auto next = queue.dequeue();
    if (queue.isEmpty()) {
        m_pendingAssignments.remove(destination_id);
    }

    if (dispatchAssignment(destination_id, next)) {
        Q_EMIT orchestratorStatus(tr("Deployment assigned: %1 -> %2")
                                    .arg(next.deployment_id, destination_id));
    }
}

} // namespace sak

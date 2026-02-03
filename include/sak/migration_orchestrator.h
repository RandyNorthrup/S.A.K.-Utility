#pragma once

#include <QObject>
#include <QTimer>
#include <QMap>
#include <QSet>

#include "sak/destination_registry.h"
#include "sak/deployment_manager.h"
#include "sak/mapping_engine.h"
#include "sak/orchestration_server_interface.h"

namespace sak {

class OrchestrationDiscoveryService;

class MigrationOrchestrator : public QObject {
    Q_OBJECT

public:
    explicit MigrationOrchestrator(QObject* parent = nullptr);

    DestinationRegistry* registry() const;
    DeploymentManager* deploymentManager() const;
    OrchestrationServerInterface* server() const;

    void setServer(OrchestrationServerInterface* server);

    bool startServer(quint16 port);
    void stopServer();
    void requestHealthCheck(const QString& destination_id);

    void startDiscovery(quint16 port);
    void stopDiscovery();

    void startHealthPolling(int interval_ms);
    void stopHealthPolling();

    void registerDestination(const DestinationPC& destination);
    void updateHealth(const QString& destination_id, const DestinationHealth& health);
    void queueDeployment(const DeploymentAssignment& assignment);
    void enableAutoAssignment(bool enabled);
    bool autoAssignmentEnabled() const;
    void setMappingStrategy(MappingEngine::Strategy strategy);
    MappingEngine::Strategy mappingStrategy() const;
    void assignDeploymentToDestination(const QString& destination_id,
                                       const DeploymentAssignment& assignment,
                                       qint64 required_free_bytes);

    void pauseAssignment(const QString& destination_id,
                         const QString& deployment_id,
                         const QString& job_id);
    void resumeAssignment(const QString& destination_id,
                          const QString& deployment_id,
                          const QString& job_id);
    void cancelAssignment(const QString& destination_id,
                          const QString& deployment_id,
                          const QString& job_id);

    bool canAssignDeployment(const QString& destination_id,
                             qint64 required_free_bytes,
                             QString* reason = nullptr) const;

Q_SIGNALS:
    void orchestratorStatus(const QString& message);
    void deploymentReady(const DeploymentAssignment& assignment);
    void deploymentRejected(const QString& destination_id, const QString& reason);
    void deploymentProgress(const DeploymentProgress& progress);
    void deploymentCompleted(const DeploymentCompletion& completion);
    void aggregateProgress(int completed, int total, int percent);

private:
    void tryAssignQueuedDeployments();
    QString selectDestinationFor(const DeploymentAssignment& assignment, qint64 required_free_bytes);
    bool dispatchAssignment(const QString& destination_id, const DeploymentAssignment& assignment);
    void handleAssignmentCompletion(const QString& destination_id);

    DestinationRegistry* m_registry{nullptr};
    DeploymentManager* m_deploymentManager{nullptr};
    OrchestrationServerInterface* m_server{nullptr};
    OrchestrationDiscoveryService* m_discovery{nullptr};
    QTimer* m_healthPollTimer{nullptr};
    QMap<QString, DeploymentProgress> m_progressByDestination;
    QSet<QString> m_completedDestinations;
    QSet<QString> m_activeDestinations;
    bool m_autoAssignmentEnabled{true};
    MappingEngine m_mappingEngine;
    QMap<QString, QQueue<DeploymentAssignment>> m_pendingAssignments;
    quint16 m_listenPort{0};
};

} // namespace sak

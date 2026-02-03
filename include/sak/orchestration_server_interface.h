#pragma once

#include <QObject>
#include <QString>

#include "sak/orchestration_types.h"

#include <QJsonObject>

namespace sak {

class OrchestrationServerInterface : public QObject {
    Q_OBJECT

public:
    explicit OrchestrationServerInterface(QObject* parent = nullptr) : QObject(parent) {}
    ~OrchestrationServerInterface() override = default;

    virtual bool start(quint16 port) = 0;
    virtual void stop() = 0;
    virtual void sendHealthCheck(const QString& destination_id) = 0;
    virtual void sendDeploymentAssignment(const QString& destination_id, const DeploymentAssignment& assignment) = 0;
    virtual void sendAssignmentPause(const QString& destination_id,
                                     const QString& deployment_id,
                                     const QString& job_id) = 0;
    virtual void sendAssignmentResume(const QString& destination_id,
                                      const QString& deployment_id,
                                      const QString& job_id) = 0;
    virtual void sendAssignmentCancel(const QString& destination_id,
                                      const QString& deployment_id,
                                      const QString& job_id) = 0;

Q_SIGNALS:
    void destinationRegistered(const DestinationPC& destination);
    void healthUpdated(const QString& destination_id, const DestinationHealth& health);
    void progressUpdated(const DeploymentProgress& progress);
    void deploymentCompleted(const DeploymentCompletion& completion);
    void statusMessage(const QString& message);
    void connectionError(const QString& message);
};

} // namespace sak

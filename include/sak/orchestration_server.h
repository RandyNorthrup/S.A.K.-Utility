#pragma once

#include <QObject>
#include <QMap>
#include <QTimer>

#include "sak/orchestration_protocol.h"
#include "sak/orchestration_server_interface.h"

class QTcpServer;
class QTcpSocket;

namespace sak {

class OrchestrationServer : public OrchestrationServerInterface {
    Q_OBJECT

public:
    explicit OrchestrationServer(QObject* parent = nullptr);

    bool start(quint16 port);
    void stop();

    void setHealthIntervalMs(int ms);
    int healthIntervalMs() const;

    void sendHealthCheck(const QString& destination_id) override;
    void sendDeploymentAssignment(const QString& destination_id, const DeploymentAssignment& assignment) override;
    void sendAssignmentPause(const QString& destination_id,
                             const QString& deployment_id,
                             const QString& job_id) override;
    void sendAssignmentResume(const QString& destination_id,
                              const QString& deployment_id,
                              const QString& job_id) override;
    void sendAssignmentCancel(const QString& destination_id,
                              const QString& deployment_id,
                              const QString& job_id) override;

Q_SIGNALS:
    void destinationRegistered(const DestinationPC& destination);
    void healthUpdated(const QString& destination_id, const DestinationHealth& health);
    void statusMessage(const QString& message);
    void connectionError(const QString& message);

private Q_SLOTS:
    void onNewConnection();
    void onSocketReadyRead();
    void onSocketDisconnected();

private:
    QString ensureDestinationId(const DestinationPC& destination, const QTcpSocket* socket) const;
    void handleMessage(QTcpSocket* socket, const QJsonObject& message);
    void sendAssignmentControl(const QString& destination_id,
                               const QString& deployment_id,
                               const QString& job_id,
                               const QString& action);

    QTcpServer* m_server{nullptr};
    QMap<QTcpSocket*, QByteArray> m_buffers;
    QMap<QString, QTcpSocket*> m_destinationSockets;
    QMap<QTcpSocket*, QString> m_socketDestinations;
    QTimer* m_healthTimer{nullptr};
    int m_healthIntervalMs{10000};
};

} // namespace sak

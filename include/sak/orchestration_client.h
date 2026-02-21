#pragma once

#include <QObject>
#include <QHostAddress>
#include <QTimer>

#include "sak/orchestration_protocol.h"
#include "sak/orchestration_types.h"

class QTcpSocket;

namespace sak {

class OrchestrationClient : public QObject {
    Q_OBJECT

public:
    explicit OrchestrationClient(QObject* parent = nullptr);

    void connectToServer(const QHostAddress& host, quint16 port);
    void disconnectFromServer();
    void setAutoReconnectEnabled(bool enabled);
    void setReconnectInterval(int ms);
    [[nodiscard]] bool isConnected() const;

    void setDestinationInfo(const DestinationPC& destination);
    void sendProgress(const DeploymentProgress& progress);
    void sendCompletion(const DeploymentCompletion& completion);

Q_SIGNALS:
    void assignmentReceived(const DeploymentAssignment& assignment);
    void assignmentPaused(const QString& deployment_id, const QString& job_id);
    void assignmentResumed(const QString& deployment_id, const QString& job_id);
    void assignmentCanceled(const QString& deployment_id, const QString& job_id);
    void connectionError(const QString& message);
    void statusMessage(const QString& message);

private Q_SLOTS:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onReconnectTimeout();

private:
    void handleMessage(const QJsonObject& message);

    QTcpSocket* m_socket{nullptr};
    QByteArray m_buffer;
    DestinationPC m_destination;
    QTimer* m_reconnectTimer{nullptr};
    QHostAddress m_lastHost;
    quint16 m_lastPort{0};
    bool m_autoReconnect{true};
};

} // namespace sak

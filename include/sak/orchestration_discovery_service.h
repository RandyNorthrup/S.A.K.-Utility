#pragma once

#include <QObject>
#include <QUdpSocket>
#include <QTimer>
#include <QHostAddress>

#include "sak/orchestration_types.h"

namespace sak {

class OrchestrationDiscoveryService : public QObject {
    Q_OBJECT

public:
    explicit OrchestrationDiscoveryService(QObject* parent = nullptr);

    void setDestinationInfo(const DestinationPC& destination);
    void setOrchestratorPort(quint16 port);
    void setPort(quint16 port);

    void startAsOrchestrator();
    void startAsDestination();
    void stop();
    bool isRunning() const;

    void sendDiscoveryProbe(const QHostAddress& address, quint16 port);
    void sendDestinationAnnounceTo(const QHostAddress& address, quint16 port);

Q_SIGNALS:
    void destinationDiscovered(const DestinationPC& destination);
    void orchestratorDiscovered(const QHostAddress& address, quint16 port);
    void discoveryError(const QString& message);

private Q_SLOTS:
    void onReadyRead();
    void onBroadcastTimeout();

private:
    void sendBroadcastDiscovery();
    void sendDestinationAnnounce(const QHostAddress& address, quint16 port);

    QUdpSocket* m_socket{nullptr};
    QTimer* m_broadcastTimer{nullptr};
    DestinationPC m_destinationInfo;
    quint16 m_orchestratorPort{54322};
    quint16 m_port{54321};
    bool m_running{false};
    bool m_roleOrchestrator{true};
};

} // namespace sak

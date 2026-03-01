// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QObject>
#include <QUdpSocket>
#include <QTimer>
#include <QHostAddress>
#include <atomic>

#include "sak/orchestration_types.h"
#include "sak/network_constants.h"

namespace sak {

/// @brief UDP broadcast discovery for orchestrator and destinations
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
    quint16 m_orchestratorPort{sak::kPortControl};
    quint16 m_port{sak::kPortDiscovery};
    std::atomic<bool> m_running{false};
    bool m_roleOrchestrator{true};
};

} // namespace sak

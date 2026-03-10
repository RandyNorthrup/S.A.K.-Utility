// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QHostAddress>
#include <QObject>
#include <QTimer>
#include <QUdpSocket>

#include <atomic>

class QNetworkInterface;

#include "sak/network_constants.h"
#include "sak/network_transfer_types.h"

namespace sak {

/// @brief UDP broadcast service for discovering peers on the local network
class PeerDiscoveryService : public QObject {
    Q_OBJECT

public:
    explicit PeerDiscoveryService(QObject* parent = nullptr);

    void setPeerInfo(const TransferPeerInfo& info);
    void setPort(quint16 port);

    void start();
    void stop();
    bool isRunning() const;

Q_SIGNALS:
    void peerDiscovered(const TransferPeerInfo& peer);
    void discoveryError(const QString& message);

private Q_SLOTS:
    void onReadyRead();
    void sendAnnouncement();

private:
    void sendResponse(const QHostAddress& address, quint16 port);
    bool broadcastOnInterface(const QByteArray& datagram, const QNetworkInterface& iface);

    QUdpSocket* m_socket{nullptr};
    QTimer* m_broadcastTimer{nullptr};
    TransferPeerInfo m_peerInfo;
    quint16 m_port{sak::kPortDiscovery};
    std::atomic<bool> m_running{false};
};

}  // namespace sak

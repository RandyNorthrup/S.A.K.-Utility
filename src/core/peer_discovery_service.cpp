// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/peer_discovery_service.h"
#include "sak/network_transfer_protocol.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QJsonDocument>
#include <QNetworkInterface>

namespace sak {

PeerDiscoveryService::PeerDiscoveryService(QObject* parent)
    : QObject(parent)
    , m_socket(new QUdpSocket(this))
    , m_broadcastTimer(new QTimer(this))
{
    m_broadcastTimer->setInterval(sak::kTimerBroadcastMs);
    connect(m_broadcastTimer, &QTimer::timeout, this, &PeerDiscoveryService::sendAnnouncement);
    connect(m_socket, &QUdpSocket::readyRead, this, &PeerDiscoveryService::onReadyRead);
}

void PeerDiscoveryService::setPeerInfo(const TransferPeerInfo& info) {
    m_peerInfo = info;
}

void PeerDiscoveryService::setPort(quint16 port) {
    m_port = port;
}

void PeerDiscoveryService::start() {
    if (m_running) {
        return;
    }

    if (!m_socket->bind(QHostAddress::AnyIPv4, m_port, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        logError("PeerDiscoveryService bind failed on port {}: {}", m_port, m_socket->errorString().toStdString());
        Q_EMIT discoveryError(tr("Failed to bind discovery port %1").arg(m_port));
        return;
    }

    m_running = true;
    m_broadcastTimer->start();
    sendAnnouncement();
    logInfo("PeerDiscoveryService started on port {}", m_port);
}

void PeerDiscoveryService::stop() {
    if (!m_running) {
        return;
    }

    m_running = false;
    m_broadcastTimer->stop();
    m_socket->close();
    logInfo("PeerDiscoveryService stopped");
}

bool PeerDiscoveryService::isRunning() const {
    return m_running;
}

void PeerDiscoveryService::sendAnnouncement() {
    QJsonObject payload;
    payload["message_type"] = "ANNOUNCE";
    payload["protocol_version"] = "1.0";
    payload["timestamp"] = QDateTime::currentDateTimeUtc().toSecsSinceEpoch();
    payload["peer_info"] = m_peerInfo.toJson();

    QJsonDocument doc(payload);
    QByteArray datagram = doc.toJson(QJsonDocument::Compact);
    bool sentAny = false;

    for (const auto& interface : QNetworkInterface::allInterfaces()) {
        if (!(interface.flags() & QNetworkInterface::IsUp) ||
            !(interface.flags() & QNetworkInterface::IsRunning) ||
            (interface.flags() & QNetworkInterface::IsLoopBack)) {
            continue;
        }

        sentAny |= broadcastOnInterface(datagram, interface);
    }

    if (!sentAny) {
        logWarning("PeerDiscoveryService announcement skipped: no broadcast interfaces available");
    }
}

void PeerDiscoveryService::onReadyRead() {
    // Maximum acceptable datagram size (64 KB) to prevent resource exhaustion.
    constexpr qint64 kMaxDatagramSize = 65536;

    while (m_socket->hasPendingDatagrams()) {
        const qint64 datagramSize = m_socket->pendingDatagramSize();
        if (datagramSize > kMaxDatagramSize || datagramSize <= 0) {
            // Discard oversized or empty datagrams.
            QByteArray discard(static_cast<int>(datagramSize), 0);
            m_socket->readDatagram(discard.data(), discard.size());
            logWarning("PeerDiscoveryService discarded oversized datagram ({} bytes)", datagramSize);
            continue;
        }

        QByteArray datagram;
        datagram.resize(static_cast<int>(datagramSize));
        QHostAddress sender;
        quint16 senderPort = 0;
        m_socket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);

        QJsonParseError error{};
        const auto doc = QJsonDocument::fromJson(datagram, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            logWarning("PeerDiscoveryService received invalid discovery packet");
            continue;
        }

        auto obj = doc.object();

        // Validate required fields to reject malformed announcements.
        const QString messageType = obj.value("message_type").toString();
        if (messageType.isEmpty()) {
            logWarning("PeerDiscoveryService received packet with missing message_type");
            continue;
        }

        if (messageType == "ANNOUNCE") {
            auto peer_obj = obj.value("peer_info").toObject();
            TransferPeerInfo peer = TransferPeerInfo::fromJson(peer_obj);
            peer.ip_address = sender.toString();
            peer.last_seen = QDateTime::currentDateTime();
            Q_EMIT peerDiscovered(peer);

            // Send unicast response
            sendResponse(sender, senderPort);
        } else if (messageType == "DISCOVERY_REPLY") {
            auto peer_obj = obj.value("peer_info").toObject();
            TransferPeerInfo peer = TransferPeerInfo::fromJson(peer_obj);
            peer.ip_address = sender.toString();
            peer.last_seen = QDateTime::currentDateTime();
            Q_EMIT peerDiscovered(peer);
        } else {
            logWarning("PeerDiscoveryService received unknown message type");
        }
    }
}

bool PeerDiscoveryService::broadcastOnInterface(
    const QByteArray& datagram, const QNetworkInterface& iface) {
    bool sent = false;
    for (const auto& entry : iface.addressEntries()) {
        if (entry.broadcast().isNull()) continue;
        m_socket->writeDatagram(datagram, entry.broadcast(), m_port);
        sent = true;
    }
    return sent;
}

void PeerDiscoveryService::sendResponse(const QHostAddress& address, quint16 port) {
    QJsonObject payload;
    payload["message_type"] = "DISCOVERY_REPLY";
    payload["protocol_version"] = "1.0";
    payload["timestamp"] = QDateTime::currentDateTimeUtc().toSecsSinceEpoch();
    payload["peer_info"] = m_peerInfo.toJson();

    QJsonDocument doc(payload);
    QByteArray datagram = doc.toJson(QJsonDocument::Compact);
    m_socket->writeDatagram(datagram, address, port);
}

} // namespace sak

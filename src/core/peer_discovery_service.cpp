#include "sak/peer_discovery_service.h"
#include "sak/network_transfer_protocol.h"
#include "sak/logger.h"

#include <QJsonDocument>
#include <QNetworkInterface>

namespace sak {

PeerDiscoveryService::PeerDiscoveryService(QObject* parent)
    : QObject(parent)
    , m_socket(new QUdpSocket(this))
    , m_broadcastTimer(new QTimer(this))
{
    m_broadcastTimer->setInterval(2000);
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
        log_error("PeerDiscoveryService bind failed on port {}: {}", m_port, m_socket->errorString().toStdString());
        Q_EMIT discoveryError(tr("Failed to bind discovery port %1").arg(m_port));
        return;
    }

    m_running = true;
    m_broadcastTimer->start();
    sendAnnouncement();
    log_info("PeerDiscoveryService started on port {}", m_port);
}

void PeerDiscoveryService::stop() {
    if (!m_running) {
        return;
    }

    m_running = false;
    m_broadcastTimer->stop();
    m_socket->close();
    log_info("PeerDiscoveryService stopped");
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

        for (const auto& entry : interface.addressEntries()) {
            if (entry.broadcast().isNull()) {
                continue;
            }
            m_socket->writeDatagram(datagram, entry.broadcast(), m_port);
            sentAny = true;
        }
    }

    if (!sentAny) {
        log_warning("PeerDiscoveryService announcement skipped: no broadcast interfaces available");
    }
}

void PeerDiscoveryService::onReadyRead() {
    while (m_socket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(static_cast<int>(m_socket->pendingDatagramSize()));
        QHostAddress sender;
        quint16 senderPort = 0;
        m_socket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);

        QJsonParseError error{};
        const auto doc = QJsonDocument::fromJson(datagram, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            log_warning("PeerDiscoveryService received invalid discovery packet");
            continue;
        }

        auto obj = doc.object();
        if (obj.value("message_type").toString() == "ANNOUNCE") {
            auto peer_obj = obj.value("peer_info").toObject();
            TransferPeerInfo peer = TransferPeerInfo::fromJson(peer_obj);
            peer.ip_address = sender.toString();
            peer.last_seen = QDateTime::currentDateTime();
            Q_EMIT peerDiscovered(peer);

            // Send unicast response
            sendResponse(sender, senderPort);
        } else if (obj.value("message_type").toString() == "DISCOVERY_REPLY") {
            auto peer_obj = obj.value("peer_info").toObject();
            TransferPeerInfo peer = TransferPeerInfo::fromJson(peer_obj);
            peer.ip_address = sender.toString();
            peer.last_seen = QDateTime::currentDateTime();
            Q_EMIT peerDiscovered(peer);
        } else {
            log_warning("PeerDiscoveryService received unknown message type");
        }
    }
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

#include "sak/orchestration_discovery_service.h"

#include "sak/logger.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QDateTime>

namespace sak {

namespace {
constexpr int kBroadcastIntervalMs = 3000;
}

OrchestrationDiscoveryService::OrchestrationDiscoveryService(QObject* parent)
    : QObject(parent)
    , m_socket(new QUdpSocket(this))
    , m_broadcastTimer(new QTimer(this))
{
    m_broadcastTimer->setInterval(kBroadcastIntervalMs);
    connect(m_broadcastTimer, &QTimer::timeout, this, &OrchestrationDiscoveryService::onBroadcastTimeout);
    connect(m_socket, &QUdpSocket::readyRead, this, &OrchestrationDiscoveryService::onReadyRead);
}

void OrchestrationDiscoveryService::setDestinationInfo(const DestinationPC& destination) {
    m_destinationInfo = destination;
}

void OrchestrationDiscoveryService::setOrchestratorPort(quint16 port) {
    m_orchestratorPort = port;
}

void OrchestrationDiscoveryService::setPort(quint16 port) {
    m_port = port;
}

void OrchestrationDiscoveryService::startAsOrchestrator() {
    m_roleOrchestrator = true;
    if (m_running) {
        return;
    }

    if (!m_socket->bind(QHostAddress::AnyIPv4, m_port, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        Q_EMIT discoveryError(tr("Failed to bind discovery port %1").arg(m_port));
        logError("OrchestrationDiscoveryService bind failed on port {}: {}", m_port,
                  m_socket->errorString().toStdString());
        return;
    }

    m_running = true;
    m_broadcastTimer->start();
    sendBroadcastDiscovery();
    logInfo("OrchestrationDiscoveryService started as orchestrator on port {}", m_port);
}

void OrchestrationDiscoveryService::startAsDestination() {
    m_roleOrchestrator = false;
    if (m_running) {
        return;
    }

    if (!m_socket->bind(QHostAddress::AnyIPv4, m_port, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        Q_EMIT discoveryError(tr("Failed to bind discovery port %1").arg(m_port));
        logError("OrchestrationDiscoveryService bind failed on port {}: {}", m_port,
                  m_socket->errorString().toStdString());
        return;
    }

    m_running = true;
    m_broadcastTimer->start();
    sendDestinationAnnounce(QHostAddress::Broadcast, m_port);
    logInfo("OrchestrationDiscoveryService started as destination on port {}", m_port);
}

void OrchestrationDiscoveryService::stop() {
    if (!m_running) {
        return;
    }

    m_running = false;
    m_broadcastTimer->stop();
    m_socket->close();
    logInfo("OrchestrationDiscoveryService stopped");
}

bool OrchestrationDiscoveryService::isRunning() const {
    return m_running;
}

void OrchestrationDiscoveryService::sendDiscoveryProbe(const QHostAddress& address, quint16 port) {
    QJsonObject payload;
    payload["message_type"] = "ORCH_DISCOVERY";
    payload["protocol_version"] = "1.0";
    payload["timestamp"] = QDateTime::currentDateTimeUtc().toSecsSinceEpoch();
    payload["orchestrator_port"] = static_cast<int>(m_orchestratorPort);

    QJsonDocument doc(payload);
    m_socket->writeDatagram(doc.toJson(QJsonDocument::Compact), address, port);
}

void OrchestrationDiscoveryService::sendDestinationAnnounceTo(const QHostAddress& address, quint16 port) {
    sendDestinationAnnounce(address, port);
}

void OrchestrationDiscoveryService::onBroadcastTimeout() {
    if (m_roleOrchestrator) {
        sendBroadcastDiscovery();
    } else {
        sendDestinationAnnounce(QHostAddress::Broadcast, m_port);
    }
}

void OrchestrationDiscoveryService::sendBroadcastDiscovery() {
    QJsonObject payload;
    payload["message_type"] = "ORCH_DISCOVERY";
    payload["protocol_version"] = "1.0";
    payload["timestamp"] = QDateTime::currentDateTimeUtc().toSecsSinceEpoch();
    payload["orchestrator_port"] = static_cast<int>(m_orchestratorPort);

    QJsonDocument doc(payload);
    QByteArray datagram = doc.toJson(QJsonDocument::Compact);

    bool sentAny = false;
    for (const auto& interface : QNetworkInterface::allInterfaces()) {
        if (!(interface.flags() & QNetworkInterface::IsUp)
            || !(interface.flags() & QNetworkInterface::IsRunning)
            || (interface.flags() & QNetworkInterface::IsLoopBack)) {
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
        logWarning("OrchestrationDiscoveryService broadcast skipped: no broadcast interfaces");
    }
}

void OrchestrationDiscoveryService::sendDestinationAnnounce(const QHostAddress& address, quint16 port) {
    QJsonObject payload;
    payload["message_type"] = "DESTINATION_ANNOUNCE";
    payload["protocol_version"] = "1.0";
    payload["timestamp"] = QDateTime::currentDateTimeUtc().toSecsSinceEpoch();
    payload["destination_info"] = m_destinationInfo.toJson();

    QJsonDocument doc(payload);
    m_socket->writeDatagram(doc.toJson(QJsonDocument::Compact), address, port);
}

void OrchestrationDiscoveryService::onReadyRead() {
    while (m_socket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(static_cast<int>(m_socket->pendingDatagramSize()));
        QHostAddress sender;
        quint16 senderPort = 0;
        m_socket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);

        QJsonParseError error{};
        const auto doc = QJsonDocument::fromJson(datagram, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            continue;
        }

        const auto obj = doc.object();
        const QString type = obj.value("message_type").toString();

        if (type == "ORCH_DISCOVERY") {
            const auto port = static_cast<quint16>(obj.value("orchestrator_port").toInt(m_orchestratorPort));
            Q_EMIT orchestratorDiscovered(sender, port);

            if (!m_roleOrchestrator) {
                sendDestinationAnnounce(sender, senderPort);
            }
        } else if (type == "DESTINATION_ANNOUNCE") {
            if (m_roleOrchestrator) {
                auto infoObj = obj.value("destination_info").toObject();
                DestinationPC destination = DestinationPC::fromJson(infoObj);
                destination.ip_address = sender.toString();
                destination.last_seen = QDateTime::currentDateTimeUtc();
                if (destination.destination_id.isEmpty()) {
                    destination.destination_id = QString("%1@%2").arg(destination.hostname, destination.ip_address);
                }
                Q_EMIT destinationDiscovered(destination);
            }
        }
    }
}

} // namespace sak

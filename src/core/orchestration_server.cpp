#include "sak/orchestration_server.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonObject>

namespace sak {

OrchestrationServer::OrchestrationServer(QObject* parent)
    : OrchestrationServerInterface(parent)
    , m_server(new QTcpServer(this))
    , m_healthTimer(new QTimer(this))
{
    connect(m_server, &QTcpServer::newConnection, this, &OrchestrationServer::onNewConnection);
    m_healthTimer->setInterval(m_healthIntervalMs);
}

bool OrchestrationServer::start(quint16 port) {
    if (m_server->isListening()) {
        return true;
    }

    if (!m_server->listen(QHostAddress::AnyIPv4, port)) {
        Q_EMIT connectionError(tr("Failed to listen on port %1").arg(port));
        return false;
    }

    Q_EMIT statusMessage(tr("Orchestrator listening on port %1").arg(port));
    return true;
}

void OrchestrationServer::stop() {
    if (m_server->isListening()) {
        m_server->close();
    }

    for (auto socket : m_buffers.keys()) {
        socket->disconnectFromHost();
        socket->deleteLater();
    }

    m_buffers.clear();
    m_destinationSockets.clear();
    m_socketDestinations.clear();
}

void OrchestrationServer::setHealthIntervalMs(int ms) {
    m_healthIntervalMs = ms;
    m_healthTimer->setInterval(ms);
}

int OrchestrationServer::healthIntervalMs() const {
    return m_healthIntervalMs;
}

void OrchestrationServer::sendHealthCheck(const QString& destination_id) {
    if (!m_destinationSockets.contains(destination_id)) {
        return;
    }
    QJsonObject payload;
    payload["destination_id"] = destination_id;
    OrchestrationProtocol::writeMessage(m_destinationSockets.value(destination_id),
        OrchestrationProtocol::makeMessage(OrchestrationMessageType::HealthCheckRequest, payload));
}

void OrchestrationServer::sendDeploymentAssignment(const QString& destination_id, const DeploymentAssignment& assignment) {
    if (!m_destinationSockets.contains(destination_id)) {
        return;
    }
    QJsonObject payload;
    payload["destination_id"] = destination_id;
    payload["assignment"] = assignment.toJson();
    OrchestrationProtocol::writeMessage(m_destinationSockets.value(destination_id),
        OrchestrationProtocol::makeMessage(OrchestrationMessageType::DeploymentAssign, payload));
}

void OrchestrationServer::sendAssignmentPause(const QString& destination_id,
                                              const QString& deployment_id,
                                              const QString& job_id) {
    sendAssignmentControl(destination_id, deployment_id, job_id, "pause");
}

void OrchestrationServer::sendAssignmentResume(const QString& destination_id,
                                               const QString& deployment_id,
                                               const QString& job_id) {
    sendAssignmentControl(destination_id, deployment_id, job_id, "resume");
}

void OrchestrationServer::sendAssignmentCancel(const QString& destination_id,
                                               const QString& deployment_id,
                                               const QString& job_id) {
    sendAssignmentControl(destination_id, deployment_id, job_id, "cancel");
}

void OrchestrationServer::onNewConnection() {
    while (m_server->hasPendingConnections()) {
        auto* socket = m_server->nextPendingConnection();
        if (!socket) {
            continue;
        }
        m_buffers.insert(socket, {});
        connect(socket, &QTcpSocket::readyRead, this, &OrchestrationServer::onSocketReadyRead);
        connect(socket, &QTcpSocket::disconnected, this, &OrchestrationServer::onSocketDisconnected);
        connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError) {
            Q_EMIT connectionError(socket->errorString());
        });
    }
}

void OrchestrationServer::onSocketReadyRead() {
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) {
        return;
    }

    auto buffer = m_buffers.value(socket);
    const auto messages = OrchestrationProtocol::readMessages(buffer, socket->readAll());
    m_buffers.insert(socket, buffer);

    for (const auto& message : messages) {
        handleMessage(socket, message);
    }
}

void OrchestrationServer::onSocketDisconnected() {
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) {
        return;
    }

    if (m_socketDestinations.contains(socket)) {
        const auto destination_id = m_socketDestinations.take(socket);
        m_destinationSockets.remove(destination_id);
        Q_EMIT statusMessage(tr("Destination disconnected: %1").arg(destination_id));
    }

    m_buffers.remove(socket);
}

QString OrchestrationServer::ensureDestinationId(const DestinationPC& destination, const QTcpSocket* socket) const {
    if (!destination.destination_id.isEmpty()) {
        return destination.destination_id;
    }
    return QString("%1@%2").arg(destination.hostname, socket->peerAddress().toString());
}

void OrchestrationServer::handleMessage(QTcpSocket* socket, const QJsonObject& message) {
    const auto type = OrchestrationProtocol::parseType(message.value("message_type").toString());
    if (!type.has_value()) {
        return;
    }

    switch (*type) {
        case OrchestrationMessageType::DestinationRegister: {
            const auto info = message.value("destination_info").toObject();
            DestinationPC destination = DestinationPC::fromJson(info);
            destination.ip_address = socket->peerAddress().toString();
            destination.destination_id = ensureDestinationId(destination, socket);

            m_destinationSockets.insert(destination.destination_id, socket);
            m_socketDestinations.insert(socket, destination.destination_id);

            Q_EMIT destinationRegistered(destination);
            Q_EMIT statusMessage(tr("Destination registered: %1").arg(destination.hostname));
            break;
        }
        case OrchestrationMessageType::HealthCheckResponse: {
            const QString destination_id = message.value("destination_id").toString();
            const auto healthObj = message.value("health_metrics").toObject();
            const DestinationHealth health = DestinationHealth::fromJson(healthObj);

            const QString resolved = destination_id.isEmpty() && m_socketDestinations.contains(socket)
                ? m_socketDestinations.value(socket)
                : destination_id;

            if (!resolved.isEmpty()) {
                Q_EMIT healthUpdated(resolved, health);
            }
            break;
        }
        case OrchestrationMessageType::ProgressUpdate: {
            DeploymentProgress progress = DeploymentProgress::fromJson(message);
            if (progress.destination_id.isEmpty() && m_socketDestinations.contains(socket)) {
                progress.destination_id = m_socketDestinations.value(socket);
            }
            Q_EMIT progressUpdated(progress);
            break;
        }
        case OrchestrationMessageType::DeploymentComplete: {
            DeploymentCompletion completion = DeploymentCompletion::fromJson(message);
            if (completion.destination_id.isEmpty() && m_socketDestinations.contains(socket)) {
                completion.destination_id = m_socketDestinations.value(socket);
            }
            Q_EMIT deploymentCompleted(completion);
            break;
        }
        default:
            break;
    }
}

void OrchestrationServer::sendAssignmentControl(const QString& destination_id,
                                                const QString& deployment_id,
                                                const QString& job_id,
                                                const QString& action) {
    if (!m_destinationSockets.contains(destination_id)) {
        return;
    }

    QJsonObject payload;
    payload["destination_id"] = destination_id;
    payload["deployment_id"] = deployment_id;
    payload["job_id"] = job_id;
    payload["action"] = action;

    OrchestrationProtocol::writeMessage(m_destinationSockets.value(destination_id),
        OrchestrationProtocol::makeMessage(OrchestrationMessageType::AssignmentControl, payload));
}

} // namespace sak

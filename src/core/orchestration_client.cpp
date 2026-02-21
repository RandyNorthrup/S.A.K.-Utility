#include "sak/orchestration_client.h"

#include <QTcpSocket>
#include <QJsonObject>

namespace sak {

OrchestrationClient::OrchestrationClient(QObject* parent)
    : QObject(parent)
    , m_socket(new QTcpSocket(this))
    , m_reconnectTimer(new QTimer(this))
{
    connect(m_socket, &QTcpSocket::connected, this, &OrchestrationClient::onConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &OrchestrationClient::onDisconnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &OrchestrationClient::onReadyRead);
    connect(m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        Q_EMIT connectionError(m_socket->errorString());
    });

    m_reconnectTimer->setInterval(5000);
    connect(m_reconnectTimer, &QTimer::timeout, this, &OrchestrationClient::onReconnectTimeout);
}

void OrchestrationClient::setReconnectInterval(int ms) {
    m_reconnectTimer->setInterval(qMax(100, ms));
}

void OrchestrationClient::connectToServer(const QHostAddress& host, quint16 port) {
    m_lastHost = host;
    m_lastPort = port;
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->disconnectFromHost();
    }
    m_socket->connectToHost(host, port);
}

void OrchestrationClient::disconnectFromServer() {
    m_autoReconnect = false;
    m_reconnectTimer->stop();
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->disconnectFromHost();
    }
}

void OrchestrationClient::setAutoReconnectEnabled(bool enabled) {
    m_autoReconnect = enabled;
    if (!m_autoReconnect) {
        m_reconnectTimer->stop();
    }
}

bool OrchestrationClient::isConnected() const {
    return m_socket->state() == QAbstractSocket::ConnectedState
        || m_socket->state() == QAbstractSocket::ConnectingState;
}

void OrchestrationClient::setDestinationInfo(const DestinationPC& destination) {
    m_destination = destination;
}

void OrchestrationClient::sendProgress(const DeploymentProgress& progress) {
    OrchestrationProtocol::writeMessage(m_socket,
        OrchestrationProtocol::makeMessage(OrchestrationMessageType::ProgressUpdate, progress.toJson()));
}

void OrchestrationClient::sendCompletion(const DeploymentCompletion& completion) {
    OrchestrationProtocol::writeMessage(m_socket,
        OrchestrationProtocol::makeMessage(OrchestrationMessageType::DeploymentComplete, completion.toJson()));
}

void OrchestrationClient::onConnected() {
    m_reconnectTimer->stop();
    if (m_destination.destination_id.isEmpty()) {
        m_destination.destination_id = QString("%1@%2").arg(m_destination.hostname, m_socket->localAddress().toString());
    }

    QJsonObject payload;
    payload["destination_info"] = m_destination.toJson();
    OrchestrationProtocol::writeMessage(m_socket,
        OrchestrationProtocol::makeMessage(OrchestrationMessageType::DestinationRegister, payload));
    Q_EMIT statusMessage(tr("Registered destination with orchestrator"));
}

void OrchestrationClient::onDisconnected() {
    Q_EMIT statusMessage(tr("Disconnected from orchestrator"));

    if (m_autoReconnect && !m_lastHost.isNull() && m_lastPort != 0) {
        m_reconnectTimer->start();
    }
}

void OrchestrationClient::onReadyRead() {
    const auto messages = OrchestrationProtocol::readMessages(m_buffer, m_socket->readAll());
    for (const auto& message : messages) {
        handleMessage(message);
    }
}

void OrchestrationClient::handleMessage(const QJsonObject& message) {
    const auto type = OrchestrationProtocol::parseType(message.value("message_type").toString());
    if (!type.has_value()) {
        return;
    }

    switch (*type) {
        case OrchestrationMessageType::HealthCheckRequest: {
            QJsonObject payload;
            payload["destination_id"] = m_destination.destination_id;
            payload["health_metrics"] = m_destination.health.toJson();
            OrchestrationProtocol::writeMessage(m_socket,
                OrchestrationProtocol::makeMessage(OrchestrationMessageType::HealthCheckResponse, payload));
            break;
        }
        case OrchestrationMessageType::DeploymentAssign: {
            auto assignmentObj = message.value("assignment").toObject();
            auto assignment = DeploymentAssignment::fromJson(assignmentObj);
            Q_EMIT assignmentReceived(assignment);
            break;
        }
        case OrchestrationMessageType::AssignmentControl: {
            const QString deployment_id = message.value("deployment_id").toString();
            const QString job_id = message.value("job_id").toString();
            const QString action = message.value("action").toString().toLower();

            if (action == "pause") {
                Q_EMIT assignmentPaused(deployment_id, job_id);
            } else if (action == "resume") {
                Q_EMIT assignmentResumed(deployment_id, job_id);
            } else if (action == "cancel") {
                Q_EMIT assignmentCanceled(deployment_id, job_id);
            }
            break;
        }
        default:
            break;
    }
}

void OrchestrationClient::onReconnectTimeout() {
    if (m_lastHost.isNull() || m_lastPort == 0) {
        return;
    }

    if (m_socket->state() == QAbstractSocket::ConnectedState
        || m_socket->state() == QAbstractSocket::ConnectingState) {
        return;
    }

    Q_EMIT statusMessage(tr("Reconnecting to orchestrator..."));
    m_socket->connectToHost(m_lastHost, m_lastPort);
}

} // namespace sak

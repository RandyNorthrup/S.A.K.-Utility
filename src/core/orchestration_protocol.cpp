#include "sak/orchestration_protocol.h"

#include <QJsonDocument>
#include <QTcpSocket>

namespace sak {

QJsonObject OrchestrationProtocol::makeMessage(OrchestrationMessageType type, const QJsonObject& payload) {
    QJsonObject message = payload;
    message["message_type"] = typeToString(type);
    message["protocol_version"] = "1.0";
    return message;
}

std::optional<OrchestrationMessageType> OrchestrationProtocol::parseType(const QString& type) {
    if (type == "DESTINATION_REGISTER") return OrchestrationMessageType::DestinationRegister;
    if (type == "HEALTH_CHECK_REQUEST") return OrchestrationMessageType::HealthCheckRequest;
    if (type == "HEALTH_CHECK_RESPONSE") return OrchestrationMessageType::HealthCheckResponse;
    if (type == "DEPLOYMENT_ASSIGN") return OrchestrationMessageType::DeploymentAssign;
    if (type == "ASSIGNMENT_CONTROL") return OrchestrationMessageType::AssignmentControl;
    if (type == "START_TRANSFER") return OrchestrationMessageType::StartTransfer;
    if (type == "PROGRESS_UPDATE") return OrchestrationMessageType::ProgressUpdate;
    if (type == "DEPLOYMENT_COMPLETE") return OrchestrationMessageType::DeploymentComplete;
    if (type == "ERROR") return OrchestrationMessageType::Error;
    if (type == "HEARTBEAT") return OrchestrationMessageType::Heartbeat;
    return std::nullopt;
}

QString OrchestrationProtocol::typeToString(OrchestrationMessageType type) {
    switch (type) {
        case OrchestrationMessageType::DestinationRegister: return "DESTINATION_REGISTER";
        case OrchestrationMessageType::HealthCheckRequest: return "HEALTH_CHECK_REQUEST";
        case OrchestrationMessageType::HealthCheckResponse: return "HEALTH_CHECK_RESPONSE";
        case OrchestrationMessageType::DeploymentAssign: return "DEPLOYMENT_ASSIGN";
        case OrchestrationMessageType::AssignmentControl: return "ASSIGNMENT_CONTROL";
        case OrchestrationMessageType::StartTransfer: return "START_TRANSFER";
        case OrchestrationMessageType::ProgressUpdate: return "PROGRESS_UPDATE";
        case OrchestrationMessageType::DeploymentComplete: return "DEPLOYMENT_COMPLETE";
        case OrchestrationMessageType::Error: return "ERROR";
        case OrchestrationMessageType::Heartbeat: return "HEARTBEAT";
        default: return "UNKNOWN";
    }
}

QByteArray OrchestrationProtocol::encodeMessage(const QJsonObject& message) {
    QJsonDocument doc(message);
    QByteArray payload = doc.toJson(QJsonDocument::Compact);

    QByteArray framed;
    framed.resize(4);
    const auto length = static_cast<quint32>(payload.size());
    framed[0] = static_cast<char>((length >> 24) & 0xFF);
    framed[1] = static_cast<char>((length >> 16) & 0xFF);
    framed[2] = static_cast<char>((length >> 8) & 0xFF);
    framed[3] = static_cast<char>(length & 0xFF);
    framed.append(payload);
    return framed;
}

bool OrchestrationProtocol::writeMessage(QTcpSocket* socket, const QJsonObject& message) {
    if (!socket) {
        return false;
    }
    QByteArray framed = encodeMessage(message);
    const auto bytes_written = socket->write(framed);
    return bytes_written == framed.size();
}

QList<QJsonObject> OrchestrationProtocol::readMessages(QByteArray& buffer, const QByteArray& incoming) {
    buffer.append(incoming);
    QList<QJsonObject> messages;

    while (buffer.size() >= 4) {
        const quint32 length = (static_cast<quint8>(buffer[0]) << 24)
                             | (static_cast<quint8>(buffer[1]) << 16)
                             | (static_cast<quint8>(buffer[2]) << 8)
                             | static_cast<quint8>(buffer[3]);

        if (buffer.size() < 4 + static_cast<int>(length)) {
            break;
        }

        QByteArray payload = buffer.mid(4, length);
        buffer.remove(0, 4 + length);

        QJsonParseError error{};
        QJsonDocument doc = QJsonDocument::fromJson(payload, &error);
        if (error.error == QJsonParseError::NoError && doc.isObject()) {
            messages.append(doc.object());
        }
    }

    return messages;
}

} // namespace sak

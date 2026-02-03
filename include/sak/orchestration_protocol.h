#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <optional>

class QTcpSocket;

namespace sak {

enum class OrchestrationMessageType {
    DestinationRegister,
    HealthCheckRequest,
    HealthCheckResponse,
    DeploymentAssign,
    AssignmentControl,
    StartTransfer,
    ProgressUpdate,
    DeploymentComplete,
    Error,
    Heartbeat
};

class OrchestrationProtocol {
public:
    static QJsonObject makeMessage(OrchestrationMessageType type, const QJsonObject& payload = {});
    static std::optional<OrchestrationMessageType> parseType(const QString& type);
    static QString typeToString(OrchestrationMessageType type);

    static QByteArray encodeMessage(const QJsonObject& message);
    static bool writeMessage(QTcpSocket* socket, const QJsonObject& message);
    static QList<QJsonObject> readMessages(QByteArray& buffer, const QByteArray& incoming);
};

} // namespace sak

#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <optional>

class QTcpSocket;

namespace sak {

enum class TransferMessageType {
    Hello,
    AuthChallenge,
    AuthResponse,
    TransferManifest,
    TransferApprove,
    TransferReject,
    FileTransferStart,
    FileTransferAck,
    TransferComplete,
    Error,
    Heartbeat
};

class TransferProtocol {
public:
    static QJsonObject makeMessage(TransferMessageType type, const QJsonObject& payload = {});
    static std::optional<TransferMessageType> parseType(const QString& type);
    static QString typeToString(TransferMessageType type);

    static QByteArray encodeMessage(const QJsonObject& message);
    static bool writeMessage(QTcpSocket* socket, const QJsonObject& message);
    static QList<QJsonObject> readMessages(QByteArray& buffer, const QByteArray& incoming);
};

} // namespace sak

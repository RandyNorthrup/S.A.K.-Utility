#include "sak/network_transfer_protocol.h"

#include <QJsonDocument>
#include <QTcpSocket>

namespace sak {

QJsonObject TransferProtocol::makeMessage(TransferMessageType type, const QJsonObject& payload) {
    QJsonObject message = payload;
    message["message_type"] = typeToString(type);
    message["protocol_version"] = "1.0";
    return message;
}

std::optional<TransferMessageType> TransferProtocol::parseType(const QString& type) {
    if (type == "HELLO") return TransferMessageType::Hello;
    if (type == "AUTH_CHALLENGE") return TransferMessageType::AuthChallenge;
    if (type == "AUTH_RESPONSE") return TransferMessageType::AuthResponse;
    if (type == "TRANSFER_MANIFEST") return TransferMessageType::TransferManifest;
    if (type == "TRANSFER_APPROVE") return TransferMessageType::TransferApprove;
    if (type == "TRANSFER_REJECT") return TransferMessageType::TransferReject;
    if (type == "FILE_TRANSFER_START") return TransferMessageType::FileTransferStart;
    if (type == "FILE_TRANSFER_ACK") return TransferMessageType::FileTransferAck;
    if (type == "TRANSFER_COMPLETE") return TransferMessageType::TransferComplete;
    if (type == "ERROR") return TransferMessageType::Error;
    if (type == "HEARTBEAT") return TransferMessageType::Heartbeat;
    return std::nullopt;
}

QString TransferProtocol::typeToString(TransferMessageType type) {
    switch (type) {
        case TransferMessageType::Hello: return "HELLO";
        case TransferMessageType::AuthChallenge: return "AUTH_CHALLENGE";
        case TransferMessageType::AuthResponse: return "AUTH_RESPONSE";
        case TransferMessageType::TransferManifest: return "TRANSFER_MANIFEST";
        case TransferMessageType::TransferApprove: return "TRANSFER_APPROVE";
        case TransferMessageType::TransferReject: return "TRANSFER_REJECT";
        case TransferMessageType::FileTransferStart: return "FILE_TRANSFER_START";
        case TransferMessageType::FileTransferAck: return "FILE_TRANSFER_ACK";
        case TransferMessageType::TransferComplete: return "TRANSFER_COMPLETE";
        case TransferMessageType::Error: return "ERROR";
        case TransferMessageType::Heartbeat: return "HEARTBEAT";
        default: return "UNKNOWN";
    }
}

QByteArray TransferProtocol::encodeMessage(const QJsonObject& message) {
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

bool TransferProtocol::writeMessage(QTcpSocket* socket, const QJsonObject& message) {
    if (!socket) {
        return false;
    }
    QByteArray framed = encodeMessage(message);
    const auto bytes_written = socket->write(framed);
    return bytes_written == framed.size();
}

QList<QJsonObject> TransferProtocol::readMessages(QByteArray& buffer, const QByteArray& incoming) {
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

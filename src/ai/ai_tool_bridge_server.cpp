// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_tool_bridge_server.h"

#include "sak/ai/openai_responses_client.h"
#include "sak/version.h"

#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRandomGenerator>
#include <QUuid>

namespace sak::ai {

namespace {

constexpr int kTokenBytes = 32;
constexpr int kMethodNotFound = -32'601;
constexpr int kMaxLineBytes = 16 * 1024 * 1024;
constexpr auto kDefaultMcpProtocol = "2025-06-18";

QString randomToken() {
    QByteArray bytes(kTokenBytes, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(bytes.data()),
                                          kTokenBytes / static_cast<int>(sizeof(quint32)));
    return QString::fromLatin1(bytes.toHex());
}

QJsonObject rpcResult(const QJsonValue& id, const QJsonObject& result) {
    return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                       {QStringLiteral("id"), id},
                       {QStringLiteral("result"), result}};
}

QJsonObject rpcError(const QJsonValue& id, int code, const QString& message) {
    return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                       {QStringLiteral("id"), id},
                       {QStringLiteral("error"),
                        QJsonObject{{QStringLiteral("code"), code},
                                    {QStringLiteral("message"), message}}}};
}

QJsonObject initializeResult(const QJsonObject& params) {
    const QString requested = params.value(QStringLiteral("protocolVersion")).toString();
    return QJsonObject{
        {QStringLiteral("protocolVersion"),
         requested.isEmpty() ? QString::fromLatin1(kDefaultMcpProtocol) : requested},
        {QStringLiteral("capabilities"),
         QJsonObject{
             {QStringLiteral("tools"), QJsonObject{{QStringLiteral("listChanged"), false}}}}},
        {QStringLiteral("serverInfo"),
         QJsonObject{{QStringLiteral("name"), QStringLiteral("sak-utility")},
                     {QStringLiteral("version"), QString::fromLatin1(get_version())}}}};
}

QJsonObject toolResult(const QString& output, bool is_error) {
    return QJsonObject{{QStringLiteral("content"),
                        QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                               {QStringLiteral("text"), output}}}},
                       {QStringLiteral("isError"), is_error}};
}

}  // namespace

AiToolBridgeServer::AiToolBridgeServer(QObject* parent)
    : QObject(parent), m_server(new QLocalServer(this)), m_token(randomToken()) {
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    connect(m_server, &QLocalServer::newConnection, this, &AiToolBridgeServer::acceptConnection);
}

AiToolBridgeServer::~AiToolBridgeServer() {
    stop();
}

bool AiToolBridgeServer::start(QString* error_message) {
    if (m_server->isListening()) {
        return true;
    }
    const QString name =
        QStringLiteral("sak-ai-tools-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    if (m_server->listen(name)) {
        return true;
    }
    if (error_message) {
        *error_message = m_server->errorString();
    }
    return false;
}

void AiToolBridgeServer::stop() {
    failPendingCalls(tr("S.A.K. tool bridge stopped"));
    m_server->close();
}

bool AiToolBridgeServer::isListening() const {
    return m_server->isListening();
}

QString AiToolBridgeServer::serverName() const {
    return m_server->fullServerName();
}

QJsonArray AiToolBridgeServer::mcpToolList() {
    QJsonArray tools;
    for (const auto& value : OpenAIResponsesClient::localToolDefinitionsForProviders()) {
        const QJsonObject tool = value.toObject();
        tools.append(
            QJsonObject{{QStringLiteral("name"), tool.value(QStringLiteral("name"))},
                        {QStringLiteral("description"), tool.value(QStringLiteral("description"))},
                        {QStringLiteral("inputSchema"), tool.value(QStringLiteral("parameters"))}});
    }
    return tools;
}

QJsonObject AiToolBridgeServer::handleMcpMessage(const QString& connection_id,
                                                 const QJsonObject& message) {
    const QString method = message.value(QStringLiteral("method")).toString();
    const QJsonValue id = message.value(QStringLiteral("id"));
    const QJsonObject params = message.value(QStringLiteral("params")).toObject();
    if (id.isUndefined() || id.isNull()) {
        return {};  // Notifications (initialized, cancelled) need no reply.
    }
    if (method == QLatin1String("initialize")) {
        return rpcResult(id, initializeResult(params));
    }
    if (method == QLatin1String("ping")) {
        return rpcResult(id, {});
    }
    if (method == QLatin1String("tools/list")) {
        return rpcResult(id, QJsonObject{{QStringLiteral("tools"), mcpToolList()}});
    }
    if (method != QLatin1String("tools/call")) {
        return rpcError(id, kMethodNotFound, QStringLiteral("Unsupported method: %1").arg(method));
    }
    const QString call_id = QStringLiteral("sak_mcp_%1_%2").arg(connection_id).arg(m_next_call++);
    m_pending.insert(call_id, PendingCall{m_dispatch_socket, id});
    const QJsonObject arguments = params.value(QStringLiteral("arguments")).toObject();
    Q_EMIT toolCallRequested(call_id,
                             params.value(QStringLiteral("name")).toString(),
                             QString::fromUtf8(
                                 QJsonDocument(arguments).toJson(QJsonDocument::Compact)));
    return {};
}

void AiToolBridgeServer::completeToolCall(const QString& call_id,
                                          const QString& output,
                                          bool is_error) {
    const PendingCall call = m_pending.take(call_id);
    if (call.socket) {
        writeMessage(call.socket, rpcResult(call.request_id, toolResult(output, is_error)));
    }
}

void AiToolBridgeServer::failPendingCalls(const QString& reason) {
    const auto pending = m_pending.keys();
    for (const auto& call_id : pending) {
        completeToolCall(call_id, reason, true);
    }
}

void AiToolBridgeServer::acceptConnection() {
    while (QLocalSocket* socket = m_server->nextPendingConnection()) {
        socket->setParent(this);
        m_authenticated.insert(socket, false);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() { readSocket(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket]() {
            m_buffers.remove(socket);
            m_authenticated.remove(socket);
            socket->deleteLater();
        });
    }
}

void AiToolBridgeServer::readSocket(QLocalSocket* socket) {
    QByteArray& buffer = m_buffers[socket];
    buffer.append(socket->readAll());
    qsizetype newline = buffer.indexOf('\n');
    while (newline >= 0) {
        const QByteArray line = buffer.left(newline).trimmed();
        buffer.remove(0, newline + 1);
        if (!line.isEmpty()) {
            handleLine(socket, line);
        }
        newline = buffer.indexOf('\n');
    }
    if (buffer.size() > kMaxLineBytes) {
        socket->abort();
    }
}

void AiToolBridgeServer::handleLine(QLocalSocket* socket, const QByteArray& line) {
    const QJsonObject message = QJsonDocument::fromJson(line).object();
    if (!m_authenticated.value(socket, false)) {
        // The first line must carry the per-session token handed to the relay.
        if (message.value(QStringLiteral("sak_bridge_token")).toString() != m_token) {
            socket->abort();
            return;
        }
        m_authenticated.insert(socket, true);
        return;
    }
    const QString connection_id = QString::number(reinterpret_cast<quintptr>(socket), 16);
    m_dispatch_socket = socket;  // tools/call replies are bound to this socket.
    const QJsonObject reply = handleMcpMessage(connection_id, message);
    m_dispatch_socket.clear();
    if (!reply.isEmpty()) {
        writeMessage(socket, reply);
    }
}

void AiToolBridgeServer::writeMessage(QLocalSocket* socket, const QJsonObject& message) {
    QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact);
    line.append('\n');
    socket->write(line);
    socket->flush();
}

}  // namespace sak::ai

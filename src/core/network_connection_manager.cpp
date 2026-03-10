// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/network_connection_manager.h"

#include "sak/logger.h"

#include <QtGlobal>

namespace sak {

NetworkConnectionManager::NetworkConnectionManager(QObject* parent)
    : QObject(parent), m_server(new QTcpServer(this)) {
    connect(m_server, &QTcpServer::newConnection, this, &NetworkConnectionManager::onNewConnection);
}

void NetworkConnectionManager::startServer(quint16 port) {
    Q_ASSERT(m_server);
    if (m_server->isListening()) {
        return;
    }

    if (!m_server->listen(QHostAddress::AnyIPv4, port)) {
        logError("NetworkConnectionManager listen failed on port {}: {}",
                 port,
                 m_server->errorString().toStdString());
        Q_EMIT connectionError(tr("Failed to listen on port %1").arg(port));
        return;
    }

    logInfo("NetworkConnectionManager listening on port {}", port);
}

void NetworkConnectionManager::stopServer() {
    if (m_server->isListening()) {
        m_server->close();
        logInfo("NetworkConnectionManager stopped server");
    }
}

void NetworkConnectionManager::connectToHost(const QHostAddress& host, quint16 port) {
    Q_ASSERT(m_socket);
    if (m_socket) {
        m_socket->disconnectFromHost();
        m_socket->deleteLater();
        m_socket = nullptr;
    }

    m_socket = new QTcpSocket(this);
    connect(m_socket, &QTcpSocket::connected, this, &NetworkConnectionManager::connected);
    connect(
        m_socket, &QTcpSocket::disconnected, this, &NetworkConnectionManager::onSocketDisconnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &NetworkConnectionManager::onSocketReadyRead);
    connect(m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        Q_EMIT connectionError(m_socket->errorString());
    });

    logInfo("NetworkConnectionManager connecting to {}:{}", host.toString().toStdString(), port);
    m_socket->connectToHost(host, port);
}

void NetworkConnectionManager::disconnectFromHost() {
    if (m_socket) {
        m_socket->disconnectFromHost();
    }
}

bool NetworkConnectionManager::isServerRunning() const {
    return m_server->isListening();
}

QTcpSocket* NetworkConnectionManager::socket() const {
    return m_socket;
}

quint16 NetworkConnectionManager::serverPort() const {
    return m_server->serverPort();
}

void NetworkConnectionManager::onNewConnection() {
    Q_ASSERT(m_socket);
    Q_ASSERT(m_server);
    if (m_socket) {
        // Disconnect all signals from old socket before replacing it to
        // prevent stale callbacks from firing after the socket is deleted.
        disconnect(m_socket, nullptr, this, nullptr);
        m_socket->disconnectFromHost();
        m_socket->deleteLater();
        m_socket = nullptr;
    }

    m_socket = m_server->nextPendingConnection();
    connect(m_socket, &QTcpSocket::readyRead, this, &NetworkConnectionManager::onSocketReadyRead);
    connect(
        m_socket, &QTcpSocket::disconnected, this, &NetworkConnectionManager::onSocketDisconnected);
    connect(m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        // Capture a local copy of the error string — m_socket may be replaced by the
        // time this lambda executes in a queued connection.
        if (!m_socket) {
            logError("NetworkConnectionManager socket error on already-released socket");
            Q_EMIT connectionError(QStringLiteral("Socket error (socket released)"));
            return;
        }
        QString errorStr = m_socket->errorString();
        logError("NetworkConnectionManager socket error: {}", errorStr.toStdString());
        Q_EMIT connectionError(errorStr);
    });

    logInfo("NetworkConnectionManager accepted connection from {}:{}",
            m_socket->peerAddress().toString().toStdString(),
            m_socket->peerPort());

    Q_EMIT connected();
}

void NetworkConnectionManager::onSocketReadyRead() {
    if (!m_socket) {
        return;
    }
    Q_EMIT dataReceived(m_socket->readAll());
}

void NetworkConnectionManager::onSocketDisconnected() {
    logInfo("NetworkConnectionManager socket disconnected");
    Q_EMIT disconnected();
}

}  // namespace sak

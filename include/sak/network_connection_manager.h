// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>

namespace sak {

/// @brief Manages TCP server and client socket connections
class NetworkConnectionManager : public QObject {
    Q_OBJECT

public:
    explicit NetworkConnectionManager(QObject* parent = nullptr);

    void startServer(quint16 port);
    void stopServer();
    void connectToHost(const QHostAddress& host, quint16 port);
    void disconnectFromHost();

    bool isServerRunning() const;
    QTcpSocket* socket() const;
    quint16 serverPort() const;

Q_SIGNALS:
    void connected();
    void disconnected();
    void connectionError(const QString& message);
    void dataReceived(const QByteArray& data);

private Q_SLOTS:
    void onNewConnection();
    void onSocketReadyRead();
    void onSocketDisconnected();

private:
    QTcpServer* m_server{nullptr};
    QTcpSocket* m_socket{nullptr};
};

} // namespace sak

#include <QtTest/QtTest>

#include "sak/network_connection_manager.h"

using namespace sak;

class NetworkConnectionTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void connectLoopback();
};

void NetworkConnectionTests::connectLoopback() {
    NetworkConnectionManager server;
    NetworkConnectionManager client;

    QSignalSpy connectedSpy(&client, &NetworkConnectionManager::connected);
    QSignalSpy serverSpy(&server, &NetworkConnectionManager::connected);

    server.startServer(0);
    QVERIFY(server.isServerRunning());

    const quint16 targetPort = server.serverPort();
    QVERIFY(targetPort > 0);
    client.connectToHost(QHostAddress::LocalHost, targetPort);

    QTRY_VERIFY(connectedSpy.count() > 0);
    QTRY_VERIFY(serverSpy.count() > 0);
}

QTEST_MAIN(NetworkConnectionTests)
#include "test_network_connection.moc"

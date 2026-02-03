#include <QtTest/QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>

#include "sak/orchestration_client.h"
#include "sak/orchestration_protocol.h"

using namespace sak;

namespace {
quint16 pickFreePort() {
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    const quint16 port = server.serverPort();
    server.close();
    return port;
}
}

class OrchestrationClientTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void receivesAssignment();
    void receivesAssignmentControl();
    void autoReconnectsAfterDisconnect();
};

void OrchestrationClientTests::receivesAssignment() {
    qRegisterMetaType<DeploymentAssignment>("DeploymentAssignment");

    const quint16 port = pickFreePort();
    QVERIFY(port != 0);

    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, port));

    OrchestrationClient client;
    DestinationPC destination;
    destination.destination_id = "dest-1";
    destination.hostname = "TEST-PC";
    client.setDestinationInfo(destination);

    QSignalSpy assignmentSpy(&client, &OrchestrationClient::assignmentReceived);

    client.connectToServer(QHostAddress::LocalHost, port);
    QVERIFY(server.waitForNewConnection(5000));

    QTcpSocket* socket = server.nextPendingConnection();
    QVERIFY(socket != nullptr);

    DeploymentAssignment assignment;
    assignment.deployment_id = "deploy-1";
    assignment.source_user = "user";
    assignment.profile_size_bytes = 1024;

    QJsonObject payload;
    payload["assignment"] = assignment.toJson();
    OrchestrationProtocol::writeMessage(socket,
        OrchestrationProtocol::makeMessage(OrchestrationMessageType::DeploymentAssign, payload));

    QVERIFY(assignmentSpy.wait(3000));
    auto args = assignmentSpy.takeFirst();
    DeploymentAssignment received = args.at(0).value<DeploymentAssignment>();
    QCOMPARE(received.deployment_id, assignment.deployment_id);
}

void OrchestrationClientTests::autoReconnectsAfterDisconnect() {
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    OrchestrationClient client;
    DestinationPC destination;
    destination.destination_id = "dest-reconnect";
    destination.hostname = "TEST-PC";
    client.setDestinationInfo(destination);
    client.setAutoReconnectEnabled(true);
    client.connectToServer(QHostAddress::LocalHost, server.serverPort());

    QVERIFY(server.waitForNewConnection(3000));
    QTcpSocket* socket = server.nextPendingConnection();
    QVERIFY(socket != nullptr);
    socket->disconnectFromHost();
    socket->deleteLater();

    QVERIFY(server.waitForNewConnection(6000));
}

void OrchestrationClientTests::receivesAssignmentControl() {
    const quint16 port = pickFreePort();
    QVERIFY(port != 0);

    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, port));

    OrchestrationClient client;
    DestinationPC destination;
    destination.destination_id = "dest-ctl";
    destination.hostname = "TEST-PC";
    client.setDestinationInfo(destination);

    QSignalSpy pausedSpy(&client, &OrchestrationClient::assignmentPaused);
    QSignalSpy resumedSpy(&client, &OrchestrationClient::assignmentResumed);
    QSignalSpy canceledSpy(&client, &OrchestrationClient::assignmentCanceled);

    client.connectToServer(QHostAddress::LocalHost, port);
    QVERIFY(server.waitForNewConnection(5000));

    QTcpSocket* socket = server.nextPendingConnection();
    QVERIFY(socket != nullptr);

    auto sendControl = [&](const QString& action) {
        QJsonObject payload;
        payload["deployment_id"] = "deploy-ctl";
        payload["job_id"] = "job-ctl";
        payload["action"] = action;
        OrchestrationProtocol::writeMessage(socket,
            OrchestrationProtocol::makeMessage(OrchestrationMessageType::AssignmentControl, payload));
    };

    sendControl("pause");
    QVERIFY(pausedSpy.wait(3000));

    sendControl("resume");
    QVERIFY(resumedSpy.wait(3000));

    sendControl("cancel");
    QVERIFY(canceledSpy.wait(3000));
}

QTEST_MAIN(OrchestrationClientTests)
#include "test_orchestration_client.moc"

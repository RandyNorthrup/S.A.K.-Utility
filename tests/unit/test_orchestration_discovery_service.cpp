#include <QtTest/QtTest>
#include <QUdpSocket>
#include <QHostAddress>

#include "sak/orchestration_discovery_service.h"

using namespace sak;

namespace {
quint16 pickFreeUdpPort() {
    QUdpSocket socket;
    if (!socket.bind(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    const quint16 port = socket.localPort();
    socket.close();
    return port;
}
}

class OrchestrationDiscoveryServiceTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void destinationRespondsToDiscovery();
    void orchestratorReceivesAnnouncement();
};

void OrchestrationDiscoveryServiceTests::destinationRespondsToDiscovery() {
    const quint16 port = pickFreeUdpPort();
    QVERIFY(port != 0);

    OrchestrationDiscoveryService destination;
    DestinationPC info;
    info.destination_id = "dest-test";
    info.hostname = "DEST-TEST";
    info.control_port = 54322;
    info.data_port = 54323;
    info.status = "ready";
    destination.setDestinationInfo(info);
    destination.setPort(port);

    QSignalSpy orchSpy(&destination, &OrchestrationDiscoveryService::orchestratorDiscovered);
    destination.startAsDestination();

    QUdpSocket probeSocket;
    QVERIFY(probeSocket.bind(QHostAddress::LocalHost, 0));

    QJsonObject payload;
    payload["message_type"] = "ORCH_DISCOVERY";
    payload["protocol_version"] = "1.0";
    payload["orchestrator_port"] = 54322;

    QJsonDocument doc(payload);
    probeSocket.writeDatagram(doc.toJson(QJsonDocument::Compact), QHostAddress::LocalHost, port);

    QVERIFY(orchSpy.wait(2000));
    destination.stop();
}

void OrchestrationDiscoveryServiceTests::orchestratorReceivesAnnouncement() {
    const quint16 port = pickFreeUdpPort();
    QVERIFY(port != 0);

    OrchestrationDiscoveryService orchestrator;
    orchestrator.setPort(port);

    QSignalSpy destSpy(&orchestrator, &OrchestrationDiscoveryService::destinationDiscovered);
    orchestrator.startAsOrchestrator();

    OrchestrationDiscoveryService destination;
    DestinationPC info;
    info.destination_id = "dest-announce";
    info.hostname = "DEST-ANNOUNCE";
    info.control_port = 54322;
    info.data_port = 54323;
    info.status = "ready";
    destination.setDestinationInfo(info);
    destination.setPort(port);
    destination.startAsDestination();

    destination.sendDestinationAnnounceTo(QHostAddress::LocalHost, port);

    QVERIFY(destSpy.wait(2000));

    destination.stop();
    orchestrator.stop();
}

QTEST_MAIN(OrchestrationDiscoveryServiceTests)
#include "test_orchestration_discovery_service.moc"

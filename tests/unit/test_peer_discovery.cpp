#include <QtTest/QtTest>
#include <QNetworkInterface>

#include "sak/peer_discovery_service.h"

using namespace sak;

class PeerDiscoveryTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void discoverPeer();
};

void PeerDiscoveryTests::discoverPeer() {
    bool hasBroadcast = false;
    for (const auto& iface : QNetworkInterface::allInterfaces()) {
        for (const auto& entry : iface.addressEntries()) {
            if (!entry.broadcast().isNull()) {
                hasBroadcast = true;
                break;
            }
        }
        if (hasBroadcast) break;
    }

    if (!hasBroadcast) {
        QSKIP("No broadcast interface available");
    }

    PeerDiscoveryService a;
    PeerDiscoveryService b;

    TransferPeerInfo infoA;
    infoA.peer_id = "peer-a";
    infoA.hostname = "A";
    infoA.mode = "source";

    TransferPeerInfo infoB;
    infoB.peer_id = "peer-b";
    infoB.hostname = "B";
    infoB.mode = "destination";

    const quint16 port = 54321;
    a.setPort(port);
    b.setPort(port);
    a.setPeerInfo(infoA);
    b.setPeerInfo(infoB);

    QSignalSpy spyA(&a, &PeerDiscoveryService::peerDiscovered);
    QSignalSpy spyB(&b, &PeerDiscoveryService::peerDiscovered);

    a.start();
    b.start();

    QTRY_VERIFY(spyA.count() > 0 || spyB.count() > 0);
}

QTEST_MAIN(PeerDiscoveryTests)
#include "test_peer_discovery.moc"

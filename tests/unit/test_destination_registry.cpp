#include <QtTest/QtTest>

#include "sak/destination_registry.h"

using namespace sak;

class DestinationRegistryTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void registerAndUpdate();
    void readinessEvaluation();
};

void DestinationRegistryTests::registerAndUpdate() {
    DestinationRegistry registry;
    DestinationPC pc;
    pc.destination_id = "dest-1";
    pc.hostname = "TEST-PC";
    pc.ip_address = "192.168.1.10";

    QSignalSpy registeredSpy(&registry, &DestinationRegistry::destinationRegistered);
    QSignalSpy updatedSpy(&registry, &DestinationRegistry::destinationUpdated);

    registry.registerDestination(pc);
    QCOMPARE(registeredSpy.count(), 1);
    QCOMPARE(updatedSpy.count(), 0);

    DestinationHealth health;
    health.cpu_usage_percent = 10;
    registry.updateHealth(pc.destination_id, health);
    QCOMPARE(updatedSpy.count(), 1);

    auto destinations = registry.destinations();
    QCOMPARE(destinations.size(), 1);
    QCOMPARE(destinations[0].destination_id, pc.destination_id);
    QCOMPARE(destinations[0].health.cpu_usage_percent, 10);
}

void DestinationRegistryTests::readinessEvaluation() {
    DestinationPC pc;
    pc.destination_id = "dest-2";
    pc.hostname = "TEST-PC";
    pc.health.cpu_usage_percent = 10;
    pc.health.ram_usage_percent = 20;
    pc.health.free_disk_bytes = 1024 * 1024 * 1024;
    pc.health.sak_service_running = true;
    pc.health.admin_rights = true;

    QString reason;
    QVERIFY(DestinationRegistry::checkReadiness(pc, 512 * 1024 * 1024, &reason));
    QVERIFY(reason.isEmpty());

    pc.health.free_disk_bytes = 1;
    QVERIFY(!DestinationRegistry::checkReadiness(pc, 512 * 1024 * 1024, &reason));
    QVERIFY(!reason.isEmpty());
}

QTEST_MAIN(DestinationRegistryTests)
#include "test_destination_registry.moc"

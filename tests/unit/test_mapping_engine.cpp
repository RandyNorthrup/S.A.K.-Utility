#include <QtTest/QtTest>

#include "sak/mapping_engine.h"

#include <QTemporaryDir>

using namespace sak;

class MappingEngineTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void selectsLargestFree();
    void roundRobinRotates();
    void savesAndLoadsTemplate();
};

static DestinationPC makeReady(const QString& id, qint64 freeBytes) {
    DestinationPC pc;
    pc.destination_id = id;
    pc.hostname = id;
    pc.health.admin_rights = true;
    pc.health.sak_service_running = true;
    pc.health.free_disk_bytes = freeBytes;
    pc.health.cpu_usage_percent = 10;
    pc.health.ram_usage_percent = 10;
    return pc;
}

void MappingEngineTests::selectsLargestFree() {
    MappingEngine engine;
    engine.setStrategy(MappingEngine::Strategy::LargestFree);

    QVector<DestinationPC> destinations;
    destinations.append(makeReady("dest-a", 100));
    destinations.append(makeReady("dest-b", 200));

    DeploymentAssignment assignment;
    assignment.profile_size_bytes = 50;

    const QString selected = engine.selectDestination(assignment, destinations, {}, 50);
    QCOMPARE(selected, QString("dest-b"));
}

void MappingEngineTests::roundRobinRotates() {
    MappingEngine engine;
    engine.setStrategy(MappingEngine::Strategy::RoundRobin);

    QVector<DestinationPC> destinations;
    destinations.append(makeReady("dest-a", 100));
    destinations.append(makeReady("dest-b", 200));

    DeploymentAssignment assignment;
    assignment.profile_size_bytes = 10;

    const QString first = engine.selectDestination(assignment, destinations, {}, 10);
    const QString second = engine.selectDestination(assignment, destinations, {}, 10);

    QVERIFY(first != second);
    QVERIFY((first == "dest-a" && second == "dest-b") || (first == "dest-b" && second == "dest-a"));
}

void MappingEngineTests::savesAndLoadsTemplate() {
    MappingEngine engine;

    MappingEngine::SourceProfile source;
    source.username = "user";
    source.source_hostname = "SOURCE";
    source.source_ip = "192.168.1.10";
    source.profile_size_bytes = 2048;

    QVector<DestinationPC> destinations;
    destinations.append(makeReady("dest-a", 1024 * 1024));

    auto mapping = engine.createOneToMany(source, destinations);
    mapping.deployment_id = "deploy-1";

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString filePath = tempDir.path() + "/mapping.json";
    QVERIFY(engine.saveTemplate(mapping, filePath));

    auto loaded = engine.loadTemplate(filePath);
    QCOMPARE(loaded.deployment_id, mapping.deployment_id);
    QCOMPARE(loaded.sources.size(), 1);
    QCOMPARE(loaded.destinations.size(), 1);
    QCOMPARE(loaded.sources[0].username, mapping.sources[0].username);
}

QTEST_MAIN(MappingEngineTests)
#include "test_mapping_engine.moc"

#include <QtTest/QtTest>
#include <QTimer>

#include "sak/parallel_transfer_manager.h"

using namespace sak;

namespace {
MappingEngine::SourceProfile makeSource(const QString& name, qint64 size) {
    MappingEngine::SourceProfile source;
    source.username = name;
    source.source_hostname = "SOURCE";
    source.source_ip = "192.168.1.2";
    source.profile_size_bytes = size;
    return source;
}

DestinationPC makeDest(const QString& id) {
    DestinationPC dest;
    dest.destination_id = id;
    dest.hostname = id;
    dest.health.admin_rights = true;
    dest.health.sak_service_running = true;
    dest.health.free_disk_bytes = 1024 * 1024 * 1024;
    return dest;
}
}

class ParallelTransferManagerStressTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void handlesManyJobs();
};

void ParallelTransferManagerStressTests::handlesManyJobs() {
    ParallelTransferManager manager;
    manager.setMaxConcurrentTransfers(4);
    manager.setGlobalBandwidthLimit(200);

    MappingEngine::DeploymentMapping mapping;
    mapping.type = MappingEngine::MappingType::OneToMany;
    mapping.sources = {makeSource("user", 1024)};
    for (int i = 0; i < 12; ++i) {
        mapping.destinations.push_back(makeDest(QString("dest-%1").arg(i + 1)));
    }

    QStringList startedJobs;
    QSignalSpy completedSpy(&manager, &ParallelTransferManager::deploymentComplete);

    connect(&manager, &ParallelTransferManager::jobStartRequested, this,
        [&manager, &startedJobs](const QString& jobId, const MappingEngine::SourceProfile&, const DestinationPC&) {
            startedJobs.append(jobId);
            QTimer::singleShot(5, [&manager, jobId]() {
                manager.markJobComplete(jobId, true);
            });
        });

    manager.startDeployment(mapping);

    QVERIFY(completedSpy.wait(5000));
    QCOMPARE(manager.totalJobs(), 12);
    QCOMPARE(manager.completedJobs(), 12);
    QCOMPARE(manager.failedJobs(), 0);
    QCOMPARE(startedJobs.size(), 12);
}

QTEST_MAIN(ParallelTransferManagerStressTests)
#include "test_parallel_transfer_manager_stress.moc"

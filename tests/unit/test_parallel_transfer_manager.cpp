#include <QtTest/QtTest>

#include "sak/parallel_transfer_manager.h"

using namespace sak;

class ParallelTransferManagerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void startsJobsUpToConcurrency();
    void completesDeployment();
    void cancelsJob();
    void respectsPriorityQueue();
    void schedulesRetryBackoff();
    void broadcastsBandwidthUpdates();
    void allocatesBandwidthByPriority();
};

static MappingEngine::SourceProfile makeSource(const QString& name, qint64 size) {
    MappingEngine::SourceProfile source;
    source.username = name;
    source.source_hostname = "SOURCE";
    source.source_ip = "192.168.1.2";
    source.profile_size_bytes = size;
    return source;
}

static DestinationPC makeDest(const QString& id) {
    DestinationPC dest;
    dest.destination_id = id;
    dest.hostname = id;
    dest.health.admin_rights = true;
    dest.health.sak_service_running = true;
    dest.health.free_disk_bytes = 1024 * 1024 * 1024;
    return dest;
}

void ParallelTransferManagerTests::startsJobsUpToConcurrency() {
    ParallelTransferManager manager;
    manager.setMaxConcurrentTransfers(1);

    MappingEngine::DeploymentMapping mapping;
    mapping.type = MappingEngine::MappingType::OneToMany;
    mapping.sources = {makeSource("user", 1024)};
    mapping.destinations = {makeDest("dest-1"), makeDest("dest-2")};

    QSignalSpy startSpy(&manager, &ParallelTransferManager::jobStartRequested);

    manager.startDeployment(mapping);
    QCOMPARE(startSpy.count(), 1);
}

void ParallelTransferManagerTests::completesDeployment() {
    ParallelTransferManager manager;

    MappingEngine::DeploymentMapping mapping;
    mapping.type = MappingEngine::MappingType::OneToMany;
    mapping.sources = {makeSource("user", 1024)};
    mapping.destinations = {makeDest("dest-1")};

    QSignalSpy completeSpy(&manager, &ParallelTransferManager::deploymentComplete);
    QSignalSpy startSpy(&manager, &ParallelTransferManager::jobStartRequested);

    manager.startDeployment(mapping);
    QCOMPARE(startSpy.count(), 1);

    const auto args = startSpy.takeFirst();
    const auto jobId = args.at(0).toString();

    manager.markJobComplete(jobId, true);
    QCOMPARE(completeSpy.count(), 1);
}

void ParallelTransferManagerTests::cancelsJob() {
    ParallelTransferManager manager;

    MappingEngine::DeploymentMapping mapping;
    mapping.type = MappingEngine::MappingType::OneToMany;
    mapping.sources = {makeSource("user", 1024)};
    mapping.destinations = {makeDest("dest-1")};

    QSignalSpy startSpy(&manager, &ParallelTransferManager::jobStartRequested);
    QSignalSpy cancelSpy(&manager, &ParallelTransferManager::jobCancelRequested);

    manager.startDeployment(mapping);
    QCOMPARE(startSpy.count(), 1);

    const auto args = startSpy.takeFirst();
    const auto jobId = args.at(0).toString();
    manager.cancelJob(jobId);

    QCOMPARE(cancelSpy.count(), 1);
}

void ParallelTransferManagerTests::respectsPriorityQueue() {
    ParallelTransferManager manager;
    manager.setMaxConcurrentTransfers(1);

    MappingEngine::DeploymentMapping mapping;
    mapping.type = MappingEngine::MappingType::OneToMany;
    mapping.sources = {makeSource("user", 1024)};
    mapping.destinations = {makeDest("dest-1"), makeDest("dest-2"), makeDest("dest-3")};

    QSignalSpy startSpy(&manager, &ParallelTransferManager::jobStartRequested);
    manager.startDeployment(mapping);
    QVERIFY(startSpy.count() >= 1);

    const auto startedJobId = startSpy.takeFirst().at(0).toString();
    const auto jobs = manager.allJobs();
    QString criticalJobId;
    QString lowJobId;
    for (const auto& job : jobs) {
        if (job.job_id == startedJobId) {
            continue;
        }
        if (criticalJobId.isEmpty()) {
            criticalJobId = job.job_id;
        } else {
            lowJobId = job.job_id;
        }
    }

    QVERIFY(!criticalJobId.isEmpty());
    manager.setJobPriority(criticalJobId, ParallelTransferManager::JobPriority::Critical);
    if (!lowJobId.isEmpty()) {
        manager.setJobPriority(lowJobId, ParallelTransferManager::JobPriority::Low);
    }

    manager.markJobComplete(startedJobId, true);
    QTRY_VERIFY2(startSpy.count() >= 1, "Expected jobStartRequested after markJobComplete");
    const auto nextJobId = startSpy.takeFirst().at(0).toString();
    QCOMPARE(nextJobId, criticalJobId);
}

void ParallelTransferManagerTests::schedulesRetryBackoff() {
    ParallelTransferManager manager;
    manager.setRetryBackoff(50, 200);
    manager.setMaxConcurrentTransfers(1);

    MappingEngine::DeploymentMapping mapping;
    mapping.type = MappingEngine::MappingType::OneToMany;
    mapping.sources = {makeSource("user", 1024)};
    mapping.destinations = {makeDest("dest-1"), makeDest("dest-2")};

    QSignalSpy startSpy(&manager, &ParallelTransferManager::jobStartRequested);
    manager.startDeployment(mapping);
    QVERIFY(startSpy.count() >= 1);

    const auto startedJobId = startSpy.takeFirst().at(0).toString();
    manager.markJobComplete(startedJobId, false, "failed");

    const auto jobs = manager.allJobs();
    QString retryJobId;
    for (const auto& job : jobs) {
        if (job.job_id != startedJobId) {
            retryJobId = job.job_id;
            break;
        }
    }

    QVERIFY(!retryJobId.isEmpty());
    startSpy.clear();
    manager.retryJob(retryJobId);

    // Retry is delayed by backoff (base 50ms) — verify it eventually fires
    QTRY_VERIFY2_WITH_TIMEOUT(startSpy.count() >= 1, "Expected retried job to start after backoff", 2000);
}

void ParallelTransferManagerTests::broadcastsBandwidthUpdates() {
    ParallelTransferManager manager;
    manager.setGlobalBandwidthLimit(100);
    manager.setMaxConcurrentTransfers(2);

    MappingEngine::DeploymentMapping mapping;
    mapping.type = MappingEngine::MappingType::OneToMany;
    mapping.sources = {makeSource("user", 1024)};
    mapping.destinations = {makeDest("dest-1"), makeDest("dest-2")};

    QSignalSpy bandwidthSpy(&manager, &ParallelTransferManager::jobBandwidthUpdateRequested);
    manager.startDeployment(mapping);

    QTRY_VERIFY2_WITH_TIMEOUT(bandwidthSpy.count() >= 2, "Expected bandwidth updates for 2 jobs", 1000);
}

void ParallelTransferManagerTests::allocatesBandwidthByPriority() {
    ParallelTransferManager manager;
    manager.setGlobalBandwidthLimit(100);
    manager.setMaxConcurrentTransfers(2);

    MappingEngine::DeploymentMapping mapping;
    mapping.type = MappingEngine::MappingType::OneToMany;
    mapping.sources = {makeSource("user", 1024)};
    mapping.destinations = {makeDest("dest-1"), makeDest("dest-2")};

    manager.startDeployment(mapping);

    const auto jobs = manager.allJobs();
    QVERIFY(jobs.size() >= 2);

    const QString criticalJobId = jobs[0].job_id;
    const QString lowJobId = jobs[1].job_id;

    QSignalSpy bandwidthSpy(&manager, &ParallelTransferManager::jobBandwidthUpdateRequested);

    manager.setJobPriority(criticalJobId, ParallelTransferManager::JobPriority::Critical);
    manager.setJobPriority(lowJobId, ParallelTransferManager::JobPriority::Low);

    QTRY_VERIFY2_WITH_TIMEOUT(bandwidthSpy.count() >= 2, "Expected bandwidth rebalance after priority change", 1000);

    QMap<QString, int> lastKbps;
    for (const auto& args : bandwidthSpy) {
        const QString jobId = args.at(0).toString();
        const int kbps = args.at(1).toInt();
        lastKbps.insert(jobId, kbps);
    }

    QVERIFY(lastKbps.contains(criticalJobId));
    QVERIFY(lastKbps.contains(lowJobId));
    QVERIFY(lastKbps.value(criticalJobId) >= lastKbps.value(lowJobId));
}

QTEST_MAIN(ParallelTransferManagerTests)
#include "test_parallel_transfer_manager.moc"

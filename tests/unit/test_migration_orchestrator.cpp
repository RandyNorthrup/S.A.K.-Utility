#include <QtTest/QtTest>

#include "sak/migration_orchestrator.h"
#include "sak/orchestration_types.h"

using namespace sak;

class FakeOrchestrationServer : public OrchestrationServerInterface {
    Q_OBJECT

public:
    explicit FakeOrchestrationServer(QObject* parent = nullptr)
        : OrchestrationServerInterface(parent) {}

    bool start(quint16) override { return true; }
    void stop() override {}

    void sendHealthCheck(const QString& destination_id) override {
        sentHealthChecks.append(destination_id);
    }

    void sendDeploymentAssignment(const QString& destination_id, const DeploymentAssignment& assignment) override {
        sentAssignments.append({destination_id, assignment.deployment_id});
    }

    void sendAssignmentPause(const QString&, const QString&, const QString&) override {}
    void sendAssignmentResume(const QString&, const QString&, const QString&) override {}
    void sendAssignmentCancel(const QString&, const QString&, const QString&) override {}

    void emitProgress(const DeploymentProgress& progress) {
        Q_EMIT progressUpdated(progress);
    }

    void emitCompletion(const DeploymentCompletion& completion) {
        Q_EMIT deploymentCompleted(completion);
    }

    QStringList sentHealthChecks;
    QVector<QPair<QString, QString>> sentAssignments;
};

class MigrationOrchestratorTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void healthPollingSendsChecks();
    void rejectsDeploymentWhenNotReady();
    void acceptsDeploymentWhenReady();
    void aggregatesProgressUpdates();
    void sendsAssignmentsWhenReady();
    void autoAssignsQueuedDeployments();
    void queuesAssignmentsPerDestination();
};

void MigrationOrchestratorTests::healthPollingSendsChecks() {
    MigrationOrchestrator orchestrator;
    auto* fakeServer = new FakeOrchestrationServer(&orchestrator);
    orchestrator.setServer(fakeServer);

    DestinationPC pc;
    pc.destination_id = "dest-1";
    pc.hostname = "TEST-PC";
    orchestrator.registerDestination(pc);

    orchestrator.startHealthPolling(10);
    QTest::qWait(50);
    orchestrator.stopHealthPolling();

    QVERIFY(!fakeServer->sentHealthChecks.isEmpty());
    QVERIFY(fakeServer->sentHealthChecks.contains("dest-1"));
}

void MigrationOrchestratorTests::rejectsDeploymentWhenNotReady() {
    MigrationOrchestrator orchestrator;

    DestinationPC pc;
    pc.destination_id = "dest-2";
    pc.hostname = "LOW-SPACE";
    pc.health.admin_rights = true;
    pc.health.sak_service_running = true;
    pc.health.free_disk_bytes = 10;
    orchestrator.registerDestination(pc);

    QSignalSpy rejectedSpy(&orchestrator, &MigrationOrchestrator::deploymentRejected);

    DeploymentAssignment assignment;
    assignment.deployment_id = "deploy-1";
    assignment.source_user = "user";
    assignment.profile_size_bytes = 1024;

    orchestrator.deploymentManager()->enqueueForDestination(assignment, pc.destination_id, 1024);
    QCOMPARE(rejectedSpy.count(), 1);
}

void MigrationOrchestratorTests::acceptsDeploymentWhenReady() {
    MigrationOrchestrator orchestrator;

    DestinationPC pc;
    pc.destination_id = "dest-3";
    pc.hostname = "READY";
    pc.health.admin_rights = true;
    pc.health.sak_service_running = true;
    pc.health.free_disk_bytes = 1024 * 1024 * 1024;
    pc.health.cpu_usage_percent = 10;
    pc.health.ram_usage_percent = 20;
    orchestrator.registerDestination(pc);

    QSignalSpy readySpy(&orchestrator, &MigrationOrchestrator::deploymentReady);
    QSignalSpy rejectedSpy(&orchestrator, &MigrationOrchestrator::deploymentRejected);

    DeploymentAssignment assignment;
    assignment.deployment_id = "deploy-2";
    assignment.source_user = "user";
    assignment.profile_size_bytes = 1024;

    orchestrator.deploymentManager()->enqueueForDestination(assignment, pc.destination_id, 1024);
    QCOMPARE(rejectedSpy.count(), 0);
    QCOMPARE(readySpy.count(), 1);
}

void MigrationOrchestratorTests::aggregatesProgressUpdates() {
    MigrationOrchestrator orchestrator;
    auto* fakeServer = new FakeOrchestrationServer(&orchestrator);
    orchestrator.setServer(fakeServer);

    DestinationPC pc;
    pc.destination_id = "dest-4";
    pc.hostname = "READY";
    orchestrator.registerDestination(pc);

    QSignalSpy aggregateSpy(&orchestrator, &MigrationOrchestrator::aggregateProgress);

    DeploymentProgress progress;
    progress.deployment_id = "deploy-3";
    progress.destination_id = "dest-4";
    progress.progress_percent = 50;

    fakeServer->emitProgress(progress);
    QVERIFY(aggregateSpy.count() >= 1);

    auto args = aggregateSpy.takeFirst();
    QCOMPARE(args.at(1).toInt(), 1);
}

void MigrationOrchestratorTests::sendsAssignmentsWhenReady() {
    MigrationOrchestrator orchestrator;
    auto* fakeServer = new FakeOrchestrationServer(&orchestrator);
    orchestrator.setServer(fakeServer);

    DestinationPC pc;
    pc.destination_id = "dest-5";
    pc.hostname = "READY";
    pc.health.admin_rights = true;
    pc.health.sak_service_running = true;
    pc.health.free_disk_bytes = 1024 * 1024 * 1024;
    orchestrator.registerDestination(pc);

    DeploymentAssignment assignment;
    assignment.deployment_id = "deploy-4";
    assignment.source_user = "user";
    assignment.profile_size_bytes = 1024;

    orchestrator.assignDeploymentToDestination(pc.destination_id, assignment, 1024);
    QCOMPARE(fakeServer->sentAssignments.size(), 1);
    QCOMPARE(fakeServer->sentAssignments[0].first, pc.destination_id);
    QCOMPARE(fakeServer->sentAssignments[0].second, assignment.deployment_id);
}

void MigrationOrchestratorTests::autoAssignsQueuedDeployments() {
    MigrationOrchestrator orchestrator;
    auto* fakeServer = new FakeOrchestrationServer(&orchestrator);
    orchestrator.setServer(fakeServer);

    DeploymentAssignment assignment;
    assignment.deployment_id = "deploy-5";
    assignment.source_user = "user";
    assignment.profile_size_bytes = 1024;

    orchestrator.queueDeployment(assignment);

    DestinationPC pc;
    pc.destination_id = "dest-6";
    pc.hostname = "READY";
    pc.health.admin_rights = true;
    pc.health.sak_service_running = true;
    pc.health.free_disk_bytes = 1024 * 1024 * 1024;
    orchestrator.registerDestination(pc);

    QTest::qWait(20);

    QCOMPARE(fakeServer->sentAssignments.size(), 1);
    QCOMPARE(fakeServer->sentAssignments[0].first, pc.destination_id);
    QCOMPARE(fakeServer->sentAssignments[0].second, assignment.deployment_id);
}

void MigrationOrchestratorTests::queuesAssignmentsPerDestination() {
    MigrationOrchestrator orchestrator;
    auto* fakeServer = new FakeOrchestrationServer(&orchestrator);
    orchestrator.setServer(fakeServer);

    DestinationPC pc;
    pc.destination_id = "dest-7";
    pc.hostname = "READY";
    pc.health.admin_rights = true;
    pc.health.sak_service_running = true;
    pc.health.free_disk_bytes = 1024 * 1024 * 1024;
    orchestrator.registerDestination(pc);

    DeploymentAssignment first;
    first.deployment_id = "deploy-6";
    first.source_user = "user1";
    first.profile_size_bytes = 512;

    DeploymentAssignment second;
    second.deployment_id = "deploy-7";
    second.source_user = "user2";
    second.profile_size_bytes = 512;

    orchestrator.assignDeploymentToDestination(pc.destination_id, first, 512);
    orchestrator.assignDeploymentToDestination(pc.destination_id, second, 512);

    QCOMPARE(fakeServer->sentAssignments.size(), 1);
    QCOMPARE(fakeServer->sentAssignments[0].second, first.deployment_id);

    DeploymentCompletion completion;
    completion.deployment_id = first.deployment_id;
    completion.destination_id = pc.destination_id;
    completion.status = "success";
    fakeServer->emitCompletion(completion);

    QCOMPARE(fakeServer->sentAssignments.size(), 2);
    QCOMPARE(fakeServer->sentAssignments[1].second, second.deployment_id);
}

QTEST_MAIN(MigrationOrchestratorTests)
#include "test_migration_orchestrator.moc"

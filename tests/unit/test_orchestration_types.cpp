#include <QtTest/QtTest>

#include "sak/orchestration_types.h"

using namespace sak;

class OrchestrationTypesTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void assignmentSerialization();
    void progressSerialization();
    void completionSerialization();
};

void OrchestrationTypesTests::assignmentSerialization() {
    DeploymentAssignment assignment;
    assignment.deployment_id = "deploy-0";
    assignment.job_id = "job-0";
    assignment.source_user = "user";
    assignment.profile_size_bytes = 2048;
    assignment.priority = "high";
    assignment.max_bandwidth_kbps = 4096;

    auto json = assignment.toJson();
    auto roundtrip = DeploymentAssignment::fromJson(json);

    QCOMPARE(roundtrip.deployment_id, assignment.deployment_id);
    QCOMPARE(roundtrip.job_id, assignment.job_id);
    QCOMPARE(roundtrip.source_user, assignment.source_user);
    QCOMPARE(roundtrip.profile_size_bytes, assignment.profile_size_bytes);
    QCOMPARE(roundtrip.priority, assignment.priority);
    QCOMPARE(roundtrip.max_bandwidth_kbps, assignment.max_bandwidth_kbps);
}

void OrchestrationTypesTests::progressSerialization() {
    DeploymentProgress progress;
    progress.deployment_id = "deploy-1";
    progress.job_id = "job-1";
    progress.destination_id = "dest-1";
    progress.progress_percent = 42;
    progress.bytes_transferred = 1024;
    progress.bytes_total = 2048;
    progress.files_transferred = 10;
    progress.files_total = 20;
    progress.current_file = "C:/file.txt";
    progress.transfer_speed_mbps = 12.5;
    progress.eta_seconds = 30;

    auto json = progress.toJson();
    auto roundtrip = DeploymentProgress::fromJson(json);

    QCOMPARE(roundtrip.deployment_id, progress.deployment_id);
    QCOMPARE(roundtrip.job_id, progress.job_id);
    QCOMPARE(roundtrip.destination_id, progress.destination_id);
    QCOMPARE(roundtrip.progress_percent, progress.progress_percent);
    QCOMPARE(roundtrip.bytes_transferred, progress.bytes_transferred);
    QCOMPARE(roundtrip.bytes_total, progress.bytes_total);
    QCOMPARE(roundtrip.files_transferred, progress.files_transferred);
    QCOMPARE(roundtrip.files_total, progress.files_total);
    QCOMPARE(roundtrip.current_file, progress.current_file);
    QCOMPARE(roundtrip.transfer_speed_mbps, progress.transfer_speed_mbps);
    QCOMPARE(roundtrip.eta_seconds, progress.eta_seconds);
}

void OrchestrationTypesTests::completionSerialization() {
    DeploymentCompletion completion;
    completion.deployment_id = "deploy-2";
    completion.job_id = "job-2";
    completion.destination_id = "dest-2";
    completion.status = "success";
    completion.summary = QJsonObject{{"total_bytes", 123}};

    auto json = completion.toJson();
    auto roundtrip = DeploymentCompletion::fromJson(json);

    QCOMPARE(roundtrip.deployment_id, completion.deployment_id);
    QCOMPARE(roundtrip.job_id, completion.job_id);
    QCOMPARE(roundtrip.destination_id, completion.destination_id);
    QCOMPARE(roundtrip.status, completion.status);
    QCOMPARE(roundtrip.summary.value("total_bytes").toInt(), 123);
}

QTEST_MAIN(OrchestrationTypesTests)
#include "test_orchestration_types.moc"

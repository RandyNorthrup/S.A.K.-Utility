// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_orchestration_server.cpp
/// @brief Unit tests for OrchestrationServer

#include "sak/orchestration_server.h"
#include "sak/orchestration_types.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

using namespace sak;

class TestOrchestrationServer : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void construction_default();
    void construction_nonCopyable();
    void healthIntervalMs_defaultValue();
    void setHealthIntervalMs_roundTrip();
    void setHealthIntervalMs_variousValues();
    void start_succeeds();
    void start_emitsStatusMessage();
    void start_alreadyListening_returnsTrue();
    void stop_withoutStart();
    void stop_afterStart();
    void startStop_lifecycle();
    void sendHealthCheck_unknownDestination();
    void sendAssignmentPause_unknownDestination();
    void sendAssignmentResume_unknownDestination();
    void sendAssignmentCancel_unknownDestination();
    void destinationHealth_defaults();
    void destinationHealth_jsonRoundTrip();
    void destinationPC_defaults();
    void destinationPC_jsonRoundTrip();
    void deploymentAssignment_defaults();
    void deploymentAssignment_jsonRoundTrip();
};

// ======================================================================
// Construction
// ======================================================================

void TestOrchestrationServer::construction_default() {
    OrchestrationServer server;
    QVERIFY(dynamic_cast<QObject*>(&server) != nullptr);
    QCOMPARE(server.healthIntervalMs(), 10'000);
}

void TestOrchestrationServer::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<OrchestrationServer>);
    QVERIFY(!std::is_copy_assignable_v<OrchestrationServer>);
}

// ======================================================================
// Health interval
// ======================================================================

void TestOrchestrationServer::healthIntervalMs_defaultValue() {
    OrchestrationServer server;
    QCOMPARE(server.healthIntervalMs(), 10'000);
    QVERIFY(server.healthIntervalMs() > 0);
}

void TestOrchestrationServer::setHealthIntervalMs_roundTrip() {
    OrchestrationServer server;
    server.setHealthIntervalMs(5000);
    QCOMPARE(server.healthIntervalMs(), 5000);
}

void TestOrchestrationServer::setHealthIntervalMs_variousValues() {
    OrchestrationServer server;

    server.setHealthIntervalMs(1000);
    QCOMPARE(server.healthIntervalMs(), 1000);

    server.setHealthIntervalMs(30'000);
    QCOMPARE(server.healthIntervalMs(), 30'000);

    server.setHealthIntervalMs(100);
    QCOMPARE(server.healthIntervalMs(), 100);
}

// ======================================================================
// Start / Stop
// ======================================================================

void TestOrchestrationServer::start_succeeds() {
    OrchestrationServer server;
    const bool started = server.start(0);
    QVERIFY(started);
    server.stop();
}

void TestOrchestrationServer::start_emitsStatusMessage() {
    OrchestrationServer server;
    QSignalSpy status_spy(&server, &OrchestrationServer::statusMessage);
    QVERIFY(status_spy.isValid());

    const bool started = server.start(0);
    QVERIFY(started);
    QCOMPARE(status_spy.count(), 1);

    const auto message = status_spy.first().first().toString();
    QVERIFY(message.contains(QStringLiteral("listening")));

    server.stop();
}

void TestOrchestrationServer::start_alreadyListening_returnsTrue() {
    OrchestrationServer server;
    QVERIFY(server.start(0));
    QVERIFY(server.start(0));
    server.stop();
}

void TestOrchestrationServer::stop_withoutStart() {
    OrchestrationServer server;
    server.stop();
    QCOMPARE(server.healthIntervalMs(), 10'000);
}

void TestOrchestrationServer::stop_afterStart() {
    OrchestrationServer server;
    QVERIFY(server.start(0));
    server.stop();
    QCOMPARE(server.healthIntervalMs(), 10'000);
}

void TestOrchestrationServer::startStop_lifecycle() {
    OrchestrationServer server;
    QVERIFY(server.start(0));
    server.stop();
    QVERIFY(server.start(0));
    server.stop();
}

// ======================================================================
// Send methods with unknown destinations
// ======================================================================

void TestOrchestrationServer::sendHealthCheck_unknownDestination() {
    OrchestrationServer server;
    server.sendHealthCheck(QStringLiteral("nonexistent_id"));
    QCOMPARE(server.healthIntervalMs(), 10'000);
}

void TestOrchestrationServer::sendAssignmentPause_unknownDestination() {
    OrchestrationServer server;
    server.sendAssignmentPause(QStringLiteral("unknown"),
                               QStringLiteral("dep1"),
                               QStringLiteral("job1"));
    QCOMPARE(server.healthIntervalMs(), 10'000);
}

void TestOrchestrationServer::sendAssignmentResume_unknownDestination() {
    OrchestrationServer server;
    server.sendAssignmentResume(QStringLiteral("unknown"),
                                QStringLiteral("dep1"),
                                QStringLiteral("job1"));
    QCOMPARE(server.healthIntervalMs(), 10'000);
}

void TestOrchestrationServer::sendAssignmentCancel_unknownDestination() {
    OrchestrationServer server;
    server.sendAssignmentCancel(QStringLiteral("unknown"),
                                QStringLiteral("dep1"),
                                QStringLiteral("job1"));
    QCOMPARE(server.healthIntervalMs(), 10'000);
}

// ======================================================================
// DestinationHealth struct
// ======================================================================

void TestOrchestrationServer::destinationHealth_defaults() {
    DestinationHealth health;
    QCOMPARE(health.cpu_usage_percent, 0);
    QCOMPARE(health.ram_usage_percent, 0);
    QCOMPARE(health.free_disk_bytes, static_cast<qint64>(0));
    QCOMPARE(health.network_latency_ms, 0);
    QVERIFY(health.sak_service_running);
    QVERIFY(health.admin_rights);
}

void TestOrchestrationServer::destinationHealth_jsonRoundTrip() {
    DestinationHealth original;
    original.cpu_usage_percent = 75;
    original.ram_usage_percent = 60;
    original.free_disk_bytes = 1024 * 1024 * 1024;
    original.network_latency_ms = 42;
    original.sak_service_running = true;
    original.admin_rights = false;

    const auto json = original.toJson();
    const auto restored = DestinationHealth::fromJson(json);

    QCOMPARE(restored.cpu_usage_percent, original.cpu_usage_percent);
    QCOMPARE(restored.ram_usage_percent, original.ram_usage_percent);
    QCOMPARE(restored.free_disk_bytes, original.free_disk_bytes);
    QCOMPARE(restored.network_latency_ms, original.network_latency_ms);
    QCOMPARE(restored.sak_service_running, original.sak_service_running);
    QCOMPARE(restored.admin_rights, original.admin_rights);
}

// ======================================================================
// DestinationPC struct
// ======================================================================

void TestOrchestrationServer::destinationPC_defaults() {
    DestinationPC dest;
    QVERIFY(dest.destination_id.isEmpty());
    QVERIFY(dest.hostname.isEmpty());
    QVERIFY(dest.ip_address.isEmpty());
    QCOMPARE(dest.control_port, sak::kPortControl);
    QCOMPARE(dest.data_port, sak::kPortData);
    QCOMPARE(dest.status, QStringLiteral("unknown"));
    QVERIFY(!dest.last_seen.isValid());
}

void TestOrchestrationServer::destinationPC_jsonRoundTrip() {
    DestinationPC original;
    original.destination_id = QStringLiteral("dest-001");
    original.hostname = QStringLiteral("WORKSTATION-5");
    original.ip_address = QStringLiteral("192.168.1.100");
    original.control_port = 54'322;
    original.data_port = 54'323;
    original.status = QStringLiteral("online");

    const auto json = original.toJson();
    const auto restored = DestinationPC::fromJson(json);

    QCOMPARE(restored.destination_id, original.destination_id);
    QCOMPARE(restored.hostname, original.hostname);
    QCOMPARE(restored.ip_address, original.ip_address);
    QCOMPARE(restored.control_port, original.control_port);
    QCOMPARE(restored.data_port, original.data_port);
    QCOMPARE(restored.status, original.status);
}

// ======================================================================
// DeploymentAssignment struct
// ======================================================================

void TestOrchestrationServer::deploymentAssignment_defaults() {
    DeploymentAssignment assignment;
    QVERIFY(assignment.deployment_id.isEmpty());
    QVERIFY(assignment.job_id.isEmpty());
    QVERIFY(assignment.source_user.isEmpty());
    QCOMPARE(assignment.profile_size_bytes, static_cast<qint64>(0));
    QCOMPARE(assignment.priority, QStringLiteral("normal"));
    QCOMPARE(assignment.max_bandwidth_kbps, 0);
}

void TestOrchestrationServer::deploymentAssignment_jsonRoundTrip() {
    DeploymentAssignment original;
    original.deployment_id = QStringLiteral("deploy-42");
    original.job_id = QStringLiteral("job-007");
    original.source_user = QStringLiteral("admin");
    original.profile_size_bytes = 5'368'709'120;
    original.priority = QStringLiteral("high");
    original.max_bandwidth_kbps = 102'400;

    const auto json = original.toJson();
    const auto restored = DeploymentAssignment::fromJson(json);

    QCOMPARE(restored.deployment_id, original.deployment_id);
    QCOMPARE(restored.job_id, original.job_id);
    QCOMPARE(restored.source_user, original.source_user);
    QCOMPARE(restored.profile_size_bytes, original.profile_size_bytes);
    QCOMPARE(restored.priority, original.priority);
    QCOMPARE(restored.max_bandwidth_kbps, original.max_bandwidth_kbps);
}

QTEST_MAIN(TestOrchestrationServer)
#include "test_orchestration_server.moc"

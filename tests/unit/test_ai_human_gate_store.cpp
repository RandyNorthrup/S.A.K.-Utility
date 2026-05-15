// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_human_gate_store.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class AiHumanGateStoreTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void appendAndLoadRoundTrips();
    void latestPendingGateIgnoresResolvedGates();
};

void AiHumanGateStoreTests::appendAndLoadRoundTrips() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    sak::ai::AiHumanGateStore store(temp_dir.path());
    sak::ai::AiHumanGate gate;
    gate.gate_id = QStringLiteral("gate_approval_1");
    gate.run_id = QStringLiteral("run_1");
    gate.workflow_id = QStringLiteral("install_app_now");
    gate.phase_id = QStringLiteral("install");
    gate.kind = QStringLiteral("approval");
    gate.name = QStringLiteral("command_approval");
    gate.status = sak::ai::humanGateWaitingStatus();
    gate.question = QStringLiteral("Approve command?");
    gate.metadata[QStringLiteral("preview")] = QStringLiteral("choco install test");

    QString error;
    QVERIFY2(store.appendGate(gate, &error), qPrintable(error));

    const auto gates = store.loadGates(&error);
    QCOMPARE(gates.size(), 1);
    QCOMPARE(gates.first().gate_id, gate.gate_id);
    QCOMPARE(gates.first().status, sak::ai::humanGateWaitingStatus());
    QCOMPARE(gates.first().metadata.value(QStringLiteral("preview")).toString(),
             QStringLiteral("choco install test"));
    QVERIFY(QFile::exists(store.gateLogPath()));
}

void AiHumanGateStoreTests::latestPendingGateIgnoresResolvedGates() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    sak::ai::AiHumanGateStore store(temp_dir.path());

    sak::ai::AiHumanGate first;
    first.gate_id = QStringLiteral("gate_1");
    first.run_id = QStringLiteral("run_1");
    first.kind = QStringLiteral("workflow_input");
    first.name = QStringLiteral("required_input");
    first.status = sak::ai::humanGateWaitingStatus();
    first.question = QStringLiteral("Which app?");

    sak::ai::AiHumanGate first_resolved = first;
    first_resolved.status = sak::ai::humanGateCompletedStatus();
    first_resolved.decision = QStringLiteral("provided");
    first_resolved.response_summary = QStringLiteral("Firefox");

    sak::ai::AiHumanGate second;
    second.gate_id = QStringLiteral("gate_2");
    second.run_id = QStringLiteral("run_2");
    second.kind = QStringLiteral("approval");
    second.name = QStringLiteral("restore_point_offer");
    second.status = sak::ai::humanGateWaitingStatus();
    second.question = QStringLiteral("Create restore point?");

    QString error;
    QVERIFY2(store.appendGate(first, &error), qPrintable(error));
    QVERIFY2(store.appendGate(first_resolved, &error), qPrintable(error));
    QVERIFY2(store.appendGate(second, &error), qPrintable(error));

    const auto pending = store.latestPendingGate(&error);
    QCOMPARE(pending.gate_id, QStringLiteral("gate_2"));
    QCOMPARE(pending.question, QStringLiteral("Create restore point?"));
}

QTEST_GUILESS_MAIN(AiHumanGateStoreTests)
#include "test_ai_human_gate_store.moc"

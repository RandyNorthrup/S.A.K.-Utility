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
    void latestPendingGateIgnoresStatuslessForgedRecord();
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
    // Eight fields are populated above and three were inspected, so a round trip that dropped
    // workflow_id / phase_id / kind / name / question passed.
    const auto& loaded = gates.first();
    QCOMPARE(loaded.gate_id, QStringLiteral("gate_approval_1"));
    QCOMPARE(loaded.run_id, QStringLiteral("run_1"));
    QCOMPARE(loaded.workflow_id, QStringLiteral("install_app_now"));
    QCOMPARE(loaded.phase_id, QStringLiteral("install"));
    QCOMPARE(loaded.kind, QStringLiteral("approval"));
    QCOMPARE(loaded.name, QStringLiteral("command_approval"));
    // Pin the persisted WIRE value, not only the accessor: a human_gates.jsonl written by an
    // older build must keep parsing.
    QCOMPARE(loaded.status, QStringLiteral("waiting_for_human"));
    QCOMPARE(loaded.status, sak::ai::humanGateWaitingStatus());
    QCOMPARE(loaded.question, QStringLiteral("Approve command?"));
    QCOMPARE(loaded.decision, QString());
    QCOMPARE(loaded.response_summary, QString());
    QCOMPARE(loaded.metadata.size(), 1);
    QCOMPARE(loaded.metadata.value(QStringLiteral("preview")).toString(),
             QStringLiteral("choco install test"));
    // appendGate stamps created_utc and leaves resolved_utc unset while the gate is pending.
    QVERIFY(loaded.created_utc.isValid());
    QVERIFY(!loaded.resolved_utc.isValid());
    // The existence check was mirror-vacuous: the writer and the check used the same accessor,
    // so the log could have landed anywhere. Pin the NAME and the LOCATION.
    QCOMPARE(store.gateLogPath(), temp_dir.path() + QStringLiteral("/human_gates.jsonl"));
    QVERIFY(QFile::exists(temp_dir.path() + QStringLiteral("/human_gates.jsonl")));
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
    QCOMPARE(pending.run_id, QStringLiteral("run_2"));
    QCOMPARE(pending.kind, QStringLiteral("approval"));
    QCOMPARE(pending.name, QStringLiteral("restore_point_offer"));
    QCOMPARE(pending.status, sak::ai::humanGateWaitingStatus());
    QCOMPARE(pending.question, QStringLiteral("Create restore point?"));
    // The winner must be the PENDING record itself, not resolved state leaking through: a
    // resolved record carries decision / response_summary / resolved_utc.
    QCOMPARE(pending.decision, QString());
    QCOMPARE(pending.response_summary, QString());
    QVERIFY(!pending.resolved_utc.isValid());
    // All three records stay in the append-only log, in order. loadGates must not drop the
    // resolved one, or gate_1's stale pending record becomes its latest state and an
    // already-approved gate is re-raised on the next start.
    const auto all_records = store.loadGates(&error);
    QCOMPARE(all_records.size(), 3);
    QCOMPARE(all_records.at(0).gate_id, QStringLiteral("gate_1"));
    QCOMPARE(all_records.at(0).status, sak::ai::humanGateWaitingStatus());
    QCOMPARE(all_records.at(1).gate_id, QStringLiteral("gate_1"));
    QCOMPARE(all_records.at(1).status, sak::ai::humanGateCompletedStatus());
    QCOMPARE(all_records.at(1).decision, QStringLiteral("provided"));
    QCOMPARE(all_records.at(1).response_summary, QStringLiteral("Firefox"));
    QVERIFY(all_records.at(1).resolved_utc.isValid());
    QCOMPARE(all_records.at(2).gate_id, QStringLiteral("gate_2"));
}

void AiHumanGateStoreTests::latestPendingGateIgnoresStatuslessForgedRecord() {
    // A later record for the same gate_id whose status is unrecognized/statusless must NOT
    // resolve (erase) a genuine pending gate: it is malformed and is ignored, so the gate
    // stays pending. Regression for the forged-resolving-record hardening.
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());
    sak::ai::AiHumanGateStore store(temp_dir.path());

    sak::ai::AiHumanGate pending;
    pending.gate_id = QStringLiteral("gate_forge");
    pending.run_id = QStringLiteral("run_1");
    pending.kind = QStringLiteral("approval");
    pending.name = QStringLiteral("command_approval");
    pending.status = sak::ai::humanGateWaitingStatus();
    pending.question = QStringLiteral("Approve command?");

    sak::ai::AiHumanGate forged = pending;
    forged.status = QStringLiteral("bogus_state");  // not a known lifecycle status

    QString error;
    QVERIFY2(store.appendGate(pending, &error), qPrintable(error));
    QVERIFY2(store.appendGate(forged, &error), qPrintable(error));

    const auto latest = store.latestPendingGate(&error);
    QCOMPARE(latest.gate_id, QStringLiteral("gate_forge"));
    QVERIFY(latest.isPending());
    QCOMPARE(latest.status, sak::ai::humanGateWaitingStatus());
    QCOMPARE(latest.question, QStringLiteral("Approve command?"));
    QCOMPARE(latest.kind, QStringLiteral("approval"));
    QCOMPARE(latest.name, QStringLiteral("command_approval"));
    QCOMPARE(latest.run_id, QStringLiteral("run_1"));
    // "A pending gate survived" is reachable through THREE guards; this test is named for only
    // one of them (the known-status filter in latestPendingGate). The forged record must remain
    // in the log verbatim, so the test cannot pass merely because appendGate refused to write
    // it or the parser dropped it on load -- both of which would silently truncate the audit
    // history instead.
    const auto records = store.loadGates(&error);
    QCOMPARE(records.size(), 2);
    QCOMPARE(records.at(0).gate_id, QStringLiteral("gate_forge"));
    QCOMPARE(records.at(0).status, sak::ai::humanGateWaitingStatus());
    QCOMPARE(records.at(1).gate_id, QStringLiteral("gate_forge"));
    QCOMPARE(records.at(1).status, QStringLiteral("bogus_state"));
}

QTEST_GUILESS_MAIN(AiHumanGateStoreTests)
#include "test_ai_human_gate_store.moc"

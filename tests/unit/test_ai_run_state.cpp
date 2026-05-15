// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_run_state.h"

#include <QtTest/QtTest>

class AiRunStateTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void statusStringsRoundTrip();
    void runStateJsonRoundTrip();
    void terminalStateDetection();
};

void AiRunStateTests::statusStringsRoundTrip() {
    QCOMPARE(sak::ai::runStatusToString(sak::ai::AiRunStatus::WaitingForHuman),
             QStringLiteral("waiting_for_human"));
    QCOMPARE(sak::ai::runStatusFromString(QStringLiteral("cancelled")),
             sak::ai::AiRunStatus::Cancelled);
    QCOMPARE(sak::ai::runStatusFromString(QStringLiteral("unknown")), sak::ai::AiRunStatus::Idle);
}

void AiRunStateTests::runStateJsonRoundTrip() {
    sak::ai::AiRunState state;
    state.run_id = QStringLiteral("run_1");
    state.workflow_id = QStringLiteral("drive_health_deep_check");
    state.status = sak::ai::AiRunStatus::Running;
    state.phase_id = QStringLiteral("collect_evidence");
    state.active_subagents = 2;
    state.completed_tools = 4;
    state.message = QStringLiteral("Collecting disk evidence");
    state.has_pending_human_gate = true;
    state.pending_human_gate.gate_id = QStringLiteral("gate_1");
    state.pending_human_gate.run_id = state.run_id;
    state.pending_human_gate.kind = QStringLiteral("approval");
    state.pending_human_gate.name = QStringLiteral("command_approval");
    state.pending_human_gate.status = sak::ai::humanGateWaitingStatus();
    state.pending_human_gate.question = QStringLiteral("Approve command?");

    const auto copy = sak::ai::AiRunState::fromJson(state.toJson());

    QCOMPARE(copy.run_id, state.run_id);
    QCOMPARE(copy.workflow_id, state.workflow_id);
    QCOMPARE(copy.status, state.status);
    QCOMPARE(copy.phase_id, state.phase_id);
    QCOMPARE(copy.active_subagents, 2);
    QCOMPARE(copy.completed_tools, 4);
    QVERIFY(copy.has_pending_human_gate);
    QCOMPARE(copy.pending_human_gate.gate_id, QStringLiteral("gate_1"));
    QCOMPARE(copy.pending_human_gate.question, QStringLiteral("Approve command?"));
}

void AiRunStateTests::terminalStateDetection() {
    QVERIFY(sak::ai::isTerminalRunStatus(sak::ai::AiRunStatus::Completed));
    QVERIFY(sak::ai::isTerminalRunStatus(sak::ai::AiRunStatus::Failed));
    QVERIFY(sak::ai::isTerminalRunStatus(sak::ai::AiRunStatus::Cancelled));
    QVERIFY(!sak::ai::isTerminalRunStatus(sak::ai::AiRunStatus::Running));
}

QTEST_GUILESS_MAIN(AiRunStateTests)
#include "test_ai_run_state.moc"

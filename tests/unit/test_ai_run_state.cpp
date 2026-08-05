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
    void panelBusyCountsEveryInFlightSource();
    void stopStatusStaysCancellingWhileDetachedToolRuns();
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

void AiRunStateTests::panelBusyCountsEveryInFlightSource() {
    // R5 p11_gui-5: the panel's busy predicate omitted the async built-in tool runner, so a
    // blocking install/recipe that was still executing reported idle. Every field must count.
    QVERIFY(!sak::ai::aiPanelIsBusy(sak::ai::AiPanelActivity{}));

    sak::ai::AiPanelActivity activity;
    activity.client_busy = true;
    QVERIFY(sak::ai::aiPanelIsBusy(activity));

    activity = {};
    activity.tool_turn_active = true;
    QVERIFY(sak::ai::aiPanelIsBusy(activity));

    activity = {};
    activity.workflow_run_active = true;
    QVERIFY(sak::ai::aiPanelIsBusy(activity));

    activity = {};
    activity.execution_broker_running = true;
    QVERIFY(sak::ai::aiPanelIsBusy(activity));

    activity = {};
    activity.offline_worker_running = true;
    QVERIFY(sak::ai::aiPanelIsBusy(activity));

    // The one the finding was about: everything else has stopped, but the detached async
    // tool is still mutating the machine, so the panel is NOT idle.
    activity = {};
    activity.async_tool_runner_running = true;
    QVERIFY(sak::ai::aiPanelIsBusy(activity));
}

void AiRunStateTests::stopStatusStaysCancellingWhileDetachedToolRuns() {
    // Stop resolves to Cancelled only when nothing is in flight. Reporting Cancelled while a
    // detached blocking tool still runs is what let the UI accept new work over a live
    // mutation, so the drain-in-progress case must stay Cancelling.
    sak::ai::AiPanelActivity draining;
    draining.async_tool_runner_running = true;
    QCOMPARE(sak::ai::aiStopRunStatus(draining), sak::ai::AiRunStatus::Cancelling);

    QCOMPARE(sak::ai::aiStopRunStatus(sak::ai::AiPanelActivity{}), sak::ai::AiRunStatus::Cancelled);
}

QTEST_GUILESS_MAIN(AiRunStateTests)
#include "test_ai_run_state.moc"

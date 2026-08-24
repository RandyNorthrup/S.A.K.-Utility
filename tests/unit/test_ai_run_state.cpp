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
    // The status string is the persisted wire form of the run snapshot: every enum value
    // must map to its own literal and parse back, or a run reloads as a different run.
    struct StatusRow {
        sak::ai::AiRunStatus status;
        const char* text;
    };
    static constexpr StatusRow kStatusTable[] = {
        {sak::ai::AiRunStatus::Idle, "idle"},
        {sak::ai::AiRunStatus::Planning, "planning"},
        {sak::ai::AiRunStatus::Running, "running"},
        {sak::ai::AiRunStatus::WaitingForHuman, "waiting_for_human"},
        {sak::ai::AiRunStatus::Cancelling, "cancelling"},
        {sak::ai::AiRunStatus::Cancelled, "cancelled"},
        {sak::ai::AiRunStatus::Completed, "completed"},
        {sak::ai::AiRunStatus::Failed, "failed"},
    };
    for (const StatusRow& row : kStatusTable) {
        const QString text = QString::fromLatin1(row.text);
        QCOMPARE(sak::ai::runStatusToString(row.status), text);
        QCOMPARE(sak::ai::runStatusFromString(text), row.status);
    }
    // The snapshot file is attacker-writable, so the parser normalizes before matching: a
    // padded/upper-cased status must still resolve instead of degrading to Idle.
    QCOMPARE(sak::ai::runStatusFromString(QStringLiteral("  CANCELLED  ")),
             sak::ai::AiRunStatus::Cancelled);
    QCOMPARE(sak::ai::runStatusFromString(QStringLiteral("unknown")), sak::ai::AiRunStatus::Idle);
    QCOMPARE(sak::ai::runStatusFromString(QString()), sak::ai::AiRunStatus::Idle);
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

    QCOMPARE(copy.run_id, QStringLiteral("run_1"));
    QCOMPARE(copy.workflow_id, QStringLiteral("drive_health_deep_check"));
    QCOMPARE(copy.status, sak::ai::AiRunStatus::Running);
    QCOMPARE(copy.phase_id, QStringLiteral("collect_evidence"));
    QCOMPARE(copy.active_subagents, 2);
    QCOMPARE(copy.completed_tools, 4);
    QCOMPARE(copy.message, QStringLiteral("Collecting disk evidence"));
    QVERIFY(copy.has_pending_human_gate);
    // The gate is the approval record the run is blocked on: identity, kind, name and
    // lifecycle status all have to survive, not just the id and the prompt text.
    QCOMPARE(copy.pending_human_gate.gate_id, QStringLiteral("gate_1"));
    QCOMPARE(copy.pending_human_gate.run_id, QStringLiteral("run_1"));
    QCOMPARE(copy.pending_human_gate.kind, QStringLiteral("approval"));
    QCOMPARE(copy.pending_human_gate.name, QStringLiteral("command_approval"));
    QCOMPARE(copy.pending_human_gate.status, QStringLiteral("waiting_for_human"));
    QVERIFY(copy.pending_human_gate.isPending());
    QCOMPARE(copy.pending_human_gate.question, QStringLiteral("Approve command?"));

    // Counters left at their defaults would survive a dropped key by reading back as 0,
    // so carry distinct values through the same round trip.
    sak::ai::AiRunState counted = state;
    counted.completed_subagents = 7;
    counted.active_tools = 3;
    const auto counted_copy = sak::ai::AiRunState::fromJson(counted.toJson());
    QCOMPARE(counted_copy.completed_subagents, 7);
    QCOMPARE(counted_copy.active_tools, 3);
}

void AiRunStateTests::terminalStateDetection() {
    QVERIFY(sak::ai::isTerminalRunStatus(sak::ai::AiRunStatus::Completed));
    QVERIFY(sak::ai::isTerminalRunStatus(sak::ai::AiRunStatus::Failed));
    QVERIFY(sak::ai::isTerminalRunStatus(sak::ai::AiRunStatus::Cancelled));
    // Exactly three statuses are terminal. Every other one is a run still in motion --
    // Cancelling above all, which is a run still draining a live mutation.
    QVERIFY(!sak::ai::isTerminalRunStatus(sak::ai::AiRunStatus::Idle));
    QVERIFY(!sak::ai::isTerminalRunStatus(sak::ai::AiRunStatus::Planning));
    QVERIFY(!sak::ai::isTerminalRunStatus(sak::ai::AiRunStatus::Running));
    QVERIFY(!sak::ai::isTerminalRunStatus(sak::ai::AiRunStatus::WaitingForHuman));
    QVERIFY(!sak::ai::isTerminalRunStatus(sak::ai::AiRunStatus::Cancelling));

    // AiRunState::isTerminal must be the same verdict, not a second opinion.
    sak::ai::AiRunState state;
    state.status = sak::ai::AiRunStatus::Cancelling;
    QVERIFY(!state.isTerminal());
    state.status = sak::ai::AiRunStatus::Failed;
    QVERIFY(state.isTerminal());
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

    // Stop reports the busy predicate's verdict, not a single-flag proxy: EVERY tracked
    // source must hold the run at Cancelling, or the panel persists "Cancelled" while
    // that source is still executing.
    sak::ai::AiPanelActivity one;
    one.client_busy = true;
    QCOMPARE(sak::ai::aiStopRunStatus(one), sak::ai::AiRunStatus::Cancelling);
    one = {};
    one.tool_turn_active = true;
    QCOMPARE(sak::ai::aiStopRunStatus(one), sak::ai::AiRunStatus::Cancelling);
    one = {};
    one.workflow_run_active = true;
    QCOMPARE(sak::ai::aiStopRunStatus(one), sak::ai::AiRunStatus::Cancelling);
    one = {};
    one.execution_broker_running = true;
    QCOMPARE(sak::ai::aiStopRunStatus(one), sak::ai::AiRunStatus::Cancelling);
    one = {};
    one.offline_worker_running = true;
    QCOMPARE(sak::ai::aiStopRunStatus(one), sak::ai::AiRunStatus::Cancelling);

    QCOMPARE(sak::ai::aiStopRunStatus(sak::ai::AiPanelActivity{}), sak::ai::AiRunStatus::Cancelled);
}

QTEST_GUILESS_MAIN(AiRunStateTests)
#include "test_ai_run_state.moc"

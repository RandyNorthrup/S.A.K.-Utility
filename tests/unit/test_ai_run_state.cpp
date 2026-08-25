// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_run_state.h"

#include <QtTest/QtTest>

class AiRunStateTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void statusStringsRoundTrip();
    void runStateJsonRoundTrip();
    // The attacker-writable-snapshot guards no fixture ever reached: the counter clamps and the
    // identity-less pending-gate fail-closed arm.
    void fromJsonClampsNegativeCountersAndFailsClosedOnAnIdentitylessGate();
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
    // NEAR MISSES. "unknown" shares no prefix, no suffix and no substring with any accepted
    // token, so it is refused by a compare loosened in ANY direction; the table above catches
    // only a truncating compare (which would collide cancelling/cancelled). The cancelling vs
    // cancelled pair is the one that matters -- mis-resolving one for the other is the difference
    // between "still draining a live mutation" and "safe to accept new work" -- and this field
    // arrives from an attacker-writable snapshot file.
    const QStringList near_misses{
        QStringLiteral("cancel"),       // truncation shared by both cancel* statuses
        QStringLiteral("cancellin"),    // truncation of cancelling
        QStringLiteral("cancelledx"),   // extension
        QStringLiteral("xcancelled"),   // embedding
        QStringLiteral("run"),          // truncation of running
        QStringLiteral("runningx"),     // extension
        QStringLiteral("waiting"),      // truncation of waiting_for_human
        QStringLiteral("waiting_for"),  // deeper truncation
        QStringLiteral("complete"),     // truncation of completed
        QStringLiteral("fail"),         // truncation of failed
    };
    for (const QString& token : near_misses) {
        QVERIFY2(sak::ai::runStatusFromString(token) == sak::ai::AiRunStatus::Idle,
                 qPrintable(QStringLiteral("'%1' resolved to a real status").arg(token)));
    }
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

    // The PERSISTED KEY NAMES. Every assertion above goes through toJson() immediately followed
    // by fromJson(), so any key renamed on BOTH halves is invisible -- yet this object is the
    // on-disk wire form of the run snapshot, and an older build's file must keep parsing. Only
    // the status ENCODING was pinned literally; its key name and all ten siblings were not.
    const QJsonObject wire = state.toJson();
    const QStringList wire_keys = wire.keys();
    const QSet<QString> expected_keys{QStringLiteral("run_id"),
                                      QStringLiteral("workflow_id"),
                                      QStringLiteral("status"),
                                      QStringLiteral("phase_id"),
                                      QStringLiteral("active_subagents"),
                                      QStringLiteral("completed_subagents"),
                                      QStringLiteral("active_tools"),
                                      QStringLiteral("completed_tools"),
                                      QStringLiteral("message"),
                                      QStringLiteral("has_pending_human_gate"),
                                      QStringLiteral("pending_human_gate")};
    QCOMPARE(QSet<QString>(wire_keys.begin(), wire_keys.end()), expected_keys);
    QCOMPARE(wire.value(QStringLiteral("run_id")).toString(), QStringLiteral("run_1"));
    QCOMPARE(wire.value(QStringLiteral("status")).toString(), QStringLiteral("running"));
    QCOMPARE(wire.value(QStringLiteral("active_subagents")).toInt(), 2);
    QCOMPARE(wire.value(QStringLiteral("completed_tools")).toInt(), 4);
    QVERIFY(wire.value(QStringLiteral("has_pending_human_gate")).toBool());
}

void AiRunStateTests::fromJsonClampsNegativeCountersAndFailsClosedOnAnIdentitylessGate() {
    // Both halves of this test reach guards that NO fixture in the tree ever entered, because
    // every value pushed through fromJson so far has been well-formed. The snapshot file is
    // attacker-writable, which is the whole reason those guards exist.

    // 1. The four std::max(0, ...) clamps. Every count any test has ever supplied is
    //    non-negative, so the clamp was an identity on every input the suite produced -- never
    //    once observed doing anything. A negative count renders nonsensically and can drive a
    //    later decrement below zero.
    QJsonObject negative;
    negative[QStringLiteral("run_id")] = QStringLiteral("run_neg");
    negative[QStringLiteral("status")] = QStringLiteral("running");
    negative[QStringLiteral("active_subagents")] = -1;
    negative[QStringLiteral("completed_subagents")] = -7;
    negative[QStringLiteral("active_tools")] = -2;
    negative[QStringLiteral("completed_tools")] = -1000;
    const auto clamped = sak::ai::AiRunState::fromJson(negative);
    QCOMPARE(clamped.active_subagents, 0);
    QCOMPARE(clamped.completed_subagents, 0);
    QCOMPARE(clamped.active_tools, 0);
    QCOMPARE(clamped.completed_tools, 0);

    // 2. The fail-closed arm for a gate payload with no identity. Normal serialization never
    //    emits has_pending_human_gate with an empty gate_id, so a snapshot that does is corrupt
    //    or tampered: clearing the flag would silently drop a genuine approval and let the run
    //    resume UNATTENDED, so the run is forced to WaitingForHuman and stays blocked. The only
    //    fixture that sets the flag carries a real gate_id, so this arm was entered by nothing.
    QJsonObject identityless;
    identityless[QStringLiteral("run_id")] = QStringLiteral("run_tamper");
    identityless[QStringLiteral("status")] = QStringLiteral("running");
    identityless[QStringLiteral("has_pending_human_gate")] = true;
    identityless[QStringLiteral("pending_human_gate")] = QJsonObject{};
    const auto tampered = sak::ai::AiRunState::fromJson(identityless);
    QVERIFY2(tampered.has_pending_human_gate,
             "the pending flag must be KEPT, not cleared: clearing it resumes the run unattended");
    QCOMPARE(tampered.status, sak::ai::AiRunStatus::WaitingForHuman);
    QVERIFY(!tampered.isTerminal());

    // 3. Control: the same snapshot WITH an identity keeps the status it declared, so the forcing
    //    above is the guard firing and not an unconditional rewrite.
    QJsonObject identified = identityless;
    QJsonObject gate;
    gate[QStringLiteral("gate_id")] = QStringLiteral("gate_ok");
    identified[QStringLiteral("pending_human_gate")] = gate;
    const auto intact = sak::ai::AiRunState::fromJson(identified);
    QCOMPARE(intact.status, sak::ai::AiRunStatus::Running);
    QCOMPARE(intact.pending_human_gate.gate_id, QStringLiteral("gate_ok"));

    // 4. The flag's own fail-closed default, on a snapshot that omits the key entirely. toJson
    //    ALWAYS writes it, so the key-absent path was unreachable through any round trip.
    QJsonObject absent;
    absent[QStringLiteral("run_id")] = QStringLiteral("run_absent");
    absent[QStringLiteral("status")] = QStringLiteral("running");
    const auto defaulted = sak::ai::AiRunState::fromJson(absent);
    QVERIFY(!defaulted.has_pending_human_gate);
    QCOMPARE(defaulted.status, sak::ai::AiRunStatus::Running);
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

    // AiRunState::isTerminal must be the same verdict as the free function, not a second opinion
    // -- and agreement over an eight-value enum cannot be established from two points. Only
    // Cancelling and Failed were ever put through the member, which proves merely that it
    // separates those two; the member has no other caller anywhere in src/ or tests/, so this is
    // its entire coverage. Drive EVERY status through both and require them to agree.
    for (const sak::ai::AiRunStatus status : {sak::ai::AiRunStatus::Idle,
                                              sak::ai::AiRunStatus::Planning,
                                              sak::ai::AiRunStatus::Running,
                                              sak::ai::AiRunStatus::WaitingForHuman,
                                              sak::ai::AiRunStatus::Cancelling,
                                              sak::ai::AiRunStatus::Cancelled,
                                              sak::ai::AiRunStatus::Completed,
                                              sak::ai::AiRunStatus::Failed}) {
        sak::ai::AiRunState state;
        state.status = status;
        QVERIFY2(state.isTerminal() == sak::ai::isTerminalRunStatus(status),
                 qPrintable(QStringLiteral("isTerminal() disagreed with isTerminalRunStatus for "
                                           "status %1")
                                .arg(sak::ai::runStatusToString(status))));
    }
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

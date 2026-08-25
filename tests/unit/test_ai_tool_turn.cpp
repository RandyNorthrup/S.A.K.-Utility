// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_tool_turn.h"

#include <QJsonArray>
#include <QtTest/QtTest>

namespace {

sak::ai::OpenAIFunctionCall makeCall(const QString& call_id, const QString& name) {
    sak::ai::OpenAIFunctionCall call;
    call.call_id = call_id;
    call.name = name;
    call.arguments_json = QStringLiteral("{\"ok\":true}");
    return call;
}

sak::ai::OpenAIFunctionOutput makeOutput(const QString& call_id, const QString& text) {
    sak::ai::OpenAIFunctionOutput output;
    output.call_id = call_id;
    output.output = text;
    return output;
}

}  // namespace

class AiToolTurnTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void beginAndAdvanceThroughCalls();
    void snapshotRoundTripsCurrentCallAndOutputs();
    void restoreRejectsInvalidSchema();
    void restoreRejectsTooManyOutputs();
    void appendRejectsMismatchedCallId();
    void beginRejectsDuplicateCallIds();
    void beginRejectsMalformedArguments();
};

void AiToolTurnTests::beginAndAdvanceThroughCalls() {
    sak::ai::AiToolTurn turn;
    QString error;
    QVERIFY(turn.begin(QStringLiteral("resp_1"),
                       {makeCall(QStringLiteral("call_1"), QStringLiteral("take_screenshot")),
                        makeCall(QStringLiteral("call_2"), QStringLiteral("download_file"))},
                       &error));
    QVERIFY(error.isEmpty());
    QVERIFY(turn.active());
    QCOMPARE(turn.callIndex(), 0);
    QCOMPARE(turn.remainingCallCount(), 2);
    QCOMPARE(turn.currentCallId(), QStringLiteral("call_1"));

    auto first = turn.appendOutput(makeOutput(QStringLiteral("call_1"), QStringLiteral("{}")));
    QVERIFY(first.ok);
    QVERIFY(!first.finished);
    QCOMPARE(turn.callIndex(), 1);
    QCOMPARE(turn.currentCallId(), QStringLiteral("call_2"));

    auto second = turn.appendOutput(
        makeOutput(QStringLiteral("call_2"), QStringLiteral("{\"success\":true}")));
    QVERIFY(second.ok);
    QVERIFY(second.finished);
    QCOMPARE(turn.completedOutputCount(), 2);
    // remainingCallCount() was asserted exactly once, at call_index 0 -- the one state where
    // `size() - index` and `size()` give the same answer, so the subtraction could be deleted.
    // The panel reports this as the number of tools still outstanding after a resume, so a
    // half-finished turn would claim the whole batch is still pending.
    QCOMPARE(turn.remainingCallCount(), 0);

    // takeOutputs() is what the panel hands straight back to OpenAI, and nothing ever looked
    // INSIDE an output: only the size was checked, so appendOutput could blank a result body or
    // takeOutputs could reverse the vector with the suite green.
    const auto taken = turn.takeOutputs();
    QCOMPARE(taken.size(), 2);
    QCOMPARE(taken.at(0).call_id, QStringLiteral("call_1"));
    QCOMPARE(taken.at(0).output, QStringLiteral("{}"));
    QCOMPARE(taken.at(1).call_id, QStringLiteral("call_2"));
    QCOMPARE(taken.at(1).output, QStringLiteral("{\"success\":true}"));
    // ... and the drain leaves the turn PROVABLY empty, which the production comment claims and
    // no assertion could see, because the count was never read after taking.
    QCOMPARE(turn.completedOutputCount(), 0);
    QVERIFY(turn.takeOutputs().isEmpty());
}

void AiToolTurnTests::snapshotRoundTripsCurrentCallAndOutputs() {
    sak::ai::AiToolTurn original;
    QVERIFY(original.begin(QStringLiteral("resp_2"),
                           {makeCall(QStringLiteral("call_1"), QStringLiteral("run_powershell")),
                            makeCall(QStringLiteral("call_2"),
                                     QStringLiteral("sak_package_manager"))}));
    QVERIFY(original.appendOutput(makeOutput(QStringLiteral("call_1"), QStringLiteral("one"))).ok);

    const QJsonObject snapshot = original.toJson(QStringLiteral("run_abc"));
    QCOMPARE(snapshot.value(QStringLiteral("schema")).toString(),
             QStringLiteral("sak.ai.pending_tool_turn.v1"));
    QCOMPARE(snapshot.value(QStringLiteral("run_id")).toString(), QStringLiteral("run_abc"));
    QCOMPARE(snapshot.value(QStringLiteral("current_call"))
                 .toObject()
                 .value(QStringLiteral("call_id"))
                 .toString(),
             QStringLiteral("call_2"));

    sak::ai::AiToolTurn restored;
    QString error;
    QVERIFY(restored.restore(snapshot, &error));
    QVERIFY(error.isEmpty());
    QVERIFY(restored.active());
    QCOMPARE(restored.responseId(), QStringLiteral("resp_2"));
    QCOMPARE(restored.callIndex(), 1);
    QCOMPARE(restored.currentCallId(), QStringLiteral("call_2"));
    QCOMPARE(restored.completedOutputCount(), 1);
    // The restore asserted only scalars -- ids, a cursor, a count -- and never what the turn
    // actually CARRIES. arguments_json survived no assertion anywhere in the file: if
    // functionCallToJson stopped writing it, decodeCalls accepts an undefined value,
    // functionCallFromJson coerces it to an empty QString, and validateCall explicitly allows
    // empty arguments -- so the restore succeeds and the resumed turn dispatches
    // run_powershell / sak_package_manager with NO ARGUMENTS. The same hole existed for the
    // output body, silently blanking a completed tool result on resume.
    QCOMPARE(snapshot.value(QStringLiteral("calls")).toArray().size(), 2);
    const QJsonObject first_call =
        snapshot.value(QStringLiteral("calls")).toArray().at(0).toObject();
    QCOMPARE(first_call.value(QStringLiteral("call_id")).toString(), QStringLiteral("call_1"));
    QCOMPARE(first_call.value(QStringLiteral("name")).toString(), QStringLiteral("run_powershell"));
    QCOMPARE(first_call.value(QStringLiteral("arguments_json")).toString(),
             QStringLiteral("{\"ok\":true}"));
    const QJsonObject snapshot_output =
        snapshot.value(QStringLiteral("outputs")).toArray().at(0).toObject();
    QCOMPARE(snapshot_output.value(QStringLiteral("call_id")).toString(), QStringLiteral("call_1"));
    QCOMPARE(snapshot_output.value(QStringLiteral("output")).toString(), QStringLiteral("one"));
    // ... and the same values survive the decode, not merely the encode.
    const auto restored_outputs = restored.takeOutputs();
    QCOMPARE(restored_outputs.size(), 1);
    QCOMPARE(restored_outputs.at(0).call_id, QStringLiteral("call_1"));
    QCOMPARE(restored_outputs.at(0).output, QStringLiteral("one"));

    // The run-id binding, which this fixture is the only one able to reach: every restore() in
    // the file omitted expected_run_id, so validateSnapshotRunId's cross-check arm was never
    // entered and the guard that stops a pending turn resuming into the WRONG run could be
    // deleted whole, or inverted, without a red test. Both arms are pinned now.
    sak::ai::AiToolTurn bound;
    QString bound_error;
    QVERIFY2(bound.restore(snapshot, &bound_error, QStringLiteral("run_abc")),
             qPrintable(bound_error));
    QVERIFY(bound.active());

    sak::ai::AiToolTurn wrong_run;
    QString wrong_error;
    QVERIFY2(!wrong_run.restore(snapshot, &wrong_error, QStringLiteral("run_other")),
             "a snapshot must not resume into a different run");
    QVERIFY(!wrong_error.isEmpty());
    QVERIFY(!wrong_run.active());
}

void AiToolTurnTests::restoreRejectsInvalidSchema() {
    QJsonObject snapshot;
    snapshot[QStringLiteral("schema")] = QStringLiteral("legacy");
    snapshot[QStringLiteral("response_id")] = QStringLiteral("resp");
    snapshot[QStringLiteral("call_index")] = 0;
    snapshot[QStringLiteral("calls")] = QJsonArray{sak::ai::AiToolTurn::functionCallToJson(
        makeCall(QStringLiteral("call_1"), QStringLiteral("run_cmd")))};

    sak::ai::AiToolTurn turn;
    QString error;
    QVERIFY(!turn.restore(snapshot, &error));
    QCOMPARE(error, QStringLiteral("Unsupported pending tool turn schema"));
    QVERIFY(!turn.active());

    // NEAR MISSES. "legacy" shares no prefix, suffix or substring with the real schema token, so
    // any loosening of the exact compare survived it -- and a startsWith/contains compare would
    // accept a v2-shaped or attacker-chosen schema this decoder does not actually understand.
    for (const QString& near_miss : {QStringLiteral("sak.ai.pending_tool_turn.v2"),
                                     QStringLiteral("sak.ai.pending_tool_turn"),
                                     QStringLiteral("sak.ai.pending_tool_turn.v1x"),
                                     QStringLiteral("x.sak.ai.pending_tool_turn.v1"),
                                     QStringLiteral("SAK.AI.PENDING_TOOL_TURN.V1"),
                                     QStringLiteral(" sak.ai.pending_tool_turn.v1")}) {
        QJsonObject probe = snapshot;
        probe[QStringLiteral("schema")] = near_miss;
        sak::ai::AiToolTurn rejected;
        QString probe_error;
        QVERIFY2(!rejected.restore(probe, &probe_error), qPrintable(near_miss));
        QCOMPARE(probe_error, QStringLiteral("Unsupported pending tool turn schema"));
        QVERIFY(!rejected.active());
    }

    // A rejected snapshot must not WIPE an in-flight turn. restore()'s own contract says it
    // validates into locals and commits only on full success, precisely so a tampered snapshot
    // cannot destroy a live turn -- but every fixture called restore() on a FRESH turn, where
    // !active() holds before the call, so the property was never observed.
    sak::ai::AiToolTurn live;
    QVERIFY(live.begin(QStringLiteral("resp_live"),
                       {makeCall(QStringLiteral("live_1"), QStringLiteral("run_cmd"))}));
    QVERIFY(live.active());
    QString live_error;
    QVERIFY(!live.restore(snapshot, &live_error));
    QVERIFY2(live.active(), "a rejected restore must not wipe an in-flight turn");
    QCOMPARE(live.responseId(), QStringLiteral("resp_live"));
    QCOMPARE(live.currentCallId(), QStringLiteral("live_1"));
}

void AiToolTurnTests::restoreRejectsTooManyOutputs() {
    QJsonObject snapshot;
    snapshot[QStringLiteral("schema")] = QStringLiteral("sak.ai.pending_tool_turn.v1");
    snapshot[QStringLiteral("response_id")] = QStringLiteral("resp");
    snapshot[QStringLiteral("call_index")] = 0;
    snapshot[QStringLiteral("calls")] = QJsonArray{sak::ai::AiToolTurn::functionCallToJson(
        makeCall(QStringLiteral("call_1"), QStringLiteral("run_cmd")))};
    snapshot[QStringLiteral("outputs")] = QJsonArray{sak::ai::AiToolTurn::functionOutputToJson(
        makeOutput(QStringLiteral("call_1"), QStringLiteral("done")))};

    sak::ai::AiToolTurn turn;
    QString error;
    QVERIFY(!turn.restore(snapshot, &error));
    // "too many" is non-unique across four fail-closed messages; pin the exact one.
    QCOMPARE(error, QStringLiteral("Pending tool turn snapshot has too many completed outputs"));
    QVERIFY(!turn.active());

    // validateSnapshotOutputsMatchCalls has THREE fail-closed arms and only the first was ever
    // reached, so the other two could be deleted silently. Both are attacks on a snapshot
    // persisted in a user-writable directory.

    // Arm two: a SHORT outputs list, which would forge completion of calls that never produced a
    // result and let resume jump past the skipped ones.
    QJsonObject short_outputs;
    short_outputs[QStringLiteral("schema")] = QStringLiteral("sak.ai.pending_tool_turn.v1");
    short_outputs[QStringLiteral("response_id")] = QStringLiteral("resp");
    // Three calls with the cursor on the third: the index is IN RANGE, so the earlier
    // invalid-call-index guard cannot be what refuses these two fixtures.
    short_outputs[QStringLiteral("call_index")] = 2;
    short_outputs[QStringLiteral("calls")] =
        QJsonArray{sak::ai::AiToolTurn::functionCallToJson(
                       makeCall(QStringLiteral("call_1"), QStringLiteral("run_cmd"))),
                   sak::ai::AiToolTurn::functionCallToJson(
                       makeCall(QStringLiteral("call_2"), QStringLiteral("download_file"))),
                   sak::ai::AiToolTurn::functionCallToJson(
                       makeCall(QStringLiteral("call_3"), QStringLiteral("take_screenshot")))};
    short_outputs[QStringLiteral("outputs")] = QJsonArray{sak::ai::AiToolTurn::functionOutputToJson(
        makeOutput(QStringLiteral("call_1"), QStringLiteral("done")))};
    sak::ai::AiToolTurn short_turn;
    QString short_error;
    QVERIFY(!short_turn.restore(short_outputs, &short_error));
    QCOMPARE(short_error,
             QStringLiteral("Pending tool turn snapshot is missing outputs for completed calls"));
    QVERIFY(!short_turn.active());

    // Arm three: the positional pairing loop, which stops a tampered snapshot pairing one call's
    // output with another call's result. The counts match here, so only the pairing can refuse it.
    QJsonObject mispaired = short_outputs;
    mispaired[QStringLiteral("outputs")] =
        QJsonArray{sak::ai::AiToolTurn::functionOutputToJson(
                       makeOutput(QStringLiteral("call_2"), QStringLiteral("wrong"))),
                   sak::ai::AiToolTurn::functionOutputToJson(
                       makeOutput(QStringLiteral("call_1"), QStringLiteral("swapped")))};
    sak::ai::AiToolTurn mispaired_turn;
    QString mispaired_error;
    QVERIFY(!mispaired_turn.restore(mispaired, &mispaired_error));
    QCOMPARE(mispaired_error,
             QStringLiteral("Pending tool turn snapshot output 0 does not match its call"));
    QVERIFY(!mispaired_turn.active());
}

void AiToolTurnTests::appendRejectsMismatchedCallId() {
    sak::ai::AiToolTurn turn;
    QVERIFY(turn.begin(QStringLiteral("resp"),
                       {makeCall(QStringLiteral("call_expected"), QStringLiteral("run_cmd"))}));

    const auto result =
        turn.appendOutput(makeOutput(QStringLiteral("call_other"), QStringLiteral("{}")));
    QVERIFY(!result.ok);
    QCOMPARE(result.error_message,
             QStringLiteral(
                 "Tool output call id mismatch: expected call_expected, got call_other"));
    QCOMPARE(turn.callIndex(), 0);
    QCOMPARE(turn.completedOutputCount(), 0);
}

void AiToolTurnTests::beginRejectsDuplicateCallIds() {
    // The whole batch is validated up front: two calls sharing a call_id must be rejected
    // atomically before ANY of them can run.
    sak::ai::AiToolTurn turn;
    QString error;
    QVERIFY(!turn.begin(QStringLiteral("resp"),
                        {makeCall(QStringLiteral("dup"), QStringLiteral("take_screenshot")),
                         makeCall(QStringLiteral("dup"), QStringLiteral("download_file"))},
                        &error));
    QCOMPARE(error, QStringLiteral("Duplicate function call id dup in batch"));
    QVERIFY(!turn.active());

    // The duplicates above are ADJACENT, so the whole-batch scan is indistinguishable from a
    // compare against only the previous call. Separate them.
    sak::ai::AiToolTurn spaced;
    QString spaced_error;
    QVERIFY(!spaced.begin(QStringLiteral("resp"),
                          {makeCall(QStringLiteral("dup"), QStringLiteral("take_screenshot")),
                           makeCall(QStringLiteral("other"), QStringLiteral("run_cmd")),
                           makeCall(QStringLiteral("dup"), QStringLiteral("download_file"))},
                          &spaced_error));
    QCOMPARE(spaced_error, QStringLiteral("Duplicate function call id dup in batch"));
    QVERIFY(!spaced.active());

    // begin() must be NON-DESTRUCTIVE on failure. Its own comment records this as a real past
    // bug -- it used to reset() on failure, so feeding invalid calls (untrusted model output)
    // while a turn was mid-flight silently wiped its pending calls and completed outputs. Every
    // fixture called begin() on a FRESH turn, where !active() holds beforehand, so a regression
    // straight back to that data loss was invisible.
    sak::ai::AiToolTurn live;
    QVERIFY(live.begin(QStringLiteral("resp_live"),
                       {makeCall(QStringLiteral("live_1"), QStringLiteral("run_cmd")),
                        makeCall(QStringLiteral("live_2"), QStringLiteral("download_file"))}));
    QVERIFY(live.appendOutput(makeOutput(QStringLiteral("live_1"), QStringLiteral("kept"))).ok);
    QCOMPARE(live.completedOutputCount(), 1);

    QString clobber_error;
    QVERIFY(!live.begin(QStringLiteral("resp_bad"),
                        {makeCall(QStringLiteral("dup"), QStringLiteral("take_screenshot")),
                         makeCall(QStringLiteral("dup"), QStringLiteral("download_file"))},
                        &clobber_error));
    QVERIFY2(live.active(), "a refused begin() must not wipe an in-flight turn");
    QCOMPARE(live.responseId(), QStringLiteral("resp_live"));
    QCOMPARE(live.currentCallId(), QStringLiteral("live_2"));
    QCOMPARE(live.completedOutputCount(), 1);
    QCOMPARE(live.takeOutputs().at(0).output, QStringLiteral("kept"));
}

void AiToolTurnTests::beginRejectsMalformedArguments() {
    // A call whose arguments_json is not a valid JSON object fails the batch up front, so an
    // earlier destructive call cannot run before the malformed one is refused.
    sak::ai::OpenAIFunctionCall bad;
    bad.call_id = QStringLiteral("c1");
    bad.name = QStringLiteral("run_cmd");
    bad.arguments_json = QStringLiteral("{not valid json");

    sak::ai::AiToolTurn turn;
    QString error;
    QVERIFY(!turn.begin(QStringLiteral("resp"), {bad}, &error));
    // The errorString() tail is Qt-version-variant; pin the deterministic prefix (with the c1 id).
    QVERIFY(error.startsWith(QStringLiteral("Function call c1 has malformed arguments_json:")));
    QVERIFY(!turn.active());

    // The guard is `parse_error != NoError || !doc.isObject()`, and "{not valid json" reaches
    // only the first arm. The second -- well-formed JSON that is NOT an object -- was never
    // exercised, so it could be deleted silently, and a JSON array smuggled in as arguments_json
    // would then be handed to the tool router as if it were a valid argument object. That defeats
    // the whole point of validating the batch atomically before any earlier destructive call runs.
    for (const QString& non_object : {QStringLiteral("[1,2,3]"),
                                      QStringLiteral("\"a string\""),
                                      QStringLiteral("42"),
                                      QStringLiteral("true"),
                                      QStringLiteral("null")}) {
        sak::ai::OpenAIFunctionCall shaped;
        shaped.call_id = QStringLiteral("c2");
        shaped.name = QStringLiteral("run_cmd");
        shaped.arguments_json = non_object;

        sak::ai::AiToolTurn shaped_turn;
        QString shaped_error;
        QVERIFY2(!shaped_turn.begin(QStringLiteral("resp"), {shaped}, &shaped_error),
                 qPrintable(non_object));
        QVERIFY2(shaped_error.startsWith(
                     QStringLiteral("Function call c2 has malformed arguments_json:")),
                 qPrintable(shaped_error));
        QVERIFY(!shaped_turn.active());
    }
}

QTEST_GUILESS_MAIN(AiToolTurnTests)
#include "test_ai_tool_turn.moc"

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
    QCOMPARE(turn.takeOutputs().size(), 2);
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
    QVERIFY(error.contains(QStringLiteral("schema")));
    QVERIFY(!turn.active());
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
    QVERIFY(error.contains(QStringLiteral("too many")));
    QVERIFY(!turn.active());
}

void AiToolTurnTests::appendRejectsMismatchedCallId() {
    sak::ai::AiToolTurn turn;
    QVERIFY(turn.begin(QStringLiteral("resp"),
                       {makeCall(QStringLiteral("call_expected"), QStringLiteral("run_cmd"))}));

    const auto result =
        turn.appendOutput(makeOutput(QStringLiteral("call_other"), QStringLiteral("{}")));
    QVERIFY(!result.ok);
    QVERIFY(result.error_message.contains(QStringLiteral("mismatch")));
    QCOMPARE(turn.callIndex(), 0);
    QCOMPARE(turn.completedOutputCount(), 0);
}

QTEST_GUILESS_MAIN(AiToolTurnTests)
#include "test_ai_tool_turn.moc"

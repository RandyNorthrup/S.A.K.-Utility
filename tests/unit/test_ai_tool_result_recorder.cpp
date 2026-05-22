// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_conversation_store.h"
#include "sak/ai/ai_tool_result_recorder.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

QString redactTestSecret(const QString& text) {
    QString redacted = text;
    redacted.replace(QStringLiteral("SECRET"), QStringLiteral("[REDACTED]"));
    return redacted;
}

QJsonObject commandResult() {
    QJsonObject result;
    result[QStringLiteral("command_id")] = QStringLiteral("cmd_001");
    result[QStringLiteral("preview")] = QStringLiteral("Get-Thing SECRET");
    result[QStringLiteral("exit_code")] = 0;
    result[QStringLiteral("stdout")] = QStringLiteral("done SECRET");
    return result;
}

QJsonObject firstJsonLine(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readLine()).object();
}

}  // namespace

class AiToolResultRecorderTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void summaryRedactsAndIncludesStatus();
    void recordWritesCommandAndTranscript();
    void recordFailsLoudWithoutActiveSession();
};

void AiToolResultRecorderTests::summaryRedactsAndIncludesStatus() {
    const QString summary = sak::ai::toolResultChatSummary(commandResult(), redactTestSecret);
    QVERIFY(summary.contains(QStringLiteral("Command: Get-Thing [REDACTED]")));
    QVERIFY(summary.contains(QStringLiteral("Exit code: 0")));
    QVERIFY(summary.contains(QStringLiteral("Output: done [REDACTED]")));
    QVERIFY(!summary.contains(QStringLiteral("SECRET")));
}

void AiToolResultRecorderTests::recordWritesCommandAndTranscript() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY2(store.startSession(QStringLiteral("Tool Result Test"), &error), qPrintable(error));

    sak::ai::AiToolResultRecordRequest request;
    request.command_id = QStringLiteral("cmd_001");
    request.command_preview = QStringLiteral("Get-Thing SECRET");
    request.result_json = commandResult();
    request.transcript_metadata = QJsonObject{{QStringLiteral("source"), QStringLiteral("unit")}};

    const auto result = sak::ai::AiToolResultRecorder::record(&store, request, redactTestSecret);
    QVERIFY2(result.ok(), qPrintable(result.errors.join(QStringLiteral("; "))));
    QVERIFY(result.command_recorded);
    QVERIFY(result.transcript_recorded);
    QVERIFY(result.wroteStore());
    QVERIFY(!result.transcript_text.contains(QStringLiteral("SECRET")));

    const QString session_path = store.currentSessionInfo().path;
    const QJsonObject command =
        firstJsonLine(QDir(session_path).filePath(QStringLiteral("commands.jsonl")));
    QCOMPARE(command.value(QStringLiteral("command")).toString(),
             QStringLiteral("Get-Thing SECRET"));

    const auto lines = store.loadTranscriptLines(store.currentSessionId(), &error);
    QCOMPARE(lines.size(), 1);
    QVERIFY(lines.first().contains(QStringLiteral("TOOL RESULT")));
    QVERIFY(lines.first().contains(QStringLiteral("Get-Thing [REDACTED]")));
    QVERIFY(!lines.first().contains(QStringLiteral("SECRET")));
}

void AiToolResultRecorderTests::recordFailsLoudWithoutActiveSession() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    sak::ai::ConversationStore store(temp.path());

    sak::ai::AiToolResultRecordRequest request;
    request.command_id = QStringLiteral("cmd_001");
    request.result_json = commandResult();

    const auto result = sak::ai::AiToolResultRecorder::record(&store, request);
    QVERIFY(!result.ok());
    QVERIFY(!result.command_recorded);
    QVERIFY(!result.transcript_recorded);
    QCOMPARE(result.errors.size(), 2);
    QVERIFY(
        result.errors.join(QStringLiteral("\n")).contains(QStringLiteral("No active AI session")));
}

QTEST_GUILESS_MAIN(AiToolResultRecorderTests)
#include "test_ai_tool_result_recorder.moc"

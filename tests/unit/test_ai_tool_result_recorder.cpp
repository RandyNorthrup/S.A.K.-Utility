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
    void summaryUsesCredentialRedactionByDefault();
    void clippedTextHonorsTinyPositiveLimit();
    void summaryDoesNotCoerceInvalidStatusValues();
    void recordWritesCommandAndTranscript();
    void recordFailsLoudWithoutActiveSession();
};

void AiToolResultRecorderTests::summaryRedactsAndIncludesStatus() {
    const QString summary = sak::ai::toolResultChatSummary(commandResult(), redactTestSecret);
    // The three appenders (command, status, stdout) run in fixed order and join with "\n";
    // the exact QCOMPARE pins order, the full line set, and redaction in one assertion.
    QCOMPARE(summary,
             QStringLiteral(
                 "Command: Get-Thing [REDACTED]\nExit code: 0\nOutput: done [REDACTED]"));
}

void AiToolResultRecorderTests::summaryUsesCredentialRedactionByDefault() {
    const QString secret = QStringLiteral("ctx7") + QStringLiteral("sk-") +
                           QStringLiteral("fc513191-580d-40c0-b244-17ea71f182b9");
    QJsonObject result;
    result[QStringLiteral("preview")] = QStringLiteral("Get-Thing -ApiKey ") + secret;
    result[QStringLiteral("stdout")] = QStringLiteral("configured ") + secret;

    const QString summary = sak::ai::toolResultChatSummary(result);
    QVERIFY(!summary.contains(secret));
    QCOMPARE(summary.count(QStringLiteral("[redacted-context7-key]")), 2);
}

void AiToolResultRecorderTests::clippedTextHonorsTinyPositiveLimit() {
    constexpr int kTinyLimit = 8;
    const QString clipped =
        sak::ai::clippedToolResultText(QStringLiteral("abcdefghijklmnopqrstuvwxyz"), kTinyLimit);
    QCOMPARE(clipped.size(), static_cast<qsizetype>(kTinyLimit));
}

void AiToolResultRecorderTests::summaryDoesNotCoerceInvalidStatusValues() {
    QJsonObject invalid_exit;
    invalid_exit[QStringLiteral("exit_code")] = QStringLiteral("not-an-exit-code");
    QCOMPARE(sak::ai::toolResultChatSummary(invalid_exit), QStringLiteral("Exit code: invalid"));
    invalid_exit[QStringLiteral("exit_code")] = 1.5;
    QCOMPARE(sak::ai::toolResultChatSummary(invalid_exit), QStringLiteral("Exit code: invalid"));
    invalid_exit[QStringLiteral("exit_code")] = 2147483648.0;
    QCOMPARE(sak::ai::toolResultChatSummary(invalid_exit), QStringLiteral("Exit code: invalid"));

    QJsonObject invalid_success;
    invalid_success[QStringLiteral("success")] = QStringLiteral("yes");
    QCOMPARE(sak::ai::toolResultChatSummary(invalid_success), QStringLiteral("Success: invalid"));
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
    QCOMPARE(result.transcript_text,
             QStringLiteral(
                 "Command: Get-Thing [REDACTED]\nExit code: 0\nOutput: done [REDACTED]"));

    const QString session_path = store.currentSessionInfo().path;
    const QJsonObject command =
        firstJsonLine(QDir(session_path).filePath(QStringLiteral("commands.jsonl")));
    QCOMPARE(command.value(QStringLiteral("command")).toString(),
             QStringLiteral("Get-Thing SECRET"));

    const auto lines = store.loadTranscriptLines(store.currentSessionId(), &error);
    QCOMPARE(lines.size(), 1);
    // transcriptDisplayLine renders "\n[HEADING]\ntext" with no timestamp; role "Tool Result"
    // -> "TOOL RESULT" and {source:unit} carries no citations, so pin the whole rendered line.
    QCOMPARE(lines.first(),
             QStringLiteral("\n[TOOL RESULT]\nCommand: Get-Thing [REDACTED]\nExit code: 0\nOutput: "
                            "done [REDACTED]"));
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
    // Both the command and transcript appends hit appendJsonLine's "No active AI session" and are
    // wrapped with their distinct prefixes; pin the exact two-element list and its order.
    QCOMPARE(result.errors,
             (QStringList{QStringLiteral("Command record failed: No active AI session"),
                          QStringLiteral("Transcript log failed: No active AI session")}));
}

QTEST_GUILESS_MAIN(AiToolResultRecorderTests)
#include "test_ai_tool_result_recorder.moc"

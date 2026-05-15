// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_conversation_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class AiConversationStoreTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void startSession_writesManifestAndListsSession();
    void appendTranscript_loadsDisplayLines();
    void writeUsage_persistsUsageJson();
    void commandLogPath_createsLogsDirectoryAndReturnsPath();
    void artifactPath_createsSubdirectoryAndReturnsPath();
    void renameSession_updatesTitleAndArtifactRoot();
    void memoryFile_appendsEntries();
    void memoryFile_initializesStructuredSections();
    void memoryFile_trimPreservesStructuredSections();
};

void AiConversationStoreTests::startSession_writesManifestAndListsSession() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY2(store.startSession(QStringLiteral("Drive Check"), &error), qPrintable(error));

    const auto sessions = store.listSessions();
    QCOMPARE(sessions.size(), 1);
    QCOMPARE(sessions.first().title, QStringLiteral("Drive Check"));
    QVERIFY(QFile::exists(sessions.first().path + QStringLiteral("/manifest.json")));
}

void AiConversationStoreTests::appendTranscript_loadsDisplayLines() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY(store.startSession(QStringLiteral("Chat"), &error));
    QVERIFY(
        store.appendTranscript(QStringLiteral("You"), QStringLiteral("check disk"), {}, &error));
    QVERIFY(
        store.appendTranscript(QStringLiteral("Assistant"), QStringLiteral("disk ok"), {}, &error));

    const auto lines = store.loadTranscriptLines(store.currentSessionId(), &error);
    QCOMPARE(lines.size(), 2);
    QCOMPARE(lines[0], QStringLiteral("\n[USER REQUEST]\ncheck disk"));
    QCOMPARE(lines[1], QStringLiteral("\n[ASSISTANT RESULT]\ndisk ok"));
}

void AiConversationStoreTests::writeUsage_persistsUsageJson() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY(store.startSession(QStringLiteral("Usage"), &error));

    sak::ai::TokenUsage turn;
    turn.input_tokens = 10;
    turn.output_tokens = 5;
    turn.total_tokens = 15;
    sak::ai::TokenUsage total = turn;
    QVERIFY(store.writeUsage(turn, total, &error));

    QFile file(store.currentSessionInfo().path + QStringLiteral("/usage.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto doc = QJsonDocument::fromJson(file.readAll());
    QVERIFY(doc.isObject());
    QCOMPARE(doc.object()
                 .value(QStringLiteral("session_total"))
                 .toObject()
                 .value(QStringLiteral("total_tokens"))
                 .toInt(),
             15);
}

void AiConversationStoreTests::commandLogPath_createsLogsDirectoryAndReturnsPath() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY(store.startSession(QStringLiteral("Logs"), &error));

    const QString stdout_path =
        store.commandLogPath(QStringLiteral("cmd_001"), QStringLiteral("stdout"), &error);
    QVERIFY2(!stdout_path.isEmpty(), qPrintable(error));
    QVERIFY(stdout_path.endsWith(QStringLiteral("/artifacts/Logs/logs/cmd_001_stdout.txt")));

    const QFileInfo info(stdout_path);
    QVERIFY(info.absoluteDir().exists());
}

void AiConversationStoreTests::artifactPath_createsSubdirectoryAndReturnsPath() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY(store.startSession(QStringLiteral("Artifacts"), &error));

    const QString screenshot_path =
        store.artifactPath(QStringLiteral("screenshots"), QStringLiteral("shot_001.png"), &error);
    QVERIFY2(!screenshot_path.isEmpty(), qPrintable(error));
    QVERIFY(
        screenshot_path.endsWith(QStringLiteral("/artifacts/Artifacts/screenshots/shot_001.png")));
    QVERIFY(QFileInfo(screenshot_path).absoluteDir().exists());

    const QString downloads_dir = store.artifactSubdir(QStringLiteral("downloads"), &error);
    QVERIFY2(!downloads_dir.isEmpty(), qPrintable(error));
    QVERIFY(QDir(downloads_dir).exists());
}

void AiConversationStoreTests::renameSession_updatesTitleAndArtifactRoot() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY(store.startSession(QStringLiteral("Old"), &error));
    const QString old_log =
        store.artifactPath(QStringLiteral("logs"), QStringLiteral("before.txt"), &error);
    QVERIFY2(!old_log.isEmpty(), qPrintable(error));
    QFile old_file(old_log);
    QVERIFY(old_file.open(QIODevice::WriteOnly | QIODevice::Text));
    old_file.write("before");
    old_file.close();

    QVERIFY(store.renameCurrentSession(QStringLiteral("Drive Check / SSD"), &error));
    QCOMPARE(store.currentSessionInfo().title, QStringLiteral("Drive Check / SSD"));

    const QString root = store.artifactRootDirectory(&error);
    QVERIFY2(!root.isEmpty(), qPrintable(error));
    QVERIFY(root.endsWith(QStringLiteral("/artifacts/Drive Check _ SSD")));
    QVERIFY(QDir(root).exists());
    QVERIFY(QFile::exists(QDir(root).filePath(QStringLiteral("logs/before.txt"))));
}

void AiConversationStoreTests::memoryFile_appendsEntries() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY(store.startSession(QStringLiteral("Memory"), &error));
    QVERIFY(store.appendMemoryEntry(QStringLiteral("User"),
                                    QStringLiteral("Request"),
                                    QStringLiteral("check my drive"),
                                    &error));
    QVERIFY(store.appendMemoryEntry(QStringLiteral("Assistant"),
                                    QStringLiteral("Finding"),
                                    QStringLiteral("SMART OK"),
                                    &error));
    const QString memory = store.memoryText(16'000, &error);
    QVERIFY(memory.contains(QStringLiteral("check my drive")));
    QVERIFY(memory.contains(QStringLiteral("SMART OK")));
}

void AiConversationStoreTests::memoryFile_initializesStructuredSections() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY(store.startSession(QStringLiteral("Memory Sections"), &error));
    QVERIFY(store.appendMemoryEntry(QStringLiteral("User"),
                                    QStringLiteral("Request"),
                                    QStringLiteral("install firefox"),
                                    &error));

    const QString memory = store.memoryText(16'000, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(memory.contains(QStringLiteral("## Pinned Facts")));
    QVERIFY(memory.contains(QStringLiteral("## Current Task")));
    QVERIFY(memory.contains(QStringLiteral("## Decisions")));
    QVERIFY(memory.contains(QStringLiteral("## Open Questions")));
    QVERIFY(memory.contains(QStringLiteral("## Artifacts")));
    QVERIFY(memory.contains(QStringLiteral("## Resolved History")));
    QVERIFY(memory.contains(QStringLiteral("install firefox")));
}

void AiConversationStoreTests::memoryFile_trimPreservesStructuredSections() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY(store.startSession(QStringLiteral("Memory Trim"), &error));

    QStringList history;
    for (int i = 0; i < 9000; ++i) {
        history << QStringLiteral(
                       "## 2026-05-13T00:%1:00Z - Tool - Log\n"
                       "resolved detail %1 with a lot of repeated diagnostic text")
                       .arg(i, 2, 10, QLatin1Char('0'));
    }
    const QString memory = QStringLiteral(
                               "# Session Working Memory\n\n"
                               "## Pinned Facts\n"
                               "- Preserve Randy's package preference.\n\n"
                               "## Current Task\n"
                               "- Build the offline installer bundle.\n\n"
                               "## Decisions\n"
                               "- Use SAK built-in downloader first.\n\n"
                               "## Open Questions\n"
                               "- Confirm destination drive.\n\n"
                               "## Artifacts\n"
                               "- reports/session.md\n\n"
                               "## Resolved History\n\n%1\n")
                               .arg(history.join(QStringLiteral("\n\n")));

    QFile file(store.currentSessionInfo().path + QStringLiteral("/memory.md"));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate));
    file.write(memory.toUtf8());
    file.close();
    QVERIFY(QFileInfo(file.fileName()).size() > 256 * 1024);

    QVERIFY(store.appendMemoryEntry(QStringLiteral("Assistant"),
                                    QStringLiteral("Latest"),
                                    QStringLiteral("Latest preserved finding"),
                                    &error));
    QFile trimmed(file.fileName());
    QVERIFY(trimmed.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString text = QString::fromUtf8(trimmed.readAll());
    trimmed.close();

    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(text.contains(QStringLiteral("## Pinned Facts")));
    QVERIFY(text.contains(QStringLiteral("Preserve Randy's package preference")));
    QVERIFY(text.contains(QStringLiteral("## Current Task")));
    QVERIFY(text.contains(QStringLiteral("Build the offline installer bundle")));
    QVERIFY(text.contains(QStringLiteral("## Open Questions")));
    QVERIFY(text.contains(QStringLiteral("Confirm destination drive")));
    QVERIFY(text.contains(QStringLiteral("older resolved history compacted by SAK")) ||
            text.contains(QStringLiteral("older section content compacted by SAK")));
    QVERIFY(text.contains(QStringLiteral("Latest preserved finding")));
    QVERIFY(QFileInfo(file.fileName()).size() <= 256 * 1024);
}

QTEST_GUILESS_MAIN(AiConversationStoreTests)
#include "test_ai_conversation_store.moc"

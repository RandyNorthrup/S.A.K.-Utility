// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_conversation_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

class AiConversationStoreTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void startSession_writesManifestAndListsSession();
    void appendTranscript_loadsDisplayLines();
    void latestAssistantResponseId_returnsLastAssistantMetadata();
    void listPromptedSessions_filtersUnpromptedSessions();
    void clearCurrentSession_preventsAccidentalWrites();
    void writeUsage_persistsUsageJson();
    void commandLogPath_createsLogsDirectoryAndReturnsPath();
    void commandLogPath_confinesTraversalTokens();
    void safeArtifactDirectoryName_rejectsDotSegments();
    void artifactPath_createsSubdirectoryAndReturnsPath();
    void renameSession_updatesTitleAndArtifactRoot();
    void caseOnlyRename_preservesArtifacts();
    void memoryFile_appendsEntries();
    void memoryFile_initializesStructuredSections();
    void memoryFile_trimPreservesStructuredSections();
    void searchSessions_findsTranscriptAndCommandIndex();
    void appendCommand_redactsSecretsInPersistedRecord();
    void concurrentReadersAndWriterDoNotDeadlockOrCorrupt();
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
    QCOMPARE(sessions.first().id, store.currentSessionId());
    QCOMPARE(sessions.first().path, store.currentSessionInfo().path);
    // The manifest must carry the WHOLE record, not merely exist: id and title round-trip through
    // disk and both timestamps are written in a parseable form (the session picker renders
    // updated_at and listSessions sorts on it).
    QFile manifest(sessions.first().path + QStringLiteral("/manifest.json"));
    QVERIFY(manifest.open(QIODevice::ReadOnly | QIODevice::Text));
    const QJsonObject manifest_root = QJsonDocument::fromJson(manifest.readAll()).object();
    QCOMPARE(manifest_root.value(QStringLiteral("id")).toString(), store.currentSessionId());
    QCOMPARE(manifest_root.value(QStringLiteral("title")).toString(),
             QStringLiteral("Drive Check"));
    QVERIFY(sessions.first().created_at.isValid());
    QVERIFY(sessions.first().updated_at.isValid());
    QCOMPARE(sessions.first().created_at, store.currentSessionInfo().created_at);
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

void AiConversationStoreTests::latestAssistantResponseId_returnsLastAssistantMetadata() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY(store.startSession(QStringLiteral("Chat"), &error));
    QVERIFY(store.appendTranscript(QStringLiteral("You"), QStringLiteral("first"), {}, &error));
    QVERIFY(store.appendTranscript(QStringLiteral("Assistant"),
                                   QStringLiteral("old"),
                                   QJsonObject{
                                       {QStringLiteral("response_id"), QStringLiteral("resp_old")}},
                                   &error));
    QVERIFY(store.appendTranscript(QStringLiteral("Tool Result"),
                                   QStringLiteral("not a chat response"),
                                   QJsonObject{{QStringLiteral("response_id"),
                                                QStringLiteral("resp_tool")}},
                                   &error));
    QVERIFY(store.appendTranscript(QStringLiteral("Assistant"),
                                   QStringLiteral("new"),
                                   QJsonObject{
                                       {QStringLiteral("response_id"), QStringLiteral("resp_new")}},
                                   &error));

    QCOMPARE(store.latestAssistantResponseId(store.currentSessionId(), &error),
             QStringLiteral("resp_new"));
    QVERIFY(error.isEmpty());
}

void AiConversationStoreTests::listPromptedSessions_filtersUnpromptedSessions() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY(store.startSession(QStringLiteral("Empty"), &error));
    const QString empty_id = store.currentSessionId();
    QVERIFY(!empty_id.isEmpty());

    QVERIFY(store.startSession(QStringLiteral("Prompted"), &error));
    const QString prompted_id = store.currentSessionId();
    QVERIFY(store.appendTranscript(QStringLiteral("You"), QStringLiteral("run scan"), {}, &error));

    // An assistant-only session HAS a transcript but no user prompt, and a user-role line whose
    // text is only whitespace is not a prompt either: the filter is on role AND text, not on
    // "the session has any transcript line at all".
    QVERIFY(store.startSession(QStringLiteral("AssistantOnly"), &error));
    const QString assistant_only_id = store.currentSessionId();
    QVERIFY(store.appendTranscript(
        QStringLiteral("Assistant"), QStringLiteral("scan finished"), {}, &error));
    QVERIFY(store.startSession(QStringLiteral("Blank"), &error));
    const QString blank_id = store.currentSessionId();
    QVERIFY(store.appendTranscript(QStringLiteral("You"), QStringLiteral("   "), {}, &error));

    const auto sessions = store.listPromptedSessions();
    QCOMPARE(sessions.size(), 1);
    QCOMPARE(sessions.first().id, prompted_id);
    QCOMPARE(sessions.first().title, QStringLiteral("Prompted"));
    QVERIFY(std::none_of(sessions.cbegin(), sessions.cend(), [&](const auto& session) {
        return session.id == empty_id || session.id == assistant_only_id || session.id == blank_id;
    }));
}

void AiConversationStoreTests::clearCurrentSession_preventsAccidentalWrites() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY(store.startSession(QStringLiteral("Current"), &error));
    store.clearCurrentSession();

    QVERIFY(store.currentSessionId().isEmpty());
    // The WHOLE record is cleared, not just the id: a stale title/path left behind is still shown
    // by the panel and still names an artifact directory the next chat writes into.
    const sak::ai::AiSessionInfo cleared = store.currentSessionInfo();
    QVERIFY(cleared.id.isEmpty());
    QVERIFY(cleared.title.isEmpty());
    QVERIFY(cleared.path.isEmpty());
    QVERIFY(!cleared.created_at.isValid());
    QVERIFY(!cleared.updated_at.isValid());
    QVERIFY(
        !store.appendTranscript(QStringLiteral("System"), QStringLiteral("not saved"), {}, &error));
    QCOMPARE(error, QStringLiteral("No active AI session"));
}

void AiConversationStoreTests::writeUsage_persistsUsageJson() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY(store.startSession(QStringLiteral("Usage"), &error));

    sak::ai::TokenUsage turn;
    turn.input_tokens = 10;
    turn.cached_input_tokens = 4;
    turn.output_tokens = 5;
    turn.reasoning_tokens = 2;
    turn.total_tokens = 15;
    // Deliberately DIFFERENT from `turn` so swapping writeUsage's two arguments, or writing one
    // of them into both slots, cannot pass.
    sak::ai::TokenUsage total;
    total.input_tokens = 110;
    total.cached_input_tokens = 44;
    total.output_tokens = 55;
    total.reasoning_tokens = 22;
    total.total_tokens = 165;
    QVERIFY2(store.writeUsage(turn, total, &error), qPrintable(error));

    QFile file(store.currentSessionInfo().path + QStringLiteral("/usage.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto doc = QJsonDocument::fromJson(file.readAll());
    QVERIFY(doc.isObject());
    const QJsonObject usage_root = doc.object();
    const QJsonObject last_turn = usage_root.value(QStringLiteral("last_turn")).toObject();
    QCOMPARE(last_turn.value(QStringLiteral("input_tokens")).toInt(), 10);
    QCOMPARE(last_turn.value(QStringLiteral("cached_input_tokens")).toInt(), 4);
    QCOMPARE(last_turn.value(QStringLiteral("output_tokens")).toInt(), 5);
    QCOMPARE(last_turn.value(QStringLiteral("reasoning_tokens")).toInt(), 2);
    QCOMPARE(last_turn.value(QStringLiteral("total_tokens")).toInt(), 15);
    const QJsonObject session_total = usage_root.value(QStringLiteral("session_total")).toObject();
    QCOMPARE(session_total.value(QStringLiteral("input_tokens")).toInt(), 110);
    QCOMPARE(session_total.value(QStringLiteral("cached_input_tokens")).toInt(), 44);
    QCOMPARE(session_total.value(QStringLiteral("output_tokens")).toInt(), 55);
    QCOMPARE(session_total.value(QStringLiteral("reasoning_tokens")).toInt(), 22);
    QCOMPARE(session_total.value(QStringLiteral("total_tokens")).toInt(), 165);
    QVERIFY(!usage_root.value(QStringLiteral("updated_at")).toString().isEmpty());
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

void AiConversationStoreTests::commandLogPath_confinesTraversalTokens() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY(store.startSession(QStringLiteral("Logs"), &error));

    // A traversal-shaped command_id / suffix must be sanitized so the log file stays inside
    // the session's logs directory (no "../" and no bare dot-segments in the filename).
    const QString path =
        store.commandLogPath(QStringLiteral("../../etc"), QStringLiteral(".."), &error);
    QVERIFY2(!path.isEmpty(), qPrintable(error));
    // Both tokens are fully determined by sanitizeLogToken, so pin the whole resulting path:
    // separators and reserved characters become '_' and any run of 2+ dots collapses to a single
    // '_', landing the file under THIS session's own logs directory. The QFileInfo::fileName()
    // checks below cannot fail for ANY input -- fileName() never returns a path separator, and a
    // ".." that survived sanitizing would be a DIRECTORY component, so it would be stripped from
    // the name rather than reported.
    QCOMPARE(path,
             store.currentSessionInfo().path +
                 QStringLiteral("/artifacts/Logs/logs/____etc__.txt"));
}

void AiConversationStoreTests::safeArtifactDirectoryName_rejectsDotSegments() {
    // A title that is only dots would traverse/alias the parent artifacts dir; it must
    // collapse to the safe fallback rather than name the directory "." or "..".
    QCOMPARE(sak::ai::ConversationStore::safeArtifactDirectoryName(QStringLiteral(".."),
                                                                   QStringLiteral("ai_x")),
             QStringLiteral("AI Session"));
    QCOMPARE(sak::ai::ConversationStore::safeArtifactDirectoryName(QStringLiteral("."),
                                                                   QStringLiteral("ai_x")),
             QStringLiteral("AI Session"));
    QCOMPARE(sak::ai::ConversationStore::safeArtifactDirectoryName(QStringLiteral("..."),
                                                                   QStringLiteral("ai_x")),
             QStringLiteral("AI Session"));
    // A normal title is preserved.
    QCOMPARE(sak::ai::ConversationStore::safeArtifactDirectoryName(QStringLiteral("My Session"),
                                                                   QStringLiteral("ai_x")),
             QStringLiteral("My Session"));
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

    // P08-01: a filename that escapes the artifact subdir must be rejected -- and rejected BY THE
    // CONTAINMENT GUARD, not by one of artifactPath's other empty-with-error return paths.
    error.clear();
    const QString escaped = store.artifactPath(QStringLiteral("downloads"),
                                               QStringLiteral("../../../../evil.txt"),
                                               &error);
    QVERIFY(escaped.isEmpty());
    QCOMPARE(error,
             QStringLiteral("Artifact filename escapes the artifact directory: "
                            "../../../../evil.txt"));
    // The other arm of the same guard: a relative filename that resolves back INSIDE the subdir
    // is accepted, so a crude "reject anything containing .." is not a passing implementation.
    error.clear();
    QCOMPARE(
        store.artifactPath(QStringLiteral("downloads"), QStringLiteral("nested/../ok.txt"), &error),
        downloads_dir + QStringLiteral("/ok.txt"));
    QVERIFY2(error.isEmpty(), qPrintable(error));
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

void AiConversationStoreTests::caseOnlyRename_preservesArtifacts() {
    // P08-02: a case-only title change ("Foo" -> "foo") maps to the same physical
    // artifact directory on a case-insensitive filesystem; the rename must not
    // merge-then-delete that shared directory and destroy the artifacts.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY(store.startSession(QStringLiteral("Foo"), &error));
    const QString log =
        store.artifactPath(QStringLiteral("logs"), QStringLiteral("keep.txt"), &error);
    QVERIFY2(!log.isEmpty(), qPrintable(error));
    QFile file(log);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("keep");
    file.close();

    QVERIFY2(store.renameCurrentSession(QStringLiteral("foo"), &error), qPrintable(error));
    QCOMPARE(store.currentSessionInfo().title, QStringLiteral("foo"));

    const QString root = store.artifactRootDirectory(&error);
    QVERIFY2(!root.isEmpty(), qPrintable(error));
    QVERIFY(QFile::exists(QDir(root).filePath(QStringLiteral("logs/keep.txt"))));
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
    QVERIFY2(error.isEmpty(), qPrintable(error));
    // Entries are APPENDED in order, each under its own "<iso> - <kind> - <title>" heading:
    // membership alone survives a store that prepends, or that drops the kind/title heading and
    // leaves the two bodies run together with no attribution.
    const qsizetype first_at = memory.indexOf(QStringLiteral(" - User - Request\ncheck my drive"));
    const qsizetype second_at = memory.indexOf(QStringLiteral(" - Assistant - Finding\nSMART OK"));
    QVERIFY(first_at >= 0);
    QVERIFY(second_at > first_at);
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

namespace {

// All SIX sections must survive the trim, in memorySectionNames() order, each with its body.
// "Decisions" and "Artifacts" were entirely unchecked, so a compactor that dropped them --
// losing exactly the record of what was already decided and produced -- stayed green.
// Returns the offset of "## Resolved History", which the caller needs to place the marker.
qsizetype verifyMemorySectionsInOrder(const QString& text) {
    const qsizetype pinned_at = text.indexOf(QStringLiteral("## Pinned Facts"));
    const qsizetype task_at = text.indexOf(QStringLiteral("## Current Task"));
    const qsizetype decisions_at = text.indexOf(QStringLiteral("## Decisions"));
    const qsizetype questions_at = text.indexOf(QStringLiteral("## Open Questions"));
    const qsizetype artifacts_at = text.indexOf(QStringLiteral("## Artifacts"));
    const qsizetype resolved_at = text.indexOf(QStringLiteral("## Resolved History"));
    [&] {
        QVERIFY(pinned_at >= 0);
        QVERIFY(task_at > pinned_at);
        QVERIFY(decisions_at > task_at);
        QVERIFY(questions_at > decisions_at);
        QVERIFY(artifacts_at > questions_at);
        QVERIFY(resolved_at > artifacts_at);
    }();
    return resolved_at;
}

}  // namespace

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
    const qsizetype resolved_at = verifyMemorySectionsInOrder(text);
    QVERIFY(text.contains(QStringLiteral("Preserve Randy's package preference")));
    QVERIFY(text.contains(QStringLiteral("Build the offline installer bundle")));
    QVERIFY(text.contains(QStringLiteral("Use SAK built-in downloader first")));
    QVERIFY(text.contains(QStringLiteral("Confirm destination drive")));
    QVERIFY(text.contains(QStringLiteral("reports/session.md")));
    // The compaction marker belongs to Resolved History (the only oversized section), and
    // compaction keeps the NEWEST history while dropping the oldest.
    QVERIFY(text.indexOf(QStringLiteral("[older section content compacted by SAK]")) > resolved_at);
    QVERIFY(!text.contains(QStringLiteral("resolved detail 00 with")));
    QVERIFY(text.contains(QStringLiteral("resolved detail 8999 with")));
    QVERIFY(text.contains(QStringLiteral("Latest preserved finding")));
    QVERIFY(QFileInfo(file.fileName()).size() <= 256 * 1024);
}

void AiConversationStoreTests::searchSessions_findsTranscriptAndCommandIndex() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY(store.startSession(QStringLiteral("Spyware scan QA"), &error));
    QVERIFY(store.appendTranscript(
        QStringLiteral("You"), QStringLiteral("Run SUPERAntiSpyware scan"), {}, &error));
    QVERIFY(store.appendCommand(QStringLiteral("Get-Process SUPERAntiSpyware"),
                                QJsonObject{{QStringLiteral("success"), false},
                                            {QStringLiteral("error_message"),
                                             QStringLiteral("health_suppressed")}},
                                &error));

    const auto results = store.searchSessions(QStringLiteral("SUPERAntiSpyware"), 10, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    // Exactly two hits: the transcript line and the command index entry. `>= 2` would miss a
    // duplicate/spurious extra hit.
    QCOMPARE(results.size(), 2);
    QFile index_file(store.currentSessionInfo().path + QStringLiteral("/search_index.jsonl"));
    QVERIFY(index_file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QJsonObject index_line =
        QJsonDocument::fromJson(index_file.readLine().trimmed()).object();
    QCOMPARE(index_line.value(QStringLiteral("schema_version")).toInt(), 1);
    QCOMPARE(index_line.value(QStringLiteral("source")).toString(), QStringLiteral("transcript"));
    QCOMPARE(index_line.value(QStringLiteral("role")).toString(), QStringLiteral("You"));
    QCOMPARE(index_line.value(QStringLiteral("text")).toString(),
             QStringLiteral("You Run SUPERAntiSpyware scan"));
    // Both snippets are fully determined (shorter than the 220-char snippet cap, so they are the
    // whole simplified indexed text) and each query term occurs exactly once.
    const auto transcript_hit = std::find_if(results.cbegin(), results.cend(), [](const auto& hit) {
        return hit.source == QStringLiteral("transcript");
    });
    QVERIFY(transcript_hit != results.cend());
    QCOMPARE(transcript_hit->snippet, QStringLiteral("You Run SUPERAntiSpyware scan"));
    QCOMPARE(transcript_hit->score, 1);
    QCOMPARE(transcript_hit->session.id, store.currentSessionId());
    const auto command_hit = std::find_if(results.cbegin(), results.cend(), [](const auto& hit) {
        return hit.source == QStringLiteral("command");
    });
    QVERIFY(command_hit != results.cend());
    QCOMPARE(command_hit->snippet,
             QStringLiteral("Get-Process SUPERAntiSpyware "
                            "{\"error_message\":\"health_suppressed\",\"success\":false}"));
    QCOMPARE(command_hit->score, 1);
    QCOMPARE(command_hit->session.id, store.currentSessionId());
    QVERIFY(std::any_of(results.cbegin(), results.cend(), [](const auto& hit) {
        return hit.source == QStringLiteral("transcript") &&
               hit.snippet.contains(QStringLiteral("SUPERAntiSpyware"));
    }));
    QVERIFY(std::any_of(results.cbegin(), results.cend(), [](const auto& hit) {
        return hit.source == QStringLiteral("command") &&
               hit.snippet.contains(QStringLiteral("SUPERAntiSpyware"));
    }));
}

void AiConversationStoreTests::appendCommand_redactsSecretsInPersistedRecord() {
    // P08-20 / P08-21: command records must not persist raw credentials.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY(store.startSession(QStringLiteral("Secrets"), &error));

    const QString secret = QStringLiteral("ctx7") + QStringLiteral("s") +
                           QStringLiteral("k-fc513191-580d-40c0-b244-17ea71f182b9");
    QVERIFY(store.appendCommand(QStringLiteral("Invoke-RestMethod -Headers @{ Authorization = '") +
                                    secret + QStringLiteral("' }"),
                                QJsonObject{{QStringLiteral("success"), true}},
                                &error));

    QFile commands(store.currentSessionInfo().path + QStringLiteral("/commands.jsonl"));
    QVERIFY(commands.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray commands_bytes = commands.readAll();
    QVERIFY(!QString::fromUtf8(commands_bytes).contains(secret));
    // Not merely "the secret is gone": the record must persist the EXACT redacted form, so a
    // command that was dropped, blanked or wholly replaced cannot pass for redaction.
    const QString redacted =
        QStringLiteral("Invoke-RestMethod -Headers @{ Authorization = '[redacted-context7-key]' }");
    const QJsonObject record = QJsonDocument::fromJson(commands_bytes.trimmed()).object();
    QCOMPARE(record.value(QStringLiteral("command")).toString(), redacted);
    QVERIFY(record.value(QStringLiteral("result"))
                .toObject()
                .value(QStringLiteral("success"))
                .toBool());
    // appendCommand persists the command at TWO sites; the search-index copy is the second, and
    // searchSessions hands it back to the model verbatim as a snippet.
    QFile index(store.currentSessionInfo().path + QStringLiteral("/search_index.jsonl"));
    QVERIFY(index.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString index_text = QString::fromUtf8(index.readAll());
    QVERIFY(!index_text.contains(secret));
    QVERIFY(index_text.contains(QStringLiteral("[redacted-context7-key]")));
}

void AiConversationStoreTests::concurrentReadersAndWriterDoNotDeadlockOrCorrupt() {
    // ConversationStore is reached from more than one thread: acting subagents run
    // allowlisted store-backed tools (session_search, artifact/download paths) on a
    // workflow WORKER thread while the GUI thread appends transcripts and mutates
    // the current-session record. Its recursive read/write lock must (a) never
    // deadlock -- including the nested-read chains artifactPath -> artifactSubdir ->
    // artifactRootDirectory -- and (b) keep every m_current_session read internally
    // consistent (no torn id/title/path). This test hangs on a locking bug and the
    // suite times out, which is the deadlock signal.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY(store.startSession(QStringLiteral("Concurrent"), &error));
    const QString session_id = store.currentSessionId();
    QVERIFY(!session_id.isEmpty());

    constexpr int kIterations = 300;
    std::atomic<bool> torn_read{false};

    auto reader = [&store, &session_id, &torn_read]() {
        for (int i = 0; i < kIterations; ++i) {
            const sak::ai::AiSessionInfo info = store.currentSessionInfo();
            // The session is never cleared or renamed here, so a snapshot must be
            // internally consistent: a torn read could pair a stale id with a fresh
            // path, or an empty id with a non-empty path.
            if (info.id != session_id || info.path.isEmpty()) {
                torn_read = true;
            }
            QString err;
            (void)store.artifactRootDirectory(&err);  // nested-read chain
            (void)store.artifactPath(QStringLiteral("downloads"), QStringLiteral("f.bin"), &err);
            (void)store.currentSessionId();
            (void)store.searchSessions(QStringLiteral("disk"), 10, &err);
        }
    };

    std::vector<std::thread> readers;
    readers.reserve(4);
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back(reader);
    }
    // Writer on this thread mirrors the GUI thread appending transcripts/commands.
    for (int i = 0; i < kIterations; ++i) {
        QString err;
        (void)store.appendTranscript(
            QStringLiteral("You"), QStringLiteral("disk check %1").arg(i), {}, &err);
    }
    for (auto& th : readers) {
        th.join();
    }

    QVERIFY(!torn_read.load());
    // The store is still consistent and usable after the hammering.
    QCOMPARE(store.currentSessionId(), session_id);
    const QString root = store.artifactRootDirectory(&error);
    QVERIFY2(!root.isEmpty(), qPrintable(error));
}

QTEST_GUILESS_MAIN(AiConversationStoreTests)
#include "test_ai_conversation_store.moc"

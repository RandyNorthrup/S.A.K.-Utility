// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_conversation_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
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
    void renameSession_rollsBackTheArtifactMoveWhenTheManifestFails();
    void renameSession_reportsAMergeItCannotUndoWhenTheManifestFails();
    void startSession_doesNotPresentASessionThatFailedToPersist();
    void caseOnlyRename_preservesArtifacts();
    void memoryFile_appendsEntries();
    void memoryFile_initializesStructuredSections();
    void memoryFile_trimPreservesStructuredSections();
    void searchSessions_findsTranscriptAndCommandIndex();
    void appendCommand_redactsSecretsInPersistedRecord();
    void appendCommand_redactsSecretsInTheResultToo();
    void artifactSubdir_confinesSubdirToTheArtifactRoot();
    void artifactPath_rejectsAFilenameThatNamesTheDirectory();
    void safeArtifactDirectoryName_rejectsReservedAndTrailingDotNames();
    void memoryFile_trimActuallyGetsUnderTheCapForNonAsciiMemory();
    void searchFallsBackToRawLogsWhenTheIndexIsKnownIncomplete();
    void concurrentReadersAndWriterDoNotDeadlockOrCorrupt();
};

namespace {

// The index is the PRIMARY source and the raw logs are only the FALLBACK, but the fixture
// makes them produce byte-identical hits -- the index text and the raw scan build the same
// string -- so no assertion in the caller can tell WHICH source answered, and either path
// could be deleted with everything still green. Isolate them one at a time.
void verifySearchAnswersFromIndexAndFromRawLogs(const sak::ai::ConversationStore& store,
                                                QFile& index_file,
                                                QString& error) {
    // The index is the PRIMARY source and the raw logs are only the FALLBACK, but this fixture
    // makes them produce byte-identical hits -- the index text and the raw scan build the same
    // "%1 %2" string -- so no assertion above can tell WHICH source answered, and either path
    // could be deleted with everything still green.
    const auto snippetFor = [](const auto& hits, const QString& source) {
        const auto it = std::find_if(hits.cbegin(), hits.cend(), [&source](const auto& hit) {
            return hit.source == source;
        });
        return it == hits.cend() ? QString() : it->snippet;
    };
    const QString session_dir = store.currentSessionInfo().path;
    const QString transcript_path = session_dir + QStringLiteral("/transcript.jsonl");
    const QString commands_path = session_dir + QStringLiteral("/commands.jsonl");
    const QString index_path = session_dir + QStringLiteral("/search_index.jsonl");
    index_file.close();

    // (a) With the raw logs moved aside, only a real index READ can still answer. An index read
    // that bails out (missing, oversized, or open-failure) turns this into zero hits.
    QVERIFY(QFile::rename(transcript_path, transcript_path + QStringLiteral(".off")));
    QVERIFY(QFile::rename(commands_path, commands_path + QStringLiteral(".off")));
    const auto index_only = store.searchSessions(QStringLiteral("SUPERAntiSpyware"), 10, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(index_only.size(), 2);
    QCOMPARE(snippetFor(index_only, QStringLiteral("transcript")),
             QStringLiteral("You Run SUPERAntiSpyware scan"));
    QCOMPARE(snippetFor(index_only, QStringLiteral("command")),
             QStringLiteral("Get-Process SUPERAntiSpyware "
                            "{\"error_message\":\"health_suppressed\",\"success\":false}"));

    // (b) And the fallback must really work for a session with NO index -- a pre-index session,
    // or one whose index grew past the size cap: restore the raw logs, drop the index, same two.
    QVERIFY(QFile::rename(transcript_path + QStringLiteral(".off"), transcript_path));
    QVERIFY(QFile::rename(commands_path + QStringLiteral(".off"), commands_path));
    QVERIFY(QFile::remove(index_path));
    const auto raw_only = store.searchSessions(QStringLiteral("SUPERAntiSpyware"), 10, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(raw_only.size(), 2);
    QCOMPARE(snippetFor(raw_only, QStringLiteral("transcript")),
             QStringLiteral("You Run SUPERAntiSpyware scan"));
    QCOMPARE(snippetFor(raw_only, QStringLiteral("command")),
             QStringLiteral("Get-Process SUPERAntiSpyware "
                            "{\"error_message\":\"health_suppressed\",\"success\":false}"));
}

// The writer half of the concurrency fixture discards all of its return values and nothing
// downstream could see a failed append -- the test never reads the transcript back -- so if
// every append had silently failed the readers would still find a consistent session and
// every assertion would still pass, leaving that half a green no-op. The results are
// deliberately not checked one-by-one: the searches run unlocked on the reader threads, so a
// manifest commit can lose a race and return false even when the record was persisted. The
// corpus it builds must still be real -- the transcript line is written before the manifest.
void verifyWriterHalfBuiltARealCorpus(const sak::ai::ConversationStore& store,
                                      const QString& session_id,
                                      int iterations,
                                      QString& error) {
    // Pin the WRITER half of the fixture too. Its 300 return values are discarded and nothing
    // downstream could see a failed append -- the test never reads the transcript back -- so if
    // every append had silently failed the readers would still find a consistent session and
    // every assertion here would still pass, leaving that half a green no-op. The results are
    // deliberately not checked one-by-one: the searches run unlocked on the reader threads, so a
    // manifest commit can lose a race and return false even when the record was persisted. The
    // corpus it builds must still be real -- the transcript line is written before the manifest
    // is reached.
    const auto written = store.loadTranscriptLines(session_id, &error);
    QCOMPARE(written.size(), iterations);
    QVERIFY2(written.constLast().endsWith(QStringLiteral("disk check %1").arg(iterations - 1)),
             qPrintable(written.constLast()));
}

// One reader thread's body. It checks not only that a snapshot is internally consistent but
// that the artifact-path RESULTS stay correct: every refusal arm in that nested read chain
// returns an empty string with an error set, so a store that refused EVERY concurrent
// request would score exactly like a healthy one when the results are discarded.
struct ContentionReaderContext {
    sak::ai::ConversationStore& store;
    const QString& session_id;
    int iterations;
    std::atomic<bool>& torn_read;
    std::atomic<bool>& bad_artifact_read;
    const QString& expected_root;
    const QString& expected_artifact;
};

auto makeContentionReader(const ContentionReaderContext& ctx) {
    return [&ctx]() {
        for (int i = 0; i < ctx.iterations; ++i) {
            const sak::ai::AiSessionInfo info = ctx.store.currentSessionInfo();
            // The session is never cleared or renamed here, so a snapshot must be
            // internally consistent: a torn read could pair a stale id with a fresh
            // path, or an empty id with a non-empty path.
            if (info.id != ctx.session_id || info.path.isEmpty()) {
                ctx.torn_read = true;
            }
            QString err;
            const QString root = ctx.store.artifactRootDirectory(&err);  // nested-read chain
            const QString artifact =
                ctx.store.artifactPath(QStringLiteral("downloads"), QStringLiteral("f.bin"), &err);
            if (root != ctx.expected_root || artifact != ctx.expected_artifact || !err.isEmpty()) {
                ctx.bad_artifact_read = true;
            }
            (void)ctx.store.currentSessionId();
            (void)ctx.store.searchSessions(QStringLiteral("disk"), 10, &err);
        }
    };
}

}  // namespace

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

    // Both loop guards must be PROVED, not merely present. As written the winning id was simply
    // "the last line in the file", so neither the role filter nor the non-empty-id guard was
    // load-bearing: the Tool Result line planted to prove the role filter sits BEFORE the final
    // assistant line. A LATER Tool Result carrying an id proves the role filter, and a LATER
    // assistant line whose id is whitespace-only proves the trim-and-non-empty guard -- a
    // streamed turn not yet assigned an id must not blank the id the next API call chains on.
    QVERIFY(
        store.appendTranscript(QStringLiteral("Assistant"),
                               QStringLiteral("streaming chunk with no id yet"),
                               QJsonObject{{QStringLiteral("response_id"), QStringLiteral("   ")}},
                               &error));
    QVERIFY(store.appendTranscript(QStringLiteral("Tool Result"),
                                   QStringLiteral("later tool result"),
                                   QJsonObject{{QStringLiteral("response_id"),
                                                QStringLiteral("resp_tool_last")}},
                                   &error));

    QCOMPARE(store.latestAssistantResponseId(store.currentSessionId(), &error),
             QStringLiteral("resp_new"));
    QVERIFY(error.isEmpty());

    // A successful lookup must CLEAR the caller's error slot, not merely leave it alone. `error`
    // was only ever written on failure and every call above succeeded, so the assertion above
    // would have passed BEFORE the call. Callers reuse one QString across store calls, so a
    // stale message left in place is reported against the wrong operation.
    error = QStringLiteral("stale failure from an earlier call");
    QCOMPARE(store.latestAssistantResponseId(store.currentSessionId(), &error),
             QStringLiteral("resp_new"));
    QVERIFY2(error.isEmpty(), qPrintable(error));

    // A session with no transcript is "no response id", NOT a failure: the missing-file arm must
    // clear the slot too, or the caller reports a phantom error for a lookup that succeeded.
    error = QStringLiteral("stale failure from an earlier call");
    QVERIFY(
        store.latestAssistantResponseId(QStringLiteral("ai_no_such_session"), &error).isEmpty());
    QVERIFY2(error.isEmpty(), qPrintable(error));
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

    // The filter accepts EITHER role spelling: the panel writes "You", while an API-side or
    // imported transcript writes "user". Every session above uses "You", so the second
    // alternative was dead weight that could be deleted silently -- and coverage measures it as
    // never once true. Dropping it makes such a chat vanish from the session picker with no
    // error at all: a silently unlistable chat.
    QVERIFY(store.startSession(QStringLiteral("ApiRole"), &error));
    const QString api_role_id = store.currentSessionId();
    QVERIFY(!api_role_id.isEmpty());
    QVERIFY(store.appendTranscript(QStringLiteral("user"), QStringLiteral("run scan"), {}, &error));

    const auto with_api_role = store.listPromptedSessions();
    QCOMPARE(with_api_role.size(), 2);
    QVERIFY(std::any_of(with_api_role.cbegin(), with_api_role.cend(), [&](const auto& session) {
        return session.id == api_role_id;
    }));
    QVERIFY(std::any_of(with_api_role.cbegin(), with_api_role.cend(), [&](const auto& session) {
        return session.id == prompted_id;
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
    // Every other field here is pinned exactly; the timestamp alone was checked only for being
    // non-empty, which any placeholder satisfies. Its instant is not knowable but its FORM is:
    // the store writes Qt::ISODateWithMs, the same spec session dates are parsed back with.
    const QString usage_stamp = usage_root.value(QStringLiteral("updated_at")).toString();
    const QDateTime usage_written_at = QDateTime::fromString(usage_stamp, Qt::ISODateWithMs);
    QVERIFY2(usage_written_at.isValid(), qPrintable(usage_stamp));
    // ...and it is THIS write's stamp, not a frozen literal that merely parses.
    QVERIFY2(qAbs(usage_written_at.msecsTo(QDateTime::currentDateTimeUtc())) < 60'000,
             qPrintable(usage_stamp));
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
    // The whole path is knowable, and a tail match cannot see WHICH directory the artifacts tree
    // hangs off -- which is exactly what confines one chat's logs to its own session folder.
    QCOMPARE(stdout_path,
             store.currentSessionInfo().path +
                 QStringLiteral("/artifacts/Logs/logs/cmd_001_stdout.txt"));

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

    // The sanitizer takes a per-call fallback -- "cmd" for the id, "output" for the suffix -- and
    // no fixture ever sanitized a token down to nothing, so neither fallback was ever the source
    // of the name and coverage measures both arms of that branch as unreached. A token that
    // sanitizes away entirely must fall back to its OWN default, so an emptied id or suffix
    // still names a distinct, non-degenerate file instead of every such command sharing one.
    const QString fallback_path =
        store.commandLogPath(QStringLiteral("."), QStringLiteral("  "), &error);
    QVERIFY2(!fallback_path.isEmpty(), qPrintable(error));
    QCOMPARE(fallback_path,
             store.currentSessionInfo().path +
                 QStringLiteral("/artifacts/Logs/logs/cmd_output.txt"));
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
    // EVERY member of the reserved class is replaced, not just the path separators -- '/' was
    // the only one probed anywhere in this file, and the preserved-title case contains none at
    // all. A shrunken class leaves ':' / '?' / '*' in the directory name, mkpath then fails, and
    // every artifact write for that chat turns into a fail-closed empty path.
    QCOMPARE(sak::ai::ConversationStore::safeArtifactDirectoryName(
                 QStringLiteral(R"(Report: <v2>?"x"|y*z\w/q)"), QStringLiteral("ai_x")),
             QStringLiteral("Report_ _v2___x__y_z_w_q"));
    // ...and the length cap actually truncates; it was probed nowhere. The bound lives in the
    // .cpp's anonymous namespace, so pin its value here by literal.
    constexpr qsizetype kExpectedMaxChars = 80;
    QCOMPARE(sak::ai::ConversationStore::safeArtifactDirectoryName(
                 QString(kExpectedMaxChars * 2, QLatin1Char('a')), QStringLiteral("ai_x")),
             QString(kExpectedMaxChars, QLatin1Char('a')));
}

void AiConversationStoreTests::safeArtifactDirectoryName_rejectsReservedAndTrailingDotNames() {
    const auto name = [](const QString& title) {
        return sak::ai::ConversationStore::safeArtifactDirectoryName(title, QStringLiteral("ai_x"));
    };

    // WINDOWS SILENTLY STRIPS a trailing dot or space from a path component, so "Report." and
    // "Report" are THE SAME DIRECTORY. Two sessions titled that way shared one artifact folder
    // and overwrote each other's files. The computed name must be the name the filesystem uses.
    QCOMPARE(name(QStringLiteral("Report.")), QStringLiteral("Report"));
    QCOMPARE(name(QStringLiteral("Report...")), QStringLiteral("Report"));
    QCOMPARE(name(QStringLiteral("Report")), QStringLiteral("Report"));
    // An interior dot is ordinary and must survive -- this is not "strip all dots".
    QCOMPARE(name(QStringLiteral("v1.2 notes")), QStringLiteral("v1.2 notes"));

    // The DOS device names are reserved for every extension and in every case, so mkpath on one
    // FAILS -- artifactRootDirectory then returns empty and every artifact write in that session
    // fails for a reason no message explains. "CON" is a perfectly ordinary chat title.
    for (const QString& reserved : {QStringLiteral("CON"),
                                    QStringLiteral("con"),
                                    QStringLiteral("Aux"),
                                    QStringLiteral("NUL"),
                                    QStringLiteral("COM1"),
                                    QStringLiteral("lpt9"),
                                    QStringLiteral("NUL.txt"),
                                    QStringLiteral("CON.")}) {
        QCOMPARE(name(reserved), QStringLiteral("AI Session"));
    }
    // Near-misses are NOT reserved and must keep their titles, or the guard is just deleting
    // legitimate names.
    for (const QString& ordinary : {QStringLiteral("CONSOLE"),
                                    QStringLiteral("COM0"),
                                    QStringLiteral("COM12"),
                                    QStringLiteral("LPT"),
                                    QStringLiteral("AUXILIARY")}) {
        QCOMPARE(name(ordinary), ordinary);
    }
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
    // The containment assertions further down are all RELATIVE to downloads_dir, so they move
    // with a mis-rooted artifact tree; nothing here named the session directory the artifacts
    // must live under. The whole path is knowable, so pin it.
    QCOMPARE(screenshot_path,
             store.currentSessionInfo().path +
                 QStringLiteral("/artifacts/Artifacts/screenshots/shot_001.png"));
    QVERIFY(QFileInfo(screenshot_path).absoluteDir().exists());

    const QString downloads_dir = store.artifactSubdir(QStringLiteral("downloads"), &error);
    QVERIFY2(!downloads_dir.isEmpty(), qPrintable(error));
    QVERIFY(QDir(downloads_dir).exists());
    // Pin WHICH directory came back, not merely that it is non-empty: it must be the managed
    // subdirectory under THIS session's artifact root.
    QCOMPARE(downloads_dir,
             store.currentSessionInfo().path + QStringLiteral("/artifacts/Artifacts/downloads"));
    // An empty or whitespace-only subdir must be REFUSED, an arm coverage measures as never
    // taken. The guard is not decorative: QDir::filePath({}) returns the directory itself
    // unchanged and mkpath on an existing directory succeeds, so without it artifactSubdir hands
    // back the ARTIFACT ROOT and a model-chosen filename lands beside the managed subdirectories.
    error.clear();
    QVERIFY(store.artifactSubdir(QStringLiteral("   "), &error).isEmpty());
    QCOMPARE(error, QStringLiteral("Artifact subdir is empty"));
    error.clear();
    QVERIFY(store.artifactSubdir(QString(), &error).isEmpty());
    QCOMPARE(error, QStringLiteral("Artifact subdir is empty"));
    error.clear();

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
    // currentSessionInfo() is the IN-MEMORY record, updated before anything is persisted, so
    // the compare above cannot see the persistence half of the rename. The session picker
    // rebuilds itself from the manifest, so a rename that never reaches disk shows the OLD title
    // again after a restart. Both stamps are pinned as well: a rename is the only point where
    // created_at and updated_at diverge, so a swapped write is invisible everywhere else.
    const auto persisted = store.listSessions();
    QCOMPARE(persisted.size(), 1);
    QCOMPARE(persisted.first().title, QStringLiteral("Drive Check / SSD"));
    QCOMPARE(persisted.first().created_at, store.currentSessionInfo().created_at);
    QCOMPARE(persisted.first().updated_at, store.currentSessionInfo().updated_at);

    const QString root = store.artifactRootDirectory(&error);
    QVERIFY2(!root.isEmpty(), qPrintable(error));
    QVERIFY(root.endsWith(QStringLiteral("/artifacts/Drive Check _ SSD")));
    QVERIFY(QDir(root).exists());
    QVERIFY(QFile::exists(QDir(root).filePath(QStringLiteral("logs/before.txt"))));
}

namespace {

/// Make writeManifest fail for a session by putting a DIRECTORY where its manifest file goes.
/// QSaveFile cannot create a file over a directory, and this needs no fault-injection seam in
/// production code -- the filesystem does it.
[[nodiscard]] bool blockManifestWrites(const QString& session_path) {
    const QString manifest = QDir(session_path).filePath(QStringLiteral("manifest.json"));
    QFile::remove(manifest);
    return QDir().mkpath(manifest);
}

/// Read a file whole, returning an empty array if it cannot be opened. Used to assert that an
/// artifact still holds its BYTES after a rollback, which QFile::exists alone cannot tell you.
[[nodiscard]] QByteArray readFileContents(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

}  // namespace

void AiConversationStoreTests::renameSession_rollsBackTheArtifactMoveWhenTheManifestFails() {
    // The artifact directory is MOVED BEFORE the manifest is written, and readers derive that
    // directory from the CURRENT title. So a manifest write that fails after the move used to
    // leave the manifest naming the old title while every artifact sat under the new one --
    // reachable by nothing, with the rename reported as a plain failure.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY(store.startSession(QStringLiteral("Old"), &error));
    const QString session_path = store.currentSessionInfo().path;

    const QString log =
        store.artifactPath(QStringLiteral("logs"), QStringLiteral("before.txt"), &error);
    QVERIFY2(!log.isEmpty(), qPrintable(error));
    QFile f(log);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("before");
    f.close();

    QVERIFY(blockManifestWrites(session_path));

    error.clear();
    QVERIFY2(!store.renameCurrentSession(QStringLiteral("New"), &error),
             "a rename whose manifest cannot be written must fail");
    QVERIFY(!error.isEmpty());

    // THE IN-MEMORY RECORD IS RESTORED: the store must not claim a title it did not persist.
    QCOMPARE(store.currentSessionInfo().title, QStringLiteral("Old"));

    // AND THE DIRECTORY MOVE IS UNDONE, which is the half that loses data. Read the artifact back
    // rather than only asking whether the path exists: a rollback that recreated an empty tree
    // would satisfy an existence check while having lost the very bytes this guard protects.
    const QString artifacts = QDir(session_path).filePath(QStringLiteral("artifacts"));
    const QString restored = QDir(artifacts).filePath(QStringLiteral("Old/logs/before.txt"));
    QVERIFY2(QFile::exists(restored),
             qPrintable(QDir(artifacts).entryList(QDir::Dirs | QDir::NoDotAndDotDot).join(',')));
    QCOMPARE(readFileContents(restored), QByteArrayLiteral("before"));
    QVERIFY(!QDir(QDir(artifacts).filePath(QStringLiteral("New"))).exists());

    // NON-VACUITY: with manifest writes working again the SAME rename succeeds and does move the
    // directory, so the assertions above pin the rollback and not a rename that never happened.
    QVERIFY(QDir(QDir(session_path).filePath(QStringLiteral("manifest.json"))).removeRecursively());
    error.clear();
    QVERIFY2(store.renameCurrentSession(QStringLiteral("New"), &error), qPrintable(error));
    QCOMPARE(store.currentSessionInfo().title, QStringLiteral("New"));
    QVERIFY(QFile::exists(QDir(artifacts).filePath(QStringLiteral("New/logs/before.txt"))));
}

void AiConversationStoreTests::renameSession_reportsAMergeItCannotUndoWhenTheManifestFails() {
    // The other half of the rollback contract. When the destination directory ALREADY EXISTS the
    // rename degrades to a merge, and a merge cannot be undone: renaming back would not separate
    // the entries again, and nothing may delete artifacts to make the failure look clean. So this
    // path must NOT roll back -- it must say exactly where the artifacts ended up and what the
    // session is still called, because only a human can reconcile that.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY(store.startSession(QStringLiteral("Old"), &error));
    const QString session_path = store.currentSessionInfo().path;

    const QString log =
        store.artifactPath(QStringLiteral("logs"), QStringLiteral("before.txt"), &error);
    QVERIFY2(!log.isEmpty(), qPrintable(error));
    QFile f(log);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("before");
    f.close();

    // Pre-create the destination so the plain rename cannot be used and the merge arm is taken.
    const QString artifacts = QDir(session_path).filePath(QStringLiteral("artifacts"));
    QVERIFY(QDir().mkpath(QDir(artifacts).filePath(QStringLiteral("New/other"))));

    QVERIFY(blockManifestWrites(session_path));

    error.clear();
    QVERIFY2(!store.renameCurrentSession(QStringLiteral("New"), &error),
             "a rename whose manifest cannot be written must fail");

    // THE ERROR NAMES THE RESIDUAL STATE: that a merge happened, where the artifacts now are, and
    // the title the session still carries. A bare "rename failed" would send someone looking for
    // the artifacts under a name that no longer holds them.
    QVERIFY2(error.contains(QStringLiteral("merged")), qPrintable(error));
    QVERIFY2(error.contains(QStringLiteral("New")), qPrintable(error));
    QVERIFY2(error.contains(QStringLiteral("Old")), qPrintable(error));

    // The in-memory title is still restored -- that half is not conditional on the outcome.
    QCOMPARE(store.currentSessionInfo().title, QStringLiteral("Old"));

    // AND NOTHING WAS DESTROYED TO TIDY UP: the merged bytes are under the new name, where the
    // error says they are, and the pre-existing entry that forced the merge is untouched.
    QCOMPARE(readFileContents(QDir(artifacts).filePath(QStringLiteral("New/logs/before.txt"))),
             QByteArrayLiteral("before"));
    QVERIFY(QDir(QDir(artifacts).filePath(QStringLiteral("New/other"))).exists());
}

void AiConversationStoreTests::startSession_doesNotPresentASessionThatFailedToPersist() {
    // The manifest is what makes a session exist: listSessions and openSession both read it. So
    // a startSession whose manifest write fails must not leave the new id as the current session
    // -- the caller would hold a currentSessionId() no reader can find, and the panel's failure
    // path returns WITHOUT clearing it, so every later append targets a non-session directory.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY(store.startSession(QStringLiteral("First"), &error));
    const QString first_id = store.currentSessionInfo().id;
    QVERIFY(!first_id.isEmpty());

    // Deny ADD_FILE on the root while leaving ADD_SUBDIRECTORY allowed, so the new session's
    // mkpath still succeeds and only the manifest FILE write fails -- which is exactly the
    // branch under test. (icacls (WD) is add-file; (AD) is add-subdirectory.)
    const QString me = qEnvironmentVariable("USERNAME");
    QVERIFY(!me.isEmpty());
    QProcess deny;
    deny.start(QStringLiteral("icacls"),
               {QDir::toNativeSeparators(temp.path()),
                QStringLiteral("/deny"),
                me + QStringLiteral(":(OI)(CI)(WD)")});
    QVERIFY(deny.waitForFinished(30'000));

    error.clear();
    const bool started = store.startSession(QStringLiteral("Second"), &error);

    QProcess allow;  // restore before asserting, so a failed assertion cannot leave the dir locked
    allow.start(QStringLiteral("icacls"),
                {QDir::toNativeSeparators(temp.path()), QStringLiteral("/remove:d"), me});
    QVERIFY(allow.waitForFinished(30'000));

    if (started) {
        QSKIP("the ACL did not block manifest creation on this filesystem; branch not exercised");
    }
    // The failed session is NOT presented: the previous session is still the current one.
    QCOMPARE(store.currentSessionInfo().id, first_id);
    QCOMPARE(store.currentSessionInfo().title, QStringLiteral("First"));
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

    // The cap is memoryText's whole contract for the prompt builder -- keep the NEWEST max_chars
    // and announce the omission, so a memory file allowed to grow cannot flood the context
    // window -- and no call anywhere in this file gets near it: every one passes 16'000 against
    // a few hundred characters, which coverage confirms leaves that arm never taken. Deleting
    // the cap outright stayed green.
    const QString capped = store.memoryText(40, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(capped.startsWith(QStringLiteral("[older working memory omitted]")));
    QVERIFY(capped.size() < memory.size());
    // The NEWEST entry survives and the OLDEST content is what gets dropped -- a left() instead
    // of a right() would keep the marker but lose this and keep the file header.
    QVERIFY(capped.contains(QStringLiteral("SMART OK")));
    QVERIFY(!capped.contains(QStringLiteral("check my drive")));
    QVERIFY(!capped.contains(QStringLiteral("# Session Working Memory")));
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
    // The header is a fixed literal and this file was just initialized from it, so the whole
    // prefix is knowable byte-for-byte. Six contains() calls pinned neither the section ORDER
    // nor the "- _none_" placeholders that tell the model a section is empty rather than
    // missing -- reordering the literal, or dropping every placeholder body, stayed green.
    const QString expected_header = QStringLiteral(
        "# Session Working Memory\n\n"
        "## Pinned Facts\n- _none_\n\n"
        "## Current Task\n- _none_\n\n"
        "## Decisions\n- _none_\n\n"
        "## Open Questions\n- _none_\n\n"
        "## Artifacts\n- _none_\n\n"
        "## Resolved History\n\n");
    QVERIFY2(memory.startsWith(expected_header), qPrintable(memory.left(400)));
    // The appended entry lands under Resolved History, i.e. after the entire header.
    const qsizetype entry_at = memory.indexOf(QStringLiteral(" - User - Request\ninstall firefox"));
    QVERIFY(entry_at >= expected_header.size());
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

    verifySearchAnswersFromIndexAndFromRawLogs(store, index_file, error);
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

void AiConversationStoreTests::appendCommand_redactsSecretsInTheResultToo() {
    // ONLY THE COMMAND USED TO BE REDACTED. `result` is the tool result -- the captured
    // stdout/stderr of the command -- and it was written verbatim into both the command log and
    // the search index, under a comment claiming this is "the single point every command record
    // and its search-index entry pass through". A token echoed by a command landed unredacted in
    // exactly the files that comment was about.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY(store.startSession(QStringLiteral("Result secrets"), &error));

    const QString echoed_token = QStringLiteral("ab+cd/efGHIJ0123+/xyz==");
    QJsonObject nested;
    nested[QStringLiteral("stdout")] = QStringLiteral("Authorization: Bearer ") + echoed_token;
    nested[QStringLiteral("exit_code")] = 0;
    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("detail")] = nested;
    result[QStringLiteral("password")] = QStringLiteral("hunter2value");

    QVERIFY(store.appendCommand(QStringLiteral("run the thing"), result, &error));

    const QString session_path = store.currentSessionInfo().path;
    for (const QString& file : {session_path + QStringLiteral("/commands.jsonl"),
                                session_path + QStringLiteral("/search_index.jsonl")}) {
        QFile persisted(file);
        QVERIFY2(persisted.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(file));
        const QString text = QString::fromUtf8(persisted.readAll());
        QVERIFY2(!text.contains(echoed_token), qPrintable(file + QStringLiteral(" -> ") + text));
        QVERIFY2(!text.contains(QStringLiteral("hunter2value")),
                 qPrintable(file + QStringLiteral(" -> ") + text));
    }

    // The record must remain a usable RECORD, not just a scrubbed one: the redaction is a tree
    // walk precisely so the document still parses and its non-secret fields survive.
    QFile commands(session_path + QStringLiteral("/commands.jsonl"));
    QVERIFY(commands.open(QIODevice::ReadOnly | QIODevice::Text));
    const QJsonObject record = QJsonDocument::fromJson(commands.readAll().trimmed()).object();
    const QJsonObject stored_result = record.value(QStringLiteral("result")).toObject();
    QVERIFY(stored_result.value(QStringLiteral("success")).toBool());
    QCOMPARE(stored_result.value(QStringLiteral("password")).toString(),
             QStringLiteral("[redacted]"));
    QCOMPARE(
        stored_result.value(QStringLiteral("detail")).toObject().value(QStringLiteral("exit_code")),
        QJsonValue(0));
}

void AiConversationStoreTests::memoryFile_trimActuallyGetsUnderTheCapForNonAsciiMemory() {
    // THE CAP IS BYTES, THE BUDGET WAS SPENT IN CHARACTERS. trimMemory compares
    // QFileInfo::size() -- bytes on disk -- against 256 KiB, then trimmed to a QString length
    // budget, and QString counts UTF-16 code units. For non-ASCII memory the two units disagree
    // badly: 192 Ki CHARACTERS of CJK is ~576 KiB of UTF-8, so the trim "succeeded" and left the
    // file FAR ABOVE the cap it exists to enforce -- and every later call trimmed to the same
    // oversized result, so it never converged.
    //
    // The existing trim test uses ASCII, where one character is one byte and the bug is
    // invisible. This one is deliberately non-ASCII.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY(store.startSession(QStringLiteral("Memory Trim Wide"), &error));

    // Three bytes per character in UTF-8, one QString character each.
    const QString wide_line = QString(200, QChar(0x6F22));  // CJK ideograph
    QStringList history;
    history.reserve(3000);
    for (int i = 0; i < 3000; ++i) {
        history << QStringLiteral("## entry %1\n%2").arg(i).arg(wide_line);
    }
    const QString memory = QStringLiteral(
                               "# Session Working Memory\n\n"
                               "## Pinned Facts\n- keep this\n\n"
                               "## Current Task\n- keep this too\n\n"
                               "## Decisions\n- decided\n\n"
                               "## Open Questions\n- asked\n\n"
                               "## Artifacts\n- reports/session.md\n\n"
                               "## Resolved History\n\n%1\n")
                               .arg(history.join(QStringLiteral("\n\n")));

    QFile file(store.currentSessionInfo().path + QStringLiteral("/memory.md"));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate));
    file.write(memory.toUtf8());
    file.close();

    constexpr qint64 kMaxMemoryBytes = 256LL * 1024LL;
    const qint64 before = QFileInfo(file.fileName()).size();
    QVERIFY2(before > kMaxMemoryBytes, qPrintable(QString::number(before)));

    QVERIFY(store.appendMemoryEntry(QStringLiteral("Assistant"),
                                    QStringLiteral("Latest"),
                                    QStringLiteral("Latest preserved finding"),
                                    &error));
    QVERIFY2(error.isEmpty(), qPrintable(error));

    // THE POINT: the file is actually under the cap afterwards, measured the same way the cap is.
    const qint64 after = QFileInfo(file.fileName()).size();
    QVERIFY2(after <= kMaxMemoryBytes,
             qPrintable(
                 QStringLiteral("trimmed to %1 bytes, cap is %2").arg(after).arg(kMaxMemoryBytes)));

    QFile trimmed(file.fileName());
    QVERIFY(trimmed.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray raw = trimmed.readAll();
    const QString text = QString::fromUtf8(raw);
    // Structure survives, and the newest entry is kept.
    static_cast<void>(verifyMemorySectionsInOrder(text));
    QVERIFY(text.contains(QStringLiteral("keep this")));
    QVERIFY(text.contains(QStringLiteral("Latest preserved finding")));
    // Truncation landed on a character boundary: a split UTF-8 sequence would decode to U+FFFD.
    QVERIFY2(!text.contains(QChar(0xFFFD)), "trim split a multi-byte character");
    QCOMPARE(text.toUtf8(), raw);
}

void AiConversationStoreTests::searchFallsBackToRawLogsWhenTheIndexIsKnownIncomplete() {
    // Index appends are best-effort BY DESIGN -- losing one must never fail a transcript or
    // command write that already succeeded. But searchIndexFile() returned true for any index it
    // could merely OPEN, and that return is what makes the caller skip the raw-log scan. So a
    // record whose index append had failed became permanently invisible to search, silently,
    // while the transcript on disk held it the whole time.
    //
    // The writer now drops a marker when an append fails. While it exists the index is demoted
    // from authoritative to advisory and the raw logs answer instead.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY(store.startSession(QStringLiteral("Index integrity"), &error));

    QVERIFY(store.appendTranscript(QStringLiteral("assistant"),
                                   QStringLiteral("SUPERAntiSpyware quick scan finished"),
                                   QJsonObject{},
                                   &error));

    const QString session_dir = store.currentSessionInfo().path;
    const QString index_path = session_dir + QStringLiteral("/search_index.jsonl");
    const QString marker_path = session_dir + QStringLiteral("/search_index.incomplete");

    // Baseline: the index answers, and it holds the record.
    QVERIFY(QFileInfo::exists(index_path));
    QVERIFY(!QFileInfo::exists(marker_path));
    QCOMPARE(store.searchSessions(QStringLiteral("SUPERAntiSpyware"), 10, &error).size(), 1);

    // Simulate the state a failed index append leaves behind: the raw transcript still has the
    // record, the INDEX DOES NOT, and the marker says so. Without the marker this search would
    // answer from the truncated index and report zero hits for a record that exists.
    QFile index(index_path);
    QVERIFY(index.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    index.close();
    QCOMPARE(store.searchSessions(QStringLiteral("SUPERAntiSpyware"), 10, &error).size(), 0);

    QFile marker(marker_path);
    QVERIFY(marker.open(QIODevice::WriteOnly | QIODevice::Text));
    marker.write("search index incomplete\n");
    marker.close();

    const auto hits = store.searchSessions(QStringLiteral("SUPERAntiSpyware"), 10, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.first().source, QStringLiteral("transcript"));
    QVERIFY2(hits.first().snippet.contains(QStringLiteral("SUPERAntiSpyware")),
             qPrintable(hits.first().snippet));
}

void AiConversationStoreTests::artifactSubdir_confinesSubdirToTheArtifactRoot() {
    // artifactPath() confines the FILENAME, and anchors that check to the directory
    // artifactSubdir() returns. So an escaping subdir does not merely create a directory in the
    // wrong place -- it silently defeats the filename guard, because a filename inside an already
    // escaped base passes containment. This is the ground that guard stands on.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY(store.startSession(QStringLiteral("Artifacts"), &error));

    const QString escaping_relative = QStringLiteral("../../../../evil");
    error.clear();
    QVERIFY2(store.artifactSubdir(escaping_relative, &error).isEmpty(), qPrintable(error));
    QCOMPARE(error,
             QStringLiteral("Artifact subdir escapes the artifact directory: %1")
                 .arg(escaping_relative));

    // An ABSOLUTE subdir is the other half: QDir::filePath returns it verbatim, so without the
    // guard it names a directory anywhere the process can write.
    const QString absolute = QDir::toNativeSeparators(temp.path()) + QStringLiteral("/outside");
    const QString absolute_forward = QDir::fromNativeSeparators(absolute);
    error.clear();
    QVERIFY2(store.artifactSubdir(absolute_forward, &error).isEmpty(), qPrintable(error));
    QVERIFY2(error.startsWith(QStringLiteral("Artifact subdir escapes")), qPrintable(error));
    QVERIFY2(!QDir(absolute_forward).exists(), "the escaping directory must never be created");

    // And the escape cannot be laundered through artifactPath's filename guard either.
    error.clear();
    QVERIFY(store.artifactPath(escaping_relative, QStringLiteral("ok.txt"), &error).isEmpty());
    QVERIFY2(!error.isEmpty(), "artifactPath must fail closed when its base escapes");

    // The other arm: a legitimate nested subdir still works, so this is not "reject anything
    // with a separator".
    error.clear();
    const QString nested = store.artifactSubdir(QStringLiteral("downloads/batch_01"), &error);
    QVERIFY2(!nested.isEmpty(), qPrintable(error));
    QVERIFY(nested.endsWith(QStringLiteral("/downloads/batch_01")));
}

void AiConversationStoreTests::artifactPath_rejectsAFilenameThatNamesTheDirectory() {
    // An empty or dot-only filename resolves to the artifact directory itself, and the
    // containment check ACCEPTS that (resolved == base) -- so the function returned a DIRECTORY
    // as a successful file path. The caller then opens it and the write fails somewhere that can
    // no longer explain why. This function promises a path to a file.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    sak::ai::ConversationStore store(temp.path());
    QString error;
    QVERIFY(store.startSession(QStringLiteral("Artifacts"), &error));

    for (const QString& filename : {QString(), QStringLiteral("."), QStringLiteral("nested/..")}) {
        error.clear();
        QVERIFY2(store.artifactPath(QStringLiteral("downloads"), filename, &error).isEmpty(),
                 qPrintable(filename));
        QCOMPARE(error,
                 QStringLiteral("Artifact filename does not name a file: '%1'").arg(filename));
    }

    // Control: a real filename still resolves, so the guard is not rejecting everything.
    error.clear();
    QVERIFY(!store.artifactPath(QStringLiteral("downloads"), QStringLiteral("f.bin"), &error)
                 .isEmpty());
    QVERIFY2(error.isEmpty(), qPrintable(error));
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
    // (c) the reader RESULTS must stay CORRECT under contention, not merely not-crash. Every
    // reader result used to be cast to void, and torn_read only fires on a wrong id or an empty
    // path -- it cannot see artifactRootDirectory() or artifactPath() returning an empty string
    // with an error set. Every refusal arm in that nested chain returns exactly that, so a store
    // that refused EVERY concurrent artifact request scored identically to a healthy one. Both
    // expected values (and their directories) are established before any thread starts, so the
    // only thing a reader can observe is a contention-induced refusal.
    std::atomic<bool> bad_artifact_read{false};
    const QString expected_root = store.artifactRootDirectory(&error);
    QVERIFY2(!expected_root.isEmpty(), qPrintable(error));
    const QString expected_artifact =
        store.artifactPath(QStringLiteral("downloads"), QStringLiteral("f.bin"), &error);
    QCOMPARE(expected_artifact, expected_root + QStringLiteral("/downloads/f.bin"));

    const ContentionReaderContext reader_ctx{.store = store,
                                             .session_id = session_id,
                                             .iterations = kIterations,
                                             .torn_read = torn_read,
                                             .bad_artifact_read = bad_artifact_read,
                                             .expected_root = expected_root,
                                             .expected_artifact = expected_artifact};
    auto reader = makeContentionReader(reader_ctx);

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
    QVERIFY2(!bad_artifact_read.load(),
             "a reader saw an empty or mismatched artifact path under contention");

    verifyWriterHalfBuiltARealCorpus(store, session_id, kIterations, error);

    // The store is still consistent and usable after the hammering.
    QCOMPARE(store.currentSessionId(), session_id);
    const QString root = store.artifactRootDirectory(&error);
    QVERIFY2(!root.isEmpty(), qPrintable(error));
}

QTEST_GUILESS_MAIN(AiConversationStoreTests)
#include "test_ai_conversation_store.moc"

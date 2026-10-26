// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_store_file_loader.h"
#include "sak/ai/ai_token_usage_tracker.h"

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QReadWriteLock>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

// Global-namespace unit-test class, befriended below so it can exercise the pure static
// path-safety seams (safeArtifactDirectoryName / commandLogPath) directly.
class AiConversationStoreTests;

namespace sak::ai {

inline constexpr int kDefaultMemoryTextMaxChars = 16'000;

/// The session picker lists at most this many sessions: the most recently active ones (by the
/// session folder's last-write time, which every manifest write moves). More folders than this
/// no longer empties the list.
inline constexpr int kAiSessionListLimit = 512;

/// Retention policy (documented default): when a new chat session is started, sessions beyond
/// the newest kAiSessionRetentionCount (least recently active first) are deleted with their
/// transcripts and artifacts. The session being started and the one open are never deleted,
/// and only folders this store created (named ai_<UTC timestamp>_<8 hex>) are touched.
inline constexpr int kAiSessionRetentionCount = 500;
/// Starting a chat runs on the GUI thread, so one start removes at most this many old folders;
/// a store far over the limit converges over later starts instead of freezing the window once.
inline constexpr int kAiSessionPrunePerStartLimit = 8;
static_assert(kAiSessionRetentionCount <= kAiSessionListLimit,
              "retained sessions must all be listable");

struct AiSessionInfo {
    QString id;
    QString title;
    QString path;
    QDateTime created_at;
    QDateTime updated_at;
    /// Whether the transcript holds a user prompt. Kept in the manifest so the session picker
    /// reads manifests only and never re-parses a transcript.
    bool has_prompt{false};
};

struct AiSessionSearchResult {
    AiSessionInfo session;
    QString source;
    QString snippet;
    int score{0};
    QDateTime timestamp_utc;
};

class ConversationStore {
    friend class ::AiConversationStoreTests;  // unit-test seam for the pure static path guards

public:
    explicit ConversationStore(QString root_dir = {});

    [[nodiscard]] QString rootDirectory() const;
    [[nodiscard]] QString currentSessionId() const;
    [[nodiscard]] AiSessionInfo currentSessionInfo() const;

    [[nodiscard]] static QString defaultRootDirectory();

    [[nodiscard]] bool ensureRoot(QString* error_message = nullptr) const;
    [[nodiscard]] QVector<AiSessionInfo> listSessions(QString* error_message = nullptr) const;
    /// Sessions whose transcript holds a user prompt, from the manifests alone. A manifest
    /// written before has_prompt existed is answered from its transcript once and then has the
    /// field written back, so later listings never parse it again.
    [[nodiscard]] QVector<AiSessionInfo> listPromptedSessions(
        QString* error_message = nullptr) const;
    /// Applies the retention policy (kAiSessionRetentionCount); startSession runs it. Returns
    /// how many session folders were removed; failures are reported, never fatal.
    int pruneSessionsBeyondRetention(int keep = kAiSessionRetentionCount,
                                     QString* error_message = nullptr) const;
    [[nodiscard]] QStringList loadTranscriptLines(const QString& session_id,
                                                  QString* error_message = nullptr) const;
    [[nodiscard]] QString latestAssistantResponseId(const QString& session_id,
                                                    QString* error_message = nullptr) const;
    [[nodiscard]] QVector<AiSessionSearchResult> searchSessions(
        const QString& query, int max_results = 50, QString* error_message = nullptr) const;

    [[nodiscard]] bool startSession(const QString& title, QString* error_message = nullptr);
    [[nodiscard]] bool openSession(const QString& session_id, QString* error_message = nullptr);
    void clearCurrentSession();
    [[nodiscard]] bool renameCurrentSession(const QString& title, QString* error_message = nullptr);

    [[nodiscard]] bool appendTranscript(const QString& role,
                                        const QString& text,
                                        const QJsonObject& metadata = {},
                                        QString* error_message = nullptr);
    [[nodiscard]] bool appendCommand(const QString& command,
                                     const QJsonObject& result,
                                     QString* error_message = nullptr);
    /// @brief Returns an absolute path under
    /// `<session>/artifacts/<chat-name>/logs/` for command output, creating the
    /// directory on demand. Returns an empty QString if no session is active or
    /// the directory cannot be created.
    [[nodiscard]] QString commandLogPath(const QString& command_id,
                                         const QString& suffix,
                                         QString* error_message = nullptr) const;

    /// @brief Returns an absolute path under
    /// `<session>/artifacts/<chat-name>/<subdir>/`, creating the directory on
    /// demand. `subdir` is one of "screenshots", "downloads", "reports",
    /// "logs", "scripts".
    [[nodiscard]] QString artifactSubdir(const QString& subdir,
                                         QString* error_message = nullptr) const;
    [[nodiscard]] QString artifactRootDirectory(QString* error_message = nullptr) const;

    /// @brief Returns an absolute path to a file inside an artifact subdir.
    [[nodiscard]] QString artifactPath(const QString& subdir,
                                       const QString& filename,
                                       QString* error_message = nullptr) const;
    /// @brief Atomically create one new managed artifact without exposing a writable leaf path.
    [[nodiscard]] QString writeArtifactFile(const QString& subdir,
                                            const QString& filename,
                                            const QByteArray& bytes,
                                            QString* error_message = nullptr) const;
    [[nodiscard]] QString copyArtifactFile(const QString& subdir,
                                           const QString& filename,
                                           const QString& source_path,
                                           QString* error_message = nullptr) const;
    [[nodiscard]] QString writeCommandLog(const QString& command_id,
                                          const QString& suffix,
                                          const QByteArray& bytes,
                                          QString* error_message = nullptr) const;
    [[nodiscard]] QString memoryText(int max_chars = kDefaultMemoryTextMaxChars,
                                     QString* error_message = nullptr) const;
    [[nodiscard]] bool appendMemoryEntry(const QString& kind,
                                         const QString& title,
                                         const QString& text,
                                         QString* error_message = nullptr);
    [[nodiscard]] bool appendContext(const QString& kind,
                                     const QString& label,
                                     const QString& path,
                                     qint64 size_bytes,
                                     QString* error_message = nullptr);
    [[nodiscard]] bool writeUsage(const TokenUsage& turn,
                                  const TokenUsage& session_total,
                                  QString* error_message = nullptr);

    static void setBeforeManifestChildOpenHookForTesting(std::function<void(const QString&)> hook);
    static void setBeforeManifestFileOpenHookForTesting(std::function<void(const QString&)> hook);
    static void setBeforeManifestFileReadHookForTesting(std::function<void(const QString&)> hook);
    static void setBeforeManifestWriteHookForTesting(std::function<void(const QString&)> hook);
    static void setBeforeArtifactChildOpenHookForTesting(std::function<void(const QString&)> hook);
    static void setBeforeArtifactLeafTempOpenHookForTesting(
        std::function<void(const QString&)> hook);
    static void setBeforeArtifactLeafPublishHookForTesting(
        std::function<void(const QString&)> hook);

private:
    [[nodiscard]] QString sessionPath(const QString& session_id) const;
    [[nodiscard]] QString currentSessionPath() const;
    [[nodiscard]] bool writeManifest(QString* error_message = nullptr) const;
    /// Retention core; @p current_id is never removed. Takes no lock on m_lock.
    int pruneSessionFolders(int keep,
                            const QString& current_id,
                            int max_removals,
                            QString* error_message) const;
    /// Backfills has_prompt into a legacy manifest that lacks it.
    [[nodiscard]] bool backfillPromptFlag(const AiSessionInfo& session,
                                          QString* error_message) const;
    [[nodiscard]] bool appendJsonLine(const QString& filename,
                                      QJsonObject object,
                                      QString* error_message = nullptr) const;
    [[nodiscard]] QString pinnedArtifactDirectory(const QString& subdir,
                                                  QString* error_message) const;
    [[nodiscard]] detail::StoreDirectoryPin pinnedArtifactDirectoryPin(
        const QString& subdir, QString* error_message) const;
    void clearArtifactDirectoryPins() const;
    [[nodiscard]] static QJsonObject usageToJson(const TokenUsage& usage);
    [[nodiscard]] static QString safeArtifactDirectoryName(const QString& title,
                                                           const QString& fallback_id);
    /// What renameArtifactDirectory actually did, so a caller rolls back only a completed move.
    /// A pre-existing destination is refused; artifacts are never merged or copied as fallback.
    enum class ArtifactRenameOutcome {
        NoChange,
        Renamed,
        Failed
    };

    [[nodiscard]] static ArtifactRenameOutcome renameArtifactDirectory(
        const QString& old_name,
        const QString& new_name,
        const QString& artifacts_root,
        QString* error_message);

    QString m_root_dir;
    AiSessionInfo m_current_session;
    // Guards m_current_session against concurrent access: acting subagents run
    // allowlisted store-backed tools (session_search, artifact/download paths) on
    // a workflow WORKER thread while the GUI thread appends transcripts/commands
    // and mutates the current-session record. Recursive so public methods that
    // call other public methods (artifactPath -> artifactSubdir; writeUsage ->
    // startSession) do not self-deadlock. Readers share; writers are exclusive.
    // m_root_dir is set once in the constructor and never reassigned, so it is read lock-free.
    mutable QReadWriteLock m_lock{QReadWriteLock::Recursive};
    mutable QMutex m_artifact_pin_mutex;
    mutable QHash<QString, detail::StoreDirectoryPin> m_artifact_directory_pins;
};

}  // namespace sak::ai

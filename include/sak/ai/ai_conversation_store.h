// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_token_usage_tracker.h"

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace sak::ai {

inline constexpr int kDefaultMemoryTextMaxChars = 16'000;

struct AiSessionInfo {
    QString id;
    QString title;
    QString path;
    QDateTime created_at;
    QDateTime updated_at;
};

struct AiSessionSearchResult {
    AiSessionInfo session;
    QString source;
    QString snippet;
    int score{0};
    QDateTime timestamp_utc;
};

class ConversationStore {
public:
    explicit ConversationStore(QString root_dir = {});

    [[nodiscard]] QString rootDirectory() const;
    [[nodiscard]] QString currentSessionId() const;
    [[nodiscard]] AiSessionInfo currentSessionInfo() const;

    [[nodiscard]] static QString defaultRootDirectory();

    [[nodiscard]] bool ensureRoot(QString* error_message = nullptr) const;
    [[nodiscard]] QVector<AiSessionInfo> listSessions() const;
    [[nodiscard]] QVector<AiSessionInfo> listPromptedSessions() const;
    [[nodiscard]] QStringList loadTranscriptLines(const QString& session_id,
                                                  QString* error_message = nullptr) const;
    [[nodiscard]] QString latestAssistantResponseId(const QString& session_id,
                                                    QString* error_message = nullptr) const;
    [[nodiscard]] QString latestSessionRole(const QString& session_id,
                                            QString* source = nullptr,
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

private:
    [[nodiscard]] QString sessionPath(const QString& session_id) const;
    [[nodiscard]] QString currentSessionPath() const;
    [[nodiscard]] bool writeManifest(QString* error_message = nullptr) const;
    [[nodiscard]] bool appendJsonLine(const QString& filename,
                                      QJsonObject object,
                                      QString* error_message = nullptr) const;
    [[nodiscard]] bool appendSearchIndexRecord(QJsonObject object,
                                               QString* error_message = nullptr) const;
    [[nodiscard]] static AiSessionInfo readManifest(const QString& session_path);
    [[nodiscard]] static QJsonObject usageToJson(const TokenUsage& usage);
    [[nodiscard]] static QString safeArtifactDirectoryName(const QString& title,
                                                           const QString& fallback_id);

    QString m_root_dir;
    AiSessionInfo m_current_session;
};

}  // namespace sak::ai

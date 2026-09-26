// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <functional>

namespace sak::ai {

inline constexpr qsizetype kMaxAgentFrameBytes = 16 * 1024 * 1024;
inline constexpr qsizetype kAgentStderrTailBytes = 8192;

/// @brief Child process speaking newline-delimited JSON on stdin/stdout.
///
/// Used for every bundled agent runtime (Codex app-server, Claude Code
/// stream-json, Gemini CLI ACP, Muse Code MSP). Non-JSON stdout lines are
/// ignored; stderr is kept as a short tail for error messages only.
class AiStdioJsonProcess : public QObject {
    Q_OBJECT

public:
    struct LaunchSpec {
        QString program;
        QStringList arguments;
        QProcessEnvironment environment;
        QString working_directory;
    };

    explicit AiStdioJsonProcess(QObject* parent = nullptr);
    ~AiStdioJsonProcess() override;

    void start(const LaunchSpec& spec);
    void stop();
    [[nodiscard]] bool isRunning() const;
    bool send(const QJsonObject& message);
    [[nodiscard]] QString stderrTail() const;

    /// @brief Split a stdout buffer into complete JSON object lines.
    [[nodiscard]] static QList<QJsonObject> takeJsonLines(QByteArray* buffer);

Q_SIGNALS:
    void started();
    void jsonReceived(const QJsonObject& message);
    void finished(int exit_code, const QString& stderr_tail);
    void startFailed(const QString& error_message);

private:
    void handleStdout();
    void handleStderr();

    QProcess* m_process;
    QByteArray m_stdout_buffer;
    QByteArray m_stderr_tail;
};

/// @brief Minimal JSON-RPC peer over an AiStdioJsonProcess.
///
/// Codex app-server omits the `"jsonrpc":"2.0"` member; ACP and MSP require
/// it, so it is configurable.
class AiJsonRpcPeer : public QObject {
    Q_OBJECT

public:
    using ResultHandler = std::function<void(const QJsonObject& result, const QString& error)>;

    explicit AiJsonRpcPeer(AiStdioJsonProcess* process, bool include_jsonrpc_field);

    void request(const QString& method, const QJsonObject& params, ResultHandler handler);
    void notify(const QString& method, const QJsonObject& params);
    void respond(const QJsonValue& id, const QJsonObject& result);
    void respondError(const QJsonValue& id, int code, const QString& message);
    /// @brief Fail every outstanding request (process exit / shutdown).
    void failAll(const QString& error_message);
    [[nodiscard]] int pendingCount() const { return static_cast<int>(m_pending.size()); }

    void handleMessage(const QJsonObject& message);

Q_SIGNALS:
    void notificationReceived(const QString& method, const QJsonObject& params);
    void requestReceived(const QJsonValue& id, const QString& method, const QJsonObject& params);

private:
    [[nodiscard]] QJsonObject envelope() const;

    AiStdioJsonProcess* m_process;
    bool m_include_jsonrpc_field;
    qint64 m_next_id{1};
    QHash<qint64, ResultHandler> m_pending;
};

}  // namespace sak::ai

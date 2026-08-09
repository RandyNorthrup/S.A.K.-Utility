// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/layout_constants.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#endif

class QProcess;

namespace sak::ai {

inline constexpr int kAiCommandDefaultTimeoutSeconds = 120;
inline constexpr int kAiCommandDefaultMaxOutputKilobytes = 256;
inline constexpr int kAiCommandDefaultMaxOutputBytes = kAiCommandDefaultMaxOutputKilobytes *
                                                       static_cast<int>(sak::kBytesPerKB);

struct AiCommandRequest {
    QString command;
    QString program;
    QStringList arguments;
    int timeout_seconds{kAiCommandDefaultTimeoutSeconds};
    bool requires_admin{false};
    int max_output_bytes{kAiCommandDefaultMaxOutputBytes};
    /// Typed rejection channel for the JSON parsers. Non-empty means the request was
    /// malformed or out of domain; every entry point refuses it with this exact message
    /// instead of executing a request that was repaired with defaults.
    QString validation_error;
};

struct AiCommandResult {
    bool started{false};
    bool cancelled{false};
    bool timed_out{false};
    bool elevated{false};
    int exit_code{-1};
    int exit_status{0};
    qint64 duration_ms{0};
    QString stdout_text;
    QString stderr_text;
    QString error_message;
    /// True when stdout/stderr were capped: the text above is NOT the complete output, so
    /// a consumer must not read it as the whole run.
    bool output_truncated{false};

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] QString toJsonString() const;
};

struct ProcessLaunchRequest {
    QString program;
    QStringList arguments;
    int timeout_seconds{kAiCommandDefaultTimeoutSeconds};
    int max_output_bytes{kAiCommandDefaultMaxOutputBytes};
    QString command_id;
    bool already_running_check{true};
};

/// @brief Async executor for AI-requested commands.
///
/// `startPowerShell` returns immediately; results are delivered via
/// `finished`. Streamed stdout/stderr are emitted as `stdoutChunk` /
/// `stderrChunk` for the command timeline. `cancel` kills the running
/// process. Elevated commands still route synchronously through the
/// injected runner (Milestone 6 will replace that with the elevated worker).
class ExecutionBroker : public QObject {
    Q_OBJECT

public:
    using ElevatedRunner = std::function<AiCommandResult(const AiCommandRequest&)>;
    using ElevatedCancel = std::function<void()>;

    explicit ExecutionBroker(QObject* parent = nullptr);
    ~ExecutionBroker() override;

    void setElevatedRunner(ElevatedRunner runner);
    void setElevatedCancel(ElevatedCancel cancel);

    /// @brief Begin executing a PowerShell command. Returns false if a
    /// command is already running or the request is rejected before
    /// process spawn; in that case `finished` is still emitted with the
    /// failure result on the next event-loop tick so callers can use a
    /// single completion path.
    bool startPowerShell(const AiCommandRequest& request, const QString& command_id = {});

    /// @brief Begin executing a `cmd.exe /c <command>` invocation.
    /// Elevation is not yet wired through the helper for cmd; if
    /// `requires_admin` is true the call fails fast.
    bool startCmd(const AiCommandRequest& request, const QString& command_id = {});

    /// @brief Launch a program directly with the given arguments.
    /// `request.program` is required. Elevation follows the same rule
    /// as `startCmd` for now.
    bool startProcess(const AiCommandRequest& request, const QString& command_id = {});

    /// @brief Request cancellation of the running command. Safe to call
    /// when idle. The running process is killed and `finished` is emitted
    /// with `cancelled=true`.
    void cancel();

    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] QString runningCommandId() const;

    [[nodiscard]] static AiCommandRequest requestFromJson(const QJsonObject& args);
    [[nodiscard]] static AiCommandRequest processRequestFromJson(const QJsonObject& args);

Q_SIGNALS:
    void started(const QString& command_id);
    void stdoutChunk(const QString& command_id, const QString& chunk);
    void stderrChunk(const QString& command_id, const QString& chunk);
    void finished(const QString& command_id, const sak::ai::AiCommandResult& result);

private:
    bool runElevatedRequest(const AiCommandRequest& request);
    void connectProcessSignals();
    bool launchProcess(const ProcessLaunchRequest& request);
#ifdef _WIN32
    void assignProcessToJob();
    void terminateProcessTree();
    void closeJobHandle();
#endif
    void completeWith(AiCommandResult result);
    void emitDeferredFinish(AiCommandResult result);
    void emitDeferredStandaloneFinish(const QString& command_id, AiCommandResult result);
    void onReadyReadStdout();
    void onReadyReadStderr();
    void fillCappedOutput(AiCommandResult& result);
    void onProcessFinished(int exit_code, int exit_status);
    void onProcessError(int error);
    void completeAsTimedOut();
    void onTimeoutTick();
    void appendCapped(QString& target, const QString& chunk);

    ElevatedRunner m_elevated_runner;
    ElevatedCancel m_elevated_cancel;
    bool m_elevated_in_flight{false};
    std::unique_ptr<QProcess> m_process;
#ifdef _WIN32
    HANDLE m_job_handle{nullptr};
#endif
    QString m_command_id;
    int m_max_output_bytes{kAiCommandDefaultMaxOutputBytes};
    int m_timeout_ms{0};
    QElapsedTimer m_timer;
    QString m_stdout_buffer;
    QString m_stderr_buffer;
    /// Bytes dropped from the head of the rolling buffers, so a capped run is never
    /// reported as complete output.
    qint64 m_output_dropped_bytes{0};
    /// Bumped once per start* call. A deferred pre-launch failure carries the generation it
    /// belongs to and refuses to touch broker state once a newer request owns it.
    quint64 m_request_generation{0};
    bool m_running{false};
    bool m_cancel_requested{false};
    bool m_finished_emitted{false};
};

}  // namespace sak::ai

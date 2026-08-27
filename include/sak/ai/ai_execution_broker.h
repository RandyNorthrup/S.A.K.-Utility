// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/layout_constants.h"
#include "sak/process_runner.h"

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

/// The command-timeout domain, declared HERE because the broker is what enforces it: launchProcess
/// clamps every request to this range, silently.
///
/// It lives in the header because a second copy drifted. The app-action planner had its own
/// ceiling of 14400 (four hours), so it produced -- and a user APPROVED -- plans promising a
/// four-hour action that the broker then cut to one hour and reported as a timeout. The plan said
/// one thing and execution did another. Anything that needs to bound a command's runtime must use
/// these, not a private constant.
inline constexpr int kAiCommandMinTimeoutSeconds = 5;
inline constexpr int kAiCommandMaxTimeoutSeconds = 3600;
inline constexpr int kAiCommandDefaultMaxOutputKilobytes = 256;
inline constexpr int kAiCommandDefaultMaxOutputBytes = kAiCommandDefaultMaxOutputKilobytes *
                                                       static_cast<int>(sak::kBytesPerKB);

/// The hard ceiling on max_output_bytes, declared HERE for exactly the reason the timeout
/// domain above is: the broker is what ENFORCES it, refusing outright any request that exceeds
/// it, so anything that budgets a command's output must be bounded by this rather than by a
/// private ceiling of its own.
///
/// A private copy had already drifted PAST the enforcer, and the failure mode is the same one
/// the timeout comment describes: AiWorkflowPowerShellToolOptions carried its own 64 MiB "hard
/// ceiling", four times what the broker accepts, so a workflow phase asking for more than
/// 16 MiB was clamped DOWN to a value the broker then REFUSED ("max_output_bytes 67108864 is
/// outside the range 1-16777216") and the command never ran at all. A ceiling that is not the
/// enforcer's ceiling bounds nothing; it only decides which requests die at the door.
///
/// A local budget may be STRICTER than this (several deliberately are); it may never be wider.
inline constexpr int kAiCommandOutputBytesCeiling = 16 * static_cast<int>(sak::kBytesPerMB);

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
    /// WHICH FILE `program` named when the plan was built, for the direct-process path.
    ///
    /// The planner resolves `program` to an absolute path so the approval preview can name the
    /// binary that will run -- but a path is a NAME, and the gap between naming it and running
    /// it is human-scale: it spans an approval dialog, a UAC prompt and restore-point creation,
    /// i.e. minutes, not microseconds. Across that gap the same string can come to mean a
    /// different file. Recording the identity here lets startProcess prove, immediately before
    /// launch, that it is about to run the file that was approved and not merely something with
    /// the approved name.
    ///
    /// Left invalid for the shell targets, which launch a System32 interpreter resolved at
    /// launch time rather than an AI-chosen path, and so have no plan-time decision to bind to.
    sak::ExecutableIdentity program_identity;
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

/// Which ExecutionBroker entry point a request is bound for. The pre-launch rules are NOT
/// uniform across the three -- only the PowerShell entry point can elevate -- so a shared
/// check has to be told which one it is deciding for.
enum class AiCommandTarget {
    PowerShell,
    Cmd,
    Process
};

/// @brief The broker's pre-launch preconditions, decided from the request ALONE.
///
/// Returns the exact refusal message ExecutionBroker would produce, or an empty string when
/// the request would be accepted. Split out of the start* entry points so a PLANNER can apply
/// the identical rule BEFORE a human is asked to approve: an empty shell command, an elevated
/// cmd.exe or direct-process launch, and an out-of-domain output budget were each previewed,
/// risk-classified, confirmed and leased, and only then refused at the broker's door.
/// Authorising a command that cannot run is worse than refusing it early -- it spends the
/// approver's attention on a decision that had no effect, and teaches them that the
/// confirmation dialog does not mean what it says.
///
/// Deliberately PURE and request-only. The broker's other pre-launch failures -- another
/// command already running, no elevated runner wired, an unresolvable System32 interpreter --
/// turn on broker or machine state that a plan cannot honestly predict, so they stay at the
/// launch point instead of being guessed at plan time.
[[nodiscard]] QString aiCommandPreconditionError(AiCommandTarget target,
                                                 const AiCommandRequest& request);

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

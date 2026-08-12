// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file elevated_helper_main.cpp
/// @brief Entry point for the elevated helper process (sak_elevated_helper.exe)
///
/// This is a headless process launched via UAC by the main S.A.K. Utility.
/// It creates a named pipe, waits for IPC commands, and executes elevated
/// tasks on behalf of the parent process. Auto-exits after inactivity timeout.

#include "sak/actions/backup_bitlocker_keys_action.h"
#include "sak/actions/check_disk_errors_action.h"
#include "sak/actions/reset_network_action.h"
#include "sak/actions/verify_system_files_action.h"
#include "sak/app_paths.h"
#include "sak/elevated_pipe_protocol.h"
#include "sak/elevated_pipe_server.h"
#include "sak/elevated_task_dispatcher.h"
#include "sak/error_codes.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/permission_manager.h"
#include "sak/process_runner.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <expected>
#include <filesystem>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
constexpr int kQuickActionCancelPollMs = sak::kTimerRetryBaseMs;
constexpr int kThermalQueryTimeoutMs = sak::kTimeoutThermalQueryMs;
constexpr int kHelperStartupFailureExitCode = 2;
// The client vanished while a privileged task was running, so its result could never be
// delivered. That is not a clean exit and must not be reported as one.
constexpr int kHelperClientLostExitCode = 3;

// A UTF-8 continuation byte has its top two bits set to 10. Mask the top two bits and compare, so
// truncation can back up out of a multi-byte sequence that straddles the byte cap.
constexpr unsigned int kUtf8ContinuationMask = 0xC0U;
constexpr unsigned int kUtf8ContinuationByte = 0x80U;

// ======================================================================
// Command-Line Parsing
// ======================================================================

struct HelperArgs {
    QString m_pipe_name;
    qint64 m_parent_pid{0};
};

bool applyPipeArg(const QString& value, HelperArgs* args) {
    if (value.isEmpty()) {
        sak::logError("ElevatedHelper: --pipe value is empty");
        return false;
    }
    if (!args->m_pipe_name.isEmpty()) {
        sak::logError("ElevatedHelper: --pipe supplied more than once");
        return false;
    }
    args->m_pipe_name = value;
    return true;
}

// Fail closed on a malformed parent PID: an unchecked toLongLong() would silently yield 0,
// which authorizes nobody but hides the real cause behind a generic pipe-start failure.
bool applyParentPidArg(const QString& value, HelperArgs* args) {
    bool ok = false;
    const qint64 pid = value.toLongLong(&ok);
    if (!ok || pid <= 0) {
        sak::logError("ElevatedHelper: invalid --parent-pid value '{}'", value.toStdString());
        return false;
    }
    if (args->m_parent_pid != 0) {
        sak::logError("ElevatedHelper: --parent-pid supplied more than once");
        return false;
    }
    args->m_parent_pid = pid;
    return true;
}

// Refuse an unrecognized option, a duplicated option and an option with no value instead of
// ignoring them: the only launcher (ElevationBroker) passes exactly --pipe and --parent-pid, so
// anything else means the command line is not the one this process was meant to run under.
std::optional<HelperArgs> parseArgs(int argc, char* argv[]) {
    HelperArgs args;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        const bool is_pipe = (arg == "--pipe");
        const bool is_parent_pid = (arg == "--parent-pid");
        if (!is_pipe && !is_parent_pid) {
            sak::logError("ElevatedHelper: unrecognized argument '{}'", arg.toStdString());
            return std::nullopt;
        }
        if (i + 1 >= argc) {
            sak::logError("ElevatedHelper: argument '{}' requires a value", arg.toStdString());
            return std::nullopt;
        }
        const QString value = QString::fromLocal8Bit(argv[++i]);
        const bool applied = is_pipe ? applyPipeArg(value, &args) : applyParentPidArg(value, &args);
        if (!applied) {
            return std::nullopt;
        }
    }
    return args;
}

// Redirect TMP/TEMP into a controlled runtime directory. Returns false if that directory
// cannot be created so the caller can fail closed: an elevated helper must not fall back to
// the inherited (potentially attacker-influenced) TMP/TEMP for its child processes.
bool configurePortableRuntimeDirs() {
    const QString temp_dir = sak::app_paths::tempDirectory();
    if (!sak::app_paths::ensureDirectory(temp_dir)) {
        sak::logError("ElevatedHelper: failed to create controlled temp dir '{}'",
                      temp_dir.toStdString());
        return false;
    }
    const QByteArray native_temp = QDir::toNativeSeparators(temp_dir).toLocal8Bit();
    // Both writes must land: a half-applied redirect leaves elevated children resolving their
    // temp directory from the inherited (potentially attacker-influenced) variable.
    const bool tmp_set = qputenv("TMP", native_temp);
    const bool temp_set = qputenv("TEMP", native_temp);
    if (!tmp_set || !temp_set) {
        sak::logError("ElevatedHelper: failed to redirect TMP/TEMP to '{}' (TMP={}, TEMP={})",
                      temp_dir.toStdString(),
                      tmp_set ? 1 : 0,
                      temp_set ? 1 : 0);
        return false;
    }
    return true;
}

// ======================================================================
// Quick Action Task Handler
// ======================================================================

/// @brief Generic handler that instantiates a QuickAction, runs it, and
///        relays progress back through the pipe.
template <typename ActionT, typename... CtorArgs>
sak::TaskHandlerResult runQuickAction(const QJsonObject& payload,
                                      sak::ProgressCallback progress,
                                      sak::CancelCheck is_cancelled,
                                      CtorArgs&&... ctor_args) {
    (void)payload;

    // Synchronous pre-cancel check: the poller below can only relay a cancel that arrives after
    // execute() has begun, so a cancel that was already requested must stop the privileged work
    // from starting at all.
    if (is_cancelled()) {
        return {.success = false,
                .data = {},
                .error_message = QStringLiteral("Task cancelled before it started")};
    }

    auto action = std::make_unique<ActionT>(std::forward<CtorArgs>(ctor_args)...);

    // Connect action progress to the progress callback
    QObject::connect(action.get(),
                     &sak::QuickAction::executionProgress,
                     [&progress](const QString& msg, int pct) { progress(pct, msg); });

    // The action runs synchronously on this worker thread, which has no Qt event loop, so a
    // QTimer here would never fire. Poll cancellation on a helper thread (jthread auto-requests
    // stop and joins on scope exit, even if execute throws) and relay it into the action so an
    // in-flight cancel is honored.
    std::jthread cancel_poller([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            if (is_cancelled()) {
                action->cancel();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kQuickActionCancelPollMs));
        }
    });

    // Execute the action synchronously
    action->execute();
    cancel_poller.request_stop();

    sak::TaskHandlerResult result;
    auto exec_result = action->lastExecutionResult();
    // A cancellation that raced the action's completion must not be reported as a success: the
    // requester asked for the work to stop, so the outcome is "cancelled", not "done".
    const bool cancelled = is_cancelled();
    result.success = exec_result.success && !cancelled;

    QJsonObject data;
    data["message"] = exec_result.message;
    data["log"] = exec_result.log;
    data["status"] = static_cast<int>(action->status());
    data["cancelled"] = cancelled;
    result.data = data;

    // Never return a failure with no error at all: the client would see "failed" and have
    // nothing to report or log.
    if (!result.success) {
        if (cancelled) {
            result.error_message = QStringLiteral("Task cancelled");
        } else if (exec_result.message.isEmpty()) {
            result.error_message = QStringLiteral("Quick action failed with status %1")
                                       .arg(static_cast<int>(action->status()));
        } else {
            result.error_message = exec_result.message;
        }
    }

    return result;
}

// ======================================================================
// Task Registration
// ======================================================================

// Paths (path/source/destination) are chosen by the main app and reach this process over the
// authenticated (parent-pid + image bound) named-pipe IPC boundary. The payload FIELDS are still
// validated here rather than coerced: an absent or wrong-typed value is refused, never turned
// into an empty string that some layer downstream might reinterpret. No reparse/confinement
// guard is added: these ops legitimately target arbitrary paths (including symlinked ones), so
// such a guard would break valid operations without fully closing the junction-TOCTOU window --
// that remains an owner decision, recorded in the review docs.
// The elevated permission ops are single synchronous syscalls with no interruptible loop, so
// is_cancelled can only be honored BEFORE the syscall starts; that check is made below.
struct PermissionTaskSpec {
    std::function<std::expected<void, sak::error_code>(
        sak::PermissionManager&, const QString&, const QString&)>
        m_operation;
    QString m_action_label;
    bool m_needs_user_sid{false};
};

sak::TaskHandlerResult runPermissionTask(const QJsonObject& payload,
                                         sak::ProgressCallback progress,
                                         const sak::CancelCheck& is_cancelled,
                                         const PermissionTaskSpec& spec) {
    const QJsonValue path_value = payload.value(QStringLiteral("path"));
    if (!path_value.isString() || path_value.toString().trimmed().isEmpty()) {
        return {.success = false,
                .data = {},
                .error_message = "Missing or invalid path in payload"};
    }
    const QString path = path_value.toString();
    QString user_sid;
    if (spec.m_needs_user_sid) {
        const QJsonValue sid_value = payload.value(QStringLiteral("user_sid"));
        if (!sid_value.isString() || sid_value.toString().trimmed().isEmpty()) {
            return {.success = false,
                    .data = {},
                    .error_message = "Missing or invalid user_sid in payload"};
        }
        user_sid = sid_value.toString();
    }
    if (is_cancelled()) {
        return {.success = false, .data = {}, .error_message = "Task cancelled before it started"};
    }
    progress(0, spec.m_action_label + path);
    sak::PermissionManager pm;
    auto result = spec.m_operation(pm, path, user_sid);
    if (!result) {
        // Do not discard the structured error: a PermissionManager that failed without setting a
        // message would otherwise produce a failure carrying no error at all.
        QString message = pm.getLastError();
        if (message.isEmpty()) {
            message = QString::fromStdString(std::string(sak::to_string(result.error())));
        }
        return {.success = false, .data = {}, .error_message = message};
    }
    progress(sak::kPercentMax, spec.m_action_label + QStringLiteral("done"));
    return {.success = true, .data = {}, .error_message = {}};
}

QString cappedTaskOutput(const QString& value, int max_bytes, bool* truncated = nullptr) {
    if (truncated != nullptr) {
        *truncated = false;
    }
    if (max_bytes <= 0) {
        return {};
    }

    const QByteArray bytes = value.toUtf8();
    if (bytes.size() <= max_bytes) {
        return value;
    }

    // Never cut inside a multi-byte UTF-8 sequence: fromUtf8 would replace the orphaned bytes
    // with U+FFFD, silently corrupting output a machine reader may be parsing. Drop back to the
    // start of the sequence that straddles the cap.
    qsizetype cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(bytes.at(cut)) & kUtf8ContinuationMask) ==
                          kUtf8ContinuationByte) {
        --cut;
    }
    if (truncated != nullptr) {
        *truncated = true;
    }

    return QString::fromUtf8(bytes.left(cut)) +
           QStringLiteral("\n...[output truncated to %1 bytes]").arg(max_bytes);
}

#ifdef _WIN32
struct ElevatedJobGuard {
    HANDLE m_job{nullptr};
    ~ElevatedJobGuard() {
        if (m_job != nullptr) {
            ::CloseHandle(m_job);  // KILL_ON_JOB_CLOSE reaps survivors
        }
    }
    // Create the KILL_ON_JOB_CLOSE job on first use. Returns false (job left null) if
    // creation or configuration fails so the caller does not rely on an unconfigured job.
    bool ensureJob() {
        if (m_job != nullptr) {
            return true;
        }
        m_job = ::CreateJobObjectW(nullptr, nullptr);
        if (m_job == nullptr) {
            sak::logError("ElevatedHelper: CreateJobObject failed: {}", ::GetLastError());
            return false;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (::SetInformationJobObject(
                m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) == 0) {
            sak::logError("ElevatedHelper: SetInformationJobObject failed: {}", ::GetLastError());
            ::CloseHandle(m_job);
            m_job = nullptr;
            return false;
        }
        return true;
    }
    // Last resort for a started child that could not be placed in the job: without the job it
    // escapes KILL_ON_JOB_CLOSE and can outlive the helper, so kill it now.
    static void terminateUncontained(qint64 pid) {
        const HANDLE handle = ::OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
        if (handle == nullptr) {
            sak::logError(
                "ElevatedHelper: OpenProcess to terminate uncontained child {} "
                "failed: {}",
                pid,
                ::GetLastError());
            return;
        }
        if (::TerminateProcess(handle, 1) == 0) {
            sak::logError("ElevatedHelper: TerminateProcess for uncontained child {} failed: {}",
                          pid,
                          ::GetLastError());
        }
        ::CloseHandle(handle);
    }
    void assign(qint64 pid) {
        if (pid <= 0) {
            return;
        }
        // Fail closed: an unassigned elevated child escapes KILL_ON_JOB_CLOSE and could
        // survive helper exit, so terminate it now rather than leak an orphaned descendant.
        // That applies just as much when the job itself could not be created as when the
        // assignment failed.
        if (!ensureJob()) {
            sak::logError("ElevatedHelper: no job object; terminating uncontained child {}", pid);
            terminateUncontained(pid);
            return;
        }
        const HANDLE handle =
            ::OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
        if (handle == nullptr) {
            sak::logError("ElevatedHelper: OpenProcess for job assign failed: {}",
                          ::GetLastError());
            terminateUncontained(pid);
            return;
        }
        if (::AssignProcessToJobObject(m_job, handle) == 0) {
            sak::logError("ElevatedHelper: AssignProcessToJobObject failed: {}; terminating child",
                          ::GetLastError());
            if (::TerminateProcess(handle, 1) == 0) {
                sak::logError(
                    "ElevatedHelper: TerminateProcess after failed job assign "
                    "failed: {}",
                    ::GetLastError());
            }
        }
        ::CloseHandle(handle);
    }
    void terminate() const {
        if ((m_job != nullptr) && (::TerminateJobObject(m_job, 1) == 0)) {
            sak::logError("ElevatedHelper: TerminateJobObject failed: {}", ::GetLastError());
        }
    }
};
#endif

constexpr int kDefaultPowerShellTimeoutSeconds = 120;
constexpr int kMinPowerShellTimeoutSeconds = 5;
constexpr int kMaxPowerShellTimeoutSeconds = 3600;
constexpr int kPowerShellMillisecondsPerSecond = 1000;
constexpr int kDefaultPowerShellOutputBytes = 262'144;
constexpr int kMinPowerShellOutputBytes = 4096;
constexpr int kMaxPowerShellOutputBytes = 4 * 1024 * 1024;
constexpr int kPowerShellOutputHalfDivisor = 2;
constexpr int kMinPowerShellHalfOutputBytes = 1024;
constexpr int kPowerShellStartProgress = 0;
constexpr int kPowerShellRunningProgress = 5;
constexpr int kPowerShellOutputProgress = 50;
constexpr int kPowerShellFinishedProgress = 100;
constexpr int kActiveTaskCancelPollMs = 50;
// The live output stream is not covered by the final output cap, so each progress frame is
// bounded on its own: a single frame must stay well under the pipe's 4 MiB payload ceiling even
// after JSON escaping.
constexpr int kMaxProgressChunkBytes = 64 * 1024;
constexpr uint64_t kMaxPartitionProbeBytes = 2ULL * 1024ULL * 1024ULL;

struct ElevatedPowerShellConfig {
    QString m_command;
    int m_timeout_ms{0};
    int m_output_cap{0};
    int m_half_cap{0};
};

QJsonObject baseElevatedPowerShellData() {
    QJsonObject data;
    data[QStringLiteral("started")] = false;
    data[QStringLiteral("cancelled")] = false;
    data[QStringLiteral("timed_out")] = false;
    data[QStringLiteral("elevated")] = true;
    data[QStringLiteral("exit_code")] = -1;
    data[QStringLiteral("exit_status")] = 0;
    data[QStringLiteral("duration_ms")] = 0.0;
    return data;
}

sak::TaskHandlerResult elevatedPowerShellFailure(QJsonObject data,
                                                 const QString& message,
                                                 qint64 duration_ms = 0) {
    sak::TaskHandlerResult task;
    task.success = false;
    task.error_message = message;
    data[QStringLiteral("duration_ms")] = static_cast<double>(duration_ms);
    data[QStringLiteral("error_message")] = message;
    task.data = data;
    return task;
}

QString elevatedPowerShellStartError(const sak::ProcessResult& result) {
    QString message = QStringLiteral("Failed to start elevated PowerShell");
    if (!result.std_err.isEmpty()) {
        message += QStringLiteral(": ") + result.std_err.trimmed();
    }
    return message;
}

void fillElevatedPowerShellResult(QJsonObject* data,
                                  const sak::ProcessResult& result,
                                  qint64 duration_ms,
                                  int half_cap) {
    (*data)[QStringLiteral("started")] = true;
    (*data)[QStringLiteral("cancelled")] = result.cancelled;
    (*data)[QStringLiteral("timed_out")] = result.timed_out;
    (*data)[QStringLiteral("exit_code")] = result.exit_code;
    (*data)[QStringLiteral("exit_status")] = result.exit_status;
    (*data)[QStringLiteral("duration_ms")] = static_cast<double>(duration_ms);
    bool stdout_truncated = false;
    bool stderr_truncated = false;
    (*data)[QStringLiteral("stdout")] =
        cappedTaskOutput(result.std_out, half_cap, &stdout_truncated);
    (*data)[QStringLiteral("stderr")] =
        cappedTaskOutput(result.std_err, half_cap, &stderr_truncated);
    // These count the RETAINED buffers, which the process runner may already have capped
    // (result.output_truncated). Report the truncation explicitly so a machine reader is never
    // left to assume the text it received is the whole output.
    (*data)[QStringLiteral("stdout_full_bytes")] =
        static_cast<double>(result.std_out.toUtf8().size());
    (*data)[QStringLiteral("stderr_full_bytes")] =
        static_cast<double>(result.std_err.toUtf8().size());
    (*data)[QStringLiteral("stdout_truncated")] = stdout_truncated;
    (*data)[QStringLiteral("stderr_truncated")] = stderr_truncated;
    (*data)[QStringLiteral("runner_output_dropped")] = result.output_truncated;
    (*data)[QStringLiteral("output_truncated")] = stdout_truncated || stderr_truncated ||
                                                  result.output_truncated;
}

QString elevatedPowerShellResultError(const sak::ProcessResult& result) {
    if (result.timed_out) {
        return QStringLiteral("Command timed out");
    }
    if (result.cancelled) {
        return QStringLiteral("Command cancelled");
    }
    // A crash-exit paired with exit code 0 is not a clean run: report the abnormal termination
    // rather than let the zero exit code speak for it.
    if (result.exit_status != 0) {
        return QStringLiteral("Command terminated abnormally (exit status %1)")
            .arg(result.exit_status);
    }
    if (result.exit_code != 0) {
        return QStringLiteral("Command exited with code %1").arg(result.exit_code);
    }
    return {};
}

sak::TaskHandlerResult elevatedPowerShellOutcome(QJsonObject data,
                                                 const sak::ProcessResult& result,
                                                 qint64 duration_ms,
                                                 int half_cap,
                                                 const sak::ProgressCallback& progress) {
    fillElevatedPowerShellResult(&data, result, duration_ms, half_cap);
    const QString result_error = elevatedPowerShellResultError(result);
    data[QStringLiteral("error_message")] = result_error;

    progress(kPowerShellFinishedProgress, QStringLiteral("Elevated PowerShell finished"));
    sak::TaskHandlerResult task;
    // B5-05: gate success on the actual outcome. The process starting is not
    // success -- a timeout, a cancellation, an abnormal (crash) termination, or a
    // non-zero exit is a failure and must be reported as one (previously success
    // was hard-coded true once the process started).
    task.success = result.completedSuccessfully() && result.exit_status == 0;
    task.error_message = result_error;
    task.data = data;
    return task;
}

// timeout_seconds/max_output_bytes are optional operational parameters. An absent key uses the
// documented default, but a PRESENT key that is not a whole number is REFUSED rather than
// silently coerced to that default: a malformed request must never run under limits the caller
// never asked for. QJsonValue::toInt(default) cannot make that distinction, hence this helper.
std::optional<int> optionalPayloadInt(const QJsonObject& payload,
                                      const QString& key,
                                      int default_value,
                                      QString* error_message) {
    const QJsonValue value = payload.value(key);
    if (value.isUndefined()) {
        return default_value;
    }
    if (!value.isDouble()) {
        *error_message = QStringLiteral("Payload field '%1' must be a number").arg(key);
        return std::nullopt;
    }
    const double raw = value.toDouble();
    if (!std::isfinite(raw) || std::floor(raw) != raw ||
        raw < static_cast<double>(std::numeric_limits<int>::min()) ||
        raw > static_cast<double>(std::numeric_limits<int>::max())) {
        *error_message =
            QStringLiteral("Payload field '%1' must be a whole 32-bit number").arg(key);
        return std::nullopt;
    }
    return static_cast<int>(raw);
}

// The std::clamp calls below enforce the operational ceiling (a security bound applied at every
// layer, and the effective values are reported back in the result data so a clamp is visible to
// the caller). payloadUInt64 (used for the raw-probe numeric fields) fails closed on wrong-typed
// input by returning std::nullopt.
std::optional<ElevatedPowerShellConfig> elevatedPowerShellConfig(const QJsonObject& payload,
                                                                 QString* error_message) {
    const QJsonValue command_value = payload.value(QStringLiteral("command"));
    if (!command_value.isString()) {
        *error_message = QStringLiteral("PowerShell command must be a string");
        return std::nullopt;
    }
    const auto timeout_seconds = optionalPayloadInt(payload,
                                                    QStringLiteral("timeout_seconds"),
                                                    kDefaultPowerShellTimeoutSeconds,
                                                    error_message);
    if (!timeout_seconds.has_value()) {
        return std::nullopt;
    }
    const auto max_output_bytes = optionalPayloadInt(
        payload, QStringLiteral("max_output_bytes"), kDefaultPowerShellOutputBytes, error_message);
    if (!max_output_bytes.has_value()) {
        return std::nullopt;
    }

    ElevatedPowerShellConfig config;
    config.m_command = command_value.toString();
    config.m_timeout_ms =
        std::clamp(*timeout_seconds, kMinPowerShellTimeoutSeconds, kMaxPowerShellTimeoutSeconds) *
        kPowerShellMillisecondsPerSecond;
    config.m_output_cap =
        std::clamp(*max_output_bytes, kMinPowerShellOutputBytes, kMaxPowerShellOutputBytes);
    config.m_half_cap = std::max(config.m_output_cap / kPowerShellOutputHalfDivisor,
                                 kMinPowerShellHalfOutputBytes);
    return config;
}

// Resolve powershell.exe to its absolute path under the system directory. Launching an
// unqualified "powershell.exe" resolves via PATH, which an attacker who controls a PATH
// entry (or the working directory) could hijack -- especially dangerous in an elevated
// process. Delegates to the shared sak::systemPowerShellPath() so this process cannot drift
// from the resolution rule the rest of the codebase uses; it returns an empty string on any
// failure so every caller fails closed rather than falling back to the unqualified name.
QString resolveSystemPowerShellPath() {
    const QString path = sak::systemPowerShellPath();
    if (path.isEmpty()) {
        sak::logError("ElevatedHelper: could not resolve the system PowerShell path");
    }
    return path;
}

// The command text crosses this boundary from the main app, and one of its senders is the AI
// assistant's elevated-command path (AiAssistantPanel::runElevatedPowerShell), so the text is
// NOT inherently trusted: the authorization decision (risk tier + human gate) is made in the app
// before the request is sent, and this process enforces only the parent-pid/image binding on the
// pipe. There is deliberately no command allowlist -- a technician utility must be able to run
// arbitrary administrative PowerShell -- so the containment that does apply is the
// KILL_ON_JOB_CLOSE job, the clamped timeout, the capped output, and success gated on
// completedSuccessfully(). -ExecutionPolicy Bypass is the standard programmatic mode.
QStringList elevatedPowerShellArgs(const QString& command) {
    return {QStringLiteral("-NoProfile"),
            QStringLiteral("-ExecutionPolicy"),
            QStringLiteral("Bypass"),
            QStringLiteral("-Command"),
            command};
}

void configureElevatedPowerShellProgress(sak::ProcessStreamingRequest* request,
                                         sak::ProgressCallback* progress,
                                         int chunk_cap) {
    request->on_output = [progress, chunk_cap](const QString& chunk, bool is_stderr) {
        // The live stream is deliberately not covered by the final output cap, so bound each
        // frame here: an unbounded chunk from a runaway child would otherwise be pushed straight
        // into the pipe (blowing the frame ceiling) and allocated in full on both sides.
        (*progress)(kPowerShellOutputProgress,
                    (is_stderr ? QStringLiteral("STDERR|") : QStringLiteral("STDOUT|")) +
                        cappedTaskOutput(chunk, chunk_cap));
    };
}

void configureElevatedPowerShellStart(sak::ProcessStreamingRequest* request,
                                      bool* started,
                                      sak::ProgressCallback* progress
#ifdef _WIN32
                                      ,
                                      ElevatedJobGuard* job
#endif
) {
    request->on_started = [started,
                           progress
#ifdef _WIN32
                           ,
                           job
#endif
    ](qint64 process_id) {
        *started = true;
#ifdef _WIN32
        job->assign(process_id);
#endif
        (*progress)(kPowerShellRunningProgress, QStringLiteral("Elevated PowerShell running"));
    };
}

void configureElevatedPowerShellTerminate(sak::ProcessStreamingRequest* request
#ifdef _WIN32
                                          ,
                                          ElevatedJobGuard* job
#endif
) {
    request->on_terminate = [
#ifdef _WIN32
                                job
#endif
    ]() {
#ifdef _WIN32
        job->terminate();
#endif
    };
}

sak::TaskHandlerResult runElevatedPowerShellTask(const QJsonObject& payload,
                                                 sak::ProgressCallback progress,
                                                 sak::CancelCheck is_cancelled) {
    QJsonObject data = baseElevatedPowerShellData();

    QString config_error;
    const auto config = elevatedPowerShellConfig(payload, &config_error);
    if (!config.has_value()) {
        return elevatedPowerShellFailure(data, config_error);
    }
    if (config->m_command.trimmed().isEmpty()) {
        return elevatedPowerShellFailure(data, QStringLiteral("PowerShell command is empty"));
    }
    // Report the limits actually in force so a clamped request is visible to the caller instead
    // of silently running under different bounds than it asked for.
    data[QStringLiteral("timeout_ms")] = config->m_timeout_ms;
    data[QStringLiteral("max_output_bytes")] = config->m_output_cap;

    const QString powershell_path = resolveSystemPowerShellPath();
    if (powershell_path.isEmpty()) {
        return elevatedPowerShellFailure(
            data, QStringLiteral("Could not resolve system PowerShell path"));
    }

    QElapsedTimer timer;
    timer.start();

    progress(kPowerShellStartProgress, QStringLiteral("Starting elevated PowerShell"));

#ifdef _WIN32
    ElevatedJobGuard job;
#endif

    bool started = false;
    sak::ProcessStreamingRequest request;
    request.program = powershell_path;
    request.args = elevatedPowerShellArgs(config->m_command);
    request.timeout_ms = config->m_timeout_ms;
    configureElevatedPowerShellProgress(&request,
                                        &progress,
                                        std::min(config->m_half_cap, kMaxProgressChunkBytes));
    configureElevatedPowerShellStart(&request,
                                     &started,
                                     &progress
#ifdef _WIN32
                                     ,
                                     &job
#endif
    );
    configureElevatedPowerShellTerminate(&request
#ifdef _WIN32
                                         ,
                                         &job
#endif
    );
    request.should_cancel = std::move(is_cancelled);
    const auto result = sak::runProcessStreaming(request);

    if (!started) {
        return elevatedPowerShellFailure(data,
                                         elevatedPowerShellStartError(result),
                                         timer.elapsed());
    }

    return elevatedPowerShellOutcome(data, result, timer.elapsed(), config->m_half_cap, progress);
}

bool isAllowedPhysicalDrivePath(const QString& path) {
    const QString prefix = QStringLiteral("\\\\.\\PhysicalDrive");
    if (!path.startsWith(prefix, Qt::CaseInsensitive)) {
        return false;
    }
    const QString suffix = path.mid(prefix.size());
    if (suffix.isEmpty()) {
        return false;
    }
    return std::ranges::all_of(suffix, [](QChar ch) { return ch.isDigit(); });
}

// Largest integer a JSON double carries exactly (2^53). Beyond it a double no longer identifies
// a single byte offset, so a value that big is refused instead of being silently rounded.
constexpr double kMaxExactJsonInteger = 9007199254740992.0;

std::optional<uint64_t> payloadUInt64(const QJsonObject& payload, const QString& key) {
    const auto value = payload.value(key);
    bool ok = false;
    const QString text = value.toString();
    if (!text.isEmpty()) {
        const auto parsed = text.toULongLong(&ok);
        if (ok) {
            return static_cast<uint64_t>(parsed);
        }
    }
    if (value.isDouble()) {
        // Fail closed on a fractional value, on a value too large to be represented exactly, and
        // on anything outside the range: casting such a double to uint64_t is undefined
        // behaviour, and these values pick raw-disk offsets and lengths.
        const double raw = value.toDouble();
        if (std::isfinite(raw) && raw >= 0.0 && raw <= kMaxExactJsonInteger &&
            std::floor(raw) == raw) {
            return static_cast<uint64_t>(raw);
        }
    }
    return std::nullopt;
}

// Both inputs are already validated as non-zero by the caller, so every bound here narrows the
// read; nothing widens it back to the ceiling.
uint64_t partitionProbeReadLimit(uint64_t partition_size_bytes, uint64_t requested_bytes) {
    uint64_t limit = kMaxPartitionProbeBytes;
    limit = std::min(limit, partition_size_bytes);
    limit = std::min(limit, requested_bytes);
    return limit;
}

struct PartitionProbeRequest {
    QString m_device_path;
    uint64_t m_offset_bytes{0};
    uint64_t m_read_limit{0};
};

sak::TaskHandlerResult partitionProbeFailure(const QString& message) {
    return {.success = false, .data = {}, .error_message = message};
}

// Resolve the optional max_bytes override. Absent means "the 2 MiB ceiling"; present means it
// MUST parse and MUST be non-zero -- an explicit zero asks for no bytes and a malformed value
// must not be read as "no limit".
std::optional<uint64_t> partitionProbeRequestedBytes(const QJsonObject& payload,
                                                     QString* error_message) {
    if (payload.value(QStringLiteral("max_bytes")).isUndefined()) {
        return kMaxPartitionProbeBytes;
    }
    const auto requested = payloadUInt64(payload, QStringLiteral("max_bytes"));
    if (!requested.has_value() || *requested == 0) {
        *error_message = QStringLiteral("Raw probe byte limit is invalid");
        return std::nullopt;
    }
    return requested;
}

// Refuse an absent or wrong-typed device_path at the field boundary rather than coercing it to an
// empty string: this required field selects the raw disk that gets read, so a missing or non-string
// value is rejected, never defaulted. isAllowedPhysicalDrivePath then constrains the string's shape
// as defence in depth.
std::optional<QString> partitionProbeDevicePath(const QJsonObject& payload,
                                                QString* error_message) {
    const QJsonValue device_value = payload.value(QStringLiteral("device_path"));
    if (!device_value.isString()) {
        *error_message = QStringLiteral("Raw probe device_path is required and must be a string");
        return std::nullopt;
    }
    const QString device_path = device_value.toString();
    if (!isAllowedPhysicalDrivePath(device_path)) {
        *error_message = QStringLiteral("Raw probe target must be \\\\.\\PhysicalDriveN");
        return std::nullopt;
    }
    return device_path;
}

std::optional<PartitionProbeRequest> partitionProbeRequest(const QJsonObject& payload,
                                                           QString* error_message) {
    const auto device_path = partitionProbeDevicePath(payload, error_message);
    if (!device_path.has_value()) {
        return std::nullopt;
    }

    const auto offset = payloadUInt64(payload, QStringLiteral("partition_offset_bytes"));
    const auto partition_size = payloadUInt64(payload, QStringLiteral("partition_size_bytes"));
    if (!offset.has_value() || !partition_size.has_value()) {
        *error_message = QStringLiteral("Raw probe offset and partition size are required");
        return std::nullopt;
    }
    if (*offset > static_cast<uint64_t>(std::numeric_limits<qint64>::max())) {
        *error_message = QStringLiteral("Raw probe offset is too large");
        return std::nullopt;
    }
    // A declared zero-length partition bounds the read to nothing: accepting it and then falling
    // back to the 2 MiB ceiling would read data the request never described.
    if (*partition_size == 0) {
        *error_message = QStringLiteral("Raw probe partition size must be greater than zero");
        return std::nullopt;
    }
    const auto requested_bytes = partitionProbeRequestedBytes(payload, error_message);
    if (!requested_bytes.has_value()) {
        return std::nullopt;
    }
    const uint64_t read_limit = partitionProbeReadLimit(*partition_size, *requested_bytes);
    if (read_limit == 0 || read_limit > kMaxPartitionProbeBytes) {
        *error_message = QStringLiteral("Raw probe byte limit is invalid");
        return std::nullopt;
    }
    return PartitionProbeRequest{.m_device_path = *device_path,
                                 .m_offset_bytes = *offset,
                                 .m_read_limit = read_limit};
}

std::optional<QByteArray> readPartitionProbeBytes(const PartitionProbeRequest& request,
                                                  QString* error_message) {
    QFile device(request.m_device_path);
    if (!device.open(QIODevice::ReadOnly)) {
        *error_message = QStringLiteral("Raw probe open failed: %1").arg(device.errorString());
        return std::nullopt;
    }
    if (!device.seek(static_cast<qint64>(request.m_offset_bytes))) {
        *error_message = QStringLiteral("Raw probe seek failed: %1").arg(device.errorString());
        return std::nullopt;
    }
    const QByteArray bytes = device.read(static_cast<qint64>(request.m_read_limit));
    // Fail closed: a read error invalidates the probe even if some bytes came back, and a
    // zero-byte read yields no signature to analyze. A short (non-empty, error-free) read is
    // legitimate on a small partition, so those are accepted.
    if (device.error() != QFileDevice::NoError) {
        *error_message = QStringLiteral("Raw probe read failed: %1").arg(device.errorString());
        return std::nullopt;
    }
    if (bytes.isEmpty()) {
        *error_message = QStringLiteral("Raw probe read returned no data");
        return std::nullopt;
    }
    return bytes;
}

sak::TaskHandlerResult partitionProbeSuccess(const PartitionProbeRequest& request,
                                             const QByteArray& bytes) {
    QJsonObject data;
    data[QStringLiteral("device_path")] = request.m_device_path;
    data[QStringLiteral("bytes_read")] = static_cast<double>(bytes.size());
    data[QStringLiteral("bytes_base64")] = QString::fromLatin1(bytes.toBase64());
    return {.success = true, .data = data, .error_message = {}};
}

sak::TaskHandlerResult runPartitionProbeReadTask(const QJsonObject& payload,
                                                 sak::ProgressCallback progress,
                                                 sak::CancelCheck is_cancelled) {
    QString error_message;
    const auto request = partitionProbeRequest(payload, &error_message);
    if (!request.has_value()) {
        return partitionProbeFailure(error_message);
    }
    if (is_cancelled()) {
        return partitionProbeFailure(QStringLiteral("Raw probe cancelled"));
    }

    progress(0, QStringLiteral("Reading partition signature probe"));
    const auto bytes = readPartitionProbeBytes(*request, &error_message);
    if (!bytes.has_value()) {
        return partitionProbeFailure(error_message);
    }
    if (is_cancelled()) {
        return partitionProbeFailure(QStringLiteral("Raw probe cancelled"));
    }

    progress(sak::kPercentMax, QStringLiteral("Partition signature probe read"));
    return partitionProbeSuccess(*request, *bytes);
}

void registerQuickActionTasks(sak::ElevatedTaskDispatcher& dispatcher) {
    dispatcher.registerHandler(QStringLiteral("Verify System Files"),
                               [](const QJsonObject& payload,
                                  sak::ProgressCallback progress,
                                  sak::CancelCheck is_cancelled) {
                                   return runQuickAction<sak::VerifySystemFilesAction>(
                                       payload, std::move(progress), std::move(is_cancelled));
                               });

    dispatcher.registerHandler(QStringLiteral("Check Disk Errors"),
                               [](const QJsonObject& payload,
                                  sak::ProgressCallback progress,
                                  sak::CancelCheck is_cancelled) {
                                   return runQuickAction<sak::CheckDiskErrorsAction>(
                                       payload, std::move(progress), std::move(is_cancelled));
                               });

    dispatcher.registerHandler(QStringLiteral("Reset Network"),
                               [](const QJsonObject& payload,
                                  sak::ProgressCallback progress,
                                  sak::CancelCheck is_cancelled) {
                                   return runQuickAction<sak::ResetNetworkAction>(
                                       payload, std::move(progress), std::move(is_cancelled));
                               });

    dispatcher.registerHandler(
        QStringLiteral("Backup BitLocker Keys"),
        [](const QJsonObject& payload,
           sak::ProgressCallback progress,
           sak::CancelCheck is_cancelled) -> sak::TaskHandlerResult {
            // The task PAYLOAD is validated here, not only the calling client image: this
            // destination receives PLAINTEXT recovery keys. An absent or wrong-typed value is
            // REFUSED rather than coerced to a built-in default -- the elevated helper must
            // never invent a destination the requester did not ask for. The action then
            // canonicalizes and policy-screens the value (screenBackupLocation) before it
            // creates anything.
            const QJsonValue raw = payload.value(QStringLiteral("backup_location"));
            if (!raw.isString() || raw.toString().trimmed().isEmpty()) {
                return {.success = false,
                        .data = {},
                        .error_message = "Missing or invalid backup_location in payload"};
            }
            return runQuickAction<sak::BackupBitlockerKeysAction>(
                payload, std::move(progress), std::move(is_cancelled), raw.toString());
        });
}

void registerPermissionTasks(sak::ElevatedTaskDispatcher& dispatcher) {
    dispatcher.registerHandler(
        QStringLiteral("TakeOwnership"),
        [](const QJsonObject& payload,
           sak::ProgressCallback progress,
           sak::CancelCheck is_cancelled) -> sak::TaskHandlerResult {
            return runPermissionTask(
                payload,
                std::move(progress),
                is_cancelled,
                PermissionTaskSpec{
                    .m_operation =
                        [](sak::PermissionManager& pm, const QString& path, const QString& sid) {
                            return pm.tryTakeOwnership(path, sid);
                        },
                    .m_action_label = QStringLiteral("Taking ownership of: "),
                    .m_needs_user_sid = true});
        });

    dispatcher.registerHandler(
        QStringLiteral("StripPermissions"),
        [](const QJsonObject& payload,
           sak::ProgressCallback progress,
           sak::CancelCheck is_cancelled) -> sak::TaskHandlerResult {
            return runPermissionTask(
                payload,
                std::move(progress),
                is_cancelled,
                PermissionTaskSpec{.m_operation =
                                       [](sak::PermissionManager& pm,
                                          const QString& path,
                                          const QString& /*sid*/) {
                                           return pm.tryStripPermissions(path);
                                       },
                                   .m_action_label = QStringLiteral("Stripping permissions: "),
                                   .m_needs_user_sid = false});
        });

    dispatcher.registerHandler(
        QStringLiteral("SetStandardPermissions"),
        [](const QJsonObject& payload,
           sak::ProgressCallback progress,
           sak::CancelCheck is_cancelled) -> sak::TaskHandlerResult {
            return runPermissionTask(
                payload,
                std::move(progress),
                is_cancelled,
                PermissionTaskSpec{
                    .m_operation =
                        [](sak::PermissionManager& pm, const QString& path, const QString& sid) {
                            return pm.trySetStandardUserPermissions(path, sid);
                        },
                    .m_action_label = QStringLiteral("Setting permissions: "),
                    .m_needs_user_sid = true});
        });
}

// The catch branch reports the real CIM failure on stderr and still emits the -1 sentinel, so a
// missing sensor and a broken query are told apart instead of both surfacing as "no data".
QString thermalQueryScript() {
    return QStringLiteral(
        "try{"
        "$t=Get-CimInstance -Namespace root/WMI "
        "-ClassName MSAcpi_ThermalZoneTemperature "
        "-ErrorAction Stop|Select-Object -First 1;"
        "if($t.CurrentTemperature -gt 0){"
        "Write-Output ([math]::Round(($t.CurrentTemperature/10)-273.15,1))"
        "}else{Write-Output '-1'}"
        "}catch{Write-Error $_;Write-Output '-1'}");
}

QString withThermalDetail(const QString& message, const sak::ProcessResult& result) {
    const QString detail = result.std_err.trimmed();
    if (detail.isEmpty()) {
        return message;
    }
    return message + QStringLiteral(": ") + detail;
}

sak::TaskHandlerResult runThermalDataTask(const QJsonObject& /*payload*/,
                                          sak::ProgressCallback progress,
                                          sak::CancelCheck is_cancelled) {
    if (is_cancelled()) {
        return {.success = false, .data = {}, .error_message = "Task cancelled before it started"};
    }
    progress(0, QStringLiteral("Querying thermal sensors..."));

    // Resolve the absolute System32 PowerShell path. Launching an unqualified
    // "powershell.exe" from this elevated helper would resolve via PATH/CWD, which
    // an attacker controlling a PATH entry or the working directory could hijack to
    // run arbitrary code elevated. Fail closed if it cannot be resolved.
    const QString powershell_path = resolveSystemPowerShellPath();
    if (powershell_path.isEmpty()) {
        return {.success = false,
                .data = {},
                .error_message = "Could not resolve system PowerShell path"};
    }

    const auto result = sak::runProcess(powershell_path,
                                        {QStringLiteral("-NoProfile"),
                                         QStringLiteral("-NoLogo"),
                                         QStringLiteral("-Command"),
                                         thermalQueryScript()},
                                        kThermalQueryTimeoutMs,
                                        is_cancelled);
    // Report what actually went wrong: a start failure, a crash and a non-zero exit are not
    // timeouts, and the child's stderr is the only description of the real fault.
    if (result.cancelled) {
        return {.success = false, .data = {}, .error_message = "Thermal query cancelled"};
    }
    if (result.timed_out) {
        return {.success = false, .data = {}, .error_message = "Thermal query timed out"};
    }
    if (!result.succeeded()) {
        return {.success = false,
                .data = {},
                .error_message = withThermalDetail(
                    QStringLiteral("Thermal query failed with exit code %1").arg(result.exit_code),
                    result)};
    }

    const QString output = result.std_out.trimmed();
    bool ok = false;
    const double temp = output.toDouble(&ok);
    if (!ok || temp <= 0) {
        return {.success = false,
                .data = {},
                .error_message = withThermalDetail(QStringLiteral("No thermal data available"),
                                                   result)};
    }

    QJsonObject data;
    data["cpu_temperature"] = temp;
    progress(sak::kPercentMax, QStringLiteral("Thermal data retrieved"));
    return {.success = true, .data = data, .error_message = {}};
}

void registerFileTasks(sak::ElevatedTaskDispatcher& dispatcher) {
    dispatcher.registerHandler(
        QStringLiteral("BackupFile"),
        [](const QJsonObject& payload,
           sak::ProgressCallback progress,
           sak::CancelCheck is_cancelled) -> sak::TaskHandlerResult {
            // Refuse an absent or wrong-typed field instead of coercing it to an empty string:
            // this handler copies files with administrator rights.
            const QJsonValue source_value = payload.value(QStringLiteral("source"));
            const QJsonValue destination_value = payload.value(QStringLiteral("destination"));
            if (!source_value.isString() || source_value.toString().isEmpty() ||
                !destination_value.isString() || destination_value.toString().isEmpty()) {
                return {.success = false,
                        .data = {},
                        .error_message = "Missing or invalid source or destination in payload"};
            }
            const QString source = source_value.toString();
            const QString destination = destination_value.toString();
            // QFile::copy is a single uninterruptible call, so a cancel can only be honored
            // before it starts.
            if (is_cancelled()) {
                return {.success = false,
                        .data = {},
                        .error_message = "Task cancelled before it started"};
            }
            progress(0, QStringLiteral("Copying: ") + source);
            if (!QFile::copy(source, destination)) {
                return {.success = false,
                        .data = {},
                        .error_message = QStringLiteral("Failed to copy: ") + source};
            }
            progress(sak::kPercentMax, QStringLiteral("File copied"));
            return {.success = true, .data = {}, .error_message = {}};
        });

    dispatcher.registerHandler(QStringLiteral("ReadThermalData"), runThermalDataTask);

    dispatcher.registerHandler(QStringLiteral("ReadPartitionProbe"), runPartitionProbeReadTask);
}

void registerAiTasks(sak::ElevatedTaskDispatcher& dispatcher) {
    dispatcher.registerHandler(QStringLiteral("RunPowerShell"), runElevatedPowerShellTask);
}

void registerAllTasks(sak::ElevatedTaskDispatcher& dispatcher) {
    registerQuickActionTasks(dispatcher);
    registerPermissionTasks(dispatcher);
    registerFileTasks(dispatcher);
    registerAiTasks(dispatcher);
}

// ======================================================================
// Main Event Loop
// ======================================================================

void sendTaskDispatchResult(sak::ElevatedPipeServer& server,
                            const std::expected<sak::TaskHandlerResult, sak::error_code>& result) {
    if (result) {
        QJsonObject data = result->data;
        if (!result->success) {
            // A failure must always carry an error: never hand the client a bare "failed" with
            // nothing that says why.
            data[QStringLiteral("error_message")] =
                result->error_message.isEmpty()
                    ? QStringLiteral("Task failed without an error message")
                    : result->error_message;
        }
        server.sendResult(result->success, data);
    } else {
        server.sendError(static_cast<int>(result.error()),
                         QString::fromStdString(std::string(sak::to_string(result.error()))));
    }
}

enum class ActiveTaskOutcome {
    Completed,
    ShutdownRequested,
    ClientLost
};

/// @brief Handle one client message that arrived while a task is running.
/// @return true when the client asked the helper to shut down once the task finishes.
/// @note The caller holds the server mutex, so this may use the server directly.
bool applyActiveTaskMessage(const sak::PipeMessage& msg,
                            sak::ElevatedPipeServer& server,
                            const QString& active_task_id,
                            const std::shared_ptr<std::atomic_bool>& cancel_requested) {
    switch (msg.type) {
    case sak::PipeMessageType::CancelRequest: {
        // A cancel that names a DIFFERENT task is stale (the task it names already finished):
        // acting on it would abort work the client never asked to stop. A cancel that names no
        // task at all still applies to the one running task.
        const QString target = msg.json.value(QStringLiteral("task")).toString();
        if (!target.isEmpty() && target != active_task_id) {
            sak::logWarning("ElevatedHelper: ignoring stale cancel for '{}' (active task '{}')",
                            target.toStdString(),
                            active_task_id.toStdString());
            return false;
        }
        cancel_requested->store(true, std::memory_order_relaxed);
        sak::logInfo("ElevatedHelper: active task cancel requested");
        return false;
    }
    case sak::PipeMessageType::Shutdown:
        cancel_requested->store(true, std::memory_order_relaxed);
        sak::logInfo("ElevatedHelper: shutdown requested during active task");
        return true;
    case sak::PipeMessageType::TaskRequest:
        // Answer instead of dropping: the helper runs one task at a time, and a silently
        // discarded request leaves the client waiting for a result that will never be sent.
        sak::logWarning("ElevatedHelper: refused a task request while '{}' is running",
                        active_task_id.toStdString());
        server.sendError(
            static_cast<int>(sak::error_code::invalid_argument),
            QStringLiteral("Elevated helper is busy with task '%1'").arg(active_task_id));
        return false;
    default:
        sak::logWarning("ElevatedHelper: ignored message type {} during active task",
                        static_cast<int>(msg.type));
        return false;
    }
}

// The task runs on a std::async worker that reports progress through the same pipe server this
// loop polls, and ElevatedPipeServer is documented single-threaded. Every touch of the server on
// either thread is serialized through @p server_mutex.
ActiveTaskOutcome finishActiveTaskWithCancelPolling(
    sak::ElevatedPipeServer& server,
    std::mutex& server_mutex,
    const QString& active_task_id,
    std::future<std::expected<sak::TaskHandlerResult, sak::error_code>>* future,
    const std::shared_ptr<std::atomic_bool>& cancel_requested) {
    bool shutdown_after_task = false;
    while (future->wait_for(std::chrono::milliseconds(kActiveTaskCancelPollMs)) !=
           std::future_status::ready) {
        const std::lock_guard<std::mutex> lock(server_mutex);
        switch (server.pollPipe()) {
        case sak::ElevatedPipeServer::PipePoll::NoData:
            continue;
        case sak::ElevatedPipeServer::PipePoll::Broken:
            // The client pipe is broken (the client died): cancel the privileged task rather than
            // spin forever treating a dead pipe as "no message" -- a dead client can no longer
            // send an explicit cancel.
            cancel_requested->store(true, std::memory_order_relaxed);
            sak::logWarning("ElevatedHelper: client pipe broken during active task; cancelling");
            return ActiveTaskOutcome::ClientLost;
        case sak::ElevatedPipeServer::PipePoll::MessageReady:
            break;
        }

        auto msg = server.readMessage();
        if (!msg) {
            cancel_requested->store(true, std::memory_order_relaxed);
            sak::logWarning("ElevatedHelper: read failed during active task ({}); cancelling",
                            std::string(sak::to_string(msg.error())));
            return ActiveTaskOutcome::ClientLost;
        }

        if (applyActiveTaskMessage(*msg, server, active_task_id, cancel_requested)) {
            shutdown_after_task = true;
        }
    }

    {
        const std::lock_guard<std::mutex> lock(server_mutex);
        sendTaskDispatchResult(server, future->get());
    }
    return shutdown_after_task ? ActiveTaskOutcome::ShutdownRequested
                               : ActiveTaskOutcome::Completed;
}

// Run one TaskRequest to completion. @p server_mutex serializes the worker's progress sends
// against this thread's pipe polling.
ActiveTaskOutcome runTaskRequest(const sak::PipeMessage& msg,
                                 sak::ElevatedPipeServer& server,
                                 std::mutex& server_mutex,
                                 sak::ElevatedTaskDispatcher& dispatcher) {
    // Validate the message shape before anything privileged runs. An absent or wrong-typed
    // task/payload is REFUSED, never coerced into an empty string or an empty object: a handler
    // that ignores its payload (Reset Network) would otherwise run off a malformed request.
    const QJsonValue task_value = msg.json.value(QStringLiteral("task"));
    const QJsonValue payload_value = msg.json.value(QStringLiteral("payload"));
    if (!task_value.isString() || task_value.toString().trimmed().isEmpty() ||
        !payload_value.isObject()) {
        sak::logError("ElevatedHelper: malformed TaskRequest refused");
        const std::lock_guard<std::mutex> lock(server_mutex);
        server.sendError(static_cast<int>(sak::error_code::invalid_argument),
                         QStringLiteral("Malformed task request: 'task' must be a non-empty "
                                        "string and 'payload' must be an object"));
        return ActiveTaskOutcome::Completed;
    }

    const QString task_id = task_value.toString();
    const QJsonObject payload = payload_value.toObject();
    auto cancel_requested = std::make_shared<std::atomic_bool>(false);

    sak::logInfo("ElevatedHelper: executing task '{}'", task_id.toStdString());

    auto progress_cb = [&server, &server_mutex](int pct, const QString& status) {
        const std::lock_guard<std::mutex> lock(server_mutex);
        server.sendProgress(pct, status);
    };
    auto cancel_cb = [cancel_requested]() {
        return cancel_requested->load(std::memory_order_relaxed);
    };

    auto future =
        std::async(std::launch::async,
                   [&dispatcher,
                    task_id,
                    payload,
                    progress_cb = std::move(progress_cb),
                    cancel_cb = std::move(cancel_cb)]() mutable {
                       return dispatcher.dispatch(task_id, payload, progress_cb, cancel_cb);
                   });
    return finishActiveTaskWithCancelPolling(
        server, server_mutex, task_id, &future, cancel_requested);
}

int runHelper(sak::ElevatedPipeServer& server, sak::ElevatedTaskDispatcher& dispatcher) {
    // Guards every use of the pipe server. Only the task path is concurrent (the handler's
    // progress callback runs on the async worker), but the mutex is held on this thread's server
    // calls too so the two can never drive the documented single-threaded server at once.
    std::mutex server_mutex;

    server.sendReady();
    sak::logInfo("ElevatedHelper: sent Ready, entering task loop");

    while (true) {
        auto msg = server.readMessage();
        if (!msg) {
            sak::logInfo("ElevatedHelper: client disconnected -- shutting down");
            break;
        }

        switch (msg->type) {
        case sak::PipeMessageType::Shutdown: {
            sak::logInfo("ElevatedHelper: received Shutdown");
            return 0;
        }
        case sak::PipeMessageType::CancelRequest: {
            sak::logInfo("ElevatedHelper: cancel requested with no active task");
            break;
        }
        case sak::PipeMessageType::TaskRequest: {
            switch (runTaskRequest(*msg, server, server_mutex, dispatcher)) {
            case ActiveTaskOutcome::Completed:
                break;
            case ActiveTaskOutcome::ShutdownRequested:
                return 0;
            case ActiveTaskOutcome::ClientLost:
                // The task ran but its result could never be delivered. Report that as an
                // abnormal exit rather than a clean one.
                return kHelperClientLostExitCode;
            }
            break;
        }
        default:
            sak::logWarning("ElevatedHelper: unexpected message type {}",
                            static_cast<int>(msg->type));
            break;
        }
    }

    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    const QCoreApplication app(argc, argv);
    app.setApplicationName("SAK Elevated Helper");

    // Initialize logger (logs directory is independent of TMP/TEMP)
    const QString log_path = sak::app_paths::logsDirectory();
    auto log_dir = std::filesystem::path(log_path.toStdWString());
    auto& logger = sak::logger::instance();
    if (auto result = logger.initialize(log_dir); !result) {
        return 1;
    }

    // Fail closed: refuse to run elevated tasks if we cannot establish a controlled
    // TMP/TEMP rather than inherit an attacker-influenced one.
    if (!configurePortableRuntimeDirs()) {
        sak::logError("ElevatedHelper: cannot establish controlled runtime dirs; aborting");
        return kHelperStartupFailureExitCode;
    }

    sak::logInfo("========================================");
    sak::logInfo("SAK Elevated Helper starting");
    sak::logInfo("========================================");

    const auto parsed_args = parseArgs(argc, argv);
    if (!parsed_args.has_value()) {
        sak::logError("ElevatedHelper: refusing to run with a malformed command line");
        return 1;
    }
    const HelperArgs args = *parsed_args;
    if (args.m_pipe_name.isEmpty()) {
        sak::logError("ElevatedHelper: --pipe argument required");
        return 1;
    }
    if (args.m_parent_pid <= 0) {
        sak::logError("ElevatedHelper: --parent-pid argument required");
        return 1;
    }

    sak::logInfo("ElevatedHelper: pipe='{}', parent_pid={}",
                 args.m_pipe_name.toStdString(),
                 args.m_parent_pid);

    // Register task handlers (compile-time allowlist)
    sak::ElevatedTaskDispatcher dispatcher;
    registerAllTasks(dispatcher);
    sak::logInfo("ElevatedHelper: registered {} task handlers", dispatcher.handlerCount());

    // Start pipe server and wait for connection
    sak::ElevatedPipeServer server(args.m_pipe_name, args.m_parent_pid);
    if (!server.start()) {
        sak::logError("ElevatedHelper: failed to start pipe server");
        return kHelperStartupFailureExitCode;
    }

    const int exit_code = runHelper(server, dispatcher);

    server.stop();
    sak::logInfo("ElevatedHelper: exiting with code {}", exit_code);
    logger.flush();

    return exit_code;
}

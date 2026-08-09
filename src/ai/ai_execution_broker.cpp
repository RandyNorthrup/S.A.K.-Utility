// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_execution_broker.h"

#include "sak/ai/ai_credential_store.h"
#include "sak/process_runner.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#ifdef _WIN32
#include <windows.h>

#include <tlhelp32.h>
#endif

#include <algorithm>
#include <utility>
#include <vector>

namespace sak::ai {

namespace {

constexpr int kTimeoutTickMs = 200;
constexpr int kMinTimeoutSeconds = 5;
constexpr int kMaxTimeoutSeconds = 3600;
constexpr int kMinOutputCap = 1024;
// Hard ceiling on what one run may make the broker retain per stream. Without it a caller
// (or a tool argument that reached one) could ask for a multi-gigabyte buffer.
constexpr int kMaxOutputCap = 16 * 1024 * 1024;
// The Windows command line tops out at 32767 characters, so anything beyond this could
// only fail deep inside CreateProcess with an opaque error.
constexpr int kMaxCommandChars = 30'000;
constexpr int kMaxArgumentChars = 30'000;
constexpr int kMaxArgumentCount = 256;

[[nodiscard]] QString cappedHeadTail(const QString& value,
                                     int max_bytes,
                                     bool* truncated = nullptr) {
    if (truncated != nullptr) {
        *truncated = false;
    }
    if (max_bytes <= 0) {
        if (truncated != nullptr) {
            *truncated = !value.isEmpty();
        }
        return {};
    }
    const QByteArray bytes = value.toUtf8();
    if (bytes.size() <= max_bytes) {
        return value;
    }
    if (truncated != nullptr) {
        *truncated = true;
    }
    // Keep the head AND the tail, eliding only the middle: the terminal lines usually
    // carry the actual failure/error, so keeping only the head would hide it.
    const int omitted = static_cast<int>(bytes.size()) - max_bytes;
    const int head_bytes = max_bytes / 2;
    const int tail_bytes = max_bytes - head_bytes;
    return QString::fromUtf8(bytes.left(head_bytes)) +
           QStringLiteral("\n...[%1 bytes truncated]...\n").arg(omitted) +
           QString::fromUtf8(bytes.right(tail_bytes));
}

/// @brief Resolve a bare program name (no path separator) to an absolute path.
///
/// CreateProcess searches the CURRENT DIRECTORY ahead of PATH, so a bare name lets a
/// planted binary win -- the same hijack the System32-qualified shells above refuse.
/// System32 wins first, then only the ABSOLUTE entries of PATH (never "." or a relative
/// entry, which resolve against the working directory). Empty return -> caller fails closed.
[[nodiscard]] QString resolveBareProgram(const QString& name) {
#ifdef Q_OS_WIN
    const QString system32 = sak::system32Path(name);
    if (!system32.isEmpty() && QFileInfo(system32).isFile()) {
        return system32;
    }
#endif
    QStringList search;
    const QStringList entries = qEnvironmentVariable("PATH").split(QDir::listSeparator(),
                                                                   Qt::SkipEmptyParts);
    for (const QString& entry : entries) {
        const QString trimmed = entry.trimmed();
        if (trimmed.isEmpty() || !QDir::isAbsolutePath(trimmed)) {
            continue;
        }
        search.append(trimmed);
    }
    if (search.isEmpty()) {
        return {};
    }
    return QStandardPaths::findExecutable(name, search);
}

/// @brief Read one optional numeric field, failing closed on a wrong type or a value
/// outside the accepted domain instead of substituting a default.
[[nodiscard]] bool parseOptionalTimeoutSeconds(const QJsonObject& args, int& out, QString& error) {
    const QJsonValue value = args.value(QStringLiteral("timeout_seconds"));
    if (value.isUndefined() || value.isNull()) {
        return true;
    }
    if (!value.isDouble()) {
        error = QStringLiteral("timeout_seconds must be a number");
        return false;
    }
    const double raw = value.toDouble();
    if (raw < kMinTimeoutSeconds || raw > kMaxTimeoutSeconds) {
        error = QStringLiteral("timeout_seconds must be between %1 and %2")
                    .arg(kMinTimeoutSeconds)
                    .arg(kMaxTimeoutSeconds);
        return false;
    }
    out = static_cast<int>(raw);
    return true;
}

[[nodiscard]] bool parseOptionalRequiresAdmin(const QJsonObject& args, bool& out, QString& error) {
    const QJsonValue value = args.value(QStringLiteral("requires_admin"));
    if (value.isUndefined() || value.isNull()) {
        return true;
    }
    if (!value.isBool()) {
        // A non-bool must never coerce to false: that is the elevation classification.
        error = QStringLiteral("requires_admin must be a boolean");
        return false;
    }
    out = value.toBool();
    return true;
}

[[nodiscard]] bool parseRequiredString(const QJsonObject& args,
                                       const QString& key,
                                       QString& out,
                                       QString& error) {
    const QJsonValue value = args.value(key);
    if (!value.isString()) {
        error = QStringLiteral("%1 must be a string").arg(key);
        return false;
    }
    out = value.toString();
    if (out.size() > kMaxCommandChars) {
        error = QStringLiteral("%1 exceeds the %2 character limit").arg(key).arg(kMaxCommandChars);
        out.clear();
        return false;
    }
    return true;
}

/// @brief Append the optional "arguments" array to @p out, returning a non-empty message when
/// the field is present but is not an array of in-domain strings.
///
/// An absent field is not a rejection; a wrong-typed one is, because every repair here would
/// silently produce a different command line than the one that was reviewed.
[[nodiscard]] QString parseOptionalArguments(const QJsonObject& args, QStringList& out) {
    const QJsonValue arguments = args.value(QStringLiteral("arguments"));
    if (arguments.isUndefined() || arguments.isNull()) {
        return {};
    }
    if (!arguments.isArray()) {
        return QStringLiteral("arguments must be an array of strings");
    }
    const QJsonArray arg_array = arguments.toArray();
    if (arg_array.size() > kMaxArgumentCount) {
        return QStringLiteral("arguments exceeds the %1 entry limit").arg(kMaxArgumentCount);
    }
    for (const auto& value : arg_array) {
        if (!value.isString()) {
            // A non-string element silently becomes a blank argument, which changes the
            // command line away from the one that was reviewed.
            return QStringLiteral("every entry of arguments must be a string");
        }
        const QString argument = value.toString();
        if (argument.size() > kMaxArgumentChars) {
            return QStringLiteral("an argument exceeds the %1 character limit")
                .arg(kMaxArgumentChars);
        }
        out.append(argument);
    }
    return {};
}

}  // namespace

QJsonObject AiCommandResult::toJson() const {
    QJsonObject obj;
    obj[QStringLiteral("started")] = started;
    obj[QStringLiteral("cancelled")] = cancelled;
    obj[QStringLiteral("timed_out")] = timed_out;
    obj[QStringLiteral("elevated")] = elevated;
    obj[QStringLiteral("exit_code")] = exit_code;
    obj[QStringLiteral("exit_status")] = exit_status;
    obj[QStringLiteral("duration_ms")] = static_cast<double>(duration_ms);
    obj[QStringLiteral("output_truncated")] = output_truncated;
    obj[QStringLiteral("stdout")] = CredentialStore::redactSecrets(stdout_text);
    obj[QStringLiteral("stderr")] = CredentialStore::redactSecrets(stderr_text);
    obj[QStringLiteral("error_message")] = CredentialStore::redactSecrets(error_message);
    return obj;
}

QString AiCommandResult::toJsonString() const {
    return QString::fromUtf8(QJsonDocument(toJson()).toJson(QJsonDocument::Compact));
}

ExecutionBroker::ExecutionBroker(QObject* parent) : QObject(parent) {}

ExecutionBroker::~ExecutionBroker() {
    if (m_process) {
        m_process->disconnect(this);
        if (m_process->state() != QProcess::NotRunning) {
#ifdef _WIN32
            terminateProcessTree();
#endif
            m_process->kill();
        }
    }
#ifdef _WIN32
    closeJobHandle();
#endif
}

#ifdef _WIN32
namespace {
// Recursively terminate a process and its descendants via a Toolhelp snapshot. Used only as a
// fallback when the Job Object could not be established: it must run while the primary is still
// alive (before m_process->kill()), because a parent-PID walk cannot find descendants once the
// parent has exited and the children have been reparented.
constexpr int kMaxProcessTreeDepth = 32;

void killProcessTreeSnapshot(DWORD process_id, int depth, std::vector<DWORD>& visited) {
    if (process_id == 0 || depth >= kMaxProcessTreeDepth) {
        return;
    }
    // Parent-PID fields are stale once a PID is reused, so the snapshot graph can contain a
    // cycle. Without a visited set that cycle recurses until the stack is exhausted.
    if (std::find(visited.begin(), visited.end(), process_id) != visited.end()) {
        return;
    }
    visited.push_back(process_id);
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (::Process32FirstW(snapshot, &entry)) {
            do {
                if (entry.th32ParentProcessID == process_id) {
                    killProcessTreeSnapshot(entry.th32ProcessID, depth + 1, visited);
                }
            } while (::Process32NextW(snapshot, &entry));
        }
        ::CloseHandle(snapshot);
    }
    HANDLE process = ::OpenProcess(PROCESS_TERMINATE, FALSE, process_id);
    if (process) {
        ::TerminateProcess(process, 1);
        ::CloseHandle(process);
    }
}

void killProcessTreeSnapshot(DWORD process_id) {
    std::vector<DWORD> visited;
    killProcessTreeSnapshot(process_id, 0, visited);
}
}  // namespace

void ExecutionBroker::assignProcessToJob() {
    if (!m_process || m_process->processId() <= 0) {
        return;
    }
    if (!m_job_handle) {
        m_job_handle = ::CreateJobObjectW(nullptr, nullptr);
        if (!m_job_handle) {
            return;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!::SetInformationJobObject(
                m_job_handle, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            // Without KILL_ON_JOB_CLOSE the job cannot guarantee descendant cleanup; drop it
            // so teardown relies on the direct primary-process kill rather than a job that
            // will not reap survivors.
            closeJobHandle();
            return;
        }
    }
    const HANDLE process = ::OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE,
                                         FALSE,
                                         static_cast<DWORD>(m_process->processId()));
    if (!process) {
        // An empty job would make terminateProcessTree() call TerminateJobObject on a job
        // that contains nothing and then RETURN, skipping the snapshot fallback entirely.
        // Drop it so the fallback still reaps the tree.
        closeJobHandle();
        return;
    }
    if (!::AssignProcessToJobObject(m_job_handle, process)) {
        // The process is NOT in the job, so TerminateJobObject would silently reap nothing
        // and grandchildren could survive. Drop the job handle so teardown falls back to
        // killing the primary process directly instead of trusting an empty job.
        closeJobHandle();
    }
    ::CloseHandle(process);
}

void ExecutionBroker::terminateProcessTree() {
    if (m_job_handle) {
        if (::TerminateJobObject(m_job_handle, 1)) {
            return;
        }
        // The job refused the kill, so nothing was reaped. Fall through to the snapshot walk
        // instead of reporting containment that did not happen.
    }
    // The Job Object could not be established (CreateJobObject/SetInformation/OpenProcess/
    // AssignProcessToJobObject failed and dropped the handle). Fall back to a snapshot walk while
    // the primary is still alive so descendants are reaped rather than orphaned -- cleanup is
    // never weaker than the direct primary kill that follows at the call sites.
    if (m_process && m_process->processId() > 0) {
        killProcessTreeSnapshot(static_cast<DWORD>(m_process->processId()));
    }
}

void ExecutionBroker::closeJobHandle() {
    if (m_job_handle) {
        ::CloseHandle(m_job_handle);  // KILL_ON_JOB_CLOSE reaps any survivors
        m_job_handle = nullptr;
    }
}
#endif

void ExecutionBroker::setElevatedRunner(ElevatedRunner runner) {
    m_elevated_runner = std::move(runner);
}

void ExecutionBroker::setElevatedCancel(ElevatedCancel cancel) {
    m_elevated_cancel = std::move(cancel);
}

bool ExecutionBroker::isRunning() const noexcept {
    return m_running;
}

QString ExecutionBroker::runningCommandId() const {
    return m_running ? m_command_id : QString{};
}

namespace {
/// @brief Apply the caller's output ceiling to a result the elevated runner produced.
///
/// The streaming cap never saw these buffers -- the runner hands back whole strings -- so
/// without this the caller's max_output_bytes would mean nothing on the elevated path.
void capElevatedOutput(AiCommandResult& result, int max_output_bytes) {
    const int cap = std::max(std::min(max_output_bytes, kMaxOutputCap) / 2, kMinOutputCap);
    bool stdout_truncated = false;
    bool stderr_truncated = false;
    result.stdout_text = cappedHeadTail(result.stdout_text, cap, &stdout_truncated);
    result.stderr_text = cappedHeadTail(result.stderr_text, cap, &stderr_truncated);
    result.output_truncated = result.output_truncated || stdout_truncated || stderr_truncated;
}
}  // namespace

/// @brief Route an admin request through the injected elevated runner and settle its outcome.
///
/// Returns false when the run never happened (no runner wired) or when the broker was
/// destroyed underneath the synchronous call; true once the outcome has been reported.
bool ExecutionBroker::runElevatedRequest(const AiCommandRequest& request) {
    if (!m_elevated_runner) {
        AiCommandResult result;
        result.elevated = true;
        result.error_message = QStringLiteral("Elevated AI command execution is not connected");
        emitDeferredFinish(std::move(result));
        return false;
    }
    m_running = true;
    m_elevated_in_flight = true;
    // Both the emission and the runner below re-enter the event loop, so a receiver can
    // destroy this broker while the synchronous call is still on the stack. Touching a
    // member after that is a use-after-free, so bail the moment the guard goes null.
    const QPointer<ExecutionBroker> self(this);
    Q_EMIT started(m_command_id);
    if (self.isNull()) {
        return false;
    }
    QElapsedTimer timer;
    timer.start();
    // The runner uses ElevationBroker::executeTask which processes events
    // periodically so the UI can update and Stop can route cancel.
    AiCommandResult elevated_result = m_elevated_runner(request);
    if (self.isNull()) {
        return false;
    }
    if (elevated_result.duration_ms == 0) {
        elevated_result.duration_ms = timer.elapsed();
    }
    elevated_result.elevated = true;
    if (m_cancel_requested && !elevated_result.cancelled) {
        // Cancellation was requested and the runner did not report it. Never present a
        // cancelled command as a clean completion.
        elevated_result.cancelled = true;
        if (elevated_result.error_message.isEmpty()) {
            elevated_result.error_message =
                QStringLiteral("Command cancelled; the elevated runner did not confirm it");
        }
    }
    capElevatedOutput(elevated_result, request.max_output_bytes);
    m_elevated_in_flight = false;
    completeWith(std::move(elevated_result));
    return true;
}

bool ExecutionBroker::startPowerShell(const AiCommandRequest& request, const QString& command_id) {
    if (m_running) {
        AiCommandResult fail;
        fail.error_message = QStringLiteral("Broker is already running a command");
        emitDeferredStandaloneFinish(command_id, std::move(fail));
        return false;
    }
    ++m_request_generation;
    m_command_id = command_id;
    m_cancel_requested = false;
    m_finished_emitted = false;
    if (!request.validation_error.isEmpty()) {
        AiCommandResult fail;
        fail.error_message = request.validation_error;
        emitDeferredFinish(std::move(fail));
        return false;
    }
    if (request.command.trimmed().isEmpty()) {
        AiCommandResult fail;
        fail.error_message = QStringLiteral("PowerShell command is empty");
        emitDeferredFinish(std::move(fail));
        return false;
    }
    if (request.requires_admin) {
        return runElevatedRequest(request);
    }
    // System32-qualified interpreter, never a bare "powershell.exe": CreateProcess searches
    // the current directory ahead of System32, so a planted powershell would execute the
    // assistant's command. Unresolvable -> refuse the run (fail closed).
    const QString powershell = sak::systemPowerShellPath();
    if (powershell.isEmpty()) {
        AiCommandResult fail;
        fail.error_message = QStringLiteral(
            "Cannot resolve the System32 PowerShell path; refusing to launch a bare "
            "powershell.exe");
        emitDeferredFinish(std::move(fail));
        return false;
    }
    QStringList args;
    args << QStringLiteral("-NoProfile") << QStringLiteral("-ExecutionPolicy")
         << QStringLiteral("Bypass") << QStringLiteral("-Command") << request.command;
    return launchProcess(
        {powershell, args, request.timeout_seconds, request.max_output_bytes, command_id, false});
}

bool ExecutionBroker::startCmd(const AiCommandRequest& request, const QString& command_id) {
    if (m_running) {
        AiCommandResult fail;
        fail.error_message = QStringLiteral("Broker is already running a command");
        emitDeferredStandaloneFinish(command_id, std::move(fail));
        return false;
    }
    ++m_request_generation;
    m_command_id = command_id;
    m_cancel_requested = false;
    m_finished_emitted = false;
    if (!request.validation_error.isEmpty()) {
        AiCommandResult fail;
        fail.error_message = request.validation_error;
        emitDeferredFinish(std::move(fail));
        return false;
    }
    if (request.command.trimmed().isEmpty()) {
        AiCommandResult fail;
        fail.error_message = QStringLiteral("CMD command is empty");
        emitDeferredFinish(std::move(fail));
        return false;
    }
    if (request.requires_admin) {
        AiCommandResult fail;
        fail.error_message = QStringLiteral(
            "Elevated cmd.exe launch is not supported; use run_powershell for admin tasks.");
        emitDeferredFinish(std::move(fail));
        return false;
    }
    // System32-qualified shell, never a bare "cmd.exe" (same search-order hijack).
    const QString shell = sak::system32Path(QStringLiteral("cmd.exe"));
    if (shell.isEmpty()) {
        AiCommandResult fail;
        fail.error_message = QStringLiteral(
            "Cannot resolve the System32 cmd.exe path; refusing to launch a bare "
            "cmd.exe");
        emitDeferredFinish(std::move(fail));
        return false;
    }
    QStringList args;
    args << QStringLiteral("/c") << request.command;
    return launchProcess(
        {shell, args, request.timeout_seconds, request.max_output_bytes, command_id, false});
}

bool ExecutionBroker::startProcess(const AiCommandRequest& request, const QString& command_id) {
    if (m_running) {
        AiCommandResult fail;
        fail.error_message = QStringLiteral("Broker is already running a command");
        emitDeferredStandaloneFinish(command_id, std::move(fail));
        return false;
    }
    ++m_request_generation;
    m_command_id = command_id;
    m_cancel_requested = false;
    m_finished_emitted = false;
    if (!request.validation_error.isEmpty()) {
        AiCommandResult fail;
        fail.error_message = request.validation_error;
        emitDeferredFinish(std::move(fail));
        return false;
    }
    if (request.program.trimmed().isEmpty()) {
        AiCommandResult fail;
        fail.error_message = QStringLiteral("Program path is empty");
        emitDeferredFinish(std::move(fail));
        return false;
    }
    if (request.requires_admin) {
        AiCommandResult fail;
        fail.error_message = QStringLiteral(
            "Elevated direct-process launch is not supported; use run_powershell for admin tasks.");
        emitDeferredFinish(std::move(fail));
        return false;
    }
    // A bare name is resolved by CreateProcess against the current directory before PATH, so
    // resolve it here (System32, then absolute PATH entries) or refuse the run.
    QString program = request.program.trimmed();
    if (!program.contains(QLatin1Char('/')) && !program.contains(QLatin1Char('\\'))) {
        const QString resolved = resolveBareProgram(program);
        if (resolved.isEmpty()) {
            AiCommandResult fail;
            fail.error_message = QStringLiteral(
                                     "Cannot resolve program '%1' to an absolute path; refusing to "
                                     "launch a bare name")
                                     .arg(program);
            emitDeferredFinish(std::move(fail));
            return false;
        }
        program = resolved;
    }
    return launchProcess({program,
                          request.arguments,
                          request.timeout_seconds,
                          request.max_output_bytes,
                          command_id,
                          false});
}

namespace {
/// @brief Decide whether a launch request's limits are usable, returning a non-empty message
/// when they are not.
///
/// A non-positive limit is not a policy choice, it is a malformed request: refuse it instead
/// of substituting a working default. (An over-ceiling timeout is still capped rather than
/// refused: app-action plans legitimately carry multi-hour scan timeouts, and
/// kMaxTimeoutSeconds is this broker's own ceiling, not a validity claim.)
[[nodiscard]] QString launchLimitsError(const ProcessLaunchRequest& request) {
    if (request.timeout_seconds <= 0) {
        return QStringLiteral("timeout_seconds %1 is not a positive value")
            .arg(request.timeout_seconds);
    }
    if (request.max_output_bytes <= 0 || request.max_output_bytes > kMaxOutputCap) {
        return QStringLiteral("max_output_bytes %1 is outside the range 1-%2")
            .arg(request.max_output_bytes)
            .arg(kMaxOutputCap);
    }
    return {};
}
}  // namespace

/// @brief Wire the current process to the drain, completion, start and error handlers.
void ExecutionBroker::connectProcessSignals() {
    // No read-buffer cap is set here because QProcess has none to set -- setReadBufferSize
    // belongs to QAbstractSocket, not to every QIODevice. The bound a hostile child actually
    // runs into is at the drain: onReadyReadStdout/Stderr read everything available on each
    // signal, appendCapped bounds what is retained, and cappedHeadTail bounds what is emitted
    // to subscribers. What remains unbounded is Qt's own buffer while the event loop is
    // stalled, and Qt exposes no knob for that on a process channel.
    connect(m_process.get(),
            &QProcess::readyReadStandardOutput,
            this,
            &ExecutionBroker::onReadyReadStdout);
    connect(m_process.get(),
            &QProcess::readyReadStandardError,
            this,
            &ExecutionBroker::onReadyReadStderr);
    connect(m_process.get(),
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this](int code, QProcess::ExitStatus status) {
                onProcessFinished(code, static_cast<int>(status));
            });
    connect(m_process.get(), &QProcess::started, this, [this]() {
#ifdef _WIN32
        assignProcessToJob();
#endif
        // A directly connected receiver may destroy this broker from started().
        const QPointer<ExecutionBroker> self(this);
        Q_EMIT started(m_command_id);
        if (self.isNull()) {
            return;
        }
        if (m_timeout_ms > 0) {
            QTimer::singleShot(kTimeoutTickMs, this, &ExecutionBroker::onTimeoutTick);
        }
    });
    connect(m_process.get(), &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        onProcessError(static_cast<int>(error));
    });
}

bool ExecutionBroker::launchProcess(const ProcessLaunchRequest& request) {
    if (request.already_running_check && m_running) {
        AiCommandResult fail;
        fail.error_message = QStringLiteral("Broker is already running a command");
        emitDeferredStandaloneFinish(request.command_id, std::move(fail));
        return false;
    }

    m_command_id = request.command_id;

    const QString limits_error = launchLimitsError(request);
    if (!limits_error.isEmpty()) {
        AiCommandResult fail;
        fail.error_message = limits_error;
        emitDeferredFinish(std::move(fail));
        return false;
    }

    m_max_output_bytes = std::max(request.max_output_bytes, kMinOutputCap);
    m_stdout_buffer.clear();
    m_stderr_buffer.clear();
    m_output_dropped_bytes = 0;
    m_cancel_requested = false;
    m_finished_emitted = false;

    const int clamped = std::clamp(request.timeout_seconds, kMinTimeoutSeconds, kMaxTimeoutSeconds);
    m_timeout_ms = clamped * kMillisecondsPerSecond;

    m_process = std::make_unique<QProcess>(this);
    connectProcessSignals();

    m_running = true;
    m_timer.start();
    m_process->start(request.program, request.arguments);
    return true;
}

void ExecutionBroker::cancel() {
    if (!m_running || m_cancel_requested) {
        return;
    }
    m_cancel_requested = true;
    if (m_elevated_in_flight && m_elevated_cancel) {
        m_elevated_cancel();
        return;
    }
    if (m_process && m_process->state() != QProcess::NotRunning) {
#ifdef _WIN32
        terminateProcessTree();
#endif
        m_process->kill();
    }
}

void ExecutionBroker::onReadyReadStdout() {
    if (!m_process) {
        return;
    }
    const QString chunk = QString::fromUtf8(m_process->readAllStandardOutput());
    if (chunk.isEmpty()) {
        return;
    }
    appendCapped(m_stdout_buffer, chunk);
    // Never hand a subscriber more than the caller's own cap in a single chunk: across a
    // queued connection an uncapped payload accumulates in the receiver's event queue.
    Q_EMIT stdoutChunk(m_command_id, cappedHeadTail(chunk, m_max_output_bytes));
}

void ExecutionBroker::onReadyReadStderr() {
    if (!m_process) {
        return;
    }
    const QString chunk = QString::fromUtf8(m_process->readAllStandardError());
    if (chunk.isEmpty()) {
        return;
    }
    appendCapped(m_stderr_buffer, chunk);
    Q_EMIT stderrChunk(m_command_id, cappedHeadTail(chunk, m_max_output_bytes));
}

/// @brief Copy the retained buffers into @p result under the caller's cap, and record whether
/// what lands there is the whole run: bytes the rolling window already dropped count as
/// truncation just as much as the cap applied here.
void ExecutionBroker::fillCappedOutput(AiCommandResult& result) {
    const int half_cap = std::max(m_max_output_bytes / 2, kMinOutputCap);
    bool stdout_truncated = false;
    bool stderr_truncated = false;
    result.stdout_text = cappedHeadTail(m_stdout_buffer, half_cap, &stdout_truncated);
    result.stderr_text = cappedHeadTail(m_stderr_buffer, half_cap, &stderr_truncated);
    result.output_truncated = stdout_truncated || stderr_truncated || m_output_dropped_bytes > 0;
}

void ExecutionBroker::onProcessFinished(int exit_code, int exit_status) {
    if (!m_process) {
        return;
    }
    onReadyReadStdout();
    onReadyReadStderr();

    AiCommandResult result;
    result.started = true;
    result.cancelled = m_cancel_requested;
    result.exit_code = exit_code;
    result.exit_status = exit_status;
    result.duration_ms = m_timer.elapsed();
    fillCappedOutput(result);
    if (m_cancel_requested) {
        result.error_message = QStringLiteral("Command cancelled");
    }
    completeWith(std::move(result));
}

void ExecutionBroker::onProcessError(int error) {
    if (!m_process || m_finished_emitted) {
        return;
    }
    if (static_cast<QProcess::ProcessError>(error) == QProcess::FailedToStart) {
        AiCommandResult result;
        result.error_message =
            QStringLiteral("Process start error: %1").arg(m_process->errorString());
        completeWith(std::move(result));
    }
}

/// @brief Settle an expired run as a timeout: reap the tree, drain what the child managed to
/// write, and report that partial output rather than an exit code the run never produced.
///
/// The cancel flag is cleared first so a timeout is never reported as a user cancellation.
void ExecutionBroker::completeAsTimedOut() {
    m_cancel_requested = false;
    if (m_process) {
#ifdef _WIN32
        terminateProcessTree();
#endif
        m_process->kill();
    }
    onReadyReadStdout();
    onReadyReadStderr();
    AiCommandResult result;
    result.started = true;
    result.timed_out = true;
    result.exit_code = -1;
    result.duration_ms = m_timer.elapsed();
    fillCappedOutput(result);
    result.error_message = QStringLiteral("Command timed out");
    completeWith(std::move(result));
}

void ExecutionBroker::onTimeoutTick() {
    if (!m_running || m_finished_emitted || !m_process) {
        return;
    }
    if (m_process->state() == QProcess::NotRunning) {
        return;
    }
    if (m_timeout_ms > 0 && m_timer.elapsed() >= m_timeout_ms) {
        completeAsTimedOut();
        return;
    }
    QTimer::singleShot(kTimeoutTickMs, this, &ExecutionBroker::onTimeoutTick);
}

void ExecutionBroker::completeWith(AiCommandResult result) {
    if (m_finished_emitted) {
        return;
    }
    m_finished_emitted = true;
    m_running = false;
    const QString id = m_command_id;
    if (m_process) {
        m_process->disconnect(this);
        if (m_process->state() != QProcess::NotRunning) {
#ifdef _WIN32
            terminateProcessTree();
#endif
            m_process->kill();
        }
        m_process.reset();
    }
#ifdef _WIN32
    closeJobHandle();
#endif
    Q_EMIT finished(id, result);
}

void ExecutionBroker::emitDeferredFinish(AiCommandResult result) {
    const QString id = m_command_id;
    const quint64 generation = m_request_generation;
    QTimer::singleShot(0, this, [this, id, generation, result]() {
        if (generation != m_request_generation) {
            // A newer request owns the broker state. Report this failure, but do not clear
            // m_running / latch m_finished_emitted: that would strand the running process
            // with no timeout and suppress its real completion.
            Q_EMIT finished(id, result);
            return;
        }
        if (m_finished_emitted) {
            return;
        }
        m_finished_emitted = true;
        m_running = false;
        Q_EMIT finished(id, result);
    });
}

void ExecutionBroker::emitDeferredStandaloneFinish(const QString& command_id,
                                                   AiCommandResult result) {
    QTimer::singleShot(0, this, [this, command_id, result]() {
        Q_EMIT this->finished(command_id, result);
    });
}

void ExecutionBroker::appendCapped(QString& target, const QString& chunk) {
    target.append(chunk);
    const QByteArray bytes = target.toUtf8();
    if (bytes.size() > m_max_output_bytes) {
        // Record what the rolling window drops so the result can say the output is partial
        // instead of presenting the retained tail as the whole run.
        m_output_dropped_bytes += static_cast<qint64>(bytes.size()) - m_max_output_bytes;
        target = QString::fromUtf8(bytes.right(m_max_output_bytes));
    }
}

AiCommandRequest ExecutionBroker::requestFromJson(const QJsonObject& args) {
    // Tool arguments are untrusted: a missing, wrong-typed or out-of-domain field is
    // recorded as a rejection the entry points refuse, never repaired into a default.
    AiCommandRequest request;
    QString error;
    if (!parseRequiredString(args, QStringLiteral("command"), request.command, error) ||
        !parseOptionalTimeoutSeconds(args, request.timeout_seconds, error) ||
        !parseOptionalRequiresAdmin(args, request.requires_admin, error)) {
        request.validation_error = error;
    }
    return request;
}

AiCommandRequest ExecutionBroker::processRequestFromJson(const QJsonObject& args) {
    AiCommandRequest request;
    QString error;
    if (!parseRequiredString(args, QStringLiteral("program"), request.program, error)) {
        request.validation_error = error;
        return request;
    }
    const QString arguments_error = parseOptionalArguments(args, request.arguments);
    if (!arguments_error.isEmpty()) {
        request.validation_error = arguments_error;
        return request;
    }
    if (!parseOptionalTimeoutSeconds(args, request.timeout_seconds, error) ||
        !parseOptionalRequiresAdmin(args, request.requires_admin, error)) {
        request.validation_error = error;
    }
    return request;
}

}  // namespace sak::ai

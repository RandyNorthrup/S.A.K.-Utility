// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_execution_broker.h"

#include "sak/ai/ai_credential_store.h"
#include "sak/contained_process_quarantine.h"
#include "sak/elevation_manager.h"
#include "sak/logger.h"
#include "sak/process_runner.h"
#include "sak/windows_process_job.h"

#include <QDeadlineTimer>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <utility>

namespace sak::ai {

namespace {

// The domain now lives in the header (kAiCommandMinTimeoutSeconds /
// kAiCommandMaxTimeoutSeconds) so the planner cannot promise a timeout this clamp would
// silently cut. Aliased here to keep the call sites below reading naturally.
constexpr int kMinTimeoutSeconds = kAiCommandMinTimeoutSeconds;
constexpr int kMaxTimeoutSeconds = kAiCommandMaxTimeoutSeconds;
constexpr int kMinOutputCap = 1024;
// Hard ceiling on what one run may make the broker retain per stream. Without it a caller
// (or a tool argument that reached one) could ask for a multi-gigabyte buffer. The value now
// lives in the header (kAiCommandOutputBytesCeiling) so a caller's own output budget cannot
// drift above the limit this file refuses at; aliased here to keep the call sites reading
// naturally, exactly as the timeout domain above is.
constexpr int kMaxOutputCap = kAiCommandOutputBytesCeiling;
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
    // The tool schema says INTEGER. Accepting 5.9 and truncating it to 5 makes the approved
    // request differ from the execution, while converting NaN to int has undefined behaviour.
    if (!std::isfinite(raw) || std::trunc(raw) != raw) {
        error = QStringLiteral("timeout_seconds must be an integer");
        return false;
    }
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
    // AN EMBEDDED NUL IS NOT A CHARACTER THE LAUNCH CAN CARRY, so a request containing one is
    // refused rather than passed along. QProcess hands the program path and the assembled
    // command line to CreateProcessW as C strings, which END AT THE FIRST NUL -- the launch is
    // silently TRUNCATED there, while the approval preview is built from the whole QString and
    // renders the NUL as a visible escape with the remainder after it.
    //
    // The disagreement runs in the dangerous direction. "Remove-Item C:/temp<NUL> -WhatIf" is
    // read and approved as a dry run and executes as a real delete: the reviewer's eye is drawn
    // to the trailing flag, and the trailing flag is exactly the part that never reaches the
    // process. Escaping it for DISPLAY is not a fix -- it makes the preview honest about a
    // character whose presence still changes what runs. No legitimate command line, program
    // path or argument contains a NUL, so the request itself fails closed.
    if (out.contains(QChar::Null)) {
        error = QStringLiteral("%1 must not contain an embedded NUL character").arg(key);
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
        // The same truncation-at-launch mismatch parseRequiredString rejects above, and argv is
        // where it bites hardest: each element is passed to CreateProcessW inside a C string, so
        // the approver reads one argument and the process receives the shorter prefix of it.
        if (argument.contains(QChar::Null)) {
            return QStringLiteral("an argument must not contain an embedded NUL character");
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

ExecutionBroker::ExecutionBroker(QObject* parent) : QObject(parent), m_deadline_timer(this) {
    // One single-shot timer armed for the run's remaining time when the child starts, and
    // stopped when the run settles -- not a 200 ms poll that wakes the GUI thread five
    // times a second for the whole life of every command.
    m_deadline_timer.setObjectName(QStringLiteral("commandDeadlineTimer"));
    m_deadline_timer.setSingleShot(true);
    m_deadline_timer.setTimerType(Qt::CoarseTimer);
    connect(&m_deadline_timer, &QTimer::timeout, this, &ExecutionBroker::onDeadlineReached);
}

namespace {

// How often SettlingJob checks whether the members a finished command left behind have exited
// after their Job was told to terminate them (bounded by kTimeoutProcessTerminateMs).
constexpr int kJobSettlePollMs = 25;

#ifdef SAK_AI_TEST_HOOKS
std::function<std::expected<std::unique_ptr<sak::WindowsProcessJob>, QString>()>
    g_job_factory_for_testing;
#endif  // SAK_AI_TEST_HOOKS

std::expected<std::unique_ptr<sak::WindowsProcessJob>, QString> createCommandJob() {
#ifdef SAK_AI_TEST_HOOKS
    if (g_job_factory_for_testing) {
        return g_job_factory_for_testing();
    }
#endif  // SAK_AI_TEST_HOOKS
    return sak::WindowsProcessJob::create();
}

// Custody adapter: a Job whose members could not be proved gone. A retry terminates what is left
// and closes the Job only once it is empty; until then the open handle keeps kill-on-close armed,
// so nothing in it can outlive the application.
class RetainedCommandJob final : public sak::ContainedProcessQuarantine::Owner {
public:
    explicit RetainedCommandJob(std::unique_ptr<sak::WindowsProcessJob> job)
        : m_job(std::move(job)) {}

    [[nodiscard]] std::expected<void, QString> retryCleanup() override {
        const auto accounting = m_job->accounting();
        if (!accounting) {
            return std::unexpected(accounting.error());
        }
        if (accounting->active_processes != 0) {
            const auto terminated = m_job->terminate();
            return std::unexpected(
                terminated ? QStringLiteral("AI command Job still has %1 active processes")
                                 .arg(accounting->active_processes)
                           : terminated.error());
        }
        return m_job->closeEmpty();
    }

private:
    std::unique_ptr<sak::WindowsProcessJob> m_job;
};

void quarantineCommandJob(std::unique_ptr<sak::WindowsProcessJob> job, const QString& reason) {
    sak::ContainedProcessQuarantine::retain(
        std::make_unique<RetainedCommandJob>(std::move(job)),
        QStringLiteral("AI command Job cleanup could not be proved: %1").arg(reason));
}

// The members a finished command left running have been told to terminate; this proves they are
// gone without blocking the GUI thread the broker lives on -- the broker is event-driven, so its
// wait is too. Once the Job is empty it is closed; a failed query, a refused close or members
// still alive at the deadline send it to quarantine instead. A child of the broker, so a broker
// destroyed mid-settle hands the Job to quarantine through the destructor.
class SettlingJob final : public QObject {
public:
    SettlingJob(std::unique_ptr<sak::WindowsProcessJob> job, QObject* parent)
        : QObject(parent)
        , m_job(std::move(job))
        , m_deadline(sak::kTimeoutProcessTerminateMs, Qt::CoarseTimer) {
        setObjectName(QStringLiteral("aiCommandJobSettle"));
        m_poll.setInterval(kJobSettlePollMs);
        connect(&m_poll, &QTimer::timeout, this, &SettlingJob::poll);
        m_poll.start();
    }

    SettlingJob(const SettlingJob&) = delete;
    SettlingJob& operator=(const SettlingJob&) = delete;
    SettlingJob(SettlingJob&&) = delete;
    SettlingJob& operator=(SettlingJob&&) = delete;

    ~SettlingJob() override {
        if (m_job) {
            quarantineCommandJob(std::move(m_job),
                                 QStringLiteral("its broker was destroyed while the members it "
                                                "had terminated were still exiting"));
        }
    }

private:
    void poll() {
        const auto accounting = m_job->accounting();
        if (!accounting) {
            settle(accounting.error());
            return;
        }
        if (accounting->active_processes == 0) {
            const auto closed = m_job->closeEmpty();
            settle(closed ? QString() : closed.error());
            return;
        }
        if (m_deadline.hasExpired()) {
            settle(QStringLiteral("%1 processes were still active %2 ms after termination")
                       .arg(accounting->active_processes)
                       .arg(sak::kTimeoutProcessTerminateMs));
        }
    }

    void settle(const QString& failure) {
        m_poll.stop();
        if (failure.isEmpty()) {
            m_job.reset();
        } else {
            quarantineCommandJob(std::move(m_job), failure);
        }
        deleteLater();
    }

    std::unique_ptr<sak::WindowsProcessJob> m_job;
    QDeadlineTimer m_deadline;
    QTimer m_poll;
};

}  // namespace

#ifdef SAK_AI_TEST_HOOKS
void ExecutionBroker::setJobFactoryForTesting(JobFactory factory) {
    g_job_factory_for_testing = std::move(factory);
}
#endif  // SAK_AI_TEST_HOOKS

ExecutionBroker::~ExecutionBroker() {
    if (m_process) {
        m_process->disconnect(this);
        if (m_process->state() != QProcess::NotRunning) {
            stopContainedProcess();
        }
        m_process.reset();
    }
    if (!m_job) {
        return;
    }
    // No event loop to settle on from here: an empty Job is closed, anything else is terminated
    // and handed to custody, whose open handle keeps kill-on-close armed until the app exits.
    const auto accounting = m_job->accounting();
    if (accounting && accounting->active_processes == 0) {
        if (const auto closed = m_job->closeEmpty(); closed) {
            m_job.reset();
            return;
        }
    }
    if (m_job) {
        static_cast<void>(m_job->terminate());
        quarantineCommandJob(std::move(m_job),
                             QStringLiteral("the broker was destroyed with the command's "
                                            "processes still active"));
    }
}

void ExecutionBroker::stopContainedProcess() {
    if (m_job) {
        if (const auto terminated = m_job->terminate(); !terminated) {
            // The primary is still killed below, and the Job's kill-on-close covers the rest
            // when it is released or quarantined; the refusal itself is reported.
            sak::logError("Could not terminate the AI command's Job: {}",
                          terminated.error().toStdString());
        }
    }
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
    }
}

void ExecutionBroker::releaseJob() {
    if (!m_job) {
        return;
    }
    std::unique_ptr<sak::WindowsProcessJob> job = std::move(m_job);
    const auto accounting = job->accounting();
    if (!accounting) {
        quarantineCommandJob(std::move(job), accounting.error());
        return;
    }
    if (accounting->active_processes == 0) {
        if (const auto closed = job->closeEmpty(); !closed) {
            quarantineCommandJob(std::move(job), closed.error());
        }
        return;
    }
    // The command finished but left processes running (Start-Process, a detached installer).
    // They were started by the command and end with it, as the Job's kill-on-close always made
    // them do; terminate them now and prove it without blocking the GUI thread.
    if (const auto terminated = job->terminate(); !terminated) {
        quarantineCommandJob(std::move(job), terminated.error());
        return;
    }
    new SettlingJob(std::move(job), this);
}

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
    // Not redundant with the earlier self.isNull(): m_elevated_runner pumps the
    // event loop (UI update + Stop routing), which can delete this broker while
    // the elevated command runs. Re-checking here is the only guard against a
    // use-after-free on the lines below.
    // cppcheck-suppress identicalConditionAfterEarlyExit
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

namespace {

constexpr qint64 kQuotedCharExpansion = 2;
constexpr qint64 kQuoteDelimiterChars = 2;

// Character count after Windows argv quoting. QProcess ultimately has to place program + argv
// in CreateProcessW's one command-line buffer, whose hard ceiling is why kMaxCommandChars exists.
// Counting the encoded form catches many individually legal arguments whose aggregate cannot run.
[[nodiscard]] qint64 quotedWindowsArgumentChars(const QString& value) {
    const bool needs_quotes = value.isEmpty() || std::ranges::any_of(value, [](QChar ch) {
                                  return ch.isSpace() || ch == QLatin1Char('"');
                              });
    if (!needs_quotes) {
        return value.size();
    }
    qint64 encoded = kQuoteDelimiterChars;
    qint64 backslashes = 0;
    for (const QChar ch : value) {
        if (ch == QLatin1Char('\\')) {
            ++backslashes;
            continue;
        }
        if (ch == QLatin1Char('"')) {
            encoded += backslashes * kQuotedCharExpansion + kQuotedCharExpansion;
            backslashes = 0;
            continue;
        }
        encoded += backslashes + 1;
        backslashes = 0;
    }
    return encoded + backslashes * kQuotedCharExpansion;
}

[[nodiscard]] QString processCommandLineLengthError(AiCommandTarget target,
                                                    const AiCommandRequest& request) {
    if (target != AiCommandTarget::Process) {
        return {};
    }
    qint64 encoded = quotedWindowsArgumentChars(request.program);
    for (const QString& argument : request.arguments) {
        encoded += 1 + quotedWindowsArgumentChars(argument);
        if (encoded > kMaxCommandChars) {
            return QStringLiteral("Process command line exceeds the %1 character limit")
                .arg(kMaxCommandChars);
        }
    }
    return {};
}

/// @brief "Is there anything to run at all" -- the per-target emptiness rule.
///
/// Split from the target-specific messages so each entry point's refusal text stays exactly
/// what it always was: a shared check that homogenised the wording would make a PowerShell
/// refusal indistinguishable from a cmd one in a log.
[[nodiscard]] QString emptyRequestError(AiCommandTarget target, const AiCommandRequest& request) {
    if (target == AiCommandTarget::Process) {
        return request.program.trimmed().isEmpty() ? QStringLiteral("Program path is empty")
                                                   : QString{};
    }
    if (!request.command.trimmed().isEmpty()) {
        return {};
    }
    return target == AiCommandTarget::PowerShell ? QStringLiteral("PowerShell command is empty")
                                                 : QStringLiteral("CMD command is empty");
}

/// @brief Elevation support is per-target: only the PowerShell entry point routes through the
/// injected elevated runner, so an admin request on the other two is refused outright.
[[nodiscard]] QString elevationUnsupportedError(AiCommandTarget target,
                                                const AiCommandRequest& request) {
    if (!request.requires_admin) {
        return {};
    }
    if (target == AiCommandTarget::Cmd) {
        return QStringLiteral(
            "Elevated cmd.exe launch is not supported; use run_powershell for admin tasks.");
    }
    if (target == AiCommandTarget::Process) {
        return QStringLiteral(
            "Elevated direct-process launch is not supported; use run_powershell for admin tasks.");
    }
    return {};  // PowerShell elevates through the injected runner rather than being refused.
}

}  // namespace

namespace {

/// @brief Resolve @p program in place to an absolute path, or return the refusal message.
///
/// THE RELATIVE-PATH ARM IS THE POINT. The old guard resolved only names with NO separator,
/// so "sub/planted.exe" skipped resolution entirely and went to QProcess::start, which resolves
/// it against the WORKING DIRECTORY -- precisely the hijack the resolution exists to prevent.
/// The planner already refuses such a path (ai_command_tool_planner.cpp), so production never
/// reached it; but startProcess is public API, nothing else enforced the rule, and a guard that
/// depends on every caller having checked first is not a guard. The two layers now agree.
[[nodiscard]] QString resolveLaunchProgram(QString& program) {
    const bool has_separator = program.contains(QLatin1Char('/')) ||
                               program.contains(QLatin1Char('\\'));
    if (!has_separator) {
        // A bare name is resolved by CreateProcess against the current directory before PATH,
        // so resolve it here (System32, then absolute PATH entries) or refuse the run. Shared
        // with the plan-time resolver (sak::resolveBareExecutable) so the binary named in the
        // approval preview and the binary launched here can never be decided by two drifting
        // copies of the same rule.
        const QString resolved = sak::resolveBareExecutable(program);
        if (resolved.isEmpty()) {
            return QStringLiteral(
                       "Cannot resolve program '%1' to an absolute path; refusing to launch a "
                       "bare name")
                .arg(program);
        }
        program = resolved;
        return {};
    }
    if (!QDir::isAbsolutePath(program)) {
        return QStringLiteral(
                   "program must be an absolute path or a bare executable name, not a "
                   "working-directory-relative path: %1")
            .arg(program);
    }
    return {};
}

}  // namespace

QString aiCommandPreconditionError(AiCommandTarget target, const AiCommandRequest& request) {
    const QString empty_error = emptyRequestError(target, request);
    if (!empty_error.isEmpty()) {
        return empty_error;
    }
    const QString elevation_error = elevationUnsupportedError(target, request);
    if (!elevation_error.isEmpty()) {
        return elevation_error;
    }
    // The SAME domain launchLimitsError enforces at the launch point, phrased identically on
    // purpose: a refusal raised at plan time and the one raised at the door must be the same
    // sentence, or a reader comparing a plan against a log has to work out whether two
    // differently-worded messages describe one rule or two.
    if (request.max_output_bytes <= 0 || request.max_output_bytes > kMaxOutputCap) {
        return QStringLiteral("max_output_bytes %1 is outside the range 1-%2")
            .arg(request.max_output_bytes)
            .arg(kMaxOutputCap);
    }
    return processCommandLineLengthError(target, request);
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
    const QString precondition_error = aiCommandPreconditionError(AiCommandTarget::PowerShell,
                                                                  request);
    if (!precondition_error.isEmpty()) {
        AiCommandResult fail;
        fail.error_message = precondition_error;
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
    return launchProcess({.program = powershell,
                          .arguments = args,
                          .timeout_seconds = request.timeout_seconds,
                          .max_output_bytes = request.max_output_bytes,
                          .command_id = command_id,
                          .already_running_check = false});
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
    const QString precondition_error = aiCommandPreconditionError(AiCommandTarget::Cmd, request);
    if (!precondition_error.isEmpty()) {
        AiCommandResult fail;
        fail.error_message = precondition_error;
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
    return launchProcess({.program = shell,
                          .arguments = args,
                          .timeout_seconds = request.timeout_seconds,
                          .max_output_bytes = request.max_output_bytes,
                          .command_id = command_id,
                          .already_running_check = false});
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
    const QString precondition_error = aiCommandPreconditionError(AiCommandTarget::Process,
                                                                  request);
    if (!precondition_error.isEmpty()) {
        AiCommandResult fail;
        fail.error_message = precondition_error;
        emitDeferredFinish(std::move(fail));
        return false;
    }
    QString program = request.program.trimmed();
    const QString program_error = resolveLaunchProgram(program);
    if (!program_error.isEmpty()) {
        AiCommandResult fail;
        fail.error_message = program_error;
        emitDeferredFinish(std::move(fail));
        return false;
    }

    // THE LAST POSSIBLE MOMENT TO CHECK. A plan carries the identity of the file its program
    // named; re-taking it here and comparing proves the launch is about to run the binary the
    // human approved rather than something that has since taken its name.
    //
    // THE RESIDUAL GAP IS STATED RATHER THAN HIDDEN: this closes the window from human scale
    // (an approval dialog, a UAC prompt, restore-point creation -- minutes) to the microseconds
    // between this comparison and CreateProcessW re-walking the path. That remainder cannot be
    // removed here. Closing it needs a launch from the pinned handle itself, and QProcess::start
    // accepts only a path -- there is no handle overload, and the Win32 route (an image section
    // plus NtCreateUserProcess) would replace this broker's whole process lifecycle.
    if (request.program_identity.valid) {
        const sak::ExecutableIdentity now = sak::executableIdentity(program);
        if (!sak::sameExecutable(request.program_identity, now)) {
            AiCommandResult fail;
            fail.error_message =
                QStringLiteral(
                    "Program '%1' is not the file that was approved; it was replaced, "
                    "moved, or became unreadable between approval and launch")
                    .arg(program);
            emitDeferredFinish(std::move(fail));
            return false;
        }
    }

    return launchProcess({.program = program,
                          .arguments = request.arguments,
                          .timeout_seconds = request.timeout_seconds,
                          .max_output_bytes = request.max_output_bytes,
                          .command_id = command_id,
                          .already_running_check = false});
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
    // No `started` or `errorOccurred` receiver: on Windows QProcess::start() creates the process
    // synchronously and emits both from inside start(), BEFORE the Job launch can be verified.
    // launchProcess() reads the outcome after start() returns and announces only a launch it has
    // proved contained.
}

void ExecutionBroker::announceLaunch() {
    // A directly connected receiver may destroy this broker from started().
    const QPointer<ExecutionBroker> self(this);
    Q_EMIT started(m_command_id);
    if (self.isNull() || !m_running) {
        return;
    }
    if (m_timeout_ms > 0) {
        // The clock started at launch, so the deadline is what is LEFT of the timeout.
        m_deadline_timer.start(static_cast<int>(
            std::max<qint64>(0, static_cast<qint64>(m_timeout_ms) - m_timer.elapsed())));
    }
}

void ExecutionBroker::failLaunch(AiCommandResult result) {
    if (m_process) {
        m_process->disconnect(this);
        stopContainedProcess();
        m_process.reset();
    }
    releaseJob();
    emitDeferredFinish(std::move(result));
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

    if (!prepareContainedProcess()) {
        return false;
    }
    return startContainedProcess(request);
}

bool ExecutionBroker::prepareContainedProcess() {
    // CONTAINED FROM CREATION. The process used to be assigned to its kill-on-close Job from the
    // `started` slot, i.e. AFTER CreateProcess, so anything it spawned before that ran outside
    // the Job; and when CreateJobObject, OpenProcess or AssignProcessToJobObject failed, the Job
    // was dropped and the command kept running uncontained, with a PID-snapshot walk as its only
    // cleanup. The Job now rides in as a PROC_THREAD_ATTRIBUTE_JOB_LIST launch attribute, so the
    // child is a member before its first instruction, and a launch without one is refused.
    auto job = createCommandJob();
    if (!job) {
        AiCommandResult fail;
        fail.error_message = QStringLiteral("Cannot contain the command in a kill-on-close Job: %1")
                                 .arg(job.error());
        emitDeferredFinish(std::move(fail));
        return false;
    }
    m_job = std::move(*job);
    m_process = std::make_unique<QProcess>(this);
    connectProcessSignals();
    if (const auto installed = m_job->installOn(*m_process); !installed) {
        AiCommandResult fail;
        fail.error_message = QStringLiteral("Cannot contain the command in a kill-on-close Job: %1")
                                 .arg(installed.error());
        failLaunch(std::move(fail));
        return false;
    }
    return true;
}

bool ExecutionBroker::startContainedProcess(const ProcessLaunchRequest& request) {
    m_running = true;
    m_timer.start();
    m_process->start(request.program, request.arguments);
    const bool started = m_process->waitForStarted(sak::kTimeoutProcessStartMs);
    if (const auto completed = m_job->completeLaunch(*m_process, started); !completed) {
        // Started but not provably inside the Job (or the launch state is inconsistent): stop it
        // and refuse, rather than run a command nothing could reap.
        AiCommandResult fail;
        fail.error_message = QStringLiteral(
                                 "The command was not contained in its kill-on-close Job and was "
                                 "stopped: %1")
                                 .arg(completed.error());
        failLaunch(std::move(fail));
        return false;
    }
    if (!started) {
        // Reported after the event loop turns, like every other refusal, so the caller's single
        // completion path sees it after start* has returned.
        AiCommandResult fail;
        fail.error_message =
            QStringLiteral("Process start error: %1").arg(m_process->errorString());
        failLaunch(std::move(fail));
        return true;
    }
    announceLaunch();
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
        stopContainedProcess();
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
    // This is the PLAIN launch path, and it reported elevated=false unconditionally. That is
    // the model's claim, not what happened: QProcess inherits this process's token, so inside
    // an elevated S.A.K. the command ran WITH administrator rights and the record said it did
    // not. Report the fact, so the transcript, the trace and anything auditing them agree with
    // the privilege the command actually had (B1-3 / H6).
    result.elevated = ElevationManager::isElevated();
    result.exit_code = exit_code;
    result.exit_status = exit_status;
    result.duration_ms = m_timer.elapsed();
    fillCappedOutput(result);
    if (m_cancel_requested) {
        result.error_message = QStringLiteral("Command cancelled");
    }
    completeWith(std::move(result));
}

/// @brief Settle an expired run as a timeout: reap the tree, drain what the child managed to
/// write, and report that partial output rather than an exit code the run never produced.
///
/// The cancel flag is cleared first so a timeout is never reported as a user cancellation.
void ExecutionBroker::completeAsTimedOut() {
    m_cancel_requested = false;
    if (m_process) {
        stopContainedProcess();
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

void ExecutionBroker::onDeadlineReached() {
    if (!m_running || m_finished_emitted || !m_process) {
        return;
    }
    if (m_process->state() == QProcess::NotRunning) {
        return;
    }
    completeAsTimedOut();
}

void ExecutionBroker::completeWith(AiCommandResult result) {
    if (m_finished_emitted) {
        return;
    }
    m_finished_emitted = true;
    m_running = false;
    m_deadline_timer.stop();
    const QString id = m_command_id;
    if (m_process) {
        m_process->disconnect(this);
        if (m_process->state() != QProcess::NotRunning) {
            stopContainedProcess();
        }
        m_process.reset();
    }
    releaseJob();
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

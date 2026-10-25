// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/process_runner.h"

#include "sak/contained_process_quarantine.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/process_runner_containment.h"
#include "sak/windows_process_job.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

#include <windows.h>

namespace sak {

namespace {

// Test-only process fault injector (R5-G14-17). Null in production -- never installed -- so
// the launchers pay only a single predicate check and behave identically. Armed by a test via
// ScopedProcessFaultInjector to substitute a chosen ProcessResult for a real launch.
ProcessFaultInjector g_process_fault_injector;

std::optional<bool> injectedDetachedLaunchResult(const QString& program, const QStringList& args) {
    if (!g_process_fault_injector) {
        return std::nullopt;
    }
    const auto injected = g_process_fault_injector(program, args);
    if (!injected.has_value()) {
        return std::nullopt;
    }
    return injected->completedSuccessfully();
}

// Hard per-stream ceiling on retained output. A runaway child (e.g. a script
// that prints gigabytes) previously grew std_out/std_err without bound until the
// process exhausted memory; caller-side caps were applied only AFTER the full
// output had already been accumulated. This bounds accumulation itself. It sits
// well above the largest caller cap (~64 MiB) so it never clips legitimate
// captured output, only pathological floods.
constexpr qsizetype kMaxAccumulatedOutputChars = static_cast<qsizetype>(128) * 1024 * 1024;

// Win32 splits file index, file size and FILETIME into a high and a low 32-bit half; this is
// the shift that recombines them.
constexpr int kFileIndexHighShift = 32;

struct ProcessRunRequest {
    QString program;
    QStringList args;
    int timeout_ms{0};
    const QProcessEnvironment* environment{nullptr};
    CancelCheck should_cancel;
    ProcessOutputCallback on_output;
    ProcessStartedCallback on_started;
};

void appendOutput(ProcessResult* result,
                  const ProcessOutputCallback& on_output,
                  const QByteArray& bytes,
                  bool stderr_stream) {
    if (bytes.isEmpty()) {
        return;
    }
    const QString chunk = QString::fromLocal8Bit(bytes);
    // Bound the RETAINED buffer only; the live stream below still gets every
    // chunk in full so streaming consumers are never cut off.
    QString& target = stderr_stream ? result->std_err : result->std_out;
    if (appendCappedOutput(target, chunk, kMaxAccumulatedOutputChars)) {
        result->output_truncated = true;
    }
    if (on_output) {
        on_output(chunk, stderr_stream);
    }
}

void drainProcessOutput(QProcess* proc,
                        ProcessResult* result,
                        const ProcessOutputCallback& on_output) {
    appendOutput(result, on_output, proc->readAllStandardOutput(), false);
    appendOutput(result, on_output, proc->readAllStandardError(), true);
}

constexpr qint64 kProcessJobPollMs = 25;
constexpr int kNormalDescendantGraceMs = 1000;

void appendProcessFailure(ProcessResult* result, const QString& detail) {
    if (!result->std_err.isEmpty()) {
        result->std_err += QLatin1Char('\n');
    }
    result->std_err += detail;
    result->exit_code = -1;
}

std::expected<bool, QString> waitForEmptyJob(const WindowsProcessJob& job,
                                             const QDeadlineTimer& deadline) {
    for (;;) {
        const auto observed = job.accounting();
        if (!observed) {
            return std::unexpected(observed.error());
        }
        if (observed->active_processes == 0) {
            return true;
        }
        const qint64 remaining = deadline.remainingTime();
        if (remaining <= 0) {
            return false;
        }
        QThread::msleep(static_cast<unsigned long>(std::min(remaining, kProcessJobPollMs)));
    }
}

std::expected<bool, QString> settleContainedDescendants(WindowsProcessJob& job,
                                                        bool terminate_now) {
    bool forced_descendants = terminate_now;
    if (!terminate_now) {
        const QDeadlineTimer grace(kNormalDescendantGraceMs, Qt::PreciseTimer);
        const auto settled = waitForEmptyJob(job, grace);
        if (!settled) {
            return std::unexpected(settled.error());
        }
        forced_descendants = !*settled;
    }
    if (forced_descendants) {
        const auto active = job.accounting();
        if (!active) {
            return std::unexpected(active.error());
        }
        if (active->active_processes != 0) {
            const auto terminated = job.terminate();
            if (!terminated) {
                return std::unexpected(terminated.error());
            }
        }
    }
    return !terminate_now && forced_descendants;
}

std::expected<void, QString> waitForPrimaryExit(QProcess* proc, const QDeadlineTimer& deadline) {
    if (proc->state() != QProcess::NotRunning) {
        const qint64 remaining = deadline.remainingTime();
        if (remaining <= 0 || !proc->waitForFinished(static_cast<int>(
                                  std::min<qint64>(remaining, std::numeric_limits<int>::max())))) {
            return std::unexpected(
                QStringLiteral("Contained process primary did not finish before cleanup deadline"));
        }
    }
    return {};
}

std::expected<void, QString> closeEmptiedJob(WindowsProcessJob& job,
                                             const QDeadlineTimer& deadline) {
    const auto empty = waitForEmptyJob(job, deadline);
    if (!empty) {
        return std::unexpected(empty.error());
    }
    if (!*empty) {
        return std::unexpected(
            QStringLiteral("Process Job still has active members at cleanup deadline"));
    }
    return job.closeEmpty();
}

std::expected<bool, QString> finishContainedProcess(QProcess* proc,
                                                    WindowsProcessJob& job,
                                                    bool terminate_now) {
    const QDeadlineTimer deadline(sak::kTimeoutProcessTerminateMs, Qt::PreciseTimer);
    const auto forced_descendants = settleContainedDescendants(job, terminate_now);
    if (!forced_descendants) {
        return std::unexpected(forced_descendants.error());
    }
    const auto primary = waitForPrimaryExit(proc, deadline);
    if (!primary) {
        return std::unexpected(primary.error());
    }
    const auto closed = closeEmptiedJob(job, deadline);
    if (!closed) {
        return std::unexpected(closed.error());
    }
    return *forced_descendants;
}

// Custody adapter for a run whose cleanup could not be proved. A retry needs only the Job: an
// empty Job means the primary has exited too, so the QProcess is never driven from a retrying
// thread. The QProcess was moved to the application thread when custody took it, and is destroyed
// there, so it is never deleted underneath that thread's event delivery.
class RetainedProcessRun final : public ContainedProcessQuarantine::Owner {
public:
    explicit RetainedProcessRun(ContainedProcessRun run) : m_run(std::move(run)) {}

    RetainedProcessRun(const RetainedProcessRun&) = delete;
    RetainedProcessRun& operator=(const RetainedProcessRun&) = delete;
    RetainedProcessRun(RetainedProcessRun&&) = delete;
    RetainedProcessRun& operator=(RetainedProcessRun&&) = delete;

    ~RetainedProcessRun() override {
        if (!m_run.process) {
            return;
        }
        if (m_run.process->thread() != QThread::currentThread()) {
            m_run.process.release()->deleteLater();
            return;
        }
        // Destroyed only after the Job proved empty, so the primary has exited and this bounded
        // wait returns at once; it lets QProcess record that exit instead of destroying itself as
        // a "still running" process.
        if (m_run.process->state() != QProcess::NotRunning &&
            !m_run.process->waitForFinished(sak::kTimeoutProcessTerminateMs)) {
            logError("ProcessRunner custody released a QProcess that did not report its exit");
        }
    }

    [[nodiscard]] std::expected<void, QString> retryCleanup() override {
        const auto terminated = settleContainedDescendants(*m_run.job, true);
        if (!terminated) {
            return std::unexpected(terminated.error());
        }
        return closeEmptiedJob(*m_run.job,
                               QDeadlineTimer(sak::kTimeoutProcessTerminateMs, Qt::PreciseTimer));
    }

private:
    ContainedProcessRun m_run;
};

void retainUnprovenRun(ContainedProcessRun& run, const QString& error) {
    // QProcess::moveToThread must run on the QProcess's own thread, which is this one.
    const QCoreApplication* const application = QCoreApplication::instance();
    if (run.process && application != nullptr && run.process->thread() != application->thread()) {
        run.process->moveToThread(application->thread());
    }
    ContainedProcessQuarantine::retain(
        std::make_unique<RetainedProcessRun>(std::move(run)),
        QStringLiteral("ProcessRunner could not prove Job cleanup: %1").arg(error));
    run = {};
}

std::expected<void, QString> startProcess(const ProcessRunRequest& request,
                                          QProcess* proc,
                                          WindowsProcessJob& job) {
    if (request.environment != nullptr) {
        proc->setProcessEnvironment(*request.environment);
    }
    const auto installed = job.installOn(*proc);
    if (!installed) {
        return std::unexpected(
            QStringLiteral("Failed to prepare contained process: %1").arg(installed.error()));
    }
    proc->start(request.program, request.args);
    const bool started = proc->waitForStarted(sak::kTimeoutProcessStartMs);
    const auto completed = job.completeLaunch(*proc, started);
    if (!completed) {
        return std::unexpected(
            QStringLiteral("Failed to verify contained process launch: %1").arg(completed.error()));
    }
    if (!started) {
        return std::unexpected(
            QStringLiteral("Failed to start process: %1").arg(proc->errorString()));
    }
    // No caller writes stdin. Immediate EOF also prevents an unattended
    // Chocolatey prompt from waiting on an inherited open pipe.
    proc->closeWriteChannel();
    return {};
}

bool stopForCancelOrTimeout(const ProcessRunRequest& request,
                            ProcessResult* result,
                            const QElapsedTimer& timer) {
    if (request.should_cancel && request.should_cancel()) {
        result->cancelled = true;
        return true;
    }
    if (request.timeout_ms > 0 && timer.elapsed() >= request.timeout_ms) {
        result->timed_out = true;
        result->std_err += QStringLiteral("Process timed out");
        return true;
    }
    return false;
}

bool pollContainedProcess(const ProcessRunRequest& request, QProcess* proc, ProcessResult* result) {
    QElapsedTimer timer;
    timer.start();
    constexpr int kPollMs = 100;
    for (;;) {
        if (proc->waitForFinished(kPollMs) || proc->state() == QProcess::NotRunning) {
            drainProcessOutput(proc, result, request.on_output);
            return false;
        }
        drainProcessOutput(proc, result, request.on_output);
        if (stopForCancelOrTimeout(request, result, timer)) {
            return true;
        }
    }
}

ProcessResult completeStartedRun(const ProcessRunRequest& request,
                                 ContainedProcessRun& run,
                                 ProcessResult result) {
    // Valid only until a settle step hands the run to custody; every use below is gated on that.
    QProcess* const proc = run.process.get();
    bool containment_closed = false;
    try {
        if (request.on_started) {
            request.on_started(proc->processId());
        }
        if (pollContainedProcess(request, proc, &result)) {
            containment_closed = true;
            if (settleContainedProcessRun(run, true, &result).has_value()) {
                drainProcessOutput(proc, &result, request.on_output);
            }
            return result;
        }
        result.exit_code = proc->exitCode();
        result.exit_status = static_cast<int>(proc->exitStatus());
        containment_closed = true;
        const std::optional<bool> forced_descendants =
            settleContainedProcessRun(run, false, &result);
        if (forced_descendants.value_or(false)) {
            appendProcessFailure(&result,
                                 QStringLiteral(
                                     "Contained process descendants remained after primary exit"));
        }
    } catch (const std::exception& error) {
        appendProcessFailure(
            &result,
            QStringLiteral("Process callback failed: %1").arg(QString::fromUtf8(error.what())));
        if (!containment_closed) {
            (void)settleContainedProcessRun(run, true, &result);
        }
    } catch (...) {
        appendProcessFailure(&result,
                             QStringLiteral("Process callback failed with an unknown exception"));
        if (!containment_closed) {
            (void)settleContainedProcessRun(run, true, &result);
        }
    }
    return result;
}

ProcessResult runProcessInternal(const ProcessRunRequest& request) {
    // An installed test injector substitutes one launch; nullopt requests the real path.
    if (g_process_fault_injector) {
        if (std::optional<ProcessResult> injected = g_process_fault_injector(request.program,
                                                                             request.args)) {
            return *injected;
        }
    }

    ProcessResult result;
    // Pre-cancellation must never launch a possibly destructive child.
    if (request.should_cancel && request.should_cancel()) {
        result.cancelled = true;
        return result;
    }

    auto created_job = WindowsProcessJob::create();
    if (!created_job) {
        appendProcessFailure(
            &result,
            QStringLiteral("Failed to prepare contained process: %1").arg(created_job.error()));
        return result;
    }
    // Heap-owned so an unproven cleanup can transfer the live QProcess and its Job to custody
    // instead of destroying them here.
    ContainedProcessRun run{.process = std::make_unique<QProcess>(),
                            .job = std::move(*created_job)};
    const auto started = startProcess(request, run.process.get(), *run.job);
    if (!started) {
        appendProcessFailure(&result, started.error());
        (void)settleContainedProcessRun(run, true, &result);
        return result;
    }
    return completeStartedRun(request, run, std::move(result));
}

}  // namespace

std::optional<bool> settleContainedProcessRun(ContainedProcessRun& run,
                                              bool terminate_now,
                                              ProcessResult* result) {
    Q_ASSERT(run.process && run.job && result != nullptr);
    const auto finished = finishContainedProcess(run.process.get(), *run.job, terminate_now);
    if (finished) {
        return *finished;
    }
    // Report + contain (owner decision 2026-10-24). This was a qFatal, so a child that merely
    // outlived the cleanup deadline under machine load took the whole application down mid-run.
    appendProcessFailure(
        result,
        QStringLiteral("Contained process cleanup could not be proved: %1").arg(finished.error()));
    retainUnprovenRun(run, finished.error());
    return std::nullopt;
}

bool appendCappedOutput(QString& target, const QString& chunk, qsizetype cap) {
    if (chunk.isEmpty()) {
        return false;
    }
    if (cap <= 0 || target.size() >= cap) {
        return true;  // no room: the whole chunk is dropped
    }
    const qsizetype room = cap - target.size();
    if (chunk.size() <= room) {
        target += chunk;
        return false;
    }
    target += chunk.left(room);  // clip to fit exactly at the cap
    return true;
}

ProcessResult runProcess(const QString& program,
                         const QStringList& args,
                         int timeout_ms,
                         const CancelCheck& should_cancel) {
    return runProcessInternal({
        .program = program,
        .args = args,
        .timeout_ms = timeout_ms,
        .should_cancel = should_cancel,
        .on_output = {},
        .on_started = {},
    });
}

ProcessResult runProcessWithEnvironment(const QString& program,
                                        const QStringList& args,
                                        int timeout_ms,
                                        const QProcessEnvironment& environment,
                                        const CancelCheck& should_cancel) {
    return runProcessInternal({
        .program = program,
        .args = args,
        .timeout_ms = timeout_ms,
        .environment = &environment,
        .should_cancel = should_cancel,
        .on_output = {},
        .on_started = {},
    });
}

ProcessResult runPowerShell(const QString& script,
                            int timeout_ms,
                            bool no_profile,
                            bool bypass_policy,
                            const CancelCheck& should_cancel) {
    QStringList args;
    if (no_profile) {
        args << "-NoProfile";
    }
    if (bypass_policy) {
        args << "-ExecutionPolicy" << "Bypass";
    }
    args << "-Command" << script;

    // Launch the System32-qualified interpreter, never a bare "powershell.exe": an
    // elevated caller must not be redirected to a PATH/CWD-planted powershell.
    const QString powershell = systemPowerShellPath();
    if (powershell.isEmpty()) {
        ProcessResult result;
        result.exit_code = -1;
        result.std_err = QStringLiteral(
            "Cannot resolve the System32 PowerShell path; refusing to launch a bare "
            "powershell.exe");
        return result;
    }
    return runProcess(powershell, args, timeout_ms, should_cancel);
}

// Reject anything that is not a bare, System32/Windows-relative name (subdirectories with
// forward slashes -- "WindowsPowerShell/v1.0/powershell.exe" -- are fine). An empty name, a
// ".." traversal, or an already-absolute/rooted path could escape the resolved system
// directory once joined, so refuse it and let the caller fail closed on the empty return.
static bool isContainedRelativeExe(const QString& relativeExe) {
    return !relativeExe.isEmpty() && !relativeExe.contains(QStringLiteral("..")) &&
           !QDir::isAbsolutePath(relativeExe);
}

QString system32Path(const QString& relativeExe) {
    if (!isContainedRelativeExe(relativeExe)) {
        return {};  // fail closed: an un-contained name must not be joined under System32
    }
    wchar_t buffer[MAX_PATH];
    const UINT len = GetSystemDirectoryW(buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};  // fail closed: cannot trust an unresolved system directory
    }
    return QDir::cleanPath(QString::fromWCharArray(buffer, static_cast<int>(len)) +
                           QLatin1Char('/') + relativeExe);
}

QString resolveBareExecutable(const QString& name) {
    const QString system32 = system32Path(name);
    if (!system32.isEmpty() && QFileInfo(system32).isFile()) {
        return system32;
    }
    // The exact-name check above is not enough: System32/whoami does not exist as a file, so an
    // extensionless name fell through to the PATH search below -- which DOES apply PATHEXT and
    // returns the first match in PATH ORDER, letting any earlier PATH directory beat System32.
    // Search System32 with the same suffix rules before considering PATH at all.
    if (!system32.isEmpty()) {
        const QString system32_dir = QFileInfo(system32).absolutePath();
        const QString suffixed = QStandardPaths::findExecutable(name, {system32_dir});
        if (!suffixed.isEmpty()) {
            return suffixed;
        }
    }
    QStringList search;
    const QStringList entries = qEnvironmentVariable("PATH").split(QDir::listSeparator(),
                                                                   Qt::SkipEmptyParts);
    for (const QString& entry : entries) {
        const QString trimmed = entry.trimmed();
        // "." and any relative entry resolve against the working directory -- exactly the hijack
        // this function exists to prevent -- so they are skipped rather than searched.
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

ExecutableIdentity executableIdentity(const QString& absolute_path) {
    ExecutableIdentity identity;
    if (absolute_path.isEmpty()) {
        return identity;
    }
    const std::wstring wide = absolute_path.toStdWString();
    // Metadata only, and share everything: this is a WITNESS, not a lock. FILE_FLAG_
    // BACKUP_SEMANTICS is absent deliberately -- a directory is not a launch target, and
    // refusing to open one here is a check the caller gets for free.
    const HANDLE handle = ::CreateFileW(wide.c_str(),
                                        FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL,
                                        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return identity;  // unreadable -> stays invalid, and every caller fails closed on that
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL ok = ::GetFileInformationByHandle(handle, &info);
    ::CloseHandle(handle);
    if (ok == 0) {
        return identity;
    }
    identity.volume_serial = info.dwVolumeSerialNumber;
    identity.file_index = (static_cast<quint64>(info.nFileIndexHigh) << kFileIndexHighShift) |
                          static_cast<quint64>(info.nFileIndexLow);
    identity.size_bytes = (static_cast<qint64>(info.nFileSizeHigh) << kFileIndexHighShift) |
                          static_cast<qint64>(info.nFileSizeLow);
    identity.last_write_ms = (static_cast<qint64>(info.ftLastWriteTime.dwHighDateTime)
                              << kFileIndexHighShift) |
                             static_cast<qint64>(info.ftLastWriteTime.dwLowDateTime);
    identity.valid = true;
    return identity;
}

bool sameExecutable(const ExecutableIdentity& lhs, const ExecutableIdentity& rhs) {
    // Two INVALID identities are not equal. Comparing "could not read" to "could not read" as a
    // match is how a revalidation check turns into a rubber stamp on exactly the runs where the
    // file became unreadable.
    if (!lhs.valid || !rhs.valid) {
        return false;
    }
    return lhs.volume_serial == rhs.volume_serial && lhs.file_index == rhs.file_index &&
           lhs.size_bytes == rhs.size_bytes && lhs.last_write_ms == rhs.last_write_ms;
}

QString windowsDirPath(const QString& relativeExe) {
    if (!isContainedRelativeExe(relativeExe)) {
        return {};  // fail closed: an un-contained name must not be joined under %WINDIR%
    }
    wchar_t buffer[MAX_PATH];
    const UINT len = GetWindowsDirectoryW(buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};  // fail closed: cannot trust an unresolved Windows directory
    }
    return QDir::cleanPath(QString::fromWCharArray(buffer, static_cast<int>(len)) +
                           QLatin1Char('/') + relativeExe);
}

bool startDetachedSystem32Tool(const QString& relativeExe,
                               const QStringList& args,
                               const QString& working_directory) {
    const QString program = system32Path(relativeExe);
    if (program.isEmpty()) {
        return false;  // fail closed: never retry with the bare, search-order-resolved name
    }
    if (const auto injected = injectedDetachedLaunchResult(program, args)) {
        return *injected;
    }
    if (!working_directory.isEmpty()) {
        return QProcess::startDetached(program, args, working_directory);
    }
    return QProcess::startDetached(program, args);
}

bool startDetachedWindowsTool(const QString& relativeExe, const QStringList& args) {
    const QString program = windowsDirPath(relativeExe);
    if (program.isEmpty()) {
        return false;  // fail closed: never retry with the bare, search-order-resolved name
    }
    if (const auto injected = injectedDetachedLaunchResult(program, args)) {
        return *injected;
    }
    return QProcess::startDetached(program, args);
}

QString systemPowerShellPath() {
    const QString path = system32Path(QStringLiteral("WindowsPowerShell/v1.0/powershell.exe"));
    // Fail closed on a missing interpreter rather than hand a non-existent path to
    // CreateProcess: the caller must surface "cannot resolve PowerShell", never retry
    // with the bare name.
    return QFile::exists(path) ? path : QString();
}

ProcessResult runProcessStreaming(const ProcessStreamingRequest& request) {
    return runProcessInternal({
        .program = request.program,
        .args = request.args,
        .timeout_ms = request.timeout_ms,
        .should_cancel = request.should_cancel,
        .on_output = request.on_output,
        .on_started = request.on_started,
    });
}

void setProcessFaultInjectorForTesting(ProcessFaultInjector injector) {
    g_process_fault_injector = std::move(injector);
}

ScopedProcessFaultInjector::ScopedProcessFaultInjector(ProcessFaultInjector injector)
    : m_previous(std::move(g_process_fault_injector)) {
    g_process_fault_injector = std::move(injector);
}

ScopedProcessFaultInjector::~ScopedProcessFaultInjector() {
    g_process_fault_injector = std::move(m_previous);
}

}  // namespace sak

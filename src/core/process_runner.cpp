// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/process_runner.h"

#include "sak/layout_constants.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include <optional>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace sak {

namespace {

// Test-only process fault injector (R5-G14-17). Null in production -- never installed -- so
// the launchers pay only a single predicate check and behave identically. Armed by a test via
// ScopedProcessFaultInjector to substitute a chosen ProcessResult for a real launch.
ProcessFaultInjector g_process_fault_injector;

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
    ProcessTerminationCallback on_terminate;
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

// Dispatch a detached whole-tree kill (taskkill /T) for the child rooted at
// @p pid. Returns true only if the kill helper was actually launched. A false
// return (no pid, or startDetached refused to spawn) MUST NOT be read as
// "terminated" -- the caller falls back to QProcess::kill so the direct child
// is always signalled rather than the cancel silently no-opping.
bool dispatchDetachedTreeKill(qint64 pid) {
#ifdef Q_OS_WIN
    if (pid <= 0) {
        return false;
    }
    // Launch the System32-qualified taskkill DIRECTLY (no cmd.exe shell, and never a bare
    // "taskkill" that a PATH/CWD-planted binary could satisfy): the tree kill can be
    // dispatched from an elevated worker, so both the program AND the tool it invokes must
    // be resolved absolutely. taskkill /T reaps the whole child tree. Unresolvable ->
    // return false so the caller kills the direct child instead.
    const QString taskkill = system32Path(QStringLiteral("taskkill.exe"));
    if (taskkill.isEmpty()) {
        return false;
    }
    return QProcess::startDetached(
        taskkill,
        {QStringLiteral("/PID"), QString::number(pid), QStringLiteral("/T"), QStringLiteral("/F")});
#else
    Q_UNUSED(pid);
    return false;
#endif
}

void terminateProcess(QProcess* proc, const ProcessTerminationCallback& on_terminate) {
    if (on_terminate) {
        on_terminate();
    }
    // Reap orphaned grandchildren via the detached whole-tree kill, and ALWAYS also kill
    // the direct child. startDetached only proves the kill helper LAUNCHED, not that
    // taskkill actually reached and terminated the child, so the child we own must be
    // signalled directly here rather than trusted to the detached, best-effort tree kill.
    // A cancel/timeout can then never leave the direct child running while we report it
    // torn down.
    dispatchDetachedTreeKill(proc->processId());
    proc->kill();
}

bool startProcess(const ProcessRunRequest& request, QProcess* proc, ProcessResult* result) {
    if (request.environment != nullptr) {
        proc->setProcessEnvironment(*request.environment);
    }
    proc->start(request.program, request.args);
    if (proc->waitForStarted(sak::kTimeoutProcessStartMs)) {
        // Close the child's stdin so any tool that reads it gets immediate EOF
        // instead of blocking. Chocolatey's non-elevated "Do you want to
        // continue?" prompt (not suppressed by --yes) otherwise stalls ~20s per
        // package waiting on a stdin that never arrives; with EOF it proceeds at
        // once. We never write to a child's stdin, so this is always safe.
        proc->closeWriteChannel();
        return true;
    }
    result->std_err = QStringLiteral("Failed to start process: %1").arg(proc->errorString());
    return false;
}

bool stopForCancelOrTimeout(const ProcessRunRequest& request,
                            QProcess* proc,
                            ProcessResult* result,
                            const QElapsedTimer& timer) {
    if (request.should_cancel && request.should_cancel()) {
        result->cancelled = true;
        terminateProcess(proc, request.on_terminate);
        return true;
    }
    if (request.timeout_ms > 0 && timer.elapsed() >= request.timeout_ms) {
        result->timed_out = true;
        result->std_err += QStringLiteral("Process timed out");
        terminateProcess(proc, request.on_terminate);
        return true;
    }
    return false;
}

ProcessResult runProcessInternal(const ProcessRunRequest& request) {
    // Test-only fault injection (R5-G14-17): an armed injector fully substitutes for the
    // launch so a caller's mid-operation failure path runs without a real child. Null in
    // production (never installed), so this is a single predicate check. A nullopt return
    // means "no fault this time" -- fall through to the real launch (pass-through).
    if (g_process_fault_injector) {
        if (std::optional<ProcessResult> injected = g_process_fault_injector(request.program,
                                                                             request.args)) {
            return *injected;
        }
    }

    ProcessResult result;
    result.exit_code = -1;

    // Honor a cancel that is ALREADY set before we launch: a pre-cancelled request must
    // not start a (possibly destructive, elevated) process at all. Without this the
    // command runs to completion and can report success, because should_cancel is
    // otherwise first consulted only after the process has already been started.
    if (request.should_cancel && request.should_cancel()) {
        result.cancelled = true;
        return result;
    }

    QProcess proc;
    if (!startProcess(request, &proc, &result)) {
        return result;
    }
    if (request.on_started) {
        request.on_started(proc.processId());
    }

    QElapsedTimer timer;
    timer.start();
    constexpr int kPollMs = 100;
    while (true) {
        if (proc.waitForFinished(kPollMs)) {
            drainProcessOutput(&proc, &result, request.on_output);
            break;
        }
        drainProcessOutput(&proc, &result, request.on_output);

        if (stopForCancelOrTimeout(request, &proc, &result, timer)) {
            proc.waitForFinished(sak::kTimeoutProcessTerminateMs);
            drainProcessOutput(&proc, &result, request.on_output);
            return result;
        }
    }

    result.exit_code = proc.exitCode();
    result.exit_status = static_cast<int>(proc.exitStatus());

    return result;
}

}  // namespace

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
    return runProcessInternal({.program = program,
                               .args = args,
                               .timeout_ms = timeout_ms,
                               .should_cancel = should_cancel});
}

ProcessResult runProcessWithEnvironment(const QString& program,
                                        const QStringList& args,
                                        int timeout_ms,
                                        const QProcessEnvironment& environment,
                                        const CancelCheck& should_cancel) {
    return runProcessInternal({.program = program,
                               .args = args,
                               .timeout_ms = timeout_ms,
                               .environment = &environment,
                               .should_cancel = should_cancel});
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

#ifdef Q_OS_WIN
// Reject anything that is not a bare, System32/Windows-relative name (subdirectories with
// forward slashes -- "WindowsPowerShell/v1.0/powershell.exe" -- are fine). An empty name, a
// ".." traversal, or an already-absolute/rooted path could escape the resolved system
// directory once joined, so refuse it and let the caller fail closed on the empty return.
static bool isContainedRelativeExe(const QString& relativeExe) {
    return !relativeExe.isEmpty() && !relativeExe.contains(QStringLiteral("..")) &&
           !QDir::isAbsolutePath(relativeExe);
}
#endif

QString system32Path(const QString& relativeExe) {
#ifdef Q_OS_WIN
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
#else
    return relativeExe;
#endif
}

QString resolveBareExecutable(const QString& name) {
#ifdef Q_OS_WIN
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
#endif
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
#ifdef Q_OS_WIN
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
#else
    // Off Windows there is no volume-serial/file-index pair to read, so the identity is
    // WEAKER by construction: size and modification time only. Stated rather than hidden --
    // a replacement that preserves both would compare equal here and not on Windows.
    const QFileInfo info(absolute_path);
    if (!info.isFile()) {
        return identity;
    }
    identity.size_bytes = info.size();
    identity.last_write_ms = info.lastModified().toMSecsSinceEpoch();
    identity.valid = true;
    return identity;
#endif
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
#ifdef Q_OS_WIN
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
#else
    return relativeExe;
#endif
}

bool startDetachedSystem32Tool(const QString& relativeExe, const QStringList& args) {
    const QString program = system32Path(relativeExe);
    if (program.isEmpty()) {
        return false;  // fail closed: never retry with the bare, search-order-resolved name
    }
    return QProcess::startDetached(program, args);
}

bool startDetachedWindowsTool(const QString& relativeExe, const QStringList& args) {
    const QString program = windowsDirPath(relativeExe);
    if (program.isEmpty()) {
        return false;  // fail closed: never retry with the bare, search-order-resolved name
    }
    return QProcess::startDetached(program, args);
}

QString systemPowerShellPath() {
#ifdef Q_OS_WIN
    const QString path = system32Path(QStringLiteral("WindowsPowerShell/v1.0/powershell.exe"));
    // Fail closed on a missing interpreter rather than hand a non-existent path to
    // CreateProcess: the caller must surface "cannot resolve PowerShell", never retry
    // with the bare name.
    return QFile::exists(path) ? path : QString();
#else
    return QStringLiteral("powershell.exe");
#endif
}

ProcessResult runProcessStreaming(const ProcessStreamingRequest& request) {
    return runProcessInternal({.program = request.program,
                               .args = request.args,
                               .timeout_ms = request.timeout_ms,
                               .should_cancel = request.should_cancel,
                               .on_output = request.on_output,
                               .on_started = request.on_started,
                               .on_terminate = request.on_terminate});
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

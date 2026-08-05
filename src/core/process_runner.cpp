// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/process_runner.h"

#include "sak/layout_constants.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace sak {

namespace {

// Hard per-stream ceiling on retained output. A runaway child (e.g. a script
// that prints gigabytes) previously grew std_out/std_err without bound until the
// process exhausted memory; caller-side caps were applied only AFTER the full
// output had already been accumulated. This bounds accumulation itself. It sits
// well above the largest caller cap (~64 MiB) so it never clips legitimate
// captured output, only pathological floods.
constexpr qsizetype kMaxAccumulatedOutputChars = static_cast<qsizetype>(128) * 1024 * 1024;

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
    // System32-qualified shell, never a bare "cmd.exe": the tree kill can be dispatched
    // from an elevated worker, so a PATH/CWD-planted cmd.exe must not be able to satisfy
    // it. Unresolvable -> return false so the caller kills the direct child instead.
    const QString shell = system32Path(QStringLiteral("cmd.exe"));
    if (shell.isEmpty()) {
        return false;
    }
    return QProcess::startDetached(shell,
                                   {QStringLiteral("/C"),
                                    QStringLiteral("taskkill /PID %1 /T /F >NUL 2>NUL").arg(pid)});
#else
    Q_UNUSED(pid);
    return false;
#endif
}

void terminateProcess(QProcess* proc, const ProcessTerminationCallback& on_terminate) {
    if (on_terminate) {
        on_terminate();
    }
    // Prefer the tree kill (reaps orphaned grandchildren), but never rely on it:
    // if it could not be launched, fall through to kill the direct child so a
    // cancel/timeout can never leave the child running while we report it torn
    // down.
    if (!dispatchDetachedTreeKill(proc->processId())) {
        proc->kill();
    }
}

bool startProcess(const ProcessRunRequest& request, QProcess* proc, ProcessResult* result) {
    if (request.environment) {
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
    ProcessResult result;
    result.exit_code = -1;

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

QString system32Path(const QString& relativeExe) {
#ifdef Q_OS_WIN
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

QString windowsDirPath(const QString& relativeExe) {
#ifdef Q_OS_WIN
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

}  // namespace sak

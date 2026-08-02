// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/process_runner.h"

#include "sak/layout_constants.h"

#include <QElapsedTimer>
#include <QProcess>

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

void terminateProcess(QProcess* proc, const ProcessTerminationCallback& on_terminate) {
    if (on_terminate) {
        on_terminate();
    }
#ifdef Q_OS_WIN
    const qint64 pid = proc->processId();
    if (pid > 0) {
        QProcess::startDetached(QStringLiteral("cmd.exe"),
                                {QStringLiteral("/C"),
                                 QStringLiteral("taskkill /PID %1 /T /F >NUL 2>NUL").arg(pid)});
        return;
    }
#endif
    proc->kill();
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

    return runProcess("powershell.exe", args, timeout_ms, should_cancel);
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

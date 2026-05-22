// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/process_runner.h"

#include "sak/layout_constants.h"

#include <QElapsedTimer>
#include <QProcess>

namespace sak {

namespace {

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
    if (stderr_stream) {
        result->std_err += chunk;
    } else {
        result->std_out += chunk;
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

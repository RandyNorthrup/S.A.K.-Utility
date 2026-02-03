// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/process_runner.h"

#include <QProcess>
#include <QElapsedTimer>

namespace sak {

ProcessResult runProcess(const QString& program, const QStringList& args, int timeout_ms, const CancelCheck& should_cancel) {
    QProcess proc;
    proc.start(program, args);

    ProcessResult result;
    QElapsedTimer timer;
    timer.start();

    const int poll_ms = 250;
    while (true) {
        if (proc.waitForFinished(poll_ms)) {
            break;
        }

        if (should_cancel && should_cancel()) {
            proc.kill();
            result.cancelled = true;
            return result;
        }

        if (timeout_ms > 0 && timer.elapsed() >= timeout_ms) {
            proc.kill();
            result.timed_out = true;
            return result;
        }
    }

    result.exit_code = proc.exitCode();
    result.exit_status = static_cast<int>(proc.exitStatus());
    result.std_out = proc.readAllStandardOutput();
    result.std_err = proc.readAllStandardError();

    return result;
}

ProcessResult runPowerShell(const QString& script, int timeout_ms, bool no_profile, bool bypass_policy, const CancelCheck& should_cancel) {
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

} // namespace sak

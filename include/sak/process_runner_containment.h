// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/process_runner.h"
#include "sak/windows_process_job.h"

#include <QProcess>
#include <QStringList>

#include <memory>
#include <optional>

/// @file process_runner_containment.h
/// @brief The step every ProcessRunner launch ends with: settling the contained process family.
namespace sak {

/// One started (or attempted) process and the kill-on-close Job that contains its family.
struct ContainedProcessRun {
    std::unique_ptr<QProcess> process;
    std::unique_ptr<WindowsProcessJob> job;
};

/// True when every process in @p member_images is a console host Windows started for the family
/// (%SystemRoot%\System32\conhost.exe), and there is at least one. A console program started
/// without a console -- every one QProcess starts from this GUI application -- gets a conhost.exe
/// of its own in its Job. It exits once its last client has, which takes milliseconds on a PC at
/// rest (5 to 20 ms on testpc1, netsh included) but can run past the 1 s grace on a loaded one,
/// and that read as "descendants remained" and failed a netsh call whose work was done. A console
/// host does none of the program's work, so it is stopped like any leftover and the run is
/// judged on the program alone. An unreadable image (empty path) is not a console host.
[[nodiscard]] bool onlyConsoleHostsRemain(const QStringList& member_images);

/// Settles @p run: optionally terminates the family (@p terminate_now), waits for the primary and
/// every descendant to leave the Job within kTimeoutProcessTerminateMs, and closes the empty Job.
/// After a normal primary exit, @p options decides how long the family may run on first (about
/// 1 s, or ProcessRunOptions::job_wait_ms under ChildProcessPolicy::WaitForJobEmpty, cut short by
/// @p should_cancel, which then marks @p result cancelled).
///
/// Returns whether descendants were still alive after a normal primary exit and had to be forced
/// (always false when @p terminate_now). Returns std::nullopt when cleanup could not be proved --
/// a member outlived the deadline, or a native Job query failed. That is load-dependent, not a
/// programming error, so it is reported rather than fatal (owner decision 2026-10-24, "report +
/// contain"): the diagnostic is appended to @p result, which is marked failed, and the process and
/// its Job are handed to sak::ContainedProcessQuarantine. Their kill-on-close Job stays armed, so
/// no member can outlive the application. @p run is empty afterwards and must not be touched.
[[nodiscard]] std::optional<bool> settleContainedProcessRun(ContainedProcessRun& run,
                                                            bool terminate_now,
                                                            ProcessResult* result,
                                                            const ProcessRunOptions& options = {},
                                                            const CancelCheck& should_cancel = {});

}  // namespace sak

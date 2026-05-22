// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <functional>

namespace sak {

/// @brief Result of an external process execution including exit code, output, and status flags
struct ProcessResult {
    int exit_code{0};
    int exit_status{0};
    bool timed_out{false};
    bool cancelled{false};
    QString std_out;
    QString std_err;

    /// @brief Check if the process completed successfully (no timeout, exit code 0)
    [[nodiscard]] bool succeeded() const noexcept { return !timed_out && exit_code == 0; }
};

using CancelCheck = std::function<bool()>;
using ProcessOutputCallback = std::function<void(const QString& chunk, bool is_stderr)>;
using ProcessStartedCallback = std::function<void(qint64 process_id)>;
using ProcessTerminationCallback = std::function<void()>;

struct ProcessStreamingRequest {
    QString program;
    QStringList args;
    int timeout_ms{0};
    ProcessOutputCallback on_output;
    ProcessStartedCallback on_started;
    ProcessTerminationCallback on_terminate;
    CancelCheck should_cancel;
};

[[nodiscard]] ProcessResult runProcess(const QString& program,
                                       const QStringList& args,
                                       int timeout_ms,
                                       const CancelCheck& should_cancel = {});
[[nodiscard]] ProcessResult runProcessWithEnvironment(const QString& program,
                                                      const QStringList& args,
                                                      int timeout_ms,
                                                      const QProcessEnvironment& environment,
                                                      const CancelCheck& should_cancel = {});
[[nodiscard]] ProcessResult runPowerShell(const QString& script,
                                          int timeout_ms,
                                          bool no_profile = true,
                                          bool bypass_policy = true,
                                          const CancelCheck& should_cancel = {});
[[nodiscard]] ProcessResult runProcessStreaming(const ProcessStreamingRequest& request);

}  // namespace sak

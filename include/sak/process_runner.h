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
    /// @brief Process exit code. Defaults to -1 (fail closed) so a ProcessResult that
    ///        was never actually run cannot report success: succeeded() requires
    ///        exit_code == 0, and every real run assigns the child's true exit code.
    int exit_code{-1};
    /// @brief QProcess::ExitStatus as an int: 0 == NormalExit, 1 == CrashExit.
    int exit_status{0};
    bool timed_out{false};
    bool cancelled{false};
    QString std_out;
    QString std_err;

    /// @brief True if captured output hit the hard accumulation ceiling and some
    ///        output was dropped from std_out/std_err (memory-exhaustion guard).
    bool output_truncated{false};

    /// @brief Check if the process completed successfully (no timeout, a normal
    ///        (non-crash) exit, and exit code 0).
    /// @note Does NOT consider cancellation -- see completedSuccessfully(). A crash
    ///       that still leaves exit_code 0 is rejected via exit_status (0 ==
    ///       QProcess::NormalExit), so a crashed child never passes as a success.
    [[nodiscard]] bool succeeded() const noexcept {
        return !timed_out && exit_status == 0 && exit_code == 0;
    }

    /// @brief Check if the process ran fully to a clean finish: not timed out,
    ///        NOT cancelled, a normal (non-crash) exit, and exit code 0.
    /// @note Stricter than succeeded(): a cancelled process (which may still
    ///       report exit_code 0 depending on how it was torn down) is NOT a
    ///       success. Use this when reporting an operation's outcome so a
    ///       cancelled/aborted run is never reported as having succeeded.
    [[nodiscard]] bool completedSuccessfully() const noexcept {
        return !timed_out && !cancelled && exit_status == 0 && exit_code == 0;
    }
};

/// @brief Append @p chunk to @p target without letting @p target exceed @p cap
///        characters.
/// @return true if any characters had to be dropped (target already full, or the
///         chunk was clipped to fit).
/// @note Bounds process-output accumulation so a runaway child cannot exhaust
///       memory; the live on_output stream still receives every chunk in full.
///       Exposed for unit testing.
[[nodiscard]] bool appendCappedOutput(QString& target, const QString& chunk, qsizetype cap);

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

/// @brief Resolve a System32-relative executable to its absolute path so a trusted
///        system tool cannot be hijacked by a PATH/CWD-planted binary of the same
///        name when launched from an elevated process. @p relativeExe is joined under
///        the real system directory (GetSystemDirectoryW), e.g. "netsh.exe" or
///        "WindowsPowerShell/v1.0/powershell.exe". Returns an empty string (the caller
///        MUST fail closed) if the system directory cannot be resolved. On non-Windows
///        returns @p relativeExe unchanged.
[[nodiscard]] QString system32Path(const QString& relativeExe);

/// @brief Absolute path to the Windows-shipped powershell.exe
///        (System32\\WindowsPowerShell\\v1.0). Launching the unqualified
///        "powershell.exe" resolves through the CreateProcess search order -- which
///        includes the current directory ahead of System32 -- so an attacker who
///        plants a powershell.exe on that search path gets it run, worse still by an
///        elevated caller. Returns an empty string when the system directory cannot
///        be resolved or the interpreter is not present there; every caller MUST fail
///        closed on empty instead of falling back to the bare name. On non-Windows
///        returns "powershell.exe".
[[nodiscard]] QString systemPowerShellPath();

/// @brief Same as system32Path but rooted at the Windows directory itself
///        (GetWindowsDirectoryW) rather than System32, for the tools that live there --
///        explorer.exe most notably, which is NOT a System32 binary. Returns an empty
///        string (the caller MUST fail closed) if the Windows directory cannot be
///        resolved. On non-Windows returns @p relativeExe unchanged.
[[nodiscard]] QString windowsDirPath(const QString& relativeExe);

/// @brief Launch a Windows-shipped System32 tool DETACHED, naming it by its absolute path.
///
/// A shell-open ("explorer.exe <uri>", "control.exe /name ...") is resolved through the
/// same CreateProcess search order as any other launch -- current directory and PATH ahead
/// of the system directories -- so a bare name is a hijack vector whether or not the target
/// is an interpreter. Returns false WITHOUT launching anything when the path cannot be
/// resolved; there is deliberately no fall back to the bare name.
[[nodiscard]] bool startDetachedSystem32Tool(const QString& relativeExe,
                                             const QStringList& args = {});

/// @brief startDetachedSystem32Tool for a tool that lives directly in the Windows
///        directory (explorer.exe) instead of System32.
[[nodiscard]] bool startDetachedWindowsTool(const QString& relativeExe,
                                            const QStringList& args = {});

}  // namespace sak

// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

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

/// @brief Test-only fault-injection seam for the process launchers (R5-G14-17).
///
/// When an injector is installed, EVERY process launch -- runProcess,
/// runProcessWithEnvironment, runPowerShell and runProcessStreaming all funnel through one
/// internal runner -- consults it FIRST. If the injector returns a ProcessResult, that result
/// is returned WITHOUT launching any process: the exact mid-operation failure (a non-zero
/// exit, a timeout, a crash-exit, truncated output) a test needs a caller's failure path to
/// actually execute. Returning nullopt lets the real launch proceed (pass-through). The
/// default is no injector, so production pays a single null check and behaves identically;
/// production code never installs one. Install through ScopedProcessFaultInjector so an armed
/// injector can never leak out of a test into the next.
using ProcessFaultInjector =
    std::function<std::optional<ProcessResult>(const QString& program, const QStringList& args)>;

/// @brief Install (or, with a null injector, clear) the process fault injector. Test-only.
void setProcessFaultInjectorForTesting(ProcessFaultInjector injector);

/// @brief RAII installer: arms @p injector for its lifetime and restores the previous state
///        (normally none) on destruction, so a test cannot leave the launchers armed.
class ScopedProcessFaultInjector {
public:
    explicit ScopedProcessFaultInjector(ProcessFaultInjector injector);
    ~ScopedProcessFaultInjector();
    ScopedProcessFaultInjector(const ScopedProcessFaultInjector&) = delete;
    ScopedProcessFaultInjector& operator=(const ScopedProcessFaultInjector&) = delete;
    ScopedProcessFaultInjector(ScopedProcessFaultInjector&&) = delete;
    ScopedProcessFaultInjector& operator=(ScopedProcessFaultInjector&&) = delete;

private:
    ProcessFaultInjector m_previous;
};

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

/// @brief Resolve a BARE executable name (no path separator) to an absolute path, fail-closed.
///
/// CreateProcess searches the CURRENT DIRECTORY ahead of PATH, so launching a bare name lets a
/// binary planted in the working directory win. System32 wins first -- both by exact name and
/// with the PATHEXT suffixes applied, because a model naturally writes "whoami" rather than
/// "whoami.exe" and the exact check alone would fall through to PATH for every extensionless
/// spelling. Only the ABSOLUTE entries of PATH are then searched; "." and relative entries are
/// skipped because they resolve against the working directory. Returns an empty string when
/// nothing resolves, and every caller MUST fail closed on that rather than launching the name.
///
/// THIS LIVES HERE BECAUSE IT HAD TWO HOMES. The AI command planner resolved the program at
/// plan time so the approval preview names the binary that will run, and the execution broker
/// resolved it again at launch time as a backstop -- byte-for-byte the same logic, in two files.
/// Fixing the System32 suffix gap in one of them was what made the copies disagree, which is the
/// whole argument for one home.
[[nodiscard]] QString resolveBareExecutable(const QString& name);

/// @brief Identity of an executable FILE, as opposed to the path that currently names it.
///
/// A resolved path is a name, and a name is not a binding. Between the moment a launch target
/// is decided and the moment it launches, the same string can come to mean a different file:
/// the file can be replaced in place, the directory renamed and another put in its place, or a
/// junction/symlink on the path retargeted. Nothing about the string changes.
///
/// @c valid is false when the file could not be examined at all. It is NOT a synthesised
/// "unknown but probably fine": every caller must fail closed on it, because an identity that
/// could not be taken cannot later be compared.
struct ExecutableIdentity {
    /// Volume serial + file index is the Win32 identity of a file: it survives a rename and
    /// differs for a replacement, which is exactly the discrimination a path cannot make.
    quint32 volume_serial{0};
    quint64 file_index{0};
    qint64 size_bytes{-1};
    qint64 last_write_ms{-1};
    bool valid{false};
};

/// @brief Capture the identity of the file at @p absolute_path, or an invalid identity.
///
/// Opens the file for metadata only, sharing read/write/delete: this is a WITNESS, not a lock.
/// Holding a deny-write handle across a human approval would block legitimate updates of a
/// system binary for as long as a dialog sits open, and would still not survive process exit,
/// so the design records what the file WAS and re-checks rather than trying to freeze it.
[[nodiscard]] ExecutableIdentity executableIdentity(const QString& absolute_path);

/// @brief True when both identities are valid AND name the same file with the same size and
///        modification time. Two invalid identities are NOT equal -- absence of evidence must
///        never compare equal to absence of evidence, or an unreadable file would validate
///        against another unreadable file.
[[nodiscard]] bool sameExecutable(const ExecutableIdentity& lhs, const ExecutableIdentity& rhs);

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

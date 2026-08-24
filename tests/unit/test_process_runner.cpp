// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_process_runner.cpp
/// @brief Unit tests for process execution utilities

#include "sak/process_runner.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QRegularExpression>
#include <QtTest/QtTest>

#include <optional>

class ProcessRunnerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // runProcess
    void runProcess_echoCommand();
    void runProcess_exitCode();
    void runProcess_stderrCapture();
    void runProcess_nonExistentProgram();
    void runProcess_timeout();
    void runProcess_timeoutKillsChildTree();
    void runProcess_cancellation();
    void runProcess_cancellationKillsChild();

    // runPowerShell
    void runPowerShell_simpleScript();
    void runPowerShell_withNoProfile();
    void runPowerShell_scriptError();

    // B5-07: bounded output accumulation
    void appendCappedOutput_boundsAccumulation();

    // B5-05: strict success (cancellation counts as failure)
    void completedSuccessfully_strictOutcome();

    // CODEX_REVIEW_4 H14: System32 qualification of system executables
    void system32Path_qualifiesUnderSystem32();

    // R5-P7-22: the shared PowerShell resolver every privileged caller must use
    void systemPowerShellPath_resolvesAbsoluteInterpreter();

    // R5-G14-17: shared process fault-injection seam
    void processFaultInjector_substitutesResultForEveryLauncher();
    void processFaultInjector_scopeRestoresRealLaunch();
};

// ============================================================================
// runProcess Tests
// ============================================================================

void ProcessRunnerTests::runProcess_echoCommand() {
    auto result = sak::runProcess("cmd.exe", {"/C", "echo Hello World"}, 10'000);

    QCOMPARE(result.exit_code, 0);
    QVERIFY(!result.timed_out);
    QVERIFY(!result.cancelled);
    QVERIFY(result.completedSuccessfully());
    QCOMPARE(result.std_out.trimmed(), QStringLiteral("Hello World"));
    // The stream split is real: appendOutput routes each drained chunk to exactly ONE buffer
    // (process_runner.cpp:58), so a child that wrote only to stdout leaves stderr EMPTY. A
    // mutant that mirrored chunks into both buffers passes every other assertion in this file
    // (runProcess_stderrCapture's child writes only to stderr), while ~20 callers that treat
    // any stderr text as a failure -- e.g. reset_network_action.cpp:68 -- would break.
    QVERIFY2(result.std_err.isEmpty(), qPrintable(result.std_err));
    // Nothing was dropped, so the truncation flag must stay clear (process_runner.cpp:59-61).
    QVERIFY(!result.output_truncated);
    QCOMPARE(result.std_out.trimmed(), QStringLiteral("Hello World"));
}

void ProcessRunnerTests::runProcess_exitCode() {
    auto result = sak::runProcess("cmd.exe", {"/C", "exit /b 42"}, 10'000);

    QCOMPARE(result.exit_code, 42);
    QVERIFY(!result.timed_out);
}

void ProcessRunnerTests::runProcess_stderrCapture() {
    auto result = sak::runProcess("cmd.exe", {"/C", "echo error message 1>&2"}, 10'000);

    // cmd writes only the echoed text to stderr; trimmed() strips the trailing space/CRLF.
    QCOMPARE(result.std_err.trimmed(), QStringLiteral("error message"));
}

void ProcessRunnerTests::runProcess_nonExistentProgram() {
    auto result = sak::runProcess("nonexistent_program_xyz_12345.exe", {}, 5000);

    // After BUG-13 fix: waitForStarted() detects launch failure immediately.
    // exit_code should be -1 and std_err should contain an error message.
    QCOMPARE(result.exit_code, -1);
    // Launch failure always begins with this fixed, locale-independent prefix (only the
    // trailing QProcess errorString() varies).
    QVERIFY2(result.std_err.startsWith(QStringLiteral("Failed to start process:")),
             qPrintable(result.std_err));
}

void ProcessRunnerTests::runProcess_timeout() {
    // Start a long-running process with a short timeout
    auto result = sak::runProcess("cmd.exe", {"/C", "ping -n 3 127.0.0.1"}, 1000);

    // The timeout arm (process_runner.cpp:143-148) must be distinguishable from the cancel arm
    // and from a normal finish: it sets timed_out and NOT cancelled, appends the timeout marker
    // to stderr, and returns on the early path that never overwrites the fail-closed -1.
    QVERIFY(result.timed_out);
    QVERIFY(!result.cancelled);
    QCOMPARE(result.exit_code, -1);
    QVERIFY2(result.std_err.contains(QStringLiteral("Process timed out")),
             qPrintable(result.std_err));
    QVERIFY(!result.completedSuccessfully());
}

void ProcessRunnerTests::runProcess_timeoutKillsChildTree() {
#ifndef Q_OS_WIN
    QSKIP("Process tree termination is Windows-specific");
#else
    const QString script =
        "Set-StrictMode -Version Latest; "
        "$child = Start-Process powershell.exe "
        "-WindowStyle Hidden "
        "-ArgumentList @('-NoProfile','-Command','Start-Sleep -Seconds 30') "
        "-PassThru; "
        "Write-Output $child.Id; "
        "Start-Sleep -Seconds 30";

    auto result = sak::runPowerShell(script, 1500);
    QVERIFY(result.timed_out);

    const QRegularExpression pidRegex(QStringLiteral("\\b(\\d+)\\b"));
    const auto match = pidRegex.match(result.std_out);
    QVERIFY2(match.hasMatch(),
             qPrintable(QStringLiteral("Child PID missing from stdout: %1").arg(result.std_out)));
    const QString childPid = match.captured(1);

    QTest::qWait(750);
    const auto tasklist = sak::runProcess(
        "cmd.exe", {"/C", QStringLiteral("tasklist /FI \"PID eq %1\"").arg(childPid)}, 5000);
    QVERIFY2(
        !tasklist.std_out.contains(childPid),
        qPrintable(
            QStringLiteral("Timed-out child process still running: %1").arg(tasklist.std_out)));
#endif
}

void ProcessRunnerTests::runProcess_cancellation() {
    bool shouldCancel = true;
    auto result = sak::runProcess("cmd.exe",
                                  {"/C", "ping -n 3 127.0.0.1"},
                                  10'000,
                                  [&shouldCancel]() -> bool { return shouldCancel; });

    // A cancel already set BEFORE the launch must make runProcessInternal refuse to START the
    // child at all (process_runner.cpp:171-174), not kill it after the fact -- so it returns
    // the fail-closed -1 with nothing ever captured.
    QVERIFY(result.cancelled);
    QVERIFY(!result.timed_out);
    QCOMPARE(result.exit_code, -1);
    QVERIFY(!result.completedSuccessfully());
    QVERIFY2(result.std_out.isEmpty(), qPrintable(result.std_out));
    QVERIFY2(result.std_err.isEmpty(), qPrintable(result.std_err));

    // ...and prove it through the one channel that fires for every process that REALLY
    // launches (process_runner.cpp:180-182): with the pre-launch guard deleted, the in-loop
    // cancel check sets the very same `cancelled` flag AFTER the child has already started.
    bool started = false;
    bool cancelAtOnce = true;
    sak::ProcessStreamingRequest request;
    request.program = QStringLiteral("cmd.exe");
    request.args = {QStringLiteral("/C"), QStringLiteral("ping -n 3 127.0.0.1")};
    request.timeout_ms = 10'000;
    request.should_cancel = [&cancelAtOnce]() -> bool {
        return cancelAtOnce;
    };
    request.on_started = [&started](qint64) {
        started = true;
    };

    const auto streamed = sak::runProcessStreaming(request);
    QVERIFY(streamed.cancelled);
    QVERIFY2(!started, "a request cancelled before launch must never start the child process");
    QCOMPARE(streamed.exit_code, -1);

    QVERIFY(result.cancelled);
}

// CODEX-2 HIGH (process_runner.cpp:67): cancel/timeout teardown must never
// fire-and-forget. A cancel of a long-lived child must actually terminate it
// (via the tree kill, or the QProcess::kill fallback when the tree kill cannot
// be launched) rather than leaving it running while reporting cancelled. The
// child sleeps far longer than the wait below, so a returned+gone process
// proves the teardown really killed it instead of just detaching.
void ProcessRunnerTests::runProcess_cancellationKillsChild() {
#ifndef Q_OS_WIN
    QSKIP("Process teardown assertion is Windows-specific");
#else
    QElapsedTimer timer;
    timer.start();

    bool shouldCancel = false;
    qint64 childPid = -1;
    sak::ProcessStreamingRequest request;
    request.program = QStringLiteral("cmd.exe");
    request.args = {QStringLiteral("/C"), QStringLiteral("ping -n 60 127.0.0.1")};
    request.timeout_ms = 60'000;
    request.should_cancel = [&shouldCancel]() -> bool {
        return shouldCancel;
    };
    request.on_started = [&](qint64 pid) {
        childPid = pid;
        shouldCancel = true;  // request cancellation as soon as the child is up
    };

    const auto result = sak::runProcessStreaming(request);

    QVERIFY(result.cancelled);
    // Cancellation must have returned promptly -- nowhere near the 60s child life.
    QVERIFY2(timer.elapsed() < 20'000,
             qPrintable(QStringLiteral("Cancel took %1 ms").arg(timer.elapsed())));

    QVERIFY(childPid > 0);
    QTest::qWait(750);  // let the async tree kill land
    const auto tasklist = sak::runProcess(
        "cmd.exe", {"/C", QStringLiteral("tasklist /FI \"PID eq %1\"").arg(childPid)}, 5000);
    QVERIFY2(!tasklist.std_out.contains(QString::number(childPid)),
             qPrintable(QStringLiteral("Cancelled child still running: %1").arg(tasklist.std_out)));
#endif
}

// ============================================================================
// runPowerShell Tests
// ============================================================================

void ProcessRunnerTests::runPowerShell_simpleScript() {
    auto result = sak::runPowerShell("Write-Output 'PowerShell Test'", 10'000);

    QCOMPARE(result.exit_code, 0);
    QVERIFY(!result.timed_out);
    QCOMPARE(result.std_out.trimmed(), QStringLiteral("PowerShell Test"));
}

void ProcessRunnerTests::runPowerShell_withNoProfile() {
    auto result = sak::runPowerShell("Write-Output $PSVersionTable.PSVersion.Major",
                                     10'000,
                                     true,   // no_profile
                                     true);  // bypass_policy

    QCOMPARE(result.exit_code, 0);
    QVERIFY(!result.timed_out);
    // $PSVersionTable.PSVersion.Major is a bare integer on every Windows PowerShell; pinning the
    // shape (not the version) stays machine-invariant while proving the script actually ran.
    QVERIFY2(
        QRegularExpression(QStringLiteral("^\\d+$")).match(result.std_out.trimmed()).hasMatch(),
        qPrintable(result.std_out));

    // The flags this test is NAMED for were never observed. Capture the exact argv the launcher
    // builds and pin it as an ORDERED catalog for BOTH arms of each switch, so a dropped
    // -NoProfile (the user's profile then runs inside an elevated launch), a dropped
    // -ExecutionPolicy Bypass, a reordering that puts -Command first, or a flag emitted when the
    // caller asked for it OFF is all caught.
    QStringList seen;
    sak::ScopedProcessFaultInjector guard(
        [&seen](const QString&, const QStringList& args) -> std::optional<sak::ProcessResult> {
            seen = args;
            sak::ProcessResult injected;
            injected.exit_code = 3;  // distinctive: proves the injector, not a real child, ran
            return injected;
        });

    seen.clear();
    QCOMPARE(sak::runPowerShell(QStringLiteral("Write-Output 1"), 10'000, true, true).exit_code, 3);
    QCOMPARE(seen,
             (QStringList{QStringLiteral("-NoProfile"),
                          QStringLiteral("-ExecutionPolicy"),
                          QStringLiteral("Bypass"),
                          QStringLiteral("-Command"),
                          QStringLiteral("Write-Output 1")}));

    seen.clear();
    QCOMPARE(sak::runPowerShell(QStringLiteral("Write-Output 1"), 10'000, false, false).exit_code,
             3);
    QCOMPARE(seen, (QStringList{QStringLiteral("-Command"), QStringLiteral("Write-Output 1")}));

    seen.clear();
    QCOMPARE(sak::runPowerShell(QStringLiteral("Write-Output 1"), 10'000, true, false).exit_code,
             3);
    QCOMPARE(seen,
             (QStringList{QStringLiteral("-NoProfile"),
                          QStringLiteral("-Command"),
                          QStringLiteral("Write-Output 1")}));

    seen.clear();
    QCOMPARE(sak::runPowerShell(QStringLiteral("Write-Output 1"), 10'000, false, true).exit_code,
             3);
    QCOMPARE(seen,
             (QStringList{QStringLiteral("-ExecutionPolicy"),
                          QStringLiteral("Bypass"),
                          QStringLiteral("-Command"),
                          QStringLiteral("Write-Output 1")}));
}

void ProcessRunnerTests::runPowerShell_scriptError() {
    auto result = sak::runPowerShell("Get-Item 'C:\\NonExistent_xyz_12345' -ErrorAction Stop",
                                     10'000);

    // Should fail with non-zero exit code
    QVERIFY(result.exit_code != 0);
}

// B5-07: output accumulation must be bounded so a runaway child cannot exhaust
// memory. The retained buffer stops growing exactly at the cap, reporting the
// drop, while a chunk that fits is appended in full.
void ProcessRunnerTests::appendCappedOutput_boundsAccumulation() {
    // A chunk that fits under the cap is appended whole; nothing dropped.
    QString buf;
    QVERIFY(!sak::appendCappedOutput(buf, QStringLiteral("hello"), 10));
    QCOMPARE(buf, QStringLiteral("hello"));

    // An empty chunk never drops.
    QVERIFY(!sak::appendCappedOutput(buf, QString(), 10));
    QCOMPARE(buf.size(), 5);

    // A chunk that overflows the cap is clipped to fit exactly and reports a drop.
    QVERIFY(sak::appendCappedOutput(buf, QStringLiteral("world!!!"), 10));
    QCOMPARE(buf.size(), 10);
    QCOMPARE(buf, QStringLiteral("helloworld"));

    // Once full, further non-empty chunks are dropped entirely (buffer frozen).
    QVERIFY(sak::appendCappedOutput(buf, QStringLiteral("more"), 10));
    QCOMPARE(buf.size(), 10);
    QCOMPARE(buf, QStringLiteral("helloworld"));

    // A non-positive cap drops any non-empty chunk.
    QString empty;
    QVERIFY(sak::appendCappedOutput(empty, QStringLiteral("x"), 0));
    QVERIFY(empty.isEmpty());

    // An EMPTY chunk never reports a drop -- not against a FULL buffer, not against a zero cap.
    // The empty-chunk check (process_runner.cpp:210-212) must come first; otherwise a harmless
    // empty read from a saturated buffer sets output_truncated and the result claims output was
    // lost. The case above uses a 5-of-10 buffer, which is not full, so it cannot see this.
    QVERIFY(!sak::appendCappedOutput(buf, QString(), 10));
    QCOMPARE(buf, QStringLiteral("helloworld"));
    QString none;
    QVERIFY(!sak::appendCappedOutput(none, QString(), 0));
    QVERIFY(none.isEmpty());

    // The exact-fit boundary: a chunk that exactly fills the remaining room is appended whole
    // and reports NO drop (process_runner.cpp:217 compares <=, not <); the very next character
    // does. Without this, `<` ships green -- identical content, but a spurious truncation
    // report that propagates to ProcessResult::output_truncated.
    QString exact;
    QVERIFY(!sak::appendCappedOutput(exact, QStringLiteral("12345"), 5));
    QCOMPARE(exact, QStringLiteral("12345"));
    QVERIFY(sak::appendCappedOutput(exact, QStringLiteral("6"), 5));
    QCOMPARE(exact, QStringLiteral("12345"));
    QString split;
    QVERIFY(!sak::appendCappedOutput(split, QStringLiteral("ab"), 5));
    QVERIFY(!sak::appendCappedOutput(split, QStringLiteral("cde"), 5));
    QCOMPARE(split, QStringLiteral("abcde"));
}

// B5-05: reporting an operation's outcome must treat a cancelled or timed-out or
// non-zero-exit run as a failure. succeeded() ignores cancellation;
// completedSuccessfully() must not.
void ProcessRunnerTests::completedSuccessfully_strictOutcome() {
    sak::ProcessResult clean;
    clean.exit_code = 0;
    QVERIFY(clean.completedSuccessfully());

    sak::ProcessResult nonzero;
    nonzero.exit_code = 1;
    QVERIFY(!nonzero.completedSuccessfully());

    sak::ProcessResult timed;
    timed.exit_code = 0;
    timed.timed_out = true;
    QVERIFY(!timed.completedSuccessfully());

    // The key case: a cancelled process that still reports exit_code 0 is NOT a
    // success under completedSuccessfully(), even though succeeded() says it is.
    sak::ProcessResult cancelled;
    cancelled.exit_code = 0;
    cancelled.cancelled = true;
    QVERIFY(cancelled.succeeded());  // documents the weaker check
    QVERIFY(!cancelled.completedSuccessfully());

    // The crash arm: a child that CRASHED but still reports exit_code 0 must be rejected by
    // BOTH predicates via exit_status (1 == QProcess::CrashExit, process_runner.h:21-22).
    // Nothing above sets exit_status, so dropping that term from either predicate is invisible.
    sak::ProcessResult crashed;
    crashed.exit_code = 0;
    crashed.exit_status = 1;
    QVERIFY(!crashed.succeeded());
    QVERIFY(!crashed.completedSuccessfully());

    // ...and the crash term is not over-broad: a normal exit really is 0.
    QCOMPARE(clean.exit_status, 0);

    // A ProcessResult that was never run is a failure under both, because exit_code defaults
    // to -1 rather than 0 (process_runner.h:17-20).
    sak::ProcessResult never;
    QCOMPARE(never.exit_code, -1);
    QCOMPARE(never.exit_status, 0);
    QVERIFY(!never.timed_out);
    QVERIFY(!never.cancelled);
    QVERIFY(!never.succeeded());
    QVERIFY(!never.completedSuccessfully());
}

// CODEX_REVIEW_4 H14: an elevated process must launch system tools by their
// System32-qualified absolute path, never a bare name a PATH/CWD-planted binary
// could satisfy. system32Path resolves under the real system directory.
void ProcessRunnerTests::system32Path_qualifiesUnderSystem32() {
#ifndef Q_OS_WIN
    QSKIP("System32 resolution is Windows-specific");
#else
    const QString netsh = sak::system32Path(QStringLiteral("netsh.exe"));
    QVERIFY(!netsh.isEmpty());
    QVERIFY(QDir::isAbsolutePath(netsh));
    QVERIFY2(netsh.endsWith(QStringLiteral("/System32/netsh.exe"), Qt::CaseInsensitive),
             qPrintable(netsh));
    QVERIFY2(QFileInfo::exists(netsh), qPrintable(netsh));

    const QString ps = sak::system32Path(QStringLiteral("WindowsPowerShell/v1.0/powershell.exe"));
    QVERIFY2(ps.endsWith(QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe"),
                         Qt::CaseInsensitive),
             qPrintable(ps));
    QVERIFY2(QFileInfo::exists(ps), qPrintable(ps));

    // The REFUSAL arm nothing exercised (process_runner.cpp:280-283, gating 288-290): a name
    // that could escape the resolved system directory is rejected outright -- empty return, no
    // join, no bare-name fallback -- so the caller must fail closed. Without this, dropping the
    // guard leaves every assertion above green while "../../Temp/netsh.exe" cleanPaths to
    // C:/Temp/netsh.exe and is handed to an elevated CreateProcess.
    QVERIFY(sak::system32Path(QString()).isEmpty());
    QVERIFY(sak::system32Path(QStringLiteral("..")).isEmpty());
    QVERIFY(sak::system32Path(QStringLiteral("../../Temp/netsh.exe")).isEmpty());
    QVERIFY(sak::system32Path(QStringLiteral("..\\Temp\\netsh.exe")).isEmpty());
    QVERIFY(sak::system32Path(QStringLiteral("C:/Temp/netsh.exe")).isEmpty());

    // The Windows-directory sibling (process_runner.cpp:303-318) resolves and refuses on
    // exactly the same terms, and had no coverage anywhere in the suite.
    const QString explorer = sak::windowsDirPath(QStringLiteral("explorer.exe"));
    QVERIFY2(QDir::isAbsolutePath(explorer), qPrintable(explorer));
    QVERIFY2(explorer.endsWith(QStringLiteral("/explorer.exe"), Qt::CaseInsensitive),
             qPrintable(explorer));
    QVERIFY2(QFileInfo::exists(explorer), qPrintable(explorer));
    QVERIFY(sak::windowsDirPath(QString()).isEmpty());
    QVERIFY(sak::windowsDirPath(QStringLiteral("../../Temp/explorer.exe")).isEmpty());
    QVERIFY(sak::windowsDirPath(QStringLiteral("C:/Temp/explorer.exe")).isEmpty());
#endif
}

// R5-P7-22: restore points, program enumeration, archiving and thermal polling all used
// to launch a BARE "powershell.exe", which CreateProcess resolves through a search order
// that puts the current directory ahead of System32 -- a planted powershell would then run
// with an elevated token. Every one of those call sites now goes through this one resolver,
// so it must hand back an ABSOLUTE path under the real system directory (never the bare
// name, and never a relative form a caller could pass straight to CreateProcess).
void ProcessRunnerTests::systemPowerShellPath_resolvesAbsoluteInterpreter() {
#ifndef Q_OS_WIN
    QSKIP("System32 resolution is Windows-specific");
#else
    const QString ps = sak::systemPowerShellPath();
    QVERIFY2(!ps.isEmpty(), "the system PowerShell must resolve on a healthy Windows install");
    QVERIFY2(ps != QStringLiteral("powershell.exe"), qPrintable(ps));
    QVERIFY2(QDir::isAbsolutePath(ps), qPrintable(ps));
    QVERIFY2(ps.endsWith(QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe"),
                         Qt::CaseInsensitive),
             qPrintable(ps));
    // Fail-closed contract: the resolver only ever returns a path that really exists, so a
    // non-empty result can be handed to CreateProcess without a PATH search.
    QVERIFY2(QFileInfo(ps).isFile(), qPrintable(ps));
    // It agrees with the generic System32 resolver used for netsh/reg/cmd/msiexec.
    QCOMPARE(ps, sak::system32Path(QStringLiteral("WindowsPowerShell/v1.0/powershell.exe")));
#endif
}

// ============================================================================
// R5-G14-17 process fault-injection seam
// ============================================================================

// The shared seam substitutes a chosen ProcessResult for the real launch through the ONE
// internal runner every launcher funnels through, so a caller's mid-operation failure path
// (non-zero exit, crash-exit, timeout) runs headless without a real child. Verified on
// runProcess AND runPowerShell (which reaches the same runner via runProcess), proving the
// seam covers every entry point, not just one.
void ProcessRunnerTests::processFaultInjector_substitutesResultForEveryLauncher() {
    int calls = 0;
    QString lastProgram;
    sak::ScopedProcessFaultInjector guard(
        [&](const QString& program, const QStringList&) -> std::optional<sak::ProcessResult> {
            ++calls;
            lastProgram = program;
            sak::ProcessResult injected;
            injected.exit_code = 7;
            injected.exit_status = 1;  // crash-exit
            injected.std_err = QStringLiteral("INJECTED-FAILURE");
            return injected;
        });

    // A command that would REALLY succeed (exit 0) instead returns the injected failure --
    // proving no real process ran.
    const auto direct = sak::runProcess("cmd.exe", {"/C", "exit /b 0"}, 10'000);
    QCOMPARE(direct.exit_code, 7);
    QCOMPARE(direct.exit_status, 1);
    QCOMPARE(direct.std_err, QStringLiteral("INJECTED-FAILURE"));
    QVERIFY(!direct.succeeded());

    // runPowerShell funnels through runProcess -> the same internal runner, so it is injected too.
    const auto shell = sak::runPowerShell("Write-Output 'would-succeed'", 10'000);
    QCOMPARE(shell.exit_code, 7);
    QCOMPARE(shell.std_err, QStringLiteral("INJECTED-FAILURE"));

    QCOMPARE(calls, 2);
#ifdef Q_OS_WIN
    // R5-P7-22: the launcher must hand the runner the ABSOLUTE System32 interpreter
    // (process_runner.cpp:263 + 272). The bare string "powershell.exe" ALSO satisfies
    // endsWith("powershell.exe"), so that check alone is green for exactly the hijackable
    // launch this contract exists to forbid.
    QVERIFY2(QDir::isAbsolutePath(lastProgram), qPrintable(lastProgram));
    QVERIFY2(lastProgram.endsWith(QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe"),
                                  Qt::CaseInsensitive),
             qPrintable(lastProgram));
    QCOMPARE(lastProgram, sak::systemPowerShellPath());
#else
    QVERIFY2(lastProgram.endsWith(QStringLiteral("powershell.exe"), Qt::CaseInsensitive),
             qPrintable(lastProgram));
#endif
}

// The injector must not leak and must be a controllable OVERLAY, not a permanent break of
// process execution. After the scoped guard ends, a REAL launch runs again; and an installed
// injector that returns nullopt lets the real launch through (pass-through). This is the
// non-vacuous half: without the disarm/pass-through paths these real launches would not run.
void ProcessRunnerTests::processFaultInjector_scopeRestoresRealLaunch() {
    {
        sak::ScopedProcessFaultInjector guard(
            [](const QString&, const QStringList&) -> std::optional<sak::ProcessResult> {
                sak::ProcessResult injected;
                injected.exit_code = 99;
                return injected;
            });
        QCOMPARE(sak::runProcess("cmd.exe", {"/C", "exit /b 0"}, 10'000).exit_code, 99);
    }
    // Guard destroyed -> real launch restored.
    const auto real = sak::runProcess("cmd.exe", {"/C", "exit /b 0"}, 10'000);
    QCOMPARE(real.exit_code, 0);
    QVERIFY(real.succeeded());

    // Nesting: the guard restores the PREVIOUS injector (process_runner.cpp:362-369), not
    // merely "none". Every check above runs with an empty outer state, so a destructor that
    // simply cleared the global would pass them all while silently disarming an enclosing
    // guard -- the enclosing test's remaining launches would then spawn REAL processes.
    {
        sak::ScopedProcessFaultInjector outer(
            [](const QString&, const QStringList&) -> std::optional<sak::ProcessResult> {
                sak::ProcessResult injected;
                injected.exit_code = 11;
                return injected;
            });
        QCOMPARE(sak::runProcess("cmd.exe", {"/C", "exit /b 0"}, 10'000).exit_code, 11);
        {
            sak::ScopedProcessFaultInjector inner(
                [](const QString&, const QStringList&) -> std::optional<sak::ProcessResult> {
                    sak::ProcessResult injected;
                    injected.exit_code = 22;
                    return injected;
                });
            QCOMPARE(sak::runProcess("cmd.exe", {"/C", "exit /b 0"}, 10'000).exit_code, 22);
        }
        // inner gone -> outer is back, NOT "no injector" (which would give the real 0).
        QCOMPARE(sak::runProcess("cmd.exe", {"/C", "exit /b 0"}, 10'000).exit_code, 11);
    }
    QCOMPARE(sak::runProcess("cmd.exe", {"/C", "exit /b 0"}, 10'000).exit_code, 0);

    // An armed injector that returns nullopt must let the real launch through.
    sak::ScopedProcessFaultInjector passthrough(
        [](const QString&, const QStringList&) -> std::optional<sak::ProcessResult> {
            return std::nullopt;
        });
    const auto passed = sak::runProcess("cmd.exe", {"/C", "exit /b 5"}, 10'000);
    QCOMPARE(passed.exit_code, 5);
}

QTEST_GUILESS_MAIN(ProcessRunnerTests)
#include "test_process_runner.moc"

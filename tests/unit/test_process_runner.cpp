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
};

// ============================================================================
// runProcess Tests
// ============================================================================

void ProcessRunnerTests::runProcess_echoCommand() {
    auto result = sak::runProcess("cmd.exe", {"/C", "echo Hello World"}, 10'000);

    QCOMPARE(result.exit_code, 0);
    QVERIFY(!result.timed_out);
    QVERIFY(!result.cancelled);
    QVERIFY(result.std_out.trimmed().contains("Hello World"));
}

void ProcessRunnerTests::runProcess_exitCode() {
    auto result = sak::runProcess("cmd.exe", {"/C", "exit /b 42"}, 10'000);

    QCOMPARE(result.exit_code, 42);
    QVERIFY(!result.timed_out);
}

void ProcessRunnerTests::runProcess_stderrCapture() {
    auto result = sak::runProcess("cmd.exe", {"/C", "echo error message 1>&2"}, 10'000);

    // stderr should contain the error message
    QVERIFY(result.std_err.contains("error message"));
}

void ProcessRunnerTests::runProcess_nonExistentProgram() {
    auto result = sak::runProcess("nonexistent_program_xyz_12345.exe", {}, 5000);

    // After BUG-13 fix: waitForStarted() detects launch failure immediately.
    // exit_code should be -1 and std_err should contain an error message.
    QCOMPARE(result.exit_code, -1);
    QVERIFY(!result.std_err.isEmpty());
}

void ProcessRunnerTests::runProcess_timeout() {
    // Start a long-running process with a short timeout
    auto result = sak::runProcess("cmd.exe", {"/C", "ping -n 3 127.0.0.1"}, 1000);

    QVERIFY(result.timed_out);
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
    QVERIFY(result.std_out.contains("PowerShell Test"));
}

void ProcessRunnerTests::runPowerShell_withNoProfile() {
    auto result = sak::runPowerShell("Write-Output $PSVersionTable.PSVersion.Major",
                                     10'000,
                                     true,   // no_profile
                                     true);  // bypass_policy

    QCOMPARE(result.exit_code, 0);
    QVERIFY(!result.std_out.trimmed().isEmpty());
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

QTEST_GUILESS_MAIN(ProcessRunnerTests)
#include "test_process_runner.moc"

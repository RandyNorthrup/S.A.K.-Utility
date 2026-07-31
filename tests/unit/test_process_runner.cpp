// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_process_runner.cpp
/// @brief Unit tests for process execution utilities

#include "sak/process_runner.h"

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

    // runPowerShell
    void runPowerShell_simpleScript();
    void runPowerShell_withNoProfile();
    void runPowerShell_scriptError();

    // B5-07: bounded output accumulation
    void appendCappedOutput_boundsAccumulation();

    // B5-05: strict success (cancellation counts as failure)
    void completedSuccessfully_strictOutcome();
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

QTEST_GUILESS_MAIN(ProcessRunnerTests)
#include "test_process_runner.moc"

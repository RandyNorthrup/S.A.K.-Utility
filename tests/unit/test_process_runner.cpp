// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_process_runner.cpp
/// @brief Unit tests for process execution utilities

#include <QtTest/QtTest>

#include "sak/process_runner.h"

class ProcessRunnerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // runProcess
    void runProcess_echoCommand();
    void runProcess_exitCode();
    void runProcess_stderrCapture();
    void runProcess_nonExistentProgram();
    void runProcess_timeout();
    void runProcess_cancellation();

    // runPowerShell
    void runPowerShell_simpleScript();
    void runPowerShell_withNoProfile();
    void runPowerShell_scriptError();
};

// ============================================================================
// runProcess Tests
// ============================================================================

void ProcessRunnerTests::runProcess_echoCommand()
{
    auto result = sak::runProcess(
        "cmd.exe", {"/C", "echo Hello World"}, 10000);

    QCOMPARE(result.exit_code, 0);
    QVERIFY(!result.timed_out);
    QVERIFY(!result.cancelled);
    QVERIFY(result.std_out.trimmed().contains("Hello World"));
}

void ProcessRunnerTests::runProcess_exitCode()
{
    auto result = sak::runProcess(
        "cmd.exe", {"/C", "exit /b 42"}, 10000);

    QCOMPARE(result.exit_code, 42);
    QVERIFY(!result.timed_out);
}

void ProcessRunnerTests::runProcess_stderrCapture()
{
    auto result = sak::runProcess(
        "cmd.exe", {"/C", "echo error message 1>&2"}, 10000);

    // stderr should contain the error message
    QVERIFY(result.std_err.contains("error message"));
}

void ProcessRunnerTests::runProcess_nonExistentProgram()
{
    auto result = sak::runProcess(
        "nonexistent_program_xyz_12345.exe", {}, 5000);

    // After BUG-13 fix: waitForStarted() detects launch failure immediately.
    // exit_code should be -1 and std_err should contain an error message.
    QCOMPARE(result.exit_code, -1);
    QVERIFY(!result.std_err.isEmpty());
}

void ProcessRunnerTests::runProcess_timeout()
{
    // Start a long-running process with a short timeout
    auto result = sak::runProcess(
        "cmd.exe", {"/C", "ping -n 3 127.0.0.1"}, 1000);

    QVERIFY(result.timed_out);
}

void ProcessRunnerTests::runProcess_cancellation()
{
    bool shouldCancel = true;
    auto result = sak::runProcess(
        "cmd.exe", {"/C", "ping -n 3 127.0.0.1"}, 10000,
        [&shouldCancel]() -> bool { return shouldCancel; });

    QVERIFY(result.cancelled);
}

// ============================================================================
// runPowerShell Tests
// ============================================================================

void ProcessRunnerTests::runPowerShell_simpleScript()
{
    auto result = sak::runPowerShell(
        "Write-Output 'PowerShell Test'", 10000);

    QCOMPARE(result.exit_code, 0);
    QVERIFY(!result.timed_out);
    QVERIFY(result.std_out.contains("PowerShell Test"));
}

void ProcessRunnerTests::runPowerShell_withNoProfile()
{
    auto result = sak::runPowerShell(
        "Write-Output $PSVersionTable.PSVersion.Major", 10000,
        true,  // no_profile
        true); // bypass_policy

    QCOMPARE(result.exit_code, 0);
    QVERIFY(!result.std_out.trimmed().isEmpty());
}

void ProcessRunnerTests::runPowerShell_scriptError()
{
    auto result = sak::runPowerShell(
        "Get-Item 'C:\\NonExistent_xyz_12345' -ErrorAction Stop", 10000);

    // Should fail with non-zero exit code
    QVERIFY(result.exit_code != 0);
}

QTEST_GUILESS_MAIN(ProcessRunnerTests)
#include "test_process_runner.moc"

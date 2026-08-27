// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_orchestrator.h"
#include "sak/ai/ai_workflow_placeholders.h"
#include "sak/ai/ai_workflow_powershell_tool_runner.h"

#include <QtTest/QtTest>

class AiWorkflowPowerShellToolRunnerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void rejectsMissingCommand();
    void requiresExecutor();
    void guardBlocksBadPowerShell();
    void sensitiveCommandRequiresConfirmation();
    void successfulRunRecordsRedactedCommand();
    void clampsCallerControlledOutputCap();
    void singleQuotedModeEscapesEmbeddedQuotes();
    void templateSingleQuoteSafetyGatesRawPlaceholders();
    void templateSingleQuoteScannerLexesQuotingContext();
    void cmdTemplateRejectsEveryPlaceholder();
};

void AiWorkflowPowerShellToolRunnerTests::rejectsMissingCommand() {
    const QJsonObject result = sak::ai::AiWorkflowPowerShellToolRunner::run(QJsonObject{}, {}, {});

    QVERIFY(!result.value(QStringLiteral("success")).toBool(true));
    QVERIFY(result.value(QStringLiteral("error_message"))
                .toString()
                .contains(QStringLiteral("requires explicit arguments.command")));
}

void AiWorkflowPowerShellToolRunnerTests::requiresExecutor() {
    sak::ai::AiWorkflowPowerShellToolCallbacks callbacks;
    callbacks.allocate_command_id = [] {
        return QStringLiteral("cmd_test");
    };

    const QJsonObject result = sak::ai::AiWorkflowPowerShellToolRunner::run(
        QJsonObject{{QStringLiteral("command"), QStringLiteral("Write-Output ok")}}, {}, callbacks);

    QCOMPARE(result.value(QStringLiteral("success")).toBool(true), false);
    QCOMPARE(result.value(QStringLiteral("error_message")).toString(),
             QStringLiteral("Workflow PowerShell executor is not configured"));

    // Sibling arm of the same two-check validator: a missing command-id allocator must refuse
    // with its own message. No test covered it, so deleting that check left the suite green
    // while a caller without an allocator would invoke an empty std::function.
    sak::ai::AiWorkflowPowerShellToolCallbacks no_allocator;
    no_allocator.execute_powershell = [](const sak::ai::AiCommandRequest&, const QString&) {
        return sak::ai::AiCommandResult{.started = true, .exit_code = 0};
    };
    const QJsonObject allocator_result = sak::ai::AiWorkflowPowerShellToolRunner::run(
        QJsonObject{{QStringLiteral("command"), QStringLiteral("Write-Output ok")}},
        {},
        no_allocator);
    QCOMPARE(allocator_result.value(QStringLiteral("success")).toBool(true), false);
    QCOMPARE(allocator_result.value(QStringLiteral("error_message")).toString(),
             QStringLiteral("Workflow PowerShell command-id allocator is not configured"));
}

void AiWorkflowPowerShellToolRunnerTests::guardBlocksBadPowerShell() {
    sak::ai::AiWorkflowPowerShellToolCallbacks callbacks;
    callbacks.allocate_command_id = [] {
        return QStringLiteral("cmd_guard");
    };
    bool executed = false;
    callbacks.execute_powershell = [&executed](const sak::ai::AiCommandRequest&, const QString&) {
        executed = true;
        return sak::ai::AiCommandResult{.started = true, .exit_code = 0};
    };

    const QJsonObject result = sak::ai::AiWorkflowPowerShellToolRunner::run(
        QJsonObject{{QStringLiteral("command"), QStringLiteral("$pid=1")}}, {}, callbacks);

    // A blocked command must be refused BEFORE the executor, and with the $PID guard's own
    // text: commandGuardBlockError has four arms that all return a non-empty string, so
    // "some error came back" does not prove this guard fired.
    QVERIFY(!executed);
    QCOMPARE(result.value(QStringLiteral("success")).toBool(true), false);
    QCOMPARE(result.value(QStringLiteral("error_message")).toString(),
             QStringLiteral("Blocked PowerShell $PID mutation. $PID is a read-only automatic "
                            "variable; use a different variable such as $processId or "
                            "$windowProcessId."));
}

void AiWorkflowPowerShellToolRunnerTests::sensitiveCommandRequiresConfirmation() {
    sak::ai::AiWorkflowPowerShellToolCallbacks callbacks;
    callbacks.allocate_command_id = [] {
        return QStringLiteral("cmd_sensitive");
    };
    callbacks.execute_powershell = [](const sak::ai::AiCommandRequest&, const QString&) {
        return sak::ai::AiCommandResult{.started = true, .exit_code = 0};
    };

    const QJsonObject result = sak::ai::AiWorkflowPowerShellToolRunner::run(
        QJsonObject{{QStringLiteral("command"),
                     QStringLiteral(
                         "Start-Process 'C:\\SAK\\data\\temp\\chocolatey\\pkg\\1.0\\setup.exe'")}},
        {},
        callbacks);

    QCOMPARE(result.value(QStringLiteral("success")).toBool(true), false);
    QCOMPARE(result.value(QStringLiteral("error_message")).toString(),
             QStringLiteral("Sensitive workflow command confirmation callback is not configured"));

    // The refusal above only proves the "no callback" arm. With a callback wired the decision
    // must be HONORED, and the prompt must carry the reason plus the real command (the preview
    // is caller-controlled and can differ from what actually runs).
    const QString sensitive_command =
        QStringLiteral("Start-Process 'C:\\SAK\\data\\temp\\chocolatey\\pkg\\1.0\\setup.exe'");
    bool executed = false;
    QString seen_title;
    QString seen_preview;
    bool seen_risky = false;
    callbacks.execute_powershell = [&executed](const sak::ai::AiCommandRequest&, const QString&) {
        executed = true;
        return sak::ai::AiCommandResult{.started = true, .exit_code = 0};
    };
    callbacks.confirm = [&seen_title, &seen_preview, &seen_risky](const QString& title,
                                                                  const QString& preview,
                                                                  bool risky) {
        seen_title = title;
        seen_preview = preview;
        seen_risky = risky;
        return false;
    };
    const QJsonObject declined = sak::ai::AiWorkflowPowerShellToolRunner::run(
        QJsonObject{{QStringLiteral("command"), sensitive_command}},
        QStringLiteral("Install the app"),
        callbacks);
    QCOMPARE(declined.value(QStringLiteral("success")).toBool(true), false);
    QCOMPARE(declined.value(QStringLiteral("error_message")).toString(),
             QStringLiteral("User declined sensitive workflow command"));
    QCOMPARE(executed, false);
    QCOMPARE(seen_title, QStringLiteral("Sensitive Workflow Command"));
    QCOMPARE(seen_risky, true);
    QCOMPARE(seen_preview,
             QStringLiteral("Cached package installer execution requested after package-manager "
                            "handling. Continue only with explicit user approval and "
                            "verification evidence.\n\nInstall the app\n\nCommand: ") +
                 sensitive_command);

    // ...and approval lets it through, so the gate is a real decision, not a hard block.
    callbacks.confirm = [](const QString&, const QString&, bool) {
        return true;
    };
    const QJsonObject approved = sak::ai::AiWorkflowPowerShellToolRunner::run(
        QJsonObject{{QStringLiteral("command"), sensitive_command}}, {}, callbacks);
    QCOMPARE(approved.value(QStringLiteral("success")).toBool(false), true);
    QCOMPARE(executed, true);
}

void AiWorkflowPowerShellToolRunnerTests::successfulRunRecordsRedactedCommand() {
    QString recorded_preview;
    QJsonObject recorded_result;
    sak::ai::AiWorkflowPowerShellToolCallbacks callbacks;
    callbacks.allocate_command_id = [] {
        return QStringLiteral("cmd_success");
    };
    callbacks.execute_powershell = [](const sak::ai::AiCommandRequest& request, const QString&) {
        sak::ai::AiCommandResult result;
        result.started = true;
        result.exit_code = request.command.contains(QStringLiteral("Write-Output")) ? 0 : 1;
        result.stdout_text = QStringLiteral("ok");
        return result;
    };
    callbacks.record_command = [&recorded_preview, &recorded_result](const QString& preview,
                                                                     const QJsonObject& result) {
        recorded_preview = preview;
        recorded_result = result;
    };

    const QJsonObject result = sak::ai::AiWorkflowPowerShellToolRunner::run(
        QJsonObject{{QStringLiteral("command"), QStringLiteral("Write-Output token=secret-value")}},
        QStringLiteral("safe preview"),
        callbacks);

    QVERIFY(result.value(QStringLiteral("success")).toBool(false));
    QCOMPARE(result.value(QStringLiteral("command_id")).toString(), QStringLiteral("cmd_success"));
    QCOMPARE(result.value(QStringLiteral("command")).toString(),
             QStringLiteral("Write-Output token=[redacted]"));
    // What is recorded must be the SAME payload that is returned, not a reduced summary.
    QCOMPARE(recorded_result, result);
    QCOMPARE(result.value(QStringLiteral("stdout")).toString(), QStringLiteral("ok"));
    QCOMPARE(result.value(QStringLiteral("exit_code")).toInt(-1), 0);
    QCOMPARE(result.value(QStringLiteral("preview")).toString(), QStringLiteral("safe preview"));
    QCOMPARE(result.value(QStringLiteral("requires_admin")).toBool(true), false);

    // "success" is an AND over five result fields, not just `started`. A process that crashed
    // with a zero exit code (exit_status != 0) and one that timed out must both report false.
    callbacks.execute_powershell = [](const sak::ai::AiCommandRequest&, const QString&) {
        return sak::ai::AiCommandResult{.started = true, .exit_code = 0, .exit_status = 1};
    };
    const QJsonObject crashed = sak::ai::AiWorkflowPowerShellToolRunner::run(
        QJsonObject{{QStringLiteral("command"), QStringLiteral("Write-Output ok")}},
        QStringLiteral("safe preview"),
        callbacks);
    QCOMPARE(crashed.value(QStringLiteral("success")).toBool(true), false);

    callbacks.execute_powershell = [](const sak::ai::AiCommandRequest&, const QString&) {
        return sak::ai::AiCommandResult{.started = true, .timed_out = true, .exit_code = 0};
    };
    const QJsonObject timed_out = sak::ai::AiWorkflowPowerShellToolRunner::run(
        QJsonObject{{QStringLiteral("command"), QStringLiteral("Write-Output ok")}},
        QStringLiteral("safe preview"),
        callbacks);
    QCOMPARE(timed_out.value(QStringLiteral("success")).toBool(true), false);
}

void AiWorkflowPowerShellToolRunnerTests::clampsCallerControlledOutputCap() {
    int seen_cap = -1;
    sak::ai::AiCommandRequest seen_request;
    sak::ai::AiWorkflowPowerShellToolCallbacks callbacks;
    callbacks.allocate_command_id = [] {
        return QStringLiteral("cmd_cap");
    };
    callbacks.execute_powershell =
        [&seen_cap, &seen_request](const sak::ai::AiCommandRequest& request, const QString&) {
            seen_cap = request.max_output_bytes;
            seen_request = request;
            return sak::ai::AiCommandResult{.started = true, .exit_code = 0};
        };

    const sak::ai::AiWorkflowPowerShellToolOptions options;

    // A near-INT_MAX request must be clamped down to the hard ceiling, not honored.
    const QJsonObject high = sak::ai::AiWorkflowPowerShellToolRunner::run(
        QJsonObject{{QStringLiteral("command"), QStringLiteral("Write-Output ok")},
                    {QStringLiteral("max_output_bytes"), 2'147'483'647}},
        {},
        callbacks);
    QVERIFY(high.value(QStringLiteral("success")).toBool(false));
    QCOMPARE(seen_cap, options.max_output_bytes);
    // THE CLAMPED VALUE MUST BE ONE THE BROKER ACCEPTS, which is the whole point of clamping
    // and is what this assertion used to miss. It pinned the literal 64 * 1024 * 1024 instead,
    // and that literal was FOUR TIMES what ExecutionBroker accepts: the runner reduced a large
    // request to 64 MiB, the broker refused it outright ("max_output_bytes 67108864 is outside
    // the range 1-16777216"), and the command never ran. The test asserted the drift.
    //
    // Asking the broker's own precondition function -- rather than naming a number here --
    // means a future widening of the runner's ceiling past the enforcer's turns this red,
    // whereas any assertion written as a literal would simply be updated to match.
    QVERIFY2(sak::ai::aiCommandPreconditionError(sak::ai::AiCommandTarget::PowerShell, seen_request)
                 .isEmpty(),
             qPrintable(sak::ai::aiCommandPreconditionError(sak::ai::AiCommandTarget::PowerShell,
                                                            seen_request)));
    // And pin the ceiling to the shared constant, so shrinking it silently still fails.
    QCOMPARE(options.max_output_bytes, sak::ai::kAiCommandOutputBytesCeiling);

    // A tiny request must be raised to the floor.
    const QJsonObject low = sak::ai::AiWorkflowPowerShellToolRunner::run(
        QJsonObject{{QStringLiteral("command"), QStringLiteral("Write-Output ok")},
                    {QStringLiteral("max_output_bytes"), 1}},
        {},
        callbacks);
    QVERIFY(low.value(QStringLiteral("success")).toBool(false));
    QCOMPARE(seen_cap, options.min_output_bytes);
    QCOMPARE(options.min_output_bytes, 1024);

    // Third arm of the same clamp: an ABSENT max_output_bytes must land on the default,
    // not on the floor or the ceiling.
    const QJsonObject omitted = sak::ai::AiWorkflowPowerShellToolRunner::run(
        QJsonObject{{QStringLiteral("command"), QStringLiteral("Write-Output ok")}}, {}, callbacks);
    QVERIFY(omitted.value(QStringLiteral("success")).toBool(false));
    QCOMPARE(seen_cap, options.default_output_bytes);
    QCOMPARE(options.default_output_bytes, 512 * 1024);
}

void AiWorkflowPowerShellToolRunnerTests::singleQuotedModeEscapesEmbeddedQuotes() {
    // B12-03 / B1-05: a placeholder value substituted into a PowerShell single-quoted literal
    // must have its single quotes doubled, so a malicious value cannot break out of the quotes
    // and inject a command. Raw mode (used only for non-shell contexts like prompts) must not
    // escape, proving the escaping is a deliberate, mode-gated behavior.
    sak::ai::AiWorkflowPhaseContext context;
    context.user_message = QStringLiteral("x'; Remove-Item C:\\ -Recurse; '");
    const QString tmpl = QStringLiteral("Write-Output '${user_message}'");

    const QString quoted = sak::ai::substituteWorkflowPlaceholders(
        tmpl, context, sak::ai::WorkflowPlaceholderMode::PowerShellSingleQuoted);
    // Every embedded single quote is doubled; the injected value stays inside one literal.
    QCOMPARE(quoted, QStringLiteral("Write-Output 'x''; Remove-Item C:\\ -Recurse; '''"));

    const QString raw = sak::ai::substituteWorkflowPlaceholders(
        tmpl, context, sak::ai::WorkflowPlaceholderMode::Raw);
    // Raw mode leaves the single quotes intact (never used to build a shell command).
    QCOMPARE(raw, QStringLiteral("Write-Output 'x'; Remove-Item C:\\ -Recurse; ''"));
    QVERIFY(quoted != raw);
}

void AiWorkflowPowerShellToolRunnerTests::templateSingleQuoteSafetyGatesRawPlaceholders() {
    // CODEX_REVIEW_4 M-B1-21: the single-quote escaping is only sufficient when every ${...}
    // placeholder sits INSIDE a single-quoted literal. Validate that invariant at load.
    using sak::ai::powerShellCommandTemplateIsSingleQuoteSafe;

    // Safe: placeholder inside '...', including the $var='${...}' capture the bundled templates
    // use.
    QVERIFY(powerShellCommandTemplateIsSingleQuoteSafe(
        QStringLiteral("Write-Output '${user_message}'")));
    QVERIFY(powerShellCommandTemplateIsSingleQuoteSafe(QStringLiteral("$name='${app_name}'; foo")));
    // '' escaped quotes inside a literal keep the span open, so a following placeholder is safe.
    QVERIFY(powerShellCommandTemplateIsSingleQuoteSafe(QStringLiteral("$x='a''b ${app_name}'")));
    // No placeholders at all -> safe (PowerShell $vars/${braced:with:colon} are not our grammar).
    QVERIFY(
        powerShellCommandTemplateIsSingleQuoteSafe(QStringLiteral("Write-Output $LASTEXITCODE")));

    // Unsafe: placeholder OUTSIDE any single-quoted literal -> raw injection risk, rejected.
    QString error;
    QVERIFY(!powerShellCommandTemplateIsSingleQuoteSafe(
        QStringLiteral("Write-Output ${user_message}"), &error));
    QCOMPARE(error,
             QStringLiteral("PowerShell workflow command places placeholder '${user_message}' "
                            "outside a single-quoted literal, so its value would be injected "
                            "unescaped"));
    // Unsafe: placeholder after the single-quoted span has closed.
    QVERIFY(!powerShellCommandTemplateIsSingleQuoteSafe(
        QStringLiteral("$a='x'; Invoke-Expression ${user_message}")));
}

void AiWorkflowPowerShellToolRunnerTests::templateSingleQuoteScannerLexesQuotingContext() {
    // R5 p1_ai-3: the placement check used to toggle on EVERY single quote, so an
    // apostrophe that is not a delimiter (one inside a double-quoted string, a comment or a
    // here-string) flipped the scanner's idea of the quoting context and a bare placeholder
    // passed. The scanner now lexes the constructs it accepts and refuses the rest.
    using sak::ai::powerShellCommandTemplateIsSingleQuoteSafe;

    // The bypass: the ' inside "'" is string CONTENT, so ${user_message} really is bare code.
    QString error;
    QVERIFY(!powerShellCommandTemplateIsSingleQuoteSafe(QStringLiteral("$x=\"'\"; ${user_message}"),
                                                        &error));
    QCOMPARE(error,
             QStringLiteral("PowerShell workflow command places placeholder '${user_message}' "
                            "outside a single-quoted literal, so its value would be injected "
                            "unescaped"));
    // Same shape with the quote-carrying string reached through an escaped quote.
    QVERIFY(!powerShellCommandTemplateIsSingleQuoteSafe(
        QStringLiteral("Write-Output \"it`'s\"; ${user_message}")));
    // A double-quoted string that legitimately closes leaves the scanner in command
    // position, so a following bare placeholder is still rejected.
    QVERIFY(!powerShellCommandTemplateIsSingleQuoteSafe(
        QStringLiteral("Write-Output \"a\"; ${app_name}")));

    // A double-quoted string BEFORE the placeholder is lexed exactly, so the bundled
    // "quote the value, then use it in a -like filter" shape keeps working.
    QVERIFY(powerShellCommandTemplateIsSingleQuoteSafe(
        QStringLiteral("Write-Output \"scan\"; $name='${app_name}'; Get-Item")));
    // A backtick escapes the next character, so an escaped quote never opens a literal.
    QVERIFY(!powerShellCommandTemplateIsSingleQuoteSafe(
        QStringLiteral("Write-Output `'; ${user_message}")));
    // A backtick immediately before the placeholder escapes its '$': the value would be
    // spliced into command position, so it must not be accepted as "inside a literal".
    QVERIFY(!powerShellCommandTemplateIsSingleQuoteSafe(QStringLiteral("Write-Output `${x}")));

    // Constructs the scanner does not lex are refused rather than guessed at (fail closed).
    QString construct_error;
    QVERIFY(!powerShellCommandTemplateIsSingleQuoteSafe(
        QStringLiteral("# it's fine\n$name='${app_name}'"), &construct_error));
    QCOMPARE(construct_error,
             QStringLiteral("PowerShell workflow command uses a comment before placeholder "
                            "'${app_name}', so the placeholder's quoting context cannot be "
                            "proven"));
    QString block_comment_error;
    QVERIFY(!powerShellCommandTemplateIsSingleQuoteSafe(
        QStringLiteral("<# it's fine #>$name='${app_name}'"), &block_comment_error));
    QCOMPARE(block_comment_error,
             QStringLiteral("PowerShell workflow command uses a comment before placeholder "
                            "'${app_name}', so the placeholder's quoting context cannot be "
                            "proven"));
    QString here_string_error;
    QVERIFY(!powerShellCommandTemplateIsSingleQuoteSafe(
        QStringLiteral("$t=@'\nit's\n'@; $name='${app_name}'"), &here_string_error));
    QCOMPARE(here_string_error,
             QStringLiteral("PowerShell workflow command uses a here-string before placeholder "
                            "'${app_name}', so the placeholder's quoting context cannot be "
                            "proven"));
    QString subexpression_error;
    QVERIFY(!powerShellCommandTemplateIsSingleQuoteSafe(
        QStringLiteral("Write-Output \"$(Get-Date -Format \"'\")\"; $n='${app_name}'"),
        &subexpression_error));
    QCOMPARE(subexpression_error,
             QStringLiteral("PowerShell workflow command uses a $( ) subexpression inside a "
                            "double-quoted string before placeholder '${app_name}', so the "
                            "placeholder's quoting context cannot be proven"));
}

void AiWorkflowPowerShellToolRunnerTests::cmdTemplateRejectsEveryPlaceholder() {
    // R5 p1_ai-2: run_cmd commands were substituted in Raw mode with no placement check and
    // no escaping. cmd.exe has no literal-quoting construct that makes an arbitrary value
    // inert, so a placeholder is rejected outright instead of escaped-and-hoped.
    using sak::ai::cmdCommandTemplateIsPlaceholderFree;

    QVERIFY(cmdCommandTemplateIsPlaceholderFree(QStringLiteral("ipconfig /all")));
    QVERIFY(cmdCommandTemplateIsPlaceholderFree(QString()));
    // %VAR% is cmd.exe's own expansion, not the workflow placeholder grammar.
    QVERIFY(cmdCommandTemplateIsPlaceholderFree(QStringLiteral("echo %PATH%")));

    QString error;
    QVERIFY(!cmdCommandTemplateIsPlaceholderFree(QStringLiteral("echo ${user_message}"), &error));
    QVERIFY(error.contains(QStringLiteral("${user_message}")));
    // Quoting the placeholder does NOT make it safe in cmd.exe, so it is rejected too.
    QVERIFY(!cmdCommandTemplateIsPlaceholderFree(QStringLiteral("findstr \"${app_name}\" a.txt")));
    QVERIFY(!cmdCommandTemplateIsPlaceholderFree(QStringLiteral("dir ${result_scan_path}")));
}

QTEST_GUILESS_MAIN(AiWorkflowPowerShellToolRunnerTests)
#include "test_ai_workflow_powershell_tool_runner.moc"

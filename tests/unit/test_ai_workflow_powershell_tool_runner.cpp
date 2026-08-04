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

    QVERIFY(!result.value(QStringLiteral("success")).toBool(true));
    QVERIFY(result.value(QStringLiteral("error_message"))
                .toString()
                .contains(QStringLiteral("executor is not configured")));
}

void AiWorkflowPowerShellToolRunnerTests::guardBlocksBadPowerShell() {
    sak::ai::AiWorkflowPowerShellToolCallbacks callbacks;
    callbacks.allocate_command_id = [] {
        return QStringLiteral("cmd_guard");
    };
    callbacks.execute_powershell = [](const sak::ai::AiCommandRequest&, const QString&) {
        return sak::ai::AiCommandResult{.started = true, .exit_code = 0};
    };

    const QJsonObject result = sak::ai::AiWorkflowPowerShellToolRunner::run(
        QJsonObject{{QStringLiteral("command"), QStringLiteral("$pid=1")}}, {}, callbacks);

    QVERIFY(!result.value(QStringLiteral("success")).toBool(true));
    QVERIFY(result.value(QStringLiteral("error_message"))
                .toString()
                .contains(QStringLiteral("$PID mutation")));
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

    QVERIFY(!result.value(QStringLiteral("success")).toBool(true));
    QVERIFY(result.value(QStringLiteral("error_message"))
                .toString()
                .contains(QStringLiteral("confirmation callback is not configured")));
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
    QVERIFY(!result.value(QStringLiteral("command"))
                 .toString()
                 .contains(QStringLiteral("secret-value")));
    QCOMPARE(recorded_preview, QStringLiteral("safe preview"));
    QVERIFY(recorded_result.value(QStringLiteral("success")).toBool(false));
}

void AiWorkflowPowerShellToolRunnerTests::clampsCallerControlledOutputCap() {
    int seen_cap = -1;
    sak::ai::AiWorkflowPowerShellToolCallbacks callbacks;
    callbacks.allocate_command_id = [] {
        return QStringLiteral("cmd_cap");
    };
    callbacks.execute_powershell = [&seen_cap](const sak::ai::AiCommandRequest& request,
                                               const QString&) {
        seen_cap = request.max_output_bytes;
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

    // A tiny request must be raised to the floor.
    const QJsonObject low = sak::ai::AiWorkflowPowerShellToolRunner::run(
        QJsonObject{{QStringLiteral("command"), QStringLiteral("Write-Output ok")},
                    {QStringLiteral("max_output_bytes"), 1}},
        {},
        callbacks);
    QVERIFY(low.value(QStringLiteral("success")).toBool(false));
    QCOMPARE(seen_cap, options.min_output_bytes);
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
    QVERIFY(quoted.contains(QStringLiteral("x''; Remove-Item C:\\ -Recurse; ''")));

    const QString raw = sak::ai::substituteWorkflowPlaceholders(
        tmpl, context, sak::ai::WorkflowPlaceholderMode::Raw);
    // Raw mode leaves the single quotes intact (never used to build a shell command).
    QVERIFY(raw.contains(QStringLiteral("x'; Remove-Item C:\\ -Recurse; '")));
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
    QVERIFY(!error.isEmpty());
    // Unsafe: placeholder after the single-quoted span has closed.
    QVERIFY(!powerShellCommandTemplateIsSingleQuoteSafe(
        QStringLiteral("$a='x'; Invoke-Expression ${user_message}")));
}

QTEST_GUILESS_MAIN(AiWorkflowPowerShellToolRunnerTests)
#include "test_ai_workflow_powershell_tool_runner.moc"

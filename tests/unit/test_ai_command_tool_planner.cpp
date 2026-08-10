// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_command_tool_planner.h"

#include <QDir>
#include <QJsonArray>
#include <QtTest/QtTest>

class AiCommandToolPlannerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void buildsPowerShellPlanWithPolicy();
    void buildsProcessPlanWithProgramPreview();
    void rejectsRelativeProcessProgram();
    void rejectsUnresolvableProcessProgram();
    void processPreviewQuotesAmbiguousArgs();
    void marksRiskyCommandAndPolicyDenial();
    void carriesPidMutationGuardBlock();
    void carriesChecksumBypassBlock();
};

void AiCommandToolPlannerTests::buildsPowerShellPlanWithPolicy() {
    QJsonObject args;
    args[QStringLiteral("command")] = QStringLiteral("Get-Date");

    const auto plan =
        sak::ai::AiCommandToolPlanner::buildPlan(QStringLiteral("run_powershell"),
                                                 args,
                                                 sak::ai::AiToolPolicy::ReadOnlyPc,
                                                 sak::ai::AiCommandToolPlanner::Options{4096});

    QCOMPARE(plan.shell_label, QStringLiteral("PowerShell"));
    QCOMPARE(plan.preview, QStringLiteral("Get-Date"));
    QCOMPARE(plan.request.command, QStringLiteral("Get-Date"));
    QCOMPARE(plan.request.max_output_bytes, 4096);
    QVERIFY(plan.policy_decision.allowed);
    QVERIFY(!plan.risky_change);
    QVERIFY(plan.guard_block_error.isEmpty());
}

void AiCommandToolPlannerTests::buildsProcessPlanWithProgramPreview() {
    QJsonObject args;
    // A bare name (cmd.exe is present under System32 on every Windows install).
    args[QStringLiteral("program")] = QStringLiteral("cmd.exe");
    args[QStringLiteral("arguments")] = QJsonArray{QStringLiteral("a.txt")};

    const auto plan = sak::ai::AiCommandToolPlanner::buildPlan(QStringLiteral("run_process"),
                                                               args,
                                                               sak::ai::AiToolPolicy::ReadOnlyPc);

    QCOMPARE(plan.shell_label, QStringLiteral("Process"));
    QCOMPARE(plan.request.arguments, QStringList{QStringLiteral("a.txt")});
#ifndef Q_OS_WIN
    QSKIP("bare-name resolution to an absolute path is Windows-specific");
#else
    // F43: the bare program name is resolved to an absolute, verified path AT PLAN TIME, and
    // both the stored request AND the preview (the text the human approves) name that resolved
    // binary -- not the bare string the broker would otherwise resolve against the process
    // search order only AFTER the user approved.
    QVERIFY(plan.guard_block_error.isEmpty());
    QVERIFY2(QDir::isAbsolutePath(plan.request.program), qPrintable(plan.request.program));
    QVERIFY2(plan.request.program.endsWith(QStringLiteral("cmd.exe"), Qt::CaseInsensitive),
             qPrintable(plan.request.program));
    QVERIFY(plan.preview.contains(QStringLiteral("cmd.exe"), Qt::CaseInsensitive));
    QVERIFY(plan.preview.contains(QStringLiteral("a.txt")));
    // A direct process launch can never be proven safe from its command line, so ReadOnlyPc
    // refuses it even after the program resolves cleanly. The plan is still built (request +
    // preview populated) for display.
    QVERIFY(!plan.policy_decision.allowed);
#endif
}

void AiCommandToolPlannerTests::rejectsRelativeProcessProgram() {
    QJsonObject args;
    // A working-directory-relative path has no defensible base and would launch CWD-relative;
    // it must be refused at plan time rather than resolved against the current directory.
    args[QStringLiteral("program")] = QStringLiteral("sub/planted.exe");

    const auto plan = sak::ai::AiCommandToolPlanner::buildPlan(QStringLiteral("run_process"),
                                                               args,
                                                               sak::ai::AiToolPolicy::ReadOnlyPc);

    QVERIFY(!plan.request.validation_error.isEmpty());
    QVERIFY(plan.guard_block_error.contains(QStringLiteral("relative")));
    QVERIFY(!plan.policy_decision.allowed);
}

void AiCommandToolPlannerTests::rejectsUnresolvableProcessProgram() {
    QJsonObject args;
    // A bare name that resolves nowhere under System32 or the absolute PATH entries fails
    // closed instead of being handed to the broker to resolve after approval.
    args[QStringLiteral("program")] = QStringLiteral("definitely-not-a-real-sak-test-binary.exe");

    const auto plan = sak::ai::AiCommandToolPlanner::buildPlan(QStringLiteral("run_process"),
                                                               args,
                                                               sak::ai::AiToolPolicy::ReadOnlyPc);

    QVERIFY(!plan.request.validation_error.isEmpty());
    QVERIFY(plan.guard_block_error.contains(QStringLiteral("Cannot resolve")));
    QVERIFY(!plan.policy_decision.allowed);
}

void AiCommandToolPlannerTests::processPreviewQuotesAmbiguousArgs() {
    QJsonObject args;
    args[QStringLiteral("program")] = QStringLiteral("C:\\Program Files\\app.exe");
    args[QStringLiteral("arguments")] = QJsonArray{QStringLiteral("hello world"),
                                                   QStringLiteral("plain"),
                                                   QStringLiteral("tab\there")};

    const auto plan = sak::ai::AiCommandToolPlanner::buildPlan(QStringLiteral("run_process"),
                                                               args,
                                                               sak::ai::AiToolPolicy::ReadOnlyPc);

    // A program path containing a space is quoted as a single token (its backslash
    // path separators are preserved verbatim, not doubled).
    QVERIFY(plan.preview.contains(QStringLiteral("\"C:\\Program Files\\app.exe\"")));
    // An argument with a space is quoted so it cannot split into two arguments.
    QVERIFY(plan.preview.contains(QStringLiteral("\"hello world\"")));
    // A plain argument is left unquoted.
    QVERIFY(plan.preview.contains(QStringLiteral(" plain ")));
    // A control character (tab) is rendered visibly, never emitted raw.
    QVERIFY(!plan.preview.contains(QLatin1Char('\t')));
    QVERIFY(plan.preview.contains(QStringLiteral("\"tab\\there\"")));
}

void AiCommandToolPlannerTests::marksRiskyCommandAndPolicyDenial() {
    QJsonObject args;
    args[QStringLiteral("command")] = QStringLiteral("Remove-Item C:\\temp\\x -Recurse");

    const auto plan = sak::ai::AiCommandToolPlanner::buildPlan(QStringLiteral("run_powershell"),
                                                               args,
                                                               sak::ai::AiToolPolicy::ReadOnlyPc);

    QVERIFY(plan.risky_change);
    QVERIFY(!plan.policy_decision.allowed);
    QVERIFY(plan.policy_decision.risky_change);
}

void AiCommandToolPlannerTests::carriesPidMutationGuardBlock() {
    QJsonObject args;
    args[QStringLiteral("command")] =
        QStringLiteral("$pid=0; [void][Win32]::GetWindowThreadProcessId($hWnd,[ref]$pid)");

    const auto plan = sak::ai::AiCommandToolPlanner::buildPlan(QStringLiteral("run_powershell"),
                                                               args,
                                                               sak::ai::AiToolPolicy::ReadOnlyPc);

    QVERIFY(plan.guard_block_error.contains(QStringLiteral("$PID mutation")));
}

void AiCommandToolPlannerTests::carriesChecksumBypassBlock() {
    QJsonObject args;
    args[QStringLiteral("command")] =
        QStringLiteral("choco install superantispyware -y --ignore-checksums");

    const auto plan = sak::ai::AiCommandToolPlanner::buildPlan(
        QStringLiteral("run_powershell"), args, sak::ai::AiToolPolicy::ExclusiveMutatingExecutor);

    QVERIFY(plan.guard_block_error.contains(QStringLiteral("checksum bypass")));
    QVERIFY(plan.guard_approval_reason.isEmpty());
    QVERIFY(plan.policy_decision.allowed);
    QVERIFY(plan.policy_decision.requires_exclusive_lease);
}

QTEST_GUILESS_MAIN(AiCommandToolPlannerTests)
#include "test_ai_command_tool_planner.moc"

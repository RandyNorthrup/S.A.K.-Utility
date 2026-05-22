// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_command_tool_planner.h"

#include <QJsonArray>
#include <QtTest/QtTest>

class AiCommandToolPlannerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void buildsPowerShellPlanWithPolicy();
    void buildsProcessPlanWithProgramPreview();
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
    args[QStringLiteral("program")] = QStringLiteral("notepad.exe");
    args[QStringLiteral("arguments")] = QJsonArray{QStringLiteral("a.txt")};

    const auto plan = sak::ai::AiCommandToolPlanner::buildPlan(QStringLiteral("run_process"),
                                                               args,
                                                               sak::ai::AiToolPolicy::ReadOnlyPc);

    QCOMPARE(plan.shell_label, QStringLiteral("Process"));
    QCOMPARE(plan.request.program, QStringLiteral("notepad.exe"));
    QCOMPARE(plan.request.arguments, QStringList{QStringLiteral("a.txt")});
    QVERIFY(plan.preview.contains(QStringLiteral("notepad.exe")));
    QVERIFY(plan.preview.contains(QStringLiteral("a.txt")));
    QVERIFY(plan.policy_decision.allowed);
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

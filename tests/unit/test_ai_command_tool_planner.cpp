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
    void rejectsNonCanonicalToolName();  // R5-G10-9
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
    QCOMPARE(plan.policy_decision.reason,
             QStringLiteral("Read-only diagnostic shell command allowed"));
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
    QCOMPARE(plan.policy_decision.reason,
             QStringLiteral("Read-only PC policy allows only known read-only diagnostic shell "
                            "commands (ping, ipconfig, systeminfo, tasklist, netstat, whoami, "
                            "Get-*/Test-* reads, ...); this command is not on the read-only "
                            "allowlist"));
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

    // The refusal echoes the sanitized offending path -- the security-relevant part an injection
    // or regression would corrupt -- and guard_block_error is copied verbatim from it.
    const QString kRelativeError = QStringLiteral(
        "program must be an absolute path or a bare executable name, not a "
        "working-directory-relative path: sub/planted.exe");
    QCOMPARE(plan.request.validation_error, kRelativeError);
    QCOMPARE(plan.guard_block_error, kRelativeError);
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

    // The refusal echoes the sanitized bare name it could not resolve; guard_block_error is the
    // same string. Pin both so a dropped/garbled echo cannot pass unnoticed.
    const QString kResolveError = QStringLiteral(
        "Cannot resolve program 'definitely-not-a-real-sak-test-binary.exe' to an absolute "
        "path; refusing to launch a bare name");
    QCOMPARE(plan.request.validation_error, kResolveError);
    QCOMPARE(plan.guard_block_error, kResolveError);
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

    // The whole preview is deterministic: the space-bearing absolute program path and the
    // space-bearing arg are each quoted as one token, the plain arg is left bare, and the
    // embedded tab is escaped visibly (never emitted raw). Pin the exact rendered command -- the
    // security-relevant text the human approves -- which subsumes the per-token quoting, ordering,
    // and no-raw-tab checks in a single assertion.
    QCOMPARE(plan.preview,
             QStringLiteral("\"C:\\Program Files\\app.exe\" \"hello world\" plain \"tab\\there\""));
}

void AiCommandToolPlannerTests::marksRiskyCommandAndPolicyDenial() {
    QJsonObject args;
    args[QStringLiteral("command")] = QStringLiteral("Remove-Item C:\\temp\\x -Recurse");

    const auto plan = sak::ai::AiCommandToolPlanner::buildPlan(QStringLiteral("run_powershell"),
                                                               args,
                                                               sak::ai::AiToolPolicy::ReadOnlyPc);

    QVERIFY(plan.risky_change);
    QVERIFY(!plan.policy_decision.allowed);
    QCOMPARE(plan.policy_decision.reason,
             QStringLiteral("Read-only PC policy blocked mutating command"));
    QVERIFY(plan.policy_decision.risky_change);
}

void AiCommandToolPlannerTests::carriesPidMutationGuardBlock() {
    QJsonObject args;
    args[QStringLiteral("command")] =
        QStringLiteral("$pid=0; [void][Win32]::GetWindowThreadProcessId($hWnd,[ref]$pid)");

    const auto plan = sak::ai::AiCommandToolPlanner::buildPlan(QStringLiteral("run_powershell"),
                                                               args,
                                                               sak::ai::AiToolPolicy::ReadOnlyPc);

    QCOMPARE(plan.guard_block_error,
             QStringLiteral("Blocked PowerShell $PID mutation. $PID is a read-only automatic "
                            "variable; use a different variable such as $processId or "
                            "$windowProcessId."));
}

void AiCommandToolPlannerTests::carriesChecksumBypassBlock() {
    QJsonObject args;
    args[QStringLiteral("command")] =
        QStringLiteral("choco install superantispyware -y --ignore-checksums");

    const auto plan = sak::ai::AiCommandToolPlanner::buildPlan(
        QStringLiteral("run_powershell"), args, sak::ai::AiToolPolicy::ExclusiveMutatingExecutor);

    QCOMPARE(plan.guard_block_error,
             QStringLiteral("Blocked package checksum bypass. Do not pass --ignore-checksums, "
                            "substitute checksums, or run cached installers after a package "
                            "checksum mismatch."));
    QVERIFY(plan.guard_approval_reason.isEmpty());
    QVERIFY(plan.policy_decision.allowed);
    QCOMPARE(plan.policy_decision.reason,
             QStringLiteral("Known local tool allowed with exclusive mutation policy"));
    QVERIFY(plan.policy_decision.requires_exclusive_lease);
}

void AiCommandToolPlannerTests::rejectsNonCanonicalToolName() {
    // The command tools are a CLOSED set of EXACT names. The router matches names case- and
    // whitespace-insensitively, so a case-variant like "Run_PowerShell" is routed here as a
    // command tool; buildPlan must refuse the non-canonical spelling (default-denied) rather
    // than fall through to the run_process branch and launch a model-supplied executable.
    QJsonObject args;
    args[QStringLiteral("command")] = QStringLiteral("Get-Date");
    const auto plan = sak::ai::AiCommandToolPlanner::buildPlan(QStringLiteral("Run_PowerShell"),
                                                               args,
                                                               sak::ai::AiToolPolicy::ReadOnlyPc);
    // The refusal echoes the sanitized non-canonical spelling that was rejected.
    QCOMPARE(plan.guard_block_error, QStringLiteral("Unsupported command tool: Run_PowerShell"));
    QCOMPARE(plan.request.validation_error,
             QStringLiteral("Unsupported command tool: Run_PowerShell"));
    QVERIFY(!plan.policy_decision.allowed);  // policy_decision stays default-denied
    QVERIFY(plan.risky_change);

    // Non-vacuity: the exact canonical name IS accepted (no guard block), so the refusal is
    // the non-canonical spelling, not a rejection of the command itself.
    const auto ok = sak::ai::AiCommandToolPlanner::buildPlan(QStringLiteral("run_powershell"),
                                                             args,
                                                             sak::ai::AiToolPolicy::ReadOnlyPc);
    QVERIFY(ok.guard_block_error.isEmpty());
}

QTEST_GUILESS_MAIN(AiCommandToolPlannerTests)
#include "test_ai_command_tool_planner.moc"
